/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — audio del frontend en la PS3. Ver audio_out.h.
 *
 * El hilo de audio no escribe al log (salsa64_log no es seguro entre
 * hilos): deja contadores que audio_out_field lleva al log desde el hilo
 * principal.
 */
#include "audio_out.h"

#include <audio/audio.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event_queue.h>
#include <sys/systime.h>
#include <sys/thread.h>

#include <spukit/spukit.h>

#include "platform/ps3/audio_spu.h"
#include "platform/ps3/log.h"
#include "platform/ps3/paths.h"
#include "rsp/audio/backend.h"

#define CFG_PATH  SALSA64_DATA_DIR "/audio.cfg"
#define WAV_PATH  SALSA64_DATA_DIR "/audio.wav"
#define DUMP_MAX_S 30
#define CFG_MAX   1024
#define LEAD      4u        /* bloques llenos por delante del que suena */
#define STATS_US  5000000u
#define THREAD_PRIO 100     /* más urgente que el hilo principal (1001) */

static struct audio_sync sync_out __attribute__((aligned(128)));
static struct salsa64_audio_spu hle_spu;
static struct salsa64_audio_sync hle_ppe;
static struct aud_state hle_ppe_state;
static struct salsa64_n64 *machine;
static enum { HLE_NONE, HLE_SPE, HLE_PPE } hle;

static struct {
	bool open;
	u32 port;
	audioPortConfig config;
	sys_event_queue_t queue;
	sys_ipc_key_t key;
	sys_ppu_thread_t thread;
	volatile int quit;
	volatile uint64_t blocks;    /* bloques llenados */
	volatile uint64_t late;      /* veces que el puerto alcanzó al hilo */
	volatile uint64_t busy_us;   /* tiempo del hilo trabajando */
} port;

/* Volcado opcional de lo que sale por el AI (diagnóstico; audio.cfg). Lo
 * llena el sumidero del AI y lo escribe audio_out_field. */
static struct {
	uint8_t *buf;
	uint32_t cap, len, rate;
	volatile uint32_t full;
	bool written;
} dump;

/* Pico de lo que entrega el AI desde la última estadística (0–32768): dice
 * si un juego que «no suena» manda silencio o audio. */
static volatile uint32_t ai_peak;

/* Para las estadísticas cada STATS_US. */
static struct {
	uint64_t t;
	uint32_t tasks, unknown_abi, spu_done, commands, hle_tasks;
	uint64_t spu_ticks, wait_us, blocks, late, busy_us;
	struct audio_sync_stats out;
} prev;

/* ---- Configuración -------------------------------------------------------- */

struct audio_cfg {
	enum audio_policy policy;
	uint32_t level_ms;
	int hle;      /* HLE_* */
	bool output;
	uint32_t dump_s;
	uint32_t task_us;  /* 0: la duración por omisión del núcleo */
};

static void read_cfg(struct audio_cfg *c)
{
	static char text[CFG_MAX + 1];
	c->policy = AUDIO_STRETCH;
	c->level_ms = 60;
	c->hle = HLE_SPE;
	c->output = true;
	c->dump_s = 0;
	c->task_us = 0;
	FILE *f = fopen(CFG_PATH, "r");
	if (!f)
		return;
	size_t len = fread(text, 1, CFG_MAX, f);
	fclose(f);
	text[len] = '\0';
	for (char *line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
		char key[32], val[32];
		if (line[0] == '#' || sscanf(line, " %31[^= ] = %31s", key, val) != 2)
			continue;
		if (!strcmp(key, "politica"))
			c->policy = !strcmp(val, "silencio") ? AUDIO_SILENCE : AUDIO_STRETCH;
		else if (!strcmp(key, "nivel_ms"))
			sscanf(val, "%u", &c->level_ms);
		else if (!strcmp(key, "hle"))
			c->hle = !strcmp(val, "ppe") ? HLE_PPE : !strcmp(val, "no") ? HLE_NONE : HLE_SPE;
		else if (!strcmp(key, "salida"))
			c->output = strcmp(val, "no") != 0;
		else if (!strcmp(key, "volcado_s"))
			sscanf(val, "%u", &c->dump_s);
		else if (!strcmp(key, "tarea_us"))
			sscanf(val, "%u", &c->task_us);
	}
	salsa64_log("audio: %s leído", CFG_PATH);
}

/* ---- Salida ------------------------------------------------------------------ */

