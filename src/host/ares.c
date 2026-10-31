/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — comparar la HLE gráfica con la consola en un mismo cuadro, a
 * partir de un volcado de la RDRAM exportado por ares (Tools → Memory →
 * Export; 4 u 8 MB, big-endian como la RDRAM).
 *
 *   salsa64-run --ares [opciones] volcado-rdram.bin...
 *
 * En la RDRAM quedan las OSTask gráficas que el planificador de libultra
 * mandó al RSP (y la de la DMEM, en 0xFC0, si también se exportó) y el
 * búfer de imagen que el RDP de ares dibujó con ellas.
 * Para cada tarea gráfica se corre la HLE (src/rsp/gfx) sobre una copia de
 * la RDRAM y su salida se dibuja con el rasterizador de referencia
 * (raster.c, a 1×); cada color image de 16 bits que dibuja la lista (menos
 * la z image) se compara con lo que hay en esa dirección del volcado: PSNR,
 * píxeles distintos y dónde, y PNG de la HLE, de la consola y de las
 * diferencias. Es el mismo cuadro con los mismos datos, sin depender de
 * llegar a la misma escena en dos emuladores.
 *
 * Doble búfer: la lista que está en la RDRAM puede ser la del cuadro que
 * el CPU arma (a medio llenar, o con los segmentos aún en cero) y no la del
 * cuadro dibujado; por eso se compara cada lista contra el búfer que ella
 * misma dibuja, y se avisa de las que dibujan en la dirección 0 o no llegan
 * a G_RDPFULLSYNC.
 *
 * Antes de dibujar se borra (a negro) el búfer comparado en la copia: si
 * no, el rasterizador lo cebaría con la imagen de la consola (prime en
 * raster.c) y lo que la HLE no dibuja saldría igual. En la consola, lo que
 * la lista no cubre conserva el cuadro de hace dos campos, que el volcado
 * no tiene: esas zonas salen como diferencia.
 */
#define _GNU_SOURCE
#include "ares.h"

#include <getopt.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "core/bus.h"
#include "core/endian.h"
#include "core/rcp.h"
#include "raster.h"
#include "rdp/targets.h"
#include "rsp/gfx/backend.h"
#include "rsp/gfx/hle.h"

#define MAX_TASKS   8
#define MAX_TARGETS 8
#define MAP_COLS    8
#define MAP_ROWS    6

static uint8_t rdram_orig[SALSA64_RDRAM_MAX] __attribute__((aligned(128)));
static uint8_t rdram_work[SALSA64_RDRAM_MAX] __attribute__((aligned(128)));
static struct salsa64_bus bus;
static struct gfx_state st;
static struct gfx_vertex out_vtx[1u << 16] __attribute__((aligned(128)));
static uint8_t out_cmd[512u << 10] __attribute__((aligned(128)));
static uint8_t out_tex[4u << 20] __attribute__((aligned(128)));

#ifdef SALSA64_GFX_TRACE
extern int salsa64_gfx_trace_now;
#endif

struct target {
	uint32_t addr, width;
};

/* Color image que dibuja la lista, de cualquier tamaño de píxel: la
 * dirección, el ancho, los bytes por píxel y el alto que alcanzan sus
 * dibujos (para borrarla sin pisar lo de al lado). */
struct image {
	uint32_t addr, width, bpp, rows;
};

/* ---- PNG sin comprimir (bloques «stored» de deflate) --------------------- */

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
	crc = ~crc;
	for (size_t i = 0; i < n; i++) {
		crc ^= p[i];
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
	uint8_t b[4];
	put32(b, len);
	fwrite(b, 1, 4, f);
	fwrite(type, 1, 4, f);
	if (len)
		fwrite(data, 1, len, f);
	put32(b, crc_update(crc_update(0, (const uint8_t *)type, 4), data, len));
	fwrite(b, 1, 4, f);
}

/* Imagen 0xRRGGBB de w × h con `stride` píxeles por fila. */
static bool png_write(const char *path, const uint32_t *px, uint32_t stride, uint32_t w, uint32_t h)
{
	size_t raw_len = (size_t)h * (1 + 3 * (size_t)w);
	size_t blocks = raw_len / 65535 + 1;
	uint8_t *raw = malloc(raw_len), *z = malloc(2 + raw_len + 5 * blocks + 4);
	FILE *f = fopen(path, "wb");
	if (!raw || !z || !f) {
		free(raw);
		free(z);
		if (f)
			fclose(f);
		return false;
	}
	uint8_t *p = raw;
	for (uint32_t y = 0; y < h; y++) {
		*p++ = 0;  /* sin filtro */
		for (uint32_t x = 0; x < w; x++) {
			uint32_t c = px[y * stride + x];
			*p++ = (uint8_t)(c >> 16), *p++ = (uint8_t)(c >> 8), *p++ = (uint8_t)c;
		}
	}
	size_t n = 0, done = 0;
	z[n++] = 0x78, z[n++] = 0x01;
	uint32_t a = 1, b = 0;
	do {
		size_t len = raw_len - done < 65535 ? raw_len - done : 65535;
		z[n++] = done + len == raw_len;  /* BFINAL, BTYPE 00 */
		z[n++] = (uint8_t)len, z[n++] = (uint8_t)(len >> 8);
		z[n++] = (uint8_t)~len, z[n++] = (uint8_t)(~len >> 8);
		memcpy(z + n, raw + done, len);
		for (size_t i = 0; i < len; i++) {
			a = (a + raw[done + i]) % 65521u;
			b = (b + a) % 65521u;
		}
		n += len;
		done += len;
	} while (done < raw_len);
	put32(z + n, b << 16 | a);
	n += 4;

	static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	uint8_t ihdr[13];
	put32(ihdr, w);
	put32(ihdr + 4, h);
	ihdr[8] = 8, ihdr[9] = 2, ihdr[10] = ihdr[11] = ihdr[12] = 0;  /* RGB de 8 bits */
	fwrite(sig, 1, 8, f);
	png_chunk(f, "IHDR", ihdr, sizeof ihdr);
	png_chunk(f, "IDAT", z, (uint32_t)n);
	png_chunk(f, "IEND", NULL, 0);
	free(raw);
	free(z);
	return fclose(f) == 0;
}

