# Audio y JPEG contra el microcódigo real: censo (2026-10-17)

La HLE de audio (`src/rsp/audio`) viene de mupen64plus-rsp-hle, que en varios
lugares trunca donde el microcódigo real redondea y que nunca se probó contra
el microcódigo. Este censo compara, tarea por tarea y byte por byte, lo que
escribe la HLE con lo que escribe el RSP real (el LLE de `src/rsp/lle`
ejecutando el microcódigo del propio juego), la corrige donde difiere y deja
anotado lo que no puede corregir. La tarea JPEG (`src/rsp/jpeg`) se midió igual.
Resultado: de 51 136 tareas de audio en 21 juegos, **13,1 % idénticas
antes y 99,87 % después**; la tarea JPEG (Zelda OoT, Pokémon Stadium 1 y 2),
0 píxeles distintos de 1 075 200.

## Método

`salsa64-run --lle=all --lle-check-audio <rom>`: cada tarea de audio corre en el
RSP real y, sobre una copia de la RDRAM del arranque de la tarea y con la DMEM
del RSP de antes de la tarea, en la HLE; al terminar la tarea se comparan todos
los tramos de la RDRAM que escribió cada lado (docs/rsp-lle.md). Para hallar
el comando donde se separan, `SALSA64_LLE_BISECT` corre la tarea cortada a k
comandos, uno por uno, y compara la DMEM y la RDRAM de cada corte; para medir
un comando, un banco de pruebas (fuera del repositorio: lee el microcódigo del
respaldo del autor) corre el RSP real sobre listas sintéticas y las compara con
la HLE, y para entender una rutina se traza y desensambla en el LLE. Nada del
microcódigo ni de los juegos entra al repositorio: las tablas que el RSP lee de
su segmento de datos (cambio de tono) la HLE las toma en ejecución de la RDRAM
del propio juego.

- Host: `-DAUD_MAX_RANGES=1024`; los juegos corren con pulsaciones programadas
  (`--pad`) para que haya música y efectos: START y A alternados cada 60
  campos (200:START;206:-;260:A;…) en la mayoría y los guiones de `docs/pads/`
  en Super Mario 64 (castillo), GoldenEye (Dam), Mario Kart 64 (Luigi), Zelda OoT
  (Kokiri), Pokémon Stadium (versus) y Banjo-Kazooie (casa). 4 a 7 mil millones
  de ciclos por juego (50 a 75 s de consola).
- «Antes» es la HLE del commit `a104e00` con el mismo oráculo; «después», la de
  esta rama.

## Resultado por variante y juego

