/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 7z ROM reader. Uses libun7zip's in-memory SzArEx API and never writes paths
 * from the archive to the filesystem. The selected member and the complete
 * solid block containing it are limited to 64 MiB.
 */
#include "sevenziprom.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "7z.h"
#include "7zCrc.h"
#include "7zFile.h"

#define SEVENZIP_LOOK_BUFFER (1u << 15)
#define SEVENZIP_METADATA_LIMIT (16u << 20)
#define SEVENZIP_TEMP_LIMIT (64u << 20)
#define SEVENZIP_ENTRY_LIMIT 4096u

union alloc_header {
    long double align_ld;
    void *align_ptr;
    size_t size;
};

struct bounded_allocator {
    ISzAlloc iface; /* Must stay first: callback receives this address. */
    size_t used;
    size_t limit;
    void *external;
    size_t external_size;
    bool external_claimed;
};

static void *bounded_alloc(ISzAllocPtr iface, size_t size)
{
    struct bounded_allocator *a = (struct bounded_allocator *)iface;
    union alloc_header *h;
    if (!a || !size)
        return NULL;
    if (a->external && !a->external_claimed && size == a->external_size) {
        a->external_claimed = true;
        return a->external;
    }
    if (a->used > a->limit || size > a->limit - a->used ||
        size > SIZE_MAX - sizeof *h)
        return NULL;
    h = (union alloc_header *)malloc(sizeof *h + size);
    if (!h)
        return NULL;
    h->size = size;
    a->used += size;
    return (void *)(h + 1);
}

static void bounded_free(ISzAllocPtr iface, void *address)
{
    struct bounded_allocator *a = (struct bounded_allocator *)iface;
    union alloc_header *h;
    if (!a || !address || address == a->external)
        return;
    h = ((union alloc_header *)address) - 1;
    if (h->size <= a->used)
        a->used -= h->size;
    free(h);
}

static void bounded_allocator_init(struct bounded_allocator *a, size_t limit)
{
    memset(a, 0, sizeof *a);
    a->iface.Alloc = bounded_alloc;
    a->iface.Free = bounded_free;
    a->limit = limit;
}

