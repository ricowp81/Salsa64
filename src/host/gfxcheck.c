/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — censo gráfico: el microcódigo real contra la HLE (solo host).
 *
 * Método (docs/gfx-censo.md). Con --lle=all las tareas gráficas corren en el
 * RSP real; lo que emite llega al RDP por DPC_START/DPC_END y dpc.c lo
 * interpreta con el mismo estado del RDP y el mismo rasterizador que usa la
 * HLE. Al arrancar cada tarea:
 *
 *   1. el estado del RDP del consumidor del LLE (registros, tiles y TMEM) se
 *      copia a un gfx_state aparte y la HLE corre la misma lista sobre él,
 *      con sus propios búferes de salida y su propia caché de texturas
 *      (cada tarea se mide sola: una diferencia no se arrastra a las
 *      siguientes);
 *   2. se guarda el estado del rasterizador (imágenes, tabla de destinos).
 *
 * Mientras la tarea corre en el LLE, cada tramo del RDP se dibuja en el
 * rasterizador de siempre. Al terminar (el primer acceso del CPU al SP con
 * HALT y TASKDONE) se guarda el estado del rasterizador, se vuelve al de
 * antes de la tarea, se dibuja la salida de la HLE y se comparan, destino por
 * destino, los píxeles (0xRRGGBB, 5 bits por canal con raster_q5). El
 * rasterizador vuelve al estado que dejó el LLE.
 *
 * Los efectos de framebuffer del rasterizador quedan como los pone run.c (nivel
 * 2 por omisión): el dibujo del LLE copia a la RDRAM lo que un juego vuelve a
 * leer; el de la HLE, no (la RDRAM se restaura).
 * Las imágenes son de RASTER_W × RASTER_H.
 */
#include "host/gfxcheck.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/rom.h"
#include "host/raster.h"
#include "rdp/targets.h"
#include "rsp/gfx/backend.h"
#include "rsp/gfx/hle.h"

/* ---- Comparación de píxeles ------------------------------------------------ */

void gchk_px_init(struct gchk_px *p)
{
	memset(p, 0, sizeof *p);
	p->x0 = p->y0 = 1 << 30;
	p->x1 = p->y1 = -1;
}

static unsigned chan_max_diff(uint32_t a, uint32_t b)
{
	unsigned m = 0;
	for (unsigned s = 0; s < 24; s += 8) {
		int d = (int)((a >> s) & 0xFF) - (int)((b >> s) & 0xFF);
		unsigned u = (unsigned)(d < 0 ? -d : d);
		m = u > m ? u : m;
	}
	return m;
}

/* ¿Algún píxel de la vecindad de 3 × 3 de `img` alrededor de (x, y) está a
 * lo más a `tol` de `c` en cada canal? */
static bool near_match(const uint32_t *img, unsigned w, unsigned hgt, unsigned stride, int x, int y, uint32_t c)
{
	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			int xx = x + dx, yy = y + dy;
			if (xx < 0 || yy < 0 || xx >= (int)w || yy >= (int)hgt)
				continue;
			if (chan_max_diff(img[(unsigned)yy * stride + (unsigned)xx], c) <= GCHK_MINOR_CHANNEL)
				return true;
		}
	}
	return false;
}

/* Media de 5 × 5 (recortada a la imagen) de cada canal, en 8 bits: lo que sobrevive de una
 * textura muestreada medio texel corrido o de un sombreado con otro redondeo. */
static uint32_t box5(const uint32_t *img, unsigned w, unsigned hgt, unsigned stride, int x, int y)
{
	unsigned sum[3] = { 0, 0, 0 }, n = 0;
	for (int dy = -2; dy <= 2; dy++) {
		for (int dx = -2; dx <= 2; dx++) {
			int xx = x + dx, yy = y + dy;
			if (xx < 0 || yy < 0 || xx >= (int)w || yy >= (int)hgt)
				continue;
			uint32_t c = img[(unsigned)yy * stride + (unsigned)xx];
			sum[0] += c >> 16 & 0xFF, sum[1] += c >> 8 & 0xFF, sum[2] += c & 0xFF;
			n++;
		}
	}
	return (sum[0] / n) << 16 | (sum[1] / n) << 8 | (sum[2] / n);
}

