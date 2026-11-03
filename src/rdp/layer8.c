/*
 * Salsa64 — capas de 8 bits dibujadas en la RDRAM sin pasar por el RSX
 * (layer8.h). Portable: el PPE y las pruebas del host.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "rdp/layer8.h"

#include <math.h>
#include <string.h>

/* Rojo y alfa del combiner que se admiten: sin color de vértice
 * (k[j][1] y k[j][2] en cero) ni segunda textura; el rojo es el texel
 * (T0 por 1, nada más) o una constante (ONE, sin textura), y el alfa, si
 * hay prueba, el alfa del texel o una constante. */
static bool combiner_ok(const struct gfx_draw *d, bool *tex_red, bool *tex_alpha)
{
	if (d->nterms || d->fog || d->ntex > 1)
		return false;
	for (int j = 0; j < GFX_K_TERMS; j++)
		for (int l = 0; l < 4; l++)
			if (d->k[j][1][l] != 0.0f || d->k[j][2][l] != 0.0f)
				return false;
	if (d->k[GFX_K_T1][0][0] != 0.0f || d->k[GFX_K_T1A][0][0] != 0.0f || d->k[GFX_K_T0A][0][0] != 0.0f)
		return false;
	if (d->ntex == 1 && d->k[GFX_K_T0][0][0] == 1.0f && d->k[GFX_K_ONE][0][0] == 0.0f)
		*tex_red = true;
	else if (d->k[GFX_K_T0][0][0] == 0.0f)
		*tex_red = false;
	else
		return false;
	*tex_alpha = false;
	if (d->atest != GFX_ATEST_NONE) {
		if (d->k[GFX_K_T1][0][3] != 0.0f || d->k[GFX_K_T1A][0][3] != 0.0f || d->k[GFX_K_T0][0][3] != 0.0f)
			return false;
		if (d->ntex == 1 && d->k[GFX_K_T0A][0][3] == 1.0f && d->k[GFX_K_ONE][0][3] == 0.0f)
			*tex_alpha = true;
		else if (d->k[GFX_K_T0A][0][3] != 0.0f)
			return false;
	}
	return true;
}

bool layer8_draw_ok(const struct gfx_draw *d, const struct gfx_vertex *v)
{
	bool tr, ta;
	if (d->h.b < 3 || d->h.b % 3 != 0 || d->blend != GFX_BLEND_OFF || d->depth != 0 || d->viscvg ||
	    d->cycle == 1 /* G_CYC_2CYCLE */)
		return false;
	if (!combiner_ok(d, &tr, &ta))
		return false;
	if (d->ntex == 1) {
		const struct gfx_tex *t = &d->tex[0];
		if (t->where == GFX_TEX_TARGET || (t->flags & GFX_TEX_BILINEAR) || !t->width || !t->height ||
		    GFX_TEX_WRAP_S(t->flags) == GFX_WRAP_MIRROR_ONCE || GFX_TEX_WRAP_T(t->flags) == GFX_WRAP_MIRROR_ONCE)
			return false;
	}
	for (uint32_t k = 0; k < d->h.b; k++)
		if (v[k].w != 1.0f)
			return false;
	return true;
}

uint64_t (*layer8_clock)(void);
bool layer8_fast = true;

static inline int wrap(int i, int n, unsigned mode)
{
	if (mode == GFX_WRAP_CLAMP)
		return i < 0 ? 0 : i >= n ? n - 1 : i;
	int m = i % n;
	if (m < 0)
		m += n;
	if (mode == GFX_WRAP_MIRROR && ((i < 0 ? -1 - i : i) / n) & 1)
		m = n - 1 - m;
	return m;
}

