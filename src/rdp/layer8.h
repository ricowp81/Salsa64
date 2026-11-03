/*
 * Salsa64 — capas de 8 bits dibujadas en la RDRAM sin pasar por el RSX.
 *
 * Yoshi's Story dibuja sus fondos (cielo, colinas, árboles) con el RDP en
 * tres color images de 8 bits de 336 × 256 (tiles CI8 de 16 × 16, copiados
 * 1:1 sin filtro ni mezcla) y las lee enseguida como imagen CI8 con paleta.
 * Llevarlas al RSX obliga a copiarlas de vuelta a la RDRAM y a que la
 * emulación espere esa copia en cada tarea (toda la cola del RSX por
 * delante: 8–13 ms por tarea en la consola). Aquí se rasterizan en el PPE,
 * directo sobre la RDRAM, con la misma semántica que el rasterizador del
 * host (src/host/raster.c): muestra en el centro del píxel, texel más
 * cercano, prueba de alfa, sin z ni mezcla. Lo que no cabe en esa semántica
 * (perspectiva, mezcla, z, combiners con color de vértice, dos texturas,
 * texturas desde un destino) deja el destino entero al RSX en esa tarea.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SALSA64_RDP_LAYER8_H
#define SALSA64_RDP_LAYER8_H

#include <stdbool.h>
#include <stdint.h>

#include "rsp/gfx/gfx.h"

#define LAYER8_MAX 4  /* destinos de 8 bits que una tarea puede dejar aquí */

struct layer8_result {
	uint32_t n;                   /* destinos dibujados en la RDRAM */
	uint32_t addr[LAYER8_MAX];    /* su dirección física y ancho */
	uint32_t width[LAYER8_MAX];
	uint32_t draws, pixels;       /* dibujos y píxeles escritos */
	uint32_t refused;             /* destinos de 8 bits que quedaron para el RSX */
	/* Por camino: rectángulos 1:1 (copia de índices por fila, entera) y
	 * triángulos genéricos (baricéntricas en coma flotante); píxeles y
	 * ticks de layer8_clock, si está. */
	uint32_t fast_pixels, slow_pixels;
	uint64_t fast_ticks, slow_ticks;
};

/* Reloj para medir los dos caminos (NULL: sin medir). */
extern uint64_t (*layer8_clock)(void);
/* El camino rápido se puede apagar (pruebas y comparación). */
extern bool layer8_fast;

/* ¿El dibujo cabe en la semántica de arriba? */
bool layer8_draw_ok(const struct gfx_draw *d, const struct gfx_vertex *v);

/* Rasteriza el dibujo sobre la color image de 8 bits `addr`/`width` de la
 * RDRAM; `arena` y `cache` son las dos memorias de texturas (gfx_tex.where).
 * Devuelve los píxeles escritos. */
uint32_t layer8_draw(uint8_t *rdram, uint32_t rdram_size, uint32_t addr, uint32_t width, const struct gfx_draw *d,
		     const struct gfx_vertex *v, const uint8_t *arena, const uint8_t *cache, struct layer8_result *out);

/* Recorre la salida de una tarea y dibuja en la RDRAM los destinos de 8
 * bits de más de `min_width` píxeles de ancho (las capas; no las sombras
 * chicas ni la cobertura) cuyos dibujos quepan todos. */
void layer8_run(uint8_t *rdram, uint32_t rdram_size, const uint8_t *cmd, uint32_t cmd_bytes,
		const struct gfx_vertex *vtx, const uint8_t *arena, const uint8_t *cache, uint32_t min_width,
		struct layer8_result *out);

#endif
