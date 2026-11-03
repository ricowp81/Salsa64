/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — COP0 del VR4300: registros de control, excepciones, TLB,
 * Count/Compare e interrupciones.
 *
 * Vectores (BEV = 0 → base 0x80000000; BEV = 1 → 0xBFC00200):
 *   +0x000 refill de TLB (32 bits), +0x080 refill XTLB (64 bits),
 *   +0x180 todas las demás. Con EXL = 1 todo va a +0x180 y EPC/BD no cambian.
 */
#include <string.h>

#include "core/sched.h"
#include "cpu/vr4300_internal.h"

#define CAUSE_IP7       0x8000u
#define CAUSE_SW_MASK   0x0300u  /* IP0–IP1: los únicos bits escribibles */
#define STATUS_WMASK    0xFF57FFFFu
#define CONFIG_WMASK    0x0F00800Fu
#define CONFIG_RESET    0x7006E463u
#define ENTRYHI_MASK    0xC00000FFFFFFE0FFull
#define ENTRYLO_MASK    0x3FFFFFFFu
#define TLB_ENTRYLO_MASK 0x03FFFFFEu  /* la TLB guarda PFN de 20 bits, C, D y V */
#define PAGEMASK_MASK   0x01FFE000u
#define VEC_BASE        0xFFFFFFFF80000000ull
#define VEC_BASE_BEV    0xFFFFFFFFBFC00200ull

/* ---- Estado derivado ------------------------------------------------------ */

static void update_irq(struct salsa64_cpu *cpu)
{
	uint32_t st = cpu->status;
	cpu->irq_pending = (cpu->cause & st & SALSA64_ST_IM) != 0 &&
			   (st & (SALSA64_ST_IE | SALSA64_ST_EXL | SALSA64_ST_ERL)) == SALSA64_ST_IE;
}

void salsa64_vr4300_update_status(struct salsa64_cpu *cpu)
{
	uint32_t st = cpu->status;
	if (st & (SALSA64_ST_EXL | SALSA64_ST_ERL))
		cpu->mode = SALSA64_MODE_KERNEL;
	else if ((st & SALSA64_ST_KSU) == 0)
		cpu->mode = SALSA64_MODE_KERNEL;
	else if ((st & SALSA64_ST_KSU) == 0x08)
		cpu->mode = SALSA64_MODE_SUPERVISOR;
	else
		cpu->mode = SALSA64_MODE_USER;

	cpu->re_xor = (cpu->mode == SALSA64_MODE_USER && (st & SALSA64_ST_RE)) ? 7 : 0;
	cpu->allow64 = cpu->mode == SALSA64_MODE_KERNEL ||
		       (cpu->mode == SALSA64_MODE_SUPERVISOR && (st & SALSA64_ST_SX)) ||
		       (cpu->mode == SALSA64_MODE_USER && (st & SALSA64_ST_UX));
	update_irq(cpu);
}

void salsa64_cpu_set_ip(struct salsa64_cpu *cpu, unsigned line, bool level)
{
	uint32_t bit = 1u << (SALSA64_CAUSE_IP_SHIFT + line);
	if (level)
		cpu->cause |= bit;
	else
		cpu->cause &= ~bit;
	update_irq(cpu);
}

/* ---- Count / Compare ------------------------------------------------------- */

uint32_t salsa64_cpu_count(const struct salsa64_cpu *cpu)
{
	return (uint32_t)(cpu->cycles >> 1) + cpu->count_offset;
}

/* Count avanza 1 cada 2 ciclos; el evento cae en el primer ciclo en que
 * Count llega a Compare. Si ya son iguales, la próxima vez es tras la vuelta
 * completa de 32 bits. */
static void schedule_compare(struct salsa64_cpu *cpu)
{
	uint32_t delta = cpu->compare - salsa64_cpu_count(cpu);
	uint64_t ticks = delta ? delta : 0x100000000ull;
	salsa64_sched_set(cpu->sched, SALSA64_EV_COMPARE, ((cpu->cycles >> 1) + ticks) << 1);
}

static void compare_event(void *ctx, uint64_t when)
{
	struct salsa64_cpu *cpu = ctx;
	cpu->cause |= CAUSE_IP7;
	update_irq(cpu);
	salsa64_sched_set(cpu->sched, SALSA64_EV_COMPARE, when + (0x100000000ull << 1));
}

