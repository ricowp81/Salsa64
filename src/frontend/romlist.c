/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — lista de ROMs para el menú del frontend. Ver romlist.h.
 */
#include "romlist.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "core/rom.h"
#include "ziprom.h"
#include "archiverom.h"
#include "rarrom.h"
#include "sevenziprom.h"
#include "core/save.h"

#define HDR_TITLE   0x20
#define HDR_CODE    0x3B
#define HDR_REGION  0x3E
#define HDR_VERSION 0x3F

struct romlist *romlist_create(void)
{
	struct romlist *l = calloc(1, ROMLIST_LIST_BYTES);
	return l;
}

void romlist_free(struct romlist *l)
{
	free(l);
}

size_t romlist_memory_bytes(void)
{
	return ROMLIST_LIST_BYTES;
}

static char *string_pool(struct romlist *l)
{
	return (char *)(l->e + ROMLIST_MAX);
}

const char *romlist_name(const struct rom_entry *e)
{
	return e->name;
}

const char *romlist_display_title(const struct rom_entry *e)
{
	if (e->kind == ROMLIST_PARENT)
		return "..";
	if (e->kind == ROMLIST_DIRECTORY)
		return romlist_name(e);
	return e->title[0] && e->title_supported ? e->title : romlist_name(e);
}

static int lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c;
}

static int casecmp(const char *a, const char *b)
{
	for (;; a++, b++) {
		int d = lower((unsigned char)*a) - lower((unsigned char)*b);
		if (d || !*a)
			return d;
	}
}

bool romlist_has_rom_ext(const char *name)
{
	if (salsa64_ziprom_is_path(name) || salsa64_rarrom_is_path(name) ||
	    salsa64_7zrom_is_path(name))
		return true;
	size_t n = strlen(name);
	if (n < 5 || name[n - 4] != '.')
		return false;
	const char *x = name + n - 3;
	return (lower(x[0]) == 'z' || lower(x[0]) == 'v' || lower(x[0]) == 'n') && x[1] == '6' && x[2] == '4';
}

void romlist_parse_header(const uint8_t raw[0x40], const char *save_cfg, size_t save_cfg_len, struct rom_entry *e)
{
	e->kind = ROMLIST_ROM;
	uint8_t h[SALSA64_ROM_HEADER_SIZE];
	memcpy(h, raw, sizeof h);
	e->format = (uint8_t)salsa64_rom_normalize(h, sizeof h);

	/* Título: ASCII imprimible; los ceros cuentan como espacio y lo demás
	 * (katakana JIS X 0201 de los títulos japoneses) como '?'. Sin
	 * espacios a los lados. */
	char t[ROMLIST_TITLE_MAX + 1];
	e->title_supported = 1;
	for (unsigned i = 0; i < ROMLIST_TITLE_MAX; i++) {
		uint8_t c = h[HDR_TITLE + i];
		if (c >= 0x80 || (c > 0 && c < 0x20))
			e->title_supported = 0;
		t[i] = c == 0 ? ' ' : c >= 0x20 && c < 0x7F ? (char)c : '?';
	}
	int a = 0, b = ROMLIST_TITLE_MAX;
	while (a < b && t[a] == ' ')
		a++;
	while (b > a && t[b - 1] == ' ')
		b--;
	memcpy(e->title, t + a, (size_t)(b - a));
	e->title[b - a] = '\0';

	/* Código de juego: vacío si la cabecera no trae (homebrew con ceros). */
	bool any = false;
	for (int i = 0; i < 4; i++) {
		uint8_t c = h[HDR_CODE + i];
		e->code[i] = c > 0x20 && c < 0x7F ? (char)c : '?';
		any |= c != 0 && c != ' ';
	}
	e->code[any ? 4 : 0] = '\0';
	e->crc1 = (uint32_t)h[0x10] << 24 | (uint32_t)h[0x11] << 16 | (uint32_t)h[0x12] << 8 | h[0x13];
	e->crc2 = (uint32_t)h[0x14] << 24 | (uint32_t)h[0x15] << 16 | (uint32_t)h[0x16] << 8 | h[0x17];
	e->region = h[HDR_REGION];
	e->version = h[HDR_VERSION];
	e->tv = (uint8_t)salsa64_rom_tv_type(h);

	int manual = SALSA64_SAVE_AUTO;
	if (save_cfg && save_cfg_len) {
		int bad;
		manual = salsa64_save_parse_config(save_cfg, save_cfg_len, h, &bad, NULL, 0);
	}
	enum salsa64_save_source src;
	e->save = (uint8_t)salsa64_save_detect(h, manual, &src);
}


