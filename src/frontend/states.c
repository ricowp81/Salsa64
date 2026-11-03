/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — estados guardados en la PS3. Ver states.h.
 */
#include "states.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/systime.h>

#include "i18n.h"
#include "platform/ps3/bgio.h"
#include "platform/ps3/bgjob.h"
#include "platform/ps3/log.h"

static uint8_t scratch[SALSA64_STATE_SCRATCH] __attribute__((aligned(128)));
static uint16_t thumb_px[SALSA64_STATE_THUMB_W * SALSA64_STATE_THUMB_H];
static bool thumb_ok;
static uint64_t thumb_us;
static bool keep_snapshot;

void states_keep_snapshot(bool keep)
{
	keep_snapshot = keep;
}

static inline void lwsync(void) { __atomic_thread_fence(__ATOMIC_ACQ_REL); }

static bool fs_read(void *ctx, void *data, uint32_t len)
{
	struct bgio_out *io = ctx;  /* se usa solo su contador de tiempo y bytes */
	uint64_t t0 = sysGetSystemTime(), done = 0;
	bool ok = sysLv2FsRead(io->fd, data, len, &done) == 0 && done == len;
	io->us += sysGetSystemTime() - t0;
	io->bytes += len;
	return ok;
}

static void slot_path(const struct salsa64_bus *bus, unsigned slot, char *buf, size_t n)
{
	char name[64];
	salsa64_state_filename(bus->rom, slot, name, sizeof name);
	snprintf(buf, n, "%s/%s", STATES_DIR, name);
}

/* ---- Escritura en segundo plano ------------------------------------------------- */

static struct {
	bool ready;                      /* foto reservada (states_init) */
	uint8_t *small, *ram;
	uint32_t ram_cap;
	struct salsa64_state_snap snap;
	char path[128], tmp[136];
	unsigned slot;
	uint64_t t_submit;
	volatile int pending;            /* hay una escritura pedida y no recogida por states_poll */
	volatile int finished;           /* el trabajo terminó: res vale */
	struct states_done res;
} bg;

/* El trabajo del hilo de escritura: comprime la RDRAM de la foto y escribe
 * el archivo (a un .tmp que se renombra al final: un corte a mitad no pisa
 * el estado anterior). */
static void bg_write(void *arg)
{
	(void)arg;
	struct states_done *r = &bg.res;
	memset(r, 0, sizeof *r);
	r->slot = bg.slot;
	sysLv2FsMkdir(STATES_DIR, 0777);  /* si ya existe, da error y no pasa nada */

	struct bgio_out io;
	bgio_lz_reset();
	bool ok = bgio_out_open(&io, bg.tmp);
	if (!ok)
		snprintf(r->err, sizeof r->err, tr("no se pudo crear %s"), bg.tmp);
	else
		ok = salsa64_state_write(&bg.snap, bgio_compress, NULL, bgio_out_write, bgio_out_patch, &io, r->err,
					 sizeof r->err);
	char ferr[160] = "";
	ok = bgio_out_finish(&io, ok, bg.tmp, bg.path, ferr, sizeof ferr);
	if (!ok && !r->err[0])
		snprintf(r->err, sizeof r->err, "%s", ferr);
	struct bgio_lz_stats st;
	bgio_lz_get(&st);
	r->ok = ok;
	r->us_io = io.us;
	r->us_spu = st.spu_us;
	r->bytes = io.bytes;
	r->raw_bytes = bg.snap.small_len + bg.snap.ram_len;
	r->blocks = st.blocks;
	r->stored = st.stored;
	r->spu_blocks = st.spu_blocks;
	r->spu = st.spu;
	r->us_total = sysGetSystemTime() - bg.t_submit;
	lwsync();
	bg.finished = 1;
}

uint32_t states_reserved(void)
{
	return bg.ready ? bg.ram_cap + SALSA64_STATE_SECTIONS : 0;
}

bool states_init(uint32_t rdram_size)
{
	if (bg.ready) {
		if (!bgjob_ready())
			bgjob_init();
		return true;
	}
	bg.ram_cap = (rdram_size + 127u) & ~127u;
	bg.small = memalign(128, SALSA64_STATE_SECTIONS);
	bg.ram = memalign(128, bg.ram_cap);
	if (!bg.small || !bg.ram) {
		free(bg.small);
		free(bg.ram);
		bg.small = bg.ram = NULL;
		salsa64_log("estados: sin memoria para la foto (%u KB): se guarda todo seguido, sin comprimir",
			    (bg.ram_cap + SALSA64_STATE_SECTIONS) >> 10);
		return false;
	}
	/* Tocada ya: que la primera foto no pague la primera escritura de cada página. */
	memset(bg.small, 0, SALSA64_STATE_SECTIONS);
	memset(bg.ram, 0, bg.ram_cap);
	if (!bgjob_init())
		salsa64_log("estados: sin hilo de escritura: se guarda todo seguido (comprimido)");
	bg.ready = true;
	salsa64_log("estados: foto de %u KB reservada; escritura en segundo plano %s", (bg.ram_cap + SALSA64_STATE_SECTIONS) >> 10,
		    bgjob_ready() ? "con un hilo propio" : "no disponible");
	return true;
}

