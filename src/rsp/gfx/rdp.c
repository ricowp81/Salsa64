/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — HLE del RDP: estado (othermode, combiner, colores, tiles,
 * imágenes), rectángulos y salida de triángulos agrupados por estado.
 *
 * Cada primitiva se dibuja con el estado vigente; ese estado se resume en un
 * gfx_draw (modo de mezcla, profundidad, texturas decodificadas, combiner
 * ya compilado) y las primitivas seguidas con el mismo resumen comparten
 * registro. Así un cuadro típico produce unos cientos de dibujos para el
 * RSX y no uno por triángulo.
 *
 * Codificación de los comandos y significado de los bits: n64brew.dev
 * (Reality Display Processor/Commands).
 */
#include <string.h>

#include "core/rcp.h"
#include "rsp/gfx/hle.h"

/* Estas rutas son rectángulos y triángulos crudos del RDP, no la emisión de
 * G_TRI de la imagen 1. Compactarlas deja el local store para los lotes de
 * geometría; las demás imágenes conservan -O2. */
#if defined(__SPU__) && GFX_SPE0_IMG == 1
#define GFX_IMG1_COLD __attribute__((optimize("Os")))
#else
#define GFX_IMG1_COLD
#endif
/* Rutas frías en todas las imágenes del SPE 0 (el recorte contra la caja:
 * pocos triángulos por tarea). */
#ifdef __SPU__
#define GFX_SPU_COLD __attribute__((optimize("Os")))
#else
#define GFX_SPU_COLD
#endif

#ifdef SALSA64_GFX_TRACE
/* Depuración en el host (src/host/run.c): el estado resumido de cada dibujo. */
void salsa64_gfx_trace_state(const struct gfx_state *g, const struct gfx_draw *d, int kind, int tile);
#endif

enum {
	RDP_TRI_FIRST = 0xC8, RDP_TRI_LAST = 0xCF,
	G_RDPLOADSYNC = 0xE6, G_RDPPIPESYNC = 0xE7, G_RDPTILESYNC = 0xE8, G_RDPFULLSYNC = 0xE9,
	G_SETKEYGB = 0xEA, G_SETKEYR = 0xEB, G_SETCONVERT = 0xEC, G_SETSCISSOR = 0xED,
	G_SETPRIMDEPTH = 0xEE, G_RDPSETOTHERMODE = 0xEF, G_LOADTLUT = 0xF0,
	G_SETTILESIZE = 0xF2, G_LOADBLOCK = 0xF3, G_LOADTILE = 0xF4, G_SETTILE = 0xF5,
	G_FILLRECT = 0xF6, G_SETFILLCOLOR = 0xF7, G_SETFOGCOLOR = 0xF8, G_SETBLENDCOLOR = 0xF9,
	G_SETPRIMCOLOR = 0xFA, G_SETENVCOLOR = 0xFB, G_SETCOMBINE = 0xFC, G_SETTIMG = 0xFD,
	G_SETZIMG = 0xFE, G_SETCIMG = 0xFF,
};

_Static_assert(sizeof(struct gfx_vertex) == 32, "gfx_vertex: 32 bytes");
_Static_assert(sizeof(struct gfx_draw) % 16 == 0, "gfx_draw: múltiplo de 16");
_Static_assert(sizeof(struct gfx_zclear) % 16 == 0, "gfx_zclear: múltiplo de 16");
_Static_assert(sizeof(struct gfx_job) <= 112, "gfx_job cabe en un trabajo de spukit");
_Static_assert(sizeof(struct gfx_result) == 112, "gfx_result conserva los 112 bytes del resultado de spukit");

/* Clases de primitiva: cambian partes del estado (los rectángulos no se
 * descartan por orientación ni usan la profundidad de los vértices). */
enum prim_kind {
	PRIM_TRI, PRIM_RECT, PRIM_FILL,
	/* Triángulos crudos del RDP: con textura (_T) y/o z (_Z) según el comando. */
	PRIM_RAW = 4, PRIM_RAW_T = 5, PRIM_RAW_Z = 6, PRIM_RAW_TZ = 7,
};

/* Mezcla del blender (othermode L, bits 16–31). */
enum { BL_IN = 0, BL_MEM = 1, BL_BL = 2, BL_FOG = 3 };
enum { BA_IN = 0, BA_FOG = 1, BA_SHADE = 2, BA_ZERO = 3 };
enum { BB_1MA = 0, BB_MEM = 1, BB_ONE = 2, BB_ZERO = 3 };

void gfx_rdp_reset(struct gfx_state *g)
{
	g->oh = 0;
	g->ol = 0;
	g->cc_hi = g->cc_lo = 0;
	g->prim = g->env = g->fog = g->blend = g->fill = 0;
	g->prim_lod = g->prim_min = 0;
	g->prim_z = g->prim_dz = 0;
	g->k0 = g->k1 = g->k2 = g->k3 = g->k4 = g->k5 = 0;
	g->scissor[0] = g->scissor[1] = 0;
	g->scissor[2] = 320 << 2;
	g->scissor[3] = 240 << 2;
	g->cimg_addr = 0;
	g->cimg_width = 320;
	g->cimg_fmt = G_IM_FMT_RGBA;
	g->cimg_siz = G_IM_SIZ_16b;
	g->zimg_addr = 0;
	g->timg_addr = 0;
	g->timg_width = 1;
	g->timg_fmt = g->timg_siz = 0;
	memset(g->tiles, 0, sizeof g->tiles);
	memset(g->loads, 0, sizeof g->loads);
	g->load_next = 0;
	memset(g->tmem, 0, sizeof g->tmem);
	g->tex_gen++;
	gfx_fb_forget(g);
}

void gfx_fb_forget(struct gfx_state *g)
{
	memset(g->fbimg, 0, sizeof g->fbimg);
	g->fbimg_cur = -1;
}

/* La color image actual recibe un dibujo en esta tarea: su entrada en
 * fbimg (la misma dirección, ancho y tamaño de píxel, una libre o la
 * dibujada hace más tareas), como el consumidor elige la ranura de un
 * destino. Solo las de 16 y 8 bits (las que lee como textura desde su
 * destino: RGBA16, e I8 de una de 8 bits) y no las imposibles (ancho 0 o de
 * más de 640): el consumidor las ignora. */
static __attribute__((noinline, cold)) void fb_drawn(struct gfx_state *g)
{
	g->fbimg_cur = -1;
	uint32_t w = g->cimg_width, addr = g->cimg_addr, task = g->job->task;
	if (w - 1 >= 640 || (g->cimg_siz != G_IM_SIZ_16b && g->cimg_siz != G_IM_SIZ_8b))
		return;
	addr |= g->cimg_siz == G_IM_SIZ_8b;  /* struct gfx_fbimg */
	/* La misma imagen o, si no está, la libre o la dibujada hace más. */
	int best = 0;
	uint32_t oldest = 0;
#pragma GCC unroll 1
	for (int i = 0; i < GFX_FB_IMAGES; i++) {
		const struct gfx_fbimg *e = &g->fbimg[i];
		if (e->addr == addr && e->width == w) {
			best = i;
			break;
		}
		uint32_t age = e->width ? task - e->task : ~0u;
		if (age > oldest) {
			oldest = age;
			best = i;
		}
	}
	struct gfx_fbimg *e = &g->fbimg[best];
	if (e->addr != addr || e->width != w) {
		e->addr = addr;
		e->width = w;
		e->lh = w * 3 / 4;
	}
	e->task = task;
	g->fbimg_cur = best;
}

/* Un dibujo de la color image actual llega hasta la fila `bottom` (sus
 * tijeras): el alto conjeturado crece como el del destino del consumidor,
 * que ignora una color image de más de 480 filas. */
static void fb_rows(struct gfx_state *g, uint32_t bottom)
{
	if (g->fbimg_cur < 0)
		return;
	struct gfx_fbimg *e = &g->fbimg[g->fbimg_cur];
	if (bottom > 480)
		e->width = 0;
	else if (bottom > e->lh)
		e->lh = bottom;
}

void gfx_init(struct gfx_state *g)
{
	memset(g, 0, sizeof *g);
	gfx_rdp_reset(g);
	gfx_geom_reset(g);
}

static void rgba8(uint32_t c, uint8_t out[4])
{
	out[0] = (uint8_t)(c >> 24);
	out[1] = (uint8_t)(c >> 16);
	out[2] = (uint8_t)(c >> 8);
	out[3] = (uint8_t)c;
}

/* Color de relleno del RDP en RGBA8888 según el tamaño de la color image. */
static uint32_t fill_rgba(const struct gfx_state *g)
{
	if (g->cimg_siz == G_IM_SIZ_32b)
		return g->fill;
	uint32_t p = g->fill & 0xFFFF;
	uint32_t r = (p >> 11) & 31, gr = (p >> 6) & 31, b = (p >> 1) & 31;
	r = r << 3 | r >> 2;
	gr = gr << 3 | gr >> 2;
	b = b << 3 | b >> 2;
	return r << 24 | gr << 16 | b << 8 | ((p & 1) ? 0xFF : 0);
}

/* Factor del blender como k + ka·α (α = alfa del color que entra): k en
 * 255avos (0, 255 o el alfa de la niebla) y ka en {−1, 0, 1}. Enteros: en
 * el SPE las comparaciones con flotantes y sus constantes pesaban ~900 bytes
 * de código. */
struct bl_factor {
	int k, ka;
};

static bool bl_eq(struct bl_factor f, int k, int ka)
{
	return f.k == k && f.ka == ka;
}

/* P·A + M·B (n64brew.dev: Set Other Modes) llevado a lo que hace el RSX:
 * se suman los factores del color que entra (IN) y del de la memoria (MEM)
 * y se elige el modo de gfx_blend que los calcula tal cual. A_SHADE se toma
 * como el alfa que entra; MEM_A como 1; los colores constantes (blend, fog)
 * en P los pone blend_constant, en M se ignoran. IN·k + MEM con k el alfa
 * de la niebla (G_RM_ADD) es la suma con el color escalado por k: lo escala
 * blend_constant. Lo que no cabe en ningún modo sale como IN·α + MEM·(1 −
 * α), que antes era la salida de todo lo que no fuera opaco, solo z o
 * aditivo (GFX_JOB_BLEND_OLD vuelve a eso para comparar). */
