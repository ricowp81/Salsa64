/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — RDP sobre el RSX. Ver rdp.h.
 *
 * Hilos (docs/hilos-ppe.md): el sink (acquire y done) corre en el hilo de
 * emulación; todo lo demás, en el de presentación. Las tareas terminadas
 * esperan en una cola de SETS entradas hasta que salsa64_rdp_flush las
 * traduce. Un juego de búferes queda libre cuando se tradujo (contador
 * `translated`) y el RSX pasó su barrera (`fence`, que el hilo de
 * emulación consulta con rsxkit_fence_done: solo lee una etiqueta).
 *
 * Destinos de render: la tabla (qué color image tiene superficie, cuál se
 * recicla, cuál se presenta) es targets.c, portable y con pruebas en el
 * host; aquí solo viven las superficies de rsxkit, una por ranura.
 *
 * Caché de texturas (gfx.h): otra región mapeada que administra la HLE.
 * La tarea n lleva el número n + 1 y, como done_task, la última tarea
 * cuya barrera ya pasó, nunca la última traducida (salsa64_rdp_dump lee
 * sus texturas).
 */
#include "rdp/rdp.h"
#include "rdp/layer8.h"
#include "rdp/mask32.h"

static inline uint64_t tb_now(void);  /* abajo; la usa el reloj de layer8 */

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/systime.h>

#include "rdp/gfxdump.h"
#include "rdp/targets.h"
#include "rdp/vmatch.h"
#include "rsp/gfx/gfx.h"
#include "rsp/gfx/hle.h"

#define SETS        3
#define VTX_BYTES   (2u << 20)
/* gfx_draw son 496 bytes: 2600 dibujos por tarea. El arena de texturas
 * casi no se usa con la caché (docs/memory-budget.md): le cede 256 KB, los
 * de CMD_INTERP y el aumento de los comandos de 768 a 1280 KB (el jefe final
 * de Conker dibuja ~1600 estrellas de un píxel, una por dibujo porque cada
 * una trae su color primario, antes de su escenario; con 768 KB la lista se
 * cortaba en la última estrella y salía solo el fondo negro). */
#define CMD_BYTES   (1280u << 10)
/* Con la interpolación la HLE pone también sus registros GFX_OP_VGROUP, en
 * hasta un tercio del búfer (gfx.h): ese tercio va de más (640 KB), y los
 * dibujos tienen el mismo lugar que sin ella (con 1280 KB para los dos, una
 * tarea pesada se cortaba y el juego recibía antes la interrupción del DP). */
#define CMD_INTERP  (CMD_BYTES / (GFX_VGROUP_SHARE - 1))
#define CMD_SET     (CMD_BYTES + CMD_INTERP)
#define TEX_BYTES   ((4u << 20) - (256u << 10) - CMD_INTERP - (CMD_BYTES - (768u << 10)))
#define SET_BYTES   (VTX_BYTES + CMD_SET + TEX_BYTES)
#define SETS_BYTES  (((SETS * SET_BYTES) + 0xFFFFFu) & ~0xFFFFFu)
#define CACHE_BYTES (8u << 20)  /* caché de texturas (docs/memory-budget.md) */
/* Efectos de framebuffer (docs/framebuffer.md): dos búferes para las copias
 * RSX → RDRAM (RGB565 a la resolución de la N64, hasta 640 × 480) y uno
 * para cebar una superficie con lo que el CPU dejó en la RDRAM (RGBA5551). */
#define RB_MAX_W    640
#define RB_MAX_H    480
#define RB_PITCH    ((RB_MAX_W * 2 + 63) & ~63u)
#define RB_SLOT     (RB_PITCH * RB_MAX_H)
#define RB_SLOTS    2
/* Copias espejo de las imágenes auxiliares chicas (iconos, pantallas), sin
 * esperar: MIRROR_BYTES es lo que cuenta como imagen chica (hasta 128 × 256 a
 * 16 bits) y MIRROR_SLOT el tamaño de cada búfer, que además guarda las
 * imágenes de una ranura que se recicla (el aviso de 538 × 100 de «Game Pak
 * Check» de Pokémon Stadium: 106 KB con el paso a 64 bytes). */
#define MIRROR_BYTES 65536u
#define MIRROR_SLOT  131072u
#define MIRRORS      4
/* Espejo del framebuffer presentado, cada campo, sin esperar (Mario Kart
 * reduce el cuadro con el CPU para la pantalla del túnel): un búfer grande
 * más, después de los RB_SLOTS y el de cebado. */
#define FRAME_SLOT  (RB_SLOTS + 1)
/* Búferes de cebado (prime): el RSX lee el cebado después, cuando llega a su
 * blit (rsxkit_target_fill solo lo encola), así que cada cebado va a un búfer
 * distinto y no se reescribe hasta que el RSX pasó la barrera del anterior.
 * Con uno solo, el cebado siguiente (Resident Evil 2: el de la imagen de
 * 189a00, con la limpieza de su z image, tres tareas después del de 63e000)
 * pisaba el búfer mientras el RSX aún no leía el anterior y esos datos salían
 * en el cuadro presentado, en triángulos y bloques de 128 bytes. El primero
 * es el de siempre (después de los RB_SLOTS); el resto, tras los espejos. */
#define PRIME_BUFS  4
#define PRIME_EXTRA ((PRIME_BUFS - 1) * RB_SLOT)
/* La z image que el CPU deja en la RDRAM, convertida a Z24S8 para cargarla
 * en la profundidad del RSX (zfeed): hasta 640 × 480, detrás de lo anterior. */
#define ZUP_PITCH   (RB_MAX_W * 4)
#define ZUP_BYTES   (ZUP_PITCH * RB_MAX_H)
#define ZUP_OFF     ((RB_SLOTS + 2) * RB_SLOT + MIRRORS * MIRROR_SLOT + PRIME_EXTRA)
#define FB_BYTES    ((ZUP_OFF + ZUP_BYTES + 0xFFFFFu) & ~0xFFFFFu)
#define POOL_BYTES  (SETS_BYTES + CACHE_BYTES + FB_BYTES)
#define MAX_DEPTHS  RDP_DEPTHS_MAX  /* targets.h: cuentan en rdp_vram_worst */
#define RB_PER_TASK 8   /* color images que una tarea puede pedir copiar */
/* Una imagen pedida también en la tarea anterior ya no se espera (iconos
 * de cada cuadro, la intro de Conker); con una ventana mayor, Link
 * redibujado al cambiar de equipo en Zelda caía como «repetida» y el CPU
 * lo copiaba antes de tiempo. */
#define RB_HISTORY  1
/* Tareas durante las que el espejo del framebuffer respeta la zona de una
 * imagen auxiliar recién copiada que se solapa con él (Link se dibuja en
 * los primeros 14 KB del framebuffer de Zelda y el CPU lo copia de ahí; el
 * filtro de la pausa lee la cobertura ~1 s). */
#define RB_KEEP_TASKS 32
#define RB_WAIT_US  200000  /* la emulación no espera una copia más que esto */
/* Interpolación por geometría (salsa64_rdp_interp, docs/interpolacion.md):
 * detrás de los vértices de una tarea con GFX_JOB_INTERP, desde VTX_INTERP
 * de su juego de búferes, van sus posiciones en la tarea anterior (un
 * float[4] por vértice, en el mismo orden). La HLE tiene todo el búfer, como
 * sin la interpolación: una tarea con más vértices que VTX_INTERP no se
 * interpola (antes se cortaba). Las tablas del emparejamiento
 * (vmatch.c) son dos, de VM_GROUPS grupos y VM_CLIP vértices (Zelda OoT usa
 * ~360 y ~2900 por cuadro; GoldenEye ~250 y ~3400); un cuadro que no cabe no
 * se interpola. */
#define VTX_INTERP  ((VTX_BYTES / 3 * 2) & ~31u)
#define VM_GROUPS   2048
#define VM_CLIP     32768
#define INTERP_DISP 120.0f  /* corte (vmatch_pair): píxeles de la N64 por cuadro del juego */
_Static_assert((VTX_BYTES - VTX_INTERP) / 16 >= VTX_INTERP / sizeof(struct gfx_vertex),
	       "una posición anterior por vértice");

_Static_assert(sizeof(struct gfx_vertex) == sizeof(rsxkit_vertex), "vértice de la HLE = vértice de rsxkit");
_Static_assert(sizeof(((struct gfx_draw *)0)->k) == sizeof(((rsxkit_draw *)0)->k), "términos k");
_Static_assert(sizeof(((struct gfx_draw *)0)->factors) == sizeof(((rsxkit_draw *)0)->factors), "factores");
_Static_assert((int)GFX_K_T0 == (int)RSXKIT_K_T0 && (int)GFX_K_T1A == (int)RSXKIT_K_T1A &&
		       (int)GFX_K_ONE == (int)RSXKIT_K_ONE, "orden de k");
_Static_assert((int)GFX_F_T0 == (int)RSXKIT_FP_T0 && (int)GFX_F_T0A == (int)RSXKIT_FP_T0A &&
		       (int)GFX_F_T1 == (int)RSXKIT_FP_T1 && (int)GFX_F_T1A == (int)RSXKIT_FP_T1A &&
		       (int)GFX_F_C == (int)RSXKIT_FP_C && (int)GFX_F_CA == (int)RSXKIT_FP_CA, "factores");
_Static_assert((int)GFX_CULL_BACK == (int)RSXKIT_CULL_BACK && (int)GFX_CULL_FRONT == (int)RSXKIT_CULL_FRONT,
	       "cull");
_Static_assert(GFX_DEPTH_TEST == RSXKIT_DEPTH_TEST && GFX_DEPTH_WRITE == RSXKIT_DEPTH_WRITE &&
		       GFX_DEPTH_DECAL == RSXKIT_DEPTH_OFFSET && GFX_DEPTH_PRIM == RSXKIT_DEPTH_CONST && GFX_DEPTH_LESS == RSXKIT_DEPTH_LESS,
	       "profundidad");
_Static_assert(GFX_FB_FRESH == RDP_TARGETS_FRESH, "la HLE y la tabla cuentan igual la antigüedad de un destino");

struct set {
	uint8_t *vtx, *cmd, *tex;
	uint32_t vtx_off, tex_off;  /* offsets del RSX */
	uint32_t fence;             /* la pone el hilo de presentación al traducir */
	volatile uint32_t fence_task;  /* número de la tarea antes de `fence` */
	struct gfx_result res;      /* la deja done; la lee flush */
	/* Color images de la tarea que hay que copiar a la RDRAM al traducirla
	 * (lo decide done, en el hilo de emulación). rb_sync: la emulación
	 * espera esa copia (la imagen acaba de aparecer: el CPU la va a leer
	 * ya, como el fondo de la pausa de Zelda); si no, se copia sin esperar
	 * (la misma imagen se pide en tareas seguidas: un efecto por cuadro
	 * recibe el cuadro anterior, como la intro de Conker). */
	uint32_t rb_addr[RB_PER_TASK], rb_width[RB_PER_TASK];
	/* Capas de 8 bits que done dibujó en la RDRAM (layer8) e imágenes
	 * chicas de 32 bits (mask32): sin copia y sin dibujos en el RSX en esta
	 * tarea. */
	uint32_t sw8_addr[LAYER8_MAX + MASK32_MAX], sw8_width[LAYER8_MAX + MASK32_MAX], sw8_n;
	uint32_t rb_rep[RB_PER_TASK];  /* tareas seguidas en que se venía pidiendo */
	bool rb_sync[RB_PER_TASK];
	uint32_t rb_n, rb_nsync;
	bool trace;  /* tarea con traza detallada en el log (ver readbacks_wanted) */
	uint32_t set_wait_us;  /* perfilado: lo que esperó acquire este juego para la tarea */
	bool interp; /* se envió con GFX_JOB_INTERP */
	bool dpc_cont;  /* tramo de una lista del RDP ya abierta (GFX_JOB_DPC_CONT): no envejece los destinos */
	/* Interpolación, al traducirla: sus posiciones de antes valen (hubo
	 * anterior y no fue un corte), su número en la tabla de destinos
	 * (rdp_targets.task: el last_task de lo que dibujó) y hasta dónde usó
	 * su arena el PPE (un redibujo decodifica detrás). */
	bool interp_ok;
	uint32_t tg_task;
	uint32_t ppe_tex_end;
};

/* Perfilado (salsa64_rdp_set_perf_hook): lo de la tarea que se traduce. */
static void (*perf_hook)(const struct salsa64_rdp_task_info *info);
static struct salsa64_rdp_task_info perf_info;

void salsa64_rdp_set_perf_hook(void (*hook)(const struct salsa64_rdp_task_info *info))
{
	perf_hook = hook;
}

/* Huella de lo que hace un combiner aproximado: ciclo, texturas, niebla,
 * términos y factores (FNV-1a). */
static void perf_approx(const struct gfx_draw *d)
{
	uint32_t h = 2166136261u;
	const uint8_t head[4] = { d->cycle, d->ntex, d->fog, d->nterms };
	for (unsigned k = 0; k < 4; k++)
		h = (h ^ head[k]) * 16777619u;
	const uint8_t *f = &d->factors[0][0];
	for (unsigned k = 0; k < sizeof d->factors; k++)
		h = (h ^ f[k]) * 16777619u;
	if (!h)
		h = 1;
	perf_info.approx_draws++;
	for (uint32_t k = 0; k < perf_info.nsig; k++)
		if (perf_info.sig[k] == h)
			return;
	if (perf_info.nsig < SALSA64_RDP_TASK_SIGS)
		perf_info.sig[perf_info.nsig++] = h;
}

static inline void lwsync(void) { __asm__ volatile("lwsync" ::: "memory"); }
static inline void hwsync(void) { __asm__ volatile("sync" ::: "memory"); }

/* Avisos al log del frontend, acotados por sitio para no inundarlo. */
static void (*rdp_log)(const char *fmt, ...);
#define LOGN(limit, ...) \
	do { \
		static unsigned n_; \
		if (rdp_log && n_ < (limit)) { \
			n_++; \
			rdp_log(__VA_ARGS__); \
		} \
	} while (0)

void salsa64_rdp_set_log(void (*fn)(const char *fmt, ...))
{
	rdp_log = fn;
}

/* Tablas del emparejamiento: memalign la primera vez que se enciende la
 * interpolación (fuera del bucle) y quedan; apagada no se reservan. */
struct vm_frame {
	struct vmatch_table t;
};
static struct vm_frame *vm;
static void *vm_mem;  /* el bloque de las dos tablas (memalign) */
static size_t vm_bytes;

#define ZCLEAR_PENDING 8
/* Lo que ppe_copy escribió (para no repetirlo igual). */
#define PPE_MEMO 512
struct ppe_memo {
	uint32_t lo, hi, key, sum;
	uint16_t x0, x1;
};
struct depth {
	bool used;
	uint32_t addr, w, h;
	uint32_t bytes;              /* lo que cuenta en rdp.vram */
	/* Borrados pendientes hasta ligarla (en orden). Rogue Squadron borra
	 * la z con dos rellenos seguidos (mitad izquierda y derecha, debajo del
	 * horizonte) antes de dibujar: con uno solo pendiente se perdía el
	 * primero y esa mitad quedaba con la z de cuadros viejos. */
	uint32_t clear_pending;
	float clear_rect[ZCLEAR_PENDING][4];
	float clear_value[ZCLEAR_PENDING];
	/* La z que llega por la RDRAM (zfeed): suma de muestras de la z image
	 * en la RDRAM, tarea en que cambió por última vez, cuántas veces cambió
	 * y la última tarea revisada. */
	uint32_t rd_sum, rd_changed, rd_changes, rd_checked;
	uint32_t fed_task;   /* última tarea en que se cargó desde la RDRAM */
	uint32_t fed_src;    /* última tarea que dibujó como color su dirección, ya cargada */
	uint32_t auto_task;  /* última tarea con el borrado automático (zauto) */
	/* Carga parcial (zfeed): la suma de la RDRAM en la última carga, si ya
	 * hubo una entera desde el último borrado, y las filas que los dibujos
	 * con escritura de z tocaron desde entonces. */
	uint32_t up_sum;
	bool full_ok;
	struct rdp_box zdirty;
	rsxkit_depth *d;
};

static struct {
	bool ready;
	/* Escala-objetivo (240 × scale líneas); cada superficie tiene la suya
	 * (surf_scale, rdp_target_scale) y todo lo que pasa de coordenadas
	 * lógicas a píxeles de una superficie usa la de esa superficie. */
	uint32_t scale;
	bool scale_reduced;          /* la escala se bajó por memoria */
	uint32_t vram;               /* bytes vivos: destinos, scratch y profundidades */
	uint32_t zeta_bytes;         /* profundidad de la pantalla agrandada (apply_scale), en el tope */
	uint32_t surf_bytes[RDP_TARGETS_MAX];
	uint32_t surf_scale[RDP_TARGETS_MAX];  /* escala de la superficie de cada ranura */
	/* Cambia cada vez que algo escribe en la superficie de la ranura (un
	 * dibujo, la carga desde la RDRAM, crearla): para no rehacer lo que
	 * se calcula de una imagen que no cambió (salsa64_rdp_present_source). */
	uint32_t surf_gen[RDP_TARGETS_MAX];
	uint32_t gen_seq;
	uint8_t *pool;
	uint32_t pool_off;
	uint8_t *cache;              /* región de la caché de texturas */
	uint32_t cache_off;
	uint32_t done_task;          /* última tarea que el RSX terminó (hilo de emulación) */
	struct set sets[SETS];
	/* Tareas: acquire y done avanzan `acquired` y `finished` (hilo de
	 * emulación); flush avanza `translated` (presentación). La tarea n usa
	 * el juego n % SETS. */
	volatile uint32_t acquired, finished, translated;
	struct rdp_targets tg;                    /* la tabla de destinos (targets.c) */
	rsxkit_target *surf[RDP_TARGETS_MAX];     /* la superficie de cada ranura */
	bool surf_dirty[RDP_TARGETS_MAX];         /* dibujada desde la última rsxkit_texture_sync */
	rsxkit_target *scratch;      /* copia del destino ligado cuando un dibujo lo lee (SELF) */
	uint32_t scratch_w, scratch_h, scratch_bytes;
	uint32_t scratch_scale;      /* escala de la superficie copiada (la del scratch) */
	int scratch_slot;            /* ranura copiada en esta tarea, o -1 */
	struct rdp_box scratch_dirty;  /* lo dibujado en esa ranura después de copiarla */
	uint32_t ppe_tex_off;        /* arena de la tarea en traducción: hasta dónde está ocupado */
	struct depth depths[MAX_DEPTHS];
	/* Últimos borrados de la z por dirección (tarea de rdp.tg), para saber
	 * qué z images el RDP nunca borra (zfeed), y la barrera de la última
	 * carga de una z desde la RDRAM (su búfer no se reescribe antes). */
	uint32_t zc_addr[4], zc_task[4];
	/* Por dirección: tarea del último borrado de toda la imagen (la caja
	 * de los borrados de una tarea va de la fila y la columna 0 a todo el
	 * ancho) y esa caja en la tarea zc_box_task. */
	uint32_t zc_full[4], zc_box_task[4];
	uint16_t zc_box[4][4];
	/* La primera color image de la tarea anterior y si la de esta es otra
	 * (la tarea empieza un cuadro: zauto). */
	uint32_t first_cimg, cimg_task;
	bool frame_start;
	uint32_t zup_fence;
	/* Direcciones de z images en uso (las escribe la traducción, las lee
	 * readbacks_wanted en el hilo de emulación): una color image en una de
	 * ellas es la z escrita como color y su copia a la RDRAM se espera. */
	volatile uint32_t zimg_seen[4];
	/* Escrituras del PPE en la RDRAM en lugar del RDP (ppe_copy): las
	 * últimas, con la tarea, para cebar los destinos y cargar las z que
	 * caen encima. */
	struct { uint32_t lo, hi, task; } ppe_wr[4];
	struct ppe_memo ppe_memo[PPE_MEMO];  /* el último rectángulo de ppe_copy por dirección */
	/* Cebado por otro ancho (get_target): suma de la RDRAM en el último,
	 * si ya hubo uno entero en esta superficie y lo dibujado desde entonces. */
	uint32_t alias_sum[RDP_TARGETS_MAX];
	bool alias_full[RDP_TARGETS_MAX];
	struct rdp_box alias_dirty[RDP_TARGETS_MAX];
	unsigned ppe_wr_next;
	int bound_t;                 /* ranura ligada, o -1 */
	struct depth *bound_d;
	unsigned last_set;           /* juego de búferes de la última tarea dibujada */
	struct gfx_result last_res;
	struct salsa64_rdp_stats stats;
	struct salsa64_rdp_tb tb;
	/* Efectos de framebuffer (docs/framebuffer.md). */
	int fbfx;                    /* 0 apagado, 1 render a textura, 2 también copias */
	bool aa_edges;               /* salsa64_rdp_aa_edges */
	bool target_alpha;           /* salsa64_rdp_target_alpha */
	bool texture_wait;           /* serialización diagnóstica de escrituras de textura */
	uint8_t *rdram;              /* la RDRAM emulada (NULL: sin copias ni cebado) */
	uint32_t rdram_size;
	void (*wake)(void);          /* despierta al hilo de presentación */
	struct rdp_vi_ring vi_ring;  /* orígenes recientes del VI (hilo de emulación) */
	uint8_t *rb;                 /* búferes de copia (RB_SLOTS) y de cebado, mapeados */
	uint32_t rb_off;
	volatile uint32_t readback_done;  /* última tarea con sus copias ya en la RDRAM */
	/* Tarea (n + 1) cuya copia a la RDRAM espera ahora done, o 0:
	 * salsa64_rdp_flush la traduce aunque quien llama todavía no la cuente.
	 * La presentación traduce solo las tareas que el RDP pudo dibujar, y eso
	 * lo dice el hilo de emulación al terminar cada campo; esperando aquí no
	 * lo termina, así que cada copia esperada se cumplía por el tope de
	 * RB_WAIT_US (200 ms por tarea en la intro de Perfect Dark: 5 campos/s). */
	volatile uint32_t rb_waiting;
	/* Lo mismo para acquire: tarea (n − SETS + 1) que tiene que estar
	 * traducida para que el juego de búferes n % SETS quede libre, o 0. Sin
	 * esto, un juego que manda más de SETS tareas gráficas dentro de un
	 * campo (Paper Mario al abrir el telón, Mario Party tras los logos) se
	 * quedaba esperando en acquire sin cerrar el campo, la presentación no
	 * recibía las tareas visibles y nunca traducía esa: interbloqueo. */
	volatile uint32_t set_waiting;
	bool rb_late_logged;
	/* Copias espejo en vuelo: el RSX escribe el búfer; cuando pasa la
	 * barrera se convierte a la RDRAM (readbacks_finish). */
	struct mirror {
		bool used;
		bool argb;  /* el búfer es ARGB8888 con el alfa del destino (rb_argb) */
		uint32_t addr, width, lw, rows, fence, bpp;
		struct rdp_box box;  /* lo que se escribe en la RDRAM (rows = box.y1) */
		uint32_t task;  /* la tarea que dibujó lo copiado (rdp_dma_since) */
		/* La misma, contada como rdp.finished (la de rdp_claim_ring), o 0:
		 * sin mirar lo que otras color images dibujaron después. */
		uint32_t fin;
	} mirrors[MIRRORS];
	/* DMA a la RDRAM desde el hilo de emulación (targets.h): una copia de
	 * la tarea T no pisa lo que un DMA escribió desde que T empezó. */
	struct rdp_dma_ring dma;
	/* Guarda contra el CPU (salsa64_rdp_guard): apagada a pedido; el
	 * vigilante del log, [watch_lo, watch_hi) de la RDRAM (vacío: apagado). */
	bool guard_off;
	uint32_t watch_lo, watch_hi;
	/* Filas de cada color image que dibujó cada tarea (hilo de emulación,
	 * con las huellas): el espejo del framebuffer no escribe lo que otra
	 * color image dibujó después (targets.h); claims_off (espejo-pisa-otras)
	 * vuelve a como antes. slot_fin: la última tarea (contada como
	 * rdp.finished) que dibujó en cada ranura. */
	struct rdp_claim_ring claims;
	bool claims_off;
	uint32_t slot_fin[RDP_TARGETS_MAX];
	struct mirror frame;         /* el espejo del framebuffer presentado */
	/* Última tarea en que cada ranura se copió a la RDRAM (o se rellenó
	 * su cobertura): el espejo del framebuffer no pisa esa zona mientras
	 * el CPU pueda estar leyéndola. */
	uint32_t rb_task[RDP_TARGETS_MAX];
	struct rdp_present last_p;   /* lo que eligió la última presentación (traza por campo) */
	struct mirror big[RB_SLOTS]; /* copias grandes sin esperar, en los búferes de RB_SLOTS */
	uint32_t prime_fence[PRIME_BUFS];  /* barrera del último cebado que leyó cada búfer (0: libre) */
	unsigned prime_next;               /* búfer del próximo cebado */
	/* Color images pedidas en las últimas RB_HISTORY tareas (hilo de
	 * emulación): una que se repite ya no se espera. */
	uint32_t rb_prev[RB_HISTORY][RB_PER_TASK], rb_prev_rep[RB_HISTORY][RB_PER_TASK];
	/* Si las copias de las capas big8 se esperan (por juego: Yoshi's Story). */
	bool big8_wait;
	/* Si las capas big8 se ceban desde la RDRAM (gfx-big8-sin-cebar lo apaga). */
	bool big8_prime;
	/* Si las capas big8 las dibuja el PPE en la RDRAM (gfx-capas8-rsx lo apaga). */
	bool layer8_on;
	/* Si las copias a la RDRAM de un destino con alfa llevan su alfa al bit
	 * de RGBA5551 (copia-sin-alfa lo apaga: alfa 1, como antes). */
	bool copy_alpha;
	/* Si los dibujos de los ciclos 1 y 2 dejan en un destino con alfa la
	 * cobertura del RDP (gfx_draw.cvg_bit); alfa-fragmento lo apaga: el
	 * alfa del fragmento, como hasta la v0.2.15. */
	bool cvg_alpha;
	/* Si las imágenes chicas de 32 bits las dibuja el PPE (gfx-mascara32-rsx lo apaga). */
	bool mask32_on;
	bool zclear_target;          /* el borrado pertenece a la próxima color image */
	/* Blindaje del RSX (rsx-sin-blindaje lo apaga): una color image que
	 * empieza fuera de la RDRAM no tiene destino (rdp_cimg_impossible). */
	bool rsx_safe;
	uint32_t rb_prev_n[RB_HISTORY];
	uint32_t rb_prev_i;
	/* Traza detallada (color images, ranuras y copias de cada tarea) de las
	 * 6 primeras tareas con copias tras 10 sin ellas (abrir una pausa),
	 * hasta 3 veces por corrida. */
	uint32_t quiet_tasks, trace_left, trace_events;
	struct { int slot; bool fresh; char how[48]; } last_get;  /* qué hizo get_target (traza); fresh: superficie nueva */
	/* Lo último presentado, para repetirlo en una pausa
	 * (salsa64_rdp_present_again); surf NULL: nada que repetir. slot, y0,
	 * pw y ph son la región sin recortar, para repetirla con otra vista o
	 * con otro recorte (el menú de pausa). */
	struct again {
		rsxkit_target *surf;
		int slot;
		uint32_t y0, pw, ph;
		rsxkit_rect src;
		uint32_t w, h;
		rsxkit_view view;
		uint32_t scale;  /* píxeles de la superficie por píxel de la N64 */
		bool interp;     /* lo presentado es un campo intermedio (rdp.interp.surf) */
	} again, again_last; /* again_last: lo del campo anterior (un intermedio que no se pudo lo repite) */
	/* salsa64_rdp_present dibuja (sin filtro) o solo elige la fuente. */
	bool no_draw;
	/* Interpolación por geometría (salsa64_rdp_interp). */
	struct {
		int mode;        /* enum salsa64_interp: con uno distinto de OFF las tareas llevan GFX_JOB_INTERP */
		float t;         /* SALSA64_INTERP_FIXED: cada tarea se dibuja en esa fracción */
		uint32_t cur;    /* tabla de la última tarea traducida (vm[cur]) */
		bool have_prev;  /* vm[cur] es de la tarea anterior a la que se traduce */
		/* Campos intermedios (SALSA64_INTERP_FIELDS, presentación): el
		 * cuadro que se muestra (rdp_targets.task de su dibujo), el campo
		 * en que apareció, los campos que duró el anterior (k) y la
		 * superficie y profundidad de los intermedios (del tamaño del
		 * destino presentado; cuentan en rdp.vram). */
		uint32_t shown_task, frame_field, k;
		uint32_t last_gap;  /* campos que duró el último cuadro (1: el juego va a 60; no se empareja) */
		rsxkit_target *surf;
		rsxkit_depth *depth;
		uint32_t w, h, bytes;
		uint32_t gen;    /* cambia con cada intermedio (salsa64_rdp_present_source) */
		/* Con el hilo de emulación (acquire, interp_hold): la tarea que
		 * acquire está tomando (n + 1), la que se redibuja ahora (su juego
		 * de búferes no se reusa), la más vieja con un redibujo que el RSX
		 * aún no terminó (keep_fence; 0 = en curso: la HLE no suelta de la
		 * caché de texturas lo que usa) y el done_task más alto que recibió
		 * la HLE. */
		volatile uint32_t acq, busy, keep_task, keep_fence, done_given;
		volatile uint32_t done_pending;  /* el done_task que acquire está por recortar (0: ninguno) */
		/* La tarea del cuadro que se muestra: la HLE no suelta de la caché
		 * nada de ella ni de las siguientes (sus intermedios aún se
		 * dibujan); 0 nada. */
		volatile uint32_t protect;
		/* La última tarea con GFX_JOB_INTERP que no cupo (tablas, vértices
		 * o registros: interp_task); 0 ninguna. acquire no pide la bandera
		 * en las INTERP_BACKOFF tareas que siguen (interp_wanted). */
		volatile uint32_t full_task;
	} interp;
	/* Lo que se traduce ahora (salsa64_rdp_translating; lo lee el latido
	 * desde otro hilo): la tarea (n + 1, 0 fuera de translate) y la etapa. */
	volatile uint32_t in_task;
	const char *volatile at;
	/* Caja negra: se avisa de cada tarea al empezar a traducirla. */
	salsa64_rdp_task_hook task_hook;
	void *task_ctx;
	struct salsa64_rdp_field_info finfo;  /* el último campo presentado (salsa64_rdp_field_info) */
} rdp;

