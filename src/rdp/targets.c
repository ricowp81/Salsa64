/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — destinos de render del RDP. Ver targets.h.
 */
#include "rdp/targets.h"

#include <math.h>
#include <string.h>

/* G_IM_SIZ_* de G_SETCIMG (gbi): 4, 8, 16 y 32 bits. */
#define IM_SIZ_8b  1u
#define IM_SIZ_32b 3u

void rdp_targets_init(struct rdp_targets *s)
{
	memset(s, 0, sizeof *s);
	s->shown = -1;
	s->vi_width = 320;
}

uint32_t rdp_surface_bytes(uint32_t w, uint32_t h)
{
	return ((w * 4 + 127) & ~127u) * h;
}

uint32_t rdp_vram_worst(uint32_t scale)
{
	uint32_t w, h;
	rdp_surface_max(scale, &w, &h);  /* a 1× una de 640 × 480 queda nativa */
	return RDP_SURFACES_MAX * rdp_surface_bytes(w, h);
}

void rdp_surface_max(uint32_t scale, uint32_t *w, uint32_t *h)
{
	*w = 320 * scale > 640 ? 320 * scale : 640;
	*h = 240 * scale > 480 ? 240 * scale : 480;
}

bool rdp_zeta_reserve(uint32_t scale, uint32_t screen_w, uint32_t screen_h, uint32_t *w, uint32_t *h)
{
	uint32_t mw, mh;
	rdp_surface_max(scale, &mw, &mh);
	if (mw <= screen_w && mh <= screen_h) {
		*w = *h = 0;
		return false;
	}
	*w = mw;
	*h = mh;
	return true;
}

void rdp_surface_rect(uint32_t scale, uint32_t x, uint32_t y, uint32_t w, uint32_t h, struct rdp_rect *out)
{
	if (!scale)
		scale = 1;
	out->x = x * scale;
	out->y = y * scale;
	out->w = w * scale;
	out->h = h * scale;
}

void rdp_target_tex_map(uint32_t scale, uint32_t lw, uint32_t x0, uint32_t y0, uint32_t tex_w, uint32_t tex_h,
			float xf0, float xf2, struct rdp_tex_map *out)
{
	if (!scale)
		scale = 1;
	out->row0 = y0 * scale;
	out->rows = tex_h * scale;
	float w = (float)tex_w, l = lw ? (float)lw : 1.0f;
	out->xf0 = xf0 * w / l;
	out->xf2 = ((float)x0 + xf2 * w) / l;
}

void rdp_decode_rgba16(const uint8_t *rdram, uint32_t rdram_size, uint32_t addr, uint32_t pitch, uint32_t w,
		       uint32_t h, uint8_t *out, uint32_t out_pitch)
{
	for (uint32_t y = 0; y < h; y++) {
		uint8_t *o = out + y * out_pitch;
		uint32_t a = addr + y * pitch * 2;
		for (uint32_t x = 0; x < w; x++, a += 2, o += 4) {
			if (a + 2 > rdram_size || a < addr) {
				memset(o, 0, 4);
				continue;
			}
			uint32_t p = (uint32_t)rdram[a] << 8 | rdram[a + 1];
			uint32_t r = p >> 11, g = (p >> 6) & 31, b = (p >> 1) & 31;
			o[0] = (uint8_t)(r << 3 | r >> 2);
			o[1] = (uint8_t)(g << 3 | g >> 2);
			o[2] = (uint8_t)(b << 3 | b >> 2);
			o[3] = (p & 1) ? 0xFF : 0;
		}
	}
}

void rdp_decode_i8(const uint8_t *rdram, uint32_t rdram_size, uint32_t addr, uint32_t pitch, uint32_t w, uint32_t h,
		   uint8_t *out, uint32_t out_pitch)
{
	for (uint32_t y = 0; y < h; y++) {
		uint8_t *o = out + y * out_pitch;
		uint32_t a = addr + y * pitch;
		for (uint32_t x = 0; x < w; x++, a++, o += 4) {
			uint8_t i = a < rdram_size && a >= addr ? rdram[a] : 0;
			o[0] = o[1] = o[2] = o[3] = i;
		}
	}
}

uint32_t rdp_z16_to_z24(uint16_t v)
{
	/* La tabla de descompresión del RDP: corrimiento de la mantisa y base. */
	static const uint8_t shift[8] = { 6, 5, 4, 3, 2, 1, 0, 0 };
	static const uint32_t base[8] = { 0x00000, 0x20000, 0x30000, 0x38000, 0x3c000, 0x3e000, 0x3f000, 0x3f800 };
	uint32_t c = v >> 2, e = c >> 11, m = c & 0x7FFu;
	uint32_t z18 = (m << shift[e]) + base[e];
	return z18 << 6 | z18 >> 12;
}

bool rdp_present_integer(uint32_t src_w, uint32_t src_h, uint32_t screen_w, uint32_t screen_h)
{
	if (!src_w || !src_h || src_w > screen_w || src_h > screen_h)
		return false;
	uint32_t k = screen_h / src_h;
	return src_h * k * 4 >= screen_h * 3;
}

uint32_t rdp_target_scale(uint32_t global, uint32_t lw, uint32_t lh)
{
	uint32_t s = lh ? 240 * global / lh : 1;
	if (s > RDP_SCALE_MAX)
		s = RDP_SCALE_MAX;
	while (s > 1 && (lw * s > 4096 || lh * s > 4096))
		s--;
	return s ? s : 1;
}

uint32_t rdp_scale_pick(uint32_t requested, uint32_t screen_h, bool *reduced)
{
	return rdp_scale_pick_budget(requested, screen_h, RDP_VRAM_BUDGET, reduced);
}