/* ---- Caché persistente de cabeceras -------------------------------------------------
 *
 * Leer la cabecera de cada ROM cuesta ~27 ms en el disco de la PS3 (un stat y
 * un open+read de un archivo grande: dos búsquedas del cabezal por ROM). Se
 * guarda, por archivo, su tamaño, fecha y los 64 bytes crudos de la cabecera;
 * el título, los CRC y el tipo de guardado se siguen calculando al armar la
 * lista (así un cambio de guardado.cfg se respeta sin invalidar nada). Por
 * cada carpeta se guarda además su fecha: con REUSE, si no cambió, la lista
 * sale de la caché sin ni siquiera leer la carpeta.
 * Archivo: cabecera fija + registros + reserva de nombres, con suma de
 * verificación; si algo no cuadra se ignora y se rehace. */
#define CACHE_MAGIC   0x53524C43u /* "SRLC" */
#define CACHE_VERSION 1u
#define CACHE_MAX     8192u
#define CACHE_POOL    (1u << 20)

enum { CK_FILE, CK_DIR, CK_META };

struct crec {
	uint32_t off, size, mtime, gen;
	uint8_t kind, dead, pad[2];
	uint8_t hdr[SALSA64_ROM_HEADER_SIZE];
};

struct cfile {
	uint32_t magic, version, rec_size, count, pool_bytes, sum;
};

static struct {
	char file[ROMLIST_PATH_MAX];
	bool on, loaded, dirty;
	struct crec *rec;
	uint32_t count, pool_used, gen;
	char *pool;
	int32_t *tab; /* tabla hash abierta de índices en rec (−1 = vacío) */
	uint32_t tab_size;
	uint32_t hits, reads, fast_dirs;
} cc;

static uint32_t fnv(const void *p, size_t n, uint32_t h)
{
	const uint8_t *b = p;
	while (n--)
		h = (h ^ *b++) * 16777619u;
	return h;
}

static void cache_free(void)
{
	free(cc.rec);
	free(cc.pool);
	free(cc.tab);
	cc.rec = NULL;
	cc.pool = NULL;
	cc.tab = NULL;
	cc.count = cc.pool_used = 0;
	cc.loaded = false;
}

static bool cache_alloc(void)
{
	cc.rec = calloc(CACHE_MAX, sizeof *cc.rec);
	cc.pool = malloc(CACHE_POOL);
	cc.tab_size = CACHE_MAX * 2;
	cc.tab = malloc(cc.tab_size * sizeof *cc.tab);
	if (!cc.rec || !cc.pool || !cc.tab) {
		cache_free();
		return false;
	}
	memset(cc.tab, 0xFF, cc.tab_size * sizeof *cc.tab);
	return true;
}

static const char *ckey(const struct crec *r)
{
	return cc.pool + r->off;
}

static int cache_find(const char *key, uint8_t kind)
{
	uint32_t i = fnv(key, strlen(key), 2166136261u) % cc.tab_size;
	for (int32_t k; (k = cc.tab[i]) >= 0; i = (i + 1) % cc.tab_size)
		if (cc.rec[k].kind == kind && !strcmp(ckey(&cc.rec[k]), key))
			return k;
	return -1;
}

