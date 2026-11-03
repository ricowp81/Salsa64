/*
 * Salsa64 — pruebas de la HLE de audio (src/rsp/audio): reconocimiento del
 * ABI, comandos de nead, ABI1 y naudio con listas sintéticas y resultados
 * calculados a mano, el MP3 contra la síntesis de MPEG-1 en coma flotante,
 * partes de la RDRAM escritas y la tarea completa por el núcleo (rsp.c)
 * con el backend síncrono.
 */
#include "test.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/endian.h"
#include "rsp/audio/backend.h"
#include "rsp/audio/hle.h"
#include "rsp/audio/selftest.h"

#define ALIST  0x100000u  /* la lista de comandos */
#define SRC    0x200000u
#define SRC2   0x200100u
#define DST    0x200200u
#define STATE  0x200400u
#define BOOK   0x200600u

static struct aud_state st;
static struct aud_result res;
static uint32_t nlist;

static uint8_t *ram(uint32_t pa)
{
	return t_n64.bus.rdram + pa;
}

static void put16(uint32_t pa, int16_t v)
{
	salsa64_wr16(ram(pa), (uint16_t)v);
}

static int16_t get16(uint32_t pa)
{
	return (int16_t)salsa64_rd16(ram(pa));
}

static void cmd(uint32_t w1, uint32_t w2)
{
	t_wr32(ALIST + 8 * nlist, w1);
	t_wr32(ALIST + 8 * nlist + 4, w2);
	nlist++;
}

static uint32_t uc_data, uc_size;  /* ucode_data y su tamaño del trabajo (0: sin segmento de datos) */
static uint32_t job_flags;

static void run_abi(enum salsa64_abi abi)
{
	struct aud_job job;
	memset(&job, 0, sizeof job);
	job.ucode_data = uc_data;
	job.ucode_data_size = uc_size;
	job.flags = job_flags;
	job.rdram_ea = (uint64_t)(uintptr_t)t_n64.bus.rdram;
	job.rdram_size = t_n64.bus.rdram_size;
	job.abi = abi;
	job.alist = ALIST;
	job.alist_size = 8 * nlist;
	aud_run(&st, &job, &res);
	nlist = 0;
}

static void fresh(void)
{
	aud_init(&st);
	nlist = 0;
	job_flags = 0;
}

/* ---- Codificación de nead (Zelda OoT) -------------------------------------- */

static void n_loadbuff(uint16_t dmem, uint32_t addr, uint16_t count)
{
	cmd(0x14000000u | (uint32_t)count << 12 | dmem, addr);
}

static void n_savebuff(uint16_t dmem, uint32_t addr, uint16_t count)
{
	cmd(0x15000000u | (uint32_t)count << 12 | dmem, addr);
}

static void n_setbuff(uint16_t in, uint16_t out, uint16_t count)
{
	cmd(0x08000000u | in, (uint32_t)out << 16 | count);
}

/* ---- Reconocimiento del ABI ------------------------------------------------- */

static enum salsa64_abi detect_with(uint32_t w0, uint32_t w10, uint32_t w28, uint32_t w30)
{
	struct salsa64_task task;
	memset(&task, 0, sizeof task);
	task.type = SALSA64_TASK_AUDIO;
	task.ucode_data = 0x80300000u;
	memset(ram(0x300000), 0, 0x40);
	t_wr32(0x300000, w0);
	t_wr32(0x300010, w10);
	t_wr32(0x300028, w28);
	t_wr32(0x300030, w30);
	return salsa64_rsp_detect_abi(&t_n64.bus, &task);
}

static void detect_abi(void)
{
	CHECK_EQ(detect_with(1, 0, 0x1E24138C, 0xF0000F00), SALSA64_ABI_AUDIO);
	CHECK_EQ(detect_with(1, 0, 0x1DC8138C, 0xF0000F00), SALSA64_ABI_AUDIO_GE);
	CHECK_EQ(detect_with(1, 0, 0x12345678, 0xF0000F00), SALSA64_ABI_UNKNOWN);
	CHECK_EQ(detect_with(1, 0x1F681230, 0, 0), SALSA64_ABI_NEAD_OOT);
	CHECK_EQ(detect_with(1, 0x11181350, 0, 0), SALSA64_ABI_NEAD_MK);
	CHECK_EQ(detect_with(1, 0x110412CC, 0, 0), SALSA64_ABI_NEAD_SF);
	CHECK_EQ(detect_with(1, 0x111812E0, 0, 0), SALSA64_ABI_NEAD_SFJ);
	CHECK_EQ(detect_with(0, 0x1AE8143C, 0, 0), SALSA64_ABI_NAUDIO_MP3);
	CHECK_EQ(detect_with(0, 0x0000127C, 0, 0), SALSA64_ABI_NAUDIO);
	CHECK_EQ(detect_with(0, 0x00001280, 0, 0), SALSA64_ABI_NAUDIO_BK);
	CHECK_EQ(detect_with(0, 0x1C58126C, 0, 0), SALSA64_ABI_NAUDIO_DK);
	CHECK_EQ(detect_with(0, 0x1AB0140C, 0, 0), SALSA64_ABI_NAUDIO_CBFD);
	CHECK_EQ(detect_with(0, 0x00000001, 0, 0), SALSA64_ABI_MUSYX1);
	CHECK_EQ(detect_with(0x00010010, 0x00000001, 0, 0), SALSA64_ABI_MUSYX1);
	CHECK_EQ(detect_with(0xF0000F00, 0x00000001, 0, 0), SALSA64_ABI_MUSYX1_RS);
	CHECK_EQ(detect_with(1, 0x00010010, 0, 0), SALSA64_ABI_MUSYX2);
	CHECK_EQ(detect_with(0, 0, 0, 0), SALSA64_ABI_UNKNOWN);
	/* Las ROMs de prueba de Salsa64. */
	struct salsa64_task task;
	memset(&task, 0, sizeof task);
	task.ucode_data = 0x300000;
	memset(ram(0x300000), 0, 0x40);
	memcpy(ram(0x300000), "Salsa64 ABI NEAD", 16);
	CHECK_EQ(salsa64_rsp_detect_abi(&t_n64.bus, &task), SALSA64_ABI_NEAD_OOT);
	memcpy(ram(0x300000), "Salsa64 ABI1\0\0\0\0", 16);
	CHECK_EQ(salsa64_rsp_detect_abi(&t_n64.bus, &task), SALSA64_ABI_AUDIO);
	/* Fuera de la RDRAM. */
	task.ucode_data = 0x7FFFF0;
	CHECK_EQ(salsa64_rsp_detect_abi(&t_n64.bus, &task), SALSA64_ABI_UNKNOWN);
}

/* ---- nead -------------------------------------------------------------------- */

static void nead_load_mix_save(void)
{
	fresh();
	for (int i = 0; i < 16; i++) {
		put16(SRC + 2 * i, (int16_t)(i * 1000 - 8000));
		put16(SRC2 + 2 * i, 30000);
	}
	n_loadbuff(0x400, SRC, 32);
	n_loadbuff(0x500, SRC2, 32);
	cmd(0x0C000000u | 32u << 12 | 0x4000, 0x0400u << 16 | 0x500);  /* MIXER ×0,5 */
	n_savebuff(0x500, DST, 32);
	run_abi(SALSA64_ABI_NEAD_OOT);
	CHECK_EQ(res.commands, 4);
	CHECK_EQ(res.errors, 0);
	for (int i = 0; i < 16; i++) {
		/* MIXER acumula como VMACF: lo que había × 0x7FFF más el producto, con un redondeo. */
		int32_t s = i * 1000 - 8000, want = (30000 * 0x7FFF + s * 0x4000 + 0x4000) >> 15;
		CHECK_EQ(get16(DST + 2 * i), want > 32767 ? 32767 : want);
	}
	CHECK_EQ(res.nranges, 1);
	CHECK_EQ(res.range_addr[0], DST);
	CHECK_EQ(res.range_len[0], 32);
}

static void nead_buffer_moves(void)
{
	fresh();
	for (int i = 0; i < 64; i++)
		put16(SRC + 2 * i, (int16_t)(i + 1));
	n_loadbuff(0x100, SRC, 128);
	cmd(0x02000000u | 0x300, 64);                           /* CLEARBUFF */
	cmd(0x0A000000u | 0x100, 0x0300u << 16 | 6);            /* DMEMMOVE: 6 → 8 bytes */
	cmd(0x09000000u | 2u << 16 | 0x100, 0x0400u << 16);     /* DUPLICATE 128 B × 2 */
	cmd(0x11000000u | 4, 0x0100u << 16 | 0x600);            /* INTERL: 4 muestras pares */
	cmd(0x04000000u | 16u << 12, 0x0100u << 16 | 0x0400);   /* ADDMIXER 16 bytes */
	cmd(0x0E000000u | 0x20u << 16 | 8, 0x0100u << 16);      /* HILOGAIN ×2 (Q4.4), 8 bytes */
	n_savebuff(0x300, DST, 16);
	n_savebuff(0x400, DST + 16, 16);
	n_savebuff(0x600, DST + 32, 8);
	n_savebuff(0x100, DST + 48, 8);
	n_savebuff(0x480, DST + 64, 8);
	run_abi(SALSA64_ABI_NEAD_OOT);
	CHECK_EQ(res.errors, 0);
	/* DMEMMOVE copió 8 bytes (4 muestras) y el resto quedó en cero. */
	CHECK_EQ(get16(DST + 0), 1);
	CHECK_EQ(get16(DST + 6), 4);
	CHECK_EQ(get16(DST + 8), 0);
	/* ADDMIXER: 0x400 = duplicado de 0x100 más 0x100 → 2 × (i + 1). */
	CHECK_EQ(get16(DST + 16), 2);
	CHECK_EQ(get16(DST + 30), 16);
	/* INTERL: muestras 0, 2, 4, 6 → 1, 3, 5, 7. */
	CHECK_EQ(get16(DST + 32), 1);
	CHECK_EQ(get16(DST + 34), 3);
	CHECK_EQ(get16(DST + 38), 7);
	/* HILOGAIN ×2 sobre las 4 primeras de 0x100. */
	CHECK_EQ(get16(DST + 48), 2);
	CHECK_EQ(get16(DST + 54), 8);
	/* La segunda copia de DUPLICATE (0x480) no pasó por ADDMIXER. */
	CHECK_EQ(get16(DST + 64), 1);
	CHECK_EQ(get16(DST + 70), 4);
	/* Cinco SAVEBUFF: los tres primeros, contiguos, se juntan. */
	CHECK_EQ(res.nranges, 3);
	CHECK_EQ(res.range_addr[0], DST);
	CHECK_EQ(res.range_len[0], 40);
}

static void nead_interleave(void)
{
	fresh();
	for (int i = 0; i < 8; i++) {
		put16(SRC + 2 * i, (int16_t)(100 + i));
		put16(SRC2 + 2 * i, (int16_t)(-100 - i));
	}
	n_loadbuff(0x100, SRC, 16);
	n_loadbuff(0x200, SRC2, 16);
	cmd(0x0D000000u | 16u << 12 | 0x300, 0x0100u << 16 | 0x200);
	n_savebuff(0x300, DST, 32);
	run_abi(SALSA64_ABI_NEAD_OOT);
	for (int i = 0; i < 8; i++) {
		CHECK_EQ(get16(DST + 4 * i), 100 + i);
		CHECK_EQ(get16(DST + 4 * i + 2), -100 - i);
	}
}

