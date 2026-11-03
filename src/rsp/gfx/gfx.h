/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — HLE de los microcódigos gráficos (F3D, F3DEX, F3DEX2) y del
 * RDP: interfaz.
 *
 * El mismo código corre en un SPE (src/spu/gfx, la configuración normal en
 * la PS3), en el PPE y en el host (backend síncrono, pruebas). Lee la
 * display list de la RDRAM y produce, por tarea:
 *
 *   vértices   gfx_vertex (32 bytes), en el formato que lee el RSX: posición
 *              homogénea en píxeles de la N64, s/t en texels y color
 *   comandos   registros gfx_cmd: cambios de destino (color image), borrado
 *              de profundidad y dibujos (gfx_draw: estado completo + un tramo
 *              de vértices, triángulos sueltos)
 *   texturas   texels RGBA8888 decodificados de la TMEM, en la caché de
 *              texturas (persiste entre tareas: ver «Caché de texturas»)
 *              o, si no hay lugar, en el arena de la tarea
 *
 * Quien consume (src/rdp en el PPE) traduce los comandos a lib/rsxkit. Los
 * registros se escriben en el orden de bytes de quien produce y se leen en
 * el mismo (SPU y PPE son big-endian).
 *
 * Nada aquí depende de PSL1GHT: todo compila con gcc nativo.
 */
#ifndef SALSA64_RSP_GFX_GFX_H
#define SALSA64_RSP_GFX_GFX_H

#include <stdint.h>

/* ---- Trabajo y resultado ---------------------------------------------------
 * Caben en el descriptor y el resultado de 112 bytes de lib/spukit. */

#define GFX_JOB_TASK 1  /* kind del trabajo en spukit */
#define GFX_JOB_HASH 2  /* prueba del hash (struct gfx_hash_job), antes de las tareas */
/* Estados guardados, entre tareas: el SPE copia tal cual el tramo de su
 * gfx_state con lo que persiste entre tareas (GFX_RAW_START a GFX_RAW_END de
 * hle.h: registros del RDP, tiles y TMEM) a `ea`, o lo trae de ahí, y
 * contesta dónde empieza y termina ese tramo en su struct (dos uint32_t) para
 * que el PPE compruebe que la disposición es la misma. El formato portable
 * (GFX_STATE_BYTES: GFX_STATE_REGS bytes de registros y la TMEM, state.c) lo
 * arma el PPE: en el SPE no sobra local store. */
#define GFX_JOB_STATE_SAVE 3
#define GFX_JOB_STATE_LOAD 4
#define GFX_STATE_REGS  256u
#define GFX_STATE_BYTES (GFX_STATE_REGS + 4096u)

/* Al cambiar de imagen del SPE 0 (rsp/gfx/spe0.h) el PPE usa los mismos
 * trabajos con fb_ea: además del tramo, las color images dibujadas (fbimg,
 * GFX_FB_BYTES en fb_ea, alineado a 16) van y vuelven, y al cargar, con
 * cache_ea, el directorio de la caché queda vacío con la barrera de
 * fence_task (gfx_tc_fence en hle.h). Con fb_ea = 0, solo el tramo. */
struct gfx_state_job {
	uint64_t ea;
	uint64_t fb_ea;
	uint64_t cache_ea;
	uint32_t cache_cap;
	uint32_t fence_task;
};
#define GFX_FB_BYTES 128u

/* Qué programa es (src/spu/gfx/main.c): imagen, entradas de la caché de
 * texturas, dónde termina el programa (_end), el tope de la pila y los
 * bytes de pila que nunca se tocaron desde que arrancó (con el patrón que
 * pone al arrancar si se le pide; si no, los que siguen en cero). */
#define GFX_JOB_INFO 5
#define GFX_JOB_CLIP4 6  /* autoprueba de recorte SIMD: solo variante experimental */
struct gfx_clip4_job { float x[4], y[4], z[4], w[4]; };
struct gfx_info {
	uint32_t image;
	uint32_t tc_entries;
	uint32_t end;
	uint32_t stack_top;
	uint32_t untouched;
	uint32_t raw_start, raw_end;
	uint32_t filled;     /* 1: la pila se llenó con el patrón; 0: se cuentan ceros (aproximado) */
	/* Diagnóstico: lo primero que el SPE leyó de la display list de la última tarea
	 * (su dirección y las 8 primeras palabras), para compararlo con lo que el PPE ve
	 * de la RDRAM al terminar (gfx_spu.c, log_bad_task). */
	uint32_t dl_addr;
	uint32_t dl_first[4];
};

