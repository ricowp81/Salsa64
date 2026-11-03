/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — backend gráfico en un SPE. Ver gfx_spu.h.
 *
 * El programa del SPE 0 viene en varias imágenes, cada una con sus familias
 * de microcódigos (rsp/gfx/spe0.h, docs/spe0-familias.md). Al arrancar el
 * juego se carga la primera (F3D); cada tarea pide la imagen de su familia y,
 * si es otra, se cambia (load_image): el estado del RDP (tramo crudo de la
 * gfx_state y las color images dibujadas) pasa por la memoria principal, el
 * directorio de la caché de texturas empieza vacío con una barrera hasta que
 * el consumidor termine la última tarea de la imagen anterior, y la imagen
 * nueva se comprueba contra el PPE (hash y tareas de prueba) antes de usarla.
 */
#include "platform/ps3/gfx_spu.h"

#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/systime.h>

#include "core/bus.h"
#include "core/endian.h"
#include "cpu/vr4300.h"
#include "platform/ps3/log.h"
#include "platform/ps3/paths.h"
#include "rsp/gfx/hle.h"
#include "rsp/gfx/selftest.h"
#include "spu_gfx0_bin.h"
#include "spu_gfx0_stack.h"
#include "spu_gfx1_bin.h"
#include "spu_gfx1_stack.h"
#include "spu_gfx1fase8_bin.h"
#include "spu_gfx1fase8_stack.h"
#include "spu_gfx2_bin.h"
#include "spu_gfx2_stack.h"
#include "spu_gfx3_bin.h"
#include "spu_gfx3_stack.h"
#include "spu_gfx4_bin.h"
#include "spu_gfx4_stack.h"
#include "spu_gfx5_bin.h"
#include "spu_gfx5_stack.h"

/* Las imágenes embebidas (src/frontend/Makefile) y sus números del build
 * (tools/spu-stack.py): hasta dónde llega el programa en el local store y
 * la holgura de la pila. */
struct gfx_spu_image {
	const char *program;
	const u8 *bin;
	const u32 *size;
	uint32_t end, stack;
	int32_t slack;
};
static const struct gfx_spu_image images[GFX_SPE0_IMAGES] = {
	{ "spu_gfx0", spu_gfx0_bin, &spu_gfx0_bin_size, SPU_GFX0_END, SPU_GFX0_STACK, SPU_GFX0_SLACK },
	{ "spu_gfx1", spu_gfx1_bin, &spu_gfx1_bin_size, SPU_GFX1_END, SPU_GFX1_STACK, SPU_GFX1_SLACK },
	{ "spu_gfx2", spu_gfx2_bin, &spu_gfx2_bin_size, SPU_GFX2_END, SPU_GFX2_STACK, SPU_GFX2_SLACK },
	{ "spu_gfx3", spu_gfx3_bin, &spu_gfx3_bin_size, SPU_GFX3_END, SPU_GFX3_STACK, SPU_GFX3_SLACK },
	{ "spu_gfx4", spu_gfx4_bin, &spu_gfx4_bin_size, SPU_GFX4_END, SPU_GFX4_STACK, SPU_GFX4_SLACK },
	{ "spu_gfx5", spu_gfx5_bin, &spu_gfx5_bin_size, SPU_GFX5_END, SPU_GFX5_STACK, SPU_GFX5_SLACK },
};
static const struct gfx_spu_image phase_images[] = {
	{ "spu_gfx1fase8", spu_gfx1fase8_bin, &spu_gfx1fase8_bin_size, SPU_GFX1FASE8_END, SPU_GFX1FASE8_STACK, SPU_GFX1FASE8_SLACK },
};
_Static_assert(GFX_SPE0_IMAGES == 6, "una entrada de `images` por imagen del SPE 0");

static const struct gfx_spu_image *image_data(const struct salsa64_gfx_spu *s, int img)
{
	if (img == GFX_SPE0_F3DEX2 && s->detail_phase == 8)
		return &phase_images[0];
	return &images[img];
}

/* El hash con SIMD del SPE (identidad de las texturas en la caché) debe dar
 * lo mismo que el de C: tramos que cortan bloques de 16 bytes y que dan la
 * vuelta al anillo. Devuelve los casos que fallaron. */
static int check_hash(spukit_worker *w)
{
	static uint8_t pat[4096] __attribute__((aligned(128)));
	static const uint32_t cases[][3] = {
		{ 4096, 0, 4096 }, { 4096, 8, 24 }, { 4096, 4088, 16 }, { 4096, 1000, 2000 },
		{ 2048, 2040, 520 }, { 4096, 3, 13 }, { 16, 0, 16 },
	};
	for (int i = 0; i < 4096; i++)
		pat[i] = (uint8_t)(i * 31 + (i >> 8));
	int bad = 0;
	for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
		struct gfx_hash_job hj = { (uint64_t)(uintptr_t)pat, cases[c][0], cases[c][1], cases[c][2] };
		const spukit_result *r = spukit_wait(w, spukit_post(w, GFX_JOB_HASH, &hj, sizeof hj));
		struct gfx_hash h;
		uint32_t key[2], spu[2];
		gfx_hash_init(&h);
		gfx_hash_ring(&h, pat, hj.size, hj.start, hj.len);
		gfx_hash_final(&h, key);
		memcpy(spu, r->data, sizeof spu);
		if (r->error || spu[0] != key[0] || spu[1] != key[1]) {
			salsa64_log("hash del SPE: caso %u da %08x%08x, el PPE %08x%08x", c, spu[0], spu[1], key[0], key[1]);
			bad++;
		}
	}
	return bad;
}

/* Tareas de prueba (rsp/gfx/selftest.c) de cada microcódigo de la imagen, en
 * el SPE y en el PPE sobre copias iguales de una RDRAM de mentira: la misma
 * salida (los flotantes a lo más SELFTEST_ULPS de distancia: el SPE redondea
 * hacia cero), los mismos contadores y las mismas texturas. Devuelve cuántas
 * difieren. El estado del SPE queda con lo de la prueba: lo borra la tarea
 * de reinicio de load_image. */
#define SELFTEST_ULPS 64
#define SPE0_ALL_PATH SALSA64_DATA_DIR "/spe0-todas"
/* Con este archivo el SPE llena su pila libre con un patrón al arrancar y
 * los bytes «sin tocar» del log son exactos; sin él, los que siguen en cero. */
#define SPE0_STACK_PATH SALSA64_DATA_DIR "/spe0-pila"
#define SPE0_DETAIL_PATH SALSA64_DATA_DIR "/gfx-spe-detalle"
#define SPE0_CLIP4_PATH SALSA64_DATA_DIR "/gfx-recorte-vector"
#define SPE0_SCALAR_PATH SALSA64_DATA_DIR "/gfx-escalar"
#define SPE0_POLL_PATH SALSA64_DATA_DIR "/gfx-sondeo"
#define SPE0_WAIT_PATH SALSA64_DATA_DIR "/gfx-espera-siempre"
struct detail_window {
	struct gfx_detail task __attribute__((aligned(128)));
	uint64_t sum[sizeof(struct gfx_detail) / 4];
	uint64_t handler;
	uint32_t count, first, last;
};