static uint8_t classify_blend(const struct gfx_state *g, unsigned p, unsigned a, unsigned m, unsigned b, uint32_t fog)
{
	struct bl_factor fa = { 0, 0 }, fb = { 0, 0 };
	switch (a) {
	case BA_IN:
	case BA_SHADE: fa.ka = 1; break;
	case BA_FOG:   fa.k = (int)(fog & 0xFF); break;
	default:       break;
	}
	switch (b) {
	case BB_1MA: fb.k = 255 - fa.k, fb.ka = -fa.ka; break;
	case BB_MEM:
	case BB_ONE: fb.k = 255; break;
	default:     break;
	}
	struct bl_factor in = { 0, 0 }, mem = { 0, 0 };
	if (p == BL_IN) in.k += fa.k, in.ka += fa.ka;
	if (p == BL_MEM) mem.k += fa.k, mem.ka += fa.ka;
	if (m == BL_IN) in.k += fb.k, in.ka += fb.ka;
	if (m == BL_MEM) mem.k += fb.k, mem.ka += fb.ka;

	if (bl_eq(mem, 0, 0))
		return GFX_BLEND_OFF;                 /* p. ej. IN·0 + IN·1 */
	if (bl_eq(in, 0, 0) && bl_eq(mem, 255, 0))
		return GFX_BLEND_KEEP;                /* solo profundidad */
	if (bl_eq(in, 0, 1) && bl_eq(mem, 255, 0))
		return GFX_BLEND_ADD;
	if (g->job->flags & GFX_JOB_BLEND_OLD)
		return GFX_BLEND_ALPHA;
	/* Lo que el RDP multiplica por el alfa que entra, o por su complemento,
	 * el RSX lo hace con los mismos factores. Vigilante 8 pinta cada
	 * explosión, humo y resplandor en dos pasadas: MEM·A_IN con el color en
	 * negro (oscurece el fondo según la textura) y encima IN + MEM con la
	 * textura de color; vistas como IN·α + MEM·(1 − α) eran cuadros negros. */
	if (bl_eq(in, 0, 0) && bl_eq(mem, 0, 1))
		return GFX_BLEND_MUL;                 /* MEM·α */
	if (bl_eq(in, 255, 0) && bl_eq(mem, 0, 1))
		return GFX_BLEND_ADD_MUL;             /* IN + MEM·α */
	if (bl_eq(in, 255, -1) && bl_eq(mem, 0, 1))
		return GFX_BLEND_ALPHA_INV;           /* IN·(1 − α) + MEM·α */
	if (in.ka == 0 && in.k > 0 && bl_eq(mem, 255, 0))
		return GFX_BLEND_ADD_ONE;             /* IN·k + MEM: k lo aplica blend_constant */
	return GFX_BLEND_ALPHA;                       /* IN·α + MEM·(1 − α) y parecidos */
}

/* El bit de alfa que el RDP deja en una color image de 16 bits en los
 * ciclos 1 y 2 (gfx_draw.cvg_bit). No es el alfa de la mezcla: es la
 * cobertura del píxel, 3 bits con el alto en el bit de alfa de RGBA5551 y
 * los otros dos en los bits ocultos de la RDRAM. Con el píxel cubierto
 * entero (cvg = 8; el RSX solo mira el centro) y mem la cobertura que lee
 * de la memoria (7 sin IM_RD: no la lee), según CVG_DST:
 *  - CLAMP: cvg − 1 sin mezcla, o cvg + mem con ella, hasta 7: el bit en 1;
 *  - WRAP: (cvg + mem) & 7 = mem;
 *  - FULL («ZAP»): 7;
 *  - SAVE: mem.
 * Con CVG_X_ALPHA la cobertura es cvg·α (α >> 5 con el píxel entero) y en
 * CLAMP el bit sale del alfa: sin FORCE_BL, α >> 5 − 1 (en 1 desde α = 160;
 * desde 128 sobre memoria en 0, con la mezcla del AA); con FORCE_BL,
 * α >> 5 + mem, que sin IM_RD es siempre 7 y con IM_RD da 1 si la memoria
 * ya estaba en 1 o desde α = 128. En WRAP y SAVE con CVG_X_ALPHA se toma la
 * memoria, como sin él. Medido contra ares: Dr. Mario 64 dibuja los paneles
 * translúcidos de su fondo (ZAP, FORCE_BL, IM_RD) con alfa 0,375 o el de una
 * máscara y la consola deja el bit en 1 (#62; docs/framebuffer.md). */
static uint8_t cvg_bit(uint32_t ol)
{
	bool im_rd = (ol & IM_RD) != 0;
	switch (ol & CVG_DST_MASK) {
	case CVG_DST_WRAP:
	case CVG_DST_SAVE:
		return im_rd ? GFX_CVG_KEEP : GFX_CVG_ONE;
	case CVG_DST_FULL:
		return GFX_CVG_ONE;
	default:
		if (!(ol & CVG_X_ALPHA))
			return GFX_CVG_ONE;
		if (ol & FORCE_BL)
			return im_rd ? GFX_CVG_MAX : GFX_CVG_ONE;
		return GFX_CVG_ALPHA;
	}
}

static void blend_state(const struct gfx_state *g, struct gfx_draw *d)
{
	uint32_t ol = g->ol;
	unsigned c0p = (ol >> 30) & 3, c0a = (ol >> 26) & 3, c0m = (ol >> 22) & 3, c0b = (ol >> 18) & 3;
	unsigned c1p = (ol >> 28) & 3, c1a = (ol >> 24) & 3, c1m = (ol >> 20) & 3, c1b = (ol >> 16) & 3;
	unsigned p = c0p, a = c0a, m = c0m, b = c0b;

	bool fog_shade = c0p == BL_FOG && c0a == BA_SHADE && c0m == BL_IN && c0b == BB_1MA;
	if (d->cycle == G_CYC_2CYCLE) {
		/* Primer ciclo de niebla (G_RM_FOG_SHADE_A): fog·a_shade + in·(1−a). */
		if (fog_shade) {
			d->fog = 1;
			d->fog_color = g->fog;
		}
		p = c1p, a = c1a, m = c1m, b = c1b;
	} else if (d->cycle == G_CYC_1CYCLE && fog_shade && (ol & FORCE_BL) &&
		   !(g->job->flags & GFX_JOB_NO_FOG_1CYCLE)) {
		/* La misma mezcla en un solo ciclo con FORCE_BL (othermode L
		 * 0xFA006220: la pista de F-Zero X, con G_FOG): el RDP la hace en
		 * todos los píxeles, FOG·a + IN·(1 − a), y escribe sin leer la
		 * memoria. Se clasificaba como «sin mezcla» y la pista lejana salía
		 * sin niebla (en la consola se funde con el cielo). Sin FORCE_BL la
		 * mezcla solo toca los bordes con AA y el interior escribe P: no es
		 * niebla. */
		d->fog = 1;
		d->fog_color = g->fog;
	}

	d->blend = !(ol & FORCE_BL) ? GFX_BLEND_OFF : classify_blend(g, p, a, m, b, g->fog);
	d->cvg_bit = cvg_bit(ol);
	/* G_RM_VISCVG: IM_RD | FORCE_BL y el blender IN·0 + BL·A_MEM. */
	if ((ol & (IM_RD | FORCE_BL)) == (IM_RD | FORCE_BL) && p == BL_IN && a == BA_ZERO && m == BL_BL && b == BB_MEM)
		d->viscvg = 1;

	if ((ol & G_AC_MASK) == G_AC_THRESHOLD) {
		d->atest = GFX_ATEST_GEQUAL;
		d->aref = (uint8_t)(g->blend & 0xFF) ? (uint8_t)(g->blend & 0xFF) : 1;
		/* Con CVG_X_ALPHA y sin FORCE_BL (G_RM_AA_*_TEX_EDGE con umbral: las
		 * hojas de Pokémon Snap, con el alfa del color de mezcla en 0) la
		 * cobertura es cvg·α y el RDP tampoco escribe donde da 0: α < 32 con
		 * el píxel cubierto. Medido contra la consola (salsa64-run --ares,
		 * docs/referencia-ares.md): 24–32 es lo mejor; con 1, las hojas
		 * salían medio texel más gruesas. */
		if ((ol & CVG_X_ALPHA) && !(ol & FORCE_BL) && d->aref < 32)
			d->aref = 32;
	} else if ((ol & CVG_X_ALPHA) && !(ol & FORCE_BL)) {
		/* Bordes recortados por el alfa de la textura (G_RM_AA_*_TEX_EDGE). */
		d->atest = GFX_ATEST_GEQUAL;
		d->aref = 128;
	} else if ((ol & (CVG_X_ALPHA | AA_EN)) == (CVG_X_ALPHA | AA_EN)) {
		/* Con FORCE_BL (G_RM_TEX_EDGE: IN·0 + IN·1, el medidor de energía de
		 * SM64): la cobertura es cvg·α/32 y el RDP no escribe los píxeles
		 * donde da 0, o sea α < 32 con el píxel cubierto entero. */
		d->atest = GFX_ATEST_GEQUAL;
		d->aref = 32;
	}
}

/* Término sin factores de la combinación (donde va una constante); lo
 * agrega si falta y hay lugar. -1 si no se puede. */
static int constant_term(struct gfx_draw *d)
{
	if (!d->nterms)
		return GFX_K_ONE;
	for (int k = 0; k < d->nterms; k++)
		if (!d->factors[k][0])
			return k;
	if (d->nterms >= GFX_K_TERMS)
		return -1;
	int k = d->nterms++;
	memset(d->factors[k], 0, sizeof d->factors[k]);
	memset(d->k[k], 0, sizeof d->k[k]);
	return k;
}

/* El blender toma el color de P: con P = FOG o BL (el registro de blend)
 * es esa constante, no la salida del combiner (sin FORCE_BL, el RDP
 * escribe P tal cual). Banjo-Kazooie funde la intro con un rectángulo
 * FOG·A_FOG + MEM·(1 − A_FOG) cuyo combiner da 0: con la niebla blanca
 * opaca la pantalla queda blanca (salía negra medio segundo) y con alfa
 * 0xD8 la escena se ve detrás (no se veía nada). El combiner pasa a dar la
 * constante, y como alfa lo que elige A, que es el factor que usa la
 * mezcla del RSX. Con M = IN y A = A_FOG (G_RM_FOG_PRIM_A), el color va
 * hacia la constante en la proporción del alfa de la niebla. */
