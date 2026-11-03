/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — menú para elegir ROM. Ver menu.h.
 *
 * Todo en un espacio lógico de lw × 720 (lw = 1280 con tele 16:9, 960 con
 * 4:3) que rsxkit estira a la pantalla. Las posiciones se dan como
 * fracciones del alto y del ancho para que la misma disposición sirva a las
 * dos proporciones.
 */
#include "menu.h"

#include "i18n.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <rsxkit/text.h>
#include <rsxkit/ui.h>

#include "core/rom.h"
#include "core/save.h"
#include "frontend/cartucho_mesh.h"
#include "padmap.h"
#include "prompts.h"

#define BIT(b) (1u << (b))
#define DIRS   (BIT(SALSA64_DS3_UP) | BIT(SALSA64_DS3_DOWN) | BIT(SALSA64_DS3_LEFT) | BIT(SALSA64_DS3_RIGHT))
#define TAU    6.28318530718f

/* Colores 0xRRGGBB. */
#define C_TEXT   0xF2F4F8u
#define C_DIM    0xA8B4C4u
#define C_ACCENT 0x9CC8FFu
#define C_WARN   0xFFC070u

/* Columna: fila elegida a COL_Y del alto; la vecina a GAP, las demás cada
 * SPACING (fracciones del alto). */
#define COL_Y   0.42f
#define GAP     0.135f
#define SPACING 0.074f

/* Versión y paquete arriba a la derecha: letra chica y a lo sumo el ancho
 * del panel (fracción del ancho), para no pasar sobre la columna. */
#define VER_SCALE 1.25f
#define VER_ROOM  0.38f

/* ---- Lógica ---------------------------------------------------------------- */

void menu_settings_default(struct menu_settings *s)
{
	s->waves = true;
	s->seda = false;
	s->overscan = MENU_OVERSCAN_NORMAL;
	s->resolution = MENU_RES_720P;
	s->scale = 0;
	s->fps = false;
	s->confirm = true;
	s->aspect = MENU_ASPECT_4_3;
	s->filter = MENU_FILTER_SMOOTH;
	s->density = MENU_DENSITY_NORMAL;
	s->interp = MENU_INTERP_OFF;
	s->rom_order = MENU_ROM_ORDER_TITLE;
	s->accessory = MENU_ACC_PAK;
	s->lang = MENU_LANG_AUTO;
	s->ra = false;
	s->ra_hardcore = false;
}

static const char *overscan_name(int o)
{
	switch (o) {
	case MENU_OVERSCAN_OFF:    return "Apagado";
	case MENU_OVERSCAN_NORMAL: return "Normal";
	case MENU_OVERSCAN_WIDE:   return "Amplio";
	default:                   return "Personalizado";
	}
}

static const char *aspect_name(int a)
{
	return a == MENU_ASPECT_16_9 ? "16:9 (estirada)" : "4:3 (original)";
}

/* Fila «Filtro» (menu.cfg «filtro = …», también por juego): clave, nombre y
 * crédito de cada uno, en el orden de enum menu_filter. Uno nuevo se agrega
 * aquí y en el enum; main.c (load_view) decide cómo se dibuja. */
static const struct {
	const char *key, *name, *credit;
} filter_info[MENU_FILTERS] = {
	[MENU_FILTER_SMOOTH] = { "suave", "Suave", "Filtro de VIDEOGAMES SCZ" },
	[MENU_FILTER_SHARP] = { "nitido", "Nítida", "Filtro de VIDEOGAMES SCZ" },
	[MENU_FILTER_SCAN_SOFT] = { "lineas", "Líneas suaves", "Filtro de VIDEOGAMES SCZ" },
	[MENU_FILTER_SCAN_STRONG] = { "lineas+", "Líneas fuertes", "Filtro de VIDEOGAMES SCZ" },
	/* Los CRT son experimentales: el nombre corto (cabe en la fila de la
	 * pausa) y el largo, con «(experimental)», en la ayuda del panel. */
	[MENU_FILTER_CRT_MASK] = { "crt-mascara", "CRT máscara (exp.)", "Filtro original del autor" },
	[MENU_FILTER_CRT_RGB] = { "crt-rgb", "CRT RGB (exp.)", "Filtro original del autor" },
	[MENU_FILTER_CRT_SVIDEO] = { "crt-svideo", "CRT S-Video (exp.)", "Filtro original del autor" },
	[MENU_FILTER_CRT_COMPOSITE] = { "crt-compuesto", "CRT compuesto (exp.)", "Filtro original del autor" },
	[MENU_FILTER_CRT_TRINITRON] = { "crt-trinitron", "CRT Trinitron (exp.)", "Filtro original del autor" },
};

static const char *filter_name(int f)
{
	return filter_info[f >= 0 && f < MENU_FILTERS ? f : MENU_FILTER_SMOOTH].name;
}

static const char *filter_key(int f)
{
	return filter_info[f >= 0 && f < MENU_FILTERS ? f : MENU_FILTER_SMOOTH].key;
}

const char *menu_filter_name(int f)
{
	return filter_name(f);
}

const char *menu_filter_credit(int f)
{
	return filter_info[f >= 0 && f < MENU_FILTERS ? f : MENU_FILTER_SMOOTH].credit;
}

const char *menu_aspect_name(int a)
{
	return aspect_name(a);
}

const char *menu_overscan_name(int o)
{
	return overscan_name(o);
}
static const char *const lang_key[MENU_LANGS] = { "es", "en", "fr", "de", "it", "pt", "nl", "sv", "da", "no", "fi", "pl",
						  "tr", "ru", "ja", "ko", "zh-hant", "zh-hans", "auto" };
static const char *const density_key[MENU_DENSITIES] = { "normal", "fina" };
static const char *const interp_key[MENU_INTERPS] = { "no", "geometria" };
static const char *const lang_label[MENU_LANGS] = { "Español", "English", "Français", "Deutsch", "Italiano", "Português",
						    "Nederlands", "Svenska", "Dansk", "Norsk", "Suomi", "Polski", "Türkçe", "Русский",
						    "日本語", "한국어", "繁體中文", "简体中文", "Automático" };
_Static_assert(MENU_LANG_AUTO == I18N_AUTO && MENU_LANG_JA == I18N_JA && MENU_LANG_ZH_HANS == I18N_ZH_HANS,
	       "los idiomas del menú son los de la tabla");

/* El nombre del idioma en el selector. Sin la fuente del sistema, el de
 * uno CJK guardado en menu.cfg sale en inglés (el menú lo usa en inglés). */
static const char *lang_name(int l)
{
	static const char *const latin[] = { "Japanese", "Korean", "Chinese (Traditional)", "Chinese (Simplified)" };
	if (l < 0 || l >= MENU_LANGS)
		l = MENU_LANG_AUTO;
	return i18n_is_cjk(l) && !i18n_cjk() ? latin[l - MENU_LANG_JA] : lang_label[l];
}

/* El selector recorre Automático y luego los idiomas en este orden; los CJK
 * solo con la fuente del sistema. */
static int lang_step(int cur, bool back)
{
	static const int order[MENU_LANGS] = { MENU_LANG_AUTO, MENU_LANG_ES, MENU_LANG_EN, MENU_LANG_FR, MENU_LANG_DE,
					       MENU_LANG_IT, MENU_LANG_PT, MENU_LANG_NL, MENU_LANG_SV, MENU_LANG_DA,
					       MENU_LANG_NO, MENU_LANG_FI, MENU_LANG_PL, MENU_LANG_TR, MENU_LANG_RU,
					       MENU_LANG_JA, MENU_LANG_KO, MENU_LANG_ZH_HANT, MENU_LANG_ZH_HANS };
	int at = 0;
	for (int i = 0; i < MENU_LANGS; i++)
		if (order[i] == cur)
			at = i;
	for (int k = 0; k < MENU_LANGS; k++) {
		at = (at + (back ? MENU_LANGS - 1 : 1)) % MENU_LANGS;
		if (!i18n_is_cjk(order[at]) || i18n_cjk())
			break;
	}
	return order[at];
}

static const char *resolution_name(int r)
{
	return r == MENU_RES_1080P ? "1080p" : "720p";
}

bool menu_scale_is_auto(int scale)
{
	return scale == 0 || (scale > MENU_SCALE_AUTO_BASE && scale <= MENU_SCALE_VALUE_MAX);
}

int menu_scale_limit(int scale, int resolution)
{
	if (scale == 0)
		return resolution == MENU_RES_1080P ? 4 : 3;
	if (scale > MENU_SCALE_AUTO_BASE && scale <= MENU_SCALE_VALUE_MAX)
		return scale - MENU_SCALE_AUTO_BASE;
	return scale >= 1 && scale <= MENU_SCALE_MAX ? scale : 1;
}

int menu_scale_auto_value(int limit)
{
	return MENU_SCALE_AUTO_BASE + (limit < 1 ? 1 : limit > MENU_SCALE_MAX ? MENU_SCALE_MAX : limit);
}

/* El 0 antiguo se normaliza a su límite antes de avanzar. */
int menu_scale_step(int scale, int resolution, int direction)
{
	int v = scale == 0 ? menu_scale_auto_value(menu_scale_limit(scale, resolution)) : scale;
	if (v < 1 || v > MENU_SCALE_VALUE_MAX)
		v = 1;
	int limit = menu_scale_limit(v, resolution);
	int slot = 2 * (limit - 1) + (menu_scale_is_auto(v) ? 1 : 0);
	slot = (slot + (direction < 0 ? MENU_SCALE_VALUE_MAX - 1 : 1)) % MENU_SCALE_VALUE_MAX;
	return slot & 1 ? menu_scale_auto_value(slot / 2 + 1) : slot / 2 + 1;
}

/* «Automática (hasta 3×)», «1× nativa», «4×»... */
const char *menu_scale_name(int scale, int resolution, char *buf, size_t n)
{
	if (menu_scale_is_auto(scale))
		snprintf(buf, n, tr("Automática (hasta %d×)"), menu_scale_limit(scale, resolution));
	else if (scale == 1)
		snprintf(buf, n, "%s", tr("1× nativa"));
	else
		snprintf(buf, n, "%d×", scale);
	return buf;
}

static const char *scale_name(const struct menu_settings *s, char *buf, size_t n)
{
	return menu_scale_name(s->scale, s->resolution, buf, n);
}

static int lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c;
}

void menu_settings_parse(struct menu_settings *s, const char *text, size_t len)
{
	size_t i = 0;
	while (i < len) {
		size_t e = i;
		while (e < len && text[e] != '\n')
			e++;
		char line[96];
		size_t n = e - i < sizeof line - 1 ? e - i : sizeof line - 1;
		memcpy(line, text + i, n);
		line[n] = '\0';
		i = e + 1;
		char key[32], val[32];
		if (line[0] == '#' || sscanf(line, " %31[^= ] = %31s", key, val) != 2)
			continue;
		for (char *p = key; *p; p++)
			*p = (char)lower((unsigned char)*p);
		for (char *p = val; *p; p++)
			*p = (char)lower((unsigned char)*p);
		bool yes = !strcmp(val, "si") || !strcmp(val, "sí") || !strcmp(val, "1");
		bool no = !strcmp(val, "no") || !strcmp(val, "0");
		/* «shader» fue un fondo de prueba que ya no existe: queda la seda. */
		if (!strcmp(key, "fondo") && (yes || no || !strcmp(val, "shader") || !strcmp(val, "seda"))) {
			s->waves = !no;
			s->seda = !strcmp(val, "seda") || !strcmp(val, "shader");
		}
		else if (!strcmp(key, "fps") && (yes || no))
			s->fps = yes;
		else if (!strcmp(key, "confirmar") && (yes || no))
			s->confirm = yes;
		else if (!strcmp(key, "retroachievements") && (yes || no))
			s->ra = yes;
		else if (!strcmp(key, "hardcore") && (yes || no))
			s->ra_hardcore = yes;
		else if (!strcmp(key, "aspecto")) {
			if (!strcmp(val, "16:9"))
				s->aspect = MENU_ASPECT_16_9;
			else if (!strcmp(val, "4:3"))
				s->aspect = MENU_ASPECT_4_3;
		} else if (!strcmp(key, "filtro")) {
			for (int f = 0; f < MENU_FILTERS; f++)
				if (!strcmp(val, filter_key(f)))
					s->filter = f;
		} else if (!strcmp(key, "idioma")) {
			for (int l = 0; l < MENU_LANGS; l++)
				if (!strcmp(val, lang_key[l]))
					s->lang = l;
		} else if (!strcmp(key, "densidad")) {
			for (int d = 0; d < MENU_DENSITIES; d++)
				if (!strcmp(val, density_key[d]))
					s->density = d;
		} else if (!strcmp(key, "interpolacion")) {
			for (int v = 0; v < MENU_INTERPS; v++)
				if (!strcmp(val, interp_key[v]))
					s->interp = v;
		} else if (!strcmp(key, "orden")) {
			if (!strcmp(val, "titulo"))
				s->rom_order = MENU_ROM_ORDER_TITLE;
			else if (!strcmp(val, "archivo"))
				s->rom_order = MENU_ROM_ORDER_FILENAME;
		} else if (!strcmp(key, "accesorio")) {
			if (!strcmp(val, "controller-pak"))
				s->accessory = MENU_ACC_PAK;
			else if (!strcmp(val, "rumble-pak"))
				s->accessory = MENU_ACC_RUMBLE;
			else if (!strcmp(val, "transfer-pak"))
				s->accessory = MENU_ACC_TRANSFER;
			else if (!strcmp(val, "ninguno"))
				s->accessory = MENU_ACC_NONE;
		}
	}
}

/* ---- Ajustes de un juego ---- */

void menu_game_default(struct menu_game *g)
{
	g->aspect = g->filter = g->overscan = g->scale = g->density = g->interp = -1;
	g->stable_scale = 0;
	g->accessory = -1;
	salsa64_padmap_inherit(&g->controls);
}