| Juego | Variante (ABI) | Tareas | Idénticas antes | Idénticas después |
|---|---|---:|---:|---:|
| Super Mario 64 (USA) | ABI1 | 3 809 | 450 (11,8 %) | 3 809 (100,0 %) |
| Wave Race 64 (USA) | ABI1 | 2 516 | 190 (7,6 %) | 2 516 (100,0 %) |
| Diddy Kong Racing (USA, rev 1) | ABI1 (Blast Corps) | 1 275 | 93 (7,3 %) | 1 274 (99,9 %) |
| GoldenEye 007 (USA) | ABI1 (GoldenEye) | 1 898 | 130 (6,8 %) | 1 898 (100,0 %) |
| Mario Kart 64 (USA) | nead (Mario Kart 64) | 3 802 | 266 (7,0 %) | 3 802 (100,0 %) |
| Star Fox 64 (USA, rev 1) | nead (Star Fox 64) | 2 010 | 0 (0,0 %) | 2 008 (99,9 %) |
| F-Zero X (USA) | nead (F-Zero X) | 2 547 | 0 (0,0 %) | 2 547 (100,0 %) |
| Yoshi's Story (USA) | nead (Yoshi's Story) | 2 548 | 488 (19,2 %) | 2 548 (100,0 %) |
| 1080° Snowboarding | nead (1080°) | 2 531 | 84 (3,3 %) | 2 528 (99,9 %) |
| Zelda: Ocarina of Time (rev B) | nead (Zelda OoT) | 4 218 | 39 (0,9 %) | 4 218 (100,0 %) |
| Zelda: Majora's Mask (USA) | nead (Zelda MM) | 3 163 | 2 560 (80,9 %) | 3 163 (100,0 %) |
| Pokémon Stadium 2 (USA) | nead (Zelda MM) | 2 550 | 173 (6,8 %) | 2 550 (100,0 %) |
| Kirby 64 (USA) | naudio | 2 552 | 730 (28,6 %) | 2 552 (100,0 %) |
| Paper Mario (USA) | naudio | 2 550 | 756 (29,6 %) | 2 550 (100,0 %) |
| Super Smash Bros. (USA) | naudio | 2 552 | 127 (5,0 %) | 2 549 (99,9 %) |
| Pokémon Stadium (USA) | naudio | 2 553 | 46 (1,8 %) | 2 552 (100,0 %) |
| Banjo-Kazooie (USA) | naudio (Banjo-Kazooie) | 2 219 | 9 (0,4 %) | 2 217 (99,9 %) |
| Donkey Kong 64 (USA) | naudio (Donkey Kong 64) | 1 249 | 34 (2,7 %) | 1 249 (100,0 %) |
| Perfect Dark (USA) | naudio con MP3 (Perfect Dark) | 1 416 | 117 (8,3 %) | 1 363 (96,3 %) |
| Banjo-Tooie (USA) | naudio con MP3 (Perfect Dark) | 1 586 | 27 (1,7 %) | 1 586 (100,0 %) |
| Conker's Bad Fur Day (USA) | naudio (Conker) | 1 592 | 389 (24,4 %) | 1 592 (100,0 %) |

El primer comando distinto de las tareas de «antes» (bisección de 12 tareas por
juego a partir de la 120) y de lo que queda en «después», por variante:

**Antes**

