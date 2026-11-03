/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — tareas del RSP: lectura del OSTask, detección del microcódigo
 * gráfico y fin de la tarea (interrupciones SP y DP). Ver rcp.h.
 *
 * Tiempos: la tarea "dura" gfx_cycles/audio_cycles ciclos del CPU; en ese
 * lapso el backend gráfico puede trabajar en paralelo (un SPE). Al llegar
 * el fin, si el backend sabe decir si terminó (`done`) y todavía no, el
 * evento se vuelve a programar poll_cycles más tarde y el CPU sigue
 * emulando (o, si está en un bucle de espera, el backend duerme un poco:
 * así el reloj emulado no se adelanta al real); si no sabe, se le espera
 * (`finish`). La interrupción del DP llega dp_cycles después de la del
 * SP, si la lista terminó con G_RDPFULLSYNC (y el RDP no está congelado:
 * salsa64_rsp_dp_done). mupen64plus-core usa retardos
 * del mismo orden (unos miles de ciclos); aquí son más largos para que el
 * SPE alcance a terminar sin frenar al CPU, y se pueden cambiar.
 *
 * Mientras la tarea sigue en curso el RSP no está HALT y una escritura
 * que quite HALT no arranca otra (como en el hardware: ya corre); una
 * petición de yield se ignora y la tarea termina normal (TASKDONE), que
 * libultra acepta como fin de la tarea.
 */
#include <string.h>

#include "core/bus.h"
#include "core/endian.h"
#include "core/rom.h"
#include "core/sched.h"
#include "cpu/vr4300.h"
#include "rsp/jpeg/jpeg.h"
#include "rsp/jpeg/re2.h"

#define GFX_CYCLES_DEFAULT   30000u  /* ~0,32 ms de la N64 */
#define AUDIO_CYCLES_DEFAULT 8000u
#define JPEG_CYCLES_DEFAULT  8000u   /* con backend: primer sondeo (~85 µs de la N64) */
#define DP_CYCLES_DEFAULT    10000u
#define POLL_CYCLES_DEFAULT  10000u  /* ~0,11 ms de la N64 entre sondeos */

/* Hasta dónde se buscan los textos del microcódigo en su segmento de datos. */
#define UCODE_SCAN_MAX 0x1000u

/* Lo que hace el BREAK final de un microcódigo; los de libultra ponen antes
 * la señal 2 (tarea terminada). */
static void rsp_break(struct salsa64_bus *bus, uint32_t signals)
{
	uint32_t s = __atomic_or_fetch(&bus->sp.regs[SALSA64_SP_STATUS],
				       SALSA64_SP_STATUS_HALT | SALSA64_SP_STATUS_BROKE | signals, __ATOMIC_SEQ_CST);
	if (s & SALSA64_SP_STATUS_INTR_BREAK) {
		bus->rsp.stats.sp_irq++;
		salsa64_mi_raise(bus, SALSA64_IRQ_SP);
	}
}

/* ¿Terminó ya la tarea en curso? Los backends que lo saben decir sin
 * bloquear (`done`) se sondean; los demás se esperan en `finish`. */
static bool task_done(struct salsa64_bus *bus, bool idle)
{
	const struct salsa64_rsp *rsp = &bus->rsp;
	if (rsp->busy == SALSA64_TASK_GFX && rsp->gfx && rsp->gfx->done)
		return rsp->gfx->done(rsp->gfx->ctx, idle);
	if (rsp->busy == SALSA64_TASK_AUDIO && rsp->audio && rsp->audio->done)
		return rsp->audio->done(rsp->audio->ctx, idle);
	if (rsp->busy == SALSA64_TASK_LLE && rsp->lle && rsp->lle->done)
		return rsp->lle->done(rsp->lle->ctx, idle);
	if (rsp->busy == SALSA64_TASK_JPEG && rsp->jpeg_be)
		return rsp->jpeg_be->done(rsp->jpeg_be->ctx, idle);
	return true;
}

static void video_time(struct salsa64_rsp *rsp, uint64_t us)
{
	rsp->stats.video_us += us;
	rsp->stats.video_k_us[rsp->jpeg_vkind] += us;
}

/* Las estadísticas de una tarea JPEG o de video ya hecha (r: su resultado;
 * us: lo que tardó el hilo del CPU en ella). */
static void jpeg_account(struct salsa64_rsp *rsp, const struct salsa64_jpeg_result *r, uint64_t us)
{
	if (rsp->jpeg_video) {
		video_time(rsp, us);
		rsp->stats.video_rows += r->macroblocks;
		rsp->stats.video_k_rows[rsp->jpeg_vkind] += r->macroblocks;
		rsp->stats.video_bad += r->bad;
	} else {
		rsp->stats.jpeg_us += us;
		rsp->stats.jpeg_mb += r->macroblocks;
		rsp->stats.jpeg_bad += r->bad;
	}
}

/* Recoge la tarea JPEG del backend (espera si hace falta) con sus
 * estadísticas; el tiempo es el del hilo del CPU, no el del backend. */
static void jpeg_collect(struct salsa64_rsp *rsp)
{
	struct salsa64_jpeg_result r;
	uint64_t t0 = rsp->clock_us ? rsp->clock_us() : 0;
	rsp->jpeg_be->finish(rsp->jpeg_be->ctx, &r);
	jpeg_account(rsp, &r, rsp->clock_us ? rsp->clock_us() - t0 : 0);
}

void salsa64_evlog_add(struct salsa64_bus *bus, uint32_t kind, uint32_t a, uint32_t b)
{
	struct salsa64_rsp *rsp = &bus->rsp;
	struct salsa64_evlog_entry *e = &rsp->evlog[rsp->evlog_n++ % SALSA64_EVLOG_N];
	e->cycles = bus->cpu ? bus->cpu->cycles : 0;
	e->kind = kind;
	e->a = a;
	e->b = b;
}

static void dp_sync_at(struct salsa64_bus *bus, uint64_t now);

/* Ritmo del RCP (gfx_model, rcp.h): la tarea gráfica que la HLE acaba de
 * terminar (flags: lo que devolvió finish) no termina para el juego antes
 * de lo que tardaría la consola en ella. Si falta, queda recogida y su fin
 * se programa a esa hora (no antes de `when`, el fin ya programado);
 * devuelve si se retrasó. Se aplica una vez por tarea: al recogerla. */
