# Compatibilidad

Censo de los juegos objetivo (tabla de docs/proyecto.md) y algunos extra. Los
juegos se prueban con respaldos propios, fuera del repositorio; aquí solo hay
datos de compatibilidad.

Para consultar el censo por título, estado, evidencia y etiqueta técnica, abre
el [radar de compatibilidad](compatibilidad/index.html). Sus datos están en
`docs/compatibilidad/catalogo.json`; esta página conserva el detalle histórico.

Las entradas antiguas conservan sus métodos de prueba como historia.

## 2026-11-02 — Reporte #68 (Conker's Bad Fur Day)

| Juego | Estado | Notas |
|---|---|---|
| Conker's Bad Fur Day | desde el ascensor del final de Heist, la sala y Conker se ven en el host como con el microcódigo real (falta la PS3) | ahí el juego cambia a su variante «F3DEX.NoN» (mismo código que «F3DEXBG.NoN»), que por el texto se tomaba por F3DEX2: sin G_TRI4, pantalla negra con líneas. Ahora el código de Conker se reconoce por su CRC (`1B4ACE88` en palabras); `conker-ucode-viejo` vuelve al texto. La luz de la HLE sigue algo más baja que la del microcódigo real, como en el resto del juego |

## 2026-10-31 — Reporte #62 (Dr. Mario 64)

| Juego | Estado | Notas |
|---|---|---|
| Dr. Mario 64 | paneles del fondo de la partida y de la demostración otra vez bien en el host, con el bit de alfa idéntico a ares (falta la PS3) | regresión de la v0.2.15: los destinos de render a textura guardaban el alfa del fragmento y no la cobertura del RDP; `alfa-fragmento` vuelve a lo de la v0.2.15. Las píldoras que no se ven mientras caen son otro problema, anterior |

## 2026-10-29 — Vigilante 8: 2nd Offense (blender)

| Juego | Estado | Notas |
|---|---|---|
| Vigilante 8: 2nd Offense | explosiones, humo y resplandor del sol con mezcla (host; falta la PS3) | el juego los pinta en dos pasadas con el mismo cuadrilátero: MEM·A_IN con el color en negro y encima IN·A_FOG + MEM (G_RM_ADD); la HLE las clasificaba como IN·α + MEM·(1 − α) y quedaba un cuadro negro. Modos nuevos de gfx_blend (`dst·α`, `src + dst·α`, `src·(1 − α) + dst·α`, `src + dst`) hasta el RSX; `mezclas-viejas` vuelve a la clasificación anterior. Escenas del host de SM64, Zelda, MK64, GoldenEye y F-Zero X sin cambios |

## 2026-10-29 — Reportes #57 y #34 (rama gh-worms-lego, sin probar en la PS3)

| Juego | Estado | Notas |
|---|---|---|
| Worms Armageddon | imagen derecha en el host, idéntica al microcódigo real en logos, título y menú de idioma | los fondos de S2DEX declaran `imageW` una columna más ancho que la fila; el paso de las filas ya es el del microcódigo (`s2dex-paso-viejo` vuelve al de antes) |
| LEGO Racers | sin el cuadro alrededor de los autos lejanos en el host | las copias a la RDRAM de los destinos con alfa llevan su alfa (`copia-sin-alfa` vuelve a alfa 1); falta confirmarlo en la PS3 |

## 2026-10-29 — Reportes de Paper Mario y Mystical Ninja

| Juego | Estado | Notas |
|---|---|---|
| Paper Mario | cuartos oscuros con la luz de Mario y de Watt (host; falta la PS3) | antes todo negro salvo el HUD; la máscara de luz (RGBA32 e I8 de 64 × 64 sobre el z-buffer) la dibuja el PPE (`gfx-mascara32-rsx` vuelve a lo de antes). Comparado contra ares en la cueva oscura del castillo de Bowser, con y sin el poder de Watt |
| Mystical Ninja Starring Goemon (Undub) | créditos finales negros y sin música, también en la consola | la tabla de archivos del parche le da al archivo 1154 (créditos) 61 708 bytes comprimidos en vez de ~130: el descompresor del juego sigue leyendo basura sin fin. Con la ROM de EE. UU. sin parche, sin probar hasta el final |

## 2026-10-22 — Candidata de la v0.2.10 (microcódigos)

| Juego | Estado | Notas |
|---|---|---|
| Dark Rift | dibuja peleadores, texturas y animaciones (PS3) | Turbo3D en la imagen 5 del SPE 0; encendido por omisión (`sin-turbo3d` lo apaga). Probada una pelea; Mario 64 sin regresión |
| Resident Evil 2 | letras de OPTIONS legibles (PS3); líneas verticales finas entre tiles | encendida por omisión (`sin-afin-cadena` la apaga); menús, inventario y partida sin regresión |
| Pokémon Snap | sin desorden de dibujo en las escenas probadas (PS3) | hasta 4× va a velocidad completa; a 5× baja a 40–52 campos/s en tramos (el RSX no alcanza) |
| Star Wars Episode I: Racer | video del logo corregido (PS3) | carrera ~47 campos/s |
| Yoshi's Story | arranque del CIC 6106 corregido; ancho de G_SETOTHERMODE de S2DEX 1.x corregido | confirmado en la PS3: el bloque responde, no desaparecen enemigos ni objetos y no se cuelga al morir; quedan puntos y colores erróneos |
| Turok: Dinosaur Hunter, Turok 2, Turok 3 | arrancan; 56–60 campos/s (PS3; en la v0.2.9 no arrancaban) | Turok 2 y 3 usan Acclaim (`E44DF568`): HLE propia con sus luces de punto (los disparos iluminan las paredes), encendida por omisión |
| Battle for Naboo | arranca con HLE (Factor 5 Indy) | 31–33 campos/s en escenas con carga |

## 2026-10-18 — Candidata de la v0.2.8 en la PS3 real

Resumen de lo que cambia de estado desde la v0.2.7 (el detalle de cada juego
está en las secciones siguientes y en `docs/estado.md`).

