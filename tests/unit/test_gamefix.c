/*
 * Salsa64 — pruebas de los arreglos por juego (core/gamefix.c): solo con el
 * encabezado exacto y las palabras originales en la RDRAM, al empezar un
 * campo, una vez, y nunca en otro juego.
 */
#include "test.h"

#include <string.h>

#include "core/endian.h"
#include "core/gamefix.h"

#define DK64_PA 0x00600914u
#define DK64_MUS_PA 0x006029DCu

static const uint32_t dk64_orig[11] = {
	0x3C038076u, 0x3C048077u, 0x2484AF10u, 0x24637A40u, 0x8C6E0284u, 0x8C8F0000u,
	0x3C018074u, 0x01CFC023u, 0xAC384478u, 0x8C790284u, 0xAC990000u,
};

/* Gestor de música (0x806029DC–0x80602A28), como lo carga el juego. */
static const uint32_t dk64_mus_orig[20] = {
	0x02808825u, 0x87395924u, 0x24010002u, 0x02202025u, 0x1721000Bu, 0x00034080u, 0x02684821u,
	0x0C182983u, 0x8D250000u, 0x00145080u, 0x3C048077u, 0x008A2021u, 0x0C1CDF94u, 0x8C84BF20u,
	0x1000000Fu, 0x26940001u, 0x00035880u, 0x026B6021u, 0x8D860000u, 0x02202025u,
};

static uint8_t rom[4096];

static void set_header(const char *code, uint32_t crc1, uint32_t crc2)
{
	memset(rom, 0, sizeof rom);
	salsa64_wr32(rom + 0x10, crc1);
	salsa64_wr32(rom + 0x14, crc2);
	memcpy(rom + 0x3B, code, 4);
	salsa64_bus_set_rom(&t_n64.bus, rom, sizeof rom);
}

static void load_orig(void)
{
	for (unsigned k = 0; k < 11; k++)
		t_wr32(DK64_PA + 4 * k, dk64_orig[k]);
}

static void load_mus_orig(void)
{
	for (unsigned k = 0; k < 20; k++)
		t_wr32(DK64_MUS_PA + 4 * k, dk64_mus_orig[k]);
}

static void spin_field(void)
{
	T_LOAD(BEQ(0, 0, 0xFFFF), NOP);
	salsa64_n64_run_field(&t_n64);
}

static void dk64_patched_once(void)
{
	set_header("NDOE", 0xEC58EABFu, 0xAD7C7169u);
	CHECK(salsa64_gamefix_name(&t_n64.bus, 0) != NULL);
	load_orig();
	spin_field();
	CHECK_EQ(t_n64.bus.gamefix_applied, 1);
	CHECK_EQ(t_rd32(DK64_PA + 4 * 2), 0x8C6E7CC4u);   /* lw t6, 0x7CC4(v1) */
	CHECK_EQ(t_rd32(DK64_PA + 4 * 6), 0x2F190001u);   /* sltiu t9, t8, 1 */
	CHECK_EQ(t_rd32(DK64_PA + 4 * 7), 0x0319C021u);   /* addu t8, t8, t9 */
	CHECK_EQ(t_rd32(DK64_PA + 4 * 10), 0x00000000u);
	spin_field();  /* ya arreglado: no se vuelve a aplicar */
	CHECK_EQ(t_n64.bus.gamefix_applied, 1);
	load_orig();   /* el juego vuelve a cargar el código: otra vez */
	spin_field();
	CHECK_EQ(t_n64.bus.gamefix_applied, 2);
	salsa64_bus_set_rom(&t_n64.bus, NULL, 0);
}

static void dk64_other_code_untouched(void)
{
	set_header("NDOE", 0xEC58EABFu, 0xAD7C7169u);
	load_orig();
	t_wr32(DK64_PA + 4 * 9, 0x00000000u);  /* una palabra distinta */
	spin_field();
	CHECK_EQ(t_n64.bus.gamefix_applied, 0);
	CHECK_EQ(t_rd32(DK64_PA + 4 * 2), dk64_orig[2]);
	salsa64_bus_set_rom(&t_n64.bus, NULL, 0);
}

