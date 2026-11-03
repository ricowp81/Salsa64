/* SPDX-License-Identifier: MIT */
/*
 * rsxkit — dibujo 3D: triángulos de un arreglo de rsxkit_vertex en el
 * destino ligado, con la combinación lineal de dos texturas y el color del
 * vértice (shaders/rsxkit_lincomb*.{vcg,fcg}) o con un fragment program
 * generado para los términos del dibujo (fpgen.c, fpcache.c).
 *
 * El vertex program lleva las coordenadas lógicas al espacio de recorte y
 * arma los siete términos X_j con constantes por dibujo; hay tres fragment
 * programs precompilados (sin textura, una, dos) que solo suman los cinco
 * primeros, y los generados multiplican sus factores por cada X_j. Todo el
 * estado se recuerda y solo se reenvía lo que cambia: un núcleo puede
 * mandar cientos de dibujos por cuadro sin llenar el búfer de comandos de
 * estado repetido.
 *
 * Con rsxkit_vertices_prev, el mismo vertex program con un arreglo más de
 * posiciones (rsxkit_lincomb_lerp.vcg): cada vértice va en prev + (pos −
 * prev)·t, para dibujar un cuadro intermedio entre dos con la misma lista.
 *
 * Sombreado lineal en pantalla (rsxkit_affine_shade): el vertex program
 * multiplica los siete X_j por w (constante «affine») y los fragment
 * programs los leen con el bit de SRC2 que quita la corrección de
 * perspectiva (fprog.c, fpgen.c): el RSX interpola X_j·w/w = X_j lineal en
 * pantalla, como el RDP de la N64. Hay una copia de cada programa
 * precompilado con ese bit y los generados lo llevan en su descripción.
 *
 * Profundidad: z/w en [0, 1] con LEQUAL (LESS con RSXKIT_DEPTH_LESS); sin recorte por los planos
 * cercano y lejano (la z se satura), como hacen los microcódigos de N64
 * sin recorte cercano. El recorte por w > 0 lo sigue haciendo el RSX. Con
 * RSXKIT_DEPTH_CONST el viewport lleva la z de todo vértice a la del
 * dibujo (escala 0): la división entre w redondea distinto en cada vértice
 * y deja pendientes de un bit que, entre dibujos coplanares, deciden al
 * azar quién pasa la prueba.
 */
#include <string.h>

#include "internal.h"

/* Método del NV40 para el ancho de línea (nv40.h: NV40TCL_LINE_WIDTH). */
#define NV40_LINE_WIDTH 0x3b8u

extern const u8 rsxkit_lincomb_vpo[];
extern const u8 rsxkit_lincomb_lerp_vpo[];
extern const u8 rsxkit_lincomb0_fpo[];
extern const u8 rsxkit_lincomb1_fpo[];
extern const u8 rsxkit_lincomb2_fpo[];

/* Constantes del vertex program, en su orden: space, xf[2], k[21], fog,
 * fogsel[2], affine. */
#define NCONST   28
#define C_K      3
#define C_FOG    24
#define C_FOGSEL 25
#define C_AFFINE 27

struct tex_state {
	int valid;
	rsxkit_texture t;
};

/* Un vertex program y sus constantes (el interpolado agrega «lerp»). */
struct vprog {
	rsxVertexProgram *vp;
	void *ucode;
	u32 const_start;
	rsxProgramConst *consts[NCONST];
	rsxProgramConst *lerp;
	int block;  /* constantes consecutivas: se suben de una vez */
};

static int incremental;
void rsxkit_3d_incremental(int enabled) { incremental = !!enabled; }
/* 1: recargar el programa y escribir su etiqueta de uso en cada dibujo (lo
 * de antes); 0 (inicial): solo al cambiar de programa y al salir del 3D. */
static int program_sync_per_draw;
void rsxkit_3d_program_sync(int per_draw) { program_sync_per_draw = !!per_draw; }

static struct {
	int stencil_valid;
	uint64_t stencil_key;
	int ready;
	rsxkit_program *cur_program; /* programa del usuario cargado (NULL: ninguno o precompilado) */
	int program_pending;         /* dibujos con cur_program sin etiqueta de uso */
	int color_aux;               /* COLOR0 desde la palabra `pad` del vértice */
	struct vprog prog[2];   /* 0: el de siempre; 1: con posiciones interpoladas */
	rsxFragmentProgram *fp[3];
	u32 fp_offset[3];
	rsxFragmentProgram *fp_aff[3];  /* los mismos, con los X_j sin corrección de perspectiva */
	u32 fp_aff_offset[3];
	const void *cur_fp;     /* programa cargado (g->fp_3d >= 0): precompilado o de la caché */
	uint32_t cur_fp_gen;    /* generación de la caché cuando se cargó */
	float c[NCONST][4];
	int c_valid;
	float lerp_t;           /* la de la constante «lerp» subida, si lerp_valid */
	int lerp_valid;
	/* Estado puesto. */
	uint8_t blend, cull, depth, alpha_ref;
	uint8_t line_width;  /* ancho de línea (6.3) de las aristas; 0: no se sabe */
	struct tex_state tex[2];
	uint32_t vb_offset;
	uint8_t vb_location;
	int vb_set;
	/* rsxkit_vertices_prev: posiciones de antes y la fracción. */
	uint32_t prev_offset;
	uint8_t prev_location;
	int prev_set;
	int prev_bound;         /* TEXCOORD1 ligado (si no, nunca se tocó: sin comandos de más) */
	float t;
} d3;