static bool gfx_model_hold(struct salsa64_rsp *rsp, uint64_t when, uint32_t flags)
{
	if (!rsp->gfx_model || !rsp->gfx || !rsp->gfx->cycles || !rsp->sched)
		return false;
	uint64_t c = (uint64_t)rsp->gfx->cycles(rsp->gfx->ctx) * rsp->gfx_model / 100u;
	rsp->stats.model_sum += c;
	uint64_t end = rsp->gfx_start + c;
	if (end <= when)
		return false;
	rsp->stats.model_held++;
	rsp->stats.model_wait += end - when;
	rsp->collected = true;
	rsp->collected_flags = flags;
	salsa64_sched_set(rsp->sched, SALSA64_EV_SP_TASK, end);
	return true;
}

static void task_event(void *ctx, uint64_t when)
{
	struct salsa64_bus *bus = ctx;
	struct salsa64_rsp *rsp = &bus->rsp;
	uint32_t flags = 0;

	/* ¿El CPU saltó ciclos (bucle de espera) desde el sondeo anterior? */
	uint64_t idle_now = bus->cpu ? bus->cpu->idle_cycles : 0;
	bool idle = idle_now != rsp->poll_idle;
	rsp->poll_idle = idle_now;
	if (rsp->collected) {
		/* Ya recogida (salsa64_rsp_quiesce): el backend no la tiene. */
		flags = rsp->collected_flags;
		rsp->collected = false;
		rsp->collected_flags = 0;
		goto finished;
	}
	if (!task_done(bus, idle)) {
		rsp->stats.polls++;
		salsa64_sched_set(rsp->sched, SALSA64_EV_SP_TASK, when + rsp->poll_cycles);
		return;
	}
	if (rsp->busy == SALSA64_TASK_LLE) {
		/* El microcódigo real (en el SPE) ya dejó SP_STATUS como lo dejó:
		 * HALT y BROKE si hizo BREAK; finish recoge su estado. */
		if (rsp->lle)
			rsp->lle->finish(rsp->lle->ctx);
		rsp->busy = 0;
		return;
	}
	if (rsp->busy == SALSA64_TASK_GFX && rsp->gfx) {
		flags = rsp->gfx->finish(rsp->gfx->ctx);
		if (gfx_model_hold(rsp, when, flags))
			return;
	} else if (rsp->busy == SALSA64_TASK_AUDIO && rsp->audio) {
		rsp->audio->finish(rsp->audio->ctx, bus);
	} else if (rsp->busy == SALSA64_TASK_JPEG && rsp->jpeg_be) {
		jpeg_collect(rsp);
	}
finished:
	if (rsp->busy == SALSA64_TASK_GFX) {
		rsp->stats.gfx_last = flags;
		if (!(flags & (SALSA64_GFX_FULLSYNC | SALSA64_GFX_CUT)))
			rsp->stats.gfx_nosync++;
	}
	salsa64_evlog_add(bus, SALSA64_EV_TASK_DONE, rsp->busy, flags);
	rsp->busy = 0;
	rsp_break(bus, SALSA64_SP_STATUS_TASKDONE);
	/* La interrupción del DP llega si la lista pidió G_RDPFULLSYNC y también
	 * si la HLE la cortó antes de llegar a él (búfer lleno, lista
	 * interminable, fin dentro de un tramo de otro microcódigo): el juego
	 * espera al RDP por cada tarea gráfica y, sin ella, se queda esperando
	 * (OoT avisa «RCP is HUNG UP» a los 3 s). Mismo principio que en
	 * salsa64_rsp_start para las tareas con un microcódigo sin HLE. */
	if (flags & SALSA64_GFX_CUT)
		rsp->stats.cut++;
	if (flags & (SALSA64_GFX_FULLSYNC | SALSA64_GFX_CUT))
		dp_sync_at(bus, when);
}

#ifdef SALSA64_DEBUG_DP
void salsa64_debug_dp(int kind, uint32_t reg, uint32_t v);
#endif

static void dp_event(void *ctx, uint64_t when)
{
	struct salsa64_bus *bus = ctx;
	struct salsa64_rsp *rsp = &bus->rsp;
	rsp->stats.dp_fired++;
	(void)when;
#ifdef SALSA64_DEBUG_DP
	salsa64_debug_dp(3, 0, 0);
#endif
	salsa64_evlog_add(bus, SALSA64_EV_DP_FIRE, rsp->dp_nnext, 0);
	salsa64_mi_raise(ctx, SALSA64_IRQ_DP);
	/* La siguiente de las que esperaban, a su hora. */
	if (rsp->dp_nnext && rsp->sched) {
		uint64_t t = rsp->dp_next[0];
		memmove(rsp->dp_next, rsp->dp_next + 1, (unsigned)(rsp->dp_nnext - 1) * sizeof rsp->dp_next[0]);
		rsp->dp_nnext--;
		salsa64_sched_set(rsp->sched, SALSA64_EV_DP_SYNC, t);
	}
}

#define DP_SAME_CYCLES 64u

/* Programa una interrupción del DP para el ciclo `t`: si ya hay una
 * programada, la más temprana va en la ranura y las demás esperan en orden
 * (con la ranura sola, la segunda borraba a la primera y el juego esperaba
 * una interrupción que no llegaba). */
static void dp_schedule(struct salsa64_rsp *rsp, uint64_t t)
{
	uint64_t cur = rsp->sched->when[SALSA64_EV_DP_SYNC];
	if (cur == SALSA64_SCHED_NEVER) {
		salsa64_sched_set(rsp->sched, SALSA64_EV_DP_SYNC, t);
		return;
	}
	/* El mismo SYNC_FULL pedido dos veces (el fin de una tarea y la lista
	 * que traía, o el RSP a bajo nivel y su entrega) es una sola
	 * interrupción: las que caen a menos de DP_SAME_CYCLES son la misma. */
	if (t - cur + DP_SAME_CYCLES < 2 * DP_SAME_CYCLES)
		return;
	for (unsigned k = 0; k < rsp->dp_nnext; k++)
		if (t - rsp->dp_next[k] + DP_SAME_CYCLES < 2 * DP_SAME_CYCLES)
			return;
	if (t < cur) {
		uint64_t tmp = cur;
		cur = t;
		t = tmp;
		salsa64_sched_set(rsp->sched, SALSA64_EV_DP_SYNC, cur);
	}
	/* t va en la cola, ordenada; llena, la última absorbe (se pierde una, como antes). */
	unsigned i = rsp->dp_nnext;
	if (i == sizeof rsp->dp_next / sizeof rsp->dp_next[0]) {
		if (rsp->dp_next[i - 1] < t)
			rsp->dp_next[i - 1] = t;
		return;
	}
	while (i && rsp->dp_next[i - 1] > t) {
		rsp->dp_next[i] = rsp->dp_next[i - 1];
		i--;
	}
	rsp->dp_next[i] = t;
	rsp->dp_nnext++;
}

