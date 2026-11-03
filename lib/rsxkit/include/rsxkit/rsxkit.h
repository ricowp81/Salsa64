/* SPDX-License-Identifier: MIT */
/*
 * rsxkit — capa gráfica genérica para el RSX de la PS3 (PSL1GHT).
 *
 * No sabe nada de ningún sistema emulado: inicializa el RSX y la salida de
 * video, lleva el ciclo de cuadro (doble búfer y flip en vsync), presenta
 * imágenes 2D de la CPU escaladas a la pantalla y dibuja triángulos en
 * destinos de render con una combinación lineal de dos texturas y el color
 * del vértice. Documentación de la API en lib/rsxkit/README.md.
 *
 * Uso típico, una vez por cuadro:
 *
 *     rsxkit_frame_begin(0x000000);
 *     rsxkit_fb_upload(fb, pixels, w, h, pitch, RSXKIT_RGBA5551);
 *     rsxkit_fb_draw(fb, &view, NULL);
 *     rsxkit_frame_end();
 *
 * No es seguro entre hilos: todas las llamadas desde el mismo hilo.
 */
#ifndef RSXKIT_RSXKIT_H
#define RSXKIT_RSXKIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rsxkit/fpgen.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Inicialización ------------------------------------------------------ */

typedef struct rsxkit_config {
	uint32_t cmd_size;       /* búfer de comandos en bytes; 0 = 512 KB */
	uint32_t host_size;      /* memoria principal mapeada al RSX (incluye el
	                            búfer de comandos); 0 = 16 MB */
	const int32_t *modes;    /* VIDEO_RESOLUTION_* en orden de preferencia;
	                            NULL = 720p, 1080i/p, 480, 576 */
	uint32_t num_modes;
} rsxkit_config;

/* Devuelve 0 o un código negativo (RSXKIT_E*). cfg puede ser NULL. */
int rsxkit_init(const rsxkit_config *cfg);
/* Espera al RSX y libera todo. */
void rsxkit_shutdown(void);
/* Cambia el modo de video sin reiniciar el RSX: elige el primer modo de
 * cfg->modes que la tele dé (cfg NULL o sin modes: la lista por omisión de
 * rsxkit_init; cmd_size y host_size se ignoran), rehace videoConfigure y los
 * búferes de color y profundidad de la pantalla. Conserva el contexto, el
 * búfer de comandos, la memoria mapeada, los framebuffers, los destinos y
 * la 2D (rsxkit_screen_info() cambia: rehacer lo que dependa de su tamaño).
 * Llamar entre cuadros, sin destino ligado. Devuelve 0 o un código negativo;
 * si falla, deja el modo anterior (si tampoco se pudo, RSXKIT_ENOMEM y no
 * hay pantalla válida). */
int rsxkit_video_reconfigure(const rsxkit_config *cfg);

enum {
	RSXKIT_OK = 0,
	RSXKIT_EINIT = -1,     /* rsxInit o la memoria mapeada */
	RSXKIT_EVIDEO = -2,    /* ninguna resolución disponible o videoConfigure */
	RSXKIT_ENOMEM = -3,    /* memoria local del RSX */
	RSXKIT_EARG = -4,      /* argumento fuera de rango */
	RSXKIT_EDEPTH = -5,    /* ninguna zeta cubre el destino (rsxkit_depth_reserve) */
	RSXKIT_EFONT = -6,     /* la fuente del sistema: módulo, biblioteca o juego de fuentes (sysfont.h) */
};

typedef struct rsxkit_screen {
	uint32_t width, height;  /* píxeles de la superficie de salida */
	float aspect;            /* ancho / alto de la imagen en la tele (4:3 o 16:9) */
} rsxkit_screen;

const rsxkit_screen *rsxkit_screen_info(void);

/* Lo que hizo rsxkit_init con el video, para el log y para esperar a que
 * el monitor se recupere. Si la salida ya está en el modo elegido (el del
 * XMB), videoConfigure lleva exactamente la configuración vigente (formato,
 * aspecto y pitch de videoGetConfiguration, que en la PS3 es 8192 a 1080p
 * y no ancho × 4) y los búferes de pantalla usan ese pitch: así el monitor
 * no renegocia el HDMI por segunda vez. Con otro modo, la configuración de
 * siempre. */
typedef struct rsxkit_video_boot {
	int32_t prev_mode;        /* VIDEO_RESOLUTION_* que tenía la salida antes (-1: no se supo) */
	int32_t mode;             /* el elegido */
	int configured;           /* 1 si se llamó a videoConfigure, 0 si se conservó el modo */
	uint64_t configured_us;   /* sysGetSystemTime() justo después de videoConfigure (0 si no) */
	/* Lo que había antes de configurar (videoGetState y videoGetConfiguration). */
	int state_ok, state, color_space, scan_mode, conversion, aspect, refresh;
	int cfg_ok, cfg_resolution, cfg_format, cfg_aspect;
	uint32_t cfg_pitch;
	uint32_t pitch;           /* el de nuestros búferes de color */
	int used_current;         /* 1 si videoConfigure llevó los valores vigentes (formato, aspecto, pitch) */
	int tiles_active;
	char tiles[160];          /* las regiones de mosaico con algo (índice:tile/límite/pitch) */
} rsxkit_video_boot;
const rsxkit_video_boot *rsxkit_video_boot_info(void);

/* ---- Cuadro -------------------------------------------------------------- */

/* Empieza a dibujar en el búfer trasero y lo limpia con un color 0xRRGGBB.
 * Si el flip anterior no ocurrió, lo espera (en el PPE: el RSX nunca queda
 * parado esperando un flip). Si la aplicación va más rápido que la tele, la
 * frena aquí. */
void rsxkit_frame_begin(uint32_t clear_rgb);
/* Programa el flip en el próximo vsync y vuelve (solo espera si quedaba otro
 * en vuelo sin rsxkit_frame_begin en medio). Después del flip, rsxkit no da
 * por puesto nada del estado del RSX: en la PS3 el sistema dibuja su menú en
 * los flips de la aplicación y lo pisa. */