uint32_t rdp_scale_pick_budget(uint32_t requested, uint32_t screen_h, uint32_t budget, bool *reduced)
{
	uint32_t s = requested;
	if (!s)
		s = screen_h >= 240 ? screen_h / 240 : 1;
	if (s > RDP_SCALE_MAX)
		s = RDP_SCALE_MAX;
	bool cut = false;
	while (s > 1 && rdp_vram_worst(s) > budget) {
		s--;
		cut = true;
	}
	if (reduced)
		*reduced = cut;
	return s;
}

static bool stale(const struct rdp_targets *s, const struct rdp_target *t)
{
	return s->field - t->last_field > RDP_TARGETS_STALE;
}

/* Orden de reciclado: menor es más reciclable. */
static int evict_class(const struct rdp_targets *s, const struct rdp_target *t)
{
	if (stale(s, t))
		return 0;
	if (t->width != s->vi_width)
		return 1;
	return 2;
}

/* Qué hace rdp_targets_get con esta color image: 0 ya existe (sirve tal
 * cual), 1 existe y crece, 2 es nueva en la ranura devuelta (libre, la de
 * otro tamaño de texel o la que se recicla). Sin tocar nada. */
static int choose(const struct rdp_targets *s, uint32_t addr, uint32_t width, uint32_t siz, uint32_t min_lh, int *kind)
{
	int free_i = -1, victim = -1, again = -1;
	uint32_t lw = width ? width : 320;
	uint32_t lh = lw * 3 / 4;
	uint32_t bpp = siz == IM_SIZ_32b ? 4 : siz == IM_SIZ_8b ? 1 : 2;
	if (min_lh > lh)
		lh = min_lh;
	*kind = 2;
	for (int i = 0; i < RDP_TARGETS_MAX; i++) {
		const struct rdp_target *t = &s->t[i];
		if (!t->used) {
			if (free_i < 0)
				free_i = i;
			continue;
		}
		if (t->addr == addr && t->width == width && t->bpp != bpp && s->task - t->last_task > RDP_TARGETS_FRESH) {
			/* La misma memoria con otro tamaño de texel, y lo de antes ya
			 * es viejo: es otra imagen. Majora's Mask usa 784600/320 como
			 * cobertura de 8 bits en la pausa y como copia de 16 bits del
			 * cuadro en las campanadas del fin del día; con la ranura de 8
			 * bits, las texturas de 16 no la encontraban y la copia a la
			 * RDRAM escribía bytes de intensidad. Si lo de antes está vivo
			 * se comparte, como hasta ahora: Zelda OoT pone su framebuffer
			 * de 16 bits como color image de 8 en la misma tarea para dejar
			 * la cobertura. */
			again = i;
			break;
		}
		if (t->addr == addr && t->width == width) {
			*kind = t->lh >= lh ? 0 : 1;
			return i;
		}
		if (i == s->shown)
			continue;
		if (victim < 0) {
			victim = i;
			continue;
		}
		const struct rdp_target *v = &s->t[victim];
		int ct = evict_class(s, t), cv = evict_class(s, v);
		if (ct < cv || (ct == cv && (int32_t)(t->last_field - v->last_field) < 0))
			victim = i;
	}
	int i = again >= 0 ? again : free_i >= 0 ? free_i : victim;
	if (i < 0)
		i = s->shown >= 0 ? s->shown : 0;  /* no pasa: hay más de una ranura */
	return i;
}

enum rdp_cimg_bad rdp_cimg_impossible(uint32_t addr, uint32_t width, uint32_t min_lh, uint32_t rdram_size)
{
	if (!width || width > 640)
		return RDP_CIMG_WIDTH;
	if (min_lh > 480)
		return RDP_CIMG_HEIGHT;
	if (rdram_size && addr >= rdram_size)
		return RDP_CIMG_RDRAM;
	return RDP_CIMG_OK;
}

int rdp_targets_recycled(const struct rdp_targets *s, uint32_t addr, uint32_t width, uint32_t siz, uint32_t min_lh)
{
	int kind;
	int i = choose(s, addr, width, siz, min_lh, &kind);
	return kind != 0 && s->t[i].used ? i : -1;
}

int rdp_targets_get(struct rdp_targets *s, uint32_t addr, uint32_t width, uint32_t siz, uint32_t min_lh,
		    bool *fresh)
{
	uint32_t lw = width ? width : 320;
	uint32_t lh = lw * 3 / 4;
	uint32_t bpp = siz == IM_SIZ_32b ? 4 : siz == IM_SIZ_8b ? 1 : 2;
	if (min_lh > lh)
		lh = min_lh;
	int kind;
	int i = choose(s, addr, width, siz, min_lh, &kind);
	if (kind == 0) {
		*fresh = false;
		return i;
	}
	if (kind == 1) {
		/* Crece: la superficie se vuelve a crear (lo dibujado hasta
		 * ahora se pierde; pasa una vez). */
		struct rdp_target *t = &s->t[i];
		t->lh = lh;
		t->drawn = false;
		t->mirrored = true;
		memset(&t->box, 0, sizeof t->box);
		s->created++;
		*fresh = true;
		return i;
	}
	struct rdp_target *t = &s->t[i];
	t->used = true;
	t->drawn = false;
	t->presented = false;
	t->cpu_owned = false;
	t->mirrored = true;  /* nada dibujado aún que copiar a la RDRAM */
	memset(&t->box, 0, sizeof t->box);
	t->addr = addr;
	t->width = width;
	t->bpp = bpp;
	t->fmt = 0;
	t->lw = lw;
	t->lh = lh;
	t->last_field = s->field;
	t->last_task = s->task;
	s->created++;
	*fresh = true;
	return i;
}

