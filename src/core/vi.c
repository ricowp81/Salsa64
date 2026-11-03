/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — Video Interface (VI).
 *
 * Temporización por campos con dos eventos del planificador: la línea de
 * VI_V_INTR (levanta la interrupción del VI en el MI) y el fin del campo.
 * VI_V_CURRENT se calcula al leerlo a partir del ciclo actual. La duración
 * de un campo sale de los registros, como en la consola:
 *
 *   líneas  = (VI_V_SYNC + 1 + campo) / 2   (VI_V_SYNC cuenta medias líneas
 *                                            menos una: 525 progresivo NTSC,
 *                                            524 entrelazado)
 *   línea   = (VI_H_SYNC[11:0] + 1) ciclos del reloj del VI
 *   ciclos  = líneas × línea × 93,75 MHz / reloj del VI
 *
 * con el reloj del VI de cada tipo de TV (n64brew.dev: Video Interface):
 * NTSC 48,681812 MHz, PAL 49,656530 MHz, MPAL 48,628322 MHz. NTSC
 * progresivo da 1 567 041 ciclos por campo (59,83 Hz). Mientras el programa
 * no programa VI_V_SYNC ni VI_H_SYNC (o si VI_V_SYNC no llega a 256 medias
 * líneas) se usan los valores estándar del tipo de TV. Un cambio de esos registros vale desde el campo siguiente; uno de
 * VI_V_INTR, desde el campo en curso si la línea aún no pasó.
 *
 * Todo en enteros para que el host y la PS3 den los mismos ciclos.
 */
#include <string.h>

#include "core/bus.h"
#include "core/gamefix.h"
#include "core/sched.h"
#include "cpu/vr4300.h"

#define VR4300_HZ 93750000ull

static const struct {
	uint32_t clock_hz;
	uint32_t v_sync;  /* medias líneas por campo − 1, progresivo */
	uint32_t h_sync;  /* ciclos del VI por línea − 1 */
} tv_timing[] = {
	[SALSA64_TV_PAL]  = { 49656530, 625, 3177 },
	[SALSA64_TV_NTSC] = { 48681812, 525, 3093 },
	[SALSA64_TV_MPAL] = { 48628322, 525, 3090 },
};

static uint64_t now(const struct salsa64_bus *bus)
{
	return bus->cpu ? bus->cpu->cycles : 0;
}

static enum salsa64_tv_type tv_of(const struct salsa64_vi *vi)
{
	for (unsigned t = 0; t < sizeof tv_timing / sizeof tv_timing[0]; t++)
		if (tv_timing[t].clock_hz == vi->clock_hz)
			return (enum salsa64_tv_type)t;
	return SALSA64_TV_NTSC;
}

/* Líneas y duración del campo que empieza, según los registros actuales. */
static void field_timing(struct salsa64_vi *vi)
{
	enum salsa64_tv_type tv = tv_of(vi);
	uint32_t v_sync = vi->regs[SALSA64_VI_V_SYNC] & 0x3FF;
	uint32_t h_sync = vi->regs[SALSA64_VI_H_SYNC] & 0xFFF;
	/* Menos de 256 medias líneas no es un modo de TV: hacks hechos para
	 * emuladores (Super Mario Odyssey 64 deja 0x34) programan valores así
	 * y cuentan con campos de 60 Hz, como en Project64 o ares. */
	if (v_sync < 0x100)
		v_sync = tv_timing[tv].v_sync;
	if (h_sync == 0)
		h_sync = tv_timing[tv].h_sync;
	vi->lines = (v_sync + 1 + vi->field) / 2;
	vi->field_cycles = (uint64_t)vi->lines * (h_sync + 1) * VR4300_HZ / vi->clock_hz;
}

/* Ciclo en que el contador llega a la línea `line` del campo en curso. */
static uint64_t line_time(const struct salsa64_vi *vi, uint32_t line)
{
	return vi->field_start + ((uint64_t)line * vi->field_cycles + vi->lines - 1) / vi->lines;
}

