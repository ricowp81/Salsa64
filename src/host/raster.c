/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — rasterizador de referencia en software para la salida de la
 * HLE gráfica (src/rsp/gfx/gfx.h), solo para el host.
 *
 * Hace lo mismo que debe hacer el RSX con esa salida (src/rdp y
 * lib/rsxkit), a la resolución de la N64 y sin buscar velocidad: sirve para
 * ver y verificar en el host lo que produce la HLE (tools/test-roms.sh con
 * gfxtest) y como referencia cuando el RSX muestre otra cosa.
 *
 * Los destinos (qué color image tiene imagen, cuál se recicla) son la misma
 * tabla que usa la PS3, src/rdp/targets.c; el campo de la tabla es aquí el
 * número de tarea. Como en la PS3, una textura cuyo origen (gfx_tex.src_*)
 * cae en un destino dibujado hace poco se lee del destino (render a
 * textura), y un destino se copia a la RDRAM (RGBA5551) al terminar la
 * tarea cuando la política de rdp_targets lo pide: así el host modela la
 * misma conducta antes de medirla en la consola.
 *
 * Simplificaciones: solo se recorta contra w > 0 (lo que está detrás de la
 * cámara, como el RSX; sin los planos de los lados ni el cercano: las
 * tijeras y el z-buffer hacen el resto), muestreo en el centro del píxel,
 * regla de cobertura por arriba a la izquierda aproximada.
 */
#include "raster.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/endian.h"
#include "core/rcp.h"
#include "rdp/mask32.h"
#include "rdp/targets.h"

#define MAX_DEPTHS 4
/* G_IM_SIZ_16b (gbi): color images y texturas de 16 bits. */
#define IM_SIZ_16b 2u

_Static_assert(GFX_FB_FRESH == RDP_TARGETS_FRESH, "la HLE y la tabla cuentan igual la antigüedad de un destino");

struct depth {
	uint32_t addr;
	bool used;
	/* Como rdp.c (zauto): la tarea del último borrado de toda la imagen, la
	 * caja de los borrados de la tarea box_task y la última tarea con el
	 * borrado automático. */
	uint32_t full_task, box_task, auto_task;
	uint16_t box[4];
	float z[RASTER_W * RASTER_H];
};

static struct rdp_targets tg;
static uint32_t rgb[RDP_TARGETS_MAX][RASTER_W * RASTER_H];
/* Alfa en los destinos, como la PS3 (salsa64_rdp_target_alpha): las
 * imágenes de 16 bits que no son del ancho del VI llevan el bit de alfa de
 * RGBA5551 en el byte alto de rgb, al revés para que lo de siempre (0) sea
 * opaco: RGB_TRANSP es alfa 0. Lo escriben como el RSX: nace en 0, un
 * dibujo deja lo que diga su cobertura (gfx_draw.cvg_bit), un relleno su
 * bit, una copia el del texel, el cebado el de la RDRAM; lo leen las
 * texturas de un destino y, con raster_copy_alpha, la copia a la RDRAM. */
#define RGB_TRANSP 0xFF000000u
static bool slot_alpha[RDP_TARGETS_MAX];
bool raster_target_alpha = true;
bool raster_copy_alpha = true;
bool raster_cvg_alpha = true;
bool raster_alpha_all = false;  /* diagnóstico: el bit en todas las de 16 bits (raster.h) */
/* Cómo escribe el alfa el dibujo en curso en un destino con alfa (como
 * rsxkit_draw.dst_alpha en rdp.c): DSTA_ONE, alfa 1; DSTA_FRAG, el del
 * fragmento; DSTA_FILL, el del relleno (fill_alpha); DSTA_KEEP, el del
 * destino; DSTA_MAX, el mayor de los dos. */
enum { DSTA_ONE, DSTA_FRAG, DSTA_FILL, DSTA_KEEP, DSTA_MAX };
static int dst_alpha_mode;
static bool fill_alpha;
static uint32_t scratch[RASTER_W * RASTER_H];  /* copia del ligado cuando un dibujo lo lee */
static int scratch_slot;
static struct rdp_box scratch_dirty;  /* lo dibujado en esa ranura después de copiarla (como rdp.c) */
static struct depth depths[MAX_DEPTHS];
static unsigned next_depth;
static uint32_t task_no;
static uint32_t raster_zauto;  /* borrados enteros de zauto (como rdp.c) */
static uint32_t first_cimg, cimg_task;  /* como rdp.c: la tarea empieza un cuadro si su primera color image es otra */
static bool frame_start;
static uint32_t vi_field;  /* campos del VI con imagen (rdp_targets_present) */

bool raster_trace_fb;
uint32_t raster_field;
int raster_fbfx = 2;
bool raster_q5 = true;
bool raster_aa_edges = false;
bool raster_affine = true;
bool raster_rsx_safe = true;
float raster_edge_width = 1.5f;  /* como rdp.c: 1,5 píxeles de la N64 */
float raster_edge_factor = 8.0f, raster_edge_units = 64.0f;  /* su desplazamiento de polígono (rsxkit) */
static struct rdp_vi_ring vi_ring;
static uint32_t primes, readbacks, mirrors, frames, dma_skipped;
static uint32_t all_draws, edge_draws, edge_tris;  /* dibujos, y los que llevaron aristas */
static uint32_t cvg_draws[4];  /* dibujos de los ciclos 1 y 2 en un destino con alfa, por GFX_CVG_* */
static uint32_t big_copies;    /* copias de imágenes de otro ancho que el VI que no caben en un espejo */
static uint32_t draws8, tex8_rsx, tex8_rdram;    /* dibujos en imágenes de 8 bits; I8 leídas de ellas */
/* Imágenes chicas de 32 bits dibujadas en software (mask32.h, como el PPE
 * en la PS3): tareas, dibujos, píxeles, bytes a la RDRAM y las que quedaron
 * aquí. SALSA64_MASCARA32_RSX=1 las deja al rasterizador, como antes. */
static uint32_t m32_tasks, m32_draws, m32_pixels, m32_bytes, m32_refused;
static int m32_on = -1;
uint32_t ps3_rb, ps3_rb_old;  /* diagnóstico (SALSA64_PS3_RB) */
static struct rdp_dma_ring dma_ring;

void raster_rdram_written(uint32_t pa, uint32_t len, uint32_t task)
{
	rdp_dma_note(&dma_ring, pa, len, task);
}

/* Un DMA escribió desde la tarea `task` en lo que se iba a copiar. */
static bool dma_newer(uint32_t addr, uint32_t bytes, uint32_t task)
{
	if (!rdp_dma_since(&dma_ring, addr, addr + bytes, task))
		return false;
	dma_skipped++;
	if (raster_trace_fb)
		fprintf(stderr, "fb: campo %u tarea %u: copia de %06x no se hace: un DMA escribió ahí después\n", raster_field,
			task_no, addr);
	return true;
}
static uint32_t rb_task[RDP_TARGETS_MAX];  /* última tarea con copia a la RDRAM por ranura */
/* Última tarea con copia pedida (rdp_readback_wanted) por ranura: una que se
 * repite en tareas seguidas va en la PS3 sin esperar y no pasa al CPU. */
static uint32_t rb_wanted_task[RDP_TARGETS_MAX];
#define RB_KEEP_TASKS 32
#define MIRROR_BYTES 65536  /* como rdp.c: tope de una copia espejo (imagen chica) */
#define MIRROR_SLOT  131072 /* como rdp.c: tope de la copia de una ranura que se recicla */
static struct gfx_job last_job;  /* la RDRAM, para el espejo de cada campo */

static void prime(const struct gfx_job *job, int slot);

static int get_target(const struct gfx_job *job, uint32_t addr, uint32_t width, uint32_t fmt, uint32_t siz,
		      uint32_t min_lh, bool *created)
{
	bool fresh;
	*created = false;
	/* Como rdp.c: una color image imposible no toca la tabla. */
	enum rdp_cimg_bad bad = rdp_cimg_impossible(addr, width, min_lh, raster_rsx_safe ? job->rdram_size : 0);
	if (bad != RDP_CIMG_OK) {
		if (bad == RDP_CIMG_RDRAM) {
			static unsigned said;
			if (said++ < 10)
				fprintf(stderr, "fb: campo %u tarea %u: color image %06x/%u fuera de la RDRAM (%u KB; alto pedido %u): "
					"se ignora\n", raster_field, task_no, addr, width, job->rdram_size >> 10, min_lh);
		} else if (raster_trace_fb) {
			fprintf(stderr, "fb: campo %u tarea %u: color image imposible %06x/%u (alto %u): se ignora\n", raster_field,
				task_no, addr, width, min_lh);
		}
		return -1;
	}
	int i = rdp_targets_get(&tg, addr, width, siz, min_lh, &fresh);
	*created = fresh;
	if (fresh) {
		/* Como rdp.c (alpha_wanted): la superficie con alfa nace con alfa 0. */
		slot_alpha[i] = raster_target_alpha && tg.t[i].bpp == 2 && (raster_alpha_all || tg.t[i].width != tg.vi_width);
		if (slot_alpha[i])
			for (uint32_t k = 0; k < RASTER_W * RASTER_H; k++)
				rgb[i][k] = RGB_TRANSP;
		else
			memset(rgb[i], 0, sizeof rgb[i]);
		tg.t[i].fmt = (uint8_t)fmt;
		rb_wanted_task[i] = 0;
	}
	if (fresh || tg.task - tg.t[i].last_task > RDP_TARGETS_FRESH)
		prime(job, i);
	return i;
}

