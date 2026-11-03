/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — combiner de color de la N64 → términos para el RSX.
 *
 * El combiner del RDP calcula, por ciclo y por canal, (A − B) × C + D con
 * entradas que son texels (TEXEL0/1), el color de sombreado (SHADE),
 * constantes (PRIM, ENV, 1, 0…) y, en el segundo ciclo, el resultado del
 * primero (COMBINED). Aquí se evalúa de forma simbólica: cada valor es un
 * polinomio por canal cuyos monomios son productos de
 *
 *     T0, T0a, T1, T1a (texels y sus alfas) y S, Sa (sombreado y su alfa)
 *
 * con las constantes ya como números. Un monomio lleva cada textura como
 * mucho una vez (hasta dos texturas distintas) y hasta tres factores de
 * sombreado; lo que se pasa de eso se aproxima quedándose con el primer
 * factor y se cuenta.
 *
 * El resultado se deja en gfx_draw de una de dos formas:
 *
 * - Combinación lineal fija (nterms = 0): color = T0·X0 + T0a·X1 + T1·X2 +
 *   T1a·X3 + X4 con X_i = k[i][0] + k[i][1]·C + k[i][2]·C.aaaa (C es el
 *   color del vértice; los X_i los arma el vertex program). Exacta mientras
 *   ningún monomio multiplique dos texturas ni dos factores de sombreado.
 *   Una textura por su propio alfa (T0·T0a, los ojos de Mario en SM64) cabe
 *   pidiendo la textura premultiplicada (RGB·A, GFX_TEX_PREMUL) si el
 *   dibujo no usa también T0 solo. Es el camino rápido: programas
 *   precompilados en lib/rsxkit.
 *
 * - Términos para el generador de fragment programs (nterms > 0): color =
 *   Σ_j (Π factors[j]) · X_j, donde los factores son T0, T0a, T1, T1a, C y
 *   Ca (gfx.h, GFX_F_*; lib/rsxkit los traduce a un programa del RSX y lo
 *   cachea). Cubre los productos de texturas y de sombreado (la niebla por
 *   luz de GoldenEye, lerp(ENV, TEXEL0, SHADE_ALPHA)·SHADE). Los monomios
 *   se agrupan en el menor número de términos: cada X_j absorbe un factor
 *   de sombreado (k[j][1]·C o k[j][2]·C.aaaa), así que un término con
 *   factores F junta los monomios F, F·S y F·Sa.
 *
 * En el canal alfa, "el mismo canal" es el alfa: ahí T0 y S se guardan
 * siempre como T0a y Sa, así que COMBINED_ALPHA se puede llevar tal cual a
 * los canales de color; y en los términos del generador, el cuarto carril
 * de un factor T0 o T0a vale T0a y el de C o Ca vale Ca, por lo que un
 * monomio del alfa puede ir en cualquier término cuyos factores, vistos
 * desde el alfa, coincidan.
 *
 * Codificación de G_SETCOMBINE y entradas de cada slot: n64brew.dev
 * (Reality Display Processor/Commands, "Set Combine Mode"). En un ciclo se
 * usa la segunda mitad del combiner.
 */
#include <string.h>

#include "rsp/gfx/hle.h"

/* Monomio: exponentes de T0, T0a, T1, T1a, S y Sa, dos bits cada uno. */
typedef uint16_t mono;
enum { F_T0, F_T0A, F_T1, F_T1A, F_S, F_SA, F_N };
#define M_SHIFT(f)  (2 * (f))
#define M_EXP(m, f) (((m) >> M_SHIFT(f)) & 3u)
#define M_ONE(f)    ((mono)(1u << M_SHIFT(f)))

#define P_MAX 16
typedef struct {
	uint8_t n;
	mono m[P_MAX];
	float c[P_MAX];
} poly;

struct cc_ctx {
	float prim[4], env[4];
	float prim_lod, lod, noise, k4, k5;
	bool approx;
	bool swap;  /* segundo ciclo de 2CYCLE: TEXEL0 lee el texel de la tile+1 */
};

static void p_zero(poly *p)
{
	/* Solo n determina qué elementos son válidos; los demás no se leen. */
	p->n = 0;
}