static uint32_t cop0_random(const struct salsa64_cpu *cpu)
{
	/* Random (6 bits) baja 1 por instrucción desde 31 y vuelve a 31 al
	 * llegar a Wired. Con Wired > 31 pasa por 0, sigue en 63 y baja hasta
	 * Wired (nemu64-test: recorre 0..63). */
	uint64_t n = cpu->cycles - cpu->random_epoch;
	uint32_t wired = cpu->wired & 63;
	uint32_t period = wired <= 31 ? 32 - wired : 96 - wired;
	return (31 - (uint32_t)(n % period)) & 63;
}

/* ---- Excepciones ----------------------------------------------------------- */

static void exception_at(struct salsa64_cpu *cpu, enum salsa64_exc code, unsigned ce, uint32_t offset)
{
	if (!(cpu->status & SALSA64_ST_EXL)) {
		if (cpu->delay_slot) {
			cpu->epc = cpu->cur_pc - 4;
			cpu->cause |= SALSA64_CAUSE_BD;
		} else {
			cpu->epc = cpu->cur_pc;
			cpu->cause &= ~SALSA64_CAUSE_BD;
		}
	} else {
		offset = 0x180;
	}
	cpu->cause = (cpu->cause & ~(0x7Cu | (3u << SALSA64_CAUSE_CE_SHIFT))) |
		     (uint32_t)code << 2 | (ce & 3) << SALSA64_CAUSE_CE_SHIFT;
	cpu->status |= SALSA64_ST_EXL;
	cpu->exc_count++;

	cpu->pc = ((cpu->status & SALSA64_ST_BEV) ? VEC_BASE_BEV : VEC_BASE) + offset;
	cpu->next_pc = cpu->pc + 4;
	cpu->branch = 0;
	cpu->redirect = 1;
	salsa64_vr4300_update_status(cpu);
	if (SALSA64_UNLIKELY(cpu->exc_hook != NULL))
		cpu->exc_hook(cpu);
}

void salsa64_vr4300_exception(struct salsa64_cpu *cpu, enum salsa64_exc code, unsigned ce)
{
	exception_at(cpu, code, ce, 0x180);
}

/* BadVAddr, Context, XContext y EntryHi.VPN2 apuntan a la dirección que falló. */
static void set_bad_vaddr(struct salsa64_cpu *cpu, uint64_t va)
{
	cpu->bad_vaddr = va;
	cpu->context = (cpu->context & 0xFFFFFFFFFF800000ull) | ((va >> 13) & 0x7FFFF) << 4;
	cpu->xcontext = (cpu->xcontext & 0xFFFFFFFE00000000ull) | ((va >> 62) & 3) << 31 |
			((va >> 13) & 0x7FFFFFF) << 4;
	cpu->entry_hi = (va & 0xC00000FFFFFFE000ull) | (cpu->entry_hi & 0xFF);
}

void salsa64_vr4300_address_error(struct salsa64_cpu *cpu, uint64_t vaddr, enum salsa64_access acc)
{
	set_bad_vaddr(cpu, vaddr);
	salsa64_vr4300_exception(cpu, acc == SALSA64_ACC_STORE ? SALSA64_EXC_ADES : SALSA64_EXC_ADEL, 0);
}

enum tlb_fault { TLB_MISS, TLB_INVALID, TLB_MODIFIED };

static void tlb_exception(struct salsa64_cpu *cpu, uint64_t va, enum salsa64_access acc, enum tlb_fault f)
{
	set_bad_vaddr(cpu, va);
	enum salsa64_exc code = f == TLB_MODIFIED ? SALSA64_EXC_MOD
				: acc == SALSA64_ACC_STORE ? SALSA64_EXC_TLBS : SALSA64_EXC_TLBL;
	uint32_t offset = 0x180;
	if (f == TLB_MISS) {
		uint32_t xbit = cpu->mode == SALSA64_MODE_KERNEL ? SALSA64_ST_KX
				: cpu->mode == SALSA64_MODE_SUPERVISOR ? SALSA64_ST_SX : SALSA64_ST_UX;
		offset = (cpu->status & xbit) ? 0x080 : 0x000;
	}
	exception_at(cpu, code, 0, offset);
}

