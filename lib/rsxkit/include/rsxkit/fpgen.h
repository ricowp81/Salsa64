/* SPDX-License-Identifier: MIT */
/*
 * rsxkit — generador de fragment programs del RSX en tiempo de ejecución.
 *
 * No hay compilador Cg en la consola: este módulo emite el microcódigo de
 * fragment program (familia NV40) a partir de una descripción genérica. El
 * programa que sale calcula, por píxel,
 *
 *     color = saturate( Σ_j  (Π factores_j) · X_j )
 *
 * donde cada término j multiplica hasta RSXKIT_FP_MAX_FACTORS factores
 * (texturas 0 y 1, sus alfas, el color del vértice y su alfa) por el
 * varying X_j, un float4 que el vertex program pone en TEXCOORD(j + 1) y
 * que para rsxkit vale k[j][0] + k[j][1]·C + k[j][2]·C.aaaa (rsxkit.h,
 * «Dibujo 3D»). Un término sin factores suma X_j tal cual. Los cuatro
 * canales van en paralelo: en el canal alfa cada textura aporta su alfa y
 * el color del vértice su alfa, así que un combiner con alfa distinto del
 * color se expresa poniendo coeficientes distintos en el cuarto carril de
 * cada X_j.
 *
 * Con eso cabe cualquier polinomio en las entradas: productos de dos
 * texturas, textura por su alfa, sombreado por sombreado (niebla por luz
 * precalculada de los juegos de Rare), mezclas por alfa, etc. Lo que la
 * combinación lineal fija de rsxkit ya representa (un factor por término)
 * se queda con sus programas precompilados; el generador cubre el resto.
 *
 * Este archivo no depende de PSL1GHT: el codificador (fpgen.c) compila en
 * cualquier host y se prueba con un intérprete del microcódigo en las
 * pruebas unitarias. La subida a la memoria del RSX y la caché de programas
 * están en fpcache.c.
 *
 * Sombreado lineal en pantalla (`affine`): el RSX interpola los varyings
 * con corrección de perspectiva, pero una instrucción puede leer su
 * entrada sin ella (bit 31 de la palabra 3: la entrada llega dividida entre
 * la w del vértice e interpolada lineal en pantalla, docs/rsx-notes.md).
 * Si el vertex program multiplica cada X_j por w, el programa generado con
 * `affine` recibe X_j interpolado lineal en pantalla, como interpola el
 * sombreado el RDP de la N64; las coordenadas de textura (TEXCOORD0) y el
 * color del vértice siguen con perspectiva. No cuesta instrucciones.
 *
 * Formato del microcódigo (docs/rsx-notes.md): 4 palabras de 32 bits por
 * instrucción; aquí se entregan en el orden natural (bit 0 de la palabra 0
 * = fin de programa); para el RSX cada palabra se guarda con sus mitades
 * de 16 bits intercambiadas (rsxkit_fp_words_to_rsx).
 */
#ifndef RSXKIT_FPGEN_H
#define RSXKIT_FPGEN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Factores de un término. */
enum rsxkit_fp_factor {
	RSXKIT_FP_NONE = 0,  /* fin de la lista de factores del término */
	RSXKIT_FP_T0,        /* textura 0 (RGBA) */
	RSXKIT_FP_T0A,       /* alfa de la textura 0, replicado en los cuatro canales */
	RSXKIT_FP_T1,        /* textura 1 */
	RSXKIT_FP_T1A,       /* alfa de la textura 1, replicado */
	RSXKIT_FP_C,         /* color del vértice (RGBA) */
	RSXKIT_FP_CA,        /* alfa del vértice, replicado */
	RSXKIT_FP_FACTORS
};

#define RSXKIT_FP_MAX_TERMS   7  /* un varying por término: TEXCOORD1..7 */
#define RSXKIT_FP_MAX_FACTORS 4

/* Descripción de un programa. Los términos van en orden (el término j lee
 * X_j) y los factores de cada término terminan en RSXKIT_FP_NONE (o llenan
 * los cuatro lugares). Los bytes que no se usan deben ser 0: la caché
 * compara la estructura completa. */
typedef struct rsxkit_fp_desc {
	uint8_t ntex;    /* texturas que muestrea: 0, 1 o 2 */
	uint8_t nterms;  /* 1..RSXKIT_FP_MAX_TERMS */
	uint8_t affine;  /* 1: los X_j se leen sin corrección de perspectiva (arriba) */
	uint8_t pad;
	uint8_t factors[RSXKIT_FP_MAX_TERMS][RSXKIT_FP_MAX_FACTORS];
} rsxkit_fp_desc;

/* Lo que el RSX necesita saber del programa además del microcódigo (son los
 * campos de rsxFragmentProgram de PSL1GHT que lee
 * rsxLoadFragmentProgramLocation). */
typedef struct rsxkit_fp_info {
	uint32_t ninsn;        /* instrucciones (16 bytes cada una) */
	uint32_t num_regs;     /* registros R usados (≥ 2) */
	uint32_t fp_control;   /* bits de control: 0x40 = escribe el color */
	uint16_t texcoords;    /* máscara de TEXCOORDn que lee */
	uint16_t texcoord2d;   /* de ellos, los de 2 componentes */
	uint16_t texcoord3d;   /* y los de 3 o 4 */
	uint16_t pad;
} rsxkit_fp_info;

/* Instrucciones como máximo que emite un programa: 2 TEX + por término
 * hasta 3 MUL/MOV + 1 MAD. */
#define RSXKIT_FP_MAX_INSNS (2 + RSXKIT_FP_MAX_TERMS * (RSXKIT_FP_MAX_FACTORS))
#define RSXKIT_FP_MAX_WORDS (4 * RSXKIT_FP_MAX_INSNS)

/* Emite el programa de `d` en `words` (hasta `max_words` palabras, en el
 * orden natural). Devuelve el número de palabras escritas, o un negativo:
 * RSXKIT_FP_EDESC si la descripción no es válida (sin términos, factor
 * desconocido, textura 1 sin ntex = 2…) o RSXKIT_FP_ESPACE si no cabe. */
#define RSXKIT_FP_EDESC  (-1)
#define RSXKIT_FP_ESPACE (-2)
int rsxkit_fpgen_emit(const rsxkit_fp_desc *d, uint32_t *words, uint32_t max_words, rsxkit_fp_info *info);

/* Deja la descripción en forma canónica (factores de cada término
 * ordenados) y devuelve 0, o RSXKIT_FP_EDESC si no es válida. No reordena
 * los términos: su orden es el de los X_j. */
int rsxkit_fp_desc_canon(rsxkit_fp_desc *d);

/* Hash de una descripción canónica (para la caché). */
uint32_t rsxkit_fp_desc_hash(const rsxkit_fp_desc *d);

/* Palabra en el orden natural → palabra como la guarda el RSX en memoria
 * (mitades de 16 bits intercambiadas), y viceversa. */
static inline uint32_t rsxkit_fp_word_swap(uint32_t w)
{
	return (w << 16) | (w >> 16);
}

#ifdef __cplusplus
}
#endif

#endif