static void tab_insert(int idx)
{
	const char *key = ckey(&cc.rec[idx]);
	uint32_t i = fnv(key, strlen(key), 2166136261u) % cc.tab_size;
	while (cc.tab[i] >= 0)
		i = (i + 1) % cc.tab_size;
	cc.tab[i] = idx;
}

/* Registro nuevo; NULL si no hay lugar (la lista funciona igual, sin caché). */
static struct crec *cache_add(const char *key, uint8_t kind)
{
	size_t n = strlen(key) + 1;
	if (cc.count >= CACHE_MAX || cc.pool_used + n > CACHE_POOL)
		return NULL;
	struct crec *r = &cc.rec[cc.count];
	memset(r, 0, sizeof *r);
	r->off = cc.pool_used;
	r->kind = kind;
	memcpy(cc.pool + cc.pool_used, key, n);
	cc.pool_used += (uint32_t)n;
	tab_insert((int)cc.count++);
	cc.dirty = true;
	return r;
}

static void cache_load(void)
{
	if (cc.loaded || !cc.on)
		return;
	if (!cache_alloc())
		return;
	cc.loaded = true;
	FILE *f = fopen(cc.file, "rb");
	if (!f)
		return;
	struct cfile h;
	bool ok = fread(&h, sizeof h, 1, f) == 1 && h.magic == CACHE_MAGIC && h.version == CACHE_VERSION &&
		  h.rec_size == sizeof(struct crec) && h.count <= CACHE_MAX && h.pool_bytes <= CACHE_POOL;
	if (ok)
		ok = fread(cc.rec, sizeof *cc.rec, h.count, f) == h.count &&
		     fread(cc.pool, 1, h.pool_bytes, f) == h.pool_bytes;
	fclose(f);
	if (ok) {
		uint32_t sum = fnv(cc.rec, h.count * sizeof *cc.rec, 2166136261u);
		ok = fnv(cc.pool, h.pool_bytes, sum) == h.sum;
	}
	/* Cada nombre debe caer dentro de la reserva y terminar en cero. */
	for (uint32_t i = 0; ok && i < h.count; i++)
		ok = cc.rec[i].off < h.pool_bytes && memchr(cc.pool + cc.rec[i].off, 0, h.pool_bytes - cc.rec[i].off);
	if (!ok) {
		memset(cc.rec, 0, CACHE_MAX * sizeof *cc.rec);
		cc.count = cc.pool_used = 0;
		cc.dirty = true; /* se rehace y se reescribe */
		return;
	}
	cc.count = h.count;
	cc.pool_used = h.pool_bytes;
	for (uint32_t i = 0; i < cc.count; i++)
		tab_insert((int)i);
}

/* Escribe la caché sin los registros muertos (y reconstruye el índice). */
static void cache_flush(void)
{
	if (!cc.on || !cc.loaded || !cc.dirty || !cc.rec)
		return;
	struct crec *nr = malloc(cc.count * sizeof *nr + 1);
	char *np = malloc(cc.pool_used + 1);
	if (!nr || !np) {
		free(nr);
		free(np);
		return;
	}
	uint32_t n = 0, used = 0;
	for (uint32_t i = 0; i < cc.count; i++) {
		if (cc.rec[i].dead)
			continue;
		const char *k = ckey(&cc.rec[i]);
		size_t len = strlen(k) + 1;
		nr[n] = cc.rec[i];
		nr[n].off = used;
		nr[n].gen = 0;
		memcpy(np + used, k, len);
		used += (uint32_t)len;
		n++;
	}
	struct cfile h = { CACHE_MAGIC, CACHE_VERSION, sizeof(struct crec), n, used, 0 };
	h.sum = fnv(np, used, fnv(nr, n * sizeof *nr, 2166136261u));
	FILE *f = fopen(cc.file, "wb");
	bool ok = f && fwrite(&h, sizeof h, 1, f) == 1 && fwrite(nr, sizeof *nr, n, f) == n && fwrite(np, 1, used, f) == used;
	if (f && fclose(f) != 0)
		ok = false;
	if (ok) {
		memcpy(cc.rec, nr, n * sizeof *nr);
		memcpy(cc.pool, np, used);
		cc.count = n;
		cc.pool_used = used;
		memset(cc.tab, 0xFF, cc.tab_size * sizeof *cc.tab);
		for (uint32_t i = 0; i < cc.count; i++)
			tab_insert((int)i);
		cc.dirty = false;
	}
	free(nr);
	free(np);
}