/* rsxkit_affine_shade; fuera de d3, que se borra al cerrar el 3D. */
static int affine_on;

static const char *const const_names[NCONST] = {
	"space", "xf[0]", "xf[1]", "k[0]", "k[1]", "k[2]", "k[3]", "k[4]", "k[5]", "k[6]", "k[7]",
	"k[8]", "k[9]", "k[10]", "k[11]", "k[12]", "k[13]", "k[14]", "k[15]", "k[16]", "k[17]", "k[18]",
	"k[19]", "k[20]", "fog", "fogsel[0]", "fogsel[1]", "affine",
};

void rsxkit_affine_shade(int on)
{
	affine_on = on != 0;
}

static int vprog_init(struct vprog *p, const u8 *vpo, int lerp)
{
	u32 size;
	p->vp = (rsxVertexProgram *)vpo;
	rsxVertexProgramGetUCode(p->vp, &p->ucode, &size);
	p->const_start = p->vp->const_start;
	p->block = 1;
	for (int i = 0; i < NCONST; i++) {
		p->consts[i] = rsxVertexProgramGetConst(p->vp, const_names[i]);
		if (!p->consts[i])
			return RSXKIT_EINIT;
		if (p->consts[i]->index != p->consts[0]->index + (u32)i)
			p->block = 0;
	}
	p->lerp = lerp ? rsxVertexProgramGetConst(p->vp, "lerp") : NULL;
	return lerp && !p->lerp ? RSXKIT_EINIT : RSXKIT_OK;
}

static int init_3d(void)
{
	if (d3.ready)
		return RSXKIT_OK;
	if (vprog_init(&d3.prog[0], rsxkit_lincomb_vpo, 0) != RSXKIT_OK ||
	    vprog_init(&d3.prog[1], rsxkit_lincomb_lerp_vpo, 1) != RSXKIT_OK)
		return RSXKIT_EINIT;
	const u8 *fps[3] = { rsxkit_lincomb0_fpo, rsxkit_lincomb1_fpo, rsxkit_lincomb2_fpo };
	for (int i = 0; i < 3; i++) {
		d3.fp[i] = (rsxFragmentProgram *)fps[i];
		d3.fp_aff[i] = (rsxFragmentProgram *)fps[i];
		if (!rsxkit_fp_upload(d3.fp[i], &d3.fp_offset[i]) ||
		    !rsxkit_fp_upload_affine(d3.fp_aff[i], &d3.fp_aff_offset[i]))
			return RSXKIT_ENOMEM;
	}
	d3.ready = 1;
	return RSXKIT_OK;
}

void rsxkit_3d_shutdown(void)
{
	rsxkit_fp_shutdown();
	memset(&d3, 0, sizeof d3);
}

