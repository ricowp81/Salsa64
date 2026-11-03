/* SPDX-License-Identifier: MIT */
/*
 * rsxkit — codificador de fragment programs del RSX (ver rsxkit/fpgen.h).
 *
 * Formato de instrucción (familia NV40; campos tomados de nvfx_shader.h de
 * cgcomp, PSL1GHT, MIT, y cotejados con los programas que cgcomp compila
 * de shaders/rsxkit_lincomb*.fcg; docs/rsx-notes.md): cuatro palabras.
 *
 *   0  bit 0 fin del programa; 1–6 registro destino; 7 destino de media
 *      precisión (H); 9–12 máscara xyzw; 13–16 registro de entrada que lee
 *      la instrucción (COL0 = 1, TEXCOORDn = 4 + n; solo cabe uno por
 *      instrucción); 17–20 unidad de textura; 22–23 precisión (0 fp32,
 *      1 fp16); 24–29 opcode; 31 saturar.
 *   1–3  fuentes 0–2: bits 0–1 tipo (0 temporal, 1 entrada, 2 constante en
 *      los 16 bytes que siguen); 2–7 índice del temporal; 8 temporal H;
 *      9–16 swizzle (2 bits por componente); 17 negar. En la palabra 1,
 *      además, bits 18–20 condición (7 = siempre) y 21–28 su swizzle. En la
 *      palabra 3, bit 31 = sin corrección de perspectiva (la entrada llega
 *      dividida entre w e interpolada lineal en pantalla: solo con `affine`,
 *      en las lecturas de X_j) y 0x7FC en los bits 19–29 cuando se lee un
 *      TEXCOORD, como cgcomp.
 *
 * Registros: R0 es el color de salida; H2 acumula, H3 y H4 reciben las
 * texturas, H5 arma los productos. Las operaciones van en fp16 como las de
 * cgcomp (las entradas son de 8 bits); la última escribe R0 saturado.
 */
#include <string.h>

#include "rsxkit/fpgen.h"

#define OP_END       0x1u
#define OP_DST(i)    ((uint32_t)(i) << 1)
#define OP_DST_HALF  (1u << 7)
#define OP_MASK_XYZW (0xFu << 9)
#define OP_INPUT(i)  ((uint32_t)(i) << 13)
#define OP_UNIT(u)   ((uint32_t)(u) << 17)
#define OP_PREC_FP16 (1u << 22)
#define OP_OPCODE(o) ((uint32_t)(o) << 24)
#define OP_SAT       (1u << 31)

#define OPC_MOV 0x01u
#define OPC_MUL 0x02u
#define OPC_ADD 0x03u
#define OPC_MAD 0x04u
#define OPC_TEX 0x17u

#define SRC_TEMP     0x0u
#define SRC_INPUT    0x1u
#define SRC_INDEX(i) ((uint32_t)(i) << 2)
#define SRC_HALF     (1u << 8)
#define SRC_SWZ(s)   ((uint32_t)(s) << 9)
#define SWZ_XYZW     0xE4u  /* 0, 1, 2, 3 */
#define SWZ_WWWW     0xFFu
#define SWZ_ZWZW     0xEEu
/* Palabra 1: condición siempre verdadera con swizzle identidad. */
#define SRC0_COND    (7u << 18 | SWZ_XYZW << 21)
/* Palabra 3 cuando se lee un TEXCOORD. */
#define SRC2_TEXCOORD (0x7FCu << 19)
/* Palabra 3: la entrada sin corrección de perspectiva (rsxkit_fp_desc.affine). */
#define SRC2_NO_PERSP (1u << 31)

#define IN_COL0  1u
#define IN_TC(n) (4u + (n))

#define REG_OUT  0  /* R0 */
#define H_ACC    2
#define H_TEX0   3
#define H_TEX1   4
#define H_PROD   5

struct opnd {
	uint8_t input;  /* 0: temporal H `index`; si no, registro de entrada */
	uint8_t index;
	uint8_t swz;
};

struct emit {
	uint32_t *w;
	uint32_t n, max;
	int err;
	uint32_t texcoords;
	int max_h;
	int affine;  /* las lecturas de X_j (TEXCOORD1..7) sin corrección de perspectiva */
};

static const struct opnd none = { 0, 0, SWZ_XYZW };

