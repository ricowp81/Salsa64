/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — CPU NEC VR4300 (MIPS III, 64 bits): estado y API del intérprete.
 *
 * Modelo de ejecución:
 *   - `pc` es la próxima instrucción a ejecutar y `next_pc` la siguiente
 *     (dentro de salsa64_cpu_run viven en registros; ver interp.c).
 *     Un salto escribe `next_pc` y marca `branch`: la instrucción que sigue
 *     (el delay slot) se ejecuta con `delay_slot` = 1 y luego el flujo va al
 *     destino. Un branch-likely no tomado salta el delay slot.
 *   - Toda excepción usa `cur_pc` (dirección de la instrucción en curso) y
 *     `delay_slot` para EPC y Cause.BD, y deja `pc` en el vector.
 *   - El tiempo es `cycles` (PClock, 93,75 MHz). Count avanza a la mitad.
 *     Por ahora cada instrucción cuesta 1 ciclo; el costo real (caché,
 *     multiplicaciones, divisiones) se calibra más adelante.
 *   - Bucles de espera (idle.c): un salto corto hacia atrás cuyo cuerpo no
 *     cambia nada (solo lee la RDRAM y registros) se salta en vueltas
 *     enteras hasta el próximo evento del planificador. El resultado es el
 *     mismo, ciclo a ciclo, que ejecutarlo; `idle_cycles` cuenta lo saltado.
 *
 * Alcance de la Fase 1:
 *   - Direccionamiento de 32 y de 64 bits (KX, SX, UX): xkuseg, xksseg,
 *     xkphys, xkseg y los segmentos compatibles, con direcciones virtuales
 *     de 40 bits y físicas de 32.
 *   - TLB de 32 entradas con refill (TLB y XTLB), inválida y modificación.
 *   - FPU: formatos S, D, W, L; modo FR; los cuatro modos de redondeo y
 *     las causas de FCR31, incluida la bandera "inexacto" (cop1.c).
 *   - Sin cachés, sin watchpoints, sin errores de bus.
 */
#ifndef SALSA64_CPU_VR4300_H
#define SALSA64_CPU_VR4300_H

#include "core/types.h"

struct salsa64_bus;
struct salsa64_sched;

/* Status */
#define SALSA64_ST_IE   0x00000001u
#define SALSA64_ST_EXL  0x00000002u
#define SALSA64_ST_ERL  0x00000004u
#define SALSA64_ST_KSU  0x00000018u
#define SALSA64_ST_UX   0x00000020u
#define SALSA64_ST_SX   0x00000040u
#define SALSA64_ST_KX   0x00000080u
#define SALSA64_ST_IM   0x0000FF00u
#define SALSA64_ST_BEV  0x00400000u
#define SALSA64_ST_RE   0x02000000u
#define SALSA64_ST_FR   0x04000000u
#define SALSA64_ST_CU0  0x10000000u
#define SALSA64_ST_CU1  0x20000000u
#define SALSA64_ST_CU2  0x40000000u

/* Cause */
#define SALSA64_CAUSE_BD       0x80000000u
#define SALSA64_CAUSE_CE_SHIFT 28
#define SALSA64_CAUSE_IP_SHIFT 8
/* Línea IP4 del Cause (bit 12): la interrupción de PRE_NMI del botón Reset. */
#define SALSA64_IP_PRENMI 4
#define SALSA64_CAUSE_EXC(c)   (((c) >> 2) & 0x1F)

/* FCR31 */
#define SALSA64_FCR31_C  0x00800000u
#define SALSA64_FCR31_FS 0x01000000u

#define SALSA64_PRID_VALUE   0x00000B22u
#define SALSA64_FCR0_VALUE   0x00000A00u
#define SALSA64_TLB_ENTRIES  32
#define SALSA64_IDLE_CACHE   1024 /* saltos descartados como bucle de espera (potencia de 2) */
/* Ranura de un salto en las tablas de bucles de espera (hash multiplicativo
 * de los 32 bits bajos): con los bits 2–11 solos, 0x80001510 y 0x80011510
 * (el osInvalDCache de NFL Blitz y su bucle principal) se pisaban el
 * descarte en cada vuelta y se volvían a analizar. */
#define SALSA64_IDLE_SLOT(pc) ((unsigned)(((uint32_t)(pc) * 0x9E3779B1u) >> 22) & (SALSA64_IDLE_CACHE - 1))

