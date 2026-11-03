/*
 * Salsa64 — pruebas del DMA del SP y de las tareas del RSP (rsp.c).
 */
#include "test.h"

#include <string.h>

#include "core/endian.h"
#include "core/rom.h"
#include "rsp/gfx/backend.h"

#define SP_REG(r) (0x04040000u + 4 * (r))

static void wr(uint32_t pa, uint32_t v)
{
	salsa64_bus_write32(&t_n64.bus, pa, v, ~0u);
}

static uint32_t rd(uint32_t pa)
{
	return salsa64_bus_read32(&t_n64.bus, pa);
}

static void sp_dma_rows(void)
{
	for (unsigned k = 0; k < 0x100; k += 4)
		t_wr32(0x3000 + k, 0xA0000000u + k);
	/* RDRAM → DMEM: 3 filas de 16 bytes saltando 8 entre filas. */
	wr(SP_REG(SALSA64_SP_MEM_ADDR), 0x0100);
	wr(SP_REG(SALSA64_SP_DRAM_ADDR), 0x3000);
	wr(SP_REG(SALSA64_SP_RD_LEN), (8u << 20) | (2u << 12) | 15);
	CHECK_EQ(rd(0x04000100), 0xA0000000);
	CHECK_EQ(rd(0x0400010C), 0xA000000C);
	CHECK_EQ(rd(0x04000110), 0xA0000018);  /* segunda fila: 16 + 8 */
	CHECK_EQ(rd(0x04000120), 0xA0000030);
	CHECK_EQ(rd(0x0400012C), 0xA000003C);
	CHECK_EQ(rd(0x04000130), 0);
	/* Las direcciones quedan al final; el largo se lee 0xFF8 con cuenta 0. */
	CHECK_EQ(rd(SP_REG(SALSA64_SP_MEM_ADDR)), 0x0130);
	CHECK_EQ(rd(SP_REG(SALSA64_SP_DRAM_ADDR)), 0x3048);
	CHECK_EQ(rd(SP_REG(SALSA64_SP_RD_LEN)), (8u << 20) | 0xFF8);
	CHECK_EQ(rd(SP_REG(SALSA64_SP_WR_LEN)), (8u << 20) | 0xFF8);

	/* IMEM → RDRAM, largo no múltiplo de 8 (se redondea hacia arriba), y
	 * la dirección del SP da la vuelta dentro de la IMEM. */
	wr(0x04001FF8, 0x11111111);
	wr(0x04001FFC, 0x22222222);
	wr(0x04001000, 0x33333333);
	wr(SP_REG(SALSA64_SP_MEM_ADDR), 0x1FF8);
	wr(SP_REG(SALSA64_SP_DRAM_ADDR), 0x4000);
	wr(SP_REG(SALSA64_SP_WR_LEN), 11);
	CHECK_EQ(t_rd32(0x4000), 0x11111111);
	CHECK_EQ(t_rd32(0x4004), 0x22222222);
	CHECK_EQ(t_rd32(0x4008), 0x33333333);
	CHECK_EQ(rd(SP_REG(SALSA64_SP_MEM_ADDR)), 0x1008);
}

/* Backend gráfico de mentira: cuenta llamadas y pide sincronizar el RDP. */
static struct {
	int starts, finishes;
	struct salsa64_task task;
	enum salsa64_gbi gbi;
} fake;

static void fake_start(void *ctx, const struct salsa64_task *task, enum salsa64_gbi gbi)
{
	(void)ctx;
	fake.starts++;
	fake.task = *task;
	fake.gbi = gbi;
}

static uint32_t fake_flags = SALSA64_GFX_FULLSYNC;  /* lo que devuelve finish */

static uint32_t fake_finish(void *ctx)
{
	(void)ctx;
	fake.finishes++;
	return fake_flags;
}

static const struct salsa64_gfx_backend fake_backend = { .start = fake_start, .finish = fake_finish };

/* Backend que tarda: `done` dice que no las primeras `fake_busy` veces y
 * anota si el núcleo le avisó que el CPU estaba en un bucle de espera. */
static int fake_busy, fake_dones, fake_idle_seen;

static bool fake_done(void *ctx, bool idle)
{
	(void)ctx;
	fake_dones++;
	fake_idle_seen += idle;
	return fake_busy-- <= 0;
}

static const struct salsa64_gfx_backend slow_backend = { .start = fake_start, .finish = fake_finish, .done = fake_done };

static void put_task(uint32_t type, uint32_t ucode_data, uint32_t dl)
{
	uint8_t *d = t_n64.bus.sp_mem + SALSA64_TASK_DMEM;
	memset(d, 0, 64);
	salsa64_wr32(d + 0x00, type);
	salsa64_wr32(d + 0x10, 0x80100000);       /* ucode */
	salsa64_wr32(d + 0x18, 0x80000000 | ucode_data);
	salsa64_wr32(d + 0x1C, 0x800);
	salsa64_wr32(d + 0x30, 0x80000000 | dl);  /* data_ptr */
	salsa64_wr32(d + 0x34, 0x40);
}

static void put_text(uint32_t pa, const char *s)
{
	memcpy(t_n64.bus.rdram + pa, s, strlen(s));
}

