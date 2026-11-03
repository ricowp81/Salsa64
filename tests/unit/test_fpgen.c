/*
 * Salsa64 — pruebas del generador de fragment programs de lib/rsxkit
 * (rsxkit/fpgen.h) con un intérprete del microcódigo del RSX.
 *
 * El intérprete decodifica el subconjunto que usan los programas de la
 * combinación lineal y los generados (TEX, MOV, MUL, ADD, MAD; registros
 * R y H; entradas COL0 y TEXCOORDn; constantes en línea; swizzle, negado,
 * máscara y saturación). Primero se ancla a la realidad: los programas que
 * cgcomp compiló de shaders equivalentes (tests/unit/fpgen_ref.h, generado
 * por tools/fpgen-ref.sh) deben dar, interpretados, la fórmula del shader.
 * Después, los programas del generador deben dar la misma fórmula, y para
 * las descripciones con shader de referencia, los mismos datos de carga
 * (TEXCOORD que leen, control).
 */
#include "test.h"

#include <math.h>
#include <string.h>

#include "fpgen_ref.h"
#include "rsxkit/fpgen.h"
#include "rsxkit/rsxkit.h"

/* ---- Intérprete ---- */

struct fp_sim {
	float r[16][4];
	float h[32][4];
	float in[16][4];   /* 1 COL0, 4.. TEXCOORD0..7 */
	float tex[2][4];   /* base del texel de cada unidad */
	int insns;         /* instrucciones ejecutadas */
	int bad;           /* algo que el intérprete no entiende */
};

#define IN_COL0 1
#define IN_TC(n) (4 + (n))

/* El texel depende de la coordenada para comprobar qué lee cada TEX. */
static void texel(const struct fp_sim *s, unsigned unit, float cx, float cy, float out[4])
{
	for (int c = 0; c < 4; c++)
		out[c] = s->tex[unit][c];
	out[0] += 0.25f * cx;
	out[1] += 0.25f * cy;
}

static void read_src(struct fp_sim *s, uint32_t w, unsigned input, const uint32_t *konst, float out[4])
{
	unsigned type = w & 3, idx = (w >> 2) & 0x3F, half = (w >> 8) & 1, swz = (w >> 9) & 0xFF, neg = (w >> 17) & 1;
	float v[4];
	switch (type) {
	case 0: memcpy(v, half ? s->h[idx] : s->r[idx], sizeof v); break;
	case 1: memcpy(v, s->in[input], sizeof v); break;
	case 2:
		for (int c = 0; c < 4; c++) {
			uint32_t bits = konst[c];
			memcpy(&v[c], &bits, 4);
		}
		break;
	default: s->bad = 1; memset(v, 0, sizeof v); break;
	}
	for (int c = 0; c < 4; c++)
		out[c] = v[(swz >> (2 * c)) & 3] * (neg ? -1.0f : 1.0f);
}

/* Corre el programa (palabras en el orden natural). Devuelve 1 si llegó al
 * fin; el color queda en s->r[0]. */
static int fp_run(struct fp_sim *s, const uint32_t *w, unsigned nwords)
{
	unsigned i = 0;
	s->insns = 0;
	while (i + 4 <= nwords) {
		uint32_t w0 = w[i];
		unsigned end = w0 & 1, dst = (w0 >> 1) & 0x3F, dhalf = (w0 >> 7) & 1, mask = (w0 >> 9) & 0xF;
		unsigned input = (w0 >> 13) & 0xF, unit = (w0 >> 17) & 0xF, opc = (w0 >> 24) & 0x3F, sat = w0 >> 31;
		const uint32_t *konst = w + i + 4;
		int has_const = 0;
		for (int k = 1; k < 4; k++)
			if ((w[i + k] & 3) == 2)
				has_const = 1;
		if (has_const && i + 8 > nwords) {
			s->bad = 1;
			return 0;
		}
		float a[4], b[4], c[4], res[4];
		read_src(s, w[i + 1], input, konst, a);
		read_src(s, w[i + 2], input, konst, b);
		read_src(s, w[i + 3], input, konst, c);
		switch (opc) {
		case 0x01: memcpy(res, a, sizeof res); break;
		case 0x02: for (int k = 0; k < 4; k++) res[k] = a[k] * b[k]; break;
		case 0x03: for (int k = 0; k < 4; k++) res[k] = a[k] + b[k]; break;
		case 0x04: for (int k = 0; k < 4; k++) res[k] = a[k] * b[k] + c[k]; break;
		case 0x17: texel(s, unit, a[0], a[1], res); break;
		default: s->bad = 1; return 0;
		}
		float *d = dhalf ? s->h[dst] : s->r[dst];
		for (int k = 0; k < 4; k++)
			if (mask & (1u << k))
				d[k] = sat ? fminf(fmaxf(res[k], 0.0f), 1.0f) : res[k];
		s->insns++;
		i += has_const ? 8 : 4;
		if (end)
			return 1;
	}
	return 0;
}

