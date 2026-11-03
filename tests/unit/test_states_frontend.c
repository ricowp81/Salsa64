/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "test.h"

#ifdef SALSA64_STATES_HOST_TEST
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

static bool writer_ready, writer_wait_ok;
static unsigned allocations, releases, writer_stops, io_stops, fail_allocation;

static void *snapshot_alloc(size_t alignment, size_t bytes)
{
	allocations++;
	return allocations == fail_allocation ? NULL : memalign(alignment, bytes);
}

static void snapshot_free(void *p)
{
	if (p) {
		/* La memoria no puede soltarse antes de esperar y parar el escritor. */
		CHECK(!writer_ready);
		releases++;
	}
	free(p);
}

/* El código real del frontend, cambiando solo sus servicios externos. */
#define memalign snapshot_alloc
#define free snapshot_free
#include "frontend/states.c"
#undef free
#undef memalign

void salsa64_log(const char *fmt, ...) { (void)fmt; }
bool bgjob_init(void) { writer_ready = true; return true; }
bool bgjob_ready(void) { return writer_ready; }
bool bgjob_wait(uint64_t us) { (void)us; return writer_wait_ok; }
void bgjob_shutdown(void) { writer_stops++; writer_ready = false; }
bool bgjob_submit(bgjob_fn fn, void *arg) { (void)fn; (void)arg; return false; }
void bgio_shutdown(void) { io_stops++; }
void bgio_lz_reset(void) {}
void bgio_lz_get(struct bgio_lz_stats *out) { memset(out, 0, sizeof *out); }
bool bgio_out_open(struct bgio_out *out, const char *path) { (void)out; (void)path; return false; }
bool bgio_out_write(void *ctx, const void *data, uint32_t len)
{ (void)ctx; (void)data; (void)len; return false; }
bool bgio_out_patch(void *ctx, uint32_t off, const uint8_t *data, uint32_t len)
{ (void)ctx; (void)off; (void)data; (void)len; return false; }
bool bgio_out_finish(struct bgio_out *out, bool ok, const char *tmp, const char *path, char *err, size_t n)
{ (void)out; (void)ok; (void)tmp; (void)path; (void)err; (void)n; return false; }
bool bgio_compress(void *ctx, const uint8_t *src, uint32_t len,
		   bool (*write)(void *, const void *, uint32_t), void *wctx)
{ (void)ctx; (void)src; (void)len; (void)write; (void)wctx; return false; }
const uint32_t *rsxkit_back_buffer(uint32_t *pitch) { *pitch = 0; return NULL; }

static void reset_counts(void)
{
	states_keep_snapshot(false);
	writer_ready = false;
	writer_wait_ok = true;
	allocations = releases = writer_stops = io_stops = fail_allocation = 0;
}

static void memory_between_games(void)
{
	reset_counts();
	for (unsigned game = 0; game < 3; game++) {
		CHECK(states_init(8u << 20));
		CHECK_EQ(states_reserved(), 9u << 20);
		CHECK(states_init(8u << 20));
		CHECK_EQ(allocations, 2 * (game + 1));
		states_shutdown(1000);
		CHECK_EQ(states_reserved(), 0);
		CHECK_EQ(releases, 2 * (game + 1));
		CHECK_EQ(writer_stops, game + 1);
		CHECK(!bg.small && !bg.ram && !bg.snap.small && !bg.snap.ram);
	}
	states_shutdown(1000);  /* idempotente */
	CHECK_EQ(releases, 6);
}

static void active_writer_keeps_memory(void)
{
	reset_counts();
	CHECK(states_init(8u << 20));
	bg.pending = 1;
	bg.ram[0] = 0x5a;
	writer_wait_ok = false;
	states_shutdown(1000);
	CHECK_EQ(states_reserved(), 9u << 20);
	CHECK_EQ(releases, 0);
	CHECK_EQ(io_stops, 0);
	CHECK(states_busy());
	CHECK_EQ(bg.ram[0], 0x5a);
	writer_wait_ok = true;
	bg.finished = 1;
	bg.res.ok = true;
	states_shutdown(1000);
	CHECK_EQ(states_reserved(), 0);
	CHECK_EQ(releases, 2);
	CHECK(!states_busy());
	struct states_done result;
	CHECK(!states_poll(&result));
	CHECK(states_init(4u << 20));
	CHECK_EQ(states_reserved(), 5u << 20);
	states_shutdown(1000);
	CHECK_EQ(releases, 4);
}

static void retained_snapshot_switch(void)
{
	reset_counts();
	states_keep_snapshot(true);
	CHECK(states_init(8u << 20));
	states_shutdown(1000);
	CHECK_EQ(states_reserved(), 9u << 20);
	CHECK_EQ(releases, 0);
	CHECK(!writer_ready);
	CHECK(states_init(8u << 20));
	CHECK(writer_ready);
	CHECK_EQ(allocations, 2);
	states_keep_snapshot(false);
	states_shutdown(1000);
	CHECK_EQ(states_reserved(), 0);
	CHECK_EQ(releases, 2);
}

static void partial_allocation(void)
{
	for (unsigned fail = 1; fail <= 2; fail++) {
		reset_counts();
		fail_allocation = fail;
		CHECK(!states_init(8u << 20));
		CHECK_EQ(states_reserved(), 0);
		CHECK_EQ(releases, 1);
		states_shutdown(1000);
		CHECK_EQ(releases, 1);
	}
}
#endif

void t_suite_states_frontend(void)
{
#ifdef SALSA64_STATES_HOST_TEST
	t_run("estados: libera los 9 MB entre juegos", memory_between_games);
	t_run("estados: conserva la foto hasta que termina el escritor", active_writer_keeps_memory);
	t_run("estados: limpia una reserva parcial", partial_allocation);
	t_run("estados: interruptor para conservar la foto", retained_snapshot_switch);
#endif
}