static void blend_constant(const struct gfx_state *g, struct gfx_draw *d)
{
	uint32_t ol = g->ol;
	unsigned sh = d->cycle == G_CYC_2CYCLE ? 0 : 2;  /* el ciclo que escribe */
	unsigned p = (ol >> (28 + sh)) & 3, a = (ol >> (24 + sh)) & 3;
	unsigned m = (ol >> (20 + sh)) & 3, b = (ol >> (16 + sh)) & 3;
	float fa = (float)(g->fog & 0xFF) / 255.0f;
	/* IN·A_FOG + MEM (G_RM_ADD): la suma con el color ya escalado por el
	 * alfa de la niebla (con 255, tal cual). */
	if (p == BL_IN) {
		if (d->blend == GFX_BLEND_ADD_ONE && a == BA_FOG && fa < 1.0f)
			for (int k = 0; k < GFX_K_TERMS; k++)
				for (int j = 0; j < 3; j++)
					for (int l = 0; l < 3; l++)
						d->k[k][j][l] *= fa;
		return;
	}
	if (p != BL_FOG && p != BL_BL)
		return;
	/* Sin escritura de color (solo z, como la máscara de la Lente de la
	 * Verdad de Zelda: blender BL·0 + MEM·1) el color del blender no se
	 * ve, pero el alfa del combiner sí cuenta para la prueba de alfa: no
	 * se toca (borrarlo anulaba toda la máscara). */
	if (d->blend == GFX_BLEND_KEEP)
		return;
	bool force = (ol & FORCE_BL) != 0;
	bool toward = force && m == BL_IN && a == BA_FOG && b == BB_1MA;
	if (force && m != BL_MEM && !toward)
		return;
	int one = constant_term(d);
	if (one < 0)
		return;
	uint32_t c = p == BL_FOG ? g->fog : g->blend;
	float rgb[3] = { (float)(c >> 24) / 255.0f, (float)((c >> 16) & 0xFF) / 255.0f,
			 (float)((c >> 8) & 0xFF) / 255.0f };
	if (toward) {
		for (int k = 0; k < GFX_K_TERMS; k++)
			for (int j = 0; j < 3; j++)
				for (int l = 0; l < 3; l++)
					d->k[k][j][l] *= 1.0f - fa;
		for (int l = 0; l < 3; l++)
			d->k[one][0][l] += fa * rgb[l];
		return;
	}
	bool alpha = force && a != BA_IN;
	for (int k = 0; k < GFX_K_TERMS; k++)
		for (int j = 0; j < 3; j++)
			for (int l = 0; l < (alpha ? 4 : 3); l++)
				d->k[k][j][l] = 0.0f;
	for (int l = 0; l < 3; l++)
		d->k[one][0][l] = rgb[l];
	if (alpha && a == BA_FOG)
		d->k[one][0][3] = fa;
	else if (alpha && a == BA_SHADE)
		d->k[one][2][3] = 1.0f;
	d->fog = 0;
	if (force)
		d->blend = classify_blend(g, BL_IN, a, m, b, g->fog);
}

static float shift_scale(unsigned shift)
{
	if (shift == 0)
		return 1.0f;
	if (shift <= 10)
		return 1.0f / (float)(1u << shift);
	return (float)(1u << (16 - shift));
}

/* `clamp`: ejes que se limitan aunque la tile repita (gfx_texrect); `lim`:
 * texels que muestrea en cada eje (0: todos), solo para los que se limitan. */
static bool texture_state(struct gfx_state *g, struct gfx_draw *d, int tile, int slot, bool copy, unsigned clamp,
			  const uint16_t lim[2])
{
	struct gfx_tex *t = &d->tex[slot];
	bool premul = (t->flags & GFX_TEX_PREMUL) != 0;  /* lo pidió gfx_combine */
	const struct gfx_tile *ti = &g->tiles[tile & 7];
	unsigned ws = ti->masks && !(ti->cms & 2) && !(clamp & 1) ? ((ti->cms & 1) ? GFX_WRAP_MIRROR : GFX_WRAP_REPEAT)
								    : GFX_WRAP_CLAMP;
	unsigned wt = ti->maskt && !(ti->cmt & 2) && !(clamp & 2) ? ((ti->cmt & 1) ? GFX_WRAP_MIRROR : GFX_WRAP_REPEAT)
								    : GFX_WRAP_CLAMP;
	uint16_t l[2] = { lim && ws == GFX_WRAP_CLAMP ? lim[0] : 0, lim && wt == GFX_WRAP_CLAMP ? lim[1] : 0 };
	if (!gfx_tile_texture(g, tile & 7, t, premul, l, ws == GFX_WRAP_CLAMP))
		return false;
	/* Un eje decodificado en un solo periodo (tmem.c): espejo una vez y la
	 * coordenada corrida un periodo (la textura es el segundo). */
	if (t->flags & GFX_TEX_ONCE_S)
		ws = GFX_WRAP_MIRROR_ONCE;
	if (t->flags & GFX_TEX_ONCE_T)
		wt = GFX_WRAP_MIRROR_ONCE;
	/* Un eje con clamp decodificado en un periodo de su máscara (tmem.c):
	 * repite o se refleja. */
	if (ws == GFX_WRAP_CLAMP && GFX_TEX_WRAP_S(t->flags))
		ws = GFX_TEX_WRAP_S(t->flags);
	if (wt == GFX_WRAP_CLAMP && GFX_TEX_WRAP_T(t->flags))
		wt = GFX_TEX_WRAP_T(t->flags);
	bool bilinear = !copy && ((g->oh >> G_MDSFT_TEXTFILT) & 3) != G_TF_POINT;
	float half = bilinear ? 0.5f : 0.0f;
	t->flags = (uint8_t)(ws | wt << 2 | (bilinear ? GFX_TEX_BILINEAR : 0) | (premul ? GFX_TEX_PREMUL : 0));
	float w = (float)t->width, h = (float)t->height;
	t->xf[0] = shift_scale(ti->shifts) / w;
	t->xf[1] = shift_scale(ti->shiftt) / h;
	t->xf[2] = (half - ti->uls / 4.0f) / w - (ws == GFX_WRAP_MIRROR_ONCE ? 1.0f : 0.0f);
	t->xf[3] = (half - ti->ult / 4.0f) / h - (wt == GFX_WRAP_MIRROR_ONCE ? 1.0f : 0.0f);
	return true;
}

/* Los triángulos del RSP no salen de su caja de recorte, el viewport
 * agrandado por la clip ratio: lo de fuera no llega al RDP. Aquí se suma a
 * las tijeras (los vértices ya están en píxeles: el viewport del RSX es todo
 * el destino). Mario Kart 64 abre la demo con un viewport que crece desde
 * una ventana y tijeras hasta la esquina (0, 0). */
static void clip_box(const struct gfx_state *g, uint16_t sc[4])
{
	for (int i = 0; i < 2; i++) {
		float r = g->clip_ratio[i] * (g->vscale[i] < 0.0f ? -g->vscale[i] : g->vscale[i]);
		float lo = (g->vtrans[i] - r) * 4.0f, hi = (g->vtrans[i] + r) * 4.0f;
		if (lo > (float)sc[i])
			sc[i] = lo >= 4095.0f ? 4095 : (uint16_t)(lo + 0.5f);
		if (hi < (float)sc[i + 2])
			sc[i + 2] = hi <= 0.0f ? 0 : (uint16_t)(hi + 0.5f);
	}
}

/* El combiner con el resultado anterior si sus entradas no cambiaron
 * (struct gfx_ccmemo): combine_cached lo copia y dice si estaba, y
 * combine_keep guarda el que acaba de calcular gfx_combine. Aparte y sin
 * inline: no suman marco a la cadena más honda de la pila del SPE 0 (que
 * pasa por gfx_combine) y el código de las copias queda en un solo lugar. */
/* Copias chicas sin las expansiones en línea de memcpy (en el SPE, cada
 * una pasaba de 1 KB de código): por palabras los términos k, por bytes
 * los factores. */
typedef uint32_t __attribute__((may_alias)) cc_word;

static __attribute__((noinline)) void cc_copy(void *dst, const void *src, uint32_t words, uint32_t bytes)
{
	cc_word *d = dst;
	const cc_word *s = src;
#pragma GCC unroll 1
	for (uint32_t i = 0; i < words; i++)
		d[i] = s[i];
	uint8_t *db = (uint8_t *)dst + 4 * words;
	const uint8_t *sb = (const uint8_t *)src + 4 * words;
#pragma GCC unroll 1
	for (uint32_t i = 0; i < bytes; i++)
		db[i] = sb[i];
}

static uint32_t combine_key5(const struct gfx_state *g, const struct gfx_draw *d)
{
	return g->prim_lod | ((g->oh >> G_MDSFT_CYCLETYPE) & 3) << 8 | (uint32_t)(d->fog != 0) << 10 | 1u << 11;
}

static __attribute__((noinline)) bool combine_cached(const struct gfx_state *g, struct gfx_draw *d, bool *approx)
{
	const struct gfx_ccmemo *m = &g->ccmemo;
	if (m->key[0] != g->cc_hi || m->key[1] != g->cc_lo || m->key[2] != g->prim || m->key[3] != g->env ||
	    m->key[4] != ((uint32_t)g->k4 | (uint32_t)g->k5 << 16) || m->key[5] != combine_key5(g, d))
		return false;
	cc_copy(d->k, m->k, sizeof d->k / 4, 0);
	cc_copy(d->factors, m->factors, 0, sizeof d->factors);
	d->nterms = (uint8_t)m->nterms;
	d->ntex = (uint8_t)m->ntex;
	for (int i = 0; i < 2; i++)
		d->tex[i].flags = (uint8_t)((d->tex[i].flags & ~GFX_TEX_PREMUL) |
					    ((m->premul >> i) & 1 ? GFX_TEX_PREMUL : 0));
	*approx = m->approx != 0;
	return true;
}