void menu_game_parse(struct menu_game *g, const char *text, size_t len)
{
	size_t i = 0;
	while (i < len) {
		size_t e = i;
		while (e < len && text[e] != '\n')
			e++;
		char line[96];
		size_t n = e - i < sizeof line - 1 ? e - i : sizeof line - 1;
		memcpy(line, text + i, n);
		line[n] = '\0';
		i = e + 1;
		char key[32], val[32];
		if (line[0] == '#' || sscanf(line, " %31[^= ] = %31s", key, val) != 2)
			continue;
		for (char *p = key; *p; p++)
			*p = (char)lower((unsigned char)*p);
		for (char *p = val; *p; p++)
			*p = (char)lower((unsigned char)*p);
		if (!strcmp(key, "aspecto")) {
			g->aspect = !strcmp(val, "16:9") ? MENU_ASPECT_16_9 : !strcmp(val, "4:3") ? MENU_ASPECT_4_3 : -1;
		} else if (!strcmp(key, "filtro")) {
			g->filter = -1;
			for (int f = 0; f < MENU_FILTERS; f++)
				if (!strcmp(val, filter_key(f)))
					g->filter = f;
		} else if (!strcmp(key, "recorte")) {
			g->overscan = !strcmp(val, "apagado") ? MENU_OVERSCAN_OFF
				      : !strcmp(val, "normal") ? MENU_OVERSCAN_NORMAL
				      : !strcmp(val, "amplio") ? MENU_OVERSCAN_WIDE : -1;
		} else if (!strcmp(key, "escala")) {
			/* Hasta '5' por compatibilidad: 5 se lee como el máximo. */
			g->scale = !strcmp(val, "auto") ? 0
				   : !strncmp(val, "auto-", 5) && val[5] >= '1' && val[5] <= '5' && !val[6]
					     ? menu_scale_auto_value(val[5] - '0')
				   : val[0] >= '1' && val[0] <= '5' && !val[1]
					     ? (val[0] - '0' > MENU_SCALE_MAX ? MENU_SCALE_MAX : val[0] - '0') : -1;
		} else if (!strcmp(key, "escala-estable")) {
			g->stable_scale = val[0] >= '1' && val[0] <= '5' && !val[1]
						  ? (val[0] - '0' > MENU_SCALE_MAX ? MENU_SCALE_MAX : val[0] - '0') : 0;
		} else if (!strcmp(key, "densidad")) {
			g->density = -1;
			for (int d = 0; d < MENU_DENSITIES; d++)
				if (!strcmp(val, density_key[d]))
					g->density = d;
		} else if (!strcmp(key, "interpolacion")) {
			g->interp = -1;
			for (int v = 0; v < MENU_INTERPS; v++)
				if (!strcmp(val, interp_key[v]))
					g->interp = v;
		} else if (!strcmp(key, "accesorio")) {
			g->accessory = !strcmp(val, "global") ? -1
					: !strcmp(val, "ninguno") ? MENU_ACC_NONE
					: !strcmp(val, "rumble-pak") ? MENU_ACC_RUMBLE
					: !strcmp(val, "transfer-pak") ? MENU_ACC_TRANSFER : MENU_ACC_PAK;
		}
	}
	salsa64_padmap_parse(&g->controls, text, len, NULL, 0);
}

int menu_game_format(const struct menu_game *g, char *buf, size_t size)
{
	static const char *const crop[] = { "apagado", "normal", "amplio" };
	static const char *const scale[] = { "auto", "1", "2", "3", "4", "auto-1", "auto-2", "auto-3", "auto-4" };
	_Static_assert(sizeof scale / sizeof scale[0] == MENU_SCALE_VALUE_MAX + 1, "una clave por escala");
	int n = snprintf(buf, size,
			"# Ajustes de este juego (global = los de Opciones)\naspecto = %s\nfiltro = %s\nrecorte = %s\nescala = %s\n"
			"escala-estable = %s\ndensidad = %s\ninterpolacion = %s\naccesorio = %s\n",
			g->aspect == MENU_ASPECT_16_9 ? "16:9" : g->aspect == MENU_ASPECT_4_3 ? "4:3" : "global",
			g->filter >= 0 && g->filter < MENU_FILTERS ? filter_key(g->filter) : "global",
			g->overscan >= MENU_OVERSCAN_OFF && g->overscan <= MENU_OVERSCAN_WIDE ? crop[g->overscan] : "global",
			g->scale >= 0 && g->scale <= MENU_SCALE_VALUE_MAX ? scale[g->scale] : "global",
			g->stable_scale >= 1 && g->stable_scale <= MENU_SCALE_MAX ? scale[g->stable_scale] : "ninguna",
			g->density >= 0 && g->density < MENU_DENSITIES ? density_key[g->density] : "global",
			g->interp >= 0 && g->interp < MENU_INTERPS ? interp_key[g->interp] : "global",
			g->accessory < 0 ? "global" : g->accessory == MENU_ACC_NONE ? "ninguno"
				: g->accessory == MENU_ACC_RUMBLE ? "rumble-pak"
				: g->accessory == MENU_ACC_TRANSFER ? "transfer-pak" : "controller-pak");
	if (n < 0 || (size_t)n >= size)
		return -1;
	int k = salsa64_padmap_format(&g->controls, buf + n, size - (size_t)n, 1);
	return k < 0 ? -1 : n + k;
}

bool menu_game_scale_set(struct menu_game *g, int scale)
{
	if (!g || scale < 0 || scale > MENU_SCALE_VALUE_MAX)
		return false;
	g->scale = scale;
	return true;
}

int menu_game_density(const struct menu_settings *s, const struct menu_game *g)
{
	return g->density >= 0 && g->density < MENU_DENSITIES ? g->density : s->density;
}

bool menu_filter_has_density(int filter)
{
	return filter == MENU_FILTER_CRT_MASK;
}

const char *menu_density_name(int density)
{
	return density == MENU_DENSITY_FINE ? "Fina" : "Normal";
}

int menu_game_interp(const struct menu_settings *s, const struct menu_game *g)
{
	int v = g->interp >= 0 ? g->interp : s->interp;
	return v >= 0 && v < MENU_INTERPS ? v : MENU_INTERP_OFF;
}

const char *menu_accessory_name(int accessory)
{
	return tr(accessory == MENU_ACC_NONE ? "Ninguno"
		  : accessory == MENU_ACC_RUMBLE ? "Rumble Pak"
		  : accessory == MENU_ACC_TRANSFER ? "Transfer Pak" : "Controller Pak");
}

const char *menu_interp_name(int interp)
{
	return interp == MENU_INTERP_GEOMETRY ? "Por geometría (exp.)" : "Apagado";
}

void menu_game_effective(const struct menu_settings *s, const struct menu_game *g, int *aspect, int *filter,
			 int *overscan, int *scale)
{
	if (aspect)
		*aspect = g->aspect >= 0 ? g->aspect : s->aspect;
	if (filter)
		*filter = g->filter >= 0 ? g->filter : s->filter;
	if (overscan)
		*overscan = g->overscan >= 0 ? g->overscan : s->overscan;
	if (scale)
		*scale = g->scale >= 0 ? g->scale : s->scale;
}

bool menu_video_parse(struct menu_settings *s, const char *text, size_t len)
{
	bool have_scale = false;
	size_t i = 0;
	while (i < len) {
		size_t e = i;
		while (e < len && text[e] != '\n')
			e++;
		char line[96];
		size_t n = e - i < sizeof line - 1 ? e - i : sizeof line - 1;
		memcpy(line, text + i, n);
		line[n] = '\0';
		i = e + 1;
		char key[32], val[32];
		if (line[0] == '#')
			continue;
		if (sscanf(line, " %31[^= ] = %31s", key, val) != 2) {
			/* Formato viejo: la línea es la resolución. */
			if (!strncmp(line, "1080", 4))
				s->resolution = MENU_RES_1080P;
			else if (!strncmp(line, "720", 3))
				s->resolution = MENU_RES_720P;
			continue;
		}
		for (char *p = key; *p; p++)
			*p = (char)lower((unsigned char)*p);
		for (char *p = val; *p; p++)
			*p = (char)lower((unsigned char)*p);
		if (!strcmp(key, "salida")) {
			if (!strncmp(val, "1080", 4))
				s->resolution = MENU_RES_1080P;
			else if (!strncmp(val, "720", 3))
				s->resolution = MENU_RES_720P;
		} else if (!strcmp(key, "escala")) {
			if (!strcmp(val, "auto")) {
				s->scale = 0;
				have_scale = true;
			} else if (!strncmp(val, "auto-", 5) && val[5] >= '1' && val[5] <= '5' && !val[6]) {
				s->scale = menu_scale_auto_value(val[5] - '0');
				have_scale = true;
			} else if (val[0] >= '1' && val[0] <= '5' && (!val[1] || (val[1] == 'x' && !val[2]))) {
				s->scale = val[0] - '0' > MENU_SCALE_MAX ? MENU_SCALE_MAX : val[0] - '0';
				have_scale = true;
			}
		}
	}
	return have_scale;
}

int menu_video_format(const struct menu_settings *s, char *buf, size_t size)
{
	char scale[8] = "auto";
	if (s->scale >= 1 && s->scale <= MENU_SCALE_MAX)
		snprintf(scale, sizeof scale, "%d", s->scale);
	else if (s->scale > MENU_SCALE_AUTO_BASE && s->scale <= MENU_SCALE_VALUE_MAX)
		snprintf(scale, sizeof scale, "auto-%d", s->scale - MENU_SCALE_AUTO_BASE);
	return snprintf(buf, size,
			"# Video de Salsa64 (se lee al arrancar)\n# escala = 1..4 fija, auto-N automática hasta N; auto conserva el valor anterior\n"
			"salida = %s\nescala = %s\n",
			s->resolution == MENU_RES_1080P ? "1080" : "720", scale);
}

int menu_settings_format(const struct menu_settings *s, char *buf, size_t size)
{
	return snprintf(buf, size,
			"# Ajustes del menú de Salsa64\n# fondo = si|no|seda; aspecto = 4:3|16:9; filtro = suave|nitido|lineas|lineas+|crt-…; "
			"densidad = normal|fina; idioma = auto|es|en|fr|de|it|pt|nl|sv|da|no|fi|pl|tr|ru; interpolacion = no|geometria; orden = titulo|archivo\n"
			"fondo = %s\nfps = %s\nconfirmar = %s\naspecto = %s\nfiltro = %s\ndensidad = %s\nidioma = %s\n"
			"interpolacion = %s\norden = %s\naccesorio = %s\nretroachievements = %s\nhardcore = %s\n",
			s->waves ? (s->seda ? "seda" : "si") : "no", s->fps ? "si" : "no", s->confirm ? "si" : "no",
			s->aspect == MENU_ASPECT_16_9 ? "16:9" : "4:3",
			filter_key(s->filter), density_key[s->density == MENU_DENSITY_FINE],
			lang_key[s->lang >= 0 && s->lang < MENU_LANGS ? s->lang : 0],
			interp_key[s->interp == MENU_INTERP_GEOMETRY],
			s->rom_order == MENU_ROM_ORDER_FILENAME ? "archivo" : "titulo",
			s->accessory == MENU_ACC_NONE ? "ninguno" : s->accessory == MENU_ACC_RUMBLE ? "rumble-pak"
				: s->accessory == MENU_ACC_TRANSFER ? "transfer-pak" : "controller-pak",
			s->ra ? "si" : "no", s->ra_hardcore ? "si" : "no");
}

bool menu_settings_cfg_differs(const struct menu_settings *a, const struct menu_settings *b)
{
	char ta[1024], tb[1024];
	menu_settings_format(a, ta, sizeof ta);
	menu_settings_format(b, tb, sizeof tb);
	return strcmp(ta, tb) != 0;
}

void menu_init(struct menu *m, const struct romlist *list, int sel, const struct menu_settings *set)
{
	memset(m, 0, sizeof *m);
	m->list = list;
	if (set)
		m->set = *set;
	else
		menu_settings_default(&m->set);
	m->sel = sel >= 0 && sel < (int)list->count ? sel : 0;
	m->pos = (float)m->sel;
	m->cover_rom = -1;
	salsa64_padmap_default(&m->global_controls);
	m->controls = m->global_controls;
}

int menu_selected(const struct menu *m)
{
	return m->page == MENU_PAGE_GAMES && m->sel >= 0 && m->sel < (int)m->list->count &&
		       m->list->e[m->sel].kind == ROMLIST_ROM
	       ? m->sel
	       : -1;
}

int menu_find_directory(const struct romlist *list, const char *name)
{
	if (!list || !name)
		return -1;
	for (uint32_t i = 0; i < list->count; i++)
		if (list->e[i].kind == ROMLIST_DIRECTORY && !strcmp(romlist_name(&list->e[i]), name))
			return (int)i;
	return -1;
}

void menu_set_status(struct menu *m, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(m->status, sizeof m->status, tr(fmt), ap);
	va_end(ap);
}

/* Opciones que no siempre ocupan una fila: «Aplicar ahora», solo con algo
 * que aplicar; las descargas, solo encendidas (descargas.cfg, download.h). */
static bool opt_visible(const struct menu *m, int opt)
{
	if (opt == MENU_OPT_APPLY)
		return menu_video_pending(m);
	if (opt == MENU_OPT_DL_COVERS || opt == MENU_OPT_DL_CHEATS)
		return m->dl_enabled;
	if (opt == MENU_OPT_DENSITY)
		return menu_filter_has_density(m->set.filter);
	return true;
}

/* Filas de la página de un juego: «Densidad», solo si el filtro que vale
 * para él es la máscara. */
static bool game_visible(const struct menu *m, int row)
{
	if (row == MENU_GAME_DENSITY)
		return menu_filter_has_density(m->game.filter >= 0 ? m->game.filter : m->set.filter);
	return true;
}

/* Fila mostrada de un ajuste del juego (una oculta da la de la siguiente). */
static int game_row(const struct menu *m, int row)
{
	int n = 0;
	for (int r = 0; r < row; r++)
		n += game_visible(m, r);
	return n;
}

/* Fila mostrada de una opción (una oculta da la de la siguiente); con
 * MENU_OPTS, cuántas filas hay. */
static int opt_row(const struct menu *m, int opt)
{
	int row = 0;
	for (int o = 0; o < opt; o++)
		row += opt_visible(m, o);
	return row;
}

static int opt_from_row(const struct menu *m, int row)
{
	for (int o = 0; o < MENU_OPTS; o++)
		if (opt_visible(m, o) && row-- == 0)
			return o;
	return MENU_OPT_BACK;
}

static void move(struct menu *m, uint32_t dir, bool wrap)
{
	if (m->page == MENU_PAGE_CONTROLS) {
		int v = m->control_sel;
		if (dir == BIT(SALSA64_DS3_UP)) v = v > 0 ? v - 1 : wrap ? MENU_CONTROL_ROWS - 1 : 0;
		else if (dir == BIT(SALSA64_DS3_DOWN)) v = v < MENU_CONTROL_ROWS - 1 ? v + 1 : wrap ? 0 : MENU_CONTROL_ROWS - 1;
		else if (dir == BIT(SALSA64_DS3_LEFT)) v = v - 6 < 0 ? 0 : v - 6;
		else if (dir == BIT(SALSA64_DS3_RIGHT)) v = v + 6 > MENU_CONTROL_ROWS - 1 ? MENU_CONTROL_ROWS - 1 : v + 6;
		m->control_sel = v;
		return;
	}
	if (m->page == MENU_PAGE_CHEATS) {
		int n = menu_cheat_rows(m), v = m->cht_sel;
		if (dir == BIT(SALSA64_DS3_UP))
			v = v > 0 ? v - 1 : wrap ? n - 1 : 0;
		else if (dir == BIT(SALSA64_DS3_DOWN))
			v = v < n - 1 ? v + 1 : wrap ? 0 : n - 1;
		else if (dir == BIT(SALSA64_DS3_LEFT))
			v = v - 6 < 0 ? 0 : v - 6;
		else if (dir == BIT(SALSA64_DS3_RIGHT))
			v = v + 6 > n - 1 ? n - 1 : v + 6;
		m->cht_sel = v;
		return;
	}
	if (m->page == MENU_PAGE_GAME) {
		int v = m->game_sel;
		for (int k = 0; k < MENU_GAME_ROWS; k++) {  /* las filas ocultas se saltan */
			if (dir == BIT(SALSA64_DS3_UP))
				v = v > 0 ? v - 1 : wrap ? MENU_GAME_ROWS - 1 : 0;
			else if (dir == BIT(SALSA64_DS3_DOWN))
				v = v < MENU_GAME_ROWS - 1 ? v + 1 : wrap ? 0 : MENU_GAME_ROWS - 1;
			if (game_visible(m, v))
				break;
		}
		m->game_sel = game_visible(m, v) ? v : m->game_sel;
		return;
	}
	bool games = m->page == MENU_PAGE_GAMES;
	int n = games ? menu_rows(m) : opt_row(m, MENU_OPTS);
	int opt_v = opt_row(m, m->opt), *s = games ? &m->sel : &opt_v;
	int page = games ? 6 : 1, v = *s;
	if (dir == BIT(SALSA64_DS3_UP))
		v = v > 0 ? v - 1 : wrap ? n - 1 : 0;
	else if (dir == BIT(SALSA64_DS3_DOWN))
		v = v < n - 1 ? v + 1 : wrap ? 0 : n - 1;
	else if (dir == BIT(SALSA64_DS3_LEFT))
		v = v - page < 0 ? 0 : v - page;
	else if (dir == BIT(SALSA64_DS3_RIGHT))
		v = v + page > n - 1 ? n - 1 : v + page;
	*s = v;
	if (!games)
		m->opt = opt_from_row(m, v);
}

