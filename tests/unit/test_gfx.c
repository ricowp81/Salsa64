/*
 * Salsa64 — pruebas de la HLE gráfica (src/rsp/gfx): display lists F3DEX2
 * armadas a mano en la RDRAM, ejecutadas con gfx_run y revisadas en su
 * salida (vértices, registros de dibujo y texturas).
 */
#include "test.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "core/endian.h"
#include "rsp/gfx/backend.h"
#include "rdp/targets.h"
#include "rsp/gfx/hle.h"
#include "rsp/gfx/clip4.h"

#define DL_PA   0x10000u
#define DATA_PA 0x20000u
#define TEX_PA  0x30000u

static struct gfx_state gs;
static struct gfx_vertex out_vtx[4096] __attribute__((aligned(16)));
static uint8_t out_cmd[64 * 1024] __attribute__((aligned(16)));
static uint8_t out_tex[256 * 1024] __attribute__((aligned(128)));
static uint8_t out_cache[64 * 1024] __attribute__((aligned(128)));
static struct gfx_job job;
static uint32_t cache_task;  /* tarea de la caché de texturas (0: sin caché) */
static struct gfx_result res;
static uint32_t dl_pc, data_pc;
static uint32_t job_ucode;   /* dirección física del microcódigo de la tarea */
static unsigned job_gbi;     /* SALSA64_GBI_* de las tareas (0: F3DEX2) */
static uint32_t job_flags;   /* GFX_JOB_* de las tareas */
static uint32_t job_cmd_cap; /* búfer de comandos de las tareas (0: todo out_cmd) */

/* ---- Armado de la display list ---- */

static void dl(uint32_t w0, uint32_t w1)
{
	t_wr32(dl_pc, w0);
	t_wr32(dl_pc + 4, w1);
	dl_pc += 8;
}

static uint32_t data_alloc(uint32_t bytes)
{
	uint32_t a = data_pc;
	data_pc += (bytes + 15) & ~15u;
	return a;
}

static void start(void)
{
	dl_pc = DL_PA;
	data_pc = DATA_PA;
	cache_task = 0;
	job_flags = 0;
	job_cmd_cap = 0;
	job_gbi = 0;
	gfx_init(&gs);
}

/* Otra tarea con el mismo estado del RDP (y la misma caché). */
static void next_task(void)
{
	dl_pc = DL_PA;
	data_pc = DATA_PA;
	cache_task++;
}

static void run(void)
{
	dl(0xDF000000, 0);  /* G_ENDDL */
	memset(&job, 0, sizeof job);
	job.rdram_ea = (uint64_t)(uintptr_t)t_n64.bus.rdram;
	job.rdram_size = t_n64.bus.rdram_size;
	job.vtx_ea = (uint64_t)(uintptr_t)out_vtx;
	job.vtx_cap = sizeof out_vtx;
	job.cmd_ea = (uint64_t)(uintptr_t)out_cmd;
	job.cmd_cap = job_cmd_cap ? job_cmd_cap : sizeof out_cmd;
	job.tex_ea = (uint64_t)(uintptr_t)out_tex;
	job.tex_cap = sizeof out_tex;
	if (cache_task) {
		job.cache_ea = (uint64_t)(uintptr_t)out_cache;
		job.cache_cap = sizeof out_cache;
		job.task = cache_task;
		job.done_task = cache_task - 1;
	}
	job.gbi = job_gbi ? job_gbi : SALSA64_GBI_F3DEX2;
	job.dl = DL_PA;
	job.ucode = job_ucode;
	job.flags = job_flags;
	gfx_run(&gs, &job, &res);
}

/* n-ésimo registro de un tipo en la salida (NULL si no hay). */
static const struct gfx_cmd *find_cmd(uint16_t op, int n)
{
	uint32_t off = 0;
	while (off + sizeof(struct gfx_cmd) <= res.cmd_bytes) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(out_cmd + off);
		if (c->op == op && n-- == 0)
			return c;
		if (c->op == GFX_OP_END || c->size == 0)
			break;
		off += c->size;
	}
	return NULL;
}

static const struct gfx_draw *draw_n(int n)
{
	return (const struct gfx_draw *)find_cmd(GFX_OP_DRAW, n);
}

/* Mtx de libultra desde floats (fila por fila). */
static uint32_t put_matrix(const float m[16])
{
	uint32_t a = data_alloc(64);
	for (int i = 0; i < 16; i++) {
		int32_t v = (int32_t)lrintf(m[i] * 65536.0f);
		salsa64_wr16(t_n64.bus.rdram + a + 2 * i, (uint16_t)((uint32_t)v >> 16));
		salsa64_wr16(t_n64.bus.rdram + a + 32 + 2 * i, (uint16_t)v);
	}
	return a;
}

static const float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

struct vtx { int16_t x, y, z, s, t; uint8_t c[4]; };

static uint32_t put_vertices(const struct vtx *v, int n)
{
	uint32_t a = data_alloc(16 * (uint32_t)n);
	for (int i = 0; i < n; i++) {
		uint8_t *p = t_n64.bus.rdram + a + 16 * i;
		salsa64_wr16(p + 0, (uint16_t)v[i].x);
		salsa64_wr16(p + 2, (uint16_t)v[i].y);
		salsa64_wr16(p + 4, (uint16_t)v[i].z);
		salsa64_wr16(p + 6, 0);
		salsa64_wr16(p + 8, (uint16_t)v[i].s);
		salsa64_wr16(p + 10, (uint16_t)v[i].t);
		memcpy(p + 12, v[i].c, 4);
	}
	return a;
}

/* Viewport de 320×240 y z 0..0x3FE. */
static void viewport_320x240(void)
{
	uint32_t a = data_alloc(16);
	const int16_t vp[8] = { 640, 480, 511, 0, 640, 480, 511, 0 };
	for (int i = 0; i < 8; i++)
		salsa64_wr16(t_n64.bus.rdram + a + 2 * i, (uint16_t)vp[i]);
	dl(0xDC080008, a);  /* G_MOVEMEM viewport */
}

static void load_mtx(const float m[16], bool projection)
{
	/* G_MTX con LOAD (y NOPUSH: el bit 0 va invertido, así que queda 1). */
	dl(0xDA380000 | (projection ? 0x04 : 0x00) | 0x02 | 0x01, put_matrix(m));
}

#define CC(a, b, c, d) (a), (b), (c), (d)

/* Combiner igual en los dos ciclos: rgb (a, b, c, d) y alfa (a, b, c, d). */
static void set_combine(unsigned a, unsigned b, unsigned c, unsigned d, unsigned aa, unsigned ab, unsigned ac,
			unsigned ad)
{
	uint32_t hi = a << 20 | c << 15 | aa << 12 | ac << 9 | a << 5 | c;
	uint32_t lo = b << 28 | b << 24 | aa << 21 | ac << 18 | d << 15 | ab << 12 | ad << 9 | d << 6 | ab << 3 | ad;
	dl(0xFC000000 | hi, lo);
}

static void set_cycle(unsigned cycle)
{
	/* G_SETOTHERMODE_H, shift 20, len 2. */
	dl(0xE3000000 | (32 - 20 - 2) << 8 | 1, cycle << 20);
}

enum { CC_COMB = 0, CC_T0 = 1, CC_T1 = 2, CC_PRIM = 3, CC_SHADE = 4, CC_ENV = 5, CC_ONE = 6, CC_ZERO = 7 };
enum { CC_C_ZERO = 31, CC_C_T0A = 8, CC_C_SHADEA = 11 };
enum { CC_A_ZERO = 15 };

/* ---- Pruebas ---- */

/* Textura apagada (sin G_TEXTURE) con un combiner que lee TEXEL0
 * (TEXEL0·SHADE, la tapa del muro de Luigi Raceway): TEXEL0 vale 1 y la
 * combinación queda sin términos de textura (el RSX no muestrea nada sin
 * ligar). */
static void gfx_untextured_texel(void)
{
	start();
	float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	load_mtx(ident, true);
	load_mtx(ident, false);
	set_combine(CC_T0, CC_ZERO, CC_SHADE, CC_ZERO, CC_T0, CC_ZERO, CC_SHADE, CC_ZERO);
	dl(0xD9000000, G_SHADING_SMOOTH);                /* geometry mode, sin textura */
	dl(0xFF100000 | 319, 0x00100000);
	const struct vtx v[3] = {
		{ 0, 0, 0, 0, 0, { 252, 252, 252, 255 } },
		{ 1, 0, 0, 0, 0, { 252, 252, 252, 255 } },
		{ 0, 1, 0, 0, 0, { 252, 252, 252, 255 } },
	};
	dl(0x01000000 | 3 << 12 | 3 << 1, put_vertices(v, 3));
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);
	run();
	const struct gfx_draw *d = draw_n(0);
	CHECK(d != NULL);
	if (!d)
		return;
	CHECK_EQ(d->ntex, 0);
	for (int t = GFX_K_T0; t <= GFX_K_T1A; t++)
		for (int i = 0; i < 3; i++)
			for (int l = 0; l < 4; l++)
				CHECK(d->k[t][i][l] == 0.0f);
	for (int l = 0; l < 3; l++)
		CHECK(fabsf(d->k[GFX_K_ONE][1][l] - 1.0f) < 1e-6f);  /* = SHADE */
	CHECK(fabsf(d->k[GFX_K_ONE][2][3] - 1.0f) < 1e-6f || fabsf(d->k[GFX_K_ONE][1][3] - 1.0f) < 1e-6f);
}

/* TEXRECT en 2 ciclos con dos tiles (el texto «NINTENDO 64» de Zelda): la
 * tile 0 cae entera en su textura y se limita, la tile 1 (ruido I8 de
 * 32×32, t × 32 con shiftt 11) se repite: cada tile con su propio límite.
 * Como la tile 1 avanza una textura entera por fila, sale una tira por
 * fila con la t de esa fila. */
static void gfx_texrect_two_tiles(void)
{
	start();
	dl(0xFD100000 | 0, TEX_PA);                                /* SETTIMG de 16 bits */
	dl(0xF5100000, 0x07000000);
	dl(0xF3000000, 0x07000000 | 511 << 12 | 512);             /* LOADBLOCK 1 KB */
	dl(0xF5880000 | 4 << 9, 0x00000000);                       /* tile 0: I8, línea 32 bytes */
	dl(0xF2000000, (31 << 2) << 12 | (1 << 2));                /* 32×2 */
	dl(0xF5880000 | 4 << 9, 0x01000000 | 5 << 14 | 11 << 10 | 5 << 4);  /* tile 1: mask 5, ×32 en t */
	dl(0xF2000000 | 119, 0x01000000 | (124 << 12) | 124);      /* ult = 29,75 */
	set_cycle(G_CYC_2CYCLE);
	set_combine(CC_T0, CC_ZERO, CC_T1, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO, CC_ONE);
	dl(0xE4000000 | (32 << 2) << 12 | (2 << 2), 0);            /* TEXRECT 0,0–32,2 */
	dl(0xE1000000, 0);                                         /* s = t = 0 */
	dl(0xF1000000, 0x04000400);                                /* dsdx = dtdy = 1 */
	run();
	CHECK_EQ(res.rects, 2);
	const struct gfx_draw *d = draw_n(0);
	CHECK(d != NULL);
	if (!d)
		return;
	CHECK_EQ(d->ntex, 2);
	CHECK_EQ(GFX_TEX_WRAP_T(d->tex[0].flags), GFX_WRAP_CLAMP);
	CHECK_EQ(GFX_TEX_WRAP_T(d->tex[1].flags), GFX_WRAP_REPEAT);
	CHECK_EQ(d->h.b, 12);
	for (uint32_t i = 0; i < 12 && i < d->h.b; i++)
		CHECK(out_vtx[d->h.a + i].t == (i < 6 ? 0.0f : 1.0f));
}

/* Clamp con una máscara más chica que la tile (la túnica de Link en OoT:
 * I8 de 16 de ancho en la TMEM, tile de 32 con máscara 4, espejo y clamp):
 * el RDP limita la coordenada a la tile y después la enmascara, así que
 * las columnas 16–31 son el reflejo de las 0–15 (antes salía la fila
 * siguiente de la TMEM y el pliegue quedaba cortado). */
static const uint8_t *tex_of(const struct gfx_tex *t);

static void gfx_tile_clamp_mirror_mask(void)
{
	start();
	for (uint32_t i = 0; i < 64; i++)
		t_n64.bus.rdram[TEX_PA + i] = (uint8_t)(3 * i + 1);       /* 16 × 4 texels I8 */
	dl(0xFD100000 | 0, TEX_PA);                                /* SETTIMG de 16 bits */
	dl(0xF5100000, 0x07000000);
	dl(0xF3000000, 0x07000000 | 31 << 12 | 1024);              /* LOADBLOCK 64 bytes, línea de 16 */
	dl(0xF5880000 | 2 << 9, 2u << 18 | 2u << 14 | 3u << 8 | 4u << 4);  /* tile 0: I8, clamp en t; espejo y clamp en s */
	dl(0xF2000000, (31 << 2) << 12 | (3 << 2));                /* 32×4 */
	set_cycle(G_CYC_1CYCLE);
	set_combine(CC_T0, CC_ZERO, CC_SHADE, CC_ZERO, CC_T0, CC_ZERO, CC_SHADE, CC_ZERO);
	dl(0xE4000000 | (32 << 2) << 12 | (4 << 2), 0);            /* TEXRECT 0,0–32,4 */
	dl(0xE1000000, 0);
	dl(0xF1000000, 0x04000400);
	run();
	const struct gfx_draw *d = draw_n(0);
	CHECK(d && d->ntex == 1);
	if (!d || d->ntex != 1)
		return;
	CHECK_EQ(d->tex[0].width, 32);
	CHECK_EQ(d->tex[0].height, 4);
	const uint8_t *p = tex_of(&d->tex[0]);
	for (uint32_t y = 0; y < 4; y++)
		for (uint32_t x = 0; x < 32; x++)
			CHECK_EQ(p[y * d->tex[0].pitch + 4 * x], t_n64.bus.rdram[TEX_PA + 16 * y + (x < 16 ? x : 31 - x)]);
}

/* P = FOG en el blender: el color es la niebla, no el combiner. El fundido
 * de la intro de Banjo-Kazooie es un rectángulo FOG·A_FOG + MEM·(1 − A_FOG)
 * (othermode L 0xC4404340) con el combiner en 0: con la niebla blanca
 * opaca, blanco sin mezcla (salía negro); con alfa 0xD8, mezcla con ese
 * alfa. */
static void gfx_blender_fog_color(void)
{
	static const struct { uint32_t fog; uint8_t blend; } c[] = {
		{ 0xFFFFFFFF, GFX_BLEND_OFF },
		{ 0xFFFFFFD8, GFX_BLEND_ALPHA },
	};
	for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
		start();
		set_cycle(G_CYC_1CYCLE);
		dl(0xFCFFFFFF, 0xFFFFFFFF);                      /* combiner: todo 0 */
		dl(0xF8000000, c[i].fog);                        /* G_SETFOGCOLOR */
		dl(0xEF000000 | G_CYC_1CYCLE << 20, 0xC4404340);  /* FOG·A_FOG + MEM·(1 − A_FOG) */
		dl(0xE4000000 | (40 << 2) << 12 | (40 << 2), 0);  /* G_TEXRECT */
		dl(0xE1000000, 0);
		dl(0xF1000000, 0x04000400);
		run();
		const struct gfx_draw *d = draw_n(0);
		CHECK(d != NULL);
		if (!d)
			continue;
		CHECK_EQ(d->blend, c[i].blend);
		float out[4] = { 0, 0, 0, 0 };
		for (int k = 0; k < GFX_K_TERMS; k++)
			for (int l = 0; l < 4; l++)
				out[l] += d->k[k][0][l];  /* sin textura ni color de vértice: solo k0 */
		for (int l = 0; l < 3; l++)
			CHECK(fabsf(out[l] - 1.0f) < 1e-6f);
		CHECK(fabsf(out[3] - (float)(c[i].fog & 0xFF) / 255.0f) < 1e-6f);
	}
}

/* Los productos del blender que el RSX hace tal cual. Vigilante 8 dibuja
 * cada explosión, humo y resplandor del sol en dos pasadas con el mismo
 * cuadrilátero: MEM·A_IN con el color en negro (oscurece el fondo según la
 * textura) y encima IN·A_FOG + MEM·1 (G_RM_ADD) con la textura de color;
 * clasificadas como IN·α + MEM·(1 − α) salían como cuadros negros. Con
 * GFX_JOB_BLEND_OLD vuelven a la clasificación de antes. */
static void gfx_blender_products(void)
{
	enum { P_IN = 0, P_MEM = 1, P_BL = 2, P_FOG = 3 };
	enum { A_IN = 0, A_FOG = 1, A_SHADE = 2, A_ZERO = 3 };
	enum { B_1MA = 0, B_MEM = 1, B_ONE = 2, B_ZERO = 3 };
#define BLW(p, a, m, b)                                                                                     \
	((uint32_t)(p) << 30 | (uint32_t)(a) << 26 | (uint32_t)(m) << 22 | (uint32_t)(b) << 18 |          \
	 (uint32_t)(p) << 28 | (uint32_t)(a) << 24 | (uint32_t)(m) << 20 | (uint32_t)(b) << 16 | 0x4040u)
	static const struct {
		uint32_t ol, fog;
		uint32_t flags;
		uint8_t blend;
		float rgb;   /* el término constante esperado (color PRIM 0x80, escalado) */
		float alpha; /* y su alfa (PRIM 0xC0 o el de la niebla) */
	} c[] = {
		{ BLW(P_MEM, A_IN, P_IN, B_ZERO), 0xFFFFFFFF, 0, GFX_BLEND_MUL, 0x80 / 255.0f, 0xC0 / 255.0f },
		{ BLW(P_MEM, A_IN, P_IN, B_1MA), 0xFFFFFFFF, 0, GFX_BLEND_ALPHA_INV, 0x80 / 255.0f, 0xC0 / 255.0f },
		{ BLW(P_MEM, A_IN, P_IN, B_MEM), 0xFFFFFFFF, 0, GFX_BLEND_ADD_MUL, 0x80 / 255.0f, 0xC0 / 255.0f },
		/* G_RM_ADD: con el alfa de la niebla en 255, la suma tal cual; con
		 * 128, el color escalado a la mitad. */
		{ BLW(P_IN, A_FOG, P_MEM, B_ONE), 0xFFFFFFFF, 0, GFX_BLEND_ADD_ONE, 0x80 / 255.0f, 0xC0 / 255.0f },
		{ BLW(P_IN, A_FOG, P_MEM, B_ONE), 0xFFFFFF80, 0, GFX_BLEND_ADD_ONE, 0x80 / 255.0f * 0x80 / 255.0f,
		  0xC0 / 255.0f },
		/* Los de siempre no cambian. */
		{ BLW(P_IN, A_IN, P_MEM, B_1MA), 0xFFFFFFFF, 0, GFX_BLEND_ALPHA, 0x80 / 255.0f, 0xC0 / 255.0f },
		{ BLW(P_IN, A_IN, P_MEM, B_MEM), 0xFFFFFFFF, 0, GFX_BLEND_ADD, 0x80 / 255.0f, 0xC0 / 255.0f },
		{ BLW(P_IN, A_ZERO, P_IN, B_ONE), 0xFFFFFFFF, 0, GFX_BLEND_OFF, 0x80 / 255.0f, 0xC0 / 255.0f },
		/* El interruptor: todo lo que no sea opaco, solo z o aditivo vuelve
		 * a IN·α + MEM·(1 − α), sin escalar nada. */
		{ BLW(P_MEM, A_IN, P_IN, B_ZERO), 0xFFFFFFFF, GFX_JOB_BLEND_OLD, GFX_BLEND_ALPHA, 0x80 / 255.0f,
		  0xC0 / 255.0f },
		{ BLW(P_IN, A_FOG, P_MEM, B_ONE), 0xFFFFFF80, GFX_JOB_BLEND_OLD, GFX_BLEND_ALPHA, 0x80 / 255.0f,
		  0xC0 / 255.0f },
	};
#undef BLW
	for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
		start();
		job_flags = c[i].flags;
		set_cycle(G_CYC_1CYCLE);
		set_combine(CC_A_ZERO, CC_ZERO, CC_C_ZERO, CC_PRIM, CC_ZERO, CC_ZERO, CC_ZERO, CC_PRIM);
		dl(0xFA000000, 0x808080C0);                      /* G_SETPRIMCOLOR */
		dl(0xF8000000, c[i].fog);                        /* G_SETFOGCOLOR */
		dl(0xEF000000 | G_CYC_1CYCLE << 20, c[i].ol);     /* G_RDPSETOTHERMODE */
		dl(0xE4000000 | (40 << 2) << 12 | (40 << 2), 0);  /* G_TEXRECT */
		dl(0xE1000000, 0);
		dl(0xF1000000, 0x04000400);
		run();
		const struct gfx_draw *d = draw_n(0);
		CHECK(d != NULL);
		if (!d)
			continue;
		CHECK_EQ(d->blend, c[i].blend);
		float out[4] = { 0, 0, 0, 0 };
		for (int k = 0; k < GFX_K_TERMS; k++)
			for (int l = 0; l < 4; l++)
				out[l] += d->k[k][0][l];  /* sin textura ni color de vértice: solo k0 */
		for (int l = 0; l < 3; l++)
			CHECK(fabsf(out[l] - c[i].rgb) < 1e-5f);
		CHECK(fabsf(out[3] - c[i].alpha) < 1e-5f);
	}
}

/* G_SETCONVERT: K5 entra al combiner (Conker: (SHADE − ENV)·K5 + PRIM en el
 * segundo ciclo, con K5 = 255; si se ignora, la cara sale negra) y persiste
 * en el estado del RDP. */
static void gfx_setconvert_k5(void)
{
	start();
	dl(0xEC000000, 0x00000000 | 0x41u << 9 | 0xFFu);  /* K4 = 0x41, K5 = 0xFF */
	run();
	CHECK_EQ(gs.k4, 0x41);
	CHECK_EQ(gs.k5, 0xFF);

	struct gfx_draw d;
	memset(&d, 0, sizeof d);
	gs.prim = 0;
	gs.env = 0;
	gs.oh = (uint32_t)G_CYC_2CYCLE << G_MDSFT_CYCLETYPE;
	/* Ciclo 0 en cero; ciclo 1: rgb (SHADE − ENV)·K5 + PRIM, alfa SHADE. */
	gs.cc_hi = 15u << 20 | 31u << 15 | 7u << 12 | 7u << 9 | 4u << 5 | 15u;
	gs.cc_lo = 15u << 28 | 5u << 24 | 7u << 21 | 7u << 18 | 7u << 15 | 7u << 12 | 4u << 9 | 3u << 6 | 7u << 3 | 4u;
	CHECK(!gfx_combine(&gs, &d));
	CHECK_EQ(d.ntex, 0);
	for (int lane = 0; lane < 3; lane++)
		CHECK(fabsf(d.k[GFX_K_ONE][1][lane] - 1.0f) < 1e-6f);

	gs.k5 = 0x80;  /* 128/255 */
	CHECK(!gfx_combine(&gs, &d));
	CHECK(fabsf(d.k[GFX_K_ONE][1][0] - 128.0f / 255.0f) < 1e-6f);
	gs.k5 = 0x1FF;  /* 9 bits con signo: −1 */
	CHECK(!gfx_combine(&gs, &d));
	CHECK(fabsf(d.k[GFX_K_ONE][1][0] + 1.0f / 255.0f) < 1e-6f);

	/* Estado guardado: K4 y K5 van y vuelven; un estado sin ellos (bytes
	 * en cero al final) los deja en cero. */
	uint8_t buf[GFX_STATE_REGS];
	gfx_state_pack_regs(&gs, buf);
	gs.k4 = gs.k5 = 0;
	CHECK(gfx_state_unpack_regs(&gs, buf));
	CHECK_EQ(gs.k4, 0x41);
	CHECK_EQ(gs.k5, 0x1FF);
	unsigned i = GFX_STATE_REGS;
	while (i > 0 && buf[i - 1] == 0)
		i--;
	memset(buf + i - 4, 0, 4);
	CHECK(gfx_state_unpack_regs(&gs, buf));
	CHECK_EQ(gs.k4, 0);
	CHECK_EQ(gs.k5, 0);
}

static void set_yuv_matrix(int k0, int k1, int k2, int k3)
{
	uint64_t matrix = (uint64_t)(k0 & 511) << 45 | (uint64_t)(k1 & 511) << 36 |
			  (uint64_t)(k2 & 511) << 27 | (uint64_t)(k3 & 511) << 18;
	dl(0xEC000000 | (uint32_t)(matrix >> 32), (uint32_t)matrix);
}

/* Bloques U,Y0,V,Y1 inventados: planos de TMEM, parejas de crominancia,
 * filas impares, vuelta a los 2 KB y los coeficientes con signo. */