/* Diagnóstico de un RSX colgado (salsa64_rdp_diag_log): lo último que pasó
 * con los destinos, las profundidades y las cargas de la z, con su tarea.
 * Son eventos raros (crear, reciclar, cargar la z): un snprintf cada uno. */
#define DIAG_LEN 24
static struct {
	char line[DIAG_LEN][144];
	uint32_t n;
} diag;

static void diag_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void diag_note(const char *fmt, ...)
{
	char *l = diag.line[diag.n % DIAG_LEN];
	int len = snprintf(l, sizeof diag.line[0], "tarea %u: ", rdp.tg.task);
	if (len < 0 || len >= (int)sizeof diag.line[0])
		len = 0;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(l + len, sizeof diag.line[0] - (size_t)len, fmt, ap);
	va_end(ap);
	diag.n++;
}

static void acquire(void *ctx, struct gfx_job *job);
static void done(void *ctx, const struct gfx_job *job, const struct gfx_result *res);
static const struct salsa64_gfx_sink sink = { NULL, acquire, done };

/* Escala-objetivo para la pantalla en curso y la profundidad de la
 * pantalla que la acompaña (ver rdp.h: un destino sin z image se liga con
 * ella y rsxkit_bind no liga uno más grande): se agranda ahora, fuera del
 * bucle, al mayor destino posible a esa escala (rdp_zeta_reserve), o
 * vuelve al tamaño de la pantalla si alcanza. Si el RSX no tiene memoria
 * para agrandarla, se baja la escala (a 1× ningún destino pasa de 640 × 480
 * y siempre alcanza). */
static void apply_scale(uint32_t requested)
{
	const rsxkit_screen *sc = rsxkit_screen_info();
	uint32_t s = rdp_scale_pick(requested, sc->height, &rdp.scale_reduced);
	for (;;) {
		uint32_t w, h;
		bool grow = rdp_zeta_reserve(s, sc->width, sc->height, &w, &h);
		if (rsxkit_depth_reserve(w, h) == RSXKIT_OK) {
			rdp.zeta_bytes = grow ? rdp_surface_bytes(w, h) : 0;
			break;
		}
		if (s <= 1) {
			rdp.zeta_bytes = 0;
			break;
		}
		s--;
		rdp.scale_reduced = true;
	}
	rdp.scale = s;
}

int salsa64_rdp_init(uint32_t scale)
{
	memset(&rdp, 0, sizeof rdp);
	rdp_targets_init(&rdp.tg);
	rdp.bound_t = -1;
	apply_scale(scale);
	rdp.scratch_slot = -1;
	rdp.pool = memalign(1u << 20, POOL_BYTES);
	if (!rdp.pool)
		return RSXKIT_ENOMEM;
	int rc = rsxkit_map_main(rdp.pool, POOL_BYTES, &rdp.pool_off);
	if (rc != RSXKIT_OK)
		return rc;
	for (unsigned i = 0; i < SETS; i++) {
		struct set *s = &rdp.sets[i];
		uint32_t base = i * SET_BYTES;
		s->vtx = rdp.pool + base;
		s->cmd = s->vtx + VTX_BYTES;
		s->tex = s->cmd + CMD_SET;
		s->vtx_off = rdp.pool_off + base;
		s->tex_off = rdp.pool_off + base + VTX_BYTES + CMD_SET;
	}
	rdp.cache = rdp.pool + SETS_BYTES;
	rdp.cache_off = rdp.pool_off + SETS_BYTES;
	rdp.rb = rdp.cache + CACHE_BYTES;
	rdp.rb_off = rdp.cache_off + CACHE_BYTES;
	rdp.fbfx = 2;
	rdp.big8_prime = true;
	rdp.layer8_on = true;
	rdp.copy_alpha = true;
	rdp.cvg_alpha = true;
	rdp.mask32_on = true;
	rdp.rsx_safe = true;
	layer8_clock = tb_now;
	rdp.aa_edges = false;  /* experimental: engordaba los modelos en la PS3 */
	rdp.ready = true;
	return RSXKIT_OK;
}

void salsa64_rdp_set_memory(uint8_t *rdram, uint32_t rdram_size, void (*wake)(void))
{
	rdp.rdram = rdram;
	rdp.rdram_size = rdram_size;
	rdp.wake = wake;
}

void salsa64_rdp_vi(const struct salsa64_vi_frame *f)
{
	rdp_vi_ring_frame(&rdp.vi_ring, f);
}

void salsa64_rdp_target_alpha(bool on)
{
	rdp.target_alpha = on;
}

void salsa64_rdp_fbfx(int level)
{
	rdp.fbfx = level < 0 ? 0 : level > 2 ? 2 : level;
}

int salsa64_rdp_interp(int mode, float t)
{
	bool on = mode == SALSA64_INTERP_FIELDS || mode == SALSA64_INTERP_FIXED;
	if (on && !vm) {
		size_t bytes = vmatch_table_bytes(VM_GROUPS, VM_CLIP);
		uint8_t *mem = memalign(128, 2 * bytes);
		vm = mem ? malloc(2 * sizeof *vm) : NULL;
		if (!vm) {
			free(mem);
			return RSXKIT_ENOMEM;
		}
		vm_mem = mem;
		vm_bytes = 2 * bytes;
		for (int i = 0; i < 2; i++)
			vmatch_table_init(&vm[i].t, mem + i * bytes, VM_GROUPS, VM_CLIP);
	}
	rdp.interp.t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
	rdp.interp.mode = on ? mode : SALSA64_INTERP_OFF;
	if (rdp.interp.mode != SALSA64_INTERP_FIELDS)
		rdp.interp.protect = 0;
	return RSXKIT_OK;
}

/* Suelta las tablas del emparejamiento (al cerrar el juego): si quedan,
 * encendidas a mitad de un juego, fijan el tope del heap por encima de la
 * memoria que el juego suelta al cerrar y el heap no la devuelve. */
void salsa64_rdp_interp_release(void)
{
	rdp.interp.mode = SALSA64_INTERP_OFF;
	rdp.interp.protect = 0;
	rdp.interp.have_prev = false;
	free(vm_mem);
	free(vm);
	vm_mem = NULL;
	vm = NULL;
	vm_bytes = 0;
}

size_t salsa64_rdp_interp_bytes(void)
{
	return vm_bytes;
}

void salsa64_rdp_aa_edges(bool on)
{
	rdp.aa_edges = on;
}

void salsa64_rdp_affine_shade(bool on)
{
	rsxkit_affine_shade(on);
}

bool salsa64_rdp_last_present(struct salsa64_rdp_present_info *out)
{
	if (rdp.last_p.slot < 0 || rdp.last_p.slot >= RDP_TARGETS_MAX)
		return false;
	const struct rdp_target *t = &rdp.tg.t[rdp.last_p.slot];
	if (!t->used || !rdp.again.surf)
		return false;
	*out = (struct salsa64_rdp_present_info){
		rdp.last_p.slot, t->addr, t->lw, t->lh, rdp.again.scale,
		rdp.last_p.y0, rdp.last_p.w, rdp.last_p.h, rdp.last_p.age,
		rdp.again.src.x, rdp.again.src.y, rdp.again.src.w, rdp.again.src.h
	};
	return true;
}

static uint32_t find_black(uint32_t lo, uint32_t hi);
static struct rdp_guard guard;  /* huellas de la guarda contra el CPU (más abajo) */

void salsa64_rdp_rdram_written(uint32_t pa, uint32_t len, bool gfx_in_flight)
{
	rdp_dma_note(&rdp.dma, pa, len, rdp.finished + (gfx_in_flight ? 1u : 0u));
	if (rdp.watch_hi > rdp.watch_lo && len && pa < rdp.watch_hi && pa + len > rdp.watch_lo && rdp.rdram) {
		uint32_t blk = find_black(pa, pa + len);
		LOGN(400,
		     "vigilante: DMA (PI, SI, SP o resultados de un SPE) 0x%06x–0x%06x con %u tareas terminadas%s: %s0x00010001 "
		     "en lo vigilado (la primera en 0x%06x)",
		     pa, pa + len, rdp.finished, gfx_in_flight ? ", una tarea gráfica en vuelo" : "",
		     blk == 0xFFFFFFFFu ? "no queda " : "queda ", blk == 0xFFFFFFFFu ? 0u : blk);
	}
}

/* Un DMA escribió desde la tarea `task` en lo que se iba a copiar: la
 * copia se descarta (Star Fox 64 carga datos encima del framebuffer que
 * acaba de limpiar). */
static bool dma_newer(uint32_t addr, uint32_t bytes, uint32_t task)
{
	if (!rdp_dma_since(&rdp.dma, addr, addr + bytes, task))
		return false;
	rdp.stats.dma_skipped++;
	return true;
}

static void interp_release(void);

static void release_targets(void)
{
	/* Lo que mask32 repone en la RDRAM era de la máquina anterior. */
	mask32_forget();
	rdp.again.surf = NULL;
	rdp.again.interp = false;
	interp_release();
	/* La tabla de destinos vuelve a contar desde 0: nada de lo anterior se
	 * redibuja. */
	rdp.interp.shown_task = rdp.interp.k = rdp.interp.last_gap = 0;
	for (unsigned i = 0; i < SETS; i++)
		rdp.sets[i].tg_task = 0;
	memset(&rdp.again_last, 0, sizeof rdp.again_last);
	for (int i = 0; i < RDP_TARGETS_MAX; i++)
		if (rdp.surf[i])
			rsxkit_target_destroy(rdp.surf[i]);
	for (int i = 0; i < MAX_DEPTHS; i++)
		if (rdp.depths[i].used)
			rsxkit_depth_destroy(rdp.depths[i].d);
	if (rdp.scratch)
		rsxkit_target_destroy(rdp.scratch);
	rdp.scratch = NULL;
	rdp.scratch_w = rdp.scratch_h = rdp.scratch_bytes = rdp.scratch_scale = 0;
	rdp.scratch_slot = -1;
	memset(rdp.surf, 0, sizeof rdp.surf);
	memset(rdp.surf_dirty, 0, sizeof rdp.surf_dirty);
	memset(rdp.surf_bytes, 0, sizeof rdp.surf_bytes);
	memset(rdp.surf_scale, 0, sizeof rdp.surf_scale);
	rdp.vram = 0;
	memset(rdp.depths, 0, sizeof rdp.depths);
	for (int k = 0; k < 4; k++)
		rdp.zimg_seen[k] = 0;
	memset(rdp.ppe_wr, 0, sizeof rdp.ppe_wr);
	memset(rdp.ppe_memo, 0, sizeof rdp.ppe_memo);
	rdp.ppe_wr_next = 0;
	memset(rdp.zc_addr, 0, sizeof rdp.zc_addr);
	memset(rdp.zc_task, 0, sizeof rdp.zc_task);
	memset(rdp.zc_full, 0, sizeof rdp.zc_full);
	memset(rdp.zc_box_task, 0, sizeof rdp.zc_box_task);
	rdp.first_cimg = rdp.cimg_task = 0;
	rdp.frame_start = false;
	rdp.zup_fence = 0;
	rdp.bound_t = -1;
	rdp.bound_d = NULL;
	/* La tabla de destinos, vacía. Sus contadores de texturas siguen: el
	 * log los resta entre informes y, al cargar un estado, salían negativos. */
	uint32_t tex_rsx = rdp.tg.tex_rsx, tex_rdram = rdp.tg.tex_rdram, tex_self = rdp.tg.tex_self;
	rdp_targets_init(&rdp.tg);
	rdp.tg.tex_rsx = tex_rsx;
	rdp.tg.tex_rdram = tex_rdram;
	rdp.tg.tex_self = tex_self;
}

uint32_t salsa64_rdp_scale(bool *reduced)
{
	if (reduced)
		*reduced = rdp.scale_reduced;
	return rdp.scale;
}

void salsa64_rdp_set_scale(uint32_t scale)
{
	if (!rdp.ready)
		return;
	rsxkit_finish();
	/* Todo lo que tiene superficie o tamaño de una superficie se suelta:
	 * destinos, scratch, profundidades. Las copias en vuelo (espejos, el
	 * cuadro, las grandes sin esperar) viven en memoria principal y no
	 * dependen de la escala, pero son del juego anterior: se descartan. */
	release_targets();
	memset(rdp.mirrors, 0, sizeof rdp.mirrors);
	memset(&rdp.frame, 0, sizeof rdp.frame);
	memset(rdp.big, 0, sizeof rdp.big);
	memset(rdp.rb_task, 0, sizeof rdp.rb_task);
	apply_scale(scale);
}

void salsa64_rdp_shutdown(void)
{
	if (!rdp.ready)
		return;
	rsxkit_finish();
	release_targets();
	/* La memoria mapeada queda reservada hasta el fin del proceso. */
	rdp.ready = false;
}

uint32_t salsa64_rdp_reset(void)
{
	if (!rdp.ready)
		return 0;
	rsxkit_finish();
	uint32_t pending = rdp.acquired - rdp.translated;
	release_targets();
	/* Lo que es de un juego vuelve a como lo deja salsa64_rdp_init; la
	 * memoria mapeada (juegos de búferes y caché de texturas) se conserva. */
	rdp.done_task = 0;
	for (unsigned i = 0; i < SETS; i++) {
		rdp.sets[i].fence = 0;
		rdp.sets[i].fence_task = 0;
		memset(&rdp.sets[i].res, 0, sizeof rdp.sets[i].res);
	}
	rdp.acquired = rdp.finished = rdp.translated = 0;
	memset(&rdp.dma, 0, sizeof rdp.dma);
	memset(&rdp.claims, 0, sizeof rdp.claims);
	memset(rdp.slot_fin, 0, sizeof rdp.slot_fin);
	rdp.readback_done = 0;
	rdp.rb_late_logged = false;
	memset(rdp.mirrors, 0, sizeof rdp.mirrors);
	memset(&rdp.frame, 0, sizeof rdp.frame);
	memset(rdp.big, 0, sizeof rdp.big);
	memset(rdp.prime_fence, 0, sizeof rdp.prime_fence);
	rdp.prime_next = 0;
	memset(rdp.rb_prev_n, 0, sizeof rdp.rb_prev_n);
	memset(rdp.rb_task, 0, sizeof rdp.rb_task);
	memset(&rdp.vi_ring, 0, sizeof rdp.vi_ring);
	memset(rdp.surf_dirty, 0, sizeof rdp.surf_dirty);
	rdp.last_set = 0;
	memset(&rdp.last_res, 0, sizeof rdp.last_res);
	rdp.interp.have_prev = false;
	rdp.interp.acq = rdp.interp.busy = rdp.interp.keep_task = rdp.interp.keep_fence = rdp.interp.done_given = 0;
	rdp.interp.done_pending = 0;
	rdp.interp.protect = 0;
	rdp.interp.full_task = 0;
	memset(&rdp.again_last, 0, sizeof rdp.again_last);
	memset(&rdp.stats, 0, sizeof rdp.stats);
	memset(&rdp.tb, 0, sizeof rdp.tb);
	rdp_guard_clear(&guard);
	return pending;
}

static void readbacks_finish(bool wait);

void salsa64_rdp_settle(void)
{
	if (!rdp.ready)
		return;
	rsxkit_finish();
	readbacks_finish(true);
}

void salsa64_rdp_drop_targets(void)
{
	if (!rdp.ready)
		return;
	rsxkit_finish();
	release_targets();
	/* Las copias que quedaran en vuelo y la memoria de lo copiado eran de la
	 * máquina anterior (salsa64_rdp_settle ya las vació): nada de eso va a
	 * la RDRAM cargada. La numeración de las tareas y readback_done siguen. */
	memset(rdp.mirrors, 0, sizeof rdp.mirrors);
	memset(&rdp.frame, 0, sizeof rdp.frame);
	memset(rdp.big, 0, sizeof rdp.big);
	memset(rdp.rb_task, 0, sizeof rdp.rb_task);
	memset(rdp.rb_prev, 0, sizeof rdp.rb_prev);
	memset(rdp.rb_prev_rep, 0, sizeof rdp.rb_prev_rep);
	memset(rdp.rb_prev_n, 0, sizeof rdp.rb_prev_n);
	rdp.rb_prev_i = 0;
	memset(&rdp.vi_ring, 0, sizeof rdp.vi_ring);
	rdp.scratch_slot = -1;
	rdp_guard_clear(&guard);  /* huellas de la RDRAM anterior */
	memset(&rdp.claims, 0, sizeof rdp.claims);
	memset(rdp.slot_fin, 0, sizeof rdp.slot_fin);
}

const struct salsa64_gfx_sink *salsa64_rdp_sink(void)
{
	return &sink;
}

/* ¿Pedir a la HLE la identidad de los vértices (GFX_JOB_INTERP) para la
 * tarea n + 1? Le cuesta al SPE 0 y la emulación lo espera (en Super Odyssey
 * 64, que no cabía en las tablas, el SPE tardaba en cada tarea un 60 % más y
 * la emulación pasó de 60 a 55 campos por segundo, con el audio estirado), así
 * que solo cuando puede servir: no con el juego a 60 cuadros (no hay campos
 * intermedios) ni en las INTERP_BACKOFF tareas que siguen a una que no cupo;
 * después se vuelve a probar (la primera con la bandera es la anterior de la
 * que sigue). La prueba fija (SALSA64_INTERP_FIXED) la pide siempre. */
#define INTERP_BACKOFF 90u
static bool interp_wanted(uint32_t n)
{
	if (rdp.interp.mode == SALSA64_INTERP_OFF || !vm)
		return false;
	if (rdp.interp.mode != SALSA64_INTERP_FIELDS)
		return true;
	if (rdp.interp.last_gap == 1)
		return false;
	uint32_t f = rdp.interp.full_task;
	return !f || (int32_t)(n + 1 - f) >= (int32_t)INTERP_BACKOFF;
}

static void acquire(void *ctx, struct gfx_job *job)
{
	(void)ctx;
	uint32_t n = rdp.acquired;
	struct set *s = &rdp.sets[n % SETS];
	/* Campos intermedios: el juego que se va a pisar no puede estar
	 * redibujándose (interp_hold, del otro lado). */
	bool fields = rdp.interp.mode == SALSA64_INTERP_FIELDS || rdp.interp.busy || rdp.interp.keep_task;
	if (fields) {
		rdp.interp.acq = n + 1;
		hwsync();
	}
	/* Libre cuando su tarea anterior (n − SETS) ya se tradujo y el RSX pasó
	 * la barrera que la siguió. */
	if (n >= SETS) {
		uint64_t t0 = 0;
		for (;;) {
			bool translated = (int32_t)(rdp.translated - (n - SETS + 1)) >= 0;
			lwsync();
			if (translated && rsxkit_fence_done(s->fence) && (!fields || rdp.interp.busy != n - SETS + 1))
				break;
			if (!t0) {
				t0 = sysGetSystemTime();
				rdp.stats.set_waits++;
				/* Que la presentación traduzca esa tarea aunque todavía no
				 * sea visible (el campo no se cierra mientras se espera). */
				rdp.set_waiting = n - SETS + 1;
				lwsync();
				if (rdp.wake)
					rdp.wake();
			}
			sysUsleep(50);
		}
		s->set_wait_us = 0;
		if (t0) {
			rdp.set_waiting = 0;
			uint64_t dt = sysGetSystemTime() - t0;
			rdp.stats.us_set_wait += dt;
			s->set_wait_us = (uint32_t)(dt > 0xFFFFFFFFu ? 0xFFFFFFFFu : dt);
		}
	}
	/* Diagnóstico fuerte: antes de que la HLE escriba caché o arena, todas
	 * las tareas anteriores deben haber sido traducidas y terminadas por el
	 * RSX. Solo se leen etiquetas ya publicadas; este hilo no toca el FIFO. */
	if (rdp.texture_wait && n) {
		uint64_t t0 = 0;
		struct set *prev = &rdp.sets[(n - 1) % SETS];
		for (;;) {
			bool translated = (int32_t)(rdp.translated - n) >= 0;
			lwsync();
			uint32_t ft = prev->fence_task;
			lwsync();
			if (translated && ft == n && rsxkit_fence_done(prev->fence))
				break;
			if (!t0) {
				t0 = sysGetSystemTime();
				rdp.stats.tex_waits++;
				rdp.set_waiting = n;
				lwsync();
				if (rdp.wake)
					rdp.wake();
			}
			sysUsleep(50);
		}
		if (t0) {
			rdp.set_waiting = 0;
			rdp.stats.us_tex_wait += sysGetSystemTime() - t0;
		}
	}
	/* Tareas que el RSX ya terminó: la de cada juego cuya barrera pasó (las
	 * hace en orden). fence_task se lee antes que fence: si es la nueva,
	 * fence también lo es (translate las escribe al revés). */
	for (unsigned k = 0; k < SETS; k++) {
		uint32_t ft = rdp.sets[k].fence_task;
		lwsync();
		if (ft && (int32_t)(ft - rdp.done_task) > 0 && rsxkit_fence_done(rdp.sets[k].fence))
			rdp.done_task = ft;
	}
	uint32_t last = rdp.translated, done = rdp.done_task;
	if ((int32_t)(done - (last - 1)) > 0)
		done = last - 1;
	if (fields) {
		/* Lo que usa una tarea que se redibuja (o cuyo redibujo el RSX no
		 * terminó) no sale de la caché de texturas. Mientras se recorta, el
		 * valor sin recortar queda a la vista de interp_hold (si llega justo
		 * ahora, desiste); después, done_given es lo que de verdad recibe la
		 * HLE. Guardar ahí el valor sin recortar hacía que, con la
		 * traducción un cuadro por delante de lo que se ve, ningún
		 * intermedio llegara a tiempo (Mario 64: todos «repetidos»). */
		rdp.interp.done_pending = done;
		hwsync();
		uint32_t kt = rdp.interp.keep_task;
		lwsync();
		uint32_t kf = rdp.interp.keep_fence;
		if (kt && (!kf || !rsxkit_fence_done(kf)) && (int32_t)(done - kt) >= 0)
			done = kt - 1;
		/* La del cuadro en pantalla y las siguientes (se van a mostrar), salvo
		 * las que ya no se pueden redibujar: si la emulación va más adelante
		 * que eso (Mario 64 dibuja cuadros antes de mostrarlos), desde la más
		 * vieja cuyo juego de búferes sigue vivo (este acquire reusa el de la
		 * tarea n + 1 − SETS). Apagarla entera en ese caso dejaba sin
		 * proteger también las siguientes: ningún intermedio a tiempo. */
		uint32_t pr = rdp.interp.protect;
		if (pr) {
			uint32_t alive = n + 2 - SETS;  /* la más vieja con su juego vivo tras este acquire */
			uint32_t lo = (int32_t)(alive - pr) > 0 ? alive : pr;
			if ((int32_t)(done - lo) >= 0)
				done = lo - 1;
		}
		if ((int32_t)(done - rdp.interp.done_given) > 0)
			rdp.interp.done_given = done;
		lwsync();
		rdp.interp.done_pending = 0;
	}
	job->task = n + 1;
	job->done_task = done;
	job->cache_ea = (uint64_t)(uintptr_t)rdp.cache;
	job->cache_cap = CACHE_BYTES;
	job->vtx_ea = (uint64_t)(uintptr_t)s->vtx;
	job->vtx_cap = VTX_BYTES;
	job->cmd_ea = (uint64_t)(uintptr_t)s->cmd;
	job->cmd_cap = CMD_BYTES;
	job->tex_ea = (uint64_t)(uintptr_t)s->tex;
	job->tex_cap = TEX_BYTES;
	/* Lo que viene de un destino reciente se lee del RSX: que la HLE no lo
	 * decodifique (texture_from_target y, si al final no se puede, la
	 * decodificación en el PPE). */
	if (rdp.fbfx >= 1)
		job->flags |= GFX_JOB_FBTEX;
	s->interp = interp_wanted(n);
	if (s->interp) {
		job->flags |= GFX_JOB_INTERP;
		job->cmd_cap = CMD_SET;
	} else if (rdp.interp.mode != SALSA64_INTERP_OFF) {
		rdp.stats.interp_skipped++;
	}
	rdp.acquired = n + 1;
}

