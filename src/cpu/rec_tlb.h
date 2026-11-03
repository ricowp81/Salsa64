/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — recompilador: código y datos fuera de KSEG0 (KSEG1 y mapeados
 * por la TLB).
 *
 * Interno del recompilador (rec.c, rec_emit.c, rec_tlb.c). Diseño en
 * docs/rec-ppe.md, «Código fuera de KSEG0» y «Datos mapeados». Resumen:
 *   - Los bloques de KSEG0 van en rec->map[] por dirección física. Los demás
 *     van aquí, con clave (vpc, física): xhash por vpc y, si el código está
 *     en la RDRAM, xpage por página física para invalidarlos al escribir.
 *     Ninguno cruza una página de 4 KB. El código puede estar en la RDRAM o
 *     en la ROM del cartucho (solo lectura: nunca se invalida).
 *   - ktlb[] guarda, por página virtual de 4 KB (bits 25–12; los bits 31–26
 *     van en la etiqueta), la traducción en modo kernel con su bit D: la
 *     usan la entrada de un bloque mapeado (compara la palabra entera con
 *     la que tenía al compilarse; si cambió, sale al despachador) y el
 *     camino rápido de las cargas y almacenamientos a direcciones mapeadas
 *     (etiqueta, validez y D en el código generado; rec_emit.c). La llenan
 *     el despachador (código) y el camino lento de los accesos (datos).
 *   - Escribir la TLB no toca los bloques: antes de correr código (y en
 *     rec_h_exec tras cada COP0), salsa64_rec_x_sync borra de ktlb[] las
 *     páginas de las entradas de la TLB que cambiaron (compara las 32 con
 *     una instantánea) y, si cambiaron el ASID o ERL, vuelve a traducir
 *     todas las anotadas.
 */
#ifndef SALSA64_CPU_REC_TLB_H
#define SALSA64_CPU_REC_TLB_H

#include "core/bus.h"
#include "core/types.h"
#include "cpu/vr4300.h"

struct salsa64_rec;

#define SALSA64_REC_X_IDS       65536u    /* ids de bloque de 16 bits (rec.h: MAX_BLOCKS + 1) */
#define SALSA64_REC_XHASH       16384     /* cabezas de xhash */
#define SALSA64_REC_VT_PAGES    16384     /* entradas de ktlb[]: bits 25–12 de la virtual */
#define SALSA64_REC_PAGE_SHIFT  12
/* Copias de los bytes MIPS de los bloques mapeados (para desprenderlos y
 * engancharlos donde vuelva su página, o cuando se reescriben con los mismos
 * bytes). Ranuras de 64, 128 y 256 bytes que se liberan al morir el bloque;
 * hasta el 2026-10-28 era un reparto lineal de 1 MB que no se recuperaba
 * hasta un vaciado completo: Perfect Dark lo llenaba en el primer minuto y
 * desde ahí cada overlay recargado se retraducía entero (110 656
 * retraducciones idénticas, el 77 % de sus compilaciones, en 318 s).
 * SALSA64_REC_DBG_NO_COPY_FREE (jit-copias-viejas) repite el reparto viejo
 * con su tope, para el A/B con el mismo binario. */
#define SALSA64_REC_X_COPY_BYTES     (4u << 20)
#define SALSA64_REC_X_COPY_OLD_BYTES (1u << 20)
#define SALSA64_REC_X_COPY_CLASSES   3           /* 64 << clase bytes */
/* Tope de bloques desprendidos a la vez (cada uno retiene su id, su copia y
 * su código): por encima, invalidar mata como antes. Perfect Dark desprende
 * hasta ~4500 por campo al recargar un overlay. */
#define SALSA64_REC_X_MAX_DETACHED   16384u

/* Estado de un bloque mapeado (x.state[]). */
enum {
	SALSA64_REC_X_LIVE,      /* o no es mapeado */
	SALSA64_REC_X_DETACHED,  /* su página se reescribió: fuera de flags y xpage, sin código ni enlaces; en xhash con
				  * su copia, para engancharlo donde vuelva la página con los mismos bytes */
};

/* Valor de ktlb[]: etiqueta (bits 31–26 de la virtual), válida, D (se puede
 * escribir) y la página física (pa < 2^29: 17 bits). 0 = sin traducción. */
#define SALSA64_REC_VT_VALID    0x02000000u
#define SALSA64_REC_VT_DIRTY    0x01000000u
#define SALSA64_REC_VT_TAG(va)  (((uint32_t)(va) >> 26) << 26)
#define SALSA64_REC_VT_PFN_MASK 0x0001FFFFu

