# Usar Salsa64 en la PS3

Guía para quien instala el `.pkg` en una PS3 con HEN o CFW. Salsa64 no
incluye ROMs ni las acepta en el repositorio: se usan respaldos propios.

## Instalación

1. Copiar el `.pkg` a la PS3 (por FTP o por USB) e instalarlo desde el
   Package Manager del XMB. Aparece como «Salsa64» en la sección de juegos.
2. Poner las ROMs donde las busca el menú (siguiente sección).
3. Abrir Salsa64 desde el XMB.

## Dónde poner las ROMs

El menú busca ROMs en estas carpetas, al arrancar la aplicación:

| Carpeta | Qué es |
|---|---|
| `/dev_hdd0/salsa64` | Disco duro interno de la PS3 |
| `/dev_usb000/salsa64` … `/dev_usb007/salsa64` | Una memoria USB: carpeta `salsa64` en la raíz |

Dos formas de llevarlas allí:

- **FTP** (webMAN MOD): conectarse a la PS3 desde la computadora y copiar los
  archivos a `/dev_hdd0/salsa64` (crear la carpeta si no existe).
- **USB**: formatear la memoria en FAT32, crear una carpeta `salsa64` en la
  raíz, copiar allí las ROMs y conectarla a la PS3 antes de abrir Salsa64.

Reglas de la búsqueda:

- Extensiones: `.z64`, `.v64`, `.n64` y `.zip` (mayúsculas o minúsculas). Se aceptan
  los tres órdenes de bytes (big-endian, byte-swapped y little-endian); el
  formato se reconoce por la cabecera, no por la extensión. Un ZIP debe ser
  estándar, sin cifrado ni ZIP64, y contener al menos una ROM con extensión
  `.z64`, `.v64` o `.n64`; se elige la primera entrada compatible. Admite
  compresión Deflate o archivos almacenados sin compresión. La ROM extraída
  no puede superar 64 MiB y se comprueba el CRC-32 antes de arrancarla.
- Las carpetas aparecen primero. × entra en una carpeta y la fila `..`
  vuelve al nivel anterior. Se recorren también las subcarpetas de cada
  carpeta `salsa64`.
- Se ignoran los archivos que empiezan con `.`, los que no tienen una de
  esas extensiones, los que miden menos de 4 KB y los ilegibles. En cada
  carpeta relativa se juntan las entradas de todas las unidades y se admiten
  hasta 4096 filas en total, contando ROMs, carpetas y `..`.
- En la raíz de cada ubicación `salsa64` no salen las carpetas que Salsa64
  crea para sus propios datos: `saves`, `states`, `covers`, `cheats`,
  `perfilado`, `perfiles`, `capturas` y `cajanegra-*`. Tampoco `juegos`
  (los `.cfg` de cada juego), salvo que tenga ROMs dentro. Dentro de una
  carpeta de ROMs (o de una subcarpeta) esos nombres sí se muestran. No se
  ocultan las demás carpetas vacías: saber si una carpeta tiene ROMs
  obligaría a leer cada una al releer la lista.
- Una ROM de hasta 64 MB se lee entera a memoria al abrirla. Las de más de 64 MB
  (hacks y homebrew, hasta los 252 MB que direcciona el cartucho) se abren **por
  fragmentos**: se quedan en el disco y el emulador lee páginas de 256 KB bajo
  demanda a una caché de 32 MB. El log lo dice («por fragmentos») y cada 5 s
  cuenta lecturas, fallos de caché y milisegundos de lectura (línea «rom:»).
  Se aceptan los tres órdenes de bytes (z64, v64, n64). Un archivo `rom-paginada`
  en la carpeta de datos fuerza el modo por fragmentos también para ROMs
  pequeñas (para comparar), y `rom-cache` con una cifra fija los MB de la caché
  (1 a 128; por omisión 32). Un hilo lector trae las páginas del disco
  en segundo plano y pide por adelantado las siguientes a cada fallo; solo se
  espera cuando el dato aún no viene en camino. Un archivo `rom-lectura-sincrona`
  vuelve a la lectura que bloquea al hilo de emulación.
- **Opciones → Orden de la lista** elige el título de la cabecera o el nombre
  del archivo (`orden = titulo|archivo` en `menu.cfg`). Si la fuente no puede
  mostrar algún carácter del título de la cabecera, se usa el nombre del
  archivo.
- La última ROM abierta recuerda también su carpeta y al iniciar vuelve a
  mostrarla.
- Si la lista está vacía, el menú muestra las carpetas donde buscó.

## Menú

Se dibuja con el RSX; el panel de la derecha muestra el archivo, su
ubicación, el código del juego, el tamaño, la región y el tipo de guardado
detectado y, si la hay, la carátula del juego (abajo, «Carátulas»). La
última ROM abierta queda elegida al volver al menú.

| Botón (DualShock 3) | Acción |
|---|---|
| Cruceta arriba/abajo | Elegir (mantenida, repite y acelera) |
| Cruceta izquierda/derecha | Saltar una página |
| × | Abrir la ROM elegida (o la opción) |
| □ | Trucos de la ROM elegida (abajo, «Trucos») |
| SELECT | Ajustes de la ROM elegida (abajo, «Ajustes por juego») |
| L3 | Actualizar la lista de ROMs de la carpeta actual (relee la carpeta y las cabeceras nuevas o cambiadas) |
| △ | Ir a Opciones |
| ○ | Volver de Opciones, de los trucos o de los ajustes; en una subcarpeta, subir un nivel; en la raíz, salir al XMB (pregunta antes: × sale, ○ sigue donde estaba) |

Las cabeceras de las ROMs se guardan en `/dev_hdd0/salsa64/romlist.cache`
(nombre, tamaño, fecha y cabecera de cada archivo): el arranque solo lee las
ROMs nuevas o cambiadas, y al volver de un juego no se relee una carpeta cuya
fecha no cambió. Si algo no se ve actualizado, L3 relee la carpeta; borrar
`romlist.cache` (por FTP) la rehace entera.

Al final de la lista están «Opciones» y «Salir al XMB».

## Remapeo del control

En **Opciones → Controles** se cambian las asignaciones para todos los juegos.
En **Ajustes** de una ROM (SELECT), la fila **Controles** abre el mismo editor
solo para ese juego; el panel de la derecha dice cuál se está editando, y las
asignaciones propias de un juego tienen prioridad sobre las globales. Cada
cambio se guarda en el momento.

× en un botón de N64 espera la pulsación del DualShock 3 y la asigna al
soltar: se pueden pulsar dos a la vez (por ejemplo L2 y R2 para Z) y la fila
queda «L2 / R2», cualquiera de los dos. SELECT deja el botón sin asignar y
START cancela sin cambiar nada.

Los botones C salen siempre del stick que no mueve el de la N64 (el derecho;
el izquierdo si en la fila **Stick** se elige el derecho); un botón asignado a
una fila C se suma al stick. **Restaurar predeterminado** repone las
asignaciones globales o hace que el juego vuelva a heredar las globales.

| Control de N64 | Predeterminado del DualShock 3 |
|---|---|
| A / B / Z | × / □ / L2 |
| Start / L / R | Start / L1 / R1 |
| Cruceta | Cruceta del DualShock 3 |
| C arriba, abajo, izquierda y derecha | Stick analógico derecho |
| Stick analógico | Stick analógico izquierdo |

El menú guarda el mapeo global en `controles.cfg` y las excepciones por juego
junto con sus ajustes en `juegos/<ROM>.cfg`.

«Actualizar lista» también está en Opciones. Vuelve a leer las carpetas de
las unidades conectadas sin salir de Salsa64, conserva la carpeta abierta y
la ROM elegida si todavía está disponible, y muestra cuántas ROMs encontró.
Sirve para detectar archivos copiados por FTP o una memoria USB conectada
después de abrir el menú.

