/* SPDX-License-Identifier: MIT */
/*
 * rsxkit — posprocesado (rsxkit/post.h): superficies intermedias,
 * programas precompilados con sus constantes, pasadas de pantalla completa
 * y marcas de tiempo del RSX.
 *
 * Una pasada es un rectángulo en modo inmediato con el vertex program
 * rsxkit_post.vcg: la posición y TEXCOORD0 van por vértice; las constantes
 * del dibujo (TEXCOORD1..7) se mandan una vez como atributos y el vertex
 * program las copia, así que no se tocan las constantes del vertex program
 * (las del 3D siguen puestas) ni hace falta parchear el fragment program
 * en cada dibujo.
 *
 * Las constantes de un programa (sus uniforms) van dentro del microcódigo:
 * cgcomp deja en el .fpo, por cada una, la lista de lugares del
 * microcódigo donde está su valor. Cada rsxkit_program tiene una copia
 * propia en memoria principal (la verdad, con la corrección de perspectiva
 * ya limpia y las constantes puestas) y otra en la memoria local del RSX;
 * rsxkit_program_set parchea las dos (la del RSX cuando este ya no la usa).
 * Las dos se registran en fprog.c para que rsxkit_after_system las
 * restaure si el sistema las pisa.
 *
 * Cada pasada termina escribiendo RSXKIT_LABEL_POST con un número que
 * crece: el de un programa o una superficie es el de su último dibujo, y
 * esperar esa etiqueta es esperar a que el RSX los suelte.
 */
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"

/* Shaders de rsxkit compilados con cgcomp y embebidos por el Makefile. */
extern const u8 rsxkit_post_vpo[];
extern const u8 rsxkit_post_reduce_fpo[];
extern const u8 rsxkit_post_reduce_h_fpo[];
extern const u8 rsxkit_post_reduce_v_fpo[];
extern const u8 rsxkit_post_zero_fpo[];

#define SURF_ALIGN 128

struct rsxkit_surface {
	int used;
	uint32_t w, h, pitch, offset;
	rsxkit_post_format format;
	void *mem;
	int main;  /* memoria principal del que la creó (rsxkit_surface_wrap_main) */
	uint32_t last_use;
};

struct rsxkit_program {
	int used;
	const rsxFragmentProgram *fpo;
	rsxFragmentProgram hdr;  /* lo que pide rsxLoadFragmentProgramLocation */
	u32 *shadow;             /* microcódigo con las constantes (memoria principal) */
	void *buf;               /* su copia en la memoria local del RSX */
	u32 size, offset, insns;
	uint32_t last_use;
};

static struct rsxkit_surface surfaces[RSXKIT_SURFACES_MAX];
static struct rsxkit_program programs[RSXKIT_PROGRAMS_MAX];

static struct {
	rsxVertexProgram *vp;
	void *vp_ucode;
	int ready;
	uint32_t seq;               /* último valor de RSXKIT_LABEL_POST enviado */
	rsxkit_program *reduce;
	int reduce_factor;          /* factor con que están puestas sus constantes */
	rsxkit_program *reduce_h, *reduce_v;  /* en dos pasadas */
	int reduce2_factor;
	rsxkit_surface *reduce_tmp;  /* la tabla entre las dos (fp16) */
	int reduce_passes;          /* 1 o 2 (0: aún sin elegir, 2) */
	rsxkit_program *zero;       /* rsxkit_post_clear */
} post;

static inline u32 halves(u32 w)
{
	return (w << 16) | (w >> 16);
}

static void post_init(void)
{
	u32 size;
	if (post.ready)
		return;
	post.vp = (rsxVertexProgram *)rsxkit_post_vpo;
	rsxVertexProgramGetUCode(post.vp, &post.vp_ucode, &size);
	*(volatile u32 *)gcmGetLabelAddress(RSXKIT_LABEL_POST) = 0;
	post.seq = 0;
	post.ready = 1;
}

static void wait_use(uint32_t seq)
{
	if (seq)
		rsxkit_wait_label(RSXKIT_LABEL_POST, seq);
}

/* ---- Superficies ----------------------------------------------------------- */

static const struct {
	u8 surface, texture, bpp;
} surf_formats[RSXKIT_POST_FORMATS] = {
	[RSXKIT_POST_RGBA8] = { GCM_SURFACE_A8R8G8B8, GCM_TEXTURE_FORMAT_A8R8G8B8, 4 },
	[RSXKIT_POST_RGBA16F] = { GCM_SURFACE_F_W16Z16Y16X16, GCM_TEXTURE_FORMAT_W16_Z16_Y16_X16_FLOAT, 8 },
};

