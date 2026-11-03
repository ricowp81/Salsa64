/*
 * Salsa64 — pruebas de la HLE de F3D (Fast3D) y F3DEX 1.x (src/rsp/gfx/f3d.c):
 * display lists sintéticas con la codificación de la primera generación,
 * ejecutadas con gfx_run y revisadas en el estado y la salida.
 */
#include "test.h"

#include <math.h>
#include <string.h>

#include "core/endian.h"
#include "rsp/gfx/hle.h"

#define DL_PA   0x10000u
#define DATA_PA 0x20000u
#define TEX_PA  0x30000u

static struct gfx_state gs;
static struct gfx_vertex out_vtx[1024] __attribute__((aligned(16)));
static uint8_t out_cmd[16 * 1024] __attribute__((aligned(16)));
static uint8_t out_tex[64 * 1024] __attribute__((aligned(128)));
static struct gfx_job job;
static struct gfx_result res;
static uint32_t dl_pc, data_pc;
static bool ex;  /* F3DEX en vez de F3D */
static bool wr;  /* la variante de Wave Race (índices ×5) */
static uint32_t job_flags;  /* GFX_JOB_* del próximo run */

/* ---- Codificación de la primera generación ---- */

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

static void start(bool f3dex)
{
	ex = f3dex;
	wr = false;
	dl_pc = DL_PA;
	data_pc = DATA_PA;
	job_flags = 0;
	gfx_init(&gs);
}

static void run(void)
{
	dl(0xB8000000, 0);  /* G_ENDDL */
	memset(&job, 0, sizeof job);
	job.rdram_ea = (uint64_t)(uintptr_t)t_n64.bus.rdram;
	job.rdram_size = t_n64.bus.rdram_size;
	job.vtx_ea = (uint64_t)(uintptr_t)out_vtx;
	job.vtx_cap = sizeof out_vtx;
	job.cmd_ea = (uint64_t)(uintptr_t)out_cmd;
	job.cmd_cap = sizeof out_cmd;
	job.tex_ea = (uint64_t)(uintptr_t)out_tex;
	job.tex_cap = sizeof out_tex;
	job.gbi = wr ? SALSA64_GBI_F3DWR : ex ? SALSA64_GBI_F3DEX : SALSA64_GBI_F3D;
	job.dl = DL_PA;
	job.flags = job_flags;
	gfx_run(&gs, &job, &res);
}

static const struct gfx_draw *draw_n(int n)
{
	uint32_t off = 0;
	while (off + sizeof(struct gfx_cmd) <= res.cmd_bytes) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(out_cmd + off);
		if (c->op == GFX_OP_DRAW && n-- == 0)
			return (const struct gfx_draw *)c;
		if (c->op == GFX_OP_END || c->size == 0)
			break;
		off += c->size;
	}
	return NULL;
}

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

/* Banderas de G_MTX en F3D/F3DEX (sin invertir). */
#define MTX_PROJECTION 0x01u
#define MTX_LOAD       0x02u
#define MTX_PUSH       0x04u

static void sp_matrix(const float m[16], uint32_t flags)
{
	dl(0x01000040 | flags << 16, put_matrix(m));
}

static void sp_viewport_320x240(void)
{
	uint32_t a = data_alloc(16);
	const int16_t vp[8] = { 640, 480, 511, 0, 640, 480, 511, 0 };
	for (int i = 0; i < 8; i++)
		salsa64_wr16(t_n64.bus.rdram + a + 2 * i, (uint16_t)vp[i]);
	dl(0x03800010, a);  /* G_MOVEMEM G_MV_VIEWPORT */
}

/* Viewport de 2·sx × 2·sy píxeles con centro en (tx, ty). */
static void sp_viewport(int sx, int sy, int tx, int ty)
{
	uint32_t a = data_alloc(16);
	const int16_t vp[8] = { (int16_t)(sx * 4), (int16_t)(sy * 4), 511, 0, (int16_t)(tx * 4), (int16_t)(ty * 4), 511, 0 };
	for (int i = 0; i < 8; i++)
		salsa64_wr16(t_n64.bus.rdram + a + 2 * i, (uint16_t)vp[i]);
	dl(0x03800010, a);
}

/* gSPClipRatio: G_MOVEWORD G_MW_CLIP en las cuatro posiciones. */
static void sp_clip_ratio(int r)
{
	dl(0xBC000004 | 0x04 << 8, (uint16_t)r);
	dl(0xBC000004 | 0x0C << 8, (uint16_t)r);
	dl(0xBC000004 | 0x14 << 8, (uint16_t)-r);
	dl(0xBC000004 | 0x1C << 8, (uint16_t)-r);
}

static void sp_vertex(uint32_t addr, unsigned n, unsigned v0)
{
	if (ex)
		dl(0x04000000 | v0 * 2 << 16 | n << 10 | (n * 16 - 1), addr);
	else
		dl(0x04000000 | (n - 1) << 20 | v0 << 16 | n * 16, addr);
}

static uint32_t tri_w(unsigned a, unsigned b, unsigned c)
{
	unsigned k = ex ? 2 : 10;
	return a * k << 16 | b * k << 8 | c * k;
}

static void sp_tri1(unsigned a, unsigned b, unsigned c, unsigned flag)
{
	dl(0xBF000000, flag << 24 | tri_w(a, b, c));
}