/* Hash de `len` bytes desde `start` de un anillo de `size` bytes (≤ 4096,
 * potencia de 2) en `ea`; el resultado son las dos palabras de la clave. */
struct gfx_hash_job {
	uint64_t ea;
	uint32_t size, start, len;
};

struct gfx_job {
	uint64_t rdram_ea;   /* dirección de la RDRAM emulada (EA del PPE) */
	uint64_t vtx_ea;     /* salida: vértices */
	uint64_t cmd_ea;     /* salida: comandos */
	uint64_t tex_ea;     /* salida: arena de texturas */
	uint64_t cache_ea;   /* región de la caché de texturas (0: sin caché) */
	uint32_t rdram_size;
	uint32_t vtx_cap;    /* capacidades en bytes */
	uint32_t cmd_cap;
	uint32_t tex_cap;
	uint32_t cache_cap;
	uint32_t gbi;        /* enum salsa64_gbi */
	uint32_t dl;         /* data_ptr del OSTask: primera display list */
	uint32_t flags;      /* GFX_JOB_* */
	/* Caché de texturas: número de esta tarea (creciente) y la última tarea
	 * cuyos dibujos ya no leen texturas (el consumidor la terminó). */
	uint32_t task;
	uint32_t done_task;
	/* Dirección física del microcódigo de la tarea (ucode del OSTask): un
	 * G_LOAD_UCODE a otra dirección es otro microcódigo (S2DEX…), cuyos
	 * comandos se saltan hasta que la lista vuelve a cargar este. Con 0
	 * (no se sabe cuál es) todo G_LOAD_UCODE es ajeno. */
	uint32_t ucode;
	/* RDP a bajo nivel (GFX_JOB_DPC): comandos [dl, dl_end) de la RDRAM
	 * o, con GFX_JOB_XBUS, de la DMEM en dmem_ea (4 KB). */
	uint32_t dl_end;
	uint64_t dmem_ea;
	/* Diagnóstico opcional externo; cero no reserva ni transfiere detalle. */
	uint64_t detail_ea;
	uint32_t detail_cap, detail_version;
};

#define GFX_DETAIL_VERSION 3u
/* Todos los campos son palabras sin punteros; tamaño múltiplo de 16.
 * Tiempos anidados: DMA está incluido en las etapas; no sumarlo otra vez. */
struct gfx_detail {
	uint32_t version, task, bytes, commands;
	uint32_t reads[4], requested[4], transferred[4], read_ticks[4];
	uint32_t send_ticks[4];  /* envío; read_ticks mide la espera, ambos anidados */
	uint32_t dl_fetches, dl_bytes, dl_hits, dl_misses;
	uint32_t dma_puts[3], dma_bytes[3], wait_ticks[3];
	uint32_t vertex_calls, vertices, lit_vertices, fog_vertices, texgen_vertices;
	uint32_t triangles_in, triangles_ok, triangles_common, triangles_near, triangles_clipped;
	uint32_t vtx_bytes, cmd_bytes, tex_bytes, state_builds;
	/* Partición exclusiva: vtx/state/load del resultado + triangle/matrix/
	 * dl/end/rest. triangle excluye state; los demás no se solapan. */
	uint32_t t_total, t_vtx, t_state, t_load, t_triangle, t_matrix, t_dl, t_end, t_rest;
	/* Solo imagen 1: ticks de la matriz del lote de cuatro. El resto de las
	 * fases se deriva de t_vtx y de los contadores de E/S para no crecer el
	 * marco de detalle en la pila del SPE. Los motivos escalares son
	 * excluyentes salvo scalar_texgen, que es subconjunto de luz. */
	uint32_t vector_vertices, scalar_forced, scalar_lighting, scalar_texgen;
	uint32_t scalar_no_fog, scalar_tail, vector_batches, vtx_transform;
};
_Static_assert(sizeof(struct gfx_job) == 112, "descriptor gráfico de 112 bytes");
_Static_assert(sizeof(struct gfx_detail) % 16 == 0, "detalle apto para DMA");

#define GFX_JOB_RESET 0x1u  /* olvidar el estado del RDP (reset de la consola) */
#define GFX_JOB_NOCACHE 0x2u  /* no usar ni llenar la caché (para medir) */
#define GFX_JOB_DPC 0x4u  /* comandos del RDP crudos (dpc.c), no una display list */
#define GFX_JOB_XBUS 0x8u  /* con GFX_JOB_DPC: los comandos están en la DMEM */
/* El microcódigo no recorta contra el plano cercano (los ".NoN" de su
 * texto: Zelda OoT, Star Fox 64, Conker; y el de Perfect Dark, sync.c). */