void rsxkit_frame_end(void);
/* true si rsxkit_frame_end no va a esperar: no hay flip en vuelo o ya
 * ocurrió. No bloquea. Sirve cuando el sistema retiene los flips (en la
 * PS3, mientras dibuja su menú encima de la aplicación): presentar solo si
 * es true, para no quedarse esperando. */
bool rsxkit_frame_ready(void);
/* Vblanks desde el arranque (gcmGetVBlankCount), para diagnosticar flips
 * lentos. */
uint64_t rsxkit_vblank_count(void);
/* Después de que el sistema dibujó encima de la aplicación (en la PS3, su
 * menú): olvida el estado del RSX que rsxkit daba por puesto (programas,
 * constantes del vertex program, arreglos de vértices, superficie, modo de
 * flip) para mandarlo todo de nuevo en el próximo dibujo: el sistema pisa
 * al menos las constantes. Revisa también los programas precompilados en la
 * memoria del RSX; devuelve cuántos encontró cambiados (los restaura y
 * vacía la caché de programas generados). */
int rsxkit_after_system(void);
/* Mapeos de rsxkit_map_main que ya no dan la misma dirección y el mismo
 * offset (diagnóstico). */
int rsxkit_main_maps_bad(void);
/* Espera a que el RSX termine todo lo enviado. */
void rsxkit_finish(void);
/* ---- Esperas largas ------------------------------------------------------ */

/* Si el RSX deja de avanzar (se colgó, o una orden espera algo que nunca
 * llega), el PPE se queda esperándolo: una barrera, rsxkit_finish, el flip
 * o lugar en el búfer de comandos. Estas funciones dejan ver y acotar esas
 * esperas. */
typedef enum rsxkit_wait_kind {
	RSXKIT_WAIT_NONE = 0,
	RSXKIT_WAIT_LABEL,  /* una etiqueta: barrera, rsxkit_finish, framebuffer, posprocesado */
	RSXKIT_WAIT_FLIP,   /* el flip en vuelo (rsxkit_frame_begin y _end) */
	RSXKIT_WAIT_FIFO,   /* lugar en el búfer de comandos (callback del sistema) */
} rsxkit_wait_kind;

typedef struct rsxkit_wait_info {
	rsxkit_wait_kind kind;
	const char *what;     /* "barrera", "rsxkit_finish", "framebuffer", "flip", "FIFO"… */
	uint32_t label;       /* LABEL: índice de la etiqueta */
	uint32_t expected;    /* LABEL: valor esperado; FIFO: palabras pedidas */
	uint32_t current;     /* LABEL: valor de la etiqueta; FLIP: estado del flip (0 = ocurrió) */
	uint32_t get, put;    /* GET y PUT del búfer de comandos (offsets del RSX), ahora */
	uint32_t get0;        /* GET al empezar la espera: si no cambió, el RSX no avanzó */
	uint64_t since_us;    /* sysGetSystemTime al empezar */
	uint64_t elapsed_us;
	uint32_t calls;       /* llamadas al gancho en esta espera (1: la primera) */
	bool done;            /* la espera terminó (solo si el gancho ya se había llamado) */
} rsxkit_wait_info;

/* Diagnóstico: palabras del búfer de comandos en un offset de E/S (GET/PUT). */
const uint32_t *rsxkit_cmd_at(uint32_t io_offset, uint32_t *avail);

/* El gancho se llama desde el hilo que espera, dentro de la llamada a
 * rsxkit que espera, cuando la espera pasa de `after_us` y después cada
 * `period_us` mientras siga, y una vez más al terminar (info->done). Sirve
 * para diagnosticar y para atender al sistema (sysUtilCheckCallback) o
 * salir; no puede llamar a rsxkit. Devuelve false para abandonar la espera:
 * rsxkit queda «perdido» (rsxkit_lost) y no vuelve a esperar al RSX (lo que
 * mande después puede perderse): queda cerrar y salir. La espera de lugar
 * en el búfer de comandos corre dentro del callback del sistema y no llama
 * al gancho: un vigía en otro hilo la ve con rsxkit_wait_now. fn NULL:
 * sin gancho. */
typedef bool (*rsxkit_wait_hook)(void *ctx, const rsxkit_wait_info *info);
void rsxkit_set_wait_hook(rsxkit_wait_hook fn, void *ctx, uint32_t after_us, uint32_t period_us);
/* La espera en curso (false si no hay ninguna), con GET, PUT y el tiempo
 * de ahora. Se puede llamar desde otro hilo (diagnóstico). */
bool rsxkit_wait_now(rsxkit_wait_info *out);
/* true si un gancho abandonó una espera. */
bool rsxkit_lost(void);
/* Prueba del gancho: deja al RSX girando para siempre en un salto a sí
 * mismo del búfer de comandos (todo lo que siga no llega). Solo para
 * probar lo que hace la aplicación con un RSX colgado. */
void rsxkit_debug_stall(void);
/* Nombre de una etiqueta de rsxkit ("barrera", "rsxkit_finish"…). */
const char *rsxkit_label_name(uint32_t index);

/* ---- Diagnóstico de un RSX colgado y blindaje ----------------------------- */

/* Todo lo de esta sección se puede llamar desde el gancho de las esperas y,
 * salvo rsxkit_abandon, desde otro hilo: solo lee memoria del PPE y los
 * registros de control del RSX. */

/* Excepciones del RSX que avisó el sistema (gcmSetGraphicsHandler, que
 * rsxkit_init registra): cuántas, el valor que pasó el sistema con la
 * primera y con la última, cuándo (sysGetSystemTime) y GET y PUT al llegar
 * la última. count 0: ninguna. */
typedef struct rsxkit_rsx_error {
	uint32_t count;
	uint32_t first, last;
	uint64_t first_us, last_us;
	uint32_t get, put;
} rsxkit_rsx_error;
void rsxkit_rsx_errors(rsxkit_rsx_error *out);

/* El búfer de comandos en offsets de E/S del RSX: dónde empieza la memoria
 * mapeada que lo contiene (base) y cuánto mide (size), el segmento en curso
 * (begin, end), dónde escribe rsxkit (current), y GET, PUT y REF del RSX.
 * Y hasta dónde llegó el RSX: la última barrera (rsxkit_fence) y el último
 * rsxkit_finish mandados y los que escribió. */