static void gfx_yuv_texture(void)
{
	static const uint8_t pixels[16] = { 128, 16, 128, 235, 96, 100, 160, 140,
		160, 110, 96, 150, 0, 16, 255, 240 };
	static const uint32_t expected[8] = { 0x10101010, 0xEBEBEBEB, 0x90592C64, 0xB881548C,
		0x427AA66E, 0x6AA2CE96, 0xBE000010, 0xFFC312F0 };
	for (int tile = 0; tile < 2; tile++)
	for (int wrap = 0; wrap < 2; wrap++)
	for (int disabled = 0; disabled < 2; disabled++) {
		start();
		cache_task = 1;
		job_flags = disabled ? GFX_JOB_NO_YUV : 0;
		uint32_t base = wrap ? 2040 : 0;
		memcpy(t_n64.bus.rdram + TEX_PA, pixels, sizeof pixels);
		memcpy(t_n64.bus.rdram + TEX_PA + 16, pixels, sizeof pixels);
		set_yuv_matrix(175, -43, -89, 222);
		dl(0xFD300007, TEX_PA);  /* YUV16, ancho 8 */
		dl(0xF5300200 | base / 8, 0x07000000);  /* plano de 8 bytes por fila */
		if (tile)
			dl(0xF4000000, 0x07000000 | 7 << 14 | 1 << 2);
		else
			dl(0xF3000000, 0x07000000 | 15 << 12 | 1024);
		dl(0xF5300200 | base / 8, 2u << 18 | 2u << 8);
		dl(0xF2000000, 7 << 14 | 1 << 2);
		set_cycle(G_CYC_1CYCLE);
		set_combine(CC_A_ZERO, CC_ZERO, CC_C_ZERO, CC_T0, CC_ZERO, CC_ZERO, CC_ZERO, CC_T0);
		dl(0xE4000000 | 8 << 14 | 2 << 2, 0);
		dl(0xE1000000, 0);
		dl(0xF1000000, 0x04000400);
		/* Sin recargar TMEM: SETCONVERT debe separar dibujo y textura. */
		set_yuv_matrix(0, 0, 0, 0);
		dl(0xE4000000 | 8 << 14 | 2 << 2, 0);
		dl(0xE1000000, 0);
		dl(0xF1000000, 0x04000400);
		run();
		CHECK_EQ(res.dl_errors, 0);
		const struct gfx_draw *d = draw_n(0), *d2 = draw_n(1);
		CHECK(d && d2);
		if (!d || !d2)
			return;
		CHECK_EQ(d->tex[0].width, 8);
		CHECK_EQ(d->tex[0].height, 2);
		const uint8_t *tex = tex_of(&d->tex[0]), *tex2 = tex_of(&d2->tex[0]);
		for (int y = 0; y < 2; y++)
			for (int x = 0; x < 8; x++) {
				CHECK_EQ(salsa64_rd32(tex + y * d->tex[0].pitch + 4 * x), disabled ? 0xFFu : expected[x]);
				if (x < 2)
					CHECK_EQ(salsa64_rd32(tex2 + y * d2->tex[0].pitch + 4 * x), disabled ? 0xFFu : expected[x]);
			}
		if (!disabled)
			CHECK(salsa64_rd32(tex + 8) != salsa64_rd32(tex2 + 8));
		/* Cambiar solo la Y debe cambiar la clave, aunque U/V no cambien. */
		uint32_t key = d->tex[0].key;
		t_n64.bus.rdram[TEX_PA + 1] = 32;
		gfx_run(&gs, &job, &res);
		d = draw_n(0);
		CHECK(d && d->tex[0].key != key);
		/* El interruptor no reutiliza el negro de la caché anterior. */
		job.flags ^= GFX_JOB_NO_YUV;
		gfx_run(&gs, &job, &res);
		d = draw_n(0);
		CHECK(d != NULL);
		if (d)
			CHECK_EQ(salsa64_rd32(tex_of(&d->tex[0])), disabled ? 0x20202020u : 0xFFu);
	}
	start();
	set_yuv_matrix(175, -43, -89, 222);
	run();
	CHECK_EQ(gs.k0, 175);
	CHECK_EQ(gs.k1, 469);
	CHECK_EQ(gs.k2, 423);
	CHECK_EQ(gs.k3, 222);
	uint8_t state[GFX_STATE_REGS];
	gfx_state_pack_regs(&gs, state);
	gs.k0 = gs.k1 = gs.k2 = gs.k3 = 0;
	CHECK(gfx_state_unpack_regs(&gs, state));
	CHECK_EQ(gs.k0, 175);
	CHECK_EQ(gs.k1, 469);
	CHECK_EQ(gs.k2, 423);
	CHECK_EQ(gs.k3, 222);
	gfx_rdp_reset(&gs);
	CHECK_EQ(gs.k0 | gs.k1 | gs.k2 | gs.k3, 0);
}

static void gfx_combiner_forms(void)
{
	/* SHADE: todo en X_4, coeficiente del color del vértice. */
	start();
	set_combine(CC_A_ZERO, CC_ZERO, CC_C_ZERO, CC_SHADE, CC_ZERO, CC_ZERO, CC_ZERO, CC_SHADE);
	gs.oh = 0;
	run();
	struct gfx_draw d;
	memset(&d, 0, sizeof d);
	gs.cc_hi = 0, gs.cc_lo = 0;
	/* Se evalúa directo sobre el estado. */
	gs.cc_hi = (15u << 20 | 31u << 15 | 7u << 12 | 7u << 9 | 15u << 5 | 31u);
	gs.cc_lo = (7u << 28 | 7u << 24 | 7u << 21 | 7u << 18 | 4u << 15 | 7u << 12 | 4u << 9 | 4u << 6 | 7u << 3 | 4u);
	CHECK(!gfx_combine(&gs, &d));
	CHECK_EQ(d.ntex, 0);
	for (int lane = 0; lane < 3; lane++)
		CHECK(d.k[GFX_K_ONE][1][lane] == 1.0f);
	CHECK(d.k[GFX_K_ONE][2][3] == 1.0f);

	/* TEXEL0 × SHADE (G_CC_MODULATERGBA): T0·S en X_0 con coeficiente S. */
	gs.cc_hi = (1u << 20 | 4u << 15 | 1u << 12 | 4u << 9 | 1u << 5 | 4u);
	gs.cc_lo = (15u << 28 | 15u << 24 | 1u << 21 | 4u << 18 | 7u << 15 | 7u << 12 | 7u << 9 | 7u << 6 | 7u << 3 | 7u);
	CHECK(!gfx_combine(&gs, &d));
	CHECK_EQ(d.ntex, 1);
	CHECK(d.k[GFX_K_T0][1][0] == 1.0f && d.k[GFX_K_T0][0][0] == 0.0f);
	CHECK(d.k[GFX_K_T0A][2][3] == 1.0f);  /* alfa: T0a · Sa */
	CHECK(d.k[GFX_K_ONE][0][0] == 0.0f);

	/* (PRIM − ENV) × TEXEL0 + ENV con prim = blanco y env = negro: T0. */
	gs.prim = 0xFFFFFFFF;
	gs.env = 0x00000000;
	gs.cc_hi = (3u << 20 | 1u << 15 | 7u << 12 | 7u << 9 | 3u << 5 | 1u);
	gs.cc_lo = (5u << 28 | 5u << 24 | 7u << 21 | 7u << 18 | 5u << 15 | 7u << 12 | 1u << 9 | 5u << 6 | 7u << 3 | 1u);
	CHECK(!gfx_combine(&gs, &d));
	CHECK(fabsf(d.k[GFX_K_T0][0][1] - 1.0f) < 1e-6f);
	CHECK(fabsf(d.k[GFX_K_ONE][0][1]) < 1e-6f);
	CHECK(fabsf(d.k[GFX_K_T0A][0][3] - 1.0f) < 1e-6f);

	/* 2 ciclos: lerp(T0, T1, prim_lod) y luego × SHADE. En el segundo ciclo
	 * no hay texels: el intercambio TEXEL0/TEXEL1 no influye. */
	gs.oh = (uint32_t)G_CYC_2CYCLE << G_MDSFT_CYCLETYPE;
	gs.prim_lod = 0x80;
	gs.cc_hi = (2u << 20 | 14u << 15 | 7u << 12 | 7u << 9 | 0u << 5 | 4u);
	gs.cc_lo = (1u << 28 | 15u << 24 | 7u << 21 | 7u << 18 | 1u << 15 | 7u << 12 | 6u << 9 | 7u << 6 | 7u << 3 | 6u);
	CHECK(!gfx_combine(&gs, &d));
	CHECK_EQ(d.ntex, 2);
	float k = 0x80 / 255.0f;
	CHECK(fabsf(d.k[GFX_K_T0][1][0] - (1.0f - k)) < 1e-5f);
	CHECK(fabsf(d.k[GFX_K_T1][1][0] - k) < 1e-5f);

	/* TEXEL0 × TEXEL1 (alfa 1): no cabe en la combinación lineal; va al
	 * generador como un término T0·T1 (alfa: su carril 3 es T0a·T1a, que
	 * vale 0 aquí, más un término sin factores con 1 en el alfa). */
	gs.oh = 0;
	gs.cc_hi = (1u << 20 | 2u << 15 | 7u << 12 | 7u << 9 | 1u << 5 | 2u);
	gs.cc_lo = (15u << 28 | 15u << 24 | 7u << 21 | 7u << 18 | 7u << 15 | 7u << 12 | 6u << 9 | 7u << 6 | 7u << 3 | 6u);
	CHECK(!gfx_combine(&gs, &d));
	CHECK_EQ(d.nterms, 2);
	CHECK_EQ(d.ntex, 2);
	CHECK_EQ(d.factors[0][0], GFX_F_NONE);
	CHECK_EQ(d.factors[1][0], GFX_F_T0);
	CHECK_EQ(d.factors[1][1], GFX_F_T1);
	CHECK_EQ(d.factors[1][2], GFX_F_NONE);
	for (int lane = 0; lane < 3; lane++) {
		CHECK(d.k[1][0][lane] == 1.0f);
		CHECK(d.k[0][0][lane] == 0.0f);
	}
	CHECK(d.k[0][0][3] == 1.0f && d.k[1][0][3] == 0.0f);
}

/* lerp(ENV, TEXEL0, SHADE_ALPHA) en el primer ciclo y × SHADE en el
 * segundo (la niebla por luz precalculada de GoldenEye y Perfect Dark):
 * T0·S·Sa − E·S·Sa + E·S, con alfa (T0a − Ea)·Sa + Ea. Dos factores de
 * sombreado: va al generador con los términos T0·C (×Ca en X) y C (E − E·Ca),
 * más el término sin factores del alfa constante. */
static void gfx_combiner_shade_squared(void)
{
	start();
	struct gfx_draw d;
	memset(&d, 0, sizeof d);
	gs.oh = (uint32_t)G_CYC_2CYCLE << G_MDSFT_CYCLETYPE;
	gs.env = 0x40800040;  /* E = (0,25, 0,5, 0, 0,25) */
	gs.cc_hi = 0x159804;
	gs.cc_lo = 0x5ffedbf8;
	CHECK(!gfx_combine(&gs, &d));
	CHECK_EQ(d.ntex, 1);
	CHECK_EQ(d.nterms, 3);
	/* Orden canónico: {} < {T0, C} < {C}. */
	CHECK_EQ(d.factors[0][0], GFX_F_NONE);
	CHECK_EQ(d.factors[1][0], GFX_F_T0);
	CHECK_EQ(d.factors[1][1], GFX_F_C);
	CHECK_EQ(d.factors[1][2], GFX_F_NONE);
	CHECK_EQ(d.factors[2][0], GFX_F_C);
	CHECK_EQ(d.factors[2][1], GFX_F_NONE);
	const float e[4] = { 0x40 / 255.0f, 0x80 / 255.0f, 0.0f, 0x40 / 255.0f };
	for (int lane = 0; lane < 3; lane++) {
		CHECK(d.k[1][2][lane] == 1.0f);                   /* T0·C·Ca */
		CHECK(d.k[1][0][lane] == 0.0f && d.k[1][1][lane] == 0.0f);
		CHECK(fabsf(d.k[2][0][lane] - e[lane]) < 1e-6f);  /* E·C */
		CHECK(fabsf(d.k[2][2][lane] + e[lane]) < 1e-6f);  /* −E·C·Ca */
		CHECK(d.k[0][0][lane] == 0.0f);
	}
	/* Alfa: T0a·Sa en el término T0·C (carril 3 = T0a·Ca), −Ea·Sa en C
	 * (Ca) y Ea en el término sin factores. */
	CHECK(d.k[1][0][3] == 1.0f);
	CHECK(fabsf(d.k[2][0][3] + e[3]) < 1e-6f);
	CHECK(fabsf(d.k[0][0][3] - e[3]) < 1e-6f);

	/* Sin textura (apagada): se quita el término T0·C; con una, no cambia. */
	struct gfx_draw d2 = d;
	gfx_combine_textures(&d2, 1);
	CHECK(!memcmp(&d2, &d, sizeof d));
	gfx_combine_textures(&d2, 0);
	CHECK_EQ(d2.nterms, 2);
	CHECK_EQ(d2.factors[0][0], GFX_F_NONE);
	CHECK_EQ(d2.factors[1][0], GFX_F_C);
	CHECK(fabsf(d2.k[1][0][0] - e[0]) < 1e-6f);
	CHECK(d2.factors[2][0] == 0 && d2.k[2][0][0] == 0.0f);

	/* Con niebla y sin alfa constante hace falta el término sin factores
	 * igual: (T0 − 0)·SHADE_ALPHA·SHADE con alfa T0a·Sa. */
	memset(&d, 0, sizeof d);
	d.fog = 1;
	gs.cc_hi = 0x159804;
	gs.cc_lo = 0xfffffff8;  /* b = 0 y d = 0 en los dos ciclos; alfa del segundo = COMBINED */
	CHECK(!gfx_combine(&gs, &d));
	CHECK_EQ(d.nterms, 2);
	CHECK_EQ(d.factors[0][0], GFX_F_NONE);
	CHECK_EQ(d.factors[1][0], GFX_F_T0);
	CHECK_EQ(d.factors[1][1], GFX_F_C);
	CHECK(d.k[0][0][0] == 0.0f && d.k[0][0][3] == 0.0f);
	d.fog = 0;
	CHECK(!gfx_combine(&gs, &d));
	CHECK_EQ(d.nterms, 1);
	CHECK_EQ(d.factors[0][0], GFX_F_T0);
}

static void gfx_triangle_transform(void)
{
	start();
	viewport_320x240();
	load_mtx(ident, true);
	load_mtx(ident, false);
	set_combine(CC_A_ZERO, CC_ZERO, CC_C_ZERO, CC_SHADE, CC_ZERO, CC_ZERO, CC_ZERO, CC_SHADE);
	dl(0xD9000000, G_SHADING_SMOOTH | G_CULL_BACK);  /* geometry mode: solo estos */
	dl(0xFF100000 | 319, 0x00100000);                /* color image */
	const struct vtx v[3] = {
		{ 0, 0, 0, 0, 0, { 255, 0, 0, 255 } },
		{ 1, 0, 0, 0, 0, { 0, 255, 0, 255 } },
		{ 0, 1, 0, 0, 0, { 0, 0, 255, 255 } },
	};
	uint32_t va = put_vertices(v, 3);
	dl(0x01000000 | 3 << 12 | (0 + 3) << 1, va);  /* G_VTX: 3 desde 0 */
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0); /* G_TRI1 (índices ×2) */
	dl(0xE9000000, 0);                              /* G_RDPFULLSYNC */
	run();

	CHECK_EQ(res.triangles, 1);
	CHECK(res.flags & GFX_RES_FULLSYNC);
	const struct gfx_cmd *t = find_cmd(GFX_OP_TARGET, 0);
	CHECK(t != NULL);
	if (t) {
		CHECK_EQ(t->a, 0x100000);
		CHECK_EQ(t->b, 320);
	}
	const struct gfx_draw *d = draw_n(0);
	CHECK(d != NULL);
	if (!d)
		return;
	CHECK_EQ(d->h.a, 0);
	CHECK_EQ(d->h.b, 3);
	CHECK_EQ(d->cull, GFX_CULL_BACK);
	CHECK_EQ(d->ntex, 0);
	/* (0,0) al centro; (1,0) un viewport.x a la derecha; (0,1) arriba. */
	CHECK(out_vtx[0].x == 160.0f && out_vtx[0].y == 120.0f && out_vtx[0].w == 1.0f);
	CHECK(out_vtx[1].x == 320.0f && out_vtx[1].y == 120.0f);
	CHECK(out_vtx[2].x == 160.0f && out_vtx[2].y == 0.0f);
	CHECK(fabsf(out_vtx[0].z - 511.0f / 1023.0f) < 1e-6f);
	CHECK_EQ(out_vtx[1].g, 255);
	CHECK_EQ(out_vtx[2].b, 255);
}

/* GFX_JOB_INTERP: el G_VTX deja su registro GFX_OP_VGROUP y cada vértice de
 * salida su identidad en pad; los que agrega el recorte cercano, la de su
 * grupo y sus dos extremos. Sin la bandera, la salida es la de siempre (pad
 * = 0, sin registros nuevos) y con ella los vértices no cambian. */
static void interp_scene(uint32_t flags, int16_t z2)
{
	start();
	job_flags = flags;
	viewport_320x240();
	load_mtx(ident, true);
	load_mtx(ident, false);
	set_combine(CC_A_ZERO, CC_ZERO, CC_C_ZERO, CC_SHADE, CC_ZERO, CC_ZERO, CC_ZERO, CC_SHADE);
	dl(0xD9000000, G_SHADING_SMOOTH);
	dl(0xFF100000 | 319, 0x00100000);
	const struct vtx v[3] = {
		{ 0, 0, 0, 0, 0, { 255, 0, 0, 255 } },
		{ 1, 0, 0, 0, 0, { 0, 255, 0, 255 } },
		{ 0, 1, z2, 0, 0, { 0, 0, 255, 255 } },
	};
	uint32_t va = put_vertices(v, 3);
	dl(0x01000000 | 3 << 12 | (0 + 3) << 1, va);
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);
	dl(0xE9000000, 0);
	run();
}

static void gfx_interp_groups(void)
{
	static struct gfx_vertex plain[8];
	interp_scene(0, 0);
	CHECK_EQ(res.triangles, 1);
	CHECK(find_cmd(GFX_OP_VGROUP, 0) == NULL);
	for (int i = 0; i < 3; i++)
		CHECK_EQ(out_vtx[i].pad, 0);
	memcpy(plain, out_vtx, sizeof plain);
	uint32_t va = DATA_PA + 16 + 64 + 64;  /* tras el viewport y las dos matrices */
	interp_scene(GFX_JOB_INTERP, 0);
	const struct gfx_vgroup *r = (const struct gfx_vgroup *)find_cmd(GFX_OP_VGROUP, 0);
	CHECK(r != NULL);
	if (!r)
		return;
	CHECK_EQ(r->h.a, va);
	CHECK_EQ(r->h.b & 0xFF, 3);
	CHECK_EQ((r->h.b >> 8) & 0xFF, 0);
	CHECK_EQ(r->h.b >> 16, 16);
	CHECK_EQ(r->h.size, sizeof *r + 3 * 16);
	CHECK(r->vp[0] == 160.0f && r->vp[2] == 160.0f);
	const float *c = (const float *)(r + 1);
	CHECK(c[4] == 1.0f && c[7] == 1.0f);  /* el segundo vértice: x = 1, w = 1 */
	CHECK_EQ(gs.vgroups, 1);
	for (int i = 0; i < 3; i++) {
		CHECK_EQ(out_vtx[i].pad, GFX_VGID(1u, (uint32_t)i));
		CHECK(out_vtx[i].x == plain[i].x && out_vtx[i].y == plain[i].y && out_vtx[i].z == plain[i].z &&
		      out_vtx[i].w == plain[i].w);
	}
	/* El tercer vértice detrás del plano cercano (z < −w): dos triángulos,
	 * con dos vértices nuevos entre extremos con identidad. */
	interp_scene(GFX_JOB_INTERP, -2);
	CHECK_EQ(res.triangles, 1);
	const struct gfx_draw *d = draw_n(0);
	CHECK(d != NULL && d->h.b == 6);
	/* Los dos nuevos: entre los lugares 1 y 2, y 2 y 0, del grupo 1. */
	unsigned clipped = 0;
	for (int i = 0; i < 6; i++) {
		uint32_t id = out_vtx[i].pad;
		if (!(id & GFX_VCLIP_FLAG))
			continue;
		clipped++;
		CHECK_EQ(GFX_VCLIP_SERIAL(id), 1);
		CHECK((GFX_VCLIP_A(id) == 1 && GFX_VCLIP_B(id) == 2) || (GFX_VCLIP_A(id) == 2 && GFX_VCLIP_B(id) == 0));
	}
	CHECK_EQ(clipped, 3);  /* el segundo triángulo repite uno de los nuevos */
	interp_scene(0, -2);
	for (int i = 0; i < 6; i++)
		CHECK_EQ(out_vtx[i].pad, 0);
}

/* Los registros GFX_OP_VGROUP no pasan de un tercio del búfer de comandos
 * (GFX_VGROUP_SHARE): los que no caben faltan (GFX_RES_VGROUP_FULL) y sus
 * vértices quedan sin identidad, pero la tarea no se corta y dibuja lo
 * suyo. El hash del contenido es FNV-1 de las palabras big-endian de los 16
 * bytes de cada vértice. */
static void gfx_interp_budget(void)
{
	start();
	job_flags = GFX_JOB_INTERP;
	job_cmd_cap = 2048;  /* 682 bytes para los registros: 7 de 96 */
	viewport_320x240();
	load_mtx(ident, true);
	load_mtx(ident, false);
	set_combine(CC_A_ZERO, CC_ZERO, CC_C_ZERO, CC_SHADE, CC_ZERO, CC_ZERO, CC_ZERO, CC_SHADE);
	dl(0xD9000000, G_SHADING_SMOOTH);
	dl(0xFF100000 | 319, 0x00100000);
	const struct vtx v[3] = {
		{ 0, 0, 0, 0, 0, { 255, 0, 0, 255 } },
		{ 1, 0, 0, 0, 0, { 0, 255, 0, 255 } },
		{ 0, 1, 0, 0, 0, { 0, 0, 255, 255 } },
	};
	uint32_t va = put_vertices(v, 3);
	for (int i = 0; i < 10; i++)
		dl(0x01000000 | 3 << 12 | (0 + 3) << 1, va);
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);
	dl(0xE9000000, 0);
	run();
	CHECK_EQ(res.triangles, 1);
	CHECK(res.flags & GFX_RES_VGROUP_FULL);
	CHECK(!(res.flags & GFX_RES_CUT));
	CHECK(res.flags & GFX_RES_FULLSYNC);
	CHECK_EQ(gs.vgroups, 7);
	CHECK(find_cmd(GFX_OP_VGROUP, 6) != NULL && find_cmd(GFX_OP_VGROUP, 7) == NULL);
	CHECK(draw_n(0) != NULL);
	for (int i = 0; i < 3; i++)
		CHECK_EQ(out_vtx[i].pad, 0);
	const struct gfx_vgroup *r = (const struct gfx_vgroup *)find_cmd(GFX_OP_VGROUP, 0);
	CHECK(r != NULL);
	if (r) {
		uint32_t h = 0x811C9DC5u;
		for (uint32_t k = 0; k < 12; k++)
			h = (h ^ salsa64_rd32(t_n64.bus.rdram + va + 4 * k)) * 0x01000193u;
		CHECK_EQ(r->h.c, h ? h : 1u);
	}
}

static void gfx_matrix_stack(void)
{
	start();
	viewport_320x240();
	load_mtx(ident, true);
	load_mtx(ident, false);
	const float tr[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.5f, 0, 0, 1 };
	const float sc[16] = { 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1 };
	dl(0xDA380000 | 0x00, put_matrix(tr));  /* PUSH (bit 0 en 0) y MUL */
	dl(0xDA380000 | 0x00, put_matrix(sc));  /* PUSH y MUL: M = sc · tr */
	const struct vtx v[1] = { { 0, 0, 0, 0, 0, { 0, 0, 0, 0 } } };
	uint32_t va = put_vertices(v, 1);
	dl(0x01000000 | 1 << 12 | 1 << 1, va);
	const struct vtx v1[1] = { { 1, 0, 0, 0, 0, { 0, 0, 0, 0 } } };
	uint32_t vb = put_vertices(v1, 1);
	dl(0x01000000 | 1 << 12 | 2 << 1, vb);  /* en el índice 1 */
	dl(0xD8380002, 64);                      /* POPMTX 1 */
	dl(0x01000000 | 1 << 12 | 3 << 1, vb);  /* índice 2, con solo tr */
	run();
	/* (0,0,0)·sc·tr = (0.5,0,0); (1,0,0)·sc·tr = (2.5,0,0); tras pop (1)·tr = 1.5. */
	CHECK(fabsf(gs.vtx[0].x - 0.5f) < 1e-4f);
	CHECK(fabsf(gs.vtx[1].x - 2.5f) < 1e-4f);
	CHECK(fabsf(gs.vtx[2].x - 1.5f) < 1e-4f);
	CHECK_EQ(gs.mv_top, 1);
}

/* Billboard de Kirby 64 (y del letrero de Smash): tras el G_MTX del objeto
 * y un G_SPECIAL_1, los G_MW_MATRIX (gSPInsertMatrix) cambian la parte 3×3
 * de la MVP por una escala que mira a la cámara; la traslación queda. La
 * HLE calcula la MVP pendiente al insertar y el siguiente G_MTX la vuelve a
 * calcular, como el RSP. */
static void gfx_insert_matrix_billboard(void)
{
	start();
	viewport_320x240();
	load_mtx(ident, true);
	/* Giro de 90° en z y traslación (0.5, 0, 0). */
	const float rot[16] = { 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 0.5f, 0, 0, 1 };
	load_mtx(rot, false);
	dl(0xD5000001, 0);           /* G_SPECIAL_1 */
	dl(0xDB000000, 0x00020000);  /* enteras de los elementos 0 y 1: 2, 0 */
	dl(0xDB000008, 0x00000003);  /* 4 y 5: 0, 3 */
	dl(0xDB000014, 0x00010000);  /* 10 y 11: 1, 0 */
	const struct vtx v[1] = { { 1, 1, 0, 0, 0, { 0, 0, 0, 0 } } };
	uint32_t va = put_vertices(v, 1);
	dl(0x01000000 | 1 << 12 | 1 << 1, va);
	dl(0xDA380000 | 0x01, put_matrix(ident));  /* NOPUSH y MUL: la MVP de nuevo */
	dl(0x01000000 | 1 << 12 | 2 << 1, va);     /* en el índice 1 */
	run();
	/* (1,1,0) con la escala (2,3,1) más la traslación; después, girado. */
	CHECK(fabsf(gs.vtx[0].x - 2.5f) < 1e-4f);
	CHECK(fabsf(gs.vtx[0].y - 3.0f) < 1e-4f);
	CHECK(fabsf(gs.vtx[1].x + 0.5f) < 1e-4f);
	CHECK(fabsf(gs.vtx[1].y - 1.0f) < 1e-4f);
	CHECK_EQ(res.unknown_cmds, 0);
}

