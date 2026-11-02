# Efectos de framebuffer

Estado al 2026-10-12. Código: `src/rdp/targets.c` (política, portable, con
pruebas), `src/rdp/rdp.c` (PS3), `src/host/raster.c` (el mismo modelo en el
host), `lib/rsxkit/src/target.c` (lo genérico del RSX), `src/rsp/gfx/tmem.c`
(procedencia de las texturas y cuáles no se decodifican). Confirmado en la
PS3 real: fondo y Link de la pausa de Zelda OoT, la cámara del túnel de
Mario Kart 64, gfxtest 35/35.

## Problema

Los juegos dibujan en destinos del RSX a resolución interna (3× a 720p con
la escala automática; cada destino con su propia escala, ver «Escala por
destino») y nada vuelve a la RDRAM. Cuando un juego usa como textura una imagen que él
mismo dibujó, o el CPU la lee y la modifica, en la RDRAM hay basura o la
imagen vieja: el fondo de la pausa de Zelda salía mal y faltaba Link en la
pantalla de equipo.

## Qué hacen los juegos (medido en el host con `--trace-fb`)

**Zelda OoT, al pausar** (tarea 5864 de `docs/pads/zelda-pausa-nueva.pad`,
campo 16107), en una sola tarea:

1. Dibuja el cuadro en el framebuffer (`3da800`, 320 de ancho).
2. Copia ese framebuffer, en 40 franjas de 320 × 6 (`LOADTILE` + `TEXRECT`),
   a una color image de 320 en la dirección del z-buffer (`12c700`,
   `fbufSave`).
3. Vuelve al framebuffer y lo copia **sobre sí mismo** en otras 40 franjas
   (`G_RM_PASS`) y después un rectángulo con `G_RM_VISCVG` (IM_RD |
   FORCE_BL, blender IN·0 + BL·A_MEM): el RDP escribe en cada píxel su
   cobertura, para el filtro de bordes del CPU.
4. Entre esa tarea y la siguiente (64 campos después), el CPU lee el
   framebuffer 1,2 millones de veces (16 por píxel: el filtro de
   antialiasing/divot sobre `fbufSave` guiado por la cobertura) y escribe el
   resultado en `fbufSave`.
5. Cada cuadro de la pausa restaura el fondo desde `fbufSave` (40 franjas)
   y dibuja el menú encima. Link se dibuja en **cada tarea** de la pausa
   en una color image de 64 × 112 en los primeros bytes del framebuffer
   que toca (`3da800/64` o `3b5000/64`, con su z-buffer 14 KB después:
   `3de000/64`, `3b8800/64`), aunque nadie lo lea; al abrir la pausa y al
   cambiar de equipo el RDP lo copia en 4 rectángulos a un búfer del heap
   (`fbufSave` de Link, `279f00/64` o `289600/64` según la sala), deja su
   cobertura con VISCVG y la copia a otro búfer **de 8 bits de intensidad**
   (`cvgSave`, `35d760/64`), y el CPU filtra ese búfer leyendo los dos. La
   pantalla de equipo lee a Link desde el búfer del heap, como textura,
   durante todo el tiempo que esté abierta. Dos color images imposibles
   (anchos 1495 y 3770) aparecen en las tareas de apertura: un comando mal
   interpretado por la HLE; se ignoran. La cobertura del fondo también es
   de 8 bits y vive en la dirección del framebuffer (`3da800/320 siz 1`).

**Mario Kart 64**: la «pantalla gigante» es la cámara sobre la entrada
del túnel de Luigi Raceway. El volcado de la PS3 frente al túnel tiene una
sola color image (el framebuffer) y ninguna textura con origen en un
destino: el CPU lee el framebuffer y lo reduce a la textura de la pantalla
(en 50 s de carrera lejos del túnel, con `mk64-luigi.pad`, no lo hace). De
paso: todas las texturas del volcado salían sin origen porque libultra
manda ancho 1 en `SETTIMG` para `LOADBLOCK`; ahora un bloque son texels
seguidos y el paso lo da la tile que lee.

**Conker**: nada en 3 minutos de intro (los efectos están dentro del
juego: pausa, sombra).

**Perfect Dark**, intro (la azotea de dataDyne, desde el campo ~1800 en la
PS3 y ~2000 en el host, sin tocar el control): un desenfoque de movimiento.
Después de dibujar la escena, el juego copia el cuadro anterior (el otro
framebuffer: `6211c0/320` mientras dibuja en `665dc0/320`, y al revés) sobre
el nuevo en 180 franjas por tarea: `SETTIMG` de 320 en la fila, `LOADBLOCK`
de una fila (320 texels RGBA16), una tile de 513 × 9 con línea de 1280
bytes y un `TEXRECT` de 320 × 1 con filtro puntual y alfa 0x17 (9 %). Ver
«Texturas desde un destino sin decodificar».

**Majora's Mask** (2026-10-12, estados de la consola cargados en el host):

- *Desenfoque de movimiento* (el baile del Espantapájaros, el recuerdo de
  Zelda con la Canción del Tiempo): un acumulador de 320 × 240 en
  `3a92c0`. Cada tarea dibuja el cuadro en el framebuffer, encima el
  acumulador en 48 franjas con alfa 0,706 (lo viejo pesa 70 %) y copia el
  resultado al acumulador en 48 franjas: la tarea siguiente lo vuelve a
  leer. Nadie más lo lee; el CPU, nunca.
- *Campanadas del fin del día*: copia el framebuffer en 40 franjas (modo
  COPY) a `784600/320` y lo redibuja desplazado en 60 franjas, con alfa
  constante 1 y la comparación por cobertura. La **pausa** usa esa misma
  dirección y ancho como imagen de 8 bits de intensidad (la cobertura de
  su filtro, como Zelda OoT).
- *Pausa*: como Zelda OoT, `383ac0/320` (16 bits) y la cobertura en
  `784600/320` de 8 bits, las dos copiadas a la RDRAM al abrirla.

## Diseño

Tres mecanismos, decididos con la tabla de destinos (`targets.c`):

