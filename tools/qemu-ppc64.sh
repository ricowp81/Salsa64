#!/bin/sh
# Corre un programa de build/ppc64 con qemu-ppc64. El directorio de trabajo
# es la raíz del repo; QEMU_TIMEOUT (segundos, 600 por omisión) corta las
# corridas colgadas. stderr se junta con stdout dentro del contenedor (Docker
# no conserva el orden entre los dos). Pasa al programa las variables
# SALSA64_TEST, SALSA64_REC_DUMP, SALSA64_REC_FAR, SALSA64_REC_PIN,
# SALSA64_REC_EAGER, SALSA64_REC_RARE_EXEC, SALSA64_REC_PROF, SALSA64_REC_LEGACY,
# SALSA64_REC_CACHE_MB, SALSA64_REC_HOT_KB, SALSA64_REC_SITES y SALSA64_REC_FASTMEM.
#
# CPU power9 (SALSA64_QEMU_CPU cambia el modelo): con -cpu 970 (el más
# parecido al PPE, Power ISA 2.01) qemu 7.2 da resultados erróneos (el
# intérprete falla 15 pruebas que pasan en la PS3 y con power7/power8), y
# power5+ se cuelga. Con power7 o power8, sin, cos y floor de la glibc
# estática mueren con SIGILL: usan mffscrn/mffscrni (Power ISA 3.0), que
# qemu solo acepta desde power9 (se caía la suite sync, por audio_sync.c;
# en la PS3 no hay glibc). power9 acepta instrucciones que el PPE no tiene:
# el emisor (src/cpu/ppc.h) se limita a mano a Power ISA 2.02 y el C se
# compila con -mcpu=cell.
#
# Respaldos de juegos (fuera del repo, nunca en él): SALSA64_ROMS con una
# carpeta que colima comparta (no todas lo están) se
# monta de solo lectura en /roms.
#   tools/qemu-ppc64.sh build/ppc64/salsa64-unit rec
#   SALSA64_ROMS=/ruta/a/respaldos \
#     tools/qemu-ppc64.sh build/ppc64/salsa64-run --lockstep /roms/goldeneye.z64
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${SALSA64_QEMU_IMAGE:-salsa64-qemu:dev}"
exec docker run --rm -i --user "$(id -u):$(id -g)" -v "$ROOT:/src" -w /src \
	${SALSA64_ROMS:+-v} ${SALSA64_ROMS:+"$SALSA64_ROMS:/roms:ro"} \
	-e SALSA64_TEST -e SALSA64_REC_DUMP -e SALSA64_REC_FAR -e SALSA64_REC_PIN -e SALSA64_REC_EAGER \
	-e SALSA64_REC_IDLE_RECOMPILE -e SALSA64_REC_RARE_EXEC -e SALSA64_REC_PROF -e SALSA64_REC_LEGACY -e SALSA64_REC_OPTS \
	-e SALSA64_REC_FPU_RES -e SALSA64_REC_COP0_EXEC -e SALSA64_REC_NO_EVICT -e SALSA64_REC_CACHE_MB \
	-e SALSA64_REC_HOT_KB -e SALSA64_REC_SITES -e SALSA64_REC_PROF_TOP -e SALSA64_REC_WIDE -e SALSA64_REC_SAMPLE -e SALSA64_REC_FASTMEM -e SALSA64_REC_NO_GROUP -e SALSA64_REC_FPU_INLINE -e SALSA64_REC_NO_JTC -e SALSA64_REC_JTC_STATS -e SALSA64_REC_OLD_COPY -e SALSA64_AUDIO_RANGES_OLD -e SALSA64_PERFILADO_CFG "$IMAGE" \
	sh -c 'cpu=$1; shift; exec timeout "$0" qemu-ppc64-static -cpu "$cpu" "$@" 2>&1' \
	"${QEMU_TIMEOUT:-600}" "${SALSA64_QEMU_CPU:-power9}" "$@"
