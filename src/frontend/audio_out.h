/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — audio del frontend en la PS3: la HLE de las tareas de audio (en
 * el SPE 1) y la salida de lo que produce el AI por el puerto de audio del
 * sistema (48 kHz estéreo) desde un hilo propio del PPE.
 *
 *   AI (hilo de la emulación) → audio_sync (anillo, estirador, remuestreo)
 *   → hilo de audio: llena los bloques del puerto (256 muestras, 5,3 ms)
 *     cuatro por delante de los que suenan, despertado por la cola de
 *     eventos del puerto; nunca espera a la emulación.
 *
 * Configuración en SALSA64_DATA_DIR/audio.cfg (opcional), una clave por
 * línea:
 *   politica = estirar | silencio   qué hacer si la emulación va lenta
 *                                   (audio_sync.h; por omisión estirar)
 *   nivel_ms = 60                   colchón mínimo del anillo
 *   hle = spe | ppe | no            dónde corre la HLE de audio (spe)
 *   salida = si | no                abrir el puerto de audio (si)
 *   tarea_us = N                    duración de una tarea de audio en
 *                                   tiempo de la N64 (rsp.c: 85 µs); si el
 *                                   SPE tarda más, el PPE lo espera
 *   volcado_s = 0                   diagnóstico: guardar los primeros N s
 *                                   que salen por el AI (hasta 30) en
 *                                   SALSA64_DATA_DIR/audio.wav. Con juegos
 *                                   comerciales es audio del juego: no va
 *                                   al repositorio.
 */
#ifndef SALSA64_FRONTEND_AUDIO_OUT_H
#define SALSA64_FRONTEND_AUDIO_OUT_H

#include "audio_sync.h"
#include "core/n64.h"
#include "rsp/audio/audio.h"

/* Arranca la HLE de audio y la salida y conecta el AI. Devuelve 0 si la
 * salida quedó andando (sin ella, la HLE y el AI siguen igual). `job_flags`:
 * AUD_JOB_* (audio.h) que llevan todas las tareas. */
int audio_out_init(struct salsa64_n64 *n64, uint32_t job_flags);
/* Una vez por campo, desde el hilo principal: estadísticas al log cada 5 s. */
void audio_out_field(uint64_t now_us);
void audio_out_shutdown(void);
/* Lo que espera en la cola de salida ya no suena (estado guardado cargado).
 * Desde el hilo de emulación o con él quieto. */
void audio_out_flush(void);
/* Pausa del juego (menú del sistema, cuadro de salir): la salida calla sin
 * perder lo que estaba en cola y sigue al volver (audio_sync_pause). */
void audio_out_pause(bool on);
/* Para las autopruebas: lo que pasó por la salida. */
const struct audio_sync_stats *audio_out_stats(void);
/* Bloques del puerto llenados por el hilo de audio. */
uint64_t audio_out_blocks(void);
/* Muestras que entregó el AI hasta ahora (al día, del lado del productor). */
uint64_t audio_out_ai_frames(void);

#endif
