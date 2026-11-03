/* SPDX-License-Identifier: GPL-2.0-or-later */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-rsp-hle - alist.c                                         *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2014 Bobby Smiles                                       *
 *   Copyright (C) 2009 Richard Goedeken                                   *
 *   Copyright (C) 2002 Hacktarux                                          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Salsa64 — operaciones comunes de las listas de audio. Adaptado de
 * src/alist.c y src/audio.c de mupen64plus-rsp-hle (commit 8a7a472a,
 * 2026-06-23) para Salsa64 (2026-10-10):
 *
 *   - La DMEM y la RDRAM se leen en el orden de bytes de la N64 con índices
 *     de la N64 (el original usa palabras del host con XOR en los índices,
 *     y FILTER e INTERLEAVE dependen de que el host sea little-endian).
 *   - La RDRAM no se toca directo: aud_load/aud_store (E/S por DMA en el
 *     SPE, memcpy en el host) y cada escritura se anota para el
 *     recompilador.
 *   - El formato del estado de ENVMIXER en la RDRAM es propio (solo lo lee
 *     la HLE); el de ADPCM, RESAMPLE, POLEF y FILTER es el de la N64.
 *   - La tabla de RESAMPLE no se incluye (lut.c es un diseño propio, solo
 *     para cuando no hay otra): se toma del segmento de datos del
 *     microcódigo del propio juego (aud_find_lut), que es la que usa el
 *     microcódigo real.
 *   - Las direcciones de la DMEM se toman módulo 4 KB.
 */
#include <string.h>

#include "rsp/audio/hle.h"

static int32_t vmulf(int16_t x, int16_t y)
{
	return ((int32_t)x * y + 0x4000) >> 15;
}

/* El microcódigo acumula con VMACF: lo que ya había se multiplica por 0x7FFF
 * (casi 1,0: un bit menos para los valores grandes) y se suma el producto,
 * con un solo redondeo al final:
 *   dst = sat((dst · 0x7FFF + src · gain + 0x4000) >> 15)
 * Comprobado contra el microcódigo real sobre destinos con datos (2560 de
 * 2560 muestras; sumar el producto redondeado a dst deja ±1). */
static void sample_mix(uint8_t *dst, int16_t src, int16_t gain)
{
	aud_st16(dst, aud_clamp16((aud_ld16(dst) * 0x7FFF + src * gain + 0x4000) >> 15));
}

/* ---- RDRAM ------------------------------------------------------------------ */

static uint32_t range_gap(uint32_t a, uint32_t ae, uint32_t b, uint32_t be)
{
	return ae < b ? b - ae : be < a ? a - be : 0;
}

static void range_join(struct aud_result *r, uint32_t i, uint32_t addr, uint32_t end)
{
	uint32_t a = r->range_addr[i], e = a + r->range_len[i];
	a = addr < a ? addr : a;
	e = end > e ? end : e;
	r->range_addr[i] = a;
	r->range_len[i] = e - a;
}

static void note_range(struct aud_state *st, uint32_t addr, uint32_t len)
{
	struct aud_result *r = st->res;
	uint32_t end = addr + len;
	for (uint32_t i = 0; i < r->nranges; i++) {
		uint32_t a = r->range_addr[i], e = a + r->range_len[i];
		if (addr <= e && end >= a) {  /* se toca o se pisa: juntar */
			range_join(r, i, addr, end);
			return;
		}
	}
	if (r->nranges < AUD_MAX_RANGES) {
		r->range_addr[r->nranges] = addr;
		r->range_len[r->nranges++] = len;
		return;
	}
	/* Retorno para comparar en la consola. */
	if (st->job->flags & AUD_JOB_RANGES_OLD) {
		range_join(r, AUD_MAX_RANGES - 1, addr, end);
		return;
	}
	/* Sin lugar: juntar el par que agrega menos espacio no escrito. El
	 * tramo nuevo participa como el indice AUD_MAX_RANGES. */
	uint32_t bi = 0, bj = AUD_MAX_RANGES, bg = UINT32_MAX;
	for (uint32_t i = 0; i < AUD_MAX_RANGES; i++) {
		uint32_t a = r->range_addr[i], e = a + r->range_len[i];
		uint32_t g = range_gap(a, e, addr, end);
		if (g < bg) {
			bi = i;
			bj = AUD_MAX_RANGES;
			bg = g;
		}
		for (uint32_t j = i + 1; j < AUD_MAX_RANGES; j++) {
			uint32_t b = r->range_addr[j], be = b + r->range_len[j];
			g = range_gap(a, e, b, be);
			if (g < bg) {
				bi = i;
				bj = j;
				bg = g;
			}
		}
	}
	if (bj == AUD_MAX_RANGES) {
		range_join(r, bi, addr, end);
		return;
	}
	uint32_t b = r->range_addr[bj], be = b + r->range_len[bj];
	range_join(r, bi, b, be);
	r->range_addr[bj] = addr;
	r->range_len[bj] = len;
}

bool aud_load(struct aud_state *st, void *dst, uint32_t addr, uint32_t size)
{
	addr &= 0xFFFFFF;
	if (aud_io_read(st, dst, addr, size))
		return true;
	memset(dst, 0, size);
	st->res->errors++;
	return false;
}