static void nead_envmixer(void)
{
	fresh();
	for (int i = 0; i < 16; i++) {
		put16(SRC + 2 * i, 10000);
		put16(SRC2 + 2 * i, 100);
	}
	n_loadbuff(0x100, SRC, 32);
	for (uint16_t b = 0x200; b <= 0x500; b += 0x100)
		n_loadbuff(b, SRC2, 32);
	/* env_values[2] = 0x8000, paso 0; pasos 0 y 0x1000 para izquierda y derecha. */
	cmd(0x12000000u | 0x80u << 16, 0x0000u << 16 | 0x1000);
	cmd(0x16000000u, 0x8000u << 16 | 0x4000);               /* izquierda 0,5; derecha 0,25 */
	/* ENVMIXER: entrada 0x100, 16 muestras; secas 0x200/0x300, efecto 0x400/0x500. */
	cmd(0x13000000u | 0x10u << 16 | 16u << 8, 0x20u << 24 | 0x30u << 16 | 0x40u << 8 | 0x50);
	n_savebuff(0x200, DST, 32);
	n_savebuff(0x300, DST + 32, 32);
	n_savebuff(0x400, DST + 64, 32);
	n_savebuff(0x500, DST + 96, 32);
	run_abi(SALSA64_ABI_NEAD_OOT);
	CHECK_EQ(res.errors, 0);
	/* Primeras 8: l = 10000·0x8000 >> 16 = 5000, r = 2500; efecto ×0,5. */
	CHECK_EQ(get16(DST + 0), 5100);
	CHECK_EQ(get16(DST + 32), 2600);
	CHECK_EQ(get16(DST + 64), 2600);
	CHECK_EQ(get16(DST + 96), 1350);
	/* Siguientes 8: la derecha sube 0x1000 → 0x5000: r = 3125, r2 = 1562. */
	CHECK_EQ(get16(DST + 16), 5100);
	CHECK_EQ(get16(DST + 48), 3225);
	CHECK_EQ(get16(DST + 112), 1662);
	CHECK_EQ(st.nead.env_values[1], 0x6000);

	/* Bits 0 y 1: inversión de las secas (XOR con −1). */
	n_loadbuff(0x200, SRC2, 32);
	cmd(0x16000000u, 0x8000u << 16 | 0x4000);
	cmd(0x13000000u | 0x10u << 16 | 8u << 8 | 0x3, 0x20u << 24 | 0x30u << 16 | 0x40u << 8 | 0x50);
	n_savebuff(0x200, DST, 16);
	run_abi(SALSA64_ABI_NEAD_OOT);
	CHECK_EQ(get16(DST + 0), 100 + (5000 ^ -1));
}

static void nead_resample(void)
{
	fresh();
	for (int i = 0; i < 16; i++)
		put16(SRC + 2 * i, 1000);
	n_loadbuff(0x108, SRC, 32);
	n_setbuff(0x108, 0x200, 32);
	cmd(0x05000000u | 0x01u << 16 | 0x8000, STATE);  /* pitch 1,0, A_INIT */
	n_savebuff(0x200, DST, 32);
	run_abi(SALSA64_ABI_NEAD_OOT);
	CHECK_EQ(res.errors, 0);
	/* Con pitch 1,0 la fase queda en 0: la salida k mira las entradas
	 * k−4 … k−1 (cero antes del búfer) con la fila 0 de la tabla. */
	int32_t c0 = aud_resample_lut[0], c1 = aud_resample_lut[1];
	CHECK_EQ(get16(DST + 0), 0);
	/* Cada producto se redondea por separado, como el microcódigo. */
	CHECK_EQ(get16(DST + 4), (1000 * c0 + 0x4000) >> 15);
	CHECK_EQ(get16(DST + 6), ((1000 * c0 + 0x4000) >> 15) + ((1000 * c1 + 0x4000) >> 15));
	CHECK_EQ(get16(DST + 8), 1000);
	CHECK_EQ(get16(DST + 30), 1000);
	/* Estado: las 4 últimas entradas y el acumulador. */
	CHECK_EQ(get16(STATE + 0), 1000);
	CHECK_EQ(get16(STATE + 6), 1000);
	CHECK_EQ(get16(STATE + 8), 0);

	/* Continuación a la mitad del tono: una entrada constante sale igual
	 * en todas las fases (cada fila suma 32768). */
	n_loadbuff(0x108, SRC, 32);
	n_setbuff(0x108, 0x200, 32);
	cmd(0x05000000u | 0x4000, STATE);
	n_savebuff(0x200, DST, 32);
	run_abi(SALSA64_ABI_NEAD_OOT);
	for (int i = 0; i < 16; i++)
		CHECK_EQ(get16(DST + 2 * i), 1000);
	CHECK_EQ(get16(STATE + 8), 0);  /* 16 × 0,5 = 8 muestras enteras */
}

static void lut_rows_sum_to_one(void)
{
	for (int p = 0; p < 64; p++) {
		const int16_t *r = aud_resample_lut + 4 * p;
		CHECK_EQ(r[0] + r[1] + r[2] + r[3], 32768);
	}
	/* Simétrica: la fase p y la 64 − p son espejo. */
	for (int p = 1; p < 64; p++)
		CHECK_EQ(aud_resample_lut[4 * p], aud_resample_lut[4 * (64 - p) + 3]);
}

/* ADPCM con 4 bits por muestra: byte de escala/predictor y 8 bytes. */
static void put_adpcm_frame(uint32_t pa, uint8_t code)
{
	static const uint8_t data[8] = { 0x12, 0x34, 0x56, 0x70, 0x9A, 0xBC, 0xDE, 0xF0 };
	ram(pa)[0] = code;
	memcpy(ram(pa + 1), data, 8);
}

static const int8_t nibbles[16] = { 1, 2, 3, 4, 5, 6, 7, 0, -7, -6, -5, -4, -3, -2, -1, 0 };

static void nead_adpcm(void)
{
	fresh();
	memset(ram(BOOK), 0, 256);
	put_adpcm_frame(SRC, 0x00);    /* escala 0: el residuo es el nibble */
	put_adpcm_frame(SRC + 9, 0xC0);  /* escala 12: nibble << 12 */
	memset(ram(SRC + 18), 0, 14);
	cmd(0x0B000000u | 256, BOOK);  /* LOADADPCM: libro en cero */
	n_loadbuff(0x100, SRC, 32);
	n_setbuff(0x100, 0x200, 64);   /* dos cuadros */
	cmd(0x01000000u | 0x01u << 16, STATE);  /* ADPCM A_INIT */
	n_savebuff(0x200, DST, 96);
	run_abi(SALSA64_ABI_NEAD_OOT);
	CHECK_EQ(res.errors, 0);
	/* Primero las 16 muestras previas (cero), luego 16 por cuadro. */
	CHECK_EQ(get16(DST + 0), 0);
	for (int i = 0; i < 16; i++) {
		CHECK_EQ(get16(DST + 32 + 2 * i), nibbles[i]);
		CHECK_EQ(get16(DST + 64 + 2 * i), nibbles[i] * 4096);
	}
	/* El estado queda con el último cuadro. */
	CHECK_EQ(get16(STATE + 0), 4096);
	CHECK_EQ(get16(STATE + 28), -1 * 4096);

	/* Predictor: book1[0] = 1,0 (Q11) suma la penúltima muestra previa a
	 * la primera de cada mitad. */
	memset(ram(BOOK), 0, 256);
	put16(BOOK, 2048);
	for (int i = 0; i < 16; i++)
		put16(STATE + 2 * i, (int16_t)(i == 14 ? 100 : i == 15 ? 200 : 0));
	cmd(0x0B000000u | 256, BOOK);
	n_loadbuff(0x100, SRC, 16);
	n_setbuff(0x100, 0x200, 32);
	cmd(0x01000000u, STATE);  /* continúa desde el estado */
	n_savebuff(0x200, DST, 64);
	run_abi(SALSA64_ABI_NEAD_OOT);
	CHECK_EQ(get16(DST + 28), 100);              /* las previas, tal cual */
	CHECK_EQ(get16(DST + 32), 1 + 100);          /* 1.ª mitad: + último[14] */
	CHECK_EQ(get16(DST + 34), 2);
	CHECK_EQ(get16(DST + 48), nibbles[8] + 7);   /* 2.ª mitad: + nueva[6] */
}

static void nead_filter(void)
{
	fresh();
	/* Dos tablas cuyo promedio es un retardo de una muestra (L[1] = 1,0). */
	for (int i = 0; i < 8; i++) {
		put16(BOOK + 2 * i, (int16_t)(i == 1 ? 0x7FFF : 0));
		put16(STATE + 0x10 + 2 * i, (int16_t)(i == 1 ? 0x7FFF : 0));
		put16(STATE + 2 * i, (int16_t)(i == 7 ? 555 : 0));  /* muestras previas */
	}
	for (int i = 0; i < 16; i++)
		put16(SRC + 2 * i, (int16_t)(i + 1));
	n_loadbuff(0x100, SRC, 32);
	cmd(0x07000000u | 0x02u << 16 | 32, BOOK);   /* cuenta y primera tabla */
	cmd(0x07000000u | 0x100, STATE);             /* filtra (segunda tabla en +0x10) */
	n_savebuff(0x100, DST, 32);
	run_abi(SALSA64_ABI_NEAD_OOT);
	CHECK_EQ(res.errors, 0);
	/* out[n] = x[n − 1] · 0x7FFF / 32768, redondeado. */
	CHECK_EQ(get16(DST + 0), 555);
	CHECK_EQ(get16(DST + 2), 1);
	CHECK_EQ(get16(DST + 30), 15);
	/* Estado: el último bloque de entrada. */
	CHECK_EQ(get16(STATE + 14), 16);

	/* Tablas: la anterior (detrás del estado) va a la mitad de camino de la
	 * de la nota, redondeando hacia arriba; la de la nota no cambia. Tres
	 * llamadas desde cero dejan lo que deja el microcódigo de Zelda en el
	 * LLE (tarea 31 de su música): 0 → 490 → 735 → 858. */
	static const int16_t note[8] = { 980, -3081, 4883, 14286, 4883, -3081, 980, 590 };
	static const int16_t after3[8] = { 858, -2695, 4273, 12501, 4273, -2695, 858, 517 };
	for (int i = 0; i < 8; i++) {
		put16(BOOK + 2 * i, note[i]);
		put16(STATE + 0x10 + 2 * i, 0);
	}
	for (int k = 0; k < 3; k++) {
		cmd(0x07000000u | 0x02u << 16 | 32, BOOK);
		cmd(0x07000000u | 0x100, STATE);
	}
	run_abi(SALSA64_ABI_NEAD_OOT);
	for (int i = 0; i < 8; i++) {
		CHECK_EQ(get16(STATE + 0x10 + 2 * i), after3[i]);
		CHECK_EQ(get16(BOOK + 2 * i), note[i]);
	}
}

static void nead_errors_and_unknown(void)
{
	fresh();
	n_savebuff(0x100, 0x7FFFF8, 16);  /* se sale de los 8 MB */
	cmd(0x00000000u, 0);              /* el comando 0 de OoT no existe */
	cmd(0x7F000000u, 0);              /* fuera de la tabla */
	run_abi(SALSA64_ABI_NEAD_OOT);
	CHECK_EQ(res.commands, 3);
	CHECK_EQ(res.errors, 1);
	CHECK_EQ(res.unknown, 2);
	CHECK_EQ(res.last_unknown, 0x7F000000u);
	CHECK_EQ(res.nranges, 0);

	/* Un ABI sin HLE no ejecuta nada. */
	n_savebuff(0x100, DST, 16);
	run_abi(SALSA64_ABI_UNKNOWN);
	CHECK_EQ(res.commands, 0);
	CHECK_EQ(res.errors, 1);
}

static void ranges_merge_when_full(void)
{
	fresh();
	for (uint32_t i = 0; i < AUD_MAX_RANGES + 2; i++)
		n_savebuff(0x100, DST + 0x100 * i, 8);
	run_abi(SALSA64_ABI_NEAD_OOT);
	CHECK_EQ(res.nranges, AUD_MAX_RANGES);
	uint32_t covered = 0;
	for (uint32_t k = 0; k < AUD_MAX_RANGES + 2; k++) {
		uint32_t addr = DST + 0x100 * k;
		bool hit = false;
		for (uint32_t i = 0; i < res.nranges; i++)
			hit |= res.range_addr[i] <= addr && res.range_addr[i] + res.range_len[i] >= addr + 8;
		covered += hit;
	}
	CHECK_EQ(covered, AUD_MAX_RANGES + 2);
	uint32_t span = 0;
	for (uint32_t i = 0; i < res.nranges; i++)
		span += res.range_len[i];
	CHECK(span < 0x400);

	/* El interruptor conserva exactamente la aproximacion anterior. */
	fresh();
	job_flags = AUD_JOB_RANGES_OLD;
	for (uint32_t i = 0; i < AUD_MAX_RANGES + 2; i++)
		n_savebuff(0x100, DST + 0x100 * i, 8);
	run_abi(SALSA64_ABI_NEAD_OOT);
	uint32_t last = AUD_MAX_RANGES - 1;
	CHECK_EQ(res.range_addr[last], DST + 0x100 * last);
	CHECK_EQ(res.range_addr[last] + res.range_len[last], DST + 0x100 * (AUD_MAX_RANGES + 1) + 8);
}

