/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — recompilador: código y datos fuera de KSEG0 (KSEG1 y mapeados
 * por la TLB). Ver rec_tlb.h y docs/rec-ppe.md, «Código fuera de KSEG0».
 */
#include "cpu/rec_tlb.h"

#include <stddef.h>
#include <string.h>

#include "cpu/rec.h"
#include "cpu/vr4300_internal.h"

_Static_assert(offsetof(struct salsa64_rec, flags) ==
		       offsetof(struct salsa64_rec, x) + offsetof(struct salsa64_rec_x, ktlb) +
			       SALSA64_REC_VT_PAGES * sizeof(uint32_t) &&
		       SALSA64_REC_VT_PAGES * sizeof(uint32_t) <= 65536,
	       "el código generado lee ktlb[] con un desplazamiento negativo de 16 bits (addis) desde flags (r28)");
_Static_assert(sizeof(struct salsa64_rec_xblock) == 8, "exit_jr indexa xb[] con << 3");
_Static_assert(SALSA64_REC_MAX_BLOCKS + 1 <= SALSA64_REC_X_IDS, "ids de 16 bits en xb[], xhash y xpage");
_Static_assert(SALSA64_REC_XHASH == 16384, "exit_jr calcula la cabeza de xhash con rlwinm 31, 17, 30");
_Static_assert(SALSA64_REC_VT_PAGES == 16384, "rec_emit.c saca la entrada de ktlb[] con rlwinm 22, 16, 29");

#define PAGE_MASK ((1u << SALSA64_REC_PAGE_SHIFT) - 1)

/* ---- Dónde está el código --------------------------------------------------------- */

/* Código compilable en la dirección física `pa`: RDRAM, o la ROM del
 * cartucho (solo lectura) salvo el ISViewer, que se escribe. */
static bool code_pa(const struct salsa64_rec *rec, uint32_t pa)
{
	const struct salsa64_bus *bus = rec->cpu->bus;
	if (pa < rec->rdram_size)
		return true;
	if (pa < SALSA64_CART_ROM_ADDR || pa >= SALSA64_CART_ROM_END)
		return false;
	return !bus->isv || pa - SALSA64_ISV_ADDR >= SALSA64_ISV_SIZE;
}

uint32_t salsa64_rec_fetch(const struct salsa64_rec *rec, uint32_t pa)
{
	if (pa < rec->rdram_size)
		return salsa64_rd32(rec->cpu->rdram + pa);
	return salsa64_bus_read32(rec->cpu->bus, pa);  /* ROM: lectura sin efectos */
}

uint32_t salsa64_rec_code_end(const struct salsa64_rec *rec, uint32_t pa, unsigned seg)
{
	/* El ISViewer empieza en una página: el límite de página lo deja fuera. */
	uint32_t end = pa < rec->rdram_size ? rec->rdram_size : SALSA64_CART_ROM_END;
	if (seg != SALSA64_REC_KSEG0 && (pa | PAGE_MASK) + 1 < end)
		end = (pa | PAGE_MASK) + 1;
	return end;
}

/* ---- TLB del recompilador (ktlb) -------------------------------------------------- */

static void live_compact(struct salsa64_rec_x *x);

static void vt_set(struct salsa64_rec *rec, uint64_t va, uint32_t pa, bool dirty)
{
	struct salsa64_rec_x *x = &rec->x;
	unsigned s = salsa64_rec_vt_slot(va);
	if (!x->ktlb[s]) {
		if (x->nlive == SALSA64_REC_VT_PAGES)  /* entradas borradas que nadie compactó (x_tlb_written) */
			live_compact(x);
		x->live[x->nlive++] = (uint16_t)s;
	}
	x->ktlb[s] = salsa64_rec_vt_value(va, pa, dirty);
}

/* ¿ktlb[] tiene la página de `va`? Entonces su física. */
static bool vt_hit(const struct salsa64_rec *rec, uint64_t va, uint32_t *pa)
{
	uint32_t v = rec->x.ktlb[salsa64_rec_vt_slot(va)];
	if (!(v & SALSA64_REC_VT_VALID) || (v & SALSA64_REC_VT_TAG(~0u)) != SALSA64_REC_VT_TAG(va))
		return false;
	*pa = (v & SALSA64_REC_VT_PFN_MASK) << SALSA64_REC_PAGE_SHIFT | ((uint32_t)va & PAGE_MASK);
	return true;
}

