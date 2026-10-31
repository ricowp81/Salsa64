/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — ejecutor de línea de comandos para el host, para pruebas
 * automatizadas (tools/test-roms.sh). No depende de PSL1GHT.
 *
 *   salsa64-run [opciones] rom.z64|rom.v64|rom.n64
 *
 * Carga la ROM, la arranca por HLE (core/boot.c) y corre el intérprete en
 * bloques de ciclos hasta que se cumple una condición. Lo que la ROM imprime
 * por el ISViewer sale por stdout; el resumen, por stderr.
 *
 * Toda la memoria se reserva antes de empezar a emular: la RDRAM y el búfer
 * del ISViewer son estáticos y la ROM se lee una vez con malloc.
 *
 * Con --frame, al terminar se escribe en PPM la imagen que muestra el VI.
 * Con --no-idle no se saltan los bucles de espera (cpu/idle.c): el
 * resultado debe ser idéntico, solo más lento.
 *
 * Guardado del cartucho (core/save.c): el tipo sale de la cabecera o de la
 * tabla por código de juego, o de --save. Con --save-dir se lee al empezar
 * y se escribe al terminar en ese directorio, con el mismo nombre que en la
 * PS3 (frontend/savefile.c); sin la opción queda solo en memoria.
 *
 * Con --rec corre el recompilador (solo en un host ppc64 big-endian: qemu,
 * tools/test-rec.sh). --lockstep además corre un segundo N64 con el
 * intérprete y, después de cada bloque recompilado, lo lleva al mismo ciclo
 * y compara el CPU (y cada tanto la RDRAM): la primera diferencia termina
 * con código 4 y el detalle.
 * Las tareas gráficas del RSP pasan por la HLE (src/rsp/gfx) en el mismo
 * hilo; al final se resume lo que produjo. Con --gfx-frame, la salida de la
 * HLE se dibuja con el rasterizador de referencia (raster.c) y se guarda la
 * imagen de la color image que muestra el VI; --gfxtest-check compara esa
 * imagen con los puntos de la ROM gfxtest (tests/roms/src/gfxtest/expect.h).
 *
 * Audio: las tareas de audio pasan por la HLE (src/rsp/audio) en el mismo
 * hilo y lo que sale por el AI se guarda; con --audio-wav se escribe en un
 * WAV y --audiotest-check lo compara muestra por muestra con el tono de la
 * ROM audiotest (tests/roms/src/audiotest/tone.h) y exige que no haya
 * huecos entre los DMA del AI.
 *
 * Controles programados (--pad): "CAMPO:BOTONES[@X,Y];…" (o separados por
 * ',') pone el control 1 al empezar ese campo del VI (A, B, Z, START,
 * DU/DD/DL/DR, L, R, CU/CD/CL/CR unidos con +; el stick como @X,Y o
 * x<n>/y<n>; "-" suelta todo) y el estado vale hasta la entrada siguiente.
 * Sirve para llegar a una escena sin manos (menús de un juego) y es
 * determinista: con --lockstep la referencia recibe lo mismo en el mismo
 * ciclo. --trace-from CAMPO retrasa las trazas (--trace-exc,
 * --trace-faults, --trace-rsp, --trace-gfx) hasta ese campo.
 * --poke "CAMPO:DIR=VALOR;…" escribe la RDRAM al empezar ese campo (8, 16
 * o 32 bits según los dígitos hexadecimales de VALOR: 0x04, 0x0400,
 * 0x00000400): para forzar una escena (p. ej. la energía de un personaje)
 * sin llegar jugando. No se repite en la referencia de --lockstep.
 *
 * Depuración de la HLE gráfica (compilar con EXTRA_CFLAGS=-DSALSA64_GFX_TRACE;
 * actúa solo en las tareas que traza --trace-gfx con SALSA64_GFX_VERBOSE):
 * por cada dibujo se imprime el estado del RDP con que se armó (combiner,
 * othermode, tiles, luces, términos k) y el rango de s/t; SALSA64_GFX_VDUMP=n
 * lista los vértices del dibujo n, SALSA64_GFX_TEXDUMP=DIR escribe las
 * texturas de cada dibujo en PPM (color y, debajo, el alfa), y en el
 * rasterizador (--gfx-frame) SALSA64_GFX_MARK=n[,m…] pinta esos dibujos de
 * magenta y SALSA64_GFX_PIX=x,y muestra qué dibujos escriben ese píxel.
 * SALSA64_TEXSTAT=N (en todas las tareas) resume cada N campos las texturas
 * pedidas y decodificadas por tarea y por qué no salieron de la caché.
 *
 * Sincronía de la HLE gráfica (sin -D): SALSA64_GFX_LATE=N la retrasa al
 * fin de la tarea, tras N sondeos del RSP, como un SPE lento mientras el CPU
 * sigue; con SALSA64_GFX_TEAR además la corre al arrancar y avisa si la
 * salida cambió entre las dos (el CPU tocó durante la tarea algo que la
 * lista lee); SALSA64_GFX_TEAR_V lista los vértices distintos.
 * SALSA64_DPC_ESTADO_APARTE=1 hace que las listas del RDP que escribe el CPU
 * (DPC_START/DPC_END) las consuma otra HLE con su propio estado y su propio
 * directorio de texturas sobre la misma caché, como la PS3 hizo con la HLE en el
 * SPE y el RDP en el PPE: Killer Instinct Gold sale con texturas rotas.
 * SALSA64_SIN_DPC=1 las deja sin consumidor (el juego no manda ninguna tarea
 * gráfica). SALSA64_JITTER=N hace al azar la duración de cada tarea del RSP
 * (audio, gráficas y la interrupción del DP) para buscar esperas eternas.
 * SALSA64_PS3_RB (con --gfx-frame) cuenta las color images que la regla de
 * copias a la RDRAM de la PS3 (readbacks_wanted, src/rdp/rdp.c) pediría.
 * SALSA64_GFX_CUTCHECK mide, por tarea, cuánto se separan los vértices que
 * la HLE puso sobre el plano cercano entre triángulos vecinos (una grieta
 * en una arista compartida sería ≥ 1 px).
 *
 * FlashRAM (compilar con EXTRA_CFLAGS=-DSALSA64_DEBUG_FLASH): desde
 * --trace-from, cada comando, acceso de E/S y DMA del chip con su modo (las
 * primeras 4000 líneas, a stderr).
 *
 * Estados guardados (core/state.c): --state-save CAMPO:ARCHIVO guarda la
 * máquina al terminar ese campo del VI (hasta 8; va campo por campo),
 * --state-load ARCHIVO la carga después del arranque y sigue desde ahí
 * (con --lockstep, también la referencia; con --lle, el estado guardado con
 * la HLE sigue con el microcódigo real), y --fields N termina al acabar
 * el campo N. Es el mismo formato que en la PS3: un estado de la consola
 * se reproduce aquí. La prueba de determinismo (tools/test-roms.sh) corre
 * de corrido hasta M y, aparte, guarda en N, carga y sigue hasta M: los dos
 * estados en M deben ser iguales byte por byte.
 *
 * Códigos de salida:
 *   0  apareció el texto de --until-text, o se agotaron los ciclos sin pedirlo
 *   1  apareció alguno de los textos de --fail-text
 *   2  se agotaron los ciclos esperando --until-text, o el CPU quedó en un
 *      bucle infinito sin interrupciones posibles
 *   3  error de argumentos o de carga
 *   4  --lockstep encontró una diferencia con el intérprete
 */
#define _GNU_SOURCE
#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../tests/roms/src/audiotest/tone.h"
#include "../../tests/roms/src/gfxtest/expect.h"
#include "ares.h"
#include "core/boot.h"
#include "core/endian.h"
#include "core/gamefix.h"
#include "core/rompg.h"
#include "core/state.h"
#include "rdp/blackbox.h"
#include "cpu/rec.h"
#include "host/rec_host.h"
#include "frontend/perfilado.h"
#include "core/faultlog.h"
#include "frontend/savefile.h"
#include "frontend/mempakfile.h"
#include "frontend/version.h"
#include "gfxcheck.h"
#include "interp.h"
#include "raster.h"
#include "rdp/targets.h"
#include "rsp/gfx/backend.h"
#include "rsp/audio/backend.h"
#include "rsp/gfx/hle.h"
#include "rsp/gfx/spe0.h"
#include "rsp/jpeg/jpeg.h"
#include "rsp/jpeg/re2.h"
#include "rsp/lle/core.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__powerpc64__) && defined(__linux__)
#include <ucontext.h>
#endif

#define DEFAULT_CYCLES (1ull << 32)  /* ~46 s de la consola */
#define DEFAULT_SLICE  (1ull << 20)
#define MAX_TEXT       255
#define MAX_FAIL_TEXTS 4
#define MAX_PADS       2048
#define MAX_POKES      64

enum stop_reason {
	STOP_NONE,
	STOP_UNTIL,
	STOP_FAIL,
	STOP_CYCLES,
	STOP_STUCK,
	STOP_FIELDS,
};

static struct salsa64_n64 n64;
static uint8_t rdram_buf[SALSA64_RDRAM_MAX] __attribute__((aligned(128)));
static uint8_t *rdram = rdram_buf;  /* o el principio de la ventana de fastmem (rec_host.h) */
static uint8_t isv_buf[SALSA64_ISV_SIZE] __attribute__((aligned(16)));
static struct salsa64_rec rec;
static bool gfx_affine_chain;
static bool gfx_no_fog_1cycle;
static bool gfx_no_rsp_clip;  /* SALSA64_SIN_SOMBRA_AFIN: sin la caja del RSP ni la sombra afín */
static bool gfx_s2dex_bg_row_old;
static bool gfx_f5_sub_old;  /* SALSA64_F5_SUBLISTA_VIEJA: GFX_JOB_F5_SUB_OLD */

/* fastmem (docs/fastmem.md): un acceso del código generado fuera de la RDRAM
 * cae en la ventana sin permisos; el manejador parchea el sitio hacia su
 * camino frío y vuelve, y la instrucción (ahora un salto) se repite. Lo que
 * no es un sitio conocido muere como antes. Solo tiene sentido en ppc64
 * Linux (qemu), donde corre el recompilador. */
#if defined(__powerpc64__) && defined(__linux__)
/* Un SIGSEGV que no es de fastmem: dónde (pc, y si es código generado, la
 * palabra de la caché) y qué dirección, a stderr sin pasar por stdio, y
 * morir como siempre. Para encontrar el bloque en un volcado
 * (SALSA64_REC_DUMP=1: «bloque <pc> entrada <palabra> …»). */
static void segv_report(uintptr_t nip, uintptr_t addr)
{
	char b[160];
	int n = 0;
	const char *h = "0123456789abcdef";
	const char *t = "salsa64-run: SIGSEGV en pc=0x";
	while (*t)
		b[n++] = *t++;
	for (int i = 60; i >= 0; i -= 4)
		b[n++] = h[(nip >> i) & 15];
	t = " direccion=0x";
	while (*t)
		b[n++] = *t++;
	for (int i = 60; i >= 0; i -= 4)
		b[n++] = h[(addr >> i) & 15];
	if (rec.code && nip >= (uintptr_t)rec.code && nip < (uintptr_t)(rec.code + rec.code_words)) {
		uintptr_t w = (nip - (uintptr_t)rec.code) / 4;
		t = " palabra=";
		while (*t)
			b[n++] = *t++;
		char d[12];
		int m = 0;
		do {
			d[m++] = (char)('0' + w % 10);
			w /= 10;
		} while (w);
		while (m)
			b[n++] = d[--m];
	}
	b[n++] = '\n';
	write(2, b, (size_t)n);
}

static void fastmem_segv(int sig, siginfo_t *si, void *uc_)
{
	ucontext_t *uc = uc_;
	uintptr_t nip = (uintptr_t)uc->uc_mcontext.gp_regs[PT_NIP];
	if (rec.fastmem && salsa64_rec_fastmem_fault(&rec, nip))
		return;
	segv_report(nip, (uintptr_t)si->si_addr);
	signal(sig, SIG_DFL);
}

static bool fastmem_install_handler(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = fastmem_segv;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	return sigaction(SIGSEGV, &sa, NULL) == 0;
}
#else
static bool fastmem_install_handler(void)
{
	return false;
}
#endif
/* SALSA64_REC_PROF=N: el perfil del recompilador (rec_prof.h, solo cuentas en
 * el host) cada N campos y al final. */
static struct salsa64_rec_prof prof, prof_prev;
static uint32_t prof_every, prof_next;
/* --profile: el perfil persistente por juego (rec_profile.h); las entradas
 * contadas se suman a la tabla cada tanto (como el frontend, entre campos). */
static struct salsa64_rec_profile profile;
static uint32_t profile_fold_next;

static void prof_line(void *ctx, const char *text)
{
	(void)ctx;
	fprintf(stderr, "%s\n", text);
}

/* SALSA64_REC_SAMPLE=1: el muestreo por tiempo (rec_sample.h) desde un hilo
 * propio cada 200 µs, como en la PS3; al final, los bloques con más
 * muestras. En qemu el tiempo no es el del Cell: prueba el mecanismo (el id
 * que deja cada bloque, las marcas, el plegado y el informe). */
static struct salsa64_rec_sample host_sample;
static volatile int sample_stop;
static pthread_t sample_tid;

static void *sample_thread(void *arg)
{
	(void)arg;
	while (!sample_stop) {
		struct timespec ts = { 0, 200000 };
		nanosleep(&ts, NULL);
		salsa64_rec_sample_tick(&host_sample, &n64.cpu);
	}
	return NULL;
}

/* --perfilado DIR: la sesión de la app de perfilado de la PS3 (docs/perfilado.md,
 * frontend/perfilado.h), escrita sin hilos: un anillo que se vacía cada 60
 * campos. Los tiempos son los del host (en qemu no dicen nada del Cell):
 * sirve para probar el formato, el lector (tools/perfilado) y el registro
 * del recompilador de punta a punta. */
static struct {
	const char *root;
	struct perf_cfg cfg;
	struct perf_session ses;
	struct perf_ring ring;
	struct salsa64_rec_trace *trace;
	bool on;
	uint64_t t0, t_field, t_win;
	uint32_t field_win, next_trace, trace_field0, rng, tasks, traces;
	uint64_t cyc_win, idle_win, compiled, retrans, words, evictions, flushes, killed, jr, interp, smp_ticks;
	uint32_t marks[SALSA64_SMP_N], gfx_tasks, draws, tris;
	struct salsa64_rec_trace_prev slow_prev;
} hp;

static uint64_t now_us(void);

static uint32_t hp_ms(void)
{
	return (uint32_t)((now_us() - hp.t0) / 1000);
}

static void hp_block(void *ctx, const struct salsa64_rec_trace_block *b)
{
	(void)ctx;
	if (hp.cfg.blocks && perf_room(&hp.ses))
		perf_put_block(&hp.ring, hp_ms(), n64.bus.vi.fields, b);
}

static void hp_evict(void *ctx, unsigned kind, unsigned seg, unsigned killed)
{
	(void)ctx;
	perf_put_evict(&hp.ring, hp_ms(), n64.bus.vi.fields, kind, seg, killed);
}

static void hp_task(const struct gfx_result *r)
{
	if (!hp.on)
		return;
	hp.tasks++;
	hp.draws += r->draws;
	hp.tris += r->triangles;
	if (!hp.cfg.tasks)
		return;
	struct perf_task t;
	memset(&t, 0, sizeof t);
	t.ms = hp_ms();
	t.field = n64.bus.vi.fields;
	t.task = hp.tasks;
	t.commands = r->commands;
	t.triangles = r->triangles;
	t.draws = (uint16_t)perf_sat16(r->draws);
	t.textures = (uint16_t)perf_sat16(r->textures);
	t.unknown = (uint16_t)perf_sat16(r->unknown_cmds);
	t.last_unknown = (uint8_t)r->last_unknown;
	t.flags = (uint8_t)r->flags;
	t.approx_cc = (uint16_t)perf_sat16(r->approx_cc);
	t.skipped = (uint16_t)perf_sat16(r->skipped);
	t.dl_errors = (uint16_t)perf_sat16(r->dl_errors);
	perf_put_task(&hp.ring, &t);
}

/* Antes de salsa64_rec_init: la configuración y rec.trace. */
static void hp_prepare(void)
{
	if (!hp.root)
		return;
	perf_cfg_defaults(&hp.cfg);
	const char *env = getenv("SALSA64_PERFILADO_CFG");
	if (env) {
		char text[512];
		snprintf(text, sizeof text, "%s", env);
		for (char *p = text; *p; p++)
			if (*p == ',')
				*p = '\n';
		perf_cfg_parse(&hp.cfg, text);
	}
	if (!hp.cfg.on)
		return;
	hp.trace = calloc(1, sizeof *hp.trace);
	if (!hp.trace)
		return;
	hp.trace->seq = hp.cfg.trace;
	hp.trace->jr = hp.cfg.jr;
	hp.trace->win_len = hp.cfg.trace_len;
	hp.trace->on_block = hp_block;
	hp.trace->on_evict = hp_evict;
	salsa64_rec_trace_init(hp.trace);
}

static bool hp_open(const uint8_t *rom, const char *path)
{
	static uint8_t mem[8u << 20];
	if (!hp.root || !hp.cfg.on)
		return true;
	char game[48];
	salsa64_save_basename(rom, game, sizeof game);
	perf_ring_init(&hp.ring, mem, sizeof mem);
	hp.ses.rings[0] = &hp.ring;
	uint64_t cap = (uint64_t)hp.cfg.cap_mb << 20;
	if (!perf_session_open(&hp.ses, hp.root, game, cap, cap / 10)) {
		fprintf(stderr, "salsa64-run: --perfilado: no se pudo crear la sesión en %s/%s\n", hp.root, game);
		return false;
	}
	hp.on = true;
	hp.t0 = hp.t_field = hp.t_win = now_us();
	hp.field_win = n64.bus.vi.fields;
	hp.cyc_win = n64.cpu.cycles;
	hp.idle_win = n64.cpu.idle_cycles;
	hp.next_trace = n64.bus.vi.fields + 30;
	char names[2048], cfg[320], ctx[1024];
	perf_names_text(names, sizeof names);
	perf_put_text(&hp.ring, PERF_R_NAMES, 0, names);
	perf_cfg_text(&hp.cfg, cfg, sizeof cfg);
	char name[21];
	memcpy(name, rom + 0x20, 20);
	name[20] = '\0';
	for (int k = 19; k >= 0 && name[k] == ' '; k--)
		name[k] = '\0';
	snprintf(ctx, sizeof ctx,
		 "version=%s\ncommit=%s\napp=salsa64-run\nappid=host\njuego=%s\ncarpeta=%s\ncodigo=%.4s\ncrc1=%08X\n"
		 "crc2=%08X\nregion=%c\nrom=%s\nrecompilador=%s\nrec_opciones=0x%x\ntraza=%s\nperfilado=%s\n",
		 salsa64_version, salsa64_commit, name, game, (const char *)rom + 0x3B, (unsigned)salsa64_rd32(rom + 0x10),
		 (unsigned)salsa64_rd32(rom + 0x14), rom[0x3E] >= 0x20 && rom[0x3E] < 0x7F ? rom[0x3E] : '?', path,
		 rec.trace ? "si" : "no", rec.opts, rec.trace && rec.trace->seq ? "si" : "no", cfg);
	perf_put_text(&hp.ring, PERF_R_CONTEXT, hp_ms(), ctx);
	fprintf(stderr, "salsa64-run: --perfilado: %s\n", hp.ses.path);
	return true;
}

/* Después de cada campo. */
static void hp_field(void)
{
	if (!hp.on)
		return;
	uint64_t t = now_us(), us = t - hp.t_field;
	hp.t_field = t;
	uint32_t field = n64.bus.vi.fields;
	struct salsa64_rec_trace *tr = rec.trace;
	if (tr) {
		if (us >= (uint64_t)hp.cfg.stall_ms * 1000u) {
			uint32_t v[PERF_S_N];
			memset(v, 0, sizeof v);
			v[PERF_S_campo] = field;
			v[PERF_S_emu_us] = (uint32_t)us;
			v[PERF_S_compilados] = tr->field.compiled;
			v[PERF_S_palabras] = tr->field.words;
			v[PERF_S_retrad] = tr->field.retrans;
			v[PERF_S_desalojos] = tr->field.evictions;
			v[PERF_S_vaciados] = tr->field.flushes;
			v[PERF_S_muertos] = tr->field.killed;
			perf_put_values(&hp.ring, PERF_R_STALL, hp_ms(), v, PERF_S_N);
		}
		memset(&tr->field, 0, sizeof tr->field);
		if (tr->seq) {
			unsigned len = salsa64_rec_trace_take(&rec, tr->armed && field - hp.trace_field0 > 4);
			if (len) {
				perf_put_trace(&hp.ring, hp_ms(), hp.trace_field0, tr->win_cycles, tr, len);
				hp.traces++;
			}
			if (!tr->armed && !tr->arm_at && field >= hp.next_trace) {
				hp.rng = hp.rng * 1103515245u + 12345u;
				tr->arm_at = n64.cpu.cycles + 1 + (hp.rng >> 8) % (n64.bus.vi.field_cycles ? n64.bus.vi.field_cycles : 1);
				hp.trace_field0 = field;
				hp.next_trace = field + 60u * hp.cfg.trace_s;
			}
		}
	}
	if (field - hp.field_win >= 300) {
		uint32_t v[PERF_W_N];
		memset(v, 0, sizeof v);
		uint64_t dt = t - hp.t_win, fields = field - hp.field_win;
		uint64_t cyc = n64.cpu.cycles - hp.cyc_win, idle = n64.cpu.idle_cycles - hp.idle_win;
		v[PERF_W_campo] = field;
		v[PERF_W_dur_ms] = (uint32_t)(dt / 1000);
		v[PERF_W_campos] = (uint32_t)fields;
		v[PERF_W_cps_x100] = (uint32_t)(dt ? fields * 100000000ull / dt : 0);
		v[PERF_W_mips_x10] = (uint32_t)(dt ? (cyc - idle) * 10 / dt : 0);
		v[PERF_W_espera_pm] = (uint32_t)(cyc ? idle * 1000 / cyc : 0);
		v[PERF_W_emu_us_campo] = (uint32_t)(fields ? dt / fields : 0);
		v[PERF_W_tareas] = hp.tasks - hp.gfx_tasks;
		v[PERF_W_dibujos] = hp.draws;
		v[PERF_W_triangulos] = hp.tris;
		hp.gfx_tasks = hp.tasks;
		hp.draws = hp.tris = 0;
		if (tr) {
			v[PERF_W_rec_bloques] = (uint32_t)(tr->compiled - hp.compiled);
			v[PERF_W_rec_retrad] = (uint32_t)(tr->retrans - hp.retrans);
			v[PERF_W_rec_palabras] = (uint32_t)(tr->words - hp.words);
			v[PERF_W_rec_desalojos] = (uint32_t)(tr->evictions - hp.evictions);
			v[PERF_W_rec_vaciados] = (uint32_t)(tr->flushes - hp.flushes);
			v[PERF_W_rec_muertos] = (uint32_t)(tr->killed - hp.killed);
			v[PERF_W_rec_interp] = (uint32_t)(rec.stats.interp_steps - hp.interp);
			v[PERF_W_rec_jr] = (uint32_t)(n64.cpu.rec_jr_count - hp.jr);
			hp.compiled = tr->compiled;
			hp.retrans = tr->retrans;
			hp.words = tr->words;
			hp.evictions = tr->evictions;
			hp.flushes = tr->flushes;
			hp.killed = tr->killed;
			hp.interp = rec.stats.interp_steps;
			hp.jr = n64.cpu.rec_jr_count;
			perf_put_slow_window(&hp.ring, hp_ms(), rec.trace, &hp.slow_prev, NULL);
		}
		if (rec.sample) {
			uint32_t other = 0;
			for (unsigned m = 1; m < SALSA64_SMP_N; m++) {
				v[PERF_W_m_nativo + m] = host_sample.marks[m] - hp.marks[m];
				other += v[PERF_W_m_nativo + m];
				hp.marks[m] = host_sample.marks[m];
			}
			v[PERF_W_m_total] = (uint32_t)(host_sample.ticks - hp.smp_ticks);
			v[PERF_W_m_nativo] = v[PERF_W_m_total] > other ? v[PERF_W_m_total] - other : 0;
			hp.smp_ticks = host_sample.ticks;
		}
		v[PERF_W_perdidos] = hp.ring.lost;
		perf_put_values(&hp.ring, PERF_R_WINDOW, hp_ms(), v, PERF_W_N);
		hp.t_win = t;
		hp.field_win = field;
		hp.cyc_win = n64.cpu.cycles;
		hp.idle_win = n64.cpu.idle_cycles;
	}
	if (field % 60 == 0)
		perf_drain(&hp.ses);
}

/* Al terminar, antes del informe del muestreo (que agota su tabla). */
static void hp_close(const char *reason)
{
	if (!hp.on)
		return;
	uint32_t ms = hp_ms();
	if (rec.trace) {
		unsigned len = salsa64_rec_trace_take(&rec, true);
		if (len)
			perf_put_trace(&hp.ring, ms, hp.trace_field0, rec.trace->win_cycles, rec.trace, len);
		perf_put_fpu(&hp.ring, ms, rec.trace);
		salsa64_rec_trace_fold_all(&rec);
		if (rec.trace->jr)
			perf_put_jr(&hp.ring, ms, rec.trace);
	}
	if (rec.sample) {
		salsa64_rec_sample_fold_all(&rec);
		perf_put_samples(&hp.ring, ms, &host_sample);
	}
	char end[512];
	snprintf(end, sizeof end, "duracion_ms=%u\ncampos=%u\nciclos=%llu\nciclos_espera=%llu\nrec_bloques=%llu\n"
		 "rec_jr=%llu\nsalida=%s\ntareas=%u\ntrazas=%u\nperdidos_emu=%u\n", ms, n64.bus.vi.fields,
		 (unsigned long long)n64.cpu.cycles, (unsigned long long)n64.cpu.idle_cycles,
		 (unsigned long long)rec.stats.compiled, (unsigned long long)n64.cpu.rec_jr_count, reason, hp.tasks,
		 hp.traces, hp.ring.lost);
	perf_put_text(&hp.ring, PERF_R_END, ms, end);
	perf_session_close(&hp.ses);
	fprintf(stderr, "salsa64-run: --perfilado: %s, %llu bytes, %u ventanas de la traza, %u bytes perdidos\n",
		hp.ses.path, (unsigned long long)hp.ses.written, hp.traces, hp.ring.lost);
	hp.on = false;
}

/* SALSA64_REC_DUMP=1: cada bloque compilado en hexadecimal (como en las
 * unitarias), para desensamblarlo (tools/rec-disasm.sh) o medirlo. */
static void dump_block(struct salsa64_rec *r, uint64_t pc, uint32_t entry, uint32_t words, bool temp)
{
	fprintf(stderr, "bloque %016llx%s entrada %u, %u palabras (%u calientes):", (unsigned long long)pc,
		temp ? " (recortado)" : "", entry, words, r->nbuf[0]);  /* nbuf[0]: la sección caliente del último bloque */
	for (uint32_t k = 0; k < words; k++)
		fprintf(stderr, " %08x", r->code[entry + k]);
	fputc('\n', stderr);
}

/* --lockstep: la máquina de referencia, solo con el intérprete. */
static struct salsa64_n64 ref;
static uint8_t ref_rdram[SALSA64_RDRAM_MAX] __attribute__((aligned(128)));
static uint8_t ref_isv_buf[SALSA64_ISV_SIZE] __attribute__((aligned(16)));
static struct {
	uint64_t exits;
	uint64_t ram_every;
	uint64_t history[16];   /* últimos bloques, para el informe */
	uint32_t fields;        /* campo del VI de la última comparación de la RDRAM */
} lock;

static struct salsa64_save save, ref_save;
static struct savefile save_file;
static struct salsa64_mempak mempak, ref_mempak;
static struct mempakfile mempak_file;

/* --pad: el control 1 por campo del VI. `applied` guarda en qué ciclo se
 * aplicó cada cambio para que la referencia de --lockstep reciba lo mismo
 * en el mismo ciclo. */
static struct {
	struct { uint32_t field; struct salsa64_pad pad; } at[MAX_PADS];
	unsigned n, next;
	struct { uint64_t cycle; struct salsa64_pad pad; } applied[MAX_PADS * 2];
	unsigned napplied, ref_next;
} pads;

/* --poke: escrituras en la RDRAM por campo del VI (`size` en bytes). */
struct poke {
	uint32_t field, addr, value;
	uint8_t size;
};
static struct {
	struct poke at[MAX_POKES];
	unsigned n, next;
} pokes;

static uint32_t trace_from;        /* --trace-from: campo desde el que se traza */
static bool trace_exc_faults_only; /* --trace-faults: sin interrupciones */

static bool tracing(void)
{
	return n64.bus.vi.fields >= trace_from;
}

static bool pad_button(const char *s, size_t n, uint16_t *bit)
{
	static const struct { const char *name; uint16_t bit; } names[] = {
		{ "A", SALSA64_PAD_A }, { "B", SALSA64_PAD_B }, { "Z", SALSA64_PAD_Z },
		{ "START", SALSA64_PAD_START }, { "DU", SALSA64_PAD_D_UP }, { "DD", SALSA64_PAD_D_DOWN },
		{ "DL", SALSA64_PAD_D_LEFT }, { "DR", SALSA64_PAD_D_RIGHT }, { "L", SALSA64_PAD_L },
		{ "R", SALSA64_PAD_R }, { "CU", SALSA64_PAD_C_UP }, { "CD", SALSA64_PAD_C_DOWN },
		{ "CL", SALSA64_PAD_C_LEFT }, { "CR", SALSA64_PAD_C_RIGHT },
	};
	for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
		if (strlen(names[i].name) == n && !strncmp(s, names[i].name, n)) {
			*bit = names[i].bit;
			return true;
		}
	return false;
}

/* "CAMPO:BOTONES[@X,Y];…" (o separados por ','): botones unidos con +,
 * '-' suelta todo, el stick como @X,Y o x<n>/y<n>. Cada estado vale hasta
 * el siguiente. */
static bool pad_parse(const char *spec)
{
	char sep = strchr(spec, ';') ? ';' : ',';
	while (*spec) {
		if (pads.n == MAX_PADS)
			return false;
		char *end;
		unsigned long field = strtoul(spec, &end, 0);
		if (end == spec || *end != ':' || (pads.n && field <= pads.at[pads.n - 1].field))
			return false;
		struct salsa64_pad pad = { 0, 0, 0, 1 };
		const char *p = end + 1;
		while (*p && *p != sep) {
			if (*p == '@') {
				long x = strtol(p + 1, &end, 10);
				if (*end != ',')
					return false;
				long y = strtol(end + 1, &end, 10);
				if (x < -128 || x > 127 || y < -128 || y > 127)
					return false;
				pad.x = (int8_t)x, pad.y = (int8_t)y;
				p = end;
				continue;
			}
			const char *q = p;
			while (*q && *q != sep && *q != '+' && *q != '@')
				q++;
			size_t n = (size_t)(q - p);
			uint16_t bit;
			if (n == 1 && *p == '-') {
				/* suelta todo */
			} else if (n > 1 && (*p == 'x' || *p == 'y')) {
				long v = strtol(p + 1, &end, 10);
				if (end != q || v < -128 || v > 127)
					return false;
				*(*p == 'x' ? &pad.x : &pad.y) = (int8_t)v;
			} else if (n && pad_button(p, n, &bit)) {
				pad.buttons |= bit;
			} else if (n) {
				return false;
			}
			p = *q == '+' ? q + 1 : q;
		}
		pads.at[pads.n].field = (uint32_t)field;
		pads.at[pads.n].pad = pad;
		pads.n++;
		spec = *p == sep ? p + 1 : p;
	}
	return pads.n > 0;
}