bool aud_store(struct aud_state *st, uint32_t addr, const void *src, uint32_t size)
{
	addr &= 0xFFFFFF;
	if (!size)
		return true;
	if (!aud_io_write(st, addr, src, size)) {
		st->res->errors++;
		return false;
	}
	note_range(st, addr, size);
	return true;
}

bool aud_load_s16(struct aud_state *st, int16_t *dst, uint32_t addr, uint32_t n)
{
	uint8_t buf[64];
	bool ok = true;
	while (n) {
		uint32_t k = n < 32 ? n : 32;
		ok &= aud_load(st, buf, addr, 2 * k);
		for (uint32_t i = 0; i < k; i++)
			*dst++ = aud_ld16(buf + 2 * i);
		addr += 2 * k;
		n -= k;
	}
	return ok;
}

bool aud_store_s16(struct aud_state *st, uint32_t addr, const int16_t *src, uint32_t n)
{
	uint8_t buf[64];
	bool ok = true;
	while (n) {
		uint32_t k = n < 32 ? n : 32;
		for (uint32_t i = 0; i < k; i++)
			aud_st16(buf + 2 * i, *src++);
		ok &= aud_store(st, addr, buf, 2 * k);
		addr += 2 * k;
		n -= k;
	}
	return ok;
}

/* ---- Movimientos ------------------------------------------------------------ */

void alist_clear(struct aud_state *st, uint16_t dmem, uint16_t count)
{
	memset(aud_dm(st, dmem), 0, count);
}

void alist_load(struct aud_state *st, uint16_t dmem, uint32_t address, uint16_t count)
{
	/* Las restricciones del DMA del RSP. */
	dmem &= ~3;
	address &= ~7u;
	aud_load(st, aud_dm(st, dmem), address, aud_align(count, 8));
}

void alist_save(struct aud_state *st, uint16_t dmem, uint32_t address, uint16_t count)
{
	dmem &= ~3;
	address &= ~7u;
	aud_store(st, address, aud_dm(st, dmem), aud_align(count, 8));
}

void alist_move(struct aud_state *st, uint16_t dmemo, uint16_t dmemi, uint16_t count)
{
	/* Byte a byte y hacia adelante: un tramo que se pisa repite el patrón. */
	while (count--)
		*aud_dm(st, dmemo++) = *aud_dm(st, dmemi++);
}

void alist_copy_every_other_sample(struct aud_state *st, uint16_t dmemo, uint16_t dmemi, uint16_t count)
{
	while (count--) {
		aud_st16(aud_dm(st, dmemo), aud_ld16(aud_dm(st, dmemi)));
		dmemo += 2;
		dmemi += 4;
	}
}

void alist_repeat64(struct aud_state *st, uint16_t dmemo, uint16_t dmemi, uint8_t count)
{
	uint8_t buf[128];
	memcpy(buf, aud_dm(st, dmemi), sizeof buf);
	while (count--) {
		memcpy(aud_dm(st, dmemo), buf, sizeof buf);
		dmemo += 128;
	}
}

void alist_copy_blocks(struct aud_state *st, uint16_t dmemo, uint16_t dmemi, uint16_t block_size, uint8_t count)
{
	int blocks = count;
	do {
		int left = block_size;
		do {
			memmove(aud_dm(st, dmemo), aud_dm(st, dmemi), 0x20);
			left -= 0x20;
			dmemi += 0x20;
			dmemo += 0x20;
		} while (left > 0);
	} while (--blocks > 0);
}

void alist_interleave(struct aud_state *st, uint16_t dmemo, uint16_t left, uint16_t right, uint16_t count)
{
	uint8_t *dst = aud_dm(st, dmemo);
	const uint8_t *l = aud_dm(st, left), *r = aud_dm(st, right);
	for (count >>= 2; count; count--) {
		int16_t l1 = aud_ld16(l), l2 = aud_ld16(l + 2);
		int16_t r1 = aud_ld16(r), r2 = aud_ld16(r + 2);
		aud_st16(dst, l1);
		aud_st16(dst + 2, r1);
		aud_st16(dst + 4, l2);
		aud_st16(dst + 6, r2);
		dst += 8;
		l += 4;
		r += 4;
	}
}

/* ---- Envolventes ------------------------------------------------------------
 *
 * ENVMIXER de ABI1 (Super Mario 64 y los demás con el microcódigo estándar de
 * libultra, el de GoldenEye y el de Diddy Kong Racing) y de naudio (Rare). El
 * estado en la RDRAM es el del microcódigo real, 80 bytes (ENVMIX_STATE de
 * libultra, 5 vectores de 8 × 16 bits):
 *   0x00  volumen izquierdo, parte entera de las 8 muestras del bloque
 *   0x10  volumen izquierdo, parte fraccionaria (16 bits)
 *   0x20  lo mismo para el derecho (entera) y 0x30 (fraccionaria)
 *   0x40  destino izq, tasa izq (alta, baja), destino der, tasa der (alta,
 *         baja), seco, húmedo
 * El volumen de cada canal se lleva por bloques de 8 muestras, con un valor
 * por muestra en 16.16. El bloque inicial vale
 *   v + (d · c_j >> 16),  c_j = (j + 1)·0x2000 (c_7 = 0xFFFF)
 * con d = v·(tasa − 1) en el exponencial (tasa 16.16 con signo, SM64) y d =
 * tasa en el lineal (GoldenEye, naudio); cada bloque siguiente multiplica los ocho
 * por la tasa (exponencial) o les suma la tasa (lineal), saturando a 32 bits
 * y, si se pasan del destino (hacia arriba con tasa ≥ 1,0 —0x10000—, hacia
 * abajo si no; en el lineal de naudio, hacia arriba con tasa ≥ 0), dejando
 * el destino en la parte entera y la fracción que salió (el microcódigo
 * decide la dirección por la tasa, no por el destino: en el de GoldenEye una
 * tasa lineal pequeña da el destino de inmediato). Cada muestra se
 * mezcla con ganancia vmulf(parte entera, seco o húmedo) al búfer de salida
 * (sample_mix). La tarea guarda los valores del último bloque usado (con un
 * solo bloque, los del siguiente). Hallado y comprobado contra el microcódigo
 * real con el LLE: más de mil configuraciones aleatorias idénticas en el
 * exponencial y 300 en el lineal, y las salidas de las tareas de Super Mario
 * 64. */

