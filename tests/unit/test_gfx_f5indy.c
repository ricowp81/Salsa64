/*
 * Salsa64 — pruebas de la HLE del microcódigo gráfico de Indiana Jones and the
 * Infernal Machine y Battle for Naboo (src/rsp/gfx/f5indy.c, de Factor 5): una
 * DMEM de 4 KB que se llena por DMA con G_MOVEMEM, listas con cabecera y
 * saltos, triángulos con las direcciones de sus vértices en la DMEM y los
 * colores por índice de la tabla de 0xD40, la iluminación sobre 0x480, la
 * lista secundaria, el othermode con la máscara contada desde arriba y las
 * partículas (los rectángulos de textura de 0x24 y la escritura a la RDRAM de
 * 0x27). Display lists sintéticas con la codificación que se midió contra el
 * microcódigo real (salsa64-run --lle=all, docs/estado.md); nada viene del
 * juego: vértices, colores y luces son inventados.
 */
#include "test.h"

#include <math.h>
#include <string.h>

#include "core/endian.h"
#include "rsp/gfx/hle.h"

#define DL_PA   0x10000u
#define DATA_PA 0x20000u

static struct gfx_state gs;
static struct gfx_vertex out_vtx[1024] __attribute__((aligned(16)));
static uint8_t out_cmd[16 * 1024] __attribute__((aligned(16)));
static uint8_t out_tex[64 * 1024] __attribute__((aligned(128)));
static struct gfx_job job;
static struct gfx_result res;
static uint32_t dl_pc, data_pc;

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
	gfx_init(&gs);
	/* La palabra de salto de software con que empieza cada lista. */
	dl(0x00000000, 0);
}

