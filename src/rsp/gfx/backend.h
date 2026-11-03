/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — backends de la HLE gráfica para el núcleo (salsa64_gfx_backend).
 *
 * Cada tarea gráfica pasa por tres manos:
 *   acquire  quien consume la salida da los búferes (vértices, comandos,
 *            texturas) y su capacidad en el trabajo;
 *   HLE      gfx_run, aquí mismo (backend síncrono) o en un SPE (el de la
 *            PS3, en src/platform/ps3/gfx_spu.c);
 *   done     al terminar la tarea (cuando el núcleo la da por terminada),
 *            quien consume recibe el trabajo y su resultado.
 */
#ifndef SALSA64_RSP_GFX_BACKEND_H
#define SALSA64_RSP_GFX_BACKEND_H

#include "core/bus.h"
#include "rsp/gfx/gfx.h"

struct salsa64_gfx_sink {
	void *ctx;
	/* Pone en job los búferes de salida (vtx_ea, cmd_ea, tex_ea y sus
	 * capacidades). Puede esperar a que el consumidor libere uno. */
	void (*acquire)(void *ctx, struct gfx_job *job);
	/* La tarea terminó: la salida está completa. */
	void (*done)(void *ctx, const struct gfx_job *job, const struct gfx_result *res);
};

/* Trabajo para una tarea (sin los búferes de salida). */
void salsa64_gfx_job(struct gfx_job *job, const struct salsa64_bus *bus, const struct salsa64_task *task,
		     enum salsa64_gbi gbi);
/* Lo que `finish` devuelve al núcleo por un resultado (SALSA64_GFX_*). */
uint32_t salsa64_gfx_task_flags(const struct gfx_result *res);
/* Ritmo del RCP (gfx_model en rcp.h, docs/ritmo-rcp.md): cuántos ciclos del
 * VR4300 tardaría la consola en una tarea con este resultado de la HLE,
 * estimado con sus comandos, triángulos y vértices según la familia del
 * microcódigo y calibrado con lo que tarda el microcódigo real en ares; 0 si
 * no se midió. */
uint32_t salsa64_gfx_model_cycles(const struct gfx_result *res, enum salsa64_gbi gbi);
/* Al terminar la tarea (en `finish`, en el hilo del CPU): los rellenos de
 * la z image de su salida (GFX_OP_ZCLEAR) se escriben en la RDRAM, como los
 * deja el RDP. El CPU puede leerla: Donkey Kong 64 mira la profundidad
 * donde se ve al jugador y, si algo lo tapa 21 cuadros seguidos, mueve la
 * cámara; sin el relleno leía 0 (tapado) y la cámara iba y venía. Lo que
 * se dibuja encima no llega: la z image queda como recién borrada. */
void salsa64_gfx_rdram_fills(const struct salsa64_bus *bus, const struct gfx_job *job,
			     const struct gfx_result *res);

/* Backend síncrono: la HLE corre en el hilo del CPU al arrancar la tarea. */
struct gfx_state;
struct salsa64_gfx_sync {
	struct salsa64_gfx_backend backend;
	/* RDP a bajo nivel: cada tramo DPC_START..DPC_END es un trabajo
	 * GFX_JOB_DPC sobre el mismo estado (salsa64_rsp_set_dpc). */
	struct salsa64_dpc_backend dpc;
	struct gfx_state *state;
	const struct salsa64_bus *bus;
	struct salsa64_gfx_sink sink;
	struct gfx_job job;
	struct gfx_result res;
	bool pending;
	bool reset;
	bool nocache;   /* las listas DPC usan solo el arena de su tarea */
	bool dpc_open;  /* el tramo anterior no cerró su lista (sin SYNC_FULL) */
	uint32_t dpc_runs;
	struct gfx_result dpc_sum;
};

/* `state` lo pone quien llama (~20 KB) y queda inicializado. */
void salsa64_gfx_sync_init(struct salsa64_gfx_sync *s, struct gfx_state *state, const struct salsa64_bus *bus,
			   const struct salsa64_gfx_sink *sink);

#endif