/* "CAMPO:DIR=VALOR;…" (o separados por ','), en orden de campo. */
static bool poke_parse(const char *spec)
{
	char sep = strchr(spec, ';') ? ';' : ',';
	while (*spec) {
		if (pokes.n == MAX_POKES)
			return false;
		char *end;
		unsigned long field = strtoul(spec, &end, 0);
		if (end == spec || *end != ':' || (pokes.n && field < pokes.at[pokes.n - 1].field))
			return false;
		unsigned long addr = strtoul(end + 1, &end, 16);
		if (*end != '=')
			return false;
		const char *v = end + 1;
		if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X'))
			v += 2;
		unsigned long value = strtoul(v, &end, 16);
		size_t digits = (size_t)(end - v);
		if (digits != 2 && digits != 4 && digits != 8)
			return false;
		uint8_t size = (uint8_t)(digits / 2);
		addr &= 0x1FFFFFFF;
		if (addr & (size - 1u) || (*end && *end != sep))
			return false;
		pokes.at[pokes.n].field = (uint32_t)field;
		pokes.at[pokes.n].addr = (uint32_t)addr;
		pokes.at[pokes.n].value = (uint32_t)value;
		pokes.at[pokes.n].size = size;
		pokes.n++;
		spec = *end == sep ? end + 1 : end;
	}
	return pokes.n > 0;
}

static void poke_field(void)
{
	uint32_t field = n64.bus.vi.fields;
	while (pokes.next < pokes.n && field >= pokes.at[pokes.next].field) {
		const struct poke *k = &pokes.at[pokes.next++];
		if (k->addr + k->size > n64.bus.rdram_size)
			continue;
		uint8_t *p = n64.bus.rdram + k->addr;
		for (unsigned i = 0; i < k->size; i++)
			p[i] = (uint8_t)(k->value >> (8 * (k->size - 1 - i)));
		if (tracing())
			fprintf(stderr, "campo %u: poke %08x = %0*x\n", field, k->addr, 2 * k->size, k->value);
	}
}

static unsigned pad_port;  /* --pad-port: el guion de --pad va a este puerto (0 a 3) */

static void pad_apply(struct salsa64_n64 *m, const struct salsa64_pad *pad)
{
	salsa64_pif_set_pad(&m->bus, pad_port, pad);
}

/* Al empezar cada campo (la máquina parada justo en el fin del anterior). */
static void pad_field(void)
{
	uint32_t field = n64.bus.vi.fields;
	const struct salsa64_pad *pad = NULL;
	while (pads.next < pads.n && field >= pads.at[pads.next].field)
		pad = &pads.at[pads.next++].pad;
	if (!pad)
		return;
	pad_apply(&n64, pad);
	if (pads.napplied < sizeof pads.applied / sizeof pads.applied[0]) {
		pads.applied[pads.napplied].cycle = n64.cpu.cycles;
		pads.applied[pads.napplied].pad = *pad;
		pads.napplied++;
	}
	if (tracing())
		fprintf(stderr, "campo %u (ciclo %llu): control %u botones 0x%04x x %d y %d\n", field,
			(unsigned long long)n64.cpu.cycles, pad_port + 1, pad->buttons, pad->x, pad->y);
}

/* Al terminar cada campo (modo campo por campo): controles programados y,
 * con --gfx-frame-every, la imagen de la HLE en ARCHIVO-<campo>.ppm (sin la
 * extensión de ARCHIVO) desde --trace-from. */
static bool gfx_dump(const char *path);

static void raster_field_vi(void);

/* SALSA64_RITMO=N: cada N campos (desde --trace-from), cuántas tareas gráficas
 * y de audio arrancaron, cuántas interrupciones del DP llegaron y cuántas veces
 * cambió VI_ORIGIN (cuadros presentados): el ritmo de dibujo del juego en
 * tiempo emulado, para compararlo con la consola (ares) en la misma escena. */
static uint32_t ritmo_every;
static void ritmo_field(void)
{
	static uint32_t from, gfx0, audio0, dp0, origin, changes, held0;
	static uint64_t sum0;
	static bool started;
	const struct salsa64_rsp_stats *s = &n64.bus.rsp.stats;
	uint32_t f = n64.bus.vi.fields;
	uint32_t org = n64.bus.vi.regs[SALSA64_VI_ORIGIN] & 0xFFFFFF;
	if (!started) {
		started = true;
		from = f, gfx0 = s->gfx, audio0 = s->audio, dp0 = s->dp_fired, origin = org, changes = 0;
		held0 = s->model_held, sum0 = s->model_sum;
		return;
	}
	changes += org != origin;
	origin = org;
	if (f - from < ritmo_every)
		return;
	double n = f - from, hz = n64.bus.vi.clock_hz > 49000000u ? 50.0 : 60.0;  /* PAL: 49,66 MHz */
	uint32_t gfx = s->gfx - gfx0;
	fprintf(stderr,
		"ritmo campos %u–%u: %u tareas gráficas (%.1f/s, %.2f por campo), %u de audio, %u DP; "
		"VI_ORIGIN cambió %u veces (%.1f cuadros/s)",
		from, f, gfx, gfx * hz / n, gfx / n, s->audio - audio0, s->dp_fired - dp0, changes, changes * hz / n);
	if (n64.bus.rsp.gfx_model && gfx)
		fprintf(stderr, "; modelo %.2f ms por tarea, %u retrasadas", (double)(s->model_sum - sum0) / gfx / 93750.0,
			s->model_held - held0);
	fputc('\n', stderr);
	from = f, gfx0 = s->gfx, audio0 = s->audio, dp0 = s->dp_fired, changes = 0;
	held0 = s->model_held, sum0 = s->model_sum;
}

static void field_done(const char *gfx_path, uint32_t frame_every)
{
	if (ritmo_every && tracing())
		ritmo_field();
	/* Con --pad: lo que el PIF entregó al juego, en cada cambio de botones
	 * (como el «PIF → juego» del log de la PS3). */
	static uint16_t sent;
	const struct salsa64_pad *out = &n64.bus.pif.sent[0];
	if (pads.n && out->buttons != sent && tracing())
		fprintf(stderr, "campo %u: PIF → juego: control 1 botones 0x%04x (lectura %u)\n", n64.bus.vi.fields,
			out->buttons, n64.bus.pif.reads[0]);
	sent = out->buttons;
	pad_field();
	poke_field();
	raster_field_vi();
	if (frame_every && gfx_path && tracing() && n64.bus.vi.fields % frame_every == 0) {
		char name[512];
		size_t len = strlen(gfx_path);
		if (len > 4 && !strcmp(gfx_path + len - 4, ".ppm"))
			len -= 4;
		snprintf(name, sizeof name, "%.*s-%u.ppm", (int)len, gfx_path, n64.bus.vi.fields);
		gfx_dump(name);
	}
}

/* La referencia de --lockstep hasta `to`, aplicando los controles en el
 * ciclo en que los recibió la máquina principal. */
static void ref_run_to(uint64_t to)
{
	while (pads.ref_next < pads.napplied && pads.applied[pads.ref_next].cycle <= to) {
		uint64_t at = pads.applied[pads.ref_next].cycle;
		if (ref.cpu.cycles < at)
			salsa64_n64_run(&ref, at - ref.cpu.cycles);
		pad_apply(&ref, &pads.applied[pads.ref_next].pad);
		pads.ref_next++;
	}
	if (ref.cpu.cycles < to)
		salsa64_n64_run(&ref, to - ref.cpu.cycles);
}

/* Audio: HLE y lo que sale por el AI (16 bits estéreo, orden de la N64). */
#define AUDIO_MAX (16u << 20)
static struct aud_state aud_state;
static struct salsa64_audio_sync aud_sync;
static uint8_t audio_buf[AUDIO_MAX];
static struct {
	uint32_t bytes, rate, dmas, gaps, rates;
	uint64_t next;      /* ciclo en que debía empezar el DMA siguiente */
	bool lost;          /* no cupo todo en audio_buf */
} audio;

/* Sumidero del AI: guarda las muestras y cuenta los huecos (un DMA que no
 * empieza justo cuando terminó el anterior). */
static void audio_push(void *ctx, const uint8_t *p, uint32_t bytes, uint32_t rate, uint64_t cycle)
{
	(void)ctx;
	if (audio.dmas && cycle != audio.next)
		audio.gaps++;
	if (audio.rate && rate != audio.rate)
		audio.rates++;
	audio.rate = rate;
	audio.dmas++;
	uint64_t div = (n64.bus.ai.regs[SALSA64_AI_DACRATE] & 0x3FFF) + 1;
	uint64_t num = (uint64_t)(bytes / 4) * 93750000ull * div, den = n64.bus.vi.clock_hz;
	audio.next = cycle + (num + den - 1) / den;
	uint32_t n = bytes < AUDIO_MAX - audio.bytes ? bytes : AUDIO_MAX - audio.bytes;
	memcpy(audio_buf + audio.bytes, p, n);
	audio.bytes += n;
	audio.lost |= n < bytes;
}

/* HLE gráfica: estado, búferes de salida y totales. */
static struct gfx_state gfx_state;
static struct salsa64_gfx_sync gfx_sync;
static bool gchk_on;  /* --lle-check-gfx */
static bool gchk_self;  /* SALSA64_GCHK_SELF=1: la HLE contra sí misma (prueba del propio método) */
/* SALSA64_DPC_ESTADO_APARTE=1: las listas del CPU las consume otra HLE con su propio estado del RDP y
 * su propio directorio de texturas sobre la misma región de la caché, como la PS3 con la HLE gráfica
 * en el SPE y un RDP en el PPE para esas listas (antes de que fueran al mismo SPE). */
static struct gfx_state gfx_state_dpc;
static struct salsa64_gfx_sync gfx_sync_dpc;

/* --lle: el microcódigo real del RSP, en rebanadas del hilo del CPU. */
static struct salsa64_lle_host lle_host;
static enum salsa64_lle_mode lle_mode;
static double lle_speed;
static struct gfx_vertex gfx_vtx[1u << 16] __attribute__((aligned(128)));
/* 1280 KB para los dibujos y, con GFX_JOB_INTERP (--interp), la mitad de eso
 * más para los registros GFX_OP_VGROUP (un tercio del total, gfx.h), como
 * en la PS3 (src/rdp/rdp.c, CMD_BYTES y CMD_INTERP). */
#ifndef GFX_CMD_DRAWS
#define GFX_CMD_DRAWS (1280u << 10)
#endif
static uint8_t gfx_cmd[GFX_CMD_DRAWS + GFX_CMD_DRAWS / (GFX_VGROUP_SHARE - 1)] __attribute__((aligned(128)));
static uint8_t gfx_tex[4u << 20] __attribute__((aligned(128)));
static uint8_t gfx_cache[8u << 20] __attribute__((aligned(128)));

/* La referencia de --lockstep lleva la misma HLE gráfica y de audio con su
 * propio estado y búferes: sus tareas dan los mismos eventos (fin de tarea,
 * G_RDPFULLSYNC → interrupción del DP) y el audio escribe lo mismo en su
 * RDRAM; la salida gráfica se descarta. Sin ellos la referencia no recibía
 * la interrupción del DP de las tareas con HLE (visto el 2026-10-11 con
 * GoldenEye y Perfect Dark: antes no dibujaban y pasaban por «sin HLE»). */
static struct gfx_state ref_gfx_state;
static struct salsa64_gfx_sync ref_gfx_sync;
static struct aud_state ref_aud_state;
static struct salsa64_audio_sync ref_aud_sync;
static struct gfx_vertex ref_gfx_vtx[1u << 16] __attribute__((aligned(128)));
static uint8_t ref_gfx_cmd[GFX_CMD_DRAWS] __attribute__((aligned(128)));
static uint8_t ref_gfx_tex[4u << 20] __attribute__((aligned(128)));
static uint8_t ref_gfx_cache[8u << 20] __attribute__((aligned(128)));
static uint32_t ref_gfx_tasks;

static void ref_gfx_acquire(void *ctx, struct gfx_job *job)
{
	(void)ctx;
	job->vtx_ea = (uint64_t)(uintptr_t)ref_gfx_vtx;
	job->vtx_cap = sizeof ref_gfx_vtx;
	job->cmd_ea = (uint64_t)(uintptr_t)ref_gfx_cmd;
	job->cmd_cap = sizeof ref_gfx_cmd;
	job->tex_ea = (uint64_t)(uintptr_t)ref_gfx_tex;
	job->tex_cap = sizeof ref_gfx_tex;
	if (!getenv("SALSA64_NO_TEXCACHE")) {
		job->cache_ea = (uint64_t)(uintptr_t)ref_gfx_cache;
		job->cache_cap = sizeof ref_gfx_cache;
	}
	job->task = ref_gfx_tasks + 1;
	job->done_task = ref_gfx_tasks;
}

static void ref_gfx_done(void *ctx, const struct gfx_job *job, const struct gfx_result *res)
{
	(void)ctx, (void)job, (void)res;
	ref_gfx_tasks++;
}
static struct {
	struct gfx_result sum;  /* contadores sumados; flags acumuladas */
	uint64_t tex_arena, tex_evicted;  /* aparte: en gfx_result son de 16 bits */
	uint32_t tasks;
	bool raster;
} gfx;

/* Diagnóstico (SALSA64_GFX_LATE=N): la HLE lee la RDRAM al final de la
 * tarea, tras N sondeos, como un SPE lento mientras el CPU sigue. Con
 * SALSA64_GFX_TEAR además corre al arrancar (y deshace su estado) y avisa
 * si los vértices difieren: el CPU cambió durante la tarea algo que la
 * tarea lee. */
static struct {
	struct salsa64_gfx_backend backend;
	struct salsa64_task task;
	enum salsa64_gbi gbi;
	unsigned polls, left, block_after, done_calls;
	uint64_t completion_calls;
	unsigned completions, completion_max, blocked;
	bool pending, tear;
	struct gfx_state state;
	uint32_t vtx_bytes, cmd_bytes;
	uint64_t start_cycle;
	unsigned torn_tasks, compared;
} gfx_late;
static struct gfx_vertex tear_vtx[1u << 16];
static uint8_t tear_cmd[GFX_CMD_DRAWS];

static void late_start(void *ctx, const struct salsa64_task *task, enum salsa64_gbi gbi)
{
	(void)ctx;
	gfx_late.task = *task;
	gfx_late.gbi = gbi;
	gfx_late.left = gfx_late.polls;
	gfx_late.done_calls = 0;
	gfx_late.pending = true;
	gfx_late.start_cycle = n64.cpu.cycles;
	if (gfx_late.tear) {
		gfx_late.state = gfx_state;
		gfx_sync.backend.start(gfx_sync.backend.ctx, task, gbi);
		gfx_sync.pending = false;
		gfx_late.vtx_bytes = gfx_sync.res.vtx_bytes;
		gfx_late.cmd_bytes = gfx_sync.res.cmd_bytes;
		memcpy(tear_vtx, gfx_vtx, gfx_late.vtx_bytes);
		memcpy(tear_cmd, gfx_cmd, gfx_late.cmd_bytes);
		gfx_state = gfx_late.state;
	}
}

static bool late_done(void *ctx, bool idle)
{
	(void)ctx, (void)idle;
	gfx_late.done_calls++;
	if (gfx_late.block_after && gfx_late.done_calls >= gfx_late.block_after) {
		gfx_late.blocked++;
		goto done;
	}
	if (gfx_late.left) {
		gfx_late.left--;
		return false;
	}
done:
	gfx_late.completions++;
	gfx_late.completion_calls += gfx_late.done_calls;
	if (gfx_late.done_calls > gfx_late.completion_max)
		gfx_late.completion_max = gfx_late.done_calls;
	return true;
}

/* Vértices de la tarea que cambiaron entre el arranque y el fin. */
static void tear_report(void)
{
	uint32_t n = gfx_sync.res.vtx_bytes < gfx_late.vtx_bytes ? gfx_sync.res.vtx_bytes : gfx_late.vtx_bytes;
	n /= sizeof(struct gfx_vertex);
	unsigned diff = 0, first = ~0u;
	float dmax = 0.0f;
	for (uint32_t i = 0; i < n; i++) {
		const struct gfx_vertex *a = &tear_vtx[i], *b = &gfx_vtx[i];
		if (!memcmp(a, b, sizeof *a))
			continue;
		if (first == ~0u)
			first = i;
		diff++;
		float ax = a->w ? a->x / a->w : 0, bx = b->w ? b->x / b->w : 0;
		float ay = a->w ? a->y / a->w : 0, by = b->w ? b->y / b->w : 0;
		float d = fabsf(ax - bx) + fabsf(ay - by);
		dmax = d > dmax ? d : dmax;
	}
	gfx_late.compared++;
	bool cmd_diff = gfx_sync.res.cmd_bytes != gfx_late.cmd_bytes ||
			memcmp(tear_cmd, gfx_cmd, gfx_late.cmd_bytes) != 0;
	if (!diff && !cmd_diff && gfx_sync.res.vtx_bytes == gfx_late.vtx_bytes)
		return;
	gfx_late.torn_tasks++;
	fprintf(stderr, "tarea gfx (campo %u) cambió durante la tarea (%llu ciclos): %u de %u vértices "
		"(primero %u, máx. %.1f píxeles)%s%s\n", n64.bus.vi.fields,
		(unsigned long long)(n64.cpu.cycles - gfx_late.start_cycle), diff, n, first, dmax,
		cmd_diff ? ", comandos distintos" : "",
		gfx_sync.res.vtx_bytes != gfx_late.vtx_bytes ? ", otro número de vértices" : "");
	if (getenv("SALSA64_GFX_TEAR_V") && first != ~0u) {
		for (uint32_t i = first; i < n && i < first + 12; i++) {
			const struct gfx_vertex *a = &tear_vtx[i], *b = &gfx_vtx[i];
			fprintf(stderr, "  v%-5u antes x/w %8.2f y/w %8.2f w %8.2f  después x/w %8.2f y/w %8.2f w %8.2f\n",
				i, a->w ? a->x / a->w : 0, a->w ? a->y / a->w : 0, a->w, b->w ? b->x / b->w : 0,
				b->w ? b->y / b->w : 0, b->w);
		}
	}
}

static uint32_t late_finish(void *ctx)
{
	(void)ctx;
	if (!gfx_late.pending)
		return 0;
	gfx_late.pending = false;
	gfx_sync.backend.start(gfx_sync.backend.ctx, &gfx_late.task, gfx_late.gbi);
	if (gfx_late.tear)
		tear_report();
	return gfx_sync.backend.finish(gfx_sync.backend.ctx);
}

static uint32_t late_cycles(void *ctx)
{
	(void)ctx;
	return gfx_sync.backend.cycles(gfx_sync.backend.ctx);
}

static unsigned trace_gfx_left;
static bool trace_fb;  /* --trace-fb */

/* Cada campo: el origen del VI al rasterizador (qué color images son
 * framebuffers), como salsa64_rdp_vi en la PS3. */
static void raster_field_vi(void)
{
	struct salsa64_vi_frame f;
	if (gfx.raster && salsa64_vi_frame(&n64.bus, &f))
		raster_vi(&f);
}

static bool trace_gfx_verbose;

#ifdef SALSA64_GFX_TRACE
int salsa64_gfx_trace_now;            /* la tarea en curso se traza (también la lee raster.c) */
static unsigned trace_draw, trace_states;  /* dibujos y estados vistos en la tarea */

/* Llamada desde rdp.c (build_state) por cada estado de dibujo que se arma. */
void salsa64_gfx_trace_state(const struct gfx_state *g, const struct gfx_draw *d, int kind, int tile)
{
	if (!salsa64_gfx_trace_now)
		return;
	fprintf(stderr, "  ST kind %d tile %d cyc %u ntex %u texon %u cc %06x:%08x oh %06x ol %08x prim %08x env %08x "
		"fog %08x mul %.0f off %.0f geom %08x texs %04x,%04x timg %06x/%u vp %.1f,%.1f±%.1f,%.1f dl %06x plod %u pmin %u\n", kind, tile,
		d->cycle, d->ntex, g->tex_on, g->cc_hi, g->cc_lo, g->oh, g->ol, g->prim, g->env, g->fog, g->fog_mul,
		g->fog_off, g->geom, g->tex_s, g->tex_t, g->timg_addr, g->timg_width, g->vtrans[0], g->vtrans[1],
		g->vscale[0], g->vscale[1], g->dl_pc, g->prim_lod, g->prim_min);
	for (int i = 0; i < 2; i++) {
		const struct gfx_tile *t = &g->tiles[(tile + i) & 7];
		fprintf(stderr, "     tile%d fmt %u siz %u pal %u line %u tmem %u cms %u cmt %u mask %u,%u shift %u,%u "
			"sz %u,%u-%u,%u", (tile + i) & 7, t->fmt, t->siz, t->pal, t->line, t->tmem, t->cms, t->cmt,
			t->masks, t->maskt, t->shifts, t->shiftt, t->uls, t->ult, t->lrs, t->lrt);
		if (i < d->ntex)
			fprintf(stderr, " -> %ux%u flags %x key %08x xf %.4f %.4f %.4f %.4f", d->tex[i].width,
				d->tex[i].height, d->tex[i].flags, d->tex[i].key, d->tex[i].xf[0], d->tex[i].xf[1],
				d->tex[i].xf[2], d->tex[i].xf[3]);
		fputc('\n', stderr);
	}
	if (trace_states++ < 3) {
		fprintf(stderr, "     luces %d:", g->num_lights);
		for (int l = 0; l <= g->num_lights; l++)
			fprintf(stderr, " [%.0f %.0f %.0f dir %.2f %.2f %.2f]", g->lights[l].col[0], g->lights[l].col[1],
				g->lights[l].col[2], g->lights[l].dir[0], g->lights[l].dir[1], g->lights[l].dir[2]);
		fputc('\n', stderr);
	}
	static const char *const tn[] = { "T0", "T0a", "T1", "T1a", "1" };
	fprintf(stderr, "     k:");
	for (int lane = 0; lane < 4; lane++) {
		fprintf(stderr, " [%c]", "RGBA"[lane]);
		for (int t = 0; t < GFX_K_TERMS; t++)
			for (int k = 0; k < 3; k++)
				if (d->k[t][k][lane] != 0.0f)
					fprintf(stderr, " %+.2f%s%s", d->k[t][k][lane], tn[t], k == 0 ? "" : k == 1 ? "C" : "Ca");
	}
	fputc('\n', stderr);
}

/* Cargas a la TMEM desde un destino de render (--trace-fb con este build):
 * sin pasar por la procedencia, para ver lo que esta no reconoce. */
void salsa64_gfx_trace_load(const struct gfx_state *g, uint32_t addr, uint32_t bytes, uint32_t line)
{
	if (!trace_fb || !tracing())
		return;
	for (int i = 0; i < RDP_TARGETS_MAX; i++) {
		uint32_t ta, tw, tb, th;
		if (raster_target_info(i, &ta, &tw, &tb, &th) && addr >= ta && addr < ta + tw * tb * th) {
			fprintf(stderr, "fb: campo %u tarea %u: carga a la TMEM desde %06x (+%x del destino %06x/%u): %u bytes, "
				"timg %u/%u siz %u, línea %u\n", n64.bus.vi.fields, gfx.tasks + 1, addr, addr - ta, ta, tw,
				bytes, g->timg_width, g->timg_siz, g->timg_fmt, line);
			return;
		}
	}
}

/* SALSA64_TEXSTAT=N (con este build): cada N campos, cuántas texturas
 * piden los dibujos, cuántas se decodifican y por qué (clave nunca vista, o
 * vista y desalojada de la caché), cuántos texels y cuántas claves
 * distintas hay; SALSA64_TEXSTAT_V lista cada decodificación desde
 * --trace-from. Las claves son los 32 bits de gfx_tex.key. */
#define TS_KEYS (1u << 20)
static struct ts_key {
	uint32_t key, window;  /* última ventana en que se pidió (0: libre) */
} ts_keys[TS_KEYS];
static struct {
	uint32_t window, from_field, tasks, req, dec, dec_new, dec_same_src, distinct, target;
	uint64_t dec_texels, distinct_bytes;
} ts = { .window = 1 };
static uint32_t ts_src[TS_KEYS];  /* origen y tamaño → última clave */

void salsa64_gfx_trace_tex(const struct gfx_state *g, const struct gfx_tile *t, const struct gfx_tex *tex,
			   bool decoded)
{
	(void)g;
	if (!getenv("SALSA64_TEXSTAT"))
		return;
	if (tex->where == GFX_TEX_TARGET) {
		/* Sin texels ni clave: se lee de un destino. */
		ts.req++;
		ts.target++;
		return;
	}
	struct ts_key *k = &ts_keys[(tex->key * 0x9E3779B1u) >> 12];
	while (k->window && k->key != tex->key)
		k = k == &ts_keys[TS_KEYS - 1] ? ts_keys : k + 1;
	bool seen = k->window != 0;
	ts.req++;
	if (k->window != ts.window) {
		ts.distinct++;
		ts.distinct_bytes += (uint32_t)tex->pitch * tex->height;
	}
	k->key = tex->key;
	k->window = ts.window;
	if (!decoded)
		return;
	ts.dec++;
	ts.dec_texels += (uint32_t)tex->width * tex->height;
	if (!seen) {
		ts.dec_new++;
		uint32_t *src = &ts_src[(tex->src_addr * 0x9E3779B1u ^ (uint32_t)tex->width << 16 ^ tex->height) &
					(TS_KEYS - 1)];
		ts.dec_same_src += tex->src_addr && *src && *src != tex->key;
		if (tex->src_addr)
			*src = tex->key;
	}
	if (getenv("SALSA64_TEXSTAT_V") && tracing())
		fprintf(stderr, "tex: campo %u tarea %u %s key %08x %ux%u fmt %u siz %u tmem %u line %u pal %u "
			"mask %u,%u cm %u,%u sz %u,%u-%u,%u src %06x/%u\n", n64.bus.vi.fields, gfx.tasks + 1,
			seen ? "vista" : "nueva", tex->key, tex->width, tex->height, t->fmt, t->siz, t->tmem, t->line,
			t->pal, t->masks, t->maskt, t->cms, t->cmt, t->uls, t->ult, t->lrs, t->lrt, tex->src_addr,
			tex->src_pitch);
}

static void ts_window(void)
{
	const char *e = getenv("SALSA64_TEXSTAT");
	uint32_t every = e ? (uint32_t)strtoul(e, NULL, 0) : 0;
	if (!every)
		return;
	ts.tasks++;
	if (n64.bus.vi.fields < ts.from_field + every)
		return;
	double n = ts.tasks;
	fprintf(stderr, "texstat campos %u–%u: %u tareas; por tarea %.1f pedidas, %.1f decodificadas (%.1f nuevas, "
		"%.1f vistas antes; %.1f nuevas desde un origen ya visto con otro contenido), %.0f texels, %.1f sin "
		"decodificar (desde un destino); en la ventana %u claves distintas, %.0f KB\n", ts.from_field,
		n64.bus.vi.fields, ts.tasks, ts.req / n, ts.dec / n, ts.dec_new / n, (ts.dec - ts.dec_new) / n,
		ts.dec_same_src / n, ts.dec_texels / n, ts.target / n, ts.distinct, ts.distinct_bytes / 1024.0);
	uint32_t window = ts.window + 1, from = n64.bus.vi.fields;
	memset(&ts, 0, sizeof ts);
	ts.window = window;
	ts.from_field = from;
}

/* Por dibujo (gfx_done): rango de s/t, vértices (SALSA64_GFX_VDUMP=n) y
 * texturas en PPM (SALSA64_GFX_TEXDUMP=DIR). */
static void trace_draw_extra(const struct gfx_draw *d)
{
	float smin = 1e30f, smax = -1e30f, tmin = 1e30f, tmax = -1e30f;
	for (uint32_t i = 0; i < d->h.b; i++) {
		const struct gfx_vertex *v = &gfx_vtx[d->h.a + i];
		smin = v->s < smin ? v->s : smin, smax = v->s > smax ? v->s : smax;
		tmin = v->t < tmin ? v->t : tmin, tmax = v->t > tmax ? v->t : tmax;
	}
	trace_draw++;
	fprintf(stderr, " s %.1f..%.1f t %.1f..%.1f", smin, smax, tmin, tmax);
	const char *vd = getenv("SALSA64_GFX_VDUMP");
	if (vd && (unsigned)strtoul(vd, NULL, 0) == trace_draw)
		for (uint32_t i = 0; i < d->h.b; i++) {
			const struct gfx_vertex *v = &gfx_vtx[d->h.a + i];
			fprintf(stderr, "\n     v%-3u x/w %7.2f y/w %7.2f w %8.2f s %8.2f t %8.2f rgba %3u %3u %3u %3u", i,
				v->w != 0.0f ? v->x / v->w : 0.0f, v->w != 0.0f ? v->y / v->w : 0.0f, v->w, v->s, v->t,
				v->r, v->g, v->b, v->a);
		}
	const char *dir = getenv("SALSA64_GFX_TEXDUMP");
	for (int ti = 0; dir && ti < d->ntex; ti++) {
		const struct gfx_tex *tx = &d->tex[ti];
		if (tx->where == GFX_TEX_TARGET)
			continue;  /* sin texels: se lee de un destino */
		const uint8_t *base = (tx->where == GFX_TEX_CACHE ? gfx_cache : gfx_tex) + tx->offset;
		char name[512];
		snprintf(name, sizeof name, "%s/d%03u-t%d.ppm", dir, trace_draw, ti);
		FILE *f = fopen(name, "wb");
		if (!f)
			continue;
		fprintf(f, "P6\n%u %u\n255\n", tx->width, tx->height * 2u);
		for (unsigned y = 0; y < tx->height; y++)
			for (unsigned x = 0; x < tx->width; x++)
				fwrite(base + y * tx->pitch + 4 * x, 1, 3, f);
		for (unsigned y = 0; y < tx->height; y++)
			for (unsigned x = 0; x < tx->width; x++) {
				uint8_t a = base[y * tx->pitch + 4 * x + 3], px[3] = { a, a, a };
				fwrite(px, 1, 3, f);
			}
		fclose(f);
	}
}
#endif

/* Accesos del CPU a los destinos de render (build -DSALSA64_WATCH_MEM):
 * por cada destino de la tabla del rasterizador, cuántas lecturas y
 * escrituras hubo entre una tarea gráfica y la siguiente, y la primera de
 * cada clase. Se imprime al empezar cada tarea. */
#ifdef SALSA64_WATCH_MEM
static struct {
	uint32_t addr, bytes, width;
	uint32_t reads, writes, first_rd, first_wr;
	uint64_t pc_rd, pc_wr;
} watch_fb[RDP_TARGETS_MAX];
static uint32_t watch_lo = ~0u, watch_hi;

void salsa64_watch_access(uint32_t pa, int write)
{
	if (pa < watch_lo || pa >= watch_hi)
		return;
	for (int i = 0; i < RDP_TARGETS_MAX; i++) {
		if (!watch_fb[i].bytes || pa < watch_fb[i].addr || pa >= watch_fb[i].addr + watch_fb[i].bytes)
			continue;
		if (write) {
			if (!watch_fb[i].writes++) {
				watch_fb[i].first_wr = pa - watch_fb[i].addr;
				watch_fb[i].pc_wr = n64.cpu.cur_pc;
			}
		} else if (!watch_fb[i].reads++) {
			watch_fb[i].first_rd = pa - watch_fb[i].addr;
			watch_fb[i].pc_rd = n64.cpu.cur_pc;
		}
	}
}