static struct depth *get_depth(uint32_t addr)
{
	for (int i = 0; i < MAX_DEPTHS; i++)
		if (depths[i].used && depths[i].addr == addr)
			return &depths[i];
	struct depth *d = &depths[next_depth++ % MAX_DEPTHS];
	d->used = true;
	d->addr = addr;
	d->full_task = d->box_task = d->auto_task = 0;
	for (int i = 0; i < RASTER_W * RASTER_H; i++)
		d->z[i] = 1.0f;
	return d;
}

#ifdef SALSA64_GFX_TRACE
/* Depuración (build con -DSALSA64_GFX_TRACE, solo en las tareas que traza
 * --trace-gfx): SALSA64_GFX_MARK=n[,m…] pinta de magenta esos dibujos y
 * SALSA64_GFX_PIX=x,y muestra qué dibujos escriben ese píxel y con qué valores. */
extern int salsa64_gfx_trace_now;
static bool raster_mark;
static unsigned ndraw;
static int pix_x = -1, pix_y = -1;
#endif

static float clamp01(float v)
{
	return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

static int wrap(int i, int n, unsigned mode)
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
		i = -1 - i;  /* reflejado una vez; más allá, el borde (abajo) */
	return i < 0 ? 0 : i >= n ? n - 1 : i;
}

/* Memoria de las texturas: arena de la tarea y región de la caché. */
struct texmem {
	const uint8_t *base[2];  /* GFX_TEX_ARENA, GFX_TEX_CACHE */
};

/* De dónde se lee cada textura del dibujo: un destino (render a textura)
 * o sus texels (de la HLE o, si la HLE la dejó para leerla de un destino y
 * no se puede, decodificados aquí desde la RDRAM). */
struct texsrc {
	int slot;            /* ranura del destino, o -1 */
	bool intensity;      /* una I8 de una imagen de 8 bits: el rojo en los cuatro canales */
	const uint32_t *pix; /* su imagen, o la copia (scratch) si es el ligado */
	uint32_t x0, y0;     /* texel (0, 0) de la textura dentro del destino */
	const uint8_t *data; /* con slot = -1: los texels RGBA8888 */
};

static void fetch(const struct gfx_tex *t, const struct texsrc *s, int x, int y, float out[4])
{
	x = wrap(x, t->width, GFX_TEX_WRAP_S(t->flags));
	y = wrap(y, t->height, GFX_TEX_WRAP_T(t->flags));
	if (s->slot >= 0) {
		uint32_t px = s->x0 + (uint32_t)x, py = s->y0 + (uint32_t)y;
		if (px >= RASTER_W)
			px = RASTER_W - 1;
		if (py >= RASTER_H)
			py = RASTER_H - 1;
		uint32_t c = s->pix[py * RASTER_W + px];
		out[0] = (c >> 16 & 0xFF) / 255.0f;
		out[1] = (c >> 8 & 0xFF) / 255.0f;
		out[2] = (c & 0xFF) / 255.0f;
		out[3] = (c & RGB_TRANSP) ? 0.0f : 1.0f;
		if (s->intensity)
			out[1] = out[2] = out[3] = out[0];
		return;
	}
	const uint8_t *p = s->data + (uint32_t)y * t->pitch + 4u * (uint32_t)x;
	for (int k = 0; k < 4; k++)
		out[k] = p[k] / 255.0f;
}

/* Muestra con semántica del RSX: u, v normalizados; centros de texel en
 * (i + 0,5) / tamaño. */
static void sample(const struct gfx_tex *t, const struct texsrc *s, float u, float v, float out[4])
{
	float x = u * t->width, y = v * t->height;
	if (!(t->flags & GFX_TEX_BILINEAR)) {
		fetch(t, s, (int)floorf(x), (int)floorf(y), out);
		return;
	}
	x -= 0.5f;
	y -= 0.5f;
	int x0 = (int)floorf(x), y0 = (int)floorf(y);
	float fx = x - x0, fy = y - y0, a[4], b[4], c[4], d[4];
	fetch(t, s, x0, y0, a);
	fetch(t, s, x0 + 1, y0, b);
	fetch(t, s, x0, y0 + 1, c);
	fetch(t, s, x0 + 1, y0 + 1, d);
	for (int k = 0; k < 4; k++)
		out[k] = (a[k] * (1 - fx) + b[k] * fx) * (1 - fy) + (c[k] * (1 - fx) + d[k] * fx) * fy;
}

