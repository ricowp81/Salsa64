# RetroAchievements

Salsa64 puede dar los logros de [RetroAchievements](https://retroachievements.org)
en los juegos de N64. Usa la biblioteca oficial **rcheevos** v12.5.0 (MIT,
`third_party/rcheevos/`, ver `docs/THIRD_PARTY.md`) con su API `rc_client`. Está
**apagado por omisión** y no toca la red mientras no lo actives.

## Cómo se usa

1. **Opciones > RetroAchievements: Sí.**
2. **Opciones > Cuenta de RetroAchievements** (×): el teclado del sistema pide
   primero el usuario y luego la contraseña. La contraseña se manda una vez al
   sitio; el sitio devuelve un **token** y solo ese se guarda, en
   `/dev_hdd0/salsa64/ra.cfg`. La fila pasa a mostrar el usuario; con × otra
   vez se cierra la sesión y se borra el token.
3. Abre un juego. Si la sesión está abierta, la ROM se identifica (MD5 de la
   ROM en formato z64, como lo hace RetroAchievements; las ROM `.v64` y `.n64`
   ya se normalizan al cargarlas) y aparece el aviso **«RA: N logros (X
   conseguidos)»**. Al desbloquear uno sale **«Logro: título (N pts)»**.
4. En la pausa (START + SELECT) está la fila **Logros**: la lista del juego con
   `[x]` en los conseguidos, su descripción y sus puntos (↑↓ elige, ←→ salta de
   a página, ○ vuelve al menú de pausa).

Los avisos salen en la esquina como los demás (`osd_notice`); en el menú, en la
línea de estado.

### El teclado del sistema

El callback de sysutil solo anota banderas (y escribe cada evento al log); el
bucle del menú hace el resto, sin dejar de presentar: cargado (0x502) →
terminado (0x503) → `oskUnloadAsync` → descargado (0x504) → recién entonces se
lee el texto y se libera el contenedor de 4 MB. Cada paso que no depende de quien
teclea tiene un tope de 5 s (`oskAbort` y aviso). La red no arranca (ni se
procesan sus trabajos) mientras el teclado está abierto, y un inicio de sesión
sin respuesta se cancela a los 40 s con aviso. Cada paso queda en el log
(«RA: teclado: …»).

## Hardcore

> **Estado:** el Hardcore de Salsa64 **todavía no está aprobado por
> RetroAchievements**. La aprobación exige que el emulador lleve 6 meses
> públicos y se pedirá en abril de 2027. Mientras tanto los logros normales
> funcionan; los de Hardcore se mandan al sitio, pero el emulador no figura en
> su lista de aprobados.

**Opciones > Hardcore** (apagado por omisión). Vale para el siguiente juego que
se abra y solo mientras ese juego tenga logros cargados. Las reglas
(`src/frontend/ra_rules.c`, con pruebas en `tests/unit/test_ra.c`) siguen los
[requisitos de RetroAchievements](https://docs.retroachievements.org/general/hardcore-compliance-requirements.html):

| Requisito | En Salsa64 |
|---|---|
| Cargar estados guardados | Bloqueado en todos los caminos (menú de pausa, SELECT + L1, prueba automática `estados-prueba`: todos pasan por `state_command`). Guardar un estado sí se permite, como dice el requisito. |
| Rebobinar, cámara lenta, cuadro a cuadro | No existen. El ritmo es el de la tele emulada; el único cambio es `turbo` (archivo de diagnóstico), que es adelanto rápido y se permite. |
| Trucos | No se muestran ni se aplican (pausa, panel y `cheats_apply`). No hay lector de memoria, depurador, ni repetición de entradas en el frontend; `replay` repite volcados gráficos, sin juego. |
| Pasar a Hardcore a mitad de juego | No vale: el juego abierto sigue como estaba y el ajuste se aplica al abrir el siguiente (queda en el log). Dejar Hardcore a mitad de juego sí vale al instante. En la práctica Opciones solo se abre desde la lista de juegos. |
| Reanudar una sesión | No hay reanudación automática de partidas: cada juego abre en limpio. |
| User agent | Propio: `Salsa64/<versión> rcheevos/12.5` (nombre y versión numérica; nunca otro cliente). |

Además: los logros y las tablas de clasificación se mandan como *hardcore*
(`h=1`), y el aviso «RA Hardcore: sin cargar estados ni trucos» sale al abrir el juego.
Reiniciar con el botón Reset y los ajustes de imagen (filtros, escala,
interpolación, aspecto) siguen permitidos: no alteran el juego.

## Archivos

| Archivo | Para qué |
|---|---|
| `menu.cfg` | `retroachievements = si\|no` y `hardcore = si\|no` |
| `ra.cfg` | `usuario`, `token` (nunca la contraseña), y a mano: `servidor = https://…` (otra base para el sitio) y `certificados = laxo` |
| `salsa64.log` | líneas «RA: …»: huella, juego, cada petición (solo el tipo y el código HTTP), costo por campo |

Borrar `ra.cfg` equivale a cerrar la sesión.

## Memoria y huella

- La RDRAM emulada es big-endian; RetroAchievements numera la RDRAM desde 0 y la
  espera como la ve un anfitrión little-endian (palabras de 32 bits al revés),
  así que el byte `a` de RetroAchievements es el byte `a ^ 3` de la nuestra
  (`ra_mem_read`). Cubre 0 – 8 MB (4 MB y Expansion Pak).
- La huella se calcula con `rc_hash` en el hilo de red sobre la ROM ya
  normalizada en memoria, sin bloquear el arranque.
- `rc_client_do_frame` corre una vez por campo emulado, en el hilo de emulación.
  Si en ese momento el hilo de red está entregando una respuesta (cerrojo
  ocupado) se salta ese campo en vez de esperar. El costo medio y el peor caso
  por campo salen en el log cada 1800 campos y al cerrar el juego.

## Red

Todas las peticiones de rc_client van por `src/frontend/netstack.c` (libhttp +
libssl, la misma pila que las descargas) en un hilo propio; las respuestas se
entregan a rc_client desde ese hilo, bajo el cerrojo del cliente. El
User-Agent es `Salsa64/<versión> rcheevos/12.5`.

**Certificados:** las credenciales y el token viajan por TLS, así que por
omisión una cadena de certificados que la consola no puede verificar **rechaza
la conexión** (el log dice «certificado de … no verificable»). Los certificados
de la consola son viejos y puede que no incluyan la raíz del sitio. Si pasa, la
salida es `certificados = laxo` en `ra.cfg` (acepta sin verificar: solo en una
red de confianza) o poner un `servidor = http://…` propio que haga de puente.

## Qué no hace (todavía)

- Insignias e iconos de los logros (solo texto).
- Marcadores de progreso, indicadores de desafío y tablas de clasificación en
  pantalla: rc_client las procesa y las manda al servidor, pero no se dibujan.
- Presencia enriquecida: el cliente la manda sola en el ping; no se muestra.
- Idiomas: los textos nuevos están traducidos a los 18 idiomas con traducción
  automática (revisarlos con hablantes nativos).

## Pruebas

`tests/unit/test_ra.c` (en `make test-host` y en las pruebas del CPU de la PS3):
lectura de la RDRAM con el orden de bytes de RA, huella de una ROM sintética en
z64, v64 y n64, y el flujo completo de `rc_client` contra un servidor simulado
en el mismo proceso (inicio de sesión bueno y malo, juego conocido y
desconocido, desbloqueo de dos logros con su aviso, Hardcore, sin red). Con
`RA_TEST_LOG=1` imprime el registro de rcheevos. No se hacen peticiones reales.
