# Salsa64 — pruebas unitarias del núcleo en el host, con el gcc nativo del
# contenedor (arm64 little-endian). Desde la raíz: tools/build.sh test-host
#
# Por omisión compila con AddressSanitizer y UBSan; SAN= los quita.

ROOT := $(abspath ../..)
OUT  ?= $(ROOT)/build/host

CC      ?= gcc
SAN     ?= -fsanitize=address,undefined -fno-sanitize-recover=undefined
RC_TOP  := $(ROOT)
include $(ROOT)/mk/rcheevos.mk
CFLAGS  := -std=gnu11 -O2 -g -Wall -Wextra -Werror -ffp-contract=off -fno-math-errno -DAUD_DEBUG -DGFX_CENSUS \
           -I$(ROOT)/src -I$(ROOT)/lib/spukit/include -I$(CURDIR) -I$(ROOT)/lib/rsxkit/include $(RC_DEFS) $(RC_INC) $(SAN) $(EXTRA_CFLAGS)
LDFLAGS := $(SAN) $(EXTRA_LDFLAGS)
LDLIBS  := -lm -lz -pthread

# lib/rsxkit/src/fpgen.c es el codificador de fragment programs del RSX:
# C puro, sin PSL1GHT, probado aquí con un intérprete del microcódigo.
CORE_SRCS := $(wildcard $(ROOT)/src/core/*.c $(ROOT)/src/cpu/*.c $(ROOT)/src/rsp/gfx/*.c $(ROOT)/src/rsp/audio/*.c $(ROOT)/src/rsp/jpeg/*.c $(ROOT)/src/rsp/lle/*.c) $(ROOT)/src/frontend/padmap.c $(ROOT)/src/frontend/pacer.c \
             $(ROOT)/src/frontend/audio_sync.c $(ROOT)/src/frontend/romlist.c $(ROOT)/src/frontend/ziprom.c $(ROOT)/tests/unit/stubs/archive_rom_stubs.c $(ROOT)/src/frontend/menu.c \
             $(ROOT)/src/frontend/osd_fps.c $(ROOT)/src/frontend/pausectl.c $(ROOT)/src/frontend/cheats.c $(ROOT)/src/frontend/cover.c $(ROOT)/src/frontend/namematch.c $(ROOT)/src/frontend/dlutil.c $(ROOT)/src/frontend/i18n.c \
             $(ROOT)/src/frontend/version.c $(ROOT)/src/frontend/perfilado.c $(ROOT)/src/frontend/savefile.c $(ROOT)/src/frontend/mempakfile.c $(ROOT)/src/frontend/transferpakfile.c \
             $(ROOT)/src/frontend/filters/crt_params.c $(ROOT)/src/frontend/cuelgue.c $(ROOT)/src/frontend/seda_calc.c $(ROOT)/src/frontend/cartucho_mesh.c $(ROOT)/src/frontend/thumbs_calc.c \
	     $(ROOT)/src/frontend/scale_auto.c $(ROOT)/src/frontend/prompts.c $(ROOT)/src/frontend/intro_logic.c \
             $(ROOT)/src/rdp/targets.c $(ROOT)/src/rdp/layer8.c $(ROOT)/src/rdp/mask32.c $(ROOT)/src/rdp/vmatch.c $(ROOT)/src/rdp/gfxdump.c $(ROOT)/src/rdp/blackbox.c $(ROOT)/src/host/raster.c $(ROOT)/src/host/gfxcheck.c $(ROOT)/lib/rsxkit/src/fpgen.c $(ROOT)/lib/rsxkit/src/sifm.c $(ROOT)/lib/rsxkit/src/text.c \
             $(ROOT)/lib/rsxkit/src/ui_geom.c $(ROOT)/lib/rsxkit/src/dsta.c $(ROOT)/src/frontend/ra_core.c $(ROOT)/src/frontend/ra_rules.c $(addprefix $(ROOT)/,$(RC_FILES))
TEST_SRCS := $(wildcard $(CURDIR)/*.c)
OBJS      := $(patsubst $(ROOT)/%.c,$(OUT)/obj/%.o,$(CORE_SRCS) $(TEST_SRCS))

.PHONY: all run clean

all: $(OUT)/salsa64-unit

run: $(OUT)/salsa64-unit
	ASAN_OPTIONS=detect_leaks=0 $<

$(OUT)/salsa64-unit: $(OBJS)
	@echo "  LD      $(notdir $@)"
	@$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

# rcheevos es código de terceros: se compila sin -Werror ni la comprobación de
# punteros nulos de UBSan (memcpy(dst, NULL, 0) con una cadena vacía, inocuo).
$(addprefix $(OUT)/obj/,$(patsubst %.c,%.o,$(RC_FILES))): CFLAGS := $(filter-out -Werror,$(CFLAGS)) -Wno-unused-parameter -fno-sanitize=nonnull-attribute

$(OUT)/obj/%.o: $(ROOT)/%.c
	@echo "  CC      $*.c"
	@mkdir -p $(@D)
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf $(OUT)

-include $(OBJS:.o=.d)

# Sustitutos PS3 solo para el ciclo de vida de la foto de estados.
$(OUT)/obj/tests/unit/test_states_frontend.o: CFLAGS += -DSALSA64_STATES_HOST_TEST -I$(CURDIR)/stubs
