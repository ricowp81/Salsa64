/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — caja negra en la PS3. Ver cajanegra.h.
 */
#include "cajanegra.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/systime.h>

#include "core/endian.h"
#include "core/rom.h"
#include "osd.h"
#include "platform/ps3/bgio.h"
#include "platform/ps3/bgjob.h"
#include "platform/ps3/log.h"
#include "rdp/blackbox.h"
#include "rdp/rdp.h"

static inline void lwsync(void) { __asm__ volatile("lwsync" ::: "memory"); }

/* Los dibujos de la tarea más grande que la HLE puede producir (rdp.c:
 * CMD_BYTES; los registros de la interpolación van en el lugar de más). */
#define CMD_MAX (768u << 10)
/* Cabecera de la ROM (como core/state.c). */
#define ROM_CRC1 0x10
#define ROM_CRC2 0x14
#define ROM_TITLE 0x20
#define ROM_TITLE_END 0x34

static struct {
	bool on;
	const struct salsa64_bus *bus;
	struct bb ring;
	uint8_t *mem;
	uint32_t mb;
	uint32_t *wsrc;
	const struct gfx_tex **wtgt;
	struct gfx_dump_work work;
	uint32_t cur_field;
	/* Escritura */
	bool want;              /* pedida por el atajo, aún sin arrancar */
	volatile int frozen;    /* el anillo se está escribiendo: no recibe nada */
	volatile int pending;   /* hay un trabajo en marcha o sin recoger */
	volatile int finished;
	struct bb_meta meta;
	char dir[64], tmp[104], path[96];
	struct {
		bool ok;
		char err[160];
		uint64_t us_total, us_io, us_spu;
		uint64_t bytes, raw;
		uint32_t tasks, fields, notes, dropped;
		uint32_t blocks, stored;
		bool spu;
	} res;
	uint64_t t_submit;
	uint32_t asked_field;
	uint32_t auto_at;       /* campo en que se pide sola (pruebas automáticas), o 0 */
	/* Costo de registrar (hilo de presentación) */
	uint64_t us_task, us_field;
	uint32_t n_task, n_task_fail, n_field;
	uint32_t max_task_us;
	/* Lo que el interpolador dejó para el siguiente campo */
	bool extra;
	uint32_t x_kind, x_t, x_k, x_src, x_prev;
} cj;

static void task_hook(void *ctx, uint32_t task, const struct gfx_dump_src *src, const struct gfx_result *res)
{
	(void)ctx;
	if (!cj.on || cj.frozen)
		return;
	uint64_t t0 = sysGetSystemTime();
	if (!bb_add_task(&cj.ring, task, cj.cur_field, src, res, &cj.work))
		cj.n_task_fail++;
	uint64_t us = sysGetSystemTime() - t0;
	cj.us_task += us;
	cj.n_task++;
	if (us > cj.max_task_us)
		cj.max_task_us = (uint32_t)us;
}

bool cajanegra_init(const struct salsa64_bus *bus)
{
	cajanegra_shutdown(0);
	salsa64_rdp_set_task_hook(NULL, NULL);
	FILE *f = fopen(CAJANEGRA_CFG, "r");
	if (!f)
		return false;
	unsigned mb = CAJANEGRA_MB_DEFAULT, auto_at = 0;
	int got = fscanf(f, "%u %u", &mb, &auto_at);
	if (got < 1)
		mb = CAJANEGRA_MB_DEFAULT;
	if (got < 2)
		auto_at = 0;
	fclose(f);
	if (mb < CAJANEGRA_MB_MIN)
		mb = CAJANEGRA_MB_MIN;
	if (mb > CAJANEGRA_MB_MAX)
		mb = CAJANEGRA_MB_MAX;
	uint32_t max_tex = GFX_DUMP_MAX_TEX(CMD_MAX);
	cj.mem = memalign(128, (mb << 20) + BB_SLACK);
	cj.wsrc = malloc(max_tex * 2 * sizeof *cj.wsrc);
	cj.wtgt = malloc(max_tex * sizeof *cj.wtgt);
	if (!cj.mem || !cj.wsrc || !cj.wtgt) {
		salsa64_log("caja negra: sin memoria para el anillo de %u MB: apagada", mb);
		free(cj.mem);
		free(cj.wsrc);
		free(cj.wtgt);
		memset(&cj, 0, sizeof cj);
		return false;
	}
	memset(cj.mem, 0, (size_t)mb << 20);  /* tocado ya: el primer registro no paga cada página */
	cj.mb = mb;
	cj.auto_at = auto_at;
	cj.bus = bus;
	cj.work = (struct gfx_dump_work){ cj.wsrc, cj.wsrc + max_tex, cj.wtgt, max_tex };
	bb_init(&cj.ring, cj.mem, mb << 20);
	cj.cur_field = 0;
	cj.on = true;
	bgjob_init();
	salsa64_rdp_set_task_hook(task_hook, NULL);
	salsa64_log("caja negra: encendida, anillo de %u MB; SELECT + L2 escribe %s en %s%s", mb, CAJANEGRA_FILE,
		    CAJANEGRA_DIR, auto_at ? " (y sola en el campo indicado: prueba automática)" : "");
	if (auto_at)
		salsa64_log("caja negra: se pedirá sola al llegar al campo %u", auto_at);
	return true;
}