/* Física de la instrucción mapeada en `va`: la de ktlb[] o, si la página no
 * está, la de la TLB del CPU (y queda en ktlb[]). */
static bool vt_translate(struct salsa64_rec *rec, uint64_t va, uint32_t *pa)
{
	if (vt_hit(rec, va, pa))
		return true;
	bool dirty;
	rec->stats.vt_probes++;
	if (!salsa64_vr4300_kernel_probe(rec->cpu, va, pa, &dirty) || !code_pa(rec, *pa))
		return false;
	vt_set(rec, va, *pa, dirty);
	return true;
}

void salsa64_rec_x_data_fill(struct salsa64_rec *rec, uint64_t va)
{
	uint32_t a = (uint32_t)va, pa;
	bool dirty;
	if ((int64_t)va != (int32_t)va || (a >= 0x80000000u && a < 0xC0000000u))
		return;  /* KSEG0/KSEG1 no pasan por aquí; direcciones de 64 bits no se anotan */
	if (rec->x.ktlb[salsa64_rec_vt_slot(va)] & SALSA64_REC_VT_VALID && vt_hit(rec, va, &pa))
		return;
	rec->stats.vt_probes++;
	if (salsa64_vr4300_kernel_probe(rec->cpu, va, &pa, &dirty))
		vt_set(rec, va, pa, dirty);
}

static void snap_take(struct salsa64_rec_tlb_snap *s, const struct salsa64_tlb_entry *e)
{
	s->entry_hi = e->entry_hi;
	s->lo0 = e->entry_lo[0];
	s->lo1 = e->entry_lo[1];
	s->page_mask = e->page_mask;
}

static bool snap_same(const struct salsa64_rec_tlb_snap *s, const struct salsa64_tlb_entry *e)
{
	return s->entry_hi == e->entry_hi && s->lo0 == e->entry_lo[0] && s->lo1 == e->entry_lo[1] &&
	       s->page_mask == e->page_mask;
}

/* Borra de ktlb[] las páginas que traduce (o traducía) una entrada de la
 * TLB: el par de páginas de su VPN2. Borrar de más es seguro (se vuelve a
 * traducir). */
static void vt_clear_entry(struct salsa64_rec_x *x, uint64_t entry_hi, uint32_t page_mask)
{
	uint32_t pair = (page_mask | 0x1FFFu) + 1;  /* bytes del par de páginas (cop0.c: odd_bit es la mitad) */
	uint32_t first = ((uint32_t)entry_hi & ~(pair - 1)) >> SALSA64_REC_PAGE_SHIFT;
	uint32_t pages = pair >> SALSA64_REC_PAGE_SHIFT;
	if (pages > SALSA64_REC_VT_PAGES)
		pages = SALSA64_REC_VT_PAGES;
	for (uint32_t k = 0; k < pages; k++)
		x->ktlb[(first + k) & (SALSA64_REC_VT_PAGES - 1)] = 0;
}

/* Quita de live[] las entradas borradas. */
static void live_compact(struct salsa64_rec_x *x)
{
	uint32_t keep = 0;
	for (uint32_t k = 0; k < x->nlive; k++)
		if (x->ktlb[x->live[k]])
			x->live[keep++] = x->live[k];
	x->nlive = keep;
}

/* Anota en ktlb[] las páginas de una entrada recién escrita, con la misma
 * búsqueda que el intérprete (si otra entrada las tapa, manda esa): así el
 * bloque o el acceso que viene a la página nueva no sale a C a traducirla.
 * GoldenEye trae ~400 páginas por campo con el refill; solo pares chicos
 * (hasta 16 páginas), los grandes se escriben una vez. */
