/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — recompilador: traducción de un bloque MIPS a PowerPC64.
 *
 * Escrito desde la documentación pública del VR4300 y de Power ISA; la
 * semántica de cada instrucción es la del intérprete (interp.c), que es la
 * referencia. Diseño en docs/rec-ppe.md.
 *
 * Registros del PPE dentro del código generado (ABI ELFv1 de PSL1GHT):
 *   r1  pila (marco de la entrada, rec.c)    r2  TOC (no se toca)
 *   r3–r12, r0  temporales y argumentos       r13 reservado (hilo)
 *   r14–r25  registros MIPS del bloque (GPR, HI = 32, LO = 33)
 *   r26 destino de JR/JALR (sobrevive al delay slot y a las llamadas)
 *   r27 sched->next   r28 code_flags   r29 cycles al entrar al bloque
 *   r30 RDRAM         r31 struct salsa64_cpu
 *   cr4 condición del salto (sobrevive al delay slot); cr0 y cr1 temporales.
 *
 * Forma de un bloque:
 *   entrada:  si es código mapeado y ktlb[] ya no traduce su página a la
 *             misma física: salir al despachador (rec_tlb.h)
 *             si cycles + n > next: salir (lo corre el intérprete hasta el
 *             evento); si hay COP1 y Status.CU1 = 0: salir igual
 *             carga de los registros que se leen antes de escribirse
 *   cuerpo:   una traducción por instrucción; los caminos lentos (E/S, TLB,
 *             código invalidado) van aparte, en la sección fría
 *   salida:   escritura de los registros modificados, cycles += n, y un
 *             enlace parcheable al bloque siguiente (o vuelta al cuerpo si
 *             el salto va al propio bloque).
 * Cada llamada a C deja antes cycles, cur_pc y delay_slot en la estructura
 * (como el intérprete) y después revisa redirect/irq_pending/rec_abort/mode
 * y el próximo evento; si algo cambió, escribe los registros y sale.
 */
#include <stddef.h>
#include <string.h>

#include "core/sched.h"
#include "cpu/fpu.h"
#include "cpu/ppc.h"
#include "cpu/rec.h"
#include "cpu/vr4300_internal.h"

/* ---- Registros y desplazamientos ------------------------------------------- */

enum {
	T_ADDR = 3,      /* dirección efectiva; primer argumento */
	T_HI = 7,        /* HI no asignado */
	T_V = 8,         /* destino no asignado */
	T_S1 = 9,        /* fuentes no asignadas */
	T_S2 = 10,
	T_X = 11,        /* auxiliares */
	T_Y = 12,
	R_CACHE0 = 14,
	R_TARGET = 26,
	R_NEXT = 27,
	R_FLAGS = 28,
	R_CYCLES = 29,
	R_RDRAM = 30,
	R_CPU = 31,
};
#define N_CACHE 12   /* sin registros fijos; con SALSA64_REC_OPT_PIN, 4 (r14–r17) */
#define CR_T  1   /* pruebas internas */
#define CR_BR 4   /* condición del salto */

#define REG_HI 32
#define REG_LO 33
#define BIT(r) (1ull << (r))

#define OFF(field) ((int32_t)offsetof(struct salsa64_cpu, field))

static int32_t off_reg(unsigned r)
{
	if (r == REG_HI)
		return OFF(hi);
	if (r == REG_LO)
		return OFF(lo);
	return OFF(gpr) + 8 * (int32_t)r;
}

/* ---- Decodificación --------------------------------------------------------- */

enum kind {
	K_NORMAL,
	K_BRANCH,     /* condicional, el delay slot siempre corre */
	K_BRANCH_L,   /* branch-likely: si no se toma, se anula el delay slot */
	K_JUMP,       /* J, JAL */
	K_JUMP_REG,   /* JR, JALR */
	K_END,        /* termina el bloque después de ella y vuelve a C */
};

enum how {
	H_NATIVE,
	H_EXEC,       /* la ejecuta el intérprete (salsa64_vr4300_exec_one) */
	H_EXC,        /* SYSCALL/BREAK: excepción segura */
	H_MTC0,       /* MTC0 a EntryLo0/1, PageMask, Index o EPC: el valor y su máscara (rec_emit_ext.h) */
	H_MTC0_ST,    /* MTC0 a Status: en línea si solo cambian IE, EXL o IM; termina el bloque */
	H_TLBW,       /* TLBWI/TLBWR: rec_h_tlbw y el bloque termina enlazado */
	H_ERET,       /* ERET: rec_h_eret y al destino por exit_jr */
};

struct dec {
	uint32_t w;
	uint8_t kind;
	uint8_t how;
	uint8_t cop1;     /* necesita Status.CU1 */
	uint64_t reads, writes;
};

static bool is_branch(const struct dec *d)
{
	return d->kind >= K_BRANCH && d->kind <= K_JUMP_REG;
}

/* ¿El código de la instrucción depende de Status.FR? Solo si accede a un
 * registro impar de la FPU por fs (MFC1, MTC1, DMFC1, DMTC1 y las
 * operaciones) o por ft (LWC1, LDC1, SWC1, SDC1): con FR = 0 ese registro
 * es la mitad alta de la pareja (fpr32_off, fpr64_off, fslot32, fslot64).
 * CFC1, CTC1 y BC1 no tocan registros de la FPU. */
static bool fr_dependent(uint32_t i)
{
	unsigned op = I_OP(i), rs = I_RS(i);
	if (op == 0x31 || op == 0x35 || op == 0x39 || op == 0x3D)
		return (I_RT(i) & 1) != 0;
	if (op == 0x11 && rs != 0x02 && rs != 0x06 && rs != 0x08)
		return (I_FS(i) & 1) != 0;
	return false;
}

static void decode(uint32_t i, struct dec *d)
{
	unsigned rs = I_RS(i), rt = I_RT(i), rd = I_RD(i);
	memset(d, 0, sizeof *d);
	d->w = i;
#define RD(x) (d->reads |= BIT(x))
#define WR(x) (d->writes |= BIT(x))
	switch (I_OP(i)) {
	case 0x00:
		switch (I_FUNCT(i)) {
		case 0x00: case 0x02: case 0x03:                       /* SLL SRL SRA */
		case 0x38: case 0x3A: case 0x3B: case 0x3C: case 0x3E: case 0x3F:
			RD(rt); WR(rd); break;
		case 0x04: case 0x06: case 0x07: case 0x14: case 0x16: case 0x17:
		case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
		case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
			RD(rs); RD(rt); WR(rd); break;
		case 0x08: RD(rs); d->kind = K_JUMP_REG; break;                 /* JR */
		case 0x09: RD(rs); WR(rd); d->kind = K_JUMP_REG; break;         /* JALR */
		case 0x0C: case 0x0D: d->how = H_EXC; d->kind = K_END; break;   /* SYSCALL BREAK */
		case 0x0F: break;                                               /* SYNC */
		case 0x10: RD(REG_HI); WR(rd); break;
		case 0x11: RD(rs); WR(REG_HI); break;
		case 0x12: RD(REG_LO); WR(rd); break;
		case 0x13: RD(rs); WR(REG_LO); break;
		case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
			RD(rs); RD(rt); WR(REG_HI); WR(REG_LO); break;
		default:  /* traps y funciones inexistentes */
			RD(rs); RD(rt); d->how = H_EXEC; break;
		}
		break;
	case 0x01:
		switch (rt) {
		case 0x00: case 0x01: case 0x02: case 0x03:
			RD(rs); d->kind = (rt & 2) ? K_BRANCH_L : K_BRANCH; break;
		case 0x10: case 0x11: case 0x12: case 0x13:
			RD(rs); WR(31); d->kind = (rt & 2) ? K_BRANCH_L : K_BRANCH; break;
		default:  /* traps con inmediato y códigos inexistentes */
			RD(rs); d->how = H_EXEC; break;
		}
		break;
	case 0x02: d->kind = K_JUMP; break;
	case 0x03: WR(31); d->kind = K_JUMP; break;
	case 0x04: case 0x05: RD(rs); RD(rt); d->kind = K_BRANCH; break;
	case 0x06: case 0x07: RD(rs); d->kind = K_BRANCH; break;
	case 0x14: case 0x15: RD(rs); RD(rt); d->kind = K_BRANCH_L; break;
	case 0x16: case 0x17: RD(rs); d->kind = K_BRANCH_L; break;
	case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E:
	case 0x18: case 0x19:
		RD(rs); WR(rt); break;
	case 0x0F: WR(rt); break;
	case 0x10:                                   /* COP0: siempre vuelve a C después */
		RD(rt); WR(rt); d->how = H_EXEC; d->kind = K_END; break;
	case 0x11:
		d->cop1 = 1;
		switch (rs) {
		case 0x08:
			if (!(rt & ~3u))
				d->kind = (rt & 2) ? K_BRANCH_L : K_BRANCH;
			else
				d->how = H_EXEC;
			break;
		case 0x00: case 0x01: WR(rt); break;                    /* MFC1 DMFC1 */
		case 0x04: case 0x05: RD(rt); break;                    /* MTC1 DMTC1 */
		case 0x02: WR(rt); d->how = H_EXEC; break;              /* CFC1 */
		case 0x06: RD(rt); d->how = H_EXEC; break;              /* CTC1 */
		case 0x10: case 0x11: {                                 /* S, D */
			unsigned f = I_FUNCT(i);
			if (!(f <= 0x03 || (f >= 0x05 && f <= 0x07) || f >= 0x30))
				d->how = H_EXEC;  /* SQRT, conversiones, redondeos */
			break;
		}
		default: d->how = H_EXEC; break;
		}
		break;
	case 0x12: RD(rt); WR(rt); d->how = H_EXEC; break;          /* COP2 */
	case 0x20: case 0x21: case 0x23: case 0x24: case 0x25: case 0x27: case 0x37:
		RD(rs); WR(rt); break;                                  /* LB LH LW LBU LHU LWU LD */
	case 0x28: case 0x29: case 0x2B: case 0x3F:
		RD(rs); RD(rt); break;                                  /* SB SH SW SD */
	case 0x31: case 0x35: case 0x39: case 0x3D:
		RD(rs); d->cop1 = 1; break;                             /* LWC1 LDC1 SWC1 SDC1 */
	case 0x1A: case 0x1B: case 0x22: case 0x26: case 0x38: case 0x3C:
		RD(rs); RD(rt); WR(rt); d->how = H_EXEC; break;         /* LDL LDR LWL LWR SC SCD */
	case 0x30: case 0x34:
		RD(rs); WR(rt); d->how = H_EXEC; break;                 /* LL LLD */
	case 0x2A: case 0x2C: case 0x2D: case 0x2E:
		RD(rs); RD(rt); d->how = H_EXEC; break;                 /* SWL SDL SDR SWR */
	case 0x2F: RD(rs); d->how = H_EXEC; break;                  /* CACHE */
	default: d->how = H_EXEC; break;                            /* COP2/COP3 y reservadas */
	}
#undef RD
#undef WR
	d->reads &= ~1ull;
	d->writes &= ~1ull;
}

/* ---- Ensamblador ------------------------------------------------------------- */

enum { SEC_HOT, SEC_COLD, SEC_ABS, SEC_NONE };

/* Qué hace la rutina compartida del camino lento (rec.c, emit_preamble,
 * slow_rtn): va en los bits 12–13 de r6. Cada bloque le llega por un
 * trampolín propio que deja su pc y su n (emit_trampolines). */
enum { SLOW_LOAD = 0, SLOW_STORE = 1, SLOW_EXEC = 2, SLOW_EXC = 3 };

/* Salida pendiente de una instrucción (se emite al final, en la sección fría). */
struct pending_exit {
	uint16_t label;
	uint8_t k;
	uint8_t after;     /* la instrucción terminó: continuar en la siguiente */
	uint8_t fflush;    /* FCR31 perezoso a escribir (rec_emit_fpu.h): 1 causas y bandera I, 2 C */
	uint64_t mask;     /* registros a escribir */
};

/* Estado de la FPU residente que depende del camino (rec_emit_fpu.h). */
struct fpu_snap {
	uint8_t ffmt[32], fw_ok[32];
	bool ran, cw;
};

/* Salto condicional interno cuyo destino está dentro del bloque: su camino
 * tomado se emite al llegar a la instrucción destino (cuando se sabe el
 * estado que el código asume ahí) y salta a ella sin salir del bloque. */
struct ijump {
	uint8_t b, t;        /* el salto y la instrucción destino */
	int label;           /* a dónde va el bc del salto (sección fría) */
	struct fpu_snap snap;  /* estado después de su delay slot */
};

struct ctx {
	int idle_patch;  /* etiqueta de la consulta que se puede quitar */
	struct salsa64_rec *rec;
	uint64_t pc;
	uint32_t pa;
	unsigned n;
	bool temp;
	bool has_branch;
	bool calls_near;        /* este bloque llama a C con `bl` (a menos de 32 MB); si no, mtctr/bctrl */
	uint8_t seg_kind;       /* SALSA64_REC_KSEG0, KSEG1 o MAPPED (rec_tlb.h) */
	bool hot;               /* se emite en la zona caliente: registra sus sitios de salida (rec_profile.h) */
	unsigned pent;          /* su entrada del perfil (índice + 1), 0 = ninguna */
	unsigned sec;
	struct dec d[SALSA64_REC_MAX_INSNS];
	uint8_t map[34];        /* registro MIPS → registro del PPE, 0 = en memoria */
	uint64_t cached;        /* en un registro del PPE (por bloque o fijo) */
	uint64_t pinned;        /* fijos entre bloques (SALSA64_REC_OPT_PIN) */
	uint64_t borrowed;      /* fijos que este bucle no usa: su registro del PPE queda para otros */
	uint64_t written;       /* asignados y modificados desde la entrada o el inicio del bucle */
	uint64_t written_start; /* written al empezar la instrucción en curso */
	uint8_t last_write[34]; /* última instrucción que escribe cada registro, + 1 (0 = ninguna) */
	uint64_t eager;         /* con su último valor, esperando para escribirse (OPT_EAGER) */
	uint8_t eager_due[34];  /* instrucción antes de la cual escribirlo */
	unsigned k;             /* instrucción en curso */
	bool in_ds;
	int body;               /* etiqueta del cuerpo (vuelta de los bucles) */
	bool fr_check;          /* el código depende de Status.FR: la entrada lo revisa (fr_dependent) */
	int exit_before[SALSA64_REC_MAX_INSNS];
	int exit_after[SALSA64_REC_MAX_INSNS];
	struct pending_exit pending[2 * SALSA64_REC_MAX_INSNS];
	unsigned npending;
	/* Cada salto del bloque (el final y los condicionales internos hacia
	 * adelante), para continuar después de su delay slot. */
	struct brinfo {
		uint8_t kind;
		uint8_t bo, bi;     /* condición de "tomado" en cr4 */
		uint64_t target, fall;
	} br[SALSA64_REC_MAX_INSNS];
	uint8_t ds_of[SALSA64_REC_MAX_INSNS];  /* k es el delay slot del salto ds_of[k] - 1 (0: no es delay slot) */
	/* FPU residente (rec_emit_fpu.h). */
	bool fpu_res;                      /* el bloque usa la FPU con registros residentes */
	bool fneed_gate, fneed_ien, fneed_c;  /* bits de FCR31 que el bloque deriva a la entrada */
	bool fpu_ran;                      /* alguna operación nativa puso causas: al salir van de cr2.1 */
	bool fc_written;                   /* C.cond escribió C: al salir va de cr2.0 */
	uint8_t fmap[32];                  /* cpu->fpr[p] → registro f14–f25 (0: ninguno) */
	uint8_t ffmt[32];                  /* formato con que ese registro tiene el valor ahora */
	uint8_t flive[32];                 /* formato con que se carga a la entrada (FR_NONE: no) */
	uint8_t fw_ok[32];                 /* cpu->rec_fw[p] tiene la palabra W de p con signo extendido */
	int exit_after_mem[SALSA64_REC_MAX_INSNS];  /* salida tras k con cpu->fcr31 ya como verdad */
	/* Saltos internos con destino dentro del bloque. */
	int insn_label[SALSA64_REC_MAX_INSNS];  /* etiqueta del código de la instrucción k (−1: nadie salta ahí) */
	struct ijump ijumps[SALSA64_REC_MAX_INSNS];
	unsigned nijumps;
	/* Código compacto (docs/rec-ppe.md, «Código compacto»); `wide` repite el
	 * de antes del 2026-10-13 (SALSA64_REC_OPT_WIDE) para medir A/B. */
	bool wide;
	int tramp;                         /* trampolín del bloque a la rutina compartida (−1: no la usa) */
	/* Dirección de un acceso (emit_addr): el tramo frío que prueba la ktlb
	 * se emite pegado al camino lento del acceso (emit_addr_cold). */
	struct {
		int mapped, back;
		uint8_t va, align;
		bool store, pending;
		/* fastmem (docs/fastmem.md): el acceso caliente va sin prueba; la
		 * prueba de siempre y una copia del acceso quedan en la sección
		 * fría (fm_cold), adonde se parchea el acceso si falla. */
		bool fm;
		bool check;         /* almacenamiento: la copia repite la prueba de code_flags */
		int fm_cold, fm_after, slow;
		uint32_t word;      /* el acceso, para la copia fría */
	} addr;
	bool fastmem;           /* rec->fastmem y no `wide`: los accesos de emit_load/emit_store sin prueba */
	/* Base en caché (docs/fastmem.md §8): accesos seguidos con el mismo
	 * registro base; la prueba una vez, en la cabeza, y T_HI = RDRAM +
	 * x_lo + span mientras dura el grupo (group_pass, emit_group_head). */
	struct grp {
		uint8_t head;       /* cabeza del grupo; GRP_NONE: no agrupado */
		uint8_t align;      /* la del acceso más grande: 0, 1, 3, 7 */
		bool last, store;   /* último del grupo; el grupo almacena (la ktlb mira D) */
		int16_t min_imm;
		uint16_t span;      /* max_imm − min_imm, redondeado abajo a la alineación */
	} grp[SALSA64_REC_MAX_INSNS];
	bool grp_live;          /* hay un grupo abierto: T_HI vale (se repone tras llamar a C) */
	unsigned grp_cur;       /* su cabeza */
	/* Cadena de escritura: los registros del bloque que escribe, por primera
	 * escritura descendente; una salida entra donde empiezan los que ya
	 * tienen valor y ejecuta hasta el blr (chain_call). */
	uint8_t chain_reg[N_CACHE + 1];
	uint8_t chain_fw[N_CACHE + 1];     /* primera instrucción que lo escribe */
	unsigned nchain;
	int chain_label[N_CACHE + 2];      /* entrada i: desde chain_reg[i]; nchain: solo el blr */
	bool chain_used;
	/* Perfilado (rec_trace.h): palabras por clase. cls es la clase a la que
	 * van las palabras desde cmark[] (posiciones en cada sección). */
	bool loop;
	uint8_t cls;
	uint32_t cmark[2];
	uint8_t cls_n[SALSA64_RT_CLASSES];
	uint32_t cls_w[SALSA64_RT_CLASSES][2];
};

#define GRP_NONE 0xFF

static void restore_borrowed(struct ctx *c);
/* rec_emit_fpu.h */
static void fpu_flush(struct ctx *c);
static void fpu_flush_bits(struct ctx *c, bool ran, bool cbit);
static void fpu_after_cop1_call(struct ctx *c, unsigned k);
static void fpu_loop_back(struct ctx *c);
static void fpu_loaded(struct ctx *c, unsigned k, unsigned rt, bool wide, unsigned d, bool from_rdram);
static unsigned fpu_store_reg(const struct ctx *c, unsigned rt, bool wide);

static void E(struct ctx *c, uint32_t w)
{
	struct salsa64_rec *r = c->rec;
	if (r->nbuf[c->sec] >= SALSA64_REC_BUF_WORDS) {
		r->overflow = true;
		return;
	}
	r->buf[c->sec][r->nbuf[c->sec]++] = w;
}

static int new_label(struct ctx *c)
{
	struct salsa64_rec *r = c->rec;
	if (r->nlabels >= SALSA64_REC_MAX_LABELS) {
		r->overflow = true;
		return 0;
	}
	r->labels[r->nlabels].sec = SEC_NONE;
	r->labels[r->nlabels].pos = 0;
	return (int)r->nlabels++;
}

