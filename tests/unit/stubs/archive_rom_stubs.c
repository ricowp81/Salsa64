/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host-only stubs: unit tests exercise extension discovery, not PS3 codecs. */
#include <string.h>
#include "frontend/rarrom.h"
#include "frontend/sevenziprom.h"

static unsigned lower_ascii(unsigned c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

bool salsa64_rarrom_is_path(const char *path)
{
    size_t n;
    if (!path)
        return false;
    n = strlen(path);
    return n >= 4 && lower_ascii((unsigned char)path[n - 4]) == '.' &&
           lower_ascii((unsigned char)path[n - 3]) == 'r' &&
           lower_ascii((unsigned char)path[n - 2]) == 'a' &&
           lower_ascii((unsigned char)path[n - 1]) == 'r';
}

int salsa64_rarrom_read_header(const char *path, uint8_t header[64], uint32_t *size)
{
    (void)path; (void)header; (void)size;
    return -1;
}

int salsa64_rarrom_extract(const char *path, uint8_t *dst, uint32_t cap, uint32_t *size)
{
    (void)path; (void)dst; (void)cap; (void)size;
    return -1;
}

bool salsa64_7zrom_is_path(const char *path)
{
    size_t n;
    if (!path)
        return false;
    n = strlen(path);
    return n >= 3 && lower_ascii((unsigned char)path[n - 3]) == '.' &&
           lower_ascii((unsigned char)path[n - 2]) == '7' &&
           lower_ascii((unsigned char)path[n - 1]) == 'z';
}

int salsa64_7zrom_read_header(const char *path, uint8_t header[64], uint32_t *size)
{
    (void)path; (void)header; (void)size;
    return -1;
}

int salsa64_7zrom_extract(const char *path, uint8_t *dst, uint32_t cap, uint32_t *size)
{
    (void)path; (void)dst; (void)cap; (void)size;
    return -1;
}