typedef struct rsxkit_fifo_info {
	uint32_t base, size;
	uint32_t begin, end, current;
	uint32_t get, put, ref;
	uint32_t fence_sent, fence_done;
	uint32_t finish_sent, finish_done;
} rsxkit_fifo_info;
void rsxkit_fifo_info_get(rsxkit_fifo_info *out);

/* Rastro de las últimas operaciones de rsxkit, anotado en el PPE (no manda
 * nada al RSX): qué fue, en qué offset del búfer de comandos empezó y sus
 * números (`fmt` es un formato de printf para a, b, c y d). Los dibujos
 * seguidos van en una sola entrada (n veces). Junto a GET, PUT y las
 * etiquetas dice qué tenía el RSX delante al colgarse. */
typedef struct rsxkit_trace_entry {
	const char *op, *fmt;
	uint32_t fifo;
	uint32_t n;
	uint32_t a, b, c, d;
	uint64_t seq;  /* número de la operación desde rsxkit_init */
} rsxkit_trace_entry;
#define RSXKIT_TRACE_LEN 64
/* Copia a `out` las últimas (hasta `max`), de la más vieja a la más nueva. */
unsigned rsxkit_trace_get(rsxkit_trace_entry *out, unsigned max);
/* «op números» de una entrada, para el log. */
int rsxkit_trace_format(const rsxkit_trace_entry *e, char *buf, size_t n);

/* Para salir con el RSX colgado: rsxkit deja de esperarlo (rsxkit_lost) y
 * de mandarle órdenes, y si GET no apunta a lo que falta mandar (está
 * dentro de la memoria del búfer y no más allá de PUT), pone PUT = GET para
 * que el RSX no tenga nada pendiente. *get y *put_before: los de antes.
 * Devuelve true si movió PUT. */
bool rsxkit_abandon(uint32_t *get, uint32_t *put_before);

/* Blindaje: lo que rsxkit ajusta para no mandarle al RSX algo que en la PS3
 * real lo dejó sin avanzar. Todo encendido por omisión.
 *   RSXKIT_SAFE_SIFM_EVEN: rsxkit_depth_load (motor de imagen escalada) con
 *   ancho de entrada par (lee un texel más por fila, que no llega a la
 *   salida). */
#define RSXKIT_SAFE_SIFM_EVEN 0x1u
#define RSXKIT_SAFE_ALL       0x1u
void rsxkit_set_safe(uint32_t flags);
uint32_t rsxkit_safe(void);

/* Búfer de color en que se está dibujando (X8R8G8B8), para pruebas: leerlo
 * desde el PPE es lento. Llamar a rsxkit_finish antes. */
const uint32_t *rsxkit_back_buffer(uint32_t *pitch_bytes);

/* ---- Framebuffer 2D ------------------------------------------------------ */

/* Formatos de píxel de origen. Todos se leen tal cual desde la textura
 * (el RSX convierte y reordena canales); ninguno pasa por el PPE. El orden
 * de bytes es el de la PS3 (big-endian) y el alfa se ignora al presentar. */
typedef enum rsxkit_format {
	RSXKIT_RGBA5551,  /* 16 bits: R[15:11] G[10:6] B[5:1] A[0] */
	RSXKIT_XRGB1555,  /* 16 bits: X[15] R[14:10] G[9:5] B[4:0] */
	RSXKIT_RGB565,    /* 16 bits: R[15:11] G[10:5] B[4:0] */
	RSXKIT_RGBA8888,  /* 32 bits: bytes R, G, B, A */
	RSXKIT_XRGB8888,  /* 32 bits: bytes X, R, G, B */
	RSXKIT_ARGB8888, /* bytes A, R, G, B; conserva alfa en posprocesado */
	RSXKIT_FORMAT_COUNT
} rsxkit_format;

typedef enum rsxkit_scale {
	RSXKIT_SCALE_FIT,      /* lo más grande posible con la proporción `aspect` */
	RSXKIT_SCALE_INTEGER,  /* múltiplo entero del tamaño, centrado */
	RSXKIT_SCALE_STRETCH,  /* toda la pantalla */
} rsxkit_scale;

typedef enum rsxkit_filter {
	RSXKIT_FILTER_LINEAR,
	RSXKIT_FILTER_NEAREST,
} rsxkit_filter;

typedef struct rsxkit_view {
	rsxkit_scale scale;
	rsxkit_filter filter;
	float aspect;  /* ancho / alto con que debe verse la imagen (4/3 para una
	                  consola de tele); 0 = píxeles cuadrados */
} rsxkit_view;

typedef struct rsxkit_rect {
	int32_t x, y;
	uint32_t w, h;
} rsxkit_rect;

typedef struct rsxkit_fb rsxkit_fb;

/* Reserva memoria del RSX para imágenes de hasta max_width × max_height
 * (dos copias: una se dibuja mientras se sube la otra). Hasta 4 a la vez.
 * NULL si no hay memoria. */
rsxkit_fb *rsxkit_fb_create(uint32_t max_width, uint32_t max_height);
void rsxkit_fb_destroy(rsxkit_fb *fb);

/* Copia una imagen de la memoria principal (pitch en bytes) a la textura.
 * Si el RSX aún dibuja con esa copia, espera. Devuelve 0 o RSXKIT_EARG. */
int rsxkit_fb_upload(rsxkit_fb *fb, const void *pixels, uint32_t width, uint32_t height,
		     uint32_t pitch, rsxkit_format format);

/* Dibuja la última imagen subida en el cuadro en curso. Si `rect` no es
 * NULL, recibe el rectángulo de pantalla que ocupó. */
void rsxkit_fb_draw(rsxkit_fb *fb, const rsxkit_view *view, rsxkit_rect *rect);

/* Rectángulo de pantalla para una imagen de width × height con `view`. */
rsxkit_rect rsxkit_layout(uint32_t width, uint32_t height, const rsxkit_view *view);

/* ---- Memoria principal y barreras ------------------------------------------ */