/* Suma c·m a p; los coeficientes que quedan en cero desaparecen. */
static void p_term(poly *p, mono m, float c, struct cc_ctx *x)
{
	if (c == 0.0f)
		return;
	for (unsigned i = 0; i < p->n; i++)
		if (p->m[i] == m) {
			p->c[i] += c;
			if (p->c[i] == 0.0f) {
				p->n--;
				p->m[i] = p->m[p->n];
				p->c[i] = p->c[p->n];
			}
			return;
		}
	if (p->n == P_MAX) {
		x->approx = true;  /* demasiados monomios: se pierde uno */
		return;
	}
	p->m[p->n] = m;
	p->c[p->n] = c;
	p->n++;
}

/* Los constructores escriben en el resultado del llamador: poly ronda
 * los 100 bytes y devolverlo por valor inflaba las pilas de los SPE. */
static void p_const(poly *p, float v, struct cc_ctx *x)
{
	(void)x; /* un polinomio constante no puede desbordar P_MAX */
	p->n = 0;
	if (v != 0.0f) {
		p->m[0] = 0;
		p->c[0] = v;
		p->n = 1;
	}
}

static void p_mono(poly *p, int f)
{
	p->n = 1;
	p->m[0] = M_ONE(f);
	p->c[0] = 1.0f;
}

static void p_tex(poly *p, int f, struct cc_ctx *x)
{
	static const int swapped[F_N] = { F_T1, F_T1A, F_T0, F_T0A, F_S, F_SA };
	p_mono(p, x->swap ? swapped[f] : f);
}

static void p_sub(poly *a, const poly *b, struct cc_ctx *x)
{
	for (unsigned i = 0; i < b->n; i++)
		p_term(a, b->m[i], -b->c[i], x);
}

static void p_add(poly *a, const poly *b, struct cc_ctx *x)
{
	for (unsigned i = 0; i < b->n; i++)
		p_term(a, b->m[i], b->c[i], x);
}

/* Producto de dos monomios: cada textura a lo sumo una vez y dos texturas
 * como máximo, hasta tres factores de sombreado. Si no, se aproxima con
 * el primero. */
static mono mono_mul(mono a, mono b, struct cc_ctx *x)
{
	unsigned tex = 0, sh = 0;
	mono r = 0;
	for (int f = 0; f < F_N; f++) {
		unsigned e = M_EXP(a, f) + M_EXP(b, f);
		if (f < F_S) {
			if (e > 1)
				goto approx;
			tex += e;
		} else {
			sh += e;
		}
		r |= (mono)(e << M_SHIFT(f));
	}
	if (tex > 2 || sh > 3)
		goto approx;
	return r;
approx:
	x->approx = true;
	return a;
}

/* out = a·b, escrito directo en out: no puede ser a ni b. Sin un polinomio
 * temporal ni su copia de vuelta, el marco es chico; está en la cadena más
 * honda de la pila del SPE 0 (begin_prim → gfx_combine → run_cycle). */
static void p_mul(poly *out, const poly *a, const poly *b, struct cc_ctx *x)
{
	p_zero(out);
	for (unsigned i = 0; i < a->n; i++)
		for (unsigned j = 0; j < b->n; j++)
			p_term(out, mono_mul(a->m[i], b->m[j], x), a->c[i] * b->c[j], x);
}

/* Entradas de un canal de color (lane 0–2). comb = COMBINED de ese canal y
 * comb_a = COMBINED del canal alfa. */
