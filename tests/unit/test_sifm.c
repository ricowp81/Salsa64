/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — pruebas del reparto en tramos del motor de imagen escalada de
 * lib/rsxkit (sifm.c: la carga de la z de la RDRAM, rsxkit_depth_load) y de
 * las color images imposibles (rdp_cimg_impossible). En la PS3 real el RSX
 * dejó de avanzar justo después de una copia de 455 texeles de ancho, la
 * única impar de la sesión (docs/rsx-notes.md): con el blindaje ningún
 * ancho de entrada es impar, y sin él los tramos son los de siempre.
 */
#include "test.h"

#include <string.h>

#include "../../lib/rsxkit/src/sifm.h"
#include "rdp/targets.h"

/* Los tramos de antes del blindaje (target.c hasta 2026-10-28), para
 * comparar: mismos rectángulos y in_w = w. */
static unsigned old_plan(uint32_t w, uint32_t h, uint32_t scale, rsxkit_sifm_tile *out)
{
	uint32_t tw = (1024 / scale) & ~15u, th = 1024 / scale;
	unsigned n = 0;
	for (uint32_t y = 0; y < h; y += th)
		for (uint32_t x = 0; x < w; x += tw) {
			out[n].x = (uint16_t)x;
			out[n].y = (uint16_t)y;
			out[n].w = (uint16_t)(w - x < tw ? w - x : tw);
			out[n].h = (uint16_t)(h - y < th ? h - y : th);
			out[n].in_w = out[n].w;
			n++;
		}
	return n;
}

static bool same(const rsxkit_sifm_tile *a, const rsxkit_sifm_tile *b, unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].w != b[i].w || a[i].h != b[i].h || a[i].in_w != b[i].in_w)
			return false;
	return true;
}

/* Con anchos pares (los de siempre: 640, 448, 400 a 2×) el blindaje no
 * cambia nada; sin el blindaje, nunca cambia nada. */
static void sifm_even_widths_unchanged(void)
{
	static const uint32_t cases[][3] = { { 640, 480, 2 }, { 448, 415, 2 }, { 400, 447, 2 }, { 320, 240, 4 },
					     { 640, 480, 5 }, { 512, 447, 3 }, { 455, 341, 2 }, { 637, 477, 3 } };
	for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
		uint32_t w = cases[k][0], h = cases[k][1], sc = cases[k][2];
		rsxkit_sifm_tile a[64], b[64];
		unsigned na = rsxkit_sifm_plan(w, h, sc, 640 * 4, 0, a, 64), nb = old_plan(w, h, sc, b);
		CHECK_EQ(na, nb);
		CHECK(same(a, b, na));
		if (w & 1)
			continue;
		na = rsxkit_sifm_plan(w, h, sc, 640 * 4, 1, a, 64);
		CHECK_EQ(na, nb);
		CHECK(same(a, b, na));
	}
}

/* El caso de Indiana Jones: 455 × 341 a 2×, desde el búfer de la z (2560
 * bytes por fila). Un tramo, de entrada 456 (par) y salida 910 × 682: la
 * misma razón, así que el texel 455 nunca llega a la salida. */
static void sifm_odd_width_padded(void)
{
	rsxkit_sifm_tile t[64];
	unsigned n = rsxkit_sifm_plan(455, 341, 2, 640 * 4, 1, t, 64);
	CHECK_EQ(n, 1);
	CHECK_EQ(t[0].x, 0);
	CHECK_EQ(t[0].w, 455);
	CHECK_EQ(t[0].in_w, 456);
	CHECK_EQ(t[0].h, 341);  /* el alto impar pasó cien veces en la consola: queda */
	CHECK_EQ(rsxkit_sifm_ratio(t[0].w, 2), 1u << 19);
	/* Con la razón de la salida (0,5) y el origen en el centro, el último
	 * píxel (909) cae en el texel 454. */
	CHECK((uint32_t)(((909u * 2 + 1) * (uint64_t)rsxkit_sifm_ratio(455, 2)) >> 21) == 454);
	/* Varios tramos por fila a 3×: solo el último puede ser impar. */
	n = rsxkit_sifm_plan(637, 100, 3, 640 * 4, 1, t, 64);
	CHECK(n >= 2);
	for (unsigned i = 0; i < n; i++) {
		CHECK_EQ(t[i].in_w % 2, 0);
		CHECK(t[i].in_w == t[i].w || (t[i].in_w == t[i].w + 1 && t[i].x + t[i].w == 637));
		CHECK((t[i].x + t[i].in_w) * 4u <= 640u * 4u);
	}
}

/* El texel de más tiene que caber en la fila: si no, no se copia. */
static void sifm_no_room_fails(void)
{
	rsxkit_sifm_tile t[64];
	CHECK_EQ(rsxkit_sifm_plan(455, 10, 2, 455 * 4, 1, t, 64), 0);
	CHECK_EQ(rsxkit_sifm_plan(455, 10, 2, 455 * 4, 0, t, 64), 1);  /* sin blindaje, como antes */
	CHECK_EQ(rsxkit_sifm_plan(0, 10, 2, 64, 1, t, 64), 0);
	CHECK_EQ(rsxkit_sifm_plan(16, 10, 9, 64, 1, t, 64), 0);
	CHECK_EQ(rsxkit_sifm_plan(640, 480, 1, 640 * 4, 1, t, 0), 0);
}

/* rdp_cimg_impossible: lo de siempre (ancho, alto) y, con el tamaño de la
 * RDRAM, una color image que empieza fuera de ella. */
static void cimg_impossible(void)
{
	const uint32_t MB8 = 8u << 20;
	CHECK_EQ(rdp_cimg_impossible(0x6df800, 400, 447, MB8), RDP_CIMG_OK);
	CHECK_EQ(rdp_cimg_impossible(0x7FF000, 640, 480, MB8), RDP_CIMG_OK);  /* empieza dentro */
	CHECK_EQ(rdp_cimg_impossible(0x900000, 455, 44, MB8), RDP_CIMG_RDRAM);
	CHECK_EQ(rdp_cimg_impossible(0x800000, 320, 0, MB8), RDP_CIMG_RDRAM);
	CHECK_EQ(rdp_cimg_impossible(0x900000, 455, 44, 0), RDP_CIMG_OK);  /* sin blindaje */
	CHECK_EQ(rdp_cimg_impossible(0x100000, 0, 0, MB8), RDP_CIMG_WIDTH);
	CHECK_EQ(rdp_cimg_impossible(0x100000, 1495, 0, MB8), RDP_CIMG_WIDTH);
	CHECK_EQ(rdp_cimg_impossible(0x100000, 320, 481, MB8), RDP_CIMG_HEIGHT);
	CHECK_EQ(rdp_cimg_impossible(0x500000, 320, 240, 4u << 20), RDP_CIMG_RDRAM);
}

void t_suite_sifm(void)
{
	RUN(sifm_even_widths_unchanged);
	RUN(sifm_odd_width_padded);
	RUN(sifm_no_room_fails);
	RUN(cimg_impossible);
}