static float edge(float ax, float ay, float bx, float by, float px, float py)
{
	return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* Canal de 0 a 1 a 8 bits. En un destino de 16 bits, con la precisión de
 * la N64: los 5 bits altos, los mismos que guarda la copia a la RDRAM
 * (readback), y de vuelta a 8 como v << 3 | v >> 2, como la HLE decodifica
 * el RGBA16 y el cebado lee la RDRAM. Así leer un destino o su copia en la
 * RDRAM da lo mismo (la RDRAM no cambia), y un efecto que vuelve a leer lo
 * que dibujó (el desenfoque de movimiento de Majora's Mask y de Perfect
 * Dark) sigue igual tras cargar un estado, en que los destinos se sueltan
 * y lo dibujado sale de la RDRAM. */
static uint32_t channel(float v, bool q5)
{
	uint32_t c = (uint32_t)(v * 255.0f + 0.5f);
	if (!q5)
		return c;
	c >>= 3;
	return c << 3 | c >> 2;
}

/* Un triángulo en pantalla: posiciones, z e 1/w de sus vértices y su área. */
struct tri {
	float sx[3], sy[3], sz[3], iw[3];
	float area;
};

/* false si no se dibuja: detrás de la cámara, degenerado o eliminado por
 * su cara (como el RSX, también en la pasada de aristas). */
static bool tri_setup(const struct gfx_draw *d, const struct gfx_vertex *v, float prim_z, struct tri *t3)
{
	for (int i = 0; i < 3; i++) {
		if (v[i].w <= 0.0f)
			return false;
		t3->iw[i] = 1.0f / v[i].w;
		t3->sx[i] = v[i].x * t3->iw[i];
		t3->sy[i] = v[i].y * t3->iw[i];
		t3->sz[i] = v[i].z * t3->iw[i];
	}
	if (d->depth & GFX_DEPTH_PRIM)
		t3->sz[0] = t3->sz[1] = t3->sz[2] = prim_z;
	t3->area = edge(t3->sx[0], t3->sy[0], t3->sx[1], t3->sy[1], t3->sx[2], t3->sy[2]);
	if (t3->area == 0.0f)
		return false;
	/* Antihorario visto con y hacia arriba (convención de OpenGL y de
	 * libultra) es el frente; con la y de pantalla hacia abajo, área < 0. */
	return !((d->cull == GFX_CULL_BACK && t3->area > 0.0f) || (d->cull == GFX_CULL_FRONT && t3->area < 0.0f));
}

/* El píxel (x, y) del triángulo, con coordenadas baricéntricas de pantalla
 * b0, b1, b2: prueba de z, combiner, prueba de alfa y mezcla. */
static void shade(const struct gfx_draw *d, const struct gfx_vertex *v, const struct tri *t3, const struct texsrc src[2],
		  uint32_t *tg_rgb, bool q5, struct depth *zb, int x, int y, float b0, float b1, float b2, float zoff)
{
	float z = b0 * t3->sz[0] + b1 * t3->sz[1] + b2 * t3->sz[2] + zoff;
	int idx = y * RASTER_W + x;
	if ((d->depth & GFX_DEPTH_TEST) && zb && z > zb->z[idx] + ((d->depth & GFX_DEPTH_DECAL) ? 1e-3f : 0.0f))
		return;
	/* ZMODE_XLU: estrictamente delante, o el z-buffer aún borrado. */
	if ((d->depth & GFX_DEPTH_LESS) && zb && z >= zb->z[idx] && zb->z[idx] < 1.0f)
		return;
	/* Coordenadas de textura con corrección de perspectiva. */
	float p0 = b0 * t3->iw[0], p1 = b1 * t3->iw[1], p2 = b2 * t3->iw[2], pw = p0 + p1 + p2;
	p0 /= pw, p1 /= pw, p2 /= pw;
	float c[4], s = p0 * v[0].s + p1 * v[1].s + p2 * v[2].s, t = p0 * v[0].t + p1 * v[1].t + p2 * v[2].t;
	const uint8_t *c0 = &v[0].r, *c1 = &v[1].r, *c2 = &v[2].r;
	/* El sombreado (y con él el factor de niebla) lineal en pantalla, como
	 * el RDP (raster_affine); si no, también con perspectiva. */
	float q0 = raster_affine ? b0 : p0, q1 = raster_affine ? b1 : p1, q2 = raster_affine ? b2 : p2;
	for (int k = 0; k < 4; k++)
		c[k] = (q0 * c0[k] + q1 * c1[k] + q2 * c2[k]) / 255.0f;

	float tx[2][4] = { { 1, 1, 1, 1 }, { 1, 1, 1, 1 } };
	for (int i = 0; i < d->ntex && i < 2; i++) {
		const struct gfx_tex *te = &d->tex[i];
		sample(te, &src[i], s * te->xf[0] + te->xf[2], t * te->xf[1] + te->xf[3], tx[i]);
	}
	float X[GFX_K_TERMS][4];
	for (int k = 0; k < GFX_K_TERMS; k++)
		for (int l = 0; l < 4; l++)
			X[k][l] = d->k[k][0][l] + d->k[k][1][l] * c[l] + d->k[k][2][l] * c[3];
	if (d->fog) {
		/* Como el vertex program de rsxkit: todo por (1 − f) y el
		 * color de niebla al término constante (el primero sin
		 * factores, que combine.c garantiza). */
		float f = c[3];
		int fog_term = GFX_K_ONE;
		if (d->nterms) {
			fog_term = -1;
			for (int k = 0; k < d->nterms && fog_term < 0; k++)
				if (!d->factors[k][0])
					fog_term = k;
		}
		for (int k = 0; k < GFX_K_TERMS; k++)
			for (int l = 0; l < 3; l++)
				X[k][l] *= 1.0f - f;
		for (int l = 0; fog_term >= 0 && l < 3; l++)
			X[fog_term][l] += f * ((d->fog_color >> (24 - 8 * l)) & 0xFF) / 255.0f;
	}
	float out[4];
	if (d->nterms) {
		/* Términos del generador de fragment programs. */
		for (int l = 0; l < 4; l++) {
			float sum = 0.0f;
			for (int k = 0; k < d->nterms; k++) {
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
			out[l] = clamp01(tx[0][l] * X[GFX_K_T0][l] + tx[0][3] * X[GFX_K_T0A][l] +
					 tx[1][l] * X[GFX_K_T1][l] + tx[1][3] * X[GFX_K_T1A][l] +
					 X[GFX_K_ONE][l]);
	}
	if (d->atest == GFX_ATEST_GEQUAL && out[3] * 255.0f + 0.5f < d->aref)
		return;
#ifdef SALSA64_GFX_TRACE
	if (raster_mark)
		out[0] = 1.0f, out[1] = 0.0f, out[2] = 1.0f, out[3] = 1.0f;
#endif
#ifdef SALSA64_GFX_TRACE
	if (pix_x == x && pix_y == y)
		fprintf(stderr, "    PIX draw %u ntex %u blend %u atest %u c %.2f %.2f %.2f %.2f tx0 %.2f %.2f %.2f %.2f "
			"X0 %.2f %.2f %.2f X4 %.2f %.2f %.2f out %.2f %.2f %.2f %.2f s %.1f t %.1f z %.3f\n",
			ndraw, d->ntex, d->blend, d->atest, c[0], c[1], c[2], c[3], tx[0][0], tx[0][1], tx[0][2], tx[0][3],
			X[GFX_K_T0][0], X[GFX_K_T0][1], X[GFX_K_T0][2], X[GFX_K_ONE][0], X[GFX_K_ONE][1], X[GFX_K_ONE][2],
			out[0], out[1], out[2], out[3], s, t, z);
#endif
	uint32_t old = tg_rgb[idx];
	float dst[3] = { (old >> 16 & 0xFF) / 255.0f, (old >> 8 & 0xFF) / 255.0f, (old & 0xFF) / 255.0f };
	if (d->blend == GFX_BLEND_KEEP) {
		if ((d->depth & GFX_DEPTH_WRITE) && zb)
			zb->z[idx] = z;
		return;
	}
	for (int l = 0; l < 3; l++) {
		switch (d->blend) {
		case GFX_BLEND_ALPHA:     out[l] = out[l] * out[3] + dst[l] * (1.0f - out[3]); break;
		case GFX_BLEND_ADD:       out[l] = clamp01(out[l] * out[3] + dst[l]); break;
		case GFX_BLEND_MUL:       out[l] = dst[l] * out[3]; break;
		case GFX_BLEND_ADD_MUL:   out[l] = clamp01(out[l] + dst[l] * out[3]); break;
		case GFX_BLEND_ALPHA_INV: out[l] = out[l] * (1.0f - out[3]) + dst[l] * out[3]; break;
		case GFX_BLEND_ADD_ONE:   out[l] = clamp01(out[l] + dst[l]); break;
		default:                  break;
		}
	}
	/* El bit de alfa: la copia del RSX a la RDRAM corta en 0x80. */
	uint32_t transp = 0;
	switch (dst_alpha_mode) {
	case DSTA_FILL: transp = fill_alpha ? 0 : RGB_TRANSP; break;
	case DSTA_FRAG: transp = out[3] < 0.5f ? RGB_TRANSP : 0; break;
	case DSTA_KEEP: transp = old & RGB_TRANSP; break;
	case DSTA_MAX:  transp = (old & RGB_TRANSP) && out[3] < 0.5f ? RGB_TRANSP : 0; break;
	default:        break;
	}
	tg_rgb[idx] = transp | channel(out[0], q5) << 16 | channel(out[1], q5) << 8 | channel(out[2], q5);
	if ((d->depth & GFX_DEPTH_WRITE) && zb)
		zb->z[idx] = z;
}

static void draw_triangle(const struct gfx_draw *d, const struct gfx_vertex *v, const struct texsrc src[2],
			  uint32_t *tg_rgb, bool q5, struct depth *zb, float prim_z)
{
	struct tri t3;
	if (!tri_setup(d, v, prim_z, &t3))
		return;
	const float *sx = t3.sx, *sy = t3.sy;
	float x0 = d->scissor[0] / 4.0f, y0 = d->scissor[1] / 4.0f;
	float x1 = d->scissor[2] / 4.0f, y1 = d->scissor[3] / 4.0f;
	float minx = fminf(fminf(sx[0], sx[1]), sx[2]), maxx = fmaxf(fmaxf(sx[0], sx[1]), sx[2]);
	float miny = fminf(fminf(sy[0], sy[1]), sy[2]), maxy = fmaxf(fmaxf(sy[0], sy[1]), sy[2]);
	int px0 = (int)floorf(fmaxf(minx, x0)), px1 = (int)ceilf(fminf(maxx, x1));
	int py0 = (int)floorf(fmaxf(miny, y0)), py1 = (int)ceilf(fminf(maxy, y1));
	if (px0 < 0) px0 = 0;
	if (py0 < 0) py0 = 0;
	if (px1 > RASTER_W) px1 = RASTER_W;
	if (py1 > RASTER_H) py1 = RASTER_H;

	for (int y = py0; y < py1; y++)
		for (int x = px0; x < px1; x++) {
			float cx = x + 0.5f, cy = y + 0.5f;
			if (cx < x0 || cx >= x1 || cy < y0 || cy >= y1)
				continue;
			float b0 = edge(sx[1], sy[1], sx[2], sy[2], cx, cy) / t3.area;
			float b1 = edge(sx[2], sy[2], sx[0], sy[0], cx, cy) / t3.area;
			float b2 = 1.0f - b0 - b1;
			if (b0 < 0.0f || b1 < 0.0f || b2 < 0.0f)
				continue;
			shade(d, v, &t3, src, tg_rgb, q5, zb, x, y, b0, b1, b2, 0.0f);
		}
}

/* Las aristas del triángulo como líneas de raster_edge_width píxeles, como
 * el RSX en modo de polígono LINE (rsxkit_draw.edges): por cada columna (o
 * fila, si la línea es más alta que ancha) cuyo centro cae en [inicio,
 * fin), los píxeles de esa columna a menos de medio ancho de la línea. */
static void draw_edges(const struct gfx_draw *d, const struct gfx_vertex *v, const struct texsrc src[2],
		       uint32_t *tg_rgb, bool q5, struct depth *zb, float prim_z)
{
	struct tri t3;
	if (!tri_setup(d, v, prim_z, &t3))
		return;
	float x0 = d->scissor[0] / 4.0f, y0 = d->scissor[1] / 4.0f;
	float x1 = d->scissor[2] / 4.0f, y1 = d->scissor[3] / 4.0f;
	/* Como rsxkit: desplazamiento de polígono hacia atrás (por omisión,
	 * factor 8 y 64 unidades de un z-buffer de 24 bits), para que el relleno
	 * gane donde cubre (la z de la línea es la del punto de la arista) y
	 * las piezas vecinas casi coplanares también. */
	float dzdx = ((t3.sz[1] - t3.sz[0]) * (t3.sy[2] - t3.sy[0]) - (t3.sz[2] - t3.sz[0]) * (t3.sy[1] - t3.sy[0])) / t3.area;
	float dzdy = ((t3.sz[2] - t3.sz[0]) * (t3.sx[1] - t3.sx[0]) - (t3.sz[1] - t3.sz[0]) * (t3.sx[2] - t3.sx[0])) / t3.area;
	float zoff = raster_edge_factor * fmaxf(fabsf(dzdx), fabsf(dzdy)) + raster_edge_units / 16777216.0f;
	for (int e = 0; e < 3; e++) {
		int i = e, j = (e + 1) % 3;
		float ax = t3.sx[i], ay = t3.sy[i], bx = t3.sx[j], by = t3.sy[j];
		bool xmajor = fabsf(bx - ax) >= fabsf(by - ay);
		float a0 = xmajor ? ax : ay, a1 = xmajor ? bx : by;
		if (a0 == a1)
			continue;
		float lo = fminf(a0, a1), hi = fmaxf(a0, a1);
		int p0 = (int)ceilf(lo - 0.5f), p1 = (int)ceilf(hi - 0.5f);  /* centros en [lo, hi) */
		int lim = xmajor ? RASTER_W : RASTER_H;
		p0 = p0 < 0 ? 0 : p0;
		p1 = p1 > lim ? lim : p1;
		for (int p = p0; p < p1; p++) {
			float t = (p + 0.5f - a0) / (a1 - a0);
			float m = xmajor ? ay + (by - ay) * t : ax + (bx - ax) * t;
			/* Línea ancha: los centros a menos de la mitad del ancho en la
			 * dirección menor (con 1, el píxel donde pasa la línea). */
			int q0 = (int)ceilf(m - raster_edge_width * 0.5f - 0.5f), q1 = (int)ceilf(m + raster_edge_width * 0.5f - 0.5f);
			for (int q = q0; q < q1; q++) {
				int x = xmajor ? p : q, y = xmajor ? q : p;
				float cx = x + 0.5f, cy = y + 0.5f;
				if (x < 0 || y < 0 || x >= RASTER_W || y >= RASTER_H || cx < x0 || cx >= x1 || cy < y0 || cy >= y1)
					continue;
				float b[3] = { 0.0f, 0.0f, 0.0f };
				b[i] = 1.0f - t, b[j] = t;
				shade(d, v, &t3, src, tg_rgb, q5, zb, x, y, b[0], b[1], b[2], zoff);
			}
		}
	}
}

/* Recorte contra w = W_MIN: el triángulo con vértices detrás de la cámara
 * queda en un polígono de 3 o 4 vértices delante (el suelo bajo la cámara
 * en Pokémon Snap); sin esto se descartaba entero y dejaba ver lo de atrás.
 * Todo es lineal en coordenadas de recorte (x, y, z y s, t sin dividir). */
#define W_MIN 1e-3f

static void lerp_vertex(const struct gfx_vertex *a, const struct gfx_vertex *b, float t, struct gfx_vertex *o)
{
	o->x = a->x + (b->x - a->x) * t;
	o->y = a->y + (b->y - a->y) * t;
	o->z = a->z + (b->z - a->z) * t;
	o->w = W_MIN;
	o->s = a->s + (b->s - a->s) * t;
	o->t = a->t + (b->t - a->t) * t;
	o->r = (uint8_t)(a->r + (b->r - a->r) * t + 0.5f);
	o->g = (uint8_t)(a->g + (b->g - a->g) * t + 0.5f);
	o->b = (uint8_t)(a->b + (b->b - a->b) * t + 0.5f);
	o->a = (uint8_t)(a->a + (b->a - a->a) * t + 0.5f);
	o->pad = 0;
}

static void draw_clipped(const struct gfx_draw *d, const struct gfx_vertex *v, const struct texsrc src[2],
			 uint32_t *tg_rgb, bool q5, struct depth *zb, float prim_z, bool edges)
{
	void (*draw)(const struct gfx_draw *, const struct gfx_vertex *, const struct texsrc[2], uint32_t *, bool,
		     struct depth *, float) = edges ? draw_edges : draw_triangle;
	if (v[0].w > W_MIN && v[1].w > W_MIN && v[2].w > W_MIN) {
		draw(d, v, src, tg_rgb, q5, zb, prim_z);
		return;
	}
	struct gfx_vertex poly[4];
	int n = 0;
	for (int i = 0; i < 3; i++) {
		const struct gfx_vertex *a = &v[i], *b = &v[(i + 1) % 3];
		if (isnan(a->w))
			return;
		bool ina = a->w > W_MIN, inb = b->w > W_MIN;
		if (ina)
			poly[n++] = *a;
		if (ina != inb)
			lerp_vertex(a, b, (W_MIN - a->w) / (b->w - a->w), &poly[n++]);
	}
	for (int i = 1; i + 1 < n; i++) {
		const struct gfx_vertex tri[3] = { poly[0], poly[i], poly[i + 1] };
		draw(d, tri, src, tg_rgb, q5, zb, prim_z);
	}
}

/* Como rdp.c: ¿los dibujos hasta el próximo TARGET son todos rellenos? */
static bool fills_only(const uint8_t *cmd, uint32_t off, uint32_t cmd_bytes)
{
	while (off + sizeof(struct gfx_cmd) <= cmd_bytes) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(cmd + off);
		if (c->op == GFX_OP_END || c->op == GFX_OP_TARGET || c->size < sizeof *c)
			break;
		if (c->op == GFX_OP_DRAW && ((const struct gfx_draw *)c)->cycle != 3 /* G_CYC_FILL */)
			return false;
		off += c->size;
	}
	return true;
}

/* Hasta qué línea llegan las tijeras de los dibujos que siguen a un TARGET
 * (como rdp.c en la PS3): el alto mínimo de su imagen. */
static uint32_t scissor_bottom(const uint8_t *cmd, uint32_t off, uint32_t cmd_bytes)
{
	uint32_t bottom = 0;
	while (off + sizeof(struct gfx_cmd) <= cmd_bytes) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(cmd + off);
		if (c->op == GFX_OP_END || c->op == GFX_OP_TARGET || c->size < sizeof *c)
			break;
		if (c->op == GFX_OP_DRAW) {
			uint32_t y1 = (((const struct gfx_draw *)c)->scissor[3] + 3) / 4;
			if (y1 > bottom)
				bottom = y1;
		}
		off += c->size;
	}
	return bottom;
}

/* Texturas que la HLE dejó para leer de un destino (GFX_TEX_TARGET) y que
 * se decodificaron aquí desde la RDRAM porque para la tabla no eran de un
 * destino vivo (como rdp.c en la PS3). Un búfer por textura del dibujo. */
static uint8_t tex_here[2][GFX_TEX_MAX];
static uint32_t tex_decoded_here;

/* El origen de una textura del dibujo, si es un destino dibujado hace
 * poco (RDP_TARGETS_FRESH tareas o menos); si no, sus texels (s->data). */
static int texture_source(const struct gfx_job *job, const struct texmem *m, const struct gfx_tex *t, int k, int cur,
			  struct texsrc *s)
{
	s->slot = -1;
	s->intensity = false;
	s->data = m->base[t->where & 1] + t->offset;
	int i = -1;
	uint32_t x0 = 0, y0 = 0;
	enum rdp_texsrc src = RDP_TEXSRC_NONE;
	/* Como rdp.c: de 16 bits, o la I8 de una imagen de 8 bits que la HLE
	 * dejó para leer del destino. */
	uint32_t bpp = t->src_siz == IM_SIZ_16b ? 2 : t->src_siz == 1u && t->where == GFX_TEX_TARGET ? 1 : 0;
	if (t->src_addr && bpp)
		src = rdp_targets_texture(&tg, t->src_addr, t->src_pitch, bpp, cur, &i, &x0, &y0);
	if ((src != RDP_TEXSRC_RSX && src != RDP_TEXSRC_SELF) || !raster_fbfx) {
		if (t->where == GFX_TEX_TARGET) {
			tex8_rdram += bpp == 1;
			memset(tex_here[k], 0, sizeof tex_here[k]);
			if (job->rdram_ea && (uint32_t)t->pitch * t->height <= sizeof tex_here[k])
				(bpp == 1 ? rdp_decode_i8 : rdp_decode_rgba16)((const uint8_t *)(uintptr_t)job->rdram_ea,
									      job->rdram_size, t->src_addr, t->src_pitch,
									      t->width, t->height, tex_here[k], t->pitch);
			s->data = tex_here[k];
			tex_decoded_here++;
			if (raster_trace_fb)
				fprintf(stderr, "fb: campo %u tarea %u: textura %ux%u desde %06x/%u sin decodificar por la HLE: "
					"se decodifica de la RDRAM (%s)\n", raster_field, task_no, t->width, t->height,
					t->src_addr, t->src_pitch, src == RDP_TEXSRC_RDRAM ? "destino viejo o del CPU" : "sin destino");
		}
		return i;
	}
	if (src == RDP_TEXSRC_SELF) {
		/* Como rdp.c: otra vez si desde la copia se dibujó donde lee. */
		if (scratch_slot != i || rdp_box_overlaps(&scratch_dirty, x0, y0, x0 + t->width, y0 + t->height)) {
			memcpy(scratch, rgb[i], sizeof scratch);
			scratch_slot = i;
			memset(&scratch_dirty, 0, sizeof scratch_dirty);
		}
		s->pix = scratch;
	} else {
		s->pix = rgb[i];
	}
	s->slot = i;
	s->intensity = bpp == 1;
	tex8_rsx += bpp == 1;
	s->x0 = x0;
	s->y0 = y0;
	return i;
}

/* G_RM_VISCVG: cobertura plena en la RDRAM en vez de dibujar (como rdp.c). */
static void viscvg(const struct gfx_job *job, int cur, const struct gfx_draw *d)
{
	const struct rdp_target *t = &tg.t[cur];
	if (raster_fbfx < 2 || t->bpp != 2 || !job->rdram_ea)
		return;
	rb_task[cur] = tg.task;
	uint8_t *rdram = (uint8_t *)(uintptr_t)job->rdram_ea;
	uint32_t y0 = d->scissor[1] / 4, y1 = (d->scissor[3] + 3) / 4, x0 = d->scissor[0] / 4, x1 = (d->scissor[2] + 3) / 4;
	if (y1 > t->lh)
		y1 = t->lh;
	if (x1 > t->width)
		x1 = t->width;
	for (uint32_t y = y0; y < y1 && x1 > x0; y++) {
		uint32_t a = t->addr + (y * t->width + x0) * 2;
		if (a + (x1 - x0) * 2 > job->rdram_size)
			break;
		memset(rdram + a, 0xFF, (x1 - x0) * 2);
	}
}

/* Ceba la imagen de la ranura con la RDRAM (RGBA5551), como hace la PS3
 * con una superficie nueva o sin dibujar desde hace tareas. */
static void prime(const struct gfx_job *job, int slot)
{
	const struct rdp_target *t = &tg.t[slot];
	/* Como rdp.c: las capas de 8 bits grandes (big8) también, con el
	 * índice en los tres canales (la copia toma el rojo). */
	bool x8 = t->bpp == 1 && t->width > 128;
	if (!raster_fbfx || (t->bpp != 2 && !x8) || !job->rdram_ea)
		return;
	const uint8_t *rdram = (const uint8_t *)(uintptr_t)job->rdram_ea;
	uint32_t w = t->width < RASTER_W ? t->width : RASTER_W, h = t->lh < RASTER_H ? t->lh : RASTER_H;
	if (x8) {
		for (uint32_t y = 0; y < h; y++) {
			uint32_t a = t->addr + y * t->width;
			if (a + w > job->rdram_size)
				break;
			for (uint32_t x = 0; x < w; x++)
				rgb[slot][y * RASTER_W + x] = (uint32_t)rdram[a + x] * 0x010101u;
		}
		primes++;
		return;
	}
	for (uint32_t y = 0; y < h; y++) {
		uint32_t a = t->addr + y * t->width * 2;
		if (a + w * 2 > job->rdram_size)
			break;
		for (uint32_t x = 0; x < w; x++) {
			uint32_t p = salsa64_rd16(rdram + a + 2 * x);
			uint32_t r = p >> 11, g = (p >> 6) & 31, b = (p >> 1) & 31;
			uint32_t transp = slot_alpha[slot] && !(p & 1) ? RGB_TRANSP : 0;
			rgb[slot][y * RASTER_W + x] = transp | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
		}
	}
	primes++;
}

/* Ranura cuyo rango de direcciones contiene `addr`, sin mirar el paso ni
 * el tamaño de texel (traza: texturas que vienen de un destino pero no
 * como se esperaba). */
static int locate_any(uint32_t addr)
{
	for (int i = 0; i < RDP_TARGETS_MAX; i++) {
		const struct rdp_target *t = &tg.t[i];
		if (t->used && addr >= t->addr && addr < t->addr + t->width * t->bpp * t->lh)
			return i;
	}
	return -1;
}

/* Copia a la RDRAM como RGBA5551 lo que el RDP pudo escribir del destino:
 * la caja `b` (rdp_box), nunca filas enteras. El bit de alfa, el del
 * destino si lo lleva y raster_copy_alpha (como rdp.c, rb_argb); si no, 1. */
static void readback(const struct gfx_job *job, int slot, const struct rdp_box *b)
{
	const struct rdp_target *t = &tg.t[slot];
	if ((t->bpp != 2 && t->bpp != 1) || !job->rdram_ea || rdp_box_empty(b))
		return;
	uint32_t w = t->width < RASTER_W ? t->width : RASTER_W, h = b->y1 < RASTER_H ? b->y1 : RASTER_H;
	uint32_t x1 = b->x1 < w ? b->x1 : w;
	uint8_t *rdram = (uint8_t *)(uintptr_t)job->rdram_ea;
	uint32_t changed = 0;  /* traza: píxeles que la copia cambia en la RDRAM */
	/* Como rdp.c (rb_argb): un framebuffer que nació con alfa, como antes. */
	bool copy_alpha = raster_copy_alpha && slot_alpha[slot] && !t->presented && t->width != tg.vi_width;
	for (uint32_t y = b->y0; y < h; y++) {
		uint32_t a = t->addr + y * t->width * t->bpp;
		if (a + x1 * t->bpp > job->rdram_size)
			break;
		for (uint32_t x = b->x0; x < x1; x++) {
			uint32_t c = rgb[slot][y * RASTER_W + x];
			if (t->bpp == 1) {
				/* Imagen de 8 bits: la intensidad es el rojo (como rdp.c). */
				changed += rdram[a + x] != (uint8_t)(c >> 16);
				rdram[a + x] = (uint8_t)(c >> 16);
				continue;
			}
			uint32_t abit = copy_alpha && (c & RGB_TRANSP) ? 0 : 1;
			uint32_t p = ((c >> 19) & 31) << 11 | ((c >> 11) & 31) << 6 | ((c >> 3) & 31) << 1 | abit;
			changed += salsa64_rd16(rdram + a + 2 * x) != (uint16_t)p;
			salsa64_wr16(rdram + a + 2 * x, (uint16_t)p);
		}
	}
	if (raster_trace_fb && t->bpp == 1)
		fprintf(stderr, "fb: campo %u tarea %u: la copia de %06x/%u cambió %u píxeles de la RDRAM\n", raster_field,
			task_no, t->addr, t->width, changed);
}

/* Espejo del framebuffer (como frame_mirror en rdp.c): lo dibujado desde
 * el último espejo (su caja), hasta la fila `rows`, a la RDRAM, una vez por
 * dibujo, sin pisar lo que una copia reciente de una imagen que se solape
 * dejó en esa memoria. */
static void frame_mirror(const struct gfx_job *job, int slot, uint32_t rows, const char *when)
{
	const struct rdp_target *t = &tg.t[slot];
	struct rdp_box box = t->box;
	if (rows > t->lh)
		rows = t->lh;
	if (rows > box.y1)
		rows = box.y1;
	box.y1 = (uint16_t)rows;
	if (rdp_box_empty(&box))
		return;
	if (t->bpp != 2 || !rows || t->addr + t->width * 2 * rows > job->rdram_size ||
	    !rdp_targets_mirror_wanted(&tg, slot))
		return;
	/* Como rdp.c: no pisar una copia reciente que se solape (Link en los
	 * primeros bytes del framebuffer de Zelda; la cobertura de VISCVG). */
	uint32_t lo = t->addr, hi = t->addr + t->width * 2 * rows;
	for (int k = 0; k < RDP_TARGETS_MAX; k++) {
		const struct rdp_target *a = &tg.t[k];
		if (a->used && tg.task - rb_task[k] <= RB_KEEP_TASKS && a->addr < hi &&
		    a->addr + a->width * a->bpp * a->lh > lo)
			return;
	}
	if (dma_newer(t->addr, t->width * 2 * rows, t->last_task))
		return;
	readback(job, slot, &box);
	rdp_targets_mirrored(&tg, slot);
	frames++;
	if (raster_trace_fb)
		fprintf(stderr, "fb: campo %u tarea %u: espejo del framebuffer %06x/%u %s: %u..%u × %u..%u\n", raster_field,
			task_no, t->addr, t->width, when, box.x0, box.x1, box.y0, box.y1);
}

/* Borra con r->color el rectángulo r (píxeles lógicos) del destino `i` y lo
 * cuenta como dibujado en la tarea: un FILLRECT repartido de una imagen alta
 * (o entregado a un destino nuevo). Como rdp.c (tall_fill_target). */
static void tall_fill_target(int i, const struct rdp_tall_rect *r, bool drawn[RDP_TARGETS_MAX],
			     struct rdp_box drawn_box[RDP_TARGETS_MAX])
{
	const struct rdp_target *t = &tg.t[i];
	int ya = (int)r->y0, yb = (int)(r->y1 + 0.999f);
	int xa = (int)r->x0, xb = (int)(r->x1 + 0.999f);
	ya = ya < 0 ? 0 : ya, yb = yb > (int)t->lh ? (int)t->lh : yb;
	xa = xa < 0 ? 0 : xa, xb = xb > (int)t->lw ? (int)t->lw : xb;
	if (yb <= ya || xb <= xa)
		return;
	for (int y = ya; y < yb && y < RASTER_H; y++)
		for (int x = xa; x < xb && x < RASTER_W; x++)
			rgb[i][y * RASTER_W + x] = r->color | (slot_alpha[i] && !r->color ? RGB_TRANSP : 0);
	rdp_targets_drawn(&tg, i);
	drawn[i] = true;
	struct rdp_box b = { (uint16_t)xa, (uint16_t)ya, (uint16_t)xb, (uint16_t)yb };
	rdp_box_union(&drawn_box[i], &b);
	rdp_box_union(&tg.t[i].box, &b);
}

/* La ranura `i` va a cambiar de imagen (la recicla otro destino de la misma
 * tarea, o crece): lo dibujado en ella hasta ahora, de una imagen auxiliar
 * chica, se copia a la RDRAM con SU dirección y SU alto, como el espejo del
 * final de la tarea, porque después la ranura es de otra y el juego usa
 * esas imágenes como textura (Pokémon Stadium: el ícono animado del botón
 * elegido y el panel del jugador 1). Lo que no es una imagen chica se
 * pierde, como antes. Sigue siendo la única copia que se hace de ella. */
static void flush_recycled(const struct gfx_job *job, int i, const struct rdp_box *box)
{
	const struct rdp_target *t = &tg.t[i];
	uint32_t rows = box->y1 < t->lh ? box->y1 : t->lh;
	if (raster_trace_fb)
		fprintf(stderr, "fb: campo %u tarea %u: la ranura %d (%06x/%u) se recicla con lo dibujado hasta la fila %u (x %u..%u, lw %u lh %u)\n",
			raster_field, task_no, i, t->addr, t->width, rows, box->x0, box->x1, t->lw, t->lh);
	if (raster_fbfx < 2 || !rows || t->presented || (t->bpp != 2 && t->bpp != 1) || t->width == tg.vi_width ||
	    t->width * t->bpp * rows > MIRROR_SLOT)
		return;
	if (dma_newer(t->addr, t->width * t->bpp * rows, task_no))
		return;
	readback(job, i, box);
	rb_task[i] = tg.task;
	mirrors++;
}

void raster_task(const struct gfx_job *job, const struct gfx_result *res)
{
	const uint8_t *cmd = (const uint8_t *)(uintptr_t)job->cmd_ea;
	const struct gfx_vertex *vtx = (const struct gfx_vertex *)(uintptr_t)job->vtx_ea;
	const struct texmem m = { { (const uint8_t *)(uintptr_t)job->tex_ea, (const uint8_t *)(uintptr_t)job->cache_ea } };
	int cur = -1;
	uint32_t off = 0;
	bool drawn[RDP_TARGETS_MAX] = { false };
	struct rdp_box drawn_box[RDP_TARGETS_MAX];  /* lo que tocaron los dibujos de la tarea */
	memset(drawn_box, 0, sizeof drawn_box);
	/* Dibujos que leen cada destino en esta tarea, por destino escrito (traza). */
	uint32_t reads[RDP_TARGETS_MAX][RDP_TARGETS_MAX] = { { 0 } }, read_stale[RDP_TARGETS_MAX][RDP_TARGETS_MAX] = { { 0 } };
	uint32_t read_age[RDP_TARGETS_MAX] = { 0 };
	uint32_t suspect[RDP_TARGETS_MAX] = { 0 };
	struct { uint32_t addr, width, bpp, rows; } tall = { 0, 0, 0, 0 };  /* color image más alta que 480 */
	++task_no;  /* tg.field lo lleva el VI (raster_vi), como en la PS3 */
	if (!(job->flags & GFX_JOB_DPC_CONT))
		rdp_targets_task(&tg);
	/* Como done en la PS3 (rdp.c): las imágenes chicas de 32 bits van a la
	 * RDRAM antes de traducir; sus destinos no se dibujan aquí. */
	if (m32_on < 0)
		m32_on = !getenv("SALSA64_MASCARA32_RSX");
	struct mask32_result m32;
	memset(&m32, 0, sizeof m32);
	if (m32_on && raster_fbfx >= 2 && job->rdram_ea) {
		mask32_run((uint8_t *)(uintptr_t)job->rdram_ea, job->rdram_size, cmd, res->cmd_bytes, vtx, m.base[0],
			   m.base[1], &m32);
		m32_tasks += m32.n > 0;
		m32_draws += m32.draws;
		m32_pixels += m32.pixels;
		m32_bytes += m32.bytes + m32.restored;
		m32_refused += m32.refused;
	}
	scratch_slot = -1;
	last_job = *job;
#ifdef SALSA64_GFX_TRACE
	ndraw = 0;
	pix_x = pix_y = -1;
	if (salsa64_gfx_trace_now && getenv("SALSA64_GFX_PIX"))
		sscanf(getenv("SALSA64_GFX_PIX"), "%d,%d", &pix_x, &pix_y);
#endif
	while (off + sizeof(struct gfx_cmd) <= res->cmd_bytes) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(cmd + off);
		if (c->op == GFX_OP_END || c->size == 0)
			break;
		switch (c->op) {
		case GFX_OP_TARGET: {
			uint32_t lh = scissor_bottom(cmd, off + c->size, res->cmd_bytes), siz = c->c & 0xFF;
			bool sw32 = false;
			for (uint32_t k = 0; k < m32.n; k++)
				sw32 |= m32.addr[k] == c->a && m32.width[k] == c->b;
			if (sw32) {
				/* Ya está en la RDRAM (mask32): ni se dibuja ni se copia. */
				cur = -1;
				tall.rows = 0;
				break;
			}
			if (getenv("SALSA64_PS3_RB")) {
				/* Diagnóstico: cuántas color images pediría copiar la regla
				 * de readbacks_wanted de la PS3 (src/rdp/rdp.c), con el ancho
				 * del VI de ahora y con el del último origen agregado al
				 * anillo (la regla de antes del 2026-10-13). */
				uint32_t old_w = vi_ring.width[(vi_ring.n + RDP_VI_RING - 1) % RDP_VI_RING];
				bool img = (siz == 2u || siz == 1u) && c->b && c->b <= 640 && vi_ring.n;
				bool cov = img && rdp_vi_ring_covers(&vi_ring, c->a, c->b);
				ps3_rb += img && (c->b != vi_ring.cur_width || !cov);
				ps3_rb_old += img && (c->b != old_w || !cov);
			}
			if (cimg_task != tg.task) {
				cimg_task = tg.task;
				frame_start = c->a != first_cimg;
				first_cimg = c->a;
			}
			/* Fuera de la RDRAM (rdp_cimg_impossible): ni destino, ni reparto
			 * de rellenos, ni reciclar una ranura por ella (como rdp.c). */
			bool outside = raster_rsx_safe && job->rdram_size && c->a >= job->rdram_size;
			bool is_tall = !outside && c->b && c->b <= 640 && rdp_targets_tall(c->b, lh) &&
				       fills_only(cmd, off + c->size, res->cmd_bytes);
			bool created;
			if (!outside && !is_tall && c->b && c->b <= 640 && lh <= 480) {
				int v = rdp_targets_recycled(&tg, c->a, c->b, siz, lh);
				if (v >= 0 && drawn[v])
					flush_recycled(job, v, &drawn_box[v]);
			}
			cur = is_tall ? -1 : get_target(job, c->a, c->b, (c->c >> 8) & 0xFF, siz, lh, &created);
			if (cur >= 0 && created) {
				/* La ranura es de otra imagen (se reciclaba la de un destino
				 * dibujado antes en esta tarea, o creció): lo dibujado y su
				 * caja eran de la anterior, y copiarlos a la RDRAM con la
				 * dirección y el alto de esta pisaría memoria del juego
				 * (Pokémon Stadium, 9 destinos en una tarea: nodos del
				 * planificador bajo el ancho de la imagen). */
				drawn[cur] = false;
				memset(&drawn_box[cur], 0, sizeof drawn_box[cur]);
				/* Un relleno de imagen alta anterior (rdp_targets_tall_note) que
				 * este destino no vio: se cebó con la RDRAM, que no lo tiene. */
				struct rdp_tall_rect pend[RDP_TALL_FILLS];
				int np = rdp_targets_tall_pending(&tg, cur, &dma_ring, pend);
				for (int k = 0; k < np; k++)
					tall_fill_target(cur, &pend[k], drawn, drawn_box);
			}
			tall.rows = is_tall ? lh : 0;
			tall.addr = c->a;
			tall.width = c->b;
			tall.bpp = siz == 3 ? 4 : siz == 2 ? 2 : 1;
			break;
		}
		case GFX_OP_ZCLEAR: {
			const struct gfx_zclear *z = (const struct gfx_zclear *)c;
			struct depth *zb = get_depth(c->a);
			if (zb->box_task != tg.task) {
				zb->box_task = tg.task;
				memcpy(zb->box, z->rect, sizeof zb->box);
			} else {
				zb->box[0] = z->rect[0] < zb->box[0] ? z->rect[0] : zb->box[0];
				zb->box[1] = z->rect[1] < zb->box[1] ? z->rect[1] : zb->box[1];
				zb->box[2] = z->rect[2] > zb->box[2] ? z->rect[2] : zb->box[2];
				zb->box[3] = z->rect[3] > zb->box[3] ? z->rect[3] : zb->box[3];
			}
			if (!zb->box[0] && !zb->box[1] && zb->box[2] >= (c->c & 0xFFFF) && zb->box[3] >= 200)
				zb->full_task = tg.task;
			for (int y = z->rect[1]; y < z->rect[3] && y < RASTER_H; y++)
				for (int x = z->rect[0]; x < z->rect[2] && x < RASTER_W; x++)
					zb->z[y * RASTER_W + x] = 1.0f;
			break;
		}
		case GFX_OP_DRAW: {
			const struct gfx_draw *d = (const struct gfx_draw *)c;
#ifdef SALSA64_GFX_TRACE
			{
				ndraw++;
				const char *mk = salsa64_gfx_trace_now ? getenv("SALSA64_GFX_MARK") : NULL;
				raster_mark = false;
				for (const char *q = mk; q && *q;) {
					char *e;
					unsigned long n = strtoul(q, &e, 10);
					if (e == q)
						break;
					if (n == ndraw)
						raster_mark = true;
					q = *e ? e + 1 : e;
				}
			}
#endif
			if (cur < 0 && tall.rows && d->cycle == 3 && d->h.b >= 3  /* G_CYC_FILL */) {
				/* Relleno de la imagen alta: a cada destino que cae dentro. */
				int slots[RDP_TARGETS_MAX];
				uint32_t first[RDP_TARGETS_MAX];
				float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
				for (uint32_t k = 0; k < d->h.b; k++) {
					const struct gfx_vertex *v = &vtx[d->h.a + k];
					if (!(v->w > 0.0f))
						continue;
					float x = v->x / v->w, y = v->y / v->w;
					x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1;
					y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
				}
				uint32_t color = (uint32_t)(d->k[GFX_K_ONE][0][0] * 255.0f + 0.5f) << 16 |
						 (uint32_t)(d->k[GFX_K_ONE][0][1] * 255.0f + 0.5f) << 8 |
						 (uint32_t)(d->k[GFX_K_ONE][0][2] * 255.0f + 0.5f);
				int n = rdp_targets_inside(&tg, tall.addr, tall.width, tall.bpp, tall.rows, slots, first);
				for (int k = 0; k < n && x1 > x0 && y1 > y0; k++) {
					const struct rdp_target *t = &tg.t[slots[k]];
					struct rdp_tall_rect r = { x0 < 0.0f ? 0.0f : x0, y0 - (float)first[k],
								   x1 > (float)t->lw ? (float)t->lw : x1, y1 - (float)first[k],
								   color };
					r.y0 = r.y0 < 0.0f ? 0.0f : r.y0;
					r.y1 = r.y1 > (float)t->lh ? (float)t->lh : r.y1;
					tall_fill_target(slots[k], &r, drawn, drawn_box);
				}
				/* Y para los destinos que se creen después. */
				rdp_targets_tall_note(&tg, tall.addr, tall.width, tall.bpp, tall.rows, x0, y0, x1, y1, color, slots, n);
				break;
			}
			if (cur < 0)
				break;
			struct gfx_draw white;
			if (d->viscvg) {
				/* Cobertura plena en la RDRAM y blanco en la imagen (como rdp.c). */
				viscvg(job, cur, d);
				white = *d;
				white.ntex = 0;
				white.nterms = 0;
				white.blend = GFX_BLEND_OFF;
				white.atest = 0;
				white.fog = 0;
				white.depth = 0;
				memset(white.k, 0, sizeof white.k);
				for (int lane = 0; lane < 4; lane++)
					white.k[GFX_K_ONE][0][lane] = 1.0f;
				d = &white;
			}
			struct texsrc src[2];
			for (int i = 0; i < 2; i++) {
				src[i].slot = -1;
				if (i < d->ntex) {
					int k = texture_source(job, &m, &d->tex[i], i, cur, &src[i]);
					if (k < 0 && raster_trace_fb && d->tex[i].src_addr) {
						int j = locate_any(d->tex[i].src_addr);
						if (j >= 0 && suspect[j]++ == 0)
							fprintf(stderr, "fb: campo %u tarea %u: textura %ux%u con origen %06x en el "
								"destino %06x/%u pero paso %u, siz %u, fmt %u\n", raster_field, task_no,
								d->tex[i].width, d->tex[i].height, d->tex[i].src_addr, tg.t[j].addr,
								tg.t[j].width, d->tex[i].src_pitch, d->tex[i].src_siz, d->tex[i].src_fmt);
					}
					if (k >= 0) {
						reads[k][cur]++;
						read_age[k] = tg.field - tg.t[k].last_field;
						if (src[i].slot < 0)
							read_stale[k][cur]++;
					}
				}
			}
			struct depth *zb = d->depth ? get_depth(d->zimg) : NULL;
			/* Como rdp.c (zauto): en una tarea que empieza un cuadro (otra
			 * primera color image que la tarea anterior), la z image que no
			 * se borró entera antes de su primer dibujo se borra entera. */
			if (zb && zb->auto_task != tg.task && frame_start) {
				zb->auto_task = tg.task;
				if (zb->full_task != tg.task) {
					for (int i = 0; i < RASTER_W * RASTER_H; i++)
						zb->z[i] = 1.0f;
					raster_zauto++;
				}
			}
			/* Como el RSX (rsxkit, RSXKIT_DEPTH_CONST): la z del primitivo
			 * exacta en todo el dibujo, no la de cada vértice entre su w. */
			float prim_z = (d->depth & GFX_DEPTH_PRIM) ? gfx_prim_z(vtx + d->h.a, d->h.b) : 0.0f;
			bool q5 = tg.t[cur].bpp == 2 && raster_q5;
			/* Como rdp.c (to_rsxkit, rsxkit_draw.dst_alpha): en un destino
			 * con alfa, un relleno deja su bit, una copia el alfa del texel y
			 * los ciclos 1 y 2 su cobertura (gfx_draw.cvg_bit); sin
			 * raster_cvg_alpha (alfa-fragmento), el alfa del fragmento. */
			dst_alpha_mode = DSTA_ONE;
			if (slot_alpha[cur]) {
				static const uint8_t cvg_mode[] = {
					[GFX_CVG_ONE] = DSTA_ONE, [GFX_CVG_KEEP] = DSTA_KEEP,
					[GFX_CVG_ALPHA] = DSTA_FRAG, [GFX_CVG_MAX] = DSTA_MAX,
				};
				if (d->cycle == 3 /* G_CYC_FILL */) {
					dst_alpha_mode = DSTA_FILL;
					fill_alpha = d->aref >= 128;
				} else if (d->cycle == 2 /* G_CYC_COPY */ || !raster_cvg_alpha) {
					dst_alpha_mode = DSTA_FRAG;
				} else {
					dst_alpha_mode = d->cvg_bit < sizeof cvg_mode ? cvg_mode[d->cvg_bit] : DSTA_ONE;
					cvg_draws[d->cvg_bit & 3]++;
				}
			}
			/* Como rdp.c: las aristas antes, en los dibujos opacos con AA_EN. */
			if (d->aa && !(d->depth & GFX_DEPTH_DECAL)) {
				for (uint32_t i = 0; raster_aa_edges && i + 3 <= d->h.b; i += 3)
					draw_clipped(d, vtx + d->h.a + i, src, rgb[cur], q5, zb, prim_z, true);
				edge_draws++;
				edge_tris += d->h.b / 3;
			}
			all_draws++;
			draws8 += tg.t[cur].bpp == 1;
			for (uint32_t i = 0; i + 3 <= d->h.b; i += 3)
				draw_clipped(d, vtx + d->h.a + i, src, rgb[cur], q5, zb, prim_z, false);
			dst_alpha_mode = 0;
			if (!drawn[cur])
				rdp_targets_drawn(&tg, cur);
			drawn[cur] = true;
			{
				/* Como rdp.c: la caja de los vértices dentro de las tijeras. */
				struct rdp_box r = { 0, 0, 0, 0 };
				float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
				bool front = true;
				for (uint32_t k = 0; k < d->h.b; k++) {
					const struct gfx_vertex *v = vtx + d->h.a + k;
					if (!(v->w > 0.0f)) {
						front = false;
						continue;
					}
					float x = v->x / v->w, y = v->y / v->w;
					x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1;
					y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
				}
				rdp_box_add(&r, x0, y0, x1, y1, front, d->scissor, tg.t[cur].lw, tg.t[cur].lh);
				rdp_box_union(&drawn_box[cur], &r);
				rdp_box_union(&tg.t[cur].box, &r);
				if (cur == scratch_slot)
					rdp_box_union(&scratch_dirty, &r);
			}
			break;
		}
		default:
			break;
		}
		off += c->size;
	}
	for (int i = 0; i < RDP_TARGETS_MAX; i++) {
		for (int j = 0; raster_trace_fb && j < RDP_TARGETS_MAX; j++)
			if (reads[i][j])
				fprintf(stderr, "fb: campo %u tarea %u: %u dibujos leen el destino %06x/%u (dibujado hace %u "
					"tareas; %u de la RDRAM por viejo o ser el mismo) hacia %06x/%u\n", raster_field,
					task_no, reads[i][j], tg.t[i].addr, tg.t[i].width, read_age[i], read_stale[i][j],
					tg.t[j].addr, tg.t[j].width);
		if (!drawn[i])
			continue;
		/* Como en la PS3: lo decide el ancho del VI y sus orígenes. */
		const struct rdp_target *t = &tg.t[i];
		const struct rdp_box *box = &drawn_box[i];
		uint32_t rows = box->y1 < t->lh ? box->y1 : t->lh;
		/* Como la PS3: la copia pedida también en la tarea anterior va sin
		 * esperar y la imagen sigue siendo del RDP (targets.h). */
		/* Como readbacks_wanted de la PS3, también una imagen de 16 bits de
		 * otro ancho que el VI que no cabe en un espejo (abajo): el fondo de
		 * 328 × 240 de Dr. Mario 64, que el juego dibuja una vez y pega en
		 * cada cuadro leyéndolo de la RDRAM. El host no la copiaba y el
		 * fondo salía de la RDRAM sin dibujar; la PS3 sí. */
		bool big_aux = t->bpp == 2 && t->width <= 640 && t->width != tg.vi_width && !t->presented && vi_ring.n &&
			       t->width * t->bpp * rows > MIRROR_BYTES;
		bool wanted = raster_fbfx >= 2 && (rdp_readback_wanted(&vi_ring, t->addr, t->width, t->bpp == 2 ? 2 : 0) ||
				       (t->bpp == 1 && t->width > 128 && t->width != vi_ring.cur_width && t->width <= 640 && vi_ring.n &&
					!rdp_vi_ring_covers(&vi_ring, t->addr, t->width)) || big_aux);
		bool again = rb_wanted_task[i] && rb_wanted_task[i] + 1 == tg.task;
		if (wanted)
			rb_wanted_task[i] = tg.task;
		if (wanted && rdp_targets_readback(&tg, i, true, !again)) {
			if (dma_newer(t->addr, t->width * t->bpp * rows, task_no))
				continue;
			readback(job, i, box);
			rb_task[i] = tg.task;
			readbacks++;
			big_copies += big_aux;
			if (raster_trace_fb)
				fprintf(stderr, "fb: campo %u tarea %u: copia a la RDRAM del destino %06x/%u: %u..%u × %u..%u (alto "
					"conjeturado %u)\n", raster_field, task_no, t->addr, t->width, box->x0, box->x1, box->y0, rows,
					t->lh);
		} else if (raster_fbfx >= 2 && rows && !t->presented && (t->bpp == 2 || t->bpp == 1) &&
			   t->width != tg.vi_width && t->width * t->bpp * rows <= MIRROR_BYTES &&
			   !(t->bpp == 1 && t->width > 128)) {
			/* Espejo de una imagen auxiliar chica (en la PS3, sin esperar). */
			if (dma_newer(t->addr, t->width * t->bpp * rows, task_no))
				continue;
			readback(job, i, box);
			rb_task[i] = tg.task;
			mirrors++;
			if (raster_trace_fb)
				fprintf(stderr, "fb: campo %u tarea %u: copia espejo del destino %06x/%u: %u..%u × %u..%u (alto "
					"conjeturado %u)\n", raster_field, task_no, t->addr, t->width, box->x0, box->x1, box->y0, rows,
					t->lh);
		} else if (raster_fbfx >= 2 && rdp_targets_mirror_at_task_end(&tg, i)) {
			/* Framebuffer: a la RDRAM al terminar la tarea que lo dibujó,
			 * como el RDP (en la PS3, sin esperar). */
			frame_mirror(job, i, rows, "al terminar la tarea");
		}
	}
}