/* Geometry mode de F3D. */
#define F3D_ZBUFFER  0x00000001u
#define F3D_SHADE    0x00000004u
#define F3D_SMOOTH   0x00000200u
#define F3D_CULL_FRONT 0x00001000u
#define F3D_CULL_BACK  0x00002000u
#define F3D_LIGHTING   0x00020000u
#define F3D_TEXGEN     0x00040000u

static void sp_set_geometry(uint32_t m) { dl(0xB7000000, m); }
static void sp_clear_geometry(uint32_t m) { dl(0xB6000000, m); }

static void sp_othermode(bool high, unsigned shift, unsigned len, uint32_t data)
{
	dl((high ? 0xBA000000u : 0xB9000000u) | shift << 8 | len, data);
}

static void basic_setup(void)
{
	sp_viewport_320x240();
	sp_matrix(ident, MTX_PROJECTION | MTX_LOAD);
	sp_matrix(ident, MTX_LOAD);
	dl(0xFF100000 | 319, 0x00100000);  /* color image */
	/* Combiner SHADE en los dos ciclos. */
	dl(0xFC000000 | (15u << 20 | 31u << 15 | 7u << 12 | 7u << 9 | 15u << 5 | 31u),
	   7u << 28 | 7u << 24 | 7u << 21 | 7u << 18 | 4u << 15 | 7u << 12 | 4u << 9 | 4u << 6 | 7u << 3 | 4u);
}

/* ---- Pruebas ---- */

static void f3d_triangle(void)
{
	start(false);
	basic_setup();
	sp_clear_geometry(0xFFFFFFFF);
	sp_set_geometry(F3D_SHADE | F3D_SMOOTH | F3D_CULL_BACK);
	const struct vtx v[3] = {
		{ 0, 0, 0, 0, 0, { 255, 0, 0, 255 } },
		{ 1, 0, 0, 0, 0, { 0, 255, 0, 255 } },
		{ 0, 1, 0, 0, 0, { 0, 0, 255, 255 } },
	};
	sp_vertex(put_vertices(v, 3), 3, 0);
	sp_tri1(0, 1, 2, 0);
	dl(0xE9000000, 0);  /* G_RDPFULLSYNC */
	run();

	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.dl_errors, 0);
	CHECK_EQ(res.triangles, 1);
	CHECK(res.flags & GFX_RES_FULLSYNC);
	CHECK_EQ(gs.geom, G_SHADE | G_SHADING_SMOOTH | G_CULL_BACK);
	const struct gfx_draw *d = draw_n(0);
	CHECK(d != NULL);
	if (!d)
		return;
	CHECK_EQ(d->h.b, 3);
	CHECK_EQ(d->cull, GFX_CULL_BACK);
	CHECK(out_vtx[0].x == 160.0f && out_vtx[0].y == 120.0f && out_vtx[0].w == 1.0f);
	CHECK(out_vtx[1].x == 320.0f && out_vtx[1].y == 120.0f);
	CHECK(out_vtx[2].x == 160.0f && out_vtx[2].y == 0.0f);
	CHECK_EQ(out_vtx[1].g, 255);  /* sombreado suave: cada uno su color */
	CHECK_EQ(out_vtx[2].b, 255);
}

/* Búfer de 16 vértices con índices ×10; el byte de flag elige el vértice
 * del sombreado plano. */
static void f3d_vertex_index_flag(void)
{
	start(false);
	basic_setup();
	sp_clear_geometry(0xFFFFFFFF);
	sp_set_geometry(F3D_SHADE);  /* plano */
	const struct vtx v[3] = {
		{ 0, 0, 0, 0, 0, { 10, 0, 0, 255 } },
		{ 1, 0, 0, 0, 0, { 20, 0, 0, 255 } },
		{ 0, 1, 0, 0, 0, { 30, 0, 0, 255 } },
	};
	sp_vertex(put_vertices(v, 3), 3, 13);  /* 13, 14, 15: el final del búfer */
	sp_tri1(13, 14, 15, 0);
	sp_tri1(13, 14, 15, 1);
	sp_tri1(13, 14, 15, 2);
	run();
	CHECK_EQ(res.triangles, 3);
	CHECK(fabsf(gs.vtx[15].y - 1.0f) < 1e-6f);
	for (int i = 0; i < 9; i++)
		CHECK_EQ(out_vtx[i].r, 10 * (i / 3 + 1));
	/* Rotar no cambia la orientación ni el lugar de los vértices. */
	CHECK(out_vtx[3].x == 320.0f && out_vtx[4].y == 0.0f && out_vtx[5].x == 160.0f);
}

static void f3d_matrix_stack(void)
{
	start(false);
	sp_viewport_320x240();
	sp_matrix(ident, MTX_PROJECTION | MTX_LOAD);
	sp_matrix(ident, MTX_LOAD);
	const float tr[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.5f, 0, 0, 1 };
	const float sc[16] = { 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1 };
	sp_matrix(tr, MTX_PUSH);
	sp_matrix(sc, MTX_PUSH);  /* M = sc · tr */
	const struct vtx v0[1] = { { 0, 0, 0, 0, 0, { 0 } } }, v1[1] = { { 1, 0, 0, 0, 0, { 0 } } };
	uint32_t a = put_vertices(v0, 1), b = put_vertices(v1, 1);
	sp_vertex(a, 1, 0);
	sp_vertex(b, 1, 1);
	dl(0xBD000000, 0);   /* G_POPMTX de la modelview */
	dl(0xBD000000, MTX_PROJECTION);  /* la proyección no tiene pila */
	sp_vertex(b, 1, 2);
	run();
	CHECK(fabsf(gs.vtx[0].x - 0.5f) < 1e-4f);
	CHECK(fabsf(gs.vtx[1].x - 2.5f) < 1e-4f);
	CHECK(fabsf(gs.vtx[2].x - 1.5f) < 1e-4f);
	CHECK_EQ(gs.mv_top, 1);
}

