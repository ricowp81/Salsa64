# RSP a bajo nivel (LLE): prototipo de la Fase 6

Fecha: 2026-10-10. Estado: prototipo acotado, validado en el host y
**medido en la PS3 real** (SLSA00082 «Salsa64 LLE»): corre, se ve y suena,
14–18 veces más lento que el RSP real; una divergencia del SPE abierta.

## Por qué

Cada microcódigo exige una HLE escrita a mano (F3D, F3DEX, F3DEX2, Fast3D
2.0G, ABI1, nead…). Los microcódigos propios (Rare, Factor 5, el video de
Resident Evil 2), los de libdragon (rspq/rdpq) y cualquier homebrew solo
caen ejecutando el microcódigo real. El SPE se parece al RSP: SIMD de 128
bits con ocho carriles de 16 bits, memoria local con DMA; la IMEM y la
DMEM (4 KB cada una) caben de sobra en el local store.

## Qué hay

### Intérprete portable (`src/rsp/lle/`, GPLv2+)

- `lle.h`: el estado del RSP (32 registros escalares, PC y PC siguiente,
  32 vectores de 8 × 16 bits, acumulador de 48 bits por elemento en tres
  vectores, VCO/VCC/VCE como máscaras de 16 bits, el divisor de VRCP/VRSQ,
  las dos tablas de 512 entradas) y la interfaz de E/S de COP0 que da cada
  plataforma (`lle_io_read/write/break/poll`).
- `cpu.c`: unidad escalar (el subconjunto MIPS del RSP: sin multiplicador,
  sin LWL/LWR, sin excepciones; los opcodes inexistentes de SPECIAL hacen
  `rd = rs >> (rs & 31)` como el hardware), saltos con delay slot (un salto
  en el delay slot de otro reemplaza el destino), COP0 (MFC0/MTC0 → E/S),
  despacho a la unidad vectorial, cargas y almacenamientos escalares byte a
  byte (no alineados, con vuelta en 4 KB).
- `vu.c`: las 48 operaciones vectoriales con la difusión de `vt` por el
  campo `e`, el acumulador y sus saturaciones (VMULF/VMULU, VMUDx/VMADx,
  VMACQ/VMULQ, VRNDP/VRNDN, VADD/VSUB con acarreo, VADDC/VSUBC, VABS,
  VLT/VEQ/VNE/VGE, VCH/VCL/VCR, VMRG, lógicas, VSAR, VRCP/VRCPL/VRCPH,
  VRSQ/VRSQL/VRSQH, VMOV, VNOP y los opcodes reservados).
- `vmem.c`: LBV/LSV/LLV/LDV/LQV/LRV/LPV/LUV/LHV/LFV/LTV y sus
  almacenamientos (incluido SWV; LWV no existe en la N64), MFC2/MTC2,
  CFC2/CTC2.
- Orden de los datos: DMEM e IMEM como bytes en el orden de la N64; los
  vectores como ocho enteros del host con `e[0]` el elemento 0. El mismo
  código vale en little-endian (host) y en big-endian (PPE y SPE, donde un
  vector cargado de memoria coincide con `e[]`): sin `#ifdef`.
- Semántica de los casos raros de la VU y de las cargas tomada de ares
  (ISC), que nemu64-test verifica contra la consola; registrado en
  `docs/THIRD_PARTY.md`.
- Precisión: una instrucción por ciclo, sin la segmentación ni la emisión
  doble del RSP; SSTEP no se emula.

### Conductor en el hilo del CPU (`core.c`, host y PPE)

- La DMEM y la IMEM son las del bus (`bus->sp_mem`); COP0 va por
  `salsa64_sp_read/write` y `salsa64_dp_read/write`: el DMA, el semáforo,
  las señales y DPC_* se comportan igual que desde el CPU.