static __attribute__((noinline)) void combine_keep(struct gfx_state *g, const struct gfx_draw *d, bool approx)
{
	struct gfx_ccmemo *m = &g->ccmemo;
	m->key[0] = g->cc_hi;
	m->key[1] = g->cc_lo;
	m->key[2] = g->prim;
	m->key[3] = g->env;
	m->key[4] = (uint32_t)g->k4 | (uint32_t)g->k5 << 16;
	m->key[5] = combine_key5(g, d);
	cc_copy(m->k, d->k, sizeof m->k / 4, 0);
	cc_copy(m->factors, d->factors, 0, sizeof m->factors);
	m->nterms = d->nterms;
	m->ntex = d->ntex;
	m->premul = (d->tex[0].flags & GFX_TEX_PREMUL ? 1u : 0u) | (d->tex[1].flags & GFX_TEX_PREMUL ? 2u : 0u);
	m->approx = approx;
}

/* Resume el estado vigente en `d` (sin el tramo de vértices). */
static void build_state(struct gfx_state *g, struct gfx_draw *d, enum prim_kind kind, int tile)
{
	memset(d, 0, sizeof *d);
	d->h.op = GFX_OP_DRAW;
	d->h.size = sizeof *d;
	d->zimg = g->zimg_addr;
	memcpy(d->scissor, g->scissor, sizeof d->scissor);
	if (kind == PRIM_TRI)
		clip_box(g, d->scissor);
	d->cycle = (uint8_t)((g->oh >> G_MDSFT_CYCLETYPE) & 3);

	if (kind == PRIM_FILL || d->cycle == G_CYC_FILL) {
		uint8_t c[4];
		rgba8(fill_rgba(g), c);
		for (int lane = 0; lane < 4; lane++)
			d->k[GFX_K_ONE][0][lane] = c[lane] / 255.0f;
		d->k[GFX_K_ONE][0][3] = 1.0f;
		/* El alfa del relleno (el bit de alfa de un píxel de 16 bits) va aparte:
		 * el color sale con alfa 1, y quien dibuja en un destino con alfa lo
		 * necesita (rsxkit_draw.dst_alpha). Sin prueba de alfa en un relleno. */
		d->aref = c[3];
		d->cycle = G_CYC_FILL;
		d->cvg_bit = GFX_CVG_ALPHA;  /* el bit del color de relleno */
		return;
	}
	if (d->cycle == G_CYC_COPY) {
		for (int lane = 0; lane < 4; lane++)
			d->k[GFX_K_T0][0][lane] = 1.0f;
		d->cvg_bit = GFX_CVG_ALPHA;  /* el RDP copia los 16 bits del texel */
		d->ntex = 1;
		if (!texture_state(g, d, tile, 0, true, g->rect_clamp & 3u, g->rect_lim))
			d->ntex = 0;
		if ((g->ol & G_AC_MASK) == G_AC_THRESHOLD) {
			d->atest = GFX_ATEST_GEQUAL;
			d->aref = 1;
		}
		return;
	}

	bool zbuf = kind == PRIM_RECT || (kind < PRIM_RAW ? (g->geom & G_ZBUFFER) != 0 : (kind & 2) != 0);
	if (zbuf && (g->ol & Z_CMP))
		d->depth |= GFX_DEPTH_TEST;
	if (zbuf && (g->ol & Z_UPD))
		d->depth |= GFX_DEPTH_WRITE;
	if ((g->ol & ZMODE_MASK) == ZMODE_DEC)
		d->depth |= GFX_DEPTH_DECAL;
	if ((d->depth & (GFX_DEPTH_TEST | GFX_DEPTH_WRITE)) && (g->ol & G_ZS_PRIM))
		d->depth |= GFX_DEPTH_PRIM;
	/* Solo con la z del primitivo: ahí el empate es exacto, como en el RDP.
	 * Con la de cada píxel, la z en coma flotante no es la del RDP y un
	 * empate de más o de menos sería al azar (las luces de los edificios de
	 * Perfect Dark, capas XLU sobre la misma geometría, se perdían). */
	if ((d->depth & (GFX_DEPTH_TEST | GFX_DEPTH_PRIM)) == (GFX_DEPTH_TEST | GFX_DEPTH_PRIM) &&
	    (g->ol & ZMODE_MASK) == ZMODE_XLU)
		d->depth |= GFX_DEPTH_LESS;
	if (kind == PRIM_TRI) {
		if ((g->geom & (G_CULL_FRONT | G_CULL_BACK)) == G_CULL_BACK)
			d->cull = GFX_CULL_BACK;
		else if ((g->geom & (G_CULL_FRONT | G_CULL_BACK)) == G_CULL_FRONT)
			d->cull = GFX_CULL_FRONT;
	}
	blend_state(g, d);
	/* El alfa del sombreado es el de la niebla con G_FOG, o el de la tabla
	 * de F3DFLX (flx.c). */
	if (kind != PRIM_TRI || !((g->geom & G_FOG) || (GFX_HAS(FLX) && gfx_flx_shade_alpha(g))))
		d->fog = 0;
	d->aa = kind == PRIM_TRI && (g->ol & AA_EN) && d->blend == GFX_BLEND_OFF && d->atest == GFX_ATEST_NONE &&
		!d->viscvg;

	/* La niebla ya está decidida: con ella, gfx_combine deja el término al
	 * que el vertex program suma el color de niebla. */
	bool approx;
	if (!combine_cached(g, d, &approx)) {
		approx = gfx_combine(g, d);
		combine_keep(g, d, approx);
	}
	if (approx)
		g->res->approx_cc++;
	d->approx = approx;
	bool textured = kind == PRIM_RECT || (kind < PRIM_RAW ? g->tex_on != 0 : (kind & 1) != 0);
	if (!textured && d->ntex) {
		d->ntex = 0;
		gfx_combine_untextured(d);
	}
	unsigned n = d->ntex;
	for (unsigned i = 0; i < n; i++)
		if (!texture_state(g, d, tile + (int)i, (int)i, false,
				   kind == PRIM_RECT ? (g->rect_clamp >> (2 * i)) & 3u : 0,
				   kind == PRIM_RECT && i == 0 ? g->rect_lim : NULL)) {
			d->ntex = (uint8_t)i;
			break;
		}
	/* Sin corrección de perspectiva (G_TP_NONE) el RDP no divide los
	 * coeficientes de textura por el término 1/W que manda el RSP: usa
	 * S·(1/W) tal cual. Los microcódigos de la segunda generación (F3DEX2,
	 * F3DZEX, el de Conker) normalizan 1/W a ~0x4000 (la mitad de 0x8000)
	 * en cada triángulo, así que la textura se muestrea a la mitad de la
	 * coordenada del vértice; medido con el microcódigo real de Majora's
	 * Mask: el sol del telescopio (un atlas de 64 × 32 con dos soles, s de
	 * 0 a 63) muestra solo el sol izquierdo. Los TEXRECT llevan s y t
	 * directos al RDP y no cambian. También F3DEX 1.x lo hace: Mace dibuja
	 * sus textos (glifos de 16 × 16 en un atlas de 160 de ancho, con s hasta
	 * 127 en los vértices) sin perspectiva, y con el LLE las coordenadas que
	 * llegan al RDP son la mitad (la tarea 361 del selector de personajes
	 * pasó de 230 tareas distintas a 0 en la primera ronda de 375). */
	if (kind == PRIM_TRI && d->ntex && !((g->oh >> G_MDSFT_TEXTPERSP) & 1) &&
	    (g->job->gbi == SALSA64_GBI_F3DEX2 || g->job->gbi == SALSA64_GBI_F3DZEX2 ||
	     g->job->gbi == SALSA64_GBI_F3DEXBG || g->job->gbi == SALSA64_GBI_F3DEX
#if GFX_HAS(F3DEX2) && GFX_HAS(S2DEX)
	     /* Solo la carga S2DEX→F3DEX2 con el interruptor activo puede poner
	      * este indicador; gfx_run lo borra al empezar cada tarea. */
	     || g->chain_ex == GFX_CHAIN_AFFINE
#endif
	    ))
		for (unsigned i = 0; i < d->ntex; i++) {
			d->tex[i].xf[0] *= 0.5f;
			d->tex[i].xf[1] *= 0.5f;
		}
	/* Sin una textura (apagadas o arena lleno): lo que la leía se anula. */
	if (d->nterms)
		gfx_combine_textures(d, d->ntex);
	else if (d->ntex < n)
		gfx_combine_textures(d, d->ntex);
	blend_constant(g, d);
#ifdef SALSA64_GFX_TRACE
	salsa64_gfx_trace_state(g, d, kind, tile);
#endif
}

void gfx_flush_draw(struct gfx_state *g)
{
	if (!g->have_draw)
		return;
	g->have_draw = false;
	if (g->draw.h.b == 0)
		return;
	if (gfx_io_cmd(g, &g->draw, sizeof g->draw))
		g->res->draws++;
}

static void send_target(struct gfx_state *g)
{
	if (g->target_sent)
		return;
	struct gfx_cmd c = { GFX_OP_TARGET, sizeof c, g->cimg_addr, g->cimg_width,
			     (uint32_t)g->cimg_fmt << 8 | g->cimg_siz };
	gfx_flush_draw(g);
	if (gfx_io_cmd(g, &c, sizeof c)) {
		g->target_sent = true;
		fb_drawn(g);
	}
}

/* Deja abierto un dibujo con el estado vigente para `nvtx` vértices más y
 * devuelve dónde escribirlos (NULL si no hay lugar). */