/* Botones de la página de opciones que no son mover la selección. */
static enum menu_action options_buttons(struct menu *m, uint32_t pressed)
{
	uint32_t change = BIT(SALSA64_DS3_CROSS) | BIT(SALSA64_DS3_LEFT) | BIT(SALSA64_DS3_RIGHT);
	if (m->opt == MENU_OPT_APPLY && !menu_video_pending(m))
		m->opt = MENU_OPT_BACK;  /* ya se aplicó */
	if ((pressed & (BIT(SALSA64_DS3_CIRCLE) | BIT(SALSA64_DS3_TRIANGLE))) ||
	    (m->opt == MENU_OPT_BACK && (pressed & BIT(SALSA64_DS3_CROSS)))) {
		m->page = MENU_PAGE_GAMES;
		return MENU_NONE;
	}
	if (m->opt == MENU_OPT_WAVES && (pressed & change)) {
		/* Ondas → Seda → No → Ondas (izquierda, al revés). */
		int b = !m->set.waves ? 2 : m->set.seda ? 1 : 0;
		b = (pressed & BIT(SALSA64_DS3_LEFT)) ? (b + 2) % 3 : (b + 1) % 3;
		m->set.waves = b != 2;
		m->set.seda = b == 1;
		return MENU_SETTINGS;
	}
	if (m->opt == MENU_OPT_OVERSCAN && (pressed & change)) {
		/* Apagado → Normal → Amplio → Apagado (izquierda, al revés); un
		 * recorte personalizado pasa a Normal. */
		int o = m->set.overscan;
		if (o == MENU_OVERSCAN_CUSTOM)
			o = MENU_OVERSCAN_NORMAL;
		else if (pressed & BIT(SALSA64_DS3_LEFT))
			o = o == MENU_OVERSCAN_OFF ? MENU_OVERSCAN_WIDE : o - 1;
		else
			o = o == MENU_OVERSCAN_WIDE ? MENU_OVERSCAN_OFF : o + 1;
		m->set.overscan = o;
		return MENU_SETTINGS;
	}
	if (m->opt == MENU_OPT_ASPECT && (pressed & change)) {
		m->set.aspect = m->set.aspect == MENU_ASPECT_16_9 ? MENU_ASPECT_4_3 : MENU_ASPECT_16_9;
		return MENU_SETTINGS;
	}
	if (m->opt == MENU_OPT_FILTER && (pressed & change)) {
		m->set.filter = (m->set.filter + (pressed & BIT(SALSA64_DS3_LEFT) ? MENU_FILTERS - 1 : 1)) % MENU_FILTERS;
		return MENU_SETTINGS;
	}
	if (m->opt == MENU_OPT_DENSITY && (pressed & change)) {
		m->set.density = m->set.density == MENU_DENSITY_FINE ? MENU_DENSITY_NORMAL : MENU_DENSITY_FINE;
		return MENU_SETTINGS;
	}
	if (m->opt == MENU_OPT_INTERP && (pressed & change)) {
		m->set.interp = m->set.interp == MENU_INTERP_GEOMETRY ? MENU_INTERP_OFF : MENU_INTERP_GEOMETRY;
		return MENU_SETTINGS;
	}
	if (m->opt == MENU_OPT_LANG && (pressed & change)) {
		m->set.lang = lang_step(m->set.lang, pressed & BIT(SALSA64_DS3_LEFT));
		return MENU_SETTINGS;
	}
	if ((m->opt == MENU_OPT_DL_COVERS || m->opt == MENU_OPT_DL_CHEATS) && (pressed & BIT(SALSA64_DS3_CROSS))) {
		m->dl_what = m->opt == MENU_OPT_DL_CHEATS;
		return MENU_DOWNLOAD;
	}
	if (m->opt == MENU_OPT_REFRESH && (pressed & BIT(SALSA64_DS3_CROSS)))
		return MENU_REFRESH;
	if (m->opt == MENU_OPT_CONTROLS && (pressed & BIT(SALSA64_DS3_CROSS)))
		return MENU_CONTROLS;
	if (m->opt == MENU_OPT_RESOLUTION && (pressed & change)) {
		m->set.resolution = m->set.resolution == MENU_RES_1080P ? MENU_RES_720P : MENU_RES_1080P;
		return MENU_SETTINGS;
	}
	if (m->opt == MENU_OPT_APPLY && (pressed & BIT(SALSA64_DS3_CROSS)))
		return MENU_APPLY;
	if (m->opt == MENU_OPT_SCALE && (pressed & change)) {
		m->set.scale = menu_scale_step(m->set.scale, m->set.resolution,
					       pressed & BIT(SALSA64_DS3_LEFT) ? -1 : 1);
		return MENU_SETTINGS;
	}
	if (m->opt == MENU_OPT_FPS && (pressed & change)) {
		m->set.fps = !m->set.fps;
		return MENU_SETTINGS;
	}
	if (m->opt == MENU_OPT_CONFIRM && (pressed & change)) {
		m->set.confirm = !m->set.confirm;
		return MENU_SETTINGS;
	}
	if (m->opt == MENU_OPT_ACCESSORY && (pressed & change)) {
		int delta = pressed & BIT(SALSA64_DS3_LEFT) ? -1 : 1;
		m->set.accessory = (m->set.accessory + delta + MENU_ACCS) % MENU_ACCS;
		return MENU_SETTINGS;
	}
	if (m->opt == MENU_OPT_RA && (pressed & change)) {
		m->set.ra = !m->set.ra;
		return MENU_SETTINGS;
	}
	if (m->opt == MENU_OPT_RA_HARDCORE && (pressed & change)) {
		m->set.ra_hardcore = !m->set.ra_hardcore;
		return MENU_SETTINGS;
	}
	if (m->opt == MENU_OPT_RA_ACCOUNT && (pressed & BIT(SALSA64_DS3_CROSS)) && !m->ra_busy)
		return m->ra_user ? MENU_RA_LOGOUT : MENU_RA_LOGIN;
	if (m->opt == MENU_OPT_ROM_ORDER && (pressed & change)) {
		m->set.rom_order = m->set.rom_order == MENU_ROM_ORDER_TITLE ? MENU_ROM_ORDER_FILENAME : MENU_ROM_ORDER_TITLE;
		return MENU_SETTINGS;
	}
	return MENU_NONE;
}

void menu_cheats_open(struct menu *m, struct cheat_set *cs, const char *title)
{
	m->cheats = cs;
	m->cht_title = title;
	m->cht_sel = 0;
	m->cht_pos = 0.0f;
	m->page = MENU_PAGE_CHEATS;
	m->rep_button = 0;
	m->status[0] = '\0';
}

void menu_game_open(struct menu *m, const struct menu_game *g, const char *title)
{
	m->game = *g;
	m->game_title = title;
	m->game_sel = 0;
	m->game_pos = 0.0f;
	m->page = MENU_PAGE_GAME;
	m->rep_button = 0;
	m->status[0] = '\0';
}

void menu_controls_open(struct menu *m, bool for_game)
{
	if (m->page != MENU_PAGE_CONTROLS)
		m->controls_return_page = m->page;
	m->control_for_game = for_game;
	m->control_sel = 0;
	m->control_pos = 0.0f;
	m->control_capture = false;
	m->controls = m->global_controls;
	if (for_game)
		salsa64_padmap_overlay(&m->controls, &m->game.controls);
	m->page = MENU_PAGE_CONTROLS;
	m->rep_button = 0;
	m->status[0] = '\0';
}

/* Asigna a la fila elegida los botones `a` y `b` del DS3 (b puede ser NONE). */
static void control_assign(struct menu *m, uint8_t a, uint8_t b)
{
	m->controls.source[m->control_sel] = a;
	m->controls.source2[m->control_sel] = b;
	if (m->control_for_game) {
		m->game.controls.source[m->control_sel] = a;
		m->game.controls.source2[m->control_sel] = b;
	} else m->global_controls = m->controls;
	m->control_capture = false;
	m->status[0] = '\0';
}

/* En la captura se juntan los botones que se pulsan y se asignan al soltar
 * todos: así L2 + R2 juntos quedan los dos para un botón de la N64. START
 * cancela y SELECT deja la fila sin asignar (antes de pulsar otro). */
static enum menu_action controls_buttons(struct menu *m, uint32_t pressed, uint32_t held)
{
	if (m->control_capture) {
		if (!m->capture_mask && (pressed & BIT(SALSA64_DS3_START))) {
			m->control_capture = false;
			m->status[0] = '\0';
			return MENU_NONE;
		}
		if (!m->capture_mask && (pressed & BIT(SALSA64_DS3_SELECT))) {
			control_assign(m, SALSA64_DS3_NONE, SALSA64_DS3_NONE);
			return MENU_CONTROLS_CHANGED;
		}
		m->capture_mask |= pressed & ((1u << SALSA64_DS3_NBUTTONS) - 1) &
				   ~(BIT(SALSA64_DS3_START) | BIT(SALSA64_DS3_SELECT));
		if (!m->capture_mask || (held & m->capture_mask))
			return MENU_NONE;
		uint8_t a = SALSA64_DS3_NONE, b = SALSA64_DS3_NONE;
		for (unsigned k = 0; k < SALSA64_DS3_NBUTTONS; k++)
			if (m->capture_mask & BIT(k)) {
				if (a == SALSA64_DS3_NONE) a = (uint8_t)k;
				else if (b == SALSA64_DS3_NONE) b = (uint8_t)k;
			}
		m->capture_mask = 0;
		control_assign(m, a, b);
		return MENU_CONTROLS_CHANGED;
	}
	if (pressed & (BIT(SALSA64_DS3_CIRCLE) | BIT(SALSA64_DS3_TRIANGLE))) {
		m->page = m->controls_return_page;
		return MENU_NONE;
	}
	if (!(pressed & BIT(SALSA64_DS3_CROSS)))
		return MENU_NONE;
	if (m->control_sel < SALSA64_IN_COUNT) {
		m->control_capture = true;
		m->capture_mask = 0;
		menu_set_status(m, "Pulsa uno o dos botones");
	} else if (m->control_sel == MENU_CONTROL_STICK) {
		m->controls.stick_source = m->controls.stick_source == SALSA64_STICK_RIGHT ? SALSA64_STICK_LEFT : SALSA64_STICK_RIGHT;
		if (m->control_for_game) m->game.controls.stick_source = m->controls.stick_source;
		else m->global_controls = m->controls;
		return MENU_CONTROLS_CHANGED;
	} else if (m->control_sel == MENU_CONTROL_RESET) {
		if (m->control_for_game) {
			salsa64_padmap_inherit(&m->game.controls);
			m->controls = m->global_controls;
		} else {
			salsa64_padmap_default(&m->global_controls);
			m->controls = m->global_controls;
		}
		return MENU_CONTROLS_CHANGED;
	} else if (m->control_sel == MENU_CONTROL_BACK) {
		m->page = m->controls_return_page;
	}
	return MENU_NONE;
}

/* Botones de la página de ajustes del juego que no son mover la selección. */
static enum menu_action game_buttons(struct menu *m, uint32_t pressed)
{
	uint32_t change = BIT(SALSA64_DS3_CROSS) | BIT(SALSA64_DS3_LEFT) | BIT(SALSA64_DS3_RIGHT);
	if (pressed & (BIT(SALSA64_DS3_CIRCLE) | BIT(SALSA64_DS3_TRIANGLE) | BIT(SALSA64_DS3_SELECT))) {
		m->page = MENU_PAGE_GAMES;
		return MENU_NONE;
	}
	if (!(pressed & change))
		return MENU_NONE;
	if (m->game_sel == MENU_GAME_BACK) {
		if (pressed & BIT(SALSA64_DS3_CROSS))
			m->page = MENU_PAGE_GAMES;
		return MENU_NONE;
	}
	if (m->game_sel == MENU_GAME_CONTROLS)
		return pressed & BIT(SALSA64_DS3_CROSS) ? MENU_CONTROLS : MENU_NONE;
	if (m->game_sel == MENU_GAME_ACCESSORY) {  /* Global → accesorio explícito → … → Global */
		int delta = pressed & BIT(SALSA64_DS3_LEFT) ? -1 : 1;
		int slot = m->game.accessory + 1;  /* 0 = Global; 1..MENU_ACCS = accesorio */
		slot = (slot + delta + MENU_ACCS + 1) % (MENU_ACCS + 1);
		m->game.accessory = slot - 1;
		return MENU_GAME_CHANGED;
	}
	/* Global → valor 0 → ... → último → Global (izquierda, al revés). */
	int *v = m->game_sel == MENU_GAME_ASPECT ? &m->game.aspect
		 : m->game_sel == MENU_GAME_FILTER ? &m->game.filter
		 : m->game_sel == MENU_GAME_DENSITY ? &m->game.density
		 : m->game_sel == MENU_GAME_SCALE ? &m->game.scale
		 : m->game_sel == MENU_GAME_INTERP ? &m->game.interp : &m->game.overscan;
	if (m->game_sel == MENU_GAME_SCALE) {
		if (*v < 0)
			*v = pressed & BIT(SALSA64_DS3_LEFT) ? MENU_SCALE_VALUE_MAX : 1;
		else {
			int normalized = *v == 0 ? menu_scale_auto_value(menu_scale_limit(*v, m->set.resolution)) : *v;
			if ((pressed & BIT(SALSA64_DS3_LEFT)) && normalized == 1)
				*v = -1;
			else if (!(pressed & BIT(SALSA64_DS3_LEFT)) && normalized == MENU_SCALE_VALUE_MAX)
				*v = -1;
			else
				*v = menu_scale_step(normalized, m->set.resolution,
						     pressed & BIT(SALSA64_DS3_LEFT) ? -1 : 1);
		}
		return MENU_GAME_CHANGED;
	}
	int count = m->game_sel == MENU_GAME_ASPECT ? 2
		    : m->game_sel == MENU_GAME_FILTER ? MENU_FILTERS
		    : m->game_sel == MENU_GAME_DENSITY ? MENU_DENSITIES
		    : m->game_sel == MENU_GAME_INTERP ? MENU_INTERPS : 3;
	int slot = *v + 1;  /* 0 = global */
	slot = (slot + (pressed & BIT(SALSA64_DS3_LEFT) ? count : 1)) % (count + 1);
	*v = slot - 1;
	return MENU_GAME_CHANGED;
}