**Procedencia de las texturas.** La HLE anota las últimas cargas a la TMEM
(`gfx_load`: tramo escrito, dirección en la RDRAM, filas) y, al resolver una
tile, deja en `gfx_tex.src_*` la dirección del texel (0, 0) de la textura en
la imagen de origen, su paso en texels y su formato. Va aparte del
contenido: dos tiles con el mismo contenido comparten textura pero no
origen. Cuesta un registro de 32 bytes por carga. Con `LOADTILE` el paso es
el ancho de `SETTIMG`; con `LOADBLOCK` (texels seguidos) es la línea de la
tile, porque la fila 1 de la textura está una línea más adelante en la
TMEM y, por lo tanto, en la RDRAM. Pero si el dibujo muestrea **una sola
fila**, el paso no cambia ningún texel que lea y vale el ancho que declaró
`SETTIMG` (con el que el RDP ubica la fila de la carga), si lo declaró
(libultra manda 1): las franjas de Perfect Dark tienen línea de 1280 bytes
(paso 640) sobre un framebuffer de 320 y no se reconocían. Para saber que
muestrea una fila, el recorte del `TEXRECT` con filtro puntual es exacto:
`floor(u) < hi` da `ceil(hi)` texels (antes `floor(hi) + 1`: las franjas
salían de 321 × 2 y ahora de 320 × 1).

**Render a textura.** Si el origen cae en un destino vivo
(`rdp_targets_texture`): dibujado hace `RDP_TARGETS_FRESH` (3) tareas o
menos y no copiado a la RDRAM, la textura se lee de su superficie del RSX
(`rsxkit_target_texture`: filas enteras desde `y0`, la columna va en `xf`),
a la escala de ese destino. Si es el destino ligado (Zelda lee lo que está dibujando), se copia
antes la superficie a un scratch (`rsxkit_target_copy`, una vez por tarea)
y se lee de ahí. Antes del primer dibujo que lee una superficie dibujada en
el mismo tramo, `rsxkit_texture_sync` (espera al RSX e invalida su caché de
texturas). Si el destino es viejo o se copió a la RDRAM, vale la textura
decodificada por la HLE desde la RDRAM, como antes; y si la HLE no la
decodificó por venir de un destino (abajo), la decodifica el consumidor.

**Copia RSX → RDRAM.** Solo para las color images de 16 bits del ancho del
VI que no son ninguno de los framebuffers recientes (`rdp_readback_wanted`
con el anillo de orígenes del VI): un cuadro guardado aparte, que el CPU
va a leer. Lo decide `done` en el hilo de emulación (recorre las cabeceras
de los registros de la tarea, sin tabla), despierta al hilo de presentación
y espera a que la copia esté en la RDRAM (`readback_done`), para que
cuando llegue la interrupción del DP la RDRAM esté al día; si tarda más
de 200 ms, sigue y lo cuenta (`tarde`). El hilo de presentación, al
traducir la tarea, manda al RSX reducir el destino a la resolución de la
N64 (`rsxkit_target_read`: un quad con el shader del blit sobre una
superficie R5G6B5 **en memoria principal mapeada**, filtro lineal), espera
la barrera y convierte a RGBA5551 escribiendo en la RDRAM
(`rsxkit_pixels_565_to_5551`, 4 píxeles por palabra). Desde entonces el
destino es «del CPU» (`cpu_owned`) hasta que el RDP lo vuelva a dibujar.
Qué copias espera la emulación (`readbacks_wanted`, hilo de emulación,
recorriendo los registros de la tarea): las imágenes auxiliares de 16
bits o de 8 de intensidad (las que no son un framebuffer reciente del VI,
de cualquier ancho razonable) cuando **acaban de aparecer** (no se pidieron
en la tarea anterior: el fondo de la pausa de Zelda y los búferes de Link,
una vez por pausa o por cambio de equipo), y las **chicas (≤ 64 KB) que
ningún dibujo de la tarea lee como textura**, pero solo las dos primeras
tareas seguidas: su único lector posible es el CPU, y una imagen que se
redibuja en cada tarea (Link en la pausa) la recibe con una tarea de
retraso sin que se note, mientras que esperarla costaba ~22 ms por tarea.
Las demás van sin esperar: las chicas que lee el RDP (iconos), y las
grandes repetidas (la intro de Conker dibuja cada tarea una color image de
292 que no se presenta y el RDP no lee: 150 copias esperadas en 5 s, 16–18
ms cada una, frenaban), que se copian en un búfer grande y el CPU recibe
el cuadro anterior. Una copia sin esperar **no pasa la imagen al CPU**
(`rdp_targets_readback(…, cpu_owns = false)`): el RDP la sigue leyendo de
su superficie mientras esté viva. El acumulador del desenfoque de
movimiento de Majora's Mask se lee en la tarea siguiente a la que lo
dibujó; leído de la copia de la RDRAM llegaba con una o dos tareas de
retraso, a 1× y cuantizado a 16 bits, y como cada cuadro mezcla 70 % del
anterior, el error se acumulaba (en el host, sin retraso, la estela salía
hasta 17 niveles más oscura; en la PS3, además, con el tramado del RSX:
un tablero de ajedrez).

**Sin tramado en las copias.** `rsxkit_target_read` escribe la superficie
de 16 bits (R5G6B5) con el *dithering* del RSX apagado: si está encendido,
suma un patrón fijo por posición antes de redondear, y una imagen que
vuelve a pasar por la copia cuadro tras cuadro lo acumula.

**La misma memoria con otro tamaño de texel.** Un destino es (dirección,
ancho) y su tamaño de texel. Si llega una color image con la misma
dirección y ancho pero otro tamaño de texel y la ranura no se dibujó en
`RDP_TARGETS_FRESH` tareas, es otra imagen y la ranura se rehace
(`rdp_targets_get`): Majora's Mask usa `784600/320` como cobertura de 8
bits en la pausa y, mucho después, como copia de 16 bits del cuadro en las
campanadas; con la ranura de 8 bits, las texturas de 16 no la encontraban
(se leían de la RDRAM) y la copia a la RDRAM escribía bytes de intensidad
en una imagen de 16 bits: un patrón fino de píxeles intercalados. Si la
ranura está viva se comparte, como antes: Zelda OoT pone su framebuffer
de 16 bits como color image de 8 en la misma tarea para dejar la
cobertura. Escena V de gfxtest.

