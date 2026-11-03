/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — estados guardados en la PS3: los archivos de las ranuras y la
 * miniatura (el formato y lo que entra en él: core/state.h).
 *
 * Archivos: SALSA64_DATA_DIR/states/<título>-<CRC1>.st0 … .st9, el mismo
 * nombre base que las partidas de saves/. Se escriben en un .tmp y se
 * renombran (un corte a mitad no pisa el estado anterior). La E/S va con
 * las llamadas al kernel (sysLv2Fs*, <sys/file.h>): la RDRAM (8 MB) sale y
 * entra de su búfer sin copias. No con sysFs* de libsysfs: esos son puentes
 * al módulo cellFs del sistema, que hay que cargar antes; en RPCS3 andan
 * igual, pero en la PS3 real colgaban la consola entera al primer mkdir.
 *
 * Todo con la emulación quieta (main.c: el hilo de emulación espera en el
 * límite del campo con la tarea del RSP recogida) y desde el hilo de
 * presentación.
 *
 * Guardar es en dos tiempos (docs/estados.md): con la emulación quieta se
 * toma una foto (las secciones chicas y una copia de la RDRAM a búferes
 * preasignados: unos milisegundos) y la emulación sigue; la RDRAM se
 * comprime en un SPE libre (lib/spukit) y el archivo se escribe desde un
 * hilo del PPE de baja prioridad (platform/ps3/bgjob.h). El resultado se
 * recoge con states_poll. Cargar y mirar las ranuras esperan a que termine
 * la escritura en curso. Sin búferes o sin hilo, todo ocurre ahí mismo
 * (como antes, pero con la RDRAM comprimida si hay con qué).
 */
#ifndef SALSA64_FRONTEND_STATES_H
#define SALSA64_FRONTEND_STATES_H

#include <rsxkit/rsxkit.h>

#include "core/state.h"
#include "platform/ps3/paths.h"

#define STATES_DIR SALSA64_DATA_DIR "/states"

/* Tiempos de la última operación (µs) y su tamaño. */
struct states_timing {
	uint64_t us_total;  /* lo que detuvo la emulación: la foto al guardar, todo al cargar */
	uint64_t us_io;     /* de eso, leyendo o escribiendo el disco */
	uint32_t bytes;
	bool async;         /* guardar: el archivo se escribe en segundo plano (states_poll) */
};

/* Cómo terminó una escritura en segundo plano. */
struct states_done {
	bool ok;
	unsigned slot;
	char err[160];
	uint64_t us_total;       /* del pedido al archivo renombrado */
	uint64_t us_io;          /* de eso, en el disco (escribir, cerrar, renombrar) */
	uint64_t us_spu;         /* comprimiendo en el SPE (suma de sus trabajos) */
	uint32_t bytes;          /* tamaño del archivo */
	uint32_t raw_bytes;      /* lo que habría pesado sin comprimir */
	uint32_t blocks, stored; /* bloques de la RDRAM y los que fueron sin comprimir */
	uint32_t spu_blocks;     /* los comprimidos por el SPE */
	bool spu;                /* hay un SPE comprimiendo */
};

/* Reserva la foto (RDRAM + secciones) y arranca el hilo de escritura.
 * Una vez por juego, fuera del bucle de emulación. false si no hubo memoria:
 * guardar sigue funcionando, todo seguido y sin comprimir. */
bool states_init(uint32_t rdram_size);
/* Comparación: conservar la foto al cerrar (por omisión se devuelve). */
void states_keep_snapshot(bool keep);
/* Bytes de la foto reservada (0 si no se reservó). */
uint32_t states_reserved(void);
/* Espera la escritura en curso (hasta wait_us) y suelta el hilo, el SPE y
 * la foto. Si vence el plazo, conserva la foto mientras el escritor la usa. */
void states_shutdown(uint64_t wait_us);
/* ¿Hay una escritura en segundo plano en marcha? */
bool states_busy(void);
/* Si una escritura terminó desde la última llamada, la describe y devuelve
 * true (una vez por escritura). Del hilo de presentación. */
bool states_poll(struct states_done *d);

/* ¿Hay un estado en la ranura para la ROM del bus? */
bool states_slot_exists(const struct salsa64_bus *bus, unsigned slot);

/* Guarda la máquina en la ranura, con la miniatura capturada si la hay. */
bool states_save(struct salsa64_n64 *n64, unsigned slot, struct states_timing *t, char *err, size_t err_size);

/* Carga la ranura sobre la máquina en marcha. *broken: falló a mitad de la
 * RDRAM y la máquina quedó inservible. */
bool states_load(struct salsa64_n64 *n64, unsigned slot, struct states_timing *t, char *err, size_t err_size,
		 bool *broken);

/* Miniatura de la imagen recién dibujada (después de rsxkit_finish): una
 * muestra del búfer del RSX por píxel de 160 × 120, dentro de `r` (donde
 * quedó la imagen de la N64). La guarda el próximo states_save. */
void states_thumb_capture(const rsxkit_rect *r);
void states_thumb_clear(void);
/* Lo que tardó la última captura (µs; 0 si no hubo). */
uint64_t states_thumb_us(void);

#endif