void romlist_cache_release(void)
{
	cache_flush();
	cache_free();
}

void romlist_cache_file(const char *path)
{
	if (path && cc.on && !strcmp(path, cc.file))
		return;
	cache_free();
	cc.on = path && path[0] && strlen(path) < sizeof cc.file;
	cc.dirty = false;
	if (cc.on)
		strcpy(cc.file, path);
}

void romlist_scan_stats(uint32_t *hits, uint32_t *reads, uint32_t *fast_dirs)
{
	if (hits)
		*hits = cc.hits;
	if (reads)
		*reads = cc.reads;
	if (fast_dirs)
		*fast_dirs = cc.fast_dirs;
}

/* ¿`key` es hijo directo de `dir`? */
static bool direct_child(const char *key, const char *dir, size_t dn)
{
	return !strncmp(key, dir, dn) && key[dn] == '/' && !strchr(key + dn + 1, '/');
}

static bool add_name(struct romlist *l, const char *name, struct rom_entry *e)
{
	size_t n = strlen(name);
	if (n > ROMLIST_NAME_MAX || l->count >= ROMLIST_MAX || l->strings_used + n + 1 > ROMLIST_STRING_POOL_MAX)
		return false;
	e->name = string_pool(l) + l->strings_used;
	memcpy((char *)e->name, name, n + 1);
	l->strings_used += (uint32_t)n + 1;
	return true;
}

