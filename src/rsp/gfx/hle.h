/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — HLE gráfica: estado interno y E/S por plataforma. Ver gfx.h.
 *
 * Todo el estado cabe en una gfx_state (~53 KB: la TMEM son 4 KB y el
 * directorio de la caché de texturas 32 KB): en el SPE vive en el local
 * store y persiste entre tareas, como el estado del RDP en la consola. El
 * estado del RSP (matrices, luces, segmentos) se reinicia en cada tarea,
 * como lo hace el microcódigo al cargarse.
 */
#ifndef SALSA64_RSP_GFX_HLE_H
#define SALSA64_RSP_GFX_HLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rsp/gfx/gfx.h"
#include "rsp/gfx/spe0.h"

/* Las otras imágenes no contienen ni ejecutan el detalle. */
#if !defined(GFX_SPE0_IMG) || GFX_SPE0_IMG == 1
#define GFX_DETAIL(g, ...) do { if ((g)->detail) { __VA_ARGS__; } } while (0)
#else
#define GFX_DETAIL(g, ...) do { if (0) { __VA_ARGS__; } } while (0)
#endif

/* Censo gráfico del host (src/host/gfxcheck.c, -DGFX_CENSUS): cada comando
 * que la HLE no implementa avisa al gancho con su opcode. En la PS3 y en el
 * SPE la macro no genera nada. */
#ifdef GFX_CENSUS
extern void (*gfx_unknown_hook)(uint32_t op);
extern int gfx_dpc_trace;  /* dpc.c: volcado crudo de los comandos (censo, SALSA64_GCHK_DPC_TRACE) */
extern uint32_t gfx_dpc_hist[64];  /* dpc.c: comandos del RDP por opcode (6 bits bajos) */
#define GFX_UNKNOWN(op) do { if (gfx_unknown_hook) gfx_unknown_hook((uint32_t)(op)); } while (0)
#else
#define GFX_UNKNOWN(op) ((void)0)
#endif

#define GFX_MAX_VTX     64   /* F3D: 16; F3DEX y F3DEX2: 32 */
struct gfx_state;
#if !defined(GFX_SPE0_IMG) || GFX_SPE0_IMG == 1
void gfx_io_detail_begin(struct gfx_state *g);
void gfx_io_detail_end(struct gfx_state *g);
#else
#define gfx_io_detail_begin(g) ((void)0)
#define gfx_io_detail_end(g) ((void)0)
#endif
#define GFX_MTX_STACK   32   /* F3D: 10, F3DEX2: 18 en la RDRAM */
#define GFX_DL_STACK    32   /* F3D: 10, F3DEX2: 18 */
#define GFX_MAX_LIGHTS  11   /* + la ambiental; F3DEX2: 7, Conker: 11 */
/* Texturas del arena que se recuerdan por tarea (para no decodificar dos
 * veces la misma con la caché llena o apagada): 32 entradas, 1,5 KB del
 * local store del SPE 0; 27 en la imagen F3DEX2 y 25 en sus imágenes
 * experimentales de fase. La caché normal de 1024 entradas sigue intacta. */
#if defined(GFX_SPE0_PHASE)
#define GFX_ARENA_TEXTURES 25
#elif defined(GFX_SPE0_IMG) && GFX_SPE0_IMG == GFX_SPE0_F3DEX2
#define GFX_ARENA_TEXTURES 27
#else
#define GFX_ARENA_TEXTURES 32
#endif
/* Texturas en la caché entre tareas: en el SPE 0, las de su imagen
 * (spe0.h; la pila manda); en el host y el PPE, 1024. El máximo de texturas
 * distintas en 100 campos es 428 (GoldenEye en Dam, en el host). No hace
 * falta potencia de 2 (texcache.c usa %). */
#ifdef GFX_SPE0_IMG
#define GFX_TC_ENTRIES  GFX_SPE0_CAT(GFX_SPE0_TC_, GFX_SPE0_IMG)
#else
#define GFX_TC_ENTRIES  1024
#endif
#define GFX_TC_SLOTS    2048 /* índice por hash de la caché (potencia de 2) */
#define GFX_MAX_COMMANDS (1u << 20)

/* Códigos de recorte de un vértice. */
#define GFX_CLIP_XN 0x01u
#define GFX_CLIP_XP 0x02u
#define GFX_CLIP_YN 0x04u
#define GFX_CLIP_YP 0x08u
#define GFX_CLIP_ZF 0x10u
#define GFX_CLIP_W  0x20u  /* detrás de la cámara (w < 0) */
#define GFX_CLIP_MASK 0x3Fu
/* Con GFX_JOB_INTERP los bits de arriba de gfx_vtx.clip guardan la
 * identidad del vértice (GFX_VGID, gfx.h): gfx_vtx_out la pasa a pad. */
#define GFX_CLIP_ID_SHIFT 8

/* Vértice del búfer del RSP, ya transformado. */
struct gfx_vtx {
	float x, y, z, w;     /* coordenadas de recorte */
	float sx, sy, sz;     /* pantalla: píxeles y profundidad (0..0x3FF) */
	float s, t;           /* texels, con la escala de G_TEXTURE */
	uint8_t c[4];         /* color final (luz y niebla aplicadas) */
	uint32_t clip;
};

struct gfx_light {
	float col[3];   /* 0..255 */
	float dir[3];   /* dirección normalizada en espacio de la cámara */
};

