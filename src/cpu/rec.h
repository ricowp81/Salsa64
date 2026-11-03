/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — recompilador dinámico MIPS (VR4300) → PowerPC64 para el PPE.
 *
 * Diseño y decisiones en docs/rec-ppe.md. Resumen:
 *   - Unidad: bloque básico que empieza en una dirección de KSEG0 dentro de
 *     la RDRAM y termina en un salto (con su delay slot), en una instrucción
 *     que cambia el estado global (COP0, SYSCALL, BREAK) o a las
 *     SALSA64_REC_MAX_INSNS instrucciones. También desde KSEG1 o mapeado por
 *     la TLB, en la RDRAM o en la ROM (rec_tlb.h). Lo demás (modos
 *     usuario/supervisor, interrupciones, delay slots pendientes) lo corre el
 *     intérprete, que es la referencia.
 *   - Mismo resultado que el intérprete, instrucción por instrucción y ciclo
 *     por ciclo: un bloque solo corre en nativo si cabe entero antes del
 *     próximo evento, y después de cada llamada a C el código revisa
 *     excepciones, interrupciones y eventos y sale si hace falta.
 *   - Registros MIPS de 64 bits en registros del PPE de 64 bits (r14–r25),
 *     asignados por bloque; el resto se lee y escribe en cpu->gpr.
 *   - Enlace directo entre bloques (se parchea un `b`), invalidación por
 *     escritura con una tabla de 128 bytes por entrada (code_flags) que
 *     revisan los almacenamientos nativos y el intérprete.
 *
 * Toda la memoria va dentro de struct salsa64_rec (unos 7 MB, se reserva una
 * vez al arrancar) salvo la caché de código, que la pone quien llama: en la
 * PS3, la cola NOBITS del segmento de texto (docs/jit-ppe.md); en un host
 * ppc64 Linux, un mmap con permiso de ejecución. En otras arquitecturas
 * salsa64_rec_init devuelve false y todo corre en el intérprete.
 */
#ifndef SALSA64_CPU_REC_H
#define SALSA64_CPU_REC_H

#include "core/bus.h"
#include "core/types.h"
#include "cpu/rec_prof.h"
#include "cpu/rec_diag.h"
#include "cpu/rec_sample.h"
#include "cpu/rec_profile.h"
#include "cpu/rec_tlb.h"
#include "cpu/rec_trace.h"

struct salsa64_cpu;

#define SALSA64_REC_MAX_INSNS   64        /* instrucciones MIPS por bloque */
#define SALSA64_REC_MAX_BLOCKS  65535u    /* ids 1..65535; 0 = sin bloque */
#define SALSA64_REC_MAX_LINKS   (1u << 17)
#define SALSA64_REC_MAP_WORDS   (SALSA64_RDRAM_MAX / 4)
#define SALSA64_REC_FLAG_SHIFT  7         /* code_flags: una entrada por 128 bytes */
#define SALSA64_REC_FLAG_BYTES  (SALSA64_RDRAM_MAX >> SALSA64_REC_FLAG_SHIFT)
#define SALSA64_REC_BUF_WORDS   16384     /* búfer de compilación por sección */
#define SALSA64_REC_MAX_LABELS  4096
#define SALSA64_REC_MAX_FIXUPS  8192
#define SALSA64_REC_SEGMENTS    8         /* tramos de la caché: llena, se desaloja el más viejo (docs/rec-ppe.md) */
/* Zona caliente (docs/rec-ppe.md, «Perfil persistente y bloques calientes»):
 * los bloques que el perfil del juego marca como calientes van juntos, al
 * principio de la caché, en dos mitades que se turnan como los tramos. */
#define SALSA64_REC_HOT_SEGMENTS 2
#define SALSA64_REC_ALL_SEGMENTS (SALSA64_REC_HOT_SEGMENTS + SALSA64_REC_SEGMENTS)

/* 16 bytes: la búsqueda de destinos de JR en el código generado indexa
 * blocks[] con un desplazamiento y lee `code` en +0. */
struct salsa64_rec_block {
	uint32_t code;       /* palabra de entrada en la caché; 0 = lo corre el intérprete */
	uint32_t pa;         /* dirección física de la primera instrucción */
	uint32_t links;      /* enlaces que llegan aquí: índice + 1 en links[], 0 = ninguno;
			      * en un id libre, el siguiente id libre */
	uint16_t bytes;      /* bytes de MIPS que cubre */
	uint8_t n;           /* instrucciones del camino más largo (≤ SALSA64_REC_MAX_INSNS) */
	uint8_t fr;          /* SALSA64_REC_FR_*: Status.FR con que se compiló y si el código depende de él */
};
/* Un bloque que lee o escribe un registro impar de la FPU depende de
 * Status.FR (con FR = 0 el impar es la mitad alta de la pareja): su entrada
 * revisa el bit y, si cambió, sale al despachador (exit_fr), que busca o
 * compila el gemelo para el otro FR (twin[]). Los demás bloques valen con
 * los dos (docs/rec-ppe.md, «Status.FR»). */