/* ---- TLB y traducción ------------------------------------------------------ */

/* Bits de una dirección virtual que la TLB compara: región (63–62) y VPN2
 * (39–13); la máscara de página quita los bajos. */
#define TLB_VPN_BITS 0xC00000FFFFFFE000ull

/* Primera entrada que coincide con `va` (VPN2 y ASID o global), o NULL. */
static const struct salsa64_tlb_entry *tlb_find(const struct salsa64_cpu *cpu, uint64_t va)
{
	uint32_t asid = (uint32_t)cpu->entry_hi & 0xFF;
	for (int n = 0; n < SALSA64_TLB_ENTRIES; n++) {
		const struct salsa64_tlb_entry *e = &cpu->tlb[n];
		if ((va ^ e->entry_hi) & e->vpn_mask)
			continue;
		if (!e->global && ((uint32_t)e->entry_hi & 0xFF) != asid)
			continue;
		return e;
	}
	return NULL;
}

static uint32_t tlb_pa(const struct salsa64_tlb_entry *e, uint32_t lo, uint64_t va)
{
	uint32_t offset_mask = e->odd_bit - 1;
	return ((lo >> 6) << 12 & ~offset_mask) | ((uint32_t)va & offset_mask);
}

#ifdef SALSA64_MEM_CENSUS
struct salsa64_mem_census salsa64_mem_census;

static unsigned census_ps(uint32_t odd_bit)
{
	unsigned k = 0;
	while (odd_bit > 0x1000u && k < 12) {
		odd_bit >>= 1;
		k++;
	}
	return k;
}

/* Un acceso a datos que pasa por la TLB (acc != FETCH): tamaño de página y
 * si su dirección doblada a una ventana de 1 o 2 GB cae donde estaría una
 * imagen de la RDRAM (docs/fastmem.md). */
static void census_tlb(uint64_t va, const struct salsa64_tlb_entry *e, enum salsa64_access acc)
{
	if (acc == SALSA64_ACC_FETCH)
		return;
	uint32_t a = (uint32_t)va;
	salsa64_mem_census.tlb_ps[census_ps(e->odd_bit)]++;
	salsa64_mem_census.slots[(a >> 28) & 7] |= 1u << ((a >> 23) & 31);
	uint32_t i1 = a & 0x3FFFFFFFu, i2 = a & 0x7FFFFFFFu;
	if (i1 < 0x800000u || (i1 >= 0x20000000u && i1 < 0x20800000u))
		salsa64_mem_census.collide_1g++;
	if (i2 < 0x800000u || (i2 >= 0x20000000u && i2 < 0x20800000u))
		salsa64_mem_census.collide_2g++;
}
#endif

static bool tlb_lookup(struct salsa64_cpu *cpu, uint64_t va, uint32_t *pa, enum salsa64_access acc)
{
	const struct salsa64_tlb_entry *e = tlb_find(cpu, va);
	if (!e) {
#ifdef SALSA64_MEM_CENSUS
		if (acc != SALSA64_ACC_FETCH)
			salsa64_mem_census.miss++;
#endif
		tlb_exception(cpu, va, acc, TLB_MISS);
		return false;
	}
	uint32_t lo = e->entry_lo[((uint32_t)va & e->odd_bit) ? 1 : 0];
	if (!(lo & 0x2)) {
#ifdef SALSA64_MEM_CENSUS
		if (acc != SALSA64_ACC_FETCH)
			salsa64_mem_census.invalid++;
#endif
		tlb_exception(cpu, va, acc, TLB_INVALID);
		return false;
	}
	if (acc == SALSA64_ACC_STORE && !(lo & 0x4)) {
#ifdef SALSA64_MEM_CENSUS
		salsa64_mem_census.modified++;
#endif
		tlb_exception(cpu, va, acc, TLB_MODIFIED);
		return false;
	}
#ifdef SALSA64_MEM_CENSUS
	census_tlb(va, e, acc);
#endif
	*pa = tlb_pa(e, lo, va);
	return true;
}