/* Hace visible para el RSX `size` bytes de memoria principal (dirección
 * alineada a 1 MB, tamaño múltiplo de 1 MB) y da su offset. Vértices y
 * texturas pueden vivir ahí: los escribe la CPU o un SPE sin copiarlos. */
int rsxkit_map_main(void *ptr, uint32_t size, uint32_t *offset);
/* Deshace un rsxkit_map_main (la misma dirección) para poder liberar esa
 * memoria: espera a que el RSX termine lo enviado. Devuelve 0 o
 * RSXKIT_EARG (no estaba mapeada). */
int rsxkit_unmap_main(void *ptr);

/* Barrera: marca lo enviado hasta ahora; rsxkit_fence_done dice si el RSX
 * ya lo terminó y rsxkit_fence_wait lo espera. rsxkit_fence_done solo lee
 * la etiqueta que escribe el RSX: es la única función que se puede llamar
 * desde otro hilo (un productor que espera a que el RSX suelte su búfer). */
uint32_t rsxkit_fence(void);
int rsxkit_fence_done(uint32_t fence);
void rsxkit_fence_wait(uint32_t fence);

/* ---- Destinos de render ------------------------------------------------------ */

typedef struct rsxkit_target rsxkit_target;  /* color X8R8G8B8 o A8R8G8B8, memoria local */
typedef struct rsxkit_program rsxkit_program; /* fragment program precompilado (post.h) */
typedef struct rsxkit_depth rsxkit_depth;    /* profundidad Z24S8, memoria local */

/* NULL si no hay memoria. El color nace negro y la profundidad en 1.
 * rsxkit_target_create: X8R8G8B8, sin alfa (el byte alto se ignora).
 *
 * rsxkit_target_create_ex con RSXKIT_TARGET_ALPHA: A8R8G8B8, un destino con
 * canal alfa, para lo que se dibuja y luego se usa como textura (render a
 * textura con transparencia: una imagen RGBA5551 de una consola de 16 bits).
 * Nace con alfa 0. Lo que lo escribe:
 *  - rsxkit_clear: el byte alto de `rgb` es el alfa;
 *  - rsxkit_draw: lo que diga rsxkit_draw.dst_alpha en cada píxel escrito
 *    (RSXKIT_DSTA_*: 1, el alfa del fragmento, el del destino sin cambiar o
 *    el mayor de los dos), con cualquier mezcla del color; las mezclas
 *    RSXKIT_BLEND_ALPHA_MAX, _REPLACE_MIN_ALPHA y _KEEP_MIN_ALPHA traen su
 *    propia cuenta del alfa (máximo o mínimo con el del destino);
 *  - rsxkit_target_fill / _fill_rows: el alfa de la imagen (el bit de alfa
 *    de RSXKIT_RGBA5551);
 *  - rsxkit_target_copy: se copia el alfa si el origen también lo tiene.
 * rsxkit_target_texture lo devuelve como RSXKIT_ARGB8888 (y los destinos sin
 * alfa como RSXKIT_XRGB8888): el 3D lee el alfa de los dos; el framebuffer
 * 2D, los blits y las pasadas de posprocesado lo leen solo de ARGB8888 (de
 * XRGB8888 toman 1). Sin alfa, la lectura trae lo que el RSX deje en el
 * byte alto de X8R8G8B8 (opaco).
 * En un destino con alfa, RSXKIT_DSTA_ONE cuesta una pasada más por dibujo
 * cuyo alfa no es 1 y MAX mezcla siempre: conviene para los que se leen
 * como textura, no para el cuadro principal. */
#define RSXKIT_TARGET_ALPHA 0x1u
rsxkit_target *rsxkit_target_create(uint32_t width, uint32_t height);
rsxkit_target *rsxkit_target_create_ex(uint32_t width, uint32_t height, uint32_t flags);
int rsxkit_target_has_alpha(const rsxkit_target *t);
/* Lo mismo que rsxkit_target_create_ex con RSXKIT_TARGET_ALPHA. */
static inline rsxkit_target *rsxkit_target_create_rgba(uint32_t width, uint32_t height)
{
	return rsxkit_target_create_ex(width, height, RSXKIT_TARGET_ALPHA);
}
void rsxkit_target_destroy(rsxkit_target *t);
void rsxkit_target_size(const rsxkit_target *t, uint32_t *width, uint32_t *height);
rsxkit_depth *rsxkit_depth_create(uint32_t width, uint32_t height);
void rsxkit_depth_destroy(rsxkit_depth *d);
/* Carga en la profundidad `d` una imagen de w × h palabras de 32 bits de la
 * memoria principal mapeada (offset alineado a 64, pitch múltiplo de 64):
 * cada palabra es la profundidad de 24 bits en los altos y el stencil en el
 * byte bajo (la misma disposición de Z24S8 que ve la CPU), agrandada `scale`
 * veces sin filtro (copia de 32 bits del motor 2D: exacta). Va encolada
 * entre esperas del RSX, así que lo dibujado antes no la ve y lo de después
 * sí. Las h filas van desde la fila y0 de la imagen (cargar solo las que
 * cambiaron). Para la z que un juego deja en la memoria con la CPU. Deja sin ligar
 * el destino: volver a rsxkit_bind. Devuelve 0 o RSXKIT_EARG. */
int rsxkit_depth_load(rsxkit_depth *d, uint32_t src_offset, uint32_t src_pitch, uint32_t w, uint32_t h,
		      uint32_t y0, uint32_t scale);

/* Dibujar en t (y su profundidad d, que puede ser NULL) hasta otro
 * rsxkit_bind o rsxkit_frame_begin. Las coordenadas de los vértices y
 * tijeras están en un espacio lógico de lw × lh (y hacia abajo) que se
 * estira sobre todo el destino: un núcleo de 320×240 puede dibujar a 3×
 * sin cambiar sus coordenadas.
 *
 * El RSX siempre tiene una zeta ligada junto al color. Sin d (o con una
 * más chica que t), se liga la profundidad de la pantalla con la prueba y
 * la escritura apagadas; si tampoco cubre a t (un destino más grande que la
 * pantalla, como 1600×1200 en 720p), no se liga nada y devuelve
 * RSXKIT_EDEPTH: una zeta más chica que el color puede fallar o recortar en
 * el RSX. Para eso está rsxkit_depth_reserve. Devuelve 0 o RSXKIT_EARG /
 * RSXKIT_EDEPTH (lo ligado antes sigue ligado). */