#define DS(w, field) ((w)->sum[offsetof(struct gfx_detail, field) / 4])
static const char *detail_phase_name(uint32_t phase)
{
	static const char *const names[] = {
		"matriz", "desempacado", "proyeccion/division", "recorte", "niebla", "conversion/escritura",
		"postproceso completo",
		"lote completo",
		"lote completo/recorte SIMD",
	};
	return phase == 8 ? names[8] : "normal";
}

static void detail_log(struct detail_window *w, uint32_t phase)
{
	if (!w || !w->count) return;
	double k = 1000.0 / (spukit_tick_hz() * w->count), n = w->count;
	salsa64_log("gfx detalle: tareas %u-%u (%u), ms/tarea manejador %.3f total %.3f vtx %.3f estado %.3f "
		    "TMEM %.3f tri sin estado %.3f matrices %.3f listas %.3f cierre %.3f resto %.3f",
		    w->first, w->last, w->count, w->handler * k, DS(w, t_total) * k, DS(w, t_vtx) * k,
		    DS(w, t_state) * k, DS(w, t_load) * k, DS(w, t_triangle) * k, DS(w, t_matrix) * k,
		    DS(w, t_dl) * k, DS(w, t_end) * k, DS(w, t_rest) * k);
	salsa64_log("gfx detalle: por tarea vertices %.1f (%0.1f cargas, luz %.1f niebla %.1f ST %.1f), "
		    "tri entrada %.1f aceptados %.1f plano comun %.1f cercano %.1f recortados %.1f; "
		    "comandos %.1f estados %.1f salida v/c/t %.0f/%.0f/%.0f bytes",
		    DS(w, vertices) / n, DS(w, vertex_calls) / n, DS(w, lit_vertices) / n, DS(w, fog_vertices) / n,
		    DS(w, texgen_vertices) / n, DS(w, triangles_in) / n, DS(w, triangles_ok) / n,
		    DS(w, triangles_common) / n, DS(w, triangles_near) / n, DS(w, triangles_clipped) / n,
		    DS(w, commands) / n, DS(w, state_builds) / n, DS(w, vtx_bytes) / n, DS(w, cmd_bytes) / n,
		    DS(w, tex_bytes) / n);
	salsa64_log("gfx detalle: lote vtx vector %.1f en %.1f lotes; escalar forzado %.1f luz %.1f (ST %.1f) "
		    "sin niebla %.1f cola %.1f",
		    DS(w, vector_vertices) / n, DS(w, vector_batches) / n, DS(w, scalar_forced) / n,
		    DS(w, scalar_lighting) / n, DS(w, scalar_texgen) / n, DS(w, scalar_no_fog) / n,
		    DS(w, scalar_tail) / n);
	/* RDRAM se divide entre todos los vértices; las otras fases, solo entre
	 * los que sí recorrieron el lote de cuatro. Así una cola escalar no baja
	 * artificialmente los ticks/vértice del lote. */
	double nv = DS(w, vertices), nb = DS(w, vector_vertices);
	double rdram = w->sum[offsetof(struct gfx_detail, send_ticks) / 4] +
		       w->sum[offsetof(struct gfx_detail, read_ticks) / 4];
	double measured = DS(w, vtx_transform), rest = DS(w, t_vtx) - rdram - measured;
	if (phase) {
		salsa64_log("gfx detalle: vtx fases ms/tarea, ticks/vtx RDRAM %.3f/%.1f fase %u %s %.3f/%.1f "
			    "resto sin esa fase %.3f/%.1f (lote vectorial)",
			    rdram * k, nv ? rdram / nv : 0.0, phase, detail_phase_name(phase),
			    measured * k, nb ? measured / nb : 0.0, rest * k, nv ? rest / nv : 0.0);
	} else {
		salsa64_log("gfx detalle: vtx fases ms/tarea, ticks/vtx RDRAM %.3f/%.1f matriz %.3f/%.1f "
			    "resto %.3f/%.1f (desemp/proy/recorte/niebla/entero/escritura)",
			    rdram * k, nv ? rdram / nv : 0.0, measured * k, nb ? measured / nb : 0.0,
			    rest * k, nv ? rest / nv : 0.0);
	}
	for (unsigned i = 0; i < 4; i++)
		salsa64_log("gfx detalle: lectura %u (vtx/mtx/pequena/TMEM): peticiones %.1f bytes pedidos %.0f DMA %.0f "
			    "envio %.3f espera %.3f ms/tarea (anidados)", i,
			    w->sum[offsetof(struct gfx_detail, reads) / 4 + i] / n,
			    w->sum[offsetof(struct gfx_detail, requested) / 4 + i] / n,
			    w->sum[offsetof(struct gfx_detail, transferred) / 4 + i] / n,
			    w->sum[offsetof(struct gfx_detail, send_ticks) / 4 + i] * k,
			    w->sum[offsetof(struct gfx_detail, read_ticks) / 4 + i] * k);
	for (unsigned i = 0; i < 3; i++)
		salsa64_log("gfx detalle: salida %u (vtx/cmd/tex): DMA %.1f bytes %.0f espera %.3f ms/tarea (anidada)",
			    i, w->sum[offsetof(struct gfx_detail, dma_puts) / 4 + i] / n,
			    w->sum[offsetof(struct gfx_detail, dma_bytes) / 4 + i] / n,
			    w->sum[offsetof(struct gfx_detail, wait_ticks) / 4 + i] * k);
	salsa64_log("gfx detalle: listas bloques %.1f bytes %.0f aciertos %.1f cambios %.1f por tarea",
		    DS(w, dl_fetches) / n, DS(w, dl_bytes) / n, DS(w, dl_hits) / n, DS(w, dl_misses) / n);
	memset(w->sum, 0, sizeof w->sum);
	w->count = 0; w->handler = 0;
}

static void detail_account(struct salsa64_gfx_spu *s, uint64_t ticks)
{
	if (!s->detail || !s->job.detail_ea) return;
	struct detail_window *w = s->detail;
	if (w->task.version != GFX_DETAIL_VERSION || w->task.task != s->job.task ||
	    w->task.bytes != sizeof w->task) {
		salsa64_log("gfx detalle: bloque invalido en tarea %u", s->job.task);
		return;
	}
	uint32_t words[sizeof(struct gfx_detail) / 4];
	memcpy(words, &w->task, sizeof words);
	for (unsigned i = 0; i < sizeof words / sizeof words[0]; i++) w->sum[i] += words[i];
	if (!w->count) w->first = s->job.task;
	w->last = s->job.task; w->count++; w->handler += ticks;
	if (w->count == 100) detail_log(w, s->detail_phase);
}
static struct {
	/* Una sola RDRAM: la HLE no la escribe (los rellenos los copia el PPE
	 * después, salsa64_gfx_rdram_fills) y el SPE termina antes de que corra
	 * el PPE. */
	uint8_t ram[GFX_SELFTEST_RAM] __attribute__((aligned(128)));
	struct gfx_vertex vtx[2][512] __attribute__((aligned(128)));
	uint8_t cmd[2][16384] __attribute__((aligned(128)));
	uint8_t tex[2][65536] __attribute__((aligned(128)));
	uint8_t cache[2][65536] __attribute__((aligned(128)));
	struct gfx_state ppe __attribute__((aligned(128)));
} chk;

