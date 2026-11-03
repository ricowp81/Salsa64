/*
 * Salsa64 — imágenes chicas de 32 bits dibujadas en el PPE (mask32.h).
 * Portable: el PPE, el rasterizador del host y las pruebas.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "rdp/mask32.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define WIN_BYTES (MASK32_W * MASK32_H * 4u)

/* Una imagen: su memoria de la RDRAM (la vista de 32 bits y la de 8 bits
 * comparten bytes, como en la consola). */
struct img {
	uint32_t addr, width;
	uint32_t rows32, rows8;  /* hasta dónde llegan las tijeras de los dibujos de cada vista */
	bool ok, any32, any8;
	uint8_t win[WIN_BYTES];
};
static struct img imgs[MASK32_MAX];
/* Lo que un dibujo lee de la imagen, copiado al empezarlo: la consola lo
 * carga a la TMEM antes de dibujar, así que lo que el dibujo escribe en la
 * misma memoria no cambia sus texels. */
static uint8_t snap[2][WIN_BYTES];

/* Lo último que fue a la RDRAM, para reponerlo (ver mask32.h): dirección,
 * bytes, tareas desde entonces y su contenido. */
static struct {
	uint32_t addr, bytes, age;
	uint8_t data[WIN_BYTES];
} keep[MASK32_MAX];
static uint32_t nkeep;

void mask32_forget(void)
{
	nkeep = 0;
}

/* ¿Algún relleno de la tarea (GFX_OP_ZCLEAR: va a la RDRAM, como el RDP)
 * tocó [lo, hi)? */
static bool filled(const uint8_t *cmd, uint32_t cmd_bytes, uint32_t lo, uint32_t hi)
{
	static const uint8_t bpp[4] = { 0, 1, 2, 4 };
	for (uint32_t off = 0; off + sizeof(struct gfx_cmd) <= cmd_bytes;) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(cmd + off);
		if (c->op == GFX_OP_END || c->size < sizeof *c)
			break;
		if (c->op == GFX_OP_ZCLEAR && c->size >= sizeof(struct gfx_zclear)) {
			const struct gfx_zclear *z = (const struct gfx_zclear *)c;
			uint32_t width = c->c & 0xFFFF, b = bpp[(c->c >> 16) & 3];
			uint32_t x1 = z->rect[2] < width ? z->rect[2] : width;
			if (b && z->rect[0] < x1 && z->rect[1] < z->rect[3]) {
				uint32_t a = c->a & 0x00FFFFFF;
				uint32_t flo = a + (z->rect[1] * width + z->rect[0]) * b;
				uint32_t fhi = a + ((z->rect[3] - 1u) * width + x1) * b;
				if (flo < hi && lo < fhi)
					return true;
			}
		}
		off += c->size;
	}
	return false;
}