int rsxkit_bind(rsxkit_target *t, rsxkit_depth *d, float lw, float lh);
/* Agranda la profundidad de la pantalla a al menos width × height (lo que
 * mida el mayor destino que se vaya a ligar sin profundidad propia; 0 × 0
 * vuelve al tamaño de la pantalla). Reserva ahora, fuera del bucle de
 * dibujo: espera al RSX y olvida lo ligado. Se conserva en
 * rsxkit_video_reconfigure. Devuelve 0, RSXKIT_EARG o RSXKIT_ENOMEM (queda
 * la reserva anterior). */
int rsxkit_depth_reserve(uint32_t width, uint32_t height);
/* Lo mismo sobre la pantalla del cuadro en curso (después de
 * rsxkit_frame_begin), sin profundidad: menús y superposiciones. */
void rsxkit_bind_frame(float lw, float lh);

#define RSXKIT_CLEAR_COLOR 0x1u
#define RSXKIT_CLEAR_DEPTH 0x2u
#define RSXKIT_CLEAR_ALPHA 0x4u /* el byte alto de rgb es el alfa aunque el destino no tenga canal alfa */
/* Borra en el destino ligado el rectángulo lógico (x0, y0, x1, y1), o todo
 * si rect es NULL. depth en [0, 1]. rgb es 0x00RRGGBB; en un destino con
 * alfa (RSXKIT_TARGET_ALPHA), o con RSXKIT_CLEAR_ALPHA, el byte alto es el
 * alfa (0 si se deja en 0). */
void rsxkit_clear(uint32_t flags, uint32_t rgb, float depth, const float rect[4]);
void rsxkit_clear_stencil(uint8_t value);

/* Presenta en el cuadro en curso la región `src` del destino (píxeles del
 * destino; NULL = todo), acomodada como una imagen de img_w × img_h con
 * `view`. */
void rsxkit_target_present(rsxkit_target *t, const rsxkit_rect *src, uint32_t img_w, uint32_t img_h,
			   const rsxkit_view *view, rsxkit_rect *out);

/* ---- Dibujo 3D ------------------------------------------------------------------ */

/* Vértice de 32 bytes: posición homogénea en el espacio lógico (x/w, y/w en
 * píxeles lógicos, z/w en [0, 1], w > 0 delante), coordenadas de textura y
 * color RGBA de 8 bits. `pad` puede llevar un segundo color RGBA de 8 bits
 * que sustituye a `rgba` en los dibujos siguientes a
 * rsxkit_vertices_color_aux(1) (datos por vértice para un programa propio). */
typedef struct rsxkit_vertex {
	float pos[4];
	float st[2];
	uint8_t rgba[4];
	uint32_t pad;
} rsxkit_vertex;

enum { RSXKIT_MEM_LOCAL = 0, RSXKIT_MEM_MAIN = 1 };
/* RSXKIT_WRAP_MIRROR_ONCE: la coordenada se refleja una vez alrededor de 0
 * (u en [−1, 0] es el espejo de [0, 1]) y fuera de [−1, 1] se fija el borde
 * (MIRROR_ONCE_CLAMP_TO_EDGE del RSX). */
enum { RSXKIT_WRAP_CLAMP = 0, RSXKIT_WRAP_REPEAT = 1, RSXKIT_WRAP_MIRROR = 2, RSXKIT_WRAP_MIRROR_ONCE = 3 };

/* Textura lineal (pitch múltiplo de 64, offset alineado a 128). Las
 * coordenadas del vértice se llevan a la textura con
 * u = s·xf[0] + xf[2], v = t·xf[1] + xf[3] (normalizadas). */
typedef struct rsxkit_texture {
	uint32_t offset;
	uint32_t pitch;
	uint16_t width, height;
	uint8_t location;   /* RSXKIT_MEM_* */
	uint8_t format;     /* rsxkit_format */
	uint8_t wrap_s, wrap_t;
	uint8_t filter;     /* rsxkit_filter */
	uint8_t remap;      /* rsxkit_remap: canales que se leen */
	uint8_t pad[2];
	float xf[4];
} rsxkit_texture;

/* Vista prestada del búfer actual, sin copiar ni reservar memoria. El ticket
 * más reciente permite leerlo tras el flip, mientras no se haya vuelto a dibujar en
 * él. Comprobar valid antes de cada lectura; guardar de nuevo tras copiarlo
 * al otro búfer. La UI, el sistema y un cambio de salida invalidan la vista.
 * Quien escriba directamente en la pantalla debe invalidarla primero. */
uint64_t rsxkit_screen_texture(rsxkit_texture *out);
bool rsxkit_screen_texture_valid(uint64_t ticket);
void rsxkit_screen_texture_invalidate(void);

/* Canales que lee una textura: los de su formato, o el rojo en los cuatro
 * (una imagen de intensidad dibujada en un destino: R = G = B = A). */
typedef enum rsxkit_remap {
	RSXKIT_REMAP_NONE,
	RSXKIT_REMAP_RRRR,
} rsxkit_remap;

typedef enum rsxkit_blend {
	RSXKIT_BLEND_OFF,    /* reemplaza */
	RSXKIT_BLEND_ALPHA,  /* src·a + dst·(1 − a) */
	RSXKIT_BLEND_ADD,    /* src·a + dst */
	RSXKIT_BLEND_NONE,   /* no escribe color */
	RSXKIT_BLEND_ALPHA_MAX, /* RGB: src*a+dst*(1-a); alfa: máximo */
	RSXKIT_BLEND_REPLACE_MIN_ALPHA, /* RGB: reemplaza; alfa: mínimo */
	RSXKIT_BLEND_KEEP_MIN_ALPHA,    /* RGB: conserva destino; alfa: mínimo */
	RSXKIT_BLEND_MUL,        /* dst·a (oscurece lo dibujado según el alfa) */
	RSXKIT_BLEND_ADD_MUL,    /* src + dst·a */
	RSXKIT_BLEND_ALPHA_INV,  /* src·(1 − a) + dst·a */
	RSXKIT_BLEND_ADD_ONE,    /* src + dst (suma sin peso) */
} rsxkit_blend;

