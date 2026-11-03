/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * RAR ROM reader. It uses RAR_TEST and UnRAR's process-data callback, so
 * decompressed bytes are copied to the caller's RAM buffer; archive paths
 * are never written to disk.
 *
 * Required UnRAR license paragraph:
 * UnRAR source code may be used in any software to handle RAR archives
 * without limitations free of charge, but cannot be used to develop RAR
 * (WinRAR) compatible archiver and to re-create RAR compression algorithm,
 * which is proprietary. Distribution of modified UnRAR source code in
 * separate form or as a part of other software is permitted, provided that
 * full text of this paragraph, starting from "UnRAR source code" words, is
 * included in license, or in documentation if license is not available,
 * and in source code comments of resulting package.
 */
#include "rarrom.h"
#include <string.h>
#include "dll.hpp"

struct rar_sink {
    uint8_t *dst;
    uint64_t expected;
    uint64_t written;
    size_t capture;
    bool failed;
};

/* The UnRAR callback API has no user pointer. Salsa64 serializes archive
 * reads on the frontend thread, so one active sink is sufficient. */
static struct rar_sink *rar_active_sink;

static unsigned rar_lower(unsigned c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static bool rar_name_is_rom(const char *name, size_t len)
{
    if (!name || len < 5 || len >= 1024 || name[len - 1] == '/' ||
        name[len - 1] == '\\')
        return false;
    const char *ext = name + len - 4;
    return ext[0] == '.' &&
        (rar_lower((unsigned char)ext[1]) == 'z' ||
         rar_lower((unsigned char)ext[1]) == 'v' ||
         rar_lower((unsigned char)ext[1]) == 'n') &&
        ext[2] == '6' && ext[3] == '4';
}

static int rar_process_data(unsigned char *data, int size)
{
    struct rar_sink *sink = rar_active_sink;
    if (!sink || size < 0 || (size && !data)) {
        if (sink)
            sink->failed = true;
        return 0;
    }
    if ((uint64_t)size > sink->expected - sink->written) {
        sink->failed = true;
        return 0;
    }
    if (sink->dst && sink->written < sink->capture) {
        size_t copy = sink->capture - (size_t)sink->written;
        if (copy > (size_t)size)
            copy = (size_t)size;
        if (copy)
            memcpy(sink->dst + (size_t)sink->written, data, copy);
    }
    sink->written += (uint64_t)size;
    return 1;
}

static int rar_test(HANDLE archive, struct rar_sink *sink)
{
    int rc;
    rar_active_sink = sink;
    RARSetProcessDataProc(archive, rar_process_data);
    rc = RARProcessFile(archive, RAR_TEST, NULL, NULL);
    rar_active_sink = NULL;
    return rc;
}

static int rar_skip(HANDLE archive)
{
    rar_active_sink = NULL;
    return RARProcessFile(archive, RAR_SKIP, NULL, NULL);
}

static int rarrom_read(const char *path, uint8_t *dst, uint32_t dst_cap,
                       uint32_t *rom_size, bool header_only)
{
    struct RAROpenArchiveDataEx open_data;
    HANDLE archive = NULL;
    uint64_t solid_bytes = 0;
    int result = -1;

    if (!path || !dst || !rom_size ||
        dst_cap < (header_only ? 64u : SALSA64_ARCHIVEROM_MIN_ROM_SIZE))
        return -1;

    memset(&open_data, 0, sizeof open_data);
    open_data.ArcName = (char *)path;
    open_data.OpenMode = RAR_OM_EXTRACT;
    archive = RAROpenArchiveEx(&open_data);
    if (!archive)
        return open_data.OpenResult == ERAR_NO_MEMORY ? SALSA64_ARCHIVEROM_NOMEM : -1;
    if (open_data.OpenResult != ERAR_SUCCESS ||
        (open_data.Flags & (ROADF_ENCHEADERS | ROADF_VOLUME | ROADF_FIRSTVOLUME))) {
        result = open_data.OpenResult == ERAR_NO_MEMORY ? SALSA64_ARCHIVEROM_NOMEM : -1;
        goto done;
    }

    for (;;) {
        struct RARHeaderDataEx header;
        uint64_t unpacked;
        size_t name_len;
        bool directory, encrypted, candidate, solid;
        int rc;

        memset(&header, 0, sizeof header);
        rc = RARReadHeaderEx(archive, &header);
        if (rc == ERAR_END_ARCHIVE)
            break;
        if (rc != ERAR_SUCCESS) {
            result = rc == ERAR_NO_MEMORY ? SALSA64_ARCHIVEROM_NOMEM : -1;
            break;
        }

        unpacked = ((uint64_t)header.UnpSizeHigh << 32) | header.UnpSize;
        name_len = strnlen(header.FileName, sizeof header.FileName);
        directory = (header.Flags & RHDF_DIRECTORY) != 0;
        encrypted = (header.Flags & RHDF_ENCRYPTED) != 0;
        solid = (open_data.Flags & ROADF_SOLID) != 0 ||
                (header.Flags & RHDF_SOLID) != 0;
        candidate = !directory && !encrypted &&
                    rar_name_is_rom(header.FileName, name_len) &&
                    unpacked >= SALSA64_ARCHIVEROM_MIN_ROM_SIZE &&
                    unpacked <= SALSA64_ARCHIVEROM_MAX_SIZE;

        /* Rechaza diccionarios grandes antes de que UnRAR los reserve. */
        if (header.DictSize > SALSA64_ARCHIVEROM_MAX_SIZE) {
            if (solid)
                break;
            rc = rar_skip(archive);
            if (rc != ERAR_SUCCESS) {
                result = rc == ERAR_NO_MEMORY ? SALSA64_ARCHIVEROM_NOMEM : -1;
                break;
            }
            continue;
        }

        if (candidate) {
            struct rar_sink sink;
            if (!header_only && unpacked > dst_cap)
                break;
            if (solid && unpacked > SALSA64_ARCHIVEROM_MAX_SIZE - solid_bytes)
                break;
            memset(&sink, 0, sizeof sink);
            sink.dst = dst;
            sink.expected = unpacked;
            sink.capture = header_only ? 64u : (size_t)unpacked;
            rc = rar_test(archive, &sink);
            if (rc == ERAR_SUCCESS && !sink.failed &&
                sink.written == unpacked && sink.written >= 64u) {
                *rom_size = (uint32_t)unpacked;
                result = 0;
            } else {
                result = rc == ERAR_NO_MEMORY ? SALSA64_ARCHIVEROM_NOMEM : -1;
            }
            break;
        }

        /* A preceding encrypted solid member makes the dictionary
         * unavailable without a password; reject rather than continue. */
        if (encrypted && solid)
            break;
        if (solid) {
            struct rar_sink discard;
            if (unpacked > SALSA64_ARCHIVEROM_MAX_SIZE - solid_bytes)
                break;
            memset(&discard, 0, sizeof discard);
            discard.expected = unpacked;
            rc = rar_test(archive, &discard);
            if (rc != ERAR_SUCCESS || discard.failed || discard.written != unpacked) {
                result = rc == ERAR_NO_MEMORY ? SALSA64_ARCHIVEROM_NOMEM : -1;
                break;
            }
            solid_bytes += unpacked;
        } else {
            rc = rar_skip(archive);
            if (rc != ERAR_SUCCESS) {
                result = rc == ERAR_NO_MEMORY ? SALSA64_ARCHIVEROM_NOMEM : -1;
                break;
            }
        }
    }

done:
    rar_active_sink = NULL;
    RARCloseArchive(archive);
    return result;
}

bool salsa64_rarrom_is_path(const char *path)
{
    size_t n;
    if (!path)
        return false;
    n = strlen(path);
    return n >= 4 && rar_lower((unsigned char)path[n - 4]) == '.' &&
           rar_lower((unsigned char)path[n - 3]) == 'r' &&
           rar_lower((unsigned char)path[n - 2]) == 'a' &&
           rar_lower((unsigned char)path[n - 1]) == 'r';
}

int salsa64_rarrom_read_header(const char *path, uint8_t header[64],
                               uint32_t *rom_size)
{
    return header ? rarrom_read(path, header, 64, rom_size, true) : -1;
}

int salsa64_rarrom_extract(const char *path, uint8_t *dst, uint32_t dst_cap,
                           uint32_t *rom_size)
{
    return rarrom_read(path, dst, dst_cap, rom_size, false);
}