static void check_job(struct gfx_job *job, int k, uint32_t gbi, uint32_t task, uint32_t dl)
{
	memset(job, 0, sizeof *job);
	job->dl = dl;
	job->rdram_ea = (uint64_t)(uintptr_t)chk.ram;
	job->rdram_size = GFX_SELFTEST_RAM;
	job->vtx_ea = (uint64_t)(uintptr_t)chk.vtx[k];
	job->vtx_cap = sizeof chk.vtx[k];
	job->cmd_ea = (uint64_t)(uintptr_t)chk.cmd[k];
	job->cmd_cap = sizeof chk.cmd[k];
	job->tex_ea = (uint64_t)(uintptr_t)chk.tex[k];
	job->tex_cap = sizeof chk.tex[k];
	job->cache_ea = (uint64_t)(uintptr_t)chk.cache[k];
	job->cache_cap = sizeof chk.cache[k];
	job->task = task;
	job->done_task = task - 1;
	job->gbi = gbi;
	job->ucode = GFX_SELFTEST_UCODE;
}

/* Las primeras palabras distintas de una salida desde `from`, al log. */
static void log_words(const char *what, const uint8_t *spe, const uint8_t *ppe, uint32_t from, uint32_t n)
{
	char line[256];
	size_t used = 0;
	unsigned shown = 0;
	line[0] = '\0';
	for (uint32_t i = from & ~3u; i + 4 <= n && shown < 6; i += 4) {
		uint32_t a, b;
		memcpy(&a, spe + i, 4);
		memcpy(&b, ppe + i, 4);
		if (a == b)
			continue;
		used += (size_t)snprintf(line + used, sizeof line - used, " %u: %08x/%08x", i, a, b);
		if (used >= sizeof line)
			break;
		shown++;
	}
	salsa64_log("SPE 0:   %s (byte: SPE/PPE)%s", what, line);
}

static int check_lists(spukit_worker *w, int img, struct detail_window *detail)
{
	static const uint32_t gbis[] = { SALSA64_GBI_F3D,    SALSA64_GBI_F3DEX,  SALSA64_GBI_F3DWR,
					 SALSA64_GBI_F3DPD,  SALSA64_GBI_F3DDKR, SALSA64_GBI_F3DJFG,
					 SALSA64_GBI_S2DEX1, SALSA64_GBI_F3DEX2, SALSA64_GBI_F3DZEX2,
					 SALSA64_GBI_S2DEX,  SALSA64_GBI_F3DEXBG,
					 SALSA64_GBI_F5ROGUE, SALSA64_GBI_F5INDY,
					 SALSA64_GBI_TURBO3D, SALSA64_GBI_TURBO3D_F3D, SALSA64_GBI_F3DEX2ACCLAIM,
					 SALSA64_GBI_F3DEX2EUROCOM };
	int bad = 0;
	uint32_t task = 0;
	memset(chk.cache, 0, sizeof chk.cache);
	gfx_init(&chk.ppe);
	for (unsigned i = 0; i < sizeof gbis / sizeof gbis[0]; i++) {
		if (gfx_spe0_image(gbis[i]) != img)
			continue;
		task++;
		struct gfx_job job[2];
		struct gfx_result rs, rp;
		uint32_t dl = gfx_selftest_build(chk.ram, gbis[i], task);
		check_job(&job[0], 0, gbis[i], task, dl);
		check_job(&job[1], 1, gbis[i], task, dl);
		if (img == 1 && detail) {
			job[0].detail_ea = (uint64_t)(uintptr_t)&detail->task;
			job[0].detail_cap = sizeof detail->task;
			job[0].detail_version = GFX_DETAIL_VERSION;
		}
		memset(chk.vtx, 0, sizeof chk.vtx);
		memset(chk.cmd, 0, sizeof chk.cmd);
		memset(chk.tex, 0, sizeof chk.tex);
		const spukit_result *r = spukit_wait(w, spukit_post(w, GFX_JOB_TASK, &job[0], sizeof job[0]));
		memcpy(&rs, r->data, sizeof rs);
		gfx_run(&chk.ppe, &job[1], &rp);
		uint32_t fv = 0, fc = 0;
		uint32_t dv = gfx_selftest_compare_vtx((const uint8_t *)chk.vtx[0], (const uint8_t *)chk.vtx[1],
						       rp.vtx_bytes, SELFTEST_ULPS, &fv);
		uint32_t dc = gfx_selftest_compare_cmd(chk.cmd[0], chk.cmd[1], rp.cmd_bytes, SELFTEST_ULPS, &fc);
		bool same = !r->error && rs.vtx_bytes == rp.vtx_bytes && rs.cmd_bytes == rp.cmd_bytes &&
			    rs.tex_bytes == rp.tex_bytes && rs.commands == rp.commands && rs.triangles == rp.triangles &&
			    rs.rects == rp.rects && rs.draws == rp.draws && rs.textures == rp.textures &&
			    rs.flags == rp.flags && rs.bg == rp.bg && rs.dl_errors == rp.dl_errors && !dv && !dc &&
			    !memcmp(chk.tex[0], chk.tex[1], rp.tex_bytes) && !memcmp(chk.cache[0], chk.cache[1], sizeof chk.cache[0]);
		if (!same) {
			salsa64_log("SPE 0: tarea de prueba gbi %u en la imagen %s: el SPE difiere del PPE (error %d; %u/%u "
				    "comandos, %u/%u dibujos, %u/%u triángulos, %u/%u rectángulos, %u/%u texturas, %u/%u "
				    "fondos, %u/%u errores, banderas %02x/%02x; bytes de vértices %u/%u, de comandos %u/%u, "
				    "del arena %u/%u; vértices %u palabras distintas desde el byte %u, comandos %u desde el %u; "
				    "arena %s, caché %s)",
				    (unsigned)gbis[i], gfx_spe0_name(img), (int)r->error, rs.commands, rp.commands, rs.draws,
				    rp.draws, rs.triangles, rp.triangles, rs.rects, rp.rects, rs.textures, rp.textures, rs.bg,
				    rp.bg, rs.dl_errors, rp.dl_errors, rs.flags, rp.flags, rs.vtx_bytes, rp.vtx_bytes,
				    rs.cmd_bytes, rp.cmd_bytes, rs.tex_bytes, rp.tex_bytes, dv, fv, dc, fc,
				    memcmp(chk.tex[0], chk.tex[1], rp.tex_bytes) ? "distinto" : "igual",
				    memcmp(chk.cache[0], chk.cache[1], sizeof chk.cache[0]) ? "distinta" : "igual");
			if (dv)
				log_words("vértices", (const uint8_t *)chk.vtx[0], (const uint8_t *)chk.vtx[1], fv, rp.vtx_bytes);
			if (dc)
				log_words("comandos", chk.cmd[0], chk.cmd[1], fc, rp.cmd_bytes);
			bad++;
		}
	}
	return bad;
}

