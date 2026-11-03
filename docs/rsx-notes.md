# Notas del RSX

Hallazgos sobre el RSX en homebrew (documentación escasa: anotar todo aquí).

## Shaders y cgcomp

- `cgcomp` (PSL1GHT) compila Cg con `libCg` a ensamblador NV40 (vp40/fp40) y
  luego lo ensambla a microcódigo del RSX con su propio parser
  (`tools/cgcomp/source/compilerfp.cpp`, `compilervp.cpp`, `fpparser.cpp`).
- `cgcomp -a` compila **desde ensamblador**, sin `libCg`. Ese ensamblador es
  código C++ autocontenido: candidato a portarse a la consola para generar
  fragment programs de combiners en tiempo de ejecución (Fase 4). Revisar
  antes su licencia (PSL1GHT es MIT) y dependencias.
- Los fragment programs se ejecutan desde memoria del RSX: copiar el
  microcódigo a un búfer de `rsxMemalign` y pasar su offset a
  `rsxLoadFragmentProgramLocation`. Los vertex programs se suben por el búfer
  de comandos (`rsxLoadVertexProgram`).
- Entradas del vertex program: `POSITION` → atributo 0, `COLOR` → atributo 3
  (`GCM_VERTEX_ATTRIB_COLOR0`). Si se enlazan 3 componentes a un `float4`,
  `w` vale 1.
- **`cgcomp` quita la corrección de perspectiva en MUL, DP2/3/4, DIV y NRM3**
  (`fpparser.cpp: IsPCDisablingInstruction`): pone el bit 31 de SRC2 de la
  instrucción, y la entrada (varying) que lee llega dividida entre w. Con
  w = 1 no se nota; con los w de un juego de N64 (≈ 2000 para la luna del
  intro de Zelda) el término vale casi cero. RPCS3 lo emula así
  (`tcN * gl_FragCoord.w` en el GLSL que registra con *Log shader
  programs*). rsxkit limpia el bit al subir cada fragment program
  (`lib/rsxkit/src/fprog.c`); gfxtest lo cubre con un cuadro de w = 1000.
  El generador de fragment programs de la Fase 4 debe dejar el bit en 0.
  Formato de cada instrucción: 4 palabras de 32 bits con las mitades de 16
  bits intercambiadas; si alguna fuente es constante (tipo 2), los 16 bytes
  siguientes son su valor.
- RPCS3 compila los shaders en segundo plano: la primera corrida después de
  cambiar un fragment program puede saltarse dibujos (pantalla negra en la
  captura). Repetir antes de concluir.

## Microcódigo del fragment program (generador de lib/rsxkit, 2026-10-10)

Lo que hace falta para emitir un fragment program sin cgcomp (fuentes:
`nvfx_shader.h` y `compilerfp.cpp` de cgcomp, MIT, y los `.fpo` que
compila; `lib/rsxkit/src/fpgen.c`):

- Cuatro palabras de 32 bits por instrucción. En memoria, cada palabra va
  con sus mitades de 16 bits intercambiadas (`endian_fp` de cgcomp:
  escribe `SWAP32(palabra)` y luego intercambia mitades, así que un PPU
  big-endian guarda `(w << 16) | (w >> 16)`). Si una fuente es constante
  (tipo 2), los 16 bytes siguientes son sus cuatro floats, con el mismo
  intercambio.
- Palabra 0: bit 0 fin del programa; bits 1–6 registro destino; bit 7
  destino H (media precisión; H2k y H2k+1 son las mitades de Rk); bits
  9–12 máscara xyzw; bits 13–16 **el único registro de entrada que puede
  leer la instrucción** (COL0 = 1, TEXCOORDn = 4 + n; las fuentes de tipo
  1 lo toman de aquí: dos entradas distintas en una instrucción no
  caben, hay que pasar una por MOV a un temporal); bits 17–20 unidad de
  textura (TEX); bits 22–23 precisión (0 fp32, 1 fp16); bits 24–29 opcode
  (MOV 1, MUL 2, ADD 3, MAD 4, TEX 0x17); bit 31 saturar.
- Palabras 1–3 (fuentes 0–2): bits 0–1 tipo (0 temporal, 1 entrada, 2
  constante); 2–7 índice del temporal; 8 temporal H; 9–16 swizzle (dos
  bits por componente: identidad 0xE4, `.wwww` 0xFF, `.zwzw` 0xEE); 17
  negar. cgcomp pone en toda instrucción, en la palabra 1, la condición
  «siempre» (7 en los bits 18–20) con swizzle identidad (bits 21–28), y
  en la palabra 3, cuando lee un TEXCOORD, 0x7FC en los bits 19–29. El
  bit 31 de la palabra 3 quita la corrección de perspectiva (arriba).
  Una fuente que no se usa va como temporal 0 con swizzle identidad.
- `result.color` es R0 (precisión completa, bit 7 en 0). cgcomp hace las
  operaciones en fp16 (precisión 1, temporales H) y en la última escribe
  R0 con precisión 1 y saturación; TEX va con precisión 0.
- Lo que `rsxLoadFragmentProgramLocation` necesita además del offset:
  `num_regs` (registros R; mínimo 2), `fp_control` (0x40 = escribe el
  color; se le suma `num_regs << 24` y el bit 10), y las máscaras
  `texcoords`/`texcoord2D`/`texcoord3D` (un `NV40TCL_TEX_COORD_CONTROL(i)`
  por TEXCOORD leído: bit 4 si es de 3 o 4 componentes, bit 0 si de 2).
  rsxkit arma un `rsxFragmentProgram` con solo esos campos.
