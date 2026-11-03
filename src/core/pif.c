/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — PIF: comandos joybus en la PIF RAM. Ver pif.h.
 *
 * Escrito desde n64brew.dev (PIF-NUS, Joybus Protocol, consultadas el
 * 2026-10-10); no deriva de código de otros emuladores.
 */
#include "core/bus.h"

#include <string.h>

#include "core/mempak.h"
#include "core/save.h"

#define CMD_BYTE       63            /* byte de comandos del PIF */
#define CMD_JOYBUS     0x01          /* preparar los intercambios joybus */
#define CMD_CHALLENGE  0x02          /* desafío y respuesta del CIC */

/* Desafío del CIC 6105 (juegos de Rare como Banjo-Tooie descifran sus datos
 * con la respuesta): 30 nibbles (alto primero) en los bytes 48–62 de la PIF
 * RAM, que la PIF reemplaza por otros 30. Autómata con dos tablas, la clave
 * empieza en 0xB; verificado nibble por nibble con 5 desafíos medidos en
 * ares (150 nibbles) corriendo Banjo-Tooie. */
#define CHL_FIRST      48
#define CHL_BYTES      15
static void cic6105_challenge(uint8_t *ram)
{
	static const uint8_t lut0[16] = { 4, 7, 0xA, 7, 0xE, 5, 0xE, 1, 0xC, 0xF, 8, 0xF, 6, 3, 6, 9 };
	static const uint8_t lut1[16] = { 4, 1, 0xA, 7, 0xE, 5, 0xE, 1, 0xC, 9, 8, 5, 6, 3, 0xC, 9 };
	const uint8_t *lut = lut0;
	unsigned key = 0xB;
	for (unsigned i = 0; i < CHL_BYTES * 2; i++) {
		uint8_t *b = &ram[CHL_FIRST + i / 2];
		unsigned in = (i & 1) ? (*b & 0xF) : (*b >> 4);
		unsigned out = (key + 5 * in) & 0xF;
		*b = (uint8_t)((i & 1) ? (*b & 0xF0) | out : (*b & 0x0F) | (out << 4));
		key = lut[out];
		unsigned sgn = (out >> 3) & 1, mag = (sgn ? ~out : out) & 7;
		unsigned mod = (mag % 3 == 1) ? sgn : 1 - sgn;
		if (lut == lut1 && (out == 0x1 || out == 0x9))
			mod = 1;
		if (lut == lut1 && (out == 0xB || out == 0xE))
			mod = 0;
		lut = mod ? lut1 : lut0;
	}
}

#define ESC_SKIP       0x00          /* saltar al canal siguiente */
#define ESC_RESET      0xFD          /* señal de reset en la línea */
#define ESC_END        0xFE
#define ESC_NOP        0xFF

#define TX_SKIP        0x80          /* no ejecutar este intercambio */
#define LEN_MASK       0x3F
#define RX_NO_DEVICE   0x80
#define RX_SHORT       0x40          /* la respuesta trajo menos bytes de los pedidos */

/* Comandos del control estándar. */
#define JB_INFO        0x00
#define JB_STATE       0x01
#define JB_PAK_READ    0x02
#define JB_PAK_WRITE   0x03
#define JB_RESET       0xFF

#define PAK_BLOCK      32
#define PAD_ID_HI      0x05          /* control estándar: 0x0500 */
#define PAD_ID_LO      0x00
#define PAD_PAK        0x01          /* estado: hay accesorio */
#define PAD_NO_PAK     0x02          /* estado: no hay accesorio */
#define PAD_PAK_CRC    0x04          /* estado: la dirección del último comando venía mal verificada */

#define PAD_RESET_COMBO (SALSA64_PAD_L | SALSA64_PAD_R | SALSA64_PAD_START)

