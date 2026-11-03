/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — pruebas de las copias de bytes de los bloques mapeados del
 * recompilador (rec_tlb.c: ranuras que se liberan, desprender y enganchar un
 * bloque reescrito con los mismos bytes). Solo con el recompilador; con el
 * intérprete comprueban los resultados y nada más.
 */
#include <string.h>

#include "test.h"

#include "cpu/rec.h"
#include "cpu/rec_tlb.h"

/* Página virtual 0x00400000 (par 0x10/0x11 de la RDRAM: 0x10000 y 0x11000). */
#define M_VA   0x0000000000400000ull
#define M_PA   0x10000u

static void map_code_page(void)
{
	t_tlb_set(4, 0x00400000, T_LO(0x10, 1, 1, 1), T_LO(0x11, 1, 1, 1), 0);
}

/* Las ranuras se reparten por clase (64, 128 y 256 bytes), se reusan al
 * liberarlas y el pool tiene tope; el reparto viejo (jit-copias-viejas) es
 * lineal y no recupera nada. */
static void copy_slots_are_reused_and_bounded(void)
{
	struct salsa64_rec *rec = CPU->rec;
	if (!rec)
		return;
	static uint32_t slots[SALSA64_REC_X_COPY_BYTES / 256];
	uint32_t a = salsa64_rec_x_copy_alloc(rec, 8), b = salsa64_rec_x_copy_alloc(rec, 64);
	CHECK(a && b && a != b);
	CHECK_EQ((a - 1) % 64, 0);
	salsa64_rec_x_copy_free(rec, a, 8);
	CHECK_EQ(salsa64_rec_x_copy_alloc(rec, 60), a);  /* misma clase: la ranura liberada */
	uint32_t c = salsa64_rec_x_copy_alloc(rec, 100), d = salsa64_rec_x_copy_alloc(rec, 256);
	CHECK(c && d && c != d && c != a && c != b);
	salsa64_rec_x_copy_free(rec, c, 100);
	CHECK(salsa64_rec_x_copy_alloc(rec, 65) == c);    /* clase de 128 */
	CHECK(salsa64_rec_x_copy_alloc(rec, 64) != c);    /* otra clase no la toma */
	salsa64_rec_x_copy_free(rec, d, 256);
	CHECK_EQ(salsa64_rec_x_copy_alloc(rec, 129), d);

	/* Agotar la clase de 256: cabe hasta el tope y ni un byte más. */
	unsigned n = 0;
	while (n < sizeof slots / sizeof slots[0] && (slots[n] = salsa64_rec_x_copy_alloc(rec, 200)) != 0)
		n++;
	CHECK(n > 0 && n < sizeof slots / sizeof slots[0]);
	CHECK(rec->x.copy_used <= SALSA64_REC_X_COPY_BYTES);
	CHECK(rec->x.copy_used + 256 > SALSA64_REC_X_COPY_BYTES);
	CHECK_EQ(salsa64_rec_x_copy_alloc(rec, 256), 0);  /* ni en la lista libre ni en el pool */
	uint32_t top = rec->x.copy_used;
	for (unsigned k = 0; k < n; k++)
		salsa64_rec_x_copy_free(rec, slots[k], 200);
	uint32_t again = salsa64_rec_x_copy_alloc(rec, 256);
	CHECK(again != 0);
	CHECK_EQ(again, slots[n - 1]);  /* la última liberada es la primera que vuelve */
	CHECK_EQ(rec->x.copy_used, top);  /* copy_used no retrocede: las libres se reusan */

	/* Reparto viejo: lineal, sin liberar, tope de 1 MB. */
	uint32_t used = rec->x.copy_used;
	rec->x.copy_used = 0;
	rec->opts |= SALSA64_REC_DBG_NO_COPY_FREE;
	uint32_t o1 = salsa64_rec_x_copy_alloc(rec, 24), o2 = salsa64_rec_x_copy_alloc(rec, 24);
	CHECK_EQ(o1, 1);
	CHECK_EQ(o2, 25);
	salsa64_rec_x_copy_free(rec, o1, 24);
	CHECK_EQ(salsa64_rec_x_copy_alloc(rec, 24), 49);  /* no recicla */
	rec->x.copy_used = SALSA64_REC_X_COPY_OLD_BYTES - 16;
	CHECK_EQ(salsa64_rec_x_copy_alloc(rec, 32), 0);
	CHECK_EQ(salsa64_rec_x_copy_alloc(rec, 16), SALSA64_REC_X_COPY_OLD_BYTES - 16 + 1);
	rec->opts &= ~SALSA64_REC_DBG_NO_COPY_FREE;
	rec->x.copy_used = used;
}

/* Un bloque mapeado reescrito con los mismos bytes se desprende y se vuelve a
 * enganchar sin compilar; con bytes distintos se compila otro y el
 * desprendido queda para cuando vuelvan los suyos. */