static void watch_fb_task(void)
{
	for (int i = 0; i < RDP_TARGETS_MAX; i++) {
		if (watch_fb[i].bytes && (watch_fb[i].reads || watch_fb[i].writes) && trace_fb && tracing())
			fprintf(stderr, "fb: campo %u, antes de la tarea %u: CPU %06x/%u: %u lecturas (+%x, pc %08llx), "
				"%u escrituras (+%x, pc %08llx)\n", n64.bus.vi.fields, gfx.tasks + 1, watch_fb[i].addr,
				watch_fb[i].width, watch_fb[i].reads, watch_fb[i].first_rd,
				(unsigned long long)watch_fb[i].pc_rd, watch_fb[i].writes, watch_fb[i].first_wr,
				(unsigned long long)watch_fb[i].pc_wr);
		watch_fb[i].reads = watch_fb[i].writes = 0;
		uint32_t addr, width, bpp, lh;
		if (raster_target_info(i, &addr, &width, &bpp, &lh)) {
			watch_fb[i].addr = addr;
			watch_fb[i].width = width;
			watch_fb[i].bytes = width * bpp * lh;
		} else {
			watch_fb[i].bytes = 0;
		}
	}
	watch_lo = ~0u;
	watch_hi = 0;
	for (int i = 0; i < RDP_TARGETS_MAX; i++) {
		if (!watch_fb[i].bytes)
			continue;
		if (watch_fb[i].addr < watch_lo)
			watch_lo = watch_fb[i].addr;
		if (watch_fb[i].addr + watch_fb[i].bytes > watch_hi)
			watch_hi = watch_fb[i].addr + watch_fb[i].bytes;
	}
}
#endif

/* DMA a la RDRAM: el rasterizador no pisa con copias viejas lo que llegó
 * después (raster_rdram_written). La tarea en vuelo es gfx.tasks + 1. */
static void rdram_written(void *ctx, uint32_t pa, uint32_t len)
{
	(void)ctx;
	if (gfx.raster)
		raster_rdram_written(pa, len, gfx.tasks + (n64.bus.rsp.busy == SALSA64_TASK_GFX ? 1u : 0u));
}

static void gfx_acquire(void *ctx, struct gfx_job *job)
{
	(void)ctx;
#ifdef SALSA64_WATCH_MEM
	watch_fb_task();
#endif
	job->vtx_ea = (uint64_t)(uintptr_t)gfx_vtx;
	job->vtx_cap = sizeof gfx_vtx;
	job->cmd_ea = (uint64_t)(uintptr_t)gfx_cmd;
	job->cmd_cap = GFX_CMD_DRAWS;
	job->tex_ea = (uint64_t)(uintptr_t)gfx_tex;
	job->tex_cap = sizeof gfx_tex;
	/* Caché de texturas: el rasterizador termina cada tarea en gfx_done, así
	 * que la anterior ya no lee nada. */
	if (!getenv("SALSA64_NO_TEXCACHE")) {
		job->cache_ea = (uint64_t)(uintptr_t)gfx_cache;
		job->cache_cap = sizeof gfx_cache;
	}
	job->task = gfx.tasks + 1;
	job->done_task = gfx.tasks;
	if (job->flags & GFX_JOB_DPC_CONT) {  /* sigue la tarea del tramo anterior (gfx.h) */
		job->task = gfx.tasks;
		job->done_task = gfx.tasks - 1;
	}
	interp_task_begin(job, n64.bus.vi.fields);
	if (job->flags & GFX_JOB_INTERP)
		job->cmd_cap = sizeof gfx_cmd;
	/* Como la PS3: el rasterizador lee de sus imágenes lo que viene de un
	 * destino reciente y decodifica él lo que al final no pueda leer. */
	if (gfx.raster && raster_fbfx >= 1 && !getenv("SALSA64_NO_FBTEX"))
		job->flags |= GFX_JOB_FBTEX;
	if (gfx_affine_chain)
		job->flags |= GFX_JOB_AFFINE_CHAIN;
	if (gfx_no_fog_1cycle)
		job->flags |= GFX_JOB_NO_FOG_1CYCLE;
	if (gfx_no_rsp_clip)
		job->flags |= GFX_JOB_NO_RSP_CLIP;
	if (salsa64_host_env_on("SALSA64_GFX_S2DEX_BG_OLD"))
		job->flags |= GFX_JOB_S2DEX_BG_OLD;
	if (salsa64_host_env_on("SALSA64_NO_YUV"))
		job->flags |= GFX_JOB_NO_YUV;
	if (salsa64_host_env_on("SALSA64_BLEND_OLD"))
		job->flags |= GFX_JOB_BLEND_OLD;
	if (gfx_s2dex_bg_row_old)
		job->flags |= GFX_JOB_S2DEX_BG_ROW_OLD;
	if (gfx_f5_sub_old)
		job->flags |= GFX_JOB_F5_SUB_OLD;
#ifdef SALSA64_GFX_TRACE
	salsa64_gfx_trace_now = trace_gfx_verbose && trace_gfx_left && n64.bus.vi.fields > (trace_from ? trace_from : 500);
	trace_draw = trace_states = 0;
	if (salsa64_gfx_trace_now)
		fprintf(stderr, "== tarea gfx (campo %u)\n", n64.bus.vi.fields);
#endif
}

/* SALSA64_GFX_CUTCHECK (diagnóstico): los vértices que la HLE puso sobre el
 * plano cercano (profundidad 0) deberían coincidir entre triángulos vecinos
 * que comparten la arista cortada; se agrupan por posición en pantalla y se
 * mide cuánto se separan dentro de cada grupo (una grieta sería ≥ 1 px). */
static void cut_check(const struct gfx_result *res)
{
	static float px[8192], py[8192];
	unsigned n = 0;
	for (uint32_t off = 0; off + sizeof(struct gfx_cmd) <= res->cmd_bytes;) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(gfx_cmd + off);
		if (c->op == GFX_OP_END || !c->size)
			break;
		if (c->op == GFX_OP_DRAW) {
			const struct gfx_draw *d = (const struct gfx_draw *)c;
			for (uint32_t i = 0; i < d->h.b && n < 8192; i++) {
				const struct gfx_vertex *v = &gfx_vtx[d->h.a + i];
				if (d->prim_kind == 0 && v->w > 0.0f && fabsf(v->z / v->w) < 1e-5f) {
					px[n] = v->x / v->w;
					py[n] = v->y / v->w;
					n++;
				}
			}
		}
		off += c->size;
	}
	if (!n)
		return;
	unsigned groups = 0, orphans = 0;
	float worst = 0.0f;
	static bool used[8192];
	memset(used, 0, n);
	for (unsigned i = 0; i < n; i++) {
		if (used[i])
			continue;
		unsigned members = 1;
		float far = 0.0f;
		for (unsigned j = i + 1; j < n; j++) {
			float dx = px[j] - px[i], dy = py[j] - py[i];
			if (fabsf(dx) < 0.5f && fabsf(dy) < 0.5f) {
				used[j] = true;
				members++;
				float dd = sqrtf(dx * dx + dy * dy);
				far = dd > far ? dd : far;
			}
		}
		groups++;
		orphans += members == 1;
		worst = far > worst ? far : worst;
	}
	fprintf(stderr, "cutcheck campo %u: %u vértices en el plano cercano, %u grupos (%u sin pareja), "
		"separación máx. %.6f px\n", n64.bus.vi.fields, n, groups, orphans, worst);
}

/* SALSA64_GFX_DUMP_TASKS=N o N-M: el volcado de la HLE (gfx_dump_header, el
 * de salsa64_rdp_dump en la PS3) de esas tareas, numeradas desde 1 como en
 * rdp.c y en el perfilado de la PS3, en SALSA64_GFX_DUMP_DIR (o .) como
 * tarea-N.bin, en el orden de bytes de la PS3; se ven con tools/gfxdump.py y se repiten en RPCS3 con
 * --replay. */
static void task_dump(uint32_t task, const struct gfx_result *res)
{
	static int parsed;
	static uint32_t lo, hi;
	if (!parsed) {
		parsed = 1;
		const char *e = getenv("SALSA64_GFX_DUMP_TASKS");
		if (e && sscanf(e, "%u-%u", &lo, &hi) < 2)
			hi = lo;
	}
	if (!lo || task < lo || task > hi)
		return;
	static uint32_t wsrc[2 * GFX_DUMP_MAX_TEX(sizeof gfx_cmd)];
	static const struct gfx_tex *wtgt[GFX_DUMP_MAX_TEX(sizeof gfx_cmd)];
	struct gfx_dump_work w = { wsrc, wsrc + GFX_DUMP_MAX_TEX(sizeof gfx_cmd), wtgt, GFX_DUMP_MAX_TEX(sizeof gfx_cmd) };
	struct gfx_dump_src src = { gfx_cmd, res->cmd_bytes, (const uint8_t *)gfx_vtx, res->vtx_bytes, gfx_tex,
				    res->tex_bytes, gfx_cache, sizeof gfx_cache, rdram, SALSA64_RDRAM_MAX };
	uint32_t size = gfx_dump_measure(&src, &w);
	uint8_t *buf = size ? malloc(size) : NULL;
	uint32_t got = buf ? gfx_dump_build(&src, &w, buf, size) : 0;
	if (got)
		ares_dump_to_be(buf, got);
	const char *dir = getenv("SALSA64_GFX_DUMP_DIR");
	char path[512];
	snprintf(path, sizeof path, "%s/tarea-%u.bin", dir ? dir : ".", task);
	FILE *f = got ? fopen(path, "wb") : NULL;
	bool ok = f && fwrite(buf, 1, got, f) == got;
	if (f && fclose(f) != 0)
		ok = false;
	fprintf(stderr, "salsa64-run: volcado de la tarea %u (campo %u, %u comandos, %u triángulos): %s %s\n", task,
		n64.bus.vi.fields, res->commands, res->triangles, ok ? "escrito en" : "FALLÓ", path);
	free(buf);
}

/* --stop-gfx N:ARCHIVO: al terminar la N-ésima tarea gráfica, guarda en PPM la
 * última color image que dibujó y termina. Compara la HLE con la LLE sobre
 * la misma tarea (el mismo estado guardado, --state-load): por campos, el
 * VI muestra imágenes a medio dibujar con el RSP a bajo nivel. */
static uint32_t stop_gfx_n, stop_gfx_seen, stop_gfx_target;
static const char *stop_gfx_path;
static bool stop_gfx_hit;
static bool gfx_dump_target(const char *path, uint32_t addr);

static void stop_gfx_task(const struct gfx_job *job, const struct gfx_result *res)
{
	(void)job;
	if (stop_gfx_hit)
		return;
	/* Con la LLE cada tramo del RDP es un trabajo y la tarea N termina cuando
	 * arranca la N + 1: el primer tramo de esta ya no dibuja en la imagen de
	 * la anterior. Con la HLE, un trabajo por tarea. */
	bool lle = n64.bus.rsp.lle_mode != SALSA64_LLE_OFF && n64.bus.rsp.stats.lle_gfx;
	if (lle && n64.bus.rsp.stats.lle_gfx > stop_gfx_n) {
		gfx_dump_target(stop_gfx_path, stop_gfx_target);
		stop_gfx_hit = true;
		return;
	}
	for (uint32_t off = 0; off + sizeof(struct gfx_cmd) <= res->cmd_bytes;) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(gfx_cmd + off);
		if (c->op == GFX_OP_END || c->size < sizeof *c)
			break;
		if (c->op == GFX_OP_TARGET)
			stop_gfx_target = c->a;
		off += c->size;
	}
	if (!lle && ++stop_gfx_seen == stop_gfx_n) {
		gfx_dump_target(stop_gfx_path, stop_gfx_target);
		stop_gfx_hit = true;
	}
}

static void gfx_done(void *ctx, const struct gfx_job *job, const struct gfx_result *res)
{
	(void)ctx;
	struct gfx_result *s = &gfx.sum;
	task_dump(gfx.tasks + 1, res);
	if (getenv("SALSA64_GFX_CUTCHECK") && n64.bus.vi.fields >= trace_from)
		cut_check(res);
	hp_task(res);
	if (!(job->flags & GFX_JOB_DPC_CONT))
		gfx.tasks++;
#ifdef SALSA64_GFX_TRACE
	ts_window();
#endif
	s->flags |= res->flags;
	s->commands += res->commands;
	s->triangles += res->triangles;
	s->rects += res->rects;
	s->draws += res->draws;
	s->textures += res->textures;
	s->tex_reused += res->tex_reused;
	s->tex_old += res->tex_old;
	gfx.tex_arena += res->tex_arena;
	gfx.tex_evicted += res->tex_evicted;
	s->tex_target += res->tex_target;
	s->loads += res->loads;
	s->unknown_cmds += res->unknown_cmds;
	if (res->unknown_cmds)
		s->last_unknown = res->last_unknown;
	s->approx_cc += res->approx_cc;
	s->dl_errors += res->dl_errors;
	s->skipped += res->skipped;
	s->bg += res->bg;
	gchk_lle_job(job, res);
	if (gfx.raster) {
		raster_field = n64.bus.vi.fields;
		raster_trace_fb = trace_fb && tracing();
		interp_before_raster();
		raster_task(job, res);
		interp_after_raster(job, res, n64.bus.vi.fields);
		if (gchk_self)
			gchk_task_end();
		if (stop_gfx_n)
			stop_gfx_task(job, res);
	}
	if (trace_gfx_left && n64.bus.vi.fields > (trace_from ? trace_from : 500)) {
		trace_gfx_left--;
		fprintf(stderr, "tarea gfx (campo %u):", n64.bus.vi.fields);
		uint32_t off = 0;
		while (off + sizeof(struct gfx_cmd) <= res->cmd_bytes) {
			const struct gfx_cmd *c = (const struct gfx_cmd *)(gfx_cmd + off);
			if (c->op == GFX_OP_END || !c->size)
				break;
			if (c->op == GFX_OP_TARGET)
				fprintf(stderr, " TARGET %06x/%u", c->a, c->b);
			else if (c->op == GFX_OP_ZCLEAR)
				fprintf(stderr, " ZCLEAR %06x", c->a);
			else if (c->op == GFX_OP_DRAW) {
				const struct gfx_draw *d = (const struct gfx_draw *)c;
				if (trace_gfx_verbose) {
					float zmin = 1e30f, zmax = -1e30f, wmin = 1e30f, wmax = -1e30f;
					float xmin = 1e30f, xmax = -1e30f, ymin = 1e30f, ymax = -1e30f;
					for (uint32_t i = 0; i < d->h.b; i++) {
						const struct gfx_vertex *v = &gfx_vtx[d->h.a + i];
						float z = v->w != 0.0f ? v->z / v->w : 0.0f;
						float x = v->w != 0.0f ? v->x / v->w : 0.0f, y = v->w != 0.0f ? v->y / v->w : 0.0f;
						zmin = z < zmin ? z : zmin, zmax = z > zmax ? z : zmax;
						wmin = v->w < wmin ? v->w : wmin, wmax = v->w > wmax ? v->w : wmax;
						xmin = x < xmin ? x : xmin, xmax = x > xmax ? x : xmax;
						ymin = y < ymin ? y : ymin, ymax = y > ymax ? y : ymax;
					}
					fprintf(stderr, "\n  D%-4u kind %u cyc %u ntex %u depth %u blend %u cull %u atest %u/%u fog %u "
						"z/w %.3f..%.3f w %.1f..%.1f x/w %.1f..%.1f y/w %.1f..%.1f sc %.0f,%.0f-%.0f,%.0f",
						d->h.b / 3, d->prim_kind, d->cycle, d->ntex, d->depth, d->blend, d->cull, d->atest,
						d->aref, d->fog, zmin, zmax, wmin, wmax, xmin, xmax, ymin, ymax, d->scissor[0] / 4.0,
						d->scissor[1] / 4.0, d->scissor[2] / 4.0, d->scissor[3] / 4.0);
#ifdef SALSA64_GFX_TRACE
					trace_draw_extra(d);
#endif
					if (d->blend) {
						static const char *const tn[] = { "T0", "T0a", "T1", "T1a", "1" };
						fprintf(stderr, "\n        alfa:");
						for (int t = 0; t < GFX_K_TERMS; t++)
							for (int k = 0; k < 3; k++)
								if (d->k[t][k][3] != 0.0f)
									fprintf(stderr, " %+.3f·%s·%s", d->k[t][k][3], tn[t],
										k == 0 ? "1" : k == 1 ? "C" : "Ca");
						for (int t = 0; t < d->ntex; t++)
							fprintf(stderr, " tex%d %ux%u pitch %u flags %x xf %.4f %.4f %.4f %.4f", t,
								d->tex[t].width, d->tex[t].height, d->tex[t].pitch, d->tex[t].flags,
								d->tex[t].xf[0], d->tex[t].xf[1], d->tex[t].xf[2], d->tex[t].xf[3]);
						const uint8_t *a = &gfx_vtx[d->h.a].r;
						fprintf(stderr, " v0 rgba %u %u %u %u", a[0], a[1], a[2], a[3]);
					}
				} else {
					fprintf(stderr, " D%u", d->h.b / 3);
				}
			}
			off += c->size;
		}
		fprintf(stderr, "\n");
	}
}

#ifdef SALSA64_DEBUG_PI
/* DMA cartucho → RDRAM (pi.c), desde --trace-from. */
void salsa64_debug_pi(uint32_t cart, uint32_t dram, uint32_t len)
{
	static unsigned n;
	if (!tracing() || n++ >= 400)
		return;
	fprintf(stderr, "PI campo %u ciclo %llu: cart %08x -> rdram %06x, %u bytes (pc %08llx)\n", n64.bus.vi.fields,
		(unsigned long long)n64.cpu.cycles, cart, dram, len, (unsigned long long)n64.cpu.cur_pc);
}
#endif

#ifdef SALSA64_DEBUG_FLASH
/* FlashRAM (flashram.c), desde --trace-from: comandos, E/S y DMA. */
void salsa64_debug_flash(char kind, unsigned mode, uint32_t off, uint32_t value)
{
	static unsigned n;
	if (!tracing() || n++ >= 4000)
		return;
	fprintf(stderr, "flash campo %u: %c modo %u %05x %08x (pc %08llx)\n", n64.bus.vi.fields, kind, mode, off, value,
		(unsigned long long)n64.cpu.cur_pc);
}
#endif

#ifdef SALSA64_DEBUG_DP
/* Registros del DP (0 lectura, 1 escritura) y escrituras de SP_STATUS (2),
 * desde --trace-from. */
void salsa64_debug_dp(int kind, uint32_t reg, uint32_t v)
{
	static unsigned n;
	if (!tracing() || n++ >= 400)
		return;
	fprintf(stderr, "%s campo %u ciclo %llu: reg %02x %s %08x (pc %08llx)\n", kind == 3 ? "DP-IRQ" : kind == 2 ? "SP" : "DP",
		n64.bus.vi.fields, (unsigned long long)n64.cpu.cycles, reg, kind == 0 ? "->" : "<-", v,
		(unsigned long long)n64.cpu.cur_pc);
}
#endif

#ifdef SALSA64_DEBUG_SPMEM
void salsa64_debug_spmem(uint32_t reg, uint32_t v)
{
	static unsigned n;
	if (n++ < 200)
		fprintf(stderr, "lee SP mem %04x = %08x (pc %08llx, ciclo %llu)\n", reg, v,
			(unsigned long long)n64.cpu.cur_pc, (unsigned long long)n64.cpu.cycles);
}
#endif

static unsigned trace_rsp_left, trace_vi_left, trace_audio_left;

/* --trace-audio: los opcodes de la lista de audio de una tarea (8 bytes por
 * comando), para comparar lo que el juego pide en escenas distintas. */
static void trace_alist(const struct salsa64_task *t)
{
	uint32_t a = t->data_ptr & 0x7FFFFF, n = t->data_size / 8;
	fprintf(stderr, "alist (campo %u, %u comandos):", n64.bus.vi.fields, n);
	for (uint32_t i = 0; i < n && a + 8 * i + 8 <= n64.bus.rdram_size; i++) {
		uint32_t w0 = salsa64_rd32(n64.bus.rdram + a + 8 * i), w1 = salsa64_rd32(n64.bus.rdram + a + 8 * i + 4);
		fprintf(stderr, " %02x", w0 >> 24);
		if (getenv("SALSA64_ALIST_VERBOSE"))
			fprintf(stderr, ":%06x:%08x", w0 & 0xFFFFFF, w1);
	}
	fprintf(stderr, "\n");
}

/* Muestreo del PC al final de cada bloque del bucle principal, por segmento:
 * cuánto del tiempo corre código mapeado por la TLB (kuseg, kseg2/3). */
enum { SEG_KSEG0, SEG_KSEG1, SEG_TLB, SEG_COUNT };
static uint64_t pc_samples[SEG_COUNT];

static void pc_sample(uint64_t pc)
{
	uint32_t top = (uint32_t)pc >> 29;
	pc_samples[top == 4 ? SEG_KSEG0 : top == 5 ? SEG_KSEG1 : SEG_TLB]++;
}

/* --lle-check-audio: cada tarea de audio que corre en el microcódigo real
 * se corre también en la HLE sobre una copia de la RDRAM; cuando el RSP
 * termina (en el siguiente arranque, o al final) se comparan los tramos
 * que la HLE escribió. Encuentra la primera divergencia del LLE.
 *
 * Hace falta compilar con -DAUD_MAX_RANGES=64 para tareas con muchos tramos
 * (el resultado de spukit solo admite 10). */
#define BIS_MAGIC 0x42495331u
#define BIS_RANGES 1024

struct bis_res {            /* lo que el hijo de SALSA64_LLE_BISECT cuenta al padre */
	uint32_t magic, k;
	uint32_t rd_ranges;     /* tramos de la RDRAM distintos */
	uint32_t rd_addr, rd_off;  /* el primero: dirección del tramo y primer byte distinto */
	uint32_t dm_bytes;      /* bytes de la DMEM distintos dentro de la ventana */
	uint32_t dm_first, dm_last;
	uint64_t dm_blocks;     /* bloques de 64 bytes de la DMEM con alguna diferencia */
	uint8_t dm_lle[16], dm_hle[16];  /* desde dm_first */
};

static struct {
	bool on, pending;
	uint8_t *rdram;         /* copia de la RDRAM con la salida de la HLE */
	uint8_t *pre;           /* la RDRAM antes de la tarea (solo SALSA64_LLE_DUMP_TASK) */
	struct aud_result res;
	uint32_t task, tasks, bad, bytes;
	uint32_t hist[5];       /* muestras de 16 bits distintas: ±1, ±2, ±3–4, ±5–16, más */
	int reports;
	/* Variables de entorno (se leen una vez):
	 *   SALSA64_LLE_VERBOSE=1       una línea por tarea: muestras distintas y diferencia máxima
	 *   SALSA64_LLE_DIFFS=1         "T n" al empezar cada tarea y una línea "D" por palabra distinta
	 *   SALSA64_LLE_DUMP_TASK=N     de esa tarea: lo que escribió (antes | lle | hle) y su lista
	 *   SALSA64_LLE_DUMP_DMEM=ruta  con la anterior, la DMEM del RSP al terminar
	 *   SALSA64_LLE_DUMP_RANGE=D:N  con DUMP_TASK, además N bytes de la RDRAM desde D (hex), antes y
	 *                               después del microcódigo real (formato de su estado, por ejemplo)
	 *   SALSA64_LLE_BISECT=A[-B][,…] las tareas A a B (o solo A; una lista separada por comas): para cada una, con k = 1, 2, …
	 *                               comandos, un proceso hijo corre la tarea cortada a k comandos en
	 *                               el microcódigo y en la HLE y compara la DMEM (ventana
	 *                               SALSA64_LLE_DMWIN=lo-hi, hex; por omisión la de los búferes de
	 *                               cada ABI) y los tramos de la RDRAM; el primer
	 *                               k distinto es el comando donde se separan
	 *   SALSA64_LLE_BISECT_STEP=n   k de n en n (por omisión 1)
	 *   SALSA64_LLE_BISECT_NOP=1    en vez de cortar data_size, poner a cero los comandos que sobran
	 *   SALSA64_LLE_BISECT_FROM=k   empezar en k comandos (el microcódigo de Zelda lee la lista por
	 *                               bloques y con pocos comandos no se deja cortar)
	 *   SALSA64_LLE_POKE=D=V[,…]    en el hijo de la bisección, escribir V (hex; 16 o 32 bits según los
	 *                               dígitos) en la RDRAM D de los dos lados antes de la tarea
	 *   SALSA64_LLE_SNAP=P[:N]      al arrancar la tarea N (1): la RDRAM en P.ram y la DMEM/IMEM
	 *                               (8 KB, con la cabecera de la tarea y el arranque de libultra,
	 *                               y el SP_PC con el que arrancó) en P.mem: son del juego, fuera del repositorio */
	bool env_read, verbose, diffs;
	uint32_t dump_task;
	const char *dump_dmem;
	uint32_t dump_range_addr, dump_range_len;
	const char *snap;
	uint32_t snap_task;
	uint32_t bis_n, bis_range[BIS_RANGES][2], bis_step, bis_from, dm_lo, dm_hi;
	enum salsa64_abi cur_abi;   /* ABI de la tarea en curso (para lo que la HLE no emula en la DMEM) */
	bool dm_env;
	bool boot_pending;  /* una tarea de audio espera a que arranque el microcódigo (shadow_lle_start) */
	struct salsa64_task boot_task;
	/* Estados de ENVMIXER de la tarea (aud_dbg_env). */
	uint32_t env_n, env_addr[AUD_DBG_ENV_MAX];
	/* En el hijo de la bisección: cuántos comandos corre y el tubo de vuelta. */
	uint32_t child_k;
	int child_fd;
	uint8_t lle_dm0[LLE_MEM_SIZE], hle_dm0[AUD_DMEM_SIZE + AUD_DMEM_SLACK];
	/* Bisección: por ABI y opcode del comando donde se separan (128 = nada: solo difiere
	 * al terminar o no se reprodujo). */
	uint32_t bis_hist[SALSA64_ABI_COUNT][129], bis_tasks;
	uint32_t abi_tasks[SALSA64_ABI_COUNT], abi_bad[SALSA64_ABI_COUNT];   /* tareas comparadas y distintas por ABI */
} shadow;

static void shadow_env(void)
{
	if (shadow.env_read)
		return;
	shadow.env_read = true;
	shadow.verbose = getenv("SALSA64_LLE_VERBOSE") != NULL;
	shadow.diffs = getenv("SALSA64_LLE_DIFFS") != NULL;
	shadow.dump_task = getenv("SALSA64_LLE_DUMP_TASK") ? (uint32_t)atoi(getenv("SALSA64_LLE_DUMP_TASK")) : 0;
	shadow.dump_dmem = getenv("SALSA64_LLE_DUMP_DMEM");
	if (getenv("SALSA64_LLE_DUMP_RANGE"))
		sscanf(getenv("SALSA64_LLE_DUMP_RANGE"), "%x:%x", &shadow.dump_range_addr, &shadow.dump_range_len);
	shadow.bis_step = getenv("SALSA64_LLE_BISECT_STEP") ? (uint32_t)atoi(getenv("SALSA64_LLE_BISECT_STEP")) : 1;
	if (shadow.bis_step == 0)
		shadow.bis_step = 1;
	shadow.bis_from = getenv("SALSA64_LLE_BISECT_FROM") ? (uint32_t)atoi(getenv("SALSA64_LLE_BISECT_FROM")) : 0;
	if (getenv("SALSA64_LLE_BISECT")) {
		/* "5", "5-9" o una lista "5,9-12,40". */
		const char *p = getenv("SALSA64_LLE_BISECT");
		while (*p && shadow.bis_n < BIS_RANGES) {
			char *end;
			unsigned a = (unsigned)strtoul(p, &end, 10), b = a;
			if (end == p)
				break;
			if (*end == '-')
				b = (unsigned)strtoul(end + 1, &end, 10);
			shadow.bis_range[shadow.bis_n][0] = a;
			shadow.bis_range[shadow.bis_n++][1] = b;
			p = *end == ',' ? end + 1 : end;
		}
	}
	shadow.snap = getenv("SALSA64_LLE_SNAP");
	shadow.snap_task = 1;
	if (shadow.snap && strchr(shadow.snap, ':'))
		shadow.snap_task = (uint32_t)atoi(strchr(shadow.snap, ':') + 1);
	shadow.dm_env = getenv("SALSA64_LLE_DMWIN") && sscanf(getenv("SALSA64_LLE_DMWIN"), "%x-%x", &shadow.dm_lo, &shadow.dm_hi) == 2;
}

/* El hijo de la bisección: compara la DMEM (en la ventana, los bytes que algún lado escribió) y
 * los tramos de la RDRAM, se lo cuenta al padre y termina. */
static void shadow_child_report(void)
{
	struct bis_res r;
	memset(&r, 0, sizeof r);
	r.magic = BIS_MAGIC;
	r.k = shadow.child_k;
	for (uint32_t i = 0; i < shadow.res.nranges && i < AUD_MAX_RANGES; i++) {
		uint32_t a = shadow.res.range_addr[i] & 0x7FFFFF, n = shadow.res.range_len[i];
		if (n > n64.bus.rdram_size - a)
			n = n64.bus.rdram_size - a;
		uint32_t k = 0;
		while (k < n && n64.bus.rdram[a + k] == shadow.rdram[a + k])
			k++;
		if (k < n) {
			if (!r.rd_ranges) {
				r.rd_addr = a;
				r.rd_off = k;
			}
			r.rd_ranges++;
		}
	}
	const uint8_t *l = n64.bus.sp_mem, *h = aud_state.dmem;
	uint32_t hi = shadow.dm_hi < LLE_MEM_SIZE ? shadow.dm_hi : LLE_MEM_SIZE;
	for (uint32_t b = shadow.dm_lo; b < hi; b++) {
		/* Un lado que no tocó el byte y otro que sí tampoco coinciden, pero un byte que ninguno
		 * escribió no cuenta (la DMEM de partida es distinta: la del microcódigo tiene su
		 * propio trabajo). */
		if (l[b] == h[b] || (l[b] == shadow.lle_dm0[b] && h[b] == shadow.hle_dm0[b]))
			continue;
		/* El POLEF de los naudio con MP3 guarda, 16 bytes antes de su búfer, un registro vectorial tal
		 * como lo dejó el comando anterior (ocho bytes de basura; abi.c NA_FILTER): no se compara. */
		if ((shadow.cur_abi == SALSA64_ABI_NAUDIO_MP3 || shadow.cur_abi == SALSA64_ABI_NAUDIO_CBFD) &&
		    ((b >= 0x4F0 && b < 0x4F8) || (b >= 0x660 && b < 0x668)))
			continue;
		if (!r.dm_bytes) {
			r.dm_first = b;
			for (unsigned j = 0; j < 16 && b + j < hi; j++) {
				r.dm_lle[j] = l[b + j];
				r.dm_hle[j] = h[b + j];
			}
		}
		r.dm_last = b;
		r.dm_blocks |= 1ull << (b >> 6);
		r.dm_bytes++;
	}
	/* SALSA64_LLE_SHOWK=k:dirección:bytes — en el hijo de k comandos, la DMEM de los dos (hex),
	 * y la de antes de la tarea. */
	{
		unsigned k = 0, a = 0, n = 0;
		const char *e = getenv("SALSA64_LLE_SHOWK");
		if (e && sscanf(e, "%u:%x:%x", &k, &a, &n) == 3 && k == shadow.child_k) {
			for (unsigned j = 0; j < n && a + j + 2 <= LLE_MEM_SIZE; j += 2)
				fprintf(stderr, "SHOWK %03x: antes lle %04x hle %04x | lle %04x hle %04x\n", a + j,
					salsa64_rd16(shadow.lle_dm0 + a + j), salsa64_rd16(shadow.hle_dm0 + a + j),
					salsa64_rd16(n64.bus.sp_mem + a + j),
					/* 0x800–0xFFF de la HLE: la memoria de trabajo del MP3 */
					a + j >= AUD_MP3_BASE ? salsa64_rd16(aud_state.mp3 + a + j - AUD_MP3_BASE) : salsa64_rd16(h + a + j));
		}
	}
	/* SALSA64_LLE_SHOWR=k:dirección:bytes — lo mismo con la RDRAM (la del microcódigo y la de la HLE). */
	{
		unsigned k = 0, a = 0, n = 0;
		const char *e = getenv("SALSA64_LLE_SHOWR");
		if (e && sscanf(e, "%u:%x:%x", &k, &a, &n) == 3 && k == shadow.child_k) {
			for (unsigned j = 0; j < n && a + j + 2 <= n64.bus.rdram_size; j += 2)
				fprintf(stderr, "SHOWR %06x: lle %04x hle %04x\n", a + j, salsa64_rd16(n64.bus.rdram + a + j),
					salsa64_rd16(shadow.rdram + a + j));
		}
	}
	ssize_t w = write(shadow.child_fd, &r, sizeof r);
	(void)w;
	_exit(0);
}