static void run(void)
{
	dl(0xB8000000, 0);
	memset(&job, 0, sizeof job);
	job.rdram_ea = (uint64_t)(uintptr_t)t_n64.bus.rdram;
	job.rdram_size = t_n64.bus.rdram_size;
	job.vtx_ea = (uint64_t)(uintptr_t)out_vtx;
	job.vtx_cap = sizeof out_vtx;
	job.cmd_ea = (uint64_t)(uintptr_t)out_cmd;
	job.cmd_cap = sizeof out_cmd;
	job.tex_ea = (uint64_t)(uintptr_t)out_tex;
	job.tex_cap = sizeof out_tex;
	job.gbi = SALSA64_GBI_F5INDY;
	job.dl = DL_PA;
	gfx_run(&gs, &job, &res);
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

/* La proyección que lleva [−128, 128] a [−1, 1] (y w = 1). */
static const float proj[16] = { 1.0f / 128, 0, 0, 0, 0, 1.0f / 128, 0, 0, 0, 0, 1.0f / 128, 0, 0, 0, 0, 1 };

/* Lo que arma el juego en cada cuadro: viewport de 320 × 240 y la matriz combinada, por DMA a
 * 0x130 y 0x5C0, y la geometría con sombreado. */
static void setup(void)
{
	uint32_t vp = data_alloc(16);
	static const int16_t v[8] = { 640, 480, 511, 0, 640, 480, 511, 0 };
	for (int i = 0; i < 8; i++)
		salsa64_wr16(t_n64.bus.rdram + vp + 2 * i, (uint16_t)v[i]);
	dl(0x0101300F, vp);
	dl(0x0105C03F, put_matrix(proj));
	dl(0xB7000000, 0x00000005);  /* G_ZBUFFER y G_SHADE (los bits de F3D) */
	dl(0xFF100000 | 319, 0x00100000);
	/* Combiner SHADE en los dos ciclos. */
	dl(0xFC000000 | (15u << 20 | 31u << 15 | 7u << 12 | 7u << 9 | 15u << 5 | 31u),
	   7u << 28 | 7u << 24 | 7u << 21 | 7u << 18 | 4u << 15 | 7u << 12 | 4u << 9 | 4u << 6 | 7u << 3 | 4u);
}

/* n vértices de 8 bytes (x, y, z, bandera) a la DMEM 0x170 y de ahí a los lugares 0… (el
 * vértice i va en la dirección 0x600 + 40 · i). */
static void load_vertices(const int16_t (*v)[3], unsigned n)
{
	uint32_t a = data_alloc(8 * n);
	for (unsigned i = 0; i < n; i++) {
		uint8_t *p = t_n64.bus.rdram + a + 8 * i;
		for (int k = 0; k < 3; k++)
			salsa64_wr16(p + 2 * k, (uint16_t)v[i][k]);
		salsa64_wr16(p + 6, 0x1234);
	}
	dl(0x01117000u | (8 * n - 1), n << 24 | a);
}

/* La tabla de colores de los triángulos: n palabras RGBA a la DMEM 0xD40. */
static uint32_t put_colors(unsigned n)
{
	uint32_t a = data_alloc(4 * n);
	for (unsigned i = 0; i < n; i++)
		t_wr32(a + 4 * i, 0x10203000u + 0x01010100u * i + 0xF0u + i);
	dl(0x010D4000u | (4 * n - 1), a);
	return a;
}

/* G_TRI1 sin textura sobre los lugares v0, v1 y v2, con los colores de las entradas c0, c1 y c2. */
static void tri1(unsigned v0, unsigned v1, unsigned v2, unsigned c0, unsigned c1, unsigned c2)
{
	dl(0xBF000000, (0x600 + 40 * v0) << 16 | (0x600 + 40 * v1));
	dl(c0 * 4 << 16 | c1 * 4 << 8 | c2 * 4, (0x600 + 40 * v2) << 16);
}

/* ---- Listas ---- */

/* La cabecera de cada lista dice a dónde salta el 0xB5; G_DL y G_BRANCHDL la saltan (el
 * 0xB5 salta a ese destino y salta también su cabecera). */
static void f5i_lists(void)
{
	start();
	uint32_t l2 = data_alloc(32), l1 = data_alloc(32);
	t_wr32(l1, l2), t_wr32(l1 + 4, 0);
	t_wr32(l1 + 8, 0xB7000000), t_wr32(l1 + 12, 0x00010000);  /* G_FOG */
	t_wr32(l1 + 16, 0xB5000000), t_wr32(l1 + 20, 0);
	t_wr32(l1 + 24, 0xB7000000), t_wr32(l1 + 28, 0x00000020);  /* no se ejecuta */
	t_wr32(l2, 0x00ABCDEF), t_wr32(l2 + 4, 0);
	t_wr32(l2 + 8, 0xB7000000), t_wr32(l2 + 12, 0x00020000);   /* G_LIGHTING */
	t_wr32(l2 + 16, 0xB8000000), t_wr32(l2 + 20, 0);
	dl(0x06000000, l1);
	dl(0xB7000000, 0x00040000);  /* G_TEXTURE_GEN en la principal */
	dl(0xE9000000, 0);
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.dl_errors, 0);
	CHECK(gs.geom & G_FOG);
	CHECK(gs.geom & G_LIGHTING);
	CHECK(gs.geom & G_TEXTURE_GEN);
	CHECK(!(gs.geom & G_ZBUFFER));
	CHECK(res.flags & GFX_RES_FULLSYNC);
	CHECK_EQ(gs.dl_sp, 0);

	/* G_BRANCHDL no vuelve, y G_CLEARGEOMETRYMODE deja lo que dice w1 (lo demás se borra). */
	start();
	uint32_t b = data_alloc(32);
	t_wr32(b, 0), t_wr32(b + 4, 0);
	t_wr32(b + 8, 0xB7000000), t_wr32(b + 12, 0x00030000);
	t_wr32(b + 16, 0xB6000000), t_wr32(b + 20, 0xFFFEFFFFu);  /* conserva todo menos G_FOG */
	t_wr32(b + 24, 0xB8000000), t_wr32(b + 28, 0);
	dl(0x07000000, b);
	dl(0xB7000000, 0x00000020);  /* no se ejecuta */
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK(!(gs.geom & G_FOG));
	CHECK(gs.geom & G_LIGHTING);
}