static inline float edge(float ax, float ay, float bx, float by, float px, float py)
{
	return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* Lo que comparten los dos caminos: cómo sale el rojo y la prueba de alfa. */
struct l8_setup {
	bool tex_red, tex_alpha;
	const struct gfx_tex *t;
	const uint8_t *tp;
	uint8_t red_const;
	float x0, y0, x1, y1;  /* tijeras, en píxeles */
};

static bool setup(const struct gfx_draw *d, uint32_t width, const uint8_t *arena, const uint8_t *cache,
		  struct l8_setup *u)
{
	if (!combiner_ok(d, &u->tex_red, &u->tex_alpha))
		return false;
	u->t = d->ntex ? &d->tex[0] : NULL;
	u->tp = NULL;
	if (u->t) {
		const uint8_t *base = u->t->where == GFX_TEX_CACHE ? cache : arena;
		if (!base)
			return false;
		u->tp = base + u->t->offset;
	}
	/* Como channel() del host: el rojo constante y la prueba de alfa. */
	u->red_const = (uint8_t)(d->k[GFX_K_ONE][0][0] * 255.0f + 0.5f);
	if (d->atest != GFX_ATEST_NONE && !u->tex_alpha && d->k[GFX_K_ONE][0][3] * 255.0f + 0.5f < (float)d->aref)
		return false;
	u->x0 = d->scissor[0] / 4.0f;
	u->y0 = d->scissor[1] / 4.0f;
	u->x1 = d->scissor[2] / 4.0f;
	u->y1 = d->scissor[3] / 4.0f;
	if (u->x1 > (float)width)
		u->x1 = (float)width;
	return true;
}

/* Camino genérico: un triángulo con baricéntricas en coma flotante, igual
 * que draw_triangle y shade del host (con w = 1). Caro en el PPE (~1 µs
 * por píxel: cinco divisiones, dos floorf y dos módulos por píxel); solo
 * para lo que no es un rectángulo 1:1. */
static uint32_t draw_tri(uint8_t *rdram, uint32_t rdram_size, uint32_t addr, uint32_t width, const struct gfx_draw *d,
			 const struct gfx_vertex *a, const struct l8_setup *u)
{
	const struct gfx_tex *t = u->t;
	float sx[3] = { a[0].x, a[1].x, a[2].x }, sy[3] = { a[0].y, a[1].y, a[2].y };
	float area = edge(sx[0], sy[0], sx[1], sy[1], sx[2], sy[2]);
	if (area == 0.0f)
		return 0;
	if ((d->cull == GFX_CULL_BACK && area > 0.0f) || (d->cull == GFX_CULL_FRONT && area < 0.0f))
		return 0;
	float minx = fminf(fminf(sx[0], sx[1]), sx[2]), maxx = fmaxf(fmaxf(sx[0], sx[1]), sx[2]);
	float miny = fminf(fminf(sy[0], sy[1]), sy[2]), maxy = fmaxf(fmaxf(sy[0], sy[1]), sy[2]);
	int px0 = (int)floorf(fmaxf(minx, u->x0)), px1 = (int)ceilf(fminf(maxx, u->x1));
	int py0 = (int)floorf(fmaxf(miny, u->y0)), py1 = (int)ceilf(fminf(maxy, u->y1));
	if (px0 < 0)
		px0 = 0;
	if (py0 < 0)
		py0 = 0;
	if (px1 > (int)width)
		px1 = (int)width;
	uint32_t written = 0;
	for (int y = py0; y < py1; y++) {
		uint32_t row = addr + (uint32_t)y * width;
		if (row + width > rdram_size)
			break;
		for (int x = px0; x < px1; x++) {
			float cx = x + 0.5f, cy = y + 0.5f;
			if (cx < u->x0 || cx >= u->x1 || cy < u->y0 || cy >= u->y1)
				continue;
			float b0 = edge(sx[1], sy[1], sx[2], sy[2], cx, cy) / area;
			float b1 = edge(sx[2], sy[2], sx[0], sy[0], cx, cy) / area;
			float b2 = 1.0f - b0 - b1;
			if (b0 < 0.0f || b1 < 0.0f || b2 < 0.0f)
				continue;
			uint8_t red = u->red_const;
			if (t) {
				/* Como shade() del host con w = 1: la interpolación
				 * con perspectiva se reduce a las baricéntricas. */
				float pw = b0 + b1 + b2, p0 = b0 / pw, p1 = b1 / pw, p2 = b2 / pw;
				float s = p0 * a[0].s + p1 * a[1].s + p2 * a[2].s;
				float tt = p0 * a[0].t + p1 * a[1].t + p2 * a[2].t;
				float uu = s * t->xf[0] + t->xf[2], vv = tt * t->xf[1] + t->xf[3];
				int tx = wrap((int)floorf(uu * t->width), t->width, GFX_TEX_WRAP_S(t->flags));
				int ty = wrap((int)floorf(vv * t->height), t->height, GFX_TEX_WRAP_T(t->flags));
				const uint8_t *p = u->tp + (uint32_t)ty * t->pitch + 4u * (uint32_t)tx;
				if (u->tex_alpha && p[3] < d->aref)
					continue;
				if (u->tex_red)
					red = p[0];
			}
			rdram[row + (uint32_t)x] = red;
			written++;
		}
	}
	return written;
}

/* ¿Los seis vértices (dos triángulos) forman un rectángulo alineado con
 * los ejes, con s función solo de x y t solo de y? Deja las esquinas. */
static bool rect_of(const struct gfx_vertex *a, float *xa, float *ya, float *xb, float *yb, float *sa, float *sb,
		    float *ta, float *tb)
{
	float xs[2], ys[2];
	int nx = 0, ny = 0;
	for (int i = 0; i < 6; i++) {
		int k = 0;
		while (k < nx && xs[k] != a[i].x)
			k++;
		if (k == nx) {
			if (nx == 2)
				return false;
			xs[nx++] = a[i].x;
		}
		k = 0;
		while (k < ny && ys[k] != a[i].y)
			k++;
		if (k == ny) {
			if (ny == 2)
				return false;
			ys[ny++] = a[i].y;
		}
	}
	if (nx != 2 || ny != 2)
		return false;
	*xa = fminf(xs[0], xs[1]);
	*xb = fmaxf(xs[0], xs[1]);
	*ya = fminf(ys[0], ys[1]);
	*yb = fmaxf(ys[0], ys[1]);
	/* Los dos triángulos cubren el rectángulo entero: cada uno con las
	 * cuatro esquinas presentes entre los dos y área no nula; y s, t
	 * consistentes por columna y fila. */
	bool seen[4] = { false, false, false, false };
	float s_at[2] = { 0, 0 }, t_at[2] = { 0, 0 };
	bool s_set[2] = { false, false }, t_set[2] = { false, false };
	for (int i = 0; i < 6; i++) {
		int cx = a[i].x == *xa ? 0 : 1, cy = a[i].y == *ya ? 0 : 1;
		seen[cx + 2 * cy] = true;
		if (s_set[cx] ? s_at[cx] != a[i].s : (s_at[cx] = a[i].s, s_set[cx] = true, false))
			return false;
		if (t_set[cy] ? t_at[cy] != a[i].t : (t_at[cy] = a[i].t, t_set[cy] = true, false))
			return false;
	}
	if (!(seen[0] && seen[1] && seen[2] && seen[3]))
		return false;
	for (int k = 0; k < 6; k += 3)
		if (edge(a[k].x, a[k].y, a[k + 1].x, a[k + 1].y, a[k + 2].x, a[k + 2].y) == 0.0f)
			return false;
	/* Las dos diagonales posibles: los dos triángulos tienen que ser los
	 * dos lados de la misma diagonal (tres esquinas distintas cada uno y
	 * no las mismas tres). */
	for (int k = 0; k < 6; k += 3) {
		int c[3];
		for (int i = 0; i < 3; i++)
			c[i] = (a[k + i].x == *xa ? 0 : 1) + 2 * (a[k + i].y == *ya ? 0 : 1);
		if (c[0] == c[1] || c[1] == c[2] || c[0] == c[2])
			return false;
	}
	int miss0 = 6, miss1 = 6;
	for (int k = 0; k < 2; k++) {
		int sum = 0;
		for (int i = 0; i < 3; i++)
			sum += (a[3 * k + i].x == *xa ? 0 : 1) + 2 * (a[3 * k + i].y == *ya ? 0 : 1);
		if (k)  /* la esquina que no está */
			miss1 = 6 - sum;
		else
			miss0 = 6 - sum;
	}
	if (miss0 == miss1 || miss0 + miss1 != 3)
		return false;
	*sa = s_at[0];
	*sb = s_at[1];
	*ta = t_at[0];
	*tb = t_at[1];
	return true;
}

/* Camino rápido: rectángulo con la textura a 1:1 (un texel por píxel,
 * muestras en el centro del texel): copia de índices por fila con enteros.
 * Exacto frente al genérico porque las muestras caen a medio texel de
 * cualquier borde (frac en [0,25, 0,75]) y el paso es 1 texel por píxel:
 * ningún redondeo de coma flotante cambia el texel. Devuelve false si no
 * es ese caso (el genérico lo dibuja). */
static bool draw_rect_1to1(uint8_t *rdram, uint32_t rdram_size, uint32_t addr, uint32_t width,
			   const struct gfx_draw *d, const struct gfx_vertex *a, const struct l8_setup *u,
			   uint32_t *written)
{
	float xa, ya, xb, yb, sa, sb, ta, tb;
	if (d->cull != GFX_CULL_NONE || !rect_of(a, &xa, &ya, &xb, &yb, &sa, &sb, &ta, &tb))
		return false;
	/* Centros dentro del rectángulo (bordes inclusive, como las
	 * baricéntricas ≥ 0) y de las tijeras [x0, x1). */
	int px0 = (int)ceilf(xa - 0.5f), px1 = (int)floorf(xb - 0.5f) + 1;
	int py0 = (int)ceilf(ya - 0.5f), py1 = (int)floorf(yb - 0.5f) + 1;
	int sx0 = (int)ceilf(u->x0 - 0.5f), sx1 = (int)ceilf(u->x1 - 0.5f);
	int sy0 = (int)ceilf(u->y0 - 0.5f), sy1 = (int)ceilf(u->y1 - 0.5f);
	if (px0 < sx0)
		px0 = sx0;
	if (px1 > sx1)
		px1 = sx1;
	if (py0 < sy0)
		py0 = sy0;
	if (py1 > sy1)
		py1 = sy1;
	if (px0 < 0)
		px0 = 0;
	if (py0 < 0)
		py0 = 0;
	if (px1 > (int)width)
		px1 = (int)width;
	if (px1 <= px0 || py1 <= py0)
		return true;  /* nada que dibujar, y nada que el genérico dibujaría */
	const struct gfx_tex *t = u->t;
	int tx0 = 0, ty0 = 0;
	if (t) {
		float dsdx = (sb - sa) / (xb - xa), dtdy = (tb - ta) / (yb - ya);
		float ux = (sa + (px0 + 0.5f - xa) * dsdx) * t->xf[0] + t->xf[2];
		float vy = (ta + (py0 + 0.5f - ya) * dtdy) * t->xf[1] + t->xf[3];
		float ufx = ux * t->width, vfy = vy * t->height;
		float stepx = dsdx * t->xf[0] * t->width, stepy = dtdy * t->xf[1] * t->height;
		float fx = ufx - floorf(ufx), fy = vfy - floorf(vfy);
		if (fabsf(stepx - 1.0f) > 1e-4f || fabsf(stepy - 1.0f) > 1e-4f || fx < 0.25f || fx > 0.75f || fy < 0.25f ||
		    fy > 0.75f)
			return false;
		/* Con el paso 1 y la muestra a medio texel, el texel de la última
		 * columna sale de sumar; si la coma flotante dijera otra cosa en el
		 * extremo, al genérico. */
		float ulast = (sa + (px1 - 1 + 0.5f - xa) * dsdx) * t->xf[0] + t->xf[2];
		float vlast = (ta + (py1 - 1 + 0.5f - ya) * dtdy) * t->xf[1] + t->xf[3];
		if ((int)floorf(ulast * t->width) != (int)floorf(ufx) + (px1 - 1 - px0) ||
		    (int)floorf(vlast * t->height) != (int)floorf(vfy) + (py1 - 1 - py0))
			return false;
		tx0 = (int)floorf(ufx);
		ty0 = (int)floorf(vfy);
	}
	unsigned ws = t ? GFX_TEX_WRAP_S(t->flags) : 0, wt = t ? GFX_TEX_WRAP_T(t->flags) : 0;
	int n = px1 - px0;
	bool inside_s = t && tx0 >= 0 && tx0 + n <= (int)t->width;
	uint32_t w = 0;
	for (int y = py0; y < py1; y++) {
		uint32_t row = addr + (uint32_t)y * width;
		if (row + width > rdram_size)
			break;
		uint8_t *dst = rdram + row + (uint32_t)px0;
		if (!t) {
			memset(dst, u->red_const, (size_t)n);
			w += (uint32_t)n;
			continue;
		}
		int ty = wrap(ty0 + (y - py0), t->height, wt);
		const uint8_t *src = u->tp + (uint32_t)ty * t->pitch;
		if (inside_s) {
			const uint8_t *p = src + 4u * (uint32_t)tx0;
			if (!u->tex_alpha) {
				if (u->tex_red)
					for (int i = 0; i < n; i++)
						dst[i] = p[4 * i];
				else
					memset(dst, u->red_const, (size_t)n);
				w += (uint32_t)n;
			} else {
				uint8_t aref = d->aref, red = u->red_const;
				for (int i = 0; i < n; i++) {
					if (p[4 * i + 3] < aref)
						continue;
					dst[i] = u->tex_red ? p[4 * i] : red;
					w++;
				}
			}
			continue;
		}
		for (int i = 0; i < n; i++) {
			const uint8_t *p = src + 4u * (uint32_t)wrap(tx0 + i, t->width, ws);
			if (u->tex_alpha && p[3] < d->aref)
				continue;
			dst[i] = u->tex_red ? p[0] : u->red_const;
			w++;
		}
	}
	*written = w;
	return true;
}

uint32_t layer8_draw(uint8_t *rdram, uint32_t rdram_size, uint32_t addr, uint32_t width, const struct gfx_draw *d,
		     const struct gfx_vertex *v, const uint8_t *arena, const uint8_t *cache, struct layer8_result *out)
{
	struct l8_setup u;
	if (!setup(d, width, arena, cache, &u))
		return 0;
	uint32_t written = 0;
	for (uint32_t k = 0; k + 3 <= d->h.b; k += 3) {
		uint64_t t0 = layer8_clock ? layer8_clock() : 0;
		uint32_t w = 0;
		if (layer8_fast && k + 6 <= d->h.b && draw_rect_1to1(rdram, rdram_size, addr, width, d, v + k, &u, &w)) {
			k += 3;
			if (out) {
				out->fast_pixels += w;
				out->fast_ticks += layer8_clock ? layer8_clock() - t0 : 0;
			}
		} else {
			w = draw_tri(rdram, rdram_size, addr, width, d, v + k, &u);
			if (out) {
				out->slow_pixels += w;
				out->slow_ticks += layer8_clock ? layer8_clock() - t0 : 0;
			}
		}
		written += w;
	}
	return written;
}

void layer8_run(uint8_t *rdram, uint32_t rdram_size, const uint8_t *cmd, uint32_t cmd_bytes,
		const struct gfx_vertex *vtx, const uint8_t *arena, const uint8_t *cache, uint32_t min_width,
		struct layer8_result *out)
{
	memset(out, 0, sizeof *out);
	/* Primera pasada: por destino, todos sus dibujos de la tarea caben o
	 * ninguno (un tramo en la RDRAM y otro en el RSX dejaría la capa a
	 * medias en los dos lados). */
	uint32_t addr[LAYER8_MAX], width[LAYER8_MAX], n = 0;
	bool ok[LAYER8_MAX], any[LAYER8_MAX];
	uint32_t off = 0;
	int cur = -1;
	while (off + sizeof(struct gfx_cmd) <= cmd_bytes) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(cmd + off);
		if (c->op == GFX_OP_END || c->size < sizeof *c)
			break;
		off += c->size;
		if (c->op == GFX_OP_TARGET) {
			cur = -1;
			if ((c->c & 0xFF) != 1u /* G_IM_SIZ_8b */ || c->b <= min_width || c->b > 640 || !c->a)
				continue;
			uint32_t i = 0;
			while (i < n && !(addr[i] == c->a && width[i] == c->b))
				i++;
			if (i == n) {
				if (n == LAYER8_MAX) {
					out->refused++;
					continue;
				}
				addr[n] = c->a;
				width[n] = c->b;
				ok[n] = true;
				any[n] = false;
				n++;
			}
			cur = (int)i;
		} else if (c->op == GFX_OP_DRAW && cur >= 0) {
			any[cur] = true;
			if (c->size < sizeof(struct gfx_draw) ||
			    !layer8_draw_ok((const struct gfx_draw *)c, vtx + ((const struct gfx_draw *)c)->h.a))
				ok[cur] = false;
		}
	}
	for (uint32_t i = 0; i < n; i++) {
		if (!any[i])
			ok[i] = false;
		else if (!ok[i])
			out->refused++;
	}
	/* Segunda: dibujar, en el orden de la tarea. */
	off = 0;
	cur = -1;
	while (off + sizeof(struct gfx_cmd) <= cmd_bytes) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(cmd + off);
		if (c->op == GFX_OP_END || c->size < sizeof *c)
			break;
		off += c->size;
		if (c->op == GFX_OP_TARGET) {
			cur = -1;
			for (uint32_t i = 0; i < n; i++)
				if (ok[i] && addr[i] == c->a && width[i] == c->b)
					cur = (int)i;
		} else if (c->op == GFX_OP_DRAW && cur >= 0) {
			const struct gfx_draw *d = (const struct gfx_draw *)c;
			out->pixels += layer8_draw(rdram, rdram_size, addr[cur], width[cur], d, vtx + d->h.a, arena, cache, out);
			out->draws++;
		}
	}
	for (uint32_t i = 0; i < n; i++)
		if (ok[i]) {
			out->addr[out->n] = addr[i];
			out->width[out->n] = width[i];
			out->n++;
		}
}
