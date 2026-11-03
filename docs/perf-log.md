# Registro de rendimiento

## 2026-10-27 — B3313, escena pesada y retraducción falsa
| Medida | B3313 | SM64 comparable |
|---|---:|---:|
| qemu, 16,72/5,01 s N64: MIPS; espera | 9,2 → 34,6; 79,6 % | 20,3 → 22,6; 89,1 % |
| bloques compilados / repetidos / invalidados | 207928 / 202275 / 207603 → 5652 / 2 / 2227 | 2483 / 0 / 0, igual |
| compilación total / repetida en qemu | 5660,7 / 5462,2 ms → 233,1 / 0,057 ms | 99,0 / 0 ms → 102,6 / 0 ms |
En la PS3 anterior, el campo pesado era 25,2 ms: el 15,1 % de qemu pone un techo orientativo de 3,8 ms/campo a la retraducción; no es tiempo medido en PS3. HLE gráfica y audio son asíncronas (≈1,6 y 1,5 ms/campo; cero esperas), hilo 1 ≈2,0 ms y CRT 10,3 ms/cuadro con RSX al 24 %: no se suman al tiempo del PPE. Caché mayor: techo nulo (cero vaciados); contención causal medida antes, ≈0,13 ms por ms de hilo 1, da ≈0,3 ms. El nuevo resumen por hueco menor elimina la invalidación de 6,4 MB; el modo anterior la reproduce. Mismo trabajo emulado y lockstep sin diferencias; falta el A/B en PS3 real.

## 2026-10-25 — Cruis'n Exotica, perfil orientativo del host

En una carrera reproducida en el host, 1375 tareas durante 2800 campos
promediaron 10506 comandos, 1682 triángulos, 355 dibujos y 1050 cargas a
TMEM por tarea; el CPU emulado quedó 88 % en espera. Frente a las 260–450
cargas por tarea de las escenas pesadas de Zelda medidas en PS3, el primer
cuello probable es el SPE/TMEM y después la traducción al RSX.

El host y qemu solo ordenan alternativas: no predicen campos/s de la PS3.
Antes de cambiar la caché o agrupar cargas hace falta medir el paquete en la
consola. Como techo orientativo, la caché redujo hasta 2,4× el tramo de
cargas de Zelda; no implica la misma mejora total ni el mismo factor aquí.

## 2026-10-25 — Filtros, segunda ronda: eliminar la copia del cuadro completo

Primera ronda, PS3/Diddy Kong Racing/Trinitron/1440×1080 desde 320×237 a 3×: promedio 7,48→6,74 ms, pero completo 10,24→11,78 ms; repetido ≈1,4 ms. Esa regresión descarta la primera propuesta. Logs: `build/ps3-filtros-opt/` y `build/ps3-filtros-sin/`.
La segunda escribe la final directamente en el búfer de pantalla. El repetido lee el anterior antes de sobrescribirlo; cada copia renueva la referencia. **Cero copias en completos y cero memoria adicional** (antes 7,91 MiB). UI/FPS, avisos, sistema o un búfer redibujado invalidan la referencia: se rehace la final. La generación de entrada sigue siendo obligatoria.

| Filtro / pasada | Antes → después, instrucciones/TEX | Tiempo anterior PS3 → estimación segunda ronda |
|---|---|---|
| Todos / reducir H + V (3×) | 17/3 + 15/3 → igual | Trinitron 1,3–1,8 ms → igual |
| Todos / lineal + resplandor | 12/1 + 31/12 → igual | Trinitron ≈1,1 ms → igual |
| Máscara normal y fina / tesela, puntos, final | 58/0, 143/10, 33/7 → igual | Sin nueva medida aislada; mismo trabajo |
| RGB / señal, decodifica, líneas A/B, final | 31/1, 49/9, 51/2+26/2, 44/5 → igual | Sin nueva medida aislada; mismo trabajo |
| S-Video / señal, decodifica, líneas A/B, final | 31/1, 103/27, 51/2+26/2, 44/5 → igual | Sin nueva medida aislada; mismo trabajo |
| Compuesto / señal y decodifica ×2, líneas A/B, final | 31/1 y 103/27 ×2, 58/3+31/3, 44/5 → igual | Sin nueva medida aislada; mismo trabajo |
| Trinitron / líneas A/B | 85/5 + 58/5 → igual | ≈2,9 ms → igual |
| Trinitron / final | 44/5 → igual | 5,4–6,0 ms → igual |
| Todos / copia en completo | 3/1 → eliminada | No se aisló; delta completo observado +1,54 ms → se elimina su causa |
| Todos / repetido válido | 3/1 → igual | ≈1,4 ms → ≈1,4 ms, pendiente PS3 |
| Trinitron / completo total | cadena original + copia → cadena original | 11,78 ms → objetivo ≤10,24 ms; estimación, no medición |

El inventario actualizado (`tools/filtros-inventario.py`, salida en `build/filtros2/inventario.txt`) conserva tamaños, formatos y tráfico por pasada. Completo: ahorra 11,87 MiB lógicos de copia; repetido: 11,87 MiB, sin contar cachés. Los sencillos y el SPE no cambian.
Se detuvo la variante fp16 del tubo tras dos fallos (máximo 255): el GLSL y el microcódigo muestran fuentes H ensambladas como R en POW/EX2/LG2. Se conservan los dos ensayos en `build/filtros2/`; **no están en el binario entregado**. No se alcanzó la optimización aritmética: ahorro estimado de shaders entregados 0 %, por debajo del tope del 10 %; no se amplió a otros shaders. La espiral de 12 muestras del resplandor no es separable en dos ejes sin cambiar su núcleo.
Verificación: host completo 691 pruebas/887841 comprobaciones/0 fallos; 17 shaders originales y `crt_spe` binarios idénticos, holgura CRT 74688 B. Autoprueba ampliada a 192 casos (los 96 originales más generación nueva, copias encadenadas e invalidación): RGB máximo ≤1, cuenta diferencias y exige exactamente cero copias en completos y una en repetidos válidos. RPCS3 720p y 1080p: 192/192 por salida, máximo cero; sus tiempos no representan la consola.
Guion PS3: crear `filtros-autoprueba` en `/dev_hdd0/salsa64`, abrir **Salsa64 Filtros / SLSA00210**, esperar la salida y guardar `salsa64.log`; exigir 192/192 y `RESULTADO filtros: OK`. Renombrar la bandera a `.off`; si hay `filtros-opt-fallo` de una prueba anterior, conservarlo hasta revisar el resultado y después renombrarlo. Sin contador FPS ni avisos, jugar Diddy Kong Racing 2 min en el mismo tramo con Trinitron/3×, sin y con `filtros-sin-optimizar`, guardando cada log antes de abrir otra vez. Repetir con F-Zero X o SM64 en el castillo sin cuadros repetidos; comparar completos, no solo promedios. Probar también abrir/cerrar pausa y botón PS: no deben quedar textos en la imagen. Rechazar máximo ≥2 o completos más lentos; no integrar sin esta confirmación.


## 2026-10-25 — Caché de la salida CRT (estimación, pendiente PS3)

Los shaders originales y `crt_spe` no cambian; la salida X8R8G8B8 se guarda con el mismo viewport y se copia sin interpolación. Copia: 3 instrucciones, 1 TEX, fp32; los 17 programas anteriores son binarios idénticos. Inventario reproducible: `python3 tools/filtros-inventario.py build/salsa64/obj`; detalle crudo en `build/filtros/`. Añade 7,91 MiB a 1080p y una copia por campo; solo ahorra al repetir imágenes con generación válida. Hipótesis de copia **0,6–1,0 ms**, sin medir: 11,87 MiB lógicos por copia de 1440×1080, más sincronización. No son tiempos nuevos de consola.

| Filtro | Antes completo PS3 (ms) | Nuevo completo estimado (ms) | Promedio antes → después con 50 % repetidos (ms) |
|---|---:|---:|---:|
| Máscara | 7,38 | 7,98–8,38 | 4,89–6,19 → 4,29–4,69 |
| RGB | 9,37 | 9,97–10,37 | 7,09–7,23 → 5,29–5,69 |
| S-Video | 10,63 | 11,23–11,63 | 7,72–7,86 → 5,92–6,32 |
| Compuesto | 13,21 | 13,81–14,21 | 9,01–9,15 → 7,21–7,61 |
| Trinitron | 9,56 | 10,16–10,56 | 7,32 → 5,38–5,78 |
| Máscara fina / lineal / resplandor | sin medida aislada comparable | cuentas intactas | fina: misma caché; las otras: sin cambio |

Trinitron: las tres ventanas de `build/filtros/ps3-trinitron-3x.log` suman 938 cuadros medidos (510 repetidos): promedio ponderado anterior 7,10 ms → estimado 4,95–5,35 ms (25–30 %). Primera ventana: repetido 5,08 ms; nuevo repetido estimado 0,6–1,0 ms. Para los cables se supone final 4,8–5,08 ms y máscara 2,4–5,0 ms: son intervalos orientativos, no mediciones aisladas nuevas. Con 50 % repetidos, compuesto supera el tope del 10 % (15–21 %); sin repetición empeora. No se cambian precisiones ni se prometen ganancias en todos los juegos. Descartar ante un píxel distinto o ahorro real insuficiente.

## 2026-10-23 — Pokémon Snap: límite estable de la escala automática (PS3 real)

Revisión `18ea16f`, automática hasta 5×. En la escena de la introducción,
5× perdió margen: la decisión de bajar llegó tras 3,0 s con VI al 92,7 %,
RSX al 107,8 % del período, cero presentados y 56 saltados en la última
ventana. Esos tres segundos de arranque fueron visibles como cortes.

A 4×, las dos ventanas siguientes de cinco segundos presentaron 300/300 y
299/299 campos, sin saltos. La presentación del RSX midió 5,2 y 7,4 ms por
campo, aproximadamente 31–44 % del período. El paso 4×→5× no sigue el
crecimiento de píxeles ni se puede predecir con el umbral anterior del 60 %:
volver a subir habría repetido los cortes. Por ello la política nueva solo
baja, recuerda 4× para ese juego y no vuelve a intentar una escala fallida
mientras permanece abierto. Falta confirmar el nuevo arranque en la PS3.

## 2026-10-21 — Battle for Naboo: coste medido sin comparación A/B (PS3)

Revisión `7c723d3`, imagen Factor 5 Indy, escala 1× sin filtro. El autor
reporta lentitud preexistente, no regresión. Primeros 600 campos cerca de
60 campos/s; tras la transición, 31,4–33,2 campos/s (52–55 % NTSC),
29,4–31,3 ms de emulación por campo. No son FPS de imágenes únicas.

En 700–799 hay 128 triángulos/tarea, SPE 3,93 ms y traducción RSX 3,59 ms;
en 1300–1399, 1385 triángulos, SPE 14,63 ms y traducción RSX 7,46 ms.
El ritmo ya es de unos 32 campos/s con poca geometría: queda por perfilar
el hilo de emulación, además de la HLE de Factor 5. No sumar ni restar
estos intervalos paralelos como si fueran una partición del cuadro.
El RSX marca una cota de ocupación 9–15 % en los últimos resúmenes.

Compilación acumulada 500,869 ms, repetidas cero. Cierre limpio tras 1625
campos y 1469 tareas, cero descartes de cola y tareas cortadas. La ventana
final de 24,5 campos/s contiene una pausa y no mide velocidad sostenida.
No hay captura anterior equivalente para atribuir cambios de rendimiento.
Log `build/ps3/re2-spe/naboo-20261021.log`, SHA-256
`d784957ff0e2908abe97523926843e0602b493835825cc37a153720ce25927fd`.

## 2026-10-21 — Pokémon Snap en el paquete conjunto (PS3 real)

Revisión `7c723d3`, afín y recorte experimental activos: 11513 campos
emulados, 8073 tareas, cierre limpio y cero descartes de la cola. Los 40
resúmenes periódicos del tramo de Snap tienen mediana 59,8 campos/s; no es
una cifra de FPS de imágenes únicas. Hay ventanas activas de 55–56,6.

No es benchmark comparable: empieza a 1× con Nítida, cambia a CRT Trinitron
y luego a escala 5×. Las ventanas de 0 y 21,8 campos/s incluyen la pausa de
8,4 s y su reanudación; no representan rendimiento sostenido. El autor
confirma que ya no observó el desorden de dibujo en lo probado, sin
especificar recorrido completo.
No se atribuye mejora de velocidad ni corrección universal del orden de dibujo.
Log `build/ps3/re2-spe/pokemon-snap-20261021.log`, SHA-256
`a6c891d39b1b73e0e442caabe2f5a8546eb0efaa264f83e2de425ad21fb5369f`.

## 2026-10-21 — Pila del diagnóstico gráfico: alineación local de DMA

Medición estática, no tiempos de PS3. La combinación RE2/SPE con el hash
separado aún deja 1472 bytes de holgura en fase 8. El bloque local del
detalle pide alineación de 128 aunque el DMA admite 16; el destino externo
conserva sus 128 y se espera la transferencia antes de salir.

Al alinear el bloque local a 16, `run_detail` baja de 464 a 352 bytes y la
cadena total de 10144 a 10032. Texto 103688 → 103680, `_end` sin cambios,
holgura 1472 → 1584. No se cambian cachés ni el mínimo de 1536, ni se
atribuye beneficio de FPS. Log `build/logs/re2-spe-pila-detalle16.log`.

La reconstrucción limpia del paquete conjunto confirma 1584 bytes de
holgura en fase 8 y 1712 en imagen 1 normal; todas las demás imágenes y
fases también cumplen el mínimo. Las rutas instrumentadas pasan el A/B de
detalle y publican sus bloques en RPCS3. No hay medición nueva de PS3.

## 2026-10-20 — A/B de recorte SIMD: menor coste de geometría (PS3 real)

Mismo paquete `salsa64-recorte-vector`: referencia `spu_gfx1fase7` frente a
`spu_gfx1fase8` con `gfx-recorte-vector`. Ambas aperturas dieron hash/listas
iguales al PPE, vectorial/escalar bit a bit iguales y detalle A/B con cero
diferencias. La experimental verificó además 64 lotes/256 máscaras de
recorte sin diferencias, completó 5975 tareas y dejó 3040 bytes de pila sin
tocar al cerrar (estimación por ceros), holgura de build 1648.

Se tomaron todas las ventanas completas de 100 tareas con más de 2000
vértices vectoriales por tarea: 50 ventanas de referencia y 51 del
experimento. Los tiempos se ponderan por vértices, usando los ms/tarea y
promedios impresos (redondeados); el decrementador usa 79,8 MHz.

| Medida | Referencia | Recorte SIMD |
| --- | ---: | ---: |
| Lote, ticks por vértice vectorial ponderados | 18,22 | 16,02 |
| Mediana por ventana y rango | 18,2 (18,0–18,4) | 16,0 (15,8–16,2) |
| Etapa de vértices, ns por vértice total ponderados | 267,56 | 241,30 |
| Cobertura de la ruta vectorial | 96,02 % | 96,05 % |

El lote consume aproximadamente **12,1 % menos** tiempo por vértice
vectorial; la etapa completa de vértices, **9,8 % menos** por vértice total.
Como comprobación local, las ventanas 5401–5500 tienen 4387,7/4344,0
vértices, 202,1/200,7 cargas y 188,4/189,4 de cola por tarea: el lote pasa
de 0,951 a 0,834 ms/tarea y vértices de 1,164 a 1,047; RDRAM y resto
mantienen 0,043 y 0,170 ms/tarea en ambos. La selección de ventanas global
no exige una escena idéntica; no atribuir un porcentaje de FPS ni de tiempo
total de cuadro a este cambio. El diagnóstico sigue activo en ambos modos.

Log experimental `build/ps3/recorte-vector-20261020.log`, SHA-256
`f5cfc805ffc7da903d81c3103bdc10b1f7f074c1ee411e24665f86fcbbf1b055`;
referencia y huella en la entrada siguiente. El resultado justifica conservar
la variante como candidata de integración; falta regresión en otras escenas
y familias antes de convertirla en comportamiento normal.

## 2026-10-20 — Referencia del A/B de recorte SIMD (PS3 real)

Paquete `salsa64-recorte-vector`, interruptor experimental apagado y fase 7
confirmada (`spu_gfx1fase7`). Hash y tareas iguales al PPE,
vectorial/escalar bit a bit iguales; A/B sintético 17 pares sin diferencias
(mediana sin/con detalle 0,205/0,209 ms). Cierre tras 6287 tareas, con 3168
bytes de pila sin tocar (estimación por ceros).

Última ventana completa 6101–6200: 7243,1 vértices/tarea, 6963,0 vectoriales
y 280,1 de cola; lote 1,592 ms/tarea y 18,2 ticks/vértice vectorial,
RDRAM 0,070, resto 0,275 de 1,938 ms/tarea de vértices (redondeados).
La parcial final (87 tareas) registra 18,3 ticks de lote y 3,1 de resto.
Es la referencia del mismo paquete: no comparar campos/s entre escenas
distintas ni anunciar una ganancia hasta recoger el modo experimental.
Log `build/ps3/recorte-referencia-20261020.log`, SHA-256
`363e91fb19096318f97c1a2169902669cb197dd976bfa7dbf3a2be2039139098`.

## 2026-10-20 — Lote completo F3DEX2 en Road Rash 64 (PS3 real)

Paquete `salsa64-spe-fase7`, imagen `spu_gfx1fase7` confirmada, 7907 tareas.
Hash y tareas iguales al PPE, vectorial/escalar bit a bit iguales; A/B 17
pares sin diferencias (mediana sin/con detalle 0,205/0,210 ms). Holgura de
build 1776 bytes y 3168 sin tocar al cerrar (estimación por ceros).

Ventana 7401–7500: 8863,8 vértices/tarea, 8561,0 vectoriales y 302,8 de cola.
Los 2,371 ms/tarea de vértices se reparten en lote completo 1,941 ms
(81,9 %), RDRAM 0,089 y resto 0,341 (14,4 %). El lote mide 18,1 ticks por
vértice vectorial y resto 3,1 por vértice total. La última ventana completa,
7801–7900, registra lote 0,992, RDRAM 0,048 y resto 0,198 de 1,238 ms/tarea;
18,3 y 3,5 ticks/vértice, respectivamente.

El intervalo incluye desempaquetado, matriz y postproceso. El trabajo interno
del lote domina este reparto; la carga externa y las colas quedan como coste
secundario. No identifica por sí solo una instrucción ni mide una mejora.
Las aperturas de fases 6 y 7 tuvieron escenas distintas: no restar sus tiempos
para calcular el coste de matriz/desempaquetado. Log local
`build/ps3/vect-fase7-20261020.log`, SHA-256
`d0c0e46827c4a7cf3f170885ad11b6a0c6e2a96f983b2f7580faca359f65a2c7`.

## 2026-10-20 — Postproceso conjunto F3DEX2 en Road Rash 64 (PS3 real)

`gfx-fase-6`, imagen `spu_gfx1fase6`, geometría vectorial. Hash y tareas
sintéticas iguales al PPE; vectorial/escalar iguales bit a bit; autoprueba
A/B 17 pares, cero diferencias, medianas sin/con 0,205/0,210 ms. El cierre
registró 6142 tareas y 3168 bytes de pila sin tocar (estimación por ceros),
con holgura de build de 1776 bytes.

Ventana completa de tareas 6001–6100: 9316,5 vértices/tarea, de los cuales
8932,4 vectoriales y 384,1 de cola. El tiempo de vértices fue 2,516 ms/tarea:
RDRAM 0,094, postproceso completo 1,452 y resto 0,969 (redondeados). Son
13,0 ticks por vértice vectorial de postproceso y 8,3 por vértice total del
resto. La ventana parcial final de 42 tareas mantiene ambos valores.

Este intervalo cubre las copias x/y/z/w y las fases 2–5 juntas, con un par
de relojes por lote. Excluye desempaquetado y matriz; esos trabajos, las
colas escalares y la envoltura de carga permanecen en resto. Los relojes y
la planificación del compilador perturban la medida; no sumar las fases
1–5 de otras aperturas para deducir una duración sin instrumentación ni
comparar campos/s como ganancia. Log local
`build/ps3/vect-fase6-20261020.log`, SHA-256
`fc21a738a5a2a6ef250a819ab34de223d759ae97f2a5a40824cf4b57aa118d78`.

## 2026-10-20 — Subfases de vértices F3DEX2 en Road Rash 64 (PS3 real)

Cinco aperturas de la misma carrera, con interpolación apagada, escala interna
5× y la ruta vectorial normal, cargaron una imagen de diagnóstico distinta de
la imagen 1. Las autopruebas vectorial/escalar y A/B de las cinco aberturas
reportaron cero diferencias. La fase 5 no incluye las copias directas de
x/y/z/w, para conservar la holgura mínima de pila.

| Subfase medida | Ticks/vértice | Resto sin esa fase | RDRAM |
|---|---:|---:|---:|
| Desempacado | **4,6** | 16,4 | 0,8 |
| Proyección/división | **4,9** | 20,1 | 0,8 |
| Recorte | **5,0** | 20,4 | 0,8 |
| Niebla | **3,8** | 20,9 | 0,8 |
| Conversión/escritura | **5,6** | 19,2 | 0,8 |

Cada fila procede de la ventana final de su propia apertura (unas seis mil
tareas de la imagen 1); las cargas no son idénticas. Por ello los restos no
se suman ni sirven para comparar campos/s. Tampoco aparece una subfase única
que explique los ~20 ticks por vértice: las cinco cuestan 3,8–5,6 ticks en
sus imágenes instrumentadas. El siguiente cambio debe partir del código
máquina de esas rutas y conservar un interruptor A/B; no se infiere una
ganancia de rendimiento de estas corridas.

## 2026-10-20 — Lote de vértices F3DEX2 del SPE (host y presupuesto de local store)

La inspección del objeto SPU de `gfx_vertices` confirmó un recorrido por
vértice: cuatro productos/sumas para cada coordenada de recorte, seis pruebas
de plano y una división para la proyección, además de ramas para luz, TEXGEN
y niebla. La ruta medida de Road Rash usa color directo y niebla; no usa las
rutas de luces o TEXGEN. El compilador emitía operaciones escalares y saltos
de recorte, no un lote automático.

La imagen 1 reúne cuatro entradas de `G_VTX` en los carriles del SPU para las
cuatro coordenadas de recorte. Conserva la secuencia de operaciones de cada
carril y deja proyección, conversión y niebla en el orden escalar para no
cambiar el redondeo. `gfx-escalar` selecciona el recorrido previo al abrir el
juego. La autoprueba F3DEX2 incorpora niebla y alterna 17 pares vectorial /
escalar: compara vértices, comandos, texturas, caché y contadores, y deja en
el log las medianas del decrementador. Falta ejecutar esa prueba en PS3 real;
por tanto **no hay un tiempo ni una ganancia medida en PS3**.

El intento de agrupar cuatro triángulos F3DEX2 no pasó el enlace: la imagen 1
quedó 272 bytes bajo el mínimo de pila. Se retiró; los triángulos siguen por
la ruta escalar. Para hacer ese trabajo habría que separar primero código o
estado de la cadena S2DEX, no reducir el margen de seguridad.

El host no ejecuta la imagen SPU y no sirve para estimar ms del Cell. Como
validación funcional (no de rendimiento), `--lle-check-gfx`, limitado a 700
campos, completó SM64 (270 tareas), OoT (540), MK64 (225) y Road Rash (696);
en Road Rash fueron 696/696 idénticas. La comparación cuantitativa de la
ganancia queda pendiente de la misma carrera de PS3 con `gfx-spe-detalle`,
primero normal y luego con `gfx-escalar`.

## 2026-10-20 — Road Rash 64: evitar retraducciones por descartes de espera (host/qemu)

NTSC, respaldo propio; 8 MB de RDRAM, Controller Pak nuevo en memoria,
`docs/pads/roadrash-carrera.pad`, hasta el campo 2100 (3 290 786 100 ciclos,
35,10 s de consola). Docker + qemu-ppc64 `power9`, registros fijos, caché
24 MB, HLE gráfica y audio del host, **sin rasterizador**. Un mismo binario,
primero `SALSA64_REC_IDLE_RECOMPILE=1`, después por omisión. Los logs locales
son `build/roadrash/antes-final.log` y `despues-final.log`.