| Juego | Estado en la v0.2.8 | Notas |
|---|---|---|
| Hydro Thunder, Rush 2049, NBA Showtime | con sonido (MusyX) | NBA Showtime corre acelerado |
| Diddy Kong Racing, Jet Force Gemini, Mickey's Speedway USA | dibujan con la HLE de F3DDKR / F3DJFG | Diddy Kong Racing jugable |
| Killer Instinct Gold | jugable | las listas del CPU las dibuja el SPE de gráficos |
| Resident Evil 2 | jugable | video, menús, personajes con la z del fondo, «YOU DIED»; el logo de N64 va a un cuarto y la sala de la estatua a 65–80 % |
| Star Wars: Rogue Squadron | jugable con HLE, 60 campos/s | logos de un menú sin transparencia; vibración de imagen en los menús |
| Indiana Jones and the Infernal Machine | jugable con HLE | |
| Star Wars Episode I: Battle for Naboo | HLE traducida, **sin probar** | sin respaldo |
| Yoshi's Story | dibuja (S2DEX + capas de 8 bits) | se puede trabar al golpear un bloque; objetos que desaparecen y puntos en texturas |
| Earthworm Jim 3D, Mace: The Dark Age | dibujan | |
| Castlevania, F-Zero X | orden de dibujo corregido | |
| FIFA 99 | inicia el partido | |
| Conker's Bad Fur Day | el jefe final ya se ve | faltan texturas y va lento |
| Zelda: Ocarina of Time | Lente de la Verdad | resplandor de antorchas a través de paredes |
| Animal Forest | reconocido por el CRC | sin probar |
| Mario Tennis, Mortal Kombat 4, Pokémon Snap, Star Fox 64, Majora's Mask, Donkey Kong 64, Road Rash 64 | sin cambio | ver los problemas conocidos en `CHANGELOG.md` |

Las ROMs de más de 64 MB (hacks y homebrew) se abren por fragmentos desde el disco; B3313 (96 MB) va a velocidad completa en la PS3.

## 2026-10-18 — Indiana Jones and the Infernal Machine con HLE gráfica (en el host y en RPCS3; falta la PS3)

Su microcódigo gráfico (Factor 5, CRC-32 de GLideN64 `6859BF8E`, 0x37AFC470 de
los bytes tal cual; sin texto de identificación) solo corría con el RSP a bajo
nivel (en el cañón de la primera misión, 1,7 M de instrucciones del RSP por tarea
gráfica: del orden de 30 ms por tarea en la PS3 con el recompilador del RSP). Ahora
`src/rsp/gfx/f5indy.c` (traducido de GLideN64; GPLv2) lo interpreta en el host, el PPE y
la imagen 4 «Factor 5 Indy» del SPE 0 (docs/spe0-familias.md). La misma HLE trae
la traducción de lo de Star Wars Episode I: Battle for Naboo (`23FEF05F`), que
comparte el microcódigo: **sin respaldo para probarla**; si sale mal, el archivo
`lle` con «all» (docs/rsp-lle.md) da el microcódigo real.

| Escena (host, respaldo propio; los mismos estados con la HLE y con `--lle=all`) | Píxeles distintos |
|---|---:|
| «ENTER NAME»: el fondo de arena y la ventana de la foto (triángulos con textura y colores por vértice) | 0,7 a 0,8 % (> 24 por canal) |
| el cañón de la primera misión: cielo, paredes, Indiana Jones, HUD y partículas | 4,5 a 9 % (> 24); 0,2 % (> 40): el tramado y el filtro de las texturas |
| la cueva oscura (campos 12 000 a 24 000 del flujo de botones al azar) | 0,00 a 0,05 % |

Por vértice (el cañón, 12 tareas, 78 945 vértices): posición a 0,25 píxeles de media
(p95 0,6; la HLE no trunca a cuartos de píxel), s y t a 0,06 texels de mediana,
colores iguales o +1 (el decodificador del RDP del host redondea el sombreado y
el microcódigo lo trunca: la tabla de colores de la DMEM sale **idéntica** en las 38 tareas
medidas del cañón y de la primera escena); rectángulos de textura: las
mismas esquinas en 12 288 de 12 444 (la diferencia son dsdx y dtdy, 4111 contra 4096 en las partículas).

La HLE corrió la primera misión del juego (de los logos a los niveles: 8859 tareas
gráficas en 12 000 campos, y 27 020 en 30 000 con un flujo de botones al azar) sin
comandos sin implementar ni errores. Cada tarea trae ~800 triángulos y, con partículas,
hasta ~2000 rectángulos: en el host, con el búfer de dibujos de 512 KB, 20 de 20 tareas
de una escena de partículas salían cortadas (el HUD, que va al final, faltaba); con 768
KB (la PS3) hay que mirar en el log «tareas cortadas».

## 2026-10-18 — Star Wars: Rogue Squadron con HLE gráfica (en el host; falta la PS3)

El microcódigo gráfico de Factor 5 (CRC-32 de GLideN64 `DA51CCDB`; no trae texto
de identificación) solo corría con el RSP a bajo nivel (10 a 13 campos/s en la
misión de la PS3 con el recompilador del RSP). Ahora `src/rsp/gfx/f5rogue.c`
(traducido de GLideN64; el binario pasa a ser GPLv2, README) lo interpreta en el
host, el PPE y el SPE 0 (imagen 3 «Factor 5», docs/spe0-familias.md). Indiana
Jones y Battle for Naboo usan otro microcódigo de Factor 5 (`F5Indi_Naboo`; HLE en la entrada de arriba).

| Escena (host, respaldo propio) | Contra el RSP real (`--lle=all`, mismos estados) |
|---|---|
| logo, planeta en órbita, rastreo de estrellas | 0,06 a 0,5 % de píxeles distintos (> 16 por canal) |
| «SELECT LEVEL» (la sala del hangar con la imagen de la nave y el texto) | 0,8 % |
| texto que se aleja (el «crawl») | 2 a 5 %: la niebla de los vértices lejanos, que el RSP a bajo nivel del host no aplica |
| escena de la trinchera y los cazas | 1,8 % |
| «MISSION GOALS» y el vuelo en Tatooine (cielo y terreno generados) | los vértices coinciden con los del RSP real (posiciones en 75 a 100 % de los triángulos que caen dentro de la pantalla; colores en 94 a 100 % y coordenadas de textura en 83 a 100 % de los vértices); el cielo difiere en el patrón del tramado: promedios de color iguales a ±3 de 255 |