static void bind(struct ctx *c, int l)
{
	c->rec->labels[l].sec = (uint8_t)c->sec;
	c->rec->labels[l].pos = c->rec->nbuf[c->sec];
}

/* Etiqueta fija en la caché: `word` es relativa al inicio de la caché (puede
 * ser negativa para funciones de C). */
static int abs_label(struct ctx *c, int64_t word)
{
	int l = new_label(c);
	c->rec->labels[l].sec = SEC_ABS;
	c->rec->labels[l].pos = (uint32_t)(int32_t)word;
	return l;
}

static void fixup(struct ctx *c, int l, bool cond)
{
	struct salsa64_rec *r = c->rec;
	if (r->nfixups >= SALSA64_REC_MAX_FIXUPS) {
		r->overflow = true;
		return;
	}
	struct salsa64_rec_fixup *f = &r->fixups[r->nfixups++];
	f->at = r->nbuf[c->sec];
	f->label = (uint16_t)l;
	f->sec = (uint8_t)c->sec;
	f->cond = cond;
}

static void emit_b(struct ctx *c, int l, bool link)
{
	fixup(c, l, false);
	E(c, link ? ppc_bl(0) : ppc_b(0));
}

static void emit_bc(struct ctx *c, unsigned bo, unsigned bi, int l)
{
	fixup(c, l, true);
	E(c, ppc_bc(bo, bi, 0));
}

/* Carga una constante de 64 bits (2 instrucciones si es extensión de signo de
 * 32 bits, como las direcciones de KSEG0). */
static void li64(struct ctx *c, unsigned rt, uint64_t v)
{
	if ((int64_t)v == (int16_t)v) {
		E(c, ppc_li(rt, (int16_t)v));
	} else if ((int64_t)v == (int32_t)v) {
		E(c, ppc_lis(rt, (int16_t)(v >> 16)));
		if (v & 0xFFFF)
			E(c, ppc_ori(rt, rt, (uint32_t)v & 0xFFFF));
	} else {
		E(c, ppc_lis(rt, (int16_t)(v >> 48)));
		E(c, ppc_ori(rt, rt, (uint32_t)(v >> 32) & 0xFFFF));
		E(c, ppc_sldi(rt, rt, 32));
		E(c, ppc_oris(rt, rt, (uint32_t)(v >> 16) & 0xFFFF));
		E(c, ppc_ori(rt, rt, (uint32_t)v & 0xFFFF));
	}
}

/* Constante de 32 bits sin signo (argumentos `uint32_t`: la ABI los pasa
 * extendidos con ceros). */
static void li32u(struct ctx *c, unsigned rt, uint32_t v)
{
	E(c, ppc_li(rt, 0));
	if (v >> 16)
		E(c, ppc_oris(rt, rt, v >> 16));
	if (v & 0xFFFF)
		E(c, ppc_ori(rt, rt, v & 0xFFFF));
}

/* Constante de 32 bits en dos palabras, con el signo extendido (quien la
 * recibe como uint32_t la recorta: las rutinas compartidas hacen clrldi). */
static void li32s(struct ctx *c, unsigned rt, uint32_t v)
{
	E(c, ppc_lis(rt, (int16_t)(v >> 16)));
	if (v & 0xFFFF)
		E(c, ppc_ori(rt, rt, v & 0xFFFF));
}

/* Perfilado (rec_trace.h): lo emitido desde la última marca va a la clase
 * en curso y pasa a contar `cls`. Devuelve la anterior. */
static unsigned cls_set(struct ctx *c, unsigned cls)
{
	unsigned old = c->cls;
	if (!c->rec->trace)
		return old;
	for (unsigned s = 0; s < 2; s++) {
		c->cls_w[old][s] += c->rec->nbuf[s] - c->cmark[s];
		c->cmark[s] = c->rec->nbuf[s];
	}
	c->cls = (uint8_t)cls;
	return old;
}

/* ---- Camino lento compartido (código compacto) -------------------------------------- */

/*
 * Lo que una carga, un almacenamiento, una instrucción al intérprete o una
 * excepción necesitan de C vive en rutinas del preámbulo (rec.c,
 * emit_preamble): dejan cycles, cur_pc y delay_slot como el intérprete,
 * llaman, recargan sched->next y dicen si hay que salir. El sitio solo pasa
 * r6 = (k + 1) | delay slot << 8 (una palabra) y salta al trampolín del
 * bloque para esa rutina, que agrega lo que es del bloque entero (su pc en
 * r7 y n en la mitad alta de r6) y sigue a la rutina. Antes cada sitio
 * repetía el pc, n, la llamada y la revisión (~25 palabras).
 */
/* Lo que el sitio pasa en r6 (sin n, que agrega el trampolín). */
static uint32_t slow_arg(const struct ctx *c, unsigned what, unsigned kind, unsigned k)
{
	return (k + 1) | (c->in_ds ? 0x100u : 0) | kind << 9 | what << 12;
}

/* r6 = (k + 1) | delay slot << 8 | tipo << 9 | qué << 12 y a la rutina por
 * el trampolín del bloque. */
static void emit_slow_call(struct ctx *c, unsigned what, unsigned kind, unsigned k)
{
	if (c->tramp < 0)
		c->tramp = new_label(c);
	/* La C pisa T_HI (el trampolín deja ahí el pc): con un grupo abierto se
	 * guarda en cpu->rec_tb y se repone; el puntero sigue valiendo (la
	 * RDRAM no se mueve y lo que cambia la TLB termina el bloque). */
	bool keep = c->grp_live && what != SLOW_EXC;
	if (keep)
		E(c, ppc_std(T_HI, OFF(rec_tb), R_CPU));
	E(c, ppc_li(6, (int32_t)slow_arg(c, what, kind, k)));
	emit_b(c, c->tramp, true);
	if (keep)
		E(c, ppc_ld(T_HI, OFF(rec_tb), R_CPU));
}

/* Al final del bloque (sección fría): el trampolín, si lo usó. */
static void emit_trampolines(struct ctx *c)
{
	if (c->tramp < 0)
		return;
	bind(c, c->tramp);
	li64(c, 7, c->pc);
	E(c, ppc_oris(6, 6, c->n));
	emit_b(c, abs_label(c, c->rec->slow_rtn), false);
}

/*
 * Cadena de escritura. Una salida a C por el camino lento (excepción,
 * interrupción, evento, código invalidado) tiene que dejar en cpu->gpr los
 * registros del bloque que ya recibieron un valor. Cada registro del PPE
 * tiene el valor correcto desde la primera instrucción que lo escribe (y
 * antes, si se cargó a la entrada, el mismo que la memoria), así que «los que
 * hay que escribir al salir en k» son los de primera escritura ≤ k (< k si k
 * no terminó). Ordenados por primera escritura descendente, son un sufijo de
 * una sola secuencia de std: la salida entra en el primero que le toca y
 * ejecuta hasta el blr. Un std por registro por bloque, en vez de uno por
 * registro sucio en cada camino lento (en GoldenEye, el 28 % de la sección
 * fría de los bloques con muchos accesos).
 */
static void chain_build(struct ctx *c)
{
	c->nchain = 0;
	c->chain_used = false;
	for (unsigned k = 0; k < c->n; k++) {
		uint64_t m = c->d[k].writes & c->cached & ~c->pinned;
		for (unsigned r = 1; m && r < 34; r++) {
			if (!(m & BIT(r)))
				continue;
			bool seen = false;
			for (unsigned i = 0; i < c->nchain; i++)
				seen |= c->chain_reg[i] == r;
			if (!seen && c->nchain < N_CACHE + 1) {  /* en orden de primera escritura; se emite al revés */
				c->chain_reg[c->nchain] = (uint8_t)r;
				c->chain_fw[c->nchain] = (uint8_t)k;
				c->nchain++;
			}
		}
	}
	for (unsigned i = 0; i <= c->nchain; i++)
		c->chain_label[i] = new_label(c);
}

/* Escribe los registros con valor al salir en k (después de k si `after`):
 * una llamada a la cadena, o nada si no hay ninguno. chain_reg[] va por
 * primera escritura ascendente: los m primeros son los que ya tienen valor,
 * y la entrada m − 1 de la cadena los escribe todos. */
static void chain_call(struct ctx *c, unsigned k, bool after)
{
	unsigned m = 0;
	while (m < c->nchain && (after ? c->chain_fw[m] <= k : c->chain_fw[m] < k))
		m++;
	if (!m)
		return;
	c->chain_used = true;
	emit_b(c, c->chain_label[m - 1], true);
}

static void emit_chain(struct ctx *c)
{
	if (!c->chain_used)
		return;
	/* La entrada i escribe chain_reg[i], chain_reg[i − 1], …, chain_reg[0]:
	 * los std en orden inverso de primera escritura, con una etiqueta antes
	 * de cada uno. */
	for (unsigned i = c->nchain; i-- > 0;) {
		bind(c, c->chain_label[i]);
		E(c, ppc_std(c->map[c->chain_reg[i]], off_reg(c->chain_reg[i]), R_CPU));
	}
	bind(c, c->chain_label[c->nchain]);  /* no se usa (nada que escribir = sin llamada) */
	E(c, PPC_BLR);
}

/* ---- Registros del bloque ------------------------------------------------------ */

/* Registro del PPE con el valor de `r`; si no está asignado, lo carga en `tmp`. */
static unsigned get(struct ctx *c, unsigned r, unsigned tmp)
{
	if (r == 0) {
		E(c, ppc_li(tmp, 0));
		return tmp;
	}
	if (c->map[r])
		return c->map[r];
	E(c, ppc_ld(tmp, off_reg(r), R_CPU));
	return tmp;
}

/* Registro donde calcular el nuevo valor de `r`. */
static unsigned dst(struct ctx *c, unsigned r, unsigned tmp)
{
	return (r && c->map[r]) ? c->map[r] : tmp;
}

/* El valor de `r` quedó en `reg` (el que dio dst).
 *
 * Con SALSA64_REC_OPT_EAGER, un registro asignado que recibe su último valor
 * del bloque se escribe en memoria dos instrucciones después, no a la
 * salida: si el bloque siguiente lo lee enseguida, el almacenamiento ya
 * salió de la cola (load-hit-store). No justo después: en el PPE, un
 * almacenamiento que espera el valor recién calculado detiene el pipeline
 * (medido: −37 % en un bucle de enteros). */
static void put(struct ctx *c, unsigned r, unsigned reg)
{
	if (r == 0 || (c->pinned & BIT(r)))
		return;  /* un registro fijo no se escribe hasta volver a C */
	if (!c->map[r]) {
		E(c, ppc_std(reg, off_reg(r), R_CPU));
		return;
	}
	c->written |= BIT(r);
	if ((c->rec->opts & SALSA64_REC_OPT_EAGER) && c->last_write[r] == c->k + 1) {
		c->eager |= BIT(r);
		c->eager_due[r] = (uint8_t)(c->k + 2);
	}
}

/* Escribe los registros de OPT_EAGER cuyo turno llegó, antes de la
 * instrucción k. */
static void flush_eager(struct ctx *c, unsigned k)
{
	for (unsigned r = 1; c->eager && r < 34; r++) {
		if ((c->eager & BIT(r)) && c->eager_due[r] <= k) {
			E(c, ppc_std(c->map[r], off_reg(r), R_CPU));
			c->eager &= ~BIT(r);
			c->written &= ~BIT(r);
		}
	}
}

/* Escribe en cpu->gpr los registros de `mask` que están en el PPE. */
static void store_regs(struct ctx *c, uint64_t mask)
{
	mask &= c->cached;
	for (unsigned r = 1; r < 34; r++)
		if (mask & BIT(r))
			E(c, ppc_std(c->map[r], off_reg(r), R_CPU));
}

/* Al salir del bloque: los fijos siguen en sus registros. */
static void writeback(struct ctx *c, uint64_t mask)
{
	mask &= c->cached & ~c->pinned;
	for (unsigned r = 1; r < 34; r++)
		if (mask & BIT(r))
			E(c, ppc_std(c->map[r], off_reg(r), R_CPU));
}

static void reload(struct ctx *c, uint64_t mask)
{
	mask &= c->cached;
	for (unsigned r = 1; r < 34; r++)
		if (mask & BIT(r))
			E(c, ppc_ld(c->map[r], off_reg(r), R_CPU));
}

/* ---- Salidas ------------------------------------------------------------------ */

static uint64_t insn_pc(const struct ctx *c, unsigned k)
{
	return c->pc + 4ull * k;
}

/* r3 = dónde sigue la ejecución después de la instrucción k. En un delay
 * slot, según su salto (la condición sigue en cr4, que las llamadas a C no
 * tocan). */
static void emit_continue(struct ctx *c, unsigned k)
{
	if (!c->ds_of[k]) {
		li64(c, 3, insn_pc(c, k + 1));
		return;
	}
	const struct brinfo *br = &c->br[c->ds_of[k] - 1];
	switch (br->kind) {
	case K_JUMP_REG:
		E(c, ppc_mr(3, R_TARGET));
		return;
	case K_BRANCH: {
		int taken = new_label(c), join = new_label(c);
		emit_bc(c, br->bo, br->bi, taken);
		li64(c, 3, br->fall);
		emit_b(c, join, false);
		bind(c, taken);
		li64(c, 3, br->target);
		bind(c, join);
		return;
	}
	default:  /* J/JAL, o branch-likely (el delay slot solo corre si se tomó) */
		li64(c, 3, br->target);
		return;
	}
}

/* Salida desde la instrucción k: antes (excepción: no escribió nada) o
 * después de completarla. Se emite al final, en la sección fría, y escribe
 * los registros con valor por la cadena de escritura (chain_call). Con el
 * código ancho, `wb` dice si escribe los registros modificados o si el sitio
 * ya los escribió antes de llamar a C (emit_load/emit_store). */
static int exit_label(struct ctx *c, unsigned k, bool after, bool wb)
{
	int *slot = after ? &c->exit_after[k] : &c->exit_before[k];
	if (*slot >= 0)
		return *slot;
	struct pending_exit *p = &c->pending[c->npending++];
	p->label = (uint16_t)new_label(c);
	p->k = (uint8_t)k;
	p->after = after;
	p->fflush = c->fpu_res ? (uint8_t)((c->fpu_ran ? 1 : 0) | (c->fc_written ? 2 : 0)) : 0;
	p->mask = wb && c->wide ? c->written_start | (after ? c->d[k].writes & c->cached : 0) : 0;
	*slot = p->label;
	return p->label;
}

/* Salida después de k cuando el intérprete acaba de correr una instrucción
 * de COP1: cpu->fcr31 es la verdad y no se escribe desde el CR. */
static int exit_label_mem(struct ctx *c, unsigned k)
{
	if (c->exit_after_mem[k] >= 0)
		return c->exit_after_mem[k];
	struct pending_exit *p = &c->pending[c->npending++];
	p->label = (uint16_t)new_label(c);
	p->k = (uint8_t)k;
	p->after = true;
	p->fflush = 0;
	p->mask = c->wide ? c->written_start | (c->d[k].writes & c->cached) : 0;
	c->exit_after_mem[k] = p->label;
	return p->label;
}

static void emit_pending_exits(struct ctx *c)
{
	c->sec = SEC_COLD;
	for (unsigned e = 0; e < c->npending; e++) {
		const struct pending_exit *p = &c->pending[e];
		bind(c, p->label);
		if (p->fflush)
			fpu_flush_bits(c, p->fflush & 1, (p->fflush & 2) != 0);
		if (c->wide)
			writeback(c, p->mask);
		else
			chain_call(c, p->k, p->after);
		E(c, ppc_addi(R_CYCLES, R_CYCLES, (int32_t)p->k + 1));
		if (p->after)
			emit_continue(c, p->k);
		else if (c->wide)
			li64(c, 3, insn_pc(c, p->k));  /* no se usa: hay excepción (redirect); el compacto no lo emite */
		restore_borrowed(c);
		emit_b(c, abs_label(c, c->rec->exit_mid), false);
	}
}

/* Salida a una dirección fija, con los registros ya escritos y cycles al día.
 * Deja un enlace que rec.c parchea con un `b` al bloque destino:
 *   nop ; bl exit_link ; <destino, 32 bits>
 * exit_link lee el destino de la palabra que sigue al bl (nunca se ejecuta:
 * no vuelve) y calcula el nop = LR - 8. Hasta el 2026-10-13 el destino iba
 * en lis/ori antes del bl: una palabra más en cada salida. */
static void emit_exit_to(struct ctx *c, uint64_t target)
{
	fpu_flush(c);
	restore_borrowed(c);
	if (!c->temp && !c->rec->no_link && (int64_t)target == (int32_t)target) {
		E(c, PPC_NOP);
		emit_b(c, abs_label(c, c->rec->exit_link), true);
		E(c, (uint32_t)target);
		return;
	}
	li64(c, 3, target);
	emit_b(c, abs_label(c, c->rec->exit_jump), false);
}

/* Salto tomado: vuelta al cuerpo si va al propio bloque (con la prueba del
 * próximo evento), o salida enlazada. Con no_link (--lockstep) los bucles
 * siguen: se compara al salir de ellos. */
static void emit_taken(struct ctx *c, uint64_t target)
{
	if (target != c->pc || c->temp) {
		emit_exit_to(c, target);
		return;
	}
	fpu_loop_back(c);  /* FCR31 a memoria y los registros de la FPU como a la entrada */
	E(c, ppc_addi(0, R_CYCLES, (int32_t)c->n));
	E(c, ppc_cmpld(0, 0, R_NEXT));
	emit_bc(c, PPC_BO_FALSE_LIKELY, PPC_BI(0, PPC_GT), c->body);
	restore_borrowed(c);
	li64(c, 3, c->pc);
	emit_b(c, abs_label(c, c->rec->exit_mid), false);
}

/* Perfil persistente (rec_profile.h): el sitio de salida (k, clase,
 * destino) de un bloque caliente cuenta cuántas veces se pasa por él:
 * cuatro palabras en el camino que sale, con la dirección absoluta del
 * contador (la tabla vive en .bss: cabe en 32 bits; si no, no se cuenta).
 * r11 y r12 están libres en todos los sitios donde se llama. */
static void emit_site(struct ctx *c, unsigned k, unsigned kind, uint64_t target)
{
	struct salsa64_rec *rec = c->rec;
	if (!c->hot || !c->pent || c->temp || !rec->profile || !rec->profile_sites)
		return;
	unsigned j = salsa64_rec_profile_site(rec->profile, c->pent, k, kind, (uint32_t)target);
	if (!j)
		return;
	uintptr_t a = (uintptr_t)&rec->profile->sites[j - 1].taken;
	if (a >= 0x7FFF8000u)
		return;
	int32_t hi = ((int32_t)a + 0x8000) >> 16, lo = (int32_t)a - (hi << 16);
	E(c, ppc_lis(T_X, hi));
	E(c, ppc_lwz(T_Y, lo, T_X));
	E(c, ppc_addi(T_Y, T_Y, 1));
	E(c, ppc_stw(T_Y, lo, T_X));
}

/* ---- Llamadas a C ---------------------------------------------------------------- */

/* Lo que el intérprete deja en la estructura antes de cada instrucción. */
static void call_prep(struct ctx *c, unsigned k)
{
	E(c, ppc_addi(0, R_CYCLES, (int32_t)k + 1));
	E(c, ppc_std(0, OFF(cycles), R_CPU));
	li64(c, 0, insn_pc(c, k));
	E(c, ppc_std(0, OFF(cur_pc), R_CPU));
	E(c, ppc_li(0, c->in_ds));
	E(c, ppc_stb(0, OFF(delay_slot), R_CPU));
}

static void emit_call(struct ctx *c, uint64_t entry)
{
	struct salsa64_rec *r = c->rec;
	int64_t rel = (int64_t)entry - (int64_t)(uintptr_t)r->code;
	if (c->calls_near) {
		emit_b(c, abs_label(c, rel / 4), true);
		return;
	}
	li64(c, T_Y, entry);
	E(c, ppc_mtctr(T_Y));
	E(c, PPC_BCTRL);
}

/* Después de una llamada: salir si hubo excepción, interrupción, código
 * invalidado o cambio de modo, o si el bloque ya no cabe antes del próximo
 * evento. */