/* Hasta qué línea llegan las tijeras de los dibujos que siguen a un
 * TARGET (hasta el siguiente): el alto mínimo de su superficie. */
/* ¿Los dibujos hasta el próximo TARGET son todos rellenos (FILLRECT)? Una
 * color image alta con solo rellenos limpia varios framebuffers de una vez
 * (Star Fox 64, DK64) y se reparte; con dibujos de verdad es una imagen
 * (la transición del cierre de DK64, de 320 × 459, que el CPU lee después)
 * y se crea tal cual. */
static bool fills_only(const struct set *s, uint32_t off, uint32_t cmd_bytes)
{
	while (off + sizeof(struct gfx_cmd) <= cmd_bytes) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(s->cmd + off);
		if (c->op == GFX_OP_END || c->op == GFX_OP_TARGET || c->size < sizeof *c)
			break;
		if (c->op == GFX_OP_DRAW && ((const struct gfx_draw *)c)->cycle != G_CYC_FILL)
			return false;
		off += c->size;
	}
	return true;
}

static uint32_t scissor_bottom(const struct set *s, uint32_t off, uint32_t cmd_bytes)
{
	uint32_t bottom = 0;
	while (off + sizeof(struct gfx_cmd) <= cmd_bytes) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(s->cmd + off);
		if (c->op == GFX_OP_END || c->op == GFX_OP_TARGET || c->size < sizeof *c)
			break;
		if (c->op == GFX_OP_DRAW) {
			uint32_t y1 = (((const struct gfx_draw *)c)->scissor[3] + 3) / 4;
			if (y1 > bottom)
				bottom = y1;
		}
		off += c->size;
	}
	return bottom;
}

/* Región lógica (x, y, w, h) del destino de la ranura, en píxeles de su
 * superficie: con la escala de ese destino (surf_scale), no la objetivo. */
static rsxkit_rect surface_rect(int slot, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	struct rdp_rect r;
	rdp_surface_rect(rdp.surf_scale[slot], x, y, w, h, &r);
	return (rsxkit_rect){ (int32_t)r.x, (int32_t)r.y, r.w, r.h };
}

/* Reloj del hilo que traduce (timebase, 79,8 MHz): rdp.tb. */
static inline uint64_t tb_now(void)
{
	uint64_t t;
	__asm__ volatile("mftb %0" : "=r"(t));
	return t;
}

/* Espera una barrera del RSX (duerme, rsxkit_fence_wait) y lo anota. */
static void fence_wait(uint32_t fence)
{
	uint64_t t0 = tb_now();
	rsxkit_fence_wait(fence);
	rdp.tb.fence += tb_now() - t0;
}

/* Imágenes de 8 bits grandes (más de 128 de ancho): no son la cobertura ni
 * las sombras chicas sino capas que el juego dibuja con el RDP y después lee
 * como CI8 con paleta (los fondos en capas de Yoshi's Story: cielo, colinas y
 * árboles, 336 × 256 en tres imágenes). El índice tiene que llegar entero a
 * la RDRAM: van a 1× (el filtro al reducir mezclaría índices vecinos), se
 * copian con los 8 bits del rojo (RGB565 solo da 5: la paleta salía revuelta)
 * y no pasan por los espejos chicos. */
static inline bool big8(uint32_t bpp, uint32_t lw)
{
	return bpp == 1 && lw > 128;
}

/* Bytes por fila del búfer de la copia de una imagen (RGB565, o XRGB8888 las big8). */
static inline uint32_t rb_pitch(uint32_t bpp, uint32_t lw)
{
	return big8(bpp, lw) ? (lw * 4 + 63) & ~63u : RB_PITCH;
}

/* Bytes por fila de una copia en ARGB8888 (rb_argb). */
static inline uint32_t argb_pitch(uint32_t lw)
{
	return (lw * 4 + 63) & ~63u;
}

/* ¿La copia a la RDRAM de esta ranura lleva el alfa del destino? Las de 16
 * bits con superficie con alfa (alpha_wanted): el RDP deja en el bit de
 * alfa de RGBA5551 lo que escribió (la cobertura de lo dibujado, el bit del
 * color de un relleno), y una textura que el juego lee de ahí cuando el
 * destino ya es viejo (RDP_TARGETS_FRESH) se decodifica de la RDRAM. Con la
 * copia en RGB565 ese bit salía siempre 1: LEGO Racers dibuja cada auto
 * lejano en una imagen de 32 × 32 borrada con alfa 0 y la usa con prueba de
 * alfa; leída de la RDRAM, el auto salía dentro de un cuadro (#34). */
static inline bool rb_argb(int slot)
{
	const struct rdp_target *t = &rdp.tg.t[slot];
	/* Un framebuffer que nació con alfa (antes del primer campo del VI no se
	 * sabe su ancho) se sigue copiando como antes. */
	return rdp.copy_alpha && t->bpp == 2 && !t->presented && t->width != rdp.tg.vi_width && rdp.surf[slot] &&
	       rsxkit_target_has_alpha(rdp.surf[slot]);
}

/* ---- Guarda contra el CPU y vigilante (rdp_guard, targets.h) ---- */

/* `guard`: 512 KB en el BSS (docs/memory-budget.md), declarada arriba. */

void salsa64_rdp_guard(bool on, uint32_t watch_lo, uint32_t watch_hi)
{
	rdp.guard_off = !on;
	rdp.watch_lo = watch_lo;
	rdp.watch_hi = watch_hi;
	rdp_guard_clear(&guard);
}

void salsa64_rdp_claims(bool on)
{
	rdp.claims_off = !on;
	memset(&rdp.claims, 0, sizeof rdp.claims);
}

static inline bool watched(uint32_t lo, uint32_t hi)
{
	return rdp.watch_hi > rdp.watch_lo && lo < rdp.watch_hi && hi > rdp.watch_lo;
}

/* Primera palabra alineada 0x00010001 (un negro de RGBA5551 dos veces) en
 * la RDRAM [lo, hi) dentro de lo vigilado, o 0xFFFFFFFF. */
static uint32_t find_black(uint32_t lo, uint32_t hi)
{
	lo = lo < rdp.watch_lo ? rdp.watch_lo : lo;
	hi = hi > rdp.watch_hi ? rdp.watch_hi : hi;
	if (hi > rdp.rdram_size)
		hi = rdp.rdram_size;
	for (uint32_t a = (lo + 3) & ~3u; a + 4 <= hi; a += 4)
		if (*(const uint32_t *)(rdp.rdram + a) == 0x00010001u)
			return a;
	return 0xFFFFFFFFu;
}

/* Hilo de emulación, al terminar la tarea `task` (contada como
 * rdp.finished): huellas de las filas que sus dibujos pudieron tocar (las
 * tijeras) en cada color image de 8 o 16 bits (las que pueden volver por una
 * copia), antes de que llegue ninguna copia de la tarea; y esas filas, de
 * cualquier color image de 8, 16 o 32 bits, en rdp.claims. */
static void guard_mark_task(const struct set *s, const struct gfx_result *res, uint32_t task)
{
	if (!rdp.rdram || rdp.fbfx < 2 || rdp.guard_off)
		return;
	uint64_t t0 = tb_now();
	uint32_t addr = 0, line = 0, cline = 0, y0 = ~0u, y1 = 0, off = 0;
	for (;;) {
		const struct gfx_cmd *c =
			off + sizeof(struct gfx_cmd) <= res->cmd_bytes ? (const struct gfx_cmd *)(s->cmd + off) : NULL;
		bool end = !c || c->op == GFX_OP_END || c->size < sizeof *c;
		if (end || c->op == GFX_OP_TARGET) {
			if (line && y1 > y0)
				rdp_guard_mark(&guard, rdp.rdram, rdp.rdram_size, addr + y0 * line, addr + y1 * line);
			if (cline && y1 > y0 && !rdp.claims_off)
				rdp_claim_note(&rdp.claims, addr, cline, addr + y0 * cline, addr + y1 * cline, task);
			if (end)
				break;
			uint32_t siz = c->c & 0xFF;
			addr = c->a;
			line = (siz == 1u || siz == 2u) && c->b && c->b <= RB_MAX_W ? c->b * siz : 0;
			cline = siz >= 1u && siz <= 3u && c->b && c->b <= RB_MAX_W ? c->b << (siz - 1u) : 0;
			y0 = ~0u;
			y1 = 0;
		} else if (c->op == GFX_OP_DRAW && c->size >= sizeof(struct gfx_draw)) {
			const struct gfx_draw *g = (const struct gfx_draw *)c;
			uint32_t a = g->scissor[1] / 4u, b = (g->scissor[3] + 3u) / 4u;
			y0 = a < y0 ? a : y0;
			y1 = b > y1 ? b : y1;
		}
		off += c->size;
	}
	rdp.stats.tb_guard_mark += tb_now() - t0;
}

/* Líneas de 128 bytes que puede tocar una fila de una escritura tardía. */
#define ROW_LINES ((RB_MAX_W * 2) / RDP_GUARD_LINE + 2)

/* Una escritura tardía (hilo de presentación) de la fila [d0, d1) de la
 * RDRAM: true si se pueden escribir todas sus líneas; si no, `ok` dice
 * cuáles (una por línea desde d0 >> 7) y *skipped cuenta las otras. */
static bool guard_row(uint32_t d0, uint32_t d1, bool ok[ROW_LINES], uint32_t *skipped)
{
	if (rdp.guard_off)
		return true;
	uint32_t l0 = d0 >> RDP_GUARD_SHIFT, l1 = (d1 - 1) >> RDP_GUARD_SHIFT;
	bool all = true;
	for (uint32_t l = l0; l <= l1 && l - l0 < ROW_LINES; l++) {
		ok[l - l0] = rdp_guard_ok(&guard, rdp.rdram, l);
		if (!ok[l - l0]) {
			all = false;
			(*skipped)++;
		}
	}
	return all;
}

/* Las líneas de la fila [d0, d1) que otra color image dibujó después
 * (los tramos `cl` de rdp_claims_after) tampoco se escriben: su huella ya
 * no es la del final de la tarea copiada (targets.h, rdp_claim_ring). true si
 * quitó alguna de `ok` (que guard_row ya llenó); *claimed las cuenta. */
static bool claims_row(uint32_t d0, uint32_t d1, bool ok[ROW_LINES], const struct rdp_claim *cl, int ncl,
		       uint32_t *claimed)
{
	uint32_t l0 = d0 >> RDP_GUARD_SHIFT, l1 = (d1 - 1) >> RDP_GUARD_SHIFT;
	bool any = false;
	for (uint32_t l = l0; l <= l1 && l - l0 < ROW_LINES; l++)
		if (ok[l - l0] && rdp_claimed(cl, ncl, l)) {
			ok[l - l0] = false;
			(*claimed)++;
			any = true;
		}
	return any;
}

static void guard_wrote(uint32_t d0, uint32_t d1, const bool *ok)
{
	if (rdp.guard_off)
		return;
	uint32_t l0 = d0 >> RDP_GUARD_SHIFT, l1 = (d1 - 1) >> RDP_GUARD_SHIFT;
	for (uint32_t l = l0; l <= l1 && l - l0 < ROW_LINES; l++)
		if (!ok || ok[l - l0])
			rdp_guard_wrote(&guard, rdp.rdram, l);
}

/* Lo que una escritura tardía cuenta al log: líneas, saltadas y la primera
 * saltada con lo que tenía (lo que el CPU dejó). */
struct late_note {
	uint32_t lines, skipped, first_skip, first_word[2];
	bool have_skip;
	/* Las que no se escribieron porque otra color image las dibujó después
	 * (claims_row), y el primer tramo de esa otra. */
	uint32_t claimed, claim_lo, claim_hi;
};

static void late_skip_note(struct late_note *nt, uint32_t d0, const bool *ok, uint32_t nlines)
{
	if (nt->have_skip)
		return;
	for (uint32_t i = 0; i < nlines && i < ROW_LINES; i++)
		if (!ok[i]) {
			uint32_t a = ((d0 >> RDP_GUARD_SHIFT) + i) << RDP_GUARD_SHIFT;
			a = (a < d0 ? d0 : a) & ~3u;
			nt->have_skip = true;
			nt->first_skip = a;
			nt->first_word[0] = *(const uint32_t *)(rdp.rdram + a);
			nt->first_word[1] = a + 8 <= rdp.rdram_size ? *(const uint32_t *)(rdp.rdram + a + 4) : 0;
			return;
		}
}

static void late_log(const char *who, uint32_t addr, uint32_t width, uint32_t bpp, const struct rdp_box *b,
		     uint32_t task, const struct late_note *nt)
{
	uint32_t lo = addr + b->y0 * width * bpp, hi = addr + b->y1 * width * bpp;
	if (nt->skipped) {
		rdp.stats.guard_copies++;
		rdp.stats.guard_lines += nt->skipped;
		LOGN(12,
		     "gfx: %s a la RDRAM 0x%06x–0x%06x (ancho %u, %u bytes por píxel, filas %u–%u, columnas %u–%u) de la "
		     "tarea %u (%u terminadas): %u de %u líneas de 128 bytes sin escribir porque algo que no es el RDP "
		     "las cambió desde que terminó la tarea (la primera en 0x%06x: %08x %08x)",
		     who, lo, hi, width, bpp, b->y0, b->y1, b->x0, b->x1, task, rdp.finished, nt->skipped, nt->lines,
		     nt->first_skip, nt->first_word[0], nt->first_word[1]);
	}
	if (nt->claimed) {
		rdp.stats.claim_copies++;
		rdp.stats.claim_lines += nt->claimed;
		LOGN(12,
		     "gfx: %s a la RDRAM 0x%06x–0x%06x (ancho %u, filas %u–%u) de la tarea %u (%u terminadas): %u de %u "
		     "líneas de 128 bytes sin escribir porque otra color image las dibujó en una tarea posterior "
		     "(0x%06x–0x%06x; espejo-pisa-otras las escribe)",
		     who, lo, hi, width, b->y0, b->y1, task, rdp.finished, nt->claimed, nt->lines, nt->claim_lo,
		     nt->claim_hi);
	}
	if (watched(lo, hi)) {
		uint32_t blk = find_black(lo, hi);
		LOGN(400,
		     "vigilante: %s 0x%06x–0x%06x (ancho %u, filas %u–%u) de la tarea %u (%u terminadas): %u líneas, "
		     "%u saltadas por la guarda%s; %s0x00010001 en lo vigilado (la primera en 0x%06x)",
		     who, lo, hi, width, b->y0, b->y1, task, rdp.finished, nt->lines, nt->skipped,
		     rdp.guard_off ? " (guarda apagada)" : "", blk == 0xFFFFFFFFu ? "no queda " : "queda ",
		     blk == 0xFFFFFFFFu ? 0u : blk);
	}
}

/* Escribe en la RDRAM, como RGBA5551, la caja `b` (rdp_box: lo que el RDP
 * pudo escribir, nunca filas enteras) de una copia RGB565 (o ARGB8888 con
 * `argb`: el bit de alfa sale del alfa del destino) de `lw` píxeles
 * por fila con paso `pitch` que empieza en la fila 0 del destino. Las líneas
 * de 128 bytes que algo que no es el RDP cambió desde que terminó la tarea
 * no se escriben (rdp_guard, targets.h); con `fin` (≠ 0, la misma tarea
 * contada como rdp.finished), tampoco las que otra color image dibujó en una
 * tarea posterior (rdp_claim_ring). `who` y `task` (la tarea que lo dibujó)
 * son para el log de la guarda y del vigilante. */
static void to_rdram(const uint8_t *src, uint32_t pitch, uint32_t addr, uint32_t width, uint32_t lw,
		     const struct rdp_box *b, uint32_t bpp, bool argb, const char *who, uint32_t task, uint32_t fin)
{
	uint64_t t0 = tb_now();
	uint32_t x1 = b->x1 < lw ? b->x1 : lw;
	if (rdp_box_empty(b) || x1 <= b->x0)
		return;
	/* La dirección 0 es la imagen de color que la HLE usa mientras ninguna tarea que corra
	 * en ella haya fijado una (SETCIMG): Resident Evil 2 lo hace en tareas S2DEX sueltas, que
	 * no tienen HLE, y sus tareas F3DEX.Rej dibujan antes un relleno de 436 × 383. Copiarlo
	 * a la RDRAM pisaba las variables de libultra (el hilo en curso, en 0x12950, quedaba en
	 * 0x00010001 y el juego se colgaba en su manejador de excepciones). */
	if (addr == 0)
		return;
	uint32_t dbpp = bpp == 1 ? 1 : 2;  /* bytes por píxel en la RDRAM */
	if (addr + (b->y1 - 1) * width * dbpp + x1 * dbpp > rdp.rdram_size)
		return;
	/* El paso de las big8 (XRGB8888) antes de soltar las líneas: con el de
	 * RGB565 (1280 contra 1344 bytes en las capas de 336 de Yoshi's Story)
	 * las últimas filas de la caja se leían de líneas viejas de la caché. */
	if (big8(bpp, lw)) {
		pitch = rb_pitch(bpp, lw);
		rdp.stats.big8_copies++;
	}
	argb = argb && bpp == 2;
	if (argb) {
		pitch = argb_pitch(lw);
		rdp.stats.alpha_copies++;
	}
	/* El RSX escribió el búfer a espaldas de la caché del PPE: soltar las
	 * líneas que pudieran quedar de la lectura anterior. */
	for (uint32_t off = b->y0 * pitch & ~127u; off < b->y1 * pitch; off += 128)
		__asm__ volatile("dcbf 0,%0" ::"r"(src + off) : "memory");
	__asm__ volatile("sync" ::: "memory");
	struct late_note nt = {0};
	bool ok[ROW_LINES];
	/* Con `fin` (la tarea que dibujó lo copiado, contada como rdp.finished),
	 * lo que otra color image dibujó en una tarea posterior no se escribe
	 * (targets.h, rdp_claim_ring); si el anillo ya no lo sabe, como antes. */
	struct rdp_claim cl[4];
	int ncl = 0;
	if (fin && !rdp.claims_off && !rdp.guard_off) {
		uint32_t lo = addr + b->y0 * width * dbpp, hi = addr + (b->y1 - 1) * width * dbpp + x1 * dbpp;
		ncl = rdp_claims_after(&rdp.claims, addr, width * dbpp, lo, hi, fin, cl, 4);
		if (ncl > 0) {
			nt.claim_lo = cl[0].lo;
			nt.claim_hi = cl[0].hi;
		} else {
			ncl = 0;
		}
	}
	for (uint32_t y = b->y0; y < b->y1; y++) {
		const uint16_t *row = (const uint16_t *)(src + y * pitch);
		uint32_t rowa = addr + y * width * dbpp, d0 = rowa + b->x0 * dbpp, d1 = rowa + x1 * dbpp;
		uint32_t nl = ((d1 - 1) >> RDP_GUARD_SHIFT) - (d0 >> RDP_GUARD_SHIFT) + 1;
		nt.lines += nl;
		bool all = guard_row(d0, d1, ok, &nt.skipped);
		if (!all)
			late_skip_note(&nt, d0, ok, nl);
		if (ncl && claims_row(d0, d1, ok, cl, ncl, &nt.claimed))
			all = false;
		/* La fila entera si valen todas sus líneas; si no, línea por línea
		 * (los píxeles de las saltadas se quedan como los dejó el CPU). */
		for (uint32_t i = 0; i < (all ? 1u : nl); i++) {
			uint32_t px0 = b->x0, px1 = x1;
			if (!all) {
				if (i >= ROW_LINES || !ok[i])
					continue;
				uint32_t la = ((d0 >> RDP_GUARD_SHIFT) + i) << RDP_GUARD_SHIFT;
				uint32_t s0 = la > d0 ? la : d0, s1 = la + RDP_GUARD_LINE < d1 ? la + RDP_GUARD_LINE : d1;
				px0 = (s0 - rowa) / dbpp;
				px1 = (s1 - rowa + dbpp - 1) / dbpp;
				px1 = px1 > x1 ? x1 : px1;
				if (px1 <= px0)
					continue;
			}
			if (big8(bpp, lw)) {
				/* XRGB8888 (bytes X, R, G, B): el índice entero es el rojo. */
				const uint8_t *px = src + y * pitch;
				uint8_t *dst = rdp.rdram + rowa;
				uint32_t changed = 0;
				for (uint32_t x = px0; x < px1; x++) {
					changed += dst[x] != px[4 * x + 1];
					dst[x] = px[4 * x + 1];
				}
				rdp.stats.big8_changed += changed;
			} else if (bpp == 1) {
				/* Color image de 8 bits (la cobertura que Zelda copia para el
				 * filtro de Link): la intensidad es el rojo del RGB565. */
				uint8_t *dst = rdp.rdram + rowa;
				for (uint32_t x = px0; x < px1; x++)
					dst[x] = (uint8_t)((row[x] >> 8 & 0xF8) | (row[x] >> 13));
			} else if (argb) {
				/* ARGB8888 (bytes A, R, G, B): 5 bits por canal y el alfa,
				 * filtrado al reducir, al bit de alfa por la mitad. */
				const uint8_t *px = src + y * pitch;
				uint8_t *dst = rdp.rdram + rowa;
				for (uint32_t x = px0; x < px1; x++) {
					const uint8_t *q = px + 4 * x;
					uint32_t v = (uint32_t)(q[1] >> 3) << 11 | (uint32_t)(q[2] >> 3) << 6 |
						     (uint32_t)(q[3] >> 3) << 1 | (q[0] >= 0x80);
					dst[2 * x] = (uint8_t)(v >> 8);
					dst[2 * x + 1] = (uint8_t)v;
				}
			} else {
				rsxkit_pixels_565_to_5551((uint16_t *)(rdp.rdram + rowa) + px0, row + px0, px1 - px0);
			}
		}
		guard_wrote(d0, d1, all ? NULL : ok);
	}
	late_log(who, addr, width, dbpp, b, task, &nt);
	rdp.tb.copy += tb_now() - t0;
}

/* Copias espejo cuya barrera ya pasó: a la RDRAM y el búfer queda libre. */
static void readbacks_finish(bool wait)
{
	struct mirror *f = &rdp.frame;
	if (f->used && (wait ? (fence_wait(f->fence), true) : rsxkit_fence_done(f->fence))) {
		if (!dma_newer(f->addr, f->width * 2 * f->rows, f->task)) {
			to_rdram(rdp.rb + FRAME_SLOT * RB_SLOT, RB_PITCH, f->addr, f->width, f->lw, &f->box, 2, false,
				 "espejo del framebuffer", f->task, f->fin);
			rdp.stats.frames_mirrored++;
		}
		f->used = false;
	}
	for (int k = 0; k < RB_SLOTS; k++) {
		struct mirror *m = &rdp.big[k];
		if (m->used && (wait ? (fence_wait(m->fence), true) : rsxkit_fence_done(m->fence))) {
			uint32_t bpp = m->bpp ? m->bpp : 2;
			if (!dma_newer(m->addr, m->width * bpp * m->rows, m->task)) {
				to_rdram(rdp.rb + k * RB_SLOT, RB_PITCH, m->addr, m->width, m->lw, &m->box, bpp, m->argb,
					 "copia grande sin esperar", m->task, 0);
				rdp.stats.mirrors++;
			}
			m->used = false;
		}
	}
	for (int k = 0; k < MIRRORS; k++) {
		struct mirror *m = &rdp.mirrors[k];
		if (!m->used)
			continue;
		if (wait)
			fence_wait(m->fence);
		else if (!rsxkit_fence_done(m->fence))
			continue;
		uint32_t pitch = (m->lw * 2 + 63) & ~63u, bpp = m->bpp ? m->bpp : 2;
		if (!dma_newer(m->addr, m->width * bpp * m->rows, m->task)) {
			to_rdram(rdp.rb + (RB_SLOTS + 2) * RB_SLOT + k * MIRROR_SLOT, pitch, m->addr, m->width, m->lw, &m->box,
				 bpp, m->argb, "copia espejo", m->task, 0);
			rdp.stats.mirrors++;
		}
		m->used = false;
	}
}