static void rsp_gfx_task(void)
{
	struct salsa64_bus *bus = &t_n64.bus;
	memset(&fake, 0, sizeof fake);
	salsa64_rsp_set_gfx(bus, &fake_backend);
	put_text(0x5000 + 0x123, "Salsa64 GBI F3DEX2");
	put_task(SALSA64_TASK_GFX, 0x5000, 0x6000);
	wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4 | 0x100);  /* quitar HALT y BROKE, int. en BREAK */

	CHECK_EQ(fake.starts, 1);
	CHECK_EQ(fake.gbi, SALSA64_GBI_F3DEX2);
	CHECK_EQ(fake.task.data_ptr, 0x80006000);
	CHECK_EQ(bus->rsp.busy, SALSA64_TASK_GFX);
	CHECK_EQ(rd(SP_REG(SALSA64_SP_STATUS)) & SALSA64_SP_STATUS_HALT, 0);
	CHECK_EQ(bus->mi.intr, 0);

	/* Fin de la tarea: BROKE, HALT, TASKDONE e interrupción SP; luego la DP. */
	uint64_t t0 = CPU->cycles;
	salsa64_sched_dispatch(&t_n64.sched, t0 + bus->rsp.gfx_cycles);
	CHECK_EQ(fake.finishes, 1);
	CHECK_EQ(bus->rsp.busy, 0);
	CHECK_EQ(rd(SP_REG(SALSA64_SP_STATUS)),
		 SALSA64_SP_STATUS_HALT | SALSA64_SP_STATUS_BROKE | SALSA64_SP_STATUS_INTR_BREAK |
		 SALSA64_SP_STATUS_TASKDONE);
	CHECK_EQ(bus->mi.intr, 1u << SALSA64_IRQ_SP);
	salsa64_sched_dispatch(&t_n64.sched, t0 + bus->rsp.gfx_cycles + bus->rsp.dp_cycles);
	CHECK_EQ(bus->mi.intr, (1u << SALSA64_IRQ_SP) | (1u << SALSA64_IRQ_DP));

	/* F3D también va al backend. */
	wr(SP_REG(SALSA64_SP_STATUS), 0x8);  /* bajar la interrupción SP */
	bus->mi.intr = 0;
	memset(t_n64.bus.rdram + 0x5000, 0, 0x800);
	put_text(0x5000 + 0x40, "Salsa64 GBI Fast3D");
	wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4);
	CHECK_EQ(fake.starts, 2);
	CHECK_EQ(fake.gbi, SALSA64_GBI_F3D);
	CHECK_EQ(bus->rsp.busy, SALSA64_TASK_GFX);
	salsa64_sched_dispatch(&t_n64.sched, CPU->cycles + bus->rsp.gfx_cycles + bus->rsp.dp_cycles);
	CHECK_EQ(fake.finishes, 2);

	/* Y las variantes de Rare. */
	wr(SP_REG(SALSA64_SP_STATUS), 0x8);
	bus->mi.intr = 0;
	memset(t_n64.bus.rdram + 0x5000, 0, 0x800);
	put_text(0x5000 + 0x40, "Salsa64 GBI F3DEXBG");
	wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4);
	CHECK_EQ(fake.starts, 3);
	CHECK_EQ(fake.gbi, SALSA64_GBI_F3DEXBG);
	salsa64_sched_dispatch(&t_n64.sched, CPU->cycles + bus->rsp.gfx_cycles + bus->rsp.dp_cycles);
	CHECK_EQ(fake.finishes, 3);
	wr(SP_REG(SALSA64_SP_STATUS), 0x8);
	bus->mi.intr = 0;
	memset(t_n64.bus.rdram + 0x5000, 0, 0x800);
	put_text(0x5000 + 0x40, "Salsa64 GBI F3DPD");
	wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4);
	CHECK_EQ(fake.starts, 4);
	CHECK_EQ(fake.gbi, SALSA64_GBI_F3DPD);
	salsa64_sched_dispatch(&t_n64.sched, CPU->cycles + bus->rsp.gfx_cycles + bus->rsp.dp_cycles);
	CHECK_EQ(fake.finishes, 4);

	/* Microcódigo desconocido: no llega al backend y se detiene en el acto;
	 * la interrupción del DP llega igual (quien espere el fin del RDP no
	 * se cuelga). */
	wr(SP_REG(SALSA64_SP_STATUS), 0x8);
	wr(0x04300000, 0x0800);  /* MI_MODE: bajar la interrupción DP */
	memset(t_n64.bus.rdram + 0x5000, 0, 0x800);
	wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4);
	CHECK_EQ(fake.starts, 4);
	CHECK_EQ(bus->rsp.stats.unknown_gbi, 1);
	CHECK_EQ(bus->rsp.stats.unknown_data, 0x80005000);
	CHECK(rd(SP_REG(SALSA64_SP_STATUS)) & SALSA64_SP_STATUS_BROKE);
	CHECK_EQ(bus->mi.intr & (1u << SALSA64_IRQ_DP), 0);
	salsa64_sched_dispatch(&t_n64.sched, CPU->cycles + bus->rsp.dp_cycles);
	CHECK(bus->mi.intr & (1u << SALSA64_IRQ_DP));
	salsa64_rsp_set_gfx(bus, NULL);
}

/* El fin de la tarea se vuelve a programar mientras el backend no termina;
 * el CPU sigue sin HALT, otro arranque se ignora y TASKDONE llega cuando
 * el backend dice que sí. */
static void rsp_gfx_task_polls(void)
{
	struct salsa64_bus *bus = &t_n64.bus;
	memset(&fake, 0, sizeof fake);
	fake_busy = 3;
	fake_dones = fake_idle_seen = 0;
	salsa64_rsp_set_gfx(bus, &slow_backend);
	put_text(0x5000 + 0x123, "Salsa64 GBI F3DEX2");
	put_task(SALSA64_TASK_GFX, 0x5000, 0x6000);
	wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4 | 0x100);
	CHECK_EQ(fake.starts, 1);

	uint64_t t0 = CPU->cycles, t = t0 + bus->rsp.gfx_cycles;
	for (int i = 0; i < 3; i++) {
		salsa64_sched_dispatch(&t_n64.sched, t);
		CHECK_EQ(fake.finishes, 0);
		CHECK_EQ(bus->rsp.busy, SALSA64_TASK_GFX);
		CHECK_EQ(rd(SP_REG(SALSA64_SP_STATUS)) & SALSA64_SP_STATUS_HALT, 0);
		CHECK_EQ(bus->rsp.stats.polls, (uint32_t)(i + 1));
		CHECK_EQ(t_n64.sched.when[SALSA64_EV_SP_TASK], t + bus->rsp.poll_cycles);
		/* Con la tarea en curso no arranca otra: sp.c solo arranca al
		 * quitar un HALT puesto, y aunque llegara aquí se ignora. */
		wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4);
		salsa64_rsp_start(bus);
		CHECK_EQ(fake.starts, 1);
		CHECK_EQ(bus->rsp.stats.starts_busy, (uint32_t)(i + 1));
		t += bus->rsp.poll_cycles;
	}
	CHECK_EQ(fake_idle_seen, 0);  /* el CPU no saltó ciclos */
	/* Un salto de bucle de espera entre sondeos se le avisa al backend. */
	CPU->idle_cycles += 5000;
	fake_busy = 1;
	salsa64_sched_dispatch(&t_n64.sched, t);
	CHECK_EQ(fake_idle_seen, 1);
	CHECK_EQ(fake.finishes, 0);
	t += bus->rsp.poll_cycles;
	salsa64_sched_dispatch(&t_n64.sched, t);
	CHECK_EQ(fake_idle_seen, 1);  /* sin más saltos, sin aviso */
	CHECK_EQ(fake.finishes, 1);
	CHECK_EQ(bus->rsp.busy, 0);
	CHECK_EQ(rd(SP_REG(SALSA64_SP_STATUS)),
		 SALSA64_SP_STATUS_HALT | SALSA64_SP_STATUS_BROKE | SALSA64_SP_STATUS_INTR_BREAK |
		 SALSA64_SP_STATUS_TASKDONE);
	CHECK_EQ(bus->mi.intr, 1u << SALSA64_IRQ_SP);
	salsa64_sched_dispatch(&t_n64.sched, t + bus->rsp.dp_cycles);
	CHECK_EQ(bus->mi.intr, (1u << SALSA64_IRQ_SP) | (1u << SALSA64_IRQ_DP));
	CHECK_EQ(fake_dones, 5);
	salsa64_rsp_set_gfx(bus, NULL);
}

/* Ritmo del RCP (gfx_model): una tarea que la HLE termina pronto no termina
 * para el juego antes de lo que el backend estima que tardaría la consola
 * (por el porcentaje), y la que tardaría menos que gfx_cycles termina como
 * siempre. También al recogerla antes (salsa64_rsp_quiesce, estados). */
static uint32_t fake_model;

static uint32_t fake_cycles(void *ctx)
{
	(void)ctx;
	return fake_model;
}

static const struct salsa64_gfx_backend model_backend = { .start = fake_start, .finish = fake_finish,
							  .cycles = fake_cycles };