rsxkit_surface *rsxkit_surface_create(uint32_t width, uint32_t height, rsxkit_post_format format)
{
	if (!width || !height || width > 4096 || height > 4096 || (unsigned)format >= RSXKIT_POST_FORMATS)
		return NULL;
	post_init();
	for (int i = 0; i < RSXKIT_SURFACES_MAX; i++) {
		struct rsxkit_surface *s = &surfaces[i];
		if (s->used)
			continue;
		memset(s, 0, sizeof *s);
		s->pitch = (width * surf_formats[format].bpp + 63) & ~63u;
		s->mem = rsxMemalign(SURF_ALIGN, s->pitch * height);
		if (!s->mem)
			return NULL;
		rsxAddressToOffset(s->mem, &s->offset);
		s->w = width;
		s->h = height;
		s->format = format;
		s->used = 1;
		return s;
	}
	return NULL;
}

rsxkit_surface *rsxkit_surface_wrap_main(uint32_t width, uint32_t height, rsxkit_post_format format, void *mem,
					 uint32_t offset, uint32_t pitch)
{
	if (!width || !height || width > 4096 || height > 4096 || (unsigned)format >= RSXKIT_POST_FORMATS || !mem ||
	    ((uintptr_t)mem & (SURF_ALIGN - 1)) || (offset & (SURF_ALIGN - 1)) || (pitch & 63) ||
	    pitch < width * surf_formats[format].bpp)
		return NULL;
	post_init();
	for (int i = 0; i < RSXKIT_SURFACES_MAX; i++) {
		struct rsxkit_surface *s = &surfaces[i];
		if (s->used)
			continue;
		memset(s, 0, sizeof *s);
		s->pitch = pitch;
		s->mem = mem;
		s->offset = offset;
		s->main = 1;
		s->w = width;
		s->h = height;
		s->format = format;
		s->used = 1;
		return s;
	}
	return NULL;
}

void rsxkit_surface_destroy(rsxkit_surface *s)
{
	if (!s || !s->used)
		return;
	wait_use(s->last_use);
	if (!s->main)
		rsxFree(s->mem);
	s->used = 0;
}

void rsxkit_surface_size(const rsxkit_surface *s, uint32_t *width, uint32_t *height)
{
	if (width)
		*width = s && s->used ? s->w : 0;
	if (height)
		*height = s && s->used ? s->h : 0;
}

const void *rsxkit_surface_data(const rsxkit_surface *s, uint32_t *pitch)
{
	if (!s || !s->used)
		return NULL;
	if (pitch)
		*pitch = s->pitch;
	return s->mem;
}

uint32_t rsxkit_surface_bytes(void)
{
	uint32_t n = 0;
	for (int i = 0; i < RSXKIT_SURFACES_MAX; i++)
		if (surfaces[i].used && !surfaces[i].main)
			n += surfaces[i].pitch * surfaces[i].h;
	return n;
}

/* ---- Programas ---------------------------------------------------------------- */

/* Instrucciones del microcódigo (cada una de 16 bytes, más 16 si lee una
 * constante; la última tiene el bit de fin). */
static u32 count_insns(const u32 *ucode, u32 size)
{
	u32 n = 0;
	for (u32 i = 0; i + 4 <= size / 4; n++) {
		u32 w0 = halves(ucode[i]);
		int has_const = 0;
		for (int s = 1; s < 4; s++)
			if ((halves(ucode[i + s]) & 3u) == 2u)
				has_const = 1;
		i += has_const ? 8 : 4;
		if (w0 & 1u)
			return n + 1;
	}
	return n;
}

rsxkit_program *rsxkit_program_create(const void *fpo)
{
	const rsxFragmentProgram *fp = fpo;
	if (!fp || fp->magic != 0x4650)  /* "FP" */
		return NULL;
	post_init();
	for (int i = 0; i < RSXKIT_PROGRAMS_MAX; i++) {
		struct rsxkit_program *p = &programs[i];
		if (p->used)
			continue;
		void *ucode;
		u32 size;
		rsxFragmentProgramGetUCode((rsxFragmentProgram *)fp, &ucode, &size);
		memset(p, 0, sizeof *p);
		p->shadow = memalign(16, size);
		p->buf = p->shadow ? rsxMemalign(64, size) : NULL;
		if (!p->buf) {
			free(p->shadow);
			return NULL;
		}
		memcpy(p->shadow, ucode, size);
		rsxkit_fp_fix_perspective(p->shadow, size);
		memcpy(p->buf, p->shadow, size);
		rsxAddressToOffset(p->buf, &p->offset);
		p->fpo = fp;
		p->hdr = *fp;
		p->size = size;
		p->insns = count_insns(p->shadow, size);
		rsxkit_fp_track(p->buf, p->shadow, size, 0);
		p->used = 1;
		return p;
	}
	return NULL;
}

