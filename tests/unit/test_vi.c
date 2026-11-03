/*
 * Salsa64 — pruebas del VI: contador de líneas, interrupción en VI_V_INTR,
 * duración de los campos y descripción del framebuffer.
 */
#include "test.h"

#define KSEG1(pa)     (0xFFFFFFFFA0000000ull | (pa))
#define VI_REG(r)     (0x04400000u + 4 * (r))
#define MI_INTR       0x04300008u
#define MI_INTR_MASK  0x0430000Cu
#define NTSC_FIELD    1567041u   /* 263 × 3094 × 93,75 MHz / 48,681812 MHz */
#define NTSC_LINES    263u
#define PAL_FIELD     1877989u   /* 313 × 3178 × 93,75 MHz / 49,656530 MHz */

static struct salsa64_bus *bus(void)
{
	return &t_n64.bus;
}

static uint32_t vi_rd(enum salsa64_vi_reg r)
{
	return salsa64_bus_read32(bus(), VI_REG(r));
}

static void vi_wr(enum salsa64_vi_reg r, uint32_t v)
{
	salsa64_bus_write32(bus(), VI_REG(r), v, 0xFFFFFFFFu);
}

static bool vi_irq(void)
{
	return (salsa64_bus_read32(bus(), MI_INTR) >> SALSA64_IRQ_VI) & 1;
}

/* El CPU da vueltas en `b .` mientras avanza el tiempo. */
static void spin(void)
{
	T_LOAD(BEQ(0, 0, 0xFFFF), NOP);
}

static void run(uint64_t cycles)
{
	salsa64_n64_run(&t_n64, cycles);
}

/* Ciclo, desde el inicio del campo, en que empieza la línea `line`. */
static uint64_t line_at(uint32_t line, uint64_t field, uint32_t lines)
{
	return (line * field + lines - 1) / lines;
}

static void field_length_ntsc(void)
{
	spin();
	CHECK_EQ(bus()->vi.lines, NTSC_LINES);
	CHECK_EQ(bus()->vi.field_cycles, NTSC_FIELD);
	CHECK_EQ(salsa64_n64_run_field(&t_n64), NTSC_FIELD);
	CHECK_EQ(bus()->vi.fields, 1);
	CHECK_EQ(salsa64_n64_run_field(&t_n64), NTSC_FIELD);
	CHECK_EQ(bus()->vi.fields, 2);
}

static void field_length_pal_and_registers(void)
{
	spin();
	salsa64_vi_set_tv(bus(), SALSA64_TV_PAL);
	salsa64_vi_start(bus());
	CHECK_EQ(bus()->vi.lines, 313);
	CHECK_EQ(salsa64_n64_run_field(&t_n64), PAL_FIELD);

	/* Registros NTSC programados: valen desde el campo siguiente. NTSC
	 * entrelazado (V_SYNC = 524): campos de 262 y 263 líneas. */
	salsa64_vi_set_tv(bus(), SALSA64_TV_NTSC);
	vi_wr(SALSA64_VI_V_SYNC, 524);
	vi_wr(SALSA64_VI_H_SYNC, 3093);
	vi_wr(SALSA64_VI_CONTROL, SALSA64_VI_CONTROL_SERRATE | SALSA64_VI_RGBA5551);
	CHECK_EQ(salsa64_n64_run_field(&t_n64), PAL_FIELD);
	CHECK_EQ(bus()->vi.lines, 263);
	CHECK_EQ(bus()->vi.field, 1);
	CHECK_EQ(salsa64_n64_run_field(&t_n64), NTSC_FIELD);
	CHECK_EQ(bus()->vi.lines, 262);
	CHECK_EQ(bus()->vi.field, 0);
}

/* Un VI_V_SYNC imposible (Super Mario Odyssey 64 deja 0x34) no acorta el
 * campo: se usa el estándar del tipo de TV, como en los emuladores para los
 * que se hizo el hack. */
static void v_sync_too_small_keeps_standard_field(void)
{
	spin();
	vi_wr(SALSA64_VI_V_SYNC, 0x34);
	CHECK_EQ(salsa64_n64_run_field(&t_n64), NTSC_FIELD);
	CHECK_EQ(salsa64_n64_run_field(&t_n64), NTSC_FIELD);
	CHECK_EQ(bus()->vi.lines, NTSC_LINES);
}

