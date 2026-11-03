/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — RDP sobre el RSX: consume la salida de la HLE gráfica
 * (src/rsp/gfx/gfx.h) y la dibuja con lib/rsxkit.
 *
 * Cada color image de la N64 (dirección en la RDRAM) tiene su destino de
 * render en el RSX, a una escala entera de la resolución de la N64 propia
 * de ese destino (3× a 720p para un framebuffer de 320 × 240; ver
 * salsa64_rdp_init); cada z image, su profundidad. Al presentar un campo, si el VI
 * muestra una color image que dibujó el RSX, se presenta ese destino; si
 * no (imagen hecha por el CPU), el frontend usa el framebuffer 2D. Qué
 * destino es ese, y cuál se recicla cuando no caben más, lo decide
 * targets.c (portable, con pruebas en tests/unit/test_targets.c).
 *
 * Los búferes de salida de la HLE (vértices, comandos, texturas) viven en
 * memoria principal mapeada al RSX, en varios juegos que se turnan: el SPE
 * escribe uno mientras el RSX dibuja otro. Una barrera del RSX dice cuándo
 * se puede volver a usar cada juego. La caché de texturas (gfx.h) es otra
 * región mapeada que la HLE administra sola; las barreras le dicen qué
 * tareas ya terminó el RSX (job->done_task).
 *
 * Solo PS3 (PSL1GHT). Dos hilos (docs/hilos-ppe.md): el sink (acquire,
 * done) se llama desde el hilo de emulación; lo demás, desde el hilo que
 * usa rsxkit (presentación). Las tareas terminadas esperan en una cola
 * hasta salsa64_rdp_flush, que las traduce en orden.
 */
#ifndef SALSA64_RDP_RDP_H
#define SALSA64_RDP_RDP_H

#include <rsxkit/rsxkit.h>

#include "core/rcp.h"
#include "rdp/gfxdump.h"
#include "rsp/gfx/backend.h"