static void rsp_gfx_model(void)
{
	struct salsa64_bus *bus = &t_n64.bus;
	memset(&fake, 0, sizeof fake);
	salsa64_rsp_set_gfx(bus, &model_backend);
	salsa64_mi_clear(bus, SALSA64_IRQ_SP);
	salsa64_mi_clear(bus, SALSA64_IRQ_DP);
	put_text(0x5000 + 0x123, "Salsa64 GBI F3DEX2");
	put_task(SALSA64_TASK_GFX, 0x5000, 0x6000);
	bus->rsp.gfx_model = 150;
	fake_model = 400000;  /* ×1,5 = 600000 ciclos */
	uint32_t held0 = bus->rsp.stats.model_held;
	wr(SP_REG(SALSA64_SP_STATUS), 0x8);
	wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4 | 0x100);
	CHECK_EQ(fake.starts, 1);
	uint64_t t0 = CPU->cycles;
	CHECK_EQ(bus->rsp.gfx_start, t0);
	salsa64_sched_dispatch(&t_n64.sched, t0 + bus->rsp.gfx_cycles);
	/* La HLE ya terminó (finish), pero el juego todavía no lo ve. */
	CHECK_EQ(fake.finishes, 1);
	CHECK_EQ(bus->rsp.busy, SALSA64_TASK_GFX);
	CHECK(bus->rsp.collected);
	CHECK_EQ(rd(SP_REG(SALSA64_SP_STATUS)) & SALSA64_SP_STATUS_HALT, 0);
	CHECK_EQ(bus->mi.intr & (1u << SALSA64_IRQ_SP), 0);
	CHECK_EQ(t_n64.sched.when[SALSA64_EV_SP_TASK], t0 + 600000);
	CHECK_EQ(bus->rsp.stats.model_held, held0 + 1);
	/* Una tarea nueva no arranca mientras tanto. */
	wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4);
	CHECK_EQ(fake.starts, 1);
	salsa64_sched_dispatch(&t_n64.sched, t0 + 600000);
	CHECK_EQ(fake.finishes, 1);  /* no se recoge dos veces */
	CHECK_EQ(bus->rsp.busy, 0);
	CHECK_EQ(bus->mi.intr, 1u << SALSA64_IRQ_SP);
	salsa64_sched_dispatch(&t_n64.sched, t0 + 600000 + bus->rsp.dp_cycles);
	CHECK_EQ(bus->mi.intr, (1u << SALSA64_IRQ_SP) | (1u << SALSA64_IRQ_DP));

	/* Más corta que gfx_cycles: termina a la hora de siempre. */
	wr(SP_REG(SALSA64_SP_STATUS), 0x8);
	salsa64_mi_clear(bus, SALSA64_IRQ_DP);
	fake_model = 1000;
	wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4 | 0x100);
	CHECK_EQ(fake.starts, 2);
	t0 = CPU->cycles;
	salsa64_sched_dispatch(&t_n64.sched, t0 + bus->rsp.gfx_cycles);
	CHECK_EQ(bus->rsp.busy, 0);
	CHECK_EQ(bus->mi.intr, 1u << SALSA64_IRQ_SP);
	CHECK_EQ(bus->rsp.stats.model_held, held0 + 1);

	/* Recogida antes (un estado guardado): el fin queda a la hora del modelo. */
	wr(SP_REG(SALSA64_SP_STATUS), 0x8);
	fake_model = 200000;
	bus->rsp.gfx_model = 100;
	wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4 | 0x100);
	CHECK_EQ(fake.starts, 3);
	t0 = CPU->cycles;
	CHECK(salsa64_rsp_quiesce(bus));
	CHECK_EQ(fake.finishes, 3);
	CHECK_EQ(t_n64.sched.when[SALSA64_EV_SP_TASK], t0 + 200000);
	salsa64_sched_dispatch(&t_n64.sched, t0 + bus->rsp.gfx_cycles);
	CHECK_EQ(bus->rsp.busy, SALSA64_TASK_GFX);
	salsa64_sched_dispatch(&t_n64.sched, t0 + 200000);
	CHECK_EQ(bus->rsp.busy, 0);
	CHECK_EQ(fake.finishes, 3);
	salsa64_sched_dispatch(&t_n64.sched, t0 + 200000 + bus->rsp.dp_cycles);
	wr(SP_REG(SALSA64_SP_STATUS), 0x8);
	salsa64_mi_clear(bus, SALSA64_IRQ_DP);
	bus->rsp.gfx_model = 0;
	salsa64_rsp_set_gfx(bus, NULL);
}

/* El modelo crece con lo que hace la tarea y una tarea vacía no cuesta nada. */
static void rsp_gfx_model_cycles(void)
{
	struct gfx_result r;
	memset(&r, 0, sizeof r);
	uint32_t empty = salsa64_gfx_model_cycles(&r, SALSA64_GBI_F3DEX2);
	CHECK_EQ(empty, 0);
	r.commands = 3000;
	r.triangles = 500;
	uint32_t big = salsa64_gfx_model_cycles(&r, SALSA64_GBI_F3DEX2);
	CHECK(big > empty);
	r.triangles = 1000;
	CHECK(salsa64_gfx_model_cycles(&r, SALSA64_GBI_F3DEX2) > big);
	r.commands = 6000;
	CHECK(salsa64_gfx_model_cycles(&r, SALSA64_GBI_F3DEX2) > big);
	/* Fast3D tarda más que F3DEX2 en lo mismo; lo que no se midió, nada. */
	CHECK(salsa64_gfx_model_cycles(&r, SALSA64_GBI_F3D) > salsa64_gfx_model_cycles(&r, SALSA64_GBI_F3DEX2));
	CHECK_EQ(salsa64_gfx_model_cycles(&r, SALSA64_GBI_F5ROGUE), 0);
	CHECK_EQ(salsa64_gfx_model_cycles(&r, SALSA64_GBI_UNKNOWN), 0);
}

/* Una lista cortada por la HLE (búfer lleno, sin G_RDPFULLSYNC) también
 * termina con la interrupción del DP: el juego espera al RDP por cada tarea
 * gráfica (Zelda OoT: «RCP is HUNG UP» a los 3 s sin ella). */
static void rsp_gfx_task_cut(void)
{
	struct salsa64_bus *bus = &t_n64.bus;
	memset(&fake, 0, sizeof fake);
	fake_flags = SALSA64_GFX_CUT;
	salsa64_rsp_set_gfx(bus, &fake_backend);
	put_text(0x5000 + 0x123, "Salsa64 GBI F3DEX2");
	put_task(SALSA64_TASK_GFX, 0x5000, 0x6000);
	wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4 | 0x100);
	uint64_t t0 = CPU->cycles;
	salsa64_sched_dispatch(&t_n64.sched, t0 + bus->rsp.gfx_cycles);
	CHECK_EQ(bus->mi.intr, 1u << SALSA64_IRQ_SP);
	CHECK_EQ(bus->rsp.stats.cut, 1);
	salsa64_sched_dispatch(&t_n64.sched, t0 + bus->rsp.gfx_cycles + bus->rsp.dp_cycles);
	CHECK_EQ(bus->mi.intr, (1u << SALSA64_IRQ_SP) | (1u << SALSA64_IRQ_DP));

	/* Sin FULLSYNC ni corte (la lista acabó en G_ENDDL): sin DP. */
	salsa64_mi_clear(bus, SALSA64_IRQ_SP);
	salsa64_mi_clear(bus, SALSA64_IRQ_DP);
	fake_flags = 0;
	wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4 | 0x100);
	t0 = CPU->cycles;
	salsa64_sched_dispatch(&t_n64.sched, t0 + bus->rsp.gfx_cycles + bus->rsp.dp_cycles);
	CHECK_EQ(bus->mi.intr, 1u << SALSA64_IRQ_SP);
	CHECK_EQ(bus->rsp.stats.cut, 1);
	fake_flags = SALSA64_GFX_FULLSYNC;
	salsa64_rsp_set_gfx(bus, NULL);
}

static void rsp_audio_task(void)
{
	struct salsa64_bus *bus = &t_n64.bus;
	put_task(SALSA64_TASK_AUDIO, 0x5000, 0x6000);
	wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4 | 0x100);
	CHECK_EQ(bus->rsp.busy, SALSA64_TASK_AUDIO);
	salsa64_sched_dispatch(&t_n64.sched, CPU->cycles + bus->rsp.audio_cycles);
	CHECK(rd(SP_REG(SALSA64_SP_STATUS)) & SALSA64_SP_STATUS_TASKDONE);
	CHECK_EQ(bus->mi.intr, 1u << SALSA64_IRQ_SP);
	salsa64_sched_dispatch(&t_n64.sched, CPU->cycles + 1000000);
	CHECK_EQ(bus->mi.intr, 1u << SALSA64_IRQ_SP);  /* sin DP */
}