bool salsa64_vr4300_kernel_probe(const struct salsa64_cpu *cpu, uint64_t va, uint32_t *pa, bool *dirty)
{
	if ((int64_t)va != (int32_t)va || cpu->mode != SALSA64_MODE_KERNEL)
		return false;
	uint32_t a = (uint32_t)va;
	if (a >= 0x80000000u && a < 0xC0000000u) {
		*pa = a & 0x1FFFFFFF;
		*dirty = true;
		return true;
	}
	if (a < 0x80000000u && (cpu->status & SALSA64_ST_ERL))
		return false;  /* kuseg sin mapear: no vale la pena compilarlo */
	const struct salsa64_tlb_entry *e = tlb_find(cpu, va);
	if (!e)
		return false;
	uint32_t lo = e->entry_lo[(a & e->odd_bit) ? 1 : 0];
	if (!(lo & 0x2))
		return false;
	*pa = tlb_pa(e, lo, va);
	*dirty = (lo & 0x4) != 0;
	return true;
}

bool salsa64_vr4300_fetch_probe(const struct salsa64_cpu *cpu, uint64_t va, uint32_t *pa)
{
	bool dirty;
	return salsa64_vr4300_kernel_probe(cpu, va, pa, &dirty);
}

/* Direccionamiento de 64 bits (KX, SX o UX según el modo). Espacios del
 * VR4300, con direcciones virtuales de 40 bits:
 *   xkuseg/xsuseg/xuseg  0x0000000000000000–0x000000FFFFFFFFFF  TLB
 *   xksseg/xsseg         0x4000000000000000–0x400000FFFFFFFFFF  TLB (no usuario)
 *   xkphys               0x8000000000000000–0xBFFFFFFFFFFFFFFF  sin TLB, solo kernel;
 *                        bits 61–59 = caché, 58–32 en 0, físico = bits 31–0
 *   xkseg                0xC000000000000000–0xC00000FF7FFFFFFF  TLB, solo kernel
 *   ckseg0/1             0xFFFFFFFF80000000–0xFFFFFFFFBFFFFFFF  sin TLB, solo kernel
 *   cksseg/ckseg3        0xFFFFFFFFC0000000–0xFFFFFFFFFFFFFFFF  TLB (supervisor:
 *                        solo hasta 0xFFFFFFFFDFFFFFFF)
 * Lo demás es error de dirección. */
static bool translate64(struct salsa64_cpu *cpu, uint64_t va, uint32_t *pa, enum salsa64_access acc)
{
	unsigned mode = cpu->mode;
#ifdef SALSA64_MEM_CENSUS
	if (acc != SALSA64_ACC_FETCH)
		salsa64_mem_census.x64++;
#endif
	if (va < 0x0000010000000000ull) {
		if (mode == SALSA64_MODE_KERNEL && (cpu->status & SALSA64_ST_ERL) && va < 0x80000000u) {
			*pa = (uint32_t)va;  /* con ERL, los primeros 2 GB quedan sin mapear */
			return true;
		}
		return tlb_lookup(cpu, va, pa, acc);
	}
	if (mode == SALSA64_MODE_USER)
		goto bad;
	if (va >= 0x4000000000000000ull && va < 0x4000010000000000ull)
		return tlb_lookup(cpu, va, pa, acc);
	if (va >= 0xFFFFFFFFC0000000ull) {
		if (mode == SALSA64_MODE_SUPERVISOR && va >= 0xFFFFFFFFE0000000ull)
			goto bad;
		return tlb_lookup(cpu, va, pa, acc);
	}
	if (mode == SALSA64_MODE_SUPERVISOR)
		goto bad;
	if (va >= 0xFFFFFFFF80000000ull) {
		*pa = (uint32_t)va & 0x1FFFFFFF;
		return true;
	}
	if ((va >> 62) == 2 && !(va & 0x07FFFFFF00000000ull)) {
		*pa = (uint32_t)va;
		return true;
	}
	if (va >= 0xC000000000000000ull && va < 0xC00000FF80000000ull)
		return tlb_lookup(cpu, va, pa, acc);
bad:
	salsa64_vr4300_address_error(cpu, va, acc);
	return false;
}