void raster_vi(const struct salsa64_vi_frame *f)
{
	rdp_vi_ring_frame(&vi_ring, f);
	/* Como la PS3: qué destino se presenta (marca `presented`, el ancho del
	 * VI y la ranura protegida del reciclado). Sin esto el host copiaba a
	 * la RDRAM todos los destinos de cada tarea, framebuffers incluidos, y
	 * no reproducía la política de la consola. */
	struct rdp_present out;
	rdp_targets_present(&tg, f, ++vi_field, &out);
	/* Como la PS3: un framebuffer dibujado que aún no se copió a la RDRAM
	 * (no se sabía que lo era al terminar su tarea) se copia al
	 * presentarlo, solo las filas que muestra el VI. */
	if (raster_fbfx >= 2 && last_job.rdram_ea && out.slot >= 0)
		frame_mirror(&last_job, out.slot, out.y0 + out.h, "al presentarlo");
}

void raster_reset(void)
{
	memset(&tg, 0, sizeof tg);
	memset(rgb, 0, sizeof rgb);
	memset(slot_alpha, 0, sizeof slot_alpha);
	scratch_slot = 0;
	memset(depths, 0, sizeof depths);
	next_depth = 0;
	task_no = vi_field = 0;
	memset(&vi_ring, 0, sizeof vi_ring);
	primes = readbacks = mirrors = frames = dma_skipped = tex_decoded_here = 0;
	all_draws = edge_draws = edge_tris = 0;
	memset(cvg_draws, 0, sizeof cvg_draws);
	big_copies = 0;
	draws8 = tex8_rsx = tex8_rdram = 0;
	m32_tasks = m32_draws = m32_pixels = m32_bytes = m32_refused = 0;
	mask32_forget();
	raster_zauto = 0;
	first_cimg = cimg_task = 0;
	frame_start = false;
	memset(&dma_ring, 0, sizeof dma_ring);
	memset(rb_task, 0, sizeof rb_task);
	memset(&last_job, 0, sizeof last_job);
}