/* Una tarea de libultra sin HLE (como la de tipo 4 que manda Zelda OoT al
 * arrancar) termina con TASKDONE: sin él, libultra la toma por un BREAK
 * suelto y nadie despierta. Lo que no parece tarea, sin señales. */
static void rsp_other_task(void)
{
	struct salsa64_bus *bus = &t_n64.bus;
	put_task(4, 0x5000, 0x6000);
	salsa64_wr32(bus->sp_mem + SALSA64_TASK_DMEM + 0x14, 0x20);   /* ucode_size */
	salsa64_wr32(bus->sp_mem + SALSA64_TASK_DMEM + 0x0C, 0x3E8);  /* ucode_boot_size */
	wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4 | 0x100);
	CHECK_EQ(rd(SP_REG(SALSA64_SP_STATUS)),
		 SALSA64_SP_STATUS_HALT | SALSA64_SP_STATUS_BROKE | SALSA64_SP_STATUS_INTR_BREAK |
		 SALSA64_SP_STATUS_TASKDONE);
	CHECK_EQ(bus->mi.intr, 1u << SALSA64_IRQ_SP);

	wr(SP_REG(SALSA64_SP_STATUS), 0x8 | 0x2000);  /* bajar interrupción y TASKDONE */
	memset(bus->sp_mem + SALSA64_TASK_DMEM, 0, 64);
	wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4);
	CHECK_EQ(rd(SP_REG(SALSA64_SP_STATUS)) & SALSA64_SP_STATUS_TASKDONE, 0);
}

/* Los microcódigos ".NoN" no recortan contra el plano cercano: el núcleo lo
 * anota al arrancar la tarea y el trabajo de la HLE lo lleva en sus banderas.
 * El de Perfect Dark (sin texto) se toma como NoN (sync.c). */
static void rsp_gfx_nonear(void)
{
	struct salsa64_bus *bus = &t_n64.bus;
	static const struct { const char *text; bool nonear; } cases[] = {
		{ "RSP Gfx ucode F3DEX.NoN   fifo 1.23 ", true },
		{ "RSP Gfx ucode F3DZEX.NoN  fifo 2.06H ", true },
		{ "RSP Gfx ucode F3DEX         0.95 ", false },
		{ "RSP Gfx ucode F3DEX       fifo 2.08 ", false },
		{ "Salsa64 GBI F3DPD", true },
	};
	salsa64_rsp_set_gfx(bus, &fake_backend);
	for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		memset(&fake, 0, sizeof fake);
		memset(t_n64.bus.rdram + 0x5000, 0, 0x800);
		put_text(0x5000 + 0x40, cases[i].text);
		put_task(SALSA64_TASK_GFX, 0x5000, 0x6000);
		wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4);
		CHECK_EQ(fake.starts, 1);
		struct gfx_job job;
		salsa64_gfx_job(&job, bus, &fake.task, fake.gbi);
		CHECK_EQ((job.flags & GFX_JOB_NONEAR) != 0, cases[i].nonear);
		salsa64_sched_dispatch(&t_n64.sched, CPU->cycles + bus->rsp.gfx_cycles + bus->rsp.dp_cycles);
		wr(SP_REG(SALSA64_SP_STATUS), 0x8);
		bus->mi.intr = 0;
	}
	salsa64_rsp_set_gfx(bus, NULL);
}

static enum salsa64_gbi detect(const char *text)
{
	struct salsa64_task task;
	memset(&task, 0, sizeof task);
	memset(t_n64.bus.rdram + 0x5000, 0, 0x800);
	put_text(0x5000 + 0x40, text);
	task.ucode_data = 0x80005000;
	task.ucode_data_size = 0x800;
	return salsa64_rsp_detect_gbi(&t_n64.bus, &task);
}

/* Los 4 bytes que, tras el estado `state` del CRC-32 (sin la inversión
 * final), lo llevan a `target`: el CRC hacia atrás 32 bits. */
static void crc32_force(uint32_t state, uint32_t target, uint8_t out[4])
{
	uint32_t r = ~target;
	for (int i = 0; i < 32; i++)
		r = (r & 0x80000000u) ? ((r ^ 0xEDB88320u) << 1) | 1 : r << 1;
	r ^= state;
	for (int i = 0; i < 4; i++)
		out[i] = (uint8_t)(r >> (8 * i));
}

/* Microcódigo sin texto (Perfect Dark): por el CRC-32 de su código. Los
 * datos son sintéticos (ceros y 4 bytes calculados para dar ese CRC). */
static void rsp_detect_gbi_crc(void)
{
	const uint32_t code = 0x8000, size = 0x1000;
	uint8_t *p = t_n64.bus.rdram + code;
	memset(p, 0, size);
	uint32_t state = 0xFFFFFFFFu;
	for (uint32_t i = 0; i < size - 4; i++) {
		state ^= p[i];
		for (int k = 0; k < 8; k++)
			state = (state >> 1) ^ (0xEDB88320u & (0u - (state & 1)));
	}
	crc32_force(state, 0xC543D0A8u, p + size - 4);
	CHECK_EQ(salsa64_crc32(p, size), 0xC543D0A8u);

	struct salsa64_task task;
	memset(&task, 0, sizeof task);
	memset(t_n64.bus.rdram + 0x5000, 0, 0x800);
	task.ucode = 0x80000000u | code;
	task.ucode_size = size;
	task.ucode_data = 0x80005000;
	task.ucode_data_size = 0x800;
	CHECK_EQ(salsa64_rsp_detect_gbi(&t_n64.bus, &task), SALSA64_GBI_F3DPD);
	task.ucode_size = 0xFF8;  /* otro tamaño: no es él */
	CHECK_EQ(salsa64_rsp_detect_gbi(&t_n64.bus, &task), SALSA64_GBI_UNKNOWN);
	task.ucode_size = size;
	p[17] ^= 1;
	CHECK_EQ(salsa64_rsp_detect_gbi(&t_n64.bus, &task), SALSA64_GBI_UNKNOWN);
	memset(p, 0, size);
}

/* Wave Race 64 y Shadows of the Empire: sus microcódigos dicen
 * "RSP SW Version: 2.0D" pero no son Fast3D; los distingue el CRC-32 de su
 * código, que va antes que el texto. */
static void rsp_detect_gbi_wr(void)
{
	const uint32_t code = 0x8000, size = 0x1000;
	uint8_t *p = t_n64.bus.rdram + code;
	memset(p, 0, size);
	uint32_t state = 0xFFFFFFFFu;
	for (uint32_t i = 0; i < size - 4; i++) {
		state ^= p[i];
		for (int k = 0; k < 8; k++)
			state = (state >> 1) ^ (0xEDB88320u & (0u - (state & 1)));
	}
	crc32_force(state, 0x81FA4A32u, p + size - 4);
	CHECK_EQ(salsa64_crc32(p, size), 0x81FA4A32u);

	struct salsa64_task task;
	memset(&task, 0, sizeof task);
	memset(t_n64.bus.rdram + 0x5000, 0, 0x800);
	put_text(0x5000 + 0x40, "RSP SW Version: 2.0D, 04-01-96");
	task.ucode = 0x80000000u | code;
	task.ucode_size = size;
	task.ucode_data = 0x80005000;
	task.ucode_data_size = 0x800;
	CHECK_EQ(salsa64_rsp_detect_gbi(&t_n64.bus, &task), SALSA64_GBI_F3DWR);
	memset(p, 0, size);
	state = 0xFFFFFFFFu;
	for (uint32_t i = 0; i < size - 4; i++) {
		state ^= p[i];
		for (int k = 0; k < 8; k++)
			state = (state >> 1) ^ (0xEDB88320u & (0u - (state & 1)));
	}
	crc32_force(state, 0xE7FA4491u, p + size - 4);
	CHECK_EQ(salsa64_crc32(p, size), 0xE7FA4491u);
	CHECK_EQ(salsa64_rsp_detect_gbi(&t_n64.bus, &task), SALSA64_GBI_F3DWR);
	p[17] ^= 1;  /* otro código con el mismo texto: Fast3D */
	CHECK_EQ(salsa64_rsp_detect_gbi(&t_n64.bus, &task), SALSA64_GBI_F3D);
	memset(p, 0, size);
}