#ifdef AUD_DEBUG
uint32_t aud_dbg_env[AUD_DBG_ENV_MAX];
uint32_t aud_dbg_mp3, aud_dbg_mp3_tasks;
uint32_t aud_dbg_env_n;
#endif

static int64_t sat32(int64_t x)
{
	return x > 0x7FFFFFFFll ? 0x7FFFFFFFll : x < -0x80000000ll ? -0x80000000ll : x;
}

/* Un valor de volumen contra el destino: pasado, la parte entera es el destino. */
static int32_t env_clamp(int64_t lane, int32_t rate, int32_t target, int32_t up)
{
	int64_t t = (int64_t)target << 16;
	if (rate >= up ? lane > t : lane < t)
		lane = t | (lane & 0xFFFF);
	return (int32_t)lane;
}

static void envmix_lanes(struct aud_state *st, bool linear, int32_t up, bool phase, bool cbfd, bool init, bool aux, uint16_t dmem_dl, uint16_t dmem_dr,
			 uint16_t dmem_wl, uint16_t dmem_wr, uint16_t dmemi, uint16_t count, int16_t dry, int16_t wet,
			 const int16_t *vol, const int16_t *target, const int32_t *rate, uint32_t address)
{
	uint8_t s[80];
	int32_t lanes[2][8], tgt[2], rt[2];

	if (init) {
		for (int c = 0; c < 2; c++) {
			int64_t v = (int64_t)vol[c] * 65536;
			int64_t diff = linear ? (int64_t)rate[c] : ((v * (uint32_t)rate[c]) >> 16) - v;
			tgt[c] = target[c];
			rt[c] = rate[c];
			for (int j = 0; j < 8; j++) {
				int64_t m = j < 7 ? (j + 1) * 0x2000 : 0xFFFF;
				lanes[c][j] = env_clamp(sat32(v + ((diff * m) >> 16)), rt[c], tgt[c], up);
			}
		}
	} else {
		aud_load(st, s, address, sizeof s);
		for (int c = 0; c < 2; c++)
			for (int j = 0; j < 8; j++)
				lanes[c][j] = (int32_t)((uint32_t)(uint16_t)salsa64_rd16(s + 0x20 * c + 2 * j) << 16 |
						       salsa64_rd16(s + 0x20 * c + 0x10 + 2 * j));
		tgt[0] = (int16_t)salsa64_rd16(s + 0x40);
		rt[0] = (int32_t)((uint32_t)salsa64_rd16(s + 0x42) << 16 | salsa64_rd16(s + 0x44));
		tgt[1] = (int16_t)salsa64_rd16(s + 0x46);
		rt[1] = (int32_t)((uint32_t)salsa64_rd16(s + 0x48) << 16 | salsa64_rd16(s + 0x4A));
		dry = (int16_t)salsa64_rd16(s + 0x4C);
		wet = (int16_t)salsa64_rd16(s + 0x4E);
	}

	unsigned n = aux ? 4 : 2, blocks = ((unsigned)count + 15u) / 16u;
	const uint8_t *in = aud_dm(st, dmemi);
	uint8_t *buf[4] = { aud_dm(st, dmem_dl), aud_dm(st, dmem_dr), aud_dm(st, dmem_wl), aud_dm(st, dmem_wr) };
	for (unsigned b = 0; b < blocks; b++) {
		if (b > 0 || !init)
			for (int c = 0; c < 2; c++)
				for (int j = 0; j < 8; j++) {
					int64_t l = lanes[c][j];
					l = linear ? l + rt[c] : (l * (uint32_t)rt[c]) >> 16;
					lanes[c][j] = env_clamp(sat32(l), rt[c], tgt[c], up);
				}
		for (unsigned j = 0; j < 8; j++) {
			unsigned k = 8 * b + j;
			int16_t l = (int16_t)(lanes[0][j] >> 16), r = (int16_t)(lanes[1][j] >> 16);
			int16_t gains[4] = { aud_clamp16(vmulf(l, dry)), aud_clamp16(vmulf(r, dry)), aud_clamp16(vmulf(l, wet)),
					     aud_clamp16(vmulf(r, wet)) };
			int16_t x = aud_ld16(in + 2 * k);
			/* Con fase, el bit bajo de seco (izquierda) o de húmedo (derecha) invierte la entrada
			 * (complemento a uno) de ese canal. */
			int16_t xs[2] = { phase && (dry & 1) ? (int16_t)~x : x, phase && (wet & 1) ? (int16_t)~x : x };
			if (cbfd) {
				/* Conker: la muestra se multiplica (VMULF) por la polaridad de cada canal, 0x7FFF o
				 * 0x8000 según el bit bajo de seco o húmedo. Con el bit 1 en cero, el primer bloque
				 * usa 0x7FFF por la polaridad (0x7FFE o 0x8002: se nota ±1 en las 8 primeras
				 * muestras) y con el bit 1 en uno, el vector de rampa de entrada que el microcódigo
				 * trae en 0xA0 de su segmento de datos; desde el segundo bloque, la polaridad sola. */
				int16_t pol[2] = { (dry & 1) ? INT16_MIN : INT16_MAX, (wet & 1) ? INT16_MIN : INT16_MAX };
				int16_t sc[2] = { pol[0], pol[1] };
				if (b == 0) {
					int16_t ramp = aud_ld16(aud_dm(st, 0xA0 + 2 * j));
					sc[0] = (int16_t)aud_clamp16(vmulf((dry & 2) ? ramp : INT16_MAX, pol[0]));
					sc[1] = (int16_t)aud_clamp16(vmulf((wet & 2) ? ramp : INT16_MAX, pol[1]));
				}
				xs[0] = (int16_t)aud_clamp16(vmulf(x, sc[0]));
				xs[1] = (int16_t)aud_clamp16(vmulf(x, sc[1]));
			}
			for (unsigned o = 0; o < n; o++)
				sample_mix(buf[o] + 2 * k, xs[o & 1], gains[o]);
		}
	}

	for (int c = 0; c < 2; c++)
		for (int j = 0; j < 8; j++) {
			salsa64_wr16(s + 0x20 * c + 2 * j, (uint16_t)((uint32_t)lanes[c][j] >> 16));
			salsa64_wr16(s + 0x20 * c + 0x10 + 2 * j, (uint16_t)lanes[c][j]);
		}
	salsa64_wr16(s + 0x40, (uint16_t)tgt[0]);
	salsa64_wr16(s + 0x42, (uint16_t)((uint32_t)rt[0] >> 16));
	salsa64_wr16(s + 0x44, (uint16_t)rt[0]);
	salsa64_wr16(s + 0x46, (uint16_t)tgt[1]);
	salsa64_wr16(s + 0x48, (uint16_t)((uint32_t)rt[1] >> 16));
	salsa64_wr16(s + 0x4A, (uint16_t)rt[1]);
	salsa64_wr16(s + 0x4C, (uint16_t)dry);
	salsa64_wr16(s + 0x4E, (uint16_t)wet);
	aud_store(st, address, s, sizeof s);
	if (st->scratch)  /* el microcódigo de ABI1 arma el estado en su zona de la DMEM y de ahí lo guarda */
		memcpy(aud_dm(st, st->scratch), s, sizeof s);
#ifdef AUD_DEBUG
	if (aud_dbg_env_n < AUD_DBG_ENV_MAX)
		aud_dbg_env[aud_dbg_env_n++] = address & 0xFFFFFF;
#endif
}

