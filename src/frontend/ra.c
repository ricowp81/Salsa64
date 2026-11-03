/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Salsa64 — RetroAchievements en la PS3; ver ra.h. */
#include "ra.h"
#include "ra_rules.h"

#include <ppu-lv2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/memory.h>
#include <sys/stat.h>
#include <sys/systime.h>
#include <sys/mutex.h>
#include <sys/sem.h>
#include <sys/thread.h>
#include <sysutil/osk.h>
#include <sysutil/sysutil.h>

#include "i18n.h"
#include "netstack.h"
#include "platform/ps3/log.h"
#include "platform/ps3/paths.h"
#include "version.h"

#define RA_CFG_PATH   SALSA64_DATA_DIR "/ra.cfg"
#define RA_JOBS       24
#define RA_NOTICES    8
#define RA_RESP_MAX   (4u << 20)
#define RA_OSK_MAX    63
#define RA_OSK_BYTES  (4u << 20)
#define RA_COST_EVERY 1800

enum { JOB_HTTP, JOB_HASH };

struct job {
	int type;
	unsigned id;
	char *url, *post, *ctype;
	const uint8_t *rom;
	size_t rom_size;
};

static struct {
	struct ra_core *core;
	bool inited, enabled, hardcore, strict, tried_token;
	char user[RA_USER_MAX], token[RA_TOKEN_MAX], host[160];
	char agent[96];

	sys_lwmutex_t core_lock, q_lock;
	sys_sem_t sem;
	sys_ppu_thread_t tid;
	bool thread_up, locks_up;
	volatile int stop;
	struct job jobs[RA_JOBS];
	unsigned jh, jn;
	char notices[RA_NOTICES][160];
	unsigned nh, nn;

	/* el juego abierto */
	volatile const uint8_t *rom;
	volatile int hashing;
	volatile bool active, game_hardcore;
	unsigned frames;
	uint64_t last_idle, login_t;

	struct ra_item list[RA_ITEMS_MAX];
	unsigned list_n, list_unlocked;
	char game_title[RA_TITLE_MAX];
} ra;

/* ---- ra.cfg: usuario, token, servidor y certificados (nunca la contraseña) ---- */

