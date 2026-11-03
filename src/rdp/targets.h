/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — destinos de render del RDP: qué color image de la N64 tiene
 * superficie en el RSX, cuál se recicla cuando no caben más y cuál se
 * presenta cuando el VI muestra una dirección de la RDRAM.
 *
 * Portable (sin rsxkit) para probar la elección en el host
 * (tests/unit/test_targets.c); rdp.c guarda aparte las superficies de
 * rsxkit, una por ranura, y crea o destruye la de una ranura cuando
 * rdp_targets_get dice que cambió de imagen.
 *
 * Cada destino es una color image (dirección, ancho); su alto lógico es
 * una conjetura, porque la N64 no declara el alto: 3/4 del ancho, o más si
 * las tijeras de los dibujos que recibe llegan más abajo (la imagen
 * auxiliar de Conker es CI8 de 64 × 64: con 48 líneas, el RSX escribía
 * fuera de la superficie). Por eso dos framebuffers seguidos se solapan en
 * la cuenta: el que empieza más cerca del origen del VI es el que se
 * muestra (Banjo-Kazooie alterna dos de 292 × 216 y presentaba el otro, con
 * solo sus últimas líneas, cuando el RDP lo acababa de dibujar).
 */
#ifndef SALSA64_RDP_TARGETS_H
#define SALSA64_RDP_TARGETS_H

#include <stdbool.h>
#include <stdint.h>

#include "core/rcp.h"

#define RDP_TARGETS_MAX   8
/* Profundidades vivas (rdp.c; Zelda en pausa alterna tres z-buffers: el
 * principal y uno por par de búferes de Link). */
#define RDP_DEPTHS_MAX    4
/* Superficies del RSX que el RDP puede tener vivas a la vez: una por
 * ranura, el scratch de los efectos de framebuffer (copia del destino
 * ligado cuando un dibujo lo lee), las profundidades y la profundidad de
 * la pantalla agrandada para ligar sin z image destinos más grandes que la
 * pantalla (rdp_zeta_reserve). lib/rsxkit admite 16 destinos y 16
 * profundidades: sobra. */
#define RDP_SURFACES_MAX  (RDP_TARGETS_MAX + 1 + RDP_DEPTHS_MAX + 1)
/* Escala interna máxima (320 × 240 × 5 = 1600 × 1200; 640 × 5 = 3200 cabe
 * en el límite de 4096 de las superficies del RSX). */
#define RDP_SCALE_MAX     5
/* Tope de memoria local del RSX para destinos, scratch, profundidades y la
 * profundidad de la pantalla agrandada (ver docs/memory-budget.md): el peor
 * caso a 5× (RDP_SURFACES_MAX de 1600 × 1200, 102,5 MiB) entra con 9,5 MiB
 * de sobra (con 96 MiB, 5× se bajaba a 4×); rdp.c recicla el destino más
 * viejo si aun así no cupiera. */
#define RDP_VRAM_BUDGET   (112u << 20)
/* Un destino sin dibujar en tantos campos ya no se presenta (ni se protege
 * del reciclado). */
#define RDP_TARGETS_STALE 30

/* Un destino dibujado hace más de tantas tareas ya no se lee del RSX como
 * textura: lo que vale es la RDRAM (docs/framebuffer.md). Dos o tres por
 * los framebuffers alternos (Zelda copia el suyo dos tareas después). */
#define RDP_TARGETS_FRESH 3

/* Lo que el RDP pudo escribir en un destino, en píxeles lógicos
 * [x0, x1) × [y0, y1): la unión de la caja de los vértices de cada dibujo
 * recortada por sus tijeras. Vacía si x1 ≤ x0 o y1 ≤ y0. Las copias a la
 * RDRAM escriben solo eso, porque el RDP nunca escribe fuera de las
 * tijeras ni de lo que dibuja, y lo que hay alrededor puede ser del juego:
 * Super Smash Bros. guarda datos y punteros en los márgenes de su z-buffer
 * (las 10 primeras filas y 10 columnas a cada lado, fuera de las tijeras
 * 10..310 × 10..230); en el KO dibuja una máscara en ese z-buffer como
 * color image y la copia de filas enteras le ponía a 1 el bit bajo de
 * cada media palabra (el alfa del RGBA5551): el juego se colgaba. */
struct rdp_box {
	uint16_t x0, y0, x1, y1;
};

static inline bool rdp_box_empty(const struct rdp_box *b)
{
	return b->x1 <= b->x0 || b->y1 <= b->y0;
}

/* Agrega a `b` un dibujo cuyos vértices caen en [x0, x1] × [y0, y1]
 * (píxeles, ya divididos por w; `all_front`: ninguno con w ≤ 0, si no se
 * toma toda la región de las tijeras) con tijeras `scissor` (x0, y0, x1, y1
 * en 10.2, gfx_draw.scissor), recortado a lw × lh. */
void rdp_box_add(struct rdp_box *b, float x0, float y0, float x1, float y1, bool all_front,
		 const uint16_t scissor[4], uint32_t lw, uint32_t lh);