static void gfx_lighting(void)
{
	start();
	viewport_320x240();
	load_mtx(ident, true);
	load_mtx(ident, false);
	dl(0xD9000000, G_LIGHTING | G_SHADING_SMOOTH);
	dl(0xDB020000, 1 * 24);  /* G_MW_NUMLIGHT: una luz */
	/* Luz 0: blanca a 200, dirección +z; ambiental (luz 1): 20. */
	uint32_t l0 = data_alloc(16), l1 = data_alloc(16);
	uint8_t *p = t_n64.bus.rdram;
	p[l0 + 0] = p[l0 + 1] = p[l0 + 2] = 200;
	p[l0 + 4] = p[l0 + 5] = p[l0 + 6] = 200;
	p[l0 + 8] = 0, p[l0 + 9] = 0, p[l0 + 10] = 127;
	p[l1 + 0] = p[l1 + 1] = p[l1 + 2] = 20;
	dl(0xDC08000A | (2 * 24 / 8) << 8, l0);  /* G_MOVEMEM luz 0 (offset 48) */
	dl(0xDC08000A | (3 * 24 / 8) << 8, l1);  /* luz 1 = ambiental */
	const struct vtx v[2] = {
		{ 0, 0, 0, 0, 0, { 0, 0, 127, 0x40 } },   /* normal hacia la luz */
		{ 0, 0, 0, 0, 0, { 0, 0, (uint8_t)-127, 0x40 } },  /* de espaldas */
	};
	dl(0x01000000 | 2 << 12 | 2 << 1, put_vertices(v, 2));
	run();
	CHECK_EQ(gs.vtx[0].c[0], 220);
	CHECK_EQ(gs.vtx[0].c[3], 0x40);
	CHECK_EQ(gs.vtx[1].c[1], 20);
}

static void gfx_texgen(void)
{
	start();
	viewport_320x240();
	load_mtx(ident, true);
	load_mtx(ident, false);
	dl(0xD9000000, G_LIGHTING | G_TEXTURE_GEN);
	dl(0xD7000002 | 0 << 8, 0x07C007C0);  /* G_TEXTURE on, escala 0x07C0 */
	uint32_t lx = data_alloc(16), ly = data_alloc(16);
	uint8_t *p = t_n64.bus.rdram;
	p[lx + 8] = 127;  /* lookat X = +x */
	p[ly + 9] = 127;  /* lookat Y = +y */
	dl(0xDC08000A | 0 << 8, lx);
	dl(0xDC08000A | (24 / 8) << 8, ly);
	const struct vtx v[2] = {
		{ 0, 0, 0, 0, 0, { 127, 0, 0, 255 } },  /* normal +x: s al máximo */
		{ 0, 0, 0, 0, 0, { 0, 0, 127, 255 } },  /* normal +z: centro */
	};
	dl(0x01000000 | 2 << 12 | 2 << 1, put_vertices(v, 2));
	run();
	/* (d + 1)·512 texels × 0x07C0/65536: 31 al máximo y 15,5 al centro. */
	CHECK(fabsf(gs.vtx[0].s - 31.0f) < 0.05f);
	CHECK(fabsf(gs.vtx[0].t - 15.5f) < 0.05f);
	CHECK(fabsf(gs.vtx[1].s - 15.5f) < 0.05f);
}

/* Textura RGBA16 de 4×2 cargada con LOADBLOCK (dxt de una fila de 8 bytes:
 * 2048, la segunda fila va intercambiada) y dibujada en un rectángulo. */
static void gfx_tmem_rgba16_block(void)
{
	start();
	uint16_t texels[8];
	for (int i = 0; i < 8; i++)
		texels[i] = (uint16_t)((i * 4) << 11 | 1);  /* rojo creciente, opaco */
	for (int i = 0; i < 8; i++)
		salsa64_wr16(t_n64.bus.rdram + TEX_PA + 2 * i, texels[i]);
	dl(0xFD100000 | 0, TEX_PA);                         /* SETTIMG RGBA16, ancho 1 */
	dl(0xF5100000, 0x07000000);                         /* SETTILE 7 (de carga), tmem 0 */
	dl(0xF3000000, 0x07000000 | 7 << 12 | 2048);         /* LOADBLOCK 8 texels, dxt 1 línea */
	dl(0xF5100000 | 1 << 9, 0x00000000);                 /* SETTILE 0: line 1 palabra */
	dl(0xF2000000, 0x00000000 | (3 << 2) << 12 | (1 << 2)); /* tile 0: 4×2 */
	set_cycle(G_CYC_COPY);
	/* TEXRECT 0,0–3,1 (copia: inclusivo), s=t=0, dsdx = 4.0. */
	dl(0xE4000000 | (3 << 2) << 12 | (1 << 2), 0);
	dl(0xE1000000, 0);
	dl(0xF1000000, 0x10000400);
	run();
	CHECK_EQ(res.rects, 1);
	CHECK_EQ(res.textures, 1);
	const struct gfx_draw *d = draw_n(0);
	CHECK(d != NULL);
	if (!d)
		return;
	CHECK_EQ(d->cycle, G_CYC_COPY);
	CHECK_EQ(d->ntex, 1);
	CHECK_EQ(d->tex[0].width, 4);
	CHECK_EQ(d->tex[0].height, 2);
	CHECK_EQ(d->h.b, 6);
	const uint8_t *tex = out_tex + d->tex[0].offset;
	for (int y = 0; y < 2; y++)
		for (int x = 0; x < 4; x++) {
			uint32_t r = (uint32_t)(y * 4 + x) * 4;
			CHECK_EQ(tex[y * d->tex[0].pitch + 4 * x], r << 3 | r >> 2);
			CHECK_EQ(tex[y * d->tex[0].pitch + 4 * x + 3], 0xFF);
		}
	/* Esquinas: 0..4 × 0..2 píxeles, s 0..4, t 0..2 texels. */
	CHECK(out_vtx[0].x == 0.0f && out_vtx[0].y == 0.0f && out_vtx[0].s == 0.0f);
	CHECK(out_vtx[1].x == 4.0f && fabsf(out_vtx[1].s - 4.0f) < 1e-5f);
	CHECK(out_vtx[2].y == 2.0f && fabsf(out_vtx[2].t - 2.0f) < 1e-5f);
	/* u = s·xf0 + xf2 normaliza a la textura. */
	CHECK(fabsf(d->tex[0].xf[0] - 0.25f) < 1e-6f);
}

/* (TEXEL0 − SHADE)·TEXEL0_ALPHA + SHADE con alfa ENV (G_SETCOMBINE de los
 * ojos de Mario en SM64): T0·T0a pide la textura premultiplicada y queda
 * exacto; con T0 solo además, se sigue aproximando. */
#define CC_EYES_HI 0x147E28u
#define CC_EYES_LO 0x44FE7B3Du

static void gfx_combiner_premul(void)
{
	start();
	struct gfx_draw d;
	memset(&d, 0, sizeof d);
	gs.oh = 0;
	gs.env = 0x000000C0;
	gs.cc_hi = CC_EYES_HI;
	gs.cc_lo = CC_EYES_LO;
	CHECK(!gfx_combine(&gs, &d));
	CHECK(d.tex[0].flags & GFX_TEX_PREMUL);
	CHECK_EQ(d.ntex, 1);
	for (int lane = 0; lane < 3; lane++) {
		CHECK(d.k[GFX_K_T0][0][lane] == 1.0f);    /* T0·T0a: la premultiplicada */
		CHECK(d.k[GFX_K_T0A][1][lane] == -1.0f);  /* −S·T0a */
		CHECK(d.k[GFX_K_ONE][1][lane] == 1.0f);   /* + S */
	}
	CHECK(fabsf(d.k[GFX_K_ONE][0][3] - 0xC0 / 255.0f) < 1e-6f);  /* alfa = ENV */

	/* (TEXEL0 − 0)·TEXEL0_ALPHA + TEXEL0: T0·T0a + T0. Ninguna textura
	 * sirve para los dos: va al generador, exacto, sin premultiplicar. */
	gs.cc_lo = 15u << 28 | 15u << 24 | 7u << 21 | 7u << 18 | 1u << 15 | 7u << 12 | 5u << 9 | 1u << 6 | 7u << 3 | 5u;
	CHECK(!gfx_combine(&gs, &d));
	CHECK(!(d.tex[0].flags & GFX_TEX_PREMUL));
	CHECK_EQ(d.nterms, 3);
	CHECK_EQ(d.factors[0][0], GFX_F_NONE);
	CHECK_EQ(d.factors[1][0], GFX_F_T0);
	CHECK_EQ(d.factors[1][1], GFX_F_NONE);
	CHECK_EQ(d.factors[2][0], GFX_F_T0);
	CHECK_EQ(d.factors[2][1], GFX_F_T0A);
	for (int lane = 0; lane < 3; lane++)
		CHECK(d.k[1][0][lane] == 1.0f && d.k[2][0][lane] == 1.0f && d.k[0][0][lane] == 0.0f);
	/* Alfa = ENVa, constante: el término sin factores. */
	CHECK(fabsf(d.k[0][0][3] - 0xC0 / 255.0f) < 1e-6f);
	CHECK(d.k[1][0][3] == 0.0f && d.k[2][0][3] == 0.0f);
}

/* La misma textura premultiplicada al decodificar: alfa 0 → negro. */
static void gfx_tmem_premul(void)
{
	start();
	for (int i = 0; i < 8; i++)  /* amarillo; alfa 0 en los pares, 1 en los impares */
		salsa64_wr16(t_n64.bus.rdram + TEX_PA + 2 * i, (uint16_t)(31u << 11 | 31u << 6 | (unsigned)(i & 1)));
	dl(0xFD100000, TEX_PA);
	dl(0xF5100000, 0x07000000);
	dl(0xF3000000, 0x07000000 | 7 << 12 | 2048);
	dl(0xF5100000 | 1 << 9, 0x00000000);
	dl(0xF2000000, (3 << 2) << 12 | (1 << 2));
	set_cycle(G_CYC_1CYCLE);
	dl(0xFC000000 | CC_EYES_HI, CC_EYES_LO);
	dl(0xE4000000 | (4 << 2) << 12 | (2 << 2), 0);
	dl(0xE1000000, 0);
	dl(0xF1000000, 0x04000400);
	run();
	CHECK_EQ(res.approx_cc, 0);
	const struct gfx_draw *d = draw_n(0);
	CHECK(d != NULL);
	if (!d)
		return;
	CHECK(d->tex[0].flags & GFX_TEX_PREMUL);
	const uint8_t *tex = out_tex + d->tex[0].offset;
	CHECK_EQ(tex[0], 0);  /* texel 0, alfa 0 */
	CHECK_EQ(tex[1], 0);
	CHECK_EQ(tex[3], 0);
	CHECK_EQ(tex[4], 0xFF);  /* texel 1, opaco: sin cambios */
	CHECK_EQ(tex[5], 0xFF);
	CHECK_EQ(tex[6], 0);
	CHECK_EQ(tex[7], 0xFF);
}

/* CI4 con paleta RGBA16 (LOADTLUT) cargada con LOADTILE. */
static void gfx_tmem_ci4_tlut(void)
{
	start();
	/* Paleta de 16 colores: entrada i = azul i·2, opaco. */
	for (int i = 0; i < 16; i++)
		salsa64_wr16(t_n64.bus.rdram + TEX_PA + 0x100 + 2 * i, (uint16_t)((i * 2) << 1 | 1));
	/* Textura 8×2 CI4: índices 0..15. */
	for (int i = 0; i < 8; i++)
		t_n64.bus.rdram[TEX_PA + i] = (uint8_t)((2 * i) << 4 | (2 * i + 1));
	/* TLUT: SETTIMG RGBA16, SETTILE 7 en tmem 256 (palabra 0x100), LOADTLUT 16. */
	dl(0xFD100000, TEX_PA + 0x100);
	dl(0xF5000100, 0x07000000);
	dl(0xF0000000, 0x07000000 | (15 << 2) << 12);
	/* Textura: SETTIMG CI8 de 4 bytes de ancho (8 texels de 4 bits). */
	dl(0xFD480000 | 3, TEX_PA);
	dl(0xF5480000 | 1 << 9, 0x07000000);
	dl(0xF4000000, 0x07000000 | (3 << 2) << 12 | (1 << 2));  /* LOADTILE 4×2 bytes */
	dl(0xF5400000 | 1 << 9, 0x00000000);                      /* tile 0: CI4, línea 8 bytes */
	dl(0xF2000000, (7 << 2) << 12 | (1 << 2));                /* 8×2 */
	dl(0xE3000000 | (32 - 14 - 2) << 8 | 1, 2u << 14);        /* TLUT RGBA16 */
	set_cycle(G_CYC_COPY);
	dl(0xE4000000 | (7 << 2) << 12 | (0 << 2), 0);
	dl(0xE1000000, 0);
	dl(0xF1000000, 0x10000400);
	run();
	const struct gfx_draw *d = draw_n(0);
	CHECK(d != NULL);
	if (!d)
		return;
	CHECK_EQ(d->tex[0].width, 8);
	const uint8_t *tex = out_tex + d->tex[0].offset;
	for (int x = 0; x < 8; x++) {
		uint32_t b5 = (uint32_t)x * 2;
		CHECK_EQ(tex[4 * x + 2], b5 << 3 | b5 >> 2);
	}
	/* Segunda fila (impar): la carga la intercambió y la lectura lo deshace. */
	for (int x = 0; x < 8; x++) {
		uint32_t b5 = (uint32_t)(x + 8) * 2;
		CHECK_EQ(tex[d->tex[0].pitch + 4 * x + 2], b5 << 3 | b5 >> 2);
	}
}

static void gfx_fill_and_zclear(void)
{
	start();
	dl(0xFE000000, 0x00200000);           /* z image */
	dl(0xFF100000 | 319, 0x00200000);     /* color image = z image */
	set_cycle(G_CYC_FILL);
	dl(0xF7000000, 0xFFFCFFFC);
	dl(0xF6000000 | (319 << 2) << 12 | (239 << 2), 0);
	dl(0xFF100000 | 319, 0x00100000);     /* color image de verdad */
	dl(0xF7000000, 0xF801F801);           /* rojo en RGBA5551 */
	dl(0xF6000000 | (319 << 2) << 12 | (239 << 2), 0);
	run();
	const struct gfx_zclear *z = (const struct gfx_zclear *)find_cmd(GFX_OP_ZCLEAR, 0);
	CHECK(z != NULL);
	if (z) {
		CHECK_EQ(z->h.a, 0x200000);
		CHECK_EQ(z->h.b, 0xFFFC);
		CHECK_EQ(z->h.c, 2u << 16 | 320);  /* 16 bits, 320 de ancho */
		CHECK_EQ(z->fill, 0xFFFCFFFC);
		CHECK_EQ(z->rect[2], 320);
		CHECK_EQ(z->rect[3], 240);
	}
	const struct gfx_draw *d = draw_n(0);
	CHECK(d != NULL);
	if (d) {
		CHECK_EQ(d->cycle, G_CYC_FILL);
		CHECK(d->k[GFX_K_ONE][0][0] == 1.0f && d->k[GFX_K_ONE][0][1] == 0.0f);
		CHECK_EQ(d->h.b, 6);
	}
	CHECK(out_vtx[5].x == 320.0f && out_vtx[5].y == 240.0f);
	/* El TARGET de la color image real llega antes del dibujo. */
	CHECK(find_cmd(GFX_OP_TARGET, 0) != NULL);
}

/* Cruis'n Exotica ejecuta con G_SPECIAL_1 un bloque contado de comandos
 * F3DEX2 y vuelve a la lista principal tras la cantidad indicada. */
static void gfx_eurocom_rdp_block(void)
{
	start();
	job_gbi = SALSA64_GBI_F3DEX2EUROCOM;
	uint32_t block = data_alloc(4 * 8);
	t_wr32(block + 0, 0xFF100000 | 319);       /* color image RGBA16, 320 */
	t_wr32(block + 4, 0x00100000);
	t_wr32(block + 8, 0xE3000A01);             /* ciclo fill */
	t_wr32(block + 12, G_CYC_FILL << 20);
	t_wr32(block + 16, 0xF7000000);            /* color rojo */
	t_wr32(block + 20, 0xF801F801);
	t_wr32(block + 24, 0xF6000000 | (31 << 2) << 12 | (15 << 2));
	t_wr32(block + 28, 0);
	dl(0xD5000004, block);
	run();
	CHECK_EQ(res.rects, 1);
	CHECK_EQ(res.dl_errors, 0);
	CHECK(find_cmd(GFX_OP_TARGET, 0) != NULL);
	CHECK(draw_n(0) != NULL);

	start();
	job_gbi = SALSA64_GBI_F3DEX2;  /* el F3DEX2 normal conserva D5 como no-op */
	dl(0xD5000004, block);
	run();
	CHECK_EQ(res.rects, 0);
}

/* El relleno de la z image también va a la RDRAM, como lo deja el RDP
 * (Donkey Kong 64 lee la profundidad donde se ve al jugador y, si lee 0,
 * lo cree tapado y mueve la cámara cada 21 cuadros). */
#define ZF_PA 0x200000u
#define ZF_W  64u

static uint16_t zf_rd16(uint32_t pa)
{
	return (uint16_t)(t_rd32(pa & ~3u) >> ((pa & 2) ? 0 : 16));
}

static void zf_prepare(void)
{
	for (uint32_t pa = ZF_PA; pa < ZF_PA + ZF_W * 8 * 2; pa += 4)
		t_wr32(pa, 0x12345678);
	dl(0xFE000000, ZF_PA);                       /* z image */
	dl(0xFF100000 | (ZF_W - 1), ZF_PA);          /* color image = z image */
	set_cycle(G_CYC_FILL);
	dl(0xF7000000, 0xFFFC0002);  /* mitades distintas: se ve cuál va en cada píxel */
	dl(0xF6000000 | (9 << 2) << 12 | (4 << 2), (3 << 2) << 12 | (2 << 2));    /* x 3..9, y 2..4 */
	dl(0xF6000000 | (70 << 2) << 12 | (6 << 2), (60 << 2) << 12 | (6 << 2));  /* x 60..70: sale del ancho */
}

/* Cada píxel del rectángulo (dentro del ancho) con la mitad del color que le
 * toca por su dirección; lo demás, intacto. */
static void zf_check(bool filled)
{
	for (uint32_t y = 0; y < 8; y++)
		for (uint32_t x = 0; x < ZF_W; x++) {
			uint32_t pa = ZF_PA + (y * ZF_W + x) * 2;
			bool in = filled && ((x >= 3 && x <= 9 && y >= 2 && y <= 4) || (x >= 60 && y == 6));
			uint16_t want = in ? ((pa & 2) ? 0x0002 : 0xFFFC) : ((pa & 2) ? 0x5678 : 0x1234);
			if (zf_rd16(pa) != want) {
				CHECK_EQ(zf_rd16(pa), want);
				return;
			}
		}
}

static void gfx_zclear_fills_rdram(void)
{
	start();
	zf_prepare();
	run();
	zf_check(false);  /* gfx_run no escribe la RDRAM */
	salsa64_gfx_rdram_fills(&t_n64.bus, &job, &res);
	zf_check(true);
}

/* El relleno se recorta con el scissor como en el RDP (10.2, borde inferior
 * derecho exclusivo). Conker borra una z image de 292 de ancho con un
 * rectángulo de 217 filas y un scissor de 216, y guarda una lista enlazada
 * en la fila 216, que el RDP no toca: pisarla lo colgaba tras el logo. */
static void gfx_zclear_scissor(void)
{
	const uint32_t pa = 0x200000, w = 292;
	start();
	for (uint32_t a = pa; a < pa + w * 218 * 2; a += 4)
		t_wr32(a, 0x12345678);
	dl(0xFE000000, pa);                                            /* z image */
	dl(0xFF100000 | (w - 1), pa);                                  /* color image = z image */
	dl(0xED000000 | (2 << 2) << 12, (290 << 2) << 12 | (216 << 2)); /* scissor (2, 0)–(290, 216) */
	set_cycle(G_CYC_FILL);
	dl(0xF7000000, 0xFFFCFFFC);
	dl(0xF6000000 | (291 << 2) << 12 | (216 << 2), 0);             /* 0..291 × 0..216 */
	run();
	const struct gfx_zclear *z = (const struct gfx_zclear *)find_cmd(GFX_OP_ZCLEAR, 0);
	CHECK(z != NULL);
	if (!z)
		return;
	CHECK_EQ(z->rect[0], 2);
	CHECK_EQ(z->rect[1], 0);
	CHECK_EQ(z->rect[2], 290);
	CHECK_EQ(z->rect[3], 216);
	salsa64_gfx_rdram_fills(&t_n64.bus, &job, &res);
	CHECK_EQ(zf_rd16(pa + 2 * 1), 0x5678);                    /* x = 1: fuera */
	CHECK_EQ(zf_rd16(pa + 2 * 2), 0xFFFC);
	CHECK_EQ(zf_rd16(pa + 2 * (215 * w + 289)), 0xFFFC);      /* última fila y columna */
	CHECK_EQ(zf_rd16(pa + 2 * (215 * w + 290)), 0x1234);
	for (uint32_t x = 0; x < w; x++)                           /* la fila 216, intacta */
		if (zf_rd16(pa + 2 * (216 * w + x)) != (x & 1 ? 0x5678 : 0x1234)) {
			CHECK_EQ(x, w);
			break;
		}

	/* Si el scissor lo deja vacío no se borra nada. */
	start();
	dl(0xFE000000, pa);
	dl(0xFF100000 | (w - 1), pa);
	dl(0xED000000 | (10 << 2) << 12 | (10 << 2), (10 << 2) << 12 | (20 << 2));
	set_cycle(G_CYC_FILL);
	dl(0xF7000000, 0xFFFCFFFC);
	dl(0xF6000000 | (291 << 2) << 12 | (216 << 2), 0);
	run();
	CHECK(find_cmd(GFX_OP_ZCLEAR, 0) == NULL);
}

/* En el backend síncrono el relleno llega al terminar la tarea (finish),
 * cuando el RDP la daría por hecha, no al arrancarla. */
static struct gfx_state zf_state;
static uint32_t zf_flags;

static void zf_acquire(void *ctx, struct gfx_job *j)
{
	(void)ctx;
	zf_flags = j->flags;
	j->vtx_ea = (uint64_t)(uintptr_t)out_vtx;
	j->vtx_cap = sizeof out_vtx;
	j->cmd_ea = (uint64_t)(uintptr_t)out_cmd;
	j->cmd_cap = sizeof out_cmd;
	j->tex_ea = (uint64_t)(uintptr_t)out_tex;
	j->tex_cap = sizeof out_tex;
}

static void zf_done(void *ctx, const struct gfx_job *j, const struct gfx_result *r)
{
	(void)ctx, (void)j, (void)r;
}

static void gfx_sync_dpc_nocache(void)
{
	static struct salsa64_gfx_sync s;
	const struct salsa64_gfx_sink sink = { NULL, zf_acquire, zf_done };
	uint8_t dmem[16] __attribute__((aligned(16))) = { 0 };
	salsa64_wr32(dmem, 0xE9000000);  /* FULLSYNC */
	salsa64_gfx_sync_init(&s, &zf_state, &t_n64.bus, &sink);
	s.dpc.run(s.dpc.ctx, 0, 8, dmem);
	CHECK(!(zf_flags & GFX_JOB_NOCACHE));
	s.nocache = true;
	s.dpc.run(s.dpc.ctx, 0, 8, dmem);
	CHECK(zf_flags & GFX_JOB_NOCACHE);
}

static void gfx_zclear_rdram_at_finish(void)
{
	static struct salsa64_gfx_sync s;
	const struct salsa64_gfx_sink sink = { NULL, zf_acquire, zf_done };
	start();
	zf_prepare();
	dl(0xDF000000, 0);  /* G_ENDDL */
	salsa64_gfx_sync_init(&s, &zf_state, &t_n64.bus, &sink);
	struct salsa64_task task;
	memset(&task, 0, sizeof task);
	task.type = SALSA64_TASK_GFX;
	task.data_ptr = DL_PA;
	s.backend.start(s.backend.ctx, &task, SALSA64_GBI_F3DEX2);
	zf_check(false);
	s.backend.finish(s.backend.ctx);
	zf_check(true);
}

/* FILLRECT en 1 ciclo: borde inferior derecho exclusivo, salvo cuando se
 * queda a un píxel del scissor (el fundido de Zelda manda 319, 239). */
static void gfx_fill_1cycle_edge(void)
{
	start();
	dl(0xFF100000 | 319, 0x00100000);
	dl(0xED000000, (320 << 2) << 12 | (240 << 2));  /* scissor 320×240 */
	set_cycle(G_CYC_1CYCLE);
	dl(0xF6000000 | (100 << 2) << 12 | (50 << 2), (10 << 2) << 12 | (10 << 2));
	dl(0xF6000000 | (319 << 2) << 12 | (239 << 2), 0);
	run();
	const struct gfx_draw *d = draw_n(0);
	CHECK(d != NULL);
	if (d)
		CHECK_EQ(d->h.b, 12);
	/* Vértice 5: abajo a la derecha de cada rectángulo. */
	CHECK(out_vtx[5].x == 100.0f && out_vtx[5].y == 50.0f);
	CHECK(out_vtx[11].x == 320.0f && out_vtx[11].y == 240.0f);
}