/* Programa la interrupción del campo en curso si su línea no ha pasado. */
static void schedule_intr(struct salsa64_bus *bus)
{
	struct salsa64_vi *vi = &bus->vi;
	if (!vi->sched)
		return;
	uint32_t line = (vi->regs[SALSA64_VI_V_INTR] & 0x3FF) >> 1;
	uint64_t when = line_time(vi, line);
	if (line < vi->lines && when >= now(bus))
		salsa64_sched_set(vi->sched, SALSA64_EV_VI_INTR, when);
	else
		salsa64_sched_cancel(vi->sched, SALSA64_EV_VI_INTR);
}

static void begin_field(struct salsa64_bus *bus, uint64_t start)
{
	struct salsa64_vi *vi = &bus->vi;
	vi->field_start = start;
	field_timing(vi);
	if (vi->sched)
		salsa64_sched_set(vi->sched, SALSA64_EV_VI_FIELD, start + vi->field_cycles);
	schedule_intr(bus);
}

static void intr_event(void *ctx, uint64_t when)
{
	(void)when;
	salsa64_mi_raise(ctx, SALSA64_IRQ_VI);
}

static void field_event(void *ctx, uint64_t when)
{
	struct salsa64_bus *bus = ctx;
	struct salsa64_vi *vi = &bus->vi;
	vi->fields++;
	salsa64_gamefix_field(bus);
	vi->field = (vi->regs[SALSA64_VI_CONTROL] & SALSA64_VI_CONTROL_SERRATE) ? vi->field ^ 1 : 0;
	begin_field(bus, when);
}

void salsa64_vi_init(struct salsa64_bus *bus, struct salsa64_sched *sched)
{
	bus->vi.sched = sched;
	if (!bus->vi.clock_hz)
		bus->vi.clock_hz = tv_timing[SALSA64_TV_NTSC].clock_hz;
	salsa64_sched_handler(sched, SALSA64_EV_VI_INTR, intr_event, bus);
	salsa64_sched_handler(sched, SALSA64_EV_VI_FIELD, field_event, bus);
}

void salsa64_vi_start(struct salsa64_bus *bus)
{
	bus->vi.field = 0;
	bus->vi.fields = 0;
	begin_field(bus, now(bus));
}

void salsa64_vi_set_tv(struct salsa64_bus *bus, enum salsa64_tv_type tv)
{
	if ((unsigned)tv < sizeof tv_timing / sizeof tv_timing[0])
		bus->vi.clock_hz = tv_timing[tv].clock_hz;
}

uint64_t salsa64_vi_cycles_left(const struct salsa64_bus *bus)
{
	uint64_t end = bus->vi.field_start + bus->vi.field_cycles, t = now(bus);
	return end > t ? end - t : 1;
}

/* Media línea en curso; el bit 0 es el campo (entrelazado). */
static uint32_t v_current(const struct salsa64_bus *bus)
{
	const struct salsa64_vi *vi = &bus->vi;
	uint64_t elapsed = now(bus) - vi->field_start;
	uint64_t line = vi->field_cycles ? elapsed * vi->lines / vi->field_cycles : 0;
	if (line >= vi->lines)
		line = vi->lines - 1;
	return ((uint32_t)line << 1 | vi->field) & 0x3FF;
}

uint32_t salsa64_vi_read(struct salsa64_bus *bus, uint32_t reg)
{
	uint32_t i = reg >> 2;
	if (i >= SALSA64_VI_NREGS)
		return 0;
	if (i == SALSA64_VI_V_CURRENT)
		return v_current(bus);
	return bus->vi.regs[i];
}

void salsa64_vi_write(struct salsa64_bus *bus, uint32_t reg, uint32_t v)
{
	uint32_t i = reg >> 2;
	if (i >= SALSA64_VI_NREGS)
		return;
	if (i == SALSA64_VI_V_CURRENT) {
		salsa64_mi_clear(bus, SALSA64_IRQ_VI);
		return;
	}
	bus->vi.regs[i] = v;
	if (i == SALSA64_VI_V_INTR)
		schedule_intr(bus);
}