void alist_envmix(struct aud_state *st, enum aud_envmix_kind kind, bool init, bool aux,
		  uint16_t dmem_dl, uint16_t dmem_dr, uint16_t dmem_wl, uint16_t dmem_wr, uint16_t dmemi,
		  uint16_t count, int16_t dry, int16_t wet, const int16_t *vol, const int16_t *target,
		  const int32_t *rate, uint32_t address)
{
	/* El lineal de naudio siempre mezcla en los cuatro búferes. */
	bool naudio = kind == AUD_ENVMIX_LIN || kind == AUD_ENVMIX_LIN_PHASE || kind == AUD_ENVMIX_LIN_CBFD;
	envmix_lanes(st, kind != AUD_ENVMIX_EXP, naudio ? 0 : 0x10000, kind == AUD_ENVMIX_LIN_PHASE || kind == AUD_ENVMIX_LIN_CBFD,
		     kind == AUD_ENVMIX_LIN_CBFD, init, aux || naudio,
		     dmem_dl, dmem_dr, dmem_wl, dmem_wr, dmemi, count, dry, wet, vol, target, rate, address);
}

/* `align`: la cuenta se redondea hacia arriba a múltiplos de 8 muestras en
 * Mario Kart 64 y de 16 en los demás nead (el microcódigo procesa dos
 * bloques por vuelta: con 184 muestras sale un bloque de más en cada búfer;
 * medido con el LLE en Star Fox 64 y en Zelda). */