- El microcódigo avanza en rebanadas: un evento del planificador
  (`SALSA64_EV_RSP_LLE`, cada 1536 ciclos del CPU) ejecuta lo que
  corresponde al tiempo transcurrido, a dos instrucciones del RSP por cada
  tres ciclos del CPU (62,5 contra 93,75 MHz); y cada acceso del CPU a un
  registro del SP o del DP (`salsa64_rsp_sync`, en `sp.c`) lo pone al día
  antes, así un bucle del CPU que sondea SP_STATUS ve avanzar al RSP entre
  lecturas. `--lle-speed` cambia la proporción.

### Enganche en el núcleo (`src/core/rsp.c`, `sp.c`, `rcp.h`)

- `salsa64_lle_backend` (start, sync, reset, y done/finish para el SPE) y
  el modo `off | auto | all`: en `auto`, al quitarle HALT al RSP la tarea
  va al microcódigo real si no tiene HLE (microcódigo gráfico desconocido,
  ABI de audio desconocido, o un arranque sin OSTask, como los de
  nemu64-test); en `all`, siempre. La HLE sigue siendo el camino por
  omisión.
- SP_STATUS se modifica con operaciones atómicas (`__atomic_*`): en la PS3
  el SPE cambia el mismo registro desde fuera. Los pares (limpiar, poner)
  con los dos bits a la vez no cambian nada, como en la consola (antes
  ganaba «poner»: ahora pasan «SP Set/Clear Signal / Interrupt /
  Interrupt on Break»).
- DPC_START/DPC_END/DPC_STATUS con semántica real: el primer START vale
  hasta el END; cada END (o descongelar) entrega [CURRENT, END) al
  consumidor del RDP y CURRENT avanza al final; xbus y freeze con sus
  pares de bits. Pasan «RDP START & END REG (masking)» y «RSP STATUS:
  start-valid».
- Interrupción del DP: con el microcódigo real la da el SYNC_FULL del
  flujo de comandos (`dpc.c` → `salsa64_rsp_dp_done`), nunca la forzada
  de «microcódigo sin HLE» ni la de lista cortada (`SALSA64_GFX_CUT`),
  que son de la HLE: la rama LLE de `salsa64_rsp_start` sale antes.

### RDP a bajo nivel (`src/rsp/gfx/dpc.c`)

- Un trabajo `GFX_JOB_DPC` ejecuta los comandos crudos [start, end) (de la
  RDRAM o, con `GFX_JOB_XBUS`, de una imagen de la DMEM) sobre el estado
  del RDP de la HLE: estado y cargas con `gfx_rdp_command`, rectángulos
  con `gfx_texrect`, triángulos 0xC8–0xCF con `gfx_rdp_triangle` (el
  camino de triángulos crudos que ya existía para el cielo de GoldenEye).
  El backend síncrono (`sync.c`) lo expone como `salsa64_dpc_backend`.
- En el SPE de la HLE gráfica compila igual (`dpc.c` está en `src/rsp/gfx`)
  aunque hoy el consumidor corre en el PPE.

### En el SPE (`src/spu/rsp/`, `src/platform/ps3/rsp_spu.c`)

- Trabajo de lib/spukit (`rsp/lle/job.h`): el PPE manda `pc`, la EA de
  `struct salsa64_sp`, de la DMEM/IMEM, de la RDRAM y de un registro de
  tramos; el SPE trae DMEM e IMEM (8 KB), corre hasta BREAK, HALT o
  200 000 instrucciones (entonces el PPE publica una continuación, así el
  CPU puede interactuar entre medias) y devuelve DMEM/IMEM, los registros
  del DMA, PC y un `lle_result`.
- SP_STATUS y el semáforo se leen y modifican con DMA atómico
  (`getllar`/`putllc` sobre la línea de 128 bytes que los contiene); el
  PPE usa `lwarx`/`stwcx` (`__atomic_*`) sobre las mismas palabras: ninguno
  pisa al otro. Cada 256 instrucciones el SPE relee SP_STATUS: un HALT del
  CPU lo detiene enseguida.