static void post_check_to(struct ctx *c, int out)
{
	E(c, ppc_lwz(0, OFF(irq_pending), R_CPU));
	E(c, ppc_cmpwi(CR_T, 0, 0));
	emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), out);
	E(c, ppc_ld(T_X, OFF(sched), R_CPU));
	E(c, ppc_ld(R_NEXT, (int32_t)offsetof(struct salsa64_sched, next), T_X));
	E(c, ppc_addi(0, R_CYCLES, (int32_t)c->n));
	E(c, ppc_cmpld(CR_T, 0, R_NEXT));
	emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_GT), out);
}

/* La instrucción k al intérprete (salsa64_vr4300_exec_one). Compacto: la
 * rutina compartida slow_exec, que deja cr5 gt si hay que salir
 * (emit_exec_check lo mira después de recargar lo que la instrucción
 * escribió). Ancho: todo en línea. */
static void emit_exec_call(struct ctx *c, unsigned k)
{
	if (c->wide) {
		call_prep(c, k);
		E(c, ppc_mr(3, R_CPU));
		li32u(c, 4, c->d[k].w);
		emit_call(c, c->rec->h_exec);
		return;
	}
	li32s(c, 4, c->d[k].w);
	emit_slow_call(c, SLOW_EXEC, 0, k);
}

/* Muestreo (rec_sample.h): las llamadas a C que no pasan por la rutina
 * compartida (TLBWI/TLBWR, ERET) marcan «camino lento» alrededor; sin
 * muestreo no se emite nada. */
static void emit_sample_mark(struct ctx *c, unsigned mark)
{
	if (!c->rec->sample)
		return;
	E(c, ppc_li(0, (int32_t)mark));
	E(c, ppc_stb(0, OFF(rec_mark), R_CPU));
}

/* Después de emit_exec_call (y de recargar): salir a `out` si hace falta. */
static void emit_exec_check(struct ctx *c, int out)
{
	if (c->wide)
		post_check_to(c, out);
	else
		emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(5, PPC_GT), out);
}

static void post_check(struct ctx *c, unsigned k)
{
	emit_exec_check(c, exit_label(c, k, true, true));
}

/* Tras una instrucción de COP1 corrida por el intérprete (rec_emit_fpu.h). */
static void post_check_mem(struct ctx *c, unsigned k)
{
	emit_exec_check(c, exit_label_mem(c, k));
}

/* Instrucción que corre el intérprete. */
static void emit_exec(struct ctx *c, unsigned k)
{
	const struct dec *d = &c->d[k];
	bool cop1 = c->fpu_res && I_OP(d->w) == 0x11;
	/* El intérprete lee y escribe cpu->gpr: antes, los que toca y están
	 * modificados (o fijos); después, los que escribe. */
	store_regs(c, (d->reads | d->writes) & (c->written | c->pinned));
	if (cop1)  /* y lee FCR31 */
		fpu_flush_bits(c, c->fpu_ran, c->fc_written);
	emit_exec_call(c, k);
	reload(c, d->writes);
	if (d->kind == K_END && SALSA64_REC_OLD(c->rec, SALSA64_REC_DBG_NO_ENDLINK)) {
		/* Como antes: al despachador, que interpreta el resto del bloque básico. */
		fpu_flush(c);
		writeback(c, c->written);
		E(c, ppc_addi(R_CYCLES, R_CYCLES, (int32_t)k + 1));
		emit_continue(c, k);
		restore_borrowed(c);
		emit_b(c, abs_label(c, c->rec->exit_mid), false);
		return;
	}
	if (cop1) {
		unsigned fmt = I_FMT(d->w);
		post_check_mem(c, k);
		fpu_after_cop1_call(c, k);
		if (fmt >= FMT_S && fmt <= FMT_L) {  /* escribió fd por fuera */
			c->ffmt[I_FD(d->w)] = 0;
			c->fw_ok[I_FD(d->w)] = 0;
		}
		return;
	}
	post_check(c, k);
	if (d->kind == K_END) {
		/* Última del bloque (COP0): rec_h_exec ya hizo lo que el despachador
		 * haría antes del bloque siguiente (rec_tlb.h, Status.FR) y
		 * post_check revisó el modo, las interrupciones y el evento: se
		 * enlaza con el siguiente como cualquier salida a dirección fija. */
		writeback(c, c->written);
		E(c, ppc_addi(R_CYCLES, R_CYCLES, (int32_t)k + 1));
		emit_exit_to(c, insn_pc(c, k + 1));
	}
}

/* Excepción segura (SYSCALL, BREAK) o condicional (desbordamiento, en la
 * sección fría). Siempre sale. */
static void emit_exception(struct ctx *c, unsigned k, unsigned code)
{
	if (c->wide) {
		call_prep(c, k);
		E(c, ppc_mr(3, R_CPU));
		E(c, ppc_li(4, (int32_t)code));
		emit_call(c, c->rec->h_exception);
	} else {
		E(c, ppc_li(4, (int32_t)code));
		emit_slow_call(c, SLOW_EXC, 0, k);
	}
	emit_b(c, exit_label(c, k, false, true), false);
}

/* Rama a un camino frío que lanza la excepción de desbordamiento. */
static void branch_overflow(struct ctx *c, unsigned bo, unsigned bi)
{
	int l = new_label(c);
	emit_bc(c, bo, bi, l);
	unsigned sec = c->sec;
	c->sec = SEC_COLD;
	bind(c, l);
	emit_exception(c, c->k, SALSA64_EXC_OV);
	c->sec = sec;
}

/* ---- Memoria ------------------------------------------------------------------- */

/*
 * Dirección efectiva en r3 y prueba del camino rápido: KSEG0 o KSEG1 (el
 * bloque solo corre en modo kernel), dentro de la RDRAM y alineada. Con
 * RDRAM de 2^s bytes, (va >> s) vale -2^(31-s) en KSEG0 y -2^(31-s) + 2^(29-s)
 * en KSEG1; el `ori` junta los dos casos. Deja en r12 el desplazamiento
 * dentro de la RDRAM.
 *
 * Si no es KSEG0/KSEG1, en la sección fría se prueba ktlb[] (rec_tlb.h): la
 * dirección es extensión de signo de 32 bits y está alineada, su página
 * tiene la etiqueta y es válida (y D, al escribir) y la física cae en la
 * RDRAM; entonces r12 queda igual y se vuelve al camino rápido. Lo demás
 * (sin traducción, E/S, excepciones) va a `slow`, que llama a C; el camino
 * lento anota la página en ktlb[] para la próxima vez.
 */
static void emit_kseg_test(struct ctx *c, unsigned va, unsigned align);
static void emit_store_check(struct ctx *c, int slow);

/* El tramo frío de emit_addr: la dirección a r3 si no estaba, la rutina
 * compartida del preámbulo (rec.c: cr1 eq y r12 listo, o ne) y, si traduce,
 * de vuelta al camino rápido. Si no traduce sigue en lo que venga después:
 * quien lo llama emite a continuación el camino lento del acceso. */
static void emit_addr_cold(struct ctx *c)
{
	const struct salsa64_rec *r = c->rec;
	unsigned align = c->addr.align;
	if (!c->addr.pending)
		return;
	c->addr.pending = false;
	if (c->addr.fm) {
		/* fastmem: la prueba de siempre y la copia del acceso (con su
		 * prueba de code_flags si almacena), para cuando el acceso
		 * caliente falló y quedó parchado hacia aquí; de vuelta a la
		 * instrucción que sigue al acceso. */
		bind(c, c->addr.fm_cold);
		emit_kseg_test(c, c->addr.va, align);
		emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), c->addr.mapped);
		bind(c, c->addr.back);
		if (c->addr.check)
			emit_store_check(c, c->addr.slow);
		E(c, c->addr.word);
		emit_b(c, c->addr.fm_after, false);
	}
	bind(c, c->addr.mapped);
	if (c->addr.va != T_ADDR)
		E(c, ppc_mr(T_ADDR, c->addr.va));
	emit_b(c, abs_label(c, r->ktlb_rtn[c->addr.store][align == 0 ? 0 : align == 1 ? 1 : align == 3 ? 2 : 3]), true);
	emit_bc(c, PPC_BO_TRUE_LIKELY, PPC_BI(CR_T, PPC_EQ), c->addr.back);
}

/* La prueba compacta de KSEG0/KSEG1 (ver emit_addr): deja el desplazamiento
 * en T_Y y cr1 eq si la dirección vale. */
static void emit_kseg_test(struct ctx *c, unsigned va, unsigned align)
{
	unsigned s = c->rec->rdram_shift, abits = align == 7 ? 3 : align == 3 ? 2 : align;
	E(c, ppc_oris(T_Y, va, 0x2000));
	E(c, ppc_addis(T_Y, T_Y, 0x6000));
	E(c, ppc_rldicl(T_X, T_Y, 64 - s, s - abits));
	E(c, ppc_cmpdi(CR_T, T_X, 0));
}

/* La prueba de code_flags de un almacenamiento en T_Y (una entrada por 128
 * bytes): con código recompilado ahí, por el camino lento. Con fastmem el
 * índice se recorta a la tabla (fuera de la RDRAM apunta a cualquier
 * bandera: si está puesta el almacenamiento va a C, que es correcto; si no,
 * falla y se parchea). */
static void emit_store_check(struct ctx *c, int slow)
{
	if (c->addr.fm)
		E(c, ppc_rlwinm(T_X, T_Y, 32 - SALSA64_REC_FLAG_SHIFT, 16, 31));
	else
		E(c, ppc_srdi(T_X, T_Y, SALSA64_REC_FLAG_SHIFT));
	E(c, ppc_lbzx(T_X, R_FLAGS, T_X));
	E(c, ppc_cmpwi(CR_T, T_X, 0));
	emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), slow);
}

/* El acceso de emit_load/emit_store. Con fastmem (emit_addr con `fm`), es
 * el sitio que se parchea si falla: queda anotado con su camino frío
 * (rec->fm_pend; finalize los pasa a palabras de la caché). */
static void fm_access(struct ctx *c, uint32_t word, bool check)
{
	struct salsa64_rec *r = c->rec;
	if (!c->addr.fm) {
		E(c, word);
		return;
	}
	int site = new_label(c);
	bind(c, site);
	E(c, word);
	bind(c, c->addr.fm_after);
	c->addr.word = word;
	c->addr.check = check;
	if (r->fm_npend < SALSA64_REC_MAX_INSNS) {
		r->fm_pend_site[r->fm_npend] = (uint16_t)site;
		r->fm_pend_cold[r->fm_npend] = (uint16_t)c->addr.fm_cold;
		r->fm_npend++;
	} else {
		r->overflow = true;
	}
}

/* ---- Base en caché (docs/fastmem.md §8) ------------------------------------------ */

/* Un acceso que entra en un grupo (los de emit_load y emit_store): tamaño y
 * si almacena. */
static bool group_access(uint32_t w, unsigned *size, bool *store)
{
	switch (I_OP(w)) {
	case 0x20: case 0x24: case 0x28: *size = 1; *store = I_OP(w) == 0x28; return true;
	case 0x21: case 0x25: case 0x29: *size = 2; *store = I_OP(w) == 0x29; return true;
	case 0x23: case 0x27: case 0x2B: case 0x31: case 0x39: *size = 4; *store = I_OP(w) == 0x2B || I_OP(w) == 0x39; return true;
	case 0x37: case 0x3F: case 0x35: case 0x3D: *size = 8; *store = I_OP(w) == 0x3F || I_OP(w) == 0x3D; return true;
	}
	return false;
}

/* Instrucciones que pisan T_HI en el camino caliente: MULT y compañía
 * (emit_hilo) y LWL/LWR y compañía (rec_emit_ext.h). Cortan el grupo. (La
 * escritura de FCR31 de las operaciones de COP1 usaba r7; ahora T_V.) */
static bool clobbers_thi(uint32_t w)
{
	unsigned op = I_OP(w);
	if (op == 0)
		return (I_FUNCT(w) & ~7u) == 0x18;
	return op == 0x1A || op == 0x1B || op == 0x22 || op == 0x26 || op == 0x2A || op == 0x2C || op == 0x2D || op == 0x2E;
}

/* Arma los grupos del bloque: desde cada acceso, los que siguen con el
 * mismo base hasta que se escribe el base, una instrucción pisa T_HI o
 * hay un destino de salto interno (al volver por ahí el base pudo cambiar
 * después del grupo); tramo de imm de hasta 32 KB; la alineación de cada
 * miembro respecto del menor imm es la de su tamaño. Grupos de uno no
 * cuentan. */
static void group_pass(struct ctx *c)
{
	unsigned n = c->n;
	bool target[SALSA64_REC_MAX_INSNS];
	for (unsigned k = 0; k < n; k++) {
		c->grp[k].head = GRP_NONE;
		target[k] = false;
	}
	c->grp_live = false;
	if (c->wide || c->fastmem || (c->rec->opts & SALSA64_REC_OPT_NO_GROUP))
		return;
	for (unsigned k = 0; k < n; k++) {
		const struct dec *d = &c->d[k];
		if (d->kind == K_BRANCH || d->kind == K_BRANCH_L) {
			int64_t t = (int64_t)k + 1 + (int16_t)d->w;
			if (t > (int64_t)k && t < (int64_t)n)
				target[t] = true;
		}
	}
	/* Un solo T_HI: mientras un grupo está abierto (hasta su último
	 * miembro) ningún acceso con otro base empieza otro; esos van con su
	 * prueba, que no toca T_HI. */
	int cover = -1;
	for (unsigned k = 0; k < n; k++) {
		unsigned size;
		bool st;
		if ((int)k <= cover || c->grp[k].head != GRP_NONE || !group_access(c->d[k].w, &size, &st))
			continue;
		unsigned rs = I_RS(c->d[k].w);
		if (rs == 0)
			continue;
		/* El delay slot de un branch-likely solo corre si se toma: no puede
		 * ser la cabeza (los miembros de después no verían T_HI). */
		if (c->ds_of[k] && c->d[c->ds_of[k] - 1].kind == K_BRANCH_L)
			continue;
		unsigned members[SALSA64_REC_MAX_INSNS], nm = 0, big = 1;
		int lo = 0x7FFF, hi = -0x8000;
		bool any_store = false;
		for (unsigned j = k; j < n; j++) {
			uint32_t w = c->d[j].w;
			if (j > k && (target[j] || clobbers_thi(w)))
				break;
			unsigned sz;
			bool s2;
			if (group_access(w, &sz, &s2) && I_RS(w) == rs && c->grp[j].head == GRP_NONE) {
				int imm = (int16_t)w;
				int nlo = imm < lo ? imm : lo, nhi = imm > hi ? imm : hi;
				if (nhi - nlo > 0x7FF8)
					break;
				lo = nlo;
				hi = nhi;
				members[nm++] = j;
				if (sz > big)
					big = sz;
				any_store |= s2;
			}
			if (c->d[j].writes & BIT(rs))
				break;
		}
		if (nm < 2)
			continue;
		/* va_lo = base + lo con lo redondeado hacia abajo a la alineación
		 * del acceso más grande: así un byte en +1 no deja fuera a las
		 * palabras en +0x18 y +0x30 (la prueba exige x_lo alineado a `big`
		 * de todos modos). Un miembro cuyo desplazamiento respecto de lo no
		 * es múltiplo de su tamaño va con su prueba. */
		unsigned align = big - 1;
		lo -= ((lo % (int)big) + (int)big) % (int)big;
		if (hi - lo > 0x7FF8)
			continue;
		unsigned kept[SALSA64_REC_MAX_INSNS], nk = 0;
		for (unsigned m = 0; m < nm; m++) {
			unsigned sz;
			bool s2;
			group_access(c->d[members[m]].w, &sz, &s2);
			if ((((int16_t)c->d[members[m]].w - lo) % (int)sz) == 0)
				kept[nk++] = members[m];
		}
		/* La cabeza final tampoco puede ser el delay slot de un
		 * branch-likely (el primero sí se revisó arriba, pero el filtro
		 * pudo dejar fuera al primero). */
		unsigned first = 0;
		while (first < nk && c->ds_of[kept[first]] && c->d[c->ds_of[kept[first]] - 1].kind == K_BRANCH_L)
			first++;
		if (nk - first < 2)
			continue;
		uint16_t span = (uint16_t)((unsigned)(hi - lo) & ~align);
		cover = (int)kept[nk - 1];
		for (unsigned m = first; m < nk; m++) {
			struct grp *g = &c->grp[kept[m]];
			g->head = (uint8_t)kept[first];
			g->align = (uint8_t)align;
			g->last = m == nk - 1;
			g->store = any_store;
			g->min_imm = (int16_t)lo;
			g->span = span;
		}
	}
}

/* La cabeza del grupo: la prueba compacta sobre x_lo + span (segmento,
 * tamaño y alineación de una vez) y T_HI. Si falla, en la sección fría la
 * ktlb con va_lo: si traduce y el tramo cabe en la página, T_HI sale de la
 * física; si no, al intérprete antes de esta instrucción (los miembros no
 * llevan prueba propia). */
static void emit_group_head(struct ctx *c, unsigned k, int slow)
{
	struct salsa64_rec *r = c->rec;
	const struct grp *g = &c->grp[k];
	unsigned s = r->rdram_shift, abits = g->align == 7 ? 3 : g->align == 3 ? 2 : g->align;
	int cold = new_label(c), back = new_label(c);
	unsigned base = get(c, I_RS(c->d[k].w), T_ADDR), va = T_ADDR;
	if (g->min_imm == 0 && base != T_ADDR)
		va = base;
	else
		E(c, ppc_addi(T_ADDR, base, g->min_imm));
	E(c, ppc_oris(T_Y, va, 0x2000));
	E(c, ppc_addis(T_Y, T_Y, 0x6000));
	if (g->span)
		E(c, ppc_addi(T_Y, T_Y, g->span));
	E(c, ppc_rldicl(T_X, T_Y, 64 - s, s - abits));
	E(c, ppc_cmpdi(CR_T, T_X, 0));
	emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), cold);
	bind(c, back);
	E(c, ppc_add(T_HI, R_RDRAM, T_Y));
	c->grp_live = true;
	c->grp_cur = k;
	r->stats.grp_heads++;

	/* Frío: la ktlb con va_lo; si traduce y el tramo cabe en la página,
	 * de vuelta con T_Y = física; si no, `slow`: quien llama hace el
	 * acceso de la cabeza por C (que anota la página si traduce) y vuelve a
	 * probar (emit_group_reprobe). */
	unsigned sec = c->sec;
	c->sec = SEC_COLD;
	bind(c, cold);
	if (va != T_ADDR)
		E(c, ppc_mr(T_ADDR, va));
	emit_b(c, abs_label(c, r->ktlb_rtn[g->store][g->align == 0 ? 0 : g->align == 1 ? 1 : g->align == 3 ? 2 : 3]), true);
	emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(1, PPC_EQ), slow);
	if (g->span) {
		E(c, ppc_rlwinm(0, T_Y, 0, 20, 31));  /* desplazamiento en la página de 4 KB */
		E(c, ppc_cmplwi(CR_T, 0, 0xFFFu - g->span));
		emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_GT), slow);
		E(c, ppc_addi(T_Y, T_Y, g->span));
	}
	emit_b(c, back, false);
	c->sec = sec;
}

/* Tras el acceso de la cabeza por C (sección fría): la ktlb otra vez con
 * va_lo (la C anotó la página si traduce) y T_HI; si sigue sin traducir, o
 * el tramo no cabe en la página, el intérprete sigue después de la cabeza. */
static void emit_group_reprobe(struct ctx *c, unsigned k)
{
	struct salsa64_rec *r = c->rec;
	const struct grp *g = &c->grp[k];
	int give_up = exit_label(c, k, true, false);
	unsigned base = get(c, I_RS(c->d[k].w), T_ADDR);
	if (g->min_imm || base != T_ADDR)
		E(c, ppc_addi(T_ADDR, base, g->min_imm));
	emit_b(c, abs_label(c, r->ktlb_rtn[g->store][g->align == 0 ? 0 : g->align == 1 ? 1 : g->align == 3 ? 2 : 3]), true);
	emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(1, PPC_EQ), give_up);
	if (g->span) {
		E(c, ppc_rlwinm(0, T_Y, 0, 20, 31));
		E(c, ppc_cmplwi(CR_T, 0, 0xFFFu - g->span));
		emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_GT), give_up);
		E(c, ppc_addi(T_Y, T_Y, g->span));
	}
	E(c, ppc_add(T_HI, R_RDRAM, T_Y));
}

