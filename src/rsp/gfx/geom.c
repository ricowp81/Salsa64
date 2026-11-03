/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — HLE de los microcódigos 3D (F3D, F3DEX, F3DEX2): matrices,
 * viewport, luces y transformación de vértices.
 *
 * Convenciones de la N64 (libultra, derivada de OpenGL): vectores fila,
 * clip = (x, y, z, 1) · MV · P; una matriz nueva multiplica por la
 * izquierda a la actual (M' = M_nueva · M). Pantalla: x = vtrans.x +
 * (x/w)·vscale.x, y = vtrans.y − (y/w)·vscale.y (la y crece hacia abajo),
 * z = vtrans.z + (z/w)·vscale.z en 0..0x3FF.
 *
 * Luces y lookat se llevan al espacio del modelo cuando cambia la
 * modelview (como hace el microcódigo) y se normalizan; la normal del
 * vértice (bytes con signo) se usa tal cual.
 */
#include <stdint.h>
#include <string.h>

#if defined(__SPU__) && GFX_SPE0_IMG == 1
#include <spu_intrinsics.h>
#include <spukit/spu.h>
#endif

#include "core/endian.h"
#include "rsp/gfx/hle.h"
#if defined(__SPU__) && GFX_SPE0_IMG == 1 && GFX_SPE0_PHASE == 8
#include "rsp/gfx/clip4.h"
#endif

static float inv_sqrt(float x)
{
	/* Newton desde una aproximación por bits: suficiente para normalizar. */
	union { float f; uint32_t u; } v = { x };
	v.u = 0x5F3759DFu - (v.u >> 1);
	float y = v.f;
	y = y * (1.5f - 0.5f * x * y * y);
	y = y * (1.5f - 0.5f * x * y * y);
	return y;
}

static void normalize3(float v[3])
{
	float d = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
	if (d > 1e-12f) {
		float k = inv_sqrt(d);
		v[0] *= k;
		v[1] *= k;
		v[2] *= k;
	}
}

static void identity(float m[16])
{
	memset(m, 0, 16 * sizeof(float));
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void gfx_geom_reset(struct gfx_state *g)
{
	g->mv_top = 0;
	identity(g->mv[0]);
	identity(g->proj);
	identity(g->mvp);
	g->mvp_dirty = true;
	g->lights_dirty = true;
	memset(g->lights, 0, sizeof g->lights);
	g->num_lights = 0;
	g->lookat[0][0] = 1.0f, g->lookat[0][1] = 0.0f, g->lookat[0][2] = 0.0f;
	g->lookat[1][0] = 0.0f, g->lookat[1][1] = 1.0f, g->lookat[1][2] = 0.0f;
	g->vscale[0] = g->vtrans[0] = 160.0f;
	g->vscale[1] = g->vtrans[1] = 120.0f;
	g->vscale[2] = g->vtrans[2] = 511.0f;
	g->clip_ratio[0] = g->clip_ratio[1] = 1.0f;  /* gfx_run pone la del microcódigo */
	g->fog_mul = g->fog_off = 0.0f;
	memset(g->vtx, 0, sizeof g->vtx);
}

/* Mtx de libultra: 16 partes enteras (s16) y luego 16 fraccionarias (u16),
 * fila por fila; valor = (entero << 16 | fracción) / 65536. */
void gfx_load_matrix(float m[16], const uint8_t *src)
{
	for (int i = 0; i < 16; i++) {
		int32_t v = (int32_t)((uint32_t)salsa64_rd16(src + 2 * i) << 16 | salsa64_rd16(src + 32 + 2 * i));
		m[i] = (float)v * (1.0f / 65536.0f);
	}
}

void gfx_mtx_mul(float out[16], const float a[16], const float b[16])
{
	float r[16];
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++)
			r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] +
				       a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
	memcpy(out, r, sizeof r);
}

