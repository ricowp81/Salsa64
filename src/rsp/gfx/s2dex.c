/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — tramo S2DEX2 dentro de una tarea de la familia F3DEX2: Zelda OoT
 * carga S2DEX con G_LOAD_UCODE para copiar el fondo de una sala
 * prerrenderizada (Room_DrawBackground2D: G_BG_COPY de una imagen RGBA16 de
 * 320 × 240) y vuelve a F3DZEX. Antes el tramo se saltaba entero (dl.c) y se
 * veía la malla de profundidad de la sala en colores planos.
 *
 * Se reconoce por el texto de los datos del microcódigo ("RSP Gfx[Safe] S2DEX
 * fifo 2.05" en los bytes que carga el G_LOAD_UCODE). Del tramo se hace lo que comparte
 * con F3DEX2 (G_DL, G_ENDDL, G_MOVEWORD de segmentos, G_SETOTHERMODE_*,
 * G_RDPHALF_*, comandos del RDP) y los fondos:
 *
 * - G_BG_COPY (uObjBg): la imagen, desde (imageX, imageY) y repitiéndose en
 *   imageW × imageH, se copia sin escala al marco (frameX, frameY, frameW,
 *   frameH) en modo copia. Como el microcódigo: tiras de filas cargadas a la
 *   TMEM con LOADTILE (las que caben en 4 KB, 2 KB con paleta) y un
 *   TEXRECT por tira (dos si la tira da la vuelta en x). Todo pasa por
 *   rdp.c: las tiras son texturas normales (caché por contenido de la TMEM).
 * - G_BG_1CYC (uObjScaleBg): igual en 1 ciclo con escala (scaleW/scaleH en
 *   u5.10: texels por píxel); con G_OBJRM_BILERP cada tira lleva una fila
 *   más para el filtro.
 * - G_OBJ_RENDERMODE: se guarda (bilineal del fondo escalado).
 * - G_OBJ_LOADTXTR (uObjTxtr): carga a la TMEM un bloque, una tile o una
 *   paleta, como LOADBLOCK, LOADTILE o LOADTLUT con la tile 7. Kirby 64
 *   carga así la paleta de sus fondos CI8 (G_BG_1CYC); sin ella el fondo
 *   salía con la paleta que hubiera quedado en la TMEM. El microcódigo se
 *   salta la carga si la palabra de estado `sid` ya trae `flag` (bajo
 *   `mask`): es solo un ahorro, aquí se carga siempre.
 * - G_RDPHALF_0 (0xE4) seguido de G_RDPHALF_1 y G_RDPHALF_2: un TEXRECT,
 *   como en F3DEX2.
 * - Objetos (G_OBJ_RECTANGLE, G_OBJ_SPRITE, G_OBJ_MOVEMEM con uObjMtx y
 *   uObjSubMtx, G_OBJ_RECTANGLE_R, G_OBJ_LDTX_SPRITE/RECT/RECT_R): ver abajo.
 *
 * También las tareas S2DEX sueltas (gfx_dl_s2dex: «RSP Gfx ucode S2DEX fifo
 * 2.06», Resident Evil 2, y «S2DEX 1.06», Yoshi's Story): la lista es la de
 * S2DEX, con los opcodes de 2.x o, para 1.x, los de la primera generación
 * (s2_canon los traduce y el resto es el mismo código). Una lista de esas
 * tareas carga el microcódigo de 3D con G_LOAD_UCODE a mitad de tarea (RE2
 * F3DEX 2.06, Yoshi's Story F3DEX 1.23) y sigue en él, y de él vuelve a S2DEX
 * por el mismo camino: el tramo, como el de Zelda OoT, y los de F3DEX2 y F3D
 * llaman aquí. Se midió contra el microcódigo real (--lle=all): BG_COPY de RE2
 * y una pantalla entera de sprites de Yoshi's Story salen con los mismos
 * píxeles; lo que difiere es el 3D (docs/estado.md).
 *
 * Los sprites de G_OBJ_SPRITE los dibuja el microcódigo con dos triángulos
 * con textura, no con TEXRECT: aquí se arma el mismo cuadrilátero
 * (gfx_rdp_tex_quad) y sale idéntico. Los de G_OBJ_RECTANGLE y
 * G_OBJ_RECTANGLE_R, con un TEXRECT; esos no se pudieron medir (ningún juego
 * de los respaldos los usa).
 *
 * Campos de uObjBg/uObjScaleBg y su punto fijo (u10.5, u10.2, s10.2;
 * escala u5.10 = texels por píxel): GLideN64 `src/uCodes/S2DEX.cpp`
 * (consultado el 2026-10-11, solo el formato; docs/THIRD_PARTY.md); los
 * objetos, de mupen64plus-video-rice y -glide64mk2 (GPLv2+, solo el formato
 * y las fórmulas, mismo documento). Opcodes
 * de S2DEX2 (los de la familia F3DEX2): G_BG_COPY = 0x0A como en la lista
 * de Zelda OoT, G_OBJ_LOADTXTR = 0x05 como en la de Kirby 64. uObjTxtr y
 * sus unidades (palabras de 64 bits de la TMEM, tamaños menos uno): la
 * misma fuente, solo el formato. Código propio.
 */
#include <string.h>

#include "core/endian.h"
#include "rsp/gfx/hle.h"

/* Opcodes de S2DEX 2.x (los de la familia F3DEX2). Los de S2DEX 1.x se
 * traducen a estos (s2_canon) y el resto del código es el mismo. */
enum {
	S2_OBJ_RECTANGLE = 0x01, S2_OBJ_SPRITE = 0x02, S2_SELECT_DL = 0x04, S2_OBJ_LOADTXTR = 0x05,
	S2_OBJ_LDTX_SPRITE = 0x06, S2_OBJ_LDTX_RECT = 0x07, S2_OBJ_LDTX_RECT_R = 0x08, S2_BG_1CYC = 0x09,
	S2_BG_COPY = 0x0A, S2_OBJ_RENDERMODE = 0x0B,
	S2_OBJ_RECTANGLE_R = 0xDA, S2_MOVEWORD = 0xDB, S2_OBJ_MOVEMEM = 0xDC, S2_LOAD_UCODE = 0xDD, S2_DL = 0xDE,
	S2_ENDDL = 0xDF,
	S2_RDPHALF_1 = 0xE1, S2_SETOTHERMODE_L = 0xE2, S2_SETOTHERMODE_H = 0xE3, S2_RDPHALF_0 = 0xE4,
	S2_RDPHALF_2 = 0xF1,
};

#if GFX_HAS(S2DEX)
/* El opcode de S2DEX 1.x (Yoshi's Story: BG_1CYC 0x01, BG_COPY 0x02,
 * OBJ_RECTANGLE 0x03, OBJ_SPRITE 0x04, OBJ_MOVEMEM 0x05, SELECT_DL 0xB0,
 * OBJ_RENDERMODE 0xB1, OBJ_RECTANGLE_R 0xB2, LOADTXTR 0xC1 y LDTX_* 0xC2 a
 * 0xC4, y los de F3D que comparte: G_DL 0x06, G_ENDDL 0xB8, G_RDPHALF_1
 * 0xB4, G_RDPHALF_2 0xB3, G_SETOTHERMODE_L/H 0xB9/0xBA, G_MOVEWORD 0xBC,
 * G_LOAD_UCODE 0xAF)
 * como el de S2DEX 2.x. Los comandos del RDP (0xE4 y 0xE6 en adelante) no
 * cambian. */
static uint32_t s2_canon(uint32_t op)
{
	switch (op) {
	case 0x01: return S2_BG_1CYC;
	case 0x02: return S2_BG_COPY;
	case 0x03: return S2_OBJ_RECTANGLE;
	case 0x04: return S2_OBJ_SPRITE;
	case 0x05: return S2_OBJ_MOVEMEM;
	case 0x06: return S2_DL;
	case 0xB0: return S2_SELECT_DL;
	case 0xB1: return S2_OBJ_RENDERMODE;
	case 0xB2: return S2_OBJ_RECTANGLE_R;
	case 0xB3: return S2_RDPHALF_2;
	case 0xB4: return S2_RDPHALF_1;
	case 0xB8: return S2_ENDDL;
	case 0xAF: return S2_LOAD_UCODE;
	case 0xB9: return S2_SETOTHERMODE_L;
	case 0xBA: return S2_SETOTHERMODE_H;
	case 0xBC: return S2_MOVEWORD;
	case 0xC1: return S2_OBJ_LOADTXTR;
	case 0xC2: return S2_OBJ_LDTX_SPRITE;
	case 0xC3: return S2_OBJ_LDTX_RECT;
	case 0xC4: return S2_OBJ_LDTX_RECT_R;
	default: return op;
	}
}
#endif

#define OBJRM_BILERP 0x08u
#define G_MW_SEGMENT 0x06u
#define BG_TILE      0       /* tile con que se dibuja */
#define BG_LOAD_TILE 7       /* G_TX_LOADTILE */

/* uObjTxtr.type */
#define OBJLT_TXTRBLOCK 0x00001033u
#define OBJLT_TXTRTILE  0x00FC1034u
#define OBJLT_TLUT      0x00000030u

/* Después de "RSP Gfx" viene el nombre, precedido de "ucode " en la mayoría
 * ("RSP Gfx ucode F3DEX.NoN fifo 1.22") o de una marca entre corchetes pegada
 * a "Gfx" (el S2DEX de Zelda OoT: "RSP Gfx[Safe] S2DEX fifo 2.05"): se saltan
 * los espacios, las marcas "[…]" y "ucode". Devuelve dónde empieza el nombre. */
static uint32_t ucode_name_at(const uint8_t *p, uint32_t size, uint32_t i)
{
	for (;;) {
		while (i < size && p[i] == ' ')
			i++;
		if (i < size && p[i] == '[') {
			uint32_t j = i + 1;
			while (j < size && p[j] > ' ' && p[j] < 0x7F && p[j] != ']')
				j++;
			if (j >= size || p[j] != ']')
				return i;
			i = j + 1;
		} else if (i + 6 <= size && memcmp(p + i, "ucode ", 6) == 0) {
			i += 5;
		} else {
			return i;
		}
	}
}

int gfx_ucode_kind(struct gfx_state *g, uint32_t data, uint32_t size, bool *nonear)
{
	*nonear = false;
	if (size == 0 || size > GFX_IO_MAX)
		size = GFX_IO_MAX;
	const uint8_t *p = gfx_io_read(g, GFX_IO_TEX, data, size);
	uint32_t i = 0;
	/* Como salsa64_rsp_detect_gbi: "RSP Gfx ucode <nombre> fifo|xbus N.xx". */
	if (!p)
		return 0;
	while (i + 24 < size && memcmp(p + i, "RSP Gfx", 7))
		i++;
	if (i + 24 >= size)
		return 0;
	i = ucode_name_at(p, size, i + 7);
	bool s2dex = i + 5 <= size && memcmp(p + i, "S2DEX", 5) == 0;
	if (i + 24 >= size || !(s2dex || memcmp(p + i, "F3D", 3) == 0) || memcmp(p + i, "F3DEXBG", 7) == 0)
		return 0;
	for (; i + 4 < size && p[i] > ' ' && p[i] < 0x7F; i++)
		if (memcmp(p + i, ".NoN", 4) == 0)
			*nonear = true;
	/* La versión: el primer "N." después del nombre. */
	for (; i + 1 < size && p[i] >= 0x20 && p[i] < 0x7F; i++)
		if (p[i] >= '0' && p[i] <= '9' && p[i + 1] == '.') {
			int gen = p[i] == '2' ? 2 : p[i] <= '1' ? 1 : 0;
			return s2dex ? (gen == 2 ? GFX_UCODE_S2DEX2 : gen == 1 ? GFX_UCODE_S2DEX1 : 0) : gen;
		}
	return 0;
}

struct bg {
	int32_t frame_x, frame_y;     /* píxeles (s10.2 → entero) */
	uint32_t frame_w, frame_h;    /* píxeles */
	uint32_t image_x, image_y;    /* texels (u10.5 → entero) */
	uint32_t image_w, image_h;    /* texels */
	uint32_t ptr;                 /* dirección segmentada de la imagen */
	unsigned fmt, siz, pal;
	uint32_t scale_w, scale_h;    /* u5.10 (1:1 = 1024) */
};

static bool read_bg(struct gfx_state *g, uint32_t seg, bool scaled, struct bg *b)
{
	const uint8_t *p = gfx_io_read(g, GFX_IO_SMALL, gfx_addr(g, seg), 40);
	if (!p) {
		g->res->dl_errors++;
		return false;
	}
	b->image_x = salsa64_rd16(p + 0) >> 5;
	b->image_w = salsa64_rd16(p + 2) >> 2;
	b->frame_x = (int16_t)salsa64_rd16(p + 4) >> 2;
	b->frame_w = salsa64_rd16(p + 6) >> 2;
	b->image_y = salsa64_rd16(p + 8) >> 5;
	b->image_h = salsa64_rd16(p + 10) >> 2;
	b->frame_y = (int16_t)salsa64_rd16(p + 12) >> 2;
	b->frame_h = salsa64_rd16(p + 14) >> 2;
	b->ptr = salsa64_rd32(p + 16);
	b->fmt = p[22] & 7;
	b->siz = p[23] & 3;
	b->pal = salsa64_rd16(p + 24) & 15;
	b->scale_w = scaled ? salsa64_rd16(p + 28) : 1024;
	b->scale_h = scaled ? salsa64_rd16(p + 30) : 1024;
	if (!b->scale_w || !b->scale_h)
		return false;
	return b->image_w && b->image_h && b->frame_w && b->frame_h && b->image_w <= 1024 && b->image_h <= 1024;
}

/* Bytes de una fila en la TMEM (múltiplo de 8): en 32 bits, por mitad. */
static uint32_t row_bytes(uint32_t width, unsigned siz)
{
	uint32_t b = siz == G_IM_SIZ_4b ? (width + 1) / 2 : siz == G_IM_SIZ_8b ? width : 2 * width;
	return (b + 7) & ~7u;
}

/* Paso entre filas de la imagen en la RDRAM, en texels de la carga (lsiz):
 * el microcódigo lo saca de imageW en palabras de 64 bits, redondeando hacia
 * abajo (imageW >> (6 − siz) en u10.2; lo mismo que guS2DInitBg deja en
 * tmemSizeW). Worms Armageddon declara imageW = 321 con filas de 320 bytes
 * (CI8): el microcódigo real pide SETTIMG de 160 texels de 16 bits y lee 330
 * bytes desde cada fila, así que la columna 321 es la primera de la fila
 * siguiente. Con el paso de 321 cada fila se corría un texel más que la
 * anterior y la imagen entera salía inclinada (#57).
 * GFX_JOB_S2DEX_BG_ROW_OLD vuelve al paso de imageW texels. */
static uint32_t bg_stride(const struct gfx_state *g, const struct bg *b, unsigned lsiz, uint32_t lw)
{
	if (g->job && (g->job->flags & GFX_JOB_S2DEX_BG_ROW_OLD))
		return lw;
	uint32_t bytes = (b->siz == G_IM_SIZ_4b ? b->image_w / 2 : b->image_w << (b->siz - 1)) & ~7u;
	uint32_t texels = bytes >> (lsiz - 1);
	return texels ? texels : lw;
}

/* Carga las filas [ty, ty + n) de la imagen a la TMEM y deja la tile de
 * dibujo apuntándolas (s de 0 a image_w − 1, t de ty a ty + n − 1). */
static void load_rows(struct gfx_state *g, const struct bg *b, uint32_t ty, uint32_t n)
{
	/* LOADTILE no carga 4 bits: se cargan como bytes, la mitad de ancho. */
	unsigned lsiz = b->siz == G_IM_SIZ_4b ? G_IM_SIZ_8b : b->siz;
	uint32_t lw = b->siz == G_IM_SIZ_4b ? (b->image_w + 1) / 2 : b->image_w;
	uint32_t line = row_bytes(b->image_w, b->siz) / 8;
	gfx_rdp_command(g, 0xFDu << 24 | b->fmt << 21 | lsiz << 19 | (bg_stride(g, b, lsiz, lw) - 1), b->ptr);
	gfx_rdp_command(g, 0xF5u << 24 | b->fmt << 21 | lsiz << 19 | line << 9, (uint32_t)BG_LOAD_TILE << 24);
	gfx_rdp_command(g, 0xF4u << 24 | (ty << 2), (uint32_t)BG_LOAD_TILE << 24 | (lw - 1) << 14 | (ty + n - 1) << 2);
	/* Dibujo: límites sin repetir (G_TX_CLAMP en s y t). */
	gfx_rdp_command(g, 0xF5u << 24 | b->fmt << 21 | b->siz << 19 | line << 9, (uint32_t)BG_TILE << 24 | b->pal << 20 |
								2u << 18 | 2u << 8);
	gfx_rdp_command(g, 0xF2u << 24 | (ty << 2), (uint32_t)BG_TILE << 24 | (b->image_w - 1) << 14 | (ty + n - 1) << 2);
}

/* TEXRECT de (x0, y0) a (x1, y1) en cuartos de píxel (borde inferior
 * derecho exclusivo) desde el texel (s, t), con dsdx/dtdy en s5.10. */
static void strip_rect(struct gfx_state *g, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t s,
		       uint32_t t, uint32_t dsdx, uint32_t dtdy, bool copy)
{
	if (x1 <= x0 || y1 <= y0)
		return;
	/* En modo copia los bordes son inclusivos y dsdx va ×4. */
	if (copy) {
		x1 -= 4;
		y1 -= 4;
		dsdx *= 4;
	}
	if (x1 > 0xFFF)
		x1 = 0xFFF;
	if (y1 > 0xFFF)
		y1 = 0xFFF;
	gfx_texrect(g, 0xE4u << 24 | x1 << 12 | y1, (uint32_t)BG_TILE << 24 | x0 << 12 | y0, s << 16 | (t & 0xFFFF),
		    (dsdx & 0xFFFF) << 16 | (dtdy & 0xFFFF), false);
}

static void bg_draw(struct gfx_state *g, uint32_t seg, bool scaled)
{
	struct bg b;
	if (!read_bg(g, seg, scaled, &b))
		return;
	/* BG_1CYC limita un marco mayor que la imagen escalada: el módulo
	 * siguiente solo sirve para el origen desplazado, no para dibujar
	 * copias completas de más. El RSP reserva el borde final en cuartos
	 * de píxel. BG_COPY sí conserva el marco de la copia sin esta cota.
	 * Los marcos que ya caben mantienen su recorrido y redondeo. */
	if (scaled && !(g->job->flags & GFX_JOB_S2DEX_BG_OLD)) {
		if (b.frame_w * b.scale_w > (b.image_w << 10)) {
			uint32_t q = (b.image_w << 12) / b.scale_w;
			b.frame_w = q ? (q - 1) >> 2 : 0;
		}
		if (b.frame_h * b.scale_h > (b.image_h << 10)) {
			uint32_t q = (b.image_h << 12) / b.scale_h;
			b.frame_h = q ? (q - 1) >> 2 : 0;
		}
	}
	unsigned cycle = (g->oh >> G_MDSFT_CYCLETYPE) & 3;
	bool copy = cycle == G_CYC_COPY;
	/* Con paleta, la mitad alta de la TMEM es de la paleta. */
	bool tlut = ((g->oh >> G_MDSFT_TEXTLUT) & 3) >= G_TT_RGBA16 && b.siz <= G_IM_SIZ_8b;
	uint32_t cap = (tlut || b.siz == G_IM_SIZ_32b) ? 2048 : 4096;
	uint32_t per_load = cap / row_bytes(b.image_w, b.siz);
	bool bilerp = scaled && (g->obj_rendermode & OBJRM_BILERP);
	if (per_load < (bilerp ? 2u : 1u))
		return;  /* una fila no cabe en la TMEM */

	/* Marco recortado a la izquierda y arriba de la pantalla: se corre la
	 * imagen lo que se recorta. Todo en texels ×1024 (u5.10) para la
	 * escala, en 32 bits: imageX < 2048, frameX ≥ −8192 y frameW < 16 384
	 * píxeles, la escala < 65 536 y la imagen ≤ 1024 texels, así que nada
	 * pasa de 2^31. En 64 bits, el SPE 0 traía 5,3 KB de libgcc (__muldi3,
	 * __moddi3, __udivdi3, __umoddi3) solo para esto. */
	uint32_t sx0 = b.image_x << 10, ty0 = b.image_y << 10;
	uint32_t fx = 0, fy = 0;
	if (b.frame_x < 0) {
		fx = (uint32_t)-b.frame_x;
		sx0 += fx * b.scale_w;
	}
	if (b.frame_y < 0) {
		fy = (uint32_t)-b.frame_y;
		ty0 += fy * b.scale_h;
	}
	if (fx >= b.frame_w || fy >= b.frame_h)
		return;
	uint32_t x_left = (uint32_t)(b.frame_x + (int32_t)fx), y = (uint32_t)(b.frame_y + (int32_t)fy);
	uint32_t y_end = (uint32_t)(b.frame_y + (int32_t)b.frame_h);
	uint32_t x_end = (uint32_t)(b.frame_x + (int32_t)b.frame_w);
	uint32_t wrap_w = b.image_w << 10, wrap_h = b.image_h << 10;
	uint32_t t = ty0 % wrap_h;

	while (y < y_end) {
		/* Filas de imagen de esta tira: desde la de t, hasta per_load (una
		 * menos con bilineal, que lee la siguiente) o el fin de la imagen. */
		uint32_t ty = (uint32_t)(t >> 10);
		uint32_t n = per_load - (bilerp ? 1 : 0);
		if (n > b.image_h - ty)
			n = b.image_h - ty;
		/* Píxeles de pantalla que caen en esas filas. */
		uint32_t t_end = (ty + n) << 10;
		uint32_t rows = (uint32_t)((t_end - t + b.scale_h - 1) / b.scale_h);
		if (rows == 0)
			rows = 1;
		if (rows > y_end - y)
			rows = y_end - y;
		uint32_t load_n = n + (bilerp && ty + n < b.image_h ? 1 : 0);
		load_rows(g, &b, ty, load_n);

		/* En x, tramos hasta la vuelta de la imagen. */
		uint32_t s = sx0 % wrap_w;
		uint32_t x = x_left;
		while (x < x_end) {
			uint32_t cols = (uint32_t)((wrap_w - s + b.scale_w - 1) / b.scale_w);
			if (cols > x_end - x)
				cols = x_end - x;
			strip_rect(g, x << 2, y << 2, (x + cols) << 2, (y + rows) << 2, (uint32_t)(s >> 5),
				   (uint32_t)(t >> 5), b.scale_w, b.scale_h, copy);
			x += cols;
			s = (s + cols * b.scale_w) % wrap_w;
		}
		y += rows;
		t = (t + rows * b.scale_h) % wrap_h;
	}
}

/* uObjTxtr (24 bytes): type, image (segmentada), cuatro campos de 16 bits
 * que dependen del tipo, flag y mask. Todo con 8 bits por texel salvo la
 * paleta: la TMEM no sabe del formato, solo de bytes. */
static void obj_load_txtr(struct gfx_state *g, uint32_t seg)
{
	const uint8_t *p = gfx_io_read(g, GFX_IO_SMALL, gfx_addr(g, seg), 24);
	if (!p) {
		g->res->dl_errors++;
		return;
	}
	uint32_t type = salsa64_rd32(p), image = salsa64_rd32(p + 4);
	uint32_t a = salsa64_rd16(p + 8), b = salsa64_rd16(p + 10), c = salsa64_rd16(p + 12);
	const uint32_t load_tile = (uint32_t)BG_LOAD_TILE << 24;
	switch (type) {
	case OBJLT_TXTRBLOCK: {
		/* a = tmem, b = tsize (palabras − 1), c = tline (dxt). */
		uint32_t lrs = ((b + 1) << 3) - 1;
		gfx_rdp_command(g, 0xFDu << 24 | G_IM_SIZ_8b << 19, image);
		gfx_rdp_command(g, 0xF5u << 24 | G_IM_SIZ_8b << 19 | (a & 0x1FF), load_tile);
		gfx_rdp_command(g, 0xF3u << 24, load_tile | (lrs > 0xFFF ? 0xFFF : lrs) << 12 | (c & 0xFFF));
		break;
	}
	case OBJLT_TXTRTILE: {
		/* a = tmem, b = twidth (unidades de 16 bits por fila − 1), c =
		 * theight (cuartos de fila − 1). */
		uint32_t w = (b + 1) << 1, rows = (c + 1) >> 2;
		if (!rows || w > 0x400)
			break;
		gfx_rdp_command(g, 0xFDu << 24 | G_IM_SIZ_8b << 19 | (w - 1), image);
		gfx_rdp_command(g, 0xF5u << 24 | G_IM_SIZ_8b << 19 | ((b + 1) >> 2) << 9 | (a & 0x1FF), load_tile);
		gfx_rdp_command(g, 0xF4u << 24, load_tile | ((w - 1) << 2) << 12 | ((rows - 1) << 2));
		break;
	}
	case OBJLT_TLUT:
		/* a = phead (palabra de la TMEM, desde 256), b = pnum (entradas − 1). */
		gfx_rdp_command(g, 0xFDu << 24 | G_IM_SIZ_16b << 19, image);
		gfx_rdp_command(g, 0xF5u << 24 | G_IM_SIZ_16b << 19 | (a & 0x1FF), load_tile);
		gfx_rdp_command(g, 0xF0u << 24, load_tile | ((b & 0xFF) << 2) << 12);
		break;
	default:
		g->res->skipped++;
		break;
	}
}

#if GFX_HAS(S2DEX)
/* --- Objetos (G_OBJ_*) --- */

/* La matriz de objetos (uObjMtx / uObjSubMtx) vive en g->raw[4..11], libre
 * en este tramo (G_RDPHALF_0 usa raw[0..1]): A, B, C, D en s15.16, X e Y en
 * s10.2 y BaseScaleX/Y en u5.10. */
#define MTX(g) ((g)->raw + 4)
enum { M_A, M_B, M_C, M_D, M_X, M_Y, M_BSX, M_BSY };

/* uObjSprite (24 bytes). */
struct obj {
	int32_t x, y;                 /* s10.2 */
	uint32_t scale_w, scale_h;    /* u5.10: texels por píxel */
	uint32_t w, h;                /* u10.5 */
	unsigned stride, adrs, fmt, siz, pal, flags;
};

static bool read_obj(struct gfx_state *g, uint32_t ptr, struct obj *o)
{
	const uint8_t *p = gfx_io_read(g, GFX_IO_SMALL, gfx_addr(g, ptr), 24);
	if (!p) {
		g->res->dl_errors++;
		return false;
	}
	o->x = (int16_t)salsa64_rd16(p + 0);
	o->scale_w = salsa64_rd16(p + 2);
	o->w = salsa64_rd16(p + 4);
	o->y = (int16_t)salsa64_rd16(p + 8);
	o->scale_h = salsa64_rd16(p + 10);
	o->h = salsa64_rd16(p + 12);
	o->stride = salsa64_rd16(p + 16);
	o->adrs = salsa64_rd16(p + 18);
	o->fmt = p[20] & 7;
	o->siz = p[21] & 3;
	o->pal = p[22] & 15;
	o->flags = p[23];
	return o->scale_w && o->scale_h && o->w && o->h && o->w < 0x8000 && o->h < 0x8000;
}

static void mtx_reset(struct gfx_state *g)
{
	uint32_t *m = MTX(g);
	m[M_A] = m[M_D] = 0x10000;
	m[M_B] = m[M_C] = m[M_X] = m[M_Y] = 0;
	m[M_BSX] = m[M_BSY] = 1024;
}

/* G_OBJ_MOVEMEM: con (w0 & 0xFFFF) = 0 la matriz entera (24 bytes), con 2
 * la submatriz (X, Y y escala base; 8 bytes). */
static void obj_movemem(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	unsigned kind = w0 & 0xFFFF;
	if (kind != 0 && kind != 2) {
		g->res->unknown_cmds++;
		GFX_UNKNOWN(0xBFu);
		return;
	}
	const uint8_t *p = gfx_io_read(g, GFX_IO_SMALL, gfx_addr(g, w1), kind ? 8 : 24);
	if (!p) {
		g->res->dl_errors++;
		return;
	}
	uint32_t *m = MTX(g);
	if (!kind) {
		for (int i = 0; i < 4; i++)
			m[M_A + i] = salsa64_rd32(p + 4 * i);
		p += 16;
	}
	m[M_X] = (uint32_t)(int32_t)(int16_t)salsa64_rd16(p);
	m[M_Y] = (uint32_t)(int32_t)(int16_t)salsa64_rd16(p + 2);
	m[M_BSX] = salsa64_rd16(p + 4);
	m[M_BSY] = salsa64_rd16(p + 6);
}

/* La tile 0 sobre la imagen del sprite en la TMEM (cargada con
 * G_OBJ_LOADTXTR): sin repetir, de 0 a imageW − 1 por imageH − 1. */
static void obj_tile(struct gfx_state *g, const struct obj *o)
{
	uint32_t w = (o->w >> 5) ? (o->w >> 5) : 1, h = (o->h >> 5) ? (o->h >> 5) : 1;
	gfx_rdp_command(g, 0xF5u << 24 | o->fmt << 21 | o->siz << 19 | (o->stride & 0x1FF) << 9 | (o->adrs & 0x1FF),
			(uint32_t)BG_TILE << 24 | o->pal << 20 | 2u << 18 | 2u << 8);
	gfx_rdp_command(g, 0xF2u << 24, (uint32_t)BG_TILE << 24 | (w - 1) << 14 | (h - 1) << 2);
}

/* Un sprite sin rotar en el rectángulo de pantalla [x0, x1) × [y0, y1) en
 * cuartos de píxel (pueden venir al revés, con la imagen volteada, y salirse
 * de la pantalla por arriba o la izquierda); dsdx/dtdy en u5.10 (texels por
 * píxel, positivos). Los flags de volteo del sprite (bit 0 = s, bit 4 = t)
 * también invierten el recorrido. En coma flotante y enteros de 32 bits: el
 * SPE no lleva la división de 64 bits (libgcc, 5 KB). */
static void obj_rect(struct gfx_state *g, const struct obj *o, float x0, float y0, float x1, float y1, float dsdx,
		     float dtdy)
{
	bool fx = o->flags & 1, fy = o->flags & 0x10;
	if (x1 < x0) {
		float t = x0;
		x0 = x1;
		x1 = t;
		fx = !fx;
	}
	if (y1 < y0) {
		float t = y0;
		y0 = y1;
		y1 = t;
		fy = !fy;
	}
	/* s y t del borde izquierdo y del superior, con el signo del recorrido. */
	float s = fx ? (float)(((o->w >> 5) - 1) << 5) : 0.0f, t = fy ? (float)(((o->h >> 5) - 1) << 5) : 0.0f;
	if (fx)
		dsdx = -dsdx;
	if (fy)
		dtdy = -dtdy;
	/* TEXRECT no tiene coordenadas negativas: se recorta y se avanza s y t
	 * (cuarto de píxel = dsdx / 4 texels, en u10.5 ×32: dsdx / 128). */
	if (x0 < 0.0f) {
		s += -x0 * dsdx / 128.0f;
		x0 = 0.0f;
	}
	if (y0 < 0.0f) {
		t += -y0 * dtdy / 128.0f;
		y0 = 0.0f;
	}
	if (x1 <= x0 || y1 <= y0 || x0 >= 4096.0f || y0 >= 4096.0f)
		return;
	bool copy = ((g->oh >> G_MDSFT_CYCLETYPE) & 3) == G_CYC_COPY;
	strip_rect(g, (uint32_t)x0, (uint32_t)y0, (uint32_t)x1, (uint32_t)y1, (uint32_t)(int32_t)s, (uint32_t)(int32_t)t,
		   (uint32_t)(int32_t)dsdx, (uint32_t)(int32_t)dtdy, copy);
}

/* G_OBJ_RECTANGLE (base = false) y G_OBJ_RECTANGLE_R (con la escala base y
 * el desplazamiento X, Y de la matriz): un TEXRECT. El ancho en cuartos de
 * píxel es imageW / 32 texels entre (scaleW / 1024 texels por píxel) × 4. */
static void obj_rectangle(struct gfx_state *g, const struct obj *o, bool base)
{
	const uint32_t *m = MTX(g);
	float bsx = base ? (float)m[M_BSX] : 1024.0f, bsy = base ? (float)m[M_BSY] : 1024.0f;
	if (bsx == 0.0f || bsy == 0.0f)
		return;
	float qw = (float)o->w * 128.0f * 1024.0f / ((float)o->scale_w * bsx);
	float qh = (float)o->h * 128.0f * 1024.0f / ((float)o->scale_h * bsy);
	float qx = base ? (float)(int32_t)m[M_X] + (float)o->x * 1024.0f / bsx : (float)o->x;
	float qy = base ? (float)(int32_t)m[M_Y] + (float)o->y * 1024.0f / bsy : (float)o->y;
	float dsdx = (float)o->scale_w, dtdy = (float)o->scale_h;
	if (base) {
		dsdx = dsdx * bsx / 1024.0f;
		dtdy = dtdy * bsy / 1024.0f;
	}
	obj_tile(g, o);
	obj_rect(g, o, qx, qy, qx + qw, qy + qh, dsdx, dtdy);
}

/* G_OBJ_SPRITE: el microcódigo lleva las cuatro esquinas del sprite por la
 * matriz (x' = A x + B y + X, y' = C x + D y + Y) y lo dibuja con dos
 * triángulos con textura, no con un TEXRECT: las esquinas son las inclusivas
 * (el ancho y el alto menos un píxel, antes de la matriz) y s, t van de 0 a
 * ese tamaño en texels (con G_OBJ_FLIP al revés). Aquí se arma el mismo
 * cuadrilátero y sale por el mismo camino que los triángulos del RDP
 * (gfx_rdp_tex_quad): con el LLE, los mismos píxeles (docs/estado.md). */
static void obj_sprite(struct gfx_state *g, const struct obj *o)
{
	const uint32_t *m = MTX(g);
	float x[4], y[4], s[4], t[4];
	float fa = (float)(int32_t)m[M_A] / 65536.0f, fb = (float)(int32_t)m[M_B] / 65536.0f;
	float fc = (float)(int32_t)m[M_C] / 65536.0f, fd = (float)(int32_t)m[M_D] / 65536.0f;
	float w = (float)o->w * 32.0f / (float)o->scale_w - 1.0f, h = (float)o->h * 32.0f / (float)o->scale_h - 1.0f;
	if (w < 0.0f)
		w = 0.0f;
	if (h < 0.0f)
		h = 0.0f;
	float tw = w * (float)o->scale_w / 1024.0f, th = h * (float)o->scale_h / 1024.0f;
	for (int i = 0; i < 4; i++) {
		float u = (i == 1 || i == 2) ? 1.0f : 0.0f, v = (i >= 2) ? 1.0f : 0.0f;
		float px = (float)o->x / 4.0f + u * w, py = (float)o->y / 4.0f + v * h;
		x[i] = fa * px + fb * py + (float)(int32_t)m[M_X] / 4.0f;
		y[i] = fc * px + fd * py + (float)(int32_t)m[M_Y] / 4.0f;
		s[i] = ((o->flags & 1) ? 1.0f - u : u) * tw;
		t[i] = ((o->flags & 0x10) ? 1.0f - v : v) * th;
	}
	obj_tile(g, o);
	gfx_rdp_tex_quad(g, BG_TILE, x, y, s, t);
}

/* G_OBJ_LDTX_*: uObjTxSprite = uObjTxtr (24 bytes) + uObjSprite (24). */
static void obj_ldtx(struct gfx_state *g, uint32_t seg, uint32_t op)
{
	struct obj o;
	obj_load_txtr(g, seg);
	if (!read_obj(g, seg + 24, &o))
		return;
	if (op == S2_OBJ_LDTX_SPRITE)
		obj_sprite(g, &o);
	else
		obj_rectangle(g, &o, op == S2_OBJ_LDTX_RECT_R);
}

static void obj_cmd(struct gfx_state *g, uint32_t op, uint32_t w1)
{
	struct obj o;
	if (!read_obj(g, w1, &o))
		return;
	if (op == S2_OBJ_SPRITE)
		obj_sprite(g, &o);
	else
		obj_rectangle(g, &o, op == S2_OBJ_RECTANGLE_R);
}
#endif /* GFX_HAS(S2DEX) */

/* Un tramo S2DEX: gen1 con los opcodes de S2DEX 1.x. `foreign`: está dentro
 * de una tarea de otra familia (G_LOAD_UCODE) y termina en el G_LOAD_UCODE
 * que vuelve; si no, es la tarea entera y termina con el G_ENDDL de la
 * lista. */
static bool s2dex_run(struct gfx_state *g, bool gen1, bool foreign)
{
	struct gfx_result *res = g->res;
	uint32_t w0, w1;
	bool nonear;
	if (foreign)
		res->flags |= GFX_RES_LOAD_UCODE;
	g->raw[0] = 0;
#if GFX_HAS(S2DEX)
	mtx_reset(g);
#endif
	while (gfx_dl_next(g, &w0, &w1)) {
#if GFX_HAS(S2DEX)
		uint32_t op = gen1 ? s2_canon(w0 >> 24) : w0 >> 24;
#else
		uint32_t op = w0 >> 24;
#endif
		switch (op) {
		case S2_LOAD_UCODE: {
#if GFX_HAS(S2DEX)
			uint32_t text = gfx_addr(g, w1);
			if (g->ucode && text == g->ucode) {
				if (foreign)
					return true;
				break;
			}
			int kind = gfx_ucode_kind(g, gfx_addr(g, g->rdphalf1), (w0 & 0xFFFF) + 1, &nonear);
			if (kind == GFX_UCODE_S2DEX2 || kind == GFX_UCODE_S2DEX1) {
				if (!foreign)
					g->ucode = text;
				break;
			}
			if (!foreign && ((kind == 1 && GFX_HAS(F3D)) || (kind == 2 && GFX_HAS(F3DEX2)))) {
				/* RE2 (F3DEX2) y Yoshi's Story (F3DEX 1.x) cargan el de 3D a mitad
				 * de la tarea y siguen con la lista: la sigue su intérprete. */
				gfx_ucode_reload(g, text, kind, nonear, gfx_addr(g, g->rdphalf1), (w0 & 0xFFFF) + 1);
#if GFX_HAS(F3DEX2) && GFX_HAS(S2DEX)
				/* En la imagen sin F3D esta rama solo admite kind == 2. */
				uint8_t chain = (g->job->flags & GFX_JOB_AFFINE_CHAIN) ? GFX_CHAIN_AFFINE : 0;
#if GFX_HAS(F3D)
				if (kind == 1)
					chain = 1;
#endif
#else
				uint8_t chain = kind == 1;
#endif
				g->chain_ex = chain;
				return true;
			}
			return gfx_dl_skip_foreign(g, gen1 ? 0xAF : S2_LOAD_UCODE, gen1 ? 0x06 : S2_DL, gen1 ? 0xB8 : S2_ENDDL);
#else
			if (g->ucode && gfx_addr(g, w1) == g->ucode)
				return true;
			/* Otro más: S2DEX otra vez sigue aquí; si no, se salta. */
			if (gfx_ucode_kind(g, gfx_addr(g, g->rdphalf1), (w0 & 0xFFFF) + 1, &nonear) != GFX_UCODE_S2DEX2)
				return gfx_dl_skip_foreign(g, S2_LOAD_UCODE, S2_DL, S2_ENDDL);
			break;
#endif
		}
		case S2_DL:
			if (!gfx_dl_call(g, w1, (w0 >> 16) & 1))
				return false;
			break;
		case S2_ENDDL:
			if (!gfx_dl_end(g)) {
				/* Con G_RDPFULLSYNC ya visto, acabar aquí es lo normal (Yoshi's
				 * Story termina sus listas dentro del tramo S2DEX); sin él, la
				 * tarea cuenta como cortada y el DP interrumpe igual. */
				if (foreign && !(res->flags & GFX_RES_FULLSYNC))
					res->flags |= GFX_RES_FOREIGN_END;
				return false;
			}
			break;
		case S2_MOVEWORD: {
			unsigned type = gen1 ? w0 & 0xFF : (w0 >> 16) & 0xFF;
			unsigned off = gen1 ? (w0 >> 8) & 0xFFFF : w0 & 0xFFFF;
			if (type == G_MW_SEGMENT)
				g->segments[(off >> 2) & 15] = w1 & 0x00FFFFFF;
			break;
		}
		case S2_RDPHALF_1:
			g->rdphalf1 = w1;
			break;
		case S2_RDPHALF_2:
			g->rdphalf2 = w1;
			if (g->raw[0] >> 24 == S2_RDPHALF_0) {
				gfx_texrect(g, g->raw[0], g->raw[1], g->rdphalf1, w1, false);
				g->raw[0] = 0;
			}
			break;
		case S2_SETOTHERMODE_L:
		case S2_SETOTHERMODE_H: {
			int len = (int)(w0 & 0xFF) + (!gen1 || (g->job->flags & GFX_JOB_S2DEX_OLD_MODES) != 0);
			int shift = gen1 ? (int)((w0 >> 8) & 0xFF) : 32 - (int)((w0 >> 8) & 0xFF) - len;
			gfx_sp_othermode(g, op == S2_SETOTHERMODE_H, shift, len, w1);
			break;
		}
		case S2_BG_COPY:
		case S2_BG_1CYC:
			bg_draw(g, w1, op == S2_BG_1CYC);
			res->bg++;
			break;
		case S2_OBJ_RENDERMODE:
			g->obj_rendermode = w1;
			break;
		case S2_OBJ_LOADTXTR:
			obj_load_txtr(g, w1);
			break;
#if GFX_HAS(S2DEX)
		case S2_OBJ_RECTANGLE:
		case S2_OBJ_SPRITE:
		case S2_OBJ_RECTANGLE_R:
			obj_cmd(g, op, w1);
			res->bg++;
			break;
		case S2_OBJ_LDTX_SPRITE:
		case S2_OBJ_LDTX_RECT:
		case S2_OBJ_LDTX_RECT_R:
			obj_ldtx(g, w1, op);
			res->bg++;
			break;
		case S2_OBJ_MOVEMEM:
			obj_movemem(g, w0, w1);
			break;
#endif
		case S2_RDPHALF_0:
			/* Seguido de G_RDPHALF_1 y G_RDPHALF_2 es un TEXRECT (el
			 * cielo de Kirby 64): se dibuja con G_RDPHALF_2. Queda en
			 * raw[], libre en este tramo, y no en locales ni pasando por
			 * gfx_dl_texrect: este marco está en la cadena más honda de la
			 * pila del SPE. */
			g->raw[0] = w0;
			g->raw[1] = w1;
			break;
		default:
			/* Comandos del RDP; lo demás de S2DEX (G_SELECT_DL) no se
			 * implementa. */
			if (op >= 0xE6) {
				gfx_rdp_command(g, w0, w1);
			} else {
				res->unknown_cmds++;
				res->last_unknown = (uint8_t)(w0 >> 24);
				GFX_UNKNOWN(w0 >> 24);
			}
			break;
		}
	}
	return false;
}

bool gfx_dl_s2dex2(struct gfx_state *g)
{
	return s2dex_run(g, false, true);
}

#if GFX_HAS(S2DEX)
bool gfx_dl_s2dex1(struct gfx_state *g)
{
	return s2dex_run(g, true, true);
}

int gfx_dl_s2dex(struct gfx_state *g, bool gen1)
{
	if (!s2dex_run(g, gen1, false))
		return GFX_S2_END;
#if GFX_HAS(F3DEX2)
	return g->chain_ex == 1 ? GFX_S2_F3DEX : GFX_S2_F3DEX2;
#else
	return g->chain_ex ? GFX_S2_F3DEX : GFX_S2_F3DEX2;
#endif
}
#endif
