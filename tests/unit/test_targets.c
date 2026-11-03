/*
 * Salsa64 — pruebas de la elección del destino de render que se presenta
 * (src/rdp/targets.c): dos framebuffers alternos en direcciones distintas
 * (que se solapan en la cuenta) o en la misma dirección con otro ancho, una
 * color image de otro ancho sobre el framebuffer, el VI sostenido sin
 * dibujar y el reciclado de ranuras.
 */
#include "test.h"

#include <string.h>

#include "rdp/targets.h"

#define SIZ_16b 2u

static struct rdp_targets s;

static struct salsa64_vi_frame frame(uint32_t origin, uint32_t width, uint32_t height)
{
	struct salsa64_vi_frame f;
	memset(&f, 0, sizeof f);
	f.type = SALSA64_VI_RGBA5551;
	f.origin = origin;
	f.width = width;
	f.height = height;
	f.stride = width * 2;
	f.bpp = 2;
	return f;
}

/* Una tarea que dibuja en la color image (addr, width). */
static int draw(uint32_t addr, uint32_t width)
{
	bool fresh;
	int i = rdp_targets_get(&s, addr, width, SIZ_16b, 0, &fresh);
	rdp_targets_drawn(&s, i);
	return i;
}

static struct rdp_present present(uint32_t field, uint32_t origin, uint32_t width, uint32_t height)
{
	struct salsa64_vi_frame f = frame(origin, width, height);
	struct rdp_present p;
	rdp_targets_present(&s, &f, field, &p);
	return p;
}

/* Banjo-Kazooie: dos framebuffers de 292 × 216 (584 × 216 bytes aparte,
 * menos que las 219 líneas que se le suponen a uno de 292), VI en la
 * línea 1 de cada uno, alternando cada 2 campos; el RDP dibuja el otro
 * mientras se muestra uno. Debe presentarse el que empieza más cerca del
 * origen, no el dibujado hace menos (que lo cubre con sus últimas líneas). */
static void targets_two_buffers_overlapping(void)
{
	const uint32_t A = 0x3a5d00, B = 0x3c49c0, LINE = 292 * 2;
	rdp_targets_init(&s);
	int a = draw(A, 292);
	struct rdp_present p = present(1, A + LINE, 292, 213);
	CHECK_EQ(p.slot, a);
	CHECK_EQ(p.y0, 1);
	CHECK_EQ(p.w, 292);
	CHECK_EQ(p.h, 213);
	/* Mientras se muestra A se dibuja B; luego el VI pasa a B. */
	int b = draw(B, 292);
	CHECK(b != a);
	p = present(2, A + LINE, 292, 213);
	CHECK_EQ(p.slot, a);
	CHECK_EQ(p.age, 1);  /* A se dibujó en el campo 1 */
	p = present(3, B + LINE, 292, 213);
	CHECK_EQ(p.slot, b);
	CHECK_EQ(p.y0, 1);
	CHECK_EQ(p.age, 1);  /* B, dibujado antes de presentar el campo 2 */
	/* Mientras se muestra B se dibuja A otra vez: A cubre el origen de B
	 * (línea 217 de sus 219) y es más reciente, pero B empieza más cerca. */
	CHECK_EQ(draw(A, 292), a);
	p = present(4, B + LINE, 292, 213);
	CHECK_EQ(p.slot, b);
	CHECK_EQ(p.h, 213);
	p = present(5, A + LINE, 292, 213);
	CHECK_EQ(p.slot, a);
	CHECK_EQ(s.created, 2);
}

/* GoldenEye: la misma dirección con un framebuffer de 320 y uno de 440
 * (cambio de resolución); se presenta el del ancho del VI. */
static void targets_same_addr_two_widths(void)
{
	const uint32_t A = 0x3b5000;
	rdp_targets_init(&s);
	int w320 = draw(A, 320);
	struct rdp_present p = present(1, A + 640, 320, 237);
	CHECK_EQ(p.slot, w320);
	int w440 = draw(A, 440);
	CHECK(w440 != w320);
	p = present(2, A + 880, 440, 326);
	CHECK_EQ(p.slot, w440);
	CHECK_EQ(p.w, 440);
	/* El de 320 sigue reciente y cubre el origen, pero el VI muestra 440. */
	CHECK_EQ(draw(A, 320), w320);
	p = present(3, A + 880, 440, 326);
	CHECK_EQ(p.slot, w440);
	p = present(4, A + 640, 320, 237);
	CHECK_EQ(p.slot, w320);
}

/* Zelda en pausa: cada tarea pone dos color images de 64 de ancho (una en
 * la dirección del framebuffer) y después el framebuffer de 320; el VI
 * alterna dos framebuffers. Se presenta siempre el de 320 y ninguna de
 * las de 64 lo desplaza de su ranura. */
static void targets_aux_image_on_framebuffer(void)
{
	const uint32_t A = 0x3b5000, B = 0x3da800, X = 0x3b8800, Y = 0x3de000;
	rdp_targets_init(&s);
	int a = draw(A, 320), b = draw(B, 320);
	struct rdp_present p = present(1, A + 0x280, 320, 237);
	CHECK_EQ(p.slot, a);
	for (uint32_t field = 2; field < 40; field++) {
		bool even = (field & 1) == 0;
		draw(even ? Y : X, 64);
		draw(even ? B : A, 64);
		CHECK_EQ(draw(even ? B : A, 320), even ? b : a);
		/* El VI muestra el otro framebuffer (el dibujado el campo anterior). */
		p = present(field, (even ? A : B) + 0x280, 320, 237);
		CHECK_EQ(p.slot, even ? a : b);
		CHECK_EQ(p.y0, 1);
		CHECK_EQ(p.w, 320);
		CHECK_EQ(p.h, 237);
	}
	CHECK_EQ(s.created, 6);
}

/* El juego deja de dibujar y el VI sostiene el mismo origen (GoldenEye
 * mientras carga): se sigue presentando el destino, más allá de los
 * campos en que pasaría por viejo; al cambiar el origen, ya no. */
static void targets_held_frame(void)
{
	const uint32_t A = 0x3b5000;
	rdp_targets_init(&s);
	int a = draw(A, 440);
	struct rdp_present p = present(1, A + 880, 440, 326);
	CHECK_EQ(p.slot, a);
	for (uint32_t field = 2; field < 2 + 3 * RDP_TARGETS_STALE; field++) {
		p = present(field, A + 880, 440, 326);
		CHECK_EQ(p.slot, a);
	}
	p = present(100, A + 880 + 0x40000, 440, 326);
	CHECK_EQ(p.slot, -1);
	/* Vuelve al origen de antes, pero ya se perdió el hilo: la RDRAM. */
	p = present(101, A + 880, 440, 326);
	CHECK_EQ(p.slot, -1);
}

/* SM64 apaga el VI 26 campos al cambiar de escena y sigue dibujando: lo
 * dibujado entonces cuenta como del campo en que vuelve la imagen. */
static void targets_drawn_while_blank(void)
{
	const uint32_t A = 0x3b5000;
	rdp_targets_init(&s);
	int a = draw(A, 320);
	struct rdp_present p = present(10, A, 320, 240);
	CHECK_EQ(p.slot, a);
	draw(A, 320);  /* en negro durante 60 campos: sin presentar */
	p = present(70, A, 320, 240);
	CHECK_EQ(p.slot, a);
	/* Sin dibujar nada en 60 campos y con otro origen entre medio: viejo. */
	p = present(100, A + 0x100000, 320, 240);
	CHECK_EQ(p.slot, -1);
	p = present(131, A, 320, 240);
	CHECK_EQ(p.slot, -1);
}

/* Reciclado: con las ranuras llenas no se toma la presentada ni, mientras
 * haya otras, un framebuffer del ancho del VI; primero las viejas, luego
 * las imágenes auxiliares. */
static void targets_recycle_order(void)
{
	const uint32_t A = 0x3b5000, B = 0x3da800;
	rdp_targets_init(&s);
	int a = draw(A, 320), b = draw(B, 320);
	present(1, A + 0x280, 320, 237);
	/* Llenar el resto con imágenes auxiliares de 64, una por campo. */
	for (int i = 2; i < RDP_TARGETS_MAX; i++) {
		draw(0x100000 + 0x2000 * i, 64);
		present(i, A + 0x280, 320, 237);
	}
	CHECK_EQ(s.created, RDP_TARGETS_MAX);
	/* Una más: sale la auxiliar más vieja (la del campo 2). */
	bool fresh;
	int n = rdp_targets_get(&s, 0x200000, 64, SIZ_16b, 0, &fresh);
	CHECK(fresh);
	CHECK(n != a && n != b);
	CHECK_EQ(s.t[n].addr, 0x200000);
	/* Los framebuffers siguen. */
	CHECK_EQ(draw(A, 320), a);
	CHECK_EQ(draw(B, 320), b);
	CHECK_EQ(s.created, RDP_TARGETS_MAX + 1);
	/* Si una ranura envejece (sin dibujar RDP_TARGETS_STALE campos), es la
	 * primera en irse aunque sea un framebuffer: B no se dibuja más. */
	for (uint32_t field = RDP_TARGETS_MAX; field < RDP_TARGETS_MAX + RDP_TARGETS_STALE + 2; field++) {
		draw(A, 320);
		for (int i = 2; i < RDP_TARGETS_MAX; i++)
			draw(0x100000 + 0x2000 * i, 64);
		draw(0x200000, 64);
		present(field, A + 0x280, 320, 237);
	}
	n = rdp_targets_get(&s, 0x300000, 64, SIZ_16b, 0, &fresh);
	CHECK(fresh);
	CHECK_EQ(n, b);
}

/* Conker: a mitad del cuadro dibuja en una color image CI8 de 64 de ancho
 * con tijeras hasta la línea 65 (es de 64 × 64): el destino nace con ese
 * alto y no con 48; si una tarea posterior llega más abajo, crece (se
 * vuelve a crear). El framebuffer de 292 × 216 no crece con 216. */