/* ---- Tareas ---------------------------------------------------------------- */

static bool in_rdram(uint32_t addr, uint32_t size)
{
	addr &= 0x1FFFFFFF;
	return addr < bus.rdram_size && size <= bus.rdram_size - addr;
}

/* ¿Un puntero de OSTask (KSEG0 o físico) dentro de la RDRAM? */
static bool task_ptr(uint32_t p)
{
	return ((p & 0xE0000000u) == 0x80000000u || p < 0x00800000u) && in_rdram(p, 8);
}

static void task_read(const uint8_t *p, struct salsa64_task *t)
{
	uint32_t *w = &t->type;
	for (unsigned i = 0; i < sizeof *t / 4; i++)
		w[i] = salsa64_rd32(p + 4 * i);
}

/* Una OSTask gráfica plausible con un microcódigo que la HLE reconoce. */
static bool task_ok(const struct salsa64_task *t, enum salsa64_gbi *gbi)
{
	/* ucode_size puede ser 0 (Conker): el núcleo tampoco lo mira. */
	if (t->type != SALSA64_TASK_GFX || !task_ptr(t->ucode) || !task_ptr(t->ucode_data) ||
	    !task_ptr(t->data_ptr) || t->ucode_size > 0x2000 || t->ucode_data_size > 0x2000)
		return false;
	*gbi = salsa64_rsp_detect_gbi(&bus, t);
	return *gbi != SALSA64_GBI_UNKNOWN;
}

/* ¿El nombre del microcódigo dice ".NoN" (sin recorte cercano), como
 * gfx_ucode_nonear en core/rsp.c? */
static bool ucode_nonear(const struct salsa64_task *t, char *text, size_t n)
{
	salsa64_rsp_ucode_text(&bus, t->ucode_data, text, n);
	static const char pre[] = "RSP Gfx ucode ";
	if (strncmp(text, pre, sizeof pre - 1))
		return false;
	for (const char *q = text + sizeof pre - 1; *q && *q != ' '; q++)
		if (!strncmp(q, ".NoN", 4))
			return true;
	return false;
}

/* ---- HLE y rasterizador ----------------------------------------------------- */

/* Detalle de la HLE de la última tarea (vértices de G_VTX, triángulos de
 * entrada): para comparar el modelo del ritmo del RCP con lo que tarda la
 * misma tarea en ares (docs/ritmo-rcp.md). */
static struct gfx_detail detail __attribute__((aligned(128)));

static void run_task(const struct salsa64_task *t, enum salsa64_gbi gbi, struct gfx_job *job,
		     struct gfx_result *res)
{
	salsa64_gfx_job(job, &bus, t, gbi);
	memset(&detail, 0, sizeof detail);
	job->detail_ea = (uint64_t)(uintptr_t)&detail;
	job->detail_cap = sizeof detail;
	job->detail_version = GFX_DETAIL_VERSION;
	job->vtx_ea = (uint64_t)(uintptr_t)out_vtx;
	job->vtx_cap = sizeof out_vtx;
	job->cmd_ea = (uint64_t)(uintptr_t)out_cmd;
	job->cmd_cap = sizeof out_cmd;
	job->tex_ea = (uint64_t)(uintptr_t)out_tex;
	job->tex_cap = sizeof out_tex;
	job->task = 1;
	job->done_task = 0;
	if (raster_fbfx >= 1)
		job->flags |= GFX_JOB_FBTEX;  /* como salsa64-run y la PS3 */
	if (!raster_affine)
		job->flags |= GFX_JOB_NO_RSP_CLIP;  /* SALSA64_SIN_SOMBRA_AFIN, como salsa64-run */
	if (getenv("SALSA64_GFX_S2DEX_BG_OLD") && strcmp(getenv("SALSA64_GFX_S2DEX_BG_OLD"), "0"))
		job->flags |= GFX_JOB_S2DEX_BG_OLD;
	gfx_init(&st);
	gfx_run(&st, job, res);
}

static bool listed(const uint32_t *v, unsigned n, uint32_t x)
{
	for (unsigned i = 0; i < n; i++)
		if (v[i] == x)
			return true;
	return false;
}