void salsa64_pif_reset(struct salsa64_bus *bus)
{
	struct salsa64_pif *pif = &bus->pif;
	for (int ch = 0; ch < SALSA64_PIF_CHANNELS; ch++)
		pif->handshake[ch] = -1;
	pif->parsed = 0;
	memset(pif->held, 0, sizeof pif->held);
	memset(pif->pak_err, 0, sizeof pif->pak_err);
	memset(pif->rumble, 0, sizeof pif->rumble);
	for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++)
		salsa64_transferpak_reset(&pif->transferpak_state[i]);
}

void salsa64_pif_set_pad(struct salsa64_bus *bus, unsigned port, const struct salsa64_pad *pad)
{
	if (port >= SALSA64_PIF_PORTS)
		return;
	/* Solo lo recién pulsado: retener todo lo pulsado alargaría cada
	 * pulsación hasta la lectura siguiente y se comería una soltada corta
	 * entre dos pulsaciones (A soltada 4 campos en SM64, PS3). */
	uint16_t pressed = (uint16_t)(pad->buttons & ~bus->pif.pads[port].buttons);
	bus->pif.held[port] = pad->connected ? (uint16_t)(bus->pif.held[port] | pressed) : 0;
	bus->pif.pads[port] = *pad;
}

uint8_t salsa64_joybus_crc8(const uint8_t *data, unsigned len)
{
	uint8_t crc = 0;
	for (unsigned i = 0; i < len; i++) {
		crc ^= data[i];
		for (int b = 0; b < 8; b++)
			crc = (uint8_t)((crc & 0x80) ? (crc << 1) ^ 0x85 : crc << 1);
	}
	return crc;
}

/* Recorre la PIF RAM y guarda el primer intercambio de cada canal. */
static void parse(struct salsa64_pif *pif, const uint8_t *ram)
{
	unsigned i = 0, ch = 0;
	for (int k = 0; k < SALSA64_PIF_CHANNELS; k++)
		pif->handshake[k] = -1;
	while (i < CMD_BYTE && ch < SALSA64_PIF_CHANNELS) {
		uint8_t b = ram[i];
		if (b == ESC_END)
			break;
		if (b == ESC_NOP || b == ESC_RESET) {
			i++;
			continue;
		}
		if (b == ESC_SKIP) {
			i++;
			ch++;
			continue;
		}
		if (i + 1 >= CMD_BYTE)
			break;
		unsigned len = 2u + (b & LEN_MASK) + (ram[i + 1] & LEN_MASK);
		if (i + len > CMD_BYTE)
			break;
		pif->handshake[ch++] = (int8_t)i;
		i += len;
	}
	pif->parsed = 1;
}

/* Lo que tarda la lectura PIF RAM → RDRAM: la PIF ejecuta los intercambios
 * joybus (unos 4 µs por bit en la línea, más su propio trabajo) antes de
 * contestar. Constantes de ares (`ares/n64/pif/hle.cpp`, ISC), a 187,5 MHz:
 * 13600 de base, 22000 por canal con control conectado, 18000 sin él, 20000
 * el cartucho y 1420 por cada byte de control corto; aquí ×3 y a ciclos del
 * VR4300 (÷2). Un control: 0,66 ms; cuatro: 1,65 ms; un bloque del
 * Controller Pak: 0,66 ms. */
uint32_t salsa64_pif_read_cycles(const struct salsa64_bus *bus)
{
	const uint8_t *ram = bus->pif_ram;
	uint32_t units = 13600, shorts = 0;
	unsigned i = 0, ch = 0;
	while (i < SALSA64_PIF_RAM_SIZE && ch < SALSA64_PIF_CHANNELS) {
		uint8_t b = ram[i++];
		if (b == ESC_END) {
			shorts++;
			break;
		}
		if (b == ESC_SKIP || b == ESC_RESET) {
			shorts++;
			ch++;
			continue;
		}
		if (b == ESC_NOP) {
			shorts++;
			continue;
		}
		if (i >= SALSA64_PIF_RAM_SIZE)
			break;
		uint8_t r = ram[i++];
		i += (b & LEN_MASK) + (r & LEN_MASK);
		if (ch < SALSA64_PIF_PORTS)
			units += bus->pif.pads[ch].connected ? 22000 : 18000;
		else
			units += 20000;
		ch++;
	}
	units += 1420 * shorts;
	return units * 3 / 2;
}

