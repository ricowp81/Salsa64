/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — ROM del cartucho por fragmentos (ver rompg.h).
 *
 * Adaptado de src/core/cart.c de SalsaDS (mismo autor, GPL-2.0-or-later).
 *
 * Reparto de trabajo en modo asíncrono:
 *   - el hilo de emulación es el único que toca map, slot_page, slot_stamp, slot_pf y la elección de víctima;
 *   - el hilo lector solo lee el archivo, llena la ranura que le tocó y pone slot_st = lista (con liberación);
 *   - la cola (q) y las esperas se protegen con el cerrojo del lector. Quien espera una ranura revisa slot_st bajo ese
 *     cerrojo y el lector avisa tras escribirlo, así que no se pierden avisos.
 * Una ranura leyéndose no se elige de víctima, y tampoco la de la última página servida (su puntero puede estar en uso).
 */
#include "core/rompg.h"

#include <stdlib.h>
#include <string.h>

#include "core/endian.h"

#define NO_PAGE 0xFFFFFFFFu

/* Pone en orden z64 los bytes de una página recién leída (las páginas miden un múltiplo de 4 y empiezan alineadas). */
static void page_normalize(uint8_t *p, size_t n, unsigned fmt)
{
	if (fmt == SALSA64_ROM_V64) {
		for (size_t i = 0; i + 1 < n; i += 2) {
			uint8_t t = p[i];
			p[i] = p[i + 1];
			p[i + 1] = t;
		}
	} else if (fmt == SALSA64_ROM_N64) {
		for (size_t i = 0; i + 3 < n; i += 4) {
			uint8_t a = p[i], b = p[i + 1];
			p[i] = p[i + 3];
			p[i + 1] = p[i + 2];
			p[i + 2] = b;
			p[i + 3] = a;
		}
	}
}

/* ---- Hilo lector: PSL1GHT en la consola, pthread en el host y las pruebas ---- */
#ifdef __lv2ppu__  /* PSL1GHT; el gcc de Linux ppc64 también define __PPU__ con -mcpu=cell */
#include <ppu-lv2.h>
#include <sys/cond.h>
#include <sys/mutex.h>
#include <sys/thread.h>

#define RPG_PRIO  1100   /* por debajo del hilo de emulación (1000): el disco no le quita CPU */
#define RPG_STACK 0x10000

struct salsa64_rompg_async {
	sys_mutex_t mtx;
	sys_cond_t cv;
	sys_ppu_thread_t tid;
	volatile int stop;
	struct salsa64_rompg *g;
};

static int os_init(struct salsa64_rompg_async *a, void (*fn)(void *))
{
	sys_mutex_attr_t ma;
	sys_cond_attr_t ca;

	sysMutexAttrInitialize(ma);
	sysCondAttrInitialize(ca);
	if (sysMutexCreate(&a->mtx, &ma) != 0)
		return -1;
	if (sysCondCreate(&a->cv, a->mtx, &ca) != 0) {
		sysMutexDestroy(a->mtx);
		return -1;
	}
	if (sysThreadCreate(&a->tid, fn, a, RPG_PRIO, RPG_STACK, THREAD_JOINABLE, (char *)"salsa64_rom") != 0) {
		sysCondDestroy(a->cv);
		sysMutexDestroy(a->mtx);
		return -1;
	}
	return 0;
}
static void os_lock(struct salsa64_rompg_async *a) { sysMutexLock(a->mtx, 0); }
static void os_unlock(struct salsa64_rompg_async *a) { sysMutexUnlock(a->mtx); }
static void os_wait(struct salsa64_rompg_async *a) { sysCondWait(a->cv, 0); }   /* con el cerrojo tomado */
static void os_wake(struct salsa64_rompg_async *a) { sysCondBroadcast(a->cv); }
static void os_end(struct salsa64_rompg_async *a)
{
	u64 rv;
	sysThreadJoin(a->tid, &rv);
	sysCondDestroy(a->cv);
	sysMutexDestroy(a->mtx);
}
static void os_exit(void) { sysThreadExit(0); }
#else
#include <pthread.h>

static void reader_main(void *arg);

struct salsa64_rompg_async {
	pthread_mutex_t mtx;
	pthread_cond_t cv;
	pthread_t tid;
	volatile int stop;
	struct salsa64_rompg *g;
};

static void *os_tramp(void *p)
{
	reader_main(p);
	return NULL;
}