struct gfx_tile {
	uint8_t fmt, siz;
	uint8_t pal;
	uint8_t cms, cmt;     /* bit 0 espejo, bit 1 limitar (G_TX_*) */
	uint8_t masks, maskt;
	uint8_t shifts, shiftt;
	uint16_t line;        /* bytes por fila en la TMEM */
	uint16_t tmem;        /* dirección en bytes */
	uint16_t uls, ult, lrs, lrt;  /* 10.2 */
};

/* Textura decodificada en el arena de la tarea en curso. */
struct gfx_texentry {
	uint32_t key[2];
	struct gfx_tex tex;
};

/* Caché de texturas entre tareas (texcache.c): directorio de lo que hay en
 * la región job->cache_ea. Las entradas forman una cola en el orden en que
 * ocupan la región (anillo); se reserva en `head_off` y se desaloja lo que
 * sigue, salvo lo que aún puede leer el consumidor (last_task > done_task),
 * que se salta. */
struct gfx_tc_entry {
	uint32_t key[2];
	uint32_t off, size;      /* lugar en la región (múltiplos de 128) */
	uint32_t last_task;      /* última tarea que la usó */
	uint16_t width, height, pitch;
	uint16_t slot;           /* posición en index */
};

struct gfx_texcache {
	uint64_t ea;             /* región con que se armó el directorio */
	uint32_t cap;
	uint32_t head_off;       /* donde va la próxima textura */
	uint32_t tail, count;    /* cola de entradas en e[] (circular) */
	uint32_t task, done_task;
	/* Tras cambiar de imagen del SPE 0 el directorio empieza vacío, pero la
	 * región puede tener texturas que el consumidor aún lee: hasta que
	 * termine la tarea `fence` (fence_on), nada nuevo entra a la región
	 * (va al arena). Ver gfx_tc_fence. */
	uint32_t fence;
	uint32_t fence_on;
	uint16_t index[GFX_TC_SLOTS];  /* posición en e[] + 1; 0 = libre */
	struct gfx_tc_entry e[GFX_TC_ENTRIES];
};

/* Estado propio de las variantes de Rare (f3dpd.c, f3dexbg.c); cada una
 * lo pone en cero al empezar la tarea. */
struct gfx_rare {
	uint32_t color_base;     /* Perfect Dark: tabla de colores de los vértices */
	/* Conker */
	uint32_t normal_base;    /* normales: 2 bytes por lugar del búfer de vértices */
	bool advanced;           /* iluminación avanzada (tras su G_LOAD_UCODE) */
	/* Conker y Diddy Kong Racing nunca van juntos: comparten lugar (el
	 * tamaño de la estructura fija dónde empieza el estado del RDP que
	 * guardan los estados, GFX_RAW_START). */
	union {
		struct {
			float coord_mod[16];     /* G_MW_COORD_MOD: posición para las luces puntuales */
			float light_pos[GFX_MAX_LIGHTS + 1][3];
			float light_ca[GFX_MAX_LIGHTS + 1];  /* intensidad de la luz puntual */
		};
		struct {                 /* F3DEX2ACCLAIM: ranuras compactas 2..9 */
			float pos[8][3];
			float ca[8], la[8], qa[8];
		} acclaim;
		struct {                 /* Diddy Kong Racing (f3ddkr.c) */
			uint32_t dma_mtx, dma_vtx;   /* G_DMA_OFFSETS: bases de las direcciones de G_MTX y G_VTX */
			uint32_t vtx_last;           /* donde sigue un G_VTX que añade */
			uint32_t dl_left;            /* comandos que le quedan a la lista contada en curso */
			uint32_t dl_depth;           /* dl_sp dentro de ella */
			uint32_t cur_mtx;            /* matriz con que se transforman los vértices (0 a 3) */
			uint32_t billboard;          /* vértices relativos al 0 (carteles) */
			uint32_t dl_pop;             /* la lista contada terminó: volver con el siguiente comando */
			uint32_t tex_on, tex_i;      /* G_DMA_TEX_OFFSET: encendido y entrada que sigue */
			uint8_t tex_off[64];         /* 32 desplazamientos u16 (en el orden de la N64) que se suman a G_SETTIMG */
		} dkr;
		struct {                 /* Rogue Squadron (f5rogue.c) */
			uint32_t color_base;         /* tabla de colores de los vértices (física) */
			uint32_t tex_scale[2];       /* escala de s y t de los vértices (G_MOVEMEM 0x82) */
			uint32_t viewport;           /* dirección del viewport en la lista (G_MOVEMEM 0x80) */
			uint32_t persp;              /* dirección de la última matriz de proyección */
			uint32_t dl[12];             /* por nivel de la pila de listas: a dónde salta el 0xB5 */
		} f5;
	};
};

/* Una carga a la TMEM (LOADBLOCK o LOADTILE): qué tramo de la TMEM
 * escribió y de dónde lo trajo, para saber de qué imagen de la RDRAM sale
 * cada textura (gfx_tex.src_*). Se guardan las últimas GFX_LOADS. */
#define GFX_LOADS 4
struct gfx_load {
	uint32_t addr;       /* RDRAM: primer byte cargado */
	uint32_t row_bytes;  /* bytes por fila de la imagen de origen */
	uint16_t tmem;       /* tramo escrito en la TMEM, en bytes (32 bits: la mitad baja) */
	uint16_t bytes;
	uint16_t line;       /* bytes por fila en la TMEM (LOADBLOCK: 0, texels seguidos) */
	uint16_t pitch;      /* texels por fila de la imagen de origen */
	uint8_t siz, fmt;
};