typedef enum rsxkit_cull {
	RSXKIT_CULL_NONE,
	RSXKIT_CULL_FRONT,  /* frente: antihorario con la y hacia arriba */
	RSXKIT_CULL_BACK,
} rsxkit_cull;

#define RSXKIT_DEPTH_TEST   0x1u
#define RSXKIT_DEPTH_WRITE  0x2u
#define RSXKIT_DEPTH_OFFSET 0x4u  /* acercar un poco (calcomanías) */
/* Todo el dibujo con la profundidad `z` de rsxkit_draw (en [0, 1]) en vez
 * de la de sus vértices: la pone el viewport, sin la división entre w, así
 * que dos dibujos con la misma z dan la misma en cada píxel y la prueba
 * LEQUAL deja pasar al último (orden de pintor con z-buffer). */
#define RSXKIT_DEPTH_CONST  0x8u
#define RSXKIT_DEPTH_GEQUAL 0x20u /* la prueba es >= */
#define RSXKIT_DEPTH_ALWAYS 0x40u /* escribir tras una prueba separada */
#define RSXKIT_DEPTH_LESS   0x10u /* la prueba es < en vez de ≤ (con la misma z no pasa) */
#define RSXKIT_DEPTH_EQUAL  0x80u /* la prueba es == (solo pasa la misma z) */

/* Lo que evalúa el RSX por píxel, con
 *   X_j = k[j][0] + k[j][1]·C + k[j][2]·C.aaaa    (C = color del vértice)
 * (los X_j los arma el vertex program; con niebla, X_j.rgb se escala por
 * (1 − C.a) y el término constante suma fog·C.a):
 * - nterms = 0: la combinación lineal fija, con programas precompilados,
 *     color = T0·X0 + T0.aaaa·X1 + T1·X2 + T1.aaaa·X3 + X4
 *   (términos RSXKIT_K_*; el constante es X4).
 * - nterms > 0: un fragment program generado para estos términos
 *   (rsxkit/fpgen.h; se genera una vez y queda en caché),
 *     color = Σ_j (Π factors[j]) · X_j
 *   con los factores RSXKIT_FP_* (texturas, sus alfas, color del vértice
 *   y su alfa; 0 termina la lista). Con niebla, el color de niebla se suma
 *   al primer término sin factores: incluir uno. Hasta RSXKIT_K_TERMS
 *   términos de RSXKIT_K_FACTORS factores. */
enum { RSXKIT_K_T0, RSXKIT_K_T0A, RSXKIT_K_T1, RSXKIT_K_T1A, RSXKIT_K_ONE, RSXKIT_K_LINCOMB };
#define RSXKIT_K_TERMS   RSXKIT_FP_MAX_TERMS
#define RSXKIT_K_FACTORS RSXKIT_FP_MAX_FACTORS

/* rsxkit_draw.dst_alpha: el alfa que queda en cada píxel escrito de un
 * destino con RSXKIT_TARGET_ALPHA, con cualquier mezcla del color (la del
 * color no toca el alfa). No cuenta con las mezclas que traen su propia
 * cuenta del alfa (RSXKIT_BLEND_ALPHA_MAX, _REPLACE_MIN_ALPHA,
 * _KEEP_MIN_ALPHA). Hasta 2026-10-31 RSXKIT_DSTA_ONE dejaba en realidad el
 * alfa del fragmento (la mezcla de alfa ONE / ZERO lo copia): quien dependa
 * de eso pasa a RSXKIT_DSTA_FRAG.
 * La mezcla del RSX no da una constante (el alfa sale de As·fs + Ad·fd y
 * todos los factores multiplican), así que RSXKIT_DSTA_ONE se hace con dos
 * pasadas cuando el alfa del fragmento no es 1 en todo el dibujo: primero
 * una solo en el canal alfa, con el mismo programa, la misma prueba de
 * profundidad (sin escribirla) y el alfa llevado a 1 o más (escalado por
 * 1/ref con prueba de alfa, para descartar los mismos fragmentos; más 1 sin
 * ella), y después el dibujo con RSXKIT_DSTA_KEEP. Con un programa propio
 * (rsxkit_draw.program), o sin lugar para un término constante, queda el
 * alfa del fragmento (rsxkit_dsta_stats lo cuenta). */
#define RSXKIT_DSTA_ONE  0  /* alfa 1 (en dos pasadas si hace falta) */
#define RSXKIT_DSTA_FRAG 1  /* el alfa del fragmento */
#define RSXKIT_DSTA_KEEP 2  /* el del destino, sin cambiar (máscara de color sin el alfa) */
#define RSXKIT_DSTA_MAX  3  /* el mayor entre el del fragmento y el del destino (ecuación MAX) */

#define RSXKIT_DSTA_PASS 4  /* interno: la pasada del alfa (solo el canal alfa, sin mezcla) */


/* Dibujos con RSXKIT_DSTA_ONE en un destino con alfa desde el arranque: los
 * que llevaron la pasada del alfa, los de una sola pasada (el alfa del
 * fragmento ya era 1) y los que quedaron con el del fragmento. */
void rsxkit_dsta_stats(uint32_t *passes, uint32_t *single, uint32_t *failed);

/* Autoprueba de dst_alpha en el RSX: en un destino con alfa borrado con
 * alfa 0x60 dibuja un cuadrado por caso con el alfa del fragmento en 0x20
 * (0xA0 en el último) y lee el alfa que quedó: RSXKIT_DSTA_ONE sin mezcla,
 * con la mezcla RSXKIT_BLEND_ALPHA, con una prueba de alfa que pasa (ref
 * 0x10) y con una que descarta (ref 0x40); FRAG; KEEP; MAX bajo y MAX alto.
 * rsxkit_dsta_expected es lo que deben dar (FF FF FF 60 20 60 60 A0). Usa
 * memoria local propia, espera al RSX y deja sin destino ligado y sin
 * vértices (llamar fuera del dibujo, p. ej. al arrancar). 0 o RSXKIT_E*. */