/* Ceba la superficie de la ranura con lo que hay en la RDRAM (lo que el
 * CPU dibujó o dejó ahí): la N64 siempre dibuja encima de la memoria. Solo
 * color images de 16 bits que caben en el búfer de cebado. */
static void prime_rows(int i, uint32_t y0, uint32_t y1);
static void prime(int i)
{
	prime_rows(i, 0, ~0u);
}

/* Suma de muestras (32 bits cada 64 bytes) de la RDRAM [lo, hi). */
static uint32_t rdram_sum(uint32_t lo, uint32_t hi)
{
	uint32_t sum = 0;
	if (hi > rdp.rdram_size)
		hi = rdp.rdram_size;
	for (uint32_t o = lo & ~3u; o + 4 <= hi; o += 64) {
		uint32_t v;
		memcpy(&v, rdp.rdram + o, 4);
		sum = (sum << 5 | sum >> 27) ^ v;
	}
	return sum;
}

/* El cebado de las filas [y0, y1) (y1 = ~0u: todas). */
static void prime_rows(int i, uint32_t y0, uint32_t y1)
{
	const struct rdp_target *t = &rdp.tg.t[i];
	/* Las capas de 8 bits (big8) también: el índice entero en XRGB8888,
	 * como lo devuelve su copia. Las otras de 8 bits (sombras de Conker,
	 * cobertura de Zelda) las rehace el RDP enteras y no se ceban. */
	bool x8 = big8(t->bpp, t->lw) && rdp.big8_prime;
	if (!rdp.rdram || rdp.fbfx < 1 || (t->bpp != 2 && !x8) || t->lw > RB_MAX_W || t->lh > RB_MAX_H)
		return;
	uint32_t line = t->width * t->bpp, bytes = line * t->lh;
	if (t->addr + bytes > rdp.rdram_size)
		return;
	if (x8 && rb_pitch(t->bpp, t->lw) * t->lh > RB_SLOT)
		return;
	if (y1 > t->lh)
		y1 = t->lh;
	if (y0 >= y1)
		return;
	/* Búfer de la rotación: si el RSX aún no leyó el cebado que lo usó, se
	 * espera su barrera (casi nunca: son PRIME_BUFS). */
	unsigned k = rdp.prime_next;
	uint32_t boff = k ? (RB_SLOTS + 2) * RB_SLOT + MIRRORS * MIRROR_SLOT + (k - 1) * RB_SLOT : RB_SLOTS * RB_SLOT;
	if (rdp.prime_fence[k] && !rsxkit_fence_done(rdp.prime_fence[k])) {
		fence_wait(rdp.prime_fence[k]);
		rdp.stats.prime_waits++;
	}
	uint8_t *dst = rdp.rb + boff;
	uint32_t pitch = x8 ? rb_pitch(t->bpp, t->lw) : (t->lw * 2 + 63) & ~63u;
	uint64_t t0 = tb_now();
	for (uint32_t y = y0; y < y1; y++) {
		if (!x8) {
			memcpy(dst + y * pitch, rdp.rdram + t->addr + y * line, t->lw * 2);
			continue;
		}
		/* XRGB8888 (bytes X, R, G, B) con el índice en los tres canales. */
		const uint8_t *src = rdp.rdram + t->addr + y * line;
		uint8_t *row = dst + y * pitch;
		for (uint32_t x = 0; x < t->lw; x++) {
			row[4 * x] = 0;
			row[4 * x + 1] = row[4 * x + 2] = row[4 * x + 3] = src[x];
		}
	}
	rdp.tb.copy += tb_now() - t0;
	if (rsxkit_target_fill_rows(rdp.surf[i], rdp.rb_off + boff, pitch, t->lw, t->lh, y0, y1,
				    x8 ? RSXKIT_XRGB8888 : RSXKIT_RGBA5551) == RSXKIT_OK) {
		rdp.bound_t = -1;  /* rsxkit_target_fill ligó la superficie sin profundidad */
		rdp.stats.primes++;
		rdp.stats.big8_primes += x8;
		rdp.surf_gen[i] = ++rdp.gen_seq;
		/* La barrera va tras el blit: pasada, el RSX ya leyó el búfer. */
		rdp.prime_fence[k] = rsxkit_fence();
		rdp.prime_next = (k + 1) % PRIME_BUFS;
	}
}

/* Suelta el scratch (se vuelve a crear cuando un dibujo lo pida). */
static void drop_scratch(void)
{
	if (!rdp.scratch)
		return;
	rsxkit_target_destroy(rdp.scratch);
	rdp.scratch = NULL;
	rdp.vram -= rdp.scratch_bytes;
	rdp.scratch_w = rdp.scratch_h = rdp.scratch_bytes = rdp.scratch_scale = 0;
	rdp.scratch_slot = -1;
}

/* Suelta la superficie de una ranura (destruye y descuenta). */
static void drop_surface(int i)
{
	if (rdp.bound_t == i)
		rdp.bound_t = -1;
	if (rdp.scratch_slot == i)
		rdp.scratch_slot = -1;  /* el scratch era de esta superficie */
	if (rdp.again.surf == rdp.surf[i])
		rdp.again.surf = NULL;
	rsxkit_target_destroy(rdp.surf[i]);
	rdp.surf[i] = NULL;
	rdp.surf_dirty[i] = false;
	rdp.vram -= rdp.surf_bytes[i];
	rdp.surf_bytes[i] = 0;
	rdp.surf_scale[i] = 0;
}

/* Hace sitio para `bytes` más bajo el tope RDP_VRAM_BUDGET: primero suelta
 * el scratch si no es de `keep` y después recicla el destino más viejo
 * (menos campos desde que se dibujó), sin tocar `keep`, el presentado en el
 * último campo ni uno dibujado hace RDP_TARGETS_FRESH tareas o menos (puede
 * ser la textura del dibujo que se está armando). false: ni así cabe. Con
 * el peor caso de rdp_vram_worst dentro del tope (rdp_scale_pick lo
 * asegura) nunca llega a reciclar: es la red si cambian el tope o el
 * máximo de superficies. */
static bool make_room(uint32_t bytes, int keep)
{
	if (rdp.vram + rdp.zeta_bytes + bytes > RDP_VRAM_BUDGET && rdp.scratch && rdp.scratch_slot != keep)
		drop_scratch();
	while (rdp.vram + rdp.zeta_bytes + bytes > RDP_VRAM_BUDGET) {
		int victim = -1;
		for (int j = 0; j < RDP_TARGETS_MAX; j++) {
			if (!rdp.surf[j] || j == keep || j == rdp.tg.shown ||
			    rdp.tg.task - rdp.tg.t[j].last_task <= RDP_TARGETS_FRESH)
				continue;
			if (victim < 0 || (int32_t)(rdp.tg.t[j].last_field - rdp.tg.t[victim].last_field) < 0)
				victim = j;
		}
		if (victim < 0)
			return false;
		drop_surface(victim);
		rdp.tg.t[victim].used = false;
		rdp.stats.targets_evicted++;
	}
	return true;
}

/* Filas que cuentan de un destino: las dibujadas si se sabe, si no su alto. */
static inline uint32_t alias_rows(const struct rdp_target *t)
{
	return t->box.y1 ? t->box.y1 : t->lh;
}

/* La última tarea en que el PPE escribió en la RDRAM [lo, hi) en lugar del
 * RDP (ppe_copy), o 0. */
static uint32_t ppe_written(uint32_t lo, uint32_t hi)
{
	uint32_t t = 0;
	for (int k = 0; k < 4; k++)
		if (rdp.ppe_wr[k].task && rdp.ppe_wr[k].lo < hi && lo < rdp.ppe_wr[k].hi &&
		    (int32_t)(rdp.ppe_wr[k].task - t) > 0)
			t = rdp.ppe_wr[k].task;
	return t;
}

/* Anota una escritura del PPE en la RDRAM [lo, hi) en la tarea en curso
 * (get_target ceba desde la RDRAM un destino que quedó atrás). */
static void ppe_wr_note(uint32_t lo, uint32_t hi)
{
	unsigned k = 0;
	while (k < 4 && !(rdp.ppe_wr[k].task == rdp.tg.task && rdp.ppe_wr[k].hi >= lo && rdp.ppe_wr[k].lo <= hi))
		k++;
	if (k == 4) {
		k = rdp.ppe_wr_next;
		rdp.ppe_wr_next = (k + 1) % 4;
		rdp.ppe_wr[k].lo = lo;
		rdp.ppe_wr[k].hi = hi;
	} else {
		rdp.ppe_wr[k].lo = lo < rdp.ppe_wr[k].lo ? lo : rdp.ppe_wr[k].lo;
		rdp.ppe_wr[k].hi = hi > rdp.ppe_wr[k].hi ? hi : rdp.ppe_wr[k].hi;
	}
	rdp.ppe_wr[k].task = rdp.tg.task;
}

/* ¿done dibujó esta color image en la RDRAM en la tarea del juego `s`? */
static bool set_sw8(const struct set *s, uint32_t addr, uint32_t width)
{
	for (uint32_t i = 0; i < s->sw8_n; i++)
		if (s->sw8_addr[i] == addr && s->sw8_width[i] == width)
			return true;
	return false;
}

/* Un rectángulo en modo copia hacia una color image de 16 bits más alta
 * que 480 filas, que no tiene superficie (get_target la ignora): el PPE lo
 * copia en la RDRAM como el RDP. Resident Evil 2 copia así, con BG_COPY de
 * 512 de ancho y 509 filas, el fondo y su profundidad, que después se leen
 * con otro ancho (596): sin esto, el fondo no se restauraba bajo Leon (su
 * estela) y la z del fondo no llegaba. Respeta la guarda contra el CPU. */
static void ppe_copy(const struct gfx_draw *g, const struct set *s, uint32_t addr, uint32_t width)
{
	const struct gfx_tex *gt = &g->tex[0];
	if (gt->where == GFX_TEX_TARGET || !gt->width || !gt->height)
		return;
	const uint8_t *tex = (gt->where == GFX_TEX_CACHE ? rdp.cache : s->tex) + gt->offset;
	const struct gfx_vertex *v = (const struct gfx_vertex *)(s->vtx) + g->h.a;
	float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, s0 = 0, t0 = 0, s1 = 0, t1 = 0;
	for (uint32_t k = 0; k < g->h.b; k++) {
		if (!(v[k].w > 0.0f))
			return;
		float x = v[k].x / v[k].w, y = v[k].y / v[k].w;
		if (x < x0) x0 = x, s0 = v[k].s;
		if (x > x1) x1 = x, s1 = v[k].s;
		if (y < y0) y0 = y, t0 = v[k].t;
		if (y > y1) y1 = y, t1 = v[k].t;
	}
	float sc[4] = { g->scissor[0] / 4.0f, g->scissor[1] / 4.0f, g->scissor[2] / 4.0f, g->scissor[3] / 4.0f };
	int xa = (int)(x0 > sc[0] ? x0 : sc[0]), xb = (int)(x1 < sc[2] ? x1 : sc[2]);
	int ya = (int)(y0 > sc[1] ? y0 : sc[1]), yb = (int)(y1 < sc[3] ? y1 : sc[3]);
	if (xa < 0) xa = 0;
	if (ya < 0) ya = 0;
	if (xb > (int)width) xb = (int)width;
	if (xb <= xa || yb <= ya || addr + (uint32_t)yb * width * 2 > rdp.rdram_size)
		return;
	float dsdx = x1 > x0 ? (s1 - s0) / (x1 - x0) : 0.0f, dtdy = y1 > y0 ? (t1 - t0) / (y1 - y0) : 0.0f;
	uint32_t lo = addr + (uint32_t)ya * width * 2, hi = addr + (uint32_t)yb * width * 2;
	/* El mismo rectángulo con la misma textura que la última vez y la RDRAM
	 * como quedó entonces (el fondo de RE2 se copia igual cada cuadro): no
	 * hace falta volver a escribirlo. Suma de todas las palabras. */
	uint32_t sum = 0;
	for (uint32_t o = lo & ~3u; o + 4 <= hi; o += 4) {
		uint32_t w;
		memcpy(&w, rdp.rdram + o, 4);
		sum = (sum << 3 | sum >> 29) ^ w;
	}
	unsigned slot = (lo >> 10) % PPE_MEMO;
	struct ppe_memo *m = &rdp.ppe_memo[slot];
	bool same = m->lo == lo && m->hi == hi && m->key == gt->key && m->x0 == (uint16_t)xa && m->x1 == (uint16_t)xb &&
		    m->sum == sum;
	struct late_note nt;
	memset(&nt, 0, sizeof nt);
	/* Copia 1:1 (lo normal en BG_COPY): el texel sale de la columna sin
	 * cuentas de punto flotante por píxel. */
	bool one = dsdx > 0.999f && dsdx < 1.001f;
	int tx0 = (int)(((s0 + ((float)xa + 0.5f - x0) * dsdx) * gt->xf[0] + gt->xf[2]) * gt->width);
	for (int y = ya; y < yb && !same; y++) {
		float t = t0 + ((float)y + 0.5f - y0) * dtdy;
		int ty = (int)((t * gt->xf[1] + gt->xf[3]) * gt->height);
		if (ty < 0 || ty >= gt->height)
			continue;
		uint32_t d0 = addr + ((uint32_t)y * width + (uint32_t)xa) * 2, d1 = addr + ((uint32_t)y * width + (uint32_t)xb) * 2;
		bool ok[ROW_LINES];
		bool all = guard_row(d0, d1, ok, &nt.skipped);
		if (!all)
			late_skip_note(&nt, d0, ok, ((d1 - 1) >> RDP_GUARD_SHIFT) - (d0 >> RDP_GUARD_SHIFT) + 1);
		const uint8_t *row = tex + (uint32_t)ty * gt->pitch;
		for (int x = xa; x < xb; x++) {
			int tx;
			if (one) {
				tx = tx0 + (x - xa);
			} else {
				float sx = s0 + ((float)x + 0.5f - x0) * dsdx;
				tx = (int)((sx * gt->xf[0] + gt->xf[2]) * gt->width);
			}
			if (tx < 0 || tx >= gt->width)
				continue;
			const uint8_t *p = row + 4 * tx;
			if (g->atest && p[3] < 128)
				continue;  /* modo copia con prueba de alfa: el bit de alfa en 0 no se escribe */
			uint32_t a = addr + ((uint32_t)y * width + (uint32_t)x) * 2;
			if (!all && !ok[(a >> RDP_GUARD_SHIFT) - (d0 >> RDP_GUARD_SHIFT)])
				continue;
			uint16_t px = (uint16_t)((p[0] >> 3) << 11 | (p[1] >> 3) << 6 | (p[2] >> 3) << 1 | (p[3] >= 128));
			rdp.rdram[a] = (uint8_t)(px >> 8);
			rdp.rdram[a + 1] = (uint8_t)px;
		}
		guard_wrote(d0, d1, all ? NULL : ok);
		nt.lines++;
	}
	if (!same) {
		sum = 0;
		for (uint32_t o = lo & ~3u; o + 4 <= hi; o += 4) {
			uint32_t w;
			memcpy(&w, rdp.rdram + o, 4);
			sum = (sum << 3 | sum >> 29) ^ w;
		}
		m->lo = lo;
		m->hi = hi;
		m->key = gt->key;
		m->x0 = (uint16_t)xa;
		m->x1 = (uint16_t)xb;
		m->sum = sum;
	} else {
		rdp.stats.ppe_copies_same++;
	}
	unsigned k = 0;
	while (k < 4 && !(rdp.ppe_wr[k].task == rdp.tg.task && rdp.ppe_wr[k].hi >= lo && rdp.ppe_wr[k].lo <= hi))
		k++;
	if (k == 4) {
		k = rdp.ppe_wr_next;
		rdp.ppe_wr_next = (k + 1) % 4;
		rdp.ppe_wr[k].lo = lo;
		rdp.ppe_wr[k].hi = hi;
	} else {
		rdp.ppe_wr[k].lo = lo < rdp.ppe_wr[k].lo ? lo : rdp.ppe_wr[k].lo;
		rdp.ppe_wr[k].hi = hi > rdp.ppe_wr[k].hi ? hi : rdp.ppe_wr[k].hi;
	}
	rdp.ppe_wr[k].task = rdp.tg.task;
	rdp.stats.ppe_copies++;
}

/* Ranura (con superficie) de una color image; -1 si el RSX no tiene
 * memoria para una nueva (esa imagen se queda en la copia de la RDRAM). La
 * superficie va a la escala del destino (rdp_target_scale), que queda en
 * surf_scale. */
/* ¿Este destino lleva canal alfa? Los de 16 bits que no son un framebuffer:
 * del ancho del VI no (el cuadro principal no se lee como textura con alfa, y
 * un destino con alfa mezcla en cada dibujo). */
static bool alpha_wanted(const struct rdp_target *t)
{
	return rdp.target_alpha && t->bpp == 2 && t->width != rdp.tg.vi_width;
}

static int get_target(uint32_t addr, uint32_t width, uint32_t fmt, uint32_t siz, uint32_t min_lh)
{
	bool fresh;
	rdp.last_get.slot = -1;
	rdp.last_get.fresh = false;
	/* Una color image imposible (la HLE la sacó de un comando mal
	 * interpretado: Zelda al abrir la pausa manda anchos de 1495 y 3770) no
	 * toca la tabla: reciclaría una ranura buena para nada. */
	enum rdp_cimg_bad bad = rdp_cimg_impossible(addr, width, min_lh, rdp.rsx_safe ? rdp.rdram_size : 0);
	if (bad != RDP_CIMG_OK) {
		rdp.stats.targets_absurd++;
		snprintf(rdp.last_get.how, sizeof rdp.last_get.how, "imposible, ignorada");
		if (bad == RDP_CIMG_RDRAM) {
			LOGN(20, "gfx: color image %06x/%u fuera de la RDRAM (%u KB; alto pedido %u, tarea %u): se ignora", addr,
			     width, rdp.rdram_size >> 10, min_lh, rdp.tg.task);
			diag_note("color image %06x/%u fuera de la RDRAM (alto pedido %u): ignorada", addr, width, min_lh);
		} else {
			LOGN(10, "gfx: color image imposible %06x/%u (alto %u): se ignora", addr, width, min_lh);
		}
		return -1;
	}
	uint32_t bpp = siz == G_IM_SIZ_32b ? 4 : siz == G_IM_SIZ_8b ? 1 : 2;
	uint32_t vi_lh = rdp_vi_ring_min_height(&rdp.vi_ring, addr, width, bpp);
	if (vi_lh > min_lh)
		min_lh = vi_lh;
	if (min_lh > 480) {
		rdp.stats.targets_absurd++;
		snprintf(rdp.last_get.how, sizeof rdp.last_get.how, "imposible, ignorada");
		LOGN(10, "gfx: color image imposible %06x/%u (alto %u): se ignora", addr, width, min_lh);
		return -1;
	}
	struct { bool used; uint32_t addr, width, bpp; } before[RDP_TARGETS_MAX];
	for (int k = 0; k < RDP_TARGETS_MAX; k++) {
		before[k].used = rdp.tg.t[k].used;
		before[k].addr = rdp.tg.t[k].addr;
		before[k].width = rdp.tg.t[k].width;
		before[k].bpp = rdp.tg.t[k].bpp;
	}
	int i = rdp_targets_get(&rdp.tg, addr, width, siz, min_lh, &fresh);
	rdp.last_get.slot = i;
	rdp.last_get.fresh = fresh;
	if (!fresh) {
		/* Sin dibujar desde hace tareas: lo que vale está en la RDRAM. */
		bool stale = rdp.tg.task - rdp.tg.t[i].last_task > RDP_TARGETS_FRESH;
		/* La misma memoria dibujada después con otro ancho (Resident Evil 2:
		 * el fondo se copia a 0x63E000 de 512 de ancho y los personajes van
		 * encima a 596): la superficie de este ancho quedó vieja. La copia
		 * de la otra a la RDRAM se espera (readbacks_wanted), así que se
		 * ceba desde ahí; si no, el fondo no se restauraba bajo el
		 * personaje y quedaba su estela. */
		bool alias = false;
		for (int j = 0; j < RDP_TARGETS_MAX && !stale; j++) {
			const struct rdp_target *o = &rdp.tg.t[j];
			/* Y que cubra casi lo mismo: Link de Zelda (64 de ancho en los
			 * primeros bytes del framebuffer) no vuelve viejo el cuadro. */
			uint32_t ai = rdp.tg.t[i].width * alias_rows(&rdp.tg.t[i]), ao = o->width * alias_rows(o);
			if (j != i && o->used && o->addr == addr && o->width != width && 2u * ao >= ai && 2u * ai >= ao &&
			    (int32_t)(o->last_task - rdp.tg.t[i].last_task) > 0 && o->last_task != rdp.tg.task)
				alias = true;
		}
		{
			const struct rdp_target *t = &rdp.tg.t[i];
			uint32_t wt = ppe_written(t->addr, t->addr + t->width * t->bpp * t->lh);
			if (wt && (int32_t)(wt - t->last_task) > 0)
				alias = true;
		}
		snprintf(rdp.last_get.how, sizeof rdp.last_get.how, stale ? "cebada" : alias ? "cebada (otro ancho)" : "viva");
		if (stale) {
			prime(i);
			rdp.alias_full[i] = false;
		} else if (alias) {
			/* Con la misma RDRAM que en el último cebado entero, solo las
			 * filas dibujadas encima desde entonces (Leon). */
			const struct rdp_target *t = &rdp.tg.t[i];
			uint32_t sum = rdram_sum(t->addr, t->addr + t->width * 2 * t->lh);
			if (rdp.alias_full[i] && sum == rdp.alias_sum[i]) {
				const struct rdp_box *b = &rdp.alias_dirty[i];
				if (b->y1 > b->y0) {
					prime_rows(i, b->y0, b->y1);
					rdp.stats.primes_alias++;
				}
			} else {
				prime(i);
				rdp.alias_full[i] = true;
				rdp.alias_sum[i] = sum;
				rdp.stats.primes_alias++;
			}
			memset(&rdp.alias_dirty[i], 0, sizeof rdp.alias_dirty[i]);
		}
		return i;
	}
	if (before[i].used && (before[i].addr != addr || before[i].width != width))
		snprintf(rdp.last_get.how, sizeof rdp.last_get.how, "nueva, recicla %06x/%u", before[i].addr,
			 before[i].width);
	else if (before[i].used && before[i].bpp != rdp.tg.t[i].bpp)
		snprintf(rdp.last_get.how, sizeof rdp.last_get.how, "nueva, era de %u bits", before[i].bpp * 8);
	else
		snprintf(rdp.last_get.how, sizeof rdp.last_get.how, before[i].used ? "crece" : "nueva");
	if (rdp.surf[i])
		drop_surface(i);
	const struct rdp_target *t = &rdp.tg.t[i];
	uint32_t sc = big8(t->bpp, t->lw) ? 1 : rdp_target_scale(rdp.scale, t->lw, t->lh);
	uint32_t bytes = rdp_surface_bytes(t->lw * sc, t->lh * sc);
	if (make_room(bytes, i))
		rdp.surf[i] = rsxkit_target_create_ex(t->lw * sc, t->lh * sc, alpha_wanted(t) ? RSXKIT_TARGET_ALPHA : 0);
	if (rdp.surf[i] && rsxkit_target_has_alpha(rdp.surf[i]))
		rdp.stats.targets_alpha++;
	if (!rdp.surf[i]) {
		diag_note("destino %06x/%u: sin superficie de %u × %u", addr, width, t->lw * sc, t->lh * sc);
		rdp.stats.targets_failed++;
		LOGN(20, "gfx: destino %06x/%u: el RSX no dio una superficie de %u × %u (%u×; %u KB vivos)", addr, width,
		     t->lw * sc, t->lh * sc, sc, rdp.vram >> 10);
		snprintf(rdp.last_get.how, sizeof rdp.last_get.how, "SIN SUPERFICIE");
		rdp.tg.t[i].used = false;
		return -1;
	}
	rdp.surf_bytes[i] = bytes;
	rdp.surf_scale[i] = sc;
	rdp.alias_full[i] = false;
	memset(&rdp.alias_dirty[i], 0, sizeof rdp.alias_dirty[i]);
	rdp.surf_gen[i] = ++rdp.gen_seq;
	rdp.vram += bytes;
	LOGN(40, "gfx: destino %06x/%u: %u × %u a %u× (%s; alto pedido %u; %u KB vivos)", addr, width, t->lw, t->lh, sc,
	     rdp.last_get.how, min_lh, rdp.vram >> 10);
	diag_note("destino %06x/%u: %u × %u a %u× en la ranura %d (%s; alto pedido %u)", addr, width, t->lw, t->lh, sc, i,
		  rdp.last_get.how, min_lh);
	rdp.surf_dirty[i] = false;
	rdp.tg.t[i].fmt = (uint8_t)fmt;
	rdp.stats.targets_created++;
	prime(i);
	return i;
}

/* Profundidad de la z image `addr` para dibujar en el destino `slot`. Va
 * en píxeles de la superficie de ese destino (su escala: rsxkit_bind la
 * pide al menos tan grande como el destino). Si la z image ya tiene una
 * que no alcanza (la usó un destino de otra escala o de otro tamaño), se
 * rehace en la misma entrada con el mayor de los dos tamaños: así no quedan
 * dos con la misma dirección (zclear borraría la que no es) ni se destruye
 * y crea a cada cambio de destino. Lo más que crece es lo que pesa el mayor
 * destino posible (rdp_vram_worst). */