static unsigned emit_addr(struct ctx *c, unsigned rs, int16_t imm, unsigned align, int slow, bool store, bool fm)
{
	struct salsa64_rec *r = c->rec;
	unsigned s = r->rdram_shift;
	int mapped = new_label(c), back = new_label(c);
	unsigned base = get(c, rs, T_ADDR), va = T_ADDR;
	if (!c->wide && imm == 0 && base != T_ADDR)
		va = base;  /* la dirección ya está en un registro: los caminos lentos la copian a r3 */
	else
		E(c, ppc_addi(T_ADDR, base, imm));
	c->addr.fm = false;
	if (fm && c->fastmem && !SALSA64_REC_OLD(r, SALSA64_REC_DBG_NO_DATA)) {
		/* fastmem: índice en la ventana de 512 MB (va & 0x1FFFFFFF, en 32
		 * bits), sin prueba: KSEG0 y KSEG1 caen en la RDRAM y lo demás en
		 * páginas sin mapear, donde el acceso falla y se parchea hacia
		 * fm_cold (emit_addr_cold: la prueba de siempre y la copia del
		 * acceso). `back` y `mapped` quedan en la sección fría. */
		E(c, ppc_rlwinm(T_Y, va, 0, 3, 31));
		c->addr.mapped = mapped;
		c->addr.back = back;
		c->addr.va = (uint8_t)va;
		c->addr.align = (uint8_t)align;
		c->addr.store = store;
		c->addr.pending = true;
		c->addr.fm = true;
		c->addr.fm_cold = new_label(c);
		c->addr.fm_after = new_label(c);
		c->addr.slow = slow;
		c->addr.check = false;
		r->stats.fm_sites++;
		return va;
	}
	if (c->wide) {
		int32_t k1 = 1 << (29 - s), region = -(1 << (31 - s)) | k1;
		E(c, ppc_sradi(T_X, T_ADDR, s));
		E(c, ppc_ori(T_X, T_X, (uint32_t)k1));
		E(c, ppc_addi(T_X, T_X, -region));
		if (align) {
			E(c, ppc_clrldi(T_Y, T_ADDR, align == 1 ? 63 : align == 3 ? 62 : 61));
			E(c, ppc_or(T_X, T_X, T_Y));
		}
		E(c, ppc_cmpdi(CR_T, T_X, 0));
	} else {
		/* Compacto: el bit 29 puesto lleva KSEG0 a KSEG1, y sumar 2^30 + 2^29
		 * a 0xFFFFFFFF_A0000000 da 2^64: de una dirección de KSEG0 o KSEG1
		 * dentro de la RDRAM quedan solo los bits del desplazamiento (r12,
		 * listo para el acceso). Rotar los bits del desplazamiento por
		 * encima de la alineación al tope y recortarlos deja 0 si vale. */
		emit_kseg_test(c, va, align);
	}
	c->addr.pending = false;
	if (SALSA64_REC_OLD(r, SALSA64_REC_DBG_NO_DATA)) {  /* sin datos mapeados: todo lo demás a C */
		emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), slow);
		if (c->wide)
			E(c, ppc_clrldi(T_Y, T_ADDR, 64 - s));
		return va;
	}
	emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), mapped);
	if (c->wide)
		E(c, ppc_clrldi(T_Y, T_ADDR, 64 - s));
	bind(c, back);

	/* La prueba de la ktlb (sección fría): compacto, pegada al camino lento
	 * del acceso (emit_addr_cold, que llama quien emite ese camino); ancho,
	 * aquí mismo, con un salto más. */
	c->addr.mapped = mapped;
	c->addr.back = back;
	c->addr.va = (uint8_t)va;
	c->addr.align = (uint8_t)align;
	c->addr.store = store;
	c->addr.pending = true;
	if (c->wide) {
		unsigned sec = c->sec;
		c->sec = SEC_COLD;
		emit_addr_cold(c);
		emit_b(c, slow, false);
		c->sec = sec;
	}
	return va;
}

/* Registros de la FPU en cpu->fpr según el Status.FR con que se compila
 * (el host es big-endian: la mitad baja de un registro va en +4). */
static int32_t fpr32_off(const struct ctx *c, unsigned r)
{
	if (c->rec->fr || !(r & 1))
		return OFF(fpr) + 8 * (int32_t)r + 4;
	return OFF(fpr) + 8 * (int32_t)(r & ~1u);
}

static int32_t fpr64_off(const struct ctx *c, unsigned r)
{
	return OFF(fpr) + 8 * (int32_t)(c->rec->fr ? r : r & ~1u);
}

enum { DST_GPR, DST_FPR32, DST_FPR64 };

static void emit_load(struct ctx *c, unsigned k, unsigned kind, unsigned where)
{
	static const uint8_t align[] = { 0, 0, 1, 1, 3, 3, 7 };
	uint32_t i = c->d[k].w;
	unsigned rt = I_RT(i);
	int slow = new_label(c), resume = new_label(c);

	const struct grp *g = &c->grp[k];
	if (g->head != GRP_NONE) {
		/* Base en caché: sin prueba ni camino frío (la cabeza probó el
		 * tramo entero); el valor directo de T_HI + desplazamiento. */
		if (g->head == k)
			emit_group_head(c, k, slow);
		int32_t disp = (int16_t)i - g->min_imm - g->span;
		unsigned d = where == DST_GPR ? dst(c, rt, T_V) : T_V;
		switch (kind) {
		case SALSA64_REC_LB:  E(c, ppc_lbz(d, disp, T_HI)); E(c, ppc_extsb(d, d)); break;
		case SALSA64_REC_LBU: E(c, ppc_lbz(d, disp, T_HI)); break;
		case SALSA64_REC_LH:  E(c, ppc_lhz(d, disp, T_HI)); E(c, ppc_extsh(d, d)); break;
		case SALSA64_REC_LHU: E(c, ppc_lhz(d, disp, T_HI)); break;
		case SALSA64_REC_LW:  E(c, ppc_lwz(d, disp, T_HI)); E(c, ppc_extsw(d, d)); break;
		case SALSA64_REC_LWU: E(c, ppc_lwz(d, disp, T_HI)); break;
		default:              E(c, ppc_ld(d, disp, T_HI)); break;
		}
		if (where == DST_FPR32)
			E(c, ppc_stw(d, fpr32_off(c, rt), R_CPU));
		else if (where == DST_FPR64)
			E(c, ppc_std(d, fpr64_off(c, rt), R_CPU));
		if (where != DST_GPR && c->fpu_res)
			fpu_loaded(c, k, rt, where == DST_FPR64, d, false);  /* de casa: T_Y no es el desplazamiento */
		bind(c, resume);
		if (where == DST_GPR)
			put(c, rt, d);
		c->rec->stats.grp_members++;
		if (g->head == k) {
			/* La cabeza sin ktlb: su carga por C, el valor a su lugar y la
			 * prueba de nuevo para los que siguen. */
			unsigned save_sec = c->sec;
			c->sec = SEC_COLD;
			bind(c, slow);
			unsigned base = get(c, I_RS(i), T_ADDR);
			if ((int16_t)i || base != T_ADDR)
				E(c, ppc_addi(T_ADDR, base, (int16_t)i));
			emit_slow_call(c, SLOW_LOAD, kind, k);
			emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(1, PPC_EQ), exit_label(c, k, false, false));
			if (d != 3)
				E(c, ppc_mr(d, 3));
			if (where == DST_FPR32)
				E(c, ppc_stw(d, fpr32_off(c, rt), R_CPU));
			else if (where == DST_FPR64)
				E(c, ppc_std(d, fpr64_off(c, rt), R_CPU));
			else if (rt && !c->map[rt])
				E(c, ppc_std(d, off_reg(rt), R_CPU));
			if (where != DST_GPR && c->fpu_res)
				fpu_loaded(c, k, rt, where == DST_FPR64, d, false);
			emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(5, PPC_GT), exit_label(c, k, true, false));
			emit_group_reprobe(c, k);
			emit_b(c, resume, false);
			c->sec = save_sec;
		}
		if (g->last)
			c->grp_live = false;
		return;
	}
	unsigned va = emit_addr(c, I_RS(i), (int16_t)i, align[kind], slow, false, true);
	unsigned d = where == DST_GPR ? dst(c, rt, T_V) : T_V;
	switch (kind) {
	case SALSA64_REC_LB:  fm_access(c, ppc_lbzx(d, R_RDRAM, T_Y), false); E(c, ppc_extsb(d, d)); break;
	case SALSA64_REC_LBU: fm_access(c, ppc_lbzx(d, R_RDRAM, T_Y), false); break;
	case SALSA64_REC_LH:  fm_access(c, ppc_lhzx(d, R_RDRAM, T_Y), false); E(c, ppc_extsh(d, d)); break;
	case SALSA64_REC_LHU: fm_access(c, ppc_lhzx(d, R_RDRAM, T_Y), false); break;
	case SALSA64_REC_LW:  fm_access(c, ppc_lwzx(d, R_RDRAM, T_Y), false); E(c, ppc_extsw(d, d)); break;
	case SALSA64_REC_LWU: fm_access(c, ppc_lwzx(d, R_RDRAM, T_Y), false); break;
	default:              fm_access(c, ppc_ldx(d, R_RDRAM, T_Y), false); break;
	}
	/* COP1: a casa y al registro residente directo de la RDRAM (T_Y),
	 * antes de `resume`: el camino lento llega ahí sin T_Y y lo carga de
	 * casa él mismo. */
	if (where == DST_FPR32)
		E(c, ppc_stw(d, fpr32_off(c, rt), R_CPU));
	else if (where == DST_FPR64)
		E(c, ppc_std(d, fpr64_off(c, rt), R_CPU));
	if (where != DST_GPR && c->fpu_res)
		fpu_loaded(c, k, rt, where == DST_FPR64, d, true);
	bind(c, resume);
	/* El valor queda en d por los dos caminos. Camino lento: la rutina
	 * compartida del preámbulo (rec.c, slow_load), por el trampolín del
	 * bloque; cr1 ne es excepción (rt sin escribir) y cr5 gt, salir después
	 * de la instrucción con el valor ya en su lugar (la cadena de escritura
	 * se encarga de rt si está en un registro del bloque). */
	unsigned save_sec = c->sec;
	c->sec = SEC_COLD;
	if (!c->wide) {
		emit_addr_cold(c);
		bind(c, slow);
		if (va != T_ADDR)
			E(c, ppc_mr(T_ADDR, va));
		emit_slow_call(c, SLOW_LOAD, kind, k);
		emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(1, PPC_EQ), exit_label(c, k, false, false));
		if (d != 3)
			E(c, ppc_mr(d, 3));
		if (where == DST_FPR32)
			E(c, ppc_stw(d, fpr32_off(c, rt), R_CPU));
		else if (where == DST_FPR64)
			E(c, ppc_std(d, fpr64_off(c, rt), R_CPU));
		else if (rt && !c->map[rt])
			E(c, ppc_std(d, off_reg(rt), R_CPU));
		if (where != DST_GPR && c->fpu_res)
			fpu_loaded(c, k, rt, where == DST_FPR64, d, false);
		emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(5, PPC_GT), exit_label(c, k, true, false));
		emit_b(c, resume, false);
	} else {
		/* Ancho: lo sucio se escribe antes de llamar, para que las salidas
		 * no tengan que hacerlo. */
		bind(c, slow);
		store_regs(c, c->written_start);
		li64(c, 7, c->pc);
		li32u(c, 6, slow_arg(c, SLOW_LOAD, kind, k) | (uint32_t)c->n << 16);
		emit_b(c, abs_label(c, c->rec->slow_rtn), true);
		emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(1, PPC_EQ), exit_label(c, k, false, false));
		int out = new_label(c);
		emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(5, PPC_GT), out);
		if (d != 3)
			E(c, ppc_mr(d, 3));
		if (where == DST_FPR32)
			E(c, ppc_stw(d, fpr32_off(c, rt), R_CPU));
		else if (where == DST_FPR64)
			E(c, ppc_std(d, fpr64_off(c, rt), R_CPU));
		else if (rt && !c->map[rt])
			E(c, ppc_std(d, off_reg(rt), R_CPU));
		if (where != DST_GPR && c->fpu_res)
			fpu_loaded(c, k, rt, where == DST_FPR64, d, false);
		emit_b(c, resume, false);
		/* Salir con el valor en su lugar: en memoria, o en su registro fijo. */
		bind(c, out);
		if (where == DST_FPR32)
			E(c, ppc_stw(3, fpr32_off(c, rt), R_CPU));
		else if (where == DST_FPR64)
			E(c, ppc_std(3, fpr64_off(c, rt), R_CPU));
		else if (rt && (c->pinned & BIT(rt)))
			E(c, ppc_mr(c->map[rt], 3));
		else if (rt)
			E(c, ppc_std(3, off_reg(rt), R_CPU));
		emit_b(c, exit_label(c, k, true, false), false);
	}
	c->sec = save_sec;
	if (where == DST_GPR)
		put(c, rt, d);
}

static void emit_store(struct ctx *c, unsigned k, unsigned kind, unsigned from)
{
	static const uint8_t align[] = { 0, 1, 3, 7 };
	uint32_t i = c->d[k].w;
	unsigned rt = I_RT(i), v = T_S1, fv = 0;
	int slow = c->rec->cpu->rdram_write_marks ? -1 : new_label(c), resume = new_label(c);

	/* SWC1/SDC1 de un valor residente: directo del registro de la FPU a la
	 * RDRAM (rec_emit_fpu.h); el camino lento lo lee de casa. */
	if (from != DST_GPR && c->fpu_res)
		fv = fpu_store_reg(c, rt, from == DST_FPR64);
	if (fv) {
		v = 0;
	} else if (from == DST_FPR32) {
		E(c, ppc_lwz(T_S1, fpr32_off(c, rt), R_CPU));
	} else if (from == DST_FPR64) {
		E(c, ppc_ld(T_S1, fpr64_off(c, rt), R_CPU));
	} else {
		v = get(c, rt, T_S1);
	}
	/* FBWrite records an address even when the CPU rewrites the same pixel
	 * value. Route this title's stores through the helper that marks it. */
	if (c->rec->cpu->rdram_write_marks) {
		unsigned base = get(c, I_RS(i), T_ADDR);
		if ((int16_t)i || base != T_ADDR)
			E(c, ppc_addi(T_ADDR, base, (int16_t)i));
		if (fv)
			E(c, from == DST_FPR64 ? ppc_ld(5, fpr64_off(c, rt), R_CPU) : ppc_lwz(5, fpr32_off(c, rt), R_CPU));
		else if (v != 5)
			E(c, ppc_mr(5, v));
		if (c->wide) {
			store_regs(c, c->written_start);
			li64(c, 7, c->pc);
			li32u(c, 6, slow_arg(c, SLOW_STORE, kind, k) | (uint32_t)c->n << 16);
			emit_b(c, abs_label(c, c->rec->slow_rtn), true);
		} else {
			emit_slow_call(c, SLOW_STORE, kind, k);
		}
		emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(1, PPC_EQ), exit_label(c, k, false, false));
		emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(5, PPC_GT), exit_label(c, k, true, false));
		emit_b(c, resume, false);
		bind(c, resume);
		return;
	}
	const struct grp *g = &c->grp[k];
	if (g->head != GRP_NONE) {
		/* Base en caché: la prueba de code_flags sobre el desplazamiento
		 * (dirección del host menos la RDRAM) y el almacenamiento directo;
		 * el camino lento (código en la zona) recompone la dirección. */
		if (g->head == k)
			emit_group_head(c, k, slow);
		int32_t disp = (int16_t)i - g->min_imm - g->span;
		E(c, ppc_addi(T_X, T_HI, disp));
		E(c, ppc_subf(T_X, R_RDRAM, T_X));
		E(c, ppc_srdi(T_X, T_X, SALSA64_REC_FLAG_SHIFT));
		E(c, ppc_lbzx(T_X, R_FLAGS, T_X));
		E(c, ppc_cmpwi(CR_T, T_X, 0));
		emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), slow);
		if (fv) {
			E(c, from == DST_FPR64 ? ppc_stfd(fv, disp, T_HI) : ppc_stfs(fv, disp, T_HI));
		} else {
			switch (kind) {
			case SALSA64_REC_SB: E(c, ppc_stb(v, disp, T_HI)); break;
			case SALSA64_REC_SH: E(c, ppc_sth(v, disp, T_HI)); break;
			case SALSA64_REC_SW: E(c, ppc_stw(v, disp, T_HI)); break;
			default:             E(c, ppc_std(v, disp, T_HI)); break;
			}
		}
		bind(c, resume);
		c->rec->stats.grp_members++;
		/* Camino lento (código en la zona, o la cabeza sin ktlb): el
		 * almacenamiento por C; la cabeza prueba de nuevo para los que
		 * siguen. */
		unsigned save = c->sec;
		c->sec = SEC_COLD;
		bind(c, slow);
		unsigned base = get(c, I_RS(i), T_ADDR);
		if ((int16_t)i || base != T_ADDR)
			E(c, ppc_addi(T_ADDR, base, (int16_t)i));
		if (fv)
			E(c, from == DST_FPR64 ? ppc_ld(5, fpr64_off(c, rt), R_CPU) : ppc_lwz(5, fpr32_off(c, rt), R_CPU));
		else if (v != 5)
			E(c, ppc_mr(5, v));
		emit_slow_call(c, SLOW_STORE, kind, k);
		emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(1, PPC_EQ), exit_label(c, k, false, false));
		emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(5, PPC_GT), exit_label(c, k, true, false));
		if (g->head == k)
			emit_group_reprobe(c, k);
		emit_b(c, resume, false);
		c->sec = save;
		if (g->last)
			c->grp_live = false;
		return;
	}
	unsigned va = emit_addr(c, I_RS(i), (int16_t)i, align[kind], slow, true, true);
	/* Código recompilado en estos 128 bytes: por el camino lento. */
	emit_store_check(c, slow);
	uint32_t w;
	if (fv) {
		w = from == DST_FPR64 ? ppc_stfdx(fv, R_RDRAM, T_Y) : ppc_stfsx(fv, R_RDRAM, T_Y);
	} else {
		switch (kind) {
		case SALSA64_REC_SB: w = ppc_stbx(v, R_RDRAM, T_Y); break;
		case SALSA64_REC_SH: w = ppc_sthx(v, R_RDRAM, T_Y); break;
		case SALSA64_REC_SW: w = ppc_stwx(v, R_RDRAM, T_Y); break;
		default:             w = ppc_stdx(v, R_RDRAM, T_Y); break;
		}
	}
	fm_access(c, w, true);
	bind(c, resume);
	unsigned save_sec = c->sec;
	c->sec = SEC_COLD;
	emit_addr_cold(c);
	bind(c, slow);
	if (c->wide)
		store_regs(c, c->written_start);
	else if (va != T_ADDR)
		E(c, ppc_mr(T_ADDR, va));
	if (fv)
		E(c, from == DST_FPR64 ? ppc_ld(5, fpr64_off(c, rt), R_CPU) : ppc_lwz(5, fpr32_off(c, rt), R_CPU));
	else if (v != 5)
		E(c, ppc_mr(5, v));
	if (c->wide) {
		li64(c, 7, c->pc);
		li32u(c, 6, slow_arg(c, SLOW_STORE, kind, k) | (uint32_t)c->n << 16);
		emit_b(c, abs_label(c, c->rec->slow_rtn), true);
	} else {
		emit_slow_call(c, SLOW_STORE, kind, k);
	}
	emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(1, PPC_EQ), exit_label(c, k, false, false));
	emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(5, PPC_GT), exit_label(c, k, true, false));
	emit_b(c, resume, false);
	c->sec = save_sec;
}

/* ---- Enteros ----------------------------------------------------------------- */

/* d = (a < b) con signo: signo de (a - b) corregido por desbordamiento. */
static void emit_slt(struct ctx *c, unsigned d, unsigned a, unsigned b)
{
	E(c, ppc_subf(0, b, a));
	E(c, ppc_xor(T_X, a, b));
	E(c, ppc_xor(T_Y, 0, a));
	E(c, ppc_and(T_X, T_X, T_Y));
	E(c, ppc_xor(0, 0, T_X));
	E(c, ppc_srdi(d, 0, 63));
}