struct salsa64_rdp_stats {
	uint32_t tasks;
	uint32_t draws, triangles;
	uint32_t targets_created;
	uint32_t dpc_cont;         /* trabajos que siguen una lista del RDP abierta (GFX_JOB_DPC_CONT): sin envejecer los destinos */
	uint32_t dpc_cont_saved;   /* destinos dibujados que con ese envejecimiento habrían pasado de frescos a cebarse de nuevo */
	uint32_t targets_evicted;  /* destinos reciclados por el tope de memoria */
	uint32_t set_waits;      /* veces que un juego de búferes seguía en uso (emulación) */
	uint32_t tex_waits;      /* modo diagnóstico: esperas antes de entregar una tarea */
	uint64_t us_translate;   /* PPE traduciendo a comandos del RSX (presentación) */
	uint64_t us_set_wait;    /* hilo de emulación esperando un juego */
	uint64_t us_tex_wait;    /* modo diagnóstico: esperando todas las tareas anteriores */
	/* Antigüedad de lo presentado desde el RSX: campos entre el dibujo del
	 * destino y el campo que lo muestra (la latencia propia del juego: 2 con
	 * doble búfer a 30 cps), sumada y la mayor. */
	uint64_t age_sum;
	uint32_t age_max;
	/* Fragment programs generados (lib/rsxkit, rsxkit_fp_stats): dibujos
	 * que usaron uno, programas distintos en la caché, generados en total,
	 * vaciados de la caché y cargas de programa (cambios, contando los
	 * precompilados). */
	uint32_t draws_generated;
	uint32_t fp_programs, fp_generated, fp_evictions, fp_loads;
	/* Dibujos con AA_EN que llevaron la pasada de aristas
	 * (salsa64_rdp_aa_edges). */
	uint32_t draws_edges;
	/* Efectos de framebuffer (docs/framebuffer.md): copias RSX → RDRAM y
	 * su tiempo en el hilo de presentación (esperar al RSX y convertir),
	 * tareas que la emulación esperó por una copia y cuánto, texturas con
	 * origen en un destino leídas de su superficie o de la RDRAM, y
	 * destinos cebados desde la RDRAM. */
	uint32_t readbacks, readback_waits, readback_late;
	uint32_t big8_sync, big8_nosync;  /* copias big8 pedidas con y sin espera */
	uint32_t big8_primes;             /* capas big8 cebadas desde la RDRAM (índices de 8 bits) */
	/* Capas de 8 bits dibujadas en la RDRAM por el PPE (layer8.h): tareas,
	 * dibujos, píxeles, esperas por la copia que se evitaron, destinos que
	 * quedaron para el RSX y el tiempo (timebase). */
	uint32_t layer8_tasks, layer8_draws, layer8_pixels, layer8_waits_avoided, layer8_refused;
	uint64_t tb_layer8;
	/* Por camino: rectángulos 1:1 (entero) y triángulos genéricos. */
	uint32_t layer8_fast_pixels, layer8_slow_pixels;
	uint64_t tb_layer8_fast, tb_layer8_slow;
	/* Imágenes chicas de 32 bits dibujadas en la RDRAM por el PPE (mask32.h):
	 * tareas, dibujos, píxeles, bytes escritos, bytes repuestos tras un
	 * relleno, imágenes que quedaron para el RSX y el tiempo (timebase). */
	uint32_t mask32_tasks, mask32_draws, mask32_pixels, mask32_bytes, mask32_restored, mask32_refused;
	uint64_t tb_mask32;
	uint32_t big8_copies, big8_changed;  /* copias big8 hechas y píxeles de la RDRAM que cambiaron */
	uint64_t us_readback, us_readback_wait;
	uint32_t mirrors, mirrors_skipped;  /* copias espejo (sin esperar) y las que no cupieron */
	uint32_t frames_mirrored;           /* campos cuyo framebuffer se copió a la RDRAM */
	uint32_t dma_skipped;               /* copias descartadas porque un DMA escribió ahí después */
	/* Guarda contra el CPU (rdp_guard, targets.h): líneas de 128 bytes que
	 * una copia tardía no escribió porque algo que no es el RDP las cambió
	 * desde que terminó su tarea, copias con alguna así, y el tiempo de
	 * tomar las huellas al terminar cada tarea (hilo de emulación). */
	uint32_t guard_lines, guard_copies;
	uint64_t tb_guard_mark;  /* ticks del timebase */
	/* Líneas que el espejo del framebuffer no escribió porque otra color
	 * image las dibujó en una tarea posterior (rdp_claim_ring, targets.h),
	 * y espejos con alguna así. */
	uint32_t claim_lines, claim_copies;
	uint32_t tall_fills;                /* rellenos de una color image más alta que 480 repartidos a sus destinos */
	uint32_t tex_rsx, tex_rdram, tex_self, primes, prime_waits;
	/* Alfa en los destinos (salsa64_rdp_target_alpha): destinos creados con
	 * canal alfa, texturas leídas de uno de ellos, y rellenos / copias de
	 * texturas que escribieron su alfa tal cual (rsxkit_draw.dst_alpha). */
	uint32_t targets_alpha, tex_alpha, alpha_fills;
	/* Dibujos de los ciclos 1 y 2 en un destino con alfa por el bit que
	 * dejan (gfx_draw.cvg_bit, GFX_CVG_*: 1, el de la memoria, el del alfa,
	 * el mayor); salsa64_rdp_set_cvg_alpha. */
	uint32_t alpha_cvg[4];
	/* Copias a la RDRAM de un destino con alfa que llevaron su alfa al bit
	 * de RGBA5551 (salsa64_rdp_set_copy_alpha). */
	uint32_t alpha_copies;
	/* Texturas que la HLE no decodificó por venir de un destino
	 * (GFX_TEX_TARGET) y que no se pudieron leer de él: decodificadas en el
	 * PPE desde la RDRAM, y las que no cupieron en el arena (dibujo saltado). */
	uint32_t tex_ppe, tex_ppe_lost;
	/* Copias pedidas que no encontraron su destino dibujado, superficies
	 * que el RSX no pudo crear y color images imposibles ignoradas. */
	uint32_t readbacks_lost, targets_failed, targets_absurd;
	/* Dibujos saltados porque rsxkit no ligó su destino (ninguna
	 * profundidad cubría la superficie; no debería pasar, ver
	 * salsa64_rdp_init). */
	uint32_t binds_failed;
	/* Interpolación por geometría (salsa64_rdp_interp): tareas emparejadas
	 * con la anterior, las que fueron un corte o no cupieron en las tablas,
	 * sus vértices de salida y los que tienen pareja, grupos hallados por el
	 * contenido y el tiempo del PPE (presentación). */
	uint32_t interp_tasks, interp_cuts, interp_full;
	uint32_t interp_cuts_camera;                 /* de ellos, por un giro de golpe de la cámara */
	uint32_t interp_vertices, interp_paired, interp_content;
	uint32_t interp_rejected, interp_reordered;  /* grupos rechazados (saltaron, se deformaron, se aplastarían) y reordenados */
	uint32_t interp_atomic;                      /* grupos con pareja sin interpolar por su objeto o su malla (vmatch_atomic) */
	uint32_t interp_kept;                        /* deformados o aplastados que se interpolan con su objeto */
	uint32_t interp_fbtex;                       /* tareas sin interpolar por leer texturas de un destino */
	uint32_t interp_skipped;                     /* tareas sin GFX_JOB_INTERP con la interpolación encendida (interp_wanted) */
	uint64_t us_interp, us_interp_build, us_interp_pair, us_interp_prev;
	/* Campos intermedios dibujados, los que tocaban pero el cuadro no se
	 * interpola (corte, sin anterior, texturas de un destino: va entero) y
	 * los que no llegaron a tiempo (su juego ya en uso o sin superficie: se
	 * repite el campo anterior), y el tiempo del PPE redibujando. */
	uint32_t interp_fields, interp_plain, interp_repeated;
	uint32_t interp_rep_set, interp_rep_cache, interp_rep_surf;  /* repetidos: su juego ya en uso, la caché, sin superficie */
	uint64_t us_interp_draw;
	/* Profundidad: dibujos que escriben la z sin probarla (en el RSX van con
	 * ALWAYS), borrados de la z (aplicados al momento, dejados pendientes
	 * hasta ligarla, y los que no cupieron en la lista y se juntaron) y
	 * color images en la dirección de una z image (el juego escribe la z
	 * como color: el RSX no la ve en su profundidad). */
	uint32_t z_write_only, zclears, zclears_pending, zclears_merged, z_as_color;
	uint32_t z_uploads;  /* z images cargadas desde la RDRAM (la z del CPU: zfeed en rdp.c) */
	uint32_t z_uploads_partial;  /* de ellas, solo las filas que los dibujos tocaron */
	uint32_t primes_alias;
	uint32_t ppe_copies, ppe_copies_same;  /* de ellos, iguales a la última vez (no se reescriben) */
    /* rectángulos de copia escritos en la RDRAM por el PPE (imágenes de más de 480 filas) */  /* cebados porque la misma memoria se dibujó después con otro ancho */
	uint32_t z_auto;     /* borrados enteros puestos por rdp.c en z images que el RDP no borra enteras (zauto) */
};