#define GFX_JOB_NONEAR 0x10u
/* El consumidor lee del RSX las texturas que vienen de una color image
 * dibujada hace poco (render a textura) y sabe decodificar él mismo las que
 * al final no pueda leer de ahí: la HLE no decodifica esas (GFX_TEX_TARGET;
 * ver «Texturas desde un destino»). */
#define GFX_JOB_FBTEX 0x20u
/* Interpolación de cuadros por geometría (docs/interpolacion.md): cada
 * G_VTX deja un registro GFX_OP_VGROUP y cada vértice de salida dice de qué
 * grupo y lugar salió (gfx_vertex.pad, GFX_VGID). Sin la bandera no se
 * escribe nada de eso (pad = 0). Los registros ocupan a lo más un tercio
 * de cmd_cap (GFX_VGROUP_SHARE): el consumidor da ese tercio de más, así los
 * dibujos tienen el mismo lugar que sin la bandera y la tarea nunca se
 * corta por ella; lo que no cabe no lleva registro ni identidad
 * (GFX_RES_VGROUP_FULL) y esa tarea no se interpola. */
#define GFX_JOB_INTERP 0x40u
#define GFX_VGROUP_SHARE 3u
/* Con GFX_JOB_DPC: este tramo sigue una lista del RDP que aún no vio su
 * SYNC_FULL (el microcódigo real entrega su salida por tramos, a veces de
 * un comando). Es la misma tarea para el consumidor: no envejece a los
 * destinos ni cambia el número de tarea (si no, 9 tramos sin dibujar en el
 * destino bastaban para que volviera a cebarse desde la RDRAM y borrara lo
 * dibujado). */
#define GFX_JOB_DPC_CONT 0x80u
/* Fuerza el recorrido escalar de geometría en el SPE 0. Es un interruptor
 * de diagnóstico A/B: el PPE lo pone al abrir el juego si existe
 * gfx-escalar en el directorio de datos. */
#define GFX_JOB_SCALAR 0x100u
/* Comparación optativa: escala afín de los triángulos F3DEX2
 * cargados dentro de una tarea S2DEX. No escala G_MODIFYVTX. */
#define GFX_JOB_AFFINE_CHAIN 0x1000u
#define GFX_JOB_S2DEX_OLD_MODES 0x2000u
/* Interruptor: no tomar como niebla la mezcla FOG·A_SHADE + IN·(1 − A) en
 * un ciclo con FORCE_BL (la pista de F-Zero X; rdp.c blend_state). El PPE
 * lo pone si existe sin-niebla-1ciclo en el directorio de datos. */
#define GFX_JOB_NO_FOG_1CYCLE 0x4000u
/* Interruptor: no recortar los triángulos contra la caja del clip ratio
 * como el RSP (rdp.c gfx_triangle): queda solo el plano cercano, como
 * antes. Va con la sombra afín del consumidor (sin-sombra-afin en el
 * directorio de datos apaga las dos cosas; en el host,
 * SALSA64_SIN_SOMBRA_AFIN=1). */
#define GFX_JOB_NO_RSP_CLIP 0x8000u
/* Diagnóstico: vuelve al negro opaco usado antes para las texturas YUV. */
#define GFX_JOB_NO_YUV 0x10000u
/* Diagnóstico: fondos escalados con el marco sin limitar a la imagen. */
#define GFX_JOB_S2DEX_BG_OLD 0x20000u
/* Diagnóstico: el blender solo con los cuatro modos de antes (todo lo demás
 * como IN·α + MEM·(1 − α)); los productos nuevos de gfx_blend no salen. */
#define GFX_JOB_BLEND_OLD 0x40000u
/* Interruptor: el paso entre filas de los fondos de S2DEX (G_BG_COPY,
 * G_BG_1CYC) vuelve a ser imageW texels en vez de imageW redondeado hacia
 * abajo a palabras de 64 bits, como el microcódigo (s2dex.c bg_stride;
 * Worms Armageddon). El PPE lo pone si existe s2dex-paso-viejo. */
#define GFX_JOB_S2DEX_BG_ROW_OLD 0x80000u
/* Interruptor: la lista secundaria de Factor 5 (Indiana Jones, f5indy.c)
 * sigue hasta el B8 000000 FFFFFFFF aunque antes haya otro G_ENDDL, como
 * antes del 2026-10-29. El PPE lo pone si existe f5-sublista-vieja. */