/* Sin la parte fraccionaria de los volúmenes ya recortados al destino (audio.h): se copia del
 * microcódigo a la copia de la HLE antes de comparar. */
static void shadow_mask_env(void)
{
	for (uint32_t e = 0; e < shadow.env_n; e++) {
		uint32_t s0 = shadow.env_addr[e];
		for (unsigned c = 0; c < 2; c++)
			for (unsigned l = 0; l < 8; l++) {
				uint32_t lo = s0 + 0x20 * c + 0x10 + 2 * l;
				if (lo + 2 <= n64.bus.rdram_size &&
				    salsa64_rd16(n64.bus.rdram + s0 + 0x20 * c + 2 * l) == salsa64_rd16(n64.bus.rdram + s0 + 0x40 + 6 * c))
					memcpy(shadow.rdram + lo, n64.bus.rdram + lo, 2);
			}
	}
}

static void shadow_compare(void)
{
	shadow.pending = false;
	shadow_env();
	shadow_mask_env();
	if (shadow.child_k)
		shadow_child_report();
	if (shadow.dump_task == shadow.task && shadow.dump_dmem) {
		FILE *f = fopen(shadow.dump_dmem, "wb");
		if (f) {
			fwrite(n64.bus.sp_mem, 1, 8192, f);
			fclose(f);
		}
	}
	if (shadow.dump_task == shadow.task && shadow.dump_range_len && shadow.pre) {
		uint32_t a = shadow.dump_range_addr & 0x7FFFFF, n = shadow.dump_range_len;
		if (n > n64.bus.rdram_size - a)
			n = n64.bus.rdram_size - a;
		fprintf(stderr, "lle-check: tarea %u, RDRAM [%06x, %u bytes] (antes | lle | hle):\n", shadow.task, a, n);
		for (uint32_t j = 0; j + 4 <= n; j += 4)
			fprintf(stderr, "  +%04x %08x %08x %08x\n", j, salsa64_rd32(shadow.pre + a + j),
				salsa64_rd32(n64.bus.rdram + a + j), salsa64_rd32(shadow.rdram + a + j));
	}
	bool bad = false;
	uint32_t nd = 0, dmax = 0;  /* muestras de 16 bits distintas y la mayor diferencia (SALSA64_LLE_VERBOSE) */
	for (uint32_t i = 0; i < shadow.res.nranges && i < AUD_MAX_RANGES; i++) {
		uint32_t a = shadow.res.range_addr[i] & 0x7FFFFF, n = shadow.res.range_len[i];
		if (n > n64.bus.rdram_size - a)
			n = n64.bus.rdram_size - a;
		shadow.bytes += n;
		const uint8_t *x = n64.bus.rdram + a, *y = shadow.rdram + a;
		for (uint32_t j = 0; j + 2 <= n; j += 2) {
			int d = (int16_t)salsa64_rd16(x + j) - (int16_t)salsa64_rd16(y + j);
			d = d < 0 ? -d : d;
			if (d) {
				shadow.hist[d == 1 ? 0 : d == 2 ? 1 : d <= 4 ? 2 : d <= 16 ? 3 : 4]++;
				nd++;
				dmax = (uint32_t)d > dmax ? (uint32_t)d : dmax;
			}
		}
		uint32_t k = 0;
		while (k < n && x[k] == y[k])
			k++;
		if (k == n)
			continue;
		bad = true;
		if (shadow.diffs) {
			/* Una línea por palabra distinta: D tarea tramo dirección +desplazamiento lle hle. */
			for (uint32_t j = 0; j + 4 <= n; j += 4) {
				uint32_t xl = salsa64_rd32(x + j), yh = salsa64_rd32(y + j);
				if (xl != yh)
					fprintf(stderr, "D %u %u %06x +%04x %08x %08x\n", shadow.task, i, a, j, xl, yh);
			}
		}
		/* Con SALSA64_LLE_DUMP_TASK, las palabras de 32 bits del tramo que difieren. */
		if (shadow.dump_task == shadow.task) {
			fprintf(stderr, "lle-check: tarea %u, tramo %u [%06x, %u bytes] (antes | lle | hle):\n", shadow.task, i, a, n);
			for (uint32_t j = 0; j + 4 <= n; j += 4) {
				uint32_t xl = salsa64_rd32(x + j), yh = salsa64_rd32(y + j);
				if (xl != yh)
					fprintf(stderr, "  +%04x %08x %08x %08x *\n", j, shadow.pre ? salsa64_rd32(shadow.pre + a + j) : 0, xl,
						yh);
			}
		}
		if (shadow.reports < 8) {
			shadow.reports++;
			uint32_t d = 0;
			for (uint32_t j = 0; j < n; j++)
				d += x[j] != y[j];
			fprintf(stderr, "lle-check: tarea de audio %u, tramo %u [%06x, %u bytes]: %u bytes distintos desde "
				"+%u: lle %02x%02x%02x%02x %02x%02x%02x%02x, hle %02x%02x%02x%02x %02x%02x%02x%02x\n",
				shadow.task, i, a, n, d, k, x[k], x[k + 1], x[k + 2], x[k + 3], x[k + 4], x[k + 5], x[k + 6],
				x[k + 7], y[k], y[k + 1], y[k + 2], y[k + 3], y[k + 4], y[k + 5], y[k + 6], y[k + 7]);
		}
	}
	shadow.bad += bad;
	shadow.abi_tasks[shadow.cur_abi]++;
	shadow.abi_bad[shadow.cur_abi] += bad;
	if (shadow.verbose)
		fprintf(stderr, "lle-check: tarea %u: %u muestras distintas, diferencia máxima %u%s\n", shadow.task, nd, dmax,
			bad ? " DISTINTA" : "");
}

/* SALSA64_LLE_BISECT: para la tarea en curso (aún sin arrancar en el microcódigo), un proceso
 * hijo por k = 1, 2, … comandos la corre cortada a k comandos —el microcódigo lee data_size de
 * la cabecera de la tarea en la DMEM— y compara con la HLE. En el hijo vuelve true (con
 * `job` y la cabecera ya cortadas) y el hijo sigue emulando hasta que el microcódigo termina
 * (shadow_compare → shadow_child_report); en el padre vuelve false cuando ya imprimió dónde se
 * separan la HLE y el microcódigo: el primer k cuyo resultado difiere. */
static bool shadow_bisect(struct aud_job *job, enum salsa64_abi abi)
{
	uint32_t ncmd = job->alist_size / 8, tasks = shadow.tasks + 1;
	struct bis_res first;
	bool have = false;
	memset(&first, 0, sizeof first);
	for (uint32_t k = shadow.bis_from ? shadow.bis_from : 1; k <= ncmd && !have; k += shadow.bis_step) {
		int fd[2];
		if (pipe(fd) != 0)
			return false;
		fflush(NULL);
		pid_t pid = fork();
		if (pid < 0) {
			close(fd[0]);
			close(fd[1]);
			return false;
		}
		if (pid == 0) {
			close(fd[0]);
			alarm(120);
			shadow.child_k = k;
			shadow.child_fd = fd[1];
			job->alist_size = 8 * k;
			if (getenv("SALSA64_LLE_BISECT_NOP")) {
				/* El microcódigo de Zelda lee la lista por bloques y no deja cortarla: se pisan con ceros
				 * los comandos que sobran y se le dice que hay todos. */
				uint32_t a = job->alist & 0x7FFFF8;
				if (a < n64.bus.rdram_size && 8 * ncmd <= n64.bus.rdram_size - a)
					memset(n64.bus.rdram + a + 8 * k, 0, 8 * (ncmd - k));
			} else {
				salsa64_wr32(n64.bus.sp_mem + SALSA64_TASK_DMEM + 0x34, 8 * k);
			}
			/* SALSA64_LLE_POKE=dir=valor[,…] (hex; 16 o 32 bits según los dígitos): escribir la RDRAM
			 * de los dos lados antes de la tarea, para probar variantes de la lista o de sus
			 * tablas sin volver a correr el juego (experimentos sobre un comando). */
			for (const char *p = getenv("SALSA64_LLE_POKE"); p && *p;) {
				char *end;
				uint32_t a = (uint32_t)strtoul(p, &end, 16) & 0x7FFFFF;
				if (*end != '=')
					break;
				const char *v = end + 1;
				uint32_t val = (uint32_t)strtoul(v, &end, 16);
				unsigned digits = (unsigned)(end - v);
				if (a + 4 <= n64.bus.rdram_size) {
					if (digits > 4) {
						salsa64_wr32(n64.bus.rdram + a, val);
						salsa64_wr32(shadow.rdram + a, val);
					} else {
						salsa64_wr16(n64.bus.rdram + a, (uint16_t)val);
						salsa64_wr16(shadow.rdram + a, (uint16_t)val);
					}
				}
				p = *end == ',' ? end + 1 : end;
			}
			return true;
		}
		close(fd[1]);
		struct bis_res r;
		ssize_t n = read(fd[0], &r, sizeof r);
		close(fd[0]);
		int status = 0;
		waitpid(pid, &status, 0);
		if (n != (ssize_t)sizeof r || r.magic != BIS_MAGIC) {
			fprintf(stderr, "bisect: tarea %u, k=%u: el hijo no contestó (estado %d)\n", tasks, k, status);
			continue;
		}
		if (r.rd_ranges || r.dm_bytes) {
			first = r;
			have = true;
		}
	}
	shadow.bis_tasks++;
	if (!have) {
		shadow.bis_hist[abi][128]++;
		fprintf(stderr, "bisect: tarea %u (%s, %u comandos): sin diferencias en ningún k\n", tasks, salsa64_abi_name(abi),
			ncmd);
		return false;
	}
	uint32_t ca = (job->alist + 8 * (first.k - 1)) & 0x7FFFFF;
	uint32_t w1 = salsa64_rd32(n64.bus.rdram + ca), w2 = salsa64_rd32(n64.bus.rdram + ca + 4);
	shadow.bis_hist[abi][(w1 >> 24) & 0x7F]++;
	fprintf(stderr, "bisect: tarea %u (%s, %u comandos): primer comando distinto %u de %u: %08x %08x (op %02x)", tasks,
		salsa64_abi_name(abi), ncmd, first.k - 1, ncmd, w1, w2, (w1 >> 24) & 0x7F);
	if (first.rd_ranges)
		fprintf(stderr, "; RDRAM: %u tramos distintos (el primero %06x +%u)", first.rd_ranges, first.rd_addr, first.rd_off);
	if (first.dm_bytes) {
		fprintf(stderr, "; DMEM: %u bytes distintos en %03x–%03x, desde %03x lle", first.dm_bytes, first.dm_first,
			first.dm_last, first.dm_first);
		for (unsigned j = 0; j < 8; j++)
			fprintf(stderr, " %02x", first.dm_lle[j]);
		fprintf(stderr, " hle");
		for (unsigned j = 0; j < 8; j++)
			fprintf(stderr, " %02x", first.dm_hle[j]);
		fprintf(stderr, "; bloques de 64:");
		for (unsigned j = 0; j < 64; j++)
			if (first.dm_blocks >> j & 1)
				fprintf(stderr, " %03x", j * 64);
	}
	fputc('\n', stderr);
	return false;
}

/* `dm_pre`: la DMEM del RSP antes de que el arranque del microcódigo (rspboot) copie su segmento
 * de datos; ver shadow_lle_start. */
static void shadow_start(const struct salsa64_task *t, const uint8_t *dm_pre)
{
	enum salsa64_abi abi = salsa64_rsp_detect_abi(&n64.bus, t);
	if (abi == SALSA64_ABI_UNKNOWN)
		return;
	if (!shadow.rdram)
		shadow.rdram = malloc(n64.bus.rdram_size);
	memcpy(shadow.rdram, n64.bus.rdram, n64.bus.rdram_size);
	shadow_env();
	shadow.cur_abi = abi;
	if (shadow.dump_task == shadow.tasks + 1) {
		if (!shadow.pre)
			shadow.pre = malloc(n64.bus.rdram_size);
		memcpy(shadow.pre, n64.bus.rdram, n64.bus.rdram_size);
	}
	struct aud_job job;
	salsa64_audio_job(&job, &n64.bus, t, abi);
	job.flags |= aud_sync.job_flags;  /* las mismas banderas (SALSA64_AUDIO_*) que la HLE normal */
	job.rdram_ea = (uint64_t)(uintptr_t)shadow.rdram;
	if (shadow.diffs)
		fprintf(stderr, "T %u\n", shadow.tasks + 1);
	if (shadow.snap && shadow.snap_task == shadow.tasks + 1) {
		char path[512];
		size_t n = strcspn(shadow.snap, ":");
		for (int w = 0; w < 2; w++) {
			snprintf(path, sizeof path, "%.*s.%s", (int)n, shadow.snap, w ? "mem" : "ram");
			FILE *f = fopen(path, "wb");
			if (f) {
				if (w) {
					uint8_t pc[4];
					fwrite(dm_pre, 1, LLE_MEM_SIZE, f);
					fwrite(n64.bus.sp_mem + LLE_MEM_SIZE, 1, LLE_MEM_SIZE, f);
					salsa64_wr32(pc, n64.bus.sp.pc);  /* SP_PC con el que arrancó la tarea */
					fwrite(pc, 1, 4, f);
				} else
					fwrite(n64.bus.rdram, 1, n64.bus.rdram_size, f);
				fclose(f);
				fprintf(stderr, "lle-check: instantánea de la tarea %u en %s\n", shadow.snap_task, path);
			}
		}
	}
	/* La DMEM de la HLE parte de la del RSP real: entre dos tareas de audio puede haber corrido
	 * otro microcódigo (gráficas) y lo que el audio lee de búferes que nadie escribió esta
	 * tarea (RESAMPLE pasado el final de su entrada) es lo que dejó. SALSA64_LLE_NOSYNC=1 no
	 * lo hace. */
	if (!getenv("SALSA64_LLE_NOSYNC")) {
		memcpy(aud_state.dmem, dm_pre, LLE_MEM_SIZE);
		memcpy(aud_state.mp3, dm_pre + AUD_MP3_BASE, AUD_MP3_SIZE);  /* la memoria de trabajo del MP3 es la DMEM de 0x800 */
	}
	memcpy(shadow.lle_dm0, dm_pre, sizeof shadow.lle_dm0);
	memcpy(shadow.hle_dm0, aud_state.dmem, sizeof shadow.hle_dm0);
	if (!shadow.dm_env) {
		/* Ventana de la DMEM con los búferes de audio: lo que hay debajo es el trabajo propio del
		 * microcódigo (su segmento de datos, que la HLE no lleva) y arriba la cabecera y la lista. */
		uint32_t lo;
		if (abi == SALSA64_ABI_AUDIO || abi == SALSA64_ABI_AUDIO_GE || abi == SALSA64_ABI_AUDIO_BC)
			lo = 0x5C0;
		else if (abi >= SALSA64_ABI_NAUDIO && abi <= SALSA64_ABI_NAUDIO_CBFD)
			lo = 0x4F0;
		else
			lo = ((t->ucode_data_size + 0x3F) & ~0x3Fu) + 0x100;  /* y el búfer de la lista, que el microcódigo copia a la DMEM */
		shadow.dm_lo = lo;
		shadow.dm_hi = 0xF80;
	}
	bool bisect = false;
	for (uint32_t i = 0; i < shadow.bis_n; i++)
		bisect |= shadow.tasks + 1 >= shadow.bis_range[i][0] && shadow.tasks + 1 <= shadow.bis_range[i][1];
	if (bisect && job.alist_size >= 8)
		shadow_bisect(&job, abi);
	aud_dbg_env_n = 0;
	aud_run(&aud_state, &job, &shadow.res);
	shadow.env_n = aud_dbg_env_n;
	memcpy(shadow.env_addr, aud_dbg_env, sizeof shadow.env_addr);
	shadow.pending = true;
	shadow.task = ++shadow.tasks;
	/* SALSA64_LLE_DUMP_TASK=N: la lista de comandos de esa tarea y lo que
	 * la HLE escribió, para seguir la divergencia a mano. */
	if (shadow.dump_task == shadow.task) {
		fprintf(stderr, "lle-check: tarea de audio %u, alist %06x (%u bytes), ABI %s, flags %x, yield %08x/%x:\n",
			shadow.task, job.alist, job.alist_size, salsa64_abi_name(abi), t->flags, t->yield_data_ptr,
			t->yield_data_size);
		for (uint32_t i = 0; i + 8 <= job.alist_size; i += 8)
			fprintf(stderr, "  %3u: %08x %08x\n", i / 8, salsa64_rd32(n64.bus.rdram + ((job.alist + i) & 0x7FFFFF)),
				salsa64_rd32(n64.bus.rdram + ((job.alist + i + 4) & 0x7FFFFF)));
		if (abi == SALSA64_ABI_MUSYX1 || abi == SALSA64_ABI_MUSYX2 || abi == SALSA64_ABI_MUSYX1_RS) {
			/* MusyX: data_size es el número de subtramas; la cabecera de cada
			 * una (0x10 bytes en v1, 0x28 en v2) y su primera voz (0x50). */
			uint32_t hdr = abi == SALSA64_ABI_MUSYX2 ? 0x28 : 0x10, stride = hdr + 32 * 0x50;
			for (uint32_t k = 0; k < job.alist_size && k < 4; k++) {
				uint32_t a = (job.alist + k * stride) & 0x7FFFFF;
				fprintf(stderr, "  SFD %u en %06x:", k, a);
				for (uint32_t j = 0; j < hdr + 0x50 && a + j + 4 <= n64.bus.rdram_size; j += 4)
					fprintf(stderr, "%s%08x", j % 16 ? " " : "\n    ", salsa64_rd32(n64.bus.rdram + a + j));
				fprintf(stderr, "\n");
			}
		}
		for (uint32_t i = 0; i < shadow.res.nranges && i < AUD_MAX_RANGES; i++)
			fprintf(stderr, "  tramo %u: %06x %u bytes\n", i, shadow.res.range_addr[i], shadow.res.range_len[i]);
	}
}

/* --lle-check-jpeg: las tareas JPEG van al microcódigo real (LLE) y la HLE
 * (rsp/jpeg) corre la misma tarea sobre una copia de la RDRAM; cuando el
 * microcódigo termina (jcheck_sync) se comparan los macrobloques escritos,
 * píxel RGBA5551 por píxel, por canal de 5 bits. Las primeras diferencias
 * van al stderr; SALSA64_JCHECK_ALL=1 las lista todas (tarea, macrobloque,
 * x, y), para buscar sesgos. SALSA64_JCHECK_DUMP=archivo agrega por tarea
 * la entrada y la salida del microcódigo (jcheck_dump), para probar otra
 * IDCT sin volver a correr el juego; el archivo es del juego: fuera del
 * repositorio. */
static struct {
	bool on, pending;
	uint8_t *rdram;
	uint8_t *input;         /* los macrobloques antes de la tarea (volcado) */
	FILE *dump;
	struct salsa64_jpeg_result res;
	uint32_t tasks, macroblocks, pixels, diff, bad;
	uint32_t hist[32];      /* píxeles por diferencia máxima entre canales */
	int reports;
	/* Video de Resident Evil 2 (rsp/jpeg/re2.c): se compara byte a byte. */
	enum salsa64_jpeg_kind kind;   /* de la tarea en curso */
	struct {
		uint32_t tasks, rows, bytes, diff, bad, skipped;  /* por tipo: tareas, unidades, bytes comparados, bytes distintos, tareas distintas, no comparables */
		uint32_t hist[6];         /* bytes distintos por |lle − hle|: 1, 2, 3–4, 5–16, 17–64, más */
	} video[3];
	/* SALSA64_RE2_DUMP=archivo[:N]: de las primeras N (8) tareas de cada tipo, los parámetros, lo
	 * que lee la tarea antes de empezar y lo que escribe el microcódigo (para ajustar la
	 * aritmética sin volver a correr el juego); el archivo es del juego: fuera del repositorio.
	 * Registro: "RE2V", tipo (0 a 2), 40 bytes de parámetros, nr y nr regiones (rol: 0 antes, 1
	 * microcódigo, 2 HLE; dirección; largo; bytes). */
	FILE *vdump;
	uint32_t vdump_max, vdump_n[3];
	uint8_t *vpre;          /* regiones de entrada de la tarea en curso (concatenadas) */
	uint8_t vpre_params[128];
	uint32_t vpre_addr[5], vpre_len[5], vpre_n;
} jcheck;

/* Una tarea de video de Resident Evil 2: cada unidad (fila, o par de filas
 * en DECODE) son mb_out bytes, una cada mb_stride (con signo, como la
 * zancada del FILL) desde out_addr, y se comparan todos. */
static void jcheck_compare_video(void)
{
	unsigned k = (unsigned)(jcheck.kind - SALSA64_JPEG_RE2_RESIZE);
	static const char *const names[3] = { "RESIZE", "DECODE", "FILL" };
	int64_t stride = (int32_t)jcheck.res.mb_stride;
	uint32_t units = jcheck.res.macroblocks, row = jcheck.res.mb_out, diff = 0;
	bool all = getenv("SALSA64_JCHECK_ALL") != NULL;
	jcheck.video[k].tasks++;
	jcheck.video[k].rows += units;
	for (uint32_t u = 0; u < units; u++) {
		int64_t a = (int64_t)(jcheck.res.out_addr & 0x1FFFFFFF) + (int64_t)u * stride;
		if (a < 0 || a + row > n64.bus.rdram_size)
			continue;
		jcheck.video[k].bytes += row;
		for (uint32_t b = 0; b < row; b++) {
			int x = n64.bus.rdram[a + b], y = jcheck.rdram[a + b], d = x > y ? x - y : y - x;
			if (!d)
				continue;
			diff++;
			jcheck.video[k].hist[d == 1 ? 0 : d == 2 ? 1 : d <= 4 ? 2 : d <= 16 ? 3 : d <= 64 ? 4 : 5]++;
			if (all || jcheck.reports < 12) {
				jcheck.reports++;
				fprintf(stderr, "lle-check: tarea de video %s %u, unidad %u, byte %u (%06x): lle %02x, hle %02x\n",
					names[k], jcheck.video[k].tasks, u, b, (unsigned)(a + b), x, y);
			}
		}
	}
	jcheck.video[k].diff += diff;
	jcheck.video[k].bad += diff != 0;
}

static void vdump_region(uint32_t role, uint32_t addr, uint32_t len, const uint8_t *src)
{
	uint8_t h[12];
	salsa64_wr32(h, role);
	salsa64_wr32(h + 4, addr);
	salsa64_wr32(h + 8, len);
	fwrite(h, 1, sizeof h, jcheck.vdump);
	fwrite(src, 1, len, jcheck.vdump);
}

/* Escribe el registro de la tarea de video en curso (con su salida del microcódigo y de la HLE). */
static void vdump_write(void)
{
	if (!jcheck.vdump || !jcheck.vpre)
		return;
	unsigned k = (unsigned)(jcheck.kind - SALSA64_JPEG_RE2_RESIZE);
	if (jcheck.vdump_n[k] >= jcheck.vdump_max)
		return;
	jcheck.vdump_n[k]++;
	uint8_t h[8];
	memcpy(h, "RE2V", 4);
	salsa64_wr32(h + 4, k);
	fwrite(h, 1, 8, jcheck.vdump);
	fwrite(jcheck.vpre_params, 1, 128, jcheck.vdump);
	uint8_t nr[4];
	uint32_t n_out = (jcheck.res.macroblocks && !jcheck.res.bad) ? 1 : 0;
	salsa64_wr32(nr, jcheck.vpre_n + 2 * n_out);
	fwrite(nr, 1, 4, jcheck.vdump);
	uint32_t off = 0;
	for (uint32_t i = 0; i < jcheck.vpre_n; i++) {
		vdump_region(i + 1 == jcheck.vpre_n ? 3 : 0, jcheck.vpre_addr[i], jcheck.vpre_len[i], jcheck.vpre + off);
		off += jcheck.vpre_len[i];
	}
	if (n_out) {
		/* Todo el tramo escrito, de la primera a la última unidad. */
		uint32_t a = jcheck.res.out_addr & 0x1FFFFFFF, len = jcheck.res.out_bytes;
		if (a <= n64.bus.rdram_size && len <= n64.bus.rdram_size - a) {
			vdump_region(1, a, len, n64.bus.rdram + a);
			vdump_region(2, a, len, jcheck.rdram + a);
		}
	}
	fflush(jcheck.vdump);
}

static void jcheck_compare(void)
{
	jcheck.pending = false;
	if (salsa64_jpeg_is_re2(jcheck.kind)) {
		vdump_write();
		jcheck_compare_video();
		return;
	}
	uint32_t a = jcheck.res.out_addr & 0x7FFFFF, n = jcheck.res.out_bytes & ~1u;
	if (a > n64.bus.rdram_size || n > n64.bus.rdram_size - a)
		return;
	/* Cada macrobloque ocupa mb_stride bytes y la salida, sus primeros
	 * mb_out. */
	uint32_t mb_stride = jcheck.res.mb_stride, mb_out = jcheck.res.mb_out;
	bool all = getenv("SALSA64_JCHECK_ALL") != NULL;
	uint32_t diff = 0, px = 0;
	for (uint32_t k = 0; k < n; k += 2) {
		if (mb_stride && k % mb_stride >= mb_out)
			continue;
		px++;
		uint32_t x = salsa64_rd16(n64.bus.rdram + a + k), y = salsa64_rd16(jcheck.rdram + a + k);
		int dm = 0;
		for (int c = 0; c < 3; c++) {
			int d = (int)((x >> (11 - 5 * c)) & 31) - (int)((y >> (11 - 5 * c)) & 31);
			d = d < 0 ? -d : d;
			dm = d > dm ? d : dm;
		}
		dm |= (x & 1) != (y & 1) ? 31 : 0;
		jcheck.hist[dm]++;
		diff += dm != 0;
		if (dm && (all || jcheck.reports < 6)) {
			jcheck.reports++;
			uint32_t o = mb_stride ? k % mb_stride / 2 : k / 2;
			fprintf(stderr, "lle-check: tarea JPEG %u, macrobloque %u (x %u, y %u): lle %04x, hle %04x\n",
				jcheck.tasks, mb_stride ? k / mb_stride : 0, o % 16, o / 16, x, y);
		}
	}
	jcheck.pixels += px;
	jcheck.diff += diff;
	jcheck.bad += diff != 0;
	if (jcheck.dump) {
		/* Registro: "JPEG", modo, macrobloques, las 3 tablas de
		 * cuantización (3 × 128 bytes), la entrada (n × mb_stride) y lo que
		 * escribió el microcódigo (n × mb_out); todo en el orden de bytes
		 * de la N64. */
		uint32_t count = mb_stride ? (n + mb_stride - 1) / mb_stride : 0;
		uint8_t h[16];
		memcpy(h, "JPEG", 4);
		salsa64_wr32(h + 4, mb_stride == 768 ? 2 : 0);
		salsa64_wr32(h + 8, count);
		salsa64_wr32(h + 12, 0);
		fwrite(h, 1, sizeof h, jcheck.dump);
		fwrite(jcheck.input, 1, 3 * 128 + count * mb_stride, jcheck.dump);
		for (uint32_t m = 0; m < count; m++)
			fwrite(n64.bus.rdram + a + m * mb_stride, 1, mb_out, jcheck.dump);
	}
}

/* Con SALSA64_RE2_DUMP: lo que la tarea de video lee, antes de que empiece (jcheck.rdram es la
 * RDRAM de ese momento, todavía sin la salida de la HLE sobre la copia: se toma de n64.bus). */
static void vdump_capture(const struct salsa64_task *t)
{
	if (!getenv("SALSA64_RE2_DUMP"))
		return;
	if (!jcheck.vdump) {
		char path[512];
		snprintf(path, sizeof path, "%s", getenv("SALSA64_RE2_DUMP"));
		char *c = strrchr(path, ':');
		jcheck.vdump_max = 8;
		if (c && atoi(c + 1) > 0) {
			jcheck.vdump_max = (uint32_t)atoi(c + 1);
			*c = 0;
		}
		if (!(jcheck.vdump = fopen(path, "wb"))) {
			fprintf(stderr, "lle-check: no se pudo abrir %s\n", path);
			setenv("SALSA64_RE2_DUMP", "", 1);
			return;
		}
	}
	unsigned k = (unsigned)(jcheck.kind - SALSA64_JPEG_RE2_RESIZE);
	if (jcheck.vdump_n[k] >= jcheck.vdump_max)
		return;
	uint32_t d = t->ucode_data & 0x1FFFFFFF;
	if (d + 128 > n64.bus.rdram_size)
		return;
	memcpy(jcheck.vpre_params, n64.bus.rdram + d, 128);
	const uint8_t *p = jcheck.vpre_params;
	uint32_t a[5] = { 0, 0, 0, 0, 0 }, l[5] = { 0, 0, 0, 0, 0 };
	unsigned n = 0;
	/* Regiones de entrada según los parámetros crudos (aunque la HLE no los acepte). */
	if (jcheck.kind == SALSA64_JPEG_RE2_RESIZE) {
		uint32_t src = salsa64_rd32(p) & 0x1FFFFFFF, width = salsa64_rd32(p + 8), rows = salsa64_rd32(p + 12);
		uint32_t xr = salsa64_rd32(p + 16), yr = salsa64_rd32(p + 20), gp = salsa64_rd32(p + 36);
		uint64_t xmax = ((uint64_t)(width ? width - 1 : 0) * xr + salsa64_rd32(p + 32)) >> 16;
		uint64_t ymax = ((uint64_t)gp + (uint64_t)(rows ? rows - 1 : 0) * yr) >> 16;
		a[0] = src + (gp >> 16) * 960;
		l[0] = (uint32_t)((ymax - (gp >> 16) + 2) * 960 + 3 * xmax + 8);
		n = 1;
	} else if (jcheck.kind == SALSA64_JPEG_RE2_DECODE) {
		uint32_t w = salsa64_rd32(p + 16), pairs = salsa64_rd32(p + 28), step = salsa64_rd32(p + 36);
		uint32_t chroma = pairs * (w / 2) + w;
		a[0] = salsa64_rd32(p) & 0x1FFFFFFF; l[0] = pairs * 2 * w;
		a[1] = salsa64_rd32(p + 4) & 0x1FFFFFFF; l[1] = chroma;
		a[2] = salsa64_rd32(p + 8) & 0x1FFFFFFF; l[2] = chroma;
		a[3] = salsa64_rd32(p + 12) & 0x1FFFFFFF;
		l[3] = pairs ? (pairs - 1) * step + step / 2 + 4 * w : 0;
		n = 4;
	} else {
		uint32_t len = (salsa64_rd32(p + 0x18) & 0xFFF) + 1, skip = (salsa64_rd32(p + 0x18) >> 20) & 0xFFF;
		uint32_t rows = salsa64_rd32(p + 12), iters = salsa64_rd32(p + 16), adv = salsa64_rd32(p + 0x1C);
		uint32_t span = iters ? (iters - 1) * adv + (rows ? (rows - 1) * (len + skip) : 0) + len : 0;
		a[0] = salsa64_rd32(p) & 0x1FFFFFFF; l[0] = span;
		a[1] = salsa64_rd32(p + 4) & 0x1FFFFFFF; l[1] = span;
		n = 2;
	}
	/* Y el código del microcódigo (4 KB), con rol 3 en el volcado. */
	a[n] = t->ucode & 0x1FFFFFFF;
	l[n++] = 0x1000;
	uint32_t total = 0;
	for (unsigned i = 0; i < n; i++) {
		if (a[i] > n64.bus.rdram_size || l[i] > n64.bus.rdram_size - a[i])
			l[i] = a[i] > n64.bus.rdram_size ? 0 : n64.bus.rdram_size - a[i];
		total += l[i];
	}
	free(jcheck.vpre);
	jcheck.vpre = malloc(total ? total : 1);
	uint32_t off = 0;
	for (unsigned i = 0; i < n; i++) {
		memcpy(jcheck.vpre + off, n64.bus.rdram + a[i], l[i]);
		jcheck.vpre_addr[i] = a[i];
		jcheck.vpre_len[i] = l[i];
		off += l[i];
	}
	jcheck.vpre_n = n;
}