/* Después de rsxkit_init. scale = 0 (automática): la mayor escala entera
 * que cabe en la pantalla (720p → 3×, 1080p → 4×); 1 .. 5: la pedida
 * (1× es la imagen nativa de 320 × 240; más que la pantalla es
 * supermuestreo: la presentación la reduce con filtro lineal). Devuelve 0
 * o un código negativo de rsxkit.
 *
 * La escala elegida es un objetivo de líneas (240 × escala: 5× = 1200) y
 * cada destino usa la escala entera que lleva su imagen a ese objetivo sin
 * pasarse (rdp_target_scale): 320 × 240 a 5×, 640 × 480 a 2×, una auxiliar
 * chica a la misma escala que su alto pida (hasta 5×). Cada superficie lleva
 * su escala (surf_scale), y la profundidad la del destino de color con que
 * se usa; rsxkit ya trabaja en coordenadas lógicas (rsxkit_bind). Todo lo
 * que pasa de coordenadas lógicas a píxeles de una superficie usa la escala
 * de esa superficie (rdp_surface_rect, rdp_target_tex_map): la
 * presentación, las copias RSX → RDRAM (reducen a 1× desde la escala del
 * destino), el espejo del framebuffer, el render a textura (un destino
 * leído como textura desde otro de otra escala: u y v van normalizadas) y
 * el scratch (copia del ligado, con su tamaño y su escala).
 *
 * Destinos sin z image propia. El RSX siempre liga una zeta junto al color;
 * un destino que se dibuja sin z image (rectángulos, 2D, el cebado, el
 * scratch) se liga con la profundidad de la pantalla, con la prueba y la
 * escritura apagadas (lo que el juego espera sin z image). A 4×–5× en 720p
 * (o 5× en 1080p) un destino es más grande que la pantalla, y una zeta más
 * chica que el color puede fallar o recortar en el RSX. Se eligió agrandar
 * la profundidad de la pantalla (rsxkit_depth_reserve) al mayor destino
 * posible a la escala, al iniciar y en salsa64_rdp_set_scale, fuera del
 * bucle: es lo más simple (un búfer que ya existía, sin tocar los
 * dibujos) y lo más seguro (el RSX queda siempre con una zeta que cubre al
 * color, la configuración que ya corre en la PS3 a 3× en 720p y a 4× en
 * 1080p). «Sin zeta» no es una opción limpia: la superficie del RSX
 * (gcmSurface) siempre lleva formato, dirección y pitch de zeta, y una
 * zeta de pitch 64 (lo que se usa para los colores sin usar) seguiría
 * siendo más chica que el color, que es justo el riesgo; una profundidad
 * por destino costaría una superficie más por ranura.
 * Cuesta a lo más una superficie del mayor tamaño (7,7 MB a 5×; en 720p,
 * 4 MB más que la de la pantalla), contada en el tope. Además rsxkit_bind
 * se niega a ligar con una zeta más chica (RSXKIT_EDEPTH) y el dibujo se
 * salta (binds_failed): nunca pasa en silencio. Política en targets.c
 * (rdp_zeta_reserve, con pruebas).
 *
 * Política de memoria local del RSX (docs/memory-budget.md):
 * 1) al arrancar el objetivo se baja hasta que el peor caso (8 destinos, el
 * scratch, 4 profundidades y la profundidad de la pantalla agrandada, de
 * 320 × 240 a esa escala; rdp_vram_worst) quepa en RDP_VRAM_BUDGET
 * (112 MiB): a 5× son 102,5 MiB y entra; 2) en el juego, lo vivo suma a lo
 * más RDP_VRAM_BUDGET: para crear una superficie que pasaría se suelta el
 * scratch y se recicla el destino más viejo (no el presentado en el último
 * campo ni uno que una textura pueda estar leyendo); si ni así cabe, o
 * rsxkit no tiene memoria, esa imagen se queda en la copia de la RDRAM y no
 * se dibuja en el RSX, sin errores. */