/* Botones de la página de trucos que no son mover la selección. */
static enum menu_action cheats_buttons(struct menu *m, uint32_t pressed)
{
	int n = m->cheats ? (int)m->cheats->n : 0;
	if (pressed & (BIT(SALSA64_DS3_CIRCLE) | BIT(SALSA64_DS3_SQUARE) | BIT(SALSA64_DS3_TRIANGLE))) {
		m->page = MENU_PAGE_GAMES;
		return MENU_NONE;
	}
	if (pressed & BIT(SALSA64_DS3_CROSS)) {
		if (m->cht_sel >= n) {
			m->page = MENU_PAGE_GAMES;  /* «Volver» */
			return MENU_NONE;
		}
		m->cheats->c[m->cht_sel].on = !m->cheats->c[m->cht_sel].on;
		return MENU_CHEATS_CHANGED;
	}
	return MENU_NONE;
}

static void open_options(struct menu *m)
{
	m->page = MENU_PAGE_OPTIONS;
	m->opt = MENU_OPT_WAVES;
	m->game_rom = menu_selected(m);
	m->opt_pos = (float)opt_row(m, m->opt);
	m->rep_button = 0;
}

enum menu_action menu_input(struct menu *m, uint32_t buttons, uint64_t now_us)
{
	const uint32_t updown = BIT(SALSA64_DS3_UP) | BIT(SALSA64_DS3_DOWN);
	uint32_t pressed = buttons & ~m->held;
	m->held = buttons;
	if (!m->armed) {
		m->armed = buttons == 0;
		return MENU_NONE;
	}
	if (m->loading)
		return MENU_NONE;
	if (m->quit_ask) {
		/* «¿Salir al XMB?»: × sale, ○ vuelve a la lista donde estaba. */
		if (pressed & (BIT(SALSA64_DS3_CROSS) | BIT(SALSA64_DS3_CIRCLE))) {
			m->quit_ask = false;
			if (pressed & BIT(SALSA64_DS3_CROSS))
				return MENU_QUIT;
		}
		m->rep_button = 0;
		return MENU_NONE;
	}
	if (m->page == MENU_PAGE_GAMES && (pressed & BIT(SALSA64_DS3_L3)))
		return MENU_REFRESH;
	if (m->page == MENU_PAGE_OPTIONS) {
		/* Arriba y abajo mueven; lo demás cambia el valor o vuelve. */
		enum menu_action a = options_buttons(m, pressed & ~updown);
		if (a != MENU_NONE || m->page != MENU_PAGE_OPTIONS) {
			m->rep_button = 0;
			return a;
		}
		pressed &= updown;
		buttons &= updown;
	} else if (m->page == MENU_PAGE_GAME) {
		/* Arriba y abajo mueven; × y ←→ cambian el ajuste, ○ vuelve. */
		enum menu_action a = game_buttons(m, pressed & ~updown);
		if (a != MENU_NONE || m->page != MENU_PAGE_GAME) {
			m->rep_button = 0;
			return a;
		}
		pressed &= updown;
		buttons &= updown;
	} else if (m->page == MENU_PAGE_CONTROLS) {
		uint32_t control_pressed = m->control_capture ? pressed : pressed & ~updown;
		enum menu_action a = controls_buttons(m, control_pressed, buttons);
		if (a != MENU_NONE || m->page != MENU_PAGE_CONTROLS || m->control_capture) {
			m->rep_button = 0;
			return a;
		}
		pressed &= updown;
		buttons &= updown;
	} else if (m->page == MENU_PAGE_CHEATS) {
		/* Arriba y abajo mueven; × cambia el truco, ○ vuelve. */
		enum menu_action a = cheats_buttons(m, pressed & ~updown);
		if (a != MENU_NONE || m->page != MENU_PAGE_CHEATS) {
			m->rep_button = 0;
			return a;
		}
		pressed &= updown;
		buttons &= updown;
	} else if (pressed & BIT(SALSA64_DS3_SELECT)) {
		m->status[0] = '\0';
		if (menu_selected(m) >= 0) {
			m->game_rom = m->sel;
			return MENU_GAME;
		}
	} else if (pressed & BIT(SALSA64_DS3_SQUARE)) {
		m->status[0] = '\0';
		if (menu_selected(m) >= 0)
			return MENU_CHEATS;
	} else if (pressed & BIT(SALSA64_DS3_CROSS)) {
		m->status[0] = '\0';
		if (menu_selected(m) >= 0)
			return MENU_OPEN;
		if (m->sel < (int)m->list->count)
			return MENU_DIRECTORY;
		if (m->sel == menu_row_exit(m))
			return MENU_QUIT;
		open_options(m);
		return MENU_NONE;
	} else if (pressed & BIT(SALSA64_DS3_TRIANGLE)) {
		open_options(m);
		return MENU_NONE;
	} else if (pressed & BIT(SALSA64_DS3_CIRCLE)) {
		if (m->list->directory[0]) {
			for (uint32_t i = 0; i < m->list->count; i++)
				if (m->list->e[i].kind == ROMLIST_PARENT) {
					m->sel = (int)i;
					m->pos = (float)m->sel;
					return MENU_DIRECTORY;
				}
		}
		/* En la raíz, salir sin bajar hasta la última fila. */
		m->status[0] = '\0';
		m->quit_ask = true;
		m->rep_button = 0;
		return MENU_NONE;
	}

	/* Una sola dirección a la vez: la recién pulsada manda. */
	uint32_t dir = pressed & DIRS;
	if (dir) {
		dir &= ~dir + 1;
		move(m, dir, true);
		m->rep_button = dir;
		m->rep_since = now_us;
		m->rep_next = now_us + MENU_REPEAT_DELAY_US;
	} else if (m->rep_button && (buttons & m->rep_button)) {
		if (now_us >= m->rep_next) {
			move(m, m->rep_button, false);
			m->rep_next = now_us + (now_us - m->rep_since >= MENU_REPEAT_FAST_AFTER_US ? MENU_REPEAT_FAST_US
												   : MENU_REPEAT_US);
		}
	} else {
		m->rep_button = 0;
	}
	return MENU_NONE;
}

uint32_t menu_autopilot(const struct menu *m, int target, uint32_t step)
{
	if (step & 1)
		return 0;
	if (m->page != MENU_PAGE_GAMES)
		return BIT(SALSA64_DS3_CIRCLE);
	if (m->sel < target)
		return BIT(SALSA64_DS3_DOWN);
	if (m->sel > target)
		return BIT(SALSA64_DS3_UP);
	return BIT(SALSA64_DS3_CROSS);
}

/* ---- Dibujo ------------------------------------------------------------------ */

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static float smooth(float v)
{
	v = clampf(v, 0.0f, 1.0f);
	return v * v * (3.0f - 2.0f * v);
}

static uint32_t rgba(uint32_t rgb, float a)
{
	return rgb << 8 | (uint32_t)(clampf(a, 0.0f, 1.0f) * 255.0f + 0.5f);
}

static uint32_t mix(uint32_t a, uint32_t b, float t)
{
	uint32_t out = 0;
	for (int s = 0; s < 24; s += 8) {
		float x = (float)(a >> s & 0xFF), y = (float)(b >> s & 0xFF);
		out |= (uint32_t)(x + (y - x) * t + 0.5f) << s;
	}
	return out;
}

/* Sigue a `target` con un suavizado exponencial (rate por segundo). */
static void ease(float *x, float target, float dt, float rate)
{
	*x += (target - *x) * (1.0f - expf(-rate * dt));
	if (fabsf(target - *x) < 0.001f)
		*x = target;
}

/* Ancho de un texto (traducido) con la fuente en uso. */
static float cells_w(const char *s, float scale)
{
	return rsxkit_ui_text_width(tr(s), scale);
}

/* Texto con sombra; si mide más de `max` (unidades lógicas; 0 = sin
 * límite), cortado con «…». */
static void stext(float x, float y, const char *s, float scale, uint32_t rgb, float a, float max)
{
	if (a <= 0.004f)
		return;
	s = tr(s);
	float off = scale >= 2.0f ? 2.0f : 1.5f;
	rsxkit_ui_text_fit(x + off, y + off, s, scale, rgba(0x000000u, a * 0.55f), max);
	rsxkit_ui_text_fit(x, y, s, scale, rgba(rgb, a), max);
}

/* Como stext, con íconos de botones en lugar de los símbolos (prompts.h). */
static void ptext(float x, float y, const char *s, float scale, uint32_t rgb, float a, float max)
{
	if (a <= 0.004f)
		return;
	s = tr(s);
	float off = scale >= 2.0f ? 2.0f : 1.5f;
	prompt_draw(x + off, y + off, s, scale, rgba(0x000000u, a * 0.55f), true, max);
	prompt_draw(x, y, s, scale, rgba(rgb, a), false, max);
}

static uint32_t hsv(float h, float s, float v)
{
	float c = v * s, hp = fmodf(h, 360.0f) / 60.0f, x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f)), m = v - c;
	float r = 0, g = 0, b = 0;
	switch ((int)hp) {
	case 0: r = c, g = x; break;
	case 1: r = x, g = c; break;
	case 2: g = c, b = x; break;
	case 3: g = x, b = c; break;
	case 4: r = x, b = c; break;
	default: r = c, b = x; break;
	}
	return (uint32_t)((r + m) * 255.0f) << 16 | (uint32_t)((g + m) * 255.0f) << 8 | (uint32_t)((b + m) * 255.0f);
}

/* Color de la etiqueta del cartucho: sale del título (siempre el mismo). */
static uint32_t label_color(const char *title)
{
	uint32_t h = 2166136261u;
	for (const char *p = title; *p; p++)
		h = (h ^ (uint8_t)*p) * 16777619u;
	return hsv((float)(h % 360u), 0.55f, 0.88f);
}

/* Nombre del fondo elegido. */
static const char *background_name(const struct menu_settings *s)
{
	return !s->waves ? "No" : s->seda ? "Seda" : "Ondas";
}

static void background(const struct menu *m, float W, float H)
{
	/* El fondo «Seda» lo dibuja main.c en el RSX antes del menú; si no
	 * pudo, van las ondas. */
	if (m->bg_drawn)
		return;
	/* Degradado que cambia despacio (un ciclo de 90 s) si el fondo está
	 * animado; fijo si no. */
	float s = m->set.waves ? 0.5f + 0.5f * sinf(m->t * TAU / 90.0f) : 0.0f;
	uint32_t top = mix(0x123058u, 0x0C3C50u, s), bottom = mix(0x2C1048u, 0x16224Eu, s);
	rsxkit_ui_gradient(0, 0, W, H, rgba(mix(top, 0x2A4C78u, 0.35f), 1.0f), rgba(top, 1.0f), rgba(bottom, 1.0f),
			   rgba(mix(bottom, 0x000000u, 0.35f), 1.0f));
	if (!m->set.waves)
		return;
	static const struct {
		float y, depth, a0, a1, f0, f1, s0, s1, phase, line;
		uint32_t crest, sheet;
	} waves[] = {
		{ 0.56f, 0.36f, 0.050f, 0.010f, 0.55f, 3.20f, 0.018f, 0.075f, 0.70f, 0.0f, 0x00000000u, 0xFFFFFF12u },
		{ 0.60f, 0.30f, 0.040f, 0.018f, 0.85f, 2.10f, 0.030f, -0.055f, 0.00f, 2.5f, 0xFFFFFF6Cu, 0xFFFFFF24u },
		{ 0.66f, 0.24f, 0.030f, 0.014f, 1.25f, 2.70f, -0.040f, 0.050f, 0.35f, 1.6f, 0xFFFFFF4Cu, 0xFFFFFF1Au },
	};
	for (unsigned i = 0; i < sizeof waves / sizeof waves[0]; i++) {
		rsxkit_wave w = {
			waves[i].y * H, waves[i].depth * H, { waves[i].a0 * H, waves[i].a1 * H }, { waves[i].f0, waves[i].f1 },
			{ waves[i].s0, waves[i].s1 }, waves[i].phase, waves[i].line, waves[i].crest, waves[i].sheet,
		};
		rsxkit_ui_wave(&w, m->t);
	}
}

/* Íconos, dibujados aquí con rectángulos y glifos de la fuente. */
static void icon_body(float x, float y, float s, float a)
{
	rsxkit_ui_gradient(x, y, s, s, rgba(0x404A5Cu, a), rgba(0x353E4Eu, a), rgba(0x262C38u, a), rgba(0x1F242Eu, a));
	rsxkit_ui_rect(x, y, s, s * 0.035f + 0.5f, rgba(0xFFFFFFu, a * 0.22f));
}

/* El cartucho: el render del modelo (ranura MENU_CART_SLOT, 256 × 128: el
 * cartucho entero con su sticker a la izquierda), un poco más grande que los
 * otros íconos; sin él, el plano. */
/* El sticker del sprite de frente, como fracción del cuadrado de 128 × 128. */
#define STICKER_X0 0.230f
#define STICKER_X1 0.733f
#define STICKER_Y0 0.219f
#define STICKER_Y1 0.785f

static void icon_cart(const struct menu *m, const struct rom_entry *rom, float dist, float x, float y, float s, float a)
{
	if (m->cart_icon) {
		float c = s * 1.3f, cx = x + (s - c) * 0.5f, cy = y + (s - c) * 0.5f;
		rsxkit_ui_image_part(MENU_CART_SLOT, cx, cy, c, c, 0.0f, 0.0f, 128.0f, 128.0f, rgba(0xFFFFFFu, a));
		float t[4];
		/* La carátula, en el sticker (con las esquinas redondeadas de la miniatura). */
		if (m->thumb && rom && m->thumb(m->thumb_user, rom, dist, t))
			rsxkit_ui_image_part(MENU_THUMBS_SLOT, cx + STICKER_X0 * c, cy + STICKER_Y0 * c,
					     (STICKER_X1 - STICKER_X0) * c, (STICKER_Y1 - STICKER_Y0) * c, t[0], t[1], t[2], t[3],
					     rgba(0xFFFFFFu, a));
		return;
	}
	icon_body(x, y, s, a);
	float g = s * 0.14f;
	rsxkit_ui_gradient(x + g, y + g, s - 2 * g, s * 0.5f, rgba(mix(CARTUCHO_LABEL_GRAY, 0xFFFFFFu, 0.25f), a),
			   rgba(CARTUCHO_LABEL_GRAY, a), rgba(CARTUCHO_LABEL_GRAY, a),
			   rgba(mix(CARTUCHO_LABEL_GRAY, 0x000000u, 0.3f), a));
	for (int i = 0; i < 4; i++)
		rsxkit_ui_rect(x + s * (0.24f + 0.14f * i), y + s * 0.78f, s * 0.08f, s * 0.1f, rgba(0x0E1116u, a));
}