uint32_t cajanegra_reserved(void)
{
	return cj.mem ? (cj.mb << 20) + BB_SLACK : 0;
}

void cajanegra_shutdown(uint64_t wait_us)
{
	if (!cj.on && !cj.mem)
		return;
	if (cj.pending && bgjob_ready() && !bgjob_wait(wait_us ? wait_us : 1)) {
		salsa64_log("caja negra: la escritura no terminó a tiempo: el anillo se queda");
		return;
	}
	if (cj.n_task || cj.n_field)
		salsa64_log("caja negra: al cerrar, %u tareas registradas (%.1f µs cada una, la más lenta %u µs, %u sin lugar) "
			    "y %u campos (%.1f µs cada uno); en el anillo %u tareas, %u campos, %u descartadas",
			    cj.n_task, cj.n_task ? (double)cj.us_task / cj.n_task : 0.0, cj.max_task_us, cj.n_task_fail,
			    cj.n_field, cj.n_field ? (double)cj.us_field / cj.n_field : 0.0, cj.ring.ntask, cj.ring.nfield,
			    cj.ring.dropped_tasks);
	salsa64_rdp_set_task_hook(NULL, NULL);
	free(cj.mem);
	free(cj.wsrc);
	free(cj.wtgt);
	memset(&cj, 0, sizeof cj);
}

bool cajanegra_enabled(void)
{
	return cj.on;
}

void cajanegra_set_field(uint32_t field)
{
	cj.cur_field = field;
}

void cajanegra_field_interp(uint32_t kind, float t, uint32_t k, uint32_t src_task, uint32_t prev_task)
{
	if (!cj.on)
		return;
	cj.extra = true;
	cj.x_kind = kind;
	memcpy(&cj.x_t, &t, sizeof cj.x_t);
	cj.x_k = k;
	cj.x_src = src_task;
	cj.x_prev = prev_task;
}

void cajanegra_field(const struct cajanegra_field *f)
{
	if (!cj.on)
		return;
	uint32_t extra_kind = cj.extra ? cj.x_kind : BB_KIND_TRUE, extra_t = cj.extra ? cj.x_t : 0,
		 extra_k = cj.extra ? cj.x_k : 0, extra_src = cj.extra ? cj.x_src : BB_TASK_NONE,
		 extra_prev = cj.extra ? cj.x_prev : BB_TASK_NONE;
	cj.extra = false;
	if (cj.frozen)
		return;
	uint64_t t0 = sysGetSystemTime();
	uint32_t w[BB_FLD_WORDS];
	memset(w, 0, sizeof w);
	w[BB_FLD_FIELD] = f->field;
	w[BB_FLD_FLAGS] = f->flags;
	w[BB_FLD_TASKS] = f->tasks;
	w[BB_FLD_TRANSLATED] = salsa64_rdp_translated();
	w[BB_FLD_VI_TYPE] = f->vi_type;
	w[BB_FLD_VI_ORIGIN] = f->vi_origin;
	w[BB_FLD_VI_WIDTH] = f->vi_width;
	w[BB_FLD_VI_HEIGHT] = f->vi_height;
	w[BB_FLD_VI_STRIDE] = f->vi_stride;
	w[BB_FLD_VI_BPP] = f->vi_bpp;
	if (f->vi_regs)
		memcpy(&w[BB_FLD_VI_REGS], f->vi_regs, SALSA64_VI_NREGS * sizeof(uint32_t));
	w[BB_FLD_KIND] = extra_kind;
	w[BB_FLD_T] = extra_t;
	w[BB_FLD_K] = extra_k;
	w[BB_FLD_BEHIND] = f->behind;
	uint64_t now = t0;
	w[BB_FLD_TIME_HI] = (uint32_t)(now >> 32);
	w[BB_FLD_TIME_LO] = (uint32_t)now;
	w[BB_FLD_SRC_TASK] = extra_src != BB_TASK_NONE ? extra_src : salsa64_rdp_translated() - 1;
	w[BB_FLD_PREV_TASK] = extra_prev;
	bb_add_field(&cj.ring, w);
	cj.us_field += sysGetSystemTime() - t0;
	cj.n_field++;
}

