# SPDX-License-Identifier: GPL-2.0-or-later
# Laboratorio del recompilador (docs/lab-recompilador.md): rec-lab con el
# compilador nativo del host (la Mac). Uso: tools/lab/build.sh [SRC] [OUT]
#   SRC  árbol de fuentes con src/cpu (por omisión, éste); sirve un checkout
#        de otra revisión para comparar emisores (tools/lab/lab-compara.sh)
#   OUT  carpeta de salida (por omisión build/lab/bin)

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
SRC  ?= $(ROOT)
OUT  ?= $(ROOT)/build/lab/bin
CC   ?= cc

CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -ffp-contract=off -fno-math-errno -Wno-unused-parameter \
          -DSALSA64_REC_LAB=1 -DSALSA64_LAB_TRACE=1 -I$(SRC)/src -I$(SRC)/lib/spukit/include
LDLIBS := -lm -pthread

# El mismo conjunto que src/host/Makefile, sin cpu/rec.c (lo incluye rec-lab.c).
CORE_SRCS := $(filter-out $(SRC)/src/cpu/rec.c,$(wildcard $(SRC)/src/core/*.c $(SRC)/src/cpu/*.c $(SRC)/src/rsp/gfx/*.c \
             $(SRC)/src/rsp/audio/*.c $(SRC)/src/rsp/jpeg/*.c $(SRC)/src/rsp/lle/*.c)) \
             $(SRC)/src/frontend/savefile.c $(SRC)/src/rdp/targets.c $(SRC)/src/rdp/mask32.c \
             $(wildcard $(SRC)/src/rdp/vmatch.c $(SRC)/src/rdp/gfxdump.c $(SRC)/src/rdp/blackbox.c)
OBJS := $(patsubst $(SRC)/%.c,$(OUT)/obj/%.o,$(CORE_SRCS)) $(OUT)/obj/rec-lab.o
HOST_OBJS := $(patsubst $(SRC)/%.c,$(OUT)/obj/%.o,$(CORE_SRCS) $(wildcard $(SRC)/src/host/*.c)) $(OUT)/obj/lab-trace.o

.PHONY: all
all: $(OUT)/rec-lab $(OUT)/salsa64-run-lab

$(OUT)/rec-lab: $(OBJS)
	@echo "  LD      rec-lab"
	@$(CC) $^ $(LDLIBS) -o $@

# salsa64-run con el gancho del intérprete y lab-trace.c (perfil exacto, lab-trace.c)
$(OUT)/salsa64-run-lab: $(HOST_OBJS)
	@echo "  LD      salsa64-run-lab"
	@$(CC) $^ $(LDLIBS) -o $@

$(OUT)/obj/lab-trace.o: $(ROOT)/tools/lab/lab-trace.c $(ROOT)/tools/lab/lab-emit.h
	@echo "  CC      lab-trace.c"
	@mkdir -p $(@D)
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OUT)/obj/rec-lab.o: $(ROOT)/tools/lab/rec-lab.c
	@echo "  CC      rec-lab.c"
	@mkdir -p $(@D)
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OUT)/obj/%.o: $(SRC)/%.c
	@echo "  CC      $*.c"
	@mkdir -p $(@D)
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(OBJS:.o=.d) $(HOST_OBJS:.o=.d)