/* G_MOVEWORD escribe w1 en la DMEM y, en 0x04 y 0x0C, el recorte; la niebla (0x160 y 0x164)
 * en 16.16. */
static void f5i_moveword(void)
{
	start();
	dl(0xBC000160, 0x00280000);  /* multiplicador: 40 en 16.16 */
	dl(0xBC000164, 0xFFFF0000u);  /* desplazamiento: -1 */
	dl(0xBC000004, 2);
	dl(0xBC00000C, 3);
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK(fabsf(gs.fog_mul - 40.0f * 256.0f) < 1e-3f);
	CHECK(fabsf(gs.fog_off + 256.0f) < 1e-3f);
	CHECK(fabsf(gs.clip_ratio[0] - 2.0f) < 1e-6f && fabsf(gs.clip_ratio[1] - 3.0f) < 1e-6f);
}

/* G_SETOTHERMODE: la máscara se cuenta desde el bit de arriba (len − 1 en los 5 bits bajos y el
 * corrimiento en los 5 bits de 8 a 12) y el bit 18 de w0 elige L en vez de H. */
static void f5i_othermode(void)
{
	start();
	/* H: el ciclo (bits 21 y 20) = 1: la máscara de 2 bits va corrida 31 − 21 = 10 desde arriba. */
	dl(0xBA000000 | 10u << 8 | 1u, 1u << 20);
	/* L, con el bit 18 de w0 (A0 = 4): bits 15 y 14 (máscara 0xC000). */
	dl(0xBA040000 | 16u << 8 | 1u, 0x00004000);
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ((gs.oh >> 20) & 3, 1u);
	CHECK_EQ(gs.ol & 0xC000u, 0x4000u);

	/* El 0xB9 solo vale si el bit 23 de w0 coincide con la palabra de 0x11C (cero al empezar). */
	start();
	dl(0xB9000000 | 16u << 8 | 1u | 0x040000, 0x00008000);  /* bit 23 = 0: se aplica */
	dl(0xB9800000 | 14u << 8 | 1u | 0x040000, 0x00000003);  /* bit 23 = 1: no */
	run();
	CHECK_EQ(gs.ol & 0xC000u, 0x8000u);
	CHECK_EQ(gs.ol & 3u, 0u);
}

/* ---- Vértices y triángulos ---- */

/* Los vértices de la DMEM se transforman con la matriz combinada y la y de pantalla va al
 * revés que en Fast3D. */
static void f5i_vertices(void)
{
	start();
	setup();
	const int16_t v[3][3] = { { 0, 0, 0 }, { 64, 0, 0 }, { 0, 64, 0 } };
	load_vertices(v, 3);
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.dl_errors, 0);
	CHECK(fabsf(gs.vscale[0] - 160.0f) < 1e-3f && fabsf(gs.vtrans[1] - 120.0f) < 1e-3f);
	CHECK(fabsf(gs.vtx[1].sx - 240.0f) < 1e-3f);
	CHECK(fabsf(gs.vtx[1].sy - 120.0f) < 1e-3f);
	CHECK(fabsf(gs.vtx[2].sx - 160.0f) < 1e-3f);
	CHECK(fabsf(gs.vtx[2].sy - 180.0f) < 1e-3f);
	CHECK(fabsf(gs.vtx[2].y - (-0.5f)) < 1e-6f);
	CHECK_EQ(gs.vtx[2].clip, 0u);
}

/* G_TRI1 y G_TRI2: los colores de la tabla de 0xD40 por desplazamiento, los vértices por su
 * dirección; sombreado suave y sin culling (un triángulo sin textura no lo tiene). */
