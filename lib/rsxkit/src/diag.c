/* SPDX-License-Identifier: MIT */
/*
 * rsxkit — diagnóstico de un RSX colgado y blindaje. Ver rsxkit.h.
 *
 * Cuando el RSX deja de avanzar, lo único que se ve desde el PPE son GET,
 * PUT y las etiquetas. En la PS3 real (Indiana Jones, 2026-10-29) GET quedó
 * en 0, el principio del búfer, así que ni siquiera dice dónde se paró: el
 * rastro de las últimas operaciones con su offset en el búfer de comandos,
 * el aviso de excepción del sistema y el búfer entero son lo que queda para
 * saber qué tenía delante.
 */
#include <stdio.h>
#include <string.h>
#include <sys/systime.h>

#include "internal.h"

static struct {
	rsxkit_trace_entry ring[RSXKIT_TRACE_LEN];
	uint64_t seq;       /* operaciones anotadas desde rsxkit_init */
	uint32_t head;      /* entradas escritas (la próxima va en head % LEN) */
	uint32_t io_base;   /* offset de E/S de rsxkit_g.host */
	int base_ok;
} tr;

/* Lo escribe el hilo del sistema que avisa las excepciones. */
static volatile struct {
	uint32_t count, first, last, get, put;
	uint64_t first_us, last_us;
} err;

static gcmControlRegister *volatile ctl;
static uint32_t safe_flags = RSXKIT_SAFE_ALL;

static inline void lwsync(void) { __asm__ volatile("lwsync" ::: "memory"); }
static inline void hwsync(void) { __asm__ volatile("sync" ::: "memory"); }

/* Del sistema (gcmSetGraphicsHandler), en su hilo: solo anota. */
static void graphics_handler(const u32 val)
{
	uint64_t now = sysGetSystemTime();
	gcmControlRegister *c = ctl;
	if (!err.count) {
		err.first = val;
		err.first_us = now;
	}
	err.last = val;
	err.last_us = now;
	if (c) {
		err.get = c->get;
		err.put = c->put;
	}
	lwsync();
	err.count++;
}

void rsxkit_diag_init(void)
{
	memset(&tr, 0, sizeof tr);
	ctl = gcmGetControlRegister();
	u32 off = 0;
	tr.base_ok = rsxkit_g.host && rsxAddressToOffset(rsxkit_g.host, &off) == 0;
	tr.io_base = off;
	gcmSetGraphicsHandler(graphics_handler);
}

void rsxkit_diag_shutdown(void)
{
	gcmSetGraphicsHandler(NULL);
}

void rsxkit_rsx_errors(rsxkit_rsx_error *out)
{
	uint32_t n = err.count;
	lwsync();
	out->count = n;
	out->first = err.first;
	out->last = err.last;
	out->first_us = err.first_us;
	out->last_us = err.last_us;
	out->get = err.get;
	out->put = err.put;
}

static uint32_t io_of(const void *p)
{
	if (!tr.base_ok || !p)
		return 0;
	return tr.io_base + (uint32_t)((const uint8_t *)p - (const uint8_t *)rsxkit_g.host);
}

void rsxkit_fifo_info_get(rsxkit_fifo_info *out)
{
	memset(out, 0, sizeof *out);
	gcmContextData *ctx = rsxkit_g.ctx;
	gcmControlRegister *c = ctl;
	out->base = tr.io_base;
	out->size = rsxkit_g.host_size;
	if (ctx) {
		out->begin = io_of(ctx->begin);
		out->end = io_of(ctx->end);
		out->current = io_of(ctx->current);
	}
	if (c) {
		out->get = c->get;
		out->put = c->put;
		out->ref = c->ref;
	}
	out->fence_sent = rsxkit_g.fence_seq;
	out->fence_done = *gcmGetLabelAddress(RSXKIT_LABEL_FENCE);
	out->finish_sent = rsxkit_g.finish_seq;
	out->finish_done = *gcmGetLabelAddress(RSXKIT_LABEL_FINISH);
}

void rsxkit_trace_note(const char *op, const char *fmt, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	rsxkit_trace_entry *e = &tr.ring[tr.head % RSXKIT_TRACE_LEN];
	e->op = op;
	e->fmt = fmt;
	e->fifo = rsxkit_g.ctx ? io_of(rsxkit_g.ctx->current) : 0;
	e->n = 1;
	e->a = a;
	e->b = b;
	e->c = c;
	e->d = d;
	e->seq = ++tr.seq;
	tr.head++;
}

void rsxkit_trace_repeat(const char *op, const char *fmt, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	if (tr.head) {
		rsxkit_trace_entry *l = &tr.ring[(tr.head - 1) % RSXKIT_TRACE_LEN];
		if (l->op == op) {
			/* La misma operación seguida: se suma `a` y quedan los últimos
			 * b, c y d; el offset es el de la primera. */
			l->n++;
			l->a += a;
			l->b = b;
			l->c = c;
			l->d = d;
			l->seq = ++tr.seq;
			return;
		}
	}
	rsxkit_trace_note(op, fmt, a, b, c, d);
}

unsigned rsxkit_trace_get(rsxkit_trace_entry *out, unsigned max)
{
	uint32_t have = tr.head < RSXKIT_TRACE_LEN ? tr.head : RSXKIT_TRACE_LEN;
	if (have > max)
		have = max;
	for (uint32_t i = 0; i < have; i++)
		out[i] = tr.ring[(tr.head - have + i) % RSXKIT_TRACE_LEN];
	return have;
}

int rsxkit_trace_format(const rsxkit_trace_entry *e, char *buf, size_t n)
{
	int len = snprintf(buf, n, "%s", e->op ? e->op : "?");
	if (e->fmt && len >= 0 && (size_t)len + 1 < n)
		len += snprintf(buf + len, n - (size_t)len, " ");
	if (e->fmt && len >= 0 && (size_t)len < n)
		len += snprintf(buf + len, n - (size_t)len, e->fmt, e->a, e->b, e->c, e->d);
	if (e->n > 1 && len >= 0 && (size_t)len < n)
		len += snprintf(buf + len, n - (size_t)len, " (%u seguidas)", e->n);
	return len;
}

bool rsxkit_abandon(uint32_t *get, uint32_t *put_before)
{
	rsxkit_wait_set_lost();
	gcmControlRegister *c = ctl;
	uint32_t g = c ? c->get : 0, p = c ? c->put : 0;
	if (get)
		*get = g;
	if (put_before)
		*put_before = p;
	/* Solo si GET está dentro de la memoria del búfer y no pasa de PUT:
	 * con PUT = GET el RSX no tiene nada pendiente. Después no se vuelve a
	 * mover PUT (rsxkit_lost: ni barreras, ni flips, ni vaciados). */
	if (!c || !tr.base_ok || (g & 3) || g < tr.io_base || g - tr.io_base >= rsxkit_g.host_size || g > p)
		return false;
	hwsync();
	c->put = g;
	hwsync();
	return true;
}

void rsxkit_set_safe(uint32_t flags)
{
	safe_flags = flags & RSXKIT_SAFE_ALL;
}

uint32_t rsxkit_safe(void)
{
	return safe_flags;
}
