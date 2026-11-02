/* SPDX-License-Identifier: MIT */
/*
 * rsxkit — destinos de render, memoria principal mapeada y barreras.
 *
 * Un destino es una superficie lineal en la memoria local del RSX que
 * después se puede leer como textura para presentarla escalada (como el
 * framebuffer 2D). Es X8R8G8B8, o A8R8G8B8 si se creó con
 * RSXKIT_TARGET_ALPHA (el alfa va en el mismo byte: una textura leída del
 * destino lo trae; ver rsxkit.h). La profundidad va aparte (Z24S8) para que varios
 * destinos compartan una, como hacen las consolas con su z-buffer.
 */
#include <malloc.h>
#include <string.h>

#include "internal.h"
#include "sifm.h"

/* Superficies y profundidades vivas a la vez. Un emulador de N64 en una
 * pausa de Zelda tiene 9 color images vivas más un scratch; con 8, la
 * novena no se creaba y lo dibujado en ella se perdía. */
#define TARGET_MAX 16
/* 128: la presentación lee el destino como textura desde una fila
 * cualquiera, y el offset de una textura va alineado a 128. */
#define SURF_ALIGN 128

struct rsxkit_target {
	int used;
	int alpha;  /* A8R8G8B8 (RSXKIT_TARGET_ALPHA) */
	uint32_t w, h, pitch, offset;
	void *mem;
};

struct rsxkit_depth {
	int used;
	uint32_t w, h, pitch, offset;
	void *mem;
};

static struct rsxkit_target targets[TARGET_MAX];
static struct rsxkit_depth depths[TARGET_MAX];

/* Destino ligado (draw3d.c lo usa para el viewport y las tijeras). */
struct rsxkit_bound rsxkit_bound;

/* Lo mapeado con rsxkit_map_main, para revisarlo (rsxkit_main_maps_bad). */
#define MAPS_MAX 16
static struct {
	void *ea;
	uint32_t size, offset;
} maps[MAPS_MAX];
static unsigned nmaps;

int rsxkit_map_main(void *ptr, uint32_t size, uint32_t *offset)
{
	if (!ptr || ((uintptr_t)ptr & 0xFFFFF) || (size & 0xFFFFF) || !size)
		return RSXKIT_EARG;
	if (gcmMapMainMemory(ptr, size, offset) != 0)
		return RSXKIT_ENOMEM;
	if (nmaps < MAPS_MAX) {
		maps[nmaps].ea = ptr;
		maps[nmaps].size = size;
		maps[nmaps].offset = *offset;
		nmaps++;
	}
	return RSXKIT_OK;
}

int rsxkit_unmap_main(void *ptr)
{
	for (unsigned i = 0; i < nmaps; i++)
		if (maps[i].ea == ptr) {
			/* Que el RSX termine lo que pudiera leer de ahí. */
			rsxkit_finish();
			if (gcmUnmapEaIoAddress(ptr) != 0)
				return RSXKIT_EARG;
			maps[i] = maps[--nmaps];
			return RSXKIT_OK;
		}
	return RSXKIT_EARG;
}

int rsxkit_main_maps_bad(void)
{
	int bad = 0;
	for (unsigned i = 0; i < nmaps; i++) {
		void *ea = NULL;
		u32 off = 0;
		if (gcmIoOffsetToAddress(maps[i].offset, &ea) != 0 || ea != maps[i].ea ||
		    rsxAddressToOffset(maps[i].ea, &off) != 0 || off != maps[i].offset)
			bad++;
	}
	return bad;
}

uint32_t rsxkit_fence(void)
{
	struct rsxkit_state *g = &rsxkit_g;
	uint32_t v = ++g->fence_seq;
	if (rsxkit_lost())
		return v;  /* abandonado (rsxkit_abandon): sin vaciar; nadie la espera */
	rsxkit_trace_note("barrera", "%u", v, 0, 0, 0);
	rsxkit_reserve(16);
	rsxSetWriteBackendLabel(g->ctx, RSXKIT_LABEL_FENCE, v);
	rsxFlushBuffer(g->ctx);
	return v;
}

int rsxkit_fence_done(uint32_t fence)
{
	volatile uint32_t *label = gcmGetLabelAddress(RSXKIT_LABEL_FENCE);
	return (int32_t)(*label - fence) >= 0;
}

void rsxkit_fence_wait(uint32_t fence)
{
	rsxkit_wait_label(RSXKIT_LABEL_FENCE, fence);
}

static void *surface_alloc(uint32_t w, uint32_t h, uint32_t *pitch, uint32_t *offset)
{
	*pitch = (w * 4 + SURF_ALIGN - 1) & ~(SURF_ALIGN - 1);
	void *mem = rsxMemalign(SURF_ALIGN, *pitch * h);
	if (mem)
		rsxAddressToOffset(mem, offset);
	return mem;
}

rsxkit_target *rsxkit_target_create(uint32_t width, uint32_t height)
{
	return rsxkit_target_create_ex(width, height, 0);
}