#define RSXKIT_DSTA_TEST_N 8
extern const uint8_t rsxkit_dsta_expected[RSXKIT_DSTA_TEST_N];
int rsxkit_dsta_selftest(uint8_t out[RSXKIT_DSTA_TEST_N]);

typedef struct rsxkit_draw {
	/* Programa opcional: entradas de rsxkit_lincomb.vcg. NULL conserva la combinación. */
	rsxkit_program *program;
	/* Stencil: 0 apagado, 1 siempre, 2 distinto, 3 igual.
	 * op: 0 reemplazar, 1 invertir, 2 conservar. Máscaras explícitas si está activo. */
	uint8_t stencil, stencil_ref, stencil_compare_mask, stencil_write_mask, stencil_op;
	/* Operación al fallar profundidad: 0 conservar, 1 reemplazar, 2 poner a cero. */
	uint8_t stencil_zfail;
	/* Operación al fallar la prueba de stencil: 0 conservar, 1 reemplazar, 2 poner a cero. */
	uint8_t stencil_sfail;
	uint8_t blend;      /* rsxkit_blend */
	uint8_t dst_alpha;  /* RSXKIT_DSTA_*: alfa que deja en un destino con alfa */
	uint8_t cull;       /* rsxkit_cull */
	uint8_t depth;      /* RSXKIT_DEPTH_* */
	uint8_t alpha_ref;  /* 0: sin prueba de alfa; si no, pasa alfa >= ref/255 */
	uint8_t ntex;       /* texturas: 0, 1 o 2 */
	uint8_t fog;
	uint8_t nterms;     /* 0: combinación lineal fija; si no, términos con `factors` */
	/* Distinto de 0: antes de los triángulos, sus aristas como líneas de
	 * edges/8 píxeles de ancho de la superficie (modo de polígono LINE, un
	 * poco más atrás en z) con el mismo estado. Los triángulos pintan
	 * encima de lo que cubren y las líneas quedan solo en las grietas
	 * finas entre triángulos vecinos (uniones en T), que el muestreo en el
	 * centro del píxel deja ver. Para dibujos opacos: con mezcla, la arista
	 * se mezclaría dos veces. */
	uint8_t edges;
	float scissor[4];   /* x0, y0, x1, y1 lógicos; se recortan al destino y,
	                       vacías (x0 ≥ x1 o y0 ≥ y1), el dibujo no se manda */
	float fog_color[4];
	rsxkit_texture tex[2];
	uint8_t factors[RSXKIT_K_TERMS][RSXKIT_K_FACTORS];  /* RSXKIT_FP_*, 0 = fin */
	float z;            /* con RSXKIT_DEPTH_CONST: la profundidad de todo el dibujo */
	float k[RSXKIT_K_TERMS][3][4];
} rsxkit_draw;

/* La pasada del alfa de RSXKIT_DSTA_ONE (lib/rsxkit/src/dsta.c, sin
 * PSL1GHT: se prueba en el host). rsxkit_dsta_alpha_is_one: el alfa del
 * fragmento es 1 en todo el dibujo (basta una pasada). rsxkit_dsta_alpha_pass
 * arma en `b` la pasada del alfa de `d`; false si no se puede. */
bool rsxkit_dsta_alpha_is_one(const rsxkit_draw *d);
bool rsxkit_dsta_alpha_pass(const rsxkit_draw *d, rsxkit_draw *b);

/* Programas generados: cuántos distintos hay en la caché, cuántos se
 * generaron en total (con los desalojos, si la caché se llenó) y cuántas
 * veces se cargó un programa distinto del anterior (incluidos los
 * precompilados). */
typedef struct rsxkit_fp_stats {
	uint32_t programs;
	uint32_t generated;
	uint32_t evictions;
	uint32_t loads;
} rsxkit_fp_stats;
void rsxkit_fp_get_stats(rsxkit_fp_stats *out);

/* Arreglo de rsxkit_vertex que usan los dibujos siguientes. */
void rsxkit_vertices(uint32_t offset, uint8_t location);
/* Cuadros intermedios: un arreglo de posiciones de antes (un float[4] por
 * vértice, en el mismo orden que el de rsxkit_vertices) y la fracción t; los
 * dibujos siguientes ponen cada vértice en prev + (pos − prev)·t (t = 1: donde
 * está). Hasta el próximo rsxkit_vertices, que lo apaga. */
void rsxkit_vertices_prev(uint32_t offset, uint8_t location, float t);
/* 1: los dibujos siguientes leen COLOR0 de la palabra `pad` de cada vértice
 * en vez de `rgba`; 0 (inicial) vuelve a `rgba`. Vale hasta que se cambie. */
void rsxkit_vertices_color_aux(int aux);
/* Triángulos sueltos: vértices first .. first + count − 1 del arreglo. */
/* Optativo: 1 envía solo constantes/stencil cambiados; 0 (inicial) envía completos. */
void rsxkit_3d_incremental(int enabled);
/* Optativo: 1 recarga el programa de rsxkit_draw.program y escribe su
 * etiqueta de uso en cada dibujo; 0 (inicial) solo al cambiar de programa,
 * al salir del 3D (pasadas, blits) y antes de destruirlo. */
void rsxkit_3d_program_sync(int per_draw);
void rsxkit_draw_triangles(const rsxkit_draw *d, uint32_t first, uint32_t count);
/* Sombreado lineal en pantalla: los términos X_j (color del vértice, su
 * alfa y la niebla) se interpolan sin corrección de perspectiva, como el
 * sombreado Gouraud del RDP de la N64 y de otras consolas de la época; las
 * coordenadas de textura siguen con perspectiva. El vertex program
 * multiplica cada X_j por w y los fragment programs lo leen con el bit del
 * RSX que entrega la entrada dividida entre w (docs/rsx-notes.md): no cuesta
 * instrucciones por píxel. Apagado al iniciar; vale para los dibujos
 * siguientes. */
void rsxkit_affine_shade(int on);

