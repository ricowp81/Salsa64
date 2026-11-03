/* SPDX-License-Identifier: MIT */
/*
 * rsxkit — framebuffer 2D: una imagen de la CPU como textura lineal en la
 * memoria local del RSX, dibujada como un rectángulo escalado.
 *
 * Cada framebuffer tiene dos copias. rsxkit_fb_upload escribe en la que no
 * se usó en el último dibujo; si el RSX aún la lee de un cuadro anterior,
 * espera a su etiqueta (escrita por el backend después de ese dibujo).
 *
 * Los formatos de origen se leen sin convertir: el formato de textura del
 * RSX y su "remap" de canales hacen el trabajo. RGBA5551 es el formato 0x97
 * del RSX (R5G5B5A1; PSL1GHT lo llama GCM_TEXTURE_FORMAT_R5G6B5A1) y
 * RGBA8888 se lee como A8R8G8B8 con los canales rotados.
 *
 * El rectángulo se dibuja en modo inmediato (atributos de vértice en el
 * búfer de comandos), sin búfer de vértices que sincronizar.
 */
#include <string.h>

#include "internal.h"

/* Shaders compilados con cgcomp y embebidos por el Makefile (bin2s). */
extern const u8 rsxkit_blit_vpo[];
extern const u8 rsxkit_blit_fpo[];

#define TEX_ALIGN 128

struct fmt_info {
	u8 gcm;
	u8 bpp;
	u32 remap;
};

#define REMAP(a, r, g, b)                                                                    \
	GCM_TEXTURE_REMAP_MODE(GCM_TEXTURE_REMAP_ORDER_XYXY, a, r, g, b,                      \
			       GCM_TEXTURE_REMAP_TYPE_ONE, GCM_TEXTURE_REMAP_TYPE_REMAP,       \
			       GCM_TEXTURE_REMAP_TYPE_REMAP, GCM_TEXTURE_REMAP_TYPE_REMAP)
/* Con el alfa de la textura (en vez de 1): blits hacia destinos con alfa. */
#define REMAP_ARGB_A                                                                          \
	GCM_TEXTURE_REMAP_MODE(GCM_TEXTURE_REMAP_ORDER_XYXY, GCM_TEXTURE_REMAP_COLOR_A,        \
			       GCM_TEXTURE_REMAP_COLOR_R, GCM_TEXTURE_REMAP_COLOR_G,           \
			       GCM_TEXTURE_REMAP_COLOR_B, GCM_TEXTURE_REMAP_TYPE_REMAP,        \
			       GCM_TEXTURE_REMAP_TYPE_REMAP, GCM_TEXTURE_REMAP_TYPE_REMAP,     \
			       GCM_TEXTURE_REMAP_TYPE_REMAP)
#define REMAP_ARGB REMAP(GCM_TEXTURE_REMAP_COLOR_A, GCM_TEXTURE_REMAP_COLOR_R, \
			 GCM_TEXTURE_REMAP_COLOR_G, GCM_TEXTURE_REMAP_COLOR_B)

static const struct fmt_info formats[RSXKIT_FORMAT_COUNT] = {
	[RSXKIT_ARGB8888] = { GCM_TEXTURE_FORMAT_A8R8G8B8, 4,
	 GCM_TEXTURE_REMAP_MODE(GCM_TEXTURE_REMAP_ORDER_XYXY, GCM_TEXTURE_REMAP_COLOR_A, GCM_TEXTURE_REMAP_COLOR_R,
	 GCM_TEXTURE_REMAP_COLOR_G, GCM_TEXTURE_REMAP_COLOR_B, GCM_TEXTURE_REMAP_TYPE_REMAP, GCM_TEXTURE_REMAP_TYPE_REMAP,
	 GCM_TEXTURE_REMAP_TYPE_REMAP, GCM_TEXTURE_REMAP_TYPE_REMAP) },
	[RSXKIT_RGBA5551] = { GCM_TEXTURE_FORMAT_R5G6B5A1, 2, REMAP_ARGB },
	[RSXKIT_XRGB1555] = { GCM_TEXTURE_FORMAT_A1R5G5B5, 2, REMAP_ARGB },
	[RSXKIT_RGB565]   = { GCM_TEXTURE_FORMAT_R5G6B5, 2, REMAP_ARGB },
	/* Bytes R, G, B, A leídos como A, R, G, B: R está en "A", G en "R"… */
	[RSXKIT_RGBA8888] = { GCM_TEXTURE_FORMAT_A8R8G8B8, 4,
			      REMAP(GCM_TEXTURE_REMAP_COLOR_B, GCM_TEXTURE_REMAP_COLOR_A,
				    GCM_TEXTURE_REMAP_COLOR_R, GCM_TEXTURE_REMAP_COLOR_G) },
	[RSXKIT_XRGB8888] = { GCM_TEXTURE_FORMAT_A8R8G8B8, 4, REMAP_ARGB },
};