/* SALSA64_RE2_FUZZ=semilla: antes de cada tarea de video se cambian sus parámetros (y las palabras
 * dram_stack y dram_stack_size de la tarea) a valores al azar dentro de lo que la HLE acepta y de las
 * regiones que el juego ya usa, para comparar con el microcódigo real mucho más que las geometrías
 * del juego (anchos, pasos, umbral del difuminado, constantes del color, largos de fila…). La
 * partida sigue con el video cambiado: sirve solo para la comparación. */
static uint32_t re2_fuzz_state;
static uint32_t re2_fuzz_stack, re2_fuzz_stack_size;

static uint32_t re2_fuzz_rand(void)
{
	re2_fuzz_state = re2_fuzz_state * 1664525u + 1013904223u;
	return re2_fuzz_state >> 8;
}

static void re2_fuzz(enum salsa64_jpeg_kind kind, const struct salsa64_task *t)
{
	uint8_t *d = n64.bus.rdram + (t->ucode_data & 0x1FFFFFFF);
	re2_fuzz_stack = re2_fuzz_rand() % 4 ? re2_fuzz_rand() * 4096u + re2_fuzz_rand() % 4096 : t->dram_stack;
	re2_fuzz_stack_size = re2_fuzz_rand() % 4 ? re2_fuzz_rand() * 4096u + re2_fuzz_rand() % 4096 : t->dram_stack_size;
	salsa64_wr32(n64.bus.sp_mem + SALSA64_TASK_DMEM + 0x20, re2_fuzz_stack);
	salsa64_wr32(n64.bus.sp_mem + SALSA64_TASK_DMEM + 0x24, re2_fuzz_stack_size);
	if (kind == SALSA64_JPEG_RE2_RESIZE) {
		uint32_t w0 = salsa64_rd32(d + 8), r0 = salsa64_rd32(d + 12), y0 = salsa64_rd32(d + 0x24);
		uint32_t xr = 8192 + re2_fuzz_rand() % 100000, yr = 16000 + re2_fuzz_rand() % 60000;
		static const int16_t thr[] = { 0, 1, 3, 5, 5, 12, 40, 0x7FFF, -3 };
		if (w0 < 4 || w0 > 1024 || r0 < 1)
			return;
		salsa64_wr32(d + 8, 4 * (1 + re2_fuzz_rand() % (w0 / 4)));
		salsa64_wr32(d + 12, 1 + re2_fuzz_rand() % r0);
		salsa64_wr32(d + 0x10, xr);
		salsa64_wr32(d + 0x14, yr);
		salsa64_wr32(d + 0x20, re2_fuzz_rand() % 3 ? re2_fuzz_rand() % 0x30000 : 0);
		salsa64_wr32(d + 0x24, (y0 & ~0xFFFFu) | (re2_fuzz_rand() % 0x10000));
		salsa64_wr16(d + 0x58, (8 * xr) >> 16);
		salsa64_wr16(d + 0x5A, (8 * xr) & 0xFFFF);
		salsa64_wr16(d + 0x5E, yr);
		salsa64_wr16(d + 0x62, (uint16_t)thr[re2_fuzz_rand() % (sizeof thr / sizeof thr[0])]);
	} else if (kind == SALSA64_JPEG_RE2_DECODE) {
		static const uint16_t zero[] = { 0x4000, 0x4000, 0x3F80, 0x4100 };
		uint32_t w = 16 * (1 + re2_fuzz_rand() % 15), pairs = 1 + re2_fuzz_rand() % 60;
		salsa64_wr32(d + 0x10, w);
		salsa64_wr32(d + 0x14, 2 * pairs);
		salsa64_wr32(d + 0x1C, pairs);
		salsa64_wr32(d + 0x20, (4 * w - 1) | (1u << 12) | ((1056 - 4 * w) << 20));
		if (re2_fuzz_rand() % 2)
			for (unsigned i = 0; i < 5; i++)
				salsa64_wr16(d + 0x40 + 2 * i, (uint16_t)(salsa64_rd16(d + 0x40 + 2 * i) + re2_fuzz_rand() % 2001 - 1000));
		salsa64_wr16(d + 0x50, zero[re2_fuzz_rand() % 4]);
	} else {
		static const uint32_t lens[] = { 8, 16, 24, 64, 120, 128, 480, 960 };
		uint32_t len = lens[re2_fuzz_rand() % 8], skip = 8 * (re2_fuzz_rand() % 13), rows = 1 + re2_fuzz_rand() % 2;
		uint32_t iters = 1 + re2_fuzz_rand() % 60;
		salsa64_wr32(d + 0x08, rows * len);
		salsa64_wr32(d + 0x0C, rows);
		salsa64_wr32(d + 0x10, iters);
		salsa64_wr32(d + 0x18, (len - 1) | (skip << 20));
		salsa64_wr32(d + 0x1C, rows * (len + skip));
	}
}

static void jcheck_start(const struct salsa64_task *t)
{
	uint32_t code = t->ucode & 0x7FFFFF;
	if (code > n64.bus.rdram_size || t->ucode_size > n64.bus.rdram_size - code)
		return;
	enum salsa64_jpeg_kind kind = salsa64_jpeg_detect(n64.bus.rdram + code, t->ucode_size);
	if (kind == SALSA64_JPEG_NONE && code + 256 <= n64.bus.rdram_size)
		kind = salsa64_re2_detect(n64.bus.rdram + code, 256);
	if (kind == SALSA64_JPEG_NONE)
		return;
	if (salsa64_jpeg_is_re2(kind)) {
		/* Video: la HLE corre sobre la copia con la estructura en ucode_data. */
		uint32_t stack = t->dram_stack, stack_size = t->dram_stack_size;
		if (getenv("SALSA64_RE2_FUZZ")) {
			if (!re2_fuzz_state)
				re2_fuzz_state = (uint32_t)atoi(getenv("SALSA64_RE2_FUZZ")) * 2654435761u + 1;
			re2_fuzz(kind, t);
			stack = re2_fuzz_stack;
			stack_size = re2_fuzz_stack_size;
		}
		if (!jcheck.rdram)
			jcheck.rdram = malloc(n64.bus.rdram_size);
		memcpy(jcheck.rdram, n64.bus.rdram, n64.bus.rdram_size);
		jcheck.kind = kind;
		salsa64_re2_run(kind, jcheck.rdram, n64.bus.rdram_size, t->ucode_data, stack, stack_size, &jcheck.res);
		unsigned k = (unsigned)(kind - SALSA64_JPEG_RE2_RESIZE);
		if (jcheck.res.bad) {
			jcheck.video[k].skipped++;
			if (getenv("SALSA64_JCHECK_VERBOSE")) {
				uint32_t d = t->ucode_data & 0x1FFFFFFF;
				fprintf(stderr, "lle-check: video %u sin HLE, segmento de datos:", k);
				for (unsigned w = 0; w < 32 && d + 4 * w + 4 <= n64.bus.rdram_size; w++)
					fprintf(stderr, " %08x", salsa64_rd32(n64.bus.rdram + d + 4 * w));
				fputc('\n', stderr);
			}
		}
		if (getenv("SALSA64_JCHECK_VERBOSE"))
			fprintf(stderr, "lle-check: video %u (campo %u): ucode_data %06x, %u unidades de %u bytes cada %d desde %06x%s\n",
				k, n64.bus.vi.fields, t->ucode_data, jcheck.res.macroblocks, jcheck.res.mb_out,
				(int)jcheck.res.mb_stride, jcheck.res.out_addr, jcheck.res.bad ? " (parámetros malos)" : "");
		jcheck.pending = !jcheck.res.bad;
		vdump_capture(t);
		return;
	}
	jcheck.kind = kind;
	if (!jcheck.rdram)
		jcheck.rdram = malloc(n64.bus.rdram_size);
	memcpy(jcheck.rdram, n64.bus.rdram, n64.bus.rdram_size);
	const char *dump = getenv("SALSA64_JCHECK_DUMP");
	if (dump && !jcheck.dump && !(jcheck.dump = fopen(dump, "wb")))
		fprintf(stderr, "lle-check: no se pudo abrir %s\n", dump);
	if (jcheck.dump) {
		/* Las tablas y los macrobloques antes de la tarea (la HLE de abajo
		 * los pisa en la copia). */
		uint32_t d = t->data_ptr & 0x7FFFFF;
		if (!jcheck.input)
			jcheck.input = malloc(3 * 128 + 64 * 768);
		memset(jcheck.input, 0, 3 * 128 + 64 * 768);
		if (d + 24 <= n64.bus.rdram_size) {
			const uint8_t *p = n64.bus.rdram + d;
			uint32_t addr = salsa64_rd32(p) & 0x7FFFFF, count = salsa64_rd32(p + 4);
			uint32_t bytes = (salsa64_rd32(p + 8) == 2 ? 768 : 512) * (count < 64 ? count : 64);
			for (int q = 0; q < 3; q++) {
				uint32_t qa = salsa64_rd32(p + 12 + 4 * q) & 0x7FFFFF;
				if (qa + 128 <= n64.bus.rdram_size)
					memcpy(jcheck.input + 128 * q, n64.bus.rdram + qa, 128);
			}
			if (addr + bytes <= n64.bus.rdram_size)
				memcpy(jcheck.input + 3 * 128, n64.bus.rdram + addr, bytes);
		}
	}
	/* SALSA64_JCHECK_SNAP=P[:N]: al arrancar la tarea JPEG N (1), la RDRAM en P.ram y la DMEM/IMEM
	 * en P.mem (con el SP_PC en cero detrás), para repetirla con otra entrada en un banco de
	 * pruebas sin volver a correr el juego. */
	if (getenv("SALSA64_JCHECK_SNAP")) {
		const char *sp = getenv("SALSA64_JCHECK_SNAP");
		size_t pn = strcspn(sp, ":");
		unsigned want = strchr(sp, ':') ? (unsigned)atoi(strchr(sp, ':') + 1) : 1;
		if (jcheck.tasks + 1 == want) {
			char path[512];
			for (int w = 0; w < 2; w++) {
				snprintf(path, sizeof path, "%.*s.%s", (int)pn, sp, w ? "mem" : "ram");
				FILE *f = fopen(path, "wb");
				if (!f)
					continue;
				if (w) {
					uint8_t pc[4];
					fwrite(n64.bus.sp_mem, 1, 2 * LLE_MEM_SIZE, f);
					salsa64_wr32(pc, 0);
					fwrite(pc, 1, 4, f);
				} else {
					fwrite(n64.bus.rdram, 1, n64.bus.rdram_size, f);
				}
				fclose(f);
				fprintf(stderr, "lle-check: instantánea de la tarea JPEG %u en %s\n", want, path);
			}
		}
	}
	salsa64_jpeg_run(kind, jcheck.rdram, n64.bus.rdram_size, t->data_ptr, &jcheck.res);
	jcheck.tasks++;
	jcheck.macroblocks += jcheck.res.macroblocks;
	jcheck.pending = !jcheck.res.bad;
}

/* El LLE del host se pone al día en cada acceso del CPU al SP o al DP: el
 * primero con el RSP en HALT tras una tarea JPEG es el manejador de la
 * interrupción, antes de que el juego copie la salida y vuelva a llenar el
 * mismo búfer con la tarea siguiente. Ahí se compara. */
static struct salsa64_lle_backend jcheck_lle;

static void jcheck_sync(void *ctx)
{
	lle_host.backend.sync(ctx);
	/* Solo al terminar: si el planificador del juego la interrumpió
	 * (yield, SIG1) se reanuda más tarde con OS_TASK_YIELDED. */
	uint32_t st = n64.bus.sp.regs[SALSA64_SP_STATUS];
	bool done = (st & SALSA64_SP_STATUS_HALT) && (st & SALSA64_SP_STATUS_TASKDONE) && !(st & SALSA64_SP_STATUS_SIG(1));
	if (jcheck.pending && done)
		jcheck_compare();
	/* Igual con --lle-check-audio: la salida del microcódigo se compara en
	 * cuanto la tarea termina, antes de que el juego toque de nuevo los
	 * búferes y el estado que la tarea escribió (MusyX los deja en su
	 * memoria, donde el juego los reutiliza). */
	if (shadow.pending && done)
		shadow_compare();
	if (gchk_pending() && done) {
		salsa64_dp_drain(&n64.bus);  /* el RDP congelado (F3DEX2 FIFO) dibuja antes de comparar */
		gchk_task_end();
	}
}

/* Arranque de una tarea de audio en el microcódigo real: libultra lo empieza copiando a la IMEM
 * y a la DMEM el código y el segmento de datos con dos DMA. En la consola eso ocurre antes de que
 * el CPU vuelva a tocar la RDRAM (el juego puede escribir en su segmento de datos justo después
 * de lanzar la tarea), pero el LLE arranca en la siguiente rebanada, ya con el CPU adelantado.
 * Para que la sombra de la HLE vea la misma RDRAM, se ejecuta a mano el arranque hasta el segundo
 * DMA y entonces se arranca la sombra. Los registros vectoriales y sus banderas los deja la tarea
 * anterior (de cualquier microcódigo) y el de audio no los inicializa: VADD suma el acarreo que
 * encuentre y el primer VADD de la tarea puede dar +1 en un carril; la HLE no tiene de dónde
 * saberlo, así que el microcódigo real parte de cero. */
static void shadow_lle_start(void *ctx)
{
	uint8_t dm_pre[LLE_MEM_SIZE];
	bool want = shadow.boot_pending;
	shadow.boot_pending = false;
	if (want) {
		memset(lle_host.rsp.v, 0, sizeof lle_host.rsp.v);
		memset(lle_host.rsp.acc, 0, sizeof lle_host.rsp.acc);
		lle_host.rsp.vco = lle_host.rsp.vcc = lle_host.rsp.vce = 0;
		memcpy(dm_pre, n64.bus.sp_mem, sizeof dm_pre);
	}
	lle_host.backend.start(ctx);
	if (!want)
		return;
	uint32_t w0 = lle_host.rsp.c0_writes[LLE_C0_RD_LEN];
	for (unsigned i = 0; i < 20000 && lle_host.rsp.c0_writes[LLE_C0_RD_LEN] < w0 + 2 && !lle_host.rsp.halt; i++)
		lle_step(&lle_host.rsp);
	shadow_start(&shadow.boot_task, dm_pre);
}

/* --lle-check-gfx: el microcódigo real corre la tarea entera en el acto, antes de que el CPU
 * avance: la HLE lee la RDRAM al arrancar la tarea y el LLE, por rebanadas, la leería
 * 1536 ciclos más tarde y con el CPU ya escribiendo (Pokémon Snap, Mario Kart 64…): lo que se
 * compara es el RSP, no la carrera con el CPU. Una tarea que espera al CPU (semáforo, yield)
 * se queda donde esté al llegar al tope y sigue por rebanadas. */
static void gchk_lle_start(void *ctx)
{
	lle_host.backend.start(ctx);
	/* SALSA64_GCHK_ASYNC=1: por rebanadas como siempre (algunos juegos esperan al RSP con otro ritmo
	 * y se atoran si la tarea termina en el acto: NBA Showtime, Hydro Thunder). */
	if (!gchk_pending() || getenv("SALSA64_GCHK_ASYNC"))
		return;
	for (unsigned total = 0; total < (16u << 20) && !lle_host.rsp.halt;) {
		uint32_t n = salsa64_lle_host_run(&lle_host, 1u << 16);
		total += n;
		if (!n)
			break;
	}
}

/* Reloj del host para lo que el RSP mide en el hilo del CPU (HLE JPEG). */
static uint64_t host_clock_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* SALSA64_JITTER=semilla: la duración de cada tarea del RSP varía al azar
 * (como el SPE de la PS3, cuyo fin llega a la hora de un sondeo): las de
 * audio entre 8000 y 68000 ciclos y las gráficas entre 0 y 8 sondeos de
 * retraso (con SALSA64_GFX_LATE puesto, en 0 si no). Para buscar esperas
 * eternas que dependen de cuándo termina cada tarea. */
static uint32_t jitter_state;

static uint32_t jitter_rand(void)
{
	jitter_state = jitter_state * 1664525u + 1013904223u;
	return jitter_state >> 8;
}

/* Tareas gráficas por microcódigo y cambios de imagen del SPE 0 que habría
 * en la PS3 (rsp/gfx/spe0.h): al final, en el resumen de gfx. */
static struct {
	uint32_t tasks[SALSA64_GBI_F3DEX2EUROCOM + 1];
	int img;          /* imagen de la última tarea con HLE, o -1 */
	uint32_t switches;
	uint32_t imgs;    /* imágenes usadas (bit por imagen) */
} gbi_hist = { .img = -1 };

static void count_gbi(enum salsa64_gbi gbi)
{
	if ((unsigned)gbi <= SALSA64_GBI_F3DEX2EUROCOM)
		gbi_hist.tasks[gbi]++;
	int img = gfx_spe0_image(gbi);
	if (img < 0)
		return;
	if (gbi_hist.img >= 0 && img != gbi_hist.img)
		gbi_hist.switches++;
	gbi_hist.img = img;
	gbi_hist.imgs |= 1u << img;
}

static void trace_rsp(void *ctx, const struct salsa64_task *t, enum salsa64_gbi gbi)
{
	(void)ctx;
	if (t->type == SALSA64_TASK_GFX)
		count_gbi(gbi);
	if (jitter_state) {
		n64.bus.rsp.audio_cycles = 8000 + 10000 * (jitter_rand() % 7);
		n64.bus.rsp.gfx_cycles = 30000 + 10000 * (jitter_rand() % 5);
		n64.bus.rsp.dp_cycles = 10000 + 5000 * (jitter_rand() % 5);
		gfx_late.polls = jitter_rand() % 9;
	}
	/* Una tarea JPEG nueva (no la reanudación de una interrumpida, con
	 * OS_TASK_YIELDED): la anterior ya terminó. */
	if (jcheck.on && (t->type == SALSA64_TASK_JPEG || (gbi == SALSA64_GBI_UNKNOWN && t->type != SALSA64_TASK_AUDIO)) &&
	    !(t->flags & 1)) {
		if (jcheck.pending)
			jcheck_compare();
		jcheck_start(t);
	}
	if (gchk_on && t->type == SALSA64_TASK_GFX && !(t->flags & 1)) {
		char key[96], text[80];
		static const char *const names[] = { "desconocido", "F3DEX2", "F3DZEX2", "F3D", "F3DEX", "S2DEX", "F3DPD",
						     "F3DEXBG", "F3DWR", "F3DDKR", "F3DJFG", "S2DEX1", "F5ROGUE", "F5INDY",
						     "Turbo3D", "Turbo3D-F3D", "F3DEX2ACCLAIM", "F3DEX2EUROCOM" };
		_Static_assert(sizeof names / sizeof names[0] == SALSA64_GBI_F3DEX2EUROCOM + 1, "names");
		salsa64_rsp_ucode_text(&n64.bus, t->ucode_data, text, sizeof text);
		uint32_t base = t->ucode_data & 0x7FFFFF, size = t->ucode_data_size;
		if (!text[0] && base < n64.bus.rdram_size && size && size <= n64.bus.rdram_size - base)
			snprintf(text, sizeof text, "sin texto, crc de ucode_data %08x", salsa64_crc32(n64.bus.rdram + base, size));
		snprintf(key, sizeof key, "%s: %s", (unsigned)gbi <= SALSA64_GBI_F3DEX2EUROCOM ? names[gbi] : "?", text);
		gchk_task_start(t, gbi, key);
	}
	if (shadow.on) {
		if (shadow.pending)
			shadow_compare();
		if (t->type == SALSA64_TASK_AUDIO) {
			shadow.boot_task = *t;
			shadow.boot_pending = true;
		}
	}
	if (trace_audio_left && t->type == SALSA64_TASK_AUDIO && tracing()) {
		trace_audio_left--;
		trace_alist(t);
	}
	if (!trace_rsp_left || !tracing())
		return;
	trace_rsp_left--;
	fprintf(stderr, "RSP en ciclo %llu: type=%u flags=%x ucode=%08x/%x data=%08x/%x data_ptr=%08x/%x "
		"boot=%08x/%x gbi=%d\n", (unsigned long long)n64.cpu.cycles, t->type, t->flags, t->ucode,
		t->ucode_size, t->ucode_data, t->ucode_data_size, t->data_ptr, t->data_size, t->ucode_boot,
		t->ucode_boot_size, gbi);
	/* Identificación del microcódigo: su texto ("RSP Gfx ucode …"), el CRC-32
	 * del código y de ucode_data (para los que no traen texto) y, en las de
	 * audio, el ABI y las palabras de ucode_data que lo distinguen. */
	char text[96];
	salsa64_rsp_ucode_text(&n64.bus, t->ucode_data, text, sizeof text);
	if (text[0])
		fprintf(stderr, "  %s\n", text);
	uint32_t base = t->ucode_data & 0x7FFFFF, size = t->ucode_data_size;
	if (base >= n64.bus.rdram_size)
		size = 0;
	else if (!size || size > 0x1000)
		size = 0x1000;
	if (size && size > n64.bus.rdram_size - base)
		size = n64.bus.rdram_size - base;
	uint32_t code = t->ucode & 0x7FFFFF, code_size = t->ucode_size;
	if (code >= n64.bus.rdram_size || code_size > n64.bus.rdram_size - code)
		code_size = 0;
	fprintf(stderr, "  crc32: ucode %08x (0x%x bytes), ucode_data %08x (0x%x bytes)\n",
		code_size ? salsa64_crc32(n64.bus.rdram + code, code_size) : 0, code_size,
		size ? salsa64_crc32(n64.bus.rdram + base, size) : 0, size);
	if (t->type == SALSA64_TASK_AUDIO && size >= 0x34)
		fprintf(stderr, "  audio %s; ucode_data[00 10 28 30]: %08x %08x %08x %08x\n",
			salsa64_abi_name(salsa64_rsp_detect_abi(&n64.bus, t)), salsa64_rd32(n64.bus.rdram + base),
			salsa64_rd32(n64.bus.rdram + base + 0x10), salsa64_rd32(n64.bus.rdram + base + 0x28),
			salsa64_rd32(n64.bus.rdram + base + 0x30));
	/* Microcódigos pequeños: volcar texto y datos. */
	if (t->ucode_size && t->ucode_size <= 0x100) {
		fprintf(stderr, "  ucode:");
		for (uint32_t i = 0; i < t->ucode_size; i += 4)
			fprintf(stderr, " %08x", salsa64_rd32(n64.bus.rdram + ((t->ucode + i) & 0x7FFFFF)));
		fprintf(stderr, "\n  data:");
		for (uint32_t i = 0; i < t->ucode_data_size && i < 0x40; i += 4)
			fprintf(stderr, " %08x", salsa64_rd32(n64.bus.rdram + ((t->ucode_data + i) & 0x7FFFFF)));
		fprintf(stderr, "\n");
	}
}

static void gfx_summary(void)
{
	const struct gfx_result *s = &gfx.sum;
	double n = gfx.tasks;
	fprintf(stderr, "salsa64-run: gfx: %u tareas (%u otras, %u con microcódigo desconocido); por tarea "
		"%.0f comandos, %.0f triángulos, %.0f rectángulos, %.0f dibujos, %.1f cargas a la TMEM, %.1f texturas "
		"decodificadas (+%.1f reusadas, %.1f de tareas anteriores; %.1f al arena, %.1f desalojadas); "
		"%u comandos sin implementar (último 0x%02X), %u de otro microcódigo saltados, %u fondos de S2DEX, "
		"%u combiners aproximados, %u errores, %u tareas cortadas, banderas 0x%02X\n",
		gfx.tasks, n64.bus.rsp.stats.other + n64.bus.rsp.stats.audio, n64.bus.rsp.stats.unknown_gbi,
		s->commands / n, s->triangles / n, s->rects / n, s->draws / n, s->loads / n, s->textures / n,
		s->tex_reused / n, s->tex_old / n, gfx.tex_arena / n, gfx.tex_evicted / n,
		s->unknown_cmds, s->last_unknown, s->skipped, s->bg, s->approx_cc, s->dl_errors, n64.bus.rsp.stats.cut,
		s->flags);
	char hist[256];
	size_t used = 0;
	hist[0] = '\0';
	for (unsigned i = 0; i <= SALSA64_GBI_F3DEX2EUROCOM; i++)
		if (gbi_hist.tasks[i] && used < sizeof hist)
			used += (size_t)snprintf(hist + used, sizeof hist - used, "%s%s %u", used ? ", " : "", salsa64_gbi_name(i),
						 gbi_hist.tasks[i]);
	char imgs[96];
	used = 0;
	imgs[0] = '\0';
	for (int i = 0; i < GFX_SPE0_IMAGES; i++)
		if ((gbi_hist.imgs >> i & 1) && used < sizeof imgs)
			used += (size_t)snprintf(imgs + used, sizeof imgs - used, "%s%s", used ? " + " : "", gfx_spe0_name(i));
	fprintf(stderr, "salsa64-run: gfx por microcódigo: %s; imagen del SPE 0: %s (%u cambios de imagen)\n",
		hist[0] ? hist : "ninguna tarea", imgs[0] ? imgs : "ninguna", gbi_hist.switches);
	for (unsigned i = 0; i < n64.bus.rsp.ucodes_n; i++) {
		const struct salsa64_ucode_seen *u = &n64.bus.rsp.ucodes[i];
		fprintf(stderr, "salsa64-run: gfx: microcódigo %s «%s», código en 0x%06x, CRC-32 %08X (palabras al revés %08X)\n",
			salsa64_gbi_name(u->gbi), u->text[0] ? u->text : "sin texto", (unsigned)u->code,
			(unsigned)u->crc, (unsigned)u->crc_w);
	}
	if (gfx.raster) {
		uint32_t rb, pr, trsx, trd, tdec;
		raster_fb_stats(&rb, &pr, &trsx, &trd, &tdec);
		fprintf(stderr, "salsa64-run: gfx: framebuffer: %u copias a la RDRAM, %u destinos cebados; texturas desde un "
			"destino: %u de su imagen, %u de la RDRAM; %u sin decodificar en la HLE (%.1f por tarea), %u de ellas "
			"decodificadas por el rasterizador\n", rb, pr, trsx, trd, s->tex_target, s->tex_target / n, tdec);
		uint32_t dr, de, dt;
		raster_edge_stats(&dr, &de, &dt);
		fprintf(stderr, "salsa64-run: gfx: aristas de los dibujos con AA: %.1f de %.1f dibujos por tarea (%.0f "
			"triángulos)%s\n", de / n, dr / n, dt / n, raster_aa_edges ? ", encendidas" : ", apagadas");
		uint32_t cvg[4], big;
		raster_alpha_stats(cvg, &big);
		if (cvg[0] || cvg[1] || cvg[2] || cvg[3] || big)
			fprintf(stderr, "salsa64-run: gfx: alfa en los destinos: cobertura de los dibujos: %u con alfa 1, %u con el "
				"de la memoria, %u con el del fragmento, %u con el mayor%s; %u copias grandes de otro ancho que el VI\n",
				cvg[0], cvg[1], cvg[2], cvg[3], raster_cvg_alpha ? "" : " (alfa-fragmento: todos con el del fragmento)",
				big);
		uint32_t d8, t8r, t8m;
		raster_8bit_stats(&d8, &t8r, &t8m);
		fprintf(stderr, "salsa64-run: gfx: imágenes de 8 bits: %u dibujos; texturas I8 de ellas: %u de su imagen, %u "
			"de la RDRAM\n", d8, t8r, t8m);
		uint32_t m32t, m32d, m32p, m32b, m32r;
		raster_mask32_stats(&m32t, &m32d, &m32p, &m32b, &m32r);
		if (m32t || m32r)
			fprintf(stderr, "salsa64-run: gfx: imágenes chicas de 32 bits en software: %u tareas, %u dibujos, %u "
				"píxeles, %u bytes a la RDRAM; %u quedaron para el rasterizador\n", m32t, m32d, m32p, m32b,
				m32r);
		fprintf(stderr, "salsa64-run: gfx: profundidad: %u borrados enteros puestos (z images que el RDP no borra "
			"enteras, como rdp.c)\n", raster_zauto_count());
	}
	if (gfx_late.tear)
		fprintf(stderr, "salsa64-run: gfx: %u tareas comparadas entre el arranque y el fin, %u cambiaron\n",
			gfx_late.compared, gfx_late.torn_tasks);
	if (gfx_late.completions)
		fprintf(stderr, "salsa64-run: gfx: modelo tardío, %u fines; atraso medio %.0f y máximo %u ciclos; "
			"%u esperas al límite\n", gfx_late.completions,
			(double)(gfx_late.completion_calls - gfx_late.completions) * n64.bus.rsp.poll_cycles /
			gfx_late.completions, (gfx_late.completion_max - 1) * n64.bus.rsp.poll_cycles, gfx_late.blocked);
	/* Listas que el CPU le da al RDP por DPC_START/DPC_END, sin pasar por el RSP. */
	if (n64.bus.rsp.stats.dpc_runs)
		fprintf(stderr, "salsa64-run: gfx: RDP desde el CPU (DPC_END): %u tramos, %u comandos, %u triángulos, "
			"%u rectángulos, %u dibujos, %u sin implementar\n", n64.bus.rsp.stats.dpc_runs,
			gfx_sync.dpc_sum.commands, gfx_sync.dpc_sum.triangles, gfx_sync.dpc_sum.rects,
			gfx_sync.dpc_sum.draws, gfx_sync.dpc_sum.unknown_cmds);
}

static void audio_summary(void)
{
	const struct salsa64_audio_stats *a = &aud_sync.stats;
	const struct salsa64_rsp_stats *r = &n64.bus.rsp.stats;
	fprintf(stderr, "salsa64-run: audio: %u tareas (%u con ABI desconocido; último ABI %s), %u comandos, "
		"%u sin implementar (último 0x%08X), %u errores; AI: %u DMA, %u muestras a %u Hz, %u huecos, "
		"%u cambios de frecuencia%s\n", r->audio, r->unknown_abi, salsa64_abi_name(n64.bus.rsp.abi),
		a->commands, a->unknown, a->last_unknown, a->errors, audio.dmas, audio.bytes / 4, audio.rate,
		audio.gaps ? audio.gaps - 1 : 0, audio.rates, audio.lost ? " (no cupo todo)" : "");
}

static void lle_summary(void)
{
	const struct salsa64_lle_host *h = &lle_host;
	const struct gfx_result *d = &gfx_sync.dpc_sum;
	fprintf(stderr, "salsa64-run: rsp lle (%s, %.3f instr/ciclo): %u arranques, %llu instrucciones (%u tareas, "
		"%.0f por tarea, la más larga %u), %u rebanadas, %u sincronías; rdp: %u tramos, %u comandos, "
		"%u triángulos, %u rectángulos, %u dibujos, %u sin implementar, %u errores\n",
		lle_mode == SALSA64_LLE_ALL ? "todo" : "sin HLE", (double)h->ipc_num / h->ipc_den, h->starts,
		(unsigned long long)h->instructions, h->tasks_done,
		h->tasks_done ? (double)h->instructions / h->tasks_done : 0.0, h->task_max, h->slices, h->syncs,
		gfx_sync.dpc_runs, d->commands, d->triangles, d->rects, d->draws, d->unknown_cmds, d->dl_errors);
	const uint32_t *cr = h->rsp.c0_reads, *cw = h->rsp.c0_writes;
	fprintf(stderr, "salsa64-run: rsp lle COP0: SP_STATUS %u lecturas/%u escrituras, semáforo %u/%u, DMA_BUSY/FULL %u, "
		"DMA %u lecturas/%u escrituras, DP %u lecturas/%u escrituras\n", cr[4], cw[4], cr[7], cw[7], cr[5] + cr[6],
		cw[2], cw[3], cr[8] + cr[9] + cr[10] + cr[11] + cr[12] + cr[13] + cr[14] + cr[15],
		cw[8] + cw[9] + cw[10] + cw[11]);
}