/* ---- ABI1 (Super Mario 64) --------------------------------------------------- */

static void abi1_segments_envmixer(void)
{
	fresh();
	for (int i = 0; i < 16; i++)
		put16(SRC + 2 * i, 1000);
	cmd(0x07000000u, 0x05u << 24 | SRC);           /* SEGMENT 5 → SRC */
	cmd(0x08000000u | 0x000, 0x0100u << 16 | 32);  /* SETBUFF in 0x5C0, out 0x6C0, 32 bytes */
	cmd(0x04000000u, 0x05u << 24);                 /* LOADBUFF desde el segmento 5 */
	cmd(0x02000000u | 0x100, 32);                  /* CLEARBUFF dl */
	cmd(0x02000000u | 0x200, 32);                  /* CLEARBUFF dr */
	cmd(0x08000000u | 0x08u << 16 | 0x200, 0x0300u << 16 | 0x400);  /* SETBUFF A_AUX: dr, wl, wr */
	cmd(0x09000000u | 0x08u << 16 | 0x7FFF, 0);    /* SETVOL A_AUX: dry 0x7FFF, wet 0 */
	cmd(0x09000000u | 0x06u << 16 | 0x4000, 0);    /* izquierda, volumen 0,5 */
	cmd(0x09000000u | 0x02u << 16 | 0x4000, 0x10000);  /* izquierda, destino 0,5 */
	cmd(0x09000000u | 0x04u << 16 | 0x2000, 0);    /* derecha, volumen 0,25 */
	cmd(0x09000000u | 0x00u << 16 | 0x2000, 0x10000);
	cmd(0x03000000u | 0x01u << 16, 0x05u << 24 | (STATE - SRC));  /* ENVMIXER A_INIT */
	cmd(0x06000000u, 0x05u << 24 | (DST - SRC));   /* SAVEBUFF out (dl) */
	cmd(0x08000000u | 0x000, 0x0200u << 16 | 32);
	cmd(0x06000000u, 0x05u << 24 | (DST + 32 - SRC));  /* SAVEBUFF dr */
	run_abi(SALSA64_ABI_AUDIO);
	CHECK_EQ(res.errors, 0);
	CHECK_EQ(res.unknown, 0);
	/* Ganancias: (0x4000 · 0x7FFF + 0x4000) >> 15 = 0x4000 y (0x2000 · 0x7FFF
	 * + 0x4000) >> 15 = 0x2000: 1000 → 500 y 250. */
	for (int i = 0; i < 16; i++) {
		CHECK_EQ(get16(DST + 2 * i), 500);
		CHECK_EQ(get16(DST + 32 + 2 * i), 250);
	}
	/* Sin A_INIT sigue desde el estado guardado: mismas ganancias. */
	cmd(0x07000000u, 0x05u << 24 | SRC);
	cmd(0x08000000u | 0x000, 0x0100u << 16 | 32);
	cmd(0x04000000u, 0x05u << 24);
	cmd(0x02000000u | 0x100, 32);
	cmd(0x03000000u, 0x05u << 24 | (STATE - SRC));
	cmd(0x06000000u, 0x05u << 24 | (DST - SRC));
	run_abi(SALSA64_ABI_AUDIO);
	CHECK_EQ(get16(DST + 0), 500);
	CHECK_EQ(get16(DST + 30), 500);
	/* Los segmentos se borran en cada tarea. */
	CHECK_EQ(st.abi1.segments[5], SRC);
}

static void abi1_mixer_interleave(void)
{
	fresh();
	for (int i = 0; i < 16; i++) {
		put16(SRC + 2 * i, (int16_t)(2 * i));
		put16(SRC2 + 2 * i, (int16_t)(-2 * i));
	}
	cmd(0x08000000u | 0x000, 0x0100u << 16 | 32);
	cmd(0x04000000u, SRC);                         /* segmento 0: dirección física */
	cmd(0x08000000u | 0x200, 0x0100u << 16 | 32);
	cmd(0x04000000u, SRC2);                        /* 0x7C0 = derecha */
	cmd(0x0C000000u | 0x7FFF, 0x0000u << 16 | 0x200);  /* MIXER: derecha += izquierda */
	cmd(0x08000000u | 0x000, 0x0300u << 16 | 32);
	cmd(0x0D000000u, 0x0000u << 16 | 0x200);       /* INTERLEAVE → 0x8C0 */
	cmd(0x08000000u | 0x000, 0x0300u << 16 | 64);
	cmd(0x06000000u, DST);
	run_abi(SALSA64_ABI_AUDIO);
	CHECK_EQ(res.errors, 0);
	for (int i = 0; i < 16; i++) {
		int32_t l = 2 * i, r = -2 * i + ((l * 0x7FFF + 0x4000) >> 15);  /* redondeado */
		CHECK_EQ(get16(DST + 4 * i), l);
		CHECK_EQ(get16(DST + 4 * i + 2), r);
	}
}

/* ---- naudio (Banjo-Kazooie, Conker, Perfect Dark…) --------------------------
 * Direcciones de la DMEM relativas a 0x4F0; casi todo sobre 0x170 bytes. */

#define NA_N      184     /* muestras por búfer */
#define NA_MAIN2  0x170   /* 0x660 */
#define NA_DL     0x4E0   /* 0x9D0 */
#define NA_DR     0x650   /* 0xB40 */
#define NA_WL     0x7C0   /* 0xCB0 */
#define NA_WR     0x930   /* 0xE20 */

/* En la RDRAM, con lugar para 0x2E0 bytes por búfer. */
#define NA_SRC    0x220000u
#define NA_SRC2   0x221000u
#define NA_DST    0x222000u
#define NA_STATE  0x224000u
#define NA_BOOK   0x225000u

static void na_loadbuff(uint16_t rel, uint32_t addr, uint16_t count)
{
	cmd(0x04000000u | (uint32_t)count << 12 | rel, addr);
}

static void na_savebuff(uint16_t rel, uint32_t addr, uint16_t count)
{
	cmd(0x06000000u | (uint32_t)count << 12 | rel, addr);
}

static void put_const(uint32_t pa, unsigned n, int16_t v)
{
	for (unsigned i = 0; i < n; i++)
		put16(pa + 2 * i, v);
}

static void naudio_load_mix_save(void)
{
	fresh();
	for (int i = 0; i < NA_N; i++)
		put16(NA_SRC + 2 * i, (int16_t)(i * 100 - 9000));
	put_const(NA_SRC2, NA_N, 1000);
	na_loadbuff(0, NA_SRC, 0x170);
	na_loadbuff(NA_MAIN2, NA_SRC2, 0x170);
	cmd(0x0C000000u | 0x4000, 0x0000u << 16 | NA_MAIN2);  /* MIXER ×0,5: MAIN → MAIN2 */
	cmd(0x02000000u | (NA_MAIN2 + 0x10), 16);           /* CLEARBUFF de 8 muestras */
	cmd(0x0A000000u | 0x002, 0x0300u << 16 | 6);         /* DMEMMOVE 6 → 8 bytes */
	na_savebuff(NA_MAIN2, NA_DST, 0x170);
	na_savebuff(0x300, NA_DST + 0x200, 16);
	run_abi(SALSA64_ABI_NAUDIO);
	CHECK_EQ(res.commands, 7);
	CHECK_EQ(res.errors, 0);
	CHECK_EQ(res.unknown, 0);
	for (int i = 0; i < NA_N; i++) {
		int32_t x = i * 100 - 9000;
		int16_t want = (int16_t)(1000 + ((x * 0x4000) >> 15));
		if (i >= 8 && i < 16)
			want = 0;
		CHECK_EQ(get16(NA_DST + 2 * i), want);
	}
	/* DMEMMOVE desde la muestra 1: 4 muestras y el resto en cero. */
	CHECK_EQ(get16(NA_DST + 0x200), -8900);
	CHECK_EQ(get16(NA_DST + 0x206), -8600);
	CHECK_EQ(get16(NA_DST + 0x208), 0);
}

/* SETVOL: izquierda volumen + seco/efecto, derecha destino/velocidad,
 * izquierda destino/velocidad, y el 0x0E que cambia la parte baja. */
static void na_setvol(uint8_t flags, uint16_t v, uint32_t w2)
{
	cmd(0x09000000u | (uint32_t)flags << 16 | v, w2);
}

static void naudio_envmixer_interleave(void)
{
	fresh();
	put_const(NA_SRC, NA_N, 10000);
	put_const(NA_SRC2, NA_N, 100);
	na_setvol(0x06, 0x4000, 0x7FFFu << 16 | 0x2000);  /* vol izq. 0,5; seco 1; efecto 0,25 */
	na_setvol(0x04, 0x2000, 0x00012345);               /* destino der. 0,25 */
	cmd(0x0E000000u, 0x0000BEEF);                      /* parte baja de la velocidad der. */
	na_setvol(0x00, 0x4000, 0);                        /* destino izq. 0,5 */
	run_abi(SALSA64_ABI_NAUDIO);
	CHECK_EQ(st.naudio.vol[0], 0x4000);
	CHECK_EQ(st.naudio.dry, 0x7FFF);
	CHECK_EQ(st.naudio.wet, 0x2000);
	CHECK_EQ(st.naudio.target[1], 0x2000);
	CHECK_EQ(st.naudio.rate[1], 0x0001BEEF);
	CHECK_EQ(st.naudio.target[0], 0x4000);

	/* Volúmenes constantes: izquierda 0,5, derecha 0,25 (velocidades 0). */
	na_setvol(0x04, 0x2000, 0);
	for (int pass = 0; pass < 2; pass++) {
		na_loadbuff(0, NA_SRC, 0x170);
		na_loadbuff(NA_DL, NA_SRC2, 0x170);
		na_loadbuff(NA_DR, NA_SRC2, 0x170);
		na_loadbuff(NA_WL, NA_SRC2, 0x170);
		na_loadbuff(NA_WR, NA_SRC2, 0x170);
		/* ENVMIXER: volumen derecho en w1; A_INIT la primera vez. */
		cmd(0x03000000u | (pass ? 0u : 0x01u << 16) | 0x2000, NA_STATE);
		na_savebuff(NA_DL, NA_DST, 0x170);
		na_savebuff(NA_DR, NA_DST + 0x200, 0x170);
		na_savebuff(NA_WL, NA_DST + 0x400, 0x170);
		na_savebuff(NA_WR, NA_DST + 0x600, 0x170);
		run_abi(SALSA64_ABI_NAUDIO);
		CHECK_EQ(res.errors, 0);
		/* Ganancias (vol · seco o efecto + 0x4000) >> 15: 0x4000, 0x2000,
		 * 0x1000 y 0x800 → 10000 sale 5000, 2500, 1250 y 625. */
		for (int i = 0; i < NA_N; i += 61) {
			CHECK_EQ(get16(NA_DST + 2 * i), 5100);
			CHECK_EQ(get16(NA_DST + 0x200 + 2 * i), 2600);
			CHECK_EQ(get16(NA_DST + 0x400 + 2 * i), 1350);
			CHECK_EQ(get16(NA_DST + 0x600 + 2 * i), 725);
		}
		CHECK_EQ(get16(NA_DST + 2 * (NA_N - 1)), 5100);
	}

	/* INTERLEAVE: secos izquierdo y derecho → MAIN (368 muestras). */
	for (int i = 0; i < NA_N; i++) {
		put16(NA_SRC + 2 * i, (int16_t)i);
		put16(NA_SRC2 + 2 * i, (int16_t)-i);
	}
	na_loadbuff(NA_DL, NA_SRC, 0x170);
	na_loadbuff(NA_DR, NA_SRC2, 0x170);
	cmd(0x0D000000u, 0);
	na_savebuff(0, NA_DST, 0x2E0);
	run_abi(SALSA64_ABI_NAUDIO);
	for (int i = 0; i < NA_N; i++) {
		CHECK_EQ(get16(NA_DST + 4 * i), i);
		CHECK_EQ(get16(NA_DST + 4 * i + 2), -i);
	}
}

/* ADPCM: estado en w1; banderas, cuenta, entrada (4 bits) y salida en w2. */
static void na_adpcm(uint8_t flags, uint32_t state, uint16_t count, uint8_t in_rel, uint16_t out_rel)
{
	cmd(0x01000000u | state, (uint32_t)flags << 28 | (uint32_t)count << 16 | (uint32_t)in_rel << 12 | out_rel);
}

