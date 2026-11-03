/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — piezas internas del intérprete del VR4300: decodificación,
 * traducción de direcciones y accesos a memoria.
 *
 * Camino rápido: KSEG0/KSEG1 en modo kernel se traducen restando la base y,
 * si la dirección física cae en la RDRAM, se lee directo del búfer con los
 * accesores de core/endian.h (carga directa en la PS3). Todo lo demás pasa
 * por salsa64_vr4300_translate_slow (TLB, modos) y por el bus.
 */
#ifndef SALSA64_CPU_VR4300_INTERNAL_H
#define SALSA64_CPU_VR4300_INTERNAL_H

#include "core/bus.h"
#include "core/endian.h"
#include "core/write_marks.h"
#include "cpu/vr4300.h"

#define I_OP(i)     ((i) >> 26)
#define I_RS(i)     (((i) >> 21) & 31)
#define I_RT(i)     (((i) >> 16) & 31)
#define I_RD(i)     (((i) >> 11) & 31)
#define I_SA(i)     (((i) >> 6) & 31)
#define I_FUNCT(i)  ((i) & 63)
#define I_SIMM(i)   ((uint64_t)(int64_t)(int16_t)(i))  /* inmediato con signo, 64 bits */
#define I_UIMM(i)   ((uint64_t)((i) & 0xFFFF))
#define I_TARGET(i) ((i) & 0x03FFFFFF)
/* COP1 */
#define I_FMT(i)    I_RS(i)
#define I_FT(i)     I_RT(i)
#define I_FS(i)     I_RD(i)
#define I_FD(i)     I_SA(i)

enum salsa64_access {
	SALSA64_ACC_FETCH,
	SALSA64_ACC_LOAD,
	SALSA64_ACC_STORE,
};

/* cop0.c */
void salsa64_vr4300_exception(struct salsa64_cpu *cpu, enum salsa64_exc code, unsigned ce);
void salsa64_vr4300_address_error(struct salsa64_cpu *cpu, uint64_t vaddr, enum salsa64_access acc);
/* align = tamaño - 1; solo se usa para invertir la dirección con Status.RE. */
bool salsa64_vr4300_translate_slow(struct salsa64_cpu *cpu, uint64_t vaddr, uint32_t *paddr,
				   enum salsa64_access acc, uint32_t align);
/* La dirección física de la instrucción en `va` en modo kernel, sin
 * excepciones ni efectos (el recompilador, idle.c): false si va no es
 * extensión de signo de 32 bits, si la TLB no la resuelve (falla o V = 0) o
 * si es kuseg con ERL. Para KSEG0/KSEG1, la resta de la base. Sirve igual
 * para una carga: en modo kernel se traduce igual (el bit D solo cuenta
 * para almacenamientos). */
bool salsa64_vr4300_fetch_probe(const struct salsa64_cpu *cpu, uint64_t va, uint32_t *pa);
/* Lo mismo, con el bit D de la entrada (si se puede escribir; en KSEG0/KSEG1
 * siempre): la TLB del recompilador la usa para datos (rec_tlb.h). */
bool salsa64_vr4300_kernel_probe(const struct salsa64_cpu *cpu, uint64_t va, uint32_t *pa, bool *dirty);
void salsa64_vr4300_update_status(struct salsa64_cpu *cpu);
void salsa64_vr4300_cop0(struct salsa64_cpu *cpu, uint32_t instr);
/* cop0.c, para el recompilador: TLBWI (o TLBWR con `random`), devuelve la
 * entrada escrita; ERET (deja pc, Status, el modo y redirect). */
unsigned salsa64_vr4300_tlbw(struct salsa64_cpu *cpu, bool random);
void salsa64_vr4300_eret(struct salsa64_cpu *cpu);
/* interp.c: una instrucción que no es salto (la usa el recompilador). */
void salsa64_vr4300_exec_one(struct salsa64_cpu *cpu, uint32_t instr);
/* interp.c: salsa64_cpu_step sin despachar los eventos vencidos (rec.c los
 * despacha y los mide). */