/* Diddy Kong Racing: sin texto y con ucode_size en 0 (el RSP carga 4 KB).
 * Se reconoce por el CRC-32 de su código (3BFF208D) o, para las versiones
 * que no se han medido aquí, por el de los 4 KB leídos en palabras de
 * memoria del host (6E6FC893 y 8D91244F el del juego medido; BDE9D1FB, Jet
 * Force Gemini y Mickey's Speedway USA). Datos sintéticos. */
static void rsp_detect_gbi_dkr(void)
{
	const uint32_t code = 0x8000, size = 0x1000;
	uint8_t *p = t_n64.bus.rdram + code;
	struct salsa64_task task;
	memset(&task, 0, sizeof task);
	memset(t_n64.bus.rdram + 0x5000, 0, 0x800);
	task.ucode = 0x80000000u | code;
	task.ucode_data = 0x80005000;
	task.ucode_data_size = 0x800;

	/* El CRC normal. */
	memset(p, 0, size);
	uint32_t state = 0xFFFFFFFFu;
	for (uint32_t i = 0; i < size - 4; i++) {
		state ^= p[i];
		for (int k = 0; k < 8; k++)
			state = (state >> 1) ^ (0xEDB88320u & (0u - (state & 1)));
	}
	crc32_force(state, 0x3BFF208Du, p + size - 4);
	CHECK_EQ(salsa64_crc32(p, size), 0x3BFF208Du);
	task.ucode_size = 0;
	CHECK_EQ(salsa64_rsp_detect_gbi(&t_n64.bus, &task), SALSA64_GBI_F3DDKR);
	task.ucode_size = size;
	CHECK_EQ(salsa64_rsp_detect_gbi(&t_n64.bus, &task), SALSA64_GBI_F3DDKR);
	task.ucode_size = 0x800;  /* otro tamaño: no es él */
	CHECK_EQ(salsa64_rsp_detect_gbi(&t_n64.bus, &task), SALSA64_GBI_UNKNOWN);
	task.ucode_size = 0;
	p[17] ^= 1;
	CHECK_EQ(salsa64_rsp_detect_gbi(&t_n64.bus, &task), SALSA64_GBI_UNKNOWN);

	/* El CRC con las palabras al revés: el flujo que lee el CRC es el de
	 * los bytes (i ^ 3), así que los 4 últimos del flujo son los 4 últimos
	 * bytes del código, al revés. */
	static const struct { uint32_t crc; enum salsa64_gbi gbi; } cases[] = {
		{ 0x8D91244Fu, SALSA64_GBI_F3DDKR }, { 0x6E6FC893u, SALSA64_GBI_F3DDKR },
		{ 0xBDE9D1FBu, SALSA64_GBI_F3DJFG }, { 0xDA51CCDBu, SALSA64_GBI_F5ROGUE },
		{ 0x6859BF8Eu, SALSA64_GBI_F5INDY }, { 0x23FEF05Fu, SALSA64_GBI_F5INDY },
		/* Animal Crossing: F3DZEX2 y S2DEX2 sin texto de versión. */
		{ 0xD39A0D4Fu, SALSA64_GBI_F3DZEX2 }, { 0x02C399DDu, SALSA64_GBI_S2DEX },
		{ 0x2BDCFC8Au, SALSA64_GBI_TURBO3D }, { 0x77195A68u, SALSA64_GBI_TURBO3D_F3D },
	};
	state = 0xFFFFFFFFu;  /* CRC de size − 4 ceros, sin la inversión final */
	for (uint32_t i = 0; i < size - 4; i++)
		for (int k = 0; k < 8; k++)
			state = (state >> 1) ^ (0xEDB88320u & (0u - (state & 1)));
	for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
		uint8_t tail[4];
		memset(p, 0, size);
		crc32_force(state, cases[c].crc, tail);
		for (int i = 0; i < 4; i++)
			p[size - 4 + (3 - i)] = tail[i];
		CHECK_EQ(salsa64_crc32_words_swapped(p, size), cases[c].crc);
		struct salsa64_gfx_backend turbo = { .turbo3d = true };
		if (cases[c].gbi >= SALSA64_GBI_TURBO3D) {
			CHECK_EQ(salsa64_rsp_detect_gbi(&t_n64.bus, &task), SALSA64_GBI_UNKNOWN);
			salsa64_rsp_set_gfx(&t_n64.bus, &turbo);
		}
		CHECK_EQ(salsa64_rsp_detect_gbi(&t_n64.bus, &task), cases[c].gbi);
		salsa64_rsp_set_gfx(&t_n64.bus, NULL);
	}
	memset(p, 0, size);
}

/* F3DEX2ACCLAIM trae el mismo texto de versión que F3DEX2. Solo el
 * interruptor optativo permite anteponer el CRC de palabras del código al
 * texto; apagado, el despacho queda exactamente en F3DEX2. Código sintético. */
static void rsp_detect_gbi_acclaim(void)
{
	struct salsa64_bus *bus = &t_n64.bus;
	const uint32_t code = 0x8000, size = 0x1000;
	uint8_t *p = bus->rdram + code, tail[4];
	uint32_t state = 0xFFFFFFFFu;
	memset(p, 0, size);
	for (uint32_t i = 0; i < size - 4; i++)
		for (int k = 0; k < 8; k++)
			state = (state >> 1) ^ (0xEDB88320u & (0u - (state & 1)));
	crc32_force(state, 0xE44DF568u, tail);
	for (int i = 0; i < 4; i++)
		p[size - 4 + (3 - i)] = tail[i];
	CHECK_EQ(salsa64_crc32_words_swapped(p, size), 0xE44DF568u);

	struct salsa64_task task;
	memset(&task, 0, sizeof task);
	memset(bus->rdram + 0x5000, 0, 0x800);
	put_text(0x5040, "RSP Gfx ucode F3DEX       fifo 2.08 ");
	task.ucode = 0x80000000u | code;
	task.ucode_size = 0;  /* el RSP carga los 4 KB completos */
	task.ucode_data = 0x80005000;
	task.ucode_data_size = 0x800;
	struct salsa64_gfx_backend off = { 0 }, on = { .acclaim = true };
	salsa64_rsp_set_gfx(bus, &off);
	CHECK_EQ(salsa64_rsp_detect_gbi(bus, &task), SALSA64_GBI_F3DEX2);
	salsa64_rsp_set_gfx(bus, &on);
	CHECK_EQ(salsa64_rsp_detect_gbi(bus, &task), SALSA64_GBI_F3DEX2ACCLAIM);
	p[17] ^= 1;
	CHECK_EQ(salsa64_rsp_detect_gbi(bus, &task), SALSA64_GBI_F3DEX2);
	salsa64_rsp_set_gfx(bus, NULL);
	memset(p, 0, size);
}

/* La variante de Eurocom también anuncia F3DEX2 2.08. Su CRC activa el
 * intérprete de G_SPECIAL_1; sin el interruptor sigue siendo F3DEX2. */