static bool audio_wav(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return false;
	uint32_t data = audio.bytes, rate = audio.rate ? audio.rate : 32000;
	uint8_t h[44];
	memcpy(h, "RIFF", 4);
	for (int i = 0; i < 4; i++) h[4 + i] = (uint8_t)((36 + data) >> (8 * i));
	memcpy(h + 8, "WAVEfmt ", 8);
	static const uint8_t fmt[8] = { 16, 0, 0, 0, 1, 0, 2, 0 };  /* PCM, 2 canales */
	memcpy(h + 16, fmt, 8);
	for (int i = 0; i < 4; i++) h[24 + i] = (uint8_t)(rate >> (8 * i));
	for (int i = 0; i < 4; i++) h[28 + i] = (uint8_t)((rate * 4) >> (8 * i));
	h[32] = 4, h[33] = 0, h[34] = 16, h[35] = 0;
	memcpy(h + 36, "data", 4);
	for (int i = 0; i < 4; i++) h[40 + i] = (uint8_t)(data >> (8 * i));
	fwrite(h, 1, sizeof h, f);
	for (uint32_t i = 0; i < data; i += 2) {
		uint8_t le[2] = { audio_buf[i + 1], audio_buf[i] };  /* la N64 es big-endian */
		fwrite(le, 1, 2, f);
	}
	fprintf(stderr, "salsa64-run: audio del AI en %s (%u muestras a %u Hz)\n", path, data / 4, rate);
	return fclose(f) == 0;
}

/* Lo que salió por el AI contra el tono de audiotest. */
static bool audiotest_check(void)
{
	struct tone_gen g = { 0, 0, 0 };
	uint32_t frames = audio.bytes / 4, bad = 0, first = 0;
	for (uint32_t i = 0; i < frames; i++) {
		int16_t l, r;
		tone_next(&g, &l, &r);
		int16_t gl = (int16_t)salsa64_rd16(audio_buf + 4 * i), gr = (int16_t)salsa64_rd16(audio_buf + 4 * i + 2);
		if (gl != l || gr != r) {
			if (!bad++)
				first = i;
		}
	}
	/* Huecos: el primer DMA empieza con la cola vacía; ninguno más. Las dos
	 * fases de la ROM son 180 búferes; el último puede seguir en la cola. */
	uint32_t gaps = audio.gaps ? audio.gaps - 1 : 0;
	bool ok = frames >= 179 * TONE_FRAMES && !bad && !gaps && !audio.rates && !audio.lost;
	char where[48] = "";
	if (bad)
		snprintf(where, sizeof where, ", la primera en la muestra %u", first);
	fprintf(stderr, "salsa64-run: audiotest: audio %s (%u muestras, %u distintas%s, %u huecos, %u Hz)\n",
		ok ? "OK" : "FALLO", frames, bad, where, gaps, audio.rate);
	return ok;
}

/* Imagen del rasterizador para la color image que muestra el VI. */
static const uint32_t *gfx_image(uint32_t *width)
{
	struct salsa64_vi_frame f;
	if (!salsa64_vi_frame(&n64.bus, &f))
		return NULL;
	return raster_image(f.origin, width);
}

static bool gfx_write_ppm(const char *path, const uint32_t *img)
{
	FILE *out = fopen(path, "wb");
	if (!out)
		return false;
	fprintf(out, "P6\n%u %u\n255\n", RASTER_W, RASTER_H);
	for (int i = 0; i < RASTER_W * RASTER_H; i++) {
		uint8_t rgb[3] = { (uint8_t)(img[i] >> 16), (uint8_t)(img[i] >> 8), (uint8_t)img[i] };
		fwrite(rgb, 1, 3, out);
	}
	fprintf(stderr, "salsa64-run: imagen de la HLE gráfica en %s\n", path);
	return fclose(out) == 0;
}

static bool gfx_dump(const char *path)
{
	uint32_t w;
	const uint32_t *img = gfx_image(&w);
	if (!img) {
		fprintf(stderr, "salsa64-run: ninguna tarea gráfica dibujó lo que muestra el VI\n");
		return false;
	}
	return gfx_write_ppm(path, img);
}

static bool gfx_dump_target(const char *path, uint32_t addr)
{
	uint32_t w;
	const uint32_t *img = raster_image(addr, &w);
	if (!img) {
		fprintf(stderr, "salsa64-run: ninguna tarea gráfica dibujó en %06x\n", addr);
		return false;
	}
	return gfx_write_ppm(path, img);
}

static uint8_t *read_file(const char *path, size_t *size);

/* Volcado de la PS3 (big-endian) → orden del host y rasterizador. */
static uint32_t be32(uint32_t v) { return salsa64_rd32((const uint8_t *)&v); }
static void swap32s(void *p, uint32_t bytes)
{
	uint32_t *w = p;
	for (uint32_t i = 0; i < bytes / 4; i++)
		w[i] = be32(w[i]);
}

_Static_assert(offsetof(struct gfx_draw, nterms) % 4 == 0 &&
	       (offsetof(struct gfx_draw, tex) - offsetof(struct gfx_draw, nterms)) % 4 == 0,
	       "bytes del combiner en palabras enteras");

static int raster_dump_buf(uint8_t *buf, size_t size, const char *path, const char *out);

/* Una tarea de la caja negra (docs/cajanegra.md): «ARCHIVO.s4bb[:TAREA]»,
 * por omisión la más nueva. Deja en *buf el volcado de la tarea (que se
 * suelta con free) y su tamaño. */
static bool caja_task(const char *spec, uint8_t **buf, size_t *size)
{
	char file[512];
	long want = -1;
	snprintf(file, sizeof file, "%s", spec);
	char *colon = strrchr(file, ':');
	if (colon && colon[1] && strspn(colon + 1, "0123456789") == strlen(colon + 1)) {
		want = strtol(colon + 1, NULL, 10);
		*colon = '\0';
	}
	size_t fsize;
	uint8_t *f = read_file(file, &fsize);
	struct bb_file_info info;
	if (!f || fsize > UINT32_MAX || !bb_file_info(f, (uint32_t)fsize, &info)) {
		fprintf(stderr, "salsa64-run: %s no es un archivo de la caja negra\n", file);
		free(f);
		return false;
	}
	uint32_t pos = 0;
	struct bb_file_rec rec, pick;
	bool found = false;
	while (bb_file_next(f, (uint32_t)fsize, &pos, &rec)) {
		if (rec.kind != BB_TASK || (want >= 0 && rec.id != (uint32_t)want))
			continue;
		pick = rec;
		found = true;  /* sin TAREA, la última (la más nueva) */
		if (want >= 0)
			break;
	}
	if (!found) {
		fprintf(stderr, "salsa64-run: %s no tiene la tarea %ld\n", file, want);
		free(f);
		return false;
	}
	uint8_t *content = malloc(pick.len);
	if (!content || pick.len <= 4 * BB_TASK_WORDS || !bb_file_content(&pick, content)) {
		fprintf(stderr, "salsa64-run: la tarea %u de %s está dañada\n", pick.id, file);
		free(content);
		free(f);
		return false;
	}
	fprintf(stderr, "salsa64-run: caja negra %s: tarea %u (campo %u, %u dibujos)\n", file, pick.id,
		salsa64_rd32(content + 4 * BB_TASK_FIELD), salsa64_rd32(content + 4 * BB_TASK_DRAWS));
	memmove(content, content + 4 * BB_TASK_WORDS, pick.len - 4 * BB_TASK_WORDS);
	*buf = content;
	*size = pick.len - 4 * BB_TASK_WORDS;
	free(f);
	return true;
}

static int raster_dump(const char *path, const char *out)
{
	size_t size;
	uint8_t *buf;
	if (strstr(path, ".s4bb")) {
		if (!caja_task(path, &buf, &size))
			return 3;
	} else {
		buf = read_file(path, &size);
	}
	return raster_dump_buf(buf, size, path, out);
}