static struct gfx_vertex *begin_prim(struct gfx_state *g, enum prim_kind kind, int tile, uint32_t nvtx)
{
	send_target(g);
	if (g->dirty || !g->have_draw || g->draw.prim_kind != (uint32_t)kind ||
	    ((kind == PRIM_RECT || kind >= PRIM_RAW) && g->draw.prim_tile != (uint32_t)tile)) {
		struct gfx_draw d;
		uint32_t t0 = gfx_io_clock();
		build_state(g, &d, kind, tile);
		g->res->t_state += gfx_io_clock() - t0;
		GFX_DETAIL(g, g->detail->state_builds++);
		d.prim_kind = kind;
		d.prim_tile = (uint16_t)tile;
		g->dirty = false;
		/* Mismo estado que el dibujo abierto: se continúa. Con la z del
		 * primitivo, también la misma z: el consumidor da una sola a
		 * todo el dibujo (gfx_prim_z). */
		bool same = g->have_draw &&
			    memcmp((const uint8_t *)&d + sizeof d.h, (const uint8_t *)&g->draw + sizeof d.h,
				   sizeof d - sizeof d.h) == 0 &&
			    (!(d.depth & GFX_DEPTH_PRIM) || g->draw_prim_z == g->prim_z);
		if (!same) {
			gfx_flush_draw(g);
			g->draw = d;
			g->draw_prim_z = g->prim_z;
			g->draw.h.a = g->vtx_count;
			g->draw.h.b = 0;
			g->have_draw = true;
			fb_rows(g, (d.scissor[3] + 3u) / 4u);
		}
	}
	struct gfx_vertex *v = gfx_io_vtx(g, nvtx);
	if (!v)
		return NULL;
	g->vtx_count += nvtx;
	g->draw.h.b += nvtx;
	return v;
}

/* Punto de a→b a la fracción t: la salida es lineal en las coordenadas de
 * recorte, así que se interpola ya escrita. Con GFX_JOB_INTERP, entre dos
 * lugares del mismo grupo lleva la identidad del tramo (gfx.h); si no, 0. */
static void lerp_vertex(struct gfx_vertex *o, const struct gfx_vertex *a, const struct gfx_vertex *b, float t)
{
	o->x = a->x + (b->x - a->x) * t;
	o->y = a->y + (b->y - a->y) * t;
	o->z = a->z + (b->z - a->z) * t;
	o->w = a->w + (b->w - a->w) * t;
	o->s = a->s + (b->s - a->s) * t;
	o->t = a->t + (b->t - a->t) * t;
	o->r = (uint8_t)(a->r + (b->r - a->r) * t + 0.5f);
	o->g = (uint8_t)(a->g + (b->g - a->g) * t + 0.5f);
	o->b = (uint8_t)(a->b + (b->b - a->b) * t + 0.5f);
	o->a = (uint8_t)(a->a + (b->a - a->a) * t + 0.5f);
	uint32_t ia = a->pad, ib = b->pad;
	o->pad = ia && ia < (1u << 25) && GFX_VGID_SERIAL(ia) == GFX_VGID_SERIAL(ib)
		 ? GFX_VCLIP_ID(GFX_VGID_SERIAL(ia), GFX_VGID_INDEX(ia), GFX_VGID_INDEX(ib)) : 0;
}

/* El RSP recorta cada triángulo contra el plano cercano y contra la caja
 * del clip ratio (el viewport agrandado r veces, clip_box) antes de
 * mandarlo al RDP, y el RDP interpola el sombreado lineal en pantalla
 * dentro de cada triángulo que recibe. Un triángulo con dos vértices cerca
 * de la cámara y muy fuera de la pantalla (el fondo de las pistas de F-Zero
 * X) llega a lo visible con la sombra del polígono recortado, no con la
 * del triángulo entero corregida por perspectiva; para reproducirlo hace
 * falta el mismo polígono y el mismo abanico que el microcódigo. Medido
 * contra el microcódigo real (F3DEX2 fifo 2.03 en la LLE del host: 114 de
 * 115 abanicos): Sutherland–Hodgman plano por plano; cada plano recorre el
 * polígono desde su primer vértice con el anterior (la intersección sale
 * antes que el vértice que entra); el plano izquierdo va al final; el
 * polígono se dibuja en abanico desde su primer vértice y cada triángulo
 * del abanico pasa por el rechazo trivial de los planos de la pantalla (los
 * que quedan enteros fuera de un mismo plano no se dibujan). Las
 * coordenadas de salida son píxeles por w: los planos son lineales en
 * ellas. Cada plano agrega a lo más un vértice: 3 + 5 como máximo.
 * GFX_JOB_NO_RSP_CLIP deja solo el plano cercano, como antes. */
#define CLIP_MAX 9

/* Un plano a·x + b·y + c·w ≥ 0 sobre el polígono src[0..n): deja el
 * resultado en dst y devuelve sus vértices (0 si queda todo fuera). */
static GFX_SPU_COLD unsigned clip_plane(const struct gfx_vertex *src, unsigned n, float a, float b, float c,
					  struct gfx_vertex *dst)
{
	float d[CLIP_MAX];
	unsigned in = 0, m = 0;
	for (unsigned i = 0; i < n; i++) {
		d[i] = a * src[i].x + b * src[i].y + c * src[i].w;
		in += d[i] >= 0.0f;
	}
	if (in == n) {
		memcpy(dst, src, n * sizeof *src);
		return n;
	}
	for (unsigned i = 0; i < n && m + 2 <= CLIP_MAX; i++) {
		unsigned j = i ? i - 1 : n - 1;
		bool ci = d[i] >= 0.0f, cj = d[j] >= 0.0f;
		if (ci != cj) {
			lerp_vertex(&dst[m], &src[j], &src[i], d[j] / (d[j] - d[i]));
			dst[m++].pad = 0;  /* GFX_VCLIP_ID es solo del plano cercano (gfx.h) */
		}
		if (ci)
			dst[m++] = src[i];
	}
	return m;
}

/* El polígono p[0..n) contra la caja del clip ratio, en el orden del
 * microcódigo (derecha, abajo, arriba, izquierda); devuelve los vértices
 * que quedan en p. */
static GFX_SPU_COLD unsigned clip_box_poly(const struct gfx_state *g, struct gfx_vertex *p, unsigned n)
{
	struct gfx_vertex q[CLIP_MAX];
	float lim[4];
	for (int i = 0; i < 2; i++) {
		float r = g->clip_ratio[i] * (g->vscale[i] < 0.0f ? -g->vscale[i] : g->vscale[i]);
		lim[2 * i] = g->vtrans[i] - r;
		lim[2 * i + 1] = g->vtrans[i] + r;
	}
	n = clip_plane(p, n, -1.0f, 0.0f, lim[1], q);   /* x ≤ xhi·w */
	n = clip_plane(q, n, 0.0f, -1.0f, lim[3], p);   /* y ≤ yhi·w */
	n = clip_plane(p, n, 0.0f, 1.0f, -lim[2], q);   /* y ≥ ylo·w */
	n = clip_plane(q, n, 1.0f, 0.0f, -lim[0], p);   /* x ≥ xlo·w */
	return n;
}

/* ¿Algún vértice de p[0..n) fuera de la caja del clip ratio? */
static inline bool outside_box(const struct gfx_state *g, const struct gfx_vertex *p, unsigned n)
{
	float r0 = g->clip_ratio[0] * (g->vscale[0] < 0.0f ? -g->vscale[0] : g->vscale[0]);
	float r1 = g->clip_ratio[1] * (g->vscale[1] < 0.0f ? -g->vscale[1] : g->vscale[1]);
	float xlo = g->vtrans[0] - r0, xhi = g->vtrans[0] + r0, ylo = g->vtrans[1] - r1, yhi = g->vtrans[1] + r1;
	bool out = false;
	for (unsigned i = 0; i < n; i++) {
		float w = p[i].w;
		out |= p[i].x < xlo * w || p[i].x > xhi * w || p[i].y < ylo * w || p[i].y > yhi * w;
	}
	return out;
}

/* Abanico desde p[0] de un polígono recortado: qué triángulos (bit i:
 * p[0], p[i], p[i+1]) se dibujan y cuántos son. Los enteros fuera de un
 * plano de la pantalla no (como el rechazo trivial del RSP con los vértices
 * nuevos). Hoja: no suma a la cadena de begin_prim. */
static GFX_SPU_COLD unsigned fan_keep(const struct gfx_state *g, const struct gfx_vertex *p, unsigned n,
				      unsigned *count)
{
	float sx = g->vscale[0] < 0.0f ? -g->vscale[0] : g->vscale[0];
	float sy = g->vscale[1] < 0.0f ? -g->vscale[1] : g->vscale[1];
	float xlo = g->vtrans[0] - sx, xhi = g->vtrans[0] + sx, ylo = g->vtrans[1] - sy, yhi = g->vtrans[1] + sy;
	unsigned flags[CLIP_MAX], keep = 0;
	*count = 0;
	for (unsigned i = 0; i < n; i++) {
		float w = p[i].w;
		flags[i] = (p[i].x < xlo * w) | (p[i].x > xhi * w) << 1 | (p[i].y < ylo * w) << 2 | (p[i].y > yhi * w) << 3;
	}
	for (unsigned i = 1; i + 1 < n; i++)
		if (!(flags[0] & flags[i] & flags[i + 1])) {
			keep |= 1u << i;
			(*count)++;
		}
	return keep;
}