Sin comandos sin implementar en 9000 y 17 000 campos desde el arranque; el
título, los menús, la selección de misión, la cinemática y el vuelo se ven bien.
Falta la PS3: velocidad y la imagen del SPE 0 (con RPCS3, en la sección de
[estado.md](estado.md), arranca y pasa sus tareas de prueba, y 970 tareas del
juego salen sin error).

## 2026-10-17 — S2DEX suelto: Resident Evil 2 y Yoshi's Story

Las tareas con el texto «RSP Gfx ucode S2DEX» (Resident Evil 2, 2.06; Yoshi's
Story, 1.06) no tenían HLE y terminaban sin dibujar. Ahora `src/rsp/gfx/s2dex.c`
las interpreta en el host, el PPE y el SPE 0 (detalle en [estado.md](estado.md)).

| Juego | Antes (host, sin `--lle`) | Después | Contra el microcódigo real (`--lle=all`) |
|---|---|---|---|
| Resident Evil 2 | 1821 tareas S2DEX sin HLE en 3000 campos | 0 sin HLE, 0 comandos sin implementar, 2895 G_BG_COPY | 0 de 76 800 píxeles distintos en 4 listas; el host no llega a los menús |
| Yoshi's Story | 3370 tareas S2DEX sin HLE, nada dibujado en 6000 campos | 0 sin HLE, 0 sin implementar, 105 756 objetos y fondos | 955 a 3849 de 76 800 píxeles distintos en 6 listas de la pantalla de resultados: los sprites idénticos, el resto es 3D |

## 2026-10-17 — Resident Evil 2: video de las cinemáticas con HLE (en el host; falta la PS3)

Las cinemáticas del juego (los logos, la intro y el video de entrada) las
decodifica el CPU hasta planos YCbCr y el RSP hace el resto con tres
microcódigos pequeños, sin texto de identificación, en tareas de tipo gráfico
(los parámetros van en `ucode_data`): **RESIZE** (cuadro RGB de 320 de ancho →
RGBA5551 en bilineal, en franjas de 63–73 filas; con difuminado en las de 596
de ancho), **DECODE** (planos Y, Cb y Cr de 240 × 120 → RGBA8888) y **FILL**
(promedio de dos cuadros: el cruce entre dos escenas). Hasta hoy esas tareas
terminaban sin hacer nada y el log las contaba como «tareas con microcódigo sin
HLE». La HLE (`src/rsp/jpeg/re2.c`, en el SPE del JPEG en la PS3) se reconoce
como lo hace mupen64plus-rsp-hle (suma de los 256 primeros bytes del código) y
solo toma las tareas cuyos parámetros y constantes se midieron; el resto va al
RSP a bajo nivel o termina sin hacer nada.

Cada tarea se comparó, byte a byte, con el microcódigo real
(`salsa64-run --lle-check-jpeg`), sin pulsaciones y hasta el campo 24 000
(400 s de consola: los logos, la intro y el video de entrada completo):

| Tarea | Tareas | Bytes comparados | Bytes distintos |
|---|---:|---:|---:|
| RESIZE (franjas de 436, 320 y 596 de ancho) | 210 | 11 374 128 | 0 |
| DECODE (240 × 120) | 1566 | 189 423 360 | 0 |
| FILL (promedio de dos cuadros de 240 × 120) | 1560 | 188 697 600 | 0 |

Con las pulsaciones de `docs/pads/re2-intro.pad` (hasta el campo 9000) también
0 bytes distintos en 45 tareas. La aritmética de mupen64plus daba el 46 %
de los bytes distintos en RESIZE y el 1,6 % y el 1,5 % en DECODE y FILL: el
color es entero con piso por producto, el RESIZE redondea con un difuminado
entre vecinos y el cuarto byte de DECODE y FILL no es cero (detalle en
[docs/estado.md](estado.md)).
Pokémon Stadium y Ocarina of Time siguen en 0 píxeles distintos en sus tareas
JPEG (Stadium 900 tareas, Stadium 2 1500, Ocarina of Time 150).

**Lo que falta para jugarlo:** en el host, sin `--lle`, los menús y los fondos
usan tareas S2DEX sueltas que no tienen HLE (14 062 en 8000 campos, contra
1090 de video), así que solo las cinemáticas se ven; con `--lle` las hace el
microcódigo real. Falta probar el video en la PS3 real (el SPE contra el PPE
coincide en el arranque en RPCS3).

## 2026-10-17 — Diddy Kong Racing con HLE (en el host; falta la PS3)

Microcódigo de Rare sin texto (F3DDKR, `ucode_size` 0; CRC-32 de sus 4 KB
`3BFF208D` en el respaldo NTSC-U rev. 1). Con la HLE de `src/rsp/gfx/f3ddkr.c`
el juego se ve y se juega en el host (`salsa64-run`, respaldo propio): logos,
intro con los personajes, título, selección de jugadores, «Game Select», elegir
pista y vehículo, «Get ready» y una carrera en Ancient Lake, y la isla de
Timber en el modo Adventure (`docs/pads/dkr-pista.pad` llega a la carrera
desde el arranque). 0 comandos sin implementar y 0 errores en 16 000 campos.

| Comparación | Resultado |
|---|---|
| ares (consola) contra la HLE, volcados de la RDRAM en cuadros coherentes | PSNR en 4×4 de 31 a 41 dB en los logos y la intro (0–10 % de píxeles a más de 8/255); 24 a 35 dB en el título y las demos de personajes en pista (6–48 %) |
| Microcódigo real (`--lle=all --lle-speed 8`) contra la HLE, mismo guion desde el arranque | 0,01–0,44 % de píxeles distintos tras tolerar 1 px en los campos 250–1750 (logos, intro, demos de personajes) y 0,5–2,5 % jugando en la isla de Timber (el HUD, que la LLE no dibuja, cuenta ~1 %); en otros tramos (título, cinemáticas) se separan por el movimiento y por los textos, que la LLE del host tampoco dibuja (la consola sí) |

Lo que queda: bordes de polígonos con antialiasing y detalle de las
texturas (el rasterizador del host; docs/referencia-ares.md), y en el título y
algunas demos el suelo sale ~9 % más claro que en la consola (240 contra 219
de media en una franja del título) sin que se sepa de dónde: la LLE da los
mismos colores que la HLE, así que no viene del microcódigo.
Variante **no probada** (sin respaldo): otras versiones de DKR (CRC de
GLideN64 `6E6FC893`). Jet Force Gemini y Mickey's Speedway USA (F3DJFG) tienen
su sección debajo.