| Cuenta o tiempo | Recompilar al descartar | Parche de una palabra |
|---|---:|---:|
| Bloques compilados | 61 422 | 8819 |
| Repeticiones de dirección/largo/huella | 52 597 | 0 |
| Tiempo del emisor (ms, timebase de qemu calibrado) | 3589,710 | 357,720 |
| Del anterior, repeticiones (ms) | 3173,611 | 0 |
| Invalidaciones por descarte de espera | 42 375 | 0 |
| De las anteriores, huella MIPS idéntica | 42 375 | 0 |
| Consultas quitadas por parche | 0 | 520 |
| Invalidaciones por CPU / PI / otros escritores | 0 / 0 / 0 | 0 / 0 / 0 |
| Bloques retirados por desalojo | 14 554 | 0 |
| Desalojos de tramo | 29 | 2 (aún vacíos) |
| Recortes por CU1 | 7 | 0 |
| Contador histórico total de invalidados | 56 936 | 0 |
| DMA del PI | 3622 | 3622 |
| Detalles perdidos por tablas llenas | 0 | 0 |

Compilaciones **−85,6 %**, tiempo dentro del emisor **−90,0 %** en esta
medición de qemu. El tiempo total fue 66,11 / 35,53 s, pero hubo otras
verificaciones concurrentes: no se usa como una ganancia controlada de
rendimiento y, en particular, **no son tiempos ni FPS de la PS3**.

Los saltos `8003ae0c` y `8003b824` comparten la ranura 49 del detector.
Los bloques `8003ad84`, `8003adf0`, `8003b788`, `8003b7d8` se tradujeron
10 420 veces cada uno con los mismos bytes MIPS en el modo anterior. Los
cuatro acumularon 2837,338 ms de timebase del host al emitirlos. Los 7
invalidados que faltan al sumar descartes y desalojos son recortes por CU1.
No hubo escritura del juego que explicara las invalidaciones masivas.

El estado entero final (antes / después / lockstep) tiene SHA-256
`517c4364211057f7a26008c74db80f1dbb8ef5e855ac96371973464f0c43f451`.
Road Rash pasa lockstep en 2100 campos; SM64, GoldenEye, MK64 y OoT en 600
campos cada uno (inicio, sin recorrido de juego de esos controles). Más
alcance y limitaciones de las pruebas en `docs/estado.md`.

El log anterior de la consola registró 46 259 invalidados y 17 desalojos
sin distinguir las causas. Un perfil anterior de otra partida tenía solo
154 retraducciones en 13 988 campos; no era el mismo recorrido. La
reproducción actual identifica el mecanismo de la cifra masiva, pero el
paquete nuevo **todavía requiere A/B en PS3** con el archivo
`/dev_hdd0/salsa64/jit-invalidacion-vieja`. No se extrapolan los milisegundos
del host al Cell.

Cada optimización lleva medición antes/después **en PS3 real** (RPCS3 no
reproduce los tiempos del Cell). Formato: fecha, commit, prueba, entorno,
antes, después, notas.

| Fecha | Commit | Prueba | Entorno | Antes | Después | Notas |
|---|---|---|---|---|---|---|
| 2026-10-28 | abd31b5 (rama recompilador-r4, sobre main 292177b) | **Copias de bytes de los bloques mapeados en ranuras que se liberan** (antes: pool lineal de 1 MB sin recuperar hasta vaciar). GoldenEye desde el arranque hasta la presa, 5400 campos con `--rec --perfilado` | Host: qemu-ppc64 power9 en Docker dev; solo cuentas | 15 445 compilaciones, 31 retraducciones idénticas (0,2 %), 25 213 desprendidos, 19 396 enganchados, 4 desalojos, 0 vaciados; pool viejo **863 KB de 1024** al llegar a la presa | Idéntico en todas las cuentas (el pool no llega a llenarse en esta escena); ranuras 1460 KB de 4096 con 5817 desprendidos vivos; 0 matados por ids | El cambio no toca el código emitido. La escena que lo exhibe es Perfect Dark en juego: en la PS3 (perfil-fpu2, 318 s) 143 220 compilaciones con 0 vaciados y **110 656 retraducciones idénticas (77 %, 6375 ms, campos de 60–120 ms con 1700–1950 de golpe)** porque el pool se llenaba en el primer minuto; en el host no se reprodujo con un guion de control. A/B en la consola con `jit-copias-viejas` (paquete «Salsa64 rec 4», SLSA00190). `--lockstep` desde el estado de la presa, 300 M ciclos: sin diferencias. Techos descartados en esta ronda con el laboratorio: load-hit-store en fronteras ~2,5 % del nativo, pila de regresos < 1 %, TLB en línea < 1 %. |
| 2026-10-16 | a3faea9 (sobre main 5227e5f) | **COP0 y MI fuera del camino lento**: bucles de espera que pasan por el planificador de libultra (`idle.c`), bloques cortados antes del COP1 con CU1 = 0, MTC0 Status y EPC en línea. Doom 64, NFL Blitz, 1080, Episode I Racer, Indiana Jones, 1800 campos desde el arranque con `--rec` y `SALSA64_REC_PROF` | Host: Docker arm64, qemu-ppc64 (power9); la Mac con carga 6–7 de otras tareas, los tiempos varían ±40 % entre corridas iguales: valen las cuentas | main: Doom 66,7 s, 0 % en espera, 49 149 llamadas a C y 160 496 instr. interpretadas por campo; Blitz 40,3 s, 0 %, 48 785 y 68 559; 1080 26,2 s, 0 %, 44 127 y 554; Racer 39,1 s, 10 %, 23 506 y 244; Indy 72,3 s, 21 %, 39 257 y 126 846 | Doom 7,8–11,7 s, **83 %, 254 y 80**; Blitz 10,8 s, **91 %, 350 y 87**; 1080 9,7 s, **86 %, 317 y 130**; Racer 32–59 s, **37 %, 365 y 140**; Indy 5,8 s, **86 %, 532 y 398** | Por partes: con los bucles de espera y el corte por CU1 (sin el MTC0 en línea) las llamadas a C por campo quedaban en 347, 573, 568, 826 y 1002; el MTC0 en línea las lleva a 254, 350, 317, 365 y 532. Racer con `--no-idle` (600 campos): MTC0 Status en C 25,5 M → 1 573. En la PS3 (sesiones de perfil-fpu2 y perfil-varios): Doom al 35 % con COP0 13,9 s y MI 9,3 s en 125 s; pendiente medir con `salsa64-perfil-cop0.pkg`. Las escenas del host son el arranque y la demo, no las partidas de la PS3. `test-rec` completo: mismas fallas de nemu64-test (433, 440 con fastmem), vitest, estados y perfilado igual; unitarias 573/0 con el intérprete y en los cinco modos del recompilador salvo `menu_scan_dir`, como en main; llamadas al intérprete de nemu64-test 1 155 508 → 1 087 186. `--lockstep` en Doom y Blitz, 600 campos: sin diferencias. |
| 2026-10-16 | abb3088 (rama fpu-redondeo, sobre main ecf0fc9) | **CVT.W/CVT.L con redondeo dirigido y MUL.S ancho en el recompilador**: bench de `salsa64-unit rec bench` (qemu, `cvt.w rm1`, `cvt.w rm2`, `mul.s mín`) | Host: Docker arm64, qemu-ppc64 (power9); solo vale la proporción, no el tiempo absoluto | CVT.W con RM=1: 189,7 ns/instr.; RM=2: 216,0; MUL.S con un operando 1e-25: 217,0; 7 M llamadas al intérprete cada uno (add.s 49,6 como referencia) | **36,2; 52,5; 42,1 ns/instr.; 0 llamadas al intérprete** (add.s 42,5) | ×5,2, ×4,1 y ×5,2 en proporción (se midieron dos corridas por versión; la primera fue ruidosa, la segunda se repite arriba). Los casos que ya eran nativos (add.s, cvt, mixto) no cambian. `test-rec` completo contra main: mismas fallas de nemu64-test (433 y 440 con fastmem), vitest, estados y perfilado igual; el paso de unitarias termina por el límite de 600 s de qemu en `menu_scan_dir`, igual que en main; llamadas al intérprete de nemu64-test 1 157 003 → 1 155 508. En la PS3 (Banjo-Tooie, 176 s): 17,09 M de CVT.W.S, 6341 ms, y 1,13 M de MUL.S, 576 ms, caían al intérprete; pendiente medir con `salsa64-perfil-fpu2.pkg`. La compuerta de la aritmética no mira U/O/Z/V (solo RM e I), así que si los MUL.S no eran de operandos fuera de [2⁻⁶⁰, 2⁶¹) seguirán saliendo. |
| 2026-10-15 | v0.2.5-91-gac22527 (sesión de perfilado de la regresión) | Banjo-Tooie; varias escenas, no desglosadas en el resumen | PS3 real, NTSC, escala 5×, filtro Ninguno al empezar (después se cambió a CRT Trinitron durante la sesión) | — | **59,7 campos/s mediana; p10 48,8** | Sesión de 1031 s. El muestreo atribuye 8,4 % de las muestras al camino lento en el total; docs/estado.md informa 35–60 % en las escenas pesadas de Rare. No se encontró la fracción separada de las ventanas normales. Retraducciones: **29 088 de 297 141 compilaciones (9,8 %)**. Palabras PPC/instrucción MIPS: 12,9 total y 5,8 calientes, sin ponderar. |
| 2026-10-15 | v0.2.5-117-g183a275 (rc1) | Regresión de varios juegos; escenas individuales no siempre identificadas en el log | PS3 real, escala y filtro según la prueba | — | Sin una mediana por juego reproducible en el resumen de la rc1 | La rc1 abrió más de 15 juegos en varias corridas. No se extrapola la velocidad desde líneas aisladas del log. |
| 2026-10-15 | v0.2.5-117-g183a275 (rc1) | Juego y escena no identificables en el bloque de medición | PS3 real, salida 1440×1080, entrada 320×237, escala 3× (con un filtro CRT la escala se limita a 3×), CRT Trinitron, reducción en 2 pasadas | — | **9,54 ms del RSX por cuadro** | La medición del log corresponde a 150 cuadros (149 medidos); el mismo bloque informa 5,04 ms para la pasada final. |
| 2026-10-15 | v0.2.6 (808c290) | Programa del SPE 0 (HLE gráfica) | Análisis estático de la pila al enlazar `spu_gfx.bin`, no una medición en la consola | — | **Holgura de pila del SPE 0: 1968 bytes** | El mínimo documentado es 1536 bytes. Con `texcache.c` en `-Os` (sin eso quedaba en 1328 al juntar F3DFLX y el S2DEX de las salas). |
| 2026-10-14 | 32eec08 → (caché de destinos de JR) | **Caché de destinos de JR/JALR** en `exit_jr`: tabla directa de 1024 entradas vpc → entrada del bloque que se consulta antes de recorrer map/xhash/blocks (16 palabras en el acierto, ~36 en el camino largo) | PS3 real (paquete «Salsa64 rec 1», SLSA00123), GoldenEye en la presa, ranura 0, quieto ~30 s, 8 ventanas estables de 5 s; lab en la Mac (qemu para la exactitud) | 13,82 ns por ciclo del hilo de emulación (13,73–14,00), 70,5–71,9 MIPS | **13,67 (13,58–13,88), 71,1–72,7 MIPS: −1,05 %** de todo el hilo (≈ −1,3 % del código traducido); acierto 92,4 % (82,7 M aciertos, 6,8 M fallos; 89 % en el lab) | Mitad de lo estimado con el modelo del lab (−2…−3,5 %): la elasticidad medida a las palabras que corren es ~0,4, no 0,58, y el hilo tiene un ~21 % que no es código traducido. Exacta: lockstep sin diferencias en GoldenEye (desde el arranque hasta la presa, 7,5 G ciclos, con 93,1 % de acierto en toda la corrida) y desde el estado de la presa, y en la intro de Perfect Dark; pruebas unitarias del recompilador (501) con y sin `FAR`. Encendida por omisión; `jit-sin-destinos` la apaga |
| 2026-10-13 | (rama del tope de cuadros, paquete «Salsa64 tope») | **Tope de cuadros en la PS3**: GoldenEye en la presa (Libre 37–202 s, 30 de 253–423 s, ajuste del juego) y Perfect Dark | PS3 real (Slim CECH-3011A, HFW 4.91 + HEN), SLSA00115, respaldos del autor, escala 5× | GoldenEye libre: 55,9 campos/s, 47 MIPS, 45 % en espera, emulación 14,2 ms por campo, 23–60 cuadros del juego | **tope 30: 56,1 campos/s, 31 MIPS (−34 %), 64 % en espera, emulación 15,2 ms**, 30–31,6 cuadros estables; Perfect Dark: 31,6 tareas gráficas por segundo | La emulación no bajó por una copia del framebuffer a la RDRAM esperada en cada cuadro (15 ms c/u): fallo previo de readbacks_wanted con el ancho del VI del anillo, arreglado después (también en Perfect Dark sin tope). Los «55–60 cuadros» de PD son el contador en modos entrelazados. Errores gráficos con 30 sin explicar. Tope en pausa; ver docs/interpolacion.md |
| 2026-10-13 | (rama del tope de cuadros) | **Tope de cuadros** (`salsa64_rsp_set_frame_cap`): el DP de cada tarea gráfica no llega antes de 1,9 o 2,9 campos desde el anterior; GoldenEye en la presa girando y caminando (300 campos) y Perfect Dark (30 s de la intro) | Mac, `salsa64-run --tope`, respaldos del autor | libre: GoldenEye 297 cuadros en 300 campos, CPU 39,5 % activo; Perfect Dark 43 cuadros/s, 16,0 % | **tope 30: GoldenEye 159 cuadros, 22,8 % (−42 %); Perfect Dark 25/s, 11,8 % (−26 %). Tope 20: 105 cuadros, 16,7 % (−58 %); 16/s, 10,3 %** | En la PS3 GoldenEye en la presa está limitado por el CPU (52–57 campos/s): falta medirlo en la consola con Libre contra 30. Zelda, SM64 y Mario Kart con 30, sin cambios; audio igual. Ver docs/interpolacion.md, «Tope de cuadros» |
| 2026-10-13 | fd1891c → (rama del código compacto) | **Código compacto del recompilador**: palabras de PPC por instrucción MIPS, GoldenEye hasta la presa (`goldeneye-dam.pad`, 7,5 G ciclos), pesado por ejecuciones de bloque | Mac, qemu-ppc64 (`salsa64-run --rec` con `SALSA64_REC_DUMP=1`, `tools/rec-expansion.py --counter 8`), respaldo del autor | 20,2 por instrucción (9,3 calientes, 11,0 frías); 17,2 MB de PPC, 305 palabras por bloque | **15,6 (8,2 calientes, 7,4 frías): −23 %**; 13,2 MB, 234 por bloque; mismo recorrido (mismas cuentas del perfil), 4 desalojos en vez de 6 | Prueba de KSEG0 en 4 palabras, un camino lento por bloque (rutina única + trampolín), cadena de escritura, enlaces de 3 palabras. **PS3 real** (mismo paquete, A/B con `jit-ancho`): GoldenEye en la presa 17,26 → 16,30 / 16,03 ns por instrucción emulada (−6…−7 %, 47,5 → 50–53 campos/s), Perfect Dark 14,85 → 14,45 (−3 %), SM64 y Zelda sin regresión. Ver «Código compacto» abajo |
| 2026-10-13 | 6f2a888 → 674ab5a | **Filtros CRT: tope de 3×, reducción en dos pasadas, reuso, Trinitron y SPE**, GoldenEye en la presa a 1080p (5× elegida) | PS3 real (Slim CECH-3011A, HFW 4.91 + HEN), app SLSA00109, respaldos del autor | 5×, una pasada: máscara 9,6, RGB 11,9, S-Video 13,0, compuesto 15,4 ms; 30–51 cuadros presentados por segundo | **3×, dos pasadas: máscara 7,4, RGB 9,4, S-Video 10,6, compuesto 13,2, Trinitron 10,1 ms; todos los campos presentados**; render del juego 5,6 → 2,1 ms | Reducir 3,12 → 1,2 ms. El SPE (señal del cable): S-Video −0,45 ms, compuesto +0,5 ms (tarda ~3 veces lo estimado). Ver «Filtros CRT en el RSX» abajo |
| 2026-10-13 | 6f2a888 | **Filtros CRT del autor en el RSX** (máscara de sombra y CRT con cable RGB, S-Video y compuesto), ciclo de 20 s por filtro, GoldenEye a 720p y 1080p, Perfect Dark y Conker a 1080p | PS3 real (Slim CECH-3011A, HFW 4.91 + HEN), app SLSA00109 «Salsa64 filtros», respaldos del autor, escala 5× | sin filtro: RSX ocupado 39–44 %, 59–60 cuadros presentados por segundo | GoldenEye, ms del RSX por cuadro a 720p / 1080p: **máscara 6,7 / 9,6, RGB 8,4 / 11,9, S-Video 9,6 / 13,0, compuesto 11,9 / 15,4**; a 1080p se presentan 51, 40, 36 y 30 cuadros por segundo | Reducir la superficie de 5× cuesta 3,1 ms en todos (la lectura, no las cuentas). Medido con Zelda en pausa: reducir 3,12 ms a 5×, 1,73 a 3×, 0,13 a 1×. Propuestas: escala 2× con un CRT (~5–6 ms menos), reducir en dos pasadas o con tiles, no rehacer el filtro en campos repetidos, señal del cable en un SPE (1–3,5 ms). Ver «Filtros CRT en el RSX» abajo |
| 2026-10-10 | 419731a → c14268c | Intérprete, `bench` mixto | PS3 real: Slim CECH-3011A, HFW 4.91 + HEN | 8,7 MIPS | 17,1 MIPS (×2,0) | Estado del bucle en registros, sin llamadas por instrucción, r0 sin escribir, tablas de `switch` sin `lwax`, FPU rápida sin pasar valores por memoria. Desglose en «Líneas base de la Fase 1» |
| 2026-10-10 | c14268c → 147d5ac | Intérprete, `bench` mixto | PS3 real (mismo equipo) | 17,1 MIPS | 21,2 MIPS (×1,24; ×2,4 desde 8,7) | Un solo despacho (SPECIAL en la misma tabla), FPU rápida sin comparaciones de punto flotante para clasificar, movimientos de COP1 en el bucle |
| 2026-10-10 | 940f6f8 | Frontend con vitest, emulación por campo | PS3 real (mismo equipo) | 65,6 ms (15,2 campos/s) | 35,0 ms (28,3 campos/s; ×1,9) | Detección de bucles de espera (`src/cpu/idle.c`): 45,6 % del tiempo saltado. Medición A/B automática del frontend, 10 s cada una, dos corridas iguales (65,56/65,66 → 34,97/34,84 ms). Lo que queda es el dibujo de vitest con el CPU |
| 2026-10-10 | 147d5ac → 940f6f8 | Intérprete, `bench` mixto | PS3 real (mismo equipo) | 21,2 MIPS | 21,0 MIPS (−1 %) | Costo del chequeo de bucle de espera en cada salto tomado. Ver «Bucles de espera» abajo |
| 2026-10-10 | f7b655d | **Recompilador** (Fase 3), nemu64-test completo | PS3 real (mismo equipo) | intérprete: 7921 ms (23,9 MIPS) | **1817 ms (104,2 MIPS; ×4,36)** | Mismas 462 fallas, mismo hash de nombres y mismo ciclo final (189 431 098). 28 694 bloques, 9,2 MB de caché, 139 440 entradas desde C. Código real: llamadas, retornos, FPU, E/S. Ver «Recompilador» abajo |
| 2026-10-10 | f7b655d | **Recompilador**, `bench` mixto | PS3 real (mismo equipo) | intérprete: 21,3 MIPS | **156,6 MIPS (×7,4)** | Bucle de 10 instrucciones con ADD.S nativa; ver desglose abajo |
| 2026-10-10 | f7b655d → f18b392 | Recompilador, escritura temprana inmediata | PS3 real (mismo equipo) | nemu64-test 1817 ms; bench enteros 749,9 MIPS | 1948 ms; 473,7 MIPS (**peor**) | Un `std` justo después de la instrucción que da el valor detiene el pipeline (~3 ciclos por almacenamiento). Pasa a opción y diferida (1fe8ebc) |
| 2026-10-10 | f18b392 | Recompilador con registros fijos (`OPT_PIN`), nemu64-test | PS3 real (mismo equipo) | 1948 ms (97,2 MIPS) | **1817 ms (104,2 MIPS; ×4,45 del intérprete)** | sp, ra, v0, v1, a0–a3 en r18–r25 entre bloques; +7 %. Por omisión en el frontend |
| 2026-10-10 | f18b392 | **Frontend con vitest**, recompilador | PS3 real (mismo equipo) | intérprete: 65,6 / 35,0 ms por campo (sin / con bucles de espera) | **5,38 / 2,90 ms (×12); 59,94 campos/s** | Velocidad completa: el resto del campo es la espera del vsync |
| 2026-10-10 | f416bee | Recompilador, cuatro variantes, nemu64-test | PS3 real (mismo equipo) | intérprete 8098 ms | sin opciones 1941 ms (×4,17); escritura temprana diferida 1992; **registros fijos 1821 (×4,45)**; ambas 1849 | La escritura temprana no compensa ni diferida: queda apagada. Con registros fijos el bench enteros cae de 750 a 128 MIPS (4 registros por bloque): préstamo en bucles (siguiente fila) |
| 2026-10-10 | b11861c | Recompilador con código en KSEG1 y por TLB (rec_tlb.c), cuatro variantes, nemu64-test y bench | PS3 real (mismo equipo) | intérprete 7972 ms; f416bee: sin opciones 1941, temprana 1992, fijos 1821, ambas 1849 | sin opciones 1973 ms (×4,04); temprana 2014; **fijos 1856 (×4,29)**; ambas 1897. Bench enteros con registros fijos **760 MIPS** (antes 128: el préstamo en bucles funciona) | Mismas 446 fallas, hash y ciclo que el intérprete; 232/232 unitarias en cada variante. 623 de 29 442 bloques fuera de KSEG0 (antes al intérprete). 1–3 % más lento que f416bee, dentro de lo que varió entre corridas anteriores (sin opciones 1817 → 1941); nemu64-test casi no tiene código mapeado, la medición que importa es GoldenEye/Perfect Dark |
| 2026-10-10 | 8c27863 | **GoldenEye (NTSC)** en el frontend, recompilador con código mapeado por la TLB | PS3 real (mismo equipo), respaldo del usuario | antes: el código mapeado lo corría el intérprete (~24–33 MIPS en nemu64-test) | **59,8 campos/s, 93,7 MIPS (velocidad completa), emulación 5,6 ms por campo** | 3512 de 3518 bloques mapeados, 5050 traducciones (todas en el arranque), ~0,08 % interpretado. El juego no pasa del arranque gráfico (microcódigo F3D sin soporte: ruido en pantalla, igual con el intérprete solo), así que es carga de CPU sin dibujar y sin bucles de espera. Perfect Dark: 5,1 ms por campo, también atorado en el arranque con el intérprete |
| 2026-10-10 | bd8ac59 → (esta rama) | Bucles de espera en código mapeado por la TLB (`idle.c`), GoldenEye y Perfect Dark 21,3 s de consola con el intérprete | Mac (salsa64-run nativo), respaldos del usuario | GoldenEye 0,0 % en bucles de espera, 16,2 s; Perfect Dark 0,0 % | **GoldenEye 97,6 %, 0,8 s; Perfect Dark 99,3 %** | Mismos registros, log e imagen del VI que sin el cambio. GoldenEye: su hilo ocioso es un `b .` mapeado en 0x7000071c (no dibuja por el microcódigo F3D). Pendiente medir en la PS3 |
| 2026-10-10 | be8f573 (main ce8351d) | **Perfect Dark (NTSC)** en el frontend con la HLE de Rare (F3DPD) y naudio, intro sin tocar el control | PS3 real (mismo equipo), respaldo del usuario | antes: no dibujaba (microcódigo sin HLE) | **59,9 campos/s (100 % de NTSC)**; la intro en alta resolución 50,7–56,3 campos/s (84–94 %); HLE en el SPE 1,4–8 ms por tarea, emulación 5,8–8,8 ms por campo; audio naudio con MP3 en el SPE 1,7–4,7 ms por tarea | 0 tareas cortadas, 0 comandos de otro microcódigo saltados, audio sin bloques tarde. Mario Kart 64 con el mismo paquete: 100 % de NTSC, sin regresión |
| 2026-10-10 | 524d80e | Bucles de espera en código mapeado, **GoldenEye** en el frontend con el recompilador | PS3 real (mismo equipo), respaldo del usuario | 59,8 campos/s, 93,7 MIPS, 0 % en bucles de espera, emulación 5,6 ms por campo | **59,8 campos/s, 1,2 MIPS, 99 % en bucles de espera, emulación 0,6 ms por campo** (0,5 ms es la copia del VI) | Su hilo ocioso es un `b .` mapeado en 0x7000071c; el juego no dibuja todavía (microcódigo F3D) |
| 2026-10-10 | 697922c → b3ba375 | **Caché de texturas** entre tareas (HLE gráfica en el SPE 0), Zelda OoT con el recompilador, ventanas fijas de 100 campos | PS3 real (mismo equipo), respaldo del usuario | HLE en el SPE 14,0–30,6 ms por tarea; escenas pesadas a 38–48 campos/s; peor campo 36–43 ms | **5,1–9,6 ms por tarea (×2,8–3,3); 59,8–60,2 campos/s en todas; peor campo 17–27 ms** | Decodificar texturas: 6,7–17,2 → 0,00–0,09 ms (0–1 textura decodificada por tarea); identificarlas (hash) 0,03–0,35 ms; cargas a la TMEM 3,7–7,3 → 1,4–3,0 ms (copia en bloque). Mismas capturas byte a byte. Ver «Caché de texturas» abajo |
| 2026-10-10 | f416bee | **Zelda OoT (NTSC) en el frontend**, recompilador con registros fijos, HLE gráfica en el SPE | PS3 real (mismo equipo), respaldo del usuario | intérprete: 15,4–17,5 campos/s, emulación 56–64 ms por campo | **41–60 campos/s (69–100 %), emulación 4–14 ms por campo** | Logo e intro a 58–60; título y escenas con muchos triángulos 41–55, ahora limitadas por la espera a la HLE gráfica del SPE (10–30 ms por tarea), no por el CPU. 86–88 % del tiempo del CPU en bucles de espera. ~10 500 bloques, 7,9 MB de caché, 1,7 % de instrucciones interpretadas. Se ve igual que con el intérprete (el autor) |
| 2026-10-10 | e210aeb | **Recompilador: conversiones de la FPU, LWL/LWR y MFC0 en nativo**, nemu64-test con registros fijos | PS3 real (mismo equipo), cpu-tests SLSA00077 | mismo binario con `RARE_EXEC` (como antes): 1894 ms (×4,29 del intérprete, 8118 ms) | **1816 ms (104,1 MIPS; ×4,47); −4,1 %** | Mismas 446 fallas, hash y ciclo. Entradas desde C 140 909 → 110 074; llamadas al intérprete 2,15 M → 1,77 M. Bench: cvt 4,2 → 40,7 MIPS, no alineadas 20,5 → 218,5, mfc0 9,8 → 701, espera por Count 8,3 → 488 (salidas a C 2 M → 8). Ver «Conversiones de la FPU…» abajo |
| 2026-10-10 | 136dff4 (PS3: ad14e4b con main 5a0a1a3) | **Generador de fragment programs** (Fase 4), GoldenEye NTSC en Dam, ~3 min de juego | PS3 real (Slim CECH-3011A, HFW 4.91 + HEN), app SLSA00087 «Salsa64 fpgen», respaldo del usuario | combinación lineal sola: 36–50 campos/s en Dam (60–84 %), 97 449 combiners aproximados en 80 s de host | **34–55 campos/s (57–93 %), emulación 18–29 ms por campo; 0 combiners aproximados**; 4 programas generados en toda la corrida (0 vaciados), hasta 131 de 350 dibujos por tarea con programa generado, 7–23 cambios de programa por tarea, traducción al RSX 0,3–1,8 ms por tarea | La niebla por luz (lerp(ENV, TEXEL0, SHADE_ALPHA)·SHADE) exacta en las capturas de la presa. El cuello sigue en el CPU y la HLE del SPE (5–15 ms por tarea), no en el RSX. Zelda OoT 53–60 campos/s, SM64 54–60, Mario Kart 60: 0 programas generados (todo cabe en la combinación lineal), sin regresión visual. Ver «Generador de fragment programs» abajo |
| 2026-10-11 | 3b95776 | **HLE de la tarea JPEG de Zelda OoT** en la PS3, dos salas (tienda Kokiri y casa de Link) | PS3 real (Slim CECH-3011A, HFW 4.91 + HEN), app SLSA00091 «Salsa64 Kokiri», respaldo del usuario | antes: fondo sin dibujar (malla en colores planos) | **67,8 y 67,3 ms del PPE por sala**; ventana de la carga 57,3–57,6 campos/s, peor campo 37–39 ms de emulación | ~110 veces lo de la Mac: flotantes y conversiones a entero por memoria en el PPE. Se queda en el PPE (una vez por sala, durante la transición); pendiente pasarlo a punto fijo |
| 2026-10-11 | (rama fervent-hodgkin) | **HLE de la tarea JPEG de Zelda OoT** (fondos prerrenderizados), una sala: 75 tareas, 300 macrobloques | Mac (salsa64-run nativo, Apple Silicon) | antes: las tareas terminaban sin hacer nada (fondo basura) | **0,61 ms por sala** en el hilo del CPU, una vez al cargarla | Corre en el PPE (`core/rsp.c`); IDCT separable en flotante sin optimizar. El frontend escribe «JPEG: N tareas, M macrobloques en X ms del PPE» al terminar cada tanda: con esa medida se decide si se queda en el PPE o va a un SPE. El fondo se copia en el SPE 0 (40 tiras por cuadro, reutilizadas por la caché de texturas) |

