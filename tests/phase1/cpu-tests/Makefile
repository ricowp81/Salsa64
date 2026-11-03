PROGRAM := salsa64-cpu-tests
TITLE   := Salsa64 pruebas del CPU
APPID   := SLSA00005
SRCS    := main.c rombench.c

# El núcleo completo y las pruebas compartidas con el host (sin su main).
R       := ../../..
RC_TOP  := $(R)
include $(R)/mk/rcheevos.mk
COMMON  := src/platform/ps3/log.c src/platform/ps3/rec_cache.c \
           $(patsubst $(R)/%,%,$(wildcard $(R)/src/core/*.c $(R)/src/cpu/*.c $(R)/src/rsp/gfx/*.c $(R)/src/rsp/audio/*.c $(R)/src/rsp/jpeg/*.c \
                                          $(R)/src/rsp/lle/*.c)) src/frontend/padmap.c src/frontend/pacer.c src/frontend/audio_sync.c src/rdp/targets.c \
           src/rdp/vmatch.c src/rdp/gfxdump.c src/rdp/blackbox.c \
           src/host/raster.c \
           src/frontend/romlist.c src/frontend/ziprom.c src/frontend/menu.c src/frontend/osd_fps.c src/frontend/pausectl.c src/frontend/cheats.c src/frontend/cover.c src/frontend/namematch.c src/frontend/dlutil.c src/frontend/i18n.c lib/rsxkit/src/text.c lib/rsxkit/src/ui_geom.c \
           src/frontend/filters/crt_params.c src/frontend/version.c src/frontend/mempakfile.c src/frontend/transferpakfile.c src/frontend/perfilado.c src/frontend/cuelgue.c src/frontend/scale_auto.c \
           $(filter-out tests/unit/main_host.c,$(patsubst $(R)/%,%,$(wildcard $(R)/tests/unit/*.c))) \
           lib/rsxkit/src/fpgen.c lib/rsxkit/src/sifm.c src/frontend/ra_core.c $(RC_FILES)
# lib/rsxkit/src/fpgen.c (codificador de fragment programs, sin PSL1GHT) se
# prueba aquí con el intérprete de tests/unit/test_fpgen.c, big-endian.
ZLIB_PREFIX ?= $(PS3DEV)/portlibs/ppu
CFLAGS  := -I$(ZLIB_PREFIX)/include -ffp-contract=off -fno-math-errno -I$(CURDIR)/$(R)/tests/unit -I$(CURDIR)/$(R)/lib/rsxkit/include $(RC_DEFS) $(RC_INC)
LIBS    := -lnet -lsysutil -lsysmodule -lrt -llv2 -lm -lz
# nemu64-test embebido (homebrew MIT, tests/roms/homebrew/) para rombench.c.
DATA    := nemu64_rom
nemu64_rom_FILE := tests/roms/homebrew/nemu64-test.z64
# Caché del recompilador al final del segmento de texto (docs/rec-ppe.md).
LDFLAGS := -L$(ZLIB_PREFIX)/lib -T $(abspath $(R))/mk/rec-cache.ld -T lv2.ld

include $(R)/mk/ps3.mk