void alist_envmix_nead(struct aud_state *st, bool swap_wet_lr, uint16_t dmem_dl, uint16_t dmem_dr,
		       uint16_t dmem_wl, uint16_t dmem_wr, uint16_t dmemi, unsigned count, unsigned align,
		       uint16_t *env_values, const uint16_t *env_steps, const int16_t *xors)
{
	const uint8_t *in = aud_dm(st, dmemi);
	uint8_t *dl = aud_dm(st, dmem_dl), *dr = aud_dm(st, dmem_dr);
	uint8_t *wl = aud_dm(st, dmem_wl), *wr = aud_dm(st, dmem_wr);
	if (swap_wet_lr) {
		uint8_t *t = wl;
		wl = wr;
		wr = t;
	}
	for (count = aud_align(count, align); count; count -= 8) {
		for (unsigned i = 0; i < 8; i++) {
			int32_t x = aud_ld16(in + 2 * i);
			int16_t l = (int16_t)((x * (int32_t)env_values[0]) >> 16) ^ xors[0];
			int16_t r = (int16_t)((x * (int32_t)env_values[1]) >> 16) ^ xors[1];
			int16_t l2 = (int16_t)(((int32_t)l * (int32_t)env_values[2]) >> 16) ^ xors[2];
			int16_t r2 = (int16_t)(((int32_t)r * (int32_t)env_values[2]) >> 16) ^ xors[3];
			aud_st16(dl + 2 * i, aud_clamp16(aud_ld16(dl + 2 * i) + l));
			aud_st16(dr + 2 * i, aud_clamp16(aud_ld16(dr + 2 * i) + r));
			aud_st16(wl + 2 * i, aud_clamp16(aud_ld16(wl + 2 * i) + l2));
			aud_st16(wr + 2 * i, aud_clamp16(aud_ld16(wr + 2 * i) + r2));
		}
		env_values[0] += env_steps[0];
		env_values[1] += env_steps[1];
		env_values[2] += env_steps[2];
		dl += 16;
		dr += 16;
		wl += 16;
		wr += 16;
		in += 16;
	}
}

/* ---- Mezclas ---------------------------------------------------------------- */

/* MIXER acumula como el ENVMIXER (sample_mix): lo que había × 0x7FFF más el
 * producto, con un solo redondeo. El truncado de mupen64plus-rsp-hle y el
 * redondeo del producto solo dejaban ±1 LSB de diferencia con el microcódigo
 * real (comparado en el LLE con Zelda OoT, Perfect Dark y Super Mario 64). */
void alist_mix(struct aud_state *st, uint16_t dmemo, uint16_t dmemi, uint16_t count, int16_t gain)
{
	uint8_t *dst = aud_dm(st, dmemo);
	const uint8_t *src = aud_dm(st, dmemi);
	for (count >>= 1; count; count--, dst += 2, src += 2)
		sample_mix(dst, aud_ld16(src), gain);
}

void alist_multQ44(struct aud_state *st, uint16_t dmem, uint16_t count, int8_t gain)
{
	uint8_t *dst = aud_dm(st, dmem);
	for (count >>= 1; count; count--, dst += 2)
		aud_st16(dst, aud_clamp16(aud_ld16(dst) * gain >> 4));
}

/* ADDMIXER: suma saturada. El microcódigo suma con VADD, que añade el acarreo
 * que haya en VCO; el MIXER anterior lo deja puesto en el carril 3 (en todos
 * los nead: medido con el LLE, independiente de la ganancia y de los datos), y
 * el primer vector de la suma, con el carril 3 en las muestras 3 (mod 8) de la
 * dirección alineada, sale +1. VADD lo consume. */
void alist_add(struct aud_state *st, uint16_t dmemo, uint16_t dmemi, uint16_t count, bool carry3)
{
	uint8_t *dst = aud_dm(st, dmemo);
	const uint8_t *src = aud_dm(st, dmemi);
	unsigned carry_at = carry3 ? (3u - (dmemo >> 1)) & 7u : ~0u;
	for (unsigned k = 0, n = count >> 1; k < n; k++, dst += 2, src += 2)
		aud_st16(dst, aud_clamp16(aud_ld16(dst) + aud_ld16(src) + (k == carry_at ? 1 : 0)));
}

/* ---- Tabla de interpolación ------------------------------------------------------
 * El cambio de tono usa 64 filas de 4 coeficientes (Q15) que el microcódigo
 * trae en su segmento de datos (ucode_data: en 0xB0–0x138 según el ABI y la
 * compilación). Se busca ahí, en la RDRAM del juego, y se reconoce por su
 * estructura: cada fila es la 63−i al revés y suma 1,0 en Q15 (±256). Si no
 * aparece (una RDRAM de prueba, otra compilación), vale la de lut.c, que da
 * lo mismo salvo por la respuesta en frecuencia. Nada del microcódigo se
 * copia al repositorio ni sale de la RDRAM del emulador. */

enum { LUT_ROWS = 64, LUT_BYTES = LUT_ROWS * 4 * 2, UCODE_DATA_WINDOW = 0x800 };

static bool lut_at(const uint8_t *d)
{
	for (unsigned r = 0; r < LUT_ROWS; r++) {
		const uint8_t *a = d + 8 * r, *b = d + 8 * (LUT_ROWS - 1 - r);
		int32_t sum = 0;
		for (unsigned k = 0; k < 4; k++) {
			if (salsa64_rd16(a + 2 * k) != salsa64_rd16(b + 2 * (3 - k)))
				return false;
			sum += (int16_t)salsa64_rd16(a + 2 * k);
		}
		if (sum < 32768 - 256 || sum > 32768 + 256)
			return false;
	}
	return true;
}

