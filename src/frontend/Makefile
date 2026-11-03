# SPDX-License-Identifier: GPL-2.0-or-later
# Salsa64 — frontend para PS3: el emulador con presentación por lib/rsxkit.
PROGRAM := salsa64
TITLE   := Salsa64
APPID   := SLSA00064
ifeq ($(FILTROS_PRUEBA),1)
TITLE   := Salsa64 Filtros
APPID   := SLSA00210
endif
# App de perfilado (docs/perfilado.md): `make PERFILADO=1 pkg` enciende el
# registro por omisión y la arma como otra app («Salsa64 perfil»,
# SLSA00120), en build/salsa64-perfil. La app normal lo lleva apagado.
PERFILADO ?= 0
ifeq ($(PERFILADO),1)
PROGRAM := salsa64-perfil
TITLE   := Salsa64 perfil
APPID   := SLSA00120
endif
SRCS    := main.c audio_out.c audio_sync.c menu.c osd.c osd_fps.c pacer.c padmap.c pausectl.c scale_auto.c replay.c romlist.c ziprom.c rarrom.c sevenziprom.c savefile.c mempakfile.c transferpakfile.c \
           cheats.c cuelgue.c intro.c intro_logic.c cover.c download.c dlutil.c netstack.c ra.c ra_core.c ra_rules.c i18n.c namematch.c screenshot.c selftest.c selftest_audio.c selftest_gfx.c states.c cajanegra.c \
           version.c perfilado.c perfilado_ps3.c filters/crt_params.c filters/filters.c seda.c seda_calc.c cartucho3d.c cartucho_mesh.c thumbs.c thumbs_calc.c prompts.c