/* d = (a < b) sin signo: préstamo de a - b. */
static void emit_sltu(struct ctx *c, unsigned d, unsigned a, unsigned b)
{
	E(c, ppc_subf(0, b, a));
	E(c, ppc_eqv(T_X, a, b));
	E(c, ppc_and(T_X, T_X, 0));
	E(c, ppc_andc(T_Y, b, a));
	E(c, ppc_or(0, T_X, T_Y));
	E(c, ppc_srdi(d, 0, 63));
}

/* LO en r0 y HI en r12 al llegar a `join`. */
static void emit_hilo(struct ctx *c, bool sx)
{
	unsigned lo = dst(c, REG_LO, T_V), hi = dst(c, REG_HI, T_HI);
	if (sx) {
		E(c, ppc_extsw(lo, 0));
		E(c, ppc_extsw(hi, T_Y));
	} else {
		E(c, ppc_mr(lo, 0));
		E(c, ppc_mr(hi, T_Y));
	}
	put(c, REG_LO, lo);
	put(c, REG_HI, hi);
}

static void emit_div(struct ctx *c, unsigned funct, unsigned a, unsigned b)
{
	int zero = new_label(c), join = new_label(c), neg1 = -1;
	bool is64 = funct >= 0x1E, sgn = !(funct & 1);
	unsigned x = T_X, y = T_Y;
	if (is64) {
		x = a;
		E(c, ppc_mr(T_Y, b));
	} else if (sgn) {
		E(c, ppc_extsw(T_X, a));
		E(c, ppc_extsw(T_Y, b));
	} else {
		E(c, ppc_clrldi(T_X, a, 32));
		E(c, ppc_clrldi(T_Y, b, 32));
	}
	E(c, ppc_cmpdi(CR_T, y, 0));
	emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), zero);
	if (is64 && sgn) {  /* INT64_MIN / -1 desborda en el PPE */
		neg1 = new_label(c);
		E(c, ppc_cmpdi(CR_T, y, -1));
		emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), neg1);
	}
	E(c, sgn ? ppc_divd(0, x, y) : ppc_divdu(0, x, y));
	E(c, ppc_mulld(T_Y, 0, y));
	E(c, ppc_subf(T_Y, T_Y, x));
	bind(c, join);
	emit_hilo(c, !is64);

	unsigned save_sec = c->sec;
	c->sec = SEC_COLD;
	bind(c, zero);
	if (sgn) {  /* LO = dividendo < 0 ? 1 : -1 */
		E(c, ppc_sradi(T_Y, x, 63));
		E(c, ppc_add(T_Y, T_Y, T_Y));
		E(c, ppc_addi(T_Y, T_Y, 1));
		E(c, ppc_neg(0, T_Y));
	} else {
		E(c, ppc_li(0, -1));
	}
	E(c, ppc_mr(T_Y, x));  /* HI = dividendo */
	emit_b(c, join, false);
	if (neg1 >= 0) {
		bind(c, neg1);
		E(c, ppc_neg(0, x));
		E(c, ppc_li(T_Y, 0));
		emit_b(c, join, false);
	}
	c->sec = save_sec;
}

static void emit_special(struct ctx *c, uint32_t i)
{
	unsigned rs = I_RS(i), rt = I_RT(i), rd = I_RD(i), sa = I_SA(i), funct = I_FUNCT(i);
	unsigned a, b, d;

	switch (funct) {
	case 0x00:                                                        /* SLL */
		if (i == 0)
			return;
		a = get(c, rt, T_S1);
		d = dst(c, rd, T_V);
		E(c, ppc_rlwinm(d, a, sa, 0, 31 - sa));
		E(c, ppc_extsw(d, d));
		put(c, rd, d);
		return;
	case 0x02:                                                        /* SRL */
		a = get(c, rt, T_S1);
		d = dst(c, rd, T_V);
		E(c, ppc_rlwinm(d, a, (32 - sa) & 31, sa, 31));
		E(c, ppc_extsw(d, d));
		put(c, rd, d);
		return;
	case 0x03:                                                        /* SRA */
		a = get(c, rt, T_S1);
		d = dst(c, rd, T_V);
		E(c, ppc_sradi(d, a, sa));
		E(c, ppc_extsw(d, d));
		put(c, rd, d);
		return;
	case 0x04: case 0x06: case 0x07:                                  /* SLLV SRLV SRAV */
		a = get(c, rt, T_S1);
		b = get(c, rs, T_S2);
		d = dst(c, rd, T_V);
		E(c, ppc_rlwinm(T_X, b, 0, 27, 31));
		E(c, funct == 0x04 ? ppc_slw(d, a, T_X) : funct == 0x06 ? ppc_srw(d, a, T_X) : ppc_srad(d, a, T_X));
		E(c, ppc_extsw(d, d));
		put(c, rd, d);
		return;
	case 0x14: case 0x16: case 0x17:                                  /* DSLLV DSRLV DSRAV */
		a = get(c, rt, T_S1);
		b = get(c, rs, T_S2);
		d = dst(c, rd, T_V);
		E(c, ppc_rlwinm(T_X, b, 0, 26, 31));
		E(c, funct == 0x14 ? ppc_sld(d, a, T_X) : funct == 0x16 ? ppc_srd(d, a, T_X) : ppc_srad(d, a, T_X));
		put(c, rd, d);
		return;
	case 0x0F:                                                        /* SYNC */
		return;
	case 0x10: case 0x12:                                             /* MFHI MFLO */
		a = get(c, funct == 0x10 ? REG_HI : REG_LO, T_S1);
		d = dst(c, rd, T_V);
		if (d != a)
			E(c, ppc_mr(d, a));
		put(c, rd, d);
		return;
	case 0x11: case 0x13: {                                           /* MTHI MTLO */
		unsigned r = funct == 0x11 ? REG_HI : REG_LO;
		a = get(c, rs, T_S1);
		d = dst(c, r, T_V);
		if (d != a)
			E(c, ppc_mr(d, a));
		put(c, r, d);
		return;
	}
	case 0x18:                                                        /* MULT */
		/* rs con sus 64 bits por rt como entero de 35 bits (interp.c). */
		a = get(c, rs, T_S1);
		b = get(c, rt, T_S2);
		E(c, ppc_sldi(T_X, b, 29));
		E(c, ppc_sradi(T_X, T_X, 29));
		E(c, ppc_mulld(0, a, T_X));
		E(c, ppc_sradi(T_Y, 0, 32));
		emit_hilo(c, true);
		return;
	case 0x19:                                                        /* MULTU */
		a = get(c, rs, T_S1);
		b = get(c, rt, T_S2);
		E(c, ppc_clrldi(T_X, a, 32));
		E(c, ppc_clrldi(T_Y, b, 32));
		E(c, ppc_mulld(0, T_X, T_Y));
		E(c, ppc_sradi(T_Y, 0, 32));
		emit_hilo(c, true);
		return;
	case 0x1C: case 0x1D:                                             /* DMULT DMULTU */
		a = get(c, rs, T_S1);
		b = get(c, rt, T_S2);
		E(c, ppc_mulld(0, a, b));
		E(c, funct == 0x1C ? ppc_mulhd(T_Y, a, b) : ppc_mulhdu(T_Y, a, b));
		emit_hilo(c, false);
		return;
	case 0x1A: case 0x1B: case 0x1E: case 0x1F:                       /* DIV DIVU DDIV DDIVU */
		a = get(c, rs, T_S1);
		b = get(c, rt, T_S2);
		emit_div(c, funct, a, b);
		return;
	case 0x20: case 0x22:                                             /* ADD SUB */
		a = get(c, rs, T_S1);
		b = get(c, rt, T_S2);
		E(c, ppc_extsw(T_X, a));
		E(c, ppc_extsw(T_Y, b));
		E(c, funct == 0x20 ? ppc_add(T_Y, T_X, T_Y) : ppc_subf(T_Y, T_Y, T_X));
		E(c, ppc_extsw(T_X, T_Y));
		E(c, ppc_cmpd(CR_T, T_X, T_Y));
		branch_overflow(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(CR_T, PPC_EQ));
		d = dst(c, rd, T_V);
		E(c, ppc_mr(d, T_X));
		put(c, rd, d);
		return;
	case 0x21: case 0x23:                                             /* ADDU SUBU */
		a = get(c, rs, T_S1);
		b = get(c, rt, T_S2);
		d = dst(c, rd, T_V);
		E(c, funct == 0x21 ? ppc_add(T_X, a, b) : ppc_subf(T_X, b, a));
		E(c, ppc_extsw(d, T_X));
		put(c, rd, d);
		return;
	case 0x24: case 0x25: case 0x26: case 0x27:                       /* AND OR XOR NOR */
		a = get(c, rs, T_S1);
		b = get(c, rt, T_S2);
		d = dst(c, rd, T_V);
		E(c, funct == 0x24 ? ppc_and(d, a, b) : funct == 0x25 ? ppc_or(d, a, b)
		   : funct == 0x26 ? ppc_xor(d, a, b) : ppc_nor(d, a, b));
		put(c, rd, d);
		return;
	case 0x2A: case 0x2B:                                             /* SLT SLTU */
		a = get(c, rs, T_S1);
		b = get(c, rt, T_S2);
		d = dst(c, rd, T_V);
		if (funct == 0x2A)
			emit_slt(c, d, a, b);
		else
			emit_sltu(c, d, a, b);
		put(c, rd, d);
		return;
	case 0x2C: case 0x2E:                                             /* DADD DSUB */
		a = get(c, rs, T_S1);
		b = get(c, rt, T_S2);
		if (funct == 0x2C) {
			E(c, ppc_add(0, a, b));
			E(c, ppc_xor(T_X, a, 0));
			E(c, ppc_xor(T_Y, b, 0));
		} else {
			E(c, ppc_subf(0, b, a));
			E(c, ppc_xor(T_X, a, b));
			E(c, ppc_xor(T_Y, a, 0));
		}
		E(c, ppc_and(T_X, T_X, T_Y));
		E(c, ppc_cmpdi(CR_T, T_X, 0));
		branch_overflow(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_LT));
		d = dst(c, rd, T_V);
		E(c, ppc_mr(d, 0));
		put(c, rd, d);
		return;
	case 0x2D: case 0x2F:                                             /* DADDU DSUBU */
		a = get(c, rs, T_S1);
		b = get(c, rt, T_S2);
		d = dst(c, rd, T_V);
		E(c, funct == 0x2D ? ppc_add(d, a, b) : ppc_subf(d, b, a));
		put(c, rd, d);
		return;
	case 0x38: case 0x3A: case 0x3B: case 0x3C: case 0x3E: case 0x3F: {  /* DSLL DSRL DSRA (+32) */
		unsigned n = sa + (funct >= 0x3C ? 32 : 0);
		a = get(c, rt, T_S1);
		d = dst(c, rd, T_V);
		switch (funct & 3) {
		case 0:  E(c, ppc_sldi(d, a, n)); break;
		case 2:  E(c, ppc_srdi(d, a, n)); break;
		default: E(c, ppc_sradi(d, a, n)); break;
		}
		put(c, rd, d);
		return;
	}
	default:
		return;  /* decode() manda lo demás al intérprete */
	}
}

static void emit_imm(struct ctx *c, uint32_t i)
{
	unsigned op = I_OP(i), rs = I_RS(i), rt = I_RT(i), a, d;
	int32_t simm = (int16_t)i;
	uint32_t uimm = i & 0xFFFF;

	switch (op) {
	case 0x08:                                                        /* ADDI */
		a = get(c, rs, T_S1);
		E(c, ppc_extsw(T_X, a));
		E(c, ppc_addi(T_Y, T_X, simm));
		E(c, ppc_extsw(T_X, T_Y));
		E(c, ppc_cmpd(CR_T, T_X, T_Y));
		branch_overflow(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(CR_T, PPC_EQ));
		d = dst(c, rt, T_V);
		E(c, ppc_mr(d, T_X));
		put(c, rt, d);
		return;
	case 0x09:                                                        /* ADDIU */
		d = dst(c, rt, T_V);
		if (rs == 0) {
			E(c, ppc_li(d, simm));
		} else {
			a = get(c, rs, T_S1);
			E(c, ppc_addi(T_X, a, simm));
			E(c, ppc_extsw(d, T_X));
		}
		put(c, rt, d);
		return;
	case 0x0A: case 0x0B:                                             /* SLTI SLTIU */
		a = get(c, rs, T_S1);
		E(c, ppc_li(T_S2, simm));
		d = dst(c, rt, T_V);
		if (op == 0x0A)
			emit_slt(c, d, a, T_S2);
		else
			emit_sltu(c, d, a, T_S2);
		put(c, rt, d);
		return;
	case 0x0C: {                                                      /* ANDI */
		/* andi. es microcódigo en el PPE: una máscara de unos seguidos
		 * es un rlwinm (que además deja en 0 los 32 bits altos); otra,
		 * li + and. */
		a = get(c, rs, T_S1);
		d = dst(c, rt, T_V);
		uint32_t low = uimm & (0u - uimm);
		if (uimm == 0) {
			E(c, ppc_li(d, 0));
		} else if (((uimm + low) & (uimm + low - 1)) == 0) {
			unsigned me = 31 - (unsigned)__builtin_ctz(uimm), mb = (unsigned)__builtin_clz(uimm);
			E(c, ppc_rlwinm(d, a, 0, mb, me));
		} else {
			li32u(c, T_X, uimm);  /* li extendería el signo */
			E(c, ppc_and(d, a, T_X));
		}
		put(c, rt, d);
		return;
	}
	case 0x0D: case 0x0E:                                             /* ORI XORI */
		a = get(c, rs, T_S1);
		d = dst(c, rt, T_V);
		E(c, op == 0x0D ? ppc_ori(d, a, uimm) : ppc_xori(d, a, uimm));
		put(c, rt, d);
		return;
	case 0x0F:                                                        /* LUI */
		d = dst(c, rt, T_V);
		E(c, ppc_lis(d, simm));
		put(c, rt, d);
		return;
	case 0x18:                                                        /* DADDI */
		a = get(c, rs, T_S1);
		E(c, ppc_addi(T_Y, a, simm));
		E(c, ppc_li(T_X, simm));
		E(c, ppc_xor(T_X, T_X, T_Y));
		E(c, ppc_xor(0, a, T_Y));
		E(c, ppc_and(T_X, T_X, 0));
		E(c, ppc_cmpdi(CR_T, T_X, 0));
		branch_overflow(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_LT));
		d = dst(c, rt, T_V);
		E(c, ppc_mr(d, T_Y));
		put(c, rt, d);
		return;
	case 0x19:                                                        /* DADDIU */
		a = get(c, rs, T_S1);
		d = dst(c, rt, T_V);
		E(c, ppc_addi(d, a, simm));
		put(c, rt, d);
		return;
	default:
		return;
	}
}

static void emit_cop1(struct ctx *c, uint32_t i)
{
	unsigned rt = I_RT(i), fs = I_FS(i), d, v;
	switch (I_RS(i)) {
	case 0x00:                                                        /* MFC1 */
		E(c, ppc_lwz(T_X, fpr32_off(c, fs), R_CPU));
		d = dst(c, rt, T_V);
		E(c, ppc_extsw(d, T_X));
		put(c, rt, d);
		return;
	case 0x01:                                                        /* DMFC1 */
		d = dst(c, rt, T_V);
		E(c, ppc_ld(d, fpr64_off(c, fs), R_CPU));
		put(c, rt, d);
		return;
	case 0x04:                                                        /* MTC1 */
		v = get(c, rt, T_S1);
		E(c, ppc_stw(v, fpr32_off(c, fs), R_CPU));
		return;
	case 0x05:                                                        /* DMTC1 */
		v = get(c, rt, T_S1);
		E(c, ppc_std(v, fpr64_off(c, fs), R_CPU));
		return;
	default:
		return;
	}
}

/* ---- FPU ----------------------------------------------------------------------- */

/*
 * Lo mismo que el camino rápido del intérprete (fpu.h: fpu_fast_arith,
 * fpu_fast_abs_neg, fpu_fast_compare, MOV), con las mismas operaciones de
 * doble precisión en el mismo orden, así que da los mismos bits y las mismas
 * causas en FCR31. Lo que ese camino no cubre (redondeo dirigido en la
 * aritmética, I habilitada, operandos fuera de la franja segura, NaN,
 * desnormales, suma con exponentes a más de 28 de distancia en S; MUL.S
 * admite cualquier normal si el producto no desborda ni queda diminuto)
 * salta a la sección fría, donde lo hace el intérprete. Las conversiones a
 * entero sí siguen el RM de FCR31 (rec_emit_ext.h, emit_toint_core).
 *
 * Operandos como en cop1.c: fs con el bit bajo ignorado si FR = 0; ft y fd,
 * el registro tal cual. En S, el valor está en la mitad baja (+4).
 */

/* r6 = FCR31 leído antes; r5 = 1 si el resultado es inexacto (o NULL: no).
 * Borra las causas, pone I si hace falta y escribe solo si cambió. El nuevo
 * FCR31 se arma en T_V (libre en las operaciones de la FPU), no en r7: r7
 * es la base en caché de un grupo de accesos (T_HI) y así los grupos no se
 * cortan en las operaciones de COP1. */
static void emit_fcr31_cause(struct ctx *c, bool inexact_in_r5)
{
	int same = new_label(c);
	E(c, ppc_rlwinm(T_V, 6, 0, 20, 13));  /* causas (bits 12–17) en 0 */
	if (inexact_in_r5) {                 /* causa I (bit 12) y bandera I (bit 2); r5 es 0 o 1 */
		E(c, ppc_rlwinm(0, 5, 12, 19, 19));
		E(c, ppc_or(T_V, T_V, 0));
		E(c, ppc_rlwinm(0, 5, 2, 29, 29));
		E(c, ppc_or(T_V, T_V, 0));
	}
	E(c, ppc_cmpw(CR_T, T_V, 6));
	emit_bc(c, PPC_BO_TRUE, PPC_BI(CR_T, PPC_EQ), same);
	E(c, ppc_stw(T_V, OFF(fcr31), R_CPU));
	bind(c, same);
}

/* Franja segura del exponente (fpu_fast_band): salta a `slow` si no. Deja el
 * exponente en `e`. */
static void emit_fpu_band(struct ctx *c, bool is_s, unsigned bits, unsigned e, int slow)
{
	int ok = new_label(c);
	if (is_s) {
		E(c, ppc_rlwinm(e, bits, 9, 24, 31));
		E(c, ppc_addi(0, e, -67));
		E(c, ppc_cmplwi(CR_T, 0, 120));
		emit_bc(c, PPC_BO_FALSE_LIKELY, PPC_BI(CR_T, PPC_GT), ok);
		E(c, ppc_rlwinm(0, bits, 0, 1, 31));
	} else {
		E(c, ppc_rldicl(e, bits, 12, 53));
		E(c, ppc_addi(0, e, -583));
		E(c, ppc_cmplwi(CR_T, 0, 880));
		emit_bc(c, PPC_BO_FALSE_LIKELY, PPC_BI(CR_T, PPC_GT), ok);
		E(c, ppc_clrldi(0, bits, 1));
	}
	E(c, ppc_cmpdi(CR_T, 0, 0));
	emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), slow);
	bind(c, ok);
}

static void emit_fpu_range(struct ctx *c, bool is_s, unsigned bits, unsigned e, bool with_sign,
			   int32_t lo, uint32_t width, int slow);  /* rec_emit_ext.h */

/* MUL.S con los bits de los operandos en r9 y r10: cada uno normal o ±0
 * (NaN, infinito y desnormal van al intérprete) y, si ninguno es cero, un
 * producto que ni desborda ni queda diminuto. Con exponentes sesgados ea y
 * eb, el del producto es s = ea + eb - 127 (+1 si el producto de las
 * mantisas llega a 2, +1 si el redondeo a 24 bits sube a la potencia
 * siguiente); con 1 <= s <= 252 queda entre 1 y 254. El producto de dos
 * float es exacto en double. Deja los exponentes en r12 y r4. */
static void emit_fpu_mul_s_range(struct ctx *c, int slow)
{
	int ok = new_label(c);
	emit_fpu_range(c, true, 9, 12, false, 1, 253, slow);
	emit_fpu_range(c, true, 10, 4, false, 1, 253, slow);
	E(c, ppc_cmpwi(CR_T, 12, 0));
	emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), ok);  /* ±0 · x = ±0 */
	E(c, ppc_cmpwi(CR_T, 4, 0));
	emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), ok);
	E(c, ppc_add(11, 12, 4));
	E(c, ppc_addi(11, 11, -128));  /* s - 1; no r0: en addi vale 0 */
	E(c, ppc_cmplwi(CR_T, 11, 251));
	emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_GT), slow);
	bind(c, ok);
}