/* Agrega un rectángulo entero ya recortado. */
void rdp_box_union(struct rdp_box *b, const struct rdp_box *r);
/* ¿Se tocan la caja y el rectángulo [x0, x1) × [y0, y1)? */
static inline bool rdp_box_overlaps(const struct rdp_box *b, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
	return !rdp_box_empty(b) && b->x0 < x1 && x0 < b->x1 && b->y0 < y1 && y0 < b->y1;
}

struct rdp_target {
	bool used;
	bool drawn;           /* dibujado desde la última presentación */
	bool presented;       /* el VI lo mostró alguna vez: es un framebuffer */
	/* Se copió a la RDRAM al terminar una tarea (rdp_targets_readback):
	 * desde entonces la verdad está ahí (el CPU puede haberla cambiado)
	 * hasta que el RDP lo vuelva a dibujar. */
	bool cpu_owned;
	/* Lo dibujado ya está en la RDRAM por el espejo del framebuffer
	 * (rdp_targets_mirrored), o no hay nada dibujado que copiar (recién
	 * creado). Lo borra el próximo dibujo. */
	bool mirrored;
	uint8_t fmt;          /* G_IM_FMT_* de la color image (lo pone quien la crea; 0 si no se sabe) */
	uint32_t addr;        /* color image: dirección física y ancho en píxeles */
	uint32_t width;
	uint32_t bpp;         /* bytes por píxel: 1, 2 o 4 */
	uint32_t lw, lh;      /* tamaño lógico (píxeles de la N64) de la superficie */
	uint32_t last_field;  /* último campo en que se dibujó (o se creó) */
	uint32_t last_task;   /* última tarea que lo dibujó */
	/* Lo dibujado desde el último espejo (rdp_targets_mirrored): lo que
	 * escribe el espejo del framebuffer (rdp_box). */
	struct rdp_box box;
};

/* Un relleno sobre una color image alta (rdp_targets_tall) que se anotó para
 * los destinos que se creen después: ver rdp_targets_tall_note. */
#define RDP_TALL_FILLS 4
#define RDP_TALL_APPLIED 8
/* Tareas que vale un relleno anotado: pasadas, lo que hay en la RDRAM manda. */
#define RDP_TALL_FILL_TASKS 32
struct rdp_tall_fill {
	bool used;
	uint32_t addr, width, bpp, rows;  /* la color image alta */
	float x0, y0, x1, y1;             /* el rectángulo, en píxeles de esa imagen */
	uint32_t color;                   /* 0xRRGGBB */
	uint32_t task;                    /* rdp_targets.task al rellenar */
	uint32_t applied[RDP_TALL_APPLIED];  /* direcciones de los destinos que ya lo recibieron */
	uint32_t napplied;
};

struct rdp_targets {
	struct rdp_target t[RDP_TARGETS_MAX];
	struct rdp_tall_fill tall[RDP_TALL_FILLS];
	uint32_t tall_next;     /* próxima entrada de tall[] a reemplazar */
	uint32_t field;         /* campo en curso: el último presentado */
	uint32_t task;          /* tarea gráfica en curso (rdp_targets_task) */
	int shown;              /* ranura presentada en el último campo, o -1 */
	uint32_t shown_origin;  /* origen del VI en ese campo */
	uint32_t shown_field;
	/* Entrelazado de alta resolución: la posición del renglón 0 del destino
	 * en la pantalla (vpos - y0) y la fila desde la que se presentó. */
	bool il_valid;
	int il_slot;
	int32_t il_v0;
	uint32_t il_y0;
	bool il_weave;
	uint32_t il_width, il_height, il_bpp;
	uint32_t vi_width;      /* ancho del VI en el último campo con imagen */
	uint32_t created;       /* superficies creadas (estadística) */
	uint32_t readbacks;     /* copias a la RDRAM (estadística) */
	uint32_t tex_rsx, tex_rdram, tex_self;  /* texturas con origen en un destino: del RSX, de la RDRAM, del ligado */
};

/* Orígenes recientes del VI (campos con imagen), para saber desde el hilo
 * que no tiene la tabla si una color image es un framebuffer o una imagen
 * auxiliar: lo lleva quien corre el VI (rdp_vi_ring_push por campo). Un
 * origen que ya está entre los recientes no se vuelve a agregar; el ancho
 * del VI de ahora es cur_width (el del último campo), no el del último
 * origen agregado: GoldenEye pasa de las pantallas de la misión (440 de
 * ancho) a la presa (320) con framebuffers que ya estaban en el anillo, y
 * con el ancho del último agregado (440) cada cuadro de la presa parecía
 * una imagen auxiliar y se copiaba a la RDRAM esperando. */
#define RDP_VI_RING 8
struct rdp_vi_ring {
	uint32_t origin[RDP_VI_RING], width[RDP_VI_RING];
	/* Geometría de cada origen. Además de identificar framebuffers, evita
	 * que una color image nazca más baja que la ventana VI que la mostrará:
	 * el video entrelazado de Racer avanza dos filas del destino por cada
	 * fila visible. */
	uint32_t height[RDP_VI_RING], stride[RDP_VI_RING], bpp[RDP_VI_RING];
	uint32_t n;
	uint32_t cur_width;
};