void gfx_matrix(struct gfx_state *g, uint32_t addr, uint32_t flags)
{
	uint32_t td = 0;
	GFX_DETAIL(g, td = gfx_io_clock());
	const uint8_t *src = gfx_io_read(g, GFX_IO_MTX, addr, 64);
	if (!src) {
		g->res->dl_errors++;
		GFX_DETAIL(g, g->detail->t_matrix += gfx_io_clock() - td);
		return;
	}
	float m[16];
	gfx_load_matrix(m, src);
	if (flags & GFX_MTX_PROJECTION) {
		/* La proyección no tiene pila: PUSH no hace nada. */
		if (flags & GFX_MTX_LOAD)
			memcpy(g->proj, m, sizeof m);
		else
			gfx_mtx_mul(g->proj, m, g->proj);
	} else {
		if ((flags & GFX_MTX_PUSH) && g->mv_top + 1 < GFX_MTX_STACK) {
			memcpy(g->mv[g->mv_top + 1], g->mv[g->mv_top], sizeof m);
			g->mv_top++;
		}
		if (flags & GFX_MTX_LOAD)
			memcpy(g->mv[g->mv_top], m, sizeof m);
		else
			gfx_mtx_mul(g->mv[g->mv_top], m, g->mv[g->mv_top]);
		g->lights_dirty = true;
	}
	g->mvp_dirty = true;
	GFX_DETAIL(g, g->detail->t_matrix += gfx_io_clock() - td);
}

void gfx_pop_matrix(struct gfx_state *g, uint32_t n)
{
	g->mv_top = n > (uint32_t)g->mv_top ? 0 : g->mv_top - (int)n;
	g->mvp_dirty = true;
	g->lights_dirty = true;
}

void gfx_force_matrix(struct gfx_state *g, uint32_t addr)
{
	const uint8_t *src = gfx_io_read(g, GFX_IO_MTX, addr, 64);
	if (src) {
		gfx_load_matrix(g->mvp, src);
		g->mvp_dirty = false;
	}
}

/* Valor en punto fijo s15.16, redondeado y saturado. */
static int32_t to_fixed(float v)
{
	float x = v * 65536.0f;
	if (x >= 2147483520.0f)
		return INT32_MAX;
	if (x <= -2147483648.0f)
		return INT32_MIN;
	return (int32_t)(x >= 0.0f ? x + 0.5f : x - 0.5f);
}

/* El Mtx guarda primero las 16 partes enteras y luego las 16 fracciones:
 * `where` < 0x20 cambia las enteras de dos elementos seguidos; si no, sus
 * fracciones. La matriz vale hasta el próximo G_MTX, que la recalcula. */
void gfx_insert_matrix(struct gfx_state *g, uint32_t where, uint32_t val)
{
	if (where >= 64)
		return;
	if (g->mvp_dirty) {
		gfx_mtx_mul(g->mvp, g->mv[g->mv_top], g->proj);
		g->mvp_dirty = false;
	}
	unsigned i = (where & 0x1F) / 2;
	for (unsigned k = 0; k < 2 && i + k < 16; k++) {
		uint32_t half = k == 0 ? val >> 16 : val & 0xFFFF;
		uint32_t fx = (uint32_t)to_fixed(g->mvp[i + k]);
		fx = where < 0x20 ? half << 16 | (fx & 0xFFFF) : (fx & 0xFFFF0000u) | half;
		g->mvp[i + k] = (float)(int32_t)fx * (1.0f / 65536.0f);
	}
}

void gfx_num_lights(struct gfx_state *g, int n)
{
	g->num_lights = n < 0 ? 0 : n > GFX_MAX_LIGHTS ? GFX_MAX_LIGHTS : n;
	g->lights_dirty = true;
}

void gfx_light_color(struct gfx_state *g, int light, uint32_t rgba)
{
	if (light < 0 || light > GFX_MAX_LIGHTS)
		return;
	g->lights[light].col[0] = (float)(rgba >> 24);
	g->lights[light].col[1] = (float)((rgba >> 16) & 0xFF);
	g->lights[light].col[2] = (float)((rgba >> 8) & 0xFF);
}

/* Dónde cambia un vértice (G_MODIFYVTX, G_MW_POINTS). */
enum { G_MWO_POINT_RGBA = 0x10, G_MWO_POINT_ST = 0x14, G_MWO_POINT_XYSCREEN = 0x18, G_MWO_POINT_ZSCREEN = 0x1C };