/* Color images de 16 bits que dibuja la salida, sin las z images. */
static unsigned find_targets(const struct gfx_result *res, struct target *out, uint32_t *zero_cimg,
			     struct image *imgs, unsigned *nimgs)
{
	uint32_t z[16];
	unsigned nz = 0, n = 0;
	struct target all[32];
	unsigned nall = 0;
	int cur = -1;
	*zero_cimg = 0;
	*nimgs = 0;
	for (uint32_t off = 0; off + sizeof(struct gfx_cmd) <= res->cmd_bytes;) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(out_cmd + off);
		if (c->op == GFX_OP_END || c->size < sizeof *c)
			break;
		if (c->op == GFX_OP_TARGET) {
			static const uint32_t bpp[4] = { 0, 1, 2, 4 };
			cur = -1;
			for (unsigned i = 0; i < *nimgs; i++)
				cur = imgs[i].addr == c->a ? (int)i : cur;
			if (cur < 0 && *nimgs < 32 && c->a && c->b && c->b <= 640 && bpp[c->c & 3]) {
				cur = (int)(*nimgs)++;
				imgs[cur] = (struct image){ c->a, c->b, bpp[c->c & 3], 0 };
			}
		}
		if (c->op == GFX_OP_TARGET && (c->c & 0xFF) == 2 && c->b && c->b <= 640 && nall < 32) {
			bool dup = false;
			for (unsigned i = 0; i < nall; i++)
				dup |= all[i].addr == c->a;
			if (!dup)
				all[nall++] = (struct target){ c->a, c->b };
		} else if (c->op == GFX_OP_ZCLEAR && nz < 16 && !listed(z, nz, c->a)) {
			z[nz++] = c->a;
		} else if (c->op == GFX_OP_DRAW) {
			const struct gfx_draw *d = (const struct gfx_draw *)c;
			if (d->depth && nz < 16 && !listed(z, nz, d->zimg))
				z[nz++] = d->zimg;
			/* Lo que alcanzan sus vértices dentro de las tijeras: las tijeras
			 * solas pueden pasarse (Conker: 65 filas en una imagen de 64,
			 * con su viewport justo después). */
			float ymax = 0.0f;
			for (uint32_t v = 0; v < d->h.b; v++) {
				const struct gfx_vertex *x = &out_vtx[d->h.a + v];
				if (x->w > 0.0f && x->y / x->w > ymax)
					ymax = x->y / x->w;
			}
			uint32_t y1 = (d->scissor[3] + 3u) / 4u, yv = (uint32_t)ceilf(ymax);
			y1 = yv < y1 ? yv : y1;
			if (cur >= 0 && y1 > imgs[cur].rows)
				imgs[cur].rows = y1;
		}
		off += c->size;
	}
	/* Sin las z images (se borran como color image antes de usarlas). */
	unsigned k = 0;
	for (unsigned i = 0; i < *nimgs; i++)
		if (!listed(z, nz, imgs[i].addr))
			imgs[k++] = imgs[i];
	*nimgs = k;
	for (unsigned i = 0; i < nall && n < MAX_TARGETS; i++) {
		if (listed(z, nz, all[i].addr))
			continue;
		if (all[i].addr == 0) {
			(*zero_cimg)++;
			continue;
		}
		out[n++] = all[i];
	}
	return n;
}

/* ---- Comparación ------------------------------------------------------------ */