void gfx_triangle(struct gfx_state *g, uint32_t a, uint32_t b, uint32_t c)
{
	uint32_t td = 0, ts = 0;
	GFX_DETAIL(g, { td = gfx_io_clock(); ts = g->res->t_state; g->detail->triangles_in++; });
	if (a >= GFX_MAX_VTX || b >= GFX_MAX_VTX || c >= GFX_MAX_VTX)
		goto detail_done;
	const struct gfx_vtx *v[3] = { &g->vtx[a], &g->vtx[b], &g->vtx[c] };
	/* Fuera del mismo plano los tres: no se ve. */
	if (v[0]->clip & v[1]->clip & v[2]->clip & GFX_CLIP_MASK) {
		GFX_DETAIL(g, g->detail->triangles_common++);
		goto detail_done;
	}
	struct gfx_vertex p[CLIP_MAX];
	for (int i = 0; i < 3; i++)
		gfx_vtx_out(g, v[i], &p[i]);
	if (!(g->geom & G_SHADING_SMOOTH)) {
		/* Sombreado plano: el color del primer vértice. */
		for (int i = 1; i < 3; i++) {
			p[i].r = p[0].r;
			p[i].g = p[0].g;
			p[i].b = p[0].b;
			p[i].a = p[0].a;
		}
	}
	/* Plano cercano (z = −w), como F3D y F3DEX: lo que queda detrás no se
	 * dibuja y un triángulo que lo cruza sale como uno o dos. Sin esto, los
	 * vértices con w ≈ 0 o negativa se proyectan lejísimos. */
	unsigned n = 3;
	bool clipped = false;
	if (g->near_clip) {
		float d[3];
		unsigned behind = 0;
		for (int i = 0; i < 3; i++) {
			d[i] = v[i]->z + v[i]->w;
			behind += d[i] < 0.0f;
		}
		if (behind == 3) {
			GFX_DETAIL(g, g->detail->triangles_near++);
			goto detail_done;
		}
		if (behind) {
			GFX_DETAIL(g, g->detail->triangles_clipped++);
			struct gfx_vertex in[3] = { p[0], p[1], p[2] };
			n = 0;
			for (int i = 0; i < 3; i++) {
				int j = i ? i - 1 : 2;
				bool ci = d[i] >= 0.0f, cj = d[j] >= 0.0f;
				if (ci != cj)
					lerp_vertex(&p[n++], &in[j], &in[i], d[j] / (d[j] - d[i]));
				if (ci)
					p[n++] = in[i];
			}
			clipped = true;
		}
	}
	/* La caja del clip ratio: solo si algún vértice se sale de ella. */
	if (!(g->job->flags & GFX_JOB_NO_RSP_CLIP) && outside_box(g, p, n)) {
		GFX_DETAIL(g, g->detail->triangles_clipped++);
		n = clip_box_poly(g, p, n);
		clipped = true;
	}
	unsigned count = 1, keep = 2;  /* sin recorte: el triángulo (p[0], p[1], p[2]) */
	if (clipped && !(n >= 3 && (keep = fan_keep(g, p, n, &count))))
		goto detail_done;
	struct gfx_vertex *o = begin_prim(g, PRIM_TRI, g->tex_tile, 3 * count);
	if (!o)
		goto detail_done;
	g->res->triangles++;
	GFX_DETAIL(g, g->detail->triangles_ok++);
	for (unsigned i = 1; i + 1 < n; i++)
		if (keep & (1u << i)) {
			o[0] = p[0];
			o[1] = p[i];
			o[2] = p[i + 1];
			o += 3;
		}
detail_done:
	GFX_DETAIL(g, g->detail->t_triangle += gfx_io_clock() - td - (g->res->t_state - ts));
}

/* Rectángulo de pantalla en dos triángulos. x, y en píxeles; s, t en texels
 * en cada esquina (0: arriba a la izquierda, 1: arriba a la derecha,
 * 2: abajo a la izquierda, 3: abajo a la derecha). */
static GFX_IMG1_COLD void rect(struct gfx_state *g, enum prim_kind kind, int tile, float x0, float y0, float x1, float y1,
		 const float s[4], const float t[4], unsigned clamp, const uint16_t lim[2])
{
	if (x1 <= x0 || y1 <= y0)
		return;
	if (clamp != g->rect_clamp) {
		g->rect_clamp = (uint8_t)clamp;
		g->dirty = true;
	}
	uint16_t l0 = lim ? lim[0] : 0, l1 = lim ? lim[1] : 0;
	if (l0 != g->rect_lim[0] || l1 != g->rect_lim[1]) {
		g->rect_lim[0] = l0;
		g->rect_lim[1] = l1;
		g->dirty = true;
	}
	struct gfx_vertex *o = begin_prim(g, kind, tile, 6);
	if (!o)
		return;
	g->res->rects++;
	float z = (g->ol & G_ZS_PRIM) ? (float)g->prim_z / 32767.0f : 0.0f;
	const float xs[4] = { x0, x1, x0, x1 }, ys[4] = { y0, y0, y1, y1 };
	static const int order[6] = { 0, 1, 2, 2, 1, 3 };
	for (int i = 0; i < 6; i++) {
		int k = order[i];
		o[i].x = xs[k];
		o[i].y = ys[k];
		o[i].z = z;
		o[i].w = 1.0f;
		o[i].s = s ? s[k] : 0.0f;
		o[i].t = t ? t[k] : 0.0f;
		o[i].r = o[i].g = o[i].b = o[i].a = 0;
		o[i].pad = 0;
	}
}

/* Texels desde el 0 hasta el que muestrea u < hi (y el siguiente con
 * filtro); 0 si son todos los n. Con filtro puntual, floor(u) < hi: ceil(hi)
 * texels (una franja de una fila con t de 0 a 1 es de una fila, no dos: así
 * su procedencia no depende del paso, tmem.c). */
static uint16_t sampled(float hi, bool bilinear, uint32_t n)
{
	uint32_t k;
	if (hi < 0.0f)
		k = 1;
	else if (bilinear)
		k = (uint32_t)hi + 2;
	else
		k = hi == 0.0f ? 1 : (uint32_t)hi + ((float)(uint32_t)hi < hi);
	return k < n ? (uint16_t)k : 0;
}

/* Ejes que se limitan aunque repitan (bits 0–1 la tile, 2–3 la del segundo
 * ciclo) y si el rectángulo va en una tira por fila: ver gfx_texrect.
 * Aparte y sin inline: en el SPE sus temporales no ocupan la pila mientras
 * gfx_texrect llama a rect, que está en la cadena más honda (fondo S2DEX →
 * TEXRECT → estado → combiner; src/spu/gfx/main.c). */
static GFX_IMG1_COLD __attribute__((noinline)) unsigned texrect_clamp(const struct gfx_state *g, int tile, unsigned cycle, float s0,
							 float t0, float dsdx, float dtdy, float ns, float nt,
							 bool *rows)
{
	unsigned clamp = 0;
	bool r = false;
	for (int i = 0; i < (cycle == G_CYC_2CYCLE ? 2 : 1); i++) {
		const struct gfx_tile *ui = &g->tiles[(tile + i) & 7];
		uint32_t uw, uh;
		gfx_tile_size(ui, &uw, &uh);
		float us = shift_scale(ui->shifts), ut = shift_scale(ui->shiftt);
		float sa = s0 * us - ui->uls / 4.0f, sb = (s0 + (ns - 1.0f) * dsdx) * us - ui->uls / 4.0f;
		float ta = t0 * ut - ui->ult / 4.0f, tb = (t0 + (nt - 1.0f) * dtdy) * ut - ui->ult / 4.0f;
		if ((sa < sb ? sa : sb) > -0.01f && (sa < sb ? sb : sa) < (float)uw - 0.99f)
			clamp |= 1u << (2 * i);
		if ((ta < tb ? ta : tb) > -0.01f && (ta < tb ? tb : ta) < (float)uh - 0.99f)
			clamp |= 2u << (2 * i);
		float step = dtdy * ut;
		r |= (step < 0.0f ? -step : step) >= 2.0f && (step < 0.0f ? -step : step) * 2.0f >= (float)uh;
	}
	*rows = r;
	return clamp;
}

GFX_IMG1_COLD void gfx_texrect(struct gfx_state *g, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3, bool flip)
{
	unsigned cycle = (g->oh >> G_MDSFT_CYCLETYPE) & 3;
	float lrx = ((w0 >> 12) & 0xFFF) / 4.0f, lry = (w0 & 0xFFF) / 4.0f;
	float ulx = ((w1 >> 12) & 0xFFF) / 4.0f, uly = (w1 & 0xFFF) / 4.0f;
	int tile = (w1 >> 24) & 7;
	float s0 = (float)(int16_t)(w2 >> 16) / 32.0f, t0 = (float)(int16_t)(w2 & 0xFFFF) / 32.0f;
	float dsdx = (float)(int16_t)(w3 >> 16) / 1024.0f, dtdy = (float)(int16_t)(w3 & 0xFFFF) / 1024.0f;
	if (cycle == G_CYC_COPY || cycle == G_CYC_FILL) {
		/* Píxeles enteros y bordes inferiores inclusivos; en copia, 4
		 * píxeles por ciclo (dsdx = 4.0 es un texel por píxel). */
		if (cycle == G_CYC_COPY)
			dsdx /= 4.0f;
		ulx = (float)(int)ulx, uly = (float)(int)uly;
		lrx = (float)(int)lrx + 1.0f, lry = (float)(int)lry + 1.0f;
	} else if (lry - uly < 1.0f) {
		lry = uly + 1.0f;
	}
	float w = lrx - ulx, h = lry - uly;

	/* Ejes cuyos texels muestreados caen todos dentro de la tile: ahí se
	 * limita aunque repita. Da lo mismo que en la N64 y evita que, a más
	 * resolución, el filtro mezcle el borde con el lado opuesto (Mario
	 * Kart 64 dibuja sus imágenes en tiras de 2 filas que repiten: salían
	 * líneas). Cada tile con su desplazamiento y su origen: la tile + 1
	 * del segundo ciclo (el ruido del texto «NINTENDO 64» de Zelda, que se
	 * repite) va en los bits 2–3.
	 * Una tile que avanza media tile o más por fila (ese ruido: la tile
	 * entera, ×32) la N64 la muestrea con la t de cada fila, la misma en
	 * toda la fila; a más resolución el RSX interpolaría t dentro de la
	 * fila y recorrería media textura (rayas). Ahí el rectángulo sale en
	 * una tira por fila. Un fondo achicado (S2DEX) avanza pocos texels por
	 * fila y queda entero. */
	const struct gfx_tile *ti = &g->tiles[tile];
	uint32_t tw, th;
	gfx_tile_size(ti, &tw, &th);
	float ns = flip ? h : w, nt = flip ? w : h;  /* muestras en s y en t */
	float ks = shift_scale(ti->shifts), kt = shift_scale(ti->shiftt);
	bool rows;
	unsigned clamp = texrect_clamp(g, tile, cycle, s0, t0, dsdx, dtdy, ns, nt, &rows);
	rows = rows && !flip && h <= 16.0f && (cycle == G_CYC_1CYCLE || cycle == G_CYC_2CYCLE);

	/* El RDP muestrea en la esquina de cada píxel; el RSX, en el centro:
	 * con filtro bilineal se corre medio píxel para no mezclar texels
	 * vecinos en un rectángulo 1:1. */
	bool bilinear = (cycle == G_CYC_1CYCLE || cycle == G_CYC_2CYCLE) &&
			((g->oh >> G_MDSFT_TEXTFILT) & 3) != G_TF_POINT;
	if (bilinear) {
		s0 -= 0.5f * dsdx;
		t0 -= 0.5f * dtdy;
	}
	/* Texels que puede muestrear el rectángulo, desde el 0 de la tile: el
	 * RSX (con u = s·k − uls, como en texture_state) toma floor(u) y, con
	 * filtro, el siguiente, para todo u dentro del rectángulo (también
	 * entre las muestras del RDP, a más resolución). En un eje limitado, lo
	 * de más allá no se usa: no hace falta decodificarlo. Solo con tiles
	 * más grandes que la TMEM (Perfect Dark copia el cuadro anterior en
	 * franjas de 320 × 1 con una tile de 513 × 9): con una que cabe, los
	 * rectángulos que la comparten (tiras de un fondo de S2DEX, letras de
	 * una misma textura) siguen en un dibujo y una sola textura. */
	uint16_t lim[2] = { 0, 0 };
	if (tw * th * (4u << ti->siz) > 4096u * 8u) {
		float se = s0 + ns * dsdx, te = t0 + nt * dtdy;
		lim[0] = sampled((se > s0 ? se : s0) * ks - ti->uls / 4.0f, bilinear, tw);
		lim[1] = sampled((te > t0 ? te : t0) * kt - ti->ult / 4.0f, bilinear, th);
	}
	float s[4], t[4];
	if (rows) {
		/* La t de la fila r es la de su esquina (sin el medio píxel). */
		float tr = bilinear ? t0 + 0.5f * dtdy : t0;
		s[0] = s0, s[1] = s0 + w * dsdx, s[2] = s0, s[3] = s0 + w * dsdx;
		for (float y = uly; y < lry; y += 1.0f, tr += dtdy) {
			t[0] = t[1] = t[2] = t[3] = tr;
			rect(g, PRIM_RECT, tile, ulx, y, lrx, y + 1.0f < lry ? y + 1.0f : lry, s, t, clamp, lim);
		}
		return;
	}
	if (!flip) {
		s[0] = s0, s[1] = s0 + w * dsdx, s[2] = s0, s[3] = s0 + w * dsdx;
		t[0] = t0, t[1] = t0, t[2] = t0 + h * dtdy, t[3] = t0 + h * dtdy;
	} else {
		s[0] = s0, s[1] = s0, s[2] = s0 + h * dsdx, s[3] = s0 + h * dsdx;
		t[0] = t0, t[1] = t0 + w * dtdy, t[2] = t0, t[3] = t0 + w * dtdy;
	}
	rect(g, PRIM_RECT, tile, ulx, uly, lrx, lry, s, t, clamp, lim);
}

