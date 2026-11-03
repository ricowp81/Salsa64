/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — PIF: comandos joybus en la PIF RAM (controles).
 *
 * Comportamiento (n64brew.dev: PIF-NUS y Joybus Protocol):
 *   - Cuando el CPU escribe la PIF RAM con el bit 0x01 del byte de comandos
 *     (PIF RAM[63]) encendido, el PIF recorre la RAM, guarda dónde empieza
 *     el intercambio de cada canal y apaga ese bit.
 *   - En cada lectura de la PIF RAM por DMA del SI, el PIF ejecuta esos
 *     intercambios, deja las respuestas en el espacio reservado de cada uno
 *     y después hace la copia a la RDRAM.
 *   - Formato: bytes de escape 0x00 (saltar canal), 0xFD (reset de la
 *     línea), 0xFE (fin), 0xFF (nada); si no, un intercambio
 *     `TX RX [tx bytes] [rx bytes]` con las longitudes en los 6 bits bajos.
 *     TX bit 7 = saltar este intercambio; RX bit 7 = no hay dispositivo,
 *     bit 6 = faltaron bytes en la respuesta (los pone el PIF).
 *   - Canales 0–3: puertos de control; 4: cartucho (EEPROM, save.c).
 *
 * Control estándar: identificación (0x00 y 0xFF: 05 00 + estado), estado
 * (0x01: botones, X, Y) y lectura y escritura del accesorio (0x02 y 0x03).
 * Con un Controller Pak enchufado (bus->mempak, mempak.h) el estado dice
 * «pak presente» y esos comandos leen y escriben sus 32 KB; sin él, y
 * siempre con un Rumble Pak (no se emula), contestan como un control sin
 * pak. L + R +
 * Start reporta RST, Start apagado y el stick en (0, 0).
 *
 * Pulsaciones retenidas: un botón que el frontend da por recién pulsado
 * sale pulsado en al menos una respuesta de estado aunque se suelte antes
 * de que el juego lea el control (una pulsación de un campo en un juego
 * que lee cada dos). Solo el flanco: un botón mantenido y soltado entre dos
 * lecturas sale suelto, y las soltadas no se retienen.
 *
 * Sin temporización: el DMA del SI sigue siendo instantáneo.
 */
#ifndef SALSA64_CORE_PIF_H
#define SALSA64_CORE_PIF_H

#include "core/types.h"
#include "core/transferpak.h"

#define SALSA64_PIF_PORTS    4
#define SALSA64_PIF_CHANNELS 5  /* 4 puertos + cartucho */

enum salsa64_pif_accessory {
	SALSA64_PIF_ACCESSORY_CONTROLLER_PAK,
	SALSA64_PIF_ACCESSORY_NONE,
	SALSA64_PIF_ACCESSORY_RUMBLE_PAK,
	SALSA64_PIF_ACCESSORY_TRANSFER_PAK,
};

/* Botones en la respuesta del comando 0x01, el bit más alto primero. */
enum salsa64_pad_button {
	SALSA64_PAD_A       = 0x8000,
	SALSA64_PAD_B       = 0x4000,
	SALSA64_PAD_Z       = 0x2000,
	SALSA64_PAD_START   = 0x1000,
	SALSA64_PAD_D_UP    = 0x0800,
	SALSA64_PAD_D_DOWN  = 0x0400,
	SALSA64_PAD_D_LEFT  = 0x0200,
	SALSA64_PAD_D_RIGHT = 0x0100,
	SALSA64_PAD_RST     = 0x0080,  /* lo pone el control con L + R + Start */
	SALSA64_PAD_L       = 0x0020,
	SALSA64_PAD_R       = 0x0010,
	SALSA64_PAD_C_UP    = 0x0008,
	SALSA64_PAD_C_DOWN  = 0x0004,
	SALSA64_PAD_C_LEFT  = 0x0002,
	SALSA64_PAD_C_RIGHT = 0x0001,
};

/* Estado de un control tal como lo da el frontend. Stick: + es derecha y
 * arriba; un control original llega a unos ±80. */
struct salsa64_pad {
	uint16_t buttons;
	int8_t x, y;
	uint8_t connected;
};

struct salsa64_pif {
	struct salsa64_pad pads[SALSA64_PIF_PORTS];
	uint8_t accessory[SALSA64_PIF_PORTS]; /* enum salsa64_pif_accessory */
	uint8_t rumble[SALSA64_PIF_PORTS];    /* latch del motor, 0 apagado, 1 encendido */
	struct salsa64_transferpak_state transferpak_state[SALSA64_PIF_PORTS];
	struct salsa64_transferpak_cart *transferpak[SALSA64_PIF_PORTS]; /* datos externos; punteros no se guardan */
	int8_t handshake[SALSA64_PIF_CHANNELS];  /* posición del byte TX de cada canal, -1 si no hay */
	uint8_t parsed;                          /* hay intercambios listos para ejecutar */
	bool challenge6105;                      /* el CIC del cartucho es el 6105: la PIF contesta su desafío */
	uint16_t held[SALSA64_PIF_PORTS];        /* recién pulsados sin entregar todavía */
	/* Lo entregado al juego (para el log del frontend): la última
	 * respuesta de estado de cada puerto y cuántas van. */
	struct salsa64_pad sent[SALSA64_PIF_PORTS];
	uint32_t reads[SALSA64_PIF_PORTS];
	uint8_t pak_err[SALSA64_PIF_PORTS];      /* el último comando del accesorio traía mal la dirección */
};

struct salsa64_bus;

/* Al reset: olvida los intercambios; los controles siguen conectados. */
void salsa64_pif_reset(struct salsa64_bus *bus);
/* Lo llama el frontend en cada cuadro (o cuando cambia). Los botones
 * pulsados quedan retenidos hasta la siguiente respuesta de estado. */
void salsa64_pif_set_pad(struct salsa64_bus *bus, unsigned port, const struct salsa64_pad *pad);
/* Después de cada escritura del CPU en la PIF RAM (DMA o directa). */
void salsa64_pif_written(struct salsa64_bus *bus);
/* Antes de copiar la PIF RAM a la RDRAM por DMA del SI. */
void salsa64_pif_execute(struct salsa64_bus *bus);
/* Ciclos del VR4300 que tarda la lectura PIF RAM → RDRAM con lo que hay en la
 * PIF RAM (la PIF ejecuta los intercambios joybus antes de contestar). */
uint32_t salsa64_pif_read_cycles(const struct salsa64_bus *bus);

/* CRC-8 de los datos del accesorio: polinomio 0x85, semilla 0. */
uint8_t salsa64_joybus_crc8(const uint8_t *data, unsigned len);

#endif
