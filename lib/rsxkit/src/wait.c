/* SPDX-License-Identifier: MIT */
/*
 * rsxkit — esperas largas al RSX.
 *
 * Cada espera de rsxkit en el PPE (una etiqueta, el flip, lugar en el búfer
 * de comandos) deja aquí qué espera y desde cuándo. Si dura más de lo
 * normal, las que son un bucle de rsxkit (etiquetas y flip) llaman al
 * gancho de la aplicación (rsxkit_set_wait_hook) desde el hilo que espera,
 * para que diga qué pasa, atienda al sistema o abandone. La del búfer de
 * comandos corre dentro del callback del sistema (fifo.c): ahí no hay
 * bucle propio, así que solo queda anotada para rsxkit_wait_now, que otro
 * hilo puede leer (un vigía).
 */
#include <sys/systime.h>

#include "internal.h"

static inline void lwsync(void) { __asm__ volatile("lwsync" ::: "memory"); }

static struct {
	rsxkit_wait_hook fn;
	void *ctx;
	uint32_t after_us, period_us;
	/* La espera en curso. seq es impar mientras se escribe cur
	 * (rsxkit_wait_now, desde otro hilo, reintenta). */
	volatile uint32_t seq;
	rsxkit_wait_info cur;
	uint64_t next_us;
	uint32_t spins;
	int in_hook;
	volatile int lost;
} w;

static gcmControlRegister *ctrl;

void rsxkit_set_wait_hook(rsxkit_wait_hook fn, void *ctx, uint32_t after_us, uint32_t period_us)
{
	w.fn = fn;
	w.ctx = ctx;
	w.after_us = after_us;
	w.period_us = period_us ? period_us : 100000u;
}

bool rsxkit_lost(void)
{
	return w.lost != 0;
}

void rsxkit_wait_set_lost(void)
{
	w.lost = 1;
}

static void regs(uint32_t *get, uint32_t *put)
{
	if (!ctrl)
		ctrl = gcmGetControlRegister();
	*get = ctrl ? ctrl->get : 0;
	*put = ctrl ? ctrl->put : 0;
}

static void write_begin(void)
{
	w.seq++;
	lwsync();
}

static void write_end(void)
{
	lwsync();
	w.seq++;
}

void rsxkit_wait_begin(rsxkit_wait_kind kind, const char *what, uint32_t label, uint32_t expected, uint32_t current)
{
	uint64_t now = sysGetSystemTime();
	uint32_t get, put;
	regs(&get, &put);
	write_begin();
	w.cur.kind = kind;
	w.cur.what = what;
	w.cur.label = label;
	w.cur.expected = expected;
	w.cur.current = current;
	w.cur.get = w.cur.get0 = get;
	w.cur.put = put;
	w.cur.since_us = now;
	w.cur.elapsed_us = 0;
	w.cur.calls = 0;
	w.cur.done = false;
	write_end();
	w.next_us = now + w.after_us;
	w.spins = 0;
}

bool rsxkit_wait_tick(uint32_t current)
{
	if (w.lost)
		return false;
	if (++w.spins & 7)
		return true;
	uint64_t now = sysGetSystemTime();
	uint32_t get, put;
	regs(&get, &put);
	write_begin();
	w.cur.current = current;
	w.cur.get = get;
	w.cur.put = put;
	w.cur.elapsed_us = now - w.cur.since_us;
	write_end();
	if (!w.fn || w.in_hook || now < w.next_us)
		return true;
	w.next_us = now + w.period_us;
	w.cur.calls++;
	rsxkit_wait_info info = w.cur;
	w.in_hook = 1;
	bool go_on = w.fn(w.ctx, &info);
	w.in_hook = 0;
	if (!go_on)
		w.lost = 1;
	return go_on;
}

void rsxkit_wait_end(void)
{
	if (w.cur.calls && w.fn && !w.in_hook) {
		/* Ya se había avisado: que sepa que terminó y cuánto duró. */
		rsxkit_wait_info info = w.cur;
		info.elapsed_us = sysGetSystemTime() - info.since_us;
		info.done = true;
		w.in_hook = 1;
		w.fn(w.ctx, &info);
		w.in_hook = 0;
	}
	write_begin();
	w.cur.kind = RSXKIT_WAIT_NONE;
	w.cur.calls = 0;
	write_end();
}

bool rsxkit_wait_now(rsxkit_wait_info *out)
{
	for (int tries = 0; tries < 100; tries++) {
		uint32_t s0 = w.seq;
		lwsync();
		*out = w.cur;
		lwsync();
		if (!(s0 & 1) && w.seq == s0) {
			if (out->kind == RSXKIT_WAIT_NONE)
				return false;
			uint32_t get, put;
			regs(&get, &put);
			out->get = get;
			out->put = put;
			out->elapsed_us = sysGetSystemTime() - out->since_us;
			return true;
		}
	}
	return false;
}

const char *rsxkit_label_name(uint32_t index)
{
	if (index == RSXKIT_LABEL_FINISH)
		return "rsxkit_finish";
	if (index == RSXKIT_LABEL_FENCE)
		return "barrera";
	if (index == RSXKIT_LABEL_POST)
		return "posprocesado";
	if (index > RSXKIT_LABEL_FENCE && index < RSXKIT_LABEL_FINISH)
		return "framebuffer";
	if (index >= RSXKIT_LABEL_USER_MIN && index <= RSXKIT_LABEL_USER_MAX)
		return "etiqueta de la aplicación";
	return "etiqueta";
}

void rsxkit_debug_stall(void)
{
	gcmContextData *ctx = rsxkit_g.ctx;
	u32 off;
	rsxkit_reserve(16);
	if (rsxAddressToOffset(ctx->current, &off) != 0)
		return;
	*ctx->current++ = 0x20000000u | off;  /* JUMP a sí mismo: el RSX gira ahí */
	rsxFlushBuffer(ctx);
}
