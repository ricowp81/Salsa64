/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — caché de código del recompilador en la PS3 (src/cpu/rec.h).
 *
 * 24 MB en una cola NOBITS del segmento de texto (mk/rec-cache.ld): el
 * programa que la use enlaza src/platform/ps3/rec_cache.c y agrega a su
 * Makefile `LDFLAGS := -T $(ROOT)/mk/rec-cache.ld -T lv2.ld`.
 *
 * RPCS3 no ejecuta código generado (docs/rpcs3.md): tools/rpcs3-run.py crea
 * SALSA64_NOREC_FLAG y entonces todo corre en el intérprete. El mismo
 * archivo sirve para apagar el recompilador en la PS3 real.
 */
#ifndef SALSA64_PLATFORM_PS3_REC_CACHE_H
#define SALSA64_PLATFORM_PS3_REC_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

#include "platform/ps3/paths.h"

/* 32 MB es el techo: los saltos al preámbulo (±32 MB) tienen que alcanzarlo
 * desde el final (docs/rec-ppe.md, «Caché de código»). */
#define SALSA64_REC_CACHE_BYTES (32u << 20)
#define SALSA64_NOREC_FLAG SALSA64_DATA_DIR "/sin-recompilador"

/* Prueba de la caché antes de usarla (issue #2: con CFW Evilnat 4.91 el
 * hilo se quedaba trabado en salsa64_rec_init, sin matar el proceso). Un
 * hilo aparte escribe en la caché, limpia cachés, ejecuta lo escrito, lo
 * reescribe y lo vuelve a ejecutar, al principio y al final, y toca cada
 * página; quien abre un juego espera su resultado hasta SALSA64_REC_PROBE_US
 * desde que empezó. Si no volvió o falló, intérprete. Con HEN dura menos de
 * un milisegundo y se lanza al arrancar la aplicación: al abrir un juego ya
 * terminó.
 *
 * Si el fallo mata el proceso en vez de trabar el hilo, el archivo de marca
 * queda escrito («probando <versión>»): el siguiente arranque con la misma
 * versión no repite la prueba y va al intérprete (borrar el archivo para
 * repetirla). docs/jit-cfw.md. */
#define SALSA64_REC_PROBE_FILE SALSA64_DATA_DIR "/jit-prueba.txt"
#define SALSA64_REC_PROBE_US   2000000u
#define SALSA64_REC_PROBE_PRIO 2000  /* por debajo de todos los hilos del frontend */

/* Lanza la prueba (una vez por proceso). marker: archivo de marca o NULL;
 * tag: versión que se escribe en él. */
void salsa64_ps3_rec_probe_start(const char *marker, const char *tag);
/* Resultado de la prueba (la lanza sin marca si nadie lo hizo). true si la
 * caché sirve; `why` dice qué paso no volvió o falló, o, si sirve, cuánto
 * tardó y los atributos de su página. *first: es la primera consulta. */
bool salsa64_ps3_rec_probe_wait(char *why, size_t n, bool *first);

/* sys_memory_get_page_attribute (syscall 351). Devuelve su código (0 = bien). */
struct salsa64_ps3_page_attr {
	uint64_t attribute, access_right;
	uint32_t page_size;
};
int32_t salsa64_ps3_page_attr(const void *p, struct salsa64_ps3_page_attr *out);
/* Con este archivo, el perfil del recompilador por causa (src/cpu/rec_prof.h)
 * va al log con las estadísticas. */
#define SALSA64_RECPROF_FLAG SALSA64_DATA_DIR "/perfil"
/* Con este archivo, el recompilador se comporta como antes del 2026-10-10
 * (SALSA64_REC_OPT_LEGACY, rec.h): para medir A/B con el mismo paquete. */
#define SALSA64_RECLEGACY_FLAG SALSA64_DATA_DIR "/jit-viejo"
/* Con este archivo, la FPU con registros residentes (SALSA64_REC_OPT_FPU_RES,
 * rec.h; experimental, más lenta en la PS3). */
#define SALSA64_RECFPURES_FLAG SALSA64_DATA_DIR "/jit-fpu-residente"

void *salsa64_ps3_rec_cache(void);

static inline int salsa64_ps3_rec_wanted(void)
{
	struct stat st;
	return stat(SALSA64_NOREC_FLAG, &st) != 0;
}

static inline int salsa64_ps3_rec_prof_wanted(void)
{
	struct stat st;
	return stat(SALSA64_RECPROF_FLAG, &st) == 0;
}

static inline int salsa64_ps3_rec_legacy_wanted(void)
{
	struct stat st;
	return stat(SALSA64_RECLEGACY_FLAG, &st) == 0;
}

static inline int salsa64_ps3_rec_fpu_res_wanted(void)
{
	struct stat st;
	return stat(SALSA64_RECFPURES_FLAG, &st) == 0;
}