/* Entradas pseudoaleatorias en [−0,5, 1,5] (fuera de [0, 1] para que la
 * saturación cuente), texels en [0, 1]. */
static uint32_t rnd_state;
static float rnd(float lo, float hi)
{
	rnd_state = rnd_state * 1664525u + 1013904223u;
	return lo + (hi - lo) * (float)(rnd_state >> 8) / 16777216.0f;
}

static void fill_inputs(struct fp_sim *s, uint32_t seed)
{
	memset(s, 0, sizeof *s);
	rnd_state = seed;
	for (int c = 0; c < 4; c++) {
		s->in[IN_COL0][c] = rnd(0.0f, 1.0f);
		s->in[IN_TC(0)][c] = rnd(0.0f, 1.0f);
		for (int j = 1; j < 8; j++)
			s->in[IN_TC(j)][c] = rnd(-0.5f, 1.5f);
		s->tex[0][c] = rnd(0.0f, 0.5f);
		s->tex[1][c] = rnd(0.0f, 0.5f);
	}
}

/* Valor de cada factor en un canal (en el canal alfa cada textura y el
 * color del vértice aportan su alfa, que es justo lo que da el carril 3 de
 * los registros). */
static float factor_value(const struct fp_sim *s, uint8_t f, int lane)
{
	float t0[4], t1[4];
	texel(s, 0, s->in[IN_TC(0)][0], s->in[IN_TC(0)][1], t0);
	texel(s, 1, s->in[IN_TC(0)][2], s->in[IN_TC(0)][3], t1);
	switch (f) {
	case RSXKIT_FP_T0: return t0[lane];
	case RSXKIT_FP_T0A: return t0[3];
	case RSXKIT_FP_T1: return t1[lane];
	case RSXKIT_FP_T1A: return t1[3];
	case RSXKIT_FP_C: return s->in[IN_COL0][lane];
	case RSXKIT_FP_CA: return s->in[IN_COL0][3];
	default: return 1.0f;
	}
}

static void expected(const struct fp_sim *s, const rsxkit_fp_desc *d, float out[4])
{
	for (int lane = 0; lane < 4; lane++) {
		float sum = 0.0f;
		for (unsigned j = 0; j < d->nterms; j++) {
			float p = s->in[IN_TC(j + 1)][lane];
			for (int k = 0; k < RSXKIT_FP_MAX_FACTORS && d->factors[j][k]; k++)
				p *= factor_value(s, d->factors[j][k], lane);
			sum += p;
		}
		out[lane] = fminf(fmaxf(sum, 0.0f), 1.0f);
	}
}

static int close4(const float a[4], const float b[4])
{
	for (int k = 0; k < 4; k++)
		if (fabsf(a[k] - b[k]) > 1e-4f)
			return 0;
	return 1;
}

/* ---- Programas de cgcomp (.fpo) ---- */

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

struct ref {
	uint32_t words[64];
	unsigned nwords;
	unsigned num_regs;
	uint32_t fp_control;
	unsigned texcoords, texcoord2d, texcoord3d;
};