void gfx_modify_vertex(struct gfx_state *g, uint32_t v, uint32_t where, uint32_t val)
{
	if (v >= GFX_MAX_VTX)
		return;
	struct gfx_vtx *p = &g->vtx[v];
	switch (where) {
	case G_MWO_POINT_RGBA:
		p->c[0] = (uint8_t)(val >> 24);
		p->c[1] = (uint8_t)(val >> 16);
		p->c[2] = (uint8_t)(val >> 8);
		p->c[3] = (uint8_t)val;
		break;
	case G_MWO_POINT_ST:
		p->s = (float)(int16_t)(val >> 16) / 32.0f;
		p->t = (float)(int16_t)(val & 0xFFFF) / 32.0f;
		/* Sin la escala de G_TEXTURE: el microcódigo real copia el ST tal cual
		 * (--lle-check-gfx: el logo de N64 de Resident Evil 2, s de 0 a 126 con
		 * escala 1/2, sale entero solo así; con la escala salía su cuarto
		 * superior izquierdo ampliado). */
		break;
	case G_MWO_POINT_XYSCREEN: {
		/* Posición de pantalla nueva (10.2): se vuelve a llevar a recorte. */
		float sx = (float)(int16_t)(val >> 16) / 4.0f, sy = (float)(int16_t)(val & 0xFFFF) / 4.0f;
		/* Un vértice cargado con w = 0 (Resident Evil 2 arma así el texto de
		 * la pantalla de muerte: vértices en cero y la posición de pantalla
		 * puesta aquí). El RSP real le da al RDP esta posición tal cual; en
		 * recorte iba con w = 0 y el RSX lo descartaba: pantalla negra. Se
		 * lleva a w = 1, delante de la cámara. No va en la imagen 0 del SPE
		 * (F3D: RE2 usa F3DEX2), que no tiene pila de sobra. */
#if !defined(GFX_SPE0_IMG) || GFX_SPE0_IMG != 0
		if (p->w == 0.0f && g->vscale[0] != 0.0f && g->vscale[1] != 0.0f) {
			p->w = 1.0f;
			p->z = 0.0f;
			p->clip = 0;
		}
#endif
		if (p->w != 0.0f && g->vscale[0] != 0.0f && g->vscale[1] != 0.0f) {
			p->x = (sx - g->vtrans[0]) / g->vscale[0] * p->w;
			p->y = (g->vtrans[1] - sy) / g->vscale[1] * p->w;
		}
		p->sx = sx;
		p->sy = sy;
		break;
	}
	case G_MWO_POINT_ZSCREEN: {
		float sz = (float)(val >> 16);
		if (p->w != 0.0f && g->vscale[2] != 0.0f)
			p->z = (sz - g->vtrans[2]) / g->vscale[2] * p->w;
		p->sz = sz;
		break;
	}
	default:
		break;
	}
}

bool gfx_vertices_culled(const struct gfx_state *g, uint32_t v0, uint32_t v1)
{
	uint32_t all = 0x3F;
	if (v0 >= GFX_MAX_VTX || v1 < v0)
		return false;
	if (v1 >= GFX_MAX_VTX)
		v1 = GFX_MAX_VTX - 1;
	for (uint32_t i = v0; i <= v1; i++)
		all &= g->vtx[i].clip;
	return all != 0;
}

/* Vp: vscale[4] y vtrans[4] (s16); x e y en cuartos de píxel. */
void gfx_set_viewport(struct gfx_state *g, const uint8_t *vp)
{
	for (int i = 0; i < 3; i++) {
		float k = i < 2 ? 0.25f : 1.0f;
		float s = (float)(int16_t)salsa64_rd16(vp + 2 * i) * k, t = (float)(int16_t)salsa64_rd16(vp + 8 + 2 * i) * k;
		if (i < 2 && (s != g->vscale[i] || t != g->vtrans[i]))
			g->dirty = true;  /* la caja de recorte va en las tijeras del dibujo */
		g->vscale[i] = s;
		g->vtrans[i] = t;
	}
}