static void naudio_adpcm_resample(void)
{
	fresh();
	memset(ram(NA_BOOK), 0, 256);
	put_adpcm_frame(NA_SRC, 0x00);
	put_adpcm_frame(NA_SRC + 9, 0xC0);
	memset(ram(NA_SRC + 18), 0, 14);
	cmd(0x0B000000u | 256, NA_BOOK);
	na_loadbuff(0, NA_SRC, 32);
	na_adpcm(0x1, NA_STATE, 64, 0, 0x200);  /* A_INIT */
	na_savebuff(0x200, NA_DST, 96);
	run_abi(SALSA64_ABI_NAUDIO_BK);
	CHECK_EQ(res.errors, 0);
	CHECK_EQ(res.unknown, 0);
	CHECK_EQ(get16(NA_DST + 0), 0);
	for (int i = 0; i < 16; i++) {
		CHECK_EQ(get16(NA_DST + 32 + 2 * i), nibbles[i]);
		CHECK_EQ(get16(NA_DST + 64 + 2 * i), nibbles[i] * 4096);
	}
	CHECK_EQ(get16(NA_STATE + 28), -1 * 4096);

	/* A_LOOP: las muestras previas salen de la dirección de SETLOOP. */
	for (int i = 0; i < 16; i++)
		put16(NA_STATE + 0x40 + 2 * i, (int16_t)(i == 15 ? 321 : 0));
	cmd(0x0F000000u, NA_STATE + 0x40);
	na_loadbuff(4, NA_SRC, 16);
	na_adpcm(0x2, NA_STATE, 32, 4, 0x200);
	na_savebuff(0x200, NA_DST, 64);
	run_abi(SALSA64_ABI_NAUDIO_BK);
	CHECK_EQ(get16(NA_DST + 30), 321);
	CHECK_EQ(get16(NA_DST + 32), 1);

	/* RESAMPLE: estado en w1; A_INIT, tono (16 bits desde el 14), entrada
	 * (12 bits desde el 2) y salida MAIN2 (bits 0–1 ≠ 0), 184 muestras. */
	put_const(NA_SRC, 200, 1000);
	na_loadbuff(0x400, NA_SRC, 400);
	cmd(0x05000000u | NA_STATE, 0x1u << 30 | 0x8000u << 14 | 0x400u << 2 | 1);
	na_savebuff(NA_MAIN2, NA_DST, 0x170);
	run_abi(SALSA64_ABI_NAUDIO);
	CHECK_EQ(res.errors, 0);
	int32_t c0 = aud_resample_lut[0], c1 = aud_resample_lut[1];
	CHECK_EQ(get16(NA_DST + 0), 0);
	CHECK_EQ(get16(NA_DST + 4), (1000 * c0 + 0x4000) >> 15);
	CHECK_EQ(get16(NA_DST + 6), ((1000 * c0 + 0x4000) >> 15) + ((1000 * c1 + 0x4000) >> 15));
	CHECK_EQ(get16(NA_DST + 8), 1000);
	CHECK_EQ(get16(NA_DST + 2 * (NA_N - 1)), 1000);
	CHECK_EQ(get16(NA_STATE + 8), 0);
}

/* Los comandos 7 y 8: sin implementar en naudio y Banjo-Kazooie, MIXER en
 * Donkey Kong 64, MP3 y nada (MP3ADDY) en los de MP3. */
static void naudio_variant_tables(void)
{
	fresh();
	cmd(0x07000000u, 0);
	cmd(0x08000000u, 0);
	run_abi(SALSA64_ABI_NAUDIO);
	CHECK_EQ(res.unknown, 2);
	cmd(0x07000000u, 0);
	run_abi(SALSA64_ABI_NAUDIO_BK);
	CHECK_EQ(res.unknown, 1);
	cmd(0x08000000u, 0x12345678);
	run_abi(SALSA64_ABI_NAUDIO_CBFD);
	CHECK_EQ(res.unknown, 0);
	CHECK_EQ(res.errors, 0);

	put_const(NA_SRC, NA_N, 2000);
	put_const(NA_SRC2, NA_N, 10);
	na_loadbuff(0, NA_SRC, 0x170);
	na_loadbuff(NA_MAIN2, NA_SRC2, 0x170);
	cmd(0x07000000u | 0x4000, 0x0000u << 16 | NA_MAIN2);
	cmd(0x08000000u | 0x4000, 0x0000u << 16 | NA_MAIN2);
	na_savebuff(NA_MAIN2, NA_DST, 0x170);
	run_abi(SALSA64_ABI_NAUDIO_DK);
	CHECK_EQ(res.unknown, 0);
	CHECK_EQ(get16(NA_DST + 0), 2010);
	CHECK_EQ(get16(NA_DST + 2 * (NA_N - 1)), 2010);
}

/* Referencia directa del IIR de naudio: y[n] = c0·x[n] + c1·x[n−1] +
 * c0·x[n−2] + 2·c8·y[n−1] + 2·c9·y[n−2], cada producto en Q15 redondeado. */
static int32_t q15(int32_t c, int32_t x)
{
	return (c * x + 0x4000) >> 15;
}

static void naudio_filter(void)
{
	static const int16_t c[4] = { 0x3000, -0x2000, 0x2000, -0x0800 };  /* c0, c1, c8, c9 */
	int16_t x[2 * NA_N + 2], y[2 * NA_N + 2];
	fresh();
	memset(ram(NA_BOOK), 0, 32);
	put16(NA_BOOK + 0, c[0]);
	put16(NA_BOOK + 2, c[1]);
	put16(NA_BOOK + 16, c[2]);
	put16(NA_BOOK + 18, c[3]);
	for (int i = 0; i < 2 * NA_N; i++) {
		x[i + 2] = (int16_t)((i * 7919) % 6001 - 3000);
		put16(NA_SRC + 2 * i, x[i + 2]);
	}
	x[0] = x[1] = y[0] = y[1] = 0;
	for (int n = 2; n < 2 * NA_N + 2; n++)
		y[n] = (int16_t)(q15(c[0], x[n]) + q15(c[1], x[n - 1]) + q15(c[0], x[n - 2]) +
				 2 * q15(c[2], y[n - 1]) + 2 * q15(c[3], y[n - 2]));

	/* Dos bloques: el segundo sigue del estado de la RDRAM (x[n−2], x[n−1],
	 * y[n−2], y[n−1] en +0, +2, +4 y +6, como lo guarda el microcódigo). El IIR
	 * ignora el byte alto de w2 y filtra siempre desde el búfer principal. */
	cmd(0x0B000000u | 32, NA_BOOK);
	na_loadbuff(0, NA_SRC, 0x170);
	cmd(0x0E000000u | 0x01u << 16, 0x01u << 24 | NA_STATE);  /* A_INIT */
	na_savebuff(0, NA_DST, 0x170);
	na_loadbuff(0, NA_SRC + 0x170, 0x170);
	cmd(0x0E000000u, NA_STATE);
	na_savebuff(0, NA_DST + 0x170, 0x170);
	run_abi(SALSA64_ABI_NAUDIO_MP3);
	CHECK_EQ(res.errors, 0);
	CHECK_EQ(res.unknown, 0);
	for (int i = 0; i < 2 * NA_N; i++)
		CHECK_EQ(get16(NA_DST + 2 * i), y[i + 2]);
	CHECK_EQ(get16(NA_STATE + 0), x[2 * NA_N]);
	CHECK_EQ(get16(NA_STATE + 2), x[2 * NA_N + 1]);
	CHECK_EQ(get16(NA_STATE + 4), y[2 * NA_N]);
	CHECK_EQ(get16(NA_STATE + 6), y[2 * NA_N + 1]);

	/* Con c0 = c1 = 0 es POLEF: sin coeficientes, salida = x · ganancia (Q14),
	 * en el segundo búfer si w2 trae 1 en el byte alto. */
	memset(ram(NA_BOOK), 0, 32);
	cmd(0x0B000000u | 32, NA_BOOK);
	na_loadbuff(NA_MAIN2, NA_SRC, 0x170);
	for (unsigned i = 0; i < 16; i++)
		st.dmem[0x660 + i] = 0xAA;      /* lo que hay antes: los 16 bytes que guarda el POLEF */
	cmd(0x0E000000u | 0x01u << 16 | 0x2000, 0x01u << 24 | NA_STATE);
	na_savebuff(NA_MAIN2, NA_DST, 0x170);
	run_abi(SALSA64_ABI_NAUDIO_MP3);
	for (int i = 0; i < NA_N; i += 37)
		CHECK_EQ(get16(NA_DST + 2 * i), (x[i + 2] * 0x2000) >> 14);
	/* Primera vuelta del bucle: con A_INIT guarda un vector en cero antes del búfer, que son las
	 * 8 últimas muestras del principal (ahí no hay nada de la lista: st.dmem lo dejó la HLE). */
	for (unsigned i = 0; i < 16; i++)
		CHECK_EQ(st.dmem[0x660 + i], 0);
}

/* OVERLOAD (Conker): ganancia saturada y luego × atenuación / 65536. */
static void naudio_overload(void)
{
	fresh();
	static const int16_t in[4] = { 1000, 10000, -10000, -3 };
	for (int i = 0; i < 2 * NA_N; i++)
		put16(NA_SRC + 2 * i, in[i & 3]);
	na_loadbuff(0, NA_SRC, 0x2E0);
	cmd(0x00000000u, 0x8000u << 16 | 4);
	na_savebuff(0, NA_DST, 0x2E0);
	run_abi(SALSA64_ABI_NAUDIO_CBFD);
	CHECK_EQ(res.unknown, 0);
	static const int16_t out[4] = { 2000, 16383, -16384, -6 };
	for (int i = 0; i < NA_N; i++)
		CHECK_EQ(get16(NA_DST + 2 * i), out[i & 3]);
}

/* ---- MP3 ----------------------------------------------------------------------
 * Contra la síntesis de MPEG-1 (ISO 11172-3, 2.4.3.2.2) en coma flotante con
 * la misma ventana (D[] reconstruida de los bloques 0–16 de la tabla y la
 * simetría D[512 − i] = ±D[i] del estándar): la salida del RSP debe ser la
 * de referencia por una constante, salvo redondeos. */

#define MP3BUF 0x210000u

static double mp3_d(int i)
{
	/* Bloques 0–16: T[j][k] = D[32k + j]. */
	if (i % 32 <= 16)
		return aud_mp3_window[(i % 32) * 32 + i / 32] / 32768.0;
	double v = mp3_d(512 - i);
	return (i & 63) ? -v : v;
}

static void mp3_window_table(void)
{
	/* Cada bloque repite sus 16 coeficientes; el pico D[256] vale 32767. */
	for (int b = 0; b < 33; b++)
		for (int k = 0; k < 15; k++)
			CHECK_EQ(aud_mp3_window[32 * b + k], aud_mp3_window[32 * b + 16 + k]);
	CHECK_EQ(aud_mp3_window[8], 32767);
	/* D[64] = 0,003250122 y D[128] = 0,031082153 (ISO), escalados por
	 * 32767 / 1,144989014. */
	CHECK_EQ(aud_mp3_window[2], 93);
	CHECK_EQ(aud_mp3_window[4], 890);
}