void rdp_targets_drawn(struct rdp_targets *s, int slot)
{
	if (slot < 0 || slot >= RDP_TARGETS_MAX)
		return;
	s->t[slot].last_field = s->field;
	s->t[slot].last_task = s->task;
	s->t[slot].drawn = true;
	s->t[slot].cpu_owned = false;  /* el RDP volvió a escribir: la verdad está en el RSX */
	s->t[slot].mirrored = false;
}

void rdp_targets_task(struct rdp_targets *s)
{
	s->task++;
}

enum rdp_texsrc rdp_targets_texture(struct rdp_targets *s, uint32_t addr, uint32_t pitch, uint32_t bpp, int cur,
				    int *slot, uint32_t *x0, uint32_t *y0)
{
	int i = rdp_targets_locate(s, addr, pitch, bpp, x0, y0);
	*slot = i;
	if (i < 0)
		return RDP_TEXSRC_NONE;
	const struct rdp_target *t = &s->t[i];
	if (t->cpu_owned || s->task - t->last_task > RDP_TARGETS_FRESH) {
		s->tex_rdram++;
		return RDP_TEXSRC_RDRAM;
	}
	if (i == cur) {
		s->tex_self++;
		return RDP_TEXSRC_SELF;
	}
	s->tex_rsx++;
	return RDP_TEXSRC_RSX;
}

/* The caller reconciled changed RDRAM spans into a live surface. allow_stale
 * is for a compatibility profile whose RDRAM copy is incomplete; never use a
 * CPU-owned target even when stale sampling is explicitly enabled. */
enum rdp_texsrc rdp_targets_texture_synced(struct rdp_targets *s, uint32_t addr, uint32_t pitch, uint32_t bpp,
					   int cur, bool allow_stale, int *slot, uint32_t *x0, uint32_t *y0)
{
	int i = rdp_targets_locate(s, addr, pitch, bpp, x0, y0);
	*slot = i;
	if (i < 0)
		return RDP_TEXSRC_NONE;
	if (s->t[i].cpu_owned || (!allow_stale && s->task - s->t[i].last_task > RDP_TARGETS_FRESH)) {
		s->tex_rdram++;
		return RDP_TEXSRC_RDRAM;
	}
	if (i == cur) {
		s->tex_self++;
		return RDP_TEXSRC_SELF;
	}
	s->tex_rsx++;
	return RDP_TEXSRC_RSX;
}

bool rdp_targets_readback(struct rdp_targets *s, int slot, bool force, bool cpu_owns)
{
	if (slot < 0 || slot >= RDP_TARGETS_MAX || !s->t[slot].used)
		return false;
	struct rdp_target *t = &s->t[slot];
	if (!force) {
		if (t->presented || t->bpp != 2 || t->width != s->vi_width)
			return false;
	}
	if (cpu_owns)
		t->cpu_owned = true;
	s->readbacks++;
	return true;
}

/* CopyColorFromRDRAM has reconciled CPU-owned bytes with the RSX surface.
 * The surface can again be sampled as a texture, while normal freshness rules
 * still decide what to do with old targets. */
void rdp_targets_rdram_uploaded(struct rdp_targets *s, int slot)
{
	if (slot >= 0 && slot < RDP_TARGETS_MAX && s->t[slot].used)
		s->t[slot].cpu_owned = false;
}

bool rdp_targets_mirror_wanted(const struct rdp_targets *s, int slot)
{
	return slot >= 0 && slot < RDP_TARGETS_MAX && s->t[slot].used && !s->t[slot].mirrored;
}

bool rdp_targets_mirror_at_task_end(const struct rdp_targets *s, int slot)
{
	if (!rdp_targets_mirror_wanted(s, slot))
		return false;
	const struct rdp_target *t = &s->t[slot];
	return t->presented && t->bpp == 2 && t->width == s->vi_width;
}

void rdp_targets_mirrored(struct rdp_targets *s, int slot)
{
	if (slot >= 0 && slot < RDP_TARGETS_MAX) {
		s->t[slot].mirrored = true;
		memset(&s->t[slot].box, 0, sizeof s->t[slot].box);
	}
}

void rdp_box_union(struct rdp_box *b, const struct rdp_box *r)
{
	if (rdp_box_empty(r))
		return;
	if (rdp_box_empty(b)) {
		*b = *r;
		return;
	}
	b->x0 = r->x0 < b->x0 ? r->x0 : b->x0;
	b->y0 = r->y0 < b->y0 ? r->y0 : b->y0;
	b->x1 = r->x1 > b->x1 ? r->x1 : b->x1;
	b->y1 = r->y1 > b->y1 ? r->y1 : b->y1;
}

void rdp_box_add(struct rdp_box *b, float x0, float y0, float x1, float y1, bool all_front,
		 const uint16_t scissor[4], uint32_t lw, uint32_t lh)
{
	/* Tijeras en 10.2: la primera columna o fila que tocan y la que ya no. */
	float sx0 = (float)(scissor[0] / 4), sy0 = (float)(scissor[1] / 4);
	float sx1 = (float)((scissor[2] + 3) / 4), sy1 = (float)((scissor[3] + 3) / 4);
	if (all_front) {
		sx0 = x0 > sx0 ? x0 : sx0;
		sy0 = y0 > sy0 ? y0 : sy0;
		sx1 = x1 < sx1 ? x1 : sx1;
		sy1 = y1 < sy1 ? y1 : sy1;
	}
	if (!(sx1 > sx0 && sy1 > sy0))
		return;
	/* Enteros que cubren la caja, recortados al destino. */
	float fx0 = floorf(sx0), fy0 = floorf(sy0), fx1 = ceilf(sx1), fy1 = ceilf(sy1);
	struct rdp_box r;
	r.x0 = (uint16_t)(fx0 < 0.0f ? 0.0f : fx0 > (float)lw ? (float)lw : fx0);
	r.y0 = (uint16_t)(fy0 < 0.0f ? 0.0f : fy0 > (float)lh ? (float)lh : fy0);
	r.x1 = (uint16_t)(fx1 < 0.0f ? 0.0f : fx1 > (float)lw ? (float)lw : fx1);
	r.y1 = (uint16_t)(fy1 < 0.0f ? 0.0f : fy1 > (float)lh ? (float)lh : fy1);
	rdp_box_union(b, &r);
}