static void targets_height_from_scissor(void)
{
	const uint32_t A = 0x3d6300, X = 0xde080;
	rdp_targets_init(&s);
	bool fresh;
	int a = rdp_targets_get(&s, A, 292, SIZ_16b, 216, &fresh);
	CHECK(fresh);
	CHECK_EQ(s.t[a].lh, 219);
	int x = rdp_targets_get(&s, X, 64, 1u, 65, &fresh);
	CHECK(fresh);
	CHECK_EQ(s.t[x].lw, 64);
	CHECK_EQ(s.t[x].lh, 65);
	CHECK_EQ(s.t[x].bpp, 1);
	CHECK_EQ(rdp_targets_get(&s, X, 64, 1u, 60, &fresh), x);
	CHECK(!fresh);
	CHECK_EQ(s.t[x].lh, 65);
	CHECK_EQ(rdp_targets_get(&s, X, 64, 1u, 128, &fresh), x);
	CHECK(fresh);
	CHECK_EQ(s.t[x].lh, 128);
	CHECK_EQ(rdp_targets_get(&s, A, 292, SIZ_16b, 0, &fresh), a);
	CHECK(!fresh);
	CHECK_EQ(s.created, 3);
	/* La presentación usa el alto real: el VI muestra 213 líneas de A. */
	rdp_targets_drawn(&s, a);
	struct rdp_present p = present(1, A + 584, 292, 213);
	CHECK_EQ(p.slot, a);
	CHECK_EQ(p.h, 213);
}

/* Origen de una textura (gfx_tex.src_*) dentro de un destino: entre dos
 * framebuffers que se solapan en la cuenta (Banjo) gana el que empieza más
 * cerca; el paso y los bytes por texel deben coincidir; una dirección fuera
 * de todos da -1. */
static void targets_locate(void)
{
	const uint32_t A = 0x3a5d00, B = 0x3c49c0, LINE = 292 * 2;
	rdp_targets_init(&s);
	int a = draw(A, 292), b = draw(B, 292);
	uint32_t x0 = 99, y0 = 99;
	/* Las primeras filas de B caen también en las últimas de A. */
	CHECK_EQ(rdp_targets_locate(&s, B + LINE, 292, 2, &x0, &y0), b);
	CHECK_EQ(x0, 0);
	CHECK_EQ(y0, 1);
	/* Dentro de A, a media fila. */
	CHECK_EQ(rdp_targets_locate(&s, A + 10 * LINE + 2 * 7, 292, 2, &x0, &y0), a);
	CHECK_EQ(x0, 7);
	CHECK_EQ(y0, 10);
	/* Otro paso u otro tamaño de texel: no es este destino. */
	CHECK_EQ(rdp_targets_locate(&s, A, 320, 2, &x0, &y0), -1);
	CHECK_EQ(rdp_targets_locate(&s, A, 292, 4, &x0, &y0), -1);
	/* Fuera de todos. */
	CHECK_EQ(rdp_targets_locate(&s, A - 2, 292, 2, &x0, &y0), -1);
	CHECK_EQ(rdp_targets_locate(&s, B + LINE * 219, 292, 2, &x0, &y0), -1);
	/* Una imagen auxiliar de 64 sobre el framebuffer de Zelda (misma
	 * dirección): el paso decide. */
	const uint32_t Z = 0x3b5000;
	rdp_targets_init(&s);
	int z = draw(Z, 320), w = draw(Z, 64);
	CHECK_EQ(rdp_targets_locate(&s, Z + 128 * 3, 64, 2, &x0, &y0), w);
	CHECK_EQ(y0, 3);
	CHECK_EQ(rdp_targets_locate(&s, Z + 640 * 3, 320, 2, &x0, &y0), z);
	CHECK_EQ(y0, 3);
}

/* Una textura que la HLE dejó para leer de un destino y que hay que
 * decodificar de la RDRAM (rdp_decode_rgba16): RGBA16 big-endian de un
 * framebuffer de 4 de ancho, dos filas desde la columna 1, a RGBA8888 con
 * 5 bits extendidos a 8 y el bit de alfa a 0 o 255; lo que cae fuera de la
 * RDRAM queda en cero. */
static void targets_decode_rgba16(void)
{
	uint8_t ram[32];
	for (int i = 0; i < 16; i++) {
		/* r = i, g = 31 − i, b = 2i, alfa en los impares. */
		uint32_t p = (uint32_t)i << 11 | (31u - i) << 6 | (2u * i & 31) << 1 | (i & 1);
		ram[2 * i] = (uint8_t)(p >> 8);
		ram[2 * i + 1] = (uint8_t)p;
	}
	uint8_t out[2][16];
	memset(out, 0xAA, sizeof out);
	rdp_decode_rgba16(ram, sizeof ram, 2, 4, 3, 2, out[0], sizeof out[0]);
	unsigned bad = 0;
	for (uint32_t y = 0; y < 2; y++)
		for (uint32_t x = 0; x < 3; x++) {
			uint32_t i = y * 4 + 1 + x, r = i, g = 31 - i, b = 2 * i & 31;
			const uint8_t *o = &out[y][4 * x];
			bad += o[0] != (r << 3 | r >> 2) || o[1] != (g << 3 | g >> 2) || o[2] != (b << 3 | b >> 2) ||
			       o[3] != ((i & 1) ? 0xFF : 0);
		}
	CHECK_EQ(bad, 0);
	CHECK_EQ(out[0][12], 0xAA);  /* el cuarto texel no se escribe */
	/* Desde el texel 14: el 14 y el 15 están, el 16 cae fuera de la RDRAM. */
	rdp_decode_rgba16(ram, sizeof ram, 2 * 14, 4, 3, 1, out[0], sizeof out[0]);
	CHECK(out[0][4] != 0 || out[0][5] != 0);
	for (int k = 8; k < 12; k++)
		CHECK_EQ(out[0][k], 0);
}

/* De dónde se lee una textura que viene de un destino (docs/framebuffer.md):
 * del RSX si lo dibujó una tarea reciente y no es el ligado ni se copió a
 * la RDRAM; si no, de la RDRAM. La pausa de Zelda: el framebuffer dibujado
 * dos tareas antes se lee del RSX al copiarlo a fbufSave; fbufSave se copia
 * a la RDRAM (el CPU lo filtra) y desde entonces se lee de la RDRAM; una
 * imagen de 64 recién dibujada se lee del RSX, salvo sobre sí misma. */
static void targets_texture_policy(void)
{
	const uint32_t FB = 0x3da800, SAVE = 0x12c700, ICON = 0x3da800;
	rdp_targets_init(&s);
	s.vi_width = 320;
	int slot;
	uint32_t x0, y0;
	rdp_targets_task(&s);  /* tarea 1: el cuadro, que el VI muestra */
	int fb = draw(FB, 320);
	present(1, FB + 640, 320, 237);
	rdp_targets_task(&s);  /* tarea 2: otro framebuffer */
	draw(0x3b5000, 320);
	rdp_targets_task(&s);  /* tarea 3: la pausa copia FB → SAVE */
	int save = draw(SAVE, 320);
	CHECK_EQ(rdp_targets_texture(&s, FB + 640 * 10, 320, 2, save, &slot, &x0, &y0), RDP_TEXSRC_RSX);
	CHECK_EQ(slot, fb);
	CHECK_EQ(y0, 10);
	/* Sobre sí mismo (el oscurecido y VISCVG): de una copia del ligado. */
	CHECK_EQ(rdp_targets_texture(&s, FB, 320, 2, fb, &slot, &x0, &y0), RDP_TEXSRC_SELF);
	CHECK_EQ(slot, fb);
	/* Al terminar la tarea, SAVE se copia a la RDRAM: es del CPU. */
	CHECK(rdp_targets_readback(&s, save, false, true));
	CHECK(s.t[save].cpu_owned);
	CHECK(!rdp_targets_readback(&s, fb, false, true));  /* ya presentado: no */
	rdp_targets_task(&s);  /* tarea 4: restaurar desde SAVE */
	CHECK_EQ(rdp_targets_texture(&s, SAVE + 640 * 6, 320, 2, fb, &slot, &x0, &y0), RDP_TEXSRC_RDRAM);
	CHECK_EQ(slot, save);
	/* Si el RDP vuelve a dibujar SAVE, vuelve a valer el RSX. */
	draw(SAVE, 320);
	CHECK(!s.t[save].cpu_owned);
	CHECK_EQ(rdp_targets_texture(&s, SAVE, 320, 2, fb, &slot, &x0, &y0), RDP_TEXSRC_RSX);
	/* Icono de 64 sobre el framebuffer, dibujado en esta tarea. */
	int icon = draw(ICON, 64);
	CHECK_EQ(rdp_targets_texture(&s, ICON + 128 * 2, 64, 2, fb, &slot, &x0, &y0), RDP_TEXSRC_RSX);
	CHECK_EQ(slot, icon);
	CHECK_EQ(y0, 2);
	CHECK(!rdp_targets_readback(&s, icon, false, true));  /* chico: se queda en el RSX */
	/* Viejo: tras RDP_TARGETS_FRESH tareas sin dibujarlo, de la RDRAM. */
	for (int k = 0; k <= RDP_TARGETS_FRESH; k++)
		rdp_targets_task(&s);
	CHECK_EQ(rdp_targets_texture(&s, ICON, 64, 2, fb, &slot, &x0, &y0), RDP_TEXSRC_RDRAM);
	CHECK_EQ(s.tex_rsx, 3);
	CHECK_EQ(s.tex_rdram, 2);
	CHECK_EQ(s.tex_self, 1);
	CHECK_EQ(s.readbacks, 1);
	/* Sin VI presentado no se sabe qué es framebuffer: con force se copia. */
	CHECK(rdp_targets_readback(&s, fb, true, true));
}

/* La misma memoria con otro tamaño de texel. Majora's Mask: la pausa usa
 * 784600/320 como cobertura de 8 bits; cientos de tareas después, las
 * campanadas del fin del día copian el cuadro ahí en 16 bits y lo leen
 * como textura de 16: tiene que ser otra imagen (de 16 bits, que las
 * texturas encuentren), no la ranura de 8. Zelda OoT, en cambio, pone su
 * framebuffer de 16 bits como color image de 8 en la misma tarea en que lo
 * dibujó (la cobertura para el filtro de la pausa): sigue siendo la misma
 * ranura, de 16 bits. */