void salsa64_cpu_step_only(struct salsa64_cpu *cpu);
/* cop1.c */
void salsa64_vr4300_cop1(struct salsa64_cpu *cpu, uint32_t instr);
void salsa64_vr4300_cop2(struct salsa64_cpu *cpu, uint32_t instr);
/* idle.c: el salto `instr` en `branch_pc` se acaba de tomar hacia `target`
 * (a menos de SALSA64_IDLE_BODY instrucciones). Si es un bucle de espera,
 * avanza el tiempo y pone cpu->redirect. Hasta el 2026-10-16 eran 8: la
 * espera de NFL Blitz (cinco osRecvMesg sin bloquear y osYieldThread)
 * vuelve con un J de 37. */
#define SALSA64_IDLE_BODY 64
void salsa64_vr4300_idle(struct salsa64_cpu *cpu, uint64_t branch_pc, uint64_t target, uint32_t instr);

static inline void vr4300_reserved(struct salsa64_cpu *cpu)
{
	salsa64_vr4300_exception(cpu, SALSA64_EXC_RI, 0);
}

/* Escribe un registro general. r0 vale siempre 0 porque nunca se escribe:
 * así leerlo (muy frecuente) no choca en el PPE con un almacenamiento
 * pendiente, como pasaría si se pusiera en 0 después de cada instrucción. */
static inline void vr4300_set_gpr(struct salsa64_cpu *cpu, unsigned r, uint64_t v)
{
	if (SALSA64_LIKELY(r != 0))
		cpu->gpr[r] = v;
}

/* ---- Traducción ------------------------------------------------------- */

/* Verifica alineación (align = tamaño - 1) y traduce. Si falla, la
 * excepción ya quedó lanzada y la instrucción no debe escribir nada. */
static inline bool vr4300_vaddr(struct salsa64_cpu *cpu, uint64_t vaddr, uint32_t align,
				enum salsa64_access acc, uint32_t *paddr)
{
	if (SALSA64_UNLIKELY(vaddr & align)) {
#ifdef SALSA64_MEM_CENSUS
		salsa64_mem_census.misaligned++;
#endif
		salsa64_vr4300_address_error(cpu, vaddr, acc);
		return false;
	}
	/* 0xFFFFFFFF80000000–0xFFFFFFFFBFFFFFFF: KSEG0 y KSEG1, sin TLB. */
	if (SALSA64_LIKELY((vaddr >> 30) == 0x3FFFFFFFEull && cpu->mode == SALSA64_MODE_KERNEL)) {
#ifdef SALSA64_MEM_CENSUS
		if (vaddr & 0x20000000)
			salsa64_mem_census.kseg1++;
		else
			salsa64_mem_census.kseg0++;
#endif
		*paddr = (uint32_t)vaddr & 0x1FFFFFFF;
		return true;
	}
	return salsa64_vr4300_translate_slow(cpu, vaddr, paddr, acc, align);
}

/* ---- Accesos físicos (RDRAM directo, resto por el bus) ------------------ */

/* rec.c: un almacenamiento cayó en una zona de la RDRAM con código
 * recompilado. */
void salsa64_rec_code_written(struct salsa64_rec *rec, uint32_t pa, uint32_t len);

static inline void vr4300_code_check(struct salsa64_cpu *cpu, uint32_t pa, uint32_t len)
{
	if (SALSA64_UNLIKELY(cpu->code_flags != NULL) && cpu->code_flags[pa >> 7])
		salsa64_rec_code_written(cpu->rec, pa, len);
}

/* Depuración en el host (build -DSALSA64_WATCH_MEM): cada acceso del CPU a
 * la RDRAM pasa por salsa64_watch_access (src/host/run.c), que cuenta los
 * que caen en un destino de render. Nunca en la PS3. */