/* El cartucho 3D de la fila elegida: el gancho se llama al enviar el lote
 * del 2D (rsxkit_ui_end), después de la franja de la fila y antes de lo que
 * se dibujó luego. Solo hay una fila elegida por cuadro. */
static struct {
	menu_cart3d_fn fn;
	void *user;
	float x, y, s, a;
} cart3d_call;

static void cart3d_hook(void *p)
{
	(void)p;
	cart3d_call.fn(cart3d_call.user, cart3d_call.x, cart3d_call.y, cart3d_call.s, cart3d_call.a);
}

static void icon_cart3d(const struct menu *m, float x, float y, float s, float a)
{
	float c = s * 1.05f;
	cart3d_call.fn = m->cart3d;
	cart3d_call.user = m->cart3d_user;
	cart3d_call.x = x + s * 0.5f;
	cart3d_call.y = y + s * 0.5f;
	cart3d_call.s = c;
	cart3d_call.a = a;
	rsxkit_ui_hook(cart3d_hook, NULL);
}

static void icon_options(float x, float y, float s, float a)
{
	icon_body(x, y, s, a);
	for (int i = 0; i < 3; i++) {
		float yy = y + s * (0.28f + 0.2f * i);
		rsxkit_ui_rect(x + s * 0.22f, yy, s * 0.56f, s * 0.07f, rgba(0xE8EEF6u, a));
		rsxkit_ui_rect(x + s * (0.3f + 0.18f * ((i * 2) % 3)), yy - s * 0.05f, s * 0.12f, s * 0.17f,
			       rgba(C_ACCENT, a));
	}
}

static void icon_glyph(float x, float y, float s, const char *g, float a)
{
	icon_body(x, y, s, a);
	float sc = s / 22.0f;
	rsxkit_ui_text(x + s * 0.5f - rsxkit_ui_text_width(g, sc) * 0.5f, y + s * 0.5f - RSXKIT_FONT_H * sc * 0.5f, g, sc,
		       rgba(C_TEXT, a), 0);
}

/* Un disco de color con tiras horizontales (rsxkit_ui solo dibuja
 * rectángulos). */
static void ui_disc(float cx, float cy, float r, uint32_t rgb, float a)
{
	enum { STRIPS = 16 };
	float h = 2.0f * r / STRIPS;
	for (int i = 0; i < STRIPS; i++) {
		float yy = -r + ((float)i + 0.5f) * h, hw = sqrtf(fmaxf(0.0f, r * r - yy * yy));
		rsxkit_ui_rect(cx - hw, cy + yy - h * 0.5f, 2.0f * hw, h + 0.5f, rgba(rgb, a));
	}
}

static void ui_letter(float cx, float cy, const char *g, float sc, uint32_t rgb, float a)
{
	rsxkit_ui_text(cx - rsxkit_ui_text_width(g, sc) * 0.5f, cy - RSXKIT_FONT_H * sc * 0.5f, g, sc, rgba(rgb, a), 0);
}

/* El botón de N64 de la fila `i` de la página de controles (el orden de
 * salsa64_padmap: A, B, Z, Start, L, R, cruceta ↑↓←→, C ↑↓←→; y el stick),
 * con los íconos de GameCube de Kenney (prompts.h); sin ellos, con
 * rectángulos y discos. */
static void icon_n64_plain(float x, float y, float s, int i, float a)
{
	float cx = x + s * 0.5f, cy = y + s * 0.5f, r = s * 0.32f, sc = s / 24.0f;
	static const char *const arrows[] = { "↑", "↓", "←", "→" };
	if (i == 0 || i == 1 || i == 3) {  /* A azul, B verde, Start rojo */
		ui_disc(cx, cy, r, i == 0 ? 0x2E5BD8u : i == 1 ? 0x2FA84Fu : 0xD8332Eu, a);
		ui_letter(cx, cy, i == 0 ? "A" : i == 1 ? "B" : "S", sc, 0xFFFFFFu, a);
	} else if (i == 2 || i == 4 || i == 5) {  /* Z, L y R: gatillos grises */
		float w = i == 2 ? s * 0.42f : s * 0.62f, h = i == 2 ? s * 0.58f : s * 0.40f;
		rsxkit_ui_rect(cx - w * 0.5f, cy - h * 0.5f, w, h, rgba(0xA4AAB0u, a));
		ui_letter(cx, cy, i == 2 ? "Z" : i == 4 ? "L" : "R", sc, 0x23272Bu, a);
	} else if (i >= 6 && i <= 9) {  /* cruceta con el brazo de la dirección resaltado */
		float w = s * 0.22f, l = s * 0.36f;
		rsxkit_ui_rect(cx - w * 0.5f, cy - l, w, 2.0f * l, rgba(0x6F757Bu, a));
		rsxkit_ui_rect(cx - l, cy - w * 0.5f, 2.0f * l, w, rgba(0x6F757Bu, a));
		float ax = i == 8 ? cx - l : i == 9 ? cx + w * 0.5f : cx - w * 0.5f;
		float ay = i == 6 ? cy - l : i == 7 ? cy + w * 0.5f : cy - w * 0.5f;
		bool vert = i <= 7;
		rsxkit_ui_rect(ax, ay, vert ? w : l - w * 0.5f, vert ? l - w * 0.5f : w, rgba(0xFFFFFFu, a));
	} else if (i >= 10 && i <= 13) {  /* botones C amarillos */
		ui_disc(cx, cy, r, 0xF2C230u, a);
		ui_letter(cx, cy, arrows[i - 10], sc, 0x3A2A00u, a);
	} else {  /* el stick */
		ui_disc(cx, cy, r, 0xA4AAB0u, a);
		ui_disc(cx, cy, r * 0.45f, 0x6F757Bu, a);
	}
}

static void icon_n64(float x, float y, float s, int i, float a)
{
	icon_body(x, y, s, a);
	if (!prompts_enabled()) {
		icon_n64_plain(x, y, s, i, a);
		return;
	}
	static const unsigned char ids[] = {
		PROMPT_N64_A, PROMPT_N64_B, PROMPT_N64_Z, PROMPT_N64_START, PROMPT_N64_L, PROMPT_N64_R,
		PROMPT_N64_DPAD_UP, PROMPT_N64_DPAD_DOWN, PROMPT_N64_DPAD_LEFT, PROMPT_N64_DPAD_RIGHT,
		PROMPT_N64_C_UP, PROMPT_N64_C_DOWN, PROMPT_N64_C_LEFT, PROMPT_N64_C_RIGHT, PROMPT_N64_STICK,
	};
	int id = ids[i >= 0 && i < (int)(sizeof ids) ? i : (int)sizeof ids - 1];
	float z = s * 0.84f;
	/* El tinte (gris de Z, L y R) multiplica el alfa de a. */
	uint32_t t = prompt_table[id].tint;
	uint32_t al = (uint32_t)(clampf(a, 0.0f, 1.0f) * (float)(t & 0xFFu) + 0.5f);
	rsxkit_ui_image_part(PROMPTS_SLOT, x + (s - z) * 0.5f, y + (s - z) * 0.5f, z, z, prompt_table[id].x, prompt_table[id].y,
			     PROMPT_CELL, PROMPT_CELL, (t & 0xFFFFFF00u) | al);
}

static float row_y(float d, float H)
{
	float ad = fabsf(d), y = ad <= 1.0f ? ad * GAP : GAP + (ad - 1.0f) * SPACING;
	return H * (COL_Y + (d < 0 ? -y : y));
}

/* Una fila de la columna: ícono, título y, si está elegida, una línea más. */
struct row {
	const char *title, *sub;
	int icon;           /* 0 cartucho, 1 opciones, 2 glifo, 3 botón de N64 (label = fila) */
	const char *glyph;
	uint32_t label;
	const char *value;  /* a la derecha del título (opciones) */
};

/* La ROM de la fila que se dibuja (para su miniatura); NULL si no es una ROM. */
static const struct rom_entry *row_rom;

static void draw_row(const struct menu *m, const struct row *r, float d, float x, float W, float H, float a)
{
	float ad = fabsf(d), k = smooth(1.0f - ad), y = row_y(d, H);
	/* Se desvanece hacia los bordes de la columna (lejos del encabezado y
	 * del pie). */
	float edge = smooth((y - H * 0.16f) / (H * 0.06f)) * smooth((H * 0.79f - y) / (H * 0.07f));
	float alpha = a * edge * clampf(1.0f - 0.17f * ad, 0.22f, 1.0f);
	if (alpha <= 0.004f)
		return;
	if (k > 0.0f)
		rsxkit_ui_gradient(x - 26.0f, y - 42.0f, W * 0.53f - x + 26.0f, 84.0f, rgba(0xFFFFFFu, 0.13f * k * alpha),
				   rgba(0xFFFFFFu, 0.0f), rgba(0xFFFFFFu, 0.07f * k * alpha), rgba(0xFFFFFFu, 0.0f));
	float s = 42.0f + 26.0f * k;
	if (r->icon == 0 && m->cart3d && k > 0.5f)
		icon_cart3d(m, x, y - s * 0.5f, s, alpha);
	else if (r->icon == 0)
		icon_cart(m, row_rom, ad, x, y - s * 0.5f, s, alpha);
	else if (r->icon == 1)
		icon_options(x, y - s * 0.5f, s, alpha);
	else if (r->icon == 3)
		icon_n64(x, y - s * 0.5f, s, (int)r->label, alpha);
	else
		icon_glyph(x, y - s * 0.5f, s, r->glyph, alpha);
	float scale = 2.0f + 0.85f * k, tx = x + 90.0f;
	if (r->value) {
		/* Título y valor caben en la columna: si no, se achica la letra
		 * antes de cortar el título (hasta 1,4). */
		float need = cells_w(r->title, 1.0f) + cells_w(r->value, 1.0f), avail = W * 0.53f - tx - 24.0f;
		if (need > 0.0f && need * scale > avail)
			scale = fmaxf(1.4f, avail / need);
	}
	float th = RSXKIT_FONT_H * scale, ty = y - th * 0.5f - 10.0f * k;
	float room = W * 0.53f - tx;
	if (r->value)
		room -= cells_w(r->value, scale) + 24.0f;  /* el valor queda siempre dentro de la columna */
	float max = room > RSXKIT_FONT_W * scale ? room : RSXKIT_FONT_W * scale;
	stext(tx, ty, r->title, scale, k > 0.5f ? 0xFFFFFFu : C_TEXT, alpha, max);
	if (r->value) {
		float tw = rsxkit_ui_text_fit_width(tr(r->title), scale, max);
		stext(tx + tw + 24.0f, ty, r->value, scale, C_ACCENT, alpha, 0);
	}
	float ks = smooth((k - 0.35f) / 0.65f);
	if (r->sub && ks > 0.0f) {
		if (r->icon != 0)
			ptext(tx, ty + th + 6.0f, r->sub, 1.5f, C_DIM, alpha * ks, W * 0.53f - tx);
		else
			stext(tx, ty + th + 6.0f, r->sub, 1.5f, C_DIM, alpha * ks, W * 0.53f - tx);
	}
}

static const char *location(const char *path, char *buf, size_t n)
{
	if (!path[0])
		return "Todas las unidades";
	if (!strncmp(path, "/dev_hdd0/", 10))
		return "Disco duro";
	if (!strncmp(path, "/dev_usb", 8) && strlen(path) > 10) {
		snprintf(buf, n, tr("Memoria USB %c"), path[10]);
		return buf;
	}
	return path;
}

static void size_text(uint32_t bytes, char *buf, size_t n)
{
	if (bytes >= (1u << 20) && bytes % (1u << 20) == 0)
		snprintf(buf, n, "%u MB", bytes >> 20);
	else if (bytes >= (1u << 20))
		snprintf(buf, n, "%.1f MB", bytes / 1048576.0);
	else
		snprintf(buf, n, "%u KB", (bytes + 1023) >> 10);
}

static void info_line(float x, float *y, float vx, float vscale, const char *label, const char *value,
		      uint32_t color, float a, float max)
{
	stext(x, *y + 2.0f, label, 1.5f, C_DIM, a, 0);
	stext(vx, *y, value, vscale, color, a, max);
	*y += 34.0f;
}

/* Panel de la página de trucos: el truco elegido y cuántos hay. */
static void cheats_panel(const struct menu *m, float px, float py, float pw, float ph, float ca)
{
	bool narrow = pw < 450.0f;
	float tscale = narrow ? 2.0f : 2.5f, dscale = narrow ? 1.35f : 1.6f;
	float x = px + 26.0f, y = py + 24.0f;
	float tmax = pw - 48.0f, lmax = pw - 48.0f;
	const struct cheat_set *cs = m->cheats;
	char buf[96];

	stext(x, y, m->cht_title ? m->cht_title : "Trucos", tscale, 0xFFFFFFu, ca, tmax);
	y += 56.0f;
	snprintf(buf, sizeof buf, tr("%u trucos, %u activos"), cs->n, cheats_active(cs));
	stext(x, y, buf, dscale, C_DIM, ca, lmax);
	y += 44.0f;
	if (m->cht_sel < (int)cs->n) {
		const struct cheat *c = &cs->c[m->cht_sel];
		/* La descripción en dos renglones si no cabe en uno (corte por
		 * palabras, o entre caracteres en japonés, chino o coreano). */
		char l1[CHEAT_DESC_MAX], l2[CHEAT_DESC_MAX];
		const char *rest;
		size_t cut = rsxkit_ui_text_wrap(c->desc, dscale, lmax, &rest);
		snprintf(l1, sizeof l1, "%.*s", (int)cut, c->desc);
		snprintf(l2, sizeof l2, "%s", rest);
		stext(x, y, l1, dscale, C_TEXT, ca, lmax);
		if (l2[0])
			stext(x, y + 30.0f, l2, dscale, C_TEXT, ca, lmax);
		y += l2[0] ? 74.0f : 44.0f;
		stext(x, y, c->on ? "Activado" : "Desactivado", tscale, c->on ? C_ACCENT : C_DIM, ca, 0);
		y += 56.0f;
		snprintf(buf, sizeof buf, tr(c->count == 1 ? "%u línea de código" : "%u líneas de código"), c->count);
		stext(x, y, buf, dscale, C_DIM, ca, lmax);
	} else {
		stext(x, y, "A la lista de juegos.", dscale, C_TEXT, ca, lmax);
	}
	if (cs->unsupported)
		stext(x, py + ph - 74.0f, "Algunos códigos no se aplican.", dscale, C_WARN, ca, lmax);
	stext(x, py + ph - 44.0f, "Se recuerdan por juego.", dscale, C_DIM, ca, lmax);
}

static void cheats_column(const struct menu *m, float x, float W, float H, float a)
{
	int n = menu_cheat_rows(m);
	for (int i = 0; i < n; i++) {
		float d = i - m->cht_pos;
		if (fabsf(d) > 7.0f)
			continue;
		struct row r = { 0 };
		r.icon = 2;
		if (i < n - 1) {
			const struct cheat *c = &m->cheats->c[i];
			r.title = c->desc;
			r.glyph = c->on ? "●" : "○";
			r.value = c->on ? "Sí" : "No";
		} else {
			r.title = "Volver";
			r.sub = "A la lista de juegos";
			r.glyph = "◀";
		}
		draw_row(m, &r, d, x, W, H, a);
	}
}