/* Congelado (FREEZE), el RDP no ejecuta: la interrupción espera a que el CPU
 * lo descongele. Banjo-Kazooie lo congela en el manejador de la interrupción
 * del DP, arranca la tarea del cuadro siguiente así y la descongela
 * después; si la interrupción de esa tarea llegara en el acto, el juego la
 * perdería y se quedaría esperando al RDP (2 cuadros y pantalla fija). */
static void dp_sync_at(struct salsa64_bus *bus, uint64_t now)
{
	struct salsa64_rsp *rsp = &bus->rsp;
	if (bus->dp[SALSA64_DP_STATUS] & SALSA64_DP_STATUS_FREEZE) {
		rsp->dp_held = true;
		rsp->stats.dp_held++;
		return;
	}
	if (rsp->sched) {
		rsp->stats.dp_sched++;
		dp_schedule(rsp, now + rsp->dp_cycles);
	}
}

void salsa64_rsp_dp_done(struct salsa64_bus *bus)
{
	dp_sync_at(bus, bus->cpu ? bus->cpu->cycles : 0);
}

void salsa64_rsp_dp_unfreeze(struct salsa64_bus *bus)
{
	struct salsa64_rsp *rsp = &bus->rsp;
	if (!rsp->dp_held)
		return;
	rsp->dp_held = false;
	salsa64_rsp_dp_done(bus);
}

bool salsa64_rsp_quiesce(struct salsa64_bus *bus)
{
	struct salsa64_rsp *rsp = &bus->rsp;
	if (rsp->busy == SALSA64_TASK_LLE || (rsp->lle && rsp->lle_mode != SALSA64_LLE_OFF))
		return false;
	if (!rsp->busy || rsp->collected)
		return true;
	uint32_t flags = 0;
	if (rsp->busy == SALSA64_TASK_GFX && rsp->gfx)
		flags = rsp->gfx->finish(rsp->gfx->ctx);
	else if (rsp->busy == SALSA64_TASK_AUDIO && rsp->audio)
		rsp->audio->finish(rsp->audio->ctx, bus);
	else if (rsp->busy == SALSA64_TASK_JPEG && rsp->jpeg_be)
		jpeg_collect(rsp);
	rsp->collected = true;
	rsp->collected_flags = flags;
	/* El fin sigue en el evento ya programado; con el ritmo del RCP, no
	 * antes de lo que tardaría la consola. */
	if (rsp->busy == SALSA64_TASK_GFX && rsp->sched &&
	    rsp->sched->when[SALSA64_EV_SP_TASK] != SALSA64_SCHED_NEVER)
		gfx_model_hold(rsp, rsp->sched->when[SALSA64_EV_SP_TASK], flags);
	return true;
}

void salsa64_rsp_sync(struct salsa64_bus *bus)
{
	const struct salsa64_lle_backend *lle = bus->rsp.lle;
	if (lle && lle->sync)
		lle->sync(lle->ctx);
}

void salsa64_rsp_set_lle(struct salsa64_bus *bus, const struct salsa64_lle_backend *lle,
			 enum salsa64_lle_mode mode)
{
	salsa64_rsp_reset(bus);
	bus->rsp.lle = lle;
	bus->rsp.lle_mode = lle ? mode : SALSA64_LLE_OFF;
}

void salsa64_rsp_set_dpc(struct salsa64_bus *bus, const struct salsa64_dpc_backend *dpc)
{
	bus->rsp.dpc = dpc;
}

void salsa64_rsp_set_jpeg(struct salsa64_bus *bus, const struct salsa64_jpeg_backend *jpeg)
{
	salsa64_rsp_reset(bus);
	bus->rsp.jpeg_be = jpeg;
}

void salsa64_rsp_init(struct salsa64_bus *bus, struct salsa64_sched *sched)
{
	struct salsa64_rsp *rsp = &bus->rsp;
	rsp->sched = sched;
	rsp->gfx_cycles = GFX_CYCLES_DEFAULT;
	rsp->audio_cycles = AUDIO_CYCLES_DEFAULT;
	rsp->jpeg_cycles = JPEG_CYCLES_DEFAULT;
	rsp->dp_cycles = DP_CYCLES_DEFAULT;
	rsp->poll_cycles = POLL_CYCLES_DEFAULT;
	salsa64_sched_handler(sched, SALSA64_EV_SP_TASK, task_event, bus);
	salsa64_sched_handler(sched, SALSA64_EV_DP_SYNC, dp_event, bus);
}

void salsa64_rsp_reset(struct salsa64_bus *bus)
{
	struct salsa64_rsp *rsp = &bus->rsp;
	/* Una tarea gráfica en vuelo se espera: el backend no se puede abandonar
	 * a medias. */
	if (rsp->collected)
		;  /* el backend ya la entregó */
	else if (rsp->busy == SALSA64_TASK_GFX && rsp->gfx)
		rsp->gfx->finish(rsp->gfx->ctx);
	else if (rsp->busy == SALSA64_TASK_AUDIO && rsp->audio)
		rsp->audio->finish(rsp->audio->ctx, bus);
	else if (rsp->busy == SALSA64_TASK_LLE && rsp->lle)
		rsp->lle->finish(rsp->lle->ctx);
	else if (rsp->busy == SALSA64_TASK_JPEG && rsp->jpeg_be)
		jpeg_collect(rsp);
	rsp->busy = 0;
	rsp->collected = false;
	rsp->collected_flags = 0;
	rsp->dp_held = false;
	rsp->dp_nnext = 0;
	if (rsp->lle && rsp->lle->reset)
		rsp->lle->reset(rsp->lle->ctx);
	if (rsp->sched) {
		salsa64_sched_cancel(rsp->sched, SALSA64_EV_SP_TASK);
		salsa64_sched_cancel(rsp->sched, SALSA64_EV_DP_SYNC);
	}
}