static inline float clamp01(float v)
{
	return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

/* Como raster.c (channel sin q5): de 0 a 1 a 8 bits. */
static inline uint8_t channel(float v)
{
	return (uint8_t)(uint32_t)(v * 255.0f + 0.5f);
}

/* Como raster.c (wrap). */
static inline int wrap(int i, int n, unsigned mode)
{
	if (mode == GFX_WRAP_REPEAT) {
		i %= n;
		return i < 0 ? i + n : i;
	}
	if (mode == GFX_WRAP_MIRROR) {
		int p = 2 * n;
		i %= p;
		if (i < 0)
			i += p;
		return i < n ? i : p - 1 - i;
	}
	if (mode == GFX_WRAP_MIRROR_ONCE && i < 0)
		i = -1 - i;
	return i < 0 ? 0 : i >= n ? n - 1 : i;
}

/* ¿La textura sale de la memoria de la imagen? Entonces *bpp es la vista
 * que lee (4: RGBA32, 1: I8; 0: otra, que aquí no se sabe leer) y (x0, y0)
 * su texel (0, 0) dentro de esa vista. */
static bool from_image(const struct gfx_tex *t, uint32_t addr, uint32_t width, uint32_t *bpp, uint32_t *x0,
		       uint32_t *y0)
{
	if (!t->src_addr || t->src_addr < addr || t->src_addr - addr >= width * 4u * MASK32_H)
		return false;
	uint32_t b = 0;
	if (t->src_siz == 3u && t->src_fmt == 0u /* G_IM_FMT_RGBA */)
		b = 4;
	else if (t->src_siz == 1u && t->src_fmt == 4u /* G_IM_FMT_I */)
		b = 1;
	uint32_t off = t->src_addr - addr;
	if (!b || t->src_pitch != width || off % b) {
		*bpp = 0;
		return true;
	}
	*bpp = b;
	*y0 = off / (width * b);
	*x0 = off % (width * b) / b;
	return true;
}

bool mask32_draw_ok(const struct gfx_draw *d, const struct gfx_vertex *v, uint32_t addr, uint32_t width)
{
	if (d->h.b < 3 || d->h.b % 3 != 0 || d->depth != 0 || d->fog || d->viscvg || d->ntex > 2 ||
	    (d->blend != GFX_BLEND_OFF && d->blend != GFX_BLEND_ALPHA && d->blend != GFX_BLEND_ADD))
		return false;
	/* Las tijeras dentro de la memoria que se sigue (MASK32_H filas). */
	if ((d->scissor[3] + 3u) / 4u > MASK32_H)
		return false;
	for (int i = 0; i < d->ntex; i++) {
		const struct gfx_tex *t = &d->tex[i];
		uint32_t bpp, x0, y0;
		if (!t->width || !t->height)
			return false;
		if (from_image(t, addr, width, &bpp, &x0, &y0)) {
			if (!bpp || (t->flags & (GFX_TEX_ONCE_S | GFX_TEX_ONCE_T)))
				return false;
		} else if (t->where == GFX_TEX_TARGET) {
			return false;  /* la leería el consumidor de un destino del RSX */
		}
	}
	for (uint32_t k = 0; k < d->h.b; k++)
		if (v[k].w != 1.0f)
			return false;
	return true;
}

/* De dónde lee cada textura del dibujo. */
struct src {
	const struct gfx_tex *t;
	const uint8_t *data;     /* texels RGBA8888 de la HLE */
	const uint8_t *win;      /* o la memoria de la imagen (data = NULL), copiada al empezar el dibujo */
	uint32_t width, bpp, x0, y0;
};

static void fetch(const struct src *s, int x, int y, float out[4])
{
	const struct gfx_tex *t = s->t;
	x = wrap(x, t->width, GFX_TEX_WRAP_S(t->flags));
	y = wrap(y, t->height, GFX_TEX_WRAP_T(t->flags));
	if (s->data) {
		const uint8_t *p = s->data + (uint32_t)y * t->pitch + 4u * (uint32_t)x;
		for (int k = 0; k < 4; k++)
			out[k] = p[k] / 255.0f;
		return;
	}
	uint32_t w = s->width, h = WIN_BYTES / (w * s->bpp);
	uint32_t px = s->x0 + (uint32_t)x, py = s->y0 + (uint32_t)y;
	px = px < w ? px : w - 1;
	py = py < h ? py : h - 1;
	const uint8_t *p = s->win + (py * w + px) * s->bpp;
	if (s->bpp == 4) {
		for (int k = 0; k < 4; k++)
			out[k] = p[k] / 255.0f;
	} else {
		out[0] = out[1] = out[2] = out[3] = p[0] / 255.0f;
	}
}

/* Como raster.c (sample): u, v normalizados, centros en (i + 0,5) / tamaño. */
static void sample(const struct src *s, float u, float v, float out[4])
{
	const struct gfx_tex *t = s->t;
	float x = u * t->width, y = v * t->height;
	if (!(t->flags & GFX_TEX_BILINEAR)) {
		fetch(s, (int)floorf(x), (int)floorf(y), out);
		return;
	}
	x -= 0.5f;
	y -= 0.5f;
	int x0 = (int)floorf(x), y0 = (int)floorf(y);
	float fx = x - (float)x0, fy = y - (float)y0, a[4], b[4], c[4], e[4];
	fetch(s, x0, y0, a);
	fetch(s, x0 + 1, y0, b);
	fetch(s, x0, y0 + 1, c);
	fetch(s, x0 + 1, y0 + 1, e);
	for (int k = 0; k < 4; k++)
		out[k] = (a[k] * (1 - fx) + b[k] * fx) * (1 - fy) + (c[k] * (1 - fx) + e[k] * fx) * fy;
}

/* Plano de un atributo: f(x, y) = f0 + fx · (x − x0) + fy · (y − y0). */
struct plane {
	float f0, fx, fy;
};

static void plane(struct plane *p, const float *xs, const float *ys, float f0, float f1, float f2, float det)
{
	p->f0 = f0;
	p->fx = ((f1 - f0) * (ys[2] - ys[0]) - (f2 - f0) * (ys[1] - ys[0])) / det;
	p->fy = ((f2 - f0) * (xs[1] - xs[0]) - (f1 - f0) * (xs[2] - xs[0])) / det;
}

static inline float at(const struct plane *p, float dx, float dy)
{
	return p->f0 + p->fx * dx + p->fy * dy;
}

/* El combiner, la prueba de alfa y la mezcla de un píxel, como shade de
 * raster.c (sin z ni niebla). false si la prueba de alfa lo descarta. */
static bool shade(const struct gfx_draw *d, const struct src src[2], const float c[4], float s, float t,
		  const float dst[3], float out[4])
{
	float tx[2][4] = { { 1, 1, 1, 1 }, { 1, 1, 1, 1 } };
	for (int i = 0; i < d->ntex && i < 2; i++) {
		const struct gfx_tex *te = src[i].t;
		sample(&src[i], s * te->xf[0] + te->xf[2], t * te->xf[1] + te->xf[3], tx[i]);
	}
	float X[GFX_K_TERMS][4];
	for (int k = 0; k < GFX_K_TERMS; k++)
		for (int l = 0; l < 4; l++)
			X[k][l] = d->k[k][0][l] + d->k[k][1][l] * c[l] + d->k[k][2][l] * c[3];
	if (d->nterms) {
		for (int l = 0; l < 4; l++) {
			float sum = 0.0f;
			for (int k = 0; k < d->nterms && k < GFX_K_TERMS; k++) {
				float p = X[k][l];
				for (int f = 0; f < GFX_K_FACTORS && d->factors[k][f]; f++)
					switch (d->factors[k][f]) {
					case GFX_F_T0: p *= tx[0][l]; break;
					case GFX_F_T0A: p *= tx[0][3]; break;
					case GFX_F_T1: p *= tx[1][l]; break;
					case GFX_F_T1A: p *= tx[1][3]; break;
					case GFX_F_C: p *= c[l]; break;
					default: p *= c[3]; break;
					}
				sum += p;
			}
			out[l] = clamp01(sum);
		}
	} else {
		for (int l = 0; l < 4; l++)
			out[l] = clamp01(tx[0][l] * X[GFX_K_T0][l] + tx[0][3] * X[GFX_K_T0A][l] + tx[1][l] * X[GFX_K_T1][l] +
					 tx[1][3] * X[GFX_K_T1A][l] + X[GFX_K_ONE][l]);
	}
	if (d->atest == GFX_ATEST_GEQUAL && out[3] * 255.0f + 0.5f < d->aref)
		return false;
	for (int l = 0; l < 3; l++) {
		if (d->blend == GFX_BLEND_ALPHA)
			out[l] = out[l] * out[3] + dst[l] * (1.0f - out[3]);
		else if (d->blend == GFX_BLEND_ADD)
			out[l] = clamp01(out[l] * out[3] + dst[l]);
	}
	return true;
}

bool mask32_fast = true;

/* ¿Cabe en el camino entero? Combinación lineal (sin términos), el mismo
 * color en los tres vértices y texturas sin filtro: los X del combiner son
 * constantes del triángulo y el texel sale de coordenadas en punto fijo.
 * En el PPE cada conversión entre coma flotante y entero pasa por la
 * memoria (fctiwz, stfd, lwz: ~40 ciclos de espera); este camino no hace
 * ninguna por píxel. */
static bool fast_ok(const struct gfx_draw *d, const struct gfx_vertex *v, const struct src src[2])
{
	if (d->nterms)
		return false;
	if (memcmp(&v[0].r, &v[1].r, 4) != 0 || memcmp(&v[0].r, &v[2].r, 4) != 0)
		return false;
	for (int i = 0; i < d->ntex && i < 2; i++)
		if (src[i].t->flags & GFX_TEX_BILINEAR)
			return false;
	return true;
}

static inline int iwrap(int i, int n, unsigned mode)
{
	if (mode == GFX_WRAP_REPEAT) {
		if (!(n & (n - 1)))
			return i & (n - 1);
		i %= n;
		return i < 0 ? i + n : i;
	}
	if (mode == GFX_WRAP_MIRROR) {
		int p = 2 * n, m = !(p & (p - 1)) ? i & (p - 1) : ((i % p) + p) % p;
		return m < n ? m : p - 1 - m;
	}
	if (mode == GFX_WRAP_MIRROR_ONCE && i < 0)
		i = -1 - i;
	return i < 0 ? 0 : i >= n ? n - 1 : i;
}

/* El texel (x, y) en enteros de 0 a 255. */
static inline void ifetch(const struct src *s, int x, int y, uint32_t out[4])
{
	const struct gfx_tex *t = s->t;
	x = iwrap(x, t->width, GFX_TEX_WRAP_S(t->flags));
	y = iwrap(y, t->height, GFX_TEX_WRAP_T(t->flags));
	const uint8_t *p;
	if (s->data) {
		p = s->data + (uint32_t)y * t->pitch + 4u * (uint32_t)x;
	} else {
		uint32_t w = s->width, h = WIN_BYTES / (w * s->bpp);
		uint32_t px = s->x0 + (uint32_t)x, py = s->y0 + (uint32_t)y;
		px = px < w ? px : w - 1;
		py = py < h ? py : h - 1;
		p = s->win + (py * w + px) * s->bpp;
		if (s->bpp == 1) {
			out[0] = out[1] = out[2] = out[3] = p[0];
			return;
		}
	}
	out[0] = p[0];
	out[1] = p[1];
	out[2] = p[2];
	out[3] = p[3];
}

static inline uint32_t div255(uint32_t v)
{
	return (v + 1u + (v >> 8)) >> 8;
}

static uint32_t draw_fast(struct img *im, uint32_t bpp, const struct gfx_draw *d, const struct gfx_vertex *v,
			  const struct src src[2], const float ea[3], const float eb[3], const float ec[3],
			  const bool incl[3], const struct plane *ps, const struct plane *pt, float x0v, float y0v,
			  int px0, int px1, int py0, int py1)
{
	/* El combiner: X constantes del triángulo, en 8.8, y por canal la lista
	 * de los términos que no son cero (en una imagen de 8 bits basta el rojo
	 * y, si hay mezcla o prueba de alfa, el alfa). Término: 0 T0, 1 T0a,
	 * 2 T1, 3 T1a; la constante va aparte. */
	float c[4] = { v[0].r / 255.0f, v[0].g / 255.0f, v[0].b / 255.0f, v[0].a / 255.0f };
	struct { uint8_t n, tex[4], chan[4]; int32_t k[4]; int32_t one; } ch[4];
	bool need_a = d->blend != GFX_BLEND_OFF || d->atest == GFX_ATEST_GEQUAL || bpp == 4;
	for (int l = 0; l < 4; l++) {
		ch[l].n = 0;
		ch[l].one = (int32_t)lrintf((d->k[GFX_K_ONE][0][l] + d->k[GFX_K_ONE][1][l] * c[l] +
					     d->k[GFX_K_ONE][2][l] * c[3]) * 256.0f) * 255 + 128;
		if (bpp == 1 && (l == 1 || l == 2))
			continue;
		if (l == 3 && !need_a)
			continue;
		for (int k = 0; k < 4; k++) {
			int32_t kk = (int32_t)lrintf((d->k[k][0][l] + d->k[k][1][l] * c[l] + d->k[k][2][l] * c[3]) * 256.0f);
			if (!kk || k / 2 >= d->ntex)
				continue;
			ch[l].tex[ch[l].n] = (uint8_t)(k / 2);
			ch[l].chan[ch[l].n] = (uint8_t)(k & 1 ? 3 : l);
			ch[l].k[ch[l].n] = kk;
			ch[l].n++;
		}
	}
	/* Coordenadas del texel: afines en (x, y), en 16.16; un paso por píxel. */
	int ntex = d->ntex < 2 ? d->ntex : 2;
	float ua[2] = { 0, 0 }, ub[2] = { 0, 0 }, uc[2] = { 0, 0 }, va[2] = { 0, 0 }, vb[2] = { 0, 0 }, vc[2] = { 0, 0 };
	int32_t du[2] = { 0, 0 }, dv[2] = { 0, 0 };
	for (int i = 0; i < ntex; i++) {
		const struct gfx_tex *t = src[i].t;
		float sw = t->xf[0] * t->width, th = t->xf[1] * t->height;
		ua[i] = ps->fx * sw;
		ub[i] = ps->fy * sw;
		uc[i] = (ps->f0 - ps->fx * x0v - ps->fy * y0v) * sw + t->xf[2] * t->width;
		va[i] = pt->fx * th;
		vb[i] = pt->fy * th;
		vc[i] = (pt->f0 - pt->fx * x0v - pt->fy * y0v) * th + t->xf[3] * t->height;
		du[i] = (int32_t)lrintf(ua[i] * 65536.0f);
		dv[i] = (int32_t)lrintf(va[i] * 65536.0f);
	}
	/* Las tijeras, en columnas y filas enteras (centro dentro de [x0, x1)). */
	float sx0 = d->scissor[0] / 4.0f, sy0 = d->scissor[1] / 4.0f;
	float sx1 = d->scissor[2] / 4.0f, sy1 = d->scissor[3] / 4.0f;
	int cx0 = (int)ceilf(sx0 - 0.5f), cx1 = (int)ceilf(sx1 - 0.5f);
	int cy0 = (int)ceilf(sy0 - 0.5f), cy1 = (int)ceilf(sy1 - 0.5f);
	px0 = px0 > cx0 ? px0 : cx0;
	px1 = px1 < cx1 ? px1 : cx1;
	py0 = py0 > cy0 ? py0 : cy0;
	py1 = py1 < cy1 ? py1 : cy1;
	uint32_t aref = d->atest == GFX_ATEST_GEQUAL ? d->aref : 0;
	uint32_t written = 0;
	for (int y = py0; y < py1; y++) {
		float cy = (float)y + 0.5f, cx = (float)px0 + 0.5f;
		float row[3];
		for (int i = 0; i < 3; i++)
			row[i] = eb[i] * cy + ec[i];
		int32_t U[2], V[2];
		for (int i = 0; i < ntex; i++) {
			U[i] = (int32_t)floorf((ua[i] * cx + ub[i] * cy + uc[i]) * 65536.0f);
			V[i] = (int32_t)floorf((va[i] * cx + vb[i] * cy + vc[i]) * 65536.0f);
		}
		uint8_t *p = im->win + ((uint32_t)y * im->width + (uint32_t)px0) * bpp;
		for (int x = px0; x < px1; x++, cx += 1.0f, p += bpp) {
			bool in = true;
			for (int i = 0; i < 3 && in; i++) {
				float e = ea[i] * cx + row[i];
				in = e > 0.0f || (e == 0.0f && incl[i]);
			}
			uint32_t tx[2][4] = { { 255, 255, 255, 255 }, { 255, 255, 255, 255 } };
			if (in)
				for (int i = 0; i < ntex; i++)
					ifetch(&src[i], U[i] >> 16, V[i] >> 16, tx[i]);
			for (int i = 0; i < ntex; i++) {
				U[i] += du[i];
				V[i] += dv[i];
			}
			if (!in)
				continue;
			int32_t o[4] = { 0, 0, 0, 255 };
			for (int l = 0; l < 4; l++) {
				int32_t sum = ch[l].one;
				for (int j = 0; j < ch[l].n; j++) {
					int32_t t = (int32_t)tx[ch[l].tex[j]][ch[l].chan[j]];
					sum += ch[l].k[j] == 256 ? t << 8 : t * ch[l].k[j];
				}
				sum >>= 8;
				o[l] = sum < 0 ? 0 : sum > 255 ? 255 : sum;
			}
			if ((uint32_t)o[3] < aref)
				continue;
			uint32_t a = (uint32_t)o[3];
			if (d->blend != GFX_BLEND_OFF && !(d->blend == GFX_BLEND_ALPHA && a == 255u)) {
				uint32_t dst[3];
				if (bpp == 4) {
					dst[0] = p[0];
					dst[1] = p[1];
					dst[2] = p[2];
				} else {
					dst[0] = dst[1] = dst[2] = p[0];
				}
				for (int l = 0; l < (bpp == 4 ? 3 : 1); l++) {
					if (d->blend == GFX_BLEND_ALPHA) {
						o[l] = (int32_t)div255((uint32_t)o[l] * a + dst[l] * (255u - a) + 127u);
					} else {
						uint32_t t = div255((uint32_t)o[l] * a + 127u) + dst[l];
						o[l] = (int32_t)(t > 255u ? 255u : t);
					}
				}
			}
			if (bpp == 4) {
				p[0] = (uint8_t)o[0];
				p[1] = (uint8_t)o[1];
				p[2] = (uint8_t)o[2];
				p[3] = (uint8_t)o[3];
			} else {
				p[0] = (uint8_t)o[0];
			}
			written++;
		}
	}
	return written;
}

/* Un triángulo (w = 1) sobre la vista `bpp` de la imagen. Muestra en el
 * centro del píxel; un píxel justo sobre una arista lo pinta un solo
 * triángulo (regla arriba-izquierda), para que la mezcla no se aplique dos
 * veces en la diagonal de un rectángulo. */
static uint32_t draw_tri(struct img *im, uint32_t bpp, const struct gfx_draw *d, const struct gfx_vertex *v,
			 const struct src src[2])
{
	float xs[3] = { v[0].x, v[1].x, v[2].x }, ys[3] = { v[0].y, v[1].y, v[2].y };
	float det = (xs[1] - xs[0]) * (ys[2] - ys[0]) - (xs[2] - xs[0]) * (ys[1] - ys[0]);
	if (det == 0.0f)
		return 0;
	/* Como raster.c (tri_setup): con la y hacia abajo, área < 0 es el frente. */
	if ((d->cull == GFX_CULL_BACK && det > 0.0f) || (d->cull == GFX_CULL_FRONT && det < 0.0f))
		return 0;
	float sign = det > 0.0f ? 1.0f : -1.0f;
	/* Aristas normalizadas (dentro: e ≥ 0): la i es la opuesta al vértice i. */
	float ea[3], eb[3], ec[3];
	bool incl[3];
	for (int i = 0; i < 3; i++) {
		int a = (i + 1) % 3, b = (i + 2) % 3;
		ea[i] = -(ys[b] - ys[a]) * sign;
		eb[i] = (xs[b] - xs[a]) * sign;
		ec[i] = -(ea[i] * xs[a] + eb[i] * ys[a]);
		incl[i] = ea[i] > 0.0f || (ea[i] == 0.0f && eb[i] > 0.0f);
	}
	struct plane ps, pt, pc[4];
	plane(&ps, xs, ys, v[0].s, v[1].s, v[2].s, det);
	plane(&pt, xs, ys, v[0].t, v[1].t, v[2].t, det);
	const uint8_t *c0 = &v[0].r, *c1 = &v[1].r, *c2 = &v[2].r;
	for (int k = 0; k < 4; k++)
		plane(&pc[k], xs, ys, c0[k] / 255.0f, c1[k] / 255.0f, c2[k] / 255.0f, det);

	float sx0 = d->scissor[0] / 4.0f, sy0 = d->scissor[1] / 4.0f;
	float sx1 = d->scissor[2] / 4.0f, sy1 = d->scissor[3] / 4.0f;
	float minx = fminf(fminf(xs[0], xs[1]), xs[2]), maxx = fmaxf(fmaxf(xs[0], xs[1]), xs[2]);
	float miny = fminf(fminf(ys[0], ys[1]), ys[2]), maxy = fmaxf(fmaxf(ys[0], ys[1]), ys[2]);
	int px0 = (int)floorf(fmaxf(minx, sx0)), px1 = (int)ceilf(fminf(maxx, sx1));
	int py0 = (int)floorf(fmaxf(miny, sy0)), py1 = (int)ceilf(fminf(maxy, sy1));
	if (px0 < 0)
		px0 = 0;
	if (py0 < 0)
		py0 = 0;
	if (px1 > (int)im->width)
		px1 = (int)im->width;
	if (py1 > MASK32_H)
		py1 = MASK32_H;
	if (mask32_fast && fast_ok(d, v, src))
		return draw_fast(im, bpp, d, v, src, ea, eb, ec, incl, &ps, &pt, xs[0], ys[0], px0, px1, py0, py1);
	uint32_t written = 0;
	for (int y = py0; y < py1; y++) {
		float cy = (float)y + 0.5f;
		if (cy < sy0 || cy >= sy1)
			continue;
		for (int x = px0; x < px1; x++) {
			float cx = (float)x + 0.5f;
			if (cx < sx0 || cx >= sx1)
				continue;
			bool in = true;
			for (int i = 0; i < 3 && in; i++) {
				float e = ea[i] * cx + eb[i] * cy + ec[i];
				in = e > 0.0f || (e == 0.0f && incl[i]);
			}
			if (!in)
				continue;
			float dx = cx - xs[0], dy = cy - ys[0];
			float c[4] = { at(&pc[0], dx, dy), at(&pc[1], dx, dy), at(&pc[2], dx, dy), at(&pc[3], dx, dy) };
			uint8_t *p = im->win + ((uint32_t)y * im->width + (uint32_t)x) * bpp;
			float dst[3], out[4];
			if (bpp == 4) {
				dst[0] = p[0] / 255.0f;
				dst[1] = p[1] / 255.0f;
				dst[2] = p[2] / 255.0f;
			} else {
				dst[0] = dst[1] = dst[2] = p[0] / 255.0f;
			}
			if (!shade(d, src, c, at(&ps, dx, dy), at(&pt, dx, dy), dst, out))
				continue;
			if (bpp == 4) {
				/* La consola guarda la cobertura en el cuarto byte; aquí el
				 * alfa del combiner (como el RSX con alfa en los destinos). */
				p[0] = channel(out[0]);
				p[1] = channel(out[1]);
				p[2] = channel(out[2]);
				p[3] = channel(out[3]);
			} else {
				p[0] = channel(out[0]);  /* en una imagen de 8 bits el RDP escribe el rojo */
			}
			written++;
		}
	}
	return written;
}

static uint32_t draw(struct img *im, uint32_t bpp, const struct gfx_draw *d, const struct gfx_vertex *v,
		     const uint8_t *arena, const uint8_t *cache)
{
	struct src src[2];
	memset(src, 0, sizeof src);
	for (int i = 0; i < d->ntex && i < 2; i++) {
		const struct gfx_tex *t = &d->tex[i];
		src[i].t = t;
		if (from_image(t, im->addr, im->width, &src[i].bpp, &src[i].x0, &src[i].y0)) {
			memcpy(snap[i], im->win, WIN_BYTES);
			src[i].win = snap[i];
			src[i].width = im->width;
			continue;
		}
		const uint8_t *base = t->where == GFX_TEX_CACHE ? cache : arena;
		if (!base)
			return 0;
		src[i].data = base + t->offset;
	}
	uint32_t written = 0;
	for (uint32_t k = 0; k + 3 <= d->h.b; k += 3)
		written += draw_tri(im, bpp, d, v + k, src);
	return written;
}

static int find(uint32_t n, uint32_t addr, uint32_t width)
{
	for (uint32_t i = 0; i < n; i++)
		if (imgs[i].addr == addr && imgs[i].width == width)
			return (int)i;
	return -1;
}

/* ¿Hubo antes de `end` un destino con esta dirección y ancho? (Uno de otro
 * tamaño de píxel ya iría al RSX, y quien traduce no dibuja en el RSX la
 * imagen que se dibuja aquí.) */
static bool seen_before(const uint8_t *cmd, uint32_t end, uint32_t addr, uint32_t width)
{
	for (uint32_t off = 0; off + sizeof(struct gfx_cmd) <= end;) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(cmd + off);
		if (c->op == GFX_OP_END || c->size < sizeof *c)
			break;
		if (off + c->size > end)
			break;
		if (c->op == GFX_OP_TARGET && c->a == addr && c->b == width)
			return true;
		off += c->size;
	}
	return false;
}