static int raster_dump_buf(uint8_t *buf, size_t size, const char *path, const char *out)
{
	if (!buf || size < sizeof(struct gfx_dump_header)) {
		fprintf(stderr, "salsa64-run: no se pudo leer %s\n", path);
		return 3;
	}
	struct gfx_dump_header h;
	memcpy(&h, buf, sizeof h);
	swap32s(&h, sizeof h);
	if (h.magic != GFX_DUMP_MAGIC || sizeof h + (size_t)h.cmd_bytes + h.vtx_bytes + h.tex_bytes > size) {
		fprintf(stderr, "salsa64-run: %s no es un volcado de la HLE\n", path);
		return 3;
	}
	uint8_t *cmd = buf + sizeof h, *vtx = cmd + h.cmd_bytes, *tex = vtx + h.vtx_bytes;
	/* Comandos: cabeceras (op y size de 16 bits) y el resto palabras de 32. */
	for (uint32_t off = 0; off + sizeof(struct gfx_cmd) <= h.cmd_bytes;) {
		struct gfx_cmd *c = (struct gfx_cmd *)(cmd + off);
		uint16_t op = salsa64_rd16((uint8_t *)&c->op), sz = salsa64_rd16((uint8_t *)&c->size);
		c->op = op, c->size = sz;
		swap32s(&c->a, sz - 4);
		if (op == GFX_OP_ZCLEAR) {
			struct gfx_zclear *z = (struct gfx_zclear *)c;
			swap32s(z->rect, sizeof z->rect);  /* deshace: son de 16 bits */
			for (int i = 0; i < 4; i++)
				z->rect[i] = salsa64_rd16((uint8_t *)&z->rect[i]);
		} else if (op == GFX_OP_DRAW) {
			struct gfx_draw *d = (struct gfx_draw *)c;
			swap32s(d->scissor, 8 + 8);  /* deshace scissor y los bytes de estado */
			for (int i = 0; i < 4; i++)
				d->scissor[i] = salsa64_rd16((uint8_t *)&d->scissor[i]);
			swap32s(&d->prim_tile, 4);  /* deshace: tile de 16 bits, cvg_bit y relleno */
			d->prim_tile = salsa64_rd16((uint8_t *)&d->prim_tile);
			/* Bytes de nterms a pad: factores, viscvg y aa. */
			swap32s(&d->nterms, offsetof(struct gfx_draw, tex) - offsetof(struct gfx_draw, nterms));
			for (int t = 0; t < 2; t++) {
				struct gfx_tex *x = &d->tex[t];
				swap32s(&x->width, 8);  /* deshace los de 16 y 8 bits */
				x->width = salsa64_rd16((uint8_t *)&x->width);
				x->height = salsa64_rd16((uint8_t *)&x->height);
				x->pitch = salsa64_rd16((uint8_t *)&x->pitch);
			}
		}
		if (op == GFX_OP_END || !sz)
			break;
		off += sz;
	}
	/* Vértices: 6 floats y un uint32 de relleno; el color son bytes. */
	for (uint32_t i = 0; i + sizeof(struct gfx_vertex) <= h.vtx_bytes; i += sizeof(struct gfx_vertex)) {
		struct gfx_vertex *v = (struct gfx_vertex *)(vtx + i);
		swap32s(v, 24);
	}
	struct gfx_job job;
	struct gfx_result res;
	memset(&job, 0, sizeof job);
	memset(&res, 0, sizeof res);
	job.cmd_ea = (uint64_t)(uintptr_t)cmd;
	job.vtx_ea = (uint64_t)(uintptr_t)vtx;
	job.tex_ea = (uint64_t)(uintptr_t)tex;
	res.cmd_bytes = h.cmd_bytes;
	res.vtx_bytes = h.vtx_bytes;
	res.tex_bytes = h.tex_bytes;
	raster_task(&job, &res);
	/* La primera color image del volcado. */
	const struct gfx_cmd *c = (const struct gfx_cmd *)cmd;
	uint32_t addr = 0, w;
	for (uint32_t off = 0; off < h.cmd_bytes && c->size; off += c->size, c = (const struct gfx_cmd *)(cmd + off))
		if (c->op == GFX_OP_TARGET) {
			addr = c->a;
			break;
		}
	const uint32_t *img = raster_image(addr, &w);
	if (!img || !out) {
		fprintf(stderr, "salsa64-run: el volcado no dibuja ninguna color image (o falta --gfx-frame)\n");
		return 3;
	}
	FILE *f = fopen(out, "wb");
	fprintf(f, "P6\n%u %u\n255\n", RASTER_W, RASTER_H);
	for (int i = 0; i < RASTER_W * RASTER_H; i++) {
		uint8_t rgb[3] = { (uint8_t)(img[i] >> 16), (uint8_t)(img[i] >> 8), (uint8_t)img[i] };
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
	fprintf(stderr, "salsa64-run: volcado de %s dibujado en %s (color image %06x)\n", path, out, addr);
	free(buf);
	return 0;
}

/* Puntos de gfxtest (expect.h) en la imagen del rasterizador. */
static bool gfxtest_check(void)
{
	static const struct gfxtest_point pts[] = { GFXTEST_POINTS };
	uint32_t w;
	const uint32_t *img = gfx_image(&w);
	if (!img) {
		fprintf(stderr, "salsa64-run: gfxtest: no hay imagen de la HLE\n");
		return false;
	}
	unsigned bad = 0;
	for (unsigned i = 0; i < sizeof pts / sizeof pts[0]; i++) {
		uint32_t got = img[pts[i].y * RASTER_W + pts[i].x], want = pts[i].rgb;
		int ok = 1;
		for (int k = 0; k < 3; k++) {
			int d = (int)((got >> (16 - 8 * k)) & 0xFF) - (int)((want >> (16 - 8 * k)) & 0xFF);
			if (d < -GFXTEST_TOL || d > GFXTEST_TOL)
				ok = 0;
		}
		if (!ok) {
			bad++;
			fprintf(stderr, "salsa64-run: gfxtest: (%u, %u) %s: 0x%06X, se esperaba 0x%06X\n", pts[i].x,
				pts[i].y, pts[i].what, got, want);
		}
	}
	fprintf(stderr, "salsa64-run: gfxtest: imagen %s (%u de %zu puntos bien)\n", bad ? "FALLO" : "OK",
		(unsigned)(sizeof pts / sizeof pts[0]) - bad, sizeof pts / sizeof pts[0]);
	return bad == 0;
}

/* Búsqueda de texto en la salida, también entre dos impresiones. */
static struct {
	const char *until, *fail[MAX_FAIL_TEXTS];
	unsigned nfail;
	size_t keep;
	size_t tail_len;
	char window[MAX_TEXT + SALSA64_ISV_SIZE];
	bool quiet;
	bool midline;  /* la última impresión no terminó en salto de línea */
	enum stop_reason hit;
} watch;

static void isv_out(void *ctx, const uint8_t *text, uint32_t len)
{
	(void)ctx;
	if (!watch.quiet && len) {
		fwrite(text, 1, len, stdout);
		watch.midline = text[len - 1] != '\n';
	}

	size_t n = watch.tail_len + len;
	memcpy(watch.window + watch.tail_len, text, len);
	if (watch.hit == STOP_NONE) {
		for (unsigned k = 0; k < watch.nfail; k++)
			if (memmem(watch.window, n, watch.fail[k], strlen(watch.fail[k])))
				watch.hit = STOP_FAIL;
		if (watch.hit == STOP_NONE && watch.until &&
		    memmem(watch.window, n, watch.until, strlen(watch.until)))
			watch.hit = STOP_UNTIL;
		if (watch.hit != STOP_NONE)
			salsa64_n64_stop(&n64);
	}
	watch.tail_len = n < watch.keep ? n : watch.keep;
	memmove(watch.window, watch.window + n - watch.tail_len, watch.tail_len);
}

static void ref_isv_out(void *ctx, const uint8_t *text, uint32_t len)
{
	(void)ctx, (void)text, (void)len;
}

#define DIFF(name, a, b)                                                                        \
	do {                                                                                    \
		if ((uint64_t)(a) != (uint64_t)(b)) {                                           \
			fprintf(stderr, "  %-10s rec=%016llx intérprete=%016llx\n", name,      \
				(unsigned long long)(a), (unsigned long long)(b));              \
			bad = true;                                                             \
		}                                                                               \
	} while (0)

/* Después de cada salida del código generado: la referencia al mismo ciclo y
 * comparar. */
static void lockstep_check(struct salsa64_rec *r, uint64_t block_pc, void *ctx)
{
	(void)r, (void)ctx;
	const struct salsa64_cpu *a = &n64.cpu, *b = &ref.cpu;
	lock.history[lock.exits % 16] = block_pc;
	lock.exits++;
	/* La referencia despacha los eventos que vencen en el ciclo al que
	 * llega; el despachador del recompilador lo haría en su próxima vuelta. */
	if (a->cycles >= n64.sched.next)
		salsa64_sched_dispatch(&n64.sched, a->cycles);
	ref_run_to(a->cycles);

	bool bad = false;
	char name[16];
	DIFF("cycles", a->cycles, b->cycles);
	DIFF("pc", a->pc, b->pc);
	DIFF("next_pc", a->next_pc, b->next_pc);
	DIFF("branch", a->branch, b->branch);
	for (int k = 1; k < 32; k++) {
		snprintf(name, sizeof name, "r%d", k);
		DIFF(name, a->gpr[k], b->gpr[k]);
	}
	DIFF("hi", a->hi, b->hi);
	DIFF("lo", a->lo, b->lo);
	for (int k = 0; k < 32; k++) {
		snprintf(name, sizeof name, "f%d", k);
		DIFF(name, a->fpr[k], b->fpr[k]);
	}
	DIFF("fcr31", a->fcr31, b->fcr31);
	DIFF("status", a->status, b->status);
	DIFF("cause", a->cause, b->cause);
	DIFF("epc", a->epc, b->epc);
	DIFF("errorepc", a->error_epc, b->error_epc);
	DIFF("badvaddr", a->bad_vaddr, b->bad_vaddr);
	DIFF("compare", a->compare, b->compare);
	DIFF("count_off", a->count_offset, b->count_offset);
	DIFF("llbit", a->llbit, b->llbit);
	DIFF("entryhi", a->entry_hi, b->entry_hi);
	DIFF("mi.intr", n64.bus.mi.intr, ref.bus.mi.intr);
	DIFF("vi.fields", n64.bus.vi.fields, ref.bus.vi.fields);
	if (!bad && (lock.exits % lock.ram_every == 0 || n64.bus.vi.fields != lock.fields)) {
		lock.fields = n64.bus.vi.fields;
		for (uint32_t pa = 0; pa < n64.bus.rdram_size; pa += 4) {
			if (salsa64_rd32(rdram + pa) != salsa64_rd32(ref_rdram + pa)) {
				fprintf(stderr, "  RDRAM %08x rec=%08x intérprete=%08x\n", pa,
					salsa64_rd32(rdram + pa), salsa64_rd32(ref_rdram + pa));
				bad = true;
				break;
			}
		}
	}
	if (!bad)
		return;
	fprintf(stderr, "salsa64-run: --lockstep: diferencia después del bloque %016llx (salida %llu, ciclo %llu)\n"
		"  bloques anteriores:", (unsigned long long)block_pc, (unsigned long long)lock.exits,
		(unsigned long long)a->cycles);
	for (unsigned k = 1; k <= 16 && k <= lock.exits; k++)
		fprintf(stderr, " %08llx", (unsigned long long)(uint32_t)lock.history[(lock.exits - k) % 16]);
	fputc('\n', stderr);
	exit(4);
}

static void save_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("salsa64-run: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

/* Instrucción en una dirección de KSEG0/KSEG1, o 0 si no cae en la RDRAM. */
static uint32_t peek(uint64_t va)
{
	uint32_t pa = (uint32_t)va & 0x1FFFFFFF;
	if ((va >> 30) != 0x3FFFFFFFEull || pa + 4 > n64.bus.rdram_size)
		return 0;
	return salsa64_rd32(n64.bus.rdram + pa);
}

static uint64_t trace_exc_left;

static void trace_exc(struct salsa64_cpu *cpu)
{
	if (!trace_exc_left || !tracing() || (trace_exc_faults_only && SALSA64_CAUSE_EXC(cpu->cause) == 0))
		return;
	trace_exc_left--;
	fprintf(stderr, "excepción %2u en ciclo %llu: pc=%016llx [%08x] epc=%016llx bd=%u badvaddr=%016llx status=%08x "
		"ip=%02x mi=%02x/%02x\n",
		SALSA64_CAUSE_EXC(cpu->cause), (unsigned long long)cpu->cycles,
		(unsigned long long)cpu->cur_pc, peek(cpu->cur_pc), (unsigned long long)cpu->epc,
		cpu->cause >> 31, (unsigned long long)cpu->bad_vaddr, cpu->status, (cpu->cause >> 8) & 0xFF,
		n64.bus.mi.intr & 0x3F, n64.bus.mi.mask & 0x3F);
}

/* `b .` (o `j .`) con NOP en el delay slot y sin interrupción que lo saque.
 * El bloque puede terminar sobre el salto o sobre su delay slot. */
static bool cpu_stuck(void)
{
	const struct salsa64_cpu *cpu = &n64.cpu;
	uint64_t at = cpu->branch ? cpu->pc - 4 : cpu->pc;
	uint32_t i = peek(at);
	bool self = i == 0x1000FFFFu ||
		    (i >> 26 == 2 && (i & 0x03FFFFFF) == (((uint32_t)at >> 2) & 0x03FFFFFF));
	if (!self || peek(at + 4) != 0 || (cpu->branch && cpu->next_pc != at))
		return false;
	uint32_t st = cpu->status;
	bool irq_possible = (st & SALSA64_ST_IE) && !(st & (SALSA64_ST_EXL | SALSA64_ST_ERL)) &&
			    (st & SALSA64_ST_IM);
	return !irq_possible;
}

static uint8_t *read_file(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	uint8_t *buf = NULL;
	long len;
	if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0 &&
	    (buf = malloc((size_t)len)) && fread(buf, 1, (size_t)len, f) == (size_t)len) {
		*size = (size_t)len;
	} else {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	return buf;
}

/* ROM por fragmentos (--rom-paged): el archivo queda abierto, la caché es de malloc y la cabecera de 4 KB, ya en orden
 * z64, es lo que devuelve (se suelta con free). El resto lo pide el bus a salsa64_rompg_read. */
static int rom_file_rd(void *user, uint64_t off, uint8_t *dst, size_t len)
{
	FILE *f = user;
	if (fseek(f, (long)off, SEEK_SET) != 0)
		return -1;
	return (int)fread(dst, 1, len, f);
}

static uint8_t *read_rom_paged(const char *path, size_t *size, unsigned page_kb, unsigned cache_kb,
			       struct salsa64_rompg *pg)
{
	FILE *f = fopen(path, "rb");
	long len;
	unsigned shift = 10;
	uint8_t *head = malloc(4096);
	uint8_t *cache = malloc((size_t)cache_kb << 10);

	while ((1u << (shift - 10)) < page_kb)
		shift++;
	if (!f || !head || !cache || fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0x1000 ||
	    (uint64_t)len > 0xFC00000u || fseek(f, 0, SEEK_SET) != 0 || fread(head, 1, 4096, f) != 4096) {
		if (f)
			fclose(f);
		free(head);
		free(cache);
		return NULL;
	}
	enum salsa64_rom_format fmt = salsa64_rom_normalize(head, 4096);
	if (salsa64_rompg_open(pg, rom_file_rd, f, (uint32_t)len, fmt, shift, cache, (size_t)cache_kb << 10) != 0) {
		fclose(f);
		free(head);
		free(cache);
		return NULL;
	}
	fprintf(stderr, "salsa64-run: ROM %ld bytes por fragmentos (páginas de %u KB, caché de %u KB, %u ranuras)\n", len,
		page_kb, cache_kb, pg->nslots);
	*size = (size_t)len;
	return head;
}

static void dump_regs(void)
{
	static const char *const names[32] = {
		"zr", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
		"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra",
	};
	const struct salsa64_cpu *cpu = &n64.cpu;
	for (int r = 0; r < 32; r++)
		fprintf(stderr, "%s=%016llx%s", names[r], (unsigned long long)cpu->gpr[r], r % 4 == 3 ? "\n" : " ");
	fprintf(stderr, "pc=%016llx status=%08x cause=%08x epc=%016llx badvaddr=%016llx\n",
		(unsigned long long)cpu->pc, cpu->status, cpu->cause,
		(unsigned long long)cpu->epc, (unsigned long long)cpu->bad_vaddr);
	const struct salsa64_bus *b = &n64.bus;
	fprintf(stderr, "MI intr=%02x mask=%02x; VI control=%08x origin=%08x v_intr=%u campos=%u; SP status=%04x; "
		"PI status=%x; tareas RSP: gfx %u, audio %u, otras %u, microcódigo desconocido %u\n",
		b->mi.intr, b->mi.mask, b->vi.regs[SALSA64_VI_CONTROL], b->vi.regs[SALSA64_VI_ORIGIN],
		b->vi.regs[SALSA64_VI_V_INTR], b->vi.fields, b->sp.regs[SALSA64_SP_STATUS], b->pi[SALSA64_PI_STATUS],
		b->rsp.stats.gfx, b->rsp.stats.audio, b->rsp.stats.other, b->rsp.stats.unknown_gbi);
	fprintf(stderr, "PIF RAM:");
	for (int i = 0; i < 64; i += 4)
		fprintf(stderr, " %08x", salsa64_rd32(b->pif_ram + i));
	fprintf(stderr, "\nSI status=%x dram=%08x\n", salsa64_si_read(b, SALSA64_SI_STATUS << 2),
		b->si[SALSA64_SI_DRAM_ADDR]);
#ifdef SALSA64_DEBUG_SPMEM
	extern unsigned salsa64_debug_irq[6];
	fprintf(stderr, "interrupciones: SP %u SI %u AI %u VI %u PI %u DP %u\n", salsa64_debug_irq[0],
		salsa64_debug_irq[1], salsa64_debug_irq[2], salsa64_debug_irq[3], salsa64_debug_irq[4],
		salsa64_debug_irq[5]);
#endif
}

/* Imagen del VI en PPM (P6), RGBA5551 o RGBA8888 → RGB de 8 bits. */
static bool dump_frame(const char *path)
{
	struct salsa64_vi_frame f;
	if (!salsa64_vi_frame(&n64.bus, &f)) {
		fprintf(stderr, "salsa64-run: el VI no muestra imagen (VI_CONTROL = 0x%08x)\n",
			n64.bus.vi.regs[SALSA64_VI_CONTROL]);
		return false;
	}
	FILE *out = fopen(path, "wb");
	if (!out)
		return false;
	fprintf(out, "P6\n%u %u\n255\n", f.width, f.height);
	for (uint32_t y = 0; y < f.height; y++) {
		const uint8_t *line = n64.bus.rdram + f.origin + y * f.stride;
		for (uint32_t x = 0; x < f.width; x++) {
			uint8_t rgb[3];
			if (f.bpp == 2) {
				uint32_t v = salsa64_rd16(line + 2 * x);
				uint32_t c[3] = { v >> 11, (v >> 6) & 31, (v >> 1) & 31 };
				for (int k = 0; k < 3; k++)
					rgb[k] = (uint8_t)(c[k] << 3 | c[k] >> 2);
			} else {
				memcpy(rgb, line + 4 * x, 3);
			}
			fwrite(rgb, 1, 3, out);
		}
	}
	fprintf(stderr, "salsa64-run: imagen del VI %ux%u (%s, origen 0x%06x) en %s; %u campos\n",
		f.width, f.height, f.bpp == 2 ? "RGBA5551" : "RGBA8888", f.origin, path,
		n64.bus.vi.fields);
	return fclose(out) == 0;
}

/* Opciones largas sin letra (códigos fuera de ASCII). */
enum { OPT_REC = 0x100, OPT_LOCKSTEP, OPT_PAD, OPT_TRACE_FROM, OPT_TRACE_FAULTS, OPT_RAM_DUMP, OPT_TRACE_AUDIO,
       OPT_FRAME_EVERY, OPT_NO_PAK, OPT_LLE, OPT_LLE_SPEED, OPT_LLE_CHECK, OPT_LLE_CHECK_JPEG, OPT_LLE_CHECK_GFX, OPT_POKE, OPT_TRACE_FB, OPT_STATE_SAVE,
       OPT_STATE_LOAD, OPT_FIELDS, OPT_PROFILE, OPT_STATE_ZIP, OPT_INTERP, OPT_INTERP_K, OPT_INTERP_DECIMATE,
       OPT_INTERP_DISP, OPT_PERFILADO, OPT_STOP_GFX, OPT_PADS, OPT_PAD_PORT, OPT_RESET_AT, OPT_NMI_AT, OPT_ROM_PAGED };

/* ---- Estados guardados (--state-save, --state-load) ------------------------- */

#define MAX_STATE_SAVES 8
static struct {
	uint32_t field[MAX_STATE_SAVES];
	const char *path[MAX_STATE_SAVES];
	unsigned n;
} state_saves;
static const char *state_load_path;
static uint32_t stop_field;
static uint8_t state_scratch[SALSA64_STATE_SCRATCH] __attribute__((aligned(16)));
static bool state_zip;  /* --state-zip: la RDRAM comprimida (formato 2) */
static uint8_t zip_small[SALSA64_STATE_SECTIONS] __attribute__((aligned(16)));
static uint8_t zip_ram[SALSA64_RDRAM_MAX] __attribute__((aligned(16)));
static struct salsa64_state_lz zip_lz;

static bool state_fwrite(void *ctx, const void *data, uint32_t len)
{
	return fwrite(data, 1, len, ctx) == len;
}

static bool state_fread(void *ctx, void *data, uint32_t len)
{
	return fread(data, 1, len, ctx) == len;
}

static bool state_fpatch(void *ctx, uint32_t offset, const uint8_t *data, uint32_t len)
{
	return fseek(ctx, offset, SEEK_SET) == 0 && fwrite(data, 1, len, ctx) == len;
}

static bool state_save_zip(FILE *f, char *err, size_t err_size)
{
	struct salsa64_state_snap snap = { zip_small, zip_ram, 0, 0 };
	return salsa64_state_capture(&n64, NULL, &snap, sizeof zip_small, sizeof zip_ram, err, err_size) &&
	       salsa64_state_write(&snap, salsa64_state_compress_host, &zip_lz, state_fwrite, state_fpatch, f, err,
				   err_size);
}

static bool state_save_file(const char *path)
{
	char err[160];
	if (!salsa64_rsp_quiesce(&n64.bus)) {
		fprintf(stderr, "salsa64-run: --state-save: sin estados con --lle\n");
		return false;
	}
	FILE *f = fopen(path, "wb");
	bool ok = f && (state_zip ? state_save_zip(f, err, sizeof err) :
			salsa64_state_save(&n64, NULL, state_fwrite, f, state_scratch, err, sizeof err));
	if (f && fclose(f) != 0)
		ok = false;
	if (!ok) {
		fprintf(stderr, "salsa64-run: no se pudo guardar el estado en %s: %s\n", path, f ? err : "no se abre");
		return false;
	}
	if (state_zip) {
		FILE *g = fopen(path, "rb");
		long sz = g && fseek(g, 0, SEEK_END) == 0 ? ftell(g) : -1;
		if (g)
			fclose(g);
		fprintf(stderr, "salsa64-run: estado del campo %u en %s (%ld bytes comprimido, de %u)\n", n64.bus.vi.fields,
			path, sz, salsa64_state_size(&n64, false));
	} else {
		fprintf(stderr, "salsa64-run: estado del campo %u en %s (%u bytes)\n", n64.bus.vi.fields, path,
			salsa64_state_size(&n64, false));
	}
	return true;
}

static bool state_load_file(struct salsa64_n64 *m, const char *path)
{
	char err[160] = "no se abre";
	bool broken = false;
	FILE *f = fopen(path, "rb");
	/* Con --lle, el estado (guardado con la HLE) se carga con el LLE
	 * apagado y se sigue con el microcódigo real: así se comparan las dos
	 * en la misma escena sin correr todo el juego con el LLE. */
	enum salsa64_lle_mode mode = m->bus.rsp.lle_mode;
	m->bus.rsp.lle_mode = SALSA64_LLE_OFF;
	bool ok = f && salsa64_rsp_quiesce(&m->bus) &&
		  salsa64_state_load(m, state_fread, f, state_scratch, err, sizeof err, &broken);
	m->bus.rsp.lle_mode = mode;
	if (f)
		fclose(f);
	if (!ok) {
		fprintf(stderr, "salsa64-run: no se pudo cargar el estado %s: %s\n", path, err);
		return false;
	}
	if (m == &n64)
		fprintf(stderr, "salsa64-run: estado %s cargado (campo %u, ciclo %llu)\n", path, m->bus.vi.fields,
			(unsigned long long)m->cpu.cycles);
	return true;
}

/* Botón Reset (--reset-at C: PRE_NMI al terminar el campo C y NMI medio
 * segundo emulado después) y NMI directo (--nmi-at C), como en la consola. */
static uint32_t reset_at, nmi_at_opt;
static int host_tv;

static bool reset_fields_done(void)
{
	uint32_t f = n64.bus.vi.fields;
	if (reset_at && f == reset_at) {
		salsa64_n64_prenmi(&n64);
		fprintf(stderr, "salsa64-run: campo %u: PRE_NMI (cause %08x), NMI en %.2f s\n", f, n64.cpu.cause,
			SALSA64_NMI_DELAY_CYCLES / 93750000.0);
	}
	if ((nmi_at_opt && f == nmi_at_opt) || salsa64_n64_nmi_due(&n64)) {
		struct salsa64_boot_info info;
		if (!salsa64_boot_nmi(&n64, host_tv, &info)) {
			fprintf(stderr, "salsa64-run: el NMI no arrancó: %s\n", info.error);
			return false;
		}
		fprintf(stderr, "salsa64-run: campo %u: NMI, osResetType %u, entrada %08x\n", f,
			(unsigned)salsa64_rd32(n64.bus.rdram + 0x30C), info.entry);
	}
	return true;
}

/* Al terminar cada campo: los --state-save que tocan. */
static bool state_fields_done(void)
{
	for (unsigned k = 0; k < state_saves.n; k++)
		if (state_saves.field[k] == n64.bus.vi.fields && !state_save_file(state_saves.path[k]))
			return false;
	return true;
}

static void usage(FILE *out)
{
	fputs("uso: salsa64-run [opciones] rom\n"
	      "  -c, --cycles N        límite de ciclos del VR4300 (por omisión 2^32)\n"
	      "  -u, --until-text T    terminar (código 0) cuando el ISViewer imprima T\n"
	      "  -f, --fail-text T     terminar con código 1 si el ISViewer imprime T (hasta 4)\n"
	      "      --tv ntsc|pal|mpal  tipo de TV (por omisión, según el encabezado)\n"
	      "      --ram 4|8         MB de RDRAM (por omisión 8)\n"
	      "      --slice N         ciclos por bloque entre verificaciones (2^20)\n"
	      "  -q, --quiet           no copiar la salida del ISViewer a stdout\n"
	      "  -r, --regs            volcar los registros del CPU al terminar\n"
	      "      --trace-exc N     mostrar las primeras N excepciones del CPU\n"
	      "      --trace-faults N  igual, sin las interrupciones\n"
	      "      --trace-from C    trazar solo desde el campo C del VI\n"
	      "      --pad GUION       control 1 por campos: \"300:START;306:-;400:A+Z@0,80\"\n"
	      "                        (también CAMPO:BOTONES,… con x80/y-50); vale hasta el siguiente\n"
	      "      --poke GUION      escribir la RDRAM al empezar un campo: \"4300:8033B21E=0x0400\"\n"
	      "                        (8/16/32 bits según los dígitos del valor)\n"
	      "      --frame ARCHIVO   al terminar, guardar en PPM la imagen del VI\n"
	      "      --ram-dump ARCHIVO  al terminar, volcar la RDRAM (tools/libultra-threads.py)\n"
	      "      --rec             usar el recompilador (host ppc64 big-endian)\n"
	      "      --lockstep[=N]    con --rec, comparar contra el intérprete después de cada\n"
	      "                        bloque, y la RDRAM en cada campo del VI y cada N bloques\n"
	      "                        (por omisión 4096)\n"
	      "      --no-idle         no saltar los bucles de espera\n"
	      "      --rom-paged PAG_KB,CACHE_KB  lee la ROM por fragmentos del archivo (caché LRU) en vez de entera\n"
	      "      --perfilado DIR   la sesión de la app de perfilado (docs/perfilado.md) en\n"
	      "                        DIR/<juego>-<CRC>/<n>.sesion; SALSA64_PERFILADO_CFG con las\n"
	      "                        claves de perfilado.cfg separadas por comas\n"
	      "      --profile ARCHIVO con --rec, perfil persistente por juego (rec_profile.h): se carga al\n"
	      "                        empezar (si existe) y se guarda al terminar; SALSA64_REC_HOT_KB fija la\n"
	      "                        zona caliente (1024) y SALSA64_REC_SITES=0 no registra sitios de salida\n"
	      "      --save TIPO       tipo de guardado: ninguno, eeprom4k, eeprom16k, sram256k, flashram\n"
	      "      --save-dir DIR    leer y escribir el guardado del cartucho y el Controller Pak (.mpk) en DIR\n"
	      "      --sin-pak         sin Controller Pak en los controles (por omisión hay uno en cada uno)\n"
	      "      --pad-port N      puerto (1 a 4) al que va el guion de --pad (por omisión 1)\n"
	      "      --pads N          controles conectados (1 a 4; por omisión 1, como la consola recién\n"
	      "                        encendida; la PS3 conecta un control por cada mando que ve)\n"
	      "      --gfx-frame ARCHIVO  al terminar, guardar en PPM lo que dibujó la HLE gráfica\n"
	      "      --gfx-frame-every N  con --pad, además cada N campos desde --trace-from (ARCHIVO-<campo>.ppm)\n"
	      "      --stop-gfx N:ARCHIVO  al terminar la N-ésima lista gráfica (G_RDPFULLSYNC) guardar en PPM la\n"
	      "                        última color image que dibujó y terminar: la misma tarea con la HLE y con\n"
	      "                        --lle=all (desde un --state-load)\n"
	      "      --gfxtest-check   verificar la imagen de la HLE con los puntos de gfxtest\n"
	      "      --trace-rsp N     mostrar las primeras N tareas del RSP\n"
	      "      --trace-audio N   opcodes de la lista de audio de las primeras N tareas de audio\n"
	      "      --trace-vi N      mostrar la imagen del VI en los últimos N campos (o «sin imagen» con\n"
	      "                        los registros que la apagan)\n"
	      "      --trace-fb        efectos de framebuffer: dibujos que leen un destino y, con el\n"
	      "                        build -DSALSA64_WATCH_MEM, accesos del CPU a los destinos (con --gfx-frame)\n"
	      "      --audio-wav ARCHIVO  al terminar, guardar en WAV lo que salió por el AI\n"
	      "      --audiotest-check comparar lo que salió por el AI con el tono de audiotest\n"
	      "      --raster-dump F   dibujar un volcado de la HLE de la PS3 (gfxdump.bin; o una tarea\n"
	      "                        de la caja negra: caja.s4bb[:TAREA], por omisión la más nueva) en\n"
	      "                        --gfx-frame y salir (sin ROM)\n"
	      "      --ares [...] RDRAM  (primera opción) comparar la HLE con un volcado de la RDRAM\n"
	      "                        de ares, sin ROM (salsa64-run --ares --help)\n"
	      "      --lle[=auto|all]  RSP a bajo nivel: el microcódigo real para lo que no tiene\n"
	      "                        HLE (auto) o para todo (all); el RDP consume sus comandos\n"
	      "      --lle-speed F     instrucciones del RSP por ciclo del CPU (nominal 0,667)\n"
	      "      --lle-check-audio con --lle=all, comparar cada tarea de audio con la HLE\n"
	      "      --lle-check-gfx   con --lle=all (lo pone), comparar cada tarea gráfica con la HLE: los\n"
	      "                        píxeles que dejan los dos en el mismo rasterizador (docs/gfx-censo.md)\n"
	      "      --lle-check-jpeg  las tareas JPEG y las de video de Resident Evil 2 al microcódigo\n"
	      "                        real (implica --lle=auto) y compararlas con la HLE (rsp/jpeg);\n"
	      "                        SALSA64_RE2_DUMP=archivo[:N] vuelca las primeras N de cada tipo de video\n"
	      "      --state-save C:ARCHIVO  guardar un estado al terminar el campo C (hasta 8)\n"
	      "      --state-load ARCHIVO    cargar un estado después del arranque y seguir desde ahí\n"
	      "      --state-zip             los --state-save con la RDRAM comprimida (formato 2, como la PS3)\n"
	      "      --fields N        terminar (código 0) al acabar el campo N del VI\n"
	      "      --reset-at C      botón Reset al acabar el campo C: PRE_NMI y, 0,5 s emulados después, NMI\n"
	      "      --nmi-at C        NMI (reset suave, RDRAM intacta, osResetType = 1) al acabar el campo C\n"
	      "      --interp DIR      prototipo de interpolación de cuadros (docs/interpolacion.md): desde\n"
	      "                        --trace-from, cuadros intermedios por geometría y por imagen en DIR\n"
	      "      --interp-k K      campos por cuadro del juego (3: intermedios en 1/3 y 2/3)\n"
	      "      --interp-decimate 1 de cada K tareas es el cuadro del juego; las demás, la verdad\n"
	      "      --interp-disp PX  desplazamiento por cuadro (píxeles de la N64) que ya es un corte (120)\n", out);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "cycles", required_argument, NULL, 'c' },
		{ "until-text", required_argument, NULL, 'u' },
		{ "fail-text", required_argument, NULL, 'f' },
		{ "tv", required_argument, NULL, 'T' },
		{ "ram", required_argument, NULL, 'R' },
		{ "slice", required_argument, NULL, 'S' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "regs", no_argument, NULL, 'r' },
		{ "trace-exc", required_argument, NULL, 'X' },
		{ "frame", required_argument, NULL, 'F' },
		{ "rec", no_argument, NULL, OPT_REC },
		{ "lockstep", optional_argument, NULL, OPT_LOCKSTEP },
		{ "pad", required_argument, NULL, OPT_PAD },
		{ "poke", required_argument, NULL, OPT_POKE },
		{ "trace-from", required_argument, NULL, OPT_TRACE_FROM },
		{ "trace-faults", required_argument, NULL, OPT_TRACE_FAULTS },
		{ "ram-dump", required_argument, NULL, OPT_RAM_DUMP },
		{ "trace-audio", required_argument, NULL, OPT_TRACE_AUDIO },
		{ "gfx-frame-every", required_argument, NULL, OPT_FRAME_EVERY },
		{ "no-idle", no_argument, NULL, 'I' },
		{ "save", required_argument, NULL, 'E' },
		{ "save-dir", required_argument, NULL, 'W' },
		{ "sin-pak", no_argument, NULL, OPT_NO_PAK },
		{ "pads", required_argument, NULL, OPT_PADS },
		{ "pad-port", required_argument, NULL, OPT_PAD_PORT },
		{ "gfx-frame", required_argument, NULL, 'G' },
		{ "gfxtest-check", no_argument, NULL, 'K' },
		{ "trace-rsp", required_argument, NULL, 'P' },
		{ "trace-vi", required_argument, NULL, 'V' },
		{ "lle", optional_argument, NULL, OPT_LLE },
		{ "lle-speed", required_argument, NULL, OPT_LLE_SPEED },
		{ "lle-check-audio", no_argument, NULL, OPT_LLE_CHECK },
		{ "lle-check-jpeg", no_argument, NULL, OPT_LLE_CHECK_JPEG },
		{ "lle-check-gfx", no_argument, NULL, OPT_LLE_CHECK_GFX },
		{ "state-save", required_argument, NULL, OPT_STATE_SAVE },
		{ "state-load", required_argument, NULL, OPT_STATE_LOAD },
		{ "state-zip", no_argument, NULL, OPT_STATE_ZIP },
		{ "fields", required_argument, NULL, OPT_FIELDS },
		{ "reset-at", required_argument, NULL, OPT_RESET_AT },
		{ "nmi-at", required_argument, NULL, OPT_NMI_AT },
		{ "stop-gfx", required_argument, NULL, OPT_STOP_GFX },
		{ "interp", required_argument, NULL, OPT_INTERP },
		{ "interp-k", required_argument, NULL, OPT_INTERP_K },
		{ "interp-decimate", no_argument, NULL, OPT_INTERP_DECIMATE },
		{ "interp-disp", required_argument, NULL, OPT_INTERP_DISP },
		{ "profile", required_argument, NULL, OPT_PROFILE },
		{ "perfilado", required_argument, NULL, OPT_PERFILADO },
		{ "rom-paged", required_argument, NULL, OPT_ROM_PAGED },
		{ "trace-gfx", required_argument, NULL, 'Q' },
		{ "trace-fb", no_argument, NULL, OPT_TRACE_FB },
		{ "raster-dump", required_argument, NULL, 'D' },
		{ "audio-wav", required_argument, NULL, 'A' },
		{ "audiotest-check", no_argument, NULL, 'Z' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	uint64_t max_cycles = DEFAULT_CYCLES, slice = DEFAULT_SLICE;
	bool cycles_set = false;
	uint32_t ram = SALSA64_RDRAM_MAX;
	int tv = SALSA64_BOOT_TV_AUTO, save_type = SALSA64_SAVE_AUTO, c;
	bool no_pak = false;
	unsigned n_pads = 1;
	bool regs = false, idle = true, use_rec = false, lockstep = false;
	const char *frame_path = NULL, *gfx_path = NULL, *save_dir = NULL, *profile_path = NULL;
	bool gfx_check = false, audio_check = false;
	bool rom_paged = false;                   /* --rom-paged PAGINA_KB,CACHE_KB: la ROM se lee por fragmentos */
	unsigned rp_page_kb = 256, rp_cache_kb = 32768;
	const char *wav_path = NULL;
	const char *dump_path = NULL, *ram_path = NULL;
	uint32_t frame_every = 0;
	struct interp_opts iopt = { .k = 3, .max_disp = 120.0f, .block = 8, .range = 8 };

	if (argc > 1 && !strcmp(argv[1], "--ares"))
		return ares_main(argc - 1, argv + 1);
	while ((c = getopt_long(argc, argv, "c:u:f:qrh", opts, NULL)) != -1) {
		switch (c) {
		case 'c': max_cycles = strtoull(optarg, NULL, 0); cycles_set = true; break;
		case 'u': watch.until = optarg; break;
		case 'f':
			if (watch.nfail == MAX_FAIL_TEXTS) {
				usage(stderr);
				return 3;
			}
			watch.fail[watch.nfail++] = optarg;
			break;
		case 'S': slice = strtoull(optarg, NULL, 0); break;
		case 'q': watch.quiet = true; break;
		case 'r': regs = true; break;
		case 'X': trace_exc_left = strtoull(optarg, NULL, 0); break;
		case 'F': frame_path = optarg; break;
		case OPT_REC: use_rec = true; break;
		case OPT_LOCKSTEP:
			use_rec = lockstep = true;
			lock.ram_every = optarg ? strtoull(optarg, NULL, 0) : 4096;
			if (!lock.ram_every)
				lock.ram_every = 1;
			break;
		case 'I': idle = false; break;
		case OPT_PAD:
			if (!pad_parse(optarg)) {
				fprintf(stderr, "salsa64-run: --pad: no se entiende \"%s\"\n", optarg);
				return 3;
			}
			break;
		case OPT_POKE:
			if (!poke_parse(optarg)) {
				fprintf(stderr, "salsa64-run: --poke: no se entiende \"%s\"\n", optarg);
				return 3;
			}
			break;
		case OPT_TRACE_FROM: trace_from = (uint32_t)strtoul(optarg, NULL, 0); break;
		case OPT_TRACE_FAULTS: trace_exc_left = strtoull(optarg, NULL, 0); trace_exc_faults_only = true; break;
		case OPT_RAM_DUMP: ram_path = optarg; break;
		case OPT_TRACE_AUDIO: trace_audio_left = (unsigned)strtoul(optarg, NULL, 0); break;
		case OPT_FRAME_EVERY: frame_every = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'W': save_dir = optarg; break;
		case OPT_NO_PAK: no_pak = true; break;
		case OPT_PADS: n_pads = (unsigned)strtoul(optarg, NULL, 0); break;
		case OPT_PAD_PORT:
			pad_port = (unsigned)strtoul(optarg, NULL, 0);
			if (pad_port < 1 || pad_port > SALSA64_PIF_PORTS) {
				fprintf(stderr, "salsa64-run: --pad-port va de 1 a %d\n", SALSA64_PIF_PORTS);
				return 2;
			}
			pad_port--;
			break;
		case 'E':
			for (int t = 0; t < SALSA64_SAVE_NTYPES; t++)
				if (!strcmp(optarg, salsa64_save_name(t)))
					save_type = t;
			if (save_type == SALSA64_SAVE_AUTO) {
				usage(stderr);
				return 3;
			}
			break;
		case 'G': gfx_path = optarg; gfx.raster = true; break;
		case 'K': gfx_check = true; gfx.raster = true; break;
		case 'P': trace_rsp_left = (unsigned)strtoul(optarg, NULL, 0); break;
		case 'V': trace_vi_left = (unsigned)strtoul(optarg, NULL, 0); break;
		case OPT_LLE:
			if (!optarg || !strcmp(optarg, "auto"))
				lle_mode = SALSA64_LLE_AUTO;
			else if (!strcmp(optarg, "all"))
				lle_mode = SALSA64_LLE_ALL;
			else {
				fprintf(stderr, "salsa64-run: --lle=%s no se entiende (auto, all)\n", optarg);
				return 3;
			}
			break;
		case OPT_LLE_SPEED: lle_speed = atof(optarg); break;
		case OPT_LLE_CHECK: shadow.on = true; break;
		case OPT_LLE_CHECK_JPEG: jcheck.on = true; break;
		case OPT_LLE_CHECK_GFX:
			gchk_on = true;
			gchk_self = getenv("SALSA64_GCHK_SELF") != NULL;
			if (!gchk_self)
				lle_mode = SALSA64_LLE_ALL;
			gfx.raster = true;
			break;
		case OPT_STATE_SAVE: {
			char *colon = strchr(optarg, ':');
			if (!colon || colon == optarg || !colon[1] || state_saves.n == MAX_STATE_SAVES) {
				fprintf(stderr, "salsa64-run: --state-save CAMPO:ARCHIVO (hasta %d)\n", MAX_STATE_SAVES);
				return 3;
			}
			state_saves.field[state_saves.n] = (uint32_t)strtoul(optarg, NULL, 0);
			state_saves.path[state_saves.n++] = colon + 1;
			break;
		}
		case OPT_STATE_LOAD: state_load_path = optarg; break;
		case OPT_STATE_ZIP: state_zip = true; break;
		case OPT_FIELDS: stop_field = (uint32_t)strtoul(optarg, NULL, 0); break;
		case OPT_RESET_AT: reset_at = (uint32_t)strtoul(optarg, NULL, 0); break;
		case OPT_NMI_AT: nmi_at_opt = (uint32_t)strtoul(optarg, NULL, 0); break;
		case OPT_STOP_GFX: {
			const char *colon = strchr(optarg, ':');
			if (!colon) {
				usage(stderr);
				return 3;
			}
			stop_gfx_n = (uint32_t)strtoul(optarg, NULL, 0);
			stop_gfx_path = colon + 1;
			gfx.raster = true;
			break;
		}
		case OPT_INTERP: iopt.dir = optarg; gfx.raster = true; break;
		case OPT_INTERP_K: iopt.k = (unsigned)strtoul(optarg, NULL, 0); break;
		case OPT_INTERP_DECIMATE: iopt.decimate = true; break;
		case OPT_INTERP_DISP: iopt.max_disp = strtof(optarg, NULL); break;
		case OPT_PROFILE: profile_path = optarg; break;
		case OPT_PERFILADO: hp.root = optarg; break;
		case OPT_ROM_PAGED:
			if (sscanf(optarg, "%u,%u", &rp_page_kb, &rp_cache_kb) != 2 || rp_page_kb < 4 ||
			    (rp_page_kb & (rp_page_kb - 1)) || rp_cache_kb < 2 * rp_page_kb) {
				usage(stderr);
				return 3;
			}
			rom_paged = true;
			break;
		case 'D': dump_path = optarg; break;
		case 'A': wav_path = optarg; break;
		case 'Z': audio_check = true; break;
		case 'Q': trace_gfx_left = (unsigned)strtoul(optarg, NULL, 0); trace_gfx_verbose = getenv("SALSA64_GFX_VERBOSE") != NULL; break;
		case OPT_TRACE_FB: trace_fb = true; break;
		case 'T':
			if (!strcmp(optarg, "ntsc")) tv = SALSA64_TV_NTSC;
			else if (!strcmp(optarg, "pal")) tv = SALSA64_TV_PAL;
			else if (!strcmp(optarg, "mpal")) tv = SALSA64_TV_MPAL;
			else { usage(stderr); return 3; }
			break;
		case 'R':
			if (!strcmp(optarg, "4")) ram = 4u << 20;
			else if (!strcmp(optarg, "8")) ram = 8u << 20;
			else { usage(stderr); return 3; }
			break;
		case 'h': usage(stdout); return 0;
		default:  usage(stderr); return 3;
		}
	}
	/* Como /dev_hdd0/salsa64/rsx-sin-blindaje en la PS3: las color images
	 * fuera de la RDRAM vuelven a tener destino (para comparar). */
	if (getenv("SALSA64_RSX_SIN_BLINDAJE") && atoi(getenv("SALSA64_RSX_SIN_BLINDAJE")))
		raster_rsx_safe = false;
	if (dump_path)
		return raster_dump(dump_path, gfx_path);
	if (stop_field && !cycles_set)
		max_cycles = UINT64_MAX;  /* --fields manda */
	size_t longest = watch.until ? strlen(watch.until) : 0;
	for (unsigned k = 0; k < watch.nfail; k++)
		if (strlen(watch.fail[k]) > longest)
			longest = strlen(watch.fail[k]);
	if (optind != argc - 1 || slice == 0 || longest > MAX_TEXT) {
		usage(stderr);
		return 3;
	}
	watch.keep = longest ? longest - 1 : 0;

	const char *path = argv[optind];
	size_t rom_size = 0;
	static struct salsa64_rompg rom_pg;
	uint8_t *rom = rom_paged ? read_rom_paged(path, &rom_size, rp_page_kb, rp_cache_kb, &rom_pg)
				 : read_file(path, &rom_size);
	if (!rom) {
		fprintf(stderr, "salsa64-run: no se pudo leer %s\n", path);
		return 3;
	}
	static const char *const fmt_names[] = { ".z64", ".v64 (convertida)", ".n64 (convertida)" };
	enum salsa64_rom_format fmt = rom_paged ? (enum salsa64_rom_format)rom_pg.fmt : salsa64_rom_normalize(rom, rom_size);

	void *fm_window = NULL;
	if (use_rec && salsa64_host_env_on("SALSA64_REC_FASTMEM")) {
		fm_window = salsa64_host_fastmem_window(ram);
		if (!fm_window) {
			fprintf(stderr, "salsa64-run: no se pudo reservar la ventana de fastmem\n");
			return 3;
		}
		rdram = fm_window;
	}
	salsa64_n64_init(&n64, rdram, ram);
	n64.bus.vi.bob_full_frame = salsa64_host_env_on("SALSA64_VI_CAMPOS");
	bool si_immediate = salsa64_host_env_on("SALSA64_SI_INMEDIATO");
	salsa64_si_set_immediate(&n64.bus, si_immediate);
	if (si_immediate)
		fprintf(stderr, "salsa64-run: SI inmediato (SALSA64_SI_INMEDIATO)\n");
	bool si_fixed = salsa64_host_env_on("SALSA64_SI_RITMO_VIEJO");
	salsa64_si_set_fixed(&n64.bus, si_fixed);
	if (si_fixed)
		fprintf(stderr, "salsa64-run: SI con 2 ms fijos (SALSA64_SI_RITMO_VIEJO)\n");
	if (getenv("SALSA64_RITMO"))
		ritmo_every = (uint32_t)strtoul(getenv("SALSA64_RITMO"), NULL, 0);
	const char *affine = getenv("SALSA64_GFX_AFFINE_CHAIN");
	gfx_affine_chain = !affine || strcmp(affine, "0");
	gfx_no_fog_1cycle = salsa64_host_env_on("SALSA64_SIN_NIEBLA_1CICLO");
	/* Como sin-sombra-afin en la PS3: ni la caja del RSP en la HLE ni la
	 * sombra lineal en pantalla en el rasterizador. */
	gfx_no_rsp_clip = salsa64_host_env_on("SALSA64_SIN_SOMBRA_AFIN");
	raster_affine = !gfx_no_rsp_clip;
	if (gfx_no_rsp_clip)
		fprintf(stderr, "salsa64-run: recorte del RSP y sombra afín apagados (SALSA64_SIN_SOMBRA_AFIN)\n");
	gfx_s2dex_bg_row_old = salsa64_host_env_on("SALSA64_S2DEX_PASO_VIEJO");
	gfx_f5_sub_old = salsa64_host_env_on("SALSA64_F5_SUBLISTA_VIEJA");
	if (getenv("SALSA64_GFX_CYCLES"))
		n64.bus.rsp.gfx_cycles = (uint32_t)strtoul(getenv("SALSA64_GFX_CYCLES"), NULL, 0);
	/* Ritmo del RCP (rsp-ritmo en la PS3): porcentaje del modelo, 0 lo apaga. */
	if (getenv("SALSA64_RITMO_RCP")) {
		n64.bus.rsp.gfx_model = (uint32_t)strtoul(getenv("SALSA64_RITMO_RCP"), NULL, 0);
		fprintf(stderr, "salsa64-run: ritmo del RCP %s (SALSA64_RITMO_RCP=%u)\n",
			n64.bus.rsp.gfx_model ? "activo" : "apagado", n64.bus.rsp.gfx_model);
	}
	if (getenv("SALSA64_DP_CYCLES"))
		n64.bus.rsp.dp_cycles = (uint32_t)strtoul(getenv("SALSA64_DP_CYCLES"), NULL, 0);
	if (getenv("SALSA64_AUDIO_CYCLES"))
		n64.bus.rsp.audio_cycles = (uint32_t)strtoul(getenv("SALSA64_AUDIO_CYCLES"), NULL, 0);
	salsa64_cpu_set_idle(&n64.cpu, idle);
	/* SALSA64_SIN_BARRIDO_CACHE=1, como sin-barrido-cache en la PS3: los
	 * barridos de la caché de datos gratis, como antes (vr4300.h). */
	bool dcache_sweep = !salsa64_host_env_on("SALSA64_SIN_BARRIDO_CACHE");
	salsa64_cpu_set_dcache_sweep(&n64.cpu, dcache_sweep);
	if (!dcache_sweep)
		fprintf(stderr, "salsa64-run: barridos de la caché de datos gratis (SALSA64_SIN_BARRIDO_CACHE)\n");
	/* Efectos de framebuffer en el rasterizador, como fbfx.cfg en la PS3:
	 * SALSA64_FBFX=0 los apaga, 1 solo render a textura, 2 todo. */
	if (getenv("SALSA64_FBFX"))
		raster_fbfx = atoi(getenv("SALSA64_FBFX"));
	if (getenv("SALSA64_AA_EDGES"))
		raster_aa_edges = atoi(getenv("SALSA64_AA_EDGES")) != 0;
	raster_target_alpha = !salsa64_host_env_on("SALSA64_SIN_ALFA_DESTINOS");
	raster_copy_alpha = !salsa64_host_env_on("SALSA64_COPIA_SIN_ALFA");
	raster_cvg_alpha = !salsa64_host_env_on("SALSA64_ALFA_FRAGMENTO");
	if (getenv("SALSA64_AA_EDGE_OFFSET"))
		sscanf(getenv("SALSA64_AA_EDGE_OFFSET"), "%f,%f", &raster_edge_factor, &raster_edge_units);
	const struct salsa64_gfx_sink sink = { NULL, gfx_acquire, gfx_done };
	n64.bus.rdram_written = rdram_written;
	salsa64_gfx_sync_init(&gfx_sync, &gfx_state, &n64.bus, &sink);
	if (gchk_on)
		gchk_init(&n64.bus, &gfx_state, &n64.bus.vi.fields, &gfx.tasks);
	iopt.from_field = trace_from;
	interp_init(&iopt);
	const char *turbo3d = getenv("SALSA64_GFX_TURBO3D");
	gfx_sync.backend.turbo3d = !turbo3d || strcmp(turbo3d, "0");
	const char *acclaim = getenv("SALSA64_GFX_ACCLAIM");
	gfx_sync.backend.acclaim = !acclaim || strcmp(acclaim, "0");
	fprintf(stderr, "salsa64-run: HLE Acclaim %s (SALSA64_GFX_ACCLAIM=0 la apaga)\n",
		gfx_sync.backend.acclaim ? "activa" : "apagada");
	const char *eurocom = getenv("SALSA64_GFX_EUROCOM");
	gfx_sync.backend.eurocom = !eurocom || strcmp(eurocom, "0");
	fprintf(stderr, "salsa64-run: HLE Eurocom %s (SALSA64_GFX_EUROCOM=0 la apaga)\n",
		gfx_sync.backend.eurocom ? "activa" : "apagada");
	salsa64_rsp_set_gfx(&n64.bus, &gfx_sync.backend);
	if (getenv("SALSA64_GFX_LATE")) {
		gfx_late.polls = (unsigned)strtoul(getenv("SALSA64_GFX_LATE"), NULL, 0);
		gfx_late.tear = getenv("SALSA64_GFX_TEAR") != NULL;
		gfx_late.backend = (struct salsa64_gfx_backend){ .start = late_start, .finish = late_finish, .done = late_done,
								      .cycles = late_cycles };
		/* Número de plazos que se deja correr al CPU antes de esperar el trabajo.
		 * 1 reproduce la espera estricta; 157, un campo NTSC. */
		if (getenv("SALSA64_GFX_BLOCK"))
			gfx_late.block_after = (unsigned)strtoul(getenv("SALSA64_GFX_BLOCK"), NULL, 0);
		gfx_late.backend.turbo3d = gfx_sync.backend.turbo3d;
		gfx_late.backend.acclaim = gfx_sync.backend.acclaim;
		gfx_late.backend.eurocom = gfx_sync.backend.eurocom;
		salsa64_rsp_set_gfx(&n64.bus, &gfx_late.backend);
	}
	if (getenv("SALSA64_JITTER")) {
		jitter_state = (uint32_t)strtoul(getenv("SALSA64_JITTER"), NULL, 0) * 2654435761u + 1u;
		if (!getenv("SALSA64_GFX_LATE")) {
			gfx_late.polls = 0;
			gfx_late.backend = (struct salsa64_gfx_backend){ .start = late_start, .finish = late_finish, .done = late_done,
								      .cycles = late_cycles };
			gfx_late.backend.turbo3d = gfx_sync.backend.turbo3d;
			gfx_late.backend.acclaim = gfx_sync.backend.acclaim;
			gfx_late.backend.eurocom = gfx_sync.backend.eurocom;
			salsa64_rsp_set_gfx(&n64.bus, &gfx_late.backend);
		}
	}
	salsa64_audio_sync_init(&aud_sync, &aud_state, &n64.bus);
	aud_sync.job_flags = salsa64_host_env_on("SALSA64_AUDIO_RANGES_OLD") ? AUD_JOB_RANGES_OLD : 0;
	/* SALSA64_AUDIO_POLEF_ZERO=1: el POLEF de ABI1 con A_INIT parte de cero (lo anterior) */
	aud_sync.job_flags |= salsa64_host_env_on("SALSA64_AUDIO_POLEF_ZERO") ? AUD_JOB_POLEF_INIT_ZERO : 0;
	salsa64_rsp_set_audio(&n64.bus, &aud_sync.backend);
	salsa64_ai_set_sink(&n64.bus, audio_push, NULL);
	if (jcheck.on && lle_mode == SALSA64_LLE_OFF)
		lle_mode = SALSA64_LLE_AUTO;
	if (lle_mode != SALSA64_LLE_OFF) {
		salsa64_lle_host_init(&lle_host, &n64.bus);
		if (lle_speed > 0.0)
			salsa64_lle_host_speed(&lle_host, lle_speed);
		jcheck_lle = lle_host.backend;
		if (jcheck.on || shadow.on || gchk_on)
			jcheck_lle.sync = jcheck_sync;
		if (shadow.on)
			jcheck_lle.start = shadow_lle_start;
		if (gchk_on && !gchk_self)
			jcheck_lle.start = gchk_lle_start;
		salsa64_rsp_set_lle(&n64.bus, &jcheck_lle, lle_mode);
	}
	/* SALSA64_SIN_DPC=1: sin quien consuma las listas del RDP que escribe el
	 * CPU (la PS3 antes de conectarlo siempre): Killer Instinct Gold no manda
	 * ninguna tarea gráfica porque espera la interrupción del DP de su
	 * SYNC_FULL. */
	if (getenv("SALSA64_DPC_ESTADO_APARTE")) {
		salsa64_gfx_sync_init(&gfx_sync_dpc, &gfx_state_dpc, &n64.bus, &sink);
		salsa64_rsp_set_dpc(&n64.bus, &gfx_sync_dpc.dpc);
	} else if (!getenv("SALSA64_SIN_DPC")) {
		salsa64_rsp_set_dpc(&n64.bus, &gfx_sync.dpc);
	}
	n64.bus.rsp.jpeg_lle = jcheck.on;
	n64.bus.rsp.clock_us = host_clock_us;
	n64.bus.rsp.trace = trace_rsp;
	if (trace_exc_left)
		n64.cpu.exc_hook = trace_exc;
	salsa64_faultlog_install(&n64.cpu, save_log);
	salsa64_bus_set_isviewer(&n64.bus, isv_buf, isv_out, NULL);
	if (rom_paged)
		salsa64_bus_set_rom_paged(&n64.bus, rom, 4096, (uint32_t)rom_size, &rom_pg);
	else
		salsa64_bus_set_rom(&n64.bus, rom, (uint32_t)(rom_size > UINT32_MAX ? UINT32_MAX : rom_size));
	struct salsa64_boot_info info;
	if (!salsa64_boot_hle(&n64, tv, &info)) {
		fprintf(stderr, "salsa64-run: no se pudo arrancar %s: %s\n", path, info.error);
		return 3;
	}
	static const char *const tv_names[] = { "PAL", "NTSC", "MPAL" };
	if (info.kind == SALSA64_BOOT_ELF)
		fprintf(stderr, "salsa64-run: %s %s, %zu bytes; ELF en 0x%X (IPL3 libre), %u bytes cargados, "
			"entrada 0x%08X, TV %s, RDRAM %u MB\n", path, fmt_names[fmt], rom_size, info.elf_offset,
			info.loaded, info.entry, tv_names[info.tv], ram >> 20);
	else
		fprintf(stderr, "salsa64-run: %s %s, %zu bytes; carga plana (CIC %s), %u bytes, "
			"entrada 0x%08X, TV %s, RDRAM %u MB\n", path, fmt_names[fmt], rom_size,
			salsa64_cic_name(info.cic), info.loaded, info.entry, tv_names[info.tv], ram >> 20);
	savefile_open(&save_file, &save, &n64.bus, rom, save_dir, NULL, save_type, save_log);
	mempakfile_open(&mempak_file, &mempak, &n64.bus, rom, save_dir, no_pak ? 0u : 0xFu, save_log);
	for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++)
		n64.bus.pif.pads[i].connected = i < n_pads;

	if (use_rec) {
		void *code = salsa64_host_rec_buffer();
		rec.opts = salsa64_host_rec_opts();
		if (salsa64_host_env_on("SALSA64_REC_DIAG"))
			rec.opts |= SALSA64_REC_OPT_DIAG;
		fprintf(stderr, "salsa64-run: descartes de espera: %s\n", rec.opts & SALSA64_REC_OPT_IDLE_RECOMPILE ? "recompilar" : "parche PPC");
		if (getenv("SALSA64_REC_DUMP"))
			rec.on_compile = dump_block;
		if (getenv("SALSA64_REC_PROF")) {
			rec.prof = &prof;
			prof.blocks = calloc(1, sizeof *prof.blocks);
			prof_every = (uint32_t)strtoul(getenv("SALSA64_REC_PROF"), NULL, 0);
			prof_next = prof_every;
		}
		rec.hot_bytes = salsa64_host_rec_hot_bytes();
		rec.fm_window = fm_window;
		hp_prepare();
		rec.trace = hp.trace;
		if (!code || !salsa64_rec_init(&rec, &n64.cpu, code, salsa64_host_rec_bytes())) {
			fprintf(stderr, "salsa64-run: el recompilador no corre en este host\n");
			return 3;
		}
		if (fm_window && !rec.fastmem) {
			fprintf(stderr, "salsa64-run: fastmem pedido pero no activo\n");
			return 3;
		}
		if (!fastmem_install_handler() && rec.fastmem) {
			fprintf(stderr, "salsa64-run: fastmem sin manejador de SIGSEGV en este host\n");
			return 3;
		}
		if (salsa64_host_env_on("SALSA64_REC_SAMPLE")) {
			salsa64_rec_sample_init(&host_sample);
			rec.sample = &host_sample;
			if (pthread_create(&sample_tid, NULL, sample_thread, NULL) != 0) {
				fprintf(stderr, "salsa64-run: no se pudo crear el hilo del muestreo\n");
				rec.sample = NULL;
			}
		}
		if (profile_path) {
			struct salsa64_rec_profile_game game;
			salsa64_rec_profile_game_of(&game, rom);
			bool loaded = salsa64_rec_profile_load(&profile, profile_path, &game);
			salsa64_rec_profile_mark_hot(&profile, salsa64_rec_hot_capacity(&rec) * 3 / 4);
			rec.profile = &profile;
			rec.profile_sites = !getenv("SALSA64_REC_SITES") || salsa64_host_env_on("SALSA64_REC_SITES");
			fprintf(stderr, "salsa64-run: perfil %s: %s, %u entradas y %u sitios, %u calientes (%u KB, umbral %u); "
				"zona caliente %u KB en dos mitades, sitios de salida %s\n", profile_path,
				loaded ? "cargado" : "vacío (sin archivo, corrupto o de otro juego)", profile.count, profile.nsites,
				profile.hot_entries, profile.hot_words / 256, profile.hot_threshold,
				salsa64_rec_hot_capacity(&rec) / 256, rec.profile_sites ? "sí" : "no");
		}
		if (lockstep) {
			/* La referencia arranca igual; cada bloque vuelve a C para comparar. */
			salsa64_n64_init(&ref, ref_rdram, ram);
			salsa64_si_set_immediate(&ref.bus, si_immediate);
			salsa64_si_set_fixed(&ref.bus, si_fixed);
			salsa64_cpu_set_idle(&ref.cpu, idle);
			salsa64_cpu_set_dcache_sweep(&ref.cpu, dcache_sweep);
			salsa64_bus_set_isviewer(&ref.bus, ref_isv_buf, ref_isv_out, NULL);
			if (rom_paged)
				salsa64_bus_set_rom_paged(&ref.bus, rom, 4096, (uint32_t)rom_size, &rom_pg);
			else
				salsa64_bus_set_rom(&ref.bus, rom, (uint32_t)(rom_size > UINT32_MAX ? UINT32_MAX : rom_size));
			ref.bus.rsp.gfx_cycles = n64.bus.rsp.gfx_cycles;
			ref.bus.rsp.gfx_model = n64.bus.rsp.gfx_model;
			ref.bus.rsp.dp_cycles = n64.bus.rsp.dp_cycles;
			ref.bus.rsp.audio_cycles = n64.bus.rsp.audio_cycles;
			const struct salsa64_gfx_sink ref_sink = { NULL, ref_gfx_acquire, ref_gfx_done };
			salsa64_gfx_sync_init(&ref_gfx_sync, &ref_gfx_state, &ref.bus, &ref_sink);
			salsa64_rsp_set_gfx(&ref.bus, &ref_gfx_sync.backend);
			salsa64_audio_sync_init(&ref_aud_sync, &ref_aud_state, &ref.bus);
			ref_aud_sync.job_flags = aud_sync.job_flags;
			salsa64_rsp_set_audio(&ref.bus, &ref_aud_sync.backend);
			struct salsa64_boot_info ref_info;
			if (!salsa64_boot_hle(&ref, tv, &ref_info)) {
				fprintf(stderr, "salsa64-run: la referencia no arrancó\n");
				return 3;
			}
			/* El mismo guardado, en una copia: sin él la PIF de la referencia
			 * responde «sin dispositivo» donde la principal ve la EEPROM. */
			if (n64.bus.save) {
				ref_save = save;
				salsa64_save_attach(&ref.bus, &ref_save);
			}
			/* Y el mismo Controller Pak: sin él el estado del control dice
			 * «sin pak» (0x02) donde la principal ve uno (0x01). */
			if (n64.bus.mempak) {
				ref_mempak = *n64.bus.mempak;
				ref.bus.mempak = &ref_mempak;
			}
			rec.no_link = true;
			rec.after_exit = lockstep_check;
		}
		fprintf(stderr, "salsa64-run: recompilador activo%s%s%s; caché en %p, llamadas a C %s\n",
			lockstep ? " con --lockstep" : "", rec.opts & SALSA64_REC_OPT_PIN ? ", registros fijos" : "",
			rec.fastmem ? ", fastmem" : "",
			code, rec.calls_near ? "con bl" : "por mtctr/bctrl");
	}

	if (state_load_path) {
		if (!state_load_file(&n64, state_load_path) || (lockstep && !state_load_file(&ref, state_load_path))) {
			free(rom);
			return 3;
		}
		/* Lo programado hasta el campo del estado ya está en él. */
		while (pokes.next < pokes.n && n64.bus.vi.fields >= pokes.at[pokes.next].field)
			pokes.next++;
	}

	/* Un control programado en el campo 0 está conectado desde el arranque
	 * (como el puerto 1 en la PS3): hay juegos que solo lo buscan al
	 * iniciar y sin él no pasan del logo (Banjo-Kazooie). Tras --state-load,
	 * el último hasta el campo del estado (el PIF ya lo tiene: no cambia
	 * nada). */
	pad_field();
	if (!hp_open(rom, path)) {
		free(rom);
		return 3;
	}

	/* Bucle principal: bloques de `slice` ciclos y verificación entre ellos. */
	enum stop_reason reason = STOP_NONE;
	uint64_t start_cycles = n64.cpu.cycles, start_idle = n64.cpu.idle_cycles, t0 = now_us();
	uint64_t tb_start = salsa64_rec_trace_now();
	while (reason == STOP_NONE) {
		uint64_t done = n64.cpu.cycles - start_cycles;
		if (done >= max_cycles) {
			reason = STOP_CYCLES;
			break;
		}
		bool trace_vi = trace_vi_left && max_cycles - done < (uint64_t)trace_vi_left * 1600000u;
		if (trace_vi) {
			/* Últimos campos: de a uno, con la imagen que describe el VI (los
			 * controles programados se aplican en field_done). */
			salsa64_n64_run_field(&n64);
			field_done(gfx_path, frame_every);
			struct salsa64_vi_frame f;
			if (trace_vi && salsa64_vi_frame(&n64.bus, &f))
				fprintf(stderr, "campo %u: origen %06x %ux%u stride %u bpp %u vpos %u (V_VIDEO %08x Y_SCALE %08x)\n",
					n64.bus.vi.fields, f.origin, f.width, f.height, f.stride, f.bpp, f.vpos,
					n64.bus.vi.regs[SALSA64_VI_V_VIDEO], n64.bus.vi.regs[SALSA64_VI_Y_SCALE]);
			else if (trace_vi)
				fprintf(stderr, "campo %u: sin imagen (CONTROL %08x ORIGIN %06x WIDTH %u H_VIDEO %08x V_VIDEO %08x)\n",
					n64.bus.vi.fields, n64.bus.vi.regs[SALSA64_VI_CONTROL],
					n64.bus.vi.regs[SALSA64_VI_ORIGIN] & 0xFFFFFF, n64.bus.vi.regs[SALSA64_VI_WIDTH],
					n64.bus.vi.regs[SALSA64_VI_H_VIDEO], n64.bus.vi.regs[SALSA64_VI_V_VIDEO]);
		} else if (pads.n || pokes.n || gfx.raster || state_saves.n || stop_field || hp.on || reset_at || nmi_at_opt ||
			   ritmo_every) {
			/* Con controles programados, campo por campo: el control cambia
			 * justo al empezar uno. Con el rasterizador también, para darle
			 * el origen del VI de cada campo (raster_vi). Los estados se
			 * guardan en el límite del campo, como en la PS3. */
			salsa64_n64_run_field(&n64);
			field_done(gfx_path, frame_every);
			if (!state_fields_done() || !reset_fields_done()) {
				free(rom);
				return 3;
			}
			if (stop_field && n64.bus.vi.fields >= stop_field && watch.hit == STOP_NONE)
				watch.hit = STOP_FIELDS;
			if (stop_gfx_hit && watch.hit == STOP_NONE)
				watch.hit = STOP_FIELDS;
			hp_field();
		} else {
			salsa64_n64_run(&n64, max_cycles - done < slice ? max_cycles - done : slice);
		}
		pc_sample(n64.cpu.pc);
		if (rec.profile && n64.bus.vi.fields >= profile_fold_next) {
			salsa64_rec_profile_fold(&rec);
			profile_fold_next = n64.bus.vi.fields + 60;
		}
		if (prof_every && n64.bus.vi.fields >= prof_next) {
			char text[2048];
			salsa64_rec_prof_report(&prof, &prof_prev, 0, prof_every, text, sizeof text);
			fprintf(stderr, "campos %u–%u, %s\n", prof_next - prof_every, n64.bus.vi.fields, text);
			prof_next = n64.bus.vi.fields + prof_every;
		}
		if (watch.hit != STOP_NONE)
			reason = watch.hit;
		else if (cpu_stuck())
			reason = STOP_STUCK;
	}
	uint64_t us = now_us() - t0, cycles = n64.cpu.cycles - start_cycles;
	uint64_t idle_cycles = n64.cpu.idle_cycles - start_idle;
	if (watch.midline)
		putchar('\n');
	fflush(stdout);

	static const char *const reason_names[] = {
		[STOP_UNTIL] = "apareció el texto esperado",
		[STOP_FAIL] = "apareció el texto de falla",
		[STOP_CYCLES] = "se agotaron los ciclos",
		[STOP_STUCK] = "bucle infinito sin interrupciones posibles",
		[STOP_FIELDS] = "se llegó al campo pedido",
	};
	hp_close(reason_names[reason]);
	uint64_t samples = pc_samples[SEG_KSEG0] + pc_samples[SEG_KSEG1] + pc_samples[SEG_TLB];
	if (samples)
		fprintf(stderr, "salsa64-run: pc al final de cada bloque (%llu muestras): KSEG0 %.1f %%, KSEG1 %.1f %%, "
			"TLB %.1f %%\n", (unsigned long long)samples, 100.0 * pc_samples[SEG_KSEG0] / samples,
			100.0 * pc_samples[SEG_KSEG1] / samples, 100.0 * pc_samples[SEG_TLB] / samples);
	/* Una instrucción por ciclo, salvo los ciclos saltados en bucles de espera. */
	fprintf(stderr, "salsa64-run: %s; %llu ciclos (%.2f s de consola) en %.2f s, %.1f MIPS, "
		"%.1f %% en bucles de espera, pc=0x%016llx\n",
		reason_names[reason], (unsigned long long)cycles, cycles / 93.75e6, us / 1e6,
		us ? (cycles - idle_cycles) / (double)us : 0.0,
		cycles ? 100.0 * idle_cycles / cycles : 0.0, (unsigned long long)n64.cpu.pc);
	if (n64.cpu.dcache_sweep_cycles)
		fprintf(stderr, "salsa64-run: barridos de la caché de datos: %u (%u ciclos cada uno)\n", n64.cpu.dcache_sweeps,
			n64.cpu.dcache_sweep_cycles);
	if (salsa64_gamefix_name(&n64.bus))
		fprintf(stderr, "salsa64-run: arreglo de juego: %s; aplicado %u veces\n", salsa64_gamefix_name(&n64.bus),
			n64.bus.gamefix_applied);
	if (use_rec) {
		salsa64_rec_diag_report(&rec, us ? (salsa64_rec_trace_now() - tb_start) * 1000000 / us : 0, true, save_log);
		const struct salsa64_rec_stats *st = &rec.stats;
		fprintf(stderr, "salsa64-run: recompilador: %llu bloques (%llu fuera de KSEG0, %llu desprendidos, %llu "
			"enganchados en otra física y %llu desprendidos matados por ids, %u KB de caché), %llu solo intérprete, %llu entradas, %llu enlaces, "
			"%llu invalidados, %llu vaciados (ids %llu, enlaces %llu, RDRAM %llu, bucles %llu, ROM %llu, estado %llu, "
			"otros %llu), %llu desalojos, %llu traducciones, %llu dependen de FR (%llu gemelos, %llu salidas por FR, %llu por CU1, %llu cortados por CU1), "
			"%llu instr. interpretadas (%.1f %%), %llu llamadas al intérprete%s\n",
			(unsigned long long)st->compiled, (unsigned long long)st->compiled_x, (unsigned long long)st->detached,
			(unsigned long long)st->rekeyed, (unsigned long long)st->reclaimed, salsa64_rec_used(&rec) / 256,
			(unsigned long long)st->interp_only, (unsigned long long)st->entries,
			(unsigned long long)st->linked, (unsigned long long)st->invalidated,
			(unsigned long long)st->flushes, (unsigned long long)st->flushes_by[SALSA64_REC_FLUSH_IDS],
			(unsigned long long)st->flushes_by[SALSA64_REC_FLUSH_LINKS], (unsigned long long)st->flushes_by[SALSA64_REC_FLUSH_RDRAM],
			(unsigned long long)st->flushes_by[SALSA64_REC_FLUSH_IDLE], (unsigned long long)st->flushes_by[SALSA64_REC_FLUSH_ROM],
			(unsigned long long)st->flushes_by[SALSA64_REC_FLUSH_STATE], (unsigned long long)st->flushes_by[SALSA64_REC_FLUSH_OTHER],
			(unsigned long long)st->evictions, (unsigned long long)st->vt_probes,
			(unsigned long long)st->fr_dep, (unsigned long long)st->fr_twins, (unsigned long long)st->fr_exits,
			(unsigned long long)st->cu1_exits, (unsigned long long)st->cu1_cuts,
			(unsigned long long)st->interp_steps, cycles ? 100.0 * (double)st->interp_steps / (double)cycles : 0.0,
			(unsigned long long)st->exec_calls, lockstep ? "; --lockstep sin diferencias" : "");
		fprintf(stderr, "salsa64-run: copias de bytes de los bloques mapeados: %u KB repartidos de %u, %u desprendidos vivos\n",
			rec.x.copy_used >> 10, (rec.opts & (SALSA64_REC_OPT_LEGACY | SALSA64_REC_DBG_NO_COPY_FREE)
						 ? SALSA64_REC_X_COPY_OLD_BYTES : SALSA64_REC_X_COPY_BYTES) >> 10, rec.x.ndetached);
		if (rec.opts & SALSA64_REC_OPT_JTC_STATS)
			fprintf(stderr, "salsa64-run: caché de destinos de JR: %u aciertos, %u fallos (%.1f %%)\n", rec.jtc_ctr[0], rec.jtc_ctr[1],
				rec.jtc_ctr[0] + rec.jtc_ctr[1] ? 100.0 * rec.jtc_ctr[0] / ((double)rec.jtc_ctr[0] + rec.jtc_ctr[1]) : 0.0);
		if (st->fm_sites || st->fm_off)
			fprintf(stderr, "salsa64-run: fastmem: %llu accesos sin prueba, %llu parchados tras fallar, %s\n",
				(unsigned long long)st->fm_sites, (unsigned long long)st->fm_faults,
				rec.fastmem ? "activo al terminar" : "apagado");
		if (rec.prof) {
			char text[2048];
			memset(&prof_prev, 0, sizeof prof_prev);
			salsa64_rec_prof_report(&prof, &prof_prev, 0, n64.bus.vi.fields, text, sizeof text);
			fprintf(stderr, "salsa64-run: %s\n", text);
			salsa64_rec_prof_blocks_report(&rec, getenv("SALSA64_REC_PROF_TOP") ? (unsigned)strtoul(getenv("SALSA64_REC_PROF_TOP"), NULL, 0) : 40,
					       prof_line, NULL);  /* más bloques para pesar la expansión (tools/rec-expansion.py) */
		}
		if (rec.sample) {
			sample_stop = 1;
			pthread_join(sample_tid, NULL);
			fprintf(stderr, "salsa64-run: muestreo: %llu muestras\n", (unsigned long long)host_sample.ticks);
			salsa64_rec_sample_report(&rec, 30, prof_line, NULL);
			rec.sample = NULL;
		}
		if (rec.profile) {
			salsa64_rec_profile_fold(&rec);
			bool saved = salsa64_rec_profile_save(&profile, profile_path);
			fprintf(stderr, "salsa64-run: perfil: %u entradas (%u cargadas, %u sin lugar) y %u sitios de salida (%u "
				"sin lugar), %llu entradas a bloques contadas (sesión %s), %llu bloques en la zona caliente (%llu no "
				"cupieron, %llu desalojos de mitad, %u KB usados); %s %s\n", profile.count, profile.loaded, profile.full,
				profile.nsites, profile.sites_full, (unsigned long long)profile.session,
				profile.session >= SALSA64_RPF_LONG_SESSION ? "larga" : "corta",
				(unsigned long long)st->hot_placed, (unsigned long long)st->hot_full,
				(unsigned long long)st->hot_evictions, salsa64_rec_hot_used(&rec) / 256,
				saved ? "guardado en" : "NO se pudo guardar en", profile_path);
			salsa64_rec_profile_detach(&rec);
		}
	}
#ifdef SALSA64_MEM_CENSUS
	{
		const struct salsa64_mem_census *mc = &salsa64_mem_census;
		uint64_t tlb = 0;
		for (unsigned k = 0; k < 13; k++)
			tlb += mc->tlb_ps[k];
		uint64_t total = mc->kseg0 + mc->kseg1 + tlb + mc->erl + mc->x64 + mc->kseg23;
		fprintf(stderr, "salsa64-run: censo de accesos a datos: %llu; KSEG0 %.2f %%, KSEG1 %.2f %% (%llu), por la TLB %.3f %% "
			"(%llu; kseg2/3 %llu; fallas %llu, inválidas %llu, modificación %llu), ERL directo %llu, 64 bits %llu, "
			"no alineados %llu; doblados a 1 GB chocan %llu, a 2 GB %llu\n",
			(unsigned long long)total, total ? 100.0 * (double)mc->kseg0 / (double)total : 0.0,
			total ? 100.0 * (double)mc->kseg1 / (double)total : 0.0, (unsigned long long)mc->kseg1,
			total ? 100.0 * (double)tlb / (double)total : 0.0, (unsigned long long)tlb, (unsigned long long)mc->kseg23,
			(unsigned long long)mc->miss, (unsigned long long)mc->invalid, (unsigned long long)mc->modified,
			(unsigned long long)mc->erl, (unsigned long long)mc->x64, (unsigned long long)mc->misaligned,
			(unsigned long long)mc->collide_1g, (unsigned long long)mc->collide_2g);
		static const char *const ps_names[13] = { "4K", "8K", "16K", "32K", "64K", "128K", "256K", "512K", "1M", "2M", "4M", "8M", "16M" };
		fprintf(stderr, "salsa64-run: censo por tamaño de página (accesos / escrituras de la TLB):");
		for (unsigned k = 0; k < 13; k++)
			if (mc->tlb_ps[k] || mc->tlbw_ps[k])
				fprintf(stderr, " %s %llu/%llu", ps_names[k], (unsigned long long)mc->tlb_ps[k], (unsigned long long)mc->tlbw_ps[k]);
		fprintf(stderr, "\nsalsa64-run: censo: ranuras de 8 MB tocadas por la TLB:");
		for (unsigned s = 0; s < 8; s++)
			for (unsigned b = 0; b < 32; b++)
				if (mc->slots[s] & (1u << b))
					fprintf(stderr, " %08X", (s << 28) | (b << 23));
		fputc('\n', stderr);
	}
#endif
	savefile_close(&save_file);
	mempakfile_close(&mempak_file);
	for (unsigned k = 0; k < state_saves.n; k++)
		if (state_saves.field[k] > n64.bus.vi.fields) {
			fprintf(stderr, "salsa64-run: --state-save: no se llegó al campo %u (terminó en el %u)\n",
				state_saves.field[k], n64.bus.vi.fields);
			free(rom);
			return 3;
		}
	if (regs)
		dump_regs();
	if (ram_path) {
		FILE *f = fopen(ram_path, "wb");
		if (!f || fwrite(rdram, 1, n64.bus.rdram_size, f) != n64.bus.rdram_size || fclose(f) != 0) {
			fprintf(stderr, "salsa64-run: no se pudo escribir %s\n", ram_path);
			return 3;
		}
		fprintf(stderr, "salsa64-run: RDRAM (%u MB) en %s\n", n64.bus.rdram_size >> 20, ram_path);
	}
	if (gfx.tasks || n64.bus.rsp.stats.unknown_gbi)
		gfx_summary();
	if (n64.bus.rsp.stats.jpeg)
		fprintf(stderr, "salsa64-run: jpeg: %u tareas en la HLE, %u macrobloques, %u con parámetros malos, "
			"%.2f ms\n", n64.bus.rsp.stats.jpeg, n64.bus.rsp.stats.jpeg_mb, n64.bus.rsp.stats.jpeg_bad,
			n64.bus.rsp.stats.jpeg_us / 1e3);
	if (n64.bus.rsp.stats.video || n64.bus.rsp.stats.video_unsup) {
		static const char *const vn[3] = { "RESIZE", "DECODE", "FILL" };
		const struct salsa64_rsp_stats *vs = &n64.bus.rsp.stats;
		fprintf(stderr, "salsa64-run: video (Resident Evil 2): %u tareas en la HLE, %u filas, %u con parámetros malos, %u "
			"sin HLE (parámetros o constantes que no se midieron); %.2f ms en el hilo del CPU\n", vs->video, vs->video_rows,
			vs->video_bad, vs->video_unsup, vs->video_us / 1e3);
		for (int k = 0; k < 3; k++)
			if (vs->video_k[k])
				fprintf(stderr, "salsa64-run:   %s: %u tareas, %u filas (unidades), %.2f ms (%.3f ms por tarea)\n", vn[k],
					vs->video_k[k], vs->video_k_rows[k], vs->video_k_us[k] / 1e3,
					vs->video_k_us[k] / 1e3 / vs->video_k[k]);
	}
	if (n64.bus.rsp.stats.audio || audio.dmas)
		audio_summary();
	if (lle_mode != SALSA64_LLE_OFF)
		lle_summary();
	if (jcheck.on) {
		if (jcheck.pending)
			jcheck_compare();
		fprintf(stderr, "salsa64-run: lle-check: %u tareas JPEG comparadas con la HLE (%u macrobloques, %u "
			"píxeles), %u píxeles distintos en %u tareas; por diferencia máxima de canal:", jcheck.tasks,
			jcheck.macroblocks, jcheck.pixels, jcheck.diff, jcheck.bad);
		for (int d = 0; d < 32; d++)
			if (jcheck.hist[d])
				fprintf(stderr, " %d:%u", d, jcheck.hist[d]);
		fputc('\n', stderr);
		static const char *const vnames[3] = { "RESIZE", "DECODE", "FILL" };
		for (int k = 0; k < 3; k++) {
			if (!jcheck.video[k].tasks && !jcheck.video[k].skipped)
				continue;
			fprintf(stderr, "salsa64-run: lle-check: video %s: %u tareas comparadas con la HLE (%u unidades, %u bytes), "
				"%u bytes distintos en %u tareas, %u no comparables; por |lle − hle|: 1:%u 2:%u 3–4:%u 5–16:%u 17–64:%u más:%u\n",
				vnames[k], jcheck.video[k].tasks, jcheck.video[k].rows, jcheck.video[k].bytes, jcheck.video[k].diff,
				jcheck.video[k].bad, jcheck.video[k].skipped, jcheck.video[k].hist[0], jcheck.video[k].hist[1],
				jcheck.video[k].hist[2], jcheck.video[k].hist[3], jcheck.video[k].hist[4], jcheck.video[k].hist[5]);
		}
	}
	if (gchk_on)
		gchk_report(stderr);
	if (shadow.on) {
		if (shadow.pending)
			shadow_compare();
		fprintf(stderr, "salsa64-run: lle-check: %u tareas de audio comparadas con la HLE, %u bytes, %u tareas "
			"distintas\n", shadow.tasks, shadow.bytes, shadow.bad);
		fprintf(stderr, "salsa64-run: lle-check: muestras de 16 bits distintas por diferencia: ±1 %u, ±2 %u, ±3–4 %u, "
			"±5–16 %u, mayores %u\n", shadow.hist[0], shadow.hist[1], shadow.hist[2], shadow.hist[3], shadow.hist[4]);
		if (aud_dbg_mp3)
			fprintf(stderr, "salsa64-run: lle-check: %u comandos MP3 en %u tareas\n", aud_dbg_mp3, aud_dbg_mp3_tasks);
		for (unsigned a = 0; a < SALSA64_ABI_COUNT; a++)
			if (shadow.abi_tasks[a])
				fprintf(stderr, "salsa64-run: lle-check: ABI %s: %u tareas, %u distintas\n",
					salsa64_abi_name((enum salsa64_abi)a), shadow.abi_tasks[a], shadow.abi_bad[a]);
		if (shadow.bis_tasks) {
			fprintf(stderr, "salsa64-run: lle-check: bisección de %u tareas; primer comando distinto por ABI y opcode:\n",
				shadow.bis_tasks);
			for (unsigned a = 0; a < SALSA64_ABI_COUNT; a++)
				for (unsigned o = 0; o < 129; o++)
					if (shadow.bis_hist[a][o])
						fprintf(stderr, "  %-28s %s %02x: %u tareas\n", salsa64_abi_name((enum salsa64_abi)a),
							o == 128 ? "(sin diferencia en ningún k)" : "op", o == 128 ? 0 : o, shadow.bis_hist[a][o]);
		}
	}
	if (wav_path && !audio_wav(wav_path)) {
		free(rom);
		return 3;
	}
	interp_report();
	if (rom_paged)
		fprintf(stderr, "salsa64-run: ROM por fragmentos: %llu accesos, %llu fallos, %llu páginas por adelantado, "
			"%llu errores, %.1f MB leídos del archivo\n", (unsigned long long)rom_pg.acc,
			(unsigned long long)rom_pg.miss, (unsigned long long)rom_pg.ra, (unsigned long long)rom_pg.errs,
			(double)rom_pg.read_bytes / 1048576.0);
	if (getenv("SALSA64_PS3_RB")) {
		extern uint32_t ps3_rb, ps3_rb_old;
		fprintf(stderr, "salsa64-run: ps3-rb: la regla de copias de la PS3 pediría %u (con el ancho del último origen "
			"agregado al anillo, la de antes: %u)\n", ps3_rb, ps3_rb_old);
	}
	if ((frame_path && !dump_frame(frame_path)) || (gfx_path && !gfx_dump(gfx_path))) {
		free(rom);
		return 3;
	}
	free(rom);
	if (gfx_check && !gfxtest_check())
		return 1;
	if (audio_check && !audiotest_check())
		return 1;

	switch (reason) {
	case STOP_FAIL:   return 1;
	case STOP_STUCK:  return 2;
	case STOP_CYCLES: return watch.until ? 2 : 0;
	default:          return 0;
	}
}