static const char *game_value(const struct menu *m, int row)
{
	switch (row) {
	case MENU_GAME_ASPECT:   return m->game.aspect < 0 ? "Global" : aspect_name(m->game.aspect);
	case MENU_GAME_FILTER:   return m->game.filter < 0 ? "Global" : filter_name(m->game.filter);
	case MENU_GAME_DENSITY:  return m->game.density < 0 ? "Global" : menu_density_name(m->game.density);
	case MENU_GAME_OVERSCAN: return m->game.overscan < 0 ? "Global" : overscan_name(m->game.overscan);
	case MENU_GAME_SCALE: {
		static char buf[24];
		return m->game.scale < 0 ? "Global" : menu_scale_name(m->game.scale, m->set.resolution, buf, sizeof buf);
	}
	case MENU_GAME_INTERP:   return m->game.interp < 0 ? "Global" : menu_interp_name(m->game.interp);
	case MENU_GAME_ACCESSORY: return m->game.accessory < 0 ? "Global" : menu_accessory_name(m->game.accessory);
	case MENU_GAME_CONTROLS: return "Configurar";
	}
	return NULL;
}

/* Panel de los ajustes del juego: qué vale ahora de cada cosa. */
static void game_panel(const struct menu *m, float px, float py, float pw, float ph, float ca)
{
	bool narrow = pw < 450.0f;
	float tscale = narrow ? 2.0f : 2.5f, dscale = narrow ? 1.35f : 1.6f;
	float x = px + 26.0f, y = py + 24.0f;
	float tmax = pw - 48.0f, lmax = pw - 48.0f;
	int a, f, o, s;
	char sbuf[24];
	menu_game_effective(&m->set, &m->game, &a, &f, &o, &s);
	stext(x, y, m->game_title ? m->game_title : "Ajustes", tscale, 0xFFFFFFu, ca, tmax);
	y += 56.0f;
	stext(x, y, "Ajustes de este juego", dscale, C_DIM, ca, lmax);
	y += 30.0f;
	static const char *const names[] = { "Aspecto", "Filtro", "Densidad", "Recorte del borde", "Escala interna",
					     "Interpolación", "Controles", "Accesorio" };
	/* Con «Densidad» son ocho: más juntos, en el mismo alto. */
	float step = game_visible(m, MENU_GAME_DENSITY) ? 30.0f : 34.0f;
	for (int i = 0; i < MENU_GAME_BACK; i++) {
		if (!game_visible(m, i))
			continue;
		const char *v = i == MENU_GAME_ASPECT	 ? aspect_name(a)
				: i == MENU_GAME_FILTER	 ? filter_name(f)
				: i == MENU_GAME_DENSITY  ? menu_density_name(menu_game_density(&m->set, &m->game))
				: i == MENU_GAME_OVERSCAN ? overscan_name(o)
				: i == MENU_GAME_SCALE	 ? menu_scale_name(s, m->set.resolution, sbuf, sizeof sbuf)
				: i == MENU_GAME_INTERP ? menu_interp_name(menu_game_interp(&m->set, &m->game))
				: i == MENU_GAME_CONTROLS ? tr("Configurar")
							  : menu_accessory_name(m->game.accessory < 0 ? m->set.accessory : m->game.accessory);
		stext(x, y, names[i], 1.3f, C_DIM, ca, 0);
		stext(x, y + 16.0f, v, dscale, m->game_sel == i ? C_ACCENT : C_TEXT, ca, lmax);
		y += step;
	}
	stext(x, py + ph - 74.0f, "«Global» usa lo de Opciones.", dscale, C_DIM, ca, lmax);
	stext(x, py + ph - 44.0f, "Se guardan en juegos/.", dscale, C_DIM, ca, lmax);
}

static void game_column(const struct menu *m, float x, float W, float H, float a)
{
	static const char *const titles[] = { "Aspecto", "Filtro", "Densidad", "Recorte del borde", "Escala interna",
					      "Interpolación", "Controles", "Accesorio", "Volver" };
	static const char *const subs[] = { "Forma de la imagen", "Suave, nítida o con líneas", "Puntos de la máscara CRT",
					    "Overscan de este juego", "Dibujo interno", "60 cuadros en juegos de 20 o 30",
					    "Ajustar botones de este juego", "Para guardar partidas", "A la lista de juegos" };
	static const char *const glyphs[] = { "▶", "─", "●", "□", "▲", "►", "●", "○", "◀" };
	_Static_assert(sizeof titles / sizeof titles[0] == MENU_GAME_ROWS, "una fila por ajuste");
	for (int i = 0; i < MENU_GAME_ROWS; i++) {
		if (!game_visible(m, i))
			continue;
		float d = (float)game_row(m, i) - m->game_pos;
		struct row r = { titles[i], subs[i], 2, glyphs[i], 0, game_value(m, i) };
		draw_row(m, &r, d, x, W, H, a);
	}
}

static const char *control_row_title(int row)
{
	static const char *const n[] = { "A", "B", "Z", "Start", "L", "R", "Cruceta arriba", "Cruceta abajo",
		"Cruceta izquierda", "Cruceta derecha", "C arriba", "C abajo", "C izquierda", "C derecha",
		"Stick", "Restaurar predeterminado", "Volver" };
	return row >= 0 && row < MENU_CONTROL_ROWS ? n[row] : "";
}

static const char *control_button_value(const struct menu *m, int row)
{
	if (row < SALSA64_IN_COUNT) {
		unsigned b = m->controls.source[row];
		static const char *const names[] = { "Cruz", "Círculo", "Cuadrado", "Triángulo", "L1", "R1", "L2", "R2",
			"L3", "R3", "Start", "Select", "Arriba", "Abajo", "Izquierda", "Derecha" };
		if (row >= SALSA64_IN_C_UP && row <= SALSA64_IN_C_RIGHT) {
			/* Los C siempre salen del otro stick; un botón asignado se suma. */
			static char buf[64];
			const char *stick = tr(m->controls.stick_source == SALSA64_STICK_RIGHT ? "Stick izquierdo"
												 : "Stick derecho");
			if (b >= SALSA64_DS3_NBUTTONS)
				return stick;
			snprintf(buf, sizeof buf, "%s / %s", stick, tr(names[b]));
			return buf;
		}
		unsigned b2 = m->controls.source2[row];
		if (b < SALSA64_DS3_NBUTTONS && b2 < SALSA64_DS3_NBUTTONS) {
			static char two[64];
			snprintf(two, sizeof two, "%s / %s", tr(names[b]), tr(names[b2]));
			return two;
		}
		return b < SALSA64_DS3_NBUTTONS ? tr(names[b]) : tr("Sin asignar");
	}
	if (row == MENU_CONTROL_STICK)
		return tr(m->controls.stick_source == SALSA64_STICK_RIGHT ? "Stick derecho" : "Stick izquierdo");
	return NULL;
}

static void controls_column(const struct menu *m, float x, float W, float H, float a)
{
	for (int i = 0; i < MENU_CONTROL_ROWS; i++) {
		bool n64 = i < SALSA64_IN_COUNT || i == MENU_CONTROL_STICK;
		struct row r = { tr(control_row_title(i)), NULL, n64 ? 3 : 2, i == MENU_CONTROL_BACK ? "◀" : "●",
				 n64 ? (uint32_t)i : 0, control_button_value(m, i) };
		draw_row(m, &r, (float)i - m->control_pos, x, W, H, a);
	}
}

/* Los datos de una ROM en el panel. Con carátula (`cover`): ella a la
 * izquierda y los datos, cada uno con su rótulo encima, a su derecha. */
static void rom_info(const struct menu *m, const struct rom_entry *e, float px, float py, float pw, float ph, float ca,
		     bool cover)
{
	bool narrow = pw < 450.0f;
	float tscale = narrow ? 2.0f : 2.5f, vscale = narrow ? 1.5f : 1.75f, dscale = narrow ? 1.35f : 1.6f;
	float x = px + 26.0f, y = py + 24.0f, vx = x + (narrow ? 124.0f : 140.0f);
	float vmax = px + pw - 20.0f - vx, tmax = pw - 48.0f;
	char buf[ROMLIST_PATH_MAX + 32], loc[24];
	(void)dscale;
	if (cover) {
		/* Carátula: cabe en cw × chh sin cambiar su proporción. */
		/* Abajo queda sitio para «● Último abierto». */
		float cw = pw * (m->cover_w > m->cover_h ? 0.52f : 0.42f), chh = ph - 24.0f - 60.0f - 44.0f;
		float s = fminf(cw / (float)m->cover_w, chh / (float)m->cover_h);
		float iw = (float)m->cover_w * s, ih = (float)m->cover_h * s;
		float iy = py + 24.0f + 60.0f;
		rsxkit_ui_rect(x - 3.0f, iy - 3.0f, iw + 6.0f, ih + 6.0f, rgba(0x000000u, ca * 0.55f));
		rsxkit_ui_image(0, x, iy, iw, ih, rgba(0xFFFFFFu, ca));
		vx = x + cw + 16.0f;
		vscale = narrow || m->cover_w > m->cover_h ? 1.35f : 1.5f;
		vmax = px + pw - 14.0f - vx;
		stext(x, y, romlist_display_title(e), tscale, 0xFFFFFFu, ca, tmax);
		y = iy;
		float step = narrow ? 44.0f : 50.0f;
		#define COVER_LINE(label, value, color)                                          \
			do {                                                                      \
				stext(vx, y, label, 1.2f, C_DIM, ca, 0);                          \
				stext(vx, y + 16.0f, value, vscale, color, ca, vmax);             \
				y += step;                                                        \
			} while (0)
		COVER_LINE("Archivo", romlist_name(e), C_TEXT);
		if (e->code[0])
			snprintf(buf, sizeof buf, "%s v%u", e->code, e->version);
		else
			snprintf(buf, sizeof buf, "—");
		COVER_LINE("Código", buf, C_TEXT);
		size_text(e->size, buf, sizeof buf);
		COVER_LINE("Tamaño", buf, C_TEXT);
		{
			const char *region = romlist_region_name(e->region),
				   *tv = e->tv == SALSA64_TV_PAL ? "PAL" : e->tv == SALSA64_TV_MPAL ? "PAL-M" : "NTSC";
			if (strcmp(region, "?"))
				snprintf(buf, sizeof buf, "%s (%s)", region, tv);
			else
				snprintf(buf, sizeof buf, "%s", tv);
			COVER_LINE("Región", buf, C_TEXT);
		}
		bool emulated = e->save == SALSA64_SAVE_NONE || salsa64_save_emulated((enum salsa64_save_type)e->save);
		snprintf(buf, sizeof buf, "%s%s", romlist_save_label(e->save), emulated ? "" : tr(" (sin emular)"));
		COVER_LINE("Guardado", buf, emulated ? C_TEXT : C_WARN);
		#undef COVER_LINE
		if (m->last[0] && !strcmp(m->last, romlist_name(e)))
			stext(x, py + ph - 30.0f, "● Último abierto", 1.2f, C_ACCENT, ca, 0);
		return;
	}
	stext(x, y, romlist_display_title(e), tscale, 0xFFFFFFu, ca, tmax);
	y += 60.0f;
	info_line(x, &y, vx, vscale, "Archivo", romlist_name(e), C_TEXT, ca, vmax);
	info_line(x, &y, vx, vscale, "Ubicación", location(m->list->directory, loc, sizeof loc), C_TEXT, ca, vmax);
	if (e->code[0])
		snprintf(buf, sizeof buf, tr("%s, versión %u"), e->code, e->version);
	else
		snprintf(buf, sizeof buf, "—");
	info_line(x, &y, vx, vscale, "Código", buf, C_TEXT, ca, vmax);
	size_text(e->size, buf, sizeof buf);
	info_line(x, &y, vx, vscale, "Tamaño", buf, C_TEXT, ca, vmax);
	const char *region = romlist_region_name(e->region),
		   *tv = e->tv == SALSA64_TV_PAL ? "PAL" : e->tv == SALSA64_TV_MPAL ? "PAL-M" : "NTSC";
	if (strcmp(region, "?"))
		snprintf(buf, sizeof buf, "%s (%s)", region, tv);
	else
		snprintf(buf, sizeof buf, "%s", tv);
	info_line(x, &y, vx, vscale, "Región", buf, C_TEXT, ca, vmax);
	bool emulated = e->save == SALSA64_SAVE_NONE || salsa64_save_emulated((enum salsa64_save_type)e->save);
	snprintf(buf, sizeof buf, "%s%s", romlist_save_label(e->save), emulated ? "" : tr(" (sin emular)"));
	info_line(x, &y, vx, vscale, "Guardado", buf, emulated ? C_TEXT : C_WARN, ca, vmax);
	if (m->last[0] && !strcmp(m->last, romlist_name(e)))
		stext(x, py + ph - 44.0f, "● Último juego abierto", 1.5f, C_ACCENT, ca, 0);
}