static void targets_same_memory_other_size(void)
{
	const uint32_t W = 0x784600, FB = 0x3da800;
	bool fresh;
	uint32_t x0, y0;
	int slot;
	rdp_targets_init(&s);
	rdp_targets_task(&s);
	int w = rdp_targets_get(&s, W, 320, 1u, 0, &fresh);
	CHECK(fresh);
	rdp_targets_drawn(&s, w);
	CHECK_EQ(s.t[w].bpp, 1);
	for (int k = 0; k < 100; k++)
		rdp_targets_task(&s);
	CHECK_EQ(rdp_targets_get(&s, W, 320, SIZ_16b, 0, &fresh), w);
	CHECK(fresh);  /* otra imagen: la superficie se vuelve a crear */
	CHECK_EQ(s.t[w].bpp, 2);
	CHECK(!s.t[w].cpu_owned);
	rdp_targets_drawn(&s, w);
	CHECK_EQ(rdp_targets_texture(&s, W + 640 * 6, 320, 2, -1, &slot, &x0, &y0), RDP_TEXSRC_RSX);
	CHECK_EQ(slot, w);
	CHECK_EQ(y0, 6);
	CHECK_EQ(s.created, 2);

	rdp_targets_init(&s);
	rdp_targets_task(&s);
	int fb = draw(FB, 320);
	CHECK_EQ(rdp_targets_get(&s, FB, 320, 1u, 0, &fresh), fb);
	CHECK(!fresh);
	CHECK_EQ(s.t[fb].bpp, 2);
	rdp_targets_drawn(&s, fb);
	/* Las franjas que lo copian sobre sí mismo lo siguen encontrando. */
	CHECK_EQ(rdp_targets_texture(&s, FB, 320, 2, fb, &slot, &x0, &y0), RDP_TEXSRC_SELF);
	CHECK_EQ(s.created, 1);
}

/* El acumulador del desenfoque de movimiento de Majora's Mask (3a92c0/320):
 * cada tarea lo dibuja (el cuadro mezclado con el anterior) y la siguiente
 * lo lee como textura. La primera copia a la RDRAM se espera y pasa al CPU;
 * las siguientes, sin esperar, no: el RDP lo sigue leyendo del RSX. */
static void targets_repeated_copy_stays_rsx(void)
{
	const uint32_t M = 0x3a92c0, FB = 0x7da800;
	uint32_t x0, y0;
	int slot;
	rdp_targets_init(&s);
	s.vi_width = 320;
	rdp_targets_task(&s);
	int fb = draw(FB, 320);
	int m = draw(M, 320);
	CHECK(rdp_targets_readback(&s, m, true, true));  /* apareció: esperada */
	CHECK(s.t[m].cpu_owned);
	rdp_targets_task(&s);
	CHECK_EQ(rdp_targets_texture(&s, M, 320, 2, fb, &slot, &x0, &y0), RDP_TEXSRC_RDRAM);
	draw(FB, 320);
	draw(M, 320);
	CHECK(rdp_targets_readback(&s, m, true, false));  /* repetida: sin esperar */
	CHECK(!s.t[m].cpu_owned);
	rdp_targets_task(&s);
	CHECK_EQ(rdp_targets_texture(&s, M, 320, 2, fb, &slot, &x0, &y0), RDP_TEXSRC_RSX);
	CHECK_EQ(slot, m);
	CHECK_EQ(s.readbacks, 2);
	/* Cuando el efecto termina y envejece, vale la última copia. */
	for (int k = 0; k <= RDP_TARGETS_FRESH; k++)
		rdp_targets_task(&s);
	CHECK_EQ(rdp_targets_texture(&s, M, 320, 2, fb, &slot, &x0, &y0), RDP_TEXSRC_RDRAM);
}

/* Lo que decide el hilo de emulación con los orígenes del VI: una color
 * image de 16 bits del ancho del VI que no es un framebuffer reciente se
 * copia (fbufSave de Zelda en el z-buffer); los framebuffers (también el
 * que empieza unas líneas antes del origen), las imágenes chicas y las de
 * otro tamaño de texel, no. */
static void targets_readback_wanted(void)
{
	struct rdp_vi_ring r;
	memset(&r, 0, sizeof r);
	CHECK(!rdp_readback_wanted(&r, 0x12c700, 320, 2));  /* sin VI aún: nada */
	rdp_vi_ring_push(&r, 0x3b5000 + 640, 320);
	rdp_vi_ring_push(&r, 0x3da800 + 640, 320);
	rdp_vi_ring_push(&r, 0x3b5000 + 640, 320);  /* repetido: no ocupa */
	CHECK_EQ(r.n, 2);
	CHECK(rdp_readback_wanted(&r, 0x12c700, 320, 2));
	CHECK(!rdp_readback_wanted(&r, 0x3b5000, 320, 2));
	CHECK(!rdp_readback_wanted(&r, 0x3da800, 320, 2));
	CHECK(!rdp_readback_wanted(&r, 0x3da800 + 640 * 100, 320, 2));
	CHECK(!rdp_readback_wanted(&r, 0x12c700, 64, 2));   /* chica */
	CHECK(!rdp_readback_wanted(&r, 0x12c700, 320, 1));  /* CI8 */
	CHECK(!rdp_readback_wanted(&r, 0x12c700, 320, 3));  /* 32 bits */
	/* Un ancho del VI nuevo (GoldenEye pasa a 440): manda el último. */
	rdp_vi_ring_push(&r, 0x200000, 440);
	CHECK(!rdp_readback_wanted(&r, 0x12c700, 320, 2));
	CHECK(rdp_readback_wanted(&r, 0x12c700, 440, 2));
	/* Y al volver a un framebuffer que ya estaba (la presa de GoldenEye, a
	 * 320 tras las pantallas de la misión a 440): no se vuelve a agregar,
	 * pero el ancho del VI es el del campo de ahora. Con el del último
	 * agregado (440), cada cuadro de 320 parecía una imagen auxiliar y la
	 * PS3 lo copiaba a la RDRAM esperando. */
	rdp_vi_ring_push(&r, 0x3da800 + 640, 320);
	CHECK_EQ(r.n, 3);
	CHECK_EQ(r.cur_width, 320);
	CHECK(!rdp_readback_wanted(&r, 0x3da800, 320, 2));
	CHECK(!rdp_readback_wanted(&r, 0x3b5000, 320, 2));
	CHECK(rdp_readback_wanted(&r, 0x12c700, 320, 2));
	CHECK(!rdp_readback_wanted(&r, 0x12c700, 440, 2));
	/* El anillo da la vuelta sin perder los últimos. */
	for (uint32_t k = 0; k < RDP_VI_RING + 2; k++)
		rdp_vi_ring_push(&r, 0x100000 + 0x40000 * k, 440);
	CHECK(!rdp_readback_wanted(&r, 0x100000 + 0x40000 * (RDP_VI_RING + 1), 440, 2));
	CHECK(rdp_readback_wanted(&r, 0x12c700, 440, 2));
}

/* Escala efectiva: automática por altura de pantalla, pedida (con tope en
 * 5) y reducida cuando el peor caso no cabe en el presupuesto de memoria
 * local del RSX. */
static void targets_scale_pick(void)
{
	bool cut = true;
	CHECK_EQ(rdp_scale_pick(0, 720, &cut), 3u);
	CHECK(!cut);
	CHECK_EQ(rdp_scale_pick(0, 1080, &cut), 4u);
	CHECK(!cut);
	CHECK_EQ(rdp_scale_pick(0, 480, &cut), 2u);
	CHECK_EQ(rdp_scale_pick(0, 100, NULL), 1u);
	for (uint32_t s = 1; s <= 5; s++) {
		CHECK_EQ(rdp_scale_pick(s, 720, &cut), s);
		CHECK(!cut);
	}
	CHECK_EQ(rdp_scale_pick(9, 720, &cut), 5u);  /* el tope no es por memoria */
	CHECK(!cut);
	/* Peor caso: 8 destinos, el scratch, 4 profundidades y la profundidad
	 * de la pantalla agrandada (14 superficies) de 320 × 240 × 25 (7,68 MB
	 * c/u) = 102,5 MiB, dentro del tope de 112 MiB (con 96, 5× no entraba). */
	CHECK_EQ(RDP_SURFACES_MAX, 14);
	CHECK_EQ(RDP_VRAM_BUDGET, 112u << 20);
	CHECK_EQ(rdp_vram_worst(5), 14u * 1600 * 4 * 1200);
	CHECK(rdp_vram_worst(5) <= RDP_VRAM_BUDGET);
	CHECK(rdp_vram_worst(5) > 96u << 20);
	CHECK(RDP_VRAM_BUDGET - rdp_vram_worst(5) > 9u << 20);  /* margen */
	CHECK_EQ(rdp_vram_worst(1), 14u * 640 * 4 * 480);  /* a 1× una de 640 × 480 queda nativa */
	CHECK_EQ(rdp_surface_bytes(1600, 1200), 1600u * 4 * 1200);
	CHECK_EQ(rdp_surface_bytes(321, 1), 1408u);  /* pitch a 128 */
	/* Con un presupuesto más chico baja la escala y avisa. */
	CHECK_EQ(rdp_scale_pick_budget(5, 720, 70u << 20, &cut), 4u);
	CHECK(cut);
	CHECK_EQ(rdp_scale_pick_budget(5, 720, 60u << 20, &cut), 3u);
	CHECK(cut);
	CHECK_EQ(rdp_scale_pick_budget(0, 1080, 40u << 20, &cut), 3u);
	CHECK(cut);
	CHECK_EQ(rdp_scale_pick_budget(5, 720, 1, &cut), 1u);
	CHECK(cut);
	CHECK_EQ(rdp_scale_pick_budget(3, 720, 100u << 20, &cut), 3u);
	CHECK(!cut);
	/* Escala por destino: el objetivo son 240 × escala líneas. */
	CHECK_EQ(rdp_target_scale(5, 320, 240), 5u);
	CHECK_EQ(rdp_target_scale(5, 640, 480), 2u);
	CHECK_EQ(rdp_target_scale(5, 64, 48), 5u);     /* auxiliar chica, con tope */
	CHECK_EQ(rdp_target_scale(5, 400, 300), 4u);
	CHECK_EQ(rdp_target_scale(3, 640, 480), 1u);
	CHECK_EQ(rdp_target_scale(3, 320, 240), 3u);
	CHECK_EQ(rdp_target_scale(3, 64, 112), 5u);    /* Link en la pausa de Zelda */
	CHECK_EQ(rdp_target_scale(1, 320, 240), 1u);
	CHECK_EQ(rdp_target_scale(5, 640, 2000), 1u);  /* nunca 0 */
	CHECK_EQ(rdp_target_scale(5, 5000, 100), 1u);  /* límite de 4096 */
	/* Ninguna superficie pasa de 320·g × 240·g (o 640 × 480 a 1×) en ningún
	 * lado, sea cual sea su tamaño lógico (alto ≥ 3/4 del ancho, como da
	 * rdp_targets_get, hasta 640 × 480): ni un destino, ni el scratch (su
	 * copia), ni una profundidad (la de un destino, o la mayor de dos en
	 * cada lado). Así RDP_SURFACES_MAX caben en rdp_vram_worst. */
	for (uint32_t g = 1; g <= RDP_SCALE_MAX; g++) {
		uint32_t mw = 320 * g > 640 ? 320 * g : 640, mh = 240 * g > 480 ? 240 * g : 480;
		for (uint32_t w = 8; w <= 640; w += 8)
			for (uint32_t lh = w * 3 / 4; lh <= 480; lh += 7) {
				uint32_t sc = rdp_target_scale(g, w, lh);
				CHECK(w * sc <= mw && lh * sc <= mh);
				CHECK(rdp_surface_bytes(w * sc, lh * sc) * RDP_SURFACES_MAX <= rdp_vram_worst(g));
			}
	}
	CHECK(640 * RDP_SCALE_MAX <= 4096);
}