void gchk_px_add(struct gchk_px *acc, const uint32_t *pre, const uint32_t *l, const uint32_t *h, unsigned w,
		 unsigned hgt, unsigned stride, uint32_t addr)
{
	uint32_t here = 0, dl_n = 0, dh_n = 0;
	acc->area += w * hgt;
	for (unsigned y = 0; y < hgt; y++) {
		for (unsigned x = 0; x < w; x++) {
			unsigned i = y * stride + x;
			bool dl = l[i] != pre[i], dh = h[i] != pre[i];
			acc->drawn_l += dl;
			acc->drawn_h += dh;
			dl_n += dl;
			dh_n += dh;
			if (l[i] == h[i])
				continue;
			here++;
			unsigned d = chan_max_diff(l[i], h[i]);
			acc->maxdiff = d > acc->maxdiff ? d : acc->maxdiff;
			acc->hist[d <= 8 ? 0 : d <= 16 ? 1 : d <= 32 ? 2 : d <= 64 ? 3 : 4]++;
			/* Un borde (cobertura de un píxel, muestreo medio texel corrido) deja el color
			 * de uno en la vecindad del otro; lo que no, es una diferencia de verdad. */
			if (d > GCHK_MINOR_CHANNEL &&
			    (!near_match(h, w, hgt, stride, (int)x, (int)y, l[i]) ||
			     !near_match(l, w, hgt, stride, (int)x, (int)y, h[i])) &&
			    chan_max_diff(box5(l, w, hgt, stride, (int)x, (int)y), box5(h, w, hgt, stride, (int)x, (int)y)) >
				    GCHK_BLUR_TOL)
				acc->nbad++;
			if (dl && !dh)
				acc->only_l++;
			else if (dh && !dl)
				acc->only_h++;
			else
				acc->both++;
			acc->x0 = (int)x < acc->x0 ? (int)x : acc->x0;
			acc->y0 = (int)y < acc->y0 ? (int)y : acc->y0;
			acc->x1 = (int)x + 1 > acc->x1 ? (int)x + 1 : acc->x1;
			acc->y1 = (int)y + 1 > acc->y1 ? (int)y + 1 : acc->y1;
		}
	}
	acc->ndiff += here;
	if (acc->ntgt < 16) {
		acc->tgt[acc->ntgt].addr = addr;
		acc->tgt[acc->ntgt].w = w;
		acc->tgt[acc->ntgt].lh = hgt;
		acc->tgt[acc->ntgt].ndiff = here;
		acc->tgt[acc->ntgt].drawn_l = dl_n;
		acc->tgt[acc->ntgt++].drawn_h = dh_n;
	}
	if (here > acc->addr_diff) {
		acc->addr_diff = here;
		acc->addr = addr;
	}
}

enum gchk_class gchk_classify(const struct gchk_px *p)
{
	if (!p->ndiff)
		return GCHK_SAME;
	if (p->maxdiff <= GCHK_MINOR_CHANNEL)
		return GCHK_MINOR;
	if ((uint64_t)p->nbad * 1000000u <= (uint64_t)p->area * GCHK_MINOR_BAD_PPM)
		return GCHK_MINOR;
	return GCHK_DIFF;
}

bool gchk_is_big(const struct gchk_px *p)
{
	return (uint64_t)p->nbad * 1000000u > (uint64_t)p->area * GCHK_BIG_BAD_PPM;
}

enum gchk_kind gchk_kind_of(const struct gchk_px *p)
{
	if (!p->ndiff)
		return GCHK_KIND_NONE;
	if (p->only_l >= p->only_h && p->only_l >= p->both)
		return GCHK_KIND_MISSING;
	if (p->only_h >= p->both)
		return GCHK_KIND_EXTRA;
	return GCHK_KIND_COLOR;
}

const char *gchk_class_name(enum gchk_class c)
{
	static const char *const n[GCHK_NCLASS] = { "idéntica", "menor", "distinta", "sin HLE" };
	return (unsigned)c < GCHK_NCLASS ? n[c] : "?";
}

const char *gchk_kind_name(enum gchk_kind k)
{
	static const char *const n[GCHK_NKIND] = { "ninguno", "la HLE no dibuja", "la HLE dibuja de más",
						   "otro color" };
	return (unsigned)k < GCHK_NKIND ? n[k] : "?";
}

/* ---- Imágenes del rasterizador ---------------------------------------------- */

#define NIMG RDP_TARGETS_MAX
#define NPIX (RASTER_W * RASTER_H)

struct imgset {
	unsigned n;
	uint32_t addr[NIMG], w[NIMG], lh[NIMG];
	uint32_t pix[NIMG][NPIX];
};

static struct imgset set_pre, set_l, set_h;
static uint32_t zeros[NPIX];

static void gather(struct imgset *s)
{
	s->n = 0;
	for (int slot = 0; slot < NIMG; slot++) {
		uint32_t addr, w, bpp, lh;
		if (!raster_target_info(slot, &addr, &w, &bpp, &lh))
			continue;
		const uint32_t *src = raster_slot_image(slot);
		if (!src)
			continue;
		s->addr[s->n] = addr;
		s->w[s->n] = w;
		s->lh[s->n] = lh;
		/* El color: el byte alto es el alfa de los destinos con alfa (raster.c). */
		for (unsigned k = 0; k < NPIX; k++)
			s->pix[s->n][k] = src[k] & 0xFFFFFFu;
		s->n++;
	}
}