#define GFX_JOB_F5_SUB_OLD 0x100000u
/* Valor interno de chain_ex; no forma parte del descriptor ni del estado RDP. */
#define GFX_CHAIN_AFFINE 2u

/* ---- Caché de texturas -------------------------------------------------------
 * Las texturas decodificadas viven en una región de memoria que pone el
 * consumidor (job->cache_ea, cache_cap; vista por el RSX) y que la HLE
 * administra sola entre tareas: cada textura se identifica por un hash de
 * los bytes de la TMEM que lee su decodificación (texels y paleta) y de los
 * parámetros de la tile, así que un cambio en la RDRAM de origen o en la
 * paleta da otra textura. Una textura se puede sobrescribir solo si la
 * última tarea que la usó es <= job->done_task: el consumidor debe dar en
 * done_task una tarea cuyos dibujos ya terminó (en la PS3, la barrera del
 * RSX) y, para los volcados, nunca la tarea anterior (task − 1). Un mismo
 * consumidor debe numerar sus tareas sin repetir; si cambia la región, la
 * caché se vacía. */

struct gfx_result {
	uint32_t vtx_bytes, cmd_bytes, tex_bytes;
	uint32_t flags;          /* GFX_RES_* */
	uint32_t commands;       /* comandos de display list ejecutados */
	uint32_t triangles;
	uint32_t rects;
	uint32_t draws;          /* registros gfx_draw */
	uint32_t textures;       /* texturas decodificadas */
	uint32_t tex_reused;     /* usos de una textura ya decodificada */
	uint32_t unknown_cmds;   /* comandos sin implementar */
	uint32_t last_unknown;   /* opcode del último */
	uint32_t approx_cc;      /* combiners aproximados */
	uint32_t dl_errors;      /* direcciones fuera de la RDRAM, pila llena */
	/* Tiempo por etapa en ticks de gfx_io_clock (en el SPE, el timebase;
	 * en el host y el PPE, 0): total, vértices, estado de dibujo (combiner
	 * y texturas, incluidos t_hash y t_tex), decodificación de texturas y
	 * cargas a la TMEM. */
	uint32_t t_total, t_vtx, t_state, t_tex, t_load;
	uint32_t loads;          /* cargas a la TMEM (LOADBLOCK, LOADTILE, LOADTLUT) */
	uint32_t load_reads;     /* lecturas de la RDRAM para esas cargas */
	uint32_t tex_old;        /* de las reusadas, decodificadas en tareas anteriores */
	uint16_t tex_arena;      /* decodificadas en el arena (sin lugar en la caché; da la vuelta en 65536) */
	uint16_t tex_evicted;    /* texturas sacadas de la caché (ídem) */
	uint32_t vertices;       /* vértices cargados (G_VTX y los de cada microcódigo): ritmo del RCP */
	uint32_t t_hash;         /* ticks identificando texturas (hash y búsqueda) */
	uint32_t skipped;        /* comandos saltados de otro microcódigo (G_LOAD_UCODE) */
	uint32_t bg;             /* fondos de S2DEX dibujados (G_BG_COPY, G_BG_1CYC) */
	uint32_t tex_target;     /* texturas sin decodificar: se leen de un destino (GFX_TEX_TARGET) */
};

#define GFX_RES_FULLSYNC     0x01u  /* G_RDPFULLSYNC: interrupción del DP */
#define GFX_RES_VTX_FULL     0x02u  /* se acabó un búfer de salida */
#define GFX_RES_CMD_FULL     0x04u
#define GFX_RES_TEX_FULL     0x08u
#define GFX_RES_LOOP         0x10u  /* demasiados comandos: se cortó */
#define GFX_RES_LOAD_UCODE   0x20u  /* G_LOAD_UCODE a otro microcódigo: se saltó su tramo */
#define GFX_RES_FOREIGN_END  0x40u  /* la lista terminó dentro de ese tramo (sin volver) */
#define GFX_RES_VGROUP_FULL  0x80u  /* GFX_JOB_INTERP: faltan registros GFX_OP_VGROUP (no se corta) */
#define GFX_RES_F5_SUB_END   0x100u /* Factor 5: una lista secundaria acabó en un G_ENDDL que no es B8 … FFFFFFFF */
/* La lista no llegó a su fin (o acabó donde no se sabe qué hizo): el juego
 * sigue esperando al RDP. */