static int os_init(struct salsa64_rompg_async *a, void (*fn)(void *))
{
	(void)fn;
	if (pthread_mutex_init(&a->mtx, NULL) != 0)
		return -1;
	if (pthread_cond_init(&a->cv, NULL) != 0) {
		pthread_mutex_destroy(&a->mtx);
		return -1;
	}
	if (pthread_create(&a->tid, NULL, os_tramp, a) != 0) {
		pthread_cond_destroy(&a->cv);
		pthread_mutex_destroy(&a->mtx);
		return -1;
	}
	return 0;
}
static void os_lock(struct salsa64_rompg_async *a) { pthread_mutex_lock(&a->mtx); }
static void os_unlock(struct salsa64_rompg_async *a) { pthread_mutex_unlock(&a->mtx); }
static void os_wait(struct salsa64_rompg_async *a) { pthread_cond_wait(&a->cv, &a->mtx); }
static void os_wake(struct salsa64_rompg_async *a) { pthread_cond_broadcast(&a->cv); }
static void os_end(struct salsa64_rompg_async *a)
{
	pthread_join(a->tid, NULL);
	pthread_cond_destroy(&a->cv);
	pthread_mutex_destroy(&a->mtx);
}
static void os_exit(void) {}
#endif

enum { ST_FREE = 0, ST_LOADING = 1, ST_READY = 2 };

#define ST_LOAD(g, s)     __atomic_load_n(&(g)->slot_st[s], __ATOMIC_ACQUIRE)
#define ST_STORE(g, s, v) __atomic_store_n(&(g)->slot_st[s], (v), __ATOMIC_RELEASE)

static uint64_t now_us(const struct salsa64_rompg *g) { return g->clock_us ? g->clock_us() : 0; }

/* Lee la página de la ranura `slot` (ya reservada para `slot_page[slot]`) y la deja lista. Lo corre el hilo lector, o
 * el de emulación en modo síncrono. Solo escribe en la ranura y en las estadísticas de lectura. */
static void slot_fill(struct salsa64_rompg *g, uint32_t slot)
{
	const size_t psz = (size_t)1 << g->shift;
	const uint64_t off = (uint64_t)g->slot_page[slot] << g->shift;
	const size_t want = off + psz > g->size ? (size_t)(g->size - off) : psz;
	uint8_t *dst = g->mem + ((size_t)slot << g->shift);
	uint64_t t0 = now_us(g);
	int got = g->rd(g->user, off, dst, want);

	if (got < 0) {
		__atomic_fetch_add(&g->errs, 1, __ATOMIC_RELAXED);
		got = 0;
	}
	if ((size_t)got < psz)
		memset(dst + got, 0, psz - (size_t)got);
	page_normalize(dst, (size_t)got, g->fmt);
	g->read_bytes += (uint64_t)got;
	if (g->clock_us) {
		uint64_t dt = g->clock_us() - t0;
		g->read_us += dt;
		if (dt > g->read_max_us)
			g->read_max_us = dt;
	}
	ST_STORE(g, slot, ST_READY);   /* la página queda visible entera o no queda */
}

static void reader_main(void *arg)
{
	struct salsa64_rompg_async *a = arg;
	struct salsa64_rompg *g = a->g;

	os_lock(a);
	while (!a->stop) {
		if (!g->q_n) {
			os_wait(a);
			continue;
		}
		uint32_t slot = g->q[g->q_head];
		g->q_head = (g->q_head + 1) % g->nslots;
		g->q_n--;
		os_unlock(a);
		slot_fill(g, slot);
		os_lock(a);
		os_wake(a);   /* con el cerrojo tomado: quien espera revisó la ranura bajo el mismo cerrojo */
	}
	os_unlock(a);
	os_exit();
}

/* Elige la ranura a pisar: una libre o la más antigua que no esté leyéndose ni sea la última servida (cuyo puntero
 * puede estar en uso). -1 si no hay. Solo la llama el hilo de emulación. */
static int victim(struct salsa64_rompg *g)
{
	int best = -1;
	uint64_t oldest = ~0ull;
	int keep = g->last_page != NO_PAGE ? g->map[g->last_page] : -1;

	for (uint32_t i = 0; i < g->nslots; i++) {
		if (g->slot_page[i] == NO_PAGE)
			return (int)i;
		if ((int)i == keep || ST_LOAD(g, i) != ST_READY)
			continue;
		if (g->slot_stamp[i] < oldest) {
			oldest = g->slot_stamp[i];
			best = (int)i;
		}
	}
	return best;
}

/* Reserva ranura para `page` y la pone a leer (al frente de la cola si `front`). Devuelve la ranura, o -1 si todas
 * están ocupadas. Síncrono: la lee ahí mismo. */