static const uint32_t *find_img(const struct imgset *s, uint32_t addr)
{
	for (unsigned i = 0; i < s->n; i++)
		if (s->addr[i] == addr)
			return s->pix[i];
	return zeros;
}

/* Imagen que los dos dejaron: el mayor ancho y alto que declare alguno. */
static void compare_sets(struct gchk_px *out)
{
	gchk_px_init(out);
	/* Direcciones de los dos lados, sin repetir. */
	uint32_t seen[2 * NIMG];
	unsigned ns = 0;
	for (int k = 0; k < 2; k++) {
		const struct imgset *s = k ? &set_h : &set_l;
		for (unsigned i = 0; i < s->n; i++) {
			bool dup = false;
			for (unsigned j = 0; j < ns; j++)
				dup |= seen[j] == s->addr[i];
			if (dup)
				continue;
			seen[ns++] = s->addr[i];
			uint32_t w = s->w[i], lh = s->lh[i];
			for (unsigned o = 0; o < 2; o++) {
				const struct imgset *t = o ? &set_h : &set_l;
				for (unsigned m = 0; m < t->n; m++)
					if (t->addr[m] == s->addr[i]) {
						w = t->w[m] > w ? t->w[m] : w;
						lh = t->lh[m] > lh ? t->lh[m] : lh;
					}
			}
			w = w < RASTER_W ? w : RASTER_W;
			lh = lh < RASTER_H ? lh : RASTER_H;
			gchk_px_add(out, find_img(&set_pre, s->addr[i]), find_img(&set_l, s->addr[i]),
				    find_img(&set_h, s->addr[i]), w, lh, RASTER_W, s->addr[i]);
		}
	}
}

void gchk_compare_raster(const void *pre, const void *post, const struct gfx_job *job, const struct gfx_result *res,
			 struct gchk_px *out)
{
	gather(&set_l);
	/* Con raster_fbfx >= 2 dibujar copia imágenes a la RDRAM: la de la HLE no debe quedar. */
	uint8_t *rdram = (uint8_t *)(uintptr_t)job->rdram_ea;
	static uint8_t *saved;
	if (rdram && job->rdram_size) {
		if (!saved)
			saved = malloc(job->rdram_size);
		if (saved)
			memcpy(saved, rdram, job->rdram_size);
	}
	raster_state_restore(pre);
	gather(&set_pre);
	raster_task(job, res);
	gather(&set_h);
	if (rdram && saved && job->rdram_size)
		memcpy(rdram, saved, job->rdram_size);
	raster_state_restore(post);
	compare_sets(out);
}

/* ---- Estado del RDP --------------------------------------------------------- */

struct sfield {
	const char *name;
	uint16_t off, size;
	uint8_t tile;  /* 1: campo de gfx_tile (se mira en las 8 tiles) */
};

#define GCHK_OH_DITHER 0xFFu
#define SF(n) { #n, (uint16_t)offsetof(struct gfx_state, n), (uint16_t)sizeof(((struct gfx_state *)0)->n), 0 }
#define SFT(n) { "tile." #n, (uint16_t)offsetof(struct gfx_tile, n), (uint16_t)sizeof(((struct gfx_tile *)0)->n), 1 }
static const struct sfield sfields[] = {
	SF(oh), { "oh.dither", 0, 0, 2 }, SF(ol), SF(cc_hi), SF(cc_lo), SF(prim), SF(env), SF(fog), SF(blend), SF(fill), SF(prim_lod),
	SF(prim_min), SF(prim_z), SF(prim_dz), SF(k4), SF(k5), SF(scissor), SF(cimg_addr), SF(cimg_width),
	SF(cimg_fmt), SF(cimg_siz), SF(zimg_addr), SF(timg_addr), SF(timg_width), SF(timg_fmt), SF(timg_siz),
	SFT(fmt), SFT(siz), SFT(pal), SFT(cms), SFT(cmt), SFT(masks), SFT(maskt), SFT(shifts), SFT(shiftt),
	SFT(line), SFT(tmem), SFT(uls), SFT(ult), SFT(lrs), SFT(lrt),
};
#define NSF (sizeof sfields / sizeof sfields[0])
_Static_assert(NSF <= 64, "la máscara de diferencias es de 64 bits");

unsigned gchk_state_nfields(void)
{
	return NSF;
}

const char *gchk_state_field_name(unsigned i)
{
	return i < NSF ? sfields[i].name : "?";
}

