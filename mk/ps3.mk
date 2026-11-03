# mk/ps3.mk — reglas comunes para programas de Salsa64 con PSL1GHT.
#
# El Makefile de cada programa define, antes del include:
#   PROGRAM     nombre del ejecutable
#   TITLE       título para PARAM.SFO
#   APPID       ID de título (4 letras + 5 dígitos)
#   SRCS        fuentes PPU, relativas al directorio del programa
#   COMMON      fuentes PPU compartidas, relativas a la raíz del repo
#   LIBS        bibliotecas de PSL1GHT a enlazar
#   SPU_PROGS   programas SPU a embeber; para cada uno, <nombre>_SRCS y,
#               opcional, <nombre>_STACK_MIN (holgura mínima de la pila en
#               bytes: si no alcanza, falla el enlace; tools/spu-stack.py) y
#               <nombre>_STACK_ARGS (sus --edge y --ignore)
#   SHADERS     shaders Cg (.vcg/.fcg), relativos al directorio del programa
#   CGCFLAGS    opciones de cgcomp (p. ej. -Wcg,-nofastprecision)
#   FPO_CHECK   1: revisa los fragment programs: corrige el choque del color
#               de salida con R0 (tools/cg-salida.py) y falla si usan
#               códigos de condición (tools/fpo-info.py); ver docs/rsx-notes.md
#   CFLAGS      opcional, se agrega a los flags de compilación PPU
#   LDFLAGS     opcional, se agrega a los flags de enlace PPU
#   DATA        archivos a embeber; para cada uno, <nombre>_FILE (relativo a
#               la raíz). Símbolos <nombre>_bin[], _end[] y _size.
#   RSXKIT      1 para enlazar lib/rsxkit (se compila en build/rsxkit con su
#               propio Makefile, sin -Isrc)
#   SPUKIT      1 para enlazar lib/spukit (build/spukit) y poner sus
#               cabeceras al alcance del PPU y de los programas SPU
#
# Salidas en build/<PROGRAM>/:
#   <PROGRAM>.elf        stripped + sprxlinker: RPCS3 y ps3load
#   <PROGRAM>.self       make_self
#   <PROGRAM>.fake.self  fself
#   <PROGRAM>.pkg        solo con `make pkg`
#   obj/<PROGRAM>.raw.elf  con símbolos, para addr2line
#
# Cada binario embebido (programa SPU o shader) genera en obj/ un header
# <archivo>_<ext>.h con los símbolos <archivo>_<ext>[], <archivo>_<ext>_end[]
# y <archivo>_<ext>_size. Los SPU se llaman spu_<nombre>.bin.

ifeq ($(strip $(PSL1GHT)),)
$(error PSL1GHT no está definido; compilar con tools/build.sh)
endif

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))..)
HERE := $(CURDIR)
OUT  := $(ROOT)/build/$(PROGRAM)
OBJ  := $(OUT)/obj

ifneq ($(V),1)
  VERB := @
endif

PPU_MACHDEP := -mhard-float -fmodulo-sched -ffunction-sections -fdata-sections
SPU_MACHDEP := -mdual-nops -fmodulo-sched -ffunction-sections -fdata-sections

PPU_CFLAGS  := -std=gnu11 -O2 -Wall -Wextra -mcpu=cell -mno-relative-jumptables $(PPU_MACHDEP) \
               -I$(PSL1GHT)/ppu/include -I$(PSL1GHT)/ppu/include/simdmath \
               -I$(ROOT)/src -I$(ROOT)/lib/spukit/include -I$(HERE) -I$(OBJ) $(CFLAGS)
PPU_LDFLAGS := $(PPU_MACHDEP) -L$(PSL1GHT)/ppu/lib -Wl,-Map,$(OUT)/$(PROGRAM).map $(LDFLAGS)
# -fstack-usage: el marco de cada función en un .su junto al objeto, para
# medir la pila de los programas SPU (tools/spu-stack.py).
SPU_CFLAGS  := -std=gnu11 -O2 -Wall -Wextra $(SPU_MACHDEP) -fstack-usage \
               -I$(PSL1GHT)/spu/include -I$(PSL1GHT)/spu/include/simdmath \
               -I$(ROOT)/src -I$(HERE) $(SPU_EXTRA_CFLAGS)
# Con -ffunction-sections, --gc-sections deja fuera del local store lo que
# nadie llama (de la biblioteca de C y de las fuentes compartidas con el
# host): 2,4–2,7 KB menos en los SPEs de la HLE gráfica, del RSP y del JPEG.
SPU_LDFLAGS := $(SPU_MACHDEP) -Wl,--gc-sections -L$(PSL1GHT)/spu/lib
SPU_LIBS    ?= -lsputhread

CONTENTID ?= UP0001-$(APPID)_00-0000000000000000