/* Última textura resuelta por tile en la tarea: vale mientras no haya
 * cargas a la TMEM ni cambios de tiles (tex_gen) ni del modo de paleta. */
struct gfx_tilememo {
	uint32_t gen;
	uint8_t tt, ok, premul, clamp_s;
	uint16_t lim[2];
	struct gfx_tex tex;
};

/* Color images de 16 y 8 bits que la HLE vio dibujar, para no decodificar con
 * GFX_JOB_FBTEX las texturas que el consumidor va a leer de su destino
 * (tmem.c). Imitan los destinos del consumidor (src/rdp/targets.c): las
 * mismas color images de 1 a 640 de ancho, con el alto conjeturado igual
 * (3/4 del ancho, o hasta donde llegan las tijeras de sus dibujos), y la
 * misma antigüedad en tareas. Lo que el consumidor sabe y la HLE no (qué
 * copió a la RDRAM para el CPU, qué superficie no pudo crear o reciclar)
 * lo resuelve él decodificando la textura (gfx.h). Campos de 32 bits: en
 * el SPE, los de 8 y 16 cuestan varias instrucciones por acceso. */
#define GFX_FB_IMAGES 8
struct gfx_fbimg {
	uint32_t addr;          /* con el bit 0 en 1 si es de 8 bits (va alineada a 64) */
	uint32_t task;          /* última tarea que la dibujó (job->task) */
	uint32_t width;         /* en píxeles; 0: libre */
	uint32_t lh;            /* alto conjeturado, en píxeles */
};

/* El último resultado del combiner (gfx_combine) y sus entradas: el
 * combiner (cc_hi, cc_lo), PRIM, ENV, PRIM_LOD, K4, K5, el tipo de ciclo y
 * si el dibujo lleva niebla. Los estados de dibujo seguidos suelen repetir
 * combiner (las 180 franjas de Perfect Dark, muchos dibujos de Conker) y
 * gfx_combine es caro: en el SPE 0 va con -Os por la pila (rdp.c,
 * combine_cached). El resultado depende solo de esas entradas, así que vale entre
 * tareas. key[5] = 0: vacío. Campos de 32 bits por el SPE. */
struct gfx_ccmemo {
	uint32_t key[6];
	uint32_t approx, nterms, ntex, premul;
	uint8_t factors[GFX_K_TERMS][GFX_K_FACTORS];
	float k[GFX_K_TERMS][3][4];
};

struct gfx_state {
	/* --- RSP (se reinicia por tarea) --- */
	float mv[GFX_MTX_STACK][16];
	int mv_top;
	float proj[16];
	float mvp[16];
	bool mvp_dirty;
	bool lights_dirty;
	bool sp_om_shadow;         /* 1 en la primera generación: G_RDPSETOTHERMODE no toca sp_ol */
	uint32_t segments[16];
	uint32_t geom;
	struct gfx_light lights[GFX_MAX_LIGHTS + 1];
	int num_lights;
	float lookat[2][3];        /* X e Y, normalizados, espacio de la cámara
	                            * (con F3DFLX, Y es la luz del alfa) */
	float light_model[GFX_MAX_LIGHTS + 1][3];  /* en espacio del modelo */
	float lookat_model[2][3];
	float vscale[3], vtrans[3];
	/* G_MW_CLIP: el RSP recorta los triángulos contra el viewport agrandado
	 * por esta razón (x, y); lo de fuera no llega al RDP. */
	float clip_ratio[2];
	float fog_mul, fog_off;
	uint16_t tex_s, tex_t;     /* escala de G_TEXTURE (0.16) */
	uint8_t tex_tile, tex_level, tex_on;
	uint32_t rdphalf1, rdphalf2;
	uint32_t obj_rendermode;   /* G_OBJ_RENDERMODE de S2DEX (s2dex.c) */
	uint32_t sp_ol;            /* othermode L que lleva el microcódigo (G_SETOTHERMODE_L); G_RDPSETOTHERMODE no lo toca (sp_om_shadow) */
	/* Procedencia transitoria de S2DEX: 0 referencia, 1 F3DEX 1.x,
	 * GFX_CHAIN_AFFINE F3DEX2 con escala afín experimental. */
	uint8_t chain_ex;
	uint32_t raw[44];          /* comando del RDP armado con G_RDPHALF_* */
	uint8_t raw_n, raw_len;    /* dobles palabras recibidas y esperadas */
	uint32_t dl_stack[GFX_DL_STACK];
	int dl_sp;
	uint32_t dl_pc;            /* siguiente comando de la display list */
	struct gfx_vtx vtx[GFX_MAX_VTX];
	struct gfx_rare rare;

