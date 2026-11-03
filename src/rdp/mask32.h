/*
 * Salsa64 — imágenes chicas de 32 bits dibujadas en el PPE, sin el RSX.
 *
 * Paper Mario arma la luz de los cuartos oscuros (la de Mario y la de Watt)
 * con el RDP en cada cuadro: una color image RGBA32 de 64 × 64 puesta
 * encima de su z-buffer (0x400), un relleno blanco y dos cuadros con una
 * textura de luz que mezclan negro con alfa 1 − I; la misma memoria como
 * color image I8 de 64 de ancho, con cuatro rectángulos 1:1 que copian
 * cada franja de 64 × 16 RGBA32 a su fila de 8 bits (el RDP escribe el
 * rojo); esa I8 a la TMEM y, ANTES de dibujar con ella, un relleno de
 * 0xFFFC que devuelve la memoria al z-buffer; por último un rectángulo de
 * toda la pantalla con la máscara (en espejo) como alfa de un negro.
 *
 * La HLE decodifica cada textura de la RDRAM al cargarla: las franjas RGBA32
 * y la máscara salían del z-buffer (0xFFFC, todo oscuro) y el cuarto
 * quedaba negro salvo el HUD. Aquí se dibujan esas imágenes en el PPE con
 * la semántica del rasterizador del host (src/host/raster.c): muestra en el
 * centro del píxel, perspectiva afín (w = 1), combinación lineal o
 * términos, prueba de alfa y mezcla con la memoria; los dibujos que leen la
 * misma memoria (la otra vista, o una franja de la misma) la leen de aquí,
 * como el RDP. Al final la RDRAM recibe la última imagen de 8 bits (o la
 * de 32 si no hubo): la tarea siguiente que la dibuja carga esa máscara a
 * la TMEM, un cuadro tarde. Los rellenos que pisan esos bytes antes (el que
 * la devuelve al z-buffer en la misma tarea y el borrado del z-buffer con
 * que el juego abre el cuadro siguiente, en una tarea aparte) se deshacen
 * ahí: sin eso la HLE volvería a leer 0xFFFC. Es lo único que se aparta de
 * la consola (el CPU vería la máscara en esos 4 KB del z-buffer), y dura a
 * lo más MASK32_KEEP tareas después de la última que la dibujó.
 *
 * Lo que no cabe (z, niebla, vértices con w ≠ 1, texturas que el consumidor
 * leería de un destino, imágenes de más de 64 × 64, otro tamaño de píxel en
 * la misma dirección) deja la imagen al RSX en esa tarea, como antes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SALSA64_RDP_MASK32_H
#define SALSA64_RDP_MASK32_H

#include <stdbool.h>
#include <stdint.h>

#include "rsp/gfx/gfx.h"

#define MASK32_W   64  /* ancho y alto máximos de la imagen */
#define MASK32_H   64
#define MASK32_MAX 2   /* imágenes (dirección, ancho) por tarea */
#define MASK32_KEEP 4  /* tareas que se repone lo último después de la que lo dibujó */

struct mask32_result {
	uint32_t n;                    /* imágenes dibujadas aquí */
	uint32_t addr[MASK32_MAX];     /* su dirección física y ancho: quien traduce no las dibuja ni las copia */
	uint32_t width[MASK32_MAX];
	uint32_t draws, pixels;        /* dibujos y píxeles escritos */
	uint32_t bytes;                /* bytes que fueron a la RDRAM */
	uint32_t refused;              /* imágenes de 32 bits chicas que quedaron para el RSX */
	uint32_t restored;             /* bytes repuestos tras un relleno de una tarea sin estas imágenes */
};

/* Camino entero (sin conversiones entre coma flotante y entero por píxel)
 * para los dibujos con combinación lineal, color constante y texturas sin
 * filtro; se puede apagar para comparar (pruebas). */
extern bool mask32_fast;

/* ¿El dibujo cabe en la semántica de arriba? Sus texturas, si vienen de
 * la imagen (dirección `addr`, ancho `width`), se leen de ella. */
bool mask32_draw_ok(const struct gfx_draw *d, const struct gfx_vertex *v, uint32_t addr, uint32_t width);

/* Olvida lo que se repone (otro juego, un estado cargado, un reset). */
void mask32_forget(void);

/* Recorre la salida de una tarea (después de salsa64_gfx_rdram_fills), dibuja
 * las imágenes de 32 bits de hasta MASK32_W × MASK32_H (y la de 8 bits en su
 * misma dirección y ancho) cuyos dibujos quepan todos y deja el resultado en
 * la RDRAM; en una tarea sin ellas, repone lo último si un relleno lo pisó.
 * `arena` y `cache` son las dos memorias de texturas (gfx_tex.where). */
void mask32_run(uint8_t *rdram, uint32_t rdram_size, const uint8_t *cmd, uint32_t cmd_bytes,
		const struct gfx_vertex *vtx, const uint8_t *arena, const uint8_t *cache, struct mask32_result *out);

#endif