static uint32_t rgb5551(uint32_t p)
{
	uint32_t r = p >> 11, g = (p >> 6) & 31, b = (p >> 1) & 31;
	return (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}

static uint32_t cons[RASTER_W * RASTER_H], diff[RASTER_W * RASTER_H];
/* Diferencia HLE − consola por canal, sin promediar y en 4×4. */
static int dpix[RASTER_H][RASTER_W][3], dbox[RASTER_H][RASTER_W][3];

static double psnr_of(double se, uint32_t samples, char *out, size_t n)
{
	double mse = se / samples;
	if (mse > 0.0)
		snprintf(out, n, "%.2f dB", 10.0 * log10(255.0 * 255.0 / mse));
	else
		snprintf(out, n, "idénticas");
	return mse;
}

/* El RDP de la consola dibuja con tramado (ordenado o de ruido) antes de
 * quedarse con 5 bits por canal: la diferencia de cada píxel lleva ese
 * ruido. Promediada en 4×4 (el periodo del tramado ordenado) queda lo que
 * cambia de verdad: color, geometría, texturas. El mapa, la caja y la
 * imagen de diferencias usan la promediada. */
static void compare(const char *prefix, uint32_t dl, const struct target *t, unsigned tol, bool png)
{
	uint32_t width, lh = RASTER_H;
	const uint32_t *hle = raster_image(t->addr, &width);
	if (!hle) {
		printf("  %06x: el rasterizador no tiene imagen\n", t->addr);
		return;
	}
	for (int i = 0; i < RDP_TARGETS_MAX; i++) {
		uint32_t a, w, bpp, h;
		if (raster_target_info(i, &a, &w, &bpp, &h) && a == t->addr)
			lh = h;
	}
	uint32_t w = width < RASTER_W ? width : RASTER_W, h = lh < RASTER_H ? lh : RASTER_H;
	if (!w || !h || !in_rdram(t->addr, width * 2 * h)) {
		printf("  %06x: fuera del volcado\n", t->addr);
		return;
	}
	double se = 0.0, se_box = 0.0, bias[3] = { 0.0, 0.0, 0.0 };
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++) {
			uint32_t p = salsa64_rd16(rdram_orig + t->addr + 2 * (y * width + x));
			uint32_t o = hle[y * RASTER_W + x];
			cons[y * RASTER_W + x] = rgb5551(p);
			/* Con la HLE a 8 bits se compara con 8·v: con el tramado del RDP
			 * (v = (c + d) >> 3, d de 0 a 7 repartido en 4×4) su promedio es
			 * c; v << 3 | v >> 2 sumaría hasta 7 en lo claro. */
			uint32_t c = raster_q5 ? rgb5551(p) : (p >> 11) << 19 | ((p >> 6) & 31) << 11 | ((p >> 1) & 31) << 3;
			for (int k = 0; k < 3; k++) {
				int d = (int)(o >> (16 - 8 * k) & 0xFF) - (int)(c >> (16 - 8 * k) & 0xFF);
				dpix[y][x][k] = d;
				se += (double)d * d;
				bias[k] += d;
			}
		}
	uint32_t bad = 0, bx0 = w, by0 = h, bx1 = 0, by1 = 0, limit = 8 * tol;
	uint32_t cell[MAP_ROWS][MAP_COLS] = { { 0 } }, cell_n[MAP_ROWS][MAP_COLS] = { { 0 } };
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++) {
			int sum[3] = { 0, 0, 0 }, n = 0;
			for (uint32_t yy = y ? y - 1 : 0; yy <= y + 2 && yy < h; yy++)
				for (uint32_t xx = x ? x - 1 : 0; xx <= x + 2 && xx < w; xx++, n++)
					for (int k = 0; k < 3; k++)
						sum[k] += dpix[yy][xx][k];
			uint32_t dmax = 0;
			for (int k = 0; k < 3; k++) {
				int d = (sum[k] + (sum[k] < 0 ? -n / 2 : n / 2)) / n;
				dbox[y][x][k] = d;
				se_box += (double)d * d;
				dmax = (uint32_t)abs(d) > dmax ? (uint32_t)abs(d) : dmax;
			}
			unsigned cy = y * MAP_ROWS / h, cx = x * MAP_COLS / w;
			cell_n[cy][cx]++;
			uint32_t c = cons[y * RASTER_W + x];
			uint32_t lum = ((c >> 16 & 0xFF) + (c >> 8 & 0xFF) + (c & 0xFF)) / 9;
			if (dmax > limit) {
				bad++;
				cell[cy][cx]++;
				bx0 = x < bx0 ? x : bx0, by0 = y < by0 ? y : by0;
				bx1 = x > bx1 ? x : bx1, by1 = y > by1 ? y : by1;
				/* Amarillo (poco) a rojo (64 o más de 255), sobre la consola en gris oscuro. */
				uint32_t g = dmax >= 64 ? 0 : 255 - dmax * 4;
				diff[y * RASTER_W + x] = 0xFF0000u | g << 8;
			} else {
				diff[y * RASTER_W + x] = lum << 16 | lum << 8 | lum;
			}
		}
	char p1[32], p2[32];
	psnr_of(se, 3 * w * h, p1, sizeof p1);
	psnr_of(se_box, 3 * w * h, p2, sizeof p2);
	printf("  %06x (%u×%u): PSNR %s por píxel, %s en 4×4; HLE − consola en promedio %+.1f %+.1f %+.1f (RGB, de "
	       "255)\n", t->addr, w, h, p1, p2, bias[0] / (w * h), bias[1] / (w * h), bias[2] / (w * h));
	printf("    %u píxeles (%.2f %%) difieren en más de %u/255 en 4×4", bad, 100.0 * bad / (w * h), limit);
	if (bad)
		printf("; caja %u..%u × %u..%u", bx0, bx1 + 1, by0, by1 + 1);
	printf("\n");
	/* El bit de alfa (la cobertura que deja el RDP), si la imagen lo lleva
	 * en el rasterizador: las de 16 bits que no son del ancho del VI, o
	 * todas con SALSA64_ALFA_EN_TODAS=1. Solo donde la HLE dibujó: lo que
	 * la lista no cubre queda en negro (con alfa 0 en un destino con alfa)
	 * y no se cuenta; tampoco lo negro que sí dibujó. */
	if (raster_image_alpha(t->addr)) {
		uint32_t a0 = 0, a1 = 0, same = 0;
		for (uint32_t y = 0; y < h; y++)
			for (uint32_t x = 0; x < w; x++) {
				uint32_t o = hle[y * RASTER_W + x];
				if (!(o & 0xFFFFFFu))
					continue;
				uint32_t hb = o >> 24 ? 0u : 1u;
				uint32_t cb = salsa64_rd16(rdram_orig + t->addr + 2 * (y * width + x)) & 1u;
				same += hb == cb;
				a0 += hb < cb;
				a1 += hb > cb;
			}
		printf("    bit de alfa: %u píxeles iguales a la consola, %u en 0 donde la consola tiene 1, %u en 1 donde "
		       "tiene 0\n", same, a0, a1);
	}
	if (bad) {
		printf("    mapa (décimas de cada celda que difieren, '.' ninguna):\n");
		for (int r = 0; r < MAP_ROWS; r++) {
			printf("      ");
			for (int col = 0; col < MAP_COLS; col++) {
				uint32_t k = cell[r][col], n = cell_n[r][col];
				int dec = n ? (int)(10ull * k / n) : 0;
				putchar(!k ? '.' : dec > 9 ? '9' : '0' + dec);
			}
			putchar('\n');
		}
	}
	if (!png)
		return;
	static const char *const kinds[] = { "hle", "consola", "dif" };
	const uint32_t *imgs[] = { hle, cons, diff };
	for (int k = 0; k < 3; k++) {
		char path[4096 + 64];  /* el prefijo y lo que se le agrega */
		snprintf(path, sizeof path, "%s-%06x-%06x-%s.png", prefix, dl, t->addr, kinds[k]);
		if (!png_write(path, imgs[k], RASTER_W, w, h))
			fprintf(stderr, "salsa64-run: no se pudo escribir %s\n", path);
		else if (k == 2)
			printf("    %.*s-{hle,consola,dif}.png\n", (int)(strlen(path) - 8), path);
	}
}

/* ---- Entrada ------------------------------------------------------------------ */