static bool translate32(struct salsa64_cpu *cpu, uint64_t va, uint32_t *pa, enum salsa64_access acc)
{
	/* 32 bits: la dirección tiene que ser extensión de signo de 32. */
	if (SALSA64_UNLIKELY(salsa64_sx32((uint32_t)va) != va)) {
		salsa64_vr4300_address_error(cpu, va, acc);
		return false;
	}

	uint32_t a = (uint32_t)va;
	if (cpu->mode == SALSA64_MODE_USER && a >= 0x80000000u) {
		salsa64_vr4300_address_error(cpu, va, acc);
		return false;
	}
	if (cpu->mode == SALSA64_MODE_SUPERVISOR && a >= 0x80000000u &&
	    (a < 0xC0000000u || a >= 0xE0000000u)) {
		salsa64_vr4300_address_error(cpu, va, acc);
		return false;
	}

	if (a < 0x80000000u) {
		if (cpu->status & SALSA64_ST_ERL) {  /* kuseg sin mapear con ERL */
#ifdef SALSA64_MEM_CENSUS
			if (acc != SALSA64_ACC_FETCH)
				salsa64_mem_census.erl++;
#endif
			*pa = a;
			return true;
		}
		return tlb_lookup(cpu, va, pa, acc);
	}
	if (a < 0xC0000000u) {
		*pa = a & 0x1FFFFFFF;
		return true;
	}
#ifdef SALSA64_MEM_CENSUS
	if (acc != SALSA64_ACC_FETCH)
		salsa64_mem_census.kseg23++;
#endif
	return tlb_lookup(cpu, va, pa, acc);
}

bool salsa64_vr4300_translate_slow(struct salsa64_cpu *cpu, uint64_t va, uint32_t *pa,
				   enum salsa64_access acc, uint32_t align)
{
	uint32_t wide = cpu->mode == SALSA64_MODE_KERNEL ? SALSA64_ST_KX
			: cpu->mode == SALSA64_MODE_SUPERVISOR ? SALSA64_ST_SX : SALSA64_ST_UX;
	bool ok = (cpu->status & wide) ? translate64(cpu, va, pa, acc) : translate32(cpu, va, pa, acc);
	/* Status.RE: XOR 7, 6, 4 o 0 según el tamaño (solo en modo usuario). */
	if (ok)
		*pa ^= cpu->re_xor & ~align;
	return ok;
}

static void tlb_write(struct salsa64_cpu *cpu, unsigned n)
{
	struct salsa64_tlb_entry *e = &cpu->tlb[n & 31];
	/* Cada par de bits de la máscara vale lo que su bit alto (nemu64-test:
	 * 01 se guarda como 00 y 10 como 11). */
	uint32_t hi = cpu->page_mask & 0x01554000u;
	e->page_mask = hi | hi >> 1;
	e->entry_hi = cpu->entry_hi & ENTRYHI_MASK & ~(uint64_t)e->page_mask;
	e->entry_lo[0] = cpu->entry_lo0 & TLB_ENTRYLO_MASK;
	e->entry_lo[1] = cpu->entry_lo1 & TLB_ENTRYLO_MASK;
	e->global = cpu->entry_lo0 & cpu->entry_lo1 & 1;
	e->vpn_mask = TLB_VPN_BITS & ~(uint64_t)e->page_mask;
	e->odd_bit = ((e->page_mask | 0x1FFFu) + 1) >> 1;
	cpu->tlb_writes++;
#ifdef SALSA64_MEM_CENSUS
	salsa64_mem_census.tlbw_ps[census_ps(e->odd_bit)]++;
#endif
}

static void tlb_read(struct salsa64_cpu *cpu, unsigned n)
{
	const struct salsa64_tlb_entry *e = &cpu->tlb[n & 31];
	cpu->page_mask = e->page_mask;
	cpu->entry_hi = e->entry_hi;
	cpu->entry_lo0 = e->entry_lo[0] | e->global;
	cpu->entry_lo1 = e->entry_lo[1] | e->global;
}

