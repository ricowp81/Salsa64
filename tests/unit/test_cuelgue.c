/*
 * Salsa64 — pruebas del aviso de juego colgado (src/frontend/cuelgue.c): solo
 * avisa de un juego que antes leía los controles o dibujaba, tras
 * CUELGUE_US sin hacer ninguna de las dos cosas con el VI avanzando, una vez
 * por episodio, y el texto trae lo que hace falta para diagnosticarlo.
 */
#include "test.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "frontend/cuelgue.h"

static char said[256][400];
static int nsaid;

static void fake_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (nsaid < 256)
		vsnprintf(said[nsaid++], sizeof said[0], fmt, ap);
	va_end(ap);
}

static struct cuelgue cg;
static uint64_t clock_us;

/* Medio segundo de reloj con 30 campos más en el VI y, si se pide, un
 * control leído y una tarea gráfica más. */
static bool tick(bool active, bool expect, bool vi_runs)
{
	struct salsa64_bus *bus = &t_n64.bus;
	if (vi_runs)
		bus->vi.fields += 30;
	if (active) {
		bus->pif.reads[0]++;
		bus->rsp.stats.gfx++;
	}
	clock_us += 500000;
	return cuelgue_watch(&cg, &t_n64, clock_us, expect, NULL, fake_log);
}

static void cuelgue_avisa_una_vez(void)
{
	nsaid = 0;
	clock_us = 1000000;
	cuelgue_reset(&cg, false);
	for (int i = 0; i < 12; i++)
		CHECK(!tick(true, true, true));  /* vivo: nada */
	/* Sin actividad: nada hasta CUELGUE_US, y entonces avisa una sola vez. */
	int fired = 0;
	for (int i = 0; i < 40; i++)
		fired += tick(false, true, true);
	CHECK_EQ(fired, 1);
	CHECK(nsaid >= 8);
	CHECK(strstr(said[0], "no lee los controles"));
	/* Vuelve a vivir: lo dice y puede avisar de nuevo en otro episodio. */
	CHECK(!tick(true, true, true));
	CHECK(strstr(said[nsaid - 1], "volvió"));
	for (int i = 0; i < 12; i++)
		tick(true, true, true);
	fired = 0;
	for (int i = 0; i < 40; i++)
		fired += tick(false, true, true);
	CHECK_EQ(fired, 1);
}

static void cuelgue_no_avisa_sin_motivo(void)
{
	/* Un juego que nunca dio señales de vida: no está colgado. */
	nsaid = 0;
	clock_us = 1000000;
	cuelgue_reset(&cg, false);
	for (int i = 0; i < 60; i++)
		CHECK(!tick(false, true, true));
	CHECK_EQ(nsaid, 0);
	/* En pausa (o sin juego) tampoco, aunque no haya actividad. */
	cuelgue_reset(&cg, false);
	for (int i = 0; i < 12; i++)
		tick(true, true, true);
	for (int i = 0; i < 60; i++)
		CHECK(!tick(false, false, true));
	CHECK_EQ(nsaid, 0);
	/* Ni con el VI quieto (de eso se ocupa el latido de los hilos). */
	cuelgue_reset(&cg, false);
	for (int i = 0; i < 12; i++)
		tick(true, true, true);
	for (int i = 0; i < 60; i++)
		CHECK(!tick(false, true, false));
	CHECK_EQ(nsaid, 0);
}

/* Con latido-siempre avisa también de un juego que nunca dio señales de vida. */
static void cuelgue_avisa_siempre(void)
{
	nsaid = 0;
	clock_us = 1000000;
	cuelgue_reset(&cg, true);
	int fired = 0;
	for (int i = 0; i < 60; i++)
		fired += tick(false, true, true);
	CHECK_EQ(fired, 1);
	CHECK(strstr(said[0], "no lee los controles"));
	/* Sigue sin avisar en pausa ni con el VI quieto. */
	cuelgue_reset(&cg, true);
	for (int i = 0; i < 60; i++)
		CHECK(!tick(false, false, true));
	cuelgue_reset(&cg, true);
	for (int i = 0; i < 60; i++)
		CHECK(!tick(false, true, false));
}