#ifdef SALSA64_WATCH_MEM
void salsa64_watch_access(uint32_t pa, int write);
#define VR4300_WATCH(pa, write) salsa64_watch_access((pa), (write))
#else
#define VR4300_WATCH(pa, write) ((void)0)
#endif

static inline uint8_t vr4300_read8(struct salsa64_cpu *cpu, uint32_t pa)
{
	VR4300_WATCH(pa, 0);
	if (SALSA64_LIKELY(pa < cpu->rdram_size))
		return salsa64_rd8(cpu->rdram + pa);
	return (uint8_t)(salsa64_bus_read32(cpu->bus, pa) >> (8 * (3 - (pa & 3))));
}

static inline uint16_t vr4300_read16(struct salsa64_cpu *cpu, uint32_t pa)
{
	VR4300_WATCH(pa, 0);
	if (SALSA64_LIKELY(pa < cpu->rdram_size))
		return salsa64_rd16(cpu->rdram + pa);
	return (uint16_t)(salsa64_bus_read32(cpu->bus, pa) >> (8 * (2 - (pa & 2))));
}

static inline uint32_t vr4300_read32(struct salsa64_cpu *cpu, uint32_t pa)
{
	VR4300_WATCH(pa, 0);
	if (SALSA64_LIKELY(pa < cpu->rdram_size))
		return salsa64_rd32(cpu->rdram + pa);
	return salsa64_bus_read32(cpu->bus, pa);
}

static inline uint64_t vr4300_read64(struct salsa64_cpu *cpu, uint32_t pa)
{
	VR4300_WATCH(pa, 0);
	if (SALSA64_LIKELY(pa < cpu->rdram_size))
		return salsa64_rd64(cpu->rdram + pa);
	return salsa64_bus_read64(cpu->bus, pa);
}

/* SB y SH reciben el registro completo: fuera de la RDRAM el VR4300 pone en
 * el bus el registro desplazado a su carril, y el RCP, la PIF y el bus PI
 * escriben esa palabra entera sin mirar el tamaño (ver core/bus.h). */
static inline void vr4300_write8(struct salsa64_cpu *cpu, uint32_t pa, uint64_t v)
{
	VR4300_WATCH(pa, 1);
	if (SALSA64_LIKELY(pa < cpu->rdram_size)) {
		salsa64_wr8(cpu->rdram + pa, (uint8_t)v);
		if (cpu->rdram_write_marks)
			salsa64_write_marks_mark_range(cpu->rdram_write_marks, cpu->rdram_size, pa, 1);
		vr4300_code_check(cpu, pa, 1);
		return;
	}
	unsigned shift = 8 * (3 - (pa & 3));
	salsa64_bus_write32(cpu->bus, pa, (uint32_t)(v << shift), 0xFFu << shift);
}

static inline void vr4300_write16(struct salsa64_cpu *cpu, uint32_t pa, uint64_t v)
{
	VR4300_WATCH(pa, 1);
	if (SALSA64_LIKELY(pa < cpu->rdram_size)) {
		salsa64_wr16(cpu->rdram + pa, (uint16_t)v);
		if (cpu->rdram_write_marks)
			salsa64_write_marks_mark_range(cpu->rdram_write_marks, cpu->rdram_size, pa, 2);
		vr4300_code_check(cpu, pa, 2);
		return;
	}
	unsigned shift = 8 * (2 - (pa & 2));
	salsa64_bus_write32(cpu->bus, pa, (uint32_t)(v << shift), 0xFFFFu << shift);
}

static inline void vr4300_write32(struct salsa64_cpu *cpu, uint32_t pa, uint32_t v)
{
	VR4300_WATCH(pa, 1);
	if (SALSA64_LIKELY(pa < cpu->rdram_size)) {
		salsa64_wr32(cpu->rdram + pa, v);
		if (cpu->rdram_write_marks)
			salsa64_write_marks_mark_range(cpu->rdram_write_marks, cpu->rdram_size, pa, 4);
		vr4300_code_check(cpu, pa, 4);
		return;
	}
	salsa64_bus_write32(cpu->bus, pa, v, 0xFFFFFFFFu);
}