void rdp_vi_ring_push(struct rdp_vi_ring *r, uint32_t origin, uint32_t width)
{
	struct salsa64_vi_frame f;
	memset(&f, 0, sizeof f);
	f.origin = origin;
	f.width = width;
	f.height = width * 3 / 4;
	f.stride = width * 2;
	f.bpp = 2;
	rdp_vi_ring_frame(r, &f);
}

void rdp_vi_ring_frame(struct rdp_vi_ring *r, const struct salsa64_vi_frame *f)
{
	if (!f->width)
		return;
	r->cur_width = f->width;
	for (uint32_t k = 0; k < RDP_VI_RING && k < r->n; k++)
		if (r->origin[(r->n - 1 - k) % RDP_VI_RING] == f->origin)
			return;
	uint32_t i = r->n % RDP_VI_RING;
	r->origin[i] = f->origin;
	r->width[i] = f->width;
	r->height[i] = f->height;
	r->stride[i] = f->stride;
	r->bpp[i] = f->bpp;
	r->n++;
}

bool rdp_vi_ring_covers(const struct rdp_vi_ring *r, uint32_t addr, uint32_t width)
{
	(void)width;
	for (uint32_t k = 0; k < RDP_VI_RING && k < r->n; k++) {
		uint32_t o = r->origin[(r->n - 1 - k) % RDP_VI_RING], w = r->width[(r->n - 1 - k) % RDP_VI_RING];
		/* Un framebuffer empieza un poco antes del origen (líneas que el VI
		 * no muestra) y mide unas 3/4 del ancho de alto. */
		uint32_t line = w * 2, lo = o > 8 * line ? o - 8 * line : 0, hi = o + line * (w * 3 / 4);
		if (addr >= lo && addr < hi)
			return true;
	}
	return false;
}

bool rdp_target_profile_vi_visible(const struct rdp_target *t, const struct rdp_vi_ring *r)
{
	return t && t->used && (t->presented || rdp_vi_ring_covers(r, t->addr, t->width));
}

uint32_t rdp_vi_ring_min_height(const struct rdp_vi_ring *r, uint32_t addr, uint32_t width, uint32_t bpp)
{
	uint32_t line = width * bpp, need = 0;
	if (!line)
		return 0;
	for (uint32_t k = 0; k < RDP_VI_RING && k < r->n; k++) {
		uint32_t i = (r->n - 1 - k) % RDP_VI_RING;
		if (r->width[i] != width || r->bpp[i] != bpp || r->stride[i] < line || r->stride[i] % line ||
		    r->origin[i] < addr)
			continue;
		uint32_t off = r->origin[i] - addr;
		/* Solo los pocos renglones ocultos antes del origen, como la regla
		 * que reconoce un framebuffer en rdp_vi_ring_covers. */
		if (off % line || off > 8 * line)
			continue;
		uint32_t rows = r->height[i] * (r->stride[i] / line);
		if (rows && off / line + rows > need)
			need = off / line + rows;
	}
	return need;
}

bool rdp_readback_wanted(const struct rdp_vi_ring *r, uint32_t addr, uint32_t width, uint32_t siz)
{
	if (siz != 2u || !r->n || width != r->cur_width)
		return false;
	return !rdp_vi_ring_covers(r, addr, width);
}

int rdp_targets_locate(const struct rdp_targets *s, uint32_t addr, uint32_t pitch, uint32_t bpp, uint32_t *x0,
		       uint32_t *y0)
{
	int best = -1;
	uint32_t best_off = 0;
	for (int i = 0; i < RDP_TARGETS_MAX; i++) {
		const struct rdp_target *t = &s->t[i];
		uint32_t line = t->width * t->bpp;
		if (!t->used || t->width != pitch || t->bpp != bpp || !line || addr < t->addr ||
		    addr >= t->addr + line * t->lh)
			continue;
		uint32_t off = addr - t->addr;
		if (best < 0 || off < best_off) {
			best = i;
			best_off = off;
		}
	}
	if (best < 0)
		return -1;
	uint32_t line = s->t[best].width * bpp;
	*x0 = (best_off % line) / bpp;
	*y0 = best_off / line;
	return best;
}

/* ¿El destino cubre el origen del VI? Da la línea lógica donde cae. */
static bool covers(const struct rdp_target *t, uint32_t origin, uint32_t *y0)
{
	uint32_t line = t->width * t->bpp;
	if (!line || origin < t->addr || origin >= t->addr + line * t->lh)
		return false;
	*y0 = (origin - t->addr) / line;
	return true;
}