void states_shutdown(uint64_t wait_us)
{
	if (bgjob_ready()) {
		if (!bgjob_wait(wait_us)) {
			salsa64_log("estados: la escritura en segundo plano no terminó en %.1f s: se sigue sin esperarla",
				    wait_us / 1e6);
			return;  /* el escritor todavía usa la foto */
		}
		bgjob_shutdown();
	}
	if (bg.pending && bg.finished)
		salsa64_log("estado: la escritura de la ranura %u terminó %s al cerrar%s%s", bg.slot,
			    bg.res.ok ? "bien" : "con error", bg.res.ok ? "" : ": ", bg.res.ok ? "" : bg.res.err);
	bgio_shutdown();
	bg.pending = bg.finished = 0;
	if (keep_snapshot)
		return;  /* comparación con la reserva permanente entre juegos */
	/* El primer juego reservaba 8 MB de RDRAM y 1 MB de secciones que
	 * permanecían vivos en el menú. La siguiente ROM de 64 MB ya no cabía. */
	uint32_t released = states_reserved();
	free(bg.small);
	free(bg.ram);
	memset(&bg, 0, sizeof bg);
	states_thumb_clear();
	thumb_us = 0;
	if (released)
		salsa64_log("estados: foto liberada al cerrar (%u KB)", released >> 10);
}

bool states_busy(void)
{
	return bg.pending && !bg.finished;
}

bool states_poll(struct states_done *d)
{
	if (!bg.pending || !bg.finished)
		return false;
	lwsync();
	*d = bg.res;
	bg.pending = 0;
	bg.finished = 0;
	return true;
}

/* Espera a que no haya escritura en marcha. false si pasó el plazo. */
static bool bg_idle(uint64_t timeout_us)
{
	if (!bg.pending)
		return true;
	uint64_t t0 = sysGetSystemTime();
	while (!bg.finished) {
		if (timeout_us && sysGetSystemTime() - t0 >= timeout_us)
			return false;
		sysUsleep(1000);
	}
	lwsync();
	return true;
}

bool states_slot_exists(const struct salsa64_bus *bus, unsigned slot)
{
	char path[128];
	sysFSStat st;
	if (!bus->rom)
		return false;
	if (bg.pending && !bg.finished && bg.slot == slot)
		return true;  /* se está escribiendo ahora mismo */
	slot_path(bus, slot, path, sizeof path);
	return sysLv2FsStat(path, &st) == 0 && st.st_size >= SALSA64_STATE_HEADER;
}

/* Sin foto: todo aquí y ahora, la RDRAM sin comprimir, como antes. */
static bool save_sync(struct salsa64_n64 *n64, const char *path, const char *tmp, struct states_timing *t,
		      char *err, size_t err_size, uint64_t t0)
{
	sysLv2FsMkdir(STATES_DIR, 0777);
	struct bgio_out io;
	if (!bgio_out_open(&io, tmp)) {
		snprintf(err, err_size, tr("no se pudo crear %s"), tmp);
		return false;
	}
	const struct salsa64_state_thumb th = { SALSA64_STATE_THUMB_W, SALSA64_STATE_THUMB_H, thumb_px };
	err[0] = '\0';
	bool ok = salsa64_state_save(n64, thumb_ok ? &th : NULL, bgio_out_write, &io, scratch, err, err_size);
	char ferr[160] = "";
	ok = bgio_out_finish(&io, ok, tmp, path, ferr, sizeof ferr);
	if (!ok) {
		if (!err[0])
			snprintf(err, err_size, "%s", ferr);
		return false;
	}
	t->us_io = io.us;
	t->bytes = io.bytes;
	t->us_total = sysGetSystemTime() - t0;
	return true;
}