/* rsxFragmentProgram de PSL1GHT: num_regs en 4, num_insn en 10, ucode_off
 * en 20, fp_control en 24, texcoords en 28, texcoord2D en 30, texcoord3D en
 * 32. Las palabras del microcódigo van con las mitades intercambiadas. */
static void load_ref(const uint8_t *fpo, size_t size, struct ref *r)
{
	memset(r, 0, sizeof *r);
	unsigned ninsn = be16(fpo + 10), off = be32(fpo + 20);
	r->num_regs = be16(fpo + 4);
	r->fp_control = be32(fpo + 24);
	r->texcoords = be16(fpo + 28);
	r->texcoord2d = be16(fpo + 30);
	r->texcoord3d = be16(fpo + 32);
	CHECK(off + 16 * ninsn <= size);
	for (unsigned i = 0; i < 4 * ninsn && i < 64; i++)
		r->words[i] = rsxkit_fp_word_swap(be32(fpo + off + 4 * i));
	r->nwords = 4 * ninsn;
}

#define LOAD_REF(name, r) load_ref(fpgen_ref_##name, sizeof fpgen_ref_##name, r)

/* Descripciones equivalentes a los shaders de referencia. */
static const rsxkit_fp_desc desc_lincomb2 = {
	2, 5, 0, 0,
	{ { RSXKIT_FP_T0 }, { RSXKIT_FP_T0A }, { RSXKIT_FP_T1 }, { RSXKIT_FP_T1A }, { 0 } }
};
static const rsxkit_fp_desc desc_t0t1 = { 2, 2, 0, 0, { { RSXKIT_FP_T0, RSXKIT_FP_T1 }, { 0 } } };
static const rsxkit_fp_desc desc_shade = {
	1, 3, 0, 0, { { RSXKIT_FP_T0, RSXKIT_FP_C, RSXKIT_FP_CA }, { RSXKIT_FP_C }, { 0 } }
};
static const rsxkit_fp_desc desc_t0a = { 1, 2, 0, 0, { { RSXKIT_FP_T0, RSXKIT_FP_T0A }, { RSXKIT_FP_CA } } };

/* Los X_j del shader de referencia de la combinación lineal están en
 * TEXCOORD1, 2, 3, 4 y 5 como en la descripción: misma fórmula. */
static void fpgen_refs_lincomb(void)
{
	struct ref r;
	struct fp_sim s;
	float want[4];
	for (uint32_t seed = 1; seed <= 8; seed++) {
		/* lincomb2: todo. */
		LOAD_REF(rsxkit_lincomb2, &r);
		fill_inputs(&s, seed);
		expected(&s, &desc_lincomb2, want);
		CHECK(fp_run(&s, r.words, r.nwords));
		CHECK(!s.bad);
		CHECK(close4(s.r[0], want));
		CHECK_EQ(r.texcoords, 0x3F);
		CHECK_EQ(r.fp_control, 0x40);

		/* lincomb1: T0·X0 + T0a·X1 + X4 (sin los términos de T1). */
		LOAD_REF(rsxkit_lincomb1, &r);
		rsxkit_fp_desc d1 = { 1, 5, 0, 0, { { RSXKIT_FP_T0 }, { RSXKIT_FP_T0A }, { 0 }, { 0 }, { 0 } } };
		fill_inputs(&s, seed);
		s.in[IN_TC(3)][0] = s.in[IN_TC(3)][1] = s.in[IN_TC(3)][2] = s.in[IN_TC(3)][3] = 0.0f;
		s.in[IN_TC(4)][0] = s.in[IN_TC(4)][1] = s.in[IN_TC(4)][2] = s.in[IN_TC(4)][3] = 0.0f;
		expected(&s, &d1, want);
		CHECK(fp_run(&s, r.words, r.nwords));
		CHECK(close4(s.r[0], want));
		CHECK_EQ(r.texcoords, 0x27);

		/* lincomb0: X4. */
		LOAD_REF(rsxkit_lincomb0, &r);
		fill_inputs(&s, seed);
		for (int c = 0; c < 4; c++)
			want[c] = fminf(fmaxf(s.in[IN_TC(5)][c], 0.0f), 1.0f);
		CHECK(fp_run(&s, r.words, r.nwords));
		CHECK(close4(s.r[0], want));
		CHECK_EQ(r.texcoords, 0x20);
	}
}