#define SALSA64_REC_FR_SET  1u  /* compilado con FR = 1 */
#define SALSA64_REC_FR_DEP  2u  /* el código depende de FR */
/* Compilado con Status.CU1 = 0: el bloque termina antes de su primera
 * instrucción de COP1 (si no es la primera), así corre en nativo con los
 * dos CU1. Un bloque compilado con CU1 = 1 que sale en la entrada por
 * CU1 = 0 se mata y se recompila así (docs/rec-ppe.md, «CU1 = 0»). */
#define SALSA64_REC_CU1_OFF 4u
/* ¿El bloque vale con Status.FR = `fr`? */
static inline bool salsa64_rec_block_fr_ok(const struct salsa64_rec_block *b, bool fr)
{
	return !(b->fr & SALSA64_REC_FR_DEP) || ((b->fr & SALSA64_REC_FR_SET) != 0) == fr;
}

/* Por qué se vació la caché entera (stats.flushes_by[]). */
enum salsa64_rec_flush_cause {
	SALSA64_REC_FLUSH_IDS,     /* se agotaron los ids de bloque */
	SALSA64_REC_FLUSH_LINKS,   /* se agotaron los enlaces */
	SALSA64_REC_FLUSH_RDRAM,   /* se reescribió la RDRAM entera (reset, carga) */
	SALSA64_REC_FLUSH_IDLE,    /* cambió la detección de bucles de espera (reset del CPU) */
	SALSA64_REC_FLUSH_ROM,     /* cambió la ROM o el ISViewer */
	SALSA64_REC_FLUSH_STATE,   /* se cargó un estado guardado */
	SALSA64_REC_FLUSH_OTHER,   /* salsa64_rec_flush sin causa (pruebas, opciones) */
	SALSA64_REC_FLUSH_N
};

struct salsa64_rec_link {
	uint32_t site;       /* palabra parcheada (un `b` al bloque; antes, un nop) */
	uint32_t next;       /* índice + 1 del siguiente, 0 = fin */
	uint32_t gen;        /* seg_gen[] del tramo del sitio al enlazar: si cambió, el sitio es de otro bloque */
	uint32_t pad;
};

struct salsa64_rec_stats {
	uint64_t compiled;       /* bloques compilados (sin los temporales) */
	uint64_t temp_compiled;  /* bloques recortados del modo estricto */
	uint64_t interp_only;    /* direcciones que se quedaron en el intérprete */
	uint64_t entries;        /* entradas desde C al código generado */
	uint64_t linked;         /* enlaces parcheados */
	uint64_t invalidated;    /* bloques invalidados */
	uint64_t flushes;        /* vaciados completos de la caché */
	uint64_t evictions;      /* tramos desalojados (en vez de vaciar todo) */
	uint64_t interp_steps;   /* instrucciones que corrió el intérprete */
	uint64_t exec_calls;     /* instrucciones que el código generado le pasó al intérprete */
	uint64_t compiled_x;     /* de compiled, los de KSEG1 o mapeados (rec_tlb.c) */
	uint64_t rekeyed;        /* bloques mapeados enganchados en otra física con los mismos bytes (rec_tlb.c) */
	uint64_t detached;       /* bloques mapeados desprendidos al reescribirse su página (rec_tlb.c) */
	uint64_t vt_probes;      /* traducciones con la TLB del CPU (rec_tlb.c) */
	uint64_t compile_us;     /* reservado para quien mida el tiempo de compilación */
	uint64_t fr_dep;         /* de compiled, los que dependen de Status.FR */
	uint64_t fr_twins;       /* de fr_dep, los compilados como gemelo de uno para el otro FR */
	uint64_t fr_exits;       /* salidas en la entrada porque cambió Status.FR (exit_fr) */
	uint64_t cu1_exits;      /* de las salidas por exit_fr, las que eran COP1 inusable (CU1 = 0) */
	uint64_t cu1_cuts;       /* bloques con COP1 recompilados cortados porque se entró con CU1 = 0 */
	uint64_t flushes_by[SALSA64_REC_FLUSH_N];  /* de flushes, por causa */
	uint64_t hot_placed;     /* de compiled, los compilados en la zona caliente */
	uint64_t hot_full;       /* calientes que fueron a la zona fría porque la caliente estaba llena */
	uint64_t hot_evictions;  /* mitades de la zona caliente desalojadas */
	uint64_t prof_folded;    /* entradas a bloques sumadas al perfil (rec->hits[] → entries) */
	uint64_t fm_sites;       /* fastmem: accesos compilados sin prueba */
	uint64_t fm_faults;      /* fastmem: accesos parchados tras fallar */
	uint64_t fm_off;         /* fastmem apagado (TLB que choca con la ventana, tabla de sitios llena) */
	uint64_t grp_heads;      /* base en caché: grupos compilados */
	uint64_t grp_members;    /* base en caché: accesos de esos grupos (cabezas incluidas) */
	uint64_t reclaimed;      /* bloques desprendidos matados para recuperar ids en vez de vaciar (rec_tlb.c) */
	/* De invalidated, los que mató una escritura del CPU (vr4300_code_check);
	 * el resto son de DMA, el RDP, los trucos o el frontend. La zona escrita
	 * que mató bloques, por clase (lo > hi: ninguna), desde que quien lee
	 * las estadísticas la vuelve a vaciar (salsa64_rec_inval_reset): para
	 * avisar quién pisó el código en una invalidación masiva. */
	uint64_t inval_cpu;
	uint32_t inval_cpu_lo, inval_cpu_hi, inval_other_lo, inval_other_hi;
};

