/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — E/S y compresión de los trabajos en segundo plano. Ver bgio.h.
 */
#include "platform/ps3/bgio.h"

#include <malloc.h>
#include <spukit/lzstream.h>
#include <spukit/spukit.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/systime.h>

#include "platform/ps3/log.h"
#include "spu_lz_bin.h"

/* ---- Archivo de salida ---------------------------------------------------------- */

/* Las escrituras se juntan en un búfer: el flujo comprimido llega en
 * pedazos de 4 bytes (encabezados) y de hasta 64 KB. */
#define WBUF_BYTES (128u << 10)
static uint8_t wbuf[WBUF_BYTES] __attribute__((aligned(128)));

bool bgio_out_open(struct bgio_out *o, const char *path)
{
	memset(o, 0, sizeof *o);
	o->fd = -1;
	s32 fd = -1;
	if (sysLv2FsOpen(path, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &fd, 0666, NULL, 0) != 0)
		return false;
	o->fd = fd;
	return true;
}

static bool out_flush(struct bgio_out *o)
{
	if (!o->fill)
		return true;
	uint64_t t0 = sysGetSystemTime(), done = 0;
	bool ok = sysLv2FsWrite(o->fd, wbuf, o->fill, &done) == 0 && done == o->fill;
	o->us += sysGetSystemTime() - t0;
	o->fill = 0;
	return ok;
}

bool bgio_out_write(void *ctx, const void *data, uint32_t len)
{
	struct bgio_out *o = ctx;
	const uint8_t *p = data;
	o->bytes += len;
	while (len) {
		if (!o->fill && len >= WBUF_BYTES / 2) {
			/* Grande: directo, sin pasar por el búfer. */
			uint64_t t0 = sysGetSystemTime(), done = 0;
			bool ok = sysLv2FsWrite(o->fd, p, len, &done) == 0 && done == len;
			o->us += sysGetSystemTime() - t0;
			return ok;
		}
		uint32_t n = WBUF_BYTES - o->fill < len ? WBUF_BYTES - o->fill : len;
		memcpy(wbuf + o->fill, p, n);
		o->fill += n;
		p += n;
		len -= n;
		if (o->fill == WBUF_BYTES && !out_flush(o))
			return false;
	}
	return true;
}

bool bgio_out_sync(struct bgio_out *o)
{
	if (o->fd < 0 || !out_flush(o))
		return false;
	uint64_t t0 = sysGetSystemTime();
	bool ok = sysLv2FsFsync(o->fd) == 0;
	o->us += sysGetSystemTime() - t0;
	return ok;
}

bool bgio_out_patch(void *ctx, uint32_t offset, const uint8_t *data, uint32_t len)
{
	struct bgio_out *o = ctx;
	if (!out_flush(o))
		return false;
	uint64_t t0 = sysGetSystemTime(), pos = 0, done = 0;
	bool ok = sysLv2FsLSeek64(o->fd, offset, 0, &pos) == 0 && pos == offset &&
		  sysLv2FsWrite(o->fd, data, len, &done) == 0 && done == len;
	o->us += sysGetSystemTime() - t0;
	return ok;
}

bool bgio_out_finish(struct bgio_out *o, bool ok, const char *tmp, const char *path, char *err, size_t n)
{
	if (o->fd >= 0) {
		if (ok && !out_flush(o)) {
			snprintf(err, n, "no se pudo escribir el archivo");
			ok = false;
		}
		uint64_t tc = sysGetSystemTime();
		if (sysLv2FsClose(o->fd) != 0 && ok) {
			snprintf(err, n, "no se pudo cerrar el archivo");
			ok = false;
		}
		o->us += sysGetSystemTime() - tc;
		o->fd = -1;
	} else if (ok) {
		snprintf(err, n, "no se pudo crear %s", tmp);
		ok = false;
	}
	if (ok) {
		uint64_t tr = sysGetSystemTime();
		sysLv2FsChmod(tmp, 0666);
		if (sysLv2FsRename(tmp, path) != 0) {
			/* Sin reemplazo atómico: quitar el viejo y volver a intentar. */
			sysLv2FsUnlink(path);
			if (sysLv2FsRename(tmp, path) != 0) {
				snprintf(err, n, "no se pudo renombrar %s", tmp);
				ok = false;
			}
		}
		o->us += sysGetSystemTime() - tr;
	}
	if (!ok)
		sysLv2FsUnlink(tmp);
	return ok;
}

/* ---- Compresión ----------------------------------------------------------------- */

static struct {
	spukit_lzs *z;
	bool tried;
	struct bgio_lz_stats st;
} lz;

/* Un sumidero que junta lo que llega en un búfer. */
struct col {
	uint8_t *at;
	uint32_t n, cap;
};