static void mp3_matches_reference(void)
{
	enum { CMDS = 4, SLOTS = 18 * CMDS };
	static double vbuf[1024], ref[SLOTS * 32];
	static int16_t sub[SLOTS][32];
	fresh();
	memset(vbuf, 0, sizeof vbuf);
	uint32_t seed = 1;
	for (int t = 0; t < SLOTS; t++)
		for (int k = 0; k < 32; k++) {
			seed = seed * 1103515245u + 12345u;
			int amp = k < 4 ? 3000 : k < 12 ? 800 : 100;
			sub[t][k] = (int16_t)((int)((seed >> 8) % (2 * amp + 1)) - amp);
		}

	/* Referencia: V = N·S, U de V, W = U·D, salida = Σ W. */
	for (int t = 0; t < SLOTS; t++) {
		memmove(vbuf + 64, vbuf, 960 * sizeof vbuf[0]);
		for (int i = 0; i < 64; i++) {
			double acc = 0;
			for (int k = 0; k < 32; k++)
				acc += cos((16 + i) * (2 * k + 1) * M_PI / 64) * sub[t][k];
			vbuf[i] = acc;
		}
		for (int j = 0; j < 32; j++) {
			double acc = 0;
			for (int i = 0; i < 8; i++)
				acc += vbuf[128 * i + j] * mp3_d(64 * i + j) + vbuf[128 * i + 96 + j] * mp3_d(64 * i + 32 + j);
			ref[32 * t + j] = acc;
		}
	}

	/* RSP: CMDS comandos MP3 seguidos; cada uno avanza el anillo 18 lugares
	 * (el índice baja 36 ≡ 4). Ganancias enteras +1 y −1: en las ranuras
	 * impares las muestras 17–31 salen con el signo cambiado y la segunda
	 * palabra lo corrige (con +1 en las dos, esas muestras salen negadas). */
	for (int c = 0; c < CMDS; c++) {
		uint32_t a = MP3BUF + 0x1000 * c;
		t_wr32(a, 0x00010000u);
		t_wr32(a + 4, 0xFFFF0000u);
		for (int t = 0; t < 18; t++)
			for (int k = 0; k < 32; k++)
				put16(a + 8 + 0x40 * t + 2 * k, sub[18 * c + t][k]);
		cmd(0x07000000u | ((32u - 4u * c) & 0x1E), a);
	}
	run_abi(SALSA64_ABI_NAUDIO_MP3);
	CHECK_EQ(res.errors, 0);
	CHECK_EQ(res.unknown, 0);
	CHECK_EQ(res.nranges, CMDS);

	/* Escala por mínimos cuadrados y error relativo. */
	double sxy = 0, sxx = 0, syy = 0;
	for (int n = 0; n < SLOTS * 32; n++) {
		double x = ref[n], y = get16(MP3BUF + 0x1000 * (n / 576) + 2 * (n % 576));
		y = (int16_t)y;
		sxy += x * y;
		sxx += x * x;
		syy += y * y;
	}
	double k = sxy / sxx, err = 0, maxerr = 0;
	for (int n = 0; n < SLOTS * 32; n++) {
		double y = (int16_t)get16(MP3BUF + 0x1000 * (n / 576) + 2 * (n % 576));
		double e = fabs(y - k * ref[n]);
		err += e * e;
		if (e > maxerr)
			maxerr = e;
	}
	double rms = sqrt(syy / (SLOTS * 32)), erms = sqrt(err / (SLOTS * 32));
	if (getenv("SALSA64_MP3_DEBUG"))
		fprintf(stderr, "mp3: k = %g, rms %.1f, error rms %.2f, máx %.1f\n", k, rms, erms, maxerr);
	/* 2026-10-10: k = 1,00002, rms 3066, error rms 4,6 y máximo 19,6. */
	CHECK(rms > 1000);
	CHECK(fabs(k - 1) < 0.001);
	CHECK(erms < 8);
	CHECK(maxerr < 40);
}

/* ---- MusyX ---------------------------------------------------------------------- */

#define MX_SFD    0x300000u
#define MX_STATE  0x302000u
#define MX_PCM    0x303000u
#define MX_ECHO   0x304000u
#define MX_SFX    0x305000u
#define MX_OUT    0x306000u

static void mx_clear(void)
{
	memset(ram(0x300000), 0, 0x8000);
	aud_init(&st);
}

/* Una voz PCM16 de 0x50 bytes en `at`: 200 muestras sin cambio de tono. */
static void mx_voice_pcm(uint32_t at, uint32_t env0, uint32_t env1, uint32_t out_ptr)
{
	t_wr32(at + 0x00, env0);
	t_wr32(at + 0x04, env1);
	put16(at + 0x22, 0x1000);        /* una muestra por salida */
	t_wr32(at + 0x24, MX_PCM);       /* tramo 0 */
	put16(at + 0x2C, 2 * 200);
	put16(at + 0x40, 200);           /* muestras */
	t_wr32(at + 0x44, out_ptr);
	put16(at + 0x48, 0x7000);        /* sin bucle */
}

static uint32_t mx_ucode_data;  /* ucode_data del trabajo (0: sin segmento de datos) */

static void mx_run(enum salsa64_abi abi, uint32_t sfd_count)
{
	struct aud_job job;
	memset(&job, 0, sizeof job);
	job.ucode_data = mx_ucode_data;
	job.rdram_ea = (uint64_t)(uintptr_t)t_n64.bus.rdram;
	job.rdram_size = t_n64.bus.rdram_size;
	job.abi = abi;
	job.alist = MX_SFD;
	job.alist_size = sfd_count;
	aud_run(&st, &job, &res);
}

static int16_t mx_rnd(int32_t x)
{
	return (int16_t)((x + 0x4000) >> 15);
}

/* Una voz de PCM16 constante con las dos envolventes en 0,5 y 0,25, sin eco:
 * L y R salen del resampler (un producto redondeado por coeficiente), de la
 * envolvente (redondeada) y de la suma con los volúmenes base. */
static void musyx_v1_pcm_voice(void)
{
	mx_clear();
	for (int i = 0; i < 200; i++)
		put16(MX_PCM + 2 * i, 8192);
	t_wr32(MX_SFD + 0x08, MX_STATE);
	mx_voice_pcm(MX_SFD + 0x10, 0x40000000u, 0x20000000u, MX_OUT);
	/* Volúmenes base de 32 bits (parte alta en +0x100, baja en +0x108): L = 100, R = −50. */
	put16(MX_STATE + 0x108, 100);
	put16(MX_STATE + 0x100 + 2, -1);
	put16(MX_STATE + 0x108 + 2, -50);
	mx_run(SALSA64_ABI_MUSYX1, 1);

	int32_t s = 0;
	for (int k = 0; k < 4; k++)
		s += mx_rnd(8192 * aud_resample_lut[k]);
	int16_t c0 = mx_rnd(s * 0x4000), c1 = mx_rnd(s * 0x2000);
	/* Los volúmenes base decaen un 3 % antes de usarse: (x · 0xF850) >> 16. */
	int32_t bl = (100 * 0xF850) >> 16;
	int32_t br = (-50 * 0xF850) >> 16;
	CHECK_EQ(res.errors, 0);
	CHECK_EQ(res.commands, 2);  /* una voz y una subtrama */
	for (int i = 0; i < 192; i += 17) {
		CHECK_EQ(get16(MX_OUT + 4 * i), c0 + bl);
		CHECK_EQ(get16(MX_OUT + 4 * i + 2), c1 + br);
	}
	/* Última muestra de la voz: los cuatro canales (cc0 y e50 sin envolvente). */
	CHECK_EQ(get16(MX_STATE + 0), c0);
	CHECK_EQ(get16(MX_STATE + 2), c1);
	CHECK_EQ(get16(MX_STATE + 4), 0);
	/* Lo escrito: la salida (768 bytes), las últimas muestras, los volúmenes
	 * base, cc0 y las últimas 4 del eco. */
	CHECK(res.nranges >= 1);
	bool covers_out = false;
	for (uint32_t i = 0; i < res.nranges; i++)
		covers_out |= res.range_addr[i] <= MX_OUT && res.range_addr[i] + res.range_len[i] >= MX_OUT + 768;
	CHECK(covers_out);
}

/* La tabla de interpolación sale del segmento de datos del microcódigo del
 * juego (no hay una en el repositorio): se reconoce por su estructura, en el
 * desplazamiento 0x20 o el 0x28 y en cualquiera, y se usa en lugar de la de
 * lut.c. Aquí, una tabla sintética (no la de ningún juego). */
static void mx_put_lut(uint32_t at)
{
	for (int r = 0; r < 64; r++) {
		put16(at + 8 * r + 0, (int16_t)(8000 - 8 * r));
		put16(at + 8 * r + 2, 8636);
		put16(at + 8 * r + 4, 8636);
		put16(at + 8 * r + 6, (int16_t)(7496 + 8 * r));
	}
}

static int16_t mx_first_samples(uint32_t j, bool own)
{
	/* Rampa 100 × j: la salida j de una voz sin cambio de tono (fase 0). */
	int32_t s = 0;
	for (int k = 0; k < 4; k++) {
		int32_t x = 100 * (int32_t)(j + k), c = own ? aud_resample_lut[k] : (k == 0 ? 8000 : k == 3 ? 7496 : 8636);
		s += mx_rnd(x * c);
	}
	return mx_rnd(s * 0x4000);
}

static void musyx_lut_from_ucode(void)
{
	static const uint32_t offsets[] = { 0x20, 0x28, 0x138 };
	for (unsigned t = 0; t < sizeof offsets / sizeof offsets[0]; t++) {
		mx_clear();
		for (int i = 0; i < 200; i++)
			put16(MX_PCM + 2 * i, (int16_t)(100 * i));
		t_wr32(MX_SFD + 0x08, MX_STATE);
		mx_voice_pcm(MX_SFD + 0x10, 0x40000000u, 0, MX_OUT);
		mx_put_lut(0x307000 + offsets[t]);
		mx_ucode_data = 0x307000;
		mx_run(SALSA64_ABI_MUSYX1, 1);
		CHECK_EQ(res.errors, 0);
		CHECK(st.lut_ok);
		/* Con baseline 0x4000 el producto es s/2; la salida es la de la tabla sintética. */
		bool differs = false;
		for (uint32_t j = 0; j < 150; j += 13) {
			CHECK_EQ(get16(MX_OUT + 4 * j), mx_first_samples(j, false));
			differs |= mx_first_samples(j, false) != mx_first_samples(j, true);
		}
		CHECK(differs);
	}
	/* Sin tabla reconocible (otra compilación, datos en cero) vale la de lut.c; con
	 * esa misma dirección se vuelve a buscar solo si cambia. */
	mx_clear();
	for (int i = 0; i < 200; i++)
		put16(MX_PCM + 2 * i, (int16_t)(100 * i));
	t_wr32(MX_SFD + 0x08, MX_STATE);
	mx_voice_pcm(MX_SFD + 0x10, 0x40000000u, 0, MX_OUT);
	mx_ucode_data = 0x307000;
	mx_run(SALSA64_ABI_MUSYX1, 1);
	CHECK(!st.lut_ok);
	for (uint32_t j = 0; j < 150; j += 13)
		CHECK_EQ(get16(MX_OUT + 4 * j), mx_first_samples(j, true));
	/* Una fila rota (no suma 1,0) no vale. */
	mx_put_lut(0x307000 + 0x28);
	put16(0x307000 + 0x28 + 8 * 20 + 2, 9000);
	put16(0x307000 + 0x28 + 8 * 43 + 4, 9000);
	aud_init(&st);
	mx_run(SALSA64_ABI_MUSYX1, 1);
	CHECK(!st.lut_ok);
	mx_ucode_data = 0;
}

/* El eco: una toma con retardo de 0 muestras lee de la posición actual del
 * búfer circular, en medio del búfer de 576 muestras. La toma con ganancia
 * 0,5 suma a L y a R; el filtro FIR (ganancia 0) deja el búfer en cero. */
static void musyx_v1_echo(void)
{
	mx_clear();
	for (int i = 0; i < 576; i++)
		put16(MX_ECHO + 2 * i, 1000);
	t_wr32(MX_SFD + 0x08, MX_STATE);
	put16(MX_SFD + 0x02, 1);                  /* índice del eco: posición 192 */
	t_wr32(MX_SFD + 0x0C, MX_SFX);
	t_wr32(MX_SFX + 0x00, MX_ECHO);
	t_wr32(MX_SFX + 0x04, 576);
	put16(MX_SFX + 0x08, 1);                  /* una toma */
	t_wr32(MX_SFX + 0x0C, 192);               /* retardo de una subtrama: lee la posición 0 */
	put16(MX_SFX + 0x2C, 0x4000);             /* ganancia 0,5 */
	/* Una voz sin muestras: la etapa de voces se salta y la salida es la de la primera. */
	t_wr32(MX_SFD + 0x10 + 0x44, MX_OUT);
	mx_run(SALSA64_ABI_MUSYX1, 1);
	CHECK_EQ(res.errors, 0);
	for (int i = 0; i < 192; i += 11) {
		CHECK_EQ(get16(MX_OUT + 4 * i), 500);
		CHECK_EQ(get16(MX_OUT + 4 * i + 2), 500);
		CHECK_EQ(get16(MX_ECHO + 2 * (192 + i)), 0);  /* e50 tras el FIR */
		CHECK_EQ(get16(MX_ECHO + 2 * i), 1000);        /* lo demás sigue igual */
	}
}

