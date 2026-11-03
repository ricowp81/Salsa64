# Estados guardados (save states)

Al cerrar un juego se espera y detiene el escritor antes de liberar la foto
(8 MiB de RDRAM + 1 MiB de secciones). Si vence el plazo, la foto se conserva
hasta que termine la escritura. `memoria-estados-vieja` en la carpeta de datos
restaura la retención entre juegos para comparar el consumo en la PS3.

Toda la máquina en un archivo, para seguir exactamente desde ahí. Diez
ranuras por juego en la PS3; en el host, `salsa64-run --state-save` y
`--state-load`. Decidido y hecho el 2026-10-11.

| Pieza | Dónde |
|---|---|
| Formato, guardar y cargar (portable) | `src/core/state.h`, `src/core/state.c` |
| Recoger la tarea del RSP en vuelo | `salsa64_rsp_quiesce` (`src/core/rsp.c`) |
| Estado de la HLE gráfica entre tareas | `src/rsp/gfx/state.c` (registros del RDP) y `sync.c` (con la TMEM) |
| Estado de la HLE de audio entre tareas | `src/rsp/audio/state.c` |
| En los SPE | trabajos `GFX_JOB_STATE_*` (el SPE copia tal cual el tramo del RDP; el PPE arma el formato, `src/platform/ps3/gfx_spu.c`) y `AUD_JOB_STATE_*` (`src/spu/gfx/main.c`, `src/spu/audio/main.c`) |
| Archivos de las ranuras y miniatura (PS3), foto y escritura en segundo plano | `src/frontend/states.c`, `src/platform/ps3/bgjob.c` |
| Compresor LZ (formato de bloque de LZ4), portable y en el SPE | `lib/spukit/include/spukit/lz.h`, `lzstream.h`, `lib/spukit/spu/lz.c` |
| Barrera entre los dos hilos, atajos, aviso | `src/frontend/main.c` (`emu_park`, `state_barrier`, `pad_combos`), `src/frontend/osd.c` |
| Host | `src/host/run.c` (`--state-save CAMPO:ARCHIVO`, `--state-load ARCHIVO`, `--fields N`) |
| Pruebas | `tests/unit/test_state.c`, `tools/test-roms.sh` (`estados`), `tools/test-rec.sh` |

## Qué se guarda

Lo que hace falta para continuar ciclo a ciclo:

- **CPU0** — VR4300: GPR, HI/LO, ciclos, PC, siguiente PC, retardo (delay
  slot y salto), LLbit, todo COP0 (con Count/Compare como desplazamiento de
  Count y el ciclo de referencia de Random), la TLB de 32 entradas, los 32
  registros de la FPU, FCR31, el latch de COP2 y los ciclos saltados en
  bucles de espera (el RSP los compara entre sondeos).
- **SCHD** — el tiempo de cada evento del planificador (Compare, VI,
  fin de tarea del RSP, DP, AI). Los números de `enum salsa64_event` quedan
  fijos: uno nuevo va al final.
- **RCP0** — MI, registros del SP y su PC, la tarea del RSP en curso (tipo,
  microcódigo, ABI, si ya se recogió y con qué resultado), DP, VI (registros,
  reloj, línea, campo, campos, inicio y duración del campo), AI (registros,
  cola de dos DMA, acarreo, lo que falta del DMA en curso), PI, RI, SI y los
  registros de la RDRAM.
- **SPME** — DMEM e IMEM (8 KB).
- **PIF0** — los controles como los vio el PIF, las pulsaciones retenidas,
  los intercambios preparados y la PIF RAM.
- **CART** — el guardado del cartucho en memoria (EEPROM, SRAM o FlashRAM).
- **FLSH** — solo con FlashRAM: una versión propia (1) y el estado del chip
  (modo, registro de estado, borrado preparado, búfer de página). Una
  versión que no se conoce, o un estado sin FLSH, deja el chip como al
  encender; los datos de CART se cargan igual.