Camino lento en Banjo-Tooie (misma sesión, «Salsa64 perfil 3»): en las líneas
«muestreo» del log de la consola (cada 5 s), las ventanas pesadas sin pausa
(34–49 campos/s, emulación de 16–28 ms por campo) pasan **35–60 %** de las
muestras en el camino lento del recompilador; las ventanas a ≥ 58 campos/s,
**4,5 %** de media (164 ventanas). Las ventanas con la pausa o el menú del
sistema abiertos se excluyen: ahí la emulación espera dentro del camino lento
y lo infla. Todavía no está separado por causa (TLB, registros de hardware,
excepciones, instrucciones al intérprete).


| 2026-10-12 | 4ca4c2a → aacdcc4 | **Tarea JPEG en el SPE en 4 carriles** (IDCT y color vectorizados, mismo resultado byte a byte), cuatro cargas de sala prerrenderizada | PS3 real (Slim CECH-3011A, HFW 4.91 + HEN), app SLSA00103 «Salsa64 PPE», respaldo del autor | SPE escalar: 41,9 ms por sala (0,56 ms la tarea más larga); la emulación dormía 36 ms por sala sondeando; peor campo de la tanda 32–36 ms | **SPE 2,86 ms por sala (×14,7; 0,039 ms la tarea más larga)**; hilo de emulación 0,32–0,35 ms; dormido 7,2–7,5 ms; ventanas de 100 campos 59,6–60,9 | El autor: «no hay tirones, solo la animación de transición, pero es muy rápida». El peor campo de la tanda (30–34 ms) ya no es el JPEG: las cargas de escena sin JPEG del mismo log llegan a 30–45 ms. Contra los flotantes en el PPE de la v0.2.0: el hilo de emulación pasa de 67,8 a 0,34 ms por sala |
| 2026-10-12 | 479c0e7 → 4ca4c2a | **Tarea JPEG de Zelda OoT en un SPE** (HLE en enteros), tres cargas de sala prerrenderizada | PS3 real (Slim CECH-3011A, HFW 4.91 + HEN), app SLSA00103 «Salsa64 PPE», respaldo del autor, escala 5× | flotantes en el PPE (SLSA00091, 3b95776): **67,3–67,8 ms del hilo de emulación por sala**; peor campo 37–39 ms; ventana de 100 campos 57,3–57,6 campos/s | **SPE: 0,34–0,38 ms del hilo de emulación** (el SPE tarda 41,9 ms, 0,56 ms la tarea más larga); peor campo 31,9–36,4 ms; ventanas 59,3–59,9. Enteros en el PPE (`jpeg-ppe`): 29,0–30,2 ms del hilo 0, peor campo 30,2–30,8 (43,5 la primera), ventanas 59,7–60,0 | El tirón casi no cambia (el autor no nota diferencia): el juego espera cada tarea del RSP, así que mientras el SPE decodifica la emulación duerme en los sondeos (~36 ms por sala). Lo que queda es la velocidad del SPE (140 µs por macrobloque, escalar). Prueba del arranque en la PS3: SPE = PPE = host (CRC EEC26FE1). Ver «Quitarle trabajo al PPE» abajo |
| 2026-10-12 | 2241c32 → 4ca4c2a | **Cuánto le quita el hilo de presentación a la emulación** (medición, sin cambio de rendimiento) | PS3 real (mismo equipo y app), respaldos del autor, escala 5× | — | GoldenEye en la presa: hilo 1 despierto **2,88 ms por campo emulado (15,8 %)**, de ellos traducción 1,81. A/B con carga de mentira en la misma corrida: **~0,13 ms de emulación perdidos por ms del hilo 1** (0,80 ms → +0,10 ms; 2,63 ms → +0,37 ms). Costo actual ≈ 0,4 de 15,5 ms por campo (2,5 %) | Pasar la traducción al RSX a un SPE ganaría ~0,25 ms por campo (~+0,7 campos/s en lo peor de la presa): no vale la pena por CPU. Ver «Quitarle trabajo al PPE» abajo |
| 2026-10-12 | 8c876ac → ad18cce | **Donkey Kong 64**: la caché del recompilador ya no se vacía al cambiar Status.FR (gemelos por FR) | Mac, qemu-ppc64 (`salsa64-run --rec`, 16 s de consola, respaldo del autor) | 236 s de reloj, 1,2 MIPS; 1 284 166 bloques, 4 173 vaciados (1 340 bloques compilados por campo) | **7,5 s (×31), 38 MIPS; 8 867 bloques, 0 vaciados, 9 bloques por campo** | DK64 cambia FR en cada cambio de hilo (~260/s). 256 bloques dependen de FR, 0 gemelos |
| 2026-10-12 | 8c876ac → ad18cce (PS3: + main da2adcd) | **Donkey Kong 64** en el frontend: intro, isla y juego, ~2 min | PS3 real (Slim CECH-3011A, HFW 4.91 + HEN), app SLSA00102 «Salsa64 DK64», respaldo del autor | 12,0–13,6 campos/s (20–23 %), 1,4–1,6 MIPS, emulación 65–75 ms por campo; 445 864 bloques y 959 vaciados en 30 s | **59,6–60,0 campos/s (100 %)**, 6,8–29,8 MIPS, emulación 4,0–6,5 ms por campo; 11 908 bloques, **0 vaciados**, 4 desalojos en 2 min | Una ventana a 47,9 al cargar la isla. 756 bloques dependen de FR, 0 gemelos. Zelda y SM64 al 100 % con el mismo paquete (un vaciado cada uno: cargar un estado) |
| 2026-10-12 | 1be3ce5 → 4d75242 (rama fervent-noyce) | **Perfil persistente del recompilador y zona caliente**, GoldenEye en la presa 1–2 min: mismo binario con `sin-perfil` → 1.ª sesión (cuenta) → 2.ª sesión (762 bloques calientes juntos en 1 MB) | PS3 real (Slim CECH-3011A, HFW 4.91 + HEN), app SLSA00104 «Salsa64 perfil», respaldo del autor | 17,15 ns por instrucción (nativo 14,82 = 47 ciclos), 49,9 campos/s en las ventanas pesadas | 1.ª: 18,19 (15,54 = 50), 45,5 campos/s; 2.ª: 18,26 (15,80 = 51), 45,8 campos/s: **sin ganancia**; contar cuesta ~5 % | Perfect Dark intro: 15,10 → 15,16 ns, igual. SM64, Zelda y Majora a 59–60 campos/s. Los perfiles (24 773 bloques, 1592 sitios y 809 sucesores en GoldenEye) quedan para los pasos siguientes. Ver «Perfil persistente y zona caliente» abajo |
| 2026-10-11 | e0aa556 (v0.1.0) → c17e790 | **Perfect Dark, intro (azotea de dataDyne con desenfoque de movimiento)**: decodificar solo lo que muestrea cada TEXRECT y decodificación de 4 en 4 texels | PS3 real (Slim CECH-3011A, HFW 4.91 + HEN), app SLSA00097 «Salsa64 texturas», respaldo del usuario, sin tocar el control | 50,0–51,1 campos/s; HLE en el SPE 39,7–61,4 ms por tarea, de ellos decodificar 36,8–58,4 ms (90–143 texturas por tarea); el CPU dormía 28–40 ms por tarea esperando al SPE | **59,8–60,3 campos/s; SPE 3,3–5,0 ms por tarea (×11–17), decodificar 0,48–0,65 ms (133–181 texturas, ×77–94)**; dormida 3,0–5,5 ms | Antes a 4× automática, después a 1× pedida (la HLE no depende de la escala). Zelda, GoldenEye y Conker sin cambios. La escena siguiente (campos 4300–5500) va a 45–53 campos/s por el CPU emulado (41–63 MIPS, 18–22 ms por campo; en las ventanas más lentas casi no espera al SPE). Ver «Texturas que cambian en cada cuadro» abajo |

## Filtros CRT en el RSX (2026-10-13)

Los filtros de la presentación (docs/filtros.md) se miden en el RSX con
marcas de tiempo entre pasadas (`rsxkit_stamp`); la emulación y la
presentación, con marcas al principio y al final de cada tramo de órdenes
(cota superior: incluye lo que el RSX esperó órdenes del PPE dentro del
tramo). El log de la PS3 escribe cada 5 s:

    rsx: emulación X ms por campo emulado, presentación Y ms por campo presentado (Z % del tiempo ocupado)
    rsx: filtro «…» WxH desde wxh: T ms del RSX por cuadro (reducir …, lineal y resplandor …, …)

Costo por píxel de cada pasada (instrucciones del fragment program, de
`tools/fpo-info.py`), que es lo que manda en el RSX: reducir 27 (9
lecturas), lineal 12, resplandor 31 (12), tesela 56, puntos 143 (10), final
de la máscara 33 (7); señal 31, decodificación 49 (9) o 103 (27), líneas
51–58 + 26–31, final del tubo 44 (5). A 1080p la imagen 4:3 son 1440 × 1080
= 1,56 M píxeles; a 720p, 960 × 720 = 0,69 M; las tablas del cable miden
~710 × 240 (señal y decodificación) y 1440 × 240 o 960 × 240 (líneas), las
de la máscara ~365 × 362 o ~245 × 243 (puntos) y 12 × 6 (teselas).

### Medición en la PS3 real

PS3 real (Slim CECH-3011A, HFW 4.91 + HEN), app SLSA00109 «Salsa64
filtros» (6f2a888), respaldos del autor (NTSC), escala 5×, `filtros-ciclo`
(ninguno → máscara → RGB → S-Video → compuesto, 20 s cada uno), jugando y
no en una escena fija: GoldenEye a 720p y a 1080p, Perfect Dark a 1080p y
Conker a 1080p (en negro por la regresión del relleno de Z: sirve para el
costo del filtro, no para el del juego). Medianas de las ventanas de 5 s
(200–300 cuadros cada una), sin la primera ventana después de cada cambio.
Log `build/ps3-logs/rsp-ps3-10-udp-2026-10-13.log` (fuera de git).

**Costo por pasada**, ms del RSX por cuadro, GoldenEye (imagen de
320 × 237; a 5× la superficie del RDP mide 1600 × 1185):

| Filtro | Salida | Reducir | Lineal y resplandor | Teselas y puntos / señal y decodificación | Líneas | Final | Total |
|---|---|---|---|---|---|---|---|
| Máscara | 960 × 720 | 3,07 | 1,15 | 0,63 | — | 1,83 | **6,69** |
| RGB | 960 × 720 | 3,07 | 1,15 | 0,88 | 1,09 | 2,25 | **8,44** |
| S-Video | 960 × 720 | 3,07 | 1,15 | 2,04 | 1,09 | 2,25 | **9,60** |
| Compuesto | 960 × 720 | 3,07 | 1,15 | 4,08 | 1,34 | 2,25 | **11,89** |
| Máscara | 1440 × 1080 | 3,12 | 1,15 | 1,26 | — | 4,11 | **9,64** |
| RGB | 1440 × 1080 | 3,12 | 1,15 | 0,88 | 1,66 | 5,04 | **11,86** |
| S-Video | 1440 × 1080 | 3,12 | 1,16 | 2,05 | 1,66 | 5,04 | **13,02** |
| Compuesto | 1440 × 1080 | 3,12 | 1,16 | 4,09 | 2,02 | 5,04 | **15,43** |

Perfect Dark a 1080p (320 × 218): máscara 9,29, RGB 11,30, S-Video 12,39,
compuesto 14,62 (reducir 2,87); con la imagen de alta resolución
(576 × 474) la máscara sube a 12,07 (reducir 4,92). Conker a 1080p
(292 × 213, la imagen en negro llega a 1×): reducir 0,10, máscara 6,41,
S-Video 9,42, compuesto 11,59.

- Las pasadas siguen el modelo de instrucciones por píxel de arriba con
  ~12 G instrucciones por segundo (24 unidades a 500 MHz): el final de la
  máscara da 4,28 ms calculados contra 4,11 medidos; puntos, señal,
  decodificación, líneas y el final del tubo, entre 0,7 y 1,1 veces lo
  calculado (lineal y resplandor, 1,3). Lo que va por píxel de salida crece 2,25 veces de 720p a
  1080p; lo que va sobre la imagen chica (reducir, lineal, resplandor,
  señal) no cambia.
- **Reducir es la excepción**: 3,1 ms donde sus 27 instrucciones por píxel
  darían 0,17. No cambia con la escena (3,11–3,13 ms en todas las ventanas
  de GoldenEye a 1080p) ni con el juego en pausa (Zelda con el menú de
  pausa, sin nada del juego en el RSX: 3,13), así que no es esperar al
  render del juego: es leer la superficie de 5× (7,6 MB a ~2,5 GB/s). Con
  la misma imagen de Zelda, cambiando la escala desde la pausa: **5× 3,12
  ms, 3× 1,73, 1× 0,13**. Crece casi en línea con la escala y no con el
  área: lo que cuesta es cuántas filas distintas lee cada píxel de la
  salida (un bloque de f × f de una superficie lineal de 6400 bytes por
  fila; rsxkit no usa las regiones con tiles del RSX).
- **Lo que pasa en la pantalla** (GoldenEye; «presentados» por segundo,
  con los campos de la N64 que se emularon):

  | | 720p: RSX ocupado | presentados | 1080p: RSX ocupado | presentados | campos/s (1080p) |
  |---|---|---|---|---|---|
  | Sin filtro | 39 % | 60 | 44 % | 59 | 59,0 |
  | Máscara | 62 % | 50 (todos) | 81 % | 51 | 53,4 |
  | RGB | 83 % | 58 (todos) | 82 % | 40 | 59,3 |
  | S-Video | 86 % | 51 | 84 % | 36 | 56,5 |
  | Compuesto | 86 % | 40 | 82 % | 30 | 59,6 |

  El juego usa 5,5–5,7 ms del RSX por campo («emulación», cota superior) y
  el campo dura 16,7 ms. A 720p caben la máscara y RGB (se presentan todos
  los campos); S-Video pierde ~6 % de los campos presentados y el
  compuesto ~30 %. A 1080p la máscara casi cabe (~4 % saltados); RGB
  pierde un tercio, S-Video ~40 % y el compuesto la mitad. La emulación no
  se frena: el PPE no espera al filtro (los campos/s cambian con la
  escena); con los cables a 1080p el hilo de presentación duerme 3–5 ms por
  campo esperando al RSX.
- **Lo dibujado en la PS3 contra la referencia** (`tools/crt-ref.c`, las
  capturas `filtro-*-fuente.ppm` y `-pantalla.ppm` de la misma corrida):
  RGB, S-Video y compuesto contra el modelo del RSX (`-rsx`), promedio
  0,06–0,10/255, a 1 o menos en el 99,0–99,7 % y hasta 20–31 en bordes;
  contra el original, promedio 0,17–0,27, a 1 o menos en el 96–98 %. Es lo
  mismo que en RPCS3. La máscara a 720p da 1/255 o menos en todo píxel (la
  captura de 1080p salió de un cuadro negro). La comprobación de la app en
  la consola: luz lineal con error 0,0010 y resplandor con 0,00001 contra
  el bilineal.

### Cómo bajar el costo (propuestas, sin implementar)

**(a) Bajar la escala interna cuando hay un CRT.** El filtro trabaja sobre
la imagen a la resolución de la N64; la escala interna solo le llega como
supermuestreo (la reducción promedia f × f muestras por píxel).

- Lo que se ahorra: *reducir* pasa de 3,12 ms a 0,13 a 1× y a 1,73 a 3×
  (medido) y a ~0,9 a 2× (interpolado). El *render del juego* (5,5–5,7 ms
  por campo a 5×) tiene 6,25 veces menos píxeles a 2× y 25 veces menos a
  1×; lo que va por dibujo (200–380 por tarea) no baja. Estimado: 3–4 ms
  menos a 2× (falta medirlo con el juego corriendo). En total, **~5–6 ms
  menos por campo a 2×**: a 1080p con el compuesto, de ~21 ms (5,7 + 15,4)
  a ~15–16, justo dentro de los 16,7 ms; los cuatro filtros cabrían.
- Si se nota: prueba en la Mac con una escena sintética (bordes, un cable
  de 0,3 y uno de 0,6 píxeles y un piso con tablero sin mipmaps),
  dibujada con 1, 2, 3 y 5 muestras por eje, reducida y pasada por
  `crt-ref`. Contra 5×: a **2×**, promedio 0,5–0,6/255, p99 11–14; lo único
  que cambia a la vista es el cable de 0,3 píxeles, que queda un poco
  punteado. A **1×**, promedio 1,6–1,8, p99 33–41: los cables finos salen
  como cuentas, el piso lejano hace moiré y en movimiento eso centellea. A
  3×, p99 5–7: igual a la vista. Los escalones de los bordes los tapa el
  CRT en las tres. Imagen de la comparación:
  `build/ps3-1/escala-crt-1x-2x-5x.png` (fuera de git; columnas 1×, 2× y
  5×; filas RGB y compuesto).
- Se puede medir ya, sin paquete nuevo: la escala se cambia desde la
  pausa (Imagen) o en Opciones; con `filtros-ciclo`, GoldenEye a 1080p a
  5×, 3×, 2× y 1×, jugando. El log da reducir, emulación, ocupado y
  presentados.
- Cambio propuesto (chico): una escala propia para cuando hay un CRT (2×
  por omisión, 1× para quien prefiera la imagen sin supermuestreo, como
  la consola), que se aplica al elegir el filtro y se deshace al quitarlo,
  sin reiniciar (eso ya existe).

**(b) Señal, resplandor y líneas en un SPE.** El RSX deja la imagen chica
en la memoria principal, el SPE calcula y el RSX lee el resultado como
textura.

- Vale solo para **señal y decodificación de los cables**: trabajan sobre
  ~710 × 237 muestras (el doble en compuesto) y el resultado es chico
  (1,35 MB en S-Video, 2,7 MB en compuesto, en fp16). El resplandor no: el
  RSX lo hace en ~1 ms gracias a su filtro bilineal (12 lecturas
  bilineales en 640 × 474), y en un SPE serían ~4–5 ms (48 lecturas y ~48
  FMA por píxel). Las líneas tampoco: su tabla mide lo que la salida
  (1440 × 237, dos tablas, 5,5 MB) y mandarla cuesta más que calcularla
  (1,7–2,0 ms en el RSX). La máscara no tiene qué mover: lo caro va por
  píxel de salida (final 4,1 ms y puntos 1,3 a 1080p).
- Estimado del SPE (4 carriles, una FMA por ciclo, los corrimientos en el
  otro pipe): ~22 ciclos por muestra con 27 tomas. S-Video ~1,2 ms en un
  SPE, compuesto ~2,3 (1,2 en dos), RGB ~0,5. Se reservan 6 SPE y se usan 3
  (HLE gráfica, audio, JPEG).
- La ida y vuelta: la reducción escribe la imagen de 320 × 237 en la
  memoria principal (300 KB, ~0,05 ms); el RSX escribe una etiqueta, el SPE
  la sondea, calcula, deja la tabla y escribe otra etiqueta; el RSX la
  espera (sin el PPE de por medio) antes de las líneas y lee la tabla desde
  la memoria principal (+0,3–0,9 ms estimados en las líneas: el RSX lee
  más lento de la XDR). Mientras, el RSX hace lineal y resplandor
  (1,15 ms): si el SPE termina antes, no espera, y no se agrega latencia.
  Hacerlo un cuadro atrás para no esperar agregaría 16,7 ms de retraso.
- Ahorro neto estimado: **compuesto 2,5–3,5 ms (con dos SPE), S-Video 1–1,5, RGB ~0,3 (no
  vale la pena), máscara nada**. Trabajo: superficies en la memoria
  principal y una espera del RSX en una etiqueta dentro de rsxkit
  (genérico, MIT), el programa del SPE en la parte GPL, el enganche en el
  PPE y la comparación contra `crt-ref` (en fp32, más cerca del original
  que las tablas fp16).