void rsxkit_format_gcm(rsxkit_format format, u8 *gcm, u32 *remap)
{
	if ((unsigned)format >= RSXKIT_FORMAT_COUNT)
		format = RSXKIT_XRGB8888;
	*gcm = formats[format].gcm;
	*remap = formats[format].remap;
}

struct rsxkit_fb {
	int used;
	int index;
	u32 max_w, max_h, pitch;
	u8 *tex[2];
	u32 tex_offset[2];
	u32 last_use[2];  /* valor de la etiqueta del último dibujo con esa copia */
	u32 seq;
	int slot;         /* copia con la última imagen subida; -1 = ninguna */
	u32 w, h;
	rsxkit_format format;
};

static struct rsxkit_fb pool[RSXKIT_FB_MAX];

static int blit_alpha;

void rsxkit_blit_alpha(int on)
{
	blit_alpha = on;
}

static struct {
	rsxVertexProgram *vp;
	rsxFragmentProgram *fp;
	void *vp_ucode;
	u32 fp_offset;
	u32 tex_unit;
	int ready;
} prog;

static int init_programs(void)
{
	u32 vp_size;

	if (prog.ready)
		return RSXKIT_OK;
	prog.vp = (rsxVertexProgram *)rsxkit_blit_vpo;
	prog.fp = (rsxFragmentProgram *)rsxkit_blit_fpo;
	rsxVertexProgramGetUCode(prog.vp, &prog.vp_ucode, &vp_size);
	/* El RSX lee el fragment program desde su memoria. */
	if (!rsxkit_fp_upload(prog.fp, &prog.fp_offset))
		return RSXKIT_ENOMEM;
	rsxProgramAttrib *tex = rsxFragmentProgramGetAttrib(prog.fp, "texture");
	prog.tex_unit = tex ? tex->index : 0;
	prog.ready = 1;
	return RSXKIT_OK;
}

/* El 3D cambia el programa: el del blit se vuelve a cargar siempre. */

rsxkit_fb *rsxkit_fb_create(uint32_t max_width, uint32_t max_height)
{
	if (!max_width || !max_height || max_width > 4096 || max_height > 4096 || init_programs() != RSXKIT_OK)
		return NULL;
	for (int i = 0; i < RSXKIT_FB_MAX; i++) {
		struct rsxkit_fb *fb = &pool[i];
		if (fb->used)
			continue;
		memset(fb, 0, sizeof *fb);
		fb->index = i;
		fb->max_w = max_width;
		fb->max_h = max_height;
		fb->pitch = (max_width * 4 + 63) & ~63u;
		for (int s = 0; s < 2; s++) {
			fb->tex[s] = rsxMemalign(TEX_ALIGN, fb->pitch * max_height);
			if (!fb->tex[s]) {
				if (s)
					rsxFree(fb->tex[0]);
				return NULL;
			}
			rsxAddressToOffset(fb->tex[s], &fb->tex_offset[s]);
			*(volatile u32 *)gcmGetLabelAddress(RSXKIT_LABEL_FB(i, s)) = 0;
		}
		fb->slot = -1;
		fb->used = 1;
		return fb;
	}
	return NULL;
}

void rsxkit_fb_destroy(rsxkit_fb *fb)
{
	if (!fb || !fb->used)
		return;
	for (int s = 0; s < 2; s++) {
		rsxkit_wait_label(RSXKIT_LABEL_FB(fb->index, s), fb->last_use[s]);
		rsxFree(fb->tex[s]);
	}
	fb->used = 0;
}