static void ai_push(void *ctx, const uint8_t *p, uint32_t bytes, uint32_t rate, uint64_t cycle)
{
	(void)cycle;
	audio_sync_push(ctx, p, bytes, rate);
	uint32_t peak = ai_peak;
	for (uint32_t i = 0; i + 1 < bytes; i += 2) {
		int32_t v = (int16_t)(p[i] << 8 | p[i + 1]);
		uint32_t a = (uint32_t)(v < 0 ? -v : v);
		if (a > peak)
			peak = a;
	}
	ai_peak = peak;
	if (dump.buf && !dump.full) {
		uint32_t n = bytes < dump.cap - dump.len ? bytes : dump.cap - dump.len;
		memcpy(dump.buf + dump.len, p, n);
		dump.len += n;
		dump.rate = rate;
		if (dump.len == dump.cap)
			__atomic_store_n(&dump.full, 1, __ATOMIC_RELEASE);
	}
}

static void write_dump(void)
{
	FILE *f = fopen(WAV_PATH, "wb");
	if (!f) {
		salsa64_log("audio: no se pudo escribir %s", WAV_PATH);
		return;
	}
	uint32_t data = dump.len, rate = dump.rate;
	uint8_t h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
			  16, 0, 0, 0, 1, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a' };
	for (int i = 0; i < 4; i++) {
		h[4 + i] = (uint8_t)((36 + data) >> (8 * i));
		h[24 + i] = (uint8_t)(rate >> (8 * i));
		h[28 + i] = (uint8_t)((rate * 4) >> (8 * i));
		h[40 + i] = (uint8_t)(data >> (8 * i));
	}
	/* WAV es little-endian: se invierte cada muestra en su lugar. */
	for (uint32_t i = 0; i + 1 < data; i += 2) {
		uint8_t t = dump.buf[i];
		dump.buf[i] = dump.buf[i + 1];
		dump.buf[i + 1] = t;
	}
	bool ok = fwrite(h, 1, sizeof h, f) == sizeof h && fwrite(dump.buf, 1, data, f) == data;
	ok &= fclose(f) == 0;
	salsa64_log("audio: volcado de %.1f s del AI (%u Hz) en %s%s", data / 4.0 / (rate ? rate : 1), rate,
		    WAV_PATH, ok ? "" : ": FALLÓ");
}

static float *block_ptr(uint32_t b)
{
	return (float *)(uintptr_t)port.config.audioDataStart + (size_t)b * 2 * AUDIO_BLOCK_SAMPLES;
}

/* Despierta con cada bloque que consume el puerto y llena los que faltan
 * hasta LEAD por delante del que suena. El índice de lectura del puerto
 * es el número de bloque (módulo numBlocks): se lleva una cuenta absoluta. */
static void audio_thread(void *arg)
{
	(void)arg;
	const uint32_t n = (uint32_t)port.config.numBlocks;
	uint64_t rabs = 0, next = 1;
	uint32_t last = 0;
	while (!port.quit) {
		sys_event_t ev;
		sysEventQueueReceive(port.queue, &ev, 100000);
		uint64_t t0 = sysGetSystemTime();
		uint32_t rmod = (uint32_t)(*(volatile uint64_t *)(uintptr_t)port.config.readIndex % n);
		rabs += (rmod + n - last) % n;
		last = rmod;
		if (next <= rabs) {
			port.late++;
			next = rabs + 1;
		}
		while (next <= rabs + LEAD) {
			audio_sync_pull(&sync_out, block_ptr((uint32_t)(next % n)), AUDIO_BLOCK_SAMPLES, sysGetSystemTime());
			next++;
			port.blocks++;
		}
		port.busy_us += sysGetSystemTime() - t0;
	}
	sysThreadExit(0);
}

static int open_port(void)
{
	audioPortParam params = { AUDIO_PORT_2CH, AUDIO_BLOCK_16, AUDIO_PORT_INITLEVEL, 1.0f };
	int rc;
	if ((rc = audioInit()) != 0) {
		salsa64_log("audio: audioInit falló (%d)", rc);
		return rc;
	}
	if ((rc = audioPortOpen(&params, &port.port)) != 0 ||
	    (rc = audioGetPortConfig(port.port, &port.config)) != 0) {
		salsa64_log("audio: no se abrió el puerto (%d)", rc);
		audioQuit();
		return rc;
	}
	if ((rc = audioCreateNotifyEventQueue(&port.queue, &port.key)) != 0 ||
	    (rc = audioSetNotifyEventQueue(port.key)) != 0) {
		salsa64_log("audio: sin cola de eventos (%d)", rc);
		audioPortClose(port.port);
		audioQuit();
		return rc;
	}
	sysEventQueueDrain(port.queue);
	memset(block_ptr(0), 0, (size_t)port.config.numBlocks * 2 * AUDIO_BLOCK_SAMPLES * sizeof(float));
	port.quit = 0;
	port.blocks = port.late = port.busy_us = 0;  /* desde el menú se abre una vez por juego */
	if ((rc = audioPortStart(port.port)) != 0 ||
	    (rc = sysThreadCreate(&port.thread, audio_thread, NULL, THREAD_PRIO, 0x10000, THREAD_JOINABLE,
				  "salsa64_audio")) != 0) {
		salsa64_log("audio: no arrancó la salida (%d)", rc);
		audioPortStop(port.port);
		audioRemoveNotifyEventQueue(port.key);
		audioPortClose(port.port);
		sysEventQueueDestroy(port.queue, 0);
		audioQuit();
		return rc;
	}
	port.open = true;
	salsa64_log("audio: puerto %u abierto: %u canales, %u bloques de %u muestras a %u Hz", port.port,
		    (unsigned)port.config.channelCount, (unsigned)port.config.numBlocks, AUDIO_BLOCK_SAMPLES,
		    AUDIO_OUT_RATE);
	return 0;
}