/* Lo que se presenta en un campo: la región lógica del destino que
 * muestra el VI. */
struct rdp_present {
	int slot;         /* -1: nada del RSX (va la copia de la RDRAM) */
	uint32_t y0;      /* primera línea lógica (el origen del VI dentro del destino) */
	uint32_t w, h;    /* píxeles lógicos */
	uint32_t age;     /* campos desde que se dibujó lo presentado (0: en este campo) */
};

void rdp_targets_init(struct rdp_targets *s);

/* Bytes de una superficie de w × h píxeles de 32 bits (pitch alineado a
 * 128, como lib/rsxkit). */
uint32_t rdp_surface_bytes(uint32_t w, uint32_t h);

/* Peor caso a la escala-objetivo `scale`: cada superficie mide a lo más
 * 320·scale × 240·scale (el alto lógico de un destino es al menos 3/4 del
 * ancho y rdp_target_scale no pasa de 240·scale líneas; a 1× una de
 * 640 × 480 queda nativa, 1,2 MB), también el scratch (copia de un
 * destino), cada profundidad (la del destino con que se usa, o la mayor de
 * dos si la comparten) y la profundidad de la pantalla agrandada
 * (rdp_zeta_reserve; se cuenta entera aunque la pantalla ya tuviera una):
 * RDP_SURFACES_MAX superficies (8 + 1 + 4 + 1). A 5× son 107,5 MB
 * (102,5 MiB); a 3×, 38,7 MB; a 4×, 68,8 MB. */
uint32_t rdp_vram_worst(uint32_t scale);

/* Lo más que mide una superficie a la escala-objetivo `scale` (destino,
 * scratch o profundidad): max(320·scale, 640) × max(240·scale, 480). */
void rdp_surface_max(uint32_t scale, uint32_t *w, uint32_t *h);

/* Profundidad de la pantalla para los destinos que se ligan sin z image
 * propia (dibujos sin z, cebado, scratch): rsxkit_bind no liga un destino
 * más grande que ella (una zeta más chica que el color puede fallar o
 * recortar en el RSX). Si a esta escala algún destino puede ser más grande
 * que la pantalla de screen_w × screen_h (4× y 5× en 720p, 5× en 1080p),
 * devuelve true con el tamaño a reservar (rsxkit_depth_reserve: el mayor
 * destino posible, rdp_surface_max); si no, false y 0 × 0 (la de la
 * pantalla alcanza). */
bool rdp_zeta_reserve(uint32_t scale, uint32_t screen_w, uint32_t screen_h, uint32_t *w, uint32_t *h);

/* Región lógica (x, y, w, h) de un destino a escala `scale`, en píxeles
 * de su superficie: lo que leen las copias RSX → RDRAM (que la reducen a
 * w × h), el espejo del framebuffer y la presentación. Siempre con la
 * escala del destino que se lee, no la objetivo. */
struct rdp_rect {
	uint32_t x, y, w, h;
};
void rdp_surface_rect(uint32_t scale, uint32_t x, uint32_t y, uint32_t w, uint32_t h, struct rdp_rect *out);

/* Render a textura: una textura de la HLE de tex_w × tex_h texels que
 * empieza en el texel (x0, y0) de un destino de `lw` puntos lógicos de
 * ancho, cuya superficie está a escala `scale`, con u = s·xf0 + xf2
 * normalizada a tex_w. Se liga como textura desde la fila row0 de la
 * superficie, `rows` filas (filas enteras: el offset de una textura va
 * alineado a 128), y u pasa a normalizarse a la fila entera del destino
 * (xf0, xf2 de salida). u y v quedan normalizadas, así que el destino en
 * que se dibuja puede tener otra escala sin cambiar nada más. */
struct rdp_tex_map {
	uint32_t row0, rows;
	float xf0, xf2;
};
void rdp_target_tex_map(uint32_t scale, uint32_t lw, uint32_t x0, uint32_t y0, uint32_t tex_w, uint32_t tex_h,
			float xf0, float xf2, struct rdp_tex_map *out);

/* Una textura que la HLE no decodificó porque venía de un destino
 * (GFX_TEX_TARGET, rsp/gfx/gfx.h) y que al final no se puede leer de él
 * (para la tabla el destino es viejo, es del CPU o no tiene superficie):
 * w × h texels RGBA16 de la RDRAM desde `addr`, con `pitch` texels por fila,
 * a RGBA8888 (bytes R, G, B, A) con `out_pitch` bytes por fila, como los
 * decodifica la HLE. Lo que cae fuera de la RDRAM queda en cero. */
void rdp_decode_rgba16(const uint8_t *rdram, uint32_t rdram_size, uint32_t addr, uint32_t pitch, uint32_t w,
		       uint32_t h, uint8_t *out, uint32_t out_pitch);
/* Lo mismo para una I8 (de una color image de 8 bits): RGBA = (I, I, I, I). */
void rdp_decode_i8(const uint8_t *rdram, uint32_t rdram_size, uint32_t addr, uint32_t pitch, uint32_t w, uint32_t h,
		   uint8_t *out, uint32_t out_pitch);