uint64_t gchk_state_diff(const struct gfx_state *a, const struct gfx_state *b)
{
	uint64_t m = 0;
	for (unsigned i = 0; i < NSF; i++) {
		const struct sfield *f = &sfields[i];
		if (f->tile == 2) {  /* los bits 0 a 7 de othermode H: máscara de mezcla y dither (la HLE no los lleva) */
			if ((a->oh ^ b->oh) & GCHK_OH_DITHER)
				m |= 1ull << i;
		} else if (i == 0) {  /* oh sin ellos */
			if ((a->oh ^ b->oh) & ~GCHK_OH_DITHER)
				m |= 1ull << i;
		} else if (f->tile) {
			for (unsigned t = 0; t < 8; t++) {
				size_t o = offsetof(struct gfx_state, tiles) + t * sizeof(struct gfx_tile) + f->off;
				if (memcmp((const uint8_t *)a + o, (const uint8_t *)b + o, f->size))
					m |= 1ull << i;
			}
		} else if (memcmp((const uint8_t *)a + f->off, (const uint8_t *)b + f->off, f->size)) {
			m |= 1ull << i;
		}
	}
	return m;
}

/* ---- Censo ------------------------------------------------------------------ */

#define MAX_KEYS 48

struct example {
	bool set;
	uint32_t task, field, ndiff, drawn_l, drawn_h;
	enum gchk_kind kind;
	int x0, y0, x1, y1;
	uint32_t addr;
};

struct key {
	char name[96];
	enum salsa64_gbi gbi;
	uint32_t tasks, cls[GCHK_NCLASS], kinds[GCHK_NKIND], cut, big, lle_poor, lle_poor_work;
	uint32_t tri_l, tri_h, rect_l, rect_h, draw_h;    /* de las tareas con HLE */
	uint32_t unk_tasks_h;                              /* tareas donde la HLE no implementó algo */
	uint32_t rdp_ops[64];                              /* comandos del RDP que llegaron del LLE, por opcode */
	uint32_t unk_h[256], unk_l[256];                   /* opcodes sin implementar en la HLE y en el RDP del LLE */
	uint32_t state_tasks, fld[64];                     /* tareas con el estado final del RDP distinto, y por campo */
	uint64_t px_drawn, px_diff, px_bad, hist[5];
	struct example worst, first;
	uint32_t dumps;
};

static struct {
	bool on;
	const struct salsa64_bus *bus;
	struct gfx_state *lle_state;
	const uint32_t *field;
	const uint32_t *lle_tasks;  /* trabajos que el consumidor del LLE ya terminó (numeran sus tareas) */
	/* Tarea en curso. */
	bool pending, hle, in_hle;
	struct key *key;
	uint32_t task_no, tasks;
	struct salsa64_task task;
	struct gfx_job job;
	struct gfx_result res;
	struct gfx_result lle;      /* suma de los tramos del RDP del LLE */
	uint32_t lle_jobs;
	uint32_t unk_h_task;
	uint32_t ops0[64];   /* gfx_dpc_hist al arrancar la tarea */
	void *pre, *post;
	uint32_t total_class[GCHK_NCLASS];
	/* Opciones del entorno. */
	FILE *log;
	const char *dump_dir;
	unsigned dump_max;
	struct key keys[MAX_KEYS];
	unsigned nkeys;
	uint32_t dump_from;
	double deadline;   /* SALSA64_GCHK_SECONDS: tope de tiempo real; al pasarlo, informe y salida */
} G;

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static struct gfx_state hle_state;
static uint8_t state_buf[GFX_STATE_BYTES];
static struct gfx_vertex hle_vtx[1u << 16] __attribute__((aligned(128)));
/* La misma capacidad para dibujos que la PS3 y la HLE principal del host.
 * Un tope menor convierte listas válidas en diferencias falsas contra el LLE. */
static uint8_t hle_cmd[1280u << 10] __attribute__((aligned(128)));
static uint8_t hle_tex[4u << 20] __attribute__((aligned(128)));
static uint8_t hle_cache[8u << 20] __attribute__((aligned(128)));

static void on_unknown(uint32_t op)
{
	if (!G.pending || !G.key)
		return;
	if (G.in_hle) {
		G.key->unk_h[op & 0xFF]++;
		G.unk_h_task++;
	} else {
		G.key->unk_l[op & 0xFF]++;
	}
}