static void tlb_probe(struct salsa64_cpu *cpu)
{
	uint64_t hi = cpu->entry_hi;
	uint32_t asid = (uint32_t)hi & 0xFF;
	cpu->index = 0x80000000u;
	for (int n = 0; n < SALSA64_TLB_ENTRIES; n++) {
		const struct salsa64_tlb_entry *e = &cpu->tlb[n];
		if ((hi ^ e->entry_hi) & e->vpn_mask)
			continue;
		if (!e->global && ((uint32_t)e->entry_hi & 0xFF) != asid)
			continue;
		cpu->index = (uint32_t)n;
		return;
	}
}

/* ---- Registros ------------------------------------------------------------- */

uint64_t salsa64_cpu_cop0_read(struct salsa64_cpu *cpu, unsigned reg)
{
	switch (reg) {
	case SALSA64_CP0_INDEX:       return cpu->index;
	case SALSA64_CP0_RANDOM:      return cop0_random(cpu);
	case SALSA64_CP0_ENTRYLO0:    return cpu->entry_lo0;
	case SALSA64_CP0_ENTRYLO1:    return cpu->entry_lo1;
	case SALSA64_CP0_CONTEXT:     return cpu->context;
	case SALSA64_CP0_PAGEMASK:    return cpu->page_mask;
	case SALSA64_CP0_WIRED:       return cpu->wired;
	case SALSA64_CP0_BADVADDR:    return cpu->bad_vaddr;
	case SALSA64_CP0_COUNT:       return salsa64_cpu_count(cpu);
	case SALSA64_CP0_ENTRYHI:     return cpu->entry_hi;
	case SALSA64_CP0_COMPARE:     return cpu->compare;
	case SALSA64_CP0_STATUS:      return cpu->status;
	case SALSA64_CP0_CAUSE:       return cpu->cause;
	case SALSA64_CP0_EPC:         return cpu->epc;
	case SALSA64_CP0_PRID:        return SALSA64_PRID_VALUE;
	case SALSA64_CP0_CONFIG:      return cpu->config;
	case SALSA64_CP0_LLADDR:      return cpu->lladdr;
	case SALSA64_CP0_WATCHLO:     return cpu->watch_lo;
	case SALSA64_CP0_WATCHHI:     return cpu->watch_hi;
	case SALSA64_CP0_XCONTEXT:    return cpu->xcontext;
	case SALSA64_CP0_PARITYERROR: return cpu->parity_error;
	case SALSA64_CP0_CACHEERROR:  return cpu->cache_error;
	case SALSA64_CP0_TAGLO:       return cpu->tag_lo;
	case SALSA64_CP0_TAGHI:       return cpu->tag_hi;
	case SALSA64_CP0_ERROREPC:    return cpu->error_epc;
	default:                      return cpu->cop0_latch;  /* 7, 21–25, 31 */
	}
}