rsxkit_target *rsxkit_target_create_ex(uint32_t width, uint32_t height, uint32_t flags)
{
	if (!width || !height || width > 4096 || height > 4096)
		return NULL;
	for (int i = 0; i < TARGET_MAX; i++) {
		struct rsxkit_target *t = &targets[i];
		if (t->used)
			continue;
		memset(t, 0, sizeof *t);
		t->mem = surface_alloc(width, height, &t->pitch, &t->offset);
		if (!t->mem)
			return NULL;
		memset(t->mem, 0, t->pitch * height);
		t->alpha = (flags & RSXKIT_TARGET_ALPHA) != 0;
		t->w = width;
		t->h = height;
		t->used = 1;
		rsxkit_trace_note("destino", "%u×%u pitch %u en 0x%x", width, height, t->pitch, t->offset);
		return t;
	}
	return NULL;
}

void rsxkit_target_destroy(rsxkit_target *t)
{
	if (!t || !t->used)
		return;
	rsxkit_trace_note("destino suelto", "%u×%u en 0x%x", t->w, t->h, t->offset, 0);
	rsxkit_finish();
	rsxFree(t->mem);
	t->used = 0;
	if (rsxkit_bound.target == t)
		rsxkit_bound.target = NULL;
}

int rsxkit_target_has_alpha(const rsxkit_target *t)
{
	return t && t->used && t->alpha;
}

void rsxkit_target_size(const rsxkit_target *t, uint32_t *width, uint32_t *height)
{
	*width = t->w;
	*height = t->h;
}

rsxkit_depth *rsxkit_depth_create(uint32_t width, uint32_t height)
{
	if (!width || !height || width > 4096 || height > 4096)
		return NULL;
	for (int i = 0; i < TARGET_MAX; i++) {
		struct rsxkit_depth *d = &depths[i];
		if (d->used)
			continue;
		memset(d, 0, sizeof *d);
		d->mem = surface_alloc(width, height, &d->pitch, &d->offset);
		if (!d->mem)
			return NULL;
		/* Z24S8 en big-endian: profundidad en los 24 bits altos. */
		uint32_t *p = d->mem;
		for (uint32_t k = 0; k < d->pitch * height / 4; k++)
			p[k] = 0xFFFFFF00u;
		d->w = width;
		d->h = height;
		d->used = 1;
		rsxkit_trace_note("profundidad", "%u×%u pitch %u en 0x%x", width, height, d->pitch, d->offset);
		return d;
	}
	return NULL;
}

void rsxkit_depth_destroy(rsxkit_depth *d)
{
	if (!d || !d->used)
		return;
	rsxkit_trace_note("profundidad suelta", "%u×%u en 0x%x", d->w, d->h, d->offset, 0);
	rsxkit_finish();
	rsxFree(d->mem);
	d->used = 0;
	if (rsxkit_bound.depth == d)
		rsxkit_bound.depth = NULL;
}

int rsxkit_depth_load(rsxkit_depth *d, uint32_t src_offset, uint32_t src_pitch, uint32_t w, uint32_t h,
		      uint32_t y0, uint32_t scale)
{
	if (!d || !d->used || !w || !h || !scale || scale > 8 || (src_offset & 63) || (src_pitch & 63) ||
	    src_pitch < w * 4 || w * scale > d->w || (y0 + h) * scale > d->h)
		return RSXKIT_EARG;
	struct rsxkit_state *g = &rsxkit_g;
	/* Tramos de hasta 1024 píxeles de salida por lado (el motor de imagen
	 * escalada no pasa de 2048) y de un múltiplo de 16 de entrada, para que
	 * el offset de cada uno quede alineado a 64; con el blindaje
	 * (RSXKIT_SAFE_SIFM_EVEN), ninguno de ancho de entrada impar (sifm.c). */
	rsxkit_sifm_tile tiles[64];
	unsigned nt = rsxkit_sifm_plan(w, h, scale, src_pitch, (rsxkit_safe() & RSXKIT_SAFE_SIFM_EVEN) != 0, tiles, 64);
	if (!nt)
		return RSXKIT_EARG;
	rsxkit_trace_note("depth_load", "%u×%u ×%u desde la fila %u", w, h, scale, y0);
	rsxkit_reserve(32 + 40 * nt);
	/* Que el 3D termine con la profundidad antes de pisarla. */
	rsxSetWaitForIdle(g->ctx);
	rsxSetTransferScaleMode(g->ctx, GCM_TRANSFER_MAIN_TO_LOCAL, GCM_TRANSFER_SURFACE);
	gcmTransferSurface surf;
	memset(&surf, 0, sizeof surf);
	surf.format = GCM_TRANSFER_SURFACE_FORMAT_A8R8G8B8;
	surf.pitch = (u16)d->pitch;
	surf.offset = d->offset;
	for (unsigned i = 0; i < nt; i++) {
		const rsxkit_sifm_tile *t = &tiles[i];
		gcmTransferScale sc;
		memset(&sc, 0, sizeof sc);
		sc.conversion = GCM_TRANSFER_CONVERSION_TRUNCATE;
		sc.format = GCM_TRANSFER_SCALE_FORMAT_A8R8G8B8;
		sc.operation = GCM_TRANSFER_OPERATION_SRCCOPY;
		sc.clipX = sc.outX = (s16)(t->x * scale);
		sc.clipY = sc.outY = (s16)((y0 + t->y) * scale);
		sc.clipW = sc.outW = (u16)(t->w * scale);
		sc.clipH = sc.outH = (u16)(t->h * scale);
		/* Entrada por salida en 12.20, redondeada hacia arriba: con el
		 * truncado (1/3 por debajo) el último píxel de cada grupo caería
		 * en el texel anterior. La del ancho visible, no la de in_w. */
		sc.ratioX = (s32)rsxkit_sifm_ratio(t->w, scale);
		sc.ratioY = (s32)rsxkit_sifm_ratio(t->h, scale);
		sc.inW = (u16)t->in_w;
		sc.inH = (u16)t->h;
		sc.pitch = (u16)src_pitch;
		sc.origin = GCM_TRANSFER_ORIGIN_CENTER;
		sc.interp = GCM_TRANSFER_INTERPOLATOR_NEAREST;
		sc.offset = src_offset + (uint32_t)t->y * src_pitch + (uint32_t)t->x * 4;
		rsxSetTransferScaleSurface(g->ctx, &sc, &surf);
	}
	/* Y el 3D no empieza hasta que la copia terminó. */
	rsxSetWaitForIdle(g->ctx);
	rsxkit_bound.target = NULL;
	rsxkit_bound.depth = NULL;
	g->state_valid = 0;
	return RSXKIT_OK;
}

