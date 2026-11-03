/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — TMEM del RDP: cargas desde la RDRAM y decodificación de una
 * tile a RGBA8888 para el RSX.
 *
 * La TMEM son 4 KB en palabras de 64 bits. En las filas impares de una
 * textura las dos mitades de 32 bits de cada palabra van intercambiadas
 * (LOADTILE lo hace por fila; LOADBLOCK por el contador dxt). Las texturas
 * de 32 bits se parten: R y G en la mitad baja, B y A en la alta (+2 KB).
 * Las paletas (LOADTLUT) van en la mitad alta, cada entrada de 16 bits
 * repetida cuatro veces (8 bytes por entrada).
 *
 * Fuentes: n64brew.dev (Reality Display Processor/Commands: Load Block,
 * Load Tile, Load TLUT, Set Tile; TMEM) y el comportamiento descrito por
 * GLideN64 (docs/THIRD_PARTY.md). Código propio.
 *
 * Caché: la textura de una tile se identifica por un hash de los bytes de
 * la TMEM que lee su decodificación (texels y paleta) y de sus parámetros
 * (texcache.c). La misma identidad reutiliza la textura ya decodificada, en
 * la misma tarea o en una anterior; una tile que no cambió desde la última
 * vez (sin cargas ni cambios de tiles) ni siquiera se vuelve a calcular.
 */
#include <string.h>

#include "core/endian.h"
#include "rsp/gfx/hle.h"


#define TMEM_MASK 0xFFFu

/* Mitad de una palabra de 64 bits de la TMEM (alineada a 8), y un texel
 * de 16 bits: lecturas alineadas (en el SPE, sin armar byte por byte). */
typedef uint32_t __attribute__((may_alias, aligned(4))) tword;
typedef uint16_t __attribute__((may_alias, aligned(2))) thalf;

static inline uint32_t tmem16(const uint8_t *tmem, uint32_t addr)
{
	return SALSA64_BE16(*(const thalf *)(const void *)(tmem + addr));
}

static inline void swap_halves(uint8_t *tmem, uint32_t addr)
{
	tword *q = (tword *)(void *)(tmem + (addr & TMEM_MASK & ~7u));
	uint32_t a = q[0];
	q[0] = q[1];
	q[1] = a;
}

/* `bytes` (múltiplo de 8) a la TMEM desde `dst` (alineado a 8); lo que pasa
 * de los 4 KB sigue desde el principio. */
static void tmem_copy(uint8_t *tmem, uint32_t dst, const uint8_t *src, uint32_t bytes)
{
	dst &= TMEM_MASK;
	uint32_t n = 4096 - dst < bytes ? 4096 - dst : bytes;
	memcpy(tmem + dst, src, n);
	memcpy(tmem, src + n, bytes - n);
}

/* Bytes por texel ×2 (los de 4 bits ocupan medio byte). */
static uint32_t half_bytes(unsigned siz)
{
	return siz == G_IM_SIZ_4b ? 1 : siz == G_IM_SIZ_8b ? 2 : siz == G_IM_SIZ_16b ? 4 : 8;
}

/* Texel de 32 bits a las dos mitades de la TMEM; idx en unidades de 16 bits. */
static void put32(uint8_t *tmem, uint32_t idx, const uint8_t *src)
{
	idx &= 0x3FF;
	memcpy(tmem + idx * 2, src, 2);
	memcpy(tmem + 0x800 + idx * 2, src + 2, 2);
}

/* YUV16 entra como U,Y0,V,Y1: la crominancia va a los 2 KB bajos y
 * la luminancia a los altos. La línea de la tile cuenta bytes por plano. */
static void put_yuv(uint8_t *tmem, uint32_t dst, uint32_t odd, const uint8_t *src, uint32_t bytes)
{
	for (uint32_t x = 0; x < bytes / 2; x++) {
		uint32_t a = ((dst + x) ^ odd) & 0x7FF;
		tmem[a] = src[2 * x];
		tmem[a + 0x800] = src[2 * x + 1];
	}
}

/* Anota una carga: el tramo [tmem, tmem + bytes) de la TMEM vino de `addr`
 * en filas de `line` bytes, de una imagen con `row_bytes` por fila. */
#ifdef SALSA64_GFX_TRACE
/* Depuración en el host (src/host/run.c): cada carga a la TMEM. */
void salsa64_gfx_trace_load(const struct gfx_state *g, uint32_t addr, uint32_t bytes, uint32_t line);
#endif

static void note_load(struct gfx_state *g, uint32_t tmem, uint32_t bytes, uint32_t line, uint32_t addr,
		      uint32_t row_bytes)
{
#ifdef SALSA64_GFX_TRACE
	salsa64_gfx_trace_load(g, addr, bytes, line);
#endif
	struct gfx_load *l = &g->loads[g->load_next++ & (GFX_LOADS - 1)];
	l->addr = addr;
	l->row_bytes = row_bytes;
	l->tmem = (uint16_t)(tmem & TMEM_MASK);
	l->bytes = (uint16_t)(bytes > 4096 ? 4096 : bytes);
	l->line = (uint16_t)line;  /* 0: LOADBLOCK, texels seguidos */
	l->pitch = g->timg_width;
	l->siz = g->timg_siz;
	l->fmt = g->timg_fmt;
}