static void f5i_tri_colors(void)
{
	start();
	setup();
	const int16_t v[4][3] = { { -64, -64, 0 }, { 64, -64, 0 }, { 64, 64, 0 }, { -64, 64, 0 } };
	load_vertices(v, 4);
	uint32_t tab = put_colors(8);
	dl(0xB7000000, 0x00002000);  /* culling de atrás: los sin textura no lo tienen */
	tri1(0, 1, 2, 0, 3, 5);
	/* G_TRI2: (v0, v1, v2) y (v0, v2, v3); los colores de las cuatro entradas. */
	dl(0xB4000000, (0x600 + 40 * 0) << 16 | (0x600 + 40 * 1));
	dl(1u * 4 << 16 | 2u * 4 << 8 | 4u * 4 | 6u * 4 << 24, (0x600 + 40 * 2) << 16 | (0x600 + 40 * 3));
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.dl_errors, 0);
	CHECK_EQ(res.triangles, 3);
	const unsigned ci[3] = { 0, 3, 5 };
	for (int i = 0; i < 3; i++) {
		uint32_t c = t_rd32(tab + 4 * ci[i]);
		CHECK_EQ(out_vtx[i].r, c >> 24);
		CHECK_EQ(out_vtx[i].g, (c >> 16) & 0xFF);
		CHECK_EQ(out_vtx[i].b, (c >> 8) & 0xFF);
		CHECK_EQ(out_vtx[i].a, c & 0xFF);
	}
	CHECK(gs.geom & G_CULL_BACK);
}

/* Las coordenadas de textura (bit 9 de w0): una palabra (s, t) por vértice, en 1/32 de texel con
 * y sin perspectiva (medido contra el RSP real, no 1/16 como GLideN64). */
static void f5i_tri_tex(void)
{
	for (int persp = 0; persp < 2; persp++) {
		start();
		setup();
		const int16_t v[3][3] = { { -64, -64, 0 }, { 64, -64, 0 }, { 0, 64, 0 } };
		load_vertices(v, 3);
		put_colors(4);
		dl(0xBA000000 | 12u << 8 | 0u, persp ? 1u << 19 : 0u);
		dl(0xBF000200, (0x600 + 40 * 0) << 16 | (0x600 + 40 * 1));
		dl(0u << 16 | 4u << 8 | 8u, (0x600 + 40 * 2) << 16);
		dl(0x10000000, 0x00001000);  /* v0: (0x1000, 0); v1: (0, 0x1000) */
		dl(0x08000800, 0);           /* v2 */
		run();
		CHECK_EQ(res.unknown_cmds, 0);
		CHECK_EQ(res.triangles, 1);
		CHECK(fabsf(gs.vtx[0].s - 4096.0f / 32.0f) < 1e-3f && fabsf(gs.vtx[0].t) < 1e-3f);
		CHECK(fabsf(gs.vtx[1].s) < 1e-3f && fabsf(gs.vtx[1].t - 4096.0f / 32.0f) < 1e-3f);
		CHECK(fabsf(gs.vtx[2].s - 2048.0f / 32.0f) < 1e-3f && fabsf(gs.vtx[2].t - 2048.0f / 32.0f) < 1e-3f);
	}
}

/* ---- DMA segmentada, iluminación y lista secundaria ---- */

/* G_SET_DLIST_ADDR arma un flujo de bloques encadenados (FD4: de dónde se lee, FD8: el enlace al que
 * sigue y FDC: lo que queda del bloque); un G_MOVEMEM sin dirección en w1 toma de él y pasa al
 * bloque siguiente cuando el actual se acaba. */
static void f5i_dma_segmented(void)
{
	start();
	setup();
	/* Dos bloques; el primero (de 0x20 bytes de datos) trae el enlace al segundo en su primera palabra. */
	uint32_t b1 = data_alloc(0x40), b2 = data_alloc(0x40);
	t_wr32(b1, b2);
	t_wr32(b2, 0);
	for (unsigned i = 0; i < 8; i++) {
		t_wr32(b1 + 8 + 4 * i, 0xA0000000u + i);
		t_wr32(b2 + 8 + 4 * i, 0xB0000000u + i);
	}
	dl(0x02000020, b1);  /* el bloque tiene 0x20 bytes de datos */
	const int16_t v[3][3] = { { -64, -64, 0 }, { 64, -64, 0 }, { 0, 64, 0 } };
	load_vertices(v, 3);
	/* 0x40 bytes a 0xD40: los 0x20 del primero y los 0x20 primeros del segundo. */
	dl(0x010D403F, 0);
	tri1(0, 1, 2, 7, 8, 15);
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.triangles, 1);
	CHECK_EQ(out_vtx[0].r, 0xA0u);
	CHECK_EQ(out_vtx[0].a, 7u);
	CHECK_EQ(out_vtx[1].r, 0xB0u);
	CHECK_EQ(out_vtx[1].a, 0u);
	CHECK_EQ(out_vtx[2].r, 0xB0u);
	CHECK_EQ(out_vtx[2].a, 7u);
}