/* Etiquetas y correcciones del ensamblador de rec_emit.c. */
struct salsa64_rec_label {
	uint32_t pos;        /* palabra dentro de su sección, o destino absoluto */
	uint8_t sec;         /* 0 caliente, 1 fría, 2 absoluta (palabra de la caché), 3 sin fijar */
};

/* Las rutinas compartidas de la FPU (rec_emit_fpurtn.h): índices de
 * rec->fpu_rtn[]. ADD/SUB/MUL/DIV en S y en D (la funct es el índice bajo),
 * C.cond, ABS/NEG, SQRT, CVT.D.S y CVT.S.D, CVT.S.W y CVT.D.W, y las
 * conversiones a entero (ROUND/TRUNC/CEIL/FLOOR a W y L, CVT.W, CVT.L),
 * diez por formato. */
enum {
	SALSA64_FPR_ADD_S, SALSA64_FPR_SUB_S, SALSA64_FPR_MUL_S, SALSA64_FPR_DIV_S,
	SALSA64_FPR_ADD_D, SALSA64_FPR_SUB_D, SALSA64_FPR_MUL_D, SALSA64_FPR_DIV_D,
	SALSA64_FPR_CMP_S, SALSA64_FPR_CMP_D,
	SALSA64_FPR_ABSNEG_S, SALSA64_FPR_ABSNEG_D,
	SALSA64_FPR_SQRT_S, SALSA64_FPR_SQRT_D,
	SALSA64_FPR_CVT_D_S, SALSA64_FPR_CVT_S_D,
	SALSA64_FPR_CVT_S_W, SALSA64_FPR_CVT_D_W,
	SALSA64_FPR_TOINT,
	SALSA64_FPR_N = SALSA64_FPR_TOINT + 20
};

struct salsa64_rec_fixup {
	uint32_t at;         /* palabra que se corrige */
	uint16_t label;
	uint8_t sec;         /* sección de la palabra */
	uint8_t cond;        /* 1 = bc (14 bits), 0 = b/bl (24 bits) */
};

/* fastmem: un acceso caliente parcheable y su camino frío (palabras de la
 * caché), con la generación del tramo del sitio (seg_gen[], o scratch_gen
 * en la zona temporal) para descartar los de bloques desalojados. */
struct salsa64_rec_fmsite {
	uint32_t site;       /* 0 = vacío (la palabra 0 es del preámbulo) */
	uint32_t cold;
	uint32_t gen;
};
#define SALSA64_REC_FM_SITES_LOG 18
#define SALSA64_REC_FM_SITES     (1u << SALSA64_REC_FM_SITES_LOG)
#define SALSA64_REC_FM_PROBES    64

/* Opciones: se ponen en rec->opts antes de salsa64_rec_init, que las
 * conserva. */
#define SALSA64_REC_OPT_PIN   1u  /* sp, ra, v0, v1, a0–a3 fijos en r18–r25 entre bloques */
#define SALSA64_REC_OPT_EAGER 2u  /* escribir cada registro poco después de su último valor */
/* Para medir: conversiones y SQRT de la FPU, CFC1/CTC1, LWL/LWR y compañía
 * por el intérprete y MFC0 terminando el bloque, como antes de traducirlos
 * (docs/perf-log.md). Se aplica al compilar: cambiarla pide vaciar la caché. */
#define SALSA64_REC_OPT_RARE_EXEC 4u
/* Para medir A/B en la PS3 con el mismo binario: lo de antes del 2026-10-10
 * (noche). COP0 y ERET vuelven al despachador y el intérprete corre el resto
 * del bloque básico, CACHE al intérprete, sin camino rápido de datos
 * mapeados (ktlb solo para código), x_sync vuelve a traducir todas las
 * páginas anotadas y no prellena, exit_jr mira solo la cabeza de xhash. Se
 * aplica al compilar y en el despachador: fijarla antes de salsa64_rec_init. */