/* El origen de una textura: los campos src_* de gfx_tex, que van seguidos
 * (8 bytes), para armarlo aparte de la textura. */
struct tex_src {
	uint32_t src_addr;
	uint16_t src_pitch;
	uint8_t src_siz, src_fmt;
} __attribute__((aligned(8)));
_Static_assert(sizeof(struct tex_src) == 8 &&
		       offsetof(struct gfx_tex, src_fmt) + 1 - offsetof(struct gfx_tex, src_addr) == 8,
	       "el origen de una textura son 8 bytes seguidos");

/* De qué carga lee la tile (la más reciente que escribió su inicio en la
 * TMEM con las mismas filas) y dónde está en la RDRAM su texel (0, 0).
 * `rows`: filas de la textura que muestrea el dibujo. */
static void provenance(const struct gfx_state *g, const struct gfx_tile *t, uint32_t rows, struct tex_src *tex)
{
	for (uint32_t k = 1; k <= GFX_LOADS; k++) {
		const struct gfx_load *l = &g->loads[(g->load_next - k) & (GFX_LOADS - 1)];
		/* libultra carga las texturas de 4 y 8 bits como de 16 (LOADBLOCK
		 * cuenta texels de 16): el tamaño que vale es el de la tile que lee. */
		if (!l->bytes || !t->line || (l->line != 0 && l->siz != t->siz))
			continue;
		if (t->tmem < l->tmem || t->tmem >= (uint32_t)l->tmem + l->bytes)
			continue;
		uint32_t off = t->tmem - l->tmem;
		if (l->line == 0) {
			/* LOADBLOCK: texels seguidos (libultra manda ancho 1 en SETTIMG):
			 * las filas son las de la tile que lee, y el paso de la imagen
			 * de origen, el ancho de esa fila. Si el dibujo muestrea una
			 * sola fila, el paso no cambia ningún texel que lea: vale el
			 * ancho de la imagen que declaró SETTIMG (con el que el RDP
			 * ubica la fila de la carga), si lo declaró. Perfect Dark copia
			 * el cuadro anterior fila por fila con tiles de 1280 bytes por
			 * línea sobre un framebuffer de 320: con el de la tile, la
			 * franja no caía en el framebuffer (paso 640). */
			tex->src_addr = l->addr + off;
			tex->src_pitch = (uint16_t)(((uint32_t)t->line * 2) >> t->siz);  /* half_bytes(siz) = 1 << siz */
			if (rows == 1 && l->pitch > 1)
				tex->src_pitch = (uint16_t)(((uint32_t)l->pitch << (l->siz & 3)) >> t->siz);
		} else {
			if (l->line != t->line)
				continue;
			tex->src_addr = l->addr + (off / l->line) * l->row_bytes + off % l->line;
			tex->src_pitch = l->pitch;
		}
		tex->src_siz = l->line == 0 ? t->siz : l->siz;
		tex->src_fmt = l->fmt;
		return;
	}
}

void gfx_load_block(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	struct gfx_tile *t = &g->tiles[(w1 >> 24) & 7];
	uint32_t uls = (w0 >> 12) & 0xFFF, ult = w0 & 0xFFF;
	uint32_t lrs = (w1 >> 12) & 0xFFF, dxt = w1 & 0xFFF;
	if (lrs < uls)
		return;
	uint32_t texels = lrs - uls + 1;
	uint32_t hb = half_bytes(g->timg_siz);
	uint32_t src = g->timg_addr + ((ult * g->timg_width + uls) * hb) / 2;
	uint32_t bytes = (texels * hb / 2 + 7) & ~7u;
	if (bytes > 4096)
		bytes = 4096;
	g->tex_gen++;
	g->res->loads++;
	g->res->load_reads++;
	const uint8_t *p = gfx_io_read(g, GFX_IO_TEX, src, bytes);
	if (!p) {
		g->res->dl_errors++;
		return;
	}
	/* Los texels van seguidos: línea 0 (la tile que lea decide las filas). */
	note_load(g, t->tmem, bytes, 0, src, 0);
	if (t->fmt == G_IM_FMT_YUV) {
		for (uint32_t k = 0; k < bytes / 8; k++)
			put_yuv(g->tmem, t->tmem + 4 * k, ((k * dxt) >> 11) & 1 ? 4 : 0, p + 8 * k, 8);
		return;
	}
	if (g->timg_siz == G_IM_SIZ_32b) {
		uint32_t base = t->tmem / 2;
		for (uint32_t k = 0; k < bytes / 8; k++) {
			uint32_t x = ((k * dxt) >> 11) & 1 ? 2 : 0;
			put32(g->tmem, (base + 2 * k) ^ x, p + 8 * k);
			put32(g->tmem, (base + 2 * k + 1) ^ x, p + 8 * k + 4);
		}
		return;
	}
	tmem_copy(g->tmem, t->tmem, p, bytes);
	if (dxt)
		for (uint32_t k = 0; k < bytes / 8; k++)
			if (((k * dxt) >> 11) & 1)
				swap_halves(g->tmem, t->tmem + 8 * k);
}