/* La iluminación básica (el cuenta de vértices o las luces con el bit 7): dato · factor >> 8, factor de 0x158. */
static void f5i_light_basic(void)
{
	start();
	setup();
	const int16_t v[3][3] = { { -64, -64, 0 }, { 64, -64, 0 }, { 0, 64, 0 } };
	load_vertices(v, 3);
	uint32_t data = data_alloc(32);
	static const uint8_t col[16] = { 0xFF, 0x80, 0x40, 0x10, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x20, 0x40, 0x60, 0x7F, 0x7E, 0x7D, 0x7C };
	memcpy(t_n64.bus.rdram + data, col, 16);
	dl(0xBC000158, 0x80C0FFFFu);
	dl(0xBC00015C, 0x80808080u);
	/* Basic: 4 vértices de 4 bytes, con el 0x80 de las cuentas. */
	dl(0x01248000u | 0x0F, 0x84000000u | data);
	tri1(0, 1, 2, 0, 1, 2);
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	/* Los bytes de la tabla en el orden del microcódigo: R, G, B, A con el factor de cada uno (A, B, G, R de 0x158: 0xFF, 0xFF, 0xC0, 0x80). */
	uint32_t c0 = t_rd32(data);
	(void)c0;
	CHECK_EQ(res.triangles, 1);
	/* Rojo con factor 0x80 (el byte alto de la palabra de factores): 0xFF · 0x80 >> 8 = 0x7F. */
	CHECK_EQ(out_vtx[0].r, 0x7Fu);
	/* Verde con 0xC0: 0x80 · 0xC0 >> 8 = 0x60. */
	CHECK_EQ(out_vtx[0].g, 0x60u);
	/* Azul con 0xFF: 0x40 · 0xFF >> 8 = 0x3F; alfa con 0xFF: 0x10 · 0xFF >> 8 = 0x0F. */
	CHECK_EQ(out_vtx[0].b, 0x3Fu);
	CHECK_EQ(out_vtx[0].a, 0x0Fu);
}

/* La lista secundaria: la dirección de 0x58C; antes de dibujar un triángulo corre sus comandos
 * hasta B8000000 FFFFFFFF, y se borra. */
static void f5i_sublist(void)
{
	start();
	setup();
	const int16_t v[3][3] = { { -64, -64, 0 }, { 64, -64, 0 }, { 0, 64, 0 } };
	load_vertices(v, 3);
	put_colors(4);
	uint32_t sub = data_alloc(48);
	t_wr32(sub, 0), t_wr32(sub + 4, 0);                        /* cabecera */
	t_wr32(sub + 8, 0xFA000000), t_wr32(sub + 12, 0x11223344);  /* G_SETPRIMCOLOR */
	t_wr32(sub + 16, 0xB8000000), t_wr32(sub + 20, 0xFFFFFFFFu);
	dl(0xBC00058C, sub);
	tri1(0, 1, 2, 0, 1, 2);
	dl(0xFA000000, 0x55555555);  /* el RDP sigue: sin sublista el color no cambia otra vez */
	tri1(0, 1, 2, 0, 1, 2);
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.triangles, 2);
	CHECK_EQ(gs.prim, 0x55555555u);  /* la sublista corrió una vez y el comando que sigue ganó */
}