/* Un píxel del z-buffer de la N64 (14 bits de z comprimida, exponente de
 * 3 y mantisa de 11, y 2 de dz) a la profundidad de 24 bits del RSX: la z
 * de 18 bits descomprimida, de 0 a 0x3FFFF, a escala de 0 a 0xFFFFFF (la
 * misma de z/w en [0, 1] de los vértices de la HLE; 0xFFFC da 0xFFFFFF). */
uint32_t rdp_z16_to_z24(uint16_t v);

/* ¿La región de src_w × src_h píxeles de superficie se presenta a un
 * múltiplo entero (sin el estiramiento fraccionario que repetía filas con
 * el recorte del overscan) en vez de ajustarse a la pantalla? Sí si cabe y
 * el múltiplo llena al menos 3/4 del alto de la pantalla (960 × 720 o
 * 912 × 684 en 720p, 1280 × 960 en 1080p, 320 × 240 a 1× por 3); no si
 * quedaría chica (640 × 480 en 720p, 960 × 720 en 1080p) o no cabe
 * (supermuestreo: se reduce con filtro lineal). */
bool rdp_present_integer(uint32_t src_w, uint32_t src_h, uint32_t screen_w, uint32_t screen_h);

/* Escala entera de un destino de lw × lh puntos lógicos para la escala-
 * objetivo `global` (1..RDP_SCALE_MAX): la mayor que no pasa de 240 × global
 * líneas, al menos 1, con tope RDP_SCALE_MAX y de 4096 por lado. */
uint32_t rdp_target_scale(uint32_t global, uint32_t lw, uint32_t lh);

/* Escala-objetivo efectiva. requested = 0: la mayor entera que cabe en una pantalla
 * de `screen_h` líneas (720 → 3, 1080 → 4); 1 .. RDP_SCALE_MAX: la pedida.
 * En ambos casos se baja hasta que rdp_vram_worst(escala) ≤
 * RDP_VRAM_BUDGET. *reduced: se bajó por memoria (no por el tope de 5 ni
 * por ser automática). */
uint32_t rdp_scale_pick(uint32_t requested, uint32_t screen_h, bool *reduced);
/* Igual con otro presupuesto (pruebas). */
uint32_t rdp_scale_pick_budget(uint32_t requested, uint32_t screen_h, uint32_t budget, bool *reduced);

/* Ranura de la color image (addr, width, siz de G_SETCIMG), existente o
 * reciclada, con al menos `min_lh` líneas (0: las de siempre, 3/4 del
 * ancho; quien llama pasa hasta dónde llegan las tijeras de los dibujos
 * que vienen). *fresh: la ranura cambió de imagen, es nueva o creció: hay
 * que crear su superficie de lw × lh (y destruir la anterior si la había).
 * Al reciclar, nunca se toma la ranura presentada en el último campo;
 * entre las demás se prefiere una vieja (RDP_TARGETS_STALE), después una
 * de un ancho distinto al del VI (imagen auxiliar, no un framebuffer) y
 * al final la dibujada hace más campos. La misma dirección y ancho con
 * otro tamaño de texel es la misma ranura si se dibujó hace
 * RDP_TARGETS_FRESH tareas o menos (Zelda OoT deja la cobertura de 8 bits
 * sobre su framebuffer vivo) y, si no, otra imagen en su lugar (Majora's
 * Mask: cobertura de 8 bits en la pausa, copia de 16 bits del cuadro en
 * las campanadas). */
int rdp_targets_get(struct rdp_targets *s, uint32_t addr, uint32_t width, uint32_t siz, uint32_t min_lh,
		    bool *fresh);
/* Por qué una color image no puede tener destino, o RDP_CIMG_OK. Las
 * imposibles salen de comandos que la HLE interpretó mal (Zelda al abrir la
 * pausa manda anchos de 1495 y 3770): ancho 0 o de más de 640, alto pedido
 * de más de 480 y, con `rdram_size` distinto de 0, una que empieza fuera de
 * la RDRAM (el RDP no tiene dónde escribirla; Indiana Jones pidió una vez
 * 900000/455 con 8 MB y el RSX se colgó en esa tarea: docs/rsx-notes.md).
 * Quien llama no la convierte en destino ni recicla una ranura por ella. */
enum rdp_cimg_bad {
	RDP_CIMG_OK = 0,
	RDP_CIMG_WIDTH,   /* ancho 0 o > 640 */
	RDP_CIMG_HEIGHT,  /* alto pedido > 480 */
	RDP_CIMG_RDRAM,   /* empieza fuera de la RDRAM */
};
enum rdp_cimg_bad rdp_cimg_impossible(uint32_t addr, uint32_t width, uint32_t min_lh, uint32_t rdram_size);
/* La ranura en uso que rdp_targets_get con esos mismos argumentos
 * reasignaría a otra imagen (la recicla o la hace crecer), o -1 si la
 * imagen ya existe o la ranura estaba libre. Sirve para copiar a la RDRAM
 * lo dibujado en ella ANTES de que cambie de dirección y de alto. */