/* Segmento de datos del microcódigo a la DMEM, como el arranque de libultra. */
void aud_boot_data(struct aud_state *st)
{
	uint32_t ucode_data = st->job->ucode_data & 0xFFFFFF, size = st->job->ucode_data_size;

	if (ucode_data == 0 || size == 0)
		return;
	size = aud_align(size > AUD_DMEM_SIZE ? AUD_DMEM_SIZE : size, 8);
	if (ucode_data > st->job->rdram_size || size > st->job->rdram_size - ucode_data)
		return;
	aud_load(st, st->dmem, ucode_data, size);
}

void aud_find_lut(struct aud_state *st)
{
	uint32_t ucode_data = st->job->ucode_data & 0xFFFFFF;
	uint8_t d[UCODE_DATA_WINDOW];

	if (st->lut_src == (ucode_data | 0x80000000u))
		return;
	st->lut_src = ucode_data | 0x80000000u;
	st->lut_ok = false;
	if (ucode_data == 0 || ucode_data > st->job->rdram_size - UCODE_DATA_WINDOW)
		return;
	if (!aud_load(st, d, ucode_data, sizeof d))
		return;
	for (unsigned o = 0; o + LUT_BYTES <= UCODE_DATA_WINDOW; o += 2) {
		/* Descarte rápido con la primera y la última fila. */
		bool first = true;
		for (unsigned k = 0; k < 4 && first; k++)
			first = salsa64_rd16(d + o + 2 * k) == salsa64_rd16(d + o + 8 * (LUT_ROWS - 1) + 2 * (3 - k));
		if (!first || !lut_at(d + o))
			continue;
		for (unsigned i = 0; i < LUT_ROWS * 4; i++)
			st->lut[i] = (int16_t)salsa64_rd16(d + o + 2 * i);
		st->lut_ok = true;
		return;
	}
}

/* ---- Cambio de tono --------------------------------------------------------- */

/* Estado de RESAMPLE en la RDRAM: las 4 muestras previas y el acumulador. */
void alist_resample(struct aud_state *st, bool init, uint16_t dmemo, uint16_t dmemi, uint16_t count,
		    uint32_t pitch, uint32_t address)
{
	uint32_t ipos = (uint32_t)(dmemi >> 1) - 4, opos = dmemo >> 1;
	uint32_t accu;
	int16_t state[5];

	if (init) {
		for (unsigned k = 0; k < 4; k++)
			aud_set_sample(st, ipos + k, 0);
		accu = 0;
	} else {
		aud_load_s16(st, state, address, 5);
		for (unsigned k = 0; k < 4; k++)
			aud_set_sample(st, ipos + k, state[k]);
		accu = (uint16_t)state[4];
	}

	const int16_t *const lut_base = aud_lut(st);
	for (count >>= 1; count; count--) {
		const int16_t *lut = lut_base + ((accu & 0xFC00) >> 8);
		int32_t v = ((aud_sample(st, ipos) * lut[0] + 0x4000) >> 15) + ((aud_sample(st, ipos + 1) * lut[1] + 0x4000) >> 15) +
			    ((aud_sample(st, ipos + 2) * lut[2] + 0x4000) >> 15) + ((aud_sample(st, ipos + 3) * lut[3] + 0x4000) >> 15);
		aud_set_sample(st, opos++, aud_clamp16(v));
		accu += pitch;
		ipos += accu >> 16;
		accu &= 0xFFFF;
	}

	for (unsigned k = 0; k < 4; k++)
		state[k] = aud_sample(st, ipos + k);
	state[4] = (int16_t)accu;
	aud_store_s16(st, address, state, 5);
	if (st->scratch) {  /* ABI1: el estado queda también en la zona de estados de la DMEM */
		uint8_t *d = aud_dm(st, st->scratch);
		for (unsigned k = 0; k < 5; k++)
			aud_st16(d + 2 * k, state[k]);
	}
}

void alist_resample_zoh(struct aud_state *st, uint16_t dmemo, uint16_t dmemi, uint16_t count,
			uint32_t pitch, uint32_t pitch_accu)
{
	uint32_t ipos = dmemi >> 1, opos = dmemo >> 1;
	for (count >>= 1; count; count--) {
		aud_set_sample(st, opos++, aud_sample(st, ipos));
		pitch_accu += pitch;
		ipos += pitch_accu >> 16;
		pitch_accu &= 0xFFFF;
	}
}

/* ---- ADPCM -------------------------------------------------------------------
 * Cuadros de 9 bytes (4 bits por muestra) o 5 (2 bits): un byte con la
 * escala y el predictor y 16 residuos. Estado: las 16 últimas muestras. */

static int16_t predict_sample(uint8_t byte, uint8_t mask, unsigned lshift, unsigned rshift)
{
	int16_t sample = (int16_t)((uint16_t)(byte & mask) << lshift);
	return (int16_t)(sample >> rshift);
}

static int32_t rdot(size_t n, const int16_t *x, const int16_t *y)
{
	int32_t accu = 0;
	y += n;
	while (n--)
		accu += *x++ * *--y;
	return accu;
}

static void compute_residuals(int16_t *dst, const int16_t *src, const int16_t *cb_entry,
			      const int16_t *last_samples)
{
	const int16_t *book1 = cb_entry, *book2 = cb_entry + 8;
	int16_t l1 = last_samples[0], l2 = last_samples[1];
	for (size_t i = 0; i < 8; i++) {
		int32_t accu = src[i] * 2048;  /* << 11 sin desplazar negativos */
		accu += book1[i] * l1 + book2[i] * l2 + rdot(i, book2, src);
		dst[i] = aud_clamp16(accu >> 11);
	}
}