/* Una lista secundaria que acaba en un G_ENDDL con la palabra baja en 0 (Indiana Jones deja
 * así algunas en 0x58C): termina ahí, como en el microcódigo real (salsa64-run --lle=all con la
 * lista de un material cambiada así: el G_SETCIMG de detrás no llega al RDP; dentro de la lista,
 * sí). Detrás van los vértices de 8 bytes del juego: (−80, 17, 136, 0) es FFB00011 00880000, un
 * G_SETCIMG de 18 de ancho en 0x880000 (el de la PS3 real). Con GFX_JOB_F5_SUB_OLD, como antes:
 * se sigue hasta el B8 … FFFFFFFF y el vértice se ejecuta. */
static void f5i_sublist_enddl(bool old)
{
	start();
	setup();
	const int16_t v[3][3] = { { -64, -64, 0 }, { 64, -64, 0 }, { 0, 64, 0 } };
	load_vertices(v, 3);
	put_colors(4);
	uint32_t sub = data_alloc(48);
	t_wr32(sub, 0), t_wr32(sub + 4, 0);
	t_wr32(sub + 8, 0xFA000000), t_wr32(sub + 12, 0x11223344);
	t_wr32(sub + 16, 0xB8000000), t_wr32(sub + 20, 0);
	t_wr32(sub + 24, 0xFFB00011), t_wr32(sub + 28, 0x00880000);  /* un vértice, no un comando */
	t_wr32(sub + 32, 0xB8000000), t_wr32(sub + 36, 0xFFFFFFFFu);
	dl(0xBC00058C, sub);
	tri1(0, 1, 2, 0, 1, 2);
	dl(0xB8000000, 0);
	memset(&job, 0, sizeof job);
	job.rdram_ea = (uint64_t)(uintptr_t)t_n64.bus.rdram;
	job.rdram_size = t_n64.bus.rdram_size;
	job.vtx_ea = (uint64_t)(uintptr_t)out_vtx;
	job.vtx_cap = sizeof out_vtx;
	job.cmd_ea = (uint64_t)(uintptr_t)out_cmd;
	job.cmd_cap = sizeof out_cmd;
	job.tex_ea = (uint64_t)(uintptr_t)out_tex;
	job.tex_cap = sizeof out_tex;
	job.gbi = SALSA64_GBI_F5INDY;
	job.dl = DL_PA;
	job.flags = old ? GFX_JOB_F5_SUB_OLD : 0;
	gfx_run(&gs, &job, &res);
	CHECK_EQ(res.triangles, 1);
	CHECK_EQ(gs.prim, 0x11223344u);  /* lo de antes del G_ENDDL corrió */
	CHECK((res.flags & GFX_RES_F5_SUB_END) != 0);
	if (old) {
		CHECK_EQ(gs.cimg_addr, 0x880000u);
		CHECK_EQ(gs.cimg_width, 18);
	} else {
		CHECK_EQ(gs.cimg_addr, 0x100000u);  /* el de setup */
		CHECK_EQ(gs.cimg_width, 320);
	}
}

static void f5i_sublist_enddl_new(void)
{
	f5i_sublist_enddl(false);
}

static void f5i_sublist_enddl_old(void)
{
	f5i_sublist_enddl(true);
}

/* Un rectángulo de textura (E4) de 16 bytes: las palabras de s, t y dsdx, dtdy siguen sin opcode. */
static void f5i_texrect(void)
{
	start();
	setup();
	dl(0xBA000000 | 10u << 8 | 1u, 0);  /* un ciclo */
	dl(0xE4000000 | (200u << 2) << 12 | (150u << 2), (100u << 2) << 12 | (50u << 2));
	dl(0, 0x04000400u);
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	CHECK_EQ(res.rects, 1);
	CHECK(res.vtx_bytes >= 6 * sizeof(struct gfx_vertex));
	CHECK(fabsf(out_vtx[0].x / out_vtx[0].w - 100.0f) < 1e-3f);
	CHECK(fabsf(out_vtx[0].y / out_vtx[0].w - 50.0f) < 1e-3f);
}