/* Triángulos seguidos con el mismo estado comparten dibujo; un cambio de
 * color primitivo abre otro. */
static void gfx_draw_batching(void)
{
	start();
	viewport_320x240();
	load_mtx(ident, true);
	load_mtx(ident, false);
	set_combine(CC_A_ZERO, CC_ZERO, CC_C_ZERO, CC_PRIM, CC_ZERO, CC_ZERO, CC_ZERO, CC_PRIM);
	const struct vtx v[4] = {
		{ 0, 0, 0, 0, 0, { 0 } }, { 1, 0, 0, 0, 0, { 0 } }, { 0, 1, 0, 0, 0, { 0 } }, { 1, 1, 0, 0, 0, { 0 } },
	};
	dl(0x01000000 | 4 << 12 | 4 << 1, put_vertices(v, 4));
	dl(0xFA000000, 0xFF0000FF);
	dl(0x06000000 | 0 << 16 | 1 << 9 | 2 << 1, 1 << 17 | 3 << 9 | 2 << 1);  /* TRI2 */
	dl(0xE7000000, 0);                                                     /* pipesync: nada */
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);
	dl(0xFA000000, 0x00FF00FF);
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);
	run();
	CHECK_EQ(res.triangles, 4);
	CHECK_EQ(res.draws, 2);
	const struct gfx_draw *a = draw_n(0), *b = draw_n(1);
	CHECK(a && b);
	if (a && b) {
		CHECK_EQ(a->h.b, 9);
		CHECK_EQ(b->h.a, 9);
		CHECK_EQ(b->h.b, 3);
		CHECK(a->k[GFX_K_ONE][0][0] == 1.0f && b->k[GFX_K_ONE][0][1] == 1.0f);
	}
}

/* Prueba de alfa que sale de la cobertura (CVG_X_ALPHA). Sin FORCE_BL
 * (G_RM_AA_ZB_TEX_EDGE) se recorta en la mitad; con FORCE_BL y AA_EN
 * (G_RM_TEX_EDGE: el medidor de energía de SM64, que salía con su fondo
 * transparente opaco) el RDP no escribe lo que tiene alfa < 32; sin AA_EN
 * la cobertura no depende del alfa. */
static void gfx_cvg_x_alpha(void)
{
	static const struct { uint32_t ol; uint8_t atest, aref; } c[] = {
		{ 0x00553078, GFX_ATEST_GEQUAL, 128 },  /* G_RM_AA_ZB_TEX_EDGE */
		{ 0x0F0A7008, GFX_ATEST_GEQUAL, 32 },   /* G_RM_TEX_EDGE */
		{ 0x0F0A7000, GFX_ATEST_NONE, 0 },      /* el mismo sin AA_EN */
		{ 0x0F0A4000, GFX_ATEST_NONE, 0 },      /* G_RM_OPA_SURF */
	};
	const struct vtx v[3] = { { 0, 0, 0, 0, 0, { 0 } }, { 1, 0, 0, 0, 0, { 0 } }, { 0, 1, 0, 0, 0, { 0 } } };
	for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
		start();
		viewport_320x240();
		load_mtx(ident, true);
		load_mtx(ident, false);
		set_combine(CC_A_ZERO, CC_ZERO, CC_C_ZERO, CC_SHADE, CC_ZERO, CC_ZERO, CC_ZERO, CC_SHADE);
		dl(0xEF000000 | G_CYC_1CYCLE << 20, c[i].ol);  /* G_RDPSETOTHERMODE */
		dl(0x01000000 | 3 << 12 | 3 << 1, put_vertices(v, 3));
		dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);
		run();
		const struct gfx_draw *d = draw_n(0);
		CHECK(d != NULL);
		if (!d)
			continue;
		CHECK_EQ(d->atest, c[i].atest);
		CHECK_EQ(d->aref, c[i].aref);
		CHECK_EQ(d->blend, GFX_BLEND_OFF);
	}
}

/* El bit de alfa que el RDP deja en una imagen de 16 bits (gfx_draw.cvg_bit):
 * la cobertura según CVG_DST, IM_RD, CVG_X_ALPHA y FORCE_BL, no el alfa de
 * la mezcla. Los paneles translúcidos del fondo de Dr. Mario 64 (ZAP,
 * FORCE_BL, IM_RD, alfa 0,375) dejan 1 en la consola (#62). */
static void gfx_cvg_bit(void)
{
	static const struct { uint32_t ol; uint8_t cvg; } c[] = {
		{ 0x00552078, GFX_CVG_ONE },   /* G_RM_AA_ZB_OPA_SURF: CLAMP */
		{ 0x0F0A4000, GFX_CVG_ONE },   /* G_RM_OPA_SURF: CLAMP */
		{ 0x0C184240, GFX_CVG_ONE },   /* Dr. Mario: ZAP, FORCE_BL, IM_RD */
		{ 0x00504241, GFX_CVG_ONE },   /* el otro de Dr. Mario, con G_AC_THRESHOLD */
		{ 0x005049D8, GFX_CVG_KEEP },  /* G_RM_AA_ZB_XLU_SURF: WRAP con IM_RD */
		{ 0x0F0A4100, GFX_CVG_ONE },   /* WRAP sin IM_RD: la memoria vale 7 */
		{ 0x0F0A4340, GFX_CVG_KEEP },  /* SAVE con IM_RD */
		{ 0x0F0A4300, GFX_CVG_ONE },   /* SAVE sin IM_RD */
		{ 0x00553078, GFX_CVG_ALPHA }, /* G_RM_AA_ZB_TEX_EDGE: CVG_X_ALPHA sin FORCE_BL */
		{ 0x0F0A7008, GFX_CVG_ONE },   /* G_RM_TEX_EDGE: CVG_X_ALPHA, FORCE_BL, sin IM_RD */
		{ 0x0F0A7048, GFX_CVG_MAX },   /* el mismo con IM_RD: cvg·α + la de la memoria */
		{ 0x0F0A7248, GFX_CVG_ONE },   /* CVG_X_ALPHA con ZAP */
	};
	const struct vtx v[3] = { { 0, 0, 0, 0, 0, { 0 } }, { 1, 0, 0, 0, 0, { 0 } }, { 0, 1, 0, 0, 0, { 0 } } };
	for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
		start();
		viewport_320x240();
		load_mtx(ident, true);
		load_mtx(ident, false);
		set_combine(CC_A_ZERO, CC_ZERO, CC_C_ZERO, CC_SHADE, CC_ZERO, CC_ZERO, CC_ZERO, CC_SHADE);
		dl(0xEF000000 | G_CYC_1CYCLE << 20, c[i].ol);  /* G_RDPSETOTHERMODE */
		dl(0x01000000 | 3 << 12 | 3 << 1, put_vertices(v, 3));
		dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);
		run();
		const struct gfx_draw *d = draw_n(0);
		CHECK(d != NULL);
		if (d)
			CHECK_EQ(d->cvg_bit, c[i].cvg);
	}
	/* Un relleno deja el bit de su color, sea cual sea CVG_DST. */
	start();
	dl(0xFF100000 | 319, 0x00100000);
	dl(0xEF000000 | G_CYC_FILL << 20, 0x005049D8);
	dl(0xF7000000, 0xF800F800);  /* rojo con alfa 0 */
	dl(0xF6000000 | (319 << 2) << 12 | (239 << 2), 0);
	run();
	const struct gfx_draw *f = draw_n(0);
	CHECK(f != NULL);
	if (f) {
		CHECK_EQ(f->cycle, G_CYC_FILL);
		CHECK_EQ(f->cvg_bit, GFX_CVG_ALPHA);
		CHECK_EQ(f->aref, 0);
	}
}

/* Cobertura con AA_EN (gfx_draw.aa): solo triángulos opacos sin prueba de
 * alfa; ni los translúcidos, ni los recortados por el alfa, ni sin AA_EN,
 * ni los rectángulos. */
static void gfx_aa_flag(void)
{
	static const struct { uint32_t ol; uint8_t aa; } c[] = {
		{ 0x00552078, 1 },  /* G_RM_AA_ZB_OPA_SURF */
		{ 0x00552048, 1 },  /* G_RM_AA_OPA_SURF */
		{ 0x005049D8, 0 },  /* G_RM_AA_ZB_XLU_SURF */
		{ 0x00553078, 0 },  /* G_RM_AA_ZB_TEX_EDGE */
		{ 0x00552230, 0 },  /* G_RM_ZB_OPA_SURF */
	};
	const struct vtx v[3] = { { 0, 0, 0, 0, 0, { 0 } }, { 1, 0, 0, 0, 0, { 0 } }, { 0, 1, 0, 0, 0, { 0 } } };
	for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
		start();
		viewport_320x240();
		load_mtx(ident, true);
		load_mtx(ident, false);
		set_combine(CC_A_ZERO, CC_ZERO, CC_C_ZERO, CC_SHADE, CC_ZERO, CC_ZERO, CC_ZERO, CC_SHADE);
		dl(0xEF000000 | G_CYC_1CYCLE << 20, c[i].ol);  /* G_RDPSETOTHERMODE */
		dl(0x01000000 | 3 << 12 | 3 << 1, put_vertices(v, 3));
		dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);
		dl(0xE4000000 | (40 << 2) << 12 | (40 << 2), 0);  /* G_TEXRECT */
		dl(0xE1000000, 0);
		dl(0xF1000000, 0x04000400);
		run();
		const struct gfx_draw *d = draw_n(0), *r = draw_n(1);
		CHECK(d && r);
		if (!d || !r)
			continue;
		CHECK_EQ(d->aa, c[i].aa);
		CHECK_EQ(r->aa, 0);
	}
}

/* Display lists anidadas, segmentos, CULLDL y BRANCH_Z. */
static void gfx_dl_flow(void)
{
	start();
	viewport_320x240();
	load_mtx(ident, true);
	load_mtx(ident, false);
	dl(0xDB060000 | 3 * 4, 0x00010000);  /* segmento 3 → 0x10000 */
	const struct vtx v[3] = { { 0, 0, 0, 0, 0, { 0 } }, { 1, 0, 0, 0, 0, { 0 } }, { 0, 1, 0, 0, 0, { 0 } } };
	uint32_t va = put_vertices(v, 3);
	/* Subrutina en 0x18000 (segmento 3 + 0x8000): un triángulo. */
	uint32_t main_pc = dl_pc;
	dl_pc = DL_PA + 0x8000;
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);
	dl(0xDF000000, 0);
	dl_pc = main_pc;
	dl(0x01000000 | 3 << 12 | 3 << 1, va);
	dl(0xDE000000, 0x03008000);          /* G_DL (llamada) por segmento */
	dl(0xDE000000, 0x03008000);          /* otra vez */
	/* Vértices fuera por la derecha: CULLDL termina la lista. */
	const struct vtx far[3] = { { 5, 0, 0, 0, 0, { 0 } }, { 6, 0, 0, 0, 0, { 0 } }, { 7, 1, 0, 0, 0, { 0 } } };
	dl(0x01000000 | 3 << 12 | 3 << 1, put_vertices(far, 3));
	dl(0x03000000 | 0, 2 * 2);
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);  /* no se ejecuta */
	run();
	CHECK_EQ(res.triangles, 2);
	CHECK_EQ(res.dl_errors, 0);
}

/* G_LOAD_UCODE a otro microcódigo (S2DEX en Zelda OoT): sus comandos se
 * saltan, siguiendo G_DL y G_ENDDL, hasta que la lista vuelve a cargar el
 * microcódigo de la tarea; lo que sigue se ejecuta y el G_RDPFULLSYNC final
 * cuenta. Cargar el mismo microcódigo no cambia nada. */
static void gfx_load_ucode_skips_foreign(void)
{
	start();
	viewport_320x240();
	load_mtx(ident, true);
	load_mtx(ident, false);
	const struct vtx v[3] = { { 0, 0, 0, 0, 0, { 0 } }, { 1, 0, 0, 0, 0, { 0 } }, { 0, 1, 0, 0, 0, { 0 } } };
	uint32_t va = put_vertices(v, 3);
	dl(0x01000000 | 3 << 12 | 3 << 1, va);
	/* Mismo microcódigo: sigue. */
	dl(0xE1000000, 0x80040000);
	dl(0xDD000FFF, 0x80030000);
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);  /* triángulo 1 */
	/* Otro microcódigo: un tramo con una subrutina, comandos que en F3DEX2
	 * serían triángulos (no deben dibujarse) y la vuelta al nuestro. */
	dl(0xE1000000, 0x80050000);
	dl(0xDD000FFF, 0x80031000);
	uint32_t main_pc = dl_pc;
	dl_pc = DL_PA + 0x8000;
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);  /* no se ejecuta */
	dl(0x0A000000, 0x80060000);                       /* G_BG_COPY de S2DEX */
	dl(0xDF000000, 0);
	dl_pc = main_pc;
	dl(0xDE000000, 0x80000000 | (DL_PA + 0x8000));   /* llamada */
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);  /* no se ejecuta */
	dl(0xE1000000, 0x80040000);
	dl(0xDD000FFF, 0x80030000);                       /* de vuelta */
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);  /* triángulo 2 */
	dl(0xE9000000, 0);                                /* G_RDPFULLSYNC */
	job_ucode = 0x30000;
	run();
	job_ucode = 0;
	CHECK_EQ(res.triangles, 2);
	CHECK_EQ(res.skipped, 7);
	CHECK_EQ(res.dl_errors, 0);
	CHECK_EQ(res.flags & (GFX_RES_FULLSYNC | GFX_RES_LOAD_UCODE | GFX_RES_LOOP),
		 GFX_RES_FULLSYNC | GFX_RES_LOAD_UCODE);
	CHECK_EQ(salsa64_gfx_task_flags(&res), SALSA64_GFX_FULLSYNC);

	/* Un tramo ajeno que nunca vuelve: la lista termina en su G_ENDDL y
	 * cuenta como cortada (no se sabe qué hizo; el juego espera al RDP). */
	next_task();
	dl(0xE1000000, 0x80050000);
	dl(0xDD000FFF, 0x80031000);
	dl(0x0A000000, 0x80060000);
	job_ucode = 0x30000;
	run();
	job_ucode = 0;
	CHECK_EQ(res.triangles, 0);
	CHECK_EQ(res.skipped, 2);
	CHECK_EQ(res.flags & (GFX_RES_FULLSYNC | GFX_RES_FOREIGN_END), GFX_RES_FOREIGN_END);
	CHECK_EQ(salsa64_gfx_task_flags(&res), SALSA64_GFX_CUT);

	/* El G_RDPFULLSYNC dentro del tramo ajeno (comando del RDP, común a la
	 * familia) sí cuenta: DP sin cortar. */
	next_task();
	dl(0xE1000000, 0x80050000);
	dl(0xDD000FFF, 0x80031000);
	dl(0x0A000000, 0x80060000);
	dl(0xE9000000, 0);
	job_ucode = 0x30000;
	run();
	job_ucode = 0;
	CHECK_EQ(res.skipped, 3);
	CHECK_EQ(salsa64_gfx_task_flags(&res), SALSA64_GFX_FULLSYNC | SALSA64_GFX_CUT);

	/* Sin tramo ajeno y sin FULLSYNC (G_ENDDL): ni DP ni corte. */
	next_task();
	run();
	CHECK_EQ(salsa64_gfx_task_flags(&res), 0);
	res.flags |= GFX_RES_LOOP;
	CHECK_EQ(salsa64_gfx_task_flags(&res), SALSA64_GFX_CUT);
}

static void ucode_load(const char *text, uint32_t text_pa)
{
	uint32_t data = data_alloc(128);
	memset(t_n64.bus.rdram + data, 0, 128);
	memcpy(t_n64.bus.rdram + data + 8, text, strlen(text));
	dl(0xE1000000, 0x80000000 | data);
	dl(0xDD0007FF, 0x80000000 | text_pa);
}

/* G_LOAD_UCODE a otro microcódigo de la misma generación (F-Zero X pasa a
 * mitad de cuadro de F3DEX 2.03 a F3DFLX.Rej 2.03F, con el auto y el HUD): se
 * sigue interpretando, con el RSP arrancado de nuevo (segmentos, modo
 * geométrico, vértices y matrices vuelven a cero); uno de la primera
 * generación o de otra familia (L3DEX) se salta. */
static void gfx_load_ucode_same_generation(void)
{
	const struct vtx v[3] = { { 0, 0, 0, 0, 0, { 0 } }, { 1, 0, 0, 0, 0, { 0 } }, { 0, 1, 0, 0, 0, { 0 } } };
	/* Dos microcódigos 2.x: el segundo sigue, sus triángulos se dibujan. */
	start();
	viewport_320x240();
	load_mtx(ident, true);
	load_mtx(ident, false);
	uint32_t va = put_vertices(v, 3);
	dl(0x01000000 | 3 << 12 | 3 << 1, va);
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);  /* triángulo 1 */
	dl(0xDB060004, 0x00123456);                       /* segmento 1 */
	ucode_load("RSP Gfx ucode F3DFLX.Rej  fifo 2.03F Yoshitaka Yasumoto 1998 Nintendo.", 0x31000);
	load_mtx(ident, true);
	load_mtx(ident, false);
	dl(0x01000000 | 3 << 12 | 3 << 1, va);
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);  /* triángulo 2 */
	dl(0xE9000000, 0);                                /* G_RDPFULLSYNC */
	job_ucode = 0x30000;
	run();
	job_ucode = 0;
	CHECK_EQ(res.triangles, 2);
	CHECK_EQ(res.skipped, 0);
	CHECK_EQ(res.dl_errors, 0);
	CHECK_EQ(res.flags & (GFX_RES_FULLSYNC | GFX_RES_LOAD_UCODE | GFX_RES_FOREIGN_END), GFX_RES_FULLSYNC);
	CHECK_EQ(salsa64_gfx_task_flags(&res), SALSA64_GFX_FULLSYNC);

	/* El modo geométrico vuelve a cero con el microcódigo: el dibujo de antes
	 * lleva G_CULL_BACK y el de después ya no. */
	next_task();
	viewport_320x240();
	load_mtx(ident, true);
	load_mtx(ident, false);
	dl(0xD9000000, G_CULL_BACK);
	dl(0x01000000 | 3 << 12 | 3 << 1, va);
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);
	ucode_load("RSP Gfx ucode F3DEX.NoN   fifo 2.04H Yoshitaka Yasumoto 1998 Nintendo.", 0x31000);
	load_mtx(ident, true);
	load_mtx(ident, false);
	dl(0x01000000 | 3 << 12 | 3 << 1, va);
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);
	job_ucode = 0x30000;
	run();
	job_ucode = 0;
	CHECK_EQ(res.triangles, 2);
	CHECK_EQ(res.draws, 2);
	CHECK_EQ(res.skipped, 0);
	CHECK(draw_n(0) && draw_n(0)->cull == GFX_CULL_BACK);
	CHECK(draw_n(1) && draw_n(1)->cull == GFX_CULL_NONE);

	/* Primera generación (1.x) o L3DEX (líneas): se saltan. */
	next_task();
	ucode_load("RSP Gfx ucode F3DEX       fifo 1.23  Yoshitaka Yasumoto 1998 Nintendo.", 0x31000);
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);  /* no se ejecuta */
	job_ucode = 0x30000;
	run();
	job_ucode = 0;
	CHECK_EQ(res.triangles, 0);
	CHECK_EQ(res.skipped, 2);  /* el triángulo y el G_ENDDL del final */
	next_task();
	ucode_load("RSP Gfx ucode L3DEX       fifo 2.03  Yoshitaka Yasumoto 1998 Nintendo.", 0x31000);
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);  /* no se ejecuta */
	job_ucode = 0x30000;
	run();
	job_ucode = 0;
	CHECK_EQ(res.triangles, 0);
	CHECK_EQ(res.skipped, 2);
}

/* Clase de los datos de microcódigo con ese texto (gfx_ucode_kind): los de
 * Nintendo traen "RSP Gfx ucode <nombre> fifo N.xx", y el S2DEX de Zelda OoT
 * "RSP Gfx[Safe] S2DEX fifo 2.05" (el prefijo entre "RSP Gfx" y el nombre
 * cambia). Textos inventados con las mismas formas. */
static int ucode_kind_of(const char *text, bool *nonear)
{
	memset(&job, 0, sizeof job);
	job.rdram_ea = (uint64_t)(uintptr_t)t_n64.bus.rdram;
	job.rdram_size = t_n64.bus.rdram_size;
	gs.job = &job;
	uint32_t at = 0x31000;
	memset(t_n64.bus.rdram + at, 0, 128);
	memcpy(t_n64.bus.rdram + at + 8, text, strlen(text));
	return gfx_ucode_kind(&gs, at, 64, nonear);
}

static void gfx_ucode_kind_texts(void)
{
	static const struct {
		const char *text;
		int kind;
		bool nonear;
	} c[] = {
		/* Segunda generación (se interpreta con el RSP arrancado de nuevo). */
		{ "RSP Gfx ucode F3DFLX.Rej  fifo 2.03F Yoshitaka Yasumoto 1998 Nintendo.", 2, false },
		{ "RSP Gfx ucode F3DEX       fifo 2.08  Yoshitaka Yasumoto 1998 Nintendo.", 2, false },
		{ "RSP Gfx ucode F3DEX.NoN   fifo 2.04H Yoshitaka Yasumoto 1998 Nintendo.", 2, true },
		{ "RSP Gfx ucode F3DZEX.NoN  fifo 2.06H Yoshitaka Yasumoto 1998 Nintendo.", 2, true },
		{ "RSP Gfx ucode F3DZEX      xbus 2.06H Yoshitaka Yasumoto 1998 Nintendo.", 2, false },
		/* Primera generación. */
		{ "RSP Gfx ucode F3DEX       fifo 1.23  Yoshitaka Yasumoto 1998 Nintendo.", 1, false },
		{ "RSP Gfx ucode F3DEX.NoN   fifo 1.23  Yoshitaka Yasumoto 1998 Nintendo.", 1, true },
		{ "RSP Gfx ucode F3DEX         0.95     Yoshitaka Yasumoto 1998 Nintendo.", 1, false },
		/* Ajenos: Conker, L3DEX; S2DEX 1.x, aparte. */
		{ "RSP Gfx ucode F3DEXBG.NoN fifo 2.08  Yoshitaka Yasumoto 1998 Nintendo.", 0, false },
		{ "RSP Gfx ucode L3DEX       fifo 2.03  Yoshitaka Yasumoto 1998 Nintendo.", 0, false },
		{ "RSP Gfx ucode S2DEX       fifo 1.06  Yoshitaka Yasumoto 1998 Nintendo.", GFX_UCODE_S2DEX1, false },
		{ "RSP Gfx ucode S2DEX  1.06 Yoshitaka Yasumoto Nintendo.", GFX_UCODE_S2DEX1, false },
		{ "RSP Gfx[Safe] S2DEX       fifo 1.06  Yoshitaka Yasumoto 1998 Nintendo.", GFX_UCODE_S2DEX1, false },
		{ "RSP Gfx[Safe] L3DEX       fifo 2.03  Yoshitaka Yasumoto 1998 Nintendo.", 0, false },
		/* S2DEX 2.x, con el prefijo de siempre y con el de Zelda OoT. */
		{ "RSP Gfx ucode S2DEX       fifo 2.05  Yoshitaka Yasumoto 1998 Nintendo.", GFX_UCODE_S2DEX2, false },
		{ "RSP Gfx[Safe] S2DEX       fifo 2.05  Yoshitaka Yasumoto 1998 Nintendo.", GFX_UCODE_S2DEX2, false },
		{ "RSP Gfx ucode [Safe] S2DEX fifo 2.05 Yoshitaka Yasumoto 1998 Nintendo.", GFX_UCODE_S2DEX2, false },
		{ "RSP Gfx[Safe]S2DEX        fifo 2.05  Yoshitaka Yasumoto 1998 Nintendo.", GFX_UCODE_S2DEX2, false },
		/* Con el mismo prefijo, el resto sigue igual. */
		{ "RSP Gfx[Safe] F3DZEX.NoN  fifo 2.06H Yoshitaka Yasumoto 1998 Nintendo.", 2, true },
		{ "RSP Gfx[Safe] F3DEXBG.NoN fifo 2.08  Yoshitaka Yasumoto 1998 Nintendo.", 0, false },
		/* Sin texto de Nintendo, o cortado. */
		{ "Salsa64 GBI F3DEX2 sin el texto de Nintendo para nada de nada de nada", 0, false },
		{ "RSP Gfx ucode", 0, false },
		{ "RSP Gfx[Safe] S2DEX", 0, false },
	};
	start();
	for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
		bool nonear = true;
		int kind = ucode_kind_of(c[i].text, &nonear);
		if (kind != c[i].kind || nonear != c[i].nonear)
			fprintf(stderr, "gfx_ucode_kind(\"%s\") = %d/%d, esperado %d/%d\n", c[i].text, kind, nonear, c[i].kind,
				c[i].nonear);
		CHECK_EQ(kind, c[i].kind);
		CHECK_EQ(nonear, c[i].nonear);
	}
}

/* F3DFLX (F-Zero X): con G_TEXTURE_GEN no genera s y t (el auto lleva la
 * escala 0xFFFF y con la texgen de F3DEX2 sus texturas quedaban fuera, en la
 * orilla: lisas) sino el alfa, de la tabla que la lista sube con G_DMA_IO,
 * por el coseno entre la normal y la luz del alfa (G_MV_LIGHT en 24, tres
 * s16). Medido con el microcódigo real (salsa64-run --lle=all). */