void gchk_init(const struct salsa64_bus *bus, struct gfx_state *lle_state, const uint32_t *field,
	       const uint32_t *lle_tasks)
{
	memset(&G, 0, sizeof G);
	G.on = true;
	G.bus = bus;
	G.lle_state = lle_state;
	G.field = field;
	G.lle_tasks = lle_tasks;
	G.pre = malloc(raster_state_size());
	G.post = malloc(raster_state_size());
	if (!G.pre || !G.post) {
		fprintf(stderr, "salsa64-run: lle-check-gfx: sin memoria\n");
		exit(3);
	}
	gfx_init(&hle_state);
	gfx_unknown_hook = on_unknown;
	if (getenv("SALSA64_GCHK_SECONDS"))
		G.deadline = now_s() + atof(getenv("SALSA64_GCHK_SECONDS"));
	if (getenv("SALSA64_GCHK_LOG"))
		G.log = fopen(getenv("SALSA64_GCHK_LOG"), "w");
	if (getenv("SALSA64_GCHK_DUMP")) {
		G.dump_dir = getenv("SALSA64_GCHK_DUMP");
		G.dump_max = getenv("SALSA64_GCHK_DUMP_N") ? (unsigned)atoi(getenv("SALSA64_GCHK_DUMP_N")) : 3;
		G.dump_from = getenv("SALSA64_GCHK_DUMP_FROM") ? (uint32_t)atoi(getenv("SALSA64_GCHK_DUMP_FROM")) : 0;
	}
	/* Los efectos de framebuffer quedan como los puso run.c (por omisión el nivel 2, como la PS3):
	 * el rasterizador copia lo dibujado a la RDRAM, y de ahí leen las texturas que un juego arma
	 * con su propia imagen (el mosaico del título de SM64). La copia de la HLE se deshace (ver
	 * gchk_compare_raster). */
}

static struct key *find_key(enum salsa64_gbi gbi, const char *name)
{
	for (unsigned i = 0; i < G.nkeys; i++)
		if (!strcmp(G.keys[i].name, name))
			return &G.keys[i];
	struct key *k = &G.keys[G.nkeys < MAX_KEYS ? G.nkeys++ : MAX_KEYS - 1];
	if (k->name[0])
		return k;  /* tabla llena: todo lo demás va a la última */
	snprintf(k->name, sizeof k->name, "%s", name);
	k->gbi = gbi;
	return k;
}

static void gfx_job_buffers(struct gfx_job *job)
{
	job->vtx_ea = (uint64_t)(uintptr_t)hle_vtx;
	job->vtx_cap = sizeof hle_vtx;
	job->cmd_ea = (uint64_t)(uintptr_t)hle_cmd;
	job->cmd_cap = sizeof hle_cmd;
	job->tex_ea = (uint64_t)(uintptr_t)hle_tex;
	job->tex_cap = sizeof hle_tex;
	job->cache_ea = (uint64_t)(uintptr_t)hle_cache;
	job->cache_cap = sizeof hle_cache;
	if (raster_fbfx >= 1 && !getenv("SALSA64_NO_FBTEX"))
		job->flags |= GFX_JOB_FBTEX;  /* como gfx_acquire */
	job->task = *G.lle_tasks + 1;
	job->done_task = *G.lle_tasks;
}

bool gchk_pending(void)
{
	return G.on && G.pending;
}

void gchk_task_start(const struct salsa64_task *t, enum salsa64_gbi gbi, const char *key)
{
	if (!G.on)
		return;
	if (G.pending)
		gchk_task_end();
	if (G.deadline && now_s() > G.deadline) {
		fprintf(stderr, "salsa64-run: lle-check-gfx: tope de tiempo (SALSA64_GCHK_SECONDS), campo %u\n", G.field ? *G.field : 0);
		gchk_report(stderr);
		exit(0);
	}
	G.pending = true;
	G.tasks++;
	G.task_no++;
	{
		static int tlo = -2, thi;
		if (tlo == -2) {
			tlo = -1;
			if (getenv("SALSA64_GCHK_DPC_TRACE"))
				sscanf(getenv("SALSA64_GCHK_DPC_TRACE"), "%d,%d", &tlo, &thi);
		}
		uint32_t f = G.field ? *G.field : 0;
		gfx_dpc_trace = tlo >= 0 && f >= (uint32_t)tlo && f <= (uint32_t)thi;
		if (gfx_dpc_trace) {
			const uint32_t *dp = G.bus->dp;
			fprintf(stderr, "dpc: === tarea %u campo %u %s: START %06x END %06x CURRENT %06x STATUS %03x\n", G.task_no, f,
				key ? key : "?", dp[SALSA64_DP_START], dp[SALSA64_DP_END], dp[SALSA64_DP_CURRENT], dp[SALSA64_DP_STATUS]);
		}
	}
	G.task = *t;
	G.key = find_key(gbi, key);
	G.hle = gbi != SALSA64_GBI_UNKNOWN;
	memset(&G.lle, 0, sizeof G.lle);
	G.lle_jobs = 0;
	G.unk_h_task = 0;
	memcpy(G.ops0, gfx_dpc_hist, sizeof G.ops0);
	if (!G.hle)
		return;
	/* El estado del RDP de quien consume al LLE, a un gfx_state aparte. */
	gfx_state_save(G.lle_state, state_buf);
	gfx_state_load(&hle_state, state_buf);
	/* Las color images que dibujó el LLE hace poco (lo que la HLE deja leer del destino). */
	memcpy(hle_state.fbimg, G.lle_state->fbimg, sizeof hle_state.fbimg);
	salsa64_gfx_job(&G.job, G.bus, t, gbi);
	gfx_job_buffers(&G.job);
	G.in_hle = true;
	gfx_run(&hle_state, &G.job, &G.res);
	G.in_hle = false;
	raster_state_save(G.pre);
}