void rsxkit_fb_shutdown(void)
{
	for (int i = 0; i < RSXKIT_FB_MAX; i++)
		if (pool[i].used) {
			rsxFree(pool[i].tex[0]);
			rsxFree(pool[i].tex[1]);
			pool[i].used = 0;
		}
	prog.ready = 0;
}

int rsxkit_fb_upload(rsxkit_fb *fb, const void *pixels, uint32_t width, uint32_t height,
		     uint32_t pitch, rsxkit_format format)
{
	if (!fb || !fb->used || (unsigned)format >= RSXKIT_FORMAT_COUNT || !width || !height ||
	    width > fb->max_w || height > fb->max_h || pitch < width * formats[format].bpp)
		return RSXKIT_EARG;

	int s = fb->slot == 0 ? 1 : 0;
	rsxkit_wait_label(RSXKIT_LABEL_FB(fb->index, s), fb->last_use[s]);

	const u8 *src = pixels;
	u8 *dst = fb->tex[s];
	u32 row = width * formats[format].bpp;
	for (u32 y = 0; y < height; y++)
		memcpy(dst + y * fb->pitch, src + y * pitch, row);

	fb->slot = s;
	fb->w = width;
	fb->h = height;
	fb->format = format;
	return RSXKIT_OK;
}

rsxkit_rect rsxkit_layout(uint32_t width, uint32_t height, const rsxkit_view *view)
{
	const rsxkit_screen *sc = &rsxkit_g.screen;
	rsxkit_rect r = { 0, 0, sc->width, sc->height };
	if (!width || !height || !view || view->scale == RSXKIT_SCALE_STRETCH)
		return r;

	/* Píxeles de la superficie: en 480/576 no son cuadrados. */
	float px_aspect = sc->aspect * sc->height / sc->width;
	float img = view->aspect > 0.0f ? view->aspect : (float)width / height;

	if (view->scale == RSXKIT_SCALE_INTEGER) {
		uint32_t k = sc->height / height;
		if (k == 0)
			k = 1;
		r.h = height * k;
		r.w = (uint32_t)(r.h * img / px_aspect + 0.5f);
		if (r.w > sc->width)
			r.w = sc->width;
	} else {
		r.w = (uint32_t)(sc->height * img / px_aspect + 0.5f);
		if (r.w <= sc->width) {
			r.h = sc->height;
		} else {
			r.w = sc->width;
			r.h = (uint32_t)(sc->width * px_aspect / img + 0.5f);
		}
	}
	r.x = (int32_t)(sc->width - r.w) / 2;
	r.y = (int32_t)(sc->height - r.h) / 2;
	return r;
}

bool rsxkit_fb_texture(const rsxkit_fb *fb, rsxkit_texture *out)
{
	memset(out, 0, sizeof *out);
	if (!fb || !fb->used || fb->slot < 0)
		return false;
	out->offset = fb->tex_offset[fb->slot];
	out->pitch = fb->pitch;
	out->width = (uint16_t)fb->w;
	out->height = (uint16_t)fb->h;
	out->location = RSXKIT_MEM_LOCAL;
	out->format = (uint8_t)fb->format;
	return true;
}

int rsxkit_blit_init(void)
{
	return init_programs();
}

static void vertex(gcmContextData *ctx, float x, float y, float u, float v, uint32_t sw, uint32_t sh)
{
	f32 uv[2] = { u, v };
	f32 pos[4] = { 2.0f * x / sw - 1.0f, 1.0f - 2.0f * y / sh, 0.0f, 1.0f };
	rsxDrawVertex2f(ctx, GCM_VERTEX_ATTRIB_TEX0, uv);
	rsxDrawVertex4f(ctx, GCM_VERTEX_ATTRIB_POS, pos);  /* la posición emite el vértice */
}

void rsxkit_fb_draw(rsxkit_fb *fb, const rsxkit_view *view, rsxkit_rect *out)
{
	rsxkit_screen_texture_invalidate();
	if (!fb || !fb->used || fb->slot < 0)
		return;
	static const rsxkit_view def = { RSXKIT_SCALE_FIT, RSXKIT_FILTER_LINEAR, 0.0f };
	if (!view)
		view = &def;
	rsxkit_rect r = rsxkit_layout(fb->w, fb->h, view);
	if (out)
		*out = r;

	int s = fb->slot;
	rsxkit_blit(fb->tex_offset[s], fb->pitch, fb->w, fb->h, 0.0f, 0.0f, 1.0f, 1.0f, fb->format, &r, view->filter);
	fb->last_use[s] = ++fb->seq;
	rsxSetWriteBackendLabel(rsxkit_g.ctx, RSXKIT_LABEL_FB(fb->index, s), fb->seq);
}