**Espejo del framebuffer.** Lo que el RDP dibuja en un framebuffer se
reduce en el RSX a un búfer y, pasada su barrera, el PPE lo escribe en la
RDRAM (sin esperas), **una sola vez por dibujo**: al terminar la tarea que
lo dibujó si el VI ya lo mostró alguna vez (16 bits, del ancho del VI) y,
si no, al presentarlo por primera vez (solo las filas que muestra el VI).
Mario Kart fabrica con el CPU la textura de la cámara del túnel de Luigi
Raceway leyendo el framebuffer: sin el espejo la pantalla salía negra. En
la PS3 cuesta 0,0 ms medidos por campo en presentación (GoldenEye en Dam
sigue a 45,8 campos/s, su rango). El espejo no pisa durante 32 tareas la
memoria de una copia reciente que se solape con el framebuffer (Link en sus
primeros 14 KB; la cobertura plena de VISCVG, que el filtro del CPU lee
durante ~1 s): sin eso, el CPU copiaba la esquina de la pantalla de pausa
en vez de a Link.

Antes (bf836f2) se copiaba el framebuffer presentado **en cada campo**, y
eso pisa lo que el CPU o un DMA escriban en esa memoria después de que el
RDP la dibujó: Super Mario 64, al pasar del logo a la cara de Mario (campo
~343), dibuja un framebuffer (tarea 95), carga por DMA ~200 KB de datos de
la cara encima de dos de sus framebuffers (`390000`–`3c2780`) mientras el
VI todavía muestra uno, y lo presenta un campo más antes de apagar el VI;
la copia pisaba esos datos y el juego no volvía a mandar tareas gráficas
(en el host se quedaba en 96 tareas con `SALSA64_FBFX=2`). Copiarlo al
presentarlo «una vez» tampoco basta (ese framebuffer se presenta por
primera vez *después* del DMA): por eso la copia sigue al dibujo, como en
la N64. En el host la copia es síncrona; en la PS3 llega a la RDRAM en una
traducción o presentación posterior (uno o dos campos), así que un DMA
sobre un framebuffer en el mismo campo en que el RDP lo terminó todavía
podría quedar debajo de la copia (en SM64 pasan ~4 campos entre la tarea
95 y el DMA).

Ese caso existe: **Star Fox 64** limpia la pantalla con una tarea (un
FILLRECT) y, antes de que el RSP termine, carga por DMA 113 KB de datos
MIO0 encima de ese mismo framebuffer (`38f800`, en trozos de 256 bytes). En
la consola el RDP acaba en una fracción de ms y el DMA le pasa por encima;
aquí la copia llegaba después (en el host, al terminar la tarea; en la PS3,
ms más tarde) y pisaba la cabecera del MIO0: el juego descomprimía basura y
todos sus hilos quedaban esperando (letras 5 s y negro). Por eso cada
escritura a la RDRAM que no pasa por el CPU (`salsa64_bus_rdram_written`:
DMA del PI, del SP, del SI, la HLE de audio) se anota en un anillo
(`rdp_dma_ring`, targets.h) con la tarea gráfica en vuelo, y **ninguna copia
de la tarea T (espejo del framebuffer, copia esperada, espejo chico) escribe
un tramo que un DMA tocó desde que T empezó**: el dato del DMA es más nuevo.
Un DMA anterior a la tarea sí se pisa, como en la N64.

**Las escrituras del CPU (2026-10-18, rc7a).** Lo mismo pasa con el CPU, y
en la PS3 con más tiempo de por medio: Indiana Jones arma sus display lists
en 0x71D358, dentro del framebuffer de 640 × 480 de la pantalla del
Expansion Pak (0x6B3000–0x749000), y Killer Instinct Gold mete objetos de
su audio donde hubo un cuadro. El espejo de ese último cuadro (negro: 0x0001
en RGBA5551) llegaba campos después y dejaba la lista llena de 0x00010001
(«tarea mala» en el log) o un puntero a función en 0x00010001 (el CPU
saltaba ahí: AdEL con `pc 0x00010001`, que es lo que tienen todos los
volcados de cuelgue de Killer Instinct). En el host la copia es síncrona y
no pasa. **Guarda** (`rdp_guard`, targets.h): al terminar cada tarea el
hilo de emulación toma una huella de cada línea de 128 bytes de las filas
que sus dibujos pueden tocar (tijeras) en cada color image de 8 o 16 bits;
una copia tardía (espejo del framebuffer, copias, espejos chicos, VISCVG)
solo escribe las líneas cuyo contenido sigue siendo esa huella o la que dejó
la última copia; las demás las cambió algo que no es el RDP y se quedan como
están. Cuesta leer las filas una vez por tarea (~150 KB en 320 × 240) y
512 KB de tablas. El log de cada 5 s dice «guarda contra el CPU: N copias
tardías con M líneas sin escribir» y las primeras 12 copias recortadas, con
lo que había en la primera línea. `sin-guarda-cpu` en la carpeta de datos la
apaga (para comparar) y `vigilante.cfg` («6b3000 749000», hexadecimal)
escribe en el log cada copia, VISCVG o DMA que cae en ese rango y si dejó
0x00010001.