void gchk_lle_job(const struct gfx_job *job, const struct gfx_result *res)
{
	(void)job;
	if (!G.on || !G.pending)
		return;
	G.lle.commands += res->commands;
	G.lle.triangles += res->triangles;
	G.lle.rects += res->rects;
	G.lle.draws += res->draws;
	G.lle.unknown_cmds += res->unknown_cmds;
	G.lle_jobs++;
}

static void keep_example(struct example *e, const struct gchk_px *p, enum gchk_kind kind)
{
	e->set = true;
	e->task = G.task_no;
	e->field = G.field ? *G.field : 0;
	e->ndiff = p->ndiff;
	e->drawn_l = p->drawn_l;
	e->drawn_h = p->drawn_h;
	e->kind = kind;
	e->x0 = p->x0;
	e->y0 = p->y0;
	e->x1 = p->x1;
	e->y1 = p->y1;
	e->addr = p->addr;
}

static void write_ppm(const char *path, const uint32_t *l, const uint32_t *h, const uint32_t *pre)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", 3 * RASTER_W, RASTER_H);
	for (int y = 0; y < RASTER_H; y++) {
		for (int part = 0; part < 3; part++) {
			for (int x = 0; x < RASTER_W; x++) {
				int i = y * RASTER_W + x;
				uint32_t c;
				if (part == 0)
					c = l[i];
				else if (part == 1)
					c = h[i];
				else
					c = l[i] == h[i] ? ((pre[i] != l[i]) ? 0x404040u : 0u)
							 : (l[i] != pre[i] && h[i] == pre[i]) ? 0xFF0000u
							 : (h[i] != pre[i] && l[i] == pre[i]) ? 0x00FF00u : 0xFFFF00u;
				uint8_t rgb[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
				fwrite(rgb, 1, 3, f);
			}
		}
	}
	fclose(f);
}