static uint32_t put_light(unsigned col, int dx, int dy, int dz)
{
	uint32_t a = data_alloc(16);
	uint8_t *p = t_n64.bus.rdram + a;
	p[0] = p[1] = p[2] = (uint8_t)col;
	p[4] = p[5] = p[6] = (uint8_t)col;
	p[8] = (uint8_t)dx, p[9] = (uint8_t)dy, p[10] = (uint8_t)dz;
	return a;
}

/* gSPNumLights (como dirección del fin de las luces), G_MV_L0 y L1 y el
 * color de una luz con G_MW_LIGHTCOL. */
static void f3d_lighting(void)
{
	start(false);
	sp_viewport_320x240();
	sp_matrix(ident, MTX_PROJECTION | MTX_LOAD);
	sp_matrix(ident, MTX_LOAD);
	sp_set_geometry(F3D_LIGHTING | F3D_SMOOTH);
	dl(0xBC000002, 0x80000000u + (1 + 1) * 32);  /* G_MW_NUMLIGHT: una luz */
	dl(0x03860010, put_light(200, 0, 0, 127));    /* luz 0, hacia +z */
	dl(0x03880010, put_light(20, 0, 0, 0));       /* luz 1: la ambiental */
	const struct vtx v[2] = {
		{ 0, 0, 0, 0, 0, { 0, 0, 127, 0x40 } },
		{ 0, 0, 0, 0, 0, { 0, 0, (uint8_t)-127, 0x40 } },
	};
	uint32_t va = put_vertices(v, 2);
	sp_vertex(va, 2, 0);
	dl(0xBC00000A | 0x00 << 8, 0x64646400);       /* luz 0 a 100 */
	dl(0xBC00000A | 0x04 << 8, 0x00000000);       /* su copia: no cambia nada */
	sp_vertex(va, 1, 2);
	run();
	CHECK_EQ(gs.num_lights, 1);
	CHECK_EQ(gs.vtx[0].c[0], 220);
	CHECK_EQ(gs.vtx[0].c[3], 0x40);
	CHECK_EQ(gs.vtx[1].c[1], 20);
	CHECK_EQ(gs.vtx[2].c[2], 120);
}

/* G_MV_LOOKATX/Y y G_TEXTURE (encendido en el byte bajo). */
static void f3d_texgen(void)
{
	start(false);
	sp_viewport_320x240();
	sp_matrix(ident, MTX_PROJECTION | MTX_LOAD);
	sp_matrix(ident, MTX_LOAD);
	sp_set_geometry(F3D_LIGHTING | F3D_TEXGEN);
	dl(0xBB000001 | 2 << 8, 0x07C007C0);           /* tile 2, encendida */
	dl(0x03840010, put_light(0, 127, 0, 0));      /* lookat X = +x */
	dl(0x03820010, put_light(0, 0, 127, 0));      /* lookat Y = +y */
	const struct vtx v[2] = {
		{ 0, 0, 0, 0, 0, { 127, 0, 0, 255 } },
		{ 0, 0, 0, 0, 0, { 0, 0, 127, 255 } },
	};
	sp_vertex(put_vertices(v, 2), 2, 0);
	run();
	CHECK_EQ(gs.tex_on, 1);
	CHECK_EQ(gs.tex_tile, 2);
	CHECK(fabsf(gs.vtx[0].s - 31.0f) < 0.05f);
	CHECK(fabsf(gs.vtx[0].t - 15.5f) < 0.05f);
	CHECK(fabsf(gs.vtx[1].s - 15.5f) < 0.05f);
}

/* Un G_RDPSETOTHERMODE crudo (0xEF) cambia el RDP pero no la copia del
 * microcódigo: el siguiente G_SETOTHERMODE_L parte de la copia (Earthworm Jim
 * 3D: sin esto Z_SOURCE_SEL quedaba en 1 y todo el escenario salía con una z). */
static void f3d_rdp_othermode_shadow(void)
{
	start(false);
	dl(0xEF000000u | 0x080CFFu, 0x0f0a4004u);          /* RDP crudo: Z_SOURCE_SEL en 1 */
	sp_othermode(false, 3, 29, 0x00553038u);           /* render mode */
	run();
	CHECK_EQ(gs.ol, 0x00553038u);
	CHECK_EQ(gs.ol & 4u, 0u);
}

/* G_SETOTHERMODE_* con desplazamiento y largo directos, y los bits de
 * geometry mode que se mueven respecto de F3DEX2. */
static void f3d_modes(void)
{
	start(false);
	sp_othermode(true, 20, 2, 1u << 20);               /* 2 ciclos */
	sp_othermode(true, 12, 2, 2u << 12);               /* bilineal */
	sp_othermode(false, 3, 29, 0xC8112078u);           /* render mode */
	sp_othermode(false, 0, 2, 1);                      /* alpha compare */
	sp_set_geometry(F3D_ZBUFFER | F3D_SMOOTH | F3D_CULL_FRONT | F3D_CULL_BACK);
	sp_clear_geometry(F3D_CULL_FRONT);
	run();
	CHECK_EQ(gs.oh, 1u << 20 | 2u << 12);
	CHECK_EQ(gs.ol, (0xC8112078u & ~7u) | 1u);
	CHECK_EQ(gs.geom, G_ZBUFFER | G_SHADING_SMOOTH | G_CULL_BACK);
}