void rdp_targets_present(struct rdp_targets *s, const struct salsa64_vi_frame *f, uint32_t field,
			 struct rdp_present *out)
{
	s->field = field;
	/* El campo solo avanza cuando el VI muestra algo: lo dibujado mientras
	 * estuvo en negro (SM64 lo apaga 26 campos al cambiar de escena) queda
	 * con el campo de ahora, no con el de antes del apagado. La antigüedad
	 * de cada destino (para medir el retraso de lo que se ve) se toma antes. */
	uint32_t age[RDP_TARGETS_MAX];
	for (int i = 0; i < RDP_TARGETS_MAX; i++) {
		age[i] = s->t[i].drawn ? 0 : field - s->t[i].last_field;
		if (s->t[i].drawn) {
			s->t[i].last_field = field;
			s->t[i].drawn = false;
		}
	}
	if (f->width)
		s->vi_width = f->width;

	int best = -1;
	uint32_t best_y0 = 0;
	for (int i = 0; i < RDP_TARGETS_MAX; i++) {
		const struct rdp_target *t = &s->t[i];
		uint32_t y0;
		if (!t->used || stale(s, t) || !covers(t, f->origin, &y0))
			continue;
		if (best >= 0) {
			const struct rdp_target *b = &s->t[best];
			bool tw = t->width == f->width, bw = b->width == f->width;
			if (bw != tw) {
				if (bw)
					continue;
			} else if (y0 != best_y0) {
				if (y0 > best_y0)
					continue;
			} else if ((int32_t)(t->last_field - b->last_field) <= 0) {
				continue;
			}
		}
		best = i;
		best_y0 = y0;
	}
	if (best < 0 && s->shown >= 0 && s->t[s->shown].used && f->origin == s->shown_origin &&
	    field - s->shown_field <= 2 && covers(&s->t[s->shown], f->origin, &best_y0))
		best = s->shown;

	uint32_t previous_field = s->shown_field;
	s->shown = best;
	s->shown_origin = f->origin;
	s->shown_field = field;
	if (best >= 0)
		s->t[best].presented = true;
	out->slot = best;
	out->y0 = out->w = out->h = out->age = 0;
	if (best < 0) {
		s->il_weave = false;
		return;
	}
	const struct rdp_target *t = &s->t[best];
	out->age = age[best];
	out->y0 = best_y0;
	out->w = f->width < t->lw ? f->width : t->lw;
	/* Entrelazado de alta resolución (los logos de Donkey Kong 64: un
	 * búfer de 640 × 480 del que cada campo muestra una línea de cada dos,
	 * con el VI a 1280 de ancho y el origen alternando entre dos líneas
	 * seguidas): el campo abarca k líneas del destino por cada una suya.
	 * Se presenta el cuadro entero desde la primera de esas líneas, que
	 * ocupa en pantalla lo mismo que el campo y no salta entre campos. */
	uint32_t line = t->width * t->bpp, k = line && f->stride > line && f->stride % line == 0 ? f->stride / line : 1;
	uint32_t rows = f->height * k;
	if (f->vpos) {
		/* Entrelazado con la posición vertical del campo (V_VIDEO y el
		 * campo): el renglón 0 del destino cae en la pantalla, medido en
		 * renglones de la trama (una fila del destino son 2 / k), en
		 * V0 = vpos - 2 y0 / k. Dos campos de un mismo cuadro caen a lo más
		 * un renglón aparte aunque cada uno empiece en una fila distinta
		 * (Racer: origen una fila arriba y V_VIDEO dos medias líneas antes;
		 * Rogue Squadron: origen una fila abajo y dos medias líneas después):
		 * se presenta la misma ventana en los dos, la del campo anterior
		 * del mismo destino, en vez de cada una desde su origen (la imagen
		 * subía y bajaba una fila por campo). El primero de la pareja
		 * alinea a k filas según la fila de la trama en que cae. */
		int32_t v0 = (int32_t)(f->vpos * k) - 2 * (int32_t)best_y0;
		uint32_t y0p = best_y0 - f->vpos % k % (best_y0 + 1);
		/* Indy alterna dos cuadros completos, uno desde la fila 1 y otro
		 * desde la 2, con V_VIDEO desplazado también. La ventana común debe
		 * sobrevivir al cambio de búfer; limitarla a una ranura hacía temblar
		 * el cuadro aunque cada superficie ya tuviera las 480 líneas. */
		bool weave = f->weave && k == 1 && rows <= t->lh - best_y0;
		bool same_frame = s->il_slot == best ||
			(weave && s->il_weave && field - previous_field == 1 && s->il_width == t->width &&
			 s->il_height == t->lh && s->il_bpp == t->bpp);
		if (s->il_valid && same_frame && v0 - s->il_v0 >= -(int32_t)k && v0 - s->il_v0 <= (int32_t)k &&
		    s->il_y0 <= best_y0 && best_y0 - s->il_y0 <= k)
			y0p = s->il_y0;
		s->il_valid = true;
		s->il_slot = best;
		s->il_v0 = v0;
		s->il_y0 = y0p;
		s->il_weave = weave;
		s->il_width = t->width;
		s->il_height = t->lh;
		s->il_bpp = t->bpp;
		out->y0 = y0p;
	} else {
		s->il_weave = false;
		if (k > 1)
			out->y0 = best_y0 - best_y0 % k;
	}
	out->h = rows < t->lh - out->y0 ? rows : t->lh - out->y0;
}

static bool tall_applied(const struct rdp_tall_fill *e, uint32_t addr)
{
	for (uint32_t k = 0; k < e->napplied && k < RDP_TALL_APPLIED; k++)
		if (e->applied[k] == addr)
			return true;
	return false;
}

static void tall_mark(struct rdp_tall_fill *e, uint32_t addr)
{
	if (e->napplied < RDP_TALL_APPLIED && !tall_applied(e, addr))
		e->applied[e->napplied++] = addr;
}

