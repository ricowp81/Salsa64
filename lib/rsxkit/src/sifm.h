/* SPDX-License-Identifier: MIT */
/*
 * rsxkit — reparto en tramos de una copia con el motor de imagen escalada
 * (NV3089, «scaled image from memory»), sin PSL1GHT: C puro que se prueba en
 * el host (tests/unit/test_sifm.c).
 */
#ifndef RSXKIT_SIFM_H
#define RSXKIT_SIFM_H

#include <stdint.h>

/* Un tramo: columnas [x, x + w) y filas [y, y + h) de la imagen de entrada
 * (en texeles); la salida es el mismo rectángulo por la escala. `in_w` es el
 * ancho de entrada que se le da al motor: igual a `w`, o `w + 1` para que
 * sea par (el texel de más está en la misma fila, dentro del pitch, y nunca
 * llega a la salida: el recorte y la razón son los de `w`). */
typedef struct rsxkit_sifm_tile {
	uint16_t x, y, w, h;
	uint16_t in_w;
} rsxkit_sifm_tile;

/* Los tramos de una imagen de w × h texeles de 4 bytes con `pitch` bytes por
 * fila, ampliada `scale` veces: hasta 1024 píxeles de salida por lado (el
 * motor no pasa de 2048) y anchos de entrada múltiplos de 16 salvo el último
 * (el offset de cada tramo queda alineado a 64). Con `even`, ningún ancho de
 * entrada es impar: el último se lee con un texel de más, que tiene que
 * caber en el pitch (siempre, si el pitch es múltiplo de 8). Devuelve
 * cuántos tramos escribió en `out` (0 si no caben en `max`, si los
 * argumentos no sirven o si, con `even`, el texel de más no cabe). Sin
 * `even` los tramos son los de siempre. */
unsigned rsxkit_sifm_plan(uint32_t w, uint32_t h, uint32_t scale, uint32_t pitch, int even, rsxkit_sifm_tile *out,
			  unsigned max);

/* Razón entrada/salida en 12.20 de un tramo de `n` texeles a `scale`,
 * redondeada hacia arriba (con el truncado, el último píxel de cada grupo
 * caería en el texel anterior). */
static inline uint32_t rsxkit_sifm_ratio(uint32_t n, uint32_t scale)
{
	return (uint32_t)((((uint64_t)n << 20) + n * scale - 1) / (n * scale));
}

#endif