PPU_OBJS    := $(patsubst $(ROOT)/%,$(OBJ)/%.o,$(abspath $(SRCS)) $(addprefix $(ROOT)/,$(COMMON)))
SHADER_BINS := $(addprefix $(OBJ)/,$(patsubst %.vcg,%.vpo,$(patsubst %.fcg,%.fpo,$(notdir $(SHADERS)))))
SPU_BINS    := $(foreach p,$(SPU_PROGS),$(OBJ)/spu_$(p).bin)
DATA_BINS   := $(foreach d,$(DATA),$(OBJ)/$(d).bin)
EMBED_OBJS  := $(addsuffix .o,$(SHADER_BINS) $(SPU_BINS) $(DATA_BINS))

ifeq ($(RSXKIT),1)
RSXKIT_LIB  := $(ROOT)/build/rsxkit/librsxkit.a
PPU_CFLAGS  += -I$(ROOT)/lib/rsxkit/include
endif

ifeq ($(SPUKIT),1)
SPUKIT_LIB  := $(ROOT)/build/spukit/libspukit.a
PPU_CFLAGS  += -I$(ROOT)/lib/spukit/include
SPU_CFLAGS  += -I$(ROOT)/lib/spukit/include
endif

vpath %.vcg $(sort $(dir $(abspath $(SHADERS))))
vpath %.fcg $(sort $(dir $(abspath $(SHADERS))))

.PHONY: all pkg clean
.SECONDARY:

all: $(OUT)/$(PROGRAM).elf $(OUT)/$(PROGRAM).self $(OUT)/$(PROGRAM).fake.self

pkg: $(OUT)/$(PROGRAM).pkg

clean:
	rm -rf $(OUT)

# --- PPU ---------------------------------------------------------------------
# Los headers de los binarios embebidos deben existir antes de compilar.
$(OBJ)/%.c.o: $(ROOT)/%.c | $(EMBED_OBJS)
	@echo "  PPU CC  $*.c"
	@mkdir -p $(@D)
	$(VERB)ppu-gcc $(PPU_CFLAGS) -MMD -MP -c $< -o $@

$(OBJ)/%.S.o: $(ROOT)/%.S
	@echo "  PPU AS  $*.S"
	@mkdir -p $(@D)
	$(VERB)ppu-gcc $(PPU_CFLAGS) -MMD -MP -x assembler-with-cpp -c $< -o $@

$(OBJ)/$(PROGRAM).raw.elf: $(PPU_OBJS) $(EMBED_OBJS) $(RSXKIT_LIB) $(SPUKIT_LIB)
	@echo "  PPU LD  $(notdir $@)"
	$(VERB)ppu-gcc $(PPU_LDFLAGS) $^ $(LIBS) -o $@

$(OUT)/$(PROGRAM).elf: $(OBJ)/$(PROGRAM).raw.elf
	@echo "  ELF     $(notdir $@)"
	$(VERB)ppu-strip $< -o $@
	$(VERB)sprxlinker $@

$(OUT)/$(PROGRAM).self: $(OUT)/$(PROGRAM).elf
	@echo "  SELF    $(notdir $@)"
	$(VERB)make_self $< $@ >/dev/null

$(OUT)/$(PROGRAM).fake.self: $(OUT)/$(PROGRAM).elf
	@echo "  FSELF   $(notdir $@)"
	$(VERB)fself $< $@

# Ícono del XMB (320×176) y fondo opcional (1920×1080): cada programa puede
# fijar ICON0 y PIC1; por omisión, el ícono genérico de PSL1GHT y sin fondo.
ICON0 ?= $(PS3DEV)/bin/ICON0.PNG
PIC1  ?=

$(OUT)/$(PROGRAM).pkg: $(OUT)/$(PROGRAM).elf $(ICON0) $(PIC1)
	@echo "  PKG     $(notdir $@)"
	$(VERB)rm -rf $(OBJ)/pkg && mkdir -p $(OBJ)/pkg/USRDIR
	$(VERB)cp $(ICON0) $(OBJ)/pkg/ICON0.PNG
	$(if $(PIC1),$(VERB)cp $(PIC1) $(OBJ)/pkg/PIC1.PNG)
	$(VERB)make_self_npdrm $< $(OBJ)/pkg/USRDIR/EBOOT.BIN $(CONTENTID) >/dev/null
	$(VERB)sfo --title "$(TITLE)" --appid "$(APPID)" -f $(PS3DEV)/bin/sfo.xml $(OBJ)/pkg/PARAM.SFO
	$(VERB)pkg --contentid $(CONTENTID) $(OBJ)/pkg/ $@ >/dev/null
	$(VERB)cp $@ $(basename $@).gnpdrm.pkg
	$(VERB)package_finalize $(basename $@).gnpdrm.pkg

# lib/rsxkit con su propio Makefile; siempre se consulta (es barato) y el
# enlace se rehace solo si cambió la biblioteca.
ifeq ($(RSXKIT),1)
.PHONY: rsxkit
rsxkit:
	$(VERB)$(MAKE) --no-print-directory -C $(ROOT)/lib/rsxkit OUT=$(ROOT)/build/rsxkit
$(RSXKIT_LIB): rsxkit ;
endif

ifeq ($(SPUKIT),1)
.PHONY: spukit
spukit:
	$(VERB)$(MAKE) --no-print-directory -C $(ROOT)/lib/spukit OUT=$(ROOT)/build/spukit
$(SPUKIT_LIB): spukit ;
endif