#ifdef SALSA64_MEM_CENSUS
/* Censo de los accesos a datos del intérprete por segmento y tamaño de
 * página de la TLB (docs/fastmem.md): solo en el host, compilando con
 * -DSALSA64_MEM_CENSUS; salsa64-run lo imprime al terminar. */
struct salsa64_mem_census {
	uint64_t kseg0, kseg1, misaligned, erl, x64, kseg23, miss, invalid, modified;
	uint64_t tlb_ps[13];      /* accesos traducidos por la TLB, por log2(página) - 12 */
	uint64_t tlbw_ps[13];     /* escrituras de la TLB, por tamaño de página */
	uint64_t collide_1g, collide_2g;  /* accesos por la TLB cuya dirección doblada a 1 o 2 GB cae en una imagen de la RDRAM */
	uint32_t slots[8];        /* ranuras de 8 MB de kuseg/kseg2-3 tocadas por la TLB (bit por ranura de 32 bits de dirección) */
};
extern struct salsa64_mem_census salsa64_mem_census;
#endif

enum salsa64_exc {
	SALSA64_EXC_INT  = 0,
	SALSA64_EXC_MOD  = 1,
	SALSA64_EXC_TLBL = 2,
	SALSA64_EXC_TLBS = 3,
	SALSA64_EXC_ADEL = 4,
	SALSA64_EXC_ADES = 5,
	SALSA64_EXC_IBE  = 6,
	SALSA64_EXC_DBE  = 7,
	SALSA64_EXC_SYS  = 8,
	SALSA64_EXC_BP   = 9,
	SALSA64_EXC_RI   = 10,
	SALSA64_EXC_CPU  = 11,
	SALSA64_EXC_OV   = 12,
	SALSA64_EXC_TR   = 13,
	SALSA64_EXC_FPE  = 15,
	SALSA64_EXC_WATCH = 23,
};

enum salsa64_cop0_reg {
	SALSA64_CP0_INDEX = 0, SALSA64_CP0_RANDOM = 1, SALSA64_CP0_ENTRYLO0 = 2,
	SALSA64_CP0_ENTRYLO1 = 3, SALSA64_CP0_CONTEXT = 4, SALSA64_CP0_PAGEMASK = 5,
	SALSA64_CP0_WIRED = 6, SALSA64_CP0_BADVADDR = 8, SALSA64_CP0_COUNT = 9,
	SALSA64_CP0_ENTRYHI = 10, SALSA64_CP0_COMPARE = 11, SALSA64_CP0_STATUS = 12,
	SALSA64_CP0_CAUSE = 13, SALSA64_CP0_EPC = 14, SALSA64_CP0_PRID = 15,
	SALSA64_CP0_CONFIG = 16, SALSA64_CP0_LLADDR = 17, SALSA64_CP0_WATCHLO = 18,
	SALSA64_CP0_WATCHHI = 19, SALSA64_CP0_XCONTEXT = 20,
	SALSA64_CP0_PARITYERROR = 26, SALSA64_CP0_CACHEERROR = 27,
	SALSA64_CP0_TAGLO = 28, SALSA64_CP0_TAGHI = 29, SALSA64_CP0_ERROREPC = 30,
};

/* Modo de operación efectivo (Status.KSU con EXL/ERL). */
enum salsa64_cpu_mode {
	SALSA64_MODE_KERNEL = 0,
	SALSA64_MODE_SUPERVISOR = 1,
	SALSA64_MODE_USER = 2,
};

struct salsa64_tlb_entry {
	uint64_t entry_hi;     /* VPN2 (enmascarado por page_mask) y ASID */
	uint32_t entry_lo[2];  /* PFN, C, D, V (G aparte) */
	uint32_t page_mask;
	uint64_t vpn_mask;     /* bits de la dirección virtual que se comparan (R y VPN2) */
	uint32_t odd_bit;      /* bit que elige entry_lo[1] */
	uint32_t global;
};

struct salsa64_cpu {
	/*
	 * Estado de ejecución, ordenado según lo usa el bucle del intérprete
	 * (interp.c). En el PPE, leer un dato recién escrito que sigue en la
	 * cola de almacenamiento (load-hit-store) cuesta decenas de ciclos: lo
	 * que el bucle escribe en cada instrucción queda en otros 16 bytes que
	 * lo que lee en cada instrucción.
	 */
	uint64_t gpr[32];      /* gpr[0] nunca se escribe (vr4300_set_gpr) */
	uint64_t hi, lo;