	/* --- RDP (persiste entre tareas) --- */
	uint32_t oh, ol;           /* othermode H y L */
	uint32_t cc_hi, cc_lo;     /* combiner */
	uint32_t prim, env, fog, blend, fill;  /* 0xRRGGBBAA, fill: como el RDP */
	uint8_t prim_lod, prim_min;
	uint16_t prim_z, prim_dz;
	uint16_t k4, k5;           /* G_SETCONVERT: 9 bits con signo */
	uint16_t k0, k1, k2, k3;   /* matriz YUV del filtro de texturas */
	uint16_t scissor[4];       /* 10.2 */
	uint32_t cimg_addr;
	uint16_t cimg_width;
	uint8_t cimg_fmt, cimg_siz;
	uint32_t zimg_addr;
	uint32_t timg_addr;
	uint16_t timg_width;
	uint8_t timg_fmt, timg_siz;
	struct gfx_tile tiles[8];
	uint32_t tex_gen;          /* cambia con cada carga a la TMEM o cambio de tile */
	struct gfx_load loads[GFX_LOADS];  /* anillo; load_next es la próxima a escribir */
	uint32_t load_next;
	uint32_t vgroups;          /* registros GFX_OP_VGROUP de la tarea (GFX_JOB_INTERP; en el hueco antes de tmem) */
	uint32_t vgroup_bytes;     /* y sus bytes (el otro lugar del hueco) */
	uint8_t tmem[4096] __attribute__((aligned(16)));
	struct gfx_texcache tc;
	/* Fuera del tramo de los estados guardados: al cargar uno se olvida
	 * (gfx_fb_forget), como el consumidor suelta sus destinos. */
	struct gfx_fbimg fbimg[GFX_FB_IMAGES] __attribute__((aligned(16)));  /* una DMA al cambiar de imagen */
	struct gfx_ccmemo ccmemo;

	/* --- Salida de la tarea en curso --- */
	const struct gfx_job *job;
	struct gfx_result *res;
	bool near_clip;            /* recortar contra el plano cercano (sin GFX_JOB_NONEAR) */
	bool dirty;                /* cambió algo del estado de dibujo */
	uint8_t rect_clamp;        /* TEXRECT en curso: 1 limitar s, 2 limitar t; tile + 1 en 4 y 8 */
	uint16_t rect_lim[2];      /* TEXRECT en curso: texels de s y t que muestrea (0: todos) */
	bool target_sent;          /* GFX_OP_TARGET de la color image actual */
	bool have_draw;            /* `draw` está abierto */
	uint16_t draw_prim_z;      /* prim_z con que se abrió (con GFX_DEPTH_PRIM, una por dibujo) */
	int32_t fbimg_cur;         /* su entrada en fbimg (send_target), o -1 */
	uint32_t ucode;            /* texto del microcódigo en curso: el de la tarea o el último G_LOAD_UCODE de su familia */
	struct gfx_draw draw __attribute__((aligned(16)));
	uint32_t vtx_count;        /* vértices escritos en la tarea */
	bool tc_on;                /* la tarea usa la caché de texturas */
	struct gfx_texentry arena_tex[GFX_ARENA_TEXTURES];
	uint32_t narena_tex;
	struct gfx_tilememo tilememo[8];

	void *io;                  /* estado de la plataforma (gfx_io.h) */
	/* Al final, en el relleno: en otro lugar desplazan los demás campos y
	 * el código del SPE 0 crece (y la pila con él). */
	uint32_t flx_table;        /* F3DFLX: la tabla del alfa (G_DMA_IO, segmentada), o 0 */
	uint8_t ucode_var;         /* GFX_UCODE_FLX: el microcódigo en curso */
	struct gfx_detail *detail;  /* fuera del tramo persistente del RDP */
};

/* ---- E/S: la implementa cada plataforma (io_mem.c, src/spu/gfx/io_spu.c) ----
 * Las direcciones son físicas de la RDRAM (ya sin segmento). */

enum gfx_io_slot { GFX_IO_VTX, GFX_IO_MTX, GFX_IO_SMALL, GFX_IO_TEX, GFX_IO_SLOTS };
#define GFX_IO_MAX 4096u  /* bytes por lectura (GFX_IO_TEX); 1 KB los demás */

/* Reloj creciente para medir etapas (0 si la plataforma no lo da). */
uint32_t gfx_io_clock(void);
/* Solo en el SPE (io_spu.c): lo primero que leyó de la display list de la última tarea (diagnóstico, gfx_info). */
void gfx_io_dbg_first(uint32_t *addr, uint32_t w[4]);
/* Prepara la E/S para un trabajo. */
void gfx_io_begin(struct gfx_state *g);
/* Termina las escrituras pendientes y deja en res los bytes escritos. */
void gfx_io_end(struct gfx_state *g);
/* Puntero a `size` bytes de la RDRAM desde `addr`, válido hasta la siguiente
 * lectura con el mismo `slot`; NULL si se sale de la RDRAM. */
const uint8_t *gfx_io_read(struct gfx_state *g, enum gfx_io_slot slot, uint32_t addr, uint32_t size);
/* Escribe `size` bytes en la RDRAM (lo que el microcódigo deja ahí: las partículas de Indiana Jones).
 * Las escrituras salen antes de terminar el trabajo (gfx_io_end). */
void gfx_io_write(struct gfx_state *g, uint32_t addr, const uint8_t *src, uint32_t size);
/* Siguiente comando de la display list (8 bytes). false si se sale. */
bool gfx_io_dl(struct gfx_state *g, uint32_t addr, uint32_t *w0, uint32_t *w1);
/* Lugar para n vértices de salida (≤ 6), o NULL si no caben. */
struct gfx_vertex *gfx_io_vtx(struct gfx_state *g, uint32_t n);
/* Escribe un registro de comando (size múltiplo de 16). false si no cabe:
 * la tarea se corta (GFX_RES_CMD_FULL), salvo un GFX_OP_VGROUP, que solo
 * falta (GFX_RES_VGROUP_FULL). */
bool gfx_io_cmd(struct gfx_state *g, const void *rec, uint32_t size);
/* Área para decodificar una textura de `size` bytes (≤ GFX_TEX_MAX, gfx.h)
 * y su entrega al arena: devuelve el offset o -1 si no cabe. */
