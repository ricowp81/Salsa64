/*
 * Salsa64 — pruebas de COP0: registros, excepciones, ERET, Count/Compare,
 * interrupciones, TLB y restricciones de modo usuario.
 */
#include "cpu/rec.h"
#include "test.h"

#define KSU_USER 0x10u

#define tlb_set t_tlb_set
#define LO      T_LO

static void prid_and_config(void)
{
	T_LOAD(MFC0(T0, SALSA64_CP0_PRID), MFC0(T1, SALSA64_CP0_CONFIG));
	t_step(2);
	CHECK_EQ(R[T0], SALSA64_PRID_VALUE);
	CHECK_EQ(R[T1], 0x7006E463);
}

static void write_masks(void)
{
	R[T0] = 0xFFFFFFFF;
	R[T1] = 0xFFFFFFE0;  /* sin KSU, ERL, EXL, IE */
	T_LOAD(MTC0(T0, SALSA64_CP0_CAUSE), MFC0(T2, SALSA64_CP0_CAUSE),
	       MTC0(T1, SALSA64_CP0_STATUS), MFC0(T3, SALSA64_CP0_STATUS));
	t_step(4);
	CHECK_EQ(R[T2], 0x300);
	CHECK_EQ(R[T3], 0xFFFFFFFFFF57FFE0ull);
}

/* MTC0 transfiere los 64 bits del GPR, como DMTC0 (nemu64-test). */
static void mtc0_writes_64_bits(void)
{
	R[T0] = 0xFFFFFFFF80001234ull;
	R[T1] = 0x0000001234567890ull;
	T_LOAD(MTC0(T0, SALSA64_CP0_EPC), DMFC0(T2, SALSA64_CP0_EPC),
	       MTC0(T1, SALSA64_CP0_EPC), DMFC0(T3, SALSA64_CP0_EPC), MFC0(T4, SALSA64_CP0_EPC));
	t_step(5);
	CHECK_EQ(R[T2], 0xFFFFFFFF80001234ull);
	CHECK_EQ(R[T3], 0x0000001234567890ull);
	CHECK_EQ(R[T4], 0x34567890);
}

static void syscall_and_eret(void)
{
	t_wr32(0x180, ERET);
	T_LOAD(SYSCALL, ADDIU(T0, ZERO, 1));
	t_step(1);
	CHECK_EXC(SALSA64_EXC_SYS, T_CODE, 0, T_VEC_GEN);
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_EPC, T_CODE + 4);  /* el manejador salta el SYSCALL */
	t_step(2);
	CHECK_EQ(CPU->status & SALSA64_ST_EXL, 0);
	CHECK_EQ(R[T0], 1);
}

static void break_exception(void)
{
	T_LOAD(NOP, BREAK);
	t_step(2);
	CHECK_EXC(SALSA64_EXC_BP, T_CODE + 4, 0, T_VEC_GEN);
}

static void bev_vectors(void)
{
	t_set_status(T_STATUS | SALSA64_ST_BEV);
	T_LOAD(SYSCALL);
	t_step(1);
	CHECK_EXC(SALSA64_EXC_SYS, T_CODE, 0, 0xFFFFFFFFBFC00380ull);
}

static void nested_exception_keeps_epc(void)
{
	t_set_status(T_STATUS | SALSA64_ST_EXL);
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_EPC, 0x1234);
	T_LOAD(SYSCALL);
	t_step(1);
	CHECK_EXC(SALSA64_EXC_SYS, 0x1234, 0, T_VEC_GEN);
}

static void reserved_instruction(void)
{
	T_LOAD(0x70000000u);  /* opcode 0x1C */
	t_step(1);
	CHECK_EXC(SALSA64_EXC_RI, T_CODE, 0, T_VEC_GEN);
}

