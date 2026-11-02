# Estado

## 2026-11-02 — #68, Conker: tras el ascensor del final de Heist, cinemática rota y pantalla negra con líneas

- Log de la PS3 (v0.2.15): todo el juego con «F3DEXBG.NoN fifo 2.08» (imagen Conker del SPE 0) hasta
  la tarea 182 262, al tomar el ascensor: «cambio de imagen Conker → F3DEX2» y el microcódigo
  «RSP Gfx ucode F3DEX.NoN fifo 2.08» **en la misma dirección (0x0bebb0) y con el mismo CRC-32 del
  código** (99E222AC, 1B4ACE88 en palabras). Desde ahí no vuelve a la imagen Conker; tras dos
  reinicios, el juego arranca con F3DEXBG y al cargar la partida pasa otra vez a F3DEX2. Ningún aviso
  de destinos, copias, z ni del RSX que cambie en ese momento.
- Reproducido en el host con el estado del reporte (su ROM era una traducción al español, de otra
  cabecera; se cargó con la USA en una copia con esa cabecera, fuera del repo): con la HLE, 3952
  comandos por tarea, 98 triángulos y **531 comandos sin implementar (último 0x18)**; la imagen es
  negra con unas líneas, como en el reporte. Con `--lle=all`, la sala con Conker (2338 triángulos
  por tarea, 0 sin implementar).
- Causa: el juego trae un solo código de 4 KB y ocho segmentos de datos, «F3DEX», «F3DEXBS»,
  «F3DEXBG» y «F3DEXBGBS», con y sin «.NoN» (en la ROM, cada 0x6C0 bytes), y copia el que toca en
  un búfer fijo. Entre ellos solo cambian el texto, las constantes del recorte cercano (DMEM 0x1AC,
  0x666, 0x6A6) y el largo de una carga de código (DMEM 0x6AC: 0x318 en BG, 0x388 en F3DEX, 0x3A0
  en BS). La detección iba por el texto: «F3DEX.NoN fifo 2.08» se tomaba por un F3DEX2, que no
  tiene G_TRI4 (0x10–0x1F): casi todos los triángulos quedaban sin dibujar y los vértices de 16
  bytes con luces de Rare se leían como los de F3DEX2.
- Que «F3DEX» y «F3DEXBG» son el mismo microcódigo para la HLE, medido con el LLE: en la cueva del
  Poo (con «F3DEXBG.NoN»), el LLE con el largo de carga de «F3DEX» (DMEM 0x6AC = 0x388) da las 50
  tareas bit a bit iguales que con el suyo, y el código de más (IMEM 0x5F0–0x65F) sí se ejecuta
  (14 190 entradas). Al revés, en la escena del reporte el LLE con el largo de «F3DEXBG» se cuelga
  en la segunda tarea (60 M de instrucciones): «F3DEX» vuelve a cargar esos 0x70 bytes porque allí
  algo pisa la IMEM, y por eso el juego lo usa en esa parte.
- Arreglo (`core/rsp.c`): el código de Conker se reconoce por el CRC-32 de sus 4 KB (palabras al
  revés 0x1B4ACE88) antes que el texto, y cualquiera de las ocho variantes va a F3DEXBG (imagen
  Conker del SPE 0, sin cambio de imagen); sin recorte cercano si el nombre dice «.No» («F3DEXBGBS.No»
  no cabe entero). El CRC de los 4 KB se calcula una vez por tarea para las tres variantes por
  código (Acclaim, Eurocom, Conker), antes eran dos cálculos. Interruptor `conker-ucode-viejo` en
  `/dev_hdd0/salsa64/` (en el host, `SALSA64_GFX_CONKER_VIEJO=1`): solo el texto, como antes.
- Antes y después contra el LLE (mismo estado, 300 y 1800 campos sin tocar el control): con el
  arreglo, la sala, Conker y los objetos en su sitio (0 comandos sin implementar, 1156 triángulos
  por tarea); `--lle-check-gfx` da 50 de 50 tareas «distintas», del mismo tipo que el resto de
  Conker con «F3DEXBG.NoN» (luz de la HLE más baja que la del LLE: razón HLE/LLE por canal 0,92,
  0,84, 0,77 en esta sala; 0,80, 0,79 en la cueva del Poo y 0,97, 0,96, 0,99 en la entrada del
  maizal, las dos con «F3DEXBG.NoN» y 50 de 50 «distintas» también). Sin ares: el estado no se
  puede llevar a ares y no hay partida del reportero para llegar a esa sala.
- Otros juegos: las 8 escenas del host (`tools/escenas-host.sh`) idénticas a main; Conker con
  «F3DEXBG.NoN» (título, Chapters, maizal y cueva del Poo, estados de #3) idéntico a main.
- Prueba: `rsp_detect_gbi_conker` (las ocho variantes con el CRC: F3DEXBG y su plano cercano; con
  el interruptor, el texto; otro código con el mismo texto, F3DEX2). Falta la PS3: el reportero,
  desde su estado (o desde la partida tras el ascensor), y con `conker-ucode-viejo` para ver la
  falla de antes.

## 2026-11-02 — #69, FIFA 99: pantalla negra con «Continue» tras el partido (espejo del framebuffer)

- Reporte (v0.2.15, HEN): al acabar un partido y darle a continuar, pantalla negra. En el log, 13
  campos después de la A (el juego ya copiaba el código de la pantalla siguiente: 7638 bloques
  invalidados por escrituras del CPU en 0x0e08b8–0x11fe41): «cpu: fallo AdEL … pc 0x8005B344,
  dirección 0x10003»; después 92–98 % en bucles de espera, ninguna tarea gráfica y audio en
  silencio (el hilo del juego quedó parado).
- Host, con el estado del reporte (la «Post Game Menu» con «Continue» elegido) y la A: el mismo
  AdEL en el mismo pc y la misma dirección con el intérprete, también con `-DSALSA64_CPI=2` (no es
  del ritmo del CPU). Volcado crudo: el hilo 1 (prioridad 60) parado en 0x8005B344, que es el
  `free` del heap del juego (bloques con cabecera de 16 bytes antes del puntero: «BM», banderas,
  tamaño, siguiente, anterior); le piden soltar 0x801abc80 (el segundo framebuffer del partido,
  de 0x800ce0a8) y lee el anterior en 0x00010001: los 64 bytes 0x1abc40–0x1abc7f, cabecera
  incluida, son negro de RGBA5551. Ya están así en el estado guardado.
- Causa, en el log de la PS3: al empezar el partido (campo ~4810, nueve minutos antes) el juego
  cambia los framebuffers de 512 × 384 del menú (0x157640, 0x1b76c0) por los de 480 × 360 del
  partido (0x157640, 0x1abcc0) en la misma memoria; el primero acaba en 0x1abc40 y la cabecera del
  bloque del segundo queda en 0x1abc70. «espejo del framebuffer a la RDRAM 0x157640–0x1b7640 (ancho
  512…) de la tarea 3678 (3679 terminadas): 1 de 3456 líneas … sin escribir (la primera en
  0x1abc80)»: el espejo del último cuadro del menú llegó después de la primera tarea del partido,
  que había renovado la huella de la línea 0x1abc00 (sus filas llegan a 0x1abc40) con la cabecera
  ya escrita; la guarda la dio por buena y el espejo escribió encima su negro (fila 337, columnas
  256–287). La línea 0x1abc80, que esa tarea no tocó, sí se salvó.
- Referencias: en el host, reponiendo la cabecera en el estado (`--poke` con
  `424d0140 00054640 802002f0 801575f0`, sacada de los bloques vecinos) «Continue» sigue a la tabla
  de posiciones de la liga; y el recorrido del log desde el arranque (los botones de la PS3 como
  `--pad`) llega al mismo partido (AC Milan – R Madrid) con esa misma cabecera intacta (el host
  escribe la RDRAM al dibujar, como el RDP, y no tiene espejos tardíos); al acabar el partido
  (campo ~36600), START y la A en «Continue» siguen a la tabla de posiciones sin excepciones.
- Arreglo (`rdp_claim_ring`, targets.h): al terminar cada tarea el hilo de emulación anota, junto
  con las huellas, qué filas de qué color image (dirección y bytes por fila) pudieron tocar sus
  dibujos (anillo de 64); el espejo del framebuffer de la tarea T (contada como `rdp.finished`,
  `slot_fin` por ranura) no escribe las líneas que una color image distinta dibujó en una tarea
  posterior. La misma color image vuelta a dibujar y otra de la misma tarea no cuentan; si el anillo
  perdió entradas, como antes. Solo el espejo del framebuffer. Interruptor `espejo-pisa-otras`; el
  log de 5 s suma los espejos y líneas recortados así. Prueba `targets_claims` con la memoria del
  juego. Falta la PS3 (un partido nuevo: el estado del reporte ya trae la cabecera rota).

## 2026-10-31 — #3, Conker: el Great Mighty Poo no abre la boca (PLAY sin confirmar, arreglo por juego)

- Reproducido en el host con el estado de la ronda anterior en Chapters (partida nueva,
  WELDERSBENCH, Sloprano marcado) y Sweet Melody sin tocar el control: el Poo aparece, la boca no
  se mueve, la escena se queda y la música cae desde el campo ~16950 (RMS ~1000 contra 3000–4000).
- Causa (vigía de la RDRAM y de instrucciones, build con `SALSA64_WATCH_MEM`; el código del juego
  va por la TLB en 0x10000000 → física 0): el gestor de música (0x1000D2F8, hilo 3, una vez por
  cuadro y canal) guarda por canal un registro de la canción (raíces en 0x800417B0). Para empezar
  una canción pone el registro y manda PLAY (0x100084D8 → 0x10017AA0: un evento en la cola del
  reproductor 0x8007B820, que el hilo 4 procesa en su cuadro de audio, cada 2 campos). En cada
  cuadro, si ve el reproductor parado (+0x2C = 0), da la canción por terminada y borra el
  registro. Al entrar a la cueva: PLAY en el campo 13150, el cuadro siguiente del juego llega en
  el 13151 (1,28 M ciclos después), ve el reproductor parado y borra el registro; el hilo de audio
  procesa el PLAY en el 13152 (2,84 M ciclos después del PLAY). La canción suena huérfana; en el
  jefe (campo 16944) el juego la vuelve a pedir, la para (espera activa en 0x10008CE8 hasta el
  16952), manda PLAY y en el 16953 vuelve a borrar el registro (procesado en el 16954). Sin
  registro el gestor ya no atiende la canción (ni llama a su función, tabla en 0x8002B07C): el
  Poo no abre la boca y la escena espera.
- Descartes y variantes: la HLE de audio no toca el estado del reproductor (lo cambia el hilo 4
  en el CPU). Con `--lle=all --lle-check-audio` (385 de 2450 tareas con diferencias, sobre todo
  MP3 de voces) este recorrido funciona, pero por fase: el PLAY sale en el 13151 y se procesa en
  el 13152, antes del cuadro del gestor. Igual entrando a Chapters desde el arranque (partida del
  autor y los bytes 0x800E9D01–04 = 0xFF de la base de trucos de mupen64plus): PLAY en el 8151,
  procesado en el 8152. Con `-DSALSA64_CPI=2`, sin arreglo, el recorrido malo funciona (el Poo
  abre la boca y la escena sigue). El fallo depende de en qué fase cae el PLAY respecto del
  cuadro de audio.
- Referencia (ares por GDB, intérprete, mismo recorrido por Chapters desde el arranque; el control
  se fuerza parcheando `osContGetReadData` y el campo es el contador de retrazados del juego,
  0x800429BC; el guion va 300 campos más tarde que en el host): registro creado en el 8519, PLAY en el 8529 (línea 0), procesado en el 8531 y el
  siguiente cuadro del gestor en el 8533 (línea 166), ya con el reproductor sonando. En la consola
  del PLAY al siguiente cuadro del gestor pasan ~4,3 campos, contra 0,8–1,0 en el host: el cuadro
  de audio (cada 2 campos) siempre llega antes. La canción entra 631 campos después de elegir
  Sweet Melody en ares y 552 en el host (con y sin arreglo).
- Arreglo (`core/gamefix.c`, tercer arreglo por juego): la función que manda PLAY (0x100084D8,
  25 palabras, solo con el código original y la ROM USA NFUE) hace lo mismo y además espera a
  que el estado deje de ser 0, como el juego ya espera el STOP en 0x10008CE8 (tope de 0x1E0000
  vueltas, ~6 campos). Interruptor `conker-musica-vieja` (en el host,
  `SALSA64_GAMEFIX_OFF=conker-musica-vieja`). Con el arreglo: PLAY en el 13150, la función
  vuelve en el 13152 tras el PLAY procesado, el registro sigue, el Poo abre la boca y la escena
  sigue; por Chapters desde el arranque, igual (vuelve en el 8152). Con el interruptor, el registro se
  borra en el 13151 como antes.
- Pruebas: suite gamefix (palabras exactas, interruptor, otra región y otro CRC intactos, y la
  función parcheada corrida en el intérprete: espera con el reproductor parado, sin PLAY si suena,
  PLAY sin esperar en pausa, tope). Falta la PS3.

## 2026-10-30 — #51, Donkey Kong 64: el DK Rap sin música (gestor de música, arreglo por juego)

- En el host, sin tocar el control y con la HLE: la música de la intro se corta hacia los 12 s
  del audio del AI y entra a los ~106 s (solo voces y ruidos entre medio), igual que en el reporte.
  `--lle=all` la arregla; `--lle=all` solo para el audio no (la HLE de audio no es: naudio da lo
  mismo que el microcódigo salvo dos bytes de un búfer de retardo), ni la duración de la tarea de
  audio. Lo decide el ritmo de los cuadros: una sola tarea gráfica (la primera del juego, campo
  176) que dure ≥ 361 000 ciclos, o `rsp-ritmo` al 100 % (al 96 % ya falla: margen del 4 %).
- Causa (volcado de la RDRAM y vigilancia de escrituras, sin material de Nintendo): el gestor de
  música (0x80602600, una vez por cuadro) pide la canción 0x4B en la casilla 0. Carga y PLAY
  (0x8060a6a0) dejan el estado de la petición (0x80745924) en 1; pasa a 2 cuando ve el
  reproductor en AL_PLAYING. Si lo ve parado con estado ≠ 2, lo para, vuelve a cargar la canción
  (0x10123098, 0x125E bytes) y manda otro PLAY. Al cambiar a la escena del rap (campo ~826) cabe
  un cuadro de un retrazado (retrazados 12, 2, 1, 2…); desde ahí el hilo principal corre en la
  fase de campo contraria y cada recarga cae ~100 000 ciclos antes del cuadro del hilo de audio:
  el PLAY nunca se confirma y la canción se recarga cada cuadro durante ~96 s, hasta que la fase
  cambia. En ares (GDB, puntos de parada en la carga y en el paso a estado 2): una sola carga en
  el retrazado 911 y estado 2 en el 913.
- Arreglo (`core/gamefix.c`, ahora con varios arreglos por ROM y un interruptor por arreglo): 20
  palabras en 0x806029DC–0x80602A28; con estado 1 el gestor pasa a la siguiente casilla en vez de
  recargar; 0 carga y 2 siguen igual. Interruptor `dk64-musica-vieja` (en el host,
  `SALSA64_GAMEFIX_OFF=dk64-musica-vieja`). Con el arreglo, la música entra a los 12,75 s del audio
  sin `rsp-ritmo` y con él, y la canción se carga una vez.
- Falta la PS3.

## 2026-10-31 — #62, Dr. Mario 64: paneles en blanco (el bit de alfa es la cobertura)

- Reporte (v0.2.15, fotos en la PS3): en la partida salen en blanco el interior del frasco, el
  marcador (TOP/SCORE/TIME), el recuadro de Dr. Mario y «MED», con una mancha oscura y una raya
  roja que tiembla en el frasco; con `copia-sin-alfa` todo bien; en la v0.2.14 también.
- Datos (traza de la HLE y volcados del host): al entrar a la partida (y a la demostración que
  sale sola tras el título) el juego dibuja una vez el fondo en una imagen de 328 × 240
  (`214e00`): fichas CI8 en ciclo 1 (CLAMP, FORCE_BL) y encima los paneles translúcidos en XLU
  (othermode L `0c184240` y `00504241`: CVG_DST_FULL, FORCE_BL, IM_RD) con el alfa de una máscara
  I4 o el del primitivo, 0,375. En cada cuadro la pega en COPY con prueba de alfa (tiras de
  328 × 6) leyéndola de la RDRAM (destino viejo).
- Causa: en una imagen de 16 bits el RDP guarda en el bit de alfa la cobertura, no el alfa de la
  mezcla (ZAP: 7, el bit en 1). El RSX guardaba el alfa del fragmento: `RSXKIT_DSTA_ONE` ponía la
  mezcla del canal alfa en ONE / ZERO, que lo copia (el «alfa 1» de 1d30178 nunca fue tal). Desde
  la v0.2.15 ese alfa llega a la copia (49063599): los paneles quedaban con el bit en 0, la copia
  COPY los descartaba y se veía lo que había debajo (blanco, y en el frasco la estela de la
  píldora). El host no lo reproducía por dos diferencias con la PS3: suponía alfa 1 y no copiaba a
  la RDRAM imágenes de otro ancho que el VI de más de 64 KB (el fondo salía de la RDRAM sin
  dibujar).
- ares (demostración, volcado en la RDRAM): `214e00` con el bit en 1 en las 76 800 posiciones
  dibujadas. El host con el arreglo: idéntico bit a bit (9 píxeles con el color a más de 2 pasos);
  con el alfa del fragmento (la v0.2.15), 19 789 en 0 (los paneles).
- Arreglo: la HLE calcula el bit por dibujo (`gfx_draw.cvg_bit`, en el relleno de `prim_tile`,
  que pasa a 16 bits: el tamaño del dibujo no cambia): 1 en CLAMP y ZAP, el de la memoria en WRAP
  y SAVE con IM_RD, el del alfa con CVG_X_ALPHA sin FORCE_BL, el mayor con CVG_X_ALPHA, FORCE_BL
  e IM_RD; el texel en COPY y el color en FILL (docs/framebuffer.md). rsxkit: `RSXKIT_DSTA_ONE`
  da 1 de verdad (dos pasadas si el alfa del fragmento no es 1: ver abajo) y hay
  `RSXKIT_DSTA_KEEP` (máscara sin el alfa) y `RSXKIT_DSTA_MAX` (ecuación MAX). `src/rdp/rdp.c`
  los usa; interruptor `alfa-fragmento` (todo con el alfa del fragmento, como la v0.2.15); el log
  de cada 5 s cuenta los dibujos por modo. Host: lo mismo (`SALSA64_ALFA_FRAGMENTO=1`) y copia
  las imágenes grandes de otro ancho como la PS3.
- `salsa64-run --ares` compara además el bit de alfa con el de la consola
  (`SALSA64_ALFA_EN_TODAS=1` en todas las de 16 bits). En 12 volcados de ares (Dr. Mario 64,
  LEGO Racers, Star Fox 64, Conker, Hydro Thunder, GoldenEye, Super Odyssey 64, Snap) la
  cobertura acierta 94–100 % de los píxeles y el alfa del fragmento 3–100 % (GoldenEye 3 %);
  lo que queda son bordes con cobertura parcial (el RSX no la tiene).
- Verificado en el host: Dr. Mario 64 en la demostración y en una partida clásica (pad desde el
  título) sin paneles en blanco; con `SALSA64_ALFA_FRAGMENTO=1` salen como en las fotos de la
  PS3, con la mancha y la raya roja en el frasco (la estela de la píldora: el fondo transparente
  deja ver los cuadros anteriores). LEGO Racers: en el volcado de ares de la carrera el bit de
  las imágenes de los autos (32 × 32) coincide en 997 de 1023 píxeles (los otros, bordes) y el
  cuadro (`--pasadas 2`) da lo mismo que main (32,31 dB en 4×4); `raster_alpha_copy` pasa.
  De 20 escenas del host (las 9 de `tools/escenas-host.sh`, F-Zero X en dos, Worms, Mario
  Tennis, Paper Mario, Smash, Snap, Conker, LEGO Racers y el título y la demostración de Dr.
  Mario), 19 idénticas a main; cambia la demostración de Dr. Mario (en main el host no copiaba su
  fondo y salía ruido). Con el alfa del fragmento las mismas 19 siguen idénticas: en lo medido,
  lo único que cambia en la PS3 respecto de la v0.2.15 es Dr. Mario. 821 pruebas unitarias sin fallos (nuevas
  `gfx_cvg_bit` y `raster_alpha_cvg`). Pila de los SPE: holgura 2304 / 2544 / 14448 / 3328 /
  2032 / 4656 / 2704 (32 bytes más de cadena y ~128 de código por imagen; mínimo 1536).
- PS3 (paquete de prueba con la primera versión, 2026-11-02): el fondo seguía en blanco con «77
  con alfa 1» en el log. La primera versión daba alfa 1 con el factor `GCM_CONSTANT_ALPHA` y eso
  no puede funcionar: la mezcla da As·fs + Ad·fd y el factor multiplica (As·Ac = el del
  fragmento). El host suponía «1» y por eso pasaba. Ahora rsxkit hace `RSXKIT_DSTA_ONE` en dos
  pasadas cuando el alfa del fragmento no es 1 en todo el dibujo: una solo en el alfa, con el
  mismo programa, vértices, tijeras y prueba de profundidad sin escribirla, y el alfa llevado a 1
  o más (×1/ref con la prueba de alfa contra 1, para descartar lo mismo; +1 en el término
  constante sin prueba), y después el dibujo con `KEEP`. Costo: un dibujo más (solo alfa, sin
  escribir z) por cada dibujo con alfa no constante en un destino con alfa; en Dr. Mario, 77 en
  la tarea del fondo. Al arrancar, `rsxkit: autoprueba dst_alpha ONE=… ONE+mezcla=… ONE+prueba=…
  ONE+descarte=… FRAG=… KEEP=… MAX=…/… (esperado ff ff ff 60 20 60 60/a0): bien|MAL` dice lo que
  deja cada modo en el RSX real; el informe de 5 s suma «alfa 1 en el RSX: N con la pasada del
  alfa, N en una pasada, N sin poder».
- Falta la PS3: la autoprueba en «bien», los paneles sin blanco, que LEGO Racers siga sin el
  cuadro y que no cambie nada visible en los juegos con render a textura (Mario Tennis, logos de
  Rogue Squadron). Aparte, las píldoras que no se ven mientras caen (otro trabajo).

## 2026-10-29 — Pila del SPE 0 tras unir F-Zero X afín y Vigilante 8 (rama pila-spe0)

- Con las dos ramas unidas (main 19f4cfa) el paquete no se armaba: holgura 800 / 1024 / 12944 /
  1824 / 608 / 3296 / 1280 bytes en las imágenes 0–5 y fase 8 (mínimo 1536; cada rama sola,
  2704 / 2672 / 14480 / 3360 / 2144 / 5216 / 2928 y 2848 / 2688 / 15216 / 4096 / 2656 / 5344 /
  2816). La cadena más honda no creció: fue código, ~2 KB por imagen de cada rama. De Vigilante 8
  en la imagen 0: YUV16 ~1,35 KB (`gfx_tile_texture` +880, `gfx_load_tile` +256,
  `gfx_load_block` +216), fondos escalados de S2DEX +272, blender +200, K0–K3 +132 en
  `gfx_rdp_command` (y −120 en `gfx_rdp_reset`).
- Todas las cadenas terminan en `begin_prim → gfx_combine 2288 → run_cycle 816 → p_mul 464` y
  empiezan con `main 736`. Combiner: `p_mul` sin polinomio temporal, `run_cycle` con dos
  polinomios en vez de cuatro (mismas operaciones, mismo orden) y `build_terms` aparte con
  `struct need` de 8 bytes: −1152 de cadena. Envoltura: `src/spu/gfx/main.c` con
  `-fno-move-loop-invariants -fno-tree-loop-im` (del bucle de `spukit_run` GCC sacaba constantes
  a 44 registros que `main` guardaba en la pila): `main` 736 → 224, −224 de código.
- Holgura: **2592 / 2832 / 14608 / 3488 / 2400 / 4960 / 3120**. Nada cambia por vértice, por
  triángulo ni por texel; una llamada más por combiner calculado y unas 10 operaciones más por
  trabajo del SPE.
- Descartado con números: sacar la conversión YUV de `texel()` a una función por fila (+1,2 KB de
  código), `bg_draw` con `noinline` (la cadena sube 272), `build_terms` sin `needs[]` (−224 de
  cadena, +232 de código).
- Verificado: 3 millones de combiners al azar idénticos byte a byte con el código anterior;
  806 pruebas unitarias sin fallos; 14 escenas del host idénticas a main (las 9 de
  `tools/escenas-host.sh`, F-Zero X en los campos 3000 y 4500, Luigi Raceway, Dam y el castillo de
  SM64). Falta la PS3.

## 2026-10-29 — #34, LEGO Racers: el cuadro alrededor de los autos lejanos (alfa en las copias a la RDRAM)

- Reporte (v0.2.14): los autos se ven bien casi siempre, pero al empezar una
  carrera y a veces en juego aparece un cuadro que parpadea alrededor de
  ellos. ROM del log igual al respaldo propio (CRC-32 `39407C9F`).
- Datos: el juego dibuja cada auto lejano en una imagen de 32 × 32 (o 64 ×
  64) que borra con un relleno 0x0000 (alfa 0) y la pega en el cuadro como
  textura RGBA16 con prueba de alfa (dos triángulos, `atest 1/1`). En la
  RDRAM de un volcado de ares de la carrera (de la rama del 2026-10-26) el
  fondo de las cinco imágenes es 0x0000 (814–908 de 1024 píxeles con alfa 0)
  y el auto lleva alfa 1. En el log del usuario, «texturas desde un
  destino: … de la RDRAM» llega a 532 (423 decodificadas en el PPE) en los
  5 s de cada salida y a 47–228 en algunas ventanas de la carrera: cuando
  una de esas imágenes lleva más de `RDP_TARGETS_FRESH` tareas sin
  redibujarse, la textura se decodifica de la RDRAM.
- Causa: las copias del RSX a la RDRAM (espejos de imágenes chicas, copias
  de la tarea) pasaban por RGB565 y `rsxkit_pixels_565_to_5551` pone el bit
  de alfa en 1: leída de la RDRAM, la imagen salía opaca con su fondo negro.
  El canal alfa de los destinos (1d30178) ya hacía bien el camino del RSX.
  No es el arreglo de mezclas de Vigilante 8 ni la profundidad del
  2026-10-26.
- Arreglo: `rsxkit_target_read` acepta `RSXKIT_ARGB8888` (el alfa del
  destino, filtrado como el color; 1 si no tiene) y `src/rdp/rdp.c` copia así
  los destinos con alfa que no son framebuffer (`rb_argb`), con el bit de
  alfa por la mitad; si la copia de 32 bits no cabe en su búfer, la de
  antes. Interruptor `copia-sin-alfa` (`salsa64_rdp_set_copy_alpha`); el log
  de cada 5 s suma «copias a la RDRAM con su alfa».
- Host: el rasterizador no tenía alfa en los destinos (el auto lejano salía
  siempre en su cuadro). Ahora lo lleva como rsxkit, en el byte alto de cada
  imagen (`RGB_TRANSP`), y lo copia a la RDRAM igual que la PS3
  (`SALSA64_SIN_ALFA_DESTINOS=1`, `SALSA64_COPIA_SIN_ALFA=1`). Con el pad de
  la rama del 2026-10-26 hasta la carrera: main saca el cuadro en cada
  cuadro con un auto lejano; con la rama, sin cuadro; con
  `SALSA64_COPIA_SIN_ALFA=1`, el cuadro vuelve solo en los campos en que la
  textura sale de la RDRAM (2810, 2870–2890: 0,07–0,14 % de la imagen), que
  es lo que veía la PS3.
- Sin regresiones: escenas del host (SM64, Banjo, Zelda título y casa, DK64,
  PD, GoldenEye, MK64, Kirby, F-Zero X en dos escenas) idénticas byte a byte
  a main; solo cambian Worms y LEGO Racers. Unitarias en Docker 790, 0
  fallidas; prueba `raster_alpha_copy` (fondo 0x0000 con alfa, 0x0001 con
  los dos interruptores, y el cebado que conserva el bit). `--lle-check-gfx`
  pasa el color sin el byte del alfa.
- Falta la PS3: el costo de la copia ARGB8888 (el doble de bytes y la
  conversión en el PPE; en LEGO Racers son imágenes de 2–8 KB) y que el
  cuadro no vuelva al empezar la carrera.

## 2026-10-29 — #57, Worms Armageddon inclinado: el paso de las filas de los fondos de S2DEX

- Reporte (PAL y USA, v0.2.14): el logo de inicio y todo el texto salen con
  rayas y «todo se ve inclinado». ROM del log (USA, CRC-32 de toda la ROM
  `5471AE3B`) igual a la del respaldo propio.
- En el host sale igual: cada fila de la imagen corrida un texel más que la
  anterior (las letras en cursiva y lo que sale por un borde vuelve por el
  otro). `--lle-check-gfx` en los primeros 300 campos: 255 de 256 tareas
  distintas del microcódigo real («dibuja de más»), con los mismos 12 496
  rectángulos; con el microcódigo real la imagen sale derecha, como en
  mupen64plus.
- Datos: todos los fondos son `G_BG_1CYC` (S2DEX 2.08) con `imageW` una
  columna más ancho que el marco: 321 × 241 para un marco de 320 × 240 (y
  41 / 36, 145 / 144), CI8 con LOADTILE. Los comandos del RDP que entrega el
  microcódigo real: `SETTIMG` de 160 texels de 16 bits (filas de **320**
  bytes), `LOADTILE` de 165 texels (lee 330 bytes desde cada fila: la
  columna 321 es la primera de la fila siguiente) y el puntero avanza 1600
  bytes cada 5 filas. La HLE pedía `SETTIMG` de 321 texels de 8 bits: paso
  de 321 bytes, un texel de más por fila.
- Causa y arreglo (`src/rsp/gfx/s2dex.c`, `bg_stride`): el paso entre filas
  es `imageW` redondeado hacia abajo a palabras de 64 bits (lo que el
  microcódigo toma de `imageW >> (6 − siz)`, el mismo cálculo que deja
  `guS2DInitBg` en `tmemSizeW`, que el juego también trae en los bytes 36–37:
  `0x50` = 80 palabras de 32 bits). Solo cambia algo cuando la fila no llena
  palabras de 64 bits; los fondos de 320 × RGBA16 (Zelda, RE2) o de ancho
  múltiplo de 8 bytes quedan igual.
- Medido: `--lle-check-gfx` 1500 campos (logos, título y menú de idioma):
  1454 de 1454 tareas idénticas al microcódigo real (antes 1 de 256 en los
  primeros 300). La imagen del VI en el campo 300, idéntica byte a byte a la
  del microcódigo real.
- Interruptor `GFX_JOB_S2DEX_BG_ROW_OLD`: archivo `s2dex-paso-viejo` en la
  PS3, `SALSA64_S2DEX_PASO_VIEJO=1` en el host (vuelve a la imagen inclinada,
  idéntica a la de main). Prueba `gfx_s2dex_bg_row_stride` (imagen RGBA16 de
  filas de 40 texels declarada de 41, y el interruptor).

## 2026-10-29 — Paper Mario: los cuartos oscuros (luz de Watt) salían negros; Mystical Ninja (Undub): créditos

- **Paper Mario, efecto.** La luz de los cuartos oscuros (Bowser's Castle,
  `kpa_01`, «Dark Cave») la arma el RDP en cada cuadro, en la tarea principal:
  color image RGBA32 de 64 × 64 en 0x400 (encima del z-buffer, que va de 0x400
  a 0x25C00), un cuadro blanco y dos cuadros (uno girado) con la textura de luz
  I8 de 0x14D500 en espejo, negro con alfa (PRIM_A − T0a)·PRIM_A y mezcla
  IN·A + MEM·(1 − A): queda la intensidad de la textura; la misma memoria como
  color image I8 de 64 de ancho y cuatro TEXRECT 1:1 que copian cada franja
  RGBA32 de 64 × 16 a su fila de 8 bits (el RDP escribe el rojo; la copia en
  el lugar funciona porque cada franja escribe detrás de lo que lee); LOADTILE
  de esa I8 a la TMEM; un FILLRECT de 0xFFFC en modo FILL sobre la imagen de
  32 bits (devuelve el z-buffer) **antes** del dibujo que la usa; y un TEXRECT
  de toda la pantalla (dsdx 2, shift ×2, espejo y clamp: la máscara cubre 32 px
  de lado) con color negro y alfa 0,749·I + 0,247. Además la tarea chica que
  abre cada cuadro borra el z-buffer entero (0xFFFC).
- **Causa.** La HLE decodifica cada carga a la TMEM de la RDRAM: las franjas
  RGBA32 y la máscara salían del z-buffer (0xFFFC → alfa ≈ 1): todo negro salvo
  el HUD, también con el poder de Watt. En la salida de la HLE la I8 final no
  puede ser render a textura (máscara dentro del clamp) y las franjas RGBA32
  tampoco (solo RGBA16 e I8 lo son); y aunque lo fueran, el relleno de 0xFFFC
  llega entre la carga y el dibujo.
- **Arreglo.** `src/rdp/mask32.c` (portable): las color images de 32 bits de
  hasta 64 × 64 y la de 8 bits en su misma dirección y ancho se dibujan en el
  PPE (en `done`, como layer8) sobre una ventana de su memoria, en el orden de
  la tarea y con la semántica del rasterizador del host (perspectiva afín,
  combinación lineal o términos, prueba de alfa, mezcla con la memoria; los
  dibujos que leen la misma memoria la leen de ahí, copiada al empezar cada
  dibujo como la TMEM). La RDRAM recibe la I8 (4 KB): la HLE de la tarea
  principal siguiente la carga, un cuadro tarde. Los rellenos que pisan esos
  bytes en las tareas que siguen (el borrado del z-buffer de la tarea chica)
  se deshacen ahí hasta 4 tareas (`MASK32_KEEP`); es lo único que se aparta de
  la consola (el CPU vería la máscara en 4 KB del z-buffer). Esas imágenes no
  van al RSX ni se copian (lista `sw8`). Camino entero por píxel (sin
  conversiones entre coma flotante y entero, que en el PPE pasan por la
  memoria) para combinación lineal con color constante y sin filtro; si no,
  el de coma flotante. Interruptor `gfx-mascara32-rsx` (PS3) y
  `SALSA64_MASCARA32_RSX=1` (host). Línea de log «gfx: imágenes chicas de 32
  bits en el PPE: tareas, dibujos, píxeles, ms por tarea; bytes a la RDRAM y
  repuestos». Host: 11 ns/px (≈ 15 000 px por tarea); falta medir en la PS3.
- **Cómo se reprodujo sin la ROM europea.** El estado del reporte es de la
  versión europea (otro código y otras direcciones en la ROM). De su FlashRAM
  (sección CART del estado) salió una partida para la ROM de EE. UU. (mismo
  formato «Mario Story 006»; suma de palabras con crc1 = 0 y crc2 = ~0), con
  `areaID`/`mapID`/`entryID`, la posición y el compañero cambiados: Watt en
  `kpa_14` frente a la puerta con llave que lleva a `kpa_01`. Guion con
  `--pad`: START, START, A, A (archivo 1), derecha, A (llave), A, A (puerta),
  derecha; C-abajo en la cueva.
- **Contra ares.** El mismo guion en ares v148 (instancia propia, GDB en otro
  puerto; los botones se escriben en `OSContPad[0]` con un punto de parada de
  escritura en cada lectura del control): en `kpa_01` se ve un círculo chico
  alrededor de Mario (15 × 19 px de píxeles claros en ares y en la rama; sin
  el arreglo, nada) y con C-abajo el cuarto entero se ilumina igual en los
  dos. Sin el arreglo, C-abajo sigue en negro (media 0,0 contra 18,0 de la rama
  y 20,9 de ares; la diferencia es el alfa de 5 bits del blender: en ares lo
  oscuro deja ver ~3 % del cuarto con tramado).
- **Mystical Ninja Starring Goemon: créditos negros y música detenida.** El
  estado del reporte es de una ROM «Undub» (CRC 9FB23339-E6B70EAC; no es la de
  EE. UU., FCBCCB21-72903C6B; difiere en los megas 0 y 5–15). En el estado, el
  hilo de carga (id 4, prioridad 9) descomprime el archivo 1154 de la tabla del
  juego (en RAM, 0x800573C8): la tabla del parche lo pone en 0xE4E8FC con fin
  en 0xE5DA08 (el inicio del 1155, que no movió): 0xF10C bytes en vez de los
  ~0x80 de su contenido. El descompresor (0x80005394) solo termina por bytes
  leídos: tras el bloque bueno lee como cabecera los bytes que siguen
  (0x8B450FC0: tipo 8, ~189 MB) y escribe sin fin más allá de la RDRAM, por
  los registros del RCP (VI: negro; AI: silencio). En la consola pasaría lo
  mismo con esa ROM. No es un fallo del emulador; pedir la prueba con la ROM
  de EE. UU. sin parche.
- Pruebas: `raster_mask32_light` (la máscara byte a byte ±1, los dos caminos,
  el resto del z-buffer intacto, la reposición tras el borrado de la tarea
  siguiente, el fin tras `MASK32_KEEP`, olvidar) y `raster_mask32_refused`.

## 2026-10-29 — rsxkit unificada con SalsaDS (rama rsxkit-unificar)

- `lib/rsxkit` es ahora la misma biblioteca, byte a byte, en Salsa64 y en
  SalsaDS (las dos copias habían divergido desde 61ea9f43, el árbol que
  SalsaDS tomó por subtree). Entra lo de SalsaDS (stencil, programa del
  usuario con etiqueta de uso, COLOR0 auxiliar, constantes incrementales,
  mezclas con máximo y mínimo del alfa, pruebas de z GEQUAL/ALWAYS/EQUAL,
  `RSXKIT_ARGB8888`, `RSXKIT_CLEAR_ALPHA`, `out_target`) y se conserva todo lo
  de Salsa64. Donde chocaban (destinos con alfa, blit con alfa, `set_depth`)
  queda una sola forma, documentada en el README de la biblioteca; la API de
  Salsa64 no cambia y `src/` no se toca.
- Para Salsa64 lo único distinto que llega al RSX: `rsxSetBlendEquation` en
  cada cambio de mezcla y en el estado 2D, tramado y stencil apagados en el
  estado fijo del 3D, un fragment program menos (el blit con alfa) y las
  etiquetas FINISH/FENCE en 0 al arrancar.
- Verificado: 785 pruebas unitarias del host (0 fallidas); las 8 escenas del
  host idénticas a main c006f36 (sin SM64, que no está en el conjunto);
  paquete `build/salsa64-rsxkit/salsa64-rsxkit.pkg` («Salsa64 rsxkit»,
  SLSA00215). Falta la PS3 real: guion en `build/rsxkit-unificar/guion-ps3.md`
  e informe completo en `build/rsxkit-unificar/informe.md`.
- Publicar en el repositorio rsxkit: la historia del split de `lib/rsxkit` no
  tiene relación con la de rsxkit/main (62bf102), así que no sirve
  `subtree push`: hay que unir el split con rsxkit/main en un worktree aparte
  (ver el informe). En el repo, el README de la lib es `README.es.md`.

## 2026-10-29 — F-Zero X: el fondo blanco de la pista (recorte del RSP + sombra afín)
- Causa, medida contra el microcódigo real (F3DEX2 fifo 2.03 corriendo en la LLE del host desde el estado de carrera, 21 campos, 5215 triángulos del RDP): el RSP recorta cada triángulo contra la caja del clip ratio (viewport ×2) y el RDP interpola el sombreado lineal en pantalla dentro de cada pieza. El fondo de la pista son cuadriláteros con dos vértices a w ≈ 540 y 2000 px fuera de la pantalla (sombra 0) y dos a w ≈ 5560 (sombra 255): la HLE mandaba el triángulo entero y el RSX lo interpolaba con perspectiva, así que la sombra caía a 0 en casi todo lo visible y salía la textura clara (el «fondo blanco»); la bruma de la pista llegaba tarde por lo mismo.
- Recorte: Sutherland–Hodgman plano por plano recorriendo el polígono desde su primer vértice con el anterior (la intersección sale antes que el vértice que entra), derecha, abajo, arriba e izquierda al final (tras el plano cercano), abanico desde el primer vértice de la lista final y rechazo trivial de cada triángulo del abanico por los planos de la pantalla. 114 de 115 abanicos del microcódigo reproducidos (`build/fzero-afin/hipotesis*.py`, `fifo.py`); el que falta es una esquina que el RSP deja a 0,9 px. `gfx_triangle` (src/rsp/gfx/rdp.c): `clip_plane`, `clip_box_poly`, `emit_fan`; el plano cercano usa el mismo recorrido (la prueba `f3d_near_clip` cambió de orden).
- Sombra afín: el rasterizador del host interpola color y alfa con las baricéntricas de pantalla (`raster_affine`); rsxkit multiplica los X_j por w en el vertex program (constante `affine`) y los fragment programs los leen con el bit 31 de SRC2 (la entrada llega dividida entre w: docs/rsx-notes.md), precompilados en copia (`rsxkit_fp_upload_affine`) y generados con `rsxkit_fp_desc.affine`. Sin instrucciones de más por píxel; 7 multiplicaciones por vértice.
- Interruptor: `/dev_hdd0/salsa64/sin-sombra-afin` apaga las dos cosas (`GFX_JOB_NO_RSP_CLIP` + `salsa64_rdp_affine_shade(false)`); en el host, `SALSA64_SIN_SOMBRA_AFIN=1` (también en `--ares`).
- Medido contra ares (4×4): r04 18,95 → 31,4 dB, r15 25,55 → 30,46 dB (HLE − consola de −14/−10/−7 a +0,3/+0,4/+1,3); 30 volcados nuevos de la demo (`build/fzero-afin/ares/`): 14 listas mejoran (hasta +14 dB), ninguna empeora. Otros juegos (Conker, GE, Hydro Thunder, Star Fox, Snap, Odyssey, Banjo-Tooie: 52 listas con ≥ 12 dB): dentro de ±0,1 dB salvo túnel de GE +0,24, Snap +0,34, GE líneas −0,2 y tres listas a medio armar de Conker (≈12 dB antes y después). Escenas del host: 6 de 9 cambian (3D), como se esperaba.
- Pruebas: `gfx_rsp_clip_box` (test_gfx.c), descriptores afines en test_fpgen.c; `tools/build.sh -C tests/unit run` sin fallos (786 pruebas).
- Pila del SPE 0 (paquete «Salsa64 F-Zero afin»): holgura 2704 / 2672 / 14480 / 3360 / 2144 / 5216 / 2928 bytes en las imágenes 0–5 y fase 8 (mínimo 1536; en la v0.2.14, 4496 / 4080 / 16496 / 5376 / 4064 / 7008 / 4208): el recorte agrega 160 bytes al marco de `gfx_triangle` y ~1,5 KB de código por imagen (las funciones del recorte van con `-Os` en todas y el abanico es una hoja fuera de la cadena de `begin_prim`).
- Falta la PS3: paquete «Salsa64 F-Zero afin» (SLSA00217); mirar Mute City y la pista del estado de carrera (fondo de edificios), y que el resto de los juegos no cambie de aspecto (el bit del RSX medido antes solo con w = 1 y con la luna de Zelda).

## 2026-10-29 — Indiana Jones, segunda ronda: la lista secundaria de Factor 5 acaba en cualquier G_ENDDL (rama `indy-rsx`)

- En la PS3 con el paquete de la primera ronda: sin cuelgue, pero tras salir
  de la cascada Indy invisible y la mitad derecha negra; 20 avisos «color
  image 880000/18 fuera de la RDRAM». Las cajas negras: los dibujos de Indy
  van a 880000/18 (z 0x820000) y un rectángulo (240,0)–(670,430) con
  s = t = 0 pinta de negro la derecha. `FFB00011 00880000` (formato 5, que
  no existe) es un vértice de 8 bytes (−80, 17, 136, 0): la HLE ejecutaba
  vértices como comandos del RDP al seguir la lista secundaria (DMEM 0x58C)
  más allá de un `B8000000 00000000` hasta un B8 … FFFFFFFF.
- Comprobado con `--lle=all` en el host (estado de una partida del autor en
  el mismo nivel, reproducida con las entradas del log): una lista de
  material cambiada para acabar en B8/0 con un G_SETCIMG detrás: la HLE
  vieja lo ejecuta, el microcódigo real no; dentro de la lista, los dos.
- Arreglo en `do_sub_dlist` (f5indy.c): cualquier G_ENDDL termina la lista;
  `f5-sublista-vieja` / `SALSA64_F5_SUBLISTA_VIEJA=1` vuelve a lo de antes.
  Unitarias 791, 0 fallos (2 nuevas en `gfx-f5indy`); escenas del host 34
  de 34 idénticas a main; Indy en juego (12 imágenes desde el estado)
  idéntico con y sin el arreglo. Falta la PS3: salir de la cascada.

## 2026-10-29 — Indiana Jones: el RSX dejó de avanzar tras una color image fuera de la RDRAM; blindaje y datos de un cuelgue (rama `indy-rsx`)

- En la PS3 real, tras once minutos de juego, al salir por segunda vez de
  una cascada: la HLE pidió el destino 900000/455 (fuera de los 8 MB de
  RDRAM; 341 = alto por omisión, 44 = tijeras), el PPE quedó esperando la
  barrera que `zfeed` pone tras una carga de la z (GET en 0, PUT 0x3dde4)
  y a los 12 s el cierre forzado dejó la consola congelada. Análisis y lo
  que queda como hipótesis en docs/rsx-notes.md: lo único mandado entre lo
  último que el RSX terminó y esa barrera fue, a lo sumo, la carga de la z
  de 455 × 341 a 2× (la primera de ancho impar de la sesión; las 121
  anteriores, de alto impar y ancho par, pasaron).
- Blindaje (encendido; `rsx-sin-blindaje` lo apaga, `SALSA64_RSX_SIN_BLINDAJE`
  en el host): las color images que empiezan fuera de la RDRAM no tienen
  destino (`rdp_cimg_impossible`, también en `raster.c`) y
  `rsxkit_depth_load` usa anchos de entrada pares (`lib/rsxkit/src/sifm.c`).
- Si el RSX se queda igual: aviso de excepciones del sistema
  (`gcmSetGraphicsHandler`, al log desde el latido), y a los 2 s registros,
  barreras, búfer de comandos alrededor de GET y de PUT, rastro de las
  últimas 64 operaciones de rsxkit con su offset y lo último de destinos,
  profundidades y cargas de la z; antes de salir, `rsx-colgado-<campo>/`
  con `fifo.bin`, `gfxdump.bin` (la tarea en traducción) y `caja.s4bb` (la
  caja negra, sin comprimir), con fsync; después rsxkit abandona el RSX
  (PUT = GET, sin esperas ni vaciados). `rsx-colgado` = `reinicio` o
  `reinicio-directo` reinicia la consola en vez de volver al XMB
  (experimental).
- Pruebas: unitarias en Docker 789, 0 fallos (suite nueva `sifm`: tramos
  iguales a los de antes con anchos pares, el caso 455 × 341, sin lugar en
  el pitch, `rdp_cimg_impossible`); en `cpu-tests` (PPU) `test_sifm.c` y
  `sifm.c` compilan, y el programa no se arma por un error que ya trae main
  (`test_raster.c`: `CLOCK_MONOTONIC`, de 0d4a19d); escenas del host
  (SM64, Zelda OoT, Indiana Jones, F-Zero X, Rogue Squadron, 1500 campos con
  START/A, imágenes cada 250 campos): 34 de 34 idénticas a main y las mismas
  tareas. Falta la PS3: Indiana Jones en la cascada, y `colgar-rsx` para ver
  qué deja la consola usable al salir.

## 2026-10-28 — Yoshi's Story: camino rápido de layer8 (el genérico costaba 1 µs por píxel en el PPE)

- En la consola (`build/yoshi-rdp/ps3/yoshi3.log`) las esperas bajaron a 0,
  pero el raster genérico de layer8 salió a ≈1 µs por píxel (6,35 ms por
  tarea; una capa entera 86 016 px ≈ 90 ms, tres ≈ 300 ms: peor campo 309–316
  ms al cambiar de página). Por píxel hacía cinco divisiones en coma
  flotante, dos `floorf` con conversión a entero y dos módulos: todo lo que
  el PPE in-order paga caro. En el host el mismo camino da 11,3 ns/px.
- Camino rápido (`draw_rect_1to1`): dos triángulos que forman un rectángulo
  alineado con s función de x y t de y, con la textura a 1 texel por píxel
  y la muestra a medio texel (frac en [0,25, 0,75]) → copia de índices por
  fila con enteros (y `memset` para el relleno), prueba de alfa por byte;
  exacto frente al genérico (ningún redondeo flotante puede cambiar el
  texel) y comprobado byte a byte contra el rasterizador del host, con y
  sin el camino rápido. Host: 0,9 ns/px (12,5× menos que el genérico); en
  el PPE se esperan 10–20 ns/px: una tira 16 × 256 ≈ 0,05–0,1 ms, una capa
  entera ≈ 1–2 ms (tres ≈ 3–5 ms al cambiar de página). No hace falta un
  SPE. El log separa ms y ns/px de cada camino.
- Pruebas: `raster_layer8_equals_host` (más un rectángulo 2:1 y uno con
  origen a medio texel por el genérico, y la igualdad con el camino rápido
  apagado) y `raster_layer8_cost` (medida, no aserción). Unitarias 784
  (2 fallos previos de `test_cover`), `raster` 10 de 10.

## 2026-10-28 — Yoshi's Story: las capas de 8 bits las dibuja el PPE en la RDRAM (saltos de cámara)

- Medido en la consola (`build/yoshi-rdp/ps3/yoshi2*.log`): con las capas en
  el RSX, «la emulación esperó 59–106 tareas por 5 s, 7,4–9,7 ms c/u» sin
  filtro (hasta 13,5 ms con el CRT), 120–130 copias big8 por 5 s. Cada
  tarea que dibuja una capa esperaba a que el RSX la dibujara y la copiara
  (toda la cola por delante): peor campo 19–46 ms, cuadros saltados y audio
  estirado. Techo: 0,7–0,9 s de cada 5 s de emulación (15–18 %), concentrado
  en los campos que tocan una capa.
- Arreglo: `src/rdp/layer8.c` (portable) rasteriza en el PPE, directo en la
  RDRAM y dentro de `done` (hilo de emulación, antes de que la tarea
  siguiente las lea), los dibujos a una color image de 8 bits de más de 128
  de ancho que quepan en su semántica (tiles 1:1 sin filtro ni mezcla ni z,
  prueba de alfa, relleno constante, cualquier triángulo con w = 1); si un
  dibujo no cabe, ese destino va entero al RSX en esa tarea. Esos destinos
  no se copian ni se dibujan en el RSX (translate los salta y anota la
  escritura para que get_target cebe la superficie si otra tarea la lleva al
  RSX). Coste estimado en el PPE: una tira de 16 × 256 ≈ 0,05 ms; una capa
  entera (86 016 px) ≈ 1–2 ms, solo al cambiar de página.
- Prueba `raster_layer8_equals_host`: layer8 contra el rasterizador del host
  byte a byte (tiles 1:1, prueba de alfa, wrap, relleno, triángulo) y el
  rechazo con mezcla. Unitarias 783 (2 fallos previos de `test_cover`),
  `raster` 9 de 9. El host no cambia (raster.c intacto): escenas iguales.
- Bandera `gfx-capas8-rsx` (vuelve a lo de antes); línea de log «gfx: capas
  de 8 bits en el PPE: tareas, dibujos, píxeles, ms por tarea, esperas
  evitadas, destinos que quedaron para el RSX». Falta la PS3.

## 2026-10-28 — Yoshi's Story: los puntos de las capas de fondo eran la copia lineal a 1:1

- Cuatro cajas negras de la consola con el paquete «Salsa64 Yoshi»: las tiras
  de 336 × 6 que la HLE decodificó de la RDRAM (reconstruidas capa por capa
  en `build/yoshi-rdp/ps3/caja-2268/capa-*.png`) ya traen los puntos en cada
  borde y una cuadrícula en las zonas planas; ninguna de esas tareas dibuja en
  las capas (los puntos ya estaban en la RDRAM). Los únicos muestreos en ese
  camino son los tiles de 16 × 16 (ya sin filtro) y `rsxkit_target_read`,
  que copiaba con filtro lineal también a 1:1.
- Causa: el lineal a 1:1 no es exacto en el RSX (el vértice se ajusta a su
  subpíxel y la muestra cae a una fracción del centro): mezcla parte del
  vecino, y en una capa cuyo rojo es un índice de paleta un índice ± 1 es otro
  color. Arreglo en rsxkit: a 1:1 el más cercano (`rsxkit_read_filter_nearest`,
  con prueba en `test_fpgen`); `gfx-copia-lineal` vuelve al lineal. Nota en
  `docs/rsx-notes.md`. No cambia el host (no usa rsxkit): escenas iguales por
  construcción; unitarias 782, `fpgen` 5 de 5.

## 2026-10-28 — Yoshi's Story: las capas de 8 bits se ceban desde la RDRAM; inventario contra ares

- **Inventario (host contra ares, mismas escenas).** Intro y título (tres
  volcados propios de ares): 28,4–31,3 dB en 4×4, diferencias solo en bordes
  (ares a UHD). Nivel «Treasure Hunt» (volcado de ares de la rama
  `yoshi-reinicio`): 28,5 dB; la cuadrícula de puntos blancos del piso sale
  igual en ares (es del juego). Menú (sin controles no se llega en ares):
  el texto son 54 dibujos 3D que leen tiras CI8 de 328 de ancho de una imagen
  estática del juego, no de un destino del RDP. La RDRAM que la PS3 guardó
  en un estado el 21/10 dibuja el nivel igual (33,6 dB contra su propio
  framebuffer): las capas llegaban bien a la RDRAM entonces.
- **Capas de fondo (336 × 256, CI8, tres).** Medido en el host con
  `--trace-fb` y el rasterizador a 640 × 480: el juego las redibuja por tiras
  de 16 px (y enteras al cambiar de página) y las lee como fondo **en la
  misma tarea**; la HLE decodifica el fondo antes de que la tira llegue a la
  RDRAM (64 de 85 copias cambian 2–5 mil píxeles). En la consola es igual
  (`done` espera la copia, ~6 ms por tarea en el log del 28/10). Las tiras
  caen en el margen oculto: retraso de una tarea, no visible salvo al
  redibujar entera. Queda anotado, sin arreglo.
- **Defecto del camino de la PS3 corregido.** `prime_rows` solo cebaba
  destinos de 16 bits: una capa de 8 bits nueva (ranura reciclada) o vieja
  (más de 3 tareas sin dibujar) arrancaba con lo que tuviera el RSX, y la
  copia siguiente (caja que abarca varias tiras, o el redibujo entero) bajaba
  a la RDRAM lo que el RDP no dibujó: tiras negras o viejas en cielo, colinas
  y árboles. Ahora las big8 se ceban con el índice entero (XRGB8888), igual
  en `src/host/raster.c`; `gfx-big8-sin-cebar` en `/dev_hdd0/salsa64/` lo
  apaga. Prueba `raster_big8_primed`. Las de 8 bits chicas (sombras de
  Conker, cobertura de Zelda) siguen sin cebarse.
- **Log de la PS3.** La línea «copias big8» dice ahora cuántas copias se
  hicieron, cuántos píxeles de la RDRAM cambiaron y cuántas capas se cebaron.
- **Host.** `RASTER_W`/`RASTER_H` (320 × 240) recortan las capas de 336 ×
  256: las columnas 320–335 y las filas 240–255 nunca llegan a la RDRAM en el
  host. Para Yoshi, compilar con `EXTRA_CFLAGS="-DRASTER_W=640
  -DRASTER_H=480"`. Con `--trace-fb` la copia de una imagen de 8 bits dice
  cuántos píxeles cambió.
- **Volcados viejos de la consola** (`rsp-jit-ps3/yoshi-rsp18`, 16/10): el
  rayado de los fondos sale igual al repetirlos en el host (`--raster-dump`):
  venía de la RDRAM de entonces (capas sin copiar, anterior a big8), no del
  RSX.
- Pruebas: unitarias del host 781 (2 fallos previos de `test_cover`, ajenos),
  `raster` 8 de 8; escenas del host iguales antes y después; Yoshi del campo
  2500 al 2760 idéntico. Falta la PS3.

## 2026-10-28 — F-Zero X: la niebla de la pista se perdía en la HLE (blender en un ciclo)
- Reporte: en carrera no hay niebla por distancia y se ve cómo se construye la pista a lo lejos (en el original sí la hay).
- Cajas negras de la PS3 (Mute City, cuatro) y volcados de ares de las carreras de la demo: la pista va con `G_FOG`, color de niebla `FAFFFF`, `G_MW_FOG` 12800/−12544 (el factor sube de z/w 0,98 a 1,0) y othermode L `0xFA006220`: FOG·A_SHADE + IN·(1 − A_SHADE) en los dos ciclos, FORCE_BL, ALPHA_CVG_SEL, Z_UPD, **en un solo ciclo** (othermode H `0x082C00`). Los vértices de la salida de la HLE traen el factor en el alfa (69–180 en la pista lejana de Mute City) pero el dibujo salía `cyc 0 fog 0 blend OFF`.
- Causa: `blend_state` (`src/rsp/gfx/rdp.c`) solo reconocía esa mezcla en el primer ciclo de 2CYCLE (G_RM_FOG_SHADE_A). En un ciclo iba a `classify_blend` (sin MEM → sin mezcla) y `blend_constant` la dejaba pasar sin tocar: el rasterizador del host y el RSX dibujaban la textura tal cual. El RDP, con FORCE_BL, mezcla FOG·a + IN·(1 − a) en todos los píxeles sin leer la memoria: es la misma niebla.
- Arreglo: en 1CYCLE con ese blender y FORCE_BL, `d->fog = 1` con el color de niebla (el resto del camino, rsxkit y el rasterizador, ya la aplicaba). Sin FORCE_BL no cuenta (el interior escribiría P). Interruptor `GFX_JOB_NO_FOG_1CYCLE`: archivo `sin-niebla-1ciclo` en la PS3, `SALSA64_SIN_NIEBLA_1CICLO=1` en el host. Prueba `gfx_fog_1cycle_force_bl`.
- Medido contra ares (`salsa64-run --ares`, demo de cuatro jugadores): r04 15,46 → 17,53 dB por píxel (16,55 → 18,95 en 4×4), r15 18,27 → 20,51 (21,20 → 25,55); la pista lejana pasó de 20–66 unidades más oscura que la consola a 6–36. Las 9 escenas del host de otros juegos, idénticas byte a byte.
- Lo que queda: el RDP interpola el sombreado (y con él el factor de niebla) lineal en pantalla; la HLE y el RSX con corrección de perspectiva, así que en los triángulos largos de la pista la bruma llega más tarde que en la consola. Experimento en el rasterizador (factor de niebla afín, sin commitear): r15 20,51 → 21,27 dB (25,55 → 27,90 en 4×4) y la pista lejana a ±6 de la consola. Para el RSX haría falta la mezcla de niebla en el fragment program con el factor por w en un texcoord (la misma obra que la sombra afín pendiente del 2026-10-14). La bruma del horizonte (fondo enorme con sombra 0/255) sigue siendo ese problema.
- Paquete «Salsa64 F-Zero» (SLSA00212) para validar en la PS3: Mute City, recta con horizonte.

## 2026-10-29 — Costo del barrido de la caché de datos (#55 Forsaken 64)
- Reproducido en el host con el respaldo: 1100 tareas y después solo audio y VI, igual que en la PS3. El hilo
  principal (pri 10) manda un pedido (tipo 0xD) a su hilo del SI (pri 125, cola 0x8008E600, la misma en la que
  recibe el aviso del SI) y espera la respuesta en 0x8008E670. En ese momento el hilo del SI está dentro de
  `osContStartReadData` esperando la escritura de la PIF RAM, con `osRecvMesg` sobre esa misma cola: toma el
  pedido por el aviso del SI, el pedido se pierde y la respuesta no llega nunca. Hace falta que el juego haya
  hablado con el pak (la escritura de la PIF RAM solo va cuando el último comando no fue leer el control): la
  corrida «sin pak» de la PS3 mandó 161 comandos del pak; en el host con `--sin-pak` desde el arranque no manda
  ninguno y pasa la intro.
- Entre que el hilo del SI empieza a esperar y el pedido pasan 2355 ciclos en Salsa64: casi todo es
  `osWritebackDCacheAll` (512 CACHE 0x01, 2048 instrucciones). Con un modelo de cachés al estilo ares (40 ciclos
  por línea sucia escrita de vuelta, 40 por llenado, 48 por línea de instrucciones) ese tramo son 24 019 ciclos:
  479 líneas sucias. La escritura dura 6098 (65 µs): en la consola termina antes del pedido. ares v148 pasa la
  intro (logo del corazón y «PRESS START»). Con `SALSA64_SI_INMEDIATO` también pasa; con los 2 ms viejos no.
- Arreglo: CACHE 0x01 sobre la línea 0 (inicio de un barrido entero) cobra 256 × 40 = 10 240 ciclos en el
  intérprete y en el recompilador (a r29 en la sección fría; sale del bloque si ya no cabe antes del evento).
  Medido con el modelo: 1–1,5 barridos por campo y 107 (GoldenEye) a 293 (Zelda) líneas sucias en promedio;
  Forsaken hace ~5 por campo. `sin-barrido-cache` (host `SALSA64_SIN_BARRIDO_CACHE=1`) lo deja gratis.
- Host, 1500 campos con y sin el costo: SM64 667, OoT 647, Cruis'n USA 694 y GoldenEye 1266 tareas iguales;
  imágenes idénticas salvo OoT (el caballo un poco más adelante). Escenas del host: 6 idénticas, DK64 y la casa
  de Link con la animación un cuadro corrida (una tarea menos). Forsaken: 1957 tareas en 4200 campos y el título.
  Unitarias: `dcache_sweep_*` (cuatro); 786 pruebas en Docker, 0 fallidas.

## 2026-10-28 — Ritmo del SI según la PIF (#55 Forsaken 64) y latido para juegos que no arrancan (#54)
- Forsaken 64 (USA) se queda en negro tras la intro, dos corridas idénticas en la PS3: 1100 tareas, 490 consultas
  de estado, 592 lecturas y 285 escrituras del Controller Pak (0 bloques cambiados: son los sondeos de 0x8000 del
  Rumble Pak y datos iguales) y después el hilo ocioso; SP/DP emparejadas, SI libre sin evento, PI y audio vivos.
  Lo que espera el juego es un mensaje del SI que ya no va a llegar. Sin el respaldo no se reproduce en el host.
- Con los 2 ms fijos de cada DMA (desde Cruis'n USA, 2026-10-25) cada bloque del pak costaba 4 ms (877 bloques,
  3,5 s). Ahora la escritura dura 65 µs y la lectura lo que la PIF tarda con lo que hay en la PIF RAM (constantes
  de ares: 0,66 ms un control o un bloque del pak, 1,65 ms cuatro controles). `si-ritmo-viejo` vuelve a los 2 ms.
- Cruis'n USA en el host con el ritmo nuevo: 694 tareas en 1500 campos (2 ms fijos: 687; inmediato: 1, el bloqueo).
  Unitarias: `si_dma_timing` y `cuelgue_avisa_siempre`; 782 pruebas, solo `cover` falla en macOS (en Docker pasa).
- Toy Story 2 (USA): entra en 0x80006000, 0 tareas, 4,4 campos/s con ~6 M instrucciones por segundo en el
  intérprete (7 por entrada al código generado): todo pasa por `rec_interp` (modo, segmento, evento o bloque sin
  código); sin el respaldo no se sabe cuál. El latido no avisaba porque el juego nunca dio señales de vida:
  `latido-siempre` lo hace avisar y volcar igual (pc, código, eventos, RDRAM) para ver dónde se queda.
- Donald Duck (PAL, #16) desde el estado del usuario: los hilos no están bloqueados (el de SP/DP despierta en cada
  campo, lee el control y dibuja; los tres que esperan colas propias son los de carga, audio y reset); las 137,8
  tareas de audio por segundo son lo normal del motor (160 muestras por tarea; desde el arranque 128–138/s) y la
  consulta del pak 2 veces por campo también. Con el microcódigo real de audio (`--lle=all`, 500 campos) el
  diálogo tampoco aparece: el audio no es la causa. Pero `--lle-check-audio` da 624 de 624 tareas distintas
  (MK64: 0 de 387): la HLE de ABI1 deja sin escribir estados de RESAMPLE/ADPCM y escribe salidas corridas 2
  bytes con los SETBUFF de este motor (in 0x004, cuentas 0x58/0x38); pendiente aparte. Hace falta un estado de
  antes de hablar con el sobrino.

## 2026-10-29 — Ritmo del RCP (#40, Shadows of the Empire acelerado)
- ares (GDB): SotE en Hoth 80–89 tareas gráficas/s y 40–44 cuadros presentados/s; el host daba 150 y 60 (el juego armaba ~75 cuadros/s). Dos tareas por cuadro; la grande tarda 8,5–18 ms de RSP en ares.
- El RDP de ares termina en el acto: lo medido es el tiempo del RSP real; el relleno del RDP no está (cota inferior).
- Modelo por familia (comandos, triángulos y vértices; `gfx_result.vertices` nuevo, `tex_arena`/`tex_evicted` a 16 bits para no crecer los 112 bytes), ajustado sobre 62 tareas volcadas en ares de SM64, SotE, GE, F-Zero X y OoT: error medio 2–5 %.
- Host con el ritmo: SotE 94 tareas/s y 47 cuadros/s; SM64, OoT, MK64 y F-Zero X igual que antes; GE 53,6 → 50,5 (ares 41,9). Ninguna ventana más lenta que ares. Sin huecos nuevos del AI.
- Encendido por omisión solo en SotE (`NSW`); `rsp-ritmo`, `rsp-ritmo-<código>` y `sin-rsp-ritmo`; porcentaje en el archivo. Host: `SALSA64_RITMO_RCP`, informe `SALSA64_RITMO=N`. Detalle en `docs/ritmo-rcp.md`.
- Docker: 793 pruebas sin fallos; escenas del host idénticas a la base. Falta la PS3 (SotE en Hoth y un control); no se llegó a los AT-AT sin jugar.

## 2026-10-28 — Correcciones de los issues #38, #39, #30 y #52
- #38: BG_1CYC limita marcos mayores que la imagen escalada; contraste con S2DEX real y `s2dex-marco-viejo` para A/B. Falta confirmar C&C en PS3.
- #39: tejido de cuadros completos; en Indy la ventana pasa de alternar filas 1/2 a mantener 1. `vi-campos` restaura la ruta anterior.
- #30: el fondo está en la tarea 253: 1200 bloques YUV cuyos texels se convertían en negro. Se implementan TMEM planar y K0–K3; `sin-yuv` restaura el negro.
- YUV tiene pruebas sintéticas; faltan el contraste visual de V8 y los casos de personajes/misil y 2nd Offense.
- #52/#47: se liberan los 9 MiB de la foto al cerrar, tras parar el escritor; `memoria-estados-vieja` conserva la reserva. Menú/ZIP distinguen falta de memoria; sin `%zu` en el frontend.
- El log de Odyssey sigue completando audio y cierra los SPE correctamente: no prueba un desborde gráfico. Falta el volcado del punto de bloqueo.
- Docker: 789 pruebas, 1878654 verificaciones, cero fallos; nueve escenas del host idénticas. Paquete con holgura mínima SPE de 2816 bytes.
- Falta validación en PS3; no se infieren tiempos del Cell a partir del host.

## 2026-10-27 — B3313: rangos de escritura del audio
- El paginado no limita la escena: tras cargar hay 0–3 fallos/5 s (0–65 ms de disco).
- La tabla de diez rangos del audio se llenaba y el último crecía hasta 6,4 MB; cada tarea invalidaba código intacto.
- En qemu, la misma partida pasó de 207928 compilaciones/202275 repetidas a 5652/2; SM64 quedó en 2483/0 en ambos modos.
- La compilación repetida de B3313 cayó de 5462,2 a 0,057 ms en 16,72 s de consola; ciclos, tareas y comandos no cambiaron.
- Al llenarse la tabla ahora se une el par separado por el hueco menor; `audio-rangos-viejos` restaura el resumen anterior.
- Una caché JIT mayor no ayuda: hubo cero vaciados; el solape del hilo gráfico y los SPE no hicieron esperar al PPE.
- `test-host`: 719 pruebas y 1303044 comprobaciones sin fallos.
- Lockstep desde estados de B3313 y SM64: 50 campos cada uno, sin diferencias.

## 2026-10-26 — #13, ROMs de más de 64 MB por fragmentos
- `src/core/rompg.c` (adaptado de SalsaDS): caché LRU de páginas de 256 KB sobre el archivo; PI DMA, lecturas de palabra del CPU, búsqueda del recompilador y arranque HLE pasan por `salsa64_bus_rom_read` / `rompg_rd32`. Intercambio de bytes de v64/n64 página a página.
- Frontend: ROM > 64 MB (hasta 252 MB) o archivo `rom-paginada` → cabecera de 4 KB en memoria y caché de 32 MB en páginas del sistema (`rom-cache` ajusta los MB); línea «rom:» cada 5 s en el log.
- Host: `salsa64-run --rom-paged PAG_KB,CACHE_KB`. OoT y Mario Kart 64 (intérprete, 300 y 1200 campos) con caché diminuta: imagen y estado idénticos a la ROM entera.
- Pruebas `rompg` (ROM sintética de 80 MB, z64/v64/n64, bordes de página, final, DMA, expulsión, arranque ELF y plano).
- Falta la PS3 real: tirones por fallos de caché y velocidad del disco.

## 2026-10-26 — #34, profundidad de LEGO Racers
- Las cajas 2219/2277 muestran la parrilla y 2650/2842 la carrera; sus 58 tareas crudas conservan coches, pista y HUD.
- En la carrera de ares, HLE y RDP real coinciden en escena; el framebuffer da 23,80 dB por píxel, 29,97 dB en 4×4 y diferencia media <1/255 por canal.
- Cada tarea borra `3c3f00`, dibuja un auxiliar de 32/64 y vuelve a borrar antes del framebuffer 320 × 240 (`1853c0`/`1aac00`).
- En PS3 los auxiliares van a 5× y el framebuffer a 3×: el segundo borrado se ejecutaba aún ligado al auxiliar y solo cubría 160/320 frente a 960 × 720.
- No es el caso de RE2: hay cero color images reutilizadas como z image y cero cargas de z desde RDRAM.
- Para NLG el borrado queda pendiente hasta ligar el destino siguiente y sobrevive si esa profundidad crece; `zclear-inmediato` restaura el camino anterior.
- LEGO Racers figura explícitamente sin chip de guardado; el Controller Pak no cambia.
- `test-host`: 713/713; `test-roms` y build PS3 completos; 8/8 escenas doradas, RE2 y Conker idénticos al árbol anterior.

## 2026-10-26 — Rumble Pak y Transfer Pak experimental
- Accesorio global, por juego y por puerto en pausa; el DS3 usa solo el motor grande y se apaga en todo cierre.
- Transfer Pak propio: secuencias 8D→89→89 y 88→84→80, acceso protegido y MBC1/2/3/5 con SRAM.
- Las ROM GB validan tamaño y cabecera; MBC3 RTC no avanza y MBC5 con rumble se rechaza.
- La sección aditiva TPAK guarda identidad, registros, SRAM y latch; estados sin ella reinician los accesorios.
- Pruebas del protocolo, SI diferido, mappers, archivos y estados; falta validación en PS3 real.

## 2026-10-25 — #37, vida de las texturas en el RSX

- La caché normal solo recicla entradas tras la barrera del RSX; las tres arenas y el staging SPE también esperan su consumidor.
- La TLUT se decodifica a RGBA8888 dentro de cada entrada: no hay una paleta mutable que el RSX lea en paralelo.
- Las listas XBUS sí tenían otro directorio sobre la misma región XDR y podían pisar una tarea SPE aún en vuelo.
- Ahora XBUS recoge primero el SPE y usa solo el arena de su tarea; `texturas-xbus-viejo` restaura el camino anterior.
- `texturas-espera-rsx` espera todas las tareas anteriores antes de escribir texturas y sirve como diagnóstico fuerte.
- El log cuenta listas XBUS, llegadas en vuelo, esperas y tiempo, además de las esperas del diagnóstico.
- Exotica produjo 0 listas XBUS en el host: el riesgo es real y general, pero no explica todavía su cuadro intermitente.

## 2026-10-25 — Monaco: atraso gráfico acotado sin quitar el solape

- El volcado mostró al hilo de audio muerto por TLBL: una lista de 101,94 ms atrasó la IRQ del SP 8,63 M ciclos y agotó sus 11 nodos.
- La ruta híbrida conserva el sondeo durante un campo y después espera al SPE; `gfx-sondeo` restaura el sondeo ilimitado y `gfx-espera-siempre`, la espera estricta.
- Con 9 M ciclos modelados, Monaco llegó a carrera en el campo 6500 con el atraso limitado a 1,56 M, 14899 tareas de audio y sin errores.
- A 1,50 M, GoldenEye llegó a la presa, y Road Rash y Cruis'n Exotica completaron sus recorridos sin alcanzar el límite.
- El log de cada ventana da mediana/máximo del atraso de la IRQ del SP, número de esperas al SPE y milisegundos esperados.

## 2026-10-25 — Midway's Greatest Arcade Hits dibuja los arcades

- Las tareas no eran vacías: F3D descartaba 20–964 comandos Sprite2D `0x09` por tarea.
- La HLE carga subimagen/TLUT y emite el TEXRECT de `BASE`, `SCALEFLIP` y `DRAW`.
- Joust: 45/45 tareas idénticas al RSP real; Tapper: 2/2, con cero píxeles distintos.
- El RDP sí escribe los destinos que cubren el VI; no cambió la presentación ni requiere interruptor.
- Host: 692 pruebas y tres gfxtest sin fallos; lockstep de cinco juegos y MGAH, 600 campos.

## 2026-10-25 — Filtros, segunda ronda: salida directa
- La final dibuja en pantalla; los repetidos leen el búfer anterior, sin reserva extra.
- UI/sistema invalidan la caché; `filtros-sin-optimizar` conserva la ruta original.
- Autoprueba: 192 casos, máximo RGB ≤1; verifica cero copias en cuadros completos.
- Host 691/887841/0; RPCS3 720p/1080p 192/192, máximo 0; pendientes tiempos PS3.
- `crt_spe` idéntico, holgura 74688 B; shaders originales idénticos.
- Variante fp16 descartada tras dos fallos de ensamblado H→R; detalle en `docs/perf-log.md`.

## 2026-10-25 — Caché de salida CRT y comparación exacta
- Misma aritmética y viewport; salida X8R8G8B8 reutilizada con generación válida.
- `filtros-sin-optimizar`: ruta anterior; `filtros-autoprueba`: 96 casos propios.
- Comparación de todos los píxeles RGB, tolerancia cero; cualquier fallo rechaza la caché.
- Inventario y estimaciones en `docs/perf-log.md`; exactitud y tiempos pendientes de PS3.
- Host: 691/887841/0; RPCS3: 96/96 a 720p y 1080p, gfxtest 42/42 normal y fase 8.
- Envoltura gráfica compacta: holguras 1728/1568 B; CRT idéntico, 74688 B.

## 2026-10-25 — #37, Cruis'n USA supera la pantalla de copyright

- El juego esperaba el evento SI en `80111B00`; el SI inmediato dejaba la
  cola `80146740` sin mensaje. ares conserva `STATUS=0x141/0x411` hasta terminar.
- El DMA del SI ahora dura unos 2 ms, expone BUSY y copia/ejecuta el PIF antes
  de levantar la interrupción. `sin-si-diferido` restaura el camino anterior.
- En el host llega a Golden Gate Park: 1672 tareas gráficas en 3900 campos;
  el A/B inmediato repite el bloqueo con una sola tarea. Falta probar la PS3.

## 2026-10-25 — #37, HLE gráfica de Cruis'n Exotica

- El código del microcódigo da CRC `C6121CAA` y se anuncia como F3DEX 2.08.
- Su `D5` ejecuta una sublista F3DEX2 contada; tratarlo como NOP perdía casi toda la geometría.
- En 1500 campos: 702 tareas, 324 idénticas y 378 menores contra LLE; 0 distintas, grandes, desconocidos, errores o cortes.
- La comparación de carrera con ares da 13,73 dB en bloques 4×4; vehículos negros pendientes, probable estado Z/RDP.
- Una carrera carga 1050 veces la TMEM por tarea con 88 % de CPU ociosa: el SPE/TMEM es el cuello probable; falta medir PS3.
- `sin-eurocom` restaura el despacho anterior; F3DEX2 conserva 1024 entradas de caché y 1632 bytes de holgura.
- Host: 691/887834 sin fallos; lockstep de 600 campos sin diferencias en SM64, OoT, MK64, GoldenEye y Cruis'n World.

## 2026-10-23 — La escala elegida en la pausa queda por juego

- Al cambiar «Escala interna» en «Imagen» se escribe de inmediato su modo
  fijo o `auto-N`; ya no reaparece un valor fijo anterior al abrir la ROM.
- La bajada automática conserva a la vez `escala = auto-N` y
  `escala-estable = M`, y M se restaura como escala y techo aprendido.
- El arranque registra si la elección viene del juego, de la global o de la
  omisión, y si su modo es fijo o automático. Hay prueba de ida y vuelta
  5× fija → automática hasta 5× → estable 4× → reapertura a techo 4×.

## 2026-10-23 — La escala automática recuerda el límite estable

- Pokémon Snap confirmó en la PS3 que 5× ocupa el 107,8 % del período y
  corta, mientras 4× presenta 300/300 y el RSX tarda 5,2–7,4 ms. El coste no
  crece de forma lineal con los píxeles, por lo que se quitó la subida basada
  en un porcentaje de margen.
- Al fallar una escala, el techo de esa apertura queda en la inmediatamente
  inferior y no vuelve a probarla. Esa escala se guarda en el ajuste del
  juego y es el punto de partida la próxima vez.
- El log registra el arranque, cada bajada, la escritura del valor estable y
  el techo vigente. Las pruebas reproducen Snap, el juego lento por CPU y el
  tirón aislado. Host: 689/887798 sin fallos; build completo y
  `test-phase2.sh` correctos.

## 2026-10-23 — Escala interna automática según la carga del RSX

- «Automática (hasta N×)» baja un paso tras 3 s con saltos, VI ≥ 92 % y
  trabajo del RSX ≥ un período; nunca actúa si la emulación es el cuello.
- La primera versión subía tras 10 s sin saltos y RSX < 60 %; la medición de
  Snap mostró que ese criterio no predice el coste del paso siguiente.
- La escala fija solo avisa; `sin-escala-auto` conserva el camino anterior.
- El cambio pausa en el límite de campo y reutiliza la recreación en vivo de
  destinos. Se añadieron series sintéticas de carga, margen y oscilación.
## 2026-10-23 — #23, cueva de Conker

- El estado reproduce 20 de 46 tareas cortadas con el búfer de 512 KiB del host.
- Con 1280 KiB no se corta ninguna tarea y aparecen la cueva, el suelo y Conker;
  el RSP LLE confirma que no era niebla, Z ni un efecto de framebuffer.
- La aplicación ya usa 1280 KiB desde `f333282` (v0.2.8); `a254865` iguala el host.

## 2026-10-23 — #3, canción del Great Mighty Poo

- La traza registró 1247 lecturas de `AI_LEN` y 2272 de `AI_STATUS`.
- No hubo escrituras de `AI_DRAM_ADDR` con el FIFO lleno: se descarta esa
  diferencia con ares como causa de este caso.
- La partida llega a Chapters, pero solo ofrece Hungover; falta un estado de
  Sweet Melody para comparar el jefe con ares sin especular.

## 2026-10-23 — OoT: el marcador diminuto pertenece a la lista del juego

- Los dos volcados de la PS3 aíslan el mismo `TEXRECT` RGBA32 de 8 × 8 en
  (1,0)–(9,8), desde la sublista del HUD en 0x17B410.
- El RSP real emite al RDP las mismas palabras `E4024020 00004000`; HLE y
  LLE producen 29 rectángulos. Es contenido de overscan, no un fallo HLE.
- El volcado disponible de ares era del título, no de esta escena. Sin cambio.

## 2026-10-23 — #29, cuadriláteros de Shadows of the Empire

- El microcódigo se anunciaba como Fast3D 2.0D, pero `B5` es un cuadrilátero
  con índices por cinco: la HLE genérica descartaba 13080 comandos en 120 de
  240 tareas y dejaba 1836960 píxeles reales distintos frente al RSP LLE.
- El CRC del código `E7FA4491` selecciona la variante F3DWR ya existente; no
  depende del CRC de la ROM. Una prueba sintética cubre ambos CRC de la familia.
- Tras el cambio: 240/240 tareas sin diferencia real ni comandos desconocidos;
  la partida llega al campo 5600 con escenario, vehículo y HUD. Falta la PS3.
- Docker: 682 pruebas, 887363 verificaciones, cero fallos; build completo con
  holguras de 1568 a 14368 bytes en las imágenes gráficas del SPE.

## 2026-10-23 — #16, Donald Duck: el estado ya contiene el bloqueo

- El estado europeo `3DF17480` carga en el campo 41374 y reproduce la captura.
  Desde el campo siguiente solo corre el hilo ocioso; los otros siete esperan
  en colas vacías y siguen idénticos 926 campos después.
- Acercarse al sobrino y repetir B/A como en el registro llega por el PIF, pero
  no despierta la lógica. Intérprete/JIT: 86 campos, 3089430 entradas y cero
  diferencias. Con Controller Pak o sin él solo difieren dos bytes de RDRAM.
- ares no puede cargar este formato de estado. Para buscar la primera divergencia
  hace falta un estado o Controller Pak de antes de llegar al sobrino.

## 2026-10-22 — HLE de F3DEX2ACCLAIM

- Encendida por omisión (`sin-acclaim` la apaga; host `SALSA64_GFX_ACCLAIM=0`).
  A/B en la PS3: los disparos iluminan las paredes solo con la HLE propia;
  misma velocidad (Turok 2 59,9; Turok 3 56,3 frente a 56,6 campos/s).
- Antepone el CRC de palabras `E44DF568`
  al texto F3DEX2; apagado conserva el despacho y la salida anteriores.
- La HLE traduce `MOVEMEM` de 24/16 bytes, luces compactas con atenuación y
  el bit geométrico 0x80; las pruebas usan solo listas y datos sintéticos.
- En el SPE 0 va en la imagen 5 con F3DEX2; conserva su caché de 512 entradas
  y deja 4880 bytes de holgura. Las demás imágenes quedan entre 1568 y 14368.
- Host: 682/887361 sin fallos; build completo y `test-phase2.sh` correctos.

## 2026-10-22 — Arranque del CIC 6106: residuo del IPL3 en la RDRAM baja

Causa del cuelgue de Yoshi's Story en el bloque de texto: el arranque HLE no
reproducía lo que el IPL3 del 6106 deja cifrado en RDRAM 0x000–0x2BB, y el
juego instalaba callbacks vacíos y nunca liberaba objetos (reserva de 100
llena; ares, 37). `boot_x106_copy` (`src/core/boot.c`) ejecuta una vez la
primera etapa del IPL3 del propio cartucho (~6000 instrucciones) y restaura el
resto de la máquina. En el host: RDRAM igual a ares byte a byte, 73 altas y 47
bajas (máximo 27 ocupados), lockstep de 600 campos sin diferencias. Falta la PS3.

## 2026-10-22 — Limpieza de candidata

- El diagnóstico del recompilador queda apagado por omisión y reserva sus
  tablas solo con `jit-diag` o `SALSA64_REC_DIAG=1`; las cuentas ligeras de
  invalidación siguen activas. Se retiraron las imágenes de medición 1–7;
  `gfx-recorte-vector` conserva su imagen propia. La autoprueba vectorial
  corre una sola vez por arranque. Se integraron los cierres de memoria y
  la corrección de S2DEX; falta completar build y regresiones en este entorno.

## 2026-10-21 — Integración autorizada de RE2 y SPE en la rama principal

- El autor da por realizadas las pruebas de RE2 y SPE y autoriza integrar
  el conjunto. Se conserva el código del paquete probado `7c723d3`,
  `Salsa64 RE2 y SPE` (`SLSA00183`); las revisiones posteriores solo
  actualizan documentación. Se contrastan los árboles de fuentes,
  bibliotecas y pruebas antes de la unión, sin cambios de ejecución.
- Evidencia conservada: host 667 pruebas/887192 verificaciones/cero fallos,
  trece imágenes SPE sobre el mínimo de 1536 y cuatro rutas RPCS3 correctas.
  El log PS3 del conjunto confirma RE2 con carga de estado y cierre limpio;
  Snap tiene además confirmación visual explícita del autor. Naboo queda
  documentado como lentitud preexistente, no como regresión.
- `gfx-afin-cadena` y `gfx-recorte-vector` permanecen optativos y apagados
  por omisión; `gfx-escalar` conserva la referencia. No se incorporan el
  experimento de destinos directos del recompilador ni nuevas HLE de
  Acclaim o Turbo3D. Los pendientes de niebla de F-Zero, corte de comandos
  de OoT y coste de la sala de la estatua siguen abiertos.
- Integración local, sin push, etiqueta ni release. No se reconstruye o
  renombra como RC el paquete experimental ya probado: conserva su código,
  identificador y SHA. Un candidato público versionado requiere preparación
  y aprobación por separado.

## 2026-10-21 — Battle for Naboo: lentitud preexistente, diagnóstico de consola

- El autor aclara que la lentitud no es una regresión. El tramo de Naboo
  en el log de `7c723d3` usa la HLE de Factor 5 Indy (`spu_gfx4`, gbi 13),
  no la imagen F3DEX2 de la optimización de Road Rash. Que las banderas
  afín/recorte estén presentes no implica que esa ruta se use en Naboo.
- Los primeros 600 campos rondan 60 campos/s. Tras la transición, los
  resúmenes periódicos marcan 31,4–33,2 campos/s (52–55 % NTSC) y 29,4–31,3
  ms de emulación por campo, con prácticamente cero espera de ritmo y cero
  tiempo saltado por bucles de espera. La ventana final de 100 campos que
  incluye una pausa da 24,5; no se toma como velocidad sostenida.
- El coste gráfico crece: campos 700–799, 128 triángulos y SPE 3,93 ms por
  tarea; 1300–1399, 1385 triángulos y SPE 14,63 ms. Como el ritmo ya ronda
  32 campos/s en el tramo de poca geometría, no se explica solo por la HLE
  gráfica: el hilo de emulación es un frente relevante que falta perfilar.
  No se restan tiempos de hilos o tareas solapadas para inventar una cuota
  exacta del CPU. En los últimos resúmenes el RSX registra solo 9–15 % de
  ocupación como cota superior; no apunta a saturación de rasterizado.
- En carga alta, la traducción al RSX cuesta unos 6–7,7 ms por tarea y el
  estado gráfico 3,5–5,1 ms en el SPE. Los vértices de Factor 5 no están
  cubiertos por el contador genérico que imprime 0,00 ms; eso no demuestra
  coste nulo. No hay detalle de subfases de la imagen 4 en este log.
- El recompilador acumula 500,869 ms de compilación y cero recompilaciones
  repetidas: no aparece la tormenta de recompilación de Road Rash. El
  contador «traducciones» es `vt_probes`, consultas de direcciones virtuales,
  no número de compilaciones. No se identifica todavía una función CPU
  culpable ni se implementa una optimización durante esta revisión.
- Final: 1625 campos emulados, 1469 tareas gráficas, cero descartes de la
  cola y cero campos sin ranura; resúmenes sin tareas cortadas ni comandos
  ajenos saltados. Hash y tareas de prueba iguales al PPE, cierre limpio y
  salida normal al XMB. Holgura de imagen 4 de 1616 bytes; 1936 sin tocar
  al cerrar (estimación por ceros). Se registra como pendiente de rendimiento
  aparte, no como regresión nueva ni certificación visual exhaustiva.
- Evidencia `build/ps3/re2-spe/naboo-20261021.log`, SHA-256
  `d784957ff0e2908abe97523926843e0602b493835825cc37a153720ce25927fd`.

## 2026-10-21 — Pokémon Snap: regresión adicional del conjunto en PS3

- El autor confirma explícitamente que ya no observó el desorden de dibujo
  reportado anteriormente, además de no ver otros defectos gráficos aparentes
  en lo jugado. Es evidencia visual positiva de esa incidencia en esta prueba.
  Log de `7c723d3`, `Salsa64 RE2 y SPE` (`SLSA00183`), tramo de Snap
  separado de las otras aperturas de la misma sesión: 11513 campos emulados
  y 8073 tareas gráficas. Cierre del juego limpio, cero descartes por cola,
  cero campos sin ranura; resúmenes con cero tareas cortadas y cero comandos
  de otro microcódigo saltados. El log termina después en el menú, no XMB.
- Afín experimental y recorte activos, imagen `spu_gfx1fase8`. Hash/tareas
  iguales al PPE; vectorial/escalar bit a bit, máscaras de 64 lotes y A/B
  de detalle sin diferencias. La ruta vectorial se usa durante el juego,
  y se reciben bloques de detalle sin avisos de datos inválidos. Holgura
  1584 bytes y 2976 sin tocar al cerrar (estimación por ceros).
- El tramo tiene 40 resúmenes periódicos: mediana 59,8 campos/s, no FPS de
  imágenes únicas. Incluye pausas y cambios de presentación: empieza a 1×,
  pasa por CRT Trinitron y termina a 5×. Hay ventanas activas de 55–56,6
  campos/s; las de 0 y 21,8 coinciden con la pausa y su reanudación. No es
  una medida A/B ni prueba de velocidad constante. Detalle en perf-log.
- La mejora visual del orden de dibujo queda confirmada por el autor para
  las escenas probadas. No se identificó aún el recorrido o la evaluación
  de fotos ni se determinó qué cambio produjo la mejora. Se conserva esa
  cobertura concreta, sin atribuirla al arreglo afín ni certificar todas
  las situaciones del juego.
- Evidencia `build/ps3/re2-spe/pokemon-snap-20261021.log`, SHA-256
  `a6c891d39b1b73e0e442caabe2f5a8546eb0efaa264f83e2de425ad21fb5369f`.

## 2026-10-21 — Primera regresión PS3 del conjunto: RE2

- Log confirma `7c723d3`, `Salsa64 RE2 y SPE` (`SLSA00183`), escala afín
  experimental activa y geometría vectorial con `spu_gfx1fase8`. Se carga
  la ranura 0 al campo 1218; RE2 completa 829 campos emulados y 3010
  tareas gráficas antes de volver al menú con estado limpio, cero campos
  descartados por la cola y cero sin ranura. Los resúmenes registran cero
  tareas cortadas y cero comandos de otra familia saltados.
- Hash y tareas iguales al PPE; vectorial/escalar bit a bit, 64 lotes de
  máscaras y A/B de detalle sin diferencias. El bloque de detalle se
  publica en hardware sin avisos de bloque inválido. Holgura estática de
  fase 8, 1584 bytes; 2976 sin tocar al cerrar, estimación por ceros.
- Es una comprobación corta, no una regresión exhaustiva ni una medida de
  ganancia. El autor avisa que terminó; falta su confirmación visual
  explícita del paquete conjunto. El log termina en el menú, sin acreditar
  todavía salida al XMB. Próxima prueba: carrera de Road Rash en el mismo
  paquete, sin cambiar interruptores. Sin integración a main ni publicación.
- Evidencia: `build/ps3/re2-spe/re2-20261021.log`, SHA-256
  `b825d70328aea45e1c6bb89b7f9efbbe3a471bdd457908c0a0b8aff4b264f151`.

## 2026-10-21 — Paquete conjunto RE2/SPE completo y validado en RPCS3

- Build limpio en Docker de `7c723d3`, sin reutilizar el directorio
  incompleto anterior. Todas las imágenes pasan el mínimo de 1536 bytes:
  normales 0…4, 1600/1712/14368/3056/1616; fases 1…8,
  1840/1712/1712/1712/1712/1840/1840/1584. No se reducen cachés,
  se omiten variantes ni se altera el presupuesto. Log
  `build/logs/re2-spe-detalle16-paquete.log`.
- Host con ASan y UBSan: **667 pruebas, 887192 verificaciones, cero fallos**;
  log `build/logs/re2-spe-detalle16-host.log`.
- Paquete `build/salsa64-re2-spe/salsa64-re2-spe.pkg`, título
  `Salsa64 RE2 y SPE`, identificador `SLSA00183`, versión
  `v0.2.9-34-g7c723d3`. Identidad comprobada en ELF y PARAM.SFO.
  SHA-256 `a9de08a9cd0e5f2a9d8210dcae300a233ceb810bed8ba03b83868260fd12d513`.
- RPCS3, `gfxtest` embebido, cuatro corridas secuenciales: referencia sin
  banderas (OK, campo 137), afín con detalle normal (122), afín con recorte
  fase 8 (125) y afín con geometría escalar forzada (122). Todas verifican
  los 42 puntos de pantalla y cierran limpias. Con `spe0-todas`, las cinco
  imágenes dan hash y tareas iguales al PPE en cada corrida.
- El detalle DMA se publica y contabiliza correctamente en las dos rutas
  instrumentadas, sin bloques inválidos. Autoprueba A/B de detalle con 17
  pares y cero diferencias; vectorial/escalar igual bit a bit; fase 8,
  64 lotes/256 máscaras con cero diferencias. Las cuatro capturas PPM son
  idénticas, SHA-256
  `ebe13720078b6f94a725155aff7f6ba66a44739d78ff49f30727729e45ecf97a`.
  Se inspeccionó además la captura de referencia. Logs
  `build/logs/re2-spe-rpcs3-{normal,afin-detalle,afin-recorte,afin-escalar}.log`.
- Persiste la advertencia inicial del homebrew, tarea 1 con resultado
  `0x60` (tramo ajeno terminado sin retorno), 307 comandos, 45 triángulos
  y 28 dibujos. El mismo caso aparece en los logs de los paquetes SPE y
  RE2 anteriores; no se presenta como corregido ni como una regresión nueva.
- El paquete combinado aún requiere comprobar OPTIONS con el mismo estado
  y una carrera de Road Rash en la PS3. Las pruebas RPCS3 no reproducen la
  entrada real del RSP de OPTIONS ni certifican tiempos o sincronización
  del hardware. La alineación DMA local de 16 cumple el contrato de la
  transferencia; queda pendiente su validación en consola. No se sube el
  paquete, integra a main ni publica durante esta preparación.

## 2026-10-21 — Recorte RE2/SPE: margen recuperado con DMA alineado a 16

- El código generado de `run_detail` reserva 464 bytes para el bloque de
  diagnóstico: además del contenido y los registros, realinea su dirección
  local a 128. El contrato de `spukit_dma_put` admite direcciones alineadas
  a 16 y tamaños múltiplos de 16. El destino externo continúa alineado a
  128 y `gfx_io_detail_end` espera el DMA antes de abandonar el marco.
- Se ajusta solo la alineación de la variable local de 128 a 16. No se
  cambia tamaño, versión, contadores, cachés, geometría ni presupuesto de
  pila. El nuevo marco mide 352 bytes: **112 menos**. En fase 8, texto
  103680, datos 800, bss 145808, libres 11616 y cadena 10032: holgura
  **1584**, superior al mínimo de 1536. Log
  `build/logs/re2-spe-pila-detalle16.log`.
- La medida aislada resuelve el límite estático de la variante pendiente;
  todavía faltan paquete limpio completo, host y RPCS3 con detalle activo
  para comprobar la publicación del bloque y las autopruebas de recorte.
  Sin cambios en main o consola ni afirmaciones de velocidad.

## 2026-10-21 — Combinación RE2/SPE: normales aptas, recorte aún no cabe

- La extracción del hash (`3818992`) pasa el build limpio de las cinco
  imágenes normales: holguras 1600, 1600, 14368, 3056 y 1616 bytes. Las
  fases 1 a 7 también cumplen el mínimo de 1536. Host con ASan y UBSan:
  **667 pruebas, 887192 verificaciones, cero fallos**,
  `build/logs/re2-spe-host.log`.
- El paquete se detiene en la fase 8: texto 103688, datos 800, bss 145808,
  11616 bytes libres, cadena 10144, holgura **1472**. Frente a la imagen 1
  normal tiene 64 bytes más de texto y `_end` avanza 128 por alineamiento;
  el marco de `main` sigue en 752. No se reduce el mínimo ni la caché.
  Log `build/logs/re2-spe-paquete.log`.
- Se mide aparte la extracción de la autoprueba `GFX_JOB_CLIP4` a una
  función noinline/cold: texto 103784, mismo `_end`, marco de `main` 752 y
  holgura 1472. No recupera pila y añade 96 bytes de texto; **se retira**.
  Log `build/logs/re2-spe-pila-clip4.log`, objeto y marcos anteriores en
  `build/diagnostico-pila/fase8-antes.*`. Tras dos fallos iguales de fase 8
  se detienen las compilaciones; no se hace un tercer intento.
- No hay paquete completo ni regresión RPCS3 del conjunto. Se conserva la
  extracción del hash que sí reduce la pila, sin quitar el recorte opcional
  ni generar un paquete que lo omita. El siguiente análisis debe centrarse
  en los marcos aún vivos del bucle o en el código de fase 8 con evidencia
  del binario; no presupone que separar otra función vaya a bastar.
  Main y los paquetes de consola anteriormente validados siguen intactos.

## 2026-10-21 — Combinación RE2/SPE: separar la autoprueba recupera pila

- El objeto anterior muestra la autoprueba de hash integrada en `main`,
  cuyo marco de 832 bytes permanece vivo durante el dibujo. Se extrae
  únicamente ese trabajo de arranque a `hash_job`, noinline y cold como los
  trabajos de estado e información. Conserva las mismas comprobaciones,
  DMA, hash y limpieza de TMEM. No cambia geometría, cola ni descriptores.
- Primera medida de la imagen 1: `main` pasa de 832 a 752 bytes y el
  trabajo de hash tiene marco propio de 144. La cadena más honda baja de
  10224 a 10144; texto 103624, datos 800, bss 145808, libres 11744:
  **1600 bytes de holgura**, con el mínimo de 1536 intacto. La separación
  añade 64 bytes de texto respecto de la última medida, pero no cambia
  `_end`; el margen ganado procede de los 80 bytes menos de pila activa.
  Log `build/logs/re2-integracion-pila-hash.log`; objeto y marcos anteriores
  conservados en `build/diagnostico-pila/`.
- Pendiente paquete limpio de todas las imágenes, host y comprobaciones
  RPCS3, incluidas las autopruebas de hash y ambas rutas de recorte.
  No se supone mejora de velocidad ni validación PS3 del binario combinado.

## 2026-10-21 — Combinación RE2/SPE: host correcto, paquete bloqueado por pila

- La unión inicial pasa 667 pruebas y 887192 verificaciones, cero fallos,
  con ASan y UBSan (`build/logs/re2-integracion-host.log`). El primer build
  limpio se detiene en la imagen 1: texto 103584, datos 800, bss 145808,
  11744 bytes libres y cadena de pila de 10224, holgura **1520 bytes**
  frente al mínimo de 1536. Log `build/logs/re2-integracion-paquete.log`.
- En la imagen 1 no existe F3D: el tramo que acepta la carga de 3D solo
  admite `kind == 2`. Se evita repetir esa comparación y se conserva la
  distinción de primera generación cuando ambas familias están presentes
  en el host/PPE. La segunda medida reduce el texto a 103560 bytes, pero
  conserva el alineamiento final, la cadena y la holgura de 1520. Log
  `build/logs/re2-integracion-spu1-compacto.log`.
- Se detienen los builds tras repetir el mismo límite. No se reducen cachés,
  no se relaja el margen de 1536 ni se genera un paquete incompleto. Las
  pruebas host de la simplificación final vuelven a pasar: 667 pruebas,
  887192 verificaciones, cero fallos; recompilación de `s2dex.c` confirmada
  en `build/logs/re2-integracion-host-final.log`.
- La cadena cruda conserva `main` 832, `run_detail` 464, `gfx_run` 176,
  `gfx_dl_f3dex2` 688 y `s2dex_run` 1200 bytes; no se atribuye el déficit a
  una función concreta sin más análisis. El siguiente trabajo debe estudiar
  código o marcos reales antes de reintentar, no repetir el mismo build.
  No hay RPCS3 ni validación PS3 del conjunto. Los paquetes separados
  previamente probados permanecen intactos, sin cambios en main o consola.

## 2026-10-21 — Combinación de OPTIONS de RE2 con la base SPE actual

- Se combina la corrección optativa de escala afín probada en la PS3
  (`4ef227f`) con la base `8ac2708`, que contiene el lote vectorial y el
  recorte experimental. Se conservan ambos interruptores independientes:
  `gfx-afin-cadena` y `gfx-recorte-vector`, apagados por omisión;
  `gfx-escalar` mantiene el retorno a la geometría escalar.
- La unión conserva los bits de trabajo existentes y añade el bit afín
  `0x1000`; no modifica el descriptor de 112 bytes ni el estado persistente.
  Los conflictos se limitaban a entradas de documentación y a dos
  definiciones contiguas de interruptores: se conservan ambas.
- Las pruebas anteriores son evidencia de cada candidato por separado, no
  validación del binario conjunto. Pendientes host, presupuesto de todas
  las imágenes SPE y regresión RPCS3, seguidos de OPTIONS en la PS3.
  No se modifica main ni se envía o publica este paquete durante la unión.

## 2026-10-21 — Regresión PS3 del paquete de integración SPE

Paquete `SLSA00181`, código `74e41b9`, sin el experimento de destinos del
recompilador ni el candidato de OPTIONS. Las revisiones posteriores a ese
código solo modifican documentación.

- Road Rash completa ambas rutas: normal, 6250 campos y 6239 tareas de
  imagen 1; fase 8, 6224 campos y 6213 tareas. Cero tareas cortadas y cierre
  limpio en ambas.
- OoT, fase 8: 7568 tareas, 22091 campos emulados, una tarea cortada por
  presupuesto de comandos y cinco campos descartados por la cola. El autor
  no observó fallos visuales. El antecedente de la ruta normal y los límites
  del diagnóstico se detallan a continuación; no se considera resuelto.
- Majora, fase 8: 9033 tareas, 27024 campos emulados, cero tareas cortadas y
  siete campos descartados por la cola. F-Zero X, fase 8: 18556 tareas,
  18826 campos emulados, cero tareas cortadas y cero campos descartados.
- En las cuatro regresiones las autopruebas registran listas/hash iguales
  al PPE, vectorial/escalar bit a bit y, con fase 8, máscaras y detalle A/B
  sin diferencias. Todos los cierres terminan sin tareas pendientes.
  No sustituye validación visual exhaustiva ni demuestra ganancia de FPS.
- El lote de vértices conserva `gfx-escalar` como retorno a la ruta anterior.
  El recorte de fase 8 permanece optativo mediante `gfx-recorte-vector`,
  todavía instrumentado. La niebla de F-Zero sigue pendiente por separado.
  Logs locales conservados en `build/ps3/spe-integracion/`.

## 2026-10-21 — OoT: corte por presupuesto de comandos, con antecedente en ruta normal

- **Evidencia actual.** En la regresión de `74e41b9`, imagen
  `spu_gfx1fase8`, una tarea de OoT alcanza exactamente 1048576 comandos.
  El resultado es `0x10` (`GFX_RES_LOOP`), con 638339 comandos desconocidos,
  dos triángulos, 52 dibujos y cero errores de dirección de lista. Consume
  188 ms en el SPE. No es un corte por capacidad de vértices, comandos de
  salida o texturas. El autor no observó un fallo visual.
- **Antecedente sin recorte experimental.** La captura anterior
  `gfx-zelda-f3dzex2.log` identifica `spu_gfx1` normal y fase 0 de detalle.
  Ya registra dos tareas con el mismo microcódigo, límite de 1048576
  comandos y 638341 comandos desconocidos. Su resultado `0x11` también
  incluye el presupuesto agotado, aunque no es una ejecución idéntica.
  Por tanto, el patrón no es exclusivo de la variante 8 ni nuevo del
  paquete combinado. Esa referencia ya contiene el lote de vértices:
  no demuestra que el problema existiera antes de toda la rama SIMD.
- **Diagnóstico de lectura.** La vista de los primeros comandos del SPE
  solo se rellena al compilar una imagen con `GFX_HAS(F5INDY)`. La imagen 1
  devuelve deliberadamente el centinela de ausencia; la línea «ninguna
  lectura» no demuestra un fallo de DMA ni que el SPE no ejecutara la lista.
- **Límite.** Solo se conservan contadores y el prefijo que ve el PPE al
  terminar, no toda la entrada del RSP en el instante del corte. No es
  posible repetir exactamente esa tarea en host con este log. No se ha
  identificado la causa del recorrido anómalo ni se atribuye a una carrera
  con el CPU, a un bucle del juego o a la geometría. El lector de listas y
  su presupuesto no cambian en esta integración; esa inspección no sustituye
  una comparación sobre la misma entrada.
- **Evidencias conservadas.**
  `build/ps3/spe-integracion/oot-vector-20261021.log`, SHA-256
  `fedecaac6c0d679b7c4bd39c6a4ab443179db10d8630258e3e20dc9bcd8cc508`;
  `build/ps3/spe-integracion/oot-normal-anterior-20261020.log`, SHA-256
  `f86af1c8b8a266547c73545a835baa35e34fee7dac261480901227edb25fa5a0`.
  El paquete actual completa OoT con cierre limpio; conserva la salvedad de
  una tarea cortada y cinco campos descartados por la cola. No se declara
  corregido el incidente ni se modifica código durante este diagnóstico.

## 2026-10-20 — Preparación de integración del lote SIMD sobre la base actual

- Se reúnen el lote de cuatro vértices, las fases de medición y el recorte
  experimental con la base que ya contiene la corrección VI de Racer y el
  catálogo de compatibilidad. No se incorpora el experimento de destinos
  directos del recompilador ni el diagnóstico Acclaim.
- `gfx-escalar` conserva el retorno a la geometría anterior. El recorte
  SIMD permanece optativo mediante `gfx-recorte-vector`: aún usa la imagen
  de fase 8 y su detalle, no una variante normal sin instrumentación.
- La revisión conserva el ABI de trabajos y resultados de 112 bytes. El
  PPE y todas las imágenes SPE deben reconstruirse juntos. Los resultados
  de consola anteriores se mantienen como evidencia histórica, no como
  validación automática del nuevo binario combinado.
- El reporte visual de niebla de F-Zero X sigue diferido; no se considera
  causado ni resuelto por esta integración. La comparación de una escena
  contra el microcódigo real sigue pendiente.
- Pruebas host de la base combinada: 665 pruebas, 887094 verificaciones,
  cero fallos con ASan y UBSan; log local
  `build/logs/spe-integracion-host.log`. Compilación limpia del frontend
  completada en Docker; log `build/logs/spe-integracion-pkg.log`. Todas las
  imágenes gráficas conservan al menos 1536 bytes de holgura estática; la
  imagen 1 normal y la fase 8 tienen 1648 bytes.
- Paquete de regresión `build/salsa64-spe-integracion/salsa64-spe-integracion.pkg`,
  título `Salsa64 SPE integracion`, identificador `SLSA00181`, versión
  `v0.2.9-16-g74e41b9`. SHA-256
  `b09cde7efabacdac0f6d19d1510d8db58151c2579a2af99347b02cd64560bedf`.
  Identidad contrastada con el ELF y PARAM.SFO.
- RPCS3, ROM gráfica homebrew embebida: `RESULTADO frontend: OK`, campo 125
  en ruta normal sin detalle y campo 137 con `gfx-recorte-vector`. La imagen
  `spu_gfx1fase8` confirma 64 lotes/256 vértices sin diferencias en las
  máscaras, salida vectorial/escalar bit a bit, listas/hash iguales al PPE
  y detalle A/B sin diferencias. Logs
  `build/logs/spe-integracion-rpcs3-{normal,vector}.log`.
  No mide rendimiento ni sustituye regresión comercial en PS3. Falta esta
  última; sin envío, integración en la rama principal ni publicación.

## 2026-10-21 — RE2: regresión básica en consola y cierre limpio

- Tras revisar menús, partida e inventario con el candidato activo, el autor
  informa que todo se ve bien. El log de la misma revisión `4ef227f`
  confirma 4620 campos emulados, 17098 tareas gráficas, cero campos
  descartados por la cola y cero campos sin ranura en RE2. Sus resúmenes
  registran cero tareas cortadas y cero comandos de otro microcódigo saltados.
- RE2 vuelve al menú con estado limpio, sin campos ni tareas pendientes.
  La sesión completa termina después mediante «Salir al XMB» y salida normal.
  Es regresión básica de las escenas revisadas, no validación exhaustiva ni
  comparación contra el microcódigo real. La sala de la estatua conserva su
  incidencia de rendimiento independiente.
- Evidencia: `build/ps3/re2-afin/regresion-20261021.log`, SHA-256
  `1ccfccdad4df9e590d486fb135db7d4613619f4af0c947f639e60d49e82e162e`.
  El candidato queda optativo, activo en la consola e independiente de main;
  falta preparar y validar la combinación con el lote vectorial integrado.

## 2026-10-21 — RE2: OPTIONS legible en la PS3 con el candidato activo

- El autor confirma que OPTIONS ya se ve correctamente. El log identifica
  `4ef227f`, aplicación `Salsa64 RE2 OPTIONS` (`SLSA00182`), y anuncia
  `gfx-afin-cadena` experimental activo. Confirma además la carga de la
  ranura 0: campo 2198 → 1218, el mismo estado guardado en la referencia.
- Las autopruebas de las imágenes usadas dan hash y tareas iguales al PPE.
  Los resúmenes recogidos registran cero tareas cortadas y cero comandos
  de otro microcódigo saltados. El log termina mientras sigue la ejecución:
  no acredita todavía el cierre limpio ni una regresión completa del juego.
- Log activo conservado en `build/ps3/re2-afin/activado-20261021.log`,
  SHA-256 `93ddcc76e87da8204c505f3bbf577ce7dc0cf99266fed7586ac7dde3f95a25e4`.
  Referencia en `build/ps3/re2-afin/referencia-20261021.log`, SHA-256
  `f87c60eef1165e187ba2606bac8bf9062befac7c1fc5f5fc3652117d8cc17ecc`.
  Estado respaldado en `build/ps3/re2-afin/referencia-options.st0`, SHA-256
  `6a709fb7271f682b74d4f0a2234cf4cbbfba694cd47afae0939d15cc57a4e112`.
- Validación visual comunicada por el autor, sin inspección de una captura
  descargada ni comparación nueva contra el microcódigo real. No demuestra
  mejora de velocidad ni resuelve el coste de la sala de la estatua.
  El candidato sigue optativo, en rama aislada y sin integrar a main.

## 2026-10-21 — RE2: el candidato afín recupera el margen del SPE

- Se revisó el estado transitorio de encadenamiento: solo S2DEX puede
  activarlo al cargar F3DEX2 con `gfx-afin-cadena`, y se reinicia en cada
  tarea. El chequeo repetido del interruptor en el RDP era redundante.
  Retirarlo solo ahorró 24 bytes de texto y no cambió el alineamiento final:
  la primera medida de esta revisión conservó 1504 bytes de holgura.
- Se reutiliza `chain_ex`, ya transitorio y fuera del tramo RDP persistente:
  0 referencia, 1 F3DEX 1.x, 2 F3DEX2 afín optativo. Desaparecen el campo y
  reinicio adicionales del candidato anterior. Los intérpretes y el retorno
  distinguen explícitamente la primera generación. No se cambian cachés,
  descriptores DMA, formatos guardados ni aritmética de coordenadas.
- La imagen F3DEX2 reconstruida mide 103632 bytes de texto, 800 de datos y
  145808 de bss; quedan 11744 bytes para una cadena de pila de 10112:
  **1632 bytes de holgura**, por encima del mínimo de 1536. La revisión
  anterior tenía 103720 bytes de texto y 1504 de holgura. Log
  `build/logs/re2-afin-estado-spu1.log`. No se bajó ningún presupuesto.
- Host con ASan y UBSan: **666 pruebas, 870778 verificaciones, cero fallos**,
  log `build/logs/re2-afin-estado-host.log`. Incluye F3DEX 1.x con el
  interruptor apagado y encendido, y repetición de la misma lista F3DEX2
  ON→OFF sin reinicializar el estado; conserva controles de perspectiva,
  sprites, TEXRECT, MODIFYVTX y tarea siguiente.
- Paquete completo compilado en Docker, código `4ef227f`:
  `build/salsa64-re2-afin/salsa64-re2-afin.pkg`, título
  `Salsa64 RE2 OPTIONS`, identificador `SLSA00182`, versión
  `v0.2.9-12-g4ef227f`. Identidad contrastada con el ELF y PARAM.SFO.
  SHA-256 `f338bcfd86caa0a5c5143fd487ab29cb8593e731d7a5809fa7cca18f02f29f29`.
  Las cinco imágenes conservan holguras de 1632, 1632, 14144, 2960 y
  1648 bytes, respectivamente. Log `build/logs/re2-afin-paquete.log`.
  Esta rama parte de la base anterior a la integración del lote vectorial;
  el paquete es aislado, no reemplaza el paquete de regresión SPE.
- RPCS3 con `gfxtest` embebido: `RESULTADO frontend: OK`, campo 138 con
  el interruptor encendido y campo 121 apagado; ambos cierres limpios.
  Con `spe0-todas`, las cinco imágenes pasan sus tareas de prueba y hash
  contra el PPE en ambas corridas. Logs
  `build/logs/re2-afin-rpcs3-{on,off}.log`. Es una prueba de arranque y
  regresión homebrew: no reproduce la transición de OPTIONS sobre la
  entrada real del RSP ni demuestra beneficio de rendimiento.
- La evidencia visual de OPTIONS continúa siendo la copia modificada de
  la salida histórica, no una ejecución del candidato sobre la entrada real
  del RSP. No se considera corregido en PS3 ni se incorpora todavía a la
  rama principal. Antes de promoverlo, comparar OPTIONS con el interruptor
  apagado y encendido, conservando estado reciente, log y captura.

## 2026-10-20 — Resident Evil 2: candidato optativo para OPTIONS

- **Contexto crudo.** La caja histórica 1289 de OPTIONS contiene 44 tareas
  de 11 campos. La tarea 4251 tiene 105 dibujos y 212 triángulos; su salida
  capturada también produce letras revueltas en el rasterizador del host.
  Un dibujo tiene 62 píxeles de ancho, ST de 0 a 124 y una textura CI8
  decodificada de 72 × 18 con transformación S de 1/72, sin el factor 1/2.
- **Reproducción sintética.** La escala afín se decide con la familia de
  inicio de `gfx_job`, no con la que S2DEX carga después. Una lista propia
  que carga F3DEX2 desde S2DEX falla en los dos factores de textura; la
  lista equivalente iniciada directamente como F3DEX2 sí usa 1/2. La
  prueba anterior de la transición solo cubría TEXRECT.
- **Candidato.** `GFX_JOB_AFFINE_CHAIN` conserva la identidad original del
  trabajo y aplica la regla ya existente a los triángulos F3DEX2 cargados
  desde S2DEX. No cambia las coordenadas de `G_MODIFYVTX`, la primera
  generación ni el descriptor DMA. TEXRECT y los sprites S2DEX —triángulos
  crudos— quedan fuera del predicado. Un byte transitorio al final del
  estado se reinicia en cada tarea; no se agrega al formato de guardado.
- **Interruptor apagado por omisión.** En PS3, crear
  `/dev_hdd0/salsa64/gfx-afin-cadena` antes de abrir el juego; sin él conserva
  la ruta anterior. Para volver a la referencia, renombrarlo a `.off` y
  reabrir. El log anuncia referencia o experimental activa. En el host,
  `SALSA64_GFX_AFFINE_CHAIN=1` activa el mismo bit del trabajo.
- **Límite de la evidencia.** Una copia local de la tarea 4251, modificada
  únicamente para reducir a la mitad las transformaciones S/T de sus 105
  dibujos, produce OPTIONS legible. Eso respalda la hipótesis, pero no es
  una ejecución del candidato sobre la entrada del RSP ni una comparación
  contra el microcódigo real. La captura original permanece intacta. No se
  considera arreglado en PS3 hasta probarlo con el mismo estado y revisión.
- **Regresión host.** La prueba controla bandera apagada/encendida,
  perspectiva, `G_MODIFYVTX` ST sin escala adicional, TEXRECT, sprite S2DEX
  dentro del tramo 3D, retorno al triángulo y reinicio en la tarea siguiente.
  Antes del cambio fallaban los dos factores S/T del caso encadenado. La
  revisión final pasa **665 pruebas, 870750 verificaciones, cero fallos**
  con ASan y UBSan; log `build/logs/re2-encadenado-acotado.log`.
- **Paquete no disponible: presupuesto de local store.** El primer build
  limpio se detuvo en la imagen 0: texto 104056, datos 800, bss 145808,
  cadena 9856 y holgura 1504 bytes, por debajo del mínimo de 1536. Se
  excluyeron por compilación el byte y sus operaciones de las imágenes
  que no reúnen F3DEX2 y S2DEX. En la segunda compilación la imagen 0
  recuperó sus 1632 bytes, pero la imagen 1 quedó en texto 103720, datos
  800, bss 145808, cadena 10112 y holgura 1504. Se detuvieron los builds:
  no se redujeron cachés ni el mínimo de pila. Los logs son
  `build/logs/re2-revision-pkg.log` y
  `build/logs/re2-revision-pkg-final.log`; los ELF, mapas y usos de pila
  quedan en `build/salsa64-re2-revision-incompleto/` y
  `build/salsa64-re2-revision/`. No se generó ni se envió un PKG.
- **Siguiente análisis, sin aplicar.** El predicado del RDP vuelve a
  comprobar la bandera que ya condiciona `chain_affine`; estudiar su
  eliminación tras revisar los invariantes puede reducir código. También
  se puede estudiar reutilizar el estado transitorio de encadenamiento
  existente para evitar otro reinicio/campo, con pruebas de generación y
  de retornos. Ninguna hipótesis garantiza recuperar el margen necesario.
- **Imágenes de diagnóstico locales.** Original:
  `build/re2-options/tarea4251.png`, SHA-256
  `3e5b7b9fce659962fcc433ec1bbce6303e9a99ca2473056d0fe462936b50010b`;
  copia con la hipótesis:
  `build/re2-options/tarea4251-hipotesis-afin.png`, SHA-256
  `a4e33a1bf54e7394c3b9a2a1bdd333e9d1625ec970902cc2578da7144c635e2f`.
  No son imágenes de una corrida nueva en consola.
- **Sala de la estatua.** Sin optimización nueva: su coste y OPTIONS son
  problemas diferentes. Falta un estado reciente de la sala con el log de
  la misma revisión; no se presupone que otra HLE de video lo resuelva.
- **Captura mínima para la siguiente comprobación.** Guardar en una ranura
  libre dentro de OPTIONS, conservar el estado y el log y una captura de
  pantalla, y recoger la caja negra del cuadro entero, no solo la tarea
  final de limpieza de profundidad. Con esa entrada se puede ejecutar la
  siguiente tarea con la HLE y con el microcódigo real. La caja 1289 solo
  conserva comandos/vértices/texturas ya producidos por la HLE, no la TMEM
  ni la lista de entrada del RSP.

## 2026-10-20 — Racer: video entrelazado, altura insuficiente del destino

Las tres cajas nuevas capturadas durante el salto del video (campos 558, 656
y 757) y la traza de los primeros 1800 campos descartan cambios de registros,
alternancia RSX/RDRAM o interpolación como explicación: el VI mantiene
`CONTROL=00013052`, 320 × 237, paso 1280 y las dos ventanas verticales
contiguas. El selector sí corta la ventana: necesita 474 filas lógicas y
elige los tres framebuffers con alturas 384, 464 y 384; presenta solo 383,
463 y 383 respectivamente. Sus escalas internas también alternan 3×, 2× y
3×. La tijera de las tareas acaba en 384, pero el video ya está en la RDRAM
cuando nace el destino; usar la tijera como alto máximo pierde la parte que
el VI todavía lee.

El anillo de VI ahora conserva la geometría de cada origen. Al crear una
color image compatible, la HLE exige el alto que cubre la ventana VI reciente
(origen oculto + filas visibles × paso), antes de crear y cebar su superficie.
Para Racer es 476 filas para el origen a dos filas de la base, suficiente
para la ventana presentada de 474. La prueba unitaria reproduce exactamente
esa geometría. Es un cambio de reserva de destino, no de los registros ni de
la selección entre campos. La suite host pasó (660 pruebas, 870 603
verificaciones). En la PS3 real, durante el mismo video, el salto dejó de
ocurrir; no se tomaron mediciones de rendimiento.

Paquete de prueba «Salsa64 Racer video» (SLSA00177):
`build/salsa64-racer-video/salsa64-racer-video.pkg`, SHA-256
`caf4eeaec9c7f9caf623571153e53e2d80504de2a2863bffedc746411c2b99d7`.

Tras integrar los cambios actuales de Road Rash y del detalle del SPE, la
regresión del host pasó con 664 pruebas y 870 680 verificaciones. Paquete
local de regresión «Salsa64 regresión» (SLSA00178):
`build/salsa64-regresion/salsa64-regresion.pkg`, SHA-256
`4717063608f6380bab8babb84be32eaf0ae1113cc446139bd50e2c3a049220eb`.
No se subió a la consola.

## 2026-10-20 — Racer: traza VI completa para capturar el video

La bandera apagada por omisión `trace-vi` ahora acepta `N` o `N INICIO` y
registra esa ventana de campos, no solo imágenes válidas. Cada campo deja una
sola decisión: RSX, RDRAM, sin imagen, copia descartada, saltado o cola llena.
La entrada lleva la instantánea de los catorce registros del VI (incluido
`V_CURRENT` calculado al cerrar el campo), `vpos`, paridad, origen, tamaño,
paso, tareas y atraso. Si el RSX lo presenta, añade sin repetir el selector:
destino, ranura, edad, ventana lógica, región de superficie, recorte,
rectángulo de salida y si es verdadero, intermedio o repetido.

La selección se consulta después de `salsa64_rdp_present`; no vuelve a recorrer
los destinos ni modifica el historial. Sin bandera no se formatea ninguna
línea ni se consulta el selector. El frontend de PS3 compiló desde un directorio
de build limpio. La suite host no llegó a enlazar por una advertencia previa de
`test_menu.c` tratada como error; el test existente de la ventana entrelazada
de Racer sigue siendo la cobertura específica. Paquete de diagnóstico
«Salsa64 Racer VI» (SLSA00176):
`build/salsa64-racer-vi/salsa64-racer-vi.pkg`, SHA-256
`af3b9147cfb1f871cf85c2a6fdad35ad6e730154b364f9e726e7340932478cd9`.
La corrida de la PS3 ya está hecha; evidencia y procedimiento en
[racer-vi.md](racer-vi.md).

## 2026-10-20 — Racer: las cajas disponibles eran de la demo, faltaba el video

Diagnóstico sobre `59cb166`, sin cambios de emulación ni de presentación.
Las únicas cajas de Racer en `build/ps3-limpieza-20261019/` son la 628
(campos 621–628) y la 762 (761–762). Repetidas en el host, las tareas 951 y
1074 muestran la demo de carrera, no el video con el salto. Los diez campos
son RSX y tienen el VI entrelazado conocido (320 × 237, paso 1280,
`V_VIDEO` 0x2501ff / 0x2301fd); no prueban la causa del salto de media
pantalla del video. `trace-vi.off` contiene 600 pero está inerte: solo se
abre `trace-vi`. La traza actual omite campos sin imagen y saltados y no
incluye registros ni ventana del destino. La caja guarda `V_CURRENT` crudo
en cero y no guarda vpos, así que tampoco permite deducir la paridad real.
Evidencia y propuesta inicial en [racer-vi.md](racer-vi.md). No hubo
validación de consola en esa revisión anterior.

## 2026-10-20 — F-Zero X: regresión de recorte y reporte de niebla ausente

- **Diferido por decisión del autor.** El reporte de niebla se retoma más
  adelante; no pedir otra captura ni bloquear los frentes de JIT y Acclaim
  por este diagnóstico. Conservar log e hipótesis, sin darlo por resuelto.

- **Captura.** Paquete de recorte SIMD, imagen `spu_gfx1fase8` confirmada,
  7574 tareas completadas. Autoprueba directa de máscaras 64 lotes/256
  vértices sin diferencias, vectorial/escalar bit a bit, hash/listas iguales
  al PPE y detalle A/B sin diferencias. Resúmenes con cero tareas cortadas
  y comandos ajenos saltados; 3040 bytes de pila sin tocar al cerrar
  (estimación por ceros). Última ventana parcial: 213,3 vértices vectoriales
  de 595,5 totales/tarea; sí ejercitó la ruta modificada.
- **Reporte visual abierto.** El autor no ve la niebla que espera durante
  la carrera. El log cuenta `G_FOG` activo (226,1 vértices/tarea en la
  última ventana), pero no conserva los factores finales, el blender y la
  salida de cada dibujo. Ese contador no demuestra que el efecto aparezca
  en pantalla. No se da por validada visualmente la niebla.
- **Inspección.** La HLE calcula el factor en el alfa; el blender reconoce
  la mezcla del primer ciclo y el consumidor transmite niebla/color al
  programa de vértices del RSX. F3DFLX usa además el color de niebla para
  brillos sin `G_FOG`, documentados el 14 de octubre; no confundirlos con
  niebla por distancia. Cinco volcados históricos `fzero-rc2` tienen dibujos
  con niebla activa y factores no nulos: no describen la escena actual ni
  descartan el reporte. La incidencia histórica de interpolación afín y
  recorte del fondo es una hipótesis de relación, no una causa confirmada.
- **Siguiente evidencia.** Un volcado `SELECT + R2` y su captura en la escena
  donde falta niebla permitirán comprobar factor alfa, estado de dibujo y
  salida RSX/rasterizador. Hace falta la misma escena de referencia para
  atribuirlo a una regresión; las autopruebas no la sustituyen. Sin cambios
  de renderizado ni paquete nuevo durante esta inspección.
- **Log local.** `build/ps3/recorte-regresion-fzero-20261020.log`, SHA-256
  `c2a89e448560c765fd539a67e1d673d92c43af36aeb0cb376746a4e2c00b254b`.

## 2026-10-20 — Recorte SIMD experimental del lote F3DEX2

- **Evidencia y cambio.** La fase 7 localizó en el lote el 81,9 % del coste
  de vértices de su ventana de carrera. En su código máquina las expresiones
  escalares de recorte generaban ramas por plano. La variante 8 calcula las
  seis banderas para cuatro vértices con comparaciones y máscaras SIMD
  explícitas, conservando la aritmética de matriz, perspectiva y niebla.
- **A/B.** `gfx-recorte-vector` selecciona la imagen experimental y mide el
  lote completo. Sin ese archivo, `gfx-fase-7` carga la referencia anterior
  con el mismo intervalo. `gfx-escalar` debe estar apagado. No hay ganancia
  medida todavía; los tiempos individuales de las microfases no predicen
  cuánto se ahorrará, porque sus relojes también cuestan.
- **Cobertura.** El host compara 16384 vértices contra las condiciones de
  la ruta escalar: planos exactos, signos de cero, w negativo y máscaras
  distintas en cada carril. La imagen experimental añade un trabajo de
  autoprueba que calcula directamente 64 lotes/256 vértices en el SPE y
  devuelve sus banderas al PPE. Se conservan listas sintéticas SPE/PPE,
  vectorial/escalar y detalle A/B. Cualquier diferencia impide validar el
  experimento; la comparación de una escena contra el microcódigo real
  sigue siendo una comprobación aparte.
- **Suite host final.** `tools/build.sh test-host`: **665 pruebas, 887093
  verificaciones, 0 fallidas** con ASan y UBSan.
- **Prioridad posterior.** Al cerrar el A/B de Road Rash, evaluar una
  optimización general del JIT con varios juegos, lockstep y medida en PS3.
- **Binario y RPCS3.** La variante 8 emite seis comparaciones vectoriales
  sin las ramas por plano en 0x9b10–0x9b64. Hasta `_end` 250256 bytes,
  cadena de pila 10224, holgura **1648** (mínimo 1536). El homebrew gráfico
  terminó `RESULTADO frontend: OK (campo 137)`; la autoprueba directa del
  SPE devolvió cero diferencias en 64 lotes/256 vértices, las listas y hash
  coincidieron con el PPE y las salidas vectorial/escalar fueron iguales bit
  a bit. Detalle A/B: cero diferencias. Estas pruebas no miden la PS3.
- **Paquete experimental.**
  `build/salsa64-recorte-vector/salsa64-recorte-vector.pkg`, título
  `Salsa64 - recorte SIMD`, SHA-256
  `c5d3fbdc8df7ad62e583fae49b32b79cd628159b60b85b906f798fa0037b5ab7`.
  A/B de carrera completado en PS3 con el mismo paquete: referencia
  `gfx-fase-7` y después `gfx-recorte-vector`, sin `gfx-escalar`.
- **Resultado PS3.** Autopruebas SPE de máscaras 64 lotes/256 vértices con
  cero diferencias, vectorial/escalar bit a bit, listas/hash iguales al PPE
  y detalle A/B con cero diferencias; cierre tras 5975 tareas y 3040 bytes
  de pila sin tocar (estimación por ceros). En ventanas completas de 100
  tareas con más de 2000 vértices vectoriales/tarea (50/51 ventanas), lote
  ponderado **18,22 → 16,02 ticks/vértice vectorial (−12,1 %)** y etapa
  completa **267,56 → 241,30 ns/vértice total (−9,8 %)**. Cobertura
  vectorial 96,02/96,05 %. Ver criterio, ventanas comparables y límites en
  `docs/perf-log.md`; no es una ganancia global de FPS. Logs locales
  `build/ps3/recorte-referencia-20261020.log` y
  `build/ps3/recorte-vector-20261020.log`. Variante aún experimental; falta
  regresión adicional antes de integrarla como ruta normal.
- **Regresión OoT.** La imagen experimental completó 2517 tareas; hash y
  listas SPE/PPE iguales, máscaras y A/B sin diferencias y vectorial/escalar
  bit a bit. Los resúmenes registraron cero tareas cortadas y cero comandos
  de otro microcódigo saltados; 3040 bytes de pila sin tocar al cerrar
  (estimación por ceros). La cobertura del lote nuevo es pequeña en esta
  escena: última ventana parcial 12,0 vértices vectoriales/tarea de 3868,1
  totales (aproximadamente 0,31 %); predominan iluminación y vértices sin
  niebla. Confirma ejecución y coexistencia de rutas, no una validación
  gráfica exhaustiva ni una ganancia en OoT. Log
  `build/ps3/recorte-regresion-oot-20261020.log`, SHA-256
  `32a06a8f996035fd238b7499f34edd7bc19763f0568dec90b571aa794b7f010e`.
- **Regresión Majora.** Imagen experimental confirmada, 4805 tareas,
  autopruebas de máscaras y detalle A/B sin diferencias, vectorial/escalar
  bit a bit y listas/hash iguales al PPE. Los resúmenes registraron cero
  tareas cortadas y comandos de otro microcódigo saltados. Al cerrar quedaron
  1968 bytes de pila sin tocar (estimación por ceros, no sustituye la holgura
  estática de 1648). Cobertura reducida y dependiente de la escena: por
  ejemplo, ventana 4501–4600 con 31,2 vértices vectoriales de 5273,0 totales
  por tarea; al final no hubo lotes aptos. No valida por sí sola el blur ni
  los defectos gráficos conocidos. Log
  `build/ps3/recorte-regresion-majora-20261020.log`, SHA-256
  `7d8429ea604301287e30baeaea3ccae85b35fbb25721093ca6306b5558f4f26b`.

## 2026-10-20 — Medición del lote SIMD completo F3DEX2

- **Pregunta.** La fase 6 dejó 0,969 ms/tarea fuera del postproceso en la
  ventana 6001–6100 de Road Rash. Ese resto mezcla desempaquetado, matriz,
  colas escalares y envoltura de carga; no identifica un coste optimizable.
- **Instrumento.** `gfx-fase-7` selecciona `spu_gfx1fase7`: un único intervalo
  desde el desempaquetado del primer vértice hasta el final del postproceso
  del cuarto. Reutiliza el acumulador y el protocolo de detalle versión 3.
  RDRAM y resto se muestran en la misma ventana; los ticks del lote se
  dividen entre los vértices vectoriales y el resto entre todos.
- **Decisión tras PS3.** En la ventana 7401–7500, el lote consume 1,941 ms
  de los 2,371 ms/tarea de vértices (81,9 %), RDRAM 0,089 y resto 0,341
  (14,4 %). Hubo 8863,8 vértices/tarea, 8561,0 vectoriales y 302,8 de cola.
  Son 18,1 ticks por vértice vectorial dentro del lote y 3,1 por vértice
  total fuera. La última ventana completa 7801–7900 da 18,3 y 3,5,
  respectivamente. El siguiente trabajo gráfico debe concentrarse en el
  código del lote; no hay evidencia de que la envoltura sea el coste
  dominante. No restar esta apertura de la fase 6 para estimar una operación
  concreta ni atribuir una ganancia: aún falta un cambio y su A/B.
- **Validación host.** `tools/build.sh test-host`: **664 pruebas, 870709
  verificaciones, 0 fallidas**. El host no ejecuta la imagen SPE; la nueva
  variante se comprobó también en PS3: vectorial/escalar iguales bit a bit,
  A/B con cero diferencias en 17 pares (sin/con 0,205/0,210 ms), hash y
  tareas iguales al PPE. `spu_gfx1fase7` confirmado, 7907 tareas y 3168 bytes
  de pila sin tocar al cerrar (estimación por ceros).
- **Build Docker.** Imagen 7 hasta 250128 bytes, cadena de pila 10224 y
  holgura **1776** (mínimo 1536). El desensamblado tiene un par de lecturas
  del decrementador en el lote, en 0x9820 y 0x9dac; el mapa del frontend
  confirma la imagen embebida. Paquete
  `build/salsa64-spe-fase7/salsa64-spe-fase7.pkg`, título visible
  `Salsa64 - fase 7`, mismo APPID. SHA-256
  `6649226156f90a1f140f47c75c25732f070de4c1ec736e96d83494aac371012c`.
  Log local `build/ps3/vect-fase7-20261020.log`; no se atribuye ganancia.

## 2026-10-20 — Intervalo conjunto del postproceso SIMD F3DEX2

- **Motivo.** Las cinco aperturas de Road Rash 64 midieron por separado
  desempaquetado, proyección, recorte, niebla y conversión/escritura entre
  3,8 y 5,6 ticks/vértice, pero dejaron 16,4–20,9 ticks fuera de cada una.
  Sus ventanas no son sumables: cada una tuvo su propio reloj y carga. La
  siguiente medida reúne las fases 2–5, incluidas las copias directas de
  x/y/z/w que no cabían en la fase 5; el desempaquetado queda fuera.
- **Instrumento.** `gfx-fase-6` carga otra imagen 1 y rodea con un único par
  de lecturas del decrementador todo el postproceso de un lote vectorial de
  cuatro: registros de posición, banderas de recorte, perspectiva,
  ST/color y niebla. No cambia las salidas ni el bloque DMA de detalle; el
  resto de la línea sigue separando RDRAM y el tiempo que queda fuera de ese
  intervalo.
- **Local store y pruebas.** La imagen termina en 250128 bytes, su cadena de
  pila es de 10224 y deja **1776 bytes** de holgura, frente al mínimo de
  1536. `tools/build.sh test-host`: **664 pruebas, 870709 verificaciones, 0
  fallidas**.
- **Paquete probado en PS3.**
  `build/salsa64-spe-vect/salsa64-spe-vect.pkg`, SHA-256
  `68945be02335b91ea5b0d88199dff4caeaeb326483492369fb5ea391a456e36e`.
  La consola confirmó `spu_gfx1fase6`, hash y tareas iguales al PPE,
  vectorial/escalar bit a bit iguales y A/B con cero diferencias en 17
  pares (medianas sin/con detalle 0,205/0,210 ms). Cerró tras 6142 tareas de
  imagen 1, con 3168 bytes de pila sin tocar (estimación por ceros).
- **Medida.** En tareas 6001–6100, vértices 2,516 ms/tarea: RDRAM 0,094,
  postproceso 1,452 y resto 0,969 (redondeados). Hubo 9316,5 vértices/tarea,
  8932,4 vectoriales y 384,1 de cola; postproceso 13,0 ticks por vértice
  vectorial y resto 8,3 por vértice total. El intervalo excluye desempaquetado
  y matriz; el resto incluye esos trabajos, las colas y la envoltura de
  carga. No puede atribuirse todavía a una operación concreta. La ventana
  parcial final confirma 13,0 y 8,3 ticks, respectivamente. Log local
  `build/ps3/vect-fase6-20261020.log`. Siguiente paso: separar el coste del
  lote completo del de carga y colas; estas cifras no prueban una mejora.

## 2026-10-20 — Cobertura de autoprueba para las variantes gráficas

- **Selector LLE.** En modo `auto`, F3DPD (Perfect Dark) y F3DEXBG (Conker)
  se reconocían y tenían HLE, pero no figuraban como disponibles al decidir
  si arrancar el RSP a bajo nivel. Se añaden al selector y la prueba del modo
  automático confirma que ambas van al backend gráfico; `all` sigue enviando
  todo al RSP a bajo nivel.
- **Autoprueba SPE/PPE.** Las tareas S2DEX sueltas ahora tienen listas propias
  para las codificaciones 1.x y 2.x: color image, tijera, G_BG_COPY,
  G_RDPFULLSYNC y su G_ENDDL. Se comparan al cargar las imágenes 0 y 1, igual
  que las demás familias. La lista F3DZEX2 ejecuta G_BRANCH_W y la de F3DEX2
  recarga F3DFLX y sube su tabla de alfa por G_DMA_IO. Las pruebas unitarias
  comprueban los contadores y las asignaciones de imagen.
- **Pruebas.** `tools/build.sh test-host`: **664 pruebas, 870709
  verificaciones, 0 fallidas**. En PS3, el interruptor temporal `spe0-todas`
  cargó las cinco imágenes y todas devolvieron el mismo hash y las mismas
  tareas sintéticas que el PPE; el resumen fue «ninguna» distinta. La imagen
  F3D tuvo 1632 bytes de holgura de pila y la F3DEX2, 1648. El interruptor se
  desactivó después de la corrida. Paquete local
  `build/salsa64-gfx-autoprueba/salsa64-gfx-autoprueba.pkg`, SHA-256
  `426937ed58123e0b8427bac2aecba000ab46229f2719edf66bbec85e6c0f5d6b`.
- **Integración S2DEX1.** Yoshi's Story ejecutó 2506 tareas en la imagen
  F3D/S2DEX1, con 0 tareas cortadas, 0 comandos de otro microcódigo saltados
  y 55 799 fondos S2DEX. Al cerrar, la imagen conservaba 2000 bytes de pila
  sin tocar; no hubo errores gráficos.
- **Integración F3DFLX.** F-Zero X completó 3857 tareas en la imagen F3DEX2,
  con 0 tareas cortadas y 0 comandos de otro microcódigo saltados; al cerrar
  quedaban 3040 bytes de pila sin tocar. El registro no cuenta cada
  `G_LOAD_UCODE`, por lo que confirma estabilidad de la carrera conocida por
  usar F3DFLX, mientras que la recarga y G_DMA_IO quedan comparados de forma
  directa en la autoprueba SPE/PPE.
- **Integración S2DEX2 y VI.** Resident Evil 2 completó 10 008 tareas en la
  imagen F3DEX2, con 0 tareas cortadas y 0 comandos de otro microcódigo
  saltados; al cerrar quedaban 2608 bytes de pila sin tocar. En la última
  ventana de la corrida, el VI no perdió copias ni destinos de framebuffer;
  las copias a RDRAM siguieron siendo el coste dominante. Esto no prueba ni
  corrige los defectos visuales conocidos del juego, que requieren reproducir
  cada escena afectada.
- **Integración F3DZEX2.** Zelda OoT seleccionó la imagen F3DEX2 para GBI 2
  y completó 3404 tareas, con 0 tareas cortadas y 0 comandos de otro
  microcódigo saltados; al cerrar quedaban 3040 bytes de pila sin tocar. La
  escena recorrida no usó fondos S2DEX2; la prueba sintética conserva la
  comprobación directa de `G_BRANCH_W`.
- **Selector LLE automático en PS3.** Con `lle=auto`, Perfect Dark ejecutó
  3740 tareas gráficas por la HLE de la imagen F3D y el resumen del LLE fue
  0 tareas, 0 instrucciones y 0 fallidas. Así se confirma en la consola que
  F3DPD no cae al intérprete real. No hubo tareas cortadas ni comandos de otro
  microcódigo saltados.
- **Selector LLE automático en PS3, Conker.** Con el mismo interruptor,
  Conker seleccionó la imagen propia para GBI 7 y ejecutó 2477 tareas por
  HLE; el resumen del LLE volvió a ser 0 tareas, 0 instrucciones y 0
  fallidas. No hubo tareas cortadas ni comandos de otro microcódigo saltados;
  al cerrar quedaban 14 448 bytes de pila sin tocar. El interruptor `lle` se
  desactivó al terminar ambas corridas, para volver a permitir estados.

## 2026-10-20 — Medición de una subfase del lote SIMD F3DEX2

- **Código máquina (imagen 1).** `spu-objdump` del `geom.c.o` y de
  `spu_gfx1.bin` confirma cuatro carriles en la multiplicación: cuatro
  cadenas `fm` + 2×`fma` + `fa` (16 operaciones vectoriales para la matriz
  de cuatro vértices). No hay llamada a `memcpy`, a divisiones de libgcc ni a
  conversiones de coma flotante por software en ese camino; las conversiones
  son `csflt`/`cfltu`. Sí hay muchas cargas y permutaciones (`lqx`/`lqd`,
  `rotqby`, `shufb`) al desempaquetar los `s16` big-endian y escribir los
  campos. La división de perspectiva sigue por vértice con
  `frest`/`fi`/`fm`/`fma`; el recorte usa comparaciones y selecciones, y el
  tramo posterior conserva ramas para los casos de `w` y la cola. Las únicas
  llamadas que aparecen en `gfx_vertices` corresponden a rutas no tomadas
  por Road Rash (luces, TEXGEN, FLX y matriz sucia); con detalle, además, las
  lecturas del reloj.
- **Coste estático aproximado.** Por lote, la matriz tiene 16 FP vectoriales:
  ~16 ciclos de emisión y ~24–32 al considerar las dependencias de las cuatro
  cadenas (≈6–8 ciclos/vértice con el pipeline ocupado). El desempaquetado y
  las escrituras usan decenas de cargas/permutaciones (≈15–30 ciclos/vértice
  ideal); la proyección escalar (recíproco/refinamiento y tres coordenadas),
  seis pruebas de recorte, ST/color y niebla quedan en ≈70–130 ciclos/vértice
  antes de esperas y derrames. La ruta escalar repite las 16 FP de matriz
  por vértice (≈24–32 ciclos/vértice por sus dependencias), pero comparte el
  mismo postproceso de ≈70–130. Son cotas de agenda, no una medición de la
  consola. La hipótesis principal para los ~0,25 µs/vértice es precisamente
  ese tramo posterior aún escalar —sobre todo perspectiva, empaquetado y
  escritura—, no la multiplicación de matriz que ya va a cuatro carriles.
- **Medición nueva.** `gfx-spe-detalle` versión 3 conserva el bloque de 272
  bytes y añade la línea `gfx detalle: vtx fases`: RDRAM (envío+espera DMA),
  matriz del lote y `resto`. Para descomponer este último sin acumular marcos
  de reloj, al abrir el juego se puede crear exactamente un archivo vacío
  `gfx-fase-1` a `gfx-fase-5` en el directorio de datos. Cada archivo carga
  una imagen 1 distinta y mide, respectivamente, desempaquetado;
  proyección/división; recorte; niebla; o conversión ST/color y escritura.
  El archivo activa también el detalle. Sin `gfx-fase-*`,
  `gfx-spe-detalle` conserva la medición de matriz; con varios archivos el
  log avisa y elige el número menor. La fase 5 no incluye las copias directas
  de x/y/z/w: añadir otro par de relojes agotaba la pila.
- **PS3 real, cinco aperturas.** Las cinco imágenes cargaron y sus
  autopruebas vectorial/escalar y A/B dieron cero diferencias. En la ventana
  final de cada apertura, RDRAM quedó en 0,8 ticks/vértice y las subfases
  dieron: desempaquetado **4,6**, proyección/división **4,9**, recorte
  **5,0**, niebla **3,8** y conversión/escritura **5,6** ticks/vértice. Los
  restos respectivos fueron 16,4, 20,1, 20,4, 20,9 y 19,2; las aperturas no
  tienen la misma carga, así que no se suman ni se usan como una comparación
  de campos/s. Ninguna subfase aislada explica los ~20 ticks: juntas son el
  siguiente objetivo de inspección, sin atribuir aún una ganancia.
- **Local store.** Build Docker: holguras de las imágenes 0–4: **1632 /
  1648 / 14272 / 2960 / 1648 bytes** (mínimo 1536). Las fases 1–5 dejan
  **1776 / 1648 / 1648 / 1648 / 1648 bytes**, con las mismas opciones de
  tamaño que la imagen 1 normal.
- **Pruebas y paquete.** `tools/build.sh test-host`: **664 pruebas, 870679
  verificaciones, 0 fallidas**. Paquete local
  `build/salsa64-spe-vect/salsa64-spe-vect.pkg`, SHA-256
  `dbe1c5c2b6bff8a0de45f2761041ceab29e304ce9d4959595d4b7e49fdbcb8ac`.
  Instalado para prueba en la PS3 real; no publicado.


## 2026-10-20 — Lote SIMD de vértices F3DEX2 con niebla

- **Hipótesis confirmada.** El objeto SPU de `gfx_vertices` recorría un
  vértice por iteración: transformación escalar, seis comparaciones de
  recorte, división para proyectar y ramas de luz/TEXGEN/niebla. Road Rash
  entra en el caso de color directo con niebla; las esperas DMA medidas antes
  siguen por debajo de 0,1 ms y no son la causa del coste.
- **Cambio.** Solo en la imagen 1, cuatro posiciones se transforman en los
  carriles del SPU cuando no hay luces ni TEXGEN y la niebla está activa. La
  proyección, ST, conversión y niebla mantienen el orden escalar. Las rutas
  restantes no cambian. El archivo `gfx-escalar` al abrir el juego selecciona
  el recorrido previo y el log dice cuál quedó activo. La autoprueba F3DEX2
  añade niebla y hace 17 pares vectorial/escalar, comprobando las salidas bit
  a bit y registrando la mediana del decrementador.
- **Triángulos.** Se intentó agrupar cuatro para el descarte por planos, pero
  la cadena F3DEX2/S2DEX quedó 272 bytes bajo el mínimo de pila; se retiró.
  No se atribuye una mejora de triángulos a esta entrega.
- **Local store.** Build Docker: holguras de las imágenes 0–4: **1632 /
  1776 / 14272 / 2960 / 1648 bytes**, mínimo 1536. El detalle de cobertura
  añadió 16 bytes al protocolo y el marco de detalle de la imagen 1 subió 16
  bytes; sigue 240 bytes sobre el mínimo. Para liberar espacio, en
  la imagen 1 TMEM y las rutas frías de rectángulos/RDP se compilan a tamaño;
  el trayecto de triángulos y vértices mantiene `-O2`.
- **Pruebas hechas.** `tools/build.sh test-host`: **664 pruebas, 870679
  verificaciones, 0 fallidas**. El censo LLE del host, limitado a 700 campos,
  ejecutó SM64 (270 tareas), OoT (540), MK64 (225) y Road Rash (696); Road
  Rash dio 696/696 idénticas. El host no ejecuta el binario SPU, por lo que no
  valida aún la paridad interna vectorial/escalar ni mide la PS3.
- **Cobertura pendiente de la PS3.** El detalle versión 2 (272 bytes) imprime
  por tarea los vértices y lotes vectoriales y los escalares por forzado,
  iluminación (y su subconjunto TEXGEN), falta de niebla y cola de 1–3. En la
  carrera ya registrada (ventana 9501–9600) había 9348,9 vértices/tarea en
  434,6 cargas, todos con niebla y ninguno iluminado; el host de 2000 campos
  reproduce la carrera con F3DEX2. Las colas pueden sumar como máximo 1303,8
  vértices/tarea (13,9 %); por tanto ni luz/TEXGEN ni el múltiplo de cuatro
  explican por sí solos una cobertura casi nula. La siguiente PS3 debe
  distinguir forzado escalar de una discrepancia de la ruta medida. El host
  no ejecuta la imagen SPE ni puede ver el bit `GFX_JOB_SCALAR` efectivo.
- **Paquete local.** `build/salsa64-spe-vect/salsa64-spe-vect.pkg`, APPID
  `SLSA00064`, SHA-256
  `ac402d68409da4efbf5c33ac602d3fe7d53979e38a2cece708c80107eedc2c78`.
  No enviado ni publicado.
- **Pendiente en PS3.** Con `gfx-spe-detalle`, comprobar primero la línea de
  autoprueba sin diferencias y su mediana; repetir la misma carrera de Road
  Rash con y sin `gfx-escalar`, y guardar la nueva línea `lote vtx`. Conservar
  ambos logs y comparar ms/tarea, vértices, triángulos, campos/s, sondeos y
  audio. Repetir
  `--lle-check-gfx` completo y un censo contra la referencia antes de asumir
  una ganancia.

## 2026-10-20 — Road Rash 64: colisión de descartes de espera, no escrituras de código

Rama `roadrash`, sobre v0.2.9. Diagnóstico, reproducción y archivo A/B en
[docs/roadrash-invalidaciones.md](roadrash-invalidaciones.md).

- **Causa medida.** Los saltos `8003ae0c` y `8003b824` caen en la ranura 49
  del detector de espera. Descartar uno sustituía al otro; `rec_idle_exit`
  invalidaba los bloques solapados para quitar la consulta, y al volver a
  traducirlos la reintroducía. Los cuatro bloques `8003ad84`, `8003adf0`,
  `8003b788` y `8003b7d8`: **10 420 traducciones por bloque**, mismo MIPS.
  El contador histórico incluía también los desalojos; el aviso de
  invalidación masiva clasificaba esos descartes internos como «DMA u otros».
- **Host/qemu, recorrido de la consola hasta el campo 2100** (NTSC, 8 MB,
  Pak nuevo, registros fijos, caché de 24 MB; sin rasterizar): **42 375
  invalidaciones por descartes**, todas con huella MIPS idéntica, **0 por
  CPU, 0 por PI, 0 por otros escritores**. Hubo 3622 DMA del PI. No hace
  falta tratar overlays ni cambiar la granularidad de escrituras: ambas
  rutas ya filtraban por solapamiento exacto de instrucciones.
- **Cambio.** Se conserva el camino nativo normal desde la primera emisión
  y se quita solo el salto PPC a la consulta cuando el bucle se descarta
  (`NOP` + sincronización de cachés). Se mantienen el bloque y sus enlaces.
  Las escrituras reales invalidan como antes; las huellas solo miden.
  `jit-invalidacion-vieja` en `/dev_hdd0/salsa64/` selecciona recompilar al
  volver a abrir el juego. El log nombra el modo, separa las causas, cuenta
  las repeticiones y mide el emisor; detalles por PC al cerrar. +1,563 MiB
  fijos (tablas y sitios de parche; `docs/memory-budget.md`).
- **Mismo binario qemu, ambos modos consecutivos:** 61 422 → **8819**
  compilaciones (−85,6 %), 52 597 → **0** repeticiones del mismo contenido,
  3589,710 → **357,720 ms** en el emisor; descartes que invalidan
  42 375 → **0**, retirados por desalojo 14 554 → **0**. Tablas sin
  saturación. El estado entero del campo 2100 es idéntico byte a byte entre
  ambos modos y el lockstep. Son tiempos del host/qemu, **no de la PS3**;
  las corridas compartieron host con otras verificaciones, por lo que no
  se usa su tiempo total como estimación de FPS.
- **Paridad y pruebas.** Road Rash, 2100 campos / 3 290 786 100 ciclos,
  44 164 470 salidas comparadas: `--lockstep sin diferencias`. Super Mario
  64, GoldenEye, Mario Kart 64 y Zelda OoT: 600 campos cada uno, todos sin
  diferencias. `tools/build.sh test-host`: **663 pruebas, 0 fallidas**.
  Las 16 pruebas de espera pasan en qemu con intérprete y recompilador
  estricto; incluyen colisión, likely, J y escritura real posterior.
  La suite completa adicional de PPC pasó las 662 pruebas de intérprete,
  pero en la segunda pasada mostró `f5i_particles` (partículas de Indiana
  Jones; CPU ya terminado) y no completó el límite de 600 s. La prueba
  aislada pasa con y sin recompilador (sin ejecutar bloques). Queda por
  aislar el estado previo de esa suite; no se presenta como una suite PPC
  completa verde ni se cambia aquí la HLE de Factor 5.
- **Paquete local.** `build/salsa64-roadrash/salsa64-roadrash.pkg`, APPID
  `SLSA00064`; build Docker y las cinco holguras de pila del SPE válidos
  (1632, 6688, 14144, 2960 y 1648 bytes, mínimo 1536). SHA-256 al pie de
  esta entrada tras el empaquetado final. No enviado ni publicado.
- **PS3 pendiente.** Repetir la carrera con y sin el archivo: registrar
  porcentaje de NTSC, ms de emulación, peor campo, compilaciones/ms del
  emisor, descartes y desalojos; comprobar gráficos y audio al entrar y
  salir de carrera. El costo del código nativo y de gráficos sigue ahí:
  no se promete velocidad completa sin medir en la consola.

## 2026-10-20 — Detalle opcional de la HLE gráfica del SPE

Rama `gfx-spe-detalle`, base `10b19cc`. Instrumentación para separar los
3,80 ms/tarea residuales (43,23 %) de Road Rash, sin optimización del
camino gráfico. El archivo `/dev_hdd0/salsa64/gfx-spe-detalle`, leído al
abrir el juego, activa solo la imagen 1; renombrarlo a `.off` y reabrir
revierte el diagnóstico. Uso, campos y límites en `docs/gfx-spe-detalle.md`.

- `gfx_job` aprovecha sus 16 bytes libres para dirección/capacidad/versión
  de un bloque DMA de 256 bytes. Descriptor y `gfx_result` quedan en 112
  bytes, con comprobaciones al compilar; la cola MIT no cambia. El SPE
  espera el DMA del detalle antes de devolver el resultado. El PPE valida
  tarea/versión/tamaño y resume cada 100 tareas instrumentadas y al cerrar.
- Cuenta DMA de lectura por ranura y de salida por tipo, bytes pedidos y
  alineados, envío/espera, vértices, luces/niebla/ST, triángulos de entrada,
  aceptados y recortados/rechazados, estado, listas y salida. Tiempos
  exclusivos de triángulos sin estado, matrices, listas y cierre; resto
  explícito para cerrar contra `t_total`. Las esperas DMA son desglose
  anidado, no sumandos adicionales.
- Sin archivo no se reserva bloque/acumulador XDR ni marco de detalle en
  el SPE; no hay DMA diagnóstico, lecturas extra del reloj ni incrementos
  nuevos. El binario conserva comprobaciones de activación. Las imágenes
  distintas de la 1 eliminan las llamadas y contadores por preprocesador;
  no se reduce la caché de texturas.
- Autoprueba en la consola: 17 pares sin/con detalle de una lista propia,
  alternando orden, RDP reiniciado y caché desactivada, salida exacta y
  mediana/mínimo/máximo del manejador. La autoprueba habitual contrasta
  además el SPE instrumentado con el PPE. Por ejecutar en PS3.
- Docker `test-host`: **661 pruebas, 870624 verificaciones, 0 fallidas**,
  ASan y UBSan; prueba nueva de paridad y de capacidad/versión rechazadas.
  La orden sin opciones tropieza con un aviso previo de GCC en
  `test_menu.c:225` (`format-truncation`); pasó con
  `EXTRA_CFLAGS=-Wno-format-truncation`, sin cambiar el menú.
- Build Docker completo del frontend y paquete: imagen 1, texto 103536,
  datos 800, bss 145808, `_end` 250256 y cadena de pila 10112 bytes;
  holguras imágenes 0–4: **1632 / 1760 / 14144 / 2960 / 1648**, todas sobre
  el mínimo de 1536. La imagen 1 deja solo 224 bytes sobre ese mínimo:
  cualquier ampliación requiere volver a comprobar el presupuesto.
 - Paquete: `build/salsa64-gfx-spe-detalle/salsa64-gfx-spe-detalle.pkg`,
   3319312 bytes, SHA-256
   `7bb387d1307552872b0e1a6f819e93ccbfd5e244a78238cd4bd71b9c37184655`.
  No se ejecutaron ROMs ni `--lle-check-gfx`; quedan la comparación con una
  captura autorizada y la validación de sincronización/coste en PS3.

Techo de ganancia de esta entrega: cero; aporta medidas. En la consola,
exigir primero autopruebas sin diferencias, después repetir el mismo
recorrido y configuración con y sin archivo, conservando aperturas y
ventanas separadas. El detalle puede perturbar tiempos y solape del DMA;
si el coste es alto, muestrear tareas completas antes de decidir un cambio.


## 2026-10-18 — Resident Evil 2: el logo de N64 salía a un cuarto

La escala de `G_TEXTURE` que rc7b3 aplicó al ST de `G_MODIFYVTX` rompía el logo
de la intro: el cuadrilátero (87 × 88) lleva s de 0 a 126 y t de 0 a 108 con
escala 1/2 sobre una textura de 64 × 54 (color y alfa apilados), y el RDP ya
reduce a la mitad las ST de los triángulos sin perspectiva de F3DEX. Con
`--lle-check-gfx` (microcódigo F3DEX.Rej real, host, campos 231–241) el LLE
dibuja el logo entero y la HLE solo su cuarto superior izquierdo ampliado; sin la
escala en `G_MODIFYVTX` esas tareas ya no salen como distintas. El microcódigo
real copia el ST tal cual. Queda por verificar que las letras del menú de
opciones (arreglo original de rc7b3) no dependían de esa escala: si vuelven a
salir revueltas, buscar por el ancho del tile y no en `gfx_modify_vertex`.

## 2026-10-18 (noche) — rc9: regresiones de la rc8 (Banjo-Tooie, DK64, Castlevania, Conker)

Datos de la PS3 con la rc8 (`build/rc8-ps3/`: `salsa64.log`, `salsa64-cajas.log` y cinco cajas negras). Rama `rc8-regresiones`.

- **Castlevania (caja 1380): arreglado en el host.** El LLE (`--lle=all --lle-check-gfx`
  desde un estado en el campo 1350 de la pantalla de datos) dejó el `ol` final en
  `0c184a50` y la HLE en `…54`: Z_SOURCE_SEL (bit 2 del othermode L) en 1. Su microcódigo es
  F3DEX2 2.06 (segunda generación) y el RDP traía ese bit de la tarea anterior; la lista
  solo manda `G_SETOTHERMODE_L` parciales (bits 3 a 31), así que el RSP real, cuya copia del
  othermode arranca en 0 con cada tarea, lo deja en 0, y la HLE (que partía del `ol` del RDP)
  lo conservaba: todos los dibujos con z del primitivo, orden de la lista, lo lejano tapaba lo
  cercano (los textos del menú detrás del libro; con el arreglo la imagen de la HLE coincide con
  la del LLE). Es lo mismo que Earthworm Jim 3D, pero en la segunda generación: ahora la copia
  (`sp_ol`, 0 al cargar el microcódigo en cada tarea) es la base del `G_SETOTHERMODE_L` en las
  dos, y en la segunda un `G_RDPSETOTHERMODE` crudo la sigue dentro de la tarea
  (`src/rsp/gfx/dl.c`, `src/rsp/gfx/rdp.c`; la línea nueva no va en las imágenes 0 y 4 del
  SPE 0: la 4 se quedó en 1520 de holgura de pila, mínimo 1536). Censo (`tools/censo-gfx.sh`,
  12 juegos, antes y después): solo Castlevania cambia (204 → 151 tareas distintas, las
  grandes de 78 a 68; quedan 93 «dibuja de más», sin mirar); DK64, Zelda,
  Kirby, Smash, Paper Mario, Conker, RE2, Perfect Dark, Mario Tennis, Mario Party y Doom 64
  idénticos número por número; `escenas-host.sh --comparar`: 9 imágenes idénticas; Banjo-Tooie
  en el host (31 imágenes del campo 10700 al 11000): idénticas. Prueba nueva
  `gfx_f3dex2_othermode_copy` en `tests/unit/test_gfx.c`.
- **Conker, jefe final (caja 6421): causa encontrada, arreglo por tamaño.** Las dos tareas de
  la caja tienen 1585 dibujos y solo estrellas: una por dibujo (un cuadrito de 1 × 1 con su
  propio color primario, así que el estado cambia en cada una y no se juntan), 496 bytes cada
  uno = 786 240 bytes, justo los 768 KB del búfer de comandos. El log de la PS3 lo dice: «tarea
  mala … banderas del resultado 0x5» (FULLSYNC + CMD_FULL, 46 tareas cortadas de 96): la lista
  se corta en la última estrella y el jefe (que va después) no llega. No es z ni orden ni una
  regresión de la rc7/rc8. Arreglo: `CMD_BYTES` de 768 a 1280 KB (+640 KB de la interpolación),
  sacados del arena de texturas (3,375 → 2,625 MB por juego de búferes; el total por juego no
  cambia, `docs/memory-budget.md`). Cabe 2600 dibujos por tarea. **Por validar en la PS3**
  (la tarea del SPE tardaba 42 ms con las estrellas solas: con el jefe encima irá a pocos
  cuadros por segundo; fundir las estrellas, que solo difieren en un color constante, en un
  solo dibujo con el color en los vértices lo arreglaría de raíz).
- **DK64, transición circular (cajas 1668, 1790, 1859): causa sin encontrar.** Las tareas son
  150 dibujos de 33 × 16 (una malla de 10 × 15 baldosas que leen el cuadro anterior de la
  RDRAM, fuente `de 44c3d0/320`, a 304 con `tex_s = 0x8000`, perspectiva activa) y, armando
  las texturas del volcado en una imagen, la captura es coherente («GAME 1» entero): los
  datos de la HLE están bien. En el host el mismo efecto (pad de la sesión de la PS3, estado
  en el campo 1612, LLE de 1612 a 1700) da 40 de 44 tareas idénticas y 4 «menores»: la HLE
  dibuja un 6 % menos de píxeles que el LLE en la primera tarea girada (48 095 contra 51 028),
  que son las grietas entre baldosas (el RDP, con AA_EN, escribe los píxeles de cobertura
  parcial; el dibujo lleva prueba de alfa y `d->aa` lo excluye, y además las aristas con AA
  están apagadas por omisión en la PS3). Ninguna de las dos cosas explica un cuadriculado
  entero; falta ver el cuadro de la PS3. El contador «texturas desde un destino» queda en 0
  durante el efecto (las baldosas se decodifican de la RDRAM, no del RSX).
- **Banjo-Tooie: sin causa en el código; hace falta una caja negra.** El resultado de la HLE
  en el host es idéntico entre la rc6 y la rc8 (31 imágenes del campo 10700 al 11000), y en el
  log de la rc8 los mecanismos de la rc7b2/b3 que se nombraron no se activan en Tooie: 0
  destinos cebados desde la RDRAM, 0 rectángulos de copia del PPE, 0 z cargadas, 0 borrados
  enteros puestos, 0 líneas de la guarda sin escribir. Lo único distinto es que la emulación
  espera cada tarea 11–17 ms a una copia del RSX a la RDRAM (150 de 150 tareas por ventana,
  16 ms por campo de emulación, «ritmo 0,3 ms»: sin holgura, el ritmo del juego tiembla): el
  log (trace `fb tarea 963`) dice que es una color image de **8 bits** en 0x3ab540 con 304 de
  ancho, propiedad del CPU, que se pide cada tarea y se espera siempre (`big8`). Esa imagen
  nunca aparece en el host (el respaldo usado en el host no llega a esa escena). Para seguir:
  una caja negra de Tooie con la cámara moviéndose (SELECT + L2) y, si se puede, la corrida
  con `sin-guarda-cpu` en `/dev_hdd0/salsa64`.
- **Herramienta.** `SALSA64_GCHK_DUMP_MINOR=1` hace que `--lle-check-gfx` también guarde las
  imágenes (LLE, HLE y diferencia) de las tareas «menores», no solo de las distintas.
- **Pruebas y paquete.** `tools/build.sh test-host`: 658 pruebas, 0 fallidas. Paquete «Salsa64
  rc9» (SLSA00168): `build/salsa64-rc9/salsa64-rc9.pkg`,
  3 299 792 bytes, sha256 `aac81b6b468226b62db605f0e4f07dd62d56d3b04d696f0e9b73d8bf0d9fbf5c`
  (v0.2.7-95-gf86d985 más estos cambios; holguras de pila de las imágenes del SPE 0: 1632,
  6560, 14016, 2832 y 1648 bytes, mínimo 1536).

## 2026-10-18 — rc7b3: Resident Evil 2 lento en la sala de la estatua y las letras del menú de opciones

Resultado de la rc7b2 en la PS3 (`build/rc7b2-ps3/`): «YOU DIED» y la sala de
la estatua salen bien, pero la sala va a 15–17 campos/s (traducción ~50 ms
por campo en el hilo 1, la emulación esperando cada tarea) y el menú de
opciones sale con letras revueltas.

- **Lentitud.** Cada cuadro hacía tres cosas enteras: `ppe_copy` del fondo
  y de la z (512 × 509, con cuentas de punto flotante por píxel), el cebado
  del destino de 596 × 437 desde la RDRAM y la carga de la z entera.
  **Arreglos** (`src/rdp/rdp.c`, `lib/rsxkit`):
  - `ppe_copy` no reescribe un rectángulo igual al de la vez anterior (misma
    textura, misma caja y la RDRAM como quedó: suma de todas las palabras),
    y la copia 1:1 va sin punto flotante por píxel.
  - El cebado por otro ancho, con la misma RDRAM que en el último cebado
    entero (suma de muestras), sube solo las filas dibujadas encima desde
    entonces (Leon), con `rsxkit_target_fill_rows`.
  - `zfeed` carga solo las filas que los dibujos con escritura de z tocaron
    desde la última carga entera (`rsxkit_depth_load` con fila de inicio),
    si la z de la RDRAM no cambió y no hay borrados pendientes.
  - Línea nueva del log: rectángulos de `ppe_copy` iguales y z cargadas en
    parte (contadores `ppe_copies_same`, `z_uploads_partial`).
- **Letras del menú de opciones (caja 1289).** Las letras son cuadriláteros
  armados con `G_MODIFYVTX` (posición y ST): 62 × 18 píxeles con s de 0 a 124
  y t de 0 a 36 sobre una textura de 72 × 18, el doble de texels de lo que
  cabe. Las ST de `G_VTX` llevan la escala de `G_TEXTURE`; las de
  `G_MODIFYVTX` no la llevaban. **Arreglo a probar** (`gfx_modify_vertex`):
  se les aplica la escala actual (fuera de la imagen 0 del SPE, como lo de
  XYSCREEN). Si la escala del menú no es 1/2, no cambia nada y hay que
  buscar en el ancho del tile (una CI8 cargada como 16 bits).
- **Yoshi (7 cajas de la rc7b2).** La guarda contra el CPU no recortó
  ninguna copia en toda la corrida de Yoshi («0 copias tardías»): lo que
  desaparece, la muerte que se congela y el negro tras el Reset no vienen de
  copias del RSX que pisen memoria. Con lo visto (la salida de la HLE trae
  los objetos que sí están, el juego apaga el VI y deja de mandar tareas
  tras el Reset) apunta al núcleo. Prueba barata sugerida en la consola:
  repetir el recorrido con `sin-recompilador` en `/dev_hdd0/salsa64`.
- **Pruebas y paquete.** `tools/build.sh test-host`: 656 pruebas, 0
  fallidas; RPCS3: repetición de la tarea del fondo de 512 × 509 sin error.
  Paquete «Salsa64 rc7b3» (SLSA00165, v0.2.7-90-gc29af9f):
  `build/salsa64-rc7b3/salsa64-rc7b3.pkg`, 3 298 768 bytes, sha256
  `08d0dc8b9dec0f9a38efb144c0ed073fa1c220209cb6d84262bb62849fb2c65e`.

## 2026-10-18 — rc7b2: Resident Evil 2, estela de Leon, menú de opciones y pantalla de muerte

Con las cajas negras de la rc7b (`build/rc7b-ps3/`):

- **La estela de Leon (caja 7448).** En esa sala el fondo se copia con 128
  `BG_COPY` a 0x63E000 **de 512 de ancho y 509 filas**, y Leon se dibuja en
  la misma dirección a **596 de ancho**: los 512 × 509 × 2 bytes son, a 596,
  el fondo de 596 × 437 (reconstruido desde el volcado sale la sala entera).
  Lo mismo pasa con la z en 0x189A00. `get_target` ignoraba la imagen de 509
  filas («color image imposible», más de 480), así que ni el fondo ni la z
  llegaban a ningún lado y bajo Leon quedaba el cuadro anterior. **Arreglo**
  (`ppe_copy` en `src/rdp/rdp.c`): los rectángulos de copia hacia una color
  image de 16 bits de más de 480 filas los escribe el PPE en la RDRAM, como
  el RDP (respetando la guarda contra el CPU y la prueba de alfa del modo
  copia), y los destinos que caen encima se ceban desde la RDRAM; `zfeed`
  carga la z desde ahí.
- **Menú de opciones en negro (caja 21456).** Mismo patrón con alturas
  normales: el fondo se copia a 0x63E000/512 y el menú se muestra en
  0x63E000/320. La superficie de 320 nunca recibía el fondo (y la copia de
  la de 512 iba sin esperar: por eso aparecía al grabar la caja negra, que
  le daba tiempo). **Arreglo**: si la misma memoria se dibujó después con
  otro ancho y un tamaño parecido (la mitad o más del área en las dos
  direcciones; Link de Zelda a 64 de ancho no cuenta), esa copia a la RDRAM
  se espera y el destino del otro ancho se ceba desde la RDRAM.
- **Pantalla negra al morir (caja 4632).** Las 246 tareas del texto traen
  vértices con x, y, z, w = 0: el juego los carga en cero y les pone la
  posición de pantalla con `G_MODIFYVTX` (XYSCREEN). Con w = 0 la HLE dejaba
  la posición de recorte en cero y el RSX los descartaba. **Arreglo**
  (`gfx_modify_vertex`, `src/rsp/gfx/geom.c`): un vértice con w = 0 pasa a
  w = 1 y su posición de pantalla se lleva a recorte.
  Solo en el host y en las imágenes 1 a 4 del SPE: la imagen 0 (F3D) no
  tiene pila de sobra, y RE2 usa F3DEX2.
- **Pruebas.** `tools/build.sh test-host`: 656 pruebas, 0 fallidas;
  `tools/escenas-host.sh --comparar` contra la rama antes de estos cambios:
  8 idénticas (SM64 no estaba en la carpeta de respaldos). Nada de esto se
  ha visto en la PS3.
- **Paquete de prueba** «Salsa64 rc7b2» (SLSA00163, v0.2.7-88-g063a2dc, con la
  guarda de `rc6-memoria` unida): `build/salsa64-rc7b2/salsa64-rc7b2.pkg`,
  3 296 464 bytes, sha256
  `f72a677903ce9b99b313a310840237bdaa6431481e9ce7d221556eb2e0161ee2`.

## 2026-10-18 — rc7b2: Yoshi tras regresar y tras el reinicio; la vibración de Rogue

Con las cajas negras de la rc6 (`build/rc6-ps3/yoshi2/`, `build/rc6-ps3/rogue2/`):

- **Yoshi, pantalla negra tras «Reiniciar (botón Reset)».** No es la
  presentación: desde el campo ~18 980 el juego deja de mandar tareas
  gráficas (la caja 21307 no tiene ninguna en 2300 campos) y él mismo apaga
  la imagen (`VI_H_VIDEO = 0` en todos los campos negros; antes 0x006C02EC),
  con el CPU en sus bucles de espera y el audio andando: el juego se queda
  esperando algo al volver a cargar la misión después del NMI. Hay que
  buscarlo en el núcleo (lo que el NMI deja en el SP/DP/PI/SI, el estado del
  RSP o de las interrupciones), no en `src/rdp`.
- **Yoshi, personajes que desaparecen al regresar.** En la caja 14688 la
  salida de la HLE sí trae a Yoshi y los objetos (el rasterizador del host los
  dibuja), y Yoshi no usa profundidad (0 dibujos con z): no tienen que ver
  `zauto` ni `zfeed`. En esos campos la caché de texturas desaloja de 20 a 29
  texturas por tarea (en el resto, casi 0). Falta una captura de la PS3 en el
  momento (SELECT + R2: volcado y su `.ppm`) para ver qué dibujos no llegan,
  y probar el mismo recorrido con la rc4 para saber si es una regresión.
- **Rogue, la imagen que vibra.** El menú es entrelazado: `VI_WIDTH` 1024
  (dos líneas de 512), y `VI_ORIGIN` alterna 0x790400 y 0x790800 (una línea
  del cuadro) junto con `VI_V_VIDEO` 0x003301ED y 0x003501EF (dos medias
  líneas). En la tele los dos campos se intercalan; si se presenta cada campo
  como una imagen completa desde su origen, sin el corrimiento de
  `V_VIDEO`, la imagen sube y baja una línea en cada campo. Es lo mismo que
  la intro de Racer. **Arreglado (rama `vi-entrelazado`, por validar en la
  PS3):** el VI da ahora la posición vertical del campo (`vpos` = primera
  media línea de `V_VIDEO` + el bit de campo, solo con SERRATE) y
  `rdp_targets_present` presenta la misma ventana del destino en los dos
  campos de un cuadro (renglón 0 del destino en `vpos - 2 y0 / k` de la
  trama, igual en ambos con tolerancia de uno): Racer (destino 320 × 480,
  stride de 2 filas, V_VIDEO 0x2501FF / 0x2301FD, origen una fila arriba en
  el segundo) y Rogue (640 × 335, stride de 1 fila, 0x3301ED / 0x3501EF,
  origen una fila abajo) quedan en `y0 = 1` en todos los campos (antes
  alternaban 1 y 2 o 0 y 2); prueba `targets_present_interlaced_vpos`. Sin
  SERRATE o sin `vpos` (los logos de DK64 de la prueba antigua) no cambia
  nada, y las 8 escenas de `tools/escenas-host.sh` salen idénticas.
- **Rogue, logos sin alfa:** no se llegó a ver.

## 2026-10-18 — rc7b: los personajes de Resident Evil 2 (la z del fondo escrita como color)

Con la caja negra de la PS3 (`build/rc6-ps3/re2b/cajanegra-1928`, la ciudad
destruida con Leon y dos zombis invisibles) se ve el orden de cada cuadro:

- tarea 6137: 35 `BG_COPY` de 512 × 4 en modo copia a la color image
  **0x189A00 de 512 de ancho** (filas 0–138): es la profundidad del fondo
  prerrenderizado (desde 0x6D4000 en la RDRAM), copiada como si fuera color;
- tarea 6138: el fondo en color a 0x76A000/512;
- tarea 6139: los personajes (30 dibujos, 1561 triángulos, prueba y
  escritura de z) en 0x76A000 de **328 de ancho**, con z image 0x189A00.

El RDP direcciona la z con el ancho de la color image, así que los 141 312
bytes que el fondo copió a 512 de ancho son, para los personajes, la z de
328 × 215. Reconstruida desde las texturas del volcado y descomprimida, la z
del fondo va de 0,78 (lo cercano) a 1,0 y los personajes están en 0,945–0,95:
con esa z los 4683 vértices de los personajes que caen en pantalla pasan la
prueba. El RSX nunca la veía: la copia iba a una superficie de color y la
profundidad 0x189A00 solo tenía la z de los personajes de cuadros viejos.

**Arreglo** (`src/rdp/rdp.c`): la copia a la RDRAM de una color image en la
dirección de una z image en uso se espera siempre (`zimg_seen`, en
`readbacks_wanted`), y `zfeed` carga la z desde la RDRAM, con el ancho de la
color image del dibujo, cuando esa dirección se dibujó como color en una
tarea anterior y aún no se cargó (antes solo cuando el contenido de la RDRAM
cambiaba: con el fondo quieto, una sola vez). La copia de RGB565 a la RDRAM
conserva 15 de los 16 bits de cada z (el último es del dz).

Paquete de prueba «Salsa64 rc7b» (SLSA00162, v0.2.7-80-gfa20b9f):
`build/salsa64-rc7b/salsa64-rc7b.pkg`, 3 289 488 bytes, sha256
`6b1016af37f32e99f3aa661c176069ff96a48c27a9366d1994193e3e1169efba` (directorios
de build borrados antes). Falta la PS3: la carga con `rsxkit_depth_load` sigue
sin probarse en el RSX real.

## 2026-10-18 — rc7a: las copias tardías del RSX pisaban lo que el CPU escribió (Indiana Jones y Killer Instinct Gold)

- **Lo que dicen los datos de la PS3 (rc6).** La ROM en memoria está bien (CRC igual al de la Mac en
  los dos juegos). Indiana Jones: «tarea mala» con la lista en 0x71D358 llena de 0x00010001; esa
  dirección cae dentro del destino de 640 × 480 de la pantalla del Expansion Pak
  (`gfx: destino 6b3000/640`, 0x6B3000–0x749000, a 85 filas del principio). Killer Instinct Gold:
  el log trae `cpu: fallo AdEL … pc 0x00010001, ra 0x80039DFC` en el campo 914; en 0x80039DF4 hay
  un `jalr` a un puntero a función leído de un objeto del audio de libultra (`lw t9, 8(s2)`): el
  puntero valía 0x00010001. Todos los volcados de cuelgue de Killer Instinct (rc4, rc6 y los cuatro
  de rsp-jit-ps3) tienen `bad_vaddr 0x00010001`; lo que se ve lleno de 0xFF o 0xFE (desde 0x0E40A0
  hasta el final, o 0x1B4ED0–0x3A3000) aparece después de la caída (el CPU sigue en un bucle de copia en 0x80031408), no es la causa.
- **Causa.** 0x0001 es un píxel negro en RGBA5551, lo que deja `to_rdram` al copiar un cuadro negro
  del RSX a la RDRAM. Esas copias (espejo del framebuffer, copias sin esperar, espejos chicos y
  VISCVG) las hace el hilo de presentación uno o más campos después de la tarea; solo se
  descartaban si un DMA había tocado la memoria después (`dma_newer`), no si la había tocado el
  CPU. Cuando el juego reusa la memoria de un framebuffer (listas de Indiana Jones en la memoria de la
  pantalla del Expansion Pak; objetos del audio de Killer Instinct al cargar la pelea), el último
  cuadro llegaba encima. En el host la copia es síncrona: por eso ninguno fallaba ahí. Es la misma
  firma que Resident Evil 2 (el hilo en curso en 0x00010001), que se había tapado solo para la
  dirección 0.
- **Arreglo: guarda contra el CPU** (`rdp_guard`, targets.h; docs/framebuffer.md). Al terminar cada
  tarea el hilo de emulación toma una huella por línea de 128 bytes de las filas que sus dibujos
  pueden tocar (tijeras) en cada color image de 8 o 16 bits; la copia tardía solo escribe las líneas
  cuyo contenido sigue siendo esa huella o la que dejó la última copia. Las demás se quedan como las
  dejó el CPU. Además `to_rdram` ya no escribe fuera de la RDRAM (no lo comprobaba). Prueba
  `targets_cpu_guard`. 512 KB de tablas (docs/memory-budget.md).
- **Instrumentación.** Log de cada 5 s: «gfx: guarda contra el CPU: N copias tardías con M líneas sin
  escribir …; huellas X ms por tarea»; las 12 primeras copias recortadas dicen origen, rango,
  tarea y lo que había en la primera línea. `sin-guarda-cpu` en la carpeta de datos la apaga (A/B).
  `vigilante.cfg` con «lo hi» en hexadecimal (p. ej. `6b3000 749000` para Indiana Jones) escribe cada
  copia, VISCVG o DMA en ese rango y si dejó 0x00010001.
- **Sin relación probada.** Yoshi's Story se congela al morir (sin cuelgue ni volcado en el log; usa
  muchas copias a la RDRAM y podría ser lo mismo: mirar si sale la línea de la guarda) y los dos
  logos sin alpha de un menú de Rogue Squadron (no son copias a la RDRAM; pendiente aparte).
- **Para probar en la PS3.** Indiana Jones (pantalla del Expansion Pak → misión), Killer Instinct
  Gold (elegir personaje y pelear dos veces), Yoshi's Story (morir), y Mario Kart 64 (túnel) y la
  pausa de Zelda como regresión de las copias. Si alguno vuelve a fallar: la línea de la guarda y,
  con `vigilante.cfg`, quién escribió el 0x00010001.
- **Paquete de prueba** «Salsa64 rc7a» (SLSA00161, v0.2.7-80-gf0ea694):
  `build/salsa64-rc7a/salsa64-rc7a.pkg`, 3 293 136 bytes, sha256
  `6db79e6b109e735277c8cffd48a83e72ec48787f10a55b3544d6ff172ee6598c`, armado con
  `tools/build.sh -C src/frontend PROGRAM=salsa64-rc7a "TITLE=Salsa64 rc7a" APPID=SLSA00161 pkg` (el
  directorio de build borrado antes). Pruebas del host: 656, 0 fallidas; `tools/test-roms.sh` bien; las
  escenas del host no cambian por construcción (salsa64-run solo gana funciones nuevas de targets.c que no llama).

## 2026-10-18 — alfa en los destinos de render a textura (Mario Tennis, logos de Rogue Squadron)

Rama `alfa-destinos` (sobre main ea6118e). Sin validar en la PS3: el host no ejecuta el camino del RSX.

- **Cambio.** `rsxkit_target_create_ex(w, h, RSXKIT_TARGET_ALPHA)` crea destinos A8R8G8B8 (nacen con
  alfa 0). `src/rdp` los usa para las color images de 16 bits que no son del ancho del VI (los
  framebuffers quedan X8R8G8B8: no se leen con alfa y un destino con alfa mezcla en cada dibujo).
  Alfa de la N64: `rsxkit_clear` con el alfa en el byte alto (0 al borrar con 0x0000); cada píxel de un
  dibujo normal queda con alfa 1 (mezcla de alfa ONE/ZERO); un relleno (`G_CYC_FILL`) deja el bit de alfa
  del color de relleno (`gfx_draw.aref`, nuevo uso en los rellenos); una copia sin mezcla (`G_CYC_COPY`)
  deja el alfa del texel; el cebado (`target_fill_rows`) trae el bit de alfa de la RDRAM; la copia a
  scratch conserva el alfa (programa de blit nuevo `rsxkit_blit_argb.fcg`). Las texturas leídas de un
  destino con alfa ya salían como `RSXKIT_XRGB8888`, cuyo alfa lee el 3D (el remap del 3D usa el canal A).
- **Interruptor.** Archivo `/dev_hdd0/salsa64/sin-alfa-destinos` (vacío): vuelve a los destinos de antes.
  Por omisión, encendido. El log dice la decisión al abrir el juego y, por intervalo,
  `gfx: alfa en los destinos: N destinos con alfa creados, N texturas leídas de uno de ellos, N rellenos o
  copias con su alfa` (`targets_alpha`, `tex_alpha`, `alpha_fills`).
- **Qué se razonó con las cajas** (sin RSX): en Mario Tennis, las texturas de partido vienen de destinos de
  64 × 64 RGBA16 (ancho 64 ≠ 320 del VI: llevan alfa) y el borrado es 0x0000; las de ancho de VI no cambian
  (Zelda pausa, túnel de MK64 y Yoshi siguen con el camino de antes si sus imágenes son del ancho del VI).
- **Paquete de prueba** «Salsa64 alfa» (SLSA00175): `build/salsa64-alfa/salsa64-alfa.pkg`, 3 300 688 bytes, sha256
  `3718f62a61f2ba395ec8c8cdd0df7321a5998af93c1002894e92b52bf7926dfe`. Probar Mario Tennis (partido y estela de la
  pelota) y los logos del menú de Rogue Squadron; después Zelda (pausa), MK64 (túnel), RE2 y Yoshi; si algo
  cambia, repetir con `sin-alfa-destinos`. `tools/build.sh test-host`: 658 pruebas, 0 fallidas.
- **Límites conocidos.** Una textura con `GFX_TEX_PREMUL` leída de un destino con alfa no se premultiplica
  (con alfa 0 el RGB suele ser 0, el efecto es mínimo). Las copias del RSX a la RDRAM siguen en RGB565 y
  vuelven con el bit de alfa en 1 (`rsxkit_pixels_565_to_5551`). Si algún juego de los que ya andan cambia,
  comparar con `sin-alfa-destinos`.

## 2026-10-18 — gráficos: Earthworm Jim 3D (orden de dibujo) y Mace (textos) arreglados en la HLE; Mario Tennis y la z de Zelda diagnosticados

Rama `graficos-jim-tennis` (sobre rc6), con las cajas negras de la rc6 en la
PS3 y el LLE (`--lle=all`, `--lle-check-gfx`) como oráculo en el host.

- **Earthworm Jim 3D, «cosas lejanas encima de cercanas».** La caja negra
  (21 tareas) ya traía la causa: todo el escenario (dibujos 1–43) sale con
  `depth 11` = prueba + escritura + **z del primitivo** y con la misma z (0),
  así que en el RSX gana el último dibujado (el orden de dibujo), y el logo y
  los textos (z del primitivo, ZMODE_XLU = prueba estricta) quedan tapados.
  Con `--lle-check-gfx` la misma tarea (la 418, campo 430) tenía el logo y el
  texto en el LLE y no en la HLE (34 774 píxeles distintos). Con el volcado de
  la HLE y `gfxdump.py edit --set N:depth=…` se vio que sin la prueba de z
  todo aparece; con la prueba pero sin LESS también. El mecanismo: Jim manda
  un G_RDPSETOTHERMODE (0xEF) de relleno con `Z_SOURCE_SEL` en 1 (0x0f0a4004)
  y después fija el modo de render con G_SETOTHERMODE_L (3, 29). En F3DEX 1.x
  el microcódigo mezcla en **su propia copia del othermode** (que el 0xEF no
  toca) y entrega la palabra entera al RDP: el bit 2 se apaga. La HLE llevaba
  una sola copia (la del RDP) y conservaba ese bit; todo el escenario salía
  con z del primitivo. (Se vio comparando `JIMDBG`: `ol=0055303c` en la HLE
  contra `00553038` del LLE en cada tarea.) **Arreglo** (`dl.c`
  `gfx_sp_othermode`, `rdp.c`): en la primera generación (F3D, F3DEX 1.x) la
  parte L del othermode que mezcla G_SETOTHERMODE_L parte de una copia propia
  (`sp_ol`, 0 al cargar el microcódigo, `sp_om_shadow`) que G_RDPSETOTHERMODE
  no cambia; en F3DEX2/F3DZEX el 0xEF sí la actualiza y se sigue como antes
  (probado: con la copia sin seguirlo, Zelda pasa de 197 a 419 tareas
  distintas). Medido con el censo (HLE contra LLE, intro y título): Jim,
  tareas grandes distintas de 147 a 0 (331 → 186 distintas, las que quedan
  son de 1–8 niveles de canal); el resto de los juegos medidos
  (Zelda, Mace, Banjo, MK64, GoldenEye, Perfect Dark, SM64, Kirby, DK64)
  igual que con rc6. En el host la tarea 418 sale con el logo, el texto y
  la sombra de Jim. Falta confirmar en la PS3 (la caja negra de la rc6 ya no
  se puede reproducir: se capturó con la HLE vieja).
- **Mace, textos.** Reproducido en el host: el selector de personajes (tarea
  361, campo 770) no dibujaba «EXECUTIONER», «MORDOS KULL» ni el «8»; el LLE
  sí. Los glifos son triángulos de 16 × 16 de un atlas IA8 de 160 de ancho
  **sin corrección de perspectiva** (`G_TP_NONE`); el LLE entrega al RDP s y t
  a la mitad de lo que dice el vértice (el RSP normaliza 1/W a ~0x4000 y el
  RDP no divide). `build_state` ya lo hacía para la segunda generación
  (el sol del telescopio de Majora); Mace con F3DEX 1.21 resultó igual. Con
  F3DEX 1.x en esa condición: Mace de 230 tareas distintas (y 164 «dibuja
  de más») a 0 en las 375 primeras; ningún otro juego cambia (ninguno dibuja
  triángulos texturados sin perspectiva con F3DEX 1.x en las escenas medidas).
- **Mario Tennis.** En el host la HLE y el LLE coinciden (23 distintas de
  2 480 tareas hasta el partido, 0 grandes; los frames de las tres cajas
  salen bien en el rasterizador del host). Lo que falla es del RSX.
  - (a) *Puntos en el logo de N64*: la tarea 20 de la caja 59 dibuja el
    logo en 8 tiras de 128 × 16; la última es un `TEXRECT` de una fila
    (y 168, lry = uly) con la tira de las filas 112–127: su primera fila
    tiene 7 texels sueltos (RDRAM después del final de la imagen). Salen
    igual en el host (HLE y LLE) y en RPCS3 (`--replay` de la tarea); la
    fila mínima de un TEXRECT de 1–2 ciclos con lry = uly es lo que hace el
    RDP (`rdp.c`, `lry - uly < 1`). Es contenido de la ROM; sin consola
    (o ares) no se puede decir si la N64 los muestra: no se tocó.
  - (b) *Texturas sin alfa* y (c) *estela de la pelota*: la HLE no tiene
    nada que arreglar; en la consola el 80 % de las texturas de una tarea
    de partido vienen del RSX (14 000–18 000 por 5 s: render a textura, con
    destinos de 64 × 64 RGBA16 y CI8 en 0xd4a80). `rsxkit_target_texture`
    las lee como X8R8G8B8 (`target.c`): no hay canal alfa, así que lo que el
    juego dibuja con alfa 0 en una imagen RGBA16 (el bit de alfa en 0) y
    después usa como textura con alfa sale opaco. La caja negra no lo
    puede mostrar (copia esas texturas de la RDRAM, ya decodificadas).
    Diseño propuesto (sin hacer: no hay cómo validarlo sin la consola ni
    RPCS3 que compila los programas de forma asíncrona, ver abajo): destinos
    de imágenes de 16 bits en A8R8G8B8, borrados con alfa 0, y alfa del
    destino = 1 en cada píxel dibujado (mezcla del canal alfa con
    `CONSTANT_ALPHA`/`ZERO`, color de mezcla con alfa 1), leídos como ARGB;
    el borrado a 0x0000 de la N64 da alfa 0. Probar en la PS3 con un
    paquete con un interruptor.
  - RPCS3 no sirve para mirar esto: la primera repetición de un volcado en
    una sesión sale con los programas de fragmento sin compilar (la red de
    Mario Tennis salió como una barra negra y con el mismo volcado, dos
    corridas después, bien).
- **Zelda, resplandor de las antorchas a través de las paredes (Pozo de
  Kakariko).** No implementado esta ronda; diseño y medición estimada en
  `docs/framebuffer.md` («La z que lee el CPU: copia desde el RSX»).
- **Pruebas.** `tools/build.sh test-host`: 656 pruebas, 0 fallidas (una nueva:
  `f3d_rdp_othermode_shadow`). `tools/escenas-host.sh` con rc6 y con la rama:
  las 9 escenas idénticas (incluida SM64 y la casa de Link). `tools/test-roms.sh`
  sin fallas. Pila del SPE 0: holgura 1632 (rc6: 1616).
- **Paquete de prueba** «Salsa64 gráficos» (SLSA00164, v0.2.7-81-g40f0d33):
  `build/salsa64-graficos/salsa64-graficos.pkg` (en el worktree de esta rama),
  3 290 000 bytes, sha256
  `14b8e900c95d66bac19f5f6cd4f0fdf797a13ff2c9d6fc7433f8934fe6a682d4`.
  Probar: Earthworm Jim 3D (intro y primer nivel: el escenario con su
  profundidad, el logo, los textos), Mace (selector de personajes, el libro
  de las reglas), y que Zelda, SM64, Banjo y MK64 sigan igual.

## 2026-10-18 — rc5b2: la nave en la misión de Rogue, la z de Resident Evil 2 y los puntos de Yoshi

Resultado de la rc5b en la PS3 real: el hangar de Rogue ya sale bien, pero en
la misión la nave sigue a pedazos y el cielo muestra una cuadrícula; en Yoshi
quedan puntos sueltos y a veces la estela; en Resident Evil 2 los personajes
no se ven y el menú de opciones queda negro. Volcados y log en
`build/rc5b-ps3/`.

- **Rogue, la misión (reproducido en el host).** El juego borra la z solo
  debajo del horizonte (y nada si se ve solo el cielo); arriba queda la z de
  la nave de los cuadros anteriores. Con `salsa64-run` hasta el vuelo en
  Tatooine (START y A cada 120 campos hasta el 17 000) y el stick tirando
  hacia arriba y girando, el rasterizador del host la pierde a pedazos igual
  que el RSX; con la z borrada en cada tarea sale entera, y también con la
  prueba de la N64 (el margen de dos veces la pendiente de z). Ni el CPU ni
  un DMA escriben esa z (vigilada con `SALSA64_WATCH_MEM`): en la N64 la nave
  pasa contra su propia z vieja gracias al margen del dz. **Arreglo**
  (`zauto` en `src/rdp/rdp.c`, igual en `src/host/raster.c`): en una tarea
  que empieza un cuadro (su primera color image no es la de la tarea
  anterior) y que antes de su primer dibujo con profundidad no borró la z
  image entera, se borra entera. Las tareas que siguen un cuadro conservan
  la z. En el host la nave sale entera en esa escena; las 9 escenas del host
  salen idénticas (el borrado se dispara también en «casa», Kirby, Banjo y
  Zelda, sin cambiar la imagen).
- **Rogue, cuadrícula del cielo.** Sale igual en el rasterizador del host
  (volcado 4622): las costuras entre los tramos del cielo vienen de la salida
  de la HLE, no del RSX. Queda por comparar con el RSP real o con ares.
- **Resident Evil 2.** La línea del log da 0 borrados de la z en el juego y
  «color images en una z image» > 0: la z image (0x189A00) es también la
  imagen del fondo de 512 de ancho, y la z de los personajes la deja el CPU
  (o un DMA) en la RDRAM, que el RSX no ve. **Arreglo a probar** (`zfeed`):
  si el RDP no borró la z image en las últimas 120 tareas y su contenido en
  la RDRAM cambió al menos dos veces, la última hace menos de 120 tareas, se
  carga desde la RDRAM antes del primer dibujo con profundidad de cada tarea
  (z de 16 bits de la N64 descomprimida, `rdp_z16_to_z24` con su prueba en
  `test_targets.c`, a Z24S8 en el PPE y `rsxkit_depth_load` agrandada a la
  escala). Sin un volcado de las tareas 3D de RE2 (los de la consola son
  todos de la tarea S2DEX del fondo) no se pudo ver qué hacen; la línea del
  log cuenta ahora «z cargadas desde la RDRAM» y «borrados enteros puestos».
  El menú de opciones en negro no se investigó (su volcado es una tarea con
  un solo borrado de 189A00 a 320 de ancho).
- **Yoshi, puntos.** En el volcado 12008 los puntos naranjas y azules en el
  árbol turquesa salen también en el host: vienen de la capa CI8 en la
  RDRAM, o sea, de cómo la dibujó el RSX. En una capa de 8 bits el color es
  un índice de paleta y el filtro bilineal del RSX mezcla una fracción de
  los vecinos aunque la muestra caiga en el centro del texel; un índice
  mezclado es otro color. **Arreglo**: los dibujos en una capa big8 van sin
  filtro (con el medio texel de la HLE, el más cercano da el mismo texel).
  La cuadrícula regular de puntos blancos (30 objetos de 24 × 21 al final de
  la tarea) sale igual en el host y en la rc4: es salida de la HLE.
- **Yoshi, estela.** La textura de la capa trae columnas en cero (alfa 0):
  partes de la capa que no estaban en la RDRAM. Las copias ya se esperan
  todas; falta un volcado de la caja negra con las tareas que dibujan las
  capas para ver qué parte no llega.
- **Pruebas.** `tools/build.sh test-host`: 651 pruebas, 0 fallidas;
  `tools/escenas-host.sh --comparar` contra la rc5b: 9 idénticas. Falta la
  PS3 (la carga de la z de RE2, `rsxkit_depth_load`, no se probó en ningún
  lado con el RSX).
- **Paquete de prueba** «Salsa64 rc5b2» (SLSA00157, v0.2.7-72-g97cdfb1):
  `build/salsa64-rc5b2/salsa64-rc5b2.pkg`, 3 285 072 bytes, sha256
  `6269dd3de5791fc3bba4f35be898576ad2d28846bf94e7c1d678465d69c914ab`, armado con
  `tools/build.sh -C src/frontend PROGRAM=salsa64-rc5b2 "TITLE=Salsa64 rc5b2"
  APPID=SLSA00157 pkg` (los directorios de build borrados antes). En RPCS3 la
  repetición del volcado 7298 corre sin error (una sola tarea: la z vieja no
  se ve ahí).

## 2026-10-18 — rc5b: modelos a pedazos en Rogue Squadron (y quizá Resident Evil 2) y la estela de Yoshi

Reporte de la rc4 en la PS3 real: en Rogue Squadron y Resident Evil 2 partes
de los modelos 3D desaparecen; en Yoshi's Story sigue la estela en algunas
capas del fondo. Con los volcados de la consola (`build/rc4-ps3/`):

- **Lo que dice el volcado.** `salsa64-run --raster-dump` (compilado con
  `-DRASTER_W=640 -DRASTER_H=480` para ver el cuadro entero de 512 × 448)
  dibuja la nave entera en los volcados 1981 (elegir nave) y 3861 (Tatooine):
  la HLE está bien; en la captura del RSX faltan el fuselaje y los motores
  (quedan los cañones) y, en 1981, la pared del hangar. Lo que falta son los
  dibujos con prueba y escritura de z (`depth 3`).
- **Causa 1 (la principal).** Rogue no borra la z en todo el cuadro: dibuja
  el fondo (hangar, piso, terreno, cielo) con escritura de z sin prueba
  (`depth 2`: 28 dibujos en 1981, 293 en 3861) y eso la deja lista. En el
  RSX la prueba apagada apaga también la escritura, así que esos dibujos no
  escribían nada y los modelos se probaban contra la z de cuadros viejos (en
  1981 no hay ningún borrado): la nave, que gira, solo pasaba donde quedaba
  delante de todas sus posiciones anteriores. Arreglo en `lib/rsxkit`: con
  escritura y sin prueba, prueba encendida con `GCM_ALWAYS`. Confirmado en
  RPCS3 con un volcado editado (docs/rsx-notes.md).
- **Causa 2.** En 3861 la z sí se borra, con dos rellenos seguidos (mitad
  derecha y mitad izquierda, de la fila 172 hacia abajo) antes de ligarla, y
  el borrado pendiente era uno solo: el segundo pisaba al primero y la mitad
  derecha quedaba sin borrar. Ahora la profundidad guarda hasta 8 borrados
  pendientes (`zclear` y `bind` en `src/rdp/rdp.c`); si no caben, el último
  crece hasta cubrir el nuevo.
- **Resident Evil 2.** Los volcados de la consola son de la tarea S2DEX del
  fondo (sin dibujos 3D) y el host no llega al juego, así que no se pudo ver
  qué hace su z. Si sus modelos usan las mismas formas, el arreglo los cubre.
  Para saberlo, el log de la PS3 tiene una línea nueva cada 5 s, «gfx:
  profundidad: N dibujos escriben la z sin probarla, M borrados de la z (P
  pendientes, J juntados), K color images en una z image»; K > 0 quiere
  decir que el juego escribe la z como color (con el RDP), cosa que el RSX
  no ve en su profundidad (sería otro arreglo: subir esa imagen a la
  profundidad del RSX).
- **Yoshi's Story.** La textura del volcado 2689 que cubre la estela (capa
  0x250F80, filas 25–30) trae ceros en las columnas 0–95 y 290–335: la capa
  no estaba entera en la RDRAM cuando la HLE la leyó. Dos causas en las
  copias de las imágenes de 8 bits grandes (`big8`): (1) cuando la capa se
  redibuja en tareas seguidas la copia iba «sin esperar» (como las imágenes
  grandes repetidas) y la tarea siguiente leía la capa vieja de la RDRAM; las
  big8 se esperan siempre, porque el juego las lee como CI8 desde la RDRAM y
  nunca desde el RSX; (2) `to_rdram` soltaba las líneas de la caché del PPE
  con el paso de RGB565 (1280 bytes) y no con el de XRGB8888 (1344): las
  últimas filas de la caja podían salir de líneas viejas.
- **Pruebas.** `tools/build.sh test-host`: 650 pruebas, 0 fallidas. Las
  escenas del host no cambian por construcción (ningún archivo que compile
  `salsa64-run` cambió: `rdp.c`, `draw3d.c` y `main.c` son solo de la PS3).
  RPCS3: repetición del volcado editado (arriba) y de 3861 sin editar (igual
  que antes; los cuadros negros de la mira y de las sombras salen también
  con la rc4 en RPCS3: son de la repetición). La z de cuadros viejos no se
  ve en una repetición (es una sola tarea con la z recién creada): falta la
  PS3.
- **Paquete de prueba** «Salsa64 rc5b» (SLSA00156, v0.2.7-68-g8e5bbc0):
  `build/salsa64-rc5b/salsa64-rc5b.pkg`, 3 282 384 bytes, sha256
  `3dccf9d1a4c68609b3f40729b4a6bdef48bda96823cbad1eecd5ed360b9f0f19`, armado con
  `tools/build.sh -C src/frontend PROGRAM=salsa64-rc5b "TITLE=Salsa64 rc5b"
  APPID=SLSA00156 pkg` (el directorio de build borrado antes).

## 2026-10-18 — rc5a: Indiana Jones y Killer Instinct Gold se quedan en negro en la PS3 con la rc4 (sin reproducir; instrumentado)

En la PS3 real con la rc4, Indiana Jones (HLE de Factor 5, imagen 4 del SPE 0) enseña la pantalla de
mejoras del Expansion Pak y se queda en negro, y Killer Instinct Gold deja elegir personaje y se
queda en negro. Ninguno se reproduce en el host (interprete), ni con el recompilador bajo qemu
ppc64 (KI: 2522 tareas gráficas hasta el campo ~1650 con las pulsaciones de
`docs/pads/killerinstinct-pelea.pad`; Indy: 876 tareas en 1640 campos), ni con `SALSA64_GFX_LATE` /
`SALSA64_GFX_TEAR`. Esta rama no declara arreglada la causa de ninguno; trae el análisis de lo que
sí dicen los logs y los volcados de la consola, un arreglo con causa lógica para el SPE y la
instrumentación para que la próxima corrida lo confirme.

- **Killer Instinct: no es una regresión de `dp_run` (rc3).** Los volcados del cuelgue
  (`cuelgue-<campo>.rdram`) de la rc4 y de dos corridas viejas (v0.2.8-rc1/rc2, del 2026-10-16)
  son idénticos en lo que importa: el hilo principal del juego (OSThread id 3, prioridad 10) está
  *parado con la bandera de falla* (estado 1, flags 2) tras una excepción de TLB al cargar de la
  dirección 0x00010001 (CP0 Cause 8, `bad_vaddr 0x10001`), con `pc 0x800010F0` y `ra 0x800012F0`:
  es el bucle de decodificación de Huffman de una descompresión (inflate) del propio juego, cuyo
  búfer de entrada (0x802D4002, ya con más de 500 KB consumidos) aparece lleno de 0xFE. Los demás
  hilos esperan mensajes con normalidad, el hilo ocioso corre en `b .` y las interrupciones del SP y
  del DP que cuenta el latido (programadas = levantadas = 2 × listas) llegaron todas: el juego no
  espera una interrupción, se murió. `tools/libultra-threads.py --all` lo muestra (sin `--all` el
  hilo parado no sale). Falta saber qué hace diferente la consola en ese punto de la carga
  (después de elegir personaje, para la pelea): datos malos del cartucho, memoria pisada por un
  escritor que no es el juego, o una carrera del propio juego que solo sale con los tiempos de la
  consola.
- **Instrumentación nueva (PS3).**
  - El log escribe, al abrir cada juego, el CRC-32 de cada megabyte de la ROM tal como está en
    memoria y el de la ROM completa («ROM: CRC-32 por megabyte desde el 0: …»). Comparar con los de
    la misma ROM en la Mac (Killer Instinct: total 31C76BE7; Indiana Jones: 4978EB57; por megabyte
    en `python3 -c` con `zlib.crc32` sobre trozos de 1 MB) dice si el archivo de la consola o su lectura
    a memoria cambió algo (sospecha: el juego descomprime y dibuja basura tras cargar datos).
  - El aviso de cuelgue (`latido:`) imprime ahora los últimos 64 eventos del RCP (interrupciones del
    SP, SI, PI y DP que suben y bajan con MI_INTR, arranques y fines de tarea con la dirección de su
    lista, listas del CPU al RDP, interrupciones del DP levantadas) con su ciclo: el anillo está en
    `salsa64_rsp.evlog` (`salsa64_evlog_add`, mi.c, rsp.c, sp.c).
  - «gfx: tarea mala» / «lista del CPU mala»: una tarea cortada, con errores de lista o con más de
    200 000 comandos escribe su lista, ucode y contadores, lo que el PPE ve de la lista al terminar y
    lo que el SPE leyó primero de ella (`gfx_info.dl_addr`, `dl_first`). Distingue una carrera
    con el CPU emulado o una lectura distinta del SPE de una lista mala del juego.
- **Indiana Jones.** En los dos últimos trabajos gráficos antes de quedarse en negro (campos
  400–499) el SPE tardó ~75 ms por tarea (el resto, 4 ms), dibujó 9 cosas, decodificó 9,5 texturas y la
  HLE vio una color image imposible (`0xEEEEFF`, ancho 3005) y una tarea cortada: la forma de una
  lista que se acaba en memoria en ceros o en basura (cada palabra en cero es un G_SPNOOP y se
  recorre la RDRAM hasta el tope de 1 M de comandos). Dos cambios en la HLE:
  - `gfx_dl_f5indy` corta la tarea (como bucle: DP forzada, `dl_errors`) tras 8192 palabras dobles
    en cero seguidas, en vez de recorrer la RDRAM entera; prueba `f5i_zeros`.
  - `gfx_io_write` del SPE (la escritura de las partículas 0x27 a la RDRAM) leía y reescribía bloques
    de 16 bytes: si el CPU emulado (que corre en el PPE mientras el SPE dibuja) cambiaba bytes del
    bloque entre la lectura y la escritura, se los pisaba con lo viejo. Ahora escribe exactamente los
    bytes pedidos con DMA de 1, 2, 4 y 8 bytes alineados y de múltiplos de 16 (sin lectura previa).
  En el host, `SALSA64_GFX_LATE=8 SALSA64_GFX_TEAR=1` muestra que en la misión de Perú el juego sí
  cambia entradas de la tarea mientras corre (24 de 1067 tareas, invisible: 0 píxeles), así que la
  lectura tardía del SPE no es inocua para este juego.
- **Local store.** La imagen 4 del SPE 0 pasa de 224 a 220 entradas de la caché de texturas para pagar la
  escritura por piezas, la guarda de ceros y la captura de la lista (holgura 1 648 contra el mínimo de
  1 536; la 0, 1 616 como antes: la captura y `gfx_io_write` solo se compilan en la imagen 4). Paquete de
  prueba «Salsa64 rc5a» (SLSA00155), armado con `tools/build.sh -C src/frontend PROGRAM=salsa64-rc5a
  "TITLE=Salsa64 rc5a" APPID=SLSA00155 pkg` (el directorio de build borrado antes).
- **Para probar en la PS3.** Indiana Jones (misma ruta de siempre: de los logos a la misión de Perú, A
  en el Expansion Pak) y Killer Instinct Gold (elegir personaje y pelear dos veces); bajar el log
  apenas termine. Si vuelven a quedarse en negro: las líneas «tarea mala» (Indy), el
  `cuelgue-<campo>.rdram` y los eventos del RCP (KI), y los CRC por megabyte de la ROM contra los de
  la Mac.

## 2026-10-18 — Reiniciar desde el menú de pausa (botón Reset y desde cero)

- **Qué hay.** Dos filas nuevas en el menú de pausa, entre «Mostrar FPS» y «Salir», las dos con el
  cuadro «¿Seguro?» (siempre, aunque «Confirmar al salir» esté en «No»; × sí, ○ no).
  - **Reiniciar (botón Reset).** La emulación (hilo 0) da la interrupción de PRE_NMI (línea IP4 del
    Cause, `salsa64_n64_prenmi`) al final del campo siguiente a la petición y, 0,5 s emulados después
    (`SALSA64_NMI_DELAY_CYCLES`), pide el NMI como una petición de estado (`STATE_RESET`): con la
    emulación quieta en el límite del campo, presentación llama a `salsa64_boot_nmi`
    (`src/core/boot.c`). Reinicia CPU, RCP, PIF y RSP (`salsa64_rsp_reset` espera la tarea en vuelo
    de la HLE, el LLE o el audio, sin dejar el SPE a medias), conserva la RDRAM, el IPL3 HLE vuelve
    a copiar el megabyte del programa desde la ROM y deja `osResetType` (0x8000030C) y `$s5` en 1; el PC
    vuelve a la entrada. El contador de campos no retrocede. Después se traducen y sueltan las colas
    del RDP, el audio y los destinos del RSX, como al cargar un estado. El SPE 0 conserva su imagen:
    si el juego carga otro microcódigo, cada tarea elige la suya como siempre. La caché del
    recompilador se vacía por la reescritura de la RDRAM entera (`SALSA64_REC_FLUSH_RDRAM`) y por el
    reset del CPU.
  - **Reiniciar desde cero.** `run_game` se volvió un bucle sobre `run_game_once`: el menú pide
    `GAME_RESTART`, el juego se cierra entero (`game_close`: partidas al disco, SPEs, caché del
    recompilador, RSX) y se abre la misma ROM con `game_reset` y los mismos ajustes, sin pasar por la
    lista (se conservan las banderas de prueba y la ranura de estados).
- **Pruebas del host.** `tests/unit/test_boot.c` (PRE_NMI en el Cause bit 12 y plazo de 0,5 s,
  segunda pulsación sin efecto, NMI con RDRAM conservada salvo el programa, `osResetType` = 1, `$s5` = 1,
  MI sin interrupciones, arranque en frío posterior a 0) y `test_pausectl.c` (filas, cuadro siempre,
  ○ cancela). `make test-host`: 653 pruebas; las dos fallidas (`menu_scan_dir` y `files` de cover)
  fallan igual en rc4 en esta Mac. `tools/escenas-host.sh --comparar` contra rc4: 9 idénticas, 0
  distintas. `salsa64-run --reset-at C` (PRE_NMI al final del campo C, NMI 30 campos después) y
  `--nmi-at C` (NMI directo): SM64 (NMI en el campo 400 y PRE_NMI en el 380), Zelda OoT y GoldenEye
  (NMI y PRE_NMI en el 700; con GoldenEye hasta el campo 1400, porque su arranque tarda ~600 campos)
  vuelven a los logos (estrella y logo de Mario, logo «Nintendo 64» y logo de Nintendo) con los dos
  tipos. Solo con el intérprete (Mac arm64): el recompilador y los SPE no se probaron aquí.
- **Por validar en la PS3.** Paquete «Salsa64 reinicio» (SLSA00158, v0.2.7-65-g4c26c69-dirty):
  `build/salsa64-reinicio/salsa64-reinicio.pkg`, 3 283 280 bytes, sha256
  `fd09bf6bd99bafd18efff4740fba788c8dd1e07c0715f852cb6f7a55663497fe`, armado con `tools/build.sh -C
  src/frontend PROGRAM=salsa64-reinicio "TITLE=Salsa64 reinicio" APPID=SLSA00158 pkg` (directorio de
  build borrado antes). Probar con JIT, con `lle` y con la interpolación: «Reiniciar (botón Reset)»
  en SM64 (logo), Zelda (logo «Nintendo 64», sin que se rompan texturas ni el audio), GoldenEye; y
  «Reiniciar desde cero» con una partida guardada antes (que siga ahí), mirando en el log las líneas
  «reinicio: PRE_NMI…», «reinicio: NMI en el campo…» y «reinicio desde cero…» y que el SPE 0 y los
  demás arranquen sin avisos. No se vio el dibujo del menú (ni en RPCS3 ni en la consola): 10 filas
  caben en 720 de alto (622 con el marco).

## 2026-10-18 — Zelda OoT: la Lente de la Verdad no quitaba las paredes falsas (alfa de la máscara anulado)

Reporte del Pozo de Kakariko (v0.2.6/0.2.7 en la PS3): el círculo de la Lente y
los objetos ocultos salían, pero las paredes falsas no desaparecían dentro del
círculo. Con el estado guardado y el volcado de la consola (`gfxdump`) del
autor.

- **Mecanismo.** La Lente es una máscara de z: un TEXRECT de pantalla completa
  (I4 de 128 × 64 con espejo; 0 dentro del círculo, 74 fuera) con z del
  primitivo = 0, sin prueba de z, con escritura de z, blender «BL·0 + MEM·1»
  (no escribe color) y prueba de alfa por umbral contra el color de mezcla
  (8). El combiner da alfa = (PRIM_A − TEXEL0_A) · PRIM_LOD_FRAC, así que solo
  los píxeles de dentro del círculo pasan y escriben z = 0. Las paredes falsas
  se dibujan después con prueba de z (≤) y fallan dentro del círculo; el
  círculo morado (alfa del mismo TEXRECT, con mezcla) y los objetos ocultos
  no dependen de esa z.
- **Causa.** `blend_constant` (`src/rsp/gfx/rdp.c`) ponía a cero todos los
  términos k (color y alfa) de un dibujo cuyo blender usa el color de mezcla o
  de niebla con FORCE_BL, para sustituirlos por esa constante; en la máscara
  (blend KEEP) borraba también el alfa, que sí cuenta para la prueba de alfa:
  con alfa 0 el umbral descartaba todo y la máscara nunca escribía z. Lo vio
  el volcado: el dibujo 80 sin ningún término k. Es de la HLE (host y PS3);
  `--lle=all` ya dibujaba bien.
- **Arreglo.** (1) `blend_constant` no toca los dibujos de blend KEEP.
  Con el estado del autor, el host con HLE y con `--lle=all` dibujan ahora lo
  mismo: aparece el pasillo oculto dentro del círculo. (2) rsxkit
  (`set_depth`): el RSX, como OpenGL, no escribe z con la prueba apagada; una
  escritura sin prueba (esta máscara) pide la prueba con GCM_ALWAYS. Sin esto
  la máscara no escribiría z en el RSX aunque el alfa estuviera bien (se
  verifica en la PS3; RPCS3 no distingue). (3) El rasterizador del host
  tampoco escribía z en los dibujos KEEP (salía antes del blender); ahora sí.
  (4) La traza (`SALSA64_GFX_TRACE`) imprime `prim_lod` y `prim_min`.
- **Pruebas.** `tools/build.sh test-host`: 650 pruebas, 0 fallidas. Las 8
  escenas del host idénticas a antes. Falta la unidad de prueba del combiner
  de la máscara.
- **Resplandor de las antorchas a través de las paredes** (mismo Pozo): no
  depende de esto. Es la z que Zelda lee con el CPU (memoria «La z image que
  lee el CPU»); el arreglo natural es copiar la z dibujada del RSX a la RDRAM
  cuando el juego la lee (la lectura de una z de 320 × 240 × 2 B = 150 KB por
  cuadro: del orden de 0,3–0,5 ms de RSX más la espera de su barrera; solo en
  las escenas donde se detecte la lectura). La Lente no lee la z con el CPU.
- **Paquete de prueba** «Salsa64 lente» (SLSA00159, v0.2.7-65-g4c26c69-dirty):
  `build/salsa64-lente/salsa64-lente.pkg`, 3 281 936 bytes, sha256
  `766cfe85250cc6594403f3ccf06d7ff2d49822ae481f3dab2557d9ab6ca3cabd`.
  Probar: Pozo de Kakariko con la partida del autor, usar la Lente frente a
  la pared falsa: debe desaparecer dentro del círculo y mostrar el pasillo.

## 2026-10-18 — rc3: DPC continuado en la PS3 y el «LLE vacío» del censo era pantalla fija

- **PS3 (por validar en la consola).** `src/rdp/rdp.c` (`translate`) no llama a
  `rdp_targets_task` en los trabajos con `GFX_JOB_DPC_CONT` (la ranura lo guarda en `done`);
  `spu_dpc_run` (`gfx_spu.c`) pone la bandera mientras la lista no haya visto SYNC_FULL ni corte,
  como `sync.c`. Contadores nuevos al final del log de la PS3: «gfx: listas del RDP: N trabajos
  continuaban una lista abierta (… M del SPE); K destinos que el envejecimiento por tramo habría
  vuelto a cebar desde la RDRAM». Para probar: Killer Instinct con JIT (dos peleas seguidas),
  Mario Kart 64 y juegos con F3DEX viejo en modo LLE, y la HLE de siempre (N debe ser 0 sin LLE ni
  listas del CPU).
- **Censo.** En GoldenEye (campo 216) y Mace (530) el LLE no estaba vacío: la escena era fija y
  dejaba en el búfer lo mismo que ya había; la medición lo contaba como vacío. Ahora «vacía» exige
  además menos triángulos y rectángulos del LLE que de la HLE. En 12 juegos pasan de 13 152 a 1 069
  tareas vacías (solo Yoshi's Story, S2DEX 1.06, tiras de `TEXRECT` en modo copia). Detalle en
  `docs/gfx-censo.md`, punto 4. Volcado crudo: `SALSA64_GCHK_DPC_TRACE=campo_ini,campo_fin`.

## 2026-10-17 — Resident Evil 2: bloques blancos, ruido y rectángulos negros (cebado de superficies)

Con el S2DEX suelto, Resident Evil 2 llegaba a los menús en la PS3 con
glitches fijos en pantalla: un rectángulo de ruido arriba a la derecha (x 256 a
319, y 0 a 12), bloques y cuñas de color crema sobre el menú (la misma cuña en
el título y en «Saved games»: un borde con pendiente 4:3 que pasa por
(97, 167) y bloques alineados a 64 píxeles) y rectángulos negros sobre el
logo.

- **Qué se vio en los volcados.** Los dos `gfxdump` (SELECT + R2) son
  idénticos: la tarea que el volcado guarda es la última de cada cuadro, la
  limpieza de la z image de `0x189A00` (un G_BG_COPY de 512 × 150 desde
  `0x6D4000`, que es una imagen crema con ruido en las primeras filas), no la
  que dibuja el menú. Un cuadro de RE2 son cuatro tareas: la copia del fondo de
  video a `63E000`/512 (512 × 150 = 320 × 240 píxeles: es una copia lineal), una
  de ajustes con tres `SETCIMG` (`189A00`/512, `63E000`/512 y `63E000`/320), la
  de F3DEX2 con los TEXRECT del logo y el menú sobre `63E000`/320 y la limpieza
  de `189A00`. La captura del RSX trae, en los píxeles malos, exactamente los
  datos de esa imagen crema colocados linealmente (147 de 256 píxeles iguales
  en el rectángulo de ruido, con la pérdida de RGB565; 1 con cualquier otro
  desplazamiento).
- **Causa.** `prime` (`src/rdp/rdp.c`) ceba la superficie de un destino con lo
  que hay en la RDRAM copiándolo a un búfer de memoria principal y encolando
  un blit (`rsxkit_target_fill` solo encola; el RSX lee el búfer después). Era
  un solo búfer para todos los cebados: el cebado de `189A00` (la z image
  crema, 1024 bytes por fila) pisaba el búfer con la superficie de
  `63E000`/320 todavía sin leer por el RSX, que va unas tareas atrás, y la
  superficie presentada quedaba con pedazos de la z image (la forma depende de
  cuánto había leído el RSX: líneas de 128 bytes y bordes de triángulos del
  blit, siempre en el mismo lugar porque el ritmo es el mismo). Ya estaba
  anotado como «observado y sin tocar» en la entrada de Star Fox 64
  (2026-10-12), donde el relleno posterior lo escondía.
- **Arreglo.** Cuatro búferes de cebado en rotación (`PRIME_BUFS`) con la
  barrera del RSX de cada uno (`rdp.prime_fence`): un cebado espera solo si el
  RSX no ha leído el que usó hace cuatro cebados. `docs/memory-budget.md`: el
  pool de framebuffer pasa de 3 a 5 MB. El log cuenta las esperas («N esperaron
  la barrera de su búfer de cebado»).
- **Sin reproducir en el host.** El host no usa `src/rdp/rdp.c` y en él el
  video de RE2 sale desordenado desde el campo ~1950 (los mismos 512 × 150 que
  la PS3 ve bien: el host reproduce solo 39 tareas de video, la PS3 150), así
  que la comprobación es la de la PS3. Las tareas de RE2 se pueden volcar en el
  host con `SALSA64_GFX_DUMP_TASKS` y dibujar con `--raster-dump`: la tarea de
  los TEXRECT sale limpia (logo y menú) también ahí, o sea que el dibujo de
  la HLE no es el problema.

## 2026-10-17 — Yoshi's Story: el cielo y las capas del fondo

El fondo del nivel (cielo, colinas, árboles lejanos) salía negro y luego como
estela de revuelto de colores, igual con la HLE que con el JIT del RSP.

- **Causa.** El juego dibuja cada capa con el RDP en una color image de 8 bits
  de 336 × 256 (0x250F80, 0x266580 y 0x27BB80) y después la usa como imagen CI8
  con paleta en `G_BG_1CYC`. Esas imágenes nunca llegaban a la RDRAM: la copia del
  destino a la RDRAM de las de 8 bits solo se hacía en el host para las de ancho
  de VI, y en la PS3 sale por RGB565, donde el índice conserva 5 de 8 bits (la
  paleta salía revuelta). Con `--lle=all` en el host la RDRAM sí recibe las capas
  y el cielo sale bien; con la HLE, 0 bytes distintos de cero en esas tres imágenes.
- **Arreglo.** Las imágenes de 8 bits de más de 128 de ancho (`big8` en
  `src/rdp/rdp.c`): superficie a 1× (reducir a 2× mezclaría índices vecinos),
  copia a la RDRAM en XRGB8888 con los 8 bits del rojo, y sin los espejos chicos.
  En el host (`src/host/raster.c`), esas imágenes también se piden a la RDRAM.
  Con eso el cielo, las colinas y los árboles salen **idénticos** a los de la
  LLE en el nivel del campo 2100 (HLE y LLE sin diferencias visibles).
- **Pruebas.** `tools/build.sh test-host` 616 pruebas, 0 fallidas;
  `tools/escenas-host.sh --comparar`: 8 imágenes idénticas.
- **Falta:** la PS3 real con el paquete «Salsa64 Yoshi cielo» (SLSA00147). La
  rama de la PS3 no se pudo medir aquí (RPCS3 no llega al nivel).

## 2026-10-18 — Indiana Jones and the Infernal Machine y Battle for Naboo: HLE gráfica de Factor 5 (host verificada contra la LLE, imagen del SPE 0 y RPCS3; falta la PS3); MusyX v2 y el microcódigo de Animal Crossing

Indiana Jones solo corría con el RSP a bajo nivel (en el cañón de la primera misión,
1,7 M de instrucciones del RSP por tarea gráfica, 33,7 M en las 20 de 40 campos; la
tarea más larga de todas, 1,5 M: con los ~49 MIPS del recompilador del RSP en la PS3, del
orden de 30 ms por tarea, y 400 ms con el intérprete). Ahora `src/rsp/gfx/f5indy.c` interpreta su
microcódigo (el de «F5Indi_Naboo» de GLideN64; sin texto de identificación, se
reconoce por el CRC-32 de sus 4 KB leídos en palabras del host, `0x6859BF8E`) en
el host, el PPE y una quinta imagen del SPE 0 («Factor 5 Indy»,
[spe0-familias.md](spe0-familias.md)). Battle for Naboo trae el mismo microcódigo
(`0x23FEF05F`, también reconocido): sus comandos propios están traducidos, pero
**sin respaldo del juego no se probaron** (más abajo). El archivo `lle` con «all»
(docs/rsp-lle.md) sigue dando el microcódigo real para cualquiera de los dos; sin el
archivo, un juego sin HLE no dibuja.

- **Licencia.** Traducción a C de `src/uCodes/F5Indi_Naboo.cpp` de GLideN64 (GPLv2
  sola; commit `a04081a`), como f5rogue.c: el archivo lleva `GPL-2.0-only` y su
  aviso de origen y el binario que lo enlaza se distribuye como GPLv2 (README,
  docs/THIRD_PARTY.md, docs/proyecto.md).
- **Qué es el microcódigo.** Una Fast3D cuya memoria es la DMEM del RSP: casi todo
  comando es una DMA a ella (G_MOVEMEM 0x01: de la RDRAM a una dirección de la DMEM, o
  de un flujo de bloques encadenados que arma G_SET_DLIST_ADDR) y lo que hace con lo traído
  depende de a dónde va: el viewport (0x130), la matriz combinada (0x5C0), la matriz de
  los vértices generados (0xE40), vértices de 8 bytes (0x170: con o sin el desplazamiento
  de 0x128, o por índices a una tabla de la RDRAM con 0x154), las luces (0xB00) y la
  iluminación misma sobre 0x480 con resultado en 0xD40 (tres variantes: la «básica», la
  de «overlay 1», con luces ambientales o puntuales, y la de «overlay 2», con luces
  ambientales o direccionales con normales). Los triángulos (0xBF, 0xB4) dicen sus
  vértices por la dirección de su lugar (0x600 + 40 por vértice) y sus colores por un
  desplazamiento en la tabla de 0xD40; con textura, sus coordenadas detrás o calculadas
  con una matriz (mapeo de entorno). Una lista secundaria (la dirección de 0x58C) corre
  antes de dibujar. De G_GEOMETRY_GEN (0x05): 0x18 (vértices con la matriz de 0xE40),
  0x24 (rectángulos de textura a partir de los vértices de unas partículas), 0x27 (las
  mueve y deja sus vértices en la RDRAM) y, de Naboo, 0x09 y 0x0C (mallas de terreno
  con colores por vértice), 0x0F, 0x15 y 0x4F y el 0xBE. La traducción lleva una DMEM de
  4 KB propia con la misma disposición (los accesos «por puntero» de GLideN64 corren en un
  host little-endian con palabras invertidas: aquí van en el orden natural).
- **Cómo se verificó.** Como Rogue: `salsa64-run --lle=all` (el microcódigo real) como verdad,
  desde estados guardados con la HLE (`--state-save` / `--state-load`; el flujo del juego
  se maneja con START y A cada 120 campos hasta la misión de Perú, y de ahí con botones al
  azar). Con `SALSA64_GFX_CYCLES=1000` y `--lle-speed 200` las tareas duran lo mismo y el
  juego no diverge. Se comparó la imagen del VI cada pocos campos, cada triángulo que los
  dos mandan al rasterizador (posición, s, t y color de cada vértice, pareados por
  cercanía), cada rectángulo de textura (las palabras que llegan a `gfx_texrect`) y la
  DMEM al terminar cada tarea (la de la LLE es la verdadera; la de la HLE se vuelca al
  final: 0xD40 a 0xDAF son los colores ya iluminados). Esas trazas y volcados son
  temporales y no están en el árbol. El desensamblador del RSP (también local) leyó el
  microcódigo y sus overlays de iluminación y de partículas cuando hizo falta.
- **Lo que se midió** (y por eso el código no es el de GLideN64):
  - Las **coordenadas de textura de los triángulos** van en 1/32 de texel con y sin
    perspectiva (GLideN64 usa 1/16 sin ella: el fondo y los carteles salían al doble);
    y la escala de G_TEXTURE no cuenta (el juego la manda en cero).
  - La **y de pantalla va al revés** de Fast3D, como en Rogue.
  - La **iluminación básica y la ambiental** usan el redondeo de los VMULF del
    microcódigo, `(a·f + 1) >> 8` y `((((a·f + 1) >> 1) · l) + 128) >> 15` (sobre los
    bytes en la mitad alta con LUV), no los truncamientos de GLideN64; en los datos medidos
    dan lo mismo (la tabla de colores sale idéntica a la del RSP real en las 38 tareas
    medidas de dos escenas del cañón y la cueva).
  - La **iluminación direccional con normales (overlay 2, tipo 1)**, la que más corre en
    los niveles (122 000 llamadas en 3000 campos del cañón), difiere en ±1 de un canal en
    algunas palabras contra el RSP real (medido en «ENTER NAME»: 401 de 4256 bytes de la tabla
    en 38 tareas): sale del redondeo de sus VMULF encadenados, que no se reprodujo (se
    leyó el código del overlay: producto de la normal por la dirección con VMULF, suma de
    las tres componentes, producto con el color de la luz, y otro con dato · factor).
    El overlay 1 solo trae el tipo 0 y las luces puntuales; en las escenas medidas corren el
    tipo 0 de overlay 1 y los tipos 0 y 1 de overlay 2 (las puntuales, en otro tramo del juego).
  - Los **rectángulos de partículas (0x24)**: las esquinas son idénticas a las del RSP
    real; dsdx y dtdy salen 0xF más altos (4111 contra 4096: GLideN64 toma una palabra
    de la matriz de 0x5C8 que el microcódigo real parece no usar así). Sin efecto visible.
  - Las **partículas (0x27)** escriben en la RDRAM sus vértices (256 bytes) y su
    máscara: `gfx_io_write` (memcpy en el host y el PPE; en el SPE una DMA con lectura
    previa de los bytes vecinos). El reloj aleatorio con que reaparecen lo da el
    registro DPC_CLOCK del RDP: aquí es un contador, así que sus posiciones de
    reaparición no coinciden con las de la LLE.
  - La **lista secundaria** solo corre comandos de estado (el modo geométrico, othermode,
    textura, MOVEWORD y el RDP), sin triángulos ni rectángulos ni más listas: así no hay
    recursión (el SPE no la admite) y GLideN64 tampoco la usa.
  - La **raíz cuadrada** del mapeo de entorno va entera (el piso exacto de la de doble
    precisión), los productos y divisiones de 64 bits de las partículas con los enteros de
    64 bits del compilador (2,7 KB de libgcc en el SPE: `__muldi3` y `__udivdi3`) y el
    desbordamiento de enteros con signo con aritmética sin signo.
- **Contra el RSP real** (mismo método; porcentaje de píxeles a más de 24 por canal; 320 × 240;
  el tramado alfa y el filtro de las texturas del rasterizador hacen el resto):

  | Escena | Campo | Píxeles distintos |
  |---|---:|---:|
  | «ENTER NAME»: arena y ventana de la foto (triángulos con textura) | 2950 a 2964 | 0,7 a 0,8 % |
  | el cañón de Perú con Indiana Jones y el HUD | 5412, 5424 | 4,5 % (0,2 % a más de 40) |
  | lo mismo, con partículas, 40 campos | 7208 a 7240 | 5,4 a 9,3 % |
  | la cueva oscura | 12 012 a 20 016 | 0,00 a 0,05 % |

  Por vértice (el cañón, 12 tareas, 78 945 vértices): posición a 0,25 píxeles de media
  (p95 0,6; la HLE no trunca a cuartos de píxel), s y t a 0,06 texels de mediana, color
  igual o +1 (el decodificador de triángulos del rasterizador del host redondea el
  sombreado; el microcódigo lo trunca) y 3,5 % de vértices sin pareja a menos de 1,5
  píxeles (las capas que se superponen). Rectángulos: 12 288 de 12 444 con las mismas
  esquinas, 1037 menos que la LLE (las partículas que reaparecen).
- **Sin comandos sin implementar** ni errores: 12 000 campos desde el arranque (8859
  tareas, con el búfer de comandos de 3 MB: 0 cortadas) y 30 000 con botones al azar
  (27 020 tareas; con el búfer de 512 KB del host, 904 cortadas, 3,3 %: las de las
  partículas).
  Por tarea (campo ~7200, el cañón): 11 136 comandos, 2125 triángulos, 1945 rectángulos y
  1056 dibujos, con ~800 triángulos y ~250 dibujos en la cueva. **Con el búfer de comandos del
  host (512 KB; 496 bytes por dibujo) 20 de 20 tareas de esa escena salían cortadas** y
  el HUD, que va al final, faltaba. La PS3 tiene 768 KB (1548 dibujos): mirar en su log
  «tareas cortadas» y, si salen, subir `CMD_BYTES` de `src/rdp/rdp.c`.
- **Imagen del SPE 0.** `GFX_SPE0_F5INDY` = 4 con solo `f5indy.c` (-Os, sin F3D): texto
  121 360, hasta `_end` 251 040, cadena más honda 9280 y holgura 1808 (mínimo 1536) con la
  caché de texturas en 224 entradas (con 768 la holgura era −10 240: la estructura de los
  pasos de Naboo pasó al bss, el recorrido de la malla sale de la función que genera los
  vértices, y la caché bajó; en el host, 0,2 texturas decodificadas por tarea contra 0,1
  con 1024). Las otras cuatro, con el mismo build: holgura 1616 (la 0: el `switch` de
  `gfx_spe0_image` tiene un caso más y mueve 128 bytes el `_end`), 7056, 14 512 y 3344.
  Autoprueba contra el PPE con una lista propia de Indiana Jones (viewport y matrices por
  DMA, vértices, colores, triángulos con y sin textura, rectángulo, las dos iluminaciones
  con una luz puntual, G_GEOMETRY_GEN 0x18 y partículas 0x24).
- **RPCS3.** `spe0-todas`: las cinco imágenes arrancan, «hash igual al del PPE, tareas de
  prueba iguales al PPE» en todas (la de Indiana Jones, 4,1 s en comprobar). El juego,
  abierto desde el menú de pruebas: el SPE 0 cambia de F3D a «Factor 5 Indy» en la
  tarea 1, 255 tareas en 500 campos a ~53 campos/s (RPCS3 no es concluyente en tiempos),
  0 errores ni comandos sin implementar, 800 triángulos y ~210 dibujos por tarea en los
  títulos y las primeras escenas.
- **Pruebas.** 12 nuevas (`tests/unit/test_gfx_f5indy.c`: listas con cabecera y saltos,
  MOVEWORD (niebla y recorte), othermode con la máscara contada desde arriba, vértices
  por DMA y su transformación, triángulos con colores por índice y con textura,
  la DMA del flujo encadenado entre dos bloques, la iluminación básica, la lista
  secundaria, el rectángulo de 16 bytes, los vértices de 0x18 y las partículas 0x27) y
  la detección por CRC en `test_rsp.c` (Indiana Jones y Naboo; Animal Crossing, abajo);
  `tools/build.sh test-host` (con ASan y UBSan): 640 pruebas, 0 fallidas;
  `tools/escenas-host.sh --comparar` contra el árbol anterior (Banjo-Kazooie, Zelda,
  DK64, Perfect Dark, GoldenEye, Mario Kart 64, Kirby 64 y la casa de Link; sin SM64: no hay
  respaldo en la carpeta de pruebas): 8 de 8 idénticas; `tools/test-roms.sh`: rc 0.
- **Lo de Battle for Naboo.** Los comandos de su microcódigo (G_GEOMETRY_GEN 0x09 y 0x0C:
  vértices con colores mezclados de dos mallas y el recorrido de los triángulos en pasos,
  0x0F con 14 palabras de datos, 0x15: un rectángulo de textura desde un vértice, 0x4F y
  el 0xBE: las coordenadas del cuadrilátero y el recorrido) están traducidos de GLideN64 con
  los accesos de la DMEM al orden natural, **pero no hay respaldo del juego para
  comprobarlos contra el RSP real**: lo que sí está probado es que compilan, caben en
  la imagen del SPE (18 088 bytes de texto: sin ellos la imagen tendría 103 272 de
  texto y 20 448 de holgura, o una caché de ~860 entradas) y que la autoprueba no los toca. Para probarlos, correr
  Battle for Naboo con `--lle=all` y con la HLE desde un mismo estado y comparar como arriba
  (la DMEM al terminar cada tarea dice primero si las mallas generadas coinciden).
- **Paquete de prueba** «Salsa64 Indy Naboo» (SLSA00153, v0.2.7-51-g28896b0):
  `build/salsa64-indy/salsa64-indy.pkg`, 3 280 656 bytes, sha256
  `d184e4b7c7679fecd7e37a5d67f238e14c8533f92f987ab2d93c82a46572c6c5`, armado con `tools/build.sh -C src/frontend PROGRAM=salsa64-indy
  "TITLE=Salsa64 Indy Naboo" APPID=SLSA00153 pkg` (el directorio de build borrado antes).
  Para probar en la PS3: abrir Indiana Jones and the Infernal Machine (de los logos a la
  misión de Perú: START y A) y mirar en el log que el SPE 0 cambie de F3D a «Factor 5
  Indy» con las «tareas de prueba iguales al PPE», que las tareas gráficas den 0 «con
  microcódigo desconocido», 0 comandos sin implementar y 0 «tareas cortadas», los campos
  por segundo en el cañón (con la LLE, del orden de 30 ms de RSP por tarea), y, si se ve mal, el
  HUD (el último de la tarea), las partículas, el cielo, y los personajes (la
  iluminación). Si el SPE llega a leer ceros (líneas «16384 comandos» en el log), avisar.
- **MusyX v2: `ptr_10`.** El audio de Indiana Jones ya iba idéntico en el censo del
  2026-10-16 (5675 de 5724 tareas; las otras 49, ±1). Con el juego jugado (la misma misión y
  la cueva, de los campos 8000 a 27 000 desde estados guardados), el oráculo
  (`--lle=all --lle-check-audio`) comparó 16 990 tareas más: **0 distintas** (ni las ±1)
  y **`ptr_10` no vino distinto de cero en ninguna** (un contador temporal en
  `musyx_v2_task` no imprimió una sola línea). La etapa existe en el microcódigo real
  (la tarea la procesa tras `lw a1,608(zero)`, con el código de 0x57C y 0x680 del
  microcódigo de audio sobre una estructura aparte, que no se leyó con detalle) y el
  HLE de mupen64plus tampoco la hace; sin un juego que la use no se puede medir y
  reconstruirla del desensamblado sin datos reales sería adivinar. Sigue contada como
  comando sin implementar (el log dice «N sin implementar (último 0x…)» con el valor de
  `ptr_10`). Para hacerla: un juego que la use (Battle for Naboo, que usa v2) y
  `--lle-check-audio` desde un estado con la tarea cerca, como arriba.
- **Animal Crossing (issue #18, traducción de Dōbutsu no Mori, 32 MB, «ANIMAL FOREST»).** El
  log de la PS3 decía «tareas gráficas con microcódigo sin HLE (datos en
  0x0011b080: "")»: sus dos microcódigos gráficos no traen el texto de versión. No hay
  respaldo aquí, así que no se pudo medir, pero la lista de microcódigos especiales de
  GLideN64 (`src/GBI.cpp`) trae dos entradas de «Animal Forest» con los CRC-32 del código
  (en palabras del host, el valor que usa también la tabla de `rsp.c`): `0xD39A0D4F`, un
  **F3DZEX2 de la familia del de Majora's Mask** (sin recorte contra el plano cercano, el
  «.NoN»), y `0x02C399DD`, un **S2DEX2**. Los dos ya están en la tabla de
  `src/core/rsp.c` (`ucode_crcs_w`, ahora con la bandera del plano cercano) y
  `test_rsp.c` los comprueba con un código fabricado para que dé ese CRC. Como ninguno es
  una variante nueva (F3DZEX2 y S2DEX2 los tenemos, de Zelda y de Kirby) no hay HLE nueva
  que escribir; si el juego sigue sin dibujar, el log de la PS3 imprime ahora, además
  de la línea de siempre, «RSP: el microcódigo desconocido: código en 0x…. (N bytes),
  CRC-32 0x… de los bytes y 0x… de las palabras del host; datos N bytes, CRC-32 0x…»
  (`salsa64_rsp_ucode_ident`): con ese CRC se ve de inmediato si es otra
  compilación (la versión japonesa de Dōbutsu no Mori, la traducción sobre otra
  base o el «+») y se añade a la tabla.
- **Falta.** Probarlo en la PS3 (el cañón, el HUD y las partículas; los campos por
  segundo; «tareas cortadas»); los ±1 de la iluminación direccional y las puntuales
  (overlay 1 y 2 con luces de tipos 2 en adelante, que corrieron poco: 13 000 y 10 000
  llamadas en 12 000 campos sin comparar); Battle for Naboo (un respaldo); `ptr_10` de MusyX v2.

## 2026-10-18 — Rogue Squadron: HLE del microcódigo gráfico de Factor 5 (host verificada, imagen del SPE 0 en RPCS3; falta la PS3)

Rogue Squadron solo corría con el RSP a bajo nivel (10 a 13 campos/s en la misión
de la PS3). Ahora `src/rsp/gfx/f5rogue.c` interpreta su microcódigo («F5Rogue»:
sin texto de identificación; se reconoce por el CRC-32 de sus 4 KB leídos en palabras
del host, `0xDA51CCDB`, el valor de GLideN64) en el host, el PPE y una cuarta
imagen del SPE 0 («Factor 5», [spe0-familias.md](spe0-familias.md)). Indiana Jones y
Battle for Naboo usan otro microcódigo de Factor 5 (HLE en la entrada de arriba).

- **Licencia.** Es una traducción a C de `src/uCodes/F5Rogue.cpp` de GLideN64
  (GPLv2 sola; commit `a04081ad`), la primera de ese proyecto que entra en el árbol: el
  archivo lleva `GPL-2.0-only` y su aviso de origen, y el binario que lo enlaza se
  distribuye como GPLv2 (README, CONTRIBUTING, docs/THIRD_PARTY.md, docs/proyecto.md).
- **Qué es el microcódigo.** Fast3D con la lista de longitud variable: cada comando
  trae sus datos detrás (viewport y escala de coordenadas de textura en `G_MOVEMEM`,
  los colores y coordenadas de un triángulo, los 40 bytes de `G_TRIGEN`); cada
  lista empieza con una palabra cuyos 24 bits bajos son el destino del `0xB5`.
  Vértices de 8 bytes que van siempre desde el lugar 0 del búfer, triángulos con un
  índice de color por vértice en una tabla, mallas de relieve generadas por el
  microcódigo (rejillas de 5 × 5 y de 3 × 3 con alturas de 8 bits y un cuadrilátero; el
  terreno y el cielo de Tatooine) y rectángulos de textura generados desde un vértice
  (carteles). El resto (G_TEXTURE, modo geométrico, G_SETOTHERMODE) es el de Fast3D
  y lo hace `f3d.c`; el cuadro completo está en la cabecera de `f5rogue.c`.
- **Cómo se verificó.** `salsa64-run --lle=all` (el microcódigo real) como verdad:
  los estados se guardan con la HLE (la LLE no guarda), y con `SALSA64_GFX_CYCLES=1000`
  y `--lle-speed 200` las tareas duran lo mismo en las dos y el juego no diverge;
  `--stop-gfx N:archivo` vuelca la imagen tras la tarea N, y un registro de cada
  triángulo de la HLE y de cada cuadrilátero del RDP de la LLE (temporal, no está en el
  árbol) permite comparar vértice por vértice. Se llega a las escenas con START y A
  cada 120 campos desde el 400 (la intro se salta; «SELECT LEVEL» hacia el campo 4000,
  «MISSION GOALS» hacia el 12 000 y el vuelo hacia el 14 000).
- **Lo que se midió** (y por eso el código no es el de GLideN64):
  - La **y de pantalla va al revés** de Fast3D (GLideN64 da vuelta el signo del
    viewport y niega la y; aquí el viewport conserva su signo, así que se invierte la y
    de cada vértice y su recorte en y).
  - Los **`G_TEXRECT` (0xE4) de la lista son de 16 bytes**: s, t, dsdx y dtdy van en la
    palabra siguiente, sin opcode, no como los dos comandos de Fast3D. Con el despacho de
    F3D los ~200 rectángulos por tarea de los menús y el HUD se perdían.
  - La **escala de s y t** de los vértices (mitad alta del producto con la escala de
    G_MOVEMEM 0x82 más la baja del producto con la segunda) coincide con la del RDP, en 1/32
    de texel con la perspectiva activa; con ella el RDP trae además una W (constante de
    2,0 en los carteles de estrellas de la intro): la HLE usa la del vértice.
  - La **opción 0x02 de la rejilla de 3 × 3** mezcla en GLideN64 hacia la altura 9 (la
    rejilla no la usa): aquí hacia la 7, como su ponderación y su color.
  - El **alfa de los vértices es el factor de niebla** (con G_FOG) y sale igual que
    en el RSP real (±1: 229 y 229, 135 y 136). El RSP a bajo nivel del host no pone el
    modo geométrico G_FOG en su estado y por eso no aplica niebla: las diferencias de 2 a
    5 % del texto que se aleja son la niebla de la HLE.
  - El **plano cercano** se quedó como el de F3D (z = −w): mover el recorte a la
    profundidad 0 de pantalla (el RSP real deja los vértices recortados en 0,001) da
    el mismo resultado con el viewport que trae la lista.
  - El **rectángulo generado**: 208 de 268 comandos idénticos y 60 con el tamaño a un
    cuarto de píxel (el RSP real usa una w algo distinta; la HLE trunca la del vértice). No se
    ha afinado.
  - El RSP real **trunca x e y de los vértices a cuartos de píxel**; la HLE no (las
    posiciones difieren hasta 0,25 píxeles).
- **Contra el RSP real** (mismo método; 6 tareas desde cada estado; porcentaje de
  píxeles a más de 16 por canal; 320 × 240):

  | Escena | Campo | Píxeles distintos |
  |---|---:|---:|
  | cinemática de la trinchera y los cazas | 2000 | 1,84 % |
  | «SELECT LEVEL» (hangar con la nave y el texto) | 4000 | 0,81 % |
  | principio del «crawl» | 5000 | 0,45 % |
  | el «crawl» | 6000 a 9000 | 2,3 a 5,3 % (la niebla, arriba) |
  | planeta en órbita | 10 000 | 0,06 % |
  | «MISSION GOALS» | 12 000 y 13 000 | 12,3 y 12,5 % (el cielo, abajo) |
  | vuelo y misión en Tatooine (cielo y terreno generados) | 11 000, 14 000 a 17 000 | 11,7 a 49 % |

  El cielo es un degradado casi liso: los promedios de color coinciden a ±3 de 255
  (el tramado y el filtro de las texturas difieren), por eso el porcentaje a más de 16
  asusta más que la imagen. Por vértice, en 2 o 4 tareas de varios estados: los de
  `G_TRIGEN` 5 × 5 coinciden con los del RSP real en las 448 de 448 (100 %); en 3 × 3
  y en el cuadrilátero, 79 % de los triángulos (los demás los recorta o los quita el RSP
  real fuera de la pantalla); colores en 94 a 100 % de los vértices y coordenadas de
  textura en 83 a 100 %.
- **Sin comandos sin implementar** en 9000 y 17 000 campos desde el arranque, ni
  errores ni tareas cortadas. Por tarea en el vuelo (campo 15 000, 1000 tareas): 9978
  comandos, 1500 triángulos, 17 rectángulos y 336 dibujos.
- **Velocidad en el host.** Del campo 15 000, `salsa64-run` con la HLE: 2000 campos en
  5,8 s; con `--lle=all`, 1000 campos en 12,2 s: ~4× por campo (con el rasterizador
  del host en los dos). En la PS3 falta medirla.
- **Imagen del SPE 0.** `GFX_SPE0_F5ROGUE` = 3 con F3D y `f5rogue.c` (-Os): texto 102 064,
  hasta `_end` 248 848, cadena más honda 9936 y holgura 3344 (mínimo 1536); las otras
  tres, con el mismo build, 1744, 7056 y 14 512. Sin aritmética de 64 bits (el rectángulo
  generado va en flotante) y las opciones de las mallas son datos. Autoprueba contra el PPE con
  una lista propia de Rogue (`selftest.c`).
- **RPCS3.** La imagen arranca y pasa su autoprueba («tareas de prueba iguales al PPE»), y
  con el juego (el menú abre la ROM de Rogue) corre 970 tareas por 1800 campos a 60
  campos/s sin errores, con la HLE dibujando (54 a 323 dibujos por tarea). Al añadir la
  imagen, las autopruebas de otras imágenes empezaron a salir distintas en RPCS3 (el SPE
  leía ceros en vez de la display list, según el diseño del código; con el intérprete
  de SPU de RPCS3 no pasa): las direcciones efectivas de las DMA de la HLE gráfica son ahora de 32
  bits (`ea32`, `io_spu.c`; la memoria principal está bajo 4 GB) y no se ha vuelto a
  ver en tres compilaciones; detalle en [spe0-familias.md](spe0-familias.md).
- **Pruebas.** 12 nuevas (`tests/unit/test_gfx_f5.c`: listas con cabecera y saltos,
  segmentos y niebla, `G_VTX` (y invertida), triángulos con colores y coordenadas de
  textura (con y sin perspectiva), sombreado plano por el vértice del índice, `G_TRI2`,
  las tres mallas de `G_TRIGEN` con su opción de interpolar, `G_TEXRECT` de 16 bytes y el
  rectángulo generado) y la detección por CRC en `test_rsp.c`; en Docker
  (`tools/build.sh test-host`, con ASan y UBSan) 628 pruebas, 0 fallidas;
  `tools/escenas-host.sh --comparar` contra el árbol anterior: 8 de 8 idénticas;
  `tools/test-roms.sh` rc 0.
- **Paquete de prueba** «Salsa64 Rogue HLE» (SLSA00150, v0.2.7-44-gcbdd240):
  `build/salsa64-rogue/salsa64-rogue.pkg`, 3 160 592 bytes, sha256
  `35359ada5d1dcbcb632866a29713f5bafab7fc794e46a0107240c87fe1e75d1f`, armado con
  `tools/build.sh -C src/frontend PROGRAM=salsa64-rogue "TITLE=Salsa64 Rogue HLE"
  APPID=SLSA00150 pkg` (el directorio de build borrado antes). Para probar en la PS3: abrir
  Rogue Squadron (el menú, «SELECT LEVEL», una misión de Tatooine en vuelo) y mirar en el
  log que el SPE 0 cambie de F3D a «Factor 5» con las «tareas de prueba iguales al PPE»,
  que las tareas gráficas den 0 «con microcódigo desconocido» y 0 comandos sin
  implementar, los campos por segundo en la misión (con la LLE: 10 a 13) y, si se ve
  mal, el cielo y el terreno, el HUD y los carteles (rectángulos generados). Si el SPE
  llega a leer ceros (líneas «16384 comandos» en el log), avisar.
- **Falta.** Probarlo en la PS3; la niebla de la HLE para el RSP a bajo nivel; afinar el
  tamaño del rectángulo generado y cuantizar x e y a cuartos de píxel si la diferencia
  se notara; Indiana Jones y Battle for Naboo (`F5Indi_Naboo.cpp`: HLE en la entrada de
  arriba) y MusyX v2, como siguientes.

## 2026-10-18 — Censo gráfico: el microcódigo real contra la HLE (`--lle-check-gfx`)

Herramienta de medición, no un arreglo: `salsa64-run --lle=all --lle-check-gfx`
corre cada tarea gráfica en el microcódigo del juego y en la HLE desde el mismo
estado del RDP, dibuja las dos salidas en el mismo rasterizador y compara los
píxeles (idéntica, menor, distinta), el estado final del RDP y los comandos que
la HLE no implementa. Método, tolerancias, tablas y lista priorizada en
**docs/gfx-censo.md**; `tools/censo-gfx.sh` y `tools/censo-gfx-tabla.py` lo
repiten. 47 juegos, 105 373 tareas: 28 032 (27 %) con el LLE casi vacío (el
camino LLE → RDP falla en F3DEX 1.x, F3D de Killer Instinct y Mortal Kombat
Trilogy y otros: hace falta arreglarlo antes de fiarse del censo ahí); de las
74 005 válidas, 48,2 % idénticas, 23,1 % menores y 28,7 % distintas. Con F3DEX2 y
Zelda (Smash, Kirby, Paper Mario, Resident Evil 2, Stadium 2, Mario Party,
Majora) la HLE es idéntica o casi en el 96–100 % de las tareas; Pokémon Snap
(124 tareas de escenas 3D), F-Zero X, Conker, DKR, Jet Force Gemini y Perfect Dark
son lo que queda por mirar. La HLE no deja ningún opcode sin implementar en las
escenas corridas (`unknown_cmds` = 0). Con `SALSA64_GCHK_SELF=1` la HLE contra sí
misma da 100 % idéntica (prueba del método). El host se compila con
`-DGFX_CENSUS` (ganchos de la HLE: `GFX_UNKNOWN`, histograma del RDP; no entran
en la PS3).

## 2026-10-17 — S2DEX suelto: Resident Evil 2 y Yoshi's Story

Las tareas gráficas con el texto «RSP Gfx ucode S2DEX» (Resident Evil 2: «fifo
2.06»; Yoshi's Story: «S2DEX  1.06», sin «fifo») no tenían HLE: el log las
contaba como «microcódigo desconocido» y la tarea terminaba sin dibujar. Ahora
`src/rsp/gfx/s2dex.c` las interpreta en el host, el PPE y el SPE 0 (partición
por familias, [spe0-familias.md](spe0-familias.md)).

- **Antes.** Resident Evil 2 (`docs/pads/re2-intro.pad`, 3000 campos): 1821
  tareas con microcódigo desconocido y 608 gráficas; con 9000 campos, 14 281 tareas
  S2DEX. Yoshi's Story (START y A cada 100 campos desde el 400, 6000 campos):
  3370 tareas S2DEX con microcódigo desconocido, ninguna dibujada (0 tareas
  gráficas en el resumen). **Después:** Resident Evil 2, 7042 tareas gráficas en
  3000 campos con 2895 fondos y 0 comandos sin implementar; Yoshi's Story, 3370
  tareas con 105 756 objetos y fondos y 0 comandos sin implementar.
- **Dos microcódigos, una familia.** S2DEX 2.x usa los opcodes de F3DEX2 (los
  mismos del tramo de Zelda OoT y Kirby 64) y S2DEX 1.x los de la primera
  generación (BG_1CYC 0x01, BG_COPY 0x02, OBJ_RECTANGLE 0x03, OBJ_SPRITE 0x04,
  OBJ_MOVEMEM 0x05, SELECT_DL 0xB0, OBJ_RENDERMODE 0xB1, OBJ_RECTANGLE_R 0xB2,
  LOADTXTR 0xC1 y LDTX_* 0xC2–0xC4); `s2_canon` traduce los de 1.x y el resto del
  código es el mismo. `SALSA64_GBI_S2DEX` (2.x, ya existía) y
  `SALSA64_GBI_S2DEX1` (nuevo, al final del enum); la versión sale del primer
  «N.» después del nombre (`core/rsp.c`). `salsa64_gfx_backend.s2dex` dice si el
  backend las hace; si no, en el modo `auto` de la LLE van al microcódigo real
  y si no terminan sin dibujar, como hasta hoy.
- **Una tarea S2DEX carga el 3D a mitad de lista.** Las dos hacen un
  `G_LOAD_UCODE` a F3DEX (RE2: F3DEX 2.06; Yoshi's Story: F3DEX 1.23) y siguen
  con la misma lista; de ahí vuelven a S2DEX para los sprites y terminan dentro
  de su tramo. `s2dex_run` recarga el microcódigo y devuelve el mando al
  intérprete de F3DEX2 o de F3D (con la bandera `chain_ex`, que lo trata como
  F3DEX 1.x); los intérpretes de F3D y F3DEX2 llaman de vuelta a `s2dex.c` para el
  tramo. Antes de esto la tarea terminaba en el `G_LOAD_UCODE` sin
  `G_RDPFULLSYNC` y el juego se quedaba sin la interrupción del DP.
- **Comandos.** G_OBJ_LOADTXTR y G_BG_COPY/1CYC ya estaban; nuevos:
  G_OBJ_MOVEMEM (uObjMtx de 24 bytes con `(w0 & 0xFFFF) = 0` y uObjSubMtx de 8
  con `2`; la matriz vive en `g->raw[4..11]`, libre en el tramo),
  G_OBJ_SPRITE, G_OBJ_LDTX_SPRITE/RECT/RECT_R, G_OBJ_RECTANGLE y
  G_OBJ_RECTANGLE_R. G_SELECT_DL no se implementa (ningún juego de los
  respaldos lo usa; cuenta como sin implementar).
- **G_OBJ_SPRITE son dos triángulos, no un TEXRECT.** Con el RDP de la LLE a la
  vista (seguimiento de los comandos que llegan por DPC_START/END, que no queda
  en el árbol) se ve que el microcódigo lleva las cuatro esquinas por la matriz
  (x' = A x + B y + X, y' = C x + D y + Y), con el segundo vértice **un píxel
  antes** (esquinas inclusivas: un sprite de 16 × 16 va de x0 a x0 + 15), y lo
  manda al RDP como dos triángulos con textura (0xCA) con s y t de 0 al mismo
  tamaño en texels (volteados si los flags lo piden o si A o D son negativas;
  Yoshi's Story usa D = −A). Un TEXRECT [x0, x0 + w) daba 1947 píxeles distintos
  en una pantalla de sprites; armar el mismo cuadrilátero con
  `gfx_rdp_tex_quad` (nuevo, en `rdp.c`, sale por el mismo camino que los
  triángulos del RDP) da **los mismos píxeles que el microcódigo real** en todos
  los sprites. Los de G_OBJ_RECTANGLE y G_OBJ_RECTANGLE_R van con TEXRECT y **no se
  pudieron medir**: ni Resident Evil 2 (en lo que alcanza el host) ni Yoshi's Story
  los usan.
- **Fondos con imageX cerca del borde (sin resolver).** Un G_BG_1CYC de Yoshi's
  Story trae imageX = 335,4 en una imagen de 336 de ancho y la LLE dibuja de un
  tirón desde s = 7,4 (335,4 − 328: la carga empieza en el múltiplo de 8
  texels anterior y lo que sobra del ancho sale de la fila siguiente, sin dar la
  vuelta); `bg_draw` da la vuelta en la imagen, como manda el manual, y parte la tira
  en dos. La imagen de esa escena es negra y no se nota; queda anotado.
- **Resident Evil 2.** En los primeros 13 500 campos del host (con `re2-intro.pad`
  y con START y A alternados) las 14 281 tareas S2DEX solo traen G_BG_COPY
  (8895; el resto son limpiezas del fondo): copian a la pantalla los cuadros del
  video, en tiras de 512 × 150 y 512 × 327 desde 0x806D4000, 0x8021FA00 y
  0x802B5A00. De ahí no pasa: el host se queda en esa pantalla desde el campo
  1500 (las imágenes cada 500 campos hasta el 13 500 son la misma), así que **no se llegó a los menús ni
  a los objetos** de Resident Evil 2. Contra el microcódigo real
  (`--state-save 1500:` y `--stop-gfx 3|6|9|12:` con y sin `--lle=all`) la
  imagen de la HLE sale idéntica: **0 de 76 800 píxeles distintos**. Que esas
  tiras salgan desordenadas (en la HLE y en la LLE por igual) sugiere que el
  búfer de video se lee con otro ancho del que se escribió; sin resolver y sin
  relación con S2DEX.
- **Yoshi's Story.** Desde el estado del campo 2500 (la pantalla de resultados
  «Beginning Treasure Hunt»), la imagen de la HLE contra `--lle=all` después de las
  listas 3, 8, 20, 40, 520 y 600: 955, 1063, 1680, 1960, 3600 y 3849 píxeles
  distintos de 76 800 (819, 520, 766, 745, 1442 y 1625 con diferencia de más de
  16 en un canal). Los sprites (los puntos del borde, el pájaro, los números)
  salen **idénticos**; lo que falta es el 3D de la misma lista: el modelo de
  Yoshi, las monedas y la manzana, más una cuña de 30 × 15 píxeles que solo dibuja
  la LLE. Es el camino de F3DEX 1.x, no el de S2DEX, y no se investigó si es de la
  recarga a mitad de tarea o ya estaba.
- **SPE 0.** Con la partición por familias el S2DEX suelto es la parte
  `GFX_PART_S2DEX` (`rsp/gfx/spe0.h`) y va en dos imágenes: la 0 (F3D, F3DEX 1.x
  y los de Rare; Yoshi's Story mezcla en una tarea S2DEX 1.x y F3DEX 1.23) y la
  1 (F3DEX2, F3DZEX2, S2DEX2 y F3DFLX; Resident Evil 2 mezcla S2DEX 2.x y
  F3DEX2 entre tareas). `gfx_spe0_image`: `SALSA64_GBI_S2DEX1` → 0 y
  `SALSA64_GBI_S2DEX` → 1. Sin esa partición no cabía: con todo en un programa,
  texto 120 056 (+9280 sobre el de entonces), holgura de pila −7552 (mínimo
  1536). Ahora: imagen 0 texto 103 688, bss 145 808, pila 11 616 libres, cadena
  de 9872 (`gfx_dl_f3d` → `gfx_dl_s2dex1` → `s2dex_run` → `obj_*` → `gfx_texrect`
  → …), **holgura 1744** (la más justa; mínimo 1536); imagen 1 texto 98 464,
  holgura 7200; imagen 2 (Conker) texto 91 608, holgura 14 784. La aritmética de
  64 bits de los objetos (`obj_rect`, `obj_rectangle`) se cambió a coma
  flotante: el SPE no lleva la división de 64 bits (libgcc, ~5 KB). Una tarea
  S2DEX que carga un microcódigo de otra imagen (S2DEX 1.x → F3DEX2, o 2.x →
  F3D) no sigue en él (`GFX_HAS`); ninguno de los dos juegos lo hace.
  `gfx_selftest_build` no tiene tarea S2DEX (la prueba de arranque compara el SPE
  con el PPE solo para las demás familias).
- **Pruebas.** Unitarias (`tools/build.sh test-host`): gfx 50 (3 nuevas: tarea
  S2DEX 2.x con LOADTXTR y OBJ_RECTANGLE, tarea S2DEX 1.x con la matriz y el
  sprite en dos triángulos —con D negativa—, y la tarea S2DEX 2.x que carga
  F3DEX2 a mitad de lista) y rsp 13 (detección de 1.x y 2.x), todas sin fallos;
  el resto de las suites sigue en 0 fallidas. Regresión: Zelda OoT (campo 900 y la
  casa nueva de `docs/pads/zelda-casa-nueva.pad`), Kirby 64, Banjo, DK64,
  GoldenEye, Mario Kart 64 y Perfect Dark con `tools/escenas-host.sh --comparar`
  contra el ejecutor anterior: **8 imágenes idénticas** (md5 de la casa
  `5f67040f…` y de Kirby `4efd365c…`, iguales antes y después); con la partición
  también salen iguales Yoshi's Story con la imagen 0 sola y Resident Evil 2 con
  la 1 sola (`-DGFX_SPE0_IMG=<n>`). `tools/test-roms.sh` rc 0 (nemu64-test 433
  fallas, 136 con `--lle`).
- **Paquete de prueba** «Salsa64 S2DEX» (SLSA00145, v0.2.7-40-g68444ad):
  `build/salsa64-s2dex.pkg`, sha256
  `58f82e100585bbdcb7bce359de1525fc984f3c7090633059f8fb4b1ad5414817`, armado con
  `tools/build.sh -C src/frontend PROGRAM=salsa64-s2dex "TITLE=Salsa64 S2DEX"
  APPID=SLSA00145 pkg` (el directorio de build borrado antes). Para probar en la PS3:
  abrir Resident Evil 2 (los logos, la intro y el video, ahora con las copias a la
  pantalla que hace S2DEX) y Yoshi's Story (la intro, el título, la selección de
  archivo y un nivel; antes no dibujaba nada); mirar en el log las líneas de
  tareas gráficas (0 «con microcódigo desconocido», 0 comandos sin implementar),
  que el SPE 0 cargue la imagen F3D (Yoshi's Story) o F3DEX2 (Resident Evil 2) y
  los campos por segundo. En RPCS3, con `spe0-todas`, las tres imágenes arrancan y
  sus tareas de prueba salen iguales a las del PPE (gfxtest; RPCS3 no corre estos
  juegos).
- **Falta.** Probarlo en la PS3 y llegar a los menús de Resident Evil 2 (el host no pasa de las
  cinemáticas): ahí estarán los G_OBJ_RECTANGLE y los demás objetos.
## 2026-10-17 — El SPE 0 por familias de microcódigos: tres imágenes

El programa del SPE 0 estaba a 250 592 de 262 144 bytes, con 1840 de holgura
de pila (mínimo 1536) y la caché de texturas bajada a 768 entradas. Ahora se
enlaza tres veces desde las mismas fuentes (`-DGFX_SPE0_IMG=<n>`,
`src/rsp/gfx/spe0.h`) y el PPE carga la imagen de la familia de cada tarea.
Todo en `docs/spe0-familias.md` (inventario, mezcla por juego, costos y la
regla para agregar un microcódigo).

- **Partición** (con los datos: ningún respaldo mezcla generaciones entre
  tareas; S2DEX2 y F3DFLX solo dentro de F3DEX2; RE2 mezcla F3DEX2 y S2DEX
  suelto): 0 «F3D» (F3D, F3DEX 1.x, F3DWR, PD, DKR/JFG/Mickey), 1 «F3DEX2»
  (F3DEX2, F3DZEX2, S2DEX2 y F3DFLX en la tarea) y 2 «Conker». Hasta `_end`
  238 992 / 239 632 / 238 352 bytes, holgura 14 080 / 13 024 / 14 784, las
  tres con la **caché de texturas en 1024**.
- **Hueco:** S2DEX suelto en la imagen 1: 11,5 KB sobre el mínimo (+3,5 KB
  por cada 128 entradas de caché que se quiten); Factor 5 en una imagen
  propia: ~21 KB. `s2dex.c` pasa sus cuentas de los fondos a 32 bits (eran
  5,3 KB de libgcc).
- **Cambio de imagen** por tarea (`need_image`, `gfx_spu.c`): el tramo del RDP
  y las color images pasan por la memoria principal, la caché de texturas
  queda vacía con una barrera hasta que el consumidor termina la última tarea
  de la imagen anterior (`gfx_tc_fence`), y cada imagen se comprueba contra el
  PPE al cargarse (hash y tareas de prueba de `src/rsp/gfx/selftest.c`). En
  RPCS3, 3,4 ms; en la práctica, un cambio por juego al empezar.
- **Host:** `salsa64-run` cuenta las tareas por microcódigo y los cambios de
  imagen que habría; compilado con una sola imagen, da las mismas imágenes que
  main en sus juegos (212 imágenes de 14 escenas iguales a main).
- **RPCS3:** con la pila libre llena de un patrón (para medirla), su
  recompilador de SPU ejecutaba mal la imagen de F3D en algunos builds (bien
  con su intérprete): la pila solo se llena con el archivo `spe0-pila`.
- **Pruebas:** `tools/build.sh test-host` 613/613; `tools/test-roms.sh` rc 0;
  `tools/build.sh` rc 0; `tools/test-rec.sh` rc 1 solo por `menu_scan_dir`
  bajo qemu (nemu64 433, fastmem 440); RPCS3: `spe0-todas`, gfxtest y sus
  variantes F3D y F3DEX, vitest, audiotest y el menú en verde.
- **Paquete de prueba** «Salsa64 SPE0» (SLSA00146, v0.2.7-32-gb3f2dc2):
  `tools/build.sh -C src/frontend PROGRAM=salsa64-spe0 "TITLE=Salsa64 SPE0"
  APPID=SLSA00146 pkg` con el directorio de build borrado antes.
- **Falta:** la PS3 real con el paquete «Salsa64 SPE0» (SLSA00146): qué imagen
  carga cada juego, el tiempo de la carga y de un cambio (`spe0-todas` hace
  los tres al arrancar) y el tiempo del SPE 0 por tarea con la caché en 1024.

## 2026-10-17 — Jet Force Gemini y Mickey's Speedway: G_DMA_TEX_OFFSET (0x02) y desplazamientos de textura

La candidata 0.2.8-rc1 dejaba a los dos juegos con comandos sin implementar
(746 y 2548 en 2500 campos, el último con código 0x02) y, en el host, el
semáforo de la salida de Mickey's Speedway no cambiaba de luz (en la PS3 el
autor lo vio igual, con la carrera arrancando). Era el mismo comando.

- **Qué es.** El microcódigo de Rare de los dos juegos (F3DJFG, el de Diddy Kong
  Racing con otro conteo de vértices) trae un comando más, 0x02, que ni
  Glide64mk2 (lo deja reservado) ni Rice (sin entrada) implementan y que
  GLideN64 aproxima (solo formato RGBA, un índice por carga a la TMEM). Su
  semántica sale de **seguir el microcódigo real en el host** (`--lle=all` con
  un seguimiento de instrucciones temporal, que no queda en el árbol): el
  comando trae por DMA la tabla que apunta w1 (el largo va en w0: 0x50 en
  todas las listas vistas), copia **64 bytes = 32 entradas u16** a su DMEM,
  enciende un interruptor y pone en cero un índice; cada G_SETTIMG que
  sigue, **de cualquier formato**, suma a su dirección (antes de resolver el segmento) la entrada del índice y
  lo avanza; **G_DMA_OFFSETS (0xBF) apaga el interruptor** (cada objeto del
  juego empieza con un 0xBF y casi siempre un 0x02). El juego cambia las
  tablas de un cuadro a otro para animar texturas: las luces del semáforo de
  la salida son cuadros de 0x0B00 bytes (0x0B00, 0x1600, 0x2100 y 0x2C00 en
  la entrada 0 de la tabla de ese objeto, en el orden en que las luces se
  encienden). En Jet Force Gemini hay tablas con entradas distintas de
  cero en varias escenas de la intro (cambian detalles de textura: de 0,01 a
  0,8 % de los píxeles en 17 de 105 imágenes).
- **Qué se hizo.** `src/rsp/gfx/f3ddkr.c`: G_DMA_TEX_OFFSET copia la tabla al
  estado de DKR (en la unión con el de Conker: el tamaño de `gfx_rare` y con él
  el de los estados guardados no cambia); G_SETTIMG pasa por el gancho de la
  variante (suma la entrada y sigue con `gfx_rdp_command`); G_DMA_OFFSETS apaga
  el interruptor. Pruebas unitarias nuevas (`dkr_tex_offsets`): el índice por
  G_SETTIMG de cualquier formato, la suma antes del segmento, G_DMA_OFFSETS,
  otro 0x02 que reinicia, la entrada 33 que no existe y la lista sin 0x02.
  Lo demás que se revisó del microcódigo de JFG: el conteo de vértices (sin
  sumar 1) coincide con el del microcódigo (alfas de abajo) y la matriz con
  multiplicación da las mismas imágenes que él; no hizo falta tocarlos.
- **Comandos sin implementar, antes → ahora** (`salsa64-run`, respaldos
  propios): Jet Force Gemini 746 → 0 en 2500 campos (el título) y
  27 981 → 0 en una partida nueva hasta la selección de personaje
  (`docs/pads/jfg-3d.pad`, 14 000 campos, 16 787 tareas, 0 errores);
  Mickey's Speedway 2548 → 0 en 2500 campos y 3242 → 0 desde el cuarto hasta
  el principio de la carrera (`docs/pads/mickey-semaforo.pad`, campos 5600 a
  12 000, 3170 tareas, 0 errores).
- **Contra el microcódigo real** (`--lle=all --lle-speed 8`, desde estados
  guardados del juego, mismos campos):
  - los **G_SETTIMG que el microcódigo manda al RDP** son iguales a los de la
    HLE, uno por uno: 26 029 de Mickey's Speedway (8 estados, 22 con
    desplazamiento distinto de cero) y 13 550 de Jet Force Gemini (8 estados,
    42 con desplazamiento);
  - los **alfas de los vértices** de cada G_VTX (leídos del búfer de vértices
    del RSP) coinciden: sin niebla 100 %; con niebla ±1 (la HLE redondea;
    p. ej. Jet Force Gemini, campo 7600: 6759 vértices, 2030 iguales sin
    niebla, 4046 con niebla iguales y 683 con diferencia de 1);
    de paso comprueba el conteo de vértices de F3DJFG;
  - la **imagen del VI** al mismo campo: Jet Force Gemini 0,96 %, 0,09 % y
    3,07 % de píxeles distintos (> 16/255 tras tolerar 1 px) en tres escenas
    (el puente de mando, una escena oscura con dos personajes y la
    selección de Juno), y 45,6 % en la selva: ahí la
    LLE del host no aplica la **niebla** del blender (`build_state` solo la
    pone en los triángulos de la HLE) y con la niebla apagada en la HLE las
    dos imágenes quedan a 1,0 %. Mickey's Speedway: 3,7 % en la tarea del
    semáforo en verde (era 4,8 % sin el comando; en las filas del
    semáforo, 6,5 % → 3,5 %) y 7,6–8,9 % en carrera (el HUD y los textos,
    que la LLE del host no dibuja);
  - **semáforo de la salida de Mickey's Speedway** (mismo guion desde el
    arranque, imágenes cada 40 campos): sin el comando las tres luces
    siguen apagadas hasta el verde de la carrera; con él pasan por rojo, rojo
    rojo, rojo rojo rojo y verde, como en la imagen del microcódigo real.
    Contra ares no se comparó: el semáforo exige pulsaciones del juego y el
    título y la demo, lo único que ares alcanza sin entrada, no usan tablas
    con entradas distintas de cero.
- **Pulsaciones nuevas:** `docs/pads/mickey-semaforo.pad` (de «Press start» a
  la parrilla y las luces) y `docs/pads/jfg-3d.pad` (partida nueva y la intro
  en 3D); en `docs/pads/README.md`.
- **SPE 0 (`spu_gfx`)**: texto 111 152, datos 800, bss 138 640 bytes
  (250 592 de 262 144; eran 250 216); pila: la cadena más honda sigue en 9520
  bytes y la holgura queda en **1840** (mínimo 1536; era 2224). Sin tocar
  la caché de texturas ni nada más.
- **Pruebas:** `tools/build.sh test-host` 603 pruebas, 0 fallidas (la nueva,
  `dkr_tex_offsets`); `tools/test-roms.sh` rc 0 (nemu64-test en 433 fallas, 136
  con `--lle`, vitest, savetest, gfxtest en sus tres variantes, estados y
  audiotest en verde); `tools/test-rec.sh` rc 1 solo por `menu_scan_dir` bajo
  qemu (como en main: no puede hacer enlaces duros en la carpeta compartida),
  con las 603 pruebas unitarias en ppc64 en los cinco modos (plain, far, pin,
  eager y fpures), nemu64-test con el recompilador en 433 fallas (pin+eager
  también) y con fastmem en 440, vitest, estados y perfilado en verde.
- **Paquete de prueba** «Salsa64 DKR» (SLSA00141), armado sobre el commit de
  estas notas con `tools/build.sh -C src/frontend PROGRAM=salsa64-dkr
  "TITLE=Salsa64 DKR" APPID=SLSA00141 pkg` (el directorio de build borrado
  antes; en el menú, la pausa y la primera línea del log sale su versión).
- **Sin cambios en los demás juegos**: Perfect Dark y Conker (3000 campos),
  GoldenEye (Dam, `docs/pads/goldeneye-dam.pad`, 6000) y Diddy Kong Racing
  (`docs/pads/dkr-pista.pad`, 10 000 campos y las imágenes de 6500, 8500 y
  9500) dan el mismo `--gfx-frame` (md5) con la rama y sin ella, y Diddy Kong
  Racing sigue en 0 comandos sin implementar.
- **Falta**: probar en la PS3 real el paquete «Salsa64 DKR» (SLSA00141):
  Mickey's Speedway hasta la salida de una carrera (las luces) y Jet Force
  Gemini hasta la intro en 3D y el primer nivel; tiempo del SPE 0 por tarea
  (el estado de DKR es el mismo; el comando nuevo es una DMA de 64 bytes y
  una suma por G_SETTIMG). Pendiente aparte, que no es del microcódigo: la
  niebla en la LLE del host.

## 2026-10-17 — Resident Evil 2: HLE del video de las cinemáticas (RESIZE, DECODE y FILL)

Hasta hoy el juego corría en la PS3 a ~32 campos/s con audio de MusyX y el log
contaba tareas gráficas con microcódigo sin HLE: eran las tres tareas de video
de sus cinemáticas, que terminaban sin hacer nada. Ahora las hace la HLE
(`src/rsp/jpeg/re2.c`, en el SPE del JPEG) y cada tarea sale **idéntica byte a
byte** al microcódigo real corriendo en el intérprete del RSP del host. Falta
probarlo en la PS3 (paquete «Salsa64 RE2», SLSA00144, abajo).

- **Qué son.** Tres microcódigos pequeños, sin texto de identificación, en
  tareas de tipo gráfico (`type` 1, `flags` 4, `data_ptr` en 0). Los
  parámetros van en `ucode_data`: un segmento de datos de 128 bytes que el
  juego rellena cada vez (parámetros en 0x00–0x27 y las constantes de los
  vectores del microcódigo en 0x40–0x6F). RESIZE reescala un cuadro RGB de 320
  de ancho (3 bytes por píxel) a RGBA5551 en bilineal: 436 × 382 en 6 franjas
  de 63–64 filas, 320 × 240 en franjas de 40 y 596 × 436 en franjas de
  72–73. DECODE convierte planos Y, Cb y Cr (4:2:0, 240 × 120) a RGBA8888 en un
  búfer de 264 píxeles de ancho (2112 bytes por par de filas) y FILL promedia
  dos de esos cuadros (el cruce entre dos escenas). El VI enseña esos búferes
  directamente, en modo de 32 bits.
- **Detección** como mupen64plus-rsp-hle (`try_normal_task_detection`, commit
  `8a7a472a`): suma de los 256 primeros bytes del código (`0x450f`, `0x3b44` y
  `0x3d84`), solo si la tarea no es de un microcódigo gráfico ni de audio
  conocido. Entra en `core/rsp.c` junto a JPEG (mismo backend; estadísticas
  aparte, `stats.video*`). En el modo LLE `auto` lo que tiene HLE se queda en
  la HLE; una tarea con parámetros o constantes que no se midieron
  (`salsa64_re2_supported`: constantes del segmento de datos distintas, anchos o
  disposiciones fuera de lo medido) cuenta en `video_unsup` y va al
  microcódigo real o termina sin hacer nada, como antes.
- **La aritmética de mupen64plus no sirve.** Se portó primero tal cual y se
  comparó: 46 % de los bytes distintos en RESIZE, 1,6 % en DECODE y 1,5 % en
  FILL. Lo que hace el microcódigo (medido con el LLE y sus volcados; nada
  del microcódigo se copió al repositorio, `docs/THIRD_PARTY.md`):
  - *FILL*: `(s + d) >> 1` en los **cuatro** bytes (mupen deja el cuarto en 0).
  - *DECODE*: cada canal es `⌊Y · 128 · K4 / 2^16⌋ ± ⌊(C · 128 − 0x4000) · K / 2^16⌋`
    con piso en cada producto, recortado a 0..0x7F80 y `>> 7`; las constantes K
    (Q16: 0xB375, 0x5B69, 0x2C0D, 0xE2D1, 0x950B) las trae el juego en su
    segmento de datos. Es entero (mupen usa dobles con constantes de 9
    decimales). El cuarto byte (alfa) el microcódigo no lo escribe: queda lo que
    hay en su DMEM de salida, que son ceros salvo en las **28 primeras
    posiciones de la primera fila de cada par**: el DMA de la croma Cr (trae 240
    bytes desde 0x280 y pasa de 0x300, donde empieza esa fila) deja ahí sus bytes
    131, 135, …; la segunda fila queda en cero. Se reproduce exacto (la HLE solo
    toma anchos múltiplos de 16 hasta 240, que es el del juego).
  - *RESIZE*: pesos de 15 bits (`⌊x_frac / 2⌋` y `⌊(y_frac / 2) mod 2^15⌋`, con la
    parte fraccionaria de `y0 + n · y_ratio`: la fila de origen es su parte
    entera, no solo la de `src_offset` como en mupen), los cuatro productos
    `⌊wx · wy / 2^16⌋` por 4, y cada canal suma los cuatro `⌊byte · peso / 2^16⌋`
    (piso por producto) más un redondeo antes de recortar a 255 y quedarse con
    5 bits. El redondeo es **4** si el umbral del difuminado del segmento de
    datos es 0 (las franjas de 436 y de 320 de ancho) y, con umbral > 0 (las de
    596), un difuminado entre vecinos: cada carril de un grupo de 8 píxeles
    acumula su suma con el redondeo anterior; la acumulada del carril de al lado
    se compara con la suma del píxel en los tres canales y, si queda a menos del
    umbral en los tres, el redondeo es esa acumulada `& 7`; si no, 4. El estado
    sigue entre grupos y entre filas y arranca con ceros y con dos palabras de la
    tarea (`dram_stack` y `dram_stack_size`: en la DMEM del RSP están justo
    donde el microcódigo guarda el último carril). El microcódigo trae dos
    filas de origen a la DMEM (1920 bytes) y escribe la fila de salida desde
    0x780: para el **último píxel de una fila de 436** el vecino de abajo a la
    derecha cae en esa fila de salida (en su píxel 0) y se lee tal como está; la
    HLE lleva una DMEM de 4 KB igual para reproducirlo.
- **Cómo se midió.** `salsa64-run --lle-check-jpeg` ahora detecta también estas
  tareas: las manda al microcódigo real, corre la HLE sobre una copia de la RDRAM
  y, al terminar el microcódigo, compara byte a byte lo que escribió cada unidad
  (fila, par de filas o pasada). `SALSA64_JCHECK_VERBOSE=1` lista cada tarea (y el
  segmento de datos de las que la HLE no acepta), `SALSA64_JCHECK_ALL=1` todas las
  diferencias y `SALSA64_RE2_DUMP=archivo[:N]` vuelca las primeras N de cada tipo
  (parámetros, entrada, salida del microcódigo y de la HLE, y su código; es del
  juego: queda fuera del repositorio). `SALSA64_RE2_FUZZ=semilla` cambia antes de
  cada tarea sus parámetros a valores al azar dentro de lo que la HLE acepta (anchos
  múltiplos de 4 o de 16, pasos y desplazamientos en x y en y, umbral del difuminado,
  constantes del color, cero de la croma, largos y saltos de las filas de FILL y las
  palabras `dram_stack` y `dram_stack_size`), para comparar mucho más que las
  geometrías del juego. Con 41 491 tuplas Y-Cb-Cr medidas se
  ajustó el color de DECODE; las constantes y el flujo de cada microcódigo se
  leyeron de su código desensamblado, solo para entender su aritmética.
- **Resultado** (sin pulsaciones, 24 000 campos = 400 s de consola, 125 s de
  corrida; la intro entera, con los logos y el video de entrada):

  | Tarea | Tareas | Unidades | Bytes comparados | Bytes distintos |
  |---|---:|---:|---:|---:|
  | RESIZE | 210 | 11 958 filas | 11 374 128 | **0** |
  | DECODE | 1566 | 93 960 pares de filas | 189 423 360 | **0** |
  | FILL | 1560 | 93 600 pasadas | 188 697 600 | **0** |

  Con `docs/pads/re2-intro.pad` (START y A, hasta el campo 9000): RESIZE 24, DECODE
  11 y FILL 10 tareas, también 0. Con los parámetros al azar (`SALSA64_RE2_FUZZ`,
  semilla 1 con 1500 campos y semillas 2 a 4 con 5000): 144 tareas de RESIZE, 934 de
  DECODE y 929 de FILL, 0 bytes distintos. **Regresión del JPEG** (mismo oráculo): Ocarina
  of Time con `docs/pads/zelda-casa-nueva.pad` (150 tareas, 600 macrobloques,
  153 600 píxeles), Pokémon Stadium con `docs/pads/stadium-vs.pad` (900 tareas,
  230 400 píxeles) y Pokémon Stadium 2 con el mismo guion (1500 tareas, 384 000
  píxeles): **0 píxeles distintos** en los tres.
- **Dónde corre y por qué.** En el SPE del JPEG (`src/spu/jpeg`, otro tipo de trabajo
  del mismo trabajador de spukit; `src/platform/ps3/jpeg_spu.c`, con el PPE de
  respaldo para lo que el SPE devuelve). Costo medido en el host (Apple Silicon,
  clang -O2, sin sanitizadores; es lo que cuesta cuando no hay backend y la
  tarea corre en el hilo del CPU):

  | Tarea | Costo por tarea | Por unidad de trabajo |
  |---|---:|---:|
  | RESIZE (63 filas × 436 píxeles, 27 468 píxeles) | 0,34–0,38 ms | ~13 ns por píxel |
  | DECODE (60 pares, 28 800 píxeles) | 0,088–0,100 ms | ~3 ns por píxel |
  | FILL (120 filas de 960 bytes) | 0,012–0,014 ms | 0,1 ns por byte |

  Un cuadro de video (a unos 15 cuadros por segundo: un DECODE y un FILL cada ~4
  campos) cuesta ~0,11 ms en el host y los logos, 6 franjas de RESIZE por cuadro,
  ~2,1 ms. El PPE es un núcleo en orden y en código escalar de enteros suele ser
  bastante más lento que este host (estimado, no medido): un cuadro de video serían
  del orden de 1 ms del hilo de emulación cada 4 campos y una franja de RESIZE de
  varios ms, y ese hilo ya es el cuello de botella de los juegos pesados. El SPE del
  JPEG está libre (solo trabaja al cargar una sala de Zelda o una imagen de
  Stadium) y la tarea es por lotes: un trabajo por tarea completa, doble búfer de
  DMA, sin diálogo fino. Por conteo de instrucciones del código SPU compilado (los
  bucles de `salsa64_re2_resize_row` y `salsa64_re2_decode_pair`) salen ~1,3–2 ms
  por franja de RESIZE y ~0,4–0,6 ms por tarea de DECODE (**estimados, no medidos
  en la PS3**); el RSP real gasta 873 000 instrucciones del microcódigo por
  franja de 436 (14 ms a 62,5 MHz; 1 356 300 en las de 596), así que el SPE va
  unas 7 veces más rápido sin tocar el hilo de emulación. La PS3 dirá lo medido:
  el log trae «video: N tareas (…) en el SPE: x, y y z ms (la más larga …)» cada 150
  tareas, y sin SPE (el archivo `jpeg-ppe` o si no arranca) corre en el PPE y da su
  costo.
  - *Host, antes y después* (`salsa64-run`, 6000 campos = 100 s de consola, sin
    sanitizadores): sin `--lle`, 24,86 s antes (241 campos/s; el video no se hacía)
    y 24,24 s después (248 campos/s con 845 tareas de video, 56,6 ms en total en
    el hilo del CPU); con `--lle` (S2DEX por el microcódigo real), 29,08 s antes
    (206 campos/s; el video corría en el microcódigo: 402 M de instrucciones del
    RSP) y 26,86 s después (223 campos/s; el RSP a bajo nivel baja a 180 M).
- **Local store del SPE del JPEG** (`spu-size`): texto 29 792, datos 800 y bss
  32 144 bytes (antes 10 880, 800 y 5 008): ~27 KB de búferes del video (la
  DMEM de RESIZE de 4 KB, filas de entrada y de salida con doble búfer, los
  planos de DECODE y las filas de FILL), 62,7 KB en total y ~195 KB libres para la
  pila (la cadena más honda del video, `handler` → `run_resize` →
  `salsa64_re2_resize_row`, usa menos de 1 KB). Sin divisiones de 64 bits en el SPE.
- **DMA.** Las direcciones de la RDRAM vienen alineadas a 8 pero no a 16 (las
  filas de 872 bytes de RESIZE alternan): la entrada se trae con el tramo
  alineado a 16 que la contiene y la salida se arma con el mismo resto módulo 16
  que su dirección y se pone con DMA de 8 bytes en los extremos y de 16 · n en el
  centro (`dma_put8`). FILL usa 16 bytes por instrucción (`spu_and`, `spu_xor`,
  `spu_rlmask`) si origen y destino están alineados, como en el juego.
- **Pruebas.** Unitarias: 609 (602 y la suite `re2`: reconocimiento, CRC de las
  tareas de prueba del arranque, FILL exacto con filas sin alinear y alineadas,
  DECODE contra tuplas medidas con el microcódigo real y el alfa, RESIZE con
  blanco, negro y gris, parámetros absurdos con ASan y UBSan, y la integración
  en el RSP con y sin backend). 0 fallidas en Docker (gcc arm64, ASan y UBSan) y la suite `re2` también en ppc64
  big-endian con qemu, en los cinco modos de `tools/test-rec.sh` (plain, far, pin, eager
  y fpures; el modo pin pasó de los 600 s con la máquina cargada y se repitió con
  `QEMU_TIMEOUT=3000`), con el mismo CRC de las tareas de prueba que el host.
  `tools/test-roms.sh`: nemu64-test en 433 fallas (136 con `--lle`), vitest,
  savetest, gfxtest en sus tres variantes, estados y audiotest en verde.
  `tools/test-rec.sh`: nemu64-test con el recompilador en 433 (con pin y eager
  también) y con fastmem en 440, vitest, estados y perfilado en verde; sale con
  rc=1 por `menu_scan_dir` bajo qemu (enlaces duros en la carpeta compartida) y, en
  el modo pin, esa suite aborta con «free(): invalid pointer» también corrida sola
  (`SALSA64_TEST=menu`), sin nada de video. `tools/build.sh` compila todo sin
  avisos. El arranque del frontend compara el
  SPE con el PPE sobre una RDRAM de mentira (`rsp/jpeg/selftest.c`: cuatro
  tareas con salidas alineadas a 8 y no a 16, y el difuminado); en RPCS3:
  «JPEG: tareas de video de prueba en el SPE: iguales al PPE y al host (SPE
  AE4B7E46, PPE AE4B7E46, host AE4B7E46)».
- **Paquete de prueba** «Salsa64 RE2» (SLSA00144, v0.2.7-24-g02dea98): `build/salsa64-re2.pkg`,
  sha256 `9d2273e5d0ffe0727545a9031a43ca3b0ac16620a8b0409568cfeabeb3d748f6`.
  Para probar en la PS3: abrir Resident Evil 2 y ver la intro: los logos y el
  video de entrada deben verse (con el cruce entre escenas) en vez de negro; mirar
  en el log «JPEG: tareas de video de prueba en el SPE: iguales…» al arrancar y las
  líneas «video: …» cada 150 tareas (ms por tipo en el SPE, la más larga, tareas
  devueltas al PPE, espera del PPE, campo más lento y «sin HLE» en 0); si dice
  DIFIEREN, el juego usa el PPE.
- **Falta.** Probar el video en la PS3 real. El juego en sí (menús, fondos y
  escenas) usa tareas S2DEX sueltas (`RSP Gfx ucode S2DEX fifo 2.06`, tipo 1) que
  no tienen HLE: en el host, sin `--lle`, 14 062 tareas gráficas sin HLE en 8000
  campos frente a 1090 de video (el log de la PS3 las seguirá contando). Las
  cinemáticas se ven; el resto del juego no hasta que S2DEX suelto tenga HLE.

## 2026-10-17 — Candidata 0.2.8-rc1: integración de Killer Instinct Gold, el censo de audio y Diddy Kong Racing

Sobre la versión 0.2.7 con MusyX y Diddy Kong Racing se unieron las ramas de
Killer Instinct Gold (listas del CPU al RDP en el SPE de gráficos, cola de
interrupciones del DP, detector de cuelgues) y del censo de audio y JPEG contra
el microcódigo real. Los choques fueron de texto (opciones de `run.c`, pruebas
de `test_rsp.c`, estas notas y la compatibilidad); no hubo cambios de
comportamiento que se pisaran: Killer Instinct solo tocó `rsp/gfx/dpc.c` del
SPE 0 y el censo solo `rsp/audio` y `rsp/jpeg`.

- **SPE 0 (`spu_gfx`)**: texto 110 776, datos 800, bss 138 640 bytes
  (250 216 de 262 144); pila: la cadena más honda usa 9520 bytes y la holgura
  queda en **2224** (mínimo 1536), igual que con Diddy Kong Racing solo: las
  listas del CPU las corre `spu_dpc_run` con el código de siempre del SPE.
- **SPE de audio (`spu_audio`)**: texto 62 288, datos 800, bss 35 088 bytes
  (98 176 de 262 144); la cadena más honda que se puede seguir usa 8448 bytes
  (`aud_run` llama por tabla a los comandos, que no se siguen).
- **SPE de JPEG**: texto 10 880, datos 800, bss 5008 bytes.
- **Pruebas:** 602 unitarias en el host (little-endian, 0 fallidas) y 602 en
  ppc64 con qemu en cinco modos (plain, far, pin, eager y fpures; 0 fallidas
  salvo `menu_scan_dir` bajo qemu, que no puede hacer enlaces duros en la
  carpeta compartida; el modo pin tarda ~11,5 min y pasa del tiempo de 600 s
  de `qemu-ppc64.sh` si hay otra carga); `tools/test-roms.sh` con nemu64-test
  en 433 fallas (136 con `--lle`), vitest, savetest, gfxtest en sus tres
  variantes, estados y audiotest en verde; `tools/test-rec.sh`: nemu64-test
  con el recompilador en 433 (pin+eager también) y con fastmem en 440, vitest,
  estados y perfilado en verde; `tools/build.sh` compila todo.
- **En el host (`salsa64-run`):** el oráculo de audio (`--lle=all
  --lle-check-audio`, 5000 millones de ciclos) da 0 tareas distintas de 3171 en
  Super Mario 64 y de 3164 en Mario Kart 64, y 1 de 3190 en Kirby 64 (la tarea
  2094, la misma con la rama del censo sola, sin la integración). Diddy Kong
  Racing con `docs/pads/dkr-pista.pad` llega a la carrera con 0 comandos sin
  implementar en 10 000 campos; Killer Instinct Gold con
  `docs/pads/killerinstinct-pelea.pad` (`--pads 2 --pad-port 2`) llega a la
  pelea y a «CONTINUE?» con 3171 listas del CPU y 0 comandos sin implementar.
  Jet Force Gemini y Mickey's Speedway USA, con la variante de Rare, dibujan sus
  títulos en 2500 campos pero con comandos sin implementar (746 y 2548, el
  último con código 0x02).

## 2026-10-17 — Diddy Kong Racing: HLE del microcódigo de Rare (F3DDKR)

Hasta hoy el juego daba pantalla negra («sin HLE de su microcódigo gráfico»).
Ahora se ve y se juega en el host; falta probarlo en la PS3.

- **El microcódigo.** Fast3D de Rare sin texto de identificación y con
  `ucode_size` en 0 en su tarea (el RSP carga 4 KB): se reconoce por el CRC-32
  del código (`3BFF208D`; `gbi_by_crc` toma el tamaño 0 como 4096) o, para
  versiones que no se han podido medir (otra de DKR, Jet Force Gemini y
  Mickey's Speedway USA), por el CRC con las palabras al revés que usa la
  lista de GLideN64 (`salsa64_crc32_words_swapped`). La geometría va por DMA:
  G_MTX de una matriz ya con la proyección a una de cuatro ranuras, G_VTX de
  vértices de 10 bytes, G_DMATRI de triángulos de 16 bytes con las
  coordenadas de textura en el triángulo, G_DL_IN_MEM con n comandos y
  vuelta sola, carteles (billboards) y G_DMA_OFFSETS; el resto es Fast3D
  (`src/rsp/gfx/f3ddkr.c`, ~2,9 KB en el SPE 0). Lo de cada comando en el
  comentario del archivo y las fuentes en `docs/THIRD_PARTY.md` (Glide64mk2
  y Rice, GPLv2 o posterior; GLideN64, solo consulta: el resultado es GPLv2
  o posterior, sin cambiar la licencia del binario). El G_TEXTURE del juego
  llega con escala 0: las coordenadas del triángulo van tal cual (/32). El
  comando 0x02 (desplazamientos de textura, que Glide64 usa para animar el
  agua) no aparece en los 30 000 campos recorridos (0 comandos sin
  implementar) y no se implementó. F3DJFG, sin respaldo, solo cambia el
  conteo de vértices (no suma 1) y la matriz con multiplicación (bit 23), como
  en GLideN64.
- **Pila del SPE 0** (`tools/spu-stack.py` en el build):

  | | texto | datos | bss | libres | cadena más honda | holgura |
  |---|---:|---:|---:|---:|---:|---:|
  | main a104e00 | 107 864 | 800 | 142 224 | 11 104 | 9 520 | 1 584 |
  | con F3DDKR sola | 110 896 | 800 | 142 224 | 8 032 | 9 520 | −1 488 (el enlace falla) |
  | con F3DDKR y 768 entradas en la caché de texturas | 110 776 | 800 | 138 640 | 11 744 | 9 520 | **2 224** |

  El código nuevo (+2,9 KB con `-Os`) se paga bajando `GFX_TC_ENTRIES` de 896
  a 768 (−3 584 bytes del directorio de la caché de texturas; el máximo de
  texturas distintas en 100 campos medido en el host es 428). La cadena más
  honda sigue siendo la del fondo S2DEX; la de DKR (`gfx_dl_f3d` →
  `dkr_command` → `gfx_triangle`) es más corta. El estado de DKR comparte
  lugar con el de Conker en `gfx_rare` (una unión: el tamaño de la
  estructura fija dónde empieza el estado del RDP de los estados guardados) y
  las matrices usan las primeras ranuras de `gfx_state.mv`. En el host, con 768
  y con 896 entradas: GoldenEye (Dam, 7000 campos) 0,2 texturas decodificadas
  por tarea y 0,1 desalojadas en las dos; Perfect Dark (6000) 0,3 y 0,1 contra
  0,0; Conker (6000) 0,1 y 0,0 en las dos; DKR (30 000 campos de carrera) 0,1
  y 0,0. Pendiente en la PS3: tiempo del SPE 0 por tarea.
- **Cómo llegar a una carrera** (nuevo `docs/pads/dkr-pista.pad`): desde el
  arranque, la carrera de Ancient Lake: «Get ready» hacia el campo 7800 y la
  pista desde el 8000.
  `--state-save`, `--pad` y `--gfx-frame-every` bastan; las imágenes y los
  estados quedan en `build/` (nada del juego va al repositorio).
- **Herramienta nueva: `salsa64-run --stop-gfx N:ARCHIVO`.** Termina al acabar
  la N-ésima tarea gráfica y guarda en PPM la última color image que dibujó,
  con la HLE o con `--lle=all` (cuenta tareas que arrancan: `rsp.stats.lle_gfx`).
  Por campos el VI enseña con la LLE imágenes a medio dibujar y el juego, al
  tardar el RSP real más que la HLE, avanza distinto; con `--lle-speed 8` la
  LLE termina antes que el campo y el juego va al mismo paso que con la HLE.
- **Contra el microcódigo real** (`--lle=all --lle-speed 8`, mismo guion,
  desde el arranque, imágenes cada 250 campos): 0,01 / 0,03 / 0,10 / 0,07 /
  0,36 / 0,43 / 0,44 % de píxeles distintos tras tolerar 1 px (diferencia por
  canal > 16/255) en los campos 250, 500, 750, 1000, 1250, 1500 y 1750; en la
  isla de Timber (modo Adventure, campos 11 500 a 16 000 cada 250, jugando)
  0,5–2,5 %; en lo demás va de 0,14 a 34 % porque la LLE del host no dibuja
  los textos y el HUD de DKR (el logo y «START / OPTIONS» del título, «8th»,
  la vuelta y el tiempo de la carrera: en la HLE y en la consola sí salen;
  por qué no salen con el RSP real queda pendiente, no es de la HLE) y por el
  movimiento. Los colores de una franja de suelo del título (media de 10 × 50
  píxeles) coinciden dentro del 1–3 % entre la LLE y la HLE, y el alfa de
  los vértices que da el microcódigo real es 0 en 1486 triángulos del título
  y 255 en 43 (con niebla, la del juego casi no actúa; sin ella, el alfa del
  vértice): lo mismo que hace la HLE, aunque no se contó triángulo a
  triángulo.
- **Contra ares** (v148; volcados de la RDRAM por GDB al primer escrito del
  CPU a cada display list, que da la lista y su color image completas y
  coherentes: `tools/ares-volcar-lista.py`, docs/referencia-ares.md; el volcado a un instante cualquiera sale con la lista de dos
  cuadros después y no sirve). `salsa64-run --ares`, PSNR en promedios de 4×4 y
  porcentaje de píxeles a más de 8/255:

  | Escena | volcado | PSNR | píxeles distintos |
  |---|---|---:|---:|
  | Logo de N64 | x-09 | 31,0 dB | 5,6 % |
  | «Copyright Rare 1997» (tarjeta de lado) | x-28 | 41,6 dB | 1,2 % |
  | Intro: Diddy volando | x-22 | 33,9 dB | 9,9 % |
  | Presentación de Pipsy (demo en pista) | y-10 | 27,2 dB | 12,3 % |
  | Presentación de Wizpig (demo en pista) | y-25 | 27,2 dB | 19,2 % |
  | Presentación de Tiptup (cueva) | y-00 | 26,9 dB | 39,7 % |
  | Título (arco de la meta) | y-36 | 24,4 dB | 41,4 % |
  | Título (árboles, karts) | y-39 | 25,2 dB | 33,0 % |

  Todos los volcados coherentes de las dos ráfagas (sin los negros): `x`
  (logos e intro de los primeros 30 s; 32 volcados) mediana de 35,6 dB
  (31,0–41,6) y 3,0 % de los píxeles a más de 8/255 (0–9,9 %); `y`
  (presentaciones de personajes, título y demos; 25 volcados) mediana de
  27,4 dB (20,5–35,0) y 21,6 % (6,3–65 %). Parecido a lo que dan los demás
  juegos con esta herramienta (31–38 dB) en la primera y por debajo en la
  segunda. Lo que queda: bordes de los polígonos con AA (la consola
  escribe los píxeles cubiertos en parte) y detalle de las texturas (el
  rasterizador del host filtra bilineal y muestrea en el centro), y en
  las escenas con suelo grande del título y de las demos (arena, pista) el
  suelo sale más claro en la HLE que en la consola (~9 % en una franja
  del título: 240 contra 219) sin explicación: la LLE del host da los mismos
  colores que la HLE y la niebla no la explica (probar sin niebla baja el
  PSNR medio de las 57 parejas de 32,2 a 30,8 dB). Es del RDP o del rasterizador, no del
  microcódigo; con una carrera real en ares se podría acotar (el título y las
  demos son lo único que ares alcanza sin entrada).
- **Pruebas de esta rama**: `tools/build.sh test-host` 584 pruebas, 0 fallidas
  (nuevas: las de `tests/unit/test_gfx_rare.c` para matrices, vértices,
  triángulos, listas contadas, carteles y F3DJFG, y la detección por CRC en
  `tests/unit/test_rsp.c`); `tools/test-roms.sh` rc 0; `tools/test-rec.sh` rc 1
  solo por `menu_scan_dir` bajo qemu (como en main), con nemu64-test en 433
  fallas, con fastmem en 440, vitest OK, estados y perfilado OK; `tools/build.sh`
  (todo) compila. Sin cambios en los demás juegos: Perfect Dark, Conker,
  GoldenEye, Mario Kart 64, OoT, Banjo-Kazooie, Smash y Kirby 64 dan la misma
  imagen (md5 del `--gfx-frame` a 2500–3000 campos) con main a104e00 y con
  esta rama.

## 2026-10-17 — Killer Instinct Gold en la PS3: el RDP del CPU, un solo estado y el aviso de cuelgue

Killer Instinct Gold (NKIE) arrancaba en la PS3 con el audio bien, 60 campos/s y
**ninguna tarea gráfica** («cierre: HLE gráfica suelta (0 tareas gráficas)»),
en la v0.2.5, la v0.2.6 y la v0.2.7, con el intérprete y con el recompilador,
con y sin Controller Pak. En el host dibujaba. Solo dibujó en la PS3 con el RSP
a bajo nivel encendido en modo `auto`, aunque ninguna tarea llegara al
microcódigo real (en RPCS3: «rsp lle: 0 tareas» y 1399 tareas gráficas por la
HLE).

### 1. Las listas del RDP que escribe el CPU no tenían quien las consumiera

- **Causa.** El juego no dibuja todo con el RSP: cada campo escribe él mismo
  listas del RDP en la RDRAM y se las da por `DPC_START`/`DPC_END` (el fondo
  del escenario y los marcos de la pantalla: de 7 a más de 60 comandos cada
  una), sin tarea del RSP; y **no manda la primera tarea gráfica hasta recibir la
  interrupción del DP de ese `SYNC_FULL`**. Esa interrupción la levanta quien
  consume la lista (`bus->rsp.dpc`, `sp.c: dp_run`); sin backend la lista solo
  se descarta y la interrupción no llega nunca. El frontend de la PS3
  conectaba el backend del RDP (`salsa64_rsp_set_dpc`) solo cuando había RSP
  a bajo nivel; el host siempre.
- **Evidencia (host, `salsa64-run`).** En 300 campos KI manda 274 tareas
  gráficas (Fast3D «RSP SW Version: 2.0D», `ucode_size` 0) y 292 de audio
  (ABI1), y escribe 137 listas al RDP; la primera tarea gráfica sale en el
  campo 27, 11 341 ciclos después de arrancar una de audio. Con
  `SALSA64_SIN_DPC=1` (no conecta el backend, como la PS3): 0 listas, **0
  tareas gráficas**. Con el backend y `SALSA64_DP_CYCLES=4000000000` (la
  interrupción del DP casi no llega): 1 lista y 1 tarea gráfica. O sea, el
  juego espera la interrupción del DP de cada lista del CPU.
- **Arreglo.** El backend del RDP se conecta siempre (`src/frontend/main.c`).

### 2. Las dos HLE se pisaban la caché de texturas

La primera versión del arreglo daba las listas del CPU a un RDP en el PPE
(`gfx_ppe`, otro `gfx_state`) mientras las tareas seguían en el SPE. Pero la
caché de texturas decodificadas es **una región de memoria con un directorio
por cada estado**: dos estados sobre la misma región reservan desde el mismo
lugar y se pisan las texturas. Medido en el host con
`SALSA64_DPC_ESTADO_APARTE=1` (otra HLE con su propio estado para las listas
del CPU, como la PS3): la pelea de Killer Instinct Gold sale con el suelo, el
helicóptero y el cielo rotos, y con un solo estado (lo normal en el host) sale
bien; los fondos del escenario, que son de esas listas, eran lo que fallaba
en la PS3 (el cielo negro del volcado). Ahora **las listas del CPU las corre el
mismo SPE que las tareas** (`gfx_spu.c: spu_dpc_run`): el mismo estado del RDP
y la misma caché, síncrono (la lista es corta y el CPU espera su interrupción
del DP), con la tarea en vuelo recogida antes (`salsa64_rsp_quiesce`) para que
sus salidas lleguen al RSX en el orden en que se dieron. Con el RSP a bajo
nivel (cientos de tramos por tarea) el RDP sigue en el PPE (`gfx_ppe`): con la
HLE gráfica en el SPE y el modo `auto`, esas dos HLE siguen pisándose. Las
listas que el CPU deja en la DMEM (xbus, que el SPE no lee) también van al
PPE.

### 3. Otros hallazgos en el camino

- **Los comandos crudos del RDP se reconocen por sus 6 bits bajos.** Las listas
  de KI traen `0x2F` (en vez de `0xEF`: SET_OTHER_MODES, incluida la
  conmutación FILL ↔ 1 ciclo), `0xBA` (SET_PRIM_COLOR, `0xFA`) y `0x24`
  (TEXRECT): `gfx_dl_dpc` los comparaba con el byte entero y los descartaba en
  silencio. Ahora `w0 |= 0xC0000000` antes de despachar (`src/rsp/gfx/dpc.c`,
  sin crecer el SPE: la holgura de pila sigue en 1584 bytes, mínimo 1536).
- **Interrupciones del DP en cola.** La ranura del planificador es una sola:
  un segundo SYNC_FULL antes de que llegara el primero (la lista del CPU y la
  tarea que le sigue) borraba al primero y el juego esperaba una interrupción
  que no llegaba. Ahora se encolan en orden (hasta 4) y cada una se levanta a su
  hora; el mismo SYNC_FULL pedido dos veces en el mismo ciclo sigue siendo
  uno. En el host, con los tiempos de siempre, no ocurría nunca.
- **Un trabajo del SPE con error ya no cuelga al juego.** Antes se ponían a 0
  sus banderas: sin SYNC_FULL ni corte no hay interrupción del DP y el juego
  espera al RDP para siempre. Ahora termina como cortado (con su interrupción)
  y el log dice «el SPE devolvió el error N» (las primeras 4).

### 4. El congelamiento de la pelea (sin resolver)

Con el paquete de esta primera versión, KI dibuja y la **primera pelea va
bien; al empezar la segunda (Orchid contra Fulgore, «READY») la imagen se
congela con la música sonando**; con el RSP a bajo nivel en todas las tareas
(en una versión de prueba de su recompilador) pasa en la primera pelea. En los
dos logs el juego deja de leer los controles (0 lecturas por segundo) y de
mandar tareas y listas, el CPU queda en el bucle de espera de libultra (`b .`),
el audio y el VI siguen. Todos los hilos del juego esperan algo que no llega.

- **No lo reproduje en el host** (el reloj de la consola, sin pulsaciones
  humanas): la pelea completa con el guion
  [docs/pads/killerinstinct-pelea.pad](pads/README.md) pasa sin esperas con la
  HLE y con `--lle=all`; tampoco con la duración de las tareas barrida (tarea
  gráfica 30 000 a 1 200 000 ciclos, de audio 8 000 a 600 000, 0 a 8 sondeos de
  retraso, `SALSA64_JITTER` al azar, interrupción del DP más lenta o con
  `SALSA64_GFX_LATE`), ni con la HLE leyendo la RDRAM al final de la tarea
  (en la pelea, ninguna tarea cambia lo que lee mientras corre).
- **Lo que sí sabemos de los dos logs:** ocurre unos 50 campos después de
  «READY»; en los dos hubo antes esperas largas de la emulación por un juego de
  búferes del RSX (196 ms; 239 a 1281 ms en el otro) y en el otro tuvo muchas
  más sin congelarse, así que no es el disparador. En el log de la HLE faltaba
  el latido «sin gráficos»: el latido de `main.c` vigila los hilos del emulador,
  no al juego (el «sin gráficos» es de la rama del RSP a bajo nivel).
- **Lo que se añadió para verlo la próxima vez** (`src/frontend/cuelgue.c`): el
  latido avisa cuando un juego que leía los controles y dibujaba deja de
  hacer las dos cosas 6 s con el VI andando. Escribe al log el estado del RCP y
  del CPU (SP/DP/MI/VI, eventos pendientes, contadores de interrupciones del
  SP y del DP programadas y levantadas, tareas sin SYNC_FULL, el código
  alrededor del pc) y a `/dev_hdd0/salsa64/` un volcado de la RDRAM
  (`cuelgue-<campo>.rdram`, que lee `tools/libultra-threads.py`: en qué espera
  cada hilo de libultra) y otro de texto con los registros
  (`cuelgue-<campo>.txt`). Hasta 3 por juego.

### 5. El cielo de la pelea

El volcado del cuadro congelado (Orchid contra Fulgore) no trae la ciudad en
llamas que se espera detrás. Con el host y la HLE: el fondo es una lista del
CPU (las 63 órdenes del dibujo del campo 2059, que sola dibuja las columnas y
el cielo naranja de arriba) que la tarea gráfica del cuadro cubre en parte, y
la ciudad se ve en unos cuadros del mismo escenario en el host (variando
cuándo se elige al personaje) y en otros no, así que su ausencia en un
cuadro suelto no prueba un fallo. Con la HLE y el estado aparte de arriba salía rota. No pude comparar el
mismo cuadro con ares ni mupen64plus (el escenario es el del personaje que
elige el jugador y no logré que mupen eligiera a Orchid).

### Pruebas

Pruebas unitarias (+9): `rsp_dpc_from_cpu`, `rsp_dp_interrupts_queue` y la
suite `cuelgue`. RPCS3 (intérprete, sin recompilador, sin archivo `lle`): antes
0 tareas gráficas en 1800 campos; ahora 1409 (y más de 1250 listas del CPU en el SPE), con el logo y las fichas de los
personajes bien dibujados y las listas del CPU en el SPE
(«gfx: N listas del CPU al RDP hechas en el SPE»). `salsa64-run`: «RDP desde
el CPU» en el resumen, `--pads N`, `--pad-port N`, `SALSA64_SIN_DPC`,
`SALSA64_DPC_ESTADO_APARTE` y `SALSA64_JITTER`; `tools/rpcs3-run.py --quitar
NOMBRE` quita un archivo de `dev_hdd0/salsa64` (p. ej. `lle`) durante la
corrida; `tools/m64p-ref.sh` con `M64P_PAD_PORT`.

## 2026-10-17 — Audio y JPEG contra el microcódigo real: 99,87 % de las tareas idénticas (antes 13,1 %)

El oráculo `--lle=all --lle-check-audio` (docs/rsp-lle.md) se usó como censo de
la HLE de audio: 51 136 tareas de 21 juegos, byte por byte. La HLE, que venía de
mupen64plus-rsp-hle y truncaba donde el RSP redondea, quedó idéntica al
microcódigo salvo en 65 tareas (carreras con el CPU del juego en los bucles
de ADPCM de Perfect Dark, Smash y Stadium, y unas pocas sin explicar). Se
corrigieron la mezcla, el cambio de tono, el ENVMIXER (estado de 80 bytes,
rampas, fases y la variante de Conker), el FILTER, el IIR y el POLEF de naudio
y el MP3 (el MP3ADDY: cada comando trae el anillo de la ventana de la RDRAM y lo
devuelve; sin eso la música de Conker salía distinta). La tarea JPEG de Zelda OoT
y Pokémon Stadium 1 y 2 usa ahora la IDCT y el color del microcódigo: 0 píxeles
distintos de 1 075 200 (antes ~1 %). Detalle, método y lo que queda en
**docs/audio-censo.md**. Falta en la PS3: escuchar (Conker, Perfect Dark,
Banjo-Tooie, Majora's Mask, Yoshi's Story, 1080°) y medir el JPEG del SPE (la
IDCT cambió de forma). En memoria, el SPE de audio crece 3 KB de código y el de
JPEG baja 1 KB (docs/memory-budget.md).

## 2026-10-16 — HLE de audio de MusyX (Factor 5)

Hydro Thunder, Rush 2049, NBA Showtime, Gauntlet Legends, Tarzan, The World
Is Not Enough, Rugrats in Paris, Polaris SnoCross, Resident Evil 2 y Rogue
Squadron (issue #11) usan MusyX v1; Indiana Jones y Battle for Naboo, v2.
Hasta hoy sus tareas de audio llegaban a la HLE, no reconocían el formato
(no es una lista de comandos) y terminaban con un error sin escribir nada: los
juegos corrían mudos.

- **Qué hay.** `src/rsp/audio/musyx.c` (puerto de mupen64plus-rsp-hle,
  commit `8a7a472a`, registrado en `docs/THIRD_PARTY.md`): v1 y v2, mismo
  código en el host, el PPE y el SPE 1. Una tarea de MusyX trae en `data_ptr`
  una lista de subtramas (SFD) y en `data_size` cuántas son; cada una mezcla
  hasta 32 voces (PCM16 o ADPCM, cambio de tono, 4 envolventes) a L, R, cc0 y
  e50, aplica el eco (hasta 8 tomas de un búfer circular y un FIR de 4
  puntos) y escribe 192 pares L:R. El estado entre tareas vive en la RDRAM
  del juego, así que la HLE no guarda nada propio salvo la tabla de
  interpolación (520 bytes en `aud_state`). `src/core/rsp.c` no cambió: en
  modo `auto` del LLE, MusyX ya iba a la HLE (la condición es «ABI
  reconocido»); lo que faltaba era que la HLE hiciera algo.
- **Compilaciones.** Cada juego trae su propia compilación del microcódigo
  (CRC distinto). La de Rogue Squadron (primera palabra de su `ucode_data` =
  `0xF0000F00`, las demás `0x00010010`) lleva la envolvente del canal 3 (el
  envío al eco) en octavos; es el ABI `SALSA64_ABI_MUSYX1_RS` («MusyX v1
  (Rogue Squadron)»).
- **La tabla de interpolación** del cambio de tono (64 × 4 coeficientes) está
  en el `ucode_data` del propio microcódigo (en 0x20 o 0x28 según la
  compilación). La HLE la busca ahí, en la RDRAM del juego, y la reconoce por
  su estructura (la fila i es la 63−i al revés y suma 1,0); si no la
  encuentra, usa la propia de `lut.c`. Con la propia el sonido tiene otra
  respuesta en frecuencia y casi todas las tareas con voces difieren del
  microcódigo real (en 32 s: Hydro Thunder 1030 de 1911 tareas, Rush 2049
  1706 de 1899 —la mitad de sus muestras a más de 4 LSB—, NBA Showtime 39 de
  1911, RE2 609 de 2857, Rogue Squadron 431 de 942, Indiana Jones 1370 de
  1903); con la del juego, ninguna. No se copia nada del microcódigo al
  repositorio.

**Validación con el microcódigo real** (`salsa64-run --lle=all
--lle-check-audio`: cada tarea de audio corre también en el intérprete del
RSP y se compara, byte a byte, lo que escribió con lo que escribe la HLE
sobre una copia de la RDRAM; host, `-DAUD_MAX_RANGES=64`). 32 s de consola
(`-c 3000000000`) y 96 s con pulsaciones de START y A cada ~1 s para pasar los
menús (con solo START, 4198 subtramas de Rogue Squadron usaban el eco); el
audio de Rogue Squadron, RE2 e Indiana Jones se mide aunque sus gráficos no
tengan HLE:

| Juego (compilación) | 32 s | con START y A (96 s) |
|---|---:|---:|
| Hydro Thunder | 1911 de 1911 idénticas | 5740 de 5740 |
| Rush 2049 | 1899 de 1899 | 5728 de 5728 |
| NBA Showtime | 1911 de 1911 | 5740 de 5740 |
| Resident Evil 2 (v1) | 2857 de 2857 (9e9 ciclos) | 2856 de 2856 |
| Star Wars: Rogue Squadron (v1, RS) | 942 de 942 | 5713 de 5713 |
| Indiana Jones (v2) | 1903 de 1903 | 5675 de 5724; las otras 49, ±1 en L |

- Las 49 de Indiana Jones empiezan en la tarea 5624, con 12–15 voces y
  eco: la salida de L difiere en +1 en las muestras 0 y 1 de cada 8, sin
  que R ni cc0 cambien. No lo expliqué (ninguna voz sola reproduce ese
  patrón; no es `ptr_10`, `mask_14` ni `mask_15`, todos en cero); queda
  como +1 LSB en 2 de cada 8 muestras de L en esas tareas.
- **Redondeos** (lo que el original de mupen64plus trunca y el microcódigo
  real redondea), todos hallados con el oráculo y confirmados con el 100 %
  de las tareas: cada producto del cambio de tono se redondea por separado
  (+0x4000 >> 15) y se suman los cuatro, saturando solo el resultado;
  las envolventes `(s · env_hi + 0x4000) >> 15`; las tomas del eco igual;
  los coeficientes del FIR `(hgain · c + 0x4000) >> 15` y cada producto
  del FIR redondeado antes de sumar. La mezcla del eco en v2
  (`(v · ganancia) >> 16`) sí trunca: redondearla da 4387 tareas
  distintas de 5724. El decaimiento de los volúmenes base es
  `(x · 0xF850) >> 16`.
- **Rogue Squadron, canal 3.** La envolvente del eco avanza en octavos y de
  ocho en ocho muestras: la base del bloque sube 8 × (paso >> 13) y la
  muestra j del bloque suma (j × paso) >> 13. Hallado ajustando 188 rampas
  (36 096 muestras): 100 % de coincidencias; con el paso entero en 32 bits,
  31,6 %. Sin esto, Rogue Squadron daba 3833 de 5713 tareas distintas (1595
  con el paso cortado a octavos pero sin la estructura de bloques).
- **El oráculo** compara ahora cuando el RSP termina la tarea (antes, al
  empezar la siguiente: MusyX deja su estado en la memoria del juego, que el
  juego reutiliza entre medias, y salían diferencias falsas), cuenta las
  muestras de 16 bits distintas por tamaño de la diferencia, y con variables
  de entorno saca lo necesario para depurar: `SALSA64_LLE_VERBOSE=1` (una
  línea por tarea), `SALSA64_LLE_DIFFS=1` (cada palabra distinta, con el
  número de tarea), `SALSA64_LLE_DUMP_TASK=N` (lo escrito antes/LLE/HLE y las
  cabeceras de las subtramas) y `SALSA64_LLE_DUMP_DMEM=archivo` (la DMEM del
  RSP al terminar). Con más de 10 tramos escritos por tarea hay que compilar
  el host con `EXTRA_CFLAGS=-DAUD_MAX_RANGES=64` (el resultado de spukit solo
  admite 10).
- **SPE 1.** Programa `spu_audio`: text 46 232 → 59 296 bytes, data 800, bss
  34 448 → 34 960 (total 81 480 → 95 056 bytes de 262 144); la cadena de
  llamadas más honda (`tools/spu-stack.py`) usa 8,4 KB de pila y quedan 167 KB
  libres. Costo por tarea: la línea de cada 5 s del log, «audio: … en el SPE
  %.3f ms por tarea (máx. …), PPE esperando …», ya lo da también para MusyX
  (ABI «MusyX v1», «MusyX v1 (Rogue Squadron)» o «MusyX v2»; «comandos por
  tarea» cuenta voces mezcladas más subtramas).
- **Arranque en la PS3.** Las listas de prueba del arranque (SPE contra PPE
  sobre una RDRAM de mentira) incluyen ahora dos subtramas de MusyX v1 y v2
  con voces PCM16 y ADPCM, eco y direcciones sin alinear; el log dice
  «audio: listas de prueba en el SPE: iguales al PPE» o «DIFIEREN».
- **Paquete de prueba** «Salsa64 MusyX» (SLSA00140, construido sobre
  `7f95b8e`). En RPCS3 arranca con «audio: listas de prueba en el SPE:
  iguales al PPE» (incluye las dos listas de MusyX) y audiotest en verde.
- **Pruebas unitarias** (+5): reconocimiento de las tres compilaciones; una
  voz de PCM16 con el resultado calculado a mano (resampler, envolventes,
  volúmenes base y última muestra); el eco; la tabla tomada del segmento de
  datos del microcódigo (en 0x20, 0x28 y 0x138, y que una tabla rota no vale);
  datos absurdos sin salirse de los búferes (con ASan y UBSan); y las listas
  del arranque con un hash de la RDRAM como regresión.

Falta:
- [ ] En la PS3 real: oír Hydro Thunder, Rush 2049 y NBA Showtime; Rogue
      Squadron solo el audio (sus gráficos no tienen HLE todavía). Mirar en
      el log la línea «audio: …» (errores en 0, ms por tarea) y que las
      listas de prueba digan «iguales al PPE».
- [ ] Sin respaldo para probar: Gauntlet Legends, Tarzan, The World Is Not
      Enough, Rugrats in Paris, Polaris SnoCross, Battle for Naboo. Si una
      compilación trae su tabla de interpolación en otro lugar, usa la de
      `lut.c` (suena casi igual); si trae `ptr_10` (v2), se cuenta como
      comando sin implementar.
- [ ] Las 49 tareas de Indiana Jones (±1) y la mezcla final de v2 con
      `ptr_10`.
- [ ] Lo mismo para los demás ABI: leer la tabla de interpolación del
      `ucode_data` del juego en vez de la propia de `lut.c` (naudio y nead
      siguen con la propia; sus diferencias están en docs/estado.md,
      2026-10-10).

## 2026-10-16 — Candidato rc5: aviso de Stadium, carpetas de datos y Donald Duck

- **Pokémon Stadium, «Game Pak Check» al volver del título.** El aviso
  («Turn the N64 Control Deck OFF…», una imagen de 538 × 100) salía en
  ruido. Es una imagen auxiliar que el juego dibuja en una color image y
  usa de textura en los cuadros siguientes; con nueve destinos en una tarea
  la ranura que la guarda se recicla enseguida y la copia a la RDRAM se
  descartaba porque 538 × 2 con paso de 64 bytes por 100 filas son 108 800
  bytes y el espejo era de 64 KB (`la ranura 1 (2863c0/538) se recicla con lo
  dibujado hasta la fila 100: perdida`): los cuadros leían la RDRAM sin
  escribir. La sospecha del alto reservado (538 × 403) no era: la copia ya
  usaba las 100 filas dibujadas. Ahora cada búfer espejo es de 128 KB
  (`MIRROR_SLOT`; el bloque sigue en 3 MB por el redondeo) y la copia de una
  ranura que se recicla admite hasta eso; el espejo del final de la tarea y
  lo que cuenta como imagen chica siguen en 64 KB. En el host, con las
  pulsaciones del log de la PS3 y 1740 campos: 31 906 de las 32 000
  muestras del aviso (el rasterizador del host es de 320 columnas) pasan de
  ruido a la imagen y no cambia ni un byte de RDRAM fuera del aviso;
  `stadium-vs.pad` da la misma RDRAM y las mismas imágenes que antes.
- **Lista de ROMs:** la raíz ya no muestra las carpetas de datos propios
  (ver `docs/uso-ps3.md`); `juegos` solo si trae ROMs.
- **Guardado:** Donald Duck Quack Attack / Goin' Quackers (NDQ) sale de la
  lista de EEPROM 4K (guarda solo en el Controller Pak; Rush 2049 y Hydro
  Thunder ya no estaban en ninguna lista).

## 2026-10-16 — COP0 y MI fuera del camino lento: esperas por el planificador, CU1 = 0 y MTC0 Status en línea

Sesiones de la app de perfilado en la PS3 real: Doom 64 al 35 % (21
campos/s) con 0 % en bucles de espera, nativo 19 %, camino lento 35 %,
intérprete 30 %, despachador 14 %; COP0 75 M llamadas (13,9 s) y registro MI
37,5 M (9,3 s) en 125 s, casi todo desde 6 PCs. NFL Blitz, 1080, Episode I
Racer e Indiana Jones con el mismo patrón.

- **Qué era.** El hilo ocioso de Doom es `for (;;) osYieldThread();`: cada
  vuelta pasa por `__osDisableInt`, `__osEnqueueAndYield`, `__osDispatchThread`
  y `__osRestoreInt` (cuatro MTC0, un ERET, MI_INTR_MASK leído y escrito);
  las 6 PCs son esas. 1080 espera con `while (x->y == s0) osYieldThread();`
  en un hilo con FPU, Blitz con cinco `osRecvMesg(..., NOBLOCK)` y
  `osYieldThread`, Racer con pares `__osDisableInt`/`__osRestoreInt` sin
  cambio de hilo. El intérprete de Doom venía de otro lado: el hilo ocioso
  corre con CU1 = 0 y los bloques del cambio de hilo tienen `cfc1`/`sdc1`/
  `ldc1` en un camino que no se toma; la entrada revisaba CU1 y salía ~14 000
  veces por campo a interpretar (~160 000 instrucciones por campo).
- **Bucles de espera** (`idle.c`): la simulación de una vuelta sigue saltos,
  llamadas y ERET, con almacenamientos que tienen que volver a su valor,
  Status, EPC, el MI, MULT/DIV y la FPU de un cambio de contexto; el punto
  fijo es exacto como antes. Saltos consultados hasta 64 instrucciones hacia
  atrás (eran 8), tablas de 1024 con hash. En el host, con los 29 respaldos,
  1800 campos con y sin `--no-idle` dan la misma RDRAM y registros. Espera:
  Doom 0 → 83 %, Blitz 0 → 91 %, 1080 0 → 86 %, Indy 1 → 86 %, Racer
  21 → 37 %.
- **CU1 = 0**: con CU1 = 0 el bloque se compila cortado antes de su primer
  COP1. Doom (qemu): 163 228 → 78 instrucciones interpretadas por campo.
- **MTC0 Status en línea** cuando solo cambian IE, EXL o IM y no queda una
  interrupción pendiente; **MTC0 EPC** nativo. Racer sin saltar esperas
  (`--no-idle`, 600 campos): 25,5 M → 1 573 MTC0 Status en C.
- **Perfilado**: desglose de COP0 por instrucción y registro y del MI por
  registro (registro COP0, 18), y la tabla de PCs lentos de cada ventana
  propia (los 4 295 094 843 de la sesión de Doom eran restas negativas de
  dos tablas Space-Saving escritas como u32; el lector lee con signo las
  sesiones viejas).
- **`--lockstep`**: la referencia no recibía el Controller Pak (que va por
  omisión desde 9d1a9f3) y se separaba al consultar los controles (Doom en
  el ciclo 4,1 M, Blitz en el 3,1 M, también con main).

Desglose en el host (qemu, 600 campos, COP0 y MI que llegan a C):

| Juego | Antes (`--no-idle`, COP0 al intérprete) | Después |
|---|---|---|
| Doom 64 | MTC0 Status 12,9 M; MI_INTR_MASK 4,26 M escrito y 4,26 M leído; ERET 4,26 M; MTC0 EPC 4,26 M | MI_INTR_MASK 9 717 y 9 649; ERET 9 728; MTC0 Status 4 253 |
| NFL Blitz | MTC0 Status 23,1 M; MI_INTR_MASK 1,78 M y 1,78 M; MTC0 EPC 1,78 M; ERET 1,78 M | MI_INTR_MASK 14 050 y 13 941; ERET 14 055; MTC0 Status 4 641 |
| 1080 | MTC0 Status 11,2 M; MI_INTR_MASK 3,72 M y 3,72 M; ERET 3,67 M; MTC0 EPC 3,67 M | MI_INTR_MASK 89 721 y 89 723; ERET 41 843; MTC0 Status 24 919 |
| Episode I Racer | MTC0 Status 25,5 M; lo demás < 8 000 | MI_INTR_MASK 7 641 y 7 640; ERET 5 893; MTC0 Status 1 573 |
| Indiana Jones | MTC0 Status 12,6 M; MI_INTR_MASK 4,05 M y 4,05 M; ERET 4,05 M; MTC0 EPC 4,05 M | MI_INTR_MASK 21 074 y 21 066; ERET 21 203; MTC0 Status 18 246; MTC0 EntryHi 14 544 |

Pruebas: `tools/build.sh test-host` 573/0; `test-rec` con las mismas fallas
de nemu64-test (433, 440 con fastmem) y rc = 1 solo por `menu_scan_dir`;
`--lockstep` de Doom y Blitz, 600 campos, sin diferencias.

Lo que queda (MI_INTR_MASK y ERET, del cambio de hilo de verdad) son unas
decenas a 300 llamadas por campo: no vale el riesgo de leer y escribir la
máscara del MI en línea por ahora. Las escenas del host no son las de la
PS3 (no hay pad para llegar a las carreras de Racer ni a las partidas): la
medición de verdad es la PS3 con el paquete de perfilado nuevo.

Pendiente: medir en la PS3 con `salsa64-perfil-cop0.pkg` (Doom 64, Blitz,
1080, Racer, Indy) y comparar con las sesiones de build/perfil-fpu2 y
build/perfil-varios. Racer tiene además 180 000 llamadas de la FPU al
intérprete en 600 campos en el host (aparte).

## 2026-10-15 — Lista compacta de ROMs

La lista solo contiene la carpeta relativa visible, combinada entre el disco
interno y las ubicaciones USB, con un tope total de 4096 filas que incluye
ROMs, carpetas y `..`. Cada fila guarda metadatos, el índice de unidad y un
puntero al nombre en un pool compacto; la ruta completa se arma al abrir la
ROM. La reserva completa es de 1 261 844 bytes en la PS3 (cabecera de 276
bytes, 4096 filas de 52 bytes y pool de nombres de 1 048 576 bytes; véase
`docs/memory-budget.md`). Se libera antes de cargar el juego y se reconstruye
al volver al menú, conservando carpeta y selección.

La cifra corresponde a la reserva máxima, no a una medición de uso real del
pool. La lectura de directorios grandes se mide con las pruebas del host; no
se obtuvo una medición nueva de tiempo en la PS3 real.

## 2026-10-15 — Navegación de carpetas y lista ampliada

La descripción de capacidad que sigue corresponde al diseño inicial de esa
implementación y fue reemplazada por la lista compacta documentada arriba.

El menú puede entrar en subcarpetas de los directorios `salsa64`; muestra
primero las carpetas y `..` sube un nivel. ○ conserva la pregunta para salir
al XMB en la raíz. La lista fija admite 4096 ROMs y hasta 256 carpetas por
cada una de las nueve ubicaciones, además de la fila para subir. Opciones guarda en
`menu.cfg` el orden por título de cabecera o nombre de archivo. Si la fuente
no representa algún byte del título, se muestra el nombre del archivo. La
última ROM abierta conserva su ruta completa y al iniciar se abre su carpeta.

La lista reserva hasta aproximadamente 11,5 MiB de memoria estática; el
preparador estático de descargas agrega 6,1 MiB y la descarga copia otros
6,1 MiB temporalmente al heap. Se arma al abrir el menú o cambiar de carpeta
y no asigna memoria en el bucle de emulación. El log registra la duración de
cada lectura y el tamaño de la lista. Falta obtener una medición de tiempo
en una carpeta grande en la PS3 real.

## 2026-10-15 — Contadores por causa del camino lento

La app de perfilado registra tiempo y llamadas por clase del camino lento y
compilaciones por motivo en ventanas de 5 s; el resumen incorpora ambos
desgloses y compara sus totales. Los datos se guardan solo con `rec->trace`
activo. La app normal no añade instrucciones al código generado. En el host
se ejecutó GoldenEye durante 300 campos: 55,7 MIPS y 87,1 % en bucles de
espera; el recompilador no está disponible en este host, por lo que esa
corrida no produjo mediciones de las causas nuevas.

Pendiente: ejecutar con la app de perfilado en la PS3 real y revisar
Banjo-Tooie, GoldenEye y Perfect Dark. La medición no cuenta aciertos de
`ktlb[]` porque requeriría instrumentar accesos generados; el total de aciertos
no está disponible bajo el requisito de conservar intacto el código emitido.

## 2026-10-15 — v0.2.6 publicada

La rc1 (v0.2.5-117-g183a275, app normal SLSA00064) se probó en la PS3 real:
más de 15 juegos en varias corridas, entre ellos Conker y Super Mario Odyssey
64 abiertos después de otros (ROM fuera del heap), F-Zero X a 5× pasando
«SELECT COURSE» y en carrera, Zelda con los fondos de las salas
prerrenderizadas (149–150 fondos de S2DEX por ventana), Perfect Dark,
GoldenEye, Pokémon Snap, Kirby 64, Banjo-Tooie y Mario Kart 64 con la
interpolación; ROMs desde `/dev_hdd0/salsa64` y desde una USB. Ninguna
espera larga al RSX, ninguna ROM sin leer. Sale como v0.2.6 sin cambios de
código respecto a la rc1 (solo CHANGELOG, README y compatibilidad).

Queda para después: el fondo blanco de F-Zero X sobre la pista (sombra afín
del RDP tras el recorte, rama aparte), la caída de Super Mario Odyssey 64
(con su línea nueva del recompilador en el log), el camino lento de los
juegos de Rare (35–60 % del tiempo en las escenas pesadas de Banjo-Tooie) y
el JIT del RSP para los juegos sin HLE.

## 2026-10-14 — La interpolación cambiada en vivo: Mario Kart 64 sin música y Super Odyssey 64 caído (paquete «Salsa64 perfil», interp)

En la regresión de la v0.2.6 en la PS3 real («perfil 3» y «perfil 5») el
autor cambió la interpolación por geometría desde «Imagen» en el menú de
pausa. En Mario Kart 64 la encendió (con la escala a 3×) y después la apagó,
la encendió y la volvió a apagar (y 5×): al rato se quitó la música de fondo
y los efectos siguieron. En Super Odyssey 64 (hack de SM64 de 64 MB) la
encendió en el campo 2208: el muestreo pasó a «eventos» 69, 91 y 94,5 %
(antes 38 %), el audio se estiró 65–100 % con la emulación al 93 % y se oía
mal; en el campo 3269 se invalidaron 6758 de 6902 bloques y la emulación
quedó a 1,2 MIPS hasta salir con el botón PS.

- **Mario Kart 64: no es la interpolación, es el botón L del juego**, que
  enciende y apaga la música. El log dice que el juego leyó L cinco veces
  (campos 4134, 4256, 4302, 4338 y 4352, entre 5 y 9 s después de la segunda
  pausa): apagada. En el host, con las entradas del log como guion de
  `--pad` (`tools/pad-del-log.py`, nuevo) y sin interpolación ni pausas, el
  volumen de las voces de la música (los instrumentos con su libro ADPCM en
  0x3FFxxx) cae a 0 en el campo ~4250 y no vuelve hasta el 10 000; sin las L
  sigue en ~13 000. La L de la N64 es L1 del DualShock 3 (`padmap.c`).
- **Super Odyssey 64: lo que sí era de la interpolación, el costo en el SPE
  0.** Con la interpolación la HLE deja por cada G_VTX un registro
  `GFX_OP_VGROUP` (hash del contenido y coordenadas de recorte); en el SPE 0,
  con `-Os`, `gfx_vgroup` llamaba a `memcpy` por cada palabra del hash y por
  cada vértice (desensamblado del objeto SPU). Los vértices pasaron de 7,2 a
  12 ms por tarea y la tarea de 14 a 19–23 ms; el fin emulado de la tarea
  gráfica espera al SPE (`rsp.c`), así que el juego vio un RSP más lento: en
  la escena de su pausa (7400 triángulos) las tareas de audio bajaron de 60 a
  28 por segundo, la emulación a 55 campos/s y el audio se estiró. Los
  «eventos» del muestreo son la emulación durmiendo mientras espera al SPE
  (19 ms por tarea). Además esa escena no cabía en las tablas del
  emparejamiento («sin lugar en las tablas» en todas las tareas): todo ese
  trabajo era en vano.
- **Riesgo de cortes por la interpolación:** los registros compartían los
  768 KB del búfer de comandos con los dibujos y con la bandera la HLE tenía
  1,33 MB de vértices en vez de 2 MB. Una tarea pesada se podía cortar, y un
  corte adelanta la interrupción del DP y pierde el final de la lista. En la
  PS3 no pasó con Super Odyssey 64 («0 tareas cortadas»); en el host (512 KB)
  sí: 771 tareas cortadas y sin el texto de su pausa.
- **La caída de Super Odyssey 64 no se reproduce.** Firma (sesión del
  perfilado): el juego en su propia pausa (START en el campo 2655, sin más
  entradas); en el campo 3268–3269, 6758 bloques invalidados con 2 KB de DMA
  del cartucho y ninguna tarea del RSP terminada; el bloque del vector de
  excepciones (0x80000180) se recompiló con 11 instrucciones (antes 4, el
  salto a `__osException`; las 11 encajan con el comienzo de
  `__osException` copiado ahí) y desde entonces el CPU entra a ese bloque y
  sale por una excepción una y otra vez (camino lento 55 %, despachador
  33 %). Alguien reescribió el código del juego y el vector. No pasa en el
  host (intérprete) con las entradas del log hasta el campo 4500, ni con
  tareas gráficas de 1,4 a 12 millones de ciclos y de audio de 0,15 a 1,5
  millones (lo que tardan en la PS3, o mucho más), ni en RPCS3 (intérprete
  del CPU) con la interpolación encendida 1900 campos en la misma escena
  (estado del host en el campo 2700). Lo que queda distinto en la PS3 son el
  recompilador y los tiempos reales: queda separado de la interpolación,
  sin causa.
- **Arreglos:**
  - `gfx_vgroup` sin `memcpy`: lee el hash por bytes y asigna las cuatro
    coordenadas; el SPU lo compila sin llamadas.
  - `GFX_JOB_INTERP` solo cuando sirve (`interp_wanted`, `rdp.c`): no con el
    juego a 60 cuadros ni en las 90 tareas que siguen a una que no cupo
    (tablas, vértices o registros); después se vuelve a probar. El log de
    cada 5 s cuenta las tareas sin la bandera.
  - Los registros van en un tercio del búfer de comandos
    (`GFX_VGROUP_SHARE`), que el consumidor da de más: 384 KB sacados del
    arena de texturas (de 3,75 a 3,375 MB por juego de búferes); la HLE
    tiene siempre los 2 MB de vértices. Lo que no cabe deja la tarea sin
    interpolar (`GFX_RES_VGROUP_FULL`) y nunca la corta.
  - Diagnóstico: una invalidación de 1000 bloques o más en un campo va al
    log («recompilador: N bloques invalidados en el campo C: X por
    escrituras del CPU (zona), Y por DMA u otros (zona)»), para saber quién
    escribió si Super Odyssey 64 se vuelve a caer.
  - Pruebas: `interp-alternar` («N [E1 E2]»: cada N campos, una pausa como
    la del menú en la que se cambia la interpolación y la escala),
    `estados-prueba` «0 B» (solo cargar la ranura 9, p. ej. un estado del
    host), la salida de `capturas` reemplaza a `AUTO_FIELDS`, y el log de
    audio da los comandos por tarea. Ver `docs/interpolacion.md`, «Lo que la
    interpolación le cuesta a la emulación».
- **Verificado:** `test-host` 531/531 en Docker (prueba nueva
  `gfx_interp_budget`: tope de los registros, sin corte, el hash); las 7
  escenas del host idénticas byte a byte a 7fda50a (112 imágenes); gfxtest
  42/42 en RPCS3 (app normal y de perfilado); holgura de la pila del SPE 0
  2112 (era 2624: el código sin llamadas ocupa ~500 bytes más; mínimo 1536).
  En RPCS3, Mario Kart 64 desde un estado del host (campo 2100) con
  `interp-alternar` «200 3 5»: 11 cambios en vivo, las tareas de audio
  parejas y 333–383 comandos por tarea (como en el host), la música sigue;
  Super Odyssey 64 en su pausa con la interpolación: 109 de 110 tareas sin
  la bandera y sin caída. En el host, Super Odyssey 64 con `--interp`: 0
  tareas cortadas y el texto de la pausa.
- **Paquete:** «Salsa64 perfil» (SLSA00120, v0.2.5-104-g908e0c8),
  `salsa64-perfil-interp.pkg`, sha256 abbc101d…80583.
- **Falta validar en la PS3 real:** Super Odyssey 64 con la misma secuencia
  (la interpolación encendida desde la pausa y la pausa del juego con
  START): «gfx en el SPE por tarea: vértices …» con la interpolación cerca
  de sin ella, muchas «tareas sin pedir la identidad» en su pausa, 60
  campos/s y el audio sin estirar. Si se cae otra vez, mirar la línea
  «recompilador: … bloques invalidados …». En Zelda OoT y Mario 64, los
  intermedios como antes (Zelda ~40/s) y el SPE 0 con la interpolación
  encendida contra apagada. Mario Kart 64: nada que arreglar.
## 2026-10-14 — Zelda OoT sin fondos prerrenderizados: el S2DEX ya no se reconocía (paquete «Salsa64 perfil 6»)

En la PS3 («perfil 3», interpolación por geometría encendida) las salas
prerrenderizadas de OoT (del puente de la muralla al Mercado) salían con las
mallas en colores planos y sin fondo: las 96 ventanas del log dicen «0 fondos
de S2DEX» aunque se decodificaron 75 tareas JPEG.

- **Causa:** `gfx_ucode_kind` (2cdd521, F-Zero X) buscaba el texto «RSP Gfx
  ucode » antes del nombre, pero el S2DEX de OoT se identifica como «RSP
  Gfx[Safe] S2DEX fifo 2.05». Devolvía 0, el `G_LOAD_UCODE` se trataba como
  microcódigo ajeno y `gfx_dl_skip_foreign` se saltaba el tramo con el
  `G_BG_COPY`. La función anterior buscaba «S2DEX» en cualquier parte. Las 7
  escenas del host no lo vieron: la de Zelda es el título (campo 900), sin
  salas prerrenderizadas.
- **Arreglo** (`s2dex.c`): se busca «RSP Gfx» y se saltan espacios, marcas
  entre corchetes y «ucode» hasta el nombre. Sigue igual: F3DFLX.Rej, F3DEX2,
  F3DZEX y `.NoN` → 2; F3DEXBG, L3DEX y S2DEX 1.x → 0. Prueba
  `gfx_ucode_kind_texts` (23 textos, con y sin el prefijo de OoT; falla sin el
  arreglo en los de «[Safe]»).
- **Escena nueva** `tools/escenas-host.sh`: las 7 escenas, Kirby 64 y la
  **casa de Link** (`zelda-casa-nueva.pad`, partida nueva, campo 14500, sala
  prerrenderizada); falla si «casa» o Kirby dan 0 fondos de S2DEX. Con perfil 5
  la casa falla (0 fondos); con el arreglo da 624 y las demás salen igual.
- **Verificado** (host nativo, respaldos propios): casa de Link hasta el campo
  14800 con partida nueva: 724 fondos de S2DEX, y las 6 imágenes (campos 14300
  a 14800) **idénticas byte a byte** a la compilación de antes de 2cdd521
  (d217638); con perfil 5, 0 fondos y la sala en verde plano. Las 7 escenas y
  Kirby 64 (308 fondos) idénticas a perfil 5; F-Zero X en carrera (estado de
  la PS3, 1200 campos): 409 triángulos, 49 rectángulos y 52 dibujos por tarea,
  imágenes idénticas a perfil 5. `test-host` 531/531 en Docker; gfxtest 42/42
  en RPCS3 (app de perfilado); **holgura de la pila del SPE 0 2368** (perfil 5:
  2624; mínimo 1536).
- **Paquete:** «Salsa64 perfil 6» (SLSA00120, v0.2.5-102-g172235f),
  `salsa64-perfil-6.pkg`, sha256 718c7793…a81bf5.
- **Falta validar en la PS3 real:** el Mercado y la tienda Kokiri con el
  paquete (el log debe volver a traer «fondos de S2DEX» por ventana). La tienda
  Kokiri no se reprodujo en el host: su receta (`zelda-kokiri.pad` + `--poke`)
  necesita la partida del autor.

## 2026-10-14 — F-Zero X en carrera: el microcódigo F3DFLX (autos sin textura) y el fondo blanco junto a la pista (paquete «Salsa64 perfil 6b»)

En la PS3 («perfil 5», interpolación por geometría, Trinitron a 3×, render a
textura) el autor vio en carrera los autos lisos, sin textura, y un fondo
blanco que tapaba la pista. Dos volcados (SELECT + R2), la largada y en
carrera, dibujados en el rasterizador del host salen igual: las dos cosas
vienen de la HLE, no del RSX.

**Autos (arreglado).** F-Zero X pasa a mitad de cada lista de «F3DEX 2.03» a
«F3DFLX.Rej 2.03F», un microcódigo propio que la HLE trataba como F3DEX2.
Medido contra el microcódigo real corriendo en la LLE del host (desde un
estado de la carrera; el vector que deja en la DMEM y el índice de cada
vértice) y contra ares:

- Con G_TEXTURE_GEN, F3DFLX **no genera s y t**: quedan las del vértice. El
  auto lleva la escala 0xFFFF y con la texgen de F3DEX2 sus coordenadas
  iban a 0–1024 texels, fuera de la tile (limitada): textura lisa.
- En cambio el **alfa del vértice** sale de una tabla de 256 bytes que la
  lista sube con G_DMA_IO detrás de los vértices (DMEM 0x8B0; los datos de
  F3DFLX traen 64 vértices de 20 bytes de 0x3B0 a 0x8B0), indexada desde
  0x930 por el producto de la normal con la «luz del alfa»: G_MV_LIGHT en el
  desplazamiento 24 (donde F3DEX2 tiene el lookat Y), tres s16 en los bytes
  8 a 13, llevados al espacio del modelo. La normal va tal cual (s8 sin
  normalizar; las del auto miden ~120), × 128 hacia abajo, y la luz con el
  módulo que le deja su normalización en el RSP (1,0055): así los índices
  coinciden con los del microcódigo salvo ±1. La tabla son bandas
  especulares (cerca de los productos 0,2 y 0,9).
- El blender mezcla ese alfa con el color de niebla (FAFFFF, casi blanco) en
  el primer ciclo (`c8000230`, G_RM_FOG_SHADE_A) sin G_FOG: los brillos del
  auto. La HLE solo hacía esa mezcla con G_FOG; con F3DFLX y G_TEXTURE_GEN
  también.
- Código: `flx.c` (fuera de línea, `-Os` en el SPE 0), `gfx_ucode_variant`
  aparte de `gfx_ucode_kind` (por el nombre de los datos que carga el
  G_LOAD_UCODE; la calcula `gfx_ucode_reload`). Los dibujos en dos ciclos
  sin textura (`ntex 0`) de los volcados son piezas con G_TEXTURE apagada:
  no eran la causa.
- **Medido contra ares** (4 volcados de la demo de antes y 90 nuevos de una
  instancia propia, carreras de uno y dos jugadores): de 156 comparaciones
  con la base de ≥ 20 dB en 4×4, 102 suben y ninguna baja, +1,26 dB de media
  (s-08, el auto rojo de cerca, 24,2 → 26,7 dB; s-41/42 31 → 38 dB). En la
  LLE desde el estado de carrera, el auto con los reflejos: HLE contra LLE
  31,3 → 35,6 dB en 4×4 (sin la mezcla de niebla daba 38,2: la LLE pasa
  por el rasterizador sin esa mezcla en los triángulos crudos, así que los
  brillos no cuentan ahí; para ellos vale ares).
- `.Rej`: según el manual de Nintendo (gspF3DLX.Rej) no recorta y no dibuja
  un triángulo con un vértice fuera de la caja del clip ratio. Se probó y se
  quitó: no se dispara en ninguno de los 94 volcados ni en la carrera, no hay
  cómo comprobarlo y la pila del SPE 0 no tenía lugar.

**Fondo blanco junto a la pista (diagnosticado, sin arreglar).** Es un
fondo enorme dibujado antes de la pista (sin z), en la parte de «F3DEX
2.03»: triángulos con un vértice lejos (w ≈ 4400, sombra 255) y dos cerca
de la cámara (w ≈ 90–400, sombra 0) muy fuera de la pantalla. El combiner
da textura·(1 − sombra) + sombra·k: donde la sombra baja, sale la textura
clara. Reproducido con ares (volcado s-15 de la demo de dos jugadores: la
consola, pared de roca oscura; la HLE, blanca; 15,9 dB en 4×4):

- La consola **interpola el sombreado lineal en pantalla** (Gouraud del
  RDP) y el RSP **recorta contra la caja del clip ratio** (2×) antes: lo vi
  en los comandos del RDP que quedaron en el búfer FIFO del volcado (los
  microcódigos «fifo» los dejan en la RDRAM; el de F-Zero mide 0x54000
  bytes). La HLE y el RSX interpolan la sombra con corrección de
  perspectiva, así que los vértices cercanos (w chico) dominan y la sombra
  cae a 0 casi en todo el triángulo.
- Probado en el rasterizador del host: solo afín, s-15 16 → 22,5 dB pero
  otras bajan; recorte contra la caja + afín, s-15 16 → 29,6 dB, s-11
  23,7 → 29,8, s-14 21,9 → 32,7, pero s-13 29,9 → 25,6 y s-16 33,4 → 26,2:
  el polígono recortado se triangula distinto (con sombra afín el abanico
  importa: en un píxel de s-16 la consola da 62,7 y mi abanico 194; en otro
  coincide, 227).
- **Para arreglarlo** hacen falta las dos cosas: el recorte del RSP contra
  la caja del clip ratio con el mismo orden de vértices y de abanico que
  F3DEX2 (leerlo de los triángulos del FIFO de los volcados o del
  microcódigo), y la sombra afín en el rasterizador y en el RSX (rsxkit:
  pasar el color multiplicado por w en un texcoord y dividir en el fragment
  program; cuesta una división por píxel). Toca a todos los juegos: va
  aparte, con su medición en la PS3.

**Verificado:** `test-host` 531/531 en Docker (pruebas nuevas
`gfx_f3dflx_alpha`: alfa por la tabla, s y t del vértice, normal sin
normalizar, niebla con el alfa sin G_FOG; con F3DEX2 la texgen de siempre);
las 7 escenas del host y además Snap, Kirby 64, Smash y Conker idénticas
byte a byte; gfxtest 42/42 en RPCS3; los volcados de ares del auto de cerca
repetidos en RPCS3 antes y después (el auto con los brillos en el RSX).
**Pila del SPE 0: holgura 1584** (mínimo 1536; era 2624): `dl.c` vuelve a
`-Os` (−370 bytes, como el 2026-10-13) y `flx.c` va con `-Os`; los dos
campos nuevos de `gfx_state` van al final (en otro lugar desplazan los demás
y el código crece). Queda la palanca de `texcache.c` con `-Os` (1,2 KB).

**Paquete:** «Salsa64 perfil 6b» (SLSA00120, v0.2.5-103-g1c48818),
`salsa64-perfil-fzero.pkg`, sha256 db9ca72f…99140.

**Falta validar en la PS3 real:** F-Zero X en carrera, el auto propio y los
rivales de cerca: deben verse las franjas y brillos claros que se mueven con
la luz (antes lisos). El fondo blanco de la pista espacial sigue (ver
arriba).

## 2026-10-14 — Las ROMs de 64 MB no cargaban después de varios juegos en la misma corrida (paquete «Salsa64 perfil 5»)

En la regresión de la v0.2.6 en la PS3 real (app de perfilado «perfil 3»),
sin salir al XMB, se abrieron Banjo-Tooie (32 MB), Star Fox 64, Pokémon Snap,
Conker (64 MB), Super Odyssey 64 (64 MB) y Zelda OoT (32 MB). Conker y Super
Odyssey 64 daban «menú: no se pudo leer …» a los 15 ms de «menú: abro»; Zelda
cargaba. Con la ROM grande como primer juego, Conker cargaba bien. 15 ms es lo
que tarda `memalign` en devolver NULL, no una lectura de 64 MB.

- **Medición:** el log trae ahora las líneas «memoria …» (`heap_report`,
  `main.c`) al abrir el menú, al abrir cada juego, con la ROM leída, al
  cerrar el juego (junto a «cierre: estado limpio») y tras soltar la ROM:
  `mallinfo`, `sbrk(0)`, la memoria de usuario libre del sistema (syscall 352)
  y la mayor reserva que cabe, en el heap y en páginas del sistema.
- **Reproducido en RPCS3** con ROMs de gfxtest rellenas con ceros a 8, 12, 16,
  32 y 64 MB y `--menu` (32, 12, 16, 64, 64, 32 MB seguidas): las dos de
  64 MB fallaban igual que en la consola. Tras el primer juego el heap se
  quedaba en 107 524 KB con 35 228 KB libres en huecos, la memoria de usuario
  libre en 50 024 KB y «cabe 48 MB».
- **Causa:** no es una fuga. El primer juego reserva, con su ROM de 32 MB ya
  en el heap, cosas que no suelta (medido: la foto de los estados guardados,
  8448 KB, una sola vez; por lectura del código, en la app de perfilado los
  anillos y el registro del recompilador, las tablas de la interpolación por
  geometría y los búferes de los filtros). Quedan por encima de la ROM; al
  soltarla el heap no puede devolver el hueco ni juntarlo, y el siguiente
  juego de 64 MB no cabe. Cada juego además deja ~2 KB sueltos en medio del
  hueco, que lo parten (la ROM de 32 MB siguiente ya no entraba en el hueco de
  35 MB y el heap crecía a 140 MB).
- **Arreglo:** la ROM sale de páginas de 1 MB del sistema (`rom_alloc` /
  `rom_free`, `sysMemoryAllocate` / `sysMemoryFree`), no del heap de newlib; si
  el sistema no las da, cae al heap y lo dice en el log. Al cerrar el juego las
  páginas vuelven enteras: tras cada juego la memoria libre vuelve a su valor
  (82 792 KB en la app normal, 77 672 KB en la de perfilado) y caben 64 MB. La
  RDRAM, las copias del VI y el pool del RSX no se tocaron. `docs/memory-budget.md`
  («Fragmentación del heap y la ROM de 64 MB») trae las cifras y la regla:
  no reservar con `memalign` bloques grandes de vida larga con una ROM viva
  en el heap.
- **Verificado:** `test-host` 530/530 en Docker (no se tocó código del host: el
  cambio es solo del frontend de la PS3, así que las 7 escenas del host salen
  idénticas por construcción); gfxtest OK en RPCS3 en la app normal y en la de
  perfilado; la secuencia 32, 12, 16, 64, 64, 32 MB en la app normal y 32, 8,
  16, 64 MB en la de perfilado: 0 fallas, la ROM de 64 MB carga y el heap se
  queda estable (74 756 KB y 80 268 KB); holgura de la pila del SPE 0 2624
  (sin cambio, mínimo 1536).
- **Paquete:** «Salsa64 perfil 5» (SLSA00120, v0.2.5-100-g0632d30), incluye los
  arreglos de «perfil 4» (tijeras de F-Zero X a 5× y seguro de las esperas al
  RSX), `salsa64-perfil-5.pkg`, sha256 30ded011…45631.
- **Falta validar en la PS3 real** (RPCS3 no reproduce el reparto de páginas
  de lv2): repetir la secuencia que falló (Banjo-Tooie, Star Fox 64, Snap,
  Conker, Super Odyssey 64, Zelda) sin salir al XMB y mirar las líneas
  «memoria …» del log: con el arreglo, «tras soltar la ROM» debe volver a la
  misma memoria libre y «cabe … 64 MB» en páginas del sistema; si apareciera
  «sin páginas de 1 MB del sistema, va en el heap», el sistema no dio las
  páginas y habría que pensar en otra salida (reservar al arrancar). Queda la
  duda de si lv2 comparte el rango de direcciones del heap con esas páginas;
  en RPCS3 no estorba.

## 2026-10-14 — Cuelgue de F-Zero X a 5× en «SELECT COURSE»: tijeras fuera del destino y esperas al RSX con tope (paquete «Salsa64 perfil 4»)

En la regresión de la v0.2.6 en la PS3 real (app de perfilado «perfil 3»,
1080p, escala 5×, efectos de framebuffer completos), F-Zero X se colgó al
pulsar START en el menú: el latido marcaba «presentación quieta en
«cola»… sin dibujar», ni el botón PS sacaba el menú y hubo que apagar la
consola a la fuerza. La última tarea registrada por el perfilado era la
792 (2862 comandos, 3510 triángulos), la primera de la escena con las
copas en 3D.

- **Reproducido en el host:** con las entradas del log UDP (`--pad`, las
  lecturas del PIF salen iguales que en la consola: 142, 351, 790…) y el
  volcado nuevo por número de tarea (`SALSA64_GFX_DUMP_TASKS=792`), la 792
  sale con los mismos 2862 comandos y 3510 triángulos: es «SELECT COURSE».
  Las copas fuera de la pantalla traen tijeras con x0 más allá del borde
  (74,75, 394,75, 714,75 y 1023,75 con x1 = 308; en las tareas siguientes,
  327, 647 y 967): vacías, el RDP no dibuja nada. Ninguna tarea de menú
  anterior las tenía; todo lo demás de la tarea está en rango (vértices,
  texturas).
- **Causa probable:** rsxkit recortaba al destino solo x1 e y1, así que a
  5× mandaba `rsxSetScissor` con x = 5119 (y 4835), por encima del máximo
  de 4095 de las superficies del RSX; a 4× o menos nunca pasa de 4095.
  RPCS3 lo acepta sin decir nada (la repetición del volcado a 5× no se
  cuelga). **Arreglo:** rsxkit recorta las cuatro coordenadas y no manda
  los dibujos con tijeras vacías; la repetición en RPCS3 da la misma imagen
  byte a byte con y sin el cambio. Falta confirmarlo en la consola.
- **Seguro (pase lo que pase):** ninguna espera del PPE al RSX es infinita
  (docs/hilos-ppe.md, «Esperas al RSX con tope»): a los 2 s una línea
  «RSX: el PPE espera hace …» con qué espera, GET y PUT, si el RSX avanzó,
  la tarea en traducción y su etapa; mientras, se atiende al sistema; a los
  12 s (o con «Salir del juego») se escribe el guardado, se cierra la sesión
  del perfilado (`salida=rsx`) y se sale. La espera de lugar en el búfer de
  comandos la vigila el latido. Probado en RPCS3 con `colgar-rsx`
  (`etiqueta`, `rsx`, `fifo`): diagnóstico a los 2 s y salida a los 12 s en
  los tres.
- **Verificado:** `test-host` 530/530 en Docker; las 7 escenas del host
  idénticas byte a byte a main c1ca4a0; gfxtest 42/42 en RPCS3 (la app
  normal y la de perfilado); holgura de la pila del SPE 0 2624 (sin
  cambio).
- **Paquete:** «Salsa64 perfil 4» (SLSA00120, v0.2.5-98-g160effd),
  `salsa64-perfil-4.pkg`, sha256 7140e49d…22b1e.
- **Qué mirar en la PS3:** F-Zero X a 5× y 1080p hasta «SELECT COURSE»
  (y de ahí a la carrera) no debería colgarse. Si se cuelga igual, ya no
  hace falta apagar a la fuerza: a los 12 s sale solo, y el log UDP trae
  la línea «RSX: …» (qué espera, GET/PUT, tarea y etapa) y la sesión del
  perfilado termina con `salida=rsx`: con eso se sigue. No se pudo saber
  si la pausa (START + SELECT en el campo 1030, junto al START del juego)
  influyó; con la causa de arriba no hace falta.

## 2026-10-14 — v0.2.6 lista para la regresión en la PS3

Todo lo de la v0.2.6 está en main (73bc213): F-Zero X (auto y HUD, recarga de
microcódigo a mitad de lista) y Banjo-Tooie (desafío del CIC 6105) jugables en
la PS3; ruido del logo de Star Fox; hojas de Pokémon Snap (umbral de alfa);
sombras de Conker (render a textura de 8 bits); interpolación por geometría
(mejor a 3×/720p); caja negra y estados comprimidos en un SPE; VI_V_SYNC
imposible (Super Odyssey 64); caché de destinos de JR encendida; laboratorio del
recompilador. Falta la regresión con la app de perfilado (sesiones en
/dev_hdd0/salsa64/perfilado/, `tools/perfilado/perfilado.py juegos`), integrar
el perfilado a main y publicar.

## 2026-10-13 — F-Zero X en carrera: el microcódigo se recarga a mitad de la lista (rama `suspicious-saha-fe2758`)

En la PS3 (main cd58563) F-Zero X corría a 60 campos/s pero en carrera no
salía el auto, ni el HUD, y el fondo se veía mal. Causa: **el juego cambia de
microcódigo a mitad de cada lista** (gSPLoadUcodeEx: `G_RDPHALF_1` con la
dirección de los datos y `G_LOAD_UCODE`) del «F3DEX 2.03» de la tarea al
«F3DFLX.Rej 2.03F» (datos en 0xD7500, texto en 0xC9F00; mismos opcodes que
F3DEX2), y la HLE tomaba cualquier `G_LOAD_UCODE` a otra dirección por un
microcódigo ajeno (S2DEX…) y se saltaba el resto de la lista: ~875 comandos
por tarea (el auto, el HUD con sus TEXRECT y el fondo de después), con la
lista terminando dentro del tramo saltado (tarea «cortada»). En el log de la
PS3: «299 tareas cortadas, 270 778 comandos de otro microcódigo saltados» por
cada ~300 tareas, de 9 dibujos por tarea.

- **Arreglo** (`f3dex2.c`, `s2dex.c`, `dl.c`): `gfx_ucode_kind` lee el texto
  de los datos que carga el `G_LOAD_UCODE` («RSP Gfx ucode <nombre> fifo N.xx»:
  generación por la versión, `.NoN`, S2DEX 2.x) y, si es otro microcódigo de la
  misma generación (2.x), `gfx_ucode_reload` lo toma como el de la tarea: el
  RSP arranca de nuevo (segmentos, modo geométrico, matrices, luces, vértices,
  clip ratio 2, textura; la lista y la pila de G_DL siguen) y se sigue
  interpretando. S2DEX2, el de la primera generación y los que no se
  reconocen (L3DEX, S2DEX 1.x, el de Conker) siguen como antes. `gfx_run` usa
  el mismo `gfx_ucode_reload` al empezar la tarea y `g->ucode` lleva el
  microcódigo en curso (la vuelta desde un tramo ajeno compara con él).
  También `G_NOOP` (0x00) de F3DEX2, que se contaba como sin implementar.
- **Medido en el host** (estado del autor, `F_ZERO_X-B30ED978.st0`, 1200
  campos de carrera): de 9 dibujos, 263 triángulos y 1 rectángulo por tarea a
  51 dibujos, 409 triángulos y 48 rectángulos; 0 comandos saltados, 0 tareas
  cortadas, 0 errores. Con ares (modo demo de F-Zero X, ráfaga de volcados por
  GDB, una instancia propia): en las tomas de un jugador la HLE dibuja el
  auto, la pista, el HUD (posiciones, vuelta, energía, tiempo, velocímetro,
  mapa) y las chispas; PSNR en 4 × 4 de 31,5 y 29,2 dB en dos tomas con el
  mismo cuadro (el HUD marca el mismo tiempo en la consola y en la HLE) y de
  23,3 dB en una donde la lista de la RDRAM era 5 centésimas más nueva que el
  búfer (el cielo con nubes y el reflejo del auto salen distintos por eso, no
  se pudo separar). Lo que difiere con el mismo cuadro: el degradado del cielo
  y las orillas de los edificios, y las chispas y los bordes del auto por la
  cobertura del AA (límite del rasterizador, docs/referencia-ares.md). Falta
  una toma sincronizada del auto de cerca.
- **RSX**: el volcado de la HLE de esa carrera (`--ares --gfxdump`) repetido
  en RPCS3 (`--replay`) sale como el rasterizador del host: auto, chispas y
  HUD. (La primera repetición de un volcado nuevo en RPCS3 dibujó las chispas
  como un rombo naranja opaco que tapaba el auto; con los mismos bytes no
  volvió a pasar en 5 corridas, ni aislando el dibujo: se atribuye al
  compilador asíncrono de shaders de RPCS3 la primera vez. Si en la consola
  aparece, mirar el dibujo con `gfxdump.py list --draw 38`.)
- **Verificado**: `test-host` 502/502 en Docker (ASan/UBSan; prueba nueva
  `gfx_load_ucode_same_generation`: sigue con 2.x, reinicia el modo
  geométrico, salta 1.x y L3DEX); las 7 escenas del host (SM64 en el campo
  700; Banjo-Kazooie, Zelda OoT, DK64, Perfect Dark, GoldenEye y MK64 en el
  900) idénticas byte a byte; Kirby 64 (S2DEX, 308 fondos) igual; gfxtest en
  RPCS3 42/42. **Pila del SPE 0: holgura 1584** (main 1856; el mínimo es
  1536): el código nuevo cuesta ~500 bytes, `dl.c` pasa a `-Os` (−370; solo
  `gfx_dl_next` corre por comando y es corta) y el enlace redondea el espacio
  libre a 128 bytes, así que con `f3dex2.c` en `-Os` (−72 bytes) la holgura
  sería 1712; no se hizo por ser el despachador por comando. Otra palanca sin
  tocar: `texcache.c` con `-Os` ahorra 1,2 KB (identificar texturas cuesta
  0,01–0,02 ms por tarea).
- **No medido**: el «desorden» del fondo que se veía en la PS3 no se
  reproduce en el host ni en RPCS3 con el arreglo (antes el host tampoco
  dibujaba auto ni HUD; el fondo de la primera mitad salía normal en el
  host); lo más probable es que fuera la lista cortada (el juego esperaba al
  RDP), pero falta verlo en la consola. El microcódigo «.Rej» rechaza los
  triángulos que habría que recortar contra el plano cercano; la HLE sigue
  recortándolos como siempre (sin diferencias visibles contra ares en las
  tomas medidas, pero no se buscaron a propósito).
- **Con main a338653 integrado** (interpolación, `--gc-sections`): holgura del SPE 0
  2624 sin el `-Os` de `dl.c` (se quitó; main 3136); `test-host` 521/521; las 7
  escenas idénticas a main; gfxtest 42/42 en RPCS3; Zelda en RPCS3 con
  `interp-prueba=1`: 71–83 % de vértices con pareja, como en la documentación
  de la interpolación (la recarga de microcódigo no toca los registros de grupos).
- **Falta**: probar el paquete en la PS3 (no se armó: lo pide main).

## 2026-10-13 — Interpolación de cuadros por geometría: 60 campos por segundo en juegos a 20 o 30 (rama `kind-dijkstra-c790f9`, paquetes SLSA00116 «Salsa64 interp 1» a «interp 8»)

La HLE deja la identidad de cada grupo de vértices (G_VTX) y sus
coordenadas de recorte; el PPE empareja los grupos de un cuadro con los del
anterior (src/rdp/vmatch.c) y en los campos entre dos cuadros redibuja la
tarea con un vertex program que interpola las cuatro coordenadas. Opción
«Interpolación» en Opciones, por juego y en la pausa («Imagen»), apagada
por omisión (apagada, gfxtest igual byte a byte a main). En la PS3, a 720p
y 3×, Zelda «mejora mucho»; a 5× el RSX no alcanza. Cada problema que vio
el autor se reprodujo en el host con sus estados y sus entradas sacadas del
log: el HUD (hash del contenido en el SPE, ortográficos sin interpolar), el
agua (vértices que cruzan la cámara; profundidad interpolada), Mario sin
intermedios (la caché de texturas), Link al darse vuelta (objetos todo o
nada, giros que aplastan), el entorno con Z (corte por giro de cámara) y
los destellos (partículas que cruzan el plano cercano).

Falta: estimar la cámara entre cuadros (slerp en vez del corte con Z),
bajar los ~2 ms del PPE por tarea, avisar en «Imagen» a 5×, notas de
vmatch en la caja negra. Por confirmar con interp 7/8: el giro con Z y los
destellos. Detalle, pendientes y la lista de commits para la v0.2.6
(`--gc-sections` primero) en docs/interpolacion.md, «Estado al
2026-10-13 y lo que falta».

## 2026-10-13 — Comparación con ares: lo que queda (rama `suspicious-saha-fe2758`)

Cerrado en esta rama: la herramienta `salsa64-run --ares`, el umbral de
alfa de TEX_EDGE con umbral (hojas de Snap), las sombras de Conker (render
a textura de 8 bits; funcionan en la PS3) y las aristas de los dibujos con
AA, apagadas por omisión tras la regresión en la PS3 (`aristas-aa` las
enciende). Las grietas de GoldenEye quedan como están (ares también deja
ver algunas); el experimento de líneas solo en las rendijas queda en la
rama (`src/host/cracks.c`, docs/rsx-notes.md) sin integrar. Pendiente,
con volcados de ares (docs/referencia-ares.md):

- el muaré de las franjas de GoldenEye en el túnel de Dam (LOD);
- TEX_EDGE sin umbral (la HLE corta en 128), con un volcado de follaje de
  Zelda o de Banjo;
- la sombra de Todd desde arriba en Snap, con un volcado de esa toma.

## 2026-10-13 — F-Zero X arranca (bus abierto del PI) y estado de arranque medido en ares (rama `pensive-germain-4c2e2b`)

- **F-Zero X** ya llega al título («PUSH START»), con 342 tareas gráficas y 569
  de audio en 10 s. No era el arranque 6106: el juego sondea el 64DD leyendo
  `0x05000508` (ASIC_STATUS) y, con 0, creía que había uno y se quedaba
  esperando al hilo del manejador del disco (Leo) para siempre. En ares, el
  espacio del PI sin dispositivo (0x05000000–0x07FFFFFF y desde 0x1FD00000)
  lee la mitad baja de la dirección repetida (`0x05080508`). Implementado en
  `salsa64_bus_read32`. Las lecturas de 0x04900000–0x04FFFFFF siguen dando 0.
- Arranque medido en ares (IPL2 e IPL3 reales) en el punto de entrada, con
  SM64 (6102), Banjo-Kazooie (6103), Perfect Dark y Banjo-Tooie (6105) y
  F-Zero X (6106): `s7 = 1` y `osVersion = 1` con todos (antes 0); 6106 deja
  DMEM/IMEM en 0xFF con 0x17D8 en IMEM 0x000 y `osCicId` = 0x17D8; 6105 deja
  DMEM e IMEM enteras en 0xA4002000. (No reproducido: s6 = 0xA0000200 con el
  6105, SP_MEM_ADDR/DRAM_ADDR/RD_LEN/WR_LEN/semáforo del SP con el 6105, los
  registros temporales, y la copia de la imagen del IPL3 6106 en RDRAM
  0x000–0x2BC, que no está tal cual en la ROM.) Nueve escenas del host
  (PD, Majora, SM64, MK64, GE, BK, Conker, DKR, DK64) idénticas antes y después.
- **Banjo-Tooie** (y su protección): la RDRAM de entrada ya coincidía con ares
  byte a byte, no era el arranque. Su cargador de datos los descifra con una
  clave de 14 bytes que sale del **desafío del CIC 6105 por la PIF** (al
  escribir la PIF RAM con el bit 0x02 en el byte 63, los 30 nibbles de los
  bytes 48–62 se reemplazan por la respuesta). Nuestra PIF no contestaba:
  descifraba basura, un tamaño salía negativo (`0xFFFB…`), la reserva de memoria
  devolvía NULL y el hilo principal caía por TLBL. Implementado en
  `src/core/pif.c` (`cic6105_challenge`, solo con CIC 6105): autómata de dos
  tablas con clave inicial 0xB, validado nibble por nibble (150) con 5 pares
  desafío/respuesta capturados en ares (prueba `pif_cic6105_challenge`). Una
  entrada de la segunda tabla (0xE → 0xC) no coincidía con lo que se
  recordaba de otras fuentes; se fijó con los datos medidos. Ahora Banjo-Tooie
  dibuja la intro (296 tareas gráficas, 470 de audio en 16 s; Dolby). PD, Conker, DK64, Majora, SM64 y
  F-Zero X dan el mismo cuadro antes y después.
- **Falta probar:** F-Zero X y Banjo-Tooie en la PS3 (regresión v0.2.6); Banjo-Tooie
  jugando más allá de la intro (otras protecciones de Rare, ANTI_TAMPER) y con
  guardado EEPROM 16K; el desafío en Perfect Dark y Conker (no lo pidieron en
  las pruebas del host); s6 = 0xA0000200 y los registros del SP con el 6105
  (no reproducidos, sin efecto visto).
- Método útil: contar las reservas del asignador del juego (ares con punto de
  parada en 0x8001ACCC y el host con paso de 1 ciclo) y seguir el primer
  argumento distinto hasta el dato mal descifrado. Ojo: los puntos de parada
  de ares solo saltan en entradas de función, no en mitad de un bloque.
- Trampa del GDB de ares: tras `\x03` llegan dos paquetes de parada; hay que
  drenar el segundo o cada respuesta llega desfasada una petición (los volcados
  de memoria salen corridos un bloque).

## 2026-10-13 — Star Fox 64: el ruido del logo inicial era el relleno de 320 × 720 que llegó antes de que hubiera framebuffers (rama `elastic-elbakyan-3dd3f0`)

El censo de v0.2.0 anotaba «falla breve en el logo inicial» y el autor la
describió como ruido en la primera pantalla. Un volcado de ares (GDB-Server,
cada ~0,3 s desde el arranque) muestra en la primera imagen ruido exacto: es
la RAM baja (`VI_ORIGIN` 0x280) dibujada como imagen, pero **no es lo que
sale**: con `VI_H_VIDEO` en 0 (libultra lo pone con `osViBlack`) el VI no
muestra nada y la consola da negro; el volcado dibuja el origen sin mirar el
área activa. Salsa64 hace lo mismo que la consola: en el host los campos 2–7
traen `VI_CONTROL` 0x1300e, `VI_ORIGIN` 0x280 y `VI_H_VIDEO` 0, y
`salsa64_vi_frame` devuelve «sin imagen» (el frontend presenta negro: la
rama `m->f.width == 0` de `present_field` no copia nada de la RDRAM; prueba
nueva en `test_vi.c`). `--trace-vi` del host ahora también anota esos campos
(«sin imagen», con los registros).

**Lo que sí sale mal** empieza en el campo 8, cuando el juego enciende el VI
(`VI_H_VIDEO` 0x006C02EC, origen 0x38fa80) y dura hasta el cambio de escena
(campo ~158), todo el logo de «Nintendo presents»: ruido de colores en el
borde de 8 px (arriba y a los lados de las primeras ~32 filas) de la imagen
del RSX. En el host se ve con `--gfx-frame` (8358 bytes distintos de cero en
la HLE a partir del campo 8; en la consola esa memoria es `0x0001` en toda
la pantalla); en RPCS3 no se reproduce (las superficies salen limpias), en
la PS3 real es el «ruido tipo estática en la parte superior» que ya había
reportado el autor tras 8c8e3cd.

Causa (traza de tareas con `--trace-gfx` y `SALSA64_GFX_VERBOSE`):

1. Entre los campos 1 y 2 el juego carga por DMA 20 KB de datos de la ROM
   en `38fa80` (filas 1–32 del primer framebuffer).
2. Campo 3, primera tarea gráfica: un FILLRECT de 320 × 720 sobre una color
   image en `38f800` limpia los tres framebuffers seguidos (`38f800`,
   `3b5000`, `3da800`), borde incluido. Todavía no existe ningún destino
   (se crean en las tres tareas siguientes), y el reparto de rellenos de
   imagen alta (`rdp_targets_inside`, sección «Star Fox 64 colgado a los 5 s»)
   solo alcanza a los destinos existentes: el relleno se pierde.
3. Campos 4, 5 y 6: nacen `3b5000`, `3da800` y `38f800`, cebados con la
   RDRAM (`prime`), donde el RDP de la HLE nunca escribió el relleno y siguen
   los datos del DMA. Cada cuadro solo limpia 8..312 × 8..232: el borde queda
   con el ruido. En la consola el borde es negro porque el relleno sí está en
   la RDRAM. El cambio de escena repite el relleno con los destinos ya
   vivos: ahí sí se limpia (por eso el ruido era «breve»).

Arreglo (`src/rdp/targets.[ch]`, `rdp.c`, `host/raster.c`): cada relleno de
imagen alta se **anota** (`rdp_targets_tall_note`: imagen, rectángulo, color,
tarea y los destinos que ya lo recibieron) y un destino que nace después y
cae dentro (misma regla que `rdp_targets_inside`) lo recibe una sola vez al
crearse (`rdp_targets_tall_pending`; antes de los dibujos de su tarea, y
cuenta como dibujado, como cualquier dibujo de la tarea).
Solo vale mientras no hayan pasado `RDP_TALL_FILL_TASKS` (32) tareas, no lo
haya recibido ya ese destino y ningún DMA haya tocado su memoria desde el
relleno (anillo de DMA; en la consola el DMA pasa por encima del relleno:
manda el dato del DMA). Lo que no cambia: los destinos que ya existían (se
reparte como antes), el cebado con la RDRAM de los destinos que nacen sin un
relleno pendiente y la rama de copia de la RDRAM al presentar un origen sin
destino (la usan los juegos que dibujan el framebuffer con el CPU; no se
tocó: sin evidencia de un caso real, un criterio de «datos del juego»
rompería esos).

Pruebas: unitarias `targets_tall_pending` (entrega única, DMA posterior,
DMA anterior, vigencia, otro ancho o dirección, rectángulo parcial),
`raster_tall_fill_first` (el escenario de Star Fox con ruido de DMA en el
primer framebuffer: el borde sale con el color del relleno; falla sin el
arreglo) y `vi_frame` con `H_VIDEO` 0 (negro). Host: Star Fox 64 con la HLE
(`--gfx-frame` cada campo del 8 al 14, y cada 20 hasta el 200): borde
limpio en todos (antes 8358 bytes de ruido). Siete escenas de otros juegos
comparadas con el árbol anterior (`--gfx-frame`, sin controles: SM64 en el
campo 700; Banjo-Kazooie, Zelda OoT, Donkey Kong 64, Perfect Dark, GoldenEye
y Mario Kart 64 en el 900), y Star Fox en el 400, salen idénticas byte a byte. RPCS3: el frontend entrega el
relleno a los tres destinos al crearse (`gfx: destino nuevo …: relleno de
imagen alta anterior`), la intro hasta el campo 400 se ve igual que la de
ares (logo, «STARFOX» con las estrellas azules) y gfxtest pasa (42 de 42
puntos). No se armó paquete: falta confirmarlo en la PS3 real.

Observado y sin tocar: `prime` usa un solo búfer de cebado (`rdp.rb +
RB_SLOTS * RB_SLOT`) y el RSX lo lee después (`rsxkit_target_fill` solo
encola el blit); tres cebados seguidos (como estos) podrían ver, en la
consola real, los datos del último. Con el relleno entregado la pantalla
entera de cada uno se vuelve a borrar y no importa en este caso; en otro
juego con relleno parcial sí podría.

## 2026-10-13 — Sombras de Conker: render a textura de 8 bits (rama `suspicious-saha-fe2758`)

En Salsa64 fallaban todas las sombras de Conker. Con ocho volcados de ares
(`salsa64-run --ares --pasadas 2`, que corre la lista como dos cuadros
seguidos para que lo que se lee por la RDRAM sea lo que copió el
consumidor, como en la PS3) salió el mecanismo: la silueta de cada
personaje se dibuja en una imagen de 64 × 64 de 8 bits declarada CI, se
desenfoca releyéndola como I8 con dos texrects y el piso la usa como alfa
de la sombra. Dos fallas: la HLE decodificaba esas I8 de la RDRAM antes de
que el RSX dibujara la silueta (solo las RGBA16 se leían del destino), y
`src/rdp` solo copiaba a la RDRAM las imágenes de 8 bits de intensidad. Y
una tercera al arreglarlas: la copia del destino que se lee mientras se
dibuja era una por tarea, y Conker reutiliza la imagen para cada
personaje. Arreglo en la HLE (`fbimg` lleva las de 8 bits; una I8 que cae
en una sale como `GFX_TEX_TARGET`), en `src/rdp` (la lee del destino con
`RSXKIT_REMAP_RRRR`, o la decodifica con `rdp_decode_i8`; copia las de 8
bits de cualquier formato; recopia el scratch si se dibujó donde se lee) y
en el rasterizador del host (docs/framebuffer.md).

- Contra la consola, 4 × 4: de 20,8–25,8 dB (como en la PS3) a 23,5–30,4,
  igual o mejor que con la máscara que la propia consola dejó en la RDRAM.
- Pila del SPE 0: holgura 1856 (main 1984; −128 bytes de texto).
- De los juegos de Rare medidos en el host, solo Conker dibuja en
  imágenes de 8 bits (3544 texturas I8 leídas de su imagen en 4000 campos
  de la intro, ninguna de la RDRAM); DK64, Perfect Dark, Banjo-Kazooie y
  GoldenEye no. Banjo-Tooie (no llega a dibujar en el host) y Diddy Kong
  Racing (microcódigo sin HLE) no se pudieron medir.
- RPCS3: gfxtest igual que main. La repetición del cuadro de Conker sale
  con el piso negro cuando hay dos copias del scratch en la misma tarea
  (con una, bien; en el host, bien).
- **PS3 real («Salsa64 aristas 2»): las sombras de Conker funcionan.** El
  piso negro era cosa de RPCS3.

## 2026-10-13 — Líneas azules de GoldenEye: aristas de los dibujos con AA como líneas (rama `suspicious-saha-fe2758`)

Con dos volcados de ares del túnel de Dam (`salsa64-run --ares`): las
líneas azules son el borrado azul que asoma por grietas de uniones en T
entre triángulos del piso y de la orilla (0,26 a ~1,1 px, todos
G_RM_AA_ZB_OPA_SURF). La consola las tapa porque con AA_EN escribe los
píxeles cubiertos en parte; el RSX y el rasterizador del host solo miran el
centro. Arreglo en el RSX (`rsxkit_draw.edges`, `src/rdp/rdp.c`): en los
dibujos opacos con AA_EN (sin calcomanías), antes de los triángulos, sus
aristas como líneas (modo de polígono LINE) de 1,5 píxeles de la N64 a la
escala de la superficie, con desplazamiento de polígono hacia atrás
(factor 8, 64 unidades). El rasterizador del host hace lo mismo y con él se
eligieron ancho y desplazamiento contra la consola (docs/rsx-notes.md):

- Grietas: el túnel, 20 de 20 píxeles tapados; la orilla, 170 de 170 (con
  1 px quedaban 32). PSNR en 4×4: 29,97 → 29,66 dB y 33,04 → 33,32.
- Las líneas engordan las siluetas contra el fondo hasta ¾ de píxel; con
  menos desplazamiento se comían piezas casi coplanares (el ojo de Banjo).
  En las 7 escenas del host cambian 82 de 105 imágenes, en los bordes
  (360–1350 píxeles por imagen); Zelda casi no usa el modo (0,3 dibujos
  por tarea).
- Dibujos con aristas por tarea: GoldenEye 64 de 154, Conker 82 de 171,
  Perfect Dark 39 de 108, Banjo 32 de 77, Mario Kart 64 29 de 113, Super
  Mario 64 16,5 de 52. En el RSX es una llamada más por dibujo AA con el
  mismo arreglo de vértices.
- RPCS3 no sirve para verlo: su backend Vulkan no aplica el modo de
  polígono (el replay del túnel sale igual con y sin aristas). gfxtest en
  RPCS3 da la misma imagen que main. **Falta la PS3 real**: el ancho va en
  el método 0x3B8 del NV40 (punto fijo 6.3, como lo decodifica RPCS3), que
  PSL1GHT no expone.
- **PS3 real («Salsa64 aristas 2»): regresión.** En los modelos los
  polígonos se superponen y salen picos en las uniones, como con
  `aa_grow`: a 5× el ancho es ~7,5 px de la superficie y las líneas
  engordan las siluetas. Quedan apagadas por omisión; `aristas-aa` en
  `dev_hdd0/salsa64` las enciende para seguir probando.
- `--ares --gfxdump` guarda la salida de la HLE de cada lista como el
  volcado de la PS3 (big-endian): `tools/gfxdump.py`, `--raster-dump` y
  `embedded` = `dump:<nombre>` en el RSX la leen. Así un cuadro de ares se
  puede repetir en la consola.

## 2026-10-13 — Comparar un cuadro con ares: `salsa64-run --ares` (rama `suspicious-saha-fe2758`)

`salsa64-run --ares volcado-rdram.bin` (y `tools/ares-comparar.sh`) dibuja
con la HLE y el rasterizador del host las tareas gráficas que quedaron en
un volcado de la RDRAM exportado por ares y compara cada búfer de imagen
que dibujan con el que dibujó ares: el mismo cuadro con los mismos datos,
sin llegar a la misma escena en los dos emuladores (docs/referencia-ares.md).
Para eso el rasterizador del host dibuja a 8 bits en ese modo
(`raster_q5`) y ahora recorta contra w > 0 como el RSX: antes descartaba
entero el triángulo con un vértice detrás de la cámara. En las 105 imágenes
de las 7 escenas del host solo cambian 4 cuadros de la intro de Perfect
Dark (2500–2600): aparecen los muros pegados a la cámara, como en el RSX y
en GLide64.

Primer uso, intro de Pokémon Snap (F3DEX2 .NoN; seis volcados, tres
cuadros distintos; en cada uno, una de las dos listas es la del cuadro que
el CPU arma y dibuja en 000000):

- La HLE dibuja la escena entera como la consola: PSNR 33,8–35,4 dB en
  promedios de 4×4 (24–31 por píxel, por el tramado de la consola) y
  diferencia media de +0,1 a +2,2/255 por canal. Ninguna geometría falta ni
  sobra.
- **Umbral de alfa de las hojas.** Usan G_RM_AA_ZB_TEX_EDGE con
  G_AC_THRESHOLD y el color de mezcla con alfa 0. `blend_state`
  (src/rsp/gfx/rdp.c) toma el umbral del color de mezcla (1/255) e ignora
  CVG_X_ALPHA, y la consola no escribe los píxeles con α·cobertura = 0
  (α < 32 con el píxel cubierto entero): la HLE deja las hojas medio texel
  más gruesas y tapa huecos de cielo. Barrido del umbral contra la
  consola: 24–32 es lo mejor en los dos cuadros; 128 (lo que usa la rama
  sin G_AC_THRESHOLD) empeora 2 dB. **Arreglado:** con CVG_X_ALPHA y sin
  FORCE_BL el umbral es al menos 32; los dos cuadros pasan de 33,91 a
  34,28 dB y de 33,80 a 34,09. Las 105 imágenes de las 7 escenas del host,
  iguales (ninguna usa ese modo); la pila del SPE 0, igual (holgura 1984).
  La rama sin umbral (128) no se pudo medir: los volcados de Snap no la
  usan.
- El resto son bordes de un píxel (la cobertura del AA, unos ½ px), el
  filtro de tres puntos contra el bilineal y el blender con 5 bits de alfa
  (un paso en los textos de los créditos): del rasterizador, no de la HLE.

## 2026-10-13 — CFW (issue #2): respaldo automático al intérprete y prueba del JIT para usuarios (rama `nifty-knuth-a7c921`, paquetes SLSA00119 «Salsa64 CFW 1» y SLSA00118 «Salsa64 JIT test»)

Con CFW Evilnat 4.91 (CECH-2501A) ningún juego arrancaba: el hilo
principal se quedaba en `salsa64_rec_init`, entre el log del guardado y
«páginas:», con la aplicación viva. Con `sin-recompilador` los juegos
arrancan (Majora a 60 fps con el intérprete). Lo único de ese tramo que
toca algo distinto de la memoria de datos es la primera escritura en la
caché de código (cola NOBITS del segmento de texto) y la syscall 351 del
log de páginas; lo probable es lo primero: un fallo de página que con ese
CFW deja el hilo detenido en vez de matar el proceso (docs/jit-cfw.md).

- La aplicación prueba la caché en un hilo aparte al arrancar (escribir,
  limpiar, ejecutar, reescribir, al principio y al final, y cada página) y
  `run_game` espera el resultado hasta 2 s; si no vuelve, intérprete y una
  línea clara en el log. Con HEN la prueba termina antes de abrir un
  juego. Si el fallo matara el proceso, `jit-prueba.txt` lo recuerda y esa
  versión va directo al intérprete.
- `tests/phase0/jit-ppe` con `PRUEBA=cfw`: la prueba para usuarios, con
  pantalla, cada vía en un hilo con tiempo límite (COLGADO sin reiniciar)
  y reanudación tras un cierre. Además de la caché de la aplicación, prueba
  escribir por PS3MAPI (`SET_PROC_MEM`), páginas ejecutables de PS3MAPI y
  `sys_dbg_write_process_memory`: si con CFW la caché no se puede escribir
  directo, alguna de esas vías puede servir al recompilador, y la prueba
  mide su costo. Instrucciones en inglés y español en docs/jit-cfw.md.

Probado en RPCS3 (no concluyente para el JIT): la prueba completa su ciclo
con reinicios, y con un cuelgue simulado la aplicación pasa al intérprete
a los 2 s (vitest OK) y la prueba marca COLGADO y sigue. Falta: el paquete
de la aplicación en la PS3 con HEN (que no cambie nada) y la prueba en la
consola con CFW.

## 2026-10-13 — Pokémon Snap, Kirby 64 y Pokémon Stadium 2: z del primitivo exacta en el RSX, paletas de S2DEX y JPEG de Stadium (rama `modest-yonath-bc13d3`, paquete SLSA00114 «Salsa64 gráficos»)

Tres causas distintas detrás de lo que se veía en la PS3 real desde la
v0.2.4.

**Pokémon Snap: orden de dibujo al azar.** Snap dibuja todo el escenario
con `G_ZS_PRIM` (la profundidad del primitivo, `G_SETPRIMDEPTH`, en vez de
la de cada píxel): todas las capas tienen la misma z y el orden lo da la
lista; la prueba del RDP (≤) deja pasar al último. En el volcado de la PS3
(campo 3638) los 4566 vértices traen z/w = 0,97690. La HLE los daba como
z = prim · w y el RSX hacía `2z − w` en el vertex program y luego la
división entre w: cada vértice redondea distinto y quedan pendientes de un
bit en la Z24, así que entre capas coplanares la prueba LEQUAL pasaba o no
al azar (troncos negros encima del follaje, suelo encimado). Arreglo:
- La HLE marca esos dibujos con `GFX_DEPTH_PRIM` y abre otro dibujo cuando
  cambia la z del primitivo (antes un dibujo podía mezclar varias: el humo
  de Conker, los destellos de Snap). La z del primitivo es de 15 bits, como
  en el RDP (Smash pone 0x9000 en su fondo y se leía como 1,125, detrás
  del plano lejano). Los triángulos crudos del RDP también la usan.
- `lib/rsxkit`: `RSXKIT_DEPTH_CONST` con `rsxkit_draw.z`: el viewport pone
  esa z en todo el dibujo (escala 0, desplazamiento z), sin pasar por la
  división entre w. Dos dibujos con la misma z dan exactamente la misma
  profundidad en cada píxel. `src/rdp` la saca del primer vértice,
  redondeada a la rejilla de 15 bits (`gfx_prim_z`).
- El rasterizador del host hace lo mismo (también tenía el empate al azar
  por un ulp).

RPCS3 no reproduce el error (dibuja bien Snap con y sin el arreglo): hay
que confirmarlo en la PS3. Con el arreglo, la intro de Snap en RPCS3 sale
como la de mupen64plus.

Segunda vuelta, tras probarlo en la PS3 (orden bien, pero cajas
translúcidas de seto y neblina sobre las colinas y una franja verde
vertical detrás del logo de HAL; volcados de los campos 207 y 664): en la
intro Snap nunca fija la z del primitivo (no hay G_SETPRIMDEPTH en los
primeros 1300 campos; en el juego la fija por objeto, entre 0x7C90 y
0x7D44), así que todo empata. Los opacos (ZMODE_OPA) pasan con el empate y
quedan en el orden de la lista; las capas translúcidas (ZMODE_XLU: setos,
neblina, pasto) en el RDP solo pasan si quedan estrictamente delante o
donde el z-buffer sigue borrado (`sz < oz || oz == máx`, la comparación de
angrylion y ParaLLEl-RDP). Con ≤ se veían enteras, con sus cajas. Ahora
esos dibujos llevan `GFX_DEPTH_LESS` (`RSXKIT_DEPTH_LESS`: GCM_LESS; el
rasterizador del host igual), solo con la z del primitivo: con la de cada
píxel, la z en coma flotante no es la del RDP y un empate de más o de
menos sería al azar (con la regla para todo XLU se perdían luces de los
edificios de Perfect Dark). En el juego, con una z por objeto, el seto
con z mayor que el terreno solo se ve sobre lo más lejano, como quiere el
juego. mupen64plus (GLide64) dibuja la intro con la z de cada píxel y
ahí sí muestra el seto detrás de las colinas y el pasto delante de Todd;
con la regla del RDP, en «JACK and BEANS» se ve a Todd con su cámara en
primer plano. Pendiente de confirmar en la PS3.

**Kirby 64: fondos y logotipos con ruido y en ajedrez.** Kirby cambia a
S2DEX dentro de la tarea para sus fondos CI8 (`G_BG_1CYC`) y carga la
paleta con `G_OBJ_LOADTXTR` (uObjTxtr de tipo paleta, 256 colores en la
mitad alta de la TMEM). La HLE saltaba todos los `G_OBJ_*`: el fondo salía
con la paleta que hubiera quedado en la TMEM (el marco de la intro verde y
negro en vez del ajedrez beige, el logotipo en arcoíris, la selección de
archivo ilegible). Ahora `G_OBJ_LOADTXTR` hace la carga de sus tres tipos
(bloque, tile y paleta) con la tile 7, siempre (el microcódigo se la salta
si su palabra de estado ya trae la misma; es solo un ahorro). Intro,
título y «Select File» como en mupen64plus. Segunda vuelta, tras probarlo
en la PS3 (cielo negro con unas esferas grises en el nivel 1-1): el cielo
se dibuja con TEXRECT dentro del tramo S2DEX (`G_RDPHALF_0`, 0xE4, seguido
de `G_RDPHALF_1` y `G_RDPHALF_2`, como el TEXRECT de F3DEX2), y la HLE lo
saltaba. Ahora sale el cielo azul con nubes y colinas, y en la tarea ya no
se salta ningún comando de S2DEX. El TEXRECT pendiente queda en `raw[]`
del estado (libre en ese tramo) y se dibuja al llegar `G_RDPHALF_2`, sin
locales ni otra llamada: `gfx_dl_s2dex2` está en la cadena más honda de la
pila del SPE 0 y la holgura quedaba en 1344 < 1536 bytes. Para pagar el
código, `f3dpd.c` (el despacho de comandos de Perfect Dark; vértices y
texturas siguen en geom.c y tmem.c con -O2) pasa a -Os: holgura de 1600
(en `b650d80`) a 1984 bytes. Medir en la PS3 que Perfect Dark no pierda
velocidad en el SPE.

Tercera vuelta (enemigos sin cuerpo en el nivel 1-1): con el RSP a bajo
nivel (`--lle=all`, desde el estado de la PS3) los enemigos salían
completos, así que la falla estaba en la HLE del RSP. Kirby orienta sus
enemigos hacia la cámara cambiando la matriz combinada con
`gSPInsertMatrix` (`G_MOVEWORD` `G_MW_MATRIX`), que la HLE de F3D ya hacía
(`gfx_insert_matrix`) y la de F3DEX2 ignoraba: los modelos quedaban
aplastados (el cuerpo dorado no se veía; el enemigo de fuego era una
línea). Smash también lo usa: en su demo cambian detalles de menos de un
píxel (Congo Jungle, Fox). Candidato a la misma causa, sin probar (no
hay respaldo en la Mac): Earthworm Jim 3D, donde el modelo de Jim no se ve
(issue #3).

La secuencia de Kirby (y la del letrero del castillo de Peach en Smash) es
`G_MTX` del objeto, `G_SPECIAL_1` (0xD5) y doce `G_MW_MATRIX` con las
partes enteras y las fracciones de las filas 0–2; la HLE calcula la MVP
pendiente al insertar, así que el 0xD5 no hace falta. Prueba en el host:
`gfx_insert_matrix_billboard`. Medido desde el estado de la PS3 (campo
7852): en la zona del enemigo del bloque, de 17,7 % a 3,7 % de píxeles
distintos del LLE; el enemigo del tronco pasa de los pies azules a la bola
de fuego entera. Contra `dd6a7b0`, con capturas cada 200–250 campos
(intros y guiones de `docs/pads`), quedan idénticos Zelda OoT (casa de
Link con partida nueva), Majora, SM64, MK64, DK64, Banjo, Perfect Dark,
GoldenEye, Conker, Paper Mario, Snap y Stadium 2; en Kirby cambian además
94 píxeles de destellos en la selección de nivel, y en Smash 134 píxeles
del letrero en un escalón de 5 bits (desde un mismo estado, un poco más
cerca del LLE).

**Pokémon Stadium 2: fondo del título como ruido.** El fondo es un JPEG
que decodifica el microcódigo JPEG del RSP: el mismo código que el de
Zelda OoT (0xAF0 bytes idénticos byte a byte; el de OoT está en su RDRAM
desde el arranque), pero la tarea pide 4 KB y lo que va detrás del código
es otra cosa en cada juego. Se reconocía por el CRC de los 4 KB, así que
la tarea de Stadium 2 se terminaba sin hacer nada y quedaban los
coeficientes en la RDRAM. Ahora se reconoce por el CRC del código. Contra
el microcódigo real (`--lle-check-jpeg`): 98,7 % de los píxeles idénticos
y el resto a ±1 en un canal de 5 bits, como con OoT.

Verificación en el host (con la segunda vuelta), píxel por píxel contra `main` con las mismas
capturas cada 150–250 campos (intros y guiones de `docs/pads`): idénticos
Zelda OoT (con sus 75 tareas JPEG), Kokiri, Majora, SM64, MK64, DK64,
Banjo, Perfect Dark, GoldenEye, Conker y Paper Mario. Cambian Kirby, Snap y
Stadium 2 (lo arreglado) y Smash en el castillo de Peach: antes faltaba el
cielo detrás del escenario y quedaba el fondo rojo de la pantalla
anterior, ahora sale como en mupen64plus. El Castillo de Hyrule no se
alcanza en el host, pero su volcado de la PS3 no tiene ningún dibujo con
z constante y prueba de profundidad (y Smash no usa S2DEX ni JPEG): no le
toca nada de esto.
`make test-host` sin fallas (prueba nueva del detector JPEG) y
`tools/test-roms.sh`.

Por confirmar en la PS3: Snap (orden de las capas), Kirby (intro y
selección de archivo), Stadium 2 (pantalla de título) y que no cambie
nada en el resto.
## 2026-10-13 — Copias a la RDRAM: el ancho del VI es el del último campo (rama `arreglo-ancho-vi`)

`readbacks_wanted` (src/rdp/rdp.c, hilo de emulación) copia a la RDRAM, y
la emulación espera, toda color image de 16 bits que es de otro ancho que
el VI o que no cae en los framebuffers recientes. El «ancho del VI» era el
del último origen *agregado* al anillo (`rdp_vi_ring`), y un origen que ya
está entre los 8 recientes no se vuelve a agregar: al volver a
framebuffers ya vistos después de una imagen de otro ancho, el ancho
quedaba en el viejo y cada cuadro parecía una imagen auxiliar.

- **Dónde se vio**: GoldenEye en la presa con el tope de cuadros de 30 (la
  rama de interpolación de cuadros, docs/interpolacion.md): tras las
  pantallas de la misión (440 de ancho) la presa (320) hacía una copia
  esperada por cuadro (15 ms c/u, ~8 ms por campo de la emulación en la
  PS3 real), y el CPU emulado trabajaba un tercio menos sin que bajara el
  tiempo por campo. Con la emulación libre el orden de los orígenes lo
  tapaba por casualidad.
- **Arreglo**: el anillo guarda también `cur_width`, el ancho del último
  campo, y lo usan `readbacks_wanted` (PS3) y `rdp_readback_wanted` (el
  rasterizador del host). Prueba en tests/unit/test_targets.c.
- **Cuánto cambia, en el host** con la regla de la PS3 (nuevo:
  `SALSA64_PS3_RB=1` con `--gfx-frame` cuenta las copias que pediría con el
  ancho nuevo y con el viejo): **Perfect Dark sin tope, en la intro (1800
  campos), de 1167 copias esperadas a 3**; GoldenEye en la presa, 4 y 4
  (con el tope de 30, de 1083 a 4); Zelda (pausa nueva), Super Mario 64,
  Mario Kart, Banjo-Kazooie, Conker (417) y Smash (80), iguales. Volcados
  del host idénticos a los de antes en esos juegos.
- **Por medir en la PS3 real**: Perfect Dark sin tope (deberían desaparecer
  las copias esperadas de la intro).

**Candidato de la v0.2.5 en la PS3 real** (2026-10-13): Opciones recorre todos
los filtros (también «CRT Trinitron», con un menu.cfg que ya lo traía) sin
colgarse; la densidad Fina elegida en Opciones llega al juego y a «Imagen»;
la intro de Perfect Dark hace 0–3 copias esperadas a la RDRAM cada 5 s (antes
una por cuadro) y va a velocidad completa (57–60 campos/s).

## 2026-10-13 — Filtros CRT del autor en el RSX (rama `upbeat-tereshkova`, paquete SLSA00109 «Salsa64 filtros»)

Los filtros CRT originales del autor (máscara de sombra v1.3 y CRT con
cable v1.1: RGB, S-Video y compuesto), portados de sus versiones de
RetroMac, en la parte GPL (`src/frontend/filters/`), sobre una maquinaria
genérica de posprocesado nueva en `lib/rsxkit` (MIT, `rsxkit/post.h`:
superficies intermedias RGBA8 y fp16, programas precompilados con sus
constantes parcheadas, pasadas con constantes por dibujo, reducción con
filtro de caja y marcas de tiempo del RSX). Se eligen en la fila
«Filtro» de Opciones y en los ajustes por juego, después de los
sencillos de VIDEOGAMES SCZ, con el crédito «Filtro original del autor».
Capturas, miniaturas y pruebas siguen sin filtro. Diseño y verificación
en docs/filtros.md.

- **Verificado en RPCS3** contra el original píxel por píxel en doble
  precisión (`tools/crt-ref.c`): la máscara coincide a 1/255 en todo píxel
  salvo el resplandor desde la rejilla (hasta 18/255 en bordes de alto
  contraste); los cables, promedio 0,13–0,26/255 y hasta 36–64/255 en bordes
  nítidos por interpolar las muestras de la señal.
- **Errores del toolchain que costaron vueltas** (docs/rsx-notes.md):
  `rsxSetTimeStamp` de PSL1GHT escribe el reporte en un lugar cualquiera (el
  RSX se colgaba en RPCS3); cgcomp no ensambla bien los códigos de condición
  (`?:`), escribe el color de salida en R0 aunque Cg lo use de temporal y
  codifica `|R0.z|` como `|R0.zxxx|`; Cg corre en silencio y `lerp(1.0,
  vector, k)` trunca el vector. El build de los filtros corrige o rechaza
  los casos de cgcomp (`tools/cg-salida.py`, `tools/fpo-info.py`).
- **Medido en la PS3 real** (`filtros-ciclo`, GoldenEye a 720p y 1080p,
  Perfect Dark y Conker a 1080p, escala 5×; docs/perf-log.md): ms del RSX
  por cuadro a 720p / 1080p, máscara 6,7 / 9,6, RGB 8,4 / 11,9, S-Video
  9,6 / 13,0, compuesto 11,9 / 15,4. Con el juego (5,5–5,7 ms por campo), a
  720p caben la máscara y RGB; a 1080p se presentan 51, 40, 36 y 30 cuadros
  por segundo (la emulación no se frena). Reducir la superficie de 5×
  cuesta 3,1 ms en todos (no es esperar al juego: en pausa da lo mismo).
  Lo dibujado en la consola coincide con la referencia como en RPCS3
  (cables: a 1/255 o menos en el 99,0–99,7 % contra el modelo del RSX), y
  la comprobación de las tablas fp16 en la consola da error 0,0010.
- **Segunda ronda** (paquete SLSA00109 en `build/ps3-4`): quinto CRT, «CRT
  Trinitron (exp.)» (el KV-29 de RetroMac: el tubo de franjas sin cable,
  con el desenfoque del haz a lo ancho; referencia en `tools/crt-ref.c`);
  con un CRT la escala interna no pasa de 3× (lo menor se respeta; en
  «Imagen», «3× (tope con CRT)»); la reducción en dos pasadas (a lo ancho
  y a lo alto; `filtros-reducir` 1 para comparar, y el ciclo alterna una
  vuelta y otra); si la imagen no cambió, solo la pasada final (el RDP
  numera lo escrito en cada destino); capturas solo con `filtros-capturas`,
  copiadas por el RSX a la memoria principal y escritas por otro hilo (la
  imagen ya no se congela ~2 s); «Aplicando filtro…» si preparar un filtro
  tarda más de un par de cuadros; la comprobación fp16, una vez por filtro.
  Arreglado para la v0.2.4: al bajar la escala en vivo con un CRT (5× → 3×
  desde «Imagen») el filtro dejaba de dibujarse: la tabla intermedia de la
  reducción quedaba con el alto de 5× y ya no cabía en la profundidad de la
  pantalla (ahora se rehace del tamaño justo), y un fallo apagaba el filtro
  hasta salir del juego (ahora se reintenta al cambiar la imagen o el filtro).
  Reproducido y comprobado en RPCS3 forzando el cambio en vivo.
- **PS3 colgada con «CRT Trinitron» en Opciones** (arreglado para la
  v0.2.5; visto en la PS3 con la v0.2.4: ocho × desde «Suave», el latido
  quieto en «menú: flip anterior» y la consola entera colgada). La ayuda
  del Trinitron tenía un solo NULL al final y el panel, que dibuja hasta
  dos NULL seguidos, leía como texto lo que el enlazador puso después (las
  tablas de otras pantallas) hasta una dirección mala. En RPCS3, igual al
  octavo ×: «Segfault reading location 3f0f5c9a3eb851ec» en el hilo
  principal (dos floats de la tabla de las ondas tomados por puntero). No
  toca el RSX ni los filtros (cambiar el filtro en Opciones solo escribe
  menu.cfg); «flip anterior» es lo que dice el latido mientras se dibuja
  el menú. Ahora cada ayuda de CRT tiene sitio para sus dos NULL, y
  `menu_options_help` (pruebas del host, con ASan) recorre cada fila de
  Opciones, cada filtro y cada densidad con el panel lleno; en RPCS3, la
  vuelta entera de los filtros en Opciones deja el menú vivo.
- **Densidad de la máscara** (para la v0.2.5, decisión del autor): fila
  «Densidad: Normal / Fina» en Opciones, en los ajustes por juego
  (`densidad = normal|fina|global`) y en «Imagen», solo con «CRT máscara».
  «Fina» = puntos cada 3 px (tríadas de 9 px) en lugar de cada 4, en filas
  de 3, radio 1,2 (elegido por el autor entre 4, 3, 2,5, 2 y 1 px comparados
  con `tools/crt-ref.c`: 2,5 no cae en píxeles enteros y da franjas; 2
  pierde los puntos redondos; 1 pierde los puntos). El brillo se conserva.
  En la PS3 (primer paquete) la «Fina» de Opciones no llegaba al juego: el
  menú reescribía menu.cfg solo si cambiaba una de las claves de antes, y la
  densidad no estaba en la lista. Ahora se compara el texto entero de
  menu.cfg (`menu_settings_cfg_differs`, con prueba en el host) y el log
  dice el filtro y la densidad al guardar Opciones y la densidad al guardar
  los ajustes de un juego.
  **Medido en la PS3** (GoldenEye en la presa a 1080p): máscara 7,4, RGB
  9,4, S-Video 10,6, compuesto 13,2 y Trinitron 10,1 ms por cuadro, todos
  los campos presentados (antes, a 5×, 30–51 por segundo); reducir 3,12 →
  1,2 ms y el render del juego 5,6 → 2,1 ms. GoldenEye casi no repite
  imagen en la presa (una tarea gráfica por campo): el reuso ahí casi no
  aplica.
- **Tercera ronda** (paquete en `build/ps3-5`): con `filtros-spe`, la señal
  y la decodificación de S-Video y compuesto en dos SPEs (src/spu/crt; el
  RSX deja la imagen en la memoria principal con una etiqueta, los SPEs
  dejan la tabla en fp16 con las suyas y el RSX las espera antes de
  linealizarla; etiquetas genéricas nuevas en rsxkit); el ciclo alterna SPE
  y RSX por vuelta y el log da la espera del RSX, la ida y vuelta y el
  trabajo del SPE. En RPCS3 da lo mismo que el RSX contra la referencia.
  **Medido en la PS3**: S-Video 0,45 ms menos, compuesto 0,5 ms más (el
  SPE tarda ~3 veces lo estimado y el RSX lo espera); queda apagado por
  omisión. Lo probable es la latencia de la cadena de FMA del filtro (ver
  docs/perf-log.md).
- **Propuestas para bajar el costo** (con estimados en docs/perf-log.md;
  la segunda ronda ya trae el tope de escala, las dos pasadas y el reuso, y
  el SPE va en un paquete aparte): escala 2× cuando hay un CRT (~5–6 ms menos por campo;
  reducir mide 3,12 ms a 5×, 1,73 a 3× y 0,13 a 1×; en una escena
  sintética 2× casi no se nota, 1× sí), reducir en dos pasadas o con las
  regiones con tiles del RSX, no rehacer el filtro en los campos que
  repiten imagen (GoldenEye y Perfect Dark dibujan 20–30 por segundo) y la
  señal y decodificación de los cables en un SPE (1–3,5 ms menos, solo
  S-Video y compuesto).

**Candidato de la v0.2.4 en la PS3 real** (2026-10-13): el primero apagaba
el CRT al bajar la escala en vivo (5× → 3× por el tope, desde «Imagen»):
«no se pudo reducir la imagen … factor 3». La tabla intermedia de la
reducción en dos pasadas se había quedado del alto de 5× y no cabía en la
profundidad; además un fallo dejaba el filtro apagado hasta salir del
juego. Arreglado (tabla del tamaño justo, reintento al cambiar algo) y
confirmado con el segundo candidato, junto con Smash (KO y Hyrule Castle).

## 2026-10-12 (cierre del día) — Para retomar

Publicadas hoy: **v0.2.0** (DK64 al 100 %, Majora con FlashRAM y efectos,
desenfoque de Perfect Dark, Star Fox, botón PS), **v0.2.1** (cámara de DK64
por el borrado de la Z en la RDRAM, Navi y antorchas de Zelda, tarea JPEG en
un SPE vectorizada) y **v0.2.2** (código compacto del recompilador: presa de
GoldenEye +6…+12 % de campos/s; Z en los dos gatillos con `Z = L2 R2`;
muestreo por tiempo con el archivo `muestreo`). Notas en `CHANGELOG.md`.

En la PS3 del autor quedan: `controles.cfg` con `Z = L2 R2`; los perfiles de
13 juegos en `perfiles/`; todas las banderas de prueba en `.off`.

En curso:
- **Recompilador RSP → SPU** (rama aparte, paquete SLSA00107 «Salsa64 RSP»):
  Diddy Kong Racing ya dibuja en la PS3 con el microcódigo real traducido
  (80 ns por instrucción del RSP, 2,4× el intérprete), pero va al ~13 %:
  mandan las salidas al intérprete por COP0 (~10 000 por tarea), los cortes
  con el registro de tramos lleno y el RDP en el PPE. Error abierto: al salir
  del juego con el LLE activo la app se queda en el cierre (el paro del SPE
  espera). Después: LLE a resolución alta, Indiana Jones (MusyX).
- **Caché del PPE, ronda 2** (rama aparte, paquete SLSA00108 «Salsa64
  caché»): medición fina de las esperas, sección fría fuera del cuerpo y
  predicción guiada por los perfiles (trazas por el sucesor dominante, pistas
  de salto, precarga del sucesor, pila de retorno). Plan en
  `docs/cache-ppe.md`.

Pendientes (censo del día y reportes): Super Smash Bros. se congela en el
primer KO (arreglado en el host el 2026-10-13, por confirmar en la PS3;
entrada de abajo); Banjo-Tooie
en la intro (host; desafío del CIC 6105 de la PIF ya implementado); F-Zero X ya arranca; Kirby 64 y Pokémon Stadium 2 con
texturas con ruido (estado de Stadium 2 en la ranura 0); Pokémon Snap con
texturas estiradas; sombras cuadradas de Conker (su espera de ~25 s al
arrancar, en la entrada del 2026-10-13); luces puntuales de Majora (la luz salta
entre fuentes al caminar: probable falta de luces puntuales en la HLE de
F3DZEX); cadencia de la presentación (campos saltados cuando la emulación va
sobrada); Doom 64 al ~40 % (instrucciones interpretadas); estados
comprimidos; Rumble Pak y Controller Pak; Fase 5 (medir el margen del RSX a
5×, multimuestreo para GoldenEye, filtros CRT).

## 2026-10-13 — Smash: el KO se colgaba porque la copia a la RDRAM pisaba los márgenes del z-buffer (rama `nifty-knuth-a7c921`, paquete SLSA00113 «Salsa64 Smash»)

La hipótesis de la entrada del 2026-10-12 (el CPU lee la silueta como
profundidades) era falsa: las «273 lecturas por campo desde `800c7840`» son
búsquedas de instrucciones (pa `0c7840` = `0a3940` + `23f00`, justo después
de las 230 filas del z-buffer, dentro del alto conjeturado de 240). El CPU
nunca lee el z-buffer.

Lo que pasa en el KO (tareas 1094–1133 de la demo, campos 1116–1155): con
el z-buffer `0a3940/320` como color image, el juego dibuja un rectángulo
negro de toda la pantalla (1 ciclo, PRIM 0x000000ff, blender IN·A + MEM·(1−A))
y una malla blanca encima: una máscara de profundidad (negro = cerca,
blanco = lejos) para que la escena solo se vea dentro de la malla. Las
tijeras son 10..310 × 10..230 y **en los márgenes del z-buffer el juego
guarda datos**: punteros y código en tramos de 40 bytes (los 20 del final
de una fila y los 20 del principio de la siguiente), más las 10 primeras
filas. El RDP nunca escribe ahí. La copia esperada escribía filas enteras
desde la superficie (cebada con la RDRAM) y, al pasar por RGBA5551 con alfa
1, ponía a 1 el bit bajo de cada media palabra: `03e00008` (jr ra) →
`03e10009`. Comparando la RDRAM con y sin la copia (`--ram-dump` con
`--gfx-frame`, que es lo que activa el rasterizador del host), las únicas
diferencias en todo el campo 1395 son esos márgenes; unos 240 campos
después del KO el juego deja de mandar tareas gráficas.

Arreglo (criterio general, no por juego): toda copia a la RDRAM (copia
esperada o sin esperar, espejo chico, espejo del framebuffer; host y PS3)
escribe solo lo que el RDP pudo escribir: la unión de la caja de los
vértices de cada dibujo recortada por sus tijeras (`struct rdp_box`,
`rdp_box_add` en targets.c; si algún vértice tiene w ≤ 0, todas las
tijeras). Antes se copiaban filas enteras hasta la más baja dibujada. El
espejo del framebuffer usa la caja acumulada desde el último espejo
(`rdp_target.box`, se vacía en `rdp_targets_mirrored`), así también vale
para el que se copia al presentarlo. `rsxkit_pixels_565_to_5551` convierte
uno a uno hasta alinear el destino, para seguir de a cuatro píxeles cuando
la caja empieza en una columna cualquiera.

Medido en el host: la demo de Smash (sin control) corre entera, 13 015
tareas en 14 000 campos con 315 copias a la RDRAM (antes se paraba en 1373
tareas, campo ~1395); prueba nueva en test_raster
(`raster_copy_inside_scissor`). Sin cambios píxel por píxel contra main en
DK64 (4000 campos), Perfect Dark (intro con el desenfoque, 2800), Mario
Kart 64 (`mk64-luigi.pad`), Banjo (`banjo-casa.pad`), Conker (5000 campos,
417 copias), GoldenEye (`goldeneye-dam.pad`) y la pausa de Zelda OoT
(`zelda-casa-nueva.pad` con START en los campos 17100, 17500 y 17800:
la copia de `fbufSave` y los búferes de Link, 404 copias, 14 cuadros
iguales; `zelda-pausa-nueva.pad` ya no llega con los tiempos de hoy: se
queda en el nombre). test-host y
tools/test-roms.sh pasan. Por confirmar en la PS3 real: Smash y Smash Remix
hasta varios KO sin `fbfx.cfg`, y la pausa de Zelda, la cámara de DK64, Navi
y las antorchas de OoT, el túnel de MK64, Majora, Perfect Dark y Conker.

## 2026-10-13 — Smash: texturas estiradas en el Castillo de Hyrule (misma rama, paquete SLSA00113)

El autor las vio en la PS3, con y sin `fbfx.cfg`, en Smash y en Smash Remix
(los ladrillos de las torres y del muro, alargados). El volcado de la consola
en ese escenario (`--raster-dump`) da en el host la misma imagen: es de la
HLE, no del RSX. Las texturas de las torres son CI4 de 32 × 32 (y otra de
64 de periodo) en tiles de 160 × 160 y 128 × 256 con **clamp y máscara**:
el RDP repite el periodo dentro de la tile. La HLE las decodificaba al
tamaño de la tile con la máscara aplicada, pero eso no cabe en el área de
decodificación (`GFX_TEX_MAX`, 32 KB: 160 × 51 y 128 × 64 texels), así que
se cortaban filas y, con el clamp, la última fila se estiraba por el resto
de la torre. Ahora (`tex_size` en tmem.c), si no cabe, ese eje se
decodifica en un periodo de su máscara y va con GFX_WRAP_REPEAT (o
GFX_WRAP_MIRROR con espejo): dentro de la tile es exacto; solo más allá de
su borde, donde el RDP fija el último texel, aquí se sigue repitiendo. El
caso de espejo con hasta dos periodos (el telescopio de Majora's Mask,
MIRROR_ONCE) no cambia. Prueba nueva en test_gfx
(`gfx_tile_clamp_mask_period`).

En el host no hay cómo llegar a Hyrule (la demo no pasa por ahí), pero la
demo de Smash tenía lo mismo: en la intro de Kongo Jungle unas franjas
amarillo-verdes estiradas abajo a la izquierda ahora son hojas y pasto.
Contra la rama anterior, DK64, Perfect Dark, MK64, Banjo, Conker y
GoldenEye salen iguales píxel por píxel; solo cambian cuadros de Smash.
Por confirmar en la PS3: el Castillo de Hyrule en Smash y en Smash Remix.

**Confirmado en la PS3 real** (2026-10-13, paquete SLSA00113, sin fbfx.cfg):
Smash y Smash Remix pasan varios KO; Hyrule Castle se ve bien. Sin
regresiones en la pausa de Zelda, el túnel de Luigi Raceway, la pausa de
Majora, el desenfoque de la intro de Perfect Dark y la pausa de Conker. Los
tirones de ~125 ms al caminar por el árbol Deku de Zelda también salen en
v0.2.3 (código nuevo que se traduce al cargar enemigos y salas). Sale en
v0.2.4 junto con los filtros CRT.

## 2026-10-13 — Conker en negro tras «For Mature» desde la v0.2.1, y sus 29 s de negro al arrancar (rama `objective-bun-2517c7`, paquete SLSA00112 «Salsa64 Conker»)

Síntoma (PS3 real con v0.2.1, v0.2.2 y main; igual en la Mac): después de
la pantalla roja «For Mature Audiences Only» el juego queda en negro a
0 fps. En la v0.2.0 llegaba al juego, pero siempre con 25–30 s de negro al
arrancar que la consola no tiene. Bisección con el intérprete del host:
54798d9 (el borrado de la z image llega a la RDRAM). En la Mac,
`--fields 3600`: 1 tarea gráfica en main, 902 en la v0.2.0.

Diagnóstico:
- **El cuelgue.** La primera tarea gráfica borra la z image (0x0E9D80,
  292 de ancho) con un FILLRECT de 217 filas (0–216) y el scissor en
  (2, 0)–(290, 216). El RDP recorta el relleno con el scissor (10.2, borde
  inferior derecho exclusivo) y llena las filas 0–215; la HLE mandaba
  `GFX_OP_ZCLEAR` con el rectángulo sin recortar y, desde 54798d9,
  `salsa64_gfx_rdram_fills` escribía también la fila 216. Ahí (0x108A40) el
  juego guarda nodos de una lista enlazada (cabeza en 0x800380B4): el hilo
  principal la recorre, toma 0xFFFCFFFC como siguiente nodo, falla la TLB
  en 0xFFFD0004 y libultra lo detiene; quedan el audio y el hilo ocioso.
  Los datos con pinta de comprimidos que había en la región antes de la
  tarea son restos de la carga (un DMA de 92 000 bytes en el campo 1, ya
  descomprimido): el RDP también los pisa y el juego no los vuelve a leer.
- **Los 29 s de negro.** El hilo principal duerme en `osContInit`, que
  espera a que `osGetTime()` llegue a 0,5 s. Con el arranque HLE el juego
  entraba con Count = 0 y llegaba ahí a los 0,097 s. Además, el
  `osSetTimer` de su libultra, cuando un temporizador nuevo queda delante,
  no descuenta lo que ya corrió del que iba primero, y el planificador de
  Conker arma uno de 20 000 cuentas cada dos campos para lanzar la tarea de
  audio (200 000 si el AI está lleno, cosa que en estado estable no pasa:
  el juego acorta el cuadro de audio si al encolarlo quedan ≥ 249
  muestras). El de `osContInit` avanzaba así 20 000 cuentas cada dos
  campos: 0,4 s de espera se volvían ~29 s (con AI_STATUS forzado a lleno,
  la espera bajaba a ~3 s, lo que confirmó el mecanismo). En la consola no
  espera: el IPL3 de Nintendo pone Count en 0 al empezar (visto corriendo
  el IPL3 del cartucho en el host) y tarda unos 500 ms en frío
  (n64brew.dev: IPL3), así que al llegar a `osContInit` ya pasó el plazo.

Arreglo:
- `fillrect` (`src/rsp/gfx/rdp.c`): el `GFX_OP_ZCLEAR` sale recortado con
  el scissor (X de `XH/4` a `⌈XL/4⌉`, igual en Y, la misma cuenta que usan
  los dibujos) y, si queda vacío, no se manda. Vale para la RDRAM, el RSX y
  el rasterizador del host. Prueba: `gfx_zclear_scissor` (el caso de
  Conker, la fila 216 intacta, y un scissor vacío).
- Arranque HLE (`src/core/boot.c`): con un IPL3 de Nintendo reconocido el
  juego entra con Count = 23 437 500 (0,5 s a 46,875 MHz). El camino ELF
  (IPL3 de libdragon) y las ROMs sin IPL3 reconocido siguen en 0
  (`test_boot`).

Efecto en otros juegos (host):
- El recorte solo cambia los borrados de Conker (todos, de (2, 0)–(291,
  217) a (2, 0)–(290, 216)). Con un aviso temporal en `fillrect`: ninguno
  en DK64 (5000 campos), Zelda con `zelda-oot.pad` (19 400 campos) ni con
  los guiones de MK64, SM64 y Banjo. Desde main, la RDRAM de DK64 y de
  Zelda en el campo 1500 queda idéntica con el recorte.
- El Count del IPL3: los juegos que dormían en `osContInit` arrancan
  antes, como en la consola. Primera tarea gráfica (campo): Zelda OoT
  30 → 10, Mario Kart 64 29 → 4, GoldenEye 57 → 45, Banjo-Kazooie 57 → 46,
  Smash 41 → 12, Mario Party 128 → 100, DK64 178 → 176; Perfect Dark y
  Paper Mario igual. **Los guiones por campo de docs/pads siguen sirviendo**
  (la primera pulsación que el juego espera absorbe el adelanto), main
  contra la rama: MK64 y SM64 terminan con la imagen idéntica; GoldenEye
  juega en la presa en el campo 6990, Banjo sale de su casa (campo
  17 500) y Zelda con `zelda-casa-nueva.pad` está en la casa de Link (campo
  15 000), con el personaje apenas en otro lugar. (`zelda-pausa-nueva.pad`
  se queda en la pantalla del nombre en las dos: ya no encajaba con los
  tiempos de main.) Lo que se toma en un campo fijo contado desde el
  arranque (`--state-save C:…`, anotaciones de perf-log) cae hasta 30
  campos más adelante en el juego; cargar estados ya guardados no cambia.

Verificado:
- [x] Host: Conker con 1740 tareas en 3600 campos: «For Mature Audiences
      Only» hacia el campo 120, la pantalla de Dolby hacia el 400, el logo
      de Nintendo hacia el 600 y la taberna en el 3600.
- [x] `test-host` 482/482 (Docker, ASan/UBSan); `test-roms` (nemu64-test
      en su línea base de 433 / 136 con LLE, vitest, savetest, gfxtest ×3,
      estados, audiotest); `test-rec` (unitarias en los cinco modos
      482/482, nemu64-test en su línea base con --rec, vitest, estados).
- [x] Paquete «Salsa64 Conker» (SLSA00112) compilado sin avisos.
- [ ] **PS3 real** (paquete «Salsa64 Conker», SLSA00112): Conker hasta
      jugar y sin el negro largo; DK64 con la cámara estable; Zelda con
      Navi y las antorchas; que los demás arranquen como antes.
**Confirmado en la PS3 real** (2026-10-13, paquete SLSA00112): Conker pasa
«For Mature» al momento, sin el negro largo, y llega a la taberna; los
estados guardados cargan. Sin cambios en DK64 (cámara estable), Zelda OoT
(resplandor de Navi y de las antorchas), Mario Kart 64, Mario Party,
GoldenEye y Banjo-Kazooie, que ahora arrancan antes. Smash sigue con su
congelón conocido en el primer KO (aparte). Sale en v0.2.3.

## 2026-10-13 — «Imagen» en el menú de pausa: filtro, aspecto, recorte y escala interna en vivo, y escala por juego

Pedido del autor tras la primera prueba de los extras en la PS3 (carátulas y
vidas infinitas de SM64 bien). En el menú de pausa, una fila «Imagen» abre
un cuadro abajo a la izquierda, sin oscurecer el juego (pausectl, IM_*):

- **Filtro, aspecto y recorte** se aplican al momento (`view_apply`) y se ven
  en el cuadro en pausa: `salsa64_rdp_present_again` acepta ahora la vista y
  el recorte (`again_set` guarda la región sin recortar y recalcula el 1:1),
  y las líneas de barrido siguen el rectángulo nuevo. Con una imagen de la
  RDRAM, el recorte se ve al seguir.
- **Escala interna**: se pone al seguir jugando, con la emulación quieta en
  el límite del campo (`image_scale_apply`, antes de soltar `pause_req`):
  `salsa64_rdp_set_scale` suelta los destinos y el juego los rehace a la
  escala nueva en su cuadro siguiente. Hacerlo con la imagen en pausa la
  dejaba en negro. Riesgo: un cuadro raro en los juegos que leen el anterior
  (copias descartadas); sin probar en la PS3.
- **Guardar para este juego**: `juegos/<ROM>.cfg` con lo que se ve; lo que
  coincide con lo global y el juego no tenía propio queda «global».
- **Escala por juego** (`escala = auto|1…5|global`, también en la página de
  SELECT del menú): `rdp_scale_set` la pide al abrir el juego (sin juego
  corriendo, como «Aplicar ahora») y al cerrarlo vuelve la de Opciones. Las
  ROMs de prueba respetan la escala del juego (verifican a cualquier escala)
  y siguen forzando aspecto, filtro y recorte.

**Confirmado en la PS3 real** (2026-10-13, GoldenEye y Zelda OoT a 1080p, en
un paquete de prueba junto con los filtros CRT de la Fase 5). En vivo desde
la pausa: los filtros sencillos y los CRT, 4:3 ↔ 16:9, recorte del borde
(8/8/6/6 y 16/16/12/12), escala 5× ↔ 3× al seguir jugando, «Guardar para este
juego» y la vuelta a lo de Opciones en otro juego. Sin cuadros raros al
cambiar la escala.

## 2026-10-13 — Contribución de VIDEOGAMES SCZ: menú de pausa, trucos, carátulas, aspecto, filtros sencillos, ajustes por juego e idiomas (paquete SLSA00110 «Salsa64 extras»)

VIDEOGAMES SCZ (creador de contenido de PS3) mandó sus extras hechos sobre
main `da2adcd` (36 archivos, +6043/−153 líneas) con permiso para integrarlos
con su crédito. Se revisaron como código ajeno (sin ejecutar nada de lo que
venía fuera del árbol del proyecto) y se aplicaron sobre v0.2.2 con una
fusión a 3 vías; los choques fueron solo de contexto (`main.c` junto al
muestreo del hilo 1, la presentación 1:1 por tamaño lógico de `rdp.c` y las
suites de las pruebas). Qué trae (docs/uso-ps3.md):

- **Menú de pausa** (START + SELECT): Continuar, Guardar/Cargar estado,
  Ranura, Trucos, Mostrar FPS y Salir; sustituye al cuadro «¿Volver al
  menú?», que queda como confirmación de «Salir» si «Confirmar al salir» está
  en sí. La pausa es la misma de siempre (`pausectl`, motivos `PAUSECTL_MENU`
  y `PAUSECTL_PANEL`), así que el botón PS encima sigue igual.
- **Trucos** (`cheats.c`): `.cht` de Project64 y de RetroArch, GameShark
  80/81/A0/A1/F0/F1/D0–D3/50, página en el menú (□) y en el juego (SELECT +
  □). Se aplican al final de cada campo en el hilo de emulación.
- **Carátulas** (`cover.c`): PNG propio (desinflado según puff, todos los
  tipos, Adam7) reducido a 320 × 448 y subido a una ranura de imagen nueva
  del 2D de rsxkit; búsqueda por nombre, código o nombre parecido de la misma
  región (`namematch.c`).
- **Aspecto** 4:3/16:9 (16:9 no usa la presentación 1:1), **filtros
  sencillos** (Suave, Nítida, Líneas suaves/fuertes, dibujadas con el 2D),
  **ajustes por juego** (`juegos/<ROM>.cfg`) y **6 idiomas** (`tr()`, tabla
  ordenada en `i18n.c`).
- **Descargas** de carátulas y trucos (`download.c`, libhttp/SSL en un hilo).

Cambios al integrar:

- **Seguridad del PNG y de los `.cht`**: pruebas con ASan/UBSan y un fuzzer
  nuevo (`tests/fuzz`, mutador propio sobre PNG y `.cht` sintéticos, cada
  truncado, longitudes de trozo enormes, cabecera zlib y CRC dañados; 2
  semillas × 3,25 M entradas sin fallos). Encontró que el lector de `.cht`
  leía más allá del texto (`hex_token` y `enable`): no pasaba en la consola
  porque `read_file` deja un `'\0'`, pero ahora todo respeta el largo. La
  segunda pasada del PNG se para en IEND y comprueba cada largo; topes de 8 MB
  de archivo y 24 MB desinflados (antes 12 y 40) y búfer del tamaño del
  archivo; las rutas de los trucos que no caben ya no se cortan en silencio.
- **Los trucos no tocan el anillo de DMA**: escriben como el CPU
  (`salsa64_bus_rdram_patched`: solo el recompilador descarta código), para
  que un truco que cambia muchas direcciones no desplace del anillo de 64 los
  DMA de verdad que protegen las copias del RDP.
- **Descargas apagadas y ocultas** salvo `descargas.cfg` con `activar = si`
  (la PS3 no habla el TLS de GitHub y los certificados no se verifican; sin
  probar en la consola). La red ya no se baja al terminar (`netDeinitialize`
  le cortaba el log UDP al logger), los módulos se cargan solo si faltaban
  (también `HTTP_UTIL` y `NETCTL`, que no se cargaban) y no se pisa un
  archivo existente. Solo escribe en `covers/` y `cheats/` (nombres sin
  carpetas, sin `..` ni punto inicial).
- **Fila «Filtro» lista para los filtros CRT** de la otra rama: tabla única
  (clave, nombre y crédito por filtro) y el crédito bajo el valor elegido en
  el panel («Filtro de VIDEOGAMES SCZ»); el texto de ayuda del filtro se
  salía del panel y se acortó.
- Las líneas de barrido van en el mismo lote del 2D que el aviso y el
  contador (antes, un `rsxkit_ui_begin` más por cuadro).
- Las pruebas automáticas (vitest, gfxtest, audiotest) fuerzan 4:3 además
  del filtro suave.
- La pila del SPE 0 no cambia (holgura 3 520 bytes).
- La carátula deja sitio abajo para «● Último abierto» (en 16:9 lo pisaba).

Pruebas: 481 unitarias del host con ASan/UBSan; `tools/test-roms.sh` sin
regresiones; `tools/test-phase2.sh` en RPCS3 bien, con una carátula y un
truco sintéticos en su `dev_hdd0` (la carátula se dibuja en el menú y
gfxtest pasa con el truco activo, que escribe fuera de la RDRAM; las
descargas dicen «apagadas»); build de la PS3 sin avisos. Riesgo menor: la
búsqueda por nombre parecido acepta un archivo cuyo nombre está dentro del
de la ROM (la variante F3D de gfxtest tomó la carátula y los trucos de
gfxtest); los trucos igual hay que activarlos a mano.

**Licencias.** Los archivos nuevos son GPLv2+. `cover.c` sigue a puff.c
función por función: se trata como versión modificada y lleva el aviso de
zlib (`GPL-2.0-or-later AND Zlib`; docs/THIRD_PARTY.md). Sus cambios en
`lib/rsxkit` (ranura de imagen del 2D: `ui.h`, `ui.c`, `ui_geom.c`) quedan
bajo **MIT** como el resto de la biblioteca, con su conformidad (dada por
correo el 2026-10-13); nada impide publicarlos.

Falta en la PS3 real (paquete SLSA00110 «Salsa64 extras», al lado del
oficial): menú con carátulas, un truco simple, aspecto 16:9, filtros
sencillos, menú de pausa y su salida, cambio de idioma, ajustes por juego,
botón PS dentro del menú y del juego, y dos o tres juegos para descartar
regresiones.
## 2026-10-13 — Fastmem en el PPE: viabilidad, diseño y prototipo en el host (rama `quirky-solomon`, paquete SLSA00111 «Salsa64 fastmem»)

Objetivo (docs/fastmem.md): que un acceso de la N64 a la RDRAM sea una
carga o un almacenamiento del PPE sin la prueba de segmento (5 palabras
calientes por acceso en el código compacto). Hecho hoy:

- **Censo de accesos a datos** en el intérprete (`-DSALSA64_MEM_CENSUS`, 11
  juegos): KSEG0 es el 97,9–99,97 % en todos, KSEG1 ≤ 0,6 %, por la TLB
  solo Perfect Dark pesa (2 %, la página de 4 MB de 0x70000000) y GoldenEye
  0,1 %; ningún acceso no alineado, ninguna colisión con una ventana doblada.
  Por eso fastmem es solo para KSEG0/KSEG1, con una ventana de 512 MB
  (`va & 0x1FFFFFFF`: KSEG1 cae sola en la RDRAM, sin espejo) y **no hace
  falta el espejo de la TLB** (además lv2 mapea en 64 KB y la N64 en 4 KB).
- **Prueba de viabilidad para la PS3** (`tests/phase0/fastmem`, paquete
  SLSA00111): regiones de sys_mmapper, mapeo, espejo, permisos, costo por
  lectura, y los fallos de página atrapados desde otro hilo con la
  notificación de lv2 (327), el contexto (58) y la reanudación (57),
  incluido parchear código generado que falló. Validada en RPCS3 (reservas,
  mapeo y datos; ahí los fallos no se producen). **En la cola de la consola**
  (pedido a main; protocolo: relanzar hasta «RESULTADO fastmem»).
- **Prototipo en el host** (`SALSA64_REC_FASTMEM=1`, commit 089377a):
  emisión sin prueba con la copia del acceso en la sección fría, tabla de
  sitios → camino frío por generación, el fallo parchea `b camino_frío`
  desde el manejador de SIGSEGV (qemu-user entrega el pc), apagado si una
  entrada de la TLB choca con la ventana. vitest OK; nemu64-test 433 + 7
  esperadas (no alineados y direcciones sin extensión de signo: excepciones
  que fastmem no lanza; `tests/roms/nemu64-test.fastmem.fallas` y
  `tools/test-rec.sh` las contemplan). **Expansión en GoldenEye: calientes
  8,16 → 6,87 palabras por instrucción (−16 %)**, frías 7,40 → 10,06; 282
  fallos parchados en 80 s de consola. Lockstep de GoldenEye hasta la presa
  y Perfect Dark: en curso.
- **Lado de la PS3** (`src/platform/ps3/fastmem.c`, archivo `jit-fastmem`):
  ventana y RDRAM por sys_mmapper, hilo ayudante con la cola de eventos,
  autoprueba al abrir el juego (localiza el pc en el contexto), estadísticas
  cada 5 s. Sin probar en la consola: depende de lo que diga la prueba de
  viabilidad (que lv2 avise, que el contexto traiga el pc, que los SPE
  hagan DMA sobre la ventana).

- **Corrección de main (tarde)**: en la PS3 hubo una sola corrida y 327
  (`sys_mmapper_enable_page_fault_notification`) devolvió EINVAL desde la
  primera llamada (los arranques que «llegaron a fallo_parche» eran mi
  RPCS3 en el mismo receptor UDP). fastmem2 (`build/ps3-2/`) prueba 14
  variantes de la llamada con el rc de cada una; en la cola de la consola.
- **Base en caché** (docs/fastmem.md §8; sin fallos, exacta, encendida por
  omisión, `jit-sin-grupos` / `SALSA64_REC_NO_GROUP=1` para el A/B): la
  prueba de KSEG0 una vez por registro base con margen (`emit_group_head`)
  y cada acceso del grupo `lwz d, disp(r7)`; T_HI = r7 como base, guardado
  en `cpu->rec_tb` alrededor de las llamadas a C; si la `ktlb` no traduce,
  el acceso de la cabeza va por C (anota la página) y se vuelve a probar.
  Host: **GoldenEye calientes 8,16 → 7,75 (−5 %), Perfect Dark 6,84 → 6,45
  (−6 %)**; unitarias, nemu64 (433 exacto, lockstep entero), vitest, PD y
  GoldenEye en lockstep sin diferencias. Tres errores que encontró el
  lockstep: grupos solapados (un solo r7), la base recompuesta con la
  doblada tras C (mapeado por la TLB → puntero fuera de la RDRAM), y la
  carga residente de la FPU desde T_Y.

- **PS3, corrida A** (paquete d939fb46): GoldenEye presa 16,63 ns/instr
  (v0.2.2 16,0–16,3: sin ganancia); Perfect Dark se congeló en la segunda
  escena: la cabeza de un grupo quedó en el delay slot de un BC1FL (el
  filtro de alineación dejaba fuera al primer acceso) y un miembro del
  camino no tomado usó r7 sin valor. Arreglado y verificado (PD 9 G con
  enlaces y en lockstep, GE lockstep); FCR31 en T_V (los grupos ya no se
  cortan en COP1): GE −6 %, PD −7 % de calientes. Cierre forzado del
  frontend si la emulación no responde en 5 s (para los SPE y sale). Rama
  rebasada sobre v0.2.4.
- **FPU compacta** (docs/fastmem.md §9, `src/cpu/rec_emit_fpurtn.h`): las
  operaciones de la FPU como 38 rutinas compartidas del preámbulo (el
  mismo código en línea con los desplazamientos en registros); el sitio son
  5–6 palabras en vez de 35–40. Host: **GoldenEye calientes 8,16 → 5,54
  (−32 %), Perfect Dark 6,84 → 5,22 (−24 %)** (con los grupos); exacta en
  todo (unitarias, nemu64 y lockstep, PD y GE en lockstep). Interruptor
  `jit-fpu-en-linea` / `SALSA64_REC_FPU_INLINE=1`. **PS3** (paquete
  `build/ps3-4`, sin banderas): GoldenEye presa 15,66 / 15,54 ns/instr
  (media / mediana; v0.2.2 16,03 / 16,30: −2…−4 %), Perfect Dark segunda
  escena 14,1–14,7 contra ~15,4 (−5…−8 %). Menos que la huella del host
  prometía (el `bl`/`blr` por operación y lo que no es el PPE); PD se ve
  bien, GoldenEye sigue con atoros en lo pesado. Rama rebasada sobre
  b890a7b (versión visible, gráficos, Kirby) para integrar tal cual.

Pendiente: la consola (fastmem2; el A/B estricto con `jit-sin-grupos` y
`jit-fpu-en-linea` en el mismo paquete); después registros vivos entre
bloques y la FPU 1 a 1 (pasos 3 y 4).

## 2026-10-13 — Recompilador: código compacto, 20,2 → 15,6 palabras de PPC por instrucción MIPS (rama `sleepy-mestorf`, paquete SLSA00106 «Salsa64 compacto»)

Sigue a la medición del perfil persistente (2026-10-12): juntar los bloques
calientes no cambió el costo del nativo; lo que manda es cuántas líneas de
caché toca cada instrucción emulada (~21 palabras de PPC por instrucción
MIPS en los bloques calientes de GoldenEye). Esta rama ataca esa
expansión (docs/rec-ppe.md, «Código compacto»; docs/perf-log.md, «Código
compacto»):

- **Prueba de KSEG0 en 4 palabras** por acceso a memoria (antes 8): `oris`
  del bit 29 + `addis 0x6000` dejan solo el desplazamiento si la dirección
  es de KSEG0/KSEG1 dentro de la RDRAM; un `rldicl` comprueba segmento,
  tamaño y alineación de una vez. Con desplazamiento 0, sin `addi`.
- **Un camino lento por bloque**: una sola rutina del preámbulo
  (`slow_rtn`: cargas, almacenamientos, instrucción al intérprete y
  excepciones, según r6) y un trampolín por bloque con su pc y su n; cada
  sitio pasa una palabra. Lo que antes eran ~19–25 palabras por sitio (FPU,
  COP0, desbordamientos, accesos) son 4–6.
- **Cadena de escritura**: los registros con valor al salir por un camino
  lento se escriben con una sola secuencia de `std` por bloque (entrada
  según la instrucción), en vez de repetir los sucios en cada sitio.
- **Enlaces de 3 palabras**: el destino va como dato después del `bl
  exit_link`.
- `SALSA64_REC_OPT_WIDE` (`jit-ancho` en la PS3, `SALSA64_REC_WIDE=1` en
  el host) repite el código de antes para el A/B con el mismo binario.
- `tools/rec-expansion.py` mide la expansión del volcado
  (`SALSA64_REC_DUMP=1`, ahora con las palabras calientes por bloque)
  pesada por ejecuciones (`SALSA64_REC_PROF_TOP` lista más bloques).

Medido en el host (GoldenEye hasta la presa, pesado por ejecuciones):
**20,2 → 15,6 palabras por instrucción (−23 %)**; calientes 9,3 → 8,2
(−12 %), frías 11,0 → 7,4 (−33 %); volcado 17,2 → 13,2 MB. Mismo recorrido
exacto (mismas cuentas del perfil).

Corrección: unitarias 437/437 en los cinco modos (plain, far, pin, eager,
FPU residente), nemu64-test 433 fallos (línea base) con y sin fijos, vitest,
estados guardados, y lockstep de nemu64-test, vitest, Perfect Dark (3 G
ciclos) y GoldenEye hasta la presa (7,5 G ciclos, 204 M salidas comparadas)
**sin diferencias**; con main fusionado (perfil persistente), de nuevo
nemu64-test, vitest, Perfect Dark y GoldenEye sin diferencias
(docs/rec-ppe.md, «Resultados»). Dos errores que encontraron las pruebas: `addi r0, 0, -4`
(r0 como base vale 0 en el PPE) y la cadena entrando por el extremo
equivocado.

**PS3 real** (2026-10-13, misma noche; detalle en docs/perf-log.md,
«Código compacto»; A/B con `jit-ancho`, el código de antes en el mismo
paquete): GoldenEye en la presa **17,26 → 16,30 / 16,03 ns por instrucción
emulada (−6…−7 %)** en las ventanas pesadas, 47,5 → 50–53 campos/s; Perfect
Dark intro 14,85 → 14,45 (−3 %; −4 % en la segunda escena, −7 % en la
primera pesada); SM64 y Zelda a 59,5–59,7 campos/s sin regresión. Es la
primera palanca del recompilador que mueve el costo por instrucción del
nativo desde la Fase 3. El autor lo notó a ojo (5–10 %, menos tirones).

**Muestreo por tiempo** (mismo día, a pedido de main; docs/rec-ppe.md,
«Muestreo por tiempo»): `src/cpu/rec_sample.c`, archivo `muestreo` en la
PS3 y `SALSA64_REC_SAMPLE=1` en el host. La entrada de cada bloque deja su
id (2 palabras), el preámbulo y el despachador marcan lo que se hace en C,
y un hilo de prioridad 1100 muestrea cada 200 µs; al cerrar el juego, los
30 bloques con más tiempo con sus palabras de PPC. Primera corrida en la
PS3 (docs/perf-log.md, «Muestreo por tiempo en la PS3»): GoldenEye en la
presa pasa el 64–66 % en código generado, 21–22 % durmiendo al ritmo, 3 %
en el intérprete, 2,6 % en el despachador, 2,5–2,9 % en eventos y 1,6–2,8 %
en los caminos lentos; el reparto por bloque es plano (los 30 primeros
suman ~30 % del nativo, ninguno pasa del 3 %) e igual con el compacto y el
ancho. Contra el perfil de entradas × palabras: el bloque de más «costo»
(7F0BA3B8) se lleva el 0,7 % del tiempo (sale por sus saltos internos) y
pesan de más el refill de la TLB (80000000, 2,1 %), JR + SQRT.S (7001F7E0,
1,9 %, `fsqrt`) y los bloques grandes con FPU en línea (20 palabras
calientes por instrucción). El muestreo cuesta ~5 % (dos palabras y un
almacenamiento por entrada a bloque): se mide con él apagado. Pendiente:
marcar como camino lento la C de TLBWR/ERET (hecho en el código, por
probar), y lo que señala para compactar: la FPU en línea, el refill y el 3 %
de intérprete.

Lo que queda grande por instrucción: la entrada de bloque (ktlb + evento,
7–8 palabras), la salida (escritura + enlace), la ALU de 32 bits con su
`extsw` y la FPU en línea (~35–40 palabras por operación). Ideas anotadas
para después en docs/rec-ppe.md (datos por trabajo, precarga del sucesor,
bench de huella).
## 2026-10-12 (noche) — Plan para la jerarquía de memoria del PPE (docs/cache-ppe.md)

Documento de diseño, sin código: dónde se van los ~54 ciclos por
instrucción MIPS del código nativo (GoldenEye en la presa: 10–14 de
trabajo, ~40 de espera), con el reparto estimado entre fetch de código,
ERAT, datos del juego, JR/JALR, fronteras de bloque y saltos, a partir de
las cifras públicas del PPE (L2 ~41 ciclos, fallo a XDR ~380, ERAT de 64
entradas de 4 KB aunque la página sea grande, salto mal predicho 24) y de
los perfiles por juego de la PS3 (256 KB de código generado cubren el
54–66 % de las entradas; el 24 % de los bloques terminan en JR; el 82 %
de las salidas van por un sucesor dominante). Palancas con mecanismo,
estimación, costo, medición y dependencias, y el orden recomendado:
código compacto (rama en curso) → bench de huella en la PS3 → la sección
fría fuera del cuerpo → pila de retorno y caché de destinos para JR →
zona caliente de 256 KB y frío de verdad → trazas por el sucesor dominante
→ pistas de salto y precarga → `code_flags` chica → copia del VI por SPE →
compilar a la segunda. Descartadas: páginas de 1 MB para la caché (el ERAT
no las aprovecha y el texto lo mapea lv2), la RMT de la L2 (privilegiada),
precarga de datos del juego. Evaluadas además, con ganancia ~0 hoy: un SPE
«acomodador» de datos para el PPE (no hay recorrido previsible y disperso en
el hilo 0; lo lineal va entero al SPE), la caché por software en el local
store de un SPE (nada irregular que llevar a un SPE) y el hilo 1 como
ayudante de precarga (solo puede precargar código, llega un bloque tarde y
cuesta ~13 % por SMT: su carga útil la hace el hilo 0 en línea con `dcbt`
del sucesor y listas de precarga por transición).

## 2026-10-12 (noche) — Donkey Kong 64: la cámara que se acerca y se aleja leía una z image vacía (rama `focused-hermann-680a6f`, paquete SLSA00105 «Salsa64 cámara»)

Síntoma (PS3 real, v0.2.0, DK64 al 100 %): la cámara 3D se acerca y se
aleja sin parar, también en espacios abiertos; el HUD queda quieto.

Diagnóstico, desde el estado que el autor guardó en la escena (túnel,
campo 4714):
- **No es el recompilador ni los gemelos por Status.FR.** El intérprete
  del host hace lo mismo, y `--lockstep` desde el estado (qemu, 186 campos,
  más de cuatro vaivenes) no tiene diferencias; en esa escena no hay
  gemelos ni salidas por FR (también lo decía el log de la PS3).
- Lo que oscila es el ojo de la cámara (`449990`): DK (actor `449100`)
  está quieto y el ojo avanza hacia él durante 21 cuadros (42 campos) y
  vuelve de golpe al punto de partida, con el control suelto o empujado.
- El juego lee la **z image de la RDRAM** (`5d5600`, 320 × 240) donde se
  ve a DK y a ±40 píxeles (`80700ae4`: valor de 16 bits con exponente de 3
  y mantisa de 11) y, si lo ve tapado 21 cuadros seguidos (contador de la
  cámara en `806210d0`), recoloca la cámara detrás de él (`80620628`). La
  HLE dibuja en el RSX y la z image de la RDRAM estaba entera en 0 (lo más
  cerca posible): DK siempre tapado. Llenándola a mano con 0xFFFC la cámara
  converge y se queda quieta.
- **Ya estaba antes de v0.2.0** (desde la primera HLE de la Fase 2: el
  borrado de la Z nunca llegó a la RDRAM); al 20 % el vaivén iba cinco
  veces más lento.

Arreglo (docs/framebuffer.md, «La z image en la RDRAM»): el borrado de la
z image (FILLRECT en modo FILL sobre ella, `GFX_OP_ZCLEAR`) se escribe
también en la RDRAM, como lo deja el RDP: `salsa64_gfx_rdram_fills`
(`src/rsp/gfx/sync.c`) en el `finish` del backend síncrono y del SPE, antes
de entregar la tarea a la presentación (las copias esperadas de la misma
tarea quedan encima, como en el hardware). `GFX_OP_ZCLEAR` lleva ahora el
tamaño de píxel y el ancho de la color image y el color de 32 bits.
Pruebas: `gfx_zclear_fills_rdram` (color por dirección, bordes sin alinear,
recorte al ancho, nada fuera del rectángulo) y `gfx_zclear_rdram_at_finish`
(se escribe al terminar la tarea, no al arrancarla).

Efecto en otros juegos (host, capturas cada 300 campos antes y después,
con los guiones de docs/pads): Mario Kart 64, Super Mario 64, Perfect
Dark (intro) y Banjo-Kazooie, idénticas. Zelda OoT decide si dibuja el
resplandor de las luces puntuales leyendo la z image en su posición; antes
las veía tapadas y ahora destapadas: las antorchas del castillo del intro
tienen su halo, el resplandor de Navi sale completo (como en la consola
cuando nada los tapa) y cambian las motas del bosque Kokiri. GoldenEye en
Dam diverge un poco desde que empieza la misión (la vista se desplaza
menos de un píxel; mismo motivo), sin cambio visible. Lo dibujado no llega
a la z image de la RDRAM, así que una luz detrás de un muro también
brilla, y en DK64 la cámara no se recoloca cuando un muro tapa de verdad a
DK (pendiente en docs/framebuffer.md). Super Smash Bros. (el KO) no se
arregla con esto: ver su entrada.

Verificado:
- [x] Host: desde el estado de la consola, la cámara converge a su lugar
      en el túnel y se queda quieta (antes, vaivén cada 42 campos).
- [x] `test-host` 439/439 (Docker, ASan/UBSan); `test-rec` (unitarias en
      los cinco modos 439/439, nemu64-test en su línea base de 433, vitest,
      estados); `test-roms` (nemu64-test 433 / 136 con LLE, vitest,
      savetest, gfxtest ×3, estados, audiotest); `tools/build.sh` sin
      avisos.
- [x] `--lockstep` de DK64 desde el estado con el arreglo (qemu, 186
      campos, también la RDRAM): sin diferencias.
- [x] **PS3 real** (paquete «Salsa64 cámara», SLSA00105, 54798d9; el
      autor, 2026-10-12): la cámara de DK64 resuelta (estado de la ranura 0
      y un espacio abierto), a 59,8 campos/s con 7,0 ms de emulación por
      campo, como antes; en Zelda, Navi se ve completo.

## 2026-10-12 (noche) — Quitarle trabajo al PPE: medición del hilo 1 y tarea JPEG en un SPE, sin tirón (rama `determined-grothendieck`, paquete SLSA00103 «Salsa64 PPE»)

Pasos 1 y 2 de «Quitarle trabajo al PPE» (entrada del 2026-10-11). Medido en
la PS3 real (4ca4c2a, escala 5×; detalle en docs/perf-log.md, «Quitarle
trabajo al PPE: el hilo 1 y la tarea JPEG»).

**Corrección:** la tarea JPEG de Zelda no corría en el hilo de presentación
sino en el de **emulación** (síncrona, dentro de `salsa64_rsp_start`): sus
68 ms por sala frenaban directamente al CPU emulado.

Hecho:
- **Medición del hilo 1** (siempre encendida, solo contadores `mftb`;
  docs/hilos-ppe.md): cada 5 s, cuánto estuvo despierta presentación por
  campo emulado, desglosado (traducción, copias a la RDRAM, texturas en el
  PPE, presentar, otros; dormido en barreras del RSX y en el flip), y los
  campos de la emulación agrupados por cuánto se solaparon, con ns por ciclo
  emulado. Diagnóstico `carga-hilo1` («A B [KB]»): trabajo de mentira en
  fases de 0, A y B µs por campo presentado, para el A/B en la misma
  corrida.
- **HLE JPEG en enteros** (`src/rsp/jpeg/jpeg.c`): IDCT con constantes Q15
  (filas con 2 bits de fracción recortadas a 16 bits, columnas al
  dieciseisavo) y color en Q14. El SPE redondea los flotantes de simple
  precisión hacia cero: con flotantes el SPE y el PPE no podían dar lo
  mismo. Contra el microcódigo real (casa de Link con partida nueva, 150
  tareas): 99,06 % de los píxeles iguales (con flotantes, 99,03 %).
- **La tarea en un SPE libre** (`src/spu/jpeg`, 11,9 KB de código; backend
  `src/platform/ps3/jpeg_spu.c` sobre lib/spukit): termina por sondeo, como
  una gráfica (`salsa64_jpeg_backend` en `core/rcp.h`). Prueba del arranque:
  dos tareas sintéticas (`rsp/jpeg/selftest.c`, con coeficientes que
  saturan y desbordan) en el SPE y en el PPE, comparadas entre sí y con el
  CRC que fija la prueba del host. Con `SALSA64_DATA_DIR/jpeg-ppe`, la tarea
  en el PPE (diagnóstico).
- **En el SPE, en 4 carriles** (`src/rsp/jpeg/jpeg_v.c` sobre `v4.h`:
  intrínsecos del SPU o, en el host, los carriles uno por uno con la misma
  semántica): descuantizar, IDCT y color con las mismas operaciones enteras
  que la escalar; la prueba del host compara 6000 macrobloques
  seudoaleatorios byte a byte.
- `salsa64-run`: `SALSA64_JCHECK_DUMP=archivo` (con `--lle-check-jpeg`)
  vuelca la entrada y la salida del microcódigo para probar otra IDCT sin
  correr el juego. El pad de partida nueva (`docs/pads/zelda-pausa-nueva.pad`)
  ya no llega a la casa con el build de hoy (se queda escribiendo el
  nombre): el nuevo es `docs/pads/zelda-casa-nueva.pad`.

Medido (PS3 real):
- **Hilo 1:** GoldenEye en la presa, despierto 2,88 ms por campo emulado
  (15,8 %; 3,2–4,4 en las ventanas a 45–50 campos/s), de ellos traducción
  1,81 y copias 0,43; Perfect Dark 1,6–2,2 (la escena del desenfoque:
  copias 1,2–1,5 y barreras 1,1–1,8); Zelda 1,1–1,3; SM64 ~1,0.
- **Costo para la emulación:** ~0,13 ms por ms de trabajo del hilo 1 (A/B en
  la presa: +0,10 ms con 0,80 ms de carga por campo, +0,37 ms con 2,63),
  casi en línea; los ns por ciclo no suben con el solape natural. Hoy, ~0,4
  de 15,5 ms por campo en la presa (2,5 %).
- **Tarea JPEG**, por sala (75 tareas, 300 macrobloques): hilo de emulación
  67,8 ms (flotantes) → 29–30 ms (enteros en el PPE) → **0,34–0,38 ms (SPE)**.
  El SPE escalar tardaba 41,9 ms y el juego espera cada tarea: la emulación
  dormía en los sondeos y el tirón casi no cambiaba (peor campo 37–39 → 32–36
  ms; el autor no notaba diferencia). **Vectorizado (aacdcc4): SPE 2,86 ms
  por sala (×14,7), y el autor: «no hay tirones, solo la animación de
  transición, pero es muy rápida».** El peor campo que queda en la tanda
  (30–34 ms) es la carga de la sala en el CPU emulado (cargas de escena sin
  JPEG del mismo log: 30–45 ms). Prueba del arranque: SPE = PPE = host
  (EEC26FE1).

Verificado: `test-host` 437/437 (Docker, ASan/UBSan) y nativo; `test-roms`;
`test-phase2` en RPCS3 (menú con cierre limpio y el SPE de la tarea JPEG
detenido); `tools/build.sh` sin avisos; `--lle-check-jpeg` en el host.

Decisión y pendiente:
- **Paso 4 (traducción al RSX en un SPE): descartado por ahora** (decidido
  el 2026-10-12 con estas mediciones). Ganaría ~0,25 ms por campo en la presa
  (1,8 ms de traducción × 0,13), ~+0,7 campos/s en lo peor, con un rediseño
  grande (rsxkit es de un hilo); lo que limita es el código nativo del
  recompilador (79 % del hilo 0). Se revisa si la traducción crece mucho (a
  5× ya va a 2,3–3,6 ms por tarea en la presa) o si el hilo 1 llega a
  limitar la presentación.
- [x] JPEG en el SPE vectorizado (aacdcc4): 41,9 → 2,86 ms por sala, sin
      tirón para el autor.
- [ ] Opcional: los sondeos de la tarea JPEG duermen 100 µs cada uno (7,4
      ms por sala); con la tarea de ~40 µs, un sondeo más corto le quitaría
      unos ms a la carga. No se nota hoy.
- Receta de la casa de Link en el host: `docs/pads/zelda-casa-nueva.pad`
  (partida nueva, `--save-dir` a una carpeta vacía; Link despierta en la
  casa hacia el campo 14 300, 150 tareas JPEG hasta el 15 500).

## 2026-10-12 (noche) — Recompilador: perfil persistente por juego y zona caliente, medidos en la PS3 (rama `fervent-noyce-db464c`, paquete SLSA00104 «Salsa64 perfil»)

Objetivo: la localidad del código del recompilador (GoldenEye en la presa y
la segunda escena de la intro de Perfect Dark, limitadas por el CPU: ~47
ciclos del PPE por instrucción MIPS con ~7 MB de bloques vivos contra 512 KB
de L2). Hecho y medido; diseño en docs/rec-ppe.md («Perfil persistente y
bloques calientes» y «Siguientes pasos»), números en docs/perf-log.md.

- **Perfil por juego** (`src/cpu/rec_profile.c`): cada bloque cuenta sus
  entradas (cuatro palabras a la entrada, no en la vuelta de un bucle), la
  cuenta se suma entre campos a una tabla por (dirección, huella de las
  instrucciones, cantidad) y se guarda en `perfiles/<juego>.prf` al cerrar
  el juego (también al volver al menú) y cada 2 min por trozos desde el
  hilo de presentación. Identidad por contenido: un bloque reescrito en la
  misma dirección es otra entrada; los gemelos por FR comparten la suya.
  El archivo lleva código de cartucho y CRC de la ROM, no contiene ni un
  byte del juego ni código traducido (direcciones, huellas de un solo
  sentido, cuentas y banderas), y si falta, está corrupto o es de otro
  juego el perfil arranca vacío. Entre sesiones, una sesión larga hace
  decaer a la mitad lo que no corrió; una corta solo suma.
- **Zona caliente**: 1 MB al principio de la caché en dos mitades que se
  turnan; al cargar el perfil se marcan las entradas de mayor puntaje
  hasta tres cuartos de la zona y sus bloques se compilan ahí.
- **Sitios de salida de los bloques calientes** (pedido de main para los
  pasos siguientes): cada salida con destino fijo cuenta cuántas veces se
  pasa; de ahí salen tomado/no tomado de cada condicional y los dos
  sucesores más frecuentes de cada bloque, con la huella del bloque del
  destino. Solo se registra.
- Interruptores: `perfil-activo` (apagado por omisión; en la medición de
  abajo el paquete lo traía encendido y `sin-perfil` lo apagaba) y
  `perfil.cfg` (`caliente = KB`, `saltos = si|no`), documentados en
  docs/uso-ps3.md; en el host,
  `salsa64-run --profile` y `SALSA64_REC_HOT_KB`/`SALSA64_REC_SITES`.

Validado: unitarias host 440/440 y ppc64 con recompilador 440/440 (plain,
pin, caché chica; suite `perfil` nueva: tabla, mezcla, marcado, archivo con
corrupción e identidad, y con el recompilador contador, zona caliente,
huella distinta, rotación de mitad y sitios); nemu64-test 433 fallas (línea
base) con perfil nuevo y cargado, también con caché de 3 MB y zona de 256 KB
(rotaciones); `--lockstep` sin diferencias en nemu64-test (las dos cachés),
vitest y Perfect Dark 3 G ciclos con el perfil cargado; test-roms en verde;
build de la PS3 sin avisos. GoldenEye hasta la presa (7,5 G ciclos) en
lockstep con el perfil cargado: sin diferencias, con el binario anterior al
formato 2 y con el final (692 calientes, 1483 sitios de salida).

**Medido en la PS3 (el autor, una sola ejecución de la app):** la zona
caliente **no da ganancia**: GoldenEye en la presa 15,54 → 15,80 ns por
instrucción de nativo entre la primera sesión (perfil vacío) y la segunda
(762 bloques calientes juntos, 1592 sitios), Perfect Dark 12,73 → 12,39:
ruido. Contar cuesta ~5 % en GoldenEye (base con `sin-perfil` 14,82 ns de
nativo y 17,15 en total contra 15,54 y 18,19). SM64, Zelda y Majora sin
regresión (59–60 campos/s). La conclusión es que lo que manda no es dónde
está cada bloque sino cuántas líneas toca por instrucción: en qemu, los 25
bloques más ejecutados de GoldenEye son 21,3 palabras de PPC por instrucción
MIPS (9,9 en la sección que se ejecuta, 11,4 en caminos lentos y salidas);
cada acceso a datos mapeados cuesta ~10 palabras calientes y ~35 frías, y
la entrada de un bloque 16–20. Las palancas (un camino lento por bloque,
escritura de sucios por máscara, camino de la ktlb en línea, entrada más
corta) y los usos del perfil (pistas de salto: en GoldenEye 612 de 931
condicionales se toman más del 80 % de las veces y 184 menos del 20 %;
sucesor contiguo; traducción por adelantado y JIT por niveles en el hilo 1
del PPE, que duerme el 85 %) están en docs/rec-ppe.md, «Siguientes pasos».

Decidido con main: el perfil queda **apagado por omisión** (sin el archivo
`perfil-activo` no hay tabla, contador ni zona caliente: costo cero en el
hilo 0); se enciende para recoger datos o cuando los pasos siguientes lo
usen. Anotada la idea de contar gratis por muestreo desde el hilo 1 (lee
cada N µs el bloque en curso) y dejar la instrumentación solo para sitios y
sucesores. `caliente = 256` (lo que cabría en la L2 con los datos) quedó sin
medir. Los perfiles de la consola están en `build/ps3-logs/prf/` de la rama
(y en `build/ps3-logs/perfil-2026-10-12/` del árbol principal, 13 juegos).
Con las huellas, 445 bloques idénticos aparecen en cinco o más de los 13
perfiles (libultra). El puntaje cuenta entradas, no tiempo: por entradas son
~9 % de cada juego, pero ponderado por instrucciones solo ~3 % (mediana;
máx. 6 %), porque son bloques diminutos (esperas, raíz cuadrada). Zelda y
Majora comparten 973 bloques; GoldenEye y Perfect Dark, 807. Hacerlos
nativos ganaría del orden de 1–3 %; la palanca mayor es compactar el código
traducido.

## 2026-10-12 (noche) — v0.2.0 publicada y censo de compatibilidad

Publicada la **v0.2.0** (pre-release, etiqueta en ad72ebf, con
`salsa64-v0.2.0.pkg`; notas en el release y en `CHANGELOG.md`). Pasada final
en la PS3 real sin cuelgues: Paper Mario, Donkey Kong 64 (zipper incluido),
Perfect Dark y Zelda.

Censo con el candidato (PS3 real, respaldos NTSC del autor), para después de
v0.2.0:

| Juego | Resultado | Pista |
|---|---|---|
| Zelda OoT, SM64, MK64, Majora, Paper Mario, Mario Party 1 y 3 | bien | Majora: iluminación de algunas zonas |
| Donkey Kong 64 | 100 % | tirones al entrar a zonas nuevas (traducción) |
| Perfect Dark, GoldenEye, Conker | jugables | CPU en las escenas pesadas; Conker: sombras cuadradas, escena pesada de la intro a 49–55 (combiner: guardar 2–4 resultados o forzar en línea lo caliente) |
| Star Fox 64 | bien | falla breve en el logo inicial (falta descripción exacta); la estela del menú es del juego |
| Super Smash Bros. (y Smash Remix) | se congela en el primer KO | causa en la entrada de arriba (z-buffer como color image) |
| Kirby 64, Pokémon Stadium 2 | ~97–100 %, texturas con ruido o ajedrez | estado de Stadium 2 en la ranura 0 |
| Pokémon Snap | jugable, texturas estiradas | ¿recorte del RSP, como MK64? |
| Doom 64 | ~40 % | ~4,5 M instrucciones interpretadas por segundo: algo que el recompilador no traduce |
| Banjo-Tooie | intro (host) | arreglado con el desafío del CIC 6105 de la PIF; falta jugar más allá de la intro y confirmar en la PS3 |
| F-Zero X | carrera (host) | arreglado con el bus abierto del PI y la recarga del microcódigo a mitad de lista; por confirmar en la PS3 |
| Diddy Kong Racing | negro | microcódigo propio de Rare sin HLE |
| Indiana Jones | negro, 18 % | microcódigo de Factor 5 (LLE del RSP) |
| Resident Evil 2 | parpadeo y texturas rotas desde el inicio | estado en la ranura 0 |

Además: cadencia de la presentación (se pierde un campo de cada dos cuando
la emulación va sobrada; 5,8 % saltados en la intro de Perfect Dark a 5×),
las uniones azules de GoldenEye y el rompecabezas de Banjo.

## 2026-10-12 (noche) — Smash: el KO se cae con las copias a la RDRAM (acotado, pendiente; rama `infallible-wozniak-aa210c`)

**Resuelto el 2026-10-13** (entrada de ese día): no era la silueta sino los
márgenes del z-buffer, fuera de las tijeras, que la copia pisaba.

Se acotó en la PS3 (con fbfx.cfg = 1 no se cae; en v0.1.0 ya pasaba
con Smash Remix). En el host nativo se reproduce con `--gfx-frame` (demo,
las tareas gráficas paran en el campo 1424): sin el espejo del framebuffer
se cuelga igual; sin las copias esperadas corre toda la demo (12 987
tareas); saltando solo la copia del destino `0a3940/320`, también. Ese
destino es el z-buffer: en el KO el juego hace `ZCLEAR 0a3940` +
`TARGET 0a3940/320` (limpia el z con un FILLRECT como color image), dibuja
128 triángulos ahí (la silueta) y el CPU lee esa memoria cada campo (273
lecturas por campo desde `800c7840`, en +0x23f00). La HLE convierte el
relleno en ZCLEAR (solo profundidad), la color image nunca recibe el
relleno, y la copia esperada escribe el contenido viejo de la superficie
más los triángulos (y alfa siempre 1); el hardware deja 0xFFFC con la
silueta encima. Arreglo pendiente: con ZCLEAR sobre una color image del
mismo destino, rellenar también la superficie de color con el color de
relleno (host y PS3) y no forzar alfa 1 en la copia; prueba en test_raster.
**Probado y no basta** (2026-10-12, con el borrado de la Z ya escrito en la
RDRAM, entrada de la cámara de DK64): en el host, la demo con
`--gfx-frame` se sigue parando (1373 tareas a los 1500 campos) también
rellenando la superficie de color y copiando 0xFFFC sin alfa forzado; solo
saltando la copia de `0a3940/320` sigue (1432). Lo que rompe es lo que el
RSX dibuja ahí (la silueta), no el borrado: mirar qué escriben esos 128
triángulos en el RDP (¿profundidades como color?). Saltar la copia de lo
que se borró como Z en la misma tarea no sirve en general: la pausa de
Zelda borra su Z y copia el cuadro a esa dirección en la misma tarea.

## 2026-10-12 (noche) — Star Fox 64: la «estela» del menú es del juego (rama `infallible-wozniak-aa210c`)

En el menú principal, al elegir una opción, las etiquetas se ven repetidas
(«se replican muchas veces»). No es un error: en cada cuadro de esa
animación el primer dibujo es un rectángulo a toda pantalla (8..312 ×
8..232) sin textura, combiner G_CC_PRIMITIVE con PRIM = 0x0000002c (negro,
alfa 17 %) y mezcla alfa, sin borrar antes el framebuffer: oscurece un 17 %
lo que había en ese búfer (el cuadro de hace tres, triple búfer) y dibuja
encima: el afterimage de ese menú. En el host se reproduce igual (3–4
copias de «Main Game» apagándose). GLide64 en mupen64plus no lo muestra
porque no conserva el framebuffer entre cuadros; el hardware sí, como
nosotros.

## 2026-10-12 (noche) — Interbloqueo de la PS3 con más de SETS tareas gráficas en un campo (Paper Mario, Mario Party, Mario Party 3; rama `infallible-wozniak-aa210c`)

Salió en el candidato de v0.2.0: PM se queda quieto al pulsar START
en el telón (campo 688), MP tras los logos (681) y MP3 (516): «RSP ocupado»,
MIPS 0, «presentación en espera», y en esa ventana «1 esperas por un juego
de búferes». En el host nativo (intérprete + HLE) los tres pasan con las
pulsaciones exactas; en qemu con el recompilador también: es de los hilos
de la PS3.

Causa: `acquire()` (hilo de emulación) espera sin tope a que la tarea
n − SETS + 1 esté traducida para reusar su juego de búferes; la
presentación solo traduce hasta las tareas «visibles» que la emulación
publica al cerrar cada campo (df45ac1, temblor de Banjo). Si un juego manda
más de SETS (3) tareas gráficas dentro de un campo, la emulación se bloquea
en acquire sin cerrar el campo, no publica las visibles y la presentación
nunca traduce esa tarea. Desde 98def88 las tareas ya no se frenan 200 ms en
`done()`, así que es más fácil que caigan varias en un campo (por eso PM
pasaba por la mañana). Arreglo igual al de 98def88 para las copias:
`acquire` publica en `rdp.set_waiting` la tarea que espera y despierta a
presentación; `salsa64_rdp_flush` traduce hasta ella aunque no sea visible.
Sin prueba unitaria (son los hilos de la PS3); por confirmar en la consola.
## 2026-10-12 — El desenfoque de Perfect Dark desde el destino del RSX; copias esperadas a 200 ms y el «estado» del SPE al triple (rama `wonderful-jepsen-ebf467`, paquete SLSA00101 «Salsa64 desenfoque»)

- **Desenfoque (docs/framebuffer.md, «Texturas desde un destino sin
  decodificar»):** las 180 franjas por tarea del cuadro anterior se
  reconocen como de su framebuffer (un `LOADBLOCK` del que se muestrea una
  fila toma el paso de `SETTIMG`) y la HLE no las decodifica
  (`GFX_TEX_TARGET`); el consumidor las lee del RSX a la resolución interna
  o, si no puede, las decodifica en el PPE. PS3 real: PD a 59,7–60,2
  campos/s en la azotea con 0,0–0,5 texturas decodificadas por tarea (antes
  133–181) y el desenfoque «sutil pero visible»; el baile del Espantapájaros
  de Majora's Mask sale por el mismo camino (96 texturas por tarea leídas del
  RSX), «sublime».
- **Copias esperadas a 200 ms (98def88, en main):** desde que la presentación
  traduce solo las tareas «visibles» (temblor de Banjo-Kazooie), la tarea
  cuya copia a la RDRAM espera la emulación no se traducía hasta el tope de
  `RB_WAIT_US`: la intro de PD a 4,9 campos/s en la PS3 (también con main
  sin esta rama). `salsa64_rdp_flush` traduce siempre esa tarea.
- **Pila del SPE 0 y combiner:** la pila no alcanzaba en main (holgura
  −3 008 bytes: docs/perf-log.md, «Pila del SPE 0»); el arreglo compiló el
  combiner con `-Os`, y en la PS3 eso triplicó el «estado» del SPE en todos
  los juegos. Ahora el resultado del combiner se memoriza con sus entradas
  (b8d9041), S2DEX va con `-Os` y el directorio de la caché de texturas
  baja a 896 entradas: holgura 3 520 bytes, comprobada en el build.
- **Pendiente: el «estado» de Conker.** En la escena pesada de su intro
  (campos 4400–4999) va a 49,3–55,4 campos/s con el SPE en 30,4–34,9 ms
  por tarea y el «estado» en 4,1–7,6 ms; antes del `-Os` iba a 51,8–58,3
  (SPE 28,7–31,1, estado 2,5–3,9) y con el `-Os` sin memorizar a 44,1–50,5
  (SPE 33,7–39,0, estado 7,5–11,8). Conker cambia de combiner o de PRIM/ENV
  entre dibujos más que PD y un solo resultado guardado no alcanza.
  Opciones, a medir en la PS3: (a) guardar 2–4 resultados (~0,4 KB cada
  uno); (b) dejar `-Os` pero forzar inline lo caliente del combiner
  (`p_term`, sumas de polinomios): en el SPU cada llamada sin pista de
  salto cuesta ~18 ciclos.
- **PS3 real con todo (ece6049):** Zelda «sublime» (59,8 campos/s, estado
  1,38 ms), GoldenEye igual que antes (limita el CPU en Dam), PD a 59,8 en
  la azotea con el estado en 0,70 ms; sus tirones son campos saltados por la
  presentación (5,8 % contra 0,9 % del paquete anterior, con la emulación a
  60), que vinieron con 99686a7 y siguen en la rama de los arreglos
  gráficos (docs/perf-log.md, «Pila del SPE 0»).

## 2026-10-12 (noche) — Donkey Kong 64: logos entrelazados y «zoom» en el juego (presentación; rama `infallible-wozniak-aa210c`)

Salió en la PS3 real con la rama del recompilador (DK64 al 100 %):
1. **Logos (campos ~100–460)**: el VI muestra 640 × 237 por campo con
   stride 2560 (VI_WIDTH 1280) y el origen alterna dos líneas seguidas: un
   búfer de 640 × 480 del que cada campo muestra una línea de cada dos. En
   el host el destino es `034000/640` (y `0ca000/640`) de 480 filas. La
   presentación tomaba 237 líneas seguidas desde el origen: la mitad de
   arriba estirada, y saltando una línea entre campos. Ahora
   `rdp_targets_present` ve que el stride es k veces la línea del destino y
   presenta k × 237 líneas desde la primera (alineada a k): el cuadro
   entero, estable (prueba `targets_present_interlaced`).
2. **Juego: «zoom» cada 2 campos** (VI de 320 × 237 fijo, dos
   framebuffers `034000/320` y `059800/320` alternando). El primero
   comparte dirección con el `034000/640` de los logos, y la superficie de
   cada destino lleva su propia escala (presupuesto de VRAM y
   `rdp_target_scale` al crearla); el modo 1:1 de la presentación usaba el
   tamaño en píxeles de la superficie, así que un búfer a 2× y otro a 3× se
   veían de tamaños distintos. Ahora la decisión y el tamaño en pantalla
   salen del tamaño lógico por la escala objetivo. El log de la PS3 anota
   cada destino al crearlo (`gfx: destino …: lw × lh a N×`). Confirmado en
   la PS3 real (el autor, 64b37ba): logos completos y sin zoom; el log
   muestra la causa: `02a200/320: 320 × 459 a 2×` y `04fa00/320: 320 × 240
   a 5×`. El 459 viene de un FILLRECT de DK64 sobre dos búferes seguidos
   (como el de 720 de Star Fox, pero sin pasar de 480): ahora una color
   image de más de pantalla y media (`rdp_targets_tall`: lh > 9/8 del
   ancho, o > 480) también se trata como alta y su relleno se reparte
   entre los destinos que empiezan dentro (el último puede asomar por
   abajo), así los dos búferes quedan a la misma escala (en el host ya no
   aparece el destino de 459). Zelda y SM64 bien con el modo 1:1 nuevo.
   **Corrección:** eso dejó negra «la escena del cierre» (el autor, con
   main 5182004): esa misma color image de 320 × 459 lleva a veces dibujos
   de verdad, no solo rellenos, y el CPU la copia después (en el log:
   «imposible … copia 02a200/320 esperada PERDIDA»). Ahora una color image
   es «alta» solo si todos sus dibujos hasta el próximo TARGET son
   rellenos (`fills_only`, host y PS3); con dibujos reales se crea tal cual
   (a la escala que le toque, que la presentación ya no convierte en
   zoom). Prueba: `raster_tall_fill` cubre los dos casos. En el host no se
   llegó a esa escena con los guiones probados; por confirmar en la PS3.
## 2026-10-12 — Donkey Kong 64 al 20 %: el recompilador vaciaba la caché en cada cambio de Status.FR (rama `zealous-wilbur-cec8b5`, paquete SLSA00102 «Salsa64 DK64»)

- **Síntoma (PS3 real, main 8c876ac):** DK64 a 12–13 campos/s con 1,4 MIPS
  útiles y 65–75 ms de emulación por campo; el log decía 445 864 bloques,
  959 vaciados y 1 desalojo en 30 s. La HLE gráfica (1 ms por tarea) y el
  audio no pesaban.
- **Causa (confirmada en el host con un contador en la escritura de
  Status):** DK64 corre hilos con FR = 1 y otros con FR = 0, y cada cambio
  de hilo cambia el bit al reponer SR: ~260 cambios por segundo de consola
  (4 100 en 16 s). El recompilador compilaba toda la caché para un FR y la
  vaciaba entera al cambiar (`salsa64_rec_run`): en qemu, 4 173 vaciados y
  1,28 M bloques compilados en 16 s de consola (1 340 por campo), 236 s de
  reloj. No era el DMA del PI, ni CACHE, ni la TLB, ni los ids: los
  vaciados ahora se cuentan por causa (`stats.flushes_by[]`, en la línea
  «recompilador:» del frontend y de salsa64-run).
- **Arreglo (docs/rec-ppe.md, «Status.FR: gemelos en vez de vaciar»):**
  solo depende de FR un bloque que accede a un registro impar de la FPU
  (`fr_dependent`, por instrucción); su entrada revisa CU1 y FR de una vez
  (una instrucción más que antes) y, si no coinciden, sale por `exit_fr` y
  el despachador busca o compila el gemelo para el otro FR (`twin[]` en
  KSEG0, filtro por FR en `xhash` fuera); los demás bloques valen con los
  dos. `salsa64_rec_invalidate` mata a los dos gemelos. +128 KB
  (docs/memory-budget.md).
- **Host (qemu, DK64 16 s de consola, `--rec`):** 236 → 7,5 s de reloj,
  1 284 166 → 8 867 bloques, 4 173 → 0 vaciados, 256 bloques dependen de FR
  y **0 gemelos** (los hilos con FR = 0 no tocan registros impares: el
  mismo código no corre con los dos FR).
- **La cuenta de «93 % en bucles de espera» es coherente:** DK64 ya da 81 %
  con el intérprete en el host; con 12 campos/s el CPU emula 19 M ciclos
  por segundo y el 7 % útil son 1,4 MIPS. Lo que estaba mal era el costo de
  ese 7 %, no la cuenta.
- **Validación en el host:** unitarias 426/426 con el recompilador en los
  cinco modos (nueva `fr_switch_keeps_code_cache`), nemu64-test con las
  433 fallas de la línea base, vitest, estados y `--lockstep` de
  nemu64-test y vitest sin diferencias (`tools/test-rec.sh --lockstep`);
  GoldenEye hasta la presa (7,5 G ciclos) y la intro de Perfect Dark (3 G)
  en `--lockstep` sin diferencias. Build de PS3 sin avisos.
- **PS3 real (paquete SLSA00102 «Salsa64 DK64», ad18cce + main da2adcd;
  el autor):** DK64 **59,6–60,0 campos/s (100 % de NTSC)** en la intro, la
  isla y el juego, emulación 4–6,5 ms por campo (antes 65–75), 11 908
  bloques y **0 vaciados** en 2 min (antes 445 864 y 959 en 30 s); una sola
  ventana a 48 campos/s al cargar la isla. 756 bloques dependen de FR y 0
  gemelos: el código con registros impares no corre con los dos FR. Zelda
  (con estado cargado y pausa) y SM64 al 100 %, un vaciado cada uno (el de
  cargar el estado); Perfect Dark al 7 % por la espera de copias de
  da2adcd, ya arreglada en main (98def88). Quedan dos defectos de
  presentación en DK64 que no son del recompilador y pasan a la rama de
  gráficos: los logos en alta resolución entrelazada (640×474, VI_WIDTH
  1280, origen y V_VIDEO alternando por campo) salen como la mitad de
  arriba estirada, y en el juego la imagen hace «zoom» cada 2 campos con
  el VI fijo (320×237, dos búferes alternos): escala distinta por destino.

## 2026-10-12 (noche) — Star Fox 64 colgado a los 5 s: el espejo del framebuffer pisaba un DMA (rama `infallible-wozniak-aa210c`)

Salió en la PS3 real (Rev 1 y Rev 0: letras unos 5 s y negro, el
CPU al 100 % sin bucles de espera reconocidos). En el host se reproduce
solo con el espejo del framebuffer (`--gfx-frame`, que lo activa; con
`SALSA64_FBFX=0` corre): el juego se queda en la tarea gráfica 67 con todos
sus hilos de libultra esperando mensajes (`tools/libultra-threads.py`) y el
hilo ocioso corriendo. No es el CIC 6101 ni el PIF ni la EEPROM (sigue
leyendo el control; la EEPROM se inicializa bien).

Causa (traza del PI y de las tareas, mismo campo 151): la tarea 67 limpia
el framebuffer `38f800` con un FILLRECT y, antes de que el RSP termine
(30 000 ciclos de HLE), el juego carga por DMA 113 KB de datos MIO0 encima
(`38f800`–`3ab1d0`, en trozos de 256 bytes). En la N64 el RDP acaba antes
y el DMA pasa por encima; en el host el espejo «al terminar la tarea» se
escribía después del DMA y pisaba la cabecera del MIO0 (en la PS3, ms más
tarde, pisaba más). Arreglo general (docs/framebuffer.md): un anillo de
DMA (`rdp_dma_ring`, targets.c, lo alimenta `salsa64_bus_rdram_written`
con la tarea gráfica en vuelo) y ninguna copia de la tarea T (espejo,
copia esperada, espejo chico; host y PS3) pisa un tramo que un DMA tocó
desde que T empezó. Pruebas: `targets_dma_ring` y `raster_mirror_after_dma`
(falla sin el arreglo). Host: Star Fox Rev 1 con el espejo, 590 tareas en
20 s y una sola copia descartada (la de la tarea 67). En la PS3 real (el
autor, con 0b4108e): **ya arranca**, pero con «cuadros negros y ruido tipo
estática en la parte superior» durante todo el juego, a velocidad 100 %.

**Estática arriba:** Star Fox limpia sus tres framebuffers seguidos
(`38f800`, `3b5000`, `3da800`) con un solo FILLRECT de 320 × 720 sobre una
color image en el primero (al arrancar, campo 3, y al pasar a la escena,
campo 158); después cada cuadro solo limpia 8..312 × 8..232 y el borde de
8 px queda como lo dejó ese relleno. La PS3 rechazaba esa color image
(«imposible», alto > 480) y el borde nunca se limpiaba: la superficie se
ceba con la RDRAM, donde las filas 0–7 de `38f800` traen los datos MIO0
cargados por DMA (en el host: basura de colores en la fila 0 del campo
300; con el arreglo, negra). Ahora un relleno sobre una color image más
alta que 480 se reparte entre los destinos existentes que caben enteros
dentro (`rdp_targets_inside`, host y PS3; pruebas `targets_inside` y
`raster_tall_fill`), sin crear un destino de 720.

**Cuadros negros:** no reproducidos ni en el host ni en RPCS3 (capturas de
cada campo hasta el 445 y cada 3 hasta el 1800 sin saltos de brillo;
presentación toda del RSX, edad 2,5–3 campos). Para verlos en la consola,
el frontend traza ahora la presentación por campo si existe
`/dev_hdd0/salsa64/trace-vi` (con un número N de campos; por omisión 300):
origen del VI, de dónde salió la imagen (RSX, RDRAM o nada), destino,
ranura, campos desde que se dibujó y cuánto va por delante la emulación.
PS3 real con 8c8e3cd (el autor): jugando ya no parpadea; queda el logo
«1997 Nintendo presents». La traza por campo lo explica: los 1200 campos
salen del RSX con 1–3 campos de edad, pero entre el campo 96 y el 107 no
hay campos: la emulación esperó 200 ms (el tope) la copia a la RDRAM de la
tarea 5 (3da800, «esperada», 1 tarde) porque con el VI apagado la
presentación no la traducía. Es lo que arregla main en 98def88 (la
presentación traduce siempre la tarea cuya copia espera la emulación),
traído a la rama en 129a6bb. `trace-vi` queda renombrado a `trace-vi.off`
en la consola.

## 2026-10-12 (tarde) — Líneas azules de GoldenEye (cobertura AA), túnica de Link (máscara con clamp) y el negro de la intro de Banjo (niebla en el blender) (rama `infallible-wozniak-aa210c`)

Con el volcado del autor en el túnel de Dam (SELECT + R2), las líneas azules
salen también en el raster del Mac: no son del RSX. `tools/gfxdump.py pixel`
(nuevo, junto con `tex`; el script ya lee el `gfx_draw` actual) muestra que
ningún triángulo cubre el centro de esos píxeles: es una unión en T entre el
piso (dibujo 5, con un vértice a media arista) y la pared (dibujo 7, la
arista entera). Ese vértice cae un píxel fuera de la recta de la pared y deja
una cuña de hasta 0,56 px por la que se ve el relleno negro con el tinte
azul del rectángulo final de la escena. La N64 la tapa porque con AA_EN el
RDP escribe todo píxel con alguna de sus 8 muestras de cobertura dentro.

`gfx_draw.aa` marca los triángulos opacos sin prueba de alfa con AA_EN
(prueba `gfx_aa_flag`), pero las dos formas de reproducir la cobertura
fallaron en la PS3 real (el autor, 2026-10-12):
- POLYGON_SMOOTH del RSX: no hace nada (el volcado 2529 traía la marca en
  100 de 109 dibujos y las líneas seguían). Ya está quitado.
- Ensanchar los triángulos en la HLE (cada arista corrida lo que llegan las
  8 muestras, vértices nuevos sobre el plano del triángulo): sí tapaba las
  líneas de Dam, pero a escala interna alta engorda todos los bordes (0,5 px
  de N64 son 1,5 px a 3×) y en mallas densas «los polígonos se superponen»,
  en Zelda y en los modelos de GoldenEye; «se ve peor todo lo demás». Quitado
  también, incluso como opción por juego. En el SPE costaba ~1 µs más por
  triángulo (sin que el PPE esperara).
Las líneas azules de GoldenEye quedan pendientes. Camino que falta probar:
multimuestreo del RSX (superficie con 4 muestras y resolución al presentar)
en rsxkit, que cubriría la cuña con la mezcla del fondo, como el VI de la N64
a 1×; cuesta memoria y relleno. O ensanchar solo las aristas que no comparte
ningún otro triángulo de la malla, lo que pide conocer la malla.

`salsa64-run --raster-dump` dejaba volteados por palabras los bytes de
`nterms` a `pad` de los volcados big-endian: los dibujos con combiners
generados salían mal en el host. Con eso corregido, la túnica de Link del
volcado del autor se ve igual en la PS3 y en el host.

**Túnica de Link (OoT):** el autor ve cortado el patrón de sombra del
pliegue. La textura del torso es una I8 de 16 texels de ancho en la TMEM
(línea de 16 bytes) en una tile de 32 con máscara 4 y espejo más clamp en s.
El RDP limita la coordenada a la tile y después la enmascara: las columnas
16–31 son el reflejo de las 0–15. `tmem.c` veía el clamp, ignoraba la
máscara y leía 32 texels seguidos (la mitad derecha salía de la fila
siguiente de la TMEM). Ahora, con clamp y una máscara más chica que la tile,
decodifica con la coordenada enmascarada, y eso entra en la llave de la
caché. HLE y LLE coincidían porque la LLE usa la misma decodificación.
Prueba: `gfx_tile_clamp_mirror_mask`. En siete juegos solo cambia OoT, y
por debajo del umbral.

**Pantalla negra de Banjo entre el logo de Rare y la cascada:** también en
el host (campos 992–1020 con `banjo-casa.pad`), con la HLE y con la LLE. El
juego funde con un rectángulo a pantalla completa con el blender
FOG·A_FOG + MEM·(1 − A_FOG) y el combiner en 0. El blender toma el color de
P, que aquí es la niebla, y eso no se hacía. Con la niebla blanca opaca
salía negro (el combiner); con alfa 0xD8 no pintaba. Ahora `blend_constant`
(src/rsp/gfx/rdp.c) pone la constante de P (niebla o blend) como color y el
alfa que elige A. En el host la transición queda blanca y después se funde
en la escena, sin el negro. Prueba: `gfx_blender_fog_color`.

**Rompecabezas de Banjo (sigue pendiente):** en esa escena todas las tareas
de Banjo terminan con el RDP congelado y la interrupción del DP sale al
descongelar. Una espera de la copia en el `done` del RDP no sirve: la tarea
congelada no se traduce hasta descongelar (si no, vuelve el temblor) y
esperarla bloquearía la emulación. La espera tiene que ir al descongelar
(`salsa64_rsp_dp_unfreeze`), con presentación traduciendo esa tarea en el
acto. Queda para después.

**PS3 real (el autor, 2026-10-12):** POLYGON_SMOOTH no hizo nada (las
líneas seguían; el volcado 2529 traía la marca en 100 de 109 dibujos). Con
el ensanchado, «ya no hay líneas azules en GoldenEye» y «la túnica sale
bien». Pero en Zelda, a escala interna alta, «los polígonos se superponen»:
el ensanchado engorda todos los bordes (0,5 px de N64 son 1,5 px a 3×) y en
mallas densas se nota. Queda como opción por juego (`GFX_JOB_AAGROW`,
sync.c, por el código "NGE" del encabezado): solo GoldenEye. En el SPE
costó ~1 µs más por triángulo en Dam (4,4 → 5,5), sin que el PPE esperara;
con recíprocos en vez de divisiones debería bajar (sin medir aún).

**PS3 real con 4e35681 (el autor, 2026-10-12):** túnica de Link bien;
Zelda y GoldenEye con los polígonos normales; en la intro de Banjo ya no
está el negro de medio segundo, queda un negro de uno o dos campos al
cambiar de escena que en el host no existe (323 campos seguidos sin saltos
de brillo): probablemente el VI muestra el framebuffer recién cambiado antes
de que el RDP lo dibuje. Menor; sin perseguir. Las líneas azules de
GoldenEye siguen pendientes (arriba).

## 2026-10-12 — Banjo-Kazooie (protección del CIC 6103), cara de Conker (K5) y rayas del logo de Zelda (rama `infallible-wozniak-aa210c`, paquete SLSA00099 «Salsa64 gráficos»)

Lo visto por el autor en la PS3 con v0.1.0 (modelos que tiemblan, cosas
dibujadas encima de otras, polígonos que faltan, el cielo) no era de la HLE
ni del RSX: el juego dibujaba así a propósito porque nuestro arranque HLE no
dejaba lo que deja el IPL3 del CIC 6103.

Diagnóstico (host, respaldo propio, `docs/pads/banjo-casa.pad`):
- La proyección de Banjo (`G_MTX` de la perspectiva y tres rotaciones)
  traía en toda escena cerca = 750 y lejos = 1250 (escala 0,5, perspNorm
  65). En la intro el fondo (árboles, cascada, cielo) quedaba detrás del
  plano lejano y la HLE rechaza los triángulos con los tres vértices ahí;
  Tooty, Bottles, la casa por dentro y el cielo de Spiral Mountain (una
  malla a w 35–135 alrededor de la cámara) quedaban delante del cercano y se
  recortaban; lo que quedaba en medio tenía su profundidad en una franja
  estrecha (z/w normalizada de 0,84 a 1,07 para Banjo): temblores y
  superposiciones. En el campo 2400 de la intro, 75,5 % de la imagen negra.
- El microcódigo real (LLE, desde el mismo estado) daba la misma imagen que
  la HLE: fondo negro y Tooty cortada. Así se descartaron `src/rsp/gfx`,
  rsxkit y el rasterizador.
- La decomp de Banjo-Kazooie (`viewport_setRenderPerspectiveMatrix`, bloque
  `ANTI_TAMPER`) lee la RDRAM física 0x1D8 antes de cada proyección; si no
  vale lo que deja el IPL3, cambia 30 y 4000 por 750 y 1250. Ejecutando en
  el emulador el IPL3 del propio respaldo (sin desensamblarlo) se midió lo
  que deja: su imagen de DMEM 0x4C0–0x764 en RDRAM 0x000–0x2A4 (0x1D8 = ROM
  0x698), y la DMEM y la IMEM en 0xFF salvo 6103 en IMEM 0x000.
- La segunda la comprueba `ucode_load` al iniciar: sin ella el juego no
  trae de la ROM (0xB70) el arranque del RSP y su búfer queda en ceros. Por
  eso `--lle=all` no arrancaba (pendiente del 2026-10-10: «la primera tarea
  corre 958 M instrucciones sin tocar COP0»).

Arreglo (`src/core/boot.c`, `boot_x103`, como el del 6105): copia de DMEM
0x4C0–0x764 (los bytes del cartucho del usuario) a RDRAM 0x000–0x2A4, DMEM e
IMEM en 0xFF y 6103 en IMEM 0x000. Fuentes en docs/THIRD_PARTY.md y tabla
en docs/cpu-vr4300.md («Arranque HLE»).

Medido en el host:
- Intro, campo 2400: 582 → 862 triángulos por tarea; píxeles negros 75,5 %
  → 0,4 %. Se ven el fondo, Tooty entera, la casa con Banjo en la cama, el
  cielo nocturno y Spiral Mountain con su cielo.
- HLE contra el microcódigo real (`--lle=all` desde el mismo estado, 40
  campos después; el LLE ahora arranca solo): 2109 y 3230 píxeles distintos
  de 76 800 en la intro (campos 2470, 2830), 5468 en la casa (11490), 1281 y
  1329 en juego frente a la casa (17000, 17600); 16 671 en la vista aérea
  (9490) con la cámara en movimiento. Lo distinto es movimiento (el LLE
  lleva otro ritmo), no geometría.
- Zelda, SM64, Mario Kart, GoldenEye, Perfect Dark y Conker: capturas cada
  500 campos idénticas a las de main (no pasan por el 6103).
- La protección no depende de la HLE gráfica: en la PS3 debería verse igual
  que en el host.

Herramientas: `--state-load` con `--lle` carga el estado (guardado con la
HLE) y sigue con el microcódigo real, para comparar las dos en la misma
escena; `docs/pads/banjo-casa.pad`.

Verificación (con main b445531): `tools/build.sh test-host` 406/406
(pruebas nuevas `boot_cic6103_leftovers`, `gfx_setconvert_k5` y
`gfx_texrect_two_tiles`), `tools/test-roms.sh` (gfxtest 35/35 ×3),
`tools/build.sh` sin avisos, `tools/test-rec.sh` y `tools/test-phase2.sh`.

**Cara de Conker (polígonos negros en la boca):** el hocico y la boca se
dibujan con `(SHADE − ENV)·K5 + PRIM` en el segundo ciclo y K5 = 255
(`G_SETCONVERT`); la HLE ignoraba ese comando y K5 valía 0. El microcódigo
real (LLE) daba la misma cara negra: era del RDP común. K4 y K5 (9 bits con
signo) van ahora al combiner y al estado del RDP, al final del formato de los
estados guardados (los viejos los cargan en cero). En el host, Conker frente
al logo de Rare: 1047 píxeles cambian (el hocico). Prueba
`gfx_setconvert_k5`.

**Rayas del logo de N64 de Zelda:** el texto «NINTENDO 64» va en
rectángulos de 2 filas en 2 ciclos: el texto en la tile 0 y un ruido I8 de
32 × 32 en la tile 1 con t × 32 (`shiftt` 11), que vuelve al mismo renglón
en cada fila y cambia de renglón por franja. El límite del TEXRECT se
calculaba con la tile 0 y se aplicaba también a la tile 1: el ruido quedaba
pegado a su primer renglón en una fila de cada dos (en el verde del trazo de
la «N», la fila de arriba de cada franja fija en 134). Ahora cada tile tiene
su límite. Además, a 2×–5× el RSX interpolaba t dentro de la fila y recorría
media textura de ruido por píxel de la N64: cuando una tile avanza media
textura o más por fila, el rectángulo sale en una tira por fila con la t de
esa fila. Prueba `gfx_texrect_two_tiles`. La «N» del logo sale más pálida
que en mupen64plus (GLide64), pero el microcódigo real da las mismas
coordenadas del mapa de entorno (82 píxeles distintos en todo el cuadro) y
el muestreo cae sobre el brillo de la textura: no hay evidencia de un error
nuestro.

**Mario Kart, negro arriba del muro de Luigi Raceway (resuelto):** era la
tapa del muro, una franja que el juego dibuja con la textura apagada y un
combiner TEXEL0·SHADE. La HLE dejaba ntex en 0 con los términos de textura:
el host toma la textura que falta como 1 (blanco) y el RSX muestreaba una
sin ligar (negro). Ahora con la textura apagada TEXEL0/TEXEL1 = 1 en la HLE
(`gfx_combine_untextured`). Hallado con volcados SELECT + R2 de la PS3 (la
captura del RSX con la franja negra, el mismo volcado en el host sin ella) y
`tools/gfxdump.py`; confirmado por el autor en la PS3. Antes, en el host el
cielo se veía siempre (no era el cielo: era esa franja). Para eso, **SELECT + R2** (`src/frontend/main.c`)
guarda `gfxdump-<campo>.bin` con la salida de la HLE del cuadro en pantalla
y `gfxdump-<campo>.ppm` con la captura del RSX de ese mismo cuadro: se
dibuja en la Mac con `salsa64-run --raster-dump` y se compara.

Comparación de capturas cada 500 campos contra main: solo cambian Banjo,
Conker y el logo de Zelda (SM64, Mario Kart, GoldenEye y Perfect Dark
idénticos).

**Temblor de Banjo (PS3, visto por el autor con el paquete de arriba):**
los modelos «avanzan y retroceden». Banjo arranca la tarea del cuadro
siguiente con el RDP congelado (FREEZE) sobre el búfer que el VI todavía
muestra y lo descongela al cambiar de búfer; el núcleo ya retenía la
interrupción del DP, pero presentación traducía la tarea al RSX en cuanto
terminaba: ese búfer mostraba el cuadro nuevo un campo antes y después el
otro búfer el anterior. Ahora cada campo cuenta como visibles solo las tareas
que el RDP pudo ejecutar (sin la retenida, `rsp.dp_held`). Confirmado por el
autor con su estado de la escena de la bruja: ya no tiembla.

**Rompecabezas de Banjo negro en la PS3 (pendiente, efectos de
framebuffer):** las piezas son texturas que el CPU copia del framebuffer en
la RDRAM (70 080 lecturas de 0x3a5d00, medido en el host). Al rearmarse, el
juego dibuja el título en un búfer que el VI no muestra y lo copia 2–4 campos
después; en el host el espejo del framebuffer es inmediato y las piezas
llevan el título, en la PS3 llega tarde y el CPU copia lo que había (negro, o
la imagen de antes). Reproducido con dos estados del autor: desde ellos el
host dibuja las piezas bien y la PS3 negras. Arreglo posible: esperar el
espejo antes de la interrupción del DP, como opción por juego (cuesta
milisegundos por cuadro); queda para después (decisión del autor).
La pantalla negra entre el logo de Rare y la cascada no está atribuida: en
el host ahí hay un fundido blanco (una nube 3D enorme pegada a la cámara);
falta un SELECT + R2 en la PS3.

Pendiente (PS3 real, paquete «Salsa64 gráficos» SLSA00099; confirma el
autor):
- [x] Banjo-Kazooie: se ven los modelos, las texturas y el cielo; ya no
      tiembla (el autor, 2026-10-12).
- [x] Conker: la cara (hocico y boca) en la intro con el logo de Rare:
      «ya sale bien» (el autor, 2026-10-12).
- [x] Zelda: el logo de N64 al arrancar sin las líneas de entrelazado: «ya
      sale bien» (el autor, 2026-10-12).
- [x] Mario Kart, Luigi Raceway: el muro sin la franja negra (el autor,
      2026-10-12).
- [ ] GoldenEye en el túnel de Dam y la túnica de Link: SELECT + R2 cuando
      se vean las líneas azules o el defecto de la túnica, y bajar
      `gfxdump-*.bin` y `.ppm` por FTP para dibujarlos en la Mac. En el
      host, Link en su casa (guion `zelda-kokiri.pad` con la partida del
      autor) no muestra un defecto visible en la túnica.
- [ ] Otros juegos del 6103 (Diddy Kong Racing, Paper Mario) no se
      probaron; el arranque nuevo reproduce lo que deja la consola.

## 2026-10-12 (madrugada) — Para retomar: pruebas pendientes en la PS3

Publicado el 2026-10-11: el repositorio es público y está el release
**v0.1.0** (pre-release, etiqueta en e0aa556, con `salsa64-v0.1.0.pkg`).
Después entraron en main la FlashRAM (7ede061: Majora's Mask jugable) y la
decodificación de texturas recortada y con vectores del SPE (5853614: intro de
Perfect Dark de 50 a 60 campos/s).

Ramas listas que esperan la prueba en la PS3, en este orden:
1. ~~`heuristic-agnesi-760f8c` (SLSA00098 «Salsa64 botón PS»)~~: confirmada en
   la PS3 real e integrada el 2026-10-12 (entrada de abajo).
2. `infallible-wozniak-aa210c` (SLSA00099 «Salsa64 gráficos»): Banjo-Kazooie
   (lo que deja el arranque del CIC 6103 en la RDRAM, DMEM e IMEM; sin eso su
   protección cambiaba los planos de la cámara) y la cara de Conker
   (`G_SETCONVERT`, K4/K5 en el estado del RDP).
3. `charming-spence-f80224` (SLSA00100 «Salsa64 Majora»): campanadas del fin
   del día (los destinos distinguen el tamaño de píxel, no solo dirección y
   ancho) y desenfoque de movimiento (baile del Espantapájaros, recuerdo de
   Zelda); estados de prueba en las ranuras 5 y 6. El telescopio necesita un
   estado guardado frente a él.

Pendientes: el telescopio de Majora; leer el cuadro anterior del desenfoque
desde el destino del RSX en alta resolución (hoy sale de la copia a 1×); el
cielo de Luigi Raceway; las líneas del logo y la túnica de Zelda; las líneas
de GoldenEye; la localidad del código del recompilador (GoldenEye en la presa y
la segunda escena de la intro de Perfect Dark, limitadas por el CPU); y un
release v0.2.0 cuando entren las tres ramas de arriba.

Después de cerrar lo anterior: revisar (solo lectura primero) el recompilador
de Wii64 y Not64 y el port `emukidid/wii64-ps3` (base de Cell64): asignación
de registros entre bloques, accesos a memoria y TLB, caché de código y FPU;
licencia archivo por archivo antes de copiar nada a `src/` (nunca a
`lib/rsxkit` ni `lib/spukit`). También ejecutar el IPL3 del cartucho al
arrancar, como mupen64plus, para no depender de reproducir lo que deja cada
CIC (protecciones de Rare). Y HLE de funciones de libultra en el CPU (como
UltraHLE): perfilar en GoldenEye y Perfect Dark cuánto tiempo va a funciones
conocidas (matrices, trigonometría, raíz, copias de memoria) y, si pesa,
reemplazarlas en el recompilador por versiones nativas con vectores del PPE.
Detección por comportamiento o por huella (hash) del código, sin copiar nada
de la biblioteca al repo; resultado exacto, porque la lógica del juego no
admite aproximaciones.

Anotado la tarde del 2026-10-12, para después de v0.2.0:
- **Sombras cuadradas en Conker** (PS3 real, desde antes de las ramas de hoy):
  las sombras redondas de los personajes salen como bloques. Probablemente la
  textura de la sombra (degradado con alfa) se dibuja sin su alfa o con el
  formato o el modo de ciclo equivocado; comparar contra mupen64plus en la
  misma escena.
- **Hacks** (reportes de usuarios): Smash Remix 2.0.1 arranca y corre bien,
  pero se cuelga cuando cae un personaje; The Sealed Palace (hack de Ocarina
  of Time) se queda en pantalla negra. Revisar con el log de la PS3 antes de
  suponer la causa (tamaño de ROM, CIC, microcódigo F3DEX3 u otro).

## 2026-10-12 — Botón PS y cuadro al salir: el juego se pausa (rama `heuristic-agnesi-760f8c`, paquete SLSA00098 «Salsa64 botón PS»)

Problema (PS3 real, v0.1.0): dentro de un juego, el botón PS abría el menú
del sistema y, al volver, Salsa64 ya no respondía y había que cerrarlo desde
el sistema (visto en Banjo-Kazooie). `sysutil_cb` solo atendía «Salir del
juego»: la emulación, el audio y los mandos seguían detrás del menú. No hay
log de ese cuelgue, así que la causa exacta no se vio.

Hecho (docs/hilos-ppe.md, «Botón PS y pausa»; docs/uso-ps3.md):
- **Pausa** con el menú del sistema abierto (`SYSUTIL_MENU_OPEN` … `CLOSE`,
  o el bit del sistema en `padInfo.info`): la emulación queda quieta al
  empezar el campo siguiente y al volver el ritmo empieza de nuevo; el audio
  calla sin perder lo encolado y sin que el hueco entre en la medición de
  velocidad (`audio_sync_pause`); se sigue presentando el último cuadro con
  flip (`present_still`, `salsa64_rdp_present_again`) y llamando a
  `sysUtilCheckCallback`. Mandos sueltos mientras los tiene el sistema; nada
  queda pegado al volver. Al log: «sistema: menú abierto/cerrado», «pausa: …»,
  «mandos: …».
- **Cuadro de START + SELECT** sobre la misma pausa: «¿Volver al menú?» (con
  `embedded`, «¿Salir al XMB?»), × sale, ○ o el atajo otra vez siguen; lo
  apretado al abrir o cerrar no llega al juego hasta soltarse. «Confirmar al
  salir» en Opciones (`confirmar = si|no` en menu.cfg, sí por omisión).
- **Latido**: un hilo que avisa en el log dónde se queda quieto cada hilo si
  uno no da vueltas en 2 s. `sin-pausa` (archivo de diagnóstico): el menú
  del sistema no pausa, como en v0.1.0, para ver con el latido qué colgaba.
- `pads_apply` ya no gira sin fin si el sistema desaloja a presentación a
  mitad de publicar los mandos.
- **Pruebas**: suite `pausectl` (9: menú del sistema, mandos tomados, cuadro
  con ○, ×, el atajo otra vez y sin confirmación, cuadro con el menú del
  sistema encima, geometría en 4:3 y 16:9, y el hueco de 6 s contra el
  ritmo: ni ráfaga ni esperas largas al volver) y dos en `sync` (con la
  pausa, el audio vuelve en < 150 ms sin estirar; sin ella, el hueco lo
  estiraba). test-host, test-roms, test-rec, test-phase2 (menú: 5 juegos
  abiertos y cerrados) y el build sin avisos, en verde, también después de
  traer main con la FlashRAM.

**En la PS3 real (2026-10-12)** la pausa sola no bastó: al volver del menú
del sistema la app seguía viva (eventos, mandos, flips) pero la pantalla
quedaba negra. Con el latido y leyendo píxeles del búfer trasero se vio la
causa, que seguramente es la de v0.1.0: el sistema **pisa el estado del
RSX en cada flip** de la aplicación mientras su menú está encima y un rato
después, y con el RSX parado en `gcmSetWaitFlip` cada flip tardaba 2,3–4,5 s.
Arreglo en `lib/rsxkit`: el flip anterior se espera en el PPE
(`rsxkit_frame_begin`) y después de cada flip se olvida todo el estado que
rsxkit daba por puesto (`rsxkit_forget_state`), más `rsxkit_after_system` y
`rsxkit_frame_ready` (docs/hilos-ppe.md). Confirmado: en el menú de Salsa64
y dentro de un juego, PS y volver tres veces cada uno (el XMB se ve con el
menú debajo, al volver todo sigue y responde; la emulación se detuvo y
retomó cada vez), el cuadro de salir con × y «Salir del juego».

También con Majora's Mask sonando: dos vueltas del botón PS (la salida
cuenta la pausa; en la ventana siguiente 0 % estirado y 0 faltas), el cuadro
con «No» y con ×, y cierre limpio. Criterio de cierre cumplido.

## 2026-10-12 — Majora's Mask: campanadas, recuerdo de Zelda y telescopio en «tablero de ajedrez» (rama `charming-spence-f80224`, paquete SLSA00100 «Salsa64 Majora»)

Problema (PS3 real, main 7ede061): en Zelda: Majora's Mask (NZSE) la
deformación de la pantalla con las campanadas del fin del día, el recuerdo
de Zelda enseñando la Canción del Tiempo y la vista del telescopio del
Observatorio se veían «como una matriz de píxeles muy pequeños
intercalados, como un tablero de ajedrez».

Medido (estados de la consola cargados en el host, log de la PS3; detalle
en docs/framebuffer.md, «Majora's Mask»):
- **Campanadas** (desde las ~5:36 hasta las 6:00): cada tarea copia el
  framebuffer a `784600/320` en 40 franjas y lo redibuja desplazado en
  56–60. La **pausa** usa esa dirección y ancho como imagen de 8 bits (la
  cobertura de su filtro). La tabla de destinos emparejaba solo dirección
  y ancho: después de abrir la pausa una vez, la ranura quedaba de 8 bits;
  las texturas de 16 bits no la encontraban (en el log de la consola, 40
  lecturas del RSX por tarea en vez de 100) y la copia a la RDRAM escribía
  bytes de intensidad en una imagen de 16 bits. En el host se reproduce
  igual (pausa, y después las campanadas: ruido) y sin la pausa sale bien.
- **Recuerdo de Zelda y baile del Espantapájaros**: desenfoque de
  movimiento con un acumulador en `3a92c0/320` que cada tarea mezcla con el
  cuadro (70 % de lo anterior) y lee en la siguiente. Se leía de su copia
  sin esperar en la RDRAM: una o dos tareas tarde, a 1× y en 16 bits; con
  la realimentación, el redondeo se acumula (en el host, sin retraso, la
  estela salía hasta 17 niveles más oscura) y, con el tramado del RSX en
  las copias a 16 bits, deja un patrón fijo por píxel.
- **Telescopio**: en el log de la consola, dos copias esperadas al entrar
  y ~39 lecturas por tarea de una imagen guardada en la RDRAM; falta un
  estado de esa escena para verlo en el host.

Hecho:
- `rdp_targets_get`: la misma dirección y ancho con otro tamaño de texel
  es otra imagen si la ranura no se dibujó en `RDP_TARGETS_FRESH` tareas
  (si está viva se comparte, como necesita la pausa de Zelda OoT).
- Las copias a la RDRAM sin esperar (la misma imagen tarea tras tarea) ya
  no pasan la imagen al CPU: el RDP la sigue leyendo del RSX mientras esté
  viva (`rdp_targets_readback(…, cpu_owns)`; el host modela lo mismo).
- `rsxkit_target_read` apaga el tramado (*dithering*) del RSX.
- El log ya no da contadores de texturas negativos tras cargar un estado.
- gfxtest, escena V: una color image de 8 bits que diez cuadros después es
  una de 16 bits leída como textura (36 puntos; sin el arreglo, negro).
- Host, contra main con las mismas entradas: pausa y equipo de Zelda OoT,
  túnel de Mario Kart, SM64 del logo a la cara e intros de Conker y
  Perfect Dark, cuadro por cuadro idénticos. Pruebas: 403 unitarias,
  `tools/test-roms.sh` (gfxtest 36/36 en sus tres variantes), compilación
  de la PS3 sin avisos.

Después, en la misma rama: **el host con la precisión de la N64.** Con el
acumulador leído del destino, guardar un estado a mitad del baile y
cargarlo daba 40 bytes de RDRAM distintos de la corrida de corrido (tras
cargar, lo dibujado sale de la RDRAM a 5 bits; de corrido, del búfer del
host a 8). Los destinos de 16 bits del host guardan ahora los 5 bits por
canal de la copia a la RDRAM: idéntico guardando en tres puntos del baile,
a mitad del desenfoque de Perfect Dark y en la pausa de Zelda OoT (16090 →
16300). La RDRAM no cambia: Zelda OoT, Mario Kart, SM64, Conker y Perfect
Dark, cuadro por cuadro iguales a main con la misma precisión.

**PS3 real (2026-10-12, paquete SLSA00100):** campanadas bien, también
con la pausa abierta antes (el autor las ve «con más resolución»: el efecto
lee la superficie del RSX a 3×); baile del Espantapájaros con la estela
limpia y fluida. Telescopio: ya sin tablero de ajedrez, pero faltaba la
máscara ovalada del lente y el sol salía seis veces. Con los estados del
autor en el host (docs/texturas.md):
- **Lente**: una I4 de 64 × 64 con espejo y clamp en una tile de
  128 × 128. La decodificación con la coordenada enmascarada (f393824, de
  la rama de gráficos) arregla el ancho; el alto no cabe en los 32 KB de
  textura del SPE: se decodifica un solo periodo y el eje va con «mirror
  once» del RSX.
- **Sol**: un atlas de dos soles dibujado sin corrección de perspectiva
  (`G_TP_NONE`): el RDP usa S·(1/W) tal cual y los microcódigos F3DEX2
  normalizan 1/W a la mitad, así que la textura se muestrea a la mitad de
  la coordenada (medido con el microcódigo real). La HLE lo hace ahora en
  los triángulos de la segunda generación.
- Las dos escenas quedan como con el RSP real (`--lle=all`), que es la
  referencia que confirmó ambas causas.

**PS3 real, segunda vuelta (2026-10-12, paquete 86d005f):** lente completo
(primer uso del MIRROR_ONCE del RSX), un solo sol, y sin regresión en la
pausa de Zelda OoT ni en el túnel de Mario Kart. Criterio de cierre
cumplido.

Falta:
- [ ] Cambios bruscos de iluminación en el alcantarillado de Ciudad Reloj
      (ranuras 2, 3 y 7 del autor: amarilla, gris y verde): el host da lo
      mismo que la consola, no es del RSX; comparar con mupen64plus.
- [ ] Escena de gfxtest para «mirror once» y para los triángulos sin
      perspectiva.

## 2026-10-11 — FlashRAM: Majora's Mask guarda (rama `clever-tereshkova-aa0089`, paquete SLSA00096 «Salsa64 FlashRAM»)

Problema (PS3 real, v0.1.0): en Majora's Mask, después de la escena inicial
con Skull Kid, Link moría y aparecía con tres ocarinas en los botones C. El
log decía «guardado: flashram (tabla por código de juego) todavía no se
emula: sin guardado»: el juego identifica el chip al arrancar y lee sus
partidas y el guardado del búho; sin chip leía ceros.

Hecho:
- **FlashRAM de 1 Mbit** (`src/core/flashram.c`, código propio desde
  n64brew.dev «Flash», cotejado con ares y mupen64plus-core; fuente de cada
  detalle en la cabecera y en docs/THIRD_PARTY.md): comandos en 0x08010000
  (borrado de chip y de sector, programación de página, modos búfer de
  página, estado, identificador y lectura), registro de estado (0x8C en
  reposo), identificador 11118001 00C2001E, DMA del PI en los dos sentidos
  y accesos de 32 bits del CPU. Un solo modelo, el MX29L1100 (direcciones de
  lectura por palabra de 16 bits, ráfagas que no salen de su bloque de
  32 KB): es el de Majora, Pokémon Snap y Puzzle League según ares y el que
  mupen64plus da a todos; libultra elige el direccionamiento por el
  identificador. Borrar y programar son instantáneos.
- **Archivo** `saves/<título>-<CRC1>.fla` de 128 KB con el volcado diferido
  de siempre; `SALSA64_SAVE_MAX` sube a 128 KB (búfer del guardado, zona de
  paso y lectura al abrir: +0,3 MB, docs/memory-budget.md). El panel del
  menú muestra «FlashRAM» sin «(sin emular)». Al cerrar, el log dice cuántos
  comandos recibió el chip.
- **Estados guardados**: los 128 KB en CART y el estado del chip en una
  sección nueva, FLSH, con versión propia (docs/estados.md). El búfer de
  paso sube de 128 a 256 KB.
- **Tabla por código**: ya marcaba FlashRAM para Majora (NZS), Paper Mario
  (NMQ), Pokémon Snap (NPF), Puzzle League (NPN), Stadium 1 y 2, Jet Force
  Gemini y otros 13; ahora las pruebas lo comprueban en varias regiones.
- **Pruebas**: suite `flashram` (10 pruebas: identificación, estado,
  lectura por palabras con la vuelta de 32 KB, borrado de sector y de chip,
  programación con AND y búfer a 0xFF, comandos raros, reset) y una de ida
  y vuelta de FLSH en `state`; 401 en total, en verde. `savetest` tiene una
  cuarta variante, `salsa64-savetest-flashram.z64`: identifica el chip,
  borra el chip, programa las 1024 páginas, relee, borra el último sector y
  comprueba que no toque el anterior; pasa sus dos arranques en
  `tools/test-roms.sh` y entra en `save-tests` (RPCS3 y PS3).
- **Majora's Mask en el host** (respaldo propio, `salsa64-run` nativo con
  `--pad`): al arrancar pide el estado y el identificador por DMA de 8
  bytes; con START en el título lee los sectores 0–3 en DMA de 8 KB, no
  encuentra partida y escribe los sectores 2–5; al crear el archivo 1
  («Please wait…») borra el sector 0 y programa sus 128 páginas. El archivo
  abre con 3 corazones, la escena inicial (bosque, Skull Kid, Epona) se
  pasa con A y Link queda jugable con 3 corazones, la espada en B y los
  botones C vacíos, vivo mil campos después. La partida persiste entre
  corridas (`--save-dir`). Para rastrear el chip:
  `EXTRA_CFLAGS=-DSALSA64_DEBUG_FLASH` y `--trace-from`.

- **PS3 real** (2026-10-11, paquete SLSA00096, salida a 1080p): el autor
  creó su archivo, pasó la escena inicial y jugó unos 18 minutos: «ya es
  jugable». En el log, el `.fla` nuevo, los dos volcados diferidos de la
  creación (512 y 128 escrituras del juego: sectores 2–5 y 0), 1959
  comandos al chip en la primera sesión (0 desconocidos) y «cerrado sin
  cambios pendientes»; al reabrir, los 128 KB leídos del `.fla` y el estado
  de la ranura 0 cargado (campo 270 → 39145); más tarde el juego volvió a
  escribir (256 escrituras, volcado diferido). Estados guardados y
  cargados varias veces (8384 KB, ~0,7 s al guardar, 0,18–0,53 s al
  cargar). 59,8 campos/s, CPU ~3,3 ms por campo. En el `.fla` bajado: la
  partida en el sector 0 («ZELDA3» en 0x24) y los sectores 2–5 escritos.
  El guardado con búho o con la Canción del Tiempo queda para más adelante
  (hace falta jugar bastante para llegar).

Falta:
- [ ] Guardar con un búho o con la Canción del Tiempo en la PS3, salir y
      reabrir (la creación del archivo y los estados ya están probados).
- [ ] Probar Paper Mario, Pokémon Snap y Puzzle League con respaldos
      propios. mupen64plus pone en 1 el bit 0 del estado al pedir el
      identificador «para Pokémon Puzzle League»; aquí, como en ares, una
      lectura de E/S en modo identificador da 0x11118001 (bit 0 en 1) y el
      estado vale 0x8C. Si Puzzle League no guarda, mirar eso primero.
- [ ] Otros modelos (MX29L1101 por byte, MN63F8MPN de Matsushita) solo si
      algún juego no se adapta al MX29L1100.

## 2026-10-11 — Estados guardados con 10 ranuras por juego y contador de FPS (rama `wonderful-blackwell-6f40d3`, paquete SLSA00094 «Salsa64 estados»)

Hecho (diseño y decisiones en docs/estados.md):

- **Núcleo portable** (`src/core/state.c`): cabecera de 64 bytes (magia,
  versión, CRC1/CRC2 y tamaño de la ROM, título, RDRAM, campo) y secciones
  «etiqueta + largo» descritas con una sola función para guardar y cargar,
  en big-endian: el mismo archivo sirve en la PS3 y en `salsa64-run`. CPU con
  COP0, TLB y FPU; planificador; MI, SP, DP, VI, AI, PI, RI, SI y la tarea del
  RSP en curso; DMEM/IMEM; PIF; guardado del cartucho; estado de las HLE
  gráfica (RDP + TMEM) y de audio (DMEM, ABI, MP3) entre tareas, también en
  los SPE (trabajos nuevos); miniatura de 160 × 120; RDRAM al final sin
  comprimir. Secciones cortas cargan con ceros (para crecer), versiones
  viejas o nuevas, otro juego u otra RDRAM se rechazan con un mensaje.
- **La tarea del RSP en vuelo** se recoge antes de guardar sin adelantar su
  fin emulado (`salsa64_rsp_quiesce`): el estado sale en cualquier campo y
  la continuación es idéntica.
- **PS3** (`src/frontend/states.c`, `main.c`): SELECT + R1 guarda, SELECT +
  L1 carga, SELECT + izquierda/derecha cambia de ranura (esos botones no
  llegan al juego con SELECT apretado); `/dev_hdd0/salsa64/states/<título>-
  <CRC1>.st0…st9`, escritos con sysFs a un .tmp y renombrados. Barrera en el
  límite del campo: emulación recoge la tarea, encola el campo y espera;
  presentación presenta lo que queda (miniatura), espera al RSX y guarda o
  carga; al cargar vacía la caché del recompilador, suelta los destinos del
  RSX (`salsa64_rdp_drop_targets`, sin tocar la numeración de las tareas) y
  vacía la cola de audio con su transición (`audio_sync_flush`). Aviso de
  ~2 s abajo en la pantalla (`src/frontend/osd.c`, rsxkit/ui).
- **Contador de FPS** (pedido de la coordinación): «VI 59,9 (100 %) · juego
  30 · pantalla 60», promedio de 0,5 s, contado solo en el hilo de
  presentación; SELECT + △ y fila «Mostrar FPS» en Opciones; se recuerda en
  menu.cfg (`fps = si|no`).
- **Host**: `salsa64-run --state-save CAMPO:ARCHIVO`, `--state-load ARCHIVO`
  (con `--lockstep` también la referencia) y `--fields N`.

Pruebas:
- Determinismo (guardar en N, cargar en otra corrida y seguir hasta M contra
  de corrido: estado byte por byte e imágenes iguales): unitarias
  (`test_state.c`, también con el recompilador), `tools/test-roms.sh`
  (gfxtest, vitest, audiotest), `tools/test-rec.sh` (gfxtest con `--rec` =
  intérprete) y respaldos propios en el host: Zelda (intro y Kokiri), SM64
  (intro y castillo), GoldenEye (intro y la presa) y Mario Kart: idénticos.
- RPCS3 (`tools/rpcs3-run.py --estados A,B`: guarda en A, carga en B y la ROM
  de prueba se tiene que verificar después): vitest, gfxtest y audiotest OK,
  cierre limpio (RPCS3 no sirve para medir los tiempos).
- `tools/build.sh test-host` 383/383 (con main); `tools/test-roms.sh` verde;
  `tools/test-rec.sh --lockstep` verde (unitarias en los cuatro modos,
  nemu64-test con la línea base, estados con `--rec` = intérprete, y
  `--lockstep` sin diferencias en nemu64-test, vitest y gfxtest desde un
  estado cargado); `tools/test-phase2.sh` verde; `tools/build.sh` (todos los
  programas) sin avisos. El SPE gráfico solo copia tal cual el tramo del
  estado del RDP y el PPE arma el formato: 0,2 KB de código, la pila queda en
  ~9,9 KB (main con los efectos de framebuffer: ~10,2 KB; la cadena más honda
  usa ~8 KB).
- Con los efectos de framebuffer de main: la barrera espera las copias RSX →
  RDRAM en vuelo y las escribe antes de guardar o cargar
  (`salsa64_rdp_settle`), y al cargar se olvidan también el scratch, los
  espejos y el historial de copias (`salsa64_rdp_drop_targets`). La prueba
  de determinismo de gfxtest corre con el rasterizador en las tres corridas
  (copias espejo y a la RDRAM leídas por el CPU en cada tarea): idéntica; en
  el host con respaldos, también la pausa de Zelda (16090 → 16300), el túnel
  de Mario Kart, la presa de GoldenEye y SM64 (del logo a la cara de Mario y
  el castillo, tras el arreglo de main de la copia del framebuffer, que
  esta prueba encontró: el rasterizador del host dejaba de dibujar SM64 a
  las 96 tareas). La marca de «ya copiado» de cada destino se reinicia al
  cargar; lo dibujado que todavía no se copió no entra en el estado
  (docs/estados.md, «Lo que no entra en un estado»).

**PS3 real (2026-10-11, noche): funciona.** Guardar y cargar en Zelda,
SM64, Mario Kart, GoldenEye (ranuras 0 y 1 independientes) y Perfect Dark;
cargar después de volver al menú y reabrir el juego; «No hay estado en la
ranura N» y «Ranura N (vacía)» donde corresponde; cierre limpio cada vez.
Guardar 568 ms de media (casi todo escribir 8 MB en el disco), cargar 171
ms (docs/perf-log.md, «Estados guardados»). El contador no cambia los
campos/s (GoldenEye en la presa 45–49 con él, como sin él).

La primera prueba colgó la consola entera al guardar: la traza por UDP
paró justo antes de crear la carpeta `states`. Las funciones `sysFs*` de
libsysfs son puentes al módulo cellFs del sistema, que hay que cargar con
sysModuleLoad; sin cargarlo, en RPCS3 andan igual y en la PS3 real cuelgan
todo. Ahora la E/S va con las llamadas al kernel (`sysLv2Fs*`,
`<sys/file.h>`), como newlib.

## 2026-10-11 — Recompilador: FPU residente, sin microcódigo del PPE, COP0 con rutinas propias y caché por tramos (rama `jovial-jones-2408ac`, paquete SLSA00093 «Salsa64 FPU»)

Objetivo: bajar los ~50 ciclos del PPE por instrucción MIPS del código
nativo (GoldenEye en la presa, 49,4 campos/s). Hecho en el host y en qemu;
la medición en la PS3 queda para cuando haya turno (cola: estados guardados,
después esta rama). Diseño en docs/rec-ppe.md.

- **Microcódigo del PPE** (manual del Cell, tabla A-1/A.1.3): toda forma con
  punto (`andi.`, `andis.`…), los desplazamientos por registro, `mcrfs` y
  `mtcrf` de varios campos cuestan ≥ 11 ciclos y vacían el pipeline; `mfcr`
  ~34. El emisor usaba `andi.` en cada ANDI de MIPS y en la compuerta de
  cada operación de la FPU, `andis.` a la entrada de cada bloque con COP1 y
  `mtcrf 0x38` en cada salida a C. Fuera (rlwinm, li+ori+and, mtocrf).
- **FPU residente** (`src/cpu/rec_emit_fpu.h`): los 12 `cpu->fpr[p]` más
  usados del bloque en f14–f25, casa siempre al día, clasificación con
  `fabs` y `fcmpu` contra constantes en f27–f31 (nada pasa por enteros),
  inexacto por `r − a == b ∧ r − b == a` en vez del TwoSum, FCR31 perezoso
  en cr2 (C, causa I, bandera I, compuerta) escrito al salir del bloque,
  LWC1/SWC1 directo entre la RDRAM y el registro, CVT.S.W/CVT.D.W con
  `fcfid`. Prueba nueva `executed_sequences_match_interp` (3000 secuencias
  de COP1 en un bloque contra el intérprete, con memoria de E/S), que
  encontró tres errores antes de medir (alias fd = fs en CVT.S.D y ADD,
  CTC1 sin escribir los enteros modificados, y el camino lento de LWC1
  cargando el registro residente de la RDRAM: Perfect Dark abortaba).
- **COP0 sin el intérprete**: MTC0 a EntryLo0/1, PageMask e Index nativos
  (el bloque sigue); TLBWI/TLBWR con `rec_h_tlbw` (ktlb al día solo para
  esa entrada, sin comparar las 32); ERET con `rec_h_eret` y salida por
  `exit_jr` como un JR. GoldenEye hacía 2200 llamadas al intérprete por
  campo por el refill (1,9 ms).
- **Caché de código por tramos**: 32 MB (el techo por el alcance de los
  saltos al preámbulo) en 8 tramos; llena, se desaloja el más viejo en vez
  de vaciar todo (los vaciados completos recompilaban ~7000 bloques de
  golpe: campos de 52 ms en el log de referencia de main). Cada bloque decide
  si llama a C con `bl` o `mtctr` según dónde cae. Las invalidaciones de
  GoldenEye (44 345 en el log de main) son en su mayoría páginas que vuelven
  en otro marco y ya se reutilizan desprendidas y enganchadas (perfil en
  qemu, 128 s: 30 105 invalidados, 29 171 desprendidos, 23 554 enganchados).
- **Páginas de 1 MB** (pedido de main): el heap de PSL1GHT ya sale de un
  mmapper de páginas de 1 MB (`ppu/librt/sbrk.c`), así que la RDRAM solo
  ganaría alineación; la caché de código no puede ir en memoria de 1 MB
  (con HEN no es ejecutable) y solo se alinea a 1 MB en `mk/rec-cache.ld`.
  El frontend anota al arrancar el tamaño de página real de RDRAM, rec, n64,
  caché y pila (syscall 351) y el archivo `rdram-1m` prueba
  `sysMemoryAllocate` de 1 MB para la RDRAM.
- Perfil de GoldenEye en qemu (128 s hasta la presa, por campo): 1590
  llamadas a C (COP0 1236, FPU 120, E/S 235), 11,9 instrucciones por bloque,
  0,80 de COP1 y 3,94 de memoria por bloque; registros más usados: v0 59 %,
  ra 57, a0 51, a1 42, sp 42, a2 33, v1 33, s0 31, at 30, a3 29, t6 26. Los
  dos bloques más calientes (3,1 % cada uno) eran un bucle de comparación
  partido porque un salto condicional apunta dentro del mismo bloque.
- **Saltos internos con destino dentro del bloque** («Saltos internos con
  destino dentro del bloque» en docs/rec-ppe.md): el camino tomado salta al
  código de la instrucción destino sin salir, reponiendo el estado de la FPU
  residente y de FCR31 que el código de ahí asume y ajustando `r29`. Y en
  un bloque sin JR/JALR, r26 es un registro más para el bloque (5 con los
  fijos). Más registros fijos entre bloques (s0 31 %, at 30 %, t6 26 %)
  queda pendiente: r27 y r28 están ocupados y liberarlos cuesta cargas por
  bloque; medir primero.

Validado en qemu-ppc64 (todo con el árbol final): unitarias 365/365 en los
cinco modos (incluida la FPU por memoria), nemu64-test 433 fallas (línea
base) con y sin registros fijos y con caché de 3 MB (93 desalojos), vitest
OK (también con 2 MB), `--lockstep` sin diferencias en nemu64-test (también
con caché de 3 MB y 92 desalojos), vitest y Perfect Dark 300 M ciclos;
`tools/build.sh test-host` 365/365 y `tools/test-roms.sh` en verde. El
lockstep de GoldenEye hasta la presa (7,5 G ciclos) pasó **sin
diferencias** con la FPU residente (9d1eba0: 283,6 M salidas comparadas) y
con el árbol final (15d6724: 204 M salidas, 1 vaciado y 6 desalojos); la
intro de Perfect Dark (3 G ciclos) también, con el árbol final. Dos
errores que solo el lockstep encontró: el camino lento de
LWC1 cargando el registro residente de la RDRAM (Perfect Dark abortaba) y
los saltos internos dejando basura en los registros escritos por las
instrucciones saltadas (nemu64-test, bloque 80014c84). `tools/build.sh`
sin advertencias.

**Medido en la PS3 real (2026-10-11, noche;** tablas en docs/perf-log.md,
«FPU residente, sin microcódigo…»; paquetes SLSA00093 «Salsa64 FPU» y
SLSA00095 «Salsa64 CPU FPU» —SLSA00094 es el de los estados guardados—,
con main 69051fc traído a la rama):
- GoldenEye, presa junto al camión: 17,5 ns por instrucción ejecutada
  contra 18,2 de main (−4 %), todo de los ayudantes de COP0 (llamadas a C
  1,56 → 0,73 ns, ktlb 0,78 → 0,20). **El código nativo sigue en ~47 ciclos
  por instrucción MIPS** con cualquier combinación de banderas: no lo
  limitan las instrucciones emitidas sino, con toda probabilidad, el tamaño
  del código (~7 MB de bloques vivos contra 512 KB de L2; la caché va en
  páginas de 64 KB). Ese es el siguiente trabajo.
- **La FPU residente resultó más lenta** en el bench del mismo paquete
  (add.s 40,0 → 30,9 MIPS, mixto 168 → 135) y en GoldenEye no se distingue:
  queda apagada por omisión (`SALSA64_REC_OPT_FPU_RES`, archivo
  `jit-fpu-residente`). Quitar el microcódigo sí suma: con la FPU por
  memoria, mixto 156,6 → 168,2, add.s 35,7 → 40,0, cvt 40,7 → 50,6.
- Zelda, SM64 y Mario Kart a 59,6 campos/s, sin regresión (emulación por
  campo 5–20 % menor que main). Perfect Dark baja a 49–50 campos/s en un
  nivel por espera del RSP (SPE de la HLE gráfica, 14 ms por campo, 131
  texturas decodificadas por tarea): lado gráfico, no el recompilador.
- Páginas: la RDRAM ya va en 1 MB; `rdram-1m` no se midió (nada que ganar).

A/B que siguen en el paquete (archivos vacíos en /dev_hdd0/salsa64/):
`jit-fpu-residente`, `jit-cop0-viejo`, `jit-sin-tramos`, `jit-viejo`,
`rdram-1m`, `perfil`.

## 2026-10-11 — SM64 se colgaba con los efectos de framebuffer: el espejo del framebuffer sigue al dibujo, no al VI

**Síntoma** (host, main 4baa6df): Super Mario 64 con `SALSA64_FBFX=2` dejaba
de mandar tareas gráficas en la 96 (campo ~343, paso del logo a la cara de
Mario; el audio seguía). Bisección con `--first-parent` en la rama de los
efectos: lo rompe b4d9d61 («Host: el modelo de copias sigue la política de
la consola»); ya había fallado en bf836f2 (espejo del framebuffer por
campo) y lo tapaba sin querer que el host copiara todos los framebuffers
al terminar cada tarea (su `vi_width` quedaba en 0).

**Causa** (docs/framebuffer.md, «Espejo del framebuffer»): el espejo
copiaba el framebuffer presentado en cada campo. SM64 dibuja un
framebuffer (tarea 95), carga por DMA ~200 KB de datos de la cara sobre dos
de sus framebuffers (`390000`–`3c2780`) mientras el VI todavía muestra uno,
y lo presenta un campo más: la copia pisaba esos datos (el código de la
cara los lee en el campo 354) y el juego se colgaba. Copiar «una vez al
presentar» no alcanza (ese framebuffer se presenta por primera vez después
del DMA). Además el host copiaba el alto conjeturado (`lh`) en vez de las
filas que muestra el VI: la intro de Conker se colgaba en la tarea 34 (en
la PS3, que copia las filas del VI, no).

**Arreglo**: lo dibujado en un framebuffer se copia a la RDRAM una vez por
dibujo, al terminar la tarea si el VI ya lo mostró alguna vez y si no al
presentarlo (`rdp_targets_mirror_at_task_end` / `rdp_targets_mirror_wanted`
en `src/rdp/targets.c`), igual en `src/rdp/rdp.c` (PS3) y en
`src/host/raster.c`. Pruebas: `targets_frame_mirror` (política) y
`raster_frame_mirror_sm64` (el rasterizador del host de punta a punta,
`tests/unit/test_raster.c`; falla con el raster.c de main).

**¿La PS3?** El camino existe ahí: `frame_mirror` de `rdp.c` copia el
presentado en cada campo desde bf836f2 y su copia llega un campo después,
o sea después del DMA del campo 343 (el host con ese retraso simulado
también se cuelga en la tarea 96). No hay registro de SM64 en la PS3 con el
espejo: `build/ps3-logs/sm64.log` no tiene «cuadros copiados» (anterior a
bf836f2). Hay que confirmarlo en la consola.

**Verificación** (host nativo, respaldos fuera del repo): SM64 660 tareas
en 1500 campos (como con `FBFX=1`) y hasta el castillo con el medidor de
energía igual que con `FBFX=1`; Conker 395 tareas (main 34; igual que
`FBFX=1`); Zelda: pausa (fondo difuminado) y pantalla de equipo con Link,
53 capturas idénticas a main (partida nueva «A» creada con el control);
Mario Kart en Luigi Raceway (`mk64-luigi.pad`) y GoldenEye idénticos a main
(el pad no llega al túnel); Banjo con la misma imagen de la HLE.
`test-host` 371/371, `test-roms` en verde (gfxtest 35/35 en las tres
variantes), `build.sh` sin avisos, `test-phase2` en verde (gfxtest 35/35
en RPCS3).

**Pendiente (PS3 real):** SM64 del logo a la cara de Mario y al castillo,
la cámara del túnel de Luigi Raceway, la pausa y la pantalla de equipo de
Zelda, Conker. Queda una carrera solo de la PS3: la copia llega a la RDRAM
uno o dos campos después de traducir la tarea, así que un DMA sobre un
framebuffer en el mismo campo en que el RDP lo terminó podría quedar
debajo (en SM64 pasan ~4 campos).

## 2026-10-11 — Efectos de framebuffer: render a textura y copias RSX → RDRAM (rama hopeful-dirac)

Diseño y hallazgos en `docs/framebuffer.md`; costos en `docs/perf-log.md`.
Resuelto y confirmado en la PS3 real con el paquete SLSA00089 («Salsa64
framebuffer»): el fondo de la pausa de Zelda OoT (nítido), Link en la
pantalla de equipo (dentro y fuera de la casa, cambiando de equipo), la
cámara del túnel de Luigi Raceway en Mario Kart 64 (el CPU fabrica esa
textura leyendo el framebuffer), gfxtest 35 de 35 (tres casos nuevos: render
a textura, copia a la RDRAM de una color image grande y de una chica que
solo lee el CPU) y sin regresiones: SM64 59,8, GoldenEye en Dam 45,8 (su
rango), Banjo 56–57, Conker 59,8 campos/s.

**Lo que hizo falta** (todo medido con datos de la consola, no a ciegas):

- Procedencia de las texturas en la HLE, tabla de destinos con política
  (`src/rdp/targets.c`), render a textura desde la superficie del RSX,
  copias RSX → RDRAM sincronizadas con la emulación, espejos asíncronos de
  las imágenes chicas y del framebuffer presentado cada campo, cebado de
  las superficies desde la RDRAM, VISCVG como cobertura plena.
- `rsxkit` solo admitía 8 superficies: la pausa de Zelda tiene 9 color
  images vivas más el scratch, la novena no se creaba y Link salía con
  basura del heap. Ahora 16, y el log cuenta «destinos sin superficie».
- La copia pedida se buscaba solo por dirección: `3da800` es el
  framebuffer (320) y el búfer de Link (64) a la vez y se copiaba el
  framebuffer entero (10 ms) en vez de Link.
- Con más de dos copias esperadas en una tarea, la tercera pisaba el
  búfer de la primera antes de que el RSX la leyera.
- La HLE emite dos color images imposibles al abrir la pausa (anchos de
  1495 y 3770): reciclaban ranuras buenas; ahora se ignoran.
- La cobertura que Zelda guarda para su filtro es una color image de 8
  bits de intensidad (`siz 1`): también se copia (las CI8 no).
- En la pausa alternan tres z-buffers; con dos profundidades se destruían
  y creaban a cada tarea (`rsxkit_finish` en cada una). Ahora cuatro.
- El modelo del host no llamaba a `rdp_targets_present`: copiaba todos los
  destinos de cada tarea y por eso «funcionaba» donde la consola no.

**Verificación (árbol final, con main traído):** `test-host` 369 pruebas
(617 939 verificaciones, 0 fallidas), `test-roms` en verde, `build.sh` sin
avisos, `test-phase2` en RPCS3 en verde (gfxtest incluido); `test-rec` en
tarea aparte (la parte unitaria 369/369 con el recompilador). En el host, gfxtest 35/35 con los tres microcódigos y la
pausa de Zelda en el bosque Kokiri y dentro de la casa (pads en
`docs/pads/`: `zelda-kokiri.pad` + `3600:START;…` llega a EQUIPMENT fuera
de la casa).

**Convivencia con la escala interna 1×–5× (fusión del mismo día).** La
escala interna llegó con escala por destino (`rdp_target_scale`,
`surf_scale`: con la objetivo 3×, el framebuffer de 320 × 240 va a 3×, Link
de 64 × 112 a 5× y un framebuffer de 640 × 480 a 1×), tope de memoria y
«Aplicar ahora»; los efectos de framebuffer suponían una escala global.
Al juntarlos (detalle en `docs/framebuffer.md`, «Escala por destino»):

- Todo lo que pasa de puntos lógicos a píxeles de una superficie usa la
  escala de esa superficie (`rdp_surface_rect`, `rdp_target_tex_map`):
  copias RSX → RDRAM (esperadas, grandes y espejos chicos), espejo del
  framebuffer presentado, render a textura (filas ligadas en la escala del
  destino leído; u y v normalizadas, así que el destino en que se dibuja
  puede tener otra) y el scratch, que toma tamaño y escala de la
  superficie copiada. Con la escala global, la copia de Link a 5× leía
  solo una esquina de su superficie y la de un destino a 1× se salía.
- Profundidades: si una z image la usan destinos de escalas distintas, se
  rehace en la misma entrada con el mayor tamaño de los dos (antes
  quedaban dos con la misma dirección y zclear borraba la vieja).
- Destinos ligados sin z image (cebado, scratch, dibujos sin z): iban
  con la profundidad de la pantalla, más chica que el destino a 4×–5× en
  720p (riesgo de que el RSX falle o recorte). Ahora la profundidad de la
  pantalla se agranda al mayor destino posible al elegir la escala
  (`rsxkit_depth_reserve`, `rdp_zeta_reserve`) y rsxkit se niega a ligar
  con una zeta más chica (`RSXKIT_EDEPTH`; el dibujo se salta y el log
  cuenta «dibujos sin ligar»). Elección explicada en `src/rdp/rdp.h`.
- Memoria: peor caso de 14 superficies (8 destinos, el scratch, 4
  profundidades y la profundidad de la pantalla agrandada), 102,5 MiB a
  5×: el tope pasó de 96 a 112 MiB (con 96, 5× se habría bajado a 4×). El
  scratch cuenta en el tope; `make_room` lo suelta primero y nunca recicla
  un destino que una textura pueda estar leyendo.
- Presentación: a múltiplo entero solo si llena al menos 3/4 del alto
  (`rdp_present_integer`); 2× en 720p salía a 640 × 480 en el centro.
- «Aplicar ahora» (`salsa64_rdp_set_scale`) suelta destinos, scratch y
  profundidades y descarta las copias en vuelo.

Verificación: `test-host` 374 pruebas (645 613 verificaciones, 0
fallidas; en `targets`, escala y peor caso, copia RSX → RDRAM simulada a
1×–5×, textura entre escalas y presentación entera), `test-roms` en verde
(gfxtest 35/35 con los tres microcódigos en el host), `build.sh` sin
avisos, `test-phase2` en RPCS3 en verde (gfxtest embebido y F3D 35/35; en
el menú, F3D a 2× tras «Aplicar ahora», 35/35) y gfxtest en RPCS3 con
`video.cfg` forzando la escala: 35/35 a 1×, 3× y 5×. Por confirmar en la
PS3 real: pausa y pantalla de equipo de Zelda y túnel de Mario Kart a 1×,
2× y 5×, y costo de las copias desde 5×. Con la profundidad de la
pantalla agrandada (commit aparte): `test-host` 375, `test-roms`,
`build.sh` y gfxtest en RPCS3 a 1× y 5× repetidos, todo en verde.

**Pendiente (otros trabajos):**

- [ ] Polígono transitorio (verde o amarillo, ~1 s) al abrir la pausa de
      Zelda en la PS3: volvió al caber los dos pares de búferes de Link;
      la causa más probable eran las dos profundidades (arreglado a cuatro
      en el último paquete, por confirmar en la consola).
- [ ] Las copias chicas que se repiten cada tarea sin que ningún dibujo
      las lea (Link redibujado en cada tarea de la pausa) se esperan solo
      las dos primeras veces y después van sin esperar: medir que la
      pausa baja de ~22 ms de espera por tarea a casi nada.
- [ ] Líneas del logo de N64 de Zelda: no era la presentación (el host
      también las dibuja): HLE del texto «NINTENDO 64» con la segunda
      textura de ruido.
- [ ] Túnica de Link con un defecto de textura (previo a la rama, también
      con `fbfx.cfg` en 0).
- [ ] Perfect Dark y Conker: copias del z-buffer (rectángulos que leen la
      z image como color image), sombra y desenfoque de Conker; color
      images de 32 bits no se copian ni se ceban.
- [ ] Reducir s× → 1× con filtro de caja (hoy bilineal del RSX en el
      centro de cada bloque) y, si pesa, la conversión en un SPE.
- [ ] Controles que a veces no responden en la pausa (también en SM64):
      arreglado en main (fba3a95), por confirmar aquí.

## 2026-10-11 — Recompilador: perfil por causa y menos salidas a C en GoldenEye y Perfect Dark (rama `upbeat-driscoll-1dd271`, paquete SLSA00088 «Salsa64 JIT»)

Hecho (sin la PS3: la noche del 10 al 11 se trabajó en qemu; la medición
en la consola queda para el día de pruebas):

- **Perfil del recompilador por causa** (`src/cpu/rec_prof.h/.c`, docs/rec-ppe.md
  «Perfil por causa»): tiempo con el timebase del PPE y cuentas de código
  generado, llamadas a C (cargas y almacenamientos lentos por tipo de
  dirección, instrucciones al intérprete por clase, excepciones), intérprete
  en el despachador por motivo, eventos por tipo (el del RSP incluye el
  sondeo y lo dormido esperando al SPE), compilar, `ktlb`, bucles de espera
  y salidas por tipo. En la PS3 lo enciende `/dev_hdd0/salsa64/perfil` y
  sale cada 5 s en ms por campo con las estadísticas; en el host,
  `SALSA64_REC_PROF=N` (cuentas cada N campos y, al final, los bloques más
  ejecutados con sus palabras y el uso de registros; `tools/mips-disasm.py`
  los lee).
- **Lo que las cuentas mandaron arreglar** (GoldenEye en la presa, qemu,
  por campo, antes → después): instrucciones interpretadas 13 900 → 500
  (cada COP0 terminaba el bloque y el intérprete seguía hasta el fin del
  bloque básico: ahora el bloque enlaza con el siguiente y `rec_h_exec`
  sincroniza la TLB del recompilador; tras un ERET tampoco se interpreta);
  traducciones de la TLB 14 000 → 360 revisiones (`x_sync` compara las 32
  entradas con una instantánea y borra solo las páginas de las que
  cambiaron; además prellena las de la entrada nueva); CACHE al intérprete
  1000 → 0 (nativo); salidas por JR 820 → 350 (`exit_jr` recorre la cadena
  de xhash); entradas desde C 1590 → 840. Perfect Dark tiene sus datos en
  una página de 4 MB mapeada (3200 cargas por campo por C en la intro): la
  `vtlb` pasó a ser `ktlb` (16 384 páginas, con el bit D) y las cargas y
  almacenamientos a direcciones mapeadas la prueban en línea («Datos
  mapeados por la TLB»). GoldenEye además compilaba cada (vpc, marco
  físico) como bloque nuevo al paginar su código (93 000 bloques y 6
  vaciados de la caché en 128 s): un bloque vivo de la misma vpc con los
  mismos bytes se reutiliza en la física nueva (`salsa64_rec_x_rekey`).
- **Bloques desprendidos** («Bloques desprendidos» en docs/rec-ppe.md):
  GoldenEye pagina su código y cada página vuelve en otro marco: compilaba
  cada (vpc, marco) de nuevo (101 900 bloques y 7 vaciados completos de la
  caché en 128 s). Un bloque mapeado invalidado se desprende con una copia
  de sus bytes y se engancha donde vuelva su página con los mismos bytes
  (parcheando la palabra de `ktlb` de su entrada): 40 850 bloques, 28 200
  enganchados, compilaciones por campo en la presa 22 → 0,7, vaciados 7 → 3.
- **Superbloques** («Saltos condicionales internos»): el perfil por bloque
  dio 5,3 instrucciones por bloque; un condicional hacia adelante ya no
  corta el bloque (su camino tomado sale enlazado, el no tomado sigue con
  los registros en el PPE). Los tres bucles más calientes de GoldenEye eran
  parejas de bloques.
- Caché de código de 16 → 24 MB (GoldenEye la llenaba cada ~40 s; ~900 bytes
  de PPC por bloque) y el camino de la `ktlb` en 8 rutinas compartidas del
  preámbulo (3 palabras por acceso en vez de ~22).
- Opción `SALSA64_REC_OPT_LEGACY` (`/dev_hdd0/salsa64/jit-viejo`,
  `SALSA64_REC_LEGACY=1`): el comportamiento anterior en el mismo binario,
  para el A/B en la consola. Bits `SALSA64_REC_OPTS` (host) para apagar una
  pieza a la vez al acotar una diferencia.
- **La referencia de `--lockstep` no tenía HLE** (gráfica ni de audio): no
  recibía la interrupción del DP de las tareas con HLE y GoldenEye y Perfect
  Dark, que ya dibujan, divergían igual con el camino viejo (bisect con los
  bits de arriba: todos fallaban en el mismo ciclo). Ahora lleva la misma
  HLE con su estado; con eso Perfect Dark pasa (200 M ciclos) y las corridas
  largas se anotan abajo.

**Para el día de pruebas en la PS3 (orden sugerido: al final, tras recorte →
líneas del logo → framebuffer):** la consola no respondía en la noche (FTP
sin conexión), así que el paquete está sin subir: desde este worktree
`PS3_IP=x.x.x.x tools/deploy-ps3.sh ftp build/salsa64-jit/salsa64-jit.pkg`
(el worktree no tiene `tools/ps3.conf`); instalarlo desde Package Manager
(SLSA00088 «Salsa64 JIT»), crear por FTP `/dev_hdd0/salsa64/perfil` (vacío:
`curl -T /dev/null ftp://x.x.x.x/dev_hdd0/salsa64/perfil`) y poner
`embedded` = `file:goldeneye.z64`.
1. Abrir «Salsa64 JIT», jugar la presa ~3 min (misma ruta que las
   mediciones anteriores), salir, bajar `/dev_hdd0/salsa64/salsa64.log`.
2. Crear `/dev_hdd0/salsa64/jit-viejo` (vacío) y repetir: es el «antes» con
   el mismo binario. Borrarlo después.
3. Si hay tiempo: Perfect Dark en un nivel (es el que más gana con los
   datos mapeados), y un minuto de Zelda, SM64 y Mario Kart (sin regresión).
Lo que hay que leer del log: la línea «campos/s … emulación X ms por campo»
y las de «perfil JIT por campo» (nativo, llamadas a C, intérprete, eventos,
compilar, ktlb, despachador) en el tramo de la presa; anotarlas en
docs/perf-log.md («Recompilador: perfil y salidas a C»).

Pruebas (todo con el árbol final, 2026-10-11):
- `tools/test-rec.sh --lockstep`: unitarias 351/351 en los cuatro modos,
  nemu64-test 433 fallas (línea base) con y sin pin+eager, vitest OK,
  `--lockstep` sin diferencias en nemu64-test y vitest.
- `tools/test-roms.sh`: nemu64-test 433 (HLE) y 136 (LLE), vitest,
  controles, savetest ×3, gfxtest ×3, audiotest: verde. `tools/build.sh
  test-host` 351/351.
- `--lockstep` de Perfect Dark (3 G ciclos, intro) y de GoldenEye con
  `goldeneye-dam.pad` hasta jugar en la presa (7,5 G ciclos, 283 M salidas
  comparadas, 1 h 46 min de qemu): **sin diferencias**.
- `tools/build.sh` (todos los programas) y el paquete: sin advertencias.
- **PS3 real (2026-10-11, tarde), A/B con el mismo paquete en la presa de
  GoldenEye:** 43,0 → 49,4 campos/s de media (+15 %), 37,5 → 50,6 MIPS
  (+35 %), 26,0 → 19,6 ns por instrucción ejecutada (−25 %); intérprete
  2,8 → 0,8 ms por campo, ktlb 3,8 → 1,0, despachador 1,0 → 0,0. **El
  nativo es el 80 % y cuesta ~50 ciclos del PPE por instrucción MIPS**, igual
  antes y después: ahí va el siguiente trabajo. Zelda, SM64 y Mario Kart
  a 59,4–59,5 campos/s (sin regresión). Tabla en docs/perf-log.md.

Pendiente / siguiente paso: con el desglose de la PS3 decidir el ataque al
código nativo (lo que no se ve en qemu): registros de la FPU residentes en
f14–f31 dentro del bloque (hoy cada operación pasa por `cpu->fpr`:
load-hit-store entre operaciones dependientes; GoldenEye 0,78 por bloque),
más registros fijos entre bloques (r26–r28 liberables; `at`, `s0`, `t6` son
los más usados sin fijar), MTC0 EntryLo/TLBWR/ERET con un ayudante propio
en vez de `exec_one` (GoldenEye: 2200 llamadas por campo del refill), y
seguir bajando el tamaño del código (la FPU y las salidas de los saltos
internos) para que la caché no se vacíe en la presa.

## 2026-10-11 — Pulsaciones perdidas del control: el bucle de presentación dejaba de leer los mandos (rama vibrant-hawking)

Reporte (el autor, PS3, 2026-10-10 y 11): a veces un botón no responde
hasta pulsarlo muchas veces (R en la pausa de Zelda, el stick en la
selección de ranura, también SM64); después «responde normal».

**Causa (log de la PS3 con los mandos instrumentados):** el hilo de
presentación pasó **7,4 s sin leer los mandos** en la selección de ranura
de Zelda (y 0,6–0,9 s en otros momentos) mientras presentaba 667 campos.
`drain()` consumía la cola hasta vaciarla, pero con el flip en vsync y la
emulación al mismo ritmo, mientras se presenta un campo llega el
siguiente: la cola no se vaciaba y el bucle no volvía a `update_pads`
hasta que los dos ritmos se desfasaban. Ahora `drain()` presenta solo lo
encolado al entrar. Lo que sí se leía llegaba siempre al juego (cada
«control 1» con su «PIF → juego» 1–3 campos después).

Medido en el host (`salsa64-run` nativo, mismas pulsaciones por campo):
- **Zelda lee el control en cada campo y acumula las pulsaciones**: en la
  pausa, R de 1, 2, 3 y 6 campos giran la página las 9 veces.
- **Zelda ignora R mientras gira la página** (~35 campos, 0,6 s): dos R
  separadas 15 o 30 campos dan un solo giro; a 45 o más, dos. Es del juego.
- **SM64 lee el control cada 2 campos**: una START de 1 campo en la fase sin
  lectura se perdía (3 de 6); con la retención del PIF, 6 de 6.

Hecho:
- **`drain()` acotado** (`src/frontend/main.c`): solo los campos que había
  al entrar; el bucle vuelve a leer los mandos en cada vuelta.
- **PIF** (`src/core/pif.c`): cada flanco de pulsación queda retenido
  (`held`) hasta salir en una respuesta de estado (comando 0x01); la
  identificación y el pak no lo gastan; desconectar o reiniciar lo olvida.
  Solo flancos: con todo lo pulsado, una A soltada 4 campos entre dos
  pulsaciones se perdía en SM64 (visto en la PS3). Guarda lo entregado
  (`sent`, `reads`) para el log. Prueba `pif_held_press`.
- **Buzón presentación → emulación**: además del seqlock, OR atómico de los
  botones recién pulsados (`pad_box.pressed`) que emulación toma y pone en
  0; los controles se aplican justo antes de emular el campo (antes, antes
  de la espera del ritmo: hasta ~12 ms más viejos).
- **Log de la PS3, solo en flancos**: «control N: … (campo M)» en
  `update_pads` y «PIF → juego: control N: … (campo M, lectura K)» cuando
  cambia lo que el PIF entregó (cola de 32 entre hilos, la escribe
  presentación). Cada 5 s: «controles: X lecturas de los mandos (hueco máx
  Y ms); el juego leyó el control 1 Z veces (W/s)». El host imprime el
  mismo «PIF → juego» con `--pad`.
- `docs/hilos-ppe.md` al día (fila de controles y de flancos del PIF).

**PS3 real** (paquete «Salsa64 control», SLSA00092; logs fuera de git):
- Antes de acotar `drain()`: huecos de 7402, 929, 831, 595 y 251 ms sin
  leer los mandos; el autor: el stick no bajaba en la selección de ranura.
- Después, Zelda: «ya funciona»; hueco máximo 19–31 ms en el juego (312 ms
  una vez, en la carga del título a la selección de ranura); 12 de 12
  pulsaciones entregadas.
- SM64 con la retención por flancos: «todo funciona perfecto»; hueco
  máximo 8,8–38 ms, el juego lee el control 30 veces/s; 75 de 75
  pulsaciones (A machacada, B, START) entregadas al juego.
- `embedded` quedó en `file:sm64.z64`.

Suites en verde con el código final: `tools/build.sh test-host` (356
pruebas), `tools/test-roms.sh`, `tools/build.sh` (sin avisos),
`tools/test-phase2.sh` (RPCS3) y `tools/test-rec.sh` (nemu64-test con las
433 fallas de siempre).

Pendiente:
- [ ] Integración en main.
- [ ] El hueco de 312 ms en la carga del título de Zelda (la presentación
      espera algo durante un campo de 170 ms de emulación): sin efecto en
      el juego; revisar si aparece en otra escena.

## 2026-10-11 — Kokiri y su tienda en colores planos: eran los fondos prerrenderizados (tarea JPEG y S2DEX)

Diagnóstico (rama `fervent-hodgkin-d38e1e`, en la Mac, sin PS3):
- **No era el combiner ni la HLE de F3DZEX.** La captura del host del
  campo 14400 (verde puro y un rectángulo azul) se reproduce bit a bit con
  la partida nueva del trabajo de framebuffer (`build/oot/pad4.txt` de su
  worktree, `MAX_PADS` subido a 2048 como en su rama). El píxel (100,150)
  lo escribe un dibujo **sin textura** de la propia sala: G_TEXTURE
  apagado, combiner SHADE·PRIM con PRIM = (0,161,0), iluminado; el azul es
  PRIM = (0,0,255) solo. La display list está en el archivo de la sala
  (segmento 3) y su cabecera de malla es de **tipo 1
  (ROOM_SHAPE_TYPE_IMAGE)**: imagen de 320 × 240 RGBA16 que dentro del
  archivo es un JPEG (`FFD8FFE0…JFIF`).
- La casa de Link (tras despertar, cámara cenital fija) y la tienda Kokiri
  son **salas prerrenderizadas**. El juego dibuja una malla «proxy» en
  colores PRIM planos solo para llenar el z-buffer y encima copia el fondo
  con S2DEX (`G_LOAD_UCODE` → `G_BG_COPY` con un `uObjBg` → vuelta a
  F3DZEX); el fondo lo decodifican al cargar la sala el CPU (Huffman) y 75
  tareas del RSP de tipo 4 (IDCT y YCbCr → RGBA5551). Salsa64 saltaba el
  tramo S2DEX (entrada del 2026-10-10 del cuelgue de Zelda) y las tareas
  JPEG terminaban sin hacer nada: se veía la proxy. Verde/azul en la casa,
  amarillo/naranja en la tienda.
- Referencias en la misma cámara: mupen64plus (GLide64 + rsp-hle, misma
  partida y pad) muestra el fondo de la casa; con `--lle=auto` (el
  microcódigo JPEG real) la RDRAM tiene la imagen bien decodificada y con
  la HLE de antes, basura.
- **El cierre del 2026-10-10 («sol cenital») vale para el bosque al aire
  libre** (balcón, campo ~3700): el piso hexagonal sale igual en
  mupen64plus. Lo que se reportó como «piso del bosque verde puro al salir
  de la casa» era la casa vista desde arriba.

Hecho:
- **HLE de la tarea JPEG de OoT** (`src/rsp/jpeg/`, nuevo): reconocida por
  el CRC-32 del microcódigo (4 KB, sin texto); por macrobloque, × tabla de
  cuantización ×16, zigzag, IDCT 8 × 8, YCbCr → RGBA5551 en su mismo lugar
  (modo 0 4:2:2 y modo 2 4:2:0). Algoritmo de `jpeg_decode_PS` de
  mupen64plus-rsp-hle (GPLv2+, `docs/THIRD_PARTY.md`), reescrito. Corre en
  el **PPE**, dentro del arranque de la tarea (`core/rsp.c`): 0,61 ms por
  sala en la Mac (300 macrobloques), una vez al cargarla.
- **S2DEX2 dentro de una tarea F3DEX2** (`src/rsp/gfx/s2dex.c`, nuevo): el
  `G_LOAD_UCODE` cuyos datos dicen «S2DEX … 2.x» ya no se salta; se
  interpretan G_DL/G_ENDDL, G_MOVEWORD de segmentos, othermode, comandos
  del RDP y **G_BG_COPY / G_BG_1CYC**: la imagen (con vuelta en x e y desde
  imageX/imageY) se carga a la TMEM por tiras con LOADTILE (las filas que
  caben en 4 KB, 2 KB con paleta; 6 filas de 320 en RGBA16) y cada tira es
  un TEXRECT (dos si da la vuelta en x), como hace el microcódigo: pasa por
  `rdp.c` como cualquier rectángulo, sin tocar `src/rdp` ni rsxkit. Las
  tiras se reutilizan entre cuadros por la caché de texturas (40 por
  cuadro, ~300 KB). Otros microcódigos ajenos se siguen saltando (gfxtest).
  Formato de `uObjBg` según GLideN64 (solo consulta).
- `salsa64-run --lle-check-jpeg`: las tareas JPEG al microcódigo real
  (implica `--lle=auto`) y la HLE sobre una copia de la RDRAM; compara al
  TASKDONE (no en el yield: el planificador de OoT interrumpe a veces una
  tarea JPEG para correr una gráfica). Estadísticas nuevas: `jpeg` (tareas,
  macrobloques, ms con `rsp.clock_us`) y `bg` (fondos de S2DEX) en el
  resumen del host y en el log de la PS3 («JPEG: N tareas … ms del PPE» al
  terminar cada tanda y «N fondos de S2DEX» en la línea `gfx:`).
- Receta de la tienda en el host (`docs/pads/README.md`): pad de Kokiri +
  `--poke` de la lista de salidas de la casa (0x0211 → 0x00C1).
- Pruebas: suite `jpeg` (5: DC por cuadrante, croma en modo 0, orientación
  del zigzag, IDCT contra una referencia en doble precisión, varios
  macrobloques y parámetros malos) y en `gfx` `gfx_s2dex_bg_copy`,
  `gfx_s2dex_bg_wrap` (vuelta en x e y, tres tiras) y `gfx_s2dex_bg_scaled`
  (G_BG_1CYC a escala 2; un S2DEX 1.x se sigue saltando).

Medido (host):
- **JPEG contra el microcódigo real**: casa (partida nueva, dos cargas, 150
  tareas, 153 600 píxeles) 99,0 % idénticos, 0,8 % a ±1 en un canal de 5
  bits; casa y tienda con la partida del autor, 99,2 % y el resto a ±1. El
  sesgo que quedaba en G (+1 en el 1,1 % de los píxeles) se quitó
  truncando cada producto de G como el punto fijo del RSP; lo que queda es
  ruido de la IDCT del RSP. 256 píxeles distintos de más en la partida
  nueva: un macrobloque que el **LLE** no escribe al reanudar una tarea JPEG
  interrumpida (yield); la HLE no se interrumpe. Pendiente del LLE, abajo.
- **Copia del fondo exacta**: en el campo 14400 el cuadro es la imagen
  RGBA16 de la RDRAM píxel a píxel salvo HUD, Link y Navi (94,7 % del
  cuadro). Contra mupen64plus (640 × 480 con filtro, reducido): mediana
  5,5/255, 91 % a ≤ 16/255.
- **Otros juegos sin cambios**: SM64 (pad del castillo), Mario Kart 64
  (demo), GoldenEye (pad de Dam) y Perfect Dark (intro): 48 capturas cada
  300 campos y los cuadros finales idénticos antes y después. En Zelda con
  el pad de Kokiri, idénticas dentro de la casa; en el balcón cambian
  Navi y las motas (378 píxeles): el juego diverge porque ahora su
  `gZBuffer` de la RDRAM (donde decodifica el JPEG) tiene la imagen y no
  basura, y el CPU lo lee.
- Local store del SPE 0: texto 97,0 KB (+4,8 KB de `s2dex.c`), pila libre
  ~13 KB y la cadena más honda ~8 KB (`-fstack-usage`).

Verificado:
- [x] `test-host` 363/363 (Docker, ASan/UBSan) y nativo; `test-roms`
      (nemu64-test 433 / 136 con LLE, vitest, controles, savetest,
      gfxtest 32/32 ×3, audiotest); `tools/build.sh` sin advertencias;
      `test-phase2` en RPCS3 (gfxtest 32/32 F3DEX2 y F3D con la HLE en el
      SPE, menú con cierre limpio); `test-rec`: ver abajo.
- [x] **PS3 real** (paquete «Salsa64 Kokiri», SLSA00091, 3b95776,
      `embedded` = `file:oot.z64`; el autor, 2026-10-11): la tienda Kokiri y
      la casa de Link «ya se ven bien». En el log, dos salas: «JPEG: 75
      tareas, 300 macrobloques en 67,8 / 67,3 ms del PPE», 0 con parámetros
      malos, y 14–47 «fondos de S2DEX» por ventana de 5 s. Las ventanas de
      100 campos con la carga de sala quedan en 57,3–57,6 campos/s con el
      peor campo en 37–39 ms de emulación (≈ 2 campos de tirón durante la
      transición; el resto, 5–8 ms por campo). Log en
      `build/ps3-logs/salsa64-kokiri.log` (fuera de git).
- **Decisión:** el JPEG se queda en el PPE (una vez por sala, durante la
  transición). Es ~110 veces más lento que en la Mac: flotantes con
  conversiones a entero (en el PPE pasan por memoria) y `lrintf`/`floorf`
  de la biblioteca; con una IDCT en punto fijo debería bajar a pocos ms
  sin pasar a un SPE (pendiente abajo).

Pendiente:
- [ ] HLE JPEG en el PPE: IDCT y conversión a RGBA5551 en punto fijo (sin
      flotantes ni `lrintf`/`floorf`), volver a medir contra el
      microcódigo real (`--lle-check-jpeg`) y en la PS3; meta < 10 ms por
      sala (hoy 68 ms).
- [ ] LLE: al reanudar una tarea JPEG interrumpida (OS_TASK_YIELDED) el
      último macrobloque queda sin escribir (tarea 49 de la partida nueva).
- [ ] LLE + RDP del host: los TEXRECT en modo copia que emite el S2DEX real
      salen negros con rayas (`--lle=all`, campo 14400); con la HLE el fondo
      sale bien.
- [ ] Otros usos de S2DEX: tareas que son S2DEX enteras (`SALSA64_GBI_S2DEX`,
      sprites) y S2DEX 1.x dentro de F3DEX 1.x siguen sin HLE.
- [ ] Majora's Mask y otros con tareas JPEG de otro microcódigo (Pokémon
      Stadium usa el mismo algoritmo con otro código: añadir su CRC al
      verlo).

## 2026-10-10 — Menú para elegir ROM (estilo XMB) y vuelta al menú con cierre limpio

Hecho (rama `intelligent-yalow-4dd14d`, desde main e7184b7):
- **Sin `embedded`, menú** (`src/frontend/menu.c`, `romlist.c`). Con
  `embedded`, todo arranca directo como antes (las pruebas, sin cambios).
  - Lista: `.z64/.v64/.n64` de `/dev_hdd0/salsa64` y
    `/dev_usb000…007/salsa64`, leyendo solo la cabecera (los tres órdenes de
    bytes). Ignora los «._*» de macOS. Título interno (0x20; si no hay, el
    nombre de archivo), código, región, tipo de guardado (tabla o
    guardado.cfg) y tamaño. Ordenada por título.
  - Disposición inspirada en el XMB, sin recursos de Sony: columna vertical
    con la fila elegida más grande y el nombre de archivo debajo, columna que
    se desliza con suavizado, panel de información que aparece al detenerse,
    fondo en degradado que cambia despacio con tres ondas animadas, texto con
    sombra, fundido de entrada y página de Opciones: fondo animado sí/no (en
    `SALSA64_DATA_DIR/menu.cfg`) y recorte del borde (el `overscan.cfg` del
    trabajo de presentación: Apagado, Normal 8/6, Amplio 16/12, o
    «Personalizado» si el archivo trae otros números; se relee al abrir cada
    juego). Íconos dibujados con rectángulos.
  - Controles: cruceta con repetición (pulsación nueva para dar la vuelta),
    izquierda/derecha para cambiar de página, X para abrir, triángulo para
    Opciones y círculo para volver. Recuerda la última ROM en
    `SALSA64_DATA_DIR/ultima-rom`. «Salir al XMB» va al final.
- **START + SELECT dentro del juego vuelve al menú.**
  - `main.c` quedó en sesiones de juego (`run_game`). `game_close` cierra
    en orden, con cada paso al log («cierre: …»): guardado al disco, audio,
    HLE gráfica y LLE con sus SPEs, recompilador, colas, destinos del RSX
    (`salsa64_rdp_reset`, que conserva la memoria mapeada). Termina en
    «cierre: estado limpio» o en «ATENCIÓN, quedó algo en vuelo».
  - `game_reset` pone en cero el resto (incluidos estáticos que antes
    sobrevivían: el último campo encolado, las ventanas de estadísticas, la
    línea del ISViewer). `audio_out` quedó reentrante. «Salir del juego» del
    sistema cierra la aplicación desde el juego o desde el menú.
- **lib/rsxkit (MIT):**
  - `rsxkit/text.h`: texto en un lienzo de la CPU. La fuente es la «fixed»
    8x13 de X11, de dominio público (`docs/THIRD_PARTY.md`).
  - `rsxkit/ui.h`: 2D en el RSX (rectángulos con degradado, texto con un
    atlas Scale2x y filtro lineal, ondas) sobre la pantalla del cuadro
    (`rsxkit_bind_frame`).
  - En RPCS3 la mezcla alfa dentro de **una sola** llamada de dibujo no ve
    lo dibujado antes en esa misma llamada (lo translúcido salía como sobre
    negro). Por eso va una llamada por cada elemento, con el estado en
    caché. Anotado en `lib/rsxkit/README.md`.
- Memoria: el menú reserva 2 MB mapeados para el 2D; la ROM va en un búfer
  a su medida, reservado al abrir cada juego y liberado al cerrarlo
  (`docs/memory-budget.md`).

Pruebas:
- Host: suite «menu», 15 pruebas: cabeceras en los tres órdenes, títulos,
  orden, directorio, última ROM, navegación, opciones, ajustes, atlas,
  geometría y cuadros del menú. Con `SALSA64_MENU_PPM` se obtiene una vista
  previa rasterizada como en el RSX (en `build/host/`).
- `tools/rpcs3-run.py --menu ROM…` (en `tools/test-phase2.sh`): el menú se
  maneja solo. Abre gfxtest, audiotest y gfxtest F3D seguidos y cada uno pasa
  su verificación tras el cierre del anterior, con «cierre: estado limpio».
  Deja una captura del menú en `dev_hdd0/salsa64/menu.ppm`.
- `test-host`, `test-roms` (nemu64-test 433 fallas con HLE y 136 con LLE,
  vitest, controles, savetest, gfxtest ×3, audiotest), build sin
  advertencias, `test-phase1`, `test-phase2` y `test-rec`: verdes.

- **PS3 real** (paquete «Salsa64 menú», SLSA00086, con main 5c2b00e
  fusionado): el autor confirma que funciona correctamente.
  - El menú listó los 8 respaldos de `/dev_hdd0/salsa64`.
  - Se abrió Mario Kart 64; START + SELECT volvió al menú, con todos los
    pasos del cierre en 0 y «cierre: estado limpio».
  - Perfect Dark escribió su EEPROM de 16 Kbit durante el juego («escrito
    diferido»), volvió al menú y, al reabrirlo, se leyeron sus 2048 bytes:
    el guardado se conserva.
  - «Salir al XMB» terminó en «salida normal» y volvió al XMB sin pantalla
    negra.
  - Cada juego arrancó con el recompilador vacío y el audio al 99–101 %
    (log en `build/ps3-logs/salsa64-menu.log`, fuera de git).

Falta (opcional):
- [ ] Medir el menú en la PS3 (cuadros por segundo con ondas; hoy no lo
      escribe al log).
- [ ] Probar el menú en una tele 4:3 o a 1080p (la vista previa del host
      cubre 4:3).

## 2026-10-10 — Mario Kart 64: polígonos estirados en los zooms (recorte del RSP en la HLE)

Reporte (el autor, PS3, «Salsa64 alfa» SLSA00085): en Mario Kart 64, al
pasar del título a lo que sigue, «polígonos mal calculados, donde se queda
una punta y se estira»; después precisó: «una pantalla que se hace pequeña,
como un gameplay que sale».

Diagnóstico (rama `reverent-meninsky-b3f79c`, en la Mac):
- No era del RSX ni de la vida de los juegos de búferes de src/rdp: con
  `salsa64-run` se simuló un SPE lento (`SALSA64_GFX_LATE=N` retrasa la HLE
  N sondeos; con `SALSA64_GFX_TEAR` la corre también al arrancar y compara)
  y en 245 tareas ninguna lectura de la RDRAM cambió durante la tarea; la
  invalidación de la caché de vértices de PSL1GHT (`rsxInvalidateVertexCache`
  escribe 0x1710 y 0x1714, como libgcm) está bien.
- El cuadro malo no es título → «GAME SELECT» (solo rectángulos, sale
  limpio) sino los **zooms de la demo**: entra creciendo desde una ventana
  (campos ~1106–1141 tras START en el 400) y al terminar se encoge
  (3300–3350 sin tocar nada). El juego los dibuja con un viewport chico que
  crece (`G_MV_VIEWPORT`), tijeras del RDP hasta (0, 0) y
  `gSPClipRatio(FRUSTRATIO_1)` cada cuadro. En el RSP real los triángulos se
  recortan contra el viewport por la clip ratio y contra el plano cercano; la
  HLE no hacía ninguna de las dos cosas: el suelo cruzaba el plano cercano
  (w → 0, x/w de −8000) y salía de la ventana como una punta hacia la
  esquina. Reproducido en el rasterizador de la Mac (campos 3310–3350 sin
  pulsar nada); la otra rama miró la transición a «GAME SELECT», que es de
  rectángulos, y por eso se vio limpia.

Arreglo (`src/rsp/gfx`; nada en src/rdp ni en lib/rsxkit):
- `G_MW_CLIP` (F3D, F3DEX, F3DEX2, el de Conker) guarda la clip ratio y
  cada tarea arranca con la de los datos del microcódigo (vuelven a la
  DMEM): **1** en la primera generación (F3D 2.0D/2.0G, F3DEX 0.95/1.21/1.22,
  el de Perfect Dark) y **2** en la segunda (F3DZEX), leído de los datos de
  los ucodes de los respaldos (offset 0x70 y 0x180: (1,0,0,r) (0,1,0,r)
  (1,0,0,−r) (0,1,0,−r)). La caja viewport × ratio se suma a las tijeras de
  los dibujos de triángulos (los rectángulos no pasan por el RSP); un
  cambio de viewport abre dibujo nuevo. GoldenEye y Perfect Dark tienen
  viewport = tijeras y no cambian.
- Recorte contra el plano cercano (z = −w) en `gfx_triangle`: un vértice
  detrás → dos triángulos, dos → uno, tres → nada; interpolación lineal en
  coordenadas de recorte, como el RSP. Los microcódigos «.NoN» (Zelda OoT
  F3DZEX.NoN, Star Fox 64 F3DEX.NoN, Conker F3DEXBG.NoN) no recortan: el
  núcleo lo lee del texto del ucode (`rsp->gfx_nonear`) y va en
  `GFX_JOB_NONEAR`. El de Perfect Dark (sin texto) se toma como NoN, **sin
  verificar contra hardware**: el autor ya lo jugó sin recorte y GLide64
  dibuja los muros cercanos de su intro que el recorte quitaría (cuadro
  3000, un muro a la izquierda).
- Efecto en otros juegos (rasterizador de la Mac, main contra la rama, un
  cuadro cada 100 campos): Zelda, Star Fox, Conker, Perfect Dark y los menús
  de Mario Kart idénticos. **SM64** en el castillo: el suelo cerca de la
  cámara (cruza el plano cercano) se descartaba entero y se veía el agua del
  foso debajo; ahora se dibuja, como en mupen64plus. **Banjo-Kazooie**: en
  la intro unos polígonos delante del plano cercano tapaban todo de negro;
  ahora se ve el logo con la abeja. **GoldenEye**: diferencias chicas dentro
  de modelos que cruzan el plano cercano (créditos), a la vista iguales.
- Pruebas: unitarias `f3dex_clip_ratio`, `f3d_near_clip`,
  `gfx_clip_ratio_f3dex2` y `rsp_gfx_nonear`; gfxtest con las escenas O
  (viewport chico + clip ratio 1 con tijeras de toda la pantalla) y R
  (cuadro que cruza el plano cercano): 30 de 30 puntos en el host (tres
  variantes) y en RPCS3 (F3DEX2 y F3D, pantalla del RSX); con main fallan
  4 puntos. Con main fusionado (generador de fragment programs, escenas P y
  Q): 329 unitarias y 32 de 32 puntos. `tools/build.sh test-host`,
  `tools/test-roms.sh`, `tools/build.sh` sin advertencias y
  `tools/test-phase2.sh`.
- La traza por dibujo de `salsa64-run` imprime además x/w, y/w, tijeras,
  viewport y la dirección de la textura.
- Respaldo propio de Mario Kart (`mk64.z64`) fuera del repo y en el
  dev_hdd0 de RPCS3.

Pendiente:
- [x] **PS3 real** con `build/salsa64/salsa64-recorte.pkg` (SLSA00090,
      «Salsa64 recorte»; el SLSA00086 que había elegido el autor ya lo usa
      «Salsa64 menú»): el autor confirmó que en Mario Kart «ya no se ve» lo
      que reportaba (log `build/ps3-logs/mk64-recorte.log`, fuera de git:
      sin errores, 60 campos/s).
- [x] PS3 real, SM64 en el castillo: «todo se ve bien» (el autor; log
      `build/ps3-logs/sm64-recorte.log`).
- [ ] **GoldenEye en la PS3 (noche del 2026-10-10): líneas azules finas en
      uniones de polígonos** que corren hacia la cámara (centro de la
      carretera del túnel de Dam, base de la barrera; fotos del autor). Sin
      atribuir. Lo que se descartó en el host: el punto de corte de una
      arista compartida se calcula en distinto orden en cada triángulo
      vecino, pero coincide hasta 0,001 px (`SALSA64_GFX_CUTCHECK`, 3572
      vértices cortados en 619 campos de Dam) y el rasterizador de la Mac no
      muestra ninguna grieta en las uniones con esta rama; las líneas no
      corren a lo largo del corte del plano cercano (sería horizontal, abajo).
      No se llegó al túnel sin manos (dos guiones chocan con paredes). El
      cian de GoldenEye ya conocido era otra cosa (soldados y armas, por el
      combiner). **Decide un A/B en la PS3**: «Salsa64 menú» o «Salsa64 alfa»
      (main sin este recorte) en el mismo túnel. Si también salen, pendiente
      aparte (no de esta rama); si no, el sospechoso es cómo el RSX
      rasteriza los triángulos cortados (volcado `gfxdump` del campo malo
      para dibujarlo en la Mac con `--raster-dump`).
      **A/B hecho el 2026-10-11 (el autor, PS3): las líneas salen igual con
      «Salsa64 menú» (main sin este recorte)** → no son de esta rama;
      pendiente aparte, fuera de ella (logs `build/ps3-logs/goldeneye-recorte-2.log`
      y `goldeneye-menu.log`, fuera de git). Fotos y logs guardados fuera
      del repo.
- [x] PS3 real, Perfect Dark con «Salsa64 recorte» (tomado como NoN): intro
      de la ciudad y un nivel, «se ve todo bien» (el autor, 2026-10-11; log
      `build/ps3-logs/perfectdark-recorte.log`).

Lo que se vio el 2026-10-11 en la PS3 (paquete «Salsa64 recorte», SLSA00090;
`embedded` en /dev_hdd0/salsa64/, se cambia por FTP; resultados arriba):
1. `file:goldeneye.z64` con «Salsa64 recorte» y luego la misma escena con
   «Salsa64 menú» o «Salsa64 alfa»: túnel de Dam con las barreras a rayas,
   mirando el centro de la carretera y la base de la barrera. Lo esperado si
   es de esta rama: líneas azules solo en «recorte».
2. `file:perfectdark.z64`: intro (vuelo por la ciudad) y un nivel. Se toma
   como NoN: lo esperado es verlo como antes; si hay puntas estiradas en
   muros cercanos, su microcódigo sí recorta (una línea en
   `src/rsp/gfx/sync.c`); si hay huecos negros, no.
3. Ya confirmados hoy: Mario Kart (demo al entrar y al encogerse, sin
   puntas) y SM64 en el castillo (suelo junto al foso), «todo se ve bien».
- [ ] En RPCS3 el modo de prueba automática sale a los 1800 campos y el
      zoom sin tocar nada cae en el 3300 (`capturas` "25 3500" no llega):
      dejar que el M de `capturas N M` mande sobre AUTO_FIELDS si hace falta
      verlo ahí.
- [ ] Banjo-Kazooie en el host se queda en el logo de Rare (tarea del campo
      68, luego solo audio): no es de esta rama.

## 2026-10-10 (noche) — Resumen del día y problemas abiertos

**Integrado y subido hoy** (main 8231305): controles y bucles de espera, guardado
del cartucho, Fase 2 gráfica (F3DEX2 en el SPE 0), recompilador (Fase 3) con TLB,
FPU/LWL/LWR/MFC0 nativos y bucles en código mapeado, dos hilos del PPE con
limitador, caché de texturas, fin de tarea del RSP por sondeo, audio en el SPE 1
(ABI1, nead y naudio con MP3; FILTER y MIXER contra el microcódigo real), F3D,
F3DEX 1.x/0.95, Fast3D 2.0G, menús de Mario Kart, presentación de GoldenEye,
arranque del CIC 6105/osCicId, censo (docs/compatibilidad.md), arreglo del
cuelgue de Zelda, microcódigos de Rare (Perfect Dark, Conker), prototipo de LLE
del RSP, generador de fragment programs (Fase 4), G_RM_TEX_EDGE (medidor de
SM64), candado de RPCS3, qemu con power9, logo y fondo del XMB.

**En la PS3 real:** Zelda, SM64, Mario Kart, GoldenEye y Perfect Dark se juegan
con audio; Zelda/SM64/MK64 a velocidad completa; GoldenEye en la presa 34–55
campos/s (emulación 18–29 ms por campo: el CPU es cuello); Perfect Dark al 100 %
en intro/menús (falta medir un nivel). Primer video público: GoldenEye.

**Integrado después del cierre:** presentación (`bold-neumann-794fc0`,
merge b166a06): Banjo sin parpadeo, Conker sin quemado, pausa de Zelda visible,
overscan. La «regresión» de GoldenEye (soldados cian, armas azul claro) era la
aproximación del combiner con ENV de la HLE vieja, idéntica en SLSA00064; con el
generador el autor vio GoldenEye normal en la PS3. Después, el menú estilo XMB
(`intelligent-yalow-4dd14d`, merge 00eba22), confirmado en la PS3.

**Integrado el 2026-10-11:** recorte del RSP (`reverent-meninsky-b3f79c`,
«Salsa64 recorte» SLSA00090): en la PS3, Mario Kart sin polígonos estirados,
SM64 y Perfect Dark bien. Las líneas azules de GoldenEye salen también sin el
recorte (A/B en la PS3): pendiente aparte, abajo. Después, el mismo día:
control (pulsaciones perdidas), fondos prerrenderizados de Zelda (JPEG + S2DEX),
recompilador con perfil por causa, efectos de framebuffer (4baa6df) y el arreglo
del espejo del framebuffer que colgaba SM64 en la cara de Mario (84ea55f; en la
PS3 sin confirmar).

**Integrado después, el 2026-10-11:** resolución 720p/1080p y escala
interna 1×–5× por destino con «Aplicar ahora» (rama
`dev-a1388b259074798ab`, 1647a25), estados guardados con contador de FPS
(`wonderful-blackwell-6f40d3`, SLSA00094) y FPU en registros, COP0 nativo y
caché por tramos (`jovial-jones-2408ac`, SLSA00093). Detalle en las entradas
de arriba y en docs/framebuffer.md.

**Problemas abiertos:**
- Líneas como de entrelazado en el logo de N64 de Zelda en la PS3; en el
  rasterizador de la Mac el cuadro sale limpio. Sospecha: el recorte del
  overscan escala 684 líneas a 720 con filtro lineal; probar con
  «Recorte del borde: Apagado» en el menú. Arreglo propuesto: tapar los bordes
  con negro sin escalar.
- **Líneas azules en uniones de GoldenEye** (PS3 real, túnel de Dam): línea fina
  del color del relleno de fondo a lo largo de uniones de polígonos que corren
  hacia la cámara (centro de la carretera, base de la barrera). Salen igual con
  «Salsa64 recorte» y con «Salsa64 menú» (A/B del 2026-10-11): no son del
  recorte, están en lo común (rsxkit, src/rdp o la HLE). En el host no se
  reproducen (sin grietas en el rasterizador de la Mac; los cortes de aristas
  vecinas coinciden a 0,001 px). Siguiente paso: gfxdump del campo malo en la
  PS3 y dibujarlo en la Mac con `--raster-dump`; si allí sale limpio, es la
  rasterización del RSX (reglas de cobertura o vértices que no caen en el mismo
  punto tras el redondeo). Evidencia: fotos y logs, fuera del repo por ser
  imágenes de un juego comercial.
- **Kokiri en colores planos, reabierto (2026-10-11).** *Resuelto en la rama
  `fervent-hodgkin-d38e1e` (entrada de arriba): eran las salas
  prerrenderizadas (fondo JPEG + S2DEX sin HLE); lo de abajo era la casa vista
  desde arriba, no el piso del bosque.* El cierre del 2026-10-10
  («es el sol cenital») no explica lo que se ve al salir de la casa de Link: en
  el host (campo 14400 con pad) el piso es verde puro, (0,71,0), (0,104,0),
  (0,161,0)…, con rojo y azul en 0, y un rectángulo azul liso. Ninguna textura
  × iluminación da eso; aquella comparación con mupen64plus fue en el balcón
  (campo ~3700) y midió colores con rojo y azul. El autor lo ve como error en la
  PS3. También en la tienda del bosque Kokiri (foto en la PS3, 2026-10-11):
  piso amarillo y paredes y mostrador naranja lisos, con los personajes y
  objetos texturizados; igual con `fbfx.cfg` = 0, así que es de la HLE. Afecta
  a las mallas de las salas, no a los actores. Evidencia fuera del repo.
  Siguiente: diagnosticar Kokiri y su tienda en colores planos (SLSA00091).
- **Pulsaciones perdidas** (PS3, 2026-10-11): resuelto en la rama
  vibrant-hawking (ver la entrada del 2026-10-11 arriba): el bucle de
  presentación dejaba de leer los mandos hasta 7,4 s; confirmado por
  el autor en Zelda y SM64 (SLSA00092).

**Siguientes pasos:**
- Recompilador: más velocidad del CPU en GoldenEye y Perfect Dark.
- Efectos de framebuffer (pendiente de abajo), sobre la presentación ya en main.
- HLE: **cara de Conker** (polígonos negros en la boca, visto en la PS3; no lo
  explicaba la presentación). Lo de Banjo (z/w > 1, modelos que tiemblan,
  cielo) era la protección anticopia del 6103 (entrada del 2026-10-12).
- Gráficos pendientes (actualizado 2026-10-11): líneas del logo de Zelda (la
  HLE muestrea mal la segunda textura de ruido del texto «NINTENDO 64»),
  defecto en la túnica de Link, líneas azules en uniones de GoldenEye.
- **Mario Kart, Luigi Raceway: negro arriba del muro de ladrillos** (PS3,
  2026-10-11, en todos los paquetes): debería verse el cielo en degradado y el
  fondo de la pista. Previo a las ramas de hoy; candidato: cómo se dibuja el
  cielo (rectángulo o triángulos con degradado al fondo). Reproducir en el host
  con docs/pads/mk64-luigi.pad y comparar contra mupen64plus.
- Zelda: fondos prerrenderizados hechos (2026-10-11); JPEG a punto fijo
  (68 ms por sala en el PPE, meta < 10 ms); luego Majora's Mask.
- Recompilador: FPU en registros, más registros fijos y refill de la TLB
  (en curso desde el 2026-10-11).
- **Recompilador, siguiente frente (medido en la PS3 el 2026-10-11):** el
  código nativo cuesta ~47 ciclos del PPE por instrucción MIPS con cualquier
  combinación de FPU, tramos y COP0; no lo limitan las instrucciones emitidas
  sino el tamaño y la localidad del código (~7 MB de bloques vivos en GoldenEye
  contra 512 KB de L2, caché en páginas de 64 KB). Trabajo: código más compacto
  y bloques calientes juntos. La FPU residente quedó apagada (más lenta).
- **Perfect Dark en un nivel a 49–50 campos/s por el lado gráfico:** 14 ms por
  campo esperando al SPE 0, ~4500 triángulos y ~131 texturas decodificadas por
  tarea (el CPU usa 4,5 ms). Candidato fuerte para pasar la conversión de
  texturas a los SPEs 2–3.
- **Quitarle trabajo al PPE** (decidido el 2026-10-11, después de cerrar los
  trabajos abiertos): el VR4300 se queda en el PPE (memoria, FPU IEEE y saltos
  no le van a un SPE), pero lo demás pasa a los SPEs libres (2–5) para que el
  hilo 0 tenga el núcleo: tarea JPEG de Zelda (68 ms en el PPE), conversión de
  texturas, lo que quede de la presentación del hilo 1. Ordenar con
  docs/uso-cell.md (uso medido por componente y por juego).
- Fase 5 (posprocesado, MSAA) después de presentación y menú; recompilador
  RSP→SPU para el LLE; conversión genérica de texturas en lib/rsxkit/spu.

**Notas:** el input lag de Zelda es antiguo y solo en Zelda (SM64 bien);
sospecha: los dos hilos más el retraso propio de Zelda; presentación agregó al
log la línea «retraso: cola … destino …» para medirlo.
`embedded` quedó en el último valor que dejó presentación. Con el menú en main:
si existe `/dev_hdd0/salsa64/embedded`, cualquier app (también la principal)
arranca directo esa ROM sin menú; el menú sale solo cuando el archivo no existe.

## 2026-10-10 — Fase 4: generador de fragment programs del RSX en tiempo de ejecución (rama compassionate-shirley)

Hecho:
- **`lib/rsxkit` emite microcódigo de fragment program** (familia NV40) sin
  compilador Cg en la consola: `include/rsxkit/fpgen.h` + `src/fpgen.c`
  (codificador, C puro, sin PSL1GHT) y `src/fpcache.c` (caché por hash de
  la descripción canónica, 128 programas en un pool de 96 KB de memoria
  local del RSX; si se llena, espera al RSX y se vacía). La descripción es
  genérica: `color = saturate(Σ_j (Π factores_j) · X_j)` con factores T0,
  T0a, T1, T1a, C (color del vértice) y Ca, hasta 7 términos (un varying
  TEXCOORD1..7 cada uno) de 4 factores; los `X_j = k0 + k1·C + k2·Ca` los
  sigue armando el vertex program (`rsxkit_lincomb.vcg`, ahora con 7 X, 27
  constantes, COLOR0 de salida y `fogsel` para sumar la niebla al término
  sin factores). La combinación lineal fija (`nterms = 0`) sigue como
  camino rápido con sus tres programas precompilados. API en
  `lib/rsxkit/README.md`; formato en `docs/rsx-notes.md` y en la cabecera
  de `fpgen.c` (fuentes MIT de cgcomp, `docs/THIRD_PARTY.md`).
- **Verificación en el host sin consola:** `tests/unit/test_fpgen.c` trae
  un intérprete del microcódigo (TEX, MOV, MUL, ADD, MAD, swizzle, H/R,
  saturación) anclado a los `.fpo` que cgcomp compila de los shaders de la
  combinación lineal y de tres shaders de referencia con productos
  (`tests/unit/fpgen/*.fcg`, `tools/fpgen-ref.sh` → `tests/unit/fpgen_ref.h`):
  primero los programas de cgcomp dan la fórmula de su shader, luego los
  generados dan la misma, y para las descripciones equivalentes leen los
  mismos TEXCOORD. Más pruebas de forma (fin de programa, un solo registro
  de entrada por instrucción, perspectiva, máscara, R0 saturado al final),
  canonización, hash y rechazo de descripciones inválidas.
- **`src/rsp/gfx/combine.c`** evalúa el combiner con monomios dispersos
  (cada textura a lo sumo una vez, dos texturas, hasta tres factores de
  sombreado) y, si el resultado no cabe en la combinación lineal (o si
  pide T0·T0a y T0 a la vez), lo agrupa en el menor número de términos
  (cada X_j absorbe un factor de sombreado) en orden canónico, con el alfa
  compartiendo términos con el color. `gfx_draw` lleva `nterms` y
  `factors` (480 bytes; `GFX_K_TERMS` = 7); `src/rdp` los pasa a rsxkit y
  el rasterizador del host los evalúa igual. El búfer de comandos de cada
  juego pasa de 512 a 768 KB a costa del arena de texturas.
- **gfxtest** tiene dos cuadros nuevos (N: TEXEL0·TEXEL1·SHADE; O:
  lerp(ENV, TEXEL0, SHADE_ALPHA)·SHADE, la niebla por luz de GoldenEye) con
  sus puntos en `expect.h`; las tres variantes dan 24/24 en el host y
  `tools/test-roms.sh` exige 0 combiners aproximados. ROMs recompiladas
  (SHA-256 nuevos).
- **GoldenEye en Dam (host):** 97 449 → **0 combiners aproximados** en la
  misma corrida (`--pad` con START y A pulsados y soltados cada 240
  campos, 7 500 M ciclos). En el nivel solo dos combiners
  (`159804:5ffedbf8` y `159a04:5ffefff8`) van por el generador: 3 términos
  {}, {T0, C}, {C}.
- Estadísticas nuevas en el log del frontend («gfx: fragment programs:
  cambios por tarea, dibujos con programa generado, programas en la
  caché»).

Decisiones:
- Una sola tabla combiner → programa, en rsxkit, por hash de la
  descripción: la estructura de los términos depende también de los
  valores de PRIM/ENV (un coeficiente en cero quita un término), así que
  una tabla por palabras de G_SETCOMBINE en src/rdp sería incorrecta.
- Operaciones en fp16 como las de cgcomp (entradas de 8 bits); R0 al
  final con saturación. Registros: H2 acumula, H3/H4 texturas, H5
  productos (`num_regs` ≤ 3).
- La niebla sigue en el vertex program: escala por (1 − C.a) y suma
  fog·C.a al primer término sin factores (`fogsel`); combine.c garantiza
  ese término cuando hay niebla.

Verificado:
- [x] RPCS3: `tools/test-phase2.sh`: gfxtest en el frontend (HLE en el SPE,
      RSX con programas generados) 24/24 puntos en F3DEX2 y F3D; 2
      programas generados, 9 cambios de programa por tarea.
- [x] `tools/test-rec.sh` (qemu ppc64, big-endian): 324/324 unitarias en
      los cuatro modos del recompilador (el intérprete del microcódigo y
      el generador también en big-endian), nemu64-test con las 433 fallas
      de la línea base, vitest OK.
- [x] Con main 5a0a1a3 fusionado (ad14e4b; gfxtest con el cuadro de
      G_RM_TEX_EDGE de main como N y los del generador como P y Q, 26
      puntos): test-host 325/325, test-roms 26/26 en las tres variantes,
      RPCS3 26/26 en F3DEX2 y F3D, compilación PS3 sin advertencias,
      test-rec en los cuatro modos del recompilador.

- [x] **PS3 real** (SLSA00087 «Salsa64 fpgen», 136dff4 y luego ad14e4b con
      main fusionado; el autor, 2026-10-10): GoldenEye 238 s hasta Dam con
      la niebla por luz exacta en las capturas (34–55 campos/s en el
      nivel, 4 programas generados, hasta 131 dibujos por tarea con
      programa generado, 0 vaciados); Zelda OoT (144 s, Kokiri), SM64
      (166 s, castillo; el medidor de energía ya sin fondo con main
      fusionado) y Mario Kart 64 (81 s, pista) sin regresión visual, 0
      programas generados y 54–60 campos/s. Números en
      `docs/perf-log.md`.

Pendiente:
- [ ] Perfect Dark en la PS3 con el generador (su combiner de niebla por
      luz es el de GoldenEye; en el host ya no se aproxima).
- [ ] Medir el tiempo del RSX por cuadro cuando haya contadores (hoy solo
      se mide el PPE).
## 2026-10-10 — Medidor de energía de SM64 con fondo opaco: G_RM_TEX_EDGE

Reporte (el autor, PS3, main de hoy): en Super Mario 64, al nadar, el
medidor de energía («POWER», rueda azul y mano) salía con un cuadro café
opaco detrás, del tamaño de su textura. Vidas, estrellas, agua y peces bien.

Diagnóstico (rama `modest-archimedes-e639a4`, en la Mac):
- Reproducido con `salsa64-run` nativo: `docs/pads/sm64-castillo.pad` lleva
  a Mario al puente del castillo y `--poke "4200:8033B21E=0x0400"` (opción
  nueva: escribe la RDRAM al empezar un campo) le baja la energía para que
  aparezca el medidor. El rasterizador de referencia lo dibujaba igual de
  opaco: era de la HLE, no de la traducción al RSX.
- Con la traza por dibujo (`-DSALSA64_GFX_TRACE`): el medidor son dos
  cuadros de 32×64 RGBA16 y la rueda de 32×32, en 1 ciclo, combiner
  TEXEL0/TEXEL0 y othermode L 0x0F0A7008 = `G_RM_TEX_EDGE`: CVG_X_ALPHA |
  ALPHA_CVG_SEL | **FORCE_BL** | AA_EN, blender in·0 + in·1. `blend_state`
  solo ponía la prueba de alfa de CVG_X_ALPHA sin FORCE_BL (las
  `G_RM_AA_*_TEX_EDGE`), así que salía sin prueba de alfa ni mezcla.
- En el RDP la cobertura pasa a ser cvg·α (cvg = α/32 con el píxel
  cubierto entero) y con AA_EN no se escriben los píxeles de cobertura 0.
- No venía de 8ec50de (TEXRECT sin costuras): la condición estaba así desde
  la primera HLE del RDP (87592d1).

Arreglo (`src/rsp/gfx/rdp.c`, `blend_state`; nada en src/rdp): con
CVG_X_ALPHA, FORCE_BL y AA_EN, prueba de alfa ≥ 32 (el RSX la recibe como
`alpha_ref` 32). Sin AA_EN la cobertura no depende del alfa y no se prueba;
sin FORCE_BL sigue el corte en 128 de antes.
- Prueba unitaria `gfx_cvg_x_alpha` (G_RM_AA_ZB_TEX_EDGE → 128,
  G_RM_TEX_EDGE → 32, sin AA_EN y G_RM_OPA_SURF → sin prueba) y escena N de
  gfxtest: cuadro con textura RGBA16 de alfa de 1 bit en G_RM_TEX_EDGE; sin
  el arreglo el texel con alfa 0 sale café (0xA55221). Ahora 24 de 24
  puntos en las tres variantes, en el host y en RPCS3 (pantalla del RSX).
- Sin cambios en otras escenas (comparación píxel a píxel del rasterizador
  de la Mac, antes y después): SM64 (título, selección de archivo, intro,
  castillo; solo cambia el recuadro del medidor), los menús de Mario
  Kart 64 hasta la selección de pista y las intros de GoldenEye, Banjo,
  Star Fox 64, Perfect Dark, Conker y Zelda OoT.
- Pruebas: `tools/build.sh test-host` (320, 0 fallidas), `tools/test-roms.sh`
  (nemu64-test 433 con HLE y 136 con LLE, gfxtest ×3, audiotest, vitest,
  controles, savetest), `tools/build.sh` sin advertencias del compilador y
  `tools/test-phase2.sh` en RPCS3 (24 de 24 en F3DEX2 y F3D).
- Paquete para la PS3: `salsa64-alfa.pkg` (SLSA00085, «Salsa64 alfa»).
- **PS3 real**: el autor confirmó el medidor de SM64 sin fondo. Log de la
  corrida sin errores (`build/ps3-logs/sm64-medidor.log`, fuera de git).

- **Mario Kart 64 en la PS3** con este paquete: menús y carrera bien según
  el autor (la pantalla gigante negra de la pista es el efecto de
  framebuffer ya pendiente, Fase 4). Vio además polígonos con «una punta que
  se queda y se estira» en la transición del título a «GAME SELECT». No es
  de esta rama: ninguna draw de Mario Kart pasa por la rama nueva de
  `blend_state` (55 026 estados rastreados, campos 300–1900). En la Mac la
  transición sale limpia: el fundido son 240 triángulos con w de 1940 a
  2900 (ninguno con w ≤ 0) y «GAME SELECT» son solo rectángulos. Hipótesis:
  el RSX lee vértices viejos (búfer que la tarea siguiente ya reescribió) en
  src/rdp.

Pendiente:
- [ ] Polígonos estirados en la transición título → «GAME SELECT» de
      Mario Kart 64 en la PS3 (tarea aparte: separar HLE de RSX con el
      volcado `gfxdump.bin` y `--raster-dump`; revisar la vida de los
      juegos de búferes de src/rdp/rdp.c).
- [ ] `embedded` de vuelta a `file:oot.z64` (como lo dejó el trabajo de
      presentación) cuando el autor termine con Mario Kart.

## 2026-10-10 — Presentación: Banjo-Kazooie (FREEZE del RDP y destino equivocado), pausa de Zelda y recorte del overscan

Rama `bold-neumann-794fc0` (trabajo de presentación). Los tres casos
pendientes de `src/rdp` y uno más que apareció en el camino.

**Banjo-Kazooie no pasaba de 2 tareas gráficas** (en la Mac, en qemu
ppc64 con y sin recompilador, y en RPCS3: pantalla fija con el audio
sonando; en la PS3 real la intro corría porque el paquete naudio era
anterior). Bisección por commits del host: lo rompió la integración del
LLE (ce8351d), que dio semántica a DPC_STATUS (antes una lectura devolvía
la última palabra escrita). Rastreando los accesos al DP: el juego usa el
bit FREEZE como semáforo: en el manejador de la interrupción del DP escribe
SET_FREEZE, arranca la tarea del cuadro siguiente con el RDP congelado y
la descongela después; la interrupción de esa tarea la espera *después* de
descongelar (en la consola el RDP congelado no ejecuta). Nuestro HLE la
daba 10 000 ciclos tras el fin de la tarea, aún congelado, y el juego la
perdía. Arreglo en el núcleo (`src/core/rsp.c`, `sp.c`): si DPC_STATUS
tiene FREEZE cuando toca la interrupción del SYNC_FULL, se retiene
(`dp_held`) y se programa cuando el CPU escribe CLR_FREEZE
(`salsa64_rsp_dp_unfreeze`). Mismo principio que mupen64plus-core (que
difiere la tarea entera mientras FREEZE). Banjo corre: 644 tareas en 27 s
en la Mac. Prueba `rsp_gfx_task` intacta (la interrupción sigue llegando
`dp_cycles` después del fin de la tarea cuando no hay freeze).

**Parpadeo de Banjo en la PS3 (a):** el juego alterna dos framebuffers de
292 × 216 a 584 × 216 bytes uno del otro, y `src/rdp` supone 3/4 del ancho
de alto (219 líneas): el primero «cubre» el origen del VI del segundo con
sus últimas 2 líneas. Entre los dos, se tomaba el dibujado hace menos, que
mientras se muestra uno es el otro (el RDP ya dibuja el cuadro siguiente):
se presentaban 2 líneas negras estiradas a toda la pantalla. Simulado con
la traza de la Mac (VI y destinos por tarea, 70 s): 566 de 4142 campos con
el destino equivocado; con la regla nueva (entre destinos del ancho del VI,
el que empieza más cerca del origen; luego el más reciente), 12, todos de
arranque o cambio de escena (el otro framebuffer aún sin dibujar).

**Conker en la PS3 (3D quemado y parpadeando, capturas del RSX negras;
pedido tras integrar la rama de Rare):** mismo error
que Banjo. La repetición del volcado de la PS3 (campo 3600) en RPCS3 con
`tools/rpcs3-run.py --replay` dibuja la escena bien: la traducción al RSX
es correcta. En la Mac (70 s con `--trace-vi` y `--trace-gfx`) Conker
alterna dos framebuffers de 292 × 216 (0x3b7600 y 0x3d6300, a 216 líneas
uno del otro) y a mitad de cada tarea dibuja en la color image CI8 de 64 de
ancho (0xde080) y vuelve: con la regla vieja 591 de 4144 campos presentaban
las 2 últimas líneas del otro framebuffer (negras, o una franja de color
estirada a toda la pantalla: lo «quemado»); con la nueva, 0. Además, el
alto supuesto de un destino (3/4 del ancho: 48 para la de 64) ahora crece
hasta donde llegan las tijeras de los dibujos que recibe (`rdp.c` mira los
dibujos que siguen a cada TARGET; la CI8 de Conker es de 64 × 64 y su
FILL va de 0 a 65): la superficie se crea del alto real y, si una tarea
posterior llega más abajo, se vuelve a crear más grande. El alto de la
presentación (`h`) también lo usa. (rsxkit ya recortaba tijeras y
borrados a la superficie: no se escribía fuera.)

**Tabla de destinos portable:** `src/rdp/targets.c` (sin rsxkit): qué
color image tiene superficie, cuál se recicla y cuál se presenta; `rdp.c`
solo guarda las superficies por ranura. Pruebas en
`tests/unit/test_targets.c` (7): dos framebuffers alternos en direcciones
distintas que se solapan en la cuenta (Banjo, Conker), la misma dirección
con dos anchos (GoldenEye 320/440), color images de 64 sobre el
framebuffer (Zelda en pausa), VI sostenido sin dibujar (GoldenEye
cargando), dibujado con el VI apagado (SM64), el orden de reciclado y el
alto por las tijeras (Conker).

**Pausa de Zelda negra en la PS3:** con el menú, cada tarea pone 3 color
images y alternando framebuffers son 6 distintas (0x3b8800/64,
0x3b5000/64, 0x3b5000/320, 0x3de000/64, 0x3da800/64, 0x3da800/320) para 4
ranuras: cada tarea tiraba 2 y entre ellas el framebuffer de 320. Ahora
8 ranuras (`RDP_TARGETS_MAX`; memoria del RSX en docs/memory-budget.md) y,
al reciclar, nunca la presentada en el último campo, primero las viejas
(30 campos), luego las de un ancho distinto al del VI (auxiliares) y al
final la más antigua. Reproducido en la Mac con `docs/pads/zelda-oot.pad`
(la partida del autor bajada de la PS3 por FTP a `build/saves/`, fuera de
git): los TARGET de la traza son esos; el rasterizador de la Mac no cambia.

**Recorte del overscan:** `SALSA64_DATA_DIR/overscan.cfg`, en píxeles de la
N64 a 320 × 240 por lado (un número: todos; dos: horizontal y vertical;
cuatro: izquierda, derecha, arriba, abajo; `0` lo apaga). **Encendido por
omisión con 8 y 6 (2,5 % por lado)**: es lo que una tele de tubo esconde y
donde los juegos dejan basura (las columnas azules de GoldenEye; los HUD
quedan dentro). Se escala con la imagen del VI (a 640 de ancho recorta
16). Vale para el destino del RSX y para la copia de la RDRAM; las ROMs de
prueba se presentan enteras (sus verificaciones leen la pantalla por
posición) y las capturas salen del tamaño recortado. En rsxkit,
`rsxkit_target_present` acepta ahora cualquier columna (la textura toma la
fila entera y la columna va en las coordenadas de textura; antes el offset
se redondeaba a 32 píxeles).

> **Corrección (2026-10-12):** el z/w > 1 y el domo fuera del plano lejano
> venían de la proyección de 750 y 1250 que pone la protección anticopia
> cuando falta lo que deja el IPL3 del 6103 (entrada del 2026-10-12). Además,
> rsxkit no descarta por z/w > 1: el RSX tiene zclamp y sin descarte de los
> planos cercano y lejano (`rsxSetZMinMaxControl`).

**Fondo negro de Banjo (c) — no es presentación:** en la Mac también sale
negro (cielo y paisaje) tras Banjo tocando. En la tarea del campo 2400 el
primer dibujo tras el borrado son 8 triángulos con textura, w de −88 a
1321 y z/w hasta 117 (el domo del cielo): el rasterizador de la Mac los
descarta por w ≤ 0 y en el RSX quedan fuera por z/w > 1 (el z normalizado
se pasa de 1 también en objetos normales: 1,05–1,36). Le toca a la HLE
(trabajo de F3D): la proyección o el recorte de esos triángulos. El síntoma
(b) (escena «desplazada», mitad izquierda vacía, pasto solo abajo) coincide
con la escena sin fondo: Banjo a la derecha sobre una franja de pasto y
todo lo demás negro. Volver a mirarlo en la PS3 con el fondo arreglado
antes de buscar otra cosa.

Herramientas (`salsa64-run`): `--pad "0:…"` conecta el control desde el
arranque (antes el primer estado entraba al terminar el campo 1; en la PS3
el puerto 1 siempre está); `--trace-exc` imprime los bits IP de Cause y
MI_INTR/MASK; variables `SALSA64_GFX_CYCLES`, `SALSA64_DP_CYCLES` y
`SALSA64_AUDIO_CYCLES` para probar otros tiempos del RSP; ganchos de
depuración `-DSALSA64_DEBUG_PI` (DMA del PI) y `-DSALSA64_DEBUG_DP`
(registros del DP, escrituras de SP_STATUS e interrupción del DP), como
`SALSA64_DEBUG_SPMEM`.

Verificación (con main 705f4ac fusionado): `tools/build.sh test-host` 326
(7 de targets); `tools/test-roms.sh` (nemu64-test 433 con HLE y 136 con
LLE, vitest, controles, savetest ×6, gfxtest ×3, audiotest); test-phase2 en
RPCS3 (gfxtest 22/22, spu-jobs, frontend F3DEX2 y F3D); `tools/test-rec.sh`
(unitarias big-endian con el recompilador ×4, nemu64-test 433, vitest);
`tools/build.sh` sin advertencias del compilador. Paquete subido a
`/dev_hdd0/packages/salsa64-presentacion.pkg`; los respaldos de Banjo y
Conker ya están en `/dev_hdd0/salsa64/`.

**En la PS3 real (el autor, paquete «Salsa64 presentación» SLSA00084 con
main 705f4ac):** Banjo ya sin parpadeo (destino de 1,5 campos de
antigüedad, cola 0); Conker «se ve muy bien, sin temblores» (57 campos/s con
escenas de 7600 triángulos); GoldenEye sin columnas azules; Zelda 100 % de
NTSC. Lo que queda no es de presentación: en Banjo los modelos tiemblan,
hay cosas dibujadas encima de otras y faltan polígonos (el rango de z, ver
arriba); en Conker polígonos negros en la boca; y en GoldenEye soldados
(y el plano frontal de Bond al entrar a Dam) teñidos de cian. Este último
se investigó a fondo porque parecía regresión de la rama: el volcado
(`gfxdump` en el campo 2400) de un cuadro con el soldado cian, dibujado en
la Mac, sale cian igual; sus dibujos (#22 y #25) llevan
`lerp(ENV, TEXEL0, SHADE_ALPHA)·SHADE` con ENV = (0,35, 0, 0), que la HLE
de 705f4ac aproxima como `T0·Ca + (0,353·(C.r − C.a), 0, 0)`: con C.a ≈ 1 y
C.r gris resta rojo. Repetido el mismo volcado en la PS3 con las dos apps
(SLSA00064 de main 705f4ac y la de esta rama), las imágenes son idénticas
píxel a píxel (0 de 75 840). El generador de fragment programs de main
(b32a6e6, fusionado aquí) hace ese combiner exacto. También se comprobó
que GoldenEye no escribe FREEZE en DPC_STATUS (solo 0x3c0, también en
Dam): el cambio del núcleo no lo toca. RPCS3 no sirve para GoldenEye: su
RSX muere («Verification failed») a los ~16 s con cualquier frontend.

**Retraso de lo que se ve (duda del autor sobre el input lag de Zelda):**
el log trae cada 5 s «retraso: cola X campos (máx N); destino Y campos de
antigüedad (máx M)»: la cola es cuántos campos lleva la emulación de
ventaja al presentar (lo nuestro: 0,00 en todos los juegos probados) y la
antigüedad cuántos campos hay entre el dibujo del destino y el campo que
lo muestra (lo del juego: 1,0 en GoldenEye, 1,5 en Banjo, 2,5 en Conker).
Zelda queda por medir con el menú de pausa y un minuto de juego.

**Con el paquete que lleva main b32a6e6 (generador de fragment
programs), el autor en la PS3:** GoldenEye «todo se ve normal»: soldados,
armas en primera persona y el plano de Bond sin tinte (80,9 dibujos por
tarea con programa generado). Zelda: el menú de pausa se ve (el mapa; los
errores que quedan en él son los efectos de framebuffer ya conocidos,
Fase 4), 100 % de NTSC, cola 0,00 y destino de 2,50 campos de antigüedad
(el juego va a 20 cps: cada cuadro se muestra 3 campos; GoldenEye 1,0 y
Banjo 1,5): la sensación de retraso de Zelda es del propio juego, no de
la cola de presentación. SM64 y Mario Kart, sin novedades. `embedded`
quedó en `file:oot.z64`.

Pendiente:
- [ ] Integración en main (la rama lleva main
      b32a6e6 fusionado y la verificación completa en verde).
- [x] HLE: el cielo y el paisaje de Banjo (arriba). No era de F3D: la
      protección anticopia del 6103 (entrada del 2026-10-12).
- [x] LLE: con `--lle=all` Banjo no arranca (la primera tarea corre 958 M
      instrucciones sin tocar COP0); no es del freeze. Era la misma
      protección: sin lo que deja el IPL3 el juego no trae el arranque del
      RSP (entrada del 2026-10-12).
- [ ] El VI de Banjo muestra 213 de 240 líneas (V_VIDEO 61–488): en una
      tele quedarían bandas; aquí se estira a 4:3. Colocar la imagen según
      la ventana del VI cuando haya un caso que lo necesite.

## 2026-10-10 — Kokiri «en colores planos»: es el sol cenital, no la HLE (referencia con mupen64plus)

> **Corrección (2026-10-11):** esto vale para el bosque al aire libre (el
> balcón, campo ~3700). Lo que el autor vio en la PS3 y el verde puro del
> host (casa de Link desde arriba, tienda Kokiri) son **salas
> prerrenderizadas**: faltaban la tarea JPEG y el fondo de S2DEX. Ver la
> entrada «Kokiri y su tienda en colores planos» del 2026-10-11.

Reporte (PS3 y rasterizador de la Mac): en el bosque
Kokiri de Zelda OoT el mundo sale en colores planos, «verde del shade», con
la HUD bien; la casa de Link sí se ve con texturas.

Diagnóstico (rama `frosty-golick-d1d71a`, en la Mac):
- Las texturas **sí** llegan y se decodifican bien: corteza CI8 con paleta,
  pasto RGBA16 (dos tiles, lerp por ENV_ALPHA), siluetas de troncos I4;
  los combiners son exactos (T0·SHADE·PRIM y lerp(T0,T1)·SHADE), `ntex` =
  1 o 2, las s/t por triángulo cubren ~20 texels. En el píxel (270,80) del
  tronco el rasterizador escribe T0 = 0,54 × X0 = 0,23 → 0,13.
- Lo que aplasta la textura es el **SHADE**: con `G_LIGHTING` los vértices
  verticales quedan en 80–120/255. Luces de la escena: sol blanco (255)
  con dirección (0, 0,99, 0,16), relleno (70,70,90) opuesto y ambiente
  80. El sol es cenital porque el juego fija `dayTime` = 0x8000 (mediodía
  exacto) al cargar la partida (leído de `gSaveContext` en el volcado de
  la RDRAM; OoT 1.2, 0x8011AC80). Las normales son normales de verdad
  (longitud ~120), no colores horneados.
- **Referencia**: mupen64plus 2.6 (brew) con GLide64 y rsp-hle, con la
  misma partida del autor, movido por `tools/m64p-ref.sh` (plugin de
  entrada propio que reproduce el `--pad`; `docs/pads/zelda-kokiri.pad`
  saca a Link al balcón). Mismos parches en 640×480: piso (56,56,3) vs
  Salsa64 (55,54,1); tronco (31,29,17) vs (29,27,15); pared (43,27,18) vs
  (37,23,14). Diferencias < 6/255 en todo: la escena **es así** a esa hora
  (el piso recibe el sol de lleno, pero sus texturas ya tienen media
  55/255). Imagen comparativa en `build/m64p-ref/kokiri-comparacion.png`
  (fuera de git).
- Única diferencia visible: zonas crema (arriba a la derecha y bajo el
  balcón) donde el rasterizador de la Mac descarta triángulos con algún
  w ≤ 0 (la pared lejana y el piso cruzan el plano de la cámara); GLide64
  los recorta. El RSX recorta en hardware, así que en la PS3 no deberían
  faltar; comprobarlo cuando la consola esté libre.
- **Confirmación con el microcódigo real** (GLide64
  también es HLE): con main ce8351d fusionado, `salsa64-run
  --lle=all` ejecuta el F3DZEX2 real (739 M instrucciones hasta el campo
  3712) y sus triángulos crudos, con los colores de vértice que calcula el
  RSP de verdad, dan los mismos parches en el campo 3700: piso (55,55,2),
  tronco (29,27,15), pared (37,23,14), túnica (53,65,28), frente a la HLE
  (55,54,1), (29,27,15), (37,23,14), (52,65,28). Las luces, normales y
  ambiente de la HLE coinciden con el microcódigo. Y en la imagen del LLE
  la pared lejana sí aparece (el microcódigo recorta contra el frustum
  antes de emitir los triángulos): las zonas crema de la HLE son solo el
  w ≤ 0 del rasterizador de la Mac.
- No se cambió nada de la HLE. Si en la PS3 la escena se ve distinta a la
  Mac (verde liso sin la corteza tenue), el problema sería de src/rdp o
  del RSX, no de la HLE: pendiente de confirmar con el autor.

Herramientas que quedan (sin cambios funcionales):
- Depuración de la HLE en el host con `EXTRA_CFLAGS=-DSALSA64_GFX_TRACE`
  (cabecera de `src/host/run.c`): estado del RDP por dibujo (combiner,
  othermode, tiles, luces, términos k), rango de s/t, vértices de un dibujo
  (`SALSA64_GFX_VDUMP`), texturas en PPM (`SALSA64_GFX_TEXDUMP`), dibujos
  pintados de magenta (`SALSA64_GFX_MARK`) y traza de un píxel
  (`SALSA64_GFX_PIX`) en el rasterizador. En `rdp.c` es solo un gancho bajo
  `#ifdef`; nada entra en el SPE.
- `tools/m64p-ref.sh` + `tools/m64p-pad/pad_plugin.c`: captura de
  referencia con mupen64plus (requiere `brew install mupen64plus`; el
  teclado de su plugin de entrada no funciona con sdl2-compat; el `.sra` de
  mupen64plus lleva los bytes de cada palabra de 32 bits invertidos; sus
  cuadros de `--testshots` no son campos del VI).
- El pad de Kokiri sirve de punto de partida para el menú de pausa y el
  resto de pendientes de Zelda.

Pendiente:
- [ ] PS3: confirmar que Kokiri se ve como en la Mac/mupen64plus (oscuro,
      textura tenue) y que no faltan la pared lejana ni el piso con w ≤ 0.
- [ ] Rasterizador de la Mac: recorte contra w = 0 (hoy descarta el
      triángulo entero), para que las capturas de referencia no tengan
      huecos crema.

## 2026-10-10 — Microcódigos gráficos de Rare: Perfect Dark y Conker

Hecho (rama `competent-haslett-3bad2a`, con main 3270538 y la rama
de arranque `amazing-noether-39a548` con `osCicId`, sin integrar):
- **Perfect Dark** (`src/rsp/gfx/f3dpd.c`): Fast3D de Rare. Solo cambian
  G_VTX (vértices de 12 bytes: x, y, z, banderas, offset del color, s, t) y
  G_VTXCOLORBASE (0x07, la tabla de colores; 4 bytes R, G, B, A o normal y
  A con G_LIGHTING); G_TRI4 en 0xB1 ya lo tenía f3d.c (como GoldenEye) y
  G_QUAD en 0xB5 con índices ×10. Cada vértice se arma en el formato de 16
  bytes y sigue el camino de siempre. El microcódigo no trae texto: se
  reconoce por el CRC-32 de su código (c543d0a8, 0x1000 bytes) solo si
  ningún texto lo identificó (`src/core/rsp.c`); `salsa64_crc32` pasó a una
  tabla de nibbles (dos pasos por byte en vez de ocho).
- **Conker** (`src/rsp/gfx/f3dexbg.c`, «F3DEXBG.NoN fifo 2.08», por su
  texto): F3DEX2 con G_TRI4 en 0x10–0x1F (índices de 5 bits; el 0x14 era
  eso), luces de 48 bytes con luz puntual (posición e intensidad),
  G_MW_COORD_MOD, tabla de normales aparte (G_MV_NORMALS) y la luz que
  multiplica el color del vértice. **Por qué no mandaba otra tarea:** su
  G_LOAD_UCODE no cambia de GBI (pasa a la iluminación avanzada y la lista
  sigue); la HLE de F3DEX2 terminaba la tarea ahí, sin llegar al
  G_RDPFULLSYNC, y el juego esperaba la interrupción del DP para siempre.
  Con main 53fb223 F3DEX2 salta el tramo de un microcódigo ajeno
  (`gfx_dl_skip_foreign`, el S2DEX de Zelda); en Conker eso perdería todo lo
  que dibuja después, así que su hook atiende el 0xDD antes que F3DEX2.
- Cambios en lo común, mínimos: las familias aceptan un *hook* con los
  comandos propios de una variante (`gfx_dl_f3d(g, hook)`,
  `gfx_dl_f3dex2(g, hook)`); geom.c separa la transformación de la lectura
  (`gfx_vertices`) y expone las luces en espacio del modelo y el texgen;
  `gfx_state.rare` (~250 bytes) con el estado de las dos variantes; dos GBI
  nuevos en rcp.h; F3DEX2, F3D y F3DEX no cambian (mismas pruebas y
  mismas imágenes de gfxtest).
- SPE 0: Perfect Dark 1,9 KB y Conker 4,4 KB de código; texto 73 KB (con main
  755cbaa), bss 145 KB, pila ~37 KB (antes ~46; `src/spu/gfx/main.c`). `GFX_MAX_LIGHTS`
  pasó de 8 a 11: Conker pide hasta 11 luces (`G_MW_NUMLIGHT` = 0x210).
- Pruebas: suite `gfx-rare` (9: colores por tabla, luz con la normal en la
  tabla, G_TRI4 y G_QUAD de PD; G_TRI4 de 5 bits, G_LOAD_UCODE que no corta
  la lista, luz básica y avanzada, 11 luces, COORD_MOD, lookat, normales y
  texgen de Conker) y en `rsp` la detección por texto y por CRC (4 KB
  sintéticos con 4 bytes calculados para dar el CRC de PD) y el despacho al
  backend: 297 en el host. Mutaciones: 6 de 6 detectadas.
- Host (respaldos propios, fuera del repo), 3 minutos sin tocar el
  control: **Perfect Dark** 6914 tareas, 0 comandos sin implementar, 0
  errores (identificación del producto en alta resolución, logo de
  Nintendo, cubo de N64 que se vuelve el logo de Rare, la ciudad, Joanna en
  la azotea); **Conker** 4468 tareas, 0 sin implementar, 0 aproximados
  (pantalla legal, logos, Conker con la motosierra, la taberna, menú
  «GAME 1»). Detalle en docs/compatibilidad.md.
- Con main 755cbaa fusionado: build sin advertencias, test-roms (nemu64-test
  446, vitest, controles, savetest, gfxtest ×3 con 22/22, audiotest),
  test-phase1 y test-phase2 en RPCS3 (297/297 big-endian antes de la última
  fusión), test-rec en qemu (nemu64-test con la línea base y vitest con el
  recompilador; las unitarias pasan hasta `gfx-rare` en los cuatro modos y
  se caen en `sync`, como en main). Sin regresión: SM64, Mario Kart 64,
  GoldenEye, Star Fox 64 y Banjo-Kazooie dan, con main y con esta rama, el
  mismo resumen gráfico y la misma imagen a los 60 s.
- Depuración en el host: `EXTRA_CFLAGS=-DSALSA64_DEBUG_DL` vuelca las
  display lists (`SALSA64_DEBUG_DL=N`, `SALSA64_DEBUG_DL_FROM=k`) y
  `-DRASTER_W=640 -DRASTER_H=480` agranda el rasterizador para pantallas de
  alta resolución.

Pendiente:
- [x] **PS3 real** (paquete «Salsa64 Rare», SLSA00081, be8f573 con main
      ce8351d): **Perfect Dark** se ve y suena bien (el autor: «se ve y suena
      espectacular»); 59,9 campos/s (100 % de NTSC) salvo la intro en alta
      resolución (84–94 %), HLE en el SPE 1,4–8 ms por tarea, 0 tareas
      cortadas, naudio con MP3 sin errores. **Mario Kart 64** (no
      regresión): 65 s al 100 % de NTSC, 0 tareas cortadas. `embedded`
      quedó como estaba (`file:oot.z64`).
- [x] **Conker en la PS3** (resuelto en la entrada «Presentación:
      Banjo-Kazooie…» de arriba, falta confirmar en la PS3): arranca y
      suena, pero el 3D sale quemado y parpadea y no se ve el logo de N64; las capturas del RSX de las
      escenas 3D (campos 3000, 3600, 4200) salen negras y el logo 2D de
      Nintendo sí. El volcado de la HLE hecho en el SPE (`gfxdump` del campo
      3600) dibujado en la Mac con `--raster-dump` sale bien e igual al
      host: la HLE es correcta y la falla es de la traducción o la
      presentación del RSX (`src/rdp/rdp.c`, lib/rsxkit). Lo propio de
      Conker: a mitad de cada cuadro dibuja en un destino CI de 8 bits y 64
      de ancho (0xDE080) y vuelve al principal (0x3D6300, 292×213 RGBA16, z
      en 0xE9D80). Pasado al trabajo de presentación (mismo tipo que la pausa
      de Zelda).
- [ ] Perfect Dark aproxima `lerp(ENV, TEXEL0, SHADE_ALPHA)·SHADE` (el de
      GoldenEye): término S·Sa en el RSX (Fase 4).
- [ ] Efectos de framebuffer de los dos (cámara espía y monitores de PD,
      copias del z-buffer, desenfoque de la pausa y sombra de Conker): Fase
      4; lista en docs/compatibilidad.md.
- [x] Sonido: naudio de los dos (trabajo de audio, en main desde ce8351d).
- [ ] La luz de Conker sigue a GLideN64 (en el modo básico la direccional
      no cuenta); contrastar con la PS3 cuando haya escenas de juego.

## Pendiente: efectos de framebuffer (anotado el 2026-10-10)

Va después de la presentación (se monta sobre su tabla de destinos).
Detectar cuándo el juego usa como textura o lee con el CPU una imagen dibujada en el RSX; copiar
RSX → RDRAM (bajando de 3× a la resolución de la N64) solo en esos casos, y para el render a
textura usar directo el destino del RSX. Casos:
- [ ] Zelda OoT: fondo difuminado de la pausa (lo difumina el CPU) y Link en la pantalla de equipo
      (render a textura); la pausa ya se ve con la presentación, pero con esos dos defectos
      (confirmado por el autor en la PS3).
- [ ] Mario Kart 64: pantallas gigantes que transmiten la carrera.
- [ ] Perfect Dark: cámara espía y otros; Majora's Mask (Canción del Vuelo, transformaciones).

## Pendiente: presentación (anotado el 2026-10-10)

Resuelto en la entrada «Presentación: Banjo-Kazooie…» de arriba (falta
confirmar en la PS3). Los tres casos estaban en `src/rdp/rdp.c` (qué
destino del RSX se presenta en cada campo):
- [x] **Banjo-Kazooie parpadea** muy fuerte (visto por el autor en la PS3;
      video de 1,6 s en la intro, Banjo tocando el banjo). Tres síntomas: (a)
      ~1 de cada 3 cuadros del video (30 cps) sale casi negro con solo la parte
      de arriba de la cabeza de Banjo: imagen vieja o a medio dibujar
      presentada entre las buenas; (b) toda la escena sale desplazada a la
      derecha y abajo (mitad izquierda vacía, Banjo cortado en el borde
      derecho, el pasto solo en un rectángulo abajo): revisar origen/escala del
      VI, viewport y scissor; (c) falta el fondo (cielo y paisaje negros).
      Revisar cómo alterna sus framebuffers y si usa efectos que leen el
      framebuffer.
- [x] **Menú de pausa de Zelda negro en la PS3:** el juego pone color images
      de 64 de ancho en la dirección del framebuffer y se pierde el destino de
      320 («0 del RSX»); en el rasterizador de la Mac se ve bien.
- [x] **Recorte del overscan:** GoldenEye deja franjas azules a los costados
      (su scissor 3D cubre las columnas 1–318 sobre un relleno azul).

## 2026-10-10 — Fase 6: prototipo de LLE del RSP (microcódigo real) en host y SPE

Hecho (rama `nervous-lewin-ae6137`, desde main 3270538; sin
integrar; diseño y mediciones en docs/rsp-lle.md):
- **Intérprete portable del RSP** (`src/rsp/lle/`: escalar, COP0, unidad
  vectorial completa con el acumulador de 48 bits y VCO/VCC/VCE, recíprocos
  por tabla, todas las cargas/almacenamientos vectoriales). Semántica de
  los casos raros de ares (ISC, registrado en THIRD_PARTY.md). Mismo código
  en host, PPE y SPE, sin `#ifdef` de orden de bytes.
- **nemu64-test con `--lle`: fallan 136 de 4637 (97 %) frente a 433 (90 %)
  sin LLE. Las 214 pruebas «RSP …» pasan todas**; quedan 4 «RDP STATUS»
  (el RDP no escribe el framebuffer en la RDRAM). Sin regresiones; 5
  mejoras en el camino HLE (pares de bits de SP_STATUS, DPC_START/END).
  `tools/test-roms.sh` corre nemu64 dos veces (líneas base
  `nemu64-test.fallas`, 289 nombres, y `nemu64-test-lle.fallas`, 71).
- **Enganche** (`rsp.c`: backend `salsa64_lle_backend`, modos
  `off/auto/all`; `sp.c`: sincronía, SP_STATUS atómico, DPC con semántica
  real) y **RDP a bajo nivel** (`src/rsp/gfx/dpc.c`: los comandos crudos
  de DPC_START..END, también por xbus, van a `gfx_rdp_command`/
  `gfx_rdp_triangle`, el camino de triángulos crudos existente).
- **Host**: `salsa64-run --lle[=auto|all]`, `--lle-speed`,
  `--lle-check-audio` (cada tarea de audio también en la HLE sobre una
  copia de la RDRAM, comparando lo escrito). SM64: el Fast3D real corre y
  el título con la cara de Mario sale igual que en HLE. Zelda: el nead
  real suena (211 M instrucciones en 16 s); las primeras 30 tareas de
  audio idénticas a la HLE byte a byte, y desde el primer sonido difiere
  solo FILTER, donde mupen64plus-rsp-hle promedia las tablas (`>> 1`) y el
  microcódigo real no → **la HLE de FILTER probablemente va a la mitad**;
  confirmar de oído.
- **SPE** (`src/spu/rsp/`, `src/platform/ps3/rsp_spu.c`): trabajo de
  spukit con DMEM/IMEM en el local store, SP_STATUS y semáforo por DMA
  atómico (getllar/putllc) contra los `__atomic_*` del PPE, DMA del SP
  entre la RDRAM y el local store, registros del DP en el SPE con captura
  de los comandos xbus para que el PPE los reproduzca (`salsa64_dp_submit`),
  continuaciones cada 200 000 instrucciones. Frontend: archivo
  `SALSA64_DATA_DIR/lle` con `auto` o `all`; líneas «rsp lle:» en el log.
- 12 pruebas unitarias nuevas (301 en total con main).
- Medido en el host: 12,5 ns por instrucción del RSP (docs/perf-log.md).
- **PS3 real**: sanidad con HLE por omisión bien (Zelda
  106 s al 100 % con audio; GoldenEye intro al 100 %, nivel a 36–50
  campos/s con el audio estirado: primera medición del nivel, no es de
  esta rama). Con `lle` = `all`, **Zelda se ve y suena con el microcódigo
  real en el SPE**: 226–283 ns por instrucción (3,5–4,4 MIPS; el RSP real
  ~62), 24–55 campos/s. Tres arreglos sobre la marcha (SP_PC pisado al
  recoger el trabajo, estado del DP entre trabajos, DMA de muchas filas de
  S2DEX en una transferencia) e instrumentación (contadores de COP0,
  latido del SPE). Queda una divergencia: tras los fondos S2DEX de la
  intro (~36 s) el microcódigo entra en un bucle sin fin que el host no
  reproduce (detalle y pista en docs/rsp-lle.md).

Pendiente:
- [ ] Divergencia del SPE tras S2DEX: modelo del protocolo de trabajos del
      SPE en el host (`lle_spu_run` con memcpy y cortes por `max_instr`)
      para reproducirla con trazas; sin turnos de consola.
- [ ] Confirmar de oído FILTER (HLE a la mitad vs microcódigo real).
- [ ] Comparar la imagen de Zelda en LLE con la HLE (host, `--gfx-frame`).
- [ ] Recompilador RSP → SPU (plan en docs/rsp-lle.md): es lo único que
      lleva de 0,06× a velocidad real; mover el consumidor del RDP al SPE.
- [ ] Homebrew de libdragon (rspq/rdpq) como prueba sin HLE.
- [ ] Limitaciones del SPE: escrituras del CPU en DMEM/IMEM durante una
      tarea, limpiar la interrupción del SP desde el microcódigo.

## 2026-10-10 — Zelda OoT se colgaba al elegir la ranura: G_LOAD_UCODE (S2DEX) y el RDP que nunca terminaba

Síntoma (PS3 real, main f698afe): al elegir una partida, pantalla negra con
una rayita amarilla arriba a la izquierda, el audio seguía (en silencio) y
no llegaba ninguna tarea gráfica más; 96 % en bucles de espera.

Diagnóstico (rama `fervent-antonelli-6904bf`, en la Mac con el
intérprete, sin PS3):
- Se reproduce igual con el intérprete y la HLE síncrona: no es del
  recompilador, ni del sondeo del SPE, ni del DMA del PI.
- Al cargar la primera sala (la casa de Link, con fondo prerrenderizado) el
  juego manda 75 tareas tipo 4 (su decodificador JPEG; sin HLE, terminan sin
  hacer nada) y una tarea gráfica cuya lista carga **S2DEX** con
  `G_LOAD_UCODE` para copiar el fondo (`uObjBg`) y después vuelve a F3DZEX.
  La HLE terminaba la tarea en el `G_LOAD_UCODE`, sin llegar al
  `G_RDPFULLSYNC`: TASKDONE sí, interrupción del DP no. El planificador de
  libultra nunca dio la tarea por terminada, el hilo gráfico de OoT esperó
  su mensaje y a los 3 s su vigilante declaró «RCP is HUNG UP»: el hilo de
  fallas dibuja el rectangulito amarillo (el de `Fault_DrawCornerRec`) y el
  juego queda muerto con el audio vivo. Visto con el volcado de la RDRAM:
  todos los hilos esperando en colas vacías.
- Nadie había pasado de la selección de archivo: no era regresión.

Arreglo:
- `src/rsp/gfx/f3dex2.c`: un `G_LOAD_UCODE` a otra dirección que la del
  microcódigo de la tarea (`job->ucode`, nuevo en `gfx_job`) ya no termina
  la lista: se saltan sus comandos (siguiendo `G_DL` y `G_ENDDL`, que la
  familia comparte) hasta el `G_LOAD_UCODE` que vuelve al de la tarea, y la
  lista sigue (Link, objetos, HUD y el FULLSYNC se ejecutan; el fondo
  prerrenderizado queda negro). `res->skipped` cuenta los saltados; la
  bandera `GFX_RES_LOAD_UCODE` marca que hubo un tramo ajeno.
- `src/core/rsp.c`: si la HLE corta una lista antes de su fin
  (`SALSA64_GFX_CUT`: búfer lleno, lista interminable, error de dirección;
  lo traduce `salsa64_gfx_task_flags` en sync.c y gfx_spu.c), la
  interrupción del DP llega de todos modos: el juego espera al RDP por cada
  tarea gráfica. Estadística `cut` (frontend: «tareas cortadas» y «comandos
  de otro microcódigo saltados» en la línea `gfx:`).
- Pruebas: `rsp_gfx_task_cut` (DP con lista cortada; sin DP si acabó en
  ENDDL sin FULLSYNC) y `gfx_load_ucode_skips_foreign` (mismo microcódigo
  sigue; tramo ajeno con subrutina saltado, 2 triángulos, FULLSYNC; tramo
  que nunca vuelve = cortado; FULLSYNC dentro del tramo cuenta). La ROM
  gfxtest lleva al final de cada lista un tramo de otro microcódigo con un
  FILLRECT rojo que taparía la escena si se ejecutara, y en el cuadro
  GFXTEST_CUT_FRAME (7) termina dentro de él: la ROM exige la interrupción
  del DP y test-roms.sh exige «1 tareas cortadas» y tramos saltados. 272
  unitarias en el host (clang nativo y Docker con ASan); test-roms
  (nemu64-test 446, vitest, controles, savetest, gfxtest 22/22, audiotest)
  OK; build sin advertencias.
- Mac: con el arreglo el juego sigue (tareas gráficas a 20/s, la escena del
  Árbol Deku con su texto); ~7 comandos de S2DEX saltados por cuadro.

Herramientas nuevas para esto (`salsa64-run`): `--pad
"CAMPO:BOTONES[@X,Y];…"` (control 1 programado por campo del VI, cada estado
hasta el siguiente; unificado con el `--pad` que traía main: misma sintaxis,
campo por campo y también en la referencia de `--lockstep`), `--trace-from CAMPO`,
`--trace-faults N` (excepciones sin las interrupciones), `--ram-dump
ARCHIVO`, y `tools/libultra-threads.py` (hilos de libultra en un volcado:
estado, pc y la cola en la que espera cada uno, sin símbolos). Con `--pad
"2200:START,2320:A,2440:A,2560:A"` y el `.sra` del autor en `--save-dir`,
Zelda entra al juego en el campo 2712.

- **PS3 real** (paquete «Salsa64 depuración», SLSA00079, ba9bdfc): Zelda
  entra al juego y el autor jugó 7 min (intro, casa de Link, Navi, bosque
  Kokiri) a 59,8 campos/s, 0 tareas cortadas, ~7 comandos de S2DEX
  saltados por cuadro en la casa; audio −4 a −11 dBFS en juego. El silencio
  de ~30 s entre elegir la partida y el vuelo de Navi es del juego: la
  Mac con sus mismas pulsaciones calla y suena en los mismos campos, y
  en ese tramo las listas de audio no traen voces (solo CLEARBUFF,
  LOADBUFF, MIXER, SAVEBUFF, DMEMMOVE, INTERLEAVE).

Con main 485a747 traído (F3D, arranque del CIC 6105 con osCicId): Zelda
sigue pasando la ranura en la Mac (el arranque nuevo no cambia nada aquí);
el salto de tramos ajenos vive ahora en `gfx_dl_skip_foreign` (dl.c) y lo
usan F3DEX2/F3DZEX y F3DEX 1.x (F3D no tiene G_LOAD_UCODE: la variante F3D
de gfxtest no lleva el tramo). Con `job->ucode` = 0 (tarea sin microcódigo
conocido, como en las pruebas de F3D) todo G_LOAD_UCODE es ajeno.

Visto en esa corrida, para otros trabajos (reproducible en la Mac con
`--pad "$(cat docs/pads/zelda-oot.pad)"`, las pulsaciones del autor
sacadas de su log; receta en la entrada de arriba):
- [x] **Texturas del mundo**: en el bosque Kokiri la geometría salía en
      colores planos (verde del shade), igual en el rasterizador de la Mac.
      No era error: es el sol cenital de mediodía (ver la entrada «Kokiri
      “en colores planos”», con la referencia de mupen64plus). Queda
      confirmar en la PS3 que se ve como en la Mac. *(2026-10-11: en las
      salas prerrenderizadas sí era error; resuelto con la tarea JPEG y
      S2DEX, entrada del 2026-10-11.)*
- [x] **Menú de pausa en negro** (frontend/rdp; resuelto por el trabajo de
      presentación, `src/rdp/targets.c`): con el menú abierto cada
      tarea (30/s) pone tres color images: dos de **64 de ancho** (0x3b8800
      y **0x3b5000, la misma dirección del framebuffer**) y luego
      0x3b5000/320; el VI sigue mostrando 0x3b5280/0x3daa80. En la PS3 el
      log dice «0 del RSX» y ~2 destinos creados por tarea: el destino de
      320 se pierde al crear uno de 64 en la misma dirección y la
      presentación cae a la copia de la RDRAM (negra, el RSX no la
      escribe). Le toca a src/rdp (destinos por dirección y ancho, o no
      tirar el de 320). Cerrar el menú vuelve al juego. Reproducible en la
      Mac: `--pad "$(cat docs/pads/zelda-oot.pad)" --trace-from 19100
      --trace-gfx 80` muestra los TARGET, y con `--gfx-frame-every 6` el
      rasterizador de referencia dibuja el menú (mapa del bosque Kokiri)
      bien: la HLE está bien, es solo la presentación.

Falta (para ver bien la casa de Link y las salas con fondo prerrenderizado):
- [x] HLE de S2DEX para el fondo (`G_BG_COPY`/`G_BG_1CYC` con `uObjBg` →
      rectángulos con textura por tiras) y detectar el microcódigo que
      carga `G_LOAD_UCODE` por el texto de sus datos (como en rsp.c).
      *Hecho el 2026-10-11 (`src/rsp/gfx/s2dex.c`).*
- [x] *(2026-10-11, `src/rsp/jpeg/`)* Tarea tipo 4 de OoT (decodificador JPEG `njpgdspMain`: IDCT y
      YUV→RGBA16 de los macrobloques que el CPU ya decodificó): sin ella el
      fondo sale negro aunque S2DEX lo copie. mupen64plus-rsp-hle la tiene
      (`jpeg.c`, GPLv2+; registrar en THIRD_PARTY si se adapta). Son 75
      tareas por sala, una vez (no por cuadro).

## 2026-10-10 — naudio: Banjo-Kazooie, Conker, Perfect Dark (con MP3) y Donkey Kong 64

Hecho (rama `charming-allen-a8e4b3`, con main 485a747):
- **naudio** en la HLE portable (`src/rsp/audio/abi.c`): las tablas de
  naudio, Banjo-Kazooie (la misma), Donkey Kong 64 (7 y 8 son MIXER) y los
  de MP3 (Perfect Dark, Banjo-Tooie, Jet Force Gemini y Conker: 0
  OVERLOAD, 7 MP3, 8 MP3ADDY, 0x0E filtro). Búferes fijos de 0x170 bytes
  desde 0x4F0; ADPCM y RESAMPLE con el estado en w1; SETVOL con sus tres
  formas y el 0x0E que salta a la mitad de SETVOL. IIR y OVERLOAD en
  `alist.c`. Codificación verificada contra `alist_naudio.c` de
  mupen64plus-rsp-hle (8a7a472a). La detección ya existía
  (`src/core/rsp_audio.c`, palabra [0x10] de `ucode_data`).
- **MP3** (`src/rsp/audio/mp3.c`): la síntesis de MPEG-1 Layer III que el
  microcódigo hace en el RSP (DCT de 32 y ventana de 512), adaptada de
  `mp3.c` del original e igual a él bit a bit (2304 muestras comparadas con
  el original compilado aparte, fuera del repo). La ventana **no** es la
  tabla del microcódigo: `tools/audio-mp3-lut.py` la genera desde la
  ventana estándar (ISO 11172-3, vía `ff_mpa_enwindow` de FFmpeg, LGPL);
  coincide con la del original en todas las entradas que se leen.
  Memoria de trabajo aparte de la DMEM de las listas (2 KB, persiste entre
  comandos como el anillo de la ventana). Las dos palabras de ganancia del
  comando: Perfect Dark manda +16 y −16; la segunda corrige el signo de
  las muestras 17–31 de las ranuras impares.
- **SPE 1**: text 43 456, data 832, bss 22 416 bytes (~66 KB de 256 KB; la
  cabecera de `src/spu/audio/main.c` lleva el desglose). Cabe sin
  overlays.
- **Pruebas** (suite audio, 23): cada comando naudio con listas sintéticas
  y resultados a mano; el IIR contra una referencia directa en dos bloques
  (sigue del estado); el MP3 contra la síntesis de MPEG-1 en coma flotante
  (escala 1,00002, error rms 4,6 LSB sobre 3066, máximo 19,6). Listas
  naudio con y sin MP3 en la autoprueba SPE contra PPE del arranque
  (iguales en RPCS3).
- **Mac** (`salsa64-run`, 3 min de consola, respaldos propios): Banjo-
  Kazooie 5373 tareas, Conker 5389 y Perfect Dark 5216, todas **sin
  errores ni comandos sin implementar** (el censo: todas con error). Banjo
  y Perfect Dark suenan con niveles normales (−14 a −24 dBFS); Perfect
  Dark manda 3166 comandos MP3 en 3 min y su PCM es continuo (sin saltos
  en las fronteras de las ranuras). Conker sale en silencio: sin su
  microcódigo gráfico se queda tras la primera tarea gráfica y no dispara
  voces (no manda ADPCM ni ENVMIXER; solo el bus de efectos). WAVs para
  escuchar en `build/naudio/` (fuera de git).
- Sin regresiones: `tools/build.sh` sin advertencias, test-host en
  verde (295 con main), `tools/test-roms.sh` (nemu64-test 446, vitest, controles,
  savetest, gfxtest ×3, audiotest) y test-phase1/2 en RPCS3 OK.

- **PS3 real** (paquete «Salsa64 naudio», SLSA00080): **Banjo-Kazooie y
  Perfect Dark suenan bien** (el autor), 0 errores y 0 comandos sin
  implementar; HLE en el SPE 1 por tarea: mediana 2,9 ms en Banjo y 5,7
  ms en Perfect Dark (la más larga 10,4 ms), el PPE nunca espera.
  Conker: pantalla negra y silencio como en la Mac, sus tareas sin
  errores (1,6 ms). Detalle en docs/perf-log.md. Respaldos subidos a
  `/dev_hdd0/salsa64/` como `banjo.z64` y `conker.z64`; `embedded` quedó
  en `file:perfectdark.z64`, como estaba al empezar.

- **Después, al integrar:**
  - `tools/test-rec.sh` se caía en la suite `sync` bajo qemu: SIGILL por
    `mffscrni` en `sin`/`cos`/`floor` de la glibc estática de ppc64 (Power
    ISA 3.0; qemu 7.2 solo lo acepta desde power9). `tools/qemu-ppc64.sh`
    usa `-cpu power9` (commit df3d652, rama `qemu-sync-fix` sobre
    main); test-rec en verde, también `--lockstep`, y con naudio y MP3
    (297 pruebas en big-endian).
  - **FILTER** (nead: Zelda, Majora…) corregido contra el microcódigo real
    con la herramienta del LLE (`--lle=all --lle-check-audio`, rama del
    LLE): la tabla de la nota no se escribe; la de detrás del estado pasa a
    `(anterior + nota + 1) >> 1` y con ella se filtra. La HLE dejaba el
    filtro a la mitad (−6 dB) y reescribía la tabla de la nota. Se oye en
    los primeros 15 s de Zelda (WAVs viejo/nuevo en `build/naudio/`).
    **PS3 real: Zelda «suena bien»** (el autor), SPE 2,1 ms por tarea.
  - **MIXER** redondea el producto como el microcódigo (+0x4000): con el
    LLE, Zelda pasa de 952 k a 210 k muestras distintas, Perfect Dark de
    1,80 M a 1,46 M, SM64 de 788 a 761 tareas distintas.
  - naudio contra el microcódigo real (LLE): Conker 476 tareas idénticas;
    Perfect Dark idéntico hasta la tarea 117 (cuando empieza a sonar). Lo
    que queda es RESAMPLE (tabla propia, a propósito) y el estado de
    ENVMIXER (formato propio: la HLE lee como suyo el que escribe el LLE,
    así que solo se compara la primera tarea de cada voz). Para medir
    naudio hace falta subir AUD_MAX_RANGES en el host (con 10 los tramos se
    juntan con memoria que el CPU escribe mientras tanto).

Notas:
- Banjo-Kazooie en la PS3: el **video parpadea** (uno de cada dos cuadros
  sale incompleto: sin suelo y con medio Banjo) y el fondo sale negro. Es
  de la HLE gráfica o la presentación, no del audio: para el trabajo de F3D.
- OVERLOAD recorre 0x170 muestras como el original (el doble que los
  demás comandos, que cuentan 0x170 bytes); ningún juego lo mandó en las
  escenas a las que llegamos, así que queda sin verificar.
- Conker: 4926 huecos entre DMA del AI en 3 min (el juego detenido); no es
  de la HLE.

Falta:
- [ ] Oír Conker cuando el trabajo de F3D lo haga avanzar (F3DEXBG).
- [ ] Probar Donkey Kong 64 (sin respaldo; la tabla es la del original).
- [ ] MusyX (Rogue Squadron, Resident Evil 2, Indiana Jones…).
- [ ] Opcional, con el LLE integrado: estado de ENVMIXER en el formato del
      microcódigo (para poder comparar más allá de la primera tarea de
      cada voz) y revisar los ±1 LSB que quedan en naudio.
- [ ] docs/compatibilidad.md: la columna de audio de Banjo, Conker y
      Perfect Dark (es del censo).
## 2026-10-10 — Arranque del CIC 6105 y censo de compatibilidad

Hecho (rama `amazing-noether-39a548`; detalle en docs/cpu-vr4300.md,
«Arranque HLE», y docs/compatibilidad.md):
- `src/core/boot.c`: con el CIC 6105/7105, el arranque HLE deja lo que su
  IPL3 deja además en la RDRAM: copia de DMEM 0x554–0x888 a 0x004–0x338
  (desde la ROM del usuario; de ahí sale 0x2E8 = 0xC86E2000), el efecto del
  DMA con paso de su microcódigo del RSP (24 filas de 8 bytes en 0x2FB1F0,
  cada 0xFF0, desde RDRAM 0x1E8 vía IMEM 0x120) y el tamaño de la RDRAM en
  0x3F0. Solo direcciones, cotejadas entre fuentes (docs/THIRD_PARTY.md) y
  verificadas con dos palabras del Perfect Dark del autor. No hay más
  efectos documentados por variante (6101/6102/6103/6106/7102: dirección
  de arranque y semilla, que ya estaban).
- `osCicId` (0x310) = 6103 o 6105 con esos IPL3: Conker solo arranca su
  hilo principal si vale 6105 (se quedaba en el hilo ocioso a los 88 mil
  ciclos); Banjo-Kazooie y Perfect Dark también lo comprueban.
- `salsa64_boot_hle_as` fuerza la variante; pruebas por variante y de los
  restos del 6105 (287/287 en el host con main 3270538).
- Perfect Dark pasa la comprobación en la Mac y en la **PS3 real**
  (frontend «Salsa64 arranque», SLSA00078): 6083 bloques (6078 fuera de
  KSEG0) en vez de 201, 693 traducciones de la TLB, 59,8 campos/s con 5,8
  ms por campo, pantalla negra. Se queda en su hilo ocioso esperando al DP
  tras su primera tarea gráfica (microcódigo de Rare sin texto, sin HLE).
  `--lockstep` en qemu: 10,7 s de consola sin diferencias.
- `salsa64-run`: `--trace-rsp` muestra el CRC-32 del microcódigo y de
  `ucode_data`, los textos de `ucode_data` en las tareas gráficas y las
  palabras que distinguen el audio; muestreo del PC por segmento al final
  (KSEG0/KSEG1/TLB). `--lockstep` da a la referencia una copia del
  guardado: sin ella su PIF respondía «sin dispositivo» en el canal de la
  EEPROM y salía una diferencia falsa (Perfect Dark, ciclo 254 M).
- `tools/censo.sh`: censo de compatibilidad con los ROMs del usuario
  desde su ruta (compila `salsa64-run` con el compilador de la Mac).
- Censo de 3 minutos de 8 juegos (los 6 objetivo, Banjo-Kazooie y Conker)
  sobre main 3270538 (F3D integrado): dibujan SM64 (título),
  OoT, MK64, SF64, GoldenEye y Banjo; suenan ABI 1 y nead. Perfect Dark
  corre sin dibujar ni sonar (su microcódigo de Rare y naudio con MP3 sin
  HLE); Conker manda una sola tarea gráfica (F3DEXBG 2.08, comando 0x14) y
  su naudio da error. Sin la rama de F3D, solo Zelda dibujaba.

Siguiente (lista priorizada en docs/compatibilidad.md):
- [ ] Audio: las tres naudio (Banjo-Kazooie, Conker, Perfect Dark con MP3).
- [ ] Microcódigo de Perfect Dark (por CRC; sin texto) y F3DEXBG de Conker.
- [ ] Probar con el control y medir en la PS3 las escenas de docs/proyecto.md.

## 2026-10-10 — F3D (Super Mario 64) y F3DEX 1.x

Hecho (rama `lucid-pare-4f93fb`, con main traído hasta f698afe; sin
integrar):
- HLE de la primera generación de microcódigos en `src/rsp/gfx/f3d.c`: F3D
  (Fast3D: búfer de 16 vértices, índices ×10, G_TRI1, G_MTX con sus
  banderas, G_MOVEMEM/G_MOVEWORD con sus índices, geometry mode en dos
  comandos con otros bits, G_SETOTHERMODE_* directo, TEXRECT con
  G_RDPHALF_2 + G_RDPHALF_CONT) y F3DEX 1.x (búfer de 32, índices ×2,
  G_TRI2, G_QUAD, G_MODIFYVTX, G_CULLDL, G_BRANCH_Z, G_LOAD_UCODE). Lo
  común salió de `f3dex2.c` a `dl.c` y `geom.c` sin cambiar F3DEX2 (mismas
  31566 verificaciones). `rsp.c` manda F3D y F3DEX al backend. En el SPE 0
  son 3,3 KB de código (presupuesto en `src/spu/gfx/main.c`).
- Pruebas: 11 nuevas en el host (`tests/unit/test_gfx_f3d.c`) más la
  detección y el despacho en `test_rsp.c`: 223 en total.
- gfxtest en tres variantes (`tools/build-gfxtest.sh`; `GBI=` en su
  Makefile): F3DEX2 (la ROM sale idéntica), F3D y F3DEX. En el host las tres
  dan los mismos puntos y la misma imagen (`tools/test-roms.sh`, que además
  falla si hay comandos sin implementar): 19 de 19 antes de traer main y
  22 de 22 con las escenas de la caché de texturas. La de F3D también en
  RPCS3 (`tools/test-phase2.sh`, 22 de 22) y en la **PS3 real** (19 de 19,
  `RESULTADO frontend: OK`, 0,18 ms por tarea en el SPE). El frontend verifica la pantalla de las
  variantes cargadas con `file:salsa64-gfxtest-*.z64` como la embebida.
- **SM64** (respaldo propio, USA, fuera del repo): en el host llega al
  título y a las demos (Bowser, Whomp's Fortress) sin comandos sin
  implementar ni errores en 2 minutos; en la **PS3 real** el autor confirmó
  el título y la selección de archivo («todo se ve bien»). Mediciones en
  docs/perf-log.md: velocidad completa en el logo y en la selección de
  archivo, 67 % de NTSC en la cara de Mario (la anima el CPU).
- Otros juegos del usuario en el host (respaldos fuera del repo), sin
  comandos sin implementar: **GoldenEye** usa Fast3D 2.0G con `G_TRI4`
  (0xB1, cuatro triángulos con índices de 4 bits; antes 140 223 comandos
  sin implementar, ahora la intro se ve con la orientación bien);
  **Mario Kart 64** trae «F3DEX 0.95», que ahora se trata como F3DEX 1.x
  (logo y carrera de demostración); **Star Fox 64** (F3DEX.NoN 1.22) y
  **Banjo-Kazooie** (F3DEX 1.21) corren con F3DEX 1.x.
- Una tarea gráfica con microcódigo sin HLE termina también del lado del
  DP (ningún juego se cuelga esperando al RDP) y el frontend la anota en
  el log con el texto del microcódigo (`salsa64_rsp_ucode_text`, también
  en `salsa64-run --trace-rsp`).
- **PS3 real con main fusionado** (recompilador, dos hilos, caché,
  sondeo): SM64 sin franjas en la transición y al 100 % de NTSC en el
  logo, la transición y la cara (antes 55–67 %); GoldenEye: la intro se
  ve, al 100 % de NTSC. El autor vio después dos errores, arreglados y
  confirmados por él en la PS3:
  - La cara del Mario 3D (de frente): el combiner de los ojos,
    (TEXEL0 − SHADE)·TEXEL0_ALPHA + SHADE, multiplica la textura por su
    alfa y combine.c lo aproximaba. Ahora pide la textura premultiplicada
    (RGB·A al decodificar) y queda exacto (2c4b3f4): 0 combiners
    aproximados en SM64.
  - El cielo de GoldenEye «infinito»: lo dibuja con triángulos crudos del
    RDP partidos en G_RDPHALF_1 + G_RDPHALF_CONT/2, que la HLE no
    dibujaba. Ahora se convierten en triángulos (2c53149).
- Después de integrar (main 3270538), confirmados por el autor en la PS3:
  - El audio de SM64, GoldenEye y Mario Kart 64 suena bien.
  - GoldenEye sin ruido entre el logo y las carpetas: el frontend elige
    el destino del ancho que muestra el VI (GE usa la misma dirección
    para 320 y 440; 66a3870) y sigue presentando el destino mientras el VI
    sostiene la imagen sin dibujar (un segundo de carga; antes salía la
    RDRAM con datos viejos; 10e869e).
  - Mario Kart 64 con título, menús y letreros: F3DEX acepta TEXRECT con
    G_RDPHALF_2 + G_RDPHALF_CONT, la secuencia de la libultra temprana
    (7343e2f), y los TEXRECT en tiras sin costuras a más resolución:
    limitar el eje si todo cae dentro de la tile y muestrear en la
    esquina del píxel con filtro bilineal (8ec50de).
- `salsa64-run --pad "CAMPO:BOTONES[@X,Y];…"`: control 1 por campos para
  llegar a escenas de juego en el host (SM64 hasta el castillo, GoldenEye
  hasta Dam).
- Arreglo en `src/rdp/rdp.c` (3729f8f): unos cuadros con franjas de
  colores en el paso del logo a la cara. SM64 apaga el VI 26 campos al
  cambiar de escena; lo dibujado entonces quedaba marcado con el campo de
  antes del apagado y el frontend, al volver la imagen, presentaba la RDRAM.
  Simulado con la traza del host: de 17 campos sin imagen del RSX a 0.

Pendiente:
- [ ] Confirmar en la PS3 el arreglo de la transición (paquete «Salsa64
      F3D», SLSA00071) y dejar `embedded` como estaba (`vitest`).
- [ ] Combiners aproximados en las demos de SM64 (cientos por minuto):
      ver cuáles son (`combine.c`).
- [ ] En la PS3: GoldenEye, Mario Kart 64 y Star Fox 64 con este
      paquete (en el host ya dibujan).
- [x] Confirmar en la PS3 la cara de Mario y el cielo de GoldenEye.
- [x] Mario Kart 64: título, menús y letreros (ver arriba). Quedan las
      pantallas gigantes de las pistas, que muestran el framebuffer
      anterior como textura (efecto de framebuffer, Fase 4).
- [ ] Perfect Dark: corre sin dibujar con un microcódigo de Rare sin
      texto (CRC c543d0a8). Conker: «F3DEXBG.NoN fifo 2.08» tomado como
      F3DEX2, deja 2 comandos sin implementar (el último 0x14) y no manda
      otra tarea.
- [ ] GoldenEye: dos combiners del nivel multiplican el sombreado por su
      alfa (lerp(ENV, TEXEL0, SHADE_ALPHA)·SHADE: niebla por luz
      precalculada) y se aproximan (75 000 en 52 s de Dam). Hace falta un
      término S·Sa por vértice en la combinación del RSX (rsxkit y
      src/rdp) o el generador de fragment programs de la Fase 4.
- [x] GoldenEye: las franjas azules de los costados son del juego (3D con
      scissor en las columnas 1–318 sobre un relleno 0x1199); en una tele
      quedan en el overscan. Opción de recorte en el frontend
      (`overscan.cfg`, trabajo de presentación).
- [ ] Star Fox 64: un cuadrito con textura abajo a la izquierda en el
      logo (host). Revisar.
- [ ] G_LINE3D de F3D (no lo usan SM64 ni GoldenEye) sin implementar.
- Para correr un respaldo en el host sin copiarlo: colima no comparte
  todas las carpetas del host, así que `salsa64-run` se compila nativo en la
  Mac (`make -C src/host OUT=$PWD/build/host-mac CC=clang SAN=`) y lee la
  ROM en su lugar.

## 2026-10-10 — Audio de punta a punta: AI, HLE de audio en el SPE 1 y salida en la PS3

Hecho (rama `amazing-varahamihira-3027c8`, con main cfa146b):
- **AI** (`src/core/ai.c`, desde n64brew.dev): cola de dos DMA, AI_STATUS
  (BUSY/FULL/ENABLED), AI_LENGTH que cuenta hacia abajo y los demás
  registros que se leen como él, frecuencia = reloj del VI de la región /
  (AI_DACRATE + 1), interrupción al **empezar** cada DMA, AI_CONTROL que
  detiene el DMA y el acarreo de los límites de 8 KB (libultra lo
  compensa). Un evento del planificador por DMA; las muestras van a un
  sumidero al empezar cada DMA.
- **HLE de audio** (`src/rsp/audio/`, adaptada de mupen64plus-rsp-hle,
  GPLv2+, avisos en cada archivo y docs/THIRD_PARTY.md): ABI1 (Super Mario
  64; variante de GoldenEye) y todas las variantes nead (Zelda OoT, Mario
  Kart 64, Star Fox 64…). DMEM en el orden de la N64 con índices de la N64
  (el original dependía del host little-endian en FILTER e INTERLEAVE),
  E/S explícita de la RDRAM, tabla de RESAMPLE **propia**
  (`tools/audio-lut.py`; la del original es del microcódigo), libro de
  códigos de 16 predictores (el original leía fuera de la tabla). El núcleo
  reconoce el ABI por las palabras de `ucode_data` (`rsp_audio.c`). ABIs
  por juego objetivo: SM64 ABI1, GoldenEye ABI1 (GE), Zelda OoT / Mario
  Kart 64 / Star Fox 64 nead, Perfect Dark naudio con MP3 (pendiente).
- **SPE 1** (`src/spu/audio/`, `src/platform/ps3/audio_spu.c`): una tarea
  por trabajo de lib/spukit, E/S por DMA (escrituras en trozos de
  alineación natural: nunca reescribe RDRAM vecina), fin de tarea por
  sondeo (`done`), aviso de lo escrito con `salsa64_bus_rdram_written`.
  Local store: 27 KB de código + 20 KB de datos. Al arrancar compara
  listas de prueba (`selftest.c`) en el SPE y en el PPE.
- **Salida** (`src/frontend/audio_out.c`, `audio_sync.c`): anillo sin
  bloqueos entre la emulación y un hilo de audio que llena el puerto de
  PSL1GHT (48 kHz, 16 bloques de 256, cuatro por delante). **Ritmo:** lo
  marca pacer.c (la tele emulada); el audio lo sigue con una corrección de
  ±0,5 % del remuestreo. **Política** (`audio.cfg`): «estirar» (por
  omisión: si la emulación se aparta más de un 3–5 % sigue su velocidad
  con WSOLA sin cambiar el tono; si se vacía repite lo reciente hasta 150
  ms y luego calla) o «silencio» (calla con 10 ms de desvanecido y sigue
  donde quedó). Toda transición pasa por la suma con ventana: sin
  chasquidos, verificado en el host con productores al 27 %, 100 % y 130 %.
  Diagnóstico: `volcado_s` (WAV de lo que sale por el AI, nunca al repo),
  `tarea_us`, pico del AI en dBFS en el log. Memoria: 0,3 MB.
- **ROM audiotest** (`tests/roms/src/audiotest`, propia, IPL3 libre): tono
  conocido por el CPU (La 4, Do izquierda, Mi derecha, Sol, Do 6) y luego
  por una tarea de audio («Salsa64 ABI NEAD»); se verifica sola y
  `salsa64-run --audiotest-check` compara lo que sale por el AI muestra por
  muestra. En `tools/test-roms.sh`.
- Pruebas: suites ai (8), audio (15) y sync (5); 269 en total en el host y
  en RPCS3 (big-endian). Pruebas de mutación: romper el acarreo, el orden
  de INTERLEAVE, la escala del ADPCM o el solapamiento del estirador las
  hace fallar. nemu64-test 446 fallas (sin cambios), vitest, controles,
  savetest, gfxtest y audiotest OK; build sin advertencias. RPCS3:
  audiotest OK en el frontend (renderer de audio Null en las pruebas).
- **PS3 real** (paquete «Salsa64 audio», SLSA00072): audiotest suena bien
  (afinador en 440 Hz, cada canal de su lado), 100 % de NTSC, 0 faltas.
  **Zelda OoT suena perfecto** (el autor): 9 min al 99,9 %, HLE en el SPE
  2,4 ms por tarea (mediana; máx. 4,0), PPE esperando 0, 0 bloques tarde.
  Mediciones en docs/perf-log.md.

Notas:
- Una vez, tras la primera corrida de Zelda (120 s), la consola quedó en
  negro después de «salida normal» y hubo que reiniciarla. No se repitió
  en tres salidas más (7 s, 9 min y SM64 100 s); ahora el cierre del audio
  anota cada paso y todos devuelven 0. Además, soltar el audio
  (`salsa64_rsp_set_audio(NULL)`) ya no termina una tarea gráfica en vuelo:
  antes hacía un reset completo del RSP y podía llamar a `wake()` del
  frontend con su semáforo ya destruido. Si vuelve a pasar, mirar esas
  líneas; `salsa64_rsp_set_gfx(NULL)` en main.c tenía el mismo riesgo
  (iba después de `sysSemDestroy(wake_sem)`): corregido al integrar, el
  semáforo se destruye después de soltar la HLE gráfica. Probablemente no
  era la causa del negro: en lv2 un semáforo destruido da error, no se
  cuelga, y el negro llegó después de «salida normal» (fin del proceso:
  ¿algún hilo, SPE o el puerto de audio aún vivo?). Pendiente si se repite.
- `.gitignore`: la regla «core» de los volcados de qemu también ignoraba
  `src/core` para archivos nuevos; se agregó «!src/core/».

Falta:
- [ ] Oír Super Mario 64 (ABI1): sus tareas corren sin errores (0,115 ms)
      pero el juego no pasa del arranque sin F3D.
- [x] naudio (Perfect Dark, Banjo, Conker; Perfect Dark con MP3): ver la entrada
      de naudio. Falta MusyX.
- [ ] Probar la política «silencio» y una escena lenta en la PS3; con el
      recompilador Zelda ya va al 100 % y casi no hace falta estirar.
- [ ] Opcional: SIMD en ENVMIXER, RESAMPLE y ADPCM del SPE (hoy 2,4 ms por
      tarea en Zelda, escalar).
- [ ] Opcional: que el audio marque el ritmo (pacer.c tiene el enganche).
## 2026-10-10 — Bucles de espera en código mapeado por la TLB

Hecho (rama `loving-spence-8951ae`, desde main bd8ac59):
- `src/cpu/idle.c` traduce el código y las cargas de la vuelta también por
  la TLB, sin efectos (`salsa64_vr4300_fetch_probe`: la búsqueda del
  intérprete sin excepciones ni cambios); antes solo KSEG0/KSEG1. El «sí»
  no se guarda (cada vez se vuelve a simular con la TLB y la memoria del
  momento) y la TLB no cambia dentro de una vuelta (COP0 la descarta): un
  remapeo o código reescrito se ve en el análisis siguiente; los descartes
  siguen por pc ^ instrucción. El recompilador no cambia (consulta lo
  mismo que el intérprete).
- Pruebas: `b .` mapeado y sondeo de una variable por dirección mapeada
  (se saltan, igual con y sin detección), y la misma dirección virtual
  remapeada a un contador (deja de saltar y queda descartado).
- Mac, 21,3 s de consola con el intérprete: GoldenEye 0,0 → **97,6 %** en
  bucles de espera (16,2 → 0,8 s); Perfect Dark 0 → 99,3 % (su `b .` del
  arranque). Registros, log e imagen del VI idénticos con y sin el cambio.
- qemu-ppc64: 233/233 con el recompilador en los cuatro modos, nemu64-test
  con la línea base, vitest OK, `--lockstep` sin diferencias en
  nemu64-test, vitest y en GoldenEye y Perfect Dark (21,3 s de consola cada
  uno, con 97,6 % y 99,3 % en bucles de espera).

- **PS3 real** (524d80e, frontend «Salsa64 TLB bucles», SLSA00074):
  GoldenEye a 59,8 campos/s con **0,6 ms de emulación por campo** (antes
  5,6; 0,5 de los 0,6 son la copia del VI) y 99 % en bucles de espera
  (antes 0 %); 1,2 MIPS ejecutados en vez de 93,7.

## 2026-10-10 — Recompilador: conversiones de la FPU, LWL/LWR y MFC0 en nativo

Hecho (rama `funny-lalande-371c7e`; diseño en docs/rec-ppe.md,
cifras en docs/perf-log.md):
- En nativo, con los mismos bits, causas y banderas de FCR31 que el
  intérprete: SQRT, CVT.D.S/CVT.S.D, ROUND/TRUNC/CEIL/FLOOR a W y L,
  CVT.W/CVT.L (redondeo al más cercano), CVT.S.W/CVT.D.W (en enteros, sin
  pasar valores entre registros enteros y de punto flotante), CFC1/CTC1.
  Lo que no da exacto (redondeo dirigido, I habilitada, operandos fuera de
  la franja, NaN, desnormales) sigue en el intérprete.
- LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR en nativo; los pares LWL+LWR y SWL+SWR
  sobre los mismos 4 bytes, byte a byte (sin desplazamientos por registro ni
  load-hit-store en copias).
- MFC0/DMFC0 ya no terminan el bloque; Count desde r29 + k + 1.
- Código nuevo en `src/cpu/rec_emit_ext.h` (lo incluye rec_emit.c); en lo
  común, ganchos de una línea, para no chocar con el trabajo de TLB.
- `rec->stats.exec_calls` (llamadas al intérprete desde el código) en
  salsa64-run y cpu-tests; `SALSA64_REC_OPT_RARE_EXEC` repite lo de antes
  con el mismo binario (cpu-tests la corre como variante).
- Pruebas: `executed_conversions_match_full` (8000 instrucciones contra el
  camino completo, más de 3000 en nativo), LWL/LWR y compañía en cada
  desplazamiento contra un modelo byte a byte, pares en los dos órdenes,
  registros fuera del PPE, caminos lentos (DMEM, fallo de TLB), código que
  se reescribe; MFC0 de cada registro y Count dentro del bloque y en un
  delay slot. 221/221 en el host y con el recompilador (plain, mtctr,
  registros fijos, escritura temprana); nemu64-test con la línea base,
  vitest OK, `--lockstep` sin diferencias en los dos. Prueba de mutación:
  10 de 10 errores puestos a propósito detectados.
- Host: llamadas al intérprete −18 % en nemu64-test y −97 % en vitest.
- Con main traído (TLB y KSEG1, bd8ac59): 240/240 en arm64 y en ppc64 en
  los cuatro modos, nemu64-test con la línea base (623 bloques fuera de
  KSEG0), vitest OK, `--lockstep` sin diferencias en los dos.

Siguiente:
- [x] **PS3 real** (cpu-tests SLSA00077 «Salsa64 CPU FPU», e210aeb):
      nemu64-test 1894 → 1816 ms (−4,1 %, ×4,47 del intérprete) con las
      mismas fallas; bench cvt 4,2 → 40,7 MIPS, no alineadas 20,5 → 218,5,
      mfc0 9,8 → 701, espera por Count 8,3 → 488 (salidas a C 2 M → 8).
      Las 4 «fallas» del RESULTADO eran verificaciones de cobertura en la
      variante `RARE_EXEC` (corregidas).
- [ ] Lo que sigue yendo al intérprete: CVT.S/D.L, CACHE, MTC0/TLB/ERET;
      MFC0 de Random.
- [x] Bench del intérprete (21 → 14,7 MIPS tras la Fase 2): en el binario
      de este trabajo da 21,1 MIPS en `t_n64` y en la máquina alineada; no se
      reproduce y la hipótesis de la página de `sched->next` quedó
      descartada (docs/perf-log.md). Depende de cómo queda el binario.
- [ ] Medir en un juego (Zelda, SM64) cuánto pesan las conversiones y las
      esperas por Count cuando la HLE gráfica deje de ser el cuello.

## 2026-10-10 — Fin de tarea del RSP por sondeo (sin bloquear al SPE)

Hecho (rama `musing-mcclintock-6b5e2c`, commits a2ffd0f + mezcla
16a544f con main 4a4d98c; diseño en docs/hilos-ppe.md «Fin de tarea sin
bloquear»):
- `src/core/rsp.c`: el backend tiene `done(ctx, idle)` (no bloquea); al
  llegar el fin de la tarea, si no terminó, el evento se vuelve a programar
  cada `poll_cycles` (10 000 ciclos) y el CPU sigue emulando. Si el CPU
  saltó ciclos (bucle de espera) desde el sondeo anterior, se le avisa al
  backend y el del SPE duerme 100 µs antes de contestar: el reloj emulado
  no corre más rápido que el real. Con la tarea en curso el RSP no está
  HALT y otro arranque se ignora (`starts_busy`). Genérico para el audio.
- Prueba unitaria `rsp_gfx_task_polls`; estadísticas `sondeos` y
  `dormida` en la línea `gfx:` del frontend.
- ROM gfxtest: la espera por una interrupción vence por tiempo (10 s de
  Count) y no por vueltas (en RPCS3 el primer trabajo del SPE compila
  segundos; antes el CPU estaba congelado mientras tanto). Reconstruida.
- Verificación: 224 pruebas en el host, cpu-tests/vitest/save-tests en
  RPCS3, gfxtest 22/22, test-roms 446, build sin advertencias.

Medido en la PS3 (docs/perf-log.md, «Fin de tarea del RSP por sondeo»):
Zelda, «esperando al SPE» 2,3–8,2 ms por tarea → 0; emulación por campo
igual (3,2–7,5 → 3,2–7,2 ms) porque el CPU está ocioso mientras el RSP
trabaja y el sondeo duerme ese tiempo; 60 campos/s sin regresiones;
gfxtest OK. La ganancia llegará donde el CPU tenga trabajo durante el
RDP.

Pendiente:
- [ ] Repetir con Super Mario 64 (F3D) cuando esté en main: 9,5 ms de CPU
      por campo, el caso donde el sondeo debería rendir.
- [ ] Recompilador: un bucle que lee Count (MFC0) sale a C en cada vuelta
      (gfxtest cayó a 9 campos/s); compilar MFC0 Count en línea (trabajo FPU, MFC0 nativo).

## 2026-10-10 — Recompilador: código en KSEG1 y mapeado por la TLB

Hecho (diseño en docs/rec-ppe.md, «Código fuera de KSEG0»):
- El recompilador compila también código en KSEG1 y mapeado por la TLB
  (kuseg, kseg2/3), con la instrucción en la RDRAM o en la ROM del
  cartucho. Antes eso lo corría el intérprete; GoldenEye y Perfect Dark
  corren casi todo su código mapeado.
- Clave de bloque (vpc, física) en una tabla aparte (`xhash`); KSEG0 sigue
  igual. Todo en `src/cpu/rec_tlb.c` (para que los merges con otros
  cambios del recompilador sean fáciles); rec.c y rec_emit.c solo lo llaman. Los bloques mapeados comparan su traducción al entrar contra una
  TLB del recompilador por página de 4 KB (`vtlb[]`, 4 instrucciones),
  también al llegar por un enlace o por JR. Escribir la TLB no mata ni
  desenlaza bloques: antes de correr código, el despachador vuelve a
  traducir las páginas anotadas si cambiaron la TLB, el ASID o ERL.
- JR/JALR encuentran en el propio código los bloques fuera de KSEG0.
- `cop0.c`: `salsa64_vr4300_fetch_probe` (búsqueda en la TLB sin
  excepciones) y `cpu->tlb_writes`.
- Pruebas nuevas (también en modo estricto): código mapeado con JAL entre
  páginas, remapeo con enlace, cambio de ASID, código mapeado reescrito por
  la dirección mapeada y por KSEG0, KSEG1, ROM por TLB y por KSEG1, dos
  páginas en la misma entrada de `vtlb[]`, bucle mapeado; bucles de espera
  en KSEG1 (se saltan) y mapeados (se descartan, como con el intérprete).
- qemu-ppc64: unitarias 222/222 con el recompilador en los cuatro modos;
  nemu64-test con las fallas de la línea base y 623 bloques fuera de KSEG0;
  vitest OK; `--lockstep` sin diferencias en nemu64-test y vitest.
- Cambiar la ROM o el ISViewer vacía el recompilador (puede haber código
  compilado desde la ROM).
- Memoria: +572 KB del estado del recompilador (6,84 MB).

Siguiente:
- **PS3 real** (b11861c, cpu-tests «Salsa64 CPU TLB», SLSA00076): 232/232
  en el intérprete y en las cuatro variantes; nemu64-test igual al
  intérprete (446 fallas, hash y ciclo), ×4,04–4,29; bench enteros con
  registros fijos 760 MIPS (el préstamo en bucles funciona; antes 128).
- qemu-ppc64 con `--lockstep` (respaldos propios, fuera del repo):
  GoldenEye y Perfect Dark 200 M ciclos cada uno sin diferencias con el
  intérprete. GoldenEye: 3518 de 3524 bloques mapeados y 5050
  traducciones en 2,1 s (reescribe la TLB seguido); Perfect Dark con 8 MB:
  3 traducciones.
- **PS3 real, frontend «Salsa64 TLB»** (SLSA00074, respaldos propios):
  - GoldenEye: 59,8 campos/s a 93,7 MIPS (velocidad completa del VR4300),
    **5,6 ms de emulación por campo**; 3512 de 3518 bloques mapeados, 5050
    traducciones en el arranque y ninguna después, ~0,08 % interpretado;
    estable 100 s. En pantalla, ruido: el juego manda una sola tarea
    gráfica con un microcódigo que la HLE no reconoce (ucode en 0x20e60,
    su F3D) y después solo audio; con el intérprete solo, en la Mac, queda
    igual (mismo bucle en 0x7000071c a los 21 s). Le toca al trabajo de F3D.
  - Perfect Dark: 59,8 campos/s, 5,1 ms por campo, pero se queda en el
    arranque (201 bloques, VI sin configurar, pantalla negra); con el
    intérprete solo también (pc 0x700017f4 a los 21 s, sin tareas del RSP
    ni excepciones). No es del recompilador: falta ver el arranque (CIC
    6105) o algún registro del hardware.
- Diagnóstico (arnés en la Mac, con el intérprete; lockstep de 10,7 s de
  consola sin diferencias en los dos juegos):
  - GoldenEye llega a su hilo ocioso (`b .` en 0x7000071c) y se queda: su
    única tarea gráfica (ucode en 0x20e60) no la reconoce la HLE, y la
    tarea nunca termina para el juego (sin DP), así que no vuelve a
    dibujar; el VI muestra RDRAM sin inicializar (el ruido). Pagina su
    código por fallos de TLB (110 en 10 s). Además el `b .` mapeado no lo
    salta idle.c (solo KSEG0/KSEG1): 0 % en bucles de espera.
  - Perfect Dark (CIC 6105) compara la palabra en 0xA00002E8 con
    0xC86E2000 y, si no coincide, se queda en `b .` (0x700017f0). Ningún
    código del juego la escribe: la deja el arranque del 6105 y nuestro
    arranque HLE no. Escribiéndola a mano (experimento, no en el repo)
    pasa, monta 0x7F000000 en la TLB y llega a su hilo ocioso.
- [x] Arranque HLE del CIC 6105 (`src/core/boot.c`; trabajo «Arranque del CIC
      6105 y censo»): lo que el IPL3 deja
      en la RDRAM (al menos 0x2E8 = 0xC86E2000), desde documentación
      pública, sin código de Nintendo. (Hecho: entrada de arriba.)
- [ ] HLE gráfica: el microcódigo de GoldenEye (F3D; trabajo de F3D) y que
      una tarea con microcódigo desconocido no deje colgado al juego.
- [x] idle.c: bucles de espera en código mapeado por la TLB (entrada de
      arriba).
- [x] GoldenEye ejecuta 0x7F000000 desde la RDRAM: pagina su código por
      fallos de TLB (visto en la emulación, ver abajo).

## 2026-10-10 — Caché de texturas entre tareas (HLE gráfica)

Hecho (rama `unruffled-maxwell-2af921`; diseño en docs/texturas.md):
- Las texturas decodificadas viven en una región de 8 MB mapeada al RSX
  (`src/rdp`) que la HLE del SPE 0 administra entre tareas
  (`src/rsp/gfx/texcache.c`). Identidad: hash de 64 bits (ronda de
  xxHash32 en ocho carriles, SIMD en el SPE) de los bytes de la TMEM que
  lee la decodificación —texels y paleta— y de los parámetros de la tile:
  un cambio en la RDRAM o en la paleta da otra textura sin vigilar
  escrituras (cubre las del CPU, que no avisan). Al arrancar, el frontend
  comprueba que el hash del SPE da lo mismo que el del PPE.
- Anillo con desalojo en orden de llegada; lo que el RSX aún puede leer
  (`job->done_task`, de las barreras de `src/rdp`) se salta. Sin lugar, la
  textura va al arena de la tarea. `salsa64_rdp_dump` copia las texturas
  de la caché al volcado (mismo formato).
- Cargas a la TMEM en bloque (sin `memcpy` de 8 bytes): ×2,4.
- Medición en el frontend: ventanas fijas de 100 campos («gfx campos A–B»,
  con el peor campo), capturas en campos fijos y salida sola
  (`capturas` = «N M»); acomodado sobre los dos hilos de main.
- PS3 real, Zelda (intro y título, con el recompilador; docs/perf-log.md):
  HLE en el SPE 14–31 → 5–9,6 ms por tarea; decodificar 6,7–17,2 → 0–0,09
  ms; las escenas a 41–48 campos/s llegan a 60; capturas idénticas byte a
  byte. Decisión: todo en el SPE 0; los SPE 2–3 quedan para el escalado de
  texturas (docs/texturas.md).
- Pruebas: caché entre tareas, texel y paleta cambiados en la RDRAM, anillo
  con texturas en uso y directorio lleno, hash por tramos; gfxtest con una
  textura y una paleta que el CPU cambia en el cuadro 60 (22 puntos).

Pendiente:
- [ ] LOADTILE pide cada fila con un DMA que espera (~2300 lecturas por
      tarea en Zelda): pedirlas por adelantado. Es lo más caro que queda
      en el SPE junto con el estado de dibujo.
- [ ] Decodificación por formato con SIMD y salidas de 16 bits en
      `lib/rsxkit/`: solo cuando falla la caché; medir antes el peor campo
      en cambios de escena (Kokiri, partida en juego).
- [ ] Achicar el arena de texturas por juego (casi no se usa).

## 2026-10-10 — Dos hilos en el PPE (frontend)

Hecho (rama `musing-mcclintock-6b5e2c`; diseño en docs/hilos-ppe.md):
- `src/frontend/main.c`: hilo de emulación (`sysThreadCreate`, prioridad
  1000, pila de 256 KB) con el CPU, el planificador, el envío de tareas al
  SPE, los controles aplicados al PIF y la copia de la imagen del VI; el
  hilo principal (1001) traduce la salida de la HLE al RSX, presenta con
  flip en vsync, lee los DualShock 3, escribe el guardado, el log de
  estadísticas, capturas y volcados. Duerme en un semáforo (la emulación
  lo despierta por campo y por tarea); nunca espera activa (SMT).
- Colas preasignadas: cola de campos (8 mensajes) con copia del VI en 3
  ranuras (4,4 MB; estados por `lwarx/stwcx.`), cola de tareas gráficas
  de 3 entradas dentro de `src/rdp` (`salsa64_rdp_flush` traduce en el
  hilo de presentación; `acquire` espera un juego libre con
  `rsxkit_fence_done`, la única función de rsxkit documentada como segura
  desde otro hilo), buzón de controles con seqlock, zona de paso del
  guardado (`savefile_stage` / `savefile_write_staged`), lwmutex en el log.
- Si la emulación va más rápida que la tele: se presenta el campo más
  nuevo y los viejos se saltan (`saltados` en el log); la emulación nunca
  espera al vsync. Los campos con verificación del selftest no se saltan.
- Verificación igual que antes: selftest de vitest (RDRAM en emulación,
  pantalla en presentación), gfxtest, capturas, `gfxdump`, repetición
  (`dump:`, de un hilo: llama a `salsa64_rdp_flush`). Pruebas: 212 en el
  host; test-roms (nemu64-test 446 fallas, vitest, controles, savetest,
  gfxtest); fase 1 y fase 2 en RPCS3.
- Lugar para el audio marcado con `audio_out:` en main.c (inicio, cada
  campo presentado, cierre; y `emu_thread` si necesita el hilo de
  emulación).
- Ritmo (`src/frontend/pacer.c`): cada
  campo a la hora de la tele emulada con el reloj real, sin acumular
  deuda; `SALSA64_DATA_DIR/turbo` lo quita para medir el máximo; la
  medición A/B de vitest va sin límite. Enganche para que el audio marque
  el ritmo (`sleep` reemplazable, `pacer_nudge`). 5 pruebas unitarias.

Medido en la PS3 real con el recompilador (main 697922c mezclado; tabla
en docs/perf-log.md, «Dos hilos en el PPE»): Zelda en el título con el
castillo 41–52 → 57–60 campos/s (el flip del vsync ya no frena a la
emulación); vitest sin límite 187 → 363 campos/s con la presentación a
60/s saltando el resto, y con ritmo 60,0 exactos; guardado por hilos y
START + SELECT bien. Las capturas periódicas `screen-N.ppm` pasaron a ser
opcionales (`SALSA64_DATA_DIR/capturas`): cada una frena ~400 ms y era el
«tirón» cada 10 s; en prueba automática siguen siempre.

Pendiente:
- [ ] La espera del hilo de emulación al SPE al terminar cada tarea (10–27
      ms por tarea, 20–53 % del hilo en Zelda) es el cuello siguiente:
      que el evento del RSP se vuelva a programar en vez de bloquear
      (`src/core/rsp.c`), o acortar la HLE (caché de texturas).
- [ ] Copia del VI (0,2–0,3 ms por campo) aunque el VI muestre un destino
      del RSX: evitarla con una pista del hilo de presentación.
- [ ] Repetir la medición con Super Mario 64 cuando F3D esté en main (caso
      de libro: 9,5 ms de emulación + 7–8 ms de presentación en un hilo).
- [ ] Menú y audio en el hilo de presentación (trabajos aparte).

## 2026-10-10 — Fase 3: recompilador MIPS → PPC64 en el PPE (×4,4 con código real)

Hecho (diseño y decisión en docs/rec-ppe.md, números en docs/perf-log.md):
- **Decisión: recompilador propio.** Not64 y Wii64 (GPLv2+, commits en
  docs/THIRD_PARTY.md) se revisaron y descartaron como base: emisor de 32
  bits con pares hi/lo, EABI de la Wii, globales de mupen64, Count solo en
  saltos y FPU sin FCR31. Se usaron solo como referencia de diseño.
- `src/cpu/rec.c`, `rec_emit.c`, `rec.h`, `ppc.h`: bloques desde KSEG0
  hasta un salto con su delay slot; en nativo solo si caben antes del
  próximo evento, así que eventos, Count, interrupciones y excepciones caen
  en el mismo ciclo que con el intérprete. 12 registros MIPS por bloque,
  escritura temprana (contra el load-hit-store entre bloques), enlaces
  parcheados, JR/JALR sin volver a C, invalidación por código escrito
  (también desde el intérprete, DMA del PI/SI/SRAM y cargador), FPU nativa
  con las mismas causas de FCR31 que el intérprete, bucles de espera con la
  misma consulta que `idle.c`. Lo raro (LWL/LWR, COP0, conversiones de la
  FPU, TLB, KSEG1, modo usuario) lo corre el intérprete.
- Caché en la cola NOBITS de 16 MB del segmento de texto
  (`mk/rec-cache.ld`): llamadas a C con `bl` directo. Estado: 6,3 MB.
- Pruebas en la Mac sin PS3: Linux ppc64 + qemu (`tools/test-rec.sh`,
  `docker/ppc64.Dockerfile`): las 197 unitarias también con el recompilador
  en modo estricto; nemu64-test con la línea base; vitest OK; `salsa64-run
  --lockstep` compara contra el intérprete bloque a bloque (sin
  diferencias en nemu64-test).
- **PS3 real** (f7b655d): 163/163 unitarias ejecutando código generado;
  nemu64-test 7,9 s → 1,8 s (**×4,36, 104 MIPS**) con las mismas fallas y
  el mismo ciclo final; bench mixto 21,3 → 156,6 MIPS.
- RPCS3 no ejecuta código generado: ahí se apaga
  (`/dev_hdd0/salsa64/sin-recompilador`, lo crea `tools/rpcs3-run.py`).
- Merges de main (controles, bucles de espera, guardado, Fase 2 gráfica)
  sin regresiones; el DMA del SP, del SI y de la SRAM avisan al
  recompilador.
- Registros fijos (sp, ra, v0, v1, a0–a3) activos en el frontend: +7 % en
  nemu64-test (×4,45 del intérprete); los bucles toman prestados los que no
  usan. La escritura temprana no compensó: opción apagada.
- **Zelda OoT en la PS3 real con el recompilador: 41–60 campos/s** (con el
  intérprete 15–17), 4–14 ms de emulación por campo; se ve igual. Donde no
  llega a 60, el PPE espera a la HLE gráfica del SPE.
- vitest en el frontend: 60 campos/s, 2,9–5,4 ms de emulación por campo.
- Paquetes de prueba con APPID propio: SLSA00063 «Salsa64 recompilador»
  (frontend) y SLSA00065 «Salsa64 CPU recompilador» (cpu-tests con cuatro
  variantes del recompilador y nemu64-test contra el intérprete).

Siguiente:
- [ ] Medir en la PS3 el préstamo de registros fijos en bucles (bench
      enteros con registros fijos: esperado ~750 MIPS, antes 128).
- [x] Más juegos de la tabla de docs/proyecto.md (SM64, MK64, SF64; GoldenEye y
      Perfect Dark usan TLB: ver la entrada de arriba). (Censo en
      docs/compatibilidad.md.)
- [x] Más en nativo: CVT/TRUNC/ROUND de la FPU, LWL/LWR/SWL/SWR, MFC0 sin
      salir del bloque (entrada de arriba). Código por TLB y KSEG1: hecho.
- [ ] CACHE.
- [ ] Fase 2: el DMA del SP hacia la RDRAM tiene que llamar a
      `salsa64_bus_rdram_written`.
## 2026-10-10 — Fase 2 (parte gráfica): criterio cumplido en PS3 real

Hecho (rama `reverent-leavitt-d0f943`, integrada en main después del
guardado y los controles):
- `lib/spukit` (MIT): trabajos por lotes en SPEs, cola en memoria principal
  y timbre por mailbox (1,76 µs ida y vuelta en la PS3).
- HLE de F3DEX2/F3DZEX2 portable (`src/rsp/gfx/`) que corre en el SPE 0
  (`src/spu/gfx/`, E/S por DMA con doble búfer) y en el host; RDP sobre
  `lib/rsxkit` (`src/rdp/`): combiners como combinación lineal, texturas,
  z-buffer, culling, niebla, mezcla según la fórmula del blender.
- Criterio: gfxtest (homebrew propio con el IPL3 libre) 19 de 19 puntos en
  host, RPCS3 y PS3; Zelda OoT (respaldo propio) llega al título y la
  intro se ve sin errores gráficos en la PS3 real (confirmado por el autor).
- Arreglos de la ronda con la PS3: TASKDONE en tareas de libultra sin HLE,
  destinos alineados a 128 (temblor), frente antihorario, mezcla opaca del
  cielo, corrección de perspectiva que `cgcomp` quita en MUL/DP (la luna;
  docs/rsx-notes.md) y FILLRECT inclusivo en 1 ciclo (columna del fundido).
  Todos verificados por el autor en la PS3 real con `salsa64-fase2.pkg`
  (APPID propio SLSA00062, título «Salsa64 Fase 2 (gfx)»).
- Depuración: volcado de la salida de una tarea (`gfxdump`), repetición en
  el RSX sin emular (`embedded` = `dump:<nombre>`), `tools/gfxdump.py` para
  editar volcados y `salsa64-run --raster-dump`.
- Mediciones en docs/perf-log.md: el cuello es el CPU (64 ms por campo, 26 %
  de NTSC); la HLE en el SPE cuesta 1–21 ms por tarea, sobre todo
  decodificando texturas.

Pendiente:
- [ ] Caché de texturas entre tareas y decodificación por formato en el SPE
      (medir antes/después en la PS3).
- [x] F3D (Super Mario 64): ver la entrada «F3D (Super Mario 64) y F3DEX 1.x».
- [ ] Casos de Cell64 (espada Kokiri sin textura, fuente de la pantalla de
      nombre).
- [ ] Al integrar con la Fase 3: `sp_dma` hacia la RDRAM debe llamar
      `salsa64_bus_rdram_written`.
- [ ] «NO CONTROLLER» en Zelda: el control por SI/PIF ya está integrado;
      comprobar en la PS3 que Zelda lo ve.
- [ ] Comprobar en la PS3 Zelda con la HLE en el SPE y la detección de
      bucles de espera juntas: lo que el RSP escriba o avise fuera del
      CPU debe ir en un evento del planificador (nota de los controles).

## 2026-10-10 — Guardado del cartucho: EEPROM y SRAM

Hecho:
- **EEPROM de 4 y 16 Kbit** en el canal 4 del PIF (`src/core/save.c`,
  desde n64brew.dev: Joybus Protocol): información (0x00/0xFF →
  0x0080/0x00C0), lectura (0x04) y escritura (0x05) de bloques de 8 bytes;
  en la de 4 Kbit los bloques 64–255 repiten 0–63. Escritura instantánea
  (en la consola, hasta 6 ms), nunca "ocupada".
- **SRAM de 256 Kbit** en el dominio 2 del PI (0x08000000, espejo cada
  32 KB): DMA en los dos sentidos y accesos directos de 32 bits del CPU.
- **Tipo por juego** (`salsa64_save_detect`): `guardado.cfg` (manual; por
  código de 3 o 4 caracteres, CRC1 o `*`) > cabecera de homebrew ("ED" en
  0x3C, tipo en 0x3F; la de n64tool de libdragon) > tabla por código
  (`src/core/savedb.c`: 178 códigos y 6 excepciones por región, solo donde
  coinciden ares y N64FlashcartMenu) > ninguno, con aviso en el log. Los
  seis juegos objetivo están en la tabla (SM64, MK64, SF64 y GoldenEye
  EEPROM 4K; Perfect Dark 16K; Zelda OoT SRAM). FlashRAM, SRAM de 768 Kbit
  y de 1 Mbit se reconocen pero no se emulan todavía.
- **Persistencia** (`src/frontend/savefile.c`, igual en la PS3 y en el
  host): un archivo por juego y tipo en `/dev_hdd0/salsa64/saves/`
  (`<título>-<CRC1>.eep|sra`, los nombres de Project64). Búfer preasignado
  de 32 KB; se cuentan las escrituras del juego que cambian datos y se
  vuelca tras 2 s sin escrituras (a lo más 30 s si no paran) y al salir.
  Escritura segura: `.tmp`, borrar y renombrar.
- Ganchos mínimos en `pif.c`, `pi.c` y `bus.c` (24 líneas); `bus->save`
  sobrevive al reset.
- Pruebas: suite `save` (14 pruebas; 196 en total) en el host y en RPCS3
  (big-endian). ROM propia `savetest` (`tests/roms/src/savetest`, tres
  variantes por la cabecera, `tools/build-savetest.sh`): escribe un patrón,
  lo relee y en el arranque siguiente verifica que persistió. En
  `tools/test-roms.sh` (dos arranques por tipo con `salsa64-run
  --save-dir`) y en RPCS3 con `salsa64-save-tests` (SLSA00020, las tres
  ROMs sin pantalla; `tools/test-phase1.sh` lo abre dos veces): pasan,
  con el volcado diferido a los 2,0 s. Prueba de mutación: romper la
  repetición de bloques, el identificador, la escritura directa o el DMA
  hacia la SRAM lo atrapan las pruebas y la ROM.
- Sin regresiones: nemu64-test 462 fallas, vitest y controles OK; compilan
  fase 0, fase 1 y el frontend sin advertencias.

- **PS3 real** (Slim CECH-3011A, HFW 4.91 + HEN): `salsa64-save-tests`
  abierto dos veces pasa en los tres tipos ("persistió el arranque 1");
  volcado diferido a los 2,0 s y, en la EEPROM de 16 Kbit, escritura al
  cerrar con cambios pendientes 26 ms después del resultado, que también
  persistió. El `rename` del `.tmp` funciona en el disco de la PS3. El
  frontend real (variante «Salsa64 guardado», SLSA00021, para no pisar la
  app de la Fase 2) con `savetest-sram` como `rom.z64`: azul con un cuadro
  y, al volver a abrir, verde con dos; START + SELECT llega a
  `savefile_close` y a "salida normal".
- `tools/rpcs3-run.py`: en el primer lanzamiento de un ELF nuevo a veces no
  alcanza a leer el TTY.log (RPCS3 compila sus módulos); si el programa
  termina sin RESULTADO leído, lo toma del "Accumulated RPCS3 TTY" del
  final de RPCS3.log.

Notas:
- En la PS3 quedaron `saves/SALSA64_SAVE_*` (de las ROMs de prueba) y los
  paquetes `salsa64-save-tests.pkg` y `salsa64-guardado.pkg`; el
  `rom.z64` de prueba se borró (el `oot.z64` del autor sigue ahí).
- En la primera salida del frontend en la PS3 el log no tuvo "salida
  normal" (no se sabe cómo se salió); en la siguiente, con START + SELECT,
  sí. Si vuelve a faltar, revisar `rsxkit_shutdown`: el guardado se vuelca
  antes, así que no se pierde.

Falta:
- [ ] Probarlo con Zelda (SRAM) y un juego con EEPROM cuando la Fase 2
      permita llegar a guardar (respaldos propios, fuera del repo).
- [x] FlashRAM (2026-10-11, entrada de arriba); faltan la SRAM de 768 Kbit
      y la de 1 Mbit.
- [x] `src/core/rcp.h`: el comentario del SI ya menciona la EEPROM (al
      integrar con la Fase 2) y el del PI, la SRAM.

## 2026-10-10 — Controles (PIF + DualShock 3) y bucles de espera

Hecho (validado en PS3 real, Slim CECH-3011A, HFW 4.91 + HEN):
- **Controles** (ceae30f): comandos joybus del PIF por el SI
  (`src/core/pif.c`, desde n64brew.dev): identificación, estado, accesorio
  sin pak, RST con L+R+Start, errores de canal. Frontend
  (`src/frontend/padmap.c`): stick izquierdo → stick (±80), stick derecho
  → botones C, × → A, □ → B, L2 → Z, L1 → L, R1 → R, START; todo
  configurable en `/dev_hdd0/salsa64/controles.cfg`. El puerto 1 siempre
  tiene control (el DS3 tarda ~0,2 s en aparecer y los juegos buscan
  controles al arrancar). vitest identifica los puertos y lee el control
  1 por el SI: en la PS3 llegaron a la ROM todos los botones, los sticks
  en su sentido (arriba = Y +80), los C del stick derecho y RST.
- **Bucles de espera** (940f6f8, `src/cpu/idle.c`): punto fijo exacto de
  una vuelta y salto en vueltas enteras hasta el próximo evento. Estado
  idéntico con y sin detección (pruebas, vitest, nemu64-test). PS3 real
  con vitest: emulación 65,6 → 35,0 ms por campo (×1,9), 15,2 → 28,3
  campos/s, 45,6 % del tiempo saltado. Costo en el bench: −1 % (21,2 →
  21,0 MIPS). docs/perf-log.md.
- Intérprete optimizado en el frontend: 23,8 MIPS con vitest (antes 13,6).
- 182 pruebas en el host y en RPCS3 (big-endian) y en la PS3; nemu64-test
  sin cambios (462).

Notas:
- ppu-gcc no pone indicaciones estáticas en los saltos (bits a/t en 0): los
  `+`/`-` que muestra objdump no son predicciones del PPE.
- Quien escriba la RDRAM fuera del CPU (RSP en SPE, DMA con latencia) debe
  hacerlo en un evento del planificador, o el salto de bucles de espera se
  lo pasaría de largo (docs/cpu-vr4300.md, «Bucles de espera»).
- `salsa64-cpu-tests-idle.pkg` (título «Salsa64 CPU bucles de espera») se
  instala sobre la misma app SLSA00005 que los demás cpu-tests.

Siguiente:
- [x] EEPROM del cartucho (canal 4 del PIF) y SRAM: entrada de arriba.
- [ ] Controller Pak y Rumble.
- [ ] Bucles de espera: sondeos de E/S que solo cambian en eventos
      (MI_INTR, SP_STATUS) y esperas por Count. (Código mapeado por TLB:
      hecho, 2026-10-10.)
- [ ] Opcional: decidir el chequeo con el inmediato del salto (no depende
      del destino) y medir si recupera el 1 %.
- [ ] Repartir emulación y presentación entre los dos hilos del PPE (ya
      importa: con vitest la emulación baja a 35 ms por campo).

## 2026-10-10 — Resumen: Fase 0 y Fase 1 en PS3 real

Hecho hoy en hardware real (Slim CECH-3011A, HFW 4.91 + HEN; sin CFW posible):
- Fase 0 completa: udplog, spu-compute (6 SPUs), rsx-triangle, cpu-tests y
  jit-ppe pasan. JIT viable solo en `.text`; la caché del recompilador va en
  una cola NOBITS de 16 MB del segmento de texto (docs/jit-ppe.md).
- Fase 1 cumplida: `salsa64.pkg` muestra la ROM `vitest` (entrada de abajo).
- Intérprete en el PPE: 8,7 → 21,2 MIPS; no llegará a ~60 sin recompilador.
- Referencia Cell64 en la misma consola: Zelda en Kokiri 45–50 VI/s, ~15 cps,
  y cuatro fallas gráficas como casos de prueba (docs/referencia-cell64.md).
- Proyecto renombrado a **Salsa64**; repo en github.com/ricowp81/Salsa64.
- Herramientas: log también en disco (`/dev_hdd0/salsa64/<prog>.log`),
  `tools/ps3-live.py` (monitor en vivo), `tools/ps3.conf` con la IP.

Siguiente (una prueba en la PS3 a la vez):
- [ ] Fase 2: RSP gráfico (F3DEX2) en SPE 0 + RDP mínimo en el RSX sobre
      `lib/rsxkit`, con la infraestructura de trabajos SPE.
- [ ] Fase 3: recompilador MIPS → PPC64 en el PPE.
- [x] Controles (SI/PIF + DualShock 3) y detección de bucles de espera
      (entrada de arriba).
- [ ] Después de la Fase 2: audio HLE en SPE 1 sobre la misma infraestructura.

## 2026-10-10 — Fase 1: un homebrew muestra imagen (criterio cumplido)

Hecho:
- VI (`src/core/vi.c`): temporización por campos con dos eventos del
  planificador; duración desde VI_V_SYNC, VI_H_SYNC y el reloj de cada tipo
  de TV (NTSC 1 567 041 ciclos, 59,83 Hz; PAL 49,92 Hz); VI_V_CURRENT
  calculado al leerlo, interrupción en VI_V_INTR, entrelazado con campos de
  262/263 líneas. `salsa64_vi_frame` describe la imagen visible (origen,
  tamaño por VI_H_VIDEO/VI_V_VIDEO y escalas, RGBA5551/RGBA8888);
  `salsa64_n64_run_field` corre hasta el fin del campo.
- `salsa64-run --frame` guarda la imagen del VI en PPM. La pantalla de
  resultados de nemu64-test se ve completa.
- ROM `vitest` (`tests/roms/src/vitest`, propia, GPLv2+): clang 14 para MIPS
  + nust64 (IPL3 libre r7) en contenedor reproducible
  (`tools/build-vitest.sh`). Barras, rampa y cuadro animado alternando
  RGBA5551 y RGBA8888 con doble búfer en la interrupción del VI; se
  verifica sola (Count por campo exacto, V_CURRENT en el manejador).
- `lib/rsxkit` (MIT, `lib/rsxkit/README.md`): init del RSX y video, cuadro
  con doble búfer y flip en vsync, framebuffer 2D escalado (fit/entero/
  estirar, lineal/cercano) con 5 formatos leídos directo por el RSX
  (RGBA5551 = textura R5G5B5A1). Compila aparte, sin -Isrc.
- Frontend (`src/frontend`, `salsa64.pkg`, SLSA00064): ROM de
  `/dev_hdd0/salsa64/rom.z64` o vitest embebida; un campo de emulación y
  una presentación por vuelta; con vitest se verifica solo (RDRAM y pantalla
  en ambos formatos + resultado de la ROM). Sale con START + SELECT.
- Pruebas: 161 en el host y en RPCS3; nemu64-test sin cambios (462);
  `tools/test-roms.sh` corre vitest y `tools/test-phase1.sh` el frontend.
- **PS3 real** (CECH-3011A, HEN): `RESULTADO frontend: OK` en el campo 124;
  8,7 campos/s (14 % de NTSC), 13,6 MIPS, presentación 0,3 ms por campo
  (docs/perf-log.md).

Notas:
- El log UDP no llegó a `tools/udplog.py`: había otro receptor abierto en el
  mismo puerto (`jit-ppe-cache`, desde las 09:27) que se quedó con los
  paquetes unicast. El log en disco (`/dev_hdd0/salsa64/salsa64.log`, por
  FTP) sí estaba completo.
- Los 13,6 MIPS se midieron antes del perfilado. La sospecha del FPSCR no
  aplicaba; el intérprete optimizado da 21,2 MIPS en el bench (entrada
  siguiente).

Siguiente (Fase 1):
- [x] Volver a medir el frontend con vitest con el intérprete optimizado:
      15,2 campos/s, 23,8 MIPS (sin detección de bucles de espera).
- [x] Detección de bucles de espera: ×1,9 con vitest (entrada de arriba).
- [x] SI: comandos del PIF (controles) y mapeo del DualShock 3.
- [ ] Repartir emulación y presentación entre los dos hilos del PPE.
- [ ] Origen del VI: verificar contra ares si libultra deja VI_ORIGIN una
      línea después del inicio del framebuffer (aquí se toma como la
      primera línea visible).
- [ ] Bus PI, cachés, CPUTest de krom y camino plano: igual que abajo.

## 2026-10-10 — Intérprete en el PPE: 8,7 → 21,2 MIPS

Hecho (números y diagnóstico en docs/perf-log.md):
- Benchmark en variantes (mixto, nop, enteros, memoria, mov.s, add.s) con
  MIPS y ns por instrucción.
- La FPU nunca cambiaba el FPSCR: la sospecha no aplicaba. Lo caro era el
  bucle (~250 ciclos por instrucción aun para un NOP: load-hit-store, una
  llamada con marco de pila por instrucción, `lwax` en los `switch`) y, en
  la FPU, pasar valores entre registros enteros y de punto flotante por la
  pila.
- Bucle con pc/next_pc/flag de salto en registros y `cpu->redirect` para
  excepciones/ERET/set_pc; r0 nunca se escribe; NOP sin segundo despacho;
  BC1 en el bucle; `-mno-relative-jumptables` en mk/ps3.mk; camino rápido
  de la FPU (src/cpu/fpu.h) con prueba de equivalencia de 20 000 casos.
- PS3 real: mixto 8,7 → 17,1 MIPS (×2,0); nop ×3,0; add.s ×2,9. 155 pruebas
  en host y RPCS3; nemu64-test sin regresiones.

- Segunda ronda (147d5ac): SPECIAL en la misma tabla de despacho, FPU
  rápida que clasifica con los bits en enteros y una franja de exponentes
  segura (una sola comparación de punto flotante), C.cond en enteros y
  MOV/ABS/NEG/MFC1/MTC1/CFC1/DMFC1/DMTC1 en el bucle. Prueba de
  equivalencia de 30 000 casos. PS3 real: mixto 17,1 → 21,2 MIPS; mov.s
  ×1,8; add.s ×1,8; nop −10 % (cadena de despacho más larga).

Siguiente:
- [ ] Opcional, poco: índice de despacho ya escalado y calculado directo de
      la palabra de la instrucción (recupera parte de los ~9 ciclos que
      perdieron NOP y LW/SW).
- El bucle cuesta ~93 ciclos aun para un NOP: el intérprete no llegará a
  ~60 MIPS en el PPE sin decodificar por adelantado, y la dependencia entre
  instrucciones por `cpu->gpr` en memoria no se quita en un intérprete. La
  meta de velocidad completa es el recompilador (Fase 3).

## 2026-10-10 — Fase 1: arranque HLE, `salsa64-run` y nemu64-test

Hecho (detalle en docs/cpu-vr4300.md):
- Arranque HLE sin PIF ROM ni IPL3 (`src/core/boot.c`, `src/core/rom.c`):
  normaliza .v64/.n64 a .z64, reconoce el IPL3 de Nintendo por CRC-32 y tiene
  dos caminos. ELF (IPL3 libre de libdragon, el de nemu64-test): carga los
  PT_LOAD, deja los boot flags en DMEM (memoria en [0], TV en [9], ELF en
  [12]) y $sp al final de la RDRAM. Plano (IPL3 de Nintendo o compat): copia
  1 MB desde 0x1000 a la dirección del encabezado (ajustada por CIC) y deja
  las variables de libultra en 0x300 y s3–s7. Común: Status 0x34000000,
  tiempos de PI DOM1, RI_SELECT, registros de RDRAM. Fuentes documentadas.
- ISViewer en 0x13FF0000, opcional (tapa los últimos 64 KB de una ROM de
  64 MB).
- `salsa64-run` (`src/host/run.c`, `tools/build.sh host`): corre una ROM N
  ciclos o hasta un texto del ISViewer, detecta bucles sin salida, traza
  excepciones (`--trace-exc`). Sin asignaciones en el bucle.
- nemu64-test compilado en contenedor reproducible
  (`tools/build-nemu64-test.sh`, mismo SHA-256 desde cero), licencias e IPL3
  verificados, en `tests/roms/homebrew/` con sus avisos.
- `tools/test-roms.sh`: corre nemu64-test y compara con la línea base de
  fallas conocidas (`tests/roms/nemu64-test.fallas`); una regresión lo hace
  fallar (probado rompiendo a propósito el enlace de JAL).
- nemu64-test pasó de no arrancar a 4175 de 4637 pruebas (90 %). Las fallas
  del CPU que encontró están arregladas: MULT con rt de 35 bits, LWR parcial,
  enlace de saltos en delay slot, MTC0 de 64 bits, Index.P, TagLo, PageMask y
  EntryLo en la TLB, Random con Wired > 31, funciones de COP0 reservadas
  como NOP, CFC0/CTC0/BC0, CACHE no alineado, latch de COP2, direccionamiento
  de 64 bits (xkuseg/xksseg/xkphys/xkseg), Status.RE; FPU: índices de
  registro con FR = 0, clasificación de NaN (estaba al revés), redondeo
  dirigido e Inexact, desbordamiento, flush con FS, rango de CVT.L,
  codificaciones inexistentes → FPE. Bus: SB/SH/SD fuera de la RDRAM y
  lecturas del bus PI de 16 bits.
- RSP provisional: quitarle HALT lo detiene en el acto con BROKE, para que
  nadie espere para siempre (Fase 2 lo reemplaza).
- Pruebas: 154 unitarias (antes 139) en host (ASan + UBSan) y en RPCS3
  (big-endian): todas pasan. Se actualizaron las 5 que codificaban
  suposiciones que nemu64-test desmintió.
- Rendimiento del intérprete en el host: 237 → 217 MIPS por la FPU exacta
  (docs/perf-log.md). nemu64-test completo: 0,75 s en el host.

Fallas que quedan en nemu64-test (fuera del intérprete): RSP 294, bus PI
del cartucho 81, cachés 46, SP (DMA y registros) 31, RDP y MI repeat 10.

Siguiente (Fase 1):
- [x] Correr `salsa64-cpu-tests` en la PS3 real: 154/154 pasan, **8,8 MIPS**
      (docs/perf-log.md).
- [x] Perfilar el intérprete en el PPE: 8,8 → 21,2 MIPS en dos rondas
      (entrada de arriba; la sospecha del FPSCR no aplicaba).
- [x] VI con contador de líneas e interrupción; framebuffer 2D en el RSX
      (primera pieza de `lib/rsxkit/`): ver la entrada de arriba.
- [ ] Bus PI: latch de escrituras al cartucho y DMA no alineado (81 pruebas
      de nemu64-test); FlashRAM (la SRAM ya está).
- [ ] Cachés del VR4300 (46 pruebas) y ciclos por instrucción (conjuntos
      "timing" y "cycle" de nemu64-test).
- [ ] CPUTest de krom ensamblado desde fuente con bootcode en ceros (traer
      solo los `.asm` por la API: el repo trae el IPL3 de Nintendo). Probaría
      el camino plano del arranque HLE.
- [x] Probar el camino plano con los juegos del usuario (fuera del repo):
      falta reproducir efectos del IPL3 de algunos CIC (6105). (Hecho:
      arranque del 6105 y censo, 2026-10-10.)

## 2026-10-10 — Fase 1 iniciada: intérprete del VR4300

Hecho (diseño en docs/cpu-vr4300.md):
- Intérprete del VR4300 en `src/cpu/`: ALU de 32/64 bits con
  desbordamiento, cargas/almacenamientos (incluidos LWL/LWR/LDL/LDR y sus
  stores), LL/SC, saltos con delay slot y branch-likely, HI/LO, MULT/DIV y
  variantes de 64 bits, traps; COP0 (Count/Compare, Status, Cause, EPC,
  ErrorEPC, excepciones, ERET, TLB de 32 entradas, modos usuario/supervisor);
  COP1 con modo FR, causas/banderas de FCR31, conversiones y comparaciones.
- Bus y mapa físico en `src/core/`: RDRAM de 4/8 MB con camino rápido desde
  KSEG0/KSEG1, SP DMEM/IMEM, ROM, PIF RAM. MI completo (interrupciones →
  IP2); PI y SI con DMA inmediato; VI, AI, SP, DP como registros. Planificador
  de eventos (Compare). Sin asignaciones en el bucle.
- Portable: mismo código para ppu-gcc (big-endian, accesos directos) y gcc
  nativo (arm64, con byte-swap).
- Pruebas: `tools/build.sh test-host` (139 pruebas, ASan + UBSan) y
  `tools/test-phase1.sh` (las mismas en RPCS3, big-endian). Todas pasan en
  ambos. Prueba de mutación: romper SRA, branch-likely o EPC las hace fallar.
- Micro-benchmark del intérprete (bucle mixto con FPU): 237 MIPS en el host
  (Mac Studio, Docker arm64); 35,6 MIPS en RPCS3 (no concluyente).
- Suites elegidas y licencias verificadas (docs/THIRD_PARTY.md):
  nemu64-test (MIT, IPL3 libre) como principal; CPUTest de krom (Unlicense)
  solo ensamblado desde fuente, porque sus ROMs traen el IPL3 de Nintendo.
  Dillonb/n64-tests descartada (sin licencia).

Siguiente (Fase 1): ver la entrada de arriba. Hechos en ella: arranque
HLE, ISViewer, contenedor de nemu64-test, direccionamiento de 64 bits y
bandera Inexact.

## 2026-10-10 — Fase 0: completa en RPCS3, falta la PS3 real

Hecho:
- Imagen `salsa64-toolchain:dev` (ver docs/toolchain.md): ps3toolchain (GCC 13.2
  PPU, 9.5 SPU) + PSL1GHT + cgcomp con Cg Toolkit vía qemu. Los 54 shaders de
  la rama `ps3` del port de SM64 compilan; 18/31 samples de PSL1GHT compilan
  (las fallas son de los samples).
- Programas de la Fase 0, compilados sin advertencias (`tools/build.sh`):
  - `tests/phase0/rsx-triangle`: triángulo + verificación de píxeles.
  - `tests/phase0/spu-compute`: 6 SPUs, DMA de doble búfer, verificación exacta.
  - `tests/phase0/jit-ppe`: 8 mecanismos de memoria, tolera caídas.
  - `tests/phase0/udplog`: 200 líneas numeradas por UDP.
- Logger TTY + UDP (`src/platform/ps3/log.c`) y receptor `tools/udplog.py`.

- RPCS3 0.0.43 + firmware 4.92 en la Mac (docs/rpcs3.md). `tools/test-phase0.sh`
  corre las cuatro pruebas: udplog, spu-compute y rsx-triangle pasan;
  jit-ppe completa su ciclo, pero RPCS3 no ejecuta código generado (no
  concluyente).

Falta para cerrar la Fase 0:
- [x] Correr los 4 programas en RPCS3 y automatizarlo.
- [x] PS3 real (2026-10-10, Slim CECH-3011A, HFW 4.91 + HEN; no admite CFW):
      udplog, spu-compute, rsx-triangle y salsa64-cpu-tests pasan; jit-ppe:
      solo `.text` es escribible y ejecutable (docs/jit-ppe.md). Números en
      docs/perf-log.md. El log también se escribe en /dev_hdd0/salsa64/<prog>.log.
- [x] Plan de la Fase 3: recompilador en el PPE con la caché en una región de
      `.text`. Falta medir el tamaño máximo de esa región (docs/jit-ppe.md).
- [ ] Antes: conectar la PS3 (está desconectada; revisar su estado). Datos a
      reunir: IP por cable, versión de firmware, apps de envío instaladas y si
      hay capturadora. Luego correr la Fase 0 y `salsa64-cpu-tests` (primer
      número de MIPS real).
- [ ] Opcional: correr Cell64 en la PS3 y guardar su `perf_trace.csv`
      (docs/referencia-cell64.md).

Nombre: **Salsa64** (decidido el 2026-10-10). Antes: "Retro64" (2026-10-10) y "Cell64" (provisional).
- Ya existe un emulador de N64 para PS3 llamado **Cell64**: v1.52 de
  "ZeBobbel", publicado en Brewology el 2026-09-27
  (store.brewology.com/ahomebrew.php?brewid=344), con una variante "HEN FIX by
  DropSonic DS". Sin código fuente publicado ni descripción técnica; el
  "sitio del desarrollador" es un canal de YouTube con dos videos de prueba.
  Por eso se renombró el proyecto. La carpeta sigue siendo `projects/cell64`.
- Referencia de rendimiento de ese emulador (videos de ZeBobbel, capturadora,
  build "40031911"): el overlay dice "INT" (intérprete; formato del overlay de
  Wii64/Not64). Super Mario 64 en el castillo: 46,9 VI/s (~78 %). Zelda OoT,
  pantalla de título: 26,4 VI/s, 9,1 FPS (~44 %). No se vio modo "DYN".
  En el video de la v1.50 (overlay nuevo, sin indicar INT/DYN): Zelda OoT en
  el menú de archivos a 50,0 VI/s y 50,0 FPS; probablemente versión PAL (50 Hz),
  o sea ~100 % en una escena ligera. En juego (bosque Kokiri): 41,3 VI/s y
  13,8 FPS (relación 3:1, coherente); ~83 % si es PAL, ~69 % si es NTSC.
  Meta para nuestro emulador: superar eso en la misma escena y región.
  Comparar contra esto nuestro intérprete (Fase 1) y el recompilador (Fase 3),
  con los mismos juegos en la misma PS3.

Notas:
- No usar `-Os` en el PPU: ICE de GCC 13.2 (docs/toolchain.md).
- En RPCS3, rsx-triangle necesita "Write Color Buffers" para su verificación.
- Archivos de Salsa64 en la PS3: `/dev_hdd0/salsa64/` (RPCS3 vacía `/dev_hdd0/tmp`).
- Juegos objetivo: tabla en docs/proyecto.md. Comparación con Cell64 en
  docs/referencia-cell64.md (no usa SPEs; todo en un hilo del PPE).
- Ideas sin decidir: versión rápida del intérprete (decodificar una vez,
  como el "FASTINT" de Cell64) y, como investigación lejana, emular la CPU en
  un SPE solo si el PPE no alcanza.