static void emit_fpu(struct ctx *c, unsigned k)
{
	uint32_t i = c->d[k].w;
	bool is_s = I_FMT(i) == FMT_S;
	unsigned funct = I_FUNCT(i), fs = I_FS(i), ft = I_FT(i), fd = I_FD(i);
	unsigned fsr = c->rec->fr ? fs : fs & ~1u;
	int32_t lo = is_s ? 4 : 0;
	int32_t oa = OFF(fpr) + 8 * (int32_t)fsr + lo, ob = OFF(fpr) + 8 * (int32_t)ft + lo;
	int32_t od = OFF(fpr) + 8 * (int32_t)fd;

	if (funct == 0x06) {  /* MOV: los 64 bits, sin tocar FCR31 */
		E(c, ppc_ld(0, OFF(fpr) + 8 * (int32_t)fsr, R_CPU));
		E(c, ppc_std(0, od, R_CPU));
		return;
	}

	int slow = new_label(c), resume = new_label(c);
	if (c->rec->fpu_rtn_ok) {
		/* La FPU compacta: los desplazamientos en r3, r4 y T_V (y el extra
		 * en r10), la rutina del preámbulo y, si no pudo, el intérprete. */
		unsigned kind;
		int extra = -1;
		if (funct == 0x05 || funct == 0x07) {
			kind = is_s ? SALSA64_FPR_ABSNEG_S : SALSA64_FPR_ABSNEG_D;
			extra = funct == 0x07;
		} else if (funct >= 0x30) {
			kind = is_s ? SALSA64_FPR_CMP_S : SALSA64_FPR_CMP_D;
			extra = (int)(funct & 15);
		} else {
			kind = (is_s ? SALSA64_FPR_ADD_S : SALSA64_FPR_ADD_D) + funct;
		}
		E(c, ppc_li(3, oa));
		if (funct >= 0x30 || funct <= 0x03)
			E(c, ppc_li(4, ob));
		E(c, ppc_li(T_V, od));
		if (extra >= 0)
			E(c, ppc_li(10, extra));
		emit_b(c, abs_label(c, c->rec->fpu_rtn[kind]), true);
		emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(1, PPC_EQ), slow);
		bind(c, resume);
		unsigned sec2 = c->sec;
		c->sec = SEC_COLD;
		bind(c, slow);
		emit_exec_call(c, k);
		post_check(c, k);
		emit_b(c, resume, false);
		c->sec = sec2;
		return;
	}
	if (funct == 0x05 || funct == 0x07) {  /* ABS, NEG */
		int ok = new_label(c);
		unsigned emax = is_s ? 0xFF : 0x7FF;
		if (is_s) {
			E(c, ppc_lwz(9, oa, R_CPU));
			E(c, ppc_rlwinm(11, 9, 9, 24, 31));
			E(c, ppc_rlwinm(12, 9, 0, 9, 31));
		} else {
			E(c, ppc_ld(9, oa, R_CPU));
			E(c, ppc_rldicl(11, 9, 12, 53));
			E(c, ppc_clrldi(12, 9, 12));
		}
		/* NaN y desnormales (mantisa ≠ 0 con exponente 0 o máximo): lento. */
		E(c, ppc_cmpdi(CR_T, 12, 0));
		emit_bc(c, PPC_BO_TRUE_LIKELY, PPC_BI(CR_T, PPC_EQ), ok);
		E(c, ppc_cmpwi(CR_T, 11, 0));
		emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), slow);
		E(c, ppc_cmpwi(CR_T, 11, (int32_t)emax));
		emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), slow);
		bind(c, ok);
		if (is_s) {
			E(c, funct == 0x07 ? ppc_xoris(9, 9, 0x8000) : ppc_rlwinm(9, 9, 0, 1, 31));
		} else if (funct == 0x07) {
			E(c, ppc_li(0, 1));
			E(c, ppc_sldi(0, 0, 63));
			E(c, ppc_xor(9, 9, 0));
		} else {
			E(c, ppc_clrldi(9, 9, 1));
		}
		E(c, ppc_lwz(6, OFF(fcr31), R_CPU));
		E(c, ppc_std(9, od, R_CPU));  /* en S, los 32 bits altos en 0 */
		emit_fcr31_cause(c, false);
	} else if (funct >= 0x30) {  /* C.cond sin NaN, en enteros */
		unsigned cond = funct & 15;
		int set = new_label(c), done = new_label(c);
		if (is_s) {
			E(c, ppc_lwz(9, oa, R_CPU));
			E(c, ppc_lwz(10, ob, R_CPU));
			E(c, ppc_rlwinm(11, 9, 0, 1, 31));
			E(c, ppc_rlwinm(12, 10, 0, 1, 31));
			E(c, ppc_lis(0, 0x7F80));
			E(c, ppc_extsw(9, 9));
			E(c, ppc_extsw(10, 10));
		} else {
			E(c, ppc_ld(9, oa, R_CPU));
			E(c, ppc_ld(10, ob, R_CPU));
			E(c, ppc_clrldi(11, 9, 1));
			E(c, ppc_clrldi(12, 10, 1));
			E(c, ppc_li(0, 0x7FF));
			E(c, ppc_sldi(0, 0, 52));
		}
		E(c, ppc_cmpld(CR_T, 11, 0));
		emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_GT), slow);
		E(c, ppc_cmpld(CR_T, 12, 0));
		emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_GT), slow);
		/* Signo y magnitud → entero ordenado: (m ^ s) - s con s = 0 o -1. */
		E(c, ppc_sradi(9, 9, 63));
		E(c, ppc_sradi(10, 10, 63));
		E(c, ppc_xor(11, 11, 9));
		E(c, ppc_subf(11, 9, 11));
		E(c, ppc_xor(12, 12, 10));
		E(c, ppc_subf(12, 10, 12));
		E(c, ppc_cmpd(CR_T, 11, 12));
		E(c, ppc_lwz(6, OFF(fcr31), R_CPU));
		E(c, ppc_rlwinm(T_V, 6, 0, 20, 13));  /* causas en 0 (en T_V, no en r7: ver emit_fcr31_cause) */
		E(c, ppc_rlwinm(T_V, T_V, 0, 9, 7));  /* C (bit 23) en 0 */
		if (cond & 4)
			emit_bc(c, PPC_BO_TRUE, PPC_BI(CR_T, PPC_LT), set);
		if (cond & 2)
			emit_bc(c, PPC_BO_TRUE, PPC_BI(CR_T, PPC_EQ), set);
		emit_b(c, done, false);
		bind(c, set);
		E(c, ppc_oris(T_V, T_V, 0x0080));
		bind(c, done);
		int same = new_label(c);
		E(c, ppc_cmpw(CR_T, T_V, 6));
		emit_bc(c, PPC_BO_TRUE, PPC_BI(CR_T, PPC_EQ), same);
		E(c, ppc_stw(T_V, OFF(fcr31), R_CPU));
		bind(c, same);
	} else {  /* ADD SUB MUL DIV */
		int done = new_label(c);
		E(c, ppc_lwz(6, OFF(fcr31), R_CPU));
		E(c, ppc_li(0, 0x83));  /* RM ≠ 0 o I habilitada (andi. es microcódigo) */
		E(c, ppc_and(0, 6, 0));
		E(c, ppc_cmpwi(0, 0, 0));
		emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(0, PPC_EQ), slow);
		E(c, is_s ? ppc_lwz(9, oa, R_CPU) : ppc_ld(9, oa, R_CPU));
		E(c, is_s ? ppc_lwz(10, ob, R_CPU) : ppc_ld(10, ob, R_CPU));
		if (is_s && funct == 0x02) {
			emit_fpu_mul_s_range(c, slow);
		} else {
			emit_fpu_band(c, is_s, 9, 12, slow);
			emit_fpu_band(c, is_s, 10, 4, slow);
		}
		if (funct == 0x03) {  /* entre cero: Z o V */
			E(c, is_s ? ppc_rlwinm(0, 10, 0, 1, 31) : ppc_clrldi(0, 10, 1));
			E(c, ppc_cmpdi(CR_T, 0, 0));
			emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), slow);
		}
		E(c, is_s ? ppc_lfs(1, oa, R_CPU) : ppc_lfd(1, oa, R_CPU));
		E(c, is_s ? ppc_lfs(2, ob, R_CPU) : ppc_lfd(2, ob, R_CPU));
		if (funct == 0x01)
			E(c, ppc_fneg(2, 2));  /* SUB = ADD con -b */
		switch (funct) {
		case 0x00: case 0x01: E(c, ppc_fadd(0, 1, 2)); break;
		case 0x02:            E(c, ppc_fmul(0, 1, 2)); break;
		default:              E(c, ppc_fdiv(0, 1, 2)); break;
		}
		E(c, ppc_li(5, 1));
		if (is_s) {
			E(c, ppc_frsp(3, 0));
			E(c, ppc_fcmpu(CR_T, 3, 0));
			emit_bc(c, PPC_BO_FALSE, PPC_BI(CR_T, PPC_EQ), done);  /* (double)f != r */
			if (funct <= 0x01) {
				/* Exponentes a más de 28 (y ninguno cero): TwoSum, lento. */
				int exact = new_label(c);
				E(c, ppc_cmpwi(CR_T, 12, 0));
				emit_bc(c, PPC_BO_TRUE, PPC_BI(CR_T, PPC_EQ), exact);
				E(c, ppc_cmpwi(CR_T, 4, 0));
				emit_bc(c, PPC_BO_TRUE, PPC_BI(CR_T, PPC_EQ), exact);
				E(c, ppc_subf(11, 4, 12));
				E(c, ppc_addi(11, 11, 28));  /* no r0: en addi vale 0 */
				E(c, ppc_cmplwi(CR_T, 11, 56));
				emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_GT), slow);
				bind(c, exact);
				E(c, ppc_li(5, 0));
			} else if (funct == 0x02) {
				E(c, ppc_li(5, 0));  /* el producto de dos float es exacto en double */
			} else {
				E(c, ppc_fnmsub(4, 0, 2, 1));  /* a - r·b */
				E(c, ppc_fsub(5, 1, 1));
				E(c, ppc_fcmpu(CR_T, 4, 5));
				emit_bc(c, PPC_BO_FALSE, PPC_BI(CR_T, PPC_EQ), done);
				E(c, ppc_li(5, 0));
			}
		} else {
			if (funct <= 0x01) {  /* TwoSum */
				E(c, ppc_fsub(4, 0, 1));       /* bb = r - a */
				E(c, ppc_fsub(5, 0, 4));       /* r - bb */
				E(c, ppc_fsub(5, 1, 5));       /* a - (r - bb) */
				E(c, ppc_fsub(3, 2, 4));       /* b - bb */
				E(c, ppc_fadd(4, 5, 3));
			} else if (funct == 0x02) {
				E(c, ppc_fmsub(4, 1, 2, 0));   /* a·b - r */
			} else {
				E(c, ppc_fnmsub(4, 0, 2, 1));  /* a - r·b */
			}
			E(c, ppc_fsub(5, 1, 1));
			E(c, ppc_fcmpu(CR_T, 4, 5));
			emit_bc(c, PPC_BO_FALSE, PPC_BI(CR_T, PPC_EQ), done);
			E(c, ppc_li(5, 0));
		}
		bind(c, done);
		if (is_s) {
			E(c, ppc_stfs(3, od + 4, R_CPU));
			E(c, ppc_li(0, 0));
			E(c, ppc_stw(0, od, R_CPU));
		} else {
			E(c, ppc_stfd(0, od, R_CPU));
		}
		emit_fcr31_cause(c, true);
	}
	bind(c, resume);

	/* Camino lento: el intérprete (no toca registros generales). */
	unsigned sec = c->sec;
	c->sec = SEC_COLD;
	bind(c, slow);
	emit_exec_call(c, k);
	post_check(c, k);
	emit_b(c, resume, false);
	c->sec = sec;
}

/* Conversiones y raíz de la FPU, CFC1/CTC1, LWL/LWR y compañía, MFC0. */
#include "cpu/rec_emit_ext.h"
/* La FPU compacta: las operaciones como rutinas compartidas del preámbulo. */
static uint32_t finalize(struct ctx *c, uint32_t base, uint32_t limit);
#include "cpu/rec_emit_fpurtn.h"
/* La FPU con registros residentes (reemplaza a emit_fpu, emit_cop1 y a lo de
 * COP1 de rec_emit_ext.h solo con SALSA64_REC_OPT_FPU_RES, experimental). */
#include "cpu/rec_emit_fpu.h"

static void emit_insn_body(struct ctx *c, unsigned k);

/* Una instrucción, con sus palabras a su clase (perfilado). */
static void emit_insn(struct ctx *c, unsigned k)
{
	const struct dec *d = &c->d[k];
	unsigned cls = d->how != H_NATIVE || d->kind == K_END ? SALSA64_RT_C_OTHER : salsa64_rec_trace_class(d->w);
	unsigned old = cls_set(c, cls);
	c->cls_n[cls]++;
	emit_insn_body(c, k);
	cls_set(c, old);
}

static void emit_insn_body(struct ctx *c, unsigned k)
{
	const struct dec *d = &c->d[k];
	uint32_t i = d->w;

	flush_eager(c, k);
	c->k = k;
	c->written_start = c->written;
	if (d->how == H_EXEC) {
		emit_exec(c, k);
		return;
	}
	if (d->how == H_EXC) {
		emit_exception(c, k, I_FUNCT(i) == 0x0C ? SALSA64_EXC_SYS : SALSA64_EXC_BP);
		return;
	}
	if (d->how == H_MTC0) {
		emit_mtc0(c, k);
		return;
	}
	if (d->how == H_MTC0_ST) {
		emit_mtc0_status(c, k);
		return;
	}
	if (d->how == H_TLBW) {
		emit_tlbw(c, k);
		return;
	}
	if (d->how == H_ERET) {
		emit_eret(c, k);
		return;
	}
	if (I_OP(i) == 0x11 && c->fpu_res) {
		emit_cop1_dispatch_res(c, k);
		return;
	}
	if (emit_ext(c, k))
		return;
	switch (I_OP(i)) {
	case 0x00: emit_special(c, i); return;
	case 0x11:
		if ((I_FMT(i) & ~1u) == FMT_S)
			emit_fpu(c, k);
		else
			emit_cop1(c, i);
		return;
	case 0x20: emit_load(c, k, SALSA64_REC_LB, DST_GPR); return;
	case 0x21: emit_load(c, k, SALSA64_REC_LH, DST_GPR); return;
	case 0x23: emit_load(c, k, SALSA64_REC_LW, DST_GPR); return;
	case 0x24: emit_load(c, k, SALSA64_REC_LBU, DST_GPR); return;
	case 0x25: emit_load(c, k, SALSA64_REC_LHU, DST_GPR); return;
	case 0x27: emit_load(c, k, SALSA64_REC_LWU, DST_GPR); return;
	case 0x37: emit_load(c, k, SALSA64_REC_LD, DST_GPR); return;
	case 0x31: emit_load(c, k, SALSA64_REC_LWU, DST_FPR32); return;
	case 0x35: emit_load(c, k, SALSA64_REC_LD, DST_FPR64); return;
	case 0x28: emit_store(c, k, SALSA64_REC_SB, DST_GPR); return;
	case 0x29: emit_store(c, k, SALSA64_REC_SH, DST_GPR); return;
	case 0x2B: emit_store(c, k, SALSA64_REC_SW, DST_GPR); return;
	case 0x3F: emit_store(c, k, SALSA64_REC_SD, DST_GPR); return;
	case 0x39: emit_store(c, k, SALSA64_REC_SW, DST_FPR32); return;
	case 0x3D: emit_store(c, k, SALSA64_REC_SD, DST_FPR64); return;
	default:   emit_imm(c, i); return;
	}
}

/* ---- Saltos ------------------------------------------------------------------- */

/* ¿Consultaría el intérprete idle_hint en este salto? Saltos condicionales
 * (también los de enlace y BC1) y J, tomados hacia atrás a menos de
 * SALSA64_IDLE_BODY instrucciones, que la tabla de descartados no tenga. */
static bool idle_candidate(const struct ctx *c, const struct dec *d, uint64_t bpc)
{
	uint32_t i = d->w;
	if (d->kind == K_JUMP_REG || (d->kind == K_JUMP && I_OP(i) != 0x02))
		return false;
	uint64_t target = I_OP(i) == 0x02 ? ((bpc + 4) & ~0x0FFFFFFFull) | (uint64_t)I_TARGET(i) << 2
					  : bpc + 4 + ((uint64_t)(int64_t)(int16_t)i << 2);
	if (bpc - target >= 4 * SALSA64_IDLE_BODY)
		return false;
	const struct salsa64_cpu *cpu = c->rec->cpu;
	return cpu->idle_reject[SALSA64_IDLE_SLOT(bpc)] != ((uint32_t)bpc ^ i);
}

/* La condición del salto b en cr4 (y el registro de enlace, si lo escribe);
 * deja su descripción en c->br[b]. */
static void emit_branch_cond(struct ctx *c, unsigned b)
{
	const struct dec *d = &c->d[b];
	uint32_t i = d->w;
	unsigned op = I_OP(i), rs = I_RS(i), rt = I_RT(i), a, x;
	uint64_t bpc = insn_pc(c, b);
	struct brinfo *br = &c->br[b];

	flush_eager(c, b);
	c->k = b;
	c->written_start = c->written;
	br->kind = d->kind;
	br->fall = bpc + 8;
	br->target = bpc + 4 + ((uint64_t)(int64_t)(int16_t)i << 2);

	switch (op) {
	case 0x02: case 0x03:                                             /* J JAL */
		br->target = ((bpc + 4) & ~0x0FFFFFFFull) | (uint64_t)I_TARGET(i) << 2;
		if (op == 0x03) {
			x = dst(c, 31, T_V);
			li64(c, x, br->fall);
			put(c, 31, x);
		}
		break;
	case 0x00:                                                        /* JR JALR */
		a = get(c, rs, R_TARGET);
		if (a != R_TARGET)
			E(c, ppc_mr(R_TARGET, a));
		if (I_FUNCT(i) == 0x09 && I_RD(i)) {
			x = dst(c, I_RD(i), T_V);
			li64(c, x, br->fall);
			put(c, I_RD(i), x);
		}
		break;
	case 0x04: case 0x05: case 0x14: case 0x15:                       /* BEQ BNE (L) */
		a = get(c, rs, T_S1);
		if (rt == 0) {
			E(c, ppc_cmpdi(CR_BR, a, 0));
		} else {
			x = get(c, rt, T_S2);
			E(c, ppc_cmpd(CR_BR, a, x));
		}
		br->bi = PPC_BI(CR_BR, PPC_EQ);
		br->bo = (op & 1) ? PPC_BO_FALSE : PPC_BO_TRUE;
		break;
	case 0x06: case 0x07: case 0x16: case 0x17:                       /* BLEZ BGTZ (L) */
		a = get(c, rs, T_S1);
		E(c, ppc_cmpdi(CR_BR, a, 0));
		br->bi = PPC_BI(CR_BR, PPC_GT);
		br->bo = (op & 1) ? PPC_BO_TRUE : PPC_BO_FALSE;
		break;
	case 0x01:                                                        /* REGIMM */
		a = get(c, rs, T_S1);
		E(c, ppc_cmpdi(CR_BR, a, 0));
		br->bi = PPC_BI(CR_BR, PPC_LT);
		br->bo = (rt & 1) ? PPC_BO_FALSE : PPC_BO_TRUE;  /* BGEZ: no menor que 0 */
		if (rt & 0x10) {  /* enlace después de leer rs */
			x = dst(c, 31, T_V);
			li64(c, x, br->fall);
			put(c, 31, x);
		}
		break;
	case 0x11:                                                        /* BC1F BC1T (L) */
		if (c->fpu_res) {
			E(c, ppc_crnot(PPC_BI(CR_BR, PPC_EQ), FCR_C));  /* eq = C en 0 (rec_emit_fpu.h) */
		} else {
			E(c, ppc_lwz(0, OFF(fcr31), R_CPU));
			E(c, ppc_rlwinm(0, 0, 9, 31, 31));  /* bit 23, C */
			E(c, ppc_cmpwi(CR_BR, 0, 0));
		}
		br->bi = PPC_BI(CR_BR, PPC_EQ);
		br->bo = (rt & 1) ? PPC_BO_FALSE : PPC_BO_TRUE;
		break;
	}
}

