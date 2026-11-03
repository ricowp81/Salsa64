/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — lo que comparten los trabajos de disco en segundo plano
 * (platform/ps3/bgjob.h): un archivo de salida con búfer que escribe con las
 * llamadas al kernel (sysLv2Fs*; nunca sysFs*, que cuelga la consola sin
 * cargar el módulo del sistema) y el compresor de bloques LZ en un SPE
 * libre (lib/spukit, lzstream.h), con respaldo en el PPE. Lo usan los
 * estados guardados (frontend/states.c) y la caja negra
 * (frontend/cajanegra.c); la caché del recompilador en disco, después.
 *
 * Todo se llama desde el hilo de bgjob (un trabajo a la vez): los búferes
 * son únicos. El SPE se arranca la primera vez que se comprime y se suelta
 * en bgio_shutdown (cierre del juego).
 */
#ifndef SALSA64_PLATFORM_PS3_BGIO_H
#define SALSA64_PLATFORM_PS3_BGIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- Archivo de salida ---------------------------------------------------------- */

struct bgio_out {
	int32_t fd;         /* -1: cerrado */
	uint64_t us;        /* tiempo dentro del sistema de archivos */
	uint32_t bytes;     /* escritos (con lo que espera en el búfer) */
	uint32_t fill;      /* bytes juntos en el búfer */
};

/* Crea (o trunca) `path`. false si no se pudo. */
bool bgio_out_open(struct bgio_out *o, const char *path);
/* Firma de salsa64_state_write_fn / bb_write_fn: ctx = struct bgio_out. */
bool bgio_out_write(void *ctx, const void *data, uint32_t len);
/* Escribe `len` bytes en la posición absoluta `offset` (vacía el búfer
 * antes) y deja el archivo ahí. Firma de salsa64_state_patch_fn. */
bool bgio_out_patch(void *ctx, uint32_t offset, const uint8_t *data, uint32_t len);
/* Vacía el búfer y pide al sistema que lleve el archivo al disco
 * (sysLv2FsFsync): para lo que se escribe antes de un cierre forzado, que
 * puede terminar con la consola apagada a la fuerza. */
bool bgio_out_sync(struct bgio_out *o);
/* Con ok: vacía el búfer, cierra y renombra `tmp` a `path` (si el sistema
 * no reemplaza, quita el viejo y repite). Sin ok o si algo falla: cierra y
 * borra `tmp`. Devuelve si quedó escrito; en `err` (n bytes), el motivo. */
bool bgio_out_finish(struct bgio_out *o, bool ok, const char *tmp, const char *path, char *err, size_t n);

/* ---- Compresión ----------------------------------------------------------------- */

struct bgio_lz_stats {
	uint32_t blocks, stored;      /* bloques entregados y los que fueron sin comprimir */
	uint32_t spu_blocks, ppe_blocks;
	uint64_t in_bytes, out_bytes;
	uint64_t spu_us;              /* el SPE comprimiendo (suma de sus trabajos) */
	uint64_t sink_us;             /* dentro de las funciones de salida (el disco) */
	bool spu;                     /* hay un SPE comprimiendo */
};

/* Firma de salsa64_state_compress_fn / bb_compress_fn (cctx no se usa):
 * comprime `len` bytes de `src` (alineado a 16, legible hasta el siguiente
 * múltiplo de 16) y entrega cada bloque con su encabezado a `write`. */
bool bgio_compress(void *cctx, const uint8_t *src, uint32_t len, bool (*write)(void *, const void *, uint32_t),
		   void *wctx);
/* Estadísticas desde la última bgio_lz_reset. */
void bgio_lz_reset(void);
void bgio_lz_get(struct bgio_lz_stats *out);

/* Suelta el SPE (al cerrar el juego, con el hilo de bgjob ocioso). */
void bgio_shutdown(void);

#endif