void rsxkit_target_shutdown(void)
{
	for (int i = 0; i < TARGET_MAX; i++) {
		if (targets[i].used)
			rsxFree(targets[i].mem);
		if (depths[i].used)
			rsxFree(depths[i].mem);
		targets[i].used = depths[i].used = 0;
	}
	memset(&rsxkit_bound, 0, sizeof rsxkit_bound);
}

int rsxkit_bind(rsxkit_target *t, rsxkit_depth *d, float lw, float lh)
{
	struct rsxkit_state *g = &rsxkit_g;
	if (!t || !t->used)
		return RSXKIT_EARG;
	if (d && (!d->used || d->w < t->w || d->h < t->h))
		d = NULL;
	/* Nunca una zeta más chica que el color (ver rsxkit.h). */
	if (!d && (g->depth_w < t->w || g->depth_h < t->h))
		return RSXKIT_EDEPTH;
	rsxkit_trace_note("ligar", "%u×%u color 0x%x z 0x%x", t->w, t->h, t->offset, d ? d->offset : g->depth_offset);
	rsxkit_reserve(256);

	gcmSurface sf;
	memset(&sf, 0, sizeof sf);
	sf.colorFormat = t->alpha ? GCM_SURFACE_A8R8G8B8 : GCM_SURFACE_X8R8G8B8;
	sf.colorTarget = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0] = t->offset;
	sf.colorPitch[0] = t->pitch;
	for (int i = 1; i < GCM_MAX_MRT_COUNT; i++) {
		sf.colorLocation[i] = GCM_LOCATION_RSX;
		sf.colorPitch[i] = 64;
	}
	sf.depthFormat = GCM_SURFACE_ZETA_Z24S8;
	sf.depthLocation = GCM_LOCATION_RSX;
	/* Sin profundidad propia se pone la de la pantalla, que cubre al
	 * destino (arriba) y nunca se toca: draw3d.c apaga la prueba y la
	 * escritura. */
	sf.depthOffset = d ? d->offset : g->depth_offset;
	sf.depthPitch = d ? d->pitch : g->depth_pitch;
	sf.type = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias = GCM_SURFACE_CENTER_1;
	sf.width = t->w;
	sf.height = t->h;
	rsxSetSurface(g->ctx, &sf);

	f32 scale[4] = { t->w * 0.5f, t->h * -0.5f, 0.5f, 0.0f };
	f32 offset[4] = { t->w * 0.5f, t->h * 0.5f, 0.5f, 0.0f };
	rsxSetViewport(g->ctx, 0, 0, t->w, t->h, 0.0f, 1.0f, scale, offset);
	for (int i = 0; i < 8; i++)
		rsxSetViewportClip(g->ctx, i, t->w, t->h);
	rsxSetScissor(g->ctx, 0, 0, t->w, t->h);

	rsxkit_bound.target = t;
	rsxkit_bound.alpha = t->alpha;
	rsxkit_bound.depth = d;
	rsxkit_bound.lw = lw > 0.0f ? lw : (float)t->w;
	rsxkit_bound.lh = lh > 0.0f ? lh : (float)t->h;
	rsxkit_bound.sx = t->w / rsxkit_bound.lw;
	rsxkit_bound.sy = t->h / rsxkit_bound.lh;
	rsxkit_bound.scissor_valid = 0;
	rsxkit_bound.w = t->w;
	rsxkit_bound.h = t->h;
	rsxkit_bound.vp_z = -1.0f;
	g->screen_bound = 0;
	g->state_valid = 0;
	return RSXKIT_OK;
}

void rsxkit_bind_frame(float lw, float lh)
{
	rsxkit_screen_texture_invalidate();
	/* Un destino que es el búfer trasero: no está en targets[], así que
	 * nadie lo libera. */
	static struct rsxkit_target frame;
	struct rsxkit_state *g = &rsxkit_g;
	frame.used = 1;
	frame.w = g->screen.width;
	frame.h = g->screen.height;
	frame.pitch = g->color_pitch;
	frame.offset = g->color_offset[g->cur];
	frame.mem = g->color_buf[g->cur];
	rsxkit_bind(&frame, NULL, lw, lh);
}