int rsxkit_program_bind_3d(rsxkit_program *p)
{
	if (!p || !p->used) return RSXKIT_EARG;
	rsxLoadFragmentProgramLocation(rsxkit_g.ctx, &p->hdr, p->offset, GCM_LOCATION_RSX);
	return RSXKIT_OK;
}

void rsxkit_program_used_3d(rsxkit_program *p)
{
	uint32_t seq = ++post.seq;
	rsxSetWriteBackendLabel(rsxkit_g.ctx, RSXKIT_LABEL_POST, seq);
	p->last_use = seq;
}

void rsxkit_program_destroy(rsxkit_program *p)
{
	if (!p || !p->used)
		return;
	rsxkit_3d_flush_use();
	wait_use(p->last_use);
	rsxkit_fp_untrack(p->buf);
	rsxFree(p->buf);
	free(p->shadow);
	p->used = 0;
}

uint32_t rsxkit_program_instructions(const rsxkit_program *p)
{
	return p && p->used ? p->insns : 0;
}

/* Pone v (4 floats) en cada lugar del microcódigo de la constante; true si
 * cambió algo. */
static bool patch_const(struct rsxkit_program *p, const rsxProgramConst *c, const float *v)
{
	const rsxConstOffsetTable *t = (const rsxConstOffsetTable *)((const u8 *)p->fpo + c->index);
	u32 w[4];
	memcpy(w, v, sizeof w);
	for (int k = 0; k < 4; k++)
		w[k] = halves(w[k]);
	bool changed = false;
	for (u32 i = 0; i < t->num; i++) {
		u32 off = t->offset[i];
		if (off + 16 > p->size)
			continue;
		u32 *dst = p->shadow + off / 4;
		if (memcmp(dst, w, sizeof w)) {
			memcpy(dst, w, sizeof w);
			changed = true;
		}
	}
	return changed;
}

int rsxkit_program_set(rsxkit_program *p, const char *name, const float *v, uint32_t count)
{
	if (!p || !p->used || !name || !v || !count)
		return RSXKIT_EARG;
	bool changed = false;
	int found = 0;
	for (uint32_t i = 0; i < count; i++) {
		char elem[64];
		const rsxProgramConst *c = NULL;
		if (count == 1)
			c = rsxFragmentProgramGetConst((rsxFragmentProgram *)p->fpo, name);
		if (!c) {
			snprintf(elem, sizeof elem, "%s[%u]", name, (unsigned)i);
			c = rsxFragmentProgramGetConst((rsxFragmentProgram *)p->fpo, elem);
		}
		if (!c)
			continue;
		found++;
		if (patch_const(p, c, v + 4 * i))
			changed = true;
	}
	if (changed) {
		/* La copia del RSX: cuando ya no la use. El RSX vuelve a leer el
		 * programa cada vez que se liga (rsxLoadFragmentProgramLocation en
		 * cada pasada). */
		wait_use(p->last_use);
		memcpy(p->buf, p->shadow, p->size);
		rsxkit_fp_track(p->buf, p->shadow, p->size, 0);
	}
	return found ? RSXKIT_OK : RSXKIT_EARG;
}

/* ---- Pasadas ------------------------------------------------------------------- */

rsxkit_post_input rsxkit_post_surface_input(const rsxkit_surface *s, rsxkit_filter filter, uint8_t wrap)
{
	rsxkit_post_input in;
	memset(&in, 0, sizeof in);
	in.surface = s;
	in.filter = (uint8_t)filter;
	in.wrap = wrap;
	in.texels = 1;
	return in;
}

rsxkit_post_input rsxkit_post_texture_input(const rsxkit_texture *t, rsxkit_filter filter, uint8_t wrap)
{
	rsxkit_post_input in;
	memset(&in, 0, sizeof in);
	in.tex = *t;
	in.filter = (uint8_t)filter;
	in.wrap = wrap;
	in.texels = 1;
	return in;
}

