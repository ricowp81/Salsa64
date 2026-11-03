/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — HLE de los microcódigos de audio (listas de audio): interfaz.
 *
 * Una tarea de audio de libultra es una lista de comandos (alist) que
 * decodifica ADPCM, cambia el tono, aplica envolventes, mezcla en la DMEM
 * del RSP y guarda el resultado en la RDRAM, de donde el juego lo pasa al
 * AI. Aquí se ejecuta la lista entera de una vez: en el SPE 1 de la PS3
 * (src/spu/audio, E/S por DMA), en el PPE o en el host (backend síncrono,
 * pruebas). Mismo código en los tres; nada depende de PSL1GHT.
 *
 * Adaptación de mupen64plus-rsp-hle (GPLv2 o posterior; avisos de copyright
 * en alist.c y abi.c, origen en docs/THIRD_PARTY.md).
 */
#ifndef SALSA64_RSP_AUDIO_AUDIO_H
#define SALSA64_RSP_AUDIO_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

/* ---- Trabajo y resultado ---------------------------------------------------
 * Caben en el descriptor y el resultado de 112 bytes de lib/spukit. */

#define AUD_JOB_TASK  2  /* kind del trabajo en spukit: una tarea */
#define AUD_JOB_RESET 3  /* volver el estado de los ABI a cero (aud_init) */
/* Estados guardados: el estado entre tareas (state.c) de y hacia
 * AUD_STATE_BYTES bytes en `ea` (alineado a 128), en un formato portable.
 * Entre tareas. */
#define AUD_JOB_STATE_SAVE 4
#define AUD_JOB_STATE_LOAD 5
#define AUD_STATE_BYTES 12032u

/* Conserva la aproximacion anterior cuando no caben todos los rangos
 * escritos en aud_result: agrandar siempre el ultimo. */
#define AUD_JOB_RANGES_OLD 0x00000001u
/* POLEF de ABI1 con A_INIT como en mupen64plus-rsp-hle (estado inicial cero)
 * en vez de como el microcódigo real, que lo toma de la zona de estados de
 * la DMEM que dejó el último RESAMPLE o ENVMIXER (ver aud_state.scratch). */
#define AUD_JOB_POLEF_INIT_ZERO 0x00000002u

struct aud_state_job {
	uint64_t ea;
};

struct aud_job {
	uint64_t rdram_ea;   /* dirección de la RDRAM emulada (EA del PPE) */
	uint32_t rdram_size;
	uint32_t abi;        /* enum salsa64_abi */
	uint32_t alist;      /* data_ptr del OSTask: dirección física de la lista */
	uint32_t alist_size; /* data_size: bytes (en MusyX, cuántas subtramas) */
	uint32_t flags;
	uint32_t ucode_data; /* ucode_data del OSTask: dirección física del segmento de datos del microcódigo */
	uint32_t ucode_data_size;  /* ucode_data_size: bytes que el arranque de libultra copia a la DMEM */
	uint32_t reserved[7];
};

/* Partes de la RDRAM que escribió la tarea (para el recompilador). Si son
 * más, se juntan los dos tramos separados por el hueco menor. El resultado
 * debe caber en el de spukit (112 bytes): solo las herramientas del host
 * (--lle-check-audio) compilan con más. */
#ifndef AUD_MAX_RANGES
#define AUD_MAX_RANGES 10
#endif

struct aud_result {
	uint32_t commands;      /* comandos ejecutados */
	uint32_t unknown;       /* comandos sin implementar */
	uint32_t last_unknown;  /* el último (w1) */
	uint32_t errors;        /* ABI sin HLE, direcciones fuera de la RDRAM */
	uint32_t nranges;
	uint32_t reserved[3];
	uint32_t range_addr[AUD_MAX_RANGES];
	uint32_t range_len[AUD_MAX_RANGES];
};

/* ---- Estado ----------------------------------------------------------------
 * La DMEM del microcódigo (4 KB, con margen para que ningún comando se
 * salga del búfer) y el estado de cada familia de ABI, que persiste entre
 * tareas como en mupen64plus-rsp-hle. ~12 KB; en el SPE vive en el local
 * store. */

#define AUD_DMEM_SIZE 0x1000u
#define AUD_DMEM_SLACK 0x1040u  /* un comando empieza en < 4 KB y abarca ≤ 4 KB */

struct aud_abi1 {           /* ABI1: Super Mario 64, GoldenEye… */
	uint32_t segments[16];
	uint16_t in, out, count;
	uint16_t dry_right, wet_left, wet_right;
	int16_t dry, wet;
	int16_t vol[2], target[2];
	int32_t rate[2];
	uint32_t loop;
	/* Libro de códigos ADPCM (hasta 8 predictores × 16 coeficientes) y
	 * coeficientes de POLEF. ADPCM indexa con el nibble del cuadro: hasta
	 * 16 predictores, así que la tabla da para 16 (el resto en cero). */
	int16_t table[16 * 16];
};