void gfx_load_tile(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	struct gfx_tile *t = &g->tiles[(w1 >> 24) & 7];
	g->tex_gen++;
	t->uls = (w0 >> 12) & 0xFFF;
	t->ult = w0 & 0xFFF;
	t->lrs = (w1 >> 12) & 0xFFF;
	t->lrt = w1 & 0xFFF;
	uint32_t x0 = t->uls >> 2, y0 = t->ult >> 2, x1 = t->lrs >> 2, y1 = t->lrt >> 2;
	if (x1 < x0 || y1 < y0)
		return;
	uint32_t width = x1 - x0 + 1, rows = y1 - y0 + 1;
	uint32_t hb = half_bytes(g->timg_siz);
	uint32_t row_bytes = (width * hb / 2 + 7) & ~7u;
	if (row_bytes > 4096)
		row_bytes = 4096;
	g->res->loads++;
	note_load(g, t->tmem, rows * t->line, t->line, g->timg_addr + ((y0 * g->timg_width + x0) * hb) / 2,
		  (uint32_t)g->timg_width * hb / 2);

	for (uint32_t r = 0; r < rows; r++) {
		uint32_t src = g->timg_addr + (((y0 + r) * g->timg_width + x0) * hb) / 2;
		g->res->load_reads++;
		const uint8_t *p = gfx_io_read(g, GFX_IO_TEX, src, row_bytes);
		if (!p) {
			g->res->dl_errors++;
			return;
		}
		if (t->fmt == G_IM_FMT_YUV) {
			put_yuv(g->tmem, t->tmem + r * t->line, (r & 1) ? 4 : 0, p, row_bytes);
			continue;
		}
		if (g->timg_siz == G_IM_SIZ_32b) {
			uint32_t base = t->tmem / 2 + r * (t->line / 2);
			uint32_t x = (r & 1) ? 2 : 0;
			for (uint32_t j = 0; j < width; j++)
				put32(g->tmem, (base + j) ^ x, p + 4 * j);
			continue;
		}
		uint32_t dst = t->tmem + r * t->line;
		tmem_copy(g->tmem, dst, p, row_bytes);
		if (r & 1)
			for (uint32_t k = 0; k < row_bytes; k += 8)
				swap_halves(g->tmem, dst + k);
	}
}

void gfx_load_tlut(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	struct gfx_tile *t = &g->tiles[(w1 >> 24) & 7];
	uint32_t uls = ((w0 >> 12) & 0xFFF) >> 2, ult = (w0 & 0xFFF) >> 2;
	uint32_t lrs = ((w1 >> 12) & 0xFFF) >> 2;
	if (lrs < uls)
		return;
	uint32_t count = lrs - uls + 1;
	if (count > 256)
		count = 256;
	uint32_t src = g->timg_addr + (ult * g->timg_width + uls) * 2;
	g->tex_gen++;
	g->res->loads++;
	g->res->load_reads++;
	const uint8_t *p = gfx_io_read(g, GFX_IO_TEX, src, count * 2);
	if (!p) {
		g->res->dl_errors++;
		return;
	}
	for (uint32_t e = 0; e < count; e++) {
		uint32_t v;
		memcpy(&v, p + 2 * e, 2);
		memcpy((uint8_t *)&v + 2, p + 2 * e, 2);
		tword *q = (tword *)(void *)(g->tmem + (0x800 | ((t->tmem + 8 * e) & 0x7FF)));
		q[0] = v;
		q[1] = v;
	}
}

void gfx_tile_size(const struct gfx_tile *t, uint32_t *w, uint32_t *h)
{
	uint32_t tw = t->lrs >= t->uls ? ((t->lrs - t->uls) >> 2) + 1 : 1;
	uint32_t th = t->lrt >= t->ult ? ((t->lrt - t->ult) >> 2) + 1 : 1;
	*w = t->masks && !(t->cms & 2) ? 1u << (t->masks > 10 ? 10 : t->masks) : tw;
	*h = t->maskt && !(t->cmt & 2) ? 1u << (t->maskt > 10 ? 10 : t->maskt) : th;
	if (*w > 1024)
		*w = 1024;
	if (*h > 1024)
		*h = 1024;
}

static uint32_t expand5(uint32_t v)
{
	return v << 3 | v >> 2;
}

static uint32_t rgba16(uint32_t p)
{
	return expand5(p >> 11) << 24 | expand5((p >> 6) & 31) << 16 | expand5((p >> 1) & 31) << 8 |
	       ((p & 1) ? 0xFF : 0);
}

/* Cuatro texels a la vez con los vectores de GCC y clang: el mismo código
 * en el host, el PPE y el SPE (allí, una instrucción por operación para los
 * cuatro), así que las pruebas del host cubren la aritmética. */
typedef uint32_t v4u __attribute__((vector_size(16)));

/* rgba16() de cada carril: cada canal de 5 bits a 8 (v << 3 | v >> 2),
 * sin ramas. */