	/* Escritos en cada instrucción para el código fuera del bucle
	 * (excepciones, COP0); el bucle no los lee. */
	uint64_t cycles;
	uint64_t cur_pc;
	uint8_t delay_slot;    /* la instrucción en curso está en un delay slot */
	uint8_t branch;        /* la instrucción en curso es un salto */
	uint8_t llbit;
	uint8_t running;
	uint8_t re_xor;        /* 7 en modo usuario con Status.RE (little-endian), si no 0 */

	/* Dentro del bucle, pc y next_pc viven en registros: aquí se escriben al
	 * salir y los escriben las excepciones, ERET y salsa64_cpu_set_pc. */
	uint64_t pc;
	uint64_t next_pc;

	/* Leídos en cada instrucción; solo cambian en caminos lentos. */
	uint8_t *rdram;        /* copia de bus->rdram para el camino rápido */
	uint32_t rdram_size;
	/* Estos cuatro bytes van juntos y alineados: el código del recompilador
	 * los lee con una sola carga después de llamar a C (rec.c) y sale del
	 * bloque si alguno no es 0. */
	uint8_t irq_pending;   /* hay interrupción habilitada y no enmascarada */
	uint8_t redirect;      /* pc/next_pc/branch cambiaron fuera del bucle: recargarlos */
	uint8_t rec_abort;     /* el recompilador invalidó código: volver a su despachador */
	uint8_t mode;          /* enum salsa64_cpu_mode */
	uint8_t allow64;       /* operaciones de 64 bits permitidas en este modo */

	/* COP0 */
	uint32_t status;
	uint32_t cause;
	uint32_t compare;
	uint32_t count_offset;   /* Count = (cycles >> 1) + count_offset */
	uint32_t index;
	uint32_t entry_lo0, entry_lo1;
	uint32_t page_mask;
	uint32_t wired;
	uint32_t config;
	uint32_t lladdr;
	uint32_t watch_lo, watch_hi;
	uint32_t parity_error, cache_error;
	uint32_t tag_lo, tag_hi;
	uint64_t random_epoch;   /* ciclo en que Random valía 31 */
	uint64_t context;
	uint64_t xcontext;
	uint64_t bad_vaddr;
	uint64_t entry_hi;
	uint64_t epc;
	uint64_t error_epc;
	uint64_t cop0_latch;     /* último valor escrito: lo leen los registros 7, 21–25, 31 */

	/* COP1 */
	uint64_t fpr[32];
	uint32_t fcr31;

	uint64_t cop2_latch;     /* MTC2/DMTC2/CTC2 escriben aquí; MFC2/DMFC2/CFC2 leen */

	struct salsa64_tlb_entry tlb[SALSA64_TLB_ENTRIES];

	struct salsa64_bus *bus;
	struct salsa64_sched *sched;

	/* Recompilador (rec.h): NULL si corre solo el intérprete. code_flags
	 * marca, por cada 128 bytes de la RDRAM, si hay código recompilado:
	 * un almacenamiento ahí avisa al recompilador para invalidarlo. */
	struct salsa64_rec *rec;
	const uint8_t *code_flags;
	uint32_t exc_count;      /* excepciones tomadas (el despachador del recompilador lo mira) */
	uint32_t tlb_writes;     /* sube con TLBWI, TLBWR y el reset: el recompilador revisa sus traducciones */
	uint32_t rec_kind;       /* tipo de salida del código generado (rec.c) */
	uint16_t rec_cur;        /* muestreo (rec_sample.h): id del bloque en curso (lo deja su entrada) */
	uint8_t rec_mark;        /* muestreo: 0 en código generado, o la categoría de lo que hace en C */
	uint64_t rec_site;       /* dirección del enlace a parchear, o 0; en un bucle de espera, la instrucción del salto */
	uint64_t rec_lr, rec_tmp;  /* las rutinas compartidas del camino lento (rec.c) guardan LR y el fin del bloque */
	uint64_t rec_tb;           /* la base en caché de un grupo (T_HI) mientras el código generado llama a C (rec_emit.c) */
	uint64_t rec_fw[32];       /* la palabra W de fpr[p] con signo extendido, para CVT.S.W/CVT.D.W con fcfid (rec_emit_fpu.h) */
	/* Perfilado (rec_trace.h): ventana abierta (la lee el preámbulo al
	 * entrar desde C), su siguiente lugar y su fin; la tabla de JR por id
	 * y cuántos JR van (exit_jr_count). */
	uint32_t rec_tron;
	uint32_t *rec_tpos, *rec_tend;
	void *rec_jrtab;
	uint64_t rec_jr_count;