void salsa64_cpu_cop0_write(struct salsa64_cpu *cpu, unsigned reg, uint64_t v)
{
	cpu->cop0_latch = v;
	switch (reg) {
	case SALSA64_CP0_INDEX:       cpu->index = (uint32_t)v & 0x8000003Fu; return;
	case SALSA64_CP0_ENTRYLO0:    cpu->entry_lo0 = (uint32_t)v & ENTRYLO_MASK; return;
	case SALSA64_CP0_ENTRYLO1:    cpu->entry_lo1 = (uint32_t)v & ENTRYLO_MASK; return;
	case SALSA64_CP0_CONTEXT:
		cpu->context = (v & 0xFFFFFFFFFF800000ull) | (cpu->context & 0x7FFFF0);
		return;
	case SALSA64_CP0_PAGEMASK:    cpu->page_mask = (uint32_t)v & PAGEMASK_MASK; return;
	case SALSA64_CP0_WIRED:
		cpu->wired = (uint32_t)v & 0x3F;
		cpu->random_epoch = cpu->cycles;
		return;
	case SALSA64_CP0_COUNT:
		cpu->count_offset = (uint32_t)v - (uint32_t)(cpu->cycles >> 1);
		schedule_compare(cpu);
		return;
	case SALSA64_CP0_ENTRYHI:     cpu->entry_hi = v & ENTRYHI_MASK; return;
	case SALSA64_CP0_COMPARE:
		cpu->compare = (uint32_t)v;
		cpu->cause &= ~CAUSE_IP7;
		update_irq(cpu);
		schedule_compare(cpu);
		return;
	case SALSA64_CP0_STATUS:
		cpu->status = (uint32_t)v & STATUS_WMASK;
		salsa64_vr4300_update_status(cpu);
		return;
	case SALSA64_CP0_CAUSE:
		cpu->cause = (cpu->cause & ~CAUSE_SW_MASK) | ((uint32_t)v & CAUSE_SW_MASK);
		update_irq(cpu);
		return;
	case SALSA64_CP0_EPC:         cpu->epc = v; return;
	case SALSA64_CP0_CONFIG:
		cpu->config = (cpu->config & ~CONFIG_WMASK) | ((uint32_t)v & CONFIG_WMASK);
		return;
	case SALSA64_CP0_LLADDR:      cpu->lladdr = (uint32_t)v; return;
	case SALSA64_CP0_WATCHLO:     cpu->watch_lo = (uint32_t)v & 0xFFFFFFFBu; return;
	case SALSA64_CP0_WATCHHI:     cpu->watch_hi = (uint32_t)v & 0xF; return;
	case SALSA64_CP0_XCONTEXT:
		cpu->xcontext = (v & 0xFFFFFFFE00000000ull) | (cpu->xcontext & 0x1FFFFFFFFull);
		return;
	case SALSA64_CP0_PARITYERROR: cpu->parity_error = (uint32_t)v & 0xFF; return;
	case SALSA64_CP0_TAGLO:       cpu->tag_lo = (uint32_t)v; return;
	case SALSA64_CP0_TAGHI:       cpu->tag_hi = 0; return;
	case SALSA64_CP0_ERROREPC:    cpu->error_epc = v; return;
	default:                      return;  /* solo lectura o sin implementar */
	}
}

/* ---- Instrucciones --------------------------------------------------------- */

static void op_eret(struct salsa64_cpu *cpu)
{
	if (cpu->status & SALSA64_ST_ERL) {
		cpu->pc = cpu->error_epc;
		cpu->status &= ~SALSA64_ST_ERL;
	} else {
		cpu->pc = cpu->epc;
		cpu->status &= ~SALSA64_ST_EXL;
	}
	cpu->next_pc = cpu->pc + 4;
	cpu->branch = 0;  /* ERET no tiene delay slot */
	cpu->redirect = 1;
	cpu->llbit = 0;
	salsa64_vr4300_update_status(cpu);
}

/* Para el recompilador (rec.c): TLBWI o TLBWR, devolviendo la entrada
 * escrita; y ERET. */
unsigned salsa64_vr4300_tlbw(struct salsa64_cpu *cpu, bool random)
{
	unsigned n = (random ? cop0_random(cpu) : cpu->index) & 31;
	tlb_write(cpu, n);
	return n;
}

void salsa64_vr4300_eret(struct salsa64_cpu *cpu)
{
	op_eret(cpu);
}

void salsa64_vr4300_cop0(struct salsa64_cpu *cpu, uint32_t i)
{
	if (cpu->mode != SALSA64_MODE_KERNEL && !(cpu->status & SALSA64_ST_CU0)) {
		salsa64_vr4300_exception(cpu, SALSA64_EXC_CPU, 0);
		return;
	}

	unsigned rt = I_RT(i), rd = I_RD(i);
	switch (I_RS(i)) {
	case 0x00:                                                             /* MFC0 */
		vr4300_set_gpr(cpu, rt, salsa64_sx32((uint32_t)salsa64_cpu_cop0_read(cpu, rd)));
		return;
	case 0x01:                                                             /* DMFC0 */
		if (!cpu->allow64)
			break;
		vr4300_set_gpr(cpu, rt, salsa64_cpu_cop0_read(cpu, rd));
		return;
	case 0x04:                                                             /* MTC0 */
		/* Transfiere los 64 bits del GPR, igual que DMTC0 (nemu64-test:
		 * Context, XContext, EPC y el latch con MTC0 + DMFC0). */
		salsa64_cpu_cop0_write(cpu, rd, cpu->gpr[rt]);
		return;
	case 0x05:                                                             /* DMTC0 */
		if (!cpu->allow64)
			break;
		salsa64_cpu_cop0_write(cpu, rd, cpu->gpr[rt]);
		return;
	case 0x02: case 0x06: case 0x08:                                       /* CFC0, CTC0, BC0 */
		return;  /* reconocidas e inertes (nemu64-test) */
	case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
	case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
		switch (I_FUNCT(i)) {
		case 0x01: tlb_read(cpu, cpu->index); return;                 /* TLBR */
		case 0x02: tlb_write(cpu, cpu->index); return;                /* TLBWI */
		case 0x06: tlb_write(cpu, cop0_random(cpu)); return;          /* TLBWR */
		case 0x08: tlb_probe(cpu); return;                            /* TLBP */
		case 0x18: op_eret(cpu); return;                              /* ERET */
		case 0x10: break;                                             /* RFE (R3000): RI */
		default:   return;  /* las demás funciones con CO = 1 no hacen nada */
		}
		break;
	default:
		break;
	}
	vr4300_reserved(cpu);
}

