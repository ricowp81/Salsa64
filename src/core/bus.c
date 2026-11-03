/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — bus físico de la N64. Ver bus.h.
 */
#include "core/bus.h"

#include <string.h>

#include "core/endian.h"
#include "core/gamefix.h"
#include "core/rompg.h"
#include "core/save.h"
#include "cpu/rec.h"
#include "cpu/vr4300.h"

void salsa64_bus_rdram_patched(struct salsa64_bus *bus, uint32_t pa, uint32_t len)
{
	if (bus->cpu && bus->cpu->rec)
		salsa64_rec_invalidate(bus->cpu->rec, pa, len);
}

void salsa64_bus_rdram_written(struct salsa64_bus *bus, uint32_t pa, uint32_t len)
{
	salsa64_bus_rdram_patched(bus, pa, len);
	if (bus->rdram_written)
		bus->rdram_written(bus->rdram_written_ctx, pa, len);
}

void salsa64_bus_init(struct salsa64_bus *bus, uint8_t *rdram, uint32_t rdram_size)
{
	memset(bus, 0, sizeof *bus);
	bus->rdram = rdram;
	bus->rdram_size = rdram_size > SALSA64_RDRAM_MAX ? SALSA64_RDRAM_MAX : rdram_size;
	/* Como una consola recién encendida: un control en el puerto 1. El
	 * frontend lo cambia según los mandos que haya. */
	bus->pif.pads[0].connected = 1;
	salsa64_bus_reset(bus);
}

void salsa64_bus_reset(struct salsa64_bus *bus)
{
	memset(bus->rdram, 0, bus->rdram_size);
	salsa64_bus_rdram_written(bus, 0, bus->rdram_size);
	salsa64_bus_reset_warm(bus);
}

/* Lo del reset sin tocar la RDRAM: la que dejan el botón Reset y el NMI. */
void salsa64_bus_reset_warm(struct salsa64_bus *bus)
{
	memset(bus->sp_mem, 0, sizeof bus->sp_mem);
	memset(bus->pif_ram, 0, sizeof bus->pif_ram);
	salsa64_pif_reset(bus);
	memset(bus->rdram_regs, 0, sizeof bus->rdram_regs);
	memset(bus->ri, 0, sizeof bus->ri);
	if (bus->isv)
		memset(bus->isv, 0, SALSA64_ISV_SIZE);
	salsa64_rcp_reset(bus);
}

void salsa64_bus_set_rom(struct salsa64_bus *bus, const uint8_t *rom, uint32_t size)
{
	uint32_t max = SALSA64_CART_ROM_END - SALSA64_CART_ROM_ADDR;
	bus->rom = rom;
	bus->rom_size = size > max ? max : size;
	bus->rom_resident = bus->rom_size;
	bus->rompg = NULL;
	bus->gamefix_applied = 0;
	salsa64_gamefix_select(bus);
	if (bus->cpu && bus->cpu->rec)  /* puede haber código compilado desde la ROM anterior */
		salsa64_rec_flush_why(bus->cpu->rec, SALSA64_REC_FLUSH_ROM);
}

void salsa64_bus_set_rom_paged(struct salsa64_bus *bus, const uint8_t *head, uint32_t head_size, uint32_t size,
			       struct salsa64_rompg *pg)
{
	salsa64_bus_set_rom(bus, head, size);
	bus->rom_resident = head_size < bus->rom_size ? head_size : bus->rom_size;
	bus->rompg = pg;
}

void salsa64_bus_rom_read(struct salsa64_bus *bus, uint32_t off, uint8_t *dst, uint32_t len)
{
	uint32_t n = 0;

	if (off < bus->rom_size) {
		n = bus->rom_size - off < len ? bus->rom_size - off : len;
		if (!bus->rompg || (uint64_t)off + n <= bus->rom_resident)
			memcpy(dst, bus->rom + off, n);
		else
			salsa64_rompg_read(bus->rompg, off, dst, n);
	}
	if (n < len)
		memset(dst + n, 0, len - n);
}

void salsa64_bus_set_isviewer(struct salsa64_bus *bus, uint8_t *buf, salsa64_isv_fn out, void *ctx)
{
	bus->isv = buf;
	bus->isv_out = out;
	bus->isv_ctx = ctx;
	if (buf)
		memset(buf, 0, SALSA64_ISV_SIZE);
	if (bus->cpu && bus->cpu->rec)  /* el ISViewer tapa la ROM: no se compila ahí */
		salsa64_rec_flush_why(bus->cpu->rec, SALSA64_REC_FLUSH_ROM);
}