#define SALSA64_REC_OPT_LEGACY 8u
/* Experimental: la FPU con registros residentes y FCR31 perezoso
 * (rec_emit_fpu.h) en vez de cada operación por cpu->fpr (rec_emit.c,
 * emit_fpu). Medido en la PS3 el 2026-10-11 es más lenta (bench add.s
 * 40,0 → 30,9 MIPS, mixto 168 → 135): apagada por omisión
 * (docs/perf-log.md). Se aplica al compilar. */
#define SALSA64_REC_OPT_FPU_RES 16u
/* Para medir A/B: COP0 como hasta el 2026-10-11, todo por el intérprete
 * (MTC0 de EntryLo/PageMask/Index, TLBWI/TLBWR y ERET tienen ahora camino
 * propio: rec_emit_ext.h y rec_h_tlbw/rec_h_eret en rec.c). Se aplica al
 * compilar. */
#define SALSA64_REC_OPT_COP0_EXEC 32u
/* Para medir A/B: la caché llena se vacía entera, como hasta el 2026-10-11,
 * en vez de desalojar el tramo más viejo. Se aplica en el despachador. */
#define SALSA64_REC_OPT_NO_EVICT 64u
/* Para medir A/B con el mismo binario: el código «ancho» de antes del
 * 2026-10-13 (docs/rec-ppe.md, «Código compacto»): la prueba de KSEG0 en 7
 * palabras, el camino lento de cada acceso con sus argumentos y su escritura
 * de registros propios, y las llamadas al intérprete en línea. Se aplica al
 * compilar. */
#define SALSA64_REC_OPT_WIDE 128u
/* fastmem (docs/fastmem.md): la RDRAM vive al principio de una ventana de
 * 512 MB (rec->fm_window, la pone quien llama antes de salsa64_rec_init) y
 * los accesos de KSEG0/KSEG1 del código generado van sin prueba de segmento:
 * índice va & 0x1FFFFFFF sobre r30. Lo que no es RDRAM cae en una página sin
 * mapear y el acceso falla; quien atiende el fallo (señal en el host, hilo
 * ayudante en la PS3) llama a salsa64_rec_fastmem_fault con el pc, que
 * parchea el acceso hacia su camino frío (la prueba de siempre). Se aplica
 * al compilar; una entrada de la TLB que choque con la ventana lo apaga. */
#define SALSA64_REC_OPT_FASTMEM 256u
/* Para medir A/B: sin la base en caché (docs/fastmem.md §8): cada acceso con
 * su prueba, como hasta el 2026-10-13. Se aplica al compilar. */
#define SALSA64_REC_OPT_NO_GROUP 512u
/* Para medir A/B: la FPU en línea en cada sitio, como hasta el 2026-10-13,
 * en vez de las rutinas compartidas del preámbulo (rec_emit_fpurtn.h). Se
 * aplica al iniciar (las rutinas no se emiten) y al compilar. */
#define SALSA64_REC_OPT_FPU_INLINE 1024u
/* Caché de destinos de JR/JALR (docs/perf-log.md, «Caché de destinos de JR»): una
 * tabla directa de 1024 entradas, vpc → palabra de entrada del bloque, que
 * exit_jr consulta antes de recorrer map/xhash/blocks (16 palabras en vez de
 * ~36 y tres cargas dependientes menos). La llena exit_jr al encontrar el
 * bloque por el camino largo; se vacía la entrada al matar o desprender el
 * bloque y cuando el despachador resuelve ese pc a otro código. Se aplica al
 * compilar (exit_jr se emite en el preámbulo). En la PS3 y en el host va
 * encendida por omisión; el frontend la apaga con el archivo jit-sin-destinos y
 * el host con SALSA64_REC_NO_JTC=1. */
#define SALSA64_REC_OPT_JTC 2048u
/* Con la caché de destinos: cuenta aciertos y fallos en jtc_ctr[] (suma unas
 * pocas palabras al camino del acierto). */
#define SALSA64_REC_OPT_JTC_STATS 4096u
/* Depuración (host, SALSA64_REC_OPTS): apagan una pieza nueva a la vez para
 * acotar una diferencia en --lockstep. LEGACY las apaga todas. */
#define SALSA64_REC_DBG_NO_ENDLINK 0x100u   /* COP0 vuelve al despachador e interpreta el resto del bloque básico */
#define SALSA64_REC_DBG_NO_CACHE   0x200u   /* CACHE al intérprete */
#define SALSA64_REC_DBG_NO_CHAIN   0x400u   /* exit_jr mira solo la cabeza de xhash */
#define SALSA64_REC_DBG_NO_TSYNC   0x800u   /* x_sync vuelve a traducir todo y no prellena */
#define SALSA64_REC_DBG_NO_DATA    0x1000u  /* sin camino rápido de datos mapeados ni anotarlos */
#define SALSA64_REC_DBG_ERET_BOUND 0x2000u  /* tras un ERET, el intérprete hasta el fin del bloque básico */
#define SALSA64_REC_DBG_NO_DETACH  0x4000u  /* invalidar mata el bloque (sin desprender ni enganchar) */
#define SALSA64_REC_DBG_NO_SUPER   0x8000u  /* un salto condicional hacia adelante termina el bloque */
#define SALSA64_REC_DBG_NO_IJUMP   0x10000u /* un salto interno con destino dentro del bloque sale enlazado */
#define SALSA64_REC_DBG_NO_CU1_CUT 0x20000u /* con CU1 = 0, un bloque con COP1 no se corta: sale en la entrada */
#define SALSA64_REC_DBG_NO_COPY_FREE 0x100000u /* copias de bytes de los bloques mapeados en reparto lineal de 1 MB sin
					       * liberar, como hasta 2026-10-28 (rec_tlb.h; jit-copias-viejas) */
