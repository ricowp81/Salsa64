# Ritmo del RCP

Una tarea gráfica emulada termina casi en el acto: la HLE la hace en el SPE 0
(en el host, en el hilo del CPU) y el núcleo da la interrupción del DP
`gfx_cycles` (0,32 ms de N64) después del arranque. Un juego cuya lógica avanza
por cuadro dibujado y que no espera el retrazo dibuja entonces más cuadros que
en la consola y corre más rápido. Es el issue #40: Star Wars: Shadows of the
Empire corre acelerado en todo el juego.

El ritmo del RCP (`gfx_model` en `src/core/rcp.h`, `gfx_model_hold` en
`src/core/rsp.c`) estima, con lo que hizo la HLE, cuánto tardaría el RSP real
en la tarea (`salsa64_gfx_model_cycles`, `src/rsp/gfx/sync.c`) y no la da por
terminada antes: la tarea queda recogida (`collected`) y su fin (interrupciones
del SP y del DP) se programa a `arranque + estimación × porcentaje / 100`. La
emulación sigue mientras tanto; si el juego espera al RCP, salta el bucle de
espera como siempre.

## Cómo se midió la consola (ares)

ares 148 con su servidor GDB (instancia propia, puerto distinto del de la
ventana del autor) y un guion en Python fuera del repositorio:

- **Interrupciones:** un Z0 en el vector de excepciones (0x80000180) para en
  cada excepción; con `Cause.ExcCode = 0` se lee `MI_INTR & MI_INTR_MASK` y se
  cuentan VI (campos), DP (fin de una tarea gráfica), SP y SI. En cada VI se lee
  `VI_ORIGIN`: sus cambios son los cuadros presentados. Tras cada parada se
  quita el Z0, se avanza una instrucción y se vuelve a poner.
- **Arranque de cada tarea:** un Z0 en `__osSpSetStatus` (la dirección sale de
  buscar `li a0, 0x125` + `jal` en un volcado de la RDRAM del host), contado si
  `a0 = 0x125` (`osSpTaskStartGo`). Se lee el OSTask de la DMEM (0xFC0): tipo,
  `data_ptr`, tamaño y `OS_TASK_YIELDED`. GoldenEye llama a la librería por su
  alias del TLB (0x70xxxxxx). El tiempo emulado de cada evento es el número de
  campo más `VI_CURRENT / V_SYNC`.
- **Duración de una tarea gráfica:** de su arranque a la interrupción del DP,
  sin los tramos cedidos a las de audio (la tarea cede con SIG1 en `SP_STATUS`
  y se reanuda con otro arranque del mismo OSTask).
- **Mandos sin manos:** en cada interrupción del SI se escribe el control 1 en
  el búfer del PIF de la RDRAM (`SI_DRAM_ADDR`, bloque `FF 01 04 01`) antes de
  que el juego lo lea. Los menús de SotE se recorren comparando una rejilla de
  16×12 de la imagen con pantallas de referencia.
- **Rasgos de la HLE de la misma tarea:** al arrancar algunas tareas se vuelcan
  la RDRAM (8 MB) y la DMEM; `salsa64-run --ares --no-png volcado` repite la
  tarea con la HLE y su línea «ritmo:» da cargas, `G_VTX`, vértices,
  triángulos de entrada y la estimación del modelo.

**El RDP de ares termina en el acto** (su `render` no cuenta ciclos; el
`SYNC_FULL` levanta la interrupción en cuanto se procesa): lo medido es el
tiempo del RSP real. El relleno del RDP real, que en escenas pesadas puede ser
mayor, no está; el modelo es una cota inferior de la consola.

## Lo medido (tiempo emulado, NTSC)

Shadows of the Empire (USA 1.2), batalla de Hoth sin tocar el control (el
deslizador vuela solo; se ven AT-ST y sondas, no se llegó a los AT-AT):