static inline v4u rgba16x4(v4u p)
{
	return (p & 0xF800) << 16 | (p & 0xE000) << 11 | (p & 0x07C0) << 13 | (p & 0x0700) << 8 |
	       (p & 0x003E) << 10 | (p & 0x0038) << 5 | ((0 - (p & 1)) & 0xFF);
}

static uint32_t ia16(uint32_t p)
{
	uint32_t i = p >> 8;
	return i << 24 | i << 16 | i << 8 | (p & 0xFF);
}

static uint32_t gray(uint32_t i, uint32_t a)
{
	return i << 24 | i << 16 | i << 8 | a;
}

/* Entrada de la paleta: 8 bytes por entrada desde 0x800. */
static uint32_t tlut(const struct gfx_state *g, uint32_t index, unsigned type)
{
	uint32_t p = tmem16(g->tmem, 0x800 + ((index * 8) & 0x7FF));
	return type == G_TT_IA16 ? ia16(p) : rgba16(p);
}

static uint32_t color8(int32_t v)
{
	return v < 0 ? 0 : v > 255 ? 255 : (uint32_t)v;
}

/* El filtro usa los coeficientes de nueve bits con signo de SETCONVERT,
 * con un bit fraccionario bajo implícito. K4/K5 los aplica el combiner
 * después; aplicarlos aquí también daría la conversión dos veces. */
static int32_t yuv_k(uint16_t k)
{
	return 2 * (((int32_t)k ^ 256) - 256) + 1;
}

static uint32_t yuv16(const struct gfx_state *g, uint32_t row, uint32_t odd, uint32_t x)
{
	if (g->job->flags & GFX_JOB_NO_YUV)
		return 0x000000FF;
	uint32_t a = ((row + x) ^ odd) & 0x7FF;
	int32_t y = g->tmem[a + 0x800];
	int32_t u = (int32_t)g->tmem[a & ~1u] - 128;
	int32_t v = (int32_t)g->tmem[a | 1u] - 128;
	int32_t r = y + ((yuv_k(g->k0) * v + 128) >> 8);
	int32_t b = y + ((yuv_k(g->k3) * u + 128) >> 8);
	int32_t green = y + ((yuv_k(g->k1) * u + yuv_k(g->k2) * v + 128) >> 8);
	return color8(r) << 24 | color8(green) << 16 | color8(b) << 8 | (uint32_t)y;
}

/* Texel (x, y) de la tile en RGBA8888 (0xRRGGBBAA). */
static uint32_t texel(const struct gfx_state *g, const struct gfx_tile *t, uint32_t x, uint32_t y, unsigned tt)
{
	uint32_t odd = (y & 1) ? 4 : 0;
	uint32_t row = t->tmem + (y & 0xFF) * t->line;
	switch (t->siz) {
	case G_IM_SIZ_4b: {
		uint8_t b = g->tmem[((row + x / 2) ^ odd) & TMEM_MASK];
		uint32_t n = (x & 1) ? b & 15 : b >> 4;
		if (tt != G_TT_NONE)
			return tlut(g, (uint32_t)t->pal << 4 | n, tt);
		switch (t->fmt) {
		case G_IM_FMT_IA: {
			uint32_t i = n >> 1;
			return gray(i << 5 | i << 2 | i >> 1, (n & 1) ? 0xFF : 0);
		}
		case G_IM_FMT_CI: {
			uint32_t i = (uint32_t)t->pal << 4 | n;
			return gray(i, i);
		}
		default:
			return gray(n << 4 | n, n << 4 | n);
		}
	}
	case G_IM_SIZ_8b: {
		uint32_t b = g->tmem[((row + x) ^ odd) & TMEM_MASK];
		if (tt != G_TT_NONE)
			return tlut(g, b, tt);
		if (t->fmt == G_IM_FMT_IA)
			return gray((b & 0xF0) | b >> 4, (b & 15) << 4 | (b & 15));
		return gray(b, b);
	}
	case G_IM_SIZ_16b: {
		uint32_t p = tmem16(g->tmem, ((row + 2 * x) ^ odd) & TMEM_MASK & ~1u);
		if (t->fmt == G_IM_FMT_IA || t->fmt == G_IM_FMT_I)
			return ia16(p);
		if (t->fmt == G_IM_FMT_YUV)
			return yuv16(g, row, odd, x);
		return rgba16(p);
	}
	default: {
		uint32_t idx = ((t->tmem / 2 + (y & 0xFF) * (t->line / 2) + x) ^ (odd ? 2 : 0)) & 0x3FF;
		uint32_t rg = tmem16(g->tmem, idx * 2), ba = tmem16(g->tmem, 0x800 + idx * 2);
		return rg << 16 | ba;
	}
	}
}

/* Hash de lo que lee texel() para la tile: las filas de texels (con la
 * paleta, si se usa) y los parámetros que cambian el resultado. */
/* Con clamp y una máscara más chica que la tile, el RDP limita la
 * coordenada al rango de la tile y después la enmascara (con espejo si
 * cms/cmt lo pide): la textura de `n` texels repite los primeros
 * 1 << máscara. La túnica de Link en OoT es una I8 de 16 de ancho en una
 * tile de 32 con máscara 4, espejo y clamp: el pliegue y su reflejo.
 * Devuelve la máscara que hay que aplicar al decodificar (0: ninguna). */