int rdp_targets_recycled(const struct rdp_targets *s, uint32_t addr, uint32_t width, uint32_t siz, uint32_t min_lh);

/* Se dibujó en la ranura durante el campo en curso. */
void rdp_targets_drawn(struct rdp_targets *s, int slot);

/* Ranura del destino que contiene la dirección `addr` de una imagen de
 * `pitch` texels por fila y `bpp` bytes por texel (el origen de una textura:
 * gfx_tex.src_*), con el texel (x0, y0) del destino en que cae; -1 si
 * ninguno. Entre destinos que se solapan, el que empieza más cerca. No
 * mira la antigüedad: quien llama decide con last_task. */
int rdp_targets_locate(const struct rdp_targets *s, uint32_t addr, uint32_t pitch, uint32_t bpp, uint32_t *x0,
		       uint32_t *y0);

/* Empieza una tarea gráfica (antes de traducir sus comandos). */
void rdp_targets_task(struct rdp_targets *s);

/* De dónde leer una textura cuyo origen es (addr, pitch, bpp): de la
 * superficie del RSX del destino `*slot` (render a textura: dibujado hace
 * RDP_TARGETS_FRESH tareas o menos y no es del CPU), de una copia del
 * destino ligado `cur` (SELF: el juego lee lo que está dibujando, como la
 * pausa de Zelda sobre su framebuffer; quien llama copia la superficie a un
 * scratch antes), de la RDRAM (la textura decodificada por la HLE: el
 * destino es viejo o se copió a la RDRAM) o de ninguno (no cae en un
 * destino). Cuenta en tex_rsx / tex_rdram / tex_self. */
enum rdp_texsrc { RDP_TEXSRC_NONE = 0, RDP_TEXSRC_RSX, RDP_TEXSRC_RDRAM, RDP_TEXSRC_SELF };
enum rdp_texsrc rdp_targets_texture(struct rdp_targets *s, uint32_t addr, uint32_t pitch, uint32_t bpp, int cur,
				    int *slot, uint32_t *x0, uint32_t *y0);
/* Like rdp_targets_texture, after CopyColorFromRDRAM synchronized changed
 * CPU spans into a live RSX-owned surface. Profiles may opt to sample a stale
 * surface only after reconciliation; CPU-owned surfaces always fall back. */
enum rdp_texsrc rdp_targets_texture_synced(struct rdp_targets *s, uint32_t addr, uint32_t pitch, uint32_t bpp,
					   int cur, bool allow_stale, int *slot, uint32_t *x0, uint32_t *y0);

/* Al terminar una tarea que dibujó la ranura: si hay que copiarla a la
 * RDRAM (rdp_readback_wanted con los orígenes del VI de la tabla) devuelve
 * true y, con `cpu_owns`, la marca del CPU: desde entonces las texturas que
 * la lean salen de la RDRAM. Con `force`, se copia sin preguntar (lo
 * decidió otro hilo con rdp_readback_wanted). Sin `cpu_owns` (una copia
 * sin esperar: la misma imagen se copia tarea tras tarea, como el
 * acumulador del desenfoque de movimiento de Majora's Mask), el RDP la
 * sigue leyendo del RSX mientras esté viva: la copia llega a la RDRAM una
 * o dos tareas después, a la resolución de la N64 y en 16 bits, y la
 * realimentación del efecto acumulaba ese retraso y ese redondeo. */
bool rdp_targets_readback(struct rdp_targets *s, int slot, bool force, bool cpu_owns);
/* Clear CPU ownership after CopyColorFromRDRAM has synchronized the RSX
 * surface with the changed RDRAM spans, so recent texture reads use that
 * reconciled surface rather than decoding the old backing again. */
void rdp_targets_rdram_uploaded(struct rdp_targets *s, int slot);

/* Espejo del framebuffer (rdp.c y el rasterizador del host): lo que el RDP
 * dibuja en un framebuffer se copia a la RDRAM para que el CPU lea el
 * cuadro y no basura (la cámara del túnel de Mario Kart), una sola vez por
 * dibujo y cuanto antes: al terminar la tarea que lo dibujó si ya se sabe
 * que es un framebuffer (rdp_targets_mirror_at_task_end: el VI lo mostró
 * alguna vez, de 16 bits y del ancho del VI) y, si no, al presentarlo
 * (rdp_targets_mirror_wanted). Copiar al presentar, o en cada campo, un
 * cuadro que el RDP dibujó antes pisa lo que el CPU o un DMA escribieron
 * entre medio: Super Mario 64, al pasar del logo a la cara de Mario,
 * dibuja un framebuffer, carga por DMA datos encima de él y de otro y el
 * VI los muestra uno o dos campos más; esas copias pisaban los datos y el
 * juego se colgaba (dejaba de mandar tareas gráficas). */
bool rdp_targets_mirror_wanted(const struct rdp_targets *s, int slot);
bool rdp_targets_mirror_at_task_end(const struct rdp_targets *s, int slot);
/* La copia del espejo de la ranura se hizo (o quedó encolada): vacía su
 * caja. Quien dibuja agrega a `box` lo de cada dibujo (rdp_box_union). */