/* Dónde está el código de un bloque. */
enum {
	SALSA64_REC_KSEG0,     /* rec->map[], sin traducción */
	SALSA64_REC_KSEG1,     /* xhash, sin traducción */
	SALSA64_REC_MAPPED,    /* xhash; revisa ktlb[] al entrar */
	SALSA64_REC_SEG_NONE,  /* no se compila: lo corre el intérprete */
};

/* 8 bytes: exit_jr indexa xb[] con << 3 y lee vpc en +0. */
struct salsa64_rec_xblock {
	uint32_t vpc;        /* dirección virtual (extensión de signo de 32 bits), también en KSEG0 */
	uint16_t hnext;      /* siguiente en la misma cabeza de xhash */
	uint16_t pnext;      /* siguiente en la misma página de xpage */
};

/* Instantánea de una entrada de la TLB del CPU (lo que decide su traducción). */
struct salsa64_rec_tlb_snap {
	uint64_t entry_hi;
	uint32_t lo0, lo1, page_mask;
};

/* Va dentro de struct salsa64_rec, justo antes de flags: ktlb[] es lo último
 * y el código generado la lee con un desplazamiento negativo desde
 * code_flags (r28). */
struct salsa64_rec_x {
	uint32_t count;          /* bloques vivos en xhash */
	uint32_t tlb_writes;     /* cpu->tlb_writes, ASID y ERL con que se revisó ktlb[] */
	uint8_t asid;
	bool erl;
	uint32_t nlive;          /* entradas de live[] */
	uint32_t copy_used;      /* bytes de copy[] repartidos alguna vez (las ranuras libres se reusan antes) */
	uint32_t copy_free[SALSA64_REC_X_COPY_CLASSES];  /* cabeza (desplazamiento + 1) de las ranuras libres de cada clase, 0 = ninguna */
	uint32_t ndetached;      /* bloques en SALSA64_REC_X_DETACHED */
	struct salsa64_rec_xblock xb[SALSA64_REC_X_IDS];
	uint16_t xhash[SALSA64_REC_XHASH];
	uint16_t xpage[SALSA64_RDRAM_MAX >> SALSA64_REC_PAGE_SHIFT];
	struct salsa64_rec_tlb_snap snap[SALSA64_TLB_ENTRIES];
	/* Bloques mapeados en la RDRAM: copia de sus bytes MIPS (copy_off + 1 en
	 * copy[]; 0 = sin copia), su código mientras están desprendidos y su
	 * estado. Un bloque vivo depende solo de su vpc y de esos bytes. */
	uint32_t copy_off[SALSA64_REC_X_IDS];
	uint32_t saved_code[SALSA64_REC_X_IDS];
	uint8_t state[SALSA64_REC_X_IDS];
	uint8_t copy[SALSA64_REC_X_COPY_BYTES];
	uint16_t live[SALSA64_REC_VT_PAGES];  /* entradas de ktlb[] distintas de 0 */
	uint32_t ktlb[SALSA64_REC_VT_PAGES];
};

/* Entrada de ktlb[] de `va`, y su valor con la página traducida a la de `pa`
 * (pa < 2^29). */
static inline unsigned salsa64_rec_vt_slot(uint64_t va)
{
	return ((uint32_t)va >> SALSA64_REC_PAGE_SHIFT) & (SALSA64_REC_VT_PAGES - 1);
}

static inline uint32_t salsa64_rec_vt_value(uint64_t va, uint32_t pa, bool dirty)
{
	return SALSA64_REC_VT_TAG(va) | SALSA64_REC_VT_VALID | (dirty ? SALSA64_REC_VT_DIRTY : 0) |
	       pa >> SALSA64_REC_PAGE_SHIFT;
}

/* La instrucción en la dirección física `pa` (RDRAM o ROM). */
uint32_t salsa64_rec_fetch(const struct salsa64_rec *rec, uint32_t pa);
/* Hasta dónde puede llegar un bloque que empieza en `pa` (fin de la RDRAM o
 * de la ROM; fuera de KSEG0, fin de la página de 4 KB). */
uint32_t salsa64_rec_code_end(const struct salsa64_rec *rec, uint32_t pa, unsigned seg);

void salsa64_rec_x_init(struct salsa64_rec *rec);
void salsa64_rec_x_flush(struct salsa64_rec *rec);
/* Antes de correr código generado (KSEG0 enlaza con mapeados) y tras cada
 * instrucción de COP0 que corre el intérprete desde el código. */