uint8_t *gfx_io_tex_begin(struct gfx_state *g, uint32_t size);
int32_t gfx_io_tex_commit(struct gfx_state *g, uint32_t size);
/* Entrega la textura (del área de gfx_io_tex_begin) a la región de la
 * caché, `off` bytes desde su inicio (múltiplo de 128). */
void gfx_io_tex_cache(struct gfx_state *g, uint32_t off, uint32_t size);

/* ---- Módulos ---- */

/* Cada familia de microcódigos decodifica sus comandos en su archivo
 * (f3dex2.c: F3DEX2 y F3DZEX2; f3d.c: F3D y F3DEX 1.x) y llama a lo de
 * abajo con los parámetros ya extraídos. Una variante de una familia pasa
 * un `hook` con sus comandos propios: devuelve true si el comando era suyo
 * (ya resuelto) y false para que siga la familia. */
typedef bool (*gfx_cmd_hook)(struct gfx_state *g, uint32_t w0, uint32_t w1);
void gfx_dl_f3dex2(struct gfx_state *g, gfx_cmd_hook hook);
void gfx_dl_f3dex2acclaim(struct gfx_state *g);
void gfx_dl_f3d(struct gfx_state *g, gfx_cmd_hook hook);
void gfx_dl_turbo3d(struct gfx_state *g);
/* Variantes de Rare: Perfect Dark y Diddy Kong Racing (de F3D) y Conker (de F3DEX2). */
void gfx_dl_f3dpd(struct gfx_state *g);
void gfx_dl_f3dexbg(struct gfx_state *g);
/* Diddy Kong Racing (de F3D, con vértices y triángulos por DMA). */
void gfx_dl_f3ddkr(struct gfx_state *g);
/* Rogue Squadron (Factor 5, de F3D con lista de comandos de longitud variable y mallas de relieve generadas). */
void gfx_dl_f5rogue(struct gfx_state *g);
/* Indiana Jones y Battle for Naboo (Factor 5): una DMEM de 4 KB con DMA, luces, partículas y mallas. */
void gfx_dl_f5indy(struct gfx_state *g);
/* dpc.c: comandos crudos del RDP [job->dl, job->dl_end) (GFX_JOB_DPC). */
void gfx_dl_dpc(struct gfx_state *g);

/* dl.c: recorrido de la display list y estado que el RSP pasa al RDP. */
/* Siguiente comando (avanza dl_pc). false: la tarea termina (fuera de la
 * RDRAM o demasiados comandos). */
bool gfx_dl_next(struct gfx_state *g, uint32_t *w0, uint32_t *w1);
/* G_DL: llamada (guarda el regreso) o salto. false si la pila se llenó. */
bool gfx_dl_call(struct gfx_state *g, uint32_t seg, bool branch);
/* G_ENDDL: regreso a quien llamó. false si terminó la lista principal. */
bool gfx_dl_end(struct gfx_state *g);
/* Salta un tramo de otro microcódigo hasta volver al de la tarea (dl.c). */
bool gfx_dl_skip_foreign(struct gfx_state *g, uint32_t op_ucode, uint32_t op_dl, uint32_t op_enddl);
/* G_LOAD_UCODE a otro microcódigo de la familia F3D de la misma generación
 * (gfx_ucode_kind): el RSP arranca de nuevo con los datos de su DMEM
 * (segmentos, luces, matrices, vértices, modo geométrico, clip ratio), pero
 * la lista y la pila de G_DL siguen. También es el arranque de cada tarea.
 * `text`: su dirección en la RDRAM; `data`, `size`: los datos que carga el
 * G_LOAD_UCODE, de donde sale la variante (gfx_ucode_variant; 0 al empezar
 * la tarea). */
void gfx_ucode_reload(struct gfx_state *g, uint32_t text, int gen, bool nonear, uint32_t data, uint32_t size);
/* F3DFLX (F-Zero X, por el nombre de sus datos): con G_TEXTURE_GEN no genera
 * s y t (quedan las del vértice) sino el alfa, de la tabla de G_DMA_IO
 * (flx_table) por el coseno con la luz del alfa (G_MV_LIGHT en 24, tres s16
 * en los bytes 8 a 13). En flx.c. */
#define GFX_UCODE_FLX 0x1u
unsigned gfx_ucode_variant(struct gfx_state *g, uint32_t data, uint32_t size);
/* G_DMA_IO: con F3DFLX, la dirección de la tabla del alfa. */
void gfx_flx_dma_io(struct gfx_state *g, uint32_t w0, uint32_t w1);
/* Con F3DFLX y G_TEXTURE_GEN, la tabla del alfa (256 bytes en GFX_IO_SMALL;
 * si no, NULL) y el alfa de un vértice por su normal (los 3 bytes s8). */
const uint8_t *gfx_flx_table(struct gfx_state *g);
uint8_t gfx_flx_alpha(const struct gfx_state *g, const uint8_t *table, const uint8_t *nrm);
/* El alfa del sombreado lleva el de la tabla (para la niebla del blender). */
bool gfx_flx_shade_alpha(const struct gfx_state *g);
/* s2dex.c: clase del microcódigo cuyos datos (data, size bytes) carga un
 * G_LOAD_UCODE: 1 o 2 (F3D, F3DEX, F3DLX…: primera o segunda generación, por
 * la versión), GFX_UCODE_S2DEX2, o 0 si es otro (L3DEX, S2DEX 1.x, el de
 * Conker…) o no se reconoce. nonear: es un ".NoN". */