- **HLEG** — lo que la HLE gráfica conserva entre tareas, como el RDP de la
  consola: othermode, combiner, colores, scissor, color/z/texture image,
  las 8 tiles y la TMEM (4 KB).
- **HLEA** — lo que la HLE de audio conserva entre tareas (como
  mupen64plus-rsp-hle): su DMEM con el margen, el estado de ABI1, nead y
  naudio (libro de ADPCM, envolventes, volúmenes) y la ventana de síntesis
  del MP3.
- **THMB** — miniatura de 160 × 120 (RGBA5551) de la imagen presentada, para
  un futuro menú de estados; va primero, el menú no tiene que leer el resto.
- **RDRM** — la RDRAM entera (4 u 8 MB), sin comprimir, al final.
- **RDRZ** — (formato 2) en lugar de RDRM: la RDRAM comprimida. Tres palabras
  (tamaño de la RDRAM, tamaño de bloque —64 KB— y número de bloques) y los
  bloques uno detrás de otro, cada uno con un encabezado de 4 bytes (el bit 31
  dice que ese bloque va tal cual porque comprimirlo no ganó, el resto es su
  largo). Los bloques son del formato de bloque de LZ4 y no dependen entre sí.

No se guarda lo que no cambia el resultado o se recalcula: punteros y
backends, configuración (duraciones de las tareas del RSP), estadísticas,
las tablas de bucles de espera descartados (solo velocidad), modo y
permisos del CPU (salen de Status) y la caché del recompilador (se vacía al
cargar).

## Formato

Big-endian y campo por campo, igual en la PS3 y en el host: un estado de la
consola se reproduce en `salsa64-run` y al revés.

- Cabecera de 64 bytes: magia `SLS64ST\0`, versión del formato (1: RDRM sin
  comprimir; 2: RDRZ, lo que escribe la PS3; se leen las dos), tamaño de la
  cabecera y del archivo, CRC1 y CRC2 de la cabecera de la ROM, tamaño de la
  ROM, título (20 bytes), tamaño de la RDRAM, campo del VI y banderas.
- Secciones «etiqueta (4 bytes) + largo (4 bytes) + datos».
- Cada sección se describe con una sola función para los dos sentidos
  (`sec_cpu`, `sec_rcp`…). Una sección más corta que lo que espera la
  versión que carga deja el resto en cero: así se agregan campos al final
  sin romper los estados viejos. Las etiquetas desconocidas se saltan.
- Un cambio incompatible sube `SALSA64_STATE_VERSION`. Los estados con una
  versión anterior a `SALSA64_STATE_MIN_VERSION` o posterior a la del
  programa se rechazan con un mensaje claro; también los de otro juego (CRC)
  y los de otro tamaño de RDRAM.
- Al cargar, la cabecera se valida antes de tocar nada, las secciones chicas
  se leen a un búfer de paso de 128 KB y se aplican solo después de leer la
  RDRAM entera. Si la lectura falla a mitad de la RDRAM la máquina queda
  inservible: en la PS3 se avisa y se cierra el juego.
- Los estados de las HLE son bloques del formato de cada backend (`state_size`
  y `save_state`/`load_state` en `salsa64_gfx_backend` y
  `salsa64_audio_backend`); si el tamaño no coincide, la HLE sigue con lo que
  tenía (casi siempre da igual: los juegos ponen el estado del RDP en cada
  cuadro).

En la PS3, guardar es en dos tiempos (más abajo) y la RDRAM va comprimida
(formato 2): 8 MB pasan a 1–2 MB en los juegos medidos en el host (OoT en la
intro 4:1, GoldenEye 7:1, Mario Kart 6:1; gfxtest 100:1). En el host,
`salsa64-run --state-save` sigue escribiendo el formato 1 (los archivos
iguales byte por byte de las pruebas de determinismo) y `--state-zip` el 2;
las dos clases se cargan en los dos lados. Antes de agregar la compresión
(2026-10-13) guardar tardaba ~0,43 s con la emulación detenida (casi todo el
disco) y cargar ~0,17 s (docs/perf-log.md).