/* ---- Inicio y reset ---------------------------------------------------------- */

void salsa64_cpu_init(struct salsa64_cpu *cpu, struct salsa64_bus *bus, struct salsa64_sched *sched)
{
	memset(cpu, 0, sizeof *cpu);
	cpu->bus = bus;
	cpu->sched = sched;
	cpu->rdram = bus->rdram;
	cpu->rdram_size = bus->rdram_size;
	bus->cpu = cpu;
	cpu->dcache_sweep_cycles = SALSA64_DCACHE_SWEEP_CYCLES;
	salsa64_sched_handler(sched, SALSA64_EV_COMPARE, compare_event, cpu);
	salsa64_cpu_reset(cpu);
}

void salsa64_cpu_reset(struct salsa64_cpu *cpu)
{
	memset(cpu->gpr, 0, sizeof cpu->gpr);
	memset(cpu->fpr, 0, sizeof cpu->fpr);
	cpu->hi = cpu->lo = 0;
	cpu->fcr31 = 0;
	cpu->cop2_latch = 0;
	cpu->llbit = 0;

	cpu->status = SALSA64_ST_ERL | SALSA64_ST_BEV;
	cpu->cause = 0;
	cpu->config = CONFIG_RESET;
	cpu->index = cpu->entry_lo0 = cpu->entry_lo1 = cpu->page_mask = 0;
	cpu->wired = 0;
	cpu->random_epoch = cpu->cycles;
	cpu->context = cpu->xcontext = cpu->bad_vaddr = cpu->entry_hi = 0;
	cpu->epc = cpu->error_epc = 0;
	cpu->lladdr = cpu->watch_lo = cpu->watch_hi = 0;
	cpu->parity_error = cpu->cache_error = cpu->tag_lo = cpu->tag_hi = 0;
	cpu->cop0_latch = 0;

	/* El contenido real de la TLB es indefinido al arrancar. Aquí quedan
	 * entradas inválidas (V = 0) con VPN distintas dentro de KSEG0, que
	 * nunca pasa por la TLB: así ninguna coincide por accidente (es lo
	 * mismo que hace libultra al inicializarla). */
	memset(cpu->tlb, 0, sizeof cpu->tlb);
	for (int n = 0; n < SALSA64_TLB_ENTRIES; n++) {
		cpu->tlb[n].entry_hi = 0xFFFFFFFF80000000ull + (uint64_t)n * 0x2000;
		cpu->tlb[n].vpn_mask = TLB_VPN_BITS;
		cpu->tlb[n].odd_bit = 0x1000;
	}
	cpu->tlb_writes++;

	cpu->compare = 0;
	cpu->count_offset = 0u - (uint32_t)(cpu->cycles >> 1);
	salsa64_cpu_set_idle(cpu, !cpu->idle_off);  /* vacía las tablas de bucles de espera */
	salsa64_vr4300_update_status(cpu);
	schedule_compare(cpu);
	salsa64_cpu_set_pc(cpu, 0xFFFFFFFFBFC00000ull);
}

void salsa64_cpu_set_pc(struct salsa64_cpu *cpu, uint64_t pc)
{
	cpu->pc = pc;
	cpu->next_pc = pc + 4;
	cpu->branch = 0;
	cpu->delay_slot = 0;
	cpu->redirect = 1;
}