/* Rectángulo lógico → píxeles del destino, recortado. */
void rsxkit_logical_rect(const float r[4], uint32_t out[4])
{
	const struct rsxkit_target *t = rsxkit_bound.target;
	float x0 = r ? r[0] * rsxkit_bound.sx : 0.0f, y0 = r ? r[1] * rsxkit_bound.sy : 0.0f;
	float x1 = r ? r[2] * rsxkit_bound.sx : (float)t->w, y1 = r ? r[3] * rsxkit_bound.sy : (float)t->h;
	/* Las cuatro dentro del destino: unas tijeras con la esquina izquierda
	 * fuera de la superficie (x0 > x1: vacías) daban una x por encima del
	 * máximo del RSX (4095) a escalas altas. F-Zero X manda x0 = 1023,75
	 * con x1 = 308 al entrar a la carrera; a 5× era x = 5119, y la PS3 se
	 * colgó ahí (RPCS3 no). */
	if (x0 < 0.0f) x0 = 0.0f;
	if (y0 < 0.0f) y0 = 0.0f;
	if (x0 > (float)t->w) x0 = (float)t->w;
	if (y0 > (float)t->h) y0 = (float)t->h;
	if (x1 > (float)t->w) x1 = (float)t->w;
	if (y1 > (float)t->h) y1 = (float)t->h;
	out[0] = (uint32_t)(x0 + 0.5f);
	out[1] = (uint32_t)(y0 + 0.5f);
	out[2] = x1 > x0 ? (uint32_t)(x1 + 0.5f) : out[0];
	out[3] = y1 > y0 ? (uint32_t)(y1 + 0.5f) : out[1];
}