static void flx_scene(const char *ucode)
{
	uint8_t *p = t_n64.bus.rdram;
	start();
	if (ucode)
		ucode_load(ucode, 0x31000);
	viewport_320x240();
	load_mtx(ident, true);
	load_mtx(ident, false);
	dl(0xD9000000, G_LIGHTING | G_TEXTURE_GEN);
	dl(0xD7000002, 0xFFFFFFFF);  /* G_TEXTURE on, escala 0xFFFF */
	dl(0xDB020000, 1 * 24);      /* G_MW_NUMLIGHT: una luz */
	uint32_t l0 = data_alloc(16), l1 = data_alloc(16), la = data_alloc(16), tab = data_alloc(256);
	memset(p + l0, 0, 16);
	memset(p + l1, 0, 16);
	memset(p + la, 0, 16);
	p[l0 + 0] = p[l0 + 1] = p[l0 + 2] = 100;
	p[l0 + 10] = 127;
	p[l1 + 0] = p[l1 + 1] = p[l1 + 2] = 20;
	salsa64_wr16(p + la + 12, 0x7F00);  /* luz del alfa: +z */
	for (int i = 0; i < 256; i++)
		p[tab + i] = (uint8_t)i;
	dl(0xDC08000A | (2 * 24 / 8) << 8, l0);
	dl(0xDC08000A | (3 * 24 / 8) << 8, l1);
	dl(0xD6000000 | (0x8B0 / 8) << 13 | (256 - 1), 0x80000000 | tab);  /* G_DMA_IO a la DMEM */
	dl(0xDC08000A | (24 / 8) << 8, la);
	const struct vtx v[4] = {
		{ 0, 0, 0, 64, 96, { 0, 0, 127, 0x40 } },            /* normal +z: al final de la tabla */
		{ 0, 0, 0, 64, 96, { 0, 0, (uint8_t)-127, 0x40 } },  /* −z: al principio */
		{ 0, 0, 0, 64, 96, { 127, 0, 0, 0x40 } },            /* +x: al centro */
		{ 0, 0, 0, 64, 96, { 0, 0, 60, 0x40 } },             /* +z corta: 60 · 1,0055, sin normalizar */
	};
	dl(0x01000000 | 4 << 12 | 4 << 1, put_vertices(v, 4));
	/* Niebla con el alfa del sombreado en el primer ciclo (G_RM_FOG_SHADE_A)
	 * y el color de niebla casi blanco, como el auto de F-Zero X. */
	dl(0xEF000000 | G_CYC_2CYCLE << 20, 0xC8000230);
	dl(0xF8000000, 0xFAFFFFFF);
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);
	job_ucode = 0x30000;
	run();
	job_ucode = 0;
}

static void gfx_f3dflx_alpha(void)
{
	flx_scene("RSP Gfx ucode F3DFLX.Rej  fifo 2.03F Yoshitaka Yasumoto 1998 Nintendo.");
	CHECK_EQ(gs.ucode_var, GFX_UCODE_FLX);
	CHECK_EQ(gs.vtx[0].c[3], 255);
	CHECK_EQ(gs.vtx[1].c[3], 0);  /* −127 · 1,0055 hacia abajo: −128 */
	CHECK_EQ(gs.vtx[2].c[3], 128);
	CHECK_EQ(gs.vtx[3].c[3], 128 + 60);
	/* s y t del vértice (2 y 3 texels) por la escala de G_TEXTURE. */
	CHECK(fabsf(gs.vtx[0].s - 2.0f) < 0.01f && fabsf(gs.vtx[0].t - 3.0f) < 0.01f);
	CHECK(gs.vtx[0].c[0] == 120 && gs.vtx[1].c[0] == 20);  /* la luz, como siempre */
	CHECK_EQ(res.unknown_cmds, 0);
	/* Ese alfa mezcla con el color de niebla aunque no haya G_FOG. */
	CHECK(draw_n(0) && draw_n(0)->fog == 1 && draw_n(0)->fog_color == 0xFAFFFFFF);

	/* Con F3DEX2 (y con el de la tarea): la texgen de siempre y el alfa del
	 * vértice; G_DMA_IO no cambia nada. */
	flx_scene("RSP Gfx ucode F3DEX       fifo 2.04H Yoshitaka Yasumoto 1998 Nintendo.");
	CHECK_EQ(gs.ucode_var, 0);
	CHECK_EQ(gs.vtx[0].c[3], 0x40);
	CHECK(fabsf(gs.vtx[0].s - 512.0f) < 1.0f);  /* (0 + 1)·512 */
	CHECK(draw_n(0) && draw_n(0)->fog == 0);    /* sin G_FOG, sin niebla */
	flx_scene(NULL);
	CHECK_EQ(gs.vtx[2].c[3], 0x40);
	CHECK(fabsf(gs.vtx[2].s - 1023.0f) < 1.0f);  /* (1 + 1)·512 */
}

/* Niebla del blender en un solo ciclo: la pista de F-Zero X va con G_FOG,
 * othermode L 0xFA006220 (FOG·A_SHADE + IN·(1 − A_SHADE) en los dos ciclos,
 * FORCE_BL, ALPHA_CVG_SEL, Z_UPD) y el ciclo en 1CYCLE. El RDP mezcla en
 * todos los píxeles (FORCE_BL) sin leer la memoria: es la misma niebla que
 * G_RM_FOG_SHADE_A en dos ciclos. Un triángulo con el mismo blender sin
 * FORCE_BL no es niebla (el interior escribe P), y la bandera
 * GFX_JOB_NO_FOG_1CYCLE la apaga. */
static void fog_1cycle_scene(uint32_t othermode_l, unsigned cycle, uint32_t flags)
{
	start();
	job_flags = flags;
	viewport_320x240();
	load_mtx(ident, true);
	load_mtx(ident, false);
	/* (SHADE − T0)·T0a + T0, como la pista. */
	set_combine(CC_SHADE, CC_T0, CC_C_T0A, CC_T0, CC_ZERO, CC_ZERO, CC_ZERO, CC_SHADE);
	dl(0xD9000000, G_ZBUFFER | G_SHADE | G_CULL_BACK | G_FOG | G_SHADING_SMOOTH);
	dl(0xDB080000, (uint32_t)0 << 16 | 100);  /* G_MW_FOG: factor 100 a cualquier z */
	dl(0xEF000000 | cycle << 20, othermode_l);
	dl(0xF8000000, 0xFAFFFFFF);              /* color de niebla */
	dl(0xFF100000 | 319, 0x00100000);
	const struct vtx v[3] = {
		{ 0, 0, 0, 0, 0, { 0, 0, 0, 255 } },
		{ 0, 1, 0, 0, 0, { 0, 0, 0, 255 } },
		{ 1, 0, 0, 0, 0, { 0, 0, 0, 255 } },
	};
	dl(0x01000000 | 3 << 12 | 3 << 1, put_vertices(v, 3));
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);
	run();
}

static void gfx_fog_1cycle_force_bl(void)
{
	fog_1cycle_scene(0xFA006220, G_CYC_1CYCLE, 0);
	const struct gfx_draw *d = draw_n(0);
	CHECK(d != NULL);
	if (d) {
		CHECK_EQ(d->cycle, G_CYC_1CYCLE);
		CHECK_EQ(d->fog, 1);
		CHECK_EQ(d->fog_color, 0xFAFFFFFF);
		CHECK_EQ(d->blend, GFX_BLEND_OFF);
		CHECK_EQ(d->depth, GFX_DEPTH_WRITE);
		CHECK(d->nterms > 0 && d->factors[0][0] == GFX_F_NONE);  /* el término al que va fog·a */
	}
	CHECK_EQ(gs.vtx[0].c[3], 100);  /* el alfa del vértice es el factor de niebla */

	/* Lo mismo en dos ciclos (G_RM_FOG_SHADE_A + un segundo ciclo opaco). */
	fog_1cycle_scene(0xC8112078, G_CYC_2CYCLE, 0);
	d = draw_n(0);
	CHECK(d && d->fog == 1 && d->fog_color == 0xFAFFFFFF);

	/* Sin FORCE_BL: el interior escribe P, no es niebla. */
	fog_1cycle_scene(0xFA002220, G_CYC_1CYCLE, 0);
	d = draw_n(0);
	CHECK(d && d->fog == 0 && d->blend == GFX_BLEND_OFF);

	/* El interruptor. */
	fog_1cycle_scene(0xFA006220, G_CYC_1CYCLE, GFX_JOB_NO_FOG_1CYCLE);
	d = draw_n(0);
	CHECK(d && d->fog == 0 && d->blend == GFX_BLEND_OFF);

	/* El blender opaco de siempre en un ciclo no cambia. */
	fog_1cycle_scene(0x00552078, G_CYC_1CYCLE, 0);
	d = draw_n(0);
	CHECK(d && d->fog == 0);
}

/* Recorte contra la caja del clip ratio, como el RSP (rdp.c gfx_triangle).
 * Proyección diagonal 1/8 (x e y de los vértices en octavos de NDC) con el
 * viewport de 320×240: la pantalla va de 0 a 320 y la caja (ratio 2) de
 * −160 a 480 en x, de −120 a 360 en y. */
static void box_scene(const struct vtx *v, uint32_t flags)
{
	static const float eighth[16] = { 0.125f, 0, 0, 0, 0, 0.125f, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	start();
	job_flags = flags;
	viewport_320x240();
	load_mtx(eighth, true);
	load_mtx(ident, false);
	dl(0xD9000000, G_SHADING_SMOOTH);
	set_combine(CC_ZERO, CC_ZERO, CC_ZERO, CC_SHADE, CC_ZERO, CC_ZERO, CC_ZERO, CC_SHADE);  /* color = sombra */
	dl(0xFF100000 | 319, 0x00100000);
	dl(0x01000000 | 3 << 12 | 3 << 1, put_vertices(v, 3));
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);
	run();
}

/* Píxel (x/w, y/w) del vértice n de la salida. */
static void out_px(int n, float *x, float *y)
{
	*x = out_vtx[n].x / out_vtx[n].w;
	*y = out_vtx[n].y / out_vtx[n].w;
}

static bool near_px(int n, float x, float y)
{
	float px, py;
	out_px(n, &px, &py);
	return fabsf(px - x) < 0.6f && fabsf(py - y) < 0.6f;
}

static void gfx_rsp_clip_box(void)
{
	/* Un vértice fuera de la caja por la derecha: el RSP recorre v0, v1, v2
	 * con el anterior y deja [v0, I01, I12, v2]; abanico desde v0: dos
	 * triángulos. La sombra se interpola a lo largo de la arista. */
	const struct vtx tri[3] = {
		{ 0, 0, 0, 0, 0, { 255, 0, 0, 255 } },     /* (160, 120) */
		{ 24, 0, 0, 0, 0, { 0, 0, 255, 255 } },    /* (640, 120): fuera de 480 */
		{ 0, 8, 0, 0, 0, { 0, 255, 0, 255 } },     /* (160, 0) */
	};
	box_scene(tri, 0);
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.triangles, 1);
	CHECK_EQ(res.vtx_bytes / sizeof(struct gfx_vertex), 6u);
	CHECK(near_px(0, 160, 120) && near_px(1, 480, 120) && near_px(2, 480, 80));
	CHECK(near_px(3, 160, 120) && near_px(4, 480, 80) && near_px(5, 160, 0));
	/* I01 a dos tercios de v0 a v1: (85, 0, 170). */
	CHECK(out_vtx[1].r >= 84 && out_vtx[1].r <= 86 && out_vtx[1].g == 0 && out_vtx[1].b >= 169 && out_vtx[1].b <= 171);
	/* En lo visible la sombra es la del polígono: en (320, 120), medio
	 * camino de v0 a I01, 170 de rojo; el triángulo entero sin recortar
	 * daría 170 también aquí (w constante): lo que cambia es el abanico. */

	/* Con el interruptor queda el triángulo entero. */
	box_scene(tri, GFX_JOB_NO_RSP_CLIP);
	CHECK_EQ(res.triangles, 1);
	CHECK_EQ(res.vtx_bytes / sizeof(struct gfx_vertex), 3u);
	CHECK(near_px(1, 640, 120));

	/* El fondo de F-Zero X: un vértice lejos y dos muy fuera de la
	 * pantalla por abajo (w constante aquí: solo cuenta la geometría). El
	 * polígono es [LB, RB, R, v0, L] y el abanico sale de la esquina LB;
	 * (LB, v0, L) queda entero a la izquierda de la pantalla y no se
	 * dibuja: dos triángulos, sin el vértice L (−160, 169). */
	const struct vtx fondo[3] = {
		{ -11, -1, 0, 0, 0, { 255, 255, 255, 255 } },   /* (−60, 135) */
		{ -105, -46, 0, 0, 0, { 0, 0, 0, 255 } },        /* (−1940, 810) */
		{ 109, 3, 0, 0, 0, { 0, 0, 0, 255 } },           /* (2340, 75) */
	};
	box_scene(fondo, 0);
	CHECK_EQ(res.triangles, 1);
	CHECK_EQ(res.vtx_bytes / sizeof(struct gfx_vertex), 6u);
	CHECK(near_px(0, -160, 360) && near_px(1, 480, 360));
	CHECK(near_px(3, -160, 360) && near_px(5, -60, 135));
	float px, py;
	out_px(2, &px, &py);
	CHECK(fabsf(px - 480.0f) < 0.6f && py > 100.0f && py < 140.0f);   /* R, en la arista v0→v2 */
	for (int n = 0; n < 6; n++) {
		out_px(n, &px, &py);
		CHECK(!(fabsf(px + 160.0f) < 0.6f && py < 300.0f));             /* L no aparece */
	}
	box_scene(fondo, GFX_JOB_NO_RSP_CLIP);
	CHECK_EQ(res.vtx_bytes / sizeof(struct gfx_vertex), 3u);

	/* Los tres dentro de la caja (aunque fuera de la pantalla): no se toca. */
	const struct vtx dentro[3] = {
		{ 0, 0, 0, 0, 0, { 255, 0, 0, 255 } },
		{ 14, 0, 0, 0, 0, { 0, 0, 255, 255 } },    /* (440, 120): fuera de la pantalla, dentro de la caja */
		{ 0, 8, 0, 0, 0, { 0, 255, 0, 255 } },
	};
	box_scene(dentro, 0);
	CHECK_EQ(res.vtx_bytes / sizeof(struct gfx_vertex), 3u);
	/* Los tres fuera del mismo plano de la pantalla: nada. */
	const struct vtx fuera[3] = {
		{ 20, 0, 0, 0, 0, { 255, 0, 0, 255 } },
		{ 24, 0, 0, 0, 0, { 0, 0, 255, 255 } },
		{ 20, 8, 0, 0, 0, { 0, 255, 0, 255 } },
	};
	box_scene(fuera, 0);
	CHECK_EQ(res.triangles, 0);
}

/* ---- S2DEX2: fondos prerrenderizados (G_BG_COPY, G_BG_1CYC) ---- */

/* G_LOAD_UCODE a un S2DEX2: los datos del microcódigo llevan el texto que
 * lo identifica (inventado aquí). La vuelta es al de la tarea (0x30000). */
static void s2dex_enter(const char *text)
{
	uint32_t data = data_alloc(64);
	memset(t_n64.bus.rdram + data, 0, 64);
	memcpy(t_n64.bus.rdram + data + 8, text, strlen(text));
	dl(0xE1000000, 0x80000000 | data);
	dl(0xDD00003F, 0x80031000);
}

static void s2dex_leave(void)
{
	dl(0xE1000000, 0x80040000);
	dl(0xDD000FFF, 0x80030000);
}

/* uObjBg (u10.5 para imageX/Y, u10.2 para tamaños, s10.2 para el marco) y
 * la escala de uObjScaleBg (u5.10). */
static uint32_t put_bg(uint32_t image_x, uint32_t image_w, int32_t frame_x, uint32_t frame_w, uint32_t image_y,
		       uint32_t image_h, int32_t frame_y, uint32_t frame_h, uint32_t ptr, uint16_t scale_w,
		       uint16_t scale_h)
{
	uint32_t a = data_alloc(40);
	uint8_t *p = t_n64.bus.rdram + a;
	memset(p, 0, 40);
	salsa64_wr16(p + 0, (uint16_t)(image_x << 5));
	salsa64_wr16(p + 2, (uint16_t)(image_w << 2));
	salsa64_wr16(p + 4, (uint16_t)(frame_x * 4));
	salsa64_wr16(p + 6, (uint16_t)(frame_w << 2));
	salsa64_wr16(p + 8, (uint16_t)(image_y << 5));
	salsa64_wr16(p + 10, (uint16_t)(image_h << 2));
	salsa64_wr16(p + 12, (uint16_t)(frame_y * 4));
	salsa64_wr16(p + 14, (uint16_t)(frame_h << 2));
	salsa64_wr32(p + 16, ptr);
	salsa64_wr16(p + 20, 0xFFF4);          /* G_BGLT_LOADTILE */
	p[22] = G_IM_FMT_RGBA;
	p[23] = G_IM_SIZ_16b;
	salsa64_wr16(p + 28, scale_w);
	salsa64_wr16(p + 30, scale_h);
	return a;
}

/* Imagen RGBA16 en TEX_PA: rojo = x, verde = y (módulo 32), opaca. */
static uint16_t bg_texel(uint32_t x, uint32_t y)
{
	return (uint16_t)((x & 31) << 11 | (y & 31) << 6 | ((x >> 5) & 31) << 1 | 1);
}

static void put_bg_image(uint32_t w, uint32_t h)
{
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++)
			salsa64_wr16(t_n64.bus.rdram + TEX_PA + 2 * (y * w + x), bg_texel(x, y));
}

/* Esquina de arriba a la izquierda (corner 0) o de abajo a la derecha (1)
 * del rectángulo r del dibujo n (los rectángulos seguidos con el mismo
 * estado van en un dibujo, 6 vértices cada uno). */
static const struct gfx_vertex *rect_v(int n, int r, int corner)
{
	const struct gfx_draw *d = draw_n(n);
	return d && (uint32_t)(6 * r + 6) <= d->h.b ? &out_vtx[d->h.a + 6 * r + (corner ? 5 : 0)] : NULL;
}

/* La textura del dibujo n tiene en (x, y) el texel (ix, iy) de la imagen. */
static bool bg_tex_is(int n, uint32_t x, uint32_t y, uint32_t ix, uint32_t iy)
{
	const struct gfx_draw *d = draw_n(n);
	if (!d || !d->ntex)
		return false;
	const uint8_t *t = (d->tex[0].where == GFX_TEX_CACHE ? out_cache : out_tex) + d->tex[0].offset +
			   y * d->tex[0].pitch + 4 * x;
	uint16_t p = bg_texel(ix, iy);
	uint32_t r = (p >> 11) & 31, g = (p >> 6) & 31, b = (p >> 1) & 31;
	return t[0] == (r << 3 | r >> 2) && t[1] == (g << 3 | g >> 2) && t[2] == (b << 3 | b >> 2) && t[3] == 0xFF;
}

/* La copia del othermode L del microcódigo de la segunda generación arranca en 0
 * con cada tarea (sus datos): el RDP trae de la tarea anterior un Z_SOURCE_SEL en
 * 1 y el primer G_SETOTHERMODE_L (render mode, bits 3 a 31) no lo hereda
 * (Castlevania: todo el escenario salía con una sola z y lo lejano tapaba lo
 * cercano). Un G_RDPSETOTHERMODE dentro de la tarea sí la actualiza. */
static void gfx_f3dex2_othermode_copy(void)
{
	start();
	gs.ol = 0x0c184a54u;  /* lo que dejó la tarea anterior: Z_SOURCE_SEL */
	dl(0xE2000000 | 0u << 8 | 28, 0x0c192048u);  /* G_SETOTHERMODE_L, 3, 29 */
	run();
	CHECK_EQ(gs.ol, 0x0c192048u);
	start();
	gs.ol = 0x0c184a54u;
	dl(0xEF000000, 0x0f0a4004u);                  /* G_RDPSETOTHERMODE crudo: Z_SOURCE_SEL */
	dl(0xE2000000 | 0u << 8 | 28, 0x00553038u);
	run();
	CHECK_EQ(gs.ol, 0x0055303cu);                 /* sigue al RDP dentro de la tarea */
}

/* Room_DrawBackground2D de Zelda OoT: S2DEX2 copia una imagen RGBA16 sin
 * escala al marco con G_BG_COPY; la lista vuelve a F3DEX2 después. */
static void gfx_s2dex_bg_copy(void)
{
	start();
	put_bg_image(40, 20);
	s2dex_enter("RSP Gfx ucode S2DEX  fifo 2.05  prueba de Salsa64");
	dl(0xEF200000, 0x00000001);  /* G_RDPSETOTHERMODE: copia, G_AC_THRESHOLD */
	dl(0x0A000000, 0x80000000 | put_bg(0, 40, 10, 40, 0, 20, 5, 20, 0x80000000 | TEX_PA, 1024, 1024));
	s2dex_leave();
	dl(0xE9000000, 0);
	job_ucode = 0x30000;
	run();
	job_ucode = 0;
	CHECK_EQ(res.bg, 1);
	CHECK_EQ(res.skipped, 0);
	CHECK_EQ(res.dl_errors, 0);
	CHECK_EQ(res.flags & (GFX_RES_FULLSYNC | GFX_RES_LOAD_UCODE | GFX_RES_FOREIGN_END),
		 GFX_RES_FULLSYNC | GFX_RES_LOAD_UCODE);
	/* 80 bytes por fila: las 20 caben en la TMEM, una tira. */
	CHECK_EQ(res.rects, 1);
	const struct gfx_draw *d = draw_n(0);
	CHECK(d != NULL);
	if (!d)
		return;
	CHECK_EQ(d->cycle, G_CYC_COPY);
	CHECK_EQ(d->ntex, 1);
	CHECK_EQ(d->tex[0].width, 40);
	CHECK_EQ(d->tex[0].height, 20);
	CHECK(bg_tex_is(0, 0, 0, 0, 0) && bg_tex_is(0, 39, 19, 39, 19) && bg_tex_is(0, 33, 7, 33, 7));
	const struct gfx_vertex *a = rect_v(0, 0, 0), *b = rect_v(0, 0, 1);
	CHECK(a->x == 10.0f && a->y == 5.0f && b->x == 50.0f && b->y == 25.0f);
	CHECK(fabsf(a->s) < 1e-5f && fabsf(a->t) < 1e-5f && fabsf(b->s - 40.0f) < 1e-4f && fabsf(b->t - 20.0f) < 1e-4f);
}

/* La imagen se repite: desde (300, 18) en una de 320 × 20 el marco da la
 * vuelta en x (20 columnas y luego 300) y en y (2 filas y luego desde 0);
 * 640 bytes por fila: tiras de 6 filas. */
static void gfx_s2dex_bg_wrap(void)
{
	start();
	put_bg_image(320, 20);
	s2dex_enter("RSP Gfx ucode S2DEX  fifo 2.05  prueba de Salsa64");
	dl(0xEF200000, 0x00000001);
	dl(0x0A000000, 0x80000000 | put_bg(300, 320, 0, 320, 18, 20, 0, 14, 0x80000000 | TEX_PA, 1024, 1024));
	s2dex_leave();
	job_ucode = 0x30000;
	run();
	job_ucode = 0;
	CHECK_EQ(res.bg, 1);
	/* Tiras: filas 18–19 (pantalla 0–1), 0–5 (2–7), 6–11 (8–13); dos
	 * rectángulos por tira, en el dibujo de su tira. */
	CHECK_EQ(res.rects, 6);
	static const float y0[3] = { 0, 2, 8 }, y1[3] = { 2, 8, 14 }, t0[3] = { 18, 0, 6 };
	for (int k = 0; k < 6; k++) {
		const struct gfx_vertex *a = rect_v(k / 2, k & 1, 0), *b = rect_v(k / 2, k & 1, 1);
		CHECK(a != NULL);
		if (!a)
			return;
		bool first = (k & 1) == 0;
		CHECK(a->y == y0[k / 2] && b->y == y1[k / 2]);
		CHECK(a->x == (first ? 0.0f : 20.0f) && b->x == (first ? 20.0f : 320.0f));
		CHECK(fabsf(a->s - (first ? 300.0f : 0.0f)) < 1e-3f);
		CHECK(fabsf(a->t - t0[k / 2]) < 1e-3f);
	}
	/* La segunda tira carga las filas 0–5: su texel (0, 0) es el (0, 0). */
	CHECK(bg_tex_is(1, 0, 0, 0, 0) && bg_tex_is(1, 319, 5, 319, 5));
	CHECK(bg_tex_is(0, 0, 0, 0, 18));
}

/* G_BG_1CYC (uObjScaleBg) con escala 2 (dos texels por píxel, u5.10 =
 * 2048): un marco de 20 × 10 recorre la imagen de 40 × 20 en 1 ciclo. Y un
 * S2DEX de la primera generación (1.x) no se interpreta: se salta. */
static void gfx_s2dex_bg_scaled(void)
{
	start();
	put_bg_image(40, 20);
	s2dex_enter("RSP Gfx ucode S2DEX  fifo 2.05  prueba de Salsa64");
	dl(0xEF000000, 0x00000001);  /* 1 ciclo, filtro puntual */
	set_combine(CC(CC_ZERO, CC_ZERO, CC_C_ZERO, CC_T0), CC(CC_A_ZERO, CC_A_ZERO, 7, 6));
	dl(0x09000000, 0x80000000 | put_bg(0, 40, 0, 20, 0, 20, 0, 10, 0x80000000 | TEX_PA, 2048, 2048));
	s2dex_leave();
	job_ucode = 0x30000;
	run();
	job_ucode = 0;
	CHECK_EQ(res.bg, 1);
	CHECK_EQ(res.rects, 1);
	const struct gfx_vertex *a = rect_v(0, 0, 0), *b = rect_v(0, 0, 1);
	CHECK(a != NULL);
	if (!a)
		return;
	CHECK(a->x == 0.0f && a->y == 0.0f && b->x == 20.0f && b->y == 10.0f);
	CHECK(fabsf(b->s - 40.0f) < 1e-3f && fabsf(b->t - 20.0f) < 1e-3f);
	CHECK_EQ(draw_n(0)->cycle, G_CYC_1CYCLE);

	next_task();
	s2dex_enter("RSP Gfx ucode S2DEX  fifo 1.06  prueba de Salsa64");
	dl(0xEF200000, 0x00000001);
	dl(0x0A000000, 0x80000000 | put_bg(0, 40, 0, 40, 0, 20, 0, 20, 0x80000000 | TEX_PA, 1024, 1024));
	s2dex_leave();
	job_ucode = 0x30000;
	run();
	job_ucode = 0;
	CHECK_EQ(res.bg, 0);
	CHECK_EQ(res.rects, 0);
	CHECK_EQ(res.skipped, 4);
}