#define GFX_RES_CUT (GFX_RES_VTX_FULL | GFX_RES_CMD_FULL | GFX_RES_TEX_FULL | GFX_RES_LOOP | GFX_RES_FOREIGN_END)

/* ---- Vértice de salida -------------------------------------------------------
 * x, y: píxeles del framebuffer de la N64 multiplicados por w; z: profundidad
 * en [0, 1] multiplicada por w; w: la w de recorte (positiva delante de la
 * cámara). s, t: coordenadas de textura en texels (ya con la escala de
 * G_TEXTURE, antes de la tile). Color en bytes R, G, B, A. */
struct gfx_vertex {
	float x, y, z, w;
	float s, t;
	uint8_t r, g, b, a;
	uint32_t pad;
};

/* ---- Comandos -----------------------------------------------------------------
 * Registros de tamaño múltiplo de 16 bytes; cada uno empieza con gfx_cmd. */

enum gfx_op {
	GFX_OP_END = 0,
	GFX_OP_TARGET,  /* a = dirección física de la color image, b = ancho,
	                   c = formato << 8 | tamaño (G_IM_FMT_*, G_IM_SIZ_*) */
	GFX_OP_ZCLEAR,  /* a = dirección de la z image, b = valor (16 bits de la
	                   N64), c = tamaño de píxel de la color image
	                   (G_IM_SIZ_*) << 16 | su ancho; gfx_zclear */
	GFX_OP_DRAW,    /* gfx_draw */
	GFX_OP_VGROUP,  /* gfx_vgroup (solo con GFX_JOB_INTERP) */
};

struct gfx_cmd {
	uint16_t op;
	uint16_t size;  /* bytes del registro completo */
	uint32_t a, b, c;
};

/* El relleno también va a la RDRAM, como lo deja el RDP: el CPU puede leer
 * la z image (salsa64_gfx_rdram_fills, rsp/gfx/backend.h). */
struct gfx_zclear {
	struct gfx_cmd h;
	uint16_t rect[4];  /* x0, y0, x1, y1 en píxeles (x1, y1 exclusivos) */
	uint32_t fill;     /* color de relleno del RDP (32 bits, como G_SETFILLCOLOR) */
	uint32_t pad;
};

/* Textura de un dibujo: texels RGBA8888 (bytes R, G, B, A) en el arena de
 * la tarea o en la región de la caché.
 *
 * Texturas desde un destino (GFX_TEX_TARGET, solo con GFX_JOB_FBTEX): una
 * RGBA16 cuyo origen cae en una color image de 16 bits, o una I8 que cae en
 * una de 8 bits (src_siz 1), que la HLE vio dibujar hace GFX_FB_FRESH
 * tareas o menos no se decodifica: no hay texels (offset y key valen 0) y
 * el consumidor la lee de su destino (render a textura; la I8, con el rojo
 * en los cuatro canales). Si no puede (para él el destino es viejo, es del
 * CPU o no tiene superficie), la decodifica él: width × height texels
 * RGBA16 (o I8) de la RDRAM desde src_addr con src_pitch texels por fila, a
 * RGBA8888 con `pitch` bytes por fila, lo mismo que habría dado la HLE con
 * la RDRAM de ahora. */
struct gfx_tex {
	uint32_t offset;    /* desde el inicio del arena o de la región */
	uint16_t width, height;
	uint16_t pitch;     /* bytes por fila */
	uint8_t flags;      /* GFX_TEX_* */
	uint8_t where;      /* GFX_TEX_ARENA, GFX_TEX_CACHE o GFX_TEX_TARGET */
	uint32_t key;       /* identidad de la textura (32 bits del hash) */
	/* s, t (texels de la N64) → coordenadas normalizadas de la textura:
	 * u = s * xf[0] + xf[2], v = t * xf[1] + xf[3]. */
	float xf[4];
	/* De dónde salió: la carga a la TMEM (LOADBLOCK, LOADTILE) de la que
	 * lee la tile. src_addr es la dirección física del texel (0, 0) de la
	 * textura en la imagen de origen (G_SETTIMG), src_pitch los texels por
	 * fila de esa imagen y src_siz/src_fmt su tamaño y formato (G_IM_*).
	 * src_addr = 0: no se sabe. El consumidor decide si esa imagen es un
	 * destino que él mismo dibujó (render a textura). */
	uint32_t src_addr;
	uint16_t src_pitch;
	uint8_t src_siz, src_fmt;
};

/* GFX_WRAP_* por eje. Al salir de gfx_tile_texture (antes de que rdp.c
 * ponga los de la tile), un eje con clamp decodificado en un periodo de su
 * máscara (tmem.c, tex_size) trae el que debe usar. */