**Además:**

- **(c) No rehacer el filtro si la imagen no cambió.** GoldenEye y Perfect
  Dark dibujan 20–30 imágenes por segundo, pero el filtro corre en cada uno
  de los 60 campos, y los cuatro filtros son fijos para una imagen fija
  (`frame_phase` = 0). Volver a presentar el resultado anterior (o no
  hacer el flip) en los campos repetidos ahorra entre la mitad y dos
  tercios del filtro en esos juegos; nada en los que van a 60.
- **(d) Reducir más barato sin bajar la escala.** Como el costo va por
  filas leídas, dos pasadas (primero a lo ancho, 1600 × 1185 → 320 × 1185,
  leyendo filas seguidas; después a lo alto, sobre la tabla chica) o la
  superficie del RDP en una región con tiles del RSX (`gcmSetTileInfo`;
  hoy ninguna superficie las usa, y también podría abaratar el propio
  render a 5×) deberían bajar buena parte de los 3,1 ms conservando el
  supermuestreo. Es barato de probar; a medir.

**Orden recomendado:** (a) y (d) primero (pocas líneas, todos los
filtros); (c) también es barato; (b) solo si el compuesto a 1080p sigue
sin caber después de esas.

### Medido en la PS3: tope de 3×, dos pasadas, reuso y SPE (2026-10-13)

PS3 real (mismo equipo), app SLSA00109 (674ab5a, `build/ps3-5`),
GoldenEye en la presa a 1080p con 5× elegida (el tope la deja en 3×),
jugando. Corrida B: `filtros-ciclo` con los cinco CRT, ~10 min, ~5 vueltas
que alternan la reducción en dos pasadas y en una. Corrida A:
`filtros-ciclo` «crt-svideo crt-compuesto» con `filtros-spe`, 3 vueltas
completas que alternan SPE y RSX (un apagón la cortó a los 4 min). Logs en
`build/ps3-5/` (fuera de git); medianas de las ventanas de 5 s. El reporte
del filtro se cortaba a 320 caracteres: el trabajo del SPE y el promedio
con imagen repetida no llegaron completos (ahora van en líneas aparte).

| Filtro (1080p, ms del RSX por cuadro) | 5×, una pasada (antes) | 3×, una pasada | 3×, dos pasadas | Campos presentados (3×, dos pasadas) |
|---|---|---|---|---|
| Máscara | 9,64 | 8,06 | **7,38** | todos |
| RGB | 11,86 | 10,23 | **9,37** | todos |
| S-Video | 13,02 | 11,18 | **10,63** | todos |
| Compuesto | 15,43 | 13,88 | **13,21** | todos (con una pasada, ~10 por segundo saltados) |
| Trinitron | — | 10,70 | **10,05** | todos |

- **Reducir**: 3,12 ms (5×, una pasada) → 1,82–1,87 (3×, una) → 1,15–1,25
  (3×, dos). Las dos pasadas ahorran ~0,65 ms y son lo que hace caber al
  compuesto.
- **Render del juego** («emulación», RSX por campo): 5,5–5,7 ms a 5× →
  2,05–2,25 a 3×. Con el filtro, el RSX va al 50–82 % (antes, 81–86 % y
  saturado) y con dos pasadas no se salta ningún campo en los cinco; la
  emulación queda limitada por el CPU (52–57 campos/s, como sin filtro en la
  presa).
- **Imagen repetida (reuso de las tablas)**: en la presa GoldenEye manda una
  tarea gráfica por campo (100 por cada 100), así que repite poco (0–12 %
  de los cuadros); con solo la pasada final, 2,4–5 ms. Sirve en escenas
  quietas y menús, no en la presa.
- **Preparar un filtro**: 73–86 ms la primera vez (con la comprobación,
  bajo «Aplicando filtro…»), 1–4 ms después.

**SPE** (corrida A, ms del RSX por cuadro con filtro):

| | RSX | SPE |
|---|---|---|
| S-Video, total | 10,77 | 10,32 |
| S-Video, señal y decodificación | 2,05 | 1,65 (espera al SPE ~1,9 desde que acaba el resplandor, más linealizar) |
| Compuesto, total | 12,86 | 13,37 |
| Compuesto, señal y decodificación | 3,92 | 4,07 (espera ~4,5) |

- La ida y vuelta (de la copia de la imagen a pasar la espera) es ~2,5 ms
  en S-Video. El SPE tarda unas tres veces lo estimado (~90–100 ciclos por
  muestra contra ~30), así que el RSX termina lineal y resplandor (1,1 ms) y
  se queda esperando. Sin fallas ni vencimientos del vigilante.
- Neto: S-Video 0,45 ms menos; compuesto 0,5 ms **más**. Con el tope y las
  dos pasadas ya caben los cinco: el SPE no hace falta para 1080p.
- Para que pague, lo probable es la latencia: cada canal suma sus 27 tomas
  en una sola cadena de FMA (6 ciclos cada una). Con varias sumas parciales
  independientes y dos columnas entrelazadas debería acercarse a 25–30
  ciclos por muestra (S-Video ~0,8 ms y compuesto ~1,6 ms por SPE): a medir.
## Fastmem y base en caché en el host (2026-10-13)

Diseño y detalle en docs/fastmem.md. Medición en el host con los volcados
(`SALSA64_REC_DUMP=1 SALSA64_REC_PROF=12000 SALSA64_REC_PROF_TOP=4000
SALSA64_REC_PIN=1`, `tools/rec-expansion.py --counter 8`, pesado por
ejecuciones): palabras de PPC por instrucción MIPS, calientes (las que se
ejecutan) y frías. La PS3 está pendiente (fastmem depende de que lv2 avise
de los fallos de página, que hoy con HEN no hace; la base en caché no
depende de nada y va en el mismo paquete con `jit-sin-grupos` para el A/B).

| | GoldenEye hasta la presa (7,5 G ciclos) | Perfect Dark intro (3 G) |
|---|---|---|
| v0.2.2 (código compacto) | 8,16 calientes + 7,40 frías = 15,57 | 6,84 + 6,50 = 13,34 |
| fastmem (`SALSA64_REC_FASTMEM=1`; ventana con SIGSEGV) | **6,87** + 10,06 = 16,93 (calientes −16 %) | — |
| base en caché (por omisión; con el arreglo del delay slot likely y FCR31 en T_V) | **7,65** + 7,20 = 14,85 (calientes −6 %) | **6,39** + 6,10 = 12,49 (−7 %) |
| base en caché + FPU compacta (por omisión; docs/fastmem.md §9) | **5,54** + 7,27 = 12,81 (calientes **−32 %**) | **5,22** + 6,05 = 11,27 (**−24 %**) |

En la PS3 (2026-10-13, paquete con la base en caché antes del arreglo):
GoldenEye en la presa 16,63 ns por instrucción (12 ventanas pesadas) contra
16,03/16,30 de v0.2.2: sin ganancia; Perfect Dark, primera ventana pesada,
10,07 contra 10,51 sin grupos (−4 %). Con la FPU compacta (paquete
`build/ps3-4`, sin banderas): GoldenEye en la presa **15,66 / 15,54**
(media / mediana de 13 ventanas pesadas; −2…−4 % contra v0.2.2, −6 % contra
la corrida A) y Perfect Dark en la segunda escena **14,1–14,7** contra ~15,4
(−5…−8 %). La ganancia de huella del host no se traslada entera: el
`bl`/`blr` por operación y lo que no es el PPE (docs/fastmem.md §9).

Todos son exactos en `--lockstep` (GoldenEye hasta la presa y Perfect
Dark sin diferencias; nemu64-test entero con la base en caché; con fastmem
nemu64 diverge solo en las pruebas de accesos no alineados y de direcciones
sin extensión de signo, que son excepciones que fastmem no lanza:
`tests/roms/nemu64-test.fastmem.fallas`). Fastmem parchea 282 (GoldenEye)
y 764 (Perfect Dark) sitios en toda la corrida: el fallo no es un costo.

Censo de accesos a datos (intérprete, `-DSALSA64_MEM_CENSUS`, 11 juegos):
KSEG0 97,9–99,97 %, KSEG1 ≤ 0,6 %, por la TLB 2 % en Perfect Dark y 0,1 %
en GoldenEye, 0 no alineados; y el 70–77 % de los accesos comparten base
con otro del mismo bloque (docs/fastmem.md, secciones 2 y 8).

## Quitarle trabajo al PPE: el hilo 1 y la tarea JPEG (2026-10-12)

PS3 real (Slim CECH-3011A, HFW 4.91 + HEN), app SLSA00103 «Salsa64 PPE»
(4ca4c2a), respaldos del autor (NTSC), escala 5×, una sola ejecución con el
menú: GoldenEye en la presa sin carga de mentira, la intro de Perfect Dark
sin tocar el control, GoldenEye otra vez con `carga-hilo1` («1000 3000»),
SM64 con la carga, Zelda con la tarea JPEG en el SPE y Zelda con
`jpeg-ppe`. Log `build/ps3-logs/ppe-hilo1-jpeg-2026-10-12.log` (fuera de
git). Método en docs/hilos-ppe.md, «Cuánto le quita el hilo de presentación
a la emulación».

### Trabajo del hilo 1 (ms por campo emulado, media de las ventanas de 5 s)

| Escena | Despierto (% del tiempo) | Traducción | Copias a la RDRAM | Presentar | Otros | Dormido en barreras del RSX | Emulación (hilo 0) |
|---|---|---|---|---|---|---|---|
| GoldenEye, presa (30 ventanas) | **2,88 (15,8 %)** | 1,81 | 0,43 | 0,11 | 0,52 | 0,01 | 15,5 ms, 55,8 campos/s (mín. 45,2) |
| GoldenEye, ventanas a 45–50 campos/s (8) | 3,2–4,4 (15–22 %) | 2,1–2,9 | 0,33–0,48 | 0,10 | 0,45–1,37 | 0–0,02 | 19,7–21,8 ms |
| Perfect Dark, intro entera (19 ventanas) | 2,21 (12,6 %) | 0,82 | 0,52 | 0,17 | 0,34 | 0,37 | 13,3 ms |
| Perfect Dark, escena del desenfoque (t 214–229 s) | 3,3–4,5 (20–27 %) | 0,46–0,51 | **1,21–1,54** | 0,28–0,33 | 0,24–0,34 | **1,06–1,83** | 7–12 ms (60 campos/s) |
| Perfect Dark, segunda escena (CPU; 5 ventanas) | 1,64 (8,2 %) | 0,73 | 0,33 | 0,15 | 0,43 | 0,01 | 18,3 ms, 50,0 campos/s |
| SM64 (sin contar la carga) | ~0,97 | 0,24 | 0,19 | 0,13 | 0,40 | 0,01 | 4,8 ms |
| Zelda (dos vueltas, 21 ventanas) | 1,1–1,3 (7 %) | 0,48–0,54 | 0,12–0,15 | 0,17–0,25 | 0,20–0,32 | 0–0,13 | 7–8 ms |

Texturas decodificadas en el PPE: 0 en todas. La traducción por tarea, a
5×, va a 2,3–3,6 ms en la presa (1,0–3,7 en Zelda). El resto del tiempo el
hilo 1 duerme (semáforo y flip anterior: 4–13 ms por campo).

### Lo que le cuesta a la emulación

**A/B en la misma corrida** (GoldenEye en la presa con `carga-hilo1`,
fases de medio segundo; 24 ventanas, ~2 200 campos por fase):

| Fase | Carga dentro del campo | Emulación por campo | ns por ciclo emulado | Diferencia |
|---|---|---|---|---|
| 0 µs | 0 | 14,75 ms | 17,82 | — |
| 1000 µs | 0,80 ms | 14,85 ms | 17,87 | +0,10 ms (+0,3 % ns/ciclo) |
| 3000 µs | 2,63 ms | 15,12 ms | 18,48 | +0,37 ms (+3,7 % ns/ciclo) |

**Pendiente: ~0,13 ms de emulación por ms de trabajo del hilo 1** (0,125 y
0,14), casi en línea en ms por campo; con 3 ms de carga los ns por ciclo
suben algo más (la L2 compartida), sin salto. El hilo 0 está detenido la
mayor parte del tiempo (unos 47 ciclos del PPE por instrucción emulada,
docs/estado.md): el otro hilo usa los ciclos que él deja libres.

**Solape natural** (campos sin carga, la misma presa): los ns por ciclo no
suben con el solape (0,1–1 ms: 18,01; 1–3 ms: 17,71; ≥ 3 ms: 17,55), de
acuerdo con una pendiente chica. En SM64 la medición no dice nada (89 % en
bucles de espera: la carga cae en la espera, +0,08–0,12 ms por campo).

**Costo actual del hilo 1** (pendiente × trabajo): GoldenEye en la presa
≈ 0,38 de 15,5 ms por campo (2,5 %); en las ventanas pesadas ≈ 0,5 de 21
(2,3 %); Perfect Dark, segunda escena, ≈ 0,21 de 18,3 (1,2 %).

**Conclusión para el paso 4 (traducción al RSX en un SPE): no por CPU.**
Moverla entera a un SPE devolvería ~0,25 ms por campo en la presa (1,8 ms
de traducción × 0,13), del orden de +0,7 campos/s en lo peor (45 → 46), y
pide un rediseño grande (rsxkit es de un solo hilo, los fragment programs se
generan y cachean en el PPE). Lo que limita es el código nativo del
recompilador (79 % del hilo 0 en la presa).

### Tarea JPEG de Zelda (salas prerrenderizadas)

Tres cargas de sala con el JPEG en el SPE y cuatro con `jpeg-ppe` (la
misma ruta: la tienda Kokiri y la casa de Link). Cada carga son 75 tareas y
300 macrobloques.

| Variante | Hilo de emulación por sala | Otro procesador | Peor campo de la tanda | Ventana de 100 campos |
|---|---|---|---|---|
| Flotantes en el PPE (2026-10-11, SLSA00091) | 67,3–67,8 ms | — | 37–39 ms | 57,3–57,6 campos/s |
| Enteros en el PPE (`jpeg-ppe`) | **29,0–30,2 ms** | — | 30,2–30,8 ms (43,5 la primera) | 59,7–60,0 |
| Enteros en el SPE, escalar (4ca4c2a) | **0,34–0,38 ms** (0 esperas, ~360 sondeos) | SPE 41,9 ms (0,56 ms la tarea más larga) | 31,9–36,4 ms | 59,3–59,9 |
| Enteros en el SPE, 4 carriles (aacdcc4) | **0,32–0,35 ms** (0 esperas, ~74 sondeos) | **SPE 2,86 ms** (0,039 ms la tarea más larga) | 30,4–34,2 ms (lo de la carga de la sala, no del JPEG) | 59,6–60,9 |

- El hilo 0 queda libre (×180 contra los flotantes), pero **el tirón casi
  no cambia** (el autor no nota diferencia; la primera salida de la casa,
  «un pelín más rápida» con el SPE): el juego espera cada tarea del RSP
  antes de mandar la siguiente, así que mientras el SPE decodifica la
  emulación duerme en los sondeos (36 ms por sala). Lo que pesa es la
  duración de la decodificación, en el procesador que sea.
- El SPE escalar iba a 140 µs por macrobloque (más lento que el PPE en
  enteros, 100 µs): cargas de 16 bits desde bytes, productos de 16 × 16
  uno por uno. **Vectorizado** (`src/rsp/jpeg/jpeg_v.c`, aacdcc4: la IDCT y
  el color en 4 carriles con `mpya` y `shufb`, el mismo resultado byte a
  byte), **9,5 µs por macrobloque: 2,86 ms por sala** (×14,7). La
  emulación ya casi no espera (un sondeo por tarea, 100 µs dormido cada uno:
  7,2–7,5 ms por sala, repartidos en 6 campos) y **el autor ya no nota el
  tirón** («solo la animación de transición, pero es muy rápida»). El peor
  campo de la tanda (30–34 ms) es la carga de la sala en el CPU emulado: en
  el mismo log, cargas de escena sin JPEG llegan a 30–45 ms. Log
  `build/ps3-logs/ppe-jpeg-vector-2026-10-12.log`.
- Prueba del arranque en la PS3 real: SPE, PPE y host dan el mismo CRC
  (EEC26FE1) en los seis arranques con el SPE escalar (3,5 ms) y con el
  vectorizado (2,0 ms).

## Pila del SPE 0 (2026-10-12)

En el SPE la pila baja desde lo alto del local store hasta el final del
programa y nada avisa si se pasa: pisa las últimas variables del bss (en el
SPE 0, las lecturas por DMA de vértices, matrices y cargas de la TMEM).
Medido con `tools/spu-stack.py` (marcos de `-fstack-usage` y grafo de
llamadas de `spu-objdump`; la cadena más honda es el fondo S2DEX de las
salas prerrenderizadas de Zelda → TEXRECT → estado de dibujo → combiner):