static void vt_prefill_entry(struct salsa64_rec *rec, const struct salsa64_tlb_entry *e)
{
	uint32_t pair = (e->page_mask | 0x1FFFu) + 1, pages = pair >> SALSA64_REC_PAGE_SHIFT;
	uint32_t base = (uint32_t)e->entry_hi & ~(pair - 1);
	if (pages > 16 || (int64_t)e->entry_hi != (int32_t)(uint32_t)e->entry_hi)
		return;
	for (uint32_t k = 0; k < pages; k++) {
		uint64_t va = (uint64_t)(int64_t)(int32_t)(base + (k << SALSA64_REC_PAGE_SHIFT));
		uint32_t pa;
		bool dirty;
		rec->stats.vt_probes++;
		if (salsa64_vr4300_kernel_probe(rec->cpu, va, &pa, &dirty))
			vt_set(rec, va, pa, dirty);
	}
}

/* Si la TLB del CPU cambió (TLBWI, TLBWR, reset), se borran de ktlb[] las
 * páginas de las entradas que cambiaron (antes y después); si cambiaron el
 * ASID o ERL, cada página anotada se vuelve a traducir y se borran las que
 * ya no dan lo mismo. Los bloques se quedan: al entrar ven que su página no
 * está y salen al despachador, que la traduce de nuevo. Todo cambio de la
 * TLB pasa por C: COP0 termina el bloque (rec_h_exec llama aquí antes de
 * seguir) y ERET y las excepciones salen con redirect al despachador. */
void salsa64_rec_x_sync(struct salsa64_rec *rec)
{
	struct salsa64_cpu *cpu = rec->cpu;
	struct salsa64_rec_x *x = &rec->x;
	uint8_t asid = (uint8_t)cpu->entry_hi;
	bool erl = (cpu->status & SALSA64_ST_ERL) != 0;
	if (cpu->tlb_writes == x->tlb_writes && asid == x->asid && erl == x->erl)
		return;
	struct salsa64_rec_prof *prof = rec->prof;
	uint64_t t0 = prof ? salsa64_rec_prof_now() : 0;
	bool full = asid != x->asid || erl != x->erl || SALSA64_REC_OLD(rec, SALSA64_REC_DBG_NO_TSYNC);
	if (cpu->tlb_writes != x->tlb_writes && rec->fastmem)  /* fastmem: ¿alguna entrada nueva choca con la ventana? */
		for (unsigned n = 0; n < SALSA64_TLB_ENTRIES; n++)
			if (!snap_same(&x->snap[n], &cpu->tlb[n]) && salsa64_rec_fastmem_collides(rec, &cpu->tlb[n])) {
				salsa64_rec_fastmem_off(rec);
				break;
			}
	x->tlb_writes = cpu->tlb_writes;
	x->asid = asid;
	x->erl = erl;
	if (full) {
		uint32_t keep = 0;
		for (uint32_t k = 0; k < x->nlive; k++) {
			unsigned s = x->live[k];
			uint32_t v = x->ktlb[s], pa;
			uint64_t va = (uint64_t)(int64_t)(int32_t)((v & SALSA64_REC_VT_TAG(~0u)) |
								   (uint32_t)s << SALSA64_REC_PAGE_SHIFT);
			bool dirty;
			rec->stats.vt_probes++;
			if (salsa64_vr4300_kernel_probe(cpu, va, &pa, &dirty) && salsa64_rec_vt_value(va, pa, dirty) == v)
				x->live[keep++] = (uint16_t)s;
			else
				x->ktlb[s] = 0;
		}
		x->nlive = keep;
		for (unsigned n = 0; n < SALSA64_TLB_ENTRIES; n++)
			snap_take(&x->snap[n], &cpu->tlb[n]);
	} else {
		unsigned changed[SALSA64_TLB_ENTRIES], nchanged = 0;
		for (unsigned n = 0; n < SALSA64_TLB_ENTRIES; n++) {
			const struct salsa64_tlb_entry *e = &cpu->tlb[n];
			if (snap_same(&x->snap[n], e))
				continue;
			vt_clear_entry(x, x->snap[n].entry_hi, x->snap[n].page_mask);
			vt_clear_entry(x, e->entry_hi, e->page_mask);
			snap_take(&x->snap[n], e);
			changed[nchanged++] = n;
		}
		live_compact(x);
		for (unsigned k = 0; k < nchanged; k++)  /* con todo borrado y live[] al día */
			vt_prefill_entry(rec, &cpu->tlb[changed[k]]);
	}
	if (prof) {
		prof->t[SALSA64_PROF_XSYNC] += salsa64_rec_prof_now() - t0;
		prof->n[SALSA64_PROF_XSYNC]++;
	}
}