void cajanegra_note(uint32_t tag, const void *data, uint32_t len)
{
	if (cj.on && !cj.frozen)
		bb_add_note(&cj.ring, tag, data, len);
}

void cajanegra_request(void)
{
	if (!cj.on) {
		osd_notice(sysGetSystemTime(), "Caja negra apagada (falta el archivo cajanegra)");
		return;
	}
	if (cj.want || cj.pending) {
		osd_notice(sysGetSystemTime(), "Caja negra: ya está escribiendo");
		return;
	}
	cj.want = true;
	cj.asked_field = cj.cur_field;
}

/* El trabajo del hilo de escritura: el anillo entero, comprimido, a un .tmp
 * que se renombra al final. */
static void cj_write(void *arg)
{
	(void)arg;
	sysLv2FsMkdir(cj.dir, 0777);
	struct bgio_out io;
	bgio_lz_reset();
	char err[160] = "";
	bool ok = bgio_out_open(&io, cj.tmp);
	uint64_t raw = 0;
	if (!ok)
		snprintf(err, sizeof err, "no se pudo crear %s", cj.tmp);
	else
		ok = bb_write(&cj.ring, &cj.meta, bgio_compress, NULL, bgio_out_write, &io, &raw);
	if (!ok && !err[0])
		snprintf(err, sizeof err, "no se pudo comprimir o escribir");
	char ferr[160] = "";
	ok = bgio_out_finish(&io, ok, cj.tmp, cj.path, ferr, sizeof ferr);
	if (!ok && !err[0])
		snprintf(err, sizeof err, "%s", ferr);
	struct bgio_lz_stats st;
	bgio_lz_get(&st);
	cj.res.ok = ok;
	memcpy(cj.res.err, err, sizeof cj.res.err);
	cj.res.us_io = io.us;
	cj.res.us_spu = st.spu_us;
	cj.res.bytes = io.bytes;
	cj.res.raw = raw;
	cj.res.blocks = st.blocks;
	cj.res.stored = st.stored;
	cj.res.spu = st.spu;
	cj.res.us_total = sysGetSystemTime() - cj.t_submit;
	lwsync();
	cj.finished = 1;
}

bool cajanegra_write_now(const char *path, uint64_t wait_us, char *err, unsigned err_size)
{
	char dummy[8];
	if (!err) {
		err = dummy;
		err_size = sizeof dummy;
	}
	err[0] = '\0';
	if (!cj.on) {
		snprintf(err, err_size, "caja negra apagada (falta %s)", CAJANEGRA_CFG);
		return false;
	}
	if ((cj.pending || bgjob_busy()) && bgjob_ready() && !bgjob_wait(wait_us ? wait_us : 1)) {
		snprintf(err, err_size, "una escritura en segundo plano no terminó a tiempo");
		return false;
	}
	cj.frozen = 1;
	struct bb_meta meta;
	memset(&meta, 0, sizeof meta);
	meta.field = cj.cur_field;
	if (cj.bus && cj.bus->rom && cj.bus->rom_size >= ROM_TITLE_END) {
		meta.crc1 = salsa64_rd32(cj.bus->rom + ROM_CRC1);
		meta.crc2 = salsa64_rd32(cj.bus->rom + ROM_CRC2);
		memcpy(meta.title, cj.bus->rom + ROM_TITLE, sizeof meta.title);
	}
	char tmp[160];
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	struct bgio_out io;
	uint64_t raw = 0;
	bool ok = bgio_out_open(&io, tmp);
	if (!ok)
		snprintf(err, err_size, "no se pudo crear %s", tmp);
	else if (!(ok = bb_write(&cj.ring, &meta, NULL, NULL, bgio_out_write, &io, &raw)))
		snprintf(err, err_size, "no se pudo escribir");
	else
		bgio_out_sync(&io);  /* al disco antes del cierre forzado */
	char ferr[160] = "";
	ok = bgio_out_finish(&io, ok, tmp, path, ferr, sizeof ferr);
	if (!ok && !err[0])
		snprintf(err, err_size, "%s", ferr);
	if (ok)
		snprintf(err, err_size, "%u tareas, %u campos, %u notas, %llu KB en %.1f ms", cj.ring.ntask, cj.ring.nfield,
			 cj.ring.nnote, (unsigned long long)(raw >> 10), io.us / 1e3);
	cj.frozen = 0;
	return ok;
}