static int claim(struct salsa64_rompg *g, uint32_t page, int front, int pf)
{
	int v = victim(g);
	struct salsa64_rompg_async *a = g->as;

	if (v < 0)
		return -1;
	if (g->slot_page[v] != NO_PAGE)
		g->map[g->slot_page[v]] = -1;
	g->slot_page[v] = page;
	g->map[page] = v;
	g->slot_stamp[v] = ++g->stamp;
	g->slot_pf[v] = (uint8_t)pf;
	ST_STORE(g, (uint32_t)v, ST_LOADING);
	if (pf)
		g->ra++;
	if (!a) {
		slot_fill(g, (uint32_t)v);
		return v;
	}
	os_lock(a);
	if (front) {
		g->q_head = (g->q_head + g->nslots - 1) % g->nslots;
		g->q[g->q_head] = (uint32_t)v;
	} else {
		g->q[(g->q_head + g->q_n) % g->nslots] = (uint32_t)v;
	}
	g->q_n++;
	os_wake(a);
	os_unlock(a);
	return v;
}

/* Espera a que la ranura esté lista; devuelve los µs esperados (0 si ya lo estaba). */
static uint64_t slot_wait(struct salsa64_rompg *g, uint32_t slot)
{
	struct salsa64_rompg_async *a = g->as;
	uint64_t t0;

	if (ST_LOAD(g, slot) == ST_READY)
		return 0;
	t0 = now_us(g);
	os_lock(a);
	while (ST_LOAD(g, slot) != ST_READY)
		os_wait(a);
	os_unlock(a);
	return g->clock_us ? g->clock_us() - t0 : 0;
}

/* Puntero a la página en la caché (la trae si falta). `span` = páginas que abarca el acceso completo. */
static const uint8_t *page_ptr(struct salsa64_rompg *g, uint32_t page, int span)
{
	int s = g->map[page];
	int trig = 0, cold = 0;   /* seguir pidiendo por delante; fallo frío */
	uint64_t wait;

	if (!g->as) {
		/* síncrono, como siempre: la falta se lee y, si es acceso secuencial, también la siguiente */
		if (s < 0) {
			s = claim(g, page, 0, 0);
			g->miss++;
			g->last_page = page;   /* la que acaba de entrar no se pisa */
			if (page > 0 && g->map[page - 1] >= 0 && page + 1 < g->npages && g->map[page + 1] < 0)
				claim(g, page + 1, 0, 1);
		}
		g->last_page = page;
		g->slot_stamp[s] = ++g->stamp;
		return g->mem + ((size_t)s << g->shift);
	}

	if (s < 0) {
		/* fría: al frente de la cola. Si todas las ranuras están leyéndose, espera a una y reintenta. */
			while ((s = claim(g, page, 1, 0)) < 0) {
			uint32_t i = 0;
			int keep = g->last_page != NO_PAGE ? g->map[g->last_page] : -1;
			while (i < g->nslots && ((int)i == keep || ST_LOAD(g, i) != ST_LOADING))
				i++;
			if (i == g->nslots) {   /* no puede pasar con nslots >= 2 */
				g->errs++;
				return g->mem;
			}
			slot_wait(g, i);
		}
		g->miss++;
		cold = 1;
		trig = 1;
	} else if (g->slot_pf[s]) {
		if (ST_LOAD(g, (uint32_t)s) == ST_READY)
			g->pf_hit++;
		else
			g->wait_n++;   /* venía en camino: se espera solo a esa lectura */
		g->slot_pf[s] = 0;
		trig = 1;
	}
	g->last_page = page;   /* su ranura queda fuera de la elección de víctima */
	if (trig || span > 1) {
		uint32_t n = 0;
		for (uint32_t p = page + 1; n < g->depth && p < g->npages; p++, n++)
			if (g->map[p] < 0 && claim(g, p, 0, 1) < 0)
				break;
	}
	wait = slot_wait(g, (uint32_t)s);
	if (wait) {
		if (cold) {
			g->cold_us += wait;
			if (wait > g->cold_max_us)
				g->cold_max_us = wait;
		} else {
			g->wait_us += wait;
			if (wait > g->wait_max_us)
				g->wait_max_us = wait;
		}
	}
	g->slot_stamp[s] = ++g->stamp;
	return g->mem + ((size_t)s << g->shift);
}

