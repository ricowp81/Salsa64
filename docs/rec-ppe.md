# Recompilador MIPS → PPC64 en el PPE (Fase 3)

Código: `src/cpu/rec.h`, `src/cpu/rec.c` (despachador, caché de bloques,
enlaces, invalidación), `src/cpu/rec_emit.c` (traducción),
`src/cpu/rec_emit_ext.h` (conversiones de la FPU, CFC1/CTC1, LWL/LWR y
compañía, MFC0, CACHE; lo incluye rec_emit.c), `src/cpu/rec_tlb.c` (código
y datos fuera de KSEG0: KSEG1 y TLB), `src/cpu/rec_prof.c` (perfil por
causa), `src/cpu/ppc.h` (codificación de instrucciones PowerPC).

Las herramientas para medir el código que emite sin la PS3 (perfil exacto,
análisis de palabras, load-hit-store y caché de instrucciones, antes/después de
un cambio) están en `tools/lab/`; ver docs/lab-recompilador.md.

## Decisión: recompilador propio (2026-10-10)

Se revisaron las dos bases candidatas (informe verificado el 2026-10-10,
solo lectura, sin copiar código):

| | Not64 | Wii64 |
|---|---|---|
| Repositorio | github.com/extremscorner/not64, `master` | github.com/emukidid/Wii64, `master` |
| Commit revisado | `d35a7a1c454eca973995939dbca3f06725cf4a6e` (2026-04-10, tag `20260410`); último cambio en `r4300/ppc`: `deff3be` (2025-02-05) | `2e1c6fb056e1965a57f6be4accdbf5460e7de291` (2026-10-09); versión 1.5.0 (`a96d9758`, 2026-09-23); el dynarec tiene trabajo activo |
| Licencia | `LICENCE.TXT` = GPL v2; cabeceras de `r4300/ppc/*` y `Recomp-Cache*`: "either version 2 of the Licence, or any later version" → **GPLv2 o posterior**, compatible | igual |
| Copyright | Mike Slegeir (tehpola) 2007-2010 (`ppc/*`, `Recomp-Cache*`); Hacktarux 2002 (núcleo mupen64); emu_kidid (`interupt.c`, `exception.c`) | igual |
| Tamaño | `r4300/ppc/` 11 196 líneas (`MIPS-to-PPC.c` 7 011, `PowerPC.h` 1 606) | 9 705 líneas |

`r4300/Invalid_Code.c/.h` no tienen aviso de licencia (unas 30 líneas).
mupen64gc (github.com/emukidid/mupen64gc) está muerto desde 2010.

Lo que hace ese recompilador y por qué no se porta:

- **Es de 32 bits.** Cada GPR de 64 bits es un registro PPC de 32 (con el
  alto implícito por extensión de signo) o un par hi/lo; solo 10 registros
  asignables. `PowerPC.h` no tiene ni una instrucción de 64 bits (`ld`,
  `std`, `rldic*`, `mulld`, `divd`); `genCmp64` compara solo 32 bits si un
  operando está mapeado en 32 (FIXME en el código). En el PPE todo eso
  sobra: un GPR del MIPS cabe en uno del PPE.
- **ABI de la Wii.** EABI de 32 bits con SDA en r13, punteros cargados con
  `lis/ori`, llamadas a C con `bl` directo y r2 como temporal. PSL1GHT es
  ELFv1 de 64 bits: descriptores de función, TOC en r2, r13 reservado.
- **Depende del núcleo de mupen64**: globales `interp_addr`, `PC->addr`,
  `delay_slot`, `next_interupt`, `last_addr`, `reg_cop0`, `interp_ops[]`,
  las macros de memoria sobre `address`/`word`… Nuestro CPU y bus son
  otros; integrarlo sería reescribir la interfaz entera.