#define SALSA64_REC_OLD(rec, bit)  ((rec)->opts & (SALSA64_REC_OPT_LEGACY | (bit)))

#define SALSA64_REC_JTC_SLOTS 1024
struct salsa64_rec_jtc {
	uint32_t vpc;        /* 32 bits bajos del destino (alineado); 1 = vacía */
	uint32_t code;       /* palabra de entrada del bloque en la caché */
};

/* Quitar la consulta de espera recompilando e invalidando rangos, para A/B. */
#define SALSA64_REC_OPT_IDLE_RECOMPILE 0x40000u
/* Huellas y tablas de diagnóstico, reservadas solo al iniciar si se piden. */
#define SALSA64_REC_OPT_DIAG 0x80000u

struct salsa64_rec {
	struct salsa64_rec_diag *diag;
	uint32_t idle_patch[SALSA64_REC_MAX_BLOCKS + 1];
	uint32_t compiled_idle_patch;
	uint32_t opts;
	/* Bytes de la zona caliente (las dos mitades juntas); 0 = sin zona. Se
	 * pone antes de salsa64_rec_init, que lo conserva y lo recorta a lo que
	 * cabe. Solo sirve con un perfil (rec->profile). */
	uint32_t hot_bytes;
	struct salsa64_cpu *cpu;
	uint32_t *code;          /* caché de código */
	uint32_t code_words;     /* capacidad, en palabras */
	uint32_t used;           /* palabras ocupadas (asignación lineal dentro del tramo en uso) */
	uint32_t scratch;        /* inicio de la zona de bloques temporales */
	uint32_t generation;     /* sube en cada vaciado completo y en cada desalojo */
	uint32_t seg_words;      /* palabras por tramo */
	uint32_t seg_end;        /* fin del tramo en uso */
	uint32_t cur_seg;        /* tramo en uso: SALSA64_REC_HOT_SEGMENTS + k */
	uint32_t hot_words;      /* palabras por mitad de la zona caliente (0 = sin zona) */
	uint32_t used_hot;       /* palabras ocupadas de la mitad caliente en uso */
	uint32_t hot_end;        /* fin de esa mitad */
	uint32_t cur_hot;        /* mitad en uso: 0 o 1 */
	bool hot_full;           /* un caliente no cupo: desalojar la otra mitad antes del próximo bloque */
	bool placed_hot;         /* el último bloque compilado fue a la zona caliente (lo deja rec_emit.c) */
	uint32_t seg_gen[SALSA64_REC_ALL_SEGMENTS];  /* sube al desalojar el tramo (enlaces con sitio en él) */
	uint64_t enter_opd[3];   /* descriptor ELFv1 de la entrada al código generado */
	uint32_t exit_link, exit_mid, exit_jump;  /* rutinas de salida (palabra en la caché) */
	uint32_t exit_jr;        /* JR/JALR: busca el bloque destino y salta, o sale como exit_jump */
	uint32_t exit_idle;      /* salto que el intérprete consultaría como bucle de espera */
	uint32_t exit_start;     /* el bloque salió en la entrada (no cabe antes del evento, COP1 inusable) */
	uint32_t exit_fr;        /* el bloque salió en la entrada: Status.FR no es el de su compilación (o CU1 = 0) */
	uint32_t exit_end;       /* el bloque terminó en una instrucción que vuelve a C (COP0, SYSCALL…) */
	uint32_t ktlb_rtn[2][4]; /* traducción de datos por ktlb[] (rec_tlb.h): [almacena][alineación 0, 1, 3, 7];
				  * r3 = va; vuelve con cr1 eq y r12 = desplazamiento en la RDRAM, o cr1 ne */
	uint32_t trace_rtn;      /* perfilado (rec_trace.h): r0 = id del bloque a la ventana; solo con rec->trace al iniciar */
	uint32_t exit_jr_count;  /* perfilado: r3 = destino, r4 = id; cuenta el JR y sigue en exit_jr */
	uint32_t slow_rtn;       /* camino lento compartido: cargas, almacenamientos, una instrucción al intérprete y
				  * excepciones, según r6 (rec_emit.c, emit_slow_call) */
	uint32_t fpu_rtn[SALSA64_FPR_N];  /* la FPU compacta (rec_emit_fpurtn.h): palabra de cada rutina */
	uint32_t fpu_rtn_words;  /* lo que ocupan en el preámbulo */
	bool fpu_rtn_ok;         /* están: los sitios de la FPU las llaman en vez de emitir en línea */