static inline void vr4300_write64(struct salsa64_cpu *cpu, uint32_t pa, uint64_t v)
{
	VR4300_WATCH(pa, 1);
	if (SALSA64_LIKELY(pa < cpu->rdram_size)) {
		salsa64_wr64(cpu->rdram + pa, v);
		if (cpu->rdram_write_marks)
			salsa64_write_marks_mark_range(cpu->rdram_write_marks, cpu->rdram_size, pa, 8);
		vr4300_code_check(cpu, pa, 8);
		return;
	}
	salsa64_bus_write64(cpu->bus, pa, v, ~0ull);
}

/* SWL/SWR y SDL/SDR: solo los bytes de `mask` (orden de la N64). */
static inline void vr4300_write32_masked(struct salsa64_cpu *cpu, uint32_t pa, uint32_t v, uint32_t mask)
{
	VR4300_WATCH(pa, 1);
	if (SALSA64_LIKELY(pa < cpu->rdram_size)) {
		uint8_t *p = cpu->rdram + pa;
		salsa64_wr32(p, (salsa64_rd32(p) & ~mask) | (v & mask));
		if (cpu->rdram_write_marks)
			salsa64_write_marks_mark_mask(cpu->rdram_write_marks, cpu->rdram_size, pa, mask, 4);
		vr4300_code_check(cpu, pa, 4);
		return;
	}
	salsa64_bus_write32(cpu->bus, pa, v, mask);
}

static inline void vr4300_write64_masked(struct salsa64_cpu *cpu, uint32_t pa, uint64_t v, uint64_t mask)
{
	VR4300_WATCH(pa, 1);
	if (SALSA64_LIKELY(pa < cpu->rdram_size)) {
		uint8_t *p = cpu->rdram + pa;
		salsa64_wr64(p, (salsa64_rd64(p) & ~mask) | (v & mask));
		if (cpu->rdram_write_marks)
			salsa64_write_marks_mark_mask(cpu->rdram_write_marks, cpu->rdram_size, pa, mask, 8);
		vr4300_code_check(cpu, pa, 8);
		return;
	}
	salsa64_bus_write64(cpu->bus, pa, v, mask);
}

/* ---- Registros de la FPU según Status.FR ------------------------------- */
/* FR = 1: 32 registros de 64 bits. FR = 0: 16 pares; un acceso de 32 bits a
 * un registro impar usa la mitad alta del par, y uno de 64 bits ignora el
 * bit bajo del número de registro. */

static inline uint32_t vr4300_fpr_get32(const struct salsa64_cpu *cpu, unsigned r)
{
	if ((cpu->status & SALSA64_ST_FR) || !(r & 1))
		return (uint32_t)cpu->fpr[r];
	return (uint32_t)(cpu->fpr[r & ~1u] >> 32);
}

static inline void vr4300_fpr_set32(struct salsa64_cpu *cpu, unsigned r, uint32_t v)
{
	if ((cpu->status & SALSA64_ST_FR) || !(r & 1))
		cpu->fpr[r] = (cpu->fpr[r] & 0xFFFFFFFF00000000ull) | v;
	else
		cpu->fpr[r & ~1u] = (cpu->fpr[r & ~1u] & 0xFFFFFFFFull) | (uint64_t)v << 32;
}

static inline uint64_t vr4300_fpr_get64(const struct salsa64_cpu *cpu, unsigned r)
{
	return cpu->fpr[(cpu->status & SALSA64_ST_FR) ? r : r & ~1u];
}

static inline void vr4300_fpr_set64(struct salsa64_cpu *cpu, unsigned r, uint64_t v)
{
	cpu->fpr[(cpu->status & SALSA64_ST_FR) ? r : r & ~1u] = v;
}

#endif