/* Imagen lógica de prueba (0xRRGGBB por punto). Su superficie a escala
 * `sc` tiene cada punto lógico en un bloque de sc × sc píxeles, como lo
 * dibuja el RSX con rsxkit_bind en coordenadas lógicas: el píxel (x, y) es
 * el punto (x / sc, y / sc). */
#define SIM_LW 320
#define SIM_LH 240
static uint32_t sim_img[SIM_LH][SIM_LW];

static void sim_fill(uint32_t lw, uint32_t lh, uint32_t seed)
{
	for (uint32_t y = 0; y < lh; y++)
		for (uint32_t x = 0; x < lw; x++) {
			seed = seed * 1103515245u + 12345u;
			sim_img[y][x] = seed >> 8 & 0xFFFFFF;
		}
}

/* Muestreo lineal de la superficie a escala `sc` (sw × sh píxeles) en
 * (u, v) en píxeles, como la textura del blit del RSX (centros en .5,
 * borde repetido). */
static uint32_t sim_bilinear(uint32_t sc, uint32_t sw, uint32_t sh, float u, float v)
{
	float fx = u - 0.5f, fy = v - 0.5f;
	int x0 = (int)(fx < 0.0f ? fx - 1.0f : fx), y0 = (int)(fy < 0.0f ? fy - 1.0f : fy);
	float ax = fx - (float)x0, ay = fy - (float)y0;
	uint32_t out = 0;
	for (int ch = 0; ch < 3; ch++) {
		float acc = 0.0f;
		for (int j = 0; j < 2; j++)
			for (int i = 0; i < 2; i++) {
				int x = x0 + i, y = y0 + j;
				x = x < 0 ? 0 : x >= (int)sw ? (int)sw - 1 : x;
				y = y < 0 ? 0 : y >= (int)sh ? (int)sh - 1 : y;
				float wgt = (i ? ax : 1.0f - ax) * (j ? ay : 1.0f - ay);
				acc += wgt * (float)(sim_img[(uint32_t)y / sc][(uint32_t)x / sc] >> (8 * ch) & 0xFF);
			}
		out |= (uint32_t)(acc + 0.5f) << (8 * ch);
	}
	return out;
}

/* Copia RSX → RDRAM simulada (rsxkit_target_read: un quad de lw × rows
 * con filtro lineal sobre la región de la superficie que da
 * rdp_surface_rect con la escala `rect_sc`). ¿Devuelve las `rows` primeras
 * filas de la imagen lógica? */
static bool sim_readback_ok(uint32_t lw, uint32_t lh, uint32_t sc, uint32_t rect_sc, uint32_t rows)
{
	struct rdp_rect r;
	rdp_surface_rect(rect_sc, 0, 0, lw, rows, &r);
	if (r.x + r.w > lw * sc || r.y + r.h > lh * sc)
		return false;  /* rsxkit_target_read la rechazaría */
	for (uint32_t y = 0; y < rows; y++)
		for (uint32_t x = 0; x < lw; x++) {
			float u = (float)r.x + ((float)x + 0.5f) * (float)r.w / (float)lw;
			float v = (float)r.y + ((float)y + 0.5f) * (float)r.h / (float)rows;
			if (sim_bilinear(sc, lw * sc, lh * sc, u, v) != sim_img[y][x])
				return false;
		}
	return true;
}

/* Copias RSX → RDRAM con la escala del destino, distinta de 3×: cada
 * destino se reduce a 1× desde su propia escala (la objetivo no sirve: Link
 * de 64 × 112 va a 5× con la objetivo 3×, un framebuffer de 640 × 480 a
 * 1×). Con cualquier escala entera el muestreo lineal en el centro de cada
 * bloque da el punto lógico. */
static void targets_readback_scaled(void)
{
	static const struct {
		uint32_t g, lw, lh, rows;
	} c[] = {
		{ 3, 64, 112, 112 },  /* Link: 5× */
		{ 3, 64, 112, 70 },   /* solo las filas dibujadas */
		{ 1, 320, 240, 240 }, { 2, 320, 240, 200 }, { 4, 320, 240, 240 }, { 5, 320, 240, 237 },
		{ 3, 320, 240, 240 },
		{ 5, 64, 48, 48 },    /* auxiliar chica: 5× con tope */
		{ 1, 64, 48, 48 },    /* 5× aunque la objetivo sea 1× */
	};
	for (unsigned k = 0; k < sizeof c / sizeof c[0]; k++) {
		uint32_t sc = rdp_target_scale(c[k].g, c[k].lw, c[k].lh);
		sim_fill(c[k].lw, c[k].lh, 7 + k);
		CHECK(sim_readback_ok(c[k].lw, c[k].lh, sc, sc, c[k].rows));
	}
	/* Con la escala objetivo (3×) en vez de la del destino (5×), la copia de
	 * Link lee solo una parte de la superficie y sale otra imagen. */
	CHECK_EQ(rdp_target_scale(3, 64, 112), 5u);
	sim_fill(64, 112, 99);
	CHECK(!sim_readback_ok(64, 112, 5, 3, 112));
	/* Un framebuffer de 640 × 480 con la objetivo 3× queda a 1×; leer un
	 * destino a 1× con la escala objetivo saca la región de la superficie. */
	CHECK_EQ(rdp_target_scale(3, 640, 480), 1u);
	sim_fill(SIM_LW, SIM_LH, 5);
	CHECK(!sim_readback_ok(SIM_LW, SIM_LH, 1, 3, SIM_LH));
	CHECK(sim_readback_ok(SIM_LW, SIM_LH, 1, 1, SIM_LH));
	/* La presentación con recorte del overscan: la región en la escala del
	 * destino. */
	struct rdp_rect r;
	rdp_surface_rect(5, 8, 6 + 2, 304, 228, &r);
	CHECK_EQ(r.x, 40);
	CHECK_EQ(r.y, 40);
	CHECK_EQ(r.w, 1520);
	CHECK_EQ(r.h, 1140);
}

/* Render a textura entre destinos de escalas distintas: se dibuja en el
 * framebuffer (3×) con una textura que sale de Link (64 × 112, a 5×). Las
 * filas ligadas van en la escala del destino leído y u, v quedan
 * normalizadas: cada texel de la textura de la HLE cae en su punto lógico
 * del destino leído, sea cual sea la escala del destino en que se dibuja. */
static void targets_texture_cross_scale(void)
{
	static const struct {
		uint32_t sc, lw, lh, x0, y0, tw, th, ds;
	} c[] = {
		{ 5, 64, 112, 0, 0, 64, 112, 0 },   /* Link entero */
		{ 5, 64, 112, 8, 20, 32, 40, 0 },   /* una parte */
		{ 3, 320, 240, 0, 6, 320, 6, 0 },   /* una franja de la pausa de Zelda */
		{ 1, 640, 480, 100, 50, 64, 32, 0 },
		{ 2, 320, 240, 16, 0, 64, 64, 16 }, /* desplazada 16 texels en s */
	};
	for (unsigned k = 0; k < sizeof c / sizeof c[0]; k++) {
		uint32_t sc = c[k].sc, sw = c[k].lw * sc;
		struct rdp_tex_map m;
		/* La HLE normaliza u a la textura: u = s / tw + ds / tw. */
		float xf0 = 1.0f / (float)c[k].tw, xf2 = (float)c[k].ds / (float)c[k].tw;
		rdp_target_tex_map(sc, c[k].lw, c[k].x0, c[k].y0, c[k].tw, c[k].th, xf0, xf2, &m);
		CHECK_EQ(m.row0, c[k].y0 * sc);
		CHECK_EQ(m.rows, c[k].th * sc);
		CHECK(m.row0 + m.rows <= c[k].lh * sc);
		bool ok = true;
		for (uint32_t t = 0; t < c[k].th && ok; t++)
			for (uint32_t s2 = 0; s2 + c[k].ds < c[k].tw && ok; s2++) {
				float u = ((float)s2 + 0.5f) * m.xf0 + m.xf2;  /* sobre la fila entera */
				float v = ((float)t + 0.5f) / (float)c[k].th;  /* sobre las filas ligadas */
				uint32_t px = (uint32_t)(u * (float)sw), py = m.row0 + (uint32_t)(v * (float)m.rows);
				ok = px / sc == c[k].x0 + c[k].ds + s2 && py / sc == c[k].y0 + t;
			}
		CHECK(ok);
	}
}

/* Destinos sin z image propia: se ligan con la profundidad de la
 * pantalla, que nunca puede ser más chica que el color (rsxkit_bind se
 * niega). A 4× y 5× en 720p (5× en 1080p) un destino es más grande que la
 * pantalla: rdp_zeta_reserve la agranda al mayor destino posible, y con eso
 * cubre cualquier destino, scratch o profundidad a esa escala, en
 * cualquier pantalla. */