- **No cumple el criterio de la Fase 3** ("mismas trazas que el
  intérprete"): Count se actualiza solo en saltos (`count_per_op` fijo) y
  los eventos se revisan por bloque; ADD/ADDI/SUB no lanzan desbordamiento;
  la FPU es nativa sin FCR31 (sin causas, banderas ni flush-to-zero); los
  stores con base sp/gp/ra/k0/k1 no revisan invalidación.
- **JR/JALR no se enlazan**: cada retorno de función vuelve al trampolín C.

Portarlo sería reescribir el emisor, el mapeo de registros, la interfaz con
el núcleo y las reglas de tiempo: más trabajo que uno propio, y sin poder
compararlo instrucción por instrucción con nuestro intérprete. Se escribió
uno propio (GPLv2+, sin código de terceros) tomando de Not64/Wii64 solo
ideas de diseño, que son comunes a los recompiladores: bloques con enlace
por parcheo de un salto, mapa de páginas con código para invalidar, camino
rápido de la RDRAM con una prueba de rango y condición del salto en un
campo de CR que sobrevive al delay slot.

## Diseño

### Cuándo corre en nativo

El despachador (`salsa64_rec_run`) reemplaza al bucle del intérprete. Un
bloque corre en nativo solo si:

- modo kernel, sin interrupción ni delay slot pendientes;
- la instrucción está en la RDRAM (por KSEG0, KSEG1 o la TLB) o en la ROM
  del cartucho (por KSEG1 o la TLB; ver «Código fuera de KSEG0»);
- cabe entero antes del próximo evento: `cycles + n ≤ sched->next` (lo
  revisa el propio bloque al entrar, también al llegar por un enlace).

Si no, el intérprete avanza hasta terminar un bloque básico (hasta un delay
slot o una excepción) y el despachador vuelve a intentarlo. Así cada evento
se atiende en el mismo ciclo que con el intérprete solo, y Count, Random y
VI_V_CURRENT dan lo mismo. Tras una excepción o un ERET dentro del bloque
(`redirect`) el despachador busca el bloque de la dirección nueva como
cualquier otro (hasta el 2026-10-10 tras un ERET interpretaba hasta el fin
del bloque básico: en GoldenEye, ~400 ERET por campo del refill de la TLB). Modo usuario o supervisor, código en otra memoria
(SP DMEM, PIF), direcciones de 64 bits y kuseg con ERL: intérprete.

### Bloques

Desde `pc` hasta un salto con su delay slot, una instrucción de COP0 que
no sea MFC0/DMFC0, SYSCALL o BREAK (siempre vuelven a C), o 64
instrucciones. Después de la instrucción de COP0 (la corre el intérprete
desde el código, `rec_h_exec`) el bloque **enlaza** con el siguiente como
cualquier salida a dirección fija: `rec_h_exec` hace ahí lo que el
despachador haría antes del bloque siguiente (revisar la TLB del
recompilador, `salsa64_rec_x_sync`, y pedir vuelta al despachador con
`rec_abort` si cambió Status.FR) y la revisión de después de la llamada
mira el modo, las interrupciones y el evento. Hasta el 2026-10-10 cada COP0
salía al despachador y el intérprete corría el resto del bloque básico:
GoldenEye hacía así 13 000 instrucciones interpretadas por campo (su refill
de la TLB, ~400 por campo, tiene cuatro COP0 seguidos). Un salto cuyo delay slot es otro salto o una de esas
instrucciones de COP0 queda fuera del bloque (lo corre el intérprete). La condición se calcula antes del delay slot en `cr4` (no
volátil, sobrevive a las llamadas a C); branch-likely no tomado salta el
delay slot y cuenta un ciclo menos.

### Saltos condicionales internos (superbloques, 2026-10-11)

GoldenEye ejecutaba 5,3 instrucciones por bloque (perfil por bloque,
pesado por ejecuciones): cada `if` y cada salida de bucle cortaban el
bloque, y cada corte cuesta la prueba del evento, la escritura y recarga de
registros por `cpu->gpr` (load-hit-store en el PPE) y el enlace. Ahora un
salto condicional **hacia adelante** (BEQ, BNE, BLEZ, BGTZ, BLTZ, BGEZ,
BC1F/T y sus likely) no termina el bloque: su delay slot se emite en línea
y el camino **tomado** sale enlazado al destino (registros modificados
escritos, `cycles` hasta el delay slot), mientras el no tomado sigue en el
mismo bloque con los registros en el PPE (`emit_internal_branch`). El
bloque termina en un salto hacia atrás (al inicio, bucle; u otro), en
J/JAL/JR/JALR, en una instrucción que vuelve a C o a las 64
instrucciones. Los tres bucles más calientes de GoldenEye (comparación de
cadenas, búsqueda en una tabla) eran parejas de bloques y pasan a ser uno.

Detalles que mantienen la exactitud:
- Cada salto guarda su descripción en `c->br[k]` y su delay slot se marca
  en `ds_of[]`: una salida a C desde ese delay slot continúa según su salto
  (`emit_continue`, con la condición en cr4, que las llamadas a C no tocan).
- Un branch-likely interno no tomado salta su delay slot y cuesta un ciclo
  menos: se descuenta de r29 (`addi r29, r29, -1`) para que `cycles =
  r29 + k + 1` siga valiendo en el resto del bloque; y el registro que
  escribe el delay slot se carga a la entrada (livein), para que la
  escritura de registros al salir lo deje con el valor de antes en ese
  camino.
- Los pares LWL/LWR y SWL/SWR no cruzan un salto.
- Solo el salto final puede ser candidato a bucle de espera (idle.c); los
  internos son hacia adelante.
- `SALSA64_REC_DBG_NO_SUPER` (y LEGACY) vuelven a cortar en cada salto.

### Saltos internos con destino dentro del bloque (2026-10-11)

Los dos bloques más ejecutados de GoldenEye (3,1 % cada uno) eran un bucle
de comparación partido en dos: el cuerpo termina en el `bne` que vuelve al
inicio, pero un `beql` del cuerpo salta a ese mismo `bne` (el final del
bucle, dentro del bloque), y el camino tomado salía enlazado a un bloque
de dos instrucciones que a su vez enlazaba de vuelta: dos salidas, dos
entradas y la escritura y recarga de registros en cada vuelta. Ahora un
salto condicional interno cuyo destino es una instrucción del mismo bloque
(después de su delay slot) salta a su código sin salir
(`ijump_target`): su camino tomado se emite en la sección fría cuando el
emisor llega a la instrucción destino, porque ahí se sabe el estado que el
código asume (`emit_ijumps_to`): los registros de la FPU residente se
reponen en el formato que asume y lo pendiente de FCR31 que el código de
ahí no escribiría se escribe antes (o se deriva de memoria lo que sí
escribiría y en este camino no se calculó; `fpu_reconcile`), y `r29` se
ajusta en `(b + 2) − t` para que `cycles = r29 + k + 1` siga valiendo. Los
registros enteros no necesitan nada: el reparto es por bloque y las
escrituras a `cpu->gpr` van por el conjunto acumulado, que incluye las de
antes del salto. No se salta a un delay slot (su código vive dentro del de
su salto), ni a la segunda de un par LWL/LWR, ni con la escritura temprana
(saltaría sus almacenamientos diferidos), ni en bloques recortados.
`SALSA64_REC_DBG_NO_IJUMP` lo apaga.

### Registros

| PPE | Uso |
|---|---|
| r14–r25 | los 12 registros MIPS más usados del bloque (GPR, HI, LO) |
| r26 | destino de JR/JALR (sobrevive al delay slot); en un bloque sin JR/JALR, uno más para el bloque |
| r27 | `sched->next` (se recarga después de cada llamada a C) |
| r28 | `code_flags` |
| r29 | `cycles` al entrar al bloque |
| r30 | RDRAM |
| r31 | `struct salsa64_cpu` |
| r3–r12, r0 | temporales |
| f14–f25 | los 12 `cpu->fpr[p]` más usados del bloque («FPU residente») |
| f26–f31 | constantes: 1,0, 0,0 y los bordes de la franja segura en S y D |
| f0–f13 | temporales de la FPU |
| cr2, cr3.3 | FCR31 perezoso: C, causa I, bandera I, compuerta |
| cr4 | condición del salto (sobrevive al delay slot) |

Al entrar se cargan los registros que el bloque lee antes de escribir. Cada
registro modificado se escribe en `cpu->gpr` apenas recibe su último valor
del bloque (no todos juntos a la salida): así el bloque siguiente no lo lee
mientras el almacenamiento sigue en la cola (load-hit-store, decenas de
ciclos en el PPE). Los que no entran en los 12 van y vienen de `cpu->gpr`
en cada uso.

Opción `SALSA64_REC_OPT_PIN` (activa en el frontend; +7 % en nemu64-test
en la PS3): sp, ra, v0, v1 y a0–a3 viven en r18–r25 todo el tiempo que
corre código generado; la entrada los carga y la salida a C los escribe, y
antes de cada llamada al intérprete se escriben los que la instrucción
toca. Quedan 4 registros por bloque (r14–r17), poco para un bucle con
muchos temporales (bench enteros 750 → 128 MIPS): un bucle que necesita
más toma prestados los fijos que no usa (los guarda al entrar y los repone
en cada salida) y vuelve a tener hasta 12.

Opción `SALSA64_REC_OPT_EAGER` (apagada): escribir cada registro dos
instrucciones después de su último valor del bloque, contra el
load-hit-store del bloque siguiente. Medido en la PS3 no compensa (−2,6 %
en nemu64-test); escribirlo justo después fue peor (−37 % en un bucle de
enteros: el almacenamiento espera al valor y detiene el pipeline).

### Llamadas a C y salidas

Antes de cada llamada se dejan `cycles`, `cur_pc` y `delay_slot` como los
deja el intérprete; después se leen de una vez `irq_pending`, `redirect`,
`rec_abort` y `mode` (cuatro bytes contiguos) y se recarga `sched->next`.
Si algo cambió, el bloque escribe sus registros y vuelve al despachador con
la dirección donde sigue (en un delay slot, según el salto).

Lo que se llama a C:
- camino lento de cargas y almacenamientos (E/S, TLB sin anotar en `ktlb[]`,
  fuera de la RDRAM, código en la zona escrita);
- instrucciones raras o con mucho estado, que ejecuta el intérprete
  (`salsa64_vr4300_exec_one`): LL/SC, traps, COP0 salvo MFC0/DMFC0
  (y MFC0 de Random), MTC0 a EntryLo/PageMask/Index/EPC y a Status en su
  camino rápido (ver «MTC0 de Status y EPC en línea»), TLBWI/TLBWR y ERET
  (estos tres últimos con rutina propia, ver «COP0 sin el intérprete»),
  CVT.S/D.L, y los caminos lentos de lo que se traduce
  (operandos fuera de la franja, redondeo dirigido en la aritmética,
  causas habilitadas);
- excepciones (SYSCALL, BREAK, desbordamiento).

En nativo: ALU de 32 y 64 bits (con desbordamiento), desplazamientos,
MULT/DIV y variantes de 64 bits, HI/LO, saltos, cargas y almacenamientos de
enteros y de COP1, LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR, MFC0/DMFC0,
MFC1/MTC1/DMFC1/DMTC1, CFC1/CTC1, BC1T/F, en S y D ADD/SUB/MUL/DIV, SQRT,
MOV, ABS, NEG, C.cond, ROUND/TRUNC/CEIL/FLOOR a W y L, CVT.W/CVT.L,
CVT.D.S y CVT.S.D, CVT.S.W/CVT.D.W, y CACHE (sin cachés emuladas, en modo
kernel solo falta la dirección sin alinear a 4: esa la lanza el intérprete;
GoldenEye hace ~1000 por campo).

`rec->stats.exec_calls` cuenta las instrucciones que el código generado le
pasa al intérprete (salsa64-run, cpu-tests y las unitarias lo muestran).
`decode()` sigue marcando lo de `rec_emit_ext.h` como antes y
`decode_ext()` lo promueve a nativo; `SALSA64_REC_OPT_RARE_EXEC` se salta
la promoción y repite lo de antes del 2026-10-10 (conversiones, SQRT,
CFC1/CTC1, LWL/LWR y compañía al intérprete, MFC0 terminando el bloque),
para medir la diferencia con el mismo binario.

La FPU nativa repite el camino rápido del intérprete (`fpu.h`): las mismas
operaciones de doble precisión en el mismo orden (suma, redondeo a simple
con `frsp`, TwoSum o fma para el inexacto), así que da los mismos bits y
las mismas causas y banderas de FCR31. Lo que ese camino no cubre (redondeo
dirigido en ADD/SUB/MUL/DIV, I habilitada, operandos fuera de la franja
segura, NaN, desnormales, sumas en S con exponentes a más de 28) salta a la
sección fría y lo hace el intérprete. La prueba `executed_matches_full`
ejecuta 6000 instrucciones aleatorias por el CPU contra el camino completo.

Redondeo dirigido en las conversiones y MUL.S ancho (2026-10-16): el molde
de «(int)x» de los compiladores de N64 pone RM = 1 en FCR31 con `ctc1`, corre
`cvt.w.s` y restaura FCR31, así que `CVT.W`/`CVT.L` corrían con RM ≠ 0 y la
compuerta (`RM ≠ 0 o I habilitada → intérprete`) los mandaba todos al
intérprete (Banjo-Tooie: 17 M llamadas, 6,3 s en 176 s). Ahora `CVT.W` y
`CVT.L` solo se compuertan por I habilitada y siguen el RM vigente
(`emit_toint_core`, compartido por la rutina del preámbulo y el código en
línea): RM 0 con `fctid` (el FPSCR del host nunca sale del más cercano), RM 1
con `fctidz` y RM 2 y 3 con `fctidz` más el ajuste de ±1 de CEIL y FLOOR
cuando el entero vuelto a double difiere de x (que es también la bandera I).
Un RM cuesta un `rlwinm`, un `cmpwi` y un salto; el rango (|x| < 2³¹ en S,
< 2³⁰ en D, < 2⁵³ a 64 bits, o ±0) es el mismo de siempre y NaN, infinito y
desnormales siguen al intérprete, que da E con la excepción. MUL.S admite
cualquier normal (exponente sesgado 1–254) o ±0 si, con `s = ea + eb − 127`,
`1 ≤ s ≤ 252`: el producto de dos float es exacto en double, el exponente
del resultado queda entre `s` y `s + 2` (producto de mantisas ≥ 2, subida
por redondeo) y por tanto ni desborda ni queda diminuto; es un superconjunto
de la franja anterior (operandos en [2⁻⁶⁰, 2⁶¹)). Las demás operaciones
conservan la franja. Las pruebas `executed_rounding_edges_match_full` y
`executed_mul_edges_match_full` ejecutan los cuatro modos con valores de
borde y sumas de exponentes sesgados en 127/128 y 379/380 (los bordes de
`s`) contra el camino
completo. La FPU residente experimental (`rec_emit_fpu.h`) conserva su
compuerta anterior.

Conversiones y raíz (2026-10-10), con la misma regla: lo que el camino
nativo no puede dar con los mismos bits y causas de `cop1.c` va al
intérprete. Clasifican el operando con sus bits en enteros (exponente en
una franja, o ±0) y nunca pasan un valor entre registros enteros y de
punto flotante:
- SQRT: positivo normal o ±0, redondeo al más cercano, I deshabilitada.
  `fsqrt` (el PPE la tiene); I según `frsp` ≠ raíz (en S) o el residuo
  a − r·r de `fnmsub`, que es lo que hace `cop1.c` con fma.
- CVT.D.S: `lfs` + `stfd`, exacto y sin causa. CVT.S.D: 2⁻¹²⁶ ≤ |x| < 2¹²⁷
  (ni desborda ni queda diminuto), `frsp` e I si cambió.
- ROUND/TRUNC/CEIL/FLOOR a W y L, y CVT.W/CVT.L con redondeo al más
  cercano: solo exponentes con los que ningún modo se sale del rango (W:
  |x| < 2³¹ en S, < 2³⁰ en D; L: < 2⁵³). `fctid` (el FPSCR siempre está al
  más cercano par, como `fpu_round`) o `fctidz`; CEIL y FLOOR corrigen el
  truncado en ±1 (exacto); I si el entero vuelto a double (`fcfid`) difiere.
  El resultado va de `stfd` a `cpu->fpr`, que es su destino.
- CVT.S.W y CVT.D.W en enteros: el entero está en `cpu->fpr` y llevarlo a
  un registro de punto flotante pide escribirlo y releerlo (load-hit-store;
  ppu-gcc hace eso con `(double)x`). Se normaliza |x| con `cntlzw` y un
  `slw` (microcódigo, uno solo) y, en S, se redondea a 24 bits al más
  cercano par sumando 0x7F más el bit que queda abajo.
- CFC1 lee FCR31 (o FCR0); CTC1 escribe los bits escribibles y, si una
  causa queda con su excepción habilitada (E siempre), deja la instrucción
  al intérprete, que la lanza.

La prueba `executed_conversions_match_full` (8000 instrucciones: SQRT, las
conversiones con enteros con fracción, empates, bordes de 2³¹ y 2⁵³ y de la
franja de CVT.S.D, CFC1/CTC1) compara el CPU con el camino completo y, con
el recompilador, exige que más de 3000 hayan corrido sin el intérprete.

### Lo que el PPE hace en microcódigo (2026-10-11)

Del manual del Cell (tabla A-1, A.1.3): una instrucción microcodificada
vacía el pipeline y tarda al menos 11 ciclos. Lo son **todas las formas con
punto** (`andi.`, `andis.`, `and.`, `rlwinm.`, `add.`…), los desplazamientos
y rotaciones por registro (`slw`, `srw`, `sld`, `srd`, `sraw`, `srad`,
`rldcl`, `rlwnm`), `lwa`/`lha`, `mtcrf` de más de un campo (`mtocrf` de uno
cuesta un ciclo), `mcrfs` y `mtfsf*`, `lmw`/`stmw`. `mfcr` cuesta ~34 ciclos
y `mffs` 11–28. Lo demás: `fcmpu` deja su bit del CR al ciclo siguiente;
`fadd`/`fmul`/`frsp`/`fsel`/`fabs`/`fctid`/`fcfid` 10 ciclos; `fdiv` 56–74 y
`fsqrt` 66–84 (sin pipeline); enteros 2 ciclos; `mulli` 6, `mulld` 15 (y
detienen el pipeline); cargas 2.

El emisor usaba `andi.` en cada ANDI de MIPS (muy frecuente) y en la
compuerta de cada operación de la FPU, `andis.` a la entrada de cada bloque
con COP1 y `mtcrf 0x38` en cada salida a C. Ahora ANDI es `rlwinm` (máscara
de unos seguidos) o `li`+`ori`+`and`; las pruebas de bits son
`rlwinm`+`cmpwi`; la salida usa tres `mtocrf`. Quedan por registro los
desplazamientos de SLLV y compañía y de LWL/LWR sueltos (no hay otra forma;
los pares LWL/LWR van byte a byte).

### FPU residente (2026-10-11; experimental, apagada por omisión)

**Medida en la PS3 es más lenta que la FPU por memoria** (bench del mismo
paquete: add.s 40,0 → 30,9 MIPS, mixto 168 → 135; en GoldenEye no se
distingue, la FPU es ~6 % de sus instrucciones): cada `fcmpu` que alimenta
un salto o un crop espera toda la latencia de la FPU, y `fabs` agrega 10
ciclos a la clasificación, mientras el camino por memoria clasifica los
bits en enteros (2 ciclos) y solo espera a la FPU en la comparación del
inexacto. Queda detrás de `SALSA64_REC_OPT_FPU_RES` (`SALSA64_REC_FPU_RES=1`
en el host, el archivo `jit-fpu-residente` en la PS3) para retomarla; la
idea que queda por probar es clasificar sin saltos intermedios (una sola
comprobación al final de la operación, con la deshecha por el intérprete) y
sin `fabs`. Lo que sigue describe el diseño tal como está.

Cada operación de la FPU carga sus operandos de `cpu->fpr`
(los bits a un registro entero para clasificarlos y el valor a uno de punto
flotante), escribe el resultado y lee, modifica y guarda FCR31: unas
40 palabras por ADD.S, con load-hit-store entre
operaciones dependientes. Con la opción (`rec_emit_fpu.h`):

- **Registros residentes.** Los 12 `cpu->fpr[p]` más usados del bloque
  viven en f14–f25 (no volátiles: sobreviven a las llamadas a C; la entrada
  guarda y repone f14–f31). Un registro del PPE tiene el valor en un
  formato: S (la mitad baja como double, lo que da `lfs`) o D (los 64 bits);
  un uso en otro formato lo recarga de casa. Los vivos a la entrada se cargan
  en el formato de su primer uso; al volver al cuerpo (bucle) se reponen los
  que cambiaron de formato.
- **Casa siempre al día.** Cada resultado se escribe en `cpu->fpr` en cuanto
  está (el almacenamiento espera por el valor, pero la comparación ya esperó
  por él): ninguna salida ni llamada a C escribe registros de la FPU y el
  intérprete (camino lento) siempre lee lo correcto. LWC1/LDC1 cargan el
  registro residente directo de la RDRAM (`lfsx`/`lfdx`) además de escribir
  casa; SWC1/SDC1 de un valor residente almacenan con `stfsx`/`stfdx`.
- **Clasificación en punto flotante.** Mover bits entre la FPU y los enteros
  pasa por memoria (load-hit-store), y `fcmpu` da su bit al ciclo siguiente:
  la franja segura de `fpu_fast_band` (|x| en [2⁻⁶⁰, 2⁶¹) en S, [2⁻⁴⁴⁰,
  2⁴⁴¹) en D, o cero) se prueba con `fabs` y tres `fcmpu` contra constantes
  que la entrada deja en f27–f31 (1,0 en f26 para CEIL/FLOOR); NaN e
  infinito fallan todas las comparaciones. Las comparaciones de los operandos
  se emiten antes de la operación y corren en paralelo con ella; un solo
  salto al camino lento junta todo con crops.
- **Inexacto sin TwoSum.** La suma en double `r = a + b` es exacta si y solo
  si `r − a == b` y `r − b == a` (dos restas independientes y dos
  comparaciones, en vez de la cadena de cuatro operaciones dependientes del
  TwoSum, ~40 ciclos): si la suma es inexacta, el error ε = (a + b) − r es
  un múltiplo no nulo del ulp del operando con ulp menor, y al restarle el
  otro operando a r queda ese operando menos ε, que no redondea de vuelta a
  él; si es exacta, las dos restas son exactas. En S además `frsp` ≠ r
  (reemplaza la prueba de exponentes); producto, cociente y raíz por el
  residuo con fma como en `cop1.c`.
- **FCR31 perezoso.** C, la causa I de la última operación, la bandera I
  acumulada y «RM ≠ 0 o I habilitada» viven en cr2 (no volátil; cr3.3
  guarda «I habilitada» para ROUND/TRUNC/CEIL/FLOOR): una operación cuesta
  uno o dos crops, BC1 un `bc` sobre el bit y C.cond un `fcmpu` y un crop.
  Se escriben en `cpu->fcr31` al salir del bloque (con dos o tres saltos,
  una vez por bloque), antes de que el intérprete corra una instrucción de
  COP1 y en CFC1; después de una instrucción de COP1 por el intérprete se
  vuelven a derivar de la memoria, y si dejó una causa que no es I (Z, V, O,
  U sin excepción) el bloque sale, porque el estado perezoso no la
  representa.
- **CVT.S.W y CVT.D.W con `fcfid`.** MTC1 y LWC1 dejan además la palabra con
  signo extendido en `cpu->rec_fw[p]` cuando una conversión la va a leer, y
  la conversión la carga con `lfd` (un load-hit-store en vez de dos y sin el
  `slw` microcodificado de la normalización en enteros).

La prueba `executed_sequences_match_interp` (3000 secuencias de 2 a 8
instrucciones de COP1 con cargas, almacenamientos, MTC1/MFC1, CFC1/CTC1 y
operandos al azar, en un solo bloque) compara el resultado entero
(registros, FCR31, excepciones, memoria) con el intérprete sobre el mismo
estado inicial, y exige que más de 600 corran enteras en nativo.

### COP0 sin el intérprete: MTC0 EntryLo, TLBWR y ERET (2026-10-11)

El refill de la TLB de GoldenEye (~400 por campo) son cuatro instrucciones
de COP0 (dos MTC0 a EntryLo, TLBWR y ERET), y cada una costaba una llamada
a `salsa64_vr4300_exec_one` más, tras cada COP0, `salsa64_rec_x_sync`
(comparar las 32 entradas de la TLB con la instantánea) y, en el ERET, la
vuelta al despachador por `redirect` (buscar el bloque, entrar de nuevo):
2200 llamadas y ~1,9 ms por campo en la PS3. Ahora
(`SALSA64_REC_OPT_COP0_EXEC` y el archivo `jit-cop0-viejo` repiten lo de
antes para medir):

- **MTC0 y DMTC0 a EntryLo0, EntryLo1, PageMask e Index** son nativos y no
  terminan el bloque: el latch (`cop0_latch`) y el campo con su máscara
  (`salsa64_cpu_cop0_write`). No cambian la traducción hasta la escritura
  de la TLB. Cause, Compare, Count, EntryHi (cambia el ASID) y lo demás
  siguen por el intérprete y terminan el bloque (EPC y Status: ver «MTC0 de
  Status y EPC en línea», 2026-10-16).
- **TLBWI/TLBWR** llaman a `rec_h_tlbw` (rec.c): la escritura
  (`salsa64_vr4300_tlbw`, que devuelve la entrada) y `salsa64_rec_x_tlb_written`,
  que pone `ktlb[]` al día solo para esa entrada (borrar sus páginas de antes
  y de ahora, instantánea y prellenado) sin recorrer las 32 ni compactar
  `live[]` (`vt_set` compacta si se llena). No hay excepción, interrupción
  ni cambio de modo posibles, así que no se revisa nada al volver: el bloque
  termina enlazado al siguiente, cuya entrada revisa su página.
- **ERET** llama a `rec_h_eret`: lo que hace el intérprete (`op_eret`) sin
  dejar `redirect`, revisa la ktlb si cambió ERL y devuelve el pc; el código
  mira la palabra de interrupción/modo y, si sigue en kernel sin
  interrupción, va al bloque destino por `exit_jr` como un JR; si no, al
  despachador con ese pc (`exit_mid`).

### MTC0 de Status y EPC en línea (2026-10-16)

`__osDisableInt` y `__osRestoreInt` de libultra (`mfc0 Status ; and/or ;
mtc0 Status`) y `__osDispatchThread` (MTC0 de Status y de EPC en cada cambio
de hilo) eran casi todo lo que llegaba a C por COP0: en la PS3, Doom 64 75 M
llamadas y 13,9 s en 125 s, NFL Blitz 19,5 s en 99 s, Episode I Racer 136 M
llamadas y 28 s en 296 s, ~185 ns cada una.

- **MTC0/DMTC0 a EPC** se suma a EntryLo, PageMask e Index: el valor al
  latch y al campo, sin terminar el bloque.
- **MTC0 a Status** (`emit_mtc0_status`, `H_MTC0_ST`) hace en línea lo de
  `salsa64_cpu_cop0_write` cuando, con KSU = 0, el valor nuevo solo cambia
  IE, EXL o IM: modo, `allow64` y `re_xor` siguen iguales (kernel) y la TLB
  del recompilador también (ERL igual). Queda la interrupción: si con el
  Status nuevo hay una habilitada y pendiente (IE, sin EXL ni ERL, IP & IM),
  o si cambia otra cosa (FR, CU1, ERL, KSU, BEV…), va al intérprete en la
  sección fría con la revisión de siempre. Son 22 palabras sin formas con
  punto (`rlwinm` + `cmpwi`). El bloque termina después del MTC0 como antes
  y enlaza con el siguiente, así un cambio de FR o de CU1 lo nota la entrada
  del bloque siguiente.

`SALSA64_REC_OPT_COP0_EXEC` repite lo de antes. Las pruebas
`status_write_enables_pending_irq`, `status_write_ie_without_irq` y
`status_write_cu1_off_then_cop1` (test_cop0.c) corren con el intérprete y
con el recompilador (`salsa64-unit rec`).

### CU1 = 0: bloques cortados antes del COP1 (2026-10-16)

La entrada de un bloque con alguna instrucción de COP1 revisa Status.CU1 y,
con CU1 = 0, sale al despachador, que interpreta hasta el fin del bloque
básico (la excepción de coprocesador inutilizable la lanza el intérprete).
Eso pasaba aunque el COP1 estuviera en un camino que no se toma: el cambio
de hilo de libultra guarda y repone la FPU (`cfc1`, `sdc1`, `ctc1`, `ldc1`)
solo si el hilo la usa, y los hilos que no la usan (el ocioso, entre otros)
corren con CU1 = 0. En Doom 64 eran ~14 000 salidas en la entrada y ~160 000
instrucciones interpretadas por campo (el 30 % del muestreo en el intérprete
y buena parte del 14 % del despachador); en Indiana Jones, el 51 % en el
intérprete.

Ahora, si al compilar Status.CU1 = 0, el bloque termina antes de su primera
instrucción de COP1 (o antes del salto cuyo delay slot la tiene), salvo que
sea la primera: así corre en nativo con los dos valores de CU1 y, con
CU1 = 1, sigue enlazado al bloque que empieza en el COP1. El bloque queda
marcado `SALSA64_REC_CU1_OFF`. Si uno compilado con CU1 = 1 sale en la
entrada con CU1 = 0 cabiendo antes del evento (o sea, por CU1), el
despachador lo mata (con su gemelo de FR) y lo recompila cortado
(`cu1_recut`, `stats.cu1_cuts`); uno ya cortado empieza con COP1 y el
intérprete lanza la excepción como siempre. `SALSA64_REC_DBG_NO_CU1_CUT`
repite lo de antes. En Doom (qemu, 600 campos) las instrucciones
interpretadas por campo bajan de 163 228 a 78.

### Cargas y almacenamientos no alineados

LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR sueltos: en la RDRAM desde KSEG0/KSEG1 (y,
al escribir, sin código en esos 128 bytes), el desplazamiento sale de la
dirección y la mezcla usa desplazamientos por registro (microcódigo en el
PPE, pero mucho menos que una llamada al intérprete); los almacenamientos
leen la palabra, la mezclan y la escriben. Un LWL y un LWR (o SWL y SWR)
consecutivos, en cualquier orden, sobre los mismos 4 bytes (`off` y
`off + 3`, misma base y mismo rt; en las cargas, la base no es rt) son un
acceso de 4 bytes sin alinear, como los escribe el compilador: se hacen
byte a byte (4 `lbz` y 3 `rlwimi`, o 4 `stb`), sin desplazamientos por
registro y sin leer la palabra antes de escribirla (en una copia, el par
siguiente escribe la palabra que el anterior acaba de escribir:
load-hit-store). Fuera de la RDRAM, con TLB o con código, el intérprete
corre las dos.

### MFC0

MFC0 y DMFC0 ya no terminan el bloque: en modo kernel (el único en que
corre un bloque) no hay excepción posible, y cada registro es un campo de
`struct salsa64_cpu` (tabla en `rec_emit.c` con lo mismo que
`salsa64_cpu_cop0_read`). Count es `(cycles >> 1) + count_offset` con
`cycles = r29 + k + 1`, lo que el intérprete tiene al ejecutar la
instrucción k. Random lo corre el intérprete (sin terminar el bloque).

### Tamaño del código generado (2026-10-11)

Un bloque de GoldenEye con 13 instrucciones, dos saltos internos y dos
cargas daba 180 palabras de PPC (volcado con `SALSA64_REC_DUMP=1` en
`salsa64-run`; `tools/rec-disasm.sh` lo desensambla): media 316 palabras
por bloque, mediana 171, p90 789. Cada carga o almacenamiento costaba ~47
palabras frías: el camino lento (`call_prep`, argumentos, llamada, revisión
de `redirect` y `post_check`) y dos salidas pendientes (antes y después)
que repetían la escritura de los registros modificados. Con superbloques y
la caché de 16 MB, GoldenEye la llenaba cada 7,5 s (17 vaciados en 128 s).
Ahora:
- El camino lento de cargas y almacenamientos son 11 rutinas compartidas
  del preámbulo (`slow_load[7]`, `slow_store[4]`; desde el 2026-10-13 una
  sola, con trampolín por bloque: «Código compacto»): el sitio escribe sus
  registros sucios (solo en ese camino, el rápido no cambia), pasa k, el
  delay slot y n en r6 y el pc en r7, y la rutina deja `cycles`, `cur_pc` y
  `delay_slot` como el intérprete, llama a C (guardando LR en
  `cpu->rec_lr`), recarga `sched->next` y vuelve con cr1 ne si hubo
  excepción o cr5 gt si hay que salir. Como lo sucio ya está en memoria,
  las salidas del sitio solo ajustan `cycles` (y el valor cargado va a su
  lugar antes de salir).
- La traducción de datos por `ktlb[]` también es compartida
  (`ktlb_rtn[2][4]`, por alineación y lectura/escritura).
- El preámbulo pasa de 256 a 1024 palabras.

Resultado (mismo volcado de GoldenEye, 4392 bloques): media 316 → 248
palabras por bloque, mediana 171 → 137, p90 789 → 621. Lo que queda grande
son las operaciones de la FPU (~55 palabras cada una, con su camino lento
al intérprete) y las salidas de los saltos internos. Con 24 MB, GoldenEye
vacía la caché 2 veces en 128 s (al cargar la presa); cada vaciado
recompila el conjunto vivo (~7000 bloques) a medida que se ejecuta.

### Código compacto: palabras por instrucción MIPS (2026-10-13)

Medición del 2026-10-12 en la PS3 (docs/perf-log.md, «Perfil persistente y
zona caliente»): juntar los bloques calientes en una zona contigua no cambia
el costo del nativo; lo que manda es cuántas líneas de caché toca cada
instrucción emulada. El volcado de GoldenEye hasta la presa
(`SALSA64_REC_DUMP=1` con el perfil por bloque, `tools/rec-expansion.py`
pesa cada bloque por sus ejecuciones) daba **20,9 palabras de PPC por
instrucción MIPS: 9,9 en la sección caliente y 11,0 en la fría**; por
mnemónico, lo que más pesaba en la caliente era la prueba de KSEG0 de cada
acceso a memoria (`sradi`, `ori`, `addi`, `clrldi`, `or`, `cmpdi`, `bne`,
`clrldi`: 8 palabras más la carga y su `extsw`) y en la fría las constantes
y saltos de los caminos lentos (`lis`/`ori` 30 %, `b`/`bl`/`bne` 31 %) y la
escritura de los registros sucios antes de cada llamada a C (`std` 10 %).

### Estado actual: Banjo-Tooie (sesión de perfilado de la regresión)

El resumen de la sesión de Banjo-Tooie informa **12,9 palabras de PPC por
instrucción MIPS en total y 5,8 calientes**, calculadas sobre bloques vivos
sin ponderar por ejecución. La sesión registró **297 141 compilaciones** y
**29 088 retraducciones (9,8 %)**. Corresponde a v0.2.5-91-gac22527, escala
5× y filtro Ninguno; no es una medición de la rc1.

Tres cambios, con `SALSA64_REC_OPT_WIDE` (`jit-ancho` en la PS3,
`SALSA64_REC_WIDE=1` en el host) para medir contra el código de antes con
el mismo binario:

- **Prueba de KSEG0 en 4 palabras** (`emit_addr`). Con el bit 29 puesto
  (`oris 0x2000`) KSEG0 y KSEG1 son la misma dirección, y sumarle 2³⁰ + 2²⁹
  (`addis 0x6000`) a 0xFFFFFFFF_A0000000 da exactamente 2⁶⁴: de una
  dirección de KSEG0 o KSEG1 dentro de la RDRAM quedan solo los bits del
  desplazamiento, ya en r12 para el acceso. Un `rldicl` rota al tope los
  bits del desplazamiento por encima de la alineación y los recorta: queda 0
  si la dirección vale (segmento, tamaño de la RDRAM y alineación de una
  vez), y `cmpdi` + `bne` van al camino de la ktlb. Son 4 palabras y ningún
  registro extra, contra 8 antes (y la rutina de la ktlb sigue devolviendo
  el desplazamiento en r12).
- **Un camino lento por bloque, no por acceso.** Una sola rutina del
  preámbulo (`slow_rtn`; antes `slow_load[7]` y `slow_store[4]`, y las
  llamadas al intérprete y a las excepciones iban en línea) recibe `r6 =
  (k + 1) | delay slot << 8 | tipo << 9 | qué << 12 | n << 16` y `r7 = pc
  del bloque`, deriva `cur_pc = pc + 4k`, deja cycles, cur_pc y delay_slot
  como el intérprete y despacha por «qué»: carga (`r3 = va`),
  almacenamiento (`r5 = valor`), una instrucción al intérprete (`r4 = la
  instrucción`) o una excepción (`r4 = el código`). Cada sitio pasa solo
  `li r6` (una palabra; y `lis`/`ori` de la instrucción, o `li` del
  código) y hace `bl` al **trampolín del bloque**, que agrega lo que es del
  bloque entero (`lis`/`ori` del pc, `oris` de n) y sigue a la rutina: 4
  palabras una vez por bloque en vez de 5–8 por sitio. Al volver, `cr5 gt`
  dice si hay que salir (también por excepción en el caso del intérprete);
  las cargas y los almacenamientos mantienen `cr1 ne` para la excepción.
  El `call_prep` + llamada + revisión en línea (~19 palabras) de cada
  instrucción al intérprete, cada camino lento de la FPU y cada
  desbordamiento de ADD/ADDI pasa a 4–6. El tramo frío que prueba la ktlb
  de un acceso va pegado a su camino lento (`emit_addr_cold`: si traduce
  vuelve al rápido, si no sigue de largo), y con desplazamiento 0 la
  dirección se toma del registro base sin copiarla (`addi` de menos en el
  camino caliente; los caminos fríos la copian a r3).
- **Cadena de escritura** (`chain_build`, `emit_chain`). Una salida a C por
  el camino lento tiene que dejar en `cpu->gpr` los registros del bloque que
  ya tienen valor; antes cada sitio escribía los suyos (`std` por registro
  sucio, repetido en cada acceso). Como cada registro del PPE tiene el
  valor correcto desde la primera instrucción que lo escribe (y antes, si se
  cargó a la entrada, el mismo que la memoria), los que hay que escribir al
  salir en k son los de primera escritura ≤ k (< k si k no terminó):
  ordenados por primera escritura descendente son un sufijo de una sola
  secuencia de `std` terminada en `blr`, y cada salida entra con `bl` en el
  último que ya tiene valor. Un `std` por registro por bloque. Las salidas
  por excepción ya no cargan r3 (el despachador sigue por `redirect`). Los
  caminos tomados de los saltos internos y las salidas finales siguen
  escribiendo en línea (son calientes; `bl`/`blr` les costaría ciclos).
- **Enlaces de 3 palabras** (en los dos modos): el destino va como dato
  después del `bl exit_link`, que lo lee de LR («Enlaces»).

Resultado en el host (mismo volcado de GoldenEye, pesado por ejecuciones;
las cuentas de abajo descuentan las 8 palabras por bloque del contador del
perfil, que la PS3 no lleva): ver docs/perf-log.md, «Código compacto».

#### Ideas anotadas para después (2026-10-13; de docs/cache-ppe.md y del autor)

Por evaluar con el muestreo por tiempo y el volcado, en este orden de
costo/beneficio; ninguna está hecha:

- **Cambiar datos por trabajo** (sobra cálculo y falta caché; el camino
  lento compartido ya es eso): (a) no traducir lo tibio, bloques con pocas
  ejecuciones en un intérprete compacto o código enhebrado, para que el
  código traducido vivo sea menor (docs/cache-ppe.md 3.8); (b) estructuras
  de la ktlb más chicas aunque cuesten un cálculo más, si las actuales
  desbordan la caché de datos (3.6); (c) secuencias repetidas en lo
  caliente a subrutinas compartidas con un salto de costo aceptable (la
  prueba de KSEG0 de 4 palabras y la carga ya están cerca del mínimo; las
  candidatas son la entrada de bloque y la FPU en línea).
- **Precarga del sucesor probable**: con los sucesores del perfil
  (rec_profile.h), emitir al entrar a un bloque caliente una precarga del
  código del sucesor (`icbt`, o lo que el PPE ofrezca sin microcódigo) y de
  la línea de datos o de la ktlb que va a tocar (`dcbt`); medir con el
  muestreo si baja el tiempo por instrucción sin subir el tamaño caliente
  (3.5).
- **Bench de huella** (docs/cache-ppe.md, sección 5): cadenas sintéticas de
  bloques que ocupen 16 KB … 16 MB de PPC, para dibujar la curva L1 → ERAT
  → L2 → XDR en la PS3 y saber en qué régimen está cada juego; junto con el
  A/B de `jit-ancho`.
- Descartado: páginas de 1 MB para la caché de código (el ERAT cubre 4 KB
  por entrada aunque la página sea grande).
- Lo que dijo el primer muestreo en la PS3 (docs/perf-log.md, «Muestreo
  por tiempo en la PS3»): el tiempo está repartido plano entre cientos de
  bloques (ninguno pasa del 3 %), los que más pesan son grandes con mucha
  FPU en línea (~20 palabras calientes por instrucción), el refill de la
  TLB se lleva ~2 % y el intérprete 3 %. Candidatos en ese orden: la FPU
  (~40 palabras por operación: compartir la clasificación y FCR31 en una
  rutina, o volver a la residente sin sus saltos), el refill (TLBWR y ERET
  por la rutina compartida) y los bloques que no caben antes del evento.

### Muestreo por tiempo desde el otro hilo (2026-10-13)

Código: `src/cpu/rec_sample.c` y `rec_sample.h`; el frontend lo enciende con
el archivo `/dev_hdd0/salsa64/muestreo` y en el host `SALSA64_REC_SAMPLE=1`
(salsa64-run, con un hilo propio). Por qué: el perfil persistente cuenta
entradas a bloques, y eso engaña (los más entrados son los de 2–4
instrucciones); lo que cuesta es tiempo, y sin el PMU del Cell la forma de
medirlo es muestrear.

- El hilo que emula deja dos bytes en `struct salsa64_cpu`: `rec_cur`, el id
  del bloque que corre (lo escribe la entrada de cada bloque, `li` + `sth`,
  dos palabras que solo se emiten con el muestreo puesto al compilar; los
  bucles no vuelven a pasar por ahí), y `rec_mark`, 0 en código generado o
  la categoría de lo que hace fuera de él: la rutina compartida del camino
  lento al llamar a C, el despachador, el intérprete, los eventos, compilar,
  los bucles de espera, el ritmo (duerme) y lo demás del hilo. Los marca el
  preámbulo (entrada, salida común, `slow_rtn`) y el despachador en C.
- Un hilo de prioridad 1100 (por debajo de emulación, 1000, y presentación,
  1001: corre en el hilo de hardware que esté libre, el de presentación
  duerme el 85 %) lee los dos bytes cada 200 µs y suma una muestra al
  bloque (`samples[id]`) o a la categoría. Al morir un bloque, o al informar,
  sus muestras pasan a una tabla por dirección virtual (16 384 entradas,
  índice abierto): un bloque recompilado varias veces suma todo bajo su
  dirección, con las palabras de PPC (y las calientes) del último compilado
  ahí. Costo para el hilo que emula: ~0,13 ms por ms de trabajo del otro
  hilo, y una muestra son decenas de ciclos (5000 por segundo); las dos
  palabras por entrada a bloque son el único costo en el camino caliente.
- Al log: cada ventana de 5 s el reparto por categoría; al cerrar el juego,
  el total (muestras, % en código generado, palabras por instrucción pesadas
  por muestras) y los 30 bloques con más muestras: dirección, muestras, % del
  tiempo y del nativo, instrucciones MIPS, palabras de PPC (calientes). Con
  el perfil de entradas de la PS3 (`perfiles/*.prf`) se compara por
  dirección: un bloque con más tiempo del que dicen sus entradas × palabras
  espera memoria (fallos de caché o de ERAT).
- Lo que no puede dar: en qué parte del cuerpo de un bloque está el hilo
  (desde usuario no se lee el pc nativo del otro hilo). Sí distingue el
  cuerpo del bloque, los caminos lentos en C y el resto del emulador.
- Carreras: el hilo que muestrea incrementa contadores que el que emula lee
  y pone a cero al plegar; se puede perder una muestra en la ventana. Es
  estadística. La prueba `t_suite_rec_sample` (tests/unit/test_rec_sample.c)
  cubre la clasificación, el plegado por dirección con ids reutilizados y
  el informe.

### Memoria

Camino rápido: `va >> log2(rdram)` vale lo mismo para KSEG0 y KSEG1 tras un
`ori`; junto con los bits de alineación da una sola comparación. Los
almacenamientos además miran `code_flags[pa >> 7]`: si hay código
recompilado en esos 128 bytes, van por C, que invalida. Si la dirección no
es de KSEG0/KSEG1, en la sección fría se prueba `ktlb[]` (ver «Datos
mapeados por la TLB») y, si traduce a la RDRAM, se vuelve al camino rápido
con el desplazamiento ya calculado; lo demás va a C.

### Bucles de espera

El intérprete salta vueltas enteras de los bucles de espera (`idle.c`,
consultado en cada salto corto hacia atrás). El código generado hace la
misma consulta: un salto que el intérprete consultaría (condicional, de
enlace, BC1 o J, tomado hacia atrás a menos de 64 instrucciones; 8 hasta el
2026-10-16) y que no esté en la tabla de descartados sale a C en el camino
tomado, antes del
delay slot y con el mismo estado (registros en memoria, cycles con el salto
contado). El despachador llama a `salsa64_vr4300_idle` igual que el
intérprete; si la tabla lo descarta, invalida el bloque y se recompila sin
la consulta. Así los ciclos saltados y las tablas quedan idénticos a los del
intérprete (las pruebas de `test_idle.c` pasan en los dos), y un bucle de
espera de verdad cuesta una salida a C por evento. Cambiar la detección
(`salsa64_cpu_set_idle`) vacía la caché.

**Esperas que pasan por el planificador de libultra (2026-10-16).** En la
PS3, Doom 64 corría al 35 % con 0 % en bucles de espera: su hilo ocioso es
`for (;;) osYieldThread();` (0x800056f0), y cada vuelta pasa por
`__osDisableInt`, `__osEnqueueAndYield` (guarda el contexto, lee
MI_INTR_MASK), `__osDispatchThread` (MTC0 Status, MTC0 EPC, escribe
MI_INTR_MASK, ERET) y `__osRestoreInt`: ~140 instrucciones con cuatro MTC0,
un ERET y dos accesos al MI, ~7 000 vueltas por campo, el 84 % de los ciclos
emulados. Las 6 PCs del camino lento de la sesión son esas (0x80041ba0 lee
la máscara, 0x80041c90 y 0x80041d1c son los MTC0 de Status y EPC,
0x80041da8 escribe la máscara, 0x8004294c y 0x80042968 son los MTC0 de
`__osDisableInt` y `__osRestoreInt`). Lo mismo, con otras formas: 1080
(`while (x->y == s0) osYieldThread();` en un hilo que usa la FPU), NFL
Blitz (cinco `osRecvMesg(..., OS_MESG_NOBLOCK)` y `osYieldThread`, con un J
de 37 instrucciones hacia atrás), Indiana Jones (en código mapeado por la
TLB) y Episode I Racer.

`idle.c` simula ahora la vuelta entera siguiendo saltos, llamadas, regresos
y ERET (hasta 1024 instrucciones) sobre copias: almacenamientos en la RDRAM
en una tabla que leen las cargas de la vuelta (al final cada byte escrito
tiene que valer lo que valía), MFC0/MTC0 de Status (solo IE, EXL, ERL e IM,
sin salir de kernel) y EPC, MFC0 de Cause, MI_INTR_MASK leído y escrito y
MI_MODE/MI_VERSION/MI_INTR leídos, MULT/DIV y HI/LO, y lo de la FPU de un
cambio de contexto (CFC1/CTC1 de FCR31, LWC1/LDC1/SWC1/SDC1). Si en algún
punto el CPU tomaría una interrupción, no es de espera. El punto fijo se
exige en los 32 registros, HI, LO, Status, EPC, el latch de COP0, LLbit, la
máscara del MI, FCR31 y los registros de la FPU. La vuelta dura lo que contó
la simulación (un ciclo por instrucción ejecutada; un delay slot anulado no
cuenta), así que el salto de tiempo es el de siempre: vueltas enteras hasta
el próximo evento.

Lo que no se simula descarta el salto para siempre si está en todos los
caminos; si aparece después de un salto condicional (llegó un mensaje y el
hilo lo atiende: Blitz lee SI_STATUS) es «esta vez no». Un salto que ya fue
de espera aguanta 64 fallos seguidos antes de descartarse (los demás, 2 como
antes). Las tablas son de 1024 entradas con un hash multiplicativo del pc
(`SALSA64_IDLE_SLOT`; con los bits 2–11 solos, dos saltos de Blitz se
pisaban el descarte en cada vuelta).

Comprobado en el host con los 29 respaldos: 1800 campos con y sin
`--no-idle` dan la misma RDRAM y los mismos registros. En % de bucles de
espera (host, intérprete, 1800 campos): Doom 0 → 83 %, NFL Blitz 0 → 91 %,
1080 0 → 86 %, Indiana Jones 1 → 86 %, Episode I Racer 21 → 37 %. Pruebas
nuevas en test_idle.c: el `osYieldThread` en miniatura (con y sin FPU), el
bucle de 40 instrucciones, el almacenamiento silencioso y la espera que tras
cada interrupción pasa una vez por algo que no se simula.

### Invalidación

`code_flags` cuenta cuántos bloques tocan cada 128 bytes. Lo revisan los
almacenamientos nativos, los del intérprete (`vr4300_code_check`) y las
escrituras externas (`salsa64_bus_rdram_written`: DMA del PI y del SI,
cargador, pruebas). Un bloque invalidado se saca del mapa, se deshacen los
enlaces que llegan a él (el `b` vuelve a ser `nop`) y, si fue durante la
ejecución, el código sale al despachador (`rec_abort`). La caché se vacía
entera con un reset, al cambiar la ROM o cargar un estado, y cuando se
agotan los ids de bloque o los enlaces (`stats.flushes_by[]` cuenta por
causa; salsa64-run y el frontend lo imprimen). Hasta el 2026-10-12 también
cada vez que cambiaba Status.FR (ver «Status.FR»).

### Status.FR: gemelos en vez de vaciar (2026-10-12)

Donkey Kong 64 corre hilos con Status.FR = 1 y otros con FR = 0, y cada
cambio de hilo (~260 por segundo de consola) cambia el bit al reponer SR.
Como la caché se compilaba para un FR y se vaciaba entera al cambiar, en la
PS3 el juego iba al 20 % de NTSC compilando 1 340 bloques por campo (4 173
vaciados y 1,28 M bloques en 16 s de consola en qemu; medido en la PS3:
959 vaciados y 446 000 bloques en 30 s). Ahora:

- El código de un bloque depende de FR solo si accede a un **registro impar
  de la FPU** por fs (MFC1, MTC1, DMFC1, DMTC1 y las operaciones) o por ft
  (LWC1, LDC1, SWC1, SDC1): con FR = 0 ese registro es la mitad alta de la
  pareja (`fpr32_off`, `fpr64_off`, `fslot32`, `fslot64`). `fr_dependent`
  lo decide por instrucción al decodificar; los demás bloques, también los
  de COP1 con registros pares, valen con los dos FR.
- Un bloque que depende de FR se compila con el FR del momento
  (`rec->fr`, `blocks[id].fr` lo guarda con `SALSA64_REC_FR_SET` y
  `SALSA64_REC_FR_DEP`) y su entrada revisa CU1 y FR de una vez (`lwz
  status ; rlwinm 6,28,31 ; rlwinm 0,31,28 ; cmpwi 8|fr ; bne`): una
  instrucción más que la comprobación de CU1 que ya tenía. Si no coincide
  sale por `exit_fr` (`SALSA64_REC_EXIT_FR`) y el despachador busca el
  **gemelo** para el otro FR (`block_for_fr`): en KSEG0 `rec->twin[id]`, y
  el mapa pasa a apuntar al último usado, así `exit_jr` (que no mira FR)
  acierta mientras el FR no cambie; fuera de KSEG0 el gemelo es otro bloque
  de la misma clave en `xhash`, que `salsa64_rec_x_find` y `x_rekey` filtran
  por FR. Si no existe, se compila (`stats.fr_twins`). Con CU1 = 0 la misma
  salida hace lo de siempre: el intérprete lanza la excepción.
- Los gemelos cubren los mismos bytes: `kill_block` deja el mapa apuntando
  al que queda y `salsa64_rec_invalidate` mata a los dos; un desalojo por
  tramos puede llevarse uno solo. Los ids de los gemelos son ids normales.
- `rec_h_exec` ya no aborta tras un MTC0 que cambia FR: el bloque termina en
  el COP0 y la entrada del siguiente, si depende de FR, lo nota.

La prueba `fr_switch_keeps_code_cache` (test_cop1.c) cambia FR varias veces
sobre un bloque con `$f1` y otro con registros pares y exige que no haya
vaciados, un gemelo por FR, ninguno para los pares, y que una escritura en
el código mate a los dos.

**Pendiente para la Fase 2:** el DMA del SP (que aún no existe) tiene que
llamar a `salsa64_bus_rdram_written` al escribir la RDRAM.

### Enlaces

Una salida a dirección fija deja `nop ; bl exit_link ; <destino>` (desde el
2026-10-13; antes el destino iba en `lis`/`ori` antes del `bl`, una palabra
más por salida). La palabra del destino nunca se ejecuta: `exit_link` la lee
de LR, la extiende con signo a 64 bits y vuelve a C con el sitio del `nop`
(LR − 8); el despachador encuentra o compila el destino y cambia el `nop`
por un `b` directo. Un salto al propio bloque vuelve al
cuerpo (después de escribir los registros modificados y con la misma
prueba del próximo evento), sin recargar nada. JR/JALR buscan el destino en
el propio código (`exit_jr`): en KSEG0 por `map[]`, fuera de KSEG0 recorre
su cadena de `xhash` hasta el primero con la misma vpc (hasta el 2026-10-10
solo miraba la cabeza: GoldenEye, con ~70 000 bloques en 16 384 cabezas,
salía a C en ~800 JR por campo); si no hay bloque, vuelven a C.

### Código fuera de KSEG0 (KSEG1 y TLB) — decisión del 2026-10-10

GoldenEye y Perfect Dark (juegos objetivo) corren casi todo su código
mapeado por la TLB en kuseg (0x7F000000). El código de un bloque depende de
su dirección virtual (`cur_pc`, el enlace de JAL, los destinos de los saltos
y las salidas) y de la física (las instrucciones). Con la TLB, una física
puede tener varias virtuales y una virtual cambia de física al reescribir la
TLB (TLBWI/TLBWR) o el ASID.

Opciones: invalidar los bloques mapeados en cada escritura de la TLB, o
dar a cada bloque la clave (vpc, física) y verificar la traducción al
entrar. Se eligió la segunda: los juegos que paginan con la TLB la
reescriben en cada falla (refill), y con la primera cada TLBWR mataría los
bloques de la página que sale y habría que recompilarlos cuando vuelve.
Visto en la emulación (2026-10-10): GoldenEye pagina su código bajo
demanda (fallos de TLB al hacer fetch en 0x7F0xxxxx, páginas de 4 KB que
trae a la RDRAM, ~0x300000–0x3A0000; 21 entradas en uso) y Perfect Dark
mapea 0x70000000 con una entrada de páginas de 4 MB y su segmento en
0x7F000000 con 15 entradas de 128 KB. Los dos ejecutan desde la RDRAM; el
código en la ROM se admite igual (KSEG1 o un mapeo a la ROM).

- **Clave:** los bloques de KSEG0 siguen en `map[]` (por física) sin
  cambios. Los demás van en `rec->x.xhash` (16 384 cabezas, por vpc)
  encadenados, con la vpc en `x.xb[]`; puede haber varios por vpc con
  físicas distintas. Solo vpc con extensión de signo de 32 bits. Todo eso
  vive en `src/cpu/rec_tlb.c` y `struct salsa64_rec_x` (`rec_tlb.h`); rec.c
  y rec_emit.c solo lo llaman.
- **Sin cruzar páginas de 4 KB** (la página mínima del VR4300): todo el
  bloque comparte la traducción. Un salto cuyo delay slot cae en la página
  siguiente queda fuera del bloque, como los demás que no caben.
- **TLB del recompilador (`ktlb[]`, hasta el 2026-10-10 `vtlb[]`):** 16 384
  entradas por página virtual de 4 KB (bits 25–12); el valor es la etiqueta
  (bits 31–26 de la virtual), válida (bit 25), D (bit 24: se puede
  escribir) y la página física (17 bits), o 0. Un bloque mapeado empieza
  con `lwz r0, ktlb[e](r28) ; xoris ; cmplwi ; bne-` (con un `addis` antes
  si la entrada queda a más de 32 KB; `ktlb[]` va justo antes de `flags`, a
  desplazamiento negativo de r28) comparando con la palabra que tenía al
  compilarse: si su página ya no traduce a su física, sale al despachador.
  También al llegar por un enlace o por `exit_jr`, así que los enlaces
  nunca se deshacen por la TLB. La misma tabla la usan las cargas y los
  almacenamientos a direcciones mapeadas («Datos mapeados por la TLB»).
- **Llenarla:** el despachador traduce la página con
  `salsa64_vr4300_kernel_probe` (la búsqueda de la TLB sin excepciones ni
  efectos, con el bit D) cuando no está en `ktlb[]`, y la anota; el camino
  lento de las cargas y almacenamientos anota la suya al traducir con
  éxito; y al escribir la TLB se anotan las páginas de la entrada nueva
  (pares de hasta 16 páginas): así el bloque o el acceso que viene a la
  página recién traída no sale a C.
- **Vaciarla (`salsa64_rec_x_sync`):** antes de correr cualquier bloque (KSEG0
  enlaza con mapeados) y en `rec_h_exec` tras cada COP0 (el bloque enlaza
  con el siguiente sin volver al despachador). Si cambió `cpu->tlb_writes`
  (sube con TLBWI, TLBWR y el reset), compara las 32 entradas con una
  instantánea (`snap[]`) y borra de `ktlb[]` solo las páginas de las que
  cambiaron (el par de su VPN2, antes y después); si cambiaron el ASID o
  ERL, vuelve a traducir cada página anotada y borra las que ya no dan lo
  mismo. Hasta el 2026-10-10 cada cambio volvía a traducir todas las páginas
  anotadas: GoldenEye, que trae ~400 páginas por campo con el refill, hacía
  14 000 traducciones por campo (109 M en 128 s). Basta porque todo cambio
  de la TLB pasa por C: COP0 termina el bloque, ERET y las excepciones
  salen con `redirect`.
- **Remapeo:** el bloque de la física vieja se queda (si la página vuelve,
  sirve de nuevo); el despachador desenlaza los de la misma vpc con otra
  física cuando la busca (`salsa64_rec_x_find`), para que sus enlaces se vuelvan a hacer
  con el bloque que vale.
- **Escrituras:** los de la RDRAM cuentan en `code_flags` y van también en
  `x.xpage` (por página física); `salsa64_rec_invalidate` los encuentra ahí.
  Los de la ROM nunca se invalidan (solo lectura); el ISViewer, que sí se
  escribe, no se compila.
- **Bucles de espera:** la salida lleva la instrucción del salto en
  `rec_site` (el despachador no relee memoria mapeada). `idle.c` analiza
  también código mapeado (traduce sin efectos con la misma búsqueda); si
  la tabla descarta el salto, el bloque se recompila sin la consulta
  (`salsa64_rec_x_kill_at`, por vpc).
- **Colisiones:** dos páginas a 64 MB caen en la misma entrada de `ktlb[]`:
  se turnan (cada cambio pasa por C). Correcto, más lento. Con 16 384
  entradas el código de Perfect Dark (0x7F000000) y sus datos (0x70000000)
  no chocan; con las 4096 de antes sí (16 MB).
- Memoria: +2,2 MB (`xb[]` 512 KB, `ktlb` 64 KB, `live` 32 KB, `xhash`
  32 KB, `xpage` 4 KB, `snap` 0,75 KB; bloques desprendidos: `copy` 1 MB,
  `copy_off` y `saved_code` 256 KB cada uno, `state` 64 KB).

### Bloques desprendidos: la página vuelve en otro marco (2026-10-11)

GoldenEye pagina su código bajo demanda con un pool de marcos: la página
virtual X vuelve en el marco que toque, y el DMA que trae otra página al
marco invalida los bloques que había. Cada (vpc, marco) era un bloque nuevo:
101 900 bloques en 128 s con unos 21 000 pc distintos, 7 vaciados completos
de la caché (cada uno recompila todo lo vivo: ~15 000 bloques de golpe) y
12–22 compilaciones por campo. El código de un bloque depende solo de su
vpc y de sus bytes, así que:

- Al compilar un bloque mapeado en la RDRAM se guarda una copia de sus
  bytes MIPS (`x.copy[]`, 1 MB; sin copia, se comporta como antes).
- Cuando una escritura invalida un bloque con copia, se **desprende**
  (`salsa64_rec_x_detach`) en vez de matarlo: pierde enlaces entrantes,
  banderas de código y `code` (así `exit_jr` y el despachador lo saltan),
  sale de `xpage`, pero sigue en `xhash` con su copia y su código guardado.
- Cuando el despachador no encuentra bloque para (vpc, pa) mapeado,
  `salsa64_rec_x_rekey` busca en la cadena uno de la misma vpc (desprendido,
  o vivo en otra física) cuya copia coincida byte a byte con lo que hay en
  pa, y lo engancha ahí: banderas y `xpage` en la física nueva, `code` de
  vuelta y la palabra de `ktlb[]` que compara al entrar parcheada en su
  código (`xoris`/`cmplwi`). Sus enlaces salientes siguen valiendo (son por
  vpc) y los entrantes se rehacen solos.
- Un vaciado completo borra copias y estados.

**Copias en ranuras (2026-10-28).** Hasta aquí `x.copy[]` era 1 MB repartido
en línea que no se recuperaba hasta un vaciado completo: Perfect Dark (143 220
compilaciones en 318 s en la PS3, 32 500 bloques vivos) lo llenaba en el
primer minuto y desde ahí ningún bloque nuevo tenía copia, así que cada
escritura sobre su código lo mataba y cada overlay recargado se retraducía
entero: 110 656 retraducciones idénticas (el 77 % de sus compilaciones, 6,4 s
de los 318, en campos de 60–120 ms con 1700–1950 retraducciones de golpe);
GoldenEye 4802 (15 %). Ahora las copias van en ranuras de 64, 128 y 256 bytes
(por `bytes` del bloque, listas libres por clase enlazadas en la propia
ranura) de un pool de 4 MB, y la ranura se libera cuando el bloque muere
(`salsa64_rec_x_remove`, también desde el desalojo de un tramo). Los
desprendidos retienen su id: hay un tope de 16 384 a la vez (por encima,
invalidar mata como antes) y, si los ids se agotan, `make_room` los mata
(`salsa64_rec_x_reclaim`, `stats.reclaimed`) antes de vaciar la caché entera.
`jit-copias-viejas` en la consola y `SALSA64_REC_OLD_COPY=1` en el host
(`SALSA64_REC_DBG_NO_COPY_FREE`) repiten el reparto viejo con su tope de 1 MB
para el A/B con el mismo binario. Un bloque de KSEG0 sigue sin copia: ahí la
retraducción idéntica (Banjo-Tooie 9,8 % de sus compilaciones, ~0,13 % del
tiempo) no justifica el costo.

### Datos mapeados por la TLB (2026-10-10)

Perfect Dark tiene sus datos en una página de 4 MB en 0x70000000: en la
intro hace ~3200 cargas y ~200 almacenamientos por campo a direcciones
mapeadas, cada uno una llamada a C con la búsqueda lineal de la TLB. (En
GoldenEye solo el código es mapeado; sus datos van por KSEG0.) Cuando la
dirección no es de KSEG0/KSEG1, el código generado prueba `ktlb[]` en la
sección fría: la dirección es extensión de signo de 32 bits (las de 64
bits no se anotan) y está alineada; la entrada `(va >> 12) & 16383` tiene
la etiqueta de `va` y es válida (y D, al escribir: una página de solo
lectura va a C, que lanza la excepción de modificación); y la física
(página de la entrada más los 12 bits bajos) cae en la RDRAM. Entonces
vuelve al camino rápido con el desplazamiento en r12 (los almacenamientos
miran `code_flags` igual que los de KSEG0). Unos 20 instrucciones contra
los cientos de ciclos de la llamada. Lo demás (sin anotar, E/S, excepción)
va a C, que anota la página al traducir con éxito.

Es exacto porque la entrada de `ktlb[]` es función de la TLB del CPU (la
misma búsqueda que el intérprete: `salsa64_vr4300_kernel_probe`, primera
entrada que coincide, V y D de la mitad que toca, ASID o global) y se borra
antes de correr código si esa entrada cambió, y porque un bloque solo corre
en modo kernel (Status.RE no aplica; con KX la traducción de una dirección
de 32 bits es la misma). Los pares LWL/LWR y SWL/SWR byte a byte exigen
además que los 4 bytes queden en la misma página (`(va & 0xFFF) <= 0xFFC`):
la traducción es la del primero.

### Perfil por causa (`rec_prof.h`, 2026-10-10)

Con `rec->prof` puesto, el despachador y las llamadas a C desde el código
generado miden con el timebase del PPE (`mftb`, 79,8 MHz en la PS3) y
cuentan por causa: código generado (cada entrada menos sus llamadas a C),
cargas y almacenamientos lentos (TLB a la RDRAM, TLB a E/S, falla de TLB,
E/S de KSEG0/1, código en la zona escrita, error de dirección),
instrucciones al intérprete (COP0, FPU, LL/SC, CACHE, traps, otras),
excepciones, el intérprete en el despachador por motivo (bloque que salió
en la entrada o a mitad, interrupción, delay slot, modo, segmento, sin
código, no cabe), los eventos por tipo (el del RSP incluye el sondeo y lo
que duerme esperando al SPE), compilar, revisar `ktlb[]`, bucles de espera y
las salidas por tipo. Apagado cuesta una carga y un salto por llamada a C.
En la PS3, el archivo `SALSA64_DATA_DIR/perfil` lo enciende y el desglose
(ms por campo) sale con las estadísticas cada 5 s. En el host,
`SALSA64_REC_PROF=N` lo escribe cada N campos (solo cuentas: qemu no mide
el Cell) y al final lista los bloques más ejecutados con sus palabras MIPS
(`tools/mips-disasm.py` las lee) y el uso de registros pesado por
ejecuciones.

### Caché de código

PS3: cola NOBITS de 32 MB (16 hasta el 2026-10-11: GoldenEye la llenaba cada
~40 s, con un vaciado completo cada vez; ~900 bytes de PPC por bloque de 5
instrucciones, sobre todo caminos fríos de memoria, que ahora llaman a
rutinas compartidas del preámbulo para la `ktlb`; 24 MB después) al final
del segmento de texto (`mk/rec-cache.ld`, docs/jit-ppe.md). 32 MB es el
techo: los saltos al preámbulo (`b`/`bl`, ±32 MB) tienen que alcanzarlo
desde el final de la caché. Las funciones de C quedan a menos de 32 MB del
preámbulo y de los primeros tramos, que las llaman con `bl` directo; cada
bloque decide al compilarse (`calls_near` por bloque) y los del final usan
`mtctr`/`bctrl`. `struct salsa64_rec` (mapa, banderas, bloques, enlaces, la
tabla de bloques fuera de KSEG0 y búferes de compilación) ocupa 7,8 MB y se
reserva una vez (docs/memory-budget.md).

**Desalojo por tramos (2026-10-11).** Hasta ahora, llena la caché, se
vaciaba entera y el conjunto vivo (~7000 bloques en GoldenEye) se
recompilaba de golpe: en la PS3, 3 vaciados por partida en la presa y
campos de hasta 52 ms. Ahora la zona de bloques se parte en
`SALSA64_REC_SEGMENTS` (8) tramos iguales y se asigna linealmente dentro
del tramo en uso; cuando no cabe otro bloque, se pasa al siguiente tramo
(el más viejo) y se matan solo los bloques cuyo código (o el guardado, si
están desprendidos) está en él: salen del mapa y de `xhash`, se deshacen
los enlaces que llegan a ellos y sus ids vuelven a la lista libre. Los
enlaces que *salen* de un bloque desalojado viven en la lista de su
destino con el sitio parcheado dentro del tramo: cada enlace guarda la
generación del tramo de su sitio (`seg_gen[]`, que sube al desalojar) y al
deshacerlo se salta si ya no coincide (la palabra pertenece a otro bloque).
El despachador también deja de parchear los sitios anotados antes de un
desalojo (`generation` sube). Las copias de bytes de los bloques
desprendidos (`x.copy[]`) no se recuperan hasta un vaciado completo, que
sigue ocurriendo si se agotan los ids o los enlaces. `SALSA64_REC_OPT_NO_EVICT`
(`jit-sin-tramos` en la PS3) y `jit-viejo` vuelven al vaciado completo.

### Perfil persistente y bloques calientes (2026-10-12)

Código: `src/cpu/rec_profile.c` (la tabla y el archivo), `rec.c` (zona
caliente, sumas) y `rec_emit.c` (consulta y contador). Medido en la PS3 el
2026-10-11: el código nativo cuesta ~47 ciclos por instrucción MIPS con
cualquier combinación de banderas; no lo limitan las instrucciones emitidas
sino el tamaño y la localidad del código (GoldenEye: ~7 MB de bloques vivos
contra 512 KB de L2, con la caché en páginas de 64 KB y el ERAT de
instrucciones del PPE cubriendo mucho menos). Lo que corre seguido está
repartido por toda la caché porque los bloques se colocan en el orden en
que se compilan. Ahora:

- **Contador por bloque.** Con un perfil conectado (`rec->profile`), la
  entrada de cada bloque incrementa `rec->hits[id]` (`addis`, `lwz`,
  `addi`, `stw`: cuatro palabras, a desplazamiento fijo de `flags`, que
  vive en r28) después de las comprobaciones de la entrada (evento, CU1,
  FR, página) y antes de cargar registros: cuenta **entradas**, no vueltas
  de un bucle dentro del bloque (la vuelta salta al cuerpo, después del
  contador), que es lo que pesa en la caché de instrucciones. Los 256 KB de
  `hits[]` se tocan a 4 bytes por entrada; los recortados (modo estricto)
  no cuentan. Entre campos, el hilo que emula suma los contadores a la
  tabla y los deja en 0 (`salsa64_rec_profile_fold`, cada 60 campos en el
  frontend); también al morir un bloque y al vaciar.
- **Identidad por contenido.** La entrada de la tabla es (dirección
  virtual, huella FNV-1a de las instrucciones MIPS del bloque, cuántas
  son). El código de la N64 llega por DMA en overlays y a veces se
  reescribe: un bloque con la misma dirección y otra huella es otra entrada,
  y nunca hereda el puntaje de la anterior. Los gemelos por Status.FR tienen
  los mismos bytes y comparten la entrada (los dos van calientes). La
  consulta se hace al compilar (`salsa64_rec_profile_lookup`, índice abierto
  por dirección; se agrega si no está, hasta 32 768 entradas) y el bloque
  guarda su entrada en `rec->pent[id]`.
- **Zona caliente.** Después del preámbulo, dos mitades (`hot_words` cada
  una; `rec->hot_bytes` las dimensiona, por omisión 1 MB entre las dos en la
  PS3, `perfil.cfg`) y después los ocho tramos fríos. Al cargar el perfil
  se marcan calientes las entradas de mayor puntaje cuyas palabras de PPC
  (las que ocuparon la última vez que se compilaron) suman hasta tres
  cuartos de la zona (`salsa64_rec_profile_mark_hot`: busca el umbral de
  puntaje por bisección, sin ordenar). Un bloque marcado se emite en la
  mitad caliente en uso; si no cabe se emite de nuevo para la zona fría
  (`emit_block` otra vez: las llamadas a C con `bl` o `mtctr` dependen de
  dónde cae) y `hot_full` hace que la próxima compilación pase a la otra
  mitad, matando lo que había en ella (`evict_segment`, como un tramo
  frío). Las mitades nunca se desalojan por llenarse los tramos fríos. El
  resto es igual: enlaces, invalidación, gemelos y bloques desprendidos no
  distinguen zonas (`seg_of` numera las mitades 0 y 1 y los tramos fríos
  desde 2).
- **Sitios de salida de los bloques calientes.** Un bloque que se emite en
  la zona caliente registra además cada sitio por donde sale con destino
  fijo: salto condicional tomado (interno o final, también los que saltan
  dentro del propio bloque), el salto final no tomado, J/JAL y el fin de
  bloque sin salto; cada uno cuenta cuántas veces se pasa por él (cuatro
  palabras en ese camino, con la dirección absoluta del contador; nunca en
  el cuerpo). La vuelta de un bucle al inicio del propio bloque no se
  cuenta (sería costo en el camino caliente; se asume tomada), ni los
  JR/JALR (destino calculado: sin sucesor). Hasta 8 sitios por bloque y
  8192 en total (`SALSA64_RPF_SITES`); `saltos = no` en `perfil.cfg` lo
  apaga para medir. Al guardar, de cada condicional sale cuántas veces se
  tomó y cuántas no (el final tiene su no tomado explícito; el interno se
  deriva: entradas al bloque menos lo que salió por los saltos anteriores,
  aproximado si el bloque salta a su propio interior), y de cada bloque sus
  dos sucesores más frecuentes: el destino y la huella del bloque que había
  ahí la última vez que se miró (lo resuelve la suma entre campos con el
  mapa de bloques). Solo se registra; los usos están en «Siguientes pasos».
- **Archivo.** `perfiles/<juego>.prf` (el nombre de las partidas: título
  y CRC1). **No contiene ni un byte de código del juego ni de código
  traducido**: direcciones virtuales, huellas FNV-1a de 64 bits de un solo
  sentido (no se puede reconstruir el bloque a partir de ellas), cuentas y
  banderas; por eso un perfil se puede compartir sin distribuir material del
  juego. La caché de código generado no se persiste nunca: se vuelve a
  traducir en cada sesión. Versión 2, big-endian, campo por campo:
  - cabecera (32 bytes): magia `SPRF`, versión, entradas, sitios, código del
    cartucho (4 letras de la cabecera de la ROM) y CRC1/CRC2 de esa
    cabecera: un archivo de otro juego se rechaza;
  - por entrada (24 bytes): dirección virtual, huella, puntaje,
    instrucciones, palabras de PPC, banderas (depende de Status.FR; se
    compiló con FR = 0 y con FR = 1; tiene más sitios de los registrados) y
    cuántos sucesores siguen, y por sucesor (16 bytes): destino, huella del
    bloque de ahí y cuenta;
  - por sitio (16 bytes): entrada, instrucción del salto, clase (condicional
    tomado, final no tomado, J, fin), destino, tomado y no tomado;
  - suma de verificación FNV-1a de todo lo anterior (8 bytes).
  Lo que no cuadra (magia, versión, identidad, cantidades, longitud, suma)
  se descarta entero y el perfil arranca vacío: el emulador funciona igual.
  Se escribe al cerrar el juego (también al volver al menú) y cada 2 min si
  hubo cuentas nuevas, por trozos de 512 entradas o sitios por vuelta del
  hilo de presentación (nada de búferes de un megabyte ni de parar la
  presentación; el que escribe lee `count` y `nsites` una vez y lo nuevo
  entra en el archivo siguiente; una cuenta a medias es solo un número un
  poco menor). Primero `.tmp` y `rename`, como las partidas. `perfil-activo`
  y `perfil.cfg` se leen al abrir cada juego.
- **Entre sesiones.** El puntaje guardado mezcla el del archivo con lo
  contado: en una sesión larga (2²⁶ entradas a bloques o más: entre medio
  minuto y un minuto de juego, Perfect Dark cuenta 1,2 M por segundo de
  consola) lo que no se ejecutó decae a la mitad, `max(hits, (score + hits) /
  2)`; en una corta solo puede subir, `max(score, hits)`, para que diez
  arranques de prueba no borren lo aprendido en una partida larga. Los
  puntajes saturan en 2³² − 1. Las cuentas de los sitios solo se suman entre
  sesiones (sin decaimiento), saturando igual.
- **Apagado por omisión** (decidido el 2026-10-12 con la medición de
  abajo): sin el archivo `perfil-activo` en `/dev_hdd0/salsa64` no hay
  tabla (se reserva con `memalign` la primera vez que se enciende, fuera del
  bucle), no se emite el contador y no hay zona caliente: costo cero en el
  hilo 0. Con el archivo, cuenta, guarda y coloca; `perfil.cfg` con
  `caliente = 0` cuenta y guarda pero no coloca. En el host, `salsa64-run
  --profile archivo` y `SALSA64_REC_HOT_KB`; las unitarias con recompilador
  tienen zona caliente de un dieciseisavo de la caché.
- **Resultado en la PS3** (docs/perf-log.md): juntar los 762 bloques más
  calientes de GoldenEye en 1 MB no cambia el costo del nativo (15,5 →
  15,8 ns por instrucción; Perfect Dark 12,7 → 12,4), y contar cuesta ~5 %.
  Lo que manda no es dónde está cada bloque sino cuántas líneas de caché
  toca por instrucción: ~21 palabras de PPC por instrucción MIPS en los
  calientes (punto 2 de «Siguientes pasos»). Lo que queda del trabajo es
  el perfil mismo: cobertura, sucesores y cuentas de los condicionales.

Memoria: `hits[]` 256 KB y `pent[]` 128 KB en `struct salsa64_rec`; la
tabla, 1 MB de entradas, 256 KB de sitios y 128 KB de índice, estática en el
frontend (docs/memory-budget.md).

La prueba `rec_counts_entries_and_places_hot_blocks` (test_rec_profile.c)
comprueba con el recompilador que un bloque marcado cae en la zona
caliente, que cuenta entradas y no vueltas, que el código reescrito en la
misma dirección es otra entrada y va a la zona fría, que al morir suma su
cuenta, y que con `hot_full` la siguiente compilación pasa a la otra mitad
sin tocar la primera; `rec_records_exit_sites_of_hot_blocks`, que un
condicional interno que sale del bloque registra su sitio, lo cuenta solo
cuando se toma, resuelve su sucesor a la huella del bloque del destino y
guarda tomado y no tomado, y que `saltos = no` no registra nada; las demás
prueban la tabla, la mezcla, el marcado, la identidad del juego y el
archivo (truncado, un byte cambiado, bytes de más, otro juego).

#### Siguientes pasos (diseño, sin implementar)

0. **Usar lo que ya registra el perfil de los bloques calientes** (sitios
   de salida y sucesores):
   - *Pistas estáticas de salto.* El PPE predice mal y un salto mal
     predicho cuesta ~20 ciclos; `bc` admite los bits de predicción
     estática (BO con `y`/`a+t`), que hoy el emisor pone con
     `PPC_BO_*_LIKELY/UNLIKELY` por convención (salida al camino lento =
     improbable). Con tomado/no tomado de cada condicional del perfil, el
     condicional interno se emite con la pista que diga la cuenta (y, si la
     cuenta dice que casi siempre se toma, invertido: el camino tomado en
     línea y el no tomado como salida), y el salto final igual.
   - *Sucesor probable contiguo.* Al compilar un bloque caliente cuyo
     sucesor más frecuente (dirección + huella) no está compilado, compilar
     el sucesor inmediatamente después en la zona caliente, y si ya está,
     nada: el enlace es un `b` y lo que importa es que las dos piezas
     queden en líneas y páginas vecinas. Con la huella se comprueba que el
     sucesor que hay hoy en el destino es el del perfil.
   - *Contar gratis, por muestreo desde el hilo 1.* El contador de cuatro
     palabras por entrada cuesta ~5 %. Alternativa: el hilo 1, que duerme
     el 85 % y le cuesta al hilo 0 ~0,13 ms por ms de trabajo, lee cada
     N µs qué bloque está corriendo el hilo 0 (el código generado deja el
     id en una palabra de `struct salsa64_cpu` solo al entrar, un `stw`:
     o nada, si el despachador lo anota al entrar desde C y las vueltas por
     enlace se atribuyen al último conocido) y suma una muestra a su
     entrada: puntajes proporcionales al tiempo, no a las entradas, que es
     lo que importa para «caliente», sin tocar el camino caliente. La
     instrumentación de sitios y sucesores queda solo para cuando se use.
   - *Traducción fuera del hilo de emulación.* La traducción es cálculo
     puro (palabras MIPS de entrada, palabras PPC de salida, más el estado
     que fija la forma del bloque: FR, segmento, palabra de la ktlb que
     compara la entrada, opciones) y con el perfil se sabe qué bloques van
     a correr (los calientes y sus sucesores). Dónde hacerla: ver el punto
     1 (el hilo 1 del PPE, antes que un SPE).

1. **Traducir por adelantado en el hilo 1 del PPE.** Hoy un bloque se
   compila la primera vez que se ejecuta: al entrar a una zona nueva (Zelda
   al cambiar de sala, DK64 al cargar un nivel) se compilan cientos de
   bloques en los primeros campos, 7–10 µs cada uno, y el campo se va a
   30–50 ms (tirón). Medido en la PS3 el 2026-10-12 (rama del PPE,
   GoldenEye en la presa): el hilo 1 (presentación) duerme ~85 % del tiempo
   y 1 ms de trabajo suyo le cuesta ~0,13 ms al hilo 0 (casi lineal hasta
   3 ms por campo, sin salto por la L2); el 79 % del hilo 0 es código
   nativo del recompilador. Así que el lugar natural para el trabajo de
   fondo del JIT es el hilo 1, no un SPE (que pediría un emisor compilable
   para SPU): traducir 100 bloques ahí cuesta ~1 ms del hilo 1 y ~0,13 ms
   del hilo 0, contra ~1 ms del hilo 0 si los compila él.
   - *Qué se traduce.* Cuando un DMA del PI escribe la RDRAM
     (`salsa64_bus_rdram_written` ya avisa al recompilador para invalidar),
     el hilo 0 recorre las entradas del perfil cuya dirección cae en
     páginas que ese DMA pudo traer (para KSEG0, la física es la virtual
     sin el segmento; para código mapeado, solo cuando la TLB ya traduce esa
     página: en el refill, `salsa64_rec_x_tlb_written` sabe qué páginas
     entran), compara la huella de lo que hay ahora en memoria con la de la
     entrada y, si coincide, encola una petición: vpc, física, segmento,
     FR, la palabra de la ktlb esperada y **una copia de las palabras MIPS**
     (hasta 64), para que el hilo 1 nunca lea la RDRAM, que el hilo 0 puede
     estar escribiendo. Índice que hace falta: las entradas del perfil
     ordenadas por página virtual (un `uint16_t` por entrada, ordenado al
     cargar). Se limita a las calientes o de puntaje mayor que un umbral,
     para no gastar caché y tiempo en bloques que esta sesión no corre.
   - *Cómo traduce el hilo 1.* Con un contexto y búferes de compilación
     propios (hoy `static struct ctx` en rec_emit.c y `rec->buf[]`: pasan a
     ser del contexto, ~200 KB más) y sin tocar nada vivo del recompilador:
     ni mapa, ni enlaces, ni banderas, ni ktlb, ni ids. Lo que el emisor
     lee hoy de `rec` son constantes de la sesión (rutinas del preámbulo,
     direcciones de `blocks[]`, `hits[]` y los sitios del perfil, opciones)
     y el estado de la petición. Entrega las palabras PPC con sus
     correcciones (`fixups`) sin resolver, porque la base en la caché la
     decide el hilo 0 al instalar (y con ella `calls_near`: o se traduce
     siempre con `mtctr` para lo encolado, o se resuelven las llamadas al
     instalar como el resto de los fixups).
   - *Cómo instala el hilo 0.* Entre campos (o en el despachador cuando
     va sobrado): toma un bloque terminado de la cola (un productor, un
     consumidor, barreras `lwsync` como en lib/spukit), comprueba que la
     huella de la memoria sigue siendo la de la petición y que la página
     traduce igual, pide id y lugar (`make_room`), resuelve los fixups con
     la base, copia, `salsa64_rec_sync` (dcbst/sync/icbi/isync, en el hilo
     que escribió) y registra como `new_block`. Si la huella cambió, se
     descarta. Todo lo que puede chocar con la invalidación pasa por el
     hilo 0, así que no hay invalidación concurrente: un bloque encolado no
     existe para el recompilador hasta que se instala.
   - *Riesgos.* (1) Invalidación concurrente: resuelta con la copia de las
     palabras y la comprobación al instalar; después de instalado, la
     invalidación normal. (2) Enlaces: solo el hilo 0 parchea; un bloque
     instalado se enlaza cuando se ejecuta, como hoy. (3) Caché de
     instrucciones: el `icbi` lo hace el hilo 0 sobre código que nadie
     ejecuta todavía; nunca se escribe código que el hilo 0 pueda estar
     ejecutando. (4) Costo por SMT en el hilo 0: 0,13 ms por ms del hilo 1;
     limitar las traducciones por campo. (5) Memoria: contexto y búferes
     del hilo 1 y la cola (unas decenas de peticiones de hasta 64 palabras).
   - *Primer uso:* la misma traducción de hoy, solo movida de hilo, con la
     cola y la validación al instalar; quita los tirones de entrar a zonas
     nuevas. Es el paso previo al siguiente.

1b. **JIT por niveles.** Con la traducción de fondo en pie, el hilo 0
   traduce rápido (nivel 0: lo de hoy, o incluso sin superbloques ni FPU en
   línea para los fríos) y el hilo 1 retraduce los bloques calientes
   (los del perfil, y los que pasen un umbral de `hits[]` en la sesión) con
   lo que cuesta tiempo de compilación: asignación de registros entre
   bloques (el sucesor probable compilado sabiendo qué registros deja el
   predecesor en el PPE, sin escribir ni recargar `cpu->gpr`: trazas de
   dos o tres bloques), pistas de salto y sucesor contiguo del punto 0,
   desenrollado de copias. La versión nueva se instala en otro lugar de la
   zona caliente y se **cambia la entrada de forma atómica**: la palabra
   `blocks[id].code` (32 bits, una escritura) y, al deshacer y rehacer los
   enlaces entrantes como en `unlink_all`, el `b` de cada sitio (otra
   escritura de 32 bits, con `icbi`); la versión vieja sigue siendo válida
   para quien ya esté en ella y muere con el tramo. Riesgos: los mismos de
   arriba más (6) dos versiones del mismo bloque vivas a la vez (como los
   gemelos por FR: ids distintos, mismos bytes, `kill_block` tiene que matar
   a las dos), y (7) los contadores (`hits[id]`, sitios del perfil): la
   versión nueva hereda la entrada del perfil y los sitios por (k, clase,
   destino), así las cuentas siguen.
2. **Optimizar más fuerte lo caliente y más compacto lo frío.** Hoy cada
   bloque se emite igual.

   *De dónde sale la expansión* (medido el 2026-10-12 en qemu con el volcado
   de bloques de GoldenEye hasta la presa, `SALSA64_REC_DUMP=1`, 14 826
   bloques, 17,9 MB de PPC; el perfil de la PS3 de la primera sesión
   coincide: 24 773 bloques y el 90 % del puntaje en 1098). Los 25 bloques
   más ejecutados (16 % de las entradas): 242 instrucciones MIPS en 5166
   palabras, **21,3 por instrucción: 9,9 en la sección caliente** (la que
   se ejecuta) **y 11,4 en la fría** (caminos lentos y salidas, que solo
   ocupan líneas vecinas); en todo el volcado la caliente es el 44 % (media
   317 palabras por bloque, 140 calientes; mediana 156, p90 800). Casi
   todo el código caliente del juego es mapeado por la TLB (0x7F… y
   0x70…), y sus datos también.
   - Un bloque de 2 instrucciones (7001F7E0: SQRT.S y JR, 89 palabras, 56
     calientes): la entrada son 18 palabras (comprobación de la página de
     la ktlb 5, del evento 3, de CU1 4, el contador del perfil 4, la
     recarga de `ra` y el destino del JR 2), la SQRT.S en línea 35
     (compuerta de FCR31 5, clasificación del operando 8, `fsqrt`, `frsp` y
     el inexacto 10, resultado y FCR31 12), la salida 3; la sección fría
     (33) es el camino al intérprete (15 de llamada con `li64` del pc y
     9 de revisión al volver) y tres salidas de 3.
   - Un bloque de 53 instrucciones con 26 accesos a memoria (7F0BA3B8:
     1365 palabras, 487 calientes): la sección caliente son 9,2 palabras
     por instrucción (ALU 51 %, saltos 13 %, estado del CPU por r31 11 %,
     comparaciones 11 %, constantes 9 %, RDRAM 5 %); la fría (878) son
     **saltos y llamadas 31 %, constantes 30 % y escrituras de estado
     28 %**: por cada acceso a memoria mapeada, ~6 palabras de vuelta de
     la rutina de la ktlb y ~30 del camino lento (`li64` del pc, los
     argumentos, la llamada y la escritura de los registros sucios antes de
     ella). Es decir, **cada acceso a datos mapeados cuesta ~10 palabras
     calientes y ~35 frías**.
   - La categoría «contador del perfil» son 4 palabras por bloque (0,3 por
     instrucción con 12 por bloque): no es la palanca.

   Palancas, por tamaño: (a) un solo camino lento por bloque en vez de uno
   por acceso (el índice del acceso en un registro y la rutina compartida
   deriva pc y argumentos: la fría de un bloque con muchos accesos baja a
   un tercio); (b) la escritura de registros sucios antes de un camino
   lento como máscara en un registro y un bucle en el preámbulo, en vez de
   `std` por registro en cada sitio (hoy 28 % de la fría de ese bloque);
   (c) para los juegos de datos mapeados, el camino rápido de la ktlb en
   línea antes que el de KSEG0 (el perfil puede anotar por acceso si fue
   KSEG0 o mapeado, como los sitios de salida); (d) en la entrada, fundir
   las comprobaciones (página, evento, CU1) en menos saltos. Con la clase
   conocida al compilar (y, con el JIT por niveles, con el hilo 1 pagando
   el tiempo de lo caliente):
   - Frío: sin superbloques ni saltos internos (menos salidas duplicadas y
     caminos fríos), sin FPU en línea (al intérprete con una llamada: ~55
     palabras menos por operación), sin camino rápido de datos mapeados
     (la rutina compartida de `ktlb` en vez del código en línea), y los
     registros fijos sin préstamo. Meta: la mitad de las palabras (hoy
     media 248 por bloque), que es la mitad de L2 y de páginas que un
     bloque frío roba al caliente cuando se ejecuta una vez.
   - Caliente: lo de hoy más lo que hoy no compensa en general porque
     cuesta tamaño: desenrollar los pares LWL/LWR y las copias, más
     registros fijos entre bloques (s0, at, t6: 26–31 % de las ejecuciones
     en GoldenEye), y las rutinas de libultra por huella (HLE de funciones:
     matrices, trigonometría, copias) solo donde el perfil diga que pesan.
   - La clase se decide con el perfil cargado; un bloque sin entrada se
     compila como ahora. Primero medir en la PS3 cuánto baja el tamaño de
     los fríos y si el nativo por instrucción se mueve; si la ganancia de
     esta rama es chica, el límite es otro (ERAT: una zona caliente que
     quepa en 64 páginas de 4 KB, 256 KB, podría pesar más que una de 1 MB).
3. **Separar las rutas frías de los bloques calientes** (opcional): las
   secciones frías (caminos lentos, salidas pendientes) van hoy pegadas al
   cuerpo, a `bc` de ±32 KB. Para ponerlas en otra zona hacen falta
   trampolines (`bc` a un `b` lejano) o `bc` invertidos sobre un `b`:
   una palabra y un salto más por camino frío, a cambio de que las líneas
   de 128 bytes del cuerpo no arrastren código que casi nunca corre. Solo
   si la medición de la zona caliente muestra que el tamaño del cuerpo
   manda.

## Pruebas en el host

RPCS3 no ejecuta código generado (docs/rpcs3.md). En la Mac se compila el
núcleo para Linux ppc64 big-endian (misma ABI ELFv1 que PSL1GHT) y se corre
con qemu-user:

```
tools/docker-image-ppc64.sh            # una vez: compilador cruzado y qemu
tools/test-rec.sh                      # todo lo de abajo
tools/build-ppc64.sh                   # build/ppc64/salsa64-unit y salsa64-run
tools/qemu-ppc64.sh build/ppc64/salsa64-unit rec
tools/qemu-ppc64.sh build/ppc64/salsa64-run --rec rom.z64
tools/qemu-ppc64.sh build/ppc64/salsa64-run --lockstep rom.z64
SALSA64_ROMS=/ruta/a/respaldos \
  tools/qemu-ppc64.sh build/ppc64/salsa64-run --lockstep /roms/goldeneye.z64
```

Los respaldos de juegos van fuera del repo, en una carpeta que
colima comparta (no todas lo están); `SALSA64_ROMS` la
monta de solo lectura en `/roms`.

- `salsa64-unit rec`: las pruebas unitarias otra vez con el recompilador en
  **modo estricto** (nunca interpreta lo que se puede compilar: si un bloque
  no cabe antes del evento, lo compila recortado), así cada `t_step(n)`
  ejecuta código generado.
- `salsa64-run --lockstep`: una segunda máquina con el intérprete; después
  de cada bloque (sin enlaces) la lleva al mismo ciclo y compara CPU, MI y
  cada N bloques la RDRAM. Es el "modo traza" del criterio de la Fase 3.
  La referencia lleva su propia HLE gráfica y de audio (desde el
  2026-10-11): sin ellas no recibía la interrupción del DP de las tareas
  con HLE (G_RDPFULLSYNC) y GoldenEye y Perfect Dark, que ya dibujan,
  divergían por una interrupción que a la referencia nunca le llegaba.
- La caché del host va en `.bss` con `mprotect` para que las llamadas sean
  `bl` como en la PS3; `SALSA64_REC_FAR=1` usa un `mmap` lejano y prueba el
  otro camino (`mtctr`/`bctrl`). `SALSA64_REC_DUMP=1` vuelca cada bloque.

qemu 7.2 con `-cpu 970` da resultados erróneos (el intérprete falla 15
pruebas que pasan en la PS3) y con `power5+` se cuelga. Se usaba `power7`;
desde el 2026-10-10 es `power9` (`SALSA64_QEMU_CPU` lo cambia): `sin`, `cos`
y `floor` de la glibc estática de ppc64 usan `mffscrn`/`mffscrni` (Power ISA
3.0), que qemu con `power7`/`power8` rechaza con SIGILL (se caía la suite
`sync`, por `audio_sync.c`; en la PS3 no hay glibc). Con `power9` las
pruebas, nemu64-test y vitest dan lo mismo que antes. El emisor se limita a
Power ISA 2.02 a mano (sin `isel`, `popcntd`, `ldbrx`).

El compilador cruzado corre emulado como amd64 y a veces muere con "internal
compiler error: Segmentation fault"; repetir compila bien
(`tools/build-ppc64.sh` reintenta una vez).

## Resultados

| Fecha | Prueba | Entorno | Resultado |
|---|---|---|---|
| 2026-10-10 | Unitarias, intérprete | qemu-ppc64 (power7) | 162/162 |
| 2026-10-10 | Unitarias, recompilador estricto (`bl` y `mtctr`) | qemu-ppc64 | 162/162; 223 bloques, 155 recortados, 38 instrucciones interpretadas |
| 2026-10-10 | nemu64-test, `--rec` | qemu-ppc64 | 462 fallas, las mismas de la línea base; 1,0 % interpretado |
| 2026-10-10 | `tools/test-rec.sh` con `-cpu power9` (main f6fed36) | qemu-ppc64 | unitarias 289/289 en los cuatro modos (con `power7` se caía la suite `sync`); nemu64-test 446 fallas, las de la línea base, también con pin+eager; vitest OK; `--lockstep` sin diferencias |
| 2026-10-10 | nemu64-test, `--lockstep` | qemu-ppc64 | sin diferencias en 28,5 M bloques |
| 2026-10-10 | vitest, `--rec` | qemu-ppc64 | `vitest: OK` |
| 2026-10-10 | Unitarias, recompilador estricto | **PS3 real** (CECH-3011A, HEN), f7b655d | 163/163; caché NOBITS, llamadas con `bl` |
| 2026-10-10 | nemu64-test, intérprete contra recompilador | **PS3 real**, f7b655d | mismas 462 fallas, mismo hash y mismo ciclo; 7,9 s → 1,8 s (**×4,36**, 104 MIPS) |
| 2026-10-10 | `bench` mixto | **PS3 real**, f7b655d | 21,3 → 156,6 MIPS (×7,4) |
| 2026-10-10 | nemu64-test, cuatro variantes | **PS3 real**, f416bee | registros fijos 1821 ms (×4,45); 213/213 en cada variante |
| 2026-10-10 | Frontend con vitest | **PS3 real**, f18b392 | 60 campos/s, 2,9–5,4 ms de emulación por campo (intérprete 35–66) |
| 2026-10-10 | **Zelda OoT** en el frontend | **PS3 real**, f416bee | 41–60 campos/s (intérprete 15–17); emulación 4–14 ms por campo; se ve igual |
| 2026-10-10 | Unitarias con código mapeado, KSEG1 y ROM; recompilador estricto (cuatro modos) | qemu-ppc64 | 222/222 |
| 2026-10-10 | nemu64-test, `--rec` y `--lockstep`, con KSEG1/TLB compilados | qemu-ppc64 | fallas de la línea base; 623 bloques fuera de KSEG0 (de 29 517), 27 traducciones; lockstep sin diferencias (también vitest) |
| 2026-10-10 | GoldenEye (USA), respaldo del usuario, `--lockstep` 200 M ciclos (2,1 s) | qemu-ppc64 | sin diferencias; 3518 de 3524 bloques mapeados, 5050 traducciones (la TLB cambia seguido), 0,1 % interpretado |
| 2026-10-10 | Perfect Dark (USA), respaldo del usuario, `--lockstep` 200 M ciclos | qemu-ppc64 | sin diferencias; 198 de 202 bloques mapeados, 3 traducciones (con 8 MB, pocas entradas grandes) |
| 2026-10-10 | Unitarias con conversiones, LWL/LWR y MFC0 en nativo | qemu-ppc64 (plain, mtctr, fijos, temprana) | 221/221; `--lockstep` sin diferencias en nemu64-test y vitest; mutación 10/10 |
| 2026-10-10 | nemu64-test y bench con y sin `RARE_EXEC` (conversiones, LWL/LWR y MFC0 al intérprete) | **PS3 real**, e210aeb | nemu64-test 1894 → 1816 ms (−4,1 %), mismas fallas; bench cvt ×9,7, no alineadas ×10,7, mfc0 ×71, espera por Count ×59 |
| 2026-10-11 | `tools/test-rec.sh --lockstep` con enlace tras COP0, ktlb de datos, bloques desprendidos, superbloques y caminos lentos compartidos | qemu-ppc64 | unitarias 351/351 en los cuatro modos; nemu64-test 433 fallas (línea base) con y sin pin+eager; vitest OK; `--lockstep` sin diferencias en nemu64-test y vitest |
| 2026-10-11 | `--lockstep` con la referencia con HLE: Perfect Dark 3 G ciclos (intro); GoldenEye con `goldeneye-dam.pad` 7,5 G ciclos (80 s de consola, hasta jugar en la presa) | qemu-ppc64 | **sin diferencias** en los dos; GoldenEye 283 M salidas comparadas en 1 h 46 min, 16 606 bloques (17 031 enganchados de vuelta) |
| 2026-10-11 | FPU residente, sin microcódigo, COP0 con rutinas propias (9d1eba0, 1cf4c09, 952366c) | qemu-ppc64 | unitarias 365/365 en cinco modos (plain, far, pin, eager y FPU por memoria), con la prueba nueva `executed_sequences_match_interp`; nemu64-test 433 fallas (línea base) con y sin pin+eager; vitest OK; `--lockstep` sin diferencias en nemu64-test, vitest y Perfect Dark 300 M ciclos. Antes del arreglo del camino lento de LWC1, Perfect Dark abortaba (SIGSEGV en qemu) |
| 2026-10-11 | Caché de 32 MB por tramos, con `SALSA64_REC_CACHE_MB` para forzar desalojos | qemu-ppc64 | unitarias 365/365 con 2 MB; nemu64-test 433 fallas con 3 MB (93 desalojos, 0 nuevas); vitest OK con 2 MB; `--lockstep` de nemu64-test con 3 MB **sin diferencias** (92 desalojos); Perfect Dark 300 M ciclos sin diferencias |
| 2026-10-11 | FPU residente (9d1eba0): GoldenEye con `goldeneye-dam.pad` 7,5 G ciclos en `--lockstep` | qemu-ppc64 | **sin diferencias**, 283,6 M salidas comparadas, 16 606 bloques (17 031 enganchados de vuelta) |
| 2026-10-11 | Saltos internos con destino dentro del bloque y r26 para el bloque (árbol final, 15d6724) | qemu-ppc64 | unitarias 365/365 con y sin fijos; nemu64-test 433 fallas y `--lockstep` sin diferencias (la primera versión dejaba basura en los registros escritos por las instrucciones saltadas: el lockstep lo encontró en el bloque 80014c84); vitest OK; Perfect Dark 300 M sin diferencias; GoldenEye hasta la presa: ejecuciones de bloques 643 M → 509 M en 128 s (perfil) y `--lockstep` 7,5 G ciclos **sin diferencias** (204 M salidas comparadas, 14 860 bloques, 1 vaciado y 6 desalojos con la caché de 24 MB del host); Perfect Dark 3 G ciclos (intro) **sin diferencias** (4 desalojos) |
| 2026-10-12 | Gemelos por Status.FR en vez de vaciar la caché (ad18cce) | qemu-ppc64 | unitarias 426/426 en los cinco modos (con `fr_switch_keeps_code_cache`); nemu64-test 433 fallas (línea base), vitest, estados y `--lockstep` de nemu64-test y vitest sin diferencias; Perfect Dark 3 G ciclos **sin diferencias** (20 971 salidas por CU1: libultra habilita la FPU por excepción); GoldenEye hasta la presa 7,5 G ciclos **sin diferencias** (14 825 bloques, 0 vaciados, 6 desalojos, 2 h 0 min); Paper Mario 1,9 G ciclos sin diferencias. DK64 en qemu: 4 173 → 0 vaciados |
| 2026-10-12 | Perfil persistente y zona caliente (4809ec2; el lockstep de GoldenEye con el binario de ae46fb7, sin sitios de salida) | qemu-ppc64 | unitarias 440/440 en plain, pin y caché chica (suite `perfil`); nemu64-test 433 fallas con perfil nuevo y cargado, también con caché de 3 MB y zona de 256 KB (rotaciones de mitad); `--lockstep` **sin diferencias** en nemu64-test (las dos cachés), vitest y Perfect Dark 3 G ciclos con el perfil cargado (624 calientes, 1367 sitios); GoldenEye hasta la presa 7,5 G ciclos con el perfil cargado (692 calientes, 1 rotación) **sin diferencias** (1 h 20 min) |
| 2026-10-12 | Formato 2 del perfil con sitios de salida (4809ec2, el árbol final del núcleo) | qemu-ppc64 | GoldenEye hasta la presa 7,5 G ciclos en `--lockstep` con el perfil cargado (692 calientes, 1483 sitios de salida contando en los caminos que salen) **sin diferencias** (1 h 26 min) |
| 2026-10-13 | Código compacto (2be85a4; con main a9df32a) | qemu-ppc64 | unitarias 437/437 (446/446 con main) en los cinco modos; nemu64-test 433 fallas (línea base) con y sin fijos; vitest y estados OK; `--lockstep` **sin diferencias** en nemu64-test, vitest, Perfect Dark 3 G ciclos (38,8 M salidas comparadas) y GoldenEye hasta la presa 7,5 G ciclos (204 M salidas, 14 825 bloques, 2 h 15 min); con main fusionado (a9df32a), otra vez nemu64-test, vitest, Perfect Dark y GoldenEye hasta la presa (204 M salidas) sin diferencias; con el muestreo por tiempo encendido (a5ea24e), unitarias 449/449, nemu64-test 433 fallas y su `--lockstep` sin diferencias. Dos errores que atraparon las pruebas antes de pasar: `addi r0, 0, -4` en la rutina compartida (r0 como base vale 0: cur_pc salía −4 y las excepciones iban a cualquier parte) y la cadena de escritura entrando por el extremo equivocado (escribía solo el primer registro: `dsub_and_daddi` y el lockstep de nemu64-test lo vieron) |
Rendimiento: solo en la PS3 real (docs/perf-log.md, «Recompilador»).

## Descartes de espera sin retraducción

Desde el trabajo de Road Rash, quitar una consulta al detector de espera
parchea su salto PPC, sin invalidar la traducción. Las colisiones de la
tabla del detector podían hacer retraducir los mismos bloques miles de
veces. Causa, contadores, reproducción y archivo de comparación:
[Road Rash: invalidaciones](roadrash-invalidaciones.md).