int salsa64_rdp_init(uint32_t scale);
/* Escala-objetivo efectiva tras salsa64_rdp_init; *reduced: se bajó por memoria. */
uint32_t salsa64_rdp_scale(bool *reduced);
/* Cambia la escala pedida (0: automática) con el RDP ya iniciado y sin juego
 * abierto: suelta los destinos, el scratch y las profundidades, descarta las
 * copias a la RDRAM en vuelo y recalcula la escala-objetivo y la
 * profundidad de la pantalla con la pantalla en curso (después de
 * rsxkit_video_reconfigure). Conserva la memoria mapeada (juegos de
 * búferes, caché de texturas y búferes de las copias). */
void salsa64_rdp_set_scale(uint32_t scale);
void salsa64_rdp_shutdown(void);
/* Entre juegos (menú): suelta los destinos y profundidades del RSX y vacía
 * las colas de tareas y las estadísticas; la memoria mapeada se conserva.
 * Sin backend gráfico conectado (salsa64_rsp_set_gfx(NULL)) y después del
 * último salsa64_rdp_flush. Devuelve las tareas que quedaban sin traducir
 * (0 si se consumió todo). */
uint32_t salsa64_rdp_reset(void);
/* Estados guardados, en la barrera (después del último salsa64_rdp_flush,
 * con la emulación quieta): settle espera las copias RSX → RDRAM que
 * quedaran en vuelo (espejos del framebuffer y de las imágenes chicas,
 * copias grandes sin esperar) y las escribe en la RDRAM, así ninguna llega
 * tarde. drop_targets, al cargar: suelta los destinos, profundidades y el
 * scratch (lo dibujado ya no es de esta máquina y los números de campo
 * retroceden) y olvida las copias y su historial, sin tocar las colas ni la
 * numeración de las tareas, que siguen con el mismo SPE. */
void salsa64_rdp_settle(void);
void salsa64_rdp_drop_targets(void);