- El DMA del SP se hace entre la RDRAM y el local store (lecturas por el
  tramo alineado a 16; escrituras en trozos de 8 o múltiplos de 16, sin
  reescribir RDRAM vecina); los tramos escritos vuelven en el resultado y
  el PPE los pasa a `salsa64_bus_rdram_written` (recompilador).
- Los registros del DP se llevan en el SPE; cada DPC_END anota el tramo en
  el registro (con los comandos copiados si venían de la DMEM, porque el
  microcódigo reescribe el anillo enseguida) y CURRENT avanza como si el
  RDP consumiera al instante. El PPE reproduce los tramos al recoger el
  trabajo (`salsa64_dp_submit`) sobre `gfx_ppe` (la HLE gráfica del PPE, que
  también sirve de consumidor del RDP). La interrupción del SP (BREAK con
  INTR_BREAK, o pedida por SP_STATUS) queda anotada y el PPE la levanta
  al recoger: latencia de un sondeo (10 000 ciclos, ~0,1 ms).
- Limitaciones del prototipo: lo que el CPU escriba en la DMEM/IMEM
  mientras el SPE corre se pierde al volver (libultra no lo hace durante
  una tarea); limpiar la interrupción del SP desde el microcódigo no se
  transmite; DPC_CURRENT lo ve el microcódigo siempre al final (el RDP
  real tarda, y el anillo xbus nunca bloquea).
- Frontend: `SALSA64_DATA_DIR/lle` con `auto` o `all`; sin el archivo,
  nada cambia. Lo que anota el log: por ventana, «rsp lle: N tareas (M
  trabajos), instrucciones por tarea, ms en el SPE por tarea, ns por
  instrucción (MIPS; el RSP real ~62), esperas, sondeos, interrupciones,
  tramos del RDP»; al salir, totales y la tarea más larga.

### Herramientas

- `salsa64-run --lle[=auto|all]`, `--lle-speed F`, `--lle-check-audio`
  (con `--lle=all`: cada tarea de audio corre también en la HLE sobre una
  copia de la RDRAM y se comparan los tramos escritos; `SALSA64_LLE_DUMP_TASK=N`
  vuelca la lista de comandos de la tarea N).
- `--lle-check-gfx` (también con `--lle=all`, que pone): el equivalente
  gráfico. Cada tarea gráfica corre entera en el acto en el microcódigo real y en
  la HLE desde el mismo estado del RDP, las dos salidas pasan por el mismo
  rasterizador del host y se comparan los píxeles (docs/gfx-censo.md);
  `tools/censo-gfx.sh` lo corre sobre una escena de cada juego.
- `tools/test-roms.sh` corre nemu64-test dos veces: con la HLE (línea base
  `tests/roms/nemu64-test.fallas`) y con `--lle` (`nemu64-test-lle.fallas`).

## Validación

- Pruebas unitarias (`tests/unit/test_rsp_lle.c`, 12): escalar y BREAK (PC
  en el delay slot), memoria no alineada, VMULF/VMACF con saturación, VCH
  y sus banderas por CFC2, VRCP por tabla y en 32 bits, LQV/SQV no
  alineadas y LDV/MTC2/MFC2, DMA desde el RSP, SP_STATUS y semáforo desde
  el RSP (y reanudar tras HALT), rebanadas por el planificador con
  interrupción al BREAK, DPC con xbus y congelado, modos auto/all.
  297 pruebas en total en el host.
- nemu64-test (host): con `--lle` fallan **136 de 4637** (97 %) frente a
  433 (90 %) sin él. Las 214 pruebas «RSP …» (ISA escalar y vectorial,
  cargas y almacenamientos con todos sus casos raros, tablas de VRCP/VRSQ,
  BREAK, señales, semáforo, registros vistos desde el RSP) pasan todas.
  Quedan 4 «RDP STATUS» (banderas de ocupado del DP y xbus desde el CPU
  comprobando el framebuffer en la RDRAM: el RDP no escribe la RDRAM) y lo
  de siempre (cachés, bus PI). Sin regresiones; 5 mejoras en el camino HLE
  por los registros del SP/DP.
