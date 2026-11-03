# SPDX-License-Identifier: GPL-2.0-or-later
# Salsa64 — ejecutor de línea de comandos para el host (salsa64-run), con el
# gcc nativo del contenedor. Desde la raíz: tools/build.sh host
#
# Por omisión compila con AddressSanitizer y UBSan, como las pruebas
# unitarias; SAN= los quita (más rápido).

ROOT := $(abspath ../..)
OUT  ?= $(ROOT)/build/host

CC      ?= gcc
SAN     ?= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS  := -std=gnu11 -O2 -g -Wall -Wextra -Werror -ffp-contract=off -fno-math-errno -DAUD_DEBUG -DGFX_CENSUS \
           -I$(ROOT)/src -I$(ROOT)/lib/spukit/include $(SAN) $(EXTRA_CFLAGS)
LDFLAGS := $(SAN) $(EXTRA_LDFLAGS)
LDLIBS  := -lm -pthread

CORE_SRCS := $(wildcard $(ROOT)/src/core/*.c $(ROOT)/src/cpu/*.c $(ROOT)/src/rsp/gfx/*.c $(ROOT)/src/rsp/audio/*.c $(ROOT)/src/rsp/jpeg/*.c $(ROOT)/src/rsp/lle/*.c) $(ROOT)/src/frontend/savefile.c $(ROOT)/src/frontend/mempakfile.c $(ROOT)/src/frontend/perfilado.c $(ROOT)/src/frontend/version.c $(ROOT)/src/rdp/targets.c $(ROOT)/src/rdp/mask32.c $(ROOT)/src/rdp/vmatch.c $(ROOT)/src/rdp/gfxdump.c $(ROOT)/src/rdp/blackbox.c
HOST_SRCS := $(wildcard $(CURDIR)/*.c)
OBJS      := $(patsubst $(ROOT)/%.c,$(OUT)/obj/%.o,$(CORE_SRCS) $(HOST_SRCS))

.PHONY: all clean

all: $(OUT)/salsa64-run

$(OUT)/salsa64-run: $(OBJS)
	@echo "  LD      $(notdir $@)"
	@$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(OUT)/obj/%.o: $(ROOT)/%.c
	@echo "  CC      $*.c"
	@mkdir -p $(@D)
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -f $(OUT)/salsa64-run

-include $(OBJS:.o=.d)