void alist_adpcm(struct aud_state *st, bool init, bool loop, bool two_bit_per_sample, uint16_t dmemo,
		 uint16_t dmemi, uint16_t count, const int16_t *codebook, uint32_t loop_address,
		 uint32_t last_frame_address)
{
	int16_t last[16];

	if (init)
		memset(last, 0, sizeof last);
	else
		aud_load_s16(st, last, loop ? loop_address : last_frame_address, 16);

	for (unsigned i = 0; i < 16; i++, dmemo += 2)
		aud_st16(aud_dm(st, dmemo), last[i]);

	for (; count >= 32; count -= 32) {
		int16_t frame[16];
		uint8_t code = *aud_dm(st, dmemi++);
		unsigned scale = code >> 4;
		const int16_t *cb = codebook + ((code & 0xF) << 4);

		if (two_bit_per_sample) {
			unsigned rshift = scale < 14 ? 14 - scale : 0;
			for (unsigned i = 0; i < 4; i++) {
				uint8_t b = *aud_dm(st, dmemi++);
				frame[4 * i + 0] = predict_sample(b, 0xC0, 8, rshift);
				frame[4 * i + 1] = predict_sample(b, 0x30, 10, rshift);
				frame[4 * i + 2] = predict_sample(b, 0x0C, 12, rshift);
				frame[4 * i + 3] = predict_sample(b, 0x03, 14, rshift);
			}
		} else {
			unsigned rshift = scale < 12 ? 12 - scale : 0;
			for (unsigned i = 0; i < 8; i++) {
				uint8_t b = *aud_dm(st, dmemi++);
				frame[2 * i + 0] = predict_sample(b, 0xF0, 8, rshift);
				frame[2 * i + 1] = predict_sample(b, 0x0F, 12, rshift);
			}
		}

		compute_residuals(last, frame, cb, last + 14);
		compute_residuals(last + 8, frame + 8, cb, last + 6);

		for (unsigned i = 0; i < 16; i++, dmemo += 2)
			aud_st16(aud_dm(st, dmemo), last[i]);
	}

	aud_store_s16(st, last_frame_address, last, 16);
}

/* ---- Filtros -----------------------------------------------------------------
 *
 * FILTER (nead): FIR de 8 coeficientes sobre bloques de 8 muestras. En
 * índices de la N64, con X = las 8 muestras guardadas seguidas del bloque:
 *   out[n] = Σ_{t=0..7} L[t] · X[8 + n − t],  redondeado (+0x4000) >> 15
 * Coeficientes: la tabla de la nota (lut_address[0]) se acerca a la mitad
 * de camino a la de la llamada anterior, guardada detrás del estado
 * (lut_address[1]): L = (anterior + nota + 1) >> 1, que queda escrita ahí;
 * la de la nota no se toca. (Yoshi's Story y 1080°, con otra compilación del
 * microcódigo, usan la tabla de la nota tal cual y no escriben nada: medido
 * con el LLE, la salida del real es exactamente el doble de la promediada
 * con una anterior en cero.) Así lo hace el microcódigo de Zelda OoT
 * ejecutado en el LLE (rama del LLE del RSP, --lle-check-audio): mupen64plus-
 * rsp-hle escribe el promedio redondeado hacia abajo en las dos tablas, y
 * con la anterior en cero el filtro sonaba a la mitad (−6 dB). */
void alist_filter(struct aud_state *st, bool init, uint16_t dmem, uint16_t count, uint32_t address,
		  const uint32_t *lut_address, bool average)
{
	int16_t lut6[8], lut5[8], x[16];
	uint8_t *io = aud_dm(st, dmem);

	if (count > 0x780)  /* lo que cabe en el búfer del original */
		count = 0x780;
	aud_load_s16(st, lut6, lut_address[0], 8);
	if (average) {
		if (init)
			memset(lut5, 0, sizeof lut5);   /* A_INIT: la tabla anterior vale cero */
		else
			aud_load_s16(st, lut5, lut_address[1], 8);
		for (unsigned i = 0; i < 8; i++)
			lut5[i] = (int16_t)((lut5[i] + lut6[i] + 1) >> 1);
		aud_store_s16(st, lut_address[1], lut5, 8);
	} else {
		/* Yoshi's Story y 1080°: la tabla de la nota va directa, sin la anterior ni el guardado. */
		memcpy(lut5, lut6, sizeof lut5);
	}

	if (init)
		for (unsigned i = 0; i < 8; i++)
			x[i] = 0;                    /* las muestras previas */
	else
		aud_load_s16(st, x, address, 8);
	for (unsigned blk = 0; blk < count; blk += 16, io += 16) {
		for (unsigned i = 0; i < 8; i++)
			x[8 + i] = aud_ld16(io + 2 * i);
		for (unsigned n = 0; n < 8 && blk + 2 * n < count; n++) {
			int32_t v = 0;
			for (unsigned t = 0; t < 8; t++)
				v += lut5[t] * x[8 + n - t];
			aud_st16(io + 2 * n, (int16_t)((v + 0x4000) >> 15));
		}
		memcpy(x, x + 8, 8 * sizeof x[0]);
	}
	aud_store_s16(st, address, x, 8);
}