- Verificación sin consola: `tests/unit/test_fpgen.c` interpreta el
  microcódigo (TEX, MOV, MUL, ADD, MAD, swizzle, H/R, saturación) y
  comprueba primero que los `.fpo` de cgcomp (`tests/unit/fpgen_ref.h`,
  generado por `tools/fpgen-ref.sh`) dan la fórmula de su shader y luego
  que los programas generados dan la misma. Los de referencia leen las
  mismas entradas que los generados equivalentes (`texcoords` igual).
- Orientación: con la y hacia arriba en el espacio de recorte, el frente de
  la N64 es antihorario (`rsxSetFrontFace(ctx, GCM_FRONTFACE_CCW)`). Con CW
  el culling sale al revés (faltaban el cielo y la luna en Zelda).
- Profundidad: `rsxSetZMinMaxControl(ctx, FALSE, TRUE, FALSE)` (sin recorte
  en z cercano/lejano por hardware; la HLE ya recortó) y z/w en [0, 1].
- **Escribir la z sin probarla** (Z_UPD sin Z_CMP en la N64): en el RSX, como
  en GL, la prueba apagada apaga también la escritura. Va con la prueba
  encendida y `GCM_ALWAYS` (`set_depth` en `lib/rsxkit/src/draw3d.c`).
  Comprobado en RPCS3 con un volcado editado de Rogue Squadron (la nave con
  escritura sin prueba y el piso detrás con prueba: antes el piso la tapaba
  entera, ahora queda encima); RPCS3 sigue al hardware en esto.
- **Cargar una profundidad desde la memoria principal**: Z24S8 lineal es,
  para el motor 2D, una superficie A8R8G8B8 (profundidad en los 24 bits
  altos, como la ve la CPU). `rsxkit_depth_load` la llena con el motor de
  imagen escalada (`rsxSetTransferScaleSurface`, `SRCCOPY`, sin filtro,
  origen en el centro, razón redondeada hacia arriba para que 1/3 no caiga
  en el texel anterior), en tramos de hasta 1024 píxeles de salida y entre
  dos `rsxSetWaitForIdle`. Sin validar aún en la PS3 real.

## Framebuffer

- Superficie X8R8G8B8 en memoria local del RSX; el PPE puede leerla por su
  dirección mapeada (lento; solo para pruebas).
- **RPCS3: para leer el framebuffer desde el PPE hay que activar "Write Color
  Buffers"** (configuración de GPU); si no, la memoria emulada no recibe el
  render y la verificación de `rsx-triangle` falla aunque se vea bien.

## Tijeras fuera del destino (F-Zero X a 5×, 2026-10-14)

El RDP acepta tijeras con la esquina izquierda más allá del borde (x0 >
x1: no dibuja nada); F-Zero X las manda en «SELECT COURSE» para las copas
fuera de la pantalla (x0 = 1023,75 con x1 = 308). rsxkit solo recortaba x1
e y1 al destino, así que a 5× mandaba `rsxSetScissor` con x = 5119, por
encima del máximo de 4095 de las superficies del RSX. La PS3 real se colgó
en esa pantalla (el PPE esperando al RSX); RPCS3 lo acepta sin decir nada.
Desde entonces rsxkit recorta las cuatro coordenadas y no manda los
dibujos con tijeras vacías. Que esa fuera la causa está por confirmar en la
consola (con el seguro de las esperas, docs/hilos-ppe.md, un cuelgue así
deja GET y PUT en el log).

## Flags de segmentos del ELF (lv2.ld)

Además de R/W/X, `lv2.ld` usa bits altos en `p_flags` para acceso del SPU y
del RSX: 0x00200000/0x00400000 (SPU escritura/lectura) y
0x02000000/0x04000000 (RSX escritura/lectura).

## Texturas (lib/rsxkit, 2026-10-10)

- `rsxLoadTexture` de PSL1GHT arma el registro de formato con
  `format << 8 | 0x8000`: el bit 0x80 de los formatos de Sony
  (`CELL_GCM_TEXTURE_*`) lo pone la biblioteca, así que los valores
  `GCM_TEXTURE_FORMAT_*` de PSL1GHT van sin él (A8R8G8B8 = 5 → 0x85).
- El formato 23 (0x97 con el bit) es **R5G5B5A1**, idéntico al RGBA5551 de
  la N64 (R en los bits altos, alfa en el bit 0). PSL1GHT lo llama, por
  error, `GCM_TEXTURE_FORMAT_R5G6B5A1`. Con él, el framebuffer de 16 bits
  de la N64 se muestra sin convertir en el PPE.
- Las texturas de 16 y 32 bits se leen en el orden de bytes de la PS3
  (big-endian): un framebuffer RGBA8888 de la N64 (bytes R, G, B, A) leído
  como A8R8G8B8 deja R en el canal "A", G en "R" y B en "G"; el remap lo
  corrige: `GCM_TEXTURE_REMAP_MODE(XYXY, B, A, R, G, ONE, REMAP, REMAP,
  REMAP)` (el primer grupo dice de qué canal sale cada salida A, R, G, B;
  el segundo, si se copia, vale 0 o vale 1).
- Texturas lineales (`GCM_TEXTURE_FORMAT_LIN`) de tamaño que no es potencia
  de dos, con coordenadas normalizadas y `CLAMP_TO_EDGE`: funcionan (320×237).
  Pitch usado: múltiplo de 64 bytes; offset alineado a 128.