void salsa64_rsp_set_gfx(struct salsa64_bus *bus, const struct salsa64_gfx_backend *gfx)
{
	salsa64_rsp_reset(bus);
	bus->rsp.gfx = gfx;
}

void salsa64_rsp_read_task(const struct salsa64_bus *bus, struct salsa64_task *task)
{
	uint32_t *w = &task->type;
	for (unsigned i = 0; i < sizeof *task / 4; i++)
		w[i] = salsa64_rd32(bus->sp_mem + SALSA64_TASK_DMEM + 4 * i);
}

static bool in_rdram(const struct salsa64_bus *bus, uint32_t addr, uint32_t size)
{
	addr &= 0x1FFFFFFF;
	return addr < bus->rdram_size && size <= bus->rdram_size - addr;
}

static bool starts_with(const uint8_t *p, const uint8_t *end, const char *s)
{
	size_t n = strlen(s);
	return (size_t)(end - p) >= n && memcmp(p, s, n) == 0;
}

static const uint8_t *find(const uint8_t *p, const uint8_t *end, const char *s)
{
	size_t n = strlen(s);
	for (; (size_t)(end - p) >= n; p++)
		if (memcmp(p, s, n) == 0)
			return p;
	return NULL;
}

/* Microcódigos cuyo texto no basta para identificarlos, por el CRC-32 de su
 * código (ucode, ucode_size bytes; solo el valor, ver docs/compatibilidad.md). */
static const struct {
	uint32_t crc, size;
	enum salsa64_gbi gbi;
} ucode_crcs[] = {
	{ 0xC543D0A8u, 0x1000, SALSA64_GBI_F3DPD },  /* Perfect Dark (NTSC-U) */
	{ 0x81FA4A32u, 0x1000, SALSA64_GBI_F3DWR },  /* Wave Race 64: "RSP SW Version: 2.0D" */
	{ 0xE7FA4491u, 0x1000, SALSA64_GBI_F3DWR },  /* Shadows of the Empire: misma variante con G_QUAD */
	{ 0x3BFF208Du, 0x1000, SALSA64_GBI_F3DDKR }, /* Diddy Kong Racing (USA, rev. 1) */
};

/* Los mismos, con el CRC-32 de los 4 KB leídos en palabras de memoria del
 * host (cada palabra de 4 bytes al revés), como lo calculan otros
 * emuladores (la lista de GLideN64, solo los valores): sirve para reconocer
 * versiones del juego que no se han podido medir aquí. */
static const struct {
	uint32_t crc;
	enum salsa64_gbi gbi;
	bool nonear;  /* no recorta contra el plano cercano (el ".NoN" que el texto de otros no trae aquí) */
} ucode_crcs_w[] = {
	{ 0x2BDCFC8Au, SALSA64_GBI_TURBO3D, false }, /* Dark Rift: medido también en la PS3 */
	{ 0x77195A68u, SALSA64_GBI_TURBO3D_F3D, false }, /* Fast3D del mismo juego */
	{ 0x8D91244Fu, SALSA64_GBI_F3DDKR, false },  /* Diddy Kong Racing (USA, rev. 1: el mismo de arriba) */
	{ 0x6E6FC893u, SALSA64_GBI_F3DDKR, false },  /* Diddy Kong Racing (otra versión: sin respaldo para probarlo) */
	{ 0xBDE9D1FBu, SALSA64_GBI_F3DJFG, false },  /* Jet Force Gemini y Mickey's Speedway USA: sin respaldo */
	{ 0xDA51CCDBu, SALSA64_GBI_F5ROGUE, false }, /* Star Wars: Rogue Squadron (USA: el de su microcódigo, CRC-32 0x415EF7D5 de los bytes tal cual) */
	{ 0x6859BF8Eu, SALSA64_GBI_F5INDY, false },  /* Indiana Jones and the Infernal Machine (USA: CRC-32 0x37AFC470 de los bytes tal cual) */
	{ 0x23FEF05Fu, SALSA64_GBI_F5INDY, false },  /* Star Wars Episode I: Battle for Naboo: sin respaldo para probarlo */
	/* Animal Crossing (Dōbutsu no Mori, «ANIMAL FOREST»): sus dos microcódigos no traen el texto de versión (el
	 * log de la PS3 decía «datos en 0x0011b080: ""»). Por la lista de otros emuladores: un F3DZEX2 de la familia
	 * del de Majora's Mask («F3DZEX.NoN fifo 2.08I») y un S2DEX2; sin respaldo para probarlos. */
	{ 0xD39A0D4Fu, SALSA64_GBI_F3DZEX2, true },
	{ 0x02C399DDu, SALSA64_GBI_S2DEX, false },
};

static enum salsa64_gbi gbi_by_crc(const struct salsa64_bus *bus, const struct salsa64_task *task, bool *nonear)
{
	/* La tarea de Rare no declara el tamaño del código (0): el RSP carga 4 KB. */
	uint32_t size = task->ucode_size ? task->ucode_size : 0x1000;
	uint32_t addr = task->ucode & 0x1FFFFFFF;
	if (size != 0x1000 || !in_rdram(bus, addr, size))
		return SALSA64_GBI_UNKNOWN;
	uint32_t crc = salsa64_crc32(bus->rdram + addr, size);
	for (unsigned i = 0; i < sizeof ucode_crcs / sizeof ucode_crcs[0]; i++)
		if (ucode_crcs[i].size == size && ucode_crcs[i].crc == crc)
			return ucode_crcs[i].gbi;
	crc = salsa64_crc32_words_swapped(bus->rdram + addr, size);
	for (unsigned i = 0; i < sizeof ucode_crcs_w / sizeof ucode_crcs_w[0]; i++)
		if (ucode_crcs_w[i].crc == crc) {
			if ((ucode_crcs_w[i].gbi == SALSA64_GBI_TURBO3D ||
			     ucode_crcs_w[i].gbi == SALSA64_GBI_TURBO3D_F3D) &&
			    (!bus->rsp.gfx || !bus->rsp.gfx->turbo3d))
				continue;
			*nonear = ucode_crcs_w[i].nonear;
			return ucode_crcs_w[i].gbi;
		}
	return SALSA64_GBI_UNKNOWN;
}