static const char *base_name(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

/* Fila de ROM a partir de los 64 bytes crudos de su cabecera. */
static bool add_rom_raw(struct romlist *l, const char *name, unsigned source, uint32_t size,
			const uint8_t raw[SALSA64_ROM_HEADER_SIZE], const char *save_cfg, size_t save_cfg_len)
{
	struct rom_entry *e = &l->e[l->count];
	memset(e, 0, sizeof *e);
	if (!add_name(l, name, e)) {
		l->skipped++;
		return false;
	}
	e->kind = ROMLIST_ROM;
	e->source = (uint8_t)source;
	e->size = size;
	romlist_parse_header(raw, save_cfg, save_cfg_len, e);
	l->count++;
	l->rom_count++;
	return true;
}

static bool read_header(const char *path, uint32_t size, uint8_t raw[SALSA64_ROM_HEADER_SIZE])
{
	if (salsa64_rarrom_is_path(path)) {
		uint32_t rom_size = 0;
		return size >= 7u &&
		       salsa64_rarrom_read_header(path, raw, &rom_size) == 0 &&
		       rom_size >= ROMLIST_MIN_SIZE &&
		       rom_size <= SALSA64_ARCHIVEROM_MAX_SIZE;
	}
	if (salsa64_7zrom_is_path(path)) {
		uint32_t rom_size = 0;
		return size >= 32u &&
		       salsa64_7zrom_read_header(path, raw, &rom_size) == 0 &&
		       rom_size >= ROMLIST_MIN_SIZE &&
		       rom_size <= SALSA64_ARCHIVEROM_MAX_SIZE;
	}
	if (salsa64_ziprom_is_path(path)) {
		uint32_t rom_size = 0;
		return size >= SALSA64_ZIPROM_MIN_SIZE &&
		       salsa64_ziprom_read_header(path, raw, &rom_size, NULL, 0) == 0 &&
		       rom_size >= ROMLIST_MIN_SIZE && rom_size <= SALSA64_ZIPROM_MAX_SIZE;
	}
	if (size < ROMLIST_MIN_SIZE)
		return false;
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	bool ok = fread(raw, 1, SALSA64_ROM_HEADER_SIZE, f) == SALSA64_ROM_HEADER_SIZE;
	fclose(f);
	return ok;
}

static bool add_file_size(struct romlist *l, const char *path, unsigned source, uint32_t size, uint32_t mtime,
		  const char *save_cfg, size_t save_cfg_len)
{
	if (l->count >= ROMLIST_MAX) {
		l->skipped++;
		return false;
	}
	uint8_t raw[SALSA64_ROM_HEADER_SIZE];
	struct crec *r = NULL;
	bool hit = false;
	if (cc.on && cc.rec && mtime) {
		int k = cache_find(path, CK_FILE);
		if (k >= 0) {
			r = &cc.rec[k];
			hit = !r->dead && r->size == size && r->mtime == mtime;
			if (hit)
				memcpy(raw, r->hdr, sizeof raw);
		}
	}
	if (hit) {
		cc.hits++;
	} else {
		cc.reads++;
		if (!read_header(path, size, raw)) {
			l->skipped++;
			return false;
		}
		if (cc.on && cc.rec && mtime) {
			if (!r)
				r = cache_add(path, CK_FILE);
			if (r) {
				r->size = size;
				r->mtime = mtime;
				memcpy(r->hdr, raw, sizeof raw);
				r->dead = 0;
				cc.dirty = true;
			}
		}
	}
	if (r)
		r->gen = cc.gen;
	return add_rom_raw(l, base_name(path), source, size, raw, save_cfg, save_cfg_len);
}

bool romlist_add_file(struct romlist *l, const char *path, unsigned source, const char *save_cfg, size_t save_cfg_len)
{
	struct stat st;
	uint32_t min_size = salsa64_ziprom_is_path(path) ? SALSA64_ZIPROM_MIN_SIZE :
		salsa64_rarrom_is_path(path) ? 7u :
		salsa64_7zrom_is_path(path) ? 32u : ROMLIST_MIN_SIZE;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
	    st.st_size < (off_t)min_size ||
	    (uint64_t)st.st_size > 0xFFFFFFFFull) {
		l->skipped++;
		return false;
	}
	return add_file_size(l, path, source, (uint32_t)st.st_size, 0, save_cfg, save_cfg_len);
}

static bool add_directory(struct romlist *l, const char *name, unsigned source)
{
	for (uint32_t i = 0; i < l->count; i++)
		if (l->e[i].kind == ROMLIST_DIRECTORY && !strcmp(romlist_name(&l->e[i]), name))
			return true;
	if (l->count >= ROMLIST_MAX)
		return false;
	struct rom_entry *e = &l->e[l->count];
	memset(e, 0, sizeof *e);
	if (!add_name(l, name, e))
		return false;
	e->kind = ROMLIST_DIRECTORY;
	e->source = (uint8_t)source;
	l->count++;
	l->dir_count++;
	return true;
}

/* Carpetas de datos propios que Salsa64 crea en la raíz de cada ubicación
 * (partidas, estados, carátulas, trucos, perfilado, perfiles, capturas, la
 * caja negra): no son de ROMs y salían como filas vacías. Solo se ocultan en
 * la raíz; si alguien entra a mano a una subcarpeta con ese nombre, se ve. */
static bool internal_dir(const char *name)
{
	static const char *const own[] = { "saves", "states", "covers", "cheats", "perfilado", "perfiles", "capturas" };
	for (unsigned i = 0; i < sizeof own / sizeof own[0]; i++)
		if (!strcmp(name, own[i]))
			return true;
	return !strncmp(name, "cajanegra-", 10);
}