## 2026-10-17 — Jet Force Gemini y Mickey's Speedway USA con HLE completa (en el host; falta la PS3)

El microcódigo de los dos (F3DJFG, CRC-32 de GLideN64 `BDE9D1FB`) es el de
Diddy Kong Racing con otro conteo de vértices y un comando más, G_DMA_TEX_OFFSET
(0x02), que la HLE no tenía: sin él daban 746 y 2548 comandos sin implementar en
los títulos y, en Mickey's Speedway, las luces del semáforo de la salida no
cambiaban nunca. Ahora quedan en **0 comandos sin implementar** y en el host:

| Juego | Hasta dónde llega (`salsa64-run`, respaldo propio) | Comandos sin implementar |
|---|---|---|
| Jet Force Gemini | título, «Select Game» con el teclado del nombre, intro de la partida nueva (el planeta, la nave, el puente de mando, una aldea en la selva: escenas en 3D) y selección de Juno (`docs/pads/jfg-3d.pad`, hasta el campo ~14 000) | 27 981 → 0 |
| Mickey's Speedway USA | título, cuarto de Mickey, personaje, «Motor Way Mania», la historieta, la parrilla y el principio de la carrera (`docs/pads/mickey-semaforo.pad`, campos ~5 600 a 12 000) | 3 242 → 0 |

