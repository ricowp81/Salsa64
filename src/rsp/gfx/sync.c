/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — backend síncrono de la HLE gráfica. Ver backend.h.
 */
#include <string.h>

#include "core/endian.h"
#include "cpu/rec.h"
#include "cpu/vr4300.h"
#include "rsp/gfx/backend.h"
#include "rsp/gfx/hle.h"

void salsa64_gfx_job(struct gfx_job *job, const struct salsa64_bus *bus, const struct salsa64_task *task,
		     enum salsa64_gbi gbi)
{
	memset(job, 0, sizeof *job);
	job->rdram_ea = (uint64_t)(uintptr_t)bus->rdram;
	job->rdram_size = bus->rdram_size;
	job->gbi = gbi;
	job->dl = task->data_ptr & 0x00FFFFFF;
	job->ucode = task->ucode & 0x00FFFFFF;
	/* El de Perfect Dark (sin texto) se toma como NoN: sin recorte el autor lo
	 * jugó bien en la PS3 y GLide64 dibuja los muros cercanos de su intro
	 * que el recorte quitaría; no está verificado contra hardware. */
	if (bus->rsp.gfx_nonear || gbi == SALSA64_GBI_F3DPD)
		job->flags |= GFX_JOB_NONEAR;
}

uint32_t salsa64_gfx_task_flags(const struct gfx_result *res)
{
	uint32_t f = 0;
	if (res->flags & GFX_RES_FULLSYNC)
		f |= SALSA64_GFX_FULLSYNC;
	if ((res->flags & GFX_RES_CUT) || res->dl_errors)
		f |= SALSA64_GFX_CUT;
	return f;
}

/* Ritmo del RCP: ciclos del VR4300 (93,75 MHz) por comando de la lista, por
 * triángulo que la HLE dibujó y por vértice cargado, por familia de
 * microcódigo. Mínimos cuadrados sin término fijo (y sin coeficientes
 * negativos) sobre el tiempo del RSP real en ares, de su arranque a la
 * interrupción del DP sin los tramos cedidos al audio, de 62 tareas volcadas
 * en ares y repetidas con la HLE (docs/ritmo-rcp.md):
 *   Fast3D: Super Mario 64, Shadows of the Empire y GoldenEye (32 tareas,
 *     error medio 3 %, máximo 12 %);
 *   F3DEX 1.x: F-Zero X (2.03, 10 tareas, error medio 5 %). Mario Kart 64
 *     (0.95) tarda el doble en lo mismo: con estos queda por debajo, nunca
 *     más lento que en ares;
 *   F3DEX2: Zelda OoT (F3DZEX 2.06H, 10 tareas, error medio 2 %).
 * Lo que no se midió (Factor 5, Turbo3D, S2DEX sueltos) da 0: sin ritmo. El
 * RDP de ares termina en el acto, así que lo que tarda el RDP real (relleno)
 * no está en estas cifras: es una cota inferior de la consola. */
uint32_t salsa64_gfx_model_cycles(const struct gfx_result *res, enum salsa64_gbi gbi)
{
	uint32_t per_cmd, per_tri, per_vtx;
	switch (gbi) {
	case SALSA64_GBI_F3D:
	case SALSA64_GBI_F3DWR:
	case SALSA64_GBI_F3DPD:
	case SALSA64_GBI_F3DDKR:
	case SALSA64_GBI_F3DJFG:
	case SALSA64_GBI_TURBO3D_F3D:
		per_cmd = 154, per_tri = 412, per_vtx = 202;
		break;
	case SALSA64_GBI_F3DEX:
		per_cmd = 41, per_tri = 124, per_vtx = 158;
		break;
	case SALSA64_GBI_F3DEX2:
	case SALSA64_GBI_F3DZEX2:
	case SALSA64_GBI_F3DEXBG:
	case SALSA64_GBI_F3DEX2ACCLAIM:
	case SALSA64_GBI_F3DEX2EUROCOM:
		per_cmd = 59, per_tri = 134, per_vtx = 99;
		break;
	default:
		return 0;
	}
	uint64_t c = (uint64_t)res->commands * per_cmd + (uint64_t)res->triangles * per_tri +
		     (uint64_t)res->vertices * per_vtx;
	return c > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)c;
}

/* Un relleno en modo FILL: cada byte de la imagen toma el byte del color
 * de 32 bits que le toca por su dirección (el RDP repite el color en
 * palabras de 64 bits), así sirve para píxeles de 8, 16 y 32 bits. */
static void fill_rdram(const struct salsa64_bus *bus, const struct gfx_zclear *z)
{
	static const uint8_t bpp[4] = { 0, 1, 2, 4 };
	uint32_t width = z->h.c & 0xFFFF, bytes = bpp[(z->h.c >> 16) & 3];
	uint32_t x0 = z->rect[0], x1 = z->rect[2] < width ? z->rect[2] : width;
	if (!bytes || x0 >= x1)
		return;
	uint32_t lo = ~0u, hi = 0;
	for (uint32_t y = z->rect[1]; y < z->rect[3]; y++) {
		uint32_t row = (z->h.a & 0x00FFFFFF) + (y * width + x0) * bytes, len = (x1 - x0) * bytes;
		if (row >= bus->rdram_size || len > bus->rdram_size - row)
			break;
		uint32_t a = row, end = row + len;
		for (; a < end && (a & 3); a++)
			bus->rdram[a] = (uint8_t)(z->fill >> (24 - 8 * (a & 3)));
		for (; a + 4 <= end; a += 4)
			salsa64_wr32(bus->rdram + a, z->fill);
		for (; a < end; a++)
			bus->rdram[a] = (uint8_t)(z->fill >> (24 - 8 * (a & 3)));
		lo = row < lo ? row : lo;
		hi = end;
	}
	/* Lo escribe el RDP, no un DMA: no es un aviso para los efectos de
	 * framebuffer (rdram_written), solo para el código recompilado. */
	if (hi > lo && bus->cpu && bus->cpu->rec)
		salsa64_rec_invalidate(bus->cpu->rec, lo, hi - lo);
}

