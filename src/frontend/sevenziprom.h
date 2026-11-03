/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SALSA64_FRONTEND_SEVENZIPROM_H
#define SALSA64_FRONTEND_SEVENZIPROM_H
#include <stdbool.h>
#include <stdint.h>
#include "archiverom.h"
bool salsa64_7zrom_is_path(const char *path);
int salsa64_7zrom_read_header(const char *path, uint8_t header[64], uint32_t *rom_size);
int salsa64_7zrom_extract(const char *path, uint8_t *dst, uint32_t dst_cap, uint32_t *rom_size);
#endif
