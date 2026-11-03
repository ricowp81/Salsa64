/* SPDX-License-Identifier: MIT */
/* rsxkit — estado compartido entre los módulos. */
#ifndef RSXKIT_INTERNAL_H
#define RSXKIT_INTERNAL_H

#include <rsx/rsx.h>

#include "rsxkit/post.h"
#include "rsxkit/rsxkit.h"

#define RSXKIT_FB_MAX       4
/* Etiquetas del RSX: 255 para rsxkit_finish y hacia abajo las de cada copia
 * de cada framebuffer. Las de 0 a 63 son del sistema. */
#define RSXKIT_LABEL_FINISH 255
#define RSXKIT_LABEL_FB(i, slot) (254 - 2 * (i) - (slot))
#define RSXKIT_LABEL_FENCE  246
/* Posprocesado (post.c): el último dibujo de cada programa y superficie. */
#define RSXKIT_LABEL_POST   245
/* Marcas de tiempo (post.c): reportes del RSX desde este índice. */
#define RSXKIT_STAMP_BASE   (2048 - RSXKIT_STAMPS)

struct rsxkit_state {
	gcmContextData *ctx;
	void *host;
	uint32_t host_size;
	rsxkit_screen screen;
	int32_t mode_id;        /* VIDEO_RESOLUTION_* en uso */
	uint32_t color_pitch, depth_pitch;
	uint32_t color_offset[2], depth_offset;
	uint32_t *color_buf[2];
	uint32_t *depth_buf;
	/* La profundidad de la pantalla mide depth_w × depth_h: el mayor entre
	 * la pantalla y lo pedido con rsxkit_depth_reserve (depth_min_*). Es la
	 * que liga un destino sin profundidad propia, así que rsxkit_bind no
	 * liga un destino que no quepa en ella. */
	uint32_t depth_w, depth_h, depth_min_w, depth_min_h;
	uint32_t cur;           /* búfer en que se dibuja */
	uint32_t finish_seq;
	uint32_t fence_seq;
	int flip_pending;
	/* Estado del dibujo 3D (draw3d.c) que hay que olvidar cuando otro
	 * módulo cambia programas, arreglos de vértices o superficie. */
	int arrays_bound;       /* hay arreglos de vértices activos */
	int prog_3d;            /* el vertex program cargado es el 3D */
	int fp_3d;              /* fragment program 3D cargado (-1: otro) */
	int state_valid;        /* el estado fijo 3D está puesto */
	int screen_bound;       /* la superficie es la pantalla (no un destino) */
};

extern struct rsxkit_state rsxkit_g;

int rsxkit_program_bind_3d(rsxkit_program *p);
void rsxkit_program_used_3d(rsxkit_program *p);

/* Destino ligado con rsxkit_bind (target.c). */
struct rsxkit_bound {
	struct rsxkit_target *target;
	struct rsxkit_depth *depth;
	float lw, lh;        /* espacio lógico */
	float sx, sy;        /* píxeles del destino por unidad lógica */
	int scissor_valid;
	uint32_t scissor[4]; /* último puesto, en píxeles */
	uint32_t w, h;       /* superficie del destino, en píxeles */
	float vp_z;          /* z fija del viewport (RSXKIT_DEPTH_CONST); < 0: la de los vértices */
	int alpha;           /* el destino tiene canal alfa (RSXKIT_TARGET_ALPHA) */
};
extern struct rsxkit_bound rsxkit_bound;
void rsxkit_logical_rect(const float r[4], uint32_t out[4]);

/* Asegura `words` palabras libres en el segmento en curso del búfer de
 * comandos (fifo.c). Cada operación pública lo llama antes de escribir. */
void rsxkit_reserve(uint32_t words);
#define RSXKIT_RESERVE_DRAW 512u

/* Espera a que la etiqueta llegue a `value` (comparación con vuelta). */
void rsxkit_wait_label(uint8_t index, uint32_t value);
/* Esperas largas (wait.c): cada espera que no se cumple de entrada anota
 * qué espera (begin), da una vuelta de tick por cada sondeo (false: la
 * aplicación la abandonó, salir del bucle) y termina con end. */
void rsxkit_wait_begin(rsxkit_wait_kind kind, const char *what, uint32_t label, uint32_t expected, uint32_t current);
bool rsxkit_wait_tick(uint32_t current);
void rsxkit_wait_end(void);
/* rsxkit_abandon: como si un gancho hubiera abandonado una espera. */
void rsxkit_wait_set_lost(void);
/* Diagnóstico (diag.c): el aviso de excepciones del sistema y el rastro de
 * operaciones. rsxkit_trace_note anota una operación con su offset en el
 * búfer de comandos (antes de escribir sus órdenes); rsxkit_trace_repeat
 * junta las seguidas iguales (dibujos) en una entrada. `fmt` es para a..d. */