static unsigned ascii_lower(unsigned c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static bool is_rom_member(const CSzArEx *db, UInt32 index)
{
    UInt16 name[256];
    size_t count = SzArEx_GetFileNameUtf16(db, index, NULL);
    size_t dot;
    if (count < 6 || count > sizeof name / sizeof name[0])
        return false;
    if (SzArEx_GetFileNameUtf16(db, index, name) != count)
        return false;
    /* count includes the UTF-16 NUL; the suffix starts five units earlier. */
    dot = count - 5;
    if (name[dot] != '.')
        return false;
    return (ascii_lower(name[dot + 1]) == 'z' ||
            ascii_lower(name[dot + 1]) == 'v' ||
            ascii_lower(name[dot + 1]) == 'n') &&
           name[dot + 2] == '6' && name[dot + 3] == '4';
}

static int sevenziprom_read(const char *path, uint8_t *dst, uint32_t dst_cap,
                            uint32_t *rom_size, bool header_only)
{
    CFileInStream archive_stream;
    CLookToRead2 look_stream;
    CSzArEx db;
    struct bounded_allocator metadata_alloc, temp_alloc, block_alloc;
    Byte *out_buffer = NULL;
    size_t out_buffer_size = 0, offset = 0, processed = 0;
    bool file_open = false, db_initialized = false;
    SRes res = SZ_ERROR_FAIL;
    int result = -1;

    if (!path || !dst || !rom_size ||
        dst_cap < (header_only ? 64u : SALSA64_ARCHIVEROM_MIN_ROM_SIZE))
        return -1;

    memset(&archive_stream, 0, sizeof archive_stream);
    memset(&look_stream, 0, sizeof look_stream);
    bounded_allocator_init(&metadata_alloc, SEVENZIP_METADATA_LIMIT);
    bounded_allocator_init(&temp_alloc, SEVENZIP_TEMP_LIMIT);
    bounded_allocator_init(&block_alloc, SALSA64_ARCHIVEROM_MAX_SIZE);

    if (InFile_Open(&archive_stream.file, path) != 0)
        return -1;
    file_open = true;
    FileInStream_CreateVTable(&archive_stream);
    LookToRead2_CreateVTable(&look_stream, False);
    look_stream.buf = (Byte *)ISzAlloc_Alloc(&metadata_alloc.iface,
                                             SEVENZIP_LOOK_BUFFER);
    if (!look_stream.buf) {
        result = SALSA64_ARCHIVEROM_NOMEM;
        goto done;
    }
    look_stream.bufSize = SEVENZIP_LOOK_BUFFER;
    look_stream.realStream = &archive_stream.vt;
    LookToRead2_Init(&look_stream);

    CrcGenerateTable();
    SzArEx_Init(&db);
    db_initialized = true;
    res = SzArEx_Open(&db, &look_stream.vt,
                      &metadata_alloc.iface, &temp_alloc.iface);
    if (res != SZ_OK) {
        result = res == SZ_ERROR_MEM ? SALSA64_ARCHIVEROM_NOMEM : -1;
        goto done;
    }
    if (db.NumFiles > SEVENZIP_ENTRY_LIMIT)
        goto done;

    for (UInt32 i = 0; i < db.NumFiles; i++) {
        UInt64 file_size, folder_size;
        UInt32 folder_index, block_index = 0xFFFFFFFFu;
        if (SzArEx_IsDir(&db, i) || !is_rom_member(&db, i))
            continue;
        file_size = SzArEx_GetFileSize(&db, i);
        if (file_size < SALSA64_ARCHIVEROM_MIN_ROM_SIZE ||
            file_size > SALSA64_ARCHIVEROM_MAX_SIZE ||
            (!header_only && file_size > dst_cap))
            continue;
        folder_index = db.FileToFolder[i];
        if (folder_index == 0xFFFFFFFFu || folder_index >= db.db.NumFolders)
            continue;
        folder_size = SzAr_GetFolderUnpackSize(&db.db, folder_index);
        if (!folder_size || folder_size < file_size ||
            folder_size > SALSA64_ARCHIVEROM_MAX_SIZE)
            continue;

        /* For a one-file folder the SDK's output allocation can point directly
         * at the final ROM buffer. Solid folders use a bounded scratch block. */
        if (!header_only && folder_size == file_size &&
            folder_size <= dst_cap) {
            block_alloc.external = dst;
            block_alloc.external_size = (size_t)folder_size;
        }
        res = SzArEx_Extract(&db, &look_stream.vt, i, &block_index,
                             &out_buffer, &out_buffer_size, &offset, &processed,
                             &block_alloc.iface, &temp_alloc.iface);
        if (res != SZ_OK) {
            result = res == SZ_ERROR_MEM ? SALSA64_ARCHIVEROM_NOMEM : -1;
            break;
        }
        if (!out_buffer || processed != (size_t)file_size ||
            offset > out_buffer_size || processed > out_buffer_size - offset) {
            result = -1;
            break;
        }
        if (header_only) {
            if (processed < 64u)
                break;
            memcpy(dst, out_buffer + offset, 64u);
        } else if (out_buffer != dst || offset != 0) {
            memmove(dst, out_buffer + offset, processed);
        }
        *rom_size = (uint32_t)file_size;
        result = 0;
        break;
    }

done:
    if (out_buffer)
        ISzAlloc_Free(&block_alloc.iface, out_buffer);
    if (db_initialized)
        SzArEx_Free(&db, &metadata_alloc.iface);
    if (look_stream.buf)
        ISzAlloc_Free(&metadata_alloc.iface, look_stream.buf);
    if (file_open)
        File_Close(&archive_stream.file);
    return result;
}

bool salsa64_7zrom_is_path(const char *path)
{
    size_t n;
    if (!path)
        return false;
    n = strlen(path);
    return n >= 3 &&
           ascii_lower((unsigned char)path[n - 3]) == '.' &&
           ascii_lower((unsigned char)path[n - 2]) == '7' &&
           ascii_lower((unsigned char)path[n - 1]) == 'z';
}

int salsa64_7zrom_read_header(const char *path, uint8_t header[64],
                              uint32_t *rom_size)
{
    return header ? sevenziprom_read(path, header, 64, rom_size, true) : -1;
}

int salsa64_7zrom_extract(const char *path, uint8_t *dst, uint32_t dst_cap,
                          uint32_t *rom_size)
{
    return sevenziprom_read(path, dst, dst_cap, rom_size, false);
}