	/* Direcciones de entrada de las funciones de C que llama el código. */
	uint64_t h_load, h_store, h_exec, h_exception, h_tlbw, h_eret;
	bool calls_near;         /* todas a menos de 32 MB del preámbulo: `bl` directo (cada bloque lo decide para sí) */

	uint32_t rdram_size;
	unsigned rdram_shift;    /* log2(rdram_size) */
	uint32_t fr;             /* Status.FR con que se compila el bloque en curso (rec_emit.c lo lee) */
	bool compiled_fr_dep;    /* el último bloque compilado depende de Status.FR (lo deja rec_emit.c) */
	bool strict;             /* pruebas: nunca interpreta lo que se puede recompilar */
	bool no_link;            /* sin enlaces: cada bloque (o bucle de un bloque) vuelve a C */
	bool map_dirty;

	unsigned cur_id;         /* id del bloque que se compila (0 en los temporales): perfil por bloque */
	uint16_t free_block;     /* lista de ids libres */
	uint32_t next_block;     /* siguiente id nunca usado */
	uint32_t free_link;      /* lista de entradas libres de links[] (índice + 1) */
	uint32_t next_link;

	/* Depuración: si no es NULL, se llama después de cada salida del código
	 * generado (modo traza, ver src/host/run.c). */
	void (*after_exit)(struct salsa64_rec *rec, uint64_t block_pc, void *ctx);
	void *after_exit_ctx;
	/* Depuración: cada bloque compilado (palabra de entrada y tamaño). */
	void (*on_compile)(struct salsa64_rec *rec, uint64_t pc, uint32_t entry, uint32_t words, bool temp);
	/* Perfil por causa (rec_prof.h); NULL = apagado. Lo pone quien llama,
	 * antes o después de salsa64_rec_init (que lo conserva). */
	struct salsa64_rec_prof *prof;
	/* Perfil persistente por juego (rec_profile.h); NULL = apagado. Lo pone
	 * quien llama después de salsa64_rec_init y con la tabla ya cargada y
	 * marcada; con él, cada bloque cuenta sus entradas en hits[] y los
	 * marcados van a la zona caliente. */
	struct salsa64_rec_profile *profile;
	/* Con el perfil: registrar los sitios de salida de los bloques calientes
	 * (cuatro palabras en cada camino que sale; rec_profile.h). Lo deja en
	 * true salsa64_rec_init; quien llama lo apaga después si quiere. */
	bool profile_sites;
	/* Muestreo por tiempo (rec_sample.h); NULL = apagado. Lo pone quien
	 * llama después de salsa64_rec_init; con él, la entrada de cada bloque
	 * deja su id en cpu->rec_cur. */
	struct salsa64_rec_sample *sample;
	/* Registro para el perfilado (rec_trace.h); NULL = apagado. Lo pone quien
	 * llama antes de salsa64_rec_init (que lo conserva y emite en el
	 * preámbulo lo que hace falta); sus opciones valen para los bloques que
	 * se compilen después. */
	struct salsa64_rec_trace *trace;

	struct salsa64_rec_stats stats;

#ifdef SALSA64_REC_LAB
	/* Laboratorio (tools/lab, docs/lab-recompilador.md): dónde empieza en cada
	 * sección el código de la instrucción MIPS k del último bloque compilado
	 * (0xFFFF: no empieza ninguna ahí; k = n: el final del cuerpo, antes del
	 * código frío fijo de entrada y salidas) y cómo se traduce (nativa, por el intérprete…). */
	struct {
		uint16_t hot[SALSA64_REC_MAX_INSNS + 1], cold[SALSA64_REC_MAX_INSNS + 1];
		uint8_t how[SALSA64_REC_MAX_INSNS + 1];  /* cómo se traduce la instrucción (enum how de rec_emit.c), 0xFF en k = n */
		bool loop;                               /* el salto final vuelve al inicio del propio bloque (bucle) */
	} lab;
#endif

	/* fastmem (SALSA64_REC_OPT_FASTMEM, docs/fastmem.md). */
	bool fastmem;            /* activo: la RDRAM está en fm_window y los accesos se compilan sin prueba */
	void *fm_window;         /* la ventana de 512 MB (quien llama, antes de salsa64_rec_init) */
	uint32_t scratch_gen;    /* sube con cada bloque temporal: generación de sus sitios */
	unsigned fm_npend, fm_nnew;
	uint16_t fm_pend_site[SALSA64_REC_MAX_INSNS], fm_pend_cold[SALSA64_REC_MAX_INSNS];  /* etiquetas (rec_emit.c) */
	uint32_t fm_new_site[SALSA64_REC_MAX_INSNS], fm_new_cold[SALSA64_REC_MAX_INSNS];    /* palabras (finalize) */