static void cop2_unusable(void)
{
	T_LOAD(0x48000000u);  /* COP2 */
	t_step(1);
	CHECK_EXC(SALSA64_EXC_CPU, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ((CPU->cause >> SALSA64_CAUSE_CE_SHIFT) & 3, 2);
}

static void traps(void)
{
	R[T0] = R[T1] = 5;
	T_LOAD(TNE(T0, T1), TGE(T0, T1));
	t_step(2);
	CHECK_EXC(SALSA64_EXC_TR, T_CODE + 4, 0, T_VEC_GEN);
}

static void trap_immediate(void)
{
	R[T0] = (uint64_t)-3;
	T_LOAD(TEQI(T0, -3));
	t_step(1);
	CHECK_EXC(SALSA64_EXC_TR, T_CODE, 0, T_VEC_GEN);
}

static void misaligned_fetch(void)
{
	R[T0] = T_CODE + 0x22;
	T_LOAD(JR(T0), NOP);
	t_step(3);
	CHECK_EXC(SALSA64_EXC_ADEL, T_CODE + 0x22, 0, T_VEC_GEN);
	CHECK_EQ(CPU->bad_vaddr, T_CODE + 0x22);
}

static void count_half_rate(void)
{
	T_LOAD(MFC0(T0, SALSA64_CP0_COUNT));
	t_wr32(T_CODE_PA + 80, MFC0(T1, SALSA64_CP0_COUNT));  /* 20 instrucciones después */
	t_step(21);
	CHECK_EQ((uint32_t)(R[T1] - R[T0]), 10);
}

/* ---- Barrido de la caché de datos (salsa64_cpu_set_dcache_sweep) ---------- */

#define CACHE(op, base, off) M_I(0x2F, base, op, off)
#define DC_IWBINV 0x01  /* Index_Writeback_Invalidate, caché de datos */
#define DC_HWB    0x19  /* Hit_Writeback, caché de datos */

/* Solo CACHE 0x01 sobre la línea 0 (cualquier dirección con los bits 12–4
 * en 0) empieza un barrido y cobra SALSA64_DCACHE_SWEEP_CYCLES; las otras
 * líneas y las otras operaciones cuestan un ciclo, como antes. */
static void dcache_sweep_cost(void)
{
	CPU->dcache_sweeps = 0;
	R[T0] = 0xFFFFFFFF80000000ull;
	T_LOAD(CACHE(DC_IWBINV, T0, 16), CACHE(DC_HWB, T0, 0), CACHE(DC_IWBINV, T0, 0x2000), ADDIU(T1, ZERO, 7));
	uint64_t c0 = CPU->cycles;
	t_step(1);
	t_step(1);
	CHECK_EQ(CPU->cycles - c0, 2);
	CHECK_EQ(CPU->dcache_sweeps, 0);
	t_step(1);
	CHECK_EQ(CPU->cycles - c0, 3 + SALSA64_DCACHE_SWEEP_CYCLES);
	CHECK_EQ(CPU->dcache_sweeps, 1);
	CHECK_EQ(CPU->pc, T_CODE + 12);
	t_step(1);
	CHECK_EQ(R[T1], 7);
}

/* osWritebackDCacheAll de libultra: las 512 líneas desde 0x80000000. Un
 * barrido, 2052 instrucciones y su costo una sola vez; con el recompilador,
 * t_step cuenta ciclos y tiene que acabar justo después del bucle. */
static void dcache_sweep_loop(void)
{
	CPU->dcache_sweeps = 0;
	T_LOAD(LUI(T0, 0x8000), ADDIU(T2, ZERO, 0x2000), ADDU(T1, T0, T2), ADDIU(T1, T1, -16),
	       CACHE(DC_IWBINV, T0, 0), SLTU(AT, T0, T1), BNE(AT, ZERO, -3), ADDIU(T0, T0, 16),
	       ADDIU(T3, ZERO, 1));
	uint64_t c0 = CPU->cycles;
	t_step(CPU->rec ? 2052 + SALSA64_DCACHE_SWEEP_CYCLES : 2052);
	CHECK_EQ(CPU->pc, T_CODE + 32);
	CHECK_EQ(CPU->cycles - c0, 2052 + SALSA64_DCACHE_SWEEP_CYCLES);
	CHECK_EQ(CPU->dcache_sweeps, 1);
	CHECK_EQ(R[T0], 0xFFFFFFFF80002000ull);
	t_step(1);
	CHECK_EQ(R[T3], 1);
}

/* En el delay slot de un salto tomado: el costo y después el destino. Count
 * avanza con los ciclos cobrados. */
static void dcache_sweep_delay_slot(void)
{
	CPU->dcache_sweeps = 0;
	R[T0] = 0xFFFFFFFF80004000ull;
	T_LOAD(BEQ(ZERO, ZERO, 2), CACHE(DC_IWBINV, T0, 0), ADDIU(T1, ZERO, 1), ADDIU(T2, ZERO, 2));
	uint64_t c0 = CPU->cycles;
	uint32_t count0 = salsa64_cpu_count(CPU);
	t_step(CPU->rec ? 2 + SALSA64_DCACHE_SWEEP_CYCLES : 2);
	CHECK_EQ(CPU->cycles - c0, 2 + SALSA64_DCACHE_SWEEP_CYCLES);
	CHECK_EQ(CPU->pc, T_CODE + 12);
	CHECK_EQ(CPU->dcache_sweeps, 1);
	CHECK(salsa64_cpu_count(CPU) - count0 >= SALSA64_DCACHE_SWEEP_CYCLES / 2);
	t_step(1);
	CHECK_EQ(R[T1], 0);
	CHECK_EQ(R[T2], 2);
}

/* sin-barrido-cache: CACHE vuelve a ser gratis. */
static void dcache_sweep_off(void)
{
	salsa64_cpu_set_dcache_sweep(CPU, false);
	CPU->dcache_sweeps = 0;
	R[T0] = 0xFFFFFFFF80000000ull;
	T_LOAD(CACHE(DC_IWBINV, T0, 0), ADDIU(T1, ZERO, 3));
	uint64_t c0 = CPU->cycles;
	t_step(2);
	CHECK_EQ(CPU->cycles - c0, 2);
	CHECK_EQ(CPU->dcache_sweeps, 0);
	CHECK_EQ(R[T1], 3);
	salsa64_cpu_set_dcache_sweep(CPU, true);
}

/* MFC0 y DMFC0 de cada registro (Random aparte): lo mismo que
 * salsa64_cpu_cop0_read, con signo desde 32 bits y en 64. Los campos llevan
 * valores con la mitad alta y el bit 31 distintos. */
static void mfc0_reads_every_register(void)
{
	CPU->index = 0x8000001F;
	CPU->entry_lo0 = 0x83FFFFFF;
	CPU->entry_lo1 = 0x0000FFFF;
	CPU->context = 0xFFFFFFFF807FFFF0ull;
	CPU->page_mask = 0x01FFE000;
	CPU->wired = 5;
	CPU->bad_vaddr = 0x0000000180000004ull;
	CPU->entry_hi = 0xC00000FFFFFFE0FFull;
	CPU->compare = 0x87654321;
	CPU->cause = 0x8000FF7C;  /* sin IE no hay interrupción */
	CPU->epc = 0x00000000DEADBEEFull;
	CPU->config = 0x7006E463;
	CPU->lladdr = 0x89ABCDEF;
	CPU->watch_lo = 0xFFFFFFFB;
	CPU->watch_hi = 0xF;
	CPU->xcontext = 0x123456789ABCDEF0ull;
	CPU->parity_error = 0xFF;
	CPU->cache_error = 0x80000000;
	CPU->tag_lo = 0xF0F0F0F0;
	CPU->error_epc = 0xFFFFFFFF80001234ull;
	CPU->cop0_latch = 0xFEDCBA9876543210ull;
	CPU->count_offset = 0xFFFFFF00;

	uint64_t calls = CPU->rec ? CPU->rec->stats.exec_calls : 0;
	for (unsigned rd = 0; rd < 32; rd++) {
		if (rd == SALSA64_CP0_RANDOM)
			continue;
		R[T1] = R[T2] = 0x5555;
		t_wr32(T_CODE_PA, MFC0(T1, rd));
		t_wr32(T_CODE_PA + 4, DMFC0(T2, rd));
		salsa64_cpu_set_pc(CPU, T_CODE);
		uint64_t c0 = CPU->cycles;
		t_step(2);
		uint64_t want = salsa64_cpu_cop0_read(CPU, rd), want64 = want;
		if (rd == SALSA64_CP0_COUNT) {
			want = (uint32_t)((c0 + 1) >> 1) + CPU->count_offset;
			want64 = (uint32_t)((uint32_t)((c0 + 2) >> 1) + CPU->count_offset);
		}
		CHECK_EQ(R[T1], (uint64_t)(int64_t)(int32_t)(uint32_t)want);
		CHECK_EQ(R[T2], want64);
	}
	if (CPU->rec && !(CPU->rec->opts & SALSA64_REC_OPT_RARE_EXEC))  /* esa opción las manda al intérprete */
		CHECK_EQ(CPU->rec->stats.exec_calls, calls);
}

/* Count leído varias veces en un mismo bloque (el recompilador ya no lo
 * termina en MFC0), desde un ciclo par y uno impar, y en un delay slot. */
static void mfc0_count_inside_block(void)
{
	for (unsigned odd = 0; odd < 2; odd++) {
		T_LOAD(NOP, MFC0(T1, SALSA64_CP0_COUNT), NOP, NOP, MFC0(T2, SALSA64_CP0_COUNT),
		       ADDIU(T3, T3, 1), MFC0(T3, SALSA64_CP0_COUNT), BEQ(ZERO, ZERO, 2),
		       MFC0(T4, SALSA64_CP0_COUNT), NOP, ADDIU(T5, ZERO, 1));
		salsa64_cpu_set_pc(CPU, T_CODE + 4 * (1 - odd));
		uint64_t c0 = CPU->cycles - (1 - odd);  /* como si hubiera corrido el NOP */
		t_step(10 - (1 - odd));
		uint32_t off = CPU->count_offset;
		CHECK_EQ(R[T1], (uint64_t)(int64_t)(int32_t)((uint32_t)((c0 + 2) >> 1) + off));
		CHECK_EQ(R[T2], (uint64_t)(int64_t)(int32_t)((uint32_t)((c0 + 5) >> 1) + off));
		CHECK_EQ(R[T3], (uint64_t)(int64_t)(int32_t)((uint32_t)((c0 + 7) >> 1) + off));
		CHECK_EQ(R[T4], (uint64_t)(int64_t)(int32_t)((uint32_t)((c0 + 9) >> 1) + off));
		CHECK_EQ(R[T5], 1);
		R[T5] = 0;
	}
}

static void count_write(void)
{
	R[T0] = 0x12345;
	T_LOAD(MTC0(T0, SALSA64_CP0_COUNT), MFC0(T1, SALSA64_CP0_COUNT));
	t_step(2);
	CHECK(R[T1] - 0x12345 <= 1);
}

static void compare_interrupt(void)
{
	t_set_status(T_STATUS | SALSA64_ST_IE | 0x8000);  /* IM7 */
	T_LOAD(MFC0(T1, SALSA64_CP0_COUNT), ADDIU(T1, T1, 10), MTC0(T1, SALSA64_CP0_COMPARE));
	t_step(17);  /* 16 ciclos tras leer Count: todavía no */
	CHECK_EQ(CPU->status & SALSA64_ST_EXL, 0);
	CHECK_EQ(CPU->cause & 0x8000, 0);

	unsigned k = 0;
	while (k < 10 && !(CPU->status & SALSA64_ST_EXL)) {
		t_step(1);
		k++;
	}
	CHECK(k >= 3 && k <= 5);
	CHECK(CPU->cause & 0x8000);
	CHECK_EQ(SALSA64_CAUSE_EXC(CPU->cause), SALSA64_EXC_INT);
	CHECK_EQ(CPU->pc, T_VEC_GEN);

	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_COMPARE, 0);
	CHECK_EQ(CPU->cause & 0x8000, 0);
}

