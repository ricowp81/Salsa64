/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — HLE gráfica: el estado que persiste entre tareas (el del RDP y
 * la TMEM) para los estados guardados. Ver GFX_STATE_* en gfx.h.
 *
 * Formato portable (big-endian, el mismo en el host, el PPE y el SPE):
 * GFX_STATE_REGS bytes con una versión y los registros, campo por campo en
 * el orden de la tabla de abajo, y después la TMEM tal cual (sus bytes ya
 * están en el orden de la N64). Lo del RSP (matrices, luces, segmentos) no
 * va: se reinicia en cada tarea. Agregar un campo: al final de regs2,
 * después de las tiles (los estados viejos lo dejan en cero), y subir
 * FORMAT solo si cambia el sentido de uno que ya estaba.
 *
 * Corre en el host y en el PPE, nunca en el SPE: el SPE solo copia el tramo
 * crudo de su estado (GFX_RAW_START a GFX_RAW_END) y el PPE lo pasa a este
 * formato (src/platform/ps3/gfx_spu.c).
 */
#include <stddef.h>
#include <string.h>

#include "core/endian.h"
#include "rsp/gfx/hle.h"

#define FORMAT 1u

struct field {
	uint16_t off;
	uint8_t size;
};

#define F(f) { offsetof(struct gfx_state, f), sizeof(((struct gfx_state *)0)->f) }
static const struct field regs[] = {
	F(oh), F(ol), F(cc_hi), F(cc_lo), F(prim), F(env), F(fog), F(blend), F(fill),
	F(prim_lod), F(prim_min), F(prim_z), F(prim_dz),
	F(scissor[0]), F(scissor[1]), F(scissor[2]), F(scissor[3]),
	F(cimg_addr), F(cimg_width), F(cimg_fmt), F(cimg_siz), F(zimg_addr),
	F(timg_addr), F(timg_width), F(timg_fmt), F(timg_siz),
};
#undef F

/* Los de cada una de las 8 tiles, relativos a la tile. */
#define T(f) { offsetof(struct gfx_tile, f), sizeof(((struct gfx_tile *)0)->f) }
static const struct field tile[] = {
	T(fmt), T(siz), T(pal), T(cms), T(cmt), T(masks), T(maskt), T(shifts), T(shiftt),
	T(line), T(tmem), T(uls), T(ult), T(lrs), T(lrt),
};
#undef T

/* Agregados después de las tiles (los estados viejos los dejan en cero). */
#define F(f) { offsetof(struct gfx_state, f), sizeof(((struct gfx_state *)0)->f) }
static const struct field regs2[] = {
	F(k4), F(k5),
	F(k0), F(k1), F(k2), F(k3),
};
#undef F

_Static_assert(sizeof(struct gfx_state) <= 0xFFFF, "los desplazamientos de la tabla no caben en 16 bits");

static uint32_t get(const uint8_t *p, unsigned size)
{
	if (size == 1)
		return *p;
	if (size == 2)
		return *(const uint16_t *)(const void *)p;
	return *(const uint32_t *)(const void *)p;
}

static void set(uint8_t *p, unsigned size, uint32_t v)
{
	if (size == 1)
		*p = (uint8_t)v;
	else if (size == 2)
		*(uint16_t *)(void *)p = (uint16_t)v;
	else
		*(uint32_t *)(void *)p = v;
}

/* Recorre la tabla en un sentido u otro; devuelve los bytes del formato. */
static unsigned walk(struct gfx_state *g, uint8_t *buf, bool load)
{
	uint8_t *base = (uint8_t *)g;
	unsigned pos = 4;
	const unsigned nr = sizeof regs / sizeof regs[0], nt = sizeof tile / sizeof tile[0];
	const unsigned n2 = sizeof regs2 / sizeof regs2[0];
	for (unsigned i = 0; i < nr + 8 * nt + n2; i++) {
		const struct field *f = i < nr ? &regs[i] : i < nr + 8 * nt ? &tile[(i - nr) % nt] : &regs2[i - nr - 8 * nt];
		uint8_t *p = base + f->off;
		if (i >= nr && i < nr + 8 * nt)
			p += offsetof(struct gfx_state, tiles) + (i - nr) / nt * sizeof(struct gfx_tile);
		if (load) {
			uint32_t v = 0;
			for (unsigned k = 0; k < f->size; k++)
				v = v << 8 | buf[pos + k];
			set(p, f->size, v);
		} else {
			uint32_t v = get(p, f->size);
			for (unsigned k = 0; k < f->size; k++)
				buf[pos + k] = (uint8_t)(v >> (8 * (f->size - 1 - k)));
		}
		pos += f->size;
	}
	return pos;
}

void gfx_state_pack_regs(const struct gfx_state *g, uint8_t out[GFX_STATE_REGS])
{
	memset(out, 0, GFX_STATE_REGS);
	salsa64_wr32(out, FORMAT);
	walk((struct gfx_state *)g, out, false);
}

bool gfx_state_unpack_regs(struct gfx_state *g, const uint8_t in[GFX_STATE_REGS])
{
	if (salsa64_rd32(in) != FORMAT)
		return false;
	walk(g, (uint8_t *)in, true);
	g->tex_gen++;  /* las texturas resueltas por tile ya no valen */
	return true;
}

/* 4 de la versión + 70 de los registros + 8 × 21 de las tiles + 12 de K0–K5. */
_Static_assert(4 + 70 + 8 * 21 + 12 <= GFX_STATE_REGS, "los registros del RDP no caben en GFX_STATE_REGS");
_Static_assert(sizeof(((struct gfx_state *)0)->tmem) == GFX_STATE_BYTES - GFX_STATE_REGS,
	       "la TMEM no es lo que resta de GFX_STATE_BYTES");
