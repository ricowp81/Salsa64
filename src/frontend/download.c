/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Salsa64 — descargas con libhttp; ver download.h. Basado en el ejemplo
 * network/httptest de PSL1GHT. */
#include "download.h"

#include "i18n.h"

#include <net/net.h>
#include <net/netctl.h>
#include <ppu-lv2.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/thread.h>

#include "cover.h"
#include "dlutil.h"
#include "i18n.h"
#include "namematch.h"
#include "netstack.h"
#include "platform/ps3/log.h"
#include "platform/ps3/paths.h"

#define DL_CFG_PATH SALSA64_DATA_DIR "/descargas.cfg"
#define DL_COVERS   SALSA64_DATA_DIR "/covers"
#define DL_CHEATS   SALSA64_DATA_DIR "/cheats"
#define INDEX_MAX   (6u << 20)
#define COVER_MAX   COVER_MAX_FILE_BYTES
#define CHEAT_MAX   (2u << 20)
#define USER_AGENT  "Mozilla/5.0 (PLAYSTATION 3; 1.00) Salsa64"

static struct {
	volatile int state, cancel;
	int what;
	volatile unsigned done, total, got, have, missing;
	char err[96];
	struct dl_rom *roms;
	unsigned n;
	sys_ppu_thread_t tid;
	bool joinable;
} job;

/* GET de `url` a un búfer que crece hasta `cap` (netstack_request). */
static int http_get(const char *url, uint8_t **buf, size_t *len, size_t cap, int *code)
{
	struct ns_req r = { "GET", url, NULL, 0, NULL, USER_AGENT, cap, 0, &job.cancel };
	return netstack_request(&r, buf, len, code);
}

static bool write_file(const char *path, const uint8_t *data, size_t len)
{
	FILE *f = fopen(path, "wb");
	bool ok = f && fwrite(data, 1, len, f) == len;
	if (f && fclose(f) != 0)
		ok = false;
	if (!ok)
		remove(path);
	return ok;
}

static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fail(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(job.err, sizeof job.err, tr(fmt), ap);
	va_end(ap);
	job.state = DL_FAILED;
}

/* descargas.cfg; false si no existe (las descargas están apagadas). */
static bool read_cfg(struct dl_cfg *cfg)
{
	char text[2048];
	dl_cfg_default(cfg);
	FILE *f = fopen(DL_CFG_PATH, "r");
	if (!f)
		return false;
	size_t n = fread(text, 1, sizeof text - 1, f);
	fclose(f);
	dl_cfg_parse(cfg, text, n);
	return true;
}

bool dl_enabled(void)
{
	struct dl_cfg cfg;
	bool on = read_cfg(&cfg) && cfg.enabled;
	salsa64_log("descargas: %s", on ? "encendidas (experimental, descargas.cfg)" : "apagadas");
	return on;
}

/* Nombre que se puede guardar: sin carpetas ni puntos al principio. */
static bool safe_name(const char *n)
{
	return n[0] && n[0] != '.' && !strchr(n, '/') && !strchr(n, '\\') && !strstr(n, "..") && strlen(n) < 160;
}