static void rsp_detect_gbi_eurocom(void)
{
	struct salsa64_bus *bus = &t_n64.bus;
	const uint32_t code = 0x8000, size = 0x1000;
	uint8_t *p = bus->rdram + code, tail[4];
	uint32_t state = 0xFFFFFFFFu;
	memset(p, 0, size);
	for (uint32_t i = 0; i < size - 4; i++)
		for (int k = 0; k < 8; k++)
			state = (state >> 1) ^ (0xEDB88320u & (0u - (state & 1)));
	crc32_force(state, 0xC6121CAAu, tail);
	for (int i = 0; i < 4; i++)
		p[size - 4 + (3 - i)] = tail[i];
	CHECK_EQ(salsa64_crc32_words_swapped(p, size), 0xC6121CAAu);

	struct salsa64_task task;
	memset(&task, 0, sizeof task);
	memset(bus->rdram + 0x5000, 0, 0x800);
	put_text(0x5040, "RSP Gfx ucode F3DEX       fifo 2.08 ");
	task.ucode = 0x80000000u | code;
	task.ucode_size = 0;
	task.ucode_data = 0x80005000;
	task.ucode_data_size = 0x800;
	struct salsa64_gfx_backend off = { 0 }, on = { .eurocom = true };
	salsa64_rsp_set_gfx(bus, &off);
	CHECK_EQ(salsa64_rsp_detect_gbi(bus, &task), SALSA64_GBI_F3DEX2);
	salsa64_rsp_set_gfx(bus, &on);
	CHECK_EQ(salsa64_rsp_detect_gbi(bus, &task), SALSA64_GBI_F3DEX2EUROCOM);
	p[17] ^= 1;
	CHECK_EQ(salsa64_rsp_detect_gbi(bus, &task), SALSA64_GBI_F3DEX2);
	salsa64_rsp_set_gfx(bus, NULL);
	memset(p, 0, size);
}

/* Animal Crossing (Dōbutsu no Mori): dos microcódigos sin texto, que se reconocen por el CRC-32 de su
 * código (en palabras del host, de la lista de otros emuladores): el F3DZEX2 trae la bandera de «sin
 * recorte contra el plano cercano» (el .NoN que un texto diría) y el S2DEX2 no. Código fabricado para
 * que dé ese CRC. Y lo que el log de la PS3 imprime de un microcódigo desconocido
 * (salsa64_rsp_ucode_ident): los CRC de los bytes y de las palabras del host, y el de los datos. */
static void rsp_detect_animal_crossing(void)
{
	struct salsa64_bus *bus = &t_n64.bus;
	const uint32_t code = 0x100000, size = 0x1000;  /* donde put_task dice que está el código */
	uint8_t *p = bus->rdram + code;
	static const struct { uint32_t crc; enum salsa64_gbi gbi; bool nonear; } cases[] = {
		{ 0xD39A0D4Fu, SALSA64_GBI_F3DZEX2, true }, { 0x02C399DDu, SALSA64_GBI_S2DEX, false },
	};
	uint32_t state = 0xFFFFFFFFu;
	for (uint32_t i = 0; i < size - 4; i++)
		for (int k = 0; k < 8; k++)
			state = (state >> 1) ^ (0xEDB88320u & (0u - (state & 1)));
	salsa64_rsp_set_gfx(bus, &fake_backend);
	for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
		uint8_t tail[4];
		memset(p, 0, size);
		crc32_force(state, cases[c].crc, tail);
		for (int i = 0; i < 4; i++)
			p[size - 4 + (3 - i)] = tail[i];
		memset(bus->rdram + 0x5000, 0, 0x800);  /* los datos: sin texto de versión */
		put_task(SALSA64_TASK_GFX, 0x5000, 0x6000);
		struct salsa64_task task;
		salsa64_rsp_read_task(bus, &task);
		CHECK_EQ(salsa64_rsp_detect_gbi(bus, &task), cases[c].gbi);
		uint32_t cb, cw, cd;
		CHECK(salsa64_rsp_ucode_ident(bus, task.ucode, task.ucode_size, task.ucode_data, task.ucode_data_size, &cb, &cw,
					      &cd));
		CHECK_EQ(cw, cases[c].crc);
		CHECK_EQ(cb, salsa64_crc32(p, size));
	}
	/* El F3DZEX2 trae la bandera del plano cercano en la tarea que arranca. */
	memset(p, 0, size);
	{
		uint8_t tail[4];
		crc32_force(state, cases[0].crc, tail);
		for (int i = 0; i < 4; i++)
			p[size - 4 + (3 - i)] = tail[i];
		memset(&fake, 0, sizeof fake);
		memset(bus->rdram + 0x5000, 0, 0x800);
		put_task(SALSA64_TASK_GFX, 0x5000, 0x6000);
		wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4);
		CHECK_EQ(fake.starts, 1);
		CHECK_EQ(fake.gbi, SALSA64_GBI_F3DZEX2);
		struct gfx_job job;
		salsa64_gfx_job(&job, bus, &fake.task, fake.gbi);
		CHECK((job.flags & GFX_JOB_NONEAR) != 0);
		salsa64_sched_dispatch(&t_n64.sched, CPU->cycles + bus->rsp.gfx_cycles + bus->rsp.dp_cycles);
		wr(SP_REG(SALSA64_SP_STATUS), 0x8);
		bus->mi.intr = 0;
	}
	uint32_t cb, cw, cd;
	CHECK(!salsa64_rsp_ucode_ident(bus, 0x7FFFFFF0u, 0x1000, 0, 0, &cb, &cw, &cd));
	salsa64_rsp_set_gfx(bus, NULL);
	memset(p, 0, size);
}

/* Conker (#68): un mismo código con ocho segmentos de datos («F3DEX», «F3DEXBS», «F3DEXBG» y «F3DEXBGBS», con y
 * sin «.NoN»); desde el ascensor del final de Heist el juego usa «F3DEX.NoN», que por el texto parecía un F3DEX2.
 * Por el CRC de su código (palabras del host) todos son F3DEXBG, con el plano cercano según su nombre; con el
 * interruptor conker-ucode-viejo, solo el texto, como antes. Código fabricado para que dé ese CRC y textos con el
 * formato de los suyos (sin copiar sus datos). */