static uint32_t src_word(const struct opnd *s)
{
	if (s->input)
		return SRC_INPUT | SRC_SWZ(s->swz);
	return SRC_TEMP | SRC_INDEX(s->index) | SRC_HALF | SRC_SWZ(s->swz);
}

/* dst < 0: R0 (salida, precisión completa); si no, el temporal H dst. */
static void insn(struct emit *e, uint32_t opc, int dst, int sat, int end, const struct opnd *s0,
		 const struct opnd *s1, const struct opnd *s2, uint32_t unit)
{
	if (e->err)
		return;
	if (e->n + 4 > e->max) {
		e->err = RSXKIT_FP_ESPACE;
		return;
	}
	uint32_t input = 0;
	const struct opnd *s[3] = { s0, s1, s2 };
	for (int i = 0; i < 3; i++) {
		if (!s[i]->input)
			continue;
		if (input && input != s[i]->input) {
			e->err = RSXKIT_FP_EDESC;  /* dos registros de entrada distintos */
			return;
		}
		input = s[i]->input;
	}
	uint32_t w0 = OP_OPCODE(opc) | OP_MASK_XYZW | OP_INPUT(input) | OP_UNIT(unit) | (end ? OP_END : 0) |
		      (sat ? OP_SAT : 0);
	if (opc != OPC_TEX)
		w0 |= OP_PREC_FP16;
	if (dst < 0) {
		w0 |= OP_DST(REG_OUT);
	} else {
		w0 |= OP_DST(dst) | OP_DST_HALF;
		if (dst > e->max_h)
			e->max_h = dst;
	}
	uint32_t w3 = src_word(s2);
	if (input >= IN_TC(0)) {
		w3 |= SRC2_TEXCOORD;
		e->texcoords |= 1u << (input - IN_TC(0));
		if (input >= IN_TC(1) && e->affine)
			w3 |= SRC2_NO_PERSP;
	}
	e->w[e->n++] = w0;
	e->w[e->n++] = src_word(s0) | SRC0_COND;
	e->w[e->n++] = src_word(s1);
	e->w[e->n++] = w3;
}

static int factor_opnd(uint8_t f, uint8_t ntex, struct opnd *o)
{
	switch (f) {
	case RSXKIT_FP_T0:
	case RSXKIT_FP_T0A:
		if (ntex < 1)
			return RSXKIT_FP_EDESC;
		o->input = 0;
		o->index = H_TEX0;
		o->swz = f == RSXKIT_FP_T0 ? SWZ_XYZW : SWZ_WWWW;
		return 0;
	case RSXKIT_FP_T1:
	case RSXKIT_FP_T1A:
		if (ntex < 2)
			return RSXKIT_FP_EDESC;
		o->input = 0;
		o->index = H_TEX1;
		o->swz = f == RSXKIT_FP_T1 ? SWZ_XYZW : SWZ_WWWW;
		return 0;
	case RSXKIT_FP_C:
	case RSXKIT_FP_CA:
		o->input = IN_COL0;
		o->index = 0;
		o->swz = f == RSXKIT_FP_C ? SWZ_XYZW : SWZ_WWWW;
		return 0;
	default:
		return RSXKIT_FP_EDESC;
	}
}

static int term_factors(const rsxkit_fp_desc *d, unsigned j)
{
	int n = 0;
	while (n < RSXKIT_FP_MAX_FACTORS && d->factors[j][n])
		n++;
	for (int k = n; k < RSXKIT_FP_MAX_FACTORS; k++)
		if (d->factors[j][k])
			return -1;  /* un factor después del fin */
	return n;
}

static int desc_valid(const rsxkit_fp_desc *d)
{
	if (d->ntex > 2 || d->nterms < 1 || d->nterms > RSXKIT_FP_MAX_TERMS || d->affine > 1 || d->pad)
		return 0;
	for (unsigned j = 0; j < RSXKIT_FP_MAX_TERMS; j++) {
		int n = term_factors(d, j);
		if (n < 0 || (j >= d->nterms && n != 0))
			return 0;
		for (int k = 0; k < n; k++) {
			struct opnd o;
			if (factor_opnd(d->factors[j][k], d->ntex, &o) != 0)
				return 0;
		}
	}
	return 1;
}