static void targets_zeta_covers(void)
{
	uint32_t w, h;
	/* El caso: 5× en 720p, Link o el framebuffer de 320 × 240 a 1600 × 1200
	 * no caben en la profundidad de 1280 × 720. */
	CHECK_EQ(rdp_target_scale(5, 320, 240) * 240, 1200u);
	CHECK(rdp_zeta_reserve(5, 1280, 720, &w, &h));
	CHECK_EQ(w, 1600);
	CHECK_EQ(h, 1200);
	CHECK(rdp_zeta_reserve(4, 1280, 720, &w, &h));
	CHECK_EQ(w, 1280);
	CHECK_EQ(h, 960);
	CHECK(rdp_zeta_reserve(5, 1920, 1080, &w, &h));  /* 1200 líneas > 1080 */
	CHECK_EQ(h, 1200);
	/* Donde la de la pantalla alcanza, no se agranda (0 × 0). */
	CHECK(!rdp_zeta_reserve(3, 1280, 720, &w, &h));
	CHECK_EQ(w, 0);
	CHECK_EQ(h, 0);
	CHECK(!rdp_zeta_reserve(4, 1920, 1080, &w, &h));
	CHECK(!rdp_zeta_reserve(2, 720, 480, &w, &h));   /* 480p: 640 × 480 */
	CHECK(!rdp_zeta_reserve(1, 720, 480, &w, &h));
	/* Todas las pantallas y escalas: la zeta (la de la pantalla, agrandada
	 * como rsxkit_depth_reserve) cubre cada superficie posible. */
	static const uint32_t screens[][2] = { { 1280, 720 }, { 1920, 1080 }, { 720, 480 }, { 720, 576 } };
	for (unsigned k = 0; k < sizeof screens / sizeof screens[0]; k++)
		for (uint32_t g = 1; g <= RDP_SCALE_MAX; g++) {
			uint32_t sw = screens[k][0], sh = screens[k][1];
			bool grow = rdp_zeta_reserve(g, sw, sh, &w, &h);
			uint32_t zw = w > sw ? w : sw, zh = h > sh ? h : sh;
			bool all_fit = true, ok = true;
			for (uint32_t lw = 8; lw <= 640; lw += 8)
				for (uint32_t lh = lw * 3 / 4; lh <= 480; lh += 7) {
					uint32_t sc = rdp_target_scale(g, lw, lh);
					ok = ok && zw >= lw * sc && zh >= lh * sc;
					all_fit = all_fit && sw >= lw * sc && sh >= lh * sc;
				}
			CHECK(ok);
			CHECK(grow == !all_fit);  /* solo se agranda cuando hace falta */
			if (grow)  /* y cuenta como una superficie más del peor caso */
				CHECK(rdp_surface_bytes(w, h) * RDP_SURFACES_MAX <= rdp_vram_worst(g));
		}
}

/* Presentar a un múltiplo entero o ajustar: entero si cabe y llena al
 * menos 3/4 del alto (lo de antes a la escala automática), ajustado si la
 * escala por destino deja la región chica. */
/* La z de la N64 que el CPU deja en la RDRAM, a la profundidad del RSX. */
static void targets_z16_to_z24(void)
{
	CHECK_EQ(rdp_z16_to_z24(0xFFFC), 0xFFFFFFu);  /* borrada: lo más lejos */
	CHECK_EQ(rdp_z16_to_z24(0x0000), 0u);
	/* Exponente 0: mantisa << 6; 0x400 de mantisa es 0x10000 de 0x3FFFF. */
	CHECK_EQ(rdp_z16_to_z24(0x400 << 2), (0x10000u << 6) | (0x10000u >> 12));
	/* Exponente 1 empieza en 0x20000 (la mitad de la escala). */
	CHECK_EQ(rdp_z16_to_z24((1u << 11) << 2), (0x20000u << 6) | (0x20000u >> 12));
	/* Creciente en todo el rango (el dz no cuenta). */
	uint32_t prev = 0;
	for (uint32_t v = 4; v < 0x10000; v += 4) {
		uint32_t z = rdp_z16_to_z24((uint16_t)v);
		CHECK(z >= prev);
		CHECK_EQ(rdp_z16_to_z24((uint16_t)(v | 3)), z);
		prev = z;
	}
}

static void targets_present_integer(void)
{
	CHECK(rdp_present_integer(960, 720, 1280, 720));     /* 3× en 720p */
	CHECK(rdp_present_integer(912, 684, 1280, 720));     /* 3× con recorte */
	CHECK(rdp_present_integer(320, 240, 1280, 720));     /* 1×: por 3 */
	CHECK(rdp_present_integer(1280, 960, 1920, 1080));   /* 4× en 1080p */
	CHECK(rdp_present_integer(640, 480, 1920, 1080));    /* 2× en 1080p: por 2 */
	CHECK(rdp_present_integer(640, 480, 720, 480));      /* 2× en 480p */
	CHECK(!rdp_present_integer(640, 480, 1280, 720));    /* 2× en 720p, o 640 × 480 a 1× */
	CHECK(!rdp_present_integer(960, 720, 1920, 1080));   /* 3× en 1080p */
	CHECK(!rdp_present_integer(1600, 1200, 1280, 720));  /* 5×: supermuestreo */
	CHECK(!rdp_present_integer(1280, 960, 1280, 720));   /* 4× en 720p */
	CHECK(!rdp_present_integer(0, 240, 1280, 720));
}

/* Espejo del framebuffer, una vez por dibujo (la política que siguen rdp.c
 * y el rasterizador del host; tests/unit/test_raster.c la prueba de punta
 * a punta). La secuencia de Super Mario 64 al pasar del logo a la cara de
 * Mario: triple búfer; el RDP dibuja B; el VI sigue en A; un DMA carga
 * datos encima de A y de B; el VI muestra B sin que el RDP lo toque. */
/* Logos de Donkey Kong 64: destino de 640 × 480 y VI de 640 × 237 con
 * stride 2560 (una línea de cada dos), origen en la línea 0 o la 1 según
 * el campo: se presentan las 474 líneas desde la 0 en los dos casos. Con
 * el stride normal, las 237 desde el origen. */
static void targets_present_interlaced(void)
{
	const uint32_t A = 0x34000;
	bool fresh;
	rdp_targets_init(&s);
	int a = rdp_targets_get(&s, A, 640, SIZ_16b, 480, &fresh);
	rdp_targets_drawn(&s, a);
	struct salsa64_vi_frame f;
	memset(&f, 0, sizeof f);
	f.type = SALSA64_VI_RGBA5551;
	f.width = 640;
	f.height = 237;
	f.stride = 2560;
	f.bpp = 2;
	struct rdp_present p;
	for (uint32_t par = 0; par < 2; par++) {
		f.origin = A + par * 1280;
		rdp_targets_present(&s, &f, 10 + par, &p);
		CHECK_EQ(p.slot, a);
		CHECK_EQ(p.y0, 0);
		CHECK_EQ(p.w, 640);
		CHECK_EQ(p.h, 474);
	}
	f.stride = 1280;
	f.origin = A + 1280;
	rdp_targets_present(&s, &f, 12, &p);
	CHECK_EQ(p.y0, 1);
	CHECK_EQ(p.h, 237);
}

/* Entrelazado con la posición vertical del campo (docs/estado.md, «VI
 * entrelazado»): Racer (destino de 320 × 480, VI de 320 × 237 con stride de
 * 2 filas; el campo 1 empieza una fila arriba y dos medias líneas antes) y
 * Rogue Squadron (destino de 640 × 335, stride de 1 fila; el campo 1
 * empieza una fila abajo y dos medias líneas después). Cada campo cae en
 * un renglón contiguo de la trama; se presenta la misma ventana en los dos,
 * no cada una desde su origen (la imagen subía y bajaba una fila). */
static void targets_present_interlaced_vpos(void)
{
	bool fresh;
	struct rdp_present p;
	/* Racer */
	const uint32_t RACER = 0x63b800;
	struct salsa64_vi_frame f = frame(RACER + 1280, 320, 237);
	f.stride = 1280;
	struct rdp_vi_ring vi;
	memset(&vi, 0, sizeof vi);
	rdp_vi_ring_frame(&vi, &f);
	/* El origen está dos filas dentro; 237 filas del VI con paso doble
	 * ocupan 474 del destino. La tijera de la tarea puede terminar antes. */
	CHECK_EQ(rdp_vi_ring_min_height(&vi, RACER, 320, 2), 476u);
	rdp_targets_init(&s);
	int r = rdp_targets_get(&s, RACER, 320, SIZ_16b, rdp_vi_ring_min_height(&vi, RACER, 320, 2), &fresh);
	rdp_targets_drawn(&s, r);
	for (uint32_t n = 0; n < 6; n++) {
		bool odd = n & 1;  /* campo con V_VIDEO 0x2301FD, origen una fila arriba */
		f.origin = odd ? RACER + 640 : RACER + 1280;
		f.vpos = odd ? 0x23 + 1 : 0x25;
		rdp_targets_present(&s, &f, 20 + n, &p);
		CHECK_EQ(p.slot, r);
		CHECK_EQ(p.y0, 1);
		CHECK_EQ(p.h, 474);
	}
	/* Rogue */
	const uint32_t ROGUE = 0x790000;
	rdp_targets_init(&s);
	int g = rdp_targets_get(&s, ROGUE, 640, SIZ_16b, 335, &fresh);
	rdp_targets_drawn(&s, g);
	f = frame(0, 640, 316);
	f.stride = 1280;
	for (uint32_t n = 0; n < 6; n++) {
		bool second = n & 1;  /* origen una fila abajo, V_VIDEO 0x3501EF */
		f.origin = ROGUE + 1280 * (1 + second);
		f.vpos = second ? 0x35 : 0x33 + 1;
		rdp_targets_present(&s, &f, 40 + n, &p);
		CHECK_EQ(p.slot, g);
		CHECK_EQ(p.y0, 1);
		CHECK_EQ(p.h, 316);
	}
	/* Sin entrelazado (vpos 0), nada cambia: cada origen es su fila. */
	f.vpos = 0;
	f.origin = ROGUE + 2 * 1280;
	rdp_targets_present(&s, &f, 50, &p);
	CHECK_EQ(p.y0, 2);
}

/* Dos cuadros completos alternos (Indy): V_VIDEO y el origen avanzan
 * juntos una fila; la ventana presentada debe ser común a ambos búferes. */