**Versión.** Arriba a la derecha, chica, bajo el número de juegos, va la
versión de Salsa64: la etiqueta y el commit, como «v0.2.5 · fb12aeb»
(«v0.2.5+3 · abc1234» si es tres commits posterior a v0.2.5; «-dirty» al
final si se compiló con cambios sin commit). Un paquete de prueba, armado
con otro título o APPID, muestra debajo también los suyos: «Salsa64 gráficos
3 · SLSA00114». La misma versión sale a la derecha de «Pausa» en el menú de
pausa y en la primera línea del log (`salsa64.log` y UDP):
«Salsa64 v0.2.5-3-gabc1234 (Salsa64 gráficos 3, SLSA00114)». Conviene
mirarla antes de probar algo: dos apps instaladas pueden llevar el mismo
título en el XMB. Al reportar un problema, incluirla.

Opciones:

- **Fondo animado**: enciende o apaga las ondas del fondo.
- **Recorte del borde (overscan)**: sin recorte, normal (por omisión) o
  amplio. Recorta los bordes de la imagen, donde algunos juegos dejan
  basura.
- **Aspecto**: 4:3 (la imagen original, con franjas a los lados en una tele
  panorámica) o 16:9 (la estira a lo ancho para llenar la pantalla). Vale al
  abrir el juego; se guarda en `menu.cfg` (`aspecto = 4:3|16:9`). Con 16:9 no
  se usa la presentación 1:1 de la escala entera.
- **Filtro**: cómo se ve la imagen del juego al escalarla. **Suave** (por
  omisión, la de siempre: los píxeles se mezclan), **Nítida** (puntos
  cuadrados, sin mezclar; se nota cuando la escala interna no coincide con la
  pantalla), **Líneas suaves** y **Líneas fuertes** (filas oscurecidas
  encima de la imagen, como una tele de tubo; las capturas salen sin ellas).
  Estos cuatro son de VIDEOGAMES SCZ. Después vienen los **filtros CRT
  originales del autor**, todavía experimentales: «CRT máscara (exp.)»,
  «CRT RGB (exp.)», «CRT S-Video (exp.)», «CRT compuesto (exp.)» y «CRT
  Trinitron (exp.)» (abajo, en «Filtros CRT»). Vale al abrir el juego; se
  guarda en `menu.cfg` (`filtro = suave|nitido|lineas|lineas+|crt-mascara|
  crt-rgb|crt-svideo|crt-compuesto|crt-trinitron`). El panel dice de quién
  es cada filtro.
- **Densidad** (solo con «CRT máscara»): Normal, la del filtro original, o
  Fina, con los puntos de fósforo más chicos y más juntos. Se
  guarda en `menu.cfg` (`densidad = normal|fina`).
- **Interpolación** (experimental): Apagado (por omisión) o Por geometría.
  En un juego que dibuja 20 o 30 cuadros por segundo (Zelda OoT, 20), los
  campos en que la tele repetiría el cuadro se dibujan con los objetos
  entre ese cuadro y el anterior: se ve a 60, con 33 ms de retraso (el
  cuadro de verdad llega dos campos después). Los vértices de cada cuadro se
  emparejan con los del anterior (docs/interpolacion.md); lo que no se
  empareja (un cambio de plano, algo que aparece) se queda donde está. Vale
  al abrir el juego, por juego y en vivo desde «Imagen» del menú de pausa;
  se guarda en `menu.cfg` (`interpolacion = no|geometria`). Cuesta más
  trabajo del RSX (la escena se dibuja hasta tres veces por cuadro del
  juego) y ~2,4 MB de memoria y una superficie más mientras está encendida.
- **Resolución**: 720p (por omisión, render a 3×) o 1080p (render a 4×).
  Se guarda en video.cfg (así arranca la próxima vez) y se pone con
  «Aplicar ahora», sin cerrar Salsa64; el panel muestra la salida actual y,
  si pediste 1080p y no se logró, recuerda activarlo en Ajustes de pantalla
  del XMB.
- **Escala interna**: tamaño del render de la N64, independiente de la
  salida de video: 1× nativa (320 × 240, la imagen original), 2×, 3×, 4× o
  5× (1600 × 1200), cada una fija o «Automática (hasta N×)». La automática
  baja un paso cuando durante 3 s la emulación conserva al menos 92 % del
  ritmo del VI, aumentan los cuadros saltados y el trabajo medido del RSX
  ocupa un período de la tele; una carga aislada o un juego lento por CPU no
  la baja. Una escala que ya falló no se vuelve a probar mientras ese juego
  siga abierto; la inferior se guarda en `juegos/<ROM>.cfg` y es la escala
  inicial y el techo aprendido la próxima vez. No sube automáticamente: el
  coste del paso siguiente no se puede predecir de forma fiable por el margen
  del actual.
  Deja al menos 10 s entre bajadas. Una escala fija
  no cambia: si el RSX no alcanza, un aviso propone bajarla. El límite elegido fija un objetivo
  de líneas (5× = 1200) y cada imagen usa la entera que llega ahí sin
  pasarse: una de 640 × 480 usa 2× a 5×. Una escala mayor que la
  pantalla es supermuestreo: la imagen se reduce con filtro lineal al
  presentarla, con más nitidez y bordes suaves, a cambio de más trabajo
  para el RSX. Una escala que deja la imagen chica (2× en 720p, 3× en
  1080p) se ajusta a la pantalla con filtro lineal; 1× se amplía a un
  múltiplo entero. Los efectos de framebuffer (pausa de Zelda, túnel de
  Mario Kart) usan la escala de cada imagen (docs/framebuffer.md). Un tope
  de 112 MiB de memoria del RSX para las imágenes vivas recicla la más
  vieja si hace falta; a 5× el peor caso son 102,5 MiB
  (docs/memory-budget.md); el log dice «RDP listo: escala-objetivo …». Se
  pone también con «Aplicar ahora».
- **Aplicar ahora**: solo aparece mientras la resolución o la escala elegidas
  difieren de las que corren. Cambia el modo de video y la escala desde el
  menú, sin juego abierto y sin cerrar Salsa64; la imagen parpadea un
  instante al cambiar de modo. Si la tele no acepta el modo, queda el
  anterior y el panel dice «No se pudo: sigue …»; si sale bien, «Aplicado:
  1920x1080, escala 4×». El log dice «video: 1920x1080, escala 4×, aplicado
  sin reiniciar». Cambiar solo la escala no toca el video.
- **Mostrar FPS**: el contador de velocidad dentro del juego (abajo, en
  «Contador de velocidad»). También se cambia en el juego con SELECT + △.
- **Confirmar al salir**: con «Sí» (por omisión), «Salir» del menú de pausa
  pregunta antes de cerrar el juego (abajo, en «Salir del juego»); con «No»,
  sale directo. Se guarda en `menu.cfg` (`confirmar = si|no`).
- **Idioma**: «Automático» (por omisión en instalaciones nuevas, sigue el
  idioma del sistema de la consola), español, English, Français, Deutsch,
  Italiano, Português, Nederlands, Svenska, Dansk, Norsk, Suomi, Polski,
  Türkçe, Русский, 日本語, 한국어, 繁體中文 o 简体中文 (los asiáticos solo si la
  fuente del sistema carga; si no, vuelven a English). Traduce el menú, los paneles, los avisos del juego, el menú de
  pausa y la lista de trucos (los títulos de los juegos y las rutas quedan
  como están). Se guarda en `menu.cfg` (`idioma = auto|es|en|fr|de|it|pt|nl|sv|da|no|fi|pl|tr|ru|ja|ko|zh-hant|zh-hans`).
  La fuente del sistema de la consola (ver `docs/fuente-sistema.md`) dibuja
  todo el texto; la 8x13 integrada queda de respaldo (solo ASCII, Latin-1 y
  parte del cirílico).
- **Descargar carátulas** / **Descargar trucos**: experimental y oculto por
  omisión (abajo, «Descargas»).