void gfx_clip_ratio(struct gfx_state *g, uint32_t offset, uint32_t value)
{
	if (offset != 0x04 && offset != 0x0C)
		return;
	int16_t r = (int16_t)(value & 0xFFFF);
	float f = (float)(r < 0 ? -r : r);
	float *dst = &g->clip_ratio[offset == 0x0C];
	if (f > 0.0f && f != *dst) {
		*dst = f;
		g->dirty = true;
	}
}

/* Light: col[3], pad, colc[3], pad, dir[3] (s8), pad. */
void gfx_set_light(struct gfx_state *g, int index, const uint8_t *src)
{
	struct gfx_light *l = &g->lights[index];
	for (int i = 0; i < 3; i++) {
		l->col[i] = (float)src[i];
		l->dir[i] = (float)(int8_t)src[8 + i];
	}
	normalize3(l->dir);
	g->lights_dirty = true;
}

void gfx_set_lookat(struct gfx_state *g, int axis, const uint8_t *src)
{
	/* F3DFLX: en el lugar del lookat Y va la luz del alfa, tres s16 en los
	 * bytes 8 a 13 (medido con la LLE: el microcódigo deja este mismo
	 * vector, en el espacio del modelo, en la DMEM). */
	bool flx = GFX_HAS(FLX) && axis == 1 && (g->ucode_var & GFX_UCODE_FLX);
	for (int i = 0; i < 3; i++)
		g->lookat[axis][i] = flx ? (float)(int16_t)salsa64_rd16(src + 8 + 2 * i) : (float)(int8_t)src[8 + i];
	normalize3(g->lookat[axis]);
	g->lights_dirty = true;
}

/* Dirección en espacio de la cámara → espacio del modelo: d_m = M3 · d (la
 * transpuesta de cómo se transforma una normal con vectores fila). */
static void to_model(const float m[16], const float d[3], float out[3])
{
	for (int i = 0; i < 3; i++)
		out[i] = m[i * 4 + 0] * d[0] + m[i * 4 + 1] * d[1] + m[i * 4 + 2] * d[2];
	normalize3(out);
}

static void update_lights(struct gfx_state *g)
{
	const float *m = g->mv[g->mv_top];
	for (int i = 0; i < g->num_lights; i++)
		to_model(m, g->lights[i].dir, g->light_model[i]);
	to_model(m, g->lookat[0], g->lookat_model[0]);
	to_model(m, g->lookat[1], g->lookat_model[1]);
	g->lights_dirty = false;
}

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* acos aproximado (error < 1e-4 rad) para G_TEXTURE_GEN_LINEAR. */
static float acos_approx(float x)
{
	x = clampf(x, -1.0f, 1.0f);
	float ax = x < 0.0f ? -x : x;
	float r = 1.5707288f + ax * (-0.2121144f + ax * (0.0742610f + ax * -0.0187293f));
	float s = 1.0f - ax;
	r *= s > 0.0f ? s * inv_sqrt(s) : 0.0f;  /* sqrt(1 − |x|) */
	return x < 0.0f ? 3.14159265f - r : r;
}

void gfx_load_vertices(struct gfx_state *g, uint32_t addr, uint32_t v0, uint32_t n)
{
	if (v0 >= GFX_MAX_VTX)
		return;
	if (n > GFX_MAX_VTX - v0)
		n = GFX_MAX_VTX - v0;
	const uint8_t *src = gfx_io_read(g, GFX_IO_VTX, addr, n * 16);
	if (!src) {
		g->res->dl_errors++;
		return;
	}
	gfx_vertices(g, src, v0, n);
	gfx_vgroup(g, addr, 16, v0, n, src);
	GFX_DETAIL(g, {
		g->detail->vertex_calls++;
		g->detail->vertices += n;
		if ((g->geom & G_LIGHTING) ||
		    (GFX_HAS(ACCLAIM) && g->job->gbi == SALSA64_GBI_F3DEX2ACCLAIM &&
		     (g->geom & G_ACCLAIM_LIGHTING))) g->detail->lit_vertices += n;
		if (g->geom & G_FOG) g->detail->fog_vertices += n;
		if (g->geom & G_TEXTURE_GEN) g->detail->texgen_vertices += n;
	});
}

