/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) los autores de GLideN64 (github.com/gonetz/GLideN64), de
 * cuyo src/uCodes/F5Indi_Naboo.cpp sale este archivo («Microcode decoding:
 * olivieryuyu»); adaptación a C y a la HLE de Salsa64 (C) 2026 ricowp81.
 *
 * Salsa64 — HLE del microcódigo gráfico de «Indiana Jones and the Infernal
 * Machine» y «Star Wars Episode I: Battle for Naboo» (Factor 5), traducida a C
 * desde GLideN64.
 *
 * GLideN64 se distribuye bajo la GNU General Public License versión 2 (sola,
 * sin «o posterior»): este archivo conserva esa licencia y el aviso de arriba;
 * el binario que lo enlaza se distribuye como GPLv2 (README.md,
 * docs/THIRD_PARTY.md).
 *
 * Qué es el microcódigo: una Fast3D cuya «memoria» es la DMEM del RSP: casi
 * todo comando es una DMA a ella (G_MOVEMEM 0x01: de la RDRAM a una dirección
 * de la DMEM, o del flujo segmentado de G_SET_DLIST_ADDR) y lo que hace con
 * lo traído depende de a dónde fue (el viewport en 0x130, una matriz en 0x5C0
 * o en 0xE40, vértices, luces, el cálculo de la iluminación sobre 0x480 con
 * resultado en 0xD40…). Los vértices del búfer son de 8 bytes en la DMEM
 * (x, y, z, bandera), los triángulos dicen sus vértices por la dirección de
 * su lugar (0x600 + 40 por vértice) y llevan los colores como índices en la
 * tabla de 0xD40 y, si tienen textura, sus coordenadas detrás; las listas
 * empiezan con una palabra cuyos 24 bits bajos son el destino del 0xB5. Lo de
 * Battle for Naboo (mallas de terreno con colores por vértice, G_GEOMETRY_GEN
 * 0x09, 0x0C y 0x0F, y 0xBE) y de Indiana Jones (partículas, 0x24 y 0x27, y
 * los vértices con la matriz propia, 0x18) cuelga del comando 0x05.
 *
 * Esta traducción lleva una DMEM de 4 KB propia (f5.dm) con la misma
 * disposición que el microcódigo: los accesos «por puntero» de GLideN64 (que
 * corre en un host little-endian con la DMEM por palabras invertidas) se
 * traducen a bytes y medias palabras en el orden natural (DMB, dh, dw).
 *
 * Cambios respecto a GLideN64 (medidos contra el microcódigo real corriendo
 * en el RSP a bajo nivel, salsa64-run --lle=all; docs/estado.md, 2026-10-18):
 * ver la lista en esa entrada. Además, y por el SPE:
 *   - sin memoria dinámica ni std::vector: arreglos de tamaño fijo;
 *   - las direcciones de la lista y de los datos se leen por la E/S de la HLE
 *     (gfx_io_read / gfx_io_dl), no de la RDRAM entera;
 *   - la raíz cuadrada entera y los productos de 64 bits van sin libm;
 *   - lo que el microcódigo escribe en la RDRAM (las partículas, 0x27) sale
 *     por gfx_io_write.
 *
 * En el SPE 0: una imagen propia (spe0.h, GFX_PART_F5INDY); su estado (la
 * DMEM) vive en f5 y persiste entre tareas como la DMEM del RSP; presupuesto
 * de local store en src/spu/gfx/main.c.
 */
#include <string.h>
#ifdef GFX_CENSUS
#include <stdio.h>
#include <stdlib.h>
#endif

#include "core/endian.h"
#include "core/rcp.h"
#include "rsp/gfx/hle.h"

/* ---- La DMEM y el estado del microcódigo ------------------------------- */

#define F5I_DL_DEPTH 10u
#define F5I_NABOO 1

static struct {
	uint8_t dm[4096] __attribute__((aligned(16)));  /* DMEM, bytes en el orden natural (big-endian) */
	uint32_t dl[12];             /* por nivel de la pila de listas: a dónde salta el 0xB5 */
	int32_t st[16];              /* matriz del cálculo de s y t (DMEM 0xE40), 16.16 */
	float gen[16];               /* matriz de G_GEOMETRY_GEN 0x18 (DMEM 0xE40) */
	uint32_t clock;              /* «DPC_CLOCK» de las partículas: contador */
	bool insub;                  /* dentro de la lista secundaria */
	/* Battle for Naboo */
	uint16_t AA;
	int16_t BB, EE, HH;
	uint8_t DD, CC, TT;
} f5;

#define DM (f5.dm)
/* Byte en la posición `m` de la memoria de GLideN64 (palabras invertidas): el
 * byte natural m ^ 3. Los punteros u8 de su código son así. */
#define DMB(m) (DM[((m) & 0xFFF) ^ 3])

static uint32_t dw(uint32_t a)
{
	return salsa64_rd32(DM + (a & 0xFFC));
}

static void dw_set(uint32_t a, uint32_t v)
{
	salsa64_wr32(DM + (a & 0xFFC), v);
}

/* Media palabra natural de dirección par `a`. */
static uint16_t dh(uint32_t a)
{
	return salsa64_rd16(DM + (a & 0xFFE));
}

static void dh_set(uint32_t a, uint16_t v)
{
	salsa64_wr16(DM + (a & 0xFFE), v);
}