void rdp_targets_tall_note(struct rdp_targets *s, uint32_t addr, uint32_t width, uint32_t bpp, uint32_t rows,
			   float x0, float y0, float x1, float y1, uint32_t color, const int *done, int ndone)
{
	if (!width || !bpp || !rows || !(x1 > x0) || !(y1 > y0))
		return;
	struct rdp_tall_fill *e = &s->tall[s->tall_next++ % RDP_TALL_FILLS];
	memset(e, 0, sizeof *e);
	e->used = true;
	e->addr = addr;
	e->width = width;
	e->bpp = bpp;
	e->rows = rows;
	e->x0 = x0;
	e->y0 = y0;
	e->x1 = x1;
	e->y1 = y1;
	e->color = color;
	e->task = s->task;
	for (int k = 0; k < ndone; k++)
		if (done[k] >= 0 && done[k] < RDP_TARGETS_MAX)
			tall_mark(e, s->t[done[k]].addr);
}

int rdp_targets_tall_pending(struct rdp_targets *s, int slot, const struct rdp_dma_ring *dma,
			     struct rdp_tall_rect out[RDP_TALL_FILLS])
{
	if (slot < 0 || slot >= RDP_TARGETS_MAX || !s->t[slot].used)
		return 0;
	const struct rdp_target *t = &s->t[slot];
	int n = 0;
	/* Del más viejo al más nuevo: el último gana donde se solapan. */
	for (uint32_t k = 0; k < RDP_TALL_FILLS; k++) {
		struct rdp_tall_fill *e = &s->tall[(s->tall_next + k) % RDP_TALL_FILLS];
		uint32_t line = e->width * e->bpp;
		if (!e->used || !line || s->task - e->task > RDP_TALL_FILL_TASKS || t->width != e->width ||
		    t->bpp != e->bpp || t->addr < e->addr || (t->addr - e->addr) % line ||
		    t->addr >= e->addr + line * e->rows || tall_applied(e, t->addr))
			continue;
		tall_mark(e, t->addr);
		if (dma && rdp_dma_since(dma, t->addr, t->addr + line * t->lh, e->task))
			continue;
		float first = (float)((t->addr - e->addr) / line);
		float ya = e->y0 - first, yb = e->y1 - first;
		float xa = e->x0 < 0.0f ? 0.0f : e->x0, xb = e->x1 > (float)t->lw ? (float)t->lw : e->x1;
		ya = ya < 0.0f ? 0.0f : ya;
		yb = yb > (float)t->lh ? (float)t->lh : yb;
		if (yb <= ya || xb <= xa)
			continue;
		out[n].x0 = xa;
		out[n].y0 = ya;
		out[n].x1 = xb;
		out[n].y1 = yb;
		out[n].color = e->color;
		n++;
	}
	return n;
}

void rdp_dma_note(struct rdp_dma_ring *r, uint32_t pa, uint32_t len, uint32_t task)
{
	if (!len)
		return;
	uint32_t n = __atomic_load_n(&r->n, __ATOMIC_RELAXED);
	uint32_t lo = pa, hi = pa + len;
	if (n) {
		uint32_t k = (n - 1) % RDP_DMA_RING;
		if (r->e[k].task == task && r->e[k].lo <= hi && r->e[k].hi >= lo) {
			/* Sigue o solapa al último de la misma tarea: se funden. */
			uint32_t nlo = r->e[k].lo < lo ? r->e[k].lo : lo, nhi = r->e[k].hi > hi ? r->e[k].hi : hi;
			r->e[k].lo = nlo;
			r->e[k].hi = nhi;
			__atomic_thread_fence(__ATOMIC_RELEASE);
			return;
		}
	}
	uint32_t k = n % RDP_DMA_RING;
	r->e[k].lo = lo;
	r->e[k].hi = hi;
	r->e[k].task = task;
	__atomic_store_n(&r->n, n + 1, __ATOMIC_RELEASE);
}

bool rdp_dma_since(const struct rdp_dma_ring *r, uint32_t lo, uint32_t hi, uint32_t task)
{
	uint32_t n = __atomic_load_n(&r->n, __ATOMIC_ACQUIRE);
	uint32_t m = n < RDP_DMA_RING ? n : RDP_DMA_RING;
	for (uint32_t i = 0; i < m; i++) {
		uint32_t k = (n - 1 - i) % RDP_DMA_RING;
		if (r->e[k].task >= task && r->e[k].lo < hi && r->e[k].hi > lo)
			return true;
	}
	return false;
}

void rdp_claim_note(struct rdp_claim_ring *r, uint32_t addr, uint32_t line, uint32_t lo, uint32_t hi, uint32_t task)
{
	if (hi <= lo)
		return;
	uint32_t n = __atomic_load_n(&r->n, __ATOMIC_RELAXED);
	/* Las de la misma tarea van seguidas: si ya está esta color image, se
	 * agranda (una tarea cambia de color image varias veces: la z, el
	 * framebuffer, otra vez el framebuffer). */
	for (uint32_t i = 0; i < n && i < RDP_CLAIM_RING; i++) {
		uint32_t k = (n - 1 - i) % RDP_CLAIM_RING;
		if (r->e[k].task != task)
			break;
		if (r->e[k].addr == addr && r->e[k].line == line) {
			uint32_t nlo = r->e[k].lo < lo ? r->e[k].lo : lo, nhi = r->e[k].hi > hi ? r->e[k].hi : hi;
			r->e[k].lo = nlo;
			r->e[k].hi = nhi;
			__atomic_thread_fence(__ATOMIC_RELEASE);
			return;
		}
	}
	uint32_t k = n % RDP_CLAIM_RING;
	r->e[k].lo = lo;
	r->e[k].hi = hi;
	r->e[k].addr = addr;
	r->e[k].line = line;
	r->e[k].task = task;
	__atomic_store_n(&r->n, n + 1, __ATOMIC_RELEASE);
}