static void rsp_detect_gbi_conker(void)
{
	struct salsa64_bus *bus = &t_n64.bus;
	const uint32_t code = 0x100000, size = 0x1000;  /* donde put_task dice que está el código */
	uint8_t *p = bus->rdram + code, tail[4];
	uint32_t state = 0xFFFFFFFFu;
	for (uint32_t i = 0; i < size - 4; i++)
		for (int k = 0; k < 8; k++)
			state = (state >> 1) ^ (0xEDB88320u & (0u - (state & 1)));
	memset(p, 0, size);
	crc32_force(state, 0x1B4ACE88u, tail);
	for (int i = 0; i < 4; i++)
		p[size - 4 + (3 - i)] = tail[i];
	CHECK_EQ(salsa64_crc32_words_swapped(p, size), 0x1B4ACE88u);

	static const struct {
		const char *text;
		bool nonear;
		enum salsa64_gbi old;  /* con conker-ucode-viejo */
	} cases[] = {
		{ "RSP Gfx ucode F3DEX.NoN   fifo 2.08 ", true, SALSA64_GBI_F3DEX2 },
		{ "RSP Gfx ucode F3DEX       fifo 2.08 ", false, SALSA64_GBI_F3DEX2 },
		{ "RSP Gfx ucode F3DEXBS.NoN fifo 2.08 ", true, SALSA64_GBI_F3DEX2 },
		{ "RSP Gfx ucode F3DEXBS     fifo 2.08 ", false, SALSA64_GBI_F3DEX2 },
		{ "RSP Gfx ucode F3DEXBG.NoN fifo 2.08 ", true, SALSA64_GBI_F3DEXBG },
		{ "RSP Gfx ucode F3DEXBG     fifo 2.08 ", false, SALSA64_GBI_F3DEXBG },
		{ "RSP Gfx ucode F3DEXBGBS.Nofifo 2.08 ", true, SALSA64_GBI_F3DEXBG },
		{ "RSP Gfx ucode F3DEXBGBS   fifo 2.08 ", false, SALSA64_GBI_F3DEXBG },
	};
	struct salsa64_gfx_backend on = fake_backend, old = fake_backend;
	old.conker_text_only = true;
	for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
		memset(bus->rdram + 0x5000, 0, 0x800);
		put_text(0x5040, cases[c].text);
		put_task(SALSA64_TASK_GFX, 0x5000, 0x6000);
		struct salsa64_task task;
		salsa64_rsp_read_task(bus, &task);
		salsa64_rsp_set_gfx(bus, &on);
		CHECK_EQ(salsa64_rsp_detect_gbi(bus, &task), SALSA64_GBI_F3DEXBG);
		/* El plano cercano llega en la tarea que arranca. */
		memset(&fake, 0, sizeof fake);
		wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4);
		CHECK_EQ(fake.starts, 1);
		CHECK_EQ(fake.gbi, SALSA64_GBI_F3DEXBG);
		struct gfx_job job;
		salsa64_gfx_job(&job, bus, &fake.task, fake.gbi);
		CHECK_EQ((job.flags & GFX_JOB_NONEAR) != 0, cases[c].nonear);
		salsa64_sched_dispatch(&t_n64.sched, CPU->cycles + bus->rsp.gfx_cycles + bus->rsp.dp_cycles);
		wr(SP_REG(SALSA64_SP_STATUS), 0x8);
		bus->mi.intr = 0;
		salsa64_rsp_set_gfx(bus, &old);
		CHECK_EQ(salsa64_rsp_detect_gbi(bus, &task), cases[c].old);
	}
	/* Otro código con el mismo texto: lo de siempre. */
	p[17] ^= 1;
	salsa64_rsp_set_gfx(bus, &on);
	struct salsa64_task task;
	salsa64_rsp_read_task(bus, &task);
	CHECK_EQ(salsa64_rsp_detect_gbi(bus, &task), SALSA64_GBI_F3DEXBG);  /* el último texto: «F3DEXBGBS» */
	memset(bus->rdram + 0x5000, 0, 0x800);
	put_text(0x5040, cases[0].text);
	CHECK_EQ(salsa64_rsp_detect_gbi(bus, &task), SALSA64_GBI_F3DEX2);
	salsa64_rsp_set_gfx(bus, NULL);
	memset(p, 0, size);
	memset(bus->rdram + 0x5000, 0, 0x800);
}

static void rsp_detect_gbi(void)
{
	/* Solo el formato de los textos de identificación, sin copiarlos. */
	CHECK_EQ(detect("RSP Gfx ucode F3DEX       fifo 2.08 "), SALSA64_GBI_F3DEX2);
	CHECK_EQ(detect("RSP Gfx ucode F3DZEX.NoN  fifo 2.06H "), SALSA64_GBI_F3DZEX2);
	CHECK_EQ(detect("RSP Gfx ucode F3DEX       xbus 1.23 "), SALSA64_GBI_F3DEX);
	CHECK_EQ(detect("RSP Gfx ucode F3DEX         0.95 "), SALSA64_GBI_F3DEX);
	CHECK_EQ(detect("RSP Gfx ucode S2DEX  fifo 2.05 "), SALSA64_GBI_S2DEX);
	CHECK_EQ(detect("RSP Gfx ucode S2DEX       fifo 2.06 "), SALSA64_GBI_S2DEX);
	CHECK_EQ(detect("RSP Gfx ucode S2DEX  1.06 "), SALSA64_GBI_S2DEX1);
	CHECK_EQ(detect("RSP Gfx ucode F3DEXBG.NoN fifo 2.08 "), SALSA64_GBI_F3DEXBG);
	CHECK_EQ(detect("RSP SW Version: 2.0X"), SALSA64_GBI_F3D);
	CHECK_EQ(detect("Salsa64 GBI F3DEX2"), SALSA64_GBI_F3DEX2);
	CHECK_EQ(detect("Salsa64 GBI F3DEX1"), SALSA64_GBI_F3DEX);
	CHECK_EQ(detect("Salsa64 GBI Fast3D"), SALSA64_GBI_F3D);
	CHECK_EQ(detect("Salsa64 GBI F3DPD"), SALSA64_GBI_F3DPD);
	CHECK_EQ(detect("Salsa64 GBI F3DEXBG"), SALSA64_GBI_F3DEXBG);
	CHECK_EQ(detect("nada"), SALSA64_GBI_UNKNOWN);

	/* El texto para los logs: hasta el primer carácter no imprimible. */
	char text[32];
	detect("RSP SW Version: 2.0X, 01-01-96");
	salsa64_rsp_ucode_text(&t_n64.bus, 0x80005000, text, sizeof text);
	CHECK(strcmp(text, "RSP SW Version: 2.0X, 01-01-96") == 0);
	salsa64_rsp_ucode_text(&t_n64.bus, 0x80005000, text, 8);
	CHECK(strcmp(text, "RSP SW ") == 0);
	detect("nada");
	salsa64_rsp_ucode_text(&t_n64.bus, 0x80005000, text, sizeof text);
	CHECK_EQ(text[0], 0);
}

/* Listas del RDP que el CPU escribe por DPC_START/DPC_END, sin RSP (Killer
 * Instinct Gold las usa para sus rectángulos y espera la interrupción del DP
 * de su SYNC_FULL antes de mandar la primera tarea gráfica): las consume el
 * backend del RDP y su SYNC_FULL levanta la interrupción del DP. Sin backend
 * la lista solo se descarta y la interrupción nunca llega; por eso el
 * frontend de la PS3 lo conecta siempre, no solo con el RSP a bajo nivel. */
static struct {
	int runs;
	uint32_t start, end;
	const uint8_t *dmem;
} dpc_fake;

static uint32_t dpc_fake_run(void *ctx, uint32_t start, uint32_t end, const uint8_t *dmem)
{
	(void)ctx;
	dpc_fake.runs++;
	dpc_fake.start = start;
	dpc_fake.end = end;
	dpc_fake.dmem = dmem;
	return SALSA64_GFX_FULLSYNC;
}

static void rsp_dpc_from_cpu(void)
{
	struct salsa64_bus *bus = &t_n64.bus;
	static const struct salsa64_dpc_backend dpc = { NULL, dpc_fake_run };
#define DP_REG(r) (0x04100000u + 4 * (r))
	memset(&dpc_fake, 0, sizeof dpc_fake);
	wr(0x04300000, 0x0800);  /* MI_MODE: bajar la interrupción DP */

	/* Sin backend: el tramo se descarta y no llega ninguna interrupción. */
	salsa64_rsp_set_dpc(bus, NULL);
	wr(DP_REG(SALSA64_DP_START), 0x2000);
	wr(DP_REG(SALSA64_DP_END), 0x2040);
	CHECK_EQ(rd(DP_REG(SALSA64_DP_CURRENT)), 0x2040);
	salsa64_sched_dispatch(&t_n64.sched, CPU->cycles + 10 * bus->rsp.dp_cycles);
	CHECK_EQ(bus->mi.intr & (1u << SALSA64_IRQ_DP), 0);
	CHECK_EQ(dpc_fake.runs, 0);

	/* Con backend: lo consume (de la RDRAM, sin xbus) y su SYNC_FULL
	 * levanta la interrupción dp_cycles después. */
	salsa64_rsp_set_dpc(bus, &dpc);
	wr(DP_REG(SALSA64_DP_START), 0x3000);
	wr(DP_REG(SALSA64_DP_END), 0x3038);
	CHECK_EQ(dpc_fake.runs, 1);
	CHECK_EQ(dpc_fake.start, 0x3000);
	CHECK_EQ(dpc_fake.end, 0x3038);
	CHECK(dpc_fake.dmem == NULL);
	CHECK_EQ(bus->mi.intr & (1u << SALSA64_IRQ_DP), 0);
	salsa64_sched_dispatch(&t_n64.sched, CPU->cycles + bus->rsp.dp_cycles);
	CHECK(bus->mi.intr & (1u << SALSA64_IRQ_DP));
	salsa64_rsp_set_dpc(bus, NULL);
#undef DP_REG
}