| | tareas gráficas/s | cuadros presentados/s |
|---|---|---|
| ares, inicio del nivel (campos 2600–3600) | 81 | 40 |
| ares, el resto (3600–12000, ventanas de 1000) | 80–89 | 40–44 |
| host antes (ventanas de 300 campos) | 145–154 (150) | 60 (el juego arma ~75) |
| host con el ritmo al 100 % | 76–110 (94) | 39–54 (47) |
| host al 150 % / 200 % | 74 / 61 | 37 / 31 |
| PS3 v0.2.11 (log del #40, otra escena de Hoth) | 136–142 | — |

Dos tareas por cuadro: una chica (0,06 ms en ares) y la grande, de 8,5 a
18 ms de RSP (mediana 10,8). En ares el juego hace la CPU (~13 ms) y espera
al RCP antes del cuadro siguiente: ~22 ms por cuadro. En el host la CPU tarda
lo mismo y el RCP 0,4 ms: 13 ms por cuadro.

## Modelo

Ciclos del VR4300 por comando de la lista, triángulo dibujado por la HLE y
vértice cargado (`gfx_result.vertices`, contado en `gfx_vertices`), sin término
fijo, por familia; mínimos cuadrados sin coeficientes negativos sobre 62 tareas
volcadas en ares:

| familia (GBI de la HLE) | comando | triángulo | vértice | tareas | error medio (máx.) |
|---|---|---|---|---|---|
| Fast3D (F3D, F3DWR, F3DPD, F3DDKR, F3DJFG) | 154 | 412 | 202 | SM64 10, SotE 12, GE 10 | 3 % (12 %) |
| F3DEX 1.x | 41 | 124 | 158 | F-Zero X 10 | 5 % (las grandes) |
| F3DEX2 (F3DZEX2, F3DEXBG, Acclaim, Eurocom) | 59 | 134 | 99 | OoT 10 | 2 % (7 %) |
| Factor 5, Turbo3D, S2DEX sueltos | — | — | — | sin medir | 0: sin ritmo |

Mario Kart 64 (F3DEX 0.95) tarda el doble que F-Zero X (2.03) en lo mismo: con
los coeficientes de F3DEX queda en la mitad, más rápido que la consola, nunca
más lento. Los rasgos que no entraron (rectángulos, cargas a la TMEM, vértices
con luz, triángulos de entrada) no mejoran el ajuste o salen negativos.

## Juegos de control (host, 5400 campos sin control desde el arranque)

Tareas gráficas por segundo, media de ventanas de 300 campos (los cuadros
presentados por segundo coinciden):

| juego | ares | host antes | host con el ritmo |
|---|---|---|---|
| Super Mario 64 | 22,2 | 28,5 | 28,5 |
| Zelda OoT | 21,4 | 21,7 | 21,7 |
| Mario Kart 64 | 28,1 | 26,9 | 26,9 |
| GoldenEye | 41,9 | 53,6 | 50,5 |
| F-Zero X | 56,9 | 57,1 | 57,1 |

Ninguna ventana queda más lenta que ares por el ritmo. Lo que separa a Mario 64
(la cabeza del título a 20 cuadros/s en ares) y a GoldenEye (los niveles de la
demostración a 13–25) del host es el tiempo de la CPU, no el del RCP. Los
huecos del AI no cambian (el ritmo no emula la cesión al audio: una tarea de
audio espera a que termine la gráfica retenida).

## Interruptores

- PS3: encendido por omisión solo en los juegos de `RITMO_RCP_JUEGOS`
  (`src/frontend/main.c`; hoy Shadows of the Empire, código `NSW?`).
  `/dev_hdd0/salsa64/rsp-ritmo` lo enciende en todos,
  `rsp-ritmo-<código>` (los 4 caracteres del código del cartucho, que el log
  escribe al abrir el juego) en uno, y `sin-rsp-ritmo` lo apaga. El archivo
  puede traer el porcentaje (10–400, 100 por omisión). El log dice «RSP: ritmo
  del RCP …» al abrir el juego y, cada 5 s, «gfx: ritmo del RCP al N %: X ms de
  N64 estimados por tarea; M de K tareas retrasadas».
- Host: `SALSA64_RITMO_RCP=porcentaje` (0 o sin la variable: apagado) y
  `SALSA64_RITMO=N`, que cada N campos escribe tareas gráficas y de audio,
  interrupciones del DP, cambios de `VI_ORIGIN` y la estimación por tarea.
- `rsp-gfx-ciclos` / `SALSA64_GFX_CYCLES` sigue siendo la duración mínima fija.

## Pendiente

- Medir en la PS3 (SotE en Hoth y un juego de control) y, si el ritmo de la
  consola real resulta más lento, subir el porcentaje del juego.
- El RDP (relleno por ciclo, modos de 1 y 2 ciclos, copia) con una referencia
  que lo cronometre: ares no lo hace.
- Cesión de la tarea gráfica retenida al audio (`osSpTaskYield`).
- Vigilante 8: 2nd Offense (sin respaldo en la Mac): `rsp-ritmo-<código>`.