### Carátulas

Si hay una imagen del juego en `/dev_hdd0/salsa64/covers/`, el panel la
muestra a la izquierda de los datos cuando la selección se detiene un
momento. Solo **PNG** (todos los tipos de color, también entrelazados, de
hasta 8 MB); se reducen a 320 × 448 como máximo. Salsa64 no trae carátulas:
se ponen por FTP. Se busca, en este orden:

1. el nombre de la ROM sin la extensión: `mijuego.z64` → `mijuego.png`;
2. el código del juego de 4 letras que muestra el panel (`NXXE.png`);
3. un nombre parecido, como los de los paquetes de carátulas de RetroArch
   («Nombre del juego (USA).png»): se ignoran mayúsculas, signos y lo que va
   entre paréntesis o corchetes, y vale que uno esté dentro del otro
   («NOMBREJUEGO64» ↔ «Nombre Juego 64 (USA).png»), siempre de la misma región
   que la ROM (por las etiquetas `(U)`, `(E)`, `(J)`, `(USA)`, `(Europe)`… o
   por la letra de región del código). Si el parecido cuadra con varios
   juegos, no se elige ninguno.

Si no encuentra ninguna, el log dice «carátula: ninguna para …» con el código
del juego, para renombrar la imagen a `<código>.png`. Una imagen dañada o
demasiado grande se ignora y el log dice por qué.

### Trucos

□ sobre una ROM abre su página de trucos: × activa o desactiva el elegido,
○ vuelve. Los activos se aplican al abrir el juego, en cada campo de video, y
se recuerdan en `<nombre>.on`, al lado del `.cht`, sin tocar el `.cht`.

Los archivos van en `/dev_hdd0/salsa64/cheats/` y se buscan como las
carátulas (nombre de la ROM con `.cht`, sin distinguir mayúsculas, o un
nombre parecido de la misma región; para los parecidos se piden 8 letras del
nombre, porque un truco de otro juego escribiría en otra memoria). Valen dos
formatos, que se reconocen solos:

- **Project64** (`Config/Cheats`): secciones `[CRC1-CRC2-C:xx]`; se usa la
  del CRC de la ROM (si ninguna coincide, la primera, y el menú avisa). Cada
  `$Grupo\Nombre` sale como «Grupo > Nombre»; los trucos con opciones (`??`
  en el valor y líneas `nn Opción` debajo) salen como un truco por opción.
- **RetroArch**:

  ```
  cheats = 1
  cheat0_desc = "Vidas infinitas"
  cheat0_code = "8107A6C4 0063"
  cheat0_enable = false
  ```

  Varias líneas de código se separan con `+`.

Códigos GameShark que se aplican: `80`, `A0`, `F0` (un byte), `81`, `A1`,
`F1` (16 bits), `D0`–`D3` (la línea siguiente solo si el byte o los 16 bits
son iguales o distintos) y `50` (repite la línea siguiente). Los demás tipos
(botones del GameShark, etc.) se ignoran y el panel lo avisa. Hasta 4096
trucos y 65 536 líneas por juego; `.cht` de hasta 1 MB. Un truco que se apaga
deja la memoria como quedó (como en la consola): para deshacerlo del todo,
cargar un estado o reiniciar el juego.

En el juego, **SELECT + □** (o «Trucos» en el menú de pausa) abre la lista de
trucos con el juego en pausa: cruceta arriba/abajo elige (mantenida, repite),
izquierda/derecha o L1/R1 saltan 8, × activa o desactiva (se guarda al
momento), ○, START o SELECT + □ cierran y el juego sigue. Si el juego no tiene
trucos, un aviso lo dice.

### Ajustes por juego

SELECT sobre una ROM abre sus ajustes: **Aspecto**, **Filtro**, **Recorte
del borde**, **Escala interna** e **Interpolación**, cada uno «Global» (lo de Opciones) o un
valor propio para ese juego (también se guardan desde «Imagen» en el menú de
pausa), y **Accesorio** («Global», «Controller Pak», «Rumble Pak»,
«Transfer Pak» o «Ninguno»; ver «Accesorios»). Se guardan en
`/dev_hdd0/salsa64/juegos/<nombre de la ROM sin
extensión>.cfg`:

```
aspecto = 16:9      # 4:3 | 16:9 | global
filtro = lineas     # suave | nitido | lineas | lineas+ | crt-mascara | crt-rgb | crt-svideo | crt-compuesto | crt-trinitron | global
recorte = apagado   # apagado | normal | amplio | global
escala = 2          # auto-N | 1 … 5 | global
escala-estable = ninguna # 1 … 5 | ninguna (techo aprendido de la automática)
densidad = fina     # normal | fina | global (la de «CRT máscara»)
interpolacion = geometria  # no | geometria | global
accesorio = controller-pak # global | controller-pak | rumble-pak | transfer-pak | ninguno
```

Al abrir el juego, el log dice «imagen: … (con ajustes de este juego)» y
«escala interna: por juego|global|por omisión, modo fija|automática, …»; al
cerrarlo vuelve la de Opciones.

### Descargas (experimental, apagadas)

Opciones puede bajar de internet las carátulas y los trucos que falten a las
ROMs de la lista, pero está **apagado y oculto por omisión**: la PS3 solo
habla TLS antiguo (GitHub exige uno moderno, así que es probable que falle) y
Salsa64 no puede verificar los certificados HTTPS (los de la consola son
viejos): la conexión no está protegida contra quien se meta en medio. Solo
se bajan PNG y `.cht` (nada que se ejecute), a `covers/` y `cheats/`, con
nombres sin carpetas, y nunca se pisa un archivo que ya esté. Todavía no se
probó en la PS3 real.

Para encenderlo, crear `/dev_hdd0/salsa64/descargas.cfg` con:

```
activar = si
# Opcional: otras direcciones (base terminada en «/» e índice con la lista
# de archivos: un nombre por línea, o el JSON de la API de GitHub).
caratulas = http://IP-DE-TU-PC:8000/covers/
caratulas_indice = http://IP-DE-TU-PC:8000/covers/index.txt
trucos = http://IP-DE-TU-PC:8000/cheats/
trucos_indice = http://IP-DE-TU-PC:8000/cheats/index.txt
```

Sin direcciones propias usa las de GitHub (el paquete de carátulas
«RetroArch-Nintendo-64-USA-Box-Art» y `Config/Cheats` de Project64). La
salida más segura es un servidor HTTP en la red de casa: bajar los archivos a
la PC, escribir `index.txt` con un nombre por línea y servir la carpeta (por
ejemplo con `python3 -m http.server 8000`). Aparecen dos filas en Opciones,
«Descargar carátulas» y «Descargar trucos»: × empieza (para cada ROM sin
carátula o sin `.cht` busca en el índice con el mismo criterio de nombres
parecidos), el pie muestra «Descargando… N de M» y ○ cancela. Sin red, dice
«No hay red».

## RetroAchievements