# Filtros de la presentación (filters/): fragment programs en Cg compilados
# con cgcomp. Van en fp32: cgcomp pide -fastprecision a Cg, que baja a fp16
# cuentas en píxeles que no lo aguantan.
# El fondo «Seda» del menú (seda/, docs/fondo-seda.md) va igual.
SHADERS := $(sort $(wildcard filters/*.fcg seda/*.fcg))
CGCFLAGS := -Wcg,-nofastprecision
FPO_CHECK := 1

R       := ../..
RC_TOP  := $(R)
include $(R)/mk/rcheevos.mk
ICON0   := $(R)/assets/ps3/ICON0.PNG
PIC1    := $(R)/assets/ps3/PIC1.PNG
COMMON  := $(RC_FILES) src/platform/ps3/log.c src/platform/ps3/rec_cache.c src/platform/ps3/fastmem.c src/platform/ps3/gfx_spu.c \
           src/platform/ps3/audio_spu.c src/platform/ps3/jpeg_spu.c src/platform/ps3/rsp_spu.c src/platform/ps3/bgjob.c src/platform/ps3/bgio.c src/rdp/rdp.c src/rdp/layer8.c src/rdp/mask32.c src/rdp/targets.c src/rdp/vmatch.c src/rdp/gfxdump.c src/rdp/blackbox.c \
           $(patsubst $(R)/%,%,$(wildcard $(R)/src/core/*.c $(R)/src/cpu/*.c $(R)/src/rsp/gfx/*.c $(R)/src/rsp/audio/*.c $(R)/src/rsp/jpeg/*.c \
                                          $(R)/src/rsp/lle/*.c))
DATA    := vitest_rom gfxtest_rom audiotest_rom cart_icon cart_mesh cart_plastic cart_label prompts icon_logo
icon_logo_FILE := assets/ps3/ICON0.PNG
vitest_rom_FILE := tests/roms/homebrew/salsa64-vitest.z64
gfxtest_rom_FILE := tests/roms/homebrew/salsa64-gfxtest.z64
audiotest_rom_FILE := tests/roms/homebrew/salsa64-audiotest.z64
# Ícono del cartucho de la lista de juegos (render del modelo, docs/THIRD_PARTY.md).
cart_icon_FILE := assets/menu/cartucho.png
# El cartucho 3D del menú: malla (tools/cartucho-malla.py) y textura del plástico.
cart_mesh_FILE := assets/menu/cartucho.mesh
cart_plastic_FILE := assets/menu/cartucho_plastico.png
cart_label_FILE := assets/menu/cartucho_etiqueta.png
# Íconos de botones del menú (Kenney «Input Prompts», CC0; tools/prompts-atlas.py).
prompts_FILE := assets/menu/prompts.png
RSXKIT  := 1
SPUKIT  := 1
# HLE gráfica (SPE 0) y de audio (SPE 1): el mismo código de src/rsp/gfx y
# src/rsp/audio con la E/S por DMA. RSP a bajo nivel (otro SPE): el
# intérprete de src/rsp/lle con la E/S por DMA de src/spu/rsp. Tarea JPEG
# (otro SPE): src/rsp/jpeg en 4 carriles (jpeg_v.c) con el bucle de DMA de
# src/spu/jpeg, y en el mismo SPE las tres tareas de video de Resident Evil 2
# (re2.c). CRT con cable (dos SPEs, con `filtros-spe`): la señal y la
# decodificación de S-Video y compuesto (src/spu/crt).
# La HLE gráfica del SPE 0 va en seis imágenes desde las mismas fuentes, cada
# una con sus familias de microcódigos (src/rsp/gfx/spe0.h,
# docs/spe0-familias.md): gfx0 la primera generación (F3D, F3DEX 1.x y las
# variantes de Rare sobre F3D), gfx1 la segunda de Nintendo (F3DEX2, F3DZEX2,
# S2DEX2 y F3DFLX en la tarea), gfx2 la de Conker (F3DEXBG y el resto de la
# segunda), gfx3 la de Rogue Squadron (Factor 5, con lo común de F3D) y gfx4 la
# de Indiana Jones y Battle for Naboo (Factor 5 con su DMEM de 4 KB); gfx5,
# Turbo3D con su Fast3D acompañante y Acclaim sobre F3DEX2. Cada una
# con sus objetos en obj/spu-gfx<n>.
GFX_IMAGES := 0 1 2 3 4 5
GFX_PHASES := 8
SPU_PROGS := $(addprefix gfx,$(GFX_IMAGES)) $(addprefix gfx1fase,$(GFX_PHASES)) audio rsp jpeg crt lz seda
GFX_SPU_SRCS := $(R)/src/spu/gfx/main.c $(R)/src/spu/gfx/io_spu.c \
                $(filter-out %/io_mem.c %/sync.c %/state.c %/selftest.c,$(wildcard $(R)/src/rsp/gfx/*.c))
$(foreach i,$(GFX_IMAGES),$(eval gfx$(i)_SRCS := $(GFX_SPU_SRCS)) $(eval gfx$(i)_OBJDIR := spu-gfx$(i)) \
	$(eval gfx$(i)_CFLAGS := -DGFX_SPE0_IMG=$(i)))
$(foreach p,$(GFX_PHASES),$(eval gfx1fase$(p)_SRCS := $(GFX_SPU_SRCS)) $(eval gfx1fase$(p)_OBJDIR := spu-gfx1fase$(p)) \
	$(eval gfx1fase$(p)_CFLAGS := -DGFX_SPE0_IMG=1 -DGFX_SPE0_PHASE=$(p)))
audio_SRCS := $(R)/src/spu/audio/main.c $(R)/src/spu/audio/io_spu.c \
              $(filter-out %/io_mem.c %/sync.c %/selftest.c,$(wildcard $(R)/src/rsp/audio/*.c))
rsp_SRCS   := $(R)/src/spu/rsp/main.c $(R)/src/spu/rsp/io_spu.c \
              $(filter-out %/core.c,$(wildcard $(R)/src/rsp/lle/*.c))
jpeg_SRCS  := $(R)/src/spu/jpeg/main.c $(R)/src/rsp/jpeg/jpeg.c $(R)/src/rsp/jpeg/jpeg_v.c $(R)/src/rsp/jpeg/re2.c
crt_SRCS   := $(R)/src/spu/crt/main.c
# Fondo «Seda» del menú (docs/fondo-seda.md): las cintas de tela de seda_calc.c, el mismo código del PPE.
seda_SRCS  := $(R)/src/spu/seda/main.c $(R)/src/frontend/seda_calc.c
# Compresor LZ de lib/spukit (estados guardados, docs/estados.md): bloques de 64 KB.
lz_SRCS    := $(R)/lib/spukit/spu/lz.c
# Pila del SPE 0 (src/spu/gfx/main.c), en cada imagen: el enlace falla si
# entre la cadena de llamadas más honda y el final del programa quedan menos
# de 1,5 KB. Las variantes llaman por puntero desde su familia; en gfx5 el
# recorrido F3DEX2 usa el gancho de Acclaim. Lo demás que llama por puntero es
# el arranque y la salida de la biblioteca de C.
GFX_STACK_LIBC := --ignore exit --ignore __call_exitprocs --ignore __do_global_ctors_aux \
                  --ignore __do_global_dtors_aux --ignore register_tm_clones --ignore deregister_tm_clones
$(foreach i,$(GFX_IMAGES),$(eval gfx$(i)_STACK_MIN := 1536))
$(foreach p,$(GFX_PHASES),$(eval gfx1fase$(p)_STACK_MIN := 1536))
gfx0_STACK_ARGS := --edge gfx_dl_f3d:pd_command --edge gfx_dl_f3d:dkr_command $(GFX_STACK_LIBC)
gfx1_STACK_ARGS := --ignore gfx_dl_f3dex2 $(GFX_STACK_LIBC)
$(foreach p,$(GFX_PHASES),$(eval gfx1fase$(p)_STACK_ARGS := $(gfx1_STACK_ARGS)))
gfx2_STACK_ARGS := --edge gfx_dl_f3dex2:cbfd_command $(GFX_STACK_LIBC)
gfx3_STACK_ARGS := --edge gfx_dl_f3d:f5_command $(GFX_STACK_LIBC)
gfx4_STACK_ARGS := $(GFX_STACK_LIBC)
gfx5_STACK_ARGS := --ignore gfx_dl_f3d --edge gfx_dl_f3dex2:acclaim_command $(GFX_STACK_LIBC)
ZLIB_PREFIX ?= $(PS3DEV)/portlibs/ppu
ARCHIVE_7Z_DIR := $(R)/third_party/libun7zip
ARCHIVE_RAR_DIR := $(R)/third_party/libunrar-ps3
ARCHIVE_LIBS := $(ARCHIVE_7Z_DIR)/libun7zip.a $(ARCHIVE_RAR_DIR)/libunrar.a
CFLAGS  := -I$(ZLIB_PREFIX)/include -I$(ARCHIVE_7Z_DIR)/include -I$(ARCHIVE_RAR_DIR)/include -ffp-contract=off -fno-math-errno $(RC_DEFS) $(RC_INC)
LIBS    := -lrsx -lgcm_sys -laudio -lio -lfontFT -lfont -lsysutil -lhttp -lhttputil -lssl -lnetctl -lnet -lsysmodule -lrt -llv2 -lstdc++ -lm -lz
# Caché del recompilador al final del segmento de texto (docs/rec-ppe.md).
LDFLAGS := -L$(ZLIB_PREFIX)/lib -T $(abspath $(R))/mk/rec-cache.ld -T lv2.ld

include $(R)/mk/ps3.mk

# Bibliotecas estáticas de los submódulos fijados en .gitmodules.
.PHONY: archive-libs
archive-libs:
	$(MAKE) --no-print-directory -C $(ARCHIVE_7Z_DIR) PSL1GHT="$(PSL1GHT)" PORTLIBS="$(ZLIB_PREFIX)"
	$(MAKE) --no-print-directory -C $(ARCHIVE_RAR_DIR) PSL1GHT="$(PSL1GHT)" PORTLIBS="$(ZLIB_PREFIX)"

$(ARCHIVE_LIBS): | archive-libs
$(OBJ)/$(PROGRAM).raw.elf: $(ARCHIVE_LIBS)


# Versión, título y APPID para version.c (menú, pausa y primera línea del
# log). tools/build.sh calcula SALSA64_VERSION y SALSA64_COMMIT fuera del
# contenedor: ahí dentro el .git de un worktree apunta a una ruta del host.
ifndef SALSA64_VERSION
SALSA64_VERSION := $(shell git -C $(R) describe --tags --always --dirty 2>/dev/null)
endif
ifndef SALSA64_COMMIT
SALSA64_COMMIT := $(shell git -C $(R) rev-parse --short HEAD 2>/dev/null)
endif
# Una cadena de C protegida para el shell (\, " y ' adentro).
c_str = '"$(subst ','\'',$(subst ",\",$(subst \,\\,$(1))))"'
VERSION_DEFS := -DSALSA64_VERSION=$(call c_str,$(or $(SALSA64_VERSION),desconocida)) \
                -DSALSA64_COMMIT=$(call c_str,$(SALSA64_COMMIT)) \
                -DSALSA64_TITLE=$(call c_str,$(TITLE)) -DSALSA64_APPID=$(call c_str,$(APPID)) \
                -DSALSA64_PERFILADO=$(PERFILADO)
$(OBJ)/src/frontend/version.c.o $(OBJ)/src/frontend/perfilado_ps3.c.o: PPU_CFLAGS += $(VERSION_DEFS)
# version.txt cambia solo si cambia algo de eso: así version.c se recompila
# (y el programa se vuelve a enlazar) solo entonces, y un `make pkg` con
# otro TITLE o APPID no se queda con los del build anterior.
$(OBJ)/src/frontend/version.c.o $(OBJ)/src/frontend/perfilado_ps3.c.o: $(OBJ)/version.txt
.PHONY: FORCE
$(OBJ)/version.txt: FORCE
	@mkdir -p $(@D)
	@printf '%s\n' $(VERSION_DEFS) >$@.tmp; if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi

# Lo que incluyen los fragment programs de los filtros.
$(OBJ)/crt_mask_dots.fpo: filters/crt.cgh
$(OBJ)/crt_cable_decode9.fpo $(OBJ)/crt_cable_decode27.fpo: filters/crt_cable_decode.cgh
$(OBJ)/crt_tube_lines_a4.fpo $(OBJ)/crt_tube_lines_a8.fpo $(OBJ)/crt_tube_lines_b4.fpo \
$(OBJ)/crt_tube_lines_b8.fpo $(OBJ)/crt_trinitron_lines_a.fpo \
$(OBJ)/crt_trinitron_lines_b.fpo: filters/crt_tube_lines.cgh

# Un objeto de src/rsp/gfx en todas las imágenes del SPE 0.
GFX_OBJ = $(foreach i,$(GFX_IMAGES),$(OBJ)/spu-gfx$(i)/src/rsp/gfx/$(1))
# Las variantes de medición son la imagen 1 con un único intervalo activo.
# Deben conservar las mismas opciones de tamaño que la imagen normal:
# de otro modo se mediría otro programa y se pondría en riesgo su pila.
GFX_PHASE_OBJ = $(foreach p,$(GFX_PHASES),$(OBJ)/spu-gfx1fase$(p)/src/rsp/gfx/$(1))
GFX_ALL_OBJ = $(call GFX_OBJ,$(1)) $(call GFX_PHASE_OBJ,$(1))

# El combiner, con -Os en el SPE 0: 5,6 KB menos de código (de 16,8 a 11,2
# KB) que quedan para la pila. Corre una vez por estado de dibujo, no por
# píxel ni por vértice.
$(call GFX_ALL_OBJ,combine.c.o): SPU_CFLAGS += -Os
# Los fondos de S2DEX (salas prerrenderizadas de Zelda OoT), también: 0,9 KB
# menos de código y 112 bytes menos de marco en la cadena más honda de la
# pila; se dibujan en unas decenas de tiras por cuadro.
$(call GFX_ALL_OBJ,s2dex.c.o): SPU_CFLAGS += -Os
# El despacho de comandos de Perfect Dark (f3dpd.c; los vértices y las
# texturas van en geom.c y tmem.c), también: lo que ahorra paga el TEXRECT
# dentro del tramo S2DEX (el cielo de Kirby 64).
$(call GFX_ALL_OBJ,f3dpd.c.o): SPU_CFLAGS += -Os
# Sprite2D es una ruta 2D de la primera generación; mantener compacto el
# despacho F3D deja la holgura de pila exigida por la imagen 0.
$(OBJ)/spu-gfx0/src/rsp/gfx/f3d.c.o: SPU_CFLAGS += -Os
# La envoltura de fase 8 incluye la autoprueba de recorte y no forma parte
# del intervalo medido; compactarla conserva la misma HLE con 1,5 KB de pila.
$(OBJ)/spu-gfx1fase8/src/spu/gfx/main.c.o: SPU_CFLAGS += -Os
# Lo de Diddy Kong Racing (f3ddkr.c: matrices, vértices y triángulos por DMA),
# también: solo corre con ese juego.
$(call GFX_ALL_OBJ,f3ddkr.c.o): SPU_CFLAGS += -Os
# Lo de Rogue Squadron (f5rogue.c: vértices, triángulos, mallas generadas y
# rectángulos), también: solo corre con ese juego.
$(call GFX_ALL_OBJ,f5rogue.c.o): SPU_CFLAGS += -Os
# Y la de Indiana Jones y Naboo (f5indy.c), igual.
$(call GFX_ALL_OBJ,f5indy.c.o): SPU_CFLAGS += -Os
# Lo de la interpolación de cuadros (vgroup.c), también: no le quita local
# store ni pila a la HLE.
$(call GFX_ALL_OBJ,vgroup.c.o): SPU_CFLAGS += -Os
# Lo de F3DFLX, el microcódigo de F-Zero X (flx.c), también: solo corre con
# ese juego. Y el recorrido de la display list (dl.c) vuelve a -Os para
# pagarlo (−370 bytes, docs/estado.md 2026-10-13): de lo suyo solo
# gfx_dl_next corre por comando, y es corta.
$(call GFX_ALL_OBJ,flx.c.o) $(call GFX_ALL_OBJ,dl.c.o): SPU_CFLAGS += -Os
# F3DEX2 solo descodifica y despacha comandos; el trabajo por vértice y
# triángulo queda en geom.c y rdp.c. En la imagen 1, hacerlo compacto deja
# lugar en el local store para el lote SIMD de geometría.
$(call GFX_ALL_OBJ,f3dex2.c.o) $(call GFX_ALL_OBJ,f3dex2acclaim.c.o): SPU_CFLAGS += -Os
# La carga/decodificación de TMEM no es la ruta que se vectoriza y, en la
# imagen 1, el tamaño compacto libera local store para la geometría caliente.
$(OBJ)/spu-gfx1/src/rsp/gfx/tmem.c.o $(call GFX_PHASE_OBJ,tmem.c.o): SPU_CFLAGS += -Os
# La caché de texturas (texcache.c), también: ~1,2 KB de código para la pila
# al juntar F3DFLX con el S2DEX de las salas de Zelda (la holgura bajaba a
# 1328); identificar texturas cuesta 0,01–0,02 ms más por tarea.
$(call GFX_ALL_OBJ,texcache.c.o): SPU_CFLAGS += -Os
# La envoltura del SPE 0 (src/spu/gfx/main.c) sin sacar invariantes de los
# bucles: del de spukit_run (una vuelta por trabajo) GCC sacaba constantes a
# 44 registros que main guarda en la pila, 736 bytes de marco debajo de toda
# tarea gráfica; así son 224 (192 en la fase 8, con -Os) y el código, 224
# bytes menos. Cuesta unas pocas constantes más que cargar por trabajo, no
# por comando.
GFX_MAIN_OBJ := $(foreach i,$(GFX_IMAGES),$(OBJ)/spu-gfx$(i)/src/spu/gfx/main.c.o) \
                $(foreach p,$(GFX_PHASES),$(OBJ)/spu-gfx1fase$(p)/src/spu/gfx/main.c.o)
$(GFX_MAIN_OBJ): SPU_CFLAGS += -fno-move-loop-invariants -fno-tree-loop-im
# make no ve un cambio de banderas: que se recompilen si cambia este archivo.
$(call GFX_ALL_OBJ,combine.c.o) $(call GFX_ALL_OBJ,s2dex.c.o) $(OBJ)/spu-gfx0/src/rsp/gfx/f3d.c.o $(call GFX_ALL_OBJ,f3dpd.c.o) \
$(call GFX_ALL_OBJ,f3ddkr.c.o) $(call GFX_ALL_OBJ,f5rogue.c.o) $(call GFX_ALL_OBJ,f5indy.c.o) $(call GFX_ALL_OBJ,vgroup.c.o) $(call GFX_ALL_OBJ,flx.c.o) \
$(call GFX_ALL_OBJ,dl.c.o) $(call GFX_ALL_OBJ,f3dex2.c.o) $(call GFX_ALL_OBJ,f3dex2acclaim.c.o) $(call GFX_ALL_OBJ,texcache.c.o): Makefile
$(OBJ)/spu-gfx1/src/rsp/gfx/tmem.c.o $(call GFX_PHASE_OBJ,tmem.c.o): Makefile
$(OBJ)/spu-gfx1fase8/src/spu/gfx/main.c.o $(GFX_MAIN_OBJ): Makefile
