/* SPDX-License-Identifier: MIT */
/*
 * rsxkit — posprocesado: pasadas de pantalla completa con fragment programs
 * precompilados (cgcomp) sobre superficies intermedias, encadenadas hasta
 * la pantalla del cuadro. Es solo la maquinaria: los filtros concretos (un
 * CRT, un escalado) los pone quien la usa, con sus propios programas.
 *
 * Una cadena típica: la imagen del cuadro (un destino a resolución interna
 * alta, un framebuffer) se reduce a la resolución de la consola
 * (rsxkit_post_reduce), un par de pasadas calculan tablas en superficies
 * intermedias y la última dibuja en la pantalla:
 *
 *     rsxkit_post_reduce(&src_tex, &region, 5, small);
 *     rsxkit_post_pass p = { .program = tabla, .out = tabla_sf, .rect = { 0, 0, w, h } };
 *     p.in[0] = rsxkit_post_surface_input(small, RSXKIT_FILTER_NEAREST, RSXKIT_POST_BORDER);
 *     p.inputs = 1;
 *     rsxkit_post_run(&p);
 *     ...
 *     p = (rsxkit_post_pass){ .program = final, .out = NULL, .rect = rect_en_pantalla };
 *     ...
 *     rsxkit_post_run(&p);
 *
 * Todas las llamadas desde el hilo que dibuja, entre rsxkit_frame_begin y
 * rsxkit_frame_end (salvo crear y destruir). Ver lib/rsxkit/README.md.
 */
#ifndef RSXKIT_POST_H
#define RSXKIT_POST_H

#include "rsxkit/rsxkit.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Superficies intermedias ------------------------------------------------- */

typedef enum rsxkit_post_format {
	RSXKIT_POST_RGBA8,    /* 4 × 8 bits sin signo, [0, 1] (X8R8G8B8); se filtra */
	RSXKIT_POST_RGBA16F,  /* 4 × fp16 (W16Z16Y16X16 en coma flotante); se filtra */
	RSXKIT_POST_FORMATS
} rsxkit_post_format;

typedef struct rsxkit_surface rsxkit_surface;

/* Superficie lineal en la memoria local del RSX que se dibuja con una
 * pasada y se lee como textura en las siguientes. Hasta RSXKIT_SURFACES_MAX
 * vivas. NULL si no hay memoria o el tamaño no vale (1 .. 4096). Nace sin
 * contenido definido. */
#define RSXKIT_SURFACES_MAX 24
rsxkit_surface *rsxkit_surface_create(uint32_t width, uint32_t height, rsxkit_post_format format);
/* Una superficie sobre memoria principal que ya mapeó quien llama
 * (rsxkit_map_main: `mem` alineada a 128 bytes, `offset` el que dio el
 * mapeo, `pitch` múltiplo de 64): el RSX la dibuja y la CPU o un SPE la
 * leen sin la lentitud de leer la memoria local desde el Cell, o al revés
 * (la escriben ellos y el RSX la lee como textura, algo más lento que de la
 * memoria local). rsxkit no la libera; cuenta en RSXKIT_SURFACES_MAX, no
 * en rsxkit_surface_bytes. */
rsxkit_surface *rsxkit_surface_wrap_main(uint32_t width, uint32_t height, rsxkit_post_format format, void *mem,
					 uint32_t offset, uint32_t pitch);
/* Espera a que el RSX deje de usarla y la libera (si es de
 * rsxkit_surface_wrap_main, solo la suelta). NULL no hace nada. */
void rsxkit_surface_destroy(rsxkit_surface *s);
void rsxkit_surface_size(const rsxkit_surface *s, uint32_t *width, uint32_t *height);
/* Memoria local que ocupan las superficies vivas, en bytes. */
uint32_t rsxkit_surface_bytes(void);
/* Los píxeles de la superficie (en el orden de bytes de la PS3: A, R, G, B
 * en RSXKIT_POST_RGBA8; cuatro fp16 R, G, B, A en RSXKIT_POST_RGBA16F),
 * para pruebas y diagnóstico: después de rsxkit_finish; leer la memoria del
 * RSX desde el PPE es lento. */