/* BG_1CYC no llena un marco sobrado con copias de la misma imagen. Las
 * cotas de los casos sobredimensionados se contrastaron con S2DEX real;
 * aquí tanto las listas como las imágenes son sintéticas. */
static void gfx_s2dex_bg_oversized(void)
{
	static const struct {
		uint32_t iw, ih, fw, fh, sw, sh, want_w, want_h;
		int x, y;
	} cases[] = {
		{ 176, 28, 352, 56, 512, 1024, 352, 27, 0, 0 },
		{ 40, 20, 100, 60, 1024, 1024, 39, 19, 0, 0 },
		{ 40, 20, 80, 40, 2048, 2048, 19, 9, 0, 0 },
		{ 40, 20, 80, 40, 1536, 1536, 26, 13, 0, 0 },
		{ 40, 20, 100, 60, 1024, 1024, 39, 19, -7, -3 },
		{ 40, 20, 20, 10, 1024, 1024, 20, 10, 0, 0 },
	};
	for (unsigned mode = 0; mode < 3; mode++) {
		bool old = mode == 1, copy = mode == 2;
		for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
			start();
			job_flags = old ? GFX_JOB_S2DEX_BG_OLD : 0;
			put_bg_image(cases[k].iw, cases[k].ih);
			s2dex_enter("RSP Gfx ucode S2DEX fifo 2.05 prueba");
			dl(copy ? 0xEF200000 : 0xEF000000, 0x00000001);
			set_combine(CC(CC_ZERO, CC_ZERO, CC_C_ZERO, CC_T0), CC(CC_A_ZERO, CC_A_ZERO, 7, 6));
			dl(copy ? 0x0A000000 : 0x09000000, 0x80000000 | put_bg(0, cases[k].iw, cases[k].x, cases[k].fw, 0,
				cases[k].ih, cases[k].y, cases[k].fh, 0x80000000 | TEX_PA, cases[k].sw, cases[k].sh));
			s2dex_leave();
			job_ucode = 0x30000;
			run();
			job_ucode = 0;
			CHECK_EQ(res.bg, 1);
			CHECK_EQ(res.dl_errors, 0);
			float xmax = 0, ymax = 0;
			for (unsigned v = 0; v < res.vtx_bytes / sizeof out_vtx[0]; v++) {
				CHECK(out_vtx[v].x >= 0 && out_vtx[v].y >= 0);
				if (out_vtx[v].x > xmax) xmax = out_vtx[v].x;
				if (out_vtx[v].y > ymax) ymax = out_vtx[v].y;
			}
			CHECK(xmax == (float)(cases[k].x + (int)(old || copy ? cases[k].fw : cases[k].want_w)));
			CHECK(ymax == (float)(cases[k].y + (int)(old || copy ? cases[k].fh : cases[k].want_h)));
		}
	}
	job_flags = 0;
}

/* Paso entre filas de un fondo cuyo imageW no llena palabras de 64 bits
 * (Worms Armageddon, #57: imageW = 321 con filas de 320 bytes en CI8). El
 * microcódigo avanza imageW redondeado hacia abajo a 8 bytes por fila: aquí
 * una imagen RGBA16 con filas de 40 texels (80 bytes) declarada de 41. Con
 * el paso de imageW (GFX_JOB_S2DEX_BG_ROW_OLD) cada fila se corría un texel
 * y la imagen salía inclinada. */
static void s2dex_bg_odd_scene(uint32_t flags)
{
	start();
	job_flags = flags;
	put_bg_image(40, 20);
	s2dex_enter("RSP Gfx ucode S2DEX  fifo 2.05  prueba de Salsa64");
	dl(0xEF000000, 0x00000001);  /* 1 ciclo, filtro puntual */
	set_combine(CC(CC_ZERO, CC_ZERO, CC_C_ZERO, CC_T0), CC(CC_A_ZERO, CC_A_ZERO, 7, 6));
	dl(0x09000000, 0x80000000 | put_bg(0, 41, 0, 40, 0, 20, 0, 10, 0x80000000 | TEX_PA, 1024, 1024));
	s2dex_leave();
	job_ucode = 0x30000;
	run();
	job_ucode = 0;
}

static void gfx_s2dex_bg_row_stride(void)
{
	s2dex_bg_odd_scene(0);
	CHECK_EQ(res.bg, 1);
	CHECK_EQ(res.rects, 1);
	CHECK(bg_tex_is(0, 0, 0, 0, 0) && bg_tex_is(0, 0, 1, 0, 1) && bg_tex_is(0, 39, 9, 39, 9));
	CHECK(bg_tex_is(0, 17, 6, 17, 6));
	/* La columna 41 de una fila es la primera de la siguiente. */
	CHECK(bg_tex_is(0, 40, 3, 0, 4));

	/* El interruptor: el paso de 41 texels corre cada fila uno más. */
	s2dex_bg_odd_scene(GFX_JOB_S2DEX_BG_ROW_OLD);
	CHECK_EQ(res.bg, 1);
	CHECK(bg_tex_is(0, 0, 0, 0, 0) && bg_tex_is(0, 0, 1, 1, 1) && bg_tex_is(0, 0, 2, 2, 2));
}

/* ---- S2DEX suelto (tareas SALSA64_GBI_S2DEX y S2DEX1): objetos ---- */

/* uObjTxtr de bloque (G_OBJ_LOADTXTR): la imagen en TEX_PA a la TMEM en
 * `tmem`, de `words` palabras de 64 bits, con la tline (dxt) dada. */
static uint32_t put_txtr_block(uint32_t words, uint32_t tmem, uint32_t tline)
{
	uint32_t a = data_alloc(24);
	uint8_t *p = t_n64.bus.rdram + a;
	memset(p, 0, 24);
	salsa64_wr32(p, 0x00001033);
	salsa64_wr32(p + 4, 0x80000000 | TEX_PA);
	salsa64_wr16(p + 8, (uint16_t)tmem);
	salsa64_wr16(p + 10, (uint16_t)(words - 1));
	salsa64_wr16(p + 12, (uint16_t)tline);
	return a;
}

/* uObjSprite: posición s10.2, escala u5.10 (texels por píxel), tamaño u10.5,
 * imagen RGBA16 con 4 palabras por fila en la TMEM 0. */
static uint32_t put_sprite(int32_t x, int32_t y, uint16_t scale_w, uint16_t scale_h, uint32_t w, uint32_t h,
			   uint32_t flags)
{
	uint32_t a = data_alloc(24);
	uint8_t *p = t_n64.bus.rdram + a;
	memset(p, 0, 24);
	salsa64_wr16(p + 0, (uint16_t)(x * 4));
	salsa64_wr16(p + 2, scale_w);
	salsa64_wr16(p + 4, (uint16_t)(w << 5));
	salsa64_wr16(p + 8, (uint16_t)(y * 4));
	salsa64_wr16(p + 10, scale_h);
	salsa64_wr16(p + 12, (uint16_t)(h << 5));
	salsa64_wr16(p + 16, 4);                /* imageStride */
	p[20] = G_IM_FMT_RGBA;
	p[21] = G_IM_SIZ_16b;
	p[23] = (uint8_t)flags;
	return a;
}

/* uObjMtx: A, B, C, D en s15.16, X e Y en píxeles (s10.2) y la escala base. */
static uint32_t put_objmtx(int32_t a, int32_t b, int32_t c, int32_t d, int32_t x, int32_t y, uint16_t bsx,
			   uint16_t bsy)
{
	uint32_t m = data_alloc(24);
	uint8_t *p = t_n64.bus.rdram + m;
	salsa64_wr32(p, (uint32_t)a);
	salsa64_wr32(p + 4, (uint32_t)b);
	salsa64_wr32(p + 8, (uint32_t)c);
	salsa64_wr32(p + 12, (uint32_t)d);
	salsa64_wr16(p + 16, (uint16_t)(x * 4));
	salsa64_wr16(p + 18, (uint16_t)(y * 4));
	salsa64_wr16(p + 20, bsx);
	salsa64_wr16(p + 22, bsy);
	return m;
}

static void s2dex_task(unsigned gbi)
{
	job_gbi = gbi;
	job_ucode = 0x30000;
	run();
	job_gbi = 0;
	job_ucode = 0;
}

/* S2DEX 2.x suelto: G_OBJ_LOADTXTR y G_OBJ_RECTANGLE (un TEXRECT en la
 * posición del sprite, a su tamaño y con la imagen de la TMEM). */
static void s2dex1_mode_case(bool old)
{
	start();
	job_flags = old ? GFX_JOB_S2DEX_OLD_MODES : 0;
	dl(0xBA000020, 0xFFFFFFFF); dl(0xB9000020, 0xFFFFFFFF);
	dl(0xBA000403, 0); dl(0xB9000403, 0);
	dl(0xBA000800, 0); dl(0xB9000800, 0); dl(0xB8000000, 0);
	s2dex_task(SALSA64_GBI_S2DEX1);
	CHECK_EQ(gs.oh, old ? 0xFFFFFE0F : 0xFFFFFF8F);
	CHECK_EQ(gs.ol, old ? 0xFFFFFE0F : 0xFFFFFF8F);
}
static void gfx_s2dex1_othermode_width(void) { s2dex1_mode_case(false); }
static void gfx_s2dex1_othermode_old(void) { s2dex1_mode_case(true); }
static void gfx_s2dex_task_rectangle(void)
{
	start();
	put_bg_image(16, 16);
	dl(0xEF000000, 0x00000001);  /* 1 ciclo, filtro puntual */
	set_combine(CC(CC_ZERO, CC_ZERO, CC_C_ZERO, CC_T0), CC(CC_A_ZERO, CC_A_ZERO, 7, 6));
	dl(0x05000000, 0x80000000 | put_txtr_block(64, 0, 0x200));
	dl(0x01000000, 0x80000000 | put_sprite(10, 5, 1024, 1024, 16, 16, 0));
	dl(0xE9000000, 0);
	s2dex_task(SALSA64_GBI_S2DEX);
	CHECK_EQ(res.bg, 1);
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.dl_errors, 0);
	CHECK_EQ(res.flags & (GFX_RES_FULLSYNC | GFX_RES_FOREIGN_END), GFX_RES_FULLSYNC);
	CHECK_EQ(res.rects, 1);
	const struct gfx_vertex *a = rect_v(0, 0, 0), *b = rect_v(0, 0, 1);
	CHECK(a != NULL);
	if (!a)
		return;
	CHECK(a->x == 10.0f && a->y == 5.0f && b->x == 26.0f && b->y == 21.0f);
	CHECK(fabsf(a->s) < 1e-5f && fabsf(b->s - 16.0f) < 1e-4f && fabsf(b->t - 16.0f) < 1e-4f);
	CHECK(bg_tex_is(0, 0, 0, 0, 0) && bg_tex_is(0, 15, 15, 15, 15) && bg_tex_is(0, 9, 3, 9, 3));
	/* Con escala 2 (dos texels por píxel) mide la mitad. */
	next_task();
	dl(0x01000000, 0x80000000 | put_sprite(0, 0, 2048, 2048, 16, 16, 0));
	dl(0xE9000000, 0);
	s2dex_task(SALSA64_GBI_S2DEX);
	a = rect_v(0, 0, 0), b = rect_v(0, 0, 1);
	CHECK(a != NULL);
	if (a)
		CHECK(b->x - a->x == 8.0f && b->y - a->y == 8.0f);
}

/* S2DEX 1.x suelto (Yoshi's Story): los opcodes 0x03/0x04/0xC1…, G_OBJ_MOVEMEM
 * con la matriz y G_OBJ_SPRITE como dos triángulos con las esquinas
 * inclusivas (ancho − 1 píxel). Con D negativa la imagen sale volteada. */
static void gfx_s2dex1_task_sprite(void)
{
	start();
	put_bg_image(16, 16);
	dl(0xEF000000, 0x00000001);
	set_combine(CC(CC_ZERO, CC_ZERO, CC_C_ZERO, CC_T0), CC(CC_A_ZERO, CC_A_ZERO, 7, 6));
	dl(0xC1000000, 0x80000000 | put_txtr_block(64, 0, 0x200));
	dl(0x05170000, 0x80000000 | put_objmtx(0x10000, 0, 0, 0x10000, 50, 30, 1024, 1024));
	dl(0x04000000, 0x80000000 | put_sprite(10, 5, 1024, 1024, 16, 16, 0));
	dl(0xB8000000, 0);
	s2dex_task(SALSA64_GBI_S2DEX1);
	CHECK_EQ(res.bg, 1);
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.triangles, 2);
	const struct gfx_draw *d = draw_n(0);
	CHECK(d != NULL);
	if (!d)
		return;
	const struct gfx_vertex *v = &out_vtx[d->h.a];
	/* Esquinas (60, 35) a (75, 50); s, t de 0 a 15. */
	CHECK(v[0].x == 60.0f && v[0].y == 35.0f && v[2].x == 75.0f && v[2].y == 50.0f);
	CHECK(v[0].s == 0.0f && v[0].t == 0.0f && v[2].s == 15.0f && v[2].t == 15.0f);

	/* D = −1: voltea en y y la esquina de arriba es la de abajo de la imagen. */
	next_task();
	dl(0x05170000, 0x80000000 | put_objmtx(0x10000, 0, 0, -0x10000, 50, 30, 1024, 1024));
	dl(0x04000000, 0x80000000 | put_sprite(10, 5, 1024, 1024, 16, 16, 0));
	dl(0xB8000000, 0);
	s2dex_task(SALSA64_GBI_S2DEX1);
	d = draw_n(0);
	CHECK(d != NULL);
	if (!d)
		return;
	v = &out_vtx[d->h.a];
	CHECK(v[0].y == 25.0f && v[2].y == 10.0f);
	CHECK(v[0].t == 0.0f && v[2].t == 15.0f);
}

/* Una tarea S2DEX 2.x que carga otro microcódigo (RE2: F3DEX 2.06) a mitad
 * de la lista sigue con el intérprete de F3DEX2: aquí, un TEXRECT en sus tres
 * comandos. */
static void gfx_s2dex_task_chains_f3dex2(void)
{
	start();
	put_bg_image(8, 8);
	dl(0xEF000000, 0x00000001);
	set_combine(CC(CC_ZERO, CC_ZERO, CC_C_ZERO, CC_T0), CC(CC_A_ZERO, CC_A_ZERO, 7, 6));
	dl(0x05000000, 0x80000000 | put_txtr_block(16, 0, 0x400));
	uint32_t data = data_alloc(64);
	memset(t_n64.bus.rdram + data, 0, 64);
	const char *text = "RSP Gfx ucode F3DEX       fifo 2.06  prueba de Salsa64";
	memcpy(t_n64.bus.rdram + data + 8, text, strlen(text));
	dl(0xE1000000, 0x80000000 | data);
	dl(0xDD00003F, 0x80031000);
	/* Ya en F3DEX2: G_TEXRECT con G_RDPHALF_1 y G_RDPHALF_2. */
	dl(0xE4000000 | 40 << 14 | 40 << 2, 0x00000000 | 4 << 14 | 4 << 2);
	dl(0xE1000000, 0);
	dl(0xF1000000, 0x04000400);
	dl(0xE9000000, 0);
	s2dex_task(SALSA64_GBI_S2DEX);
	CHECK_EQ(res.dl_errors, 0);
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.rects, 1);
	CHECK_EQ(res.flags & (GFX_RES_FULLSYNC | GFX_RES_FOREIGN_END), GFX_RES_FULLSYNC);
}

/* El interruptor de segunda generación no cambia el destino F3DEX 1.x.
 * Su END propio debe terminar sin caer en el intérprete F3DEX2. */
static void gfx_s2dex_chain_first_generation(void)
{
	for (unsigned enabled = 0; enabled < 2; enabled++) {
		start();
		job_gbi = SALSA64_GBI_S2DEX;
		job_flags = enabled ? GFX_JOB_AFFINE_CHAIN : 0;
		uint32_t data = data_alloc(64);
		memset(t_n64.bus.rdram + data, 0, 64);
		static const char text[] = "RSP Gfx ucode F3DEX fifo 1.23 Salsa64";
		memcpy(t_n64.bus.rdram + data, text, sizeof text);
		dl(0xE1000000, data);
		dl(0xDD00003F, 0x00031000);
		dl(0xE9000000, 0);
		dl(0xB8000000, 0);
		run();
		CHECK_EQ(gs.chain_ex, 1);
		CHECK_EQ(res.dl_errors, 0);
		CHECK_EQ(res.unknown_cmds, 0);
		CHECK_EQ(res.flags & (GFX_RES_FULLSYNC | GFX_RES_CUT), GFX_RES_FULLSYNC);
	}
}

/* ---- Caché de texturas ---- */

/* RGBA16 4×2 desde TEX_PA con LOADBLOCK y un TEXRECT en copia. */
static void dl_rgba16_rect(void)
{
	dl(0xFD100000 | 0, TEX_PA);
	dl(0xF5100000, 0x07000000);
	dl(0xF3000000, 0x07000000 | 7 << 12 | 2048);
	dl(0xF5100000 | 1 << 9, 0x00000000);
	dl(0xF2000000, 0x00000000 | (3 << 2) << 12 | (1 << 2));
	set_cycle(G_CYC_COPY);
	dl(0xE4000000 | (3 << 2) << 12 | (1 << 2), 0);
	dl(0xE1000000, 0);
	dl(0xF1000000, 0x10000400);
}

static const uint8_t *tex_of(const struct gfx_tex *t)
{
	return (t->where == GFX_TEX_CACHE ? out_cache : out_tex) + t->offset;
}

/* La misma textura en otra tarea sale de la caché; si cambia un texel en la
 * RDRAM (el último del tramo cargado), se vuelve a decodificar. */
static void gfx_texcache_across_tasks(void)
{
	start();
	cache_task = 1;
	for (int i = 0; i < 8; i++)
		salsa64_wr16(t_n64.bus.rdram + TEX_PA + 2 * i, (uint16_t)((i * 4) << 11 | 1));
	dl_rgba16_rect();
	run();
	const struct gfx_draw *d = draw_n(0);
	CHECK(d != NULL);
	if (!d)
		return;
	CHECK_EQ(res.textures, 1);
	CHECK_EQ(res.tex_old, 0);
	CHECK_EQ(d->tex[0].where, GFX_TEX_CACHE);
	uint32_t first = d->tex[0].offset;
	CHECK_EQ(tex_of(&d->tex[0])[d->tex[0].pitch + 4 * 3], 28u << 3 | 28u >> 2);

	next_task();
	dl_rgba16_rect();
	run();
	d = draw_n(0);
	CHECK_EQ(res.textures, 0);
	CHECK_EQ(res.tex_old, 1);
	CHECK(d && d->tex[0].where == GFX_TEX_CACHE && d->tex[0].offset == first);

	/* Texel (3, 1): el último de los 16 bytes cargados. */
	salsa64_wr16(t_n64.bus.rdram + TEX_PA + 14, (uint16_t)(31u << 6 | 1));
	next_task();
	dl_rgba16_rect();
	run();
	d = draw_n(0);
	CHECK_EQ(res.textures, 1);
	if (!d)
		return;
	CHECK(d->tex[0].offset != first);
	const uint8_t *p = tex_of(&d->tex[0]) + d->tex[0].pitch + 4 * 3;
	CHECK_EQ(p[0], 0);
	CHECK_EQ(p[1], 0xFF);

	/* De vuelta al original: la primera sigue en la caché. */
	salsa64_wr16(t_n64.bus.rdram + TEX_PA + 14, (uint16_t)(28u << 11 | 1));
	next_task();
	dl_rgba16_rect();
	run();
	d = draw_n(0);
	CHECK_EQ(res.textures, 0);
	CHECK(d && d->tex[0].offset == first);

	/* Con GFX_JOB_NOCACHE, al arena. */
	next_task();
	dl_rgba16_rect();
	dl(0xDF000000, 0);
	memset(&job, 0, sizeof job);
	job.rdram_ea = (uint64_t)(uintptr_t)t_n64.bus.rdram;
	job.rdram_size = t_n64.bus.rdram_size;
	job.vtx_ea = (uint64_t)(uintptr_t)out_vtx;
	job.vtx_cap = sizeof out_vtx;
	job.cmd_ea = (uint64_t)(uintptr_t)out_cmd;
	job.cmd_cap = sizeof out_cmd;
	job.tex_ea = (uint64_t)(uintptr_t)out_tex;
	job.tex_cap = sizeof out_tex;
	job.cache_ea = (uint64_t)(uintptr_t)out_cache;
	job.cache_cap = sizeof out_cache;
	job.task = cache_task;
	job.done_task = cache_task - 1;
	job.flags = GFX_JOB_NOCACHE;
	job.gbi = SALSA64_GBI_F3DEX2;
	job.dl = DL_PA;
	gfx_run(&gs, &job, &res);
	d = draw_n(0);
	CHECK_EQ(res.textures, 1);
	CHECK(d && d->tex[0].where == GFX_TEX_ARENA);
}

/* CI4 con paleta: si cambia una entrada de la paleta en la RDRAM, la
 * textura es otra. */
static void dl_ci4_rect(void)
{
	dl(0xFD100000, TEX_PA + 0x100);
	dl(0xF5000100, 0x07000000);
	dl(0xF0000000, 0x07000000 | (15 << 2) << 12);
	dl(0xFD480000 | 3, TEX_PA);
	dl(0xF5480000 | 1 << 9, 0x07000000);
	dl(0xF4000000, 0x07000000 | (3 << 2) << 12 | (1 << 2));
	dl(0xF5400000 | 1 << 9, 0x00000000);
	dl(0xF2000000, (7 << 2) << 12 | (1 << 2));
	dl(0xE3000000 | (32 - 14 - 2) << 8 | 1, 2u << 14);
	set_cycle(G_CYC_COPY);
	dl(0xE4000000 | (7 << 2) << 12 | (0 << 2), 0);
	dl(0xE1000000, 0);
	dl(0xF1000000, 0x10000400);
}

static void gfx_texcache_palette(void)
{
	start();
	cache_task = 1;
	for (int i = 0; i < 16; i++)
		salsa64_wr16(t_n64.bus.rdram + TEX_PA + 0x100 + 2 * i, (uint16_t)((i * 2) << 1 | 1));
	for (int i = 0; i < 8; i++)
		t_n64.bus.rdram[TEX_PA + i] = (uint8_t)((2 * i) << 4 | (2 * i + 1));
	dl_ci4_rect();
	run();
	CHECK_EQ(res.textures, 1);
	next_task();
	dl_ci4_rect();
	run();
	CHECK_EQ(res.textures, 0);
	/* Entrada 3: azul 6 → verde 31. */
	salsa64_wr16(t_n64.bus.rdram + TEX_PA + 0x100 + 6, (uint16_t)(31u << 6 | 1));
	next_task();
	dl_ci4_rect();
	run();
	CHECK_EQ(res.textures, 1);
	const struct gfx_draw *d = draw_n(0);
	if (!d)
		return;
	const uint8_t *p = tex_of(&d->tex[0]);
	CHECK_EQ(p[4 * 3 + 1], 0xFF);  /* texel 3: verde */
	CHECK_EQ(p[4 * 3 + 2], 0);
	CHECK_EQ(p[4 * 4 + 2], 8u << 3 | 8u >> 2);  /* texel 4: azul 8, igual */
}

/* El anillo de la caché: con el consumidor dos tareas atrás, nunca se
 * desaloja ni se pisa una textura que aún puede leer; las entradas no se
 * enciman y el índice las encuentra después de moverse. */
static uint32_t lcg(uint32_t *s)
{
	*s = *s * 1103515245u + 12345u;
	return *s >> 8;
}

/* Tareas con `ops` usos de claves al azar (de `keys` distintas; tamaño
 * 128 o de 128 a 3200 bytes) sobre una región de `cap` bytes. */
static void ring_run(uint32_t cap, uint32_t keys, int ops, bool small, uint32_t *ins, uint32_t *hit, uint32_t *fail)
{
	static struct gfx_state g;
	memset(&g, 0, sizeof g);
	memset(&job, 0, sizeof job);
	memset(&res, 0, sizeof res);
	g.job = &job;
	g.res = &res;
	job.cache_ea = (uint64_t)(uintptr_t)out_cache;  /* el directorio no la toca */
	job.cache_cap = cap;
	enum { KEYS = 4000 };
	static uint32_t last[KEYS], off[KEYS];
	memset(last, 0, sizeof last);
	uint32_t seed = 1;
	*ins = *hit = *fail = 0;
	for (uint32_t task = 1; task <= 400; task++) {
		job.task = task;
		job.done_task = task - 3;
		gfx_tc_begin(&g);
		CHECK(g.tc_on);
		for (int n = 0; n < ops; n++) {
			uint32_t k = lcg(&seed) % (task < 200 ? keys / 8 : keys);
			uint32_t key[2] = { k * 0x9E3779B1u, k };
			bool old;
			const struct gfx_tc_entry *e = gfx_tc_find(&g, key, &old);
			if (e) {
				(*hit)++;
				CHECK_EQ(e->off, off[k]);
				CHECK_EQ(old, last[k] != task);
				last[k] = task;
				continue;
			}
			uint32_t sz = small ? 128 : 128 + (lcg(&seed) % 4) * 1000;
			struct gfx_tc_entry *ne = gfx_tc_insert(&g, key, sz);
			if (!ne) {
				(*fail)++;
				continue;
			}
			(*ins)++;
			CHECK(ne->off + ne->size <= g.tc.cap);
			off[k] = ne->off;
			last[k] = task;
		}
		/* Las que se usaron después de done_task siguen donde estaban. */
		for (uint32_t k = 0; k < keys; k++)
			if (last[k] && (int32_t)(last[k] - job.done_task) > 0) {
				bool found = false;
				for (uint32_t i = 0; i < g.tc.count; i++) {
					const struct gfx_tc_entry *e = &g.tc.e[(g.tc.tail + i) % GFX_TC_ENTRIES];
					if (e->key[1] == k) {
						found = true;
						CHECK_EQ(e->off, off[k]);
					}
				}
				CHECK(found);
			}
		/* En el orden de la región desde head_off y sin encimarse. */
		uint32_t prev_end = g.tc.head_off;
		bool wrapped = false;
		for (uint32_t i = 0; i < g.tc.count; i++) {
			const struct gfx_tc_entry *e = &g.tc.e[(g.tc.tail + i) % GFX_TC_ENTRIES];
			if (e->off < prev_end) {
				CHECK(!wrapped);
				wrapped = true;
			}
			CHECK(!wrapped || e->off + e->size <= g.tc.head_off);
			prev_end = e->off + e->size;
		}
	}
}