- ABI1 — RESAMPLE (05): 24
- ABI1 (Blast Corps) — SEGMENT (07): 12
- ABI1 (GoldenEye) — SEGMENT (07): 12
- naudio — CLEARBUFF (02): 48
- naudio (Banjo-Kazooie) — CLEARBUFF (02): 12
- naudio (Conker) — LOADBUFF (04): 7, MP3ADDY (08): 3, CLEARBUFF (02): 2
- naudio (Donkey Kong 64) — CLEARBUFF (02): 12
- naudio con MP3 (Perfect Dark) — CLEARBUFF (02): 24
- nead (1080°) — CLEARBUFF (02): 12
- nead (F-Zero X) — CLEARBUFF (02): 12
- nead (Mario Kart 64) — CLEARBUFF (02): 12
- nead (Star Fox 64) — ADDMIXER (04): 9, RESAMPLE (05): 3
- nead (Yoshi's Story) — CLEARBUFF (02): 12
- nead (Zelda MM) — CLEARBUFF (02): 24
- nead (Zelda OoT) — CLEARBUFF (02): 12

**Después**

- ABI1 (Blast Corps) — POLEF (0e): 1
- naudio — ADPCM (01): 3, DMEMMOVE (0a): 1
- naudio con MP3 (Perfect Dark) — ADPCM (01): 50, DMEMMOVE (0a): 1
- nead (1080°) — CLEARBUFF (02): 3
- nead (Star Fox 64) — DMEMMOVE (0a): 1, MIXER (0c): 1

## Lo que se corrigió y por qué

**Todas las variantes**
- *Arranque de la tarea.* El arranque del RSP copia `ucode_data` a la DMEM
  [0, tamaño) antes de cualquier comando; la HLE lo hace igual
  (`aud_boot_data`): los comandos que leen constantes o datos viejos de esa
  zona ven lo mismo.
- *Cambio de tono (RESAMPLE).* La tabla de interpolación (64 × 4) está en el
  `ucode_data` del juego; la HLE la busca ahí (`aud_find_lut`, la reconoce por
  su estructura: la fila i es la 63−i al revés y suma 1,0) y cada producto se
  redondea por separado, como el RSP; mupen trunca.
- *Mezcla (MIXER, ENVMIXER).* El microcódigo acumula con VMACF: lo que ya había
  se multiplica por 0x7FFF y se suma el producto con un solo redondeo,
  `dst · 0x7FFF + src · ganancia + 0x4000 >> 15`; sumar el producto redondeado
  a dst deja ±1.

**ABI1 (Super Mario 64, Wave Race 64, Blast Corps/Diddy Kong Racing, GoldenEye)**
- *ENVMIXER* con el estado de 80 bytes del microcódigo (cinco vectores de ocho
  carriles: volumen izquierdo alto y bajo, derecho alto y bajo, y los
  parámetros), no el propio de 40: rampa exponencial (multiplica cada carril
  por la tasa) en ABI1 y en la compilación de GoldenEye, constantes de los
  carriles al iniciar (j+1)·0x2000 y 0xFFFF en el último, recorte hacia el
  destino según el signo de la tasa con umbral 0x10000 y saturación a 32 bits.

**nead (Mario Kart 64, Star Fox 64, F-Zero X, Yoshi's Story, 1080°, Zelda OoT y MM, Stadium 2)**
- *ENVMIXER* redondea la cuenta a múltiplos de 16 muestras (8 en Mario Kart 64): el
  microcódigo procesa dos bloques por vuelta.
- *MIXER seguido de ADDMIXER:* el acarreo (VCO) del MIXER suma 1 en el carril 3
  del primero del ADD; RESAMPLE lo borra (`alist_add`).
- *FILTER:* la tabla de la nota se promedia con la de la llamada anterior
  (guardada detrás del estado) en Zelda; Yoshi's Story y 1080° la usan tal cual.
  A_INIT pone a cero la tabla anterior y las muestras previas.

**naudio (Banjo-Kazooie, Donkey Kong 64, Kirby 64, Paper Mario, Smash, Stadium)**
- *ENVMIXER* lineal con el estado de 80 bytes; en Donkey Kong 64 y en los de MP3 el bit
  bajo de seco o húmedo invierte la entrada de ese canal.
- *Búferes* a 0x500 (no 0x4F0) en los microcódigos con MP3.

**naudio con MP3 (Perfect Dark, Banjo-Tooie) y Conker**
- *IIR:* el estado de 8 bytes es x[n−2], x[n−1], y[n−2], y[n−1] en +0, +2, +4 y +6
  (mupen los leía y escribía en +4 con otro orden); el IIR ignora el byte alto
  de w2 y filtra siempre desde 0x500 más los 12 bits bajos de w0.
- *POLEF:* guarda en los 16 bytes anteriores a su búfer un vector inicial (ceros
  con A_INIT; si no, 8 bytes de un registro vectorial que dejó el comando
  anterior y 8 de estado): con el segundo búfer pisa las 8 últimas muestras
  del principal. Se emulan los que se pueden y el oráculo no compara los 8
  bytes de basura.
- *MP3ADDY* (comando 8; un no-op en mupen): fija la dirección del búfer de
  estado de la corriente. Cada comando MP3 trae de ahí los 0x440 bytes del
  anillo de la ventana (DMEM 0x8A0–0xCDF) y los guarda al terminar, porque
  los búferes de naudio ocupan la misma zona de la DMEM y la pisan entre dos
  MP3. Es lo que hacía que las tareas de música de Conker salieran
  distintas (y permite varias corrientes). La dirección se pierde con la
  cabecera de la tarea: cada tarea empieza con su MP3ADDY.
- *Conker, ENVMIXER:* la muestra se multiplica (VMULF) por la polaridad de cada
  canal, 0x7FFF o 0x8000 según el bit bajo de seco o húmedo; en el primer
  bloque de 8 muestras pasa antes por 0x7FFF (escala 0x7FFE o 0x8002: ±1 en
  esas muestras) o, con el bit 1 en uno, por el vector de rampa de entrada
  (0xA0 del segmento de datos).

## JPEG (Zelda OoT, Pokémon Stadium 1 y 2)

La IDCT y la conversión a RGBA5551 se rehicieron con la aritmética del
microcódigo (`src/rsp/jpeg/jpeg.c` y la versión de 4 carriles `jpeg_v.c`, que
dan lo mismo byte a byte): mitades par e impar con las constantes
round(32768 · cos(kπ/16)), productos de dos en dos con un solo redondeo,
sumas saturadas a 16 bits, filas y luego columnas (que salen (a ± b + 2) >> 2);
el color suma la parte entera de cada constante y multiplica la fraccionaria
(Q16) con el piso del producto. Antes: 99,0–99,2 % de los píxeles iguales (el
resto a ±1 en un canal de 5 bits).

| Juego | Tareas (macrobloques) | Píxeles | Distintos antes | Distintos después |
|---|---:|---:|---:|---:|
| Zelda OoT (Kokiri) | 75 (300) | 76 800 | 597 (0,78 %) | 0 |
| Pokémon Stadium | 900 (900) | 230 400 | 2 546 (1,10 %) | 0 |
| Pokémon Stadium 2 | 3000 (3000) | 768 000 | 7 372 (0,96 %) | 0 |

Además, 0 píxeles distintos en más de 400 000 de macrobloques sintéticos
(continuos, un solo coeficiente, aleatorios y con saturación) corridos en el
RSP real. La prueba del arranque (SPE = PPE = host) cambia de CRC: 0xF5C74ACF.

## Lo que queda (65 tareas de 51 136)

- **Perfect Dark (53), Super Smash Bros. (3) y Pokémon Stadium (1):** ADPCM con
  A_LOOP. El estado del bucle (32 bytes junto al estado de la nota) lo reescribe
  el CPU del juego mientras la tarea corre: el RSP real lee los datos nuevos
  (en Smash, el RSP no hace ningún DMA a esa dirección en toda la corrida) y la HLE,
  que lee la RDRAM al arrancar la tarea, los viejos. En Perfect Dark son
  siempre los mismos 16 valores. No es un error de la aritmética; arreglarlo
  exigiría que la HLE leyera la RDRAM más tarde.
- **1080° (3):** tareas cuya DMEM de partida difiere entre el RSP real y la
  HLE desde el primer comando; sin explicar.
- **Banjo-Kazooie (2), Star Fox 64 (2):** unas pocas muestras (1 a 3) ±1 que no se
  reproducen en una segunda corrida o no se localizan a un comando: carreras con
  el CPU.
- **Diddy Kong Racing (1):** la primera tarea de audio del juego (POLEF de ABI1,
  712 muestras −1 contra 0); el POLEF de ABI1 sale idéntico en el banco de
  pruebas con datos aleatorios y con ceros, así que depende del estado del
  juego al arrancar.
- **MP3:** solo Conker lo mandó en las escenas del censo (342 comandos en 269
  tareas: todas idénticas desde el MP3ADDY); Perfect Dark y Banjo-Tooie no
  mandaron ninguno, así que su camino de MP3 (el mismo código) queda medido
  únicamente por Conker.
- **JPEG:** nada.

## Relación con mupen64plus-rsp-hle

La rama master sigue en el commit `8a7a472a` (2026-06-23), el que se tomó como
base: desde entonces solo hay cambios de CI, los límites de `hle_execute()`
(2025-09) y la corrección para Pokémon Stadium 2 de mupen64plus-core #1130
(2025-05). Ninguno toca la aritmética de audio ni el JPEG, así que nada de lo de
arriba está corregido allá (los redondeos, el estado del ENVMIXER, el IIR y el
POLEF de naudio, el MP3ADDY, la IDCT y el color). Cuando un cambio de aquí
pueda servirles, el hallazgo está en este documento con su medida; el código
de Salsa64 queda con los avisos de copyright originales (docs/THIRD_PARTY.md).

## Local store del SPE de audio (SPE 1)

| | antes (2026-10-16) | después |
|---|---:|---:|
| text | 59 296 | 62 288 |
| data | 800 | 800 |
| bss | 34 960 | 35 088 |
| total | 95 056 | 98 176 |
| libre para la pila | ~167 KB | 163 808 bytes |
| pila de la cadena más honda (`tools/spu-stack.py`) | 8,4 KB (MusyX v2) | 8,4 KB (la misma) |

Las DMA nuevas (el anillo del MP3, 0x440 bytes en cada sentido por comando) son
múltiplos de 16; no hay reservas dinámicas en el bucle. La pila del ENVMIXER de
naudio usa 1,8 KB y la del MP3 0,8 KB. Hay que validar en la PS3 que el JPEG del
SPE (ahora con 128 reordenamientos por bloque en vez de la IDCT con
constantes repetidas) sigue por debajo del milisegundo por macrobloque.

## Pruebas

- Unitarias nuevas o cambiadas (`tests/unit/test_audio.c`, `test_jpeg.c`):
  estado y rampas del ENVMIXER de ABI1, fases de naudio, base 0x500, FILTER directo y
  promediado, acarreo del ADD, copia del segmento de datos, tabla de cambio de
  tono del juego, IIR y POLEF con el estado del microcódigo, ENVMIXER de Conker,
  anillo del MP3 por MP3ADDY.
- Los resultados de cada comprobación (host, ROMs de prueba, recompilador,
  compilación de todos los programas) están en el informe del trabajo.

## POLEF de ABI1 con A_INIT (2026-10-29)

Punto de partida: una ronda anterior midió «624 de 624 tareas distintas» en
Donald Duck: Quack Attack (ABI1, el sintetizador de libultra con
reverberación) y lo atribuyó a estados de RESAMPLE/ADPCM sin guardar y a
salidas corridas 2 bytes. Era el oráculo: ese host estaba compilado con los 10
tramos de spukit (`AUD_MAX_RANGES`) y este motor escribe ~30 tramos por tarea
(un SAVEBUFF y un estado por voz y por etapa del eco), así que los tramos se
juntaban por el hueco menor y la comparación incluía huecos que el CPU del
juego escribe mientras la tarea corre. Con `-DAUD_MAX_RANGES=1024` (lo que
exige docs/rsp-lle.md), la misma ROM da **1 de 3855** tareas distintas desde el
arranque (Europa) y 1 de 3852 (USA), y **0 de 2203** desde un estado guardado
dentro del juego; con 10 tramos, 1302 de 1649. Los SETBUFF a direcciones
impares de la DMEM (entrada 0x001, 0x004, 0x142…) son los de libultra
(`alLoadBuffer` carga alineado a 8 y ajusta la entrada) y la HLE ya los hacía
bien; los estados sí se guardan.

La tarea que faltaba se bisecó al **POLEF con A_INIT** (`0e01xxxx`). Con los
datos crudos del hijo de la bisección (DMEM y RDRAM de los dos lados antes y
después del comando, y `SALSA64_LLE_POKE` para cambiar la tabla y los
comandos vecinos sin volver a correr el juego):

- La salida real es la del filtro de dos polos con l2 = −57 (0xFFC7) y, con
  h1[0] ≠ 0, l1 = 0x0012: exactamente las muestras 3.ª y 4.ª del estado que el
  RESAMPLE anterior de la misma cadena de eco dejó en la zona de estados del
  microcódigo (DMEM 0xF90–0xF99: 4 muestras y el acumulador). Un MIXER
  intercalado antes del POLEF (que pisa los registros vectoriales) no cambia
  nada: el valor viene de la DMEM, no de un registro. mupen64plus-rsp-hle
  parte de l1 = l2 = 0 con A_INIT.
- La misma zona la escriben el RESAMPLE (10 bytes) y el ENVMIXER (sus 80 bytes
  de estado, el final: los carriles 2 y 3 del volumen izquierdo quedan en +4 y
  +6) en las tres compilaciones de ABI1 (la estándar, la de GoldenEye y la de
  Blast Corps/Diddy Kong Racing, medidas); el ADPCM, los MIXER, LOADBUFF,
  SAVEBUFF, SETBUFF y LOADADPCM no la tocan. El POLEF pone a cero los 4
  primeros bytes (con y sin A_INIT) y no deja ahí su estado.
- Es también la tarea que quedaba en Diddy Kong Racing: su primer POLEF (A_INIT,
  la primera tarea de audio del juego) lee la DMEM recién encendida, 0xFFFF, y
  como `>> 14` redondea hacia abajo, un −1 en el estado se sostiene: 712
  muestras a −1 contra 0.

La HLE lleva ahora esa zona en su propia DMEM (`aud_state.scratch`, 0xF90 en
ABI1): RESAMPLE y ENVMIXER dejan ahí su estado final y POLEF con A_INIT toma
l1 y l2 de +4 y +6 y pone a cero los 4 primeros bytes. Como la DMEM de la HLE
persiste entre tareas, vale también entre tareas (en la consola real una tarea
gráfica intermedia pisa esa zona con lo suyo: ahí no hay valor «correcto» que
reproducir). Interruptor para volver a lo anterior: `audio-polef-cero` en
`/dev_hdd0/salsa64/` (en el host, `SALSA64_AUDIO_POLEF_ZERO=1`), que es lo que
mide la columna «antes».

Censo con el mismo oráculo (host con `-DAUD_MAX_RANGES=1024`, 3 × 10⁹ ciclos,
~32 s de consola, pulsaciones programadas; «antes» = con `audio-polef-cero`):

| Juego | Variante (ABI) | Tareas | Distintas antes | Distintas después |
|---|---|---:|---:|---:|
| Donald Duck: Quack Attack (Europa) | ABI1 | 3 855 | 1 | 0 |
| Donald Duck: Goin' Quackers (USA) | ABI1 | 3 852 | 1 | 0 |
| Diddy Kong Racing (USA, rev 1) | ABI1 (Blast Corps) | 956 | 1 | 0 |
| GoldenEye 007 (USA) | ABI1 (GoldenEye) | 941 | 0 | 0 |
| Super Mario 64 (USA) | ABI1 | 1 895 | 0 | 0 |
| Wave Race 64 (USA) | ABI1 | 1 898 | 0 | 0 |
| Mario Kart 64 (USA) | nead (Mario Kart 64) | 1 895 | 0 | 0 |
| Star Fox 64 (USA, rev 1) | nead (Star Fox 64) | 1 718 | 3 | 3 |
| Zelda: Ocarina of Time (rev B) | nead (Zelda OoT) | 1 665 | 0 | 0 |

Las 3 de Star Fox 64 son las carreras con el CPU de siempre (1 a 4 bytes a
+13 de un tramo, iguales antes y después); los nead y naudio no tocan la zona
(`scratch` = 0 fuera de ABI1). Donald Duck (Europa) desde el estado guardado
del informe #16, dentro del juego: 2203 tareas, 0 distintas.

Costo en el SPE 1: por RESAMPLE 5 escrituras de 16 bits al local store, por
ENVMIXER una copia de 80 bytes y por POLEF dos lecturas y una escritura de 4
bytes; sin DMA nuevos ni reservas. Local store del SPE 1 (`spu-size`): text
62 824 → 63 472 (+648), data 800 y bss 35 088 sin cambio; ~162,6 KB libres
para la pila.

## Qué conviene escuchar en la PS3

Conker (música y efectos), Perfect Dark y Banjo-Tooie (música con MP3), Majora's
Mask, Yoshi's Story y 1080° (reverberación y filtros), Mario Kart 64, Kirby 64 y
Paper Mario; y las salas de Zelda OoT con fondo prerrenderizado (JPEG).