static void software_interrupt(void)
{
	t_set_status(T_STATUS | SALSA64_ST_IE | 0x100);  /* IM0 */
	R[T0] = 0x100;
	T_LOAD(MTC0(T0, SALSA64_CP0_CAUSE), NOP);
	t_step(2);
	CHECK_EXC(SALSA64_EXC_INT, T_CODE + 4, 0, T_VEC_GEN);
}

static void interrupt_blocked_by_exl_and_ie(void)
{
	R[T0] = 0x100;
	t_set_status(T_STATUS | SALSA64_ST_EXL | SALSA64_ST_IE | 0x100);
	T_LOAD(MTC0(T0, SALSA64_CP0_CAUSE), NOP, NOP);
	t_step(2);
	CHECK_EQ(CPU->pc, T_CODE + 8);
	t_set_status(T_STATUS | 0x100);  /* sin IE */
	t_step(1);
	CHECK_EQ(CPU->pc, T_CODE + 12);
}

static void mi_interrupt_on_ip2(void)
{
	struct salsa64_bus *bus = &t_n64.bus;
	salsa64_bus_write32(bus, 0x0430000C, 0x800, ~0u);  /* máscara DP */
	t_set_status(T_STATUS | SALSA64_ST_IE | 0x400);
	salsa64_mi_raise(bus, SALSA64_IRQ_DP);
	CHECK(CPU->cause & 0x400);
	T_LOAD(NOP);
	t_step(1);
	CHECK_EXC(SALSA64_EXC_INT, T_CODE, 0, T_VEC_GEN);
	salsa64_bus_write32(bus, 0x04300000, 0x800, ~0u);  /* MI_MODE: limpiar DP */
	CHECK_EQ(CPU->cause & 0x400, 0);
}