	/* Depuración: si no es NULL, se llama al tomar cada excepción (con
	 * Cause, EPC y el vector ya escritos). */
	void (*exc_hook)(struct salsa64_cpu *cpu);

	/* Barrido de la caché de datos (salsa64_cpu_set_dcache_sweep): ciclos
	 * que se cobran al empezar uno (0 = gratis, como antes) y cuántos van. */
	uint32_t dcache_sweep_cycles;
	uint32_t dcache_sweeps;

	/* Bucles de espera (idle.c). Las tablas guardan pc ^ instrucción del
	 * salto: descartados para siempre, con fallos seguidos (cuántos en
	 * idle_fails) y que ya fueron de espera alguna vez. */
	uint64_t idle_cycles;    /* ciclos saltados desde el arranque */
	uint8_t idle_off;        /* salsa64_cpu_set_idle(cpu, false) */
	uint32_t idle_reject[SALSA64_IDLE_CACHE];
	uint32_t idle_strike[SALSA64_IDLE_CACHE];
	uint32_t idle_proven[SALSA64_IDLE_CACHE];
	uint8_t idle_fails[SALSA64_IDLE_CACHE];
};

void salsa64_cpu_init(struct salsa64_cpu *cpu, struct salsa64_bus *bus,
		      struct salsa64_sched *sched);
/* Reset en frío: Status.ERL|BEV, PC = 0xBFC00000 (PIF ROM). */
void salsa64_cpu_reset(struct salsa64_cpu *cpu);
void salsa64_cpu_set_pc(struct salsa64_cpu *cpu, uint64_t pc);

/* Ejecuta una instrucción (o atiende una interrupción pendiente). */
void salsa64_cpu_step(struct salsa64_cpu *cpu);
/* Ejecuta hasta que un evento ponga running = 0 (ver salsa64_n64_run). Con
 * cpu->rec, corre el recompilador (rec.h) en vez del intérprete. */
void salsa64_cpu_run(struct salsa64_cpu *cpu);

/* Activa o apaga el salto de bucles de espera (activo por omisión). Apagarlo
 * da el mismo resultado, solo más lento: sirve para comparar y medir. */
void salsa64_cpu_set_idle(struct salsa64_cpu *cpu, bool on);

/*
 * Costo de un barrido de la caché de datos (activo por omisión). Sin cachés
 * emuladas, CACHE es gratis; en la consola, Index_Writeback_Invalidate (op
 * 0x01) escribe de vuelta cada línea sucia de 16 bytes, unos 40 ciclos cada
 * una (ares, `ares/n64/cpu/dcache.cpp`). libultra barre las 512 líneas
 * desde la 0 en osWritebackDCacheAll y en osWritebackDCache/osInvalDCache
 * de 8 KB o más: cada CACHE 0x01 sobre la línea 0 cobra
 * SALSA64_DCACHE_SWEEP_CYCLES (la mitad de las líneas sucias). Medido con un
 * modelo de la caché al estilo ares, un barrido encuentra en promedio 107
 * (GoldenEye) a 293 (Zelda OoT) líneas sucias, hasta 512. Sin ese costo,
 * Forsaken 64 manda un pedido a su hilo del SI 25 µs después de que este
 * empieza a esperar la escritura de la PIF RAM (65 µs) y libultra lo toma
 * por el aviso del SI (issue #55). Apagarlo vuelve a CACHE gratis. Va antes
 * de correr: el recompilador decide al compilar si lo emite.
 */
#define SALSA64_DCACHE_SWEEP_CYCLES (256u * 40u)
void salsa64_cpu_set_dcache_sweep(struct salsa64_cpu *cpu, bool on);

/* Línea de interrupción externa: 2 = RCP (MI), 7 = Count/Compare. */
void salsa64_cpu_set_ip(struct salsa64_cpu *cpu, unsigned line, bool level);

uint32_t salsa64_cpu_count(const struct salsa64_cpu *cpu);
uint64_t salsa64_cpu_cop0_read(struct salsa64_cpu *cpu, unsigned reg);
void salsa64_cpu_cop0_write(struct salsa64_cpu *cpu, unsigned reg, uint64_t value);

#endif