#define GFX_TEX_WRAP_S(f)   ((f) & 3u)         /* GFX_WRAP_* */
#define GFX_TEX_WRAP_T(f)   (((f) >> 2) & 3u)
#define GFX_TEX_BILINEAR    0x10u
#define GFX_TEX_PREMUL      0x20u  /* texels con RGB·A (combine.c) */
/* La textura es el segundo periodo (el reflejado) de una tile con espejo y
 * clamp más grande que su máscara (tmem.c): el eje va con
 * GFX_WRAP_MIRROR_ONCE y u o v corridos un periodo (rdp.c). */
#define GFX_TEX_ONCE_S      0x40u
#define GFX_TEX_ONCE_T      0x80u
enum { GFX_TEX_ARENA = 0, GFX_TEX_CACHE = 1, GFX_TEX_TARGET = 2 };
/* Ninguna textura pasa de tantos bytes (pitch × height): las que no caben
 * pierden filas (el área de decodificación del SPE). */
#define GFX_TEX_MAX 32768u
/* Tareas que una color image dibujada sigue leyéndose del RSX como textura:
 * la misma cuenta que RDP_TARGETS_FRESH del consumidor (src/rdp/targets.h). */
#define GFX_FB_FRESH 3
/* GFX_WRAP_MIRROR_ONCE: refleja una vez alrededor de 0 (u en [−1, 0] es el
 * espejo de [0, 1]) y fija el borde más allá, como el RDP con espejo y
 * clamp en una tile de dos periodos. */
enum { GFX_WRAP_CLAMP = 0, GFX_WRAP_REPEAT = 1, GFX_WRAP_MIRROR = 2, GFX_WRAP_MIRROR_ONCE = 3 };

/* Lo que evalúa el RSX por píxel (lib/rsxkit, «Dibujo 3D»), con
 *   X_j = k[j][0] + k[j][1] * C + k[j][2] * C.aaaa     (C: color del vértice)
 * y T0, T1 las dos texturas:
 * - nterms = 0, la combinación lineal fija (programas precompilados):
 *   color = T0 * X_0 + T0.aaaa * X_1 + T1 * X_2 + T1.aaaa * X_3 + X_4.
 *   Cubre exactamente los combiners de la N64 que no multiplican una
 *   textura por otra ni el sombreado por sí mismo.
 * - nterms > 0, un fragment program generado para estos términos:
 *   color = Σ_j (Π factors[j]) * X_j, con los factores GFX_F_* (los mismos
 *   valores que rsxkit/fpgen.h). Cubre el resto (combine.c). */
enum { GFX_K_T0, GFX_K_T0A, GFX_K_T1, GFX_K_T1A, GFX_K_ONE, GFX_K_LINCOMB };
#define GFX_K_TERMS   7  /* términos como máximo (y de la combinación lineal: los 5 primeros) */
#define GFX_K_FACTORS 4  /* factores por término */
enum { GFX_F_NONE = 0, GFX_F_T0, GFX_F_T0A, GFX_F_T1, GFX_F_T1A, GFX_F_C, GFX_F_CA };

/* Modo de mezcla con el framebuffer. */
enum gfx_blend {
	GFX_BLEND_OFF = 0,   /* opaco */
	GFX_BLEND_ALPHA,     /* src * a + dst * (1 - a) */
	GFX_BLEND_ADD,       /* src * a + dst */
	GFX_BLEND_KEEP,      /* no escribe color (solo profundidad) */
	/* Los demás productos del blender del RDP que el RSX hace tal cual
	 * (rdp.c, classify_blend): un juego oscurece lo dibujado con una
	 * pasada MEM·A_IN y suma el color encima con otra IN + MEM (las
	 * explosiones, el humo y el resplandor del sol de Vigilante 8). */
	GFX_BLEND_MUL,       /* dst * a */
	GFX_BLEND_ADD_MUL,   /* src + dst * a */
	GFX_BLEND_ALPHA_INV, /* src * (1 - a) + dst * a */
	GFX_BLEND_ADD_ONE,   /* src + dst */
};