static void dump_images(struct key *k, const struct gchk_px *p)
{
	if (!G.dump_dir || k->dumps >= G.dump_max || G.task_no < G.dump_from)
		return;
	/* gather() ya dejó las imágenes en set_l / set_h / set_pre. */
	char name[160], path[512];
	size_t j = 0;
	for (const char *s = k->name; *s && j + 1 < 40; s++)
		name[j++] = (*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') ? *s : '_';
	name[j] = '\0';
	snprintf(path, sizeof path, "%s/%s-t%u-c%u.ppm", G.dump_dir, name, G.task_no, G.field ? *G.field : 0);
	write_ppm(path, find_img(&set_l, p->addr), find_img(&set_h, p->addr), find_img(&set_pre, p->addr));
	k->dumps++;
}

void gchk_task_end(void)
{
	if (!G.on || !G.pending)
		return;
	G.pending = false;
	struct key *k = G.key;
	k->tasks++;
	for (unsigned i = 0; i < 64; i++)
		k->rdp_ops[i] += gfx_dpc_hist[i] - G.ops0[i];
	if (!G.hle) {
		k->cls[GCHK_NOHLE]++;
		G.total_class[GCHK_NOHLE]++;
		if (G.log)
			fprintf(G.log, "%u\t%u\t%s\tsin HLE\t0\t0\t0\t0\t%u\t0\n", G.task_no, G.field ? *G.field : 0, k->name,
				G.lle.triangles);
		return;
	}
	raster_state_save(G.post);
	struct gchk_px p;
	gchk_compare_raster(G.pre, G.post, &G.job, &G.res, &p);
	enum gchk_class cls = gchk_classify(&p);
	enum gchk_kind kind = cls == GCHK_DIFF ? gchk_kind_of(&p) : GCHK_KIND_NONE;
	k->cls[cls]++;
	k->kinds[kind]++;
	k->big += cls == GCHK_DIFF && gchk_is_big(&p);
	/* El microcódigo real casi no dibuja donde la HLE dibuja de verdad (≥ 2 % del área): el camino LLE
	 * → RDP falla en esa tarea y la comparación no dice nada de la HLE. */
	bool poor = (uint64_t)p.drawn_h * 50 > p.area && (uint64_t)p.drawn_l * 10 < p.drawn_h;
	k->lle_poor += poor;
	/* «Vacía» es «no cambió nada respecto a lo que había en el búfer»: una pantalla fija (el mismo cuadro
	 * en un búfer triple, un menú) también da 0 píxeles nuevos aunque el LLE la dibuje entera. Si el LLE
	 * entregó tantos triángulos y rectángulos como la HLE, la tarea es de ese tipo. */
	k->lle_poor_work += poor && G.lle.triangles + G.lle.rects >= G.res.triangles + G.res.rects;
	G.total_class[cls]++;
	k->cut += (G.res.flags & GFX_RES_CUT) != 0 || G.res.dl_errors != 0;
	k->tri_l += G.lle.triangles;
	k->tri_h += G.res.triangles;
	k->rect_l += G.lle.rects;
	k->rect_h += G.res.rects;
	k->draw_h += G.res.draws;
	k->unk_tasks_h += G.unk_h_task != 0;
	k->px_drawn += p.drawn_l > p.drawn_h ? p.drawn_l : p.drawn_h;
	k->px_diff += p.ndiff;
	k->px_bad += p.nbad;
	for (unsigned i = 0; i < 5; i++)
		k->hist[i] += p.hist[i];
	uint64_t sd = gchk_state_diff(G.lle_state, &hle_state);
	if (sd) {
		k->state_tasks++;
		for (unsigned i = 0; i < NSF; i++)
			k->fld[i] += (sd >> i) & 1;
	}
	if (cls == GCHK_MINOR && getenv("SALSA64_GCHK_DUMP_MINOR"))
		dump_images(k, &p);
	if (cls == GCHK_DIFF) {
		if (!k->first.set)
			keep_example(&k->first, &p, kind);
		if (!k->worst.set || p.ndiff > k->worst.ndiff)
			keep_example(&k->worst, &p, kind);
		dump_images(k, &p);
	}
	if (getenv("SALSA64_GCHK_STATE") && sd)
		fprintf(stderr, "gchk: estado tarea %u (campo %u): LLE oh %08x ol %08x cc %08x/%08x; HLE oh %08x ol %08x cc %08x/%08x\n",
			G.task_no, G.field ? *G.field : 0, G.lle_state->oh, G.lle_state->ol, G.lle_state->cc_hi, G.lle_state->cc_lo,
			hle_state.oh, hle_state.ol, hle_state.cc_hi, hle_state.cc_lo);
	if (getenv("SALSA64_GCHK_VERBOSE") && cls == GCHK_DIFF) {
		fprintf(stderr, "gchk: tarea %u (campo %u) %s:", G.task_no, G.field ? *G.field : 0, k->name);
		for (unsigned t = 0; t < p.ntgt; t++)
			fprintf(stderr, " [%06x %ux%u dif %u L %u H %u]", p.tgt[t].addr, p.tgt[t].w, p.tgt[t].lh, p.tgt[t].ndiff,
				p.tgt[t].drawn_l, p.tgt[t].drawn_h);
		fprintf(stderr, "\n  HLE: %u comandos, %u tri, %u rect, %u dibujos, %u texturas (%u de un destino), flags %x, cmd_bytes %u; "
			"LLE: %u trabajos, %u comandos, %u tri, %u rect, %u dibujos\n", G.res.commands, G.res.triangles, G.res.rects,
			G.res.draws, G.res.textures, G.res.tex_target, G.res.flags, G.res.cmd_bytes, G.lle_jobs, G.lle.commands,
			G.lle.triangles, G.lle.rects, G.lle.draws);
	}
	if (G.log)
		fprintf(G.log, "%u\t%u\t%s\t%s\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%s\t%d,%d-%d,%d\t%06x\t%llx\t%u\n", G.task_no,
			G.field ? *G.field : 0, k->name, gchk_class_name(cls), p.ndiff, p.maxdiff, p.drawn_l, p.drawn_h,
			G.lle.triangles, G.res.triangles, G.lle.rects, G.res.rects, gchk_kind_name(kind), p.x0, p.y0, p.x1, p.y1,
			p.addr, (unsigned long long)sd, p.nbad);
}

uint32_t gchk_total_tasks(void)
{
	return G.tasks;
}

uint32_t gchk_total_class(enum gchk_class c)
{
	return (unsigned)c < GCHK_NCLASS ? G.total_class[c] : 0;
}

/* ---- Informe ---------------------------------------------------------------- */

static void print_ops(FILE *f, const char *label, const uint32_t *h)
{
	/* Los cinco opcodes más frecuentes. */
	bool used[256] = { false };
	char buf[256];
	size_t n = 0;
	buf[0] = '\0';
	for (int r = 0; r < 6; r++) {
		int best = -1;
		for (int i = 0; i < 256; i++)
			if (h[i] && !used[i] && (best < 0 || h[i] > h[best]))
				best = i;
		if (best < 0)
			break;
		used[best] = true;
		n += (size_t)snprintf(buf + n, sizeof buf - n, "%s0x%02X ×%u", n ? ", " : "", best, h[best]);
	}
	if (n)
		fprintf(f, "gchk:   %s: %s\n", label, buf);
}

void gchk_report(FILE *f)
{
	if (!G.on)
		return;
	if (G.pending)
		gchk_task_end();
	uint32_t with_hle = G.total_class[GCHK_SAME] + G.total_class[GCHK_MINOR] + G.total_class[GCHK_DIFF];
	fprintf(f, "salsa64-run: lle-check-gfx: %u tareas gráficas, %u con HLE: %u idénticas (%.2f %%), %u con diferencia "
		"menor, %u distintas; %u sin HLE\n", G.tasks, with_hle, G.total_class[GCHK_SAME],
		with_hle ? 100.0 * G.total_class[GCHK_SAME] / with_hle : 0.0, G.total_class[GCHK_MINOR],
		G.total_class[GCHK_DIFF], G.total_class[GCHK_NOHLE]);
	for (unsigned i = 0; i < G.nkeys; i++) {
		const struct key *k = &G.keys[i];
		uint32_t n = k->cls[GCHK_SAME] + k->cls[GCHK_MINOR] + k->cls[GCHK_DIFF];
		fprintf(f, "gchk: [%s] tareas %u, con HLE %u: idénticas %u, menores %u, distintas %u (grandes %u), sin HLE %u, cortadas %u; "
			"triángulos LLE %u / HLE %u, rectángulos %u / %u, dibujos HLE %u; píxeles distintos %llu (de verdad %llu) de %llu "
			"dibujados; tareas con comandos sin implementar en la HLE %u; estado final del RDP distinto en %u; LLE casi vacía en %u (de ellas %u con triángulos y rectángulos del LLE ≥ HLE: pantalla fija, no vacía)\n",
			k->name, k->tasks, n, k->cls[GCHK_SAME], k->cls[GCHK_MINOR], k->cls[GCHK_DIFF], k->big, k->cls[GCHK_NOHLE],
			k->cut, k->tri_l, k->tri_h, k->rect_l, k->rect_h, k->draw_h, (unsigned long long)k->px_diff,
			(unsigned long long)k->px_bad, (unsigned long long)k->px_drawn, k->unk_tasks_h, k->state_tasks, k->lle_poor, k->lle_poor_work);
		if (k->kinds[GCHK_KIND_MISSING] || k->kinds[GCHK_KIND_EXTRA] || k->kinds[GCHK_KIND_COLOR])
			fprintf(f, "gchk:   tipo de las distintas: la HLE no dibuja %u, dibuja de más %u, otro color %u\n",
				k->kinds[GCHK_KIND_MISSING], k->kinds[GCHK_KIND_EXTRA], k->kinds[GCHK_KIND_COLOR]);
		if (k->px_diff)
			fprintf(f, "gchk:   píxeles distintos por diferencia de canal: ≤8 %llu, ≤16 %llu, ≤32 %llu, ≤64 %llu, más %llu\n",
				(unsigned long long)k->hist[0], (unsigned long long)k->hist[1], (unsigned long long)k->hist[2],
				(unsigned long long)k->hist[3], (unsigned long long)k->hist[4]);
		{
			uint32_t h[256] = { 0 };
			for (unsigned o = 0; o < 64; o++)
				h[0xC0 + o] = k->rdp_ops[o];
			print_ops(f, "comandos del RDP que entregó el LLE (opcode)", h);
		}
		print_ops(f, "comandos sin implementar en la HLE", k->unk_h);
		print_ops(f, "comandos sin implementar en el RDP del LLE", k->unk_l);
		char buf[256];
		size_t o = 0;
		buf[0] = '\0';
		for (unsigned j = 0; j < NSF; j++)
			if (k->fld[j])
				o += o < sizeof buf ? (size_t)snprintf(buf + o, sizeof buf - o, "%s%s ×%u", o ? ", " : "",
								       sfields[j].name, k->fld[j]) : 0;
		if (o)
			fprintf(f, "gchk:   estado final del RDP distinto en: %s\n", buf);
		if (k->worst.set)
			fprintf(f, "gchk:   peor: tarea %u (campo %u), %s, %u píxeles distintos de %u/%u dibujados, caja "
				"(%d,%d)-(%d,%d), destino %06x; primera: tarea %u (campo %u)\n", k->worst.task, k->worst.field,
				gchk_kind_name(k->worst.kind), k->worst.ndiff, k->worst.drawn_l, k->worst.drawn_h, k->worst.x0,
				k->worst.y0, k->worst.x1, k->worst.y1, k->worst.addr, k->first.task, k->first.field);
	}
	if (G.log)
		fflush(G.log);
}