int rsxkit_fp_desc_canon(rsxkit_fp_desc *d)
{
	if (!desc_valid(d))
		return RSXKIT_FP_EDESC;
	for (unsigned j = 0; j < d->nterms; j++) {
		uint8_t *f = d->factors[j];
		int n = term_factors(d, j);
		for (int a = 1; a < n; a++)  /* inserción: son hasta 4 */
			for (int b = a; b > 0 && f[b - 1] > f[b]; b--) {
				uint8_t t = f[b];
				f[b] = f[b - 1];
				f[b - 1] = t;
			}
	}
	return 0;
}

uint32_t rsxkit_fp_desc_hash(const rsxkit_fp_desc *d)
{
	const uint8_t *p = (const uint8_t *)d;
	uint32_t h = 2166136261u;  /* FNV-1a */
	for (size_t i = 0; i < sizeof *d; i++) {
		h ^= p[i];
		h *= 16777619u;
	}
	return h;
}

int rsxkit_fpgen_emit(const rsxkit_fp_desc *d, uint32_t *words, uint32_t max_words, rsxkit_fp_info *info)
{
	if (!desc_valid(d))
		return RSXKIT_FP_EDESC;
	struct emit e = { words, 0, max_words, 0, 0, 0, d->affine };
	struct opnd uv = { IN_TC(0), 0, SWZ_XYZW }, uv2 = { IN_TC(0), 0, SWZ_ZWZW };
	if (d->ntex >= 1)
		insn(&e, OPC_TEX, H_TEX0, 0, 0, &uv, &none, &none, 0);
	if (d->ntex >= 2)
		insn(&e, OPC_TEX, H_TEX1, 0, 0, &uv2, &none, &none, 1);
	/* H3 y H4 se cuentan aunque solo los escriba TEX. */
	if (d->ntex >= 1 && e.max_h < H_TEX0)
		e.max_h = H_TEX0;
	if (d->ntex >= 2 && e.max_h < H_TEX1)
		e.max_h = H_TEX1;

	struct opnd acc = { 0, H_ACC, SWZ_XYZW }, prod = { 0, H_PROD, SWZ_XYZW };
	for (unsigned j = 0; j < d->nterms; j++) {
		int n = term_factors(d, j);
		int first = j == 0, last = j + 1 == d->nterms;
		int dst = last ? -1 : H_ACC;
		struct opnd x = { IN_TC(j + 1), 0, SWZ_XYZW };
		struct opnd f[RSXKIT_FP_MAX_FACTORS];
		memset(f, 0, sizeof f);
		for (int k = 0; k < n; k++)
			factor_opnd(d->factors[j][k], d->ntex, &f[k]);
		if (n == 0) {
			if (first)
				insn(&e, OPC_MOV, dst, last, last, &x, &none, &none, 0);
			else
				insn(&e, OPC_ADD, dst, last, last, &acc, &x, &none, 0);
			continue;
		}
		const struct opnd *p = &f[0];
		if (n > 1 || f[0].input) {
			/* Producto en H5: X_j es otra entrada y no cabe en la misma
			 * instrucción que el color del vértice. */
			if (n == 1)
				insn(&e, OPC_MOV, H_PROD, 0, 0, &f[0], &none, &none, 0);
			else
				insn(&e, OPC_MUL, H_PROD, 0, 0, &f[0], &f[1], &none, 0);
			for (int k = 2; k < n; k++)
				insn(&e, OPC_MUL, H_PROD, 0, 0, &prod, &f[k], &none, 0);
			p = &prod;
		}
		if (first)
			insn(&e, OPC_MUL, dst, last, last, p, &x, &none, 0);
		else
			insn(&e, OPC_MAD, dst, last, last, p, &x, &acc, 0);
	}
	if (e.err)
		return e.err;
	if (info) {
		memset(info, 0, sizeof *info);
		info->ninsn = e.n / 4;
		/* R0 y los R que ocupan los H usados (H2k y H2k+1 son R_k). */
		info->num_regs = (uint32_t)(e.max_h / 2) + 1;
		if (info->num_regs < 2)
			info->num_regs = 2;
		info->fp_control = 0x40;  /* escribe R0 como color */
		info->texcoords = (uint16_t)e.texcoords;
		info->texcoord3d = (uint16_t)e.texcoords;
	}
	return (int)e.n;
}
