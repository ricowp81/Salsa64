/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SALSA64_FRONTEND_RARROM_H
#define SALSA64_FRONTEND_RARROM_H
#include <stdbool.h>
#include <stdint.h>
#include "archiverom.h"
bool salsa64_rarrom_is_path(const char *path);
int salsa64_rarrom_read_header(const char *path, uint8_t header[64], uint32_t *rom_size);
int salsa64_rarrom_extract(const char *path, uint8_t *dst, uint32_t dst_cap, uint32_t *rom_size);
#endif