static struct depth *get_depth(uint32_t addr, int slot)
{
	const struct rdp_target *t = &rdp.tg.t[slot];
	uint32_t sc = rdp.surf_scale[slot] ? rdp.surf_scale[slot] : 1;
	uint32_t w = t->lw * sc, h = t->lh * sc;
	struct depth *same = NULL, *free_d = NULL;
	for (int i = 0; i < MAX_DEPTHS; i++) {
		struct depth *d = &rdp.depths[i];
		if (d->used && d->addr == addr) {
			if (d->w >= w && d->h >= h)
				return d;
			same = d;
		}
		if (!d->used && !free_d)
			free_d = d;
	}
	struct depth *d = same ? same : free_d ? free_d : &rdp.depths[addr & 1];
	/* Al crecer la profundidad, conserva el borrado que pidió el destino
	 * grande antes de su primer dibujo. LEGO Racers crea primero la de
	 * 32 x 32 y acto seguido la agranda para 320 x 240. */
	uint32_t pending = rdp.zclear_target && same ? same->clear_pending : 0;
	float pending_rect[ZCLEAR_PENDING][4], pending_value[ZCLEAR_PENDING];
	if (pending) {
		memcpy(pending_rect, same->clear_rect, pending * sizeof pending_rect[0]);
		memcpy(pending_value, same->clear_value, pending * sizeof pending_value[0]);
	}
	if (same) {
		w = same->w > w ? same->w : w;
		h = same->h > h ? same->h : h;
	}
	if (d->used) {
		if (rdp.bound_d == d)
			rdp.bound_d = NULL;
		rsxkit_depth_destroy(d->d);
		d->used = false;
		rdp.vram -= d->bytes;
		d->bytes = 0;
	}
	uint32_t bytes = rdp_surface_bytes(w, h);
	d->d = make_room(bytes, slot) ? rsxkit_depth_create(w, h) : NULL;
	diag_note("profundidad %06x: %u × %u para el destino %06x/%u%s", addr, w, h, t->addr, t->width,
		  d->d ? (same ? " (crece)" : "") : ": SIN SUPERFICIE");
	if (!d->d)
		return NULL;
	d->bytes = bytes;
	rdp.vram += bytes;
	d->used = true;
	d->addr = addr;
	d->w = w;
	d->h = h;
	d->clear_pending = pending;
	if (pending) {
		memcpy(d->clear_rect, pending_rect, pending * sizeof d->clear_rect[0]);
		memcpy(d->clear_value, pending_value, pending * sizeof d->clear_value[0]);
	}
	d->rd_sum = d->rd_changed = d->rd_changes = d->rd_checked = 0;
	d->fed_task = d->auto_task = d->fed_src = 0;
	d->up_sum = 0;
	d->full_ok = false;
	memset(&d->zdirty, 0, sizeof d->zdirty);
	for (int k = 0; k < 4; k++)
		if (!rdp.zimg_seen[k] || rdp.zimg_seen[k] == addr || k == 3) {
			rdp.zimg_seen[k] = addr;
			break;
		}
	return d;
}

/* La z que el juego deja en la RDRAM con el CPU (o con un DMA) y que el RDP
 * nunca borra: Resident Evil 2 pone ahí, cuadro a cuadro, la profundidad del
 * fondo prerrenderizado, y sus personajes se prueban contra ella. El RSX
 * solo ve su propia profundidad, así que se carga la de la RDRAM antes del
 * primer dibujo con profundidad de cada tarea, si (1) la z image no se
 * borró con el RDP en las últimas ZFEED_TASKS tareas y (2) su contenido en
 * la RDRAM cambió al menos dos veces, la última hace menos de ZFEED_TASKS.
 * Rogue Squadron (que borra con el RDP) y los juegos cuya z image nadie
 * toca en la RDRAM no pasan por aquí. */
#define ZFEED_TASKS 120u
static bool zfeed(struct depth *d, int slot)
{
	const struct rdp_target *t = &rdp.tg.t[slot];
	uint32_t task = rdp.tg.task;
	if (!d || !d->d || !rdp.rdram)
		return false;
	if (d->rd_checked == task)
		return d->fed_task == task;
	d->rd_checked = task;
	uint32_t width = t->width, rows = t->lh;
	if (!width || width > RB_MAX_W || rows > RB_MAX_H || d->addr + width * rows * 2 > rdp.rdram_size)
		return false;
	/* Una muestra de 32 bits cada 64 bytes: barato y ve cualquier fondo nuevo. */
	const uint8_t *z = rdp.rdram + d->addr;
	uint32_t sum = 0, bytes = width * rows * 2;
	for (uint32_t o = 0; o + 4 <= bytes; o += 64) {
		uint32_t v;
		memcpy(&v, z + o, 4);
		sum = (sum << 5 | sum >> 27) ^ v;
	}
	if (sum != d->rd_sum) {
		d->rd_sum = sum;
		d->rd_changed = task;
		d->rd_changes++;
	}
	/* La z escrita como color en una tarea anterior (Resident Evil 2: la
	 * profundidad del fondo copiada con BG_COPY de 512 de ancho a la z
	 * image, que los personajes leen a 328): su copia a la RDRAM ya se
	 * esperó (readbacks_wanted), así que basta cargarla. */
	uint32_t src = 0;
	for (int i = 0; i < RDP_TARGETS_MAX; i++) {
		const struct rdp_target *c = &rdp.tg.t[i];
		if (c->used && c->addr == d->addr && c->last_task < task && c->last_task > d->fed_src &&
		    c->last_task > src)
			src = c->last_task;
	}
	{
		uint32_t wt = ppe_written(d->addr, d->addr + width * rows * 2);
		if (wt && (int32_t)(wt - d->fed_src) > 0 && (int32_t)(wt - src) > 0)
			src = wt;
	}
	if (!src) {
		for (int k = 0; k < 4; k++)
			if (rdp.zc_addr[k] == d->addr && task - rdp.zc_task[k] < ZFEED_TASKS)
				return false;
		if (d->rd_changes < 2 || task - d->rd_changed >= ZFEED_TASKS)
			return false;
	}
	uint32_t sc = rdp.surf_scale[slot] ? rdp.surf_scale[slot] : 1;
	uint32_t lw = t->lw < width ? t->lw : width;
	if (lw * sc > d->w || rows * sc > d->h)
		return false;
	/* Solo las filas que hace falta: con la misma z en la RDRAM que en la
	 * última carga entera (el fondo de RE2 se vuelve a copiar igual cada
	 * cuadro), las que los personajes tocaron con su z desde entonces. */
	uint32_t ry0 = 0, ry1 = rows;
	bool partial = d->full_ok && sum == d->up_sum && !d->clear_pending;
	if (partial) {
		ry0 = d->zdirty.y0 < rows ? d->zdirty.y0 : rows;
		ry1 = d->zdirty.y1 < rows ? d->zdirty.y1 : rows;
		if (ry1 <= ry0) {
			d->fed_task = task;  /* nada que restaurar */
			if (src)
				d->fed_src = src;
			memset(&d->zdirty, 0, sizeof d->zdirty);
			return true;
		}
	}
	if (rdp.zup_fence)
		fence_wait(rdp.zup_fence);
	uint8_t *dst = rdp.rb + ZUP_OFF;
	uint64_t t0 = tb_now();
	for (uint32_t y = ry0; y < ry1; y++) {
		const uint8_t *src = z + y * width * 2;
		uint32_t *out = (uint32_t *)(dst + y * ZUP_PITCH);
		for (uint32_t x = 0; x < lw; x++)
			out[x] = rdp_z16_to_z24((uint16_t)(src[2 * x] << 8 | src[2 * x + 1])) << 8;
	}
	/* Que el RSX lea lo escrito y no líneas de la caché del PPE. */
	for (uint32_t o = ry0 * ZUP_PITCH; o < ry1 * ZUP_PITCH; o += 128)
		__asm__ volatile("dcbst 0,%0" ::"r"(dst + o) : "memory");
	__asm__ volatile("sync" ::: "memory");
	rdp.tb.copy += tb_now() - t0;
	diag_note("z %06x desde la RDRAM: %u × %u (filas %u a %u) a %u× en %u × %u%s", d->addr, lw, ry1 - ry0, ry0, ry1, sc,
		  d->w, d->h, src ? " (la z se dibujó como color)" : "");
	if (rsxkit_depth_load(d->d, rdp.rb_off + ZUP_OFF + ry0 * ZUP_PITCH, ZUP_PITCH, lw, ry1 - ry0, ry0, sc) == RSXKIT_OK) {
		rdp.zup_fence = rsxkit_fence();
		rdp.bound_t = -1;
		rdp.bound_d = NULL;
		/* Lo que estaba pendiente de borrar ya lo pisó la carga (entera). */
		d->clear_pending = 0;
		d->full_ok = true;
		d->up_sum = sum;
		memset(&d->zdirty, 0, sizeof d->zdirty);
		if (partial)
			rdp.stats.z_uploads_partial++;
		rdp.stats.z_uploads++;
		d->fed_task = task;
		if (src)
			d->fed_src = src;
		LOGN(5, "gfx: z image %06x/%u cargada desde la RDRAM (%u filas a %u×): el RDP no la borra y el CPU la cambia",
		     d->addr, width, rows, sc);
		return true;
	}
	return false;
}

/* Una z image que el RDP nunca borra entera: Rogue Squadron borra solo lo
 * que queda debajo del horizonte (y nada si solo se ve el cielo), y lo de
 * arriba guarda la z de la nave de cuadros anteriores. En la N64 la prueba
 * de z tiene el margen del dz y la nave, siempre a la misma distancia,
 * pasa contra su propia z vieja; con la prueba exacta del RSX se perdía a
 * pedazos al girar (en el host igual: con la z borrada en cada tarea sale
 * entera; y con la prueba de la N64, con el margen de dos veces la
 * pendiente, también). En una tarea que empieza un cuadro (su primera color
 * image no es la de la tarea anterior: Rogue dibuja cada cuadro en otro de
 * sus tres framebuffers) y que antes de su primer dibujo con profundidad no
 * borró la z image entera (ni la cargó desde la RDRAM), se borra entera.
 * Las tareas que siguen un cuadro (la misma color image) conservan la z;
 * los juegos que la borran entera cada cuadro no pasan por aquí, y los que
 * la borran con el CPU quedan bien (borrada). */
static void zauto(struct depth *d)
{
	uint32_t task = rdp.tg.task;
	if (!d || !d->d || d->auto_task == task || !rdp.frame_start)
		return;
	d->auto_task = task;
	for (int k = 0; k < 4; k++)
		if (rdp.zc_addr[k] == d->addr && rdp.zc_full[k] == task)
			return;
	/* Primero, antes de los borrados que la tarea ya dejó pendientes. */
	uint32_t n = d->clear_pending < ZCLEAR_PENDING ? d->clear_pending : ZCLEAR_PENDING - 1;
	memmove(d->clear_rect[1], d->clear_rect[0], n * sizeof d->clear_rect[0]);
	memmove(&d->clear_value[1], &d->clear_value[0], n * sizeof d->clear_value[0]);
	d->clear_rect[0][0] = d->clear_rect[0][1] = 0.0f;
	d->clear_rect[0][2] = d->clear_rect[0][3] = 4096.0f;
	d->clear_value[0] = 1.0f;
	d->clear_pending = n + 1;
	d->full_ok = false;
	if (rdp.bound_d == d) {
		rdp.bound_t = -1;  /* bind aplica los pendientes al volver a ligarla */
		rdp.bound_d = NULL;
	}
	rdp.stats.z_auto++;
}

/* Liga el destino de la ranura con la profundidad `d` (NULL: sin z image,
 * con la de la pantalla, que apply_scale dejó del tamaño del mayor destino).
 * false si rsxkit no lo ligó (ninguna zeta lo cubre): el dibujo se salta en
 * vez de ir con una zeta más chica que el color. */
static bool bind(int slot, struct depth *d)
{
	if (slot == rdp.bound_t && d == rdp.bound_d && !(d && d->clear_pending))
		return true;
	const struct rdp_target *t = &rdp.tg.t[slot];
	int rc = rsxkit_bind(rdp.surf[slot], d ? d->d : NULL, (float)t->lw, (float)t->lh);
	if (rc != RSXKIT_OK) {
		rdp.bound_t = -1;
		rdp.bound_d = NULL;
		rdp.stats.binds_failed++;
		LOGN(10, "gfx: destino %06x/%u no se ligó (%d): ninguna profundidad cubre su superficie", t->addr, t->width,
		     rc);
		return false;
	}
	rdp.bound_t = slot;
	rdp.bound_d = d;
	if (d && d->clear_pending) {
		for (uint32_t i = 0; i < d->clear_pending; i++)
			rsxkit_clear(RSXKIT_CLEAR_DEPTH, 0, d->clear_value[i], d->clear_rect[i]);
		d->clear_pending = 0;
	}
	return true;
}

/* Borrado del z-buffer: se aplica ahora si está ligado, o al ligarlo. */
static void zclear(const struct gfx_zclear *z)
{
	struct depth *d = NULL;
	for (int i = 0; i < MAX_DEPTHS; i++)
		if (rdp.depths[i].used && rdp.depths[i].addr == z->h.a)
			d = &rdp.depths[i];
	float rect[4] = { z->rect[0], z->rect[1], z->rect[2], z->rect[3] };
	/* Z de 18 bits de la N64 en el valor de relleno (14 bits + dz). */
	float value = (float)(z->h.b >> 2) / 16383.0f;
	rdp.stats.zclears++;
	for (int i = 0; i < MAX_DEPTHS; i++)
		if (rdp.depths[i].used && rdp.depths[i].addr == z->h.a)
			rdp.depths[i].full_ok = false;
	{
		int k = 0, old = 0;
		while (k < 4 && rdp.zc_addr[k] != z->h.a)
			k++;
		if (k == 4) {
			for (int j = 1; j < 4; j++)
				if (rdp.zc_task[j] < rdp.zc_task[old])
					old = j;
			k = old;
		}
		if (rdp.zc_addr[k] != z->h.a)
			rdp.zc_full[k] = 0;
		rdp.zc_addr[k] = z->h.a;
		rdp.zc_task[k] = rdp.tg.task;
		uint16_t *b = rdp.zc_box[k];
		if (rdp.zc_box_task[k] != rdp.tg.task) {
			rdp.zc_box_task[k] = rdp.tg.task;
			memcpy(b, z->rect, sizeof z->rect);
		} else {
			b[0] = z->rect[0] < b[0] ? z->rect[0] : b[0];
			b[1] = z->rect[1] < b[1] ? z->rect[1] : b[1];
			b[2] = z->rect[2] > b[2] ? z->rect[2] : b[2];
			b[3] = z->rect[3] > b[3] ? z->rect[3] : b[3];
		}
		/* Toda la imagen: desde la fila y la columna 0, a todo el ancho y
		 * hasta 200 filas o más (el alto no viene en el borrado). */
		if (!b[0] && !b[1] && b[2] >= (z->h.c & 0xFFFF) && b[3] >= 200)
			rdp.zc_full[k] = rdp.tg.task;
	}
	if (d && d == rdp.bound_d && !rdp.zclear_target) {
		rsxkit_clear(RSXKIT_CLEAR_DEPTH, 0, value, rect);
		return;
	}
	if (d) {
		uint32_t i = d->clear_pending;
		rdp.stats.zclears_pending++;
		if (i == ZCLEAR_PENDING) {
			rdp.stats.zclears_merged++;
			/* Sin lugar: el último crece hasta cubrir también este. */
			i--;
			float *r = d->clear_rect[i];
			rect[0] = rect[0] < r[0] ? rect[0] : r[0];
			rect[1] = rect[1] < r[1] ? rect[1] : r[1];
			rect[2] = rect[2] > r[2] ? rect[2] : r[2];
			rect[3] = rect[3] > r[3] ? rect[3] : r[3];
		} else {
			d->clear_pending++;
		}
		memcpy(d->clear_rect[i], rect, sizeof rect);
		d->clear_value[i] = value;
	}
	/* Si aún no existe, nacerá en 1 (borrada). */
}

/* Si la textura viene de un destino dibujado hace poco, se lee de su
 * superficie (a la escala de ese destino, que puede no ser la del destino
 * en que se dibuja: u y v van normalizadas, así que solo las filas ligadas
 * dependen de la escala, rdp_target_tex_map) en vez de la copia
 * decodificada de la RDRAM (*bound). Devuelve true si hay que sincronizar
 * antes del dibujo (la superficie se dibujó desde la última vez). */
static bool texture_from_target(const struct gfx_tex *gt, int cur, rsxkit_texture *t, bool *bound)
{
	*bound = false;
	/* De 16 bits, o una I8 de una color image de 8 bits que la HLE dejó
	 * para leer de ahí (las otras de 8 bits no se leen igual del RSX). */
	uint32_t bpp = gt->src_siz == 2u ? 2 : gt->src_siz == 1u && gt->where == GFX_TEX_TARGET ? 1 : 0;
	if (rdp.fbfx < 1 || !gt->src_addr || !bpp)
		return false;
	int slot;
	uint32_t x0, y0;
	enum rdp_texsrc src = rdp_targets_texture(&rdp.tg, gt->src_addr, gt->src_pitch, bpp, cur, &slot, &x0, &y0);
	if ((src != RDP_TEXSRC_RSX && src != RDP_TEXSRC_SELF) || !rdp.surf[slot])
		return false;
	const struct rdp_target *tg = &rdp.tg.t[slot];
	/* Una textura que se repite a lo ancho no cabe en filas enteras. */
	if (GFX_TEX_WRAP_S(gt->flags) != GFX_WRAP_CLAMP && gt->width != tg->lw)
		return false;
	const rsxkit_target *from = rdp.surf[slot];
	uint32_t sc = rdp.surf_scale[slot];
	bool copied = false;
	if (src == RDP_TEXSRC_SELF) {
		/* Lee lo que está dibujando: una copia de la superficie tal como
		 * está ahora (una por tarea; las franjas de Zelda leen cada una
		 * filas que aún no reescribió). El scratch toma el tamaño, y con él
		 * la escala, de la superficie copiada. */
		/* Otra vez si desde la copia se dibujó donde lee la textura: Conker
		 * usa la misma imagen de 8 bits para la sombra de cada personaje y
		 * desenfoca cada silueta leyendo lo que acaba de dibujar. Las
		 * franjas de Zelda leen filas que aún no reescribieron: una copia. */
		if (rdp.scratch_slot != slot || rdp_box_overlaps(&rdp.scratch_dirty, x0, y0, x0 + gt->width, y0 + gt->height)) {
			uint32_t w, h;
			rsxkit_target_size(rdp.surf[slot], &w, &h);
			if (rdp.scratch && (rdp.scratch_w != w || rdp.scratch_h != h ||
					    rsxkit_target_has_alpha(rdp.scratch) != rsxkit_target_has_alpha(rdp.surf[slot])))
				drop_scratch();
			if (!rdp.scratch) {
				uint32_t bytes = rdp_surface_bytes(w, h);
				if (make_room(bytes, slot))
					rdp.scratch = rsxkit_target_create_ex(w, h, rsxkit_target_has_alpha(rdp.surf[slot]) ? RSXKIT_TARGET_ALPHA : 0);
				if (rdp.scratch) {
					rdp.scratch_w = w;
					rdp.scratch_h = h;
					rdp.scratch_bytes = bytes;
					rdp.vram += bytes;
				}
			}
			if (!rdp.scratch || rsxkit_target_copy(rdp.scratch, rdp.surf[slot]) != RSXKIT_OK)
				return false;
			rdp.bound_t = -1;  /* rsxkit_target_copy dejó ligado el scratch */
			rdp.scratch_slot = slot;
			memset(&rdp.scratch_dirty, 0, sizeof rdp.scratch_dirty);
			rdp.scratch_scale = sc;
			copied = true;
		}
		from = rdp.scratch;
		sc = rdp.scratch_scale;
	}
	struct rdp_tex_map map;
	rdp_target_tex_map(sc, tg->lw, x0, y0, gt->width, gt->height, gt->xf[0], gt->xf[2], &map);
	rsxkit_texture nt;
	rsxkit_target_texture(from, map.row0, map.rows, &nt);
	if (!nt.height)
		return false;
	if (rsxkit_target_has_alpha(from))
		rdp.stats.tex_alpha++;
	t->offset = nt.offset;
	t->pitch = nt.pitch;
	t->width = nt.width;
	t->height = nt.height;
	t->location = nt.location;
	t->format = nt.format;
	/* Una I8: la intensidad que el RDP escribió en la imagen es el rojo. */
	t->remap = bpp == 1 ? RSXKIT_REMAP_RRRR : RSXKIT_REMAP_NONE;
	t->wrap_s = RSXKIT_WRAP_CLAMP;
	/* u recorre la fila entera del destino: la textura empieza en x0. */
	t->xf[0] = map.xf0;
	t->xf[2] = map.xf2;
	*bound = true;
	/* El primer dibujo que lee el scratch recién copiado también tiene que
	 * esperar al RSX e invalidar su caché de texturas (si no, un cuadro con
	 * basura: el haz verde al abrir la pausa de Zelda en la PS3). */
	return copied || (src == RDP_TEXSRC_RSX && rdp.surf_dirty[slot]);
}

/* G_RM_VISCVG: en vez de dibujar, deja en la RDRAM «cobertura plena» en
 * las filas del rectángulo (el CPU de Zelda salta los píxeles cubiertos
 * del todo al filtrar el fondo de la pausa). */
static void viscvg(int cur, const struct gfx_draw *g)
{
	const struct rdp_target *t = &rdp.tg.t[cur];
	if (!rdp.rdram || rdp.fbfx < 2 || t->bpp != 2)
		return;
	rdp.rb_task[cur] = rdp.tg.task;
	uint32_t y0 = g->scissor[1] / 4, y1 = (g->scissor[3] + 3) / 4, x0 = g->scissor[0] / 4, x1 = (g->scissor[2] + 3) / 4;
	if (y1 > t->lh)
		y1 = t->lh;
	if (x1 > t->width)
		x1 = t->width;
	uint64_t t0 = tb_now();
	struct late_note nt = {0};
	bool ok[ROW_LINES];
	for (uint32_t y = y0; y < y1 && x1 > x0; y++) {
		uint32_t a = t->addr + (y * t->width + x0) * 2, e = a + (x1 - x0) * 2;
		if (e > rdp.rdram_size)
			break;
		uint32_t nl = ((e - 1) >> RDP_GUARD_SHIFT) - (a >> RDP_GUARD_SHIFT) + 1;
		nt.lines += nl;
		if (guard_row(a, e, ok, &nt.skipped)) {
			memset(rdp.rdram + a, 0xFF, e - a);
			guard_wrote(a, e, NULL);
			continue;
		}
		/* Solo las líneas que el CPU no cambió desde que terminó la tarea. */
		late_skip_note(&nt, a, ok, nl);
		for (uint32_t i = 0; i < nl && i < ROW_LINES; i++) {
			uint32_t la = ((a >> RDP_GUARD_SHIFT) + i) << RDP_GUARD_SHIFT;
			uint32_t s0 = la > a ? la : a, s1 = la + RDP_GUARD_LINE < e ? la + RDP_GUARD_LINE : e;
			if (ok[i])
				memset(rdp.rdram + s0, 0xFF, s1 - s0);
		}
		guard_wrote(a, e, ok);
	}
	if (y1 > y0 && x1 > x0) {
		struct rdp_box box = {.x0 = (uint16_t)x0, .y0 = (uint16_t)y0, .x1 = (uint16_t)x1, .y1 = (uint16_t)y1};
		late_log("VISCVG", t->addr, t->width, 2, &box, rdp.translated + 1, &nt);
	}
	rdp.tb.copy += tb_now() - t0;
}

/* Una textura que la HLE dejó para leer de un destino (GFX_TEX_TARGET) y
 * que no se pudo leer de él (para la tabla el destino es viejo, es del CPU
 * o no tiene superficie): se decodifica de la RDRAM en el arena de la
 * tarea, detrás de lo que escribió la HLE (el arena casi no se usa con la
 * caché de texturas). false si no cabe. */
static bool texture_from_rdram(const struct gfx_tex *gt, struct set *s, rsxkit_texture *t)
{
	uint32_t off = (rdp.ppe_tex_off + 127) & ~127u, size = (uint32_t)gt->pitch * gt->height;
	if (!rdp.rdram || off + size > TEX_BYTES) {
		rdp.stats.tex_ppe_lost++;
		LOGN(10, "gfx: textura %ux%u desde %06x/%u: no cabe en el arena para decodificarla en el PPE", gt->width,
		     gt->height, gt->src_addr, gt->src_pitch);
		return false;
	}
	uint64_t t0 = tb_now();
	if (gt->src_siz == 1u)
		rdp_decode_i8(rdp.rdram, rdp.rdram_size, gt->src_addr, gt->src_pitch, gt->width, gt->height, s->tex + off,
			      gt->pitch);
	else
		rdp_decode_rgba16(rdp.rdram, rdp.rdram_size, gt->src_addr, gt->src_pitch, gt->width, gt->height,
				  s->tex + off, gt->pitch);
	rdp.tb.tex += tb_now() - t0;
	rdp.ppe_tex_off = off + size;
	t->offset = s->tex_off + off;
	rdp.stats.tex_ppe++;
	return true;
}

/* Algún dibujo traducido leyó una textura de un destino (o de la RDRAM por
 * venir de uno): esa tarea no se interpola (redibujada más tarde, leería lo
 * que el destino tenga entonces). Lo baja translate. */
static bool to_rsxkit_fb;

/* false: el dibujo no se puede hacer (una textura que no hubo dónde
 * decodificar). */