struct aud_nead {           /* nead (ABI2): Zelda OoT, Mario Kart 64, Star Fox 64… */
	uint16_t in, out, count;
	uint16_t env_values[3], env_steps[3];
	uint32_t loop;
	uint16_t filter_count;
	uint32_t filter_lut[2];
	int16_t table[16 * 16];  /* como en ABI1 */
	bool vco3;               /* acarreo del carril 3 que dejó el último MIXER (ver N_MIXER); no se guarda */
};

struct aud_naudio {         /* naudio: Banjo-Kazooie, Conker, Perfect Dark, Donkey Kong 64… */
	int16_t dry, wet;
	int16_t vol[2], target[2];
	int32_t rate[2];
	uint32_t loop;
	uint32_t mp3_addr;       /* MP3ADDY: búfer de estado de la corriente de MP3 en la RDRAM (0: sin fijar) */
#ifdef AUD_DEBUG
	bool mp3_seen;           /* el censo cuenta las tareas con MP3 */
#endif
	int16_t table[16 * 16];  /* libro de ADPCM y coeficientes de POLEF/IIR, como en ABI1 */
};

/* Memoria de trabajo de MP3 (naudio de Perfect Dark, Banjo-Tooie y Conker):
 * las direcciones 0x800–0xFFF de la DMEM del decodificador, aparte de la
 * de las listas como en mupen64plus-rsp-hle. 0x8A0–0xCDF guarda las 16
 * últimas salidas de la DCT (la ventana de síntesis), que siguen de un
 * comando MP3 al siguiente. */
#define AUD_MP3_BASE 0x800u
#define AUD_MP3_SIZE 0x800u
#define AUD_MP3_RING 0x8A0u        /* el anillo de la ventana (la salida de la DCT) */
#define AUD_MP3_RING_SIZE 0x440u

struct aud_job;
struct aud_result;

struct aud_state {
	uint8_t dmem[AUD_DMEM_SIZE + AUD_DMEM_SLACK] __attribute__((aligned(16)));
	struct aud_abi1 abi1;
	struct aud_nead nead;
	struct aud_naudio naudio;
	uint8_t mp3[AUD_MP3_SIZE] __attribute__((aligned(16)));
	/* Tabla de interpolación del cambio de tono que el microcódigo trae en
	 * su segmento de datos (alist.c, aud_find_lut): sin buscar (lut_src ==
	 * 0), no encontrada (lut_ok == false: vale la de lut.c) o hallada. */
	int16_t lut[64 * 4];
	uint32_t lut_src;
	bool lut_ok;
	/* Zona de estados del microcódigo de ABI1 en la DMEM (0xF90; 0 en los
	 * demás ABI y con AUD_JOB_POLEF_INIT_ZERO): ahí deja RESAMPLE sus 4
	 * muestras y el acumulador y ENVMIXER sus 80 bytes de estado, y de ahí
	 * (+4, +6) toma POLEF con A_INIT las dos muestras previas en vez de
	 * ponerlas a cero (medido con el microcódigo real: docs/audio-censo.md).
	 * La HLE la lleva en su propia DMEM. Se fija en cada tarea; no se guarda. */
	uint16_t scratch;
	const struct aud_job *job;
	struct aud_result *res;
	void *io;               /* estado de la plataforma (io_mem.c, src/spu/audio) */
};

/* Depuración del oráculo (--lle-check-audio; el host compila con -DAUD_DEBUG):
 * direcciones de los estados de ENVMIXER que escribió la tarea. La parte
 * fraccionaria del volumen de un carril ya recortado al destino sale de
 * registros vectoriales que el microcódigo real no limpia (con una tasa
 * saturada, como 0x7FFFFFFF, cambia de un arranque a otro), así que el
 * oráculo no la compara. */
#ifdef AUD_DEBUG
#define AUD_DBG_ENV_MAX 512
extern uint32_t aud_dbg_env[AUD_DBG_ENV_MAX];
extern uint32_t aud_dbg_mp3, aud_dbg_mp3_tasks;   /* comandos MP3 y tareas que los tienen */
extern uint32_t aud_dbg_env_n;
#endif

void aud_init(struct aud_state *st);
/* El estado entre tareas en el formato de AUD_STATE_BYTES (state.c), y de
 * vuelta (false si el formato no es este). */
void aud_state_save(const struct aud_state *st, uint8_t *out);
bool aud_state_load(struct aud_state *st, const uint8_t *in);
/* Ejecuta la lista de audio del trabajo y llena `res`. */
void aud_run(struct aud_state *st, const struct aud_job *job, struct aud_result *res);

#endif