/* Con este archivo, COP0 como hasta el 2026-10-11, todo por el intérprete
 * (SALSA64_REC_OPT_COP0_EXEC). */
#define SALSA64_RECCOP0EXEC_FLAG SALSA64_DATA_DIR "/jit-cop0-viejo"

static inline int salsa64_ps3_rec_cop0_exec_wanted(void)
{
	struct stat st;
	return stat(SALSA64_RECCOP0EXEC_FLAG, &st) == 0;
}

/* Con este archivo, la caché llena se vacía entera como hasta el 2026-10-11
 * en vez de desalojar el tramo más viejo (SALSA64_REC_OPT_NO_EVICT). */
#define SALSA64_RECNOEVICT_FLAG SALSA64_DATA_DIR "/jit-sin-tramos"

static inline int salsa64_ps3_rec_no_evict_wanted(void)
{
	struct stat st;
	return stat(SALSA64_RECNOEVICT_FLAG, &st) == 0;
}

/* Con este archivo, el muestreo por tiempo del recompilador (rec_sample.h):
 * la entrada de cada bloque deja su id y un hilo de baja prioridad muestrea
 * cada SAMPLE_US; al cerrar el juego, al log los bloques que más tiempo se
 * llevan. */
#define SALSA64_RECSAMPLE_FLAG SALSA64_DATA_DIR "/muestreo"

static inline int salsa64_ps3_rec_sample_wanted(void)
{
	struct stat st;
	return stat(SALSA64_RECSAMPLE_FLAG, &st) == 0;
}

/* Con este archivo, el código generado «ancho» de antes del 2026-10-13
 * (SALSA64_REC_OPT_WIDE): para medir el compacto contra él con el mismo
 * paquete. */
#define SALSA64_RECWIDE_FLAG SALSA64_DATA_DIR "/jit-ancho"

static inline int salsa64_ps3_rec_wide_wanted(void)
{
	struct stat st;
	return stat(SALSA64_RECWIDE_FLAG, &st) == 0;
}

/* Con este archivo, sin la base en caché (SALSA64_REC_OPT_NO_GROUP,
 * docs/fastmem.md §8): cada acceso con su prueba, para el A/B. */
#define SALSA64_RECNOGROUP_FLAG SALSA64_DATA_DIR "/jit-sin-grupos"

static inline int salsa64_ps3_rec_no_group_wanted(void)
{
	struct stat st;
	return stat(SALSA64_RECNOGROUP_FLAG, &st) == 0;
}

/* Con este archivo, la FPU en línea en cada sitio como hasta el 2026-10-13
 * (SALSA64_REC_OPT_FPU_INLINE) en vez de las rutinas compartidas del
 * preámbulo (docs/fastmem.md §9), para el A/B. */
#define SALSA64_RECFPUINLINE_FLAG SALSA64_DATA_DIR "/jit-fpu-en-linea"

static inline int salsa64_ps3_rec_fpu_inline_wanted(void)
{
	struct stat st;
	return stat(SALSA64_RECFPUINLINE_FLAG, &st) == 0;
}

/* La caché de destinos de JR/JALR en exit_jr (SALSA64_REC_OPT_JTC) va
 * encendida por omisión; con el archivo jit-sin-destinos se apaga (para el
 * A/B) y con jit-destinos-cuentas además cuenta aciertos y fallos para el log
 * (suma unas palabras en cada JR). */
#define SALSA64_RECNOJTC_FLAG SALSA64_DATA_DIR "/jit-sin-destinos"
#define SALSA64_RECJTCSTATS_FLAG SALSA64_DATA_DIR "/jit-destinos-cuentas"

static inline int salsa64_ps3_rec_jtc_wanted(void)
{
	struct stat st;
	return stat(SALSA64_RECNOJTC_FLAG, &st) != 0;
}

static inline int salsa64_ps3_rec_jtc_stats_wanted(void)
{
	struct stat st;
	return stat(SALSA64_RECJTCSTATS_FLAG, &st) == 0;
}

/* Las copias de bytes de los bloques mapeados van en ranuras que se liberan
 * al morir el bloque (SALSA64_REC_DBG_NO_COPY_FREE apagado); con el archivo
 * jit-copias-viejas vuelven al reparto lineal de 1 MB sin liberar de antes
 * del 2026-10-28 (para el A/B: Perfect Dark retraducía sus overlays). */
#define SALSA64_RECOLDCOPY_FLAG SALSA64_DATA_DIR "/jit-copias-viejas"

static inline int salsa64_ps3_rec_old_copy_wanted(void)
{
	struct stat st;
	return stat(SALSA64_RECOLDCOPY_FLAG, &st) == 0;
}

#endif