/* Misma lista propia, sin caché y con RDP reiniciado en cada muestra.
 * Alternar el orden evita atribuir todo el calentamiento a un lado. */
static int check_detail(spukit_worker *w, struct detail_window *detail)
{
	uint32_t ticks[2][17];
	uint32_t dl = gfx_selftest_build(chk.ram, SALSA64_GBI_F3DEX2, 7);
	int bad = 0;
	for (unsigned sample = 0; sample < 17; sample++) {
		struct gfx_result out[2];
		for (unsigned pass = 0; pass < 2; pass++) {
			unsigned mode = pass ^ (sample & 1);
			struct gfx_job j;
			check_job(&j, mode, SALSA64_GBI_F3DEX2, 7, dl);
			j.flags = GFX_JOB_RESET | GFX_JOB_NOCACHE;
			if (mode) {
				j.detail_ea = (uint64_t)(uintptr_t)&detail->task;
				j.detail_cap = sizeof detail->task;
				j.detail_version = GFX_DETAIL_VERSION;
			}
			const spukit_result *r = spukit_wait(w, spukit_post(w, GFX_JOB_TASK, &j, sizeof j));
			ticks[mode][sample] = r->ticks;
			memcpy(&out[mode], r->data, sizeof out[mode]);
			if (r->error) bad++;
		}
		uint32_t first;
		if (out[0].vtx_bytes != out[1].vtx_bytes || out[0].cmd_bytes != out[1].cmd_bytes ||
		    out[0].tex_bytes != out[1].tex_bytes || out[0].flags != out[1].flags ||
		    out[0].triangles != out[1].triangles || out[0].commands != out[1].commands ||
		    gfx_selftest_compare_vtx((const uint8_t *)chk.vtx[0], (const uint8_t *)chk.vtx[1],
					    out[0].vtx_bytes, 0, &first) ||
		    gfx_selftest_compare_cmd(chk.cmd[0], chk.cmd[1], out[0].cmd_bytes, 0, &first) ||
		    memcmp(chk.tex[0], chk.tex[1], out[0].tex_bytes)) bad++;
	}
	for (unsigned mode = 0; mode < 2; mode++) {
		for (unsigned i = 1; i < 17; i++) {
			uint32_t value = ticks[mode][i], j = i;
			while (j && ticks[mode][j - 1] > value) { ticks[mode][j] = ticks[mode][j - 1]; j--; }
			ticks[mode][j] = value;
		}
	}
	double k = 1e3 / spukit_tick_hz();
	salsa64_log("gfx detalle: autoprueba A/B 17 pares, sin/con mediana %.3f/%.3f ms, "
		    "min %.3f/%.3f max %.3f/%.3f, diferencias %d (lista propia, no mide una carrera)",
		    ticks[0][8] * k, ticks[1][8] * k, ticks[0][0] * k, ticks[1][0] * k,
		    ticks[0][16] * k, ticks[1][16] * k, bad);
	return bad;
}

/* La lista propia de F3DEX2 trae cuatro vértices con niebla en cada G_VTX.
 * Se corre una vez por carriles y otra con GFX_JOB_SCALAR sobre el mismo SPE;
 * las salidas que consume el PPE deben coincidir palabra por palabra. Los
 * ticks no se comparan porque precisamente son el objeto del A/B. */
static int check_clip4(spukit_worker *w)
{
	static const float values[8] = { -65536.0f, -2.0f, -1.0f, -0.0f, 0.0f, 1.0f, 2.0f, 65536.0f };
	unsigned bad = 0;
	for (unsigned iw = 0; iw < 8; iw++)
	for (unsigned ix = 0; ix < 8; ix++) {
		struct gfx_clip4_job j;
		uint32_t ref[4], got[4];
		for (unsigned i = 0; i < 4; i++) {
			j.x[i] = values[(ix + i) & 7];
			j.y[i] = values[(ix + i + 1) & 7];
			j.z[i] = values[(ix + i + 3) & 7];
			j.w[i] = values[(iw + i) & 7];
			ref[i] = 0;
			if (j.x[i] < -j.w[i]) ref[i] |= GFX_CLIP_XN;
			if (j.x[i] > j.w[i]) ref[i] |= GFX_CLIP_XP;
			if (j.y[i] < -j.w[i]) ref[i] |= GFX_CLIP_YN;
			if (j.y[i] > j.w[i]) ref[i] |= GFX_CLIP_YP;
			if (j.z[i] > j.w[i]) ref[i] |= GFX_CLIP_ZF;
			if (j.w[i] < 0.0f) ref[i] |= GFX_CLIP_W;
		}
		const spukit_result *r = spukit_wait(w, spukit_post(w, GFX_JOB_CLIP4, &j, sizeof j));
		memcpy(got, r->data, sizeof got);
		bad += r->error != 0 || memcmp(got, ref, sizeof ref) != 0;
	}
	salsa64_log("gfx: autoprueba recorte SIMD: 64 lotes/256 vertices, %u diferencias", bad);
	return bad != 0;
}

