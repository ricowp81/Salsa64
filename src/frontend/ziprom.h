/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SALSA64_FRONTEND_ZIPROM_H
#define SALSA64_FRONTEND_ZIPROM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/rom.h"

/* Un ZIP es un contenedor, no datos de ROM. Un archivo admitido tiene una
 * entrada .z64/.v64/.n64 utilizable, usa STORE o DEFLATE y no está cifrado
 * ni es ZIP64. La extracción está acotada por SALSA64_ZIPROM_MAX_SIZE y
 * verifica el tamaño descomprimido y el CRC-32 antes de arrancar el juego. */
#define SALSA64_ZIPROM_MAX_SIZE (64u << 20)
#define SALSA64_ZIPROM_MIN_SIZE 22u
/* Error distinguible de archivo inválido o fallo de lectura (-1). */
#define SALSA64_ZIPROM_NOMEM (-2)

bool salsa64_ziprom_is_path(const char *path);

/* Lee solo la cabecera del cartucho para que el explorador de ROMs muestre el
 * título sin reservar ni descomprimir la ROM entera. Devuelve 0 si tiene éxito, SALSA64_ZIPROM_NOMEM si falta memoria. */
int salsa64_ziprom_read_header(const char *path,
                               uint8_t header[SALSA64_ROM_HEADER_SIZE],
                               uint32_t *rom_size,
                               char *entry_name, size_t entry_name_size);

/* Extrae y valida la ROM elegida en memoria del llamador. dst_cap debe
 * bastar para la ROM descomprimida. Devuelve 0 solo tras pasar el CRC del ZIP. */
int salsa64_ziprom_extract(const char *path, uint8_t *dst, uint32_t dst_cap,
                           uint32_t *rom_size,
                           char *entry_name, size_t entry_name_size);

#endif