static void gfx_texcache_ring(void)
{
	start();
	uint32_t ins, hit, fail;
	/* Texturas grandes: la región se llena antes que el directorio. */
	ring_run(40000, 300, 8, false, &ins, &hit, &fail);
	CHECK(ins > 500 && hit > 500 && res.tex_evicted > 300);
	CHECK(fail < ins / 10);  /* lo que no cabe va al arena */
	/* Texturas chicas y muchas claves: se llena el directorio. */
	ring_run(1u << 20, 4000, 40, true, &ins, &hit, &fail);
	CHECK(ins > 4000 && hit > 1000 && res.tex_evicted > 3000);
	CHECK_EQ(fail, 0);
}

/* El hash ve exactamente el tramo pedido, también al dar la vuelta. */
static void gfx_texcache_hash(void)
{
	static uint8_t ring[4096] __attribute__((aligned(16)));
	for (int i = 0; i < 4096; i++)
		ring[i] = (uint8_t)(i * 7 + 3);
	uint32_t k0[2], k1[2];
	struct gfx_hash h;
	gfx_hash_init(&h);
	gfx_hash_ring(&h, ring, 4096, 8, 24);  /* bytes 8..31 */
	gfx_hash_final(&h, k0);
	ring[7] ^= 1;
	ring[32] ^= 1;
	gfx_hash_init(&h);
	gfx_hash_ring(&h, ring, 4096, 8, 24);
	gfx_hash_final(&h, k1);
	CHECK(k0[0] == k1[0] && k0[1] == k1[1]);
	ring[31] ^= 1;
	gfx_hash_init(&h);
	gfx_hash_ring(&h, ring, 4096, 8, 24);
	gfx_hash_final(&h, k1);
	CHECK(k0[0] != k1[0] && k0[1] != k1[1]);
	/* Vuelta: 4088..4095 y 0..7. */
	gfx_hash_init(&h);
	gfx_hash_ring(&h, ring, 4096, 4088, 16);
	gfx_hash_final(&h, k0);
	ring[3] ^= 0x80;
	gfx_hash_init(&h);
	gfx_hash_ring(&h, ring, 4096, 4088, 16);
	gfx_hash_final(&h, k1);
	CHECK(k0[0] != k1[0]);
	ring[8] ^= 0x80;
	ring[4087] ^= 0x80;
	gfx_hash_init(&h);
	gfx_hash_ring(&h, ring, 4096, 4088, 16);
	gfx_hash_final(&h, k0);
	CHECK(k0[0] == k1[0] && k0[1] == k1[1]);
	/* Mismo contenido en otro lugar alineado igual: misma clave. */
	memcpy(ring + 1024, ring + 512, 256);
	gfx_hash_init(&h);
	gfx_hash_ring(&h, ring, 4096, 512, 256);
	gfx_hash_final(&h, k0);
	gfx_hash_init(&h);
	gfx_hash_ring(&h, ring, 4096, 1024, 256);
	gfx_hash_final(&h, k1);
	CHECK(k0[0] == k1[0] && k0[1] == k1[1]);
}
/* Procedencia de las texturas (gfx_tex.src_*): un LOADTILE de las filas
 * 10–15 de una imagen de 320 (un framebuffer) deja la dirección de su
 * texel (0, 0) y el paso de la imagen; un LOADBLOCK de una textura de 32×32
 * también; y dos cargas con el mismo contenido desde direcciones distintas
 * comparten la textura decodificada pero no el origen. */
static void gfx_texture_provenance(void)
{
	const uint32_t FB_PA = 0x100000;
	start();
	/* Filas 10–15 de un framebuffer RGBA16 de 320: texel = número de fila. */
	for (int y = 10; y < 16; y++)
		for (int x = 0; x < 320; x++)
			salsa64_wr16(t_n64.bus.rdram + FB_PA + (y * 320 + x) * 2, (uint16_t)(y << 11 | 1));
	dl(0xFD100000 | 319, FB_PA);                             /* SETTIMG RGBA16, ancho 320 */
	dl(0xF5100000 | 80 << 9, 0x07000000);                     /* SETTILE 7: línea 80 palabras, tmem 0 */
	dl(0xF4000000 | (0 << 2) << 12 | (10 << 2), 0x07000000 | (319 << 2) << 12 | (15 << 2)); /* LOADTILE */
	dl(0xF5100000 | 80 << 9, 0x00000000);                     /* SETTILE 0 */
	dl(0xF2000000, (319 << 2) << 12 | (5 << 2));              /* tile 0: 320×6 */
	set_cycle(G_CYC_COPY);
	dl(0xE4000000 | (319 << 2) << 12 | (15 << 2), (0 << 2) << 12 | (10 << 2));  /* TEXRECT 0,10–319,15 */
	dl(0xE1000000, 0);
	dl(0xF1000000, 0x10000400);
	/* Una textura normal de 32×32 por LOADBLOCK. */
	dl(0xFD100000 | 31, TEX_PA);
	dl(0xF5100000, 0x07000000);
	dl(0xF3000000, 0x07000000 | 1023 << 12 | 256);          /* LOADBLOCK 1024 texels, dxt 1 fila de 8 bytes */
	dl(0xF5100000 | 8 << 9, 0x00000000);                      /* SETTILE 0: línea 8 palabras */
	dl(0xF2000000, (31 << 2) << 12 | (31 << 2));              /* 32×32 */
	dl(0xE4000000 | (31 << 2) << 12 | (31 << 2), 0);
	dl(0xE1000000, 0);
	dl(0xF1000000, 0x10000400);
	/* El mismo contenido (ceros) desde otra dirección, con ancho 1 en
	 * SETTIMG como lo manda libultra (gDPLoadTextureBlock). */
	dl(0xFD100000 | 0, TEX_PA + 0x4000);
	dl(0xF5100000, 0x07000000);
	dl(0xF3000000, 0x07000000 | 1023 << 12 | 256);
	dl(0xF5100000 | 8 << 9, 0x00000000);
	dl(0xF2000000, (31 << 2) << 12 | (31 << 2));
	dl(0xE4000000 | (31 << 2) << 12 | (31 << 2), 0);
	dl(0xE1000000, 0);
	dl(0xF1000000, 0x10000400);
	/* Una I8 de 32×32 cargada como libultra: SETTIMG de 16 bits, LOADBLOCK
	 * de 512 texels de 16 bits, y la tile que lee de 8 bits. */
	dl(0xFD100000 | 0, TEX_PA + 0x8000);
	dl(0xF5100000, 0x07000000);
	dl(0xF3000000, 0x07000000 | 511 << 12 | 512);
	dl(0xF5880000 | 4 << 9, 0x00000000);                      /* tile 0: I8, línea 4 palabras (32 bytes) */
	dl(0xF2000000, (31 << 2) << 12 | (31 << 2));
	dl(0xE4000000 | (31 << 2) << 12 | (31 << 2), 0);
	dl(0xE1000000, 0);
	dl(0xF1000000, 0x10000400);
	run();
	CHECK_EQ(res.rects, 4);
	const struct gfx_draw *d3 = draw_n(3);
	CHECK(d3 != NULL);
	if (d3) {
		CHECK_EQ(d3->tex[0].src_addr, TEX_PA + 0x8000);
		CHECK_EQ(d3->tex[0].src_pitch, 32);
		CHECK_EQ(d3->tex[0].src_siz, G_IM_SIZ_8b);
	}
	const struct gfx_draw *d0 = draw_n(0), *d1 = draw_n(1), *d2 = draw_n(2);
	CHECK(d0 && d1 && d2);
	if (!d0 || !d1 || !d2)
		return;
	CHECK_EQ(d0->ntex, 1);
	CHECK_EQ(d0->tex[0].width, 320);
	CHECK_EQ(d0->tex[0].height, 6);
	CHECK_EQ(d0->tex[0].src_addr, FB_PA + 10 * 640);
	CHECK_EQ(d0->tex[0].src_pitch, 320);
	CHECK_EQ(d0->tex[0].src_siz, G_IM_SIZ_16b);
	CHECK_EQ(d0->tex[0].src_fmt, G_IM_FMT_RGBA);
	/* El contenido es el del framebuffer: la fila 2 de la textura es la 12. */
	const uint8_t *tex = (d0->tex[0].where == GFX_TEX_CACHE ? out_cache : out_tex) + d0->tex[0].offset;
	CHECK_EQ(tex[2 * d0->tex[0].pitch + 4 * 5], (12 << 3) | (12 >> 2));
	CHECK_EQ(d1->tex[0].src_addr, TEX_PA);
	CHECK_EQ(d1->tex[0].src_pitch, 32);
	CHECK_EQ(d1->tex[0].width, 32);
	CHECK_EQ(d2->tex[0].src_addr, TEX_PA + 0x4000);
	CHECK_EQ(d2->tex[0].src_pitch, 32);
	CHECK_EQ(d2->tex[0].src_siz, G_IM_SIZ_16b);
	/* Misma textura decodificada (reusada), distinto origen. */
	CHECK_EQ(d2->tex[0].offset, d1->tex[0].offset);
	CHECK_EQ(d2->tex[0].where, d1->tex[0].where);
	CHECK(res.tex_reused >= 1);
}

/* Referencia aparte de tmem.c (n64brew.dev, TMEM): texel (x, y) de una
 * tile en 0xRRGGBBAA. Filas de `line` bytes desde `tmem`, las impares con
 * las mitades de cada palabra intercambiadas; 32 bits partidos en dos
 * mitades de la TMEM; paletas de 16 bits en la mitad alta, 8 bytes por
 * entrada. */
static uint32_t ref_c5(uint32_t v)
{
	return (v << 3 | v >> 2) & 0xFF;
}

static uint32_t ref_16(uint32_t p, bool ia)
{
	if (ia)
		return (p >> 8) * 0x01010100u | (p & 0xFF);
	return ref_c5(p >> 11) << 24 | ref_c5((p >> 6) & 31) << 16 | ref_c5((p >> 1) & 31) << 8 | ((p & 1) ? 0xFF : 0);
}

static uint32_t ref_texel(const uint8_t *tm, unsigned fmt, unsigned siz, unsigned pal, unsigned tt, uint32_t tmem,
			  uint32_t line, uint32_t x, uint32_t y)
{
	uint32_t row = tmem + (y & 0xFF) * line, sw = (y & 1) ? 4 : 0;
	if (siz == G_IM_SIZ_32b) {
		uint32_t i = ((tmem / 2 + (y & 0xFF) * (line / 2) + x) ^ (sw / 2)) & 0x3FF;
		return salsa64_rd16(tm + 2 * i) << 16 | salsa64_rd16(tm + 0x800 + 2 * i);
	}
	if (siz == G_IM_SIZ_16b) {
		uint32_t p = salsa64_rd16(tm + (((row + 2 * x) ^ sw) & 0xFFE));
		return fmt == G_IM_FMT_YUV ? 0xFF : ref_16(p, fmt == G_IM_FMT_IA || fmt == G_IM_FMT_I);
	}
	uint32_t v;
	if (siz == G_IM_SIZ_8b) {
		v = tm[((row + x) ^ sw) & 0xFFF];
	} else {
		uint8_t b = tm[((row + x / 2) ^ sw) & 0xFFF];
		v = (x & 1) ? b & 15 : b >> 4;
		if (tt || fmt == G_IM_FMT_CI)
			v |= pal << 4;
	}
	if (tt)
		return ref_16(salsa64_rd16(tm + 0x800 + ((v * 8) & 0x7FF)), tt == G_TT_IA16);
	uint32_t i = v, a = v;
	if (siz == G_IM_SIZ_4b && fmt == G_IM_FMT_IA) {
		i = (v >> 1) << 5 | (v >> 1) << 2 | (v >> 1) >> 1;
		a = (v & 1) ? 0xFF : 0;
	} else if (siz == G_IM_SIZ_4b && fmt != G_IM_FMT_CI) {
		i = a = v * 17;
	} else if (siz == G_IM_SIZ_8b && fmt == G_IM_FMT_IA) {
		i = (v >> 4) * 17;
		a = (v & 15) * 17;
	}
	return i * 0x01010100u | a;
}

/* La decodificación de cada formato (la de RGBA16 va de palabra en
 * palabra; las demás, texel por texel, de 4 en 4 a la salida) contra la
 * referencia, con filas impares, anchos que no son múltiplo de 4 y tiles
 * que dan la vuelta a la TMEM. La TMEM se llena con LOADBLOCK sin dxt: los
 * mismos bytes que la RDRAM. */
/* Espejo y clamp con una tile de dos periodos que no cabe en GFX_TEX_MAX
 * (el lente del telescopio de Majora's Mask: I4 de 64 × 64 en una tile de
 * 128 × 128, 64 KB en RGBA): se decodifica solo el segundo periodo de t
 * (las filas 64–127, reflejo de las 63–0), el eje va con
 * GFX_WRAP_MIRROR_ONCE y v corrido un periodo; s, que sí cabe, se
 * decodifica entero con su reflejo. */
static void gfx_tile_mirror_once(void)
{
	start();
	for (int i = 0; i < 2048; i++)
		t_n64.bus.rdram[TEX_PA + i] = (uint8_t)(37 * i + 11);
	dl(0xFD100000, TEX_PA);                                 /* SETTIMG de 16 bits */
	dl(0xF5100000, 0x07000000);
	dl(0xF3000000, 0x07000000 | 1023 << 12);                /* LOADBLOCK 2 KB */
	dl(0xF5000000 | (uint32_t)G_IM_FMT_I << 21 | (uint32_t)G_IM_SIZ_4b << 19 | 4u << 9,
	   3u << 18 | 6u << 14 | 3u << 8 | 6u << 4);              /* tile 0: I4, línea 32, espejo + clamp, máscara 64 */
	dl(0xF2000000, (uint32_t)(127 << 2) << 12 | (127 << 2)); /* 128 × 128 */
	dl(0xE3000000 | (32 - 12 - 2) << 8 | 1, 0);             /* filtro puntual */
	set_cycle(G_CYC_1CYCLE);
	set_combine(CC_T0, CC_ZERO, CC_SHADE, CC_ZERO, CC_T0, CC_ZERO, CC_SHADE, CC_ZERO);
	dl(0xE4000000 | (uint32_t)(128 << 2) << 12 | (128 << 2), 0);
	dl(0xE1000000, 0);
	dl(0xF1000000, 0x04000400);
	run();
	const struct gfx_draw *d = draw_n(0);
	CHECK(d && d->ntex == 1);
	if (!d || d->ntex != 1)
		return;
	CHECK_EQ(d->tex[0].width, 128);
	CHECK_EQ(d->tex[0].height, 64);
	CHECK_EQ(GFX_TEX_WRAP_S(d->tex[0].flags), GFX_WRAP_CLAMP);
	CHECK_EQ(GFX_TEX_WRAP_T(d->tex[0].flags), GFX_WRAP_MIRROR_ONCE);
	CHECK(d->tex[0].xf[3] > -1.001f && d->tex[0].xf[3] < -0.999f);
	CHECK(d->tex[0].xf[1] > 1.0f / 64.0f - 1e-6f && d->tex[0].xf[1] < 1.0f / 64.0f + 1e-6f);
	const uint8_t *p = tex_of(&d->tex[0]);
	unsigned bad = 0;
	for (uint32_t y = 0; y < 64; y++)
		for (uint32_t x = 0; x < 128; x++)
			bad += salsa64_rd32(p + y * d->tex[0].pitch + 4 * x) !=
			       ref_texel(gs.tmem, G_IM_FMT_I, G_IM_SIZ_4b, 0, 0, 0, 32, x < 64 ? x : 127 - x, 63 - y);
	t_checks++;
	if (bad)
		t_fail(__FILE__, __LINE__, "%u texels distintos del reflejo", bad);
}

/* Clamp con una máscara más chica que la tile, sin caber en GFX_TEX_MAX
 * (el Castillo de Hyrule de Super Smash Bros.: CI4 de 32 × 32 en una tile
 * de 160 × 160 con clamp y máscara 5): t se decodifica en un periodo de su
 * máscara y repite, en vez de cortar filas y estirar la última; s, que
 * cabe, se decodifica entera con su máscara y sigue con clamp. */
static void gfx_tile_clamp_mask_period(void)
{
	start();
	for (int i = 0; i < 2048; i++)
		t_n64.bus.rdram[TEX_PA + i] = (uint8_t)(29 * i + 3);
	dl(0xFD100000, TEX_PA);
	dl(0xF5100000, 0x07000000);
	dl(0xF3000000, 0x07000000 | 1023 << 12);                /* LOADBLOCK 2 KB */
	dl(0xF5000000 | (uint32_t)G_IM_FMT_I << 21 | (uint32_t)G_IM_SIZ_4b << 19 | 2u << 9,
	   2u << 18 | 5u << 14 | 2u << 8 | 5u << 4);              /* tile 0: I4, línea 16, clamp, máscara 32 */
	dl(0xF2000000, (uint32_t)(159 << 2) << 12 | (159 << 2)); /* 160 × 160 */
	dl(0xE3000000 | (32 - 12 - 2) << 8 | 1, 0);             /* filtro puntual */
	set_cycle(G_CYC_1CYCLE);
	set_combine(CC_T0, CC_ZERO, CC_SHADE, CC_ZERO, CC_T0, CC_ZERO, CC_SHADE, CC_ZERO);
	dl(0xE4000000 | (uint32_t)(160 << 2) << 12 | (160 << 2), 0);
	dl(0xE1000000, 0);
	dl(0xF1000000, 0x04000400);
	run();
	const struct gfx_draw *d = draw_n(0);
	CHECK(d && d->ntex == 1);
	if (!d || d->ntex != 1)
		return;
	CHECK_EQ(d->tex[0].width, 160);
	CHECK_EQ(d->tex[0].height, 32);
	CHECK_EQ(GFX_TEX_WRAP_S(d->tex[0].flags), GFX_WRAP_CLAMP);
	CHECK_EQ(GFX_TEX_WRAP_T(d->tex[0].flags), GFX_WRAP_REPEAT);
	CHECK(d->tex[0].xf[1] > 1.0f / 32.0f - 1e-6f && d->tex[0].xf[1] < 1.0f / 32.0f + 1e-6f);
	const uint8_t *p = tex_of(&d->tex[0]);
	unsigned bad = 0;
	for (uint32_t y = 0; y < 32; y++)
		for (uint32_t x = 0; x < 160; x++)
			bad += salsa64_rd32(p + y * d->tex[0].pitch + 4 * x) !=
			       ref_texel(gs.tmem, G_IM_FMT_I, G_IM_SIZ_4b, 0, 0, 0, 16, x & 31, y);
	t_checks++;
	if (bad)
		t_fail(__FILE__, __LINE__, "%u texels distintos del periodo", bad);
}

/* Sin corrección de perspectiva (G_TP_NONE) la textura de un triángulo
 * se muestrea a la mitad de la coordenada del vértice (el RDP usa S·(1/W)
 * tal cual y F3DEX2 normaliza 1/W a la mitad; el sol del telescopio de
 * Majora's Mask): la escala de s y t de la textura se reduce a la mitad.
 * Con G_TP_PERSP no cambia. Los TEXRECT no cambian. */
static void tri_no_persp_half(bool chained, bool enabled)
{
	for (int persp = 0; persp < 2; persp++) {
		start();
		if (chained) {
			if (enabled)
				job_flags |= GFX_JOB_AFFINE_CHAIN;
			job_gbi = SALSA64_GBI_S2DEX;
			uint32_t data = data_alloc(64);
			memset(t_n64.bus.rdram + data, 0, 64);
			static const char text[] = "RSP Gfx ucode F3DEX fifo 2.06 Salsa64";
			memcpy(t_n64.bus.rdram + data, text, sizeof text);
			dl(0xE1000000, data);
			dl(0xDD00003F, 0x00031000);
		}
		viewport_320x240();
		load_mtx(ident, true);
		load_mtx(ident, false);
		for (uint32_t i = 0; i < 64; i++)
			t_n64.bus.rdram[TEX_PA + i] = (uint8_t)(5 * i + 3);
		dl(0xFD100000, TEX_PA);
		dl(0xF5100000, 0x07000000);
		dl(0xF3000000, 0x07000000 | 31 << 12 | 1024);
		dl(0xF5880000 | 2 << 9, 2u << 18 | 2u << 8);          /* tile 0: I8 de 16 × 4, clamp */
		dl(0xF2000000, (15 << 2) << 12 | (3 << 2));
		set_cycle(G_CYC_1CYCLE);
		dl(0xE3000000 | (32 - 19 - 1) << 8 | 0, (uint32_t)persp << 19);  /* G_TP_NONE o G_TP_PERSP */
		set_combine(CC_T0, CC_ZERO, CC_SHADE, CC_ZERO, CC_T0, CC_ZERO, CC_SHADE, CC_ZERO);
		dl(0xD7000002, 0xFFFFFFFF);                            /* G_TEXTURE encendida, escala 1 */
		dl(0xD9000000, G_SHADING_SMOOTH);
		dl(0xFF100000 | 319, 0x00100000);
		const struct vtx v[3] = {
			{ 0, 0, 0, 0, 0, { 255, 255, 255, 255 } },
			{ 1, 0, 0, 16 * 32, 0, { 255, 255, 255, 255 } },
			{ 0, 1, 0, 0, 4 * 32, { 255, 255, 255, 255 } },
		};
		uint32_t va = put_vertices(v, 3);
		dl(0x01000000 | 3 << 12 | (0 + 3) << 1, va);
		/* MODIFYVTX ST no lleva otra escala de G_TEXTURE: el cambio afín
		 * debe vivir en el estado de muestreo, no en estas coordenadas. */
		dl(0x02140002, (16u * 32u) << 16);
		dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);
		if (chained) {
			/* TEXRECT no usa la escala de los triángulos afines. */
			dl(0xE4000000 | 40 << 14 | 40 << 2, 4 << 14 | 4 << 2);
			dl(0xE1000000, 0);
			dl(0xF1000000, 0x04000400);
			/* El tramo 3D carga S2DEX para un sprite y después vuelve.
			 * Sus triángulos crudos ya llevan ST de texel y no se escalan. */
			uint32_t data = data_alloc(64);
			memset(t_n64.bus.rdram + data, 0, 64);
			static const char text[] = "RSP Gfx ucode S2DEX fifo 2.05 Salsa64";
			memcpy(t_n64.bus.rdram + data, text, sizeof text);
			dl(0xE1000000, data);
			dl(0xDD00003F, 0x00032000);
			dl(0x02000000, put_sprite(10, 5, 1024, 1024, 16, 4, 0));
			dl(0xDD000000, 0x00031000);
			dl(0xF5880000 | 2 << 9, 2u << 18 | 2u << 8);
			dl(0xF2000000, (15 << 2) << 12 | (3 << 2));
			dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);
		}
		dl(0xE9000000, 0);
		run();
		const struct gfx_draw *d = draw_n(0);
		CHECK(d && d->ntex == 1);
		if (!d || d->ntex != 1)
			return;
		float k = persp || (chained && !enabled) ? 1.0f : 0.5f;
		CHECK_EQ(out_vtx[1].s, 16.0f);
		CHECK(d->tex[0].xf[0] > k / 16.0f - 1e-6f && d->tex[0].xf[0] < k / 16.0f + 1e-6f);
		CHECK(d->tex[0].xf[1] > k / 4.0f - 1e-6f && d->tex[0].xf[1] < k / 4.0f + 1e-6f);
		if (chained) {
			CHECK_EQ(gs.chain_ex, enabled ? GFX_CHAIN_AFFINE : 0);
			CHECK_EQ(res.draws, 4);
			CHECK_EQ(res.dl_errors, 0);
			CHECK_EQ(res.unknown_cmds, 0);
			for (int i = 1; i < 4; i++) {
				const struct gfx_draw *a = draw_n(i);
				CHECK(a && a->ntex == 1);
				if (!a || a->ntex != 1)
					return;
				float scale = i == 3 ? k : 1.0f;
				CHECK(fabsf(a->tex[0].xf[0] * a->tex[0].width - scale) < 1e-6f);
				CHECK(fabsf(a->tex[0].xf[1] * a->tex[0].height - scale) < 1e-6f);
			}
			if (enabled) {
				/* Repetir la misma entrada ON→OFF sin gfx_init: el estado
				 * transitorio no debe mantener la corrección anterior. */
				job.flags &= ~GFX_JOB_AFFINE_CHAIN;
				gfx_run(&gs, &job, &res);
				CHECK_EQ(gs.chain_ex, 0);
				CHECK_EQ(res.draws, 4);
				for (int i = 0; i < 4; i += 3) {
					const struct gfx_draw *a = draw_n(i);
					CHECK(a && a->ntex == 1);
					if (!a || a->ntex != 1)
						return;
					CHECK(fabsf(a->tex[0].xf[0] * a->tex[0].width - 1.0f) < 1e-6f);
					CHECK(fabsf(a->tex[0].xf[1] * a->tex[0].height - 1.0f) < 1e-6f);
				}
			}
			/* La tarea siguiente no hereda la procedencia S2DEX. */
			dl_pc = DL_PA;
			job_flags &= ~GFX_JOB_AFFINE_CHAIN;
			job_gbi = SALSA64_GBI_F3DEX2;
			dl(0xE9000000, 0);
			run();
			CHECK_EQ(gs.chain_ex, 0);
		}
	}
}