static bool load(const char *path, uint8_t *buf, size_t cap, size_t *size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	*size = fread(buf, 1, cap, f);
	bool ok = !ferror(f);
	fclose(f);
	return ok;
}

struct opts {
	const char *dmem;   /* volcado de la DMEM (o NULL: el de al lado, si lo hay) */
	const char *out;    /* directorio de los PNG (NULL: el del volcado) */
	uint32_t only_dl, only_fb;
	unsigned tol;
	bool png;
	bool gfxdump;       /* también la salida de la HLE como gfxdump (tools/gfxdump.py) */
	unsigned passes;    /* veces que se corre la lista (--pasadas) */
};

/* La salida de la tarea como el volcado de la PS3 (gfx_dump_header y los
 * registros en big-endian, como lo escribe src/rdp), para listar los
 * dibujos con tools/gfxdump.py, dibujarlos con --raster-dump o repetirlos
 * en el RSX ("dump:<nombre>" en embedded, en RPCS3 o en la consola). */
static uint8_t dump_buf[(512u << 10) + (sizeof out_vtx)];

static void swap16(uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++, p += 2) {
		uint8_t t = p[0];
		p[0] = p[1], p[1] = t;
	}
}

static void swap32(uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++, p += 4) {
		uint8_t t0 = p[0], t1 = p[1];
		p[0] = p[3], p[1] = p[2], p[2] = t1, p[3] = t0;
	}
}

#define SW16(base, type, field, n) swap16((base) + offsetof(type, field), (n))
#define SW32(base, type, field, n) swap32((base) + offsetof(type, field), (n))

/* Los registros de comandos del orden del host al de la PS3, campo por campo. */
static void cmds_to_be(uint8_t *cmd, uint32_t bytes)
{
	for (uint32_t off = 0; off + sizeof(struct gfx_cmd) <= bytes;) {
		uint8_t *r = cmd + off;
		const struct gfx_cmd *c = (const struct gfx_cmd *)r;
		uint16_t op = c->op, size = c->size;
		if (op == GFX_OP_ZCLEAR && size >= sizeof(struct gfx_zclear)) {
			SW16(r, struct gfx_zclear, rect, 4);
			SW32(r, struct gfx_zclear, fill, 2);  /* fill y pad */
		} else if (op == GFX_OP_DRAW && size >= sizeof(struct gfx_draw)) {
			SW32(r, struct gfx_draw, zimg, 1);
			SW16(r, struct gfx_draw, scissor, 4);
			SW32(r, struct gfx_draw, fog_color, 2);  /* fog_color, prim_kind */
			SW16(r, struct gfx_draw, prim_tile, 1);  /* detrás, cvg_bit y relleno: bytes */
			for (int t = 0; t < 2; t++) {
				uint8_t *x = r + offsetof(struct gfx_draw, tex) + t * sizeof(struct gfx_tex);
				SW32(x, struct gfx_tex, offset, 1);
				SW16(x, struct gfx_tex, width, 3);  /* width, height, pitch */
				SW32(x, struct gfx_tex, key, 1);
				SW32(x, struct gfx_tex, xf, 4);
				SW32(x, struct gfx_tex, src_addr, 1);
				SW16(x, struct gfx_tex, src_pitch, 1);
			}
			SW32(r, struct gfx_draw, k, sizeof(((struct gfx_draw *)0)->k) / 4);
		}
		SW16(r, struct gfx_cmd, op, 2);  /* op y size */
		SW32(r, struct gfx_cmd, a, 3);
		if (op == GFX_OP_END || size < sizeof(struct gfx_cmd))
			break;
		off += size;
	}
}

_Static_assert(offsetof(struct gfx_vertex, r) == 24 && sizeof(struct gfx_vertex) == 32, "vértice de 6 floats, RGBA y relleno");

void ares_dump_to_be(uint8_t *dump, uint32_t size)
{
	const uint16_t one = 1;
	struct gfx_dump_header *h = (struct gfx_dump_header *)dump;
	if (!*(const uint8_t *)&one || size < sizeof *h || sizeof *h + (uint64_t)h->cmd_bytes + h->vtx_bytes > size)
		return;
	uint8_t *cmd = dump + sizeof *h, *vtx = cmd + h->cmd_bytes;
	cmds_to_be(cmd, h->cmd_bytes);
	for (uint32_t i = 0; i + sizeof(struct gfx_vertex) <= h->vtx_bytes; i += sizeof(struct gfx_vertex)) {
		swap32(vtx + i, 6);
		SW32(vtx + i, struct gfx_vertex, pad, 1);
	}
	swap32(dump, 4);
}

static void write_gfxdump(const char *prefix, uint32_t dl, const struct gfx_result *res)
{
	char path[4096 + 64];
	snprintf(path, sizeof path, "%s-%06x.gfxdump.bin", prefix, dl);
	if ((size_t)res->cmd_bytes + res->vtx_bytes > sizeof dump_buf)
		return;
	uint8_t *cmd = dump_buf, *vtx = dump_buf + res->cmd_bytes;
	memcpy(cmd, out_cmd, res->cmd_bytes);
	memcpy(vtx, out_vtx, res->vtx_bytes);
	struct gfx_dump_header h = { GFX_DUMP_MAGIC, res->cmd_bytes, res->vtx_bytes, res->tex_bytes };
	const uint16_t one = 1;
	if (*(const uint8_t *)&one) {  /* host little-endian */
		cmds_to_be(cmd, res->cmd_bytes);
		for (uint32_t i = 0; i + sizeof(struct gfx_vertex) <= res->vtx_bytes; i += sizeof(struct gfx_vertex)) {
			swap32(vtx + i, 6);
			SW32(vtx + i, struct gfx_vertex, pad, 1);
		}
		swap32((uint8_t *)&h, 4);
	}
	FILE *f = fopen(path, "wb");
	if (!f || fwrite(&h, sizeof h, 1, f) != 1 || fwrite(dump_buf, 1, res->cmd_bytes + res->vtx_bytes, f) !=
							       res->cmd_bytes + res->vtx_bytes ||
	    fwrite(out_tex, 1, res->tex_bytes, f) != res->tex_bytes) {
		fprintf(stderr, "salsa64-run: no se pudo escribir %s\n", path);
		if (f)
			fclose(f);
		return;
	}
	fclose(f);
	printf("  salida de la HLE en %s\n", path);
}