static void v_current_counts_half_lines(void)
{
	spin();
	CHECK_EQ(vi_rd(SALSA64_VI_V_CURRENT), 0);
	run(line_at(100, NTSC_FIELD, NTSC_LINES) + 10);
	CHECK_EQ(vi_rd(SALSA64_VI_V_CURRENT), 200);
	run(line_at(262, NTSC_FIELD, NTSC_LINES) - line_at(100, NTSC_FIELD, NTSC_LINES));
	CHECK_EQ(vi_rd(SALSA64_VI_V_CURRENT), 524);
	salsa64_n64_run_field(&t_n64);
	CHECK_EQ(vi_rd(SALSA64_VI_V_CURRENT), 0);
}

static void v_intr_raises_mi_and_v_current_clears(void)
{
	spin();
	CHECK_EQ(vi_rd(SALSA64_VI_V_INTR), SALSA64_VI_V_INTR_RESET);
	vi_wr(SALSA64_VI_V_INTR, 40);  /* línea 20 */
	uint64_t at = line_at(20, NTSC_FIELD, NTSC_LINES);
	run(at - 1);
	CHECK(!vi_irq());
	run(2);
	CHECK(vi_irq());
	CHECK_EQ(vi_rd(SALSA64_VI_V_CURRENT), 40);
	vi_wr(SALSA64_VI_V_CURRENT, 0);
	CHECK(!vi_irq());

	/* Una vez por campo. */
	salsa64_n64_run_field(&t_n64);
	CHECK(!vi_irq());
	run(at + 1);
	CHECK(vi_irq());
}

static void v_intr_reset_never_fires(void)
{
	spin();
	salsa64_n64_run_field(&t_n64);
	salsa64_n64_run_field(&t_n64);
	CHECK(!vi_irq());
	/* Una línea que ya pasó en este campo espera al siguiente. */
	run(line_at(50, NTSC_FIELD, NTSC_LINES));
	vi_wr(SALSA64_VI_V_INTR, 2);
	CHECK(!vi_irq());
	salsa64_n64_run_field(&t_n64);
	run(line_at(1, NTSC_FIELD, NTSC_LINES) + 1);
	CHECK(vi_irq());
}

/* La cadena completa: VI → MI → Cause.IP2 → excepción de interrupción. */
static void vi_interrupt_reaches_cpu(void)
{
	spin();
	salsa64_bus_write32(bus(), MI_INTR_MASK, 0x80, 0xFFFFFFFFu);  /* VI */
	t_set_status(T_STATUS | SALSA64_ST_IE | (1u << (8 + 2)));
	vi_wr(SALSA64_VI_V_INTR, 2);
	run(line_at(1, NTSC_FIELD, NTSC_LINES) + 2);
	CHECK_EQ(SALSA64_CAUSE_EXC(CPU->cause), SALSA64_EXC_INT);
	CHECK(CPU->cause & (1u << (8 + 2)));
	CHECK(CPU->status & SALSA64_ST_EXL);
	CHECK_EQ(CPU->pc & 0xFFFFFFFFu, (T_VEC_GEN & 0xFFFFFFFFu) + 4);
}

static void set_mode(uint32_t control, uint32_t origin, uint32_t width)
{
	/* NTSC 320×240 progresivo, los valores que usan las ROMs de prueba. */
	vi_wr(SALSA64_VI_CONTROL, control);
	vi_wr(SALSA64_VI_ORIGIN, origin);
	vi_wr(SALSA64_VI_WIDTH, width);
	vi_wr(SALSA64_VI_H_VIDEO, 0x006C02EC);
	vi_wr(SALSA64_VI_V_VIDEO, 0x002501FF);
	vi_wr(SALSA64_VI_X_SCALE, width * 1024 / 640);
	vi_wr(SALSA64_VI_Y_SCALE, 0x400);
}