void salsa64_gfx_rdram_fills(const struct salsa64_bus *bus, const struct gfx_job *job,
			     const struct gfx_result *res)
{
	const uint8_t *cmd = (const uint8_t *)(uintptr_t)job->cmd_ea;
	for (uint32_t off = 0; off + sizeof(struct gfx_cmd) <= res->cmd_bytes;) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(cmd + off);
		if (c->op == GFX_OP_END || c->size < sizeof(struct gfx_cmd))
			break;
		if (c->op == GFX_OP_ZCLEAR && c->size >= sizeof(struct gfx_zclear))
			fill_rdram(bus, (const struct gfx_zclear *)c);
		off += c->size;
	}
}

static void sync_start(void *ctx, const struct salsa64_task *task, enum salsa64_gbi gbi)
{
	struct salsa64_gfx_sync *s = ctx;
	salsa64_gfx_job(&s->job, s->bus, task, gbi);
	if (s->reset) {
		s->job.flags |= GFX_JOB_RESET;
		s->reset = false;
	}
	s->sink.acquire(s->sink.ctx, &s->job);
	gfx_run(s->state, &s->job, &s->res);
	s->pending = true;
}

static uint32_t sync_finish(void *ctx)
{
	struct salsa64_gfx_sync *s = ctx;
	if (!s->pending)
		return 0;
	s->pending = false;
	salsa64_gfx_rdram_fills(s->bus, &s->job, &s->res);
	s->sink.done(s->sink.ctx, &s->job, &s->res);
	return salsa64_gfx_task_flags(&s->res);
}

static uint32_t sync_cycles(void *ctx)
{
	struct salsa64_gfx_sync *s = ctx;
	return salsa64_gfx_model_cycles(&s->res, (enum salsa64_gbi)s->job.gbi);
}

void gfx_state_save(const struct gfx_state *g, uint8_t *out)
{
	gfx_state_pack_regs(g, out);
	memcpy(out + GFX_STATE_REGS, g->tmem, sizeof g->tmem);
}

bool gfx_state_load(struct gfx_state *g, const uint8_t *in)
{
	if (!gfx_state_unpack_regs(g, in))
		return false;
	memcpy(g->tmem, in + GFX_STATE_REGS, sizeof g->tmem);
	gfx_fb_forget(g);  /* el consumidor también suelta sus destinos */
	return true;
}

static bool sync_save_state(void *ctx, uint8_t *buf)
{
	struct salsa64_gfx_sync *s = ctx;
	gfx_state_save(s->state, buf);
	return true;
}

static bool sync_load_state(void *ctx, const uint8_t *buf)
{
	struct salsa64_gfx_sync *s = ctx;
	s->reset = false;
	return gfx_state_load(s->state, buf);
}

static uint32_t sync_dpc_run(void *ctx, uint32_t start, uint32_t end, const uint8_t *dmem)
{
	struct salsa64_gfx_sync *s = ctx;
	struct gfx_job job;
	struct gfx_result res;
	memset(&job, 0, sizeof job);
	job.rdram_ea = (uint64_t)(uintptr_t)s->bus->rdram;
	job.rdram_size = s->bus->rdram_size;
	job.dmem_ea = (uint64_t)(uintptr_t)dmem;
	job.dl = start;
	job.dl_end = end;
	job.flags = GFX_JOB_DPC | (dmem ? GFX_JOB_XBUS : 0);
	if (s->nocache)
		job.flags |= GFX_JOB_NOCACHE;
	if (s->reset) {
		job.flags |= GFX_JOB_RESET;
		s->reset = false;
	}
	if (s->dpc_open)
		job.flags |= GFX_JOB_DPC_CONT;
	s->sink.acquire(s->sink.ctx, &job);
	gfx_run(s->state, &job, &res);
	salsa64_gfx_rdram_fills(s->bus, &job, &res);
	s->sink.done(s->sink.ctx, &job, &res);
	s->dpc_runs++;
	s->dpc_sum.commands += res.commands;
	s->dpc_sum.triangles += res.triangles;
	s->dpc_sum.rects += res.rects;
	s->dpc_sum.draws += res.draws;
	s->dpc_sum.unknown_cmds += res.unknown_cmds;
	s->dpc_sum.dl_errors += res.dl_errors;
	s->dpc_sum.flags |= res.flags;
	s->dpc_open = !(res.flags & GFX_RES_FULLSYNC);
	return (res.flags & GFX_RES_FULLSYNC) ? SALSA64_GFX_FULLSYNC : 0;
}

void salsa64_gfx_sync_init(struct salsa64_gfx_sync *s, struct gfx_state *state, const struct salsa64_bus *bus,
			   const struct salsa64_gfx_sink *sink)
{
	memset(s, 0, sizeof *s);
	s->backend.ctx = s;
	s->backend.start = sync_start;
	s->backend.finish = sync_finish;
	s->backend.cycles = sync_cycles;
	s->backend.state_size = GFX_STATE_BYTES;
	s->backend.save_state = sync_save_state;
	s->backend.load_state = sync_load_state;
	s->backend.s2dex = true;
	s->dpc.ctx = s;
	s->dpc.run = sync_dpc_run;
	s->state = state;
	s->bus = bus;
	s->sink = *sink;
	gfx_init(state);
}