/* Los dos tramos del ADPCM y el PCM se pegan, y un dato absurdo no se sale
 * de los búferes locales: se cuenta como error. */
static void musyx_garbage_stays_inside(void)
{
	uint32_t seed = 0x9E3779B9u;
	for (int round = 0; round < 24; round++) {
		mx_clear();
		for (uint32_t a = 0x300000; a < 0x308000; a += 4) {
			seed = seed * 1664525u + 1013904223u;
			uint32_t v = seed;
			/* Direcciones plausibles en las palabras de puntero, el resto ruido. */
			if ((a & 0x1F) == 0x08 || (a & 0x1F) == 0x0C || (a & 0x3C) == 0x24)
				v = 0x300000u + (v & 0x7FFC);
			t_wr32(a, v);
		}
		enum salsa64_abi abi = round % 3 == 0 ? SALSA64_ABI_MUSYX2 : round % 3 == 1 ? SALSA64_ABI_MUSYX1 : SALSA64_ABI_MUSYX1_RS;
		mx_run(abi, 1 + (uint32_t)(round % 3));
		CHECK(res.nranges <= AUD_MAX_RANGES);
	}
	/* Sin subtramas o con demasiadas: error y nada escrito. */
	mx_clear();
	mx_run(SALSA64_ABI_MUSYX1, 0);
	CHECK_EQ(res.errors, 1);
	CHECK_EQ(res.nranges, 0);
	mx_run(SALSA64_ABI_MUSYX2, AUD_MUSYX_MAX_SFD + 1);
	CHECK_EQ(res.errors, 1);
	CHECK_EQ(res.nranges, 0);
}

/* La lista de arranque de la PS3 (selftest.c): sin errores, con las 4 voces
 * y subtramas que arma, y la misma salida en dos corridas desde el mismo
 * estado. Los valores de `golden` son de la implementación actual: cualquier
 * cambio en las cuentas de MusyX los mueve y hay que revisarlo contra el
 * microcódigo real (docs/estado.md). */
static uint32_t fnv32(const uint8_t *p, uint32_t n)
{
	uint32_t h = 2166136261u;
	while (n--)
		h = (h ^ *p++) * 16777619u;
	return h;
}

static void musyx_selftest_lists(void)
{
	static uint8_t a[AUD_SELFTEST_RAM], b[AUD_SELFTEST_RAM];
	static struct aud_state sa, sb;
	static const struct { uint32_t abi, golden; } cases[] = {
		{ SALSA64_ABI_MUSYX1, 0x5faf9a2cu },
		{ SALSA64_ABI_MUSYX1_RS, 0xbbef1e66u },
		{ SALSA64_ABI_MUSYX2, 0x676898efu },
	};
	for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		struct aud_job job;
		struct aud_result ra, rb;
		aud_init(&sa);
		aud_init(&sb);
		memset(&job, 0, sizeof job);
		job.rdram_size = AUD_SELFTEST_RAM;
		job.abi = cases[i].abi;
		job.alist_size = aud_selftest_build(a, cases[i].abi, 777u + i);
		CHECK_EQ(job.alist_size, 2);
		memcpy(b, a, sizeof b);
		job.rdram_ea = (uint64_t)(uintptr_t)a;
		aud_run(&sa, &job, &ra);
		job.rdram_ea = (uint64_t)(uintptr_t)b;
		aud_run(&sb, &job, &rb);
		CHECK_EQ(ra.errors, 0);
		CHECK_EQ(ra.unknown, 0);
		CHECK_EQ(ra.commands, 8);  /* (3 voces + 1 subtrama) × 2 */
		CHECK(memcmp(a, b, sizeof a) == 0);
		CHECK_EQ(fnv32(a, sizeof a), cases[i].golden);
	}
}

/* ---- Tarea completa por el núcleo ------------------------------------------- */

static void core_runs_audio_task(void)
{
	static struct aud_state core_state;
	static struct salsa64_audio_sync sync;
	struct salsa64_bus *bus = &t_n64.bus;
	salsa64_audio_sync_init(&sync, &core_state, bus);
	salsa64_rsp_set_audio(bus, &sync.backend);
	T_LOAD(BEQ(0, 0, 0xFFFF), NOP);

	memset(ram(0x300000), 0, 0x40);
	memcpy(ram(0x300000), "Salsa64 ABI NEAD", 16);
	for (int i = 0; i < 8; i++)
		put16(SRC + 2 * i, (int16_t)(7 * i));
	nlist = 0;
	n_loadbuff(0x100, SRC, 16);
	n_savebuff(0x100, DST, 16);
	uint32_t task[16] = { SALSA64_TASK_AUDIO, 0, 0, 0x1000, 0x80301000u, 0x100, 0x80300000u, 0x40,
			      0, 0, 0, 0, 0x80000000u | ALIST, 8 * nlist, 0, 0 };
	for (int i = 0; i < 16; i++)
		salsa64_wr32(bus->sp_mem + SALSA64_TASK_DMEM + 4 * i, task[i]);
	bus->sp.regs[SALSA64_SP_STATUS] = SALSA64_SP_STATUS_HALT;
	salsa64_bus_write32(bus, 0x04040010u, 0x001, ~0u);  /* quitar HALT */
	CHECK_EQ(bus->rsp.abi, SALSA64_ABI_NEAD_OOT);
	CHECK_EQ(bus->rsp.busy, SALSA64_TASK_AUDIO);
	salsa64_n64_run(&t_n64, bus->rsp.audio_cycles + 1);
	CHECK_EQ(bus->rsp.busy, 0);
	CHECK(bus->sp.regs[SALSA64_SP_STATUS] & SALSA64_SP_STATUS_TASKDONE);
	CHECK_EQ(sync.stats.tasks, 1);
	CHECK_EQ(sync.stats.commands, 2);
	CHECK_EQ(get16(DST + 14), 49);

	/* ABI desconocido: termina igual, sin HLE. */
	uint32_t unknown0 = bus->rsp.stats.unknown_abi;
	memset(ram(0x300000), 0, 0x40);
	bus->sp.regs[SALSA64_SP_STATUS] = SALSA64_SP_STATUS_HALT;
	salsa64_bus_write32(bus, 0x04040010u, 0x001, ~0u);
	CHECK_EQ(bus->rsp.abi, SALSA64_ABI_UNKNOWN);
	salsa64_n64_run(&t_n64, bus->rsp.audio_cycles + 1);
	CHECK(bus->sp.regs[SALSA64_SP_STATUS] & SALSA64_SP_STATUS_TASKDONE);
	CHECK_EQ(sync.stats.tasks, 1);
	CHECK_EQ(bus->rsp.stats.unknown_abi, unknown0 + 1);
	salsa64_rsp_set_audio(bus, NULL);
}

/* Las listas que compara el arranque en la PS3 (SPE contra PPE): corren sin
 * errores, con un solo comando desconocido a propósito en nead, y dos
 * corridas desde el mismo estado dejan lo mismo. */
static void selftest_lists_are_clean(void)
{
	static uint8_t a[AUD_SELFTEST_RAM], b[AUD_SELFTEST_RAM];
	static struct aud_state sa, sb;
	static const uint32_t abis[] = { SALSA64_ABI_NEAD_OOT, SALSA64_ABI_AUDIO, SALSA64_ABI_NEAD_MK,
					 SALSA64_ABI_NAUDIO_MP3, SALSA64_ABI_NAUDIO_BK };
	aud_init(&sa);
	aud_init(&sb);
	for (unsigned i = 0; i < sizeof abis / sizeof abis[0]; i++) {
		struct aud_job job;
		struct aud_result ra, rb;
		memset(&job, 0, sizeof job);
		job.rdram_size = AUD_SELFTEST_RAM;
		job.abi = abis[i];
		job.alist_size = aud_selftest_build(a, abis[i], 12345u + i);
		memcpy(b, a, sizeof b);
		job.rdram_ea = (uint64_t)(uintptr_t)a;
		aud_run(&sa, &job, &ra);
		job.rdram_ea = (uint64_t)(uintptr_t)b;
		aud_run(&sb, &job, &rb);
		CHECK_EQ(ra.errors, 0);
		CHECK_EQ(ra.commands, job.alist_size / 8);
		CHECK_EQ(ra.unknown, abis[i] == SALSA64_ABI_NEAD_OOT ? 1 : 0);
		CHECK(memcmp(a, b, sizeof a) == 0);
		/* La DMEM guardada no queda en cero: los comandos hicieron algo. */
		unsigned nonzero = 0;
		for (uint32_t k = 0x2000; k < 0x3000; k++)
			nonzero += a[k] != 0;
		CHECK(nonzero > 1000);
	}
}


static int16_t get16_dmem(uint32_t a)
{
	return (int16_t)salsa64_rd16(st.dmem + a);
}

/* ---- Lo comprobado contra el microcódigo real (docs/audio-censo.md) -------------
 * Los valores esperados salen de ejecutar el microcódigo de los respaldos del
 * autor con listas sintéticas en el LLE (--lle-check-audio y el banco de
 * pruebas del censo); aquí se fijan con datos propios, sin nada del juego. */

/* ENVMIXER de ABI1 con el estado del microcódigo: volumen exponencial de 256
 * con tasa 2,0 y destino 1280, entrada constante 0x7FFF y seco 0x7FFF: la
 * salida es la parte entera del volumen de cada muestra. */
static void abi1_envmixer_real_state(void)
{
	fresh();
	put_const(SRC, 40, 0x7FFF);
	cmd(0x08000000u | 0x000, 0x0100u << 16 | 80);   /* in 0x5C0, out 0x6C0, 80 bytes = 5 bloques */
	cmd(0x04000000u, SRC);
	cmd(0x08000000u | 0x08u << 16 | 0x200, 0x0300u << 16 | 0x400);
	cmd(0x09000000u | 0x08u << 16 | 0x7FFF, 0);
	cmd(0x09000000u | 0x06u << 16 | 0x0100, 0);
	cmd(0x09000000u | 0x04u << 16 | 0x0100, 0);
	cmd(0x09000000u | 0x02u << 16 | 0x0500, 0x20000);
	cmd(0x09000000u | 0x00u << 16 | 0x0500, 0x20000);
	cmd(0x03000000u | 0x01u << 16, STATE);
	cmd(0x06000000u, DST);
	run_abi(SALSA64_ABI_AUDIO);
	CHECK_EQ(res.errors, 0);
	static const int16_t want[24] = {
		288, 320, 352, 384, 416, 448, 480, 511,           /* v + v·(tasa − 1)·c_j, con c_7 = 0xFFFF */
		576, 640, 704, 768, 832, 896, 960, 1023,          /* ×2 en cada carril */
		1152, 1280, 1280, 1280, 1280, 1280, 1280, 1280    /* ×2 y recortado al destino */
	};
	for (int i = 0; i < 24; i++)
		CHECK_EQ(get16(DST + 2 * i), want[i]);
	/* El estado es el de libultra: 8 carriles (parte entera y fracción) por
	 * canal, destino, tasa (alta, baja), seco y húmedo. */
	for (int j = 0; j < 8; j++) {
		CHECK_EQ(get16(STATE + 2 * j), 0x0500);
		CHECK_EQ(get16(STATE + 0x20 + 2 * j), 0x0500);
	}
	CHECK_EQ(get16(STATE + 0x40), 0x0500);
	CHECK_EQ(get16(STATE + 0x42), 2);
	CHECK_EQ(get16(STATE + 0x44), 0);
	CHECK_EQ(get16(STATE + 0x4C), 0x7FFF);
	CHECK_EQ(get16(STATE + 0x4E), 0);

	/* Sin A_INIT sigue desde el estado: el volumen ya está en el destino. */
	cmd(0x08000000u | 0x000, 0x0100u << 16 | 16);
	cmd(0x04000000u, SRC);
	cmd(0x08000000u | 0x08u << 16 | 0x200, 0x0300u << 16 | 0x400);
	cmd(0x02000000u | 0x100, 16);
	cmd(0x03000000u, STATE);
	cmd(0x06000000u, DST);
	run_abi(SALSA64_ABI_AUDIO);
	for (int i = 0; i < 8; i++)
		CHECK_EQ(get16(DST + 2 * i), 1280);
}