**Lo que otra color image dibujó después** (`rdp_claim_ring`, targets.h;
FIFA 99, #69). La huella de una línea la renueva cualquier tarea cuyos
dibujos puedan tocarla, también con otra color image, y entonces ya incluye
lo que el CPU escribió antes de esa tarea. FIFA 99 pasa de los framebuffers
de 512 × 384 del menú (0x157640 y 0x1b76c0) a los de 480 × 360 del partido
en la misma memoria: suelta los viejos y pide dos bloques al heap; la
cabecera del segundo (0x1abc70: «BM», banderas, tamaño, siguiente y
anterior) cae justo detrás del primer framebuffer nuevo, que acaba en
0x1abc40. La primera tarea del partido renueva la huella de la línea
0x1abc00 y el espejo del último cuadro del menú, que llega después, la
pasaba: dejaba ahí su negro (0x00010001). Nueve minutos después, al darle a
«Continue» tras el partido, el juego suelta ese bloque, lee el anterior en
0x00010001 y falla (AdEL en 0x10003, pc 0x8005B344): pantalla negra. Ahora
el hilo de emulación anota además, por tarea, qué filas de qué color image
(dirección y bytes por fila, de 8, 16 o 32 bits) pudieron tocar sus dibujos,
en un anillo de 64 entradas, y el espejo del framebuffer de la tarea T no
escribe las líneas que una color image distinta dibujó en una tarea
posterior a T (lo que la consola tendría encima). La misma color image vuelta
a dibujar no cuenta (eso lo cubre la guarda), ni otra de la misma tarea; si
el anillo ya perdió entradas posteriores a T, se copia como antes. Solo el
espejo del framebuffer (las copias y los espejos chicos siguen igual). El log
de cada 5 s suma «N espejos con M líneas sin escribir porque otra color
image las dibujó después» y las primeras 12 dicen el tramo;
`espejo-pisa-otras` en la carpeta de datos vuelve a como antes.

**Rellenos de imagen alta antes de que existan los destinos.** Star Fox 64
limpia sus tres framebuffers con un solo FILLRECT de 320 × 720 en la primera
tarea gráfica (campo 3); los destinos nacen en las tres siguientes. El
reparto a los destinos existentes (`rdp_targets_inside`) no los alcanza, y
al cebarse con la RDRAM —donde el RDP de la HLE no escribió el relleno y
quedan los datos del DMA de arranque— el borde de 8 px que el juego nunca
vuelve a tocar salía con ruido durante todo el logo. Cada relleno se anota
(`rdp_targets_tall_note`) y un destino que nace después y cae dentro lo recibe
una vez (`rdp_targets_tall_pending`), salvo que hayan pasado 32 tareas o que
un DMA haya tocado su memoria desde el relleno (el anillo de arriba): el dato
del DMA es más nuevo.

Las imágenes chicas (iconos, pantallas) no se esperan: al terminar la tarea
que las dibujó se encola una **copia espejo** (solo las filas que llegaron
las tijeras, hasta 64 KB, cuatro en vuelo) y se convierte a la RDRAM en
una traducción posterior, cuando el RSX pasó su barrera, sin marcar el
destino como del CPU. Así, mientras son recientes se leen del RSX a 3× y,
cuando un dibujo las lee ya viejas (Link en la pantalla de equipo de Zelda
se dibuja en `289600/64` al abrir la pausa y se lee 395 tareas después), la
RDRAM tiene lo que el RDP dejó.

**Solo lo que el RDP pudo escribir.** Toda copia a la RDRAM (esperada, sin
esperar, espejo chico, espejo del framebuffer) escribe la unión de la caja
de los vértices de cada dibujo recortada por sus tijeras (`struct rdp_box`
en targets.h), no filas enteras: el alto de un destino es una conjetura y
pasarse pisaría datos del juego, y fuera de las tijeras también puede haber
datos. Super Smash Bros. guarda punteros y código en los márgenes de su
z-buffer (las 10 primeras filas y 10 columnas a cada lado, fuera de las
tijeras 10..310 × 10..230); en el KO dibuja una máscara de profundidad en
ese z-buffer como color image, y la copia de filas enteras, al pasar por
RGBA5551 con alfa 1, les ponía a 1 el bit bajo de cada media palabra: el
juego se colgaba ~4 s después. El espejo del framebuffer usa la caja
acumulada desde el último espejo (`rdp_target.box`).

**El alfa en las copias (2026-10-29).** Los destinos de render a textura
con alfa (`salsa64_rdp_target_alpha`: los de 16 bits que no son del ancho
del VI) se copian a la RDRAM desde una lectura ARGB8888 del RSX
(`rsxkit_target_read` con `RSXKIT_ARGB8888`) y su alfa va al bit de
RGBA5551, como lo deja el RDP; antes la copia pasaba por RGB565 y el bit
salía siempre 1. Importa cuando la textura se lee de la RDRAM porque el
destino ya es viejo (`RDP_TARGETS_FRESH`): LEGO Racers dibuja cada auto
lejano en una imagen de 32 × 32 borrada con 0x0000 (en la RDRAM de ares el
fondo de esas imágenes es 0x0000 y el auto lleva alfa 1) y la usa con
prueba de alfa; leída de la copia con alfa 1, el auto salía dentro de un
cuadro (#34). Un framebuffer que nació con alfa (antes del primer campo no
se sabe el ancho del VI) se sigue copiando como antes. `copia-sin-alfa`
vuelve a la copia en RGB565. El host (`src/host/raster.c`) lleva el mismo
alfa en el byte alto de cada imagen (`SALSA64_SIN_ALFA_DESTINOS`,
`SALSA64_COPIA_SIN_ALFA`).

**El bit de alfa es la cobertura (2026-10-31, #62).** En una color image de
16 bits el RDP no guarda en el bit de alfa de RGBA5551 el alfa de la mezcla:
guarda la cobertura del píxel (3 bits: el alto ahí, los otros dos en los
bits ocultos de la RDRAM). En FILL escribe el color de relleno tal cual (su
bit) y en COPY los 16 bits del texel. En los ciclos 1 y 2, con el píxel
cubierto entero (el RSX no tiene cobertura parcial; `mem` es la cobertura
que lee de la memoria, 7 sin IM_RD), según CVG_DST:

| CVG_DST | cobertura escrita | bit (`gfx_draw.cvg_bit`) |
|---|---|---|
| CLAMP | cvg − 1 sin mezcla, cvg + mem con ella, hasta 7 | 1 (`GFX_CVG_ONE`) |
| WRAP | (cvg + mem) & 7 = mem | el de la memoria con IM_RD (`GFX_CVG_KEEP`); 1 sin IM_RD |
| FULL («ZAP») | 7 | 1 |
| SAVE | mem | como WRAP |

Con CVG_X_ALPHA la cobertura es cvg·α (α >> 5 con el píxel entero) y en
CLAMP el bit sale del alfa: sin FORCE_BL, del alfa del fragmento
(`GFX_CVG_ALPHA`: en 1 desde α = 160, o 128 sobre memoria en 0; el RSX corta
en 128); con FORCE_BL e IM_RD, 1 si la memoria ya lo tenía o desde α = 128
(`GFX_CVG_MAX`); con FORCE_BL sin IM_RD, siempre 1. Lo calcula la HLE
(`src/rsp/gfx/rdp.c`, `cvg_bit`); `src/rdp/rdp.c` lo pasa a
`rsxkit_draw.dst_alpha` (`RSXKIT_DSTA_ONE`, `_KEEP`, `_FRAG`, `_MAX`) y el
host (`src/host/raster.c`) hace lo mismo.

Hasta la v0.2.15 los dibujos dejaban en el RSX el alfa del fragmento:
`RSXKIT_DSTA_ONE` ponía la mezcla del canal alfa en ONE / ZERO, que copia el
del fragmento en vez de dar 1, y el host suponía 1. La mezcla no puede dar
una constante: el alfa sale de As·fs + Ad·fd y todos los factores
multiplican (con `GCM_CONSTANT_ALPHA` da As·Ac; el primer intento con ese
factor dejó los paneles en blanco en la PS3 aunque la HLE pedía alfa 1 en
los 77 dibujos). rsxkit hace `RSXKIT_DSTA_ONE` en dos pasadas cuando el alfa
del fragmento no es 1 en todo el dibujo: primero una solo en el canal alfa,
con el mismo programa, los mismos vértices y la misma prueba de profundidad
(sin escribirla), con el alfa llevado a 1 o más (que el RSX satura al
escribir): con prueba de alfa (pasa α ≥ r) multiplicado por 1/r y probado
contra 1, para que descarte los mismos fragmentos; sin ella, más 1 en el
término constante. Después, el dibujo con `RSXKIT_DSTA_KEEP`. El log de cada
5 s cuenta los dibujos con la pasada, los de una sola (el alfa ya era 1) y
los que no se pudieron, y al arrancar «rsxkit: autoprueba dst_alpha …» dice
lo que deja cada modo en el RSX. Con las copias con alfa
de la v0.2.15 eso llegó a la RDRAM: Dr. Mario 64 dibuja una vez el fondo de
la partida en una imagen de 328 × 240 (`214e00`), con los paneles
translúcidos (marcador, frasco, el recuadro de Dr. Mario, VIRUS LEVEL, SPEED,
VIRUSES) en XLU (ZAP, FORCE_BL, IM_RD) con alfa 0,375 o el de una máscara
I4, y en cada cuadro la pega en COPY con prueba de alfa leyéndola de la
RDRAM. Con el bit en 0 los paneles se descartaban: salían en blanco y, en el
frasco, con la estela de la píldora que cae. En la RDRAM de ares esa imagen
tiene el bit en 1 en las 76 800 posiciones dibujadas; el host da lo mismo
(antes, 19 789 en 0). `alfa-fragmento` (`SALSA64_ALFA_FRAGMENTO=1` en el host)
vuelve al alfa del fragmento.

El host tampoco copiaba a la RDRAM una imagen de 16 bits de otro ancho que
el VI que no cabe en un espejo (64 KB), y la PS3 sí (`readbacks_wanted`):
ahora la copia como la PS3 (el fondo de Dr. Mario salía de la RDRAM sin
dibujar). `salsa64-run --ares` compara además el bit de alfa de las imágenes
que lo llevan con el de la consola (`SALSA64_ALFA_EN_TODAS=1`: en todas las
de 16 bits).

**Lo que rompía a Link en la PS3** (y que el host no reproducía porque
copiaba todos los destinos de cada tarea sin llamar a
`rdp_targets_present`; ya lo llama): `rsxkit` admitía 8 superficies y la
pausa tiene 9 color images vivas más el scratch, así que la novena no se
creaba y lo dibujado en ella se perdía (el log cuenta «destinos sin
superficie» y «copias perdidas»; ahora 16); la copia pedida se buscaba
solo por dirección y `3da800` es el framebuffer y el búfer de Link a la
vez; con más de dos copias esperadas por tarea la tercera pisaba el búfer
de la primera; las color images imposibles reciclaban ranuras; la
cobertura de 8 bits no se copiaba; y con dos profundidades los tres
z-buffers de la pausa se destruían y creaban a cada tarea (ahora cuatro).
Copias de 8 bits: solo de intensidad (`G_IM_FMT_I`), con el rojo del
RGB565 como valor; una CI8 no se reconstruye desde el RSX.

**Cebado.** La N64 dibuja siempre encima de lo que hay en la RDRAM. Una
superficie nueva, o sin dibujar desde hace más de `RDP_TARGETS_FRESH`
tareas, se pinta primero con lo que hay en la RDRAM (`rsxkit_target_fill`
desde un búfer mapeado), para que lo que el CPU dibujó ahí (iconos de
Zelda) no se pierda cuando el RDP dibuja encima.

**VISCVG.** No se dibuja: se deja «cobertura plena» (0xFF) en la RDRAM del
rectángulo; el filtro del CPU salta los píxeles cubiertos del todo y el
fondo queda nítido (en la N64 solo se filtran los bordes).

## Texturas desde un destino sin decodificar (2026-10-12)

Antes, toda textura se decodificaba en el SPE desde la TMEM aunque el
consumidor fuera a leerla de un destino del RSX. En el desenfoque de
Perfect Dark eso eran 180 franjas nuevas por tarea, 116 000 texels, que
además llenaban la caché de texturas y desalojaban ~34 texturas por tarea;
y como la procedencia no se reconocía (paso 640 contra un destino de 320,
arriba), el «fantasma» salía de la copia a 1× del espejo del framebuffer:
de baja resolución a 3×.

**Qué no decodifica la HLE.** La HLE lleva su propia cuenta de las color
images de 16 bits que dibujó (`gfx_state.fbimg`, 8 entradas, como las
ranuras de `targets.c`: misma dirección y ancho, alto conjeturado de 3/4 del
ancho o hasta donde llegan las tijeras, ancho de 1 a 640, la tarea en que
se dibujó por última vez; fuera del tramo de los estados guardados, se
olvida al cargar uno o con un reset). Si el trabajo trae `GFX_JOB_FBTEX`
(lo pone el consumidor cuando lee de sus destinos: `fbfx.cfg` ≥ 1), una
textura RGBA16 sin paleta ni RGB·A cuyo origen cae en una de esas imágenes,
dibujada hace `GFX_FB_FRESH` (= `RDP_TARGETS_FRESH`, 3) tareas o menos,
buscada como la busca `rdp_targets_locate` (paso = ancho) y que no repite
en s salvo que mida lo que la imagen, sale como `GFX_TEX_TARGET`: con su
tamaño, su `pitch` y su origen, sin texels, sin hash y sin pasar por la
caché (`res->tex_target` las cuenta).

**El consumidor.** La lee de su destino como cualquier textura con origen
en un destino (`texture_from_target`). Lo que él sabe y la HLE no
—que copió ese destino a la RDRAM para el CPU (`cpu_owned`), que no pudo
crear su superficie o la recicló, que el render a textura está apagado—
lo resuelve decodificándola él desde la RDRAM con
`rdp_decode_rgba16` (`targets.c`, portable): en la PS3, en el PPE, en el
arena de la tarea detrás de lo que escribió la HLE (el arena casi no se
usa con la caché); en el host, en un búfer del rasterizador. Da lo mismo
que habría dado la HLE con la RDRAM de ese momento. Si no hay lugar en el
arena, el dibujo se salta y se cuenta. `salsa64_rdp_dump` las escribe
decodificadas para que la repetición en el host las tenga.

Así el caso viejo o «del CPU» no se rompe: la predicción de la HLE solo
decide dónde se decodifica, nunca qué se ve. Medido en el host, el
respaldo se usa poco: en la intro de Perfect Dark, 2 tareas al empezar el
desenfoque (360 franjas: el consumidor copió `665dc0` a la RDRAM porque el
VI todavía no lo había mostrado, así que lo lee de ahí) de 59 620; en la
pausa de Zelda (`zelda-oot.pad` con la partida del autor, campos hasta el
19400), 360 de 498: las 40 franjas que restauran el fondo desde
`fbufSave` en las tres tareas que siguen a cada copia para el CPU (tres
aperturas de la pausa; la HLE lo vio dibujar, el consumidor lo tiene como
del CPU), 1920 texels cada una; y ninguna en
gfxtest (1 textura por tarea, siempre de su destino; en RPCS3 con el
frontend, 45 del RSX y 0 en el PPE). En las escenas medidas de Banjo, SM64,
Mario Kart, GoldenEye y Conker no hay texturas desde un destino.

**Imágenes de 8 bits (2026-10-13, sombras de Conker).** Conker dibuja la
silueta de cada personaje en una color image de 64 × 64 de 8 bits
(declarada CI), la desenfoca con dos texrects que la releen como I8 y la
usa en el piso como alfa de la sombra (α = shadeα · (1 − T0a)). En una
imagen de 8 bits el RDP escribe el rojo, sea cual sea su formato. La HLE
lleva también esas imágenes en `fbimg` (con `bpp`) y una I8 sin paleta que
cae en una de ellas sale como `GFX_TEX_TARGET` con `src_siz` 1; el
consumidor la lee de su destino con el rojo en los cuatro canales
(`rsxkit_texture.remap` = `RSXKIT_REMAP_RRRR`) o, si no puede, la decodifica
de la RDRAM con `rdp_decode_i8`. Las imágenes de 8 bits de cualquier
formato vuelven a la RDRAM como las de intensidad (antes solo con
`G_IM_FMT_I`; la CI8 de Conker nunca volvía). Antes, la HLE decodificaba
esas I8 de la RDRAM al procesar la lista, antes de que el RSX dibujara la
silueta: el desenfoque copiaba la máscara vieja encima y la sombra salía
como un cuadro oscuro (o nada, según lo que hubiera en la RDRAM). Y como
la misma imagen sirve para la sombra de cada personaje de la tarea, la
copia del destino que se está dibujando (`RDP_TEXSRC_SELF`, el scratch) ya
no es una por tarea: se rehace si desde la copia se dibujó donde lee la
textura (`scratch_dirty`); las franjas de Zelda, que leen filas que aún no
reescribieron, siguen con una. Medido contra la consola con `salsa64-run
--ares --pasadas 2` en ocho volcados de ares: de 20,8–25,8 dB en 4 × 4 a
23,5–30,4, igual o mejor que con la máscara que dejó la consola en la
RDRAM. Justo después de la imagen (64 filas) Conker guarda el viewport del
cuadro: las copias a la RDRAM van acotadas a lo que dibujaron los
vértices, no a las tijeras (65). En RPCS3 la repetición del cuadro sale
con el piso negro cuando hay dos copias del scratch en la misma tarea (con
una sola, bien; el rasterizador del host, bien): falta verlo en la PS3.

**Costo en el SPE 0.** Texto +2,1 KB y la tabla 128 bytes. Para no
comerse la pila, los fondos de S2DEX se compilan con `-Os` (−0,9 KB de
código y 112 bytes menos en la cadena más honda): holgura de la pila
2,6 KB (`src/spu/gfx/main.c`; el build falla por debajo de 1,5 KB).

**Determinismo de los estados en el host.** Al cargar un estado los
destinos se sueltan y la primera tarea lee el cuadro anterior de la RDRAM;
de corrido, de la imagen del rasterizador. Con imágenes de 8 bits por canal
eso daba otra RDRAM (4699 bytes dos campos después de cargar a mitad del
desenfoque de Perfect Dark). Desde que los destinos de 16 bits del host
guardan 5 bits por canal (rama de Majora's Mask), las dos lecturas dan lo
mismo: guardar en el campo 2100 y cargar da la misma RDRAM que de corrido
dos campos después y la misma RDRAM e imagen cien campos después (main sin
esta rama: 83 bytes distintos en el campo 2200). Diez campos después de
cargar difieren los dos, igual con o sin esta rama, en 147 274 bytes: el
espejo del framebuffer, que tras cargar se copia al presentarlo y no al
terminar la tarea.

## Escala por destino

La escala interna (1×–5×, `video.cfg` y «Escala interna» del menú) es un
objetivo de 240 · g líneas, y cada destino tiene su propia escala
(`rdp_target_scale`, guardada en `surf_scale`): con la objetivo 3×, el
framebuffer de 320 × 240 va a 3×, Link (64 × 112) a 5× y un framebuffer de
640 × 480 a 1×. Los efectos de framebuffer nacieron con una escala global;
al juntarse, todo lo que pasa de coordenadas lógicas a píxeles de una
superficie usa la escala de **esa** superficie (`rdp_surface_rect`,
`rdp_target_tex_map` en `targets.c`, con pruebas):

- **Copias RSX → RDRAM** (esperadas, grandes sin esperar, espejos chicos y
  el espejo del framebuffer, una vez por dibujo, al terminar la tarea o al
  presentarlo): la región leída es `lw · s × filas · s` con la `s` del
  destino copiado, y el RSX la reduce a `lw × filas`. Con la escala objetivo en su lugar, la copia de Link a 5× leía
  solo la esquina de la superficie, y la de un destino a 1× se salía de
  ella (rsxkit la rechazaba).
- **Render a textura**: se ligan las filas `y0 · s … (y0 + alto) · s` del
  destino leído, con su `s`; u y v van normalizadas, así que el destino en
  que se dibuja puede tener otra escala sin cambiar nada (con la objetivo
  3×, Zelda copia el framebuffer a fbufSave, los dos a 3×; Link, a 5×, se
  copia a su búfer del heap, también a 5×, y la pantalla de equipo lo
  dibuja sobre el framebuffer a 3×).
- **Scratch** (el destino ligado leído como textura): toma el tamaño de la
  superficie copiada y, con él, su escala (`scratch_scale`); si en la
  misma tarea esa superficie se rehace (crece), el scratch deja de valer.
- **Profundidades**: en píxeles de la superficie del destino con que se
  dibujan (rsxkit las pide al menos de ese tamaño). Si una z image ya tiene
  profundidad y no alcanza (la usó un destino de otra escala), se rehace en
  la misma entrada con el mayor tamaño de los dos en cada lado: antes
  quedaban dos profundidades con la misma dirección (zclear borraba la
  vieja) y, entre dos destinos que se turnan, se destruía y creaba a cada
  cambio. Los borrados van en coordenadas lógicas del destino ligado.
- **Cobertura (VISCVG) y cebado**: no dependen de la escala (la cobertura
  se escribe en la RDRAM en puntos lógicos; el cebado estira la imagen de
  la RDRAM sobre la superficie entera).
- **Presentación**: la región en la escala del destino presentado. Se
  presenta a un múltiplo entero solo si cabe y llena al menos 3/4 del alto
  de la pantalla (`rdp_present_integer`: 3× en 720p, 4× en 1080p, 1× por
  3); si quedaría chica (2× en 720p, un framebuffer de 640 × 480 a 1×) se
  ajusta como antes del recorte del overscan.
- **Memoria**: el scratch cuenta en el tope de 112 MiB con los destinos,
  las profundidades y la profundidad de la pantalla agrandada; el peor caso
  son 14 superficies (8 + scratch + 4 + 1) de 320 · g × 240 · g, 102,5 MiB
  a 5× (`docs/memory-budget.md`).
- **Destinos ligados sin profundidad propia** (cebado, scratch, dibujos
  sin z image): van con la profundidad de la pantalla, que a 4×–5× en 720p
  era más chica que el destino. Ahora se agranda al mayor destino posible
  al elegir la escala (`rsxkit_depth_reserve`, `rdp_zeta_reserve`) y
  `rsxkit_bind`, `rsxkit_target_copy`, `rsxkit_target_fill` y
  `rsxkit_target_read` se niegan con una zeta más chica (`RSXKIT_EDEPTH`).
  Elección explicada en `src/rdp/rdp.h`.
- **«Aplicar ahora»** (`salsa64_rdp_set_scale`, entre juegos): suelta
  destinos, scratch y profundidades y descarta las copias en vuelo; la
  memoria mapeada de las copias se conserva (no depende de la escala).

La reducción a 1× es bilineal en el centro de cada bloque de s × s: a
escala impar toma el píxel del centro; a escala par, el promedio de los
2 × 2 centrales (`targets_readback_scaled` lo simula en el host a 1×–5×).

El host (`src/host/raster.c`) sigue la misma política con la misma tabla:
lee texturas de sus imágenes, copia a la RDRAM y ceba, de modo que la
pausa de Zelda en la Mac muestra lo que mostrará la PS3 (sin los tiempos).
Sus destinos de 16 bits guardan cada canal con la precisión de la N64 (los
5 bits altos, los mismos que guarda la copia a la RDRAM, y de vuelta a 8
como `v << 3 | v >> 2`, igual que la HLE decodifica el RGBA16): así leer
un destino o su copia en la RDRAM da exactamente lo mismo, y la RDRAM no
cambia. Hace falta para los estados guardados: al cargar,
los destinos se sueltan y lo dibujado se lee de la RDRAM; con 8 bits por
canal en el host, un efecto que vuelve a leer lo que dibujó (el
acumulador del desenfoque de Majora's Mask) daba, guardando a mitad del
baile del Espantapájaros, 40 bytes de RDRAM distintos de la corrida de
corrido. La PS3 sigue a 8 bits por canal (el RSX) y no es determinista.

## Interruptor y costo

`SALSA64_DATA_DIR/fbfx.cfg`: `0` apaga todo (como antes), `1` solo render
a textura, `2` también copias y cebado (por omisión). En el host,
`SALSA64_FBFX`. El log escribe cada 5 s «gfx: framebuffer: N copias (ms
c/u), la emulación esperó N tareas (ms c/u, N tarde), texturas desde un
destino: del RSX, de la RDRAM, del ligado, decodificadas en el PPE (sin
lugar); destinos cebados», y la línea de la HLE cuántas texturas por tarea
no decodificó por venir de un destino. En el host, `salsa64-run` lo resume
al final («sin decodificar en la HLE, decodificadas por el rasterizador»)
y `SALSA64_TEXSTAT` lo cuenta por ventana; `SALSA64_NO_FBTEX=1` vuelve a
decodificarlas todas en la HLE. Mediciones en la PS3 en `docs/perf-log.md`;
memoria en `docs/memory-budget.md`.

## La z image en la RDRAM (2026-10-12)

**Donkey Kong 64** lee la profundidad desde el CPU: cada cuadro mira la z
image (`5d5600`, 320 × 240) en el punto de la pantalla donde se ve al
jugador y a ±40 píxeles (rutina en `80700ae4`: el valor de 16 bits con 3
bits de exponente y 11 de mantisa) y, si algo lo tapa 21 cuadros seguidos,
recoloca la cámara detrás de él. Como la profundidad solo vivía en el RSX,
en la RDRAM había ceros (lo más cerca posible): el juego veía al jugador
tapado siempre y la cámara iba y volvía cada 21 cuadros.

El borrado de la z image (FILLRECT en modo FILL con la color image en la
z image, `GFX_OP_ZCLEAR`) se escribe ahora también en la RDRAM, como lo deja
el RDP: `salsa64_gfx_rdram_fills` (`src/rsp/gfx/sync.c`) recorre la salida
de la tarea en su `finish` (backend síncrono y el del SPE, en el hilo del
CPU) y pone el color de relleno de 32 bits por dirección (sirve para 8, 16
y 32 bits por píxel), recortado al ancho de la imagen y, desde la HLE, al
scissor (borde inferior derecho exclusivo, como el RDP): Conker borra 217
filas con un scissor de 216 y guarda una lista enlazada en la fila que el
RDP no toca (docs/estado.md, 2026-10-13). Va antes de entregar
la tarea a la presentación, así que las copias esperadas de esa misma
tarea (Zelda copia el cuadro a la dirección del z-buffer en la pausa) la
pisan después, en el mismo orden que en el hardware. Invalida el código
recompilado del tramo y no avisa a los efectos de framebuffer
(`rdram_written` es para DMA).

Lo que se dibuja no llega: la z image de la RDRAM queda como recién
borrada, y para el juego nada tapa al jugador (en el hardware, un muro sí).

## La z que lee el CPU: copia desde el RSX (diseño, 2026-10-18)

Caso que lo pide: Zelda OoT, el resplandor de las antorchas del Pozo de
Kakariko se ve a través de las paredes (y, con la misma causa, DK64 y la
cámara). Zelda lee con el CPU la z image de la RDRAM en el punto del
resplandor para decidir si está tapado; en la PS3 a la RDRAM solo llega el
borrado de la z (`salsa64_gfx_rdram_fills`), no lo dibujado (sección de
arriba). No se implementó en esta ronda; lo que sigue es el plan y su costo.

1. **Cuándo.** Solo con la z image que el CPU lee: no hay protección de
   páginas en la PS3 (fastmem bloqueado por lv2, docs/fastmem.md), así que no
   se puede detectar la lectura; el interruptor es por juego (lista por
   código del cartucho: Zelda OoT/MM y DK64) y un archivo de bandera para
   probar con otros. Apagado no cuesta nada.
2. **Qué.** Al terminar la última tarea de un cuadro que dibujó con prueba
   o escritura de z en una z image (`GFX_DEPTH_*`), copiar la z image del
   RSX (Z24S8 a la resolución interna del destino) a la RDRAM de esa z
   image (320 × 240 × 2 B = 150 KB).
3. **Cómo, en el RSX.** Una transferencia (`rsxSetTransferData` /
   `rsxSetTransferScaleSurface`) trata la zeta como un A8R8G8B8 de 32 bits y
   la reduce con muestreo puntual (no se puede filtrar una z) del
   tamaño interno (hasta 1600 × 1200 a 5×) a 320 × 240 en una superficie en
   memoria principal mapeada; el mismo mecanismo de `rsxkit_target_read`
   (copia a la RDRAM del color, 0,3–7 ms medidos en la PS3 según las escenas:
   docs/estado.md, Mario Tennis). Estimación: 0,3–0,5 ms de RSX y la espera
   de su barrera (la misma que ya espera la copia del color).
4. **Cómo, en el CPU/SPE.** Convertir 76 800 palabras Z24 a la z de la N64:
   `z18 = z24 >> 6` (la zeta del RSX ya es 0..1 como la z de la N64 tras la
   viewport), comprimida a 14 bits en el formato del RDP (3 bits de
   exponente, 11 de mantisa; el exponente es el número de unos iniciales de
   los 18 bits) y dos bits de `dz`; el consumidor lee la palabra de 16 bits
   `[z comprimida (14)][dz (2)]`. El `dz` puede salir de la pendiente entre
   vecinos o fijo (el juego compara con margen). Vectorizado en un SPE:
   decenas de µs; en el PPE, ~0,3 ms. Va en `src/rdp/` (la conversión es de
   la N64; el copiado de la zeta en `lib/rsxkit/`, genérico).
5. **Orden.** Después de `salsa64_gfx_rdram_fills` de la misma tarea (el
   borrado ya está en la RDRAM) y antes de que el CPU emulado siga: la tarea
   se da por terminada solo cuando la copia se escribió (barrera), como las
   copias del color; hay que respetar la guarda contra el CPU de la rama
   `rc6-memoria` (el CPU no toca esa RDRAM mientras se escribe).
6. **Medir.** ms de RSX y de espera por cuadro en `docs/perf-log.md` con
   el interruptor encendido y apagado, en el Pozo de Kakariko y en DK64.

## Pendiente

- Profundidad de lo dibujado en la z image de la RDRAM (ver «La z image
  en la RDRAM»): rasterizarla en un SPE, como la referencia de software de
  otros emuladores, o copiarla del RSX. Hoy los juegos que la leen la ven
  siempre borrada (Donkey Kong 64: la cámara no se recoloca por oclusión;
  posible el destello del sol de Zelda aun detrás de un muro).

- Polígono transitorio (~1 s) al abrir la pausa de Zelda en la PS3:
  confirmar que las cuatro profundidades lo quitan.
- Conker, intro: qué es la color image de 292 que dibuja cada tarea sin
  presentarla ni leerla (el host no la reproduce ni con START y A; en la
  PS3 sí): un `gfxdump` en esa escena lo diría.
- Perfect Dark y Conker: copias del z-buffer (rectángulos que leen la z
  image como color image), sombra y desenfoque de Conker; color images de
  32 bits y CI8 (Conker) no se copian ni se ceban.
- El «estado» del SPE con las franjas de Perfect Dark casi se duplicó
  (combiner en `-Os`, docs/perf-log.md): guardar el resultado del combiner
  mientras no cambien sus entradas si llega a costar campos/s.
- Comprobar que el baile del Espantapájaros y el recuerdo de Zelda en
  Majora's Mask también salen por `GFX_TEX_TARGET` (hace falta un estado en
  esas escenas).
- Una franja de `LOADBLOCK` de libultra (ancho 1 en `SETTIMG`) con una tile
  de línea distinta del ancho de la imagen sigue sin reconocerse: no hay de
  dónde sacar el paso (no se vio en ningún juego).
- Reducir s× → 1× con filtro de caja (hoy bilineal del RSX en el centro de
  cada bloque: el píxel central a escala impar, 2 × 2 a escala par) y, si
  hiciera falta por costo, la conversión en un SPE.
- Confirmar en la PS3 real la pausa y la pantalla de equipo de Zelda y el
  túnel de Mario Kart a 1×, 2× y 5× (en RPCS3, gfxtest 35/35 a 1×, 3× y 5×).
- La HLE emite dos color images imposibles al abrir la pausa de Zelda
  (anchos 1495 y 3770): de qué comando salen.