/* MTC0 a Status como en __osDisableInt y __osRestoreInt (el recompilador lo
 * hace en línea si solo cambian IE, EXL o IM y no queda una interrupción
 * pendiente): con una pendiente, la toma después del MTC0; sin ella, sigue;
 * y apagar CU1 hace que el COP1 siguiente lance la excepción. Muchos ciclos,
 * para que los bloques corran en nativo con el recompilador. */
static void status_write_enables_pending_irq(void)
{
	t_set_status(T_STATUS | 0x100);  /* IM0, sin IE */
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_CAUSE, 0x100);
	t_wr32(0x180, ADDIU(T2, 0, 9));
	t_wr32(0x184, BEQ(0, 0, 0xFFFF));
	t_wr32(0x188, NOP);
	T_LOAD(MFC0(T0, SALSA64_CP0_STATUS), ORI(T0, T0, 1), MTC0(T0, SALSA64_CP0_STATUS),
	       ADDIU(T1, 0, 5), BEQ(0, 0, 0xFFFF), NOP);
	t_step(400);
	CHECK_EQ(R[T2], 9);
	CHECK_EQ(R[T1], 0);
	CHECK_EQ(SALSA64_CAUSE_EXC(CPU->cause), SALSA64_EXC_INT);
	CHECK_EQ(CPU->epc, T_CODE + 12);
}

static void status_write_ie_without_irq(void)
{
	uint32_t st = T_STATUS | SALSA64_ST_IE | 0x8400;
	t_set_status(st);
	T_LOAD(MFC0(T0, SALSA64_CP0_STATUS), ADDIU(1, 0, 0xFFFE), AND(T1, T0, 1), MTC0(T1, SALSA64_CP0_STATUS),
	       MFC0(T2, SALSA64_CP0_STATUS), MTC0(T0, SALSA64_CP0_STATUS), ADDIU(T3, 0, 5),
	       BEQ(0, 0, 0xFFFF), NOP);
	t_step(400);
	CHECK_EQ(R[T3], 5);
	CHECK_EQ(CPU->status, st);
	CHECK_EQ(R[T2], (uint64_t)(int32_t)(st & ~SALSA64_ST_IE));
	CHECK_EQ(CPU->cop0_latch, R[T0]);
	CHECK_EQ(CPU->status & SALSA64_ST_EXL, 0);
}

static void status_write_cu1_off_then_cop1(void)
{
	t_set_status(T_STATUS);
	t_wr32(0x180, BEQ(0, 0, 0xFFFF));
	t_wr32(0x184, NOP);
	T_LOAD(MFC0(T0, SALSA64_CP0_STATUS), LUI(1, 0xDFFF), ORI(1, 1, 0xFFFF), AND(T1, T0, 1),
	       MTC0(T1, SALSA64_CP0_STATUS), MFC1(T2, 0), ADDIU(T3, 0, 5), BEQ(0, 0, 0xFFFF), NOP);
	t_step(400);
	CHECK_EQ(R[T3], 0);
	CHECK_EQ(SALSA64_CAUSE_EXC(CPU->cause), SALSA64_EXC_CPU);
	CHECK_EQ((CPU->cause >> SALSA64_CAUSE_CE_SHIFT) & 3, 1);
	CHECK_EQ(CPU->epc, T_CODE + 20);
}

static void eret_from_error_level(void)
{
	t_set_status(T_STATUS | SALSA64_ST_ERL);
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_ERROREPC, T_CODE + 0x20);
	T_LOAD(ERET);
	t_step(1);
	CHECK_EQ(CPU->pc, T_CODE + 0x20);
	CHECK_EQ(CPU->status & SALSA64_ST_ERL, 0);
}

static void kuseg_unmapped_with_erl(void)
{
	t_wr32(T_DATA_PA, 0xABCD1234);
	t_set_status(T_STATUS | SALSA64_ST_ERL);
	R[T0] = T_DATA_PA;
	T_LOAD(LW(T1, 0, T0));
	t_step(1);
	CHECK_EQ(R[T1], 0xFFFFFFFFABCD1234ull);
}