void mask32_run(uint8_t *rdram, uint32_t rdram_size, const uint8_t *cmd, uint32_t cmd_bytes,
		const struct gfx_vertex *vtx, const uint8_t *arena, const uint8_t *cache, struct mask32_result *out)
{
	memset(out, 0, sizeof *out);
	if (!rdram)
		return;
	/* Primera pasada: las imágenes y si todos sus dibujos caben. */
	uint32_t n = 0;
	int cur = -1;
	uint32_t view = 0;
	uint32_t off = 0;
	while (off + sizeof(struct gfx_cmd) <= cmd_bytes) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(cmd + off);
		if (c->op == GFX_OP_END || c->size < sizeof *c)
			break;
		if (c->op == GFX_OP_TARGET) {
			uint32_t siz = c->c & 0xFF;
			int i = find(n, c->a, c->b);
			cur = -1;
			if (siz == 3u && c->b && c->b <= MASK32_W && c->a && c->a < rdram_size &&
			    rdram_size - c->a >= WIN_BYTES) {
				if (i < 0) {
					if (n == MASK32_MAX || seen_before(cmd, off, c->a, c->b)) {
						out->refused++;
						off += c->size;
						continue;
					}
					i = (int)n++;
					memset(&imgs[i], 0, offsetof(struct img, win));
					imgs[i].addr = c->a;
					imgs[i].width = c->b;
					imgs[i].ok = true;
				}
				cur = i;
				view = 4;
			} else if (i >= 0) {
				if (siz == 1u) {
					cur = i;
					view = 1;
				} else {
					imgs[i].ok = false;  /* otro tamaño de píxel en la misma imagen */
				}
			}
		} else if (c->op == GFX_OP_DRAW && cur >= 0) {
			struct img *im = &imgs[cur];
			const struct gfx_draw *d = (const struct gfx_draw *)c;
			if (c->size < sizeof *d || !mask32_draw_ok(d, vtx + d->h.a, im->addr, im->width)) {
				im->ok = false;
			} else {
				uint32_t rows = (d->scissor[3] + 3u) / 4u;
				if (view == 4) {
					im->any32 = true;
					im->rows32 = rows > im->rows32 ? rows : im->rows32;
				} else {
					im->any8 = true;
					im->rows8 = rows > im->rows8 ? rows : im->rows8;
				}
			}
		}
		off += c->size;
	}
	for (uint32_t i = 0; i < n; i++) {
		struct img *im = &imgs[i];
		if (im->ok && !im->any32 && !im->any8)
			im->ok = false;  /* nada que dibujar aquí */
		else if (!im->ok)
			out->refused++;
		if (im->ok)
			memcpy(im->win, rdram + im->addr, WIN_BYTES);
	}
	/* Segunda: dibujar, en el orden de la tarea. */
	off = 0;
	cur = -1;
	while (off + sizeof(struct gfx_cmd) <= cmd_bytes) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(cmd + off);
		if (c->op == GFX_OP_END || c->size < sizeof *c)
			break;
		if (c->op == GFX_OP_TARGET) {
			int i = find(n, c->a, c->b);
			cur = i >= 0 && imgs[i].ok ? i : -1;
			view = (c->c & 0xFF) == 3u ? 4 : 1;
		} else if (c->op == GFX_OP_DRAW && cur >= 0) {
			const struct gfx_draw *d = (const struct gfx_draw *)c;
			out->pixels += draw(&imgs[cur], view, d, vtx + d->h.a, arena, cache);
			out->draws++;
		}
		off += c->size;
	}
	/* A la RDRAM, la última vista que se dibujó: la de 8 bits si la hubo. */
	for (uint32_t i = 0; i < n; i++) {
		const struct img *im = &imgs[i];
		if (!im->ok)
			continue;
		uint32_t bytes = im->any8 ? im->width * im->rows8 : im->width * 4u * im->rows32;
		memcpy(rdram + im->addr, im->win, bytes);
		out->bytes += bytes;
		out->addr[out->n] = im->addr;
		out->width[out->n] = im->width;
		if (out->n == 0)
			nkeep = 0;
		keep[nkeep].addr = im->addr;
		keep[nkeep].bytes = bytes;
		keep[nkeep].age = 0;
		memcpy(keep[nkeep].data, im->win, bytes);
		nkeep++;
		out->n++;
	}
	if (out->n)
		return;
	/* Una tarea sin estas imágenes: si un relleno suyo pisó lo último que
	 * fue a la RDRAM (el borrado del z-buffer con que Paper Mario abre cada
	 * cuadro, en una tarea aparte), se repone, hasta MASK32_KEEP tareas. */
	uint32_t live = 0;
	for (uint32_t i = 0; i < nkeep; i++) {
		if (++keep[i].age > MASK32_KEEP)
			continue;
		if (keep[i].addr < rdram_size && rdram_size - keep[i].addr >= keep[i].bytes &&
		    filled(cmd, cmd_bytes, keep[i].addr, keep[i].addr + keep[i].bytes)) {
			memcpy(rdram + keep[i].addr, keep[i].data, keep[i].bytes);
			out->restored += keep[i].bytes;
		}
		keep[live++] = keep[i];
	}
	nkeep = live;
}