static uint32_t umin(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

/* ---- Vértices ---------------------------------------------------------- */

/* Un vértice de 8 bytes transformado en el lugar `i` (como en f5rogue.c): sin luces ni
 * generación de s y t, y con el color en cero (después lo pone el comando
 * que lo usa). */
static void load_vertex(struct gfx_state *g, uint32_t i, int16_t x, int16_t y, int16_t z)
{
	uint8_t buf[16] __attribute__((aligned(16)));
	memset(buf, 0, sizeof buf);
	salsa64_wr16(buf + 0, (uint16_t)x);
	salsa64_wr16(buf + 2, (uint16_t)y);
	salsa64_wr16(buf + 4, (uint16_t)z);
	uint32_t geom = g->geom;
	g->geom = geom & ~(G_LIGHTING | G_TEXTURE_GEN);
	gfx_vertices(g, buf, i, 1);
	g->geom = geom;
	/* La y de pantalla va al revés de Fast3D (GLideN64 la niega tras transformar): se
	 * invierte y, con ella, el recorte en y. */
	struct gfx_vtx *v = &g->vtx[i];
	v->y = -v->y;
	v->sy = 2.0f * g->vtrans[1] - v->sy;
	uint32_t clip = v->clip, yc = clip & (GFX_CLIP_YN | GFX_CLIP_YP);
	if (yc == GFX_CLIP_YN || yc == GFX_CLIP_YP)
		v->clip = clip ^ (GFX_CLIP_YN | GFX_CLIP_YP);
}

/* Vértice de la DMEM (x, y, z, bandera) en el lugar i, con un desplazamiento. */
static void load_dm_vertex(struct gfx_state *g, uint32_t i, uint32_t addr, uint16_t dx, uint16_t dy, uint16_t dz)
{
	if (i >= GFX_MAX_VTX)
		return;
	load_vertex(g, i, (int16_t)(dh(addr) + dx), (int16_t)(dh(addr + 2) + dy), (int16_t)(dh(addr + 4) + dz));
}

/* ---- DMA a la DMEM ----------------------------------------------------- */

static bool copy_in(struct gfx_state *g, uint32_t dst, uint32_t src, uint32_t n)
{
	if (n == 0)
		return true;
	if (dst >= 4096 || n > 480)
		return false;
	if (n > 4096 - dst)
		n = 4096 - dst;
	const uint8_t *s = gfx_io_read(g, GFX_IO_SMALL, src & 0x00FFFFFF, n);
	if (!s) {
		g->res->dl_errors++;
		return false;
	}
	memcpy(DM + dst, s, n);
	return true;
}

static void dma_direct(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	copy_in(g, (w0 >> 8) & 0xFFC, w1, (((w0 & 0xFF) + 1) >> 2) * 4);
}

/* Lo que el microcódigo trae de un flujo de bloques de 0x100 bytes encadenados
 * (G_SET_DLIST_ADDR): FD4 la dirección de lectura, FD8 el enlace al bloque
 * que sigue y FDC lo que queda del actual. */
static __attribute__((noinline)) void dma_segmented(struct gfx_state *g, uint32_t w0)
{
	if ((w0 & 0xFF) == 0)
		return;
	int32_t A0 = (int32_t)(w0 & 0xFF);
	uint32_t A2 = dw(0xFDC);
	int32_t A3 = A0 + 1;
	int32_t A1 = (int32_t)(A2 - (uint32_t)A3);
	uint32_t V1 = (w0 >> 8) & 0xFFF;
	unsigned guard = 0;
	while (A1 < 0 && guard++ < 8) {
		uint32_t V0 = dw(0xFD4);
		A3 = A0 - (int32_t)A2;
		copy_in(g, V1, V0, A2);
		V1 += A2;
		A0 = A3;
		uint32_t link = dw(0xFD8);
		const uint8_t *p = gfx_io_read(g, GFX_IO_SMALL, link & 0x00FFFFFF, 4);
		if (!p)
			return;
		dw_set(0xFD8, salsa64_rd32(p));
		dw_set(0xFDC, 0x100);
		dw_set(0xFD4, link + 8);
		A2 = 0x100;
		A3 = A0 + 1;
		A1 = (int32_t)(A2 - (uint32_t)A3);
	}
	uint32_t V0 = dw(0xFD4);
	copy_in(g, V1, V0, (uint32_t)(A0 + 1));
	dw_set(0xFDC, (uint32_t)A1);
	dw_set(0xFD4, V0 + (uint32_t)A3);
}

/* ---- Iluminación (G_MOVEMEM a 0x480) ----------------------------------- */

/* Geometría de las luces puntuales: (X, Y, Z) con signo de 16 bits de la
 * palabra de la DMEM en `a` y la siguiente. */
struct v3 {
	int16_t x, y, z;
};

static struct v3 dm_v3(uint32_t a)
{
	struct v3 v = { (int16_t)dh(a), (int16_t)dh(a + 2), (int16_t)dh(a + 4) };
	return v;
}

/* Distancia al cuadrado por la atenuación de la luz: el P1 que mezcla el color. */
static uint32_t light_p1(struct v3 d, uint32_t lightAddr)
{
	uint64_t T = (uint64_t)dw(lightAddr + 4);
	T = 0xFFFF000000000000ull | (T << 16);
	int32_t sum = (int32_t)((uint32_t)((int32_t)d.x * d.x) + (uint32_t)((int32_t)d.y * d.y) + (uint32_t)((int32_t)d.z * d.z));
	uint64_t K = (uint64_t)(int64_t)sum * T;
	uint32_t P = (uint32_t)(K >> 32);
	return (P & 0xFFFF0000u) == 0xFFFF0000u ? (P & 0xFFFF) : 0;
}

/* Las posiciones de los dos vértices de un par (según los dos bits de la máscara de 0x5B0). */
static void pair_addrs(uint64_t pos, uint32_t lightAddr, uint32_t out[2])
{
	switch (pos & 3) {
	case 0:
		out[0] = out[1] = lightAddr - 0x10;
		break;
	case 1:
		out[0] = lightAddr - 0x08;
		out[1] = lightAddr - 0x10;
		break;
	case 2:
		out[0] = lightAddr - 0x10;
		out[1] = lightAddr - 0x08;
		break;
	default:
		out[0] = out[1] = lightAddr - 0x08;
		break;
	}
}

static uint64_t pos_mask(void)
{
	return (uint64_t)dw(0x5B0) | (uint64_t)dw(0x5B4) << 32;
}

static void clear_pos_mask(void)
{
	dw_set(0x5B0, 0);
	dw_set(0x5B4, 0);
}

/* Producto de dos bytes (colores o factores de 0 a 255) como lo hace el microcódigo con
 * VMULF sobre los bytes en la mitad alta (LUV: byte << 7): (a·f + 1) >> 8 (m8: lo que sale
 * de los tres primeros pasos: dato · factor), y con un tercer byte (la luz) el producto de
 * ese resultado por ella, redondeado: ((((a·f + 1) >> 1) · l) + 128) >> 15 (m16). GLideN64
 * trunca (a·f >> 8, a·f·l >> 16); las dos coinciden salvo en el último bit. */
static uint32_t m8(uint32_t a, uint32_t f)
{
	return (a * f + 1) >> 8;
}

static uint32_t m16(uint32_t a, uint32_t f, uint32_t l)
{
	return ((((a * f + 1) >> 1) * l) + 128) >> 15;
}

static void light_mul_factors(uint32_t S)
{
	if (dw(0x5A8) == 0xFFFFFFFFu)
		return;
	uint32_t dst = 0xD40;
	for (uint32_t i = 0; i < (S >> 2); i++, dst += 4)
		for (uint32_t j = 0; j < 4; j++)
			DMB(dst + j) = (uint8_t)m8(DMB(dst + j), DMB(0x5A8 + j));
}

static __attribute__((noinline)) void lighting_overlay1(uint32_t w0, uint32_t w1)
{
	const uint32_t S = ((((w1 >> 24) & 0x7F) << 2) + 4) & 0xFFF8;
	memset(DM + 0xD40, 0, S);
	uint8_t L = DM[0x58B];
	if (L == 0) {
		clear_pos_mask();
		return;
	}
	uint32_t lightAddr = 0xB10;
	const uint32_t dataAddr = (w0 >> 8) & 0xFFF;
	for (;;) {
		uint32_t dst = 0xD40, data = dataAddr, light = lightAddr;
		if (DMB(light) == 0) {
			for (uint32_t i = 0; i < (S >> 3); i++) {
				uint32_t factor = 0x158;
				for (unsigned h = 0; h < 2; h++) {
					DMB(dst) = (uint8_t)m8(DMB(data), DMB(factor));
					for (uint32_t j = 1; j < 4; j++)
						DMB(dst + j) = (uint8_t)umin(m16(DMB(data + j), DMB(factor + j), DMB(light + j)) + DMB(dst + j), 0xFF);
					factor += 4;
					dst += 4;
					data += 4;
				}
			}
		} else {
			uint32_t offsetIdx = 0;
			uint64_t vtxAddrPos = pos_mask();
			const uint32_t factors[2] = { dw(0x158), dw(0x15C) };
			for (uint32_t i = 0; i < (S >> 3); i++) {
				uint32_t vtxAddr[2];
				pair_addrs(vtxAddrPos, lightAddr, vtxAddr);
				vtxAddrPos >>= 2;
				for (uint32_t j = 0; j < 2; j++) {
					struct v3 v1 = dm_v3(vtxAddr[j]);
					uint32_t addr = 0x170 + DM[0x380 + (offsetIdx & 0x7F)];
					struct v3 v = dm_v3(addr & 0x0FFC);
					offsetIdx++;
					struct v3 d = { (int16_t)(v.x - v1.x), (int16_t)(v.y - v1.y), (int16_t)(v.z - v1.z) };
					uint32_t P1 = light_p1(d, lightAddr);
					uint32_t lw = dw(lightAddr);
					uint32_t D = ((lw >> 24) * P1) >> 16;
					uint32_t E = (((lw >> 16) & 0xFF) * P1) >> 16;
					uint32_t F = (((lw >> 8) & 0xFF) * P1) >> 16;
					uint32_t J = m8(factors[j] & 0xFF, DMB(data + 0));
					if (DMB(light) == 4) {
						DMB(dst) = (uint8_t)J;
						DMB(dst + 1) = (uint8_t)umin(DMB(dst + 1) + F, 0xFF);
						DMB(dst + 2) = (uint8_t)umin(DMB(dst + 2) + E, 0xFF);
						DMB(dst + 3) = (uint8_t)umin(DMB(dst + 3) + D, 0xFF);
					} else {
						D += m8(factors[j] >> 24, DMB(data + 3));
						E += m8((factors[j] >> 16) & 0xFF, DMB(data + 2));
						F += m8((factors[j] >> 8) & 0xFF, DMB(data + 1));
						DMB(dst) = (uint8_t)J;
						DMB(dst + 1) = (uint8_t)umin(F >> 1, 0xFF);
						DMB(dst + 2) = (uint8_t)umin(E >> 1, 0xFF);
						DMB(dst + 3) = (uint8_t)umin(D >> 1, 0xFF);
					}
					dst += 4;
					data += 4;
				}
			}
		}
		if (--L == 0) {
			clear_pos_mask();
			light_mul_factors(S);
			return;
		}
		lightAddr += 0x18;
	}
}

static __attribute__((noinline)) void lighting_overlay2(uint32_t w0, uint32_t w1)
{
	const uint32_t S = ((((w1 >> 24) & 0x7F) << 2) + 4) & 0xFFF8;
	memset(DM + 0xD40, 0, S + 8);
	uint8_t L = DM[0x58B];
	if (L == 0) {
		clear_pos_mask();
		return;
	}
	uint32_t lightAddr = 0xB10;
	const uint32_t dataAddr = (w0 >> 8) & 0xFFF;
	for (;;) {
		uint32_t dst = 0xD40, data = dataAddr, light = lightAddr;
		if (DMB(light) == 0) {
			for (uint32_t i = 0; i < (S >> 3); i++) {
				uint32_t factor = 0x158;
				for (unsigned h = 0; h < 2; h++) {
					DMB(dst) = (uint8_t)m8(DMB(data), DMB(factor));
					for (uint32_t j = 1; j < 4; j++)
						DMB(dst + j) = (uint8_t)umin(m16(DMB(data + j), DMB(factor + j), DMB(light + j)) + DMB(dst + j), 0xFF);
					factor += 4;
					dst += 4;
					data += 4;
				}
			}
		} else if (DMB(light) == 1) {
			uint32_t factor2Addr = 0x380;
			uint64_t vtxAddrPos = pos_mask();
			const uint32_t factors[2] = { dw(0x158), dw(0x15C) };
			for (uint32_t i = 0; i < (S >> 3); i++) {
				uint32_t vtxAddr[2];
				pair_addrs(vtxAddrPos, lightAddr, vtxAddr);
				vtxAddrPos >>= 2;
				for (uint32_t j = 0; j < 2; j++) {
					struct v3 v = dm_v3(vtxAddr[j]);
					uint32_t V = dw(factor2Addr);
					uint32_t I = (uint32_t)((((int32_t)v.x * (int8_t)(V >> 24) + (int32_t)v.y * (int8_t)(V >> 16) +
								  (int32_t)v.z * (int8_t)(V >> 8)) >> 8) & 0xFFFF);
					if (I & 0x8000)
						I = 0;
					V = dw(lightAddr);
					uint32_t D = ((V >> 24) * I) >> 8;
					uint32_t E = (((V >> 16) & 0xFF) * I) >> 8;
					uint32_t F = (((V >> 8) & 0xFF) * I) >> 8;
					V = factors[j];
					uint32_t K = (V & 0xFF) * DMB(data);
					uint32_t J = ((V >> 8) & 0xFF) * DMB(data + 1);
					uint32_t H = ((V >> 16) & 0xFF) * DMB(data + 2);
					uint32_t G = (V >> 24) * DMB(data + 3);
					data += 4;
					DMB(dst) = (uint8_t)(K >> 8);
					DMB(dst + 1) = (uint8_t)umin(DMB(dst + 1) + ((F * J) >> 22), 0xFF);
					DMB(dst + 2) = (uint8_t)umin(DMB(dst + 2) + ((E * H) >> 22), 0xFF);
					DMB(dst + 3) = (uint8_t)umin(DMB(dst + 3) + ((D * G) >> 22), 0xFF);
					dst += 4;
					factor2Addr += 4;
				}
			}
		} else {
			uint32_t offsetAddr = 0x380;
			const uint32_t factors[2] = { dw(0x158), dw(0x15C) };
			(void)factors;
			struct v3 v1 = dm_v3(lightAddr - 0x10);
			for (uint32_t i = 0; i < (S >> 3); i++) {
				for (uint32_t j = 0; j < 2; j++) {
					uint32_t offset = dw(offsetAddr + j * 4);
					struct v3 v = dm_v3(0x170 + (offset & 0xFF));
					struct v3 d = { (int16_t)(v.x - v1.x), (int16_t)(v.y - v1.y), (int16_t)(v.z - v1.z) };
					uint32_t P1 = light_p1(d, lightAddr);
					uint32_t factor = 0x158 + j * 4;
					DMB(dst) = (uint8_t)m8(DMB(data), DMB(factor));
					for (uint32_t k = 1; k < 4; k++)
						DMB(dst + k) = (uint8_t)umin(((((DMB(light + k) * P1) >> 16) * DMB(data + k) * DMB(factor + k) + 0x8000) >> 16) + DMB(dst + k), 0xFF);
					dst += 4;
					data += 4;
				}
				offsetAddr += 8;
			}
		}
		if (--L == 0) {
			clear_pos_mask();
			return;
		}
		lightAddr += 0x18;
	}
}

static __attribute__((noinline)) void lighting_basic(uint32_t w0, uint32_t w1)
{
	int32_t count = (int32_t)((w1 >> 24) & 0x7F);
	uint32_t data = (w0 >> 8) & 0xFFF, dst = 0xD40;
	while (count > 0) {
		for (unsigned h = 0; h < 2; h++) {
			for (uint32_t j = 0; j < 8; j++)
				DMB(dst + j) = (uint8_t)m8(DMB(data + j), DMB(0x158 + j));
			dst += 8;
			data += 8;
		}
		count -= 4;
	}
}

/* Colores de 16 bits (RGB565) de la DMEM a palabras RGBA8888, en el mismo lugar. */
static __attribute__((noinline)) void rebuild_colors(uint32_t w0, uint32_t w1)
{
	const uint32_t addr = (w0 >> 8) & 0xFFF;
	if (addr >= 0x588)
		return;
	uint32_t count = umin((w1 >> 24) & 0xFF, (0x588 - addr) >> 2);
	for (uint32_t i = count; i-- > 0;) {
		uint32_t V = dh(addr + 2 * i);
		uint32_t R = (V >> 8) & 0xF8, G = (V & 0x7E0) >> 3, B = (V & 0x1F) << 3;
		dw_set(addr + 4 * i, R << 24 | G << 16 | B << 8 | 0xFF);
	}
}

/* ---- G_MOVEMEM y compañía ---------------------------------------------- */

static void load_st_matrix(void)
{
	for (uint32_t i = 0; i < 16; i++)
		f5.st[i] = (int32_t)((uint32_t)dh(0xE40 + 2 * i) << 16 | dh(0xE60 + 2 * i));
}

/* Vértices de la DMEM, sumándoles el desplazamiento de 0x128 (cero: tal cual). */
static __attribute__((noinline)) void vertices_adjust(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	const uint32_t addr = (w0 >> 8) & 0xFFF, n = (w1 >> 24) & 0x7F;
	uint16_t X0 = 0, Y0 = 0, Z0 = 0;
	if (dw(0x128) | dw(0x12C)) {
		X0 = dh(0x128);
		Y0 = dh(0x12A);
		Z0 = dh(0x12C);
	}
	for (uint32_t i = 0; i < n; i++)
		load_dm_vertex(g, i, addr + 8 * i, X0, Y0, Z0);
}

/* Vértices con la posición en una tabla de la RDRAM por índices de la DMEM. */
static __attribute__((noinline)) void vertices_indexed(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	const uint32_t count = (w1 >> 24) & 0x7F, addr = (w0 >> 8) & 0xFFF;
	const uint32_t base = dw(0x154);
	const uint16_t X0 = dh(0x128), Y0 = dh(0x12A), Z0 = dh(0x12C);
	for (uint32_t i = 0; i < count; i++) {
		uint32_t ra = (uint32_t)dh(addr + 2 * i) * 6 + base;
		const uint8_t *p = gfx_io_read(g, GFX_IO_SMALL, ra & 0x00FFFFFE, 6);
		if (!p) {
			g->res->dl_errors++;
			return;
		}
		if (i < GFX_MAX_VTX)
			load_vertex(g, i, (int16_t)(salsa64_rd16(p) + X0), (int16_t)(salsa64_rd16(p + 2) + Y0),
				    (int16_t)(salsa64_rd16(p + 4) + Z0));
	}
}

static __attribute__((noinline)) void cmd_movemem(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	if ((w1 & 0x00FFFFFF) != 0)
		dma_direct(g, w0, w1);
	else
		dma_segmented(g, w0);

	switch (w0 & 0x00F00000) {
	case 0x00000000:
		switch (w0) {
		case 0x0101300F: {
			const uint8_t *s = gfx_io_read(g, GFX_IO_SMALL, gfx_addr(g, w1), 16);
			if (s)
				gfx_set_viewport(g, s);
			break;
		}
		case 0x0105C03F:
			gfx_force_matrix(g, gfx_addr(g, w1));
			break;
		case 0x010E403F: {
			const uint8_t *s = gfx_io_read(g, GFX_IO_MTX, w1 & 0x00FFFFFF, 64);
			if (s)
				gfx_load_matrix(f5.gen, s);
			load_st_matrix();
			break;
		}
		}
		break;
	case 0x00100000:
		vertices_adjust(g, w0, w1);
		break;
	case 0x00300000:
		DM[0x58B] = (uint8_t)(w1 >> 24);
		break;
	case 0x00500000:
		vertices_indexed(g, w0, w1);
		break;
	default:
		if (((w0 >> 8) & 0xFFF) == 0x480) {
			const uint32_t F = w0 >> 22;
			if (F & 2)
				rebuild_colors(w0, w1);
			const uint32_t C = ((w1 >> 24) & 0xFF) | DM[0x58B];
			if ((C & 0x80) == 0) {
				if (F & 1)
					lighting_overlay1(w0, w1);
				else
					lighting_overlay2(w0, w1);
			} else {
				lighting_basic(w0, w1);
			}
		}
		break;
	}
}

static void cmd_set_dlist_addr(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	const uint8_t *p = gfx_io_read(g, GFX_IO_SMALL, w1 & 0x00FFFFFF, 4);
	if (!p)
		return;
	dw_set(0xFD4, w1 + 8);
	dw_set(0xFD8, salsa64_rd32(p));
	dw_set(0xFDC, w0 & 0x1FF);
}

/* ---- Listas ------------------------------------------------------------ */

static void list_entry(struct gfx_state *g)
{
	/* La lista nueva empieza con la palabra de salto de software; el
	 * microcódigo la guarda y sigue desde el segundo comando. */
	uint32_t w0, w1;
	if (g->dl_sp >= (int)F5I_DL_DEPTH || !gfx_io_dl(g, g->dl_pc, &w0, &w1)) {
		f5.dl[g->dl_sp & 15] = 0;
		return;
	}
	f5.dl[g->dl_sp] = w0 & 0x00FFFFFFu;
	g->dl_pc += 8;
}

static void set_other_mode(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	uint32_t mask = (uint32_t)((int32_t)0x80000000 >> (w0 & 0x1F));
	mask >>= (w0 >> 8) & 0x1F;
	const uint32_t A0 = (w0 >> 16) & 7;
	uint32_t *mode = A0 == 0 ? &g->oh : A0 == 4 ? &g->ol : NULL;
	if (!mode)
		return;
	uint32_t v = (*mode & ~mask) | w1;
	if (v != *mode)
		g->dirty = true;
	*mode = v;
}

static uint32_t geom_bits(uint32_t m)
{
	/* La de F3D, con G_SHADE como el sombreado suave (GBI_InitFlags: G_SHADING_SMOOTH = G_SHADE). */
	uint32_t r = m & (G_ZBUFFER | G_SHADE | G_FOG | G_LIGHTING | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR | G_LOD);
	if (m & 0x00001000u)
		r |= G_CULL_FRONT;
	if (m & 0x00002000u)
		r |= G_CULL_BACK;
	return r;
}

static void cmd_moveword(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	const uint32_t dest = w0 & 0xFFF;
	dw_set(dest, w1);
	switch (dest) {
	case 0x04:
	case 0x0C:
		gfx_clip_ratio(g, dest, w1);
		break;
	case 0x160:
		g->fog_mul = (float)(int32_t)w1 * (1.0f / 256.0f);
		break;
	case 0x164:
		g->fog_off = (float)(int32_t)w1 * (1.0f / 256.0f);
		break;
	default:
		break;
	}
}

/* Los comandos de estado (los que caben también en la lista secundaria): lo
 * que no dibuja ni salta. true si el comando era de estos. */
static bool state_cmd(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	const uint32_t op = w0 >> 24;
	switch (op) {
	case 0x00:
		return true;
	case 0xB6:
		gfx_sp_geometry(g, g->geom & ~geom_bits(~w1));
		return true;
	case 0xB7:
		gfx_sp_geometry(g, g->geom | geom_bits(w1));
		return true;
	case 0xB9:
		if (((w0 >> 23) & 1) == dw(0x11C))
			set_other_mode(g, w0, w1);
		return true;
	case 0xBA:
		set_other_mode(g, w0, w1);
		return true;
	case 0xBB:
		gfx_sp_texture(g, w1, (w0 >> 11) & 7, (w0 >> 8) & 7, w0 & 0xFF);
		dw_set(0x148, w0);
		return true;
	case 0xBC:
		cmd_moveword(g, w0, w1);
		return true;
	default:
		if (op >= 0xC0 && op != 0xE4 && op != 0xE5) {
			gfx_rdp_command(g, w0, w1);
			return true;
		}
		return false;
	}
}


/* La lista secundaria (la dirección que dejó un G_MOVEWORD en 0x58C): antes de
 * dibujar, el microcódigo corre sus comandos hasta el B8 000000 FFFFFFFF.
 *
 * Indiana Jones también deja en 0x58C listas que terminan con un B8 000000
 * 00000000 (las de un objeto: un triángulo y su fin, 2026-10-29). Siguiendo
 * hasta el B8 … FFFFFFFF, la HLE seguía más allá del fin por los vértices de
 * 8 bytes que hay detrás y ejecutaba como comandos del RDP los que empiezan
 * con 0xC0 o más: un vértice (−80, 17, 136) es «FFB00011 00880000», un
 * G_SETCIMG de 18 de ancho en 0x880000; otro, un G_SETZIMG en 0x820000. Los
 * dibujos de Indy iban a esa imagen (invisible) y el estado quedaba revuelto
 * (media pantalla negra). Ahora cualquier G_ENDDL cierra la lista;
 * GFX_JOB_F5_SUB_OLD (f5-sublista-vieja) vuelve a lo de antes. */
static void do_sub_dlist(struct gfx_state *g)
{
	const uint32_t addr = dw(0x58C) & 0x00FFFFFF;
	if (addr == 0 || f5.insub)
		return;
	f5.insub = true;
	const uint32_t save = g->dl_pc;
	uint32_t pc = addr + 8;
#ifdef GFX_CENSUS
	static int trace = -1;
	if (trace < 0)
		trace = getenv("SALSA64_F5_SUB_TRACE") != NULL;
	bool past = false;
#endif
	for (;;) {
		uint32_t w0, w1, n0, n1;
		if (g->res->commands >= GFX_MAX_COMMANDS || !gfx_io_dl(g, pc, &w0, &w1)) {
			g->res->dl_errors++;
			break;
		}
		if (w0 == 0xB8000000u && w1 == 0xFFFFFFFFu)
			break;
		if ((w0 >> 24) == 0xB8) {
			g->res->flags |= GFX_RES_F5_SUB_END;
			if (!(g->job->flags & GFX_JOB_F5_SUB_OLD))
				break;
#ifdef GFX_CENSUS
			if (trace)
				fprintf(stderr, "f5 sublista %06x: B8 %08x en %06x; sigue (f5-sublista-vieja)\n", addr, w1, pc);
			past = true;
#endif
		}
#ifdef GFX_CENSUS
		if (trace && past && (w0 >> 24) >= 0xC0)
			fprintf(stderr, "f5 sublista %06x: más allá del fin, %06x: %08x %08x\n", addr, pc, w0, w1);
#endif
		g->res->commands++;
		const bool next_bd = gfx_io_dl(g, pc + 8, &n0, &n1) && (n0 >> 24) == 0xBD;
		g->dl_pc = pc + 8;
		state_cmd(g, w0, w1);
		pc = g->dl_pc;
		if (next_bd) {
			dw_set(0xE58, w0);
			dw_set(0xE5C, w1);
			break;
		}
	}
#ifdef GFX_CENSUS
	if (trace)
		fprintf(stderr, "f5 sublista %06x: hasta %06x\n", addr, pc);
#endif
	g->dl_pc = save;
	dw_set(0x58C, 0);
	f5.insub = false;
}

/* ---- Triángulos -------------------------------------------------------- */

static uint32_t isqrt32(uint32_t v)
{
	uint32_t r = 0, bit = 1u << 30;
	while (bit > v)
		bit >>= 2;
	while (bit) {
		if (v >= r + bit) {
			v -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return r;
}

/* Coordenadas de textura de «mapeo de entorno»: la posición del vértice por la
 * matriz de ajuste, normalizada. `idx`: bytes bajos de los datos del vértice
 * (su lugar en 0x170). */
static uint32_t calc_st(uint32_t idx)
{
	const int32_t *mtx = f5.st;
	const int32_t X = (int16_t)dh(0x170 + idx), Y = (int16_t)dh(0x172 + idx), Z = (int16_t)dh(0x174 + idx);
	int32_t X1 = (int32_t)((uint32_t)X * (uint32_t)mtx[0] + (uint32_t)Y * (uint32_t)mtx[4] + (uint32_t)Z * (uint32_t)mtx[8] + (uint32_t)mtx[12]);
	int32_t Y1 = (int32_t)((uint32_t)X * (uint32_t)mtx[1] + (uint32_t)Y * (uint32_t)mtx[5] + (uint32_t)Z * (uint32_t)mtx[9] + (uint32_t)mtx[13]);
	int32_t Z1 = (int32_t)((uint32_t)X * (uint32_t)mtx[2] + (uint32_t)Y * (uint32_t)mtx[6] + (uint32_t)Z * (uint32_t)mtx[10] + (uint32_t)mtx[14]);
	X1 = (int32_t)((uint32_t)X1 - ((uint32_t)dh(0xF0) << 16));
	Y1 = (int32_t)((uint32_t)Y1 - ((uint32_t)dh(0xF2) << 16));
	Z1 = (int32_t)((uint32_t)Z1 - ((uint32_t)dh(0xF4) << 16));
	const uint32_t ax = X1 < 0 ? 0u - (uint32_t)X1 : (uint32_t)X1;
	const uint32_t ay = Y1 < 0 ? 0u - (uint32_t)Y1 : (uint32_t)Y1;
	const uint32_t az = Z1 < 0 ? 0u - (uint32_t)Z1 : (uint32_t)Z1;
	uint64_t R = ((uint64_t)ax * ax >> 16) + ((uint64_t)ay * ay >> 16) + ((uint64_t)az * az >> 16);
	uint32_t R1 = (uint32_t)R;
	if (R > 0x0000FFFFFFFFull)
		R1 = 0x7FFF0000u | (uint32_t)(R & 0xFFFF);
	uint32_t D = isqrt32(R1);
	uint32_t den = D * 0x300u;
	D = den ? 0xFFFFFFFFu / den : 0xFFFFFFFFu;
	uint32_t V = (uint32_t)(((uint64_t)D * (uint64_t)(int64_t)X1) >> 16);
	uint32_t W = (uint32_t)(((uint64_t)D * (uint64_t)(int64_t)Y1) >> 16);
	uint32_t S = (V * dh(0xEC)) >> 16;
	uint32_t T = (W * dh(0xEE)) >> 16;
	return S << 16 | (T & 0xFFFF);
}

static bool tri_clipped(const struct gfx_state *g, uint32_t a, uint32_t b, uint32_t c)
{
	return (g->vtx[a].clip & g->vtx[b].clip & g->vtx[c].clip & GFX_CLIP_MASK) != 0;
}

/* Prepara tres vértices para dibujar: sin sombreado, el color de PRIM en todos.
 * false si el triángulo queda fuera de la pantalla. */
static bool add_vertices(struct gfx_state *g, uint32_t a, uint32_t b, uint32_t c)
{
	if (tri_clipped(g, a, b, c))
		return false;
	if (!(g->geom & G_SHADE)) {
		const uint32_t idx[3] = { a, b, c };
		for (unsigned i = 0; i < 3; i++) {
			uint8_t *col = g->vtx[idx[i]].c;
			col[0] = (uint8_t)(g->prim >> 24);
			col[1] = (uint8_t)(g->prim >> 16);
			col[2] = (uint8_t)(g->prim >> 8);
			col[3] = (uint8_t)g->prim;
		}
	}
	return true;
}

static void draw_tri_now(struct gfx_state *g, uint32_t a, uint32_t b, uint32_t c, bool tex)
{
	const uint32_t geom = g->geom;
	uint32_t ng = geom | G_SHADING_SMOOTH;
	if (tex) {
		if ((ng & (G_CULL_FRONT | G_CULL_BACK)) == (G_CULL_FRONT | G_CULL_BACK))
			ng &= ~G_CULL_FRONT;
	} else {
		ng &= ~(G_CULL_FRONT | G_CULL_BACK);
	}
	if (ng != geom)
		gfx_sp_geometry(g, ng);
	gfx_triangle(g, a, b, c);
	if (g->geom != geom)
		gfx_sp_geometry(g, geom);
}

static __attribute__((noinline)) void cmd_tri(struct gfx_state *g, uint32_t w0, uint32_t w1, uint32_t ca, bool tri2)
{
	const bool tex = (w0 & 0x0200) != 0;
	const uint8_t *p = gfx_io_read(g, GFX_IO_SMALL, ca + 8, tex ? 24 : 8);
	g->dl_pc = ca + (tex ? 32 : 16);
	if (!p) {
		g->res->dl_errors++;
		return;
	}
	const uint32_t w2 = salsa64_rd32(p), w3 = salsa64_rd32(p + 4);
	const uint32_t v[4] = { (((w1 >> 16) & 0xFFF) - 0x600u) / 40, (((w1) & 0xFFF) - 0x600u) / 40,
				(((w3 >> 16) & 0xFFF) - 0x600u) / 40, (((w3) & 0xFFF) - 0x600u) / 40 };
	const uint32_t cidx[4] = { (w2 >> 16) & 0xFF, (w2 >> 8) & 0xFF, w2 & 0xFF, (w2 >> 24) & 0xFF };
	const uint32_t n = tri2 ? 4 : 3;
	uint32_t st[4] = { 0, 0, 0, 0 };
	if (tex) {
		if (w0 & 0x0800) {
			for (uint32_t i = 0; i < n; i++)
				st[i] = calc_st(salsa64_rd32(p + 8 + 4 * i) & 0xFF);
		} else {
			for (uint32_t i = 0; i < n; i++)
				st[i] = salsa64_rd32(p + 8 + 4 * i);
		}
	}
	for (uint32_t i = 0; i < n; i++) {
		if (v[i] >= GFX_MAX_VTX)
			return;
		struct gfx_vtx *vt = &g->vtx[v[i]];
		const uint32_t c = dw(0xD40 + cidx[i]);
		vt->c[0] = (uint8_t)(c >> 24);
		vt->c[1] = (uint8_t)(c >> 16);
		vt->c[2] = (uint8_t)(c >> 8);
		vt->c[3] = (uint8_t)c;
		if (tex) {
			/* La escala de G_TEXTURE no cuenta (el juego la manda en cero). */
			/* El RSP real pasa s y t al RDP tal cual (s10.5): 1/32 de texel, con o sin
			 * perspectiva (GLideN64 usa 1/16 sin ella; medido contra --lle=all). */
			const float k = 1.0f / 32.0f;
			vt->s = (float)(int16_t)(st[i] >> 16) * k;
			vt->t = (float)(int16_t)st[i] * k;
		}
	}
	bool added = add_vertices(g, v[0], v[1], v[2]);
	if (tri2)
		added |= add_vertices(g, v[0], v[2], v[3]);
	if (!added)
		return;
	do_sub_dlist(g);
	draw_tri_now(g, v[0], v[1], v[2], tex);
	if (tri2)
		draw_tri_now(g, v[0], v[2], v[3], tex);
}

/* ---- Comando 0x05: generación de geometría ------------------------------ */

/* Vértices de la DMEM (0x170…) con la matriz de 0xE40 en lugar de la combinada. */
static __attribute__((noinline)) void gen_vertices(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	float saved[16];
	const bool dirty = g->mvp_dirty;
	memcpy(saved, g->mvp, sizeof saved);
	g->mvp_dirty = false;
	memcpy(g->mvp, f5.gen, sizeof saved);
	uint32_t A = (w0 & 0x0000FFFF) | (w1 & 0xFFFF0000u), B = 1, count = 0;
	while (A != 0 && count < 32) {
		uint32_t C = A & B;
		if (C != 0) {
			A ^= C;
			load_dm_vertex(g, count, 0x170 + 8 * count, 0, 0, 0);
		}
		count++;
		B <<= 1;
	}
	memcpy(g->mvp, saved, sizeof saved);
	g->mvp_dirty = dirty;
}

/* Flotante no negativo → entero sin pasarse de 2^32. */
static uint32_t ftou(float f)
{
	return f >= 4294967040.0f ? 0xFFFFFF00u : (uint32_t)f;
}

/* ---- Partículas y rectángulos de textura generados (Indiana Jones) ------- */

/* Escribe en la RDRAM lo que el microcódigo deja ahí (los vértices de las partículas). */
static void rdram_write(struct gfx_state *g, uint32_t addr, const uint8_t *src, uint32_t n)
{
	gfx_io_write(g, addr & 0x00FFFFFF, src, n);
}

/* Un rectángulo de textura de pantalla: esquinas en píxeles (flotantes; la
 * esquina de arriba a la izquierda no baja de 0), s, t y dsdx, dtdy en el
 * formato del RDP. */
static void emit_texrect(struct gfx_state *g, float ulx, float uly, float lrx, float lry, uint32_t tile, uint16_t S,
			 uint16_t T, uint16_t dsdx, uint16_t dtdy, bool flip)
{
	int ix0 = ulx > 0.0f ? (int)(ulx * 4.0f) : 0, iy0 = uly > 0.0f ? (int)(uly * 4.0f) : 0;
	int ix1 = lrx > 0.0f ? (int)(lrx * 4.0f) : 0, iy1 = lry > 0.0f ? (int)(lry * 4.0f) : 0;
	uint32_t r0 = 0xE4000000u | ((uint32_t)(ix1 & 0xFFF) << 12) | (uint32_t)(iy1 & 0xFFF);
	uint32_t r1 = (tile << 24) | ((uint32_t)(ix0 & 0xFFF) << 12) | (uint32_t)(iy0 & 0xFFF);
	gfx_texrect(g, r0, r1, (uint32_t)S << 16 | T, (uint32_t)dsdx << 16 | dtdy, flip);
}

static void set_prim_depth_color(struct gfx_state *g, const struct gfx_vtx *v, uint32_t prim)
{
	float z15 = v->sz * (32767.0f / 1023.0f);
	z15 = z15 < 0.0f ? 0.0f : z15 > 32767.0f ? 32767.0f : z15;
	gfx_rdp_command(g, 0xEE000000u, (uint32_t)(uint16_t)(int)z15 << 16);
	gfx_rdp_command(g, 0xFA000000u, prim);
}

/* Máscara de los vértices que quedan dentro de las cajas de límites (DMEM en
 * limAddr…): sus bits se apagan. `vb` son los 32 vértices de la RDRAM. */
static uint32_t particle_mask(const uint8_t *vb, uint32_t mask, uint32_t limAddr)
{
	const int16_t XL = (int16_t)dh(limAddr), YL = (int16_t)dh(limAddr + 2), ZL = (int16_t)dh(limAddr + 4);
	const int16_t XR = (int16_t)dh(limAddr + 8), YR = (int16_t)dh(limAddr + 10), ZR = (int16_t)dh(limAddr + 12);
	uint32_t S = 0xFFFFFFFEu;
	for (uint32_t i = 0; i < 0x20; i++) {
		const int16_t x = (int16_t)salsa64_rd16(vb + 8 * i), y = (int16_t)salsa64_rd16(vb + 8 * i + 2),
			      z = (int16_t)salsa64_rd16(vb + 8 * i + 4);
		if (x > XL && x < XR && y > YL && y < YR && z > ZL && z < ZR)
			mask &= S;
		S = (S << 1) | 1;
	}
	return mask;
}

/* G_GEOMETRY_GEN 0x24: ca = dirección del comando (10 palabras): rectángulos
 * de textura a partir de los vértices de las partículas. */
static __attribute__((noinline)) void texrect_gen_particles(struct gfx_state *g, uint32_t ca)
{
	uint32_t p[10];
	{
		const uint8_t *s = gfx_io_read(g, GFX_IO_SMALL, ca, 40);
		if (!s)
			return;
		for (unsigned i = 0; i < 10; i++)
			p[i] = salsa64_rd32(s + 4 * i);
	}
	const uint32_t vp = dw(0x130), vpx = (vp >> 17) & 0x7FFF, vpy = (vp >> 1) & 0x7FFF;
	const uint32_t maxX = ((p[0] & 0xFFFF) * vpx) >> 16, maxY = ((p[1] >> 16) * vpy) >> 16;
	const uint64_t MX = (uint64_t)((p[4] & 0xFFFF0000u) | (p[5] >> 16));
	const uint64_t xScale = MX * (p[7] >> 16) * vpx;
	const uint64_t MY = (uint64_t)(((p[4] & 0xFFFF) << 16) | (p[5] & 0xFFFF));
	const uint64_t yScale = MY * (p[7] & 0xFFFF) * vpy;
	const uint32_t mp = dw(0x5C8);
	const uint64_t Q = (uint64_t)((p[6] & 0xFFFF0000u) | (mp >> 16));
	const uint64_t R = (uint64_t)(((p[6] & 0xFFFF) << 16) | (mp & 0xFFFF));
	const uint32_t colorScale = p[3];

	int32_t count = (int32_t)((p[0] >> 16) & 0xFF);
	uint32_t vtxAddr = p[2] & 0x00FFFFFF;
	uint8_t vb[256] __attribute__((aligned(16)));
	for (; count > 0; --count, vtxAddr += 0x100) {
		const uint8_t *s = gfx_io_read(g, GFX_IO_SMALL, vtxAddr, 256);
		if (!s) {
			g->res->dl_errors++;
			return;
		}
		memcpy(vb, s, 256);
		for (uint32_t i = 0; i < 0x20; i++)
			load_vertex(g, i, (int16_t)salsa64_rd16(vb + 8 * i), (int16_t)salsa64_rd16(vb + 8 * i + 2),
				    (int16_t)salsa64_rd16(vb + 8 * i + 4));
		uint32_t K = p[8], B = p[9] & 0xFF, limAddr = 0xC28;
		while (B != 0) {
			K = particle_mask(vb, K, limAddr);
			B--;
			limAddr += 0x10;
		}
		for (uint32_t i = 0; i < 0x20; i++) {
			const uint32_t D = K & 1;
			K >>= 1;
			if (D == 0)
				continue;
			const struct gfx_vtx *v = &g->vtx[i];
			if (v->clip & (GFX_CLIP_XN | GFX_CLIP_XP | GFX_CLIP_YN | GFX_CLIP_YP | GFX_CLIP_W))
				continue;
			const float screenX = v->sx, screenY = v->sy;
			const uint32_t w_i = v->w >= 1.0f ? (uint32_t)v->w : 1u;
			const uint32_t inv_w = 0xFFFFFFFFu / w_i;
			const uint32_t flag = salsa64_rd16(vb + 8 * i + 6);
			const uint32_t val = dw(0x480 + ((flag >> 8) & 0xFF));
			const uint32_t A = val >> 16, C = val & 0xFFFF;

			uint64_t offset_x_i = (xScale * inv_w) >> 32;
			offset_x_i = (offset_x_i * A) >> 32;
			if (offset_x_i > maxX)
				offset_x_i = maxX;
			if (offset_x_i < 4)
				offset_x_i = 4;
			const float offset_x_f = (float)(uint32_t)offset_x_i * 0.25f;
			const float ulx = screenX - offset_x_f;
			const int32_t G = ulx > 0.0f ? 0 : (int32_t)ulx;
			const float lrx = screenX + offset_x_f > 0.0f ? screenX + offset_x_f : 0.0f;
			if (lrx - ulx <= 0.0f)
				continue;

			uint64_t offset_y_i = (yScale * inv_w) >> 32;
			offset_y_i = (offset_y_i * C) >> 32;
			if (offset_y_i > maxY)
				offset_y_i = maxY;
			if (offset_y_i < 4)
				offset_y_i = 4;
			const float offset_y_f = (float)(uint32_t)offset_y_i * 0.25f;
			const float uly = screenY - offset_y_f;
			const int32_t H = uly > 0.0f ? 0 : (int32_t)uly;
			const float lry = screenY + offset_y_f > 0.0f ? screenY + offset_y_f : 0.0f;
			if (lry - uly <= 0.0f)
				continue;

			const uint64_t Q1 = Q * 0x40 / offset_x_i;
			const uint16_t dsdx_i = (uint16_t)(Q1 >> 16);
			const uint64_t R1 = R * 0x40 / offset_y_i;
			const uint16_t dtdy_i = (uint16_t)(R1 >> 16);

			uint64_t U = Q1 * 0x40;
			if (U > 0x7FFFFFFFull)
				U = 0x7FFFFFFFull;
			const uint16_t U1 = (uint16_t)(((0u - (uint32_t)U) * (uint32_t)G) >> 16);
			const uint16_t S = (uint16_t)((U1 - 2) * 8);
			uint64_t V = R1 * 0x40;
			if (V > 0x7FFFFFFFull)
				V = 0x7FFFFFFFull;
			const uint16_t V1 = (uint16_t)(((0u - (uint32_t)V) * (uint32_t)H) >> 16);
			const uint16_t T = (uint16_t)((V1 - 2) * 8);

			const uint32_t pc = dw(0x380 + (flag & 0xFF));
			const uint32_t prim = ((((pc >> 24) * (colorScale >> 24)) >> 8) & 0xFF) << 24 |
					      ((((pc >> 16 & 0xFF) * (colorScale >> 16 & 0xFF)) >> 8) & 0xFF) << 16 |
					      ((((pc >> 8 & 0xFF) * (colorScale >> 8 & 0xFF)) >> 8) & 0xFF) << 8 |
					      ((((pc & 0xFF) * (colorScale & 0xFF)) >> 8) & 0xFF);
			set_prim_depth_color(g, v, prim);
			emit_texrect(g, ulx, uly, lrx, lry, 0, S, T, dsdx_i, dtdy_i, false);
		}
	}
}

/* G_GEOMETRY_GEN 0x27: ca = dirección del comando (6 palabras): mueve las
 * partículas (vértices en la RDRAM) y deja sus vértices nuevos en la RDRAM. */
static __attribute__((noinline)) void gen_particle_vertices(struct gfx_state *g, uint32_t ca)
{
	uint32_t p[6];
	{
		const uint8_t *s = gfx_io_read(g, GFX_IO_SMALL, ca, 24);
		if (!s)
			return;
		for (unsigned i = 0; i < 6; i++)
			p[i] = salsa64_rd32(s + 4 * i);
	}
	{
		const uint8_t *s = gfx_io_read(g, GFX_IO_SMALL, p[3] & 0x00FFFFFF, 8);
		if (!s)
			return;
		memcpy(DM + 0xC10, s, 8);
	}
	const uint32_t L = dw(0xC10);
	if (L == 0)
		return;
	{
		const uint8_t *s = gfx_io_read(g, GFX_IO_SMALL, p[2] & 0x00FFFFFF, 256);
		if (!s)
			return;
		memcpy(DM + 0x170, s, 256);
	}
	const uint32_t corrector = p[0] & 0xFFFF;
	const uint32_t M = p[5] & 0xFFFF;
	uint32_t U = L, vtxAddr = 0x170, lightAddr = 0xB00, factorAddr = 0x380, colorAddr = 0x480, count = 0, Q = 0;
	while (U != 0 && count < 32) {
		uint16_t F = (uint16_t)((((uint32_t)dh(lightAddr) << 12) + (uint32_t)dh(lightAddr + 2) * corrector) << 4 >> 16);
		bool endCycle = false;
		if ((M & 0x2000) == 0) {
			int16_t F1;
			unsigned guard = 0;
			do {
				F = (uint16_t)((((uint32_t)dh(lightAddr) << 12) + (uint32_t)dh(lightAddr + 2) * corrector) << 4 >> 16);
				F1 = (int16_t)(F + 0xF000);
				if (F1 < 0) {
					dh_set(lightAddr, F);
					if (M & 0x40)
						dh_set(vtxAddr + 6, (uint16_t)((((F >> 4) & 0xFC) << 8) | (dh(vtxAddr + 6) & 0xFF)));
					if (M & 0x20)
						dh_set(vtxAddr + 6, (uint16_t)(((F >> 4) & 0xFC) | (dh(vtxAddr + 6) & 0xFF00)));
				} else {
					dh_set(lightAddr, 0);
					if (M & 0x4000) {
						const uint32_t clk = ++f5.clock * 0x9E3779B1u >> 8;
						const uint32_t V0 = dw(0xC18), V1 = dw(0xC1C), V2 = dw(0xC20), V3 = dw(0xC24);
						dh_set(vtxAddr, (uint16_t)((clk & (V0 >> 16)) + (V2 >> 16)));
						dh_set(vtxAddr + 2, (uint16_t)(((clk >> 3) & (V0 & 0xFFFF)) + (V2 & 0xFFFF)));
						dh_set(vtxAddr + 4, (uint16_t)(((clk >> 6) & (V1 >> 16)) + (V3 >> 16)));
						dh_set(vtxAddr + 6, (uint16_t)(dh(vtxAddr + 6) & (V1 & 0xFFFF)));
					} else {
						Q |= 1u << count;
						endCycle = true;
						break;
					}
				}
			} while (F1 >= 0 && ++guard < 16);
		}
		if (!endCycle) {
			uint32_t X = ((uint32_t)dh(vtxAddr) & 0xFFFF) << 12;
			uint32_t Y = ((uint32_t)dh(vtxAddr + 2) & 0xFFFF) << 12;
			uint32_t Z = ((uint32_t)dh(vtxAddr + 4) & 0xFFFF) << 12;
			X += (uint32_t)(int32_t)(int16_t)(p[4] >> 16) * corrector;
			Y += (uint32_t)(int32_t)(int16_t)p[4] * corrector;
			Z += (uint32_t)(int32_t)(int16_t)(p[5] >> 16) * corrector;
			if ((M & 0x0380) != 0) {
				uint32_t offset = factorAddr - 0x380;
				if ((M & 0x0200) == 0) {
					if ((M & 0x080) != 0)
						offset = (F >> 4) & 0xF8;
					else
						offset = (dw(lightAddr + 4) >> 24) << 3;
				}
				const uint32_t f0 = dw(0x380 + offset), f1 = dw(0x380 + offset + 4);
				X += (uint32_t)(int32_t)(int16_t)(f0 >> 16) * corrector;
				Y += (uint32_t)(int32_t)(int16_t)f0 * corrector;
				Z += (uint32_t)(int32_t)(int16_t)(f1 >> 16) * corrector;
			}
			if ((M & 0x1C00) != 0) {
				uint32_t offset = colorAddr - 0x480;
				if ((M & 0x1000) == 0) {
					if ((M & 0x0400) != 0)
						offset = (F >> 4) & 0xF8;
					else
						offset = ((dw(lightAddr + 4) >> 16) & 0xFF) << 3;
				}
				const uint32_t f0 = dw(0x480 + offset), f1 = dw(0x480 + offset + 4);
				X += (uint32_t)(int32_t)(int16_t)(f0 >> 16) * corrector;
				Y += (uint32_t)(int32_t)(int16_t)f0 * corrector;
				Z += (uint32_t)(int32_t)(int16_t)(f1 >> 16) * corrector;
			}
			X = (X << 4) >> 16;
			Y = (Y << 4) >> 16;
			Z = (Z << 4) >> 16;
			dh_set(vtxAddr, (uint16_t)X);
			dh_set(vtxAddr + 2, (uint16_t)Y);
			dh_set(vtxAddr + 4, (uint16_t)Z);
			if (M & 0x0008) {
				const uint32_t V0 = dw(0xC00), V1 = dw(0xC04), V2 = dw(0xC08), V3 = dw(0xC0C);
				int16_t x = (int16_t)dh(vtxAddr), y = (int16_t)dh(vtxAddr + 2), z = (int16_t)dh(vtxAddr + 4);
				if (x < (int16_t)(V0 >> 16))
					x = (int16_t)(x + (int16_t)(V2 >> 16));
				if (x > (int16_t)(V0 >> 16) + (int16_t)(V2 >> 16))
					x = (int16_t)(x - (int16_t)(V2 >> 16));
				if (y < (int16_t)V0)
					y = (int16_t)(y + (int16_t)V2);
				if (y > (int16_t)V0 + (int16_t)V2)
					y = (int16_t)(y - (int16_t)V2);
				if (z < (int16_t)(V1 >> 16))
					z = (int16_t)(z + (int16_t)(V3 >> 16));
				if (z > (int16_t)(V1 >> 16) + (int16_t)(V3 >> 16))
					z = (int16_t)(z - (int16_t)(V3 >> 16));
				dh_set(vtxAddr, (uint16_t)x);
				dh_set(vtxAddr + 2, (uint16_t)y);
				dh_set(vtxAddr + 4, (uint16_t)z);
			}
		}
		vtxAddr += 8;
		lightAddr += 8;
		factorAddr += 8;
		colorAddr += 8;
		U >>= 1;
		count++;
	}

	rdram_write(g, p[2], DM + 0x170, 256);
	if ((M & 0x04) == 0) {
		uint8_t w[4];
		salsa64_wr32(w, L & ~Q);
		rdram_write(g, p[3], w, 4);
		rdram_write(g, p[1] >> 8, DM + 0xB00, count * 8);
	}
}

/* ---- Battle for Naboo (sin respaldo para probarlo: lo de aquí sigue a GLideN64 tal cual) --- */
#ifdef F5I_NABOO

static float naboo_k(void)
{
	return 1.0f / 32.0f;
}

/* Los dos comandos (ocho bytes cada uno) que hay en la DMEM en `addr`. */
static void naboo_do_commands(struct gfx_state *g, uint32_t addr)
{
	for (unsigned i = 0; i < 2; i++)
		state_cmd(g, dw(addr + 8 * i), dw(addr + 8 * i + 4));
}

static void naboo_draw_tri(struct gfx_state *g, uint32_t a, uint32_t b, uint32_t c)
{
	if (!add_vertices(g, a, b, c))
		return;
	const uint32_t A = (dw(0x100) >> 8) & 0xFFFF, B = DM[0x103], C = A | B;
	dh_set(0x100, (uint16_t)C);
	if ((dw(0x58C) & 0x00FFFFFF) == 0) {
		if ((C << 8) != B)
			naboo_do_commands(g, 0xE50 + B);
		draw_tri_now(g, a, b, c, true);
		return;
	}
	do_sub_dlist(g);
	dw_set(0xE50, 0xEF000000u | (g->oh & 0x00FFFFFFu));
	dw_set(0xE54, g->ol);
	const uint32_t E = (0xEF000000u | (g->oh & 0x00FFFFFFu)) & 0xFFCFFFFFu;
	dw_set(0xE70, E);
	dw_set(0xE60, E | 0x00100000u);
	const uint8_t G = DM[0x101];
	if (G != 0)
		naboo_do_commands(g, 0xE50 + G);
	draw_tri_now(g, a, b, c, true);
}

static void naboo_set_color(struct gfx_state *g, uint32_t vi, uint32_t m)
{
	struct gfx_vtx *vt = &g->vtx[vi];
	vt->c[0] = DMB(m + 0);
	vt->c[1] = DMB(m + 1);
	vt->c[2] = DMB(m + 2);
	vt->c[3] = DMB(m + 3);
}

/* Las dos pasadas de triángulos de una malla. */
static void naboo_step3(struct gfx_state *g, uint32_t GG)
{
	uint32_t idxs[3], vtx[3], alphaSum = 0;
	for (unsigned i = 0; i < 3; i++)
		idxs[i] = dh(GG + 2 * (i == 0 ? 9 : i == 1 ? 10 : 11)) + f5.CC;
	for (unsigned i = 0; i < 3; i++) {
		const uint32_t vtxAddr = dh(0xD58 + 2 * (idxs[i] / 2));
		vtx[i] = (vtxAddr - 0x600u) / 40;
		if (vtx[i] >= GFX_MAX_VTX)
			return;
		const uint32_t m = 0x0B78 + idxs[i] * 2 + ((uint32_t)f5.HH & 0xFFF);
		naboo_set_color(g, vtx[i], m);
		alphaSum += g->vtx[vtx[i]].c[3];
	}
	if (f5.HH == 0) {
		dw_set(0x100, 0);
		naboo_draw_tri(g, vtx[0], vtx[1], vtx[2]);
	} else if (alphaSum == 0) {
		DM[0x103] = 0x00;
		naboo_draw_tri(g, vtx[0], vtx[1], vtx[2]);
	} else if (f5.HH > 0) {
		DM[0x103] = 0x10;
		naboo_draw_tri(g, vtx[0], vtx[1], vtx[2]);
	} else if (alphaSum != 0x02FD) {
		DM[0x103] = 0x00;
		naboo_draw_tri(g, vtx[0], vtx[1], vtx[2]);
	} else {
		uint32_t alphaSum2 = 0;
		uint8_t alphas[3];
		for (unsigned i = 0; i < 3; i++) {
			const uint32_t m = 0xB00 + idxs[i] * 2 + ((uint32_t)f5.HH & 0xFFF);
			alphas[i] = DMB(m + 3);
			alphaSum2 += alphas[i];
		}
		if (alphaSum2 == 0) {
			g->vtx[vtx[0]].c[3] = alphas[0];
			DM[0x103] = 0x20;
		} else {
			DM[0x103] = 0x10;
			for (unsigned i = 0; i < 3; i++)
				g->vtx[vtx[i]].c[3] = alphas[i];
		}
		naboo_draw_tri(g, vtx[0], vtx[1], vtx[2]);
	}
}

static void naboo_run_polygons(struct gfx_state *g, uint32_t step)
{
	for (unsigned guard = 0; guard < 4096; guard++) {
		switch (step) {
		case 3:
			naboo_step3(g, (uint32_t)(int32_t)f5.EE - f5.DD);
			naboo_step3(g, (uint32_t)(int32_t)f5.EE - f5.DD + 0x08);
			step = 4;
			break;
		case 4:
			if (f5.DD == 0x10) {
				step = 6;
			} else {
				f5.CC = (uint8_t)(f5.CC + f5.TT);
				if (((f5.AA & 0xFF) | ((uint16_t)f5.BB & 0xFF)) == 0)
					return;
				step = 5;
			}
			break;
		case 5:
			f5.DD = 0x10;
			if ((f5.AA & 0x80) == 0) {
				f5.AA = (uint16_t)(f5.AA << 1);
				step = 6;
			} else if ((f5.AA & 0x8000) == 0) {
				f5.AA = (uint16_t)(f5.AA << 1);
				step = 3;
			} else {
				f5.AA = (uint16_t)(f5.AA << 1);
				return;
			}
			break;
		default:
			f5.DD = 0;
			if (((uint16_t)f5.BB & 0x80) == 0) {
				f5.BB = (int16_t)((uint16_t)f5.BB << 1);
				step = 4;
			} else if (((uint16_t)f5.BB & 0x8000) == 0) {
				f5.BB = (int16_t)((uint16_t)f5.BB << 1);
				step = 3;
			} else {
				f5.BB = (int16_t)((uint16_t)f5.BB << 1);
				return;
			}
			break;
		}
	}
}

/* 0xBE: las coordenadas de textura del cuadrilátero y el recorrido de la malla;
 * ca = dirección del comando. Con el comando 0x05 (mallas), step = 5. */
static __attribute__((noinline)) void naboo_draw_polygons(struct gfx_state *g, uint32_t ca)
{
	const uint8_t *pp = gfx_io_read(g, GFX_IO_SMALL, ca, 8);
	if (!pp)
		return;
	const uint32_t p0 = salsa64_rd32(pp), p1 = salsa64_rd32(pp + 4);
	if ((p0 >> 24) == 0xBE) {
		f5.HH = (int16_t)(p0 & 0xFFFF);
		const uint32_t GG = (uint32_t)(int32_t)f5.EE - f5.DD;
		const uint32_t Idx1 = (dh(GG + 18) + f5.CC) >> 1, Idx2 = (dh(GG + 20) + f5.CC) >> 1;
		const uint32_t Idx3 = (dh(GG + 22) + f5.CC) >> 1, Idx4 = (dh(GG + 28) + f5.CC) >> 1;
		const uint32_t v1 = (dh(0xD58 + 2 * Idx1) - 0x600u) / 40, v2 = (dh(0xD58 + 2 * Idx2) - 0x600u) / 40;
		const uint32_t v3 = (dh(0xD58 + 2 * Idx3) - 0x600u) / 40, v4 = (dh(0xD58 + 2 * Idx4) - 0x600u) / 40;
		if (v1 >= GFX_MAX_VTX || v2 >= GFX_MAX_VTX || v3 >= GFX_MAX_VTX || v4 >= GFX_MAX_VTX)
			return;
		const float k = naboo_k();
		g->vtx[v1].s = 0.0f;
		g->vtx[v1].t = (float)(int16_t)(p1 & 0xFFFF) * k;
		g->vtx[v2].s = (float)(int16_t)(p1 >> 16) * k;
		g->vtx[v2].t = 0.0f;
		g->vtx[v3].s = 0.0f;
		g->vtx[v3].t = 0.0f;
		g->vtx[v4].s = g->vtx[v2].s;
		g->vtx[v4].t = g->vtx[v1].t;
		naboo_run_polygons(g, 3);
	} else {
		naboo_run_polygons(g, 5);
	}
}

static __attribute__((noinline)) void naboo_copy_dataFF(struct gfx_state *g, uint32_t ca)
{
	const uint8_t *s = gfx_io_read(g, GFX_IO_SMALL, ca, 16);
	if (!s)
		return;
	uint32_t p[4];
	for (unsigned i = 0; i < 4; i++)
		p[i] = salsa64_rd32(s + 4 * i);
	dw_set(0x100, 0);
#define DD(k) (0xDB8 + 4 * (k))
	dw_set(DD(0), dw(DD(0)) & 0xFFFF);
	dw_set(DD(1), (p[0] & 0xFFFF) << 16 | (dw(DD(1)) & 0xFFFF));
	uint32_t V = (p[0] >> 16) & 0xFF;
	dw_set(DD(2), V << 24 | 0x00FFFF);
	dw_set(DD(3), V * 2);
	dw_set(DD(4), V << 24);
	V = (p[1] >> 24) & 0xFF;
	dw_set(DD(5), V);
	dw_set(DD(7), (dw(DD(7)) & 0xFFFF0000u) | V * 2);
	V = p[2] >> 16;
	dw_set(DD(10), (dw(DD(10)) & 0xFFFF0000u) | V);
	dw_set(DD(12), (dw(DD(12)) & 0xFFFF0000u) | V);
	V = p[2] & 0xFFFF;
	dw_set(DD(14), (dw(DD(14)) & 0xFFFF0000u) | V);
	dw_set(DD(16), (dw(DD(16)) & 0xFFFF0000u) | V);
	V = p[3] >> 16;
	dw_set(DD(18), (dw(DD(18)) & 0xFFFF0000u) | V);
	dw_set(DD(20), (dw(DD(20)) & 0xFFFF0000u) | V);
	V = p[3] & 0xFFFF;
	dw_set(DD(22), (dw(DD(22)) & 0xFFFF0000u) | V);
	dw_set(DD(24), (dw(DD(24)) & 0xFFFF0000u) | V);
	V = (p[1] >> 8) & 0xFFFF;
	dw_set(DD(27), (dw(DD(27)) & 0xFFFF0000u) | V);
	dw_set(DD(29), (dw(DD(29)) & 0xFFFF0000u) | V);
#undef DD
}

static __attribute__((noinline)) void naboo_copy_data00(struct gfx_state *g, uint32_t ca)
{
	const uint8_t *s = gfx_io_read(g, GFX_IO_SMALL, ca, 64);
	if (!s)
		return;
	uint32_t p[16];
	for (unsigned i = 0; i < 16; i++)
		p[i] = salsa64_rd32(s + 4 * i);
	dw_set(0xF8, p[3]);
	uint32_t V = (p[0] >> 16) & 0xFF;
	dw_set(0xFC, V << 24 | (p[10] & 0xFF) << 16);
#define DD(k) (0xDD0 + 4 * (k))
	dw_set(DD(0), (p[2] & 0xFFFF0000u) | (p[14] >> 16));
	dw_set(DD(1), (p[2] & 0xFFFF) << 16 | (dw(DD(1)) & 0xFFFF));
	dw_set(DD(2), (p[2] & 0xFFFF0000u) | (p[14] & 0xFFFF));
	dw_set(DD(3), (p[2] & 0xFFFF) << 16 | 0x0100);
	V = p[4] & 0xFFFF0000u;
	dw_set(DD(4), V | (dw(DD(4)) & 0xFFFF));
	dw_set(DD(6), V | (dw(DD(6)) & 0xFFFF));
	V = (p[4] & 0xFFFF) << 16;
	dw_set(DD(8), V | (dw(DD(8)) & 0xFFFF));
	dw_set(DD(10), V | (dw(DD(10)) & 0xFFFF));
	V = p[5] & 0xFFFF0000u;
	dw_set(DD(12), V | (dw(DD(12)) & 0xFFFF));
	dw_set(DD(14), V | (dw(DD(14)) & 0xFFFF));
	V = (p[5] & 0xFFFF) << 16;
	dw_set(DD(16), V | (dw(DD(16)) & 0xFFFF));
	dw_set(DD(18), V | (dw(DD(18)) & 0xFFFF));
	V = (p[6] & 0xFFFF0000u) | (p[8] >> 16);
	dw_set(DD(5), V);
	dw_set(DD(7), V);
	V = (p[6] & 0xFFFF) << 16 | (p[8] & 0xFFFF);
	dw_set(DD(9), V);
	dw_set(DD(11), V);
	V = (p[7] & 0xFFFF0000u) | (p[9] >> 16);
	dw_set(DD(13), V);
	dw_set(DD(15), V);
	V = (p[7] & 0xFFFF) << 16 | (p[9] & 0xFFFF);
	dw_set(DD(17), V);
	dw_set(DD(19), V);
	V = (p[0] & 0xFFFF) << 16 | (p[13] >> 16);
	dw_set(DD(20), V);
	dw_set(DD(21), (p[15] & 0xFFFF0000u) | (dw(DD(21)) & 0xFFFF));
	uint32_t d22 = p[11] & 0xFF;
	if (d22 > 127)
		d22 |= 0xFF00;
	V = ((p[0] & 0xFFFF) + 0x00FF) & 0xFF00;
	dw_set(DD(22), d22 | V << 16);
	dw_set(DD(23), (p[15] & 0xFFFF) << 16 | (dw(DD(23)) & 0xFFFF));
	dw_set(DD(24), 0x7FFF7FFFu);
	dw_set(DD(25), p[12]);
	dw_set(DD(26), 0x7FFF7FFFu);
	dw_set(DD(27), p[12]);
	dw_set(DD(28), p[10]);
	dw_set(DD(29), p[10]);
	dw_set(DD(30), p[11]);
	dw_set(DD(31), p[11]);
#undef DD
	/* Paso 5: 0xF0 bytes de la RDRAM a 0xC68. */
	{
		const uint8_t *c = gfx_io_read(g, GFX_IO_SMALL, (p[1] >> 8) & 0x00FFFFFF, 0xF0);
		if (c)
			memcpy(DM + 0xC68, c, 0xF0);
	}
	DM[0x37E] = 0xFF;
	static const uint32_t table[14] = { 0x06000628, 0x06500678, 0x06A006C8, 0x06F00718, 0x07400768,
					    0x079007B8, 0x07E00808, 0x08300858, 0x088008A8, 0x08D008F8,
					    0x09200948, 0x09700998, 0x09C009E8, 0x0A100A38 };
	for (unsigned i = 0; i < 14; i++)
		dw_set(0xD58 + 4 * i, table[i]);
	dw_set(0xD90, 0x0A600000u | (dw(0xD90) & 0xFFFF));
}

/* 0x15: rectángulo de textura de un vértice. p: 8 palabras desde ca. */
static __attribute__((noinline)) void naboo_texrect_gen(struct gfx_state *g, uint32_t ca)
{
	const uint8_t *s = gfx_io_read(g, GFX_IO_SMALL, ca, 32);
	if (!s)
		return;
	uint32_t p[8];
	for (unsigned i = 0; i < 8; i++)
		p[i] = salsa64_rd32(s + 4 * i);
	const uint32_t vi = ((p[0] >> 5) & 0x07F8) / 40;
	if (vi >= GFX_MAX_VTX)
		return;
	const struct gfx_vtx *v = &g->vtx[vi];
	if (v->clip & (GFX_CLIP_XN | GFX_CLIP_XP | GFX_CLIP_YN | GFX_CLIP_YP | GFX_CLIP_W))
		return;
	const bool flip = (p[0] & 1) != 0;
	const float screen_x = v->sx, screen_y = v->sy;
	const uint32_t w_i = v->w >= 1.0f ? (uint32_t)v->w : 1u;
	const uint32_t vpw = dw(0x130);
	const uint32_t viewport_x = (vpw >> 17) & 0x7FFF, viewport_y = (vpw >> 1) & 0x7FFF;
	const uint32_t persp_x = (p[5] & 0xFFFF0000u) | (p[6] >> 16);
	const uint32_t persp_y = (p[5] << 16) | (p[6] & 0xFFFF);
	uint32_t p3x = p[3] >> 16, p3y = p[3] & 0xFFFF;
	if (flip) {
		uint32_t x = p3x;
		p3x = p3y;
		p3y = x;
	}
	const float wf = (float)w_i * 65536.0f;
	uint32_t off_x = ftou((float)(p3x * viewport_x) * (float)persp_x / wf);
	uint32_t off_y = ftou((float)(p3y * viewport_y) * (float)persp_y / wf);
	const float off_xf = (float)off_x * 0.25f, off_yf = (float)off_y * 0.25f;
	const float ulx = screen_x - off_xf, lrx = screen_x + off_xf;
	if (lrx - ulx <= 0.0f)
		return;
	const float uly = screen_y - off_yf, lry = screen_y + off_yf;
	if (lry - uly <= 0.0f)
		return;
	uint32_t p4x = p[4] & 0xFFFF0000u, p4y = p[4] << 16;
	const float *m = g->mvp;
	float fx = m[0] < 0 ? -m[0] : m[0], fy = m[1] < 0 ? -m[1] : m[1];
	fx -= (float)(int32_t)fx;
	fy -= (float)(int32_t)fy;
	p4x |= (uint32_t)(fx * 65536.0f);
	p4y |= (uint32_t)(fy * 65536.0f);
	if (flip) {
		uint32_t x = off_x;
		off_x = off_y;
		off_y = x;
	}
	if (off_x == 0 || off_y == 0)
		return;
	uint16_t dsdx_i = (uint16_t)(ftou((float)p4x / (float)off_x) >> 10);
	uint16_t dtdy_i = (uint16_t)(ftou((float)p4y / (float)off_y) >> 10);
	uint16_t E = 0, F = 0;
	if (p[0] & 2) {
		dsdx_i = (uint16_t)-dsdx_i;
		E = (uint16_t)(p[4] >> 16);
	}
	if (p[0] & 4) {
		dtdy_i = (uint16_t)-dtdy_i;
		F = (uint16_t)p[4];
	}
	uint16_t rdsdx = dsdx_i, rdtdy = dtdy_i;
	if (flip) {
		uint16_t x = rdsdx;
		rdsdx = rdtdy;
		rdtdy = x;
	}
	uint16_t S, T;
	if (ulx > 0.0f) {
		S = (uint16_t)(E + 0xFFF0);
	} else {
		const int ulx_i = (int)(ulx * 4.0f);
		S = (uint16_t)(((uint32_t)(0 - (int)((uint32_t)rdsdx << 6) * ulx_i) << 3) + E + 0xFFF0u);
	}
	if (uly > 0.0f) {
		T = (uint16_t)(F + 0xFFF0);
	} else {
		const int uly_i = (int)(uly * 4.0f);
		T = (uint16_t)(((uint32_t)(0 - (int)((uint32_t)rdtdy << 6) * uly_i) << 3) + F + 0xFFF0u);
	}
	do_sub_dlist(g);
	set_prim_depth_color(g, v, p[7]);
	if (g->geom & G_FOG)
		gfx_rdp_command(g, 0xF8000000u, (p[7] & 0xFFFFFF00u) | v->c[3]);
	emit_texrect(g, ulx, uly, lrx, lry, g->tex_tile, S, T, dsdx_i, dtdy_i, flip);
}

/* Estado de las dos mitades de vértices que se generan a la vez. */
struct nvd {
	uint16_t x, y, z, flag;
	uint32_t E;
	uint16_t K, L, M, N, J;
	uint8_t c1[4], c2[4];  /* r, g, b, a */
};

struct nctx {
	uint32_t p[6];
	uint32_t src, dst1, dst2;  /* dmemSrcAddr y las direcciones de destino de las dos mitades */
	uint32_t G, DD6, I, E24, E34, E40, E48;
	int32_t K, L, M, N, O, P, Q, R;
	struct nvd vd1, vd2;
};

static int32_t mult_and_clip(int32_t s, int32_t t, int32_t max)
{
	if (((uint32_t)s & 0x80000000u) != ((uint32_t)t & 0x80000000u))
		return 0;
	int64_t r = ((int64_t)s * (int64_t)t) >> 32;
	return (int32_t)(r < (int64_t)max ? r : (int64_t)max);
}

static void nvd_step2(struct nvd *vd, uint32_t V, uint32_t pw1, uint32_t C2)
{
	vd->x = (uint16_t)(((V >> 24) & 0xFF) << 8);
	vd->z = (uint16_t)(((V >> 8) & 0xFF) << 8);
	vd->flag = (uint16_t)((V & 0xFF) << 8);
	const uint32_t B = (uint32_t)((int32_t)(int8_t)(V >> 8) * 0x10000);
	const int32_t D = (int32_t)(B - (pw1 >> 16));
	vd->E = C2 + (uint32_t)(((int64_t)D * D) >> 16);
}

static void nvd_step3(const struct nctx *c, struct nvd *vd)
{
	vd->K = (uint16_t)mult_and_clip(c->K - (int32_t)vd->E, c->O, 0x7FFF);
	vd->L = (uint16_t)mult_and_clip(c->L - (int32_t)vd->E, c->P, 0x7FFF);
	vd->M = (uint16_t)mult_and_clip(c->M - (int32_t)vd->E, c->Q, (int32_t)(c->E34 >> 16));
	vd->N = (uint16_t)mult_and_clip(c->N - (int32_t)vd->E, c->R, (int32_t)(c->E34 & 0xFFFF));
	uint64_t J = ((uint64_t)vd->E * c->I) >> 16;
	J *= J;
	uint16_t j = (uint16_t)(J >> 47);
	vd->J = j < 0x7FFF ? j : 0x7FFF;
}

/* Pasos 1 a 3 (iguales en 0x09 y 0x0C): las DMA a la DMEM y las constantes. */
static bool nctx_begin(struct gfx_state *g, uint32_t ca, bool mode0c, struct nctx *c)
{
	const uint8_t *s = gfx_io_read(g, GFX_IO_SMALL, ca, 24);
	if (!s)
		return false;
	for (unsigned i = 0; i < 6; i++)
		c->p[i] = salsa64_rd32(s + 4 * i);
	const uint32_t *p = c->p;
	if (mode0c)
		copy_in(g, 0x380, dw(0xF8), 480);
	c->src = p[0] & 0xFFF;
	const uint32_t dmemDstAddr = dw(c->src) >> 16;
	const uint32_t rdramAddr = p[4] & 0x00FFFFFF;
	copy_in(g, dmemDstAddr, rdramAddr, 40);
	const uint32_t dmemDstAddr2 = dw(c->src + 8) >> 16;
	const uint32_t rdramShift = dw(0xDBC) >> 16;
	copy_in(g, dmemDstAddr2, rdramAddr + rdramShift, 40);
	if (((p[0] >> 12) & 0xF) > 7) {
		const uint32_t ra2 = p[5] & 0x00FFFFFF;
		copy_in(g, dmemDstAddr + 0x78, ra2, 40);
		copy_in(g, dmemDstAddr2 + 0x78, ra2 + rdramShift, 40);
	}
	c->dst1 = dw(c->src + 0x10) >> 16;
	c->dst2 = dw(c->src + 0x18) >> 16;
	const uint32_t A = (uint32_t)((int32_t)(int8_t)(p[1] >> 24) * 0x10000);
	const uint32_t C32 = A - (dw(0xDD0) >> 16);
	const int64_t C64 = (int64_t)(int32_t)C32;
	const uint32_t C2 = (uint32_t)((C64 * C64) >> 16);
	c->G = C32 * (dw(0xDC4) & 0xFFFF) + ((dw(0xDCC) & 0xFFFF) << 16);
	memset(&c->vd1, 0, sizeof c->vd1);
	memset(&c->vd2, 0, sizeof c->vd2);
	nvd_step2(&c->vd1, p[1] + (dw(0xDC0) >> 16), dw(0xDD4), C2);
	nvd_step2(&c->vd2, p[1], dw(0xDDC), C2);
	c->E24 = dw(0xE24);
	c->I = (dw(0xE2C) & 0xFFFF0000u) | (c->E24 >> 16);
	const uint32_t DE0 = dw(0xDE0), DE4 = dw(0xDE4), DF0 = dw(0xDF0), DF4 = dw(0xDF4);
	c->K = (int32_t)((DF0 & 0xFFFF0000u) | (DE0 >> 16));
	c->L = (int32_t)((DF0 & 0xFFFF) << 16 | (DE0 & 0xFFFF));
	c->M = (int32_t)((DF4 & 0xFFFF0000u) | (DE4 >> 16));
	c->N = (int32_t)((DF4 & 0xFFFF) << 16 | (DE4 & 0xFFFF));
	const uint32_t E00 = dw(0xE00), E04 = dw(0xE04), E10 = dw(0xE10), E14 = dw(0xE14);
	c->O = (int32_t)((E10 & 0xFFFF0000u) | (E00 >> 16));
	c->P = (int32_t)((E10 & 0xFFFF) << 16 | (E00 & 0xFFFF));
	c->Q = (int32_t)((E14 & 0xFFFF0000u) | (E04 >> 16));
	c->R = (int32_t)((E14 & 0xFFFF) << 16 | (E04 & 0xFFFF));
	c->E34 = dw(0xE34);
	c->E40 = dw(0xE40);
	c->E48 = dw(0xE48);
	c->DD6 = (dw(0xDD4) & 0xFFFF) << 16;
	return true;
}

static uint8_t dmn(uint32_t a)
{
	return DM[a & 0xFFF];
}

static uint8_t mix3(uint32_t factor, uint32_t base, uint32_t M, uint32_t e40, uint32_t N, uint32_t e48)
{
	return (uint8_t)((factor * base + M * e40 + N * e48) >> 16);
}

static void put_colors(uint32_t a, const struct nvd *vd)
{
	dw_set(a + 0xF0, (uint32_t)vd->c1[0] << 24 | (uint32_t)vd->c1[1] << 16 | (uint32_t)vd->c1[2] << 8 | vd->c1[3]);
	dw_set(a + 0x168, (uint32_t)vd->c2[0] << 24 | (uint32_t)vd->c2[1] << 16 | (uint32_t)vd->c2[2] << 8 | vd->c2[3]);
}

static void put_vertices(struct nctx *c, uint32_t addrShift)
{
	const uint32_t o1 = 0x170 + addrShift * 2, o2 = 0x1C0 + addrShift * 2;
	dh_set(o1, c->vd1.x);
	dh_set(o1 + 2, c->vd1.y);
	dh_set(o1 + 4, c->vd1.z);
	dh_set(o1 + 6, c->vd1.flag);
	dh_set(o2, c->vd2.x);
	dh_set(o2 + 2, c->vd2.y);
	dh_set(o2 + 4, c->vd2.z);
	dh_set(o2 + 6, c->vd2.flag);
}

static void nctx_advance(struct nctx *c)
{
	const uint32_t V0 = dw(0xDC0), V1 = dw(0xDC4), V3 = dw(0xDCC);
	c->vd1.x = (uint16_t)(c->vd1.x + (V0 >> 16));
	c->vd1.flag = (uint16_t)(c->vd1.flag + (V1 & 0xFFFF));
	c->vd2.x = (uint16_t)(c->vd2.x + (V0 >> 16));
	c->vd2.flag = (uint16_t)(c->vd2.flag + (V3 & 0xFFFF));
	c->vd1.E += c->G;
	c->vd2.E += c->G;
	c->G += c->DD6;
}

/* Pasos finales: los vértices a sus lugares (con el desplazamiento de 0x128, y
 * GLideN64 suma Z0 a x) y el recorrido de la malla. */
static void nctx_finish(struct gfx_state *g, struct nctx *c)
{
	const bool adjust = (dh(0x128) | dh(0x12A) | dh(0x12C)) != 0;
	for (unsigned h = 0; h < 2; h++) {
		const uint32_t param = h ? (c->p[2] & 0xFFFF) : (c->p[2] >> 16);
		const uint32_t numVtx = param & 0x1F;
		if (numVtx == 0)
			continue;
		const uint32_t offset = param >> 5;
		const uint32_t srcAddr = (h ? 0x1C0 : 0x170) + offset;
		const uint32_t dstAddr = (h ? c->dst2 : c->dst1) + offset * 5;
		const uint32_t v0 = (dstAddr - 0x600u) / 40;
		for (uint32_t i = 0; i < numVtx; i++) {
			const uint16_t dx = adjust ? (uint16_t)(dh(0x128) + dh(0x12C)) : 0, dy = adjust ? dh(0x12A) : 0;
			load_dm_vertex(g, v0 + i, srcAddr + 8 * i, dx, dy, 0);
		}
	}
	/* params16[6 ^ 1] y params16[7 ^ 1]: las medias palabras naturales 6 y 7; params8[1 ^ 3] y
	 * params8[5 ^ 3]: los bytes naturales 1 y 5. */
	f5.AA = (uint16_t)(c->p[3] >> 16);
	f5.BB = (int16_t)(c->p[3] & 0xFFFF);
	f5.CC = (uint8_t)(c->p[0] >> 16);
	f5.TT = (uint8_t)(c->p[1] >> 16);
	f5.EE = (int16_t)c->src;
	f5.HH = (int16_t)(dw(0xDB8) >> 16);
	f5.DD = 0;
}

static __attribute__((noinline)) void naboo_gen_vertices0c(struct gfx_state *g, uint32_t ca)
{
	static struct nctx c;  /* en el bss: no pesa en la pila */
	if (!nctx_begin(g, ca, true, &c))
		return;
	const uint32_t E20 = dw(0xE20), E28 = dw(0xE28), DD0 = dw(0xDD0), DD8 = dw(0xDD8);
	const uint8_t RR = DM[0x0FC], HH = DM[0x0FD];
	const uint32_t startAddr = c.p[4] >> 24, endAddr = c.p[5] >> 24;
	for (uint32_t addrShift = startAddr; addrShift <= endAddr && addrShift < 0x100; addrShift += 4) {
		nvd_step3(&c, &c.vd1);
		nvd_step3(&c, &c.vd2);
		struct nvd *vds[2] = { &c.vd1, &c.vd2 };
		const uint32_t addr[2] = { (dw(c.src) >> 16) + addrShift, (dw(c.src + 8) >> 16) + addrShift };
		for (unsigned h = 0; h < 2; h++) {
			struct nvd *vd = vds[h];
			const uint32_t a = addr[h];
			const uint32_t factor = 0xFFFE - vd->M - vd->N;
			const uint32_t U1 = dw(a);
			vd->c1[0] = mix3(factor, U1 >> 24, vd->M, c.E40 >> 24, vd->N, c.E48 >> 24);
			vd->c1[1] = mix3(factor, (U1 >> 16) & 0xFF, vd->M, (c.E40 >> 16) & 0xFF, vd->N, (c.E48 >> 16) & 0xFF);
			vd->c1[2] = mix3(factor, (U1 >> 8) & 0xFF, vd->M, (c.E40 >> 8) & 0xFF, vd->N, (c.E48 >> 8) & 0xFF);
			const uint32_t U2 = dw(a + 0x78);
			const uint8_t u1 = (uint8_t)((U1 & 0xFF) + RR), u2 = (uint8_t)((U2 & 0xFF) + HH);
			const uint32_t u3 = (uint32_t)(int32_t)(int8_t)dmn(0x380 + u1), u4 = (uint32_t)(int32_t)(int8_t)dmn(0x380 + u2);
			const uint32_t L1 = (E28 & 0xFFFF) << 16 | (E20 & 0xFFFF);
			const uint32_t L2 = (DD8 & 0xFFFF) << 16 | (DD0 & 0xFFFF);
			const uint16_t u5 = (uint16_t)((u3 * L1 + u4 * L2) >> 16);
			const uint16_t u6 = (uint16_t)((u5 + (E20 >> 16)) & 0xFFFF);
			const uint16_t u7 = (uint16_t)(((0x7FFFu - vd->K) * u6 + (uint32_t)vd->K * (E28 >> 16)) >> 16);
			vd->y = (uint16_t)(((0x7FFFu - vd->J) * u7 + (uint32_t)vd->J * (c.E24 & 0xFFFF)) >> 16);
			const uint8_t A1 = dmn(0x4FF + u4);
			const uint32_t f1 = 0xFF - A1, f2 = 0xFF * A1;
			const uint8_t Va = (uint8_t)((f1 * (U2 >> 24) + f2) >> 8);
			const uint8_t Vb = (uint8_t)((f1 * ((U2 >> 16) & 0xFF) + f2) >> 8);
			const uint8_t Vc = (uint8_t)((f1 * ((U2 >> 8) & 0xFF) + f2) >> 8);
			vd->c2[0] = mix3(factor, Va, vd->M, c.E40 >> 24, vd->N, c.E48 >> 24);
			vd->c2[1] = mix3(factor, Vb, vd->M, (c.E40 >> 16) & 0xFF, vd->N, (c.E48 >> 16) & 0xFF);
			vd->c2[2] = mix3(factor, Vc, vd->M, (c.E40 >> 8) & 0xFF, vd->N, (c.E48 >> 8) & 0xFF);
			const uint32_t f3 = 0x7FFFu - vd->K;
			for (unsigned k = 0; k < 3; k++)
				vd->c2[k] = (uint8_t)((((f3 * vd->c2[k] + (uint32_t)vd->K * vd->c1[k]) << 1) >> 16) + 1);
			vd->c1[3] = (uint8_t)(((uint32_t)vd->K * vd->K) >> 22);
			vd->c2[3] = (uint8_t)(((((0x7FFFu - vd->L) * A1 + (uint32_t)vd->L * 0xFF) << 1) >> 16) + 1);
			put_colors(a, vd);
		}
		put_vertices(&c, addrShift);
		nctx_advance(&c);
	}
	nctx_finish(g, &c);
}

static uint32_t avg_bytes(uint32_t a, uint32_t b)
{
	uint32_t r = 0;
	for (unsigned i = 0; i < 32; i += 8)
		r |= ((((a >> i) & 0xFF) + ((b >> i) & 0xFF)) >> 1) << i;
	return r;
}

/* Colores y la y de una mitad de 0x09: mezcla con los dos vecinos. */
static void naboo09_set(const struct nctx *c, const struct nvd *vd, uint32_t addr, uint32_t addr3, uint32_t addr4,
			uint8_t col[4], uint16_t *Y)
{
	const uint32_t factor1 = 0x7FFFu - vd->L, factor2 = 0xFFFEu - vd->M - vd->N;
	const uint32_t U = dw(addr), T = avg_bytes(dw(addr3), dw(addr4));
	uint64_t V[3];
	for (unsigned k = 0; k < 3; k++) {
		const unsigned sh = 24 - 8 * k;
		V[k] = ((uint64_t)factor1 * ((U >> sh) & 0xFF) + (uint64_t)vd->L * ((T >> sh) & 0xFF)) * factor2 >> 16;
	}
	const uint32_t TA = ((dw(addr3) & 0xFF) << 7) + ((dw(addr4) & 0xFF) << 7);
	const uint32_t Vy1 = (factor1 * ((U & 0xFF) << 8)) >> 16, Vy2 = ((uint32_t)vd->L * TA) >> 16;
	const uint32_t Vy = Vy1 + Vy2;
	col[0] = (uint8_t)(((V[0] << 1) + (uint64_t)vd->M * (c->E40 >> 24) + (uint64_t)vd->N * (c->E48 >> 24)) >> 16);
	col[1] = (uint8_t)(((V[1] << 1) + (uint64_t)vd->M * ((c->E40 >> 16) & 0xFF) + (uint64_t)vd->N * ((c->E48 >> 16) & 0xFF)) >> 16);
	col[2] = (uint8_t)(((V[2] << 1) + (uint64_t)vd->M * ((c->E40 >> 8) & 0xFF) + (uint64_t)vd->N * ((c->E48 >> 8) & 0xFF)) >> 16);
	*Y = (uint16_t)(((uint64_t)Vy * (0x7FFFu - vd->J) + (uint64_t)vd->J * (c->E24 & 0xFFFF)) >> 16);
}

static __attribute__((noinline)) void naboo_gen_vertices09(struct gfx_state *g, uint32_t ca)
{
	static struct nctx c;  /* en el bss: no pesa en la pila */
	if (!nctx_begin(g, ca, false, &c))
		return;
	const uint32_t startAddr = c.p[4] >> 24, endAddr = c.p[5] >> 24;
	for (uint32_t addrShift = startAddr; addrShift <= endAddr && addrShift < 0x100; addrShift += 4) {
		nvd_step3(&c, &c.vd1);
		nvd_step3(&c, &c.vd2);
		struct nvd *vds[2] = { &c.vd1, &c.vd2 };
		const uint32_t addr[2] = { (dw(c.src) >> 16) + addrShift, (dw(c.src + 8) >> 16) + addrShift };
		const uint32_t Vw[2] = { dw(c.src + 0x20), dw(c.src + 0x24) };
		for (unsigned h = 0; h < 2; h++) {
			struct nvd *vd = vds[h];
			const uint32_t addr3 = (Vw[h] >> 16) + addrShift + (addrShift & 4);
			const uint32_t addr4 = (Vw[h] & 0xFFFF) + addrShift - (addrShift & 4);
			uint16_t Y1, Y2;
			naboo09_set(&c, vd, addr[h], addr3, addr4, vd->c1, &Y1);
			naboo09_set(&c, vd, addr[h] + 0x78, addr3 + 0x78, addr4 + 0x78, vd->c2, &Y2);
			const uint32_t f3 = 0x7FFFu - vd->K;
			for (unsigned k = 0; k < 3; k++)
				vd->c2[k] = (uint8_t)((((f3 * vd->c2[k] + (uint32_t)vd->K * vd->c1[k]) << 1) >> 16) + 1);
			vd->y = (uint16_t)(((f3 * Y2 + (uint32_t)vd->K * Y1) << 1) >> 16);
			vd->c1[3] = vd->c2[3] = (uint8_t)(((uint32_t)vd->K * vd->K) >> 22);
			put_colors(addr[h], vd);
		}
		put_vertices(&c, addrShift);
		nctx_advance(&c);
	}
	nctx_finish(g, &c);
}

#endif /* F5I_NABOO */

/* ---- Despacho ---------------------------------------------------------- */

static void cmd_geometry_gen(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	const uint32_t ca = g->dl_pc - 8;
	switch (w1 & 0xFF) {
	case 0x18:
		gen_vertices(g, w0, w1);
		break;
	case 0x24:
		do_sub_dlist(g);
		texrect_gen_particles(g, ca);
		g->dl_pc = ca + 40;
		break;
	case 0x27:
		gen_particle_vertices(g, ca);
		g->dl_pc = ca + 24;
		break;
#ifdef F5I_NABOO
	case 0x09:
		naboo_gen_vertices09(g, ca);
		naboo_run_polygons(g, 5);  /* fuera de lo anterior: no suma su marco a la pila */
		g->dl_pc = ca + 24;
		break;
	case 0x0C:
		naboo_gen_vertices0c(g, ca);
		naboo_run_polygons(g, 5);
		g->dl_pc = ca + 24;
		break;
	case 0x0F:
		if (DM[0x37E] == 0) {
			naboo_copy_data00(g, ca);
			g->dl_pc = ca + 64;
		} else {
			naboo_copy_dataFF(g, ca);
			g->dl_pc = ca + 16;
		}
		break;
	case 0x15:
		naboo_texrect_gen(g, ca);
		g->dl_pc = ca + 32;
		break;
	case 0x4F:
		DM[0x37E] = 0;
		break;
#endif
	default:
		break;
	}
}

/* Un comando de la lista principal. false: la tarea terminó. */
static bool f5i_cmd(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	const uint32_t ca = g->dl_pc - 8;  /* dirección del comando: sus datos siguen */
	const uint32_t op = w0 >> 24;
	if (state_cmd(g, w0, w1))
		return true;
	switch (op) {
	case 0x01:
		cmd_movemem(g, w0, w1);
		break;
	case 0x02:
		cmd_set_dlist_addr(g, w0, w1);
		break;
	case 0x05:
		cmd_geometry_gen(g, w0, w1);
		break;
	case 0x06:
		if (g->dl_sp + 1 >= (int)F5I_DL_DEPTH || !gfx_dl_call(g, w1, false)) {
			g->res->dl_errors++;
			return false;
		}
		list_entry(g);
		break;
	case 0x07:
		gfx_dl_call(g, w1, true);
		list_entry(g);
		break;
	case 0xB4:
	case 0xBF:
		cmd_tri(g, w0, w1, ca, op == 0xB4);
		break;
	case 0xB5:
		g->dl_pc = f5.dl[g->dl_sp];
		list_entry(g);
		break;
	case 0xB8:
		return gfx_dl_end(g);
#ifdef F5I_NABOO
	case 0xBE:
		naboo_draw_polygons(g, ca);
		break;
#endif
	case 0xE4:
	case 0xE5: {
		do_sub_dlist(g);
		uint32_t w2, w3;
		if (!gfx_io_dl(g, g->dl_pc, &w2, &w3))
			break;
		g->dl_pc += 8;
		gfx_texrect(g, w0, w1, w2, w3, op == 0xE5);
		break;
	}
	default:
		g->res->unknown_cmds++;
		g->res->last_unknown = op;
		break;
	}
	return true;
}

void gfx_dl_f5indy(struct gfx_state *g)
{
	memset(&g->rare, 0, sizeof g->rare);
	g->clip_ratio[0] = g->clip_ratio[1] = 1.0f;
	/* El microcódigo pone en cero estas palabras al empezar cada tarea. */
	static const uint16_t clear[7] = { 0x11C, 0x120, 0x124, 0x37C, 0x58C, 0x5B0, 0x5B4 };
	for (unsigned i = 0; i < 7; i++)
		dw_set(clear[i], 0);
	f5.insub = false;
	list_entry(g);
	uint32_t w0, w1;
	/* Una lista que entra en memoria en ceros (aún sin escribir, o ya borrada) no acaba
	 * nunca: cada palabra en cero es un G_SPNOOP y la tarea recorre la RDRAM entera
	 * (1 M de comandos, ~75 ms en el SPE) antes de cortarse. 8192 seguidas (64 KB de
	 * ceros) no son una lista: se corta ya, como un bucle, para que el juego reciba
	 * su interrupción del DP y el PPE anote la lista (log_bad_task). */
	unsigned nops = 0;
	while (gfx_dl_next(g, &w0, &w1)) {
		if (w0 == 0 && w1 == 0) {
			if (++nops >= 8192) {
				g->res->flags |= GFX_RES_LOOP;
				g->res->dl_errors++;
				break;
			}
			continue;
		}
		nops = 0;
		if (!f5i_cmd(g, w0, w1))
			break;
	}
}