/* Triángulos de la salida con algún vértice detrás de la cámara. */
static uint32_t behind_camera(const struct gfx_result *res)
{
	uint32_t n = 0;
	for (uint32_t off = 0; off + sizeof(struct gfx_cmd) <= res->cmd_bytes;) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(out_cmd + off);
		if (c->op == GFX_OP_END || c->size < sizeof *c)
			break;
		if (c->op == GFX_OP_DRAW)
			for (uint32_t v = 0; v + 3 <= c->b; v += 3) {
				const struct gfx_vertex *x = &out_vtx[c->a + v];
				n += !(x[0].w > 0.0f) || !(x[1].w > 0.0f) || !(x[2].w > 0.0f);
			}
		off += c->size;
	}
	return n;
}

/* La OSTask de un volcado de la DMEM (4 u 8 KB: DMEM e IMEM). */
static bool dmem_task(const char *path, struct salsa64_task *t)
{
	static uint8_t dmem[0x2000];
	size_t size;
	if (!load(path, dmem, sizeof dmem, &size) || size < 0x1000)
		return false;
	task_read(dmem + SALSA64_TASK_DMEM, t);
	return true;
}

/* Un volcado: sus tareas gráficas, dibujadas y comparadas. Devuelve las
 * color images comparadas, o -1 si el volcado no sirve. */