/* Evita devolver y copiar poly por valor en el camino caliente del SPE. */
static void in_rgb(poly *out, int slot, unsigned v, int lane, const poly *comb,
		   const poly *comb_a, struct cc_ctx *x)
{
	switch (v) {
	case 0: *out = *comb; return;
	case 1: p_tex(out, F_T0, x); return;
	case 2: p_tex(out, F_T1, x); return;
	case 3: p_const(out, x->prim[lane], x); return;
	case 4: p_mono(out, F_S); return;
	case 5: p_const(out, x->env[lane], x); return;
	default: break;
	}
	switch (slot) {
	case 'a':  /* 6 ONE, 7 NOISE, 8–15 cero */
		p_const(out, v == 6 ? 1.0f : v == 7 ? x->noise : 0.0f, x);
		return;
	case 'b':  /* 6 CENTER, 7 K4, 8–15 cero */
		p_const(out, v == 7 ? x->k4 : 0.0f, x);
		return;
	case 'c':
		switch (v) {
		case 6: p_const(out, 0.0f, x); return;              /* SCALE */
		case 7: *out = *comb_a; return;                     /* COMBINED_ALPHA */
		case 8: p_tex(out, F_T0A, x); return;
		case 9: p_tex(out, F_T1A, x); return;
		case 10: p_const(out, x->prim[3], x); return;
		case 11: p_mono(out, F_SA); return;
		case 12: p_const(out, x->env[3], x); return;
		case 13: p_const(out, x->lod, x); return;           /* LOD_FRACTION */
		case 14: p_const(out, x->prim_lod, x); return;      /* PRIM_LOD_FRAC */
		case 15: p_const(out, x->k5, x); return;
		default: p_const(out, 0.0f, x); return;
		}
	default:   /* 'd': 6 ONE, 7 cero */
		p_const(out, v == 6 ? 1.0f : 0.0f, x);
		return;
	}
}

static void in_alpha(poly *out, int slot, unsigned v, const poly *comb_a, struct cc_ctx *x)
{
	if (slot == 'c') {
		switch (v) {
		case 0: p_const(out, x->lod, x); return;
		case 6: p_const(out, x->prim_lod, x); return;
		case 7: p_const(out, 0.0f, x); return;
		default: break;
		}
	} else if (v == 0) {
		*out = *comb_a;
		return;
	}
	switch (v) {
	case 1: p_tex(out, F_T0A, x); return;
	case 2: p_tex(out, F_T1A, x); return;
	case 3: p_const(out, x->prim[3], x); return;
	case 4: p_mono(out, F_SA); return;
	case 5: p_const(out, x->env[3], x); return;
	case 6: p_const(out, 1.0f, x); return;
	default: p_const(out, 0.0f, x); return;
	}
}

struct cc_cycle {
	unsigned a, b, c, d;      /* RGB */
	unsigned aa, ab, ac, ad;  /* alfa */
};

/* Campos de G_SETCOMBINE (n64brew.dev). */
static void unpack(uint32_t hi, uint32_t lo, struct cc_cycle cyc[2])
{
	cyc[0].a = (hi >> 20) & 0xF;
	cyc[0].c = (hi >> 15) & 0x1F;
	cyc[0].aa = (hi >> 12) & 0x7;
	cyc[0].ac = (hi >> 9) & 0x7;
	cyc[1].a = (hi >> 5) & 0xF;
	cyc[1].c = hi & 0x1F;
	cyc[0].b = (lo >> 28) & 0xF;
	cyc[1].b = (lo >> 24) & 0xF;
	cyc[1].aa = (lo >> 21) & 0x7;
	cyc[1].ac = (lo >> 18) & 0x7;
	cyc[0].d = (lo >> 15) & 0x7;
	cyc[0].ab = (lo >> 12) & 0x7;
	cyc[0].ad = (lo >> 9) & 0x7;
	cyc[1].d = (lo >> 6) & 0x7;
	cyc[1].ab = (lo >> 3) & 0x7;
	cyc[1].ad = lo & 0x7;
}

/* (A − B)·C + D por canal, con dos polinomios en el marco: A − B en a, C en
 * b, el producto directo en out[lane] y D sumado después (las entradas no
 * tienen efectos, el orden de las operaciones sobre los polinomios es el
 * mismo). out no puede ser comb: COMBINED se lee de comb después de escribir
 * out. run_cycle está en la cadena más honda de la pila del SPE 0. */