static void check_ref_vs_gen(const uint8_t *fpo, size_t size, const rsxkit_fp_desc *d)
{
	struct ref r;
	load_ref(fpo, size, &r);
	uint32_t words[RSXKIT_FP_MAX_WORDS];
	rsxkit_fp_info info;
	int n = rsxkit_fpgen_emit(d, words, RSXKIT_FP_MAX_WORDS, &info);
	CHECK(n > 0);
	CHECK_EQ(info.texcoords, r.texcoords);
	CHECK_EQ(info.texcoord2d, r.texcoord2d);
	CHECK_EQ(info.texcoord3d, r.texcoord3d);
	CHECK_EQ(info.fp_control, r.fp_control);
	for (uint32_t seed = 11; seed <= 18; seed++) {
		struct fp_sim s;
		float want[4], got_ref[4];
		fill_inputs(&s, seed);
		expected(&s, d, want);
		CHECK(fp_run(&s, r.words, r.nwords));
		CHECK(!s.bad);
		CHECK(close4(s.r[0], want));
		memcpy(got_ref, s.r[0], sizeof got_ref);
		fill_inputs(&s, seed);
		CHECK(fp_run(&s, words, (unsigned)n));
		CHECK(!s.bad);
		CHECK(close4(s.r[0], want));
		CHECK(close4(s.r[0], got_ref));
	}
}

static void fpgen_refs_products(void)
{
	check_ref_vs_gen(fpgen_ref_ref_t0t1, sizeof fpgen_ref_ref_t0t1, &desc_t0t1);
	check_ref_vs_gen(fpgen_ref_ref_shade, sizeof fpgen_ref_ref_shade, &desc_shade);
	check_ref_vs_gen(fpgen_ref_ref_t0a, sizeof fpgen_ref_ref_t0a, &desc_t0a);
}

/* ---- Generador ---- */

/* Propiedades que debe tener todo programa emitido. */
static void check_shape(const uint32_t *w, int n, const rsxkit_fp_desc *d, const rsxkit_fp_info *info)
{
	CHECK(n > 0 && n % 4 == 0);
	CHECK_EQ(info->ninsn, (uint32_t)n / 4);
	CHECK(info->num_regs >= 2);
	unsigned tc = 0;
	for (int i = 0; i < n; i += 4) {
		int last = i + 4 == n;
		CHECK_EQ(w[i] & 1, (uint32_t)last);                   /* fin solo en la última */
		CHECK_EQ((w[i + 1] >> 18) & 7, 7);                     /* condición: siempre */
		CHECK_EQ((w[i] >> 9) & 0xF, 0xF);                      /* escribe xyzw */
		unsigned input = (w[i] >> 13) & 0xF;
		int inputs = 0;
		for (int k = 1; k < 4; k++) {
			CHECK((w[i + k] & 3) != 2);                     /* sin constantes en línea */
			if ((w[i + k] & 3) == 1)
				inputs = 1;
		}
		/* Con corrección de perspectiva, salvo los X_j de un programa afín. */
		CHECK_EQ(w[i + 3] >> 31, (uint32_t)(d->affine && inputs && input >= IN_TC(1)));
		if (inputs)
			CHECK(input == IN_COL0 || input >= IN_TC(0));
		if (inputs && input >= IN_TC(0)) {
			tc |= 1u << (input - IN_TC(0));
			CHECK_EQ((w[i + 3] >> 19) & 0x7FF, 0x7FC);
		}
		if (last) {
			CHECK_EQ((w[i] >> 1) & 0x3F, 0);                /* R0 */
			CHECK_EQ((w[i] >> 7) & 1, 0);
			CHECK_EQ(w[i] >> 31, 1);                        /* saturado */
		} else {
			CHECK_EQ(w[i] >> 31, 0);
		}
	}
	unsigned want_tc = d->ntex ? 1u : 0u;
	for (unsigned j = 0; j < d->nterms; j++)
		want_tc |= 1u << (j + 1);
	CHECK_EQ(tc, want_tc);
	CHECK_EQ(info->texcoords, want_tc);
	CHECK_EQ(info->texcoord3d, want_tc);
	CHECK_EQ(info->texcoord2d, 0);
}