static void other_games_untouched(void)
{
	set_header("NDOP", 0xEC58EABFu, 0xAD7C7169u);  /* otra región */
	CHECK(salsa64_gamefix_name(&t_n64.bus, 0) == NULL);
	set_header("NDOE", 0x0DD4ABABu, 0xB5A2A91Eu);  /* otro CRC (la demo de quiosco) */
	CHECK(salsa64_gamefix_name(&t_n64.bus, 0) == NULL);
	load_orig();
	load_mus_orig();
	spin_field();
	CHECK_EQ(t_n64.bus.gamefix_applied, 0);
	CHECK_EQ(t_rd32(DK64_PA + 4 * 2), dk64_orig[2]);
	CHECK_EQ(t_rd32(DK64_MUS_PA + 4 * 2), dk64_mus_orig[2]);
	salsa64_bus_set_rom(&t_n64.bus, NULL, 0);
	CHECK(salsa64_gamefix_name(&t_n64.bus, 0) == NULL);
}

/* Los dos arreglos de DK64 USA, en orden; solo el de la música tiene interruptor. */
static void dk64_two_fixes(void)
{
	set_header("NDOE", 0xEC58EABFu, 0xAD7C7169u);
	CHECK(salsa64_gamefix_name(&t_n64.bus, 0) != NULL);
	CHECK(salsa64_gamefix_name(&t_n64.bus, 1) != NULL);
	CHECK(salsa64_gamefix_name(&t_n64.bus, 2) == NULL);
	CHECK(salsa64_gamefix_switch(&t_n64.bus, 0) == NULL);
	CHECK(salsa64_gamefix_switch(&t_n64.bus, 1) != NULL);
	CHECK(!strcmp(salsa64_gamefix_switch(&t_n64.bus, 1), "dk64-musica-vieja"));
	CHECK(salsa64_gamefix_enabled(&t_n64.bus, 0) && salsa64_gamefix_enabled(&t_n64.bus, 1));
	salsa64_bus_set_rom(&t_n64.bus, NULL, 0);
}

/* Con el estado de la petición en 1 el gestor pasa a la siguiente casilla;
 * con 0 carga; con 2 sigue como el original. */
static void dk64_music_patched(void)
{
	set_header("NDOE", 0xEC58EABFu, 0xAD7C7169u);
	t_wr32(DK64_PA, 0);  /* sin el código de los retrazados: solo este arreglo */
	load_mus_orig();
	spin_field();
	CHECK_EQ(t_n64.bus.gamefix_applied, 1);
	CHECK_EQ(t_rd32(DK64_MUS_PA + 4 * 0), 0x02802025u);   /* or   a0, s4, zero */
	CHECK_EQ(t_rd32(DK64_MUS_PA + 4 * 2), 0x1320000Du);   /* beq  t9, zero, 0x80602a1c */
	CHECK_EQ(t_rd32(DK64_MUS_PA + 4 * 4), 0x33210001u);   /* andi at, t9, 1 */
	CHECK_EQ(t_rd32(DK64_MUS_PA + 4 * 5), 0x14200017u);   /* bne  at, zero, 0x80602a50 */
	CHECK_EQ(t_rd32(DK64_MUS_PA + 4 * 19), 0x02802025u);  /* or   a0, s4, zero */
	for (unsigned k = 6; k < 19; k++)
		CHECK_EQ(t_rd32(DK64_MUS_PA + 4 * k), dk64_mus_orig[k]);
	spin_field();
	CHECK_EQ(t_n64.bus.gamefix_applied, 1);
	salsa64_bus_set_rom(&t_n64.bus, NULL, 0);
}

static void dk64_music_switch_off(void)
{
	set_header("NDOE", 0xEC58EABFu, 0xAD7C7169u);
	salsa64_gamefix_disable(&t_n64.bus, 1);
	CHECK(salsa64_gamefix_enabled(&t_n64.bus, 0));
	CHECK(!salsa64_gamefix_enabled(&t_n64.bus, 1));
	load_orig();
	load_mus_orig();
	spin_field();
	CHECK_EQ(t_n64.bus.gamefix_applied, 1);  /* solo el de los retrazados */
	CHECK_EQ(t_rd32(DK64_PA + 4 * 2), 0x8C6E7CC4u);
	for (unsigned k = 0; k < 20; k++)
		CHECK_EQ(t_rd32(DK64_MUS_PA + 4 * k), dk64_mus_orig[k]);
	salsa64_bus_set_rom(&t_n64.bus, NULL, 0);  /* otra ROM: vuelve a estar encendido */
	set_header("NDOE", 0xEC58EABFu, 0xAD7C7169u);
	CHECK(salsa64_gamefix_enabled(&t_n64.bus, 1));
	salsa64_bus_set_rom(&t_n64.bus, NULL, 0);
}

/* Conker USA: la función que manda PLAY (0x100084D8 por la TLB, física
 * 0x84D8), como la carga el juego. */
#define CONKER_PA 0x000084D8u

static const uint32_t conker_orig[25] = {
	0x27BDFFE0u, 0xAFBF0014u, 0xAFA40020u, 0x308500FFu, 0x3C0F8004u, 0x25EFC900u, 0x00057080u,
	0x01CF1021u, 0x8C440000u, 0x0C005EA0u, 0xAFA2001Cu, 0x10400006u, 0x8FB8001Cu, 0x0C005EA0u,
	0x8F040000u, 0x24010003u, 0x54410005u, 0x8FBF0014u, 0x8FB9001Cu, 0x0C005EA8u, 0x8F240000u,
	0x8FBF0014u, 0x27BD0020u, 0x03E00008u, 0x00000000u,
};

static void conker_header(void)
{
	set_header("NFUE", 0x30C7AC50u, 0x7704072Du);
}

static void load_conker_orig(void)
{
	for (unsigned k = 0; k < 25; k++)
		t_wr32(CONKER_PA + 4 * k, conker_orig[k]);
}

/* Para correr la función parcheada desde KSEG0 (0x800084D8: misma RDRAM; sus
 * jal van a 0x80017A80/0x80017AA0, también la misma): la tabla de
 * reproductores en 0x8003C900 con el 0 en 0x80040000 (estado en +0x2C), y en
 * 0x80017AA0 un PLAY de prueba que cuenta las llamadas en T_DATA y, si
 * T_DATA+4 no es 0, lo deja como estado (el hilo de audio que lo procesa en
 * el acto). T_CODE llama con a0 = 0 y se queda en un `b .`. */
#define CONKER_PLAYER_PA 0x00040000u
#define CONKER_STATE_PA  (CONKER_PLAYER_PA + 0x2C)

static void conker_setup(uint32_t state, uint32_t consume)
{
	t_wr32(0x0003C900u, 0x80000000u | CONKER_PLAYER_PA);
	t_wr32(CONKER_STATE_PA, state);
	t_wr32(T_DATA_PA, 0);
	t_wr32(T_DATA_PA + 4, consume);
	static const uint32_t play[] = {
		LUI(9, 0x8000), LW(8, 0x2000, 9), ADDIU(8, 8, 1), SW(8, 0x2000, 9), LW(10, 0x2004, 9),
		BEQ(10, 0, 2), NOP, SW(10, 0x2C, 4), JR(31), NOP,
	};
	for (unsigned k = 0; k < sizeof play / sizeof play[0]; k++)
		t_wr32(0x00017AA0u + 4 * k, play[k]);
	T_LOAD(JAL(0x800084D8u), ADDIU(4, 0, 0), BEQ(0, 0, 0xFFFF), NOP);
	salsa64_cpu_set_pc(CPU, T_CODE);
	R[29] = 0xFFFFFFFF80300000ull;
}

static bool conker_returned(void)
{
	return (CPU->pc == T_CODE + 8 || CPU->pc == T_CODE + 12) && R[29] == 0xFFFFFFFF80300000ull;
}

static bool conker_waiting(void)
{
	return CPU->pc >= 0xFFFFFFFF8000851Cull && CPU->pc <= 0xFFFFFFFF8000852Cull;
}

static void conker_patched(void)
{
	conker_header();
	CHECK(salsa64_gamefix_name(&t_n64.bus, 0) != NULL);
	CHECK(salsa64_gamefix_name(&t_n64.bus, 1) == NULL);
	CHECK(!strcmp(salsa64_gamefix_switch(&t_n64.bus, 0), "conker-musica-vieja"));
	load_conker_orig();
	spin_field();
	CHECK_EQ(t_n64.bus.gamefix_applied, 1);
	CHECK_EQ(t_rd32(CONKER_PA + 4 * 8), 0x8C82002Cu);   /* lw   v0, 0x2C(a0) */
	CHECK_EQ(t_rd32(CONKER_PA + 4 * 13), 0x0C005EA8u);  /* jal  0x10017aa0 */
	CHECK_EQ(t_rd32(CONKER_PA + 4 * 16), 0x3C01001Eu);  /* lui  at, 0x1E */
	CHECK_EQ(t_rd32(CONKER_PA + 4 * 20), 0x1C20FFFCu);  /* bgtz at, espera */
	CHECK_EQ(t_rd32(CONKER_PA + 4 * 24), 0x00000000u);
	spin_field();
	CHECK_EQ(t_n64.bus.gamefix_applied, 1);
	salsa64_bus_set_rom(&t_n64.bus, NULL, 0);
}

static void conker_other_code_untouched(void)
{
	conker_header();
	load_conker_orig();
	t_wr32(CONKER_PA + 4 * 19, 0x0C005EA9u);  /* otra llamada */
	spin_field();
	CHECK_EQ(t_n64.bus.gamefix_applied, 0);
	CHECK_EQ(t_rd32(CONKER_PA + 4 * 8), conker_orig[8]);
	set_header("NFUE", 0x30C7AC50u, 0x00000000u);  /* otro CRC */
	CHECK(salsa64_gamefix_name(&t_n64.bus, 0) == NULL);
	set_header("NFUP", 0x30C7AC50u, 0x7704072Du);  /* otra región */
	CHECK(salsa64_gamefix_name(&t_n64.bus, 0) == NULL);
	salsa64_bus_set_rom(&t_n64.bus, NULL, 0);
}

/* Parado: manda PLAY y espera a que el estado deje de ser 0. */
static void conker_play_waits(void)
{
	conker_header();
	load_conker_orig();
	spin_field();
	conker_setup(0, 0);
	t_step(400);
	CHECK_EQ(t_rd32(T_DATA_PA), 1);
	CHECK(conker_waiting());
	t_wr32(CONKER_STATE_PA, 1);  /* el hilo de audio procesó el PLAY */
	t_step(20);
	CHECK(conker_returned());
	CHECK_EQ(t_rd32(T_DATA_PA), 1);
	salsa64_bus_set_rom(&t_n64.bus, NULL, 0);
}

/* Parado y procesado en el acto: vuelve enseguida; sonando: sin PLAY; en
 * pausa (3): PLAY sin esperar, como el original. */
static void conker_play_states(void)
{
	conker_header();
	load_conker_orig();
	spin_field();
	conker_setup(0, 1);
	t_step(60);
	CHECK(conker_returned());
	CHECK_EQ(t_rd32(T_DATA_PA), 1);
	CHECK_EQ(t_rd32(CONKER_STATE_PA), 1);
	conker_setup(1, 0);
	t_step(60);
	CHECK(conker_returned());
	CHECK_EQ(t_rd32(T_DATA_PA), 0);
	conker_setup(3, 0);
	t_step(60);
	CHECK(conker_returned());
	CHECK_EQ(t_rd32(T_DATA_PA), 1);
	conker_setup(2, 0);  /* deteniéndose: ni PLAY ni espera */
	t_step(60);
	CHECK(conker_returned());
	CHECK_EQ(t_rd32(T_DATA_PA), 0);
	salsa64_bus_set_rom(&t_n64.bus, NULL, 0);
}

/* Si el PLAY nunca se procesa, la espera termina (con el tope reducido a
 * 0x10000 vueltas para la prueba). */
static void conker_wait_bounded(void)
{
	conker_header();
	load_conker_orig();
	spin_field();
	t_wr32(CONKER_PA + 4 * 16, LUI(1, 1));
	conker_setup(0, 0);
	t_step(0x10000 * 5 + 200);
	CHECK(conker_returned());
	CHECK_EQ(t_rd32(T_DATA_PA), 1);
	CHECK_EQ(t_rd32(CONKER_STATE_PA), 0);
	salsa64_bus_set_rom(&t_n64.bus, NULL, 0);
}

static void conker_switch_off(void)
{
	conker_header();
	salsa64_gamefix_disable(&t_n64.bus, 0);
	CHECK(!salsa64_gamefix_enabled(&t_n64.bus, 0));
	load_conker_orig();
	spin_field();
	CHECK_EQ(t_n64.bus.gamefix_applied, 0);
	for (unsigned k = 0; k < 25; k++)
		CHECK_EQ(t_rd32(CONKER_PA + 4 * k), conker_orig[k]);
	salsa64_bus_set_rom(&t_n64.bus, NULL, 0);
}

void t_suite_gamefix(void)
{
	RUN(dk64_patched_once);
	RUN(dk64_other_code_untouched);
	RUN(other_games_untouched);
	RUN(dk64_two_fixes);
	RUN(dk64_music_patched);
	RUN(dk64_music_switch_off);
	RUN(conker_patched);
	RUN(conker_other_code_untouched);
	RUN(conker_play_waits);
	RUN(conker_play_states);
	RUN(conker_wait_bounded);
	RUN(conker_switch_off);
}