/* ---- API --------------------------------------------------------------------- */

int audio_out_init(struct salsa64_n64 *n64, uint32_t job_flags)
{
	struct audio_cfg cfg;
	machine = n64;
	read_cfg(&cfg);
	audio_sync_init(&sync_out, cfg.policy, cfg.level_ms);

	hle = cfg.hle;
	if (hle == HLE_SPE) {
		int rc;
		spukit_init(6);
		if ((rc = salsa64_audio_spu_init(&hle_spu, &n64->bus)) != 0) {
			salsa64_log("audio: no arrancó el SPE de la HLE (%d): uso el PPE", rc);
			hle = HLE_PPE;
		}
		hle_spu.job_flags = job_flags;
	}
	if (hle == HLE_PPE) {
		salsa64_audio_sync_init(&hle_ppe, &hle_ppe_state, &n64->bus);
		hle_ppe.job_flags = job_flags;
	}
	salsa64_rsp_set_audio(&n64->bus, hle == HLE_SPE ? &hle_spu.backend : hle == HLE_PPE ? &hle_ppe.backend : NULL);
	dump.len = dump.rate = 0;
	dump.full = 0;
	dump.written = false;
	ai_peak = 0;
	if (cfg.dump_s) {
		/* Se reserva al arrancar, no en el bucle (y una sola vez aunque se
		 * abran varios juegos desde el menú). 50 kHz como máximo. */
		uint32_t s = cfg.dump_s > DUMP_MAX_S ? DUMP_MAX_S : cfg.dump_s;
		if (dump.buf && dump.cap < s * 50000u * 4u) {
			free(dump.buf);
			dump.buf = NULL;
		}
		dump.cap = s * 50000u * 4u;
		if (!dump.buf)
			dump.buf = memalign(128, dump.cap);
		salsa64_log("audio: volcado de los primeros %u s del AI en %s%s", s, WAV_PATH,
			    dump.buf ? "" : ": sin memoria");
	} else if (dump.buf) {
		free(dump.buf);
		dump.buf = NULL;
	}
	salsa64_ai_set_sink(&n64->bus, ai_push, &sync_out);
	if (cfg.task_us)
		n64->bus.rsp.audio_cycles = (uint32_t)(cfg.task_us * 93.75);
	salsa64_log("audio: HLE en %s, política %s, colchón %u ms, tarea de %.0f µs de la N64",
		    hle == HLE_SPE ? "el SPE" : hle == HLE_PPE ? "el PPE" : "ninguna parte (sin HLE)",
		    cfg.policy == AUDIO_SILENCE ? "silencio" : "estirar", cfg.level_ms,
		    n64->bus.rsp.audio_cycles / 93.75);
	memset(&prev, 0, sizeof prev);
	return cfg.output ? open_port() : -1;
}

void audio_out_flush(void)
{
	audio_sync_flush(&sync_out);
}

void audio_out_pause(bool on)
{
	audio_sync_pause(&sync_out, on);
}

const struct audio_sync_stats *audio_out_stats(void)
{
	return &sync_out.stats;
}

uint64_t audio_out_blocks(void)
{
	return port.blocks;
}

uint64_t audio_out_ai_frames(void)
{
	return __atomic_load_n(&sync_out.wpos, __ATOMIC_ACQUIRE);
}