static void check_desc(const rsxkit_fp_desc *d)
{
	uint32_t words[RSXKIT_FP_MAX_WORDS];
	rsxkit_fp_info info;
	int n = rsxkit_fpgen_emit(d, words, RSXKIT_FP_MAX_WORDS, &info);
	CHECK(n > 0);
	if (n <= 0)
		return;
	check_shape(words, n, d, &info);
	for (uint32_t seed = 21; seed <= 26; seed++) {
		struct fp_sim s;
		float want[4];
		fill_inputs(&s, seed);
		expected(&s, d, want);
		CHECK(fp_run(&s, words, (unsigned)n));
		CHECK(!s.bad);
		CHECK(close4(s.r[0], want));
	}
}

static void fpgen_emit_forms(void)
{
	/* Un término de cada clase, al principio, en medio y al final. */
	static const rsxkit_fp_desc descs[] = {
		{ 0, 1, 0, 0, { { 0 } } },                                    /* X0 */
		{ 1, 1, 0, 0, { { RSXKIT_FP_T0 } } },                         /* T0·X0 */
		{ 0, 1, 0, 0, { { RSXKIT_FP_C } } },                          /* C·X0: MOV + MUL */
		{ 0, 1, 0, 0, { { RSXKIT_FP_C, RSXKIT_FP_CA } } },            /* C·Ca·X0 */
		{ 0, 2, 0, 0, { { 0 }, { RSXKIT_FP_CA } } },
		{ 2, 3, 0, 0, { { RSXKIT_FP_T1A }, { 0 }, { RSXKIT_FP_T0A, RSXKIT_FP_T1 } } },
		{ 2, 4, 0, 0,
		  { { RSXKIT_FP_T0, RSXKIT_FP_T1, RSXKIT_FP_C, RSXKIT_FP_CA }, { RSXKIT_FP_T0A, RSXKIT_FP_T1A, RSXKIT_FP_C },
		    { RSXKIT_FP_CA, RSXKIT_FP_CA }, { RSXKIT_FP_T0, RSXKIT_FP_T0 } } },
		/* Siete términos (todos los varyings). */
		{ 2, 7, 0, 0,
		  { { RSXKIT_FP_T0 }, { RSXKIT_FP_T0A }, { RSXKIT_FP_T1 }, { RSXKIT_FP_T1A }, { 0 },
		    { RSXKIT_FP_T0, RSXKIT_FP_T1 }, { RSXKIT_FP_C, RSXKIT_FP_CA } } },
		/* GoldenEye: (T0 − E)·Sa·S + E·S → T0·C·Ca·X0 + C·X1 (Ca en X1.k2). */
		{ 1, 2, 0, 0, { { RSXKIT_FP_T0, RSXKIT_FP_C, RSXKIT_FP_CA }, { RSXKIT_FP_C } } },
	};
	for (unsigned i = 0; i < sizeof descs / sizeof descs[0]; i++)
		check_desc(&descs[i]);
	/* Los mismos, afines: el bit 31 en las lecturas de X_j y nada más (la
	 * cuenta del intérprete es la misma: el bit es cosa del interpolador). */
	for (unsigned i = 0; i < sizeof descs / sizeof descs[0]; i++) {
		rsxkit_fp_desc a = descs[i];
		a.affine = 1;
		check_desc(&a);
	}
	check_desc(&desc_lincomb2);
	check_desc(&desc_t0t1);
	check_desc(&desc_shade);
	check_desc(&desc_t0a);

	/* El programa más largo cabe en RSXKIT_FP_MAX_WORDS. */
	rsxkit_fp_desc big = { 2, 7, 0, 0, { { 0 } } };
	for (unsigned j = 0; j < 7; j++)
		for (int k = 0; k < 4; k++)
			big.factors[j][k] = (uint8_t)(RSXKIT_FP_T0 + (j + (unsigned)k) % 6);
	check_desc(&big);
	uint32_t words[RSXKIT_FP_MAX_WORDS];
	CHECK_EQ(rsxkit_fpgen_emit(&big, words, 8, NULL), (uint64_t)RSXKIT_FP_ESPACE);
}