static void cuelgue_texto(void)
{
	struct salsa64_bus *bus = &t_n64.bus;
	bus->mi.intr = 0x15;
	bus->mi.mask = 0x3F;
	bus->rsp.stats.dp_sched = 7;
	bus->rsp.stats.dp_fired = 6;
	bus->rsp.stats.gfx_nosync = 2;
	salsa64_sched_set(&t_n64.sched, SALSA64_EV_DP_SYNC, CPU->cycles + 5000);
	salsa64_bus_write32(bus, 0x04800000, 0x4000, ~0u);
	salsa64_bus_write32(bus, 0x04800010, 0x1FC007C0, ~0u);
	CHECK_EQ(salsa64_si_read(bus, SALSA64_SI_STATUS << 2), 0x411);
	char text[8192];
	unsigned n = cuelgue_state_text(&t_n64, text, sizeof text);
	CHECK(n > 100 && n < sizeof text);
	CHECK(strstr(text, "MI: INTR 15 máscara 3f"));
	CHECK(strstr(text, "DP programadas 7, levantadas 6"));
	CHECK(strstr(text, "sin SYNC_FULL ni corte 2"));
	CHECK(strstr(text, "DP +5000"));
	CHECK(strstr(text, "SI: STATUS 00000411"));
	CHECK(strstr(text, "SI +6098"));
	CHECK(strstr(text, "tarea -"));
	CHECK(strstr(text, "eventos del RCP"));
	/* Un búfer chico se corta sin pasarse. */
	char small[40];
	n = cuelgue_state_text(&t_n64, small, sizeof small);
	CHECK(n < sizeof small);
	salsa64_sched_cancel(&t_n64.sched, SALSA64_EV_DP_SYNC);
}

/* El aviso escribe el volcado de la RDRAM y el de los registros, y no más de CUELGUE_DUMPS por juego. */
static void cuelgue_volcado(void)
{
	char dir[] = "/tmp/salsa64-cuelgue-XXXXXX";
	CHECK(mkdtemp(dir) != NULL);
	nsaid = 0;
	clock_us = 1000000;
	cuelgue_reset(&cg, false);
	t_wr32(0x1000, 0xCAFEBABE);
	CPU->cur_pc = 0xFFFFFFFF80000400ull;
	for (int i = 0; i < 12; i++)
		tick(true, true, true);
	for (int ep = 0; ep < CUELGUE_DUMPS + 2; ep++) {
		/* Un episodio: sin actividad hasta el aviso, y un poco de vida para el siguiente. */
		for (int i = 0; i < 40; i++)
			cuelgue_watch(&cg, &t_n64, (clock_us += 500000, t_n64.bus.vi.fields += 30, clock_us), true, dir, fake_log);
		for (int i = 0; i < 8; i++)
			tick(true, true, true);
	}
	char path[160];
	uint32_t field = 0;
	int dumps = 0;
	for (int i = 0; i < nsaid; i++)
		if (sscanf(said[i], "latido: volcado de la RDRAM en %*[^ ] y de los registros en cuelgue-%u.txt: bien", &field) == 1)
			dumps++;
	CHECK_EQ(dumps, CUELGUE_DUMPS);
	snprintf(path, sizeof path, "%s/cuelgue-%u.rdram", dir, field);
	struct stat st;
	CHECK(stat(path, &st) == 0);
	CHECK_EQ((uint32_t)st.st_size, t_n64.bus.rdram_size);
	FILE *f = fopen(path, "rb");
	CHECK(f != NULL);
	if (f) {
		uint8_t w[4];
		CHECK(fseek(f, 0x1000, SEEK_SET) == 0 && fread(w, 1, 4, f) == 4);
		CHECK(w[0] == 0xCA && w[3] == 0xBE);  /* la RDRAM como la ve el juego (big-endian) */
		fclose(f);
	}
	snprintf(path, sizeof path, "%s/cuelgue-%u.txt", dir, field);
	f = fopen(path, "r");
	CHECK(f != NULL);
	if (f) {
		char text[16384];
		size_t n = fread(text, 1, sizeof text - 1, f);
		text[n] = '\0';
		CHECK(strstr(text, "MI: INTR") && strstr(text, "gpr:") && strstr(text, "r28"));
		fclose(f);
	}
	char cmd[200];
	snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
	CHECK(system(cmd) == 0);
}

void t_suite_cuelgue(void)
{
	RUN(cuelgue_avisa_una_vez);
	RUN(cuelgue_no_avisa_sin_motivo);
	RUN(cuelgue_avisa_siempre);
	RUN(cuelgue_texto);
	RUN(cuelgue_volcado);
}