- Super Mario 64 (respaldo del usuario, host, `--lle=all`): el microcódigo
  Fast3D real corre en el intérprete (161 M instrucciones en 9,6 s de
  consola, 858 tareas) y el título con la cara de Mario sale por el camino
  de triángulos crudos igual que con la HLE (solo difiere el parpadeo del
  «PRESS START»). 0 comandos sin implementar.
- Zelda OoT (host, `--lle=all`): 211 M instrucciones en 16 s de consola
  (1379 tareas, 153 000 por tarea, la más larga 542 000); el audio (nead
  real) suena con la misma energía que la HLE. `--lle-check-audio`: las
  primeras 30 tareas de audio son idénticas byte a byte a la HLE; desde el
  primer sonido difieren solo los bloques que escribe **FILTER**, con
  valores exactamente dobles: mupen64plus-rsp-hle (de donde viene la HLE)
  promedia las dos tablas del filtro (`(lut5 + lut6) >> 1`) y el
  microcódigo real no. Pendiente confirmar de oído cuál suena como la
  consola (el LLE ejecuta lo real).
- El gráfico de Zelda en LLE produce 375 000 triángulos crudos en 63 000
  tramos xbus sin errores; la imagen no se comparó aún con la HLE.

## Mediciones

Host (Mac, clang nativo, `-c 1500000000` = 16 s de consola, Zelda OoT,
`--lle=all`): 0,95 s sin LLE y 3,58 s con él, por 211 M instrucciones del
RSP → **12,5 ns por instrucción** del RSP en el Mac (~40 ciclos de un
núcleo a ~3,3 GHz; ver `docs/perf-log.md`).

PS3 real (2026-10-10, Zelda OoT con `lle` = `all`; tabla completa en
`docs/perf-log.md`): el intérprete en el SPE hace **226–283 ns por
instrucción del RSP (3,5–4,4 MIPS)** en el microcódigo gráfico y de
audio, y 112 ns (9 MIPS) en los tramos de S2DEX dominados por DMA. El RSP
real hace ~62,5 M instrucciones/s: **14–18 veces más lento**, muy por
debajo de la estimación previa (0,3–1×). Causas, por orden: el SPU no tiene
unidad escalar ni predicción de saltos (el `switch` de despacho es un salto
indirecto por instrucción, ~20 ciclos de penalización cada uno, y cada
acceso a un byte de la DMEM es carga de 16 bytes + rotación); cada MFC0/
MTC0 de SP_STATUS o del semáforo es un DMA atómico de ~1 µs, y F3DEX2 toma
y suelta el semáforo alrededor de cada DMA (2–190 veces por tarea) y lee
SP_STATUS 180–430 veces por tarea; cada DMA del SP es una transferencia
síncrona (~2 µs). Se ve y se oye: el título y la intro de Zelda se dibujan
con el F3DZEX real por el camino de triángulos crudos y el nead real suena,
a 24–55 campos/s con el audio estirado.

Lo que salió en la consola y se corrigió sobre la marcha (tres paquetes):

- **SP_PC pisado al recoger el trabajo**: el CPU escribe SP_PC = 0 y quita
  HALT para la tarea siguiente (al ceder la gráfica para el audio) antes de
  que el PPE recoja el trabajo anterior; `collect` escribía el PC donde se
  detuvo el microcódigo viejo y la tarea nueva arrancaba en cualquier
  parte. Ahora `bus->sp.pc_writes` cuenta las escrituras del CPU y el PC
  recogido solo se aplica si no hubo una nueva (también en el conductor del
  host).
- **Estado del DP entre trabajos**: una continuación (200 000
  instrucciones) o una reanudación partía de registros viejos del DP: un
  DPC_START pendiente de su END (START_VALID) y DPC_CURRENT se perdían y el
  microcódigo xbus esperaba a que CURRENT alcanzara su puntero. El
  resultado del trabajo trae los cuatro registros y `collect` los vuelca.