	/* Compilación (rec_emit.c). */
	uint32_t buf[2][SALSA64_REC_BUF_WORDS];
	uint32_t nbuf[2];
	struct salsa64_rec_label labels[SALSA64_REC_MAX_LABELS];
	struct salsa64_rec_fixup fixups[SALSA64_REC_MAX_FIXUPS];
	uint32_t nlabels, nfixups;
	bool overflow;

	uint16_t map[SALSA64_REC_MAP_WORDS];          /* bloque que empieza en cada palabra */
	struct salsa64_rec_x x;                       /* fuera de KSEG0 (rec_tlb.h): justo antes de flags */
	uint8_t flags[SALSA64_REC_FLAG_BYTES];        /* bloques que tocan cada 128 bytes (satura en 255) */
	/* Entradas a cada bloque desde que se sumaron al perfil (rec_profile.h):
	 * el código generado incrementa hits[id] al entrar (a desplazamiento fijo
	 * de flags, que vive en r28). pent[id] es la entrada del perfil + 1. */
	uint32_t hits[SALSA64_REC_MAX_BLOCKS + 1];
	uint16_t pent[SALSA64_REC_MAX_BLOCKS + 1];
	/* Caché de destinos de JR (SALSA64_REC_OPT_JTC): el código generado la lee desde flags (r28)
	 * con un desplazamiento fijo. jtc_slot[id] es la entrada que apunta al bloque id (para
	 * vaciarla al matarlo); jtc_ctr: aciertos y fallos (JTC_STATS). */
	struct salsa64_rec_jtc jtc[SALSA64_REC_JTC_SLOTS];
	uint16_t jtc_slot[SALSA64_REC_MAX_BLOCKS + 1];
	uint32_t jtc_ctr[2];
	struct salsa64_rec_block blocks[SALSA64_REC_MAX_BLOCKS + 1];
	uint16_t twin[SALSA64_REC_MAX_BLOCKS + 1];    /* gemelo de un bloque de KSEG0 que depende de FR, para el otro FR */
	struct salsa64_rec_link links[SALSA64_REC_MAX_LINKS];
	struct salsa64_rec_fmsite fm_sites[SALSA64_REC_FM_SITES];  /* fastmem: sitio → camino frío (3 MB) */
};

/*
 * Prepara el recompilador sobre `cpu` con una caché de código de `bytes`
 * (alineada a 128 bytes, escribible y ejecutable) y lo activa
 * (cpu->rec). Devuelve false si el host no puede ejecutar el código que
 * genera (no es ppc64 big-endian) o la caché es muy chica; entonces el CPU
 * sigue con el intérprete.
 */
bool salsa64_rec_init(struct salsa64_rec *rec, struct salsa64_cpu *cpu, void *code, size_t bytes);
/* Vuelve al intérprete. */
void salsa64_rec_detach(struct salsa64_rec *rec);
/* fastmem: un acceso del código generado falló en `nip`. Si es un sitio
 * conocido, lo parchea hacia su camino frío (quien llama reanuda el hilo,
 * que repite la instrucción) y devuelve true; si no, false (el fallo es de
 * otra cosa). Sin reservar memoria ni llamar fuera: sirve desde un manejador
 * de señal o desde otro hilo con el que falló suspendido. */
bool salsa64_rec_fastmem_fault(struct salsa64_rec *rec, uintptr_t nip);
/* fastmem: ¿una entrada de la TLB mapea direcciones que, dobladas a la
 * ventana (va & 0x1FFFFFFF), caerían en la RDRAM? Solo kuseg y kseg2/3
 * con direcciones de 32 bits: KSEG0/KSEG1 no pasan por la TLB. */
bool salsa64_rec_fastmem_collides(const struct salsa64_rec *rec, const struct salsa64_tlb_entry *e);
/* fastmem: apagarlo (vacía la caché; los bloques se recompilan con la prueba). */
void salsa64_rec_fastmem_off(struct salsa64_rec *rec);
/* Igual que salsa64_cpu_run, con el recompilador. */
void salsa64_rec_run(struct salsa64_rec *rec);
/* La RDRAM [pa, pa + len) cambió fuera del CPU (DMA, carga de la ROM,
 * pruebas): descarta el código que dependa de ella. */
void salsa64_rec_invalidate(struct salsa64_rec *rec, uint32_t pa, uint32_t len);
/* Vacía las zonas de inval_cpu_lo/hi e inval_other_lo/hi. */
void salsa64_rec_inval_reset(struct salsa64_rec *rec);
/* Descarta todo el código generado (y cuenta por qué). */
void salsa64_rec_flush_why(struct salsa64_rec *rec, enum salsa64_rec_flush_cause why);
static inline void salsa64_rec_flush(struct salsa64_rec *rec) { salsa64_rec_flush_why(rec, SALSA64_REC_FLUSH_OTHER); }
/* Palabras de la caché ocupadas (preámbulo, la zona caliente hasta donde va y
 * la fría hasta donde va). */