static void run_cycle(const struct cc_cycle *cy, poly out[4], const poly comb[4], struct cc_ctx *x)
{
	for (int lane = 0; lane < 3; lane++) {
		poly a, b;
		in_rgb(&a, 'a', cy->a, lane, &comb[lane], &comb[3], x);
		in_rgb(&b, 'b', cy->b, lane, &comb[lane], &comb[3], x);
		p_sub(&a, &b, x);
		in_rgb(&b, 'c', cy->c, lane, &comb[lane], &comb[3], x);
		p_mul(&out[lane], &a, &b, x);
		in_rgb(&a, 'd', cy->d, lane, &comb[lane], &comb[3], x);
		p_add(&out[lane], &a, x);
	}
	poly a, b;
	in_alpha(&a, 'a', cy->aa, &comb[3], x);
	in_alpha(&b, 'b', cy->ab, &comb[3], x);
	p_sub(&a, &b, x);
	in_alpha(&b, 'c', cy->ac, &comb[3], x);
	p_mul(&out[3], &a, &b, x);
	in_alpha(&a, 'd', cy->ad, &comb[3], x);
	p_add(&out[3], &a, x);
}

static void rgba(uint32_t c, float out[4])
{
	out[0] = (float)(c >> 24) / 255.0f;
	out[1] = (float)((c >> 16) & 0xFF) / 255.0f;
	out[2] = (float)((c >> 8) & 0xFF) / 255.0f;
	out[3] = (float)(c & 0xFF) / 255.0f;
}

/* ---- Combinación lineal fija ---- */

/* Término y lugar de la combinación lineal para un monomio; false si no
 * cabe (dos texturas, dos factores de sombreado). T·Ta va al término de T
 * (textura premultiplicada). */
static bool lincomb_slot(mono m, int *term, int *slot)
{
	unsigned s = M_EXP(m, F_S), sa = M_EXP(m, F_SA);
	if (s + sa > 1)
		return false;
	*slot = s ? 1 : sa ? 2 : 0;
	unsigned t0 = M_EXP(m, F_T0) + M_EXP(m, F_T0A), t1 = M_EXP(m, F_T1) + M_EXP(m, F_T1A);
	if (t0 && t1)
		return false;
	if (t0)
		*term = M_EXP(m, F_T0) ? GFX_K_T0 : GFX_K_T0A;
	else if (t1)
		*term = M_EXP(m, F_T1) ? GFX_K_T1 : GFX_K_T1A;
	else
		*term = GFX_K_ONE;
	return true;
}

static bool fit_lincomb(const poly out[4], struct gfx_draw *d)
{
	bool plain[2] = { false, false }, premul[2] = { false, false };
	for (int lane = 0; lane < 4; lane++)
		for (unsigned i = 0; i < out[lane].n; i++) {
			mono m = out[lane].m[i];
			int term, slot;
			if (!lincomb_slot(m, &term, &slot))
				return false;
			for (int t = 0; t < 2; t++) {
				unsigned e = M_EXP(m, t ? F_T1 : F_T0), ea = M_EXP(m, t ? F_T1A : F_T0A);
				if (e && ea)
					premul[t] = true;
				else if (e)
					plain[t] = true;
			}
		}
	/* T·Ta y T solo en el mismo dibujo: no hay una sola textura que sirva
	 * para los dos (va al generador). */
	if ((plain[0] && premul[0]) || (plain[1] && premul[1]))
		return false;
	memset(d->k, 0, sizeof d->k);
	bool t0 = false, t1 = false;
	for (int lane = 0; lane < 4; lane++)
		for (unsigned i = 0; i < out[lane].n; i++) {
			int term, slot;
			lincomb_slot(out[lane].m[i], &term, &slot);
			d->k[term][slot][lane] = out[lane].c[i];
			t0 |= term == GFX_K_T0 || term == GFX_K_T0A;
			t1 |= term == GFX_K_T1 || term == GFX_K_T1A;
		}
	for (int t = 0; t < 2; t++)
		if (premul[t])
			d->tex[t].flags |= GFX_TEX_PREMUL;
	d->nterms = 0;
	d->ntex = t1 ? 2 : t0 ? 1 : 0;
	return true;
}

/* ---- Términos para el generador ---- */

/* Un monomio del canal alfa, visto desde los términos: T0 y T0a son lo
 * mismo (T0a), S y Sa también (Sa). */
static mono canon(mono m)
{
	unsigned t0 = M_EXP(m, F_T0) + M_EXP(m, F_T0A), t1 = M_EXP(m, F_T1) + M_EXP(m, F_T1A);
	unsigned s = M_EXP(m, F_S) + M_EXP(m, F_SA);
	return (mono)(t0 << M_SHIFT(F_T0) | t1 << M_SHIFT(F_T1) | s << M_SHIFT(F_S));
}