/* ¿El nombre del microcódigo ("RSP Gfx ucode F3DEX.NoN fifo 1.22"…) dice
 * que no recorta contra el plano cercano? */
static bool gfx_ucode_nonear(const uint8_t *q, const uint8_t *end)
{
	for (; q + 4 <= end && *q && *q != ' '; q++)
		if (starts_with(q, end, ".NoN"))
			return true;
	return false;
}

/* Variantes de F3DEX2 que solo se distinguen por los 4 KB de código (CRC-32
 * en palabras al revés, como ucode_crcs_w). */
#define ACCLAIM_CRC_W 0xE44DF568u
#define EUROCOM_CRC_W 0xC6121CAAu
#define CONKER_CRC_W 0x1B4ACE88u /* 0x99E222AC con los bytes tal cual */

/* El CRC-32 (palabras al revés) de los 4 KB de código de la tarea, o 0 si no
 * carga 4 KB de la RDRAM o ninguna variante que lo mira está activa: una sola
 * vez por tarea para todas. */
static uint32_t code_crc_w(const struct salsa64_bus *bus, const struct salsa64_task *task)
{
	const struct salsa64_gfx_backend *gfx = bus->rsp.gfx;
	if (!gfx || !(gfx->acclaim || gfx->eurocom || !gfx->conker_text_only))
		return 0;
	uint32_t addr = task->ucode & 0x1FFFFFFFu;
	uint32_t size = task->ucode_size ? task->ucode_size : 0x1000u;
	return size == 0x1000u && in_rdram(bus, addr, size) ? salsa64_crc32_words_swapped(bus->rdram + addr, size) : 0;
}

/* La variante Acclaim comparte el texto de F3DEX2: solo se distingue por
 * los 4 KB de código y solo cuando el interruptor optativo está activo. */
static bool acclaim_ucode(const struct salsa64_bus *bus, uint32_t crc_w)
{
	return bus->rsp.gfx && bus->rsp.gfx->acclaim && crc_w == ACCLAIM_CRC_W;
}

/* Cruis'n Exotica conserva el texto de F3DEX2, pero usa G_SPECIAL_1 para
 * ejecutar bloques contados de comandos F3DEX2. Se distingue por el código, como
 * las demás variantes cuyo texto genérico no basta. */
static bool eurocom_ucode(const struct salsa64_bus *bus, uint32_t crc_w)
{
	return bus->rsp.gfx && bus->rsp.gfx->eurocom && crc_w == EUROCOM_CRC_W;
}

/* Conker (#68): el juego trae un solo código y ocho segmentos de datos que
 * se llaman «F3DEX», «F3DEXBS», «F3DEXBG» y «F3DEXBGBS», con y sin «.NoN».
 * Entre ellos solo cambian el texto, las constantes del recorte cercano y
 * una palabra (DMEM 0x6AC); todos traen G_TRI4, los vértices de 16 bytes y
 * las luces de Rare. El juego usa «F3DEXBG.NoN» y, desde el ascensor del
 * final de Heist, «F3DEX.NoN»: por su texto parecía un F3DEX2 y la HLE no
 * dibujaba sus G_TRI4 (0x10–0x1F, «sin implementar»). El interruptor
 * conker-ucode-viejo (conker_text_only) vuelve a mirar solo el texto. */
static bool conker_ucode(const struct salsa64_bus *bus, uint32_t crc_w)
{
	return bus->rsp.gfx && !bus->rsp.gfx->conker_text_only && crc_w == CONKER_CRC_W;
}

static enum salsa64_gbi detect_gbi(const struct salsa64_bus *bus, const struct salsa64_task *task, bool *nonear);

const char *salsa64_gbi_name(enum salsa64_gbi gbi)
{
	static const char *const names[] = { "desconocido", "F3DEX2", "F3DZEX2", "F3D", "F3DEX", "S2DEX", "F3DPD",
					     "F3DEXBG", "F3DWR", "F3DDKR", "F3DJFG", "S2DEX1", "F5ROGUE", "F5INDY",
					     "Turbo3D", "Turbo3D-F3D", "F3DEX2ACCLAIM", "F3DEX2EUROCOM" };
	_Static_assert(sizeof names / sizeof names[0] == SALSA64_GBI_F3DEX2EUROCOM + 1, "names");
	return (unsigned)gbi < sizeof names / sizeof names[0] ? names[gbi] : "?";
}

/* Anota un microcódigo gráfico la primera vez que aparece (dirección del
 * código y familia): los CRC solo se calculan entonces, no en cada tarea. */
static void note_ucode(struct salsa64_bus *bus, const struct salsa64_task *task, enum salsa64_gbi gbi)
{
	struct salsa64_rsp *rsp = &bus->rsp;
	uint32_t code = task->ucode & 0x1FFFFFFFu;
	for (unsigned i = 0; i < rsp->ucodes_n; i++)
		if (rsp->ucodes[i].code == code && rsp->ucodes[i].gbi == gbi)
			return;
	if (rsp->ucodes_n >= sizeof rsp->ucodes / sizeof rsp->ucodes[0])
		return;
	struct salsa64_ucode_seen *u = &rsp->ucodes[rsp->ucodes_n++];
	memset(u, 0, sizeof *u);
	u->code = code;
	u->gbi = (uint8_t)gbi;
	uint32_t size = task->ucode_size ? task->ucode_size : 0x1000u;
	if (size == 0x1000u && in_rdram(bus, code, size)) {
		u->crc = salsa64_crc32(bus->rdram + code, size);
		u->crc_w = salsa64_crc32_words_swapped(bus->rdram + code, size);
	}
	/* El texto de versión de los datos («RSP Gfx ucode …» o «RSP SW Version: …»),
	 * solo caracteres imprimibles. */
	uint32_t addr = task->ucode_data & 0x1FFFFFFFu;
	uint32_t dsize = task->ucode_data_size;
	if (dsize == 0 || dsize > UCODE_SCAN_MAX)
		dsize = UCODE_SCAN_MAX;
	if (addr >= bus->rdram_size)
		return;
	if (dsize > bus->rdram_size - addr)
		dsize = bus->rdram_size - addr;
	const uint8_t *p = bus->rdram + addr, *end = p + dsize;
	const uint8_t *q = find(p, end, "RSP Gfx ucode ");
	if (!q)
		q = find(p, end, "RSP SW Version: ");
	if (!q)
		return;
	size_t n = 0;
	for (; q < end && n + 1 < sizeof u->text && *q >= 0x20 && *q < 0x7F; q++)
		u->text[n++] = (char)*q;
	while (n && u->text[n - 1] == ' ')
		n--;
	u->text[n] = '\0';
}