static bool to_rsxkit(const struct gfx_draw *g, struct set *s, int cur, rsxkit_draw *d, bool *sync)
{
	*sync = false;
	static const uint8_t blend[] = {
		[GFX_BLEND_OFF] = RSXKIT_BLEND_OFF,       [GFX_BLEND_ALPHA] = RSXKIT_BLEND_ALPHA,
		[GFX_BLEND_ADD] = RSXKIT_BLEND_ADD,       [GFX_BLEND_KEEP] = RSXKIT_BLEND_NONE,
		[GFX_BLEND_MUL] = RSXKIT_BLEND_MUL,       [GFX_BLEND_ADD_MUL] = RSXKIT_BLEND_ADD_MUL,
		[GFX_BLEND_ALPHA_INV] = RSXKIT_BLEND_ALPHA_INV, [GFX_BLEND_ADD_ONE] = RSXKIT_BLEND_ADD_ONE,
	};
	static const uint8_t wrap[] = {
		[GFX_WRAP_CLAMP] = RSXKIT_WRAP_CLAMP, [GFX_WRAP_REPEAT] = RSXKIT_WRAP_REPEAT,
		[GFX_WRAP_MIRROR] = RSXKIT_WRAP_MIRROR, [GFX_WRAP_MIRROR_ONCE] = RSXKIT_WRAP_MIRROR_ONCE,
	};
	memset(d, 0, offsetof(rsxkit_draw, k));
	d->blend = g->blend < sizeof blend ? blend[g->blend] : RSXKIT_BLEND_OFF;
	d->cull = g->cull;
	d->depth = g->depth;
	if (g->depth & GFX_DEPTH_PRIM)
		d->z = gfx_prim_z((const struct gfx_vertex *)(s->vtx) + g->h.a, g->h.b);
	d->alpha_ref = g->atest ? (g->aref ? g->aref : 1) : 0;
	d->ntex = g->ntex > 2 ? 2 : g->ntex;
	d->fog = g->fog;
	/* Con AA_EN el RDP escribe los píxeles que el triángulo cubre en parte
	 * y así tapa las grietas de las uniones en T (el piso del túnel de Dam
	 * en GoldenEye: el borrado azul se veía por ellas); el RSX solo mira el
	 * centro. Las aristas como líneas antes de los triángulos las tapan a
	 * cualquier escala (docs/rsx-notes.md). Sin calcomanías: sus líneas no
	 * llevarían el desplazamiento de polígono de la calcomanía. */
	if (rdp.aa_edges && g->aa && !(g->depth & GFX_DEPTH_DECAL)) {
		/* 1,5 píxeles de la N64 a la escala de la superficie, en octavos:
		 * las grietas de los dos volcados de ares del túnel de Dam se tapan
		 * desde 1,5 (con 1 quedaban tiras de ~1 px entre aristas casi
		 * paralelas); medido con el rasterizador del host, raster.c. */
		uint32_t sc = cur >= 0 && rdp.surf_scale[cur] ? rdp.surf_scale[cur] : 1;
		d->edges = (uint8_t)(12u * sc > 255u ? 255u : 12u * sc);
	}
	d->nterms = g->nterms;
	memcpy(d->factors, g->factors, sizeof d->factors);
	for (int i = 0; i < 4; i++) {
		d->scissor[i] = g->scissor[i] / 4.0f;
		d->fog_color[i] = ((g->fog_color >> (24 - 8 * i)) & 0xFF) / 255.0f;
	}
	for (int i = 0; i < d->ntex; i++) {
		const struct gfx_tex *gt = &g->tex[i];
		rsxkit_texture *t = &d->tex[i];
		t->offset = (gt->where == GFX_TEX_CACHE ? rdp.cache_off : s->tex_off) + gt->offset;
		t->pitch = gt->pitch;
		t->width = gt->width;
		t->height = gt->height;
		t->location = RSXKIT_MEM_MAIN;
		t->format = RSXKIT_RGBA8888;
		t->wrap_s = wrap[GFX_TEX_WRAP_S(gt->flags)];
		t->wrap_t = wrap[GFX_TEX_WRAP_T(gt->flags)];
		t->filter = (gt->flags & GFX_TEX_BILINEAR) ? RSXKIT_FILTER_LINEAR : RSXKIT_FILTER_NEAREST;
		/* En una capa de 8 bits (big8) el color es un índice de paleta: el
		 * RDP la dibuja 1:1 y su filtro cae en el centro del texel, pero el
		 * del RSX mezcla una fracción de los vecinos (pocos bits de peso) y
		 * un índice mezclado es otro color: los puntos sueltos en los
		 * bordes de las capas de Yoshi's Story. Con el desplazamiento de
		 * medio texel de la HLE, el más cercano da el mismo texel. */
		if (cur >= 0 && big8(rdp.tg.t[cur].bpp, rdp.tg.t[cur].lw))
			t->filter = RSXKIT_FILTER_NEAREST;
		memcpy(t->xf, gt->xf, sizeof t->xf);
		bool from_target = false;
		if (texture_from_target(gt, cur, t, &from_target))
			*sync = true;
		if (from_target || gt->where == GFX_TEX_TARGET)
			to_rsxkit_fb = true;
		if (gt->where == GFX_TEX_TARGET && !from_target && !texture_from_rdram(gt, s, t))
			return false;
	}
	memcpy(d->k, g->k, sizeof d->k);
	/* Destino con alfa: el bit de alfa que el RDP deja en lo que escribe. En
	 * un relleno, el del color (gfx_draw.aref); en una copia, el del texel
	 * (copia los 16 bits tal cual); en los ciclos 1 y 2, la cobertura y no
	 * el alfa de la mezcla (gfx_draw.cvg_bit: 1, el de la memoria, el del
	 * alfa con CVG_X_ALPHA o el mayor). `alfa-fragmento` vuelve a dejar el
	 * alfa del fragmento en todo, como hasta la v0.2.15 (rsxkit dejaba ese
	 * con RSXKIT_DSTA_ONE): Dr. Mario 64 dibuja los paneles translúcidos de
	 * su fondo con alfa 0,375, los copiaba a la RDRAM con el bit en 0 y los
	 * pega con prueba de alfa: salían en blanco (#62). */
	if (cur >= 0 && rdp.surf[cur] && rsxkit_target_has_alpha(rdp.surf[cur])) {
		static const uint8_t dsta[] = {
			[GFX_CVG_ONE] = RSXKIT_DSTA_ONE, [GFX_CVG_KEEP] = RSXKIT_DSTA_KEEP,
			[GFX_CVG_ALPHA] = RSXKIT_DSTA_FRAG, [GFX_CVG_MAX] = RSXKIT_DSTA_MAX,
		};
		if (g->cycle == G_CYC_FILL) {
			d->dst_alpha = RSXKIT_DSTA_FRAG;
			d->k[RSXKIT_K_ONE][0][3] = (float)g->aref / 255.0f;
			rdp.stats.alpha_fills++;
		} else if (g->cycle == G_CYC_COPY) {
			d->dst_alpha = RSXKIT_DSTA_FRAG;
			rdp.stats.alpha_fills++;
		} else if (!rdp.cvg_alpha) {
			d->dst_alpha = RSXKIT_DSTA_FRAG;
		} else {
			uint8_t m = g->cvg_bit < sizeof dsta ? g->cvg_bit : GFX_CVG_ONE;
			d->dst_alpha = dsta[m];
			rdp.stats.alpha_cvg[m]++;
		}
	}
	return true;
}

/* Color images auxiliares de 16 bits de la tarea (las que no son un
 * framebuffer reciente del VI): se copian a la RDRAM al traducirla. La
 * emulación espera aquí la copia de las que acaban de aparecer (el CPU las
 * suele leer en cuanto llega la interrupción del DP: el fondo de la pausa
 * de Zelda, o Link recién dibujado que el CPU copia a otra parte); las que
 * se repiten en RB_HISTORY tareas van sin esperar (iconos de cada cuadro,
 * la intro de Conker). Solo recorre las cabeceras de los registros. */
/* ¿Algún dibujo de la tarea lee como textura la color image (addr, width)?
 * Si ninguno la lee, el único lector posible es el CPU. Mira el origen de
 * las dos texturas de cada dibujo (gfx_tex.src_addr) contra la imagen con
 * el alto de siempre (240 filas). */
static bool rdp_reads_image(const struct set *s, const struct gfx_result *res, uint32_t addr, uint32_t width)
{
	uint32_t lo = addr, hi = addr + width * 2 * 240;
	uint32_t off = 0;
	while (off + sizeof(struct gfx_cmd) <= res->cmd_bytes) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(s->cmd + off);
		if (c->op == GFX_OP_END || c->size < sizeof *c)
			break;
		if (c->op == GFX_OP_DRAW && c->size >= sizeof(struct gfx_draw)) {
			const struct gfx_draw *g = (const struct gfx_draw *)c;
			for (int i = 0; i < g->ntex && i < 2; i++)
				if (g->tex[i].src_addr >= lo && g->tex[i].src_addr < hi)
					return true;
		}
		off += c->size;
	}
	return false;
}

void salsa64_rdp_set_big8_wait(bool wait)
{
	rdp.big8_wait = wait;
}

void salsa64_rdp_set_copy_alpha(bool on)
{
	rdp.copy_alpha = on;
}

void salsa64_rdp_set_cvg_alpha(bool on)
{
	rdp.cvg_alpha = on;
}

void salsa64_rdp_set_big8_prime(bool prime)
{
	rdp.big8_prime = prime;
}

void salsa64_rdp_set_layer8(bool on)
{
	rdp.layer8_on = on;
}

void salsa64_rdp_set_mask32(bool on)
{
	rdp.mask32_on = on;
}

void salsa64_rdp_set_rsx_safe(bool on)
{
	rdp.rsx_safe = on;
}

void salsa64_rdp_zclear_target(bool on)
{
	rdp.zclear_target = on;
}

void salsa64_rdp_set_texture_wait(bool wait)
{
	rdp.texture_wait = wait;
}

/* ¿Hay otra color image en la misma dirección, con otro ancho y de un
 * tamaño parecido (la mitad o más del área de la otra, en las dos
 * direcciones)? Resident Evil 2 copia el fondo a 512 de ancho y lo muestra
 * a 596 o a 320 (el menú de opciones); Link de Zelda (64 de ancho en los
 * primeros bytes del framebuffer) no cuenta. */
static bool alias_of(uint32_t addr, uint32_t width)
{
	uint32_t area = 0;
	for (int j = 0; j < RDP_TARGETS_MAX; j++)
		if (rdp.tg.t[j].used && rdp.tg.t[j].addr == addr && rdp.tg.t[j].width == width)
			area = rdp.tg.t[j].width * alias_rows(&rdp.tg.t[j]);
	if (!area)
		area = width * width * 3u / 4u;
	for (int j = 0; j < RDP_TARGETS_MAX; j++) {
		const struct rdp_target *o = &rdp.tg.t[j];
		uint32_t a = o->width * alias_rows(o);
		if (o->used && o->addr == addr && o->width != width && 2u * a >= area && 2u * area >= a)
			return true;
	}
	return false;
}

static void readbacks_wanted(struct set *s, const struct gfx_result *res)
{
	s->rb_n = s->rb_nsync = 0;
	s->trace = false;
	if (!rdp.rdram || rdp.fbfx < 2) {
		memset(rdp.rb_prev_n, 0, sizeof rdp.rb_prev_n);
		return;
	}
	uint32_t off = 0;
	while (off + sizeof(struct gfx_cmd) <= res->cmd_bytes) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(s->cmd + off);
		if (c->op == GFX_OP_END || c->size < sizeof *c)
			break;
		/* Auxiliar: de otro ancho que el VI (aunque esté en la dirección
		 * del framebuffer: Zelda dibuja a Link en sus primeros bytes con
		 * ancho 64) o, del ancho del VI, fuera de los framebuffers
		 * recientes. */
		uint32_t vi_w = rdp.vi_ring.n ? rdp.vi_ring.cur_width : 0;
		uint32_t siz = c->c & 0xFF;
		/* De 16 bits, o de 8 (la cobertura que Zelda guarda para su filtro,
		 * las máscaras de sombra de Conker): en una de 8 bits el RDP escribe
		 * el rojo, sea cual sea el formato; Conker la declara CI y la lee
		 * como I8. */
		if (c->op == GFX_OP_TARGET && (siz == 2u || siz == 1u) && c->b && c->b <= 640 &&
		    rdp.vi_ring.n && (c->b != vi_w || !rdp_vi_ring_covers(&rdp.vi_ring, c->a, c->b)) &&
		    !set_sw8(s, c->a, c->b)) {
			uint32_t k = 0;
			while (k < s->rb_n && s->rb_addr[k] != c->a)
				k++;
			if (k == s->rb_n && k < RB_PER_TASK) {
				uint32_t rep = 0;  /* tareas seguidas en que se venía pidiendo */
				for (uint32_t h = 0; h < RB_HISTORY; h++)
					for (uint32_t j = 0; j < rdp.rb_prev_n[h]; j++)
						if (rdp.rb_prev[h][j] == c->a)
							rep = rdp.rb_prev_rep[h][j] + 1;
				bool again = rep > 0;
				/* Se espera si acaba de aparecer y, si es chica y ningún
				 * dibujo la lee (Link redibujado cada tarea de la pausa de
				 * Zelda, que el CPU podría copiar), también la segunda vez;
				 * de ahí en adelante va sin esperar y el CPU recibe la de la
				 * tarea anterior (esperarla costaba ~22 ms por tarea). Las
				 * grandes repetidas nunca se esperan (la intro de Conker). */
				bool small = c->b * (siz == 1u ? 1 : 2) * 240 <= MIRROR_BYTES;  /* 64 de ancho sí; 320, no */
				/* Las capas de 8 bits (big8) se esperan siempre: el juego las
				 * lee como CI8 con paleta desde la RDRAM (no desde el RSX), y
				 * la copia sin esperar llegaba después de que la tarea
				 * siguiente decodificara la capa: quedaba la estela de la
				 * capa vieja (Yoshi's Story, las capas que se redibujan en
				 * tareas seguidas). */
				bool zimg = false;
				for (int z = 0; z < 4; z++)
					zimg |= rdp.zimg_seen[z] == c->a;
				/* La misma memoria con otro ancho (get_target la ceba desde
				 * la RDRAM): la copia tiene que estar antes. */
				zimg |= alias_of(c->a, c->b);
				bool b8 = big8(siz == 1u ? 1u : 2u, c->b);
				bool b8w = b8 && rdp.big8_wait;
				if (b8) {
					if (b8w)
						rdp.stats.big8_sync++;
					else
						rdp.stats.big8_nosync++;
				}
				bool sync = !again || b8w || zimg ||
					    (small && rep < 2 && !rdp_reads_image(s, res, c->a, c->b));
				s->rb_rep[k] = rep;
				s->rb_sync[k] = sync;
				s->rb_nsync += sync;
				s->rb_width[s->rb_n] = c->b;
				s->rb_addr[s->rb_n++] = c->a;
			}
		}
		off += c->size;
	}
	if (s->rb_n) {
		if (rdp.quiet_tasks >= 10 && rdp.trace_events < 3) {
			rdp.trace_left = 6;
			rdp.trace_events++;
		}
		rdp.quiet_tasks = 0;
	} else {
		rdp.quiet_tasks++;
	}
	s->trace = rdp.trace_left > 0;
	if (s->trace)
		rdp.trace_left--;
	rdp.rb_prev_i = (rdp.rb_prev_i + 1) % RB_HISTORY;
	memcpy(rdp.rb_prev[rdp.rb_prev_i], s->rb_addr, sizeof rdp.rb_prev[0]);
	memcpy(rdp.rb_prev_rep[rdp.rb_prev_i], s->rb_rep, sizeof rdp.rb_prev_rep[0]);
	rdp.rb_prev_n[rdp.rb_prev_i] = s->rb_n;
}

static void done(void *ctx, const struct gfx_job *job, const struct gfx_result *res)
{
	(void)ctx;
	uint32_t n = rdp.finished;
	struct set *s = &rdp.sets[n % SETS];
	s->res = *res;
	s->dpc_cont = (job->flags & GFX_JOB_DPC_CONT) != 0;
	/* Las capas de 8 bits, en la RDRAM ya mismo (layer8.h): la tarea
	 * siguiente las lee de ahí sin esperar al RSX. */
	s->sw8_n = 0;
	if (rdp.layer8_on && rdp.rdram && rdp.fbfx >= 2) {
		uint64_t t0 = tb_now();
		struct layer8_result lr;
		layer8_run(rdp.rdram, rdp.rdram_size, s->cmd, res->cmd_bytes, (const struct gfx_vertex *)s->vtx, s->tex,
			   rdp.cache, 128, &lr);
		for (uint32_t i = 0; i < lr.n; i++) {
			s->sw8_addr[i] = lr.addr[i];
			s->sw8_width[i] = lr.width[i];
		}
		s->sw8_n = lr.n;
		rdp.stats.layer8_tasks += lr.n > 0;
		rdp.stats.layer8_draws += lr.draws;
		rdp.stats.layer8_pixels += lr.pixels;
		rdp.stats.layer8_refused += lr.refused;
		rdp.stats.layer8_fast_pixels += lr.fast_pixels;
		rdp.stats.layer8_slow_pixels += lr.slow_pixels;
		rdp.stats.tb_layer8_fast += lr.fast_ticks;
		rdp.stats.tb_layer8_slow += lr.slow_ticks;
		rdp.stats.tb_layer8 += tb_now() - t0;
	}
	uint32_t l8n = s->sw8_n;
	/* Las imágenes chicas de 32 bits (la luz de los cuartos oscuros de
	 * Paper Mario, mask32.h), también en la RDRAM ya mismo: la HLE de la
	 * tarea siguiente carga de ahí la máscara. */
	if (rdp.mask32_on && rdp.rdram && rdp.fbfx >= 2) {
		uint64_t t0 = tb_now();
		struct mask32_result mr;
		mask32_run(rdp.rdram, rdp.rdram_size, s->cmd, res->cmd_bytes, (const struct gfx_vertex *)s->vtx, s->tex,
			   rdp.cache, &mr);
		for (uint32_t i = 0; i < mr.n && s->sw8_n < LAYER8_MAX + MASK32_MAX; i++) {
			s->sw8_addr[s->sw8_n] = mr.addr[i];
			s->sw8_width[s->sw8_n] = mr.width[i];
			s->sw8_n++;
		}
		rdp.stats.mask32_tasks += mr.n > 0;
		rdp.stats.mask32_draws += mr.draws;
		rdp.stats.mask32_pixels += mr.pixels;
		rdp.stats.mask32_bytes += mr.bytes;
		rdp.stats.mask32_restored += mr.restored;
		rdp.stats.mask32_refused += mr.refused;
		rdp.stats.tb_mask32 += tb_now() - t0;
	}
	readbacks_wanted(s, res);
	if (l8n && !s->rb_nsync)
		rdp.stats.layer8_waits_avoided++;
	guard_mark_task(s, res, n + 1);
	lwsync();
	rdp.finished = n + 1;
	if (!s->rb_nsync)
		return;
	rdp.rb_waiting = n + 1;
	lwsync();  /* antes de despertar a quien traduce */
	if (rdp.wake)
		rdp.wake();
	uint64_t t0 = sysGetSystemTime();
	rdp.stats.readback_waits++;
	while ((int32_t)(rdp.readback_done - (n + 1)) < 0) {
		sysUsleep(50);
		if (sysGetSystemTime() - t0 > RB_WAIT_US) {
			rdp.stats.readback_late++;
			break;
		}
	}
	rdp.rb_waiting = 0;
	rdp.stats.us_readback_wait += sysGetSystemTime() - t0;
}

static void frame_mirror(int slot, uint32_t rows, bool wait);
static void again_set(int slot, uint32_t y0, uint32_t pw, uint32_t ph, const rsxkit_view *view,
		      const struct salsa64_overscan *crop);

/* Interpolación: empareja los vértices de la tarea `task` con los de la
 * anterior (vmatch.c) y deja sus posiciones de allá detrás de los vértices.
 * Sin pareja (o en un corte), la de ahora. Si la tarea no cupo (tablas,
 * vértices más allá de VTX_INTERP o registros que faltan) no se interpola,
 * no sirve de anterior y acquire deja de pedir la bandera un rato. */
static void interp_task(struct set *s, const struct gfx_result *res, uint32_t task)
{
	uint64_t t0 = sysGetSystemTime();
	uint32_t c = rdp.interp.cur ^ 1;
	struct vmatch_table *cur = &vm[c].t, *prev = rdp.interp.have_prev ? &vm[c ^ 1].t : NULL;
	bool room = res->vtx_bytes <= VTX_INTERP && !(res->flags & GFX_RES_VGROUP_FULL);
	vmatch_build(cur, s->cmd, res->cmd_bytes);
	if (!room)
		cur->full = true;
	if (cur->full)
		rdp.interp.full_task = task;
	uint64_t t1 = sysGetSystemTime();
	struct vmatch_stats st;
	uint32_t nv = res->vtx_bytes / sizeof(struct gfx_vertex);
	vmatch_pair(prev, cur, INTERP_DISP, &st);
	vmatch_atomic(cur, (const struct gfx_vertex *)s->vtx, nv, &st);
	uint64_t t2 = sysGetSystemTime();
	uint32_t paired = room ? vmatch_prev(prev, cur, &st, (const struct gfx_vertex *)s->vtx, nv,
					     (float(*)[4])(s->vtx + VTX_INTERP))
			       : 0;
	uint64_t t3 = sysGetSystemTime();
	rdp.stats.us_interp_build += t1 - t0;
	rdp.stats.us_interp_pair += t2 - t1;
	rdp.stats.us_interp_prev += t3 - t2;
	rdp.interp.cur = c;
	rdp.interp.have_prev = true;
	s->interp_ok = prev && !st.cut && !cur->full && paired;
	if (prev) {
		rdp.stats.interp_tasks++;
		rdp.stats.interp_cuts += st.cut;
		rdp.stats.interp_cuts_camera += st.camera;
		rdp.stats.interp_full += cur->full;
		rdp.stats.interp_vertices += nv;
		rdp.stats.interp_paired += paired;
		rdp.stats.interp_content += st.by_content;
		rdp.stats.interp_rejected += st.jumped + st.deformed + st.collapsed + st.near;
		rdp.stats.interp_reordered += st.reordered;
		rdp.stats.interp_atomic += st.atomic;
		rdp.stats.interp_kept += st.kept;
	}
	if (rdp.interp.mode == SALSA64_INTERP_FIXED && rdp.interp.t < 1.0f && room)
		rsxkit_vertices_prev(s->vtx_off + VTX_INTERP, RSXKIT_MEM_MAIN, rdp.interp.t);
	rdp.stats.us_interp += sysGetSystemTime() - t0;
}

/* Traduce la salida de la tarea n a comandos del RSX. */
/* Borra con `color` el rectángulo (xa, ya)–(xb, yb) (píxeles lógicos) del
 * destino `i`, como un FILLRECT del RDP, y lo cuenta como dibujado en la
 * tarea. false si no hay nada que borrar o no se pudo ligar. */
static bool tall_fill_target(int i, float xa, float ya, float xb, float yb, uint32_t color, bool drawn[RDP_TARGETS_MAX],
			     struct rdp_box drawn_box[RDP_TARGETS_MAX])
{
	const struct rdp_target *t = &rdp.tg.t[i];
	if (yb <= ya || xb <= xa || !rdp.surf[i] || !bind(i, NULL))
		return false;
	float rect[4] = { xa, ya, xb, yb };
	/* Con alfa en el destino: un relleno de un color que no es 0 deja alfa 1. */
	rsxkit_clear(RSXKIT_CLEAR_COLOR, color | (color && rsxkit_target_has_alpha(rdp.surf[i]) ? 0xFF000000u : 0), 0.0f, rect);
	rdp_targets_drawn(&rdp.tg, i);
	rdp.slot_fin[i] = rdp.in_task;
	drawn[i] = true;
	rdp.surf_gen[i] = ++rdp.gen_seq;
	rdp.surf_dirty[i] = true;
	struct rdp_box r = { 0, 0, 0, 0 };
	const uint16_t all[4] = { 0, 0, (uint16_t)(t->lw * 4), (uint16_t)(t->lh * 4) };
	rdp_box_add(&r, xa, ya, xb, yb, true, all, t->lw, t->lh);
	rdp_box_union(&drawn_box[i], &r);
	rdp_box_union(&rdp.tg.t[i].box, &r);
	rdp.stats.tall_fills++;
	return true;
}

/* Copia espejo de una imagen auxiliar chica dibujada en la tarea `n` (nunca
 * presentada, de otro ancho que el VI): el RSX reduce la ranura `i` a un
 * búfer de espejos y la RDRAM la tendrá cuando un dibujo la lea ya vieja
 * (Link en la pantalla de equipo de Zelda, 395 tareas después). No se
 * espera: se convierte en una traducción posterior, cuando el RSX pasó la
 * barrera. Devuelve el espejo usado, o -1 si la ranura no cumple o no hay
 * espejo libre. `box` es lo dibujado en ella en la tarea. */
static int mirror_small(int i, const struct rdp_box *box, uint32_t n, uint32_t limit)
{
	const struct rdp_target *t = &rdp.tg.t[i];
	uint32_t rows = box->y1 < t->lh ? box->y1 : t->lh;
	uint32_t pitch = (t->lw * 2 + 63) & ~63u;
	if (!rdp.rdram || rdp.fbfx < 2 || !rows || t->presented || (t->bpp != 2 && t->bpp != 1) || big8(t->bpp, t->lw) ||
	    t->width == rdp.tg.vi_width || pitch * rows > limit || t->addr + t->width * t->bpp * rows > rdp.rdram_size)
		return -1;
	int k = 0;
	while (k < MIRRORS && rdp.mirrors[k].used)
		k++;
	if (k == MIRRORS) {
		rdp.stats.mirrors_skipped++;
		return -1;
	}
	/* Con el alfa del destino, si la copia de 32 bits cabe en el búfer. */
	bool argb = rb_argb(i) && argb_pitch(t->lw) * rows <= MIRROR_SLOT;
	if (argb)
		pitch = argb_pitch(t->lw);
	rsxkit_rect src = surface_rect(i, 0, 0, t->lw, rows);
	if (rsxkit_target_read(rdp.surf[i], &src, rdp.rb_off + (RB_SLOTS + 2) * RB_SLOT + k * MIRROR_SLOT, pitch, t->lw,
			       rows, argb ? RSXKIT_ARGB8888 : RSXKIT_RGB565) != RSXKIT_OK)
		return -1;
	rdp.bound_t = -1;
	rdp.rb_task[i] = rdp.tg.task;
	struct mirror *m = &rdp.mirrors[k];
	m->used = true;
	m->argb = argb;
	m->addr = t->addr;
	m->width = t->width;
	m->lw = t->lw;
	m->rows = rows;
	m->box = *box;
	m->box.y1 = (uint16_t)rows;
	m->bpp = t->bpp;
	m->task = n + 1;
	return k;
}