#define GFX_DEPTH_TEST   0x01u
#define GFX_DEPTH_WRITE  0x02u
#define GFX_DEPTH_DECAL  0x04u  /* ZMODE_DEC: desplazamiento de polígono */
/* G_ZS_PRIM: el RDP prueba y escribe la misma z (la de G_SETPRIMDEPTH) en
 * todos los píxeles del dibujo; los vértices la traen como z = prim · w.
 * Pokémon Snap dibuja así todo el escenario: el orden lo da la lista y la
 * prueba (≤) deja pasar al último. El consumidor no debe sacarla de los
 * vértices dividiendo entre w (cada uno redondea distinto y dos dibujos
 * con la misma z se pisan al azar): usa gfx_prim_z. */
#define GFX_DEPTH_PRIM   0x08u
/* ZMODE_XLU con la z del primitivo: el RDP solo deja pasar lo que queda
 * estrictamente delante (o lo que cae donde el z-buffer sigue borrado); con
 * la misma z, no. Snap dibuja sus capas translúcidas (setos, neblina) con
 * la misma z del primitivo que el escenario ya escrito: el RDP las
 * descarta, con ≤ se veían como cajas. */
#define GFX_DEPTH_LESS   0x10u

/* La z de un dibujo con GFX_DEPTH_PRIM, exacta: la de su primer vértice
 * delante de la cámara, en la rejilla de G_SETPRIMDEPTH (15 bits). */
static inline float gfx_prim_z(const struct gfx_vertex *v, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		if (v[i].w > 0.0f) {
			float z = v[i].z / v[i].w * 32767.0f;
			z = z < 0.0f ? 0.0f : z > 32767.0f ? 32767.0f : z;
			return (float)(int32_t)(z + 0.5f) / 32767.0f;
		}
	return 0.0f;
}

enum { GFX_CULL_NONE = 0, GFX_CULL_FRONT = 1, GFX_CULL_BACK = 2 };
enum { GFX_ATEST_NONE = 0, GFX_ATEST_GEQUAL = 1 };

/* gfx_draw.cvg_bit: el bit de alfa que el RDP deja en una color image de 16
 * bits (RGBA5551) en los píxeles que escribe. No es el alfa de la mezcla: en
 * los ciclos 1 y 2 es la cobertura (rdp.c, cvg_bit); en FILL, el bit del
 * color de relleno, y en COPY, el del texel. Con un píxel cubierto entero
 * (el RSX no tiene cobertura parcial):
 *  - GFX_CVG_ONE: queda en 1 (CVG_DST_CLAMP y ZAP, y WRAP y SAVE sin IM_RD);
 *  - GFX_CVG_KEEP: el de la memoria no cambia (WRAP y SAVE con IM_RD);
 *  - GFX_CVG_ALPHA: el alfa del fragmento decide (FILL, COPY, y CLAMP con
 *    CVG_X_ALPHA, donde la cobertura es cvg·α);
 *  - GFX_CVG_MAX: 1 si ya lo era o si el alfa del fragmento llega a la mitad
 *    (CLAMP con CVG_X_ALPHA, FORCE_BL e IM_RD: cvg·α + la de la memoria).
 * docs/framebuffer.md, «El bit de alfa es la cobertura». */
enum { GFX_CVG_ONE = 0, GFX_CVG_KEEP = 1, GFX_CVG_ALPHA = 2, GFX_CVG_MAX = 3 };

struct gfx_draw {
	struct gfx_cmd h;        /* a = primer vértice, b = número de vértices */
	uint32_t zimg;           /* z image en uso (dirección física) */
	uint16_t scissor[4];     /* x0, y0, x1, y1 en píxeles ×4 (10.2) */
	uint8_t cycle;           /* G_CYC_*: 0 1CYCLE, 1 2CYCLE, 2 COPY, 3 FILL */
	uint8_t cull;            /* GFX_CULL_* (en coordenadas de pantalla de la N64) */
	uint8_t depth;           /* GFX_DEPTH_* */
	uint8_t blend;           /* gfx_blend */
	uint8_t atest;           /* GFX_ATEST_* */
	uint8_t aref;            /* referencia de la prueba de alfa (0–255); en un relleno (G_CYC_FILL), su alfa */
	uint8_t fog;             /* 1: mezclar el color con fog_color según C.a */
	uint8_t ntex;            /* texturas usadas: 0, 1 o 2 */
	uint32_t fog_color;      /* 0xRRGGBBAA */
	uint32_t prim_kind;      /* informativo: 0 triángulos, 1 rectángulos, 2 relleno */
	uint16_t prim_tile;      /* informativo: tile de los rectángulos */
	uint8_t cvg_bit;         /* GFX_CVG_*: el bit de alfa que deja en una imagen de 16 bits */
	uint8_t pad_cvg;
	uint8_t nterms;          /* 0: combinación lineal fija; si no, términos con `factors` */
	uint8_t factors[GFX_K_TERMS][GFX_K_FACTORS];  /* GFX_F_*, 0 = fin de la lista */
	/* G_RM_VISCVG: el RDP escribe la cobertura de cada píxel como color
	 * (IM_RD | FORCE_BL, blender IN·0 + BL·A_MEM); Zelda lo usa para que
	 * el CPU filtre los bordes del fondo de la pausa. El consumidor no lo
	 * dibuja: deja «cobertura plena» en la RDRAM (docs/framebuffer.md). */
	uint8_t viscvg;
	/* Triángulos opacos con AA_EN: el RDP escribe el píxel si cualquiera de
	 * sus 8 muestras de cobertura cae dentro, no solo el centro, y así
	 * tapa las grietas de medio píxel de las uniones en T (piso y paredes
	 * de Dam en GoldenEye). Informativo: ni POLYGON_SMOOTH del RSX ni
	 * ensanchar los triángulos lo reproducen bien (docs/rsx-notes.md). */
	uint8_t aa;
	/* El combiner se aproximó (combine.c): informativo, para el perfilado
	 * (docs/perfilado.md: dibujos aproximados por tarea y los nuevos). */
	uint8_t approx;
	struct gfx_tex tex[2];
	float k[GFX_K_TERMS][3][4];
};