void audio_out_field(uint64_t now_us)
{
	if (dump.buf && !dump.written && __atomic_load_n(&dump.full, __ATOMIC_ACQUIRE)) {
		dump.written = true;
		write_dump();
	}
	if (!prev.t) {
		prev.t = now_us;
		return;
	}
	uint64_t dt = now_us - prev.t;
	if (dt < STATS_US)
		return;
	const struct salsa64_rsp_stats *rs = &machine->bus.rsp.stats;
	uint32_t tasks = rs->audio - prev.tasks;
	if (tasks) {
		char where[160] = "";
		if (hle == HLE_SPE) {
			uint32_t done = hle_spu.stats.tasks;
			uint32_t n = done - prev.spu_done ? done - prev.spu_done : 1;
			prev.spu_done = done;
			snprintf(where, sizeof where, "; en el SPE %.3f ms por tarea (máx. %.3f), PPE esperando %.3f ms",
				 (hle_spu.spu_ticks - prev.spu_ticks) * 1e3 / spukit_tick_hz() / n,
				 hle_spu.spu_max * 1e3 / spukit_tick_hz(), (hle_spu.wait_us - prev.wait_us) / 1e3 / n);
			prev.spu_ticks = hle_spu.spu_ticks;
			prev.wait_us = hle_spu.wait_us;
		}
		const struct salsa64_audio_stats *as = hle == HLE_SPE ? &hle_spu.stats : &hle_ppe.stats;
		/* Comandos por tarea: si un juego deja de mandar las voces de su
		 * música (Mario Kart sin música de fondo, con los efectos), baja. */
		uint32_t hn = as->tasks - prev.hle_tasks;
		salsa64_log("audio: %u tareas (%.1f/s), ABI %s, %u con ABI desconocido; %.0f comandos por tarea, %u sin "
			    "implementar (último 0x%08X), %u errores%s", tasks, tasks * 1e6 / dt,
			    salsa64_abi_name(machine->bus.rsp.abi), rs->unknown_abi - prev.unknown_abi,
			    hn ? (double)(as->commands - prev.commands) / hn : 0.0, as->unknown, as->last_unknown, as->errors,
			    where);
		prev.commands = as->commands;
		prev.hle_tasks = as->tasks;
	}
	prev.tasks = rs->audio;
	prev.unknown_abi = rs->unknown_abi;

	const struct audio_sync_stats *o = &sync_out.stats;
	uint64_t out = o->out_frames - prev.out.out_frames;
	if (port.open && out) {
		salsa64_log("audio: salida %.0f ms de colchón, emulación al %.0f %%, %s; en 5 s: %.0f %% en silencio, "
			    "%.0f %% estirado, %u faltas, %u silencios, %u saltos, %u descartadas, %u pausas, %u bloques "
			    "tarde; hilo %.2f %% del tiempo; del AI %u Hz, pico %.0f dBFS",
			    audio_sync_level_ms(&sync_out), sync_out.speed_est * 100.0,
			    sync_out.stretching ? "estirando" : "normal",
			    100.0 * (o->silent_frames - prev.out.silent_frames) / out,
			    100.0 * (o->stretch_frames - prev.out.stretch_frames) / out,
			    o->underruns - prev.out.underruns, o->silences - prev.out.silences,
			    o->skips - prev.out.skips, o->dropped - prev.out.dropped, o->pauses - prev.out.pauses,
			    (unsigned)(port.late - prev.late), 100.0 * (port.busy_us - prev.busy_us) / dt,
			    sync_out.rate, ai_peak ? 20.0 * log10(ai_peak / 32768.0) : -99.0);
		ai_peak = 0;
	}
	prev.out = *o;
	prev.late = port.late;
	prev.busy_us = port.busy_us;
	prev.blocks = port.blocks;
	prev.t = now_us;
}

void audio_out_shutdown(void)
{
	if (!machine)
		return;
	salsa64_ai_set_sink(&machine->bus, NULL, NULL);
	salsa64_rsp_set_audio(&machine->bus, NULL);  /* espera la tarea en vuelo */
	if (hle == HLE_SPE)
		salsa64_log("audio: SPE de la HLE detenido (salida %d)", salsa64_audio_spu_shutdown(&hle_spu));
	if (dump.buf && !dump.written && dump.len) {
		dump.written = true;
		write_dump();  /* lo que alcanzó a juntar */
	}
	if (port.open) {
		/* Orden de PSL1GHT (audio/audio.h), con el hilo ya terminado. Cada
		 * resultado va al log: un recurso del sistema que quede enganchado
		 * al proceso puede colgar la consola al salir. */
		u64 rv = 0;
		port.quit = 1;
		int rj = sysThreadJoin(port.thread, &rv);
		int rs = audioPortStop(port.port);
		int rr = audioRemoveNotifyEventQueue(port.key);
		int rc = audioPortClose(port.port);
		int rd = sysEventQueueDestroy(port.queue, 0);
		if (rd)
			rd = sysEventQueueDestroy(port.queue, 1) ? rd : 0;  /* 1: forzar */
		int rq = audioQuit();
		port.open = false;
		salsa64_log("audio: cierre del puerto: join %d, stop %d, cola fuera %d, close %d, cola destruida %d, "
			    "quit %d", rj, rs, rr, rc, rd, rq);
	}
	salsa64_log("audio: %llu muestras del AI, %llu a la salida (%llu en silencio), %llu bloques",
		    (unsigned long long)sync_out.stats.in_frames, (unsigned long long)sync_out.stats.out_frames,
		    (unsigned long long)sync_out.stats.silent_frames, (unsigned long long)port.blocks);
	machine = NULL;
}