void rsxkit_forget_state(void)
{
	struct rsxkit_state *g = &rsxkit_g;
	rsxkit_reserve(80);
	for (int i = 0; i < 16; i++)
		rsxBindVertexArrayAttrib(g->ctx, i, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	g->arrays_bound = 0;
	d3.prev_bound = 0;
	rsxkit_leave_3d();
	g->screen_bound = 0;
	rsxkit_bound.target = NULL;
	rsxkit_bound.depth = NULL;
	rsxkit_bound.scissor_valid = 0;
}

/* Etiqueta de uso del programa con dibujos pendientes: antes de cambiar de
 * programa, al salir del 3D y antes de destruir un programa. */
void rsxkit_3d_flush_use(void)
{
	if (d3.program_pending && d3.cur_program)
		rsxkit_program_used_3d(d3.cur_program);
	d3.program_pending = 0;
}

void rsxkit_leave_3d(void)
{
	struct rsxkit_state *g = &rsxkit_g;
	rsxkit_3d_flush_use();
	d3.cur_program = NULL;
	if (g->arrays_bound) {
		rsxkit_reserve(80);
		for (int i = 0; i < 16; i++)
			rsxBindVertexArrayAttrib(g->ctx, i, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
		g->arrays_bound = 0;
		d3.prev_bound = 0;
	}
	g->prog_3d = 0;
	g->fp_3d = -1;
	g->state_valid = 0;
	d3.c_valid = 0;
}

void rsxkit_3d_forget_vertices(void)
{
	d3.vb_set = 0;
	d3.prev_set = 0;
	rsxkit_g.arrays_bound = 0;
}

void rsxkit_vertices(uint32_t offset, uint8_t location)
{
	struct rsxkit_state *g = &rsxkit_g;
	d3.vb_offset = offset;
	d3.vb_location = location;
	d3.vb_set = 1;
	d3.prev_set = 0;
	g->arrays_bound = 0;  /* se vuelven a ligar en el próximo dibujo */
	rsxkit_reserve(32);
	/* Lo que hay en memoria cambió: vértices y texturas nuevas. */
	rsxInvalidateVertexCache(g->ctx);
	rsxInvalidateTextureCache(g->ctx, GCM_INVALIDATE_TEXTURE);
	d3.tex[0].valid = d3.tex[1].valid = 0;
}

void rsxkit_vertices_color_aux(int aux)
{
	aux = !!aux;
	if (d3.color_aux != aux)
		rsxkit_g.arrays_bound = 0;
	d3.color_aux = aux;
}

void rsxkit_vertices_prev(uint32_t offset, uint8_t location, float t)
{
	if (!d3.prev_set || d3.prev_offset != offset || d3.prev_location != location)
		rsxkit_g.arrays_bound = 0;
	d3.prev_offset = offset;
	d3.prev_location = location;
	d3.prev_set = 1;
	d3.t = t;
}

static void bind_arrays(void)
{
	struct rsxkit_state *g = &rsxkit_g;
	u8 loc = d3.vb_location == RSXKIT_MEM_MAIN ? GCM_LOCATION_CELL : GCM_LOCATION_RSX;
	u32 o = d3.vb_offset;
	const u8 stride = sizeof(rsxkit_vertex);
	rsxBindVertexArrayAttrib(g->ctx, GCM_VERTEX_ATTRIB_POS, 0, o + 0, stride, 4, GCM_VERTEX_DATA_TYPE_F32, loc);
	rsxBindVertexArrayAttrib(g->ctx, GCM_VERTEX_ATTRIB_TEX0, 0, o + 16, stride, 2, GCM_VERTEX_DATA_TYPE_F32, loc);
	rsxBindVertexArrayAttrib(g->ctx, GCM_VERTEX_ATTRIB_COLOR0, 0, o + (d3.color_aux ? 28 : 24), stride, 4,
				 GCM_VERTEX_DATA_TYPE_U8, loc);
	if (d3.prev_set) {
		u8 ploc = d3.prev_location == RSXKIT_MEM_MAIN ? GCM_LOCATION_CELL : GCM_LOCATION_RSX;
		rsxBindVertexArrayAttrib(g->ctx, GCM_VERTEX_ATTRIB_TEX1, 0, d3.prev_offset, 16, 4,
					 GCM_VERTEX_DATA_TYPE_F32, ploc);
		d3.prev_bound = 1;
	} else if (d3.prev_bound) {
		rsxBindVertexArrayAttrib(g->ctx, GCM_VERTEX_ATTRIB_TEX1, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32,
					 GCM_LOCATION_RSX);
		d3.prev_bound = 0;
	}
	g->arrays_bound = 1;
}

/* Estado fijo del 3D: se pone una vez por destino ligado. */
static void fixed_state(void)
{
	struct rsxkit_state *g = &rsxkit_g;
	gcmContextData *ctx = g->ctx;
	rsxSetColorMaskMrt(ctx, 0);
	rsxSetDitherEnable(ctx, GCM_FALSE);
	rsxSetShadeModel(ctx, GCM_SHADE_MODEL_SMOOTH);
	rsxSetZMinMaxControl(ctx, GCM_FALSE, GCM_TRUE, GCM_FALSE);
	rsxSetDepthFunc(ctx, GCM_LEQUAL);
	rsxSetBlendEquation(ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
	/* Frente: antihorario con la y hacia arriba (en el espacio de recorte,
	 * antes del viewport). Verificado en RPCS3 y en la PS3 con gfxtest. */
	rsxSetFrontFace(ctx, GCM_FRONTFACE_CCW);
	rsxSetPolygonOffset(ctx, -1.0f, -2.0f);
	/* Forzar que el primer dibujo ponga todo. */
	d3.blend = d3.cull = d3.depth = d3.alpha_ref = 0xFF;
	d3.line_width = 0;
	d3.stencil_valid = 0;
	d3.tex[0].valid = d3.tex[1].valid = 0;
	rsxkit_bound.scissor_valid = 0;
	g->state_valid = 1;
}

/* La pasada del alfa de un dibujo con RSXKIT_DSTA_ONE (dsta.c): sin mezcla
 * y con la máscara de color en el alfa solo. */
#define DSTA_ALPHA_ONLY RSXKIT_DSTA_PASS

/* El alfa que queda en un destino con alfa (rsxkit_bound.alpha), según
 * rsxkit_draw.dst_alpha y con cualquier mezcla del color: el del fragmento
 * (factores ONE / ZERO del canal alfa), el del destino (la máscara de color
 * sin el alfa) o el mayor de los dos (ecuación MAX, que no mira los
 * factores). La mezcla no puede dar una constante: el canal alfa sale de
 * As·fs + Ad·fd y todos los factores multiplican (GCM_CONSTANT_ALPHA da
 * As·Ac, no Ac). RSXKIT_DSTA_ONE lo resuelve rsxkit_draw_triangles con dos
 * pasadas; aquí vale como el del fragmento. Sin alfa en el destino, el
 * canal de alfa de la mezcla no cuenta (ZERO / ONE). Las mezclas con
 * máximo o mínimo del alfa (ALPHA_MAX, REPLACE_MIN_ALPHA, KEEP_MIN_ALPHA)
 * traen su propia cuenta y no miran dst_alpha. */
static void set_blend(uint8_t blend, uint8_t dsta)
{
	gcmContextData *ctx = rsxkit_g.ctx;
	int alpha = rsxkit_bound.alpha;
	if (!alpha || dsta > DSTA_ALPHA_ONLY)
		dsta = RSXKIT_DSTA_FRAG;  /* sin alfa no cuenta: una sola clave */
	if (dsta == RSXKIT_DSTA_ONE)
		dsta = RSXKIT_DSTA_FRAG;
	if (dsta == DSTA_ALPHA_ONLY)
		blend = RSXKIT_BLEND_OFF;
	uint8_t key = (uint8_t)(blend | dsta << 5);
	if (key == d3.blend)
		return;
	d3.blend = key;
	bool own = blend == RSXKIT_BLEND_ALPHA_MAX || blend == RSXKIT_BLEND_REPLACE_MIN_ALPHA ||
		   blend == RSXKIT_BLEND_KEEP_MIN_ALPHA;
	bool max = !own && alpha && dsta == RSXKIT_DSTA_MAX;
	/* Factores del canal alfa en las mezclas que no traen los suyos: el del
	 * fragmento en un destino con alfa, el del destino en uno sin alfa. */
	u16 sa = alpha ? GCM_ONE : GCM_ZERO, da = alpha ? GCM_ZERO : GCM_ONE;
	/* La ecuación del alfa: máximo o mínimo en las mezclas que lo piden (y
	 * con RSXKIT_DSTA_MAX) y suma en las demás (fixed_state la deja en suma). */
	rsxSetBlendEquation(ctx, GCM_FUNC_ADD,
			    blend == RSXKIT_BLEND_ALPHA_MAX || max ? GCM_FUNC_MAX
			    : (blend == RSXKIT_BLEND_REPLACE_MIN_ALPHA || blend == RSXKIT_BLEND_KEEP_MIN_ALPHA)
				    ? GCM_FUNC_MIN
				    : GCM_FUNC_ADD);
	u32 mask = GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A;
	switch (blend) {
	case RSXKIT_BLEND_REPLACE_MIN_ALPHA:
	case RSXKIT_BLEND_KEEP_MIN_ALPHA:
		rsxSetBlendEnable(ctx, GCM_TRUE);
		rsxSetBlendFunc(ctx, blend == RSXKIT_BLEND_KEEP_MIN_ALPHA ? GCM_ZERO : GCM_ONE,
				blend == RSXKIT_BLEND_KEEP_MIN_ALPHA ? GCM_ONE : GCM_ZERO, GCM_ONE, GCM_ONE);
		break;
	case RSXKIT_BLEND_ALPHA_MAX:
		/* RGB como ALPHA; el alfa con MAX, que no mira los factores. */
		rsxSetBlendEnable(ctx, GCM_TRUE);
		rsxSetBlendFunc(ctx, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA, GCM_ONE, GCM_ONE);
		break;
	case RSXKIT_BLEND_ALPHA:
		rsxSetBlendEnable(ctx, GCM_TRUE);
		rsxSetBlendFunc(ctx, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA, sa, da);
		break;
	case RSXKIT_BLEND_ADD:
		rsxSetBlendEnable(ctx, GCM_TRUE);
		rsxSetBlendFunc(ctx, GCM_SRC_ALPHA, GCM_ONE, sa, da);
		break;
	case RSXKIT_BLEND_MUL:
		rsxSetBlendEnable(ctx, GCM_TRUE);
		rsxSetBlendFunc(ctx, GCM_ZERO, GCM_SRC_ALPHA, sa, da);
		break;
	case RSXKIT_BLEND_ADD_MUL:
		rsxSetBlendEnable(ctx, GCM_TRUE);
		rsxSetBlendFunc(ctx, GCM_ONE, GCM_SRC_ALPHA, sa, da);
		break;
	case RSXKIT_BLEND_ALPHA_INV:
		rsxSetBlendEnable(ctx, GCM_TRUE);
		rsxSetBlendFunc(ctx, GCM_ONE_MINUS_SRC_ALPHA, GCM_SRC_ALPHA, sa, da);
		break;
	case RSXKIT_BLEND_ADD_ONE:
		rsxSetBlendEnable(ctx, GCM_TRUE);
		rsxSetBlendFunc(ctx, GCM_ONE, GCM_ONE, sa, da);
		break;
	case RSXKIT_BLEND_NONE:
		rsxSetBlendEnable(ctx, GCM_FALSE);
		mask = 0;
		break;
	default:
		/* Sin mezcla del color: solo el mayor necesita la mezcla. */
		if (max) {
			rsxSetBlendEnable(ctx, GCM_TRUE);
			rsxSetBlendFunc(ctx, GCM_ONE, GCM_ZERO, GCM_ONE, GCM_ONE);
		} else {
			rsxSetBlendEnable(ctx, GCM_FALSE);
		}
		break;
	}
	if (alpha && !own && mask) {
		if (dsta == RSXKIT_DSTA_KEEP)
			mask &= ~(u32)GCM_COLOR_MASK_A;
		else if (dsta == DSTA_ALPHA_ONLY)
			mask = GCM_COLOR_MASK_A;
	}
	rsxSetColorMask(ctx, mask);
}

static void set_depth(uint8_t depth)
{
	gcmContextData *ctx = rsxkit_g.ctx;
	if (!rsxkit_bound.depth)
		depth = 0;
	if (depth == d3.depth)
		return;
	d3.depth = depth;
	/* Escribir sin probar: en el RSX (como en GL) la prueba apagada apaga
	 * también la escritura, así que va con la prueba encendida y ALWAYS.
	 * Rogue Squadron dibuja así el fondo (hangar, terreno, cielo) para
	 * dejar la z de cada cuadro sin borrarla: sin escribirla, los modelos
	 * se probaban contra la z de cuadros viejos y desaparecían a pedazos.
	 * La máscara de la Lente de la Verdad de Zelda (un TEXRECT que solo
	 * escribe z) necesita lo mismo. */
	u32 func = !(depth & RSXKIT_DEPTH_TEST) || (depth & RSXKIT_DEPTH_ALWAYS) ? GCM_ALWAYS
		   : (depth & RSXKIT_DEPTH_EQUAL)			       ? GCM_EQUAL
		   : (depth & RSXKIT_DEPTH_GEQUAL)			       ? GCM_GEQUAL
		   : (depth & RSXKIT_DEPTH_LESS)			       ? GCM_LESS
									       : GCM_LEQUAL;
	rsxSetDepthTestEnable(ctx, (depth & (RSXKIT_DEPTH_TEST | RSXKIT_DEPTH_WRITE)) ? GCM_TRUE : GCM_FALSE);
	rsxSetDepthWriteEnable(ctx, (depth & RSXKIT_DEPTH_WRITE) ? GCM_TRUE : GCM_FALSE);
	rsxSetPolygonOffsetFillEnable(ctx, (depth & RSXKIT_DEPTH_OFFSET) ? GCM_TRUE : GCM_FALSE);
	rsxSetDepthFunc(ctx, func);
}

/* z < 0: la de los vértices (el viewport que puso rsxkit_bind). */
static void set_zconst(float z)
{
	if (z == rsxkit_bound.vp_z)
		return;
	rsxkit_bound.vp_z = z;
	/* Como rsxkit_bind, salvo la z. */
	uint32_t w = rsxkit_bound.w, h = rsxkit_bound.h;
	f32 scale[4] = { w * 0.5f, h * -0.5f, z < 0.0f ? 0.5f : 0.0f, 0.0f };
	f32 offset[4] = { w * 0.5f, h * 0.5f, z < 0.0f ? 0.5f : z, 0.0f };
	rsxSetViewport(rsxkit_g.ctx, 0, 0, w, h, 0.0f, 1.0f, scale, offset);
}

static void set_cull(uint8_t cull)
{
	gcmContextData *ctx = rsxkit_g.ctx;
	if (cull == d3.cull)
		return;
	d3.cull = cull;
	if (cull == RSXKIT_CULL_NONE) {
		rsxSetCullFaceEnable(ctx, GCM_FALSE);
		return;
	}
	rsxSetCullFaceEnable(ctx, GCM_TRUE);
	rsxSetCullFace(ctx, cull == RSXKIT_CULL_FRONT ? GCM_CULL_FRONT : GCM_CULL_BACK);
}

static void set_alpha(uint8_t ref)
{
	gcmContextData *ctx = rsxkit_g.ctx;
	if (ref == d3.alpha_ref)
		return;
	d3.alpha_ref = ref;
	rsxSetAlphaTestEnable(ctx, ref ? GCM_TRUE : GCM_FALSE);
	if (ref)
		rsxSetAlphaFunc(ctx, GCM_GEQUAL, ref);
}

/* r: las tijeras en píxeles del destino (rsxkit_logical_rect), no vacías. */
static void set_scissor(const uint32_t r[4])
{
	uint32_t w = r[2] - r[0], h = r[3] - r[1];
	uint32_t v[4] = { r[0], r[1], w, h };
	if (rsxkit_bound.scissor_valid && !memcmp(v, rsxkit_bound.scissor, sizeof v))
		return;
	memcpy(rsxkit_bound.scissor, v, sizeof v);
	rsxkit_bound.scissor_valid = 1;
	rsxSetScissor(rsxkit_g.ctx, r[0], r[1], w, h);
}

/* Formatos de textura que lee el 3D (los mismos del framebuffer 2D). */
static void texture_format(uint8_t fmt, u8 *gcm, u32 *remap)
{
#define REMAP(a, r, g, b)                                                                    \
	GCM_TEXTURE_REMAP_MODE(GCM_TEXTURE_REMAP_ORDER_XYXY, a, r, g, b,                      \
			       GCM_TEXTURE_REMAP_TYPE_REMAP, GCM_TEXTURE_REMAP_TYPE_REMAP,     \
			       GCM_TEXTURE_REMAP_TYPE_REMAP, GCM_TEXTURE_REMAP_TYPE_REMAP)
	switch (fmt) {
	case RSXKIT_RGBA5551:
		*gcm = GCM_TEXTURE_FORMAT_R5G6B5A1;
		*remap = REMAP(GCM_TEXTURE_REMAP_COLOR_A, GCM_TEXTURE_REMAP_COLOR_R, GCM_TEXTURE_REMAP_COLOR_G,
			       GCM_TEXTURE_REMAP_COLOR_B);
		break;
	case RSXKIT_ARGB8888:
	case RSXKIT_XRGB8888:
		*gcm = GCM_TEXTURE_FORMAT_A8R8G8B8;
		*remap = REMAP(GCM_TEXTURE_REMAP_COLOR_A, GCM_TEXTURE_REMAP_COLOR_R, GCM_TEXTURE_REMAP_COLOR_G,
			       GCM_TEXTURE_REMAP_COLOR_B);
		break;
	default:  /* RGBA8888: bytes R, G, B, A leídos como A, R, G, B */
		*gcm = GCM_TEXTURE_FORMAT_A8R8G8B8;
		*remap = REMAP(GCM_TEXTURE_REMAP_COLOR_B, GCM_TEXTURE_REMAP_COLOR_A, GCM_TEXTURE_REMAP_COLOR_R,
			       GCM_TEXTURE_REMAP_COLOR_G);
		break;
	}
#undef REMAP
}

static u8 wrap_mode(uint8_t w)
{
	return w == RSXKIT_WRAP_REPEAT ? GCM_TEXTURE_REPEAT
	       : w == RSXKIT_WRAP_MIRROR ? GCM_TEXTURE_MIRRORED_REPEAT
	       : w == RSXKIT_WRAP_MIRROR_ONCE ? GCM_TEXTURE_MIRROR_ONCE_CLAMP_TO_EDGE
					 : GCM_TEXTURE_CLAMP_TO_EDGE;
}

static void set_texture(int unit, const rsxkit_texture *t)
{
	gcmContextData *ctx = rsxkit_g.ctx;
	struct tex_state *s = &d3.tex[unit];
	/* xf va en las constantes, no en la unidad de textura. */
	if (s->valid && !memcmp(&s->t, t, offsetof(rsxkit_texture, xf)))
		return;
	s->valid = 1;
	s->t = *t;

	gcmTexture tex;
	memset(&tex, 0, sizeof tex);
	u8 gcm;
	u32 remap;
	texture_format(t->format, &gcm, &remap);
	if (t->remap == RSXKIT_REMAP_RRRR)
		remap = GCM_TEXTURE_REMAP_MODE(GCM_TEXTURE_REMAP_ORDER_XYXY, GCM_TEXTURE_REMAP_COLOR_R,
					       GCM_TEXTURE_REMAP_COLOR_R, GCM_TEXTURE_REMAP_COLOR_R,
					       GCM_TEXTURE_REMAP_COLOR_R, GCM_TEXTURE_REMAP_TYPE_REMAP,
					       GCM_TEXTURE_REMAP_TYPE_REMAP, GCM_TEXTURE_REMAP_TYPE_REMAP,
					       GCM_TEXTURE_REMAP_TYPE_REMAP);
	tex.format = gcm | GCM_TEXTURE_FORMAT_LIN | GCM_TEXTURE_FORMAT_NRM;
	tex.mipmap = 1;
	tex.dimension = GCM_TEXTURE_DIMS_2D;
	tex.cubemap = GCM_FALSE;
	tex.remap = remap;
	tex.width = t->width;
	tex.height = t->height;
	tex.depth = 1;
	tex.location = t->location == RSXKIT_MEM_MAIN ? GCM_LOCATION_CELL : GCM_LOCATION_RSX;
	tex.pitch = t->pitch;
	tex.offset = t->offset;
	rsxLoadTexture(ctx, (u8)unit, &tex);
	rsxTextureControl(ctx, (u8)unit, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
	u8 filt = t->filter == RSXKIT_FILTER_NEAREST ? GCM_TEXTURE_NEAREST : GCM_TEXTURE_LINEAR;
	rsxTextureFilter(ctx, (u8)unit, 0, filt, filt, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	rsxTextureWrapMode(ctx, (u8)unit, wrap_mode(t->wrap_s), wrap_mode(t->wrap_t), GCM_TEXTURE_CLAMP_TO_EDGE, 0,
			   GCM_TEXTURE_ZFUNC_LESS, 0);
}

static void set_constants(const rsxkit_draw *d, const struct vprog *p)
{
	gcmContextData *ctx = rsxkit_g.ctx;
	if (p->lerp && (!d3.lerp_valid || d3.lerp_t != d3.t)) {
		const float l[4] = { d3.t, 0.0f, 0.0f, 0.0f };
		rsxLoadVertexProgramParameterBlock(ctx, p->const_start + p->lerp->index, 1, l);
		d3.lerp_t = d3.t;
		d3.lerp_valid = 1;
	}
	float c[NCONST][4];
	memset(c, 0, sizeof c);
	c[0][0] = 2.0f / rsxkit_bound.lw;
	c[0][1] = 2.0f / rsxkit_bound.lh;
	for (int i = 0; i < 2; i++)
		if (i < d->ntex)
			memcpy(c[1 + i], d->tex[i].xf, sizeof c[1 + i]);
	memcpy(c[C_K], d->k, sizeof d->k);
	if (d->fog) {
		memcpy(c[C_FOG], d->fog_color, sizeof c[C_FOG]);
		c[C_FOG][3] = 1.0f;
		/* El color de niebla va al término constante: X4 en la combinación
		 * lineal, el primero sin factores en un programa generado. */
		int fog_term = RSXKIT_K_ONE;
		if (d->nterms) {
			fog_term = -1;
			for (int j = 0; j < d->nterms && fog_term < 0; j++)
				if (!d->factors[j][0])
					fog_term = j;
		}
		if (fog_term >= 0)
			c[C_FOGSEL + fog_term / 4][fog_term % 4] = 1.0f;
	}
	c[C_AFFINE][0] = affine_on ? 1.0f : 0.0f;
	unsigned first = 0, end = NCONST;
	if (d3.c_valid) {
		while (first < end && !memcmp(c[first], d3.c[first], sizeof c[0])) first++;
		if (first == end) return;
		while (end > first && !memcmp(c[end-1], d3.c[end-1], sizeof c[0])) end--;
	}
	if (!incremental) { first = 0; end = NCONST; }
	memcpy(d3.c + first, c + first, (end - first) * sizeof c[0]);
	d3.c_valid = 1;
	if (p->block) {
		/* Un tramo contiguo, con los mismos valores y registros del bloque completo. */
		rsxLoadVertexProgramParameterBlock(ctx, p->const_start + p->consts[first]->index,
		                                   end - first, &c[first][0]);
	} else {
		for (unsigned i = first; i < end; i++)
			rsxSetVertexProgramParameter(ctx, p->vp, p->consts[i], c[i]);
	}
}

static void draw_triangles(const rsxkit_draw *d, uint32_t first, uint32_t count)
{
	struct rsxkit_state *g = &rsxkit_g;
	gcmContextData *ctx = g->ctx;
	if (!count || !d3.vb_set || !rsxkit_bound.target || g->screen_bound || init_3d() != RSXKIT_OK)
		return;
	/* Con las tijeras vacías no hay nada que dibujar: no se manda (unas
	 * tijeras de ancho o alto 0 no son algo que el RSX tenga que ver). */
	uint32_t sc[4];
	rsxkit_logical_rect(d->scissor, sc);
	if (sc[2] <= sc[0] || sc[3] <= sc[1])
		return;
	rsxkit_trace_repeat("dibujar", "%u vértices; el último desde el %u, tijeras %u,%u", count, first, sc[0], sc[1]);
	/* Lo más que escribe un dibujo: programa de vértices (~140), constantes
	 * (~140), programa de fragmentos (~22), texturas, estado y el dibujo. */
	rsxkit_reserve(RSXKIT_RESERVE_DRAW + 64u);
	if (!g->state_valid)
		fixed_state();
	/* prog_3d: 1 el de siempre, 2 el interpolado (las constantes de uno
	 * no valen para el otro). */
	const struct vprog *vp = &d3.prog[d3.prev_set];
	if (g->prog_3d != 1 + d3.prev_set) {
		rsxLoadVertexProgram(ctx, vp->vp, vp->ucode);
		g->prog_3d = 1 + d3.prev_set;
		d3.c_valid = 0;
		d3.lerp_valid = 0;
	}
	/* Programa: precompilado por número de texturas, o generado para los
	 * términos del dibujo (y cacheado). */
	int ntex = d->ntex > 2 ? 2 : d->ntex;
	/* Los precompilados: la copia afín es otro programa (su propio puntero
	 * distingue las dos en cur_fp). */
	const void *want = affine_on ? (const void *)&d3.fp_aff[ntex] : (const void *)d3.fp[ntex];
	const rsxFragmentProgram *hdr = d3.fp[ntex];
	u32 offset = affine_on ? d3.fp_aff_offset[ntex] : d3.fp_offset[ntex];
	if (d->nterms) {
		rsxkit_fp_desc desc;
		memset(&desc, 0, sizeof desc);
		desc.ntex = (uint8_t)ntex;
		desc.nterms = d->nterms > RSXKIT_K_TERMS ? RSXKIT_K_TERMS : d->nterms;
		desc.affine = (uint8_t)affine_on;
		memcpy(desc.factors, d->factors, sizeof desc.factors);
		const struct rsxkit_fp_cached *fc = rsxkit_fp_lookup(&desc);
		if (!fc)
			return;  /* descripción inválida o sin memoria: no se dibuja */
		want = fc;
		hdr = &fc->hdr;
		offset = fc->offset;
	}
	if (d->program) {
		/* Cargar el programa solo al cambiar: recargarlo en cada dibujo
		 * vuelve a traer el microcódigo y escribe su etiqueta de uso, una
		 * barrera del back end por dibujo. */
		if (program_sync_per_draw || d3.cur_program != d->program) {
			rsxkit_3d_flush_use();
			if (rsxkit_program_bind_3d(d->program) != RSXKIT_OK) return;
			d3.cur_program = d->program;
		}
		g->fp_3d = -1;
	} else {
		if (d3.cur_program) { rsxkit_3d_flush_use(); d3.cur_program = NULL; }
		if (g->fp_3d < 0 || d3.cur_fp != want || d3.cur_fp_gen != rsxkit_fp_generation()) {
			rsxLoadFragmentProgramLocation(ctx, hdr, offset, GCM_LOCATION_RSX);
			g->fp_3d = 1;
			d3.cur_fp = want;
			d3.cur_fp_gen = rsxkit_fp_generation();
			rsxkit_fp_count_load();
		}
	}
	if (!g->arrays_bound)
		bind_arrays();
	set_scissor(sc);
	set_blend(d->blend, d->dst_alpha);
	set_depth(d->depth);
	/* Sin prueba ni escritura la z no importa: no se toca el viewport. */
	if (d3.depth & (RSXKIT_DEPTH_TEST | RSXKIT_DEPTH_WRITE))
		set_zconst(d->depth & RSXKIT_DEPTH_CONST ? (d->z < 0.0f ? 0.0f : d->z > 1.0f ? 1.0f : d->z) : -1.0f);
	set_cull(d->cull);
	set_alpha(d->alpha_ref);
	uint64_t sk = (uint64_t)d->stencil | ((uint64_t)d->stencil_ref << 8) |
	              ((uint64_t)d->stencil_compare_mask << 16) | ((uint64_t)d->stencil_write_mask << 24) |
	              ((uint64_t)d->stencil_op << 32) | ((uint64_t)d->stencil_zfail << 40) |
	              ((uint64_t)d->stencil_sfail << 48);
	if (!incremental || !d3.stencil_valid || d3.stencil_key != sk) {
		rsxSetStencilTestEnable(ctx, d->stencil ? GCM_TRUE : GCM_FALSE);
		if (d->stencil) {
			rsxSetStencilMask(ctx, d->stencil_write_mask);
			rsxSetStencilFunc(ctx, d->stencil == 2 ? GCM_NOTEQUAL : d->stencil == 3 ? GCM_EQUAL : GCM_ALWAYS, d->stencil_ref, d->stencil_compare_mask);
			rsxSetStencilOp(ctx, d->stencil_sfail == 2 ? GCM_ZERO : d->stencil_sfail ? GCM_REPLACE : GCM_KEEP,
			                d->stencil_zfail == 2 ? GCM_ZERO : d->stencil_zfail ? GCM_REPLACE : GCM_KEEP,
			                d->stencil_op == 2 ? GCM_KEEP : d->stencil_op ? GCM_INVERT : GCM_REPLACE);
		}
		d3.stencil_key = sk; d3.stencil_valid = 1;
	}
	for (int i = 0; i < ntex; i++)
		set_texture(i, &d->tex[i]);
	set_constants(d, vp);
	if (d->edges) {
		/* Las aristas primero (rsxkit_draw.edges); el modo de polígono no
		 * cambia la eliminación de caras: van solo las de los triángulos que
		 * se dibujan. La z de un píxel de línea es la del punto de la arista,
		 * no la del centro del píxel: con LEQUAL le ganaría al relleno en los
		 * dibujos con mucha pendiente de z (tiras finas). Desplazadas hacia
		 * atrás, el relleno siempre pasa donde cubre; con 8 veces la
		 * pendiente, tampoco le ganan a piezas vecinas casi coplanares ya
		 * dibujadas, y en las grietas lo de atrás está lejos. */
		if (d->edges != d3.line_width) {
			/* PSL1GHT no tiene rsxSetLineWidth: el método del NV40, con el
			 * ancho en punto fijo 6.3 (como nouveau). */
			u32 *p = (u32 *)ctx->current;
			p[0] = (1u << 18) | NV40_LINE_WIDTH;
			p[1] = d->edges;
			ctx->current = p + 2;
			d3.line_width = d->edges;
		}
		rsxSetPolygonOffset(ctx, 8.0f, 64.0f);
		rsxSetPolygonOffsetLineEnable(ctx, GCM_TRUE);
		rsxSetFrontPolygonMode(ctx, GCM_POLYGON_MODE_LINE);
		rsxSetBackPolygonMode(ctx, GCM_POLYGON_MODE_LINE);
		rsxDrawVertexArray(ctx, GCM_TYPE_TRIANGLES, first, count);
		rsxSetFrontPolygonMode(ctx, GCM_POLYGON_MODE_FILL);
		rsxSetBackPolygonMode(ctx, GCM_POLYGON_MODE_FILL);
		rsxSetPolygonOffsetLineEnable(ctx, GCM_FALSE);
		rsxSetPolygonOffset(ctx, -1.0f, -2.0f);  /* el de las calcomanías (fixed_state) */
	}
	rsxDrawVertexArray(ctx, GCM_TYPE_TRIANGLES, first, count);
	if (d->program) {
		if (program_sync_per_draw) rsxkit_program_used_3d(d->program);
		else d3.program_pending = 1;
	}
}

/* Contadores de la pasada del alfa (rsxkit_dsta_stats). */
static uint32_t dsta_passes, dsta_single, dsta_failed;

void rsxkit_dsta_stats(uint32_t *passes, uint32_t *single, uint32_t *failed)
{
	*passes = dsta_passes;
	*single = dsta_single;
	*failed = dsta_failed;
}

void rsxkit_draw_triangles(const rsxkit_draw *d, uint32_t first, uint32_t count)
{
	bool own = d->blend == RSXKIT_BLEND_ALPHA_MAX || d->blend == RSXKIT_BLEND_REPLACE_MIN_ALPHA ||
		   d->blend == RSXKIT_BLEND_KEEP_MIN_ALPHA || d->blend == RSXKIT_BLEND_NONE;
	if (!count || !rsxkit_bound.target || !rsxkit_bound.alpha || d->dst_alpha != RSXKIT_DSTA_ONE || own) {
		draw_triangles(d, first, count);
		return;
	}
	/* RSXKIT_DSTA_ONE en un destino con alfa: si el alfa del fragmento ya
	 * es 1, una pasada que lo deja tal cual; si no, primero la del alfa
	 * (rsxkit_dsta_alpha_pass, sin escribir la profundidad: las dos ven la
	 * misma) y después el dibujo sin tocar el alfa (RSXKIT_DSTA_KEEP;
	 * ninguna mezcla del color lee el alfa del destino). */
	rsxkit_draw a = *d;
	if (rsxkit_dsta_alpha_is_one(d)) {
		a.dst_alpha = RSXKIT_DSTA_FRAG;
		dsta_single++;
		draw_triangles(&a, first, count);
		return;
	}
	rsxkit_draw b;
	if (rsxkit_dsta_alpha_pass(d, &b)) {
		draw_triangles(&b, first, count);
		dsta_passes++;
		a.dst_alpha = RSXKIT_DSTA_KEEP;
	} else {
		a.dst_alpha = RSXKIT_DSTA_FRAG;  /* lo que se puede: el del fragmento */
		dsta_failed++;
	}
	draw_triangles(&a, first, count);
}