void raster_fb_stats(uint32_t *n_readbacks, uint32_t *n_primes, uint32_t *tex_rsx, uint32_t *tex_rdram,
		     uint32_t *tex_decoded)
{
	*n_readbacks = readbacks;
	*n_primes = primes;
	*tex_rsx = tg.tex_rsx;
	*tex_rdram = tg.tex_rdram;
	*tex_decoded = tex_decoded_here;
}

uint32_t raster_zauto_count(void)
{
	return raster_zauto;
}

void raster_8bit_stats(uint32_t *draws, uint32_t *tex_rsx, uint32_t *tex_rdram)
{
	*draws = draws8;
	*tex_rsx = tex8_rsx;
	*tex_rdram = tex8_rdram;
}

void raster_mask32_stats(uint32_t *tasks, uint32_t *draws, uint32_t *pixels, uint32_t *bytes, uint32_t *refused)
{
	*tasks = m32_tasks;
	*draws = m32_draws;
	*pixels = m32_pixels;
	*bytes = m32_bytes;
	*refused = m32_refused;
}

void raster_alpha_stats(uint32_t cvg[4], uint32_t *big)
{
	memcpy(cvg, cvg_draws, sizeof cvg_draws);
	*big = big_copies;
}

void raster_edge_stats(uint32_t *draws, uint32_t *with_edges, uint32_t *tris)
{
	*draws = all_draws;
	*with_edges = edge_draws;
	*tris = edge_tris;
}