La E/S en la PS3 va con las llamadas al kernel (`sysLv2Fs*` de
`<sys/file.h>`), no con `sysFs*` de libsysfs: esas son puentes al módulo
cellFs del sistema, que hay que cargar antes; sin cargarlo, en RPCS3 andan
y en la consola real colgaban todo al primer `mkdir`. Tamaño en disco: ~8,1 MB por ranura
(RDRAM de 8 MB más ~70 KB de secciones y miniatura), ~81 MB por juego con
las diez ranuras llenas (docs/memory-budget.md).

## Guardar en dos tiempos y en segundo plano (2026-10-13)

Lo único que necesita la emulación quieta es **tomar una foto**; escribirla
no. `salsa64_state_capture` arma la cabecera y las secciones chicas en un
búfer de 256 KB y copia la RDRAM (8 MB, un `memcpy`) a otro búfer, los dos
reservados al abrir el juego (`states_init`; docs/memory-budget.md). La
emulación sigue ahí mismo. Después, un hilo del PPE de baja prioridad
(`bgjob`, prioridad 1500: por debajo de la emulación y la presentación)
escribe el archivo con `salsa64_state_write`:

1. Las secciones chicas tal cual y el encabezado de RDRZ.
2. La RDRAM, bloque por bloque: el SPE libre (`spukit_lzs`, programa
   `lib/spukit/spu/lz.c`) comprime hasta ocho bloques por delante de lo que
   el hilo escribe; el SPE solo hace DMA, así que la E/S es del hilo
   (`sysLv2Fs*`, nunca `sysFs*`). Sin SPE, o si su resultado no coincide con
   el del PPE en la prueba que corre la primera vez (`lz_selftest`), comprime
   el hilo.
3. Los dos tamaños que solo se saben al final (el del archivo y el de RDRZ)
   se escriben al empezar con `sysLv2FsLSeek64`; el archivo era un `.tmp` y
   se renombra, igual que antes.

Reglas: una sola foto, así que un segundo guardado espera a que termine el
anterior (hasta 5 s; el aviso sale igual); cargar y consultar las ranuras
esperan la escritura en curso; al cerrar el juego se termina de escribir
antes de soltar el SPE. Si la escritura falla (disco lleno…), el aviso
«No se pudo guardar» sale cuando el hilo lo sabe (`states_poll`), y el estado
anterior de esa ranura sigue intacto. Sin memoria para la foto o sin hilo, se
guarda como antes, todo seguido.

El log de la PS3 trae dos líneas por estado: `estado: ranura N (campo C):
emulación detenida X ms (la foto)…` y, al terminar la escritura, `estado: ranura N
escrita en segundo plano: … ms de punta a punta, … ms en el disco, … ms del SPE
comprimiendo; K KB de M KB (r:1)…`.

## Cuándo es seguro: la barrera

Solo en el límite de un campo y con la emulación quieta:

1. Presentación (SELECT + R1 o L1) deja la petición (`state_req`).
2. Emulación, al terminar el campo, recoge la tarea del RSP en vuelo
   (`salsa64_rsp_quiesce`: espera al SPE gráfico o de audio y guarda el
   resultado), encola el campo marcado (`FIELD_STATE`) y se queda esperando
   (`emu_park`, dormida).
3. Presentación presenta lo que quedaba en la cola (del campo marcado sale
   la miniatura, antes de dibujar el aviso), traduce todas las tareas
   gráficas, espera al RSX (`rsxkit_finish`), espera las copias RSX → RDRAM
   de los efectos de framebuffer que quedaran en vuelo y las escribe
   (`salsa64_rdp_settle`: ninguna llega tarde sobre una RDRAM cargada) y
   toma la foto (el archivo se escribe después, en segundo plano) o carga.
4. Al cargar: la caché del recompilador se vacía (`salsa64_rec_flush`, dentro
   de `salsa64_state_load`), los destinos del RSX se sueltan
   (`salsa64_rdp_drop_targets`: lo dibujado ya no es de esta máquina y el
   número de campo retrocede; también el scratch, los espejos y el historial
   de copias; las colas y la numeración de las tareas siguen, porque el SPE
   es el mismo), la cola de audio se vacía con su transición
   (`audio_out_flush`) y se olvidan el último campo encolado y las ventanas
   que cuentan campos. El VI se relee solo en el campo siguiente.