static void fpgen_desc_rules(void)
{
	rsxkit_fp_desc d = { 1, 2, 0, 0, { { RSXKIT_FP_C, RSXKIT_FP_T0A, RSXKIT_FP_T0 }, { RSXKIT_FP_CA } } };
	CHECK_EQ(rsxkit_fp_desc_canon(&d), 0);
	CHECK_EQ(d.factors[0][0], RSXKIT_FP_T0);
	CHECK_EQ(d.factors[0][1], RSXKIT_FP_T0A);
	CHECK_EQ(d.factors[0][2], RSXKIT_FP_C);
	CHECK_EQ(d.factors[0][3], 0);
	uint32_t h = rsxkit_fp_desc_hash(&d);
	rsxkit_fp_desc d2 = { 1, 2, 0, 0, { { RSXKIT_FP_T0A, RSXKIT_FP_C, RSXKIT_FP_T0 }, { RSXKIT_FP_CA } } };
	CHECK_EQ(rsxkit_fp_desc_canon(&d2), 0);
	CHECK(!memcmp(&d, &d2, sizeof d));
	CHECK_EQ(rsxkit_fp_desc_hash(&d2), h);
	d2.factors[1][0] = RSXKIT_FP_C;
	CHECK(rsxkit_fp_desc_hash(&d2) != h);
	/* La afín es otro programa: otro hash; affine > 1 o el relleno no valen. */
	rsxkit_fp_desc da = d;
	da.affine = 1;
	CHECK_EQ(rsxkit_fp_desc_canon(&da), 0);
	CHECK(rsxkit_fp_desc_hash(&da) != h);
	da.affine = 2;
	CHECK_EQ(rsxkit_fp_desc_canon(&da), (uint64_t)RSXKIT_FP_EDESC);
	da.affine = 0;
	da.pad = 1;
	CHECK_EQ(rsxkit_fp_desc_canon(&da), (uint64_t)RSXKIT_FP_EDESC);

	uint32_t words[RSXKIT_FP_MAX_WORDS];
	/* Sin términos. */
	rsxkit_fp_desc bad = { 1, 0, 0, 0, { { 0 } } };
	CHECK_EQ(rsxkit_fpgen_emit(&bad, words, RSXKIT_FP_MAX_WORDS, NULL), (uint64_t)RSXKIT_FP_EDESC);
	/* Textura 1 sin ntex = 2. */
	rsxkit_fp_desc bad2 = { 1, 1, 0, 0, { { RSXKIT_FP_T1 } } };
	CHECK_EQ(rsxkit_fpgen_emit(&bad2, words, RSXKIT_FP_MAX_WORDS, NULL), (uint64_t)RSXKIT_FP_EDESC);
	/* Factor después del fin de la lista. */
	rsxkit_fp_desc bad3 = { 1, 1, 0, 0, { { RSXKIT_FP_T0, 0, RSXKIT_FP_C } } };
	CHECK_EQ(rsxkit_fp_desc_canon(&bad3), (uint64_t)RSXKIT_FP_EDESC);
	/* Factor desconocido y término más allá de nterms. */
	rsxkit_fp_desc bad4 = { 1, 1, 0, 0, { { RSXKIT_FP_FACTORS } } };
	CHECK_EQ(rsxkit_fpgen_emit(&bad4, words, RSXKIT_FP_MAX_WORDS, NULL), (uint64_t)RSXKIT_FP_EDESC);
	rsxkit_fp_desc bad5 = { 1, 1, 0, 0, { { RSXKIT_FP_T0 }, { RSXKIT_FP_T0 } } };
	CHECK_EQ(rsxkit_fpgen_emit(&bad5, words, RSXKIT_FP_MAX_WORDS, NULL), (uint64_t)RSXKIT_FP_EDESC);
	rsxkit_fp_desc bad6 = { 3, 1, 0, 0, { { 0 } } };
	CHECK_EQ(rsxkit_fpgen_emit(&bad6, words, RSXKIT_FP_MAX_WORDS, NULL), (uint64_t)RSXKIT_FP_EDESC);
}