/*
 * Tamaño visible (n64brew.dev: Video Interface): VI_H_VIDEO y VI_V_VIDEO dan
 * el área activa en píxeles de salida (640 de ancho) y en medias líneas;
 * VI_X_SCALE y VI_Y_SCALE (2.10) dicen cuántos píxeles del framebuffer
 * avanza el VI por cada uno de salida. Ejemplo NTSC 320×240: 640 × 0,5 = 320
 * píxeles y 474 / 2 × 1,0 = 237 líneas. Por ahora no se respetan el
 * desplazamiento horizontal ni los filtros.
 */
bool salsa64_vi_frame(const struct salsa64_bus *bus, struct salsa64_vi_frame *f)
{
	const uint32_t *r = bus->vi.regs;
	memset(f, 0, sizeof *f);
	f->type = (enum salsa64_vi_type)(r[SALSA64_VI_CONTROL] & SALSA64_VI_CONTROL_TYPE);
	if (f->type != SALSA64_VI_RGBA5551 && f->type != SALSA64_VI_RGBA8888)
		return false;

	uint32_t h_start = (r[SALSA64_VI_H_VIDEO] >> 16) & 0x3FF, h_end = r[SALSA64_VI_H_VIDEO] & 0x3FF;
	uint32_t v_start = (r[SALSA64_VI_V_VIDEO] >> 16) & 0x3FF, v_end = r[SALSA64_VI_V_VIDEO] & 0x3FF;
	uint32_t x_scale = r[SALSA64_VI_X_SCALE] & 0xFFF, y_scale = r[SALSA64_VI_Y_SCALE] & 0xFFF;
	uint32_t pitch = r[SALSA64_VI_WIDTH] & 0xFFF;
	if (h_end <= h_start || v_end <= v_start || !x_scale || !y_scale || !pitch)
		return false;

	f->bpp = f->type == SALSA64_VI_RGBA8888 ? 4 : 2;
	f->stride = pitch * f->bpp;
	f->width = ((h_end - h_start) * x_scale + 0x200) >> 10;
	f->height = (((v_end - v_start) >> 1) * y_scale + 0x200) >> 10;
	if (f->width > pitch)
		f->width = pitch;
	f->origin = r[SALSA64_VI_ORIGIN] & 0xFFFFFF;

	/* Solo líneas completas dentro de la RDRAM. */
	if (f->origin >= bus->rdram_size || f->width == 0)
		return false;
	uint32_t room = bus->rdram_size - f->origin;
	if (room < f->width * f->bpp)
		return false;
	uint32_t max_lines = (room - f->width * f->bpp) / f->stride + 1;
	if (f->height > max_lines)
		f->height = max_lines;
	bool serrate = (r[SALSA64_VI_CONTROL] & SALSA64_VI_CONTROL_SERRATE) != 0;
	/* Un cuadro completo ya contiene las dos paridades: no desplazarlo de
	 * nuevo por campo. Lo es un búfer contiguo del que cada renglón del campo
	 * avanza más de una fila: 640 × 480 a Y_SCALE 2,0 (Indiana Jones, #39) o
	 * escalado entre 1 y 2 (FIFA 99, #69: 512 × 384 a 1,6 en los menús y
	 * 480 × 360 a 1,5 en el partido; el origen va a la fila 1 en los campos
	 * impares y a la 2 en los pares, V_VIDEO dos medias líneas después, y
	 * cada campo puede ser del otro búfer). Los modos que leen una fila de
	 * cada dos (paso doble) o una por renglón conservan la compensación
	 * anterior. */
	bool full = y_scale == 0x800 && f->height >= 400;
	bool scaled = !bus->vi.bob_scaled_frame && y_scale > 0x400 && y_scale < 0x800;
	f->weave = serrate && !bus->vi.bob_full_frame && (full || scaled) && pitch == f->width;
	f->vpos = serrate ? v_start + (f->weave ? 0 : bus->vi.field & 1) : 0;
	return f->height > 0;
}