#define GFX_UCODE_S2DEX2 3
#define GFX_UCODE_S2DEX1 4
int gfx_ucode_kind(struct gfx_state *g, uint32_t data, uint32_t size, bool *nonear);
/* El tramo S2DEX2 hasta el G_LOAD_UCODE que vuelve al de la tarea (true: la
 * lista sigue; false como gfx_dl_skip_foreign). */
bool gfx_dl_s2dex2(struct gfx_state *g);
/* Una tarea S2DEX suelta (SALSA64_GBI_S2DEX y S2DEX1): la lista entera, con los
 * opcodes de 2.x o, si gen1, de 1.x. GFX_S2_F3DEX2 o
 * GFX_S2_F3DEX: la lista sigue en ese intérprete (un G_LOAD_UCODE a mitad de
 * tarea, ya recargado). */
#define GFX_S2_END 0
#define GFX_S2_F3DEX2 1
#define GFX_S2_F3DEX 2
int gfx_dl_s2dex(struct gfx_state *g, bool gen1);
/* Como gfx_dl_s2dex2, con los opcodes de S2DEX 1.x. */
bool gfx_dl_s2dex1(struct gfx_state *g);
/* G_TEXRECT y G_TEXRECTFLIP: s, t y dsdx, dtdy llegan en los dos comandos
 * siguientes si sus opcodes están en ops_st y ops_d (hasta 4 opcodes, uno
 * por byte). Si no, el rectángulo se descarta y ese comando sigue normal. */
void gfx_dl_texrect(struct gfx_state *g, uint32_t w0, uint32_t w1, uint32_t ops_st, uint32_t ops_d);
/* Una doble palabra de un comando del RDP que llega por G_RDPHALF_* (alta
 * en G_RDPHALF_1, baja en el siguiente); al completar el comando, lo
 * ejecuta. */
void gfx_dl_rdp_word(struct gfx_state *g, uint32_t hi, uint32_t lo);
/* G_BRANCH_Z: salta a la lista de G_RDPHALF_1 si la z de pantalla del
 * vértice es <= zval. */
void gfx_branch_z(struct gfx_state *g, uint32_t v, uint32_t zval);
/* G_TEXTURE: escala s, t (0.16) en `scale`, nivel, tile y encendido. */
void gfx_sp_texture(struct gfx_state *g, uint32_t scale, unsigned level, unsigned tile, unsigned on);
/* Geometry mode nuevo completo, con los bits de F3DEX2 (abajo). */
void gfx_sp_geometry(struct gfx_state *g, uint32_t geom);
/* G_SETOTHERMODE_H/L: `len` bits desde el bit `shift`. */
void gfx_sp_othermode(struct gfx_state *g, bool high, int shift, int len, uint32_t data);

/* geom.c: matrices, vértices, luces. */
void gfx_geom_reset(struct gfx_state *g);
void gfx_load_matrix(float m[16], const uint8_t *src);
void gfx_mtx_mul(float out[16], const float a[16], const float b[16]);
void gfx_load_vertices(struct gfx_state *g, uint32_t addr, uint32_t v0, uint32_t n);
/* Lo mismo con los n vértices de 16 bytes ya leídos (o armados) en `src`. */
void gfx_vertices(struct gfx_state *g, const uint8_t *src, uint32_t v0, uint32_t n);
/* vgroup.c (con -Os en el SPE 0): lo de GFX_JOB_INTERP, sin local store
 * aparte. gfx_vgroup_begin, al empezar la tarea. gfx_vgroup (sin la bandera
 * no hace nada): el registro GFX_OP_VGROUP de los n vértices recién cargados
 * en vtx[v0…] desde `addr` (`stride` bytes por vértice en la lista del
 * juego; `src`, sus n vértices de 16 bytes como los leyó la HLE, de los que
 * va el hash del contenido) y su identidad en clip. */
void gfx_vgroup_begin(struct gfx_state *g);
void gfx_vgroup(struct gfx_state *g, uint32_t addr, uint32_t stride, uint32_t v0, uint32_t n, const uint8_t *src);
/* Luces y lookat en espacio del modelo (light_model, lookat_model), al día. */
void gfx_light_vectors(struct gfx_state *g);
/* G_TEXTURE_GEN: s, t del vértice según su normal (unitaria, espacio del
 * modelo) y el lookat, con la escala de G_TEXTURE. */
void gfx_texgen_vertex(struct gfx_state *g, const float nrm[3], struct gfx_vtx *v);
/* Suma las luces compactas Acclaim al RGB en flotante, antes de cuantizar. */
void gfx_acclaim_light(const struct gfx_state *g, const struct gfx_vtx *v, float rgb[3]);
void gfx_set_viewport(struct gfx_state *g, const uint8_t *vp);
/* G_MOVEWORD G_MW_CLIP: `offset` dentro de la tabla (RNX 0x04, RNY 0x0C; los
 * positivos RPX y RPY son los mismos con signo contrario). */