static void job_main(void *arg)
{
	(void)arg;
	struct dl_cfg cfg;
	const bool covers = job.what == 0;
	const char *ext = covers ? ".png" : ".cht", *dir = covers ? DL_COVERS : DL_CHEATS;
	const size_t min_key = covers ? 4 : 8, cap = covers ? COVER_MAX : CHEAT_MAX;
	uint8_t *index = NULL, *file = NULL;
	char *names = NULL;
	size_t ilen = 0, nlen = 0;
	int code = 0;
	const char *step = "";
	bool netctl = false, acquired = false;

	if (!read_cfg(&cfg) || !cfg.enabled) {
		fail("Las descargas están apagadas (descargas.cfg)");
		goto out;
	}
	const char *base = covers ? cfg.covers : cfg.cheats;
	char index_url[DL_URL_MAX + 16];
	snprintf(index_url, sizeof index_url, "%s", covers ? cfg.covers_index : cfg.cheats_index);
	if (!base[0]) {
		fail("Falta la dirección en descargas.cfg");
		goto out;
	}
	if (!index_url[0])
		snprintf(index_url, sizeof index_url, "%sindex.txt", base);

	int rc = netstack_acquire(&step);
	acquired = rc >= 0;
	if (rc < 0) {
		salsa64_log("descargas: red: falló %s (%d)", step, rc);
		fail("No hay red: conecta la consola a internet");
		goto out;
	}
	s32 netstate = 0;
	netctl = netCtlInit() >= 0;
	if (!netctl || netCtlGetState(&netstate) < 0 || netstate != NET_CTL_STATE_IPObtained) {
		salsa64_log("descargas: sin conexión (estado %d)", (int)netstate);
		fail("No hay red: conecta la consola a internet");
		goto out;
	}
	mkdir(SALSA64_DATA_DIR, 0777);
	mkdir(dir, 0777);

	salsa64_log("descargas: índice %s", index_url);
	if (http_get(index_url, &index, &ilen, INDEX_MAX, &code) < 0) {
		salsa64_log("descargas: índice: error %d", code);
		if (code == -2) {
			job.state = DL_CANCELLED;
			goto out;
		}
		char why[24];
		if (code >= 100)
			snprintf(why, sizeof why, "HTTP %d", code);
		else
			snprintf(why, sizeof why, "0x%x", (unsigned)code);
		fail("No se pudo bajar el índice: %s", why);
		goto out;
	}
	names = malloc(ilen + 1);
	if (!names) {
		fail("No se pudo bajar el índice: %s", tr("memoria"));
		goto out;
	}
	nlen = dl_index_names((const char *)index, ilen, ext, names, ilen + 1);
	free(index);
	index = NULL;
	salsa64_log("descargas: el índice trae %u bytes de nombres %s", (unsigned)nlen, ext);

	for (unsigned i = 0; i < job.n && !job.cancel; i++) {
		const struct dl_rom *r = &job.roms[i];
		char path[320], found[256], url[DL_URL_MAX + 700], esc[600];
		if (name_find(dir, ext, r->name, r->title, r->code, min_key, path, sizeof path)) {
			job.have++;
		} else if (!name_find_list(names, nlen, ext, r->name, r->title, r->code, min_key, found, sizeof found) ||
			   !safe_name(found)) {
			job.missing++;
		} else {
			dl_url_escape(found, esc, sizeof esc);
			snprintf(url, sizeof url, "%s%s", base, esc);
			snprintf(path, sizeof path, "%s/%s", dir, found);
			size_t flen = 0;
			FILE *old = fopen(path, "rb");
			if (old) {
				/* Un archivo del usuario con ese nombre no se pisa. */
				fclose(old);
				job.have++;
			} else if (http_get(url, &file, &flen, cap, &code) == 0) {
				if (write_file(path, file, flen)) {
					job.got++;
					salsa64_log("descargas: %s -> %s (%u bytes)", r->name, path, (unsigned)flen);
				} else {
					job.missing++;
					salsa64_log("descargas: no se pudo escribir %s", path);
				}
				free(file);
				file = NULL;
			} else {
				job.missing++;
				salsa64_log("descargas: %s: error %d", url, code);
				if (code == -2)
					break;
			}
		}
		job.done = i + 1;
	}
	job.state = job.cancel ? DL_CANCELLED : DL_DONE;
	salsa64_log("descargas: %s: %u nuevas, %u ya estaban, %u sin coincidencia", covers ? "carátulas" : "trucos", job.got,
		    job.have, job.missing);
out:
	free(index);
	free(file);
	free(names);
	if (netctl)
		netCtlTerm();
	if (acquired)
		netstack_release();
	sysThreadExit(0);
}

bool dl_start(int what, const struct dl_rom *roms, unsigned n)
{
	if (job.state == DL_RUNNING)
		return false;
	netstack_init();
	dl_shutdown();
	if (!n)
		return false;
	job.roms = malloc((size_t)n * sizeof *roms);
	if (!job.roms)
		return false;
	memcpy(job.roms, roms, (size_t)n * sizeof *roms);
	job.n = n;
	job.what = what;
	job.done = job.got = job.have = job.missing = 0;
	job.total = n;
	job.cancel = 0;
	job.err[0] = '\0';
	job.state = DL_RUNNING;
	if (sysThreadCreate(&job.tid, job_main, NULL, 1500, 0x20000, THREAD_JOINABLE, (char *)"descargas") != 0) {
		job.state = DL_FAILED;
		snprintf(job.err, sizeof job.err, "%s", tr("hilo"));
		free(job.roms);
		job.roms = NULL;
		return false;
	}
	job.joinable = true;
	return true;
}

void dl_cancel(void)
{
	job.cancel = 1;
}

void dl_get(struct dl_status *st)
{
	st->state = job.state;
	st->what = job.what;
	st->done = job.done;
	st->total = job.total;
	st->got = job.got;
	st->have = job.have;
	st->missing = job.missing;
	memcpy(st->err, job.err, sizeof st->err);
}

void dl_shutdown(void)
{
	if (job.joinable) {
		job.cancel = 1;
		u64 rv;
		sysThreadJoin(job.tid, &rv);
		job.joinable = false;
	}
	free(job.roms);
	job.roms = NULL;
}