static void translate(uint32_t n)
{
	uint64_t t0 = sysGetSystemTime(), tb0 = tb_now();
	struct set *s = &rdp.sets[n % SETS];
	const struct gfx_result *res = &s->res;
	rdp.at = "inicio";
	rdp.in_task = n + 1;
	if (rdp.task_hook) {
		struct gfx_dump_src src = { s->cmd, res->cmd_bytes, s->vtx, res->vtx_bytes, s->tex, res->tex_bytes,
					    rdp.cache, CACHE_BYTES, rdp.rdram, rdp.rdram_size };
		rdp.task_hook(rdp.task_ctx, n, &src, res);
	}
	if (res->flags & GFX_RES_F5_SUB_END)
		LOGN(5, "gfx: tarea %u: una lista secundaria de Factor 5 acabó en un G_ENDDL que no es B8 … FFFFFFFF "
		     "(f5-sublista-vieja seguiría por lo que hay detrás)", n);
	rsxkit_vertices(s->vtx_off, RSXKIT_MEM_MAIN);
	s->interp_ok = false;
	to_rsxkit_fb = false;
	/* Un juego a 60 cuadros no tiene campos intermedios: no se empareja. */
	if (s->interp && !(rdp.interp.mode == SALSA64_INTERP_FIELDS && rdp.interp.last_gap == 1))
		interp_task(s, res, n + 1);
	else
		rdp.interp.have_prev = false;
	rdp.bound_t = -1;
	rdp.bound_d = NULL;
	if (s->dpc_cont) {
		/* Sigue una lista del RDP abierta (LLE, lista del CPU): es la misma tarea,
		 * no envejece a los destinos (si no, tres tramos sin dibujar en uno
		 * bastaban para cebarlo otra vez desde la RDRAM y borrar lo dibujado). */
		rdp.stats.dpc_cont++;
		for (int i = 0; i < RDP_TARGETS_MAX; i++) {
			const struct rdp_target *t = &rdp.tg.t[i];
			if (t->used && t->drawn && rdp.tg.task - t->last_task <= RDP_TARGETS_FRESH &&
			    rdp.tg.task + 1 - t->last_task > RDP_TARGETS_FRESH)
				rdp.stats.dpc_cont_saved++;
		}
	} else {
		rdp_targets_task(&rdp.tg);
	}
	s->tg_task = rdp.tg.task;
	rdp.scratch_slot = -1;
	rdp.ppe_tex_off = res->tex_bytes;
	readbacks_finish(false);
	rsxkit_fp_stats fp0 = { 0 };
	if (perf_hook) {
		memset(&perf_info, 0, sizeof perf_info);
		rsxkit_fp_get_stats(&fp0);
	}
	int cur = -1;
	bool skip8 = false;  /* los dibujos de una capa que done ya puso en la RDRAM */
	bool drawn[RDP_TARGETS_MAX] = { false };
	rdp.at = "dibujos";
	struct rdp_box drawn_box[RDP_TARGETS_MAX];  /* lo que tocaron los dibujos de la tarea */
	memset(drawn_box, 0, sizeof drawn_box);
	int mirror_k[RDP_TARGETS_MAX + MIRRORS];  /* espejos de la tarea (los de ranuras recicladas y los del final) */
	uint32_t nmirror = 0;

	uint32_t off = 0;
	rsxkit_draw rd;
	struct { uint32_t addr, width, bpp, rows; } tall = { 0, 0, 0, 0 };  /* color image más alta que 480 */
	struct { bool on; uint32_t addr, width; } ppe_img = { false, 0, 0 };  /* la de ppe_copy */
	while (off + sizeof(struct gfx_cmd) <= res->cmd_bytes) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(s->cmd + off);
		if (c->op == GFX_OP_END || c->size < sizeof *c)
			break;
		switch (c->op) {
		case GFX_OP_TARGET: {
			uint32_t lh = scissor_bottom(s, off + c->size, res->cmd_bytes), siz = c->c & 0xFF;
			skip8 = set_sw8(s, c->a, c->b);
			if (skip8) {
				/* La capa (o la imagen de 32 bits) ya está en la RDRAM: ni se
				 * dibuja ni se copia; si otra tarea la lleva al RSX,
				 * get_target la ceba de ahí. */
				ppe_wr_note(c->a, c->a + c->b * (siz == 3u ? 4u : siz == 2u ? 2u : 1u) * (lh ? lh : 240u));
				cur = -1;
				break;
			}
			/* Fuera de la RDRAM (rdp_cimg_impossible, con el blindaje): ni
			 * destino, ni reparto de rellenos, ni copias del PPE, ni reciclar
			 * una ranura por ella. */
			bool outside = rdp.rsx_safe && rdp.rdram_size && c->a >= rdp.rdram_size;
			bool is_tall = !outside && c->b && c->b <= 640 && rdp_targets_tall(c->b, lh) &&
				       fills_only(s, off + c->size, res->cmd_bytes);
			rdp.at = "destinos";
			if (rdp.cimg_task != rdp.tg.task) {
				rdp.cimg_task = rdp.tg.task;
				rdp.frame_start = c->a != rdp.first_cimg;
				rdp.first_cimg = c->a;
			}
			for (int k = 0; k < MAX_DEPTHS; k++)
				if (rdp.depths[k].used && rdp.depths[k].addr == c->a) {
					rdp.stats.z_as_color++;
					LOGN(10, "gfx: color image %06x/%u en la dirección de una z image (la z como color: el RSX no la ve)",
					     c->a, c->b);
					break;
				}
			if (!outside && !is_tall && c->b && c->b <= 640 && lh <= 480) {
				/* La ranura va a ser de otra imagen (la recicla este destino o
				 * crece): lo que el RDP dibujó en ella hasta ahora, si es una
				 * imagen auxiliar chica, se copia con SU dirección y SU alto
				 * antes de que cambien; después el juego la usa de textura
				 * (Pokémon Stadium: nueve destinos en una tarea). */
				int v = rdp_targets_recycled(&rdp.tg, c->a, c->b, siz, lh);
				if (v >= 0 && drawn[v] && rdp.surf[v]) {
					int k = mirror_small(v, &drawn_box[v], n, MIRROR_SLOT);
					if (k >= 0)
						mirror_k[nmirror++] = k;
					LOGN(20, "gfx: la ranura %d (%06x/%u) se recicla con lo dibujado hasta la fila %u: %s", v,
					     rdp.tg.t[v].addr, rdp.tg.t[v].width, drawn_box[v].y1, k >= 0 ? "copiada" : "perdida");
				}
			}
			cur = is_tall ? -1 : get_target(c->a, c->b, (c->c >> 8) & 0xFF, siz, lh);
			rdp.at = "dibujos";
			/* Más alta que 480 (sin superficie): los rectángulos de copia van
			 * a la RDRAM con el PPE (ppe_copy). */
			ppe_img.on = cur < 0 && !outside && !is_tall && siz == 2u && c->b && c->b <= 640 && lh > 480 &&
				     rdp.rdram && rdp.fbfx >= 2;
			ppe_img.addr = c->a;
			ppe_img.width = c->b;
			if (cur >= 0 && rdp.last_get.fresh) {
				/* Ranura de otra imagen: lo dibujado y su caja eran del destino
				 * anterior (ver raster.c). */
				drawn[cur] = false;
				memset(&drawn_box[cur], 0, sizeof drawn_box[cur]);
				/* Un relleno de imagen alta anterior (rdp_targets_tall_note) que
				 * este destino no vio: se cebó con la RDRAM, que no lo tiene. */
				struct rdp_tall_rect pend[RDP_TALL_FILLS];
				int np = rdp_targets_tall_pending(&rdp.tg, cur, &rdp.dma, pend);
				for (int k = 0; k < np; k++)
					if (tall_fill_target(cur, pend[k].x0, pend[k].y0, pend[k].x1, pend[k].y1, pend[k].color, drawn,
							     drawn_box))
						LOGN(20, "gfx: destino nuevo %06x/%u: relleno de imagen alta anterior (%.0f,%.0f)–(%.0f,%.0f)",
						     c->a, c->b, pend[k].x0, pend[k].y0, pend[k].x1, pend[k].y1);
			}
			if (is_tall)
				snprintf(rdp.last_get.how, sizeof rdp.last_get.how, "alta, rellenos repartidos");
			tall.rows = is_tall ? lh : 0;
			tall.addr = c->a;
			tall.width = c->b;
			tall.bpp = siz == 3 ? 4 : siz == 2 ? 2 : 1;
			if (s->trace && rdp_log)
				rdp_log("gfx: fb tarea %u: color image %06x/%u siz %u → ranura %d (%s)", n, c->a, c->b,
					c->c & 0xFF, cur, rdp.last_get.how);
			break;
		}
		case GFX_OP_ZCLEAR:
			zclear((const struct gfx_zclear *)c);
			break;
		case GFX_OP_DRAW: {
			const struct gfx_draw *g = (const struct gfx_draw *)c;
			if (skip8)
				break;
			if (g->approx && perf_hook)
				perf_approx(g);
			if (cur < 0 && tall.rows && g->cycle == G_CYC_FILL && g->h.b >= 3) {
				/* Relleno de la imagen alta (rdp_targets_inside): se borra el
				 * rectángulo en cada destino que cae dentro. */
				int slots[RDP_TARGETS_MAX];
				uint32_t first[RDP_TARGETS_MAX];
				const struct gfx_vertex *v = (const struct gfx_vertex *)(s->vtx) + g->h.a;
				float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
				for (uint32_t k = 0; k < g->h.b; k++) {
					if (!(v[k].w > 0.0f))
						continue;
					float x = v[k].x / v[k].w, y = v[k].y / v[k].w;
					x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1;
					y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
				}
				uint32_t color = (uint32_t)(g->k[GFX_K_ONE][0][0] * 255.0f + 0.5f) << 16 |
						 (uint32_t)(g->k[GFX_K_ONE][0][1] * 255.0f + 0.5f) << 8 |
						 (uint32_t)(g->k[GFX_K_ONE][0][2] * 255.0f + 0.5f);
				int nin = rdp_targets_inside(&rdp.tg, tall.addr, tall.width, tall.bpp, tall.rows, slots, first);
				for (int k = 0; k < nin && x1 > x0 && y1 > y0; k++) {
					int i = slots[k];
					const struct rdp_target *t = &rdp.tg.t[i];
					float ya = y0 - (float)first[k], yb = y1 - (float)first[k];
					ya = ya < 0.0f ? 0.0f : ya;
					yb = yb > (float)t->lh ? (float)t->lh : yb;
					float xa = x0 < 0.0f ? 0.0f : x0, xb = x1 > (float)t->lw ? (float)t->lw : x1;
					tall_fill_target(i, xa, ya, xb, yb, color, drawn, drawn_box);
				}
				/* Y para los destinos que se creen después. */
				rdp_targets_tall_note(&rdp.tg, tall.addr, tall.width, tall.bpp, tall.rows, x0, y0, x1, y1, color, slots,
						      nin);
				break;
			}
			if (cur < 0 && ppe_img.on && g->cycle == G_CYC_COPY && g->ntex == 1 && g->h.b >= 3) {
				ppe_copy(g, s, ppe_img.addr, ppe_img.width);
				break;
			}
			if (cur < 0 || !g->h.b)
				break;
			bool sync;
			if (g->viscvg) {
				/* Cobertura plena en la RDRAM y, en la superficie, blanco
				 * (el RDP copia después esa «cobertura» a otra imagen y el
				 * CPU la lee: Link en la pausa de Zelda). */
				viscvg(cur, g);
				struct gfx_draw w = *g;
				w.ntex = 0;
				w.nterms = 0;
				w.blend = GFX_BLEND_OFF;
				w.atest = 0;
				w.fog = 0;
				w.depth = 0;
				memset(w.k, 0, sizeof w.k);
				for (int lane = 0; lane < 4; lane++)
					w.k[GFX_K_ONE][0][lane] = 1.0f;
				to_rsxkit(&w, s, cur, &rd, &sync);
			} else if (!to_rsxkit(g, s, cur, &rd, &sync)) {
				break;
			}
			if (sync) {
				/* Lee una superficie dibujada en este mismo tramo. */
				rsxkit_texture_sync();
				memset(rdp.surf_dirty, 0, sizeof rdp.surf_dirty);
			}
			struct depth *d = rdp.bound_t == cur ? rdp.bound_d : NULL;
			if ((g->depth & (GFX_DEPTH_TEST | GFX_DEPTH_WRITE)) == GFX_DEPTH_WRITE)
				rdp.stats.z_write_only++;
			if (g->depth) {
				rdp.at = "profundidad";
				d = get_depth(g->zimg, cur);
				if (!(rdp.fbfx >= 2 && zfeed(d, cur)))
					zauto(d);
				rdp.at = "dibujos";
			}
			if (!bind(cur, d))
				break;
			rdp_targets_drawn(&rdp.tg, cur);
			rdp.slot_fin[cur] = rdp.in_task;
			drawn[cur] = true;
			rdp.surf_gen[cur] = ++rdp.gen_seq;
			rdp.surf_dirty[cur] = true;
			/* Qué puede tocar el dibujo: la caja de sus vértices dentro de
			 * las tijeras (las copias a la RDRAM escriben solo eso: el alto
			 * del destino es una conjetura y pasarse pisaría el heap del
			 * juego, como detrás del búfer de Link en Zelda; y fuera de las
			 * tijeras hay datos, como en los márgenes del z-buffer de
			 * Smash; targets.h). */
			{
				const struct gfx_vertex *v = (const struct gfx_vertex *)(s->vtx) + g->h.a;
				float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
				bool front = true;
				for (uint32_t k = 0; k < g->h.b; k++) {
					if (!(v[k].w > 0.0f)) {
						front = false;
						continue;
					}
					float x = v[k].x / v[k].w, y = v[k].y / v[k].w;
					x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1;
					y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
				}
				const struct rdp_target *t = &rdp.tg.t[cur];
				struct rdp_box r = { 0, 0, 0, 0 };
				rdp_box_add(&r, x0, y0, x1, y1, front, g->scissor, t->lw, t->lh);
				rdp_box_union(&drawn_box[cur], &r);
				rdp_box_union(&rdp.tg.t[cur].box, &r);
			if (d && (g->depth & GFX_DEPTH_WRITE))
				rdp_box_union(&d->zdirty, &r);
			rdp_box_union(&rdp.alias_dirty[cur], &r);
				if (cur == rdp.scratch_slot)
					rdp_box_union(&rdp.scratch_dirty, &r);
			}
			rsxkit_draw_triangles(&rd, g->h.a, g->h.b);
			rdp.stats.draws++;
			if (rd.edges)
				rdp.stats.draws_edges++;
			if (g->nterms)
				rdp.stats.draws_generated++;
			rdp.stats.triangles += g->h.b / 3;
			break;
		}
		default:
			break;
		}
		off += c->size;
	}
	if (to_rsxkit_fb && s->interp_ok) {
		s->interp_ok = false;
		rdp.stats.interp_fbtex++;
	}
	rdp.at = "copias a la RDRAM";
	/* Copias a la RDRAM que pidió done: el RSX reduce cada destino a su
	 * búfer y, pasada la barrera, el PPE lo convierte y lo escribe. */
	struct { int slot; uint32_t buf, rows; struct rdp_box box; bool argb; } rb[RB_SLOTS];
	uint32_t nrb = 0;
	bool sync_rb[RDP_TARGETS_MAX] = { false };
	uint32_t big_used = 0;  /* búferes grandes tomados en esta tarea */
	for (int k = 0; k < RB_SLOTS; k++)
		big_used |= rdp.big[k].used << k;
	for (uint32_t k = 0; k < s->rb_n; k++) {
		bool handled = false;
		for (int i = 0; i < RDP_TARGETS_MAX; i++) {
			const struct rdp_target *t = &rdp.tg.t[i];
			uint32_t rows = drawn_box[i].y1 < t->lh ? drawn_box[i].y1 : t->lh;
			/* La misma dirección puede ser dos color images de anchos
			 * distintos (Link de Zelda en los primeros bytes del framebuffer):
			 * la pedida es la de ese ancho. */
			if (!t->used || t->addr != s->rb_addr[k] || t->width != s->rb_width[k])
				continue;
			if (!drawn[i] || !rows || (t->bpp != 2 && t->bpp != 1) || t->lw > RB_MAX_W || t->lh > RB_MAX_H ||
			    t->addr + t->width * t->bpp * rows > rdp.rdram_size) {
				if (s->trace && rdp_log)
					rdp_log("gfx: fb tarea %u: copia %06x/%u ranura %d descartada (dibujada %d, %u filas, %u bpp)",
						n, t->addr, t->width, i, drawn[i], rows, t->bpp);
				continue;
			}
			/* Repetida y chica: espejo chico sin esperar (abajo). */
			uint32_t pitch_small = (t->lw * 2 + 63) & ~63u;
			if (!s->rb_sync[k] && t->width != rdp.tg.vi_width && pitch_small * rows <= MIRROR_BYTES && !big8(t->bpp, t->lw)) {
				handled = true;
				break;
			}
			/* Un búfer grande libre. Si los dos están tomados por copias
			 * esperadas de esta misma tarea, su lectura aún no pasó por el
			 * RSX (la barrera va al final): se espera una barrera y se
			 * convierten antes de reutilizar el búfer (antes la tercera
			 * copia esperada de una tarea pisaba la primera). */
			int buf = -1;
			for (int b = 0; b < RB_SLOTS && buf < 0; b++)
				if (!(big_used & (1u << b)))
					buf = b;
			if (buf < 0) {
				if (nrb) {
					fence_wait(rsxkit_fence());
					for (uint32_t j = 0; j < nrb; j++) {
						const struct rdp_target *tj = &rdp.tg.t[rb[j].slot];
						if (dma_newer(tj->addr, tj->width * tj->bpp * rb[j].rows, n + 1))
							continue;
						to_rdram(rdp.rb + rb[j].buf * RB_SLOT, RB_PITCH, tj->addr, tj->width, tj->lw, &rb[j].box,
							 tj->bpp, rb[j].argb, "copia de la tarea", n + 1, 0);
						rdp.stats.readbacks++;
					}
					nrb = 0;
				}
				readbacks_finish(true);
				big_used = 0;
				buf = 0;
			}
			/* La esperada pasa al CPU (la va a leer y quizá cambiar: el
			 * fondo de la pausa de Zelda); la que se repite sin esperar no:
			 * el RDP la sigue leyendo del RSX (targets.h). */
			if (!rdp_targets_readback(&rdp.tg, i, true, s->rb_sync[k]))
				continue;
			/* El RSX reduce a 1× desde la escala de este destino. */
			rsxkit_rect src = surface_rect(i, 0, 0, t->lw, rows);
			bool x8 = big8(t->bpp, t->lw);
			/* Con el alfa del destino (rb_argb), si la copia de 32 bits cabe. */
			bool argb = !x8 && rb_argb(i) && argb_pitch(t->lw) * rows <= RB_SLOT;
			uint32_t pitch = argb ? argb_pitch(t->lw) : rb_pitch(t->bpp, t->lw);
			if (pitch * rows > RB_SLOT ||
			    rsxkit_target_read(rdp.surf[i], &src, rdp.rb_off + buf * RB_SLOT, pitch, t->lw, rows,
					       x8 ? RSXKIT_XRGB8888 : argb ? RSXKIT_ARGB8888 : RSXKIT_RGB565) != RSXKIT_OK) {
				LOGN(10, "gfx: copia a la RDRAM de %06x/%u: rsxkit_target_read falló", t->addr, t->width);
				continue;
			}
			rdp.bound_t = -1;
			big_used |= 1u << buf;
			sync_rb[i] = true;
			rdp.rb_task[i] = rdp.tg.task;
			handled = true;
			if (s->trace && rdp_log)
				rdp_log("gfx: fb tarea %u: copia %06x/%u ranura %d %s, %u..%u × %u..%u, búfer %d", n, t->addr,
					t->width, i, s->rb_sync[k] ? "esperada" : "sin esperar", drawn_box[i].x0, drawn_box[i].x1,
					drawn_box[i].y0, rows, buf);
			if (s->rb_sync[k]) {
				rb[nrb].slot = i;
				rb[nrb].buf = buf;
				rb[nrb].argb = argb;
				rb[nrb].rows = rows;
				rb[nrb].box = drawn_box[i];
				rb[nrb].box.y1 = (uint16_t)rows;
				nrb++;
			} else {
				struct mirror *m = &rdp.big[buf];
				m->used = true;
				m->argb = argb;
				m->addr = t->addr;
				m->width = t->width;
				m->lw = t->lw;
				m->rows = rows;
				m->box = drawn_box[i];
				m->box.y1 = (uint16_t)rows;
				m->bpp = t->bpp;
				m->task = n + 1;
				m->fence = 0;  /* se pone con la barrera de la tarea */
			}
			break;
		}
		if (!handled) {
			rdp.stats.readbacks_lost++;
			if (s->trace && rdp_log)
				rdp_log("gfx: fb tarea %u: copia %06x/%u %s PERDIDA: sin ranura dibujada", n, s->rb_addr[k],
					s->rb_width[k], s->rb_sync[k] ? "esperada" : "sin esperar");
		}
	}
	/* Copias espejo de las imágenes auxiliares chicas dibujadas en la
	 * tarea (nunca presentadas, de otro ancho que el VI): la RDRAM las
	 * tendrá cuando un dibujo las lea ya viejas (Link en la pantalla de
	 * equipo de Zelda, 395 tareas después). No se espera: se convierten en
	 * una traducción posterior, cuando el RSX pasó la barrera. */
	for (int i = 0; rdp.rdram && rdp.fbfx >= 2 && i < RDP_TARGETS_MAX; i++) {
		if (!drawn[i] || sync_rb[i])
			continue;
		int k = mirror_small(i, &drawn_box[i], n, MIRROR_BYTES);
		if (k >= 0)
			mirror_k[nmirror++] = k;
	}
	if (s->trace && rdp_log) {
		char line[400];
		int len = snprintf(line, sizeof line, "gfx: fb tarea %u: espejos %u, tabla:", n, nmirror);
		for (int i = 0; i < RDP_TARGETS_MAX && len < (int)sizeof line; i++) {
			const struct rdp_target *t = &rdp.tg.t[i];
			if (t->used)
				len += snprintf(line + len, sizeof line - len, " r%d %06x/%u lh%u t-%u%s%s%s", i, t->addr, t->width,
						t->lh, rdp.tg.task - t->last_task, t->presented ? " vi" : "",
						t->cpu_owned ? " cpu" : "", rdp.surf[i] ? "" : " SIN-SUP");
		}
		rdp_log("%s", line);
	}
	s->fence = rsxkit_fence();
	lwsync();
	s->fence_task = n + 1;
	for (uint32_t k = 0; k < nmirror; k++)
		rdp.mirrors[mirror_k[k]].fence = s->fence;
	for (int k = 0; k < RB_SLOTS; k++)
		if (rdp.big[k].used && !rdp.big[k].fence)
			rdp.big[k].fence = s->fence;
	if (nrb) {
		rdp.at = "barrera de las copias";
		uint64_t t1 = sysGetSystemTime();
		fence_wait(s->fence);
		for (uint32_t k = 0; k < nrb; k++) {
			const struct rdp_target *t = &rdp.tg.t[rb[k].slot];
			if (dma_newer(t->addr, t->width * t->bpp * rb[k].rows, n + 1))
				continue;
			to_rdram(rdp.rb + rb[k].buf * RB_SLOT, RB_PITCH, t->addr, t->width, t->lw, &rb[k].box, t->bpp,
				 rb[k].argb, "copia de la tarea", n + 1, 0);
			rdp.stats.readbacks++;
		}
		readbacks_finish(false);  /* pasó la barrera: los espejos también */
		rdp.stats.us_readback += sysGetSystemTime() - t1;
	}
	rdp.at = "espejo del framebuffer";
	/* Espejo de los framebuffers que dibujó la tarea (el VI ya los mostró
	 * alguna vez): ahora, como el RDP, y no al presentarlos. Va después de
	 * poner las barreras de las copias de arriba (readbacks_finish no debe
	 * ver una sin barrera). */
	for (int i = 0; rdp.rdram && rdp.fbfx >= 2 && i < RDP_TARGETS_MAX; i++) {
		uint32_t rows = drawn_box[i].y1 < rdp.tg.t[i].lh ? drawn_box[i].y1 : rdp.tg.t[i].lh;
		if (drawn[i] && !sync_rb[i] && rdp_targets_mirror_at_task_end(&rdp.tg, i))
			frame_mirror(i, rows, true);
	}
	lwsync();  /* la RDRAM, antes de decir que está */
	rdp.readback_done = n + 1;
	rdp.at = "fin";
	s->ppe_tex_end = rdp.ppe_tex_off;
	rdp.last_res = *res;
	rdp.last_set = (unsigned)(s - rdp.sets);
	rdp.stats.tasks++;
	uint64_t us = sysGetSystemTime() - t0;
	rdp.stats.us_translate += us;
	rdp.tb.translate += tb_now() - tb0;
	if (perf_hook) {
		rsxkit_fp_stats fp1;
		rsxkit_fp_get_stats(&fp1);
		perf_info.task = n + 1;
		perf_info.res = res;
		perf_info.us_translate = (uint32_t)us;
		perf_info.us_set_wait = s->set_wait_us;
		perf_info.fp_loads = fp1.loads - fp0.loads;
		perf_info.readbacks = nrb;
		perf_hook(&perf_info);
	}
	rdp.in_task = 0;
	rdp.at = NULL;
}