- **DMA de muchas filas**: S2DEX carga fondos con DMAs de cientos de filas
  (`count`) con un salto (`skip`), y el SPE hacía una transferencia
  síncrona por fila (~2 µs): 150 filas → 300 µs por DMA, miles por tarea,
  segundos por tarea (se veía como «se atora a menos de un cuadro por
  segundo»). Ahora el tramo que abarcan todas las filas se trae de una vez
  si cabe en 32 KB.
- **Instrumentación** que lo hizo visible: contadores de accesos a COP0
  por registro en el intérprete (`c0_reads/c0_writes`, portables), que el
  host resume al salir y el SPE devuelve por trabajo, y un **latido** del
  SPE cada 4096 instrucciones (PC, $1–$4, contadores) en `lle_log`, que el
  PPE imprime cuando un trabajo lleva más de un segundo en vuelo.

**Lo que queda abierto**: tras los fondos de la intro (~36 s de consola)
el microcódigo entra en un bucle que no termina ninguna tarea: 935 M
instrucciones en 5 168 trabajos, DMAs de entrada con punteros que crecen de
64 en 64 bytes desde 0x81FFFFC0 (fuera de la RDRAM), semáforo tomado y
suelto miles de veces, sin lecturas de SP_STATUS. En el host la misma
escena pasa (4 036 tareas en 43 s de consola, sin atascos), así que es
una divergencia del camino del SPE, muy probablemente en las fronteras
entre trabajos (reinicio de la E/S, recarga de DMEM/IMEM y de los
registros del DMA, o el estado del DP) y no en el intérprete. Siguiente
paso recomendado: un **modelo del protocolo de trabajos del SPE en el
host** (correr `lle_spu_run` con memcpy en vez de DMA, con los mismos
cortes por `max_instr`) para reproducirlo con trazas de COP0 y DMEM, sin
turnos de consola.

## Plan: recompilador RSP → SPU

1. Unidad de compilación: bloques básicos de la IMEM (4 KB = 1024
   instrucciones; el microcódigo cambia por overlays con DMA a la IMEM, que
   invalidan los bloques del tramo escrito, como `recompiler.invalidate`
   en ares). Caché de bloques por hash del contenido para reusar overlays
   que vuelven.
2. Estado en registros: el SPU tiene 128 registros de 128 bits. Los 32
   vectores del RSP, los tres del acumulador y las banderas caben en
   registros fijos; los 32 escalares, en los carriles preferidos de otros
   registros (el SPU opera escalares en el carril 0 de un registro de 128
   bits). Nada de `s->r[i]` en memoria: eso es lo que paga el intérprete.
3. Vectorial: VADD/VSUB/VAND… son una instrucción del SPU (`a`, `sf`,
   `and` sobre halfwords); VMULF/VMUDx/VMADx se arman con `mpy`/`mpyhh`
   (16 × 16 → 32 por carril, pares e impares) y sumas en 32 bits, más la
   saturación con `cgt`/`selb`; la difusión de `vt(e)` es un `shufb` con
   16 patrones constantes; VRCP/VRSQ, por tabla en el local store (2 KB).
   Las cargas y almacenamientos vectoriales no alineados son `rotqby` y
   `shufb` desde la DMEM del local store.
4. Escalar y saltos: traducción directa (el SPU tiene `brz`/`brnz`,
   `hbr` para preparar el salto; las ramas cuestan si fallan el hint:
   convendrá enlazar bloques (saltos directos) y hint estáticos).
5. COP0 y DMA: llamadas a la E/S de io_spu.c, como hoy.
6. Expectativa: 2–6 instrucciones del SPU por instrucción del RSP, con
   emisión doble (par/impar) parcial → 300–800 M instrucciones del RSP por
   segundo: 5–12 veces el RSP real. Eso deja margen para que un SPE
   ejecute el RSP de cualquier juego en tiempo real.