static int one_dump(const char *path, const struct opts *o)
{
	size_t size;
	if (!load(path, rdram_orig, sizeof rdram_orig, &size) || (size != (4u << 20) && size != (8u << 20))) {
		fprintf(stderr, "salsa64-run: %s no es un volcado de la RDRAM de 4 u 8 MB\n", path);
		return -1;
	}
	bus.rdram = rdram_orig;
	bus.rdram_size = (uint32_t)size;
	const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	size_t n = strlen(path), nb = strlen(base);
	if (n > 4 && !strcmp(path + n - 4, ".bin"))
		n -= 4, nb -= nb > 4 ? 4 : 0;
	char prefix[4096];
	if (o->out)
		snprintf(prefix, sizeof prefix, "%s/%.*s", o->out, (int)nb, base);
	else
		snprintf(prefix, sizeof prefix, "%.*s", (int)n, path);
	printf("== %s (%zu MB)\n", base, size >> 20);

	/* Tareas: la de la DMEM (la última que arrancó) y las de la RDRAM. Sin
	 * --dmem se busca el volcado de la DMEM del mismo momento, como lo
	 * nombra ares (…-rsp-dmem-<fecha>.bin junto a …-rdram-<fecha>.bin) o
	 * …-rsp-dmem.bin junto a …-rdram.bin. */
	struct salsa64_task tasks[MAX_TASKS];
	enum salsa64_gbi gbis[MAX_TASKS];
	uint32_t where[MAX_TASKS];
	unsigned nt = 0;
	char dmem_path[4096];
	const char *dm = o->dmem, *r = strstr(base, "-rdram");
	if (!dm && r) {
		snprintf(dmem_path, sizeof dmem_path, "%.*s-rsp-dmem%s", (int)(r - path), path, r + 6);
		FILE *f = fopen(dmem_path, "rb");
		if (f) {
			fclose(f);
			dm = dmem_path;
		}
	}
	if (dm) {
		if (!dmem_task(dm, &tasks[0])) {
			fprintf(stderr, "salsa64-run: %s no es un volcado de la DMEM\n", dm);
			return -1;
		}
		if (task_ok(&tasks[0], &gbis[0]))
			where[nt++] = 0xFFFFFFFFu;
		else
			printf("la OSTask de la DMEM no es gráfica (tipo %u)\n", tasks[0].type);
	}
	for (uint32_t off = 0; off + 64 <= bus.rdram_size && nt < MAX_TASKS; off += 4) {
		if (salsa64_rd32(rdram_orig + off) != SALSA64_TASK_GFX)
			continue;
		struct salsa64_task t;
		enum salsa64_gbi g;
		task_read(rdram_orig + off, &t);
		if (!task_ok(&t, &g))
			continue;
		bool dup = false;
		for (unsigned i = 0; i < nt; i++)
			dup |= (tasks[i].data_ptr & 0x00FFFFFF) == (t.data_ptr & 0x00FFFFFF);
		if (dup)
			continue;
		tasks[nt] = t, gbis[nt] = g, where[nt] = off;
		nt++;
	}
	if (!nt) {
		printf("  ninguna OSTask gráfica con un microcódigo conocido\n");
		return 0;
	}

	int compared = 0;
	for (unsigned i = 0; i < nt; i++) {
		const struct salsa64_task *t = &tasks[i];
		uint32_t dl = t->data_ptr & 0x00FFFFFF;
		if (o->only_dl && dl != o->only_dl)
			continue;
		char text[96];
		bus.rsp.gfx_nonear = ucode_nonear(t, text, sizeof text);
		if (where[i] == 0xFFFFFFFFu)
			printf("tarea de la DMEM: lista %06x, %s\n", dl, text);
		else
			printf("tarea en %06x: lista %06x, %s\n", where[i], dl, text);

		/* Primera pasada: qué color images dibuja. */
		struct gfx_job job;
		struct gfx_result res;
		bus.rdram = rdram_orig;
		run_task(t, gbis[i], &job, &res);
		struct target tg[MAX_TARGETS];
		struct image imgs[32];
		uint32_t zero;
		unsigned nimgs;
		unsigned ntg = find_targets(&res, tg, &zero, imgs, &nimgs);
		printf("  %u comandos, %u triángulos, %u rectángulos, %u dibujos%s%s", res.commands, res.triangles,
		       res.rects, res.draws, res.flags & GFX_RES_FULLSYNC ? ", llega a G_RDPFULLSYNC" : ", SIN G_RDPFULLSYNC",
		       res.flags & GFX_RES_CUT ? ", CORTADA" : "");
		if (res.unknown_cmds)
			printf(", %u comandos sin implementar (último %02x)", res.unknown_cmds, res.last_unknown);
		if (res.dl_errors)
			printf(", %u errores de la lista", res.dl_errors);
		printf("\n");
		printf("  ritmo: %u cargas a la TMEM, %u G_VTX con %u vértices (%u con luz; %u en el resultado), %u triángulos de entrada; "
		       "modelo del RCP %u ciclos (%.2f ms)\n",
		       res.loads, detail.vertex_calls, detail.vertices, detail.lit_vertices, res.vertices, detail.triangles_in,
		       salsa64_gfx_model_cycles(&res, gbis[i]), salsa64_gfx_model_cycles(&res, gbis[i]) / 93750.0);
		uint32_t behind = behind_camera(&res);
		if (behind)
			printf("  %u triángulos con algún vértice detrás de la cámara (w ≤ 0; se recortan)\n", behind);
		if (zero)
			printf("  dibuja en la color image 000000: segmento sin poner (¿lista del cuadro que se arma?)\n");
		if (o->only_fb) {
			uint32_t w = 320;
			for (unsigned k = 0; k < ntg; k++)
				w = tg[k].addr == o->only_fb ? tg[k].width : w;
			ntg = 1;
			tg[0] = (struct target){ o->only_fb, w };
		}
		if (!ntg) {
			printf("  ninguna color image de 16 bits que comparar\n");
			continue;
		}

		/* Segunda pasada sobre la copia, con los búferes comparados en negro.
		 * Con --pasadas N > 1, también las demás color images que dibuja la
		 * lista (las auxiliares, de render a textura) y la lista N veces
		 * seguidas, como N cuadros: lo que la HLE lee de ellas por la RDRAM
		 * es lo que el consumidor copió ahí en la pasada anterior, como en
		 * la PS3, y no lo que dibujó la consola. */
		memcpy(rdram_work, rdram_orig, bus.rdram_size);
		for (unsigned k = 0; k < ntg; k++) {
			uint32_t a = tg[k].addr, len = tg[k].width * 2 * RASTER_H;
			if (a < bus.rdram_size)
				memset(rdram_work + a, 0, len < bus.rdram_size - a ? len : bus.rdram_size - a);
		}
		for (unsigned k = 0; o->passes > 1 && k < nimgs; k++) {
			uint32_t a = imgs[k].addr, len = imgs[k].width * imgs[k].bpp * imgs[k].rows;
			if (a < bus.rdram_size)
				memset(rdram_work + a, 0, len < bus.rdram_size - a ? len : bus.rdram_size - a);
		}
		if (o->passes > 1)
			printf("  %u pasadas, con las %u color images de la lista borradas antes\n", o->passes, nimgs);
		bus.rdram = rdram_work;
		raster_reset();
		for (unsigned p = 0; p < o->passes; p++) {
#ifdef SALSA64_GFX_TRACE
			salsa64_gfx_trace_now = getenv("SALSA64_GFX_VERBOSE") != NULL && p + 1 == o->passes;
#endif
			run_task(t, gbis[i], &job, &res);
			if (o->gfxdump && p + 1 == o->passes)
				write_gfxdump(prefix, dl, &res);
			raster_task(&job, &res);
		}
		bus.rdram = rdram_orig;
		uint32_t dr, de, dt;
		raster_edge_stats(&dr, &de, &dt);
		if (de)
			printf("  aristas de los dibujos con AA: %u de %u dibujos (%u triángulos)%s\n", de, dr, dt,
			       raster_aa_edges ? ", encendidas" : ", apagadas");
		for (unsigned k = 0; k < ntg; k++) {
			compare(prefix, dl, &tg[k], o->tol, o->png);
			compared++;
		}
	}
	return compared;
}