/* POLEF de ABI1 con A_INIT: el microcódigo toma las dos muestras previas de
 * su zona de estados de la DMEM (+4 y +6 de 0xF90), que es donde el último
 * RESAMPLE dejó sus 4 muestras y el ENVMIXER sus carriles (alist_polef). Un
 * RESAMPLE 1:1 deja ahí las últimas muestras de su entrada; el POLEF
 * siguiente, con entrada cero y h1 = 0, h2[0] = 0,75, arranca con
 * 0,75 · l2 y decae; con AUD_JOB_POLEF_INIT_ZERO (o sin RESAMPLE antes) sale
 * cero, como en mupen64plus-rsp-hle. */
static void abi1_polef_init_from_scratch(void)
{
	static const int16_t h2[8] = { 0x3000, 0x2400, 0x1B00, 0x1440, 0x0F30, 0x0B64, 0x088B, 0x0668 };
	for (int pass = 0; pass < 3; pass++) {
		fresh();
		job_flags = pass == 1 ? AUD_JOB_POLEF_INIT_ZERO : 0;
		/* Entrada del RESAMPLE: 16 muestras, las 4 últimas −100, 200, 18, −57. */
		put_const(SRC, 16, 1000);
		put16(SRC + 24, -100);
		put16(SRC + 26, 200);
		put16(SRC + 28, 18);
		put16(SRC + 30, -57);
		put_const(SRC2, 16, 0);                        /* la entrada del POLEF: silencio */
		for (int i = 0; i < 8; i++) {
			put16(BOOK + 2 * i, 0);                    /* h1 */
			put16(BOOK + 16 + 2 * i, h2[i]);
		}
		if (pass != 2) {
			cmd(0x08000000u | 0x000, 0x0100u << 16 | 32);   /* in 0x5C0, out 0x6C0, 32 bytes */
			cmd(0x04000000u, SRC);
			cmd(0x05000000u | 0x01u << 16 | 0x8000, STATE); /* RESAMPLE A_INIT, tono 1,0 */
		}
		cmd(0x08000000u | 0x200, 0x0300u << 16 | 32);   /* in 0x7C0, out 0x8C0 */
		cmd(0x04000000u, SRC2);
		cmd(0x0B000000u | 32, BOOK);                    /* LOADADPCM: h1, h2 */
		cmd(0x0E000000u | 0x01u << 16 | 0x4000, STATE + 0x40);  /* POLEF A_INIT, ganancia 1,0 */
		cmd(0x08000000u | 0x000, 0x0300u << 16 | 32);
		cmd(0x06000000u, DST);                          /* SAVEBUFF 0x8C0 */
		run_abi(SALSA64_ABI_AUDIO);
		CHECK_EQ(res.errors, 0);
		CHECK_EQ(res.unknown, 0);
		if (pass != 0) {
			for (int i = 0; i < 16; i++)
				CHECK_EQ(get16(DST + 2 * i), 0);
			continue;
		}
		/* El estado del RESAMPLE quedó en la DMEM 0xF90 y l1 = 18, l2 = −57: la salida
		 * es la respuesta del filtro de dos polos a ese estado con entrada cero. */
		CHECK_EQ(get16(STATE + 6), -57);
		CHECK_EQ((int16_t)salsa64_rd16(st.dmem + 0xF94), 18);
		CHECK_EQ((int16_t)salsa64_rd16(st.dmem + 0xF96), -57);
		int16_t l2 = -57, want[16];
		for (int b = 0; b < 2; b++) {
			int16_t out[8];
			for (int i = 0; i < 8; i++)
				out[i] = (int16_t)((h2[i] * l2) >> 14);  /* h1 = 0 y la entrada es cero: solo pesa l2 */
			memcpy(want + 8 * b, out, sizeof out);
			l2 = out[7];
		}
		CHECK_EQ(want[0], -43);
		for (int i = 0; i < 16; i++)
			CHECK_EQ(get16(DST + 2 * i), want[i]);
		/* POLEF deja a cero los 4 primeros bytes de la zona y guarda sus 4 últimas salidas. */
		CHECK_EQ(salsa64_rd32(st.dmem + 0xF90), 0);
		CHECK_EQ(get16(STATE + 0x40 + 6), want[15]);
	}
}

/* ENVMIXER lineal (GoldenEye): carril j del bloque inicial = v + tasa·c_j; cada
 * bloque suma la tasa. La dirección sale de la tasa: una tasa ≥ 1,0 recorta
 * hacia arriba; una menor da el destino de inmediato. */
static void abi1_envmixer_linear(void)
{
	fresh();
	put_const(SRC, 40, 0x7FFF);
	cmd(0x08000000u | 0x000, 0x0100u << 16 | 48);   /* 3 bloques */
	cmd(0x04000000u, SRC);
	cmd(0x08000000u | 0x08u << 16 | 0x200, 0x0300u << 16 | 0x400);
	cmd(0x09000000u | 0x08u << 16 | 0x7FFF, 0);
	cmd(0x09000000u | 0x06u << 16 | 0x0100, 0);
	cmd(0x09000000u | 0x04u << 16 | 0x0100, 0);
	cmd(0x09000000u | 0x02u << 16 | 0x5000, 0x20000);
	cmd(0x09000000u | 0x00u << 16 | 0x5000, 0x20000);
	cmd(0x03000000u | 0x01u << 16, STATE);
	run_abi(SALSA64_ABI_AUDIO_GE);
	CHECK_EQ(res.errors, 0);
	/* Estado tras 3 bloques: los del tercero (bloque 2): 256 + 4 + 2,0·(j + 1)/8. */
	static const uint16_t hi[8] = { 0x0104, 0x0104, 0x0104, 0x0105, 0x0105, 0x0105, 0x0105, 0x0105 };
	static const uint16_t lo[8] = { 0x4000, 0x8000, 0xC000, 0x0000, 0x4000, 0x8000, 0xC000, 0xFFFE };
	for (int j = 0; j < 8; j++) {
		CHECK_EQ((uint16_t)get16(STATE + 2 * j), hi[j]);
		CHECK_EQ((uint16_t)get16(STATE + 0x10 + 2 * j), lo[j]);
	}
	/* Tasa pequeña (0x1000 < 1,0): el volumen salta al destino. */
	cmd(0x08000000u | 0x000, 0x0100u << 16 | 32);
	cmd(0x04000000u, SRC);
	cmd(0x08000000u | 0x08u << 16 | 0x200, 0x0300u << 16 | 0x400);
	cmd(0x09000000u | 0x06u << 16 | 0x0100, 0);
	cmd(0x09000000u | 0x04u << 16 | 0x0100, 0);
	cmd(0x09000000u | 0x02u << 16 | 0x0500, 0x1000);
	cmd(0x09000000u | 0x00u << 16 | 0x0500, 0x1000);
	cmd(0x03000000u | 0x01u << 16, STATE + 0x100);
	run_abi(SALSA64_ABI_AUDIO_GE);
	CHECK_EQ((uint16_t)get16(STATE + 0x100), 0x0500);
	CHECK_EQ((uint16_t)get16(STATE + 0x110), 0x1200);   /* v + tasa·(1 + c_0) */
	CHECK_EQ((uint16_t)get16(STATE + 0x11E), 0x1FFF);
}

/* naudio de Donkey Kong 64, Perfect Dark, Banjo-Tooie y Conker: el bit bajo del
 * seco invierte (complemento a uno) la entrada del canal izquierdo y el del
 * húmedo la del derecho. */
static void naudio_phase_flags(void)
{
	static const int xs[4] = { 1000, 12345, -100, 0 };
	for (int variant = 0; variant < 2; variant++) {
		enum salsa64_abi abi = variant ? SALSA64_ABI_NAUDIO_MP3 : SALSA64_ABI_NAUDIO_DK;
		for (int odd = 0; odd < 2; odd++) {
			fresh();
			for (int i = 0; i < NA_N; i++)
				put16(NA_SRC + 2 * i, (int16_t)xs[i & 3]);
			na_loadbuff(0, NA_SRC, 0x170);
			/* seco y húmedo 0x7FFE o 0x7FFF */
			na_setvol(0x06, 0x7FFF, (uint32_t)(0x7FFE + odd) << 16 | (uint32_t)(0x7FFE + odd));
			na_setvol(0x04, 0x7FFF, 0);
			na_setvol(0x00, 0x7FFF, 0);
			cmd(0x03000000u | (0x01u << 16) | 0x7FFF, NA_STATE);
			na_savebuff(NA_DL, NA_DST, 0x170);
			na_savebuff(NA_DR, NA_DST + 0x200, 0x170);
			run_abi(abi);
			CHECK_EQ(res.errors, 0);
			/* ganancia = vmulf(0x7FFF, 0x7FFE + odd); la entrada se invierte si odd */
			int32_t g = (0x7FFF * (0x7FFE + odd) + 0x4000) >> 15;
			for (int i = 0; i < 4; i++) {
				int32_t x = odd ? ~xs[i] : xs[i];
				int32_t want = (x * g + 0x4000) >> 15;
				CHECK_EQ(get16(NA_DST + 2 * i), want);
				CHECK_EQ(get16(NA_DST + 0x200 + 2 * i), want);
			}
		}
	}
	/* Banjo-Kazooie no invierte nada con el mismo bit. */
	fresh();
	put_const(NA_SRC, NA_N, 1000);
	na_loadbuff(0, NA_SRC, 0x170);
	na_setvol(0x06, 0x7FFF, 0x7FFFu << 16 | 0x7FFF);
	na_setvol(0x04, 0x7FFF, 0);
	na_setvol(0x00, 0x7FFF, 0);
	cmd(0x03000000u | (0x01u << 16) | 0x7FFF, NA_STATE);
	na_savebuff(NA_DL, NA_DST, 0x20);
	run_abi(SALSA64_ABI_NAUDIO_BK);
	CHECK(get16(NA_DST) > 0);
}

/* Los naudio con MP3 tienen todos los búferes 0x10 bytes más arriba (0x500). */
static void naudio_mp3_buffers_base(void)
{
	fresh();
	put_const(NA_SRC, NA_N, 777);
	na_loadbuff(0, NA_SRC, 0x170);
	run_abi(SALSA64_ABI_NAUDIO_MP3);
	CHECK_EQ(get16_dmem(0x500), 777);
	CHECK_EQ(get16_dmem(0x4F0), 0);
	fresh();
	put_const(NA_SRC, NA_N, 777);
	na_loadbuff(0, NA_SRC, 0x170);
	run_abi(SALSA64_ABI_NAUDIO_BK);
	CHECK_EQ(get16_dmem(0x4F0), 777);
}

/* FILTER de Yoshi's Story y 1080°: la tabla de la nota va directa y no se
 * escribe nada aparte de las muestras previas; en Zelda se promedia con la
 * anterior y queda guardada. */
static void nead_filter_direct(void)
{
	for (int variant = 0; variant < 2; variant++) {
		fresh();
		for (int i = 0; i < 8; i++) {
			put16(STATE + 0x100 + 2 * i, 0);                           /* estado: 8 muestras previas */
			put16(STATE + 0x110 + 2 * i, 0);                           /* la tabla anterior, a continuación */
			put16(STATE + 0x200 + 2 * i, (int16_t)(i ? 0 : 16384));   /* tabla de la nota: un solo tap = 0,5 */
			put16(SRC + 2 * i, (int16_t)(1000 * (i + 1)));
		}
		n_loadbuff(0x400, SRC, 16);
		cmd(0x07020000u | 16, STATE + 0x200);                          /* tabla de la nota, 16 bytes */
		cmd(0x07000000u | 0x400, STATE + 0x100);                       /* filtrar */
		n_savebuff(0x400, DST, 16);
		run_abi(variant ? SALSA64_ABI_NEAD_YS : SALSA64_ABI_NEAD_OOT);
		CHECK_EQ(res.errors, 0);
		/* Directa: muestra · 0,5. Promediada con cero: ·0,25 (y queda escrita). */
		for (int i = 0; i < 8; i++)
			CHECK_EQ(get16(DST + 2 * i), variant ? 500 * (i + 1) : 250 * (i + 1));
	}
}