static void cfg_read(void)
{
	FILE *f = fopen(RA_CFG_PATH, "r");
	ra.strict = true;
	if (!f)
		return;
	char line[256];
	while (fgets(line, sizeof line, f)) {
		char *eq = strchr(line, '=');
		if (line[0] == '#' || !eq)
			continue;
		*eq = '\0';
		char *key = line, *val = eq + 1;
		while (*key == ' ' || *key == '\t')
			key++;
		for (char *e = key + strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t'); )
			*--e = '\0';
		while (*val == ' ' || *val == '\t')
			val++;
		val[strcspn(val, "\r\n")] = '\0';
		for (char *e = val + strlen(val); e > val && (e[-1] == ' ' || e[-1] == '\t'); )
			*--e = '\0';
		if (!strcmp(key, "usuario"))
			snprintf(ra.user, sizeof ra.user, "%s", val);
		else if (!strcmp(key, "token"))
			snprintf(ra.token, sizeof ra.token, "%s", val);
		else if (!strcmp(key, "servidor"))
			snprintf(ra.host, sizeof ra.host, "%s", val);
		else if (!strcmp(key, "certificados"))
			ra.strict = strcmp(val, "laxo") != 0;
	}
	fclose(f);
}

static bool cfg_write(void)
{
	mkdir(SALSA64_DATA_DIR, 0777);
	FILE *f = fopen(RA_CFG_PATH, "w");
	if (!f)
		return false;
	fprintf(f, "# RetroAchievements (docs/retroachievements.md). Aqui solo va el TOKEN que devuelve el sitio; la\n"
		   "# contrasena no se guarda. Para cerrar la sesion: Opciones > RetroAchievements, o borrar este archivo.\n"
		   "usuario = %s\ntoken = %s\n", ra.user, ra.token);
	if (ra.host[0])
		fprintf(f, "servidor = %s\n", ra.host);
	if (!ra.strict)
		fprintf(f, "certificados = laxo\n");
	else
		fprintf(f, "# certificados = laxo   (acepta una cadena de certificados que la consola no puede comprobar)\n");
	bool ok = fclose(f) == 0;
	return ok;
}

/* ---- Avisos y cola de trabajos ---- */

static void q_lock(void)
{
	sysLwMutexLock(&ra.q_lock, 0);
}

static void q_unlock(void)
{
	sysLwMutexUnlock(&ra.q_lock);
}

static void push_notice(const char *text)
{
	q_lock();
	if (ra.nn == RA_NOTICES) {  /* se pierde el más viejo */
		ra.nh = (ra.nh + 1) % RA_NOTICES;
		ra.nn--;
	}
	snprintf(ra.notices[(ra.nh + ra.nn) % RA_NOTICES], sizeof ra.notices[0], "%s", text);
	ra.nn++;
	q_unlock();
}

bool ra_poll_notice(char *buf, size_t size)
{
	if (!ra.locks_up)
		return false;
	bool got = false;
	q_lock();
	if (ra.nn) {
		snprintf(buf, size, "%s", ra.notices[ra.nh]);
		ra.nh = (ra.nh + 1) % RA_NOTICES;
		ra.nn--;
		got = true;
	}
	q_unlock();
	return got;
}

static void ops_notice(void *ctx, const char *text)
{
	(void)ctx;
	push_notice(text);
}

static void ops_log(void *ctx, const char *text)
{
	(void)ctx;
	salsa64_log("%s", text);
}

static uint64_t ops_now(void *ctx)
{
	(void)ctx;
	return sysGetSystemTime();
}

static void ops_lock(void *ctx)
{
	(void)ctx;
	sysLwMutexLock(&ra.core_lock, 0);
}

static void ops_unlock(void *ctx)
{
	(void)ctx;
	sysLwMutexUnlock(&ra.core_lock);
}

static bool ops_trylock(void *ctx)
{
	(void)ctx;
	return sysLwMutexTryLock(&ra.core_lock) == 0;
}

static void worker_start(void);
static bool osk_busy(void);

static char *dupstr(const char *s)
{
	if (!s)
		return NULL;
	size_t n = strlen(s) + 1;
	char *d = malloc(n);
	if (d)
		memcpy(d, s, n);
	return d;
}

static void job_free(struct job *j)
{
	if (j->post) {
		memset(j->post, 0, strlen(j->post));  /* lleva el token, o la contraseña al iniciar sesión */
		free(j->post);
	}
	free(j->url);
	free(j->ctype);
	memset(j, 0, sizeof *j);
}

static bool job_push(const struct job *j)
{
	bool ok = false;
	q_lock();
	if (ra.jn < RA_JOBS) {
		ra.jobs[(ra.jh + ra.jn) % RA_JOBS] = *j;
		ra.jn++;
		ok = true;
	}
	q_unlock();
	if (ok)
		sysSemPost(ra.sem, 1);
	return ok;
}

static bool job_pop(struct job *j)
{
	bool ok = false;
	q_lock();
	if (ra.jn) {
		*j = ra.jobs[ra.jh];
		memset(&ra.jobs[ra.jh], 0, sizeof *j);
		ra.jh = (ra.jh + 1) % RA_JOBS;
		ra.jn--;
		ok = true;
	}
	q_unlock();
	return ok;
}

/* Un POST de rc_client: se copia y lo hace el hilo de red. */
static void ops_send(void *ctx, unsigned id, const char *url, const char *post, const char *ctype)
{
	(void)ctx;
	struct job j;
	memset(&j, 0, sizeof j);
	j.type = JOB_HTTP;
	j.id = id;
	j.url = dupstr(url);
	j.post = dupstr(post);
	j.ctype = dupstr(ctype);
	worker_start();
	if (!j.url || !job_push(&j)) {
		job_free(&j);
		ra_core_complete(ra.core, id, 0, NULL, 0);  /* sin lugar o sin memoria: rc_client lo reintenta */
	}
}

/* ---- Hilo de red ---- */

static void do_http(struct job *j, bool net_ok)
{
	uint8_t *buf = NULL;
	size_t len = 0;
	int code = 0, rc = -1;
	char kind[24] = "?";
	if (j->post) {
		const char *r = strstr(j->post, "r=");
		if (r) {
			size_t n = strcspn(r + 2, "&");
			if (n >= sizeof kind)
				n = sizeof kind - 1;
			memcpy(kind, r + 2, n);
			kind[n] = '\0';
		}
	}
	if (net_ok) {
		struct ns_req r = { j->post ? "POST" : "GET", j->url, j->post, j->post ? strlen(j->post) : 0, j->ctype,
				    ra.agent, RA_RESP_MAX, ra.strict, &ra.stop };
		rc = netstack_request(&r, &buf, &len, &code);
	} else {
		code = -4;
	}
	if (rc == 0) {
		salsa64_log("RA: %s -> HTTP 200, %u bytes", kind, (unsigned)len);
		ra_core_complete(ra.core, j->id, 200, (const char *)buf, len);
	} else {
		salsa64_log("RA: %s -> %s %d (0x%x)", kind, code >= 100 ? "HTTP" : "error", code, (unsigned)code);
		/* Un estado HTTP sin cuerpo (4xx, 5xx) o sin respuesta: rc_client decide si reintenta. */
		ra_core_complete(ra.core, j->id, code >= 100 ? code : 0, NULL, 0);
	}
	free(buf);
}

static void do_hash(struct job *j)
{
	char hash[33];
	if (ra.rom != j->rom) {  /* el juego se cerró antes de empezar: la ROM puede ya no estar */
		ra.hashing = 0;
		return;
	}
	uint64_t t0 = sysGetSystemTime();
	bool ok = ra_rom_hash(j->rom, j->rom_size, hash);
	uint64_t t1 = sysGetSystemTime();
	if (!ok) {
		salsa64_log("RA: la ROM no se pudo identificar (%u bytes)", (unsigned)j->rom_size);
		push_notice(tr("RA: no se pudo identificar la ROM"));
	} else if (ra.rom == j->rom) {  /* el juego sigue abierto */
		salsa64_log("RA: huella %s (%u bytes, %.0f ms)", hash, (unsigned)j->rom_size, (double)(t1 - t0) / 1000.0);
		ra_core_load_game(ra.core, hash);
	}
	ra.hashing = 0;
}

static void worker(void *arg)
{
	(void)arg;
	const char *what = "";
	int rc = netstack_acquire(&what);
	if (rc < 0)
		salsa64_log("RA: red: falló %s (%d)", what, rc);
	bool net_ok = rc >= 0;
	while (!ra.stop) {
		sysSemWait(ra.sem, 500000);
		struct job j;
		while (!ra.stop && !osk_busy() && job_pop(&j)) {  /* con el teclado abierto, la red espera */
			if (j.type == JOB_HASH)
				do_hash(&j);
			else
				do_http(&j, net_ok);
			job_free(&j);
		}
	}
	if (net_ok)
		netstack_release();
	sysThreadExit(0);
}

static void worker_start(void)
{
	/* Con el teclado del sistema abierto no se levanta la pila HTTPS (sysModuleLoad,
	 * netInitialize): ra_osk_step lo vuelve a pedir al descargarse. */
	if (osk_busy())
		return;
	q_lock();
	bool start = !ra.thread_up;
	if (start)
		ra.thread_up = true;
	q_unlock();
	if (!start)
		return;
	ra.stop = 0;
	if (sysThreadCreate(&ra.tid, worker, NULL, 1500, 0x30000, THREAD_JOINABLE, (char *)"ra") != 0) {
		salsa64_log("RA: no se pudo crear el hilo de red");
		ra.thread_up = false;
	}
}

/* ---- Teclado del sistema ---- */

enum { OSK_IDLE, OSK_USER, OSK_PASS };
/* Pasos del teclado (ra_osk_step, solo en el bucle principal): el callback de
 * sysutil únicamente anota banderas; oskUnloadAsync, oskAbort y el contenedor
 * van en el bucle. */
enum { OS_NONE, OS_OPENING, OS_TYPING, OS_UNLOADING };
#define OSK_STEP_US 5000000ull  /* tope de cada paso que no depende de quien teclea */
static struct {
	int stage, step;
	sys_mem_container_t cont;
	bool cont_ok, accepted;
	u16 text[RA_OSK_MAX + 1], msg[96];
	oskCallbackReturnParam ret;  /* estático: lo escribe el sistema en oskUnloadAsync */
	char user[RA_USER_MAX], pass[160];
	uint64_t step_t;
	volatile int ev_loaded, ev_finished, ev_canceled, ev_unloaded;
} osk;

static void utf8_to_u16(const char *s, u16 *out, size_t cap)
{
	size_t n = 0;
	while (*s && n + 1 < cap) {
		unsigned char c = (unsigned char)*s;
		u32 cp = '?';
		if (c < 0x80) {
			cp = c;
			s += 1;
		} else if ((c & 0xE0) == 0xC0 && s[1]) {
			cp = ((u32)(c & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
			s += 2;
		} else if ((c & 0xF0) == 0xE0 && s[1] && s[2]) {
			cp = ((u32)(c & 0x0F) << 12) | (((unsigned char)s[1] & 0x3F) << 6) | ((unsigned char)s[2] & 0x3F);
			s += 3;
		} else {
			s += 1;  /* fuera del plano básico: se descarta */
			continue;
		}
		out[n++] = (u16)cp;
	}
	out[n] = 0;
}

#define u16_to_utf8 ra_u16_to_utf8

static void osk_scrub(void)
{
	memset(osk.text, 0, sizeof osk.text);
	memset(osk.pass, 0, sizeof osk.pass);
	osk.ret.len = 0;
}

static void osk_release(void)
{
	if (osk.cont_ok) {
		sysMemContainerDestroy(osk.cont);
		osk.cont_ok = false;
		salsa64_log("RA: teclado: contenedor liberado");
	}
}

static void osk_end(void)
{
	osk.stage = OSK_IDLE;
	osk.step = OS_NONE;
	osk_release();
	osk_scrub();
	memset(osk.user, 0, sizeof osk.user);
	if (ra.jn)
		worker_start();  /* la red espera a que el teclado se descargue */
}

static bool osk_open(int stage)
{
	salsa64_log("RA: teclado: abriendo (%s)", stage == OSK_USER ? "usuario" : "contraseña");
	if (sysMemContainerCreate(&osk.cont, RA_OSK_BYTES) != 0) {
		salsa64_log("RA: sin memoria para el teclado del sistema");
		return false;
	}
	osk.cont_ok = true;
	osk.stage = stage;
	osk.step = OS_OPENING;
	osk.step_t = sysGetSystemTime();
	osk.accepted = false;
	osk.ev_loaded = osk.ev_finished = osk.ev_canceled = osk.ev_unloaded = 0;
	memset(osk.text, 0, sizeof osk.text);
	utf8_to_u16(tr(stage == OSK_USER ? "Usuario de RetroAchievements" : "Contraseña de RetroAchievements"), osk.msg,
		    sizeof osk.msg / sizeof osk.msg[0]);
	static u16 start[1] = { 0 };
	oskInputFieldInfo info = { osk.msg, start, RA_OSK_MAX };
	oskParam param;
	memset(&param, 0, sizeof param);
	param.allowedPanels = OSK_PANEL_TYPE_ALPHABET | OSK_PANEL_TYPE_NUMERAL | OSK_PANEL_TYPE_LATIN | OSK_PANEL_TYPE_ENGLISH;
	param.firstViewPanel = OSK_PANEL_TYPE_ALPHABET;
	param.prohibitFlags = OSK_PROHIBIT_RETURN;
	if (stage == OSK_PASS) {
		/* El panel de contraseñas del sistema oculta lo que se teclea. */
		param.allowedPanels |= OSK_PANEL_TYPE_PASSWORD;
		param.firstViewPanel = OSK_PANEL_TYPE_PASSWORD;
	} else {
		param.prohibitFlags |= OSK_PROHIBIT_SPACE;
	}
	oskSetKeyLayoutOption(OSK_FULLKEY_PANEL);
	oskSetInitialInputDevice(OSK_DEVICE_PAD);
	oskSetInitialKeyLayout(OSK_INITIAL_FULLKEY_PANEL);
	oskSetLayoutMode(OSK_LAYOUTMODE_HORIZONTAL_ALIGN_CENTER | OSK_LAYOUTMODE_VERTICAL_ALIGN_CENTER);
	int rc = oskLoadAsync(osk.cont, &param, &info);
	salsa64_log("RA: teclado: oskLoadAsync = %d", rc);
	if (rc != 0) {
		osk_end();
		return false;
	}
	return true;
}

bool ra_osk_active(void)
{
	return osk.stage != OSK_IDLE;
}

/* El callback de sysutil (hilo principal, dentro de sysUtilCheckCallback): solo
 * registra y anota banderas. */
bool ra_sysutil_event(uint64_t status, uint64_t param)
{
	switch (status) {
	case SYSUTIL_OSK_LOADED:
	case SYSUTIL_OSK_DONE:
	case SYSUTIL_OSK_UNLOADED:
	case SYSUTIL_OSK_INPUT_ENTERED:
	case SYSUTIL_OSK_INPUT_CANCELED:
		break;
	default:
		return false;
	}
	salsa64_log("RA: teclado: evento sysutil 0x%llx (parámetro 0x%llx)", (unsigned long long)status,
		    (unsigned long long)param);
	if (status == SYSUTIL_OSK_LOADED)
		osk.ev_loaded = 1;
	else if (status == SYSUTIL_OSK_DONE)
		osk.ev_finished = 1;
	else if (status == SYSUTIL_OSK_INPUT_CANCELED)
		osk.ev_canceled = 1;
	else if (status == SYSUTIL_OSK_UNLOADED)
		osk.ev_unloaded = 1;
	return true;
}

static void osk_timeout(const char *where)
{
	salsa64_log("RA: teclado: tope de tiempo esperando %s; se aborta", where);
	oskAbort();
	osk_end();
	push_notice(tr("RA: el teclado del sistema no responde"));
}

/* Un paso del teclado por vuelta del bucle principal (que sigue haciendo flip y
 * sysUtilCheckCallback): LOADED → (quien teclea) → FINISHED → oskUnloadAsync →
 * UNLOADED → recién entonces se lee el texto y se libera el contenedor. */
static void osk_step(void)
{
	if (osk.step == OS_NONE)
		return;
	uint64_t now = sysGetSystemTime();
	if (osk.step == OS_OPENING) {
		if (osk.ev_loaded) {
			osk.step = OS_TYPING;
			salsa64_log("RA: teclado: abierto, esperando al usuario");
		} else if (now - osk.step_t > OSK_STEP_US) {
			osk_timeout("que abra");
		}
		return;
	}
	if (osk.step == OS_TYPING) {
		if (osk.ev_finished || osk.ev_canceled) {
			bool fin = osk.ev_finished != 0;
			memset(&osk.ret, 0, sizeof osk.ret);
			osk.ret.res = OSK_OK;
			osk.ret.len = RA_OSK_MAX;
			osk.ret.str = osk.text;
			osk.accepted = fin;
			if (!fin) {
				salsa64_log("RA: teclado: cancelado, oskAbort");
				oskAbort();
			}
			int rc = oskUnloadAsync(&osk.ret);
			salsa64_log("RA: teclado: oskUnloadAsync = %d", rc);
			osk.step = OS_UNLOADING;
			osk.step_t = now;
		}
		return;  /* teclear no tiene tope */
	}
	/* OS_UNLOADING */
	if (!osk.ev_unloaded) {
		if (now - osk.step_t > OSK_STEP_US)
			osk_timeout("que se descargue");
		return;
	}
	int stage = osk.stage;
	bool ok = osk.accepted && osk.ret.res == OSK_OK && osk.ret.len > 0;
	salsa64_log("RA: teclado: descargado (res %d, %d caracteres)", (int)osk.ret.res, (int)osk.ret.len);
	osk_release();
	osk.step = OS_NONE;
	osk.stage = OSK_IDLE;
	if (!ok) {
		osk_end();
		return;
	}
	if (stage == OSK_USER) {
		u16_to_utf8(osk.text, osk.ret.len, osk.user, sizeof osk.user);
		memset(osk.text, 0, sizeof osk.text);
		if (!osk.user[0] || !osk_open(OSK_PASS))
			osk_end();
		return;
	}
	u16_to_utf8(osk.text, osk.ret.len, osk.pass, sizeof osk.pass);
	memset(osk.text, 0, sizeof osk.text);
	salsa64_log("RA: teclado: usuario «%s» (%u bytes), contraseña de %u bytes", osk.user, (unsigned)strlen(osk.user),
		    (unsigned)strlen(osk.pass));
	snprintf(ra.user, sizeof ra.user, "%s", osk.user);
	push_notice(tr("RA: conectando…"));
	ra.login_t = now;
	worker_start();  /* con el teclado ya descargado */
	if (!ra_core_login_begin(ra.core, osk.user, osk.pass))  /* borra osk.pass */
		ra.login_t = 0;
	osk_end();
}

static bool osk_busy(void)
{
	return osk.stage != OSK_IDLE;
}

/* ---- API ---- */

bool ra_init(void)
{
	if (ra.inited)
		return true;
	memset(&ra, 0, sizeof ra);
	netstack_init();
	sys_lwmutex_attr_t rec = { SYS_LWMUTEX_PROTOCOL_FIFO, SYS_LWMUTEX_ATTR_RECURSIVE, "racore" };
	sys_lwmutex_attr_t nrec = { SYS_LWMUTEX_PROTOCOL_FIFO, SYS_LWMUTEX_ATTR_NOT_RECURSIVE, "raq" };
	sys_sem_attr_t sa = { SYS_SEM_ATTR_PROTOCOL, SYS_SEM_ATTR_PSHARED, 0, 0, 0, "ra" };
	if (sysLwMutexCreate(&ra.core_lock, &rec) != 0 || sysLwMutexCreate(&ra.q_lock, &nrec) != 0 ||
	    sysSemCreate(&ra.sem, &sa, 0, RA_JOBS * 2) != 0) {
		salsa64_log("RA: no se pudieron crear los cerrojos");
		return false;
	}
	ra.locks_up = true;
	cfg_read();
	/* «Salsa64/0.2.13 rcheevos/12.5»: sin la «v» ni espacios de la versión. */
	const char *v = salsa64_version[0] == 'v' ? salsa64_version + 1 : salsa64_version;
	char ver[48];
	snprintf(ver, sizeof ver, "%s", v);
	for (char *p = ver; *p; p++)
		if (*p == ' ' || *p == '/' || *p == '"')
			*p = '_';
	struct ra_ops ops = { NULL, ops_send, ops_lock, ops_unlock, ops_trylock, ops_notice, ops_log, ops_now };
	ra.core = ra_core_create(&ops);
	if (!ra.core) {
		salsa64_log("RA: sin memoria para rc_client");
		return false;
	}
	snprintf(ra.agent, sizeof ra.agent, "Salsa64/%s %s", ver, ra_core_agent_clause(ra.core));
	if (ra.host[0])
		ra_core_set_host(ra.core, ra.host);
	ra.inited = true;
	salsa64_log("RA: %s; cuenta %s; servidor %s; certificados %s", ra.agent, ra.token[0] ? "guardada" : "sin iniciar",
		    ra.host[0] ? ra.host : "retroachievements.org", ra.strict ? "estrictos" : "laxos (ra.cfg)");
	return true;
}

void ra_shutdown(void)
{
	if (!ra.inited)
		return;
	ra.active = false;
	ra.stop = 1;
	if (ra.thread_up) {
		sysSemPost(ra.sem, 1);
		u64 rv;
		sysThreadJoin(ra.tid, &rv);
		ra.thread_up = false;
	}
	ra_core_destroy(ra.core);
	ra.core = NULL;
	ra.inited = false;
	memset(ra.token, 0, sizeof ra.token);
}

void ra_apply(bool enabled, bool hardcore)
{
	if (!ra.inited)
		return;
	bool was = ra.enabled;
	ra.enabled = enabled;
	ra.hardcore = hardcore;
	/* Con un juego abierto, Hardcore no se activa sin reiniciarlo (queda para
	 * el siguiente juego); desactivarlo sí vale al instante. */
	bool deferred;
	bool mode = ra_rules_mode_change(ra.active, ra.game_hardcore, hardcore, &deferred);
	if (ra.active) {
		ra.game_hardcore = mode;
		if (deferred)
			salsa64_log("RA: Hardcore pedido con un juego abierto; vale desde el siguiente");
		ra_core_set_hardcore(ra.core, mode);
	} else {
		ra_core_set_hardcore(ra.core, hardcore);
	}
	if (enabled && (!was || !ra.tried_token) && ra.user[0] && ra.token[0] && ra_core_login_state(ra.core) == RA_LOGGED_OUT) {
		ra.tried_token = true;
		salsa64_log("RA: iniciando sesión con el token guardado");
		ra_core_login_token(ra.core, ra.user, ra.token);
	}
}

bool ra_enabled(void)
{
	return ra.inited && ra.enabled;
}

const char *ra_user(void)
{
	static char name[RA_USER_MAX];
	if (!ra.inited || ra_core_login_state(ra.core) != RA_LOGGED_IN)
		return NULL;
	return ra_core_user(ra.core, name, sizeof name, NULL, 0) ? name : NULL;
}

bool ra_logging_in(void)
{
	return ra.inited && (ra_core_login_state(ra.core) == RA_LOGGING_IN || osk.stage != OSK_IDLE);
}

void ra_login_begin(void)
{
	if (!ra.inited || osk.stage != OSK_IDLE)
		return;
	osk_open(OSK_USER);
}

void ra_logout(void)
{
	if (!ra.inited)
		return;
	ra_game_close();
	ra_core_logout(ra.core);
	memset(ra.token, 0, sizeof ra.token);
	ra.user[0] = '\0';
	ra.tried_token = false;
	cfg_write();
	salsa64_log("RA: sesión cerrada, token borrado de ra.cfg");
}

void ra_game_open(const uint8_t *rom, size_t rom_size, const uint8_t *rdram, size_t rdram_size)
{
	ra.active = false;
	ra.frames = 0;
	if (!ra.inited || !ra.enabled)
		return;
	ra_core_set_memory(ra.core, rdram, (uint32_t)rdram_size);
	ra_core_set_hardcore(ra.core, ra.hardcore);
	if (ra_core_login_state(ra.core) == RA_LOGGED_OUT) {
		push_notice(tr("RA: inicia sesión en Opciones"));
		salsa64_log("RA: activado pero sin sesión; el juego corre sin logros");
		return;
	}
	ra.rom = rom;
	ra.game_hardcore = ra.hardcore;
	ra_core_cost(ra.core, NULL, NULL);
	struct job j;
	memset(&j, 0, sizeof j);
	j.type = JOB_HASH;
	j.rom = rom;
	j.rom_size = rom_size;
	worker_start();
	ra.hashing = 1;  /* hasta que el hilo de red termine con la ROM (ra_game_close espera) */
	if (!ra.thread_up || !job_push(&j)) {
		ra.hashing = 0;
		ra.rom = NULL;
		salsa64_log("RA: sin hilo de red o cola llena, sin logros en este juego");
		return;
	}
	ra.active = true;
}

void ra_game_close(void)
{
	if (!ra.inited)
		return;
	bool had = ra.active || ra_core_game_state(ra.core) != RA_GAME_NONE;
	ra.active = false;
	ra.rom = NULL;
	for (int i = 0; ra.hashing && i < 5000; i++)  /* la huella todavía lee la ROM */
		sysUsleep(1000);
	if (had) {
		unsigned calls = 0;
		double worst = 0, avg = ra_core_cost(ra.core, &calls, &worst);
		if (calls)
			salsa64_log("RA: al cerrar, %u campos, %.1f µs por campo (peor %.0f µs)", calls, avg, worst);
		ra_core_unload_game(ra.core);
	}
	ra.list_n = 0;
}

void ra_frame(void)
{
	if (!ra.active)
		return;
	ra_core_frame(ra.core);
	if (++ra.frames % RA_COST_EVERY == 0) {
		unsigned calls = 0;
		double worst = 0, avg = ra_core_cost(ra.core, &calls, &worst);
		salsa64_log("RA: %u campos, %.1f µs por campo (peor %.0f µs), %u saltados", calls, avg, worst,
			    ra_core_skipped(ra.core));
	}
}

bool ra_hardcore_active(void)
{
	/* Desde que se abre el juego (la huella y los datos tardan unos segundos)
	 * hasta que se sabe que no tiene logros o no se pudo cargar. */
	if (!ra.inited || !ra.game_hardcore || !ra.active)
		return false;
	int st = ra_core_game_state(ra.core);
	return st == RA_GAME_LOADED || st == RA_GAME_LOADING || st == RA_GAME_NONE;
}

bool ra_game_loaded(void)
{
	return ra.inited && ra_core_game_state(ra.core) == RA_GAME_LOADED;
}

void ra_pump(bool paused)
{
	if (!ra.inited)
		return;
	osk_step();
	if (ra.login_t && ra_core_login_state(ra.core) != RA_LOGGING_IN)
		ra.login_t = 0;
	if (ra.login_t && sysGetSystemTime() - ra.login_t > 40000000ull) {
		salsa64_log("RA: inicio de sesión sin respuesta tras 40 s; se cancela");
		ra_core_login_abort(ra.core);
		ra.login_t = 0;
		push_notice(tr("RA: sin respuesta del servidor (tiempo agotado)"));
	}
	if (ra_core_take_new_token(ra.core)) {
		char user[RA_USER_MAX], token[RA_TOKEN_MAX];
		if (ra_core_user(ra.core, user, sizeof user, token, sizeof token)) {
			snprintf(ra.user, sizeof ra.user, "%s", user);
			snprintf(ra.token, sizeof ra.token, "%s", token);
			salsa64_log("RA: token %s en ra.cfg", cfg_write() ? "guardado" : "NO se pudo guardar");
		}
		memset(token, 0, sizeof token);
	}
	if (paused && ra.active) {
		uint64_t now = sysGetSystemTime();
		if (now - ra.last_idle > 250000) {
			ra.last_idle = now;
			ra_core_idle(ra.core);
		}
	}
}

unsigned ra_list_refresh(void)
{
	ra.list_n = 0;
	ra.list_unlocked = 0;
	ra.game_title[0] = '\0';
	if (!ra.inited)
		return 0;
	ra.list_n = ra_core_items(ra.core, ra.list, RA_ITEMS_MAX);
	for (unsigned i = 0; i < ra.list_n; i++)
		ra.list_unlocked += ra.list[i].unlocked;
	ra_core_summary(ra.core, ra.game_title, sizeof ra.game_title, NULL, NULL, NULL, NULL);
	return ra.list_n;
}

unsigned ra_list_count(void)
{
	return ra.list_n;
}

const struct ra_item *ra_list_get(unsigned i)
{
	return i < ra.list_n ? &ra.list[i] : NULL;
}

const char *ra_game_title(void)
{
	return ra.game_title;
}

unsigned ra_list_unlocked(void)
{
	return ra.list_unlocked;
}
