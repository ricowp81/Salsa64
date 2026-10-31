# rsxkit

> **English:** A generic graphics layer for the PlayStation 3's RSX (video setup, double-buffered frames, 2D/3D drawing, a run-time fragment program generator and a post-processing chain), for homebrew built with [PSL1GHT](https://github.com/ps3dev/PSL1GHT). MIT licensed. It was born inside [Salsa64](https://github.com/ricowp81/Salsa64), a Nintendo 64 emulator for the PS3, but knows nothing about any emulated system. The documentation below is in Spanish; the API is plain C. Third-party material: [THIRD_PARTY.md](THIRD_PARTY.md).
>
> Repositorio propio: [github.com/ricowp81/rsxkit](https://github.com/ricowp81/rsxkit). Salsa64 lo lleva en `lib/rsxkit` (git subtree) y los cambios se sincronizan en los dos sentidos.

Capa gráfica genérica para el RSX de la PlayStation 3 con
[PSL1GHT](https://github.com/ps3dev/PSL1GHT). Nació dentro de Salsa64 (un
emulador de N64), pero no sabe nada de ningún sistema emulado: sirve a
cualquier núcleo o programa homebrew que necesite presentar imágenes de la
CPU en pantalla. Licencia **MIT** (`LICENSE`), para que también la puedan
usar proyectos que no son GPL.

Estado: hace la inicialización del RSX y la salida de video, el ciclo de
cuadro con doble búfer, un framebuffer 2D escalado, dibujo 3D en destinos
de render a una escala entera (triángulos con una combinación lineal de dos
texturas y el color del vértice, o con un fragment program generado en
tiempo de ejecución para cualquier polinomio en esas entradas), texto con
una fuente de mapa de bits, un 2D para menús (rectángulos, texto
escalable y ondas animadas en el RSX) y la maquinaria de posprocesado
(pasadas encadenadas con fragment programs precompilados, sobre
superficies intermedias, y marcas de tiempo del RSX para medirlas). Lo que
viene (conversión de texturas en SPE) se agregará con la misma regla: nada
específico de un sistema emulado. Los filtros concretos no van aquí: los
pone quien usa rsxkit, con sus propios programas.

## Compilar

Requiere el toolchain de PSL1GHT (`ppu-gcc`, `ppu-ar`, `bin2s`) y `cgcomp`
con el Cg Toolkit de NVIDIA para los dos shaders del blit.

```bash
make                    # → build/librsxkit.a
make OUT=/otra/ruta     # salida en otro directorio
```

Para usarla: `-I<rsxkit>/include` y enlazar `librsxkit.a` antes de
`-lrsx -lgcm_sys -lsysutil -lio`.

## Uso

```c
#include <rsxkit/rsxkit.h>

rsxkit_init(NULL);                          /* 720p si se puede */
rsxkit_fb *fb = rsxkit_fb_create(640, 480); /* tamaño máximo de imagen */
const rsxkit_view view = { RSXKIT_SCALE_FIT, RSXKIT_FILTER_LINEAR, 4.0f / 3.0f };

for (;;) {
	/* ... producir la imagen en `pixels` ... */
	rsxkit_frame_begin(0x000000);
	rsxkit_fb_upload(fb, pixels, 320, 240, 320 * 2, RSXKIT_RGBA5551);
	rsxkit_fb_draw(fb, &view, NULL);
	rsxkit_frame_end();                     /* flip en vsync */
}

rsxkit_shutdown();
```

Todas las llamadas desde el mismo hilo. No hay asignaciones de memoria
después de `rsxkit_init` y `rsxkit_fb_create`.

## API

### Inicialización

| Función | Qué hace |
|---|---|
| `int rsxkit_init(const rsxkit_config *cfg)` | Reserva la memoria principal mapeada al RSX y el búfer de comandos, elige la primera resolución disponible de `cfg->modes` (por omisión 720p, 1080, 480, 576), crea dos búferes de color X8R8G8B8 y uno de profundidad en la memoria local del RSX. `cfg` puede ser `NULL`. Devuelve `RSXKIT_OK` o un código negativo (`RSXKIT_EINIT`, `RSXKIT_EVIDEO`, `RSXKIT_ENOMEM`). |
| `void rsxkit_shutdown(void)` | Espera al RSX y libera lo reservado. |
| `int rsxkit_video_reconfigure(const rsxkit_config *cfg)` | Cambia la salida de video sin reiniciar el RSX (`rsxInit`/gcm solo se inicializa una vez por proceso y la memoria mapeada no se desmapea): elige el primer modo de `cfg->modes` que la tele dé (`NULL`: lista por omisión), rehace `videoConfigure` y los búferes de color y profundidad. Conserva contexto, memoria mapeada, framebuffers, destinos y la 2D; `rsxkit_screen_info()` cambia, así que hay que recalcular lo que dependa del tamaño. Se llama entre cuadros. Si falla deja el modo anterior y devuelve el error. |
| `const rsxkit_screen *rsxkit_screen_info(void)` | Ancho y alto de la superficie de salida y proporción de la tele (`aspect`: 16/9 o 4/3, según la configuración de video del sistema). |

### Cuadro

| Función | Qué hace |
|---|---|
| `void rsxkit_frame_begin(uint32_t clear_rgb)` | Empieza el cuadro en el búfer trasero y lo limpia con `0xRRGGBB`. Hay como máximo un flip en vuelo: si el anterior no ocurrió, aquí espera, en el PPE (si la aplicación produce cuadros más rápido que la tele, la frena aquí; si va más lenta, no espera nunca). El RSX no queda parado esperando el flip (sin `gcmSetWaitFlip`): con el RSX en esa espera, el sistema de la PS3 tardaba segundos en dibujar su menú encima. |
| `void rsxkit_frame_end(void)` | Programa el flip para el siguiente vsync y vuelve sin esperar. |
| `uint64_t rsxkit_vblank_count(void)` | Vblanks desde el arranque, para diagnosticar flips lentos. |
| `int rsxkit_after_system(void)` | Después de que el sistema dibujó encima (el menú del botón PS): olvida el estado del RSX que rsxkit daba por puesto para mandarlo de nuevo, y revisa los programas precompilados en la memoria del RSX; devuelve cuántos estaban cambiados (los restaura). |
| `bool rsxkit_frame_ready(void)` | `true` si `rsxkit_frame_end` no va a esperar (ningún flip en vuelo, o ya ocurrió). No bloquea. En la PS3, mientras el sistema dibuja su menú encima de la aplicación, los flips tardan segundos en completarse: presentar solo cuando da `true` para no dejar de atender los eventos del sistema. |
| `void rsxkit_finish(void)` | Espera a que el RSX termine todo lo enviado. |
| `void rsxkit_set_wait_hook(fn, ctx, after_us, period_us)` | Esperas largas: si una espera del PPE al RSX (una etiqueta o barrera, `rsxkit_finish`, el flip) pasa de `after_us`, rsxkit llama a `fn(ctx, &info)` desde el hilo que espera, cada `period_us` mientras dure y una vez más al terminar (`info.done`). `rsxkit_wait_info` dice qué espera (`what`, etiqueta, valor esperado y actual), GET y PUT del búfer de comandos, GET al empezar (si no cambió, el RSX no avanzó) y el tiempo. El gancho puede diagnosticar, atender al sistema o salir del proceso; no puede llamar a rsxkit. Si devuelve `false` la espera se abandona y rsxkit queda perdido (`rsxkit_lost`): no vuelve a esperar al RSX y lo que se mande puede perderse; queda cerrar. |
| `bool rsxkit_wait_now(rsxkit_wait_info *out)` | La espera en curso, desde cualquier hilo (un vigía). Es la única forma de ver la espera de lugar en el búfer de comandos (`RSXKIT_WAIT_FIFO`): corre dentro del callback del sistema y no llama al gancho. |
| `void rsxkit_rsx_errors(rsxkit_rsx_error *out)` | Excepciones del RSX que avisó el sistema: `rsxkit_init` registra `gcmSetGraphicsHandler` y el aviso solo anota cuántas, el valor de la primera y de la última, cuándo y GET/PUT en ese momento. Desde cualquier hilo. |
| `void rsxkit_fifo_info_get(rsxkit_fifo_info *out)` | El búfer de comandos en offsets de E/S (memoria que lo contiene, segmento en curso, dónde escribe rsxkit), GET, PUT y REF, y la última barrera y el último `rsxkit_finish` mandados y escritos por el RSX. Desde cualquier hilo. |
| `unsigned rsxkit_trace_get(out, max)` / `rsxkit_trace_format` | Rastro de las últimas 64 operaciones de rsxkit (ligar, borrar, dibujos seguidos juntos, cargas de la z, lecturas y llenados de destinos, barreras, flips…), cada una con el offset del búfer de comandos en que empezó. Se anota en el PPE, sin órdenes al RSX. Con GET, PUT y las etiquetas dice qué tenía el RSX delante al colgarse. |
| `bool rsxkit_abandon(uint32_t *get, uint32_t *put_before)` | Para salir con el RSX colgado: rsxkit no lo vuelve a esperar ni a vaciar el búfer (ni barreras, ni `rsxkit_finish`, ni flips) y, si GET está dentro del búfer y no pasa de PUT, pone PUT = GET para que no le quede nada pendiente. |
| `void rsxkit_set_safe(uint32_t flags)` | Blindaje (todo encendido por omisión). `RSXKIT_SAFE_SIFM_EVEN`: `rsxkit_depth_load` reparte la copia en tramos de ancho de entrada par (el motor de imagen escalada; un ancho impar es lo último que se mandó antes de que el RSX de la PS3 dejara de avanzar); el tramo impar lee un texel más de su fila, que no llega a la salida. |
| `const uint32_t *rsxkit_back_buffer(uint32_t *pitch)` | Búfer de color del cuadro en curso (X8R8G8B8), para pruebas automáticas. Llamar después de `rsxkit_finish`. Leer la memoria del RSX desde el PPE es muy lento. |

### Framebuffer 2D

Una imagen en memoria principal se copia a una textura lineal en la
memoria local del RSX y se dibuja como un rectángulo. Cada `rsxkit_fb`
tiene dos copias: se sube una mientras el RSX puede estar leyendo la otra
del cuadro anterior; si no, `rsxkit_fb_upload` espera a la etiqueta del
RSX que marca el fin de ese dibujo.

| Función | Qué hace |
|---|---|
| `rsxkit_fb *rsxkit_fb_create(uint32_t max_w, uint32_t max_h)` | Reserva dos texturas de `max_w × max_h` a 32 bits. Hasta 4 framebuffers a la vez. `NULL` si no hay memoria. |
| `void rsxkit_fb_destroy(rsxkit_fb *fb)` | Espera a que el RSX deje de usarlo y lo libera. |
| `int rsxkit_fb_upload(fb, pixels, w, h, pitch, format)` | Copia `h` líneas de `w` píxeles (separadas por `pitch` bytes). `RSXKIT_EARG` si no cabe o el formato no existe. |
| `void rsxkit_fb_draw(fb, view, rect)` | Dibuja la última imagen subida en el cuadro en curso; si `rect` no es `NULL`, recibe el rectángulo de pantalla usado. |
| `rsxkit_rect rsxkit_layout(w, h, view)` | El rectángulo que usaría `rsxkit_fb_draw`, sin dibujar. |

Formatos (`rsxkit_format`), en el orden de bytes de la PS3 (big-endian). El
RSX los lee tal cual: el formato de textura y su reordenamiento de canales
("remap") hacen la conversión, sin pasar los píxeles por el PPE. El alfa se
ignora al presentar.

| Formato | Píxel | Textura del RSX |
|---|---|---|
| `RSXKIT_RGBA5551` | 16 bits: R[15:11] G[10:6] B[5:1] A[0] (el de la N64) | R5G5B5A1 (`0x97`; en PSL1GHT, `GCM_TEXTURE_FORMAT_R5G6B5A1`) |
| `RSXKIT_XRGB1555` | 16 bits: X[15] R[14:10] G[9:5] B[4:0] (0RGB1555 de libretro) | A1R5G5B5 |
| `RSXKIT_RGB565` | 16 bits: R[15:11] G[10:5] B[4:0] | R5G6B5 |
| `RSXKIT_RGBA8888` | 32 bits: bytes R, G, B, A | A8R8G8B8 con los canales rotados |
| `RSXKIT_XRGB8888` | 32 bits: bytes X, R, G, B | A8R8G8B8 |

Vista (`rsxkit_view`):

- `scale`: `RSXKIT_SCALE_FIT` (lo más grande posible con la proporción
  `aspect`, centrado), `RSXKIT_SCALE_INTEGER` (alto múltiplo entero del de
  la imagen) o `RSXKIT_SCALE_STRETCH` (toda la pantalla).
- `filter`: `RSXKIT_FILTER_LINEAR` o `RSXKIT_FILTER_NEAREST`.
- `aspect`: ancho/alto con que debe verse la imagen (4/3 para una consola
  de tele de definición estándar); `0` = píxeles cuadrados. Tiene en cuenta
  que en 480/576 los píxeles de la superficie no son cuadrados.

### Memoria principal y barreras

| Función | Qué hace |
|---|---|
| `int rsxkit_map_main(void *p, uint32_t size, uint32_t *offset)` | Hace visible para el RSX memoria principal (alineada a 1 MB, tamaño múltiplo de 1 MB) y da su offset. Ahí pueden vivir vértices y texturas que escribe la CPU o un SPE, sin copias. |
| `int rsxkit_unmap_main(void *p)` | Deshace un `rsxkit_map_main` (la misma dirección) para poder liberar esa memoria; espera antes a que el RSX termine lo enviado. |
| `uint32_t rsxkit_fence(void)` | Marca lo enviado hasta ahora. |
| `int rsxkit_fence_done(uint32_t f)` / `void rsxkit_fence_wait(uint32_t f)` | ¿Ya lo terminó el RSX? / esperarlo. Sirve para reutilizar un búfer que el RSX estaba leyendo. `rsxkit_fence_done` solo lee la etiqueta que escribe el RSX y es la única función que puede llamarse desde otro hilo (el que produce los búferes). |

### Destinos de render

Un destino es una superficie X8R8G8B8 (o A8R8G8B8, ver abajo) en la memoria local; la profundidad
(Z24S8) va aparte para que varios destinos compartan una. Al ligarlos se da
un **espacio lógico** (por ejemplo 320×240) que se estira sobre todo el
destino: un núcleo dibuja en sus coordenadas y la escala (2×, 3×…) la pone
el tamaño del destino.

| Función | Qué hace |
|---|---|
| `rsxkit_target *rsxkit_target_create(w, h)` / `rsxkit_target_destroy(t)` | Destino de color; nace negro. Hasta 16 vivos a la vez (profundidades aparte, también 16). |
| `rsxkit_depth *rsxkit_depth_create(w, h)` / `rsxkit_depth_destroy(d)` | Profundidad; nace en 1. Debe ser al menos tan grande como el destino. |
| `int rsxkit_bind(t, d, lw, lh)` | Dibujar en `t` (y `d`, o `NULL`) con espacio lógico `lw × lh`, hasta otro `rsxkit_bind` o `rsxkit_frame_begin`. Sin `d` (o con una más chica que `t`) liga la profundidad de la pantalla, con prueba y escritura apagadas; si tampoco cubre a `t`, no liga nada y devuelve `RSXKIT_EDEPTH` (una zeta más chica que el color puede fallar o recortar en el RSX). |
| `int rsxkit_depth_reserve(w, h)` | Agranda la profundidad de la pantalla a al menos `w × h`, para ligar sin profundidad propia destinos más grandes que la pantalla (1600×1200 en 720p). Reserva al llamarla (fuera del bucle de dibujo), olvida lo ligado y se conserva en `rsxkit_video_reconfigure`; `0 × 0` vuelve al tamaño de la pantalla. `rsxkit_target_copy`, `rsxkit_target_fill` y `rsxkit_target_read` también devuelven `RSXKIT_EDEPTH` si su superficie no cabe en ella. |
| `rsxkit_target *rsxkit_target_create_ex(w, h, flags)` / `int rsxkit_target_has_alpha(t)` | Con `RSXKIT_TARGET_ALPHA`, un destino A8R8G8B8 con canal alfa, para render a textura con transparencia (una imagen RGBA5551 de una consola de 16 bits). Nace con alfa 0. Lo escriben: `rsxkit_clear` (el byte alto de `rgb` es el alfa), los dibujos (según `rsxkit_draw.dst_alpha`, con cualquier mezcla del color: `RSXKIT_DSTA_ONE` deja alfa 1 en cada píxel escrito, `RSXKIT_DSTA_FRAG` el alfa del fragmento —rellenos de un color de alfa 0/1, copias de textura—, `RSXKIT_DSTA_KEEP` el del destino sin cambiar y `RSXKIT_DSTA_MAX` el mayor de los dos; hasta 2026-10-31 `RSXKIT_DSTA_ONE` dejaba en realidad el del fragmento), `rsxkit_target_fill` (el bit de alfa de `RSXKIT_RGBA5551`) y `rsxkit_target_copy` (copia el alfa si el origen también lo tiene). `rsxkit_target_texture` lo devuelve como `RSXKIT_ARGB8888` (los destinos sin alfa, como `RSXKIT_XRGB8888`): el 3D y el posprocesado leen su alfa; `rsxkit_target_present` lo ignora. Las mezclas `RSXKIT_BLEND_ALPHA_MAX`, `_REPLACE_MIN_ALPHA` y `_KEEP_MIN_ALPHA` dejan en el alfa el máximo o el mínimo con el del destino, sin mirar `dst_alpha`. `RSXKIT_CLEAR_ALPHA` en `rsxkit_clear` toma el byte alto como alfa aunque el destino no tenga canal alfa. `rsxkit_target_create_rgba(w, h)` es `rsxkit_target_create_ex` con `RSXKIT_TARGET_ALPHA`. Con `RSXKIT_DSTA_ONE` o `_MAX` cada dibujo en un destino con alfa mezcla (factor constante o ecuación MAX en el canal alfa, también en los opacos): úsese en los que se leen como textura, no en el cuadro principal. |
| `void rsxkit_clear(flags, rgb, depth, rect)` | Borra color y/o profundidad en un rectángulo lógico (`NULL`: todo). `rgb` es `0x00RRGGBB`; en un destino con alfa el byte alto es el alfa. |
| `void rsxkit_target_present(t, src, img_w, img_h, view, out)` | En el cuadro en curso, presenta la región `src` del destino (cualquier rectángulo: sirve para recortar el overscan) acomodada como una imagen de `img_w × img_h` (como `rsxkit_fb_draw`). |

Un destino como textura, y de vuelta a la memoria principal (efectos de
framebuffer de un núcleo: render a textura, lo que el CPU emulado lee de su
memoria de video):

| Función | Qué hace |
|---|---|
| `void rsxkit_target_texture(t, y0, rows, out)` | Llena una `rsxkit_texture` para leer en un dibujo 3D `rows` filas enteras del destino desde `y0` (el offset de una textura va alineado a 128: la columna va en `xf`). Deja offset, pitch, tamaño, formato y lugar; quien llama pone wrap, filter y `xf`. No puede ser el destino ligado. |
| `void rsxkit_texture_sync(void)` | Antes del primer dibujo que lea como textura un destino dibujado en el mismo tramo: espera al RSX e invalida su caché de texturas. |
| `int rsxkit_target_copy(dst, src)` | Copia todo `src` en `dst` (para leer lo que se está dibujando: se copia y se lee la copia). Deja `dst` ligado sin profundidad. |
| `int rsxkit_target_fill(t, src_offset, src_pitch, w, h, format)` | Pinta todo el destino con una imagen de la memoria principal mapeada, estirada sin filtro (cebar un destino con lo que la CPU dejó en su memoria). Deja `t` ligado sin profundidad. |
| `int rsxkit_target_read(t, src, dst_offset, dst_pitch, img_w, img_h, format)` | Reduce la región `src` del destino a `img_w × img_h` (filtro lineal) y la escribe en memoria principal mapeada, en `RSXKIT_RGB565`, `RSXKIT_XRGB8888` o `RSXKIT_ARGB8888` (este con el alfa de un destino `RSXKIT_TARGET_ALPHA`, filtrado como el color; 1 si no tiene), con el pipeline 3D sobre una superficie en memoria principal. Encola y vuelve: `rsxkit_fence` / `rsxkit_fence_wait` dicen cuándo está. Los píxeles de 16 bits quedan en orden de bytes de la PS3, sin tramado (el RSX no suma su patrón de *dithering*: una imagen que vuelve por aquí cada cuadro lo acumularía). Deshace lo ligado. |
| `void rsxkit_pixels_565_to_5551(dst, src, n)` | RGB565 → RGBA5551 con alfa 1 (el formato de 16 bits de una consola), cuatro píxeles por palabra; `dst` y `src` pueden ser el mismo. |

### Dibujo 3D

```c
rsxkit_vertices(offset_vb, RSXKIT_MEM_MAIN);  /* arreglo de rsxkit_vertex */
rsxkit_bind(target, depth, 320, 240);
rsxkit_draw d = { .blend = RSXKIT_BLEND_OFF, .depth = RSXKIT_DEPTH_TEST | RSXKIT_DEPTH_WRITE,
                  .scissor = { 0, 0, 320, 240 }, .ntex = 1, ... };
d.k[RSXKIT_K_T0][1][0..3] = 1;                /* color = textura × color del vértice */
rsxkit_draw_triangles(&d, first, count);
```

- **Vértice** (`rsxkit_vertex`, 32 bytes): posición homogénea en el espacio
  lógico (x/w, y/w en píxeles lógicos con la y hacia abajo, z/w en [0, 1],
  w > 0 delante), coordenadas de textura `st` y color RGBA de 8 bits. La
  perspectiva la corrige el RSX con la w.
- **Combinación lineal** (por píxel, con `nterms = 0`):
  `color = T0·X0 + T0.aaaa·X1 + T1·X2 + T1.aaaa·X3 + X4`, con
  `X_j = k[j][0] + k[j][1]·C + k[j][2]·C.aaaa` y C el color del vértice
  (los `X_j` los arma el vertex program). Representa exactamente cualquier
  mezcla de dos texturas que no las multiplique entre sí; sirve para los
  combiners de una consola o para el clásico textura × color. Son tres
  fragment programs precompilados (sin textura, una, dos).
- **Términos con programa generado** (`nterms > 0`): ver «Generador de
  fragment programs» más abajo.
- **Texturas** (`rsxkit_texture`): lineales, en memoria local o principal,
  con los formatos de `rsxkit_format`, repetición, espejo, límite o espejo
  una sola vez (`RSXKIT_WRAP_MIRROR_ONCE`: u en [−1, 0] es el reflejo de
  [0, 1] y fuera de [−1, 1] se fija el borde), filtro y la transformación
  `u = s·xf[0] + xf[2]`, `v = t·xf[1] + xf[3]`.
- **Estado**: mezcla (`rsxkit_blend`: opaca, alfa `src·a + dst·(1 − a)`,
  aditiva `src·a + dst`, sin color, y los demás productos con el alfa del
  fragmento que un blender de consola pide: `dst·a`, `src + dst·a`,
  `src·(1 − a) + dst·a` y `src + dst`), profundidad (prueba
  LEQUAL, escritura, desplazamiento para calcomanías), recorte de caras
  (frente = antihorario con la y hacia arriba), prueba de alfa, tijeras
  lógicas (recortadas al destino; si quedan vacías el dibujo no se manda:
  el RSX de la PS3 se colgó con una x de tijeras mayor que 4095) y niebla por vértice (`fog`: mezcla hacia `fog_color` según
  `C.a`). La z no se recorta contra los planos cercano y lejano: se satura.
- **Profundidad constante** (`RSXKIT_DEPTH_CONST`): todo el dibujo prueba y
  escribe la `z` de `rsxkit_draw` en vez de la de sus vértices. La pone el
  viewport (escala 0, desplazamiento `z`), no la división entre w, así que
  dos dibujos con la misma `z` dan exactamente la misma profundidad en cada
  píxel y LEQUAL deja pasar al último: orden de pintor sobre un z-buffer
  (la profundidad del primitivo del RDP de la N64). Con `z = c` en los
  vértices (`pos[2] = c·pos[3]`) cada vértice redondearía distinto y los
  dibujos coplanares se pisarían al azar.
- **Sombreado lineal en pantalla** (`rsxkit_affine_shade(1)`): los `X_j`
  (color del vértice, su alfa y la niebla) se interpolan sin corrección de
  perspectiva, como el Gouraud del RDP de la N64 y de otras consolas de la
  época; las coordenadas de textura siguen con perspectiva. El vertex
  program multiplica cada `X_j` por w y los fragment programs los leen con
  el bit del RSX que entrega la entrada dividida entre w: no cuesta
  instrucciones por píxel (`docs/rsx-notes.md` del emulador).
- `rsxkit_vertices` invalida las cachés de vértices y texturas del RSX:
  llamarlo cada vez que cambió lo que hay en memoria.
- **Segundo color por vértice**: la palabra `pad` del vértice puede llevar
  otro RGBA de 8 bits; con `rsxkit_vertices_color_aux(1)` los dibujos
  siguientes lo leen como COLOR0 en lugar de `rgba` (datos por vértice para
  un programa propio sin otro arreglo ni constantes por dibujo).
- **Stencil**: prueba (siempre, distinto, igual) con referencia y máscaras,
  y operaciones separadas al pasar (`stencil_op`), al fallar la profundidad
  (`stencil_zfail`) y al fallar el stencil (`stencil_sfail`): conservar,
  reemplazar o poner a cero. La profundidad admite también `RSXKIT_DEPTH_EQUAL`.
- rsxkit recuerda el estado y solo reenvía lo que cambió. Un programa propio
  (`rsxkit_draw.program`) se carga solo al cambiar de programa y su etiqueta
  de uso se escribe al cambiarlo, al salir del 3D o antes de destruirlo;
  `rsxkit_3d_program_sync(1)` vuelve a cargarlo y etiquetarlo en cada dibujo.

### Generador de fragment programs

No hay compilador Cg en la consola, así que para lo que la combinación
lineal no representa (una textura por otra, el color del vértice por su
alfa, cualquier producto de las entradas) rsxkit **emite el microcódigo
del fragment program** del RSX en tiempo de ejecución (`rsxkit/fpgen.h`,
`src/fpgen.c`) y lo cachea (`src/fpcache.c`). Un dibujo pide uno con
`nterms > 0`:

```c
rsxkit_draw d = { ..., .ntex = 2, .nterms = 2 };
/* color = T0·T1·X0 + X1 */
d.factors[0][0] = RSXKIT_FP_T0; d.factors[0][1] = RSXKIT_FP_T1;
d.k[0][0][0..3] = 1;                 /* X0 = 1 */
d.k[1][1][0..3] = 1;                 /* X1 = C (color del vértice) */
rsxkit_draw_triangles(&d, first, count);
```

- El programa calcula `color = saturate(Σ_j (Π factors[j]) · X_j)`, con
  los mismos `X_j` del vertex program (`k[j]`, hasta `RSXKIT_K_TERMS` = 7
  términos, uno por varying TEXCOORD1..7) y hasta `RSXKIT_K_FACTORS` = 4
  factores por término: `RSXKIT_FP_T0`, `RSXKIT_FP_T0A` (alfa de la
  textura 0, replicado), `RSXKIT_FP_T1`, `RSXKIT_FP_T1A`, `RSXKIT_FP_C`
  (color del vértice) y `RSXKIT_FP_CA` (su alfa, replicado); 0 termina la
  lista. Un término sin factores suma `X_j`.
- Los cuatro canales van en paralelo: en el canal alfa cada textura
  aporta su alfa y C su alfa, así que un combiner con alfa distinto del
  color se expresa con coeficientes distintos en el cuarto carril de los
  `k[j]`.
- Con `fog`, el color de niebla se suma al **primer término sin
  factores**: incluir uno (con `k` en cero si no hace falta para otra
  cosa).
- `rsxkit_fp_desc` (`ntex`, `nterms`, `factors`) identifica el programa;
  la caché lo busca por hash de la descripción canónica (factores de cada
  término ordenados; el orden de los términos sí cuenta, porque fija qué
  `X_j` lee cada uno). Conviene que quien traduce ponga los términos en
  un orden canónico para que el mismo conjunto dé siempre el mismo
  programa. Un juego usa decenas de programas distintos; la caché guarda
  128 en 96 KB de memoria local del RSX y, si se llena, espera al RSX y
  se vacía (`rsxkit_fp_stats.evictions`).
- Costo: 1 instrucción por textura, 1 por término más 1 por factor
  adicional (y 1 más si el único factor es el color del vértice). Las
  operaciones van en fp16, como las de cgcomp. Cambiar de programa son
  ~20 palabras en el búfer de comandos; `rsxkit_fp_get_stats` cuenta
  programas, generaciones y cargas.
- El codificador (`rsxkit_fpgen_emit`) no depende de PSL1GHT: compila en
  cualquier host y las pruebas unitarias de Salsa64 lo verifican con un
  intérprete del microcódigo anclado a programas compilados por cgcomp
  (`tests/unit/test_fpgen.c`, `tools/fpgen-ref.sh`). Puede usarse solo
  para generar programas fuera de la consola.

Formato del microcódigo: ver la cabecera de `src/fpgen.c` y
`docs/rsx-notes.md` del repositorio de Salsa64.

### Texto en pantalla (`rsxkit/text.h`)

Para menús y mensajes: texto con una fuente de mapa de bits sobre una imagen
en memoria principal (un **lienzo**), que después se presenta como cualquier
otra con `rsxkit_fb_upload` + `rsxkit_fb_draw`. No usa el RSX ni PSL1GHT, no
asigna memoria y compila en cualquier host (se puede probar en la PC).

```c
#include <rsxkit/text.h>

static uint32_t px[360][640];
rsxkit_canvas c = { px, 640, 360, 640 * 4, RSXKIT_XRGB8888 };
rsxkit_fill(&c, 0, 0, 640, 360, 0x101820);
rsxkit_text_draw(&c, 24, 14, "Elige un juego…", 0, 0xE8ECF0, 1);
rsxkit_fb_upload(fb, px, 640, 360, 640 * 4, RSXKIT_XRGB8888);
rsxkit_fb_draw(fb, &(rsxkit_view){ RSXKIT_SCALE_FIT, RSXKIT_FILTER_NEAREST, 16.0f / 9.0f }, NULL);
```

| Función | Qué hace |
|---|---|
| `uint32_t rsxkit_color(format, rgb)` | `0xRRGGBB` en el formato del lienzo (alfa opaco). |
| `void rsxkit_fill(c, x, y, w, h, rgb)` | Rellena un rectángulo, recortado al lienzo. |
| `uint32_t rsxkit_text_draw(c, x, y, utf8, max_cells, rgb, scale)` | Dibuja una cadena UTF-8 con la esquina superior izquierda de la primera celda en (x, y); cada píxel de la fuente como un cuadro de `scale × scale`. Solo pinta los píxeles de los glifos y recorta en los bordes. Si la cadena tiene más de `max_cells` celdas (0 = sin límite), dibuja `max_cells − 1` y «…». Devuelve las celdas dibujadas. |
| `uint32_t rsxkit_text_cells(utf8)` | Caracteres de una cadena UTF-8. |
| `int rsxkit_font_has(cp)` | ¿La fuente tiene ese punto de código? |

- Fuente: la «fixed» 8x13 de X11 (paquete misc-misc de X.Org, **dominio
  público**: «Public domain font. Share and enjoy.»), celda de
  `RSXKIT_FONT_W × RSXKIT_FONT_H` = 8 × 13 con la línea base en la fila 11.
  Trae ASCII, Latin-1 (acentos, ñ, ¿¡) y unos símbolos para menús: flechas
  ← ↑ → ↓, triángulos ▲ ▼ ◀ ▶, ● ○ ■ □ •, — – « » y «…». Lo que no tiene se
  dibuja como «?»; cada byte de una secuencia UTF-8 inválida, como «�».
- La tabla de glifos (`src/font_8x13.h`) la genera `tools/bdf2c.py` desde
  el BDF; para otra fuente BDF de 8 píxeles de ancho y celda fija basta
  volver a correrlo.
- Los píxeles se escriben en el orden de bytes de la PS3 (big-endian) sea
  cual sea el host, en cualquiera de los formatos de `rsxkit_format`.
- Con un lienzo de 640 × 360 a 720p (2×) o 1080 (3×) y
  `RSXKIT_FILTER_NEAREST`, el texto sale nítido: 80 columnas × 27 filas.

### 2D en el RSX para menús (`rsxkit/ui.h`)

Rectángulos con degradado, texto y ondas animadas, dibujados por el RSX
sobre la pantalla del cuadro en curso con mezcla alfa. El PPE solo arma
unos miles de vértices por cuadro.

```c
#include <rsxkit/ui.h>

rsxkit_ui_init();                               /* una vez, después de rsxkit_init */
for (;;) {
	rsxkit_frame_begin(0x000000);
	rsxkit_ui_begin(1280.0f, 720.0f);           /* espacio lógico */
	rsxkit_ui_gradient(0, 0, 1280, 720, 0x123058FF, 0x123058FF, 0x2C1048FF, 0x2C1048FF);
	rsxkit_wave ola = { 430, 210, { 29, 13 }, { 0.85f, 2.1f }, { 0.03f, -0.055f }, 0, 2.5f,
	                    0xFFFFFF6C, 0xFFFFFF24 };
	rsxkit_ui_wave(&ola, segundos);
	rsxkit_ui_text(100, 300, "Hola", 3.0f, 0xFFFFFFFF, 0);
	rsxkit_ui_end();
	rsxkit_frame_end();
}
```

| Función | Qué hace |
|---|---|
| `int rsxkit_ui_init(void)` | Reserva 6 MB de memoria principal mapeada al RSX: el atlas (la 8x13 y la caché de glifos), dos búferes de vértices que se turnan con una barrera y `RSXKIT_UI_IMAGES` ranuras de imagen. |
| `void rsxkit_ui_begin(lw, lh)` / `rsxkit_ui_end()` | Empieza el lote sobre la pantalla del cuadro (`rsxkit_bind_frame`) con espacio lógico `lw × lh` / lo envía. |
| `void rsxkit_ui_begin_target(t, lw, lh)` | Igual que `rsxkit_ui_begin`, sobre un destino de render (`rsxkit_target_create`) en vez de la pantalla; sin borrarlo (`rsxkit_clear` antes). Con `rsxkit_texture_sync` y `rsxkit_target_texture` la imagen se usa como textura (p. ej. para un filtro de posprocesado). |
| `rsxkit_ui_gradient(x, y, w, h, tl, tr, bl, br)` / `rsxkit_ui_rect(x, y, w, h, rgba)` | Rectángulo con un color por esquina / de un color. |
| `float rsxkit_ui_text(x, y, utf8, scale, rgba, max_cells)` | Texto: a escala 1 el renglón mide 13 unidades de alto; con la 8x13 cada carácter, 8 de ancho. Con `max_cells`, «…» al final si no cabe en `max_cells × 8 × scale`. Devuelve el ancho. |
| `float rsxkit_ui_text_fit(x, y, utf8, scale, rgba, max_w)` | Igual, con el límite como ancho lógico. |
| `float rsxkit_ui_text_width(utf8, scale)` / `rsxkit_ui_text_fit_width(utf8, scale, max_w)` | Ancho con la fuente en uso (con la 8x13, celdas × 8 × escala) / el que dibujaría `_fit` con ese límite (para alinear a la derecha). |
| `size_t rsxkit_ui_text_wrap(utf8, scale, max_w, &next)` | Corte de línea: bytes del primer renglón que caben; corta en espacios o, en japonés, chino y coreano, entre caracteres (sin empezar renglón con la puntuación de cierre). |
| `rsxkit_ui_font(const rsxkit_glyph_source *src)` | Fuente externa (proporcional) para todo el texto; `NULL`, la 8x13. Ver «Fuente externa» abajo. |
| `rsxkit_ui_font_stats_get(&st)` | Estado de la caché de glifos (aciertos, rasterizados, reemplazos, diferidos, con la 8x13). |
| `rsxkit_ui_hook(fn, user)` | Un dibujo propio (3D con `rsxkit_draw_triangles`, por ejemplo un modelo girando) en medio del lote: lo anterior queda debajo y lo posterior encima. `rsxkit_ui_end` llama a `fn(user)` en ese punto con la pantalla del cuadro ligada y el mismo espacio lógico; `fn` puede cambiar los vértices con `rsxkit_vertices` (el 2D vuelve a poner los suyos), no debe llamar a otra `rsxkit_ui_*` y debe tener su propia memoria de vértices y texturas. Hasta `RSXKIT_UI_MAX_HOOKS` (4) por cuadro; la pantalla del cuadro no tiene profundidad, así que un modelo ordena sus triángulos de atrás hacia adelante. |
| `rsxkit_ui_wave(const rsxkit_wave *w, float t)` | Una onda a lo ancho: cresta de dos senos con línea brillante y lámina que se desvanece hacia abajo, en el instante `t`. |
| `int rsxkit_ui_image_set(slot, rgba, w, h)` | Sube una imagen RGBA8888 (bytes R, G, B, A; hasta `RSXKIT_UI_IMAGE_MAX_W × _H`, 320 × 448) a una ranura (`RSXKIT_UI_IMAGES`, hoy 4). Espera a que el RSX termine los cuadros que la usan: se llama entre cuadros y rara vez (al cambiar de carátula). |
| `int rsxkit_ui_image_update(slot, x, y, w, h, rgba)` | Reescribe un rectángulo de una imagen ya subida (una casilla de un atlas de miniaturas): `w × h` píxeles RGBA8888 seguidos. Espera al RSX como `_set`: no más de una por cuadro. |
| `rsxkit_ui_image(slot, x, y, w, h, rgba)` | Dibuja la imagen de la ranura estirada al rectángulo; `rgba` la tiñe y le da alfa (`0xFFFFFFFF` tal cual). Sin imagen subida, no dibuja nada. |
| `rsxkit_ui_image_part(slot, x, y, w, h, sx, sy, sw, sh, rgba)` | Igual, pero solo el rectángulo (`sx`, `sy`, `sw`, `sh`) de la imagen, en píxeles: varias figuras en una misma ranura. |
| `void rsxkit_bind_frame(lw, lh)` (`rsxkit.h`) | El 3D de `rsxkit_draw_triangles` sobre la pantalla del cuadro en vez de un destino. |

- Colores `0xRRGGBBAA`. Todo con mezcla alfa, sin profundidad.
- El atlas tiene los glifos de `rsxkit/text.h` al doble (16 × 26) con
  **Scale2x** (la regla de EPX de Andrea Mazzoleni: las diagonales salen
  suaves) y alfa de 8 bits; con el filtro lineal del RSX el texto escala
  bien. Con un espacio lógico de 720 de alto en una pantalla de 720 líneas,
  la escala 2 va un téxel por píxel.
- Cada llamada (rectángulo, texto, láminas y líneas de una onda) va en su
  propia llamada de dibujo al RSX: lo que se superpone se mezcla en orden
  también en RPCS3, que dentro de una sola llamada no ve lo dibujado antes
  (el RSX real sí). Con el estado en caché, cada una son unas palabras del
  búfer de comandos.
- La geometría no usa el RSX: con `rsxkit_ui_batch` y `rsxkit_ui_atlas` se
  arma en memoria propia, para pruebas y vistas previas en la PC.

#### Fuente externa y caché de glifos

`rsxkit_ui_font(src)` cambia la fuente de todo el texto del 2D por una que
da glifos con `rsxkit_glyph_source.render`: rasteriza un carácter con
cobertura de 8 bits en una caja de `RSXKIT_GLYPH_BOX` (26 × 26 téxeles, el
renglón entero al doble) y devuelve su avance, dónde empieza la imagen y su
ancho. Lo que la fuente no tiene (`render` < 0) se dibuja con la 8x13.

- El atlas (512 × 952) guarda arriba a la izquierda la 8x13 y en el resto
  `RSXKIT_UI_GLYPH_SLOTS` (392) casillas de 28 × 28 para los glifos
  rasterizados. Una tabla de 2048 glifos conocidos guarda el avance aunque
  la imagen salga del atlas: medir rasteriza cada carácter una sola vez.
- Reemplazo: si no hay casilla libre, la usada hace más cuadros (LRU por
  cuadro), nunca una usada en este cuadro o en el anterior, que el RSX puede
  estar leyendo (`rsxkit_ui_begin` espera la barrera del de hace dos). No se
  espera al RSX ni se corta el cuadro: lo que no cabe, o lo que pasa de 32
  rasterizados por cuadro para volver al atlas, sale en el cuadro siguiente.
- El texto proporcional cambia las cuentas de quien mide: usar
  `rsxkit_ui_text_width`, `_fit` y `_wrap` en vez de celdas × 8.

### Fuente del sistema de la PS3 (`rsxkit/sysfont.h`)

Las fuentes del XMB (Rodin, NewRodin, YG para el coreano, DFHei5 para el
chino) con los módulos del firmware libfont, FreeType y libfontFT. Se leen
de `/dev_flash` en tiempo de ejecución: no van en el programa. Enlazar con
`-lfontFT -lfont -lsysmodule`.

```c
#include <rsxkit/sysfont.h>

if (rsxkit_sysfont_open(RSXKIT_SYSFONT_JAPANESE, 0) == RSXKIT_OK)
	rsxkit_ui_font(rsxkit_sysfont_source());   /* si falla, sigue la 8x13 */
...
rsxkit_ui_font(NULL);
rsxkit_sysfont_close();
```

| Función | Qué hace |
|---|---|
| `int rsxkit_sysfont_open(script, bold)` | Carga los módulos, inicia libfont (1 MB de caché de archivos) y abre el juego de fuentes de la escritura (`RSXKIT_SYSFONT_LATIN`, `_JAPANESE`, `_KOREAN`, `_TCHINESE`, `_SCHINESE`), probando del más completo («RODIN2», con Latin2) al más viejo. Otra llamada cambia de juego (vaciar la caché con `rsxkit_ui_font(NULL)` y volver a ponerla). |
| `rsxkit_sysfont_source()` / `rsxkit_sysfont_has(cp)` | La fuente para `rsxkit_ui_font` / si dibuja un carácter. |
| `rsxkit_sysfont_info_get(&info)` | Juego abierto, calibración, en qué paso y con qué código falló, memoria de libfont (actual y máxima). |
| `rsxkit_sysfont_close()` | Cierra fuente, renderizador y biblioteca y descarga los módulos. |

Los glifos salen a 22 píxeles por eme con la línea base en la fila 20 de la
caja. Detalles, códigos de los juegos de fuentes y trampas de la API en las
notas del RSX de Salsa64 (`docs/fuente-sistema.md`).
- Hasta `RSXKIT_UI_MAX_QUADS` (4096) quads por cuadro; los de más se
  descartan y se cuentan (`rsxkit_ui_dropped`).

### Posprocesado (`rsxkit/post.h`)

Pasadas de pantalla completa con fragment programs precompilados (Cg con
cgcomp), encadenadas sobre superficies intermedias hasta la pantalla del
cuadro. Es la maquinaria; los filtros (un CRT, un escalado) los escribe
quien la usa.

```c
#include <rsxkit/post.h>

/* Al preparar (una vez por tamaño): superficies y programas. */
rsxkit_surface *chico = rsxkit_surface_create(320, 240, RSXKIT_POST_RGBA8);
rsxkit_surface *tabla = rsxkit_surface_create(320, 240, RSXKIT_POST_RGBA16F);
rsxkit_program *p1 = rsxkit_program_create(mi_tabla_fpo);
rsxkit_program *p2 = rsxkit_program_create(mi_final_fpo);
const float gamma[4] = { 2.4f, 0, 0, 0 };
rsxkit_program_set(p1, "k", gamma, 1);          /* uniform float4 k */

/* En cada cuadro, entre rsxkit_frame_begin y rsxkit_frame_end: */
rsxkit_texture src;
rsxkit_target_texture(destino, 0, 0, &src);     /* la imagen a escala 5× */
rsxkit_post_reduce(&src, &region, 5, chico);    /* a 320 × 240, filtro de caja */
rsxkit_post_pass a = { .program = p1, .out = tabla, .inputs = 1 };
a.in[0] = rsxkit_post_surface_input(chico, RSXKIT_FILTER_NEAREST, RSXKIT_POST_CLAMP);
rsxkit_post_run(&a);
rsxkit_post_pass b = { .program = p2, .out = NULL, .rect = en_pantalla, .inputs = 1 };
b.in[0] = rsxkit_post_surface_input(tabla, RSXKIT_FILTER_LINEAR, RSXKIT_POST_BORDER);
rsxkit_post_run(&b);
```

| Función | Qué hace |
|---|---|
| `rsxkit_surface_create(w, h, format)` / `_destroy` | Superficie lineal en la memoria local: `RSXKIT_POST_RGBA8` (A8R8G8B8) o `RSXKIT_POST_RGBA16F` (4 × fp16). Se dibuja con una pasada y se lee como textura en las siguientes; ninguna puede ser más grande que la profundidad de la pantalla (que va ligada con ella; `rsxkit_depth_reserve`). Hasta 24. `rsxkit_surface_data` da sus píxeles para pruebas (lento). |
| `rsxkit_program_create(fpo)` / `_destroy` | Un .fpo de cgcomp con su propia copia en la memoria del RSX: dos programas del mismo .fpo pueden llevar constantes distintas. Hasta 32. |
| `rsxkit_program_set(p, name, v, count)` | Un `uniform float4` (o `count` elementos de un arreglo). En el RSX las constantes van dentro del microcódigo, una copia en cada instrucción que las lee; se parchea la copia del programa (si el RSX lo está usando, espera). Para valores que cambian poco; lo que cambia en cada dibujo va en `pass.k`. |
| `rsxkit_post_run(pass)` | Dibuja el rectángulo `pass.rect` de la salida (`pass.out`, o la pantalla si es NULL) con el programa y hasta 8 texturas (`pass.in`, unidad = índice). Antes espera en el RSX a que termine lo anterior e invalida la caché de texturas: una pasada puede leer lo que escribió la anterior o un destino recién dibujado. |
| `rsxkit_surface_wrap_main(w, h, format, mem, offset, pitch)` | Superficie sobre memoria principal que ya mapeó quien llama (`rsxkit_map_main`; `mem` alineada a 128, `pitch` múltiplo de 64): el RSX la dibuja y la CPU o un SPE la leen rápido (leer la memoria local desde el Cell es muy lento), o al revés. rsxkit no la libera. |
| `pass.kx`, `pass.ky` | Constantes afines (opcionales): con `kx[j]` o `ky[j]` distintos de 0, TEXCOORD(j + 1) vale `k[j] + kx[j]·(i + 0,5) + ky[j]·(j + 0,5)` en el píxel (i, j) del rectángulo. Lo que es lineal en la pantalla (coordenadas de una textura que se desplaza, un degradado) llega interpolado y el programa no gasta instrucciones en armarlo. En 0, constantes como siempre. |
| `rsxkit_post_clear(s)` | Borra la superficie (todos sus canales a 0), p. ej. antes de sumarle mallas. |
| `rsxkit_post_run_mesh(pass, mesh)` | Dibuja triángulos indexados (índices de 16 bits) de un arreglo de `rsxkit_post_vertex` (posición en el espacio de recorte y TEXCOORD0 con corrección de perspectiva) en memoria principal o local, con el programa, las entradas y la salida de `pass`, sumando (`RSXKIT_BLEND_ADD`: src + dst, también en fp16 y con valores negativos) o no, y descartando caras de frente o de atrás. Sirve para que el rasterizador encuentre los cruces de cada rayo de la pantalla con una superficie (cada triángulo aporta una vez por píxel que cubre) en lugar de buscarlos en un programa por píxel. |
| `rsxkit_post_reduce(src, region, factor, dst)` | Filtro de caja: cada píxel de `dst` promedia un bloque `factor × factor` (1–6) de la región. Para llevar una imagen dibujada a escala interna alta a la resolución de la consola. Por omisión en dos pasadas (a lo ancho sobre una tabla fp16 interna, después a lo alto), para leer la imagen grande por filas seguidas; en una sola pasada (3 × 3 lecturas bilineales por píxel) la lectura de una superficie lineal a factor 5 costaba 3,1 ms en la PS3 para 320 × 237. |
| `rsxkit_post_reduce_passes(n)` | 1 o 2 pasadas para `rsxkit_post_reduce` (para comparar); devuelve lo que había. |
| `rsxkit_post_label_write(i, v)` / `rsxkit_post_label_wait(i, v)` / `rsxkit_label_address(i)` | Etiquetas del RSX para coordinarlo con un SPE o la CPU (las de `RSXKIT_LABEL_USER_MIN` a `_MAX` no las usa rsxkit): el RSX escribe una cuando terminó lo anterior; el RSX espera a que otro escriba un valor; la dirección en la memoria principal, para leerla o escribirla por DMA. |
| `rsxkit_post_surface_input` / `rsxkit_post_texture_input` | Una entrada: superficie intermedia o cualquier `rsxkit_texture` (`rsxkit_target_texture`, `rsxkit_fb_texture`), con filtro, modo fuera de la imagen (`RSXKIT_POST_CLAMP`, `_BORDER` = 0, `_REPEAT`) y coordenadas en texeles. |
| `rsxkit_stamp(i)` / `rsxkit_stamp_ns(i)` | Marca de tiempo del RSX (0 .. 63): el RSX escribe su reloj en ns cuando llega a ese punto y terminó lo anterior; se lee cuando una barrera posterior pasó (`rsxkit_fence_done`). La diferencia entre dos marcas es lo que tardó lo mandado entre ellas. |

Lo que recibe el fragment program:

- `TEXCOORD0`: `xy = origin + (i + 0,5, j + 0,5)·step` en el centro del
  píxel `(i, j)` del rectángulo (con `origin = 0`, `step = 1`: los píxeles
  desde su esquina, con la y hacia abajo) y `zw` normalizadas en el
  rectángulo. El RSX interpola con un error de ~1e-6: si una cuenta
  necesita el centro exacto (empates, `floor` de una posición que puede
  caer en un entero), usar `floor(pos.xy) + 0.5`.
- `TEXCOORD1..7`: las constantes del dibujo (`pass.k`), iguales en todo el
  rectángulo. Llegan como atributos que el vertex program copia: no se
  tocan las constantes del vertex program del 3D ni se parchea nada.
- Texturas en `TEXUNIT0..7`, como las declare el programa.

Con cgcomp, las cuentas en texeles necesitan fp32: compilar con
`-Wcg,-nofastprecision` (cgcomp pide `-fastprecision` a Cg, que baja a
fp16 lo que cree que lo aguanta). Y tres errores de cgcomp que conviene
conocer (detalles en `docs/rsx-notes.md` de Salsa64): no ensambla bien los
códigos de condición (`?:`, `sign`), escribe el color de salida en R0
aunque Cg lo use de temporal, y codifica mal `|R0.z|` (valor absoluto con
un componente).

## Recursos del RSX que usa

- Etiquetas 255 (`rsxkit_finish`), de 254 hacia abajo dos por framebuffer
  (247–254 con 4 framebuffers), 246 (barreras) y 245 (posprocesado). Las de
  0 a 63 son del sistema. Reportes 1984 a 2047 (marcas de tiempo).
- Programas: el blit (`shaders/rsxkit_blit.*`) y el 3D
  (`rsxkit_lincomb.vcg` con 27 constantes desde la 0, salidas COLOR0 y
  TEXCOORD0..7; `rsxkit_lincomb{0,1,2}.fcg` y los generados, en un pool
  de 96 KB de memoria local); unidades de textura 0 y 1; atributos
  POSITION (0), COLOR0 (3) y TEXCOORD0 (8).
- Búfer de comandos: antes de cada operación rsxkit pide el espacio que
  puede usar, para que las funciones de PSL1GHT no cambien de segmento por
  su cuenta (su callback tiene un error que corrompe el FIFO; ver
  `src/fifo.c`). Quien mezcle llamadas propias a PSL1GHT con rsxkit debería
  hacer lo mismo o usar búferes de comandos grandes.
- Unidad de textura que asigna `cgcomp` al sampler del fragment program
  (normalmente la 0) y los atributos de vértice POSITION (0) y TEXCOORD0 (8),
  en modo inmediato.
- Memoria local: 2 búferes de color + 1 de profundidad del tamaño de la
  pantalla (11 MB a 720p, 24 MB a 1080), `2 × max_w × max_h × 4` bytes por
  framebuffer y `w × h × 4` por destino y por profundidad.
- Memoria principal mapeada: 6 MB si se usa `rsxkit_ui_init` (1,9 MB del
  atlas con la caché de glifos, 1,5 MB de vértices y 576 KB por ranura de
  imagen redondeados a 64 KB). La caché de glifos suma ~35 KB de `.bss`.
- Fuente del sistema (`sysfont.h`): 1 MB de caché de archivos, lo que
  libfont pida por sus funciones de memoria (el renderizador, hasta 1 MB) y
  9 KB de superficie; todo se suelta con `rsxkit_sysfont_close`.
- Posprocesado: lo que midan las superficies (`rsxkit_surface_bytes`) y una
  copia de cada programa (memoria local y principal). Vertex program
  `rsxkit_post.vcg` (atributos POSITION y TEXCOORD0..7, en modo inmediato).

## Origen del código

Código propio. Las marcas de tiempo escriben el método GET_REPORT
(0x1800) a mano: `rsxSetTimeStamp` de PSL1GHT (f649a08) deja el dato en la
palabra equivocada y el RSX escribe el reporte en un lugar cualquiera. La
inicialización del RSX sigue el sample `rsxtest` de
PSL1GHT (MIT); el formato de instrucción del fragment program viene de
`nvfx_shader.h` y `compilerfp.cpp` de cgcomp (PSL1GHT, MIT), cotejado con
los programas que cgcomp compila de `shaders/rsxkit_lincomb*.fcg`. Los
glifos de `src/font_8x13.h` son los de la fuente «fixed» 8x13 de X11
(gitlab.freedesktop.org/xorg/font/misc-misc, `8x13.bdf`, de dominio
público). El agrandado de los glifos del atlas (`src/ui_geom.c`) aplica la
regla de Scale2x/EPX, escrita aquí a partir de su descripción pública (no
se copió código). Las ranuras de imagen del 2D (`rsxkit_ui_image_set` y
`rsxkit_ui_image`, para las carátulas del menú de Salsa64) son una
contribución de VIDEOGAMES SCZ, bajo MIT con su conformidad
(2026-10-13). La fuente del sistema (`src/sysfont.c`) se escribió con las
cabeceras de PSL1GHT (`font/font.h`, `font/fontFT.h`, `font/fontset.h`,
MIT); las fuentes del firmware no se copian, se abren en la consola. No
contiene código de proyectos GPL ni de proyectos sin licencia clara.

`rsxkit_screen_texture` presta como textura el búfer recién dibujado sin copiarlo.
Su ticket más reciente debe pasar `rsxkit_screen_texture_valid` tras el flip;
la UI, el redibujo y el sistema invalidan el préstamo. Quien escriba directamente
en la pantalla llama antes a `rsxkit_screen_texture_invalidate`.