/* «juegos» guarda los .cfg de cada juego; se oculta salvo que alguien haya
 * puesto ROMs ahí (una sola lectura de una carpeta pequeña, solo en la raíz). */
static bool games_dir_without_roms(const char *path)
{
	DIR *d = opendir(path);
	if (!d)
		return true;
	struct dirent *de;
	bool roms = false;
	while (!roms && (de = readdir(d)) != NULL)
		roms = de->d_name[0] != '.' && romlist_has_rom_ext(de->d_name);
	closedir(d);
	return !roms;
}

/* La carpeta sale entera de la caché si su fecha no cambió desde la última
 * lectura completa. */
static bool scan_from_cache(struct romlist *l, const char *dir, unsigned source, uint32_t dir_mtime,
			    const char *save_cfg, size_t save_cfg_len)
{
	char mk[ROMLIST_PATH_MAX + 2];
	if (!dir_mtime || snprintf(mk, sizeof mk, "#%s", dir) >= (int)sizeof mk)
		return false;
	int m = cache_find(mk, CK_META);
	if (m < 0 || cc.rec[m].dead || cc.rec[m].mtime != dir_mtime)
		return false;
	size_t dn = strlen(dir);
	l->skipped += cc.rec[m].size;
	for (uint32_t i = 0; i < cc.count; i++) {
		struct crec *r = &cc.rec[i];
		if (r->dead || r->kind == CK_META || !direct_child(ckey(r), dir, dn))
			continue;
		const char *name = ckey(r) + dn + 1;
		r->gen = cc.gen;
		if (r->kind == CK_DIR) {
			if (!add_directory(l, name, source))
				l->skipped++;
		} else if (l->count >= ROMLIST_MAX) {
			l->skipped++;
		} else {
			cc.hits++;
			add_rom_raw(l, name, source, r->size, r->hdr, save_cfg, save_cfg_len);
		}
	}
	cc.fast_dirs++;
	return true;
}

static bool scan_one(struct romlist *l, const char *dir, unsigned source, bool at_root, unsigned flags,
		     const char *save_cfg, size_t save_cfg_len)
{
	struct stat dst;
	uint32_t dir_mtime = 0;
	bool use_cache = cc.on && cc.rec;
	if (use_cache && stat(dir, &dst) == 0 && S_ISDIR(dst.st_mode))
		dir_mtime = (uint32_t)dst.st_mtime;
	cc.gen++;
	if (use_cache && (flags & ROMLIST_SCAN_REUSE) && scan_from_cache(l, dir, source, dir_mtime, save_cfg, save_cfg_len))
		return true;
	uint32_t skipped0 = l->skipped;
	DIR *d = opendir(dir);
	if (!d)
		return false;
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		const char *name = de->d_name;
		if (name[0] == '.' || !strcmp(name, ".."))
			continue;
		if (at_root && internal_dir(name))
			continue;
		char path[ROMLIST_PATH_MAX];
		int n = snprintf(path, sizeof path, "%s/%s", dir, name);
		if (n < 0 || (size_t)n >= sizeof path) {
			l->skipped++;
			continue;
		}
		struct stat st;
		if (stat(path, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			if (at_root && !strcmp(name, "juegos") && games_dir_without_roms(path))
				continue;
			if (!add_directory(l, name, source)) {
				l->skipped++;
			} else if (use_cache) {
				int k = cache_find(path, CK_DIR);
				struct crec *r = k >= 0 ? &cc.rec[k] : cache_add(path, CK_DIR);
				if (r)
					r->gen = cc.gen, r->dead = 0;
			}
			continue;
		}
		if (!S_ISREG(st.st_mode) || !romlist_has_rom_ext(name))
			continue;
		if (st.st_size < (off_t)(salsa64_ziprom_is_path(name) ? SALSA64_ZIPROM_MIN_SIZE : ROMLIST_MIN_SIZE) ||
		    (uint64_t)st.st_size > 0xFFFFFFFFull) {
			l->skipped++;
			continue;
		}
		add_file_size(l, path, source, (uint32_t)st.st_size, (uint32_t)st.st_mtime, save_cfg, save_cfg_len);
	}
	closedir(d);
	if (use_cache && dir_mtime) {
		/* Lo que ya no está en la carpeta sale de la caché; y se anota su fecha. */
		size_t dn = strlen(dir);
		for (uint32_t i = 0; i < cc.count; i++) {
			struct crec *r = &cc.rec[i];
			if (!r->dead && r->kind != CK_META && r->gen != cc.gen && direct_child(ckey(r), dir, dn)) {
				r->dead = 1;
				cc.dirty = true;
			}
		}
		char mk[ROMLIST_PATH_MAX + 2];
		if (snprintf(mk, sizeof mk, "#%s", dir) < (int)sizeof mk) {
			int k = cache_find(mk, CK_META);
			struct crec *r = k >= 0 ? &cc.rec[k] : cache_add(mk, CK_META);
			if (r && (r->mtime != dir_mtime || r->size != l->skipped - skipped0 || r->dead)) {
				r->mtime = dir_mtime;
				r->size = l->skipped - skipped0;
				r->dead = 0;
				cc.dirty = true;
			}
		}
	}
	return true;
}