/* TEXRECT: en F3D, seguido de G_RDPHALF_2 (s, t) y G_RDPHALF_CONT
 * (dsdx, dtdy), o de G_RDPHALF_1 y G_RDPHALF_2. Si falta una mitad, el
 * rectángulo no se dibuja y el comando siguiente se ejecuta. */
static void f3d_texrect(void)
{
	start(false);
	for (int i = 0; i < 8; i++)
		salsa64_wr16(t_n64.bus.rdram + TEX_PA + 2 * i, (uint16_t)((i * 4) << 11 | 1));
	dl(0xFF100000 | 319, 0x00100000);
	dl(0xFD100000, TEX_PA);
	dl(0xF5100000, 0x07000000);
	dl(0xF3000000, 0x07000000 | 7 << 12 | 2048);
	dl(0xF5100000 | 1 << 9, 0x00000000);
	dl(0xF2000000, (3 << 2) << 12 | (1 << 2));
	sp_othermode(true, 20, 2, 2u << 20);  /* copia */
	dl(0xE4000000 | (3 << 2) << 12 | (1 << 2), 0);
	dl(0xB3000000, 0);
	dl(0xB2000000, 0x10000400);
	dl(0xE4000000 | (13 << 2) << 12 | (1 << 2), (10 << 2) << 12);
	dl(0xB4000000, 0);
	dl(0xB3000000, 0x10000400);
	/* Incompleto: la mitad que falta es un G_SETGEOMETRYMODE. */
	dl(0xE4000000 | (23 << 2) << 12 | (1 << 2), (20 << 2) << 12);
	dl(0xB3000000, 0);
	sp_set_geometry(F3D_ZBUFFER);
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.rects, 2);
	CHECK_EQ(gs.geom, G_ZBUFFER);
	const struct gfx_draw *d = draw_n(0);
	CHECK(d != NULL);
	if (!d)
		return;
	CHECK_EQ(d->h.b, 12);
	CHECK(out_vtx[1].x == 4.0f && fabsf(out_vtx[1].s - 4.0f) < 1e-5f);
	CHECK(out_vtx[2].y == 2.0f && fabsf(out_vtx[2].t - 2.0f) < 1e-5f);
	CHECK(out_vtx[6].x == 10.0f && out_vtx[7].x == 14.0f);
}

/* Sprite2D de la primera generación: BASE cambia temporalmente CULLDL y
 * POPMTX por SCALEFLIP y DRAW. La subimagen CI4 y su paleta deben llegar a
 * la TMEM y producir el mismo TEXRECT que arma el microcódigo. */
static void f3d_sprite2d(void)
{
	start(false);
	dl(0xFF100000 | 319, 0x00100000);  /* color image */
	/* TEXEL0 en color y alfa. */
	dl(0xFC000000 | 1u << 20 | 31u << 15 | 7u << 12 | 7u << 9 | 1u << 5 | 31u,
	   7u << 28 | 7u << 24 | 7u << 21 | 7u << 18 | 1u << 15 | 7u << 12 | 1u << 9 | 1u << 6 | 7u << 3 | 1u);
	uint32_t desc = data_alloc(24);
	salsa64_wr32(t_n64.bus.rdram + desc, TEX_PA);
	salsa64_wr32(t_n64.bus.rdram + desc + 4, TEX_PA + 0x100);
	salsa64_wr16(t_n64.bus.rdram + desc + 8, 8);   /* ancho */
	salsa64_wr16(t_n64.bus.rdram + desc + 10, 8);  /* stride */
	salsa64_wr16(t_n64.bus.rdram + desc + 12, 2);  /* alto */
	t_n64.bus.rdram[desc + 14] = G_IM_FMT_CI;
	t_n64.bus.rdram[desc + 15] = G_IM_SIZ_4b;
	for (int i = 0; i < 8; i++)
		t_n64.bus.rdram[TEX_PA + i] = (uint8_t)(i << 4 | (i + 1));
	for (int i = 0; i < 16; i++)
		salsa64_wr16(t_n64.bus.rdram + TEX_PA + 0x100 + 2 * i, (uint16_t)((i * 2) << 1 | 1));
	dl(0x09000018, desc);               /* G_SPRITE2D_BASE */
	dl(0xBE000000, 0x04000400);         /* escala 1:1, sin voltear */
	dl(0xBD000000, 0x00100020);         /* (4, 8) píxeles */
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.dl_errors, 0);
	CHECK_EQ(res.loads, 2);
	CHECK_EQ(res.rects, 1);
	CHECK_EQ(res.draws, 1);
	const struct gfx_draw *d = draw_n(0);
	CHECK(d != NULL);
	if (!d)
		return;
	CHECK_EQ(d->tex[0].width, 8);
	CHECK_EQ(d->tex[0].height, 2);
	CHECK(out_vtx[0].x == 4.0f && out_vtx[0].y == 8.0f);
	CHECK(out_vtx[1].x == 12.0f && out_vtx[2].y == 10.0f);
}