const uint32_t *raster_image(uint32_t addr, uint32_t *width)
{
	for (int i = 0; i < RDP_TARGETS_MAX; i++) {
		const struct rdp_target *t = &tg.t[i];
		if (t->used && addr >= t->addr && addr < t->addr + t->width * t->bpp * t->lh) {
			*width = t->width;
			return rgb[i];
		}
	}
	return NULL;
}

bool raster_image_alpha(uint32_t addr)
{
	for (int i = 0; i < RDP_TARGETS_MAX; i++) {
		const struct rdp_target *t = &tg.t[i];
		if (t->used && addr >= t->addr && addr < t->addr + t->width * t->bpp * t->lh)
			return slot_alpha[i];
	}
	return false;
}

const uint32_t *raster_slot_image(int slot)
{
	return slot >= 0 && slot < RDP_TARGETS_MAX && tg.t[slot].used ? rgb[slot] : NULL;
}

bool raster_target_info(int slot, uint32_t *addr, uint32_t *width, uint32_t *bpp, uint32_t *lh)
{
	if (slot < 0 || slot >= RDP_TARGETS_MAX || !tg.t[slot].used)
		return false;
	*addr = tg.t[slot].addr;
	*width = tg.t[slot].width;
	*bpp = tg.t[slot].bpp;
	*lh = tg.t[slot].lh;
	return true;
}

/* Todo el estado del rasterizador, para dibujar cuadros inventados
 * (interp.c) sin tocar los destinos del juego. */
#define RASTER_STATE(X)                                                                              \
	X(tg) X(rgb) X(scratch) X(scratch_slot) X(depths) X(next_depth) X(task_no) X(vi_field) X(vi_ring) \
	X(primes) X(readbacks) X(mirrors) X(frames) X(dma_skipped) X(dma_ring) X(rb_task) X(rb_wanted_task) \
	X(last_job) X(tex_decoded_here) X(slot_alpha)
#define RASTER_SIZE(v) + sizeof(v)
size_t raster_state_size(void)
{
	return 0 RASTER_STATE(RASTER_SIZE);
}

void raster_state_save(void *buf)
{
	uint8_t *p = buf;
#define RASTER_SAVE(v) memcpy(p, &(v), sizeof(v)), p += sizeof(v);
	RASTER_STATE(RASTER_SAVE)
#undef RASTER_SAVE
}

void raster_state_restore(const void *buf)
{
	const uint8_t *p = buf;
#define RASTER_LOAD(v) memcpy(&(v), p, sizeof(v)), p += sizeof(v);
	RASTER_STATE(RASTER_LOAD)
#undef RASTER_LOAD
}