void salsa64_pif_written(struct salsa64_bus *bus)
{
	uint8_t *ram = bus->pif_ram;
	if ((ram[CMD_BYTE] & CMD_CHALLENGE) && bus->pif.challenge6105)
		cic6105_challenge(ram);
	if (!(ram[CMD_BYTE] & CMD_JOYBUS))
		return;
	parse(&bus->pif, ram);
	ram[CMD_BYTE] &= (uint8_t)~CMD_JOYBUS;
}

/* Cuenta un comando del accesorio: contestado (`done`), a un control sin pak
 * o con la dirección mal verificada. `mp` puede ser NULL (sin Controller Pak
 * en el bus). */
static void pak_count(struct salsa64_mempak *mp, bool pak, bool ok, uint16_t raw, bool is_write)
{
	if (!mp)
		return;
	if (ok) {
		if (is_write)
			mp->cmd_writes++;
		else
			mp->cmd_reads++;
	} else if (!pak) {
		mp->cmd_absent++;
	} else {
		mp->cmd_bad_addr++;
		mp->last_bad_addr = raw;
	}
}

/* Respuesta del control estándar en `reply`; devuelve su longitud, o -1 si
 * el control no contesta (comando desconocido o incompleto). */
static int pad_command(struct salsa64_bus *bus, unsigned port, const uint8_t *tx, unsigned txlen,
		       uint8_t *reply)
{
	struct salsa64_pif *pif = &bus->pif;
	struct salsa64_mempak *mp = bus->mempak;
	bool rumble_pak = pif->accessory[port] == SALSA64_PIF_ACCESSORY_RUMBLE_PAK;
	bool transfer_pak = pif->accessory[port] == SALSA64_PIF_ACCESSORY_TRANSFER_PAK;
	struct salsa64_transferpak_cart *tp = pif->transferpak[port];
	bool controller_pak = pif->accessory[port] == SALSA64_PIF_ACCESSORY_CONTROLLER_PAK &&
			      mp && mp->present[port];
	bool pak = rumble_pak || transfer_pak || controller_pak;
	const struct salsa64_pad *pad = &pif->pads[port];
	switch (tx[0]) {
	case JB_INFO:
	case JB_RESET:
		reply[0] = PAD_ID_HI;
		reply[1] = PAD_ID_LO;
		if (mp && !rumble_pak && !transfer_pak)
			mp->cmd_status++;
		reply[2] = pak ? (uint8_t)(PAD_PAK | (pif->pak_err[port] ? PAD_PAK_CRC : 0)) : PAD_NO_PAK;
		return 3;
	case JB_STATE: {
		/* Lo pulsado ahora y lo que se soltó sin haberse entregado. */
		uint16_t b = (pad->buttons | pif->held[port]) & (uint16_t)~SALSA64_PAD_RST;
		int8_t x = pad->x, y = pad->y;
		pif->held[port] = 0;
		if ((b & PAD_RESET_COMBO) == PAD_RESET_COMBO) {
			b = (uint16_t)((b & ~SALSA64_PAD_START) | SALSA64_PAD_RST);
			x = y = 0;
		}
		pif->sent[port] = (struct salsa64_pad){ b, x, y, 1 };
		pif->reads[port]++;
		reply[0] = (uint8_t)(b >> 8);
		reply[1] = (uint8_t)b;
		reply[2] = (uint8_t)x;
		reply[3] = (uint8_t)y;
		return 4;
	}
	case JB_PAK_READ: {
		/* Sin pak, o con la dirección mal verificada: 32 ceros y el CRC
		 * complementado (0xFF). */
		if (txlen < 3)
			return -1;
		uint16_t raw = (uint16_t)(tx[1] << 8 | tx[2]);
		bool ok = false;
		if (salsa64_mempak_addr_ok(raw)) {
			uint16_t addr = raw & 0xFFE0u;
			if (rumble_pak) {
				/* El Rumble Pak se identifica devolviendo 0x80 en 0x8000–0x8FFF. */
				memset(reply, addr >= 0x8000u && addr < 0x9000u ? 0x80 : 0x00, PAK_BLOCK);
				ok = true;
			} else if (transfer_pak) {
				salsa64_transferpak_read(&pif->transferpak_state[port], tp, addr, reply);
				ok = true; /* sin cartucho GB, el Transfer Pak sigue contestando estado */
			} else if (controller_pak) {
				ok = salsa64_mempak_read(mp, port, raw, reply);
			}
		}
		if (!ok)
			memset(reply, 0, PAK_BLOCK);
		if (!rumble_pak && !transfer_pak)
			pak_count(mp, controller_pak, ok, raw, false);
		pif->pak_err[port] = pak && !ok;
		reply[PAK_BLOCK] = ok ? salsa64_joybus_crc8(reply, PAK_BLOCK) : 0xFF;
		return PAK_BLOCK + 1;
	}
	case JB_PAK_WRITE: {
		/* El CRC de los datos recibidos; complementado si no se escribió. */
		if (txlen < 3 + PAK_BLOCK)
			return -1;
		uint16_t raw = (uint16_t)(tx[1] << 8 | tx[2]);
		bool ok = false;
		if (rumble_pak) {
			ok = salsa64_mempak_addr_ok(raw);
			if (ok && (raw & 0xFFE0u) == 0xC000u) {
				/* El latch binario está en el bit 0 del primer byte del bloque. */
				pif->rumble[port] = tx[3] & 1u;
			}
		} else if (transfer_pak) {
			ok = salsa64_mempak_addr_ok(raw);
			if (ok)
				salsa64_transferpak_write(&pif->transferpak_state[port], tp, raw & 0xFFE0u, tx + 3);
		} else if (controller_pak) {
			ok = salsa64_mempak_write(mp, port, raw, tx + 3);
		}
		if (!rumble_pak && !transfer_pak)
			pak_count(mp, controller_pak, ok, raw, true);
		pif->pak_err[port] = pak && !ok;
		uint8_t crc = salsa64_joybus_crc8(tx + 3, PAK_BLOCK);
		reply[0] = ok ? crc : (uint8_t)~crc;
		return 1;
	}
	default:
		return -1;
	}
}