| Versión | Texto | bss | Libre (`_end`–`__stack`) | Cadena más honda | Holgura |
|---|---|---|---|---|---|
| b445531 (texturas de Perfect Dark) | 102 624 | 146 704 | ~11 980 | 9 056 | ~2 930 |
| 11ff7d1 (más arreglos gráficos) | 106 776 | 146 704 | 7 648 | 9 472 | **−1 824** |
| 8c876ac (efectos de Majora's Mask) | 107 904 | 146 704 | 6 496 | 9 504 | **−3 008** |
| este arreglo, sobre 8c876ac | 102 560 | 145 168 | 13 408 | 9 600 | **3 808** |
| con el desenfoque de Perfect Dark y S2DEX en `-Os` | 103 744 | 145 296 | 12 128 | 9 488 | **2 640** |
| más el combiner memorizado (b8d9041) | 105 488 | 145 680 | 9 952 | 9 504 | **448** |
| más el directorio de la caché de texturas en 896 entradas | 105 984 | 142 096 | 13 024 | 9 504 | **3 520** |

La rama de los arreglos gráficos sumó 4,1 KB de código (estado de dibujo,
TEXRECT por tile, decodificación con máscara, blender con constante) y
duplicó el marco de `gfx_texrect` (416 → 832 bytes); la de Majora, 1,1 KB
más. El arreglo:

- El combiner (`combine.c`) se compila con `-Os` en el SPE 0: 16 768 →
  11 152 bytes de código (los marcos crecen 350 bytes en la cadena). Corre
  una vez por estado de dibujo; **falta medir en la PS3 real** si el
  «estado» de la línea de la HLE (ms por tarea) cambia.
- El cálculo por tile de `gfx_texrect` va en una función aparte: su marco
  baja a 464 bytes (−368 en la cadena, +272 de código).
- El registro de texturas del arena por tarea, de 64 a 32 entradas (−1 536
  bytes de bss): solo se usa con la caché llena o apagada.

El build ahora lo comprueba: `gfx_STACK_MIN` (1,5 KB) en
`src/frontend/Makefile`; si la holgura baja de ahí, el enlace de `spu_gfx`
falla con la cadena más honda.

**El combiner en `-Os` en la PS3 real** (2026-10-12, paquete SLSA00101 con
98def88; medianas de las ventanas de 100 campos contra c17e790 a 3× y la
rama de los arreglos gráficos): el «estado» de la HLE en el SPE sube ×2,7–3,6
en todos los juegos (Zelda OoT 0,80 → 2,77 ms por tarea, GoldenEye 2,70 →
7,26, Conker 0,36 → 1,29, Perfect Dark 1,93 → 2,92) y, donde el SPE manda,
cuesta campos/s: la escena pesada de la intro de Conker (campos 4400–4999)
baja de 51,8–58,3 a 44,1–50,5 campos/s con el SPE en 33,7–39,0 ms por
tarea. Volver a `-O2` no cabe (holgura −1 360 bytes sobre 98def88).

Arreglo: el resultado del combiner se guarda con sus entradas (el combiner,
PRIM, ENV, PRIM_LOD, K4, K5, el tipo de ciclo y si hay niebla:
`struct gfx_ccmemo`) y un estado de dibujo con las mismas entradas lo copia
en vez de recalcularlo (`combine_cached` y `combine_keep` en `rdp.c`, aparte
para no sumar a la cadena de la pila; copias por palabras en vez de
`memcpy`, que en el SPE se expandía a más de 1 KB por copia). Holgura sobre
98def88: 1 728 bytes. Con las texturas desde un destino encima, el
directorio de la caché de texturas pasa a 896 entradas (holgura 3 520).

En la PS3 real (paquete SLSA00101 sobre ece6049, 1080p a 5×; medianas de
las ventanas de 100 campos; «antes» = c17e790, «-Os» = el paquete con el
combiner en `-Os` sin memorizar):

| Juego | Campos/s (mín.) | SPE por tarea | Estado: antes → -Os → memorizado |
|---|---|---|---|
| Zelda OoT | 59,8 (56,6) | 5,16 ms | 0,80 → 2,77 → 1,38 ms |
| GoldenEye (Dam) | 59,7 (41,6, CPU) | 6,37 ms | 2,70 → 7,26 → 1,82 ms |
| Conker (intro) | 59,8 (49,3) | 4,81 ms | 0,36 → 1,29 → 0,78 ms |
| Perfect Dark (intro) | 59,8 (42,8, CPU) | 4,23 ms | 1,93 → 2,92 → 0,70 ms |

En la escena pesada de Conker (campos 4400–4999) el «estado» queda en
4,1–7,6 ms (antes 2,5–3,9, con `-Os` 7,5–11,8) y los campos/s en
49,3–55,4 (antes 51,8–58,3): pendiente en docs/estado.md. Las expulsiones
de la caché de texturas con 896 entradas, en GoldenEye, iguales que con
1024 (0–2,2 por tarea).

## El desenfoque de Perfect Dark desde el destino del RSX (2026-10-12)

Rama wonderful-jepsen (docs/framebuffer.md, «Texturas desde un destino sin
decodificar»): las 180 franjas por tarea del cuadro anterior se reconocen
como de su framebuffer y se leen del RSX a la resolución interna; la HLE ya
no las decodifica (`GFX_TEX_TARGET`).

**Host** (Mac, `salsa64-run` nativo con `-DSALSA64_GFX_TRACE`,
`--gfx-frame` y `SALSA64_TEXSTAT=100`, sin tocar el control; antes = main
b445531). Por tarea gráfica, ventanas de 100 campos:

| Campos | Texturas pedidas | Decodificadas | Texels decodificados | Sin decodificar (desde un destino) | Texturas distintas en la ventana |
|---|---|---|---|---|---|
| 2029–2129 | 198,3 | 180,1 → 0,1 | 115 632 → 72 | 0 → 180,0 | 23 717 → 92 KB |
| 2129–2229 | 207,2 | 180,7 → 0,5 | 116 376 → 519 | 0 → 180,0 | 23 821 → 196 KB |
| 2229–2329 | 222,7 | 180,0 → 0,0 | 115 583 → 23 | 0 → 180,0 | 23 817 → 192 KB |
| 2329–2430 | 235,4 | 180,3 → 0,2 | 115 861 → 141 | 0 → 180,0 | 24 326 → 228 KB |
| 2430–2530 | 292,6 | 180,3 → 0,3 | 115 854 → 253 | 0 → 180,0 | 23 906 → 281 KB |

En toda la corrida (1622 tareas hasta el campo 2600): 34,2 → 0,5 texturas
decodificadas por tarea y 33,6 → 0,0 desalojadas de la caché. El
rasterizador leyó 59 260 franjas de su imagen y decodificó 360 desde la
RDRAM (dos tareas al empezar el efecto, con el destino copiado para el
CPU). La HLE en el SPE no se mide en el host; en la PS3 decodificar las
franjas costaba 0,48–0,65 ms por tarea (arriba).

Capturas en campos fijos, antes contra después: la pausa de Zelda OoT
(16, `zelda-oot.pad` con la partida del autor), Banjo (12), SM64 (15), Mario
Kart (19), GoldenEye (10) y Conker (11) idénticas byte a byte; gfxtest 35/35
en sus tres variantes (`tools/test-roms.sh`) y en RPCS3 con el frontend
(`tools/test-phase2.sh`: 45 texturas del RSX, 0 decodificadas en el PPE).
En la pausa de Zelda el consumidor decodificó 360 franjas de 320 × 6: las
40 del fondo guardado para el CPU en las tres tareas después de cada una de
las tres copias. Perfect Dark (15): sobre b445531, distintas solo en las
180 filas de las franjas, en 1 nivel de 255 (hasta 3 en el campo 2600),
porque la imagen del host tenía 8 bits por canal y la copia en la RDRAM 5.
Sobre main da2adcd, con los destinos de 16 bits del host a 5 bits por
canal, 14 de 15 idénticas; en el campo 2000, al empezar el efecto, 11
píxeles casi negros difieren en un paso de 5 bits (0 contra 8 en un canal),
sin causa identificada. Con esa base, las de la pausa de Zelda, SM64, Mario
Kart, GoldenEye, Banjo y Conker siguen idénticas, y gfxtest 42/42 en el
host y en RPCS3.

**SPE 0** (`spu-size` y `tools/spu-stack.py`, sobre main da2adcd): texto
102 560 → 103 744 bytes (+2 080 de las texturas desde un destino y −896 de
compilar los fondos de S2DEX con `-Os`), bss +128 (la tabla de color
images). Holgura de la pila: 3 808 → 2 640 bytes; sin el S2DEX en `-Os`
quedaba en 1 632, apenas sobre el mínimo de 1 536 que comprueba el build.
Con el combiner memorizado encima bajaba a 448, y el directorio de la caché
de texturas pasó de 1024 a 896 entradas (−3,5 KB; en el host, el máximo de
texturas distintas en 100 campos es 428, GoldenEye en Dam): 3 520 («Pila
del SPE 0», arriba).

**PS3 real** (2026-10-12, Slim CECH-3011A, HFW 4.91 + HEN; paquete
SLSA00101 «Salsa64 desenfoque», 7b20d0b sobre main 98def88; 1080p, escala
5× pedida; Perfect Dark NTSC, respaldo del autor, intro sin tocar el
control). Contra c17e790 a 3× (la versión anterior, mismas ventanas):

| Campos | Campos/s | Texturas decodificadas | Sin decodificar (del RSX) | SPE | Estado | Decodificar |
|---|---|---|---|---|---|---|
| 1800–1899 | 59,8 → 59,8 | 133,3 → 0,1 | 0 → 180 | 3,29 → 4,66 ms | 1,55 → 2,89 | 0,48 → 0,01 |
| 2000–2099 | 59,8 → 59,8 | 176,9 → 0,5 | 0 → 180 | 4,16 → 5,78 ms | 1,91 → 3,50 | 0,65 → 0,03 |
| 2400–2499 | 59,9 → 59,7 | 144,9 → 0,0 | 0 → 180 | 10,10 → 14,30 ms | 2,86 → 7,04 | 0,54 → 0,00 |
| 3000–3099 | 59,8 → 59,8 | 178,5 → 0,0 | 0 → 180 | 6,01 → 8,11 ms | 2,14 → 4,21 | 0,67 → 0,00 |
| 3800–3899 | 60,1 → 59,8 | 175,2 → 0,0 | 0 → 180 | 6,02 → 8,27 ms | 2,17 → 4,39 | 0,64 → 0,00 |

- 250 560 franjas leídas del destino del RSX; el PPE decodificó 760, en las
  tareas en que empieza el efecto (el destino recién copiado para el CPU,
  como en el host). El autor: el desenfoque se ve «sutil pero visible».
- Los campos/s no cambian; las bajadas (2500–2699, 3700) son las de antes,
  por el CPU.
- El «estado» del SPE casi se duplica aunque ya no se decodifica ni se
  identifica por hash: cada franja es un dibujo con estado nuevo y llama al
  combiner (180 veces por tarea), que desde da2adcd va con `-Os` en el SPE
  0 (pila); la rama de los arreglos gráficos también agregó trabajo al
  estado (blender). Sin aislar. No cuesta campos/s en Perfect Dark (14 ms
  como mucho, contra 33 por tarea); si hiciera falta, las 180 franjas
  comparten combiner y se puede guardar su resultado mientras no cambien
  sus entradas.
- Intro en alta resolución (campos 400–599): 57,8–59,7 campos/s con las
  copias esperadas en 3,7–10,7 ms y ninguna tarde; antes de 98def88 se
  cumplían por el tope de 200 ms y la intro iba a 4,9 campos/s
  (docs/framebuffer.md, «Copia RSX → RDRAM»).

## Texturas que cambian en cada cuadro: Perfect Dark (2026-10-11)

**PS3 real** (Slim CECH-3011A, HFW 4.91 + HEN), Perfect Dark NTSC
(respaldo del usuario, fuera del repo) desde el menú, sin tocar el control:
la intro hasta la azotea de dataDyne (campo ~1800), donde el juego copia el
cuadro anterior sobre el nuevo en 180 franjas de 320 × 1 por tarea
(diagnóstico en docs/texturas.md). «Antes» = release v0.1.0 (e0aa556),
escala automática 4×; «después» = c17e790 (app SLSA00097 «Salsa64
texturas»), escala 1× pedida. La HLE no depende de la escala (los tiempos
del SPE se comparan tal cual); los campos/s de antes estaban limitados por
el SPE, no por el RSX. Por tarea gráfica, ventanas de 100 campos:

| Campos | Campos/s | Tareas | Texturas decodificadas | HLE en el SPE | Decodificar | Vértices |
|---|---|---|---|---|---|---|
| 1700–1799 | 53,0 → 57,3 | 25 → 25 | 2,2 → 2,2 | 3,52 → 2,75 ms | 0,74 → 0,05 | 0,71 → 0,70 |
| 1800–1899 | **51,1 → 59,8** | 36 → 50 | 90,3 → 133,3 | 39,72 → 3,29 ms | 36,84 → 0,48 | 0,77 → 0,77 |
| 1900–1999 | **50,0 → 59,9** | 30 → 50 | 143,1 → 172,8 | 61,35 → 3,51 ms | 58,40 → 0,62 | 0,77 → 0,78 |
| 2000–2099 | **50,1 → 59,8** | 33 → 50 | 132,2 → 176,9 | 57,32 → 4,16 ms | 53,76 → 0,65 | 0,97 → 0,97 |
| 2100–2199 | **50,0 → 60,3** | 32 → 51 | 129,2 → 173,1 | 57,10 → 5,01 ms | 52,69 → 0,62 | 1,45 → 1,48 |

(La corrida de antes terminó en el campo 2278.) Después, de los campos 2200
a 3900 (la azotea con más geometría, hasta 4400 triángulos por tarea):
57,0–60,3 campos/s, SPE 6,0–10,1 ms por tarea, decodificar 0,50–0,67 ms
con 140–181 texturas por tarea; dos ventanas a 47,8 y 53,4 coinciden con
más carga de CPU (49 MIPS, 43 % en espera).

- El juego sigue pidiendo texturas nuevas en cada tarea (las franjas son
  el cuadro anterior: 133–181 decodificadas por tarea, antes 90–143); lo
  que cambió es el costo de cada una: 408 µs por franja de 513 × 9 (88 ns
  por texel) → 3,6 µs por franja de 321 × 2 (5,6 ns por texel, con el
  hash, la caché y el DMA de salida).
- El CPU emulado dormía 28–40 ms por tarea esperando al SPE; ahora 3,0–5,5
  ms (ventanas de 5 s).
- **Límite que queda en Perfect Dark:** la escena siguiente de la intro
  (campos 4300–5500, t = 98–123 s) va a 44,8–52,9 campos/s sin decodificar
  nada (0–0,6 texturas por tarea): emulación 18–22 ms por campo, 41–63
  MIPS emulados y 11–50 % en bucles de espera; en las ventanas por debajo
  de 50 campos/s el CPU espera al SPE 0,02–0,85 ms por tarea. Es el hilo 0
  del PPE con el código recompilado, como GoldenEye en la presa
  (docs/uso-cell.md), no la HLE.
- **Sin regresiones** (un minuto de cada uno, jugando; escenas no idénticas
  a las de referencia, medianas de las ventanas de 5 s): Zelda OoT 59,8
  campos/s (mín. 58,7), SPE 4,50 ms por tarea (antes, en el log de la
  v0.1.0: 59,8 y 5,69); GoldenEye 53,4 (mín. 47,6), SPE 7,59, limitado por
  el CPU (antes, en la presa: 54,1 y 6,42); Conker 59,8 (mín. 54,8), SPE
  3,72 (máx. 30,1 en la intro, igual que antes: 59,8, 7,26, máx. 30,5). En
  los tres, decodificar sigue en 0,00–0,05 ms por tarea.
- **A 3× (720p, escala automática)**, la misma corrida repetida: la azotea
  a 59,3–60,2 campos/s con el SPE en 3,3–7,1 ms por tarea y las mismas
  texturas; de 2200 a 3900, 47,9–60,2 (las bajadas, en las mismas ventanas
  que a 1×, con más MIPS); la escena siguiente, 43–59. El RSX no limita: el
  flip espera el vsync 4–11 ms por campo y el espejo del framebuffer copia
  145–202 cuadros cada 5 s sin que la emulación lo espere. El autor: «se
  ve muy fluida y nítida».
- Logs: `build/ps3-logs/salsa64-texturas-c17e790.log` y
  `salsa64-texturas-c17e790-3x.log` (fuera de git) y el de antes,
  `salsa64-v010-pd-majora.log`.

## Estados guardados en segundo plano y comprimidos (2026-10-13)

**Pendiente de PS3 real**: el paquete lo pide la sesión principal. Lo
medido hasta ahora (RPCS3 no sirve para tiempos; el host sí para el
tamaño):

| Medida | Antes | Después | Dónde |
|---|---|---|---|
| Tamaño del archivo (RDRAM de 8 MB) | 8,1 MB siempre | OoT (intro, campo 1500) 1,98 MB; GoldenEye 1,22 MB; Mario Kart 64 1,46 MB; gfxtest 80 KB | host, `salsa64-run --state-zip`, respaldos del autor |
| Emulación detenida al guardar | ~430 ms (417–1064; 520 de disco) | la foto: `memcpy` de 8 MB + secciones, 2 ms en RPCS3 (en la PS3, por medir) | log: «emulación detenida … (la foto)» |
| Cargar | ~171 ms (161 leyendo) | por medir (menos disco, más descompresión en el PPE) | log: «cargado de la ranura» |

Lo que escribe el log de la PS3 en cada guardado: «emulación detenida X ms
(la foto)» y, cuando termina el hilo, «escrita en segundo plano: … ms de punta
a punta, … ms en el disco, … ms del SPE comprimiendo … KB de … KB (r:1)». El
costo para el PPE (hilo de baja prioridad, ~1 syscall por 64 KB) se mide con
las líneas «hilo 1» de docs/hilos-ppe.md.

**Caja negra (2026-10-13), RPCS3 (sin tiempos del Cell):** Mario Kart 64
(respaldo del autor), anillo de 12 MB: 82 tareas y 165 campos caben (150 KB
por tarea con sus texturas), 12,15 MB → 3,15 MB (3,9:1), el SPE comprime en
72 ms y todo se escribe en 76 ms de punta a punta; registrar una tarea
138 µs de media en RPCS3 (30 µs en gfxtest) y un campo 1,7 µs. En la PS3
real, por medir con las líneas «caja negra: al cerrar…» y «escrita en segundo
plano…» del log. El estado de ese juego: 8,25 MB → 1,14 MB (7,2:1).
**Ojo al medir la foto en RPCS3:** la primera foto tras recompilar el
programa tarda ~275 ms porque RPCS3 recompila los trabajos del SPE de
guardar el estado de las HLE; copiar la RDRAM son 1,2 ms.

## Estados guardados (2026-10-11)

PS3 real (Slim CECH-3011A, HFW 4.91 + HEN), paquete SLSA00094 «Salsa64
estados», recompilador y efectos de framebuffer activos, 720p, juegos
abiertos desde el menú. Tiempos que escribe el frontend en el log
(«estado: guardado/cargado … ms»), desde la barrera hasta el archivo
cerrado (docs/estados.md):

| Operación | Veces | Media | Mín – máx | Disco | Notas |
|---|---|---|---|---|---|
| Guardar (~8,1 MB) | 14 | 568 ms | 417 – 1064 ms | 520 ms de media escribiendo (~16 MB/s) | Zelda, SM64, Mario Kart, GoldenEye, Perfect Dark. La miniatura cuesta 10 ms (19 200 lecturas del búfer del RSX); el estado de las HLE en los SPE, < 1 ms |
| Cargar | 19 | 171 ms | 148 – 183 ms | 161 ms de media leyendo (~50 MB/s) | Lo demás (secciones, vaciar el recompilador, soltar destinos) ~10 ms |

Lo que pesa es escribir 8 MB en el disco duro: si molesta, el próximo paso
es comprimir (la RDRAM de 8 MB suele tener la mitad en ceros) o escribir
desde otro hilo.

Contador de velocidad (SELECT + △) encendido: GoldenEye en la presa a 45–49
campos/s, como sin contador (45,8 en «Efectos de framebuffer» y 49,4 en el
A/B del recompilador); Zelda, SM64 y Mario Kart a 59,4–60 campos/s. El
dibujo del contador y del aviso cuesta < 0,2 ms por cuadro presentado en el
hilo de presentación (presentación 0,2–0,6 ms por cuadro con él, igual que
antes).

## Efectos de framebuffer (2026-10-11)

Rama hopeful-dirac (docs/framebuffer.md). Lo que cuesta cada mecanismo lo
escribe el frontend cada 5 s («gfx: framebuffer: N copias a la RDRAM (ms
c/u en presentación), la emulación esperó N tareas (ms c/u, N tarde), N
copias espejo, N cuadros copiados; texturas desde un destino: del RSX, de
la RDRAM, del ligado; destinos cebados; copias perdidas, destinos sin
superficie, color images imposibles»). Interruptor:
`SALSA64_DATA_DIR/fbfx.cfg` (`0` apaga todo, `1` solo render a textura,
`2` todo, por omisión).

**RPCS3** (0.0.43; no vale para tiempos): gfxtest 35 de 35; 122 copias de
320 × 240 en 120 cuadros, 2,05 ms cada una en presentación. Sirve solo
para saber que el mecanismo funciona de punta a punta.

**PS3 real** (2026-10-11, Slim CECH-3011A, HFW 4.91 + HEN, app SLSA00089
«Salsa64 framebuffer», 720p a 3×; logs fuera del repo):

| Juego | Escena | Campos/s | Copias a la RDRAM (esperadas por la emulación) | Espejos | Notas |
|---|---|---|---|---|---|
| gfxtest | 35 de 35 puntos | — | 1 por cuadro (la imagen de 64 que lee el CPU): 6,7–6,9 ms c/u en presentación; la emulación esperó 7,4–7,9 ms por tarea | 2 por cuadro | Un caso artificial que copia en cada cuadro: el peor costo posible |
| Zelda OoT | bosque Kokiri, un minuto | 59,8 (100 %) | 0 | 0 | Emulación 4,0 ms por campo, igual que antes; 225–290 cuadros copiados por 5 s (espejo del framebuffer, 0,0 ms medidos) |
| Zelda OoT | abrir la pausa | — | el fondo (320 × 240): 16 ms; y 4 chicas (64 × 112): Link, su z, su copia y la cobertura | — | Una vez por pausa; la N64 tarda ~1 s en filtrar el fondo |
| Zelda OoT | pausa abierta (paquete con 16 superficies) | 59,9 (100 %) | 2 por tarea (Link y su z, redibujados cada tarea aunque nadie los lea): 7,4–8,4 ms c/u; la emulación esperó 16–24 ms por tarea | 0 | 44 texturas por tarea desde la RDRAM (las franjas del fondo y Link). El paquete final espera esas copias solo las dos primeras tareas: por medir |
| Mario Kart 64 | Luigi Raceway, túnel | 60 | 0 | 0 | La cámara del túnel se ve gracias al espejo del framebuffer por campo |
| SM64 | castillo exterior, un minuto | 59,8 (100 %) | 1 al arrancar (12 ms) | 0 | |
| GoldenEye | Dam, un minuto | 45,8 (76 %) | 0 | 0 | Dentro del rango previo del nivel (34–55: va por el CPU); 225–229 cuadros copiados por 5 s, 0,0 ms por campo en presentación |
| Banjo-Kazooie | intro y Spiral Mountain | 56–57 | 0 | 0 | Igual que antes |
| Conker | intro, 3 minutos | 59,8 | 1–2 por ventana de 5 s (una color image de 292 que se dibuja cada tarea y no se presenta: la primera se espera, las demás van sin esperar) | muchas | Igual que antes |

## Caché de texturas (2026-10-10)

**PS3 real** (Slim CECH-3011A, HFW 4.91 + HEN), app SLSA00070 «Salsa64
texturas», Zelda OoT NTSC (respaldo del usuario, fuera del repo), sin
tocar el control: arranque, logo, intro y título. Las dos compilaciones
son de un hilo con el recompilador y solo difieren en la HLE gráfica:
«antes» = main 697922c más la instrumentación (ventanas fijas de 100
campos, capturas en campos fijos, salida en el campo 3510; parche en la
rama, sin cambios de comportamiento); «después» = b3ba375 (caché de
texturas, docs/texturas.md). La emulación es determinista: misma ventana,
misma escena; las 7 capturas (campos 500, 1000, …, 3500) son **idénticas
byte a byte**. Por tarea gráfica (~33 tareas cada 100 campos):

| Campos | Escena | Campos/s | Peor campo | Dibujos | Texturas decodificadas | HLE en el SPE | Decodificar | Cargas a la TMEM | PPE esperando al SPE |
|---|---|---|---|---|---|---|---|---|---|
| 0–99 | Logo de N64 | 59,5 → 57,9 | 29,6 → 71,5 | 22 | 18,0 → 0,2 | 1,04 → 0,51 ms | 0,47 → 0,01 | 0,12 → 0,04 | 0,66 → 0,13 |
| 500–599 | Intro nocturna | 60,2 → 60,0 | 24,8 → 17,2 | 110 | 83,3 → 0,7 | 14,04 → 5,09 ms | 6,75 → 0,06 | 3,73 → 1,43 | 13,56 → 4,62 |
| 1300–1399 | Intro (Epona) | 59,8 → 60,0 | 32,0 → 18,5 | 137 | 111,5 → 0,0 | 19,75 → 6,17 ms | 11,03 → 0,00 | 4,82 → 2,22 | 19,28 → 5,71 |
| 1700–1799 | Intro, la más pesada | **41,3 → 59,9** | 211,9 → 21,8 | 248/244 | 170,0 → 0,7 | 28,31 → 8,92 ms | 15,82 → 0,09 | 6,54 → 2,82 | 27,81 → 8,44 |
| 2000–2099 | Título con el castillo | 60,4 → 60,5 | 29,2 → 17,1 | 147 | 91,3 → 0,1 | 21,03 → 6,43 ms | 11,47 → 0,01 | 5,63 → 2,42 | 20,55 → 5,94 |
| 2200–2299 | Escena con más dibujos | **45,2 → 60,1** | 42,7 → 21,8 | 272/268 | 182,7 → 0,0 | 30,62 → 9,33 ms | 17,21 → 0,00 | 7,10 → 2,96 | 30,13 → 8,84 |
| 2500–2599 | | **45,2 → 60,4** | 37,5 → 20,0 | 201/200 | 130,5 → 0,0 | 25,38 → 8,38 ms | 13,06 → 0,00 | 6,96 → 2,93 | 24,91 → 7,90 |
| 3100–3199 | | **45,1 → 60,0** | 34,9 → 20,0 | 163/161 | 137,0 → 0,0 | 22,39 → 6,79 ms | 12,85 → 0,00 | 5,34 → 2,54 | 21,91 → 6,31 |

Tiempos del decrementador del SPE (`gfx_result.t_*`); el peor campo es el
intervalo más largo entre dos campos en el bucle (con la presentación). Las
ventanas que terminan en un múltiplo de 500 (400–499, 900–999, …) tienen
una captura (~400 ms) y no se comparan; las de 0–99 incluyen el arranque.

- La decodificación desaparece: 0 a 1,1 texturas decodificadas por tarea
  (se reusan 36–320 por tarea, la mitad de tareas anteriores); en los 3510
  campos no hubo desalojos ni texturas al arena (cabe todo en 8 MB y 1024
  entradas). Identificar las texturas (hash de lo que leen en la TMEM y
  búsqueda) cuesta 0,03–0,35 ms por tarea.
- Las cargas a la TMEM bajan ×2,4 por copiar en bloque (antes `memcpy` de 8
  bytes por palabra y 4 más para intercambiar mitades). Ahora son lo más
  caro de la HLE junto con el estado: 2250–2570 lecturas de la RDRAM por
  tarea para 260–450 cargas (LOADTILE lee fila por fila, cada una con un
  DMA que espera). Siguiente paso: pedir las filas por adelantado o con
  una lista de DMA.
- Menos dibujos (248 → 244): la misma textura cargada dos veces en una
  tarea antes daba dos copias en el arena con distinta dirección y cortaba
  el dibujo; ahora es la misma entrada.
- El PPE sigue esperando al SPE 4,6–9,1 ms por tarea en la intro y el
  título, pero ya cabe en el campo: todas las ventanas sin captura llegan a
  60 campos/s.

## Dos hilos en el PPE (2026-10-10)

**PS3 real** (Slim CECH-3011A, HFW 4.91 + HEN), app SLSA00073 «Salsa64
hilos», recompilador activo en los dos casos. «Antes» = main 697922c (un
hilo: emulación, presentación y flip en vsync en el mismo bucle).
«Después» = ee5e917 + 5ee9f40 (hilo de emulación + hilo de presentación,
ritmo de la tele emulada; docs/hilos-ppe.md). Ventanas de 5 s del log del
frontend; los campos por segundo son los **emulados**.

### vitest (ROM propia, pantalla 320×240 por el CPU)

| | Antes (un hilo) | Después (dos hilos, con ritmo) | Después sin límite (medición A/B) |
|---|---|---|---|
| Campos/s | 59,9 (tope del flip en vsync) | 59,8–60,0 (ritmo) | **187 → 363** sin/con detección de bucles de espera |
| Emulación por campo | 2,5–5,2 ms | 2,3–2,4 ms | 5,02 → 2,45 ms (×2,1) |
| Copia del VI | — | 0,30 ms (300 KB) | 0,30 ms |
| Presentación por campo | 11,4–14,1 ms (casi todo el flip) | 0,2–0,3 ms + flip 0,0–0,1 ms | 0,4 ms + flip 14,6 ms, a 60/s; 1 100–1 500 campos saltados por ventana |
| Espera del ritmo | — | 14,0 ms por campo | — |

La medición A/B de vitest antes daba 59,94 → 59,94 campos/s (el flip
tapaba todo); ahora mide de verdad: 187 → 363.

### Zelda: Ocarina of Time (NTSC, respaldo del usuario, fuera del repo)

Campos/s por escena (ventana de 5 s que contiene el campo):

| Escena (campo) | Antes (un hilo) | Después (dos hilos) |
|---|---|---|
| Logo de N64 (~150–590) | 57,8–59,0 | 58,4 |
| Intro nocturna (~590–1200) | 59,0–60,0 | 57,9–59,4 |
| Link en Epona (~1200–1750) | 58,6–59,4 | 57,1–59,4 |
| Título con el castillo (~1750–2700) | **52,3 → 50,9 → 47,3 → 45,0 → 41,3** | **57,0 / 59,4 / 57,0 / 59,3 / 59,9** |
| Después del título (~2700–4700) | 41,6–59,9 | 57,4–59,7 |

Desglose por campo en el título (ventana del campo ~2250): antes
emulación 12,2 ms + presentación 8,8 ms (flip) = 21 ms → 47 campos/s;
después emulación 13,7 ms, ritmo 2,8 ms, presentación 0,0 ms, flip 8,9 ms
en el otro hilo → 57–59 campos/s. Las ventanas a 57 son las que tienen una
captura `screen-N.ppm`: cada captura frena la presentación ~400 ms (leer
el búfer del RSX desde el PPE), descarta 4 campos, hace esperar al hilo
de emulación ~250 ms por un juego de búferes y reinicia el ritmo. Por eso
las capturas periódicas pasaron a ser opcionales (`capturas`).

Lo que queda dentro de «emulación»: la espera del hilo de emulación al
SPE al terminar cada tarea gráfica, 10–27 ms por tarea con ~20 tareas/s
(20–53 % del tiempo del hilo). Es el cuello siguiente: que el evento del
RSP se vuelva a programar en vez de bloquear (`src/core/rsp.c`), o acortar
la HLE en el SPE (caché de texturas). La traducción al RSX cuesta
0,15–0,88 ms por tarea y la copia del VI 0,20 ms por campo (se copia
aunque el VI muestre un destino del RSX; se puede evitar).

### Guardado por hilos

savetest (SRAM) dos veces en la PS3: «persistió el arranque 1», volcado
diferido escrito por el hilo de presentación a los ~2 s (257 escrituras
del juego), salida limpia. START + SELECT y «Salir del juego» funcionan;
los controles llegan por el buzón entre hilos (vitest los identifica).

## Audio: naudio y MP3 en el SPE 1 (2026-10-10)

**PS3 real** (mismo equipo), rama `charming-allen-a8e4b3` con main
485a747, recompilador. Paquete «Salsa64 naudio» (SLSA00080). Respaldos
propios; ventanas de 5 s.

| | Banjo-Kazooie | Perfect Dark | Conker |
|---|---|---|---|
| ABI | naudio (Banjo-Kazooie) | naudio con MP3 | naudio (Conker) |
| Corrida | 89 s, del logo a jugar | 179 s, sin imagen (falta su microcódigo gráfico) | 96 s, pantalla negra (se detiene tras la primera tarea gráfica) |
| Tareas | ~30/s, 0 sin implementar, 0 errores | ~30/s, 0 sin implementar, 0 errores | 30/s, 0 sin implementar, 0 errores |
| HLE en el SPE 1, por tarea | mediana **2,92 ms** (ventanas 1,24–3,65; la más larga 5,68) | mediana **5,70 ms** (2,09–7,83; la más larga 10,38) | **1,56 ms** (solo el bus de efectos: el juego no dispara voces) |
| PPE esperando al SPE | 0,000 ms | 0,000 ms | 0,000 ms |
| Emulación | 100 % de media (mín. 88 %) | 97 % (mín. 78 %: el CPU) | 100 % |
| Salida | 1,1 % en silencio (el llenado), 6,6 % estirado, 13 faltas breves, 0 bloques tarde | 2,3 % en silencio, 20,5 % estirado (las bajadas del CPU), 28 faltas breves, 0 bloques tarde | normal |
| Hilo de audio | 1,9 % | 2,2 % | 1,7 % |
| Oído (el autor) | «suena bien» (el video parpadea: de la HLE gráfica) | «suena bien» | silencio, lo esperado |

Listas de prueba al arrancar (SPE contra PPE, ahora 5: nead OoT y MK, ABI1,
naudio con MP3 y Banjo-Kazooie): iguales, 2,9 ms.

El SPE 1 pasa ~9 % del tiempo en Banjo y ~17 % en Perfect Dark (más
comandos por tarea: ~875 contra ~380, y el MP3). Código escalar; los
candidatos a SIMD siguen siendo ENVMIXER, RESAMPLE y ADPCM, y ahora la
ventana del MP3.

## Audio: AI, HLE de audio en el SPE 1 y salida (2026-10-10)

**PS3 real** (mismo equipo), rama `amazing-varahamihira-3027c8`
con main 298b18d (recompilador, dos hilos con ritmo, fin de tarea por
sondeo). Paquete «Salsa64 audio» (SLSA00072). Ventanas de 5 s.

audiotest (ROM propia; tono por el CPU y luego por una tarea de audio de
14 comandos nead cada 16 ms):

| | PS3 real |
|---|---|
| Velocidad | 59,8 campos/s (100 % de NTSC), 96 % del tiempo en bucles de espera |
| HLE de audio en el SPE, por tarea | **0,057 ms** (máx. 0,057), PPE esperando 0,000 ms, 62,5 tareas/s |
| Listas de prueba al arrancar (SPE contra PPE, 3 listas) | iguales, 1,0 ms (en RPCS3, 1,3 s: compila el programa del SPE) |
| Salida | colchón 78–98 ms, régimen normal (sin estirar), 0 faltas, 0 silencios, 0 saltos, 0 bloques tarde; 2 % de silencio solo en los primeros 5 s (llenado del colchón) |
| Hilo de audio | 1,9 % del tiempo de pared |
| Oído (el autor) | suena bien: el afinador marca 440 Hz y cada nota sale de su lado |
| Corrida completa | 96 s, 3 007 488 muestras del AI, 4 510 464 a la salida (4 392 en silencio: el llenado inicial), 17 619 bloques; START + SELECT cierra el audio y sale normal |

Zelda OoT NTSC (respaldo propio), recompilador, del logo al juego;
corrida de 535 s (105 ventanas) más una de 120 s con el mismo resultado:

| | PS3 real |
|---|---|
| ABI | nead (Zelda OoT): 0 comandos sin implementar, 0 errores, ~60 tareas/s |
| HLE de audio en el SPE 1, por tarea | mediana **2,42 ms**, ventanas de 5 s entre 0,30 (arranque) y 2,83 ms; la tarea más larga 4,02 ms |
| PPE esperando al SPE | **0,000 ms** (fin de tarea por sondeo; el SPE termina antes de que el núcleo pregunte) |
| Salida | emulación al 99,9 % de media, régimen normal; 0,06 % del tiempo en silencio (el llenado inicial), 0,4 % estirado (los arranques de escena), 3 faltas breves en 9 min, 0 silencios, 0 saltos, 0 bloques tarde |
| Hilo de audio | 2,0 % del tiempo de pared |
| Oído (el autor) | «se escucha perfecto» |

Super Mario 64 NTSC: sin F3D (otro trabajo) el juego no pasa del arranque y
no hay imagen ni sonido audible, pero sus tareas de ABI1 corren en el SPE
1 sin errores ni comandos desconocidos: **0,115 ms** por tarea, 60/s.
Pendiente oírlo cuando F3D esté integrado.

El costo de la HLE de Zelda (2,4 ms por tarea, código escalar) cabe de
sobra en el SPE (14 % de un campo) y no frena al PPE. Si hace falta, los
bucles de ENVMIXER, RESAMPLE y ADPCM son candidatos claros a SIMD.


## Fin de tarea del RSP por sondeo (2026-10-10)

**PS3 real** (mismo equipo), recompilador y caché de texturas en los dos
casos, dos hilos. «Antes» = main 4a4d98c (el evento de fin de tarea
espera al SPE); «después» = 16a544f + 0cb8f63 (sondeo: el evento se
reprograma cada 10 000 ciclos; si el CPU está en bucle de espera, el
backend duerme 100 µs por sondeo). Zelda OoT NTSC, 52 s desde el logo
hasta pasado el título; ventanas de 5 s.

| | Antes (espera) | Después (sondeo) |
|---|---|---|
| Campos/s | 58,5–60,0 | 58,5–59,9 |
| Emulación por campo | 3,2–7,5 ms | 3,2–7,2 ms |
| Emulación esperando al SPE, por tarea | 2,3–8,2 ms (100 esperas / 100 tareas) | **0,00 ms (0 esperas)** |
| Sondeos por tarea / dormida en bucles de espera | — | 33–75 / 2,9–6,9 ms |
| Ritmo (margen por campo) | 9,0–13,6 ms | 9,3–13,6 ms |

Lectura: la espera bloqueante desapareció, pero en Zelda no hay nada que
hacer mientras el RSP trabaja (el CPU está en el bucle ocioso de
libultra el 86–88 % del tiempo): el tiempo que antes bloqueaba ahora lo
duerme el sondeo en pasos de 100 µs, y la emulación por campo queda
igual. La ganancia aparecerá donde el CPU tenga trabajo durante el RDP
(lógica del juego después de mandar la tarea; Super Mario 64 con F3D es
el candidato). Sin regresiones: 60 campos/s, 0 descartados, salida
limpia; gfxtest OK en la PS3.

gfxtest en la PS3: OK (22 de 22) antes y después; 38 → 36–37 campos/s
(26 → 27 ms por campo: la ROM espera al RSP con un bucle que no es de
espera pura y ahora lee Count cada 1024 vueltas). La primera versión de
la ROM con espera por tiempo leía Count en cada vuelta y el recompilador
salía a C en cada iteración (64 M salidas, 319 M instrucciones
interpretadas, 9 campos/s).

## Líneas base de la Fase 0 (pendientes de PS3 real)

- `spu-compute`: 6 MB, PPE escalar contra 6 SPUs (tiempos en el log).
  **PS3 real** (2026-10-10, CECH-3011A, HEN): PPE 13 000 µs; 6 SPUs 889 µs
  incluyendo arranque y join (≈14,6×). Los 6 SPUs están disponibles con HEN.
- `rsx-triangle`: 120 cuadros con vsync.
  **PS3 real** (2026-10-10, CECH-3011A, HEN): verificación de píxeles OK a
  1280×720; 120 cuadros en 2067 ms (≈58 cps; el cuadro de verificación espera
  al RSX). La inicialización del RSX tarda ≈2 s.
- `jit-ppe`: costo de limpiar 64 KB de caché (dcbst + icbi).

## Líneas base de la Fase 1

Micro-benchmark del intérprete (`tests/unit/bench.c`): bucle de 10
instrucciones (LW, ADDIU, SW, ADDU, SLL, XOR, ADD.S, ADDIU, BNE, NOP) desde
KSEG0, 20 M instrucciones.

| Fecha | Commit | Entorno | Resultado | Notas |
|---|---|---|---|---|
| 2026-10-10 | (este) | Host: Mac Studio, Docker arm64, gcc 12 -O2 | 237 MIPS | Solo referencia del host |
| 2026-10-10 | (este) | RPCS3 0.0.43, PPU LLVM | 35,6 MIPS | **No concluyente** |
| 2026-10-10 | (arranque HLE) | Host: Mac Studio, Docker arm64, gcc 12 -O2 | 217 MIPS | −8 %: la FPU calcula ahora Inexact y los modos de redondeo con exactitud (TwoSum/fma); el bucle tiene un ADD.S cada 10 instrucciones. Costo de corrección, no regresión |
| 2026-10-10 | (arranque HLE) | RPCS3 0.0.43, PPU LLVM | 29,0 MIPS | **No concluyente** |
| 2026-10-10 | 81da001 | **PS3 real**: Slim CECH-3011A, HFW 4.91 + HEN, ppu-gcc -O2 -mcpu=cell | **8,8 MIPS** | Primer número real. 154/154 pruebas pasan en big-endian. Muy lejos de los ~60 MIPS que pide la N64 a velocidad completa: perfilar antes de seguir (la sospecha del FPSCR resultó falsa: ver la sección del perfilado) |
| 2026-10-10 | 419731a | **PS3 real** (mismo equipo) | 8,7 MIPS | Mismo intérprete con el benchmark en variantes: repite la línea base |
| 2026-10-10 | c14268c | **PS3 real** (mismo equipo), + `-mno-relative-jumptables` | **17,1 MIPS** | ×2,0. Desglose abajo |
| 2026-10-10 | c14268c | Host: Mac Studio, Docker arm64, gcc 12 -O2 | 294 MIPS | Solo referencia del host |
| 2026-10-10 | 147d5ac | **PS3 real** (mismo equipo) | **21,2 MIPS** | Segunda ronda. Desglose abajo |
| 2026-10-10 | 147d5ac | Host: Mac Studio, Docker arm64, gcc 12 -O2 | 297 MIPS | Solo referencia del host |

### Desglose por clase de instrucción (2026-10-10)

Variantes de `tests/unit/bench.c` (bucles de 10 instrucciones con la misma
cola ADDIU + BNE + delay slot). Ciclos del PPE = ns × 3,2 (3,2 GHz). PS3
real (CECH-3011A, HFW 4.91 + HEN); paquete `salsa64-cpu-tests`, 154/154
pruebas en ambas corridas.

| Variante | Original (419731a) | Ronda 1 (c14268c) | Ronda 2 (147d5ac) | Ciclos/instr. | Total |
|---|---|---|---|---|---|
| mixto | 8,7 MIPS | 17,1 | 21,2 | 364 → 187 → 150 | ×2,4 |
| nop | 12,6 | 38,2 | 34,4 | 254 → 84 → 93 | ×2,7 |
| enteros | 12,8 | 23,5 | 25,6 | 248 → 136 → 124 | ×2,0 |
| memoria (LW/SW) | 14,0 | 22,1 | 20,9 | 227 → 144 → 152 | ×1,5 |
| mov.s | 8,6 | 13,3 | 23,5 | 369 → 240 → 136 | ×2,7 |
| add.s (inexacto) | 2,6 | 7,6 | 14,0 | 1194 → 416 → 227 | ×5,3 |

Host (Mac Studio, Docker arm64, sin sanitizers), solo referencia: mixto
231 → 294 MIPS. RPCS3 dio 21,4 MIPS después (antes 29): no concluyente, su
recompilador trata distinto la función grande del bucle.

Diagnóstico (antes):
- La sospecha del FPSCR no aplicaba: `cop1.c` nunca cambia el modo de
  redondeo del host (calcula al más cercano y corrige con TwoSum/fma).
- El bucle costaba ~250 ciclos por instrucción aun para un NOP: volvía a
  leer pc, next_pc, cycles y las banderas de salto justo después de
  escribirlos (load-hit-store), llamaba a `execute()` (que guardaba y
  restauraba registros en la pila: otro load-hit-store), ponía `gpr[0] = 0`
  y lo leía en la instrucción siguiente, y cada `switch` usaba `lwax`
  (microcódigo).
- ADD.S costaba ~1340 ciclos más que un NOP: cada valor pasaba entre
  registros enteros y de punto flotante por la pila (el PPE no tiene
  movimientos directos), unas cinco veces por instrucción, más una cadena
  de llamadas.

Lo que queda (después), en ciclos por instrucción por encima de un NOP:
- ALU de SPECIAL ~75 y LW/SW ~76: segundo despacho (`switch` de funct) y
  dependencias entre instrucciones a través de `cpu->gpr` en memoria (la
  siguiente lee el registro que la anterior acaba de escribir). SLL/SRL/SRA
  usan desplazamientos variables (microcódigo).
- MOV.S ~223: todo lo que no cubre el camino rápido de la FPU pasa por la
  llamada a `salsa64_vr4300_cop1` y dos `switch` más.
- ADD.S ~475 en el camino rápido: se presume que son las comparaciones de
  punto flotante seguidas de salto (unas diez), que en el PPE esperan toda
  la latencia de la FPU. Siguiente paso: clasificar entradas con los bits
  en registros enteros (cargados de memoria, sin transferencia) y una franja
  de exponentes que garantice que no hay desbordamiento ni subdesbordamiento.

Segunda ronda (147d5ac), en ciclos por encima de un NOP: ALU de SPECIAL
75 → 45, MOV.S 223 → 61, ADD.S 475 → 192; LW/SW igual (~74). El índice
del despacho único (64 + funct, sin salto) alarga la cadena de despacho de
todas las instrucciones: NOP y memoria, que no usaban el segundo despacho,
perdieron ~9 ciclos. Para una mezcla de juego con ~1/3 de SPECIAL queda a
favor; se podría acortar la cadena calculando el índice ya escalado y
directo desde la palabra de la instrucción (unos 2–4 ciclos).

Lo que queda: el bucle cuesta ~93 ciclos aun para un NOP (~45
instrucciones del PPE). Es sobre todo la cadena dependiente de cada
instrucción: cargar la palabra de la RDRAM → extraer el índice → cargar la
dirección de la tabla → mtctr → bctr, más la latencia de cada carga en un
procesador in-order. Bajarlo de verdad pide decodificar por adelantado
(caché de instrucciones predecodificadas o despacho en tubería: decodificar
la siguiente mientras corre la actual), y la dependencia entre
instrucciones por `cpu->gpr` en memoria no se quita en un intérprete. Es el
terreno del recompilador (Fase 3).

Reglas del PPE que salieron de aquí (aplican también al recompilador de la
Fase 3): no releer lo recién escrito; nada de funciones cortas con marco
de pila en el camino caliente; evitar `lwax`/`lha` (cargas con extensión
de signo), desplazamientos variables y formas con punto (`rldicl.`,
`srdi.`) en código caliente; no mover valores entre registros enteros y de
punto flotante; clasificar valores de punto flotante con sus bits en
enteros y no con comparaciones de punto flotante seguidas de salto;
`-mno-relative-jumptables` en todo el código del PPU.

Frontend con vitest (`salsa64.pkg`, ROM propia: bucles de dibujo con
enteros y espera activa a la interrupción del VI, sin FPU):

| Fecha | Commit | Entorno | Resultado | Notas |
|---|---|---|---|---|
| 2026-10-10 | d27735e | RPCS3 0.0.43, PPU LLVM | 24,0 campos/s, 37,7 MIPS | **No concluyente** |
| 2026-10-10 | d27735e | **PS3 real**: Slim CECH-3011A, HFW 4.91 + HEN | **8,7 campos/s (14 % de NTSC), 13,6 MIPS** | Emulación 114,6 ms por campo, presentación (subida de textura + dibujo en rsxkit) 0,3 ms. 13,6 MIPS sin FPU contra 8,8 del bench con un ADD.S cada 10 instrucciones, medido antes del perfilado (el costo de la FPU era el paso de valores por la pila, no el FPSCR) |
| 2026-10-10 | 940f6f8, sin detección | **PS3 real** (mismo equipo) | **15,2 campos/s (25 % de NTSC), 23,8 MIPS** | Intérprete optimizado (147d5ac) sin saltar bucles de espera: emulación 65,6 ms por campo. ×1,75 frente a d27735e |
| 2026-10-10 | 940f6f8, con detección | **PS3 real** (mismo equipo) | **28,3 campos/s (47 % de NTSC), 24,1 MIPS** | Emulación 35,0 ms por campo; 45,6 % del tiempo de la consola se saltó en el bucle de espera al VI. El tope de 60 campos/s lo pone el flip en vsync de rsxkit |

### Bucles de espera (2026-10-10)

Medición A/B en la misma corrida (`salsa64.pkg` de 940f6f8): tras
verificarse con vitest, el frontend corre 10 s sin detección y 10 s con
ella. Dos corridas seguidas dieron lo mismo con 0,2 % de diferencia.

| | Sin detección | Con detección |
|---|---|---|
| Emulación por campo | 65,6 ms | 35,0 ms (×1,9) |
| Campos/s | 15,2 | 28,3 |
| MIPS (instrucciones ejecutadas) | 23,8 | 24,1 |
| Tiempo de la consola saltado | 0 % | 45,6 % |

En el host (salsa64-run con ASan) vitest hasta su verificación: 1,70 →
0,93 s. nemu64-test no espera nunca (0 % saltado, mismo tiempo).

Costo en el intérprete (`salsa64-cpu-tests`, bench de 10 instrucciones con
BNE tomado cada 10). El log anterior de cpu-tests en la consola repetía
exactamente los números de 147d5ac, así que la variación entre corridas
parece menor a 0,1 MIPS:

| Variante | 147d5ac | 940f6f8 |
|---|---|---|
| mixto | 21,2 | 21,0 |
| nop | 34,4 | 33,9 |
| enteros | 25,6 | 25,3 |
| memoria | 20,9 | 20,6 |
| mov.s | 23,5 | 23,2 |
| add.s | 14,0 | 14,0 |

Unos 15 ciclos más por salto tomado (el BNE del bench salta 36 bytes
atrás: solo hace la resta y la comparación, no consulta la tabla). Se
revisó el código del PPU: **ppu-gcc no pone indicaciones estáticas en los
saltos** (bits a/t de BO en 0 en los 337 saltos condicionales del bucle,
igual que antes; los `+`/`-` de objdump siguen la convención antigua del
bit «y» y no son predicciones del PPE). Así que no es una predicción
estática equivocada; queda la cadena dependiente (destino → resta →
comparación → salto) y el cambio de disposición del código. Posible
mejora: decidir con el inmediato de la instrucción (desplazamiento entre
−8 y −1), que no depende del destino calculado. Medir antes de adoptarla.

Corrida completa de nemu64-test (`salsa64-run`, ≈189 M ciclos del VR4300):
0,75 s en el host sin sanitizers (253 MIPS); 2,1 s con ASan + UBSan.

## Recompilador (Fase 3)

Primera corrida en la PS3 real (2026-10-10, Slim CECH-3011A, HFW 4.91 +
HEN), paquete `salsa64-cpu-tests` de f7b655d (JR en línea y FPU nativa;
sin la escritura temprana ni los registros fijos de 632a796). Caché en la
cola NOBITS de 16 MB del segmento de texto, llamadas a C con `bl`.

- Pruebas unitarias con el recompilador en modo estricto (cada `t_step`
  ejecuta código generado): 163/163. El código generado corre, se
  reescribe y se invalida en el hardware (dcbst/sync/icbi/isync).
- nemu64-test completo, intérprete contra recompilador: mismas 462
  fallas, mismo hash de nombres (`fdfdf160a2fe7ad8`), mismo ciclo final.

| `bench` (`tests/unit/bench.c`) | Intérprete | Recompilador | Ciclos del PPE por instr. (rec) |
|---|---|---|---|
| mixto | 21,3 MIPS | 156,6 MIPS (×7,4) | 20 |
| nop | 32,6 | 1547,0 (×47) | 2,1 |
| enteros | 25,3 | 749,9 (×30) | 4,3 |
| memoria (LW/SW) | 20,9 | 159,7 (×7,6) | 20 |
| mov.s | 23,6 | 538,4 (×23) | 5,9 |
| add.s (inexacto) | 13,9 | 35,7 (×2,6) | 90 |

Los bucles del bench son bloques que vuelven sobre sí mismos sin escribir
memoria ni salir a C: miden el mejor caso. "memoria" queda en ~20 ciclos
por instrucción por el load-hit-store propio del bucle (LW de lo que el SW
de la vuelta anterior acaba de escribir) y por la prueba de rango. ADD.S
inexacto cuesta ~90 ciclos: la FPU hace la suma, el redondeo a simple y la
comparación de punto flotante seguida de salto (espera toda la latencia),
más la actualización condicional de FCR31.

nemu64-test es la medición con código real: 104 MIPS. Con el modelo actual
de un ciclo por instrucción, la velocidad completa pide hasta 93,75 MIPS
emulados por segundo, menos lo que se salta en bucles de espera (casi la
mitad del campo en escenas ligeras). Pendiente: medir con juegos (tabla de
docs/proyecto.md) en el frontend, y con 632a796 (escritura temprana, registros
fijos: cpu-tests mide nemu64-test con y sin ellos).

### Conversiones de la FPU, LWL/LWR y MFC0 en nativo (2026-10-10)

Antes iban al intérprete (una llamada a C por instrucción, con su
escritura y recarga de registros y la revisión de eventos al volver) y MFC0
terminaba el bloque. Ahora en nativo: SQRT, CVT.D.S/CVT.S.D,
ROUND/TRUNC/CEIL/FLOOR/CVT a W y L, CVT.S.W/CVT.D.W, CFC1/CTC1,
LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR (los pares LWL+LWR y SWL+SWR byte a byte) y
MFC0/DMFC0 (docs/rec-ppe.md). `SALSA64_REC_OPT_RARE_EXEC` repite lo de
antes con el mismo binario: con ella, salsa64-run da exactamente las
cifras de la línea base anterior (139 861 entradas, 1 989 273
interpretadas en nemu64-test).

Host (qemu ppc64, registros fijos; cuenta de eventos, no tiempo):

| Prueba | Antes (`RARE_EXEC`) | Después | |
|---|---|---|---|
| nemu64-test: llamadas al intérprete desde el código | 2 154 355 | 1 773 580 | −18 % |
| nemu64-test: entradas desde C | 139 861 | 109 026 | −22 % |
| nemu64-test: instrucciones del intérprete en el despachador | 1 989 273 | 1 591 007 | −20 % |
| vitest: llamadas al intérprete | 4 428 | 125 | −97 % |
| vitest: entradas desde C | 925 | 803 | −13 % |

Lo que queda en nemu64-test son sobre todo sus pruebas de COP0 (MTC0, TLB,
ERET), traps y CACHE.

Bench del recompilador en qemu (10 M instrucciones por caso, registros
fijos; cuentas, no tiempo):

| Caso | Antes (`RARE_EXEC`) | Después |
|---|---|---|
| `espera` (bucle de 5 que sondea Count, como la ROM gfxtest) | 2 000 000 entradas desde C y 2 000 000 llamadas al intérprete (una por vuelta) | 8 entradas, 0 llamadas |
| `mfc0` (Count, Status, Cause, EPC) | 1 000 000 entradas, 1 000 000 llamadas | 7 entradas, 0 llamadas |
| `cvt` | 6 999 958 llamadas | 0 |
| `no alin.` | 5 999 964 llamadas | 0 |

**PS3 real** (2026-10-10, Slim CECH-3011A, HFW 4.91 + HEN; cpu-tests
SLSA00077 «Salsa64 CPU FPU» de e210aeb, mismo binario con y sin
`RARE_EXEC`, registros fijos):

| | Antes (`RARE_EXEC`) | Después | |
|---|---|---|---|
| nemu64-test | 1894 ms (99,9 MIPS, ×4,29) | **1816 ms (104,1 MIPS, ×4,47)** | −4,1 % |
| nemu64-test: entradas desde C | 140 909 | 110 074 | −22 % |
| nemu64-test: llamadas al intérprete | 2 154 724 | 1 773 947 | −18 % |
| bench `cvt` (CVT.S.W, TRUNC.W.S, CVT.D.S, CVT.S.D, SQRT.S, CFC1, CVT.W.S) | 4,2 MIPS | **40,7 MIPS** | ×9,7 |
| bench `no alin.` (pares LWL/LWR y SWL/SWR) | 20,5 MIPS | **218,5 MIPS** | ×10,7 |
| bench `mfc0` (Count, Status, Cause, EPC) | 9,8 MIPS | **700,7 MIPS** | ×71 |
| bench `espera` (bucle que sondea Count) | 8,3 MIPS; 2 000 000 salidas a C | **487,8 MIPS; 8 salidas** | ×59 |

Las demás variantes del bench no cambian (mixto 156 MIPS). Con el
intérprete solo, `cvt` corre a 4,4 MIPS: el camino completo de las
conversiones es caro. En nemu64-test la ganancia es chica porque lo que
queda interpretado son sus pruebas de COP0 (MTC0, TLB, ERET), traps y
CACHE; en un juego pesan las conversiones (cada `(int)f` es un TRUNC.W.S),
las copias no alineadas y las esperas por Count. En Zelda no se vería en
los campos/s todavía: donde no llega a 60 espera a la HLE gráfica del SPE.

El RESULTADO de esa corrida dice FALLO por 4 verificaciones de cobertura
(«corrió en nativo») que fallan a propósito en la variante `RARE_EXEC`;
las comparaciones de resultados pasaron en las tres variantes. Corregido
después (las verificaciones no aplican con esa opción).

### Recompilador: perfil por causa y salidas a C (2026-10-11)

Perfil nuevo (`src/cpu/rec_prof.h`, docs/rec-ppe.md «Perfil por causa»).
Primero las **cuentas** en qemu (no miden el Cell, pero dicen cuántas veces
pasa cada cosa), GoldenEye NTSC hasta la presa con `docs/pads/goldeneye-dam.pad`
(`SALSA64_REC_PROF=600`, 12 000 M ciclos = 128 s de consola), ventana de
campos 4800–5400 (en juego), por campo:

| | Antes (main 23a3289) | Después | |
|---|---|---|---|
| Entradas desde C al código generado | 1591 | 841 | −47 % |
| Llamadas a C desde el código | 2109 (CACHE 1006, COP0 690, FPU 160, E/S 254) | 2589 (COP0 2173, FPU 162, E/S 254) | los COP0 que antes corría el intérprete «a mitad» ahora son llamadas |
| Instrucciones interpretadas en el despachador | 13 872 (13 381 «a mitad», tras un COP0) | 507 (359 fallas de TLB al buscar código, 112 bloques sin compilar) | −96 % |
| Traducciones de la TLB (`vt_probes`) | ~14 000 (109 M en 128 s) | 360 revisiones selectivas (3,5 M en 128 s) | |
| Salidas por JR sin bloque a mano | 824 | 348 | la cadena de xhash y el prellenado de `ktlb` |
| Salidas por ERET/excepción | 37 | 397 | el ERET ya no lo corre el intérprete |
| Tiempo de qemu, 128 s de consola | 181,6 s | 165,4 s | orientativo |

Perfect Dark (intro, 64 s de consola), total por campo: 3163 cargas y 201
almacenamientos por C a direcciones mapeadas por la TLB (sus datos en una
página de 4 MB en 0x70000000), 770 CACHE, 200 COP0.

**Bloques, código y vaciados** (misma corrida de GoldenEye, 128 s; el
perfil por bloque pesa por ejecuciones):

| Estado | Bloques compilados | Instrucciones por bloque | Vaciados de la caché | Compilaciones por campo en la presa |
|---|---|---|---|---|
| main (antes) | 101 910 | 5,3 | 7 (16 MB) | 22 |
| + bloques desprendidos | 40 851 (28 206 enganchados de vuelta) | 5,3 | 3 | 0,7 |
| + superbloques | 131 228 | 11,8 | 17 (16 MB: ~900 bytes de PPC por bloque) | — |
| + rutinas de ktlb y caché de 24 MB | 26 243 | 11,8 | 2 | — |
| + camino lento compartido | 24 901 | 11,9 | 2 (10,3 MB al final; el segundo al cargar la presa) | 0,6 |

Perfect Dark con lo mismo (64 s): 16,1 instrucciones por bloque (5,2 de
memoria), 2 vaciados con 24 MB antes del camino lento compartido.

**PS3 real** (Slim CECH-3011A, HFW 4.91 + HEN; paquete SLSA00088 «Salsa64
JIT» con main fusionado, `perfil` puesto; el A/B es con el mismo binario:
`jit-viejo` repite el comportamiento anterior). GoldenEye NTSC en la presa,
~2 min de juego cada uno (ventanas de 5 s del log, resumidas con
`tools/jit-perfil.py`; las escenas no son idénticas, por eso también el
tiempo por instrucción ejecutada):

| | Camino viejo (`jit-viejo`) | Nuevo | |
|---|---|---|---|
| Campos/s en la presa | 33–59, media **43,0** (72 % de NTSC) | 41–59, media **49,4** (83 %) | +15 % |
| MIPS (sin bucles de espera) | 37,5 | 50,6 | +35 % |
| Emulación por campo | 22,7 ms | 20,1 ms | |
| … nativo | 13,7 | 16,0 | igual por instrucción (15,7 vs 15,6 ns) |
| … llamadas a C | 0,6 | 1,9 | los COP0 que antes corría el intérprete «a mitad» |
| … intérprete | 2,8 | 0,8 | |
| … eventos | 0,5 | 0,5 | |
| … compilar | 0,3 | 0,3 | |
| … ktlb (revisar tras la TLB) | 3,8 | 1,0 | |
| … despachador | 1,0 | 0,0 | |
| Tiempo por instrucción ejecutada | 26,0 ns | 19,6 ns | −25 % |
| Bloques compilados / vaciados (≈110 s) | 67 988 / 2 | 35 913 / 2 | |
| Traducciones de la TLB | 132,7 M | 4,4 M | |
| Instrucciones interpretadas | 68,8 M | 7,7 M | |

Lo que dice el desglose nuevo: **el código nativo es el 80 % del tiempo y
cuesta ~50 ciclos del PPE por instrucción MIPS** (15,6 ns); los
superbloques no bajaron ese costo por instrucción. Lo demás ya es chico:
llamadas a C 1,9 ms (COP0 del refill: 2200 por campo), ktlb 1,0 ms (las
~360 escrituras de la TLB por campo: la sincronía selectiva compara 32
entradas y prellena con la búsqueda lineal; se puede traducir directo
desde la entrada escrita), intérprete 0,8. Los 2 vaciados en ~110 s son
por tamaño (23 MB de los 24): cada uno recompila el conjunto vivo. Siguiente
objetivo: el costo por instrucción del nativo (FPU por memoria, registros
entre bloques).

Sin regresión en los ligeros con el mismo paquete (un minuto o más de
juego cada uno, PS3 real): Zelda OoT 59,4 campos/s de media (99–100 %,
emulación 7,7 ms por campo, 58 ventanas), Super Mario 64 59,5 (100 %, 5,4
ms, 70 ventanas), Mario Kart 64 59,4 (100 %, 5,9 ms, 34 ventanas); los tres
con «salida normal» y «cierre: estado limpio». Con `jit-viejo`, la primera
corrida se quedó en negro: el preámbulo cortaba antes de las rutinas
compartidas nuevas (arreglado; el camino viejo queda validado en qemu con
nemu64-test y GoldenEye).

### FPU residente, sin microcódigo, COP0 con rutinas y caché por tramos (2026-10-11, rama jovial-jones)

Lo que cambia en el código nativo (docs/rec-ppe.md «Lo que el PPE hace en
microcódigo», «FPU residente», «COP0 sin el intérprete», «Desalojo por
tramos»), y por qué debería bajar los ~50 ciclos por instrucción:

| Pieza | Antes (por instrucción afectada) | Ahora |
|---|---|---|
| ANDI de MIPS | `andi.` (microcódigo: ≥ 11 ciclos, vacía el pipeline) | `rlwinm` (1) o `li`+`ori`+`and` (3) |
| Entrada de un bloque con COP1 | `andis.` (≥ 11) | `rlwinm`+`cmpwi` (2) |
| Salida a C | `mtcrf 0x38` (microcódigo, 9 micropalabras) | 3 `mtocrf` (3) |
| ADD.S en la franja, inexacto | ~40 palabras, 2 cargas de bits + 2 de valor, `andi.`, FCR31 leído, modificado y guardado, TwoSum o prueba de exponentes: ~90 ciclos (bench) | ~27 palabras sin cargas ni almacenamientos de FCR31: fabs×2, 6 fcmpu, op, frsp, 2 fsub, 3 fcmpu, crops; FCR31 se escribe al salir del bloque |
| ADD.D | TwoSum: 5 operaciones dependientes (~50 ciclos de latencia) | dos restas independientes y dos comparaciones (~23) |
| LWC1 → operación dependiente | `stw` a casa + `lwz`/`lfs` de casa (load-hit-store) | `lfsx` directo de la RDRAM al registro residente |
| MTC0 EntryLo (×2 por refill) | exec_one + x_sync (~2800 ciclos cada llamada en la PS3, 2200 por campo) | 3 palabras |
| TLBWR | exec_one + comparar 32 entradas + compactar `live[]` | rutina propia: solo esa entrada |
| ERET | exec_one + redirect + despachador (buscar el bloque, entrar) | rutina propia + `exit_jr` al bloque destino |
| Caché llena | vaciado completo: ~7000 bloques recompilados de golpe (campos de 52 ms) | desalojo del tramo más viejo (1/8 de 32 MB) |

En qemu las cuentas del perfil (llamadas a C, instrucciones interpretadas)
no cambian con la FPU residente: el dominio del camino rápido es el mismo
y solo cambia lo que cuesta cada operación, que qemu no mide. GoldenEye
hasta la presa (128 s, `goldeneye-dam.pad`, caché de 24 MB del host), por
campo, antes → con los ayudantes de COP0 y el desalojo por tramos:

| | FPU residente (9d1eba0) | + COP0 y tramos (d719987) | + saltos internos (15d6724) |
|---|---|---|---|
| Llamadas a C | 1590 (COP0 1236, FPU 120, E/S 235) | 1016 (COP0 661, FPU 120) | 1016 |
| Entradas desde C | 482 | 265 (el ERET sigue por `exit_jr`) | 264 |
| Salidas por redirect (ERET, excepción) | 218 | 0 | 0 |
| Instrucciones interpretadas | 578 | 578 | 578 |
| Llamadas al intérprete en 128 s | 10,4 M | 6,0 M | 6,0 M |
| Entradas en 128 s | 3,69 M | 2,03 M | 2,02 M |
| Ejecuciones de bloques en 128 s | 643 M | 637 M | **509 M** (−21 %: los saltos internos ya no cortan) |
| Bloques compilados / vaciados / desalojos | 26 581 / 2 / — | 20 819 / 1 / 10 | 17 308 / 1 / 8 |
| Instrucciones por bloque | 11,9 | 12,0 | 12,3 |

**PS3 real** (2026-10-11, noche; Slim CECH-3011A, HFW 4.91 + HEN; paquetes
SLSA00093 «Salsa64 FPU» y SLSA00095 «Salsa64 CPU FPU» de la rama con main
69051fc; A/B con archivos-bandera, `perfil` puesto). GoldenEye NTSC en la
presa hasta el camión; costo por instrucción ejecutada en las ventanas
pesadas (MIPS > 40, espera ≤ 35 %), `emulación × campos/s / MIPS`. La
referencia de main es su log del A/B de la tarde (`salsa64-jit-nuevo.log`,
misma escena), medido igual:

| GoldenEye, presa (ns por instrucción) | main (ee146b6) | rama | rama, `jit-fpu-memoria` | rama, `jit-cop0-viejo` | rama, `jit-sin-tramos` |
|---|---|---|---|---|---|
| Total | 18,2 | 17,5 | 17,2 | 17,7 | 16,1 |
| … código nativo | 14,6 (47 ciclos) | 14,7 (47) | 14,9 (48) | 13,9 (44) | 13,8 (44) |
| … llamadas a C | 1,56 | **0,73** | 0,78 | 1,50 | 0,72 |
| … ktlb | 0,78 | **0,20** | 0,21 | 0,74 | 0,19 |
| … despachador | 0,00 | 0,22 | 0,20 | 0,00 | 0,21 |
| Ventanas pesadas | 10 | 20 | 11 | 7 | 8 |
| Campos/s en esas ventanas | 44,6 | 48,4 | 51,1 | 46,8 | 48,3 |

- **Lo que se ve:** los ayudantes de COP0 bajan las llamadas a C y la ktlb
  a la mitad o menos (−1,4 ns por instrucción; con `jit-cop0-viejo` vuelven
  a lo de main). En total la rama queda ~4 % por debajo de main por
  instrucción (18,2 → 17,5).
- **Lo que no se ve:** el código nativo sigue en ~47 ciclos del PPE por
  instrucción MIPS con o sin la FPU residente, sin microcódigo y con saltos
  internos. Las diferencias entre corridas del mismo binario (13,8–14,9 ns)
  son de escena. El nativo no está limitado por las instrucciones que se
  emiten: GoldenEye tiene ~7000 bloques vivos (~7 MB de código PPC) contra
  512 KB de L2 (fallos de caché de instrucciones y de L2 en casi cada
  bloque); el siguiente paso es el tamaño y la localidad del código, no
  instrucción por instrucción.
- **Desalojo por tramos:** en 5,7 min de la corrida 1, 38 desalojos y 1
  vaciado (por ids agotados: 98 000 bloques compilados); los peores campos
  (150–300 ms) son pantallas de carga en todas las corridas, también sin
  tramos. Las corridas cortas no llegan a llenar los 32 MB.
- **Páginas** (log al arrancar, syscall 351): la RDRAM ya está en páginas
  de 1 MB (heap de PSL1GHT); `struct salsa64_rec`, `n64` (en .bss) y la
  caché de código en páginas de 64 KB; la pila en 4 KB. `rdram-1m` no tiene
  qué ganar y no se midió. La caché (32 MB) son 512 páginas de 64 KB, más
  que el ERAT de instrucciones del PPE: otro motivo para achicar el código.

**Bench del recompilador** (cpu-tests, mismo paquete, variante con la FPU
residente y la de omisión; registros fijos):

| bench | antes de la rama (e210aeb) | FPU por memoria (omisión) | FPU residente |
|---|---|---|---|
| mixto | 156,6 MIPS | **168,2** | 135,1 |
| add.s | 35,7 | **40,0** | 30,9 |
| cvt | 40,7 | **50,6** | 45,7 |
| enteros / memoria / mov.s | 750 / 160 / 538 | 733 / 160 / 525 | 743 / 159 / 529 |
| nemu64-test | ×4,47 | **×4,28** | ×4,20 |

La FPU residente es **más lenta** (add.s −23 %, mixto −20 %): cada `fcmpu`
que alimenta un salto o un crop espera toda la latencia de la FPU, y `fabs`
agrega 10 ciclos a la clasificación; el camino por memoria clasifica en
enteros (2 ciclos) y solo espera a la FPU en la comparación del inexacto.
**Queda apagada por omisión** (`SALSA64_REC_OPT_FPU_RES`, archivo
`jit-fpu-residente` en la PS3, para retomarla). Lo que sí suma es quitar el
microcódigo: con la FPU por memoria, mixto 156,6 → 168,2 (+7 %), add.s
35,7 → 40,0 (+12 %), cvt 40,7 → 50,6 (+24 %).

Sin regresión en el resto (un minuto o más cada uno, sin banderas, FPU
residente, comparado con los logs de main de la tarde):

| Juego | campos/s main → rama | emulación por campo | nativo por instrucción |
|---|---|---|---|
| Zelda OoT (9 min) | 59,4 → 59,6 | 7,7 → 6,1 ms | 14,5 → 13,9 ns |
| Super Mario 64 | 59,5 → 59,6 | 5,4 → 4,9 ms | 14,5 → 13,6 ns |
| Mario Kart 64 | 59,4 → 59,6 | 5,9 → 5,6 ms | 13,3 → 12,4 ns |
| Perfect Dark (un nivel) | — | 7–20 ms | 49–50 campos/s al final por el RSP: 14 ms por campo esperando al SPE de la HLE gráfica (~4500 triángulos y 131 texturas decodificadas por tarea); el CPU usa 4,5 ms |

Logs en `build/ps3-logs/` de la rama (fpu-1 … fpu-8, cpu-fpu-bench*).

### Código compacto: palabras por instrucción MIPS (2026-10-13)

Diseño en docs/rec-ppe.md, «Código compacto». Medición en el host (qemu no
mide el Cell: cuenta palabras de PPC, que es lo que toca la caché de
instrucciones por instrucción emulada), GoldenEye NTSC hasta la presa con
`docs/pads/goldeneye-dam.pad`, 7,5 G ciclos, `SALSA64_REC_DUMP=1
SALSA64_REC_PROF=12000 SALSA64_REC_PROF_TOP=4000 SALSA64_REC_PIN=1`;
`tools/rec-expansion.py base.log nuevo.log --counter 8` pesa cada bloque
por sus ejecuciones y descuenta las 8 palabras del contador del perfil (que
la PS3 no lleva). Los dos volcados tienen los mismos 14 826 bloques y las
mismas cuentas del perfil (el recorrido es idéntico).

| | Antes (fd1891c) | Después | |
|---|---|---|---|
| Palabras por instrucción MIPS, pesadas por ejecuciones | 20,23 | **15,56** | −23 % |
| … sección caliente (la que se ejecuta) | 9,26 | **8,15** | −12 % |
| … sección fría (caminos lentos y salidas) | 10,97 | **7,40** | −33 % |
| Los 25 bloques más ejecutados (242 instrucciones) | 19,7 (9,1 + 10,6) | 15,4 (8,1 + 7,3) | |
| Volcado completo | 17,2 MB, 305 palabras por bloque (141 calientes) | 13,2 MB, 234 (125) | −23 % |
| Desalojos de tramos en la corrida | 6 | 4 | |

Por bloque (los más ejecutados; palabras totales antes → después, calientes
entre paréntesis): un bucle de 53 instrucciones con 26 accesos a memoria
(7F0BA3B8) 1285 → 900 (487 → 402); 20 instrucciones con 5 de COP1 y 6
accesos (7F0B0064) 781 → 583 (414 → 395); 13 instrucciones con 3 de COP1
(7F0B05C0) 370 → 275 (185 → 173); `jr ra` + `lw` (70012230) 59 → 51 (31 →
28); `bnel` + `lhu` (7F06C87C) 67 → 61 (33 → 28). El bloque de 2
instrucciones con SQRT.S (7001F7E0) baja solo en lo frío (92 → 78; 59
calientes): la raíz en línea sigue en 35 palabras.

Por mnemónico en la sección caliente de los 40 bloques más ejecutados
(palabras por instrucción MIPS, pesadas): antes `addi` 1,27, `bne` 0,70,
`ori` 0,66, `clrldi` 0,57, `cmpdi` 0,46, `or` 0,41, `extsw` 0,35, `sradi`
0,34, `std` 0,34, `lwz` 0,30; después `addi` 0,86, `bne` 0,70, `cmpdi`
0,46, `oris` 0,42, `addis` 0,35, `extsw` 0,35, `std` 0,33, `lwz` 0,30,
`ld` 0,27, `rldicl` 0,26. La prueba de dirección (`oris`, `addis`,
`rldicl`, `cmpdi`, `bne`: 5 palabras por acceso) y la carga con su `extsw`
son ahora el grueso de cada acceso; lo que queda grande por bloque es la
entrada (ktlb 4–5 palabras + evento 3) y la salida (escritura de registros +
`addi` + enlace de 3), y por instrucción la ALU de 32 bits con su `extsw`.

En la fría, lo que queda por acceso: el camino lento (`li`, `bl`, `bne`,
`mr`, `bgt`, `b`: 6), la prueba de la ktlb pegada (3) y las dos salidas
pendientes de la instrucción (3 y 5–6, compartidas con lo demás que salga
en esa instrucción), más el trampolín del bloque (4, una vez).

**PS3 real** (2026-10-13, Slim CECH-3011A, HFW 4.91 + HEN; paquete
SLSA00106 «Salsa64 compacto» con main b93d4ce fusionado; una ejecución de
la app para GE compacto → PD compacto → `jit-ancho` (el código de antes,
mismo binario) → GE → PD → SM64 y Zelda sin `jit-ancho`, y otra con
GoldenEye compacto 2 min más; sin `perfil`, así que el costo es por
instrucción emulada en total, `emulación × campos/s / MIPS`, en las
ventanas de 5 s con MIPS ≥ 40 y espera ≤ 35 %). Logs
`build/ps3-logs/compacto-2026-10-12.log` y `compacto-ge-2026-10-12.log`.

| | Compacto | Ancho (`jit-ancho`) | |
|---|---|---|---|
| GoldenEye presa, 1.ª corrida: ventanas pesadas (9 de 24) | **16,30 ns/instr**, 50,2 campos/s, 60,0 MIPS, 19,5 ms por campo | — | |
| GoldenEye presa, repetición (19 de 45) | **16,03 ns/instr**, 53,0 campos/s, 59,7 MIPS, 18,1 ms | — | |
| GoldenEye presa, ancho (12 de 24) | — | 17,26 ns/instr, 47,5 campos/s, 55,5 MIPS, 20,4 ms | **−5,6 % / −7,1 %** por instrucción; +6…+12 % de campos/s |
| GoldenEye, todas las ventanas (lo ligero inflado por las esperas) | 17,2 / 16,8 ns, 55,9 / 56,6 campos/s | 19,8 ns, 53,0 campos/s | |
| Perfect Dark intro a 5×, pesadas (5 y 4) | 14,45 ns (2.ª escena 15,2 / 14,5 / 16,9 / 15,2 → 15,4) | 14,85 ns (2.ª escena 16,5 / 15,5 / 16,1 → 16,0) | **−3 % / −4 %**; primera escena pesada 10,6 contra 11,3 (−7 %) |
| SM64 y Zelda OoT, 30 s | 59,7 y 59,5 campos/s (4,4 y 6,8 ms de emulación por campo) | — | sin regresión |

En la presa ventana a ventana (de pie, jugando): compacto 16,0–17,5 ns con
46–57 campos/s; ancho 17,2–18,6 ns con 40–53 campos/s; las ventanas de
entrada a la presa (menos espera, 68 MIPS) dan 11–14 en los dos. El autor
lo notó a ojo (5–10 % y menos tirones en lo pesado). Es la primera palanca
del recompilador que mueve el costo por instrucción del nativo desde la
Fase 3 (la FPU residente, el microcódigo y las rutinas de COP0 no lo
movieron; docs/cache-ppe.md, sección 2): confirma que lo que manda son las
líneas de caché por instrucción. Las escenas no son idénticas entre
corridas (el autor juega); por eso se compara el costo por instrucción y no
los campos/s. Pendiente: el muestreo por tiempo (abajo) para ver qué bloques
se llevan el tiempo antes y después, y con `perfil` el desglose nativo /
llamadas a C.

### Muestreo por tiempo en la PS3 (2026-10-13)

Primera corrida del muestreo (docs/rec-ppe.md, «Muestreo por tiempo»;
archivo `muestreo`, paquete SLSA00106 a5ea24e): GoldenEye en la presa y la
intro de Perfect Dark a 5×, compacto y `jit-ancho`, 95–200 s cada una;
4050–4300 muestras por segundo de las 5000 pedidas (el hilo pierde ~15 %
de sus despertares).

| | GoldenEye compacto | GoldenEye ancho | Perfect Dark compacto | Perfect Dark ancho |
|---|---|---|---|---|
| Muestras (s) | 774 936 (190) | 795 950 (197) | 415 053 (96) | 438 895 (102) |
| Código generado | 63,7 % | 66,0 % | 41,5 % | 46,2 % |
| Durmiendo al ritmo | 22,3 % | 21,2 % | 24,5 % | 19,7 % |
| Intérprete | 3,3 % | 3,0 % | 1,3 % | 1,3 % |
| Despachador | 2,6 % | 2,6 % | 0,8 % | 0,8 % |
| Eventos | 2,5 % | 2,9 % | 24,6 % | 25,1 % |
| Camino lento en C | 2,8 % | 1,6 % | 2,1 % | 1,3 % |
| Compilar | 0,6 % | 0,8 % | 2,1 % | 2,6 % |
| Ventanas pesadas: ns por instrucción emulada | 17,10 (9) | 17,63 (11) | 15,01 (5) | 15,24 (5) |

- **El muestreo cuesta ~5 %**: las mismas escenas sin él dieron 16,0–16,3
  (compacto) y 17,26 (ancho) en GoldenEye; con él, 17,10 y 17,63. Son las
  dos palabras y el almacenamiento por entrada a bloque (2,5 M por segundo)
  más el otro hilo de hardware despierto 4000 veces por segundo. Para medir
  ganancias se corre apagado; sirve para el reparto.
- **El reparto por bloque es plano**: en GoldenEye los 30 bloques con más
  tiempo suman ~30 % del nativo y ninguno pasa del 3 %; y es el mismo con el
  compacto y con el ancho (razón 0,9–1,2 por bloque). La ganancia del
  compacto está repartida por todo el código, como corresponde a «líneas
  por instrucción».
- **Tiempo contra entradas × palabras** (el perfil de la PS3 del día
  anterior): 7F0BA3B8, el primero por costo estimado (6,8 %), se lleva el
  0,7 % del tiempo (razón 0,10): superbloque de 53 instrucciones que sale
  por sus saltos internos casi siempre; las entradas no son instrucciones
  ejecutadas. Al revés, 80000000 (el refill de la TLB, 2,1 % del nativo,
  razón 15: incluye la C de TLBWR y ERET, que el siguiente paquete marca
  como camino lento), 7001F7E0 (JR + SQRT.S, 1,9 %, razón 4: `fsqrt` son
  66–84 ciclos sin pipeline), 7F03E470 (6,6) y 7000B2D0 (el bucle más
  entrado: 12 instrucciones, 99 palabras, razón 27) pesan mucho más de lo
  que dicen sus palabras: latencia, no tamaño.
- **Pesados por muestras**, los bloques que se llevan el tiempo tienen 21,3
  palabras de PPC por instrucción (contra 15,6 pesados por entradas): son
  los grandes con mucha FPU en línea (7F057F28: 41 instrucciones, 1172
  palabras, 838 calientes; en Perfect Dark 70018000: 64, 1860, 1237). Los
  primeros de GoldenEye: 7F0B05C0 3,1 % (13 instr., 3 de COP1), 7F057F28
  2,4, 80000000 2,1, 7001F7E0 1,9, 7F0B0688 1,8, 7F0B1794 1,4, 7F0581B0
  1,3, 7F0B0064 1,3. De Perfect Dark: 7F16CCA0 2,1, 7001A454 1,8,
  70048510 1,8 (3 instrucciones), 70017E30 1,5, 70018000 1,4.
- Lo que señala para compactar después: la FPU en línea (~40 palabras por
  operación), el refill de la TLB, y el 3 % de intérprete de GoldenEye
  (bloques que no caben antes del evento).

### Perfil persistente y zona caliente (2026-10-12, rama fervent-noyce)

Diseño en docs/rec-ppe.md, «Perfil persistente y bloques calientes». Lo que
se puede saber sin la PS3: cuánto del código que corre cabe en una zona
caliente de un tamaño dado. Perfiles generados en qemu (`salsa64-run --rec
--profile`), entradas a bloques (no vueltas de bucle) acumuladas por
tamaño de código de PPC de los bloques ordenados por entradas:

| Zona caliente | GoldenEye hasta la presa (80 s, 14 336 bloques, 17,9 MB de código) | Perfect Dark, intro (32 s, 9351 bloques, 11,2 MB) | nemu64-test (23 510 bloques, 20,9 MB) |
|---|---|---|---|
| 128 KB | 116 bloques, 47,0 % de las entradas | 120 bloques, 73,6 % | 176 bloques, 93,3 % |
| 256 KB | 229, 61,2 % | 202, 82,3 % | 329, 96,3 % |
| 512 KB | 462, 76,8 % | 399, 89,9 % | 626, 97,4 % |
| **768 KB** (lo que se marca con la zona de 1 MB) | **664, 83,3 %** | **577, 92,9 %** | 985, 97,9 % |
| 1 MB | 888, 88,0 % | 784, 95,0 % | 1324, 98,2 % |
| 2 MB | 1701, 94,6 % | 1810, 98,8 % | 2150, 98,6 % |
| 4 MB | 3494, 98,5 % | 3414, 99,9 % | 4160, 99,0 % |

GoldenEye cuenta 2,5 M entradas a bloques por segundo de consola (204 M en
80 s) y Perfect Dark 1,2 M; el contador son cuatro palabras por entrada.

Expansión del código (mismo volcado de GoldenEye, `SALSA64_REC_DUMP=1`, con
las palabras de la sección caliente en cada línea; `build/`-scripts de la
rama): 14 826 bloques, 17,9 MB de PPC, la sección caliente 8,0 MB (44 %);
media 317 palabras por bloque (140 calientes), mediana 156, p90 800. Los 25
bloques más ejecutados (perfil por bloque, 16 % de las entradas): 242
instrucciones MIPS en 5166 palabras, 21,3 por instrucción (9,9 calientes,
11,4 frías); de 10,7 (12 instrucciones, 2 accesos) a 44,5 (2 instrucciones
con SQRT.S) por instrucción. Desglose en docs/rec-ppe.md, «Siguientes
pasos», punto 2.
Con la zona de 1 MB (dos mitades de 512 KB) GoldenEye marca 663 bloques
calientes (767 KB, umbral 53 259 entradas) y Perfect Dark 576 (764 KB).
En qemu no se mide tiempo del Cell: lo que importa (ms por campo, ns por
instrucción del nativo) va abajo, medido en la PS3.

**PS3 real** (2026-10-12, noche; Slim CECH-3011A, HFW 4.91 + HEN; paquete
SLSA00104 «Salsa64 perfil», 4d75242 + main 1be3ce5; una sola ejecución de la
app, `perfil` presente, volviendo al menú entre juegos). Costo por
instrucción ejecutada en las ventanas pesadas (MIPS > 40, espera ≤ 35 %),
`emulación × campos/s / MIPS`; «base» es el mismo binario con el perfil
apagado (en ese paquete venía encendido y `sin-perfil` lo apagaba; desde el
mismo día va apagado por omisión y lo enciende `perfil-activo`):

| GoldenEye, presa, 1–2 min cada una | base (`sin-perfil`) | 1.ª sesión (perfil vacío: solo cuenta) | 2.ª sesión (perfil cargado: 715 calientes marcados, 762 colocados, 1592 sitios) |
|---|---|---|---|
| ns por instrucción, total | 17,15 | 18,19 (+6 %) | 18,26 |
| … código nativo | 14,82 (47 ciclos) | 15,54 (50) | 15,80 (51) |
| … llamadas a C | 0,83 | 0,79 | 0,85 |
| Ventanas pesadas: campos/s, MIPS, emulación | 13: 49,9, 56,9, 19,65 ms | 11: 45,5, 54,3, 21,73 ms | 7: 45,8, 54,0, 21,73 ms |
| Todas las ventanas: campos/s, emulación | 34: 55,0, 15,82 ms | 24: 52,5, 16,93 ms | 30: 53,8, 16,77 ms |

| Perfect Dark, intro a 5× hasta la segunda escena | 1.ª sesión | 2.ª sesión (555 calientes marcados, 593 colocados, 1544 sitios) |
|---|---|---|
| ns por instrucción, total / nativo / llamadas a C | 15,10 / 12,73 (41 ciclos) / 0,44 | 15,16 / 12,39 (40) / 0,39 |
| Ventanas pesadas: campos/s, MIPS, emulación | 5: 47,9, 60,9, 19,36 ms | 3: 47,6, 57,9, 18,77 ms |
| Todas: campos/s, emulación | 22: 55,7, 14,09 ms | 19: 55,9, 13,48 ms |

Sin regresión en lo ligero (una sesión, perfil nuevo): Super Mario 64 58,9
campos/s y 4,90 ms de emulación por campo (29 ventanas), Zelda OoT 59,5 y
7,40 ms (12), Majora's Mask 59,1 y 6,23 ms (13).

- **La zona caliente no se nota**: GoldenEye 15,54 → 15,80 ns por
  instrucción de nativo entre la primera y la segunda sesión, Perfect Dark
  12,73 → 12,39; las dos diferencias están dentro de lo que varía entre
  corridas de la misma escena (13,8–14,9 en mediciones anteriores). Los
  762 bloques más calientes de GoldenEye juntos en 1 MB (dos mitades de
  512 KB, una rotación) no cambian el costo del nativo. Lo que manda no es
  dónde está cada bloque sino cuántas líneas de caché toca por instrucción
  (21 palabras de PPC por instrucción MIPS en los bloques calientes, arriba):
  con eso, el conjunto que corre por campo no cabe en la L2 lo pongan donde
  lo pongan. La zona de 256 KB (la que sí cabría con los datos) quedó sin
  medir: `caliente = 256` en `perfil.cfg`.
- **Contar cuesta ~5 %** en GoldenEye (nativo 14,82 → 15,54 ns; total
  17,15 → 18,19): cuatro palabras y un almacenamiento por entrada a bloque
  (2,5 M por segundo) más las líneas de `hits[]`. Más de lo estimado
  (2 %); parte puede ser escena (las ventanas pesadas de la 1.ª sesión
  fueron más pesadas: 21,7 contra 19,7 ms). Los sitios de salida de la 2.ª
  sesión (1592 contadores en los caminos que salen) no se distinguen del
  ruido.
- **Lo que sí queda:** el perfil por juego con la cobertura (90 % del
  puntaje de GoldenEye en 1098 bloques; 256 KB de código cubren el 66 %),
  los sucesores (809 en GoldenEye, 659 en Perfect Dark) y las cuentas de
  los condicionales: de 931 con cien pasadas o más en GoldenEye, 612 se
  toman más del 80 % de las veces y 184 menos del 20 %; en Perfect Dark
  580 y 203 de 927. Es la base de las pistas de salto, el sucesor contiguo
  y la traducción por adelantado (docs/rec-ppe.md, «Siguientes pasos»).
  Logs en `build/ps3-logs/perfil-ps3-ftp-3.log` y perfiles en
  `build/ps3-logs/prf/` de la rama.

### Bench del intérprete tras traer la Fase 2: 21 → 14,7 MIPS (diagnóstico, 2026-10-10)

En la PS3, el bench del intérprete bajó en todas las variantes al traer la
Fase 2 (f18b392 → 59fba13; nop 34 → 19,5 MIPS, unos 70 ciclos más por
instrucción), sin cambios en interp.c. Lo revisado:
- `salsa64_cpu_run` es el mismo código byte a byte en los dos ELF (solo
  cambian direcciones y desplazamientos del TOC); en la Mac el bench da lo
  mismo con los dos árboles (~305 MIPS mixto).
- En la misma corrida de la PS3, el bench del recompilador no cambió
  (156,7 MIPS), y usa la misma `t_n64` y la misma RDRAM.
- `t_n64` sigue alineada a 128 (0x118ed580 → 0x118f5600) y `t_rdram`
  también (0x110ed500 → 0x110f5580); la tabla de saltos del intérprete está
  dentro del código (0x21180 → 0x21e90).
- Lo que solo el intérprete toca en cada instrucción: la búsqueda en la
  RDRAM de pruebas (`t_rdram + 0x1000`) y `sched->next`. Con la Fase 2,
  `sched->next` quedó en otra página de 4 KB que el CPU (0x118f6028) y esa
  página comparte los bits bajos con la de la búsqueda (0x110f6580). No se
  encontró por qué eso costaría 70 ciclos (en el ERAT de datos de 2 vías
  caben las dos); queda como hipótesis.

Prueba en la PS3 (cpu-tests SLSA00077 de e210aeb): «bench alineado» corre
las mismas variantes con el intérprete en una máquina aparte con estructura
y RDRAM alineadas a 64 KB. Resultado: **en ese binario los dos dan lo de
antes** (mixto 21,1 y 21,1 MIPS, nop 33,1 y 32,7), con `t_n64` en
0x121a2f00 y `t_rdram` en 0x119a2e80. Ahí `sched->next` también queda en
otra página de 4 KB que el CPU, con los mismos bits bajos que la de la
búsqueda, así que esa hipótesis queda descartada. La caída a 14,7 depende
de cómo queda armado el binario y no se encontró el mecanismo; si vuelve a
aparecer, comparar contra este binario.

## Líneas base de la Fase 2 (parte gráfica)

**PS3 real** (2026-10-10, Slim CECH-3011A, HFW 4.91 + HEN):

| Prueba | Commit | Resultado | Notas |
|---|---|---|---|
| `spu-jobs` (lib/spukit) | fffc4b5 | Ida y vuelta PPE → SPE → PPE de un trabajo vacío: **1,76 µs** (promedio de 1000; 0,04 µs en el SPE). 24 trabajos de 1 MB en 2 SPEs: 74,8 ms incluida la verificación en el PPE | El timbre por mailbox es barato frente a un cuadro (16,7 ms): un trabajo por cuadro no pesa |
| Frontend con gfxtest | fffc4b5 | `RESULTADO frontend: OK`, 13 de 13 puntos de la pantalla bien. Por tarea (29 triángulos, 9 dibujos, 3 texturas): **HLE en el SPE 0,12 ms**, PPE esperando al SPE 0,00 ms (el SPE termina antes del fin de la tarea), traducción al RSX en el PPE 0,05 ms (0,22 ms la primera ventana, con los destinos nuevos) | La emulación del CPU (intérprete) se lleva 74,6 ms por campo: 13,4 campos/s. La imagen es idéntica a la de RPCS3 |

En RPCS3 la misma tarea tarda 18 ms en el SPE: no concluyente.

**Zelda: Ocarina of Time en PS3 real** (2026-10-10, respaldo del usuario,
NTSC, fuera del repo; commit 5ca3815, `salsa64-fase2.pkg`, HLE en el SPE 0,
intérprete del CPU). Ventanas de 5 s del log del frontend; los tiempos de
etapa salen del decrementador del SPE (`gfx_result.t_*`):

| Escena | Campos/s | Emulación por campo | Por tarea gráfica | HLE en el SPE | Vértices | Estado (texturas) | Cargas a la TMEM |
|---|---|---|---|---|---|---|---|
| Logo de N64, campo ~150 | 17,5 (29 %) | 56 ms | 22 dibujos, 131 triángulos | 1,03 ms | 0,09 ms | 0,69 ms (0,47) | 0,12 ms |
| Intro nocturna, campo ~500 | 16,0 (27 %) | 62 ms | 59 dibujos, 320 triángulos | 10,3 ms | 1,12 ms | 4,51 ms (3,93) | 3,76 ms |
| Link en Epona bajo la luna, campo ~800 | 15,4 (26 %) | 64 ms | 136 dibujos, 1155 triángulos | 14,4 ms | 1,11 ms | 8,24 ms (6,82) | 3,77 ms |
| Título con el castillo, campo ~2000 | 15,4 (26 %) | 64 ms | 146 dibujos, 575 triángulos | 20,9 ms | 1,23 ms | 12,85 ms (11,34) | 5,66 ms |

- El cuello es el CPU (intérprete, 24 MIPS): la emulación se lleva 56–64 ms
  por campo. El PPE espera al SPE 13–19 ms por tarea, pero hay ~5 tareas
  por segundo, así que pesa menos que el CPU.
- En el SPE manda la decodificación de texturas (cada tarea vuelve a
  decodificar todo a RGBA8888) y luego las cargas a la TMEM: primero una
  caché de texturas entre tareas por contenido, después decodificación por
  formato con SIMD.
- La traducción al RSX en el PPE cuesta 0,2–0,4 ms por tarea.
- Misma escena en RPCS3: 16 ms por tarea en el SPE (no concluyente).

**Super Mario 64 en PS3 real** (2026-10-10, respaldo del usuario, USA/NTSC,
fuera del repo; commit d0bc043, paquete «Salsa64 F3D» SLSA00071, HLE de F3D
en el SPE 0, intérprete del CPU). Ventanas de 5 s del log del frontend:

| Escena | Campos/s | Emulación por campo | Por tarea gráfica | HLE en el SPE | Vértices | Estado (texturas) | Cargas a la TMEM |
|---|---|---|---|---|---|---|---|
| Arranque y logo, campos 0–~290 | 57,1 (95 %) | 3,0 ms | 7 dibujos, 1002 triángulos | 1,49 ms | 0,45 ms | 0,36 ms (0,34) | 0,09 ms |
| Paso del logo a la cara, campos ~290–460 | 32,7 (55 %) | 22,1 ms | 36 dibujos, 877 triángulos | 4,55 ms | 1,02 ms | 0,74 ms (0,52) | 1,86 ms |
| Cara de Mario, campos ~460–660 | 40,0 (67 %) | 23,8 ms | 57 dibujos, 1324 triángulos | 6,89 ms | 1,44 ms | 1,04 ms (0,65) | 3,07 ms |
| Selección de archivo | 60,1 (100 %) | 5,5 ms | 86 dibujos, 328 triángulos | 2,81 ms | 0,21 ms | 1,58 ms (1,06) | 0,56 ms |

- A diferencia de Zelda, el CPU solo pesa en la cara de Mario (la anima el
  CPU: 15 MIPS y 76 % en bucles de espera): 67 % de NTSC. El logo y la
  selección de archivo van a velocidad completa con el intérprete.
- En el SPE pesan las cargas a la TMEM de la cara (3 ms por tarea) y el
  estado de dibujo con muchas texturas (selección de archivo); la
  traducción al RSX en el PPE, 0,1–0,2 ms por tarea.
- gfxtest en F3D en la misma consola: 0,18 ms por tarea en el SPE, como en
  F3DEX2.

### Zelda OoT con el recompilador (2026-10-10)

Respaldo del usuario (`oot.z64`, NTSC, fuera del repositorio), frontend de
f416bee con el recompilador (registros fijos) y la HLE gráfica de la Fase 2
en el SPE 0. Ventanas de 5 s del log, desde el arranque:

| Tramo | Campos/s | Emulación por campo | HLE en el SPE por tarea | Triángulos por tarea |
|---|---|---|---|---|
| Logo de N64 | 58,1–59,9 | 3,8–7,6 ms | 1,2–10,7 ms | 132–459 |
| Intro | 54,3–59,8 | 9,3–11,7 ms | 13,9–22,1 ms | 300–1321 |
| Título y escenas con más dibujos | 41,4–50,3 | 11,7–14,5 ms | 20,0–29,8 ms | 900–1700 |
| Escenas ligeras (luego) | 58,9–59,9 | 7,1–8,9 ms | 10,3–14,2 ms | 165–400 |

Con el intérprete (Fase 2, misma consola): 15,4–17,5 campos/s y 56–64 ms
de emulación por campo. El CPU dejó de ser el cuello: donde no llega a 60,
el PPE espera a la tarea gráfica del SPE (caché de texturas pendiente en la
Fase 2). Lo interpretado (~190 000 instrucciones por segundo, 1,7 %) no
está desglosado: el intérprete termina el bloque antes de cada evento y
después de cada interrupción, y corre lo que el recompilador no toma
(código fuera de KSEG0, un salto en el delay slot de otro). Pendiente
medirlo por causa si llega a pesar.

## RSP a bajo nivel (Fase 6, prototipo; 2026-10-10)

Intérprete del microcódigo real (`src/rsp/lle`, docs/rsp-lle.md).

**Host** (Mac, clang nativo sin sanitizadores, `salsa64-run -c 1500000000`
= 16 s de consola de Zelda OoT, respaldo del usuario fuera del repo):

| Modo | Tiempo real | Instrucciones del RSP | Por instrucción |
|---|---|---|---|
| HLE (gráficos y audio) | 0,95 s | — | — |
| `--lle=all` (todo el microcódigo real) | 3,58 s | 211 M (1379 tareas, 153 000 por tarea, la más larga 542 000) | 12,5 ns (~40 ciclos del Mac) |

- nemu64-test: 2,0 s sin LLE, 3,1 s con `--lle` (5,9 M instrucciones del
  RSP en 240 000 arranques: la suite arranca el RSP por cada prueba).
- Super Mario 64, 9,6 s de consola: 161 M instrucciones (858 tareas).

**PS3 real** (2026-10-10, Slim CECH-3011A, HFW 4.91 + HEN; paquete «Salsa64
LLE» SLSA00082 con main bd9fcd3 fusionado; Zelda OoT, respaldo del usuario,
`lle` = `all`: todo el microcódigo real en un SPE, el RDP en el PPE).
Ventanas de 5 s del log («rsp lle:»):

| Tramo | Tareas (trabajos) | Instr. por tarea | SPE por tarea | ns por instrucción | MIPS del RSP emulado | Campos/s |
|---|---|---|---|---|---|---|
| Logo y título (F3DZEX2 + nead) | 251 (251) | 39 000 | 8,8 ms | 226 | 4,4 | 55 |
| Título, más geometría | 207 (209) | 59 000 | 15,4 ms | 260 | 3,8 | 24 |
| Intro, campo de Hyrule | 153 (153) | 96 000 | 27,0 ms | 282 | 3,6 | 30 |
| Intro, escenas grandes | 97 (97) | 170 000 | 48,2 ms | 283 | 3,5 | 38 |
| Intro, fondos S2DEX (DMA de muchas filas) | 53 (224) | 814 000 | 91 ms | 112 | 9,0 | — |

- El RSP real hace ~62,5 M instrucciones/s: el intérprete en C en el SPE
  va **14–18 veces más lento** (3,5–4,4 MIPS) en el microcódigo gráfico y
  de audio, y 7 veces más lento donde casi todo es DMA. Mucho peor que la
  estimación previa (0,3–1×): el SPU no tiene unidad escalar ni predicción
  de saltos, el `switch` de despacho es un salto indirecto por instrucción
  y cada acceso a COP0 que vive en el bus es un DMA atómico de ~1 µs.
- Tráfico de COP0 por tarea (Zelda): SP_STATUS 180–430 lecturas, semáforo
  2–190 tomas y sueltas (F3DEX2 lo usa como cerrojo del DMA), DMA 60–150
  filas de entrada y 30–70 de salida, DP 25–60 lecturas, 0 reintentos
  atómicos. Solo los accesos atómicos son 1,5–2,5 ms de los 8,8 ms por
  tarea del primer tramo.
- Con el mismo paquete y sin `lle` (HLE por omisión): Zelda 106 s al
  100 % de NTSC, audio 0 faltas, «salida normal»; GoldenEye intro y
  menús al 99–100 % y **en el nivel (Dam) 36–50 campos/s** (60–84 % de
  NTSC: HLE gráfica en el SPE 8–10 ms por tarea con 3 000–3 700
  triángulos, CPU a ~40 MIPS con 24–46 % en bucles de espera; el audio se
  estira). Es la primera medición del nivel en la PS3 (las anteriores eran
  de la intro); no hay A/B con el paquete de main.

## Generador de fragment programs (2026-10-10)

**PS3 real** (Slim CECH-3011A, HFW 4.91 + HEN), app SLSA00087 «Salsa64
fpgen» (136dff4; la tercera y cuarta corridas con ad14e4b, main 5a0a1a3
fusionado), respaldos del usuario fuera del repo, capturas cada 10 s
(`capturas`). Ventanas de 5 s del log («gfx: fragment programs:»).

| Juego | Escenas | Campos/s | Emulación por campo | Dibujos con programa generado por tarea | Programas generados | Cambios de programa por tarea |
|---|---|---|---|---|---|---|
| GoldenEye (238 s) | título, carpetas, Dam ~3 min | intro 56–60; Dam 34–55 | 3,5–5,7 ms; Dam 18–29 ms | 0; Dam 4–131 (de 56–351) | 4 (1 en la intro, 3 en Dam; 0 vaciados) | intro 2–6; Dam 7–23 |
| Zelda OoT (144 s) | título, Kokiri | 53–60 | — | 0 | 0 | 8–23 |
| Super Mario 64 (166 s) | castillo exterior | 54–60 | — | 0 | 0 | 8–10 |
| Mario Kart 64 (81 s) | título, menús, pista | 60 | — | 0 | 0 | 8–9 |

- Los «cambios de programa» cuentan también los precompilados (sin
  textura, una, dos): es el costo de siempre, ~20 palabras del búfer de
  comandos cada uno. El generador no agrega cambios en los juegos que no
  lo usan.
- GoldenEye es el único de los cuatro que necesita programas generados:
  los dos combiners de niebla por luz del nivel (3 términos: {}, {T0, C},
  {C}) y uno de la intro. La caché de 128 nunca se llena.
- No hay medición del tiempo del RSX por cuadro (sin contadores de GPU):
  la traducción en el PPE y el tiempo de emulación por campo no cambian
  frente a la medición anterior de Dam (36–50 campos/s); el cuello sigue
  en el CPU (~40 MIPS con 43 % en bucles de espera) y en la HLE del SPE
  (hasta 15 ms por tarea con 3 500–3 900 triángulos).
- Host (Mac, salsa64-run nativo, 7 500 M ciclos hasta Dam con
  `docs/pads/goldeneye-dam.pad`): 97 449 → 0 combiners aproximados.