static void frame_description(void)
{
	struct salsa64_vi_frame f;

	CHECK(!salsa64_vi_frame(bus(), &f));  /* apagado tras el reset */
	CHECK_EQ(f.type, SALSA64_VI_BLANK);

	set_mode(0x3202, 0x100000, 320);
	CHECK(salsa64_vi_frame(bus(), &f));
	CHECK_EQ(f.type, SALSA64_VI_RGBA5551);
	CHECK_EQ(f.origin, 0x100000);
	CHECK_EQ(f.width, 320);
	CHECK_EQ(f.height, 237);
	CHECK_EQ(f.bpp, 2);
	CHECK_EQ(f.stride, 640);

	/* osViBlack: libultra deja VI_H_VIDEO en 0 y el VI no muestra nada, aunque
	 * VI_CONTROL y VI_ORIGIN sigan como estaban (Star Fox 64 arranca así con
	 * el origen en la RAM baja, 0x280: la consola da negro). */
	set_mode(0x300E, 0x280, 320);
	vi_wr(SALSA64_VI_H_VIDEO, 0);
	CHECK(!salsa64_vi_frame(bus(), &f));
	CHECK_EQ(f.width, 0);
	vi_wr(SALSA64_VI_H_VIDEO, 0x006C02EC);
	vi_wr(SALSA64_VI_ORIGIN, 0x38FA80);
	CHECK(salsa64_vi_frame(bus(), &f));
	CHECK_EQ(f.origin, 0x38FA80);

	set_mode(0x3203, 0xA0200000u, 320);  /* los bits altos de ORIGIN no cuentan */
	CHECK(salsa64_vi_frame(bus(), &f));
	CHECK_EQ(f.type, SALSA64_VI_RGBA8888);
	CHECK_EQ(f.origin, 0x200000);
	CHECK_EQ(f.bpp, 4);
	CHECK_EQ(f.stride, 1280);

	set_mode(0x3202, 0, 640);  /* 640×480: Y_SCALE 2,0 */
	vi_wr(SALSA64_VI_Y_SCALE, 0x800);
	CHECK(salsa64_vi_frame(bus(), &f));
	CHECK_EQ(f.width, 640);
	CHECK_EQ(f.height, 474);

	/* Al final de la RDRAM: solo las líneas que caben enteras. */
	set_mode(0x3202, bus()->rdram_size - 640 * 10, 320);
	CHECK(salsa64_vi_frame(bus(), &f));
	CHECK_EQ(f.height, 10);
	set_mode(0x3202, bus()->rdram_size, 320);
	CHECK(!salsa64_vi_frame(bus(), &f));

	set_mode(0x3201, 0, 320);  /* formato reservado */
	CHECK(!salsa64_vi_frame(bus(), &f));
}

static void full_interlaced_frame(void)
{
	struct salsa64_vi_frame f;
	set_mode(0x3252, 0x100000, 640);
	vi_wr(SALSA64_VI_Y_SCALE, 0x02000800);
	for (uint32_t field = 0; field < 2; field++) {
		bus()->vi.field = field;
		CHECK(salsa64_vi_frame(bus(), &f));
		CHECK(f.weave);
		CHECK_EQ(f.vpos, 0x25);
		CHECK_EQ(f.height, 474);
		CHECK_EQ(f.origin, 0x100000);
		/* La presentación no altera la paridad que lee el juego. */
		CHECK_EQ(vi_rd(SALSA64_VI_V_CURRENT) & 1, field);
	}
	bus()->vi.bob_full_frame = true;
	CHECK(salsa64_vi_frame(bus(), &f));
	CHECK(!f.weave);
	CHECK_EQ(f.vpos, 0x26);
	bus()->vi.bob_full_frame = false;
	/* Un campo de 237 líneas y un paso doble (Racer/DK64) se conservan. */
	vi_wr(SALSA64_VI_WIDTH, 1280);
	vi_wr(SALSA64_VI_Y_SCALE, 0x400);
	CHECK(salsa64_vi_frame(bus(), &f));
	CHECK(!f.weave);
	CHECK_EQ(f.vpos, 0x26);
	CHECK_EQ(f.height, 237);
	CHECK_EQ(f.stride, 2560);
	/* Rogue usa 316 líneas de un búfer de 335: un cuadro completo escalado
	 * (#69); con vi-escalado-campos, por campos como antes. */
	vi_wr(SALSA64_VI_WIDTH, 640);
	vi_wr(SALSA64_VI_Y_SCALE, 0x555);
	CHECK(salsa64_vi_frame(bus(), &f));
	CHECK(f.weave);
	CHECK_EQ(f.vpos, 0x25);
	CHECK_EQ(f.height, 316);
	bus()->vi.bob_scaled_frame = true;
	CHECK(salsa64_vi_frame(bus(), &f));
	CHECK(!f.weave);
	CHECK_EQ(f.vpos, 0x26);
	bus()->vi.bob_scaled_frame = false;
	vi_wr(SALSA64_VI_CONTROL, 0x3202);
	vi_wr(SALSA64_VI_Y_SCALE, 0x800);
	CHECK(salsa64_vi_frame(bus(), &f));
	CHECK(!f.weave);
	CHECK_EQ(f.vpos, 0);
}