Contra el microcódigo real (`--lle=all`, desde estados del juego, mismos
campos): los G_SETTIMG que manda al RDP coinciden uno por uno (26 029 de
Mickey's Speedway y 13 550 de Jet Force Gemini); los alfas de los vértices que
carga coinciden salvo ±1 en los de niebla (la HLE redondea). La imagen del VI
difiere 0,1–3 % en tres de las cuatro escenas probadas de Jet Force Gemini y
3,7–8,9 % en las de Mickey's Speedway, casi todo el HUD y los textos que la
LLE del host no dibuja; las luces del semáforo pasan de apagadas a iguales a
las del microcódigo real. La LLE del host tampoco aplica la niebla del
blender (`build_state` solo la pone en los triángulos de la HLE): en la
escena de la selva de Jet Force Gemini, con mucha niebla, las dos imágenes se
separan 45,6 % y con la niebla apagada en la HLE solo 1,0 %.
Variantes sin respaldo: otras versiones de Mickey's Speedway y de Jet Force
Gemini.

## 2026-10-17 — Killer Instinct Gold: dibuja con la HLE; la segunda pelea se congela en la PS3

| Juego | Antes | Ahora |
|---|---|---|
| Killer Instinct Gold (NKIE) | PS3: 0 tareas gráficas en todas las versiones (solo dibujaba con `lle` encendido); host: dibuja | RPCS3 sin `lle`: 1409 tareas gráficas en 1800 campos (logo y fichas de los personajes); host: la pelea completa, sin esperas |

El juego da al RDP listas escritas por el CPU (`DPC_START`/`DPC_END`, sin RSP:
el fondo del escenario y los marcos) y no manda tareas gráficas hasta recibir
la interrupción del DP de su `SYNC_FULL`. Esas listas las corre ahora el mismo
SPE que las tareas (con otro estado del RDP sobre la misma caché de texturas
los fondos salían rotos); detalle en [docs/estado.md](estado.md).

**Sin resolver (PS3 real):** con la primera versión del arreglo la primera
pelea va bien y al empezar la segunda (hacia 50 campos después de «READY») el
juego deja de leer los controles y de mandar tareas gráficas, con la música
sonando; con el RSP a bajo nivel en todas las tareas (en una versión de
prueba de su recompilador) pasa en la primera. No se reproduce en el host. La
versión siguiente avisa y vuelca la RDRAM (`cuelgue-<campo>.rdram` en
`/dev_hdd0/salsa64/`) para ver en qué espera cada hilo.

## 2026-10-16 — Audio de MusyX (Factor 5): HLE nueva, verificada en el host

Los juegos de MusyX corrían mudos: su tarea de audio no es una lista de
comandos y la HLE la daba por mala. Ahora hay HLE de MusyX v1 y v2
(`src/rsp/audio/musyx.c`, SPE 1 en la PS3). Cada tarea se comparó, byte a
byte, con el microcódigo real corriendo en el intérprete del RSP
(`salsa64-run --lle=all --lle-check-audio`); detalle y método en
[docs/estado.md](estado.md). **Falta oírlos en la PS3 real**: aquí solo hay
medición en el host.

| Juego | MusyX | Tareas de audio idénticas al microcódigo real (96 s de consola, con START y A) | Observaciones |
|---|---|---:|---|
| Hydro Thunder | v1 | 5740 de 5740 | Sin eco en la prueba. |
| Rush 2049 | v1 | 5728 de 5728 | Sin eco en la prueba. |
| NBA Showtime | v1 | 5740 de 5740 | Sin eco en la prueba. |
| Resident Evil 2 | v1 | 2856 de 2856 | Con eco (reverberación) en todas las subtramas. Además tiene HLE del video de las cinemáticas (arriba); los menús y el juego usan S2DEX sueltas, que desde el 2026-10-17 tienen HLE (sección de arriba). |
| Star Wars: Rogue Squadron | v1 (otra compilación) | 5713 de 5713 | Con eco, con la envolvente del envío en rampa. Los gráficos, con HLE desde el 2026-10-18. |
| Indiana Jones | v2 | 5675 de 5724 (las otras 49, +1 en 2 de cada 8 muestras del canal izquierdo) y, con el juego jugado (campos 8000 a 27 000 de un flujo de botones al azar: las tareas siguientes), 16 990 de 16 990 | `ptr_10` (la etapa de v2 que el HLE no hace) no apareció en ninguna; sus gráficos, con HLE desde el 2026-10-18. |

Sin respaldo para probar: Gauntlet Legends, Tarzan, The World Is Not Enough,
Rugrats in Paris, Polaris SnoCross y Battle for Naboo.

## 2026-10-16 — Candidata de la v0.2.7 en la PS3 real

PS3 con HEN, respaldos NTSC. La rc5 (v0.2.6-47-gd01e523, la que se publica
como v0.2.7) abrió estos juegos en una sola corrida; la velocidad es el rango
de las ventanas de 5 s del log (sin la primera y la última de cada juego) y
cuántas bajaron de 58.

| Juego | Estado | Velocidad | Notas |
|---|---|---:|---|
| Doom 64 | Jugable | ~60 campos/s; 2 de 98 ventanas bajo 58 (53,5 la menor) | Era ~40 % (las esperas de libultra por el planificador ahora se saltan). |
| NFL Blitz | Jugable | 59,2–60,2 campos/s | Era ~26 campos/s. |
| 1080 Snowboarding | Jugable | ~60 campos/s; 2 de 43 bajo 58 (51,8 la menor) | Con el Controller Pak; sin él se quedaba leyendo el control. |
| Mario Kart 64 | Jugable | ~60 campos/s; 6 de 130 bajo 58 (53,7 la menor) | Lee y escribe su `.mpk` (fantasmas). |
| Pokémon Stadium | Jugable | ~60 campos/s; 6 de 84 bajo 58 (54,6 la menor) | «Game Pak Check» completo (también al volver del título), «Please Select» sin fantasma, VS sin caerse. |
| Pokémon Snap | Jugable | ~60 campos/s; 2 de 13 bajo 58 (55,6 la menor) | Queda el orden de dibujo de la v0.2.6. |
| Wave Race 64 | Jugable | — | Variante F3DWR del microcódigo (probado con la rc4). |
| Star Wars Episode I: Racer | Jugable, lento | 43–47 campos/s en carrera | Medido con la app de perfilado de la misma ronda del CPU (era ~32). |

## 2026-10-15 — Censo en la PS3 real (v0.2.6)

Regresión de la versión candidata y comprobación de la rc1 en la PS3 real.
La rc1 abrió más de 15 juegos en varias corridas; la tabla recoge los títulos
con resultado o incidencia documentados. Sin una medición explícita no se
asigna velocidad.

| Juego | Estado | Velocidad | Notas |
|---|---|---:|---|
| Banjo-Tooie | Jugable | 59,7 campos/s de mediana en la sesión de perfilado | Prueba larga; escenas pesadas limitadas por el camino lento del recompilador. |
| F-Zero X | Jugable; carrera y «SELECT COURSE» superados | — | A 5×. Niebla de la pista desde la rama `fzero-niebla` (blender en un ciclo; `sin-niebla-1ciclo` la apaga). El fondo de las pistas con la sombra del RDP (recorte contra la caja del clip ratio como el RSP + sombreado lineal en pantalla en el RSX; `sin-sombra-afin` lo apaga), pendiente de la PS3. |
| Pokémon Snap | Jugable | — | Orden de dibujo incorrecto: la correa de la mochila queda delante y la playa delante de las montañas. |
| Zelda: Ocarina of Time | Jugable | — | Se probaron salas con fondos prerrenderizados; el registro de la sesión informa 149–150 fondos S2DEX por ventana. |
| GoldenEye 007 | Jugable | — | Incluido en la regresión; sin medición de velocidad atribuible a esta prueba en el censo. |
| Perfect Dark | Jugable | — | Incluido en la regresión; sin medición de velocidad atribuible a esta prueba en el censo. |
| Kirby 64 | Jugable | — | Incluido en la regresión. |
| Mario Kart 64 | Jugable | — | Se verificó la interpolación. |
| Conker's Bad Fur Day | Abre después de otros juegos | — | Se comprobó la apertura de una ROM grande fuera del heap. |
| Super Mario Odyssey 64 | Inestable | — | Se registró una caída durante una prueba anterior de interpolación; queda pendiente una causa. |
| Star Fox 64 | Jugable, con fallo gráfico | — | Las opciones del menú se repiten. |
| Wave Race 64 | Fallo gráfico | — | Las texturas aparecen «como rayos desde el centro». |
| Yoshi's Story | Dibuja en el host (antes, nada) | — | Sus tareas S2DEX 1.x sueltas tienen HLE desde el 2026-10-17 (sección de arriba). Sin probar en la PS3. |
| FIFA 99 | La intro funciona; el partido no arranca | — | Fallo conocido. |
| Banjo-Kazooie | Jugable, con fallo gráfico | — | Las piezas del rompecabezas salen negras. |
| Diddy Kong Racing | Jugable en el host | — | HLE de F3DDKR desde el 2026-10-17 (sección de arriba); sin probar en la PS3. |
| Jet Force Gemini | Solo los títulos, con comandos sin implementar | Dibuja con todos sus comandos en el host | G_DMA_TEX_OFFSET (2026-10-17, sección de arriba); sin probar en la PS3. |
| Mickey's Speedway USA | Solo los títulos; el semáforo de la salida no cambia | Dibuja con todos sus comandos en el host; el semáforo cambia de luz | G_DMA_TEX_OFFSET (2026-10-17, sección de arriba); sin probar en la PS3. |
| Indiana Jones | Jugable en el host y corre en RPCS3 (HLE gráfica del 2026-10-18; antes, solo con el RSP a bajo nivel) | — | `f5indy.c` (sección de arriba); sin probar en la PS3. |
| Star Wars: Rogue Squadron | Jugable en el host (HLE gráfica del 2026-10-18; antes, solo con el RSP a bajo nivel) | — | `f5rogue.c` (sección de arriba); sin probar en la PS3. |
| Animal Crossing (Dōbutsu no Mori, traducción «ANIMAL FOREST», issue #18) | Sin dibujar: sus dos microcódigos no traían texto de versión | — | Reconocidos por el CRC-32 de su código (`D39A0D4F`: F3DZEX2 como el de Majora's Mask, sin recorte contra el plano cercano; `02C399DD`: S2DEX2; los valores de la lista de GLideN64). **Sin respaldo para probarlo**; si sigue sin dibujar, el log de la PS3 trae ahora los CRC del código desconocido (docs/estado.md, 2026-10-18). |
| Resident Evil 2 | Cinemáticas y sus copias a la pantalla en el host; los menús, sin ver | — | HLE del video (RESIZE, DECODE y FILL) idéntica al microcódigo real y, desde el 2026-10-17, las tareas S2DEX sueltas (G_BG_COPY de los cuadros de video: idéntico al microcódigo real; el host no llega a los menús). Sin probar en la PS3. |

Las pruebas de rendimiento detalladas de Banjo-Tooie corresponden a la sesión
de perfilado indicada en [docs/perf-log.md](perf-log.md); no representan una
mediana de la rc1.

## 2026-10-12 — Censo en la PS3 real (v0.2.0 a v0.2.5)

PS3 con HEN, respaldos NTSC, salida a 1080p con escala 5×. Este censo queda
como antecedente; el de 2026-10-15 es el más reciente.

| Juego | Estado | Notas |
|---|---|---|
| Zelda: Ocarina of Time | velocidad completa | sin tirón en las salas prerrenderizadas desde v0.2.1 |
| Zelda: Majora's Mask | velocidad completa | guardado en FlashRAM; la iluminación de algunas zonas salta entre fuentes de luz |
| Super Mario 64, Mario Kart 64 | velocidad completa | |
| Donkey Kong 64 | velocidad completa | cámara estable desde v0.2.1; tirones breves al entrar a zonas nuevas |
| Paper Mario, Mario Party, Mario Party 3 | velocidad completa | |
| Perfect Dark | velocidad completa en la intro y menús | 43–55 cuadros/s en las escenas más pesadas (CPU) |
| GoldenEye 007 | 45–60 cuadros/s | la presa es la zona más pesada; líneas azules finas en algunas uniones |
| Conker's Bad Fur Day | ~60 en juego | **v0.2.1 y v0.2.2: se queda en negro tras «For Mature»** (usar v0.2.0 o v0.2.3); desde v0.2.3 arranca sin los 25–30 s de negro; sombras cuadradas |
| Star Fox 64, Banjo-Kazooie | velocidad completa | Star Fox: ruido de colores arriba durante el logo «Nintendo presents» hasta v0.2.5 (el relleno de arranque llegaba antes que los framebuffers); corregido en el árbol, por confirmar en la PS3. Banjo: las pantallas del rompecabezas salen en negro |
| Kirby 64, Pokémon Stadium 2 | ~97–100 % | algunas texturas con ruido o en ajedrez |
| Pokémon Snap | jugable | texturas estiradas en algunos polígonos |
| Super Smash Bros. (y Smash Remix) | velocidad completa | desde v0.2.4 pasa los KO y el Castillo de Hyrule se ve bien (confirmado en la PS3 real); en v0.2.3 y anteriores se congelaba en el primer KO (`/dev_hdd0/salsa64/fbfx.cfg` con un `1` lo evitaba) |
| Doom 64 | ~40 % | limitado por el CPU |
| Banjo-Tooie | jugable en la PS3 (primera prueba) | desafío del CIC 6105 de la PIF; confirmado en la PS3 real el 2026-10-13 |
| F-Zero X | jugable en la PS3 (v0.2.6) | bus abierto del PI (sondeo del 64DD), el microcódigo que recarga a mitad de lista (F3DEX → F3DFLX) y los brillos de los autos por tabla; a 5× se colgaba en «SELECT COURSE» hasta la v0.2.6; la niebla de la pista (blender FOG·A_SHADE en un solo ciclo con FORCE_BL, que la HLE solo tomaba en dos ciclos) desde la rama `fzero-niebla`, `sin-niebla-1ciclo` la apaga; el fondo de las pistas (objetos del fondo pintados sobre la pista, bruma del horizonte tardía) era la sombra afín del RDP tras el recorte del RSP: desde la rama `fzero-afin` la HLE recorta contra la caja del clip ratio como el microcódigo y el RSX interpola el sombreado lineal en pantalla (`sin-sombra-afin` lo apaga); contra ares, las carreras de la demo pasan de 19–27 a 30–37 dB en 4×4; falta la PS3 |
| Diddy Kong Racing | pantalla negra | microcódigo propio de Rare; en desarrollo con el RSP a bajo nivel |
| Resident Evil 2 | no jugable | microcódigo de Capcom |

Las versiones europeas (PAL) y japonesas no se han probado.

## 2026-10-10 (3) — microcódigos gráficos de Rare (Perfect Dark y Conker)

Código medido: rama `competent-haslett-3bad2a` (main 3270538 con la
rama de arranque y `osCicId`; luego main 53fb223 fusionado), mismo método: 3 minutos de consola
sin tocar el control.

| Juego | Gráficos (HLE) | A los 3 minutos |
|---|---|---|
| Perfect Dark | **F3DPD** (`src/rsp/gfx/f3dpd.c`), reconocido por el CRC-32 del código (c543d0a8, 0x1000 bytes): 6914 tareas, **0 comandos sin implementar**, 0 errores; 1 combiner aproximado (abajo) | Pantalla de identificación (alta resolución, 576×474), logo de Nintendo, cubo de N64 que se vuelve el logo de Rare, vuelo por la ciudad y Joanna en la azotea |
| Conker's Bad Fur Day | **F3DEXBG** (`src/rsp/gfx/f3dexbg.c`), por su texto: 4468 tareas, **0 comandos sin implementar**, 0 combiners aproximados, 0 errores | Pantalla legal, logo de Nintendo, Conker con la motosierra, logo de Rareware, la taberna y el menú «GAME 1» |

- **PS3 real** (paquete «Salsa64 Rare» con main ce8351d): Perfect Dark se
  ve y suena bien, al 100 % de NTSC salvo la intro en alta resolución.
  Conker arranca y suena, pero en el RSX el 3D sale quemado y parpadea (la
  HLE del SPE es correcta: su volcado dibujado en la Mac sale bien); falla
  de presentación con su destino CI de 8 bits y 64 de ancho a mitad de
  cuadro, pasada al trabajo de presentación.
- Con el control (`--pad`: START y luego A cada 5 s): Perfect Dark llega a
  «Enter Agent Name» (7782 tareas en 150 s) y Conker al comienzo del juego
  (6830 tareas en 260 s), los dos con 0 comandos sin implementar y 0
  errores.
- **Perfect Dark** es Fast3D salvo los vértices: G_VTX de 12 bytes con el
  color en una tabla aparte (G_VTXCOLORBASE, 0x07); G_TRI4 en 0xB1 como
  GoldenEye. Su intro es de alta resolución: el rasterizador del host
  (320×240) la ve completa con `EXTRA_CFLAGS="-DRASTER_W=640
  -DRASTER_H=480"`.
- **Conker** quedaba mudo de imagen porque su G_LOAD_UCODE (0xDD) no
  cambia de GBI: carga la iluminación avanzada y la lista sigue. La HLE de
  F3DEX2 terminaba la tarea ahí, sin llegar al G_RDPFULLSYNC, y el juego
  esperaba para siempre la interrupción del DP. El 0x14 era su G_TRI4
  (0x10–0x1F).
- Combiner aproximado de Perfect Dark: `lerp(ENV, TEXEL0, SHADE_ALPHA) ·
  SHADE` (2 ciclos), el mismo de GoldenEye (motor de Rare): hace falta un
  término S·Sa en la combinación del RSX (Fase 4). 40 905 dibujos en 3
  minutos.
- Conker, en la escena del logo de Rareware, muestra la boca abierta con el
  interior oscuro (su textura de 32×32: fondo blanco y la boca en el
  centro); parece correcto, pendiente de confirmar en la PS3.
- Efectos de framebuffer de estos juegos, fuera de esta tarea (Fase 4).
  Perfect Dark: la cámara espía y los monitores (imagen dibujada y leída
  como textura) y la lectura del z-buffer con rectángulos texturizados
  (GLideN64: `hack_rectDepthBufferCopyPD`, `hack_clearAloneDepthBuffer`,
  `texturedRectDepthBufferCopy`). Conker: el desenfoque de la pausa, la
  sombra de Conker, la copia del z-buffer y rectángulos que copian la
  imagen sobre sí misma (`hack_blurPauseScreen`,
  `hack_rectDepthBufferCopyCBFD`, `hack_fbTextureOffset`,
  `texturedRectCopyToItself`).

## 2026-10-10 (2) — main 3270538 (F3D integrado) + osCicId

Código medido: esta rama (arranque del 6105 y `osCicId`) con main 3270538
(audio en el SPE 1, bucles de espera mapeados, F3D 2.0D/2.0G y F3DEX
0.95/1.x, el DP se cierra tras un microcódigo sin HLE). Antes de integrar
F3D se midió con su rama fusionada aparte: los mismos números.

| Juego | Región / versión | CIC | Guardado | Gráficos (HLE) | Audio (HLE) | A los 3 minutos |
|---|---|---|---|---|---|---|
| Super Mario 64 | NTSC-U, NSME v0 | 6102 | EEPROM 4K | F3D 2.0D: 5252 tareas, 0 comandos sin implementar | ABI 1: 10757 tareas, 0 errores | **Título** («PRESS START» con la cara de Mario) |
| Zelda: Ocarina of Time | NTSC-U 1.2, CZLE v2 | 6105 | SRAM 256K | F3DZEX 2.06H + S2DEX 2.05: 3724 tareas | nead (OoT): 10494 tareas, 0 errores | Demos del título (escena 3D de bosque) |
| Mario Kart 64 | NTSC-U, NKTE v0 | 6102 | EEPROM 4K | F3DEX 0.95: 5101 tareas | nead (MK64): 10707 tareas, 0 errores | Intro (bandera de cuadros) |
| Star Fox 64 | NTSC-U, NFXE v0 | 6101 | EEPROM 4K | F3DEX 1.22 NoN: 5375 tareas | nead (SF64): 10654 tareas, 0 errores | Intro («We need your help, Star Fox!») |
| GoldenEye 007 | NTSC-U, NGEE v0 | 6102 | EEPROM 4K | F3D 2.0G (Rare): 10127 tareas | ABI 1 (GoldenEye): 5362 tareas, 0 errores | Créditos de la intro; VI de 440×326 |
| Perfect Dark | NTSC-U, NPDE v0 | 6105 | EEPROM 16K | **Sin HLE**: 6761 tareas con microcódigo desconocido (sin texto) | naudio con MP3: 5208 tareas, **todas con error** | Corre (VI 320×218 en el Expansion Pak), no dibuja ni suena |
| Banjo-Kazooie (extra) | NTSC-U, NBKE v0 | 6103 | EEPROM 4K | F3DEX 1.21: 4604 tareas | naudio (BK): 5365 tareas, **todas con error** | Demo de juego (Banjo en un nivel) |
| Conker's Bad Fur Day (extra) | NTSC-U, NFUE v0 | 6105 | EEPROM 16K | F3DEXBG.NoN fifo 2.08 (variante de F3DEX2): **1 tarea** a los 29,5 s, 2 comandos sin implementar (último 0x14), y no manda más | naudio (Conker): 5381 tareas, **todas con error** | VI 292×213; espera tras la primera tarea gráfica |

Todos corren 3 minutos sin excepciones raras (interrupciones, coprocesador
no usable por el cambio perezoso de la FPU y, en GoldenEye y Conker, fallos
de TLB porque paginan código desde la ROM). Bucles de espera: 62 % (Perfect
Dark) a 98 % (Conker); GoldenEye pasó de 0 % a 86 % con los bucles mapeados de
main.

Código fuera de KSEG0 (PC muestreado al final de cada bloque): 100 % en
GoldenEye, Perfect Dark y Conker (código en 0x7000xxxx, 0x1000xxxx y
0x1500xxxx por la TLB); 0 % en los demás. Con el recompilador: Perfect Dark
6289 de 6294 bloques en 10,7 s de consola (qemu, `--lockstep` sin
diferencias) y 6078 de 6083 en la PS3 real.

### Conker: `osCicId`

Sin este commit, Conker se quedaba a los 88 mil ciclos en su hilo ocioso
sin configurar el VI ni mandar tareas: su hilo ocioso crea el hilo principal
pero **solo lo arranca si `osCicId` (0x80000310) vale 6105**, y el arranque
HLE no escribía esa palabra. El IPL3 del 6105 deja ahí su número y el del
6103, el suyo (Banjo-Kazooie y Diddy Kong Racing comprueban 6103; Perfect
Dark lo comprueba al cerrar puertas; DK64 también lo espera). Ahora el
arranque escribe 6103 o 6105 según la variante (docs/cpu-vr4300.md).

Después corre su lógica y su audio, configura el VI y a los 29,5 s manda su
primera tarea gráfica (microcódigo «F3DEXBG.NoN fifo 2.08», ucode en
0xBEBB0 con ucode_size 0, data en 0xC0050). La HLE la toma como F3DEX2, deja
2 comandos sin implementar (el último 0x14) y el juego no manda otra en los
3 minutos: le toca al trabajo de F3D. Su audio usa naudio de Conker
(`ucode_data` 00000000 1ab0140c 1fa41340 f0000f00).

### Perfect Dark

- Antes del arranque del 6105: `b .` en 0x700017F0 a los 15 M ciclos,
  porque 0xA00002E8 ≠ 0xC86E2000 (la palabra la deja el IPL3 del 6105). 201
  bloques en la PS3.
- Ahora: monta 0x7F000000 en la TLB, configura el VI, manda su primera
  tarea gráfica en el ciclo 186 M (ucode en 0x5A0B0, data en 0x89AD0, flags
  2 = DP_WAIT, sin texto: variante de Rare que GLideN64 llama F3DPD) y audio
  desde el ciclo 585 M. Con el DP cerrado tras el microcódigo desconocido
  (rama de F3D) sigue corriendo y alterna búferes de 320×218 en el Expansion
  Pak.
- PS3 real (frontend «Salsa64 arranque», SLSA00078, con el recompilador,
  antes de la rama de F3D): pasa la comprobación; 59,8 campos/s, 93,7 MIPS,
  5,8 ms de emulación por campo, pantalla negra.
- Sus otras comprobaciones del «bootloader» (ROM 0xA5C, 0x454, 0xDC0, según
  `docs/piracychecks.md` del decomp) leen la ROM del cartucho (0xB0000A5C):
  no dependen del arranque. La de `osCicId` (al cerrar puertas) queda
  cubierta por este commit.
- El desafío del CIC 6105 por la PIF durante el juego no apareció en 3
  minutos.

### Identificación de los microcódigos

| Juego | Gráfico: texto en `ucode_data`; CRC-32 de ucode / data | Audio: `ucode_data` en 0x00, 0x10, 0x28, 0x30 |
|---|---|---|
| SM64 | «RSP SW Version: 2.0D, 04-01-96»; 11d4da6c / b0de6837 | 00000001 11181470 1e24138c f0000f00 (ABI 1) |
| OoT | «RSP Gfx ucode F3DZEX.NoN fifo 2.06H» y «RSP Gfx[Safe] S2DEX fifo 2.05»; 72fea817 / 56159df6 | 00000001 1f681230 1b341190 17841830 (nead de OoT/MM) |
| MK64 | «RSP Gfx ucode F3DEX 0.95»; cd02d1ad / 270ed8eb | 00000001 11181350 1a18126c 1a8c1adc (nead de Mario Kart) |
| SF64 | «RSP Gfx ucode F3DEX.NoN 1.22»; cc888e6c / 5b69be26 | 00000001 110412cc 1b2c120c 180418b0 (nead de Star Fox) |
| GoldenEye | «RSP SW Version: 2.0G, 09-30-96»; 9cba9d04 / 2a9b4b97 | 00000001 11181470 1dc8138c f0000f00 (ABI 1 de GoldenEye; ucode_size = 0) |
| Perfect Dark | sin texto; c543d0a8 / 49fd74ec | 00000000 1ae8143c 1fb01358 f0000f00 (naudio de Banjo-Tooie/JFG/PD) |
| Banjo-Kazooie | «RSP Gfx ucode F3DEX 1.21»; 9fc2ac6e / 85d611d3 | 10f013a0 00001280 00010010 00020004 (naudio de Banjo-Kazooie) |
| Conker | «RSP Gfx ucode F3DEXBG.NoN fifo 2.08»; ucode_size 0 / 1c5c25dc | 00000000 1ab0140c 1fa41340 f0000f00 (naudio de Conker) |
| Diddy Kong Racing | sin texto; ucode_size 0, 3bff208d (4 KB del código) / c432d0ea | 00000001 111814a4 1e3c1390 f0000f00 (ABI 1 de Blast Corps) |

Los nombres de las variantes de audio siguen la identificación de
mupen64plus-rsp-hle (por esas mismas palabras; solo consulta). Los CRC de
`ucode_data` del audio cambian entre tareas en algunos juegos: los datos
tienen partes que el microcódigo escribe; las cuatro palabras son estables.

### Lo que falta, por prioridad

| # | Qué | Desbloquea | Quién |
|---|---|---|---|
| 1 | HLE de naudio: la de Banjo-Kazooie, la de Conker y la de Perfect Dark (con MP3); hoy toda tarea da error | Sonido en los tres juegos de Rare (y Banjo-Tooie, JFG) | trabajo de audio (`src/rsp/audio`, SPE 1) |
| 2 | ~~HLE del microcódigo de Perfect Dark~~ hecho: F3DPD (sección 3) | Perfect Dark | trabajo de Rare |
| 3 | ~~F3DEXBG 2.08 de Conker~~ hecho: G_TRI4 en 0x10–0x1F y G_LOAD_UCODE que no corta la lista (sección 3) | Conker | trabajo de Rare |
| 4 | Probar con el control (pasar los títulos; `--pad` de la rama de F3D) y medir en la PS3 las escenas de docs/proyecto.md | Mediciones de la tabla de juegos | tras 1–3 |
| 5 | ~~Desafío del CIC 6105 por la PIF~~ hecho 2026-10-13 (`cic6105_challenge`); Banjo-Tooie lo pide al cargar datos | Banjo-Tooie (arreglado), Perfect Dark y Conker si lo piden | — |
| 6 | Zelda: en una captura de la demo el cielo aparece en la parte de abajo; revisar si es el encuadre o geometría que falta | Zelda | trabajo de F3D |

## 2026-10-10 (1) — primera pasada, sin F3D ni audio

Con el arranque del 6105 y antes de main f698afe: solo Zelda dibujaba. Los
demás mandaban una tarea gráfica con un microcódigo sin HLE y se quedaban
esperando la interrupción del DP, porque esa tarea terminaba con TASKDONE
pero sin G_RDPFULLSYNC. Un experimento local (disparar el DP tras esa tarea)
mostró que los seis seguían 3 minutos sin otro bloqueo; la rama de F3D hace
ahora eso mismo. Tareas del RSP en esos 3 minutos: SM64 10765 de audio,
OoT 3724 gráficas y 10494 de audio, MK64 10763, SF64 5384, GoldenEye 5362,
Perfect Dark 5203, Banjo-Kazooie 5365 (de audio, más la gráfica sin HLE).