static unsigned clamp_mask(unsigned mask, unsigned cm, uint32_t n)
{
	if (mask > 10)
		mask = 10;
	return mask && (cm & 2) && n > (1u << mask) ? mask : 0;
}

static uint32_t masked(uint32_t v, unsigned mask, unsigned cm)
{
	if ((cm & 1) && ((v >> mask) & 1))
		v = ~v;
	return v & ((1u << mask) - 1);
}

static void tile_key(const struct gfx_state *g, const struct gfx_tile *t, uint32_t w, uint32_t h, unsigned tt,
		     bool premul, uint8_t once, uint32_t key[2])
{
	struct gfx_hash hs;
	gfx_hash_init(&hs);
	/* Versión de la decodificación (1), máscaras dentro del clamp y si va
	 * premultiplicada. */
	unsigned ms = clamp_mask(t->masks, t->cms, w), mt = clamp_mask(t->maskt, t->cmt, h);
	uint32_t masks = ms ? ms | (t->cms & 1u) << 4 : 0;
	masks |= mt ? (mt | (t->cmt & 1u) << 4) << 8 : 0;
	masks |= (uint32_t)once << 16;  /* un solo periodo decodificado (GFX_TEX_ONCE_*, GFX_WRAP_* por eje) */
	gfx_hash_words(&hs, (uint32_t)t->fmt << 24 | (uint32_t)t->siz << 16 | (uint32_t)t->pal << 8 | tt,
		       (uint32_t)t->line << 16 | (t->tmem & 15), w << 16 | h, 1u | masks << 8 | (uint32_t)premul << 31);
	/* texel() lee por fila 8 bytes enteros (la fila impar intercambia
	 * mitades dentro de cada palabra) y solo 256 filas distintas. */
	bool yuv = t->fmt == G_IM_FMT_YUV && t->siz == G_IM_SIZ_16b;
	uint32_t row = t->siz == G_IM_SIZ_4b ? (w + 1) / 2 : t->siz == G_IM_SIZ_8b || yuv ? w : 2 * w;
	uint32_t rows = h < 256 ? h : 256;
	uint32_t len = (rows - 1) * t->line + ((row + 7) & ~7u);
	if (t->siz == G_IM_SIZ_32b || yuv) {
		/* R y G en la mitad baja, B y A en la alta, en el mismo lugar. */
		gfx_hash_ring(&hs, g->tmem, 0x800, t->tmem, len);
		gfx_hash_ring(&hs, g->tmem + 0x800, 0x800, t->tmem, len);
	} else {
		gfx_hash_ring(&hs, g->tmem, 4096, t->tmem, len);
	}
	if (yuv)
		gfx_hash_words(&hs, (uint32_t)g->k0 << 16 | g->k1, (uint32_t)g->k2 << 16 | g->k3,
			       g->job->flags & GFX_JOB_NO_YUV, 0);
	if (tt != G_TT_NONE) {
		/* Paleta: el banco de la tile (4 bits) o toda (8 bits). */
		if (t->siz == G_IM_SIZ_4b)
			gfx_hash_ring(&hs, g->tmem, 4096, 0x800 + (uint32_t)t->pal * 128, 128);
		else
			gfx_hash_ring(&hs, g->tmem, 4096, 0x800, 0x800);
	}
	gfx_hash_final(&hs, key);
}

/* RGB·A (0xRRGGBBAA), redondeado. */
static uint32_t premultiply(uint32_t c)
{
	uint32_t a = c & 0xFF;
	uint32_t r = ((c >> 24) * a + 127) / 255, g = (((c >> 16) & 0xFF) * a + 127) / 255;
	uint32_t b = (((c >> 8) & 0xFF) * a + 127) / 255;
	return r << 24 | g << 16 | b << 8 | a;
}

/* Cuatro texels seguidos en la salida (alineada a 16), en el orden de bytes
 * de la N64. En el SPE y el PPE, un solo almacenamiento de 16 bytes (en el
 * SPE, uno por texel lee y reescribe el mismo quadword cuatro veces). */
static inline void put4v(uint8_t *o, v4u c)
{
#if SALSA64_HOST_BIG_ENDIAN
	*(v4u *)(void *)o = c;
#else
	tword *q = (tword *)(void *)o;
	for (int i = 0; i < 4; i++)
		q[i] = SALSA64_BE32(c[i]);
#endif
}

static inline void put4(uint8_t *o, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	put4v(o, (v4u){ a, b, c, d });
}

/* Una fila de una tile de 16 bits RGBA (o CI sin paleta, que se lee igual)
 * desde el byte `row` de la TMEM: de palabra en palabra de 64 bits, cuatro
 * texels por vuelta; en las filas impares las mitades van intercambiadas.
 * Es lo que más se decodifica en cada cuadro (copias del framebuffer). */