/* Segmentos (G_MW_SEGMENT), G_DL con y sin regreso y G_CULLDL (×40). */
static void f3d_dl_flow(void)
{
	start(false);
	basic_setup();
	dl(0xBC000006 | (3 * 4) << 8, 0x00010000);  /* segmento 3 → 0x10000 */
	const struct vtx v[3] = { { 0, 0, 0, 0, 0, { 0 } }, { 1, 0, 0, 0, 0, { 0 } }, { 0, 1, 0, 0, 0, { 0 } } };
	uint32_t va = put_vertices(v, 3);
	uint32_t main_pc = dl_pc;
	dl_pc = DL_PA + 0x8000;  /* subrutina: un triángulo */
	sp_tri1(0, 1, 2, 0);
	dl(0xB8000000, 0);
	dl_pc = DL_PA + 0x9000;  /* destino del salto: otro y vuelve a quien llamó */
	sp_tri1(0, 1, 2, 0);
	dl(0xB8000000, 0);
	dl_pc = DL_PA + 0xA000;  /* llamada que salta sin regreso */
	dl(0x06010000, 0x03009000);
	sp_tri1(0, 1, 2, 0);     /* no se ejecuta */
	dl_pc = main_pc;
	sp_vertex(va, 3, 0);
	dl(0x06000000, 0x03008000);  /* llamada */
	dl(0x06000000, 0x0300A000);  /* llamada a la que salta */
	const struct vtx far[3] = { { 5, 0, 0, 0, 0, { 0 } }, { 6, 0, 0, 0, 0, { 0 } }, { 7, 1, 0, 0, 0, { 0 } } };
	sp_vertex(put_vertices(far, 3), 3, 0);
	dl(0xBE000000 | 0 * 40, (2 + 1) * 40);  /* todos fuera por la derecha */
	sp_tri1(0, 1, 2, 0);                    /* no se ejecuta */
	run();
	CHECK_EQ(res.triangles, 2);
	CHECK_EQ(res.dl_errors, 0);
	CHECK_EQ(res.unknown_cmds, 0);
}

/* G_MW_POINTS (40 bytes por vértice), gSPForceMatrix y G_MW_MATRIX. */
static void f3d_points_and_matrix(void)
{
	start(false);
	sp_viewport_320x240();
	const float sc[16] = { 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1 };
	uint32_t m = put_matrix(sc);
	dl(0x039E0010, m);       /* G_MV_MATRIX_1: las cuatro partes */
	dl(0x03980010, m + 16);
	dl(0x039A0010, m + 32);
	dl(0x039C0010, m + 48);
	const struct vtx v[2] = { { 1, 0, 0, 0, 0, { 0 } }, { 1, 1, 0, 0, 0, { 0 } } };
	uint32_t va = put_vertices(v, 2);
	sp_vertex(va, 2, 0);
	dl(0xBC00000C | (1 * 40 + 0x10) << 8, 0x11223344);  /* color del vértice 1 */
	/* Parte entera de los elementos 4 y 5 (fila 1): 0 y 3. */
	dl(0xBC000000 | 0x08 << 8, 0x00000003);
	sp_vertex(va, 2, 2);
	run();
	CHECK(fabsf(gs.vtx[0].x - 2.0f) < 1e-5f);
	CHECK_EQ(gs.vtx[1].c[0], 0x11);
	CHECK_EQ(gs.vtx[1].c[3], 0x44);
	CHECK(fabsf(gs.vtx[3].x - 2.0f) < 1e-5f);
	CHECK(fabsf(gs.vtx[3].y - 3.0f) < 1e-5f);  /* y = 1·3 */
	CHECK_EQ(res.unknown_cmds, 0);
}

/* Lo que F3D no tiene cuenta como desconocido. */
static void f3d_unknown(void)
{
	start(false);
	dl(0xB0000000, 0);   /* G_BRANCH_Z de F3DEX */
	dl(0xB5000000, 0);   /* G_LINE3D */
	dl(0x02000000, 0);   /* reservado */
	dl(0xC0000000, 0);   /* G_NOOP del RDP: nada */
	run();
	CHECK_EQ(res.unknown_cmds, 3);
	CHECK_EQ(res.last_unknown, 0x02);
}

/* G_TRI4 de Fast3D 2.0G (GoldenEye): el triángulo k con el nibble k de w0
 * y los nibbles 2k y 2k+1 de w1; los que repiten un índice no se dibujan. */
static void f3d_tri4(void)
{
	start(false);
	basic_setup();
	const float quarter[16] = { 0.25f, 0, 0, 0, 0, 0.25f, 0, 0, 0, 0, 0.25f, 0, 0, 0, 0, 1 };
	sp_matrix(quarter, MTX_LOAD);  /* la rejilla de 4×4 dentro del recorte */
	sp_set_geometry(F3D_SMOOTH | F3D_CULL_BACK);
	struct vtx v[16];
	memset(v, 0, sizeof v);
	for (int i = 0; i < 16; i++) {
		v[i].x = (int16_t)(i & 3);
		v[i].y = (int16_t)(i >> 2);
		v[i].c[0] = (uint8_t)(i * 16);
		v[i].c[3] = 255;
	}
	sp_vertex(put_vertices(v, 16), 16, 0);
	/* (0,1,4) y (5,9,8)… antihorarios con la y hacia arriba; el tercero
	 * vacío (0,0,0) y el cuarto (15,14,11) también antihorario. */
	dl(0xB1000000 | 15u << 12 | 0u << 8 | 5u << 4 | 0u, 11u << 28 | 14u << 24 | 0u << 16 | 8u << 12 | 9u << 8 | 4u << 4 | 1u);
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.triangles, 3);
	CHECK_EQ(out_vtx[0].r, 0 * 16);
	CHECK_EQ(out_vtx[1].r, 1 * 16);
	CHECK_EQ(out_vtx[2].r, 4 * 16);
	CHECK_EQ(out_vtx[3].r, 5 * 16);
	CHECK_EQ(out_vtx[5].r, 8 * 16);
	CHECK_EQ(out_vtx[6].r, 15 * 16);
	CHECK_EQ(out_vtx[8].r, 11 * 16);
}