void cajanegra_pump(uint64_t now_us)
{
	(void)now_us;
	if (!cj.on)
		return;
	if (cj.auto_at && cj.cur_field >= cj.auto_at) {
		cj.auto_at = 0;
		cj.want = true;
		cj.asked_field = cj.cur_field;
	}
	if (cj.pending && cj.finished) {
		lwsync();
		if (cj.res.ok) {
			salsa64_log("caja negra: %s escrita en segundo plano: %.1f ms de punta a punta, %.1f ms en el disco, "
				    "%.1f ms del SPE comprimiendo (%s); %llu KB de %llu KB (%.1f:1) con %u tareas, %u campos y "
				    "%u notas (%u descartadas por falta de lugar)",
				    cj.path, cj.res.us_total / 1e3, cj.res.us_io / 1e3, cj.res.us_spu / 1e3,
				    cj.res.spu ? "SPE" : "PPE", (unsigned long long)(cj.res.bytes >> 10),
				    (unsigned long long)(cj.res.raw >> 10),
				    cj.res.bytes ? (double)cj.res.raw / (double)cj.res.bytes : 0.0, cj.res.tasks, cj.res.fields,
				    cj.res.notes, cj.res.dropped);
		} else {
			osd_notice(sysGetSystemTime(), "Caja negra: falló");
			salsa64_log("caja negra: FALLO al escribir %s: %s", cj.path, cj.res.err);
		}
		cj.pending = 0;
		cj.finished = 0;
		cj.frozen = 0;
	}
	if (!cj.want || cj.pending)
		return;
	if (bgjob_busy())
		return;  /* un estado se está escribiendo: se espera */
	cj.want = false;
	snprintf(cj.dir, sizeof cj.dir, CAJANEGRA_DIR, cj.asked_field);
	snprintf(cj.path, sizeof cj.path, "%s/" CAJANEGRA_FILE, cj.dir);
	snprintf(cj.tmp, sizeof cj.tmp, "%s.tmp", cj.path);
	memset(&cj.meta, 0, sizeof cj.meta);
	cj.meta.field = cj.asked_field;
	if (cj.bus && cj.bus->rom && cj.bus->rom_size >= ROM_TITLE_END) {
		cj.meta.crc1 = salsa64_rd32(cj.bus->rom + ROM_CRC1);
		cj.meta.crc2 = salsa64_rd32(cj.bus->rom + ROM_CRC2);
		memcpy(cj.meta.title, cj.bus->rom + ROM_TITLE, sizeof cj.meta.title);
	}
	cj.res.tasks = cj.ring.ntask;
	cj.res.fields = cj.ring.nfield;
	cj.res.notes = cj.ring.nnote;
	cj.res.dropped = cj.ring.dropped_tasks;
	cj.t_submit = sysGetSystemTime();
	cj.frozen = 1;
	cj.pending = 1;
	cj.finished = 0;
	lwsync();
	osd_notice(sysGetSystemTime(), "Caja negra: escribiendo cajanegra-%u", cj.asked_field);
	salsa64_log("caja negra: escribiendo %s (%u tareas, %u campos, %u notas; el anillo tiene %u KB)", cj.path, cj.res.tasks,
		    cj.res.fields, cj.res.notes, cj.ring.used >> 10);
	if (!bgjob_submit(cj_write, NULL))
		cj_write(NULL);  /* sin hilo: aquí mismo */
}