/* ---- Triángulos crudos del RDP --------------------------------------------
 * Comandos 0xC8–0xCF (n64brew.dev, "Reality Display Processor/Commands"):
 * aristas H (mayor, de YH a YL), M (de YH a YM) y L (de YM a YL) en
 * píxeles, y bloques opcionales de sombreado, textura y z con su valor en
 * (XH, floor(YH)), su cambio por píxel en x (DxDx) y a lo largo de la
 * arista mayor (DxDe). El RDP rellena dos trapecios: de YH a YM entre H y
 * M, y de YM a YL entre H y L; cada uno sale como dos triángulos con los
 * atributos evaluados en sus esquinas. Llegan en `w` como pares de
 * palabras (alta, baja) de cada doble palabra del comando. */

/* Coordenada x (s11.16 en los 28 bits bajos) y pendiente (s13.16 en 30). */
static float raw_x(uint32_t v) { return (float)((int32_t)(v << 4) >> 4) * (1.0f / 65536.0f); }
static float raw_dx(uint32_t v) { return (float)((int32_t)(v << 2) >> 2) * (1.0f / 65536.0f); }
/* y en s11.2 (14 bits). */
static float raw_y(uint32_t v) { return (float)((int32_t)(v << 18) >> 18) * 0.25f; }

/* Canal k (0–3) de un bloque de 8 dobles palabras desde w: entero en la
 * doble palabra `di`, fracción en `df`. */
static float raw_coef(const uint32_t *w, int di, int df, int k)
{
	uint32_t wi = w[2 * di + (k >> 1)], wf = w[2 * df + (k >> 1)];
	uint32_t hi = (k & 1) ? wi & 0xFFFF : wi >> 16, lo = (k & 1) ? wf & 0xFFFF : wf >> 16;
	return (float)(int32_t)(hi << 16 | lo) * (1.0f / 65536.0f);
}

struct raw_attr {
	float v[4], dx[4], de[4];
};

static void raw_block(const uint32_t *b, struct raw_attr *a)
{
	for (int k = 0; k < 4; k++) {
		a->v[k] = raw_coef(b, 0, 2, k);
		a->dx[k] = raw_coef(b, 1, 3, k);
		a->de[k] = raw_coef(b, 4, 6, k);
	}
}

unsigned gfx_rdp_raw_words(uint32_t op)
{
	if (op == 0xE4 || op == 0xE5)  /* TEXRECT y TEXRECTFLIP */
		return 2;
	if (op < 0xC8 || op > 0xCF)
		return 1;
	return 4 + ((op & 4) ? 8 : 0) + ((op & 2) ? 8 : 0) + ((op & 1) ? 2 : 0);
}

#if GFX_HAS(S2DEX)
/* Un cuadrilátero con textura (sin sombreado ni z), como los dos triángulos
 * de un sprite rotado de S2DEX: esquinas en orden alrededor, x e y en
 * píxeles y s, t en texels de la tile. */
void gfx_rdp_tex_quad(struct gfx_state *g, int tile, const float *x, const float *y, const float *s, const float *t)
{
	static const int order[6] = { 0, 1, 2, 0, 2, 3 };
	struct gfx_vertex v[4];
	memset(v, 0, sizeof v);
	for (int i = 0; i < 4; i++) {
		v[i].x = x[i];
		v[i].y = y[i];
		v[i].w = 1.0f;
		v[i].s = s[i];
		v[i].t = t[i];
	}
	struct gfx_vertex *o = begin_prim(g, (enum prim_kind)(PRIM_RAW | 1), tile, 6);
	if (!o)
		return;
	g->res->triangles += 2;
	for (int i = 0; i < 6; i++)
		o[i] = v[order[i]];
}
#endif

GFX_IMG1_COLD void gfx_rdp_triangle(struct gfx_state *g, const uint32_t *w)
{
	uint32_t op = w[0] >> 24;
	bool shade = op & 4, tex = op & 2, zb = op & 1;
	int tile = (w[0] >> 16) & 7;
	float yl = raw_y(w[0]), ym = raw_y(w[1] >> 16), yh = raw_y(w[1]);
	float xl = raw_x(w[2]), dxl = raw_dx(w[3]);
	float xh = raw_x(w[4]), dxh = raw_dx(w[5]);
	float xm = raw_x(w[6]), dxm = raw_dx(w[7]);
	float y0 = (float)(int)(yh >= 0.0f ? yh : yh - 0.75f);  /* floor(YH) */
	const uint32_t *p = w + 8;
	struct raw_attr sh, tx;
	memset(&sh, 0, sizeof sh);
	memset(&tx, 0, sizeof tx);
	if (shade) {
		raw_block(p, &sh);
		p += 16;
	}
	if (tex) {
		raw_block(p, &tx);
		p += 16;
	}
	float z = 0.0f, dzdx = 0.0f, dzde = 0.0f;
	if (zb) {
		z = (float)(int32_t)p[0] * (1.0f / 65536.0f);
		dzdx = (float)(int32_t)p[1] * (1.0f / 65536.0f);
		dzde = (float)(int32_t)p[2] * (1.0f / 65536.0f);
	}
	bool persp = tex && ((g->oh >> G_MDSFT_TEXTPERSP) & 1);

	/* Esquinas: (x, y) de cada trapecio, en orden alrededor. */
	float c[2][4][2];
	int quads = 0;
	if (ym > yh) {
		float hm = xh + dxh * (ym - y0), mm = xm + dxm * (ym - y0);
		float h0 = xh + dxh * (yh - y0), m0 = xm + dxm * (yh - y0);
		float q[4][2] = { { h0, yh }, { m0, yh }, { mm, ym }, { hm, ym } };
		memcpy(c[quads++], q, sizeof q);
	}
	if (yl > ym) {
		float hm = xh + dxh * (ym - y0), hl = xh + dxh * (yl - y0);
		float lm = xl, ll = xl + dxl * (yl - ym);
		float q[4][2] = { { hm, ym }, { lm, ym }, { ll, yl }, { hl, yl } };
		memcpy(c[quads++], q, sizeof q);
	}
	for (int qi = 0; qi < quads; qi++) {
		struct gfx_vertex v[4];
		for (int i = 0; i < 4; i++) {
			float x = c[qi][i][0], y = c[qi][i][1];
			/* Atributo en (x, y): por la arista mayor hasta y, luego en x. */
			float e = y - y0, d = x - (xh + dxh * e);
			float a[4], t[4];
			for (int k = 0; k < 4; k++) {
				a[k] = sh.v[k] + sh.de[k] * e + sh.dx[k] * d;
				t[k] = tx.v[k] + tx.de[k] * e + tx.dx[k] * d;
			}
			/* S y T en s10.21: texels = valor / 32; con perspectiva, ÷ W
			 * (W ≈ 1 en 0x7FFF.0000) y la w de salida es 1/W. */
			float wn = persp ? t[2] * (1.0f / 32768.0f) : 1.0f;
			if (wn < 1e-6f)
				wn = 1e-6f;
			float ww = 1.0f / wn;
			struct gfx_vertex *o = &v[i];
			o->w = ww;
			o->x = x * ww;
			o->y = y * ww;
			/* Con G_ZS_PRIM el RDP no usa la z del triángulo. */
			float zz = !zb ? 0.0f
				   : (g->ol & G_ZS_PRIM) ? (float)g->prim_z / 32767.0f
							 : (z + dzde * e + dzdx * d) * (1.0f / 32768.0f);
			o->z = (zz < 0.0f ? 0.0f : zz > 1.0f ? 1.0f : zz) * ww;
			o->s = t[0] / 32.0f * ww;
			o->t = t[1] / 32.0f * ww;
			uint8_t col[4];
			for (int k = 0; k < 4; k++) {
				float f = a[k] < 0.0f ? 0.0f : a[k] > 255.0f ? 255.0f : a[k];
				col[k] = (uint8_t)(f + 0.5f);
			}
			o->r = col[0], o->g = col[1], o->b = col[2], o->a = col[3];
			o->pad = 0;
		}
		static const int order[6] = { 0, 1, 2, 0, 2, 3 };
		enum prim_kind kind = (enum prim_kind)(PRIM_RAW | (tex ? 1 : 0) | (zb ? 2 : 0));
		struct gfx_vertex *o = begin_prim(g, kind, tile, 6);
		if (!o)
			return;
		g->res->triangles += 2;
		for (int i = 0; i < 6; i++)
			o[i] = v[order[i]];
	}
}