# --- SPU ---------------------------------------------------------------------
$(OBJ)/spu/%.c.o: $(ROOT)/%.c
	@echo "  SPU CC  $*.c"
	@mkdir -p $(@D)
	$(VERB)spu-gcc $(SPU_CFLAGS) -MMD -MP -c $< -o $@

# Un programa con <nombre>_OBJDIR compila sus fuentes aparte, en obj/<dir>,
# con <nombre>_CFLAGS además de SPU_CFLAGS: varias imágenes desde las mismas
# fuentes (las del SPE 0, docs/spe0-familias.md). Con <nombre>_STACK_MIN, los
# números de la pila quedan también en obj/spu_<nombre>_stack.h para el PPE
# (SPU_<NOMBRE>_END, _FREE, _STACK y _SLACK).
define SPU_PROG_RULES
$(1)_OBJDIR ?= spu
$(1)_OBJS := $$(patsubst $$(ROOT)/%,$$(OBJ)/$$($(1)_OBJDIR)/%.o,$$(abspath $$($(1)_SRCS)))
ifneq ($$($(1)_OBJDIR),spu)
$$(OBJ)/$$($(1)_OBJDIR)/%.c.o: $$(ROOT)/%.c
	@echo "  SPU CC  $$*.c ($(1))"
	@mkdir -p $$(@D)
	$$(VERB)spu-gcc $$(SPU_CFLAGS) $$($(1)_CFLAGS) -MMD -MP -c $$< -o $$@
endif
$$(OBJ)/spu_$(1).bin: $$($(1)_OBJS)
	@echo "  SPU LD  spu_$(1)"
	$$(VERB)spu-gcc $$(SPU_LDFLAGS) $$^ $$(SPU_LIBS) -o $$@
	$$(VERB)spu-size $$@ | tail -n 1 | awk '{ printf "          local store: text %d, data %d, bss %d bytes\n", $$$$1, $$$$2, $$$$3 }'
	$$(if $$($(1)_STACK_MIN),$$(VERB)python3 $$(ROOT)/tools/spu-stack.py --bin $$@ --min $$($(1)_STACK_MIN) \
		--header $$(OBJ)/spu_$(1)_stack.h --name SPU_$$(shell echo $(1) | tr a-z A-Z) \
		$$($(1)_STACK_ARGS) $$($(1)_OBJS) || { rm -f $$@; exit 1; })
endef
$(foreach p,$(SPU_PROGS),$(eval $(call SPU_PROG_RULES,$(p))))

# --- Shaders y binarios embebidos ----------------------------------------------
$(OBJ)/%.vpo: %.vcg
	@echo "  CGCOMP  $(notdir $<)"
	@mkdir -p $(@D)
	$(VERB)cgcomp -v $(CGCFLAGS) $< $@

# Con FPO_CHECK, el ensamblador que Cg le da a cgcomp se revisa y se vuelve
# a ensamblar: el color de salida que choca con R0 va a un temporal
# (tools/cg-salida.py) y los códigos de condición hacen fallar el build
# (tools/fpo-info.py). Sin nada que corregir, el .fpo sale idéntico.
$(OBJ)/%.fpo: %.fcg
	@echo "  CGCOMP  $(notdir $<)"
	@mkdir -p $(@D)
ifeq ($(FPO_CHECK),1)
	$(VERB)cgcomp -f -d $(CGCFLAGS) $< $@ >/dev/null && \
		python3 $(ROOT)/tools/cg-salida.py $@.dump --corrige $@.asm && \
		cgcomp -f -a $@.asm $@ >/dev/null && \
		python3 $(ROOT)/tools/fpo-info.py $@ --sin-condiciones >/dev/null || { rm -f $@; exit 1; }
else
	$(VERB)cgcomp -f $(CGCFLAGS) $< $@
endif

# bin2s exporta el símbolo <archivo>_<ext>; el header declara los tres.
define EMBED
	@echo "  EMBED   $(notdir $<)"
	$(VERB)bin2s -a 64 $< | ppu-as -o $@
	$(VERB)sym=$$(echo $(notdir $<) | sed -e 's/^\([0-9]\)/_\1/' | tr . _); \
	printf '#include <ppu-types.h>\nextern const u8 %s[];\nextern const u8 %s_end[];\nextern const u32 %s_size;\n' \
		$$sym $$sym $$sym > $(OBJ)/$$(echo $(notdir $<) | tr . _).h
endef

define DATA_RULES
$$(OBJ)/$(1).bin: $$(ROOT)/$$($(1)_FILE)
	@mkdir -p $$(@D)
	$$(VERB)cp $$< $$@
endef
$(foreach d,$(DATA),$(eval $(call DATA_RULES,$(d))))

$(OBJ)/%.vpo.o: $(OBJ)/%.vpo ; $(EMBED)
$(OBJ)/%.fpo.o: $(OBJ)/%.fpo ; $(EMBED)
$(OBJ)/%.bin.o: $(OBJ)/%.bin ; $(EMBED)

-include $(PPU_OBJS:.o=.d) $(foreach p,$(SPU_PROGS),$($(p)_OBJS:.o=.d))