7. Alternativa sin JIT en el SPE: el código SPU se genera en el PPE y se
   manda por DMA al local store (el SPE ejecuta lo que haya en su local
   store sin restricciones del sistema): evita escribir el generador en
   SPU y permite depurarlo en el host.

## Plan: RDP a bajo nivel en el RSX

Hoy los comandos crudos van al mismo rasterizado HLE que la display list
(`gfx_rdp_triangle` reconstruye los vértices de las aristas y los
coeficientes). Lo que falta para que un juego sin HLE se vea bien:

- Combiners y blender completos: el generador de fragment programs de la
  Fase 4 (`lib/rsxkit`); hoy los combiners se aproximan a una combinación
  lineal (`combine.c`).
- Cobertura, antialiasing y los modos de z del RDP (ZMODE, prim depth,
  dz) en el RSX; niebla por el blender.
- Efectos que leen el framebuffer (IM_RD, copias de la color image a la
  RDRAM): decidir por juego cuándo copiar RSX → RDRAM.
- Mover el consumidor al SPE de la HLE gráfica (ya compila `dpc.c`): hoy
  corre en el PPE al recoger cada trabajo y cuesta ms por cuadro en juegos
  grandes. Con el RSP en LLE en un SPE y el RDP en otro, la cadena RSP →
  RDP → RSX queda fuera del PPE.
- Las cuatro pruebas «RDP STATUS» de nemu64-test exigen que el RDP escriba
  el framebuffer en la RDRAM (relleno de un rectángulo): solo con un
  rasterizador por software del RDP (en un SPE) o copiando de vuelta.

## Recomendación

Seguir, con los pies en la tierra: el intérprete portable cubre el ISA
completo verificado contra hardware, corre microcódigo real de gráficos y
de audio en el host y en el SPE, y la integración (modos auto/all, RDP por
DPC, SPE con DMA atómico) está hecha; pero a 3,5–4,4 MIPS en el SPE está
14–18 veces por debajo del RSP real, y un intérprete mejor (despacho sin
saltos indirectos, acceso a la DMEM por palabras, caché local de SP_STATUS
y del semáforo con invalidación por el PPE) ganaría quizá 3–5×, no 15×.
Lo que llega a velocidad real es el recompilador RSP → SPU (plan arriba;
2–4 semanas con nemu64-test como red), que además elimina la mayor parte
del costo de los accesos a COP0 al convertirlos en código recto. Antes de
eso, cerrar la divergencia del SPE con el modelo del protocolo en el host.
Con eso, cualquier microcódigo (Rare, Factor 5, libdragon) corre sin HLE a
mano, y la HLE queda como camino rápido para lo conocido.

## Cómo probar

Host:

```bash
tools/build.sh host
build/host/salsa64-run --lle tests/roms/homebrew/nemu64-test.z64 -u 'success rate)'
build/host/salsa64-run --lle=all --lle-check-audio <rom>
```

`--lle-check-audio` compara lo que escribe cada tarea de audio en la HLE y
en el microcódigo real **cuando el microcódigo termina la tarea** (MusyX
deja su estado en la memoria del juego, que el juego reutiliza al terminar:
comparar más tarde da diferencias falsas) y al final cuenta las muestras de
16 bits distintas por tamaño de la diferencia. Variables de entorno:
`SALSA64_LLE_VERBOSE=1` (una línea por tarea), `SALSA64_LLE_DIFFS=1` (una
línea «D tarea tramo dirección +desplazamiento lle hle» por palabra
distinta y «T n» al empezar cada tarea), `SALSA64_LLE_DUMP_TASK=N` (de esa
tarea, lo escrito antes/LLE/HLE y su lista de comandos o las cabeceras de las
subtramas de MusyX) y `SALSA64_LLE_DUMP_DMEM=archivo` (con la anterior, la DMEM
del RSP al terminar). Una tarea que escribe más de 10 tramos (MusyX) pide
compilar el host con `make -C src/host EXTRA_CFLAGS=-DAUD_MAX_RANGES=64`.