/* El código generado acaba de escribir la entrada n de la TLB (rec.c,
 * rec_h_tlbw, en vez del intérprete): lo que salsa64_rec_x_sync haría al
 * comparar las 32 entradas con la instantánea, solo con esa y sin compactar
 * live[] (vt_set lo hace si se llena). ASID y ERL no cambian con una
 * escritura. GoldenEye hace ~400 por campo con el refill. */
void salsa64_rec_x_tlb_written(struct salsa64_rec *rec, unsigned n)
{
	struct salsa64_rec_x *x = &rec->x;
	const struct salsa64_tlb_entry *e = &rec->cpu->tlb[n & 31];
	struct salsa64_rec_prof *prof = rec->prof;
	uint64_t t0 = prof ? salsa64_rec_prof_now() : 0;
	if (!snap_same(&x->snap[n & 31], e)) {
		vt_clear_entry(x, x->snap[n & 31].entry_hi, x->snap[n & 31].page_mask);
		vt_clear_entry(x, e->entry_hi, e->page_mask);
		snap_take(&x->snap[n & 31], e);
		vt_prefill_entry(rec, e);
		if (rec->fastmem && salsa64_rec_fastmem_collides(rec, e))
			salsa64_rec_fastmem_off(rec);
	}
	x->tlb_writes = rec->cpu->tlb_writes;
	if (prof) {
		prof->t[SALSA64_PROF_XSYNC] += salsa64_rec_prof_now() - t0;
		prof->n[SALSA64_PROF_XSYNC]++;
	}
}

unsigned salsa64_rec_seg(struct salsa64_rec *rec, uint64_t pc, uint32_t *pa)
{
	if (pc & 3)
		return SALSA64_REC_SEG_NONE;
	if ((pc >> 29) == 0x7FFFFFFFCull) {  /* KSEG0 */
		*pa = (uint32_t)pc & 0x1FFFFFFF;
		return *pa < rec->rdram_size ? SALSA64_REC_KSEG0 : SALSA64_REC_SEG_NONE;
	}
	if ((int64_t)pc != (int32_t)pc)
		return SALSA64_REC_SEG_NONE;
	if ((pc >> 29) == 0x7FFFFFFFDull) {  /* KSEG1 */
		*pa = (uint32_t)pc & 0x1FFFFFFF;
		return code_pa(rec, *pa) ? SALSA64_REC_KSEG1 : SALSA64_REC_SEG_NONE;
	}
	return vt_translate(rec, pc, pa) ? SALSA64_REC_MAPPED : SALSA64_REC_SEG_NONE;
}

/* ---- Bloques fuera de KSEG0 -------------------------------------------------------- */

static unsigned xhash_slot(uint64_t vpc)
{
	return ((uint32_t)vpc >> 2) & (SALSA64_REC_XHASH - 1);
}

void salsa64_rec_x_init(struct salsa64_rec *rec)
{
	struct salsa64_cpu *cpu = rec->cpu;
	rec->x.tlb_writes = cpu->tlb_writes;
	rec->x.asid = (uint8_t)cpu->entry_hi;
	rec->x.erl = (cpu->status & SALSA64_ST_ERL) != 0;
	for (unsigned n = 0; n < SALSA64_TLB_ENTRIES; n++)
		snap_take(&rec->x.snap[n], &cpu->tlb[n]);
}

void salsa64_rec_x_flush(struct salsa64_rec *rec)
{
	memset(rec->x.xhash, 0, sizeof rec->x.xhash);
	memset(rec->x.xpage, 0, sizeof rec->x.xpage);
	memset(rec->x.state, 0, sizeof rec->x.state);
	memset(rec->x.copy_free, 0, sizeof rec->x.copy_free);
	rec->x.copy_used = 0;
	rec->x.ndetached = 0;
	rec->x.count = 0;
}