void gfx_clip_ratio(struct gfx_state *g, uint32_t offset, uint32_t value);
void gfx_set_light(struct gfx_state *g, int index, const uint8_t *src);
void gfx_set_lookat(struct gfx_state *g, int axis, const uint8_t *src);
/* G_MTX: carga o multiplica (y apila) la modelview o la proyección. */
#define GFX_MTX_PUSH       0x01u
#define GFX_MTX_LOAD       0x02u
#define GFX_MTX_PROJECTION 0x04u
void gfx_matrix(struct gfx_state *g, uint32_t addr, uint32_t flags);
void gfx_pop_matrix(struct gfx_state *g, uint32_t n);
/* Matriz combinada: entera (gSPForceMatrix) o media palabra de dos
 * elementos (G_MW_MATRIX; `where` en bytes dentro del Mtx). */
void gfx_force_matrix(struct gfx_state *g, uint32_t addr);
void gfx_insert_matrix(struct gfx_state *g, uint32_t where, uint32_t val);
void gfx_num_lights(struct gfx_state *g, int n);
void gfx_light_color(struct gfx_state *g, int light, uint32_t rgba);
/* G_MODIFYVTX / G_MW_POINTS: `where` es G_MWO_POINT_* (geom.c). */
void gfx_modify_vertex(struct gfx_state *g, uint32_t v, uint32_t where, uint32_t val);
/* G_CULLDL: true si los vértices v0..v1 quedan todos fuera de un plano. */
bool gfx_vertices_culled(const struct gfx_state *g, uint32_t v0, uint32_t v1);
/* Pantalla → coordenadas homogéneas de salida. */
void gfx_vtx_out(const struct gfx_state *g, const struct gfx_vtx *v, struct gfx_vertex *o);

/* rdp.c: estado y primitivas del RDP. */
void gfx_rdp_reset(struct gfx_state *g);
/* Olvida las color images dibujadas (reset, estado cargado). */
void gfx_fb_forget(struct gfx_state *g);
void gfx_rdp_command(struct gfx_state *g, uint32_t w0, uint32_t w1);
void gfx_triangle(struct gfx_state *g, uint32_t a, uint32_t b, uint32_t c);
void gfx_texrect(struct gfx_state *g, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3, bool flip);
/* Cuadrilátero con textura de esquinas (x, y) en píxeles y (s, t) en texels
 * (rdp.c, solo con GFX_HAS(S2DEX)). */
void gfx_rdp_tex_quad(struct gfx_state *g, int tile, const float *x, const float *y, const float *s, const float *t);
void gfx_flush_draw(struct gfx_state *g);
/* Triángulos crudos del RDP (0xC8–0xCF) que llegan por G_RDPHALF_*: dobles
 * palabras del comando según su opcode y el triángulo con ellas (`w`:
 * pares alta, baja). */
unsigned gfx_rdp_raw_words(uint32_t op);
void gfx_rdp_triangle(struct gfx_state *g, const uint32_t *w);

/* texcache.c: hash de 64 bits por bloques de 16 bytes y caché de texturas
 * entre tareas. */
struct gfx_hash {
	uint32_t acc[8] __attribute__((aligned(16)));
	uint32_t len;
};
void gfx_hash_init(struct gfx_hash *h);
/* Bytes [start, start + len) de un anillo de `size` bytes (potencia de 2,
 * alineado a 16): lo que pase del final sigue desde el principio. */