int salsa64_rompg_open(struct salsa64_rompg *g, int (*rd)(void *, uint64_t, uint8_t *, size_t), void *user,
		       uint32_t size, enum salsa64_rom_format fmt, unsigned page_shift, uint8_t *mem,
		       size_t mem_bytes)
{
	if (!g || !rd || !mem || page_shift < 12 || page_shift > 24 || (mem_bytes >> page_shift) < 2 || size < 0x1000)
		return -1;
	memset(g, 0, sizeof *g);
	g->rd = rd;
	g->user = user;
	g->mem = mem;
	g->size = size;
	g->shift = page_shift;
	g->fmt = (uint8_t)fmt;
	g->npages = (uint32_t)(((uint64_t)size + ((uint64_t)1 << page_shift) - 1) >> page_shift);
	g->nslots = (uint32_t)(mem_bytes >> page_shift);
	if (g->nslots > g->npages)
		g->nslots = g->npages < 2 ? 2 : g->npages;
	g->last_page = NO_PAGE;
	g->map = malloc(sizeof(int32_t) * g->npages);
	g->slot_page = malloc(sizeof(uint32_t) * g->nslots);
	g->slot_stamp = calloc(g->nslots, sizeof(uint64_t));
	g->slot_st = calloc(g->nslots, 1);
	g->slot_pf = calloc(g->nslots, 1);
	g->q = malloc(sizeof(uint32_t) * g->nslots);
	if (!g->map || !g->slot_page || !g->slot_stamp || !g->slot_st || !g->slot_pf || !g->q) {
		salsa64_rompg_close(g);
		return -1;
	}
	for (uint32_t i = 0; i < g->npages; i++)
		g->map[i] = -1;
	for (uint32_t i = 0; i < g->nslots; i++)
		g->slot_page[i] = NO_PAGE;
	return 0;
}

int salsa64_rompg_async_start(struct salsa64_rompg *g, unsigned depth)
{
	struct salsa64_rompg_async *a;

	if (!g || !g->map || g->as)
		return -1;
	a = calloc(1, sizeof *a);
	if (!a)
		return -1;
	a->g = g;
	g->q_head = g->q_n = 0;
	g->depth = depth > g->nslots / 4 ? g->nslots / 4 : depth;
	if (g->depth < 1)
		g->depth = 1;
	if (os_init(a, reader_main) != 0) {
		free(a);
		return -1;
	}
	g->as = a;
	return 0;
}

void salsa64_rompg_close(struct salsa64_rompg *g)
{
	struct salsa64_rompg_async *a = g->as;

	if (a) {   /* el lector termina la lectura en curso y sale; lo que quedaba en la cola se descarta */
		os_lock(a);
		a->stop = 1;
		os_wake(a);
		os_unlock(a);
		os_end(a);
		free(a);
	}
	free(g->slot_st);
	free(g->slot_pf);
	free(g->q);
	free(g->map);
	free(g->slot_page);
	free(g->slot_stamp);
	memset(g, 0, sizeof *g);
}

void salsa64_rompg_read(struct salsa64_rompg *g, uint32_t off, uint8_t *dst, uint32_t len)
{
	const uint32_t pmask = (1u << g->shift) - 1u;

	g->acc++;
	while (len) {
		uint32_t page = off >> g->shift, in = off & pmask, n = (pmask + 1u) - in;
		const uint8_t *p = page_ptr(g, page, (int)(((off + len - 1u) >> g->shift) - page) + 1);

		g->last_ptr = p;
		if (n > len)
			n = len;
		memcpy(dst, p + in, n);
		dst += n;
		off += n;
		len -= n;
	}
}

uint32_t salsa64_rompg_rd32(struct salsa64_rompg *g, uint32_t off)
{
	const uint32_t pmask = (1u << g->shift) - 1u;
	uint8_t b[4];

	if ((off >> g->shift) == g->last_page && (off & pmask) <= pmask - 3u)
		return salsa64_rd32(g->last_ptr + (off & pmask));
	salsa64_rompg_read(g, off, b, 4);
	return salsa64_rd32(b);
}

void salsa64_rompg_window(struct salsa64_rompg *g, struct salsa64_rompg_win *w)
{
	w->acc = g->acc - g->w_acc;
	w->miss = g->miss - g->w_miss;
	w->ra = g->ra - g->w_ra;
	w->us = g->read_us - g->w_us;
	w->pf_hit = g->pf_hit - g->w_pf_hit;
	w->wait_n = g->wait_n - g->w_wait_n;
	w->wait_us = g->wait_us - g->w_wait_us;
	w->wait_max_us = g->wait_max_us;
	w->cold_us = g->cold_us - g->w_cold_us;
	g->w_acc = g->acc;
	g->w_miss = g->miss;
	g->w_ra = g->ra;
	g->w_us = g->read_us;
	g->w_pf_hit = g->pf_hit;
	g->w_wait_n = g->wait_n;
	g->w_wait_us = g->wait_us;
	g->w_cold_us = g->cold_us;
	g->wait_max_us = 0;
}