/* ---- Un destino como textura y de vuelta a la memoria principal --------------
 * Render a textura: lo dibujado en un destino se puede leer en un dibujo 3D
 * (rsxkit_draw.tex) sin copiarlo. La textura son `rows` filas enteras del
 * destino desde la fila `y0` (el offset de una textura va alineado a 128:
 * una columna cualquiera no; la columna va en xf). Deja en `out` offset,
 * pitch, tamaño, formato y lugar; quien llama pone wrap, filter y xf. El
 * destino no puede ser el que está ligado. */
void rsxkit_target_texture(const rsxkit_target *t, uint32_t y0, uint32_t rows, rsxkit_texture *out);
/* Antes del primer dibujo que lea como textura un destino dibujado desde
 * la última vez que se llamó: espera a que el RSX termine lo enviado e
 * invalida su caché de texturas. */
void rsxkit_texture_sync(void);

/* Copia a la memoria principal (mapeada con rsxkit_map_main) la región
 * `src` del destino (NULL: todo), reducida a img_w × img_h píxeles con
 * filtro lineal (o copiada tal cual si ya miden lo mismo:
 * rsxkit_target_read_exact), en `format` (RSXKIT_RGB565, RSXKIT_XRGB8888 o
 * RSXKIT_ARGB8888: este último con el alfa de un destino creado con
 * RSXKIT_TARGET_ALPHA, filtrado como el color, y 1 si no tiene) y con
 * `dst_pitch` bytes por fila (múltiplo de 64; offset alineado a 128). Lo
 * hace el RSX con el pipeline 3D sobre una superficie en memoria
 * principal: encola el trabajo y vuelve; rsxkit_fence/rsxkit_fence_wait
 * dicen cuándo está escrito. Los píxeles de 16 bits quedan en el orden de
 * bytes de la PS3 (big-endian), como los lee rsxkit_fb_upload, y sin
 * tramado (dithering): cada píxel es el redondeo del filtro, sin un patrón
 * que dependa de su posición. Deshace el destino ligado: volver a llamar a
 * rsxkit_bind para seguir dibujando. Devuelve 0, RSXKIT_EARG o
 * RSXKIT_EDEPTH (img_w × img_h no cabe en la profundidad de la pantalla,
 * que va ligada con la salida). */
int rsxkit_target_read(const rsxkit_target *t, const rsxkit_rect *src, uint32_t dst_offset, uint32_t dst_pitch,
		       uint32_t img_w, uint32_t img_h, rsxkit_format format);

/* Filtro de rsxkit_target_read: lineal cuando reduce (img_w × img_h menor
 * que la región), y el más cercano cuando la región ya mide img_w × img_h
 * (copia 1:1). El filtro lineal a 1:1 no es una copia exacta en el RSX: el
 * vértice se ajusta a su subpíxel y la muestra cae a una fracción de texel
 * del centro, así que mezcla una parte del vecino; con imágenes que llevan
 * un índice de paleta en el rojo (una capa de 8 bits) un índice mezclado es
 * otro color (puntos sueltos en cada borde). `exact` = 0 vuelve al lineal
 * siempre (para comparar); por omisión, 1. */
static inline int rsxkit_read_filter_nearest(uint32_t src_w, uint32_t src_h, uint32_t img_w, uint32_t img_h, int exact)
{
	return exact && src_w == img_w && src_h == img_h;
}
void rsxkit_target_read_exact(int exact);

/* Copia todo el destino `src` en `dst` (del mismo tamaño o mayor; se
 * estira si no), para leer como textura lo que se está dibujando en
 * `src`. Deja `dst` ligado, sin profundidad: volver a rsxkit_bind.
 * Devuelve 0, RSXKIT_EARG, RSXKIT_ENOMEM o RSXKIT_EDEPTH (como rsxkit_bind
 * sin profundidad). */
int rsxkit_target_copy(rsxkit_target *dst, const rsxkit_target *src);

/* Pinta todo el destino con una imagen de w × h píxeles de la memoria
 * principal mapeada (offset alineado a 128, pitch múltiplo de 64, en
 * `format`), estirada texel por píxel (sin filtro): para cebar un destino
 * con lo que la CPU dejó en su memoria antes de que el 3D dibuje encima.
 * Deja el destino ligado, sin profundidad: volver a rsxkit_bind con ella.
 * Devuelve 0, RSXKIT_EARG o RSXKIT_EDEPTH (como rsxkit_bind sin
 * profundidad). */
/* Como rsxkit_target_fill, solo con las filas [y0, y1) de la imagen (en las
 * mismas filas del destino, a su escala): para volver a cebar lo que se
 * dibujó encima sin subir la imagen entera. */
int rsxkit_target_fill_rows(rsxkit_target *t, uint32_t src_offset, uint32_t src_pitch, uint32_t w, uint32_t h,
			    uint32_t y0, uint32_t y1, rsxkit_format format);
/* Replace a rectangular region of a target from a packed source image. The
 * destination rectangle uses physical target pixels; source dimensions and
 * pitch describe the main-memory image. */
int rsxkit_target_fill_rect(rsxkit_target *t, uint32_t src_offset, uint32_t src_pitch, uint32_t src_w,
			    uint32_t src_h, uint32_t dst_x, uint32_t dst_y, uint32_t dst_w, uint32_t dst_h,
			    rsxkit_format format);
/* Replace a rectangle without alpha blending (for clearing previously covered
 * CPU-written pixels whose RGBA5551 coverage bit has just become zero). */
int rsxkit_target_replace_rect(rsxkit_target *t, uint32_t src_offset, uint32_t src_pitch, uint32_t src_w,
			       uint32_t src_h, uint32_t dst_x, uint32_t dst_y, uint32_t dst_w, uint32_t dst_h,
			       rsxkit_format format);
int rsxkit_target_fill(rsxkit_target *t, uint32_t src_offset, uint32_t src_pitch, uint32_t w, uint32_t h,
		       rsxkit_format format);

/* RGB565 (16 bits, orden de bytes de la máquina) → RGBA5551 con alfa 1,
 * n píxeles; dst y src pueden ser el mismo. Para dejar en el formato de
 * una consola de 16 bits lo que escribe rsxkit_target_read. */
void rsxkit_pixels_565_to_5551(uint16_t *dst, const uint16_t *src, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif
