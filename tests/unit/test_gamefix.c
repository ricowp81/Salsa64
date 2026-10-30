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

void t_suite_gamefix(void)
{
	RUN(dk64_patched_once);
	RUN(dk64_other_code_untouched);
	RUN(other_games_untouched);
	RUN(dk64_two_fixes);
	RUN(dk64_music_patched);
	RUN(dk64_music_switch_off);
}