/* Efectos de framebuffer (docs/framebuffer.md). La RDRAM emulada, para
 * copiar ahí los destinos que el CPU va a leer y cebar superficies con lo
 * que el CPU dibujó, y cómo despertar al hilo de presentación cuando una
 * tarea necesita esa copia antes de terminar (`done` espera a que esté).
 * Antes de la primera tarea; NULL apaga las copias. */
/* Las copias de las capas de 8 bits (big8) se esperan solo si el juego lo pide
 * (Yoshi's Story: las lee como CI8 desde la RDRAM); por omisión no se esperan. */
void salsa64_rdp_set_big8_wait(bool wait);
/* Las capas de 8 bits (big8) se ceban desde la RDRAM con el índice entero
 * cuando su superficie es nueva o quedó vieja, como las de 16 bits; `false`
 * (bandera gfx-big8-sin-cebar) vuelve a dejarlas con lo que tuviera el RSX. */
void salsa64_rdp_set_big8_prime(bool prime);
/* Las copias a la RDRAM de los destinos con alfa (salsa64_rdp_target_alpha)
 * se leen del RSX en ARGB8888 y dejan el alfa en el bit de RGBA5551, como el
 * RDP; `false` (bandera copia-sin-alfa) vuelve a copiarlas en RGB565 con
 * alfa 1 (LEGO Racers, #34: autos lejanos dentro de un cuadro). */
void salsa64_rdp_set_copy_alpha(bool on);
/* Los dibujos de los ciclos 1 y 2 dejan en un destino con alfa la cobertura
 * del RDP (gfx_draw.cvg_bit: casi siempre 1), no el alfa de la mezcla;
 * `false` (bandera alfa-fragmento) vuelve al alfa del fragmento, como hasta
 * la v0.2.15 (Dr. Mario 64, #62: paneles del fondo en blanco). */
void salsa64_rdp_set_cvg_alpha(bool on);
/* Las capas de 8 bits (big8) las dibuja el PPE en la RDRAM (layer8.h) y no
 * pasan por el RSX ni por su copia; `false` (bandera gfx-capas8-rsx) vuelve
 * a dibujarlas en el RSX y a esperar la copia. */
void salsa64_rdp_set_layer8(bool on);
/* Las imágenes chicas de 32 bits (hasta 64 × 64) y la de 8 bits en su misma
 * memoria las dibuja el PPE en la RDRAM (mask32.h: la luz de los cuartos
 * oscuros de Paper Mario) y no pasan por el RSX; `false` (bandera
 * gfx-mascara32-rsx) las deja en el RSX como antes. */
void salsa64_rdp_set_mask32(bool on);
/* Blindaje del RSX (encendido por omisión; `false` con la bandera
 * rsx-sin-blindaje): una color image que empieza fuera de la RDRAM no tiene
 * destino (rdp_cimg_impossible: Indiana Jones pidió una vez 900000/455 y el
 * RSX se colgó en esa tarea). */
void salsa64_rdp_set_rsx_safe(bool on);
/* Interruptor por juego: cada borrado de z espera al próximo destino que se
 * liga. LEGO Racers emite el borrado antes de cambiar de color image y
 * alterna una auxiliar de 32 x 32 y el framebuffer de 320 x 240 con la misma
 * z image; ejecutarlo sobre el destino anterior lo recorta al pequeño. */
void salsa64_rdp_zclear_target(bool on);
/* Diagnóstico: antes de cada tarea, espera mediante sondeo a que el RSX
 * termine todas las anteriores. No emite órdenes desde el hilo productor. */
void salsa64_rdp_set_texture_wait(bool wait);
void salsa64_rdp_set_memory(uint8_t *rdram, uint32_t rdram_size, void (*wake)(void));
/* Hilo de emulación, al terminar cada campo con imagen: geometría del VI,
 * para distinguir framebuffers de imágenes auxiliares y reservar una color
 * image que cubra toda su ventana. */
void salsa64_rdp_vi(const struct salsa64_vi_frame *f);
/* Interruptor: 0 apaga los efectos de framebuffer (las texturas que
 * vienen de un destino se leen de la RDRAM como antes y nada se copia), 1
 * solo render a textura, 2 también las copias a la RDRAM (por omisión). */