int romlist_scan_sources_ex(struct romlist *l, const char *const *roots, unsigned root_count, const char *relative,
		 const char *save_cfg, size_t save_cfg_len, unsigned flags)
{
	cc.hits = cc.reads = cc.fast_dirs = 0;
	cache_load();
	if (!l || !roots || root_count > ROMLIST_SOURCES_MAX || strlen(relative) >= sizeof l->directory)
		return -1;
	l->count = l->rom_count = l->dir_count = l->skipped = l->strings_used = 0;
	strcpy(l->directory, relative);
	if (relative[0] && l->count < ROMLIST_MAX) {
		struct rom_entry *e = &l->e[l->count];
		memset(e, 0, sizeof *e);
		if (add_name(l, "..", e)) {
			e->kind = ROMLIST_PARENT;
			l->count++;
		}
	}
	int opened = 0;
	for (unsigned i = 0; i < root_count; i++) {
		char dir[ROMLIST_PATH_MAX];
		int n = relative[0] ? snprintf(dir, sizeof dir, "%s/%s", roots[i], relative)
				    : snprintf(dir, sizeof dir, "%s", roots[i]);
		if (n < 0 || (size_t)n >= sizeof dir)
			continue;
		if (scan_one(l, dir, i, relative[0] == 0, flags, save_cfg, save_cfg_len))
			opened++;
	}
	cache_flush();
	return opened ? (int)l->rom_count : -1;
}

int romlist_scan_sources(struct romlist *l, const char *const *roots, unsigned root_count, const char *relative,
		 const char *save_cfg, size_t save_cfg_len)
{
	return romlist_scan_sources_ex(l, roots, root_count, relative, save_cfg, save_cfg_len, 0);
}

int romlist_scan_dir(struct romlist *l, const char *dir, const char *root, const char *save_cfg, size_t save_cfg_len)
{
	const char *roots[] = { root };
	char relative[ROMLIST_PATH_MAX] = "";
	size_t rn = strlen(root);
	if (!strncmp(dir, root, rn) && dir[rn] == '/')
		snprintf(relative, sizeof relative, "%s", dir + rn + 1);
	else if (strcmp(dir, root))
		return -1;
	return romlist_scan_sources(l, roots, 1, relative, save_cfg, save_cfg_len);
}

bool romlist_build_path(const struct romlist *l, const struct rom_entry *e, const char *const *roots,
		unsigned root_count, char *path, size_t path_size)
{
	if (!l || !e || !roots || e->source >= root_count || e->kind == ROMLIST_PARENT)
		return false;
	const char *name = romlist_name(e);
	int n = l->directory[0] ? snprintf(path, path_size, "%s/%s/%s", roots[e->source], l->directory, name)
				: snprintf(path, path_size, "%s/%s", roots[e->source], name);
	return n >= 0 && (size_t)n < path_size;
}

