/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — el juego se quedó esperando. Ver cuelgue.h.
 */
#include "frontend/cuelgue.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/endian.h"

/* Texto con límite: cada pieza se agrega al final de `out`. */
struct tbuf {
	char *p;
	unsigned size, n;
};

static void tput(struct tbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void tput(struct tbuf *b, const char *fmt, ...)
{
	if (b->n + 1 >= b->size)
		return;
	va_list ap;
	va_start(ap, fmt);
	int k = vsnprintf(b->p + b->n, b->size - b->n, fmt, ap);
	va_end(ap);
	if (k > 0)
		b->n = (unsigned)k < b->size - b->n ? b->n + (unsigned)k : b->size - 1;
}

/* Ciclos que faltan para un evento ("nunca" si no está programado). */
static void tevent(struct tbuf *b, const char *name, const struct salsa64_n64 *n64, int ev)
{
	uint64_t w = n64->sched.when[ev];
	if (w == SALSA64_SCHED_NEVER)
		tput(b, " %s -", name);
	else
		tput(b, " %s %+lld", name, (long long)(w - n64->cpu.cycles));
}

static void tcode(struct tbuf *b, const struct salsa64_n64 *n64, uint64_t pc)
{
	uint32_t pa = (uint32_t)pc & 0x1FFFFFFFu;
	if (((uint32_t)pc & 0xC0000000u) != 0x80000000u || pa < 16 || pa + 32 > n64->bus.rdram_size) {
		tput(b, "  código: pc fuera de KSEG0/KSEG1\n");
		return;
	}
	tput(b, "  código en pc-16..pc+28:");
	for (uint32_t a = pa - 16; a < pa + 32; a += 4)
		tput(b, " %08x", salsa64_rd32(n64->bus.rdram + a));
	tput(b, "\n");
}

unsigned cuelgue_state_text(const struct salsa64_n64 *n64, char *out, unsigned size)
{
	struct tbuf b = { out, size, 0 };
	const struct salsa64_bus *bus = &n64->bus;
	const struct salsa64_cpu *cpu = &n64->cpu;
	const struct salsa64_rsp_stats *st = &bus->rsp.stats;
	if (size)
		out[0] = '\0';

	tput(&b, "campo %u, ciclo %llu, VI: CONTROL %08x ORIGIN %06x WIDTH %u V_INTR %u V_CURRENT %u\n", bus->vi.fields,
	     (unsigned long long)cpu->cycles, bus->vi.regs[SALSA64_VI_CONTROL], bus->vi.regs[SALSA64_VI_ORIGIN],
	     bus->vi.regs[SALSA64_VI_WIDTH], bus->vi.regs[SALSA64_VI_V_INTR], bus->vi.regs[SALSA64_VI_V_CURRENT]);
	tput(&b, "VI: H_VIDEO %08x V_VIDEO %08x; PI: STATUS %08x DRAM %08x CART %08x; SI: STATUS %08x DRAM %08x\n",
	     bus->vi.regs[SALSA64_VI_H_VIDEO], bus->vi.regs[SALSA64_VI_V_VIDEO], bus->pi[SALSA64_PI_STATUS],
	     bus->pi[SALSA64_PI_DRAM_ADDR], bus->pi[SALSA64_PI_CART_ADDR], salsa64_si_read(bus, SALSA64_SI_STATUS << 2),
	     bus->si[SALSA64_SI_DRAM_ADDR]);
	tput(&b, "NMI: pendiente %u ciclo %llu; cpu: siguiente %016llx EPC %016llx ErrorEPC %016llx\n",
	     n64->nmi_pending, (unsigned long long)n64->nmi_at, (unsigned long long)cpu->pc,
	     (unsigned long long)cpu->epc, (unsigned long long)cpu->error_epc);
	tput(&b, "MI: INTR %02x máscara %02x modo %08x; SP_STATUS %04x SP_PC %03x semáforo %u; "
		 "DP: STATUS %03x START %06x END %06x CURRENT %06x\n",
	     bus->mi.intr, bus->mi.mask, bus->mi.mode, bus->sp.regs[SALSA64_SP_STATUS], bus->sp.pc,
	     bus->sp.regs[SALSA64_SP_SEMAPHORE], bus->dp[SALSA64_DP_STATUS], bus->dp[SALSA64_DP_START],
	     bus->dp[SALSA64_DP_END], bus->dp[SALSA64_DP_CURRENT]);
	tput(&b, "eventos (ciclos desde ahora):");
	tevent(&b, "tarea", n64, SALSA64_EV_SP_TASK);
	tevent(&b, "DP", n64, SALSA64_EV_DP_SYNC);
	tevent(&b, "VI", n64, SALSA64_EV_VI_INTR);
	tevent(&b, "campo", n64, SALSA64_EV_VI_FIELD);
	tevent(&b, "AI", n64, SALSA64_EV_AI_DMA);
	tevent(&b, "SI", n64, SALSA64_EV_SI_DMA);
	tevent(&b, "Compare", n64, SALSA64_EV_COMPARE);
	tput(&b, "; RSP: tarea en curso %u, DP retenida %u, tarea recogida %u\n", bus->rsp.busy, bus->rsp.dp_held,
	     bus->rsp.collected);
	tput(&b, "tareas: gráficas %u (sin SYNC_FULL ni corte %u, última con banderas %u, cortadas %u), audio %u, "
		 "otras %u, sin HLE %u, RSP a bajo nivel %u, sondeos %u, arranques con el RSP ocupado %u\n",
	     st->gfx, st->gfx_nosync, st->gfx_last, st->cut, st->audio, st->other, st->unknown_gbi, st->lle, st->polls,
	     st->starts_busy);
	tput(&b, "interrupciones: SP al fin de tarea %u; DP programadas %u, levantadas %u, retenidas por el congelado %u; "
		 "listas del CPU al RDP %u\n",
	     st->sp_irq, st->dp_sched, st->dp_fired, st->dp_held, st->dpc_runs);
	tput(&b, "controles: lecturas del juego %u %u %u %u; AI: DMA %u\n", bus->pif.reads[0], bus->pif.reads[1],
	     bus->pif.reads[2], bus->pif.reads[3], bus->ai.dmas);
	tput(&b, "cpu: pc %016llx ra %016llx sp %016llx Status %08x Cause %08x Count %08x Compare %08x\n",
	     (unsigned long long)cpu->cur_pc, (unsigned long long)cpu->gpr[31], (unsigned long long)cpu->gpr[29],
	     cpu->status, cpu->cause, (uint32_t)((cpu->cycles >> 1) + cpu->count_offset), cpu->compare);
	tcode(&b, n64, cpu->cur_pc);
	/* Los últimos eventos del RCP (sin las interrupciones del VI y del AI): el juego tiene que
	 * verlos todos en este orden para seguir; si falta o sobra alguno, ahí está el cuelgue. */
	{
		static const char *const kinds[] = { "?", "IRQ sube", "IRQ baja", "tarea arranca", "tarea termina",
						     "lista del CPU", "DP levanta" };
		static const char *const irqs[] = { "SP", "SI", "AI", "VI", "PI", "DP" };
		const struct salsa64_rsp *r = &bus->rsp;
		uint32_t n = r->evlog_n < SALSA64_EVLOG_N ? r->evlog_n : SALSA64_EVLOG_N;
		tput(&b, "últimos %u eventos del RCP (ciclos antes de este momento):\n", n);
		for (uint32_t i = 0; i < n; i++) {
			const struct salsa64_evlog_entry *e = &r->evlog[(r->evlog_n - n + i) % SALSA64_EVLOG_N];
			unsigned long long ago = (unsigned long long)(cpu->cycles - e->cycles);
			const char *kn = e->kind <= SALSA64_EV_DP_FIRE ? kinds[e->kind] : "?";
			if (e->kind == SALSA64_EV_IRQ_UP || e->kind == SALSA64_EV_IRQ_DOWN)
				tput(&b, "  -%llu %s %s (MI_INTR %02x)\n", ago, kn, e->a < 6 ? irqs[e->a] : "?", e->b);
			else if (e->kind == SALSA64_EV_TASK_START)
				tput(&b, "  -%llu %s tipo %u lista %08x\n", ago, kn, e->a, e->b);
			else if (e->kind == SALSA64_EV_TASK_DONE)
				tput(&b, "  -%llu %s tipo %u banderas %u\n", ago, kn, e->a, e->b);
			else if (e->kind == SALSA64_EV_CPU_LIST)
				tput(&b, "  -%llu %s %06x-%06x\n", ago, kn, e->a, e->b);
			else
				tput(&b, "  -%llu %s (esperando %u)\n", ago, kn, e->a);
		}
	}
	return b.n;
}

static void dump_files(const struct salsa64_n64 *n64, const char *dir, uint32_t field, const char *text,
		       void (*log)(const char *fmt, ...))
{
	char path[160];
	snprintf(path, sizeof path, "%s/cuelgue-%u.txt", dir, field);
	FILE *f = fopen(path, "w");
	if (f) {
		fputs(text, f);
		fputs("gpr:\n", f);
		for (int i = 0; i < 32; i += 4)
			fprintf(f, "  r%-2d %016llx %016llx %016llx %016llx\n", i, (unsigned long long)n64->cpu.gpr[i],
				(unsigned long long)n64->cpu.gpr[i + 1], (unsigned long long)n64->cpu.gpr[i + 2],
				(unsigned long long)n64->cpu.gpr[i + 3]);
		fprintf(f, "hi %016llx lo %016llx epc %016llx bad_vaddr %016llx\n", (unsigned long long)n64->cpu.hi,
			(unsigned long long)n64->cpu.lo, (unsigned long long)n64->cpu.epc,
			(unsigned long long)n64->cpu.bad_vaddr);
		fprintf(f, "dmem 0xFC0 (la última tarea del RSP):");
		for (int i = 0; i < 16; i++)
			fprintf(f, " %08x", salsa64_rd32(n64->bus.sp_mem + 0xFC0 + 4 * i));
		fputc('\n', f);
		fclose(f);
	}
	snprintf(path, sizeof path, "%s/cuelgue-%u.rdram", dir, field);
	f = fopen(path, "wb");
	bool ok = f != NULL;
	if (f) {
		for (uint32_t off = 0; off < n64->bus.rdram_size && ok; off += 65536) {
			uint32_t n = n64->bus.rdram_size - off < 65536 ? n64->bus.rdram_size - off : 65536;
			ok = fwrite(n64->bus.rdram + off, 1, n, f) == n;
		}
		ok = fclose(f) == 0 && ok;
	}
	log("latido: volcado de la RDRAM en %s/cuelgue-%u.rdram y de los registros en cuelgue-%u.txt: %s", dir, field,
	    field, ok ? "bien" : "FALLO");
}

void cuelgue_reset(struct cuelgue *c, bool always)
{
	memset(c, 0, sizeof *c);
	c->always = always;
}

bool cuelgue_watch(struct cuelgue *c, const struct salsa64_n64 *n64, uint64_t now_us, bool expect,
		   const char *dir, void (*log)(const char *fmt, ...))
{
	const struct salsa64_bus *bus = &n64->bus;
	uint32_t reads = bus->pif.reads[0] + bus->pif.reads[1] + bus->pif.reads[2] + bus->pif.reads[3];
	uint32_t work = bus->rsp.stats.gfx + bus->rsp.stats.dpc_runs + bus->rsp.stats.lle;
	uint32_t fields = bus->vi.fields;

	if (!c->init || !expect || fields == c->fields) {
		/* Sin juego, en pausa o sin avanzar el VI (de eso se ocupa el latido de los hilos): empezar de nuevo. */
		c->init = true;
		c->t_change = now_us;
		c->reads = reads;
		c->work = work;
		c->fields = fields;
		return false;
	}
	c->fields = fields;
	if (reads != c->reads || work != c->work) {
		if (c->said)
			log("latido: el juego volvió a leer los controles o a dibujar tras %.1f s", (now_us - c->t_change) / 1e6);
		c->said = false;
		c->t_change = now_us;
		c->reads = reads;
		c->work = work;
		if (c->active < 16)
			c->active++;
		return false;
	}
	/* Un juego que nunca dio señales de vida (un menú del juego, una pantalla fija desde el arranque) no
	 * cuenta como colgado: tiene que haber estado leyendo o dibujando antes; salvo con latido-siempre,
	 * para ver dónde se queda un juego que no arranca. */
	if (c->said || (c->active < 6 && !c->always) || now_us - c->t_change < CUELGUE_US)
		return false;
	c->said = true;

	static char text[8192];  /* con el anillo de eventos del RCP; una vez por corrida, desde el hilo del latido */
	cuelgue_state_text(n64, text, sizeof text);
	log("latido: el juego no lee los controles ni manda tareas gráficas ni listas al RDP hace %.1f s y el VI sigue "
	    "(campo %u): ¿esperando una interrupción o un mensaje que no llega?", (now_us - c->t_change) / 1e6, fields);
	for (char *line = text, *next; *line; line = next) {
		next = strchr(line, '\n');
		if (next)
			*next++ = '\0';
		else
			next = line + strlen(line);
		if (*line)
			log("latido:   %s", line);
	}
	if (dir && c->dumps < CUELGUE_DUMPS) {
		c->dumps++;
		/* El texto vuelve a armarse: el log lo partió en líneas. */
		cuelgue_state_text(n64, text, sizeof text);
		dump_files(n64, dir, fields, text, log);
	}
	return true;
}