/* Monomio que calcula una lista de factores en los canales de color. */
static mono factors_mono(const uint8_t f[GFX_K_FACTORS])
{
	static const mono of[] = { 0, M_ONE(F_T0), M_ONE(F_T0A), M_ONE(F_T1), M_ONE(F_T1A), M_ONE(F_S), M_ONE(F_SA) };
	mono m = 0;
	for (int k = 0; k < GFX_K_FACTORS && f[k]; k++)
		m = (mono)(m + of[f[k]]);
	return m;
}

/* Lista de factores para el monomio `m`: en forma de color (exponentes de
 * cada factor) o canónica del alfa (T0 dos veces = T0 y T0a; S tres veces
 * = C, Ca, C). */
static void mono_factors(mono m, bool alpha, uint8_t f[GFX_K_FACTORS])
{
	static const uint8_t color[F_N] = { GFX_F_T0, GFX_F_T0A, GFX_F_T1, GFX_F_T1A, GFX_F_C, GFX_F_CA };
	static const uint8_t pair[F_N][2] = { { GFX_F_T0, GFX_F_T0A }, { 0, 0 }, { GFX_F_T1, GFX_F_T1A },
					      { 0, 0 }, { GFX_F_C, GFX_F_CA }, { 0, 0 } };
	int n = 0;
	memset(f, 0, GFX_K_FACTORS);
	for (int k = 0; k < F_N; k++)
		for (unsigned e = 0; e < M_EXP(m, k) && n < GFX_K_FACTORS; e++)
			f[n++] = alpha ? pair[k][e & 1] : color[k];
}

/* 8 bytes (c primero, sin relleno): build_terms guarda 4·P_MAX en la pila. */
struct need {
	float c;
	mono m;       /* canónico en el canal alfa */
	uint8_t lane;
	bool done;
};

/* Candidatos de un monomio: el término con todos sus factores (lugar 0) o
 * sin un factor de sombreado, que absorbe X_j (lugar 1: S, lugar 2: Sa; en
 * el alfa los dos lugares valen Ca, se usa el 1). */
static int candidates(const struct need *nd, mono f[3], int slot[3])
{
	int n = 0;
	f[n] = nd->m, slot[n++] = 0;
	if (M_EXP(nd->m, F_S))
		f[n] = (mono)(nd->m - M_ONE(F_S)), slot[n++] = 1;
	if (nd->lane < 3 && M_EXP(nd->m, F_SA))
		f[n] = (mono)(nd->m - M_ONE(F_SA)), slot[n++] = 2;
	return n;
}

/* ¿El término de factores `f` sirve a este monomio? Devuelve el lugar o −1. */
static int term_serves(const uint8_t f[GFX_K_FACTORS], const struct need *nd)
{
	mono tm = factors_mono(f);
	if (nd->lane == 3)
		tm = canon(tm);
	mono cf[3];
	int slot[3], n = candidates(nd, cf, slot);
	for (int i = 0; i < n; i++)
		if (cf[i] == tm)
			return slot[i];
	return -1;
}

/* Aparte (noinline): sus needs[] (512 bytes) no se suman al marco de
 * gfx_combine mientras corre run_cycle, la cadena más honda de la pila del
 * SPE 0. Una llamada más por combiner calculado, no por vértice ni píxel. */