void gfx_light_vectors(struct gfx_state *g)
{
	if (g->lights_dirty)
		update_lights(g);
}

void gfx_texgen_vertex(struct gfx_state *g, const float nrm[3], struct gfx_vtx *v)
{
	const float *lx = g->lookat_model[0], *ly = g->lookat_model[1];
	float dx = nrm[0] * lx[0] + nrm[1] * lx[1] + nrm[2] * lx[2];
	float dy = nrm[0] * ly[0] + nrm[1] * ly[1] + nrm[2] * ly[2];
	float s, t;
	/* En unidades s10.5, antes de la escala de G_TEXTURE: (d + 1)/2 en
	 * 0..1 llega a 1024 texels; con la escala típica de mapeo de entorno
	 * (0x07C0) quedan ~31. */
	if (g->geom & G_TEXTURE_GEN_LINEAR) {
		s = acos_approx(-dx) * (1024.0f * 32.0f / 3.14159265f);
		t = acos_approx(-dy) * (1024.0f * 32.0f / 3.14159265f);
	} else {
		s = (dx + 1.0f) * (512.0f * 32.0f);
		t = (dy + 1.0f) * (512.0f * 32.0f);
	}
	v->s = s * ((float)g->tex_s / (65536.0f * 32.0f));
	v->t = t * ((float)g->tex_t / (65536.0f * 32.0f));
}

#if defined(__SPU__) && GFX_SPE0_IMG == 1
/* Cuatro vértices sin luces ni generación de coordenadas. La entrada de los
 * G_VTX es de 16 bytes, así que se juntan cuatro posiciones en los carriles
 * del SPU y se conserva para cada carril la misma secuencia fm/fma/fa que
 * usa el compilador en el camino escalar. La proyección, la conversión y la
 * niebla siguen el orden escalar: de ese modo la salida es intercambiable
 * con gfx-escalar incluso en los bordes de redondeo.
 *
 * Road Rash usa esta forma (color por vértice y niebla), que evita cuatro
 * recorridos independientes de la matriz y los saltos de recorte. Las
 * combinaciones con luz o TEXGEN conservan el camino general de abajo. */
typedef vector float gfx_v4f;

static inline gfx_v4f v4_dot(const gfx_v4f x, const gfx_v4f y, const gfx_v4f z, const float *m, unsigned c)
{
	gfx_v4f r = spu_mul(y, spu_splats(m[4 + c]));
	r = spu_madd(x, spu_splats(m[c]), r);
	r = spu_madd(z, spu_splats(m[8 + c]), r);
	return spu_add(r, spu_splats(m[12 + c]));
}