unsigned salsa64_rec_x_reclaim(struct salsa64_rec *rec)
{
	struct salsa64_rec_x *x = &rec->x;
	unsigned killed = 0;
	for (unsigned id = 1; x->ndetached && id < rec->next_block; id++) {
		if (x->state[id] != SALSA64_REC_X_DETACHED)
			continue;
		salsa64_rec_kill_block(rec, id);
		killed++;
	}
	return killed;
}

/* ---- Copias de bytes (ranuras de 64, 128 y 256 bytes) ------------------------------ */

static unsigned copy_class(uint32_t bytes)
{
	return bytes <= 64 ? 0 : bytes <= 128 ? 1 : 2;  /* MAX_BLOCK_BYTES = 256 */
}

uint32_t salsa64_rec_x_copy_alloc(struct salsa64_rec *rec, uint32_t bytes)
{
	struct salsa64_rec_x *x = &rec->x;
	if (SALSA64_REC_OLD(rec, SALSA64_REC_DBG_NO_COPY_FREE)) {  /* lineal y sin liberar, como hasta 2026-10-28 */
		if (x->copy_used + bytes > SALSA64_REC_X_COPY_OLD_BYTES)
			return 0;
		x->copy_used += bytes;
		return x->copy_used - bytes + 1;
	}
	unsigned c = copy_class(bytes);
	uint32_t size = 64u << c;
	if (x->copy_free[c]) {
		uint32_t off1 = x->copy_free[c];
		memcpy(&x->copy_free[c], x->copy + off1 - 1, sizeof x->copy_free[c]);  /* la ranura libre guarda la siguiente */
		return off1;
	}
	if (x->copy_used + size > SALSA64_REC_X_COPY_BYTES)
		return 0;
	x->copy_used += size;
	return x->copy_used - size + 1;
}

void salsa64_rec_x_copy_free(struct salsa64_rec *rec, uint32_t off1, uint32_t bytes)
{
	struct salsa64_rec_x *x = &rec->x;
	if (!off1 || SALSA64_REC_OLD(rec, SALSA64_REC_DBG_NO_COPY_FREE))
		return;
	unsigned c = copy_class(bytes);
	memcpy(x->copy + off1 - 1, &x->copy_free[c], sizeof x->copy_free[c]);
	x->copy_free[c] = off1;
}

static void copy_release(struct salsa64_rec *rec, unsigned id)
{
	struct salsa64_rec_x *x = &rec->x;
	salsa64_rec_x_copy_free(rec, x->copy_off[id], rec->blocks[id].bytes);
	x->copy_off[id] = 0;
}

void salsa64_rec_x_add(struct salsa64_rec *rec, unsigned id, uint64_t pc, uint32_t pa, unsigned seg, bool code)
{
	struct salsa64_rec_x *x = &rec->x;
	struct salsa64_rec_xblock *b = &x->xb[id];
	b->vpc = (uint32_t)pc;
	if (seg == SALSA64_REC_KSEG0)
		return;
	uint16_t *h = &x->xhash[xhash_slot(pc)];
	b->hnext = *h;
	*h = (uint16_t)id;
	b->pnext = 0;
	x->state[id] = SALSA64_REC_X_LIVE;
	x->copy_off[id] = 0;
	if (pa < rec->rdram_size) {
		uint16_t *p = &x->xpage[pa >> SALSA64_REC_PAGE_SHIFT];
		b->pnext = *p;
		*p = (uint16_t)id;
		/* La copia de sus bytes, para engancharlo donde vuelva su página
		 * (o cuando lo reescriban con lo mismo). */
		uint32_t bytes = rec->blocks[id].bytes;
		if (code && seg == SALSA64_REC_MAPPED) {
			x->copy_off[id] = salsa64_rec_x_copy_alloc(rec, bytes);
			if (x->copy_off[id])
				memcpy(x->copy + x->copy_off[id] - 1, rec->cpu->rdram + pa, bytes);
		}
	}
	x->count++;
	rec->stats.compiled_x += code;
}

