/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Pruebas de RetroAchievements (src/frontend/ra_core.c): la lectura de la
 * RDRAM como la espera RA, la huella de una ROM sintética en z64, v64 y n64
 * y el flujo completo de rc_client (inicio de sesión, juego, logros, aviso al
 * desbloquear, Hardcore) contra un servidor simulado en el propio proceso:
 * la respuesta se entrega dentro de `send`, con el JSON que manda el servidor
 * (nunca se toca la red ni se usan credenciales reales).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frontend/ra_rules.h"

#include "frontend/ra_core.h"
#include "test.h"

#define ROM_SIZE 4096
/* MD5 de la ROM sintética de rom_make() en z64 (calculado aparte con hashlib). */
#define ROM_MD5 "59890102ff737e12869752e101b83bfa"

static void rom_make(uint8_t *rom)
{
	for (unsigned i = 0; i < ROM_SIZE; i++)
		rom[i] = (uint8_t)(i * 7 + 3);
	rom[0] = 0x80;  /* la cabecera de una ROM z64: 80 37 12 40 */
	rom[1] = 0x37;
	rom[2] = 0x12;
	rom[3] = 0x40;
}

static void mem_swap(void)
{
	/* Una palabra big-endian 0x11223344 en el byte 0 de la RDRAM. */
	uint8_t ram[16] = { 0x11, 0x22, 0x33, 0x44, 0xAA, 0xBB, 0xCC, 0xDD };
	uint8_t b[4];
	CHECK_EQ(ra_mem_read(ram, sizeof ram, 0, b, 1), 1);
	CHECK_EQ(b[0], 0x44);
	CHECK_EQ(ra_mem_read(ram, sizeof ram, 1, b, 1), 1);
	CHECK_EQ(b[0], 0x33);
	/* Leída como palabra little-endian (lo que hace rcheevos), es el valor de la N64. */
	CHECK_EQ(ra_mem_read(ram, sizeof ram, 0, b, 4), 4);
	CHECK_EQ((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24, 0x11223344u);
	/* Media palabra en 0 = la baja del valor de 32 bits. */
	CHECK_EQ(ra_mem_read(ram, sizeof ram, 0, b, 2), 2);
	CHECK_EQ((uint32_t)b[0] | (uint32_t)b[1] << 8, 0x3344u);
	CHECK_EQ(ra_mem_read(ram, sizeof ram, 4, b, 4), 4);
	CHECK_EQ(b[0], 0xDD);
	CHECK_EQ(b[3], 0xAA);
	/* Fuera de rango: nada. */
	CHECK_EQ(ra_mem_read(ram, sizeof ram, 14, b, 4), 0);
	CHECK_EQ(ra_mem_read(ram, sizeof ram, 16, b, 1), 0);
	CHECK_EQ(ra_mem_read(NULL, sizeof ram, 0, b, 1), 0);
}

static void hash_rom(void)
{
	static uint8_t z64[ROM_SIZE], v64[ROM_SIZE], n64[ROM_SIZE];
	char h[33];
	rom_make(z64);
	CHECK(ra_rom_hash(z64, ROM_SIZE, h));
	CHECK(!strcmp(h, ROM_MD5));
	/* v64: bytes pares e impares intercambiados. */
	for (unsigned i = 0; i < ROM_SIZE; i += 2) {
		v64[i] = z64[i + 1];
		v64[i + 1] = z64[i];
	}
	CHECK_EQ(v64[0], 0x37);  /* y 80 en el segundo byte */
	CHECK(ra_rom_hash(v64, ROM_SIZE, h));
	CHECK(!strcmp(h, ROM_MD5));
	/* n64: palabras invertidas. */
	for (unsigned i = 0; i < ROM_SIZE; i += 4)
		for (unsigned k = 0; k < 4; k++)
			n64[i + k] = z64[i + 3 - k];
	CHECK_EQ(n64[0], 0x40);
	CHECK(ra_rom_hash(n64, ROM_SIZE, h));
	CHECK(!strcmp(h, ROM_MD5));
	/* Lo que no es una ROM de N64. */
	z64[0] = 0x00;
	CHECK(!ra_rom_hash(z64, ROM_SIZE, h));
}

/* ---- Servidor simulado ---- */

static struct {
	struct ra_core *core;
	unsigned requests;
	char kinds[32][24];      /* el `r=` de cada petición */
	char last_award[256];    /* post de la última «awardachievement» */
	char notices[16][160];
	unsigned nnotices;
	int game_id;             /* lo que responde «gameid» */
	bool login_ok;
	bool offline;            /* sin respuesta (status 0) */
	bool silent;             /* ni responde: la petición queda colgada */
	bool saw_secret;         /* ¿se escapó la contraseña a una URL o al registro? */
	bool saw_pass_in_post;   /* la petición de inicio de sesión lleva la contraseña completa */
} srv;

static const char GAME_JSON[] =
	"{\"Success\":true,\"GameId\":77,\"Title\":\"Juego Sintetico\",\"ConsoleId\":2,"
	"\"ImageIconUrl\":\"https://media.retroachievements.org/Images/000001.png\",\"RichPresenceGameId\":77,"
	"\"RichPresencePatch\":\"\",\"Sets\":[{\"AchievementSetId\":9,\"GameId\":77,\"Title\":\"Juego Sintetico\","
	"\"Type\":\"core\",\"ImageIconUrl\":\"https://media.retroachievements.org/Images/000001.png\","
	"\"Achievements\":["
	"{\"ID\":101,\"Title\":\"Primero\",\"Description\":\"El byte 0 vale 0x44\",\"Flags\":3,\"Points\":5,"
	"\"MemAddr\":\"0xH0000=68\",\"Author\":\"x\",\"BadgeName\":\"00001\",\"Created\":1,\"Modified\":1,\"Type\":\"\","
	"\"Rarity\":50.0,\"RarityHardcore\":10.0,\"BadgeURL\":\"https://m/1.png\",\"BadgeLockedURL\":\"https://m/1l.png\"},"
	"{\"ID\":102,\"Title\":\"Segundo\",\"Description\":\"La palabra 0 vale 0x11223344\",\"Flags\":3,\"Points\":10,"
	"\"MemAddr\":\"0xX0000=287454020\",\"Author\":\"x\",\"BadgeName\":\"00002\",\"Created\":1,\"Modified\":1,\"Type\":\"\","
	"\"Rarity\":20.0,\"RarityHardcore\":5.0,\"BadgeURL\":\"https://m/2.png\",\"BadgeLockedURL\":\"https://m/2l.png\"}"
	"],\"Leaderboards\":[]}]}";

static void s_notice(void *ctx, const char *text)
{
	(void)ctx;
	if (srv.nnotices < 16)
		snprintf(srv.notices[srv.nnotices++], sizeof srv.notices[0], "%s", text);
}

static void s_log(void *ctx, const char *text)
{
	(void)ctx;
	if (getenv("RA_TEST_LOG"))
		printf("  log: %s\n", text);
	if (strstr(text, "secreto-de-prueba"))
		srv.saw_secret = true;
}

static uint64_t s_now(void *ctx)
{
	(void)ctx;
	static uint64_t t;
	return t += 16667;
}

static const char *field(const char *post, const char *key, char *out, size_t n)
{
	char pat[32];
	snprintf(pat, sizeof pat, "%s=", key);
	const char *p = post;
	while ((p = strstr(p, pat))) {
		if (p == post || p[-1] == '&') {
			p += strlen(pat);
			size_t l = strcspn(p, "&");
			if (l >= n)
				l = n - 1;
			memcpy(out, p, l);
			out[l] = '\0';
			return out;
		}
		p++;
	}
	out[0] = '\0';
	return NULL;
}

static void s_send(void *ctx, unsigned id, const char *url, const char *post, const char *ctype)
{
	(void)ctx;
	(void)ctype;
	char r[24];
	CHECK(url && !strncmp(url, "https://", 8));
	CHECK(post != NULL);
	if (strstr(url, "secreto-de-prueba"))
		srv.saw_secret = true;
	if (strstr(post, "p=secreto-de-prueba"))
		srv.saw_pass_in_post = true;
	field(post, "r", r, sizeof r);
	if (srv.requests < 32)
		snprintf(srv.kinds[srv.requests], sizeof srv.kinds[0], "%s", r);
	srv.requests++;
	if (srv.silent)
		return;
	if (srv.offline) {
		ra_core_complete(srv.core, id, 0, NULL, 0);
		return;
	}
	const char *body = "{\"Success\":false,\"Error\":\"desconocida\"}";
	if (!strcmp(r, "login2")) {
		body = srv.login_ok ? "{\"Success\":true,\"User\":\"Probador\",\"Token\":\"token-falso\",\"Score\":10,"
				      "\"SoftcoreScore\":5,\"Messages\":0}"
				    : "{\"Success\":false,\"Error\":\"Credenciales no validas\",\"Code\":\"invalid_credentials\"}";
	} else if (!strcmp(r, "achievementsets")) {
		body = srv.game_id ? GAME_JSON
				   : "{\"Success\":true,\"GameId\":0,\"Title\":\"\",\"ConsoleId\":0,\"ImageIconUrl\":\"\",\"Sets\":[]}";
	} else if (!strcmp(r, "startsession")) {
		body = "{\"Success\":true,\"Unlocks\":[],\"HardcoreUnlocks\":[],\"ServerNow\":1700000000}";
	} else if (!strcmp(r, "awardachievement")) {
		snprintf(srv.last_award, sizeof srv.last_award, "%s", post);
		body = "{\"Success\":true,\"Score\":15,\"SoftcoreScore\":15,\"AchievementID\":101,\"AchievementsRemaining\":1}";
	} else if (!strcmp(r, "ping")) {
		body = "{\"Success\":true}";
	}
	ra_core_complete(srv.core, id, 200, body, strlen(body));
}

static struct ra_core *srv_start(bool login_ok, int game_id)
{
	memset(&srv, 0, sizeof srv);
	srv.login_ok = login_ok;
	srv.game_id = game_id;
	struct ra_ops ops = { NULL, s_send, NULL, NULL, NULL, s_notice, s_log, s_now };
	srv.core = ra_core_create(&ops);
	CHECK(srv.core != NULL);
	return srv.core;
}

static bool noticed(const char *what)
{
	for (unsigned i = 0; i < srv.nnotices; i++)
		if (strstr(srv.notices[i], what))
			return true;
	return false;
}

static bool asked(const char *kind)
{
	for (unsigned i = 0; i < srv.requests && i < 32; i++)
		if (!strcmp(srv.kinds[i], kind))
			return true;
	return false;
}

static void flow(void)
{
	static uint8_t ram[64];
	struct ra_core *c = srv_start(true, 77);
	ra_core_set_memory(c, ram, sizeof ram);
	CHECK_EQ(ra_core_login_state(c), RA_LOGGED_OUT);

	/* Sesión con contraseña: llega el token, que ra.c guarda; la contraseña no sale a ningún registro. */
	ra_core_login_password(c, "Probador", "secreto-de-prueba");
	CHECK_EQ(ra_core_login_state(c), RA_LOGGED_IN);
	CHECK(noticed("Probador"));
	CHECK(ra_core_take_new_token(c));
	CHECK(!ra_core_take_new_token(c));
	char user[RA_USER_MAX], token[RA_TOKEN_MAX];
	CHECK(ra_core_user(c, user, sizeof user, token, sizeof token));
	CHECK(!strcmp(user, "Probador"));
	CHECK(!strcmp(token, "token-falso"));
	CHECK(!ra_core_hardcore_active(c));

	/* El juego: huella, datos, lista. */
	ra_core_load_game(c, ROM_MD5);
	CHECK_EQ(ra_core_game_state(c), RA_GAME_LOADED);
	CHECK(asked("achievementsets") && asked("startsession"));
	char title[RA_TITLE_MAX];
	unsigned total = 0, unlocked = 9, points = 0, pts_un = 9;
	CHECK(ra_core_summary(c, title, sizeof title, &total, &unlocked, &points, &pts_un));
	CHECK(!strcmp(title, "Juego Sintetico"));
	CHECK_EQ(total, 2);
	CHECK_EQ(unlocked, 0);
	CHECK_EQ(points, 15);
	CHECK(noticed("2 logros (0 conseguidos)"));
	struct ra_item it[8];
	CHECK_EQ(ra_core_items(c, it, 8), 2);
	CHECK(!it[0].unlocked && !it[1].unlocked);

	/* Sin cumplir la condición, nada. */
	for (int i = 0; i < 5; i++)
		ra_core_frame(c);
	CHECK(!asked("awardachievement"));

	/* RDRAM big-endian con el byte 0 de RA = 0x44: es el último byte de la palabra 0x11223344. */
	ram[0] = 0x11, ram[1] = 0x22, ram[2] = 0x33, ram[3] = 0x44;
	for (int i = 0; i < 5; i++)
		ra_core_frame(c);
	CHECK(asked("awardachievement"));
	CHECK(noticed("Primero"));
	CHECK(noticed("Segundo"));
	char hc[8];
	CHECK(field(srv.last_award, "h", hc, sizeof hc) != NULL);
	CHECK(!strcmp(hc, "0"));  /* softcore */
	CHECK_EQ(ra_core_items(c, it, 8), 2);
	CHECK(it[0].unlocked && it[1].unlocked);
	CHECK(ra_core_summary(c, NULL, 0, &total, &unlocked, NULL, NULL));
	CHECK_EQ(unlocked, 2);

	/* El costo por cuadro queda medido. */
	unsigned calls = 0;
	double worst = 0, avg = ra_core_cost(c, &calls, &worst);
	CHECK_EQ(calls, 10);
	CHECK(avg >= 0.0 && worst >= avg);
	CHECK(!srv.saw_secret);

	ra_core_unload_game(c);
	CHECK_EQ(ra_core_game_state(c), RA_GAME_NONE);
	ra_core_logout(c);
	CHECK_EQ(ra_core_login_state(c), RA_LOGGED_OUT);
	ra_core_destroy(c);
}

static void bad_login(void)
{
	struct ra_core *c = srv_start(false, 77);
	ra_core_login_password(c, "Probador", "secreto-de-prueba");
	CHECK_EQ(ra_core_login_state(c), RA_LOGGED_OUT);
	CHECK(noticed("Credenciales no validas"));
	CHECK(!ra_core_take_new_token(c));
	char user[8];
	CHECK(!ra_core_user(c, user, sizeof user, NULL, 0));
	CHECK(!srv.saw_secret);
	ra_core_destroy(c);
}

static void unknown_game(void)
{
	static uint8_t ram[64];
	struct ra_core *c = srv_start(true, 0);
	ra_core_set_memory(c, ram, sizeof ram);
	ra_core_login_token(c, "Probador", "token-falso");
	CHECK_EQ(ra_core_login_state(c), RA_LOGGED_IN);
	CHECK(!ra_core_take_new_token(c));  /* por token no hay token nuevo que guardar */
	ra_core_set_hardcore(c, true);
	ra_core_load_game(c, ROM_MD5);
	CHECK_EQ(ra_core_game_state(c), RA_GAME_UNKNOWN);
	CHECK(noticed("no reconocido"));
	CHECK(!ra_core_hardcore_active(c));  /* sin logros, sin restricciones */
	ra_core_destroy(c);
}

static void hardcore(void)
{
	static uint8_t ram[64];
	struct ra_core *c = srv_start(true, 77);
	ra_core_set_memory(c, ram, sizeof ram);
	ra_core_set_hardcore(c, true);
	CHECK(ra_core_hardcore(c));
	CHECK(!ra_core_hardcore_active(c));  /* todavía sin juego */
	ra_core_login_token(c, "Probador", "token-falso");
	ra_core_load_game(c, ROM_MD5);
	CHECK(ra_core_hardcore_active(c));
	CHECK(noticed("Hardcore"));
	/* Un logro que ya se cumple al cargar espera a dejar de cumplirse (rcheevos): antes, cuadros con la condición falsa. */
	for (int i = 0; i < 5; i++)
		ra_core_frame(c);
	ram[3] = 0x44;
	for (int i = 0; i < 5; i++)
		ra_core_frame(c);
	char hc[8];
	CHECK(field(srv.last_award, "h", hc, sizeof hc) != NULL);
	CHECK(!strcmp(hc, "1"));
	ra_core_unload_game(c);
	CHECK(!ra_core_hardcore_active(c));
	ra_core_destroy(c);
}

static void offline(void)
{
	/* Sin red: el inicio de sesión falla con aviso y no se cuelga nada. */
	struct ra_core *c = srv_start(true, 77);
	srv.offline = true;
	ra_core_login_password(c, "Probador", "secreto-de-prueba");
	CHECK(ra_core_login_state(c) != RA_LOGGED_IN);
	ra_core_destroy(c);
}

static void login_abort(void)
{
	/* Un servidor que no contesta: el inicio de sesión queda «en curso» hasta que
	 * ra.c lo cancela por tiempo, y después se puede volver a intentar. */
	struct ra_core *c = srv_start(true, 77);
	srv.silent = true;
	ra_core_login_password(c, "Probador", "secreto-de-prueba");
	CHECK_EQ(ra_core_login_state(c), RA_LOGGING_IN);
	ra_core_login_abort(c);
	CHECK_EQ(ra_core_login_state(c), RA_LOGGED_OUT);
	srv.silent = false;
	ra_core_login_password(c, "Probador", "secreto-de-prueba");
	CHECK_EQ(ra_core_login_state(c), RA_LOGGED_IN);
	ra_core_destroy(c);
}

/* El flujo de los dos teclados (ra.c): el mismo búfer UTF-16 estático sirve para el
 * usuario y la contraseña, y se borra entre uno y otro. El inicio de sesión recibe
 * los dos textos completos (el servidor simulado ve la contraseña en la petición)
 * y la contraseña queda borrada después. Aquí hubo un error: se borraba la
 * contraseña antes de copiarla y rc_client devolvía RC_INVALID_STATE. */
static void two_keyboards(void)
{
	static uint16_t text[64];
	char user[64], pass[160];
	struct ra_core *c = srv_start(true, 77);
	const char *u = "Probador", *p = "secreto-de-prueba";
	memset(text, 0, sizeof text);
	for (int i = 0; u[i]; i++)
		text[i] = (uint16_t)u[i];
	ra_u16_to_utf8(text, 63, user, sizeof user);  /* 63 = capacidad del teclado, no el largo */
	CHECK(!strcmp(user, "Probador"));
	memset(text, 0, sizeof text);
	for (int i = 0; p[i]; i++)
		text[i] = (uint16_t)p[i];
	ra_u16_to_utf8(text, 63, pass, sizeof pass);
	memset(text, 0, sizeof text);
	CHECK(!strcmp(pass, "secreto-de-prueba"));
	CHECK(ra_core_login_begin(c, user, pass));
	CHECK_EQ(pass[0], 0);
	CHECK_EQ(ra_core_login_state(c), RA_LOGGED_IN);
	CHECK(srv.saw_pass_in_post);
	CHECK(!srv.saw_secret);
	/* Vacíos no se piden. */
	char empty[4] = "";
	CHECK(!ra_core_login_begin(c, user, empty));
	CHECK(!ra_core_login_begin(c, "", pass));
	/* UTF-16 con acentos y fuera de ASCII. */
	uint16_t acc[3] = { 0x00E9, 0x20AC, 0 };
	ra_u16_to_utf8(acc, 63, user, sizeof user);
	CHECK(!strcmp(user, "\xC3\xA9\xE2\x82\xAC"));
	ra_core_destroy(c);
}

/* Reglas de Hardcore (ra_rules.c). */
static void rules_hardcore(void)
{
	/* Con Hardcore: nada de cargar estados, trucos, rebobinar, cámara lenta ni
	 * cuadro a cuadro; guardar, adelanto rápido y gráficos valen. */
	CHECK(!ra_rules_allowed(true, RA_RULE_STATE_LOAD));
	CHECK(!ra_rules_allowed(true, RA_RULE_CHEATS));
	CHECK(!ra_rules_allowed(true, RA_RULE_REWIND));
	CHECK(!ra_rules_allowed(true, RA_RULE_SLOWMO));
	CHECK(!ra_rules_allowed(true, RA_RULE_FRAME_STEP));
	CHECK(!ra_rules_allowed(true, RA_RULE_ACTIONS));  /* lo desconocido, no */
	CHECK(ra_rules_allowed(true, RA_RULE_STATE_SAVE));
	CHECK(ra_rules_allowed(true, RA_RULE_FAST_FORWARD));
	CHECK(ra_rules_allowed(true, RA_RULE_GRAPHICS));
	/* Sin Hardcore todo vale. */
	for (int a = 0; a < RA_RULE_ACTIONS; a++)
		CHECK(ra_rules_allowed(false, (enum ra_rule_action)a));
}

static void rules_mode_change(void)
{
	bool d = true;
	/* Sin juego abierto, el ajuste vale tal cual. */
	CHECK(ra_rules_mode_change(false, false, true, &d));
	CHECK(!d);
	CHECK(!ra_rules_mode_change(false, true, false, &d));
	/* Normal → Hardcore con el juego abierto: no, queda para el siguiente. */
	CHECK(!ra_rules_mode_change(true, false, true, &d));
	CHECK(d);
	/* Hardcore → normal con el juego abierto: al instante. */
	CHECK(!ra_rules_mode_change(true, true, false, &d));
	CHECK(!d);
	/* Sin cambio. */
	CHECK(ra_rules_mode_change(true, true, true, &d));
	CHECK(!ra_rules_mode_change(true, false, false, NULL));
}

void t_suite_ra(void)
{
	RUN(two_keyboards);
	RUN(login_abort);
	RUN(mem_swap);
	RUN(hash_rom);
	RUN(flow);
	RUN(bad_login);
	RUN(unknown_game);
	RUN(hardcore);
	RUN(offline);
	RUN(rules_hardcore);
	RUN(rules_mode_change);
}