5. Presentación baja la petición; emulación sigue y reinicia el ritmo.

**La tarea recogida no adelanta su fin.** La tarea del RSP «dura» un tiempo
emulado (rsp.c); recogerla antes solo adelanta el trabajo del backend: su
fin llega a la misma hora con el resultado guardado (`collected`), sin
volver a llamar al backend. Por eso el estado se puede guardar con una
tarea a medias y la continuación es idéntica.

Funciona con el intérprete y con el recompilador y con los dos hilos del
PPE. Con el RSP a bajo nivel (`lle`) no hay estados: el microcódigo corre en
un SPE sin forma de pararlo en un punto; se avisa.

## Determinismo: la prueba

De corrido hasta el campo M contra guardar en N, cargar en otra corrida y
seguir hasta M: los dos estados en M iguales byte por byte (CPU, RDRAM, RCP,
planificador, HLE) y las mismas imágenes (VI y HLE gráfica).

- `tests/unit/test_state.c`: un programa de prueba con la máquina de las
  unitarias (también con el recompilador en `tools/test-rec.sh`).
- `tools/test-roms.sh`: gfxtest (150 → 300, con la imagen de la HLE),
  vitest (120 → 240) y audiotest (100 → 200).
- `tools/test-rec.sh`: gfxtest con `--rec`, comparado además con el
  intérprete; con `--lockstep`, también la corrida que arranca de un estado.
- Con respaldos propios (fuera del repositorio, 2026-10-11): Zelda OoT
  (intro 1500 → 1800 y Kokiri 3400 → 3600), Super Mario 64 (900 → 1200 y el
  castillo 4100 → 4300), GoldenEye (1200 → 1500 y la presa, código mapeado
  por la TLB, 7200 → 7500) y Mario Kart 64 (900 → 1200): todo idéntico.
  Con los efectos de framebuffer (el rasterizador en las tres corridas: las
  copias RSX → RDRAM son parte del estado): la pausa de Zelda (16090 →
  16300, `zelda-pausa-nueva.pad`, con las copias de la pausa y el filtro del
  CPU), el túnel de Mario Kart (3000 → 3300, `mk64-luigi.pad`), la presa de
  GoldenEye, y SM64 del logo a la cara de Mario (300 → 500) y en el
  castillo: idénticos.

En la PS3 la emulación no es determinista ciclo a ciclo (los SPE terminan
cuando terminan y el fin de tarea se sondea), así que allí se prueba que
guardar y cargar funcione, no que dos corridas coincidan.

## Lo que no entra en un estado

Los píxeles que solo están en el RSX. Con los efectos de framebuffer, lo
que el RDP dibuja vuelve a la RDRAM por las copias (docs/framebuffer.md);
la barrera espera las que estén en vuelo. Lo que todavía no se copió
porque espera su momento (un framebuffer dibujado por primera vez se copia
cuando el VI lo presenta) se copia después de guardar, como siempre, pero
no queda en el archivo: al cargar ese estado, esa copia no llega (un cuadro
del framebuffer con lo que había en la RDRAM). La marca de «ya copiado» de
cada destino se reinicia al cargar (`drop_targets`), así que nada se copia
dos veces. Pasa al empezar un juego o cuando cambia de framebuffers; en los
juegos medidos, guardar en medio del juego da estados idénticos.

## Lo que se ve al cargar

- La imagen del primer campo puede salir negra: los destinos del RSX se
  soltaron y la RDRAM no tiene lo que dibujó la HLE; el campo siguiente ya
  lo dibuja el juego.
- El guardado del cartucho vuelve al del momento del estado y se escribe al
  disco con el ritmo de siempre (es lo que el juego cree que tiene; si no,
  podría corromperlo).
- El sonido que esperaba en cola se desvanece y sigue lo del estado.