static inline bool isv_hit(const struct salsa64_bus *bus, uint32_t paddr)
{
	return bus->isv && paddr - SALSA64_ISV_ADDR < SALSA64_ISV_SIZE;
}

static void isv_write(struct salsa64_bus *bus, uint32_t off, uint32_t value, uint32_t mask)
{
	uint8_t *p = bus->isv + off;
	salsa64_wr32(p, (salsa64_rd32(p) & ~mask) | (value & mask));
	if (off == SALSA64_ISV_WRITE_LEN && bus->isv_out) {
		uint32_t len = salsa64_rd32(p);
		if (len > SALSA64_ISV_SIZE - SALSA64_ISV_BUFFER)
			len = SALSA64_ISV_SIZE - SALSA64_ISV_BUFFER;
		bus->isv_out(bus->isv_ctx, bus->isv + SALSA64_ISV_BUFFER, len);
	}
}

/* Registros del RCP: página de 1 MB dentro de 0x04000000–0x048FFFFF. */
static uint32_t rcp_read(struct salsa64_bus *bus, uint32_t paddr)
{
	uint32_t reg = paddr & 0xFFFFF;
	switch ((paddr >> 20) & 0xF) {
	case 0x0:
		if (reg < 0x40000)
			return salsa64_rd32(bus->sp_mem + (reg & (SALSA64_SP_MEM_SIZE - 1)));
		return salsa64_sp_read(bus, reg);
	case 0x1: return salsa64_dp_read(bus, reg);
	case 0x3: return salsa64_mi_read(bus, reg);
	case 0x4: return salsa64_vi_read(bus, reg);
	case 0x5: return salsa64_ai_read(bus, reg);
	case 0x6: return salsa64_pi_read(bus, reg);
	case 0x7: return reg < SALSA64_RI_NREGS * 4 ? bus->ri[reg >> 2] : 0;
	case 0x8: return salsa64_si_read(bus, reg);
	default:  return 0;  /* DP span (0x042) y huecos */
	}
}

static void rcp_write(struct salsa64_bus *bus, uint32_t paddr, uint32_t value)
{
	uint32_t reg = paddr & 0xFFFFF;
	switch ((paddr >> 20) & 0xF) {
	case 0x0:
		if (reg < 0x40000) {
			salsa64_wr32(bus->sp_mem + (reg & (SALSA64_SP_MEM_SIZE - 1)), value);
			return;
		}
		salsa64_sp_write(bus, reg, value);
		return;
	case 0x1: salsa64_dp_write(bus, reg, value); return;
	case 0x3: salsa64_mi_write(bus, reg, value); return;
	case 0x4: salsa64_vi_write(bus, reg, value); return;
	case 0x5: salsa64_ai_write(bus, reg, value); return;
	case 0x6: salsa64_pi_write(bus, reg, value); return;
	case 0x7:
		if (reg < SALSA64_RI_NREGS * 4)
			bus->ri[reg >> 2] = value;
		return;
	case 0x8: salsa64_si_write(bus, reg, value); return;
	default:  return;
	}
}

/* Bus PI de 16 bits: el RCP lee dos medias palabras desde la dirección par,
 * así que una lectura de 8/16 bits en paddr & 2 trae la palabra que empieza
 * ahí (n64brew.dev: Memory map, "PI external bus"). */
static uint32_t pi_bus_read(struct salsa64_bus *bus, uint32_t paddr)
{
	paddr &= ~1u;
	if (isv_hit(bus, paddr) && paddr + 4 <= SALSA64_ISV_ADDR + SALSA64_ISV_SIZE)
		return salsa64_rd32(bus->isv + (paddr - SALSA64_ISV_ADDR));
	uint32_t off = paddr - SALSA64_CART_ROM_ADDR, v = 0;
	if (bus->rompg && (uint64_t)off + 4 > bus->rom_resident) {
		if (off >= bus->rom_resident && off + 4 <= bus->rom_size)
			return salsa64_rompg_rd32(bus->rompg, off);
		uint8_t b[4];
		salsa64_bus_rom_read(bus, off, b, 4);  /* a caballo de la cabecera o pasando el final */
		return salsa64_rd32(b);
	}
	for (unsigned k = 0; k < 4; k++)
		v = v << 8 | (off + k < bus->rom_size ? bus->rom[off + k] : 0);
	return v;
}