static int col_sink(void *ctx, const void *d, uint32_t len)
{
	struct col *c = ctx;
	if (len > c->cap - c->n)
		return -1;
	memcpy(c->at + c->n, d, len);
	c->n += len;
	return 0;
}

/* Bloques de prueba del compresor: lo que da el SPE tiene que ser, byte a
 * byte, lo que da el PPE, o se comprime en el PPE. */
static bool lz_selftest(spukit_lzs *z)
{
	enum { LEN = SPUKIT_LZ_BLOCK + 30000, OUT = 2 * (SPUKIT_LZ_BOUND(SPUKIT_LZ_BLOCK) + 64) };
	uint8_t *buf = memalign(128, ((LEN + 127) & ~127u) + 2 * OUT);
	if (!buf)
		return false;
	uint8_t *got = buf + ((LEN + 127) & ~127u), *ref = got + OUT;
	uint32_t x = 12345;
	for (uint32_t i = 0; i < LEN; i++) {
		x = x * 1664525u + 1013904223u;
		buf[i] = (i / 4096) % 3 == 0 ? (uint8_t)(x >> 16) : (i / 4096) % 3 == 1 ? 0 : (uint8_t)("salsa64"[i % 7] + (x >> 29));
	}
	struct col col = { got, 0, OUT };
	bool ok = spukit_lzs_run(z, buf, LEN, col_sink, &col, NULL) == 0;
	/* Referencia: los mismos bloques en el PPE. */
	uint32_t rn = 0;
	for (uint32_t off = 0; ok && off < LEN; off += SPUKIT_LZ_BLOCK) {
		uint32_t n = LEN - off < SPUKIT_LZ_BLOCK ? LEN - off : SPUKIT_LZ_BLOCK;
		uint32_t c = spukit_lzs_ppe_block(z, buf + off, n, ref + rn + 4);
		spukit_lz_frame(ref + rn, c ? c : n, c == 0);
		if (!c)
			memcpy(ref + rn + 4, buf + off, n);
		rn += 4 + (c ? c : n);
	}
	ok = ok && rn == col.n && memcmp(got, ref, rn) == 0;
	free(buf);
	return ok;
}

static void lz_open(void)
{
	if (lz.tried)
		return;
	lz.tried = true;
	uint64_t t0 = sysGetSystemTime();
	spukit_init(6);
	spukit_lzs *z = spukit_lzs_open(spu_lz_bin);
	const char *how = "en un SPE";
	if (z && spukit_lzs_has_spu(z) && !lz_selftest(z)) {
		salsa64_log("bgio: el compresor del SPE no da lo mismo que el del PPE: se comprime en el PPE");
		spukit_lzs_close(z);
		z = NULL;
	}
	if (z && !spukit_lzs_has_spu(z))
		how = "en el PPE (el SPE no arrancó)";
	if (!z) {
		z = spukit_lzs_open(NULL);
		how = "en el PPE";
	}
	lz.z = z;
	salsa64_log("bgio: compresor LZ %s (%.1f ms con la prueba)", how, (sysGetSystemTime() - t0) / 1e3);
}

struct lz_cb {
	bool (*write)(void *, const void *, uint32_t);
	void *wctx;
};

static int lz_sink(void *ctx, const void *d, uint32_t len)
{
	struct lz_cb *c = ctx;
	return c->write(c->wctx, d, len) ? 0 : -1;
}

bool bgio_compress(void *cctx, const uint8_t *src, uint32_t len, bool (*write)(void *, const void *, uint32_t),
		   void *wctx)
{
	(void)cctx;
	lz_open();
	if (!lz.z)
		return false;
	struct lz_cb c = { write, wctx };
	spukit_lzs_stats st;
	bool ok = spukit_lzs_run(lz.z, src, len, lz_sink, &c, &st) == 0;
	uint64_t hz = spukit_tick_hz();
	lz.st.blocks += st.blocks;
	lz.st.stored += st.stored;
	lz.st.spu_blocks += st.spu_blocks;
	lz.st.ppe_blocks += st.ppe_blocks;
	lz.st.in_bytes += st.in_bytes;
	lz.st.out_bytes += st.out_bytes;
	lz.st.spu_us += hz ? st.spu_ticks * 1000000ull / hz : 0;
	lz.st.sink_us += st.sink_us;
	lz.st.spu = spukit_lzs_has_spu(lz.z);
	return ok;
}

void bgio_lz_reset(void)
{
	memset(&lz.st, 0, sizeof lz.st);
}

void bgio_lz_get(struct bgio_lz_stats *out)
{
	*out = lz.st;
}

void bgio_shutdown(void)
{
	if (lz.z) {
		spukit_lzs_close(lz.z);
		lz.z = NULL;
	}
	lz.tried = false;
}