enum salsa64_gbi salsa64_rsp_detect_gbi(const struct salsa64_bus *bus, const struct salsa64_task *task)
{
	bool nonear;
	return detect_gbi(bus, task, &nonear);
}

static enum salsa64_gbi detect_gbi(const struct salsa64_bus *bus, const struct salsa64_task *task, bool *nonear)
{
	*nonear = false;
	uint32_t addr = task->ucode_data & 0x1FFFFFFF;
	uint32_t size = task->ucode_data_size;
	if (size == 0 || size > UCODE_SCAN_MAX)
		size = UCODE_SCAN_MAX;
	if (addr >= bus->rdram_size)
		return SALSA64_GBI_UNKNOWN;
	if (size > bus->rdram_size - addr)
		size = bus->rdram_size - addr;
	const uint8_t *p = bus->rdram + addr, *end = p + size;
	/* Antes del texto genérico F3DEX2, que estas variantes también traen. */
	const uint32_t crc_w = code_crc_w(bus, task);
	if (acclaim_ucode(bus, crc_w))
		return SALSA64_GBI_F3DEX2ACCLAIM;
	if (eurocom_ucode(bus, crc_w))
		return SALSA64_GBI_F3DEX2EUROCOM;
	if (conker_ucode(bus, crc_w)) {
		/* Sin recorte cercano si el nombre dice «.No» («F3DEXBGBS.No» no
		 * cabe entero en el texto; sus datos son los de «F3DEXBG.NoN»). */
		const uint8_t *q = find(p, end, "RSP Gfx ucode ");
		if (q)
			for (q += 14; q + 3 <= end && *q > ' ' && *q < 0x7F; q++)
				if (starts_with(q, end, ".No")) {
					*nonear = true;
					break;
				}
		return SALSA64_GBI_F3DEXBG;
	}

	/* ROMs de prueba de Salsa64 (sin microcódigo de Nintendo). */
	if (find(p, end, "Salsa64 GBI Turbo3D") && bus->rsp.gfx && bus->rsp.gfx->turbo3d)
		return SALSA64_GBI_TURBO3D;
	if (find(p, end, "Salsa64 GBI F3DEXBG"))
		return SALSA64_GBI_F3DEXBG;
	if (find(p, end, "Salsa64 GBI F3DPD"))
		return SALSA64_GBI_F3DPD;
	if (find(p, end, "Salsa64 GBI F3DEX2"))
		return SALSA64_GBI_F3DEX2;
	if (find(p, end, "Salsa64 GBI F3DEX1"))
		return SALSA64_GBI_F3DEX;
	if (find(p, end, "Salsa64 GBI Fast3D"))
		return SALSA64_GBI_F3D;

	/* "RSP Gfx ucode <nombre> fifo|xbus <versión> ...": la versión 2.x es
	 * la segunda generación (F3DEX2 y familia). */
	const uint8_t *q = find(p, end, "RSP Gfx ucode ");
	if (q) {
		q += 14;
		*nonear = gfx_ucode_nonear(q, end);
		if (starts_with(q, end, "S2DEX")) {
			/* 1.x (Yoshi's Story: «S2DEX  1.06») o 2.x («S2DEX fifo 2.06»): por la primera versión «N.». */
			const uint8_t *v = q + 5;
			while (v + 1 < end && *v && !(v[0] >= '0' && v[0] <= '9' && v[1] == '.'))
				v++;
			return v + 1 < end && *v == '1' ? SALSA64_GBI_S2DEX1 : SALSA64_GBI_S2DEX;
		}
		/* El de Conker ("F3DEXBG.NoN fifo 2.08"): F3DEX2 con lo suyo. */
		if (starts_with(q, end, "F3DEXBG"))
			return SALSA64_GBI_F3DEXBG;
		bool zex = starts_with(q, end, "F3DZEX");
		const uint8_t *v = q;
		while (v < end && *v && !(v[0] >= '0' && v[0] <= '9' && v + 1 < end && v[1] == '.'))
			v++;
		if (v < end && *v == '2')
			return zex ? SALSA64_GBI_F3DZEX2 : SALSA64_GBI_F3DEX2;
		/* 1.x y la 0.9x anterior (Mario Kart 64): primera generación. */
		if (v < end && (*v == '1' || *v == '0'))
			return SALSA64_GBI_F3DEX;
		return SALSA64_GBI_UNKNOWN;
	}
	/* Los que se conocen por el CRC de su código van antes que el texto
	 * genérico: la variante de Wave Race también dice "2.0". */
	enum salsa64_gbi by_crc = gbi_by_crc(bus, task, nonear);
	if (by_crc != SALSA64_GBI_UNKNOWN)
		return by_crc;
	/* Fast3D (Super Mario 64 y los primeros juegos) no trae ese texto. */
	if (find(p, end, "RSP SW Version: 2.0"))
		return SALSA64_GBI_F3D;
	return SALSA64_GBI_UNKNOWN;
}

void salsa64_rsp_ucode_text(const struct salsa64_bus *bus, uint32_t ucode_data, char *out, size_t n)
{
	uint32_t addr = ucode_data & 0x1FFFFFFF;
	if (n == 0)
		return;
	out[0] = '\0';
	if (addr >= bus->rdram_size)
		return;
	uint32_t size = bus->rdram_size - addr < UCODE_SCAN_MAX ? bus->rdram_size - addr : UCODE_SCAN_MAX;
	const uint8_t *p = bus->rdram + addr, *end = p + size;
	const uint8_t *q = find(p, end, "RSP ");
	if (!q)
		return;
	size_t i = 0;
	while (i + 1 < n && q + i < end && q[i] >= 0x20 && q[i] < 0x7F) {
		out[i] = (char)q[i];
		i++;
	}
	out[i] = '\0';
}