static void tlb_mapped_access(void)
{
	t_wr32(0x10010, 0xAAAA5555);
	t_wr32(0x11010, 0x12345678);
	R[T0] = 0;
	R[T1] = LO(0x10, 1, 1, 1);
	R[T2] = LO(0x11, 1, 1, 1);
	R[T3] = 5;
	R[T7] = 0x77;
	T_LOAD(MTC0(T0, SALSA64_CP0_ENTRYHI), MTC0(T1, SALSA64_CP0_ENTRYLO0),
	       MTC0(T2, SALSA64_CP0_ENTRYLO1), MTC0(ZERO, SALSA64_CP0_PAGEMASK),
	       MTC0(T3, SALSA64_CP0_INDEX), TLBWI,
	       LW(T4, 0x10, ZERO), LW(T5, 0x1010, ZERO), SW(T7, 0x14, ZERO),
	       TLBP, MFC0(T6, SALSA64_CP0_INDEX));
	t_step(11);
	CHECK_EQ(R[T4], 0xFFFFFFFFAAAA5555ull);
	CHECK_EQ(R[T5], 0x12345678);
	CHECK_EQ(t_rd32(0x10014), 0x77);
	CHECK_EQ(R[T6], 5);
}

static void tlb_read_back(void)
{
	tlb_set(7, 0x00402000 | 0x42, LO(0x20, 1, 1, 1), LO(0x21, 0, 1, 1), 0x6000);
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_ENTRYHI, 0);
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_ENTRYLO0, 0);
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_PAGEMASK, 0);
	R[T0] = 7;
	T_LOAD(MTC0(T0, SALSA64_CP0_INDEX), TLBR, MFC0(T1, SALSA64_CP0_ENTRYHI),
	       MFC0(T2, SALSA64_CP0_ENTRYLO0), MFC0(T3, SALSA64_CP0_ENTRYLO1),
	       MFC0(T4, SALSA64_CP0_PAGEMASK));
	t_step(6);
	CHECK_EQ(R[T1], 0x00400042);  /* VPN2 enmascarado por la página de 16 KB */
	CHECK_EQ(R[T2], LO(0x20, 1, 1, 1));
	CHECK_EQ(R[T3], LO(0x21, 0, 1, 1));
	CHECK_EQ(R[T4], 0x6000);
}

static void tlb_probe_miss(void)
{
	R[T0] = 0x7FFFE000;
	T_LOAD(MTC0(T0, SALSA64_CP0_ENTRYHI), TLBP, MFC0(T1, SALSA64_CP0_INDEX));
	t_step(3);
	CHECK_EQ(R[T1], 0xFFFFFFFF80000000ull);
}

static void tlb_large_pages(void)
{
	/* Páginas de 16 KB: el par cubre 0x10000–0x17FFF. */
	tlb_set(1, 0x00010000, LO(0x40, 1, 1, 1), LO(0x50, 1, 1, 1), 0x6000);
	t_wr32(0x41234, 0x01010101);
	t_wr32(0x50010, 0x02020202);
	R[T0] = 0x00010000;
	T_LOAD(LW(T1, 0x1234, T0), LUI(T3, 1), ORI(T3, T3, 0x4010), LW(T2, 0, T3));
	t_step(4);
	CHECK_EQ(R[T1], 0x01010101);
	CHECK_EQ(R[T2], 0x02020202);
}

static void tlb_refill_on_miss(void)
{
	R[T0] = 0x00400120;
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_ENTRYHI, 0x33);
	T_LOAD(LW(T1, 0, T0));
	t_step(1);
	CHECK_EXC(SALSA64_EXC_TLBL, T_CODE, 0, T_VEC_TLB);
	CHECK_EQ(CPU->bad_vaddr, 0x00400120);
	CHECK_EQ(CPU->context & 0x7FFFF0, (0x00400120 >> 13) << 4);
	CHECK_EQ(CPU->entry_hi, 0x00400033);  /* VPN2 de la falla, ASID intacto */
}

static void tlb_refill_store(void)
{
	R[T0] = 0x00400120;
	T_LOAD(SW(T1, 0, T0));
	t_step(1);
	CHECK_EXC(SALSA64_EXC_TLBS, T_CODE, 0, T_VEC_TLB);
}

static void tlb_miss_with_exl_uses_general_vector(void)
{
	t_set_status(T_STATUS | SALSA64_ST_EXL);
	R[T0] = 0x00400120;
	T_LOAD(LW(T1, 0, T0));
	t_step(1);
	CHECK_EQ(SALSA64_CAUSE_EXC(CPU->cause), SALSA64_EXC_TLBL);
	CHECK_EQ(CPU->pc, T_VEC_GEN);
}

static void tlb_invalid_entry(void)
{
	tlb_set(2, 0x00400000, LO(0x10, 1, 0, 1), LO(0x11, 1, 1, 1), 0);
	R[T0] = 0x00400120;
	T_LOAD(LW(T1, 0, T0));
	t_step(1);
	CHECK_EXC(SALSA64_EXC_TLBL, T_CODE, 0, T_VEC_GEN);
}

static void tlb_modified(void)
{
	tlb_set(3, 0x00400000, LO(0x10, 0, 1, 1), LO(0x11, 1, 1, 1), 0);
	R[T0] = 0x00400120;
	T_LOAD(LW(T1, 0, T0), SW(T1, 0, T0));
	t_step(2);
	CHECK_EXC(SALSA64_EXC_MOD, T_CODE + 4, 0, T_VEC_GEN);
}

static void tlb_asid_mismatch(void)
{
	tlb_set(4, 0x00400000 | 0x01, LO(0x10, 1, 1, 0), LO(0x11, 1, 1, 0), 0);
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_ENTRYHI, 0x02);
	R[T0] = 0x00400120;
	T_LOAD(LW(T1, 0, T0));
	t_step(1);
	CHECK_EXC(SALSA64_EXC_TLBL, T_CODE, 0, T_VEC_TLB);
	t_set_status(T_STATUS);
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_ENTRYHI, 0x01);
	salsa64_cpu_set_pc(CPU, T_CODE);
	t_step(1);
	CHECK_EQ(CPU->pc, T_CODE + 4);
}