/* POLEF: filtro de dos polos por bloques de 8 (coeficientes en `table`).
 * Estado en la RDRAM: las 4 últimas muestras; se releen la 3.ª y la 4.ª.
 *
 * Con A_INIT, el microcódigo de ABI1 (las tres compilaciones) no pone a cero
 * las dos muestras previas: pone a cero los 4 primeros bytes de su zona de
 * estados de la DMEM (st->scratch, 0xF90) y toma l1 y l2 de los bytes +4 y
 * +6, que son lo que dejó ahí el último RESAMPLE (sus muestras 3.ª y 4.ª) o
 * ENVMIXER (los carriles 2 y 3 del volumen izquierdo). Medido con el
 * microcódigo real en Donald Duck: Quack Attack (la única tarea distinta de
 * 3855) y en la primera tarea de Diddy Kong Racing, donde la DMEM recién
 * encendida vale 0xFFFF y el filtro arrastra −1 (el >> 14 redondea hacia
 * abajo) durante toda la tarea; mupen64plus-rsp-hle parte de cero. */
void alist_polef(struct aud_state *st, bool init, uint16_t dmemo, uint16_t dmemi, uint16_t count,
		 uint16_t gain, int16_t *table, uint32_t address)
{
	uint8_t *dst = aud_dm(st, dmemo);
	const int16_t *h1 = table;
	int16_t *h2 = table + 8;
	int16_t l1 = 0, l2 = 0, h2_before[8], frame[8], out[8] = { 0 };

	count = (uint16_t)aud_align(count, 16);
	if (!count)
		return;
	if (!init) {
		int16_t s[4];
		aud_load_s16(st, s, address, 4);
		l1 = s[2];
		l2 = s[3];
	} else if (st->scratch) {
		l1 = aud_ld16(aud_dm(st, st->scratch + 4));
		l2 = aud_ld16(aud_dm(st, st->scratch + 6));
	}
	if (st->scratch)
		memset(aud_dm(st, st->scratch), 0, 4);
	for (unsigned i = 0; i < 8; i++) {
		h2_before[i] = h2[i];
		h2[i] = (int16_t)(((int32_t)h2[i] * gain) >> 14);
	}
	do {
		for (unsigned i = 0; i < 8; i++, dmemi += 2)
			frame[i] = aud_ld16(aud_dm(st, dmemi));
		for (unsigned i = 0; i < 8; i++) {
			int32_t accu = frame[i] * gain;
			accu += h1[i] * l1 + h2_before[i] * l2 + rdot(i, h2, frame);
			out[i] = aud_clamp16(accu >> 14);
			aud_st16(dst + 2 * i, out[i]);
		}
		l1 = out[6];
		l2 = out[7];
		dst += 16;
		count -= 16;
	} while (count != 0);
	aud_store_s16(st, address, out + 4, 4);
}

/* IIR (naudio de Conker y los de MP3): biquad sobre bloques de 8 con
 *   y[n] = c0·x[n] + c1·x[n−1] + c0·x[n−2] + 2·c8·y[n−1] + 2·c9·y[n−2]
 * (c = table[i]; cada producto redondeado a Q15 como VMULF) y la salida
 * truncada a 16 bits sin saturar, como en el original. Estado en la RDRAM:
 * y[n−2], y[n−1] en +4/+6 y x[n−2], x[n−1] en +8/+10. */
void alist_iirf(struct aud_state *st, bool init, uint16_t dmemo, uint16_t dmemi, uint16_t count,
		const int16_t *table, uint32_t address)
{
	uint8_t *dst = aud_dm(st, dmemo);
	int16_t y[8] = { 0 }, x[4] = { 0 };
	unsigned index = 7;

	if (!init) {
		int16_t s[4];
		aud_load_s16(st, s, address, 4);
		x[1] = s[0];
		x[2] = s[1];
		y[6] = s[2];
		y[7] = s[3];
	}
	int32_t prev = vmulf(table[9], y[6]) * 2;
	for (count = (uint16_t)aud_align(count, 16); count; count -= 16, dst += 16) {
		for (unsigned i = 0; i < 8; i++, dmemi += 2) {
			x[index & 3] = aud_ld16(aud_dm(st, dmemi));
			int32_t accu = prev + vmulf(table[0], x[index & 3]) + vmulf(table[1], x[(index - 1) & 3]) +
				       vmulf(table[0], x[(index - 2) & 3]);
			accu += vmulf(table[8], y[index]) * 2;
			prev = vmulf(table[9], y[index]) * 2;
			y[i] = (int16_t)accu;
			aud_st16(dst + 2 * i, y[i]);
			index = (index + 1) & 7;
		}
	}
	int16_t s[4] = { x[(index - 2) & 3], x[(index - 1) & 3], y[6], y[7] };
	aud_store_s16(st, address, s, 4);
}

/* OVERLOAD (distorsión de Conker): ganancia saturada y luego atenuación
 * (Q16 sin signo). `count` en bytes. */
void alist_overload(struct aud_state *st, uint16_t dmem, uint16_t count, int16_t gain, uint16_t attenuation)
{
	uint8_t *p = aud_dm(st, dmem);
	for (count >>= 1; count; count--, p += 2) {
		int32_t accu = aud_clamp16(aud_ld16(p) * gain);
		aud_st16(p, (int16_t)((accu * attenuation) >> 16));
	}
}