void rsxkit_diag_init(void);
void rsxkit_diag_shutdown(void);
void rsxkit_trace_note(const char *op, const char *fmt, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
void rsxkit_trace_repeat(const char *op, const char *fmt, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
/* Viewport y estado fijo del dibujo 2D sobre la superficie en curso. */
void rsxkit_set_2d_state(void);
/* Vuelve a la pantalla (búfer trasero) como superficie, con el estado 2D. */
void rsxkit_bind_screen(void);
void rsxkit_fb_shutdown(void);
/* Antes de dibujar en modo inmediato con otros programas: apaga los
 * arreglos de vértices del 3D y olvida su estado. */
void rsxkit_leave_3d(void);
/* Escribe la etiqueta de uso del programa 3D con dibujos pendientes. */
void rsxkit_3d_flush_use(void);
/* Olvida el arreglo de vértices de rsxkit_vertices (se liberó su memoria):
 * hasta el próximo rsxkit_vertices no se dibuja. */
void rsxkit_3d_forget_vertices(void);
/* Olvida todo el estado del RSX que rsxkit da por puesto (arreglos de
 * vértices, programas, estado fijo y constantes del 3D, superficie): el
 * próximo dibujo lo manda de nuevo. Después de cada flip (frame_end). */
void rsxkit_forget_state(void);
void rsxkit_3d_shutdown(void);
void rsxkit_target_shutdown(void);
void rsxkit_post_shutdown(void);
/* Copia un fragment program a la memoria del RSX, con interpolación de
 * perspectiva en todas sus entradas (fprog.c). NULL si no hay memoria. */
void *rsxkit_fp_upload(rsxFragmentProgram *fp, u32 *offset);
/* Igual, pero las instrucciones que leen TEXCOORD1..7 (los términos X_j)
 * quedan sin corrección de perspectiva: el sombreado lineal en pantalla de
 * rsxkit_affine_shade (el vertex program multiplica los X_j por w). */
void *rsxkit_fp_upload_affine(rsxFragmentProgram *fp, u32 *offset);
/* Programas subidos con rsxkit_fp_upload o registrados con
 * rsxkit_fp_track cuya copia en la memoria del RSX cambió; con `repair`,
 * los vuelve a copiar. */
int rsxkit_fp_verify(int repair);
/* Registra (o, si ya estaba, vuelve a sumar) la copia `buf` de un programa
 * para rsxkit_fp_verify, que la restaura desde `src` (`fix` 1: limpiando la
 * corrección de perspectiva como rsxkit_fp_upload; 2: como
 * rsxkit_fp_upload_affine). */
void rsxkit_fp_track(void *buf, const void *src, u32 size, int fix);
void rsxkit_fp_untrack(void *buf);
void rsxkit_fp_fix_perspective(void *ucode, u32 size);
/* Formato y reordenamiento de canales de textura del RSX para un
 * rsxkit_format (fb.c). */
void rsxkit_format_gcm(rsxkit_format format, u8 *gcm, u32 *remap);
/* rsxkit_set_2d_state sobre una superficie de w × h (post.c). */
void rsxkit_set_2d_state_size(uint32_t w, uint32_t h);
/* Vacía la caché de programas generados: se generan y suben de nuevo. */
void rsxkit_fp_flush(void);
/* Programas generados (fpcache.c): el de una descripción, generado y
 * subido la primera vez; NULL si no es válida o no hay memoria. `hdr` y
 * `offset` son lo que pide rsxLoadFragmentProgramLocation. */
struct rsxkit_fp_cached {
	rsxkit_fp_desc desc;
	rsxFragmentProgram hdr;
	u32 offset;
};
const struct rsxkit_fp_cached *rsxkit_fp_lookup(const rsxkit_fp_desc *desc);
/* Cambia cada vez que la caché se vacía: lo que apuntaba a una entrada ya
 * no vale. */
uint32_t rsxkit_fp_generation(void);
void rsxkit_fp_count_load(void);
void rsxkit_fp_shutdown(void);
/* Dibuja la región [u0, u1] × [v0, v1] (coordenadas normalizadas) de una
 * textura lineal de la memoria local como un rectángulo en la pantalla
 * (fb.c). */
void rsxkit_blit(uint32_t offset, uint32_t pitch, uint32_t tex_w, uint32_t tex_h, float u0, float v0, float u1,
		 float v1, rsxkit_format format, const rsxkit_rect *r, rsxkit_filter filter);
/* Lo mismo sobre la superficie ya ligada, de surf_w × surf_h píxeles (no
 * liga la pantalla): rsxkit_target_read dibuja así en su superficie de
 * salida. rsxkit_blit_init carga los programas del blit (0 si están). */
void rsxkit_blit_to(uint32_t offset, uint8_t location, uint32_t pitch, uint32_t tex_w, uint32_t tex_h, float u0,
		    float v0, float u1, float v1, rsxkit_format format, const rsxkit_rect *r, uint32_t surf_w,
		    uint32_t surf_h, rsxkit_filter filter);
int rsxkit_blit_init(void);
/* Los blits siguientes (rsxkit_blit_to) escriben el alfa de la textura en vez
 * de 1, y las texturas ARGB8888 / RGBA5551 se leen con su alfa. Para llenar
 * o copiar entre destinos con alfa (target.c); 0 lo apaga. */
void rsxkit_blit_alpha(int on);

#endif