static enum romlist_order sort_order;
static int entry_cmp(const void *pa, const void *pb)
{
	const struct rom_entry *a = pa, *b = pb;
	if (a->kind != b->kind) {
		if (a->kind == ROMLIST_PARENT || b->kind == ROMLIST_PARENT)
			return a->kind == ROMLIST_PARENT ? -1 : 1;
		return a->kind == ROMLIST_DIRECTORY ? -1 : 1;
	}
	const char *an = romlist_name(a), *bn = romlist_name(b);
	if (a->kind != ROMLIST_ROM) {
		int d = casecmp(an, bn);
		return d ? d : (int)a->source - (int)b->source;
	}
	if (sort_order == ROMLIST_ORDER_FILENAME) {
		int d = casecmp(an, bn);
		return d ? d : (int)a->source - (int)b->source;
	}
	int d = casecmp(a->title[0] && a->title_supported ? a->title : an,
			b->title[0] && b->title_supported ? b->title : bn);
	if (!d)
		d = casecmp(an, bn);
	return d ? d : (int)a->source - (int)b->source;
}

void romlist_sort(struct romlist *l, enum romlist_order order)
{
	sort_order = order;
	qsort(l->e, l->count, sizeof l->e[0], entry_cmp);
}

int romlist_find(const struct romlist *l, const char *path, const char *const *roots, unsigned root_count)
{
	char candidate[ROMLIST_PATH_MAX];
	for (uint32_t i = 0; i < l->count; i++)
		if (romlist_build_path(l, &l->e[i], roots, root_count, candidate, sizeof candidate) && !strcmp(candidate, path))
			return (int)i;
	return -1;
}

const char *romlist_save_label(uint8_t save)
{
	switch (save) {
	case SALSA64_SAVE_EEPROM_4K:  return "EEPROM 4K";
	case SALSA64_SAVE_EEPROM_16K: return "EEPROM 16K";
	case SALSA64_SAVE_SRAM_256K:  return "SRAM";
	case SALSA64_SAVE_SRAM_768K:  return "SRAM 768K";
	case SALSA64_SAVE_FLASHRAM:   return "FlashRAM";
	case SALSA64_SAVE_SRAM_1M:    return "SRAM 1M";
	default:                      return "—";
	}
}

const char *romlist_region_name(uint8_t region)
{
	/* n64brew.dev, ROM Header: código de destino. */
	switch (region) {
	case 'A': return "Asia";
	case 'B': return "Brasil";
	case 'C': return "China";
	case 'D': return "Alemania";
	case 'E': return "EE. UU.";
	case 'F': return "Francia";
	case 'G': return "Gateway";
	case 'H': return "Países Bajos";
	case 'I': return "Italia";
	case 'J': return "Japón";
	case 'K': return "Corea";
	case 'L': return "Gateway";
	case 'N': return "Canadá";
	case 'P': case 'X': case 'Y': case 'Z': return "Europa";
	case 'S': return "España";
	case 'U': return "Australia";
	case 'W': return "Escandinavia";
	default:  return "?";
	}
}

bool romlist_load_last(const char *file, char *path, size_t size)
{
	FILE *f = fopen(file, "r");
	if (!f)
		return false;
	bool ok = fgets(path, (int)size, f) != NULL;
	fclose(f);
	if (!ok)
		return false;
	path[strcspn(path, "\r\n")] = '\0';
	return path[0] != '\0';
}

bool romlist_save_last(const char *file, const char *path)
{
	FILE *f = fopen(file, "w");
	if (!f)
		return false;
	bool ok = fprintf(f, "%s\n", path) > 0;
	return fclose(f) == 0 && ok;
}