static void usage(FILE *out)
{
	fputs("uso: salsa64-run --ares [opciones] volcado-rdram.bin...\n"
	      "  Dibuja con la HLE gráfica las tareas de cada volcado de la RDRAM (ares: Tools →\n"
	      "  Memory → Export) y compara cada búfer de imagen que dibujan con el del volcado.\n"
	      "      --dmem ARCHIVO  volcado de la DMEM del RSP (un solo volcado de la RDRAM):\n"
	      "                      también su OSTask (0xFC0); sin la opción se usa el\n"
	      "                      …-rsp-dmem-<fecha>.bin de al lado, si existe\n"
	      "      --task DL       solo la tarea cuya lista (data_ptr) es DL (hex)\n"
	      "      --fb DIR        comparar solo esa color image (hex)\n"
	      "      --tol N         diferencia por canal (promedio de 4×4), en pasos de 5 bits,\n"
	      "                      que no cuenta (1)\n"
	      "      --q5            la HLE a 5 bits por canal (por omisión, a 8 como el RSX)\n"
	      "      --out DIR       PNG en DIR/<volcado>-<lista>-<búfer>-{hle,consola,dif}.png\n"
	      "                      (por omisión, junto al volcado)\n"
	      "      --no-png        solo las cifras\n"
	      "      --pasadas N     la lista N veces seguidas (2 a 8) con todas sus color images\n"
	      "                      borradas antes: el render a textura sale de lo que el\n"
	      "                      rasterizador copió a la RDRAM, como en la PS3, no de la consola\n"
	      "      --gfxdump       también la salida de la HLE de cada lista, como el gfxdump de\n"
	      "                      la PS3 (big-endian): <volcado>-<lista>.gfxdump.bin, para\n"
	      "                      tools/gfxdump.py, --raster-dump o \"dump:\" en el RSX\n"
	      "  En las imágenes de 16 bits con alfa en el rasterizador compara también el bit\n"
	      "  de alfa (la cobertura del RDP); SALSA64_ALFA_EN_TODAS=1 lo lleva en todas,\n"
	      "  SALSA64_ALFA_FRAGMENTO=1 deja el alfa del fragmento (como la v0.2.15).\n",
	      out);
}

/* Variable de entorno puesta y distinta de "" y de "0" (como salsa64_host_env_on). */
static bool env_on(const char *name)
{
	const char *v = getenv(name);
	return v && *v && *v != '0';
}

int ares_main(int argc, char **argv)
{
	static const struct option longopts[] = {
		{ "dmem", required_argument, NULL, 'd' },
		{ "task", required_argument, NULL, 't' },
		{ "fb", required_argument, NULL, 'b' },
		{ "tol", required_argument, NULL, 'T' },
		{ "out", required_argument, NULL, 'o' },
		{ "no-png", no_argument, NULL, 'n' },
		{ "q5", no_argument, NULL, 'q' },
		{ "gfxdump", no_argument, NULL, 'g' },
		{ "pasadas", required_argument, NULL, 'p' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	struct opts o = { .tol = 1, .png = true, .passes = 1 };
	bool q5 = false;
	int c;
	optind = 1;
	while ((c = getopt_long(argc, argv, "h", longopts, NULL)) != -1) {
		switch (c) {
		case 'd': o.dmem = optarg; break;
		case 't': o.only_dl = (uint32_t)strtoul(optarg, NULL, 16) & 0x00FFFFFF; break;
		case 'b': o.only_fb = (uint32_t)strtoul(optarg, NULL, 16) & 0x00FFFFFF; break;
		case 'T': o.tol = (unsigned)strtoul(optarg, NULL, 0); break;
		case 'o': o.out = optarg; break;
		case 'n': o.png = false; break;
		case 'q': q5 = true; break;
		case 'g': o.gfxdump = true; break;
		case 'p': o.passes = (unsigned)strtoul(optarg, NULL, 0); break;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 3;
		}
	}
	if (optind >= argc || (o.dmem && argc - optind > 1) || !o.passes || o.passes > 8) {
		usage(stderr);
		return 3;
	}
	if (o.out && (o.png || o.gfxdump))
		mkdir(o.out, 0777);  /* si ya existe, nada */
	/* El comparador no arranca el núcleo ni sus backends. Activa aquí las
	 * variantes optativas para que sus CRC se reconozcan igual que al jugar. */
	static struct salsa64_gfx_backend detector = { .turbo3d = true, .acclaim = true, .eurocom = true };
	salsa64_rsp_set_gfx(&bus, &detector);
	if (getenv("SALSA64_FBFX"))
		raster_fbfx = atoi(getenv("SALSA64_FBFX"));
	if (getenv("SALSA64_AA_EDGES"))
		raster_aa_edges = atoi(getenv("SALSA64_AA_EDGES")) != 0;
	if (getenv("SALSA64_AA_EDGE_OFFSET"))
		sscanf(getenv("SALSA64_AA_EDGE_OFFSET"), "%f,%f", &raster_edge_factor, &raster_edge_units);
	if (getenv("SALSA64_AA_EDGE_WIDTH"))
		raster_edge_width = (float)atof(getenv("SALSA64_AA_EDGE_WIDTH"));
	/* El alfa de los destinos como al jugar (run.c), y en todas las imágenes
	 * de 16 bits para comparar su bit con el de la consola. */
	raster_target_alpha = !env_on("SALSA64_SIN_ALFA_DESTINOS");
	raster_copy_alpha = !env_on("SALSA64_COPIA_SIN_ALFA");
	raster_cvg_alpha = !env_on("SALSA64_ALFA_FRAGMENTO");
	raster_alpha_all = env_on("SALSA64_ALFA_EN_TODAS");
	{
		const char *e = getenv("SALSA64_SIN_SOMBRA_AFIN");  /* como salsa64_host_env_on */
		raster_affine = !(e && *e && strcmp(e, "0"));
	}
	raster_q5 = q5;
	int total = 0;
	for (int i = optind; i < argc; i++) {
		int n = one_dump(argv[i], &o);
		if (n < 0)
			return 3;
		total += n;
	}
	return total ? 0 : 2;
}