/* ---- Código mapeado y en KSEG1 ----------------------------------------------
 * Con el recompilador, estos bloques van por dirección virtual y física
 * (rec.c): el mapeado revisa su traducción al entrar, también si llega por
 * un enlace. */

#define V_CODE 0x00400000u  /* kuseg; par de páginas de 4 KB: 0x00400000 y 0x00401000 */

/* En `pa`: T0 += inc y vuelve por RA. */
static void put_inc(uint32_t pa, int16_t inc)
{
	t_wr32(pa, ADDIU(T0, T0, inc));
	t_wr32(pa + 4, JR(RA));
	t_wr32(pa + 8, NOP);
}

static void tlb_exec_mapped_call(void)
{
	tlb_set(10, V_CODE, LO(0x10, 1, 1, 1), LO(0x11, 1, 1, 1), 0);
	/* Página par: JAL a la impar; al volver, suma y regresa a KSEG0. */
	t_wr32(0x10000, ADDIU(T1, ZERO, 7));
	t_wr32(0x10004, JAL(V_CODE + 0x1100));
	t_wr32(0x10008, ADDIU(T0, ZERO, 5));
	t_wr32(0x1000C, ADDU(T2, T0, T1));
	t_wr32(0x10010, JR(T7));
	t_wr32(0x10014, NOP);
	t_wr32(0x11100, ADDIU(T0, T0, 1));
	t_wr32(0x11104, JR(RA));
	t_wr32(0x11108, ADDIU(T1, T1, 2));
	R[T7] = T_CODE + 12;
	T_LOAD(LUI(S0, V_CODE >> 16), JR(S0), NOP, ADDIU(T3, ZERO, 1));
	t_step(13);
	CHECK_EQ(R[T0], 6);
	CHECK_EQ(R[T1], 9);
	CHECK_EQ(R[T2], 15);
	CHECK_EQ(R[RA], V_CODE + 12);
	CHECK_EQ(R[T3], 1);
	CHECK_EQ(CPU->pc, T_CODE + 16);
}

/* Bucle mapeado que llama a una función mapeada: el JR de vuelta encuentra
 * el bloque en el código generado (exit_jr) desde la segunda vuelta. */
static void tlb_exec_mapped_loop(void)
{
	tlb_set(10, V_CODE, LO(0x10, 1, 1, 1), LO(0x11, 1, 1, 1), 0);
	t_wr32(0x10000, JAL(V_CODE + 0x100));
	t_wr32(0x10004, ADDIU(T1, T1, 1));
	t_wr32(0x10008, J(V_CODE));
	t_wr32(0x1000C, NOP);
	put_inc(0x10100, 2);
	salsa64_cpu_set_pc(CPU, V_CODE);
	t_step(7 * 20);
	CHECK_EQ(R[T0], 40);
	CHECK_EQ(R[T1], 20);
	CHECK_EQ(R[RA], V_CODE + 8);
	CHECK_EQ(CPU->pc, V_CODE);
}

/* Bucle en KSEG0: llama a V_CODE, que salta (J, enlazable) a la página
 * impar, y después reescribe EntryLo1 con T5. */
static void load_remap_loop(void)
{
	t_wr32(0x10000, J(V_CODE + 0x1000));
	t_wr32(0x10004, NOP);
	R[S0] = V_CODE;
	T_LOAD(JALR(RA, S0), NOP, MTC0(T5, SALSA64_CP0_ENTRYLO1), TLBWI, J(T_CODE_PA), NOP);
}

static void tlb_exec_remap(void)
{
	tlb_set(10, V_CODE, LO(0x10, 1, 1, 1), LO(0x11, 1, 1, 1), 0);
	put_inc(0x11000, 1);
	put_inc(0x21000, 100);
	load_remap_loop();
	R[T5] = LO(0x21, 1, 1, 1);
	t_step(11);
	CHECK_EQ(R[T0], 1);
	CHECK_EQ(CPU->pc, T_CODE);
	t_step(11);
	CHECK_EQ(R[T0], 101);  /* la página impar ahora es 0x21000 */
	t_step(11);
	CHECK_EQ(R[T0], 201);
	R[T5] = LO(0x11, 1, 1, 1);
	t_step(11);
	CHECK_EQ(R[T0], 301);
	t_step(11);
	CHECK_EQ(R[T0], 302);  /* de vuelta en 0x11000 */
	CHECK_EQ(CPU->pc, T_CODE);
}

/* Dos entradas para la misma página con ASID distinto: cambiar EntryHi
 * cambia el código que corre. */
static void tlb_exec_asid(void)
{
	tlb_set(11, V_CODE | 1, LO(0x10, 1, 1, 0), LO(0x11, 1, 1, 0), 0);
	tlb_set(12, V_CODE | 2, LO(0x20, 1, 1, 0), LO(0x21, 1, 1, 0), 0);
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_ENTRYHI, 1);
	put_inc(0x10000, 1);
	put_inc(0x20000, 100);
	R[S0] = V_CODE;
	R[T5] = 2;
	T_LOAD(JALR(RA, S0), NOP, MTC0(T5, SALSA64_CP0_ENTRYHI), J(T_CODE_PA), NOP);
	t_step(8);
	CHECK_EQ(R[T0], 1);
	t_step(8);
	CHECK_EQ(R[T0], 101);
	R[T5] = 1;
	t_step(8);
	CHECK_EQ(R[T0], 201);
	t_step(8);
	CHECK_EQ(R[T0], 202);
	CHECK_EQ(CPU->pc, T_CODE);
}

/* El código mapeado se reescribe con un SW desde `base` (la dirección
 * mapeada o la de KSEG0) y la siguiente llamada corre la instrucción nueva. */