static void row_rgba16(const uint8_t *tmem, uint32_t row, uint32_t odd, uint32_t w, uint8_t *o)
{
	for (uint32_t x = 0; x < w; x += 4, row += 8, o += 16) {
		const tword *q = (const tword *)(const void *)(tmem + (row & TMEM_MASK));
		uint32_t a = SALSA64_BE32(q[odd]), b = SALSA64_BE32(q[odd ^ 1]);
		put4v(o, rgba16x4((v4u){ a >> 16, a & 0xFFFF, b >> 16, b & 0xFFFF }));
	}
}

/* La salida tiene `pitch` bytes por fila, múltiplo de 64: se escribe de 4
 * en 4 texels, hasta 3 de más al final de cada fila (no se muestrean). */
static void decode(const struct gfx_state *g, const struct gfx_tile *t, uint32_t w, uint32_t h, uint32_t pitch,
		   uint32_t x0, uint32_t y0, unsigned tt, bool premul, uint8_t *out)
{
	/* (x0, y0): primer texel de la tile que se decodifica (el segundo
	 * periodo de un eje con espejo y clamp: resolve_content). */
	unsigned ms = clamp_mask(t->masks, t->cms, w + x0), mt = clamp_mask(t->maskt, t->cmt, h + y0);
	bool rgba = t->siz == G_IM_SIZ_16b && t->fmt != G_IM_FMT_IA && t->fmt != G_IM_FMT_I &&
		    t->fmt != G_IM_FMT_YUV && !premul && !ms;
	for (uint32_t y = 0; y < h; y++) {
		uint8_t *row = out + y * pitch;
		uint32_t ty = mt ? masked(y + y0, mt, t->cmt) : y;
		if (rgba) {
			row_rgba16(g->tmem, t->tmem + (ty & 0xFF) * t->line, ty & 1, w, row);
			continue;
		}
		for (uint32_t x = 0; x < w; x += 4) {
			uint32_t c[4];
			for (int i = 0; i < 4; i++) {
				uint32_t tx = ms ? masked(x + (uint32_t)i + x0, ms, t->cms) : x + (uint32_t)i;
				c[i] = texel(g, t, tx, ty, tt);
				if (premul)
					c[i] = premultiply(c[i]);
			}
			put4(row + 4 * x, c[0], c[1], c[2], c[3]);
		}
	}
}

static bool resolve_content(struct gfx_state *g, const struct gfx_tile *t, unsigned tt, bool premul, uint32_t w,
			    uint32_t h, uint32_t x0, uint32_t y0, uint8_t once, struct gfx_tex *tex);

/* Tamaño de la textura de la tile: lo que el dibujo no puede muestrear no
 * se decodifica (Perfect Dark copia el cuadro anterior en franjas de 320 × 1
 * con tiles de 513 × 9), ni más filas de las que caben en el área de
 * decodificación. Una tile con espejo y clamp de hasta dos periodos de su
 * máscara que no cabe en GFX_TEX_MAX (el lente del telescopio de Majora's
 * Mask: una I4 de 64 × 64 en una tile de 128 × 128, 64 KB en RGBA) se
 * decodifica en un solo periodo, el segundo (el reflejado), desde el texel
 * (x0, y0), y el eje va con GFX_WRAP_MIRROR_ONCE (once: GFX_TEX_ONCE_*): el
 * RSX refleja la coordenada una vez y fija el borde más allá, como el RDP.
 * Una tile con clamp y una máscara más chica que no cabe de otro modo se
 * decodifica en un periodo de su máscara y el eje repite (o se refleja):
 * dentro de la tile es lo que muestrea el RDP; solo más allá de su borde,
 * donde el RDP fija el último texel, aquí se sigue repitiendo. El eje lo
 * dice `once` con su GFX_WRAP_* en los bits de GFX_TEX_WRAP_S / _T. Antes
 * se cortaban filas y la última se estiraba: el Castillo de Hyrule de
 * Super Smash Bros. tiene CI4 de 32 × 32 en tiles de 160 × 160 con clamp y
 * máscara 5 (y 64 de periodo en tiles de 128 × 256); de 51 o 64 filas,
 * los ladrillos de las torres y del muro salían alargados.
 * Primero t (las filas), después s. false si no queda nada. */