/* G_GEOMETRY_GEN 0x18: vértices de la DMEM con la matriz de 0xE40 en lugar de la combinada. */
static void f5i_gen_vertices(void)
{
	start();
	setup();
	const float gen[16] = { 1.0f / 64, 0, 0, 0, 0, 1.0f / 64, 0, 0, 0, 0, 1.0f / 64, 0, 0, 0, 0, 1 };
	dl(0x010E403F, put_matrix(gen));
	const int16_t v[2][3] = { { 0, 0, 0 }, { 32, 0, 0 } };
	load_vertices(v, 2);  /* DMEM 0x170: los dos; la combinada los lleva a (160, 120) y (200, 120) */
	dl(0x05000003, 0x00000018);  /* los lugares 0 y 1 con la matriz de 0xE40 */
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	/* x = 32 / 64 = 0,5 → 160 + 0,5 · 160 = 240 (con la combinada serían 200). */
	CHECK(fabsf(gs.vtx[1].sx - 240.0f) < 1e-3f);
	CHECK(fabsf(gs.vtx[0].sx - 160.0f) < 1e-3f);
}

/* Las partículas: 0x27 mueve los vértices de la RDRAM (cada uno suma su paso por el factor) y los deja
 * en ella. */
static void f5i_particles(void)
{
	start();
	setup();
	uint32_t verts = data_alloc(256), mask = data_alloc(16);
	memset(t_n64.bus.rdram + verts, 0, 256);
	for (unsigned i = 0; i < 4; i++) {
		uint8_t *p = t_n64.bus.rdram + verts + 8 * i;
		salsa64_wr16(p, (uint16_t)(10 * i));
		salsa64_wr16(p + 2, (uint16_t)(20 * i));
		salsa64_wr16(p + 4, 5);
	}
	t_wr32(mask, 0x0000000F);  /* cuatro vivas */
	t_wr32(mask + 4, 0);
	/* Factor 0x1000 (4096 = 1,0 con 12 bits de fracción), pasos (1, 2, 3) y M = 4 (no guarda la máscara). */
	dl(0x05001000, 0x00000027);
	dl(verts, mask);
	dl(0x00010002, 0x00030004);
	run();
	CHECK_EQ(res.unknown_cmds, 0);
	for (unsigned i = 0; i < 4; i++) {
		const uint8_t *p = t_n64.bus.rdram + verts + 8 * i;
		CHECK_EQ(salsa64_rd16(p), 10 * i + 1);
		CHECK_EQ(salsa64_rd16(p + 2), 20 * i + 2);
		CHECK_EQ(salsa64_rd16(p + 4), 8u);
	}
	/* Las demás (hasta las 32) tampoco se mueven de más: la quinta no se tocó. */
	CHECK_EQ(salsa64_rd16(t_n64.bus.rdram + verts + 8 * 5 + 4), 0u);
}

/* Una lista que entra en ceros (memoria sin escribir) se corta en vez de recorrer la RDRAM entera. */
static void f5i_zeros(void)
{
	start();
	memset(t_n64.bus.rdram + DL_PA + 8, 0, 100000);
	dl_pc += 80000;  /* 10 000 palabras dobles en cero seguidas, y luego un fin que no se alcanza */
	run();
	CHECK(res.flags & GFX_RES_LOOP);
	CHECK(res.flags & GFX_RES_CUT);
	CHECK(res.dl_errors > 0);
	CHECK(res.commands < 9000);
	CHECK(!(res.flags & GFX_RES_FULLSYNC));
}

void t_suite_gfx_f5indy(void)
{
	RUN(f5i_lists);
	RUN(f5i_moveword);
	RUN(f5i_othermode);
	RUN(f5i_vertices);
	RUN(f5i_tri_colors);
	RUN(f5i_tri_tex);
	RUN(f5i_dma_segmented);
	RUN(f5i_light_basic);
	RUN(f5i_sublist);
	RUN(f5i_sublist_enddl_new);
	RUN(f5i_sublist_enddl_old);
	RUN(f5i_texrect);
	RUN(f5i_gen_vertices);
	RUN(f5i_particles);
	RUN(f5i_zeros);
}