#define REMAP_ARGB                                                                                         \
	GCM_TEXTURE_REMAP_MODE(GCM_TEXTURE_REMAP_ORDER_XYXY, GCM_TEXTURE_REMAP_COLOR_A, GCM_TEXTURE_REMAP_COLOR_R, \
			       GCM_TEXTURE_REMAP_COLOR_G, GCM_TEXTURE_REMAP_COLOR_B, GCM_TEXTURE_REMAP_TYPE_REMAP,  \
			       GCM_TEXTURE_REMAP_TYPE_REMAP, GCM_TEXTURE_REMAP_TYPE_REMAP, GCM_TEXTURE_REMAP_TYPE_REMAP)

static void set_input(gcmContextData *ctx, u8 unit, const rsxkit_post_input *in)
{
	gcmTexture tex;
	memset(&tex, 0, sizeof tex);
	u8 fmt;
	u32 remap;
	if (in->surface) {
		const struct rsxkit_surface *s = in->surface;
		fmt = surf_formats[s->format].texture;
		remap = REMAP_ARGB;
		tex.width = s->w;
		tex.height = s->h;
		tex.pitch = s->pitch;
		tex.offset = s->offset;
		tex.location = s->main ? GCM_LOCATION_CELL : GCM_LOCATION_RSX;
	} else {
		rsxkit_format_gcm((rsxkit_format)in->tex.format, &fmt, &remap);
		tex.width = in->tex.width;
		tex.height = in->tex.height;
		tex.pitch = in->tex.pitch;
		tex.offset = in->tex.offset;
		tex.location = in->tex.location == RSXKIT_MEM_MAIN ? GCM_LOCATION_CELL : GCM_LOCATION_RSX;
	}
	tex.format = fmt | GCM_TEXTURE_FORMAT_LIN | (in->texels ? GCM_TEXTURE_FORMAT_UNRM : GCM_TEXTURE_FORMAT_NRM);
	tex.mipmap = 1;
	tex.dimension = GCM_TEXTURE_DIMS_2D;
	tex.cubemap = GCM_FALSE;
	tex.remap = remap;
	tex.depth = 1;

	u8 filt = in->filter == RSXKIT_FILTER_NEAREST ? GCM_TEXTURE_NEAREST : GCM_TEXTURE_LINEAR;
	u8 wrap = in->wrap == RSXKIT_POST_BORDER   ? GCM_TEXTURE_BORDER
		  : in->wrap == RSXKIT_POST_REPEAT ? GCM_TEXTURE_REPEAT
						   : GCM_TEXTURE_CLAMP_TO_EDGE;
	if (in->refresh)
		rsxTextureControl(ctx, unit, GCM_FALSE, 0, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
	rsxLoadTexture(ctx, unit, &tex);
	rsxTextureControl(ctx, unit, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
	rsxTextureFilter(ctx, unit, 0, filt, filt, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	rsxTextureWrapMode(ctx, unit, wrap, wrap, wrap, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
	if (in->wrap == RSXKIT_POST_BORDER)
		rsxTextureBorderColor(ctx, unit, 0);
}

/* Liga la superficie de salida (o la pantalla) con la profundidad de la
 * pantalla, que debe cubrirla. */
static int bind_output(struct rsxkit_surface *s, uint32_t *w, uint32_t *h)
{
	struct rsxkit_state *g = &rsxkit_g;
	if (!s) {
		rsxkit_screen_texture_invalidate();
		if (!g->screen_bound)
			rsxkit_bind_screen();
		else
			rsxkit_set_2d_state();
		*w = g->screen.width;
		*h = g->screen.height;
		return RSXKIT_OK;
	}
	if (s->w > g->depth_w || s->h > g->depth_h)
		return RSXKIT_EDEPTH;
	gcmSurface sf;
	memset(&sf, 0, sizeof sf);
	sf.colorFormat = surf_formats[s->format].surface;
	sf.colorTarget = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = s->main ? GCM_LOCATION_CELL : GCM_LOCATION_RSX;
	sf.colorOffset[0] = s->offset;
	sf.colorPitch[0] = s->pitch;
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
	sf.width = s->w;
	sf.height = s->h;
	rsxSetSurface(g->ctx, &sf);
	rsxkit_set_2d_state_size(s->w, s->h);
	/* Lo ligado ya no es ni la pantalla ni un destino. */
	g->screen_bound = 0;
	rsxkit_bound.target = NULL;
	rsxkit_bound.depth = NULL;
	rsxkit_bound.scissor_valid = 0;
	*w = s->w;
	*h = s->h;
	return RSXKIT_OK;
}

static void vertex(gcmContextData *ctx, float x, float y, float sw, float sh, const float t[4])
{
	f32 pos[4] = { 2.0f * x / sw - 1.0f, 1.0f - 2.0f * y / sh, 0.0f, 1.0f };
	rsxDrawVertex4f(ctx, GCM_VERTEX_ATTRIB_TEX0, t);
	rsxDrawVertex4f(ctx, GCM_VERTEX_ATTRIB_POS, pos);  /* la posición emite el vértice */
}

int rsxkit_post_run(const rsxkit_post_pass *p)
{
	struct rsxkit_state *g = &rsxkit_g;
	gcmContextData *ctx = g->ctx;
	if (!p || !p->program || !p->program->used || p->inputs > RSXKIT_POST_INPUTS || (p->out && !p->out->used) ||
	    (p->out && p->out_target))
		return RSXKIT_EARG;
	for (uint32_t i = 0; i < p->inputs; i++)
		if (p->in[i].surface && !p->in[i].surface->used)
			return RSXKIT_EARG;
	post_init();
	rsxkit_trace_repeat("pasada", "%u en total; la última con %u entradas", 1, p->inputs, 0, 0);
	rsxkit_reserve(RSXKIT_RESERVE_DRAW);
	rsxkit_leave_3d();
	uint32_t sw, sh;
	int rc;
	if (p->out_target) {
		rsxkit_target_size(p->out_target, &sw, &sh);
		rc = rsxkit_bind(p->out_target, NULL, (float)sw, (float)sh);
		if (rc == RSXKIT_OK)
			rsxkit_set_2d_state_size(sw, sh);
	} else rc = bind_output(p->out, &sw, &sh);
	if (rc != RSXKIT_OK)
		return rc;
	rsxkit_rect r = p->rect;
	if (!r.w || !r.h)
		r = (rsxkit_rect){ 0, 0, sw, sh };

	/* Lo que lee esta pasada pudo escribirlo la anterior. */
	rsxSetWaitForIdle(ctx);
	rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);
	for (uint32_t i = 0; i < p->inputs; i++)
		set_input(ctx, (u8)i, &p->in[i]);

	rsxLoadVertexProgram(ctx, post.vp, post.vp_ucode);
	rsxLoadFragmentProgramLocation(ctx, &p->program->hdr, p->program->offset, GCM_LOCATION_RSX);

	float stx = p->step[0] != 0.0f ? p->step[0] : 1.0f, sty = p->step[1] != 0.0f ? p->step[1] : 1.0f;
	float x0 = (float)r.x, y0 = (float)r.y, x1 = x0 + r.w, y1 = y0 + r.h;
	float u0 = p->origin[0], v0 = p->origin[1], u1 = u0 + r.w * stx, v1 = v0 + r.h * sty;
	const float t00[4] = { u0, v0, 0.0f, 0.0f }, t10[4] = { u1, v0, 1.0f, 0.0f };
	const float t01[4] = { u0, v1, 0.0f, 1.0f }, t11[4] = { u1, v1, 1.0f, 1.0f };
	/* Constantes afines: un valor por esquina (las esquinas del rectángulo
	 * están medio píxel antes y después de los centros extremos). */
	int affine = 0;
	for (int j = 0; j < RSXKIT_POST_K; j++)
		for (int c = 0; c < 4; c++)
			if (p->kx[j][c] != 0.0f || p->ky[j][c] != 0.0f)
				affine = 1;
	const float *const tc[4] = { t00, t10, t01, t11 };
	const float cu[4] = { 0.0f, (float)r.w, 0.0f, (float)r.w }, cv[4] = { 0.0f, 0.0f, (float)r.h, (float)r.h };
	const float px[4] = { x0, x1, x0, x1 }, py[4] = { y0, y0, y1, y1 };
	rsxDrawVertexBegin(ctx, GCM_TYPE_TRIANGLE_STRIP);
	if (!affine)
		for (int j = 0; j < RSXKIT_POST_K; j++)
			rsxDrawVertex4f(ctx, GCM_VERTEX_ATTRIB_TEX1 + j, p->k[j]);
	for (int v = 0; v < 4; v++) {
		if (affine)
			for (int j = 0; j < RSXKIT_POST_K; j++) {
				float kv[4];
				for (int c = 0; c < 4; c++)
					kv[c] = p->k[j][c] + p->kx[j][c] * cu[v] + p->ky[j][c] * cv[v];
				rsxDrawVertex4f(ctx, GCM_VERTEX_ATTRIB_TEX1 + j, kv);
			}
		vertex(ctx, px[v], py[v], (float)sw, (float)sh, tc[v]);
	}
	rsxDrawVertexEnd(ctx);

	uint32_t seq = ++post.seq;
	rsxSetWriteBackendLabel(ctx, RSXKIT_LABEL_POST, seq);
	p->program->last_use = seq;
	if (p->out)
		p->out->last_use = seq;
	for (uint32_t i = 0; i < p->inputs; i++)
		if (p->in[i].surface)
			((struct rsxkit_surface *)p->in[i].surface)->last_use = seq;
	return RSXKIT_OK;
}

int rsxkit_post_clear(rsxkit_surface *s)
{
	if (!s || !s->used)
		return RSXKIT_EARG;
	/* Una pasada que escribe 0 y no el borrado del RSX (rsxClearSurface):
	 * así vale igual en cualquier formato de superficie. */
	if (!post.zero && !(post.zero = rsxkit_program_create(rsxkit_post_zero_fpo)))
		return RSXKIT_ENOMEM;
	rsxkit_post_pass p;
	memset(&p, 0, sizeof p);
	p.program = post.zero;
	p.out = s;
	return rsxkit_post_run(&p);
}

int rsxkit_post_run_mesh(const rsxkit_post_pass *p, const rsxkit_post_mesh *m)
{
	struct rsxkit_state *g = &rsxkit_g;
	gcmContextData *ctx = g->ctx;
	if (!p || !m || !p->program || !p->program->used || p->inputs > RSXKIT_POST_INPUTS ||
	    (p->out && !p->out->used) || !m->count || m->count % 3 || (m->vertices & 15) || (m->indices & 1) ||
	    (m->blend != RSXKIT_BLEND_OFF && m->blend != RSXKIT_BLEND_ADD))
		return RSXKIT_EARG;
	for (uint32_t i = 0; i < p->inputs; i++)
		if (p->in[i].surface && !p->in[i].surface->used)
			return RSXKIT_EARG;
	post_init();
	rsxkit_trace_repeat("pasada", "%u en total; la última con %u entradas", 1, p->inputs, 0, 0);
	rsxkit_reserve(RSXKIT_RESERVE_DRAW);
	rsxkit_leave_3d();
	uint32_t sw, sh;
	int rc = bind_output(p->out, &sw, &sh);
	if (rc != RSXKIT_OK)
		return rc;

	rsxSetWaitForIdle(ctx);
	rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);
	rsxInvalidateVertexCache(ctx);
	for (uint32_t i = 0; i < p->inputs; i++)
		set_input(ctx, (u8)i, &p->in[i]);
	rsxLoadVertexProgram(ctx, post.vp, post.vp_ucode);
	rsxLoadFragmentProgramLocation(ctx, &p->program->hdr, p->program->offset, GCM_LOCATION_RSX);

	if (m->blend == RSXKIT_BLEND_ADD) {
		rsxSetBlendEnable(ctx, GCM_TRUE);
		rsxSetBlendFunc(ctx, GCM_ONE, GCM_ONE, GCM_ONE, GCM_ONE);
		rsxSetBlendEquation(ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
	}
	if (m->cull != RSXKIT_CULL_NONE) {
		/* Como en draw3d.c: frente antihorario con la y hacia arriba. */
		rsxSetFrontFace(ctx, GCM_FRONTFACE_CCW);
		rsxSetCullFaceEnable(ctx, GCM_TRUE);
		rsxSetCullFace(ctx, m->cull == RSXKIT_CULL_FRONT ? GCM_CULL_FRONT : GCM_CULL_BACK);
	}
	u8 loc = m->location == RSXKIT_MEM_MAIN ? GCM_LOCATION_CELL : GCM_LOCATION_RSX;
	const u8 stride = sizeof(rsxkit_post_vertex);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_POS, 0, m->vertices, stride, 4, GCM_VERTEX_DATA_TYPE_F32, loc);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX0, 0, m->vertices + 16, stride, 4, GCM_VERTEX_DATA_TYPE_F32,
				 loc);
	g->arrays_bound = 1;
	rsxDrawIndexArray(ctx, GCM_TYPE_TRIANGLES, m->indices, m->count, GCM_INDEX_TYPE_16B, loc);
	/* Lo que dejan las pasadas: sin mezcla, sin descartar caras y sin
	 * arreglos (los vuelve a poner quien los use). */
	rsxkit_reserve(RSXKIT_RESERVE_DRAW);
	if (m->blend == RSXKIT_BLEND_ADD)
		rsxSetBlendEnable(ctx, GCM_FALSE);
	if (m->cull != RSXKIT_CULL_NONE)
		rsxSetCullFaceEnable(ctx, GCM_FALSE);
	rsxkit_leave_3d();

	uint32_t seq = ++post.seq;
	rsxSetWriteBackendLabel(ctx, RSXKIT_LABEL_POST, seq);
	p->program->last_use = seq;
	if (p->out)
		p->out->last_use = seq;
	for (uint32_t i = 0; i < p->inputs; i++)
		if (p->in[i].surface)
			((struct rsxkit_surface *)p->in[i].surface)->last_use = seq;
	return RSXKIT_OK;
}

/* ---- Reducción con filtro de caja ------------------------------------------------ */

/* En cada eje, el bloque de f texeles se cubre con hasta 3 lecturas
 * bilineales: una en la frontera entre dos texeles promedia los dos; si f
 * es impar, la última cae en el centro del último texel. Desplazamientos
 * desde la esquina del bloque, en texeles, y pesos. */
static void reduce_taps(int f, float off[4], float w[4])
{
	memset(off, 0, 4 * sizeof *off);
	memset(w, 0, 4 * sizeof *w);
	int k = 0;
	for (int t = 0; t + 1 < f && k < 3; t += 2, k++) {
		off[k] = (float)(t + 1);
		w[k] = 2.0f / f;
	}
	if ((f & 1) && k < 3) {
		off[k] = f - 0.5f;
		w[k] = 1.0f / f;
	}
}

/* Las constantes de un programa de reducción para el factor f. */
static void reduce_set(rsxkit_program *p, int f)
{
	float off[4], wt[4];
	reduce_taps(f, off, wt);
	rsxkit_program_set(p, "tx", off, 1);
	rsxkit_program_set(p, "wx", wt, 1);
}

int rsxkit_post_reduce_passes(int passes)
{
	int was = post.reduce_passes ? post.reduce_passes : 2;
	if (passes == 1 || passes == 2)
		post.reduce_passes = passes;
	return was;
}

/* La primera pasada deja (w × region.h) en la tabla fp16; la segunda la
 * reduce a lo alto en `dst`. */
static int reduce_two(const rsxkit_texture *src, const rsxkit_rect *region, uint32_t factor, rsxkit_surface *dst,
		      uint32_t w, uint32_t h)
{
	if (!post.reduce_h) {
		post.reduce_h = rsxkit_program_create(rsxkit_post_reduce_h_fpo);
		post.reduce_v = rsxkit_program_create(rsxkit_post_reduce_v_fpo);
		if (!post.reduce_h || !post.reduce_v) {
			rsxkit_program_destroy(post.reduce_h);
			rsxkit_program_destroy(post.reduce_v);
			post.reduce_h = post.reduce_v = NULL;
			return RSXKIT_ENOMEM;
		}
		post.reduce2_factor = 0;
	}
	if (post.reduce2_factor != (int)factor) {
		reduce_set(post.reduce_h, (int)factor);
		reduce_set(post.reduce_v, (int)factor);
		post.reduce2_factor = (int)factor;
	}
	/* Del tamaño justo, no la más grande que hizo falta: una salida se liga
	 * con la profundidad de la pantalla y no puede ser más alta que ella.
	 * Al bajar la escala interna (5× → 3×), la profundidad vuelve a la de la
	 * pantalla y la tabla de 5× (región.h = 1185) ya no se podía ligar. */
	uint32_t tw, th;
	rsxkit_surface_size(post.reduce_tmp, &tw, &th);
	if (tw != w || th != region->h) {
		rsxkit_surface_destroy(post.reduce_tmp);
		post.reduce_tmp = rsxkit_surface_create(w, region->h, RSXKIT_POST_RGBA16F);
		if (!post.reduce_tmp)
			return RSXKIT_ENOMEM;
	}
	rsxkit_post_pass p;
	memset(&p, 0, sizeof p);
	p.program = post.reduce_h;
	p.out = post.reduce_tmp;
	p.rect = (rsxkit_rect){ 0, 0, w, region->h };
	p.in[0] = rsxkit_post_texture_input(src, RSXKIT_FILTER_LINEAR, RSXKIT_POST_CLAMP);
	p.inputs = 1;
	p.k[0][0] = (float)region->x;
	p.k[0][1] = (float)region->y;
	p.k[0][2] = (float)factor;
	int rc = rsxkit_post_run(&p);
	if (rc != RSXKIT_OK)
		return rc;
	p.program = post.reduce_v;
	p.out = dst;
	p.rect = (rsxkit_rect){ 0, 0, w, h };
	p.in[0] = rsxkit_post_surface_input(post.reduce_tmp, RSXKIT_FILTER_LINEAR, RSXKIT_POST_CLAMP);
	p.k[0][0] = p.k[0][1] = 0.0f;
	return rsxkit_post_run(&p);
}

int rsxkit_post_reduce(const rsxkit_texture *src, const rsxkit_rect *region, uint32_t factor, rsxkit_surface *dst)
{
	if (!src || !region || !dst || !dst->used || factor < 1 || factor > 6 || region->x < 0 || region->y < 0)
		return RSXKIT_EARG;
	uint32_t w = region->w / factor, h = region->h / factor;
	if (!w || !h || w > dst->w || h > dst->h)
		return RSXKIT_EARG;
	if (factor > 1 && post.reduce_passes != 1)
		return reduce_two(src, region, factor, dst, w, h);
	if (!post.reduce) {
		post.reduce = rsxkit_program_create(rsxkit_post_reduce_fpo);
		if (!post.reduce)
			return RSXKIT_ENOMEM;
		post.reduce_factor = 0;
	}
	if (post.reduce_factor != (int)factor) {
		reduce_set(post.reduce, (int)factor);
		post.reduce_factor = (int)factor;
	}
	rsxkit_post_pass p;
	memset(&p, 0, sizeof p);
	p.program = post.reduce;
	p.out = dst;
	p.rect = (rsxkit_rect){ 0, 0, w, h };
	p.in[0] = rsxkit_post_texture_input(src, RSXKIT_FILTER_LINEAR, RSXKIT_POST_CLAMP);
	p.inputs = 1;
	p.k[0][0] = (float)region->x;
	p.k[0][1] = (float)region->y;
	p.k[0][2] = (float)factor;
	return rsxkit_post_run(&p);
}

/* ---- Etiquetas ---------------------------------------------------------------------- */

void rsxkit_post_label_write(uint32_t index, uint32_t value)
{
	gcmContextData *ctx = rsxkit_g.ctx;
	rsxkit_reserve(16);
	rsxSetWriteBackendLabel(ctx, (u8)index, value);
	rsxFlushBuffer(ctx);
}

void rsxkit_post_label_wait(uint32_t index, uint32_t value)
{
	rsxkit_reserve(16);
	rsxSetWaitLabel(rsxkit_g.ctx, (u8)index, value);
}

volatile uint32_t *rsxkit_label_address(uint32_t index)
{
	return (volatile uint32_t *)gcmGetLabelAddress((u8)index);
}

/* ---- Marcas de tiempo ------------------------------------------------------------ */

/* Un reporte del RSX (método 0x1800, GET_REPORT): escribe en la entrada
 * `index` del área de reportes su reloj (ns) y el contador del tipo, cuando
 * llega ahí y terminó lo anterior. Se usa el tipo 1 (píxeles que pasaron
 * la prueba de profundidad): solo interesa el reloj. No se usa
 * rsxSetTimeStamp de PSL1GHT (f649a08): escribe el dato en la palabra
 * siguiente a la que corresponde y deja en su lugar lo que hubiera en el
 * búfer de comandos, así que el RSX escribe el reporte en un lugar
 * cualquiera de su memoria (en RPCS3: «NV4097_GET_REPORT: Bad type 0» y el
 * RSX colgado al rato). */
#define NV40_GET_REPORT     0x1800u
#define REPORT_ZPASS_PIXELS 1u

void rsxkit_stamp(uint32_t i)
{
	if (i >= RSXKIT_STAMPS)
		return;
	gcmContextData *ctx = rsxkit_g.ctx;
	rsxkit_reserve(2);
	u32 *p = (u32 *)ctx->current;
	p[0] = (1u << 18) | NV40_GET_REPORT;
	p[1] = (REPORT_ZPASS_PIXELS << 24) | ((RSXKIT_STAMP_BASE + i) * 16u);
	ctx->current = p + 2;
}

void rsxkit_post_shutdown(void)
{
	for (int i = 0; i < RSXKIT_PROGRAMS_MAX; i++)
		if (programs[i].used) {
			rsxkit_fp_untrack(programs[i].buf);
			rsxFree(programs[i].buf);
			free(programs[i].shadow);
			programs[i].used = 0;
		}
	for (int i = 0; i < RSXKIT_SURFACES_MAX; i++)
		if (surfaces[i].used) {
			if (!surfaces[i].main)
				rsxFree(surfaces[i].mem);
			surfaces[i].used = 0;
		}
	post.reduce = post.reduce_h = post.reduce_v = post.zero = NULL;
	post.reduce_tmp = NULL;
	post.ready = 0;
}

uint64_t rsxkit_stamp_ns(uint32_t i)
{
	return i < RSXKIT_STAMPS ? gcmGetTimeStamp(RSXKIT_STAMP_BASE + i) : 0;
}