static void gfx_vertices4_fog(struct gfx_state *g, const uint8_t *src, uint32_t v0)
{
	union { gfx_v4f q; float f[4]; } x, y, z, xo, yo, zo, wo;
	const float *m = g->mvp;
	/* `vtx_transform` se reutiliza para una única subfase seleccionada por
	 * imagen; sin una imagen de fase conserva la medida de la matriz. */
	uint32_t t = 0;
#if defined(GFX_SPE0_PHASE) && (GFX_SPE0_PHASE == 1 || GFX_SPE0_PHASE >= 7)
	GFX_DETAIL(g, t = ~spu_read_decrementer());
#endif
	for (unsigned i = 0; i < 4; i++, src += 16) {
		x.f[i] = (float)(int16_t)salsa64_rd16(src + 0);
		y.f[i] = (float)(int16_t)salsa64_rd16(src + 2);
		z.f[i] = (float)(int16_t)salsa64_rd16(src + 4);
	}
#if defined(GFX_SPE0_PHASE) && GFX_SPE0_PHASE == 1
	GFX_DETAIL(g, g->detail->vtx_transform += ~spu_read_decrementer() - t;);
#endif
#if !defined(GFX_SPE0_PHASE)
	GFX_DETAIL(g, t = gfx_io_clock());
#endif
	xo.q = v4_dot(x.q, y.q, z.q, m, 0);
	yo.q = v4_dot(x.q, y.q, z.q, m, 1);
	zo.q = v4_dot(x.q, y.q, z.q, m, 2);
	wo.q = v4_dot(x.q, y.q, z.q, m, 3);
	#if !defined(GFX_SPE0_PHASE)
	GFX_DETAIL(g, g->detail->vtx_transform += gfx_io_clock() - t;);
	#endif
	src -= 64;
#if defined(GFX_SPE0_PHASE) && GFX_SPE0_PHASE == 6
	/* Un solo intervalo para el postproceso completo del lote: incluye las
	 * copias x/y/z/w que no cabían en la fase 5 y las fases 2 a 5.
	 * El desempaquetado y la matriz quedan fuera del intervalo. */
	GFX_DETAIL(g, t = ~spu_read_decrementer());
#endif
	#if GFX_SPE0_PHASE == 8
	union { gfx_clip4u q; uint32_t u[4]; } clips;
	clips.q = gfx_clip4_mask(xo.q, yo.q, zo.q, wo.q);
	#endif
	for (unsigned i = 0; i < 4; i++, src += 16) {
		struct gfx_vtx *v = &g->vtx[v0 + i];
		v->x = xo.f[i];
		v->y = yo.f[i];
		v->z = zo.f[i];
		v->w = wo.f[i];
		/* La variante 8 calcula los cuatro juegos de banderas con SIMD.
		 * La referencia conserva las comparaciones escalares anteriores. */
	#if defined(GFX_SPE0_PHASE) && GFX_SPE0_PHASE == 3
		GFX_DETAIL(g, t = ~spu_read_decrementer());
	#endif
	#if GFX_SPE0_PHASE == 8
		v->clip = clips.u[i];
	#else
		v->clip = (uint32_t)(v->x < -v->w) * GFX_CLIP_XN |
			  (uint32_t)(v->x >  v->w) * GFX_CLIP_XP |
			  (uint32_t)(v->y < -v->w) * GFX_CLIP_YN |
			  (uint32_t)(v->y >  v->w) * GFX_CLIP_YP |
			  (uint32_t)(v->z >  v->w) * GFX_CLIP_ZF |
			  (uint32_t)(v->w < 0.0f) * GFX_CLIP_W;
	#endif
	#if defined(GFX_SPE0_PHASE) && GFX_SPE0_PHASE == 3
		GFX_DETAIL(g, g->detail->vtx_transform += ~spu_read_decrementer() - t;);
	#endif
		/* La división queda escalar deliberadamente: el refinamiento que
		 * emite spu-gcc para / reproduce el redondeo de la ruta de referencia. */
	#if defined(GFX_SPE0_PHASE) && GFX_SPE0_PHASE == 2
		GFX_DETAIL(g, t = ~spu_read_decrementer());
	#endif
		float iw = v->w != 0.0f ? 1.0f / v->w : 0.0f;
		v->sx = g->vtrans[0] + v->x * iw * g->vscale[0];
		v->sy = g->vtrans[1] - v->y * iw * g->vscale[1];
		v->sz = g->vtrans[2] + v->z * iw * g->vscale[2];
	#if defined(GFX_SPE0_PHASE) && GFX_SPE0_PHASE == 2
		GFX_DETAIL(g, g->detail->vtx_transform += ~spu_read_decrementer() - t;);
	#endif
	#if defined(GFX_SPE0_PHASE) && GFX_SPE0_PHASE == 5
		GFX_DETAIL(g, t = ~spu_read_decrementer());
	#endif
		v->s = (float)(int16_t)salsa64_rd16(src + 8) * ((float)g->tex_s / (65536.0f * 32.0f));
		v->t = (float)(int16_t)salsa64_rd16(src + 10) * ((float)g->tex_t / (65536.0f * 32.0f));
		v->c[0] = src[12]; v->c[1] = src[13]; v->c[2] = src[14];
	#if defined(GFX_SPE0_PHASE) && GFX_SPE0_PHASE == 5
		GFX_DETAIL(g, g->detail->vtx_transform += ~spu_read_decrementer() - t;);
	#endif
	#if defined(GFX_SPE0_PHASE) && GFX_SPE0_PHASE == 4
		GFX_DETAIL(g, t = ~spu_read_decrementer());
	#endif
		float zw = v->w > 0.0f && v->z >= -v->w ? v->z * iw : -1.0f;
		v->c[3] = (uint8_t)clampf(zw * g->fog_mul + g->fog_off + 0.5f, 0.0f, 255.0f);
	#if defined(GFX_SPE0_PHASE) && GFX_SPE0_PHASE == 4
		GFX_DETAIL(g, g->detail->vtx_transform += ~spu_read_decrementer() - t;);
	#endif
	}
#if defined(GFX_SPE0_PHASE) && (GFX_SPE0_PHASE >= 6)
	GFX_DETAIL(g, g->detail->vtx_transform += ~spu_read_decrementer() - t;);
#endif
}
#endif