static __attribute__((noinline)) void build_terms(const poly out[4], struct gfx_draw *d, struct cc_ctx *x)
{
	struct need needs[4 * P_MAX];
	unsigned nneeds = 0;
	for (int lane = 0; lane < 4; lane++)
		for (unsigned i = 0; i < out[lane].n; i++) {
			struct need *nd = &needs[nneeds++];
			nd->m = lane == 3 ? canon(out[lane].m[i]) : out[lane].m[i];
			nd->c = out[lane].c[i];
			nd->lane = (uint8_t)lane;
			nd->done = false;
		}

	memset(d->k, 0, sizeof d->k);
	memset(d->factors, 0, sizeof d->factors);
	unsigned nterms = 0;
	for (;;) {
		/* El término que sirve a más monomios pendientes. */
		uint8_t best[GFX_K_FACTORS];
		int best_count = 0;
		for (unsigned i = 0; i < nneeds; i++) {
			if (needs[i].done)
				continue;
			mono cf[3];
			int slot[3], n = candidates(&needs[i], cf, slot);
			for (int c = 0; c < n; c++) {
				uint8_t f[GFX_K_FACTORS];
				mono_factors(cf[c], needs[i].lane == 3, f);
				int count = 0;
				for (unsigned j = 0; j < nneeds; j++)
					if (!needs[j].done && term_serves(f, &needs[j]) >= 0)
						count++;
				/* A igual cuenta, los factores menores (C antes que Ca):
				 * el mismo conjunto de monomios da siempre los mismos
				 * términos. */
				if (count > best_count || (count == best_count && memcmp(f, best, sizeof best) < 0)) {
					best_count = count;
					memcpy(best, f, sizeof best);
				}
			}
		}
		if (!best_count)
			break;
		if (nterms == GFX_K_TERMS) {
			x->approx = true;  /* no caben más términos: se pierden monomios */
			break;
		}
		memcpy(d->factors[nterms], best, GFX_K_FACTORS);
		for (unsigned j = 0; j < nneeds; j++) {
			int slot = needs[j].done ? -1 : term_serves(best, &needs[j]);
			if (slot < 0)
				continue;
			d->k[nterms][slot][needs[j].lane] += needs[j].c;
			needs[j].done = true;
		}
		nterms++;
	}

	/* Orden canónico de los términos (por sus factores), para que el mismo
	 * conjunto dé siempre el mismo programa. */
	for (unsigned a = 1; a < nterms; a++)
		for (unsigned b = a; b > 0 && memcmp(d->factors[b - 1], d->factors[b], GFX_K_FACTORS) > 0; b--) {
			uint8_t tf[GFX_K_FACTORS];
			float tk[3][4];
			memcpy(tf, d->factors[b], sizeof tf);
			memcpy(d->factors[b], d->factors[b - 1], sizeof tf);
			memcpy(d->factors[b - 1], tf, sizeof tf);
			memcpy(tk, d->k[b], sizeof tk);
			memcpy(d->k[b], d->k[b - 1], sizeof tk);
			memcpy(d->k[b - 1], tk, sizeof tk);
		}
	/* Con niebla, el vertex program suma fog·C.a al primer término sin
	 * factores: que exista. */
	if (d->fog && (nterms == 0 || d->factors[0][0] != GFX_F_NONE)) {
		if (nterms < GFX_K_TERMS) {
			memmove(d->factors[1], d->factors[0], (GFX_K_TERMS - 1) * GFX_K_FACTORS);
			memmove(d->k[1], d->k[0], (GFX_K_TERMS - 1) * sizeof d->k[0]);
			memset(d->factors[0], 0, GFX_K_FACTORS);
			memset(d->k[0], 0, sizeof d->k[0]);
			nterms++;
		} else {
			x->approx = true;  /* la niebla no se suma */
		}
	}
	bool t0 = false, t1 = false;
	for (unsigned j = 0; j < nterms; j++)
		for (int k = 0; k < GFX_K_FACTORS; k++) {
			t0 |= d->factors[j][k] == GFX_F_T0 || d->factors[j][k] == GFX_F_T0A;
			t1 |= d->factors[j][k] == GFX_F_T1 || d->factors[j][k] == GFX_F_T1A;
		}
	d->nterms = (uint8_t)nterms;
	d->ntex = t1 ? 2 : t0 ? 1 : 0;
	if (nterms == 0)
		d->nterms = 0;  /* todo era cero: la combinación lineal vacía */
}

