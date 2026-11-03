/* SPDX-License-Identifier: MIT */
/*
 * El alfa 1 de RSXKIT_DSTA_ONE en un destino con alfa (rsxkit.h,
 * rsxkit_draw.dst_alpha). La mezcla del RSX no da una constante: el canal
 * alfa sale de As·fs + Ad·fd y todos los factores multiplican (con
 * GCM_CONSTANT_ALPHA da As·Ac, no Ac). Por eso, si el alfa del fragmento no
 * es 1 en todo el dibujo, draw3d.c dibuja antes una pasada solo en el canal
 * alfa que lo lleva a 1 o más (el RSX lo satura al escribirlo) en los mismos
 * fragmentos que escribe el dibujo, y después el dibujo con
 * RSXKIT_DSTA_KEEP.
 *
 * No depende de PSL1GHT: las pruebas del host lo ejercitan.
 */
#include <string.h>

#include "rsxkit/rsxkit.h"

/* Términos que lee el programa del dibujo: los de la combinación lineal fija
 * (RSXKIT_K_LINCOMB) o los del generado. */
static unsigned draw_terms(const rsxkit_draw *d)
{
	return d->nterms ? (d->nterms > RSXKIT_K_TERMS ? RSXKIT_K_TERMS : d->nterms) : RSXKIT_K_LINCOMB;
}

/* ¿El término j no tiene factores (es constante por vértice)? X4 en la
 * combinación lineal; en un programa generado, el de la lista vacía. */
static bool term_constant(const rsxkit_draw *d, unsigned j)
{
	return d->nterms ? !d->factors[j][0] : j == RSXKIT_K_ONE;
}

bool rsxkit_dsta_alpha_is_one(const rsxkit_draw *d)
{
	/* El alfa es la suma de los términos, lineal en sus k[j][·][3] (la
	 * niebla no lo toca): 1 si solo lo aportan términos sin factores, sin el
	 * color del vértice, y suman 1. */
	float sum = 0.0f;
	for (unsigned j = 0; j < draw_terms(d); j++) {
		const float(*k)[4] = d->k[j];
		if (!term_constant(d, j) || k[1][3] != 0.0f || k[2][3] != 0.0f) {
			if (k[0][3] != 0.0f || k[1][3] != 0.0f || k[2][3] != 0.0f)
				return false;
			continue;
		}
		sum += k[0][3];
	}
	return sum == 1.0f;
}

bool rsxkit_dsta_alpha_pass(const rsxkit_draw *d, rsxkit_draw *b)
{
	if (d->program)
		return false;
	*b = *d;
	b->blend = RSXKIT_BLEND_OFF;
	b->dst_alpha = RSXKIT_DSTA_PASS;
	b->depth = (uint8_t)(d->depth & ~RSXKIT_DEPTH_WRITE);
	b->edges = 0;
	if (b->stencil) {
		/* La misma prueba, sin tocarlo. */
		b->stencil_write_mask = 0;
		b->stencil_op = 2;
		b->stencil_zfail = 0;
		b->stencil_sfail = 0;
	}
	if (d->alpha_ref) {
		/* Pasa α ≥ r/255: con α·255/r (un poco más, por el redondeo) y la
		 * prueba contra 1 pasan los mismos fragmentos, todos con 1 o más. */
		float s = 255.0f / d->alpha_ref * (1.0f + 1.0f / 1024.0f);
		for (unsigned j = 0; j < RSXKIT_K_TERMS; j++)
			for (unsigned i = 0; i < 3; i++)
				b->k[j][i][3] *= s;
		b->alpha_ref = 255;
		return true;
	}
	/* Sin prueba de alfa escribe todo lo que cubre: más 1 en un término sin
	 * factores (se agrega uno si falta y hay lugar). */
	int c = -1;
	for (unsigned j = 0; j < draw_terms(b) && c < 0; j++)
		if (term_constant(b, j))
			c = (int)j;
	if (c < 0) {
		if (b->nterms >= RSXKIT_K_TERMS)
			return false;
		c = b->nterms++;
		memset(b->factors[c], 0, sizeof b->factors[c]);
		memset(b->k[c], 0, sizeof b->k[c]);
	}
	b->k[c][0][3] += 1.0f;
	return true;
}