static void code_write_loop(uint64_t base)
{
	tlb_set(10, V_CODE, LO(0x10, 1, 1, 1), LO(0x11, 1, 1, 1), 0);
	put_inc(0x10000, 1);
	salsa64_cpu_set_pc(CPU, T_CODE);
	R[T0] = 0;
	R[S0] = V_CODE;
	R[S1] = base;
	R[T6] = ADDIU(T0, T0, 100);
	T_LOAD(JALR(RA, S0), NOP, SW(T6, 0, S1), J(T_CODE_PA), NOP);
	t_step(8);
	CHECK_EQ(R[T0], 1);
	t_step(8);
	CHECK_EQ(R[T0], 101);
	R[T6] = ADDIU(T0, T0, 1000);
	t_step(8);
	CHECK_EQ(R[T0], 201);
	t_step(8);
	CHECK_EQ(R[T0], 1201);
}

static void tlb_exec_code_write(void)
{
	code_write_loop(V_CODE);
	code_write_loop(0xFFFFFFFF80010000ull);
}

static void exec_kseg1(void)
{
	t_wr32(0x10000, ADDIU(T0, ZERO, 3));
	t_wr32(0x10004, JAL(0x10100));
	t_wr32(0x10008, NOP);
	t_wr32(0x1000C, ADDIU(T1, T0, 1));
	t_wr32(0x10100, JR(RA));
	t_wr32(0x10104, ADDIU(T0, T0, 10));
	salsa64_cpu_set_pc(CPU, 0xFFFFFFFFA0010000ull);
	t_step(6);
	CHECK_EQ(R[T0], 13);
	CHECK_EQ(R[T1], 14);
	CHECK_EQ(R[RA], 0xFFFFFFFFA001000Cull);
	CHECK_EQ(CPU->pc, 0xFFFFFFFFA0010010ull);
}

/* Código en la ROM del cartucho, por la TLB y por KSEG1. */
static void exec_rom(void)
{
	static uint8_t rom[0x2000];
	static const uint32_t code[] = { ADDIU(T0, T0, 0x55), JR(RA), NOP };
	for (unsigned k = 0; k < 3; k++)
		for (unsigned b = 0; b < 4; b++)
			rom[0x1000 + 4 * k + b] = (uint8_t)(code[k] >> (24 - 8 * b));
	salsa64_bus_set_rom(&t_n64.bus, rom, sizeof rom);
	tlb_set(10, V_CODE, LO(0x10000, 0, 1, 1), LO(0x10001, 0, 1, 1), 0);
	R[S0] = V_CODE + 0x1000;
	R[S1] = 0xFFFFFFFFB0001000ull;
	T_LOAD(JALR(RA, S0), NOP, JALR(RA, S1), NOP, ADDIU(T1, T0, 1));
	t_step(11);
	CHECK_EQ(R[T0], 0xAA);
	CHECK_EQ(R[T1], 0xAB);
	CHECK_EQ(CPU->pc, T_CODE + 20);
	salsa64_bus_set_rom(&t_n64.bus, NULL, 0);
}

/* Dos páginas a 16 MB caen en la misma entrada de la TLB del recompilador:
 * se turnan, sin cambiar el resultado. */
static void tlb_exec_same_slot(void)
{
	tlb_set(10, V_CODE, LO(0x10, 1, 1, 1), LO(0x11, 1, 1, 1), 0);
	tlb_set(13, V_CODE + 0x01000000, LO(0x30, 1, 1, 1), LO(0x31, 1, 1, 1), 0);
	t_wr32(0x10000, JAL(V_CODE + 0x01000000));
	t_wr32(0x10004, NOP);
	t_wr32(0x10008, J(V_CODE));
	t_wr32(0x1000C, NOP);
	put_inc(0x30000, 1);
	R[S0] = V_CODE;
	T_LOAD(JR(S0), NOP);
	t_step(2 + 7 * 5);
	CHECK_EQ(R[T0], 5);
	CHECK_EQ(CPU->pc, V_CODE);
}

static void user_mode_cannot_fetch_kseg0(void)
{
	t_set_status((T_STATUS & ~SALSA64_ST_CU0) | KSU_USER);
	T_LOAD(NOP);
	t_step(1);
	CHECK_EXC(SALSA64_EXC_ADEL, T_CODE, 0, T_VEC_GEN);
}

static void user_mode_restrictions(void)
{
	/* Página 0 de kuseg → física 0x10000; ahí va el código de usuario. */
	tlb_set(0, 0, LO(0x10, 1, 1, 1), LO(0x11, 1, 1, 1), 0);
	t_wr32(0x10000, MFC0(T0, SALSA64_CP0_STATUS));
	t_wr32(0x10004, DADDU(T1, T2, T3));
	R[T2] = 0x100000000ull;
	R[T3] = 1;

	t_set_status((T_STATUS & ~SALSA64_ST_CU0) | KSU_USER);
	salsa64_cpu_set_pc(CPU, 0);
	t_step(1);
	CHECK_EXC(SALSA64_EXC_CPU, 0, 0, T_VEC_GEN);
	CHECK_EQ((CPU->cause >> SALSA64_CAUSE_CE_SHIFT) & 3, 0);

	t_set_status((T_STATUS & ~SALSA64_ST_CU0) | KSU_USER);
	salsa64_cpu_set_pc(CPU, 4);
	t_step(1);
	CHECK_EXC(SALSA64_EXC_RI, 4, 0, T_VEC_GEN);  /* 64 bits sin UX */

	t_set_status((T_STATUS & ~SALSA64_ST_CU0) | KSU_USER | SALSA64_ST_UX);
	salsa64_cpu_set_pc(CPU, 4);
	t_step(1);
	CHECK_EQ(R[T1], 0x100000001ull);
	CHECK_EQ(CPU->status & SALSA64_ST_EXL, 0);
}