uint32_t salsa64_rec_used(const struct salsa64_rec *rec);
/* Palabras de la zona caliente (las dos mitades) y las ocupadas en la mitad
 * en uso. */
uint32_t salsa64_rec_hot_capacity(const struct salsa64_rec *rec);
uint32_t salsa64_rec_hot_used(const struct salsa64_rec *rec);
/* Suma al perfil (rec->profile) las entradas contadas en hits[] de todos los
 * bloques vivos y las deja en 0. Fuera del bucle de emulación (entre campos),
 * desde el hilo que emula. Sin perfil no hace nada. */
void salsa64_rec_profile_fold(struct salsa64_rec *rec);
/* Suma lo pendiente y suelta el perfil (rec->profile = NULL). */
void salsa64_rec_profile_detach(struct salsa64_rec *rec);

/* ---- Interno: compartido por rec.c y rec_emit.c ---------------------------- */

/* Cómo volvió el código generado a C (cpu->rec_kind). */
enum {
	SALSA64_REC_EXIT_LINK = 1,  /* a una dirección fija; cpu->rec_site es el enlace a parchear */
	SALSA64_REC_EXIT_MID = 2,   /* a mitad de bloque (evento, interrupción, código invalidado) */
	SALSA64_REC_EXIT_JUMP = 3,  /* a un destino calculado (JR/JALR) o sin enlace */
	SALSA64_REC_EXIT_IDLE = 4,  /* r3 = pc de un salto tomado, antes de su delay slot (idle.c) */
	SALSA64_REC_EXIT_START = 5, /* como EXIT_MID, en la entrada del bloque: no cabe antes del evento o COP1 inusable */
	SALSA64_REC_EXIT_END = 6,   /* el bloque terminó completo en una instrucción de K_END; sigue en r3 */
	SALSA64_REC_EXIT_FR = 7,    /* en la entrada: Status.FR no es el de la compilación, o CU1 = 0 */
};

/* Tipos de acceso del camino lento (salsa64_rec_h_load/h_store). */
enum { SALSA64_REC_LB, SALSA64_REC_LBU, SALSA64_REC_LH, SALSA64_REC_LHU,
       SALSA64_REC_LW, SALSA64_REC_LWU, SALSA64_REC_LD };
enum { SALSA64_REC_SB, SALSA64_REC_SH, SALSA64_REC_SW, SALSA64_REC_SD };

/* Con SALSA64_REC_OPT_PIN: el registro MIPS salsa64_rec_pin[k] vive en
 * r(SALSA64_REC_PIN_PPC + k) mientras corre el código generado; la entrada lo
 * carga y la salida a C lo escribe. Son los de la convención de llamadas:
 * pasan de un bloque a otro en cada llamada y cada retorno. */
#define SALSA64_REC_PIN_N   8
#define SALSA64_REC_PIN_PPC 18
static const uint8_t salsa64_rec_pin[SALSA64_REC_PIN_N] __attribute__((unused)) = { 29, 31, 2, 3, 4, 5, 6, 7 };

/* rec.c (interno): deja visible para el PPE el código recién escrito. */
void salsa64_rec_sync(const void *p, size_t bytes);
/* rec.c (interno): ¿un bloque que empieza en la palabra `base` llama a las
 * funciones de C con `bl` (a menos de 32 MB)? Si no, por mtctr/bctrl. */
bool salsa64_rec_calls_near_at(const struct salsa64_rec *rec, uint32_t base);

/* Constantes de la FPU residente (rec_emit_fpu.h): la entrada al código
 * generado las deja en f26–f31, en este orden: 1,0; 0,0; y los bordes de la
 * franja segura de fpu_fast_band en S (2^-60, 2^61) y en D (2^-440, 2^441). */
extern const double salsa64_rec_fconst[6];

/* rec_emit.c (interno): compila el bloque en `pc` (física `pa`, segmento
 * `seg` de rec_tlb.h) de hasta `max` instrucciones. Devuelve la palabra de
 * entrada en la caché, o 0 si no se pudo; `n` recibe las instrucciones del
 * camino más largo y `bytes` las que cubre. */
/* La FPU compacta: emite las rutinas compartidas a partir de la palabra
 * `base` (el final del preámbulo) sin pasar de `limit`; false si no caben. */
bool salsa64_rec_emit_fpu_rtns(struct salsa64_rec *rec, uint32_t base, uint32_t limit);
uint32_t salsa64_rec_compile(struct salsa64_rec *rec, uint64_t pc, uint32_t pa, unsigned seg, unsigned max,
			     bool temp, unsigned *n, uint32_t *bytes);

#endif