static void panel(const struct menu *m, float W, float H, float a)
{
	float px = W * 0.555f, py = H * 0.17f, pw = W * 0.39f, ph = H * 0.56f;
	rsxkit_ui_gradient(px, py, pw, ph, rgba(0x000000u, a * 0.40f), rgba(0x000000u, a * 0.28f),
			   rgba(0x000000u, a * 0.24f), rgba(0x000000u, a * 0.14f));
	rsxkit_ui_rect(px, py, pw, 2.0f, rgba(0xFFFFFFu, a * 0.32f));
	if (m->page == MENU_PAGE_CONTROLS) {
		float x = px + 26.0f, y = py + 24.0f;
		float tmax = pw - 48.0f;
		stext(x, y, tr("Controles"), 2.0f, 0xFFFFFFu, a, tmax);
		/* Desde Opciones se cambia el mapeo de todos los juegos; desde los
		 * ajustes de una ROM (SELECT), solo el de ese juego. */
		stext(x, y + 30.0f, tr(m->control_for_game ? "Este juego" : "Todos los juegos"), 1.35f, C_ACCENT, a,
		      pw - 52.0f);
		y += 78.0f;
#define PANEL_MAX(sc) (pw - 52.0f)
		stext(x, y, tr(control_row_title(m->control_sel)), 1.6f, C_DIM, a, PANEL_MAX(1.6f));
		stext(x, y + 30.0f, control_button_value(m, m->control_sel), 2.0f, C_ACCENT, a, PANEL_MAX(2.0f));
		if (m->control_capture) {
			stext(x, py + ph - 70.0f, tr("Pulsa uno o dos botones"), 1.35f, C_WARN, a, PANEL_MAX(1.35f));
			ptext(x, py + ph - 42.0f, tr("START cancela · SELECT sin asignar"), 1.2f, C_DIM, a, PANEL_MAX(1.2f));
		} else if (m->control_sel == MENU_CONTROL_STICK) {
			ptext(x, py + ph - 52.0f, tr("× alterna el stick analógico"), 1.35f, C_DIM, a, PANEL_MAX(1.35f));
		}
#undef PANEL_MAX
		return;
	}

	/* Los trucos de la ROM, cuando la página está (o se está yendo). */
	float ct = smooth(m->cht_t);
	if (ct > 0.001f && m->cheats)
		cheats_panel(m, px, py, pw, ph, a * ct * (1.0f - clampf(fabsf(m->cht_sel - m->cht_pos) * 1.6f, 0.0f, 1.0f)));
	if (m->page == MENU_PAGE_CHEATS)
		return;
	float gt = smooth(m->game_t);
	if (gt > 0.001f)
		game_panel(m, px, py, pw, ph, a * gt);
	if (m->page == MENU_PAGE_GAME)
		return;

	/* Lo de adentro aparece cuando la columna se detiene. */
	bool games = m->page == MENU_PAGE_GAMES;
	float settle = 1.0f - clampf(fabsf(games ? m->sel - m->pos : opt_row(m, m->opt) - m->opt_pos) * 1.6f, 0.0f, 1.0f);
	float ca = a * smooth(settle) * (games ? 1.0f - m->page_t : m->page_t) * (1.0f - ct) * (1.0f - gt);
	/* Con tele 4:3 el panel es angosto: letra un poco más chica. */
	bool narrow = pw < 450.0f;
	float tscale = narrow ? 2.0f : 2.5f, dscale = narrow ? 1.35f : 1.6f;
	float x = px + 26.0f, y = py + 24.0f;
	float tmax = pw - 48.0f, lmax = pw - 48.0f;
	char buf[ROMLIST_PATH_MAX + 32];

	if (!games) {
		static const char *const waves_text[] = { "Ondas: las de siempre.", "Seda: cintas de tela que",
							  "ondulan y brillan de canto.", "No: fondo fijo.", NULL,
							  "Se guarda en menu.cfg.", NULL, NULL };
		static const char *const crop_text[] = { "Quita el borde de la imagen", "que una tele de tubo no deja",
							 "ver (ahí algunos juegos dejan", "basura).", NULL,
							 "Normal: 8 y 6 puntos por lado;", "amplio: 16 y 12.",
							 "Se guarda en overscan.cfg.", NULL, NULL };
		static const char *const aspect_text[] = { "4:3: la imagen original de", "la N64, con franjas a los lados", "en una tele panorámica.",
							   "16:9: la estira para llenar", "la pantalla.", NULL,
							   "Vale al abrir el juego.", "Se guarda en menu.cfg.", NULL, NULL };
		/* Sin las líneas de «vale al abrir»: abajo del valor va el crédito. */
		static const char *const filter_text[] = { "Suave: la imagen se mezcla", "al escalar (la de siempre).", "Nítida: puntos cuadrados,",
							   "sin difuminar.", "Líneas: oscurece filas como", "una tele de tubo (suaves o", "fuertes).", NULL,
							   NULL };
		/* Los CRT, cada uno con lo suyo. Cada fila termina en dos NULL
		 * (los que falten los pone el tamaño): con uno solo, el bucle de
		 * abajo leía fuera del arreglo y colgaba la PS3. */
		static const char *const crt_text[5][8] = {
			{ "CRT de máscara de sombra", "(experimental).", "Puntos de fósforo", "redondos en triángulo.", NULL, NULL,
			  NULL },
			{ "CRT con cable RGB", "(experimental).", "Tele Trinitron chica por", "euroconector RGB: imagen", "limpia.",
			  NULL, NULL },
			{ "CRT con cable S-Video", "(experimental).", "Tele Trinitron chica por", "S-Video: el color se",
			  "corre un poco.", NULL, NULL },
			{ "CRT con cable compuesto", "(experimental).", "Tele Trinitron chica por", "compuesto: sangrado de",
			  "color y dot crawl.", NULL, NULL },
			{ "CRT Trinitron", "(experimental).", "Sony KV-29 de la sala,", "sin la señal del cable:",
			  "franjas de fósforo y", "líneas del haz.", NULL, NULL },
		};
		bool crt = m->set.filter >= MENU_FILTER_CRT_MASK && m->set.filter <= MENU_FILTER_CRT_TRINITRON;
		static const char *const density_text[] = { "Los puntos de fósforo del", "CRT de máscara de sombra.",
							    "Normal: como el filtro", "original. Fina: puntos más",
							    "chicos y más juntos.", "Se guarda en menu.cfg.", NULL,
							    NULL };
		static const char *const lang_text[] = { "Idioma del menú y de los", "avisos del juego.", "Automático: el de la consola.", NULL,
						       "Se guarda en menu.cfg.", NULL, NULL };
		static const char *const interp_text[] = { "Dibuja los campos de en", "medio con los objetos entre",
							   "un cuadro y el siguiente:", "Zelda (20 cuadros) se ve a",
							   "60, con 33 ms de retraso.", "(experimental).", NULL,
							   "Se guarda en menu.cfg.", NULL, NULL };
		static const char *const dlc_text[] = { "Baja las carátulas de los", "juegos de la lista a", "covers/. Hace falta red", "y la dirección en",
							"descargas.cfg.", NULL, "×: descargar.", NULL, NULL };
		static const char *const dlt_text[] = { "Baja los trucos de los", "juegos de la lista a", "cheats/. Hace falta red", "y la dirección en",
							"descargas.cfg.", NULL, "×: descargar.", NULL, NULL };
		static const char *const res_text[] = { "Salida de video: 720p (3x)", "o 1080p (4x). Luego, abajo,", "«Aplicar ahora».",
							"Se guarda en video.cfg.", NULL, NULL };
		static const char *const fps_text[] = { "Campos del VI por segundo y", "velocidad, cuadros del juego",
							"y de la pantalla, en una", "esquina. En el juego,",
							"SELECT + triángulo.", "Se guarda en menu.cfg.", NULL, NULL };
		static const char *const confirm_text[] = { "START + SELECT en el juego", "pregunta antes de salir:",
							    "× sí, ○ sigue jugando.", "Con «No», sale directo.",
							    "Se guarda en menu.cfg.", NULL, NULL };
		static const char *const accessory_text[] = { "Accesorio inicial de los cuatro", "controles de N64.", NULL,
							       "Controller Pak guarda datos.", "Rumble Pak activa la vibración.",
							       "Transfer Pak carga .gb/.gbc y usa .sav.", NULL,
							       "Se guarda en menu.cfg.", NULL, NULL };
		static const char *const ra_text[] = { "Logros de retroachievements.org en", "los juegos de N64: avisos al",
						       "desbloquear y lista en la pausa.", NULL,
						       "Hace falta una cuenta y red.", "Se guarda en menu.cfg.", NULL, NULL };
		static const char *const ra_acc_text[] = { "Usuario y contraseña con el", "teclado del sistema. Solo se",
							   "guarda el token, en ra.cfg;", "la contraseña, nunca.", NULL,
							   "×: iniciar o cerrar sesión.", NULL, NULL };
		static const char *const ra_hc_text[] = { "Hardcore: sin cargar estados", "ni trucos mientras hay logros.",
							  "Vale al abrir el siguiente", "juego.", NULL,
							  "Se guarda en menu.cfg.", NULL, NULL };
		const char *const order_text[] = { tr("Orden de los juegos de la lista."), NULL,
						   tr("Se guarda en menu.cfg."), NULL, NULL };
		/* Escala interna: tamaño y texto dependen del valor. */
		char scale_buf[24], size_buf[32];
		int eff = m->set.scale ? m->set.scale : (m->set.resolution == MENU_RES_1080P ? 4 : 3);
		snprintf(size_buf, sizeof size_buf, tr("%d× = %d×%d puntos"), eff, 320 * eff, 240 * eff);
		const char *const scale_text[] = { size_buf, "Mayor que la pantalla es", "supermuestreo: más nitidez",
						   "y bordes suaves, pero más", "trabajo para el RSX.",
						   "1×: la imagen original N64.", "Luego, «Aplicar ahora».", NULL, NULL };
		char want_buf[40];
		snprintf(want_buf, sizeof want_buf, tr("%s, escala %d×"), resolution_name(m->set.resolution),
			 m->set.scale ? m->set.scale : (m->set.resolution == MENU_RES_1080P ? 4 : 3));
		const char *const apply_text[] = { "Pone la salida y la escala", "elegidas sin cerrar Salsa64:", want_buf, NULL,
						   "Si la tele no acepta el", "modo, queda el anterior.", NULL, NULL };
		static const char *const back_text[] = { "A la lista de juegos.", NULL, NULL };
		const char *title = m->opt == MENU_OPT_WAVES ? "Fondo"
				    : m->opt == MENU_OPT_OVERSCAN ? "Recorte del borde"
				    : m->opt == MENU_OPT_ASPECT ? "Aspecto"
				    : m->opt == MENU_OPT_FILTER ? "Filtro de imagen"
				    : m->opt == MENU_OPT_DENSITY ? "Densidad de la máscara"
					    : m->opt == MENU_OPT_LANG ? "Idioma"
					    : m->opt == MENU_OPT_DL_COVERS ? "Descargar carátulas"
					    : m->opt == MENU_OPT_DL_CHEATS ? "Descargar trucos"
					    : m->opt == MENU_OPT_RESOLUTION ? "Resolución"
				    : m->opt == MENU_OPT_SCALE ? "Escala interna"
				    : m->opt == MENU_OPT_APPLY ? "Aplicar ahora"
				    : m->opt == MENU_OPT_INTERP ? "Interpolación"
				    : m->opt == MENU_OPT_FPS ? "Mostrar FPS"
					    : m->opt == MENU_OPT_CONFIRM ? "Confirmar al salir"
					    : m->opt == MENU_OPT_ACCESSORY ? "Accesorio del control"
					    : m->opt == MENU_OPT_ROM_ORDER ? "Orden de la lista"
				    : m->opt == MENU_OPT_RA ? "RetroAchievements"
				    : m->opt == MENU_OPT_RA_ACCOUNT ? "Cuenta de RetroAchievements"
				    : m->opt == MENU_OPT_RA_HARDCORE ? "Hardcore"
					    : m->opt == MENU_OPT_REFRESH ? "Actualizar lista" : "Volver";
		const char *const *text = m->opt == MENU_OPT_WAVES ? waves_text
					  : m->opt == MENU_OPT_OVERSCAN ? crop_text
					  : m->opt == MENU_OPT_ASPECT ? aspect_text
					  : m->opt == MENU_OPT_FILTER ? (crt ? crt_text[m->set.filter - MENU_FILTER_CRT_MASK]
										  : filter_text)
					  : m->opt == MENU_OPT_DENSITY ? density_text
					  : m->opt == MENU_OPT_LANG ? lang_text
					  : m->opt == MENU_OPT_DL_COVERS ? dlc_text
					  : m->opt == MENU_OPT_DL_CHEATS ? dlt_text
					  : m->opt == MENU_OPT_RESOLUTION ? res_text
					  : m->opt == MENU_OPT_SCALE ? scale_text
					  : m->opt == MENU_OPT_APPLY ? apply_text
					  : m->opt == MENU_OPT_INTERP ? interp_text
					  : m->opt == MENU_OPT_FPS ? fps_text
					  : m->opt == MENU_OPT_CONFIRM ? confirm_text
					  : m->opt == MENU_OPT_ACCESSORY ? accessory_text
					  : m->opt == MENU_OPT_ROM_ORDER ? order_text
					  : m->opt == MENU_OPT_RA ? ra_text
					  : m->opt == MENU_OPT_RA_ACCOUNT ? ra_acc_text
					  : m->opt == MENU_OPT_RA_HARDCORE ? ra_hc_text : back_text;
		const char *value = m->opt == MENU_OPT_WAVES ? background_name(&m->set)
				    : m->opt == MENU_OPT_OVERSCAN ? overscan_name(m->set.overscan)
				    : m->opt == MENU_OPT_ASPECT ? aspect_name(m->set.aspect)
				    : m->opt == MENU_OPT_FILTER ? filter_name(m->set.filter)
				    : m->opt == MENU_OPT_DENSITY ? menu_density_name(m->set.density)
				    : m->opt == MENU_OPT_LANG ? lang_name(m->set.lang)
				    : m->opt == MENU_OPT_RESOLUTION ? resolution_name(m->set.resolution)
				    : m->opt == MENU_OPT_SCALE ? scale_name(&m->set, scale_buf, sizeof scale_buf)
				    : m->opt == MENU_OPT_INTERP ? menu_interp_name(m->set.interp)
				    : m->opt == MENU_OPT_FPS ? (m->set.fps ? "Sí" : "No")
				    : m->opt == MENU_OPT_CONFIRM ? (m->set.confirm ? "Sí" : "No")
				    : m->opt == MENU_OPT_ACCESSORY ? menu_accessory_name(m->set.accessory)
				    : m->opt == MENU_OPT_ROM_ORDER ? (m->set.rom_order == MENU_ROM_ORDER_FILENAME ? tr("Nombre de archivo") : tr("Título de cabecera"))
				    : m->opt == MENU_OPT_RA ? (m->set.ra ? "Sí" : "No")
				    : m->opt == MENU_OPT_RA_ACCOUNT ? (m->ra_busy ? "Conectando…" : m->ra_user ? m->ra_user : "Iniciar sesión")
				    : m->opt == MENU_OPT_RA_HARDCORE ? (m->set.ra_hardcore ? "Sí" : "No") : NULL;
		stext(x, y, title, tscale, 0xFFFFFFu, ca, tmax);
		y += 56.0f;
		/* Líneas cortas (caben en 4:3); NULL es un renglón en blanco, dos
		 * NULL seguidos terminan. */
		bool dim = false;  /* lo que va después del renglón en blanco */
		for (int i = 0; text[i] || text[i + 1]; i++, y += 30.0f) {
			if (!text[i])
				dim = true;
			else
				ptext(x, y, text[i], dscale, dim ? C_DIM : C_TEXT, ca, lmax);
		}
		if (value)
			stext(x, y + 16.0f, value, tscale, C_ACCENT, ca, 0);
		if (m->note[0] && (m->opt == MENU_OPT_RESOLUTION || m->opt == MENU_OPT_SCALE || m->opt == MENU_OPT_APPLY))
			stext(x, py + ph - 44.0f, m->note, dscale, C_ACCENT, ca, lmax);
		if (m->opt == MENU_OPT_FILTER) {  /* quién hizo el filtro elegido */
			int f = m->set.filter >= 0 && m->set.filter < MENU_FILTERS ? m->set.filter : MENU_FILTER_SMOOTH;
			stext(x, y + 16.0f + 46.0f, filter_info[f].credit, 1.35f, C_DIM, ca, lmax);
		}
		if (m->opt == MENU_OPT_RESOLUTION) {
			y += 16.0f + 56.0f;
			if (m->out_w && m->out_h) {
				snprintf(buf, sizeof buf, tr("Salida actual: %ux%u"), m->out_w, m->out_h);
				stext(x, y, buf, dscale, C_TEXT, ca, lmax);
				y += 30.0f;
			}
			/* Pidió 1080p al arrancar y la tele o el XMB no lo dieron. */
			if (m->res_run == MENU_RES_1080P && m->out_h && m->out_h != 1080) {
				stext(x, y, "No hay 1080p: actívalo en", dscale, C_DIM, ca, lmax);
				stext(x, y + 30.0f, "Ajustes de pantalla del XMB.", dscale, C_DIM, ca, lmax);
			}
		}
		return;
	}
	int s = menu_selected(m);
	if (s < 0) {
		bool exit = m->sel == menu_row_exit(m);
		stext(x, y, exit ? "Salir al XMB" : "Opciones", tscale, 0xFFFFFFu, ca, tmax);
		stext(x, y + 56.0f, exit ? "Cierra Salsa64 y vuelve" : "Fondo, recorte, filtro,", dscale, C_TEXT, ca, lmax);
		stext(x, y + 86.0f, exit ? "al menú de la consola." : "resolución, idioma y FPS.", dscale, C_TEXT, ca, lmax);
		return;
	}
	const struct rom_entry *e = &m->list->e[s];
	float cv = m->cover_rom == s && m->cover_w ? smooth(m->cover_t) : 0.0f;
	if (cv < 0.999f)
		rom_info(m, e, px, py, pw, ph, ca * (1.0f - cv), false);
	if (cv > 0.001f)
		rom_info(m, e, px, py, pw, ph, ca * cv, true);
}