int rdp_claims_after(const struct rdp_claim_ring *r, uint32_t addr, uint32_t line, uint32_t lo, uint32_t hi,
		     uint32_t task, struct rdp_claim *out, int max)
{
	uint32_t n = __atomic_load_n(&r->n, __ATOMIC_ACQUIRE);
	if (n > RDP_CLAIM_RING && (int32_t)(r->e[n % RDP_CLAIM_RING].task - task) > 0)
		return -1;  /* la más vieja que queda ya es posterior: pudo perderse alguna */
	uint32_t m = n < RDP_CLAIM_RING ? n : RDP_CLAIM_RING;
	int k = 0;
	for (uint32_t i = 0; i < m && max > 0; i++) {
		uint32_t j = (n - 1 - i) % RDP_CLAIM_RING;
		uint32_t elo = r->e[j].lo, ehi = r->e[j].hi;
		if ((int32_t)(r->e[j].task - task) <= 0 || (r->e[j].addr == addr && r->e[j].line == line) ||
		    elo >= hi || ehi <= lo)
			continue;
		if (k < max) {
			out[k].lo = elo;
			out[k].hi = ehi;
			k++;
		} else {
			out[max - 1].lo = elo < out[max - 1].lo ? elo : out[max - 1].lo;
			out[max - 1].hi = ehi > out[max - 1].hi ? ehi : out[max - 1].hi;
		}
	}
	return k;
}


uint32_t rdp_guard_hash(const uint8_t *line)
{
	/* Suma y o exclusivo rotado de las 16 palabras dobles: cualquier
	 * cambio de una sola palabra cambia la suma; sin multiplicaciones (en
	 * el PPE, mulld es lento y en cadena). Se lee como big-endian para que
	 * el host y la PS3 den lo mismo (pruebas). */
	uint64_t s = 0x9E3779B97F4A7C15ull, x = 0;
	for (unsigned i = 0; i < RDP_GUARD_LINE / 8; i++) {
		uint64_t w;
		memcpy(&w, line + 8 * i, 8);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
		w = __builtin_bswap64(w);
#endif
		s += w;
		x ^= w << (i * 4 + 1) | w >> (63 - i * 4);
	}
	s += x << 17 | x >> 47;
	return (uint32_t)(s ^ s >> 32) | 1u;
}

void rdp_guard_clear(struct rdp_guard *g)
{
	memset(g, 0, sizeof *g);
}

void rdp_guard_mark(struct rdp_guard *g, const uint8_t *rdram, uint32_t size, uint32_t lo, uint32_t hi)
{
	if (hi > size)
		hi = size;
	if (hi > RDP_GUARD_LINES << RDP_GUARD_SHIFT)
		hi = RDP_GUARD_LINES << RDP_GUARD_SHIFT;
	if (lo >= hi)
		return;
	for (uint32_t l = lo >> RDP_GUARD_SHIFT; l <= (hi - 1) >> RDP_GUARD_SHIFT; l++)
		g->base[l] = rdp_guard_hash(rdram + (l << RDP_GUARD_SHIFT));
}

/* Baseline used by CopyColorFromRDRAM. Unlike `base`/`wrote`, this snapshot
 * is updated only when data is uploaded to a target or our own readback writes
 * RDRAM. A later task-end guard therefore cannot absorb a CPU change before
 * the target is reused. */
void rdp_guard_profile_mark(struct rdp_guard *g, const uint8_t *rdram, uint32_t size, uint32_t lo, uint32_t hi)
{
	if (!g || !rdram)
		return;
	if (hi > size)
		hi = size;
	uint32_t limit = RDP_GUARD_LINES << RDP_GUARD_SHIFT;
	if (hi > limit)
		hi = limit;
	if (lo >= hi)
		return;
	for (uint32_t l = lo >> RDP_GUARD_SHIFT; l <= (hi - 1) >> RDP_GUARD_SHIFT; l++)
		g->profile[l] = rdp_guard_hash(rdram + (l << RDP_GUARD_SHIFT));
}

/* Do not let a late RSX readback replace a CPU change which predates the
 * most recent graphics task. The ordinary task-end guard cannot catch that
 * case because it snapshots the already-modified RDRAM. A zero baseline means
 * this line has not been uploaded/read back by the profile and remains safe. */
bool rdp_guard_profile_ok(const struct rdp_guard *g, const uint8_t *rdram, uint32_t line)
{
	if (!g || !rdram || line >= RDP_GUARD_LINES)
		return false;
	uint32_t baseline = g->profile[line];
	return !baseline || rdp_guard_hash(rdram + (line << RDP_GUARD_SHIFT)) == baseline;
}

/* Start a fresh CPU overlay batch when a newer RDP task updates framebuffer
 * bytes. Retire older covered overlays but retain erase tombstones until the
 * VI thread applies them; unchanged pixels survive repeated presentations. */
bool rdp_profile_pending_retire_ready(uint32_t target_task, uint32_t batch, bool batch_valid)
{
	return !batch_valid || (int32_t)(target_task - batch) >= 0;
}