static void xpage_remove(struct salsa64_rec_x *x, unsigned id, uint32_t pa)
{
	uint16_t *p = &x->xpage[pa >> SALSA64_REC_PAGE_SHIFT];
	while (*p != id)
		p = &x->xb[*p].pnext;
	*p = x->xb[id].pnext;
}

static void xpage_add(struct salsa64_rec_x *x, unsigned id, uint32_t pa)
{
	uint16_t *p = &x->xpage[pa >> SALSA64_REC_PAGE_SHIFT];
	x->xb[id].pnext = *p;
	*p = (uint16_t)id;
}

bool salsa64_rec_x_detach(struct salsa64_rec *rec, unsigned id)
{
	struct salsa64_rec_x *x = &rec->x;
	if (!x->copy_off[id] || x->state[id] != SALSA64_REC_X_LIVE || SALSA64_REC_OLD(rec, SALSA64_REC_DBG_NO_DETACH))
		return false;
	if (x->ndetached >= SALSA64_REC_X_MAX_DETACHED && !SALSA64_REC_OLD(rec, SALSA64_REC_DBG_NO_COPY_FREE))
		return false;  /* demasiados retenidos: matar, como antes */
	salsa64_rec_block_detach(rec, id);  /* enlaces, banderas, code = 0 */
	xpage_remove(x, id, rec->blocks[id].pa);
	x->state[id] = SALSA64_REC_X_DETACHED;
	x->ndetached++;
	rec->stats.detached++;
	return true;
}

bool salsa64_rec_x_remove(struct salsa64_rec *rec, unsigned id)
{
	struct salsa64_rec_x *x = &rec->x;
	struct salsa64_rec_xblock *b = &x->xb[id];
	if (b->vpc >> 29 == 4)
		return false;  /* KSEG0 */
	uint16_t *p = &x->xhash[xhash_slot(b->vpc)];
	while (*p != id)
		p = &x->xb[*p].hnext;
	*p = b->hnext;
	uint32_t pa = rec->blocks[id].pa;
	if (pa < rec->rdram_size && x->state[id] == SALSA64_REC_X_LIVE)
		xpage_remove(x, id, pa);
	if (x->state[id] == SALSA64_REC_X_DETACHED)
		x->ndetached--;
	x->state[id] = SALSA64_REC_X_LIVE;
	copy_release(rec, id);
	x->count--;
	return true;
}

/* Lo pasa al frente de su cabeza de xhash (exit_jr recorre la cadena y toma
 * el primero con la misma vpc).
 *
 * Los de la misma vpc con otra física (la TLB la remapeó) se desenlazan: si
 * no, cada enlace que llega a ellos saldría en su entrada todas las veces;
 * así vuelven a C una vez y se enlazan con el bloque que vale ahora. Un
 * bloque con enlaces pasó por aquí al frente después de enlazarse, así que
 * se lo encuentra antes que al de la otra física. */
unsigned salsa64_rec_x_find(struct salsa64_rec *rec, uint64_t pc, uint32_t pa)
{
	struct salsa64_rec_x *x = &rec->x;
	bool fr = (rec->cpu->status & SALSA64_ST_FR) != 0;
	uint16_t *head = &x->xhash[xhash_slot(pc)], *p = head;
	for (unsigned id = *p; id; p = &x->xb[id].hnext, id = *p) {
		if (x->xb[id].vpc != (uint32_t)pc || x->state[id] != SALSA64_REC_X_LIVE)
			continue;
		if (rec->blocks[id].pa != pa) {
			if (rec->blocks[id].links)
				salsa64_rec_unlink_block(rec, id);
			continue;
		}
		if (!salsa64_rec_block_fr_ok(&rec->blocks[id], fr))
			continue;  /* el gemelo del otro Status.FR: sigue en la cadena para cuando vuelva */
		if (p != head) {
			*p = x->xb[id].hnext;
			x->xb[id].hnext = *head;
			*head = (uint16_t)id;
		}
		return id;
	}
	return 0;
}