static void gfx_tri_no_persp_half(void)
{
	tri_no_persp_half(false, false);
}

/* El mismo intérprete F3DEX2 debe producir la misma escala cuando empieza
 * en una tarea S2DEX que lo carga después con G_LOAD_UCODE. Los TEXRECT y
 * los triángulos con perspectiva no cambian de escala. */
static void gfx_s2dex_chained_affine_texture(void)
{
	tri_no_persp_half(true, false);
	tri_no_persp_half(true, true);
}

static void gfx_tmem_decode_formats(void)
{
	static const struct {
		uint8_t fmt, siz, pal, tt;
		uint16_t tmem, line, w, h;
	} cases[] = {
		{ G_IM_FMT_RGBA, G_IM_SIZ_16b, 0, 0, 0, 16, 7, 5 },
		{ G_IM_FMT_RGBA, G_IM_SIZ_16b, 0, 0, 4064, 24, 11, 6 },  /* da la vuelta */
		{ G_IM_FMT_CI, G_IM_SIZ_16b, 0, 0, 8, 32, 16, 3 },
		{ G_IM_FMT_IA, G_IM_SIZ_16b, 0, 0, 8, 8, 4, 3 },
		{ G_IM_FMT_I, G_IM_SIZ_16b, 0, 0, 2048, 16, 5, 4 },
		{ G_IM_FMT_YUV, G_IM_SIZ_16b, 0, 0, 0, 8, 3, 2 },
		{ G_IM_FMT_RGBA, G_IM_SIZ_32b, 0, 0, 0, 8, 4, 3 },
		{ G_IM_FMT_RGBA, G_IM_SIZ_32b, 0, 0, 2040, 16, 6, 3 },
		{ G_IM_FMT_IA, G_IM_SIZ_8b, 0, 0, 16, 16, 13, 4 },
		{ G_IM_FMT_I, G_IM_SIZ_8b, 0, 0, 4088, 16, 9, 3 },
		{ G_IM_FMT_CI, G_IM_SIZ_8b, 0, 0, 0, 8, 8, 2 },
		{ G_IM_FMT_CI, G_IM_SIZ_8b, 0, G_TT_RGBA16, 64, 24, 21, 5 },
		{ G_IM_FMT_CI, G_IM_SIZ_8b, 0, G_TT_IA16, 64, 8, 6, 3 },
		{ G_IM_FMT_IA, G_IM_SIZ_4b, 0, 0, 24, 8, 15, 3 },
		{ G_IM_FMT_I, G_IM_SIZ_4b, 0, 0, 0, 16, 31, 5 },
		{ G_IM_FMT_CI, G_IM_SIZ_4b, 5, 0, 32, 8, 9, 2 },
		{ G_IM_FMT_CI, G_IM_SIZ_4b, 3, G_TT_RGBA16, 4080, 8, 16, 4 },
	};
	uint32_t seed = 0x5A15A64u;
	for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
		start();
		if (cases[c].fmt == G_IM_FMT_YUV)
			job_flags = GFX_JOB_NO_YUV;  /* cubre también el diagnóstico anterior */
		for (int i = 0; i < 4096; i++)
			t_n64.bus.rdram[TEX_PA + i] = (uint8_t)(lcg(&seed) >> 24);
		dl(0xFD100000, TEX_PA);                            /* SETTIMG RGBA16 */
		dl(0xF5100000, 0x07000000);                        /* SETTILE 7, tmem 0 */
		dl(0xF3000000, 0x07000000 | 2047 << 12);           /* LOADBLOCK 4 KB, sin dxt */
		dl(0xF5000000 | (uint32_t)cases[c].fmt << 21 | (uint32_t)cases[c].siz << 19 |
			   (uint32_t)(cases[c].line / 8) << 9 | cases[c].tmem / 8,
		   (uint32_t)cases[c].pal << 20);
		dl(0xF2000000, (uint32_t)(cases[c].w - 1) << 14 | (uint32_t)(cases[c].h - 1) << 2);
		dl(0xE3000000 | (32 - 14 - 2) << 8 | 1, (uint32_t)cases[c].tt << 14);
		set_cycle(G_CYC_COPY);
		dl(0xE4000000 | (uint32_t)(cases[c].w - 1) << 14 | (uint32_t)(cases[c].h - 1) << 2, 0);
		dl(0xE1000000, 0);
		dl(0xF1000000, 0x10000400);
		run();
		const struct gfx_draw *d = draw_n(0);
		CHECK(d && d->ntex == 1);
		if (!d || !d->ntex)
			return;
		CHECK_EQ(d->tex[0].width, cases[c].w);
		CHECK_EQ(d->tex[0].height, cases[c].h);
		const uint8_t *tex = tex_of(&d->tex[0]);
		unsigned bad = 0;
		for (uint32_t y = 0; y < cases[c].h; y++)
			for (uint32_t x = 0; x < cases[c].w; x++)
				bad += salsa64_rd32(tex + y * d->tex[0].pitch + 4 * x) !=
				       ref_texel(gs.tmem, cases[c].fmt, cases[c].siz, cases[c].pal, cases[c].tt,
						 cases[c].tmem, cases[c].line, x, y);
		t_checks++;
		if (bad)
			t_fail(__FILE__, __LINE__, "caso %u: %u texels distintos de la referencia", c, bad);
	}
}

/* Perfect Dark copia el cuadro anterior en franjas: LOADBLOCK de una fila de
 * 320 texels y TEXRECT de 320 × 1 con una tile de 513 × 9 (más grande que
 * la TMEM). Solo se decodifica lo que el rectángulo puede muestrear: 320 × 1
 * con filtro puntual, y como es una sola fila su origen tiene el paso de
 * SETTIMG (320), no el de la línea de la tile (640). Con una tile que cabe
 * en la TMEM, la tile entera, con el paso de la línea. */
static void gfx_texrect_sampled_texels(void)
{
	for (int fits = 0; fits < 2; fits++) {
		start();
		for (int i = 0; i < 320; i++)
			salsa64_wr16(t_n64.bus.rdram + TEX_PA + 2 * i, (uint16_t)(i << 1 | 1));
		dl(0xFD100000 | 319, TEX_PA);                      /* SETTIMG RGBA16, 320 de ancho */
		dl(0xF5100000, 0x07000000);
		dl(0xF3000000, 0x07000000 | 319 << 12);            /* LOADBLOCK 320 texels */
		dl(0xF5100000 | 160 << 9, 0);                      /* tile 0: línea 1280 bytes */
		if (fits)
			dl(0xF2000000, 63u << 14 | 7u << 2);           /* 64 × 8: 1 KB */
		else
			dl(0xF2000000, 2048u << 12 | 32u);             /* 513 × 9: 9 KB */
		set_cycle(G_CYC_1CYCLE);  /* filtro puntual */
		set_combine(CC(CC_ZERO, CC_ZERO, CC_C_ZERO, CC_T0), CC(CC_A_ZERO, CC_A_ZERO, 7, 6));
		uint32_t right = fits ? 40 : 320;
		/* TEXRECT (0, 20)–(right, 21), s = t = 0, dsdx = dtdy = 1. */
		dl(0xE4000000 | right << 14 | 21 << 2, 20 << 2);
		dl(0xE1000000, 0);
		dl(0xF1000000, 0x04000400);
		run();
		const struct gfx_draw *d = draw_n(0);
		CHECK(d && d->ntex == 1);
		if (!d || !d->ntex)
			return;
		CHECK_EQ(d->tex[0].width, fits ? 64 : 320);
		CHECK_EQ(d->tex[0].height, fits ? 8 : 1);
		CHECK_EQ(d->tex[0].src_addr, TEX_PA);
		CHECK_EQ(d->tex[0].src_pitch, fits ? 640 : 320);
		const uint8_t *tex = tex_of(&d->tex[0]);
		unsigned bad = 0;
		for (uint32_t x = 0; x < (fits ? 64u : 320u); x++)
			bad += tex[4 * x + 2] != ref_c5(x & 31) || tex[4 * x + 3] != 0xFF;
		CHECK_EQ(bad, 0);
		/* u = s·xf0 + xf2 normaliza al ancho decodificado. */
		CHECK(fabsf(d->tex[0].xf[0] * d->tex[0].width - 1.0f) < 1e-5f);
	}
}

/* Una franja de la fila `y` de un framebuffer RGBA16 de 320 en `fb`, como
 * la copia Perfect Dark: SETTIMG de 320 en la fila, LOADBLOCK de 320
 * texels, tile de 513 × 9 (1280 bytes por línea) y TEXRECT de 320 × 1 en la
 * misma fila, filtro puntual. */
static void pd_stripe(uint32_t fb, uint32_t y)
{
	dl(0xFD100000 | 319, fb + y * 640);
	dl(0xF5100000, 0x07000000);
	dl(0xF3000000, 0x07000000 | 319 << 12);
	dl(0xF5100000 | 160 << 9, 0);
	dl(0xF2000000, 2048u << 12 | 32u);
	dl(0xE4000000 | 320u << 14 | (y + 1) << 2, y << 2);
	dl(0xE1000000, 0);
	dl(0xF1000000, 0x04000400);
}

/* Un dibujo en la color image de 320 en `fb` (rectángulo de relleno). */
static void draw_on(uint32_t fb)
{
	dl(0xFF100000 | 319, fb);
	set_cycle(G_CYC_FILL);
	dl(0xF7000000, 0xF801F801);
	dl(0xF6000000 | (319 << 2) << 12 | (239 << 2), 0);
}

/* Con GFX_JOB_FBTEX, una textura RGBA16 que viene de una color image que la
 * HLE vio dibujar hace GFX_FB_FRESH tareas o menos no se identifica ni se
 * decodifica: sale como GFX_TEX_TARGET con su tamaño y su procedencia, para
 * que el consumidor la lea del destino (el cuadro anterior de Perfect Dark;
 * las franjas de Zelda sobre su propio framebuffer). Se decodifica como
 * siempre sin la bandera, de una imagen sin dibujar desde hace más tareas,
 * con otro paso, o si la tile no es RGBA16. */
static void gfx_texture_from_target(void)
{
	const uint32_t FB_A = 0x100000, FB_B = 0x140000;
	start();
	for (int x = 0; x < 320; x++)
		salsa64_wr16(t_n64.bus.rdram + FB_A + 20 * 640 + 2 * x, (uint16_t)(x << 1 | 1));
	job_flags = GFX_JOB_FBTEX;
	next_task();  /* tarea 1: se dibuja FB_A */
	draw_on(FB_A);
	run();
	CHECK_EQ(gs.fbimg[0].width, 320);
	CHECK_EQ(gs.fbimg[0].addr, FB_A);
	CHECK_EQ(gs.fbimg[0].lh, 240);

	/* Tarea 2: la franja de FB_A sobre FB_B no se decodifica. */
	next_task();
	draw_on(FB_B);
	set_cycle(G_CYC_1CYCLE);
	set_combine(CC(CC_ZERO, CC_ZERO, CC_C_ZERO, CC_T0), CC(CC_A_ZERO, CC_A_ZERO, 7, 6));
	pd_stripe(FB_A, 20);
	run();
	const struct gfx_draw *d = draw_n(1);
	CHECK(d && d->ntex == 1);
	if (!d || !d->ntex)
		return;
	CHECK_EQ(d->tex[0].where, GFX_TEX_TARGET);
	CHECK_EQ(d->tex[0].offset, 0);
	CHECK_EQ(d->tex[0].width, 320);
	CHECK_EQ(d->tex[0].height, 1);
	CHECK_EQ(d->tex[0].pitch, (320 * 4 + 63) & ~63u);
	CHECK_EQ(d->tex[0].src_addr, FB_A + 20 * 640);
	CHECK_EQ(d->tex[0].src_pitch, 320);
	CHECK_EQ(d->tex[0].src_siz, G_IM_SIZ_16b);
	CHECK_EQ(res.textures, 0);
	CHECK_EQ(res.tex_target, 1);
	CHECK_EQ(res.tex_bytes, 0);
	/* Lo que decodificaría el consumidor desde la RDRAM es lo que habría
	 * dado la HLE: el texel 5 (canales de 5 bits = 5 >> 1 en el azul…). */
	uint8_t here[320 * 4];
	rdp_decode_rgba16(t_n64.bus.rdram, t_n64.bus.rdram_size, d->tex[0].src_addr, d->tex[0].src_pitch, 320, 1, here,
			  sizeof here);
	CHECK_EQ(here[4 * 5 + 2], ref_c5(5));
	CHECK_EQ(here[4 * 5 + 3], 0xFF);

	/* Sin la bandera, la misma franja se decodifica (y da lo mismo). */
	job_flags = 0;
	next_task();
	draw_on(FB_B);
	set_cycle(G_CYC_1CYCLE);
	set_combine(CC(CC_ZERO, CC_ZERO, CC_C_ZERO, CC_T0), CC(CC_A_ZERO, CC_A_ZERO, 7, 6));
	pd_stripe(FB_A, 20);
	run();
	d = draw_n(1);
	CHECK(d && d->ntex == 1 && d->tex[0].where != GFX_TEX_TARGET);
	if (d && d->ntex == 1 && d->tex[0].where != GFX_TEX_TARGET)
		CHECK(memcmp(tex_of(&d->tex[0]), here, sizeof here) == 0);
	CHECK_EQ(res.tex_target, 0);

	/* FB_A dibujada hace exactamente GFX_FB_FRESH tareas: todavía del
	 * destino; una tarea más tarde, de la RDRAM (decodificada o de la caché). */
	job_flags = GFX_JOB_FBTEX;
	next_task();
	draw_on(FB_A);
	run();
	for (int k = 0; k < GFX_FB_FRESH - 1; k++) {
		next_task();
		draw_on(FB_B);
		run();
	}
	for (int k = 0; k < 2; k++) {
		next_task();
		draw_on(FB_B);
		set_cycle(G_CYC_1CYCLE);
		set_combine(CC(CC_ZERO, CC_ZERO, CC_C_ZERO, CC_T0), CC(CC_A_ZERO, CC_A_ZERO, 7, 6));
		pd_stripe(FB_A, 20);
		run();
		CHECK_EQ(res.tex_target, k == 0 ? 1u : 0u);
	}
	CHECK(res.textures + res.tex_reused >= 1);

	/* En la misma tarea: Zelda copia su framebuffer en franjas de 320 × 6
	 * con LOADTILE (paso de SETTIMG) y la tile IA16 de la misma imagen sí se
	 * decodifica. */
	next_task();
	draw_on(FB_A);
	set_cycle(G_CYC_COPY);
	dl(0xFD100000 | 319, FB_A);
	dl(0xF5100000 | 80 << 9, 0x07000000);
	dl(0xF4000000 | (10 << 2), 0x07000000 | (319 << 2) << 12 | (15 << 2));
	dl(0xF5100000 | 80 << 9, 0x00000000);
	dl(0xF2000000, (319 << 2) << 12 | (5 << 2));
	dl(0xE4000000 | (319 << 2) << 12 | (15 << 2), (0 << 2) << 12 | (10 << 2));
	dl(0xE1000000, 0);
	dl(0xF1000000, 0x10000400);
	dl(0xF5700000 | 80 << 9, 0x00000000);                     /* tile 0: IA16 */
	dl(0xE4000000 | (319 << 2) << 12 | (15 << 2), (0 << 2) << 12 | (10 << 2));
	dl(0xE1000000, 0);
	dl(0xF1000000, 0x10000400);
	run();
	const struct gfx_draw *z0 = draw_n(1), *z1 = draw_n(2);
	CHECK(z0 && z1);
	if (!z0 || !z1)
		return;
	CHECK_EQ(z0->tex[0].where, GFX_TEX_TARGET);
	CHECK_EQ(z0->tex[0].src_addr, FB_A + 10 * 640);
	CHECK_EQ(z0->tex[0].src_pitch, 320);
	CHECK_EQ(z0->tex[0].height, 6);
	CHECK(z1->tex[0].where != GFX_TEX_TARGET);
	CHECK_EQ(res.tex_target, 1);
}

/* G_RM_VISCVG (IM_RD | FORCE_BL, blender IN·0 + BL·A_MEM): el dibujo sale
 * marcado para que el consumidor deje cobertura plena en vez de dibujar;
 * G_RM_PASS (FORCE_BL, IN·0 + IN·1), no. */
static void gfx_viscvg_flag(void)
{
	start();
	dl(0xFD100000 | 319, TEX_PA);
	dl(0xF5100000 | 80 << 9, 0x07000000);
	dl(0xF4000000, 0x07000000 | (319 << 2) << 12 | (5 << 2));
	dl(0xF5100000 | 80 << 9, 0x00000000);
	dl(0xF2000000, (319 << 2) << 12 | (5 << 2));
	set_cycle(G_CYC_1CYCLE);
	/* G_RM_PASS: c1 = (IN, 0, IN, 1) → 0x0C080000 y FORCE_BL. */
	dl(0xEF000000, 0x0C080000 | FORCE_BL);
	dl(0xE4000000 | (319 << 2) << 12 | (5 << 2), 0);
	dl(0xE1000000, 0);
	dl(0xF1000000, 0x04000400);
	/* G_RM_VISCVG: c1 = (IN, 0, BL, A_MEM) → 0x0C840000 con IM_RD y FORCE_BL. */
	dl(0xEF000000, 0x0C840000 | IM_RD | FORCE_BL);
	dl(0xE4000000 | (319 << 2) << 12 | (239 << 2), 0);
	dl(0xE1000000, 0);
	dl(0xF1000000, 0x04000400);
	run();
	const struct gfx_draw *d0 = draw_n(0), *d1 = draw_n(1);
	CHECK(d0 && d1);
	if (!d0 || !d1)
		return;
	CHECK_EQ(d0->viscvg, 0);
	CHECK_EQ(d1->viscvg, 1);
}

/* F3DEX2 arranca con clip ratio 2 (sus datos) y G_MOVEWORD G_MW_CLIP la
 * cambia: la caja de recorte del RSP va en las tijeras de los triángulos. */
static void gfx_clip_ratio_f3dex2(void)
{
	start();
	uint32_t a = data_alloc(16);
	const int16_t vp[8] = { 160, 120, 511, 0, 640, 480, 511, 0 };  /* 40×30 en (160, 120) */
	for (int i = 0; i < 8; i++)
		salsa64_wr16(t_n64.bus.rdram + a + 2 * i, (uint16_t)vp[i]);
	dl(0xDC080008, a);
	load_mtx(ident, true);
	load_mtx(ident, false);
	set_combine(CC_A_ZERO, CC_ZERO, CC_C_ZERO, CC_SHADE, CC_ZERO, CC_ZERO, CC_ZERO, CC_SHADE);
	dl(0xD9000000, G_SHADING_SMOOTH);
	dl(0xFF100000 | 319, 0x00100000);
	const struct vtx v[3] = {
		{ -4, -4, 0, 0, 0, { 255, 255, 255, 255 } },
		{ 4, -4, 0, 0, 0, { 255, 255, 255, 255 } },
		{ 0, 4, 0, 0, 0, { 255, 255, 255, 255 } },
	};
	dl(0x01000000 | 3 << 12 | 3 << 1, put_vertices(v, 3));
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);
	dl(0xDB040004, 1);       /* gSPClipRatio(FRUSTRATIO_1) */
	dl(0xDB04000C, 1);
	dl(0xDB040014, 0xFFFF);
	dl(0xDB04001C, 0xFFFF);
	dl(0x05000000 | 0 << 16 | 1 << 9 | 2 << 1, 0);
	run();
	CHECK_EQ(res.triangles, 2);
	const struct gfx_draw *d0 = draw_n(0), *d1 = draw_n(1);
	CHECK(d0 && d1);
	if (!d0 || !d1)
		return;
	CHECK(d0->scissor[0] == 80 * 4 && d0->scissor[1] == 60 * 4 && d0->scissor[2] == 240 * 4 &&
	      d0->scissor[3] == 180 * 4);
	CHECK(d1->scissor[0] == 120 * 4 && d1->scissor[1] == 90 * 4 && d1->scissor[2] == 200 * 4 &&
	      d1->scissor[3] == 150 * 4);}

/* Misma operación vectorial que la variante SPE; el oráculo usa el OR
 * condicional de la ruta escalar. Incluye igualdad con planos, w negativo,
 * cero con ambos signos y carriles que producen banderas distintas. */
static void gfx_clip_four_planes(void)
{
	static const float values[8] = { -65536.0f, -2.0f, -1.0f, -0.0f, 0.0f, 1.0f, 2.0f, 65536.0f };
	for (unsigned iw = 0; iw < 8; iw++)
	for (unsigned ix = 0; ix < 8; ix++)
	for (unsigned iy = 0; iy < 8; iy++)
	for (unsigned iz = 0; iz < 8; iz++) {
		union { gfx_clip4f q; float f[4]; } x, y, z, w;
		union { gfx_clip4u q; uint32_t u[4]; } clips;
		for (unsigned lane = 0; lane < 4; lane++) {
			x.f[lane] = values[(ix + lane) & 7];
			y.f[lane] = values[(iy + lane * 3) & 7];
			z.f[lane] = values[(iz + lane * 5) & 7];
			w.f[lane] = values[(iw + lane * 7) & 7];
		}
		clips.q = gfx_clip4_mask(x.q, y.q, z.q, w.q);
		for (unsigned lane = 0; lane < 4; lane++) {
			uint32_t ref = 0;
			if (x.f[lane] < -w.f[lane]) ref |= GFX_CLIP_XN;
			if (x.f[lane] > w.f[lane]) ref |= GFX_CLIP_XP;
			if (y.f[lane] < -w.f[lane]) ref |= GFX_CLIP_YN;
			if (y.f[lane] > w.f[lane]) ref |= GFX_CLIP_YP;
			if (z.f[lane] > w.f[lane]) ref |= GFX_CLIP_ZF;
			if (w.f[lane] < 0.0f) ref |= GFX_CLIP_W;
			CHECK_EQ(clips.u[lane], ref);
		}
	}
}

void t_suite_gfx(void)
{
	RUN(gfx_clip_four_planes);
	RUN(gfx_combiner_forms);
	RUN(gfx_s2dex_task_rectangle);
	RUN(gfx_s2dex1_othermode_width);
	RUN(gfx_s2dex1_othermode_old);
	RUN(gfx_s2dex1_task_sprite);
	RUN(gfx_s2dex_task_chains_f3dex2);
	RUN(gfx_s2dex_chain_first_generation);
	RUN(gfx_setconvert_k5);
	RUN(gfx_yuv_texture);
	RUN(gfx_texrect_two_tiles);
	RUN(gfx_untextured_texel);
	RUN(gfx_combiner_shade_squared);
	RUN(gfx_triangle_transform);
	RUN(gfx_interp_groups);
	RUN(gfx_interp_budget);
	RUN(gfx_clip_ratio_f3dex2);
	RUN(gfx_matrix_stack);
	RUN(gfx_insert_matrix_billboard);
	RUN(gfx_lighting);
	RUN(gfx_texgen);
	RUN(gfx_tmem_rgba16_block);
	RUN(gfx_tmem_ci4_tlut);
	RUN(gfx_tmem_decode_formats);
	RUN(gfx_texrect_sampled_texels);
	RUN(gfx_combiner_premul);
	RUN(gfx_tmem_premul);
	RUN(gfx_fill_and_zclear);
	RUN(gfx_eurocom_rdp_block);
	RUN(gfx_zclear_fills_rdram);
	RUN(gfx_zclear_scissor);
	RUN(gfx_zclear_rdram_at_finish);
	RUN(gfx_sync_dpc_nocache);
	RUN(gfx_fill_1cycle_edge);
	RUN(gfx_draw_batching);
	RUN(gfx_cvg_x_alpha);
	RUN(gfx_cvg_bit);
	RUN(gfx_dl_flow);
	RUN(gfx_load_ucode_skips_foreign);
	RUN(gfx_load_ucode_same_generation);
	RUN(gfx_ucode_kind_texts);
	RUN(gfx_f3dflx_alpha);
	RUN(gfx_fog_1cycle_force_bl);
	RUN(gfx_rsp_clip_box);
	RUN(gfx_s2dex_bg_copy);
	RUN(gfx_s2dex_bg_wrap);
	RUN(gfx_s2dex_bg_scaled);
	RUN(gfx_s2dex_bg_oversized);
	RUN(gfx_s2dex_bg_row_stride);
	RUN(gfx_texcache_across_tasks);
	RUN(gfx_texcache_palette);
	RUN(gfx_texcache_ring);
	RUN(gfx_texcache_hash);
	RUN(gfx_texture_provenance);
	RUN(gfx_texture_from_target);
	RUN(gfx_viscvg_flag);
	RUN(gfx_aa_flag);
	RUN(gfx_tile_clamp_mirror_mask);
	RUN(gfx_blender_fog_color);
	RUN(gfx_blender_products);
	RUN(gfx_tile_mirror_once);
	RUN(gfx_tile_clamp_mask_period);
	RUN(gfx_tri_no_persp_half);
	RUN(gfx_s2dex_chained_affine_texture);
	RUN(gfx_f3dex2_othermode_copy);
}