void gfx_hash_ring(struct gfx_hash *h, const uint8_t *ring, uint32_t size, uint32_t start, uint32_t len);
void gfx_hash_words(struct gfx_hash *h, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
void gfx_hash_final(const struct gfx_hash *h, uint32_t key[2]);

/* Al empezar una tarea: decide si usa la caché (g->tc_on) y la prepara. */
void gfx_tc_begin(struct gfx_state *g);
/* Textura con ese hash (y la marca usada en esta tarea), o NULL. */
const struct gfx_tc_entry *gfx_tc_find(struct gfx_state *g, const uint32_t key[2], bool *old);
/* Lugar para una textura nueva de `size` bytes, o NULL si no hay (todo lo
 * que habría que desalojar lo puede leer aún el consumidor). */
struct gfx_tc_entry *gfx_tc_insert(struct gfx_state *g, const uint32_t key[2], uint32_t size);
/* Directorio vacío sobre la región (ea, cap) con una barrera: mientras el
 * consumidor no termine la tarea `task` (la última de la imagen anterior
 * del SPE 0, que pudo dejar ahí texturas en uso), las texturas nuevas van
 * al arena. */
void gfx_tc_fence(struct gfx_state *g, uint64_t ea, uint32_t cap, uint32_t task);

/* tmem.c: cargas a la TMEM y decodificación de una tile. */
void gfx_load_block(struct gfx_state *g, uint32_t w0, uint32_t w1);
void gfx_load_tile(struct gfx_state *g, uint32_t w0, uint32_t w1);
void gfx_load_tlut(struct gfx_state *g, uint32_t w0, uint32_t w1);
/* Decodifica la tile a RGBA8888 (o la toma de la caché) y llena `tex`
 * salvo flags y xf; con premul, RGB·A (otra entrada de la caché). lim[0] y
 * lim[1], si no son 0, acotan el ancho y el alto a los texels que se
 * muestrean (desde el 0; el eje debe limitarse, no repetir). clamp_s: el
 * dibujo limita s (no repite); con GFX_JOB_FBTEX, una textura que viene
 * de un destino reciente no se decodifica (GFX_TEX_TARGET). false si no
 * hay lugar. */
bool gfx_tile_texture(struct gfx_state *g, int tile, struct gfx_tex *tex, bool premul, const uint16_t lim[2],
		      bool clamp_s);
/* Tamaño de la textura de una tile (potencia de 2 si se repite). */
void gfx_tile_size(const struct gfx_tile *t, uint32_t *w, uint32_t *h);

/* Tramo de gfx_state con lo que persiste entre tareas, alineado a 16 para
 * el DMA: desde othermode (y lo que comparta su bloque de 16 bytes con el
 * final del estado de las variantes de Rare, que se reinicia en cada tarea)
 * hasta el final de la TMEM. El trabajo de estado del SPE lo copia tal cual
 * (gfx.h). */
#define GFX_RAW_START (offsetof(struct gfx_state, oh) & ~(size_t)15)
#define GFX_RAW_END   (offsetof(struct gfx_state, tmem) + sizeof(((struct gfx_state *)0)->tmem))
_Static_assert(GFX_RAW_END % 16 == 0 && GFX_RAW_END - GFX_RAW_START <= 16384,
	       "el tramo del estado del RDP no cabe en una DMA");
_Static_assert(sizeof(((struct gfx_state *)0)->fbimg) == GFX_FB_BYTES &&
		       offsetof(struct gfx_state, fbimg) % 16 == 0,
	       "las color images dibujadas no van en una DMA de GFX_FB_BYTES");

/* state.c: lo que persiste entre tareas, en el formato portable de
 * GFX_STATE_* (gfx.h): los registros solos (el SPE manda la TMEM por DMA)
 * y, en sync.c (fuera del SPE), todo junto (GFX_STATE_BYTES). false si el
 * formato no es este. */
void gfx_state_pack_regs(const struct gfx_state *g, uint8_t out[GFX_STATE_REGS]);
bool gfx_state_unpack_regs(struct gfx_state *g, const uint8_t in[GFX_STATE_REGS]);
void gfx_state_save(const struct gfx_state *g, uint8_t *out);
bool gfx_state_load(struct gfx_state *g, const uint8_t *in);

/* combine.c: combiner → combinación lineal fija o términos del generador
 * (gfx.h). Lee d->fog (con niebla deja un término sin factores). Devuelve
 * true si tuvo que aproximar. */
bool gfx_combine(const struct gfx_state *g, struct gfx_draw *d);
/* Deja el dibujo con solo `ntex` texturas: anula (combinación lineal) o
 * quita (términos) lo que leía las que faltan. */
void gfx_combine_textures(struct gfx_draw *d, unsigned ntex);
void gfx_combine_untextured(struct gfx_draw *d);

/* Segmentada → física. */
static inline uint32_t gfx_addr(const struct gfx_state *g, uint32_t seg)
{
	return (g->segments[(seg >> 24) & 15] + (seg & 0x00FFFFFF)) & 0x00FFFFFF;
}

/* Bits de geometry mode de F3DEX2: los que guarda gfx_state.geom con
 * cualquier microcódigo (f3d.c traduce los suyos). */
#define G_ZBUFFER            0x00000001u
#define G_SHADE              0x00000004u
#define G_ACCLAIM_LIGHTING   0x00000080u
#define G_CULL_FRONT         0x00000200u
#define G_CULL_BACK          0x00000400u
#define G_FOG                0x00010000u
#define G_LIGHTING           0x00020000u
#define G_TEXTURE_GEN        0x00040000u
#define G_TEXTURE_GEN_LINEAR 0x00080000u
#define G_LOD                0x00100000u
#define G_SHADING_SMOOTH     0x00200000u
#define G_POINT_LIGHTING     0x00400000u
#define G_CLIPPING           0x00800000u

/* othermode H */
#define G_MDSFT_CYCLETYPE  20
#define G_MDSFT_TEXTPERSP  19
#define G_MDSFT_TEXTLOD    16
#define G_MDSFT_TEXTLUT    14
#define G_MDSFT_TEXTFILT   12
#define G_CYC_1CYCLE 0
#define G_CYC_2CYCLE 1
#define G_CYC_COPY   2
#define G_CYC_FILL   3
#define G_TT_NONE    0
#define G_TT_RGBA16  2
#define G_TT_IA16    3
#define G_TF_POINT   0
#define G_TF_AVERAGE 3
#define G_TF_BILERP  2

/* othermode L */
#define G_AC_MASK        0x3u
#define G_AC_THRESHOLD   0x1u
#define G_ZS_PRIM        0x4u
#define AA_EN            0x0008u
#define Z_CMP            0x0010u
#define Z_UPD            0x0020u
#define IM_RD            0x0040u
#define CLR_ON_CVG       0x0080u
#define CVG_DST_MASK     0x0300u
#define CVG_DST_CLAMP    0x0000u
#define CVG_DST_WRAP     0x0100u
#define CVG_DST_FULL     0x0200u  /* «ZAP»: 7 */
#define CVG_DST_SAVE     0x0300u
#define ZMODE_MASK       0x0C00u
#define ZMODE_OPA        0x0000u
#define ZMODE_INTER      0x0400u
#define ZMODE_XLU        0x0800u
#define ZMODE_DEC        0x0C00u
#define CVG_X_ALPHA      0x1000u
#define ALPHA_CVG_SEL    0x2000u
#define FORCE_BL         0x4000u

/* Formatos de imagen. */
enum { G_IM_FMT_RGBA = 0, G_IM_FMT_YUV = 1, G_IM_FMT_CI = 2, G_IM_FMT_IA = 3, G_IM_FMT_I = 4 };
enum { G_IM_SIZ_4b = 0, G_IM_SIZ_8b = 1, G_IM_SIZ_16b = 2, G_IM_SIZ_32b = 3 };

#endif