void salsa64_rec_x_sync(struct salsa64_rec *rec);
/* El código generado escribió la entrada n de la TLB (rec.c, rec_h_tlbw):
 * ktlb[] al día solo para ella. */
void salsa64_rec_x_tlb_written(struct salsa64_rec *rec, unsigned n);
/* Segmento y física del código en `pc` (modo kernel), o SALSA64_REC_SEG_NONE. */
unsigned salsa64_rec_seg(struct salsa64_rec *rec, uint64_t pc, uint32_t *pa);
/* El camino lento acaba de traducir con éxito una carga o un almacenamiento
 * a `va` (modo kernel): anota la página en ktlb[] si es mapeada y se
 * traduce sin excepción. */
void salsa64_rec_x_data_fill(struct salsa64_rec *rec, uint64_t va);
/* El bloque fuera de KSEG0 de (pc, pa), o 0. */
unsigned salsa64_rec_x_find(struct salsa64_rec *rec, uint64_t pc, uint32_t pa);
/* El primer bloque vivo, con código, de `vpc` (cualquier física), o 0: para
 * resolver sucesores del perfil (rec.c). No reordena la cadena. */
unsigned salsa64_rec_x_find_vpc(const struct salsa64_rec *rec, uint64_t vpc);
/* No hay bloque de (pc, pa) mapeado: si hay uno de pc (vivo en otra física,
 * o desprendido) cuya copia de bytes coincide con lo que hay en pa (la
 * página volvió en otro marco), lo engancha en pa y lo devuelve; si no, 0. */
unsigned salsa64_rec_x_rekey(struct salsa64_rec *rec, uint64_t pc, uint32_t pa);
/* El bloque mapeado `id` en la RDRAM se invalidó (su página se reescribió):
 * si tiene copia, lo desprende en vez de matarlo; false si hay que matarlo. */
bool salsa64_rec_x_detach(struct salsa64_rec *rec, unsigned id);
/* Bloque nuevo `id` (de cualquier segmento): anota su vpc y, fuera de KSEG0,
 * lo agrega a xhash y xpage. */
void salsa64_rec_x_add(struct salsa64_rec *rec, unsigned id, uint64_t pc, uint32_t pa, unsigned seg, bool code);
/* Lo saca de xhash y xpage; false si es de KSEG0 (está en map[]). */
bool salsa64_rec_x_remove(struct salsa64_rec *rec, unsigned id);
/* Mata los bloques fuera de KSEG0 con código en la RDRAM [pa, end). */
void salsa64_rec_x_invalidate(struct salsa64_rec *rec, uint32_t pa, uint32_t end);
/* Mata los bloques fuera de KSEG0 que contienen `vpc`. */
void salsa64_rec_x_kill_at(struct salsa64_rec *rec, uint64_t vpc);
/* Sin ids libres: mata los bloques desprendidos (que retienen el suyo) para
 * no vaciar la caché entera; devuelve cuántos. */
unsigned salsa64_rec_x_reclaim(struct salsa64_rec *rec);
/* Ranuras de copy[] (para las pruebas; rec_tlb.c las usa por dentro):
 * reserva una para `bytes` y devuelve su desplazamiento + 1, o 0 si no hay;
 * la libera. */
uint32_t salsa64_rec_x_copy_alloc(struct salsa64_rec *rec, uint32_t bytes);
void salsa64_rec_x_copy_free(struct salsa64_rec *rec, uint32_t off1, uint32_t bytes);

/* rec.c, para rec_tlb.c. */
void salsa64_rec_kill_block(struct salsa64_rec *rec, unsigned id);
void salsa64_rec_unlink_block(struct salsa64_rec *rec, unsigned id);
/* El bloque mapeado `id` pasa a la física `pa` (banderas de código y la
 * palabra de ktlb[], `vt`, que compara al entrar); `code` es su entrada
 * (la de ahora, o la guardada si estaba desprendido) y `had_flags` si sus
 * banderas de código siguen puestas en la física vieja. False si su código
 * no empieza como se espera. */
bool salsa64_rec_block_rekey(struct salsa64_rec *rec, unsigned id, uint32_t pa, uint32_t vt, uint32_t code,
			     bool had_flags);
/* Deja al bloque sin enlaces entrantes, sin banderas de código y con code = 0
 * (para desprenderlo); rec_tlb.c lo saca de xpage. */
void salsa64_rec_block_detach(struct salsa64_rec *rec, unsigned id);

#endif