/* rsxkit_target_read: el más cercano solo cuando la región y la salida
 * miden lo mismo (copia 1:1, las capas de 8 bits); al reducir, lineal; y
 * con exact = 0, lineal siempre. */
static void rsxkit_read_filter(void)
{
	CHECK(rsxkit_read_filter_nearest(336, 256, 336, 256, 1));
	CHECK(rsxkit_read_filter_nearest(336, 16, 336, 16, 1));
	CHECK(!rsxkit_read_filter_nearest(960, 720, 320, 240, 1));
	CHECK(!rsxkit_read_filter_nearest(336, 256, 336, 128, 1));
	CHECK(!rsxkit_read_filter_nearest(336, 256, 336, 256, 0));
}

/* El alfa del fragmento de un dibujo de rsxkit (rsxkit.h, rsxkit_draw.k):
 * Σ_j (Π factores) · X_j.a con X_j.a = k[j][0][3] + (k[j][1][3] + k[j][2][3])·Ca,
 * con el alfa de las texturas t0, t1 y el del vértice ca. */
static float draw_alpha(const rsxkit_draw *d, float t0, float t1, float ca)
{
	float a = 0.0f;
	unsigned n = d->nterms ? d->nterms : RSXKIT_K_LINCOMB;
	for (unsigned j = 0; j < n; j++) {
		float x = d->k[j][0][3] + (d->k[j][1][3] + d->k[j][2][3]) * ca, f = 1.0f;
		if (d->nterms) {
			for (unsigned i = 0; i < RSXKIT_K_FACTORS && d->factors[j][i]; i++) {
				uint8_t q = d->factors[j][i];
				f *= q == RSXKIT_FP_T0 || q == RSXKIT_FP_T0A ? t0 : q == RSXKIT_FP_T1 || q == RSXKIT_FP_T1A ? t1 : ca;
			}
		} else {
			f = j == RSXKIT_K_T0 || j == RSXKIT_K_T0A ? t0 : j == RSXKIT_K_T1 || j == RSXKIT_K_T1A ? t1 : 1.0f;
		}
		a += f * x;
	}
	return a;
}

/* La pasada del alfa de RSXKIT_DSTA_ONE (lib/rsxkit/src/dsta.c): la mezcla
 * del RSX no da una constante (el primer intento, con GCM_CONSTANT_ALPHA, dejó
 * el alfa del fragmento en la PS3), así que una pasada solo en el alfa lo
 * lleva a 1 o más en los mismos fragmentos que escribe el dibujo: con
 * prueba de alfa, los que pasan α ≥ r (y ninguno más); sin ella, todos. */