void salsa64_rdp_fbfx(int level);
/* Interpolación de cuadros por geometría (docs/interpolacion.md). Con un
 * modo distinto de OFF, las tareas que se envían piden a la HLE la
 * identidad de sus vértices (GFX_JOB_INTERP) y al traducir cada una el PPE
 * empareja sus vértices con los de la anterior (vmatch.c) y deja sus
 * posiciones de allá junto a los vértices. FIELDS: en los campos en que el
 * VI repite un cuadro de 2 a 4 campos, se presenta esa tarea redibujada con
 * las posiciones entre el cuadro anterior y ese (salsa64_rdp_present).
 * FIXED (para probar): cada tarea se dibuja en prev + (pos − prev)·t, sin
 * campos intermedios. Apagada no cambia nada ni reserva memoria; la primera
 * vez que se enciende reserva las tablas (~2,4 MB) y, con FIELDS, la
 * superficie de los intermedios se crea al primero (cuenta en el tope de
 * memoria del RSX). Fuera del bucle o con la emulación en pausa; devuelve 0
 * o RSXKIT_ENOMEM. */
enum salsa64_interp { SALSA64_INTERP_OFF, SALSA64_INTERP_FIELDS, SALSA64_INTERP_FIXED };
int salsa64_rdp_interp(int mode, float t);
/* Suelta las tablas de la interpolación (al cerrar el juego; interp_apply las
 * vuelve a pedir al abrir el siguiente si sigue encendida) y dice cuánto
 * tienen reservado (0 si no hay). */
void salsa64_rdp_interp_release(void);
size_t salsa64_rdp_interp_bytes(void);
/* Interruptor: en los dibujos opacos con AA_EN, las aristas como líneas
 * antes de los triángulos (rsxkit_draw.edges): tapan las grietas de las
 * uniones en T que el RDP tapa con la cobertura. Apagado por omisión: en
 * la PS3 a escala alta engordaba los modelos (docs/rsx-notes.md). */
void salsa64_rdp_aa_edges(bool on);
/* Interruptor: el RSX interpola el sombreado de los vértices (color, alfa y
 * con ellos el factor de niebla) lineal en pantalla, como el RDP de la N64
 * (rsxkit_affine_shade); va con el recorte de la HLE contra la caja del
 * clip ratio (GFX_JOB_NO_RSP_CLIP lo apaga). Encendido por omisión. */
void salsa64_rdp_affine_shade(bool on);
/* Interruptor: los destinos de 16 bits que no son un framebuffer del VI
 * (render a textura) se crean con canal alfa (A8R8G8B8) y llevan el alfa de
 * la N64: 0 al borrarse con 0x0000, 1 en cada píxel dibujado (el alfa del
 * relleno o del texel en los rellenos y copias). Lo dibujado con alfa 0 y
 * leído después como textura sale transparente, no opaco (Mario Tennis, los
 * logos de Rogue Squadron). Encendido por omisión; `on` false vuelve a los
 * destinos X8R8G8B8 de antes. Aplica a los destinos que se creen después. */
void salsa64_rdp_target_alpha(bool on);
/* Guarda de las copias tardías contra lo que escribió el CPU (rdp_guard,
 * targets.h): encendida por omisión; `on` false la apaga (para comparar).
 * Vigilante: si watch_hi > watch_lo, el log dice cada escritura que no es
 * del CPU (copias del RSX, VISCVG, DMA) en la RDRAM [watch_lo, watch_hi), y
 * si dejó la palabra 0x00010001. Fuera del bucle. */
void salsa64_rdp_guard(bool on, uint32_t watch_lo, uint32_t watch_hi);
/* El espejo del framebuffer no escribe las líneas que otra color image
 * dibujó en una tarea posterior a la suya (rdp_claim_ring, targets.h; FIFA 99
 * al acabar el partido): encendido por omisión; `on` false vuelve a como
 * antes (solo la guarda). Fuera del bucle. */
void salsa64_rdp_claims(bool on);
/* Hilo de emulación: un DMA escribió la RDRAM [pa, pa + len); con
 * `gfx_in_flight`, mientras corría una tarea gráfica. Las copias de esa
 * tarea y de las anteriores no pisan ese tramo (targets.h, rdp_dma_ring). */
void salsa64_rdp_rdram_written(uint32_t pa, uint32_t len, bool gfx_in_flight);
/* Lo que eligió la última salsa64_rdp_present. Es una consulta del resultado
 * ya elegido: no vuelve a recorrer los destinos ni cambia su historial. */