static void targets_present_full_interlaced(void)
{
	const uint32_t base[2] = { 0x6b3000, 0x749000 };
	bool fresh;
	struct rdp_present p;
	for (unsigned legacy = 0; legacy < 2; legacy++) {
		rdp_targets_init(&s);
		int slot[2];
		for (unsigned i = 0; i < 2; i++) {
			slot[i] = rdp_targets_get(&s, base[i], 640, SIZ_16b, 480, &fresh);
			rdp_targets_drawn(&s, slot[i]);
		}
		for (unsigned n = 0; n < 6; n++) {
			unsigned i = n & 1;
			struct salsa64_vi_frame f = frame(base[i] + (1 + i) * 1280, 640, 474);
			f.vpos = i ? 0x25 : 0x23 + legacy;
			f.weave = !legacy;
			rdp_targets_present(&s, &f, 10 + n, &p);
			CHECK_EQ(p.slot, slot[i]);
			CHECK_EQ(p.y0, legacy ? 1 + i : 1);
			CHECK_EQ(p.h, 474);
		}
		/* Tras una pausa sin presentar no se arrastra la ventana entre
		 * superficies de otra escena aunque tengan el mismo tamaño. */
		struct salsa64_vi_frame gap = frame(base[0] + 1280, 640, 474);
		gap.vpos = 0x23;
		gap.weave = !legacy;
		rdp_targets_present(&s, &gap, 16, &p);
		gap.origin = base[1] + 2 * 1280;
		gap.vpos = 0x25;
		rdp_targets_present(&s, &gap, 18, &p);
		CHECK_EQ(p.y0, 2);
		/* Cambiar la ventana del juego de verdad no queda fijado al pasado. */
		struct salsa64_vi_frame f = frame(base[0] + 4 * 1280, 640, 474);
		f.vpos = 0x25;
		f.weave = !legacy;
		rdp_targets_present(&s, &f, 19, &p);
		CHECK_EQ(p.y0, 4);
	}
}

/* FIFA 99 (#69): dos búferes de 480 × 360 (y de 512 × 384 en los menús)
 * que el VI muestra a Y_SCALE 1,5 (1,6): cada campo puede ser del otro
 * búfer, con el origen en la fila 1 (campo impar, V_VIDEO 0x23) o en la 2
 * (par, 0x25). Los orígenes y campos son los de salsa64-run --trace-vi en el
 * partido (campos 5501–5513, iguales en ares). Tejido, la ventana se queda
 * en la fila 1; por campos (vi-escalado-campos) saltaba 1 ↔ 2 cada campo. */
static void targets_present_scaled_interlaced(void)
{
	static const struct { uint32_t base[2], width, lh, height; } m[2] = {
		{ { 0x157640, 0x1abcc0 }, 480, 360, 356 },
		{ { 0x157640, 0x1b76c0 }, 512, 384, 379 },
	};
	/* (búfer, campo) de los campos 5501–5513 del partido. */
	static const uint8_t seq[13][2] = {
		{ 0, 1 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 0, 1 }, { 0, 0 }, { 1, 1 },
		{ 0, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 1, 0 }, { 0, 1 },
	};
	bool fresh;
	struct rdp_present p;
	for (unsigned mi = 0; mi < 2; mi++) {
		for (unsigned legacy = 0; legacy < 2; legacy++) {
			rdp_targets_init(&s);
			int slot[2];
			for (unsigned i = 0; i < 2; i++) {
				slot[i] = rdp_targets_get(&s, m[mi].base[i], m[mi].width, SIZ_16b, m[mi].lh, &fresh);
				rdp_targets_drawn(&s, slot[i]);
			}
			unsigned jumps = 0, prev = 0, want_jumps = 0, want_prev = 0;
			for (unsigned n = 0; n < 13; n++) {
				unsigned b = seq[n][0], odd = seq[n][1];
				struct salsa64_vi_frame f = frame(m[mi].base[b] + m[mi].width * 2 * (odd ? 1 : 2), m[mi].width,
								  m[mi].height);
				/* Lo que da salsa64_vi_frame (test_vi.c, scaled_interlaced_frame). */
				f.weave = !legacy;
				f.vpos = odd ? 0x23 + legacy : 0x25;
				rdp_targets_present(&s, &f, 100 + n, &p);
				CHECK_EQ(p.slot, slot[b]);
				CHECK_EQ(p.h, m[mi].height);
				/* Por campos, la ventana solo se sostenía dentro del mismo
				 * búfer: un campo par que sigue al otro búfer salía de la fila 2. */
				unsigned want = !legacy || odd || (n && seq[n - 1][0] == b) ? 1 : 2;
				CHECK_EQ(p.y0, want);
				jumps += n && p.y0 != prev;
				want_jumps += n && want != want_prev;
				prev = p.y0;
				want_prev = want;
			}
			CHECK_EQ(jumps, want_jumps);
			CHECK_EQ(jumps, legacy ? 8u : 0u);
		}
	}
	/* Rogue Squadron (un solo búfer, Y_SCALE 1,33): tejido también queda en
	 * la fila 1, como por campos. */
	const uint32_t ROGUE = 0x790000;
	rdp_targets_init(&s);
	int g = rdp_targets_get(&s, ROGUE, 640, SIZ_16b, 335, &fresh);
	rdp_targets_drawn(&s, g);
	for (uint32_t n = 0; n < 6; n++) {
		bool second = n & 1;
		struct salsa64_vi_frame f = frame(ROGUE + 1280 * (1 + second), 640, 316);
		f.weave = true;
		f.vpos = second ? 0x35 : 0x33;
		rdp_targets_present(&s, &f, 60 + n, &p);
		CHECK_EQ(p.slot, g);
		CHECK_EQ(p.y0, 1);
		CHECK_EQ(p.h, 316);
	}
}

/* Color image de 320 × 720 (Star Fox 64): los tres framebuffers seguidos
 * caben enteros dentro, con sus filas de inicio; otro ancho, no. */
static void targets_inside(void)
{
	const uint32_t A = 0x38f800, B = 0x3b5000, C = 0x3da800;
	bool fresh;
	rdp_targets_init(&s);
	int a = rdp_targets_get(&s, A, 320, SIZ_16b, 240, &fresh);
	int b = rdp_targets_get(&s, B, 320, SIZ_16b, 240, &fresh);
	int c = rdp_targets_get(&s, C, 320, SIZ_16b, 240, &fresh);
	int slots[RDP_TARGETS_MAX];
	uint32_t first[RDP_TARGETS_MAX];
	int n = rdp_targets_inside(&s, A, 320, 2, 720, slots, first);
	CHECK_EQ(n, 3);
	for (int i = 0; i < n; i++) {
		if (slots[i] == a) CHECK_EQ(first[i], 0);
		if (slots[i] == b) CHECK_EQ(first[i], 240);
		if (slots[i] == c) CHECK_EQ(first[i], 480);
	}
	CHECK_EQ(rdp_targets_inside(&s, A, 320, 2, 600, slots, first), 3);  /* C asoma por abajo */
	CHECK_EQ(rdp_targets_inside(&s, A, 320, 2, 480, slots, first), 2);  /* C empieza fuera */
	CHECK_EQ(rdp_targets_inside(&s, A, 640, 2, 720, slots, first), 0);
	CHECK_EQ(rdp_targets_inside(&s, A + 2, 320, 2, 720, slots, first), 0);  /* desalineado */
	CHECK(rdp_targets_tall(320, 459));
	CHECK(rdp_targets_tall(320, 720));
	CHECK(!rdp_targets_tall(320, 240));
	CHECK(!rdp_targets_tall(320, 288));  /* PAL */
	CHECK(!rdp_targets_tall(640, 480));
	CHECK(rdp_targets_tall(640, 720));
}

/* Star Fox 64: el FILLRECT de 320 × 720 llega en la primera tarea gráfica,
 * antes de que exista ningún framebuffer; los que nacen después lo reciben
 * una vez, salvo que un DMA haya tocado su memoria desde el relleno, que
 * hayan pasado RDP_TALL_FILL_TASKS tareas o que ya lo hayan recibido. */
static void targets_tall_pending(void)
{
	const uint32_t A = 0x38f800, B = 0x3b5000, C = 0x3da800;
	bool fresh;
	struct rdp_dma_ring dma;
	struct rdp_tall_rect r[RDP_TALL_FILLS];
	memset(&dma, 0, sizeof dma);
	rdp_targets_init(&s);
	rdp_targets_task(&s);  /* tarea 1: el relleno de 320 × 720, sin destinos */
	rdp_targets_tall_note(&s, A, 320, 2, 720, 0, 0, 320, 720, 0x28323c, NULL, 0);
	/* Las tres tareas siguientes crean un destino cada una. */
	rdp_targets_task(&s);
	int b = rdp_targets_get(&s, B, 320, SIZ_16b, 240, &fresh);
	CHECK(fresh);
	CHECK_EQ(rdp_targets_tall_pending(&s, b, &dma, r), 1);
	CHECK_EQ(r[0].x0, 0.0f);
	CHECK_EQ(r[0].y0, 0.0f);  /* B empieza en la fila 240 de la imagen alta: el relleno lo cubre entero */
	CHECK_EQ(r[0].x1, 320.0f);
	CHECK_EQ(r[0].y1, 240.0f);
	CHECK_EQ(r[0].color, 0x28323cu);
	CHECK_EQ(rdp_targets_tall_pending(&s, b, &dma, r), 0);  /* una sola vez por destino */
	rdp_targets_task(&s);
	int c = rdp_targets_get(&s, C, 320, SIZ_16b, 240, &fresh);
	CHECK_EQ(rdp_targets_tall_pending(&s, c, &dma, r), 1);
	/* Un DMA tocó la memoria de A después del relleno (tarea 1): no se pisa. */
	rdp_dma_note(&dma, A + 0x2800, 0x100, 1);
	rdp_targets_task(&s);
	int a = rdp_targets_get(&s, A, 320, SIZ_16b, 240, &fresh);
	CHECK_EQ(rdp_targets_tall_pending(&s, a, &dma, r), 0);
	/* Un DMA anterior al relleno no cuenta (lo hace la consola antes de que el RDP lo borre). */
	rdp_targets_init(&s);
	memset(&dma, 0, sizeof dma);
	rdp_dma_note(&dma, A + 0x2800, 0x100, 0);
	rdp_targets_task(&s);
	rdp_targets_tall_note(&s, A, 320, 2, 720, 0, 0, 320, 720, 0x010203, NULL, 0);
	rdp_targets_task(&s);
	a = rdp_targets_get(&s, A, 320, SIZ_16b, 240, &fresh);
	CHECK_EQ(rdp_targets_tall_pending(&s, a, &dma, r), 1);
	/* Un destino que ya existía y lo recibió al repartirse no lo recibe otra vez. */
	rdp_targets_init(&s);
	memset(&dma, 0, sizeof dma);
	a = rdp_targets_get(&s, A, 320, SIZ_16b, 240, &fresh);
	int done[1] = { a };
	rdp_targets_task(&s);
	rdp_targets_tall_note(&s, A, 320, 2, 720, 0, 0, 320, 720, 0x010203, done, 1);
	CHECK_EQ(rdp_targets_tall_pending(&s, a, &dma, r), 0);
	/* Pasadas RDP_TALL_FILL_TASKS tareas vale lo que haya en la RDRAM. */
	for (int k = 0; k <= RDP_TALL_FILL_TASKS; k++)
		rdp_targets_task(&s);
	b = rdp_targets_get(&s, B, 320, SIZ_16b, 240, &fresh);
	CHECK_EQ(rdp_targets_tall_pending(&s, b, &dma, r), 0);
	/* Otro ancho u otra dirección fuera de la imagen: nada. */
	rdp_targets_init(&s);
	rdp_targets_task(&s);
	rdp_targets_tall_note(&s, A, 320, 2, 720, 0, 0, 320, 720, 0x010203, NULL, 0);
	int w = rdp_targets_get(&s, B, 640, SIZ_16b, 480, &fresh);
	CHECK_EQ(rdp_targets_tall_pending(&s, w, &dma, r), 0);
	int o = rdp_targets_get(&s, A + 0x4000 * 12, 320, SIZ_16b, 240, &fresh);  /* más allá de las 720 filas */
	CHECK_EQ(rdp_targets_tall_pending(&s, o, &dma, r), 0);
	/* Un rectángulo parcial (8..312 × 8..232 de la fila 240): se recorta al destino. */
	rdp_targets_init(&s);
	rdp_targets_task(&s);
	rdp_targets_tall_note(&s, A, 320, 2, 720, 8, 248, 312, 472, 0x111111, NULL, 0);
	b = rdp_targets_get(&s, B, 320, SIZ_16b, 240, &fresh);
	CHECK_EQ(rdp_targets_tall_pending(&s, b, &dma, r), 1);
	CHECK_EQ(r[0].x0, 8.0f);
	CHECK_EQ(r[0].y0, 8.0f);
	CHECK_EQ(r[0].x1, 312.0f);
	CHECK_EQ(r[0].y1, 232.0f);
}