/* Triángulo crudo del RDP (0xCC, con sombreado) armado con G_RDPHALF_1 +
 * G_RDPHALF_CONT (y G_RDPHALF_2 en la última doble palabra), como el cielo
 * de GoldenEye: dos trapecios con el color evaluado en las esquinas. */
static void f3d_rdp_raw_triangle(void)
{
	start(false);
	basic_setup();
	const uint32_t cmd[24] = {
		0xCC000000u | 120, 80u << 16 | 40,  /* YL 30, YM 20, YH 10 (s11.2) */
		50u << 16, 5u << 16,                 /* XL 50, DxLDy 5 */
		100u << 16, 0,                       /* XH 100, DxHDy 0 */
		50u << 16, 0,                        /* XM 50, DxMDy 0 */
		255u << 16 | 0, 0u << 16 | 255,      /* R G B A enteros en (XH, YH) */
		1u << 16, 0,                         /* DrDx = 1 */
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* fracciones, DxDe, DxDy */
	};
	for (int i = 0; i < 12; i++) {
		dl(0xB4000000, cmd[2 * i]);
		dl(i == 11 ? 0xB3000000 : 0xB2000000, cmd[2 * i + 1]);
	}
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.triangles, 4);
	const struct gfx_draw *d = draw_n(0);
	CHECK(d != NULL);
	if (!d)
		return;
	CHECK_EQ(d->h.b, 12);
	CHECK_EQ(d->cull, GFX_CULL_NONE);
	CHECK_EQ(d->depth, 0);
	/* De YH a YM entre H (x 100) y M (x 50); R baja 1 por píxel hacia M. */
	CHECK(out_vtx[0].x == 100.0f && out_vtx[0].y == 10.0f && out_vtx[0].w == 1.0f);
	CHECK_EQ(out_vtx[0].r, 255);
	CHECK(out_vtx[1].x == 50.0f && out_vtx[1].y == 10.0f);
	CHECK_EQ(out_vtx[1].r, 205);
	CHECK_EQ(out_vtx[1].a, 255);
	CHECK(out_vtx[2].x == 50.0f && out_vtx[2].y == 20.0f);
	CHECK(out_vtx[5].x == 100.0f && out_vtx[5].y == 20.0f);
	/* De YM a YL entre H y L, que sale de (50, 20) con pendiente 5. */
	CHECK(out_vtx[7].x == 50.0f && out_vtx[7].y == 20.0f);
	CHECK(out_vtx[8].x == 100.0f && out_vtx[8].y == 30.0f);
}

/* F3DEX 1.x: búfer de 32 con índices ×2, G_TRI2, G_QUAD, G_MODIFYVTX,
 * G_CULLDL, G_BRANCH_Z, TEXRECT con G_RDPHALF_1/2 y G_LOAD_UCODE. */
static void f3dex_commands(void)
{
	start(true);
	basic_setup();
	const struct vtx v[4] = {
		{ 0, 0, 0, 0, 0, { 1, 0, 0, 255 } },
		{ 1, 0, 0, 0, 0, { 2, 0, 0, 255 } },
		{ 0, 1, 0, 0, 0, { 3, 0, 0, 255 } },
		{ 1, 1, 0, 0, 0, { 4, 0, 0, 255 } },
	};
	uint32_t va = put_vertices(v, 4);
	sp_set_geometry(F3D_SMOOTH);             /* mismos bits que F3D */
	sp_vertex(va, 4, 28);                    /* 28..31 */
	sp_tri1(28, 29, 30, 0);
	dl(0xB1000000 | tri_w(28, 29, 30), tri_w(29, 31, 30));  /* G_TRI2 */
	dl(0xB5000000, 28u * 2 << 24 | tri_w(29, 31, 30));      /* G_QUAD */
	dl(0xB2100000 | 30 * 2, 0xAABBCCDD);    /* G_MODIFYVTX color del 30 */
	/* G_BRANCH_Z: la z de pantalla del 28 (511) <= 0x3FF: salta. */
	uint32_t main_pc = dl_pc;
	dl_pc = DL_PA + 0x8000;
	sp_tri1(28, 29, 30, 0);
	dl(0xB8000000, 0);
	dl_pc = main_pc;
	dl(0xB4000000, DL_PA + 0x8000);          /* G_RDPHALF_1: destino */
	dl(0xB0000000 | (28 * 5) << 12 | 28 * 2, 0x3FF);
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.triangles, 6);
	CHECK(fabsf(gs.vtx[31].x - 1.0f) < 1e-6f);
	CHECK_EQ(gs.vtx[30].c[0], 0xAA);
	CHECK_EQ(out_vtx[17].r, 0xAA);           /* el último triángulo: después del cambio */

	start(true);
	basic_setup();
	sp_vertex(va, 4, 0);
	dl(0xBE000000 | 0 * 2, 3 * 2);           /* G_CULLDL 0..3: se ven, sigue */
	sp_tri1(0, 1, 2, 0);
	sp_othermode(true, 20, 2, 2u << 20);
	dl(0xE4000000 | (3 << 2) << 12 | (1 << 2), 0);
	dl(0xB4000000, 0);
	dl(0xB3000000, 0x10000400);
	/* La secuencia anterior (G_RDPHALF_2 + G_RDPHALF_CONT), que manda
	 * F3DEX 0.95 en Mario Kart 64: también es un rectángulo. */
	dl(0xE4000000 | (13 << 2) << 12 | (1 << 2), (10 << 2) << 12);
	dl(0xB3000000, 0);
	dl(0xB2000000, 0x10000400);
	dl(0xAF000000, 0);                       /* G_LOAD_UCODE: termina */
	sp_tri1(0, 1, 2, 0);
	run();
	CHECK_EQ(res.triangles, 1);
	CHECK_EQ(res.rects, 2);
	CHECK(res.flags & GFX_RES_LOAD_UCODE);
}