void rsxkit_blit(uint32_t offset, uint32_t pitch, uint32_t tex_w, uint32_t tex_h, float u0, float v0, float u1,
		 float v1, rsxkit_format format, const rsxkit_rect *rp, rsxkit_filter filter)
{
	if (init_programs() != RSXKIT_OK)
		return;
	rsxkit_reserve(RSXKIT_RESERVE_DRAW);
	rsxkit_leave_3d();
	if (!rsxkit_g.screen_bound)
		rsxkit_bind_screen();
	rsxkit_blit_to(offset, RSXKIT_MEM_LOCAL, pitch, tex_w, tex_h, u0, v0, u1, v1, format, rp, rsxkit_g.screen.width,
		       rsxkit_g.screen.height, filter);
}

void rsxkit_blit_to(uint32_t offset, uint8_t location, uint32_t pitch, uint32_t tex_w, uint32_t tex_h, float u0,
		    float v0, float u1, float v1, rsxkit_format format, const rsxkit_rect *rp, uint32_t surf_w,
		    uint32_t surf_h, rsxkit_filter filter)
{
	gcmContextData *ctx = rsxkit_g.ctx;
	if (init_programs() != RSXKIT_OK)
		return;
	rsxkit_trace_repeat("blit", "%u en total; el último de %u×%u a %u de ancho", 1, tex_w, tex_h, rp->w);
	rsxkit_reserve(RSXKIT_RESERVE_DRAW);
	rsxkit_rect r = *rp;
	const struct fmt_info *fi = &formats[format];
	gcmTexture tex;
	memset(&tex, 0, sizeof tex);
	tex.format = fi->gcm | GCM_TEXTURE_FORMAT_LIN | GCM_TEXTURE_FORMAT_NRM;
	tex.mipmap = 1;
	tex.dimension = GCM_TEXTURE_DIMS_2D;
	tex.cubemap = GCM_FALSE;
	tex.remap = fi->remap;
	/* El fragment program escribe el alfa que trae la textura: 1 con el
	 * remapeo de la tabla (salvo RSXKIT_ARGB8888) y el suyo hacia un destino
	 * con alfa (rsxkit_blit_alpha), donde XRGB8888 y RGBA5551 llevan el suyo. */
	if (blit_alpha && (format == RSXKIT_XRGB8888 || format == RSXKIT_RGBA5551))
		tex.remap = REMAP_ARGB_A;
	tex.width = tex_w;
	tex.height = tex_h;
	tex.depth = 1;
	tex.location = location == RSXKIT_MEM_MAIN ? GCM_LOCATION_CELL : GCM_LOCATION_RSX;
	tex.pitch = pitch;
	tex.offset = offset;

	u8 filt = filter == RSXKIT_FILTER_NEAREST ? GCM_TEXTURE_NEAREST : GCM_TEXTURE_LINEAR;
	u8 unit = (u8)prog.tex_unit;
	rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);
	rsxLoadTexture(ctx, unit, &tex);
	rsxTextureControl(ctx, unit, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
	rsxTextureFilter(ctx, unit, 0, filt, filt, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	rsxTextureWrapMode(ctx, unit, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE,
			   GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);

	rsxLoadVertexProgram(ctx, prog.vp, prog.vp_ucode);
	rsxLoadFragmentProgramLocation(ctx, prog.fp, prog.fp_offset, GCM_LOCATION_RSX);

	float x0 = (float)r.x, y0 = (float)r.y, x1 = x0 + r.w, y1 = y0 + r.h;
	rsxDrawVertexBegin(ctx, GCM_TYPE_TRIANGLE_STRIP);
	vertex(ctx, x0, y0, u0, v0, surf_w, surf_h);
	vertex(ctx, x1, y0, u1, v0, surf_w, surf_h);
	vertex(ctx, x0, y1, u0, v1, surf_w, surf_h);
	vertex(ctx, x1, y1, u1, v1, surf_w, surf_h);
	rsxDrawVertexEnd(ctx);
}
