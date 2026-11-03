/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — caché de código del recompilador en un host Linux (ppc64 con
 * qemu; tools/test-rec.sh). Compartido por salsa64-run y salsa64-unit.
 *
 * Como en la PS3, la caché va pegada al programa para que el código generado
 * llame a C con `bl` directo: es un arreglo en .bss al que se le agrega
 * permiso de ejecución. Con SALSA64_REC_FAR=1 se usa un mmap lejano, que
 * obliga a llamar por mtctr/bctrl (el otro camino del emisor).
 * SALSA64_REC_PIN=1, SALSA64_REC_EAGER=1 y SALSA64_REC_RARE_EXEC=1 activan
 * las opciones SALSA64_REC_OPT_PIN, SALSA64_REC_OPT_EAGER,
 * SALSA64_REC_OPT_RARE_EXEC y SALSA64_REC_OPT_LEGACY.
 */
#ifndef SALSA64_HOST_REC_HOST_H
#define SALSA64_HOST_REC_HOST_H

#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "cpu/rec.h"

#define SALSA64_HOST_REC_BYTES (24u << 20)

static uint8_t salsa64_host_rec_cache[SALSA64_HOST_REC_BYTES] __attribute__((aligned(65536)));

/* Variable de entorno puesta y distinta de "" y de "0". */
static inline int salsa64_host_env_on(const char *name)
{
	const char *v = getenv(name);
	return v && *v && *v != '0';
}

/* Tamaño de la caché: SALSA64_REC_CACHE_MB (hasta 24) la achica, para
 * forzar desalojos por tramos y vaciados en las pruebas. */
static inline size_t salsa64_host_rec_bytes(void)
{
	const char *v = getenv("SALSA64_REC_CACHE_MB");
	unsigned long mb = v ? strtoul(v, NULL, 0) : 0;
	if (mb >= 1 && mb <= 24)
		return (size_t)mb << 20;
	return SALSA64_HOST_REC_BYTES;
}

/* Zona caliente (rec.h, hot_bytes): SALSA64_REC_HOT_KB; por omisión 1 MB. */
static inline uint32_t salsa64_host_rec_hot_bytes(void)
{
	const char *v = getenv("SALSA64_REC_HOT_KB");
	return v ? (uint32_t)(strtoul(v, NULL, 0) << 10) : 1u << 20;
}

static inline void *salsa64_host_rec_buffer(void)
{
	if (!salsa64_host_env_on("SALSA64_REC_FAR") &&
	    mprotect(salsa64_host_rec_cache, sizeof salsa64_host_rec_cache,
		     PROT_READ | PROT_WRITE | PROT_EXEC) == 0)
		return salsa64_host_rec_cache;
	void *p = mmap(NULL, SALSA64_HOST_REC_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	return p == MAP_FAILED ? NULL : p;
}

/* fastmem (docs/fastmem.md): SALSA64_REC_FASTMEM=1. La ventana de 512 MB
 * sin permisos con la RDRAM al principio; el resto falla con SIGSEGV y el
 * manejador de quien llama (run.c) parchea el acceso (salsa64_rec_fastmem_fault). */
#define SALSA64_HOST_FM_WINDOW (512u << 20)

static inline void *salsa64_host_fastmem_window(size_t rdram_bytes)
{
	void *w = mmap(NULL, SALSA64_HOST_FM_WINDOW, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (w == MAP_FAILED)
		return NULL;
	if (mmap(w, rdram_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != w) {
		munmap(w, SALSA64_HOST_FM_WINDOW);
		return NULL;
	}
	return w;
}

static inline uint32_t salsa64_host_rec_opts(void)
{
	return (salsa64_host_env_on("SALSA64_REC_IDLE_RECOMPILE") ? SALSA64_REC_OPT_IDLE_RECOMPILE : 0) |
	       (salsa64_host_env_on("SALSA64_REC_FASTMEM") ? SALSA64_REC_OPT_FASTMEM : 0) |
	       (salsa64_host_env_on("SALSA64_REC_NO_GROUP") ? SALSA64_REC_OPT_NO_GROUP : 0) |
	       (salsa64_host_env_on("SALSA64_REC_FPU_INLINE") ? SALSA64_REC_OPT_FPU_INLINE : 0) |
	       (salsa64_host_env_on("SALSA64_REC_NO_JTC") ? 0 : SALSA64_REC_OPT_JTC) |
	       (salsa64_host_env_on("SALSA64_REC_JTC_STATS") ? SALSA64_REC_OPT_JTC | SALSA64_REC_OPT_JTC_STATS : 0) |
	       (salsa64_host_env_on("SALSA64_REC_PIN") ? SALSA64_REC_OPT_PIN : 0) |
	       (salsa64_host_env_on("SALSA64_REC_EAGER") ? SALSA64_REC_OPT_EAGER : 0) |
	       (salsa64_host_env_on("SALSA64_REC_RARE_EXEC") ? SALSA64_REC_OPT_RARE_EXEC : 0) |
	       (salsa64_host_env_on("SALSA64_REC_LEGACY") ? SALSA64_REC_OPT_LEGACY : 0) |
	       (salsa64_host_env_on("SALSA64_REC_FPU_RES") ? SALSA64_REC_OPT_FPU_RES : 0) |
	       (salsa64_host_env_on("SALSA64_REC_COP0_EXEC") ? SALSA64_REC_OPT_COP0_EXEC : 0) |
	       (salsa64_host_env_on("SALSA64_REC_NO_EVICT") ? SALSA64_REC_OPT_NO_EVICT : 0) |
	       (salsa64_host_env_on("SALSA64_REC_WIDE") ? SALSA64_REC_OPT_WIDE : 0) |
	       (salsa64_host_env_on("SALSA64_REC_OLD_COPY") ? SALSA64_REC_DBG_NO_COPY_FREE : 0) |
	       (getenv("SALSA64_REC_OPTS") ? (uint32_t)strtoul(getenv("SALSA64_REC_OPTS"), NULL, 0) : 0);
}

#endif