/* Anillo de DMA: los trozos seguidos de la misma tarea se funden; una
 * copia de la tarea T solo se detiene por DMA desde T. */
static void targets_dma_ring(void)
{
	struct rdp_dma_ring r;
	memset(&r, 0, sizeof r);
	for (uint32_t i = 0; i < 500; i++)
		rdp_dma_note(&r, 0x38f800 + i * 256, 256, 67);  /* Star Fox: 125 KB en trozos */
	CHECK_EQ(r.n, 1);
	CHECK(rdp_dma_since(&r, 0x38f800, 0x38f800 + 640 * 232, 67));
	CHECK(rdp_dma_since(&r, 0x38f800, 0x38f800 + 640 * 232, 60));
	CHECK(!rdp_dma_since(&r, 0x38f800, 0x38f800 + 640 * 232, 68));  /* dibujado después del DMA */
	CHECK(!rdp_dma_since(&r, 0x300000, 0x310000, 1));
	rdp_dma_note(&r, 0x100000, 64, 68);
	CHECK_EQ(r.n, 2);
	CHECK(rdp_dma_since(&r, 0x100000, 0x100040, 68));
	/* Se llena: sobreviven las últimas RDP_DMA_RING. */
	for (uint32_t i = 0; i < RDP_DMA_RING + 5; i++)
		rdp_dma_note(&r, 0x200000 + i * 4096, 16, 70 + i);
	CHECK(!rdp_dma_since(&r, 0x100000, 0x100040, 68));
	CHECK(rdp_dma_since(&r, 0x200000 + (RDP_DMA_RING + 4) * 4096, 0x200000 + (RDP_DMA_RING + 4) * 4096 + 16, 1));
}

/* La guarda de las copias tardías (rdp_guard): Indiana Jones arma sus
 * listas de pantalla donde estuvo el framebuffer de 640 × 480 de la pantalla
 * del Expansion Pak (0x6b3000); la copia de ese cuadro, que llega después,
 * no debe pisar lo que el CPU escribió. */
static uint8_t guard_ram[1u << 20] __attribute__((aligned(128)));
static struct rdp_guard guard_t;

static void targets_cpu_guard(void)
{
	const uint32_t FB = 0x6b300, LINE = 1280, ROWS = 240;
	memset(guard_ram, 0, sizeof guard_ram);
	rdp_guard_clear(&guard_t);
	/* Sin huella, todo se puede escribir (como antes de la guarda). */
	CHECK(rdp_guard_ok(&guard_t, guard_ram, FB >> RDP_GUARD_SHIFT));
	CHECK(rdp_guard_hash(guard_ram) != 0);
	/* Termina la tarea que dibujó el cuadro: huellas de sus filas. */
	rdp_guard_mark(&guard_t, guard_ram, sizeof guard_ram, FB, FB + LINE * ROWS);
	uint32_t l0 = FB >> RDP_GUARD_SHIFT, l1 = (FB + LINE * ROWS - 1) >> RDP_GUARD_SHIFT;
	for (uint32_t l = l0; l <= l1; l++)
		CHECK(rdp_guard_ok(&guard_t, guard_ram, l));
	/* El CPU escribe una lista (una sola palabra basta) en la fila 85. */
	uint32_t dl = FB + 85 * LINE + 0x58;
	guard_ram[dl] = 0xDE;
	CHECK(!rdp_guard_ok(&guard_t, guard_ram, dl >> RDP_GUARD_SHIFT));
	CHECK(rdp_guard_ok(&guard_t, guard_ram, (dl >> RDP_GUARD_SHIFT) + 1));
	CHECK(rdp_guard_ok(&guard_t, guard_ram, (dl >> RDP_GUARD_SHIFT) - 1));
	/* Cualquier byte de la línea, y cambios que conservan la suma. */
	guard_ram[dl] = 0;
	CHECK(rdp_guard_ok(&guard_t, guard_ram, dl >> RDP_GUARD_SHIFT));
	uint32_t la = dl & ~(RDP_GUARD_LINE - 1);
	guard_ram[la + 7] = 1;   /* +1 en la palabra 0 */
	guard_ram[la + 15] = 0xFF;  /* y −1 en la 1: la misma suma */
	guard_ram[la + 14] = 0xFF;
	guard_ram[la + 13] = 0xFF;
	guard_ram[la + 12] = 0xFF;
	guard_ram[la + 11] = 0xFF;
	guard_ram[la + 10] = 0xFF;
	guard_ram[la + 9] = 0xFF;
	guard_ram[la + 8] = 0xFF;
	CHECK(!rdp_guard_ok(&guard_t, guard_ram, la >> RDP_GUARD_SHIFT));
	memset(guard_ram + la, 0, 16);
	/* La copia escribe la línea: su resultado también vale (la tarea
	 * siguiente pudo tomar las huellas antes de que llegara la copia). */
	uint32_t l = (FB + 10 * LINE) >> RDP_GUARD_SHIFT;
	memset(guard_ram + (l << RDP_GUARD_SHIFT), 0x01, RDP_GUARD_LINE);  /* el cuadro, de la copia */
	CHECK(!rdp_guard_ok(&guard_t, guard_ram, l));
	rdp_guard_wrote(&guard_t, guard_ram, l);
	CHECK(rdp_guard_ok(&guard_t, guard_ram, l));
	rdp_guard_mark(&guard_t, guard_ram, sizeof guard_ram, FB, FB + LINE);  /* otra tarea, sin la copia */
	CHECK(rdp_guard_ok(&guard_t, guard_ram, l));
	guard_ram[(l << RDP_GUARD_SHIFT) + 3] = 0x42;  /* y después el CPU */
	CHECK(!rdp_guard_ok(&guard_t, guard_ram, l));
	/* Fuera de la RDRAM no marca nada. */
	rdp_guard_clear(&guard_t);
	rdp_guard_mark(&guard_t, guard_ram, sizeof guard_ram, sizeof guard_ram - 64, sizeof guard_ram + 4096);
	CHECK(guard_t.base[(sizeof guard_ram - 1) >> RDP_GUARD_SHIFT] != 0);
	CHECK_EQ(guard_t.base[sizeof guard_ram >> RDP_GUARD_SHIFT], 0);
}

/* Lo que otra color image dibujó después (rdp_claim_ring), con la memoria de
 * FIFA 99 (#69) desplazada 1 MB: el menú dibuja en 512 × 384 desde 0x57640;
 * el juego suelta ese framebuffer, pide uno de 480 × 360 en el mismo lugar
 * (acaba en 0xabc40) y otro bloque cuya cabecera del heap queda en 0xabc70;
 * la primera tarea del partido renueva la huella de la línea 0xabc00 y el
 * espejo tardío del menú la pasaba y escribía su negro sobre la cabecera. */