static int check_vector(spukit_worker *w)
{
	struct gfx_result out[2];
	uint32_t ticks[2][17];
	uint32_t dl = gfx_selftest_build(chk.ram, SALSA64_GBI_F3DEX2, 11);
	int bad = 0;
	for (unsigned sample = 0; sample < 17; sample++) for (unsigned pass = 0; pass < 2; pass++) {
		unsigned mode = pass ^ (sample & 1);
		struct gfx_job j;
		check_job(&j, mode, SALSA64_GBI_F3DEX2, 11, dl);
		j.flags = GFX_JOB_RESET | GFX_JOB_NOCACHE | (mode ? GFX_JOB_SCALAR : 0);
		memset(chk.vtx[mode], 0, sizeof chk.vtx[mode]);
		memset(chk.cmd[mode], 0, sizeof chk.cmd[mode]);
		memset(chk.tex[mode], 0, sizeof chk.tex[mode]);
		memset(chk.cache[mode], 0, sizeof chk.cache[mode]);
		const spukit_result *r = spukit_wait(w, spukit_post(w, GFX_JOB_TASK, &j, sizeof j));
		memcpy(&out[mode], r->data, sizeof out[mode]);
		ticks[mode][sample] = r->ticks;
		bad += r->error != 0;
		if (pass) {
			struct gfx_result a = out[0], b = out[1];
			a.t_total = a.t_vtx = a.t_state = a.t_tex = a.t_load = a.t_hash = 0;
			b.t_total = b.t_vtx = b.t_state = b.t_tex = b.t_load = b.t_hash = 0;
			bad += memcmp(&a, &b, sizeof a) != 0 || out[0].vtx_bytes != out[1].vtx_bytes ||
				memcmp(chk.vtx[0], chk.vtx[1], out[0].vtx_bytes) != 0 ||
				memcmp(chk.cmd[0], chk.cmd[1], out[0].cmd_bytes) != 0 ||
				memcmp(chk.tex[0], chk.tex[1], out[0].tex_bytes) != 0 ||
				memcmp(chk.cache[0], chk.cache[1], sizeof chk.cache[0]) != 0;
		}
	}
	for (unsigned mode = 0; mode < 2; mode++)
		for (unsigned i = 1; i < 17; i++) {
			uint32_t value = ticks[mode][i], j = i;
			while (j && ticks[mode][j - 1] > value) { ticks[mode][j] = ticks[mode][j - 1]; j--; }
			ticks[mode][j] = value;
		}
	double k = 1e3 / spukit_tick_hz();
	salsa64_log("gfx: autoprueba vectorial/escalar (F3DEX2 con niebla): salida %s, mediana %.3f/%.3f ms (17 pares)",
		bad ? "DISTINTA" : "igual bit a bit", ticks[0][8] * k, ticks[1][8] * k);
	return bad;
}

/* Estadísticas de un trabajo que terminó (tarea o lista del CPU). */
static void account_res(struct salsa64_gfx_spu *s, const struct gfx_result *res, uint64_t ticks)
{
	s->spu_ticks += ticks;
	s->t_vtx += res->t_vtx;
	s->t_state += res->t_state;
	s->t_tex += res->t_tex;
	s->t_load += res->t_load;
	s->textures += res->textures;
	s->tex_reused += res->tex_reused;
	s->loads += res->loads;
	s->load_reads += res->load_reads;
	s->tex_old += res->tex_old;
	s->tex_arena += res->tex_arena;
	s->tex_evicted += res->tex_evicted;
	s->t_hash += res->t_hash;
	s->skipped += res->skipped;
	s->bg += res->bg;
	s->tex_target += res->tex_target;
}

/* Un trabajo que el SPE devolvió con error no dibujó (o no sabemos qué): termina
 * como cortado, con su interrupción del DP; sin ella el juego se quedaría
 * esperando al RDP para siempre. */
static void job_error(struct salsa64_gfx_spu *s, const spukit_result *r, struct gfx_result *res, const char *what)
{
	memset(res, 0, sizeof *res);
	res->flags |= GFX_RES_CUT;
	if (s->errors++ < 4)
		salsa64_log("gfx: el SPE devolvió el error %d en %s (van %u): termina como cortada", (int)r->error, what,
			    s->errors);
}

static void account(struct salsa64_gfx_spu *s, uint64_t ticks)
{
	s->tasks++;
	account_res(s, &s->res, ticks);
	detail_account(s, ticks);
}

/* ---- Imágenes del SPE 0 ------------------------------------------------------ */

/* Lo que pasa de una imagen a la siguiente: el tramo crudo del estado del RDP
 * (en `shadow`, como los estados guardados) y las color images dibujadas. */
static struct gfx_state shadow __attribute__((aligned(128)));
static uint8_t swap_fb[GFX_FB_BYTES] __attribute__((aligned(128)));

static bool spu_info(spukit_worker *w, struct gfx_info *in)
{
	const spukit_result *r = spukit_wait(w, spukit_post(w, GFX_JOB_INFO, NULL, 0));
	memcpy(in, r->data, sizeof *in);
	return !r->error;
}

/* Pila que el SPE nunca tocó desde que arrancó la imagen (medida en el SPE). */
static void log_untouched(struct salsa64_gfx_spu *s, const char *when)
{
	struct gfx_info in;
	if (s->worker && spu_info(s->worker, &in))
		salsa64_log("SPE 0: imagen %s %s: %u bytes de pila sin tocar nunca (%s; holgura del build %d; %u tareas "
			    "en esta imagen)", gfx_spe0_name(s->image), when, in.untouched,
			    in.filled ? "con el patrón" : "en cero, aproximado", (int)image_data(s, s->image)->slack,
			    s->tasks - s->image_tasks0);
}

/* Diagnóstico: una tarea cortada, con errores de lista o con cientos de miles de
 * comandos (Indiana Jones se quedaba en negro tras una tarea de 75 ms en el SPE:
 * una lista que el PPE y el SPE no ven igual, o una lista de ceros). Anota qué ve
 * el PPE de la lista al terminar y qué leyó el SPE al empezar, para distinguir
 * una carrera con el CPU emulado de una lista mala del juego. Solo las primeras
 * 16, y con la tarea ya recogida (el SPE está libre). */
static void log_bad_task(struct salsa64_gfx_spu *s, const struct gfx_job *job, const struct gfx_result *res,
			 const char *what)
{
	if (!(res->flags & GFX_RES_CUT) && !res->dl_errors && res->commands < 200000u)
		return;
	if (s->bad_logged++ >= 16)
		return;
	uint32_t ppe[8] = { 0 }, dl = job->dl & 0x00FFFFFFu;
	if (dl + 32 <= job->rdram_size)
		for (unsigned i = 0; i < 8; i++)
			ppe[i] = salsa64_rd32(s->bus->rdram + dl + 4 * i);
	struct gfx_info in;
	memset(&in, 0, sizeof in);
	bool ok = s->worker && spu_info(s->worker, &in);
	salsa64_log("gfx: %s mala (la %u): lista %06x (ucode %06x, gbi %u, banderas 0x%x), %u comandos, %u triángulos, "
		    "%u dibujos, %u errores de lista, %u sin implementar (último 0x%02x), banderas del resultado 0x%x, "
		    "%.2f ms en el SPE; el PPE ve la lista %08x %08x %08x %08x %08x %08x %08x %08x; %s",
		    what, s->bad_logged, dl, job->ucode, job->gbi, job->flags, res->commands, res->triangles, res->draws,
		    res->dl_errors, res->unknown_cmds, res->last_unknown, res->flags, res->t_total / 1000.0 / 79.8,
		    ppe[0], ppe[1], ppe[2], ppe[3], ppe[4], ppe[5], ppe[6], ppe[7], ok ? "" : "sin la vista del SPE");
	if (ok)
		salsa64_log("gfx:   el SPE leyó primero en %06x%s: %08x %08x %08x %08x", in.dl_addr & 0x00FFFFFFu,
			    in.dl_addr == ~0u ? " (ninguna lectura)" : "", in.dl_first[0], in.dl_first[1], in.dl_first[2],
			    in.dl_first[3]);
}