const void *rsxkit_surface_data(const rsxkit_surface *s, uint32_t *pitch);

/* ---- Programas ------------------------------------------------------------------ */

typedef struct rsxkit_program rsxkit_program;

/* Un fragment program compilado por cgcomp (el .fpo entero, embebido) con
 * su propia copia en la memoria del RSX: dos rsxkit_program del mismo .fpo
 * pueden llevar constantes distintas. Recibe las entradas de rsxkit_post_run
 * (TEXCOORD0 a TEXCOORD7) y lee sus texturas de las unidades TEXUNIT0 a
 * TEXUNIT7. NULL si no hay memoria o el .fpo no es válido. */
#define RSXKIT_PROGRAMS_MAX 32
rsxkit_program *rsxkit_program_create(const void *fpo);
/* Espera a que el RSX deje de usarlo y lo libera. */
void rsxkit_program_destroy(rsxkit_program *p);
/* Fija la constante `name` (un `uniform float4` del programa; con
 * count > 1, los elementos name[0] .. name[count − 1] de un arreglo) a
 * count float4 de `v`. El RSX no tiene registros de constantes en los
 * fragment programs: el valor va dentro del microcódigo, en cada
 * instrucción que lo lee; aquí se parchea la copia del programa. Si cambia
 * algo y el RSX todavía usa el programa, espera a que lo suelte: para
 * valores que cambian poco (al cambiar de resolución o de ajuste); lo que
 * cambia en cada dibujo va en rsxkit_post_pass.k. Devuelve 0, o
 * RSXKIT_EARG si el programa no tiene esa constante (el compilador quita
 * las que no se usan). */
int rsxkit_program_set(rsxkit_program *p, const char *name, const float *v, uint32_t count);
/* Instrucciones del programa (el costo por píxel lo manda este número). */
uint32_t rsxkit_program_instructions(const rsxkit_program *p);

/* ---- Pasadas --------------------------------------------------------------------- */

/* Modo de las coordenadas fuera de la textura. */
enum {
	RSXKIT_POST_CLAMP,   /* repite el borde */
	RSXKIT_POST_BORDER,  /* 0 (negro, alfa 0) */
	RSXKIT_POST_REPEAT,  /* solo con coordenadas normalizadas */
};

/* Una textura de entrada de una pasada, en la unidad de su índice. */
typedef struct rsxkit_post_input {
	const rsxkit_surface *surface;  /* una superficie intermedia; si es NULL, `tex` */
	rsxkit_texture tex;             /* cualquier otra (rsxkit_target_texture,
	                                   rsxkit_fb_texture): offset, pitch, tamaño,
	                                   formato y lugar; wrap, filter y xf no se usan */
	uint8_t filter;                 /* rsxkit_filter */
	uint8_t wrap;                   /* RSXKIT_POST_* */
	uint8_t texels;                 /* 1: coordenadas en texeles, sin normalizar
	                                   (el centro del texel i está en i + 0,5) */
	uint8_t refresh;                /* 1: desligar/religar tras escrituras externas;
	                                   también fuerza su renovación en RPCS3 */
} rsxkit_post_input;

#define RSXKIT_POST_INPUTS 8
#define RSXKIT_POST_K      7