void rdp_targets_mirrored(struct rdp_targets *s, int slot);

/* Escrituras a la RDRAM que no pasan por el CPU (DMA del PI, del SP, del SI,
 * salida de la HLE de audio), con el número de la tarea gráfica en vuelo
 * al ocurrir (o la última terminada, si no había ninguna). Una copia a la
 * RDRAM de lo que dibujó la tarea T no debe pisar lo que un DMA escribió
 * desde que T empezó: en la consola el RDP termina antes de que llegue el
 * DMA, aquí la copia llega después (en la PS3, varios ms). Star Fox 64
 * limpia la pantalla con una tarea y en el mismo campo carga por DMA
 * datos comprimidos encima de ese framebuffer; el espejo los pisaba y el
 * juego se quedaba esperando. Los tramos seguidos de la misma tarea se
 * funden (los DMA van en trozos de 256 bytes). Lo escribe el hilo de
 * emulación y lo lee el de presentación: `n` se publica al final. */
/* Color image más alta que una pantalla (Star Fox 64: 320 × 720 en
 * 0x38f800, sus tres framebuffers seguidos, para limpiarlos con un solo
 * FILLRECT al arrancar y al cambiar de escena; también borra el borde de
 * 8 px que el juego nunca vuelve a tocar). No se crea un destino para ella:
 * los rellenos se reparten entre los destinos ya existentes que empiezan
 * dentro (alineados a sus líneas; el último puede asomar por abajo).
 * Devuelve cuántos y, por cada uno, la ranura y la fila de la imagen alta
 * en que empieza. */
int rdp_targets_inside(const struct rdp_targets *s, uint32_t addr, uint32_t width, uint32_t bpp, uint32_t rows,
		       int slots[RDP_TARGETS_MAX], uint32_t first_row[RDP_TARGETS_MAX]);
/* ¿Una color image de `width` × `lh` es «alta»: más de pantalla y media
 * (lh > 1,5 × 3/4 del ancho) o más de 480? Donkey Kong 64 rellena dos
 * framebuffers de 320 × 240 seguidos con un FILLRECT de 459 filas: con un
 * destino de 320 × 459 ese búfer salía a otra escala que el siguiente. */
static inline bool rdp_targets_tall(uint32_t width, uint32_t lh)
{
	return lh > 480 || lh > width * 9 / 8;
}

#define RDP_DMA_RING 64
struct rdp_dma_ring {
	struct { uint32_t lo, hi, task; } e[RDP_DMA_RING];
	uint32_t n;  /* entradas escritas en total (las válidas: las últimas RDP_DMA_RING) */
};
void rdp_dma_note(struct rdp_dma_ring *r, uint32_t pa, uint32_t len, uint32_t task);
/* ¿Algún DMA desde la tarea `task` (inclusive) tocó [lo, hi)? */
bool rdp_dma_since(const struct rdp_dma_ring *r, uint32_t lo, uint32_t hi, uint32_t task);

/* Guarda de las copias tardías contra lo que escribió el CPU.
 *
 * En la N64 el RDP escribe la color image mientras corre la tarea; lo que el
 * CPU escribe después en esa memoria queda encima. En la PS3 la copia del
 * RSX a la RDRAM (espejos del framebuffer, copias sin esperar, VISCVG) la
 * hace el hilo de presentación uno o más campos después: si entretanto el
 * juego reusó esa memoria (Indiana Jones arma sus listas de pantalla donde
 * estuvo el framebuffer de 640 × 480 de la pantalla del Expansion Pak;
 * Killer Instinct Gold, objetos del audio donde estuvo un cuadro), la copia
 * le pisaba sus datos con el cuadro, que en negro es 0x0001 en RGBA5551: la
 * lista salía llena de 0x00010001 y el CPU saltaba a 0x00010001.
 *
 * Huellas por línea de 128 bytes. `base` la escribe el hilo de emulación
 * al terminar una tarea; `wrote`, el de presentación después de una copia
 * tardía. `profile` conserva un baseline independiente para CopyColorFromRDRAM:
 * las huellas de tareas cambian continuamente y no sirven para recordar una
 * escritura CPU entre el dibujo y el uso siguiente de la superficie. */
#define RDP_GUARD_SHIFT 7
#define RDP_GUARD_LINE  (1u << RDP_GUARD_SHIFT)
#define RDP_GUARD_LINES ((8u << 20) >> RDP_GUARD_SHIFT)
struct rdp_guard {
	uint32_t base[RDP_GUARD_LINES];
	uint32_t wrote[RDP_GUARD_LINES];
	uint32_t profile[RDP_GUARD_LINES];
};
/* Huella de 128 bytes alineados (nunca 0). */
uint32_t rdp_guard_hash(const uint8_t *line);
void rdp_guard_clear(struct rdp_guard *g);
/* Hilo de emulación: huella de las líneas de [lo, hi) (recortado a `size`). */
void rdp_guard_mark(struct rdp_guard *g, const uint8_t *rdram, uint32_t size, uint32_t lo, uint32_t hi);
/* Baseline estable del perfil CopyColorFromRDRAM, independiente de las guardas tardías. */
void rdp_guard_profile_mark(struct rdp_guard *g, const uint8_t *rdram, uint32_t size, uint32_t lo, uint32_t hi);
/* ¿Una copia RSX->RDRAM puede escribir esta línea sin pisar datos del CPU? */
bool rdp_guard_profile_ok(const struct rdp_guard *g, const uint8_t *rdram, uint32_t line);