static void games_column(const struct menu *m, float x, float W, float H, float a)
{
	int n = menu_rows(m);
	int first = (int)floorf(m->pos - 7.0f), last = (int)ceilf(m->pos + 7.0f);
	if (first < 0)
		first = 0;
	if (last >= n)
		last = n - 1;
	for (int i = first; i <= last; i++) {
		float d = i - m->pos;
		struct row r = { 0 };
		row_rom = NULL;
		if (i < (int)m->list->count) {
			row_rom = m->list->e[i].kind == ROMLIST_ROM ? &m->list->e[i] : NULL;
			const struct rom_entry *e = &m->list->e[i];
			r.title = romlist_display_title(e);
			r.label = label_color(r.title);
			r.sub = e->kind == ROMLIST_DIRECTORY ? tr("Carpeta")
				: e->kind == ROMLIST_PARENT ? tr("Subir un nivel") : romlist_name(e);
			if (e->kind == ROMLIST_DIRECTORY || e->kind == ROMLIST_PARENT) {
				r.icon = 2;
				r.glyph = e->kind == ROMLIST_DIRECTORY ? "▶" : "◀";
			}
		} else if (i == (int)m->list->count) {
			r.title = "Opciones";
			r.sub = "Fondo, imagen, idioma y descargas";
			r.icon = 1;
		} else {
			r.title = "Salir al XMB";
			r.sub = "Vuelve al menú de la consola";
			r.icon = 2;
			r.glyph = "◀";
		}
		draw_row(m, &r, d, x, W, H, a);
	}
	if (!m->list->count) {
		float y = H * 0.16f;
		stext(x, y, "No encontré ROMs (.z64, .v64, .n64) en:", 1.6f, C_TEXT, a, 0);
		for (unsigned i = 0; i < m->ndirs && i < MENU_DIRS_MAX; i++)
			stext(x + 24.0f, y + 28.0f * (i + 1), m->dirs[i], 1.5f, C_DIM, a, 0);
	}
}

static void options_column(const struct menu *m, float x, float W, float H, float a)
{
	struct row waves = { "Fondo", "Ondas, seda o fijo", 2, "~", 0, background_name(&m->set) };
	struct row crop = { "Recorte del borde", "Overscan de la imagen del juego", 2, "□", 0,
			    overscan_name(m->set.overscan) };
	struct row aspect = { "Aspecto", "Forma de la imagen del juego (al abrir uno)", 2, "▶", 0,
			      aspect_name(m->set.aspect) };
	struct row filter = { "Filtro", "Suave, nítida, líneas o CRT", 2, "─", 0, filter_name(m->set.filter) };
	struct row density = { "Densidad", "Puntos de la máscara CRT", 2, "●", 0, menu_density_name(m->set.density) };
	struct row lang = { "Idioma", "Idioma del menú y de los avisos", 2, "A", 0, lang_name(m->set.lang) };
	struct row dlc = { "Descargar carátulas", "De internet a la carpeta covers", 2, "↓", 0, NULL };
	struct row dlt = { "Descargar trucos", "De internet a la carpeta cheats", 2, "↓", 0, NULL };
	struct row res = { "Resolución", "Salida de video", 2, "■", 0, resolution_name(m->set.resolution) };
	char sbuf[24];
	struct row scale = { "Escala interna", "Dibujo interno", 2, "▲", 0,
			     scale_name(&m->set, sbuf, sizeof sbuf) };
	struct row apply = { "Aplicar ahora", "Salida y escala elegidas", 2, "●", 0, NULL };
	struct row interp = { "Interpolación", "60 cuadros en juegos de 20 o 30", 2, "►", 0,
			      menu_interp_name(m->set.interp) };
	struct row fps = { "Mostrar FPS", "Velocidad en una esquina del juego", 2, "#", 0, m->set.fps ? "Sí" : "No" };
	struct row confirm = { "Confirmar al salir", "START + SELECT pregunta antes", 2, "?", 0,
			       m->set.confirm ? "Sí" : "No" };
	struct row accessory = { "Accesorio del control", "Controller Pak, Rumble Pak, Transfer Pak o ninguno", 2, "●", 0,
				  menu_accessory_name(m->set.accessory) };
	struct row order = { "Orden de la lista", "Título de cabecera o nombre de archivo", 2, "↑", 0,
			     m->set.rom_order == MENU_ROM_ORDER_FILENAME ? tr("Nombre de archivo") : tr("Título de cabecera") };
	struct row ra = { "RetroAchievements", "Logros de retroachievements.org", 2, "R", 0, m->set.ra ? "Sí" : "No" };
	struct row ra_acc = { "Cuenta de RetroAchievements", "Iniciar o cerrar sesión", 2, "●", 0,
			      m->ra_busy ? "Conectando…" : m->ra_user ? m->ra_user : "Iniciar sesión" };
	struct row ra_hc = { "Hardcore", "Sin estados ni trucos con logros", 2, "!", 0, m->set.ra_hardcore ? "Sí" : "No" };
	struct row refresh = { "Actualizar lista", "Volver a leer las carpetas de ROMs", 2, "○", 0, NULL };
	struct row controls = { "Controles", "Botones del control de N64", 2, "●", 0, NULL };
	struct row back = { "Volver", "A la lista de juegos", 2, "◀", 0, NULL };
	draw_row(m, &waves, MENU_OPT_WAVES - m->opt_pos, x, W, H, a);
	draw_row(m, &crop, MENU_OPT_OVERSCAN - m->opt_pos, x, W, H, a);
	draw_row(m, &aspect, MENU_OPT_ASPECT - m->opt_pos, x, W, H, a);
	draw_row(m, &filter, MENU_OPT_FILTER - m->opt_pos, x, W, H, a);
	if (opt_visible(m, MENU_OPT_DENSITY))
		draw_row(m, &density, (float)opt_row(m, MENU_OPT_DENSITY) - m->opt_pos, x, W, H, a);
	draw_row(m, &res, (float)opt_row(m, MENU_OPT_RESOLUTION) - m->opt_pos, x, W, H, a);
	draw_row(m, &scale, (float)opt_row(m, MENU_OPT_SCALE) - m->opt_pos, x, W, H, a);
	if (menu_video_pending(m))
		draw_row(m, &apply, (float)opt_row(m, MENU_OPT_APPLY) - m->opt_pos, x, W, H, a);
	draw_row(m, &interp, (float)opt_row(m, MENU_OPT_INTERP) - m->opt_pos, x, W, H, a);
	draw_row(m, &fps, (float)opt_row(m, MENU_OPT_FPS) - m->opt_pos, x, W, H, a);
	draw_row(m, &confirm, (float)opt_row(m, MENU_OPT_CONFIRM) - m->opt_pos, x, W, H, a);
	draw_row(m, &accessory, (float)opt_row(m, MENU_OPT_ACCESSORY) - m->opt_pos, x, W, H, a);
	draw_row(m, &lang, (float)opt_row(m, MENU_OPT_LANG) - m->opt_pos, x, W, H, a);
	draw_row(m, &order, (float)opt_row(m, MENU_OPT_ROM_ORDER) - m->opt_pos, x, W, H, a);
	draw_row(m, &ra, (float)opt_row(m, MENU_OPT_RA) - m->opt_pos, x, W, H, a);
	draw_row(m, &ra_acc, (float)opt_row(m, MENU_OPT_RA_ACCOUNT) - m->opt_pos, x, W, H, a);
	draw_row(m, &ra_hc, (float)opt_row(m, MENU_OPT_RA_HARDCORE) - m->opt_pos, x, W, H, a);
	draw_row(m, &refresh, (float)opt_row(m, MENU_OPT_REFRESH) - m->opt_pos, x, W, H, a);
	draw_row(m, &controls, (float)opt_row(m, MENU_OPT_CONTROLS) - m->opt_pos, x, W, H, a);
	if (m->dl_enabled) {
		draw_row(m, &dlc, (float)opt_row(m, MENU_OPT_DL_COVERS) - m->opt_pos, x, W, H, a);
		draw_row(m, &dlt, (float)opt_row(m, MENU_OPT_DL_CHEATS) - m->opt_pos, x, W, H, a);
	}
	draw_row(m, &back, (float)opt_row(m, MENU_OPT_BACK) - m->opt_pos, x, W, H, a);
}

void menu_frame(struct menu *m, float lw, float lh, uint64_t now_us)
{
	float dt = m->last_us && now_us > m->last_us ? (float)(now_us - m->last_us) / 1e6f : 0.0f;
	if (dt > 0.1f)
		dt = 0.1f;
	m->last_us = now_us;
	m->t += dt;
	ease(&m->pos, (float)m->sel, dt, 14.0f);
	if (m->opt == MENU_OPT_APPLY && !menu_video_pending(m))
		m->opt = MENU_OPT_BACK;  /* ya se aplicó */
	ease(&m->opt_pos, (float)opt_row(m, m->opt), dt, 14.0f);
	ease(&m->page_t, m->page == MENU_PAGE_OPTIONS ? 1.0f : 0.0f, dt, 10.0f);
	ease(&m->cht_pos, (float)m->cht_sel, dt, 14.0f);
	ease(&m->cht_t, m->page == MENU_PAGE_CHEATS ? 1.0f : 0.0f, dt, 10.0f);
	ease(&m->game_pos, (float)game_row(m, m->game_sel), dt, 14.0f);
	ease(&m->game_t, m->page == MENU_PAGE_GAME ? 1.0f : 0.0f, dt, 10.0f);
	ease(&m->control_pos, (float)m->control_sel, dt, 14.0f);
	ease(&m->control_t, m->page == MENU_PAGE_CONTROLS ? 1.0f : 0.0f, dt, 10.0f);
	ease(&m->cover_t, m->cover_rom >= 0 && m->cover_rom == menu_selected(m) && m->cover_w ? 1.0f : 0.0f, dt, 10.0f);
	m->intro = clampf(m->intro + dt / 0.45f, 0.0f, 1.0f);

	float W = lw, H = lh, a = smooth(m->intro);
	background(m, W, H);

	/* Encabezado. */
	stext(W * 0.06f, H * 0.055f, "Salsa64", 2.5f, 0xFFFFFFu, a, 0);
	stext(W * 0.06f + 7 * 20.0f + 22.0f, H * 0.055f + 8.0f,
	      m->page == MENU_PAGE_GAMES ? "Juegos" : m->page == MENU_PAGE_CHEATS ? "Trucos"
					    : m->page == MENU_PAGE_GAME ? "Ajustes" : m->page == MENU_PAGE_CONTROLS ? "Controles" : "Opciones", 2.0f, C_DIM, a, 0);
	char count[24];
	snprintf(count, sizeof count, tr(m->list->rom_count == 1 ? "%u juego" : "%u juegos"), m->list->rom_count);
	stext(W * 0.94f - cells_w(count, 1.75f), H * 0.055f + 10.0f, count, 1.75f, C_DIM, a, 0);
	/* Versión y paquete, chicos, entre el contador y el panel. */
	const char *ver[2] = { m->version, m->build };
	for (int i = 0; i < 2; i++) {
		if (!ver[i])
			continue;
		float max = W * VER_ROOM, w = rsxkit_ui_text_fit_width(tr(ver[i]), VER_SCALE, max);
		stext(W * 0.94f - w, H * 0.055f + 38.0f + 18.0f * i, ver[i], VER_SCALE, C_DIM, a * 0.85f, max);
	}

	/* Columna (se desliza al cambiar de página) y panel. */
	float x = W * 0.07f, p = smooth(m->page_t), ch = smooth(m->cht_t), gm = smooth(m->game_t);
	float ctls = smooth(m->control_t);
	float g = (1.0f - p) * (1.0f - ch) * (1.0f - gm) * (1.0f - ctls);
	if (g > 0.001f)
		games_column(m, x - p * W * 0.06f - ch * W * 0.06f - gm * W * 0.06f, W, H, a * g);
	if (gm > 0.001f)
		game_column(m, x + (1.0f - gm) * W * 0.06f, W, H, a * gm);
	if (p > 0.001f)
		options_column(m, x + (1.0f - p) * W * 0.06f, W, H, a * p);
	if (ch > 0.001f && m->cheats)
		cheats_column(m, x + (1.0f - ch) * W * 0.06f, W, H, a * ch);
	if (ctls > 0.001f)
		controls_column(m, x + (1.0f - ctls) * W * 0.06f, W, H, a * ctls);
	panel(m, W, H, a);

	/* Pie: mensaje, botones y cómo volver. */
	if (m->status[0])
		ptext(W * 0.06f, H * 0.815f, m->status, 1.6f, C_WARN, a, W * 0.88f);
	ptext(W * 0.06f, H * 0.865f,
	      m->page == MENU_PAGE_CONTROLS ? tr("×  Asignar o elegir   ↑↓  Mover   ○  Volver")
	      : m->page == MENU_PAGE_GAMES ? "×  Abrir   ↑↓  Elegir   □  Trucos   SELECT  Ajustes   △  Opciones"
		      : m->page == MENU_PAGE_GAME ? "×  Cambiar      ↑↓  Elegir      ○  Volver"
	      : m->page == MENU_PAGE_CHEATS ? "×  Activar o desactivar      ↑↓  Elegir      ○  Volver"
					     : "×  Cambiar      ○  Volver",
	      1.6f, C_TEXT, a, 0);
	ptext(W * 0.06f, H * 0.865f + 30.0f, "En el juego, START + SELECT abre el menú de pausa.", 1.35f, C_DIM, a, 0);

	if (m->loading) {
		rsxkit_ui_rect(0, 0, W, H, rgba(0x000000u, 0.55f));
		char line[ROMLIST_PATH_MAX + 32];
		snprintf(line, sizeof line, tr("Cargando %s…"), m->loading);
		float w = rsxkit_ui_text_fit_width(tr(line), 2.5f, W * 0.9f);
		stext((W - w) * 0.5f, H * 0.46f, line, 2.5f, 0xFFFFFFu, 1.0f, W * 0.9f);
	}
}