void salsa64_pif_execute(struct salsa64_bus *bus)
{
	struct salsa64_pif *pif = &bus->pif;
	uint8_t *ram = bus->pif_ram;
	if (!pif->parsed)
		return;
	for (int ch = 0; ch < SALSA64_PIF_CHANNELS; ch++) {
		if (pif->handshake[ch] < 0)
			continue;
		unsigned at = (unsigned)pif->handshake[ch];
		uint8_t txb = ram[at];
		if (txb & TX_SKIP)
			continue;
		unsigned txlen = txb & LEN_MASK, rxlen = ram[at + 1] & LEN_MASK;
		uint8_t *tx = ram + at + 2, *rx = tx + txlen;
		uint8_t reply[PAK_BLOCK + 1];
		int n = -1;
		if (txlen && ch < SALSA64_PIF_PORTS && pif->pads[ch].connected)
			n = pad_command(bus, (unsigned)ch, tx, txlen, reply);
		else if (ch == SALSA64_PIF_PORTS && bus->save)  /* EEPROM del cartucho */
			n = salsa64_save_joybus(bus->save, tx, txlen, reply);
		uint8_t err = 0;
		if (n < 0) {
			err = RX_NO_DEVICE;
		} else {
			unsigned copy = (unsigned)n < rxlen ? (unsigned)n : rxlen;
			memcpy(rx, reply, copy);
			if ((unsigned)n < rxlen)
				err = RX_SHORT;
		}
		ram[at + 1] = (uint8_t)(rxlen | err);
	}
}