/* Un comando del RDP que llega partido entre dos DPC_END (F3DEX 1.x avanza
 * DPC_END de a 8 bytes: un TEXRECT de 16 bytes son dos tramos) no se entrega
 * a medias: espera al resto y el backend lo recibe entero, por una imagen
 * propia (modo xbus) cuyo comando empieza en 0; lo que sigue se entrega por
 * su lado. Un tramo con un comando completo y la mitad de otro entrega solo
 * el primero. */
static struct {
	int runs;
	uint32_t len;
	uint8_t first[16];
	bool xbus;
} dpc_cut;

static uint32_t dpc_cut_run(void *ctx, uint32_t start, uint32_t end, const uint8_t *dmem)
{
	(void)ctx;
	dpc_cut.runs++;
	dpc_cut.len = end - start;
	dpc_cut.xbus = dmem != NULL;
	for (unsigned i = 0; i < 16; i++)
		dpc_cut.first[i] = dmem ? dmem[(start + i) & 0xFFF] : t_n64.bus.rdram[start + i];
	return 0;
}

static void rsp_dpc_split_command(void)
{
	struct salsa64_bus *bus = &t_n64.bus;
	static const struct salsa64_dpc_backend dpc = { NULL, dpc_cut_run };
#define DP_REG(r) (0x04100000u + 4 * (r))
	static const uint8_t texrect[16] = { 0xE4, 0x12, 0x34, 0x56, 0x00, 0x11, 0x22, 0x33,
					     0xAA, 0xBB, 0xCC, 0xDD, 0x04, 0x00, 0x04, 0x00 };
	memset(&dpc_cut, 0, sizeof dpc_cut);
	memcpy(bus->rdram + 0x5000, texrect, 16);
	memset(bus->rdram + 0x5010, 0, 16);  /* SYNC de 8 bytes: opcode 0x00 con los dos bits altos = 0xC0 → 1 palabra */
	bus->rsp.dp_carry_n = 0;
	salsa64_rsp_set_dpc(bus, &dpc);

	wr(DP_REG(SALSA64_DP_START), 0x5000);
	wr(DP_REG(SALSA64_DP_END), 0x5008);  /* la primera mitad del TEXRECT */
	CHECK_EQ(dpc_cut.runs, 0);
	wr(DP_REG(SALSA64_DP_END), 0x5010);  /* la segunda: ya está entero */
	CHECK_EQ(dpc_cut.runs, 1);
	CHECK_EQ(dpc_cut.len, 16);
	CHECK(dpc_cut.xbus);
	CHECK(!memcmp(dpc_cut.first, texrect, 16));

	/* Un comando completo y la mitad de otro: el primero ya, el resto después. */
	memcpy(bus->rdram + 0x5018, texrect, 16);
	wr(DP_REG(SALSA64_DP_END), 0x5020);  /* [0x5010, 0x5020): 8 bytes de relleno + la mitad del TEXRECT */
	CHECK_EQ(dpc_cut.runs, 2);
	CHECK_EQ(dpc_cut.len, 8);
	CHECK(!dpc_cut.xbus);
	wr(DP_REG(SALSA64_DP_END), 0x5028);
	CHECK_EQ(dpc_cut.runs, 3);
	CHECK_EQ(dpc_cut.len, 16);
	CHECK(!memcmp(dpc_cut.first, texrect, 16));
	CHECK_EQ(bus->rsp.dp_carry_n, 0);

	/* Un búfer nuevo (otro START) abandona lo que quedara a medias. */
	wr(DP_REG(SALSA64_DP_END), 0x5030);
	salsa64_rsp_set_dpc(bus, NULL);
#undef DP_REG
}

/* Dos SYNC_FULL cercanos (la lista del CPU y la tarea que le sigue) son dos
 * interrupciones del DP, cada una a su hora: la ranura del planificador es
 * una sola y la segunda borraba a la primera; el mismo SYNC_FULL pedido dos
 * veces en el mismo ciclo sigue siendo una. */
static void rsp_dp_interrupts_queue(void)
{
	struct salsa64_bus *bus = &t_n64.bus;
	bus->mi.intr = 0;
	uint64_t t0 = CPU->cycles;
	uint32_t base = bus->rsp.stats.dp_fired;
	uint32_t d = bus->rsp.dp_cycles;

	salsa64_rsp_dp_done(bus);
	salsa64_rsp_dp_done(bus);  /* el mismo: una sola */
	CPU->cycles = t0 + 3000;
	salsa64_rsp_dp_done(bus);  /* otro distinto, antes de que llegue el primero */
	CPU->cycles = t0 + 5000;
	salsa64_rsp_dp_done(bus);  /* y otro */

	salsa64_sched_dispatch(&t_n64.sched, t0 + d - 1);
	CHECK_EQ(bus->mi.intr & (1u << SALSA64_IRQ_DP), 0);
	salsa64_sched_dispatch(&t_n64.sched, t0 + d);
	CHECK(bus->mi.intr & (1u << SALSA64_IRQ_DP));
	CHECK_EQ(bus->rsp.stats.dp_fired, base + 1);
	salsa64_mi_clear(bus, SALSA64_IRQ_DP);  /* el juego la atiende */
	salsa64_sched_dispatch(&t_n64.sched, t0 + d + 2999);
	CHECK_EQ(bus->mi.intr & (1u << SALSA64_IRQ_DP), 0);
	salsa64_sched_dispatch(&t_n64.sched, t0 + d + 3000);
	CHECK(bus->mi.intr & (1u << SALSA64_IRQ_DP));
	salsa64_mi_clear(bus, SALSA64_IRQ_DP);
	salsa64_sched_dispatch(&t_n64.sched, t0 + d + 5000);
	CHECK(bus->mi.intr & (1u << SALSA64_IRQ_DP));
	salsa64_mi_clear(bus, SALSA64_IRQ_DP);
	CHECK_EQ(bus->rsp.stats.dp_fired, base + 3);
	salsa64_sched_dispatch(&t_n64.sched, t0 + 100 * d);
	CHECK_EQ(bus->mi.intr & (1u << SALSA64_IRQ_DP), 0);  /* no sobra ninguna */
	CPU->cycles = t0;
}

void t_suite_rsp(void)
{
	RUN(sp_dma_rows);
	RUN(rsp_gfx_task);
	RUN(rsp_gfx_task_polls);
	RUN(rsp_gfx_task_cut);
	RUN(rsp_gfx_model);
	RUN(rsp_gfx_model_cycles);
	RUN(rsp_audio_task);
	RUN(rsp_other_task);
	RUN(rsp_detect_gbi);
	RUN(rsp_gfx_nonear);
	RUN(rsp_detect_gbi_crc);
	RUN(rsp_detect_gbi_wr);
	RUN(rsp_detect_gbi_dkr);
	RUN(rsp_detect_gbi_acclaim);
	RUN(rsp_detect_gbi_eurocom);
	RUN(rsp_detect_gbi_conker);
	RUN(rsp_detect_animal_crossing);
	RUN(rsp_dpc_from_cpu);
	RUN(rsp_dpc_split_command);
	RUN(rsp_dp_interrupts_queue);
}