static void rsxkit_dsta_pass(void)
{
	rsxkit_draw d, b;
	/* Ya 1 (solo el término constante): una pasada. */
	memset(&d, 0, sizeof d);
	d.k[RSXKIT_K_ONE][0][3] = 1.0f;
	CHECK(rsxkit_dsta_alpha_is_one(&d));
	d.k[RSXKIT_K_T0A][0][3] = 0.5f;
	CHECK(!rsxkit_dsta_alpha_is_one(&d));
	d.k[RSXKIT_K_T0A][0][3] = 0.0f;
	d.k[RSXKIT_K_ONE][1][3] = 0.5f;  /* con el alfa del vértice */
	CHECK(!rsxkit_dsta_alpha_is_one(&d));

	/* Combinación lineal, α = 0,75·T0a, sin prueba: todo con 1 o más. */
	memset(&d, 0, sizeof d);
	d.k[RSXKIT_K_T0A][0][3] = 0.75f;
	d.blend = RSXKIT_BLEND_ALPHA;
	d.depth = RSXKIT_DEPTH_TEST | RSXKIT_DEPTH_WRITE;
	d.edges = 8;
	CHECK(rsxkit_dsta_alpha_pass(&d, &b));
	CHECK_EQ(b.blend, RSXKIT_BLEND_OFF);
	CHECK_EQ(b.dst_alpha, RSXKIT_DSTA_PASS);
	CHECK_EQ(b.depth, RSXKIT_DEPTH_TEST);
	CHECK_EQ(b.edges, 0);
	CHECK_EQ(b.alpha_ref, 0);
	for (int t = 0; t <= 255; t++)
		CHECK(draw_alpha(&b, t / 255.0f, 0.0f, 0.0f) >= 1.0f);

	/* Con prueba de alfa: la pasada pasa la suya (≥ 1) justo donde el dibujo
	 * pasa la de él, con los alfas de 8 bits y en el umbral. */
	static const uint8_t refs[] = { 1, 32, 96, 128, 200, 255 };
	for (unsigned r = 0; r < sizeof refs; r++) {
		memset(&d, 0, sizeof d);
		d.k[RSXKIT_K_T0A][0][3] = 1.0f;
		d.alpha_ref = refs[r];
		CHECK(rsxkit_dsta_alpha_pass(&d, &b));
		CHECK_EQ(b.alpha_ref, 255);
		for (int t = 0; t <= 255; t++) {
			bool a_pass = t >= refs[r];
			bool b_pass = draw_alpha(&b, t / 255.0f, 0.0f, 0.0f) >= 1.0f;
			if (a_pass != b_pass)
				t_fail(__FILE__, __LINE__, "ref %u, alfa %d: el dibujo %s y la pasada %s", refs[r], t,
				       a_pass ? "pasa" : "no", b_pass ? "pasa" : "no");
			t_checks++;
		}
	}

	/* Programa generado sin término constante: se agrega uno. */
	memset(&d, 0, sizeof d);
	d.nterms = 2;
	d.factors[0][0] = RSXKIT_FP_T0A;
	d.factors[0][1] = RSXKIT_FP_CA;
	d.factors[1][0] = RSXKIT_FP_T1A;
	d.k[0][0][3] = 1.0f;
	d.k[1][0][3] = 0.5f;
	CHECK(rsxkit_dsta_alpha_pass(&d, &b));
	CHECK_EQ(b.nterms, 3);
	CHECK_EQ(b.factors[2][0], 0);
	for (int t = 0; t <= 255; t += 15)
		CHECK(draw_alpha(&b, t / 255.0f, 1.0f - t / 255.0f, 0.5f) >= 1.0f);
	/* Sin lugar para él, o con un programa propio, no se puede. */
	d.nterms = RSXKIT_K_TERMS;
	for (unsigned j = 0; j < RSXKIT_K_TERMS; j++)
		d.factors[j][0] = RSXKIT_FP_C;
	CHECK(!rsxkit_dsta_alpha_pass(&d, &b));
	d.alpha_ref = 64;
	CHECK(rsxkit_dsta_alpha_pass(&d, &b));  /* con prueba basta escalar */
	d.program = (rsxkit_program *)&d;
	CHECK(!rsxkit_dsta_alpha_pass(&d, &b));
}

void t_suite_fpgen(void)
{
	RUN(rsxkit_dsta_pass);
	RUN(rsxkit_read_filter);
	RUN(fpgen_refs_lincomb);
	RUN(fpgen_refs_products);
	RUN(fpgen_emit_forms);
	RUN(fpgen_desc_rules);
}