void gfx_vertices(struct gfx_state *g, const uint8_t *src, uint32_t v0, uint32_t n)
{
	if (v0 >= GFX_MAX_VTX)
		return;
	if (n > GFX_MAX_VTX - v0)
		n = GFX_MAX_VTX - v0;
	g->res->vertices += n;  /* ritmo del RCP (salsa64_gfx_model_cycles) */
	if (g->mvp_dirty) {
		gfx_mtx_mul(g->mvp, g->mv[g->mv_top], g->proj);
		g->mvp_dirty = false;
	}
	bool lighting = (g->geom & G_LIGHTING) != 0;
	bool acclaim_lighting = GFX_HAS(ACCLAIM) && g->job->gbi == SALSA64_GBI_F3DEX2ACCLAIM &&
				(g->geom & G_ACCLAIM_LIGHTING) != 0;
	if (lighting && g->lights_dirty)
		update_lights(g);
	const uint8_t *flx_tab = GFX_HAS(FLX) && lighting ? gfx_flx_table(g) : NULL;
	const float *m = g->mvp;
	/* s, t del vértice (s10.5) × escala de G_TEXTURE (0.16) → texels. */
	float ks = (float)g->tex_s / (65536.0f * 32.0f), kt = (float)g->tex_t / (65536.0f * 32.0f);

#if defined(__SPU__) && GFX_SPE0_IMG == 1
	/* El lote solo cubre la forma uniforme y medida. El resto sigue abajo,
	 * también cuando gfx-escalar pide la ruta anterior para comparar. */
	if (g->job->flags & GFX_JOB_SCALAR) {
		GFX_DETAIL(g, g->detail->scalar_forced += n);
	} else if (lighting || acclaim_lighting) {
		GFX_DETAIL(g, {
			g->detail->scalar_lighting += n;
			if (g->geom & G_TEXTURE_GEN)
				g->detail->scalar_texgen += n;
		});
	} else if (!(g->geom & G_FOG)) {
		GFX_DETAIL(g, g->detail->scalar_no_fog += n);
	} else {
		uint32_t batches = n / 4;
		GFX_DETAIL(g, {
			g->detail->vector_vertices += batches * 4;
			g->detail->vector_batches += batches;
			g->detail->scalar_tail += n - batches * 4;
		});
		while (n >= 4) {
			gfx_vertices4_fog(g, src, v0);
			src += 64;
			v0 += 4;
			n -= 4;
		}
	}
#endif

	for (uint32_t i = 0; i < n; i++, src += 16) {
		struct gfx_vtx *v = &g->vtx[v0 + i];
		float x = (float)(int16_t)salsa64_rd16(src + 0);
		float y = (float)(int16_t)salsa64_rd16(src + 2);
		float z = (float)(int16_t)salsa64_rd16(src + 4);
		v->x = x * m[0] + y * m[4] + z * m[8] + m[12];
		v->y = x * m[1] + y * m[5] + z * m[9] + m[13];
		v->z = x * m[2] + y * m[6] + z * m[10] + m[14];
		v->w = x * m[3] + y * m[7] + z * m[11] + m[15];

		uint32_t clip = 0;
		if (v->x < -v->w) clip |= GFX_CLIP_XN;
		if (v->x > v->w) clip |= GFX_CLIP_XP;
		if (v->y < -v->w) clip |= GFX_CLIP_YN;
		if (v->y > v->w) clip |= GFX_CLIP_YP;
		if (v->z > v->w) clip |= GFX_CLIP_ZF;
		if (v->w < 0.0f) clip |= GFX_CLIP_W;
		v->clip = clip;

		float iw = v->w != 0.0f ? 1.0f / v->w : 0.0f;
		v->sx = g->vtrans[0] + v->x * iw * g->vscale[0];
		v->sy = g->vtrans[1] - v->y * iw * g->vscale[1];
		v->sz = g->vtrans[2] + v->z * iw * g->vscale[2];

		float s = (float)(int16_t)salsa64_rd16(src + 8);
		float t = (float)(int16_t)salsa64_rd16(src + 10);
		const uint8_t *c = src + 12;
		v->s = s * ks;
		v->t = t * kt;

		if (lighting) {
			float nrm[3] = { (float)(int8_t)c[0], (float)(int8_t)c[1], (float)(int8_t)c[2] };
			normalize3(nrm);
			const struct gfx_light *amb = &g->lights[g->num_lights];
			float rgb[3] = { amb->col[0], amb->col[1], amb->col[2] };
			for (int l = 0; l < g->num_lights; l++) {
				const float *d = g->light_model[l];
				float k = nrm[0] * d[0] + nrm[1] * d[1] + nrm[2] * d[2];
				if (k > 0.0f) {
					rgb[0] += k * g->lights[l].col[0];
					rgb[1] += k * g->lights[l].col[1];
					rgb[2] += k * g->lights[l].col[2];
				}
			}
			if (acclaim_lighting)
				gfx_acclaim_light(g, v, rgb);
			for (int k = 0; k < 3; k++)
				v->c[k] = (uint8_t)clampf(rgb[k] + 0.5f, 0.0f, 255.0f);
			v->c[3] = c[3];

			/* F3DFLX: s y t quedan las del vértice y el alfa sale de su
			 * tabla (flx.c). */
			if (flx_tab)
				v->c[3] = gfx_flx_alpha(g, flx_tab, c);
			else if (g->geom & G_TEXTURE_GEN)
				gfx_texgen_vertex(g, nrm, v);
		} else if (acclaim_lighting) {
			float rgb[3] = { (float)c[0], (float)c[1], (float)c[2] };
			gfx_acclaim_light(g, v, rgb);
			for (int k = 0; k < 3; k++)
				v->c[k] = (uint8_t)clampf(rgb[k] + 0.5f, 0.0f, 255.0f);
			v->c[3] = c[3];
		} else {
			v->c[0] = c[0];
			v->c[1] = c[1];
			v->c[2] = c[2];
			v->c[3] = c[3];
		}

		if (g->geom & G_FOG) {
			/* Alfa = factor de niebla según la profundidad (z/w se fija
			 * en −1 delante del plano cercano o detrás de la cámara). */
			float zw = v->w > 0.0f && v->z >= -v->w ? v->z * iw : -1.0f;
			float f = zw * g->fog_mul + g->fog_off;
			v->c[3] = (uint8_t)clampf(f + 0.5f, 0.0f, 255.0f);
		}
	}
}

void gfx_vtx_out(const struct gfx_state *g, const struct gfx_vtx *v, struct gfx_vertex *o)
{
	o->x = g->vtrans[0] * v->w + g->vscale[0] * v->x;
	o->y = g->vtrans[1] * v->w - g->vscale[1] * v->y;
	if (g->ol & G_ZS_PRIM)
		o->z = (float)g->prim_z / 32767.0f * v->w;
	else
		o->z = (g->vtrans[2] * v->w + g->vscale[2] * v->z) * (1.0f / 1023.0f);
	o->w = v->w;
	o->s = v->s;
	o->t = v->t;
	o->r = v->c[0];
	o->g = v->c[1];
	o->b = v->c[2];
	o->a = v->c[3];
	o->pad = v->clip >> GFX_CLIP_ID_SHIFT;  /* GFX_VGID con GFX_JOB_INTERP; si no, 0 */
}
