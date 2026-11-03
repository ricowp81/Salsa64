/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — el juego se quedó esperando (frontend/cuelgue.c).
 *
 * El latido de main.c vigila los hilos del emulador, no al juego. Un juego
 * que se queda esperando una interrupción o un mensaje que no llega (Killer
 * Instinct Gold al empezar una pelea) deja al emulador andando, el audio
 * sonando y la última imagen quieta, y el log sin una sola línea. Esto lo
 * detecta: el juego dejó de leer los controles y de mandar tareas gráficas
 * y listas al RDP mientras el VI sigue contando campos. Entonces escribe al
 * log el estado del RCP y del CPU, y a SALSA64_DATA_DIR un volcado de la
 * RDRAM (cuelgue-<campo>.rdram, que tools/libultra-threads.py lee: en qué
 * espera cada hilo de libultra) y otro de texto con todos los registros
 * (cuelgue-<campo>.txt).
 *
 * Solo lee el estado de la máquina (desde el hilo del latido, sin pararla): lo
 * que ve puede estar a medio cambiar, pero un juego colgado no cambia.
 */
#ifndef SALSA64_FRONTEND_CUELGUE_H
#define SALSA64_FRONTEND_CUELGUE_H

#include <stdbool.h>
#include <stdint.h>

#include "core/n64.h"

/* Segundos sin actividad del juego para dar el aviso, y volcados por juego. */
#define CUELGUE_US      6000000u
#define CUELGUE_DUMPS   3

struct cuelgue {
	uint64_t t_change;     /* última vez (µs) que el juego hizo algo */
	uint32_t reads, work, fields;
	uint32_t active;       /* vueltas del latido con actividad (hasta 16) */
	uint32_t dumps;        /* volcados hechos en este juego */
	bool said;             /* el aviso de este episodio ya salió */
	bool init;
	bool always;           /* avisar también de un juego que nunca dio señales de vida (latido-siempre) */
};

/* Al empezar un juego. `always`: avisar y volcar aunque el juego nunca haya
 * leído los controles ni dibujado (un juego que se queda en su arranque). */
void cuelgue_reset(struct cuelgue *c, bool always);

/* Cada vuelta del latido. `expect`: el juego corre (no está en pausa ni
 * parado). `dir`: dónde escribir los volcados (sin barra final) o NULL
 * para solo avisar. `log` escribe una línea al log. Devuelve true si dio el
 * aviso en esta vuelta. */
bool cuelgue_watch(struct cuelgue *c, const struct salsa64_n64 *n64, uint64_t now_us, bool expect,
		   const char *dir, void (*log)(const char *fmt, ...));

/* El estado del RCP y del CPU como texto, para el log y el archivo (también
 * lo prueba tests/unit). Devuelve cuántos bytes escribió en `out`. */
unsigned cuelgue_state_text(const struct salsa64_n64 *n64, char *out, unsigned size);

#endif