/* CPU-written overlay lifetime for CopyColorFromRDRAM. */
enum rdp_profile_pending_mode {
	RDP_PROFILE_PENDING_NONE = 0,
	RDP_PROFILE_PENDING_BLEND = 1,
	RDP_PROFILE_PENDING_REPLACE = 2,
	RDP_PROFILE_PENDING_FOLLOWUP = 3,
	/* An applied overlay superseded by a newer RDP task. Retire without
	 * uploading; REPLACE remains reserved for a CPU-cleared transparent pixel. */
	RDP_PROFILE_PENDING_RETIRE = 4,
};
bool rdp_profile_pending_new_batch(uint8_t *pending, uint32_t count, uint32_t *last_batch,
				   bool *last_batch_valid, uint32_t batch);
/* A retired overlay is safe to drop only after its own target was drawn in
 * the task that superseded the batch. */
bool rdp_profile_pending_retire_ready(uint32_t target_task, uint32_t batch, bool batch_valid);
uint8_t rdp_profile_pending_note(uint8_t pending, bool was_covered, bool is_covered);
uint8_t rdp_profile_pending_refresh(uint8_t pending, bool is_covered);
uint8_t rdp_profile_pending_complete(uint8_t mode);
/* ¿Se puede escribir la línea `line` (dirección >> RDP_GUARD_SHIFT)? */
static inline bool rdp_guard_ok(const struct rdp_guard *g, const uint8_t *rdram, uint32_t line)
{
	uint32_t b = g->base[line], w = g->wrote[line];
	if (!b && !w)
		return true;
	uint32_t h = rdp_guard_hash(rdram + (line << RDP_GUARD_SHIFT));
	return h == b || h == w;
}
/* Hilo de presentación: la copia escribió en la línea `line`. */
static inline void rdp_guard_wrote(struct rdp_guard *g, const uint8_t *rdram, uint32_t line)
{
	g->wrote[line] = rdp_guard_hash(rdram + (line << RDP_GUARD_SHIFT));
}
/* True when any initialized profile-baseline line overlapping this 16-bit
 * target row has changed since it was last uploaded/read back. */
bool rdp_target_row_cpu_changed(const struct rdp_target *t, uint32_t row, const struct rdp_guard *g,
				const uint8_t *rdram, uint32_t rdram_size);

/* Lo que tomó otra color image después (complemento de la guarda).
 *
 * La huella `base` de una línea la renueva cualquier tarea cuyos dibujos
 * puedan tocarla, aunque sea con otra color image: entonces ya incluye lo que
 * el CPU escribió antes de esa tarea y la guarda no lo ve. FIFA 99 cambia de
 * los framebuffers de 512 × 384 del menú a los de 480 × 360 del partido en la
 * misma memoria: suelta los viejos, pide dos bloques al heap (la cabecera del
 * segundo, en 0x1abc70, va justo después del primer framebuffer nuevo, que
 * acaba en 0x1abc40) y la primera tarea del partido renueva la huella de la
 * línea 0x1abc00; el espejo del último cuadro del menú, que llega después,
 * pasaba la guarda y escribía negro (0x00010001) sobre la cabecera. Al
 * acabar el partido el juego suelta ese bloque, lee el anterior en
 * 0x00010001 y falla (AdEL en 0x10003): pantalla negra.
 *
 * Por eso el hilo de emulación anota también, por tarea, qué filas de qué
 * color image (dirección y bytes por fila) pudieron tocar sus dibujos, y una
 * copia tardía de la color image X dibujada por última vez en la tarea T no
 * escribe las líneas que una color image distinta de X tocó en una tarea
 * posterior a T: en la consola, eso que dibujó después (o lo que el CPU
 * escribió antes de que lo dibujara) queda encima. La misma color image
 * vuelta a dibujar no cuenta (eso ya lo cubre la guarda). */
#define RDP_CLAIM_RING 64
struct rdp_claim {
	uint32_t lo, hi;  /* bytes [lo, hi) de la RDRAM */
};
struct rdp_claim_ring {
	struct { uint32_t lo, hi, addr, line, task; } e[RDP_CLAIM_RING];
	uint32_t n;  /* entradas escritas en total (las válidas: las últimas RDP_CLAIM_RING) */
};
/* Hilo de emulación: la tarea `task` dibujó en las filas [lo, hi) de la
 * color image (addr, line bytes por fila). Se funde con otra entrada de la
 * misma tarea y la misma color image. */