static void random_in_range(void)
{
	R[T0] = 10;
	T_LOAD(MTC0(T0, SALSA64_CP0_WIRED), MFC0(T1, SALSA64_CP0_RANDOM));
	t_wr32(T_CODE_PA + 0x100, MFC0(T2, SALSA64_CP0_RANDOM));
	t_step(2);
	CHECK(R[T1] >= 10 && R[T1] <= 31);
	t_step(63);
	CHECK(R[T2] >= 10 && R[T2] <= 31);
	CHECK(R[T1] != R[T2] || R[T1] == 31);
}

static void unused_register_latch(void)
{
	R[T0] = 0x1234;
	T_LOAD(MTC0(T0, 7), MFC0(T1, 22));
	t_step(2);
	CHECK_EQ(R[T1], 0x1234);
}

/* Sin COP2, con Status.CU2 los movimientos usan un latch de 64 bits; el resto
 * da RI con Cause.CE = 2 (nemu64-test, cop_unusable). */
static void cop2_latch(void)
{
	t_set_status(T_STATUS | SALSA64_ST_CU2);
	R[T0] = 0x0123456789ABCDEFull;
	T_LOAD(M_R(0x12, 0x04, T0, 5, 0, 0),   /* MTC2 */
	       M_R(0x12, 0x00, T1, 6, 0, 0),   /* MFC2 */
	       M_R(0x12, 0x01, T2, 7, 0, 0),   /* DMFC2 */
	       M_R(0x12, 0x03, T3, 7, 0, 0));  /* DCFC2: RI */
	t_step(4);
	CHECK_EQ(R[T1], 0xFFFFFFFF89ABCDEFull);
	CHECK_EQ(R[T2], 0x0123456789ABCDEFull);
	CHECK_EXC(SALSA64_EXC_RI, T_CODE + 12, 0, T_VEC_GEN);
	CHECK_EQ((CPU->cause >> SALSA64_CAUSE_CE_SHIFT) & 3, 2);

	t_set_status(T_STATUS);
	salsa64_cpu_set_pc(CPU, T_CODE);
	t_step(1);
	CHECK_EXC(SALSA64_EXC_CPU, T_CODE, 0, T_VEC_GEN);
	CHECK_EQ((CPU->cause >> SALSA64_CAUSE_CE_SHIFT) & 3, 2);
}

/* KX = 1: xkphys (0x9000...) llega a la memoria física; con bits 58–32
 * distintos de 0 es error de dirección. */
static void xkphys_with_kx(void)
{
	t_wr32(0x2000, 0xCAFEF00D);
	t_set_status(T_STATUS | SALSA64_ST_KX);
	R[T0] = 0x9000000000002000ull;
	R[T1] = 0x9000000100002000ull;
	T_LOAD(LW(T2, 0, T0), LW(T3, 0, T1));
	t_step(2);
	CHECK_EQ(R[T2], 0xFFFFFFFFCAFEF00Dull);
	CHECK_EXC(SALSA64_EXC_ADEL, T_CODE + 4, 0, T_VEC_GEN);
	CHECK_EQ(CPU->bad_vaddr, 0x9000000100002000ull);
}

void t_suite_cop0(void)
{
	RUN(prid_and_config);
	RUN(write_masks);
	RUN(mtc0_writes_64_bits);
	RUN(syscall_and_eret);
	RUN(break_exception);
	RUN(bev_vectors);
	RUN(nested_exception_keeps_epc);
	RUN(reserved_instruction);
	RUN(cop2_unusable);
	RUN(traps);
	RUN(trap_immediate);
	RUN(misaligned_fetch);
	RUN(count_half_rate);
	RUN(dcache_sweep_cost);
	RUN(dcache_sweep_loop);
	RUN(dcache_sweep_delay_slot);
	RUN(dcache_sweep_off);
	RUN(mfc0_reads_every_register);
	RUN(mfc0_count_inside_block);
	RUN(count_write);
	RUN(compare_interrupt);
	RUN(software_interrupt);
	RUN(interrupt_blocked_by_exl_and_ie);
	RUN(mi_interrupt_on_ip2);
	RUN(status_write_enables_pending_irq);
	RUN(status_write_ie_without_irq);
	RUN(status_write_cu1_off_then_cop1);
	RUN(eret_from_error_level);
	RUN(kuseg_unmapped_with_erl);
	RUN(tlb_mapped_access);
	RUN(tlb_read_back);
	RUN(tlb_probe_miss);
	RUN(tlb_large_pages);
	RUN(tlb_refill_on_miss);
	RUN(tlb_refill_store);
	RUN(tlb_miss_with_exl_uses_general_vector);
	RUN(tlb_invalid_entry);
	RUN(tlb_modified);
	RUN(tlb_asid_mismatch);
	RUN(tlb_exec_mapped_call);
	RUN(tlb_exec_mapped_loop);
	RUN(tlb_exec_remap);
	RUN(tlb_exec_asid);
	RUN(tlb_exec_code_write);
	RUN(exec_kseg1);
	RUN(exec_rom);
	RUN(tlb_exec_same_slot);
	RUN(user_mode_cannot_fetch_kseg0);
	RUN(user_mode_restrictions);
	RUN(random_in_range);
	RUN(unused_register_latch);
	RUN(cop2_latch);
	RUN(xkphys_with_kx);
}