uint32_t salsa64_bus_read32(struct salsa64_bus *bus, uint32_t paddr)
{
	if (paddr >= SALSA64_CART_ROM_ADDR && paddr < SALSA64_CART_ROM_END)
		return pi_bus_read(bus, paddr);
	paddr &= ~3u;
	if (paddr < bus->rdram_size)
		return salsa64_rd32(bus->rdram + paddr);
	if (paddr < 0x03F00000)
		return 0;
	if (paddr < 0x04000000)
		return paddr < SALSA64_RDRAM_REGS_END ? bus->rdram_regs[(paddr >> 2) & (SALSA64_RDRAM_NREGS - 1)] : 0;
	if (paddr < 0x04900000)
		return rcp_read(bus, paddr);
	if (paddr >= SALSA64_PIF_RAM_ADDR && paddr < SALSA64_PIF_RAM_ADDR + SALSA64_PIF_RAM_SIZE)
		return salsa64_rd32(bus->pif_ram + (paddr - SALSA64_PIF_RAM_ADDR));
	if (paddr - SALSA64_SRAM_ADDR < SALSA64_SRAM_SPAN)
		return salsa64_save_dom2_read32(bus, paddr);
	/* Bus del PI sin dispositivo (el 64DD en 0x05000000–0x07FFFFFF, lo que
	 * está después de la PIF): nadie maneja el bus y la lectura trae la mitad
	 * baja de la dirección, repetida (medido en ares: 0x05000508 lee
	 * 0x05080508). F-Zero X sondea el estado del 64DD (0x05000508) y, con 0,
	 * cree que hay uno y se queda esperando al manejador del disco. */
	if (paddr - 0x05000000u < 0x03000000u || paddr >= 0x1FD00000u)
		return (paddr & 0xFFFFu) * 0x10001u;
	return 0;
}

void salsa64_bus_write32(struct salsa64_bus *bus, uint32_t paddr, uint32_t value, uint32_t mask)
{
	paddr &= ~3u;
	if (paddr < bus->rdram_size) {
		uint8_t *p = bus->rdram + paddr;
		salsa64_wr32(p, (salsa64_rd32(p) & ~mask) | (value & mask));
		return;
	}
	/* Fuera de la RDRAM el tamaño del acceso se ignora: se escribe la palabra
	 * del bus completa. El ISViewer es la excepción (dispositivo de
	 * depuración que se usa byte a byte). */
	if (paddr < 0x03F00000)
		return;
	if (paddr < 0x04000000) {
		if (paddr < SALSA64_RDRAM_REGS_END)
			bus->rdram_regs[(paddr >> 2) & (SALSA64_RDRAM_NREGS - 1)] = value;
		return;
	}
	if (paddr < 0x04900000) {
		rcp_write(bus, paddr, value);
		return;
	}
	if (isv_hit(bus, paddr)) {
		isv_write(bus, paddr - SALSA64_ISV_ADDR, value, mask);
		return;
	}
	if (paddr >= SALSA64_PIF_RAM_ADDR && paddr < SALSA64_PIF_RAM_ADDR + SALSA64_PIF_RAM_SIZE) {
		salsa64_wr32(bus->pif_ram + (paddr - SALSA64_PIF_RAM_ADDR), value);
		salsa64_pif_written(bus);
		return;
	}
	if (paddr - SALSA64_SRAM_ADDR < SALSA64_SRAM_SPAN) {
		salsa64_save_dom2_write32(bus, paddr, value);
		return;
	}
	/* ROM del cartucho y huecos: se ignora. */
}

uint64_t salsa64_bus_read64(struct salsa64_bus *bus, uint32_t paddr)
{
	paddr &= ~7u;
	if (paddr < bus->rdram_size)
		return salsa64_rd64(bus->rdram + paddr);
	return (uint64_t)salsa64_bus_read32(bus, paddr) << 32 | salsa64_bus_read32(bus, paddr + 4);
}

void salsa64_bus_write64(struct salsa64_bus *bus, uint32_t paddr, uint64_t value, uint64_t mask)
{
	paddr &= ~7u;
	if (paddr < bus->rdram_size) {
		uint8_t *p = bus->rdram + paddr;
		salsa64_wr64(p, (salsa64_rd64(p) & ~mask) | (value & mask));
		return;
	}
	/* El VR4300 pone primero la palabra alta y el RCP solo escribe esa. */
	salsa64_bus_write32(bus, paddr, (uint32_t)(value >> 32), (uint32_t)(mask >> 32));
}