typedef struct rsxkit_post_pass {
	rsxkit_program *program;
	rsxkit_surface *out;     /* NULL: la pantalla del cuadro en curso */
	/* Destino alternativo; no combinar con out. NULL en ambos: pantalla. */
	rsxkit_target *out_target;
	rsxkit_rect rect;        /* píxeles de la salida que se dibujan (w o h en 0: toda) */
	/* TEXCOORD0 en el centro del píxel (i, j) del rectángulo:
	 *   xy = origin + (i + 0,5, j + 0,5)·step   (step en 0 vale 1)
	 *   zw = ((i + 0,5) / rect.w, (j + 0,5) / rect.h)
	 * Con origin = 0 y step = 1, xy son los píxeles desde la esquina del
	 * rectángulo, con la y hacia abajo. */
	float origin[2], step[2];
	rsxkit_post_input in[RSXKIT_POST_INPUTS];
	uint32_t inputs;
	/* Constantes de este dibujo: TEXCOORD1 .. TEXCOORD7 (iguales en todos los
	 * píxeles). */
	float k[RSXKIT_POST_K][4];
	/* Opcional: constantes afines. Con kx[j] o ky[j] distintos de 0,
	 * TEXCOORD(j + 1) vale k[j] + kx[j]·(i + 0,5) + ky[j]·(j + 0,5) en el
	 * centro del píxel (i, j) del rectángulo: lo que es lineal en la
	 * pantalla lo interpola el RSX sin cuentas en el programa. En 0 (con
	 * el memset de siempre), constantes. No cuentan en las mallas. */
	float kx[RSXKIT_POST_K][4], ky[RSXKIT_POST_K][4];
} rsxkit_post_pass;

/* Entrada que lee una superficie intermedia. */
rsxkit_post_input rsxkit_post_surface_input(const rsxkit_surface *s, rsxkit_filter filter, uint8_t wrap);
/* Entrada que lee una textura cualquiera en texeles. */
rsxkit_post_input rsxkit_post_texture_input(const rsxkit_texture *t, rsxkit_filter filter, uint8_t wrap);

/* Dibuja el rectángulo con el programa. Antes espera (en el RSX, no en el
 * PPE) a que termine lo anterior e invalida la caché de texturas: una
 * pasada puede leer lo que escribió la anterior, o un destino recién
 * dibujado. Deja ligada su salida: después de una pasada a una superficie,
 * el siguiente dibujo de rsxkit vuelve a ligar el suyo. Devuelve 0,
 * RSXKIT_EARG o RSXKIT_EDEPTH (la salida es más grande que la profundidad
 * de la pantalla, que va ligada con ella: rsxkit_depth_reserve). */
int rsxkit_post_run(const rsxkit_post_pass *p);

/* Borra la superficie (todos sus canales a 0) antes de sumarle mallas. */
int rsxkit_post_clear(rsxkit_surface *s);

/* ---- Pasadas con malla ------------------------------------------------------------ */

/* Un vértice de malla: la posición en el espacio de recorte (x/w e y/w en
 * [−1, 1] sobre la salida, la y hacia arriba; w > 0) y TEXCOORD0, que el
 * RSX interpola con corrección de perspectiva. */
typedef struct rsxkit_post_vertex {
	float pos[4];
	float t0[4];
} rsxkit_post_vertex;

typedef struct rsxkit_post_mesh {
	uint32_t vertices;  /* offset del primer rsxkit_post_vertex (alineado a 16) */
	uint32_t indices;   /* offset del primer índice (uint16_t, triángulos sueltos) */
	uint32_t count;     /* índices (múltiplo de 3) */
	uint8_t location;   /* RSXKIT_MEM_* de vértices e índices */
	uint8_t blend;      /* RSXKIT_BLEND_OFF, o RSXKIT_BLEND_ADD: suma sin peso (src + dst) */
	uint8_t cull;       /* rsxkit_cull (frente: antihorario con la y hacia arriba) */
	uint8_t pad;
} rsxkit_post_mesh;

/* Dibuja triángulos de una malla sobre la salida de la pasada `p`, con su
 * programa y sus entradas (rect, origin, step y k no se usan). Para sumar
 * en una superficie fp16 lo que aporta cada triángulo de una superficie
 * vista desde la cámara (una pasada que no busca nada por píxel: el
 * rasterizador da cada cruce del rayo). Los vértices y los índices los
 * escribe la CPU o un SPE; no cambiarlos hasta que el RSX los suelte
 * (rsxkit_fence). Devuelve 0, RSXKIT_EARG o RSXKIT_EDEPTH. */