bool states_save(struct salsa64_n64 *n64, unsigned slot, struct states_timing *t, char *err, size_t err_size)
{
	uint64_t t0 = sysGetSystemTime();
	char path[128], tmp[136];
	memset(t, 0, sizeof *t);
	slot_path(&n64->bus, slot, path, sizeof path);
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	if (!bg.ready)
		return save_sync(n64, path, tmp, t, err, err_size, t0);

	/* La escritura anterior tiene que haber terminado: la foto es una sola. */
	if (!bg_idle(5000000)) {
		snprintf(err, err_size, "%s", tr("el estado anterior sigue escribiéndose"));
		return false;
	}
	if (bg.pending && bg.finished) {
		/* Terminó y nadie la recogió: se cuenta aquí. */
		salsa64_log("estado: la escritura anterior (ranura %u) terminó %s", bg.slot, bg.res.ok ? "bien" : "con error");
		bg.pending = 0;
		bg.finished = 0;
	}
	const struct salsa64_state_thumb th = { SALSA64_STATE_THUMB_W, SALSA64_STATE_THUMB_H, thumb_px };
	bg.snap = (struct salsa64_state_snap){ bg.small, bg.ram, 0, 0 };
	if (!salsa64_state_capture(n64, thumb_ok ? &th : NULL, &bg.snap, SALSA64_STATE_SECTIONS, bg.ram_cap, err,
				   err_size))
		return false;
	{
		/* Una vez por juego: cuánto de la foto es la copia de la RDRAM (para
		 * distinguirlo de las secciones en el log). */
		static bool measured;
		if (!measured) {
			measured = true;
			uint64_t tm = sysGetSystemTime();
			memcpy(bg.ram, n64->bus.rdram, n64->bus.rdram_size);
			salsa64_log("estados: copiar la RDRAM (%u KB) a la foto: %.1f ms; la foto entera %.1f ms",
				    n64->bus.rdram_size >> 10, (sysGetSystemTime() - tm) / 1e3, (tm - t0) / 1e3);
		}
	}
	memcpy(bg.path, path, sizeof bg.path);
	memcpy(bg.tmp, tmp, sizeof bg.tmp);
	bg.slot = slot;
	bg.t_submit = t0;
	bg.finished = 0;
	bg.pending = 1;
	lwsync();
	t->async = true;
	if (!bgjob_submit(bg_write, NULL)) {
		/* Sin hilo: aquí mismo (más largo, pero guarda). */
		bg_write(NULL);
		t->async = false;
	}
	t->us_total = sysGetSystemTime() - t0;
	return true;
}

bool states_load(struct salsa64_n64 *n64, unsigned slot, struct states_timing *t, char *err, size_t err_size,
		 bool *broken)
{
	char path[128];
	memset(t, 0, sizeof *t);
	*broken = false;
	/* Cargar lo que se está escribiendo: esperar a que esté en el disco. */
	if (!bg_idle(10000000)) {
		snprintf(err, err_size, "%s", tr("el estado sigue escribiéndose"));
		return false;
	}
	uint64_t t0 = sysGetSystemTime();
	slot_path(&n64->bus, slot, path, sizeof path);
	struct bgio_out io;
	memset(&io, 0, sizeof io);
	s32 fd = -1;
	if (sysLv2FsOpen(path, SYS_O_RDONLY, &fd, 0, NULL, 0) != 0) {
		snprintf(err, err_size, "%s", tr("no hay estado"));
		return false;
	}
	io.fd = fd;
	bool ok = salsa64_state_load(n64, fs_read, &io, scratch, err, err_size, broken);
	sysLv2FsClose(fd);
	t->us_io = io.us;
	t->bytes = io.bytes;
	t->us_total = sysGetSystemTime() - t0;
	return ok;
}

void states_thumb_capture(const rsxkit_rect *r)
{
	uint32_t pitch;
	uint64_t t0 = sysGetSystemTime();
	const uint32_t *px = rsxkit_back_buffer(&pitch);
	thumb_ok = false;
	thumb_us = 0;
	if (!px || !r->w || !r->h)
		return;
	/* Una lectura por píxel de la miniatura: leer la memoria del RSX desde
	 * el PPE es lento, así que nada de promediar (19 200 lecturas). */
	for (uint32_t y = 0; y < SALSA64_STATE_THUMB_H; y++) {
		uint32_t sy = (uint32_t)r->y + (uint32_t)((2 * y + 1) * (uint32_t)r->h / (2 * SALSA64_STATE_THUMB_H));
		const uint32_t *line = px + sy * (pitch / 4);
		for (uint32_t x = 0; x < SALSA64_STATE_THUMB_W; x++) {
			uint32_t sx = (uint32_t)r->x + (uint32_t)((2 * x + 1) * (uint32_t)r->w / (2 * SALSA64_STATE_THUMB_W));
			uint32_t c = line[sx];
			thumb_px[y * SALSA64_STATE_THUMB_W + x] =
				(uint16_t)(((c >> 19) & 31) << 11 | ((c >> 11) & 31) << 6 | ((c >> 3) & 31) << 1 | 1);
		}
	}
	thumb_ok = true;
	thumb_us = sysGetSystemTime() - t0;
}

void states_thumb_clear(void)
{
	thumb_ok = false;
}

uint64_t states_thumb_us(void)
{
	return thumb_us;
}
