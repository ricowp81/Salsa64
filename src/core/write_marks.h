/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SALSA64_CORE_WRITE_MARKS_H
#define SALSA64_CORE_WRITE_MARKS_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* One bit per 16-bit N64 pixel. Shared by emulation and presentation threads. */
static inline uint32_t salsa64_write_marks_mask(unsigned bit, unsigned count)
{
	uint32_t low;
	if (count == 32)
		low = UINT32_MAX;
	else
		low = (1u << count) - 1u;
	return low << bit;
}

static inline void salsa64_write_marks_mark_range(uint32_t *marks, uint32_t ram_size,
						   uint32_t pa, uint32_t len)
{
	if (!marks || !len || pa >= ram_size)
		return;
	uint64_t end = (uint64_t)pa + len;
	if (end > ram_size)
		end = ram_size;
	uint32_t first = pa >> 1, last = (uint32_t)((end - 1) >> 1);
	while (first <= last) {
		unsigned bit = first & 31u, n = last - first + 1u;
		if (n > 32u - bit)
			n = 32u - bit;
		__atomic_fetch_or(&marks[first >> 5], salsa64_write_marks_mask(bit, n), __ATOMIC_RELEASE);
		first += n;
	}
}

/* mask follows the big-endian byte-lane convention of vr4300_write*_masked. */
static inline void salsa64_write_marks_mark_mask(uint32_t *marks, uint32_t ram_size,
						  uint32_t pa, uint64_t mask, unsigned bytes)
{
	for (unsigned i = 0; i < bytes; i++) {
		unsigned shift = 8u * (bytes - 1u - i);
		if ((mask >> shift) & 0xFFu)
			salsa64_write_marks_mark_range(marks, ram_size, pa + i, 1);
	}
}

static inline bool salsa64_write_marks_any_range(const uint32_t *marks, uint32_t ram_size,
						  uint32_t pa, uint32_t len)
{
	if (!marks || !len || pa >= ram_size)
		return false;
	uint64_t end = (uint64_t)pa + len;
	if (end > ram_size)
		end = ram_size;
	uint32_t first = pa >> 1, last = (uint32_t)((end - 1) >> 1);
	while (first <= last) {
		unsigned bit = first & 31u, n = last - first + 1u;
		if (n > 32u - bit)
			n = 32u - bit;
		if (__atomic_load_n(&marks[first >> 5], __ATOMIC_ACQUIRE) &
		    salsa64_write_marks_mask(bit, n))
			return true;
		first += n;
	}
	return false;
}

/* Take [pa, pa+len): out has one byte per intersecting pixel, starting at
 * floor(pa/2). A concurrent write after fetch_and stays pending for next VI. */
static inline uint32_t salsa64_write_marks_take_range(uint32_t *marks, uint32_t ram_size,
						       uint32_t pa, uint32_t len,
						       uint8_t *out, uint32_t out_pixels)
{
	if (!marks || !out || !len || pa >= ram_size)
		return 0;
	uint64_t end = (uint64_t)pa + len;
	if (end > ram_size)
		end = ram_size;
	uint32_t first = pa >> 1, last = (uint32_t)((end - 1) >> 1);
	uint32_t pixels = last - first + 1u;
	if (out_pixels < pixels)
		return 0;
	memset(out, 0, pixels);
	uint32_t count = 0;
	while (first <= last) {
		unsigned bit = first & 31u, n = last - first + 1u;
		if (n > 32u - bit)
			n = 32u - bit;
		uint32_t mask = salsa64_write_marks_mask(bit, n);
		uint32_t old = __atomic_fetch_and(&marks[first >> 5], ~mask, __ATOMIC_ACQ_REL) & mask;
		for (unsigned j = 0; j < n; j++) {
			if (old & (1u << (bit + j))) {
				out[(first - (pa >> 1)) + j] = 1;
				count++;
			}
		}
		first += n;
	}
	return count;
}
#endif