struct salsa64_rdp_present_info {
	int slot;                 /* ranura del destino */
	uint32_t addr, width, height, scale;
	uint32_t y0, w, h, age;  /* ventana lógica y antigüedad */
	uint32_t src_x, src_y, src_w, src_h;  /* región final de la superficie */
};
bool salsa64_rdp_last_present(struct salsa64_rdp_present_info *out);

/* Avisos y la traza de las copias (los escribe el frontend en su log). */
void salsa64_rdp_set_log(void (*fn)(const char *fmt, ...));

/* Las dos manos de un backend gráfico (rsp/gfx/backend.h). acquire puede
 * esperar a que el RSX libere un juego de búferes (nunca llama a rsxkit
 * salvo rsxkit_fence_done). done puede esperar a que el hilo de
 * presentación copie un destino a la RDRAM (ver salsa64_rdp_set_memory). */
const struct salsa64_gfx_sink *salsa64_rdp_sink(void);

/* Traduce a comandos del RSX las tareas terminadas con número menor que
 * `upto` (~0u: todas) y, siempre, la que la emulación espera en done por
 * una copia a la RDRAM. Devuelve cuántas van traducidas en total. */
uint32_t salsa64_rdp_flush(uint32_t upto);
/* Tareas terminadas (done) hasta ahora; para el hilo de emulación, que lo
 * manda con cada campo. */
uint32_t salsa64_rdp_tasks_done(void);

/* Recorte del overscan al presentar, en píxeles de la imagen del VI por
 * cada lado (0: sin recorte). */
struct salsa64_overscan {
	uint32_t left, right, top, bottom;
};

/* Si el VI muestra una color image dibujada por el RSX en los últimos
 * campos, la presenta (en el cuadro en curso de rsxkit), sin los bordes de
 * `crop` (NULL: entera), y devuelve true. */
bool salsa64_rdp_present(const struct salsa64_vi_frame *f, uint32_t field, const rsxkit_view *view,
			 const struct salsa64_overscan *crop, rsxkit_rect *out);
/* Con `draw` en false, salsa64_rdp_present elige qué se ve (y hace todo lo
 * demás) pero no lo dibuja: lo dibuja quien llama, con un filtro (desde
 * salsa64_rdp_present_source) o sin él (salsa64_rdp_present_again). */
void salsa64_rdp_present_draw(bool draw);
/* Lo último que eligió salsa64_rdp_present como textura (el destino
 * entero), la región que se ve (en píxeles del destino, sin el overscan) y
 * la escala de ese destino; con `view` o `crop`, antes los aplica como
 * salsa64_rdp_present_again (en la pausa). En `gen` (si no es NULL), un
 * número que cambia cada vez que algo escribe en ese destino: si es el
 * mismo de antes con el mismo destino, la imagen no cambió. false si no
 * hay. */
bool salsa64_rdp_present_source(const rsxkit_view *view, const struct salsa64_overscan *crop, rsxkit_texture *tex,
				rsxkit_rect *region, uint32_t *scale, uint32_t *gen);
/* Vuelve a dibujar lo último que presentó salsa64_rdp_present (mismo
 * destino, región y escala), sin tocar la tabla de destinos ni las copias a
 * la RDRAM: para seguir mostrando el último cuadro con la emulación en
 * pausa. Con `view` o `crop` (si no, NULL), con esa vista y ese recorte (el
 * menú de pausa los cambia en vivo; las dos cosas se piden juntas: un NULL
 * es «sin recorte» o «la vista por omisión»). false si no hay (nada
 * presentado, o el destino se soltó). */
bool salsa64_rdp_present_again(const rsxkit_view *view, const struct salsa64_overscan *crop, rsxkit_rect *out);

void salsa64_rdp_get_stats(struct salsa64_rdp_stats *out);
/* Solo lo que esperó la emulación (por un juego de búferes y por copias a la
 * RDRAM), en µs desde el arranque: barato, para cada campo. */
void salsa64_rdp_get_waits(uint64_t *set_wait_us, uint64_t *readback_wait_us);

