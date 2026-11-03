/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — caja negra en la PS3 (docs/cajanegra.md): guarda en un anillo en
 * memoria la salida de la HLE gráfica de las últimas tareas y lo que se
 * presentó en cada campo (rdp/blackbox.h) y, con SELECT + L2, lo escribe
 * entero, comprimido, desde un hilo de baja prioridad (platform/ps3/bgjob.h
 * y bgio.h) a SALSA64_DATA_DIR/cajanegra-<campo>/caja.s4bb, para bajarlo por
 * FTP y repetirlo en la Mac (tools/cajanegra.py, salsa64-run --raster-dump).
 *
 * Apagada por omisión: se enciende creando SALSA64_DATA_DIR/cajanegra (su
 * contenido, si lo hay, es el tamaño del anillo en MB, 12 por omisión, y
 * opcionalmente un campo en que la caja se escribe sola, para las pruebas
 * automáticas: «12 150»). Apagada no cuesta nada ni reserva memoria.
 *
 * Todas las llamadas son del hilo de presentación (el de las tareas
 * traducidas y los campos presentados), salvo el trabajo de escritura.
 */
#ifndef SALSA64_FRONTEND_CAJANEGRA_H
#define SALSA64_FRONTEND_CAJANEGRA_H

#include <stdbool.h>
#include <stdint.h>

#include "core/bus.h"
#include "platform/ps3/paths.h"

#define CAJANEGRA_CFG SALSA64_DATA_DIR "/cajanegra"
#define CAJANEGRA_DIR SALSA64_DATA_DIR "/cajanegra-%u"
#define CAJANEGRA_FILE "caja.s4bb"

#define CAJANEGRA_MB_DEFAULT 12u
#define CAJANEGRA_MB_MIN 2u
#define CAJANEGRA_MB_MAX 48u

/* Lo que presentación sabe de un campo al presentarlo. */
struct cajanegra_field {
	uint32_t field;            /* n64.bus.vi.fields al terminar el campo */
	uint32_t tasks;            /* tareas gráficas terminadas hasta ahí */
	uint32_t flags;            /* BB_FLD_SHOWN, SKIPPED, RSX, RDRAM, BLANK */
	uint32_t vi_type, vi_origin, vi_width, vi_height, vi_stride, vi_bpp;
	const uint32_t *vi_regs;   /* SALSA64_VI_NREGS registros (NULL: ceros) */
	uint32_t behind;           /* campos que la emulación llevaba de ventaja */
};

/* Al abrir un juego: lee el archivo de configuración y, si está, reserva el
 * anillo y se engancha a las tareas. Devuelve si quedó encendida. */
bool cajanegra_init(const struct salsa64_bus *bus);
/* Al cerrar el juego: espera una escritura en curso (hasta wait_us), suelta
 * el anillo y el gancho. */
void cajanegra_shutdown(uint64_t wait_us);
bool cajanegra_enabled(void);
/* Bytes que tiene reservados (anillo y tablas de trabajo; 0 sin reserva). */
uint32_t cajanegra_reserved(void);

/* Presentación empieza a procesar este campo: las tareas que se traduzcan
 * ahora llevan su número. */
void cajanegra_set_field(uint32_t field);
void cajanegra_field(const struct cajanegra_field *f);
/* Para quien interpola cuadros (docs/cajanegra.md): lo que describe al campo
 * que se va a registrar a continuación (kind: BB_KIND_*, t en [0, 1), k
 * campos por cuadro del juego, src_task/prev_task: la tarea que dibujó lo
 * presentado y la anterior en el emparejado, o 0xFFFFFFFF). Vale para un solo
 * cajanegra_field. */
void cajanegra_field_interp(uint32_t kind, float t, uint32_t k, uint32_t src_task, uint32_t prev_task);
/* Una nota con una etiqueta de 4 letras (BB_FOURCC) y bytes opacos. */
void cajanegra_note(uint32_t tag, const void *data, uint32_t len);

/* SELECT + L2: pide la escritura del anillo; la hace cajanegra_pump. Con la
 * caja apagada o ocupada, avisa en pantalla. */
void cajanegra_request(void);
/* Una vez por vuelta de presentación: arranca la escritura pedida y recoge
 * el resultado de la anterior. */
void cajanegra_pump(uint64_t now_us);
/* Para un cierre forzado (el RSX colgado): escribe el anillo entero ya, sin
 * comprimir y en el hilo que llama, a `path` (por un .tmp que se renombra).
 * Si hay una escritura en segundo plano, la espera hasta `wait_us` (si no
 * termina, no escribe). Se puede llamar desde el gancho de las esperas al
 * RSX o desde el latido con presentación quieta: nadie más toca el anillo.
 * Devuelve false si la caja está apagada o algo falló (`err`, si no es
 * NULL, dice qué). */
bool cajanegra_write_now(const char *path, uint64_t wait_us, char *err, unsigned err_size);

#endif