/* El RSP recorta los triángulos contra el viewport por la clip ratio: aquí
 * van en las tijeras del dibujo (no en las de los rectángulos). La demo de
 * Mario Kart 64 abre con un viewport chico, clip ratio 1 y tijeras hasta
 * (0, 0). La primera generación arranca con ratio 1. */
static void f3dex_clip_ratio(void)
{
	start(true);
	basic_setup();
	sp_viewport(40, 30, 160, 120);  /* ventana x 120–200, y 90–150 */
	const struct vtx v[3] = {
		{ -4, -4, 0, 0, 0, { 255, 255, 255, 255 } },
		{ 4, -4, 0, 0, 0, { 255, 255, 255, 255 } },
		{ 0, 4, 0, 0, 0, { 255, 255, 255, 255 } },
	};
	sp_vertex(put_vertices(v, 3), 3, 0);
	sp_tri1(0, 1, 2, 0);                     /* ratio de los datos: 1 */
	sp_clip_ratio(2);
	sp_tri1(0, 1, 2, 0);
	sp_viewport(80, 60, 160, 120);           /* otro viewport: otro dibujo */
	sp_tri1(0, 1, 2, 0);
	dl(0xF6000000 | (100 << 2) << 12 | (100 << 2), (10 << 2) << 12 | (10 << 2));  /* G_FILLRECT */
	run();
	CHECK_EQ(res.triangles, 3);
	const struct gfx_draw *d0 = draw_n(0), *d1 = draw_n(1), *d2 = draw_n(2), *d3 = draw_n(3);
	CHECK(d0 && d1 && d2 && d3);
	if (!d0 || !d1 || !d2 || !d3)
		return;
	CHECK(d0->scissor[0] == 120 * 4 && d0->scissor[1] == 90 * 4 && d0->scissor[2] == 200 * 4 &&
	      d0->scissor[3] == 150 * 4);
	CHECK(d1->scissor[0] == 80 * 4 && d1->scissor[1] == 60 * 4 && d1->scissor[2] == 240 * 4 &&
	      d1->scissor[3] == 180 * 4);
	/* ±160 × 120 por 2: más que la pantalla, mandan las tijeras del RDP. */
	CHECK(d2->scissor[0] == 0 && d2->scissor[1] == 0 && d2->scissor[2] == 320 * 4 && d2->scissor[3] == 240 * 4);
	/* El rectángulo, siempre con las tijeras del RDP. */
	CHECK(d3->scissor[0] == 0 && d3->scissor[1] == 0 && d3->scissor[2] == 320 * 4 && d3->scissor[3] == 240 * 4);
}

/* Plano cercano (z = −w): con matrices identidad w = 1 y queda detrás lo
 * que tiene z < −1. Un vértice detrás: el triángulo sale como dos; dos: uno
 * más chico; los tres: nada. Con un microcódigo .NoN no se recorta. */
static void f3d_near_clip(void)
{
	start(false);
	basic_setup();
	sp_clear_geometry(0xFFFFFFFF);
	sp_set_geometry(F3D_SHADE | F3D_SMOOTH);
	const struct vtx v[5] = {
		{ 0, 0, 1, 0, 0, { 0, 0, 0, 255 } },
		{ 1, 0, 1, 0, 0, { 200, 0, 0, 255 } },
		{ 0, 1, -3, 0, 0, { 0, 100, 0, 255 } },
		{ 1, 1, -3, 0, 0, { 0, 0, 0, 255 } },
		{ 0, -1, -5, 0, 0, { 0, 0, 0, 255 } },
	};
	sp_vertex(put_vertices(v, 5), 5, 0);
	sp_tri1(0, 1, 2, 0);
	sp_tri1(0, 2, 3, 0);
	sp_tri1(2, 3, 4, 0);
	run();
	CHECK_EQ(res.triangles, 2);
	const struct gfx_draw *d = draw_n(0);
	CHECK(d && d->h.b == 9);
	/* v0 (160,120), v1 (320,120), corte v1–v2 en (240,60), corte v2–v0
	 * en (160,60). El recorte recorre v0, v1, v2 con el anterior, como el
	 * RSP (rdp.c): el corte v2–v0 sale antes que v0 y el abanico parte de
	 * él: [I20, v0, v1, I12]. */
	static const float want[6][2] = { { 160, 60 }, { 160, 120 }, { 320, 120 }, { 160, 60 }, { 320, 120 }, { 240, 60 } };
	for (int i = 0; i < 6; i++) {
		const struct gfx_vertex *o = &out_vtx[i];
		CHECK(fabsf(o->x / o->w - want[i][0]) < 1e-3f && fabsf(o->y / o->w - want[i][1]) < 1e-3f);
		CHECK(o->z >= -1e-4f);
	}
	CHECK_EQ(out_vtx[5].r, 100);  /* mitad de 200 y 0 */
	CHECK_EQ(out_vtx[5].g, 50);
	CHECK(fabsf(out_vtx[5].z) < 1e-4f);  /* el corte queda en el plano cercano */
	/* Dos detrás (v2, v3): un triángulo [I30, v0, I02]. */
	CHECK(fabsf(out_vtx[6].x / out_vtx[6].w - 240) < 1e-3f && fabsf(out_vtx[6].y / out_vtx[6].w - 60) < 1e-3f);
	CHECK(fabsf(out_vtx[7].x / out_vtx[7].w - 160) < 1e-3f && fabsf(out_vtx[7].y / out_vtx[7].w - 120) < 1e-3f);
	CHECK(fabsf(out_vtx[8].x / out_vtx[8].w - 160) < 1e-3f && fabsf(out_vtx[8].y / out_vtx[8].w - 60) < 1e-3f);

	start(false);
	job_flags = GFX_JOB_NONEAR;
	basic_setup();
	sp_vertex(put_vertices(v, 5), 5, 0);
	sp_tri1(0, 1, 2, 0);
	sp_tri1(2, 3, 4, 0);
	run();
	CHECK_EQ(res.triangles, 2);
	d = draw_n(0);
	CHECK(d && d->h.b == 6);
	CHECK(fabsf(out_vtx[2].x / out_vtx[2].w - 160) < 1e-3f && fabsf(out_vtx[2].y / out_vtx[2].w) < 1e-3f);
}

