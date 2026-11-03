/* SPDX-License-Identifier: MIT */
/*
 * rsxkit — texto con una fuente de mapa de bits sobre un lienzo en memoria
 * principal (rsxkit/text.h). Sin RSX ni PSL1GHT: compila en cualquier host.
 */
#include <stddef.h>
#include <stdint.h>

#include "rsxkit/text.h"

#include "font.h"
#include "font_8x13.h"

_Static_assert(FONT_W == RSXKIT_FONT_W && FONT_H == RSXKIT_FONT_H && FONT_ASCENT == RSXKIT_FONT_ASCENT,
	       "la tabla de glifos no es la de la cabecera");

#define REPLACEMENT 0xFFFDu
#define ELLIPSIS    0x2026u

static uint32_t bytes_per_pixel(rsxkit_format format)
{
	return format == RSXKIT_ARGB8888 || format == RSXKIT_RGBA8888 || format == RSXKIT_XRGB8888 ? 4 : 2;
}

uint32_t rsxkit_color(rsxkit_format format, uint32_t rgb)
{
	uint32_t r = rgb >> 16 & 0xFF, g = rgb >> 8 & 0xFF, b = rgb & 0xFF;
	switch (format) {
	case RSXKIT_RGBA5551:
		return (r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | 1;
	case RSXKIT_XRGB1555:
		return 1u << 15 | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3);
	case RSXKIT_RGB565:
		return (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3);
	case RSXKIT_RGBA8888:
		return r << 24 | g << 16 | b << 8 | 0xFF;
	default:  /* RSXKIT_XRGB8888 */
		return 0xFFu << 24 | r << 16 | g << 8 | b;
	}
}

/* Una fila de n píxeles de un color ya convertido, en big-endian. */
static void span(uint8_t *p, uint32_t n, uint32_t bpp, uint32_t v)
{
	if (bpp == 4) {
		for (uint32_t i = 0; i < n; i++, p += 4) {
			p[0] = (uint8_t)(v >> 24);
			p[1] = (uint8_t)(v >> 16);
			p[2] = (uint8_t)(v >> 8);
			p[3] = (uint8_t)v;
		}
	} else {
		for (uint32_t i = 0; i < n; i++, p += 2) {
			p[0] = (uint8_t)(v >> 8);
			p[1] = (uint8_t)v;
		}
	}
}

/* Rectángulo recortado al lienzo, con el color ya convertido. */
static void fill_packed(const rsxkit_canvas *c, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t v)
{
	if (!c || !c->pixels || w <= 0 || h <= 0)
		return;
	int64_t x0 = x, y0 = y, x1 = (int64_t)x + w, y1 = (int64_t)y + h;
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > (int64_t)c->width)
		x1 = c->width;
	if (y1 > (int64_t)c->height)
		y1 = c->height;
	if (x0 >= x1 || y0 >= y1)
		return;
	uint32_t bpp = bytes_per_pixel(c->format);
	uint8_t *row = (uint8_t *)c->pixels + (size_t)y0 * c->pitch + (size_t)x0 * bpp;
	for (int64_t yy = y0; yy < y1; yy++, row += c->pitch)
		span(row, (uint32_t)(x1 - x0), bpp, v);
}

void rsxkit_fill(const rsxkit_canvas *c, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgb)
{
	if (c)
		fill_packed(c, x, y, w, h, rsxkit_color(c->format, rgb));
}

uint32_t rsxkit_utf8_next(const char **s)
{
	const uint8_t *p = (const uint8_t *)*s;
	uint32_t c = p[0];
	unsigned n;
	uint32_t min;
	if (c < 0x80) {
		*s += 1;
		return c;
	} else if ((c & 0xE0) == 0xC0) {
		n = 1, min = 0x80, c &= 0x1F;
	} else if ((c & 0xF0) == 0xE0) {
		n = 2, min = 0x800, c &= 0x0F;
	} else if ((c & 0xF8) == 0xF0) {
		n = 3, min = 0x10000, c &= 0x07;
	} else {
		*s += 1;
		return REPLACEMENT;
	}
	for (unsigned i = 1; i <= n; i++) {
		if ((p[i] & 0xC0) != 0x80) {
			*s += 1;
			return REPLACEMENT;
		}
		c = c << 6 | (p[i] & 0x3F);
	}
	*s += 1 + n;
	return c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF) ? REPLACEMENT : c;
}

uint32_t rsxkit_text_cells(const char *utf8)
{
	uint32_t n = 0;
	if (utf8)
		while (*utf8) {
			rsxkit_utf8_next(&utf8);
			n++;
		}
	return n;
}

static int find_glyph(uint32_t cp)
{
	int lo = 0, hi = FONT_GLYPHS - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (font_cp[mid] == cp)
			return mid;
		if (font_cp[mid] < cp)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return -1;
}

int rsxkit_font_has(uint32_t codepoint)
{
	return find_glyph(codepoint) >= 0;
}

int rsxkit_glyph_total(void)
{
	return FONT_GLYPHS;
}

int rsxkit_glyph_index(uint32_t cp)
{
	int g = find_glyph(cp);
	return g >= 0 ? g : find_glyph('?');
}

const uint8_t *rsxkit_glyph_rows(int index)
{
	return font_rows[index >= 0 && index < FONT_GLYPHS ? index : 0];
}

static void glyph(const rsxkit_canvas *c, int32_t x, int32_t y, uint32_t cp, uint32_t v, uint32_t k)
{
	const uint8_t *rows = rsxkit_glyph_rows(rsxkit_glyph_index(cp));
	for (int row = 0; row < FONT_H; row++) {
		uint8_t bits = rows[row];
		for (int col = 0; bits; col++, bits = (uint8_t)(bits << 1))
			if (bits & 0x80)
				fill_packed(c, x + col * (int32_t)k, y + row * (int32_t)k, (int32_t)k, (int32_t)k, v);
	}
}

uint32_t rsxkit_text_draw(const rsxkit_canvas *c, int32_t x, int32_t y, const char *utf8, uint32_t max_cells,
			  uint32_t rgb, uint32_t scale)
{
	if (!c || !c->pixels || !utf8)
		return 0;
	uint32_t k = scale ? scale : 1;
	uint32_t v = rsxkit_color(c->format, rgb);
	uint32_t total = max_cells ? rsxkit_text_cells(utf8) : 0;
	uint32_t n = 0;
	while (*utf8) {
		uint32_t cp = rsxkit_utf8_next(&utf8);
		if (max_cells && total > max_cells && n == max_cells - 1)
			cp = ELLIPSIS;
		glyph(c, x + (int32_t)(n * FONT_W * k), y, cp, v, k);
		if (++n == max_cells)
			break;
	}
	return n;
}
