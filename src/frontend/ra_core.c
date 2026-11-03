/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Salsa64 — RetroAchievements, parte portable; ver ra_core.h. */
#include "ra_core.h"

#include <rc_client.h>
#include <rc_consoles.h>
#include <rc_hash.h>
#include <rc_version.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "i18n.h"

#define RA_PENDING_MAX 16

struct pending {
	unsigned id;
	rc_client_server_callback_t cb;
	void *cb_data;
};

struct ra_core {
	struct ra_ops ops;
	rc_client_t *client;
	const uint8_t *rdram;
	uint32_t rdram_size;
	struct pending pend[RA_PENDING_MAX];
	unsigned next_id;
	int login;
	rc_client_async_handle_t *login_h;
	int game;
	bool hardcore;
	bool new_token;
	char agent[48];
	/* costo de ra_core_frame */
	uint64_t cost_us, worst_us;
	unsigned calls, skipped;
};

/* ---- Utilidades ---- */

static void lock(struct ra_core *c)
{
	if (c->ops.lock)
		c->ops.lock(c->ops.ctx);
}

static void unlock(struct ra_core *c)
{
	if (c->ops.unlock)
		c->ops.unlock(c->ops.ctx);
}

static void say(struct ra_core *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(struct ra_core *c, const char *fmt, ...)
{
	char text[160];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(text, sizeof text, fmt, ap);
	va_end(ap);
	if (c->ops.notice)
		c->ops.notice(c->ops.ctx, text);
}

static void note(struct ra_core *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void note(struct ra_core *c, const char *fmt, ...)
{
	char text[200];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(text, sizeof text, fmt, ap);
	va_end(ap);
	if (c->ops.log)
		c->ops.log(c->ops.ctx, text);
}

static void copy_trunc(char *dst, size_t n, const char *src)
{
	if (!src)
		src = "";
	size_t len = strlen(src);
	if (len >= n) {
		len = n - 1;
		while (len && ((unsigned char)src[len] & 0xC0) == 0x80)
			len--;  /* no partir un carácter UTF-8 */
	}
	memcpy(dst, src, len);
	dst[len] = '\0';
}

/* ---- Memoria y huella ---- */

void ra_u16_to_utf8(const uint16_t *in, int len, char *out, size_t cap)
{
	size_t n = 0;
	for (int i = 0; i < len && in[i]; i++) {
		uint16_t ch = in[i];
		size_t need = ch < 0x80 ? 1 : ch < 0x800 ? 2 : 3;
		if (n + need >= cap)
			break;
		if (need == 1) {
			out[n++] = (char)ch;
		} else if (need == 2) {
			out[n++] = (char)(0xC0 | (ch >> 6));
			out[n++] = (char)(0x80 | (ch & 0x3F));
		} else {
			out[n++] = (char)(0xE0 | (ch >> 12));
			out[n++] = (char)(0x80 | ((ch >> 6) & 0x3F));
			out[n++] = (char)(0x80 | (ch & 0x3F));
		}
	}
	out[n] = '\0';
}

uint32_t ra_mem_read(const uint8_t *rdram, uint32_t size, uint32_t address, uint8_t *buf, uint32_t n)
{
	if (!rdram || address >= size || n > size - address)
		return 0;
	for (uint32_t i = 0; i < n; i++)
		buf[i] = rdram[(address + i) ^ 3u];
	return n;
}

bool ra_rom_hash(const uint8_t *rom, size_t size, char hash[33])
{
	rc_hash_iterator_t it;
	rc_hash_initialize_iterator(&it, "rom.z64", rom, size);
	int ok = rc_hash_generate(hash, RC_CONSOLE_NINTENDO_64, &it);
	rc_hash_destroy_iterator(&it);
	return ok != 0;
}

static uint32_t read_cb(uint32_t address, uint8_t *buf, uint32_t n, rc_client_t *client)
{
	struct ra_core *c = rc_client_get_userdata(client);
	return ra_mem_read(c->rdram, c->rdram_size, address, buf, n);
}

static rc_clock_t time_cb(const rc_client_t *client)
{
	struct ra_core *c = rc_client_get_userdata((rc_client_t *)client);
	return c->ops.now_us ? c->ops.now_us(c->ops.ctx) / 1000u : 0;
}

/* ---- Red ---- */

static void server_call(const rc_api_request_t *req, rc_client_server_callback_t cb, void *cb_data, rc_client_t *client)
{
	struct ra_core *c = rc_client_get_userdata(client);
	struct pending *p = NULL;
	for (unsigned i = 0; i < RA_PENDING_MAX && !p; i++)
		if (!c->pend[i].cb)
			p = &c->pend[i];
	if (!p) {
		rc_api_server_response_t r = { NULL, 0, RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR };
		note(c, "RA: demasiadas peticiones a la vez");
		cb(&r, cb_data);
		return;
	}
	if (++c->next_id == 0)
		c->next_id = 1;
	p->id = c->next_id;
	p->cb = cb;
	p->cb_data = cb_data;
	c->ops.send(c->ops.ctx, p->id, req->url, req->post_data, req->content_type);
}

void ra_core_complete(struct ra_core *c, unsigned id, int status, const char *body, size_t len)
{
	lock(c);
	for (unsigned i = 0; i < RA_PENDING_MAX; i++) {
		struct pending *p = &c->pend[i];
		if (!p->cb || p->id != id)
			continue;
		rc_client_server_callback_t cb = p->cb;
		void *cb_data = p->cb_data;
		p->cb = NULL;
		rc_api_server_response_t r;
		if (status >= 100) {
			r.body = body;
			r.body_length = len;
			r.http_status_code = status;
		} else {
			r.body = NULL;
			r.body_length = 0;
			r.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
		}
		cb(&r, cb_data);
		break;
	}
	unlock(c);
}

/* ---- Eventos y resultados ---- */

static void event_cb(const rc_client_event_t *ev, rc_client_t *client)
{
	struct ra_core *c = rc_client_get_userdata(client);
	switch (ev->type) {
	case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
		note(c, "RA: logro %u desbloqueado", ev->achievement->id);
		say(c, tr("Logro: %s (%u pts)"), ev->achievement->title, ev->achievement->points);
		break;
	case RC_CLIENT_EVENT_GAME_COMPLETED:
		say(c, "%s", tr("RA: ¡juego completado!"));
		break;
	case RC_CLIENT_EVENT_SERVER_ERROR:
		note(c, "RA: error del servidor en %s: %s", ev->server_error->api, ev->server_error->error_message);
		say(c, "%s", tr("RA: error del servidor"));
		break;
	case RC_CLIENT_EVENT_DISCONNECTED:
		say(c, "%s", tr("RA: sin conexión; los logros se enviarán después"));
		break;
	case RC_CLIENT_EVENT_RECONNECTED:
		say(c, "%s", tr("RA: conexión recuperada"));
		break;
	case RC_CLIENT_EVENT_RESET:
		note(c, "RA: pide reiniciar (Hardcore)");
		break;
	default:
		break;
	}
}

static void log_cb(const char *message, const rc_client_t *client)
{
	struct ra_core *c = rc_client_get_userdata((rc_client_t *)client);
	note(c, "rc: %s", message);
}

static void login_done(int result, const char *error, rc_client_t *client, void *ud)
{
	struct ra_core *c = rc_client_get_userdata(client);
	bool by_password = ud != NULL;
	c->login_h = NULL;
	if (result == RC_OK) {
		const rc_client_user_t *u = rc_client_get_user_info(client);
		c->login = RA_LOGGED_IN;
		c->new_token = by_password;
		note(c, "RA: sesión iniciada");
		say(c, tr("RA: sesión iniciada como %s"), u && u->display_name ? u->display_name : "?");
	} else if (result == RC_ABORTED) {
		c->login = RA_LOGGED_OUT;
	} else {
		c->login = RA_LOGGED_OUT;
		note(c, "RA: no se pudo iniciar sesión (%d): %s", result, error ? error : "(sin mensaje)");
		say(c, tr("RA: no se pudo iniciar sesión: %s"), error ? error : "?");
	}
}

static void load_done(int result, const char *error, rc_client_t *client, void *ud)
{
	struct ra_core *c = rc_client_get_userdata(client);
	(void)ud;
	if (result == RC_OK) {
		rc_client_user_game_summary_t s;
		rc_client_get_user_game_summary(client, &s);
		const rc_client_game_t *g = rc_client_get_game_info(client);
		c->game = RA_GAME_LOADED;
		note(c, "RA: juego «%s», %u logros (%u conseguidos), %u puntos", g && g->title ? g->title : "?",
		     s.num_core_achievements, s.num_unlocked_achievements, s.points_core);
		if (s.num_core_achievements)
			say(c, tr("RA: %u logros (%u conseguidos)"), s.num_core_achievements, s.num_unlocked_achievements);
		else
			say(c, "%s", tr("RA: este juego no tiene logros"));
		if (c->hardcore)
			say(c, "%s", tr("RA Hardcore: sin cargar estados ni trucos"));
	} else if (result == RC_NO_GAME_LOADED) {
		c->game = RA_GAME_UNKNOWN;
		note(c, "RA: juego no reconocido");
		say(c, "%s", tr("RA: juego no reconocido"));
	} else if (result == RC_ABORTED) {
		c->game = RA_GAME_NONE;
	} else {
		c->game = RA_GAME_FAILED;
		note(c, "RA: no se pudo cargar el juego (%d)", result);
		say(c, tr("RA: no se pudo cargar el juego: %s"), error ? error : "?");
	}
}

/* ---- API ---- */

struct ra_core *ra_core_create(const struct ra_ops *ops)
{
	struct ra_core *c = calloc(1, sizeof *c);
	if (!c)
		return NULL;
	c->ops = *ops;
	c->client = rc_client_create(read_cb, server_call);
	if (!c->client) {
		free(c);
		return NULL;
	}
	rc_client_set_userdata(c->client, c);
	rc_client_set_hardcore_enabled(c->client, 0);  /* rc_client lo trae encendido; aquí es opcional */
	rc_client_set_event_handler(c->client, event_cb);
	rc_client_set_get_time_millisecs_function(c->client, time_cb);
	rc_client_enable_logging(c->client, RC_CLIENT_LOG_LEVEL_INFO, log_cb);
	rc_client_get_user_agent_clause(c->client, c->agent, sizeof c->agent);
	return c;
}

void ra_core_destroy(struct ra_core *c)
{
	if (!c)
		return;
	lock(c);
	rc_client_destroy(c->client);
	c->client = NULL;
	unlock(c);
	free(c);
}

void ra_core_set_host(struct ra_core *c, const char *url)
{
	(void)c;
	rc_api_set_host(url);
	rc_api_set_image_host(url);
}

const char *ra_core_agent_clause(struct ra_core *c)
{
	return c->agent;
}

void ra_core_set_memory(struct ra_core *c, const uint8_t *rdram, uint32_t size)
{
	lock(c);
	c->rdram = rdram;
	c->rdram_size = size;
	unlock(c);
}

void ra_core_set_hardcore(struct ra_core *c, bool on)
{
	lock(c);
	c->hardcore = on;
	rc_client_set_hardcore_enabled(c->client, on ? 1 : 0);
	unlock(c);
}

bool ra_core_hardcore(struct ra_core *c)
{
	return c->hardcore;
}

bool ra_core_hardcore_active(struct ra_core *c)
{
	return c && c->hardcore && c->game == RA_GAME_LOADED;
}

void ra_core_login_password(struct ra_core *c, const char *user, const char *pass)
{
	lock(c);
	c->login = RA_LOGGING_IN;
	c->login_h = rc_client_begin_login_with_password(c->client, user, pass, login_done, c);
	unlock(c);
}

bool ra_core_login_begin(struct ra_core *c, const char *user, char *pass)
{
	size_t ul = user ? strlen(user) : 0, pl = pass ? strlen(pass) : 0;
	note(c, "RA: inicio de sesión: usuario «%s» (%u bytes), contraseña de %u bytes", user ? user : "", (unsigned)ul, (unsigned)pl);
	bool ok = ul > 0 && pl > 0 && c->login != RA_LOGGING_IN;
	if (!ok)
		note(c, "RA: inicio de sesión no válido (%s)", !ul ? "usuario vacío" : !pl ? "contraseña vacía" : "ya hay uno en curso");
	else
		ra_core_login_password(c, user, pass);
	if (pass)
		memset(pass, 0, pl);
	return ok;
}

void ra_core_login_token(struct ra_core *c, const char *user, const char *token)
{
	lock(c);
	c->login = RA_LOGGING_IN;
	c->login_h = rc_client_begin_login_with_token(c->client, user, token, login_done, NULL);
	unlock(c);
}

void ra_core_logout(struct ra_core *c)
{
	lock(c);
	rc_client_logout(c->client);
	c->login = RA_LOGGED_OUT;
	c->game = RA_GAME_NONE;
	unlock(c);
}

void ra_core_login_abort(struct ra_core *c)
{
	/* rc_client no deja iniciar otra sesión mientras la anterior espera su
	 * respuesta (abortarla no libera ese estado): se le entrega un fallo y su
	 * respuesta tardía, si llega, se ignora. */
	lock(c);
	for (unsigned i = 0; i < RA_PENDING_MAX && c->login == RA_LOGGING_IN; i++)
		if (c->pend[i].cb)
			ra_core_complete(c, c->pend[i].id, 0, NULL, 0);
	if (c->login == RA_LOGGING_IN)
		c->login = RA_LOGGED_OUT;
	unlock(c);
}

int ra_core_login_state(struct ra_core *c)
{
	return c->login;
}

bool ra_core_user(struct ra_core *c, char *user, size_t usize, char *token, size_t tsize)
{
	bool ok = false;
	lock(c);
	const rc_client_user_t *u = rc_client_get_user_info(c->client);
	if (u && u->username) {
		if (user)
			copy_trunc(user, usize, u->username);
		if (token)
			copy_trunc(token, tsize, u->token);
		ok = true;
	}
	unlock(c);
	return ok;
}

bool ra_core_take_new_token(struct ra_core *c)
{
	bool v = c->new_token;
	c->new_token = false;
	return v;
}

void ra_core_load_game(struct ra_core *c, const char *hash)
{
	lock(c);
	c->game = RA_GAME_LOADING;
	rc_client_begin_load_game(c->client, hash, load_done, NULL);
	unlock(c);
}

void ra_core_unload_game(struct ra_core *c)
{
	lock(c);
	rc_client_unload_game(c->client);
	c->game = RA_GAME_NONE;
	unlock(c);
}

int ra_core_game_state(struct ra_core *c)
{
	return c->game;
}

bool ra_core_summary(struct ra_core *c, char *title, size_t tsize, unsigned *total, unsigned *unlocked,
		     unsigned *points, unsigned *points_unlocked)
{
	bool ok = false;
	lock(c);
	if (c->game == RA_GAME_LOADED) {
		rc_client_user_game_summary_t s;
		const rc_client_game_t *g = rc_client_get_game_info(c->client);
		rc_client_get_user_game_summary(c->client, &s);
		if (title)
			copy_trunc(title, tsize, g ? g->title : "");
		if (total)
			*total = s.num_core_achievements;
		if (unlocked)
			*unlocked = s.num_unlocked_achievements;
		if (points)
			*points = s.points_core;
		if (points_unlocked)
			*points_unlocked = s.points_unlocked;
		ok = true;
	}
	unlock(c);
	return ok;
}

unsigned ra_core_items(struct ra_core *c, struct ra_item *out, unsigned max)
{
	unsigned n = 0;
	lock(c);
	if (c->game == RA_GAME_LOADED) {
		rc_client_achievement_list_t *l = rc_client_create_achievement_list(
			c->client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
		if (l) {
			for (uint32_t b = 0; b < l->num_buckets; b++) {
				const rc_client_achievement_bucket_t *bk = &l->buckets[b];
				for (uint32_t i = 0; i < bk->num_achievements && n < max; i++) {
					const rc_client_achievement_t *a = bk->achievements[i];
					copy_trunc(out[n].title, sizeof out[n].title, a->title);
					copy_trunc(out[n].desc, sizeof out[n].desc, a->description);
					out[n].points = a->points;
					out[n].unlocked = a->unlocked != 0;
					n++;
				}
			}
			rc_client_destroy_achievement_list(l);
		}
	}
	unlock(c);
	return n;
}

void ra_core_frame(struct ra_core *c)
{
	uint64_t t0 = c->ops.now_us ? c->ops.now_us(c->ops.ctx) : 0;
	if (c->ops.trylock) {
		if (!c->ops.trylock(c->ops.ctx)) {
			c->skipped++;
			return;
		}
	} else {
		lock(c);
	}
	rc_client_do_frame(c->client);
	unlock(c);
	if (c->ops.now_us) {
		uint64_t d = c->ops.now_us(c->ops.ctx) - t0;
		c->cost_us += d;
		if (d > c->worst_us)
			c->worst_us = d;
		c->calls++;
	}
}

void ra_core_idle(struct ra_core *c)
{
	lock(c);
	rc_client_idle(c->client);
	unlock(c);
}

double ra_core_cost(struct ra_core *c, unsigned *calls, double *worst_us)
{
	double avg = c->calls ? (double)c->cost_us / c->calls : 0.0;
	if (calls)
		*calls = c->calls;
	if (worst_us)
		*worst_us = (double)c->worst_us;
	c->cost_us = c->worst_us = 0;
	c->calls = 0;
	return avg;
}

unsigned ra_core_skipped(struct ra_core *c)
{
	unsigned n = c->skipped;
	c->skipped = 0;
	return n;
}