Para el censo de docs/audio-censo.md el host se compila con
`-DAUD_MAX_RANGES=1024`. **Sin eso, el oráculo da falsos positivos:** con los
10 tramos del resultado de spukit, una tarea que escribe más de 10 tramos los
junta por el hueco menor y compara también los huecos, que el CPU del juego
puede haber escrito mientras la tarea corría (en el motor de libultra con
reverberación, como Donald Duck: Quack Attack, con ~30 tramos por tarea, salen
«distintas» 1302 de 1649 tareas que con 1024 tramos son 1 de 3855). Con
`-DAUD_MAX_RANGES=1024` (el host lleva `-DAUD_DEBUG`: guarda las direcciones
de los estados de ENVMIXER para no comparar la parte fraccionaria que el
microcódigo deja sin limpiar) y el oráculo arranca el microcódigo antes de
la HLE (sin concurrencia en el arranque), pone a cero los registros
vectoriales, le da la DMEM de antes de la tarea a la HLE y no compara los 8
bytes de basura que el POLEF de los naudio con MP3 guarda delante de su búfer.
Además de las de arriba, la bisección y el banco de pruebas:

- `SALSA64_LLE_BISECT=A[-B][,…]`: para cada una de esas tareas, un proceso
  hijo por k = 1, 2, … comandos la corre cortada a k (el microcódigo lee
  `data_size` de la cabecera) en el microcódigo y en la HLE y compara la DMEM
  (ventana `SALSA64_LLE_DMWIN=lo-hi`, en hexadecimal) y los tramos de la
  RDRAM: el primer k distinto señala el comando donde se separan («primer
  comando distinto n» es el comando de índice n, desde 0). Al final cuenta, por
  ABI, el primer opcode que difiere. `_STEP=n` (k de n en n), `_FROM=k`
  (empezar en k) y `_NOP=1` (poner a cero los comandos que sobran en vez de
  cortar la lista, para el microcódigo de Zelda, que lee la lista por bloques).
- `SALSA64_LLE_POKE=dir=valor[,…]` (hex; 16 o 32 bits según los dígitos): en el
  hijo de la bisección, escribir la RDRAM de los dos lados antes de la tarea:
  cambiar un comando de la lista, una tabla o un estado y ver qué hace el
  microcódigo con ello sin volver a correr el juego (así se midió de dónde toma
  POLEF las muestras previas con A_INIT).
- `SALSA64_LLE_SHOWK=k:dir:bytes` y `SALSA64_LLE_SHOWR=k:dir:bytes`: en el hijo
  de k comandos, la DMEM (con la memoria de trabajo del MP3 en 0x800–0xFFF) o
  la RDRAM de los dos lados; `SALSA64_LLE_DUMP_RANGE=dir:bytes` (con DUMP_TASK)
  la RDRAM antes, del microcódigo y de la HLE.
- `SALSA64_LLE_SNAP=prefijo[:N]` (la tarea de audio N) y
  `SALSA64_JCHECK_SNAP=prefijo[:N]` (la tarea JPEG N): la RDRAM en
  `prefijo.ram` y la DMEM/IMEM con el PC en `prefijo.mem`. Con eso, un banco de
  pruebas (fuera del repositorio: lee el microcódigo del respaldo del autor)
  corre el RSP real sobre listas o macrobloques sintéticos y los compara con
  la HLE, que es como se midió cada comando de docs/audio-censo.md.
- La línea «lle-check: ABI nombre: N tareas, M distintas» al final de cada
  corrida. `SALSA64_JCHECK_DUMP=archivo` guarda la entrada y la salida real de
  las tareas JPEG para probar otra IDCT sin volver a correr el juego.

PS3: paquete «Salsa64 LLE» (SLSA00082); en `SALSA64_DATA_DIR` un archivo
`lle` con `all` (todo el microcódigo real) o `auto`; `embedded` con
`file:<rom>` como siempre. Mirar las líneas «rsp lle:» del log.