static bool tex_size(const struct gfx_tile *t, const uint16_t lim[2], uint32_t *wp, uint32_t *hp, uint32_t *x0p,
		     uint32_t *y0p, uint8_t *oncep)
{
	uint32_t w, h, x0 = 0, y0 = 0;
	uint8_t once = 0;
	gfx_tile_size(t, &w, &h);
	if (lim[0] && lim[0] < w)
		w = lim[0];
	if (lim[1] && lim[1] < h)
		h = lim[1];
	uint32_t pitch = (w * 4 + 63) & ~63u;
	if (pitch * h > GFX_TEX_MAX) {
		unsigned mt = clamp_mask(t->maskt, t->cmt, h);
		if (mt && (t->cmt & 1) && h <= 2u << mt) {
			h = y0 = 1u << mt;
			once |= GFX_TEX_ONCE_T;
		}
	}
	if (pitch * h > GFX_TEX_MAX) {
		unsigned ms = clamp_mask(t->masks, t->cms, w);
		if (ms && (t->cms & 1) && w <= 2u << ms) {
			w = x0 = 1u << ms;
			pitch = (w * 4 + 63) & ~63u;
			once |= GFX_TEX_ONCE_S;
		}
	}
	if (pitch * h > GFX_TEX_MAX) {
		unsigned mt = clamp_mask(t->maskt, t->cmt, h);
		if (mt && !(once & GFX_TEX_ONCE_T)) {
			h = 1u << mt;
			once |= ((t->cmt & 1) ? GFX_WRAP_MIRROR : GFX_WRAP_REPEAT) << 2;
		}
	}
	if (pitch * h > GFX_TEX_MAX) {
		unsigned ms = clamp_mask(t->masks, t->cms, w);
		if (ms && !(once & GFX_TEX_ONCE_S)) {
			w = 1u << ms;
			pitch = (w * 4 + 63) & ~63u;
			once |= (t->cms & 1) ? GFX_WRAP_MIRROR : GFX_WRAP_REPEAT;
		}
	}
	if (pitch * h > GFX_TEX_MAX)
		h = GFX_TEX_MAX / pitch;
	*wp = w;
	*hp = h;
	*x0p = x0;
	*y0p = y0;
	*oncep = once;
	return h != 0;
}

/* Con GFX_JOB_FBTEX: ¿el consumidor va a leer la textura de su destino? Sí
 * si es una RGBA16 (sin paleta ni RGB·A) que cae en una color image de 16
 * bits, o una I8 que cae en una de 8 bits (el RDP escribe ahí el rojo, sea
 * cual sea su formato: las máscaras de sombra de Conker, declaradas CI y
 * leídas como I8, a veces en el mismo cuadro), dibujada hace GFX_FB_FRESH
 * tareas o menos, buscada como la busca él (rdp_targets_locate: del ancho
 * del paso y el mismo tamaño de píxel, dentro de su alto conjeturado) y, si
 * repite en s, del ancho de la imagen (texture_from_target la liga en filas
 * enteras), y sin una máscara dentro del clamp. Lo que él sabe y la HLE no
 * lo arregla decodificándola. */
static __attribute__((noinline)) bool from_target(const struct gfx_state *g, const struct gfx_tile *t, unsigned tt,
						 bool premul, bool clamp_s, uint32_t w, uint32_t h,
						 const struct tex_src *src)
{
	/* Con una máscara dentro del clamp la textura repite sus primeros texels
	 * (decode), y ni el destino ni el consumidor lo hacen. */
	uint32_t i8 = src->src_siz == G_IM_SIZ_8b && t->fmt == G_IM_FMT_I;
	if (!(g->job->flags & GFX_JOB_FBTEX) || !src->src_addr || t->siz != src->src_siz || tt != G_TT_NONE ||
	    premul || (!i8 && (t->siz != G_IM_SIZ_16b || t->fmt != G_IM_FMT_RGBA)) ||
	    clamp_mask(t->masks, t->cms, w) || clamp_mask(t->maskt, t->cmt, h))
		return false;
	/* Las de 8 bits llevan el bit 0 de la dirección en fbimg (alineadas a 64). */
	uint32_t addr = src->src_addr | i8, pitch = src->src_pitch, task = g->job->task;
	/* Sin desenrollar: en el SPE, el código cuesta pila (local store). */
#pragma GCC unroll 1
	for (int i = 0; i < GFX_FB_IMAGES; i++) {
		const struct gfx_fbimg *e = &g->fbimg[i];
		if (e->width == pitch && addr - e->addr < (e->width * e->lh << (1 - i8)) && !((addr ^ e->addr) & 1) &&
		    task - e->task <= GFX_FB_FRESH && (clamp_s || w == e->width))
			return true;
	}
	return false;
}

/* La textura de la tile (contenido, de la caché o decodificado) y de
 * dónde vino. El origen se pone aparte: dos tiles con el mismo contenido
 * comparten textura pero pueden venir de distinto lugar. Si el consumidor
 * la va a leer de un destino, no se identifica ni se decodifica. */
#ifdef SALSA64_GFX_TRACE
/* Depuración en el host (src/host/run.c): cada textura que pide un dibujo. */
void salsa64_gfx_trace_tex(const struct gfx_state *g, const struct gfx_tile *t, const struct gfx_tex *tex,
			   bool decoded);
#endif