void rsxkit_clear(uint32_t flags, uint32_t rgb, float depth, const float rect[4])
{
	struct rsxkit_state *g = &rsxkit_g;
	if (!rsxkit_bound.target)
		return;
	uint32_t r[4];
	rsxkit_logical_rect(rect, r);
	if (r[2] <= r[0] || r[3] <= r[1])
		return;
	uint32_t mask = 0;
	if (flags & RSXKIT_CLEAR_COLOR)
		mask |= GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A;
	if ((flags & RSXKIT_CLEAR_DEPTH) && rsxkit_bound.depth)
		mask |= GCM_CLEAR_Z | GCM_CLEAR_S;
	if (!mask)
		return;
	rsxkit_trace_repeat("borrar", "%u; el último con máscara 0x%x desde %u,%u", 1, mask, r[0], r[1]);
	rsxkit_reserve(64);
	/* El borrado respeta las tijeras. */
	rsxSetScissor(g->ctx, r[0], r[1], r[2] - r[0], r[3] - r[1]);
	rsxkit_bound.scissor_valid = 0;
	if (depth < 0.0f) depth = 0.0f;
	if (depth > 1.0f) depth = 1.0f;
	/* El alfa (byte alto) cuenta en un destino con alfa o si lo piden. */
	rsxSetClearColor(g->ctx, rsxkit_bound.alpha || (flags & RSXKIT_CLEAR_ALPHA) ? rgb : rgb & 0x00FFFFFFu);
	rsxSetClearDepthStencil(g->ctx, (uint32_t)(depth * 16777215.0f) << 8);
	rsxSetColorMask(g->ctx, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
	rsxSetDepthWriteEnable(g->ctx, GCM_TRUE);
	rsxClearSurface(g->ctx, mask);
	g->state_valid = 0;
}

void rsxkit_clear_stencil(uint8_t value)
{
	if (!rsxkit_bound.target || !rsxkit_bound.depth) return;
	rsxkit_reserve(32);
	rsxSetScissor(rsxkit_g.ctx, 0, 0, rsxkit_bound.w, rsxkit_bound.h);
	rsxSetStencilMask(rsxkit_g.ctx, 255);
	rsxSetClearDepthStencil(rsxkit_g.ctx, value);
	rsxClearSurface(rsxkit_g.ctx, GCM_CLEAR_S);
	rsxkit_g.state_valid = 0;
}

void rsxkit_target_texture(const rsxkit_target *t, uint32_t y0, uint32_t rows, rsxkit_texture *out)
{
	memset(out, 0, sizeof *out);
	if (!t || !t->used || y0 >= t->h)
		return;
	if (!rows || y0 + rows > t->h)
		rows = t->h - y0;
	out->offset = t->offset + y0 * t->pitch;
	out->pitch = t->pitch;
	out->width = (uint16_t)t->w;
	out->height = (uint16_t)rows;
	out->location = RSXKIT_MEM_LOCAL;
	out->format = t->alpha ? RSXKIT_ARGB8888 : RSXKIT_XRGB8888;
}

void rsxkit_texture_sync(void)
{
	struct rsxkit_state *g = &rsxkit_g;
	rsxkit_trace_repeat("texturas al día", NULL, 0, 0, 0, 0);
	rsxkit_reserve(16);
	rsxSetWaitForIdle(g->ctx);
	rsxInvalidateTextureCache(g->ctx, GCM_INVALIDATE_TEXTURE);
}

/* Formatos de superficie que puede leer la CPU: R5G6B5 es el 3 (PSL1GHT lo
 * llama GCM_SURFACE_R5G5B5). */
static int read_surface_format(rsxkit_format f, uint32_t *gcm, uint32_t *bpp)
{
	switch (f) {
	case RSXKIT_RGB565:
		*gcm = GCM_SURFACE_R5G5B5;
		*bpp = 2;
		return 1;
	case RSXKIT_XRGB8888:
		*gcm = GCM_SURFACE_X8R8G8B8;
		*bpp = 4;
		return 1;
	case RSXKIT_ARGB8888:  /* con el alfa del destino (1 si no tiene) */
		*gcm = GCM_SURFACE_A8R8G8B8;
		*bpp = 4;
		return 1;
	default:
		return 0;
	}
}

static int read_exact = 1;

void rsxkit_target_read_exact(int exact)
{
	read_exact = exact != 0;
}

int rsxkit_target_read(const rsxkit_target *t, const rsxkit_rect *src, uint32_t dst_offset, uint32_t dst_pitch,
		       uint32_t img_w, uint32_t img_h, rsxkit_format format)
{
	struct rsxkit_state *g = &rsxkit_g;
	uint32_t gcm, bpp;
	if (!t || !t->used || !img_w || !img_h || img_w > 4096 || img_h > 4096 || !read_surface_format(format, &gcm, &bpp) ||
	    (dst_offset & 127) || (dst_pitch & 63) || dst_pitch < img_w * bpp)
		return RSXKIT_EARG;
	/* La salida va con la profundidad de la pantalla: tiene que cubrirla. */
	if (img_w > g->depth_w || img_h > g->depth_h)
		return RSXKIT_EDEPTH;
	rsxkit_rect s = src ? *src : (rsxkit_rect){ 0, 0, t->w, t->h };
	if (s.x < 0 || s.y < 0 || !s.w || !s.h || (uint32_t)s.x + s.w > t->w || (uint32_t)s.y + s.h > t->h)
		return RSXKIT_EARG;
	if (rsxkit_blit_init() != RSXKIT_OK)
		return RSXKIT_ENOMEM;
	rsxkit_trace_note("leer destino", "%u×%u a %u×%u", s.w, s.h, img_w, img_h);
	rsxkit_reserve(RSXKIT_RESERVE_DRAW);
	rsxkit_leave_3d();

	/* La superficie de salida, en memoria principal; sin profundidad
	 * propia (la de la pantalla, con la prueba apagada). */
	gcmSurface sf;
	memset(&sf, 0, sizeof sf);
	sf.colorFormat = gcm;
	sf.colorTarget = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_CELL;
	sf.colorOffset[0] = dst_offset;
	sf.colorPitch[0] = dst_pitch;
	for (int i = 1; i < GCM_MAX_MRT_COUNT; i++) {
		sf.colorLocation[i] = GCM_LOCATION_RSX;
		sf.colorPitch[i] = 64;
	}
	sf.depthFormat = GCM_SURFACE_ZETA_Z24S8;
	sf.depthLocation = GCM_LOCATION_RSX;
	sf.depthOffset = g->depth_offset;
	sf.depthPitch = g->depth_pitch;
	sf.type = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias = GCM_SURFACE_CENTER_1;
	sf.width = img_w;
	sf.height = img_h;
	rsxSetSurface(g->ctx, &sf);
	f32 scale[4] = { img_w * 0.5f, img_h * -0.5f, 0.5f, 0.0f };
	f32 offset[4] = { img_w * 0.5f, img_h * 0.5f, 0.5f, 0.0f };
	rsxSetColorMask(g->ctx, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
	rsxSetColorMaskMrt(g->ctx, 0);
	rsxSetViewport(g->ctx, 0, 0, img_w, img_h, 0.0f, 1.0f, scale, offset);
	rsxSetScissor(g->ctx, 0, 0, img_w, img_h);
	for (int i = 0; i < 8; i++)
		rsxSetViewportClip(g->ctx, i, img_w, img_h);
	rsxSetDepthTestEnable(g->ctx, GCM_FALSE);
	rsxSetDepthWriteEnable(g->ctx, GCM_FALSE);
	rsxSetCullFaceEnable(g->ctx, GCM_FALSE);
	rsxSetBlendEnable(g->ctx, GCM_FALSE);
	rsxSetAlphaTestEnable(g->ctx, GCM_FALSE);
	/* Sin tramado: al escribir una superficie de 16 bits el RSX suma, si
	 * está encendido, un patrón fijo por píxel antes de redondear. Una
	 * imagen que vuelve a pasar por aquí cuadro tras cuadro (un efecto que
	 * mezcla el cuadro con su copia anterior) acumula ese patrón y queda
	 * como un tablero de ajedrez. Nadie lo vuelve a encender. */
	rsxSetDitherEnable(g->ctx, GCM_FALSE);

	/* Que el RSX termine de escribir el destino antes de leerlo; la
	 * región son filas enteras y la columna va en u. */
	rsxSetWaitForIdle(g->ctx);
	rsxInvalidateTextureCache(g->ctx, GCM_INVALIDATE_TEXTURE);
	uint32_t toff = t->offset + (uint32_t)s.y * t->pitch;
	float u0 = (float)s.x / t->w, u1 = (float)((uint32_t)s.x + s.w) / t->w;
	rsxkit_rect full = { 0, 0, img_w, img_h };
	/* A 1:1 el más cercano: el lineal mezcla una fracción del vecino
	 * (rsxkit_read_filter_nearest en rsxkit.h). */
	rsxkit_filter filter = rsxkit_read_filter_nearest(s.w, s.h, img_w, img_h, read_exact) ? RSXKIT_FILTER_NEAREST
										      : RSXKIT_FILTER_LINEAR;
	/* En ARGB8888 el alfa del destino llega a la salida (filtrado como el
	 * color); sin alfa en el destino, o en los otros formatos, sale 1. */
	rsxkit_blit_alpha(format == RSXKIT_ARGB8888 && t->alpha);
	rsxkit_blit_to(toff, RSXKIT_MEM_LOCAL, t->pitch, t->w, s.h, u0, 0.0f, u1, 1.0f, RSXKIT_XRGB8888, &full, img_w,
		       img_h, filter);
	rsxkit_blit_alpha(0);

	/* Lo ligado ya no vale: ni la pantalla ni un destino. */
	rsxkit_bound.target = NULL;
	rsxkit_bound.depth = NULL;
	g->screen_bound = 0;
	g->state_valid = 0;
	return RSXKIT_OK;
}

int rsxkit_target_copy(rsxkit_target *dst, const rsxkit_target *src)
{
	if (!dst || !dst->used || !src || !src->used || dst == src)
		return RSXKIT_EARG;
	if (rsxkit_blit_init() != RSXKIT_OK)
		return RSXKIT_ENOMEM;
	rsxkit_trace_note("copiar destino", "%u×%u a %u×%u", src->w, src->h, dst->w, dst->h);
	int rc = rsxkit_bind(dst, NULL, (float)dst->w, (float)dst->h);
	if (rc != RSXKIT_OK)
		return rc;
	gcmContextData *ctx = rsxkit_g.ctx;
	rsxkit_reserve(RSXKIT_RESERVE_DRAW);
	rsxkit_leave_3d();
	rsxSetColorMask(ctx, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
	rsxSetDepthTestEnable(ctx, GCM_FALSE);
	rsxSetDepthWriteEnable(ctx, GCM_FALSE);
	rsxSetCullFaceEnable(ctx, GCM_FALSE);
	rsxSetBlendEnable(ctx, GCM_FALSE);
	rsxSetAlphaTestEnable(ctx, GCM_FALSE);
	/* Que el RSX termine de escribir el origen antes de leerlo. */
	rsxSetWaitForIdle(ctx);
	rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);
	rsxkit_rect full = { 0, 0, dst->w, dst->h };
	/* Entre dos destinos con alfa, el alfa se copia; si no, el copiado
	 * (o un destino sin alfa) queda con alfa 1 como antes. */
	rsxkit_blit_alpha(dst->alpha && src->alpha);
	rsxkit_blit_to(src->offset, RSXKIT_MEM_LOCAL, src->pitch, src->w, src->h, 0.0f, 0.0f, 1.0f, 1.0f, RSXKIT_XRGB8888,
		       &full, dst->w, dst->h, RSXKIT_FILTER_NEAREST);
	rsxkit_blit_alpha(0);
	rsxkit_g.state_valid = 0;
	return RSXKIT_OK;
}

int rsxkit_target_fill(rsxkit_target *t, uint32_t src_offset, uint32_t src_pitch, uint32_t w, uint32_t h,
		       rsxkit_format format)
{
	if (!t || !t->used || !w || !h || (unsigned)format >= RSXKIT_FORMAT_COUNT || (src_offset & 127) ||
	    (src_pitch & 63))
		return RSXKIT_EARG;
	if (rsxkit_blit_init() != RSXKIT_OK)
		return RSXKIT_ENOMEM;
	rsxkit_trace_note("llenar destino", "%u×%u desde %u×%u", t->w, t->h, w, h);
	/* Ligar el destino entero con el estado 2D (sin profundidad ni mezcla)
	 * y pintarlo con la imagen estirada, texel por píxel. */
	int rc = rsxkit_bind(t, NULL, (float)t->w, (float)t->h);
	if (rc != RSXKIT_OK)
		return rc;
	gcmContextData *ctx = rsxkit_g.ctx;
	rsxkit_reserve(RSXKIT_RESERVE_DRAW);
	rsxkit_leave_3d();
	rsxSetColorMask(ctx, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
	rsxSetDepthTestEnable(ctx, GCM_FALSE);
	rsxSetDepthWriteEnable(ctx, GCM_FALSE);
	rsxSetCullFaceEnable(ctx, GCM_FALSE);
	rsxSetBlendEnable(ctx, GCM_FALSE);
	rsxSetAlphaTestEnable(ctx, GCM_FALSE);
	rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);
	rsxkit_rect full = { 0, 0, t->w, t->h };
	rsxkit_blit_alpha(t->alpha);  /* el bit de alfa de RGBA5551 llega al destino */
	rsxkit_blit_to(src_offset, RSXKIT_MEM_MAIN, src_pitch, w, h, 0.0f, 0.0f, 1.0f, 1.0f, format, &full, t->w, t->h,
		       RSXKIT_FILTER_NEAREST);
	rsxkit_blit_alpha(0);
	/* El estado 3D hay que volver a ponerlo: lo ligado sigue siendo t. */
	rsxkit_g.state_valid = 0;
	return RSXKIT_OK;
}

int rsxkit_target_fill_rows(rsxkit_target *t, uint32_t src_offset, uint32_t src_pitch, uint32_t w, uint32_t h,
			    uint32_t y0, uint32_t y1, rsxkit_format format)
{
	if (!t || !t->used || !w || !h || y1 > h || y0 >= y1 || (unsigned)format >= RSXKIT_FORMAT_COUNT ||
	    (src_offset & 127) || (src_pitch & 63))
		return RSXKIT_EARG;
	if (y0 == 0 && y1 == h)
		return rsxkit_target_fill(t, src_offset, src_pitch, w, h, format);
	if (rsxkit_blit_init() != RSXKIT_OK)
		return RSXKIT_ENOMEM;
	rsxkit_trace_note("llenar filas", "%u×%u, filas %u a %u", t->w, t->h, y0, y1);
	int rc = rsxkit_bind(t, NULL, (float)t->w, (float)t->h);
	if (rc != RSXKIT_OK)
		return rc;
	gcmContextData *ctx = rsxkit_g.ctx;
	rsxkit_reserve(RSXKIT_RESERVE_DRAW);
	rsxkit_leave_3d();
	rsxSetColorMask(ctx, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
	rsxSetDepthTestEnable(ctx, GCM_FALSE);
	rsxSetDepthWriteEnable(ctx, GCM_FALSE);
	rsxSetCullFaceEnable(ctx, GCM_FALSE);
	rsxSetBlendEnable(ctx, GCM_FALSE);
	rsxSetAlphaTestEnable(ctx, GCM_FALSE);
	rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);
	/* Las filas [y0, y1) de la imagen en las mismas filas del destino (a su
	 * escala): el resto queda como estaba. */
	uint32_t d0 = (uint32_t)((uint64_t)y0 * t->h / h), d1 = (uint32_t)(((uint64_t)y1 * t->h + h - 1) / h);
	rsxkit_rect r = { 0, (int32_t)d0, t->w, d1 - d0 };
	float v0 = (float)d0 * h / t->h / h, v1 = (float)d1 * h / t->h / h;
	rsxkit_blit_alpha(t->alpha);
	rsxkit_blit_to(src_offset, RSXKIT_MEM_MAIN, src_pitch, w, h, 0.0f, v0, 1.0f, v1, format, &r, t->w, t->h,
		       RSXKIT_FILTER_NEAREST);
	rsxkit_blit_alpha(0);
	rsxkit_g.state_valid = 0;
	return RSXKIT_OK;
}

void rsxkit_pixels_565_to_5551(uint16_t *dst, const uint16_t *src, uint32_t n)
{
	uint32_t i = 0;
	/* Uno a uno hasta alinear el destino (una copia que empieza en una
	 * columna cualquiera): desde ahí, si el origen también queda alineado,
	 * cuatro píxeles por palabra de 64 bits: R y los 5 bits altos de G
	 * quedan donde están, B sube un bit y el alfa es 1. */
	for (; i < n && ((uintptr_t)(dst + i) & 7); i++)
		dst[i] = (uint16_t)((src[i] & 0xFFC0) | ((src[i] & 0x1F) << 1) | 1);
	for (; i + 4 <= n && !((uintptr_t)(src + i) & 7) && !((uintptr_t)(dst + i) & 7); i += 4) {
		uint64_t q;
		memcpy(&q, src + i, 8);
		q = (q & 0xFFC0FFC0FFC0FFC0ull) | ((q & 0x001F001F001F001Full) << 1) | 0x0001000100010001ull;
		memcpy(dst + i, &q, 8);
	}
	for (; i < n; i++)
		dst[i] = (uint16_t)((src[i] & 0xFFC0) | ((src[i] & 0x1F) << 1) | 1);
}

void rsxkit_target_present(rsxkit_target *t, const rsxkit_rect *src, uint32_t img_w, uint32_t img_h,
			   const rsxkit_view *view, rsxkit_rect *out)
{
	if (!t || !t->used)
		return;
	static const rsxkit_view def = { RSXKIT_SCALE_FIT, RSXKIT_FILTER_LINEAR, 0.0f };
	if (!view)
		view = &def;
	rsxkit_rect s = src ? *src : (rsxkit_rect){ 0, 0, t->w, t->h };
	if (s.x < 0 || s.y < 0 || !s.w || !s.h || (uint32_t)s.x + s.w > t->w || (uint32_t)s.y + s.h > t->h)
		s = (rsxkit_rect){ 0, 0, t->w, t->h };
	rsxkit_rect r = rsxkit_layout(img_w ? img_w : s.w, img_h ? img_h : s.h, view);
	if (out)
		*out = r;
	/* La textura son las filas de la región, enteras (el offset de una
	 * textura va alineado a 128: el destino y su pitch lo están, una
	 * columna cualquiera no); la columna va en las coordenadas de textura. */
	uint32_t offset = t->offset + (uint32_t)s.y * t->pitch;
	float u0 = (float)s.x / t->w, u1 = (float)((uint32_t)s.x + s.w) / t->w;
	/* Que el RSX termine de escribir el destino antes de leerlo. */
	rsxSetWaitForIdle(rsxkit_g.ctx);
	rsxInvalidateTextureCache(rsxkit_g.ctx, GCM_INVALIDATE_TEXTURE);
	rsxkit_blit(offset, t->pitch, t->w, s.h, u0, 0.0f, u1, 1.0f, RSXKIT_XRGB8888, &r, view->filter);
}

/* ---- Autoprueba del alfa de los dibujos (rsxkit_dsta_selftest) -------------- */

/* Un caso: el modo, la mezcla, la prueba de alfa y el alfa del fragmento. */
static const struct {
	uint8_t dsta, blend, ref, a;
} dsta_case[RSXKIT_DSTA_TEST_N] = {
	{ RSXKIT_DSTA_ONE, RSXKIT_BLEND_OFF, 0, 0x20 },     /* 1 */
	{ RSXKIT_DSTA_ONE, RSXKIT_BLEND_ALPHA, 0, 0x20 },   /* 1, con la mezcla del color por el alfa */
	{ RSXKIT_DSTA_ONE, RSXKIT_BLEND_OFF, 0x10, 0x20 },  /* 1: pasa la prueba de alfa */
	{ RSXKIT_DSTA_ONE, RSXKIT_BLEND_OFF, 0x40, 0x20 },  /* el del destino: la prueba lo descarta */
	{ RSXKIT_DSTA_FRAG, RSXKIT_BLEND_OFF, 0, 0x20 },    /* el del fragmento */
	{ RSXKIT_DSTA_KEEP, RSXKIT_BLEND_OFF, 0, 0x20 },    /* el del destino */
	{ RSXKIT_DSTA_MAX, RSXKIT_BLEND_OFF, 0, 0x20 },     /* el del destino, mayor */
	{ RSXKIT_DSTA_MAX, RSXKIT_BLEND_OFF, 0, 0xA0 },     /* el del fragmento, mayor */
};
#define DSTA_TEST_DST 0x60u  /* el alfa con que se borra el destino */

const uint8_t rsxkit_dsta_expected[RSXKIT_DSTA_TEST_N] = { 0xFF, 0xFF, 0xFF, DSTA_TEST_DST, 0x20, DSTA_TEST_DST,
							   DSTA_TEST_DST, 0xA0 };

int rsxkit_dsta_selftest(uint8_t out[RSXKIT_DSTA_TEST_N])
{
	enum { CELL = 8, W = CELL * RSXKIT_DSTA_TEST_N, H = CELL };
	memset(out, 0, RSXKIT_DSTA_TEST_N);
	rsxkit_target *t = rsxkit_target_create_ex(W, H, RSXKIT_TARGET_ALPHA);
	rsxkit_vertex *v = rsxMemalign(128, 6 * RSXKIT_DSTA_TEST_N * sizeof *v);
	uint32_t voff = 0;
	int rc = RSXKIT_OK;
	if (!t || !v || rsxAddressToOffset(v, &voff) != 0) {
		rc = RSXKIT_ENOMEM;
		goto out;
	}
	/* Un cuadrado de CELL × CELL por caso, en dos triángulos. */
	for (int i = 0; i < RSXKIT_DSTA_TEST_N; i++) {
		static const float corner[6][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
		for (int k = 0; k < 6; k++) {
			rsxkit_vertex *p = &v[6 * i + k];
			memset(p, 0, sizeof *p);
			p->pos[0] = (float)(CELL * (i + corner[k][0]));
			p->pos[1] = (float)(CELL * corner[k][1]);
			p->pos[2] = 0.5f;
			p->pos[3] = 1.0f;
			memset(p->rgba, 0xFF, sizeof p->rgba);
		}
	}
	if ((rc = rsxkit_bind(t, NULL, (float)W, (float)H)) != RSXKIT_OK)
		goto out;
	rsxkit_clear(RSXKIT_CLEAR_COLOR, DSTA_TEST_DST << 24 | 0x204060u, 1.0f, NULL);
	rsxkit_vertices(voff, RSXKIT_MEM_LOCAL);
	for (int i = 0; i < RSXKIT_DSTA_TEST_N; i++) {
		rsxkit_draw d;
		memset(&d, 0, sizeof d);
		d.blend = dsta_case[i].blend;
		d.dst_alpha = dsta_case[i].dsta;
		d.alpha_ref = dsta_case[i].ref;
		d.cull = RSXKIT_CULL_NONE;
		d.scissor[2] = (float)W;
		d.scissor[3] = (float)H;
		d.k[RSXKIT_K_ONE][0][0] = 1.0f;
		d.k[RSXKIT_K_ONE][0][1] = 0.5f;
		d.k[RSXKIT_K_ONE][0][2] = 0.25f;
		d.k[RSXKIT_K_ONE][0][3] = dsta_case[i].a / 255.0f;
		rsxkit_draw_triangles(&d, 6u * (uint32_t)i, 6);
	}
	rsxkit_fence_wait(rsxkit_fence());
	/* A8R8G8B8 en big-endian: el primer byte de cada píxel es el alfa. */
	for (int i = 0; i < RSXKIT_DSTA_TEST_N; i++)
		out[i] = ((const volatile uint8_t *)t->mem)[(CELL / 2) * t->pitch + (CELL * i + CELL / 2) * 4];
out:
	rsxkit_3d_forget_vertices();
	rsxkit_leave_3d();
	if (v) {
		rsxkit_finish();
		rsxFree(v);
	}
	rsxkit_target_destroy(t);
	return rc;
}