int rsxkit_post_run_mesh(const rsxkit_post_pass *p, const rsxkit_post_mesh *m);

/* Reduce la región `region` (en texeles) de `src` con un filtro de caja:
 * cada píxel de `dst` es el promedio de un bloque factor × factor (1 a 6).
 * Escribe region.w / factor × region.h / factor píxeles desde la esquina
 * de `dst`. Sirve para llevar una imagen dibujada a resolución interna alta
 * a la de la consola (con factor 1 la copia tal cual). Por omisión en dos
 * pasadas: primero a lo ancho, con 3 lecturas bilineales por píxel, sobre
 * una tabla fp16 interna de (region.w / factor) × region.h, y después a lo
 * alto, para que el RSX lea la imagen grande por filas seguidas (en una
 * sola pasada cada píxel lee un bloque de factor filas de una superficie
 * lineal: 3,1 ms en la PS3 para 320 × 237 a factor 5, casi todo la lectura
 * y no las cuentas). Devuelve lo de rsxkit_post_run
 * o RSXKIT_ENOMEM (sin memoria para la tabla). */
int rsxkit_post_reduce(const rsxkit_texture *src, const rsxkit_rect *region, uint32_t factor, rsxkit_surface *dst);
/* 1: rsxkit_post_reduce en una sola pasada (3 × 3 lecturas bilineales por
 * píxel, sin tabla); 2: en dos. Devuelve lo que había. Otro valor solo
 * pregunta. */
int rsxkit_post_reduce_passes(int passes);

/* La última imagen subida a un framebuffer 2D, como textura. false si no
 * hay. */
bool rsxkit_fb_texture(const rsxkit_fb *fb, rsxkit_texture *out);

/* ---- Etiquetas: el RSX con un SPE o la CPU ----------------------------------------- */

/* Etiquetas del RSX que rsxkit no usa, para coordinarlo con otros: un SPE
 * espera a que el RSX deje algo en la memoria principal y el RSX espera a
 * que el SPE deje su resultado. */
#define RSXKIT_LABEL_USER_MIN 200
#define RSXKIT_LABEL_USER_MAX 239
/* En el búfer de comandos: cuando termine lo dibujado antes, el RSX escribe
 * `value` en la etiqueta `index`. Manda lo encolado al RSX. */
void rsxkit_post_label_write(uint32_t index, uint32_t value);
/* En el búfer de comandos: el RSX no sigue hasta que la etiqueta `index`
 * valga `value`. Si nadie la escribe se queda ahí: quien la pide debe
 * asegurarse de que se escriba (o escribirla desde la CPU para soltarlo). */
void rsxkit_post_label_wait(uint32_t index, uint32_t value);
/* La etiqueta en la memoria principal: la CPU la lee y la escribe; un SPE,
 * por DMA a esta dirección (16 bytes por etiqueta). */
volatile uint32_t *rsxkit_label_address(uint32_t index);

/* ---- Medición ---------------------------------------------------------------------- */

/* Marcas de tiempo del RSX: el RSX escribe su reloj, en ns, en la marca `i`
 * (0 .. RSXKIT_STAMPS − 1) cuando llega a ese punto del búfer de comandos,
 * después de terminar lo anterior. La diferencia entre dos marcas es lo que
 * tardó el RSX en lo mandado entre ellas (más lo que haya esperado órdenes
 * de la CPU, si no estaban todas). */
#define RSXKIT_STAMPS 64
void rsxkit_stamp(uint32_t i);
/* El valor escrito (ns). Válido cuando el RSX ya pasó por la marca: una
 * barrera posterior (rsxkit_fence_done). */
uint64_t rsxkit_stamp_ns(uint32_t i);

#ifdef __cplusplus
}
#endif

#endif