static bool resolve(struct gfx_state *g, const struct gfx_tile *t, unsigned tt, bool premul, const uint16_t lim[2],
		    bool clamp_s, struct gfx_tex *tex)
{
	uint32_t w, h, x0, y0;
	uint8_t once;
	if (!tex_size(t, lim, &w, &h, &x0, &y0, &once))
		return false;
	struct tex_src src = { 0, 0, 0, 0 };
	provenance(g, t, h, &src);
#ifdef SALSA64_GFX_TRACE
	uint32_t before = g->res->textures;
#endif
	memset(tex, 0, sizeof *tex);
	tex->flags = once;
	tex->width = (uint16_t)w;
	tex->height = (uint16_t)h;
	tex->pitch = (uint16_t)((w * 4 + 63) & ~63u);
	/* from_target no deja pasar una máscara dentro del clamp: once es 0. */
	if (from_target(g, t, tt, premul, clamp_s, w, h, &src)) {
		tex->where = GFX_TEX_TARGET;
		g->res->tex_target++;
	} else if (!resolve_content(g, t, tt, premul, w, h, x0, y0, once, tex)) {
		return false;
	}
	/* Copiar cada campo evita escribir 8 bytes a través del subobjeto
	 * src_addr (4 bytes), que GCC 13 rechaza con -Wstringop-overflow. */
	/* Una sola copia de 8 bytes desde el inicio de los campos (no a
	 * través del subobjeto src_addr, que GCC 13 rechaza): copiar campo por
	 * campo agranda el código del SPE 0 y le quita holgura de pila. */
	memcpy((char *)tex + offsetof(struct gfx_tex, src_addr), &src, sizeof src);
#ifdef SALSA64_GFX_TRACE
	salsa64_gfx_trace_tex(g, t, tex, g->res->textures != before);
#endif
	return true;
}

static bool resolve_content(struct gfx_state *g, const struct gfx_tile *t, unsigned tt, bool premul, uint32_t w,
			    uint32_t h, uint32_t x0, uint32_t y0, uint8_t once, struct gfx_tex *tex)
{
	uint32_t pitch = (w * 4 + 63) & ~63u;
	uint32_t t0 = gfx_io_clock();
	uint32_t key[2];
	tile_key(g, t, w, h, tt, premul, once, key);
	tex->key = key[0];  /* el tamaño y las banderas ya los puso resolve */

	/* Ya decodificada: en el arena de esta tarea o en la caché. */
	const struct gfx_tc_entry *found = NULL;
	bool old = false;
	for (uint32_t i = 0; i < g->narena_tex; i++)
		if (g->arena_tex[i].key[0] == key[0] && g->arena_tex[i].key[1] == key[1]) {
			*tex = g->arena_tex[i].tex;
			g->res->tex_reused++;
			g->res->t_hash += gfx_io_clock() - t0;
			return true;
		}
	if (g->tc_on)
		found = gfx_tc_find(g, key, &old);
	g->res->t_hash += gfx_io_clock() - t0;
	if (found) {
		tex->where = GFX_TEX_CACHE;
		tex->offset = found->off;
		g->res->tex_reused++;
		g->res->tex_old += old;
		return true;
	}

	uint32_t size = pitch * h;
	uint8_t *out = gfx_io_tex_begin(g, size);
	if (!out)
		return false;
	t0 = gfx_io_clock();
	decode(g, t, w, h, pitch, x0, y0, tt, premul, out);
	struct gfx_tc_entry *e = g->tc_on ? gfx_tc_insert(g, key, size) : NULL;
	if (e) {
		e->width = (uint16_t)w;
		e->height = (uint16_t)h;
		e->pitch = (uint16_t)pitch;
		gfx_io_tex_cache(g, e->off, size);
		tex->where = GFX_TEX_CACHE;
		tex->offset = e->off;
	} else {
		int32_t off = gfx_io_tex_commit(g, size);
		if (off < 0) {
			g->res->t_tex += gfx_io_clock() - t0;
			return false;
		}
		tex->where = GFX_TEX_ARENA;
		tex->offset = (uint32_t)off;
		g->res->tex_arena += g->tc_on;
		if (g->narena_tex < GFX_ARENA_TEXTURES) {
			g->arena_tex[g->narena_tex].key[0] = key[0];
			g->arena_tex[g->narena_tex].key[1] = key[1];
			g->arena_tex[g->narena_tex].tex = *tex;
			g->narena_tex++;
		}
	}
	g->res->t_tex += gfx_io_clock() - t0;
	g->res->textures++;
	return true;
}

bool gfx_tile_texture(struct gfx_state *g, int tile, struct gfx_tex *tex, bool premul, const uint16_t lim[2],
		      bool clamp_s)
{
	const struct gfx_tile *t = &g->tiles[tile & 7];
	/* La paleta se usa con texturas de 4 y 8 bits si el othermode la activa. */
	unsigned tt = (g->oh >> G_MDSFT_TEXTLUT) & 3;
	if (t->siz > G_IM_SIZ_8b || tt < G_TT_RGBA16)
		tt = G_TT_NONE;
	struct gfx_tilememo *m = &g->tilememo[tile & 7];
	if (m->ok && m->gen == g->tex_gen && m->tt == tt && m->premul == premul && m->clamp_s == clamp_s &&
	    m->lim[0] == lim[0] && m->lim[1] == lim[1]) {
		*tex = m->tex;
		if (tex->where == GFX_TEX_TARGET)
			g->res->tex_target++;
		else
			g->res->tex_reused++;
		return true;
	}
	if (!resolve(g, t, tt, premul, lim, clamp_s, tex))
		return false;
	m->ok = 1;
	m->gen = g->tex_gen;
	m->tt = (uint8_t)tt;
	m->premul = premul;
	m->clamp_s = clamp_s;
	m->lim[0] = lim[0];
	m->lim[1] = lim[1];
	m->tex = *tex;
	return true;
}