bool gfx_combine(const struct gfx_state *g, struct gfx_draw *d)
{
	struct cc_ctx x;
	memset(&x, 0, sizeof x);
	rgba(g->prim, x.prim);
	rgba(g->env, x.env);
	x.prim_lod = g->prim_lod / 255.0f;
	x.noise = 0.5f;
	/* K4 y K5 tienen 9 bits con signo, en la escala de los colores (255 = 1). */
	x.k4 = (float)((int32_t)((uint32_t)g->k4 << 23) >> 23) / 255.0f;
	x.k5 = (float)((int32_t)((uint32_t)g->k5 << 23) >> 23) / 255.0f;

	struct cc_cycle cyc[2];
	unpack(g->cc_hi, g->cc_lo, cyc);

	/* COMBINED del primer ciclo (o del único) vale cero. Con dos ciclos,
	 * el primero deja su resultado en p1 y el segundo lo lee de ahí y
	 * escribe en p0: run_cycle no puede escribir donde lee. */
	poly p0[4] = { 0 }, p1[4];
	const poly *out = p1;
	unsigned cycle = (g->oh >> G_MDSFT_CYCLETYPE) & 3;
	if (cycle == G_CYC_2CYCLE) {
		run_cycle(&cyc[0], p1, p0, &x);
		/* Rareza del pipeline del RDP (n64brew.dev): en el segundo ciclo
		 * TEXEL0 da el texel de la tile siguiente y TEXEL1 el del píxel
		 * siguiente (≈ TEXEL0). */
		x.swap = true;
		run_cycle(&cyc[1], p0, p1, &x);
		out = p0;
	} else {
		run_cycle(&cyc[1], p1, p0, &x);
	}

	d->tex[0].flags &= (uint8_t)~GFX_TEX_PREMUL;
	d->tex[1].flags &= (uint8_t)~GFX_TEX_PREMUL;
	memset(d->factors, 0, sizeof d->factors);
	if (!fit_lincomb(out, d))
		build_terms(out, d, &x);
	return x.approx;
}

/* Con la textura apagada (G_TEXTURE en off) un combiner que lee TEXEL0 o
 * TEXEL1 los toma como 1: así lo dibuja el host (una textura que falta vale
 * 1) y así sale en el RSX, que si no muestrearía una textura sin ligar (la
 * tapa del muro de Luigi Raceway, TEXEL0·SHADE sin textura, salía negra). */
void gfx_combine_untextured(struct gfx_draw *d)
{
	if (d->nterms == 0) {
		for (int t = GFX_K_T0; t <= GFX_K_T1A; t++) {
			for (int i = 0; i < 3; i++)
				for (int l = 0; l < 4; l++)
					d->k[GFX_K_ONE][i][l] += d->k[t][i][l];
			memset(d->k[t], 0, sizeof d->k[t]);
		}
		return;
	}
	for (unsigned j = 0; j < d->nterms; j++) {
		uint8_t *f = d->factors[j];
		unsigned n = 0;
		for (int k = 0; k < GFX_K_FACTORS && f[k]; k++)
			if (f[k] != GFX_F_T0 && f[k] != GFX_F_T0A && f[k] != GFX_F_T1 && f[k] != GFX_F_T1A)
				f[n++] = f[k];
		for (; n < GFX_K_FACTORS; n++)
			f[n] = 0;
	}
}

void gfx_combine_textures(struct gfx_draw *d, unsigned ntex)
{
	if (d->nterms == 0) {
		for (int t = ntex == 0 ? GFX_K_T0 : GFX_K_T1; ntex < 2 && t <= GFX_K_T1A; t++)
			memset(d->k[t], 0, sizeof d->k[t]);
		return;
	}
	unsigned n = 0;
	for (unsigned j = 0; j < d->nterms; j++) {
		bool drop = false;
		for (int k = 0; k < GFX_K_FACTORS; k++) {
			uint8_t f = d->factors[j][k];
			drop |= (f == GFX_F_T0 || f == GFX_F_T0A) && ntex < 1;
			drop |= (f == GFX_F_T1 || f == GFX_F_T1A) && ntex < 2;
		}
		if (drop)
			continue;
		if (n != j) {
			memcpy(d->factors[n], d->factors[j], GFX_K_FACTORS);
			memcpy(d->k[n], d->k[j], sizeof d->k[n]);
		}
		n++;
	}
	for (unsigned j = n; j < d->nterms; j++) {
		memset(d->factors[j], 0, GFX_K_FACTORS);
		memset(d->k[j], 0, sizeof d->k[j]);
	}
	d->nterms = (uint8_t)n;
	if (n == 0)
		memset(d->k, 0, sizeof d->k);  /* combinación lineal vacía: cero */
}