static void mapped_code_rewritten_with_same_bytes_is_reattached(void)
{
	struct salsa64_rec *rec = CPU->rec;
	map_code_page();
	t_wr32(M_PA + 0, ADDIU(T0, T0, 1));
	t_wr32(M_PA + 4, ADDIU(T0, T0, 1));
	t_wr32(M_PA + 8, JR(T1));
	t_wr32(M_PA + 12, NOP);
	R[T0] = 0;
	R[T1] = T_CODE;
	uint64_t compiled = rec ? rec->stats.compiled : 0, detached = rec ? rec->stats.detached : 0;
	uint64_t rekeyed = rec ? rec->stats.rekeyed : 0, killed = rec ? rec->stats.invalidated : 0;

	salsa64_cpu_set_pc(CPU, M_VA);
	t_step(4);
	CHECK_EQ(R[T0], 2);
	CHECK_EQ(CPU->pc, T_CODE);
	if (rec) {
		CHECK_EQ(rec->stats.compiled, compiled + 1);
		CHECK_EQ(rec->x.ndetached, 0);
	}

	/* Los mismos bytes: desprender y enganchar, no compilar. */
	t_wr32(M_PA + 0, ADDIU(T0, T0, 1));
	if (rec) {
		CHECK_EQ(rec->stats.detached, detached + 1);
		CHECK_EQ(rec->stats.invalidated, killed + 1);
		CHECK_EQ(rec->x.ndetached, 1);
	}
	salsa64_cpu_set_pc(CPU, M_VA);
	t_step(4);
	CHECK_EQ(R[T0], 4);
	if (rec) {
		CHECK_EQ(rec->stats.compiled, compiled + 1);
		CHECK_EQ(rec->stats.rekeyed, rekeyed + 1);
		CHECK_EQ(rec->x.ndetached, 0);
	}

	/* Otros bytes: el desprendido no sirve, se compila uno nuevo. */
	t_wr32(M_PA + 4, ADDIU(T0, T0, 2));
	salsa64_cpu_set_pc(CPU, M_VA);
	t_step(4);
	CHECK_EQ(R[T0], 7);
	if (rec) {
		CHECK_EQ(rec->stats.detached, detached + 2);
		CHECK_EQ(rec->stats.compiled, compiled + 2);
		CHECK_EQ(rec->stats.rekeyed, rekeyed + 1);
		CHECK_EQ(rec->x.ndetached, 1);
	}

	/* Vuelven los bytes de antes: el desprendido se engancha y el nuevo
	 * queda desprendido; después, recuperar ids los mata y libera. */
	t_wr32(M_PA + 4, ADDIU(T0, T0, 1));
	salsa64_cpu_set_pc(CPU, M_VA);
	t_step(4);
	CHECK_EQ(R[T0], 9);
	if (rec) {
		CHECK_EQ(rec->stats.compiled, compiled + 2);
		CHECK_EQ(rec->stats.rekeyed, rekeyed + 2);
		CHECK_EQ(rec->x.ndetached, 1);
		uint64_t before = rec->stats.invalidated;
		CHECK_EQ(salsa64_rec_x_reclaim(rec), 1);
		CHECK_EQ(rec->x.ndetached, 0);
		CHECK_EQ(rec->stats.invalidated, before + 1);
		CHECK(rec->x.copy_free[0] != 0);  /* la ranura de 16 bytes volvió a la lista de 64 */
	}
	salsa64_cpu_set_pc(CPU, M_VA);
	t_step(4);
	CHECK_EQ(R[T0], 11);
}

/* Con el reparto viejo lleno no hay copia: la escritura mata y se recompila
 * (lo de antes del 2026-10-28). */
static void old_pool_full_retranslates(void)
{
	struct salsa64_rec *rec = CPU->rec;
	if (!rec)
		return;
	map_code_page();
	t_wr32(M_PA + 0, ADDIU(T0, T0, 1));
	t_wr32(M_PA + 4, JR(T1));
	t_wr32(M_PA + 8, NOP);
	R[T0] = 0;
	R[T1] = T_CODE;
	rec->opts |= SALSA64_REC_DBG_NO_COPY_FREE;
	rec->x.copy_used = SALSA64_REC_X_COPY_OLD_BYTES;  /* lleno */
	uint64_t compiled = rec->stats.compiled, detached = rec->stats.detached;
	salsa64_cpu_set_pc(CPU, M_VA);
	t_step(3);
	CHECK_EQ(R[T0], 1);
	CHECK_EQ(rec->stats.compiled, compiled + 1);
	t_wr32(M_PA + 0, ADDIU(T0, T0, 1));
	salsa64_cpu_set_pc(CPU, M_VA);
	t_step(3);
	CHECK_EQ(R[T0], 2);
	CHECK_EQ(rec->stats.detached, detached);
	CHECK_EQ(rec->stats.compiled, compiled + 2);
	rec->opts &= ~SALSA64_REC_DBG_NO_COPY_FREE;
	rec->x.copy_used = 0;
	salsa64_rec_flush(rec);  /* dejar el pool como lo encuentra la prueba siguiente */
}

void t_suite_rec_copy(void)
{
	RUN(copy_slots_are_reused_and_bounded);
	RUN(mapped_code_rewritten_with_same_bytes_is_reattached);
	RUN(old_pool_full_retranslates);
}