/* ENVMIXER de nead: la cuenta se redondea a múltiplos de 16 muestras. */
static void nead_envmixer_count_rounds(void)
{
	fresh();
	put_const(SRC, 16, 4000);
	n_loadbuff(0x100, SRC, 32);
	cmd(0x12000000u | 0x80u << 16, 0);
	cmd(0x16000000u, 0x8000u << 16 | 0x8000);
	cmd(0x13000000u | 0x10u << 16 | 8u << 8, 0x20u << 24 | 0x30u << 16 | 0x40u << 8 | 0x50);   /* 8 muestras */
	n_savebuff(0x200, DST, 32);
	run_abi(SALSA64_ABI_NEAD_OOT);
	CHECK_EQ(res.errors, 0);
	CHECK_EQ(get16(DST + 14), 2000);   /* la 8.ª */
	CHECK_EQ(get16(DST + 30), 2000);   /* y la 16.ª, que no se había pedido */
}

/* ADDMIXER tras MIXER (nead): el acarreo que deja MIXER en VCO (carril 3) lo
 * consume el primer vector del ADDMIXER; RESAMPLE lo borra. */
static void nead_add_carry_after_mixer(void)
{
	for (int pass = 0; pass < 3; pass++) {
		fresh();
		put_const(SRC, 16, 100);
		put_const(SRC2, 16, 200);
		n_loadbuff(0x100, SRC, 32);
		n_loadbuff(0x200, SRC2, 32);
		if (pass < 2)
			cmd(0x0C000000u | 32u << 12 | 0x4000, 0x0100u << 16 | 0x300);   /* MIXER (otro búfer) */
		if (pass == 1) {
			n_setbuff(0x100, 0x500, 32);                                    /* RESAMPLE borra el acarreo */
			cmd(0x05000000u | 0x01u << 16 | 0x8000, STATE);
		}
		cmd(0x04000000u | 32u << 12, 0x0100u << 16 | 0x200);               /* ADDMIXER: 0x200 += 0x100 */
		n_savebuff(0x200, DST, 32);
		run_abi(SALSA64_ABI_NEAD_OOT);
		CHECK_EQ(res.errors, 0);
		for (int i = 0; i < 16; i++)
			CHECK_EQ(get16(DST + 2 * i), 300 + (pass == 0 && i == 3));
	}
}

/* El arranque de libultra copia el segmento de datos del microcódigo a la
 * DMEM antes de cada tarea; en los de 2 KB eso pisa los búferes. */
static void boot_data_copy(void)
{
	fresh();
	uc_data = 0x230000;
	uc_size = 0x800;
	for (int i = 0; i < 0x400; i++)
		put16(uc_data + 2 * i, (int16_t)(i + 1));
	cmd(0x08000000u | 0x000, 0x0004u << 16 | 16);       /* out = 0x5C4 de la DMEM: muestra 0x2E2 del segmento */
	cmd(0x06000000u, DST);
	run_abi(SALSA64_ABI_AUDIO_GE);
	CHECK_EQ(get16(DST + 0), 0x2E3);
	CHECK_EQ(get16(DST + 2), 0x2E4);
	/* Con ucode_data_size = 0 (pruebas, ROMs sin microcódigo) no toca la DMEM. */
	fresh();
	uc_size = 0;
	st.dmem[0x5C4] = 0x12;
	st.dmem[0x5C5] = 0x34;
	cmd(0x08000000u | 0x000, 0x0004u << 16 | 16);
	cmd(0x06000000u, DST);
	run_abi(SALSA64_ABI_AUDIO_GE);
	CHECK_EQ(get16(DST + 0), 0x1234);
	uc_data = uc_size = 0;
}

/* La tabla de interpolación de RESAMPLE se toma del segmento de datos del
 * microcódigo (como la de MusyX): fila r = fila 63 − r al revés, suma 1,0. */
static void resample_table_from_ucode_data(void)
{
	fresh();
	uc_data = 0x238000;
	uc_size = 0x2C0;
	memset(ram(uc_data), 0, 0x2C0);
	run_abi(SALSA64_ABI_AUDIO);
	CHECK(!st.lut_ok);   /* sin tabla reconocible vale la de lut.c */
	for (int r = 0; r < 32; r++) {
		int a = 3000 + 40 * r, b = 12000 - 40 * r;
		int16_t row[4] = { (int16_t)a, (int16_t)b, (int16_t)(32768 - a - b - 16), 16 };
		for (int k = 0; k < 4; k++) {
			put16(uc_data + 0xC0 + 8 * r + 2 * k, row[k]);
			put16(uc_data + 0xC0 + 8 * (63 - r) + 2 * (3 - k), row[k]);
		}
	}
	aud_init(&st);
	run_abi(SALSA64_ABI_AUDIO);
	CHECK(st.lut_ok);
	CHECK_EQ(st.lut[0], 3000);
	CHECK_EQ(st.lut[63 * 4 + 3], 3000);
	uc_data = uc_size = 0;
}


/* ---- Conker (CBFD) y MP3ADDY ------------------------------------------------------ */

static int j_ramp(int j)
{
	return j * 1000 - 4000;
}

static int32_t vm(int a, int b)
{
	return (a * b * 2 + 0x8000) >> 16;
}

static void put_envstate(uint32_t pa, int vol, int tgt, int dry, int wet)
{
	memset(ram(pa), 0, 80);
	for (int c = 0; c < 2; c++)
		for (int j = 0; j < 8; j++)
			put16(pa + 0x20 * c + 2 * j, (int16_t)vol);
	put16(pa + 0x40, (int16_t)tgt);
	put16(pa + 0x46, (int16_t)tgt);
	put16(pa + 0x4C, (int16_t)dry);
	put16(pa + 0x4E, (int16_t)wet);
}

/* ENVMIXER de Conker: la muestra se multiplica (VMULF) por la polaridad de cada canal, 0x7FFF
 * o 0x8000 según el bit bajo de seco o húmedo; el primer bloque de 8 muestras pasa antes por
 * 0x7FFF (0x7FFE o 0x8002 de escala) y, con el bit 1 en uno, por el vector de rampa de 0xA0
 * del segmento de datos. Medido contra el microcódigo real (docs/audio-censo.md). */
static void naudio_cbfd_envmixer(void)
{
	const int X = 20000, V = 0x4000, DRY = 0x7FFC, WET = 0x7FFD;
	fresh();
	put_const(NA_SRC, NA_N, (int16_t)X);
	put_envstate(NA_STATE, V, V, DRY, WET);
	na_loadbuff(0, NA_SRC, 0x170);
	cmd(0x03000000u, NA_STATE);
	na_savebuff(NA_DL, NA_DST, 0x170);
	na_savebuff(NA_WR, NA_DST + 0x200, 0x170);
	run_abi(SALSA64_ABI_NAUDIO_CBFD);
	CHECK_EQ(res.errors, 0);
	CHECK_EQ(res.unknown, 0);
	int gd = vm(V, DRY), gw = vm(V, WET);
	for (int lane = 0; lane < 12; lane += 5) {
		int sd = lane < 8 ? vm(0x7FFF, 0x7FFF) : 0x7FFF;       /* seco par: polaridad positiva */
		int sw = lane < 8 ? vm(0x7FFF, -0x8000) : -0x8000;      /* húmedo impar: polaridad negativa */
		int xd = vm(X, sd), xw = vm(X, sw);
		if (xw > 32767)
			xw = 32767;
		CHECK_EQ(get16(NA_DST + 2 * lane), (xd * gd + 0x4000) >> 15);               /* seco izquierdo */
		CHECK_EQ(get16(NA_DST + 0x200 + 2 * lane), (xw * vm(V, WET) + 0x4000) >> 15);  /* húmedo derecho */
	}
	(void)gw;
	/* Con el bit 1 del seco en uno, el primer bloque pasa por la rampa del segmento de datos. */
	{
		static uint8_t data[0x100];
		memset(data, 0, sizeof data);
		for (int j = 0; j < 8; j++)
			salsa64_wr16(data + 0xA0 + 2 * j, (uint16_t)(j * 1000 - 4000));
		memcpy(ram(NA_BOOK), data, sizeof data);
		uc_data = NA_BOOK;
		uc_size = sizeof data;
		fresh();
		put_const(NA_SRC, NA_N, (int16_t)X);
		put_envstate(NA_STATE, V, V, 0x7FFE, WET);
		na_loadbuff(0, NA_SRC, 0x170);
		cmd(0x03000000u, NA_STATE);
		na_savebuff(NA_DL, NA_DST, 0x170);
		run_abi(SALSA64_ABI_NAUDIO_CBFD);
		uc_data = uc_size = 0;
		int g2 = vm(V, 0x7FFE);
		for (int lane = 0; lane < 8; lane++) {
			int x2 = vm(X, vm(j_ramp(lane), 0x7FFF));
			CHECK_EQ(get16(NA_DST + 2 * lane), (x2 * g2 + 0x4000) >> 15);
		}
		CHECK_EQ(get16(NA_DST + 2 * 9), (vm(X, 0x7FFF) * g2 + 0x4000) >> 15);
	}
}

/* MP3ADDY: cada MP3 trae de la RDRAM el anillo de la ventana y lo guarda al terminar; sin MP3ADDY
 * el anillo queda en la memoria de trabajo de la HLE. */
static void naudio_mp3addy_ring(void)
{
	enum { RING = 0x238000, BUF = 0x239000 };
	uint8_t pat[0x440];
	for (unsigned i = 0; i < sizeof pat; i++)
		pat[i] = (uint8_t)(i * 7 + 3);
	int16_t out[2][16];
	for (int run = 0; run < 2; run++) {
		fresh();
		memcpy(ram(RING), pat, sizeof pat);
		memset(ram(BUF), 0, 0x500);
		t_wr32(BUF, 0x00010000u);
		t_wr32(BUF + 4, 0x00010000u);
		for (int i = 0; i < 0x240; i++)
			put16(BUF + 8 + 2 * i, (int16_t)(i % 31 * 40 - 600));
		if (run)
			memset(st.mp3, 0x55, sizeof st.mp3);   /* basura en la memoria de trabajo: no debe importar */
		cmd(0x08000000u, RING);                      /* MP3ADDY */
		cmd(0x07000004u, BUF);                       /* MP3 */
		run_abi(SALSA64_ABI_NAUDIO_MP3);
		CHECK_EQ(res.errors, 0);
		CHECK_EQ(res.unknown, 0);
		for (int i = 0; i < 16; i++)
			out[run][i] = get16(BUF + 2 * i);
		unsigned same = 0;
		for (unsigned i = 0; i < sizeof pat; i++)
			same += ram(RING)[i] == pat[i];
		CHECK(same < sizeof pat / 2);              /* el anillo se guardó de vuelta */
	}
	CHECK(memcmp(out[0], out[1], sizeof out[0]) == 0);

	/* Sin MP3ADDY no se toca la RDRAM del anillo. */
	fresh();
	memcpy(ram(RING), pat, sizeof pat);
	cmd(0x07000004u, BUF);
	run_abi(SALSA64_ABI_NAUDIO_MP3);
	CHECK(memcmp(ram(RING), pat, sizeof pat) == 0);
}

void t_suite_audio(void)
{
	RUN(detect_abi);
	RUN(nead_load_mix_save);
	RUN(nead_buffer_moves);
	RUN(nead_interleave);
	RUN(nead_envmixer);
	RUN(nead_resample);
	RUN(lut_rows_sum_to_one);
	RUN(nead_adpcm);
	RUN(nead_filter);
	RUN(nead_errors_and_unknown);
	RUN(ranges_merge_when_full);
	RUN(abi1_segments_envmixer);
	RUN(abi1_mixer_interleave);
	RUN(naudio_load_mix_save);
	RUN(naudio_envmixer_interleave);
	RUN(naudio_adpcm_resample);
	RUN(naudio_variant_tables);
	RUN(naudio_filter);
	RUN(naudio_overload);
	RUN(mp3_window_table);
	RUN(mp3_matches_reference);
	RUN(musyx_v1_pcm_voice);
	RUN(musyx_lut_from_ucode);
	RUN(musyx_v1_echo);
	RUN(musyx_garbage_stays_inside);
	RUN(musyx_selftest_lists);
	RUN(core_runs_audio_task);
	RUN(selftest_lists_are_clean);
	RUN(abi1_envmixer_real_state);
	RUN(abi1_envmixer_linear);
	RUN(abi1_polef_init_from_scratch);
	RUN(naudio_phase_flags);
	RUN(naudio_mp3_buffers_base);
	RUN(nead_filter_direct);
	RUN(nead_envmixer_count_rounds);
	RUN(nead_add_carry_after_mixer);
	RUN(boot_data_copy);
	RUN(resample_table_from_ucode_data);
	RUN(naudio_cbfd_envmixer);
	RUN(naudio_mp3addy_ring);
}