static void fillrect(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	unsigned cycle = (g->oh >> G_MDSFT_CYCLETYPE) & 3;
	float lrx = (float)((w0 >> 14) & 0x3FF), lry = (float)((w0 >> 2) & 0x3FF);
	float ulx = (float)((w1 >> 14) & 0x3FF), uly = (float)((w1 >> 2) & 0x3FF);
	if (cycle == G_CYC_FILL || cycle == G_CYC_COPY) {
		lrx += 1.0f;
		lry += 1.0f;
	} else {
		if (lry == uly)
			lry += 1.0f;
		/* En 1 y 2 ciclos el borde inferior derecho es exclusivo, pero hay
		 * juegos que lo mandan inclusivo (el fundido de Zelda: 319, 239) y
		 * en la consola la columna y la fila que faltan quedan fuera de la
		 * imagen visible de la tele. Aquí se ven: si el rectángulo se queda
		 * a un píxel del scissor, llega hasta él. */
		if ((uint32_t)lrx * 4 + 4 == g->scissor[2])
			lrx += 1.0f;
		if ((uint32_t)lry * 4 + 4 == g->scissor[3])
			lry += 1.0f;
	}
	if (cycle == G_CYC_FILL && g->cimg_addr == g->zimg_addr) {
		/* Borrar el z-buffer: se dibuja en la color image que apunta a él.
		 * El RDP recorta el relleno con el scissor (10.2, borde inferior
		 * derecho exclusivo) y este rectángulo también va a la RDRAM: Conker
		 * manda 217 filas con un scissor de 216 y guarda una lista enlazada
		 * en la fila 216, que el RDP no toca. */
		uint32_t x0 = (uint32_t)ulx, y0 = (uint32_t)uly, x1 = (uint32_t)lrx, y1 = (uint32_t)lry;
		uint32_t sx0 = g->scissor[0] >> 2, sy0 = g->scissor[1] >> 2;
		uint32_t sx1 = (g->scissor[2] + 3u) >> 2, sy1 = (g->scissor[3] + 3u) >> 2;
		x0 = x0 > sx0 ? x0 : sx0;
		y0 = y0 > sy0 ? y0 : sy0;
		x1 = x1 < sx1 ? x1 : sx1;
		y1 = y1 < sy1 ? y1 : sy1;
		if (x0 >= x1 || y0 >= y1)
			return;
		struct gfx_zclear z;
		memset(&z, 0, sizeof z);
		z.h.op = GFX_OP_ZCLEAR;
		z.h.size = sizeof z;
		z.h.a = g->zimg_addr;
		z.h.b = g->fill & 0xFFFF;
		z.h.c = (uint32_t)g->cimg_siz << 16 | g->cimg_width;
		z.rect[0] = (uint16_t)x0;
		z.rect[1] = (uint16_t)y0;
		z.rect[2] = (uint16_t)x1;
		z.rect[3] = (uint16_t)y1;
		z.fill = g->fill;
		gfx_flush_draw(g);
		gfx_io_cmd(g, &z, sizeof z);
		g->res->rects++;
		return;
	}
	rect(g, cycle == G_CYC_FILL ? PRIM_FILL : PRIM_RECT, 0, ulx, uly, lrx, lry, NULL, NULL, 3, NULL);
}

static void set_image(uint32_t w0, uint8_t *fmt, uint8_t *siz, uint16_t *width)
{
	*fmt = (w0 >> 21) & 7;
	*siz = (w0 >> 19) & 3;
	*width = (uint16_t)((w0 & 0xFFF) + 1);
}

GFX_IMG1_COLD void gfx_rdp_command(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	uint32_t op = w0 >> 24;
	switch (op) {
	case G_RDPFULLSYNC:
		g->res->flags |= GFX_RES_FULLSYNC;
		return;
	case G_RDPLOADSYNC:
	case G_RDPPIPESYNC:
	case G_RDPTILESYNC:
	case G_SETKEYGB:
	case G_SETKEYR:
		return;
	case G_SETCONVERT:
		/* K0–K3 convierten YUV en el filtro; K4 y K5 quedan en el
		 * combiner. Otra matriz invalida también las tiles resueltas. */
		g->k0 = (uint16_t)((w0 >> 13) & 0x1FF);
		g->k1 = (uint16_t)((w0 >> 4) & 0x1FF);
		g->k2 = (uint16_t)(((w0 & 15) << 5) | (w1 >> 27));
		g->k3 = (uint16_t)((w1 >> 18) & 0x1FF);
		g->k4 = (uint16_t)((w1 >> 9) & 0x1FF);
		g->k5 = (uint16_t)(w1 & 0x1FF);
		g->tex_gen++;
		break;
	case G_SETSCISSOR:
		g->scissor[0] = (w0 >> 12) & 0xFFF;
		g->scissor[1] = w0 & 0xFFF;
		g->scissor[2] = (w1 >> 12) & 0xFFF;
		g->scissor[3] = w1 & 0xFFF;
		break;
	case G_SETPRIMDEPTH: {
		/* La z del primitivo es de 15 bits: el RDP ignora el de arriba
		 * (Smash la pone en 0x9000 para su fondo). */
		uint16_t z = (uint16_t)(w1 >> 16) & 0x7FFF;
		if ((g->ol & G_ZS_PRIM) && g->prim_z != z)
			g->dirty = true;  /* otro dibujo: una z por dibujo */
		g->prim_z = z;
		g->prim_dz = (uint16_t)w1;
		break;
	}
	case G_RDPSETOTHERMODE:
		g->oh = w0 & 0x00FFFFFF;
		g->ol = w1;
		/* La segunda generación sí sigue al RDP con el 0xEF, pero dentro de la tarea: la copia
		 * arranca en 0 con cada una (Castlevania: el Z_SOURCE_SEL que el RDP traía de la tarea
		 * anterior se colaba en el primer G_SETOTHERMODE_L y todo el escenario salía con una
		 * sola z). */
#if !defined(GFX_SPE0_IMG) || (GFX_SPE0_IMG != 0 && GFX_SPE0_IMG != 4)
		/* No va en las imágenes 0 (primera generación: sp_om_shadow siempre vale 1) ni 4
		 * (Indiana Jones no usa G_SETOTHERMODE parciales: sin lugar de pila de sobra). */
		if (!g->sp_om_shadow)
			g->sp_ol = g->ol;
#endif
		break;
	case G_LOADTLUT: {
		uint32_t t0 = gfx_io_clock();
		gfx_load_tlut(g, w0, w1);
		g->res->t_load += gfx_io_clock() - t0;
		break;
	}
	case G_SETTILESIZE: {
		struct gfx_tile *t = &g->tiles[(w1 >> 24) & 7];
		g->tex_gen++;
		t->uls = (w0 >> 12) & 0xFFF;
		t->ult = w0 & 0xFFF;
		t->lrs = (w1 >> 12) & 0xFFF;
		t->lrt = w1 & 0xFFF;
		break;
	}
	case G_LOADBLOCK:
	case G_LOADTILE: {
		uint32_t t0 = gfx_io_clock();
		if (op == G_LOADBLOCK)
			gfx_load_block(g, w0, w1);
		else
			gfx_load_tile(g, w0, w1);
		g->res->t_load += gfx_io_clock() - t0;
		break;
	}
	case G_SETTILE: {
		struct gfx_tile *t = &g->tiles[(w1 >> 24) & 7];
		g->tex_gen++;
		t->fmt = (w0 >> 21) & 7;
		t->siz = (w0 >> 19) & 3;
		t->line = (uint16_t)(((w0 >> 9) & 0x1FF) * 8);
		t->tmem = (uint16_t)((w0 & 0x1FF) * 8);
		t->pal = (w1 >> 20) & 0xF;
		t->cmt = (w1 >> 18) & 3;
		t->maskt = (w1 >> 14) & 0xF;
		t->shiftt = (w1 >> 10) & 0xF;
		t->cms = (w1 >> 8) & 3;
		t->masks = (w1 >> 4) & 0xF;
		t->shifts = w1 & 0xF;
		break;
	}
	case G_FILLRECT:
		fillrect(g, w0, w1);
		return;
	case G_SETFILLCOLOR:
		g->fill = w1;
		break;
	case G_SETFOGCOLOR:
		g->fog = w1;
		break;
	case G_SETBLENDCOLOR:
		g->blend = w1;
		break;
	case G_SETPRIMCOLOR:
		g->prim = w1;
		g->prim_min = (uint8_t)(w0 >> 8);
		g->prim_lod = (uint8_t)w0;
		break;
	case G_SETENVCOLOR:
		g->env = w1;
		break;
	case G_SETCOMBINE:
		g->cc_hi = w0 & 0x00FFFFFF;
		g->cc_lo = w1;
		break;
	case G_SETTIMG:
		set_image(w0, &g->timg_fmt, &g->timg_siz, &g->timg_width);
		g->timg_addr = gfx_addr(g, w1);
		return;
	case G_SETZIMG:
		g->zimg_addr = gfx_addr(g, w1);
		break;
	case G_SETCIMG: {
		uint32_t addr = gfx_addr(g, w1);
		uint8_t fmt, siz;
		uint16_t width;
		set_image(w0, &fmt, &siz, &width);
		if (addr != g->cimg_addr || width != g->cimg_width || siz != g->cimg_siz) {
			gfx_flush_draw(g);
			g->target_sent = false;
			g->fbimg_cur = -1;
		}
		g->cimg_addr = addr;
		g->cimg_fmt = fmt;
		g->cimg_siz = siz;
		g->cimg_width = width;
		break;
	}
	default:
		if (op >= RDP_TRI_FIRST && op <= RDP_TRI_LAST) {
			/* Triángulos crudos del RDP: no los genera F3DEX2. */
			g->res->unknown_cmds++;
			g->res->last_unknown = op;
			GFX_UNKNOWN(op);
		}
		return;
	}
	g->dirty = true;
}