unsigned salsa64_rec_x_find_vpc(const struct salsa64_rec *rec, uint64_t vpc)
{
	const struct salsa64_rec_x *x = &rec->x;
	for (unsigned id = x->xhash[xhash_slot(vpc)]; id; id = x->xb[id].hnext)
		if (x->xb[id].vpc == (uint32_t)vpc && x->state[id] == SALSA64_REC_X_LIVE && rec->blocks[id].code)
			return id;
	return 0;
}

/* GoldenEye pagina su código bajo demanda, cada página vuelve en el marco
 * que toque y el DMA que trae otra página al marco invalida los bloques que
 * había: sin esto, cada (vpc, marco) era un bloque nuevo (93 000 en 128 s,
 * con 6 vaciados de la caché, cada uno con todo por recompilar). El código
 * de un bloque depende solo de su vpc y de sus bytes (la copia). */
unsigned salsa64_rec_x_rekey(struct salsa64_rec *rec, uint64_t pc, uint32_t pa)
{
	struct salsa64_rec_x *x = &rec->x;
	const uint8_t *rdram = rec->cpu->rdram;
	uint32_t vt = x->ktlb[salsa64_rec_vt_slot(pc)];
	bool fr = (rec->cpu->status & SALSA64_ST_FR) != 0;
	if (pa >= rec->rdram_size || (vt & ~SALSA64_REC_VT_DIRTY) != salsa64_rec_vt_value(pc, pa, false))
		return 0;
	uint16_t *head = &x->xhash[xhash_slot(pc)], *p = head;
	for (unsigned id = *p; id; p = &x->xb[id].hnext, id = *p) {
		struct salsa64_rec_block *b = &rec->blocks[id];
		bool detached = x->state[id] == SALSA64_REC_X_DETACHED;
		uint32_t old = b->pa, code = detached ? x->saved_code[id] : b->code;
		if (x->xb[id].vpc != (uint32_t)pc || !x->copy_off[id] || !code || pa + b->bytes > rec->rdram_size ||
		    !salsa64_rec_block_fr_ok(b, fr) || memcmp(x->copy + x->copy_off[id] - 1, rdram + pa, b->bytes) != 0)
			continue;
		if (!salsa64_rec_block_rekey(rec, id, pa, vt, code, !detached))
			continue;
		if (!detached)
			xpage_remove(x, id, old);
		else
			x->ndetached--;
		xpage_add(x, id, pa);
		x->state[id] = SALSA64_REC_X_LIVE;
		if (p != head) {
			*p = x->xb[id].hnext;
			x->xb[id].hnext = *head;
			*head = (uint16_t)id;
		}
		rec->stats.rekeyed++;
		return id;
	}
	return 0;
}

void salsa64_rec_x_invalidate(struct salsa64_rec *rec, uint32_t pa, uint32_t end)
{
	struct salsa64_rec_x *x = &rec->x;
	for (uint32_t p = pa >> SALSA64_REC_PAGE_SHIFT; x->count && p <= (end - 1) >> SALSA64_REC_PAGE_SHIFT; p++) {
		for (unsigned id = x->xpage[p], next; id; id = next) {
			next = x->xb[id].pnext;
			const struct salsa64_rec_block *b = &rec->blocks[id];
			if (b->pa < end && b->pa + b->bytes > pa) {
				if (rec->diag)
					salsa64_rec_diag_hit(rec, id, pa, end - pa);
				if (!salsa64_rec_x_detach(rec, id))
					salsa64_rec_kill_block(rec, id);
			}
		}
	}
}

void salsa64_rec_x_kill_at(struct salsa64_rec *rec, uint64_t vpc)
{
	struct salsa64_rec_x *x = &rec->x;
	uint32_t v = (uint32_t)vpc, page = v & ~PAGE_MASK;
	for (uint32_t k = 0; k < SALSA64_REC_MAX_INSNS && 4 * k <= v - page; k++) {
		uint32_t start = v - 4 * k;
		for (unsigned id = x->xhash[xhash_slot(start)], next; id; id = next) {
			next = x->xb[id].hnext;
			if (x->xb[id].vpc == start && start + rec->blocks[id].bytes > v) {
				if (rec->diag)
					salsa64_rec_diag_hit(rec, id, rec->blocks[id].pa + v - start, 4);
				salsa64_rec_kill_block(rec, id);
			}
		}
	}
}