/* Carga la imagen `img` (con la tarea en vuelo ya recogida). Si había otra,
 * su estado del RDP pasa a la nueva. 0 o el error de spukit (y sin
 * trabajador). */
static int load_image(struct salsa64_gfx_spu *s, int img, uint32_t gbi)
{
	if (img < 0 || img >= GFX_SPE0_IMAGES)
		return SPUKIT_EARG;
	uint64_t t0 = sysGetSystemTime();
	const struct gfx_spu_image *image = image_data(s, img);
	int from = s->worker ? s->image : -1;
	bool carry = false;
	if (s->worker) {
		log_untouched(s, "antes de cambiar");
		struct gfx_state_job sj = { (uint64_t)(uintptr_t)((uint8_t *)&shadow + GFX_RAW_START),
					    (uint64_t)(uintptr_t)swap_fb, 0, 0, 0 };
		const spukit_result *r = spukit_wait(s->worker, spukit_post(s->worker, GFX_JOB_STATE_SAVE, &sj, sizeof sj));
		uint32_t where[2];
		memcpy(where, r->data, sizeof where);
		carry = !r->error && where[0] == GFX_RAW_START && where[1] == GFX_RAW_END;
		spukit_worker_stop(s->worker);
		s->worker = NULL;
	}
	uint64_t t1 = sysGetSystemTime();
	struct stat st;
	s->worker = spukit_worker_start(image->bin, "salsa64_gfx", stat(SPE0_STACK_PATH, &st) == 0);
	if (!s->worker) {
		salsa64_log("SPE 0: no arrancó la imagen %s (%d)", gfx_spe0_name(img), spukit_last_error());
		s->image = -1;
		return spukit_last_error();
	}
	uint64_t t2 = sysGetSystemTime();
	s->image = img;
	s->image_tasks0 = s->tasks;
	/* Comprobaciones contra el PPE: el hash de las texturas y las tareas de
	 * prueba de los microcódigos de la imagen. */
	int bad_hash = check_hash(s->worker);
	int bad_lists = check_lists(s->worker, img, s->detail);
	if (img == 1 && !s->vector_checked) {
		bad_lists += check_vector(s->worker);
		s->vector_checked = true;
	}
	if (img == 1 && s->detail_phase == 8) bad_lists += check_clip4(s->worker);
	if (img == 1 && s->detail && !s->detail_checked) {
		bad_lists += check_detail(s->worker, s->detail);
		s->detail_checked = true;
	}
	s->nocache = bad_hash != 0;
	if (bad_lists)
		s->selftest_bad |= 1u << img;
	/* Lo que dejó la prueba se olvida: el estado del RDP a cero (y las color
	 * images); la caché de texturas apunta a la región de la prueba y la
	 * primera tarea del juego la vacía. */
	struct gfx_job reset;
	memset(&reset, 0, sizeof reset);
	reset.rdram_ea = (uint64_t)(uintptr_t)chk.ram;
	reset.rdram_size = GFX_SELFTEST_RAM;
	reset.flags = GFX_JOB_DPC | GFX_JOB_RESET;
	spukit_wait(s->worker, spukit_post(s->worker, GFX_JOB_TASK, &reset, sizeof reset));
	uint64_t t3 = sysGetSystemTime();
	bool carried = false;
	if (carry) {
		struct gfx_state_job sj = { (uint64_t)(uintptr_t)((uint8_t *)&shadow + GFX_RAW_START),
					    (uint64_t)(uintptr_t)swap_fb, s->job.cache_ea, s->job.cache_cap, s->last_task };
		const spukit_result *r = spukit_wait(s->worker, spukit_post(s->worker, GFX_JOB_STATE_LOAD, &sj, sizeof sj));
		carried = !r->error;
	}
	uint64_t t4 = sysGetSystemTime();
	struct gfx_info in;
	memset(&in, 0, sizeof in);
	spu_info(s->worker, &in);
	if (from >= 0) {
		s->switches++;
		s->switch_us += t4 - t0;
	}
	salsa64_log("SPE 0: imagen %s (%s, %u bytes del ELF) %s gbi %u: local store %u de 262144 bytes "
		    "hasta _end, caché de texturas %u entradas, pila: la cadena más honda %u y holgura %d (build), "
		    "%u sin tocar al cargar; hash %s, tareas de prueba %s",
		    gfx_spe0_name(img), image->program, (unsigned)*image->size, from >= 0 ? "en lugar de la anterior para" : "para",
		    (unsigned)gbi, in.end, in.tc_entries, image->stack, (int)image->slack, in.untouched,
		    bad_hash ? "DISTINTO del PPE (sin caché de texturas)" : "igual al del PPE",
		    bad_lists ? "DISTINTAS del PPE" : "iguales al PPE");
	if (in.image != (uint32_t)img || in.raw_start != GFX_RAW_START || in.raw_end != GFX_RAW_END)
		salsa64_log("SPE 0: ATENCIÓN, la imagen dice ser la %u con el tramo del RDP en %u–%u (aquí %u–%u)",
			    in.image, in.raw_start, in.raw_end, (unsigned)GFX_RAW_START, (unsigned)GFX_RAW_END);
	if (from >= 0)
		salsa64_log("SPE 0: cambio de imagen %s → %s en la tarea %u (cambio número %u): %.2f ms (guardar y parar "
			    "%.2f, arrancar %.2f, comprobar %.2f, estado %.2f); estado del RDP %s, caché de texturas vacía "
			    "con barrera hasta la tarea %u",
			    gfx_spe0_name(from), gfx_spe0_name(img), s->last_task + 1, s->switches, (t4 - t0) / 1e3,
			    (t1 - t0) / 1e3, (t2 - t1) / 1e3, (t3 - t2) / 1e3, (t4 - t3) / 1e3,
			    carried ? "llevado" : "PERDIDO", s->last_task);
	else
		salsa64_log("SPE 0: arranque de la imagen %s: %.2f ms (arrancar %.2f, comprobar %.2f)", gfx_spe0_name(img),
			    (t4 - t0) / 1e3, (t2 - t1) / 1e3, (t3 - t2) / 1e3);
	return 0;
}

/* La imagen que hace las tareas de `gbi` (o la que haya, para lo que no es de
 * ninguna familia: listas del CPU, estados guardados), cargada. false si no
 * hay trabajador. */
static bool need_image(struct salsa64_gfx_spu *s, uint32_t gbi)
{
	int img = gfx_spe0_image(gbi);
	if (img < 0)
		img = s->image >= 0 ? s->image : GFX_SPE0_F3D;
	if (!s->worker || s->image != img)
		load_image(s, img, gbi);
	return s->worker != NULL;
}