static void targets_claims(void)
{
	const uint32_t FB = 0x57640, MENU = 1024, GAME = 960, HDR = 0xabc70;
	struct rdp_claim_ring r;
	struct rdp_claim cl[4];
	memset(&r, 0, sizeof r);
	memset(guard_ram, 0, sizeof guard_ram);
	rdp_guard_clear(&guard_t);
	/* Tarea 3678: el último cuadro del menú. */
	memset(guard_ram + FB, 0x01, MENU * 384);
	rdp_guard_mark(&guard_t, guard_ram, sizeof guard_ram, FB, FB + MENU * 384);
	rdp_claim_note(&r, FB, MENU, FB, FB + MENU * 384, 3678);
	/* El CPU: cabecera del bloque del segundo framebuffer y su primera palabra. */
	static const uint8_t hdr[20] = { 0x42, 0x4d, 0x01, 0x40, 0x00, 0x05, 0x46, 0x40, 0x80, 0x20, 0x02, 0xf0,
					 0x80, 0x15, 0x75, 0xf0, 0x80, 0x23, 0x8c, 0xf0 };
	memcpy(guard_ram + HDR, hdr, sizeof hdr);
	/* Tarea 3679: el primer cuadro del partido, en 480 × 360. */
	rdp_guard_mark(&guard_t, guard_ram, sizeof guard_ram, FB, FB + GAME * 360);
	rdp_claim_note(&r, FB, GAME, FB, FB + GAME * 360, 3679);
	CHECK_EQ(FB + GAME * 360, 0xabc40);
	/* La guarda ve la línea 0xabc80 cambiada, pero no la 0xabc00: su huella
	 * es de la tarea 3679, con la cabecera ya escrita. */
	CHECK(rdp_guard_ok(&guard_t, guard_ram, HDR >> RDP_GUARD_SHIFT));
	CHECK(!rdp_guard_ok(&guard_t, guard_ram, (HDR + 16) >> RDP_GUARD_SHIFT));
	/* El espejo del menú (tarea 3678): la línea de la cabecera es de otra
	 * color image posterior; lo que quedó fuera de las dos, no. */
	int n = rdp_claims_after(&r, FB, MENU, FB, FB + MENU * 384, 3678, cl, 4);
	CHECK_EQ(n, 1);
	CHECK_EQ(cl[0].lo, FB);
	CHECK_EQ(cl[0].hi, 0xabc40);
	CHECK(rdp_claimed(cl, n, HDR >> RDP_GUARD_SHIFT));
	CHECK(rdp_claimed(cl, n, FB >> RDP_GUARD_SHIFT));
	CHECK(!rdp_claimed(cl, n, (HDR + 16) >> RDP_GUARD_SHIFT));
	CHECK(!rdp_claimed(cl, n, (FB + MENU * 383) >> RDP_GUARD_SHIFT));
	/* El espejo del partido (tarea 3679) no ve nada: el menú es anterior. */
	CHECK_EQ(rdp_claims_after(&r, FB, GAME, FB, FB + GAME * 360, 3679, cl, 4), 0);
	/* La misma color image vuelta a dibujar no cuenta (eso es de la guarda),
	 * ni otra de la misma tarea (una imagen dentro del framebuffer). */
	rdp_claim_note(&r, FB, GAME, FB, FB + GAME * 100, 3680);
	rdp_claim_note(&r, FB + GAME * 200, GAME, FB + GAME * 200, FB + GAME * 220, 3680);
	CHECK_EQ(rdp_claims_after(&r, FB, GAME, FB, FB + GAME * 360, 3679, cl, 4), 1);
	CHECK_EQ(cl[0].lo, FB + GAME * 200);
	CHECK_EQ(rdp_claims_after(&r, FB, GAME, FB, FB + GAME * 360, 3680, cl, 4), 0);
	/* Fuera del tramo copiado, nada. */
	CHECK_EQ(rdp_claims_after(&r, FB, MENU, 0x200000, 0x210000, 3678, cl, 4), 0);
	/* Varias de la misma color image en una tarea se funden. */
	uint32_t before = r.n;
	rdp_claim_note(&r, FB + GAME * 200, GAME, FB + GAME * 300, FB + GAME * 310, 3680);
	CHECK_EQ(r.n, before);
	CHECK_EQ(rdp_claims_after(&r, FB, GAME, FB, FB + GAME * 360, 3679, cl, 4), 1);
	CHECK_EQ(cl[0].hi, FB + GAME * 310);
	/* Más tramos que lugar: el último cubre los que sobran. */
	for (uint32_t i = 0; i < 6; i++)
		rdp_claim_note(&r, 0x100000 + i * 0x1000, 64, 0x100000 + i * 0x1000, 0x100100 + i * 0x1000, 3681 + i);
	n = rdp_claims_after(&r, 0x90000, 64, 0x100000, 0x110000, 3680, cl, 4);
	CHECK_EQ(n, 4);
	CHECK(rdp_claimed(cl, n, 0x100000 >> RDP_GUARD_SHIFT));
	CHECK(rdp_claimed(cl, n, (0x100000 + 5 * 0x1000) >> RDP_GUARD_SHIFT));
	/* Si el anillo ya perdió entradas posteriores a la tarea, no se sabe. */
	for (uint32_t i = 0; i < RDP_CLAIM_RING; i++)
		rdp_claim_note(&r, 0x300000, 64, 0x300000, 0x300040, 4000 + i);
	CHECK_EQ(rdp_claims_after(&r, FB, MENU, FB, FB + MENU * 384, 3678, cl, 4), -1);
	CHECK_EQ(rdp_claims_after(&r, FB, MENU, 0x300000, 0x300040, 4000, cl, 4), 4);
	CHECK_EQ(rdp_claims_after(&r, FB, MENU, 0x300000, 0x300040, 4063, cl, 4), 0);
}

static void targets_frame_mirror(void)
{
	const uint32_t A = 0x38f800, B = 0x3b5000, C = 0x3da800, LINE = 640;
	bool fresh;
	rdp_targets_init(&s);
	int a = rdp_targets_get(&s, A, 320, SIZ_16b, 0, &fresh);
	CHECK(!rdp_targets_mirror_wanted(&s, a));  /* recién creado: nada dibujado */
	rdp_targets_drawn(&s, a);
	/* El VI aún no lo mostró: no se sabe que es un framebuffer; se copia al
	 * presentarlo, una vez. */
	CHECK(rdp_targets_mirror_wanted(&s, a));
	CHECK(!rdp_targets_mirror_at_task_end(&s, a));
	struct rdp_present p = present(1, A + LINE, 320, 237);
	CHECK_EQ(p.slot, a);
	CHECK(rdp_targets_mirror_wanted(&s, p.slot));
	rdp_targets_mirrored(&s, p.slot);
	p = present(2, A + LINE, 320, 237);
	CHECK(!rdp_targets_mirror_wanted(&s, p.slot));

	/* Triple búfer: B y C se copian al presentarlos la primera vez; desde
	 * entonces, cada dibujo al terminar su tarea. */
	int b = draw(B, 320);
	CHECK(!rdp_targets_mirror_at_task_end(&s, b));
	p = present(3, B + LINE, 320, 237);
	CHECK_EQ(p.slot, b);
	CHECK(rdp_targets_mirror_wanted(&s, b));
	rdp_targets_mirrored(&s, b);
	int c = draw(C, 320);
	p = present(5, C + LINE, 320, 237);
	CHECK_EQ(p.slot, c);
	rdp_targets_mirrored(&s, c);
	uint32_t field = 7;
	for (int k = 0; k < 6; k++, field += 2) {
		uint32_t fb = k % 3 == 0 ? A : k % 3 == 1 ? B : C;
		int i = draw(fb, 320);
		CHECK(rdp_targets_mirror_at_task_end(&s, i));
		rdp_targets_mirrored(&s, i);
		p = present(field, fb + LINE, 320, 237);
		CHECK_EQ(p.slot, i);
		CHECK(!rdp_targets_mirror_wanted(&s, p.slot));
	}

	/* El cambio de escena: B dibujado y copiado al terminar su tarea; el
	 * VI en A dos campos, el DMA, y B un campo: nada que copiar. */
	CHECK_EQ(draw(B, 320), b);
	CHECK(rdp_targets_mirror_at_task_end(&s, b));
	rdp_targets_mirrored(&s, b);
	p = present(field++, A + LINE, 320, 237);
	CHECK(!rdp_targets_mirror_wanted(&s, p.slot));
	p = present(field++, A + LINE, 320, 237);
	CHECK(!rdp_targets_mirror_wanted(&s, p.slot));
	p = present(field++, B + LINE, 320, 237);
	CHECK_EQ(p.slot, b);
	CHECK(!rdp_targets_mirror_wanted(&s, p.slot));

	/* Una color image del ancho del VI que el VI no muestra (el fbufSave de
	 * Zelda) o de otro ancho (Link, 64) no es un framebuffer: no va por el
	 * espejo al terminar la tarea (rdp_readback_wanted y las copias
	 * espejo de las chicas se ocupan). */
	CHECK(!rdp_targets_mirror_at_task_end(&s, draw(0x12c700, 320)));
	CHECK(!rdp_targets_mirror_at_task_end(&s, draw(0x279f00, 64)));
}

/* rdp_targets_recycled dice, sin tocar la tabla, qué ranura en uso cambiaría
 * de imagen: la que recicla rdp_targets_get con la tabla llena, la que
 * crece; nada si la imagen ya existe o hay ranura libre. */
static void targets_recycled_slot(void)
{
	bool fresh;
	memset(&s, 0, sizeof s);
	s.shown = -1;
	s.vi_width = 320;
	for (uint32_t k = 0; k < RDP_TARGETS_MAX; k++) {
		CHECK_EQ(rdp_targets_recycled(&s, 0x100000 + k * 0x10000, 172, SIZ_16b, 48), -1);  /* libre */
		s.field++;
		s.task++;
		draw(0x100000 + k * 0x10000, 172);
	}
	CHECK_EQ(rdp_targets_recycled(&s, 0x100000, 172, SIZ_16b, 48), -1);  /* ya existe */
	int v = rdp_targets_recycled(&s, 0x200000, 172, SIZ_16b, 48);
	CHECK(v >= 0);
	CHECK(s.t[v].used);
	uint32_t old_addr = s.t[v].addr;
	CHECK_EQ(s.t[v].addr, 0x100000);  /* la más vieja */
	CHECK_EQ(s.t[v].addr, old_addr);  /* y preguntar no la cambió */
	int i = rdp_targets_get(&s, 0x200000, 172, SIZ_16b, 48, &fresh);
	CHECK(fresh);
	CHECK_EQ(i, v);
	/* Crecer: la misma dirección y ancho con más alto cambia la misma ranura. */
	int g = rdp_targets_recycled(&s, 0x110000, 172, SIZ_16b, 400);
	CHECK(g >= 0);
	CHECK_EQ(s.t[g].addr, 0x110000);
	i = rdp_targets_get(&s, 0x110000, 172, SIZ_16b, 400, &fresh);
	CHECK(fresh);
	CHECK_EQ(i, g);
}

void t_suite_targets(void)
{
	RUN(targets_recycled_slot);
	RUN(targets_dma_ring);
	RUN(targets_cpu_guard);
	RUN(targets_claims);
	RUN(targets_inside);
	RUN(targets_tall_pending);
	RUN(targets_present_interlaced);
	RUN(targets_present_interlaced_vpos);
	RUN(targets_present_full_interlaced);
	RUN(targets_present_scaled_interlaced);
	RUN(targets_locate);
	RUN(targets_decode_rgba16);
	RUN(targets_texture_policy);
	RUN(targets_readback_wanted);
	RUN(targets_same_memory_other_size);
	RUN(targets_repeated_copy_stays_rsx);
	RUN(targets_height_from_scissor);
	RUN(targets_two_buffers_overlapping);
	RUN(targets_same_addr_two_widths);
	RUN(targets_aux_image_on_framebuffer);
	RUN(targets_held_frame);
	RUN(targets_drawn_while_blank);
	RUN(targets_recycle_order);
	RUN(targets_frame_mirror);
	RUN(targets_scale_pick);
	RUN(targets_readback_scaled);
	RUN(targets_texture_cross_scale);
	RUN(targets_present_integer);
	RUN(targets_z16_to_z24);
	RUN(targets_zeta_covers);
}