/* Salto condicional interno (hacia adelante) en b, con su delay slot en
 * b + 1: el bloque sigue por el camino no tomado y el tomado sale enlazado
 * al destino, con los registros escritos y los ciclos hasta el delay slot.
 * Así un `if` o la salida de un bucle no cortan el bloque (GoldenEye hacía
 * 5,3 instrucciones por bloque). En un branch-likely no tomado el delay
 * slot no corre: desde ahí el bloque cuesta un ciclo menos, y se descuenta
 * de r29 para que `cycles = r29 + k + 1` siga valiendo; su destino se carga
 * a la entrada (livein) para que lo que se escriba después tenga el valor
 * de antes en ese camino. */
/* ¿El destino del salto interno en b es una instrucción del bloque a la que
 * se puede saltar sin salir (docs/rec-ppe.md, «Saltos internos con destino
 * dentro del bloque»)? No a un delay slot (su código vive dentro del de su
 * salto), ni a la segunda de un par LWL/LWR (se emiten juntas), ni con la
 * escritura temprana (sus almacenamientos diferidos se saltarían), ni en un
 * bloque recortado. Devuelve la instrucción, o -1. */
static int ijump_target(const struct ctx *c, unsigned b, uint64_t target)
{
	uint64_t off = target - c->pc;
	if (c->temp || (c->rec->opts & SALSA64_REC_OPT_EAGER) || SALSA64_REC_OLD(c->rec, SALSA64_REC_DBG_NO_IJUMP))
		return -1;
	if ((off & 3) || off / 4 >= c->n)
		return -1;
	unsigned t = (unsigned)(off / 4);
	if (t <= b + 1 || c->ds_of[t])
		return -1;
	if (t >= 1 && !c->ds_of[t - 1] && !is_branch(&c->d[t - 1]) && unaligned_pair(c, t - 1, t + 1))
		return -1;
	return (int)t;
}

/* Destino del salto condicional en b, sin su brinfo (para el análisis de
 * la entrada, antes de emitir). */
static uint64_t branch_target_of(const struct ctx *c, unsigned b)
{
	return insn_pc(c, b) + 4 + ((uint64_t)(int64_t)(int16_t)c->d[b].w << 2);
}

/* Registros que hay que cargar a la entrada por los saltos internos con
 * destino dentro del bloque: lo que escriben las instrucciones que un salto
 * se salta tiene que valer lo de la entrada en su camino tomado (los
 * registros del bloque se escriben a cpu->gpr al salir, y lo que se lee
 * después del destino tiene que ser lo de antes), como el delay slot de un
 * branch-likely interno. Lo escrito antes del salto (o por él y su delay
 * slot) ya vale lo mismo en los dos caminos. */
static uint64_t ijump_livein(const struct ctx *c)
{
	uint64_t livein = 0, before = 0;
	for (unsigned b = 0; b + 1 < c->n; b++) {
		const struct dec *d = &c->d[b];
		bool internal = !c->ds_of[b] && (d->kind == K_BRANCH || d->kind == K_BRANCH_L) &&
				!(c->has_branch && b == c->n - 2);
		if (internal) {
			int t = ijump_target(c, b, branch_target_of(c, b));
			if (t >= 0) {
				uint64_t skipped = 0;
				for (unsigned j = b + 2; j < (unsigned)t; j++)
					skipped |= c->d[j].writes;
				livein |= skipped & ~(before | d->writes | c->d[b + 1].writes);
			}
		}
		before |= d->writes;
	}
	return livein;
}

/* Anota el salto interno de b a t con el estado de ahora (después de su
 * delay slot); su camino tomado se emite al llegar a t (emit_ijumps_to). */
static int ijump_add(struct ctx *c, unsigned b, unsigned t)
{
	struct ijump *ij = &c->ijumps[c->nijumps++];
	ij->b = (uint8_t)b;
	ij->t = (uint8_t)t;
	ij->label = new_label(c);
	fpu_snapshot(c, &ij->snap);
	if (c->insn_label[t] < 0)
		c->insn_label[t] = new_label(c);
	return ij->label;
}

/* Al llegar a la instrucción k: los caminos tomados de los saltos internos
 * que van a ella (sección fría): el estado de la FPU como lo asume el código
 * de k, los ciclos (de b + 2 a k: el código de k asume k) y el salto. */
static void emit_ijumps_to(struct ctx *c, unsigned k)
{
	if (c->insn_label[k] >= 0)
		bind(c, c->insn_label[k]);
	for (unsigned j = 0; j < c->nijumps; j++) {
		const struct ijump *ij = &c->ijumps[j];
		if (ij->t != k)
			continue;
		unsigned sec = c->sec;
		c->sec = SEC_COLD;
		bind(c, ij->label);
		emit_site(c, ij->b, SALSA64_RPF_SITE_COND, insn_pc(c, k));
		fpu_reconcile(c, &ij->snap);
		E(c, ppc_addi(R_CYCLES, R_CYCLES, (int32_t)ij->b + 2 - (int32_t)k));
		emit_b(c, c->insn_label[k], false);
		c->sec = sec;
	}
}

static void emit_internal_branch(struct ctx *c, unsigned b)
{
	bool likely = c->d[b].kind == K_BRANCH_L;
	emit_branch_cond(c, b);
	const struct brinfo *br = &c->br[b];
	int t = ijump_target(c, b, br->target);
	if (likely) {
		struct fpu_snap snap;
		int nt = new_label(c);
		emit_bc(c, br->bo == PPC_BO_TRUE ? PPC_BO_FALSE : PPC_BO_TRUE, br->bi, nt);
		fpu_snapshot(c, &snap);  /* el camino no tomado no corre el delay slot */
		c->in_ds = true;
		emit_insn(c, b + 1);
		c->in_ds = false;
		if (t >= 0) {
			emit_b(c, ijump_add(c, b, (unsigned)t), false);
		} else {
			writeback(c, c->written);
			E(c, ppc_addi(R_CYCLES, R_CYCLES, (int32_t)b + 2));
			emit_site(c, b, SALSA64_RPF_SITE_COND, br->target);
			emit_exit_to(c, br->target);
		}
		bind(c, nt);
		fpu_restore(c, &snap);
		E(c, ppc_addi(R_CYCLES, R_CYCLES, -1));
		return;
	}
	c->in_ds = true;
	emit_insn(c, b + 1);
	c->in_ds = false;
	if (t >= 0) {
		emit_bc(c, br->bo, br->bi, ijump_add(c, b, (unsigned)t));
		return;
	}
	int taken = new_label(c);
	emit_bc(c, br->bo, br->bi, taken);
	unsigned sec = c->sec;
	c->sec = SEC_COLD;
	bind(c, taken);
	writeback(c, c->written);
	E(c, ppc_addi(R_CYCLES, R_CYCLES, (int32_t)b + 2));
	emit_site(c, b, SALSA64_RPF_SITE_COND, br->target);
	emit_exit_to(c, br->target);
	c->sec = sec;
}

/* El salto en b y su delay slot en b + 1 (la última del bloque). */
static void emit_branch(struct ctx *c, unsigned b)
{
	const struct dec *d = &c->d[b];
	uint32_t i = d->w;
	uint64_t bpc = insn_pc(c, b);
	bool likely = d->kind == K_BRANCH_L;

	emit_branch_cond(c, b);
	const struct brinfo *br = &c->br[b];

	/* Bucle de espera posible (idle.c): el camino tomado vuelve a C antes del
	 * delay slot, en el mismo estado en que el intérprete consulta idle_hint;
	 * rec.c llama a salsa64_vr4300_idle y, si la tabla lo descarta, quita
	 * el salto a la consulta. El camino nativo queda listo desde el inicio. */
	bool idle = idle_candidate(c, d, bpc);
	bool patch_idle = !(c->rec->opts & SALSA64_REC_OPT_IDLE_RECOMPILE);
	int idle_exit = -1;
	if (idle) {
		idle_exit = new_label(c);
		unsigned sec = c->sec;
		c->sec = SEC_COLD;
		bind(c, idle_exit);
		fpu_flush(c);
		writeback(c, c->written);
		E(c, ppc_addi(R_CYCLES, R_CYCLES, (int32_t)b + 1));
		if (patch_idle)
			li64(c, 0, ((uint64_t)c->rec->cur_id << 32) | i);
		else
			li32u(c, 0, i);  /* el despachador no relee la instrucción */
		E(c, ppc_std(0, OFF(rec_site), R_CPU));
		li64(c, 3, bpc);
		restore_borrowed(c);
		emit_b(c, abs_label(c, c->rec->exit_idle), false);
		c->sec = sec;
		if (d->kind == K_BRANCH) {
			if (patch_idle) {
				c->idle_patch = new_label(c);
				bind(c, c->idle_patch);
			}
			emit_bc(c, br->bo, br->bi, idle_exit);
		}
	}

	if (likely) {
		/* No tomado: se salta el delay slot y cuenta solo el salto. */
		int nt = new_label(c);
		emit_bc(c, br->bo == PPC_BO_TRUE ? PPC_BO_FALSE : PPC_BO_TRUE, br->bi, nt);
		unsigned sec = c->sec;
		c->sec = SEC_COLD;
		bind(c, nt);
		writeback(c, c->written);
		E(c, ppc_addi(R_CYCLES, R_CYCLES, (int32_t)b + 1));
		emit_site(c, b, SALSA64_RPF_SITE_FALL, br->fall);
		emit_exit_to(c, br->fall);
		c->sec = sec;
	}
	if (idle && d->kind != K_BRANCH) {
		if (patch_idle) {
			c->idle_patch = new_label(c);
			bind(c, c->idle_patch);
		}
		emit_b(c, idle_exit, false);
		if (!patch_idle)
			return;
	}

	c->in_ds = true;
	emit_insn(c, b + 1);
	c->in_ds = false;

	writeback(c, c->written);
	E(c, ppc_addi(R_CYCLES, R_CYCLES, (int32_t)c->n));
	switch (d->kind) {
	case K_JUMP_REG:
		fpu_flush(c);
		E(c, ppc_mr(3, R_TARGET));
		restore_borrowed(c);
		if (c->temp || (c->rec->no_link && !(c->rec->opts & SALSA64_REC_OPT_JTC))) {
			emit_b(c, abs_label(c, c->rec->exit_jump), false);
		} else if (c->rec->trace && c->rec->trace->jr && c->rec->cur_id) {
			E(c, ppc_li(4, (int16_t)c->rec->cur_id));  /* perfilado: el JR cuenta por su bloque */
			emit_b(c, abs_label(c, c->rec->exit_jr_count), false);
		} else {
			emit_b(c, abs_label(c, c->rec->exit_jr), false);
		}
		return;
	case K_BRANCH: {
		if (idle && !patch_idle) {  /* el tomado ya salió antes del delay slot */
			emit_site(c, b, SALSA64_RPF_SITE_FALL, br->fall);
			emit_exit_to(c, br->fall);
			return;
		}
		int taken = new_label(c);
		emit_bc(c, br->bo, br->bi, taken);
		emit_site(c, b, SALSA64_RPF_SITE_FALL, br->fall);
		emit_exit_to(c, br->fall);
		bind(c, taken);
		if (br->target != c->pc)  /* la vuelta al propio bloque no se cuenta (camino caliente) */
			emit_site(c, b, SALSA64_RPF_SITE_COND, br->target);
		emit_taken(c, br->target);
		return;
	}
	default:  /* J/JAL o branch-likely tomado */
		if (br->target != c->pc)
			emit_site(c, b, d->kind == K_JUMP ? SALSA64_RPF_SITE_JUMP : SALSA64_RPF_SITE_COND, br->target);
		emit_taken(c, br->target);
		return;
	}
}

/* ---- Bloque ------------------------------------------------------------------- */

static uint32_t fetch(const struct salsa64_rec *rec, uint32_t pa)
{
	return salsa64_rec_fetch(rec, pa);  /* RDRAM o ROM */
}

/* Elige los registros MIPS más usados del bloque para r14–r25 (o r14–r17
 * con registros fijos, que ya están en r18–r25).
 *
 * Con registros fijos, un bucle (bloque que vuelve sobre sí mismo) que
 * necesita más de 4 registros propios toma prestados los fijos que no usa:
 * los guarda en cpu->gpr al entrar y los repone en cada salida
 * (restore_borrowed). En la PS3, un bucle de enteros con 9 temporales pasó
 * de 128 a ~750 MIPS así; fuera de los bucles no compensa. */
static void allocate(struct ctx *c, bool loop)
{
	unsigned count[34] = { 0 }, nslots = 0;
	uint8_t slots[N_CACHE + 1];
	uint64_t used = 0;
	bool has_jr = false;  /* sin JR/JALR, r26 (su destino) queda para el bloque */
	for (unsigned k = 0; k < c->n; k++) {
		uint64_t m = c->d[k].reads | c->d[k].writes;
		used |= m;
		has_jr |= c->d[k].kind == K_JUMP_REG;
		for (unsigned r = 1; r < 34; r++)
			if (m & BIT(r))
				count[r]++;
	}
	memset(c->map, 0, sizeof c->map);
	c->cached = c->pinned = c->borrowed = 0;
	if (c->rec->opts & SALSA64_REC_OPT_PIN) {
		uint64_t pins = 0;
		for (unsigned k = 0; k < SALSA64_REC_PIN_N; k++)
			pins |= BIT(salsa64_rec_pin[k]);
		unsigned demand = (unsigned)__builtin_popcountll(used & ~pins & ~1ull);
		for (unsigned r = R_CACHE0; r < SALSA64_REC_PIN_PPC; r++)
			slots[nslots++] = (uint8_t)r;
		if (!has_jr)
			slots[nslots++] = R_TARGET;
		for (unsigned k = 0; k < SALSA64_REC_PIN_N; k++) {
			unsigned m = salsa64_rec_pin[k], p = SALSA64_REC_PIN_PPC + k;
			if (loop && !(used & BIT(m)) && nslots < demand && nslots < sizeof slots) {
				c->borrowed |= BIT(m);
				slots[nslots++] = (uint8_t)p;
			} else {
				c->map[m] = (uint8_t)p;
				c->pinned |= BIT(m);
				count[m] = 0;
			}
		}
		c->cached = c->pinned;
	} else {
		for (unsigned r = R_CACHE0; r < R_CACHE0 + N_CACHE; r++)
			slots[nslots++] = (uint8_t)r;
		if (!has_jr)
			slots[nslots++] = R_TARGET;
	}
	for (unsigned slot = 0; slot < nslots; slot++) {
		unsigned best = 0;
		for (unsigned r = 1; r < 34; r++)
			if (count[r] > count[best] && !(c->cached & BIT(r)))
				best = r;
		if (!best)
			break;
		c->map[best] = slots[slot];
		c->cached |= BIT(best);
		count[best] = 0;
	}
}

/* Registro del PPE de un fijo prestado (ver allocate). */
static unsigned pin_ppc(unsigned m)
{
	for (unsigned k = 0; k < SALSA64_REC_PIN_N; k++)
		if (salsa64_rec_pin[k] == m)
			return SALSA64_REC_PIN_PPC + k;
	return 0;
}

/* Antes de cualquier salida del bloque: los fijos prestados vuelven a su
 * registro (el que sigue los espera ahí). */
static void restore_borrowed(struct ctx *c)
{
	for (unsigned r = 1; c->borrowed && r < 32; r++)
		if (c->borrowed & BIT(r))
			E(c, ppc_ld(pin_ppc(r), off_reg(r), R_CPU));
}

/* Ubica las secciones en la caché a partir de la palabra `base`, resuelve los
 * saltos y copia. Devuelve las palabras usadas, o 0 si algo no cupo. */
static uint32_t finalize(struct ctx *c, uint32_t base, uint32_t limit)
{
	struct salsa64_rec *rec = c->rec;
	uint32_t nhot = rec->nbuf[SEC_HOT], ncold = rec->nbuf[SEC_COLD];
	rec->fm_nnew = 0;
	if (rec->overflow || base + nhot + ncold > limit)
		return 0;
	uint32_t start[2] = { base, base + nhot };
	for (uint32_t f = 0; f < rec->nfixups; f++) {
		const struct salsa64_rec_fixup *fx = &rec->fixups[f];
		const struct salsa64_rec_label *l = &rec->labels[fx->label];
		int64_t to;
		if (l->sec == SEC_ABS)
			to = (int32_t)l->pos;
		else if (l->sec == SEC_HOT || l->sec == SEC_COLD)
			to = (int64_t)start[l->sec] + l->pos;
		else
			return 0;  /* etiqueta sin fijar: error del compilador */
		int64_t from = (int64_t)start[fx->sec] + fx->at;
		int64_t disp = (to - from) * 4;
		uint32_t *w = &rec->buf[fx->sec][fx->at];
		if (fx->cond) {
			if (!ppc_fits_bc(disp))
				return 0;
			*w = (*w & ~0xFFFCu) | ((uint32_t)disp & 0xFFFC);
		} else {
			if (!ppc_fits_b(disp))
				return 0;
			*w = (*w & ~0x03FFFFFCu) | ((uint32_t)disp & 0x03FFFFFC);
		}
	}
	/* fastmem: los sitios y sus caminos fríos, en palabras de la caché
	 * (rec.c los anota con la generación del tramo al instalar el bloque). */
	for (unsigned k = 0; k < rec->fm_npend; k++) {
		const struct salsa64_rec_label *ls = &rec->labels[rec->fm_pend_site[k]], *lc = &rec->labels[rec->fm_pend_cold[k]];
		if (ls->sec > SEC_COLD || lc->sec > SEC_COLD)
			return 0;
		rec->fm_new_site[k] = start[ls->sec] + ls->pos;
		rec->fm_new_cold[k] = start[lc->sec] + lc->pos;
	}
	rec->fm_nnew = rec->fm_npend;
	if (c->idle_patch >= 0) {
		const struct salsa64_rec_label *p = &rec->labels[c->idle_patch];
		rec->compiled_idle_patch = start[p->sec] + p->pos;
	}
	memcpy(rec->code + start[0], rec->buf[SEC_HOT], nhot * 4);
	memcpy(rec->code + start[1], rec->buf[SEC_COLD], ncold * 4);
	salsa64_rec_sync(rec->code + base, (nhot + ncold) * 4);
	return nhot + ncold;
}

/* Deja en el contexto solo lo que dejó la decodificación (para emitir el
 * mismo bloque otra vez, en otra zona de la caché). */
static void reset_emission(struct ctx *c)
{
	struct salsa64_rec *rec = c->rec;
	uint64_t pc = c->pc;
	uint32_t pa = c->pa;
	unsigned n = c->n;
	bool temp = c->temp, has_branch = c->has_branch, fr_check = c->fr_check;
	uint8_t seg_kind = c->seg_kind;
	unsigned pent = c->pent;
	struct dec d[SALSA64_REC_MAX_INSNS];
	uint8_t ds_of[SALSA64_REC_MAX_INSNS];
	memcpy(d, c->d, sizeof d);
	memcpy(ds_of, c->ds_of, sizeof ds_of);
	memset(c, 0, sizeof *c);
	c->idle_patch = -1;
	rec->compiled_idle_patch = 0;
	c->rec = rec;
	c->pc = pc;
	c->pa = pa;
	c->n = n;
	c->temp = temp;
	c->wide = (rec->opts & SALSA64_REC_OPT_WIDE) != 0;
	c->fastmem = rec->fastmem && !c->wide;
	c->tramp = -1;
	c->has_branch = has_branch;
	c->fr_check = fr_check;
	c->seg_kind = seg_kind;
	c->pent = pent;
	memcpy(c->d, d, sizeof d);
	memcpy(c->ds_of, ds_of, sizeof ds_of);
}

#ifdef SALSA64_REC_LAB
/* Laboratorio (tools/lab): anota dónde empieza el código de la instrucción k. */
#define LAB_MARK(c, k) ((c)->rec->lab.hot[k] = (uint16_t)(c)->rec->nbuf[SEC_HOT], (c)->rec->lab.cold[k] = (uint16_t)(c)->rec->nbuf[SEC_COLD], \
			(c)->rec->lab.how[k] = (k) < (c)->n ? (c)->d[k].how : 0xFF)