static void spu_start(void *ctx, const struct salsa64_task *task, enum salsa64_gbi gbi)
{
	struct salsa64_gfx_spu *s = ctx;
	s->late_started = false;
	if (!need_image(s, gbi)) {
		/* Sin SPE: la tarea termina cortada (spu_finish), con su DP. */
		memset(&s->job, 0, sizeof s->job);
		s->pending = false;
		s->no_worker = true;
		return;
	}
	salsa64_gfx_job(&s->job, s->bus, task, gbi);
	s->sink.acquire(s->sink.ctx, &s->job);
	s->last_task = s->job.task;
	if (s->nocache)
		s->job.flags |= GFX_JOB_NOCACHE;
	if (s->scalar)
		s->job.flags |= GFX_JOB_SCALAR;
	if (s->detail && s->image == 1) {
		struct detail_window *w = s->detail;
		w->task.version = 0;  /* detectar resultado ausente o viejo */
		s->job.detail_ea = (uint64_t)(uintptr_t)&w->task;
		s->job.detail_cap = sizeof w->task;
		s->job.detail_version = GFX_DETAIL_VERSION;
	}
	s->seq = spukit_post(s->worker, GFX_JOB_TASK, &s->job, sizeof s->job);
	s->pending = true;
}

#define POLL_SLEEP_US 100

static void late_account(struct salsa64_gfx_spu *s, uint64_t cycles)
{
	uint64_t bin = cycles / GFX_LATE_BIN_CYCLES;
	if (bin >= GFX_LATE_HIST_N)
		bin = GFX_LATE_HIST_N - 1;
	s->late_hist[bin]++;
}

static bool spu_done(void *ctx, bool idle)
{
	struct salsa64_gfx_spu *s = ctx;
	if (!s->pending)
		return true;
	uint64_t now = s->bus->cpu ? s->bus->cpu->cycles : 0;
	if (!s->late_started) {
		s->late_cycle0 = now;
		s->late_started = true;
	}
	uint64_t late = now - s->late_cycle0;
	if (spukit_done(s->worker, s->seq)) {
		late_account(s, late);
		return true;
	}
	s->polls++;
	/* El SPE conserva un campo entero de solape adicional. Si tampoco acabó,
	 * el PPE deja de adelantar el reloj emulado y lo espera aquí. */
	if (!s->poll_forever && late >= s->max_late_cycles) {
		uint64_t t0 = sysGetSystemTime();
		s->waits++;
		while (!spukit_done(s->worker, s->seq))
			sysUsleep(POLL_SLEEP_US);
		s->wait_us += sysGetSystemTime() - t0;
		late_account(s, late);
		return true;
	}
	if (idle) {
		sysUsleep(POLL_SLEEP_US);
		s->sleep_us += POLL_SLEEP_US;
		if (spukit_done(s->worker, s->seq)) {
			late_account(s, late);
			return true;
		}
	}
	return false;
}

static uint32_t spu_finish(void *ctx)
{
	struct salsa64_gfx_spu *s = ctx;
	if (s->no_worker) {
		s->no_worker = false;
		memset(&s->res, 0, sizeof s->res);  /* sin resultado: el ritmo del RCP cuenta solo la base */
		return SALSA64_GFX_FULLSYNC | SALSA64_GFX_CUT;
	}
	if (!s->pending)
		return 0;
	s->pending = false;
	uint64_t t0 = 0;
	if (!spukit_done(s->worker, s->seq)) {
		t0 = sysGetSystemTime();
		s->waits++;
	}
	const spukit_result *r = spukit_wait(s->worker, s->seq);
	if (t0)
		s->wait_us += sysGetSystemTime() - t0;
	memcpy(&s->res, r->data, sizeof s->res);
	if (r->error)
		job_error(s, r, &s->res, "una tarea");
	account(s, r->ticks);
	log_bad_task(s, &s->job, &s->res, "tarea");
	salsa64_gfx_rdram_fills(s->bus, &s->job, &s->res);
	s->sink.done(s->sink.ctx, &s->job, &s->res);
	return salsa64_gfx_task_flags(&s->res);
}

/* Ritmo del RCP (rsp.c, gfx_model): lo que tardaría la consola en la tarea
 * que acaba de recoger spu_finish, con su resultado. */
static uint32_t spu_cycles(void *ctx)
{
	struct salsa64_gfx_spu *s = ctx;
	return salsa64_gfx_model_cycles(&s->res, (enum salsa64_gbi)s->job.gbi);
}

/* Lista del RDP que escribe el CPU: el mismo SPE y el mismo estado que las
 * tareas (el de las dos HLE separadas se pisaba la caché de texturas: los
 * fondos de Killer Instinct Gold salían rotos). La tarea en vuelo, si la
 * hay, se recoge antes para que su salida y la de la lista lleguen al
 * consumidor en el orden en que se dieron. Síncrono: el CPU espera su
 * SYNC_FULL (la interrupción del DP) y la lista es corta. */
static uint32_t spu_dpc_run(void *ctx, uint32_t start, uint32_t end, const uint8_t *dmem)
{
	struct salsa64_gfx_spu *s = ctx;
	if (dmem && s->xbus) {
		s->xbus_runs++;
		if (s->pending) {
			s->xbus_inflight++;
			if (s->xbus_safe) {
				uint64_t t0 = sysGetSystemTime();
				salsa64_rsp_quiesce((struct salsa64_bus *)s->bus);
				s->xbus_waits++;
				s->xbus_wait_us += sysGetSystemTime() - t0;
			}
		}
		return s->xbus->run(s->xbus->ctx, start, end, dmem);
	}
	if (s->pending)
		salsa64_rsp_quiesce((struct salsa64_bus *)s->bus);
	if (!need_image(s, SALSA64_GBI_UNKNOWN))
		return SALSA64_GFX_FULLSYNC;
	struct gfx_job job;
	struct gfx_result res;
	memset(&job, 0, sizeof job);
	job.rdram_ea = (uint64_t)(uintptr_t)s->bus->rdram;
	job.rdram_size = s->bus->rdram_size;
	job.dmem_ea = (uint64_t)(uintptr_t)dmem;
	job.dl = start;
	job.dl_end = end;
	job.flags = GFX_JOB_DPC | (dmem ? GFX_JOB_XBUS : 0);
	if (s->dpc_open) {  /* misma tarea que el tramo anterior (gfx.h) */
		job.flags |= GFX_JOB_DPC_CONT;
		s->dpc_conts++;
	}
	s->sink.acquire(s->sink.ctx, &job);
	s->last_task = job.task;
	if (s->nocache)
		job.flags |= GFX_JOB_NOCACHE;
	uint64_t t0 = sysGetSystemTime();
	const spukit_result *r = spukit_wait(s->worker, spukit_post(s->worker, GFX_JOB_TASK, &job, sizeof job));
	s->dpc_us += sysGetSystemTime() - t0;
	memcpy(&res, r->data, sizeof res);
	if (r->error)
		job_error(s, r, &res, "una lista del CPU");
	s->dpc_jobs++;
	account_res(s, &res, r->ticks);
	log_bad_task(s, &job, &res, "lista del CPU");
	salsa64_gfx_rdram_fills(s->bus, &job, &res);
	s->sink.done(s->sink.ctx, &job, &res);
	s->dpc_open = !(res.flags & (GFX_RES_FULLSYNC | GFX_RES_CUT));
	return (res.flags & (GFX_RES_FULLSYNC | GFX_RES_CUT)) ? SALSA64_GFX_FULLSYNC : 0;
}