bool rdp_profile_pending_new_batch(uint8_t *pending, uint32_t count, uint32_t *last_batch,
				   bool *last_batch_valid, uint32_t batch)
{
	if (!pending || !count || !last_batch || !last_batch_valid)
		return false;
	if (*last_batch_valid && *last_batch == batch)
		return false;
	/* A newer task redraws the target underneath all older CPU overlays.
	 * Retire every old state (including an unapplied transparent erase)
	 * without writing stale pixels over the new render. Changed pixels in
	 * this batch are then re-queued as BLEND or REPLACE by pending_note(). */
	for (uint32_t i = 0; i < count; i++)
		if (pending[i] != RDP_PROFILE_PENDING_NONE)
			pending[i] = RDP_PROFILE_PENDING_RETIRE;
	*last_batch = batch;
	*last_batch_valid = true;
	return true;
}

/* Keep a covered CPU-written pixel over later RDP draws until a newer CPU
 * overlay batch replaces it or the CPU clears its RGBA5551 coverage bit. A
 * transparent replacement is consumed once, even before the first replay. */
uint8_t rdp_profile_pending_note(uint8_t pending, bool was_covered, bool is_covered)
{
	if (is_covered)
		return RDP_PROFILE_PENDING_BLEND;
	/* RETIRE alone is not evidence that this source pixel was visible.
	 * A changed but still-uncovered pixel must stay retired: replacing it with
	 * transparent alpha would punch a hole in the newly rendered RSX image.
	 * Only a real old covered pixel (or a current covered overlay) needs erase. */
	if (was_covered || pending == RDP_PROFILE_PENDING_BLEND ||
	    pending == RDP_PROFILE_PENDING_FOLLOWUP)
		return RDP_PROFILE_PENDING_REPLACE;
	return pending;
}

/* Refresh pending state against the latest stable RDRAM coverage bit. */
uint8_t rdp_profile_pending_refresh(uint8_t pending, bool is_covered)
{
	if (!pending)
		return RDP_PROFILE_PENDING_NONE;
	/* A retired overlay waits for per-pixel RDP draw coverage. Until a later
	 * draw reaches that pixel, retain covered source pixels or schedule a
	 * transparent replacement when the CPU has cleared coverage. */
	if (pending == RDP_PROFILE_PENDING_RETIRE)
		return is_covered ? RDP_PROFILE_PENDING_FOLLOWUP : RDP_PROFILE_PENDING_REPLACE;
	if (is_covered) {
		if (pending != RDP_PROFILE_PENDING_FOLLOWUP)
			return RDP_PROFILE_PENDING_BLEND;
	} else if (pending == RDP_PROFILE_PENDING_BLEND ||
		   pending == RDP_PROFILE_PENDING_FOLLOWUP) {
		return RDP_PROFILE_PENDING_REPLACE;
	}
	return pending;
}

/* Covered overlays persist for the current CPU update batch; a newer batch
 * replaces stale positions, while coverage clears are one-shot erases. */
uint8_t rdp_profile_pending_complete(uint8_t mode)
{
	/* Each VI buffer is presented independently. Keep covered CPU pixels on
	 * the RSX surface through every later RDP redraw; refresh() turns the
	 * marker into a one-shot replacement as soon as RDRAM coverage goes low. */
	return mode == RDP_PROFILE_PENDING_BLEND || mode == RDP_PROFILE_PENDING_FOLLOWUP
		? RDP_PROFILE_PENDING_FOLLOWUP : RDP_PROFILE_PENDING_NONE;
}

/* A CPU-modified row is uploaded only if a previously initialized profile
 * baseline no longer matches. Guard lines are absolute 128-byte spans, so
 * unaligned row strides conservatively mark adjacent rows when they overlap
 * the same line. */
bool rdp_target_row_cpu_changed(const struct rdp_target *t, uint32_t row, const struct rdp_guard *g,
				const uint8_t *rdram, uint32_t rdram_size)
{
	if (!t || !g || !rdram || !t->used || t->bpp != 2 || !t->width || row >= t->lh)
		return false;
	uint64_t row_bytes = (uint64_t)t->width * t->bpp;
	uint64_t lo64 = (uint64_t)t->addr + (uint64_t)row * row_bytes;
	uint64_t hi64 = lo64 + row_bytes;
	if (lo64 >= rdram_size || hi64 <= lo64)
		return false;
	if (hi64 > rdram_size)
		hi64 = rdram_size;
	uint32_t first = (uint32_t)(lo64 >> RDP_GUARD_SHIFT);
	uint32_t last = (uint32_t)((hi64 - 1) >> RDP_GUARD_SHIFT);
	if (first >= RDP_GUARD_LINES)
		return false;
	if (last >= RDP_GUARD_LINES)
		last = RDP_GUARD_LINES - 1;
	for (uint32_t line = first; line <= last; line++) {
		uint32_t baseline = g->profile[line];
		if (baseline && rdp_guard_hash(rdram + (line << RDP_GUARD_SHIFT)) != baseline)
			return true;
	}
	return false;
}

int rdp_targets_inside(const struct rdp_targets *s, uint32_t addr, uint32_t width, uint32_t bpp, uint32_t rows,
		       int slots[RDP_TARGETS_MAX], uint32_t first_row[RDP_TARGETS_MAX])
{
	uint32_t line = width * bpp;
	int n = 0;
	if (!line)
		return 0;
	for (int i = 0; i < RDP_TARGETS_MAX; i++) {
		const struct rdp_target *t = &s->t[i];
		/* Empieza dentro, alineado a sus líneas; puede asomar por abajo
		 * (DK64: 459 filas sobre dos búferes de 240). */
		if (!t->used || t->width != width || t->bpp != bpp || t->addr < addr || (t->addr - addr) % line ||
		    t->addr >= addr + line * rows)
			continue;
		slots[n] = i;
		first_row[n] = (t->addr - addr) / line;
		n++;
	}
	return n;
}