/* Variante de Wave Race: G_VTX con v0·5 en el byte 2 y el conteo en 7 bits,
 * índices ×5 y G_QUAD (0xB5) con cuatro índices. */
static void f3dwr_commands(void)
{
	/* x/5 por multiplicación y desplazamiento, exacto en 0–255. */
	for (uint32_t b = 0; b < 256; b++)
		CHECK_EQ((b * 205u) >> 10, b / 5);

	start(false);
	wr = true;
	basic_setup();
	const float quarter[16] = { 0.25f, 0, 0, 0, 0, 0.25f, 0, 0, 0, 0, 0.25f, 0, 0, 0, 0, 1 };
	sp_matrix(quarter, MTX_LOAD);
	sp_clear_geometry(0xFFFFFFFF);
	sp_set_geometry(F3D_SMOOTH);
	struct vtx v[32];
	memset(v, 0, sizeof v);
	for (int i = 0; i < 32; i++) {
		v[i].x = (int16_t)(i & 3);
		v[i].y = (int16_t)((i >> 2) & 3);
		v[i].c[0] = (uint8_t)(i * 8);
		v[i].c[3] = 255;
	}
	uint32_t a = put_vertices(v, 32);
	dl(0x04000a4f, a);                /* 5 vértices desde el 0 */
	dl(0x04501cdf, a + 16 * 16);      /* 14 vértices desde el 16 */
	dl(0xBF000000, 0u << 16 | 5u << 8 | 10u);                 /* triángulo 0, 1, 2 */
	dl(0xB5000000, 16u * 5 << 24 | 17u * 5 << 16 | 18u * 5 << 8 | 19u * 5);  /* quad 16–19 */
	dl(0xB5000000, 29u * 5 << 24 | 28u * 5 << 16 | 27u * 5 << 8 | 26u * 5);  /* el último llega a 29 */
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.triangles, 5);
	/* Los vértices cargados en 0–4 y 16–29. */
	CHECK(fabsf(gs.vtx[4].x - 0.0f) < 1e-6f);
	CHECK(fabsf(gs.vtx[29].x - 0.25f) < 1e-6f);
	CHECK(fabsf(gs.vtx[29].y - 0.75f) < 1e-6f);
	/* Triángulo (0,1,2) y los dos del primer quad (16,17,18) y (16,18,19). */
	CHECK_EQ(out_vtx[0].r, 0 * 8);
	CHECK_EQ(out_vtx[1].r, 1 * 8);
	CHECK_EQ(out_vtx[2].r, 2 * 8);
	CHECK_EQ(out_vtx[3].r, 16 * 8);
	CHECK_EQ(out_vtx[4].r, 17 * 8);
	CHECK_EQ(out_vtx[5].r, 18 * 8);
	CHECK_EQ(out_vtx[6].r, 16 * 8);
	CHECK_EQ(out_vtx[7].r, 18 * 8);
	CHECK_EQ(out_vtx[8].r, 19 * 8);
	CHECK_EQ(out_vtx[9].r, 29 * 8);
	CHECK_EQ(out_vtx[11].r, 27 * 8);
	CHECK_EQ(out_vtx[14].r, 26 * 8);

	/* Sin la variante, 0xB5 es G_LINE3D de F3D: no se dibuja nada. */
	start(false);
	dl(0xB5000000, 0);
	run();
	CHECK_EQ(res.triangles, 0);
}

void t_suite_gfx_f3d(void)
{
	RUN(f3d_triangle);
	RUN(f3d_vertex_index_flag);
	RUN(f3d_matrix_stack);
	RUN(f3d_lighting);
	RUN(f3d_texgen);
	RUN(f3d_modes);
	RUN(f3d_rdp_othermode_shadow);
	RUN(f3d_texrect);
	RUN(f3d_sprite2d);
	RUN(f3d_dl_flow);
	RUN(f3d_points_and_matrix);
	RUN(f3d_unknown);
	RUN(f3d_tri4);
	RUN(f3dwr_commands);
	RUN(f3d_rdp_raw_triangle);
	RUN(f3dex_commands);
	RUN(f3dex_clip_ratio);
	RUN(f3d_near_clip);
}
