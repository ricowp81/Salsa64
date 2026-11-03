/* SPDX-License-Identifier: MIT */
/*
 * rsxkit — tramos del motor de imagen escalada. Ver sifm.h.
 *
 * Por qué el ancho par: el driver nouveau de Mesa (nv30_transfer.c, la
 * familia NV40 del RSX) redondea a 2 el ancho y el alto de entrada de cada
 * copia con este motor, y en la PS3 real una copia de 455 texeles de ancho
 * (la única impar de la sesión) es lo último que se mandó antes de que el
 * RSX dejara de avanzar (Indiana Jones, 2026-10-29: docs/rsx-notes.md). Las
 * de alto impar (447 filas) pasaron más de cien veces en la misma sesión:
 * el alto queda como estaba.
 */
#include "sifm.h"

unsigned rsxkit_sifm_plan(uint32_t w, uint32_t h, uint32_t scale, uint32_t pitch, int even, rsxkit_sifm_tile *out,
			  unsigned max)
{
	if (!w || !h || !scale || scale > 8 || w > 4096 || h > 4096 || pitch < w * 4)
		return 0;
	uint32_t tw = (1024 / scale) & ~15u, th = 1024 / scale;
	unsigned n = 0;
	for (uint32_t y = 0; y < h; y += th) {
		uint32_t ih = h - y < th ? h - y : th;
		for (uint32_t x = 0; x < w; x += tw) {
			uint32_t iw = w - x < tw ? w - x : tw, in_w = iw;
			/* Impar: solo el último tramo de la fila (los otros son
			 * múltiplos de 16). Se lee un texel más, que la fila tiene si
			 * el pitch es múltiplo de 8 (4 × impar no lo es: sobra al
			 * menos uno); si no lo tiene, no se puede. */
			if (even && (iw & 1)) {
				if ((x + iw + 1) * 4 > pitch)
					return 0;
				in_w = iw + 1;
			}
			if (n >= max)
				return 0;
			out[n].x = (uint16_t)x;
			out[n].y = (uint16_t)y;
			out[n].w = (uint16_t)iw;
			out[n].h = (uint16_t)ih;
			out[n].in_w = (uint16_t)in_w;
			n++;
		}
	}
	return n;
}