- Tras reescribir una textura hay que invalidar la caché de texturas
  (`rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE)`) antes de dibujar.
- Modo inmediato: `rsxDrawVertexBegin` + `rsxDrawVertex2f(TEX0)` +
  `rsxDrawVertex4f(POS)` + `rsxDrawVertexEnd`; escribir el atributo 0
  (posición) emite el vértice, así que va al final. Los arreglos de vértices
  se desactivan con `rsxBindVertexArrayAttrib(..., elems = 0, ...)`.
- Etiquetas: `rsxSetWriteBackendLabel` después de un dibujo se escribe cuando
  el RSX termina ese dibujo; sirve para saber cuándo se puede reescribir
  una textura. Antes de esperar una etiqueta hay que vaciar el búfer de
  comandos (`rsxFlushBuffer`), o la espera no termina.

Verificado en RPCS3 0.0.43 y en la **PS3 real** (2026-10-10, CECH-3011A,
HEN): el frontend con vitest lee la pantalla del RSX y RGBA5551 y RGBA8888
salen con el color esperado a 1280×720. Subir la textura y dibujar cuesta
0,2–0,4 ms por cuadro en el PPE.

## Trampas de PSL1GHT (f649a08, 2026-10-10)

- **`rsxSetVertexProgramConstants` está roto:** avanza `context->current` a
  mano mientras escribe y después otra vez con `RSX_CONTEXT_CURRENT_END`, así
  que deja palabras sin escribir en el FIFO (RPCS3: "FIFO error: possible
  desync event" y "Dead FIFO commands queue"); además su `count` son floats,
  no vec4, y en el bucle de 32 floats copia dos veces el mismo bloque. Usar
  `rsxLoadVertexProgramParameterBlock(ctx, base, n_vec4, datos)`, que parte en
  bloques de 8 vec4 bien.
- `rsxLoadVertexProgram` sube también las constantes internas que pone
  `cgcomp` (literales como 2.0): no hace falta subirlas a mano.
- `cgcomp` asigna las constantes del vertex program en orden de declaración
  y consecutivas (un `uniform float4 k[15]` queda como `k[0]`…`k[14]`), así
  que se pueden subir de una vez.

## Mezcla alfa dentro de una sola llamada de dibujo (RPCS3, 2026-10-10)

En RPCS3 (0.0.43, renderizador de la Mac), los triángulos translúcidos de
**una misma** `rsxDrawVertexArray` no se mezclan con lo que dibujaron los
triángulos anteriores de esa llamada: el destino que ven es el de antes de
la llamada. Con el menú en un solo lote (fondo opaco y encima barras,
paneles, ondas y sombras con alfa) todo lo translúcido salía como fuente ×
alfa sobre negro (panel (0, 0, 0), sombras como cajas negras, ondas
oscuras); con el fondo en una llamada aparte, la mezcla sobre él salía bien.
Entre llamadas distintas el orden se respeta (gfxtest comprueba una mezcla
al 50 % y pasa).

El RSX real mezcla en el orden de las primitivas, como cualquier GPU; esto
es de RPCS3. Para que se vea igual en los dos, `lib/rsxkit/ui.h` manda una
llamada de dibujo por elemento (rectángulo, texto, láminas y líneas de cada
onda: `rsxkit_ui_groups`); con el estado en caché, cada llamada son unas
pocas palabras del búfer de comandos. Quien junte en un solo lote
geometría translúcida que se superpone debería hacer lo mismo si quiere
verla bien en RPCS3.

## Cobertura con AA_EN: grietas de las uniones en T

Con AA_EN el RDP escribe un píxel si cualquiera de sus 8 muestras de
cobertura cae dentro del triángulo; el RSX solo mira el centro. En las
uniones en T (un vértice que queda una fracción de píxel fuera de la arista
del vecino) se abren grietas por las que se ve lo de atrás: las «líneas
azules» del túnel de Dam en GoldenEye son el borrado azul que asoma por
ellas (medido con `salsa64-run --ares` en dos volcados de ares: grietas de
0,26 a ~1,1 px entre dibujos del piso y de la orilla, G_RM_AA_ZB_OPA_SURF;
la consola no las tiene).

- `POLYGON_SMOOTH_ENABLE` (método 0x1838 del NV40,
  `rsxSetPolygonSmoothEnable`), 2026-10-12: en la PS3 real no cambia nada;
  RPCS3 también lo ignora.
- Ensanchar los triángulos en la HLE (`aa_grow`, 9c2ee06): tapaba las
  grietas en la PS3 real, pero a escala interna alta engordaba todos los
  bordes y los polígonos se encimaban (Zelda, los modelos de GoldenEye); se
  quitó en 4e35681.
- **Aristas como líneas** (2026-10-13, `rsxkit_draw.edges`): en los dibujos
  opacos con AA_EN (sin calcomanías), antes de los triángulos se dibujan
  sus aristas con el modo de polígono LINE y el mismo estado; los
  triángulos pintan encima de lo que cubren y las líneas quedan solo en
  las grietas. Dos detalles medidos en el rasterizador del host, que hace
  lo mismo (`raster.c`, `draw_edges`): la z de un píxel de línea es la del
  punto de la arista, no la del centro del píxel, y con LEQUAL le ganaba
  al relleno en las tiras finas (las lámparas del túnel salían oscuras):
  las líneas van con desplazamiento de polígono hacia atrás
  (`rsxSetPolygonOffsetLineEnable`). Con factor 1 y 2 unidades todavía le
  ganaban a piezas vecinas casi coplanares ya dibujadas (se comían el
  blanco del ojo de Banjo); con factor 8 y 64 unidades no, y las grietas
  se siguen tapando porque lo que hay detrás está lejos (en una escena de
  Banjo los píxeles que cambian bajan de 2263 a 1269). Y el ancho: con 1 px
  quedaban tiras de ~1 px entre aristas casi paralelas (32 de 170 píxeles
  en el segundo volcado); con 1,5 px de la N64 no queda ninguna en los dos
  volcados, a costa de 0,1–0,2 dB por bordes algo más gruesos. El ancho va
  en píxeles de la N64 (1,5 × la escala de la superficie, en el método
  0x3B8 del NV40, punto fijo 6.3, que PSL1GHT no expone), así que tapa lo
  mismo a cualquier escala interna. GoldenEye lleva aristas en 54 de sus
  72 dibujos por cuadro.
- **En la PS3 real (paquete «Salsa64 aristas 2», 2026-10-13): regresión.**
  En los modelos «los polígonos superan su límite y se superponen,
  generando picos en todas las uniones», lo mismo que con `aa_grow`. A
  escala interna alta 1,5 px de la N64 son ~7,5 px de la superficie a 5×,
  y las líneas sobresalen de los vértices y engordan las siluetas de los
  modelos con AA; el host a 1× no lo mostraba. Queda apagada por omisión
  (`aristas-aa` en `dev_hdd0/salsa64` la enciende; `SALSA64_AA_EDGES=1` en
  el host) para seguir probando alternativas que no toquen las siluetas.

## Fragment programs de posprocesado con cgcomp (2026-10-13, filtros CRT)

Lo que hizo falta saber para escribir fragment programs largos en Cg
(rsxkit/post.h y src/frontend/filters):

- **cgcomp llama a Cg con `-O3 -fastmath -fastprecision -unroll count=8
  -ifcvt all`** (se ve con `cgcomp -d`, que deja el ensamblador NV40 en
  `<salida>.dump`). Con `-fastprecision`, Cg baja a fp16 (registros H) lo
  que cree que lo aguanta, también un `floor` de coordenadas en píxeles:
  con 11 bits de mantisa, 359,67 queda en 359,75. Para cuentas en texeles o
  píxeles de pantalla se compila con **`-Wcg,-nofastprecision`**: todo
  `float` va en fp32 y lo que se quiera en fp16 se declara `half`. Solo
  cabe un `-Wcg,…` (dos opciones separadas por coma dentro de él tiran a
  cgcomp con una violación de segmento). Los shaders de la combinación
  lineal del 3D siguen sin la opción (sus cuentas son colores).
- Los bucles de largo fijo se desenrollan aunque pasen de 8 vueltas (12
  lecturas del resplandor, 27 muestras del cable). Un arreglo de uniforms
  solo se puede indexar con la variable del bucle: `w[j + 1]` no compila
  («Only arrays of texcoords may be indexed»); se usan dos arreglos.
- `#include "x.cgh"` funciona, relativo al archivo que incluye: variantes
  de un mismo programa con `#define` y un include.
- **cgcomp no sabe de varias salidas** (MRT): un `out float4 c1 : COLOR1`
  compila, pero el ensamblador de cgcomp escribe `result.color[1]` en R0,
  encima del color 0. Una tabla que necesita 6 valores por texel va en dos
  pasadas (o dos superficies de fp16).
- **Nada de `?:`, `sign()` ni comparaciones que elijan un valor**: Cg
  las compila con códigos de condición (`MOVRC RC`, un `MOVR R1(LT)`
  condicional) y el ensamblador de cgcomp no los codifica bien: en la
  pasada de puntos del CRT, el signo del erf salía mal y la energía de
  cada punto 1,8× a 5× más alta (RPCS3, 2026-10-13). `step()` (SGE) y
  `lerp` con un 0/1 dan lo mismo sin condiciones. `tools/fpo-info.py
  --sin-condiciones` las detecta en el .fpo (bit 8 de la palabra 0, o una
  condición distinta de «siempre» en los bits 18–20 de la palabra 1), y el
  build de los filtros falla si aparecen (`FPO_CHECK` en mk/ps3.mk).
- **La salida choca con R0.** En el ensamblador que Cg le da a cgcomp,
  `oCol` (result.color) es un registro aparte, pero cgcomp lo escribe en
  R0, que Cg también usa como temporal. Cuando Cg escribe el color por
  componentes y después lee R0 creyendo que tiene otra cosa, el programa
  hace otra cuenta: en la pasada de puntos del CRT, `oCol.x = A` pisaba la
  energía que leían las instrucciones siguientes (B y la energía salían
  ×4,6 en RPCS3). No hay forma de evitarlo desde el Cg (lo reordena igual):
  `tools/cg-salida.py` lo detecta componente por componente en el volcado
  (`cgcomp -d`) y lo corrige mandando las escrituras de oCol a un temporal
  nuevo y copiándolo con un `MOVR oCol` al final; `cgcomp -a` ensambla ese
  texto (con los nombres de las constantes: sin cambios, sale el mismo
  .fpo). Los shaders de rsxkit no tienen el choque (revisados).
- **Valor absoluto con un componente**: el ensamblador de cgcomp codifica
  `-|R0.z|` con el swizzle `.zxxx` (completa con x en vez de repetir el
  componente; con `-R0.z` sin las barras lo hace bien). En la pasada de
  líneas del compuesto, el peso de la segunda tabla en G y B salía de la
  columna y no de la fase: tinte rojo en toda la imagen (lo mostró el GLSL
  que registra RPCS3 con «Log shader programs», `abs(r0.zxxx)`).
  `tools/cg-salida.py --corrige` lo escribe como `-|R0.zzzz|` antes de
  reensamblar; `tools/fpo-info.py -v` muestra swizzles y barras de cada
  fuente del .fpo.
- **cgcomp llama a Cg en silencio** (`-q`): sus avisos no se ven. Uno
  importa: con un escalar como primer argumento, `lerp(1.0, v, k)` elige
  `lerp(float, float, float)` y convierte el vector a escalar (se queda con
  la x; aviso C7011). En la máscara de franjas del CRT con cable los tres
  canales salían con la franja del rojo. Escribir los argumentos de las
  funciones como vectores del mismo tamaño (`float3(1.0, 1.0, 1.0)`).
- Cg traduce `exp(x)` como `pow(e, x)`, que cgcomp expande a LG2 + MUL +
  EX2 por componente: usar `exp2(x · log2 e)`. `sqrt` sale como RSQ + RCP
  (sqrt(0) = 0). LG2, EX2, RCP y RSQ son escalares: `pow` de un `float3`
  son 9 instrucciones.
- Cada lectura de textura necesita su coordenada en un registro: con la
  coordenada armada a mano (`float2(x + j, y)`) Cg agrega dos MOV por
  lectura. Juntar dos coordenadas en un `float4` (`b + float4(j, 0, j, 0)`
  y leer `.xy` y `.zw`) deja una suma por cada dos lecturas.
- Las constantes (uniforms) del fragment program van dentro del
  microcódigo, una copia en cada instrucción que las lee; el .fpo trae la
  tabla de lugares (`rsxConstOffsetTable`) y rsxkit las parchea en su copia
  del programa (`rsxkit_program_set`). Un programa ya parcheado se vuelve a
  leer al ligarlo de nuevo (`rsxLoadFragmentProgramLocation`).
- **Programas largos (un fondo de prueba del menú con raymarching, 2026-10-27; no quedó).** cgcomp revienta
  («munmap_chunk(): invalid pointer») con un programa de ~1300
  instrucciones (100 pasos de raymarching desenrollados); con ~330 anda.
  La marcha del fondo va en cuatro pasadas de 25 pasos que se pasan la t
  por una superficie fp16. Y Cg adelanta todo lo que no depende de lo
  anterior (las 32 lecturas de textura, sus comparaciones y productos de
  un mínimo sobre 32 muestras) y se queda sin registros («Temporary
  register limit of 32 exceeded»): hacer que cada coordenada dependa del
  resultado anterior con algo que vale 0 pero el compilador no puede
  probar (`saturate(mn + mx − 10⁹)`) deja el programa en 8 registros a
  cambio de dos sumas por muestra.
- **Una sola entrada (TEXCOORD) y una sola constante por instrucción.** Cg
  agrega un MOV cuando una instrucción lee dos atributos o dos uniforms
  distintos (las dos de un mismo float4 con distinto swizzle cuentan como
  una). Lo que es lineal en la pantalla conviene pasarlo como constante
  afín de la pasada (`rsxkit_post_pass.kx`/`ky`): llega interpolado y no
  cuesta instrucciones.
- `tools/fpo-info.py` resume un .fpo: instrucciones, lecturas, precisión de
  cada operación y cuántas copias tiene cada constante.

Superficies y texturas de posprocesado: render a superficies lineales
`GCM_SURFACE_F_W16Z16Y16X16` (4 × fp16, 8 bytes por píxel) y
`A8R8G8B8`, leídas como textura (`W16_Z16_Y16_X16_FLOAT` y `A8R8G8B8`) con
coordenadas en texeles (`GCM_TEXTURE_FORMAT_UNRM`, centro del texel i en
i + 0,5) y `GCM_TEXTURE_BORDER` con color 0 para leer negro fuera de la
imagen. La profundidad ligada es la de la pantalla (prueba y escritura
apagadas): ninguna superficie puede ser más grande que ella. Marcas de
tiempo del RSX: `rsxSetTimeStamp(ctx, i)` y `gcmGetTimeStamp(i)` (reportes
2048 − 64 a 2047 en rsxkit). Pendiente de confirmar en la PS3 real:
filtrado bilineal de las texturas fp16, el modo BORDER y el orden de los
canales de fp16 (RPCS3 los acepta).

## Fuente del sistema con libfont (2026-10-26)

El texto del 2D puede salir con la fuente del XMB (libfont + FreeType del
firmware) en una caché de glifos del atlas de rsxkit. API, juegos de fuentes,
trampas (callbacks por descriptor de 32 bits, `fontSetScalePixel` mal
declarada en PSL1GHT, calibración de la línea base) y qué falta medir en la
PS3: `docs/fuente-sistema.md`.

## Fondo «Seda»: constantes en las mallas (2026-10-27)

`rsxkit_post_pass.k` (TEXCOORD1..7) solo existe en las pasadas de pantalla
(`rsxkit_post_run`): el vertex program de las mallas pasa nada más
TEXCOORD0, y las constantes `uniform` de un programa se parchean en su copia
del microcódigo (`rsxkit_program_set`), lo que obliga a esperar al RSX si el
programa está en uso. Para un color que cambia con la hora, el fondo «Seda»
lo lleva en una rampa de 16 texeles RGBA8 en memoria principal (una por
capa, triple búfer) que la malla lee con el tono interpolado; más barato
que parchear y sin esperas. Detalles en `docs/fondo-seda.md`.

## Video: lo que deja el XMB y las renegociaciones del monitor (2026-10-27)

Medido en una PS3 con un monitor ASUS ProArt por HDMI (a 1080p), con el
registro de `rsxkit_init` («video: antes de configurar…»):

- **La salida llega «desactivada»** (`videoGetState`: estado 0) al arrancar la
  aplicación, aunque el XMB ya esté en el mismo modo. Sin `videoConfigure` no
  se activa: la imagen salió con bloques revueltos (probado sin llamarlo).
- **`videoGetConfiguration` da la configuración del XMB**: resolución 1080,
  formato XRGB y **pitch 8192**, no ancho × 4 (7680). Con `videoConfigure` y
  nuestro pitch de 7680 el monitor renegociaba el HDMI una segunda vez.
- **Una sola renegociación** si `videoConfigure` lleva exactamente los
  valores vigentes (formato, aspecto, pitch) y los búferes de pantalla usan
  ese pitch (`rsxkit_init` lo hace solo cuando la resolución vigente es la
  elegida). Queda la que hace el sistema al salir del XMB, que no se puede
  evitar. En el ProArt la intro se ve entera sin esperar (se probó con
  `intro-espera` de 750, 400 y 0 ms); la secuencia de arranque
  (`src/frontend/intro.c`) no espera por omisión, y `intro-espera` (ms desde el
  `videoConfigure`) sirve a una tele más lenta.
- Con otra resolución que la vigente, `videoConfigure` va como siempre
  (pitch = ancho × 4). «Aplicar ahora» (`rsxkit_video_reconfigure`) también:
  los búferes vuelven a ancho × 4. Todo lo demás lee el pitch de la API
  (`rsxkit_back_buffer`, `rsxkit_screen_texture`, `rsxkit_bind_frame`).
- Regiones de mosaico (tiles) del sistema: el registro las lista; no se
  quitan.

## Copias a 1:1 con filtro lineal: puntos en las capas de 8 bits (2026-10-28)

`rsxkit_target_read` copiaba siempre con filtro lineal (reduce 3× → 1×). Con
una superficie a 1× (las capas de fondo de 8 bits de Yoshi's Story, 336 × 256)
la copia es 1:1 y el lineal no es exacto en el RSX: el vértice se ajusta a su
subpíxel y la muestra cae a una fracción de texel del centro, así que el rojo
sale mezclado con el vecino. En RGB es invisible; en una capa cuyo rojo es un
índice de paleta, un índice ± 1 es otro color: puntos sueltos en cada borde de
la capa (y una cuadrícula en las zonas planas, en las esquinas de los tiles).
Reproducido con las cajas negras de la consola: las tiras de 336 × 6 que la
HLE decodificó de la RDRAM ya traen los puntos (`build/yoshi-rdp/ps3/`).
Arreglo: a 1:1 el más cercano (`rsxkit_read_filter_nearest`); la bandera
`gfx-copia-lineal` vuelve al lineal. El mismo fenómeno obligó antes a dibujar
sin filtro dentro de esas capas (rdp.c, «puntos naranjas y azules»).

## Sombreado lineal en pantalla con el bit 31 de SRC2 (2026-10-29)

El RDP de la N64 interpola el sombreado (color, alfa y con él el factor de
niebla) lineal en pantalla; el RSX, con corrección de perspectiva. En los
triángulos largos (la pista de F-Zero X, w de 500 a 6000; el fondo de sus
pistas, con dos vértices cerca de la cámara muy fuera de la pantalla) la
diferencia es grande: la bruma llegaba tarde y el fondo salía con la textura
en vez de la sombra. El mismo bit que cgcomp pone por error (arriba:
«quita la corrección de perspectiva en MUL, DP…») sirve a propósito: la
entrada llega **dividida entre la w del vértice e interpolada lineal en
pantalla** (lo que RPCS3 emula como `tcN * gl_FragCoord.w`). Si el vertex
program multiplica cada término X_j por w (`rsxkit_lincomb*.vcg`, constante
`affine`), el fragment program que lee TEXCOORD1..7 con el bit recibe
X_j·w/w = X_j lineal en pantalla: la sombra del RDP, sin instrucciones de
más por píxel. Las coordenadas de textura (TEXCOORD0) y el color del
vértice (COL0, factores C y Ca de los programas generados) siguen con
perspectiva.

- `rsxkit_affine_shade(1)` (lib/rsxkit/src/draw3d.c): la constante `affine`
  del vertex program, una copia de cada programa precompilado con el bit en
  las lecturas de TEXCOORD1..7 (`rsxkit_fp_upload_affine`, fprog.c) y
  `rsxkit_fp_desc.affine` en los generados (fpgen.c: otro programa en la
  caché). El emulador lo enciende por omisión (`salsa64_rdp_affine_shade`);
  `sin-sombra-afin` en el directorio de datos lo apaga junto con el recorte
  del RSP de la HLE (abajo); en el host, `SALSA64_SIN_SOMBRA_AFIN=1`.
- Solo tiene sentido con el recorte de la HLE contra la caja del clip ratio
  (`src/rsp/gfx/rdp.c`, `gfx_triangle`): el RDP interpola lineal **dentro
  del polígono que el RSP le manda**, no dentro del triángulo del juego. Sin
  recortar, la sombra afín de un triángulo con vértices a miles de píxeles
  queda peor que la de perspectiva. Medido contra el microcódigo real
  (F3DEX2 fifo 2.03 en la LLE del host): Sutherland–Hodgman recorriendo el
  polígono desde su primer vértice con el anterior, el plano izquierdo al
  final, abanico desde el primer vértice de la lista final y rechazo trivial
  de cada triángulo del abanico por los planos de la pantalla (114 de 115
  abanicos iguales; el que falta es un vértice de esquina a 0,9 px).
- Precisión: el RSX hace X_j·w/w en el interpolador en fp32; los X_j llegan
  a las instrucciones en fp16 como antes. En RPCS3 (`gl_FragCoord.w`), lo
  mismo. Pendiente de confirmar en la PS3 real con el paquete «Salsa64
  F-Zero afin»: si el bit no hiciera lo que el apunte de arriba midió, toda
  la imagen saldría escalada por w; el interruptor lo deshace.

## Indiana Jones: el RSX deja de avanzar tras una color image fuera de la RDRAM (2026-10-29)

Lo que dejó el log de la PS3 real (escala-objetivo 4×, 1080p, la HLE de
Factor 5 Indy en el SPE 0) tras once minutos de juego normales:

- A los 736,65 s, al salir por segunda vez de una cascada de la primera
  misión: `gfx: destino 900000/455: 455 × 341 a 2× (nueva, recicla
  684000/448; alto pedido 44)`. Es el único destino de ancho raro de la
  sesión (los demás, 400, 448, 512 y 640, creados entre los 86 y los 116 s);
  la primera salida de la cascada no lo pidió. Justo antes los dibujos por
  tarea suben de 127 a 280.
- Dos segundos después, el PPE espera la barrera 267875 (la etiqueta va en
  267874) en la etapa «profundidad» de la tarea 19625, con GET en 0 y PUT
  en 0x3dde4, y GET no se movió en toda la espera. A los 12 s, cierre
  forzado: se paran los SPE, se sale… y la consola entera quedó congelada.
  La caja negra no se escribió (solo se escribía con SELECT + L2).

**De dónde sale el destino.** 341 es el alto por omisión de
`rdp_targets_get` (3/4 del ancho), 44 el fondo de las tijeras que siguen y
2× la escala de un destino de 341 líneas con la escala-objetivo 4×. La
dirección 0x900000 está fuera de los 8 MB de la RDRAM: el RDP de la consola
no tiene dónde escribir ahí, así que no es una imagen que el juego vaya a
ver (un efecto de la cascada o una copia a textura vivirían en la RDRAM).
Es un G_SETCIMG que la HLE leyó de algo que no es una lista válida (la de
Indiana Jones corre listas secundarias de la DMEM, `do_sub_dlist`, y pasa
al RDP cualquier palabra desde 0xC0) o uno que el juego manda sin efecto en
la N64; sin el volcado de esa tarea no se puede decir cuál. En el host,
con las entradas del log como `--pad` y sin la partida del autor, el juego
se queda en el menú «SELECT GAME» (las pulsaciones no llevan al mismo
lugar) y en 39 500 campos no pide ninguna color image fuera de la RDRAM; y
no sale siempre (una de dos salidas de la cascada): hace falta el volcado
de la consola.

**Por qué el RSX se queda (lo que lo sostiene).**
- La espera es la de `zfeed` (la única espera de barrera en la etapa
  «profundidad»): antes de una carga de la z desde la RDRAM espera la
  barrera de la carga anterior, y esa barrera (267875) es la que `zfeed`
  pone justo después de cargar. La barrera anterior (267874) sí se
  escribió, y el reciclado del destino 684000/448, que pasa por
  `rsxkit_finish`, terminó (la etapa «destinos» no aparece): el RSX estaba
  al día al crear el destino de 455 y se paró antes de terminar esa carga
  de la z. Entre una cosa y la otra solo se le mandaron, a lo sumo, los
  dibujos sin z del destino nuevo y la carga.
- En Indiana Jones la z se carga desde la RDRAM al cambiar de destino (su
  z image es la dirección de uno de sus framebuffers, que `zfeed` ve
  dibujado como color, o cambia lo que la suma de muestras mira). En la
  misma sesión hubo 120 cargas de 400 × 447 y una de 448 × 415 sin
  problema: alto de entrada impar, ancho siempre par. La del destino de 455
  es de 455 × 341 a 2×: **ancho de entrada impar**, la primera de la sesión,
  con el motor de imagen escalada (NV3089, `rsxkit_depth_load`).
- nouveau (Mesa, `nv30_transfer.c`, la familia NV40 del RSX) redondea a 2
  el ancho y el alto de entrada de cada copia con ese motor.
- GET en 0 y quieto: 0 es el principio de la memoria del búfer (el bloque
  de arranque de libgcm, que el RSX ejecuta una vez). Un RSX atascado en
  una orden deja GET en ella; GET de vuelta en 0 sin avanzar se parece más
  a un canal que el sistema reinició o detuvo tras una excepción.

**Hipótesis (sin confirmar en la consola):** que lo que el RSX no tolera
sea el ancho de entrada impar del motor de imagen escalada. Alternativas
que el log no descarta: la superficie de 910 de ancho (la primera que no
es múltiplo de 16) con los dibujos que fueran antes de la carga, o algo de
la tarea que no se ve sin su volcado.

**Blindaje** (encendido; `rsx-sin-blindaje` en `dev_hdd0/salsa64` lo apaga,
`SALSA64_RSX_SIN_BLINDAJE=1` en el host):
- Una color image que empieza fuera de la RDRAM no tiene destino
  (`rdp_cimg_impossible`, `src/rdp/targets.c`; igual en `raster.c`): ni
  superficie, ni reparto de rellenos, ni copias del PPE, ni recicla una
  ranura. El caso de la cascada no llega al RSX.
- `rsxkit_depth_load` reparte la copia en tramos de ancho de entrada par
  (`lib/rsxkit/src/sifm.c`): el tramo impar lee un texel más de su fila
  (dentro del pitch) con la misma razón y el mismo recorte, así que la
  salida no cambia. Con anchos pares los tramos son los de siempre.
- En el host, SM64, Zelda OoT, Indiana Jones, F-Zero X y Rogue Squadron
  dan las mismas tareas e imágenes en sus primeros 1500 campos.

**Cuando el RSX se queda igual** (`src/frontend/main.c`, `rsx_diag_full` y
`rsx_stuck_files`):
- rsxkit registra el aviso de excepciones del sistema
  (`gcmSetGraphicsHandler`): el latido lo deja en el log en cuanto llega,
  y el diagnóstico lo repite.
- A los 2 s: registros (GET, PUT, REF, la última barrera y el último
  `rsxkit_finish` mandados y escritos), el búfer de comandos alrededor de
  GET y de los 672 bytes antes de PUT (con GET en 0 solo esto dice qué se
  mandó al final), el rastro de las últimas 64 operaciones de rsxkit con su
  offset en el búfer (`rsxkit_trace_get`: ligar, borrar, dibujos juntos,
  cargas de la z, barreras…) y lo último de los destinos, profundidades y
  cargas de la z (`salsa64_rdp_diag_log`).
- A los 12 s, antes de salir, en `rsx-colgado-<campo>/`: el búfer de
  comandos (`fifo.bin`), el volcado de la tarea que se traducía
  (`gfxdump.bin`, para `salsa64-run --raster-dump`) y el anillo de la caja
  negra si está encendida (`caja.s4bb`, sin comprimir), todo con
  `sysLv2FsFsync`.
- Salida: rsxkit abandona el RSX (`rsxkit_abandon`: no lo vuelve a esperar
  ni a vaciar el búfer, ni flips) y pone PUT = GET para que no le quede
  nada pendiente; no se suelta su memoria ni se esperan flips. Por qué se
  congeló la consola: no se puede ver desde el PPE. Los grupos de SPU (lo
  que la colgó el 2026-10-13) estaban parados («SPE detenidos» llegó al
  log), así que lo que queda es lo que el proceso le deja al sistema: un
  contexto del RSX parado con trabajo pendiente; si el sistema espera a que
  el RSX lo termine (al liberar el contexto o al volver a dibujar el XMB),
  no termina nunca. PUT = GET cubre el primer caso; si el RSX quedó en
  un estado que solo un reinicio arregla, ninguna salida al XMB sirve. Para
  probarlo, `rsx-colgado` con `reinicio` (reinicio por el sistema,
  `sys_sm_shutdown` 0x200) o `reinicio-directo` (0x1200) hace que el
  cierre forzado reinicie la consola en vez de volver al XMB, después de
  escribir todo lo anterior.

### Segunda ronda: de dónde salían 900000/455 y 880000/18 (2026-10-29)

Con el blindaje, Indiana Jones ya no colgó la PS3, pero al salir de la
cascada pidió 880000/18 (y una z image en 0x820000) en cada tarea: Indy
invisible y la mitad derecha de la pantalla negra. Las cajas negras de esas
tareas lo muestran en la salida de la HLE: los dibujos de Indy (tijeras en
su recuadro) van a 880000/18, y un rectángulo de (240, 0) a (670, 430) con
s = t = 0 pinta de negro la derecha.

**Origen: la HLE ejecutaba vértices como comandos.** El G_SETCIMG
`FFB00011 00880000` tiene formato 5, que no existe: son los 8 bytes de un
vértice de Factor 5 (x −80, y 17, z 136, bandera 0); 900000/455 es otro
(y 454, z 144), y el rectángulo, un G_FILLRECT de otro vértice. La lista
secundaria (la dirección de DMEM 0x58C que el microcódigo corre antes de
dibujar) se seguía hasta un B8 000000 FFFFFFFF; Indiana Jones deja ahí
también listas que acaban en un B8 000000 00000000 (en la RDRAM de una
partida en el host, 58 de los punteros a 0x58C), y la HLE seguía de largo
por los vértices de detrás y ejecutaba como comandos del RDP las palabras
desde 0xC0. El estado que dejan (color image, z image, rectángulos, modos)
persiste: por eso pasa en cada tarea de la escena.

**Comprobado contra el microcódigo real** (`salsa64-run --lle=all`, desde
un estado de una partida en el mismo nivel): con la lista de un material
cambiada para acabar en `B8000000 00000000` y un G_SETCIMG a 0x900000
detrás, la HLE vieja lo ejecuta (aviso «fuera de la RDRAM») y el microcódigo
real no; con el G_SETCIMG dentro de la lista (antes del B8 … FFFFFFFF), lo
ejecutan los dos. El microcódigo termina la lista en cualquier G_ENDDL.

**Arreglo:** `do_sub_dlist` (`src/rsp/gfx/f5indy.c`) termina en cualquier
G_ENDDL (`GFX_RES_F5_SUB_END` en el resultado; la PS3 lo anota en el log);
`f5-sublista-vieja` en `dev_hdd0/salsa64` (`SALSA64_F5_SUBLISTA_VIEJA=1` en
el host) vuelve a lo de antes. El blindaje del RSX se queda como red.