/* FIFA 99 (#69): 512 × 384 a Y_SCALE 1,6 (menús) y 480 × 360 a 1,5
 * (partido), los registros que deja el juego (los mismos en ares). En los
 * campos impares el origen está en la fila 1 y V_VIDEO empieza en 0x23; en
 * los pares, en la fila 2 y en 0x25. Es un cuadro completo: vpos sin el bit
 * de campo, como Indiana Jones; los de 240 renglones (Y_SCALE 1,0) no. */
static void scaled_interlaced_frame(void)
{
	static const struct { uint32_t width, x_scale, y_scale, base, height; } m[2] = {
		{ 512, 0x333, 0x02000666, 0x157640, 379 },
		{ 480, 0x300, 0x02000600, 0x1abcc0, 356 },
	};
	struct salsa64_vi_frame f;
	for (unsigned i = 0; i < 2; i++) {
		for (uint32_t field = 0; field < 2; field++) {
			set_mode(0x13056, m[i].base + m[i].width * 2 * (2 - field), m[i].width);
			vi_wr(SALSA64_VI_X_SCALE, m[i].x_scale);
			vi_wr(SALSA64_VI_Y_SCALE, m[i].y_scale);
			vi_wr(SALSA64_VI_V_VIDEO, field ? 0x002301FD : 0x002501FF);
			bus()->vi.field = field;
			CHECK(salsa64_vi_frame(bus(), &f));
			CHECK_EQ(f.width, m[i].width);
			CHECK_EQ(f.height, m[i].height);
			CHECK_EQ(f.stride, m[i].width * 2);
			CHECK(f.weave);
			CHECK_EQ(f.vpos, field ? 0x23 : 0x25);
			/* vi-escalado-campos: por campos, con el bit de campo. */
			bus()->vi.bob_scaled_frame = true;
			CHECK(salsa64_vi_frame(bus(), &f));
			CHECK(!f.weave);
			CHECK_EQ(f.vpos, field ? 0x24 : 0x25);
			bus()->vi.bob_scaled_frame = false;
			/* vi-campos apaga los dos. */
			bus()->vi.bob_full_frame = true;
			CHECK(salsa64_vi_frame(bus(), &f));
			CHECK(!f.weave);
			bus()->vi.bob_full_frame = false;
		}
	}
	/* 240 renglones con SERRATE (Y_SCALE 1,0) y sin SERRATE: igual que antes. */
	set_mode(0x13056, 0x100000, 320);
	CHECK(salsa64_vi_frame(bus(), &f));
	CHECK(!f.weave);
	set_mode(0x13016, 0x100000, 480);
	vi_wr(SALSA64_VI_X_SCALE, 0x300);
	vi_wr(SALSA64_VI_Y_SCALE, 0x02000600);
	CHECK(salsa64_vi_frame(bus(), &f));
	CHECK(!f.weave);
	CHECK_EQ(f.vpos, 0);
	/* Más de dos filas por renglón no es un cuadro: tampoco. */
	set_mode(0x13056, 0x100000, 480);
	vi_wr(SALSA64_VI_Y_SCALE, 0xA00);
	CHECK(salsa64_vi_frame(bus(), &f));
	CHECK(!f.weave);
	bus()->vi.field = 0;
}

void t_suite_vi(void)
{
	RUN(field_length_ntsc);
	RUN(field_length_pal_and_registers);
	RUN(v_sync_too_small_keeps_standard_field);
	RUN(v_current_counts_half_lines);
	RUN(v_intr_raises_mi_and_v_current_clears);
	RUN(v_intr_reset_never_fires);
	RUN(vi_interrupt_reaches_cpu);
	RUN(frame_description);
	RUN(full_interlaced_frame);
	RUN(scaled_interlaced_frame);
}