bool salsa64_rsp_ucode_ident(const struct salsa64_bus *bus, uint32_t ucode, uint32_t ucode_size, uint32_t ucode_data,
			     uint32_t data_size, uint32_t *crc_bytes, uint32_t *crc_words, uint32_t *crc_data)
{
	uint32_t size = ucode_size ? ucode_size : 0x1000;
	if (size > 0x1000)
		size = 0x1000;
	*crc_bytes = *crc_words = *crc_data = 0;
	if (!in_rdram(bus, ucode, size))
		return false;
	const uint8_t *p = bus->rdram + (ucode & 0x1FFFFFFF);
	*crc_bytes = salsa64_crc32(p, size);
	*crc_words = salsa64_crc32_words_swapped(p, size);
	if (data_size > 0x1000)
		data_size = 0x1000;
	if (in_rdram(bus, ucode_data, data_size))
		*crc_data = salsa64_crc32(bus->rdram + (ucode_data & 0x1FFFFFFF), data_size);
	return true;
}

void salsa64_rsp_start(struct salsa64_bus *bus)
{
	struct salsa64_rsp *rsp = &bus->rsp;
	struct salsa64_task task;
	if (rsp->busy && rsp->busy != SALSA64_TASK_LLE) {
		/* Ya corre una tarea (su fin espera al backend): como en el
		 * hardware, quitar HALT a un RSP que corre no hace nada. */
		rsp->stats.starts_busy++;
		return;
	}
	if (rsp->busy == SALSA64_TASK_LLE && rsp->lle) {
		/* El CPU detuvo al microcódigo (HALT) y lo reanuda: el backend
		 * recoge lo que quedó en vuelo y sigue. */
		rsp->lle->finish(rsp->lle->ctx);
		rsp->busy = 0;
	}
	salsa64_rsp_read_task(bus, &task);

	uint64_t now = bus->cpu ? bus->cpu->cycles : 0;
	rsp->poll_idle = bus->cpu ? bus->cpu->idle_cycles : 0;
	bool valid = in_rdram(bus, task.data_ptr, 8) && in_rdram(bus, task.ucode_data, 16);

	bool nonear = false;
	enum salsa64_gbi gbi = task.type == SALSA64_TASK_GFX && valid ? detect_gbi(bus, &task, &nonear)
								       : SALSA64_GBI_UNKNOWN;
	enum salsa64_abi abi = task.type == SALSA64_TASK_AUDIO && valid ? salsa64_rsp_detect_abi(bus, &task)
								       : SALSA64_ABI_UNKNOWN;
	if (task.type == SALSA64_TASK_GFX && valid)
		note_ucode(bus, &task, gbi);
	salsa64_evlog_add(bus, SALSA64_EV_TASK_START, task.type, task.data_ptr);
	/* JPEG: el microcódigo se reconoce por su CRC (no trae texto). */
	enum salsa64_jpeg_kind jpeg = SALSA64_JPEG_NONE;
	if (task.type == SALSA64_TASK_JPEG && valid && !rsp->jpeg_lle && in_rdram(bus, task.ucode, task.ucode_size)) {
		jpeg = salsa64_jpeg_detect(bus->rdram + (task.ucode & 0x1FFFFFFF), task.ucode_size);
	} else if (valid && !rsp->jpeg_lle && gbi == SALSA64_GBI_UNKNOWN && abi == SALSA64_ABI_UNKNOWN &&
		   in_rdram(bus, task.ucode, 256)) {
		/* Video de Resident Evil 2: tres microcódigos pequeños sin texto, de tareas
		 * de tipo gráfico, que se reconocen (como en mupen64plus-rsp-hle) por la
		 * suma de los 256 primeros bytes del código; solo si no es un microcódigo
		 * gráfico o de audio conocido. Sus parámetros van en ucode_data. */
		jpeg = salsa64_re2_detect(bus->rdram + (task.ucode & 0x1FFFFFFF), 256);
		/* Solo las tareas con los parámetros y constantes que se midieron contra el
		 * microcódigo real; el resto va a él (RSP a bajo nivel) o termina sin hacer nada. */
		if (jpeg != SALSA64_JPEG_NONE && !salsa64_re2_supported(jpeg, bus->rdram, bus->rdram_size, task.ucode_data)) {
			rsp->stats.video_unsup++;
			jpeg = SALSA64_JPEG_NONE;
		}
	}
	/* Lo que se le pasa al backend o a la HLE: para el video, la estructura
	 * de la tarea es ucode_data. */
	struct salsa64_task jtask = task;
	if (salsa64_jpeg_is_re2(jpeg))
		jtask.data_ptr = task.ucode_data;
	rsp->jpeg_video = salsa64_jpeg_is_re2(jpeg);
	rsp->jpeg_vkind = rsp->jpeg_video ? (unsigned)(jpeg - SALSA64_JPEG_RE2_RESIZE) : 0;
	/* Quien traza (el censo gráfico del host) arma la HLE con el mismo recorte. */
	rsp->gfx_nonear = nonear;
	if (rsp->trace)
		rsp->trace(rsp->trace_ctx, &task, gbi);

	/* S2DEX suelto: solo si el backend gráfico lo hace (rcp.h). */
	const bool s2dex_gbi = gbi == SALSA64_GBI_S2DEX || gbi == SALSA64_GBI_S2DEX1;
	const bool s2dex_ok = s2dex_gbi && rsp->gfx && rsp->gfx->s2dex;

	/* RSP a bajo nivel: lo que no tiene HLE (modo auto) o todo (all). Una
	 * tarea con HLE "disponible" es la que sus backends saben hacer. */
	if (rsp->lle && rsp->lle_mode != SALSA64_LLE_OFF && rsp->sched) {
		bool hle_gfx = task.type == SALSA64_TASK_GFX && valid && rsp->gfx &&
			       (gbi == SALSA64_GBI_F3DEX2 || gbi == SALSA64_GBI_F3DZEX2 ||
				gbi == SALSA64_GBI_F3DEX2ACCLAIM || gbi == SALSA64_GBI_F3DEX2EUROCOM || gbi == SALSA64_GBI_F3D ||
				gbi == SALSA64_GBI_F3DEX || gbi == SALSA64_GBI_F3DWR || gbi == SALSA64_GBI_F3DPD ||
				gbi == SALSA64_GBI_F3DDKR || gbi == SALSA64_GBI_F3DJFG || gbi == SALSA64_GBI_F3DEXBG ||
				gbi == SALSA64_GBI_F5ROGUE || gbi == SALSA64_GBI_F5INDY ||
				gbi == SALSA64_GBI_TURBO3D || gbi == SALSA64_GBI_TURBO3D_F3D || s2dex_ok);
		bool hle_audio = task.type == SALSA64_TASK_AUDIO && valid && rsp->audio && abi != SALSA64_ABI_UNKNOWN;
		if (rsp->lle_mode == SALSA64_LLE_ALL || !(hle_gfx || hle_audio || jpeg != SALSA64_JPEG_NONE)) {
			rsp->stats.lle++;
			rsp->stats.lle_gfx += task.type == SALSA64_TASK_GFX;
			rsp->lle->start(rsp->lle->ctx);
			if (rsp->lle->done) {
				rsp->busy = SALSA64_TASK_LLE;
				salsa64_sched_set(rsp->sched, SALSA64_EV_SP_TASK, now + rsp->poll_cycles);
			}
			return;
		}
	}
	/* Las de video de Resident Evil 2 llegan como tareas gráficas pero van por el camino de las JPEG. */
	if (task.type == SALSA64_TASK_GFX && valid && rsp->sched && !rsp->jpeg_video) {
		if (gbi != SALSA64_GBI_UNKNOWN && (!s2dex_gbi || s2dex_ok)) {
			rsp->stats.gfx++;
			rsp->gfx_start = now;
			rsp->busy = SALSA64_TASK_GFX;
			rsp->gbi = gbi;
			rsp->gfx_nonear = nonear;
			if (rsp->gfx)
				rsp->gfx->start(rsp->gfx->ctx, &task, gbi);
			salsa64_sched_set(rsp->sched, SALSA64_EV_SP_TASK, now + rsp->gfx_cycles);
			return;
		}
		/* Microcódigo sin HLE: la tarea termina abajo y, como casi toda
		 * display list acaba en G_RDPFULLSYNC, también llega la
		 * interrupción del DP; si no, un juego que la espere se cuelga
		 * (GoldenEye antes de reconocer su Fast3D 2.0G). */
		rsp->stats.unknown_gbi++;
		rsp->stats.unknown_data = task.ucode_data;
		rsp->stats.unknown_ucode = task.ucode;
		rsp->stats.unknown_ucode_size = task.ucode_size;
		rsp->stats.unknown_data_size = task.ucode_data_size;
		dp_sync_at(bus, now);
	} else if (task.type == SALSA64_TASK_AUDIO && valid && rsp->sched) {
		/* La HLE de audio (rsp_audio.c y src/rsp/audio; en la PS3, el SPE
		 * 1). Con un ABI desconocido la tarea solo termina. */
		rsp->stats.audio++;
		rsp->busy = SALSA64_TASK_AUDIO;
		rsp->abi = abi;
		if (rsp->abi == SALSA64_ABI_UNKNOWN)
			rsp->stats.unknown_abi++;
		else if (rsp->audio)
			rsp->audio->start(rsp->audio->ctx, &task, rsp->abi);
		salsa64_sched_set(rsp->sched, SALSA64_EV_SP_TASK, now + rsp->audio_cycles);
		return;
	} else if (jpeg != SALSA64_JPEG_NONE && rsp->jpeg_be && rsp->sched) {
		/* JPEG en otro procesador (en la PS3, un SPE): termina por sondeo,
		 * como una gráfica, y el CPU sigue mientras tanto. */
		if (rsp->jpeg_video) {
			rsp->stats.video++;
			rsp->stats.video_k[rsp->jpeg_vkind]++;
		} else {
			rsp->stats.jpeg++;
		}
		rsp->busy = SALSA64_TASK_JPEG;
		uint64_t t0 = rsp->clock_us ? rsp->clock_us() : 0;
		rsp->jpeg_be->start(rsp->jpeg_be->ctx, &jtask, jpeg);
		if (rsp->clock_us) {
			if (rsp->jpeg_video)
				video_time(rsp, rsp->clock_us() - t0);
			else
				rsp->stats.jpeg_us += rsp->clock_us() - t0;
		}
		salsa64_sched_set(rsp->sched, SALSA64_EV_SP_TASK, now + rsp->jpeg_cycles);
		return;
	} else if (jpeg != SALSA64_JPEG_NONE) {
		/* JPEG (fondos prerrenderizados de Zelda OoT) en el hilo del CPU:
		 * pocas y una vez por sala; se hace aquí mismo y la tarea termina
		 * abajo. */
		struct salsa64_jpeg_result r;
		uint64_t t0 = rsp->clock_us ? rsp->clock_us() : 0;
		if (rsp->jpeg_video)
			salsa64_re2_run(jpeg, bus->rdram, bus->rdram_size, jtask.data_ptr, task.dram_stack,
					task.dram_stack_size, &r);
		else
			salsa64_jpeg_run(jpeg, bus->rdram, bus->rdram_size, jtask.data_ptr, &r);
		jpeg_account(rsp, &r, rsp->clock_us ? rsp->clock_us() - t0 : 0);
		if (rsp->jpeg_video) {
			rsp->stats.video++;
			rsp->stats.video_k[rsp->jpeg_vkind]++;
		} else {
			rsp->stats.jpeg++;
		}
	} else {
		rsp->stats.other++;
	}
	/* Sin HLE para esta tarea: se detiene en el acto. Si parece una tarea de
	 * libultra (como hace mupen64plus-rsp-hle: rspboot de hasta 4 KB), con
	 * TASKDONE: el manejador de interrupciones de libultra solo avisa que la
	 * tarea terminó si ve TASKDONE o YIELDED; si no, la toma por un BREAK
	 * suelto y quien la espera no despierta nunca (Zelda OoT al arrancar
	 * manda una tarea de tipo 4 así). */
	bool libultra = task.type != 0 && task.ucode_size != 0 && task.ucode_boot_size <= 0x1000;
	rsp_break(bus, libultra ? SALSA64_SP_STATUS_TASKDONE : 0);
}