uint32_t salsa64_rdp_flush(uint32_t upto)
{
	uint32_t n = rdp.translated, done_n = rdp.finished;
	lwsync();  /* los resultados, después de ver `finished` */
	if (upto == ~0u)
		upto = done_n;
	uint32_t w = rdp.rb_waiting;  /* la que espera la emulación, siempre */
	if (w && (int32_t)(w - upto) > 0)
		upto = w;
	w = rdp.set_waiting;  /* y la que libera el juego de búferes que espera acquire */
	if (w && (int32_t)(w - upto) > 0)
		upto = w;
	while ((int32_t)(n - done_n) < 0 && (int32_t)(n - upto) < 0) {
		translate(n);
		lwsync();  /* la barrera del juego, antes de darlo por traducido */
		rdp.translated = ++n;
	}
	return n;
}

uint32_t salsa64_rdp_tasks_done(void)
{
	return rdp.finished;
}

/* Espejo de un framebuffer: el RSX lo reduce a un búfer y, pasada su
 * barrera (readbacks_finish, en una traducción o presentación posterior),
 * el PPE lo escribe en la RDRAM. Así lo que el CPU lea del framebuffer
 * (Mario Kart reduce el cuadro para la pantalla del túnel) es el cuadro,
 * no basura. Una sola vez por dibujo (targets.h): al terminar la tarea que
 * lo dibujó (`wait`: si el espejo anterior aún no pasó su barrera, se
 * espera) o, si entonces no se sabía que era un framebuffer, al
 * presentarlo (solo las filas que muestra el VI; sin esperar: si el espejo
 * anterior aún no pasó la barrera, este campo se salta). Copiar en cada
 * campo, o al presentar, un cuadro que el RDP dibujó antes pisaba lo que
 * el juego cargó por DMA después en esa memoria (SM64 al pasar del logo a
 * la cara de Mario se colgaba). */
static void frame_mirror(int slot, uint32_t rows, bool wait)
{
	const struct rdp_target *t = &rdp.tg.t[slot];
	readbacks_finish(false);
	if (wait && rdp.frame.used) {
		fence_wait(rdp.frame.fence);
		readbacks_finish(false);
	}
	if (rdp.frame.used || !rdp.rdram || rdp.fbfx < 2 || !rdp.surf[slot] || t->bpp != 2 || t->lw > RB_MAX_W ||
	    rows > RB_MAX_H || !rows || t->addr + t->width * 2 * rows > rdp.rdram_size ||
	    !rdp_targets_mirror_wanted(&rdp.tg, slot))
		return;
	if (rows > t->lh)
		rows = t->lh;
	/* Lo dibujado desde el último espejo (targets.h), hasta `rows`. */
	struct rdp_box box = t->box;
	if (rows > box.y1)
		rows = box.y1;
	box.y1 = (uint16_t)rows;
	if (rdp_box_empty(&box))
		return;
	/* No pisar lo que una copia reciente dejó en esa misma memoria (Link
	 * en los primeros bytes del framebuffer; la cobertura de VISCVG). */
	uint32_t lo = t->addr, hi = t->addr + t->width * 2 * rows;
	for (int i = 0; i < RDP_TARGETS_MAX; i++) {
		const struct rdp_target *a = &rdp.tg.t[i];
		if (!a->used || rdp.tg.task - rdp.rb_task[i] > RB_KEEP_TASKS)
			continue;
		uint32_t alo = a->addr, ahi = a->addr + a->width * a->bpp * a->lh;
		if (alo < hi && ahi > lo)
			return;
	}
	rsxkit_rect src = surface_rect(slot, 0, 0, t->lw, rows);
	if (rsxkit_target_read(rdp.surf[slot], &src, rdp.rb_off + FRAME_SLOT * RB_SLOT, RB_PITCH, t->lw, rows,
			       RSXKIT_RGB565) != RSXKIT_OK)
		return;
	rdp.bound_t = -1;
	rdp_targets_mirrored(&rdp.tg, slot);
	rdp.frame.used = true;
	rdp.frame.addr = t->addr;
	rdp.frame.width = t->width;
	rdp.frame.lw = t->lw;
	rdp.frame.rows = rows;
	rdp.frame.box = box;
	rdp.frame.task = t->last_task;
	rdp.frame.fin = rdp.slot_fin[slot];
	rdp.frame.fence = rsxkit_fence();
}

/* ---- Campos intermedios (SALSA64_INTERP_FIELDS, docs/interpolacion.md) ----
 *
 * En los campos en que el VI repite un cuadro del juego que tardó k campos
 * (2–4: 30 a 15 cuadros), la tarea que lo dibujó se vuelve a traducir con
 * sus vértices entre el cuadro anterior y ese (t = (j + 1)/k en el campo j
 * desde que apareció; el cuadro de verdad llega k − 1 campos después: 33 ms
 * a 20 cuadros) en una superficie aparte, y se presenta esa. Solo si su juego
 * de búferes sigue con ella (interp_hold) y sus posiciones de antes valen. */

static void interp_release(void)
{
	if (rdp.interp.surf)
		rsxkit_target_destroy(rdp.interp.surf);
	if (rdp.interp.depth)
		rsxkit_depth_destroy(rdp.interp.depth);
	rdp.interp.surf = NULL;
	rdp.interp.depth = NULL;
	rdp.vram -= rdp.interp.bytes < rdp.vram ? rdp.interp.bytes : rdp.vram;
	rdp.interp.w = rdp.interp.h = rdp.interp.bytes = 0;
}

/* Superficie y profundidad de los intermedios del tamaño de la del destino
 * `slot` (cuentan en el tope de memoria como un destino más). */
static bool interp_surface(int slot)
{
	uint32_t w, h;
	rsxkit_target_size(rdp.surf[slot], &w, &h);
	if (rdp.interp.surf && rdp.interp.w == w && rdp.interp.h == h)
		return true;
	interp_release();
	uint32_t bytes = 2 * rdp_surface_bytes(w, h);
	if (!make_room(bytes, slot))
		return false;
	rdp.interp.surf = rsxkit_target_create(w, h);
	rdp.interp.depth = rdp.interp.surf ? rsxkit_depth_create(w, h) : NULL;
	if (!rdp.interp.depth) {
		interp_release();
		LOGN(5, "gfx: interpolación: sin memoria del RSX para la superficie de %ux%u", w, h);
		return false;
	}
	rdp.interp.w = w, rdp.interp.h = h, rdp.interp.bytes = bytes;
	rdp.vram += bytes;
	return true;
}

/* La tarea `task` (fence_task) se va a redibujar: que acquire no reuse su
 * juego y que la HLE no suelte de la caché lo que usa. false si ya es
 * tarde (su juego se está reusando, o una tarea en la HLE pudo soltarlo). */
static bool interp_hold(uint32_t task)
{
	rdp.interp.busy = task;
	uint32_t kf = rdp.interp.keep_fence;
	if (!rdp.interp.keep_task || (kf && rsxkit_fence_done(kf)))
		rdp.interp.keep_task = task;
	rdp.interp.keep_fence = 0;
	hwsync();
	uint32_t pend = rdp.interp.done_pending;
	bool reused = (int32_t)(rdp.interp.acq - (task + SETS)) >= 0;
	bool cache = (int32_t)(rdp.interp.done_given - task) >= 0 || (pend && (int32_t)(pend - task) >= 0);
	if (reused || cache) {
		rdp.stats.interp_rep_set += reused;
		rdp.stats.interp_rep_cache += !reused;
		rdp.interp.keep_fence = rsxkit_fence();
		lwsync();
		rdp.interp.busy = 0;
		return false;
	}
	return true;
}

/* Redibuja la tarea de `s` (la que dibujó el destino `slot`) en
 * rdp.interp.surf con las posiciones en la fracción t: primero una copia
 * del destino (lo que la tarea no redibuja queda igual) y después solo sus
 * dibujos en ese destino, con la profundidad aparte. No toca la tabla de
 * destinos, las copias a la RDRAM ni el estado de lo que se dibuja de
 * verdad. */
static void interp_draw(struct set *s, int slot, float t)
{
	const struct rdp_target *tg = &rdp.tg.t[slot];
	const struct gfx_result *res = &s->res;
	rsxkit_target_copy(rdp.interp.surf, rdp.surf[slot]);
	rdp.bound_t = -1;
	rdp.bound_d = NULL;
	bool bound = false;
	rsxkit_vertices(s->vtx_off, RSXKIT_MEM_MAIN);
	rsxkit_vertices_prev(s->vtx_off + VTX_INTERP, RSXKIT_MEM_MAIN, t);
	rdp.scratch_slot = -1;
	rdp.ppe_tex_off = s->ppe_tex_end;
	bool here = false, zpend = false;
	uint32_t zaddr = 0;
	float zval = 1.0f, zrect[4];
	rsxkit_draw rd;
	for (uint32_t off = 0; off + sizeof(struct gfx_cmd) <= res->cmd_bytes;) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(s->cmd + off);
		if (c->op == GFX_OP_END || c->size < sizeof *c)
			break;
		off += c->size;
		if (c->op == GFX_OP_TARGET) {
			here = c->a == tg->addr && c->b == tg->width;
		} else if (c->op == GFX_OP_ZCLEAR) {
			const struct gfx_zclear *z = (const struct gfx_zclear *)c;
			zaddr = z->h.a;
			zval = (float)(z->h.b >> 2) / 16383.0f;
			for (int i = 0; i < 4; i++)
				zrect[i] = z->rect[i];
			zpend = true;
		} else if (c->op == GFX_OP_DRAW && here && c->size >= sizeof(struct gfx_draw)) {
			const struct gfx_draw *g = (const struct gfx_draw *)c;
			bool sync;
			if (!g->h.b || g->viscvg || !to_rsxkit(g, s, slot, &rd, &sync))
				continue;
			if (sync)
				rsxkit_texture_sync();
			/* Una copia al scratch (to_rsxkit) liga otro destino. */
			if (!bound || rdp.bound_t != -2) {
				if (rsxkit_bind(rdp.interp.surf, rdp.interp.depth, (float)tg->lw, (float)tg->lh) != RSXKIT_OK)
					break;
				rdp.bound_t = -2;
				if (!bound)  /* la profundidad aparte, lejos al empezar */
					rsxkit_clear(RSXKIT_CLEAR_DEPTH, 0, 1.0f, NULL);
				bound = true;
			}
			/* Y como la borró la tarea, al primer dibujo que la usa. */
			if (zpend && g->depth && g->zimg == zaddr) {
				rsxkit_clear(RSXKIT_CLEAR_DEPTH, 0, zval, zrect);
				zpend = false;
			}
			rsxkit_draw_triangles(&rd, g->h.a, g->h.b);
		}
	}
	s->ppe_tex_end = rdp.ppe_tex_off;
	rdp.bound_t = -1;
	rdp.bound_d = NULL;
}

/* Si el campo es intermedio, lo dibuja y lo deja para presentar
 * (rdp.again.interp). */
static void interp_field(const struct rdp_present *p, uint32_t field)
{
	uint32_t task = rdp.tg.t[p->slot].last_task;
	struct set *s = NULL;
	for (unsigned i = 0; i < SETS; i++)
		if (rdp.sets[i].tg_task == task && rdp.sets[i].fence_task)
			s = &rdp.sets[i];
	if (task != rdp.interp.shown_task) {
		uint32_t gap = field - rdp.interp.frame_field;
		rdp.interp.k = rdp.interp.shown_task && gap >= 2 && gap <= 4 ? gap : 0;
		rdp.interp.last_gap = rdp.interp.shown_task ? gap : 0;
		rdp.interp.shown_task = task;
		rdp.interp.frame_field = field;
		/* Desde ahora la HLE no suelta lo que usa esta tarea (acquire). */
		rdp.interp.protect = s ? s->fence_task : 0;
	}
	uint32_t j = field - rdp.interp.frame_field;
	if (rdp.interp.k < 2 || j + 1 >= rdp.interp.k)
		return;  /* el cuadro de verdad */
	if (!s || !s->interp || !s->interp_ok) {
		rdp.stats.interp_plain++;  /* este cuadro no se interpola: va entero */
		return;
	}
	bool surf = interp_surface(p->slot);
	if (!surf)
		rdp.stats.interp_rep_surf++;
	if (!surf || !interp_hold(s->fence_task)) {
		/* Ya es tarde para redibujarlo: se repite lo del campo anterior (el
		 * cuadro final adelantaría y el intermedio siguiente volvería atrás:
		 * eso se ve como un parpadeo). */
		const struct again *l = &rdp.again_last;
		if (l->surf && (l->interp ? l->surf == rdp.interp.surf : rdp.surf[l->slot] == l->surf))
			rdp.again = *l;
		rdp.stats.interp_repeated++;
		rdp.finfo = (struct salsa64_rdp_field_info){ SALSA64_FIELD_REPEAT, (float)(j + 1) / (float)rdp.interp.k,
							     rdp.interp.k, s->fence_task - 1, s->fence_task - 2 };
		return;
	}
	uint64_t t0 = sysGetSystemTime();
	interp_draw(s, p->slot, (float)(j + 1) / (float)rdp.interp.k);
	rdp.finfo = (struct salsa64_rdp_field_info){ SALSA64_FIELD_INTERP, (float)(j + 1) / (float)rdp.interp.k,
						     rdp.interp.k, s->fence_task - 1, s->fence_task - 2 };
	uint32_t f = rsxkit_fence();
	s->fence = f;
	rdp.interp.keep_fence = f;
	lwsync();
	rdp.interp.busy = 0;
	rdp.again.surf = rdp.interp.surf;
	rdp.again.interp = true;
	rdp.interp.gen = ++rdp.gen_seq;
	rdp.stats.interp_fields++;
	rdp.stats.us_interp_draw += sysGetSystemTime() - t0;
}

bool salsa64_rdp_present(const struct salsa64_vi_frame *f, uint32_t field, const rsxkit_view *view,
			 const struct salsa64_overscan *crop, rsxkit_rect *out)
{
	if (!rdp.ready)
		return false;
	struct rdp_present p;
	rdp_targets_present(&rdp.tg, f, field, &p);
	if (p.slot < 0 || !rdp.surf[p.slot] || !p.w || !p.h)
		return false;
	rdp.last_p = p;
	rdp.stats.age_sum += p.age;
	if (p.age > rdp.stats.age_max)
		rdp.stats.age_max = p.age;
	rdp.again.interp = false;
	rdp.finfo.kind = SALSA64_FIELD_TRUE;
	again_set(p.slot, p.y0, p.w, p.h, view, crop);
	if (rdp.interp.mode == SALSA64_INTERP_FIELDS)
		interp_field(&p, field);
	else if (rdp.interp.surf)
		interp_release();
	if (rdp.interp.mode != SALSA64_INTERP_FIELDS)
		rdp.interp.protect = 0;
	rdp.again_last = rdp.again;
	if (!rdp.no_draw)
		rsxkit_target_present(rdp.again.surf, &rdp.again.src, rdp.again.w, rdp.again.h, &rdp.again.view, out);
	else if (out)
		*out = rsxkit_layout(rdp.again.w, rdp.again.h, &rdp.again.view);
	frame_mirror(p.slot, p.y0 + p.h, false);
	return true;
}

/* Lo que se presenta de la región (slot, y0, pw × ph): recorte, tamaño y
 * vista, en rdp.again (salsa64_rdp_present y su repetición). */
static void again_set(int slot, uint32_t y0, uint32_t pw, uint32_t ph, const rsxkit_view *view,
		      const struct salsa64_overscan *crop)
{
	/* Recorte del overscan: la región se achica por los cuatro lados y se
	 * presenta como una imagen más chica (misma proporción de pantalla). */
	uint32_t cl = 0, cr = 0, ct = 0, cb = 0;
	if (crop) {
		cl = crop->left, cr = crop->right, ct = crop->top, cb = crop->bottom;
		if (cl + cr >= pw)
			cl = cr = 0;
		if (ct + cb >= ph)
			ct = cb = 0;
	}
	uint32_t w = pw - cl - cr, h = ph - ct - cb;
	rsxkit_rect src = surface_rect(slot, cl, y0 + ct, w, h);
	/* Si la región cabe en la pantalla, a 1:1 (modo entero con su tamaño
	 * en píxeles de la superficie: factor 1, píxeles cuadrados, centrada;
	 * en 480/576 el píxel no es cuadrado y el ancho se estira igual que
	 * antes). Ajustarla a la pantalla estiraba el recorte del overscan
	 * (912 × 684 a 960 × 720, ×1,053) y el filtro lineal repetía una fila
	 * cada ~19: las líneas en el logo de N64 de Zelda. Lo que sobra queda
	 * negro, que es como se borra el cuadro. Sin recorte no cambia nada:
	 * 960 × 720 cabía justo. Con la escala por destino la región puede
	 * quedar muy por debajo de la pantalla (640 × 480 a 2×, o un
	 * framebuffer de 640 × 480 a 1× con la escala-objetivo 3×, en 720p):
	 * entonces se ajusta como siempre (rdp_present_integer). La decisión y
	 * el tamaño en pantalla salen del tamaño lógico por la escala objetivo,
	 * no de la superficie: dos framebuffers del mismo juego pueden tener
	 * superficies de escala distinta (Donkey Kong 64 crea uno de 320 × 240
	 * mientras viven los de 640 × 480 de sus logos) y con el tamaño de la
	 * superficie cada uno salía de un tamaño: un «zoom» cada 2 campos. */
	const rsxkit_screen *sc = rsxkit_screen_info();
	uint32_t sw = w * rdp.scale, sh = h * rdp.scale;
	rdp.again.surf = rdp.again.interp ? rdp.interp.surf : rdp.surf[slot];
	rdp.again.scale = rdp.surf_scale[slot];
	rdp.again.slot = slot;
	rdp.again.y0 = y0;
	rdp.again.pw = pw;
	rdp.again.ph = ph;
	rdp.again.src = src;
	/* Con una proporción panorámica pedida (aspecto 16:9 del menú) no vale la
	 * presentación a 1:1: esa dibuja la región en su proporción de 4:3 y el
	 * aspecto no se aplicaría. Se ajusta a la pantalla con esa proporción. */
	if (view && view->scale == RSXKIT_SCALE_FIT && view->aspect <= 1.5f && rdp_present_integer(sw, sh, sc->width, sc->height)) {
		rsxkit_view one = { RSXKIT_SCALE_INTEGER, view->filter, 0.0f };
		rdp.again.w = sw;
		rdp.again.h = sh;
		rdp.again.view = one;
	} else {
		rdp.again.w = w;
		rdp.again.h = h;
		rdp.again.view = view ? *view : (rsxkit_view){ RSXKIT_SCALE_FIT, RSXKIT_FILTER_LINEAR, 0.0f };
	}
}

void salsa64_rdp_present_draw(bool draw)
{
	rdp.no_draw = !draw;
}

/* Lo que eligió la última presentación sigue vivo: el destino, o el campo
 * intermedio. */
static bool again_valid(void)
{
	return rdp.again.surf &&
	       (rdp.again.interp ? rdp.again.surf == rdp.interp.surf : rdp.surf[rdp.again.slot] == rdp.again.surf);
}

bool salsa64_rdp_present_source(const rsxkit_view *view, const struct salsa64_overscan *crop, rsxkit_texture *tex,
				rsxkit_rect *region, uint32_t *scale, uint32_t *gen)
{
	if (!rdp.ready || !again_valid())
		return false;
	if (view || crop)
		again_set(rdp.again.slot, rdp.again.y0, rdp.again.pw, rdp.again.ph, view, crop);
	rsxkit_target_texture(rdp.again.surf, 0, 0, tex);
	*region = rdp.again.src;
	*scale = rdp.again.scale ? rdp.again.scale : 1;
	if (gen)
		*gen = rdp.again.interp ? rdp.interp.gen : rdp.surf_gen[rdp.again.slot];
	return true;
}

bool salsa64_rdp_present_again(const rsxkit_view *view, const struct salsa64_overscan *crop, rsxkit_rect *out)
{
	if (!rdp.ready || !again_valid())
		return false;
	if (view || crop)
		again_set(rdp.again.slot, rdp.again.y0, rdp.again.pw, rdp.again.ph, view, crop);
	rsxkit_target_present(rdp.again.surf, &rdp.again.src, rdp.again.w, rdp.again.h, &rdp.again.view, out);
	return true;
}

void salsa64_rdp_field_info(struct salsa64_rdp_field_info *out)
{
	*out = rdp.finfo;
}

void salsa64_rdp_set_task_hook(salsa64_rdp_task_hook fn, void *ctx)
{
	rdp.task_ctx = ctx;
	rdp.task_hook = fn;
}

uint32_t salsa64_rdp_translated(void)
{
	return rdp.translated;
}

uint32_t salsa64_rdp_translating(const char **stage)
{
	uint32_t t = rdp.in_task;
	if (stage) {
		const char *at = rdp.at;
		*stage = t && at ? at : "";
	}
	return t;
}

void salsa64_rdp_diag_log(void (*log)(const char *fmt, ...))
{
	uint32_t have = diag.n < DIAG_LEN ? diag.n : DIAG_LEN;
	for (uint32_t i = 0; i < have; i++)
		log("gfx reciente %u/%u: %s", i + 1, have, diag.line[(diag.n - have + i) % DIAG_LEN]);
	int b = rdp.bound_t;
	if (b >= 0 && b < RDP_TARGETS_MAX) {
		const struct rdp_target *t = &rdp.tg.t[b];
		log("gfx ligado: ranura %d, destino %06x/%u de %u × %u a %u×, profundidad %06x de %u × %u", b, t->addr, t->width,
		    t->lw, t->lh, rdp.surf_scale[b], rdp.bound_d ? rdp.bound_d->addr : 0, rdp.bound_d ? rdp.bound_d->w : 0,
		    rdp.bound_d ? rdp.bound_d->h : 0);
	}
	for (int i = 0; i < RDP_TARGETS_MAX; i++) {
		const struct rdp_target *t = &rdp.tg.t[i];
		if (t->used)
			log("gfx ranura %d: %06x/%u, %u × %u a %u×, superficie %s, dibujada hace %u tareas", i, t->addr, t->width,
			    t->lw, t->lh, rdp.surf_scale[i], rdp.surf[i] ? "sí" : "no", rdp.tg.task - t->last_task);
	}
	for (int i = 0; i < MAX_DEPTHS; i++) {
		const struct depth *d = &rdp.depths[i];
		if (d->used)
			log("gfx profundidad %d: %06x de %u × %u, cargada desde la RDRAM en la tarea %u", i, d->addr, d->w, d->h,
			    d->fed_task);
	}
}

/* El volcado de la tarea `s` con su resultado `r` (gfxdump.c). */
static bool dump_set(const struct set *s, const struct gfx_result *r, const char *path);

bool salsa64_rdp_dump_current(const char *path, uint32_t *task)
{
	if (!rdp.ready)
		return false;
	uint32_t n = rdp.in_task;
	if (!n) {
		if (task)
			*task = rdp.translated ? rdp.translated - 1 : 0;
		return salsa64_rdp_dump(path);
	}
	if (task)
		*task = n - 1;
	const struct set *s = &rdp.sets[(n - 1) % SETS];
	return s->res.cmd_bytes && dump_set(s, &s->res, path);
}

bool salsa64_rdp_dump(const char *path)
{
	if (!rdp.ready || !rdp.last_res.cmd_bytes)
		return false;
	return dump_set(&rdp.sets[rdp.last_set], &rdp.last_res, path);
}

static bool dump_set(const struct set *s, const struct gfx_result *r, const char *path)
{
	struct gfx_dump_src src = { s->cmd, r->cmd_bytes, s->vtx, r->vtx_bytes, s->tex, r->tex_bytes,
				    rdp.cache, CACHE_BYTES, rdp.rdram, rdp.rdram_size };
	/* El armado (gfxdump.c) es el mismo de la caja negra; aquí, con
	 * memoria de la que se pide y se suelta (es una orden de depuración). */
	uint32_t max = GFX_DUMP_MAX_TEX(r->cmd_bytes);
	uint32_t *wsrc = malloc(max * 2 * sizeof *wsrc);
	const struct gfx_tex **wtgt = malloc(max * sizeof *wtgt);
	bool ok = false;
	if (wsrc && wtgt) {
		struct gfx_dump_work w = { wsrc, wsrc + max, wtgt, max };
		uint32_t need = gfx_dump_measure(&src, &w);
		uint8_t *buf = need ? malloc(need) : NULL;
		if (buf && gfx_dump_build(&src, &w, buf, need) == need) {
			FILE *f = fopen(path, "wb");
			ok = f && fwrite(buf, 1, need, f) == need;
			if (f)
				fclose(f);
		}
		free(buf);
	}
	free(wsrc);
	free(wtgt);
	return ok;
}

void salsa64_rdp_get_waits(uint64_t *set_wait_us, uint64_t *readback_wait_us)
{
	*set_wait_us = rdp.stats.us_set_wait;
	*readback_wait_us = rdp.stats.us_readback_wait;
}

void salsa64_rdp_get_tb(struct salsa64_rdp_tb *out)
{
	*out = rdp.tb;
}

void salsa64_rdp_get_stats(struct salsa64_rdp_stats *out)
{
	*out = rdp.stats;
	rsxkit_fp_stats fp;
	rsxkit_fp_get_stats(&fp);
	out->fp_programs = fp.programs;
	out->fp_generated = fp.generated;
	out->fp_evictions = fp.evictions;
	out->fp_loads = fp.loads;
	out->tex_rsx = rdp.tg.tex_rsx;
	out->tex_rdram = rdp.tg.tex_rdram;
	out->tex_self = rdp.tg.tex_self;
}