#else
#define LAB_MARK(c, k) ((void)0)
#endif

/* Emite el bloque decodificado en `c` a partir de la palabra `base` de la
 * caché, sin pasar de `limit`. Devuelve las palabras usadas, o 0 si no
 * cupo. */
static uint32_t emit_block(struct ctx *c, uint32_t base, uint32_t limit)
{
	struct salsa64_rec *rec = c->rec;
	uint64_t pc = c->pc;
	uint32_t pa = c->pa;
	unsigned n = c->n, seg = c->seg_kind;
	bool temp = c->temp, any_cop1 = false;

	c->calls_near = salsa64_rec_calls_near_at(rec, base);
	group_pass(c);
	for (unsigned k = 0; k < n; k++) {
		any_cop1 |= c->d[k].cop1;
		c->exit_before[k] = c->exit_after[k] = c->exit_after_mem[k] = -1;
		c->insn_label[k] = -1;
	}
	/* ¿Bucle? El salto final vuelve al inicio (emit_taken). */
	bool loop = false;
	if (c->has_branch && !temp) {
		uint32_t i = c->d[n - 2].w;
		uint64_t bpc = pc + 4ull * (n - 2), target = 0;
		if (I_OP(i) == 0x02 || I_OP(i) == 0x03)
			target = ((bpc + 4) & ~0x0FFFFFFFull) | (uint64_t)I_TARGET(i) << 2;
		else if (c->d[n - 2].kind != K_JUMP_REG)
			target = bpc + 4 + ((uint64_t)(int64_t)(int16_t)i << 2);
		loop = target == pc;
	}
	c->loop = loop;
	allocate(c, loop);
	fpu_allocate(c);

	uint64_t seen = 0, livein = 0;
	for (unsigned k = 0; k < n; k++) {
		livein |= c->d[k].reads & ~seen;
		if (c->ds_of[k] && c->d[k - 1].kind == K_BRANCH_L && !(c->has_branch && k == n - 1))
			livein |= c->d[k].writes & ~seen;  /* likely interno no tomado: el delay slot no corre */
		seen |= c->d[k].writes;
		for (unsigned r = 1; r < 34; r++)
			if (c->d[k].writes & BIT(r))
				c->last_write[r] = (uint8_t)(k + 1);
	}
	livein |= ijump_livein(c);  /* lo que se saltan los saltos internos con destino dentro del bloque */

	rec->nbuf[0] = rec->nbuf[1] = 0;
	rec->nlabels = rec->nfixups = 0;
	rec->fm_npend = 0;
	rec->overflow = false;
	c->cls = SALSA64_RT_C_ENTRY;
	c->cmark[0] = c->cmark[1] = 0;
	memset(c->cls_n, 0, sizeof c->cls_n);
	memset(c->cls_w, 0, sizeof c->cls_w);
#ifdef SALSA64_REC_LAB
	memset(&rec->lab, 0xFF, sizeof rec->lab);
	rec->lab.loop = loop;
#endif
	if (!c->wide)
		chain_build(c);
	c->sec = SEC_HOT;

	/* Entrada: ¿cabe antes del próximo evento?, ¿COP1 usable? */
	int start_exit = new_label(c), vt_miss = -1;
	if (seg == SALSA64_REC_MAPPED) {
		/* ¿ktlb[] (justo antes de flags, en r28) sigue traduciendo la
		 * página igual que ahora (el despachador la acaba de anotar, con su
		 * bit D)? xoris deja 0 en la mitad alta solo si coincide; cmplwi
		 * compara entonces la baja. */
		uint32_t v = rec->x.ktlb[salsa64_rec_vt_slot(pc)];
		int32_t off = 4 * (int32_t)salsa64_rec_vt_slot(pc) - (int32_t)sizeof rec->x.ktlb;
		if ((v & ~SALSA64_REC_VT_DIRTY) != salsa64_rec_vt_value(pc, pa, false))
			return 0;  /* no debería pasar: la página no está anotada como se pidió */
		vt_miss = new_label(c);
		if (off < -32768) {
			E(c, ppc_addis(T_X, R_FLAGS, -1));
			E(c, ppc_lwz(0, off + 65536, T_X));
		} else {
			E(c, ppc_lwz(0, off, R_FLAGS));
		}
		E(c, ppc_xoris(0, 0, v >> 16));
		E(c, ppc_cmplwi(0, 0, v & 0xFFFF));
		emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(0, PPC_EQ), vt_miss);
	}
	E(c, ppc_addi(0, R_CYCLES, (int32_t)n));
	E(c, ppc_cmpld(0, 0, R_NEXT));
	emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(0, PPC_GT), start_exit);
	int fr_exit = -1;
	if (any_cop1 && c->fr_check) {
		/* CU1 y FR de una vez: rotar 6 deja CU1, CU0, RP y FR en los cuatro
		 * bits bajos y la máscara envolvente (bits 31 y 0–28) quita CU0 y
		 * RP. Si no es «CU1 = 1 y el FR de la compilación», al despachador
		 * (exit_fr: busca el gemelo, o con CU1 = 0 interpreta). */
		fr_exit = new_label(c);
		E(c, ppc_lwz(0, OFF(status), R_CPU));
		E(c, ppc_rlwinm(0, 0, 6, 28, 31));
		E(c, ppc_rlwinm(0, 0, 0, 31, 28));
		E(c, ppc_cmpwi(0, 0, 8 | (rec->fr ? 1 : 0)));
		emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(0, PPC_EQ), fr_exit);
	} else if (any_cop1) {
		E(c, ppc_lwz(0, OFF(status), R_CPU));
		E(c, ppc_rlwinm(0, 0, 3, 31, 31));  /* CU1 (bit 29); andis. es microcódigo */
		E(c, ppc_cmpwi(0, 0, 0));
		emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(0, PPC_EQ), start_exit);
	}
	if (rec->profile && rec->cur_id && !temp) {
		/* Perfil persistente (rec_profile.h): hits[id]++ al entrar (no en
		 * cada vuelta de un bucle), a desplazamiento fijo de flags (r28). */
		int32_t off = (int32_t)(offsetof(struct salsa64_rec, hits) - offsetof(struct salsa64_rec, flags)) +
			      4 * (int32_t)rec->cur_id;
		int32_t hi = (off + 0x8000) >> 16, lo = off - (hi << 16);
		E(c, ppc_addis(T_X, R_FLAGS, hi));
		E(c, ppc_lwz(T_Y, lo, T_X));
		E(c, ppc_addi(T_Y, T_Y, 1));
		E(c, ppc_stw(T_Y, lo, T_X));
	}
	if (rec->sample && rec->cur_id && !temp) {  /* muestreo (rec_sample.h): el bloque en curso */
		E(c, ppc_li(0, (int16_t)rec->cur_id));
		E(c, ppc_sth(0, OFF(rec_cur), R_CPU));
	}
	if (rec->trace && rec->trace->seq && rec->cur_id && !temp && !(rec->opts & SALSA64_REC_OPT_FPU_RES)) {
		/* Perfilado (rec_trace.h): con ventana abierta (cr3 ne), el id a la
		 * ventana por un tramo frío; cerrada, una palabra. */
		int stub = new_label(c), back = new_label(c);
		emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(3, PPC_EQ), stub);
		bind(c, back);
		c->sec = SEC_COLD;
		bind(c, stub);
		E(c, ppc_li(0, (int16_t)rec->cur_id));
		emit_b(c, abs_label(c, rec->trace_rtn), true);
		emit_b(c, back, false);
		c->sec = SEC_HOT;
	}
	for (unsigned r = 1; c->borrowed && r < 32; r++)  /* fijos prestados: a cpu->gpr */
		if (c->borrowed & BIT(r))
			E(c, ppc_std(pin_ppc(r), off_reg(r), R_CPU));
	reload(c, livein & ~c->pinned);
	if (c->fpu_res)
		fpu_entry(c);
	c->body = new_label(c);
	bind(c, c->body);
	if (rec->prof && rec->prof->blocks && rec->cur_id) {  /* perfil por bloque: una cuenta por paso por el cuerpo */
		li64(c, T_X, (uintptr_t)&rec->prof->blocks->hits[rec->cur_id]);
		E(c, ppc_ld(T_Y, 0, T_X));
		E(c, ppc_addi(T_Y, T_Y, 1));  /* no r0: en addi vale 0 */
		E(c, ppc_std(T_Y, 0, T_X));
	}

	cls_set(c, SALSA64_RT_C_ALU);
	for (unsigned k = 0; k < n; k++) {
		unsigned cls = cls_set(c, SALSA64_RT_C_BRANCH);
		LAB_MARK(c, k);
		emit_ijumps_to(c, k);
		if (c->has_branch && k == n - 2) {
			c->cls_n[SALSA64_RT_C_BRANCH]++;
			emit_branch(c, k);
			break;
		}
		if (is_branch(&c->d[k])) {
			c->cls_n[SALSA64_RT_C_BRANCH]++;
			emit_internal_branch(c, k);
			cls_set(c, cls);
			k++;
			continue;
		}
		cls_set(c, cls);
		unsigned seg_end = k;  /* un par LWL/LWR no cruza un salto */
		while (seg_end < n && !is_branch(&c->d[seg_end]))
			seg_end++;
		cls_set(c, SALSA64_RT_C_MEM);
		if (emit_unaligned_pair(c, k, seg_end)) {
			c->cls_n[SALSA64_RT_C_MEM] += 2;
			cls_set(c, cls);
			k++;
			continue;
		}
		cls_set(c, cls);
		emit_insn(c, k);
		if (c->d[k].kind == K_END)
			break;
	}
	cls_set(c, SALSA64_RT_C_EXIT);
	if (!c->has_branch && c->d[n - 1].kind != K_END) {
		writeback(c, c->written);
		E(c, ppc_addi(R_CYCLES, R_CYCLES, (int32_t)n));
		emit_site(c, n, SALSA64_RPF_SITE_END, pc + 4ull * n);
		emit_exit_to(c, pc + 4ull * n);
	}
	LAB_MARK(c, n);

	c->sec = SEC_COLD;
	bind(c, start_exit);
	li64(c, 3, pc);
	emit_b(c, abs_label(c, rec->exit_start), false);
	if (fr_exit >= 0) {
		bind(c, fr_exit);
		li64(c, 3, pc);
		emit_b(c, abs_label(c, rec->exit_fr), false);
	}
	if (vt_miss >= 0) {  /* la página cambió de traducción: que la busque el despachador */
		bind(c, vt_miss);
		li64(c, 3, pc);
		emit_b(c, abs_label(c, rec->exit_jump), false);
	}
	emit_pending_exits(c);
	if (!c->wide) {
		emit_chain(c);
		emit_trampolines(c);
	}
	cls_set(c, SALSA64_RT_C_EXIT);

	return finalize(c, base, limit);
}

/* Perfilado (rec_trace.h): cómo termina el bloque recién emitido, su huella
 * y sus palabras por clase, para salsa64_rec_trace_compiled (rec.c). */
static void trace_describe(struct ctx *c, uint32_t words)
{
	struct salsa64_rec *rec = c->rec;
	struct salsa64_rec_trace_block *tb = &rec->trace->cur;
	unsigned n = c->n, end = SALSA64_RT_END_LIMIT;
	uint32_t w[SALSA64_REC_MAX_INSNS];
	for (unsigned k = 0; k < n; k++)
		w[k] = c->d[k].w;
	if (c->has_branch) {
		uint32_t i = c->d[n - 2].w;
		uint64_t bpc = c->pc + 4ull * (n - 2);
		if (c->d[n - 2].kind == K_JUMP)
			tb->target = (uint32_t)(((bpc + 4) & ~0x0FFFFFFFull) | (uint64_t)I_TARGET(i) << 2);
		else if (c->d[n - 2].kind != K_JUMP_REG)
			tb->target = (uint32_t)(bpc + 4 + ((uint64_t)(int64_t)(int16_t)i << 2));
		switch (c->d[n - 2].kind) {
		case K_JUMP_REG:
			end = I_FUNCT(i) == 0x09 ? SALSA64_RT_END_JALR : I_RS(i) == 31 ? SALSA64_RT_END_JR_RA : SALSA64_RT_END_JR;
			break;
		case K_JUMP:
			end = I_OP(i) == 0x03 ? SALSA64_RT_END_JAL : SALSA64_RT_END_J;
			break;
		case K_BRANCH_L:
			end = SALSA64_RT_END_LIKELY;
			break;
		default:
			end = SALSA64_RT_END_BRANCH;
			break;
		}
	} else if (c->d[n - 1].kind == K_END) {
		end = SALSA64_RT_END_C;
	}
	tb->hash = c->pent ? rec->profile->entries[c->pent - 1].hash : salsa64_rec_profile_hash(w, n);
	tb->end = (uint8_t)(end | (c->loop ? SALSA64_RT_END_LOOP : 0));
	tb->nbr = c->cls_n[SALSA64_RT_C_BRANCH];
	uint32_t hot = rec->nbuf[SEC_HOT];
	tb->hot = (uint16_t)(hot > 0xFFFF ? 0xFFFF : hot);
	tb->cold = (uint16_t)(words - hot > 0xFFFF ? 0xFFFF : words - hot);
	for (unsigned k = 0; k < SALSA64_RT_CLASSES; k++) {
		tb->ins[k] = c->cls_n[k];
		tb->whot[k] = (uint16_t)(c->cls_w[k][SEC_HOT] > 0xFFFF ? 0xFFFF : c->cls_w[k][SEC_HOT]);
		tb->wcold[k] = (uint16_t)(c->cls_w[k][SEC_COLD] > 0xFFFF ? 0xFFFF : c->cls_w[k][SEC_COLD]);
	}
}

uint32_t salsa64_rec_compile(struct salsa64_rec *rec, uint64_t pc, uint32_t pa, unsigned seg, unsigned max,
			     bool temp, unsigned *n_out, uint32_t *bytes_out)
{
	static struct ctx ctx;  /* grande para la pila; el recompilador corre en un solo hilo */
	struct ctx *c = &ctx;
	uint32_t end = salsa64_rec_code_end(rec, pa, seg);  /* fuera de KSEG0, sin cruzar páginas */

	memset(c, 0, sizeof *c);
	c->idle_patch = -1;
	rec->compiled_idle_patch = 0;
	c->rec = rec;
	c->pc = pc;
	c->pa = pa;
	c->temp = temp;
	c->seg_kind = (uint8_t)seg;
	c->wide = (rec->opts & SALSA64_REC_OPT_WIDE) != 0;
	c->fastmem = rec->fastmem && !c->wide;
	c->tramp = -1;
	if (max > SALSA64_REC_MAX_INSNS)
		max = SALSA64_REC_MAX_INSNS;

	/* Decodificación: hasta un salto con su delay slot, una instrucción que
	 * termina el bloque, el máximo o el final de la RDRAM. Un salto cuyo
	 * delay slot no se puede compilar (otro salto, COP0…) queda fuera. */
	/* Con CU1 = 0 (un hilo que no usa la FPU), el bloque se corta antes de
	 * su primera instrucción de COP1 si no es la primera: la entrada de un
	 * bloque con COP1 revisa CU1 y saldría siempre, aunque el COP1 esté en
	 * un camino que no se toma (el guardado y la reposición de la FPU en el
	 * cambio de hilo de libultra, sobre el hilo ocioso). */
	bool cu1_cut = !(rec->cpu->status & SALSA64_ST_CU1) && !SALSA64_REC_OLD(rec, SALSA64_REC_DBG_NO_CU1_CUT);
	unsigned n = 0;
	while (n < max && pa + 4 * n + 4 <= end) {
		struct dec *d = &c->d[n];
		decode_ext(rec, fetch(rec, pa + 4 * n), d);
		if (cu1_cut && n && d->cop1)
			break;
		if (is_branch(d)) {
			if (n + 1 >= max || pa + 4 * n + 8 > end)
				break;
			decode_ext(rec, fetch(rec, pa + 4 * n + 4), &c->d[n + 1]);
			if (c->d[n + 1].kind != K_NORMAL || (cu1_cut && n && c->d[n + 1].cop1))
				break;
			c->ds_of[n + 1] = (uint8_t)(n + 1);
			/* Un condicional hacia adelante no corta el bloque: su camino
			 * tomado sale enlazado (emit_internal_branch). */
			uint64_t bpc = pc + 4ull * n, target = bpc + 4 + ((uint64_t)(int64_t)(int16_t)d->w << 2);
			bool internal = (d->kind == K_BRANCH || d->kind == K_BRANCH_L) && target > bpc &&
					!SALSA64_REC_OLD(rec, SALSA64_REC_DBG_NO_SUPER);
			n += 2;
			if (!internal) {
				c->has_branch = true;
				break;
			}
			continue;
		}
		n++;
		if (d->kind == K_END)
			break;
	}
	if (n == 0)
		return 0;
	c->n = n;
	for (unsigned k = 0; k < n; k++)
		c->fr_check |= fr_dependent(c->d[k].w);
	rec->compiled_fr_dep = c->fr_check;

	/* Perfil persistente (rec_profile.h): la entrada del bloque por su
	 * dirección y la huella de sus instrucciones, y si va a la zona
	 * caliente. Un bloque cuya huella no coincide es otro bloque. */
	unsigned pent = 0;
	bool hot = false;
	if (rec->profile && !temp && rec->cur_id) {
		uint32_t w[SALSA64_REC_MAX_INSNS];
		for (unsigned k = 0; k < n; k++)
			w[k] = c->d[k].w;
		pent = salsa64_rec_profile_lookup(rec->profile, (uint32_t)pc, salsa64_rec_profile_hash(w, n), n, 0, true);
		hot = pent && rec->hot_words && rec->profile->entries[pent - 1].hot;
		rec->pent[rec->cur_id] = (uint16_t)pent;
		salsa64_rec_profile_set_flags(rec->profile, pent, (c->fr_check ? SALSA64_RPF_FR_DEP : 0) |
				(rec->fr ? SALSA64_RPF_FR_SEEN1 : SALSA64_RPF_FR_SEEN0));
		c->pent = pent;
	}

	/* Si un bloque caliente no cabe en su mitad, se emite de nuevo para la
	 * zona fría (las llamadas a C dependen de dónde cae) y la próxima
	 * compilación pasa a la otra mitad (make_room, rec.c). */
	uint32_t base, words;
	for (;;) {
		base = temp ? rec->scratch : hot ? rec->used_hot : rec->used;
		uint32_t limit = temp ? rec->code_words : hot ? rec->hot_end : rec->seg_end;
		c->hot = hot;
		words = emit_block(c, base, limit);
		if (words)
			break;
		if (!hot)
			return 0;
		hot = false;
		rec->hot_full = true;
		rec->stats.hot_full++;
		reset_emission(c);
	}
	if (!temp) {
		uint32_t next = (base + words + 31) & ~31u;  /* bloques alineados a 128 bytes */
		if (hot)
			rec->used_hot = next;
		else
			rec->used = next;
		rec->placed_hot = hot;
		if (pent)
			salsa64_rec_profile_set_words(rec->profile, pent, words);
		if (rec->sample && rec->cur_id)
			salsa64_rec_sample_compiled(rec->sample, rec->cur_id, words, rec->nbuf[SEC_HOT]);
	}
	if (rec->trace && rec->cur_id && !temp)
		trace_describe(c, words);
	if (rec->prof && rec->prof->blocks && rec->cur_id && !temp) {
		struct salsa64_rec_prof_block *pb = &rec->prof->blocks->info[rec->cur_id];
		unsigned fpu = 0, mem = 0;
		pb->regs = 0;
		for (unsigned k = 0; k < n; k++) {
			uint32_t w = c->d[k].w;
			pb->regs |= c->d[k].reads | c->d[k].writes;
			fpu += I_OP(w) == 0x11 && (I_FMT(w) & ~1u) == FMT_S;
			mem += I_OP(w) >= 0x20 && I_OP(w) != 0x2F;
		}
		pb->pc = (uint32_t)pc;
		pb->n = (uint16_t)n;
		pb->fpu = (uint8_t)fpu;
		pb->mem = (uint8_t)mem;
	}
	if (rec->on_compile)
		rec->on_compile(rec, pc, base, words, temp);
	*n_out = n;
	*bytes_out = 4 * n;
	return base;
}