/* Estados guardados: el SPE copia el tramo crudo de su gfx_state (registros
 * del RDP, tiles y TMEM) al mismo lugar de esta imagen, o lo trae de ella; el
 * formato portable se arma aquí (rsp/gfx/state.c). Si el SPE dice que el
 * tramo está en otro lugar de su struct (otra disposición), no se guarda ni
 * se carga nada de la HLE. Sin tarea en vuelo. */
static bool spu_state(struct salsa64_gfx_spu *s, uint32_t kind)
{
	if (s->pending || !need_image(s, SALSA64_GBI_UNKNOWN))
		return false;
	struct gfx_state_job sj = { (uint64_t)(uintptr_t)((uint8_t *)&shadow + GFX_RAW_START), 0, 0, 0, 0 };
	const spukit_result *r = spukit_wait(s->worker, spukit_post(s->worker, kind, &sj, sizeof sj));
	uint32_t where[2];
	memcpy(where, r->data, sizeof where);
	if (r->error || where[0] != GFX_RAW_START || where[1] != GFX_RAW_END) {
		salsa64_log("estado de la HLE gráfica: el SPE no lo dio (error %d, tramo %u–%u en el SPE, %u–%u aquí)",
			    r->error, where[0], where[1], (unsigned)GFX_RAW_START, (unsigned)GFX_RAW_END);
		return false;
	}
	return true;
}

static bool spu_save_state(void *ctx, uint8_t *buf)
{
	if (!spu_state(ctx, GFX_JOB_STATE_SAVE))
		return false;
	gfx_state_save(&shadow, buf);
	return true;
}

static bool spu_load_state(void *ctx, const uint8_t *buf)
{
	/* Lo que comparte el tramo y no es del formato (tex_gen, el final del
	 * estado de Rare) se conserva: primero se trae lo que tiene el SPE. */
	if (!spu_state(ctx, GFX_JOB_STATE_SAVE) || !gfx_state_load(&shadow, buf))
		return false;
	return spu_state(ctx, GFX_JOB_STATE_LOAD);
}

int salsa64_gfx_spu_init(struct salsa64_gfx_spu *s, const struct salsa64_bus *bus,
			 const struct salsa64_gfx_sink *sink)
{
	memset(s, 0, sizeof *s);
	s->backend.ctx = s;
	s->backend.start = spu_start;
	s->backend.finish = spu_finish;
	s->backend.cycles = spu_cycles;
	s->backend.state_size = GFX_STATE_BYTES;
	s->backend.save_state = spu_save_state;
	s->backend.load_state = spu_load_state;
	s->backend.s2dex = true;
	s->dpc.ctx = s;
	s->dpc.run = spu_dpc_run;
	s->bus = bus;
	s->sink = *sink;
	s->image = -1;
	/* Con el archivo spe0-todas, al arrancar se cargan y comprueban todas
	 * las imágenes, una tras otra (y el cambio entre ellas, con su costo):
	 * para probarlas en RPCS3 con las ROMs de prueba, que usan solo una
	 * familia. Después se sigue como siempre, con la primera. */
	struct stat st;
	/* La hora emulada del fin no depende de cuánto tarde el SPE. Al llegar a
	 * ella se conserva hasta un campo de solape; después el PPE espera sin
	 * adelantar más el reloj de la N64. El sondeo anterior y la espera desde
	 * el primer plazo quedan como comparaciones. */
	s->max_late_cycles = bus->vi.field_cycles ? bus->vi.field_cycles : 1562500u;
	if (stat(SPE0_POLL_PATH, &st) == 0) {
		s->poll_forever = true;
		salsa64_log("gfx: fin de tarea sondeado (gfx-sondeo)");
	} else if (stat(SPE0_WAIT_PATH, &st) == 0) {
		s->max_late_cycles = 0;
		salsa64_log("gfx: fin de tarea con espera desde el primer plazo (gfx-espera-siempre)");
	} else {
		salsa64_log("gfx: fin de tarea híbrido; hasta %u ciclos de solape antes de esperar al SPE",
			    s->max_late_cycles);
	}
	s->backend.done = spu_done;
	if (stat(SPE0_CLIP4_PATH, &st) == 0) {
		s->detail_phase = 8;
		salsa64_log("gfx: recorte SIMD experimental (gfx-recorte-vector), mide lote completo; sustituye gfx-fase");
	}
	if (stat(SPE0_DETAIL_PATH, &st) == 0 || s->detail_phase) {
		s->detail = memalign(128, sizeof(struct detail_window));
		if (s->detail) memset(s->detail, 0, sizeof(struct detail_window));
		salsa64_log("gfx detalle: %s (solo imagen 1, fase %u: %s)",
			s->detail ? "activado" : "sin memoria, desactivado", s->detail_phase,
			detail_phase_name(s->detail_phase));
	}
	s->scalar = stat(SPE0_SCALAR_PATH, &st) == 0;
	salsa64_log("gfx: geometria del SPE: %s", s->scalar ? "escalar (gfx-escalar)" : "vectorial");
	if (stat(SPE0_ALL_PATH, &st) == 0) {
		salsa64_log("SPE 0: %s: cargo y compruebo las %d imágenes", SPE0_ALL_PATH, GFX_SPE0_IMAGES);
		for (int i = GFX_SPE0_IMAGES - 1; i > 0; i--) {
			int rc = load_image(s, i, SALSA64_GBI_UNKNOWN);
			if (rc)
				return rc;
		}
		salsa64_log("SPE 0: imágenes con tareas de prueba distintas del PPE: %s",
			    s->selftest_bad ? "ALGUNA (ver arriba)" : "ninguna");
	}
	/* La primera imagen, para saber ya si el SPE arranca (si no, la HLE va
	 * en el PPE); la primera tarea de otra familia la cambia. */
	int rc = load_image(s, GFX_SPE0_F3D, SALSA64_GBI_UNKNOWN);
	s->switches = 0;
	s->switch_us = 0;
	return rc;
}

void salsa64_gfx_spu_shutdown(struct salsa64_gfx_spu *s)
{
	if (s->pending)
		spu_finish(s);
	if (s->worker) {
		log_untouched(s, "al cerrar");
		if (s->switches)
			salsa64_log("SPE 0: %u cambios de imagen, %.2f ms en total", s->switches, s->switch_us / 1e3);
		spukit_worker_stop(s->worker);
	}
	s->worker = NULL;
	detail_log(s->detail, s->detail_phase);
	free(s->detail);
	s->detail = NULL;
}
