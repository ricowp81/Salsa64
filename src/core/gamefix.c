/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — arreglos por juego en el código cargado en la RDRAM. Ver
 * gamefix.h.
 *
 * Donkey Kong 64 (USA, NDOE): el juego mide cuántos retrazados del VI pasaron
 * desde el cuadro anterior (contador del planificador en 0x80767CC4, el valor
 * anterior en 0x8076AF10) y lo guarda en 0x80744478; después divide entre ese
 * número en coma flotante con las excepciones de operación inválida y de
 * división entre cero habilitadas en el FCSR. En los mapas de juego el
 * cálculo pone un mínimo de 1; en los de escena cinemática (la bandera de
 * 0x8076AF14 en 0) guarda la resta tal cual. Al cambiar de escena a la mitad
 * de la introducción de una partida nueva (mapa 0xAC), el juego reinicia el
 * contador, y con nuestro CPU los dos primeros cuadros de la escena caben en
 * el mismo retrazado: el segundo mide 0, divide 0 entre 0, el hilo principal
 * se para por la excepción y quedan solo el audio y el VI (pantalla negra con
 * música, issue #51). En la consola, mucho más lenta por instrucción, entre
 * esos dos cuadros siempre pasa al menos un retrazado.
 *
 * El arreglo reescribe las 11 instrucciones del camino de las escenas
 * (0x80600914–0x8060093C, la función que llama el bucle principal antes de
 * esperar el cuadro) para que haga lo mismo que el camino de juego: la resta
 * con mínimo 1. Las direcciones salen del volcado de la RDRAM de nuestra
 * propia emulación; nada de material de Nintendo.
 *
 * Donkey Kong 64 (USA), la música del DK Rap (issue #51): el gestor de música
 * (0x80602600, una vez por cuadro) lleva por cada una de sus 4 casillas la
 * canción pedida y un estado por petición (0x80745924: 0 sin tocar, 1 cargada
 * y con PLAY enviado, 2 sonando). Pasa a 2 cuando ve el reproductor de la
 * casilla en AL_PLAYING; si lo ve parado y el estado no es 2, vuelve a llamar
 * a la función que para el reproductor, carga la canción de la ROM y manda
 * PLAY (0x8060a6a0). El hilo de audio procesa el PLAY en su siguiente cuadro
 * de audio, cada 2 campos. En la consola el gestor vuelve a mirar cuando el
 * PLAY ya se procesó (en ares: una sola carga y estado 2 al cuadro siguiente).
 * Con nuestro CPU, más rápido por instrucción, al cambiar a la escena del rap
 * cabe un cuadro de un solo retrazado; desde ahí el hilo principal corre en
 * la fase de campo contraria y su recarga cae justo antes del cuadro de
 * audio, cada cuadro: el reproductor nunca llega a sonar y la canción se
 * vuelve a cargar 30 veces por segundo durante ~96 s, hasta que la fase
 * cambia; entonces entra a destiempo con la letra.
 *
 * El arreglo cambia ese camino para que con el estado en 1 (PLAY enviado,
 * aún sin confirmar) el gestor espere al reproductor en vez de recargar la
 * canción; con 0 carga y con 2 sigue como el original. Son 20 palabras en
 * 0x806029DC–0x80602A28: como el «or a0, s1» de la carga se mueve a la
 * ranura del primer salto, s1 deja de usarse y las dos llamadas toman a0
 * de s4 (s1 = s4 en el original).
 *
 * Conker's Bad Fur Day (USA, NFUE), el Great Mighty Poo sin abrir la boca
 * (issue #3). El código del juego va por la TLB en 0x10000000, que apunta a
 * la RDRAM física 0: el pc 0x1000xxxx es la física 0x0000xxxx. El gestor de
 * música (0x1000D2F8, una vez por cuadro y por canal, hilo 3) lleva por canal
 * un registro de la canción (raíces en 0x800417B0). Para cambiar de canción
 * para la que suena (0x10008CE8: STOP y espera activa a que el hilo de audio,
 * el 4, deje el reproductor parado), pone el registro nuevo, carga y manda
 * PLAY (0x100084D8 → 0x10017AA0, un evento en la cola del reproductor que
 * el hilo de audio procesa en su siguiente cuadro, cada 2 campos). En cada
 * cuadro, si ve el reproductor parado (estado en +0x2C igual a 0), da la
 * canción por terminada y borra su registro. Con nuestro CPU el cuadro
 * siguiente del juego puede llegar antes que el cuadro de audio: el gestor ve
 * parado un reproductor al que acaba de mandar PLAY, borra el registro y la
 * canción suena sin él: el gestor ya no la atiende y el Poo no abre la boca
 * (la escena espera). En el host, entrando a Sweet Melody desde Chapters:
 * PLAY en el campo 13150, registro borrado en el 13151 y PLAY procesado en el
 * 13152 (la canción de la cueva queda huérfana); en el jefe, PLAY en el
 * 16952, borrado en el 16953, procesado en el 16954. Con 2 ciclos por
 * instrucción (-DSALSA64_CPI=2) el mismo recorrido funciona.
 *
 * El arreglo reescribe la función que manda PLAY (0x100084D8, 25 palabras):
 * hace lo mismo (PLAY si el reproductor está parado o en pausa) y, después de
 * mandarlo, espera a que el estado deje de ser 0, como ya espera el STOP en
 * 0x10008CE8 (tope de 0x1E0000 vueltas, unos 6 campos). El estado se lee
 * directo (+0x2C), que es lo que hace 0x10017A80.
 */
#include "core/gamefix.h"

#include <string.h>

#include "core/bus.h"

struct gamefix {
	const char *name;
	const char *off;    /* archivo del directorio de datos que lo apaga, o NULL */
	char code[4];       /* encabezado 0x3B–0x3E */
	uint32_t crc1, crc2;  /* encabezado 0x10 y 0x14 */
	uint32_t pa;        /* dirección física del código */
	unsigned n;         /* palabras */
	uint32_t orig[25];
	uint32_t fix[25];
};

static const struct gamefix fixes[] = {
	{
		"Donkey Kong 64 (USA): retrazados entre cuadros de las escenas con mínimo 1", NULL,
		{ 'N', 'D', 'O', 'E' }, 0xEC58EABFu, 0xAD7C7169u, 0x00600914u, 11,
		{
			0x3C038076u,  /* lui   v1, 0x8076 */
			0x3C048077u,  /* lui   a0, 0x8077 */
			0x2484AF10u,  /* addiu a0, a0, -0x50F0      a0 = &anterior */
			0x24637A40u,  /* addiu v1, v1, 0x7A40 */
			0x8C6E0284u,  /* lw    t6, 0x284(v1)        contador */
			0x8C8F0000u,  /* lw    t7, 0(a0)            anterior */
			0x3C018074u,  /* lui   at, 0x8074 */
			0x01CFC023u,  /* subu  t8, t6, t7 */
			0xAC384478u,  /* sw    t8, 0x4478(at)       retrazados, puede ser 0 */
			0x8C790284u,  /* lw    t9, 0x284(v1) */
			0xAC990000u,  /* sw    t9, 0(a0)            anterior = contador */
		},
		{
			0x3C038076u,  /* lui   v1, 0x8076 */
			0x3C048077u,  /* lui   a0, 0x8077 */
			0x8C6E7CC4u,  /* lw    t6, 0x7CC4(v1)       contador */
			0x8C8FAF10u,  /* lw    t7, -0x50F0(a0)      anterior */
			0xAC8EAF10u,  /* sw    t6, -0x50F0(a0)      anterior = contador */
			0x01CFC023u,  /* subu  t8, t6, t7 */
			0x2F190001u,  /* sltiu t9, t8, 1 */
			0x0319C021u,  /* addu  t8, t8, t9           mínimo 1 */
			0x3C018074u,  /* lui   at, 0x8074 */
			0xAC384478u,  /* sw    t8, 0x4478(at) */
			0x00000000u,  /* nop */
		},
	},
	{
		"Donkey Kong 64 (USA): el gestor de música espera el PLAY en vez de recargar la canción",
		"dk64-musica-vieja",
		{ 'N', 'D', 'O', 'E' }, 0xEC58EABFu, 0xAD7C7169u, 0x006029DCu, 20,
		{
			0x02808825u,  /* or    s1, s4, zero         (ranura de «bne canción != pedida, carga») */
			0x87395924u,  /* lh    t9, 0x5924(t9)       estado de la petición */
			0x24010002u,  /* addiu at, zero, 2 */
			0x02202025u,  /* or    a0, s1, zero */
			0x1721000Bu,  /* bne   t9, at, carga        0 o 1: parar, cargar y PLAY otra vez */
			0x00034080u,  /* sll   t0, v1, 2 */
			0x02684821u,  /* addu  t1, s3, t0 */
			0x0C182983u,  /* jal   0x8060a60c           2: volumen y PLAY sin recargar */
			0x8D250000u,  /* lw    a1, 0(t1) */
			0x00145080u,  /* sll   t2, s4, 2 */
			0x3C048077u,  /* lui   a0, 0x8077 */
			0x008A2021u,  /* addu  a0, a0, t2 */
			0x0C1CDF94u,  /* jal   0x80737e50 */
			0x8C84BF20u,  /* lw    a0, -0x40E0(a0)      reproductor de la casilla */
			0x1000000Fu,  /* b     siguiente casilla */
			0x26940001u,  /* addiu s4, s4, 1 */
			0x00035880u,  /* carga: sll t3, v1, 2 */
			0x026B6021u,  /* addu  t4, s3, t3 */
			0x8D860000u,  /* lw    a2, 0(t4)            volumen */
			0x02202025u,  /* or    a0, s1, zero         casilla */
		},
		{
			0x02802025u,  /* or    a0, s4, zero         casilla, para los dos caminos */
			0x87395924u,  /* lh    t9, 0x5924(t9) */
			0x1320000Du,  /* beq   t9, zero, carga      0: cargar y PLAY */
			0x00034080u,  /* sll   t0, v1, 2 */
			0x33210001u,  /* andi  at, t9, 1 */
			0x14200017u,  /* bne   at, zero, 0x80602a50 1: esperar al reproductor (siguiente casilla) */
			0x02684821u,  /* addu  t1, s3, t0 */
			0x0C182983u,  /* jal   0x8060a60c           2: como el original */
			0x8D250000u,  /* lw    a1, 0(t1) */
			0x00145080u,  /* sll   t2, s4, 2 */
			0x3C048077u,  /* lui   a0, 0x8077 */
			0x008A2021u,  /* addu  a0, a0, t2 */
			0x0C1CDF94u,  /* jal   0x80737e50 */
			0x8C84BF20u,  /* lw    a0, -0x40E0(a0) */
			0x1000000Fu,  /* b     siguiente casilla */
			0x26940001u,  /* addiu s4, s4, 1 */
			0x00035880u,  /* carga: sll t3, v1, 2 */
			0x026B6021u,  /* addu  t4, s3, t3 */
			0x8D860000u,  /* lw    a2, 0(t4) */
			0x02802025u,  /* or    a0, s4, zero */
		},
	},
	{
		"Conker's Bad Fur Day (USA): el PLAY de una canción espera a que el hilo de audio lo procese",
		"conker-musica-vieja",
		{ 'N', 'F', 'U', 'E' }, 0x30C7AC50u, 0x7704072Du, 0x000084D8u, 25,
		{
			0x27BDFFE0u,  /* addiu sp, sp, -32 */
			0xAFBF0014u,  /* sw    ra, 20(sp) */
			0xAFA40020u,  /* sw    a0, 32(sp) */
			0x308500FFu,  /* andi  a1, a0, 0xff         reproductor */
			0x3C0F8004u,  /* lui   t7, 0x8004 */
			0x25EFC900u,  /* addiu t7, t7, -0x3700      tabla de reproductores (0x8003C900) */
			0x00057080u,  /* sll   t6, a1, 2 */
			0x01CF1021u,  /* addu  v0, t6, t7 */
			0x8C440000u,  /* lw    a0, 0(v0) */
			0x0C005EA0u,  /* jal   0x10017a80           estado */
			0xAFA2001Cu,  /* sw    v0, 28(sp) */
			0x10400006u,  /* beq   v0, zero, play       parado: PLAY */
			0x8FB8001Cu,  /* lw    t8, 28(sp) */
			0x0C005EA0u,  /* jal   0x10017a80 */
			0x8F040000u,  /* lw    a0, 0(t8) */
			0x24010003u,  /* addiu at, zero, 3 */
			0x54410005u,  /* bnel  v0, at, salida       solo PLAY si está en pausa (3) */
			0x8FBF0014u,  /* lw    ra, 20(sp) */
			0x8FB9001Cu,  /* play: lw t9, 28(sp) */
			0x0C005EA8u,  /* jal   0x10017aa0           PLAY (evento 0x0F a su cola) */
			0x8F240000u,  /* lw    a0, 0(t9) */
			0x8FBF0014u,  /* lw    ra, 20(sp) */
			0x27BD0020u,  /* salida: addiu sp, sp, 32 */
			0x03E00008u,  /* jr    ra */
			0x00000000u,  /* nop */
		},
		{
			0x27BDFFE0u,  /* addiu sp, sp, -32 */
			0xAFBF0014u,  /* sw    ra, 20(sp) */
			0x308500FFu,  /* andi  a1, a0, 0xff */
			0x3C0F8004u,  /* lui   t7, 0x8004 */
			0x25EFC900u,  /* addiu t7, t7, -0x3700 */
			0x00057080u,  /* sll   t6, a1, 2 */
			0x01CF1021u,  /* addu  v0, t6, t7 */
			0x8C440000u,  /* lw    a0, 0(v0)            reproductor */
			0x8C82002Cu,  /* lw    v0, 0x2C(a0)         estado (lo mismo que 0x10017a80) */
			0x10400003u,  /* beq   v0, zero, play */
			0x24010003u,  /* addiu at, zero, 3 */
			0x14410009u,  /* bne   v0, at, salida */
			0x00000000u,  /* nop */
			0x0C005EA8u,  /* play: jal 0x10017aa0 */
			0xAFA4001Cu,  /* sw    a0, 28(sp) */
			0x8FA4001Cu,  /* lw    a0, 28(sp) */
			0x3C01001Eu,  /* lui   at, 0x1E             tope: 0x1E0000 vueltas */
			0x8C82002Cu,  /* espera: lw v0, 0x2C(a0) */
			0x14400002u,  /* bne   v0, zero, salida     el hilo de audio ya lo procesó */
			0x2421FFFFu,  /* addiu at, at, -1 */
			0x1C20FFFCu,  /* bgtz  at, espera */
			0x8FBF0014u,  /* salida: lw ra, 20(sp) */
			0x03E00008u,  /* jr    ra */
			0x27BD0020u,  /* addiu sp, sp, 32 */
			0x00000000u,  /* nop */
		},
	},
};

#define NFIXES (sizeof fixes / sizeof fixes[0])

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

void salsa64_gamefix_select(struct salsa64_bus *bus)
{
	bus->gamefix_sel = bus->gamefix_on = 0;
	if (!bus->rom || bus->rom_resident < 0x40)
		return;
	const uint8_t *h = bus->rom;
	for (unsigned i = 0; i < NFIXES; i++) {
		const struct gamefix *f = &fixes[i];
		if (!memcmp(h + 0x3B, f->code, 4) && rd32(h + 0x10) == f->crc1 && rd32(h + 0x14) == f->crc2)
			bus->gamefix_sel |= 1u << i;
	}
	bus->gamefix_on = bus->gamefix_sel;
}

/* Índice en fixes[] del k-ésimo arreglo elegido, o -1. */
static int nth(const struct salsa64_bus *bus, unsigned k)
{
	for (unsigned i = 0; i < NFIXES; i++)
		if ((bus->gamefix_sel >> i & 1) && !k--)
			return (int)i;
	return -1;
}

const char *salsa64_gamefix_name(const struct salsa64_bus *bus, unsigned k)
{
	int i = nth(bus, k);
	return i < 0 ? NULL : fixes[i].name;
}

const char *salsa64_gamefix_switch(const struct salsa64_bus *bus, unsigned k)
{
	int i = nth(bus, k);
	return i < 0 ? NULL : fixes[i].off;
}

bool salsa64_gamefix_enabled(const struct salsa64_bus *bus, unsigned k)
{
	int i = nth(bus, k);
	return i >= 0 && (bus->gamefix_on >> i & 1);
}

void salsa64_gamefix_disable(struct salsa64_bus *bus, unsigned k)
{
	int i = nth(bus, k);
	if (i >= 0)
		bus->gamefix_on &= ~(1u << i);
}

static void apply(struct salsa64_bus *bus, const struct gamefix *f)
{
	if ((uint64_t)f->pa + 4 * f->n > bus->rdram_size)
		return;
	uint8_t *p = bus->rdram + f->pa;
	for (unsigned k = 0; k < f->n; k++)
		if (rd32(p + 4 * k) != f->orig[k])
			return;  /* todavía no está cargado, ya está arreglado o es otro código */
	for (unsigned k = 0; k < f->n; k++) {
		p[4 * k] = (uint8_t)(f->fix[k] >> 24);
		p[4 * k + 1] = (uint8_t)(f->fix[k] >> 16);
		p[4 * k + 2] = (uint8_t)(f->fix[k] >> 8);
		p[4 * k + 3] = (uint8_t)f->fix[k];
	}
	salsa64_bus_rdram_patched(bus, f->pa, 4 * f->n);
	bus->gamefix_applied++;
}

void salsa64_gamefix_field(struct salsa64_bus *bus)
{
	for (unsigned i = 0; i < NFIXES; i++)
		if (bus->gamefix_on >> i & 1)
			apply(bus, &fixes[i]);
}