void rdp_claim_note(struct rdp_claim_ring *r, uint32_t addr, uint32_t line, uint32_t lo, uint32_t hi, uint32_t task);
/* Tramos de [lo, hi) que tocó una color image distinta de (addr, line) en
 * una tarea posterior a `task`, hasta `max` (si son más, el último se
 * agranda hasta cubrirlos). −1 si el anillo ya perdió entradas posteriores
 * a `task` (no se sabe). */
int rdp_claims_after(const struct rdp_claim_ring *r, uint32_t addr, uint32_t line, uint32_t lo, uint32_t hi,
		     uint32_t task, struct rdp_claim *out, int max);
/* ¿La línea `line` (dirección >> RDP_GUARD_SHIFT) toca alguno de los tramos? */
static inline bool rdp_claimed(const struct rdp_claim *c, int n, uint32_t line)
{
	for (int i = 0; i < n; i++)
		if (line >= c[i].lo >> RDP_GUARD_SHIFT && line <= (c[i].hi - 1) >> RDP_GUARD_SHIFT)
			return true;
	return false;
}

/* Un relleno sobre una color image alta que se reparte entre los destinos
 * existentes (rdp_targets_inside) no llega a los que se creen después:
 * Star Fox 64 limpia sus tres framebuffers con un solo FILLRECT de 320 × 720
 * en la primera tarea gráfica (campo 3), cuando aún no hay ninguno; los
 * destinos nacen en las tres siguientes y se cebaban con la RDRAM, donde el
 * RDP de la HLE nunca escribió el relleno y siguen los datos que el juego
 * cargó antes por DMA (ruido de colores en el borde de 8 px y arriba, hasta
 * el relleno del cambio de escena, campo 158: el logo de «Nintendo
 * presents»). En la consola ese relleno sí está en la RDRAM. Se anota el
 * relleno (`done`, las ranuras que ya lo recibieron) y rdp_targets_tall_pending
 * lo da, una sola vez, a cada destino nuevo que cae dentro de la imagen alta
 * (misma regla que rdp_targets_inside) mientras no hayan pasado
 * RDP_TALL_FILL_TASKS tareas ni un DMA haya tocado la memoria del destino
 * desde el relleno (esos datos son más nuevos: en la consola el DMA pasa
 * por encima del relleno). */
struct rdp_tall_rect {
	float x0, y0, x1, y1;  /* en píxeles lógicos del destino */
	uint32_t color;        /* 0xRRGGBB */
};
void rdp_targets_tall_note(struct rdp_targets *s, uint32_t addr, uint32_t width, uint32_t bpp, uint32_t rows,
			   float x0, float y0, float x1, float y1, uint32_t color, const int *done, int ndone);
/* Los rellenos pendientes del destino recién creado `slot`, del más viejo al
 * más nuevo; los marca como entregados. */
int rdp_targets_tall_pending(struct rdp_targets *s, int slot, const struct rdp_dma_ring *dma,
			     struct rdp_tall_rect out[RDP_TALL_FILLS]);

void rdp_vi_ring_push(struct rdp_vi_ring *r, uint32_t origin, uint32_t width);
void rdp_vi_ring_frame(struct rdp_vi_ring *r, const struct salsa64_vi_frame *f);
/* ¿Hay que copiar a la RDRAM la color image (addr, width, siz) cuando el
 * RDP termine de dibujarla? Sí para una imagen de 16 bits del ancho del VI
 * que no es ninguno de los framebuffers recientes: un cuadro guardado
 * aparte, que el CPU suele leer (el fondo de la pausa de Zelda). Las
 * imágenes chicas (iconos, pantallas) se quedan en el RSX como textura. */
bool rdp_readback_wanted(const struct rdp_vi_ring *r, uint32_t addr, uint32_t width, uint32_t siz);
/* ¿La color image (addr, width) está dentro de alguno de los framebuffers
 * recientes del VI? Las que no lo están son imágenes auxiliares. */
bool rdp_vi_ring_covers(const struct rdp_vi_ring *r, uint32_t addr, uint32_t width);
/* Only visible/recent VI buffers consume CPU FBWrite events; auxiliary targets
 * retain the byte-difference path and cannot stay permanently dirty. */
bool rdp_target_profile_vi_visible(const struct rdp_target *t, const struct rdp_vi_ring *r);
/* Alto mínimo de la color image (addr, width, bpp) que cubre una ventana
 * VI reciente. 0 si no hay una compatible. */
uint32_t rdp_vi_ring_min_height(const struct rdp_vi_ring *r, uint32_t addr, uint32_t width, uint32_t bpp);

/* Qué presentar en el campo `field` para la imagen `f` del VI: entre los
 * destinos recientes que cubren el origen, el del ancho del VI; entre
 * iguales, el que empieza más cerca del origen y, si empatan, el dibujado
 * hace menos. Si no hay ninguno pero el VI sigue en el mismo origen que
 * el campo anterior, se sostiene el destino presentado entonces (el juego
 * deja de dibujar mientras carga). */
void rdp_targets_present(struct rdp_targets *s, const struct salsa64_vi_frame *f, uint32_t field,
			 struct rdp_present *out);

#endif