/* Un G_VTX con GFX_JOB_INTERP: la entrada de la lista del juego (dirección
 * física, n, v0 y paso en bytes, para emparejarla con la misma en el cuadro
 * anterior), el viewport con que se cargó y, detrás, las n coordenadas de
 * recorte de sus vértices (x, y, z, w; h.size = 32 + 16 n). Los grupos de
 * una tarea se numeran desde 1 en el orden de sus registros; el vértice i
 * (desde v0) sale con pad = GFX_VGID(número, i). Los que agrega el recorte
 * cercano entre los lugares a y b de un mismo grupo (siempre en los juegos
 * medidos), GFX_VCLIP_ID(número, a, b): la fracción sale de las coordenadas
 * de recorte del grupo, t = (z + w)_a / ((z + w)_a − (z + w)_b). Los demás
 * (rectángulos, recortes entre grupos, los vértices del recorte contra la
 * caja del clip ratio) llevan pad = 0. */
struct gfx_vgroup {
	struct gfx_cmd h;   /* a = dirección, b = n | v0 << 8 | paso << 16, c = hash del contenido (no 0) */
	float vp[4];        /* vscale x, y y vtrans x, y */
	float vz[4];        /* la z de salida es vz[1]·w + vz[0]·z (gfx_vtx_out; con G_ZS_PRIM, vz[0] = 0); vz[2], vz[3]: 0 */
};
#define GFX_VGID(serial, i) ((serial) << 6 | (i))
#define GFX_VCLIP_FLAG 0x80000000u
#define GFX_VCLIP_ID(serial, a, b) (GFX_VCLIP_FLAG | (serial) << 12 | (a) << 6 | (b))
#define GFX_VCLIP_SERIAL(id) (((id) & ~GFX_VCLIP_FLAG) >> 12)
#define GFX_VCLIP_A(id) (((id) >> 6) & 63u)
#define GFX_VCLIP_B(id) ((id) & 63u)
#define GFX_VGID_SERIAL(id) ((id) >> 6)
#define GFX_VGID_INDEX(id)  ((id) & 63u)

#define GFX_ALIGN16(x) (((x) + 15u) & ~15u)

/* Volcado de la salida de una tarea, para depurar (src/rdp lo escribe en
 * la PS3; salsa64-run --raster-dump lo dibuja en el host). Todo en el
 * orden de bytes de quien lo escribió (big-endian en la PS3): cabecera,
 * comandos, vértices y arena de texturas, uno detrás de otro. */
#define GFX_DUMP_MAGIC 0x53344744u  /* "S4GD" */
struct gfx_dump_header {
	uint32_t magic;
	uint32_t cmd_bytes, vtx_bytes, tex_bytes;
};

/* ---- Ejecución -----------------------------------------------------------------
 * gfx_run interpreta la display list de un trabajo. Las funciones gfx_io_*
 * (gfx_io.h) las da cada plataforma. El estado persistente (RDP, TMEM) vive
 * en una gfx_state que pone quien llama. */
struct gfx_state;
void gfx_init(struct gfx_state *g);
void gfx_run(struct gfx_state *g, const struct gfx_job *job, struct gfx_result *res);

#endif