/* Tiempo del hilo que traduce (presentación), en ticks del timebase: la
 * traducción entera (con lo que sigue cuando cae dentro de ella), las copias
 * que convierte el PPE hacia la RDRAM (copias, espejos, cebado, VISCVG), las
 * texturas decodificadas en el PPE y las esperas a una barrera del RSX
 * (dormido). Lo mide docs/hilos-ppe.md, «Cuánto le quita el hilo 1». */
struct salsa64_rdp_tb {
	uint64_t translate, copy, tex, fence;
};
void salsa64_rdp_get_tb(struct salsa64_rdp_tb *out);

/* Perfilado (docs/perfilado.md): después de traducir cada tarea (otro gancho que el de la caja negra), en el hilo
 * que traduce (presentación). El volcado de esa tarea se puede pedir desde
 * el gancho (salsa64_rdp_dump: es la última). */
#define SALSA64_RDP_TASK_SIGS 4
struct salsa64_rdp_task_info {
	uint32_t task;                 /* número de tarea (job->task) */
	const struct gfx_result *res;  /* lo que dio la HLE */
	uint32_t us_translate;         /* traducirla al RSX */
	uint32_t us_set_wait;          /* lo que esperó la emulación su juego de búferes */
	uint32_t approx_draws;         /* dibujos con el combiner aproximado (gfx_draw.approx) */
	uint32_t nsig;                 /* huellas distintas de esos dibujos (términos y factores), hasta SALSA64_RDP_TASK_SIGS */
	uint32_t sig[SALSA64_RDP_TASK_SIGS];
	uint32_t fp_loads;             /* cambios de fragment program */
	uint32_t readbacks;            /* copias a la RDRAM hechas al traducirla */
};
void salsa64_rdp_set_perf_hook(void (*hook)(const struct salsa64_rdp_task_info *info));
/* Gancho de la caja negra (rdp/blackbox.h): se llama desde el hilo de
 * presentación al empezar a traducir cada tarea, con su salida tal como la
 * dejó la HLE (el volcado se arma con gfx_dump_build; las texturas de la
 * caché siguen en su lugar hasta que el RSX termine la tarea). Con NULL, no
 * cuesta nada. */
typedef void (*salsa64_rdp_task_hook)(void *ctx, uint32_t task, const struct gfx_dump_src *src,
				      const struct gfx_result *res);
void salsa64_rdp_set_task_hook(salsa64_rdp_task_hook fn, void *ctx);
/* Lo que fue el último campo de salsa64_rdp_present con la interpolación
 * por geometría, para la caja negra: kind SALSA64_FIELD_* (el cuadro de
 * verdad, un intermedio dibujado o uno repetido porque no llegó), la t y la
 * k del intermedio, la tarea redibujada y la anterior con que se emparejó
 * (numeración del gancho). */
enum { SALSA64_FIELD_TRUE, SALSA64_FIELD_INTERP, SALSA64_FIELD_REPEAT };
struct salsa64_rdp_field_info {
	uint32_t kind;
	float t;
	uint32_t k, task, prev_task;
};
void salsa64_rdp_field_info(struct salsa64_rdp_field_info *out);
/* Tareas traducidas hasta ahora (la numeración de las tareas del gancho). */
uint32_t salsa64_rdp_translated(void);
/* La tarea que se está traduciendo (n + 1; 0 si ninguna) y en `stage` la
 * etapa («dibujos», «destinos», «copias a la RDRAM»…). Para el latido:
 * se puede leer desde otro hilo (lectura suelta, solo diagnóstico). */
uint32_t salsa64_rdp_translating(const char **stage);

/* Escribe la salida de la última tarea (gfx.h: gfx_dump_header) en `path`,
 * con las texturas de la caché copiadas al arena (el volcado no depende de
 * la caché). Espera al RSX. Para depurar. */
bool salsa64_rdp_dump(const char *path);
/* Lo mismo con la tarea que se está traduciendo (la de un RSX colgado), o la
 * última si no hay ninguna; *task: su número. No espera al RSX ni lo toca:
 * se puede llamar desde el gancho de las esperas o desde otro hilo con la
 * traducción quieta. */
bool salsa64_rdp_dump_current(const char *path, uint32_t *task);
/* Al log, para un RSX colgado: los últimos destinos creados o reciclados,
 * profundidades y cargas de la z (con su tarea), lo ligado y las tablas de
 * destinos y profundidades. Solo lee. */
void salsa64_rdp_diag_log(void (*log)(const char *fmt, ...));

#endif