Logros de [retroachievements.org](https://retroachievements.org) en los juegos
de N64 (apagado por omisión). **Opciones > RetroAchievements: Sí**, luego
**Cuenta de RetroAchievements** (×): el teclado del sistema pide usuario y
contraseña; solo se guarda el token (`ra.cfg`). Al abrir un juego sale «RA: N
logros (X conseguidos)», al desbloquear uno su título, y en la pausa (START +
SELECT) la fila **Logros** lista los del juego. El modo **Hardcore** (Opciones,
vale al abrir el siguiente juego) quita los estados y los trucos mientras haya
logros. Detalles, red y límites en `docs/retroachievements.md`.

## Controles en el juego

Los DualShock 3 de los puertos 1 a 4 son los controles 1 a 4 de la N64.

| N64 | DualShock 3 |
|---|---|
| Stick | Stick izquierdo |
| Botones C | Stick derecho |
| Cruceta | Cruceta |
| A | × |
| B | □ |
| Z | L2 |
| L | L1 |
| R | R1 |
| Start | START |

○, △, R2, L3, R3 y SELECT no tienen función en la N64. SELECT sirve para
los atajos de Salsa64:

| Atajo | Qué hace |
|---|---|
| START + SELECT | Menú de pausa: continuar, estados, trucos, FPS, reiniciar y salir. Ver «Salir del juego» |
| SELECT + □ | Lista de trucos del juego, con el juego en pausa (ver «Trucos») |
| SELECT + R1 | Guarda el estado en la ranura actual |
| SELECT + L1 | Carga el estado de la ranura actual |
| SELECT + derecha / izquierda (cruceta) | Cambia de ranura (0 a 9, en ronda) |
| SELECT + △ | Muestra u oculta el contador de velocidad |
| SELECT + R2 | Depuración gráfica: guarda en `/dev_hdd0/salsa64/` la salida de la HLE del cuadro en pantalla (`gfxdump-<campo>.bin`) y su captura (`gfxdump-<campo>.ppm`) |
| SELECT + L2 | Caja negra (solo con el archivo `cajanegra`, abajo): escribe en `/dev_hdd0/salsa64/cajanegra-<campo>/caja.s4bb` las últimas tareas gráficas y campos presentados, para repetirlos en la Mac |

Mientras SELECT está apretado, R1, L1, R2, L2, △ y la cruceta izquierda/derecha no
llegan al juego (□ tampoco, cuando abre la lista de trucos) (no se mueve el personaje al cambiar de ranura). Si
`controles.cfg` asigna SELECT a un botón de la N64, ese botón se pulsa
también con los atajos.

«Salir del juego» del menú del sistema (botón PS) sale en cualquier
momento.

## Salir del juego y el botón PS

START + SELECT pausa el juego (imagen y sonido quietos) y abre el **menú de
pausa** sobre el último cuadro (START solo no sirve: los juegos de la N64 lo
usan):

| Fila | Qué hace |
|---|---|
| Continuar | El juego sigue donde estaba |
| Guardar estado / Cargar estado | En la ranura actual, como SELECT + R1 / L1 |
| Ranura ◀ n ▶ | Cambia la ranura (cruceta izquierda/derecha o ×) |
| Trucos | La lista de trucos del juego (si tiene) |
| Imagen | Filtro, aspecto, recorte del borde, escala interna e interpolación del juego en curso (abajo) |
| Mostrar FPS | Enciende o apaga el contador de velocidad |
| Reiniciar (botón Reset) | El reinicio en caliente de la consola. Pregunta siempre «¿Seguro?» (× sí, ○ no). El juego sigue ~0,5 s emulados: primero recibe la interrupción de PRE_NMI (es la que libultra usa para guardar y callar el audio) y después el NMI: el CPU y el RCP se reinician, la RDRAM se conserva salvo el primer megabyte, que se vuelve a copiar de la ROM, y el juego ve `osResetType` = 1 (reset suave). Vuelve al logo o al título con lo que el juego dejó en la RDRAM, como el botón Reset de una N64 |
| Reiniciar desde cero | Arranque en frío del mismo juego sin pasar por la lista: cierra el juego entero (escribe la partida, el Controller Pak y la EEPROM/SRAM/Flash, suelta los SPEs y vacía la caché del recompilador) y lo abre otra vez con los mismos ajustes. Pregunta siempre «¿Seguro?»; lo que el juego no haya guardado se pierde |
| Volver al menú (con `embedded`, Salir al XMB) | Cierra el juego como siempre (guarda la partida, apaga el audio y los SPEs). Con «Confirmar al salir: Sí» pregunta antes: × sí, ○ sigue jugando |

Cruceta arriba/abajo elige (da la vuelta), × acepta; ○, START o START +
SELECT otra vez cierran el menú y el juego sigue. Mientras el menú o el
cuadro de confirmación están a la vista, nada de lo que se apriete llega al
juego, y el botón con que se cierran tampoco (el ○ no cuenta como B al
volver; hay que soltarlo). Los atajos de SELECT siguen funcionando igual.

**Imagen** abre un cuadro abajo a la izquierda, sin oscurecer el juego,
con **Filtro**, **Aspecto**, **Recorte del borde**, **Escala interna** e
**Interpolación**: cruceta izquierda/derecha (o ×) cambia el valor. La
interpolación se nota al seguir jugando. El filtro, el aspecto y el
recorte se ven al momento sobre el cuadro en pausa (el recorte de una imagen
que el juego dibujó con el CPU, al seguir). La escala interna se pone al
seguir jugando: cambiarla suelta las imágenes del juego en el RSX, que se
vuelven a dibujar a la escala nueva en el cuadro siguiente (puede verse un
cuadro raro en los juegos que reusan el anterior). La escala queda guardada
de inmediato para ese juego, incluida «Automática (hasta N×)» y su techo
aprendido. Los demás valores duran esta partida; **Guardar para este juego**
los deja en `juegos/<ROM>.cfg` (lo que coincide con Opciones queda
«global»). ○ o «Volver» regresan al menú de
pausa; START cierra todo y el juego sigue.

El **botón PS** abre el menú del sistema encima del juego: Salsa64 pausa la
emulación y el sonido mientras está abierto y sigue mostrando el último
cuadro; al cerrarlo, el juego sigue donde estaba. Los botones que se
aprieten en el menú del sistema no llegan al juego, y ninguno queda pegado
al volver. Desde ahí, «Salir del juego» cierra Salsa64 en cualquier momento
(también en el menú de Salsa64).

Para cambiar el mapeo, ver `controles.cfg` más abajo.

## Partidas guardadas

Salsa64 guarda lo que el cartucho guardaría (EEPROM, SRAM de 256 Kbit y
FlashRAM de 1 Mbit) en `/dev_hdd0/salsa64/saves`, un archivo por juego, con
el nombre `<título>-<CRC1>.<extensión>` (`.eep` para EEPROM, `.sra` para
SRAM, `.fla` para FlashRAM, de 128 KB). Se escribe unos segundos después de
la última escritura del juego y al volver al menú o salir. Para respaldarlas,
copiar esa carpeta por FTP. Los archivos van en el orden de bytes de la N64:
los `.sra` y `.fla` de mupen64plus traen invertidos los bytes de cada palabra
de 32 bits y hay que darlos vuelta para usarlos aquí.

El tipo de guardado se detecta solo: primero lo que diga `guardado.cfg`, luego
la cabecera de los homebrew y por último una tabla interna por código de
juego. El panel del menú lo muestra («EEPROM 4K», «SRAM», «FlashRAM»…); si
un juego no guarda y el panel dice «—», se fija a mano en `guardado.cfg`.
Los que guardan solo en el Controller Pak (Donald Duck Quack Attack, Rush
2049, Hydro Thunder) no llevan tipo de cartucho: el panel dice «—» y su
partida va al pak.
Usan FlashRAM, entre otros, Zelda: Majora's Mask, Paper Mario, Pokémon Snap,
Pokémon Puzzle League, Pokémon Stadium 1 y 2 y Jet Force Gemini. La SRAM de
768 Kbit y la de 1 Mbit se reconocen pero todavía no se emulan (el panel
dice «(sin emular)»).

## Accesorios

Los juegos que guardan en la memoria que se enchufa al control (Ms. Pac-Man,
por ejemplo) encuentran un Controller Pak de 32 KB en cada control conectado,
ya formateado. Las partidas viven en `/dev_hdd0/salsa64/saves`, junto a las
del cartucho, en un solo archivo por juego, `<título>-<CRC1>.mpk`, de 128 KB:
el pak del control 1 en el byte 0x0000, el del 2 en 0x8000, el del 3 en 0x10000
y el del 4 en 0x18000 (el mismo orden y los mismos bytes que el `.mpk` de
mupen64plus, así que un `.mpk` de ahí se copia con ese nombre y sirve). Se
escribe unos segundos después de la última escritura del juego, sin tocar
el disco en cada comando, y al volver al menú o salir.

Opciones elige el accesorio inicial de los cuatro controles. SELECT sobre una
ROM puede heredarlo o fijar otro. En el menú de pausa, **Accesorios** cambia
cada puerto cuando la emulación ya se detuvo; esa elección dura hasta cerrar
el juego. Controller Pak sigue siendo la omisión.

El Rumble Pak usa el motor grande del DualShock 3 a plena fuerza y deja
apagado el motor pequeño. Ambos motores se apagan al pausar, abrir el menú del
sistema, cambiar de accesorio y durante cualquier cierre, también el forzado.
El Transfer Pak experimental se describe en `docs/transferpak.md`.

## Estados guardados

Un estado guarda el juego entero en el instante (como pausar la consola y
sacarle una foto a la memoria): se vuelve exactamente a ese punto, en
cualquier parte del juego, no solo donde el juego deja guardar. Hay diez
ranuras por juego (0 a 9); al abrir un juego la ranura actual es la 0.

- **SELECT + R1** guarda en la ranura actual (si había algo, lo reemplaza).
- **SELECT + L1** carga la ranura actual.
- **SELECT + derecha / izquierda** cambia de ranura.

Un aviso abajo en la pantalla dice qué pasó durante unos 2 segundos:
«Estado guardado en ranura 3», «Ranura 4 (vacía)», «Estado cargado», «No
hay estado en la ranura 2». Guardar detiene la imagen medio segundo (lo que
tarda la PS3 en escribir unos 8 MB en el disco) y cargar, menos de un quinto
de segundo; los tiempos quedan en el log.

Los archivos van en `/dev_hdd0/salsa64/states`, con el mismo nombre que la
partida del juego en `saves/` y la ranura en la extensión:
`<título>-<CRC1>.st0` … `.st9` (unos 8 MB cada uno). Para respaldarlos,
copiar esa carpeta por FTP. Sirven también en `salsa64-run` (la PC) y al
revés.

A tener en cuenta:

- Cargar un estado también vuelve la partida del cartucho (EEPROM, SRAM o
  FlashRAM) a
  como estaba en ese momento, y así se escribe en `saves/`: un estado viejo
  deshace lo guardado después dentro del juego.
- Un estado es de una ROM: con otra (u otra versión del mismo juego) se
  rechaza. También se rechazan los de una versión de Salsa64 que ya no los
  entiende; el aviso lo dice.
- Con `lle` (RSP de bajo nivel, experimental) no hay estados.
- El primer cuadro después de cargar puede salir negro.

## Contador de velocidad

Con **SELECT + △** (o «Mostrar FPS» en Opciones) aparece arriba a la
izquierda una línea como:

```
VI 59,9 (100 %) · juego 30 · pantalla 60
```

- **VI**: campos de video que emula Salsa64 por segundo y, entre paréntesis,
  la velocidad respecto de la consola (100 % es velocidad completa, NTSC o
  PAL).
- **juego**: cuadros nuevos que dibuja el juego por segundo (muchos juegos
  van a 20 o 30 aunque la tele vaya a 60).
- **pantalla**: cuadros que presenta la PS3 por segundo.

Es un promedio de medio segundo. Se recuerda en `menu.cfg` (`fps = si`).

## Filtros CRT

Salsa64 trae los **filtros CRT originales del autor, portados de sus
versiones de RetroMac** (sus apps para Mac y Apple TV). Son
**experimentales**: en el menú se llaman «CRT … (exp.)» y su costo en el
RSX todavía se está midiendo.

- «CRT máscara de sombra» (`crt-mascara`): un monitor con puntos de
  fósforo redondos en triángulo, con el haz más grueso en lo brillante y
  el resplandor sobre los huecos.
- «CRT cable RGB», «CRT cable S-Video» y «CRT cable compuesto»
  (`crt-rgb`, `crt-svideo`, `crt-compuesto`): una tele Trinitron chica
  conectada con ese cable. RGB: imagen limpia; S-Video: el color se corre
  un poco; compuesto: sangrado de color, dot crawl y arcoíris en los
  detalles finos.
  Con la **densidad** «Fina» (en Opciones, por juego o en «Imagen», debajo
  del filtro) los puntos son más chicos y más juntos: a 720p y 1080p, cada
  3 px (tríadas de 9 px) en lugar de cada 4 (tríadas de 12), siempre
  redondos y alineados a los píxeles: sin moaré, y con el mismo brillo.
- «CRT Trinitron» (`crt-trinitron`): una Trinitron grande de la sala (el
  Sony KV-29), sin la señal de un cable: franjas de fósforo, el haz más
  grueso en lo brillante y un poco de desenfoque a lo ancho.

Se eligen en Opciones → **Filtro** (o por juego, con SELECT sobre la
ROM), o en vivo en «Imagen» del menú de pausa; el archivo `filtro` en
`/dev_hdd0/salsa64` (la clave en la primera línea) manda sobre lo
elegido, para pruebas. Al cambiar de filtro, si preparar sus tablas tarda
más de un par de cuadros, se ve «Aplicando filtro…» (la primera vez de
cada filtro, con la comprobación en el RSX, unas decenas de ms). Trabajan
sobre la imagen a la resolución original de la N64 (la escala interna se
reduce primero con un filtro de caja, como un supermuestreo) y ocupan el
rectángulo de la imagen en la pantalla. Las capturas, las miniaturas de
los estados y los volcados siguen sin filtro.

**Con un CRT la escala interna no pasa de 3×**: si en Opciones, por juego
o en «Imagen» está fija o automática por encima de 3×, mientras haya un CRT
se usa 3× y al quitarlo vuelve el límite elegido (en una escala fija,
«3× (tope con CRT)»; en una automática se muestra la escala actual; el log
lo dice). Lo elegido menor (1× o 2×) se respeta. A 3× el
supermuestreo ya no se nota detrás del CRT y el RSX se ahorra el render y
la reducción de la imagen grande.

### Costo de los filtros

Cada filtro se mide en el RSX con marcas de tiempo entre sus pasadas (el log
lo escribe cada 5 s, «rsx: filtro …»). Medido en la PS3 real con GoldenEye
en la presa a 1080p (docs/perf-log.md), en ms del RSX por cuadro: con el
tope de 3× y la reducción en dos pasadas se presentan todos los campos con
cualquiera de los cinco; antes, a 5×, se saltaban cuadros.

| Filtro | 1080p, tope de 3× | Antes: 1080p a 5× (cuadros por segundo) | Antes: 720p a 5× |
|---|---|---|---|
| CRT máscara de sombra | 7,4 ms | 9,6 ms (51) | 6,7 ms (todos) |
| CRT cable RGB | 9,4 ms | 11,9 ms (40) | 8,4 ms (todos) |
| CRT cable S-Video | 10,6 ms | 13,0 ms (36) | 9,6 ms (51) |
| CRT cable compuesto | 13,2 ms | 15,4 ms (30) | 11,9 ms (40) |
| CRT Trinitron | 10,1 ms | — | — |

La emulación no se frena: si el RSX no alcanza, se saltan cuadros de la
pantalla. Con el tope, el render del juego también baja (de 5,6 a 2,1 ms
por campo en la presa). `filtros-spe` (la señal del cable en dos SPEs) por
ahora no ahorra: S-Video 0,45 ms menos, compuesto 0,5 ms más.

## Notas por juego

- **Conker's Bad Fur Day**: en v0.2.1 y v0.2.2 se queda en negro tras la
  pantalla «For Mature»; está arreglado en v0.2.3, que además arranca sin los
  25–30 s de negro de antes.
- **Super Smash Bros.** (y Smash Remix): en v0.2.3 y anteriores se congela
  en el primer KO; arreglado en v0.2.4, igual que las texturas estiradas del
  Castillo de Hyrule. Con esas versiones viejas, `/dev_hdd0/salsa64/fbfx.cfg`
  con un `1` adentro lo evitaba (quitarlo para los demás juegos); desde
  v0.2.4 no hace falta.
- Lista completa de lo probado en la PS3 real: [compatibilidad.md](compatibilidad.md).

## Abrir una ROM sin menú

Si existe `/dev_hdd0/salsa64/embedded`, el menú no aparece y se arranca
directo lo que diga el archivo. Para abrir una ROM propia:

```
file:mijuego.z64
```

(una sola línea, el nombre de un archivo que esté en `/dev_hdd0/salsa64`).
Con este archivo presente, «Salir al XMB» del menú de pausa (START + SELECT)
sale al XMB en lugar de volver al menú (con el cuadro «¿Salir al XMB?» si
«Confirmar al salir» está en sí).
Para recuperar el menú, borrar `embedded`.

## Lanzar una ROM desde otro programa (webMAN MOD y frontends)

Salsa64 puede arrancar una ROM sin pasar por la lista, para que un frontend
o webMAN MOD la abra desde el XMB. Hay dos formas; si llegan las dos, manda
la primera:

1. **Argumento del programa:** la ruta de la ROM como primer argumento
   (`argv[1]`), por ejemplo con `sysProcessExitSpawn2` apuntando al
   `EBOOT.BIN` de Salsa64.
2. **Archivo `rom.txt`:** si no hay argumento, Salsa64 busca
   `/dev_hdd0/game/SLSA00064/rom.txt` (el ID de título del paquete normal;
   una variante con otro ID busca en su propia carpeta). Lee la primera línea
   (hasta 250 caracteres; se quitan el salto de línea, el BOM y los espacios
   del final), **borra el archivo** y abre esa ROM. Como se borra al leerlo,
   la siguiente vez que se abra Salsa64 desde el XMB sale el menú normal.

Reglas:

- La ruta tiene que ser absoluta (`/dev_hdd0/...`, `/dev_usb000/...`, etc.) y
  terminar en `.z64`, `.n64` o `.v64`, igual que las ROMs de la lista. No hace
  falta que esté en una de las carpetas de ROMs de Salsa64.
- Si la ROM no existe, no se puede leer o la ruta no sirve, no se pierde nada:
  sale el menú normal con el aviso «No se pudo abrir <ruta>» y el motivo queda
  en `salsa64.log`.
- La línea de `salsa64.log` es `arranque directo: <ruta> (desde argv|rom.txt)`.
- La ROM se abre igual que desde el menú: ajustes por juego (`juegos/`),
  partidas, Controller Pak, estados, trucos, mapeo de controles y la última
  ROM abierta funcionan igual.
- **Al salir del juego** (menú de pausa, «Volver al menú») Salsa64 vuelve al
  menú, como siempre; ahí se puede abrir otra ROM o «Salir al XMB». «Salir del
  juego» del botón PS cierra Salsa64 directo. Si se quiere salir al XMB al
  terminar el juego, hay que usar `embedded` (sección anterior), que es otro
  modo.
- Con `embedded` presente (pruebas y arranque fijo), `argv` y `rom.txt` se
  ignoran y `rom.txt` no se borra.

Ejemplo para un script de webMAN: escribir la ruta en
`/dev_hdd0/game/SLSA00064/rom.txt` y lanzar `/dev_hdd0/game/SLSA00064/USRDIR/EBOOT.BIN`.

## Caja negra (depuración)

Para ver en la Mac lo que solo pasa en la PS3 (la interpolación de cuadros,
por ejemplo): la consola guarda en memoria lo último que hizo la HLE gráfica y
lo que se presentó en cada campo, y con un atajo lo escribe a disco.

1. Crear por FTP el archivo vacío `/dev_hdd0/salsa64/cajanegra` (o con un
   número: el tamaño del anillo en MB). Abrir el juego: el log dice «caja
   negra: encendida».
2. Cuando pase lo que se quiere estudiar, **SELECT + L2**. Sale el aviso «Caja
   negra: escribiendo cajanegra-N» y el juego sigue (la escritura es en
   segundo plano; mientras dura, el anillo no recibe nada nuevo).
3. Bajar `/dev_hdd0/salsa64/cajanegra-<campo>/caja.s4bb` por FTP.
4. En la Mac, `tools/cajanegra.py info|list|campos|extract caja.s4bb`
   (extract deja cada tarea como un `gfxdump` que lee `tools/gfxdump.py`) y
   `salsa64-run --raster-dump caja.s4bb[:TAREA] --gfx-frame imagen.ppm` dibuja
   una tarea con el rasterizador del host.

Qué guarda, el formato y cómo coordinarlo con la interpolación:
`docs/cajanegra.md`. Con el archivo apagado no hay costo alguno; encendido,
el log al cerrar el juego dice cuánto tardó el registro por tarea y por
campo.

## Si el juego parece colgado

Muchos «cuelgues» son la pantalla de error del propio juego: un hilo salta a
una dirección que no existe o lee de una desalineada, libultra lo marca como
fallado y el hilo de fallos del juego se queda leyendo el control miles de
veces por segundo (el log dice «el juego leyó el control 1 N veces» con N
enorme, 0 % en espera y 0 campos del RSX). Para verlo, el log deja una línea
cuando el CPU toma una excepción de dirección o de bus en el código del juego:

```
cpu: fallo AdEL (dirección inválida al leer o buscar una instrucción) en el campo 1440: pc 0x00010001, dirección 0x0000000000010001, ra 0x800028D4, sp 0x80081798
```

Una línea por tipo y pc. Con el pc y el ra se busca en el volcado de la
RDRAM (`salsa64-run --ram-dump`, `tools/libultra-threads.py`) qué hilo
cayó y desde dónde.

## Archivos de configuración

Todos están en `/dev_hdd0/salsa64` y son opcionales. Son archivos de texto;
se crean por FTP.

| Archivo | Formato | Qué hace |
|---|---|---|
| `menu.cfg` | Líneas `fondo = si\|no`, `fps = si\|no`, `confirmar = si\|no`, `aspecto = 4:3\|16:9`, `filtro = suave\|nitido\|lineas\|lineas+\|crt-mascara\|crt-rgb\|crt-svideo\|crt-compuesto` , `densidad = normal\|fina`, `idioma = auto\|es\|en\|fr\|de\|it\|pt\|nl\|sv\|da\|no\|fi\|pl\|tr\|ru\|ja\|ko\|zh-hant\|zh-hans` e `interpolacion = no\|geometria` | Fondo animado del menú, contador de velocidad en el juego, si «Salir» del menú de pausa pregunta antes (sí sin la línea), aspecto, filtro, idioma e interpolación. Lo escriben el menú (Opciones) y SELECT + △ |
| `juegos/<ROM>.cfg` | `aspecto`, `filtro`, `recorte`, `escala`, `densidad`, `interpolacion` = valor o `global`; `escala-estable` = `1` a `5` o `ninguna`; `accesorio` = `global`, `controller-pak`, `rumble-pak`, `transfer-pak` o `ninguno` | Ajustes de un juego (sección «Ajustes por juego»). Lo escriben el menú (SELECT sobre una ROM), «Imagen» del menú de pausa y la escala automática cuando aprende un límite estable |
| `ra.cfg` | `usuario`, `token`, `servidor`, `certificados = laxo` | RetroAchievements (sección «RetroAchievements» y `docs/retroachievements.md`). Lo escribe el menú al iniciar sesión: guarda el token, nunca la contraseña. Se activa con `retroachievements = si` y `hardcore = si` en `menu.cfg` (Opciones) |
| `descargas.cfg` | `activar = si` y direcciones | Enciende las descargas experimentales (sección «Descargas») |
| `overscan.cfg` | Uno, dos o cuatro números: `8`, `8 6` o `izq der arr abajo` | Recorte de la imagen en píxeles de la N64 (a 320 × 240) por lado. Por omisión `8 6`; `0` lo apaga; máximo 64. Lo escribe el menú (`0`, `8 6` o `16 12`) |
| `video.cfg` | `salida = 720` o `1080`; `escala = auto-N`, `auto` o `1` a `5` | Salida de video y escala interna del render de la N64 (se leen al arrancar). `auto-N` ajusta la escala sin pasar N; `auto` conserva la compatibilidad anterior (3× a 720p, 4× a 1080p). Sin archivo: 720p y `auto`. Si la tele o el XMB no aceptan 1080, usa el siguiente modo. Acepta el formato viejo de una línea (`1080` o `720`). Lo escribe el menú con «Resolución» y «Escala interna» |
| `sin-escala-auto` | Basta con que exista | Diagnóstico: impide los cambios automáticos y conserva la escala máxima elegida para comparar; no afecta a las escalas fijas |
| `controles.cfg` | Líneas `BOTON_N64 = BOTON_DS3`; `#` comenta | Mapeo del control (abajo) |
| `guardado.cfg` | Líneas `CLAVE = tipo`; `#` comenta | Tipo de guardado por juego (abajo) |
| `audio.cfg` | Líneas `clave = valor` | Audio (abajo) |
| `ultima-rom` | Ruta de la última ROM abierta | Lo escribe el menú; no hace falta tocarlo |
| `cajanegra` | Vacío, o `MB` (tamaño del anillo, 2 a 48; 12 por omisión) | Enciende la caja negra (sección «Caja negra»). Sin el archivo no cuesta nada ni reserva memoria |
| `log.cfg` | `ip` o `ip:puerto` | Destino del log por UDP, para quien depura (por omisión, difusión al puerto 18194) |
| `turbo` | Basta con que exista | Sin límite de velocidad (para medir; no es un modo de juego) |
| `capturas` | Basta con que exista | Guarda capturas de pantalla (`screen-N.ppm`) cada pocos segundos; frena la imagen un instante en cada una |
| `lle` | Vacío o `all` | Prueba el RSP de bajo nivel (experimental, lento). Vacío: solo microcódigo sin HLE; `all`: todo |
| `gfx_ppe` | Basta con que exista | Corre la HLE gráfica en el PPE en lugar del SPE (para comparar tiempos) |
| `audio-rangos-viejos` | Basta con que exista | Diagnóstico: al llenarse los diez rangos escritos por una tarea de audio, agranda siempre el último como antes; sin el archivo une los dos separados por el hueco menor para no invalidar código intacto |
| `sin-recompilador` | Basta con que exista | Apaga el recompilador dinámico: todo corre en el intérprete (más lento). No hace falta con CFW: si el recompilador no puede usar su memoria en la consola, Salsa64 pasa solo al intérprete y lo dice en el log (docs/jit-cfw.md) |
| `jit-prueba.txt` | Lo escribe Salsa64 | Marca de la prueba del recompilador al arrancar; solo queda si una prueba cerró la aplicación, y entonces esa versión usa el intérprete. Borrarlo para que vuelva a probar |
| `colgar-rsx` | `etiqueta N`, `rsx N` o `fifo N` | Prueba del seguro del RSX: cuelga a propósito al presentar el campo N (una barrera que nunca llega; el RSX girando; el RSX girando y el búfer de comandos lleno). Salsa64 debe dejar el diagnóstico a los 2 s y salir solo a los 12 s (docs/hilos-ppe.md). Borrarlo después |
| `f5-sublista-vieja` | Basta con que exista | Indiana Jones y Battle for Naboo (Factor 5): la lista secundaria de la HLE vuelve a seguir hasta el B8 … FFFFFFFF aunque antes haya otro G_ENDDL, como antes del 2026-10-29 (ejecutaba vértices como comandos: Indy invisible, media pantalla negra). Para comparar |
| `rsx-sin-blindaje` | Basta con que exista | Apaga el blindaje del RSX (docs/rsx-notes.md, Indiana Jones): vuelven a tener destino las color images que empiezan fuera de la RDRAM y las cargas de la z desde la RDRAM vuelven a mandar al RSX anchos de entrada impares. Para comparar |
| `rsx-colgado` | `reinicio` o `reinicio-directo` | Experimental: si el RSX deja de avanzar y Salsa64 sale solo (a los 12 s), después de escribir `rsx-colgado-<campo>/` reinicia la consola (por el sistema, o directo) en vez de volver al XMB, que con el RSX colgado dejó la consola congelada. Sin el archivo, vuelve al XMB |
| `rsx-colgado-<campo>/` | Lo escribe Salsa64 | Cuando el RSX deja de avanzar: `fifo.bin` (el búfer de comandos), `gfxdump.bin` (la tarea que se traducía, para `salsa64-run --raster-dump`) y, con la caja negra encendida, `caja.s4bb`. Bajarlo por FTP junto con el log |
| `sin-pausa` | Basta con que exista | El menú del sistema (botón PS) no pausa la emulación, como hasta v0.1.0 (para diagnosticar; docs/hilos-ppe.md) |
| `perfil-activo` | Basta con que exista | Enciende el perfil persistente del recompilador (apagado por omisión): cuenta las entradas a cada bloque, las guarda en `perfiles/<juego>.prf` y compila los calientes juntos. Medido en la PS3: contar cuesta ~5 % y juntarlos no gana nada (el costo lo pone cuántas líneas toca cada instrucción); sirve para recoger datos para lo que sigue (docs/rec-ppe.md) |
| `perfil.cfg` | `caliente = KB`, `saltos = si\|no` | Con `perfil-activo`: tamaño de la zona caliente de la caché de código (por omisión 1024 KB, en dos mitades; `0` cuenta y guarda el perfil sin zona caliente) y si los bloques calientes registran sus sitios de salida (sí por omisión) |
| `embedded` | `file:nombre` | Abre una ROM sin menú (sección anterior) |
| `jit-ancho` | Basta con que exista | El recompilador emite el código «ancho» de antes del 2026-10-13 (prueba de KSEG0 de 8 palabras, un camino lento por acceso), para medir el compacto contra él con el mismo paquete (docs/rec-ppe.md, «Código compacto») |
| `filtro` | Una clave: `crt-mascara`, `crt-rgb`, `crt-svideo`, `crt-compuesto`, `crt-trinitron` o `crt-mascara-fina` (la máscara con densidad fina) | Ese filtro CRT en lugar del elegido en el menú, también en las pruebas automáticas (para comparar) |
| `filtros-ciclo` | Vacío, o las claves de los filtros que se quieren (`crt-trinitron crt-compuesto`) | En el juego pasa por todos los filtros CRT (o por los de la lista), uno cada 20 s desde «Ninguno», con un aviso en pantalla; el log dice cada 5 s cuánto tarda cada uno en el RSX (para medirlos en una sola corrida). Cada vuelta cambia la reducción entre dos pasadas y una (si no la fija `filtros-reducir`), y la escala va con el tope de 3× de los CRT toda la corrida |
| `filtros-capturas` | Basta con que exista | La primera vez de cada filtro CRT y alto de pantalla guarda la imagen que entra y la que dibuja (`filtro-<clave>-<alto>-fuente.ppm` y `-pantalla.ppm`), sin congelar la imagen (las escribe otro hilo); para compararlas con `tools/crt-ref.c` |
| `filtros-reducir` | `1` o `2` | La reducción de la escala interna de los CRT en una pasada o en dos (por omisión, dos); para comparar su costo |
| `filtros-spe` | Basta con que exista | La señal y la decodificación de «CRT S-Video» y «CRT compuesto» en dos SPEs en lugar del RSX (experimental); con `filtros-ciclo`, cada vuelta las alterna entre los SPEs y el RSX (y la reducción queda en dos pasadas), para comparar en la misma corrida. Si un SPE falla, vuelven al RSX y el log lo dice |
| `capturas-pantalla` | Basta con que exista | Con `capturas`: además guarda la pantalla entera como se ve, con el filtro (`pantalla-N.ppm`) |
| `interp-prueba` | Vacío o un número entre 0 y 1 (por omisión 1) | Prueba de la interpolación: cada tarea gráfica se empareja con la anterior y se dibuja en esa fracción entre las dos (0: un cuadro atrás; 1: como siempre), sin campos intermedios y pase lo que pase con la opción; para aislar el emparejamiento de la presentación. El log de cada 5 s trae «gfx: interpolación» |
| `filtro-ver` | El nombre de una tabla y una escala (`puntos 0.5`) | Diagnóstico de los filtros: dibuja esa tabla intermedia en la esquina de la pantalla (docs/filtros.md) |
| `jit-sin-grupos` | Basta con que exista | El recompilador no agrupa los accesos a memoria por registro base (la «base en caché», docs/fastmem.md §8, encendida por omisión): cada acceso con su prueba, como hasta el 2026-10-13, para medir con el mismo paquete |
| `jit-fpu-en-linea` | Basta con que exista | El recompilador emite las operaciones de la FPU en línea en cada sitio, como hasta el 2026-10-13, en vez de llamar a las rutinas compartidas del preámbulo (la «FPU compacta», docs/fastmem.md §9, encendida por omisión), para medir con el mismo paquete |
| `jit-sin-destinos` | Basta con que exista | Apaga la caché de destinos de JR/JALR de `exit_jr` (encendida por omisión desde 2026-10-14; docs/perf-log.md), para el A/B con el mismo paquete |
| `jit-destinos-cuentas` | Basta con que exista | Con la caché de destinos, cuenta aciertos y fallos y los escribe en el log de estadísticas del recompilador («caché de destinos de JR: N aciertos, M fallos»); suma unas palabras en cada JR, no se usa para medir tiempos |
| `jit-fastmem` | Basta con que exista | Experimental: la RDRAM en una ventana de 512 MB de sys_mmapper y los accesos de KSEG0/KSEG1 sin prueba; solo se enciende si lv2 avisa de los fallos de página (docs/fastmem.md); hoy en la PS3 con HEN no lo hace y queda apagado con el motivo en el log |
| `perfilado.cfg` | Líneas `clave = valor` | Solo la app «Salsa64 perfil»: qué registra cada sesión de juego en `perfilado/` y con qué tope (docs/perfilado.md). Sin archivo, todo encendido y 200 MB |
| `muestreo` | Basta con que exista | Muestreo por tiempo del recompilador: un hilo de baja prioridad mira cada 200 µs qué bloque corre el hilo de emulación (o si está en C: camino lento, intérprete, eventos…); cada 5 s el reparto al log y, al salir del juego, los 30 bloques con más tiempo con sus palabras de PPC (docs/rec-ppe.md, «Muestreo por tiempo»). Costo: dos palabras por entrada a bloque |
| `aristas-aa` | Basta con que exista | Experimental: la pasada de aristas de los dibujos con antialiasing, líneas que tapan las grietas de las uniones en T (las líneas azules del piso del túnel de Dam en GoldenEye). Apagada por omisión: a escala alta hace que los polígonos de los modelos se superpongan en las uniones (docs/rsx-notes.md) |
| `sin-alfa-destinos` | Basta con que exista | Apaga el canal de alfa de los destinos de render a textura de 16 bits (transparencia de lo que el juego dibuja en una imagen y usa después como textura: Mario Tennis, logos de Rogue Squadron). Encendido por omisión; para comparar |
| `s2dex-paso-viejo` | Basta con que exista | Worms Armageddon (#57): los fondos de S2DEX vuelven a avanzar las filas de la imagen por `imageW` entero en vez de redondearlo a palabras de 64 bits como el microcódigo; la imagen vuelve a salir inclinada. Para comparar |
| `copia-sin-alfa` | Basta con que exista | LEGO Racers (#34): las copias a la RDRAM de los destinos de render a textura con alfa vuelven a hacerse en RGB565 con el bit de alfa en 1 (los autos lejanos salen dentro de un cuadro cuando su imagen se lee de la RDRAM). Sin el archivo, el bit lleva el alfa del destino, como el RDP. Para comparar |
| `alfa-fragmento` | Basta con que exista | Dr. Mario 64 (#62): los dibujos vuelven a dejar en los destinos de render a textura con alfa el alfa del fragmento, como hasta la v0.2.15 (los paneles translúcidos del fondo de la partida salen en blanco). Sin el archivo dejan el bit que deja el RDP, la cobertura según el modo de cada dibujo (casi siempre 1). Para comparar |
| `zclear-inmediato` | Basta con que exista | LEGO Racers: restaura el borrado de profundidad inmediato anterior, que podía recortarse al destino auxiliar de 32/64 píxeles. Sin el archivo, el borrado espera al destino que le sigue. |

La carpeta también puede tener `covers/` (carátulas), `cheats/` (trucos y
sus `.on`), `juegos/` (ajustes por juego), `states/` (estados guardados), `perfiles/` (el
perfil del recompilador de cada juego, `<juego>.prf`, solo con
`perfil-activo`; no contiene nada del juego, solo direcciones, huellas y
cuentas, y se puede borrar), `perfilado/` (las sesiones de la app «Salsa64
perfil», docs/perfilado.md: una carpeta por juego con un archivo por
partida, sin nada del juego salvo los volcados de `volcados/`; se rota sola
con un tope de 200 MB), el log (`salsa64.log`) y archivos de diagnóstico
que escribe la aplicación (`gfxdump.bin`, `screen-N.ppm`, `menu.ppm`);
no hace falta tocarlos.

### `controles.cfg`

```
# comentario
Z = L2 R2
C_ABAJO = CIRCULO
zona_muerta = 12
```

- Cada botón de la N64 acepta uno o dos botones del DualShock 3, separados
  por espacios o una coma: `Z = L2 R2` deja Z en los dos gatillos.

- N64: `A B Z START L R ARRIBA ABAJO IZQUIERDA DERECHA C_ARRIBA C_ABAJO
  C_IZQUIERDA C_DERECHA`.
- DualShock 3: `CRUZ CIRCULO CUADRADO TRIANGULO L1 R1 L2 R2 L3 R3 START SELECT
  ARRIBA ABAJO IZQUIERDA DERECHA NADA`.
- Ajustes del stick: `zona_muerta` (0 a 100), `rango` (1 a 127), `umbral_c`
  (1 a 127).
- Mayúsculas o minúsculas da igual. Las líneas que no se entienden se
  ignoran (y se anotan en el log).

### `guardado.cfg`

Una línea por juego; gana la más específica.

```
NZL = sram256k        # código de 3 caracteres: todas las regiones
NK4E = eeprom16k      # código de 4: una región
EC7011B7 = ninguno    # CRC1 de la cabecera (8 dígitos hex): una ROM
* = eeprom4k          # cualquier ROM
```

Tipos: `ninguno`, `eeprom4k`, `eeprom16k`, `sram256k` (o `sram`) y
`flashram`. Los demás (`sram768k`, `sram1m`) se reconocen pero no se
emulan.

### `audio.cfg`

| Clave | Valores | Efecto |
|---|---|---|
| `politica` | `estirar` (por omisión) o `silencio` | Qué hacer con el audio si la emulación va lenta |
| `nivel_ms` | número (por omisión 60) | Colchón mínimo de audio |
| `hle` | `spe` (por omisión), `ppe` o `no` | Dónde corre la emulación del audio |
| `salida` | `si` (por omisión) o `no` | Abrir o no el puerto de audio |

Hay otras dos claves de diagnóstico (`tarea_us`, `volcado_s`) en
`src/frontend/audio_out.h`.
