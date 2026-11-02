/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — frontend mínimo para PS3.
 *
 * Carga una ROM, la arranca por HLE y la emula en dos hilos del PPE
 * (docs/hilos-ppe.md):
 *
 *   emulación     (emu_thread, prioridad 1000): corre el CPU (recompilador,
 *                 o el intérprete en RPCS3 o con el archivo SALSA64_NOREC_FLAG;
 *                 ver platform/ps3/rec_cache.h) hasta el fin de cada campo del
 *                 VI, aplica los controles al PIF, copia
 *                 la imagen del VI y encola el campo; las tareas gráficas del
 *                 RSP van por HLE al SPE 0 (src/platform/ps3/gfx_spu.c) y su
 *                 salida queda en la cola de src/rdp.
 *   presentación  (el hilo principal, prioridad 1001): traduce la salida de
 *                 la HLE a comandos del RSX (salsa64_rdp_flush) y presenta
 *                 cada campo con lib/rsxkit: si lo dibujó el RSX, ese destino
 *                 de render; si no, la copia del framebuffer de la RDRAM como
 *                 textura, escalada a 4:3. Lee los DualShock 3, vuelca el
 *                 guardado, escribe el log de estadísticas, capturas y
 *                 volcados, y decide la salida.
 *
 * Si la emulación va más rápida que la tele se presenta el campo más nuevo y
 * los demás se saltan: la emulación nunca espera al vsync. Lo que la frena
 * es el ritmo de la tele emulada (pacer.c: cada campo a su hora, sin
 * acumular deuda); con SALSA64_DATA_DIR/turbo presente corre al máximo
 * (para medir). La medición A/B de vitest corre siempre al máximo.
 *
 * ROM: si existe SALSA64_DATA_DIR/embedded, se arranca directo lo que diga
 * (las pruebas automáticas dependen de esto): "vitest", "gfxtest" o
 * "audiotest", esa ROM de prueba embebida; "file:<nombre>", ese archivo de
 * SALSA64_DATA_DIR (para probar una ROM en RPCS3 sin pisar el rom.z64 que
 * leen otras compilaciones; las variantes de gfxtest, salsa64-gfxtest-*.z64,
 * se verifican como la embebida); "dump:<nombre>", la repetición de un
 * volcado de la HLE en el RSX (replay.c); otra cosa,
 * SALSA64_DATA_DIR/rom.z64 (cualquiera de los tres órdenes de bytes) si
 * existe y si no gfxtest. START + SELECT sale al XMB.
 *
 * Sin embedded, el menú (menu.c, romlist.c), con una disposición inspirada
 * en el XMB y dibujado entero por el RSX (rsxkit/ui.h): los .z64/.v64/.n64 y los .zip compatibles
 * de SALSA64_DATA_DIR y de /dev_usbNNN/salsa64 en una columna, con carpetas
 * primero y navegación por subcarpetas; las ROMs se ordenan por título o
	* nombre de archivo (menu.cfg). Admite 4096 filas totales por carpeta
	* relativa entre todas las unidades, contando ROMs y subcarpetas. Un panel de
 * información muestra archivo, código, región, tipo de
 * guardado) y ondas de fondo; en Opciones se apagan las ondas (se guarda
 * en SALSA64_DATA_DIR/menu.cfg) y se elige el recorte del overscan (se
 * escribe overscan.cfg, que run_game relee en cada juego) y la resolución de
 * salida (video.cfg; vale al volver a abrir Salsa64). Cruceta para elegir, X para abrir, triángulo
 * para Opciones; la última ROM abierta queda en SALSA64_DATA_DIR/ultima-rom
 * y es la elegida al volver. Dentro del juego, START + SELECT vuelve al menú: se
 * cierra todo lo del juego (guardado al disco, audio, HLE gráfica y de
 * audio, SPEs, recompilador, destinos del RSX, colas entre hilos, ritmo;
 * cada paso al log, game_close) y se puede abrir otro. «Salir al XMB» está
 * al final de la lista. El menú reserva al arrancar un búfer para la ROM
 * más grande (64 MB) y los 2 MB mapeados del 2D; nada más se asigna al
 * cambiar de juego salvo los bloques de control de los SPEs. Con
 * SALSA64_DATA_DIR/capturas (o en prueba automática) deja una captura del
 * menú en SALSA64_DATA_DIR/menu.ppm.
 *
 * Con las ROMs de prueba el frontend se verifica a sí mismo (selftest.c,
 * selftest_gfx.c) e imprime "RESULTADO frontend: OK/FALLO"; en modo de
 * prueba automática (platform/ps3/autotest.h) sale después. En prueba
 * automática sin embedded el menú se maneja solo: abre en orden las ROMs
 * que nombra SALSA64_DATA_DIR/menu-prueba (una por línea; las de prueba se
 * verifican como las embebidas), vuelve al menú después de cada una y sale
 * con "RESULTADO frontend" del conjunto (tools/rpcs3-run.py --menu). Una
 * línea «@video 720|1080 escala» aplica la salida y la escala como «Aplicar
 * ahora», entre juegos.
 * Con vitest y fuera de la prueba automática, después mide 10 s sin
 * detección de bucles de espera y 10 s con ella ("MEDICIÓN …" en el log)
 * y sigue.
 *
 * Controles: los DualShock 3 de los puertos 1–4 son los controles de la N64
 * (padmap.c; se cambian con SALSA64_DATA_DIR/controles.cfg). El puerto 1
 * siempre tiene un control conectado (neutro sin mando): el DS3 tarda unos
 * cuadros en aparecer y los juegos buscan controles al arrancar. Los cambios
 * de botones van al log.
 *
 * Estados guardados (states.c, core/state.h, docs/estados.md): diez
 * ranuras por juego en SALSA64_DATA_DIR/states. SELECT + R1 guarda en la
 * ranura actual, SELECT + L1 carga, SELECT + izquierda/derecha cambia de
 * ranura; mientras SELECT está apretado esos botones no llegan al juego.
 * Se hacen en el límite de un campo con la emulación quieta: el hilo de
 * emulación recoge la tarea del RSP en vuelo y espera; el de presentación
 * vacía las colas, espera al RSX y guarda o carga (state_barrier). Un aviso
 * corto en pantalla (osd.c) dice qué pasó. SELECT + □ abre la lista de
 * trucos del juego con la emulación en pausa (× activa o desactiva, ○ cierra;
 * lo activo se guarda junto al .cht). SELECT + △ muestra u oculta el
 * contador de velocidad (VI/s y %, cuadros del juego y de la pantalla), que
 * se recuerda en menu.cfg.
 *
 * Guardado del cartucho (EEPROM, SRAM, FlashRAM; savefile.c): tipo por
 * SALSA64_DATA_DIR/guardado.cfg, la cabecera de homebrew o la tabla por
 * código de juego; un archivo por juego en SALSA64_DATA_DIR/saves, escrito
 * unos segundos después de la última escritura del juego y al salir.
 *
 * HLE gráfica: en el SPE 0 (lib/spukit); con SALSA64_DATA_DIR/gfx_ppe
 * presente, en el PPE (para comparar tiempos).
 *
 * Audio (audio_out.c): HLE de las tareas de audio en el SPE 1 y lo que sale
 * por el AI al puerto de audio, desde un hilo propio; se configura con
 * SALSA64_DATA_DIR/audio.cfg. El ritmo lo marca pacer.c (la tele
 * emulada); el audio lo sigue con su corrección fina (audio_sync.h). Con
 * "audiotest" en embedded, la ROM de prueba del audio se verifica sola
 * (selftest_audio.c) y sigue sonando.
 *
 * Presentación: la imagen del VI sin el overscan (SALSA64_DATA_DIR/
 * overscan.cfg: píxeles por lado a 320 × 240; por omisión 8 y 6, 0 la
 * apaga), escalada a 4:3. Qué destino del RSX se presenta lo decide
 * src/rdp/targets.c.
 *
 * Capturas: con una ROM del disco y SALSA64_DATA_DIR/capturas presente,
 * cada SHOT_US se guarda la pantalla en SALSA64_DATA_DIR/screen-N.ppm (N de
 * 1 a SHOT_MAX, en ronda), para verla desde la Mac por FTP. Cada captura
 * frena la presentación ~400 ms (leer el búfer del RSX desde el PPE es
 * lento): se nota como un tirón, por eso es opcional. En modo de prueba
 * automática se captura siempre, cada AUTO_SHOT_FIELDS campos, y se sale
 * tras AUTO_FIELDS (para correr una ROM en RPCS3 sin manos). Si capturas
 * dice un número N, se captura en cambio cada N campos exactos
 * (campo-<campo>.ppm; esos campos no se saltan): la emulación es
 * determinista, así que se comparan entre compilaciones; un segundo número
 * M hace salir en el campo M (mediciones sin manos).
 *
 * Medición de la HLE gráfica: además de stats() cada STATS_US, el hilo de
 * emulación resume cada GFX_WIN_FIELDS campos ("gfx campos A–B": campos/s,
 * el campo que más tardó en emularse y el trabajo del SPE). Mismas
 * ventanas, mismas escenas en todas las corridas.
 *
 * Si SALSA64_DATA_DIR/gfxdump dice un número de campo,
 * en ese campo se vuelca la salida de la última tarea gráfica a
 * SALSA64_DATA_DIR/gfxdump.bin (salsa64-run --raster-dump la dibuja).
 * SELECT + R2 en el juego hace lo mismo en el campo que se está mostrando,
 * a gfxdump-<campo>.bin, con la captura del RSX de ese cuadro al lado
 * (gfxdump-<campo>.ppm): para comparar el RSX con el rasterizador del host.
 *
 * Salir: «Salir al XMB» en el menú, START + SELECT sin menú (con embedded),
 * o "Salir del juego" desde el menú del sistema en cualquier momento.
 * START + SELECT pregunta antes (cuadro «¿Volver al menú?» o «¿Salir al
 * XMB?», × sí, ○ no; «confirmar = no» en menu.cfg sale directo). Con el
 * menú del sistema abierto (botón PS) o el cuadro a la vista, la emulación
 * y el audio quedan en pausa y se sigue presentando el último cuadro
 * (pausectl.h, docs/hilos-ppe.md).
 * Lo que la ROM imprime por el ISViewer va al log.
 */
#include <errno.h>
#include <io/pad.h>
#include <lv2/process.h>
#include <malloc.h>
#include <ppu-lv2.h>
#include <ppu-types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/memory.h>
#include <sys/process.h>
#include <sys/sem.h>
#include <sys/stat.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <sysutil/sysutil.h>
#include <sysutil/video.h>

#include <rsxkit/post.h>
#include <rsxkit/rsxkit.h>
#include <rsxkit/sysfont.h>
#include <rsxkit/ui.h>
#include <spukit/spukit.h>

#include "core/rompg.h"
#include "core/boot.h"
#include "core/endian.h"
#include "core/gamefix.h"
#include "core/faultlog.h"
#include "cpu/rec.h"
#include "audio_out.h"
#include "audiotest_rom_bin.h"
#include "cheats.h"
#include "cover.h"
#include "download.h"
#include "gfxtest_rom_bin.h"
#include "i18n.h"
#include "intro.h"
#include "menu.h"
#include "osd.h"
#include "pacer.h"
#include "perfilado_ps3.h"
#include "padmap.h"
#include "pausectl.h"
#include "platform/ps3/autotest.h"
#include "platform/ps3/fastmem.h"
#include "platform/ps3/gfx_spu.h"
#include "platform/ps3/jpeg_spu.h"
#include "platform/ps3/rsp_spu.h"
#include "platform/ps3/log.h"
#include "platform/ps3/rec_cache.h"
#include "platform/ps3/thread_cleanup.h"
#include "ra.h"
#include "frontend/filters/filters.h"
#include "frontend/seda.h"
#include "rdp/rdp.h"
#include "rsp/gfx/hle.h"
#include "replay.h"
#include "ra_rules.h"
#include "cuelgue.h"
#include "romlist.h"
#include "ziprom.h"
#include "savefile.h"
#include "scale_auto.h"
#include "mempakfile.h"
#include "transferpakfile.h"
#include "screenshot.h"
#include "selftest.h"
#include "selftest_audio.h"
#include "selftest_gfx.h"
#include "states.h"
#include "cajanegra.h"
#include "rdp/blackbox.h"
#include "version.h"
#include "vitest_rom_bin.h"
#include "cart_icon_bin.h"
#include "cart_mesh_bin.h"
#include "cart_plastic_bin.h"
#include "cart_label_bin.h"
#include "frontend/cartucho3d.h"
#include "frontend/thumbs.h"
#include "frontend/thumbs_calc.h"
#include "prompts_bin.h"
#include "prompts.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define ROM_PATH     SALSA64_DATA_DIR "/rom.z64"
#define PADS_PATH    SALSA64_DATA_DIR "/controles.cfg"
#define SAVES_DIR    SALSA64_DATA_DIR "/saves"
#define SAVE_CFG     SALSA64_DATA_DIR "/guardado.cfg"
#define PADS_CFG_MAX 4096
#define EMBED_PATH   SALSA64_DATA_DIR "/embedded"
#define GFX_PPE_PATH SALSA64_DATA_DIR "/gfx_ppe"
#define TURBO_PATH   SALSA64_DATA_DIR "/turbo"
#define LLE_PATH     SALSA64_DATA_DIR "/lle"
#define SHOTS_PATH   SALSA64_DATA_DIR "/capturas"
#define LAST_ROM_PATH SALSA64_DATA_DIR "/ultima-rom"
#define MENU_TEST_PATH SALSA64_DATA_DIR "/menu-prueba"
#define MENU_CFG_PATH SALSA64_DATA_DIR "/menu.cfg"
#define CHEATS_DIR    SALSA64_DATA_DIR "/cheats"
#define COVERS_DIR    SALSA64_DATA_DIR "/covers"
#define GAMES_DIR     SALSA64_DATA_DIR "/juegos"
#define MENU_TEST_MAX 8
#define MENU_IDLE_FIELDS 900  /* prueba automática: sin acción en tanto, falla */
#define OVERSCAN_PATH SALSA64_DATA_DIR "/overscan.cfg"
#define VIDEO_CFG_PATH SALSA64_DATA_DIR "/video.cfg"
#define SCALE_AUTO_OFF_PATH SALSA64_DATA_DIR "/sin-escala-auto"
/* Diagnóstico: con este archivo, el menú del sistema no pausa la emulación
 * (como hasta v0.1.0); los mandos siguen sueltos y el latido mira. */
#define NO_PAUSE_PATH SALSA64_DATA_DIR "/sin-pausa"
#define JPEG_PPE_PATH SALSA64_DATA_DIR "/jpeg-ppe"  /* diagnóstico: la tarea JPEG en el PPE */
/* Prueba del seguro del RSX (rsx_hang_test): «etiqueta N», «rsx N» o
 * «fifo N» cuelga a propósito al presentar el campo N. */
#define HANG_TEST_PATH SALSA64_DATA_DIR "/colgar-rsx"
/* Prueba de la interpolación por geometría (SALSA64_INTERP_FIXED): con el
 * archivo, cada tarea se empareja con la anterior y se dibuja en la fracción
 * «t» de su contenido (por omisión 1: igual que sin interpolar; 0: un cuadro
 * atrás), sin los campos intermedios, pase lo que pase con la opción. */
#define INTERP_TEST_PATH SALSA64_DATA_DIR "/interp-prueba"
/* Prueba de la interpolación cambiada en vivo: «N [E1 E2]» en el archivo,
 * cada N campos se pausa la emulación como desde el menú de pausa, se
 * enciende o apaga la interpolación por geometría (y, con E1 y E2, la escala
 * interna pasa de una a otra, aplicada al seguir) y sigue medio segundo
 * después (tools/rpcs3-run.py --bandera). */
#define INTERP_ALT_PATH SALSA64_DATA_DIR "/interp-alternar"
#define INTERP_ALT_HOLD_US 500000u
/* Filtro de la presentación (frontend/filters): el de menu.cfg («filtro =
 * …»). Con el archivo `filtro` (una clave, p. ej. «crt-mascara»), ese,
 * también en las pruebas automáticas; con `filtros-ciclo`, todos por turnos
 * cada FILTER_CYCLE_US, para medirlos (el log dice cuánto tarda cada uno en
 * el RSX; cada vuelta cambia la reducción entre dos pasadas y una, si no
 * la fija `filtros-reducir`); con `filtros-capturas`, la primera vez de cada
 * filtro guarda la imagen que entra y la que sale (filtro-<clave>-<alto>-
 * fuente.ppm y -pantalla.ppm); con `filtros-reducir` («1» o «2»), la
 * reducción en esas pasadas; con `filtros-spe`, la señal y la
 * decodificación de S-Video y compuesto en dos SPEs (con el ciclo, cada
 * vuelta las alterna entre los SPEs y el RSX); con `capturas-pantalla`, las capturas
 * periódicas guardan además la pantalla entera como se ve (con el filtro)
 * en pantalla-N.ppm. */
#define FILTER_PATH        SALSA64_DATA_DIR "/filtro"
#define FILTER_CYCLE_PATH  SALSA64_DATA_DIR "/filtros-ciclo"
#define FILTER_SHOTS_PATH  SALSA64_DATA_DIR "/filtros-capturas"
#define FILTER_REDUCE_PATH SALSA64_DATA_DIR "/filtros-reducir"
#define FILTER_SPE_PATH    SALSA64_DATA_DIR "/filtros-spe"
#define FILTER_CYCLE_US    20000000u
/* Preparar un filtro que tarde más que esto (ms) se avisa en pantalla. */
#define FILTER_NOTICE_MS   33u
/* Con un CRT la escala interna no pasa de esta (lo elegido menor se
 * respeta): el filtro ve la imagen a la resolución de la consola y más
 * escala solo le llega como supermuestreo; a 3× ya no se nota y a 5× el
 * RSX no alcanza a 1080p (docs/perf-log.md, «Filtros CRT en el RSX»). */
#define CRT_SCALE_MAX      3
#define SHOTS_FULL_PATH   SALSA64_DATA_DIR "/capturas-pantalla"
#define FILTER_VIEW_PATH  SALSA64_DATA_DIR "/filtro-ver"
/* Con este archivo, la RDRAM emulada sale de sysMemoryAllocate con páginas
 * de 1 MB en vez del heap (memalign): para medir si el PPE pierde tiempo en
 * fallos de TLB (docs/perf-log.md). El tamaño de página real de cada zona
 * se anota en el log al arrancar (sys_memory_get_page_attribute). */
#define RDRAM_1M_PATH SALSA64_DATA_DIR "/rdram-1m"
/* Perfil persistente del recompilador (cpu/rec_profile->h): un archivo por
 * juego en PROFILE_DIR con cuántas veces entra la ejecución a cada bloque;
 * en la sesión siguiente los bloques calientes se compilan juntos. Apagado
 * por omisión (medido en la PS3 el 2026-10-12: contar cuesta ~5 % y juntar
 * los calientes no gana nada; docs/perf-log.md): el archivo `perfil-activo`
 * lo enciende, y entonces se reserva la tabla y el código generado cuenta;
 * `perfil.cfg` (`caliente = KB`, `saltos = si|no`) dimensiona la zona
 * caliente y el registro de sitios (docs/uso-ps3.md, docs/rec-ppe.md). */
#define PROFILE_DIR      SALSA64_DATA_DIR "/perfiles"
#define PROFILE_ON_PATH  SALSA64_DATA_DIR "/perfil-activo"
#define PROFILE_CFG_PATH SALSA64_DATA_DIR "/perfil.cfg"
#define PROFILE_HOT_KB   1024u            /* zona caliente por omisión */
#define PROFILE_SAVE_US  120000000ull     /* escritura periódica, además de la del cierre */
#define PROFILE_FOLD_FIELDS 60            /* campos entre sumas de rec->hits[] al perfil */
#define PROFILE_STEP_ENTRIES 512          /* entradas escritas por vuelta, si escribe el hilo de presentación */
#define PROFILE_IO_PRIO  1500             /* el hilo que escribe el perfil: por debajo de todos */
#define PROFILE_IO_STACK (64u << 10)
#define ROM_MAX      (64u << 20)          /* la ROM que cabe entera en memoria; más grande, por fragmentos */
#define ROM_FILE_MAX 0x0FC00000u          /* 252 MB: todo el espacio del cartucho (0x10000000–0x1FBFFFFF) */
#define ROM_PAGE_SHIFT 18u                /* páginas de 256 KB */
#define ROM_CACHE_MB 32u                  /* caché de páginas por omisión */
#define ROM_HEAD     4096u                /* la cabecera y el IPL3, siempre en memoria */
#define ROM_PAGED_FLAG SALSA64_DATA_DIR "/rom-paginada"  /* existe: aun las ROMs pequeñas van por fragmentos */
#define ROM_CACHE_CFG  SALSA64_DATA_DIR "/rom-cache"     /* MB de la caché (1 a 128), una cifra */
#define ROM_SYNC_FLAG  SALSA64_DATA_DIR "/rom-lectura-sincrona"  /* existe: sin hilo lector, cada fallo espera al disco */
#define ROM_DEPTH      8u                 /* páginas que se piden por adelantado en segundo plano */
#define FB_MAX_W     640
#define FB_MAX_H     576
#define STATS_US     5000000u
#define BENCH_US     10000000u
#define SHOT_US      10000000u
#define SHOT_MAX     9
#define AUTO_SHOT_FIELDS 300
#define AUTO_FIELDS      1800

/* Hilo de emulación: más prioridad que el principal (1001), para que el
 * sistema le quite ciclos antes a la presentación. */
#define EMU_PRIO     1000
#define EMU_STACK    (256u << 10)
#define WAKE_US      4000  /* el hilo de presentación revisa controles y colas al menos cada tanto */
#define STILL_US     16000 /* en pausa, el último cuadro se vuelve a presentar cada tanto (flip) */
#define OVERLAY_US   10000 /* con el sistema encima, el menú atiende eventos y mandos cada tanto */
#define SLOW_FLIP_US 100000 /* un flip que tarda más, al log */
/* Latido: un hilo aparte mira que los dos se muevan y, si uno se queda
 * quieto más de STALL_US, dice dónde (docs/hilos-ppe.md). */
#define WATCH_PRIO   500
#define WATCH_US     500000u
#define STALL_US     2000000u
#define STALL_LOG_US 5000000u

static struct salsa64_n64 n64;
/* Trucos del juego que corre (cheats.h): se cargan al abrirlo y el hilo de
 * emulación los aplica al final de cada campo. */
static struct cheat_set game_cheats;
static struct salsa64_rec rec;
static struct salsa64_rec_prof rec_prof, rec_prof_prev;  /* perfil del recompilador (SALSA64_RECPROF_FLAG) */
static bool use_rec;  /* el CPU corre con el recompilador */
static struct salsa64_rec_profile *rec_profile;         /* perfil persistente por juego (1,4 MB; solo con perfil-activo) */
static struct salsa64_rec_profile_writer rec_profile_w; /* su escritura por trozos, si la hace el hilo de presentación */
static struct {
	bool on;                  /* hay perfil para este juego (rec.profile) */
	char path[SALSA64_RPF_PATH_MAX];
	uint64_t t_saved;         /* cuándo empezó la última escritura periódica */
	uint64_t session_saved;   /* rec_profile->session en esa escritura */
	uint32_t fold_fields;     /* campos desde la última suma al perfil */
	volatile uint32_t saves, save_errors;
	/* Hilo propio de baja prioridad para escribir el archivo (el disco
	 * bloquea; la presentación no debe esperarlo): la presentación solo le
	 * avisa por el semáforo. Si no se pudo crear, escribe la presentación por
	 * trozos. */
	bool thread_ok;
	volatile int stop;        /* al cerrar: que termine */
	volatile int due;         /* hay una escritura pedida */
	sys_ppu_thread_t tid;
	sys_sem_t sem;
} rprof;
/* Muestreo por tiempo del recompilador (rec_sample.h; archivo `muestreo`):
 * un hilo de prioridad SAMPLE_PRIO, por debajo de emulación y presentación
 * (corre en el hilo de hardware que esté libre: el de presentación duerme
 * el 85 %), lee cada SAMPLE_US dónde está el hilo de emulación. Al cerrar el
 * juego, los 30 bloques con más muestras van al log. */
#define SAMPLE_PRIO 1100
#define SAMPLE_US   200u
static struct salsa64_rec_sample *rec_sample;  /* ~1 MB, solo con el archivo */
static struct {
	bool on;
	volatile int stop;
	sys_ppu_thread_t tid;
	uint64_t t0;
	struct salsa64_rec_sample_prev prev;  /* para el reparto de cada ventana de estadísticas */
} rsmp;
static uint8_t isv_buf[SALSA64_ISV_SIZE] __attribute__((aligned(16)));
static struct salsa64_save save;
static struct savefile save_file;
static struct salsa64_mempak mempak;
static struct mempakfile mempak_file;
static struct transferpakfile transferpak_files[SALSA64_PIF_PORTS];
static uint8_t *rdram;
static volatile int running;    /* la sesión de juego sigue */
static volatile int emu_done;   /* el hilo de emulación salió de su bucle */
static bool reclaim_emu;        /* optativo: recoger reservas TLS de newlib antes de salir */
static volatile int app_quit;   /* «Salir del juego» del sistema: salir de la aplicación */
/* Pausa (pausectl.h): el menú del sistema y el cuadro de START + SELECT.
 * pause_ctl es solo del hilo de presentación (el callback de sysutil corre
 * en él); pause_req es lo que pide a emulación, que se detiene al empezar
 * el campo siguiente y lo dice con emu_paused. */
static struct pausectl pause_ctl;
static volatile uint32_t pause_req;
static volatile int emu_paused;
static uint64_t sys_menu_t0;    /* cuándo se abrió el menú del sistema */
static bool sys_no_pause;       /* NO_PAUSE_PATH: el menú del sistema no pausa */
static volatile int emu_live;   /* el hilo de emulación existe (el latido lo espera) */
static struct cuelgue cuelgue;  /* el juego se quedó esperando (cuelgue.c; lo mira el latido) */
/* Dónde anda cada hilo, para el latido (watch_thread). */
static struct {
	volatile uint32_t pres, emu;  /* vueltas */
	const char *volatile pres_at, *volatile emu_at;
	volatile int quit;
} beat = { 0, 0, "arranque", "-", 0 };
static int in_menu;             /* sin embedded: START + SELECT vuelve al menú */
/* Las líneas de resultado de una ROM de prueba: con el menú, la final es la
 * del conjunto (tools/rpcs3-run.py corta 30 s después de la primera). */
static const char *res_tag = "RESULTADO frontend";
static int game_auto_ok;        /* prueba automática con una ROM del disco: llegó a AUTO_FIELDS */
static int gfx_on_spu;
static struct salsa64_boot_info boot_info;
static int selftest_on;   /* vitest */
static int gfxtest_on;    /* gfxtest */
static int audiotest_on;  /* audiotest */
static struct salsa64_gfx_spu gfx_spu;
/* La HLE en el PPE (si se pide) y, con el RSP a bajo nivel, el RDP en el PPE
 * (consume los tramos de comandos que entrega el microcódigo real). */
static struct salsa64_gfx_sync gfx_ppe;
static struct gfx_state gfx_ppe_state;
/* RSP a bajo nivel (Fase 6): con SALSA64_DATA_DIR/lle presente, el
 * microcódigo real corre en un SPE para lo que no tiene HLE ("auto") o
 * para todo ("all"); el RDP lo consume gfx_ppe en el PPE. */
static struct salsa64_rsp_spu rsp_spu;
static struct salsa64_jpeg_spu jpeg_spu;
static int jpeg_on_spu;  /* las tareas JPEG van al SPE (jpeg_spu) */
static enum salsa64_lle_mode lle_mode;
static sys_sem_t wake_sem;  /* la emulación despierta a la presentación */
static struct pacer pacer;  /* ritmo de la tele emulada (hilo de emulación) */

static void pacer_sleep(void *ctx, uint64_t us)
{
	(void)ctx;
	sysUsleep((u32)us);
}

static inline void lwsync(void) { __asm__ volatile("lwsync" ::: "memory"); }

static void wake(void)
{
	sysSemPost(wake_sem, 1);  /* si ya está en 1, da error: no importa */
}

/* Diagnóstico de la pantalla: cuántos cuadros faltan para leer dos píxeles
 * del cuadro dibujado (frame_end); al arrancar y al volver del sistema. */
static uint32_t screen_probe = 30;

/* Cuando el sistema deja de estar encima: el estado del RSX de la
 * aplicación puede no ser el que rsxkit da por puesto (después de su menú,
 * la PS3 quedaba en negro aunque se siguiera presentando). */
static void system_overlay_off(void)
{
	int bad = rsxkit_after_system();
	salsa64_log("sistema: al volver, estado del RSX puesto de nuevo; %d programas cambiados en su memoria, %d mapeos "
		    "de la memoria principal distintos", bad, rsxkit_main_maps_bad());
	screen_probe = 30;  /* medio segundo después: lo que quedó en pantalla */
}

/* Dentro de una espera larga al RSX (rsx_wait_hook) los eventos se atienden
 * igual, pero lo que manda órdenes al RSX (system_overlay_off) queda para
 * después: rsxkit está a mitad de una llamada. */
static volatile int rsx_in_wait;
static bool overlay_off_pending;

/* Eventos del sistema. Corre dentro de sysUtilCheckCallback, en el hilo
 * de presentación (o el del menú): puede tocar pause_ctl y escribir al log. */
static void sysutil_cb(u64 status, u64 param, void *user)
{
	(void)user;
	uint64_t now = sysGetSystemTime();
	bool was_over = pausectl_overlay(&pause_ctl);
	switch (status) {
	case SYSUTIL_EXIT_GAME:
		salsa64_log("sistema: «Salir del juego»");
		app_quit = 1;
		running = 0;
		break;
	case SYSUTIL_MENU_OPEN:
		pausectl_system_menu(&pause_ctl, true);
		sys_menu_t0 = now;
		if (emu_live)
			salsa64_log("sistema: menú abierto (campo %u)", n64.bus.vi.fields);
		else
			salsa64_log("sistema: menú abierto (sin juego)");
		break;
	case SYSUTIL_MENU_CLOSE:
		pausectl_system_menu(&pause_ctl, false);
		salsa64_log("sistema: menú cerrado tras %.1f s", sys_menu_t0 ? (now - sys_menu_t0) / 1e6 : 0.0);
		break;
	case SYSUTIL_DRAW_BEGIN:
		pausectl_system_draw(&pause_ctl, true);
		salsa64_log("sistema: empieza a dibujar encima");
		break;
	case SYSUTIL_DRAW_END:
		pausectl_system_draw(&pause_ctl, false);
		salsa64_log("sistema: deja de dibujar encima");
		break;
	default:
		if (ra_sysutil_event(status, param))
			break;  /* el teclado del sistema (usuario y contraseña de RetroAchievements) */
		salsa64_log("sistema: evento 0x%llx (parámetro 0x%llx)", (unsigned long long)status,
			    (unsigned long long)param);
		break;
	}
	if (was_over && !pausectl_overlay(&pause_ctl)) {
		if (rsx_in_wait)
			overlay_off_pending = true;
		else
			system_overlay_off();
	}
}

/* sysUtilCheckCallback y lo que haya quedado de una espera al RSX. */
static void sysutil_check(void)
{
	sysUtilCheckCallback();
	if (overlay_off_pending) {
		overlay_off_pending = false;
		system_overlay_off();
	}
}

static struct salsa64_padmap padmap;

/* ---- Lo que cruza entre los hilos (docs/hilos-ppe.md) ----------------------- */

/* Copia de la imagen del VI al terminar el campo: la escribe emulación, la
 * sube al RSX presentación. Tres ranuras bastan: una escribiéndose, una
 * en cola (la más nueva; las viejas se liberan) y una en uso. */
#define SNAP_SLOTS 3
#define SNAP_BYTES (FB_MAX_W * FB_MAX_H * 4)
enum { SNAP_FREE, SNAP_WRITING, SNAP_QUEUED, SNAP_IN_USE };
static uint8_t *snap_buf[SNAP_SLOTS];
static volatile uint32_t snap_state[SNAP_SLOTS];

static int snap_cas(int slot, uint32_t from, uint32_t to)
{
	return __sync_bool_compare_and_swap(&snap_state[slot], from, to);
}

/* Un campo terminado, de emulación a presentación. */
#define FIELD_QUEUE 8
#define FIELD_CHECK 0x1u  /* verificar la pantalla (selftest): no se salta */
#define FIELD_SHOT  0x2u  /* captura de un campo fijo (capturas = N): no se salta */
#define FIELD_STATE 0x4u  /* emulación se detiene después de este campo (estado guardado): miniatura */
#define FIELD_KEEP  (FIELD_CHECK | FIELD_SHOT | FIELD_STATE)
#define GFX_WIN_FIELDS 100
static uint32_t cap_every, exit_field;  /* capturas = "N M" */
static uint32_t trace_vi_count, trace_vi_first;  /* SALSA64_DATA_DIR/trace-vi: ventana de campos */
struct field_msg {
	uint32_t field;             /* n64.bus.vi.fields al terminar */
	uint32_t tasks;             /* tareas gráficas terminadas hasta ahí */
	struct salsa64_vi_frame f;  /* f.width == 0: sin imagen */
	int32_t snap;               /* ranura con la copia del VI, o -1 */
	uint32_t flags;
	volatile uint32_t skip;     /* emulación lo pone si encoló uno más nuevo */
	uint32_t vi[SALSA64_VI_NREGS];  /* registros del VI al terminar el campo (caja negra) */
	uint32_t vi_current, parity;  /* lecturas calculadas al terminar el campo */
};
static struct field_msg field_q[FIELD_QUEUE];
static volatile uint32_t field_head, field_tail;  /* head la mueve emulación; tail, presentación */

/* Controles, de presentación a emulación: el último estado (seqlock: seq
 * impar = escribiendo) y, aparte, el OR de los botones recién pulsados
 * desde que emulación los tomó, para que una pulsación publicada y soltada
 * entre dos campos llegue al PIF (que la retiene hasta entregarla al
 * juego). Solo los flancos: con todo lo pulsado, cada soltada se
 * retrasaría un campo. */
static struct {
	volatile uint32_t seq;
	struct salsa64_pad pads[SALSA64_PIF_PORTS];
	volatile uint32_t pressed[SALSA64_PIF_PORTS];
} pad_box;

/* The emulation thread publishes the Rumble Pak latch; only the presentation
 * thread calls libpad's actuator API alongside the normal pad polling. */
static struct {
	volatile uint32_t seq;
	uint8_t on[SALSA64_PIF_PORTS];
} rumble_box;
static uint8_t rumble_applied[SALSA64_PIF_PORTS];

static uint16_t pad_published[SALSA64_PIF_PORTS];

static void pads_publish(const struct salsa64_pad pads[SALSA64_PIF_PORTS])
{
	pad_box.seq++;
	lwsync();
	memcpy(pad_box.pads, pads, sizeof pad_box.pads);
	lwsync();
	pad_box.seq++;
	for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++) {
		uint16_t rise = (uint16_t)(pads[i].buttons & ~pad_published[i]);
		if (rise)
			__sync_fetch_and_or(&pad_box.pressed[i], rise);
		pad_published[i] = pads[i].buttons;
	}
}

static void rumble_publish(void)
{
	rumble_box.seq++;
	lwsync();
	memcpy(rumble_box.on, n64.bus.pif.rumble, sizeof rumble_box.on);
	lwsync();
	rumble_box.seq++;
}

static void rumble_snapshot(uint8_t on[SALSA64_PIF_PORTS])
{
	for (unsigned spins = 0;; spins++) {
		uint32_t s1 = rumble_box.seq;
		if (s1 & 1) {
			if (spins > 64)
				sysUsleep(20);
			continue;
		}
		lwsync();
		memcpy(on, rumble_box.on, SALSA64_PIF_PORTS);
		lwsync();
		if (rumble_box.seq == s1)
			return;
	}
}

static void rumble_stop_all(void)
{
	padActParam act;
	memset(&act, 0, sizeof act);
	for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++) {
		/* También se manda cero si el estado local quedó desfasado por una
		 * desconexión o por un cierre de emergencia. */
		ioPadSetActDirect(i, &act);
		rumble_applied[i] = 0;
	}
}

static void pads_apply(void)
{
	struct salsa64_pad pads[SALSA64_PIF_PORTS];
	for (unsigned spins = 0;; spins++) {
		uint32_t s1 = pad_box.seq;
		if (s1 & 1) {
			/* Presentación está escribiendo: son unos nanosegundos, salvo
			 * que el sistema la haya desalojado a la mitad; este hilo tiene
			 * más prioridad, así que no se gira para siempre. */
			if (spins > 64)
				sysUsleep(20);
			continue;
		}
		lwsync();
		memcpy(pads, pad_box.pads, sizeof pads);
		lwsync();
		if (pad_box.seq == s1)
			break;
	}
	for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++) {
		if (pad_box.pressed[i])
			pads[i].buttons |= (uint16_t)__sync_fetch_and_and(&pad_box.pressed[i], 0);
		salsa64_pif_set_pad(&n64.bus, i, &pads[i]);
	}
	rumble_publish();
}

/* Flancos de lo que el PIF entregó al juego (respuestas de estado): los
 * anota emulación al terminar cada campo y los escribe presentación. */
#define PIF_EDGES 32
static struct {
	struct salsa64_pad pad;
	uint32_t field, reads;
	uint8_t port;
} pif_edge[PIF_EDGES];
static volatile uint32_t pif_edge_head, pif_edge_tail, pif_edge_lost;
static uint16_t pif_edge_seen[SALSA64_PIF_PORTS];

static void pif_edges_note(void)
{
	const struct salsa64_pif *pif = &n64.bus.pif;
	for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++) {
		if (pif->sent[i].buttons == pif_edge_seen[i])
			continue;
		pif_edge_seen[i] = pif->sent[i].buttons;
		if (pif_edge_head - pif_edge_tail >= PIF_EDGES) {
			__sync_fetch_and_add(&pif_edge_lost, 1);
			continue;
		}
		uint32_t k = pif_edge_head % PIF_EDGES;
		pif_edge[k].pad = pif->sent[i];
		pif_edge[k].field = n64.bus.vi.fields;
		pif_edge[k].reads = pif->reads[i];
		pif_edge[k].port = (uint8_t)i;
		lwsync();
		pif_edge_head++;
	}
}

static void pif_edges_log(void)
{
	while (pif_edge_tail != pif_edge_head) {
		lwsync();  /* la entrada, después de ver head */
		uint32_t k = pif_edge_tail % PIF_EDGES;
		char desc[128];
		salsa64_pad_describe(&pif_edge[k].pad, desc, sizeof desc);
		salsa64_log("PIF → juego: control %u: %s (campo %u, lectura %u)", pif_edge[k].port + 1u, desc,
			    pif_edge[k].field, pif_edge[k].reads);
		lwsync();
		pif_edge_tail++;
	}
	uint32_t lost = pif_edge_lost ? __sync_fetch_and_and(&pif_edge_lost, 0) : 0;
	if (lost)
		salsa64_log("PIF → juego: %u flancos sin anotar (cola llena)", lost);
}

/* Contadores del hilo de emulación; presentación los lee cada 5 s. */
static struct {
	volatile uint64_t us_emu, us_copy, us_pace;
	volatile uint32_t fields, dropped, no_slot;
} emu_stats;

/* ---- Cuánto le quita el hilo de presentación a la emulación ------------------
 * (docs/hilos-ppe.md). Los dos hilos comparten el núcleo del PPE: lo que
 * hace presentación le quita ciclos a emulación. Presentación publica cuánto
 * lleva despierta (ticks del timebase; dormida = el semáforo y la espera del
 * flip anterior) y emulación mide, en cada campo, cuánto se solapó con su
 * trabajo; las ventanas de 5 s agrupan los campos por solape y dan, para
 * cada grupo, ms de emulación y ns por ciclo emulado fuera de los bucles de
 * espera (lo que no depende de cuánto trabajo traiga el campo). Con
 * SALSA64_DATA_DIR/carga-hilo1 («A B [KB]», por omisión 1000 3000 512),
 * además, trabajo de mentira por campo presentado que recorre KB de memoria
 * (512: toda la L2), en fases de medio segundo: 0, A y B µs; el log compara
 * los campos de cada fase. Sin el archivo, solo los contadores. */
#define LOAD_PATH SALSA64_DATA_DIR "/carga-hilo1"
#define LOAD_KB_DEFAULT 512u
#define LOAD_PHASES 3
#define OVL_GROUPS 4

static inline uint64_t tb_now(void)
{
	uint64_t t;
	__asm__ volatile("mftb %0" : "=r"(t));
	return t;
}

/* Un contador de ticks que escribe presentación y lee emulación: lo hecho
 * hasta `since` y desde cuándo corre (0: parado). */
struct busy_clock {
	volatile uint64_t done, since;
};

static void busy_start(struct busy_clock *c, uint64_t t)
{
	c->since = t;
}

static void busy_stop(struct busy_clock *c, uint64_t t)
{
	if (!c->since)
		return;
	c->done += t - c->since;
	lwsync();
	c->since = 0;
}

/* Desde el otro hilo: lo hecho hasta `t`. */
static uint64_t busy_at(const struct busy_clock *c, uint64_t t)
{
	for (;;) {
		uint64_t since = c->since;
		lwsync();
		uint64_t done = c->done;
		lwsync();
		if (since == c->since)
			return done + (since && t > since ? t - since : 0);
	}
}

/* Campos agrupados por solape (emulación; presentación los lee cada 5 s). */
struct ovl_group {
	uint64_t fields, tb_emu, tb_ovl, tb_load, cycles, sleep_us;
};

static struct {
	struct busy_clock busy, load_clock;  /* presentación despierta; carga de mentira */
	bool load;                     /* carga-hilo1 presente */
	uint32_t load_us[LOAD_PHASES], load_kb;  /* µs por campo de cada fase (la primera, 0) */
	uint8_t *load_buf;
	uint64_t load_next;            /* timebase del próximo cambio de fase */
	volatile uint32_t load_phase;  /* fase en curso (la lee emulación) */
	/* Presentación, ventana en curso (ticks): flip anterior (dormida),
	 * flush (traducción y lo que cae dentro), presentar el campo (sin el
	 * flush ni el flip anterior) y la carga de mentira; lo de rdp.tb que
	 * cayó dentro de los flush. */
	uint64_t t_flip, t_flush, t_present, t_load;
	struct salsa64_rdp_tb in_flush;
	/* Emulación: por solape (< 0,1 ms, 0,1–1, 1–3, ≥ 3) y por fase de la
	 * carga de mentira (la misma al empezar y al terminar el campo). */
	struct ovl_group by_ovl[OVL_GROUPS], by_load[LOAD_PHASES];
} h1;

static uint8_t h1_load_mem[LOAD_KB_DEFAULT << 10] __attribute__((aligned(128)));

static void load_config(void)
{
	FILE *f = fopen(LOAD_PATH, "r");
	h1.load = false;
	h1.load_phase = 0;
	h1.load_next = 0;
	if (!f)
		return;
	h1.load = true;
	h1.load_us[0] = 0;
	h1.load_kb = LOAD_KB_DEFAULT;
	int n = fscanf(f, "%u %u %u", &h1.load_us[1], &h1.load_us[2], &h1.load_kb);
	fclose(f);
	if (n < 1)
		h1.load_us[1] = 1000;
	if (n < 2)
		h1.load_us[2] = 3 * h1.load_us[1];
	if (n < 3 || !h1.load_kb || h1.load_kb > LOAD_KB_DEFAULT)
		h1.load_kb = LOAD_KB_DEFAULT;
	h1.load_buf = h1_load_mem;
	salsa64_log("carga-hilo1: trabajo de mentira por campo presentado sobre %u KB, en fases de medio segundo de 0, %u "
		    "y %u µs (diagnóstico, %s)", h1.load_kb, h1.load_us[1], h1.load_us[2], LOAD_PATH);
}

/* La carga de mentira: lee y escribe una línea de caché por vuelta sobre
 * load_kb, como la traducción que recorre la salida de la HLE y escribe
 * comandos del RSX, hasta gastar el tiempo pedido. */
static void h1_load_run(uint32_t us)
{
	static uint32_t pos;
	uint64_t hz = spukit_tick_hz(), t0 = tb_now(), end = t0 + (uint64_t)us * hz / 1000000u;
	uint32_t words = h1.load_kb << 7, acc = 0;  /* palabras de 8 bytes */
	volatile uint64_t *p = (volatile uint64_t *)h1.load_buf;
	busy_start(&h1.load_clock, t0);
	do {
		for (int k = 0; k < 256; k++) {
			acc += (uint32_t)p[pos];
			p[(pos + 8) % words] = acc;
			pos = (pos + 16) % words;  /* 128 bytes: la línea siguiente */
		}
	} while (tb_now() < end);
	uint64_t t1 = tb_now();
	busy_stop(&h1.load_clock, t1);
	h1.t_load += t1 - t0;
}

/* Al terminar cada campo (emulación). phase: la fase de la carga de
 * mentira durante todo el campo (-1: cambió en medio o sin carga). */
static void ovl_note(uint64_t tb_emu, uint64_t tb_ovl, uint64_t tb_load, uint64_t cycles, uint64_t sleep_us, int phase)
{
	uint64_t ms = spukit_tick_hz() / 1000u;
	int g = tb_ovl < ms / 10 ? 0 : tb_ovl < ms ? 1 : tb_ovl < 3 * ms ? 2 : 3;
	/* Por solape, solo los campos sin carga de mentira: así una corrida con
	 * carga-hilo1 da las dos mediciones. */
	struct ovl_group *o[2] = { tb_load || phase > 0 ? NULL : &h1.by_ovl[g], NULL };
	if (phase >= 0)
		o[1] = &h1.by_load[phase];
	for (int i = 0; i < 2; i++) {
		if (!o[i])
			continue;
		o[i]->fields++;
		o[i]->tb_emu += tb_emu;
		o[i]->tb_ovl += tb_ovl;
		o[i]->tb_load += tb_load;
		o[i]->cycles += cycles;
		o[i]->sleep_us += sleep_us;
	}
}

/* «n campos: x ms de emulación, y ns por ciclo, solape z ms» de un grupo
 * (diferencia con la ventana anterior). */
static int ovl_text(char *out, size_t n, const struct ovl_group *a, const struct ovl_group *b, bool load)
{
	uint64_t f = a->fields - b->fields, cyc = a->cycles - b->cycles;
	double k = 1e3 / spukit_tick_hz();  /* ticks → ms */
	if (!f)
		return snprintf(out, n, "0");
	double emu = (a->tb_emu - b->tb_emu) * k, sleep = (a->sleep_us - b->sleep_us) / 1e3;
	int w = snprintf(out, n, "%llu (%.2f ms, %.2f ns/ciclo, solape %.2f ms", (unsigned long long)f, emu / f,
			 cyc ? (emu - sleep) * 1e6 / cyc : 0.0, (a->tb_ovl - b->tb_ovl) * k / f);
	if (w > 0 && (size_t)w < n)
		w += snprintf(out + w, n - w, load ? ", carga %.2f ms)" : ")", (a->tb_load - b->tb_load) * k / f);
	return w;
}

/* Petición de presentación a emulación: empezar la medición A/B de vitest. */
static volatile int bench_request;

/* Estados guardados (states.c, core/state.h). Presentación pide guardar o
 * cargar (SELECT + R1 / L1); emulación, al terminar el campo, recoge la
 * tarea del RSP en vuelo, encola el campo con FIELD_STATE (de él sale la
 * miniatura) y espera quieta; presentación vacía las colas, espera al RSX,
 * hace el trabajo y la suelta (state_barrier). */
enum { STATE_NONE, STATE_SAVE, STATE_LOAD, STATE_RESET };  /* STATE_RESET: el NMI del botón Reset, pedido por la emulación */
static volatile int state_req;     /* lo pone presentación; lo baja al terminar */
static volatile int state_parked;  /* emulación espera en el límite del campo */
static volatile int soft_reset_req;  /* 1: «Reiniciar (botón Reset)» pedido; 2: PRE_NMI dado, falta el NMI */
static volatile int restart_cold;    /* «Reiniciar desde cero» pedido: el juego se cierra y se abre otra vez */
static volatile int state_quiet;   /* la tarea del RSP quedó recogida (sin LLE) */
static unsigned state_slot;        /* ranura actual, 0 … SALSA64_STATE_SLOTS − 1 */
static struct osd_fps fps;         /* contador de velocidad (SELECT + △, menu.cfg) */
static bool ui_ok;                 /* rsxkit_ui listo: aviso y contador sobre el juego */
static float ui_lw, ui_lh;         /* su espacio lógico */
/* Versión corta (menú y pausa) y, en un paquete de prueba, «título · APPID»
 * (version.h); las arma main() al arrancar. */
static char version_text[64], build_text[160];

/* Sink gráfico del frontend: el del RDP más el timbre a presentación. */
static const struct salsa64_gfx_sink *rdp_sink;
static bool gfx_affine_chain;
static bool gfx_s2dex_old_modes;
static bool gfx_no_fog_1cycle;
static bool gfx_no_rsp_clip;  /* sin-sombra-afin: la HLE no recorta contra la caja del RSP */
static bool gfx_s2dex_bg_old;
static bool gfx_no_yuv;
static bool gfx_blend_old;
static bool gfx_s2dex_bg_row_old;
static bool gfx_f5_sub_old;

static void front_acquire(void *ctx, struct gfx_job *job)
{
	(void)ctx;
	rdp_sink->acquire(rdp_sink->ctx, job);
	if (gfx_affine_chain)
		job->flags |= GFX_JOB_AFFINE_CHAIN;
	if (gfx_s2dex_bg_old)
		job->flags |= GFX_JOB_S2DEX_BG_OLD;
	if (gfx_no_yuv)
		job->flags |= GFX_JOB_NO_YUV;
	if (gfx_blend_old)
		job->flags |= GFX_JOB_BLEND_OLD;
	if (gfx_s2dex_old_modes)
		job->flags |= GFX_JOB_S2DEX_OLD_MODES;
	if (gfx_no_fog_1cycle)
		job->flags |= GFX_JOB_NO_FOG_1CYCLE;
	if (gfx_no_rsp_clip)
		job->flags |= GFX_JOB_NO_RSP_CLIP;
	if (gfx_s2dex_bg_row_old)
		job->flags |= GFX_JOB_S2DEX_BG_ROW_OLD;
	if (gfx_f5_sub_old)
		job->flags |= GFX_JOB_F5_SUB_OLD;
}

/* DMA a la RDRAM (hilo de emulación): las copias del RDP no lo pisan. */
static void front_rdram_written(void *ctx, uint32_t pa, uint32_t len)
{
	(void)ctx;
	salsa64_rdp_rdram_written(pa, len, n64.bus.rsp.busy == SALSA64_TASK_GFX);
}

static void front_done(void *ctx, const struct gfx_job *job, const struct gfx_result *res)
{
	(void)ctx;
	rdp_sink->done(rdp_sink->ctx, job, res);
	wake();
}

static const struct salsa64_gfx_sink front_sink = { NULL, front_acquire, front_done };

/* ---- Medición A/B con vitest (hilo de emulación) ----------------------------- */

static struct {
	int stage;  /* 0 nada, 1 sin detección, 2 con detección, 3 terminada */
	uint64_t t0, us_emu, cycles0, idle0;
	uint32_t fields;
	double rate[2], ms[2];
} bench;

static void bench_begin(int stage, uint64_t now)
{
	bench.stage = stage;
	bench.t0 = now;
	bench.us_emu = 0;
	bench.fields = 0;
	bench.cycles0 = n64.cpu.cycles;
	bench.idle0 = n64.cpu.idle_cycles;
	salsa64_cpu_set_idle(&n64.cpu, stage == 2);
}

static void bench_field(uint64_t now, uint64_t us_emu)
{
	if (bench.stage != 1 && bench.stage != 2)
		return;
	bench.us_emu += us_emu;
	bench.fields++;
	uint64_t dt = now - bench.t0;
	if (dt < BENCH_US)
		return;
	int k = bench.stage - 1;
	uint64_t cyc = n64.cpu.cycles - bench.cycles0, idle = n64.cpu.idle_cycles - bench.idle0;
	bench.rate[k] = bench.fields * 1e6 / dt;
	bench.ms[k] = bench.us_emu / 1000.0 / bench.fields;
	salsa64_log("MEDICIÓN vitest %s detección de bucles de espera: %.2f campos/s, emulación %.2f ms "
		    "por campo, %.1f MIPS, %.1f %% del tiempo saltado (%u campos en %.1f s)",
		    k ? "con" : "sin", bench.rate[k], bench.ms[k], (double)(cyc - idle) / dt,
		    cyc ? 100.0 * idle / cyc : 0.0, bench.fields, dt / 1e6);
	if (k == 0) {
		bench_begin(2, now);
	} else {
		bench.stage = 3;
		salsa64_log("MEDICIÓN vitest: emulación %.2f → %.2f ms por campo (×%.1f), %.2f → %.2f campos/s",
			    bench.ms[0], bench.ms[1], bench.ms[1] > 0 ? bench.ms[0] / bench.ms[1] : 0.0,
			    bench.rate[0], bench.rate[1]);
	}
}

/* ---- Controles (hilo de presentación) --------------------------------------- */

/* Lee controles.cfg si existe; si no, el mapeo por omisión. */
static void load_padmap(void)
{
	static char text[PADS_CFG_MAX];
	salsa64_padmap_default(&padmap);
	FILE *f = fopen(PADS_PATH, "rb");
	if (!f) {
		salsa64_log("controles: mapeo por omisión (no hay %s)", PADS_PATH);
		return;
	}
	size_t len = fread(text, 1, sizeof text, f);
	fclose(f);
	char err[96];
	int bad = salsa64_padmap_parse(&padmap, text, len, err, sizeof err);
	if (bad)
		salsa64_log("controles: %s con %d líneas que no entendí; la primera, %s", PADS_PATH, bad, err);
	else
		salsa64_log("controles: %s", PADS_PATH);
}

static bool save_padmap(const struct salsa64_padmap *map)
{
	char text[512];
	int n = salsa64_padmap_format(map, text, sizeof text, 0);
	if (n < 0)
		return false;
	FILE *f = fopen(PADS_PATH, "w");
	bool ok = f && fwrite(text, 1, (size_t)n, f) == (size_t)n;
	if (f && fclose(f) != 0) ok = false;
	return ok;
}

static struct salsa64_ds3 ds3_from(const padData *d)
{
	struct salsa64_ds3 s;
	s.buttons = (uint32_t)d->BTN_CROSS << SALSA64_DS3_CROSS | (uint32_t)d->BTN_CIRCLE << SALSA64_DS3_CIRCLE |
		    (uint32_t)d->BTN_SQUARE << SALSA64_DS3_SQUARE | (uint32_t)d->BTN_TRIANGLE << SALSA64_DS3_TRIANGLE |
		    (uint32_t)d->BTN_L1 << SALSA64_DS3_L1 | (uint32_t)d->BTN_R1 << SALSA64_DS3_R1 |
		    (uint32_t)d->BTN_L2 << SALSA64_DS3_L2 | (uint32_t)d->BTN_R2 << SALSA64_DS3_R2 |
		    (uint32_t)d->BTN_L3 << SALSA64_DS3_L3 | (uint32_t)d->BTN_R3 << SALSA64_DS3_R3 |
		    (uint32_t)d->BTN_START << SALSA64_DS3_START | (uint32_t)d->BTN_SELECT << SALSA64_DS3_SELECT |
		    (uint32_t)d->BTN_UP << SALSA64_DS3_UP | (uint32_t)d->BTN_DOWN << SALSA64_DS3_DOWN |
		    (uint32_t)d->BTN_LEFT << SALSA64_DS3_LEFT | (uint32_t)d->BTN_RIGHT << SALSA64_DS3_RIGHT;
	s.lx = (uint8_t)d->ANA_L_H;
	s.ly = (uint8_t)d->ANA_L_V;
	s.rx = (uint8_t)d->ANA_R_H;
	s.ry = (uint8_t)d->ANA_R_V;
	return s;
}

/* ---- Estados guardados y atajos con SELECT (hilo de presentación) ---------- */

static unsigned state_req_slot;  /* la ranura de la petición en curso */
static bool dump_now;            /* SELECT + R2: volcar la HLE en el próximo cuadro */
static int combos_on;            /* dentro del juego (no en el menú) */
static uint32_t state_done;      /* barreras terminadas */
static bool state_last_ok;       /* cómo terminó la última */

/* Prueba automática de los estados (SALSA64_DATA_DIR/estados-prueba = "A B",
 * tools/rpcs3-run.py --estados): guardar en la ranura 9 al llegar al campo
 * A y cargarla en el B; la ROM de prueba tiene que verificarse igual
 * después. */
#define STATE_TEST_PATH SALSA64_DATA_DIR "/estados-prueba"
enum { ST_OFF, ST_SAVE, ST_SAVING, ST_LOAD, ST_LOADING, ST_DONE, ST_FAILED };
static struct {
	uint32_t save_at, load_at, done0;
	int stage;
} state_test;

static void fps_setting_save(bool on);
static bool fps_setting_load(void);
static bool confirm_setting_load(void);

static void state_command(int what)
{
	uint64_t now = sysGetSystemTime();
	if (state_req || state_parked)
		return;  /* ya hay una en curso, o emulación todavía sale de la anterior */
	if (!ra_rules_allowed(ra_hardcore_active(), what == STATE_LOAD ? RA_RULE_STATE_LOAD : RA_RULE_STATE_SAVE)) {
		osd_notice(now, "RA Hardcore: sin cargar estados");
		return;
	}
	if (lle_mode != SALSA64_LLE_OFF) {
		osd_notice(now, "Estados no disponibles con lle");
		return;
	}
	if (what == STATE_LOAD && !states_slot_exists(&n64.bus, state_slot)) {
		osd_notice(now, "No hay estado en la ranura %u", state_slot);
		salsa64_log("estado: no hay estado en la ranura %u", state_slot);
		return;
	}
	salsa64_log("estado: %s la ranura %u (pedido en el campo %u)", what == STATE_SAVE ? "guardar en" : "cargar",
		    state_slot, n64.bus.vi.fields);
	state_req_slot = state_slot;
	lwsync();
	state_req = what;
}

static void state_slot_step(int d)
{
	state_slot = (state_slot + SALSA64_STATE_SLOTS + (unsigned)d) % SALSA64_STATE_SLOTS;
	bool have = states_slot_exists(&n64.bus, state_slot);
	osd_notice(sysGetSystemTime(), have ? "Ranura %u" : "Ranura %u (vacía)", state_slot);
}

/* Lo que pidió la lista de trucos del juego (pausectl): activar o desactivar
 * el elegido y avisar de que no hay trucos. */
static bool pm_slot_have;  /* la ranura actual tiene estado (para el menú de pausa) */

static void image_events(void);
static void accessory_menu_events(void);
static int menu_accessory_default(void);

/* Lo que pidió el menú de pausa (pausectl): estados, ranura, FPS. */
static void pause_menu_events(void)
{
	static bool was_open;
	if (pause_ctl.menu && !was_open) {
		pm_slot_have = states_slot_exists(&n64.bus, state_slot);
		ra_list_refresh();  /* los logros que se ven en la pausa */
		pause_ctl.ach_rows = ra_list_count();
		pause_ctl.ach_avail = pause_ctl.ach_rows > 0;
	}
	was_open = pause_ctl.menu;
	int d = pause_ctl.menu_do;
	if (d < 0)
		return;
	pause_ctl.menu_do = -1;
	uint64_t now = sysGetSystemTime();
	switch (d) {
	case PM_SAVE:
		state_command(STATE_SAVE);
		break;
	case PM_LOAD:
		state_command(STATE_LOAD);
		break;
	case PM_SLOT:
		state_slot = (state_slot + SALSA64_STATE_SLOTS + (unsigned)pause_ctl.menu_arg) % SALSA64_STATE_SLOTS;
		pm_slot_have = states_slot_exists(&n64.bus, state_slot);
		break;
	case PM_FPS:
		fps.on = !fps.on;
		osd_fps_reset(&fps);
		fps_setting_save(fps.on);
		break;
	case PM_CHEATS:
		osd_notice(now, ra_hardcore_active() ? "RA Hardcore: sin trucos" : "No hay trucos para este juego");
		break;
	case PM_ACHIEVEMENTS:
		osd_notice(now, ra_enabled() ? "Sin logros para este juego" : "RetroAchievements está apagado (Opciones)");
		break;
	}
}

static void cheat_panel_events(void)
{
	if (pause_ctl.panel_denied) {
		pause_ctl.panel_denied = false;
		osd_notice(sysGetSystemTime(), "No hay trucos para este juego");
	}
	int t = pause_ctl.toggle;
	if (t < 0)
		return;
	pause_ctl.toggle = -1;
	if ((unsigned)t >= game_cheats.n)
		return;
	game_cheats.c[t].on = !game_cheats.c[t].on;
	bool ok = cheats_save(&game_cheats);
	salsa64_log("trucos: «%s» %s, %u activos%s", game_cheats.c[t].desc, game_cheats.c[t].on ? "activado" : "desactivado",
		    cheats_active(&game_cheats), ok ? "" : " (no se pudo guardar)");
}

/* Atajos dentro del juego, con SELECT (la N64 no lo tiene; START + SELECT
 * vuelve al menú): + R1 guarda el estado en la ranura actual, + L1 lo
 * carga, + izquierda o derecha de la cruceta cambia de ranura, + △
 * muestra u oculta el contador de velocidad, + R2 vuelca la salida de la
 * HLE gráfica con la captura del cuadro y + L2 escribe la caja negra (el
 * anillo de las últimas tareas y campos; cajanegra.h). Mientras SELECT está apretado
 * esos botones no llegan al juego. Devuelve los botones para el mapeo. */
#define COMBO_MASK (1u << SALSA64_DS3_R1 | 1u << SALSA64_DS3_L1 | 1u << SALSA64_DS3_LEFT | \
		    1u << SALSA64_DS3_RIGHT | 1u << SALSA64_DS3_TRIANGLE | 1u << SALSA64_DS3_R2 | 1u << SALSA64_DS3_L2)

static uint32_t pad_combos(unsigned port, uint32_t b)
{
	static uint32_t prev[SALSA64_PIF_PORTS];
	uint32_t rise = b & ~prev[port];
	prev[port] = b;
	if (!combos_on || !(b & 1u << SALSA64_DS3_SELECT))
		return b;
	if (rise & 1u << SALSA64_DS3_R1)
		state_command(STATE_SAVE);
	if (rise & 1u << SALSA64_DS3_L1)
		state_command(STATE_LOAD);
	if (rise & 1u << SALSA64_DS3_RIGHT)
		state_slot_step(1);
	if (rise & 1u << SALSA64_DS3_LEFT)
		state_slot_step(-1);
	if (rise & 1u << SALSA64_DS3_TRIANGLE) {
		fps.on = !fps.on;
		osd_fps_reset(&fps);
		osd_notice(sysGetSystemTime(), "FPS: %s", tr(fps.on ? "sí" : "no"));
		fps_setting_save(fps.on);
	}
	if (rise & 1u << SALSA64_DS3_R2)
		dump_now = true;
	if (rise & 1u << SALSA64_DS3_L2)
		cajanegra_request();
	return b & ~COMBO_MASK;
}

/* Lecturas de los mandos en la ventana de estadísticas y el hueco más
 * largo entre dos: una pulsación más corta que el hueco no llega a verse
 * (ioPadGetData da solo el último estado). */
static struct {
	uint64_t last_us, max_gap_us;
	uint32_t polls;
} pad_poll;

/* El stick izquierdo como cruceta para el menú (mandos con la cruceta
 * gastada): solo el eje dominante, apretado al pasar de PAD_STICK_ON del
 * centro y suelto al volver a menos de PAD_STICK_OFF, para que no tiemble en
 * el borde. update_pads deja aquí las direcciones de todos los mandos. */
#define PAD_STICK_ON 80
#define PAD_STICK_OFF 48
static uint32_t pad_stick_dirs;

static uint32_t stick_dirs(uint32_t prev, int x, int y)
{
	bool horiz = abs(x) >= abs(y);
	int v = horiz ? x : y;
	uint32_t neg = 1u << (horiz ? SALSA64_DS3_LEFT : SALSA64_DS3_UP);
	uint32_t pos = 1u << (horiz ? SALSA64_DS3_RIGHT : SALSA64_DS3_DOWN);
	if (v > ((prev & pos) ? PAD_STICK_OFF : PAD_STICK_ON))
		return pos;
	if (-v > ((prev & neg) ? PAD_STICK_OFF : PAD_STICK_ON))
		return neg;
	return 0;
}

/* DualShock 3 → controles de la N64, publicados para el hilo de emulación.
 * ioPadGetData da len = 0 cuando nada cambió: se conserva el último
 * estado. Con el menú del sistema abierto o los mandos tomados por el
 * sistema (bit 0 de padInfo.info), todo suelto: lo que se suelte allá no se
 * ve aquí, así que nada queda pegado al volver. pausectl decide qué llega al
 * juego (nada con el cuadro de salir a la vista; lo apretado al abrirlo o
 * cerrarlo, hasta que se suelte). Devuelve 1 si hay que salir del juego
 * (START + SELECT, confirmado o sin confirmación); en `buttons` (puede ser
 * NULL), los botones de todos los mandos juntos (el menú). */
static int update_pads(uint32_t *buttons)
{
	static struct salsa64_ds3 last[SALSA64_PIF_PORTS] = {
		[0 ... SALSA64_PIF_PORTS - 1] = { 0, 128, 128, 128, 128 },
	};
	static struct salsa64_pad logged[SALSA64_PIF_PORTS];
	static struct salsa64_ds3 raw_logged[SALSA64_PIF_PORTS] = {
		[0 ... SALSA64_PIF_PORTS - 1] = { 0, 128, 128, 128, 128 },
	};
	static uint8_t present[SALSA64_PIF_PORTS];
	static uint32_t stick[SALSA64_PIF_PORTS];
	const struct salsa64_ds3 neutral = { 0, 128, 128, 128, 128 };
	struct salsa64_pad pads[SALSA64_PIF_PORTS];
	uint32_t raw[SALSA64_PIF_PORTS], game[SALSA64_PIF_PORTS];
	padInfo info;
	padData data;
	uint32_t all = 0;

	if (buttons)
		*buttons = 0;
	pad_stick_dirs = 0;
	uint64_t now = sysGetSystemTime();
	if (pad_poll.last_us && now - pad_poll.last_us > pad_poll.max_gap_us)
		pad_poll.max_gap_us = now - pad_poll.last_us;
	pad_poll.last_us = now;
	pad_poll.polls++;
	pif_edges_log();
	if (ioPadGetInfo(&info) != 0) {
		/* A transient pad query failure must not leave a motor running. */
		for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++) {
			if (!rumble_applied[i])
				continue;
			padActParam act;
			memset(&act, 0, sizeof act);
			if (ioPadSetActDirect(i, &act) == PAD_OK)
				rumble_applied[i] = 0;
		}
		return 0;
	}
	bool intercepted = (info.info & 1) != 0;
	if (intercepted != pause_ctl.intercepted)
		salsa64_log("mandos: %s", intercepted ? "los tiene el sistema" : "de vuelta del sistema");
	bool sys = intercepted || pausectl_overlay(&pause_ctl);
	uint8_t rumble_wanted[SALSA64_PIF_PORTS];
	rumble_snapshot(rumble_wanted);
	for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++) {
		if (present[i] != !!info.status[i]) {
			present[i] = !!info.status[i];
			salsa64_log("mando %u %s", i + 1, present[i] ? "conectado" : "desconectado");
			last[i] = neutral;  /* nada pegado */
			if (!present[i])
				rumble_applied[i] = 0; /* volver a aplicar si el mando se reconecta */
		}
		if (info.status[i] && ioPadGetData(i, &data) == 0 && data.len > 0 && !sys)
			last[i] = ds3_from(&data);
		if (sys)
			last[i] = neutral;

		/* El Rumble Pak de N64 es binario: se representa con el motor grande
		 * del DualShock 3 a plena fuerza; el pequeño queda siempre apagado. */
		bool rumble_ui_paused = pause_ctl.dialog || pause_ctl.panel || pause_ctl.menu || pause_ctl.image || pause_ctl.accessory;
		bool want_rumble = info.status[i] && !sys && !rumble_ui_paused && rumble_wanted[i];
		if (want_rumble != !!rumble_applied[i]) {
			padActParam act;
			memset(&act, 0, sizeof act);
			if (want_rumble)
				act.large_motor = 0xFF;
			if (ioPadSetActDirect(i, &act) == PAD_OK)
				rumble_applied[i] = (uint8_t)want_rumble;
		}
		raw[i] = info.status[i] ? last[i].buttons : 0;
		stick[i] = info.status[i] ? stick_dirs(stick[i], last[i].lx - 128, last[i].ly - 128) : 0;
		pad_stick_dirs |= stick[i];
	}
	bool no_cheats = !ra_rules_allowed(ra_hardcore_active(), RA_RULE_CHEATS);  /* Hardcore: los trucos no valen */
	pause_ctl.panel_avail = game_cheats.n > 0 && !no_cheats;
	pause_ctl.panel_rows = no_cheats ? 0 : game_cheats.n;
	enum pausectl_action act = pausectl_pads(&pause_ctl, raw, intercepted, combos_on != 0, game);
	cheat_panel_events();
	pause_menu_events();
	accessory_menu_events();
	image_events();
	for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++) {
		struct salsa64_pad p = { 0, 0, 0, i == 0 };
		if (info.status[i]) {
			/* Los sticks del DS3 tal como llegan, para revisar el mapeo. */
			const struct salsa64_ds3 *a = &last[i], *b = &raw_logged[i];
			if (abs(a->lx - b->lx) >= 40 || abs(a->ly - b->ly) >= 40 ||
			    abs(a->rx - b->rx) >= 40 || abs(a->ry - b->ry) >= 40) {
				salsa64_log("mando %u: stick izquierdo (%u,%u), derecho (%u,%u)", i + 1,
					    a->lx, a->ly, a->rx, a->ry);
				raw_logged[i] = *a;
			}
			struct salsa64_ds3 d = last[i];
			d.buttons = pad_combos(i, game[i]);
			salsa64_padmap_apply(&padmap, &d, &p);
			all |= game[i];
		}
		pads[i] = p;

		/* Al log: conexión y botones; el stick, si se movió bastante. */
		struct salsa64_pad *o = &logged[i];
		int dx = p.x - o->x, dy = p.y - o->y;
		if (p.connected != o->connected || p.buttons != o->buttons ||
		    dx * dx + dy * dy >= 40 * 40) {
			char desc[128];
			/* El campo que emulación tenía en curso, para cotejar con el
			 * «PIF → juego» del mismo botón. */
			if (p.connected) {
				salsa64_pad_describe(&p, desc, sizeof desc);
				salsa64_log("control %u: %s (campo %u)", i + 1, desc, n64.bus.vi.fields);
			} else {
				salsa64_log("control %u: desconectado (campo %u)", i + 1, n64.bus.vi.fields);
			}
			*o = p;
		}
	}
	pads_publish(pads);
	if (buttons)
		*buttons = all;
	if (act == PAUSECTL_RESET) {
		salsa64_log("reinicio: botón Reset pedido desde el menú de pausa (campo %u)", n64.bus.vi.fields);
		lwsync();
		soft_reset_req = 1;
	} else if (act == PAUSECTL_RESTART) {
		salsa64_log("reinicio: desde cero pedido desde el menú de pausa (campo %u)", n64.bus.vi.fields);
		restart_cold = 1;
	}
	return act == PAUSECTL_EXIT || act == PAUSECTL_RESTART;
}

/* ---- ROM ------------------------------------------------------------------ */

static int file_exists(const char *path)
{
	FILE *f = fopen(path, "r");
	if (f)
		fclose(f);
	return f != NULL;
}

/* Tamaño de página (KB) de la memoria donde cae `p`, por la syscall 351
 * (platform/ps3/rec_cache.h); 0 si falla. */
static unsigned page_kb(const void *p)
{
	struct salsa64_ps3_page_attr a;
	return salsa64_ps3_page_attr(p, &a) == 0 ? a.page_size / 1024 : 0;
}

/* Texto del ISViewer: al log línea por línea (hilo de emulación). */
static char isv_line[256];
static uint32_t isv_len;

static void isv_out(void *ctx, const uint8_t *text, uint32_t len)
{
	char *line = isv_line;
	uint32_t n = isv_len;
	(void)ctx;
	if (selftest_on)
		selftest_isv(text, len);
	if (gfxtest_on)
		gfxtest_isv(text, len);
	if (audiotest_on)
		audiotest_isv(text, len);
	for (uint32_t i = 0; i < len; i++) {
		if (text[i] == '\n' || n == sizeof isv_line - 1) {
			line[n] = '\0';
			salsa64_log("rom: %s", line);
			n = 0;
		}
		if (text[i] != '\n')
			line[n++] = (char)text[i];
	}
	isv_len = n;
}

/* La ROM va fuera del heap de newlib, en páginas de 1 MB que se piden al
 * sistema al abrir el juego y se devuelven al cerrarlo. En el heap, lo que el
 * primer juego reserva «para siempre» (la foto de los estados, el perfilado,
 * las tablas de la interpolación…) queda por encima de su ROM: al soltarla,
 * el heap no puede devolver el hueco (el tope está ocupado) ni juntarlo con el
 * resto, y la ROM de 64 MB siguiente ya no cabe aunque sobre memoria. Las
 * páginas del sistema no dependen de eso (docs/memory-budget.md). */
static void *rom_pages;  /* la ROM en curso, si salió de las páginas del sistema */
static bool rom_no_memory;  /* fallo de reserva en la lectura actual, no de E/S */
static void heap_report(const char *when);
static bool menu_release_memory(void);

static void *rom_alloc(size_t len)
{
	size_t bytes = (len + (1u << 20) - 1) & ~((size_t)(1u << 20) - 1);
	sys_mem_addr_t addr = 0;
	if (!rom_pages && sysMemoryAllocate(bytes, SYS_MEMORY_PAGE_SIZE_1M, &addr) == 0) {
		rom_pages = (void *)(uintptr_t)addr;
		return rom_pages;
	}
	if (!rom_pages) {
		/* No cabe: se dice cuánto había y se reintenta una vez tras soltar lo que aún tenga el menú. */
		heap_report("sin páginas para la ROM");
		if (menu_release_memory() && sysMemoryAllocate(bytes, SYS_MEMORY_PAGE_SIZE_1M, &addr) == 0) {
			rom_pages = (void *)(uintptr_t)addr;
			salsa64_log("ROM de %u MB: cupo en páginas de 1 MB al soltar lo del menú", (unsigned)(bytes >> 20));
			return rom_pages;
		}
	}
	salsa64_log("ROM de %u MB: sin páginas de 1 MB del sistema, va en el heap", (unsigned)(bytes >> 20));
	void *p = memalign(128, len);
	if (!p)
		rom_no_memory = true;
	return p;
}

static FILE *rom_fp;             /* ROM por fragmentos (más abajo): el archivo abierto, */
static uint8_t *rom_head;        /* su cabecera de 4 KB, que es lo que ve el resto del frontend, */
static struct salsa64_rompg rom_pg;  /* y la caché de páginas */
static void rom_free(void *p)
{
	if (p && p == rom_head) {  /* ROM por fragmentos: la cabecera, el archivo y la caché */
		salsa64_rompg_close(&rom_pg);
		if (rom_fp)
			fclose(rom_fp);
		rom_fp = NULL;
		free(rom_head);
		rom_head = NULL;
		if (rom_pages) {
			sysMemoryFree((sys_mem_addr_t)(uintptr_t)rom_pages);
			rom_pages = NULL;
		}
		return;
	}
	if (p && p == rom_pages) {
		sysMemoryFree((sys_mem_addr_t)(uintptr_t)p);
		rom_pages = NULL;
	} else {
		free(p);
	}
}

/* Memoria para el log: el heap de newlib (mallinfo), hasta dónde llegó
 * (sbrk), lo que queda de la memoria de usuario del sistema (syscall 352, sin
 * envoltura en PSL1GHT) y la mayor reserva que cabe hoy: en el heap (un
 * bloque contiguo) y en páginas del sistema, que es de donde sale la ROM.
 * Se prueba reservando y soltando. */
static unsigned fit_mb(bool pages)
{
	unsigned lo = 0, hi = ROM_MAX >> 20;  /* más de una ROM no hace falta */
	while (lo < hi) {
		unsigned mid = (lo + hi + 1) / 2;
		bool ok;
		if (pages) {
			sys_mem_addr_t a = 0;
			ok = sysMemoryAllocate((size_t)mid << 20, SYS_MEMORY_PAGE_SIZE_1M, &a) == 0;
			if (ok)
				sysMemoryFree(a);
		} else {
			void *q = memalign(128, (size_t)mid << 20);
			ok = q != NULL;
			free(q);
		}
		if (ok)
			lo = mid;
		else
			hi = mid - 1;
	}
	return lo;
}

static void heap_report(const char *when)
{
	static struct {
		u32 total;
		u32 available;
	} um __attribute__((aligned(16)));
	struct mallinfo mi = mallinfo();
	lv2syscall1(352, (u64)(uintptr_t)&um);
	bool um_ok = (s32)p1 == 0;
	void *top = sbrk(0);
	unsigned heap_fit = fit_mb(false), pages_fit = fit_mb(true);
	salsa64_log("memoria %s: heap %d KB (%d en uso, %d libres en %d bloques, tope %p), usuario %u de %u KB libres, "
		    "cabe en el heap una reserva de %u MB y en páginas del sistema, %u MB",
		    when, mi.arena >> 10, mi.uordblks >> 10, mi.fordblks >> 10, mi.ordblks, top,
		    um_ok ? um.available >> 10 : 0, um_ok ? um.total >> 10 : 0, heap_fit, pages_fit);
}

/* ROM por fragmentos (docs/memory-budget.md): el archivo queda abierto, el núcleo pide páginas de 256 KB a una caché con
 * LRU que sale de páginas del sistema, y lo único que devuelve read_rom_file es la cabecera de 4 KB (rom_head). */
static uint64_t clock_us(void);

static int rom_rd(void *user, uint64_t off, uint8_t *dst, size_t len)
{
	FILE *f = user;
	if (fseek(f, (long)off, SEEK_SET) != 0)
		return -1;
	return (int)fread(dst, 1, len, f);
}

/* Caché de `*bytes` (múltiplo de 1 MB) en páginas del sistema, bajando a la mitad hasta 4 MB si no hay tanto. */
static void *rom_cache_alloc(size_t *bytes)
{
	const size_t floor_b = *bytes < (4u << 20) ? *bytes : (4u << 20);
	for (size_t b = *bytes; b >= floor_b; b >>= 1) {
		sys_mem_addr_t addr = 0;
		if (!rom_pages && sysMemoryAllocate(b, SYS_MEMORY_PAGE_SIZE_1M, &addr) == 0) {
			rom_pages = (void *)(uintptr_t)addr;
			*bytes = b;
			return rom_pages;
		}
	}
	return NULL;
}

static unsigned rom_cache_mb_cfg(void)
{
	unsigned mb = ROM_CACHE_MB;
	FILE *f = fopen(ROM_CACHE_CFG, "r");
	if (f) {
		unsigned v = 0;
		if (fscanf(f, "%u", &v) == 1 && v >= 1 && v <= 128)
			mb = v;
		fclose(f);
	}
	return mb;
}

/* Abre `path` por fragmentos; devuelve la cabecera (rom_head) o NULL sin dejar nada reservado. */
static uint8_t *rom_open_paged(FILE *f, const char *path, uint32_t len, bool forced)
{
	size_t cache_bytes = (size_t)rom_cache_mb_cfg() << 20;
	if (cache_bytes > (((size_t)len + (1u << 20) - 1) & ~((size_t)(1u << 20) - 1)))
		cache_bytes = ((size_t)len + (1u << 20) - 1) & ~((size_t)(1u << 20) - 1);
	uint8_t *head = memalign(128, ROM_HEAD);
	void *cache = head ? rom_cache_alloc(&cache_bytes) : NULL;
	if (!head || !cache || fseek(f, 0, SEEK_SET) != 0 || fread(head, 1, ROM_HEAD, f) != ROM_HEAD) {
		rom_no_memory = !head || !cache;
		salsa64_log("ROM %s: no pude preparar la lectura por fragmentos (caché de %u MB)", path,
			    (unsigned)(cache_bytes >> 20));
		if (cache) {
			sysMemoryFree((sys_mem_addr_t)(uintptr_t)rom_pages);
			rom_pages = NULL;
		}
		free(head);
		return NULL;
	}
	setvbuf(f, NULL, _IONBF, 0);  /* lecturas de 256 KB directas, sin pasar por el búfer de stdio */
	enum salsa64_rom_format fmt = salsa64_rom_normalize(head, ROM_HEAD);
	errno = 0;
	if (salsa64_rompg_open(&rom_pg, rom_rd, f, len, fmt, ROM_PAGE_SHIFT, cache, cache_bytes) != 0) {
		rom_no_memory = errno == ENOMEM;
		sysMemoryFree((sys_mem_addr_t)(uintptr_t)rom_pages);
		rom_pages = NULL;
		free(head);
		salsa64_log("ROM %s: no pude abrir la caché de páginas", path);
		return NULL;
	}
	rom_pg.clock_us = clock_us;
	if (file_exists(ROM_SYNC_FLAG))
		salsa64_log("ROM %s: rom-lectura-sincrona, sin hilo lector", path);
	else if (salsa64_rompg_async_start(&rom_pg, ROM_DEPTH) != 0)
		salsa64_log("ROM %s: no pude crear el hilo lector, la lectura queda síncrona", path);
	rom_fp = f;
	rom_head = head;
	salsa64_log("ROM %s: %u bytes, por fragmentos (páginas de %u KB, caché de %u MB = %u páginas de %u en total)%s", path,
		    (unsigned)len, 1u << (ROM_PAGE_SHIFT - 10), (unsigned)(cache_bytes >> 20), rom_pg.nslots,
		    rom_pg.npages, forced ? " [forzado por rom-paginada]" : "");
	return head;
}

/* Lee una ROM entera: en `buf` (de `cap` bytes) o, con buf NULL, en memoria reservada a su medida (rom_alloc; se suelta
 * con rom_free). Con buf NULL, una ROM de más de ROM_MAX (o con el archivo rom-paginada) se abre por fragmentos y lo que
 * devuelve es su cabecera. NULL si no se pudo o no mide entre 0x1000 bytes y ROM_MAX (o cap; ROM_FILE_MAX por
 * fragmentos). */
static uint8_t *read_rom_file(const char *path, uint8_t *buf, uint32_t cap, uint32_t *size)
{
	rom_no_memory = false;
	/* ZIP: se inspecciona la entrada elegida antes de reservar memoria, para que
	 * el archivo no necesite un segundo búfer grande. La extracción completa
	 * verifica el flujo Deflate, el tamaño descomprimido y el CRC-32 antes de
	 * que el juego vea nada. */
	if (salsa64_ziprom_is_path(path)) {
		uint8_t header[SALSA64_ROM_HEADER_SIZE];
		uint32_t rom_len = 0, actual_len = 0;
		uint32_t limit = buf ? cap : ROM_MAX;
		int rc = salsa64_ziprom_read_header(path, header, &rom_len, NULL, 0);
		if (rc != 0 || rom_len < 0x1000u || rom_len > limit || rom_len > SALSA64_ZIPROM_MAX_SIZE) {
			rom_no_memory = rc == SALSA64_ZIPROM_NOMEM;
			return NULL;
		}
		uint8_t *dst = buf ? buf : rom_alloc(rom_len);
		if (!dst)
			return NULL;
		/* Ajusta el límite del extractor a la memoria realmente reservada.
		 * El archivo se abre dos veces (vista previa y extracción), así que
		 * debe seguir siendo seguro aunque cambie entre las dos lecturas. */
		uint32_t dst_cap = buf ? cap : rom_len;
		rc = salsa64_ziprom_extract(path, dst, dst_cap, &actual_len, NULL, 0);
		if (rc != 0 || actual_len != rom_len) {
			rom_no_memory = rc == SALSA64_ZIPROM_NOMEM;
			if (!buf)
				rom_free(dst);
			return NULL;
		}
		*size = actual_len;
		return dst;
	}

	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	long len = -1;
	if (fseek(f, 0, SEEK_END) == 0)
		len = ftell(f);
	uint8_t *dst = NULL;
	bool forced = !buf && file_exists(ROM_PAGED_FLAG);
	if (!buf && len >= 0x1000 && (uint64_t)len <= ROM_FILE_MAX && (forced || (uint64_t)len > ROM_MAX) && !rom_head) {
		dst = rom_open_paged(f, path, (uint32_t)len, forced);
		if (dst) {
			*size = (uint32_t)len;
			return dst;  /* el archivo queda abierto hasta rom_free */
		}
	} else if (len >= 0x1000 && (uint32_t)len <= (buf ? cap : ROM_MAX) && fseek(f, 0, SEEK_SET) == 0) {
		dst = buf ? buf : rom_alloc((size_t)len);
		if (dst && fread(dst, 1, (size_t)len, f) != (size_t)len) {
			if (!buf)
				rom_free(dst);
			dst = NULL;
		}
	}
	fclose(f);
	if (dst)
		*size = (uint32_t)len;
	return dst;
}

/* CRC-32 de cada megabyte de la ROM tal como está en memoria (ya en orden z64), para el log: si
 * un juego lee datos malos del cartucho en la PS3 (Killer Instinct e Indiana Jones se quedaban
 * descomprimiendo o dibujando basura tras cargar un nivel y no en el host), comparar con los de
 * la misma ROM en la Mac dice si el archivo de la consola o su lectura a memoria cambió algo.
 * Con tabla de 256 entradas: ~0,15 s para 32 MB. */
static void log_rom_crcs(const uint8_t *rom, uint32_t size)
{
	static uint32_t tab[256];
	if (!tab[1]) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++)
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			tab[i] = c;
		}
	}
	uint32_t all = 0xFFFFFFFFu;
	char line[600];
	unsigned n = 0, first = 0;
	line[0] = '\0';
	for (uint32_t mb = 0; mb < size; mb += 1u << 20) {
		uint32_t end = size - mb < (1u << 20) ? size : mb + (1u << 20), c = 0xFFFFFFFFu;
		for (uint32_t i = mb; i < end; i++) {
			c = tab[(c ^ rom[i]) & 0xFF] ^ (c >> 8);
			all = tab[(all ^ rom[i]) & 0xFF] ^ (all >> 8);
		}
		n += (unsigned)snprintf(line + n, sizeof line - n, " %08X", ~c);
		if (n > sizeof line - 24 || end == size) {
			salsa64_log("ROM: CRC-32 por megabyte desde el %u:%s", first, line);
			line[0] = '\0';
			n = 0;
			first = (mb >> 20) + 1;
		}
	}
	salsa64_log("ROM: CRC-32 de los %u bytes en memoria: %08X", size, ~all);
}

static uint8_t *embedded(const uint8_t *bin, uint32_t bin_size, uint32_t *size)
{
	uint8_t *buf = memalign(128, bin_size);
	if (buf) {
		memcpy(buf, bin, bin_size);
		*size = bin_size;
	}
	return buf;
}

/* ROM embebida elegida en EMBED_PATH, o NULL. */
static uint8_t *load_embedded(uint32_t *size, const char **source)
{
	char name[80] = "";
	FILE *f = fopen(EMBED_PATH, "r");
	if (!f)
		return NULL;
	if (!fgets(name, sizeof name, f))
		name[0] = '\0';
	fclose(f);
	name[strcspn(name, " \r\n")] = '\0';
	if (!strcmp(name, "vitest")) {
		*source = "vitest (embebida)";
		selftest_on = 1;
		return embedded(vitest_rom_bin, vitest_rom_bin_size, size);
	}
	if (!strcmp(name, "gfxtest")) {
		*source = "gfxtest (embebida)";
		gfxtest_on = 1;
		return embedded(gfxtest_rom_bin, gfxtest_rom_bin_size, size);
	}
	if (!strcmp(name, "audiotest")) {
		*source = "audiotest (embebida)";
		audiotest_on = 1;
		return embedded(audiotest_rom_bin, audiotest_rom_bin_size, size);
	}
	if (!strncmp(name, "file:", 5)) {
		static char path[96];
		snprintf(path, sizeof path, SALSA64_DATA_DIR "/%s", name + 5);
		uint8_t *buf = read_rom_file(path, NULL, 0, size);
		if (buf) {
			*source = path;
			if (!strncmp(name + 5, "salsa64-gfxtest", 15))
				gfxtest_on = 1;
			return buf;
		}
		salsa64_log("no se pudo leer %s", path);
		return NULL;
	}
	salsa64_log("%s dice \"%s\": no es vitest, gfxtest, audiotest ni file:<nombre>", EMBED_PATH, name);
	return NULL;
}

/* ROM del disco (preasignada una sola vez) o la embebida. */
static uint8_t *load_rom(uint32_t *size, const char **source)
{
	uint8_t *rom = load_embedded(size, source);
	if (rom)
		return rom;
	if (file_exists(ROM_PATH)) {
		if ((rom = read_rom_file(ROM_PATH, NULL, 0, size))) {
			*source = ROM_PATH;
			return rom;
		}
		salsa64_log("no se pudo leer %s: uso la ROM de prueba", ROM_PATH);
	}
	*source = "gfxtest (embebida)";
	gfxtest_on = 1;
	return embedded(gfxtest_rom_bin, gfxtest_rom_bin_size, size);
}

/* SALSA64_DATA_DIR/lle: ausente = sin LLE; "all" = todo el microcódigo;
 * cualquier otra cosa = solo lo que no tiene HLE. */
static enum salsa64_lle_mode lle_config(void)
{
	FILE *f = fopen(LLE_PATH, "r");
	if (!f)
		return SALSA64_LLE_OFF;
	char word[16] = "";
	if (fscanf(f, "%15s", word) != 1)
		word[0] = '\0';
	fclose(f);
	return strcmp(word, "all") == 0 ? SALSA64_LLE_ALL : SALSA64_LLE_AUTO;
}

/* Recorte del overscan: los bordes de la imagen que una tele de tubo no
 * muestra (y donde los juegos dejan basura: GoldenEye rellena de azul la
 * primera y la última columna). En píxeles de la N64 a 320 × 240, por
 * lado; se escala con la imagen del VI (a 640 de ancho recorta el doble).
 * Por omisión 8 y 6 (2,5 % por lado); overscan.cfg lo cambia con un
 * número (todos los lados), dos (horizontal, vertical) o cuatro
 * (izquierda, derecha, arriba, abajo); 0 lo apaga. Las ROMs de prueba se
 * presentan enteras (sus verificaciones leen la pantalla por posición). */
static struct { uint32_t left, right, top, bottom; } overscan = { 8, 8, 6, 6 };

static void load_overscan(void)
{
	overscan.left = overscan.right = 8;  /* por omisión (un juego con ajustes propios los cambió) */
	overscan.top = overscan.bottom = 6;
	FILE *f = fopen(OVERSCAN_PATH, "r");
	if (!f) {
		salsa64_log("overscan: recorte %u %u %u %u por omisión (no hay %s)", overscan.left, overscan.right,
			    overscan.top, overscan.bottom, OVERSCAN_PATH);
		return;
	}
	unsigned v[4] = { 0, 0, 0, 0 };
	int n = fscanf(f, "%u %u %u %u", &v[0], &v[1], &v[2], &v[3]);
	fclose(f);
	for (int i = 0; i < 4; i++)
		if (v[i] > 64)
			v[i] = 64;
	if (n == 1)
		overscan.left = overscan.right = overscan.top = overscan.bottom = v[0];
	else if (n == 2)
		overscan.left = overscan.right = v[0], overscan.top = overscan.bottom = v[1];
	else if (n == 4)
		overscan.left = v[0], overscan.right = v[1], overscan.top = v[2], overscan.bottom = v[3];
	else
		salsa64_log("overscan: %s sin números; queda el recorte por omisión", OVERSCAN_PATH);
	salsa64_log("overscan: recorte %u %u %u %u (%s)", overscan.left, overscan.right, overscan.top,
		    overscan.bottom, OVERSCAN_PATH);
}

/* El recorte de overscan.cfg como opción del menú (Opciones). */
static int overscan_preset(void)
{
	uint32_t l = overscan.left, r = overscan.right, t = overscan.top, b = overscan.bottom;
	if (!l && !r && !t && !b)
		return MENU_OVERSCAN_OFF;
	if (l == 8 && r == 8 && t == 6 && b == 6)
		return MENU_OVERSCAN_NORMAL;
	if (l == 16 && r == 16 && t == 12 && b == 12)
		return MENU_OVERSCAN_WIDE;
	return MENU_OVERSCAN_CUSTOM;
}

static void overscan_save(int preset)
{
	const char *text = preset == MENU_OVERSCAN_OFF ? "0\n" : preset == MENU_OVERSCAN_WIDE ? "16 12\n" : "8 6\n";
	FILE *f = fopen(OVERSCAN_PATH, "w");
	bool ok = f && fputs(text, f) >= 0;
	if (f && fclose(f) != 0)
		ok = false;
	if (!ok)
		salsa64_log("menú: no se pudo escribir %s", OVERSCAN_PATH);
	load_overscan();
}

/* El recorte para una imagen del VI (0 con las ROMs de prueba). */
static struct salsa64_overscan overscan_for(const struct salsa64_vi_frame *f)
{
	struct salsa64_overscan c = { 0, 0, 0, 0 };
	if (selftest_on || gfxtest_on || audiotest_on || !f->width || !f->height)
		return c;
	c.left = overscan.left * f->width / 320;
	c.right = overscan.right * f->width / 320;
	c.top = overscan.top * f->height / 240;
	c.bottom = overscan.bottom * f->height / 240;
	if (c.left + c.right >= f->width)
		c.left = c.right = 0;
	if (c.top + c.bottom >= f->height)
		c.top = c.bottom = 0;
	return c;
}

/* ---- Hilo de emulación ------------------------------------------------------ */

/* Al terminar un campo: la imagen del VI (copiada a una ranura) y el
 * mensaje para presentación. Si hay un campo anterior sin consumir y no
 * lleva verificación, se marca para saltarlo y se le suelta la ranura. */
static uint32_t post_last = ~0u;  /* índice del último campo encolado */

/* Tareas gráficas que el RDP ya pudo dibujar, para presentación. Con el
 * RDP congelado (DPC_STATUS FREEZE) la tarea terminada no se ejecuta hasta
 * que el CPU lo descongela (rsp.c retiene su interrupción): Banjo-Kazooie
 * arranca así la del cuadro siguiente sobre el búfer que el VI todavía
 * muestra. Dibujarla antes en el RSX adelantaba ese cuadro un campo y luego
 * el otro búfer mostraba el anterior: los modelos temblaban. */
static volatile uint32_t tasks_visible;

static uint32_t visible_tasks(void)
{
	return salsa64_rdp_tasks_done() - (n64.bus.rsp.dp_held ? 1u : 0u);
}

static bool trace_vi_want(uint32_t field)
{
	return trace_vi_count && field >= trace_vi_first && field - trace_vi_first < trace_vi_count;
}

/* Escribe una sola decisión por campo de la ventana. Todos los datos del VI
 * vienen de `m`, que el hilo de emulación terminó antes de encolarlo. */
static void trace_vi_log(const struct field_msg *m, const char *decision, uint32_t behind,
			 const struct salsa64_rdp_present_info *p, const struct salsa64_rdp_field_info *fi,
			 const struct salsa64_overscan *crop, const rsxkit_rect *out)
{
	if (!trace_vi_want(m->field))
		return;
	const uint32_t *v = m->vi;
	if (p) {
		const char *kind = fi->kind == SALSA64_FIELD_INTERP ? "intermedio" :
			fi->kind == SALSA64_FIELD_REPEAT ? "repetido" : "verdadero";
		salsa64_log("vi campo %u: %s; tareas %u; origen %06x; imagen %ux%u paso %u bpp %u; vpos %u paridad %u; atraso %u; regs %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x; destino %06x ranura %d logico %ux%u escala %u edad %u; ventana %u,%u %ux%u; superficie %u,%u %ux%u; recorte %u,%u,%u,%u; salida %d,%d %ux%u; %s t=%.3f k=%u tareas %u/%u",
			    m->field, decision, m->tasks, m->f.origin, m->f.width, m->f.height, m->f.stride, m->f.bpp,
			    m->f.vpos, m->parity, behind, v[0], v[1], v[2], v[3], m->vi_current, v[5], v[6], v[7],
			    v[8], v[9], v[10], v[11], v[12], v[13], p->addr, p->slot, p->width, p->height, p->scale,
			    p->age, p->y0, 0u, p->w, p->h, p->src_x, p->src_y, p->src_w, p->src_h, crop->left,
			    crop->right, crop->top, crop->bottom, out->x, out->y, out->w, out->h, kind, fi->t, fi->k,
			    fi->task, fi->prev_task);
	} else {
		salsa64_log("vi campo %u: %s; tareas %u; origen %06x; imagen %ux%u paso %u bpp %u; vpos %u paridad %u; atraso %u; regs %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
			    m->field, decision, m->tasks, m->f.origin, m->f.width, m->f.height, m->f.stride, m->f.bpp,
			    m->f.vpos, m->parity, behind, v[0], v[1], v[2], v[3], m->vi_current, v[5], v[6], v[7],
			    v[8], v[9], v[10], v[11], v[12], v[13]);
	}
}

static void post_field(uint32_t flags)
{
	uint32_t last_idx = post_last;
	struct field_msg m;
	memset(&m, 0, sizeof m);
	m.field = n64.bus.vi.fields;
	m.tasks = visible_tasks();
	tasks_visible = m.tasks;
	m.snap = -1;
	m.flags = flags;
	memcpy(m.vi, n64.bus.vi.regs, sizeof m.vi);
	m.vi_current = salsa64_vi_read(&n64.bus, SALSA64_VI_V_CURRENT * 4);
	m.parity = n64.bus.vi.field & 1;

	if (salsa64_vi_frame(&n64.bus, &m.f) && m.f.width <= FB_MAX_W && m.f.height <= FB_MAX_H) {
		salsa64_rdp_vi(&m.f);  /* qué color images son framebuffers y qué alto necesitan */
		if (selftest_on && selftest_frame(&n64.bus, &m.f))
			m.flags |= FIELD_CHECK;
		if (cap_every && m.field % cap_every == 0)
			m.flags |= FIELD_SHOT;
		for (int i = 0; i < SNAP_SLOTS; i++) {
			if (snap_cas(i, SNAP_FREE, SNAP_WRITING)) {
				m.snap = i;
				break;
			}
		}
		if (m.snap >= 0) {
			uint64_t t0 = sysGetSystemTime();
			uint8_t *dst = snap_buf[m.snap];
			const uint8_t *src = n64.bus.rdram + m.f.origin;
			uint32_t line = m.f.width * m.f.bpp;
			if (m.f.stride == line) {
				memcpy(dst, src, line * m.f.height);
			} else {
				for (uint32_t y = 0; y < m.f.height; y++)
					memcpy(dst + y * line, src + y * m.f.stride, line);
			}
			emu_stats.us_copy += sysGetSystemTime() - t0;
		} else {
			emu_stats.no_slot++;
		}
	} else {
		m.f.width = 0;
	}

	uint32_t h = field_head;
	/* Con verificación pendiente esperamos lugar (solo en las pruebas);
	 * si no, un campo que no cabe se pierde: la emulación no espera. */
	while (h - field_tail >= FIELD_QUEUE && (m.flags & FIELD_KEEP) && running)
		sysUsleep(200);
	if (h - field_tail >= FIELD_QUEUE) {
		if (m.snap >= 0)
			snap_state[m.snap] = SNAP_FREE;
		emu_stats.dropped++;
		trace_vi_log(&m, "cola llena", 0, NULL, NULL, NULL, NULL);
		return;
	}
	if (last_idx != ~0u && (int32_t)(last_idx - field_tail) >= 0) {
		struct field_msg *o = &field_q[last_idx % FIELD_QUEUE];
		if (!(o->flags & FIELD_KEEP)) {
			o->skip = 1;
			if (o->snap >= 0)
				snap_cas(o->snap, SNAP_QUEUED, SNAP_FREE);  /* si presentación no la tomó ya */
		}
	}
	if (m.snap >= 0)
		snap_state[m.snap] = SNAP_QUEUED;
	field_q[h % FIELD_QUEUE] = m;
	lwsync();  /* el mensaje y la copia, antes de publicarlos */
	field_head = h + 1;
	post_last = h;
	wake();
}

/* Estadística de la HLE gráfica en el SPE por ventanas fijas de campos
 * (hilo de emulación, donde se suman los contadores del SPE). */
static struct {
	int on;
	uint32_t field0;
	uint64_t t0, worst;  /* inicio y el campo que más tardó en emularse (µs) */
	struct salsa64_gfx_spu spu;
	struct salsa64_rdp_stats rdp;
} gwin;

static void gfx_window(uint64_t now, uint64_t us_field)
{
	uint32_t field = n64.bus.vi.fields;
	if (us_field > gwin.worst)
		gwin.worst = us_field;
	if (!gwin.t0)
		gwin.t0 = now;
	if (field - gwin.field0 < GFX_WIN_FIELDS)
		return;
	struct salsa64_rdp_stats rs;
	salsa64_rdp_get_stats(&rs);
	const struct salsa64_gfx_spu *s = &gfx_spu, *s0 = &gwin.spu;
	uint32_t tasks = s->tasks - s0->tasks;
	if (tasks && now > gwin.t0) {
		double k = 1e3 / spukit_tick_hz() / tasks;
		salsa64_log("gfx campos %u–%u: %.1f campos/s (peor campo %.1f ms de emulación), %u tareas; por tarea: "
			    "%.0f dibujos, %.0f triángulos, %.1f texturas decodificadas (+%.1f reusadas, %.1f de tareas "
			    "anteriores; %.1f al arena, %.1f desalojadas; %.1f sin decodificar, desde un destino), %.1f "
			    "cargas a la TMEM (%.1f lecturas); SPE %.2f ms (vértices %.2f, estado %.2f, identificar texturas "
			    "%.2f, decodificar %.2f, cargas %.2f), PPE esperando %.2f ms, traducción al RSX %.2f ms",
			    gwin.field0, field - 1, (field - gwin.field0) * 1e6 / (now - gwin.t0), gwin.worst / 1e3, tasks,
			    (double)(rs.draws - gwin.rdp.draws) / tasks, (double)(rs.triangles - gwin.rdp.triangles) / tasks,
			    (double)(s->textures - s0->textures) / tasks, (double)(s->tex_reused - s0->tex_reused) / tasks,
			    (double)(s->tex_old - s0->tex_old) / tasks, (double)(s->tex_arena - s0->tex_arena) / tasks,
			    (double)(s->tex_evicted - s0->tex_evicted) / tasks, (double)(s->tex_target - s0->tex_target) / tasks,
			    (double)(s->loads - s0->loads) / tasks, (double)(s->load_reads - s0->load_reads) / tasks,
			    (s->spu_ticks - s0->spu_ticks) * k, (s->t_vtx - s0->t_vtx) * k, (s->t_state - s0->t_state) * k,
			    (s->t_hash - s0->t_hash) * k, (s->t_tex - s0->t_tex) * k, (s->t_load - s0->t_load) * k,
			    (s->wait_us - s0->wait_us) / 1e3 / tasks, (rs.us_translate - gwin.rdp.us_translate) / 1e3 / tasks);
		uint64_t samples = 0, half, median = 0, maximum = 0;
		bool median_set = false;
		for (unsigned i = 0; i < GFX_LATE_HIST_N; i++)
			samples += s->late_hist[i] - s0->late_hist[i];
		half = (samples + 1) / 2;
		uint64_t seen = 0;
		for (unsigned i = 0; i < GFX_LATE_HIST_N; i++) {
			uint32_t n = s->late_hist[i] - s0->late_hist[i];
			if (n) maximum = (uint64_t)i * GFX_LATE_BIN_CYCLES;
			seen += n;
			if (!median_set && n && seen >= half) {
				median = (uint64_t)i * GFX_LATE_BIN_CYCLES;
				median_set = true;
			}
		}
		salsa64_log("gfx IRQ SP: atraso mediana %llu, máximo %llu ciclos en %llu tareas; "
			    "el PPE esperó al SPE %u veces, %.2f ms en total",
			    (unsigned long long)median, (unsigned long long)maximum, (unsigned long long)samples,
			    s->waits - s0->waits, (s->wait_us - s0->wait_us) / 1e3);
		uint32_t xbus = s->xbus_runs - s0->xbus_runs;
		uint32_t tw = rs.tex_waits - gwin.rdp.tex_waits;
		if (xbus || tw)
			salsa64_log("gfx texturas: %u listas XBUS, %u llegaron con el SPE en vuelo y %u esperaron %.2f ms; "
			    "espera diagnóstica %u veces, %.2f ms en la ventana", xbus,
			    s->xbus_inflight - s0->xbus_inflight, s->xbus_waits - s0->xbus_waits,
			    (s->xbus_wait_us - s0->xbus_wait_us) / 1e3, tw,
			    (rs.us_tex_wait - gwin.rdp.us_tex_wait) / 1e3);
	}
	gwin.field0 = field;
	gwin.t0 = now;
	gwin.worst = 0;
	gwin.spu = *s;
	gwin.rdp = rs;
}

/* Tareas gráficas con un microcódigo sin HLE (terminan sin dibujar): al log
 * la primera y luego cada vez que se duplican, para no llenarlo. */
static uint32_t unknown_gbi_next = 1;

static void log_unknown_gbi(void)
{
	const struct salsa64_rsp_stats *st = &n64.bus.rsp.stats;
	if (st->unknown_gbi < unknown_gbi_next)
		return;
	char text[96];
	salsa64_rsp_ucode_text(&n64.bus, st->unknown_data, text, sizeof text);
	salsa64_log("RSP: %u tareas gráficas con microcódigo sin HLE (datos en 0x%08x: \"%s\"); terminan sin dibujar",
		    st->unknown_gbi, (unsigned)st->unknown_data, text);
	/* Para identificarlo cuando no trae texto (Animal Crossing, 2026-10): código y CRC-32. */
	uint32_t crc_b, crc_w, crc_d;
	if (salsa64_rsp_ucode_ident(&n64.bus, st->unknown_ucode, st->unknown_ucode_size, st->unknown_data,
				    st->unknown_data_size, &crc_b, &crc_w, &crc_d))
		salsa64_log("RSP: el microcódigo desconocido: código en 0x%08x (%u bytes), CRC-32 0x%08X de los bytes y 0x%08X de las "
			    "palabras del host; datos %u bytes, CRC-32 0x%08X",
			    (unsigned)st->unknown_ucode, (unsigned)st->unknown_ucode_size, (unsigned)crc_b, (unsigned)crc_w,
			    (unsigned)st->unknown_data_size, (unsigned)crc_d);
	else
		salsa64_log("RSP: el microcódigo desconocido: código en 0x%08x (%u bytes) fuera de la RDRAM; datos en 0x%08x (%u bytes)",
			    (unsigned)st->unknown_ucode, (unsigned)st->unknown_ucode_size, (unsigned)st->unknown_data,
			    (unsigned)st->unknown_data_size);
	unknown_gbi_next = st->unknown_gbi * 2;
}

/* Fondos prerrenderizados (Zelda OoT): las tareas JPEG de una sala llegan
 * en tanda al cargarla (la HLE corre en un SPE o, con JPEG_PPE_PATH, en el
 * hilo de emulación); al terminar la tanda (un campo sin tareas nuevas), al
 * log cuántas, cuánto tardaron y el tirón: los campos de la tanda y el más
 * lento de emulación (sin la espera del ritmo). */
static struct {
	uint32_t seen, tasks, mb, field0, fields;
	uint64_t us, us_max, spu_ticks, sleep_us, wait_us;
	uint32_t polls, waits, on_ppe;
} jpeg_log;

static uint64_t clock_us(void)
{
	return (uint64_t)sysGetSystemTime();
}

static void log_jpeg(uint64_t us_field)
{
	const struct salsa64_rsp_stats *st = &n64.bus.rsp.stats;
	const struct salsa64_jpeg_spu *js = &jpeg_spu;
	if (st->jpeg < jpeg_log.tasks)
		memset(&jpeg_log, 0, sizeof jpeg_log);  /* otro juego */
	if (st->jpeg != jpeg_log.seen) {
		/* Un campo de la tanda. */
		if (!jpeg_log.fields++)
			jpeg_log.field0 = n64.bus.vi.fields;
		if (us_field > jpeg_log.us_max)
			jpeg_log.us_max = us_field;
	} else if (st->jpeg != jpeg_log.tasks) {
		uint32_t tasks = st->jpeg - jpeg_log.tasks;
		if (jpeg_on_spu)
			salsa64_log("JPEG: %u tareas, %u macrobloques en el SPE: %.2f ms (la más larga %.3f ms), %u devueltas al "
				    "PPE; hilo de emulación %.2f ms (%u esperas de %.2f ms, %u sondeos, %.1f ms dormido); "
				    "campos %u–%u, el más lento %.1f ms de emulación; %u con parámetros malos en total",
				    tasks, st->jpeg_mb - jpeg_log.mb, (js->spu_ticks - jpeg_log.spu_ticks) * 1e3 / spukit_tick_hz(),
				    js->spu_max * 1e3 / spukit_tick_hz(), js->on_ppe - jpeg_log.on_ppe,
				    (st->jpeg_us - jpeg_log.us) / 1e3, js->waits - jpeg_log.waits,
				    (js->wait_us - jpeg_log.wait_us) / 1e3, js->polls - jpeg_log.polls,
				    (js->sleep_us - jpeg_log.sleep_us) / 1e3, jpeg_log.field0,
				    jpeg_log.field0 + jpeg_log.fields - 1, jpeg_log.us_max / 1e3, st->jpeg_bad);
		else
			salsa64_log("JPEG: %u tareas, %u macrobloques en %.2f ms del PPE (hilo de emulación); campos %u–%u, el "
				    "más lento %.1f ms de emulación; %u con parámetros malos en total",
				    tasks, st->jpeg_mb - jpeg_log.mb, (st->jpeg_us - jpeg_log.us) / 1e3, jpeg_log.field0,
				    jpeg_log.field0 + jpeg_log.fields - 1, jpeg_log.us_max / 1e3, st->jpeg_bad);
		jpeg_log.tasks = st->jpeg;
		jpeg_log.mb = st->jpeg_mb;
		jpeg_log.us = st->jpeg_us;
		jpeg_log.spu_ticks = js->spu_ticks;
		jpeg_log.sleep_us = js->sleep_us;
		jpeg_log.wait_us = js->wait_us;
		jpeg_log.polls = js->polls;
		jpeg_log.waits = js->waits;
		jpeg_log.on_ppe = js->on_ppe;
		jpeg_log.fields = 0;
		jpeg_log.us_max = 0;
	}
	jpeg_log.seen = st->jpeg;
}

/* Video de Resident Evil 2 (RESIZE, DECODE y FILL; rsp/jpeg/re2.c): llegan sin parar mientras
 * suena una cinemática, así que se informa por ventanas de VIDEO_LOG_TASKS tareas: cuántas de
 * cada tipo, cuánto tardaron en el SPE (o en el hilo de emulación, si van al PPE), la más
 * larga, lo que esperó el PPE y el campo más lento de la ventana. */
#define VIDEO_LOG_TASKS 150u
static struct {
	uint32_t seen, tasks, field0, fields, polls, waits;
	uint32_t k[3], rows[3], on_ppe[3];
	uint64_t ticks[3], us, us_max, k_us[3], wait_us, sleep_us;
} video_log;

static void log_video(uint64_t us_field)
{
	const struct salsa64_rsp_stats *st = &n64.bus.rsp.stats;
	const struct salsa64_jpeg_spu *js = &jpeg_spu;
	if (st->video < video_log.tasks)
		memset(&video_log, 0, sizeof video_log);  /* otro juego */
	if (st->video != video_log.seen) {
		if (!video_log.fields++)
			video_log.field0 = n64.bus.vi.fields;
		if (us_field > video_log.us_max)
			video_log.us_max = us_field;
	}
	video_log.seen = st->video;
	if (st->video - video_log.tasks < VIDEO_LOG_TASKS)
		return;
	const double hz = jpeg_on_spu ? spukit_tick_hz() : 1.0;
	uint32_t n[3];
	double ms[3], max[3];
	for (int k = 0; k < 3; k++) {
		n[k] = st->video_k[k] - video_log.k[k];
		ms[k] = jpeg_on_spu ? (js->v_ticks[k] - video_log.ticks[k]) * 1e3 / hz : (st->video_k_us[k] - video_log.k_us[k]) / 1e3;
		max[k] = jpeg_on_spu ? js->v_max[k] * 1e3 / hz : 0.0;
	}
	if (jpeg_on_spu)
		salsa64_log("video: %u tareas (RESIZE %u, DECODE %u, FILL %u; %u, %u y %u filas) en el SPE: %.2f, %.2f y %.2f ms "
			    "(la más larga desde el arranque %.3f, %.3f y %.3f ms), %u devueltas al PPE; hilo de emulación %.2f ms "
			    "(%u esperas de %.2f ms, %u sondeos, %.1f ms dormido); campos %u–%u, el más lento %.1f ms de emulación; "
			    "%u con parámetros malos y %u sin HLE en total",
			    st->video - video_log.tasks, n[0], n[1], n[2], st->video_k_rows[0] - video_log.rows[0],
			    st->video_k_rows[1] - video_log.rows[1], st->video_k_rows[2] - video_log.rows[2], ms[0], ms[1], ms[2],
			    max[0], max[1], max[2], js->v_on_ppe[0] + js->v_on_ppe[1] + js->v_on_ppe[2] -
			    video_log.on_ppe[0] - video_log.on_ppe[1] - video_log.on_ppe[2], (st->video_us - video_log.us) / 1e3,
			    js->waits - video_log.waits, (js->wait_us - video_log.wait_us) / 1e3, js->polls - video_log.polls,
			    (js->sleep_us - video_log.sleep_us) / 1e3, video_log.field0, video_log.field0 + video_log.fields - 1,
			    video_log.us_max / 1e3, st->video_bad, st->video_unsup);
	else
		salsa64_log("video: %u tareas (RESIZE %u, DECODE %u, FILL %u; %u, %u y %u filas) en el PPE (hilo de emulación): "
			    "%.2f, %.2f y %.2f ms; campos %u–%u, el más lento %.1f ms de emulación; %u con parámetros malos y %u sin "
			    "HLE en total", st->video - video_log.tasks, n[0], n[1], n[2], st->video_k_rows[0] - video_log.rows[0],
			    st->video_k_rows[1] - video_log.rows[1], st->video_k_rows[2] - video_log.rows[2], ms[0], ms[1], ms[2],
			    video_log.field0, video_log.field0 + video_log.fields - 1, video_log.us_max / 1e3, st->video_bad,
			    st->video_unsup);
	video_log.tasks = st->video;
	video_log.us = st->video_us;
	video_log.wait_us = js->wait_us;
	video_log.sleep_us = js->sleep_us;
	video_log.polls = js->polls;
	video_log.waits = js->waits;
	for (int k = 0; k < 3; k++) {
		video_log.k[k] = st->video_k[k];
		video_log.rows[k] = st->video_k_rows[k];
		video_log.k_us[k] = st->video_k_us[k];
		video_log.ticks[k] = js->v_ticks[k];
		video_log.on_ppe[k] = js->v_on_ppe[k];
	}
	video_log.fields = 0;
	video_log.us_max = 0;
}

/* ---- Perfilado por campo (perfilado_ps3.h; hilo de emulación) ----------------------
 * Lo que costó cada campo y por qué: si la emulación del campo pasa del
 * umbral (tiron_ms), un registro TIRON con la traducción, las esperas al SPE
 * y al RSX, la copia del VI, JPEG y el muestreo de ese campo. */
struct perf_snap {
	uint64_t cycles, idle, spe_wait, spe_sleep, set_wait, rb_wait, copy, jpeg, interp, smp_ticks, invalidated, pi;
	uint32_t gfx, audio, marks[SALSA64_SMP_N];
};
static uint64_t perf_compile_ticks;  /* total compilando (para las ventanas de stats) */
static volatile uint32_t perf_worst_us; /* el campo más largo de la ventana (la reinicia perf_window) */

static void perf_snap_take(struct perf_snap *s)
{
	s->cycles = n64.cpu.cycles;
	s->idle = n64.cpu.idle_cycles;
	s->spe_wait = gfx_spu.wait_us + jpeg_spu.wait_us;
	s->spe_sleep = gfx_spu.sleep_us + jpeg_spu.sleep_us;
	salsa64_rdp_get_waits(&s->set_wait, &s->rb_wait);
	s->copy = emu_stats.us_copy;
	s->jpeg = n64.bus.rsp.stats.jpeg_us + n64.bus.rsp.stats.video_us;  /* JPEG y video de RE2 */
	s->interp = use_rec ? rec.stats.interp_steps : 0;
	s->invalidated = use_rec ? rec.stats.invalidated : 0;
	s->pi = n64.bus.pi_dma_bytes;
	s->gfx = n64.bus.rsp.stats.gfx;
	s->audio = n64.bus.rsp.stats.audio;
	s->smp_ticks = rsmp.on ? rec_sample->ticks : 0;
	for (unsigned m = 0; m < SALSA64_SMP_N; m++)
		s->marks[m] = rsmp.on ? rec_sample->marks[m] : 0;
	if (use_rec && rec.trace)
		memset(&rec.trace->field, 0, sizeof rec.trace->field);
}

static void perf_field_end(const struct perf_snap *a, uint64_t us_emu, uint64_t tb_ovl)
{
	struct salsa64_rec_trace_field tf;
	memset(&tf, 0, sizeof tf);
	if (use_rec && rec.trace) {
		tf = rec.trace->field;
		perf_compile_ticks += tf.compile_ticks;
		if (tf.compiled > rec.trace->compiles.max_field_compiles) {
			rec.trace->compiles.max_field_compiles = tf.compiled;
			rec.trace->compiles.max_field = n64.bus.vi.fields;
		}
		perf_app_field(&rec, n64.bus.vi.field_cycles);
	}
	if (us_emu > perf_worst_us)
		perf_worst_us = (uint32_t)us_emu;
	if (us_emu < (uint64_t)perf.cfg.stall_ms * 1000u)
		return;
	struct perf_snap b;
	perf_snap_take(&b);
	uint64_t hz = spukit_tick_hz();
	uint32_t v[PERF_S_N];
	memset(v, 0, sizeof v);
	v[PERF_S_campo] = n64.bus.vi.fields;
	v[PERF_S_emu_us] = (uint32_t)us_emu;
	v[PERF_S_ciclos] = (uint32_t)(b.cycles - a->cycles);
	v[PERF_S_ciclos_espera] = (uint32_t)(b.idle - a->idle);
	v[PERF_S_compilados] = tf.compiled;
	v[PERF_S_compilar_us] = (uint32_t)(hz ? tf.compile_ticks * 1000000u / hz : 0);
	v[PERF_S_palabras] = tf.words;
	v[PERF_S_retrad] = tf.retrans;
	v[PERF_S_desalojos] = tf.evictions;
	v[PERF_S_vaciados] = tf.flushes;
	v[PERF_S_muertos] = tf.killed;
	v[PERF_S_interp] = (uint32_t)(b.interp - a->interp);
	v[PERF_S_espera_spe_us] = (uint32_t)(b.spe_wait - a->spe_wait);
	v[PERF_S_dormido_us] = (uint32_t)(b.spe_sleep - a->spe_sleep);
	v[PERF_S_espera_juego_us] = (uint32_t)(b.set_wait - a->set_wait);
	v[PERF_S_copias_espera_us] = (uint32_t)(b.rb_wait - a->rb_wait);
	v[PERF_S_copia_vi_us] = (uint32_t)(b.copy - a->copy);
	v[PERF_S_jpeg_us] = (uint32_t)(b.jpeg - a->jpeg);
	v[PERF_S_tareas] = b.gfx - a->gfx;
	v[PERF_S_audio_tareas] = b.audio - a->audio;
	v[PERF_S_solape_hilo1_us] = (uint32_t)(hz ? tb_ovl * 1000000u / hz : 0);
	uint32_t other = 0;
	for (unsigned m = 1; m < SALSA64_SMP_N; m++) {
		v[PERF_S_m_nativo + m] = b.marks[m] - a->marks[m];
		other += v[PERF_S_m_nativo + m];
	}
	v[PERF_S_m_total] = (uint32_t)(b.smp_ticks - a->smp_ticks);
	v[PERF_S_m_nativo] = v[PERF_S_m_total] > other ? v[PERF_S_m_total] - other : 0;
	v[PERF_S_invalidados] = (uint32_t)(b.invalidated - a->invalidated);
	v[PERF_S_pi_dma_kb] = (uint32_t)((b.pi - a->pi) >> 10);
	perf_put_values(&perf.emu, PERF_R_STALL, perf_ms(), v, PERF_S_N);
}

/* Estado guardado pedido: emulación queda quieta en el límite del campo
 * hasta que presentación termina (state_barrier). */
static void emu_park(void)
{
	beat.emu_at = "estado guardado";
	lwsync();
	state_parked = 1;
	wake();
	while (state_req && running)
		sysUsleep(1000);
	lwsync();
	state_parked = 0;
	/* El reloj siguió mientras tanto: el ritmo empieza de nuevo. */
	pacer_init(&pacer, pacer.enabled, pacer_sleep, NULL);
}

/* Una invalidación masiva del recompilador (el código del juego pisado
 * entero, como Super Odyssey 64 al caerse en la PS3 con la interpolación
 * encendida: 6758 bloques en un campo) va al log con quién escribió: el CPU
 * o lo demás (DMA, el RDP, los trucos), y en qué zona de la RDRAM. */
#define INVAL_MASS 1000u
static struct {
	uint64_t all, cpu;
} inval_seen;

static void inval_check(void)
{
	struct salsa64_rec_stats *st = &rec.stats;
	uint64_t written = st->invalidated;
	if (written < inval_seen.all || st->inval_cpu < inval_seen.cpu)
		inval_seen.all = inval_seen.cpu = 0;  /* el recompilador volvió a empezar (otro juego) */
	uint64_t all = written - inval_seen.all, cpu = st->inval_cpu - inval_seen.cpu;
	uint64_t other = all > cpu ? all - cpu : 0;
	if (all >= INVAL_MASS)
		salsa64_log("recompilador: %llu bloques invalidados en el campo %u: %llu por escrituras del CPU "
			    "(%06x–%06x), %llu por DMA u otros (%06x–%06x)", (unsigned long long)all, n64.bus.vi.fields,
			    (unsigned long long)cpu, cpu ? st->inval_cpu_lo : 0, cpu ? st->inval_cpu_hi : 0,
			    (unsigned long long)other, other ? st->inval_other_lo : 0,
			    other ? st->inval_other_hi : 0);
	inval_seen.all = written;
	inval_seen.cpu = st->inval_cpu;
	salsa64_rec_inval_reset(&rec);
}

/* Pausa (menú del sistema, cuadro de salir): quieta en el límite del campo
 * hasta que presentación baje pause_req. Las tareas del RSP en vuelo
 * terminan solas en su SPE y su fin emulado llega en el campo siguiente;
 * el AI no entrega nada, y el audio calla aparte (audio_out_pause). */
static void emu_pause(void)
{
	beat.emu_at = "pausa";
	lwsync();
	emu_paused = 1;
	wake();
	while (pause_req && running)
		sysUsleep(2000);
	lwsync();
	emu_paused = 0;
	/* El reloj saltó lo que duró la pausa: el ritmo empieza de nuevo. */
	pacer_init(&pacer, pacer.enabled, pacer_sleep, NULL);
}

static void emu_thread(void *arg)
{
	(void)arg;
	while (running) {
		beat.emu++;
		if (pause_req) {
			emu_pause();
			continue;
		}
		uint64_t t0 = sysGetSystemTime();
		/* Al ritmo de la tele emulada; la medición A/B va al máximo y
		 * queda fuera del ritmo (ni espera ni avanza la referencia). */
		int paced = bench.stage != 1 && bench.stage != 2;
		if (paced) {
			beat.emu_at = "ritmo";
			n64.cpu.rec_mark = SALSA64_SMP_PACE;  /* muestreo (rec_sample.h) */
			pacer_wait(&pacer, t0);
			uint64_t tp = sysGetSystemTime();
			emu_stats.us_pace += tp - t0;
			t0 = tp;
		}
		n64.cpu.rec_mark = SALSA64_SMP_OTHER;
		beat.emu_at = "mandos";
		pads_apply();  /* después de la espera: lo más reciente de los mandos */
		beat.emu_at = "campo";
		struct perf_snap ps0;
		if (perf.on)
			perf_snap_take(&ps0);
		uint64_t c0 = n64.cpu.cycles, i0 = n64.cpu.idle_cycles, s0 = gfx_spu.sleep_us + jpeg_spu.sleep_us;
		uint64_t tb0 = tb_now(), o0 = busy_at(&h1.busy, tb0), l0 = busy_at(&h1.load_clock, tb0);
		uint32_t ph0 = h1.load_phase;
		uint64_t cycles = salsa64_n64_run_field(&n64);
		uint64_t tb1 = tb_now(), ovl = busy_at(&h1.busy, tb1) - o0;
		ovl_note(tb1 - tb0, ovl, busy_at(&h1.load_clock, tb1) - l0,
			 (n64.cpu.cycles - c0) - (n64.cpu.idle_cycles - i0), gfx_spu.sleep_us + jpeg_spu.sleep_us - s0,
			 h1.load && h1.load_phase == ph0 ? (int)ph0 : -1);
		if (ra_rules_allowed(ra_hardcore_active(), RA_RULE_CHEATS))
			cheats_apply(&game_cheats, &n64.bus);  /* los activos del juego (vacío si no hay) */
		ra_frame();  /* RetroAchievements: una vez por campo emulado (no hace nada sin juego con logros) */
		if (use_rec) {
			inval_check();
			if (rec.diag && n64.bus.vi.fields % 300 == 0)
				salsa64_rec_diag_report(&rec, spukit_tick_hz(), false, salsa64_log);
		}
		uint64_t t1 = sysGetSystemTime();
		n64.cpu.rec_mark = SALSA64_SMP_OTHER;
		beat.emu_at = "fin del campo";
		pif_edges_note();
		log_unknown_gbi();
		log_jpeg(t1 - t0);
		log_video(t1 - t0);
		if (paced)
			pacer_advance(&pacer, cycles);
		savefile_stage(&save_file, t1);
		mempakfile_stage(&mempak_file, t1);
		for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++)
			transferpakfile_stage(&transferpak_files[i], t1);
		if (rprof.on && ++rprof.fold_fields >= PROFILE_FOLD_FIELDS) {
			salsa64_rec_profile_fold(&rec);  /* entre campos, fuera del código generado */
			rprof.fold_fields = 0;
		}
		/* Estado guardado: la tarea del RSP en vuelo se recoge (su fin
		 * emulado no se adelanta) antes de encolar el campo, así su salida
		 * va con él y las colas quedan vacías en la barrera. */
		/* Botón Reset: primero PRE_NMI (libultra guarda y calla el audio) y,
		 * ~0,5 s emulados después, el NMI, que se hace con la emulación quieta
		 * como un estado cargado (state_barrier). */
		if (soft_reset_req == 1 && !state_req && !n64.nmi_pending) {
			soft_reset_req = 2;
			salsa64_n64_prenmi(&n64);
			salsa64_log("reinicio: PRE_NMI en el campo %u (NMI en %.2f s emulados)", n64.bus.vi.fields,
				    SALSA64_NMI_DELAY_CYCLES / 93750000.0);
		}
		if (!state_req && salsa64_n64_nmi_due(&n64)) {
			lwsync();
			state_req = STATE_RESET;
		}
		int req = state_req;
		if (req)
			state_quiet = salsa64_rsp_quiesce(&n64.bus);
		beat.emu_at = "encolar";
		post_field(req ? FIELD_STATE : 0);
		/* audio_out: si el audio necesita este hilo (fin de cada tarea
		 * de audio), aquí. */
		if (bench_request && !bench.stage)
			bench_begin(1, t1);
		else
			bench_field(t1, t1 - t0);
		emu_stats.us_emu += t1 - t0;
		if (gwin.on)
			gfx_window(t1, t1 - t0);
		if (perf.on)
			perf_field_end(&ps0, t1 - t0, ovl);
		lwsync();
		emu_stats.fields++;
		if (req)
			emu_park();
	}
	if (use_rec && rec.diag)
		salsa64_rec_diag_report(&rec, spukit_tick_hz(), true, salsa64_log);
	beat.emu_at = "terminado";
	lwsync();
	emu_done = 1;
	wake();
	/* Última operación de libc del hilo. El principal espera el join antes
	 * de recoger el juego; no se toca su contexto ni sus FILE compartidos. */
	if (reclaim_emu)
		salsa64_thread_reclaim();
	sysThreadExit(0);
}

/* ---- Hilo de presentación --------------------------------------------------- */

static struct {
	rsxkit_view view;
	rsxkit_fb *fb;
	int autotest;
	int shots_on;  /* capturas periódicas (SHOTS_PATH o prueba automática) */
	enum selftest_state result;
	uint32_t dump_field;
	uint64_t t_shot;
	uint32_t shots;
	/* Estadísticas de la ventana en curso. */
	uint64_t us_present, us_flip;
	uint32_t presented, blank, rsx_frames, skipped;
	uint64_t skipped_total;  /* no se vacía con la ventana de estadísticas */
	/* Retraso de la presentación: campos que la emulación lleva de ventaja
	 * al campo que se presenta (cola entre hilos), sumado y el mayor. */
	uint64_t behind_sum;
	uint32_t behind_max;
	/* Contador de velocidad: cuadros nuevos del juego (cambios del
	 * framebuffer del VI, también en campos saltados) y presentados en
	 * total (no se vacían con las estadísticas). */
	uint32_t last_origin, game_frames, frames_total;
	/* Pausa: qué fue lo último que se vio (para repetirlo, present_still),
	 * cuándo fue el último flip y cuántos cuadros se repitieron. */
	enum { STILL_NONE, STILL_RSX, STILL_FB } still;
	uint64_t t_flip;
	uint32_t still_frames;
} pres = { .view = { RSXKIT_SCALE_FIT, RSXKIT_FILTER_LINEAR, 4.0f / 3.0f } };

/* Filtro de la presentación. Con filtro, salsa64_rdp_present (o el
 * framebuffer 2D) solo elige qué se ve y el filtro lo dibuja al final del
 * cuadro; las capturas, las miniaturas de los estados y las pruebas
 * automáticas leen la imagen sin filtro, que entonces se dibuja antes
 * (filter_plain) y el filtro la tapa. */
static struct {
	int sel;        /* el elegido (menu.cfg o el archivo `filtro`) */
	int cur;        /* el que se usa: sel, o el del ciclo de medición */
	bool cycle;     /* filtros-ciclo */
	int cycle_reduce;  /* el ciclo alterna la reducción (1 o 2 pasadas) en cada vuelta; 0: no */
	bool cycle_spe;    /* el ciclo alterna el SPE en cada vuelta */
	bool spe;          /* la señal del cable va en el SPE ahora */
	int cycle_list[SALSA64_CRTS];  /* los del ciclo, desde «Ninguno» */
	int cycle_n, cycle_i;
	bool full_shots;
	bool announced; /* se avisó «Aplicando filtro…»: la preparación va en este cuadro */
	uint64_t t_cycle;
	bool pending;   /* este cuadro va con filtro: falta dibujarlo */
	bool plain;     /* ... y ya tiene la imagen sin filtro */
	struct salsa64_filter_src src;
	uint32_t full_shot;
} flt;

/* Tiempo del RSX en lo que manda la presentación, con marcas de tiempo
 * (rsxkit/post.h; las de 0 a 23 son de frontend/filters): cada tramo de
 * órdenes (traducir las tareas gráficas de la emulación; presentar un campo
 * con su filtro y lo de encima) entre una marca al empezar y otra al
 * terminar, y una barrera; se suma cuando el RSX pasó la barrera. Es una
 * cota superior: incluye lo que el RSX esperó órdenes del PPE dentro del
 * tramo. */
#define RT_STAMP0 24
#define RT_PAIRS  16
enum { RT_EMU, RT_PRES, RT_KINDS };
static struct {
	uint32_t next;
	uint8_t kind[RT_PAIRS];
	bool pending[RT_PAIRS];
	uint32_t fence[RT_PAIRS];
	uint64_t ns[RT_KINDS];
	uint64_t total_ns[RT_KINDS];  /* desde el principio del juego */
	uint32_t n[RT_KINDS];
	int open;  /* par abierto, o -1 */
} rt = { .open = -1 };

static void rt_collect(void)
{
	for (uint32_t i = 0; i < RT_PAIRS; i++) {
		if (!rt.pending[i] || !rsxkit_fence_done(rt.fence[i]))
			continue;
		rt.pending[i] = false;
		uint64_t a = rsxkit_stamp_ns(RT_STAMP0 + 2 * i), b = rsxkit_stamp_ns(RT_STAMP0 + 2 * i + 1);
		if (b < a || b - a > 200000000u)
			continue;
		rt.ns[rt.kind[i]] += b - a;
		rt.total_ns[rt.kind[i]] += b - a;
		rt.n[rt.kind[i]]++;
	}
}

static void rt_begin(int kind)
{
	if (rt.open >= 0)
		return;
	rt_collect();
	uint32_t i = rt.next++ % RT_PAIRS;
	rt.pending[i] = false;  /* si seguía en vuelo, se pierde esa medición */
	rt.kind[i] = (uint8_t)kind;
	rt.open = (int)i;
	rsxkit_stamp(RT_STAMP0 + 2 * i);
}

static void rt_end(void)
{
	if (rt.open < 0)
		return;
	uint32_t i = (uint32_t)rt.open;
	rsxkit_stamp(RT_STAMP0 + 2 * i + 1);
	rt.fence[i] = rsxkit_fence();
	rt.pending[i] = true;
	rt.open = -1;
}

static bool filter_tests(void)
{
	return selftest_on || gfxtest_on || audiotest_on;
}

/* La imagen del cuadro sin filtro, si va con filtro y aún no está (antes
 * de una captura). */
static void filter_plain(rsxkit_rect *r)
{
	if (!flt.pending || flt.plain)
		return;
	if (pres.still == STILL_RSX)
		salsa64_rdp_present_again(NULL, NULL, r);
	else if (pres.still == STILL_FB)
		rsxkit_fb_draw(pres.fb, &pres.view, r);
	flt.plain = true;
}

/* La fuente del filtro: lo que eligió salsa64_rdp_present o el framebuffer
 * 2D (`shown`: su tamaño sin el overscan). */
static bool filter_source(bool rsx, const struct salsa64_vi_frame *shown)
{
	struct salsa64_filter_src *s = &flt.src;
	memset(s, 0, sizeof *s);
	s->aspect = pres.view.aspect;
	if (rsx)
		return (s->has_gen = salsa64_rdp_present_source(NULL, NULL, &s->tex, &s->region, &s->factor, &s->gen));
	if (!rsxkit_fb_texture(pres.fb, &s->tex))
		return false;
	s->region = (rsxkit_rect){ 0, 0, shown->width, shown->height };
	s->factor = 1;
	return true;
}

/* La fuente del filtro en la pausa: lo último presentado con la vista y el
 * recorte de ahora («Imagen» del menú de pausa los cambia). */
static bool filter_source_again(const struct salsa64_overscan *crop)
{
	struct salsa64_filter_src *s = &flt.src;
	memset(s, 0, sizeof *s);
	s->aspect = pres.view.aspect;
	return (s->has_gen = salsa64_rdp_present_source(&pres.view, crop, &s->tex, &s->region, &s->factor, &s->gen));
}

/* Si preparar el filtro va a tardar más de un par de cuadros (tablas
 * nuevas, la comprobación de la primera vez), este cuadro va sin filtro
 * con el aviso «Aplicando filtro…» y la preparación va en el siguiente:
 * mientras, se ve el aviso y no una imagen quieta que parece colgada.
 * true: este cuadro va sin filtro. */
static bool filter_announce(uint64_t now)
{
	if (flt.announced || salsa64_filters_setup_ms(flt.cur, &flt.src) <= FILTER_NOTICE_MS)
		return false;
	flt.announced = true;
	osd_notice(now, "Aplicando filtro…");
	return true;
}

/* salsa64_filters_draw del filtro en curso; si se avisó «Aplicando
 * filtro…», ya está: se quita el aviso. */
static bool filter_draw(rsxkit_rect *r)
{
	struct salsa64_filter_src src = flt.src;
	/* El sistema puede escribir en los búferes presentados. */
	src.screen_busy = pause_ctl.system || pause_ctl.drawing || pause_ctl.intercepted;
	bool ok = salsa64_filters_draw(flt.cur, &src, r);
	if (flt.announced) {
		flt.announced = false;
		osd_notice_clear();
	}
	return ok;
}

/* Dibuja el filtro del cuadro (o, si no se pudo, la imagen sin filtro). */
static void filter_finish(rsxkit_rect *r)
{
	if (!flt.pending)
		return;
	if (!filter_draw(NULL))
		filter_plain(r);
	flt.pending = false;
}

/* Al abrir un juego: el CRT elegido en el menú (enum salsa64_crt, de
 * load_view), o el del archivo `filtro`, que manda. */
static void filter_setup(int menu_filter)
{
	memset(&flt, 0, sizeof flt);
	flt.sel = menu_filter;
	FILE *f = fopen(FILTER_PATH, "r");
	if (f) {
		char key[32] = "";
		if (fscanf(f, "%31s", key) == 1) {
			int k = salsa64_crt_from_key(key);
			if (k >= 0)
				flt.sel = k;
			else
				salsa64_log("filtros: «%s» en %s no es un filtro", key, FILTER_PATH);
		}
		fclose(f);
	}
	flt.cur = flt.sel;
	/* Diagnóstico: `filtro-ver` con «tabla [escala]» dibuja esa tabla del
	 * filtro en la esquina de la pantalla (frontend/filters/filters.h). */
	f = fopen(FILTER_VIEW_PATH, "r");
	if (f) {
		char name[32] = "";
		float scale = 1.0f;
		if (fscanf(f, "%31s %f", name, &scale) >= 1)
			salsa64_filters_debug(name, scale);
		fclose(f);
	} else {
		salsa64_filters_debug(NULL, 1.0f);
	}
	flt.cycle = file_exists(FILTER_CYCLE_PATH);
	/* El ciclo: todos, o los de la lista que tenga el archivo (claves). */
	flt.cycle_n = 0;
	flt.cycle_list[flt.cycle_n++] = SALSA64_CRT_NONE;
	f = flt.cycle ? fopen(FILTER_CYCLE_PATH, "r") : NULL;
	if (f) {
		char key[32];
		while (flt.cycle_n < SALSA64_CRTS && fscanf(f, "%31s", key) == 1) {
			int k = salsa64_crt_from_key(key);
			if (k > SALSA64_CRT_NONE)
				flt.cycle_list[flt.cycle_n++] = k;
		}
		fclose(f);
	}
	if (flt.cycle_n == 1)
		for (int k = SALSA64_CRT_NONE + 1; k < SALSA64_CRTS; k++)
			flt.cycle_list[flt.cycle_n++] = k;
	flt.cycle_i = 0;
	flt.full_shots = file_exists(SHOTS_FULL_PATH);
	if (flt.cycle)
		flt.cur = SALSA64_CRT_NONE;
	flt.t_cycle = sysGetSystemTime();
	int passes = 2;
	f = fopen(FILTER_REDUCE_PATH, "r");
	if (f) {
		if (fscanf(f, "%d", &passes) != 1 || (passes != 1 && passes != 2))
			passes = 2;
		fclose(f);
	} else if (flt.cycle && !file_exists(FILTER_SPE_PATH)) {
		flt.cycle_reduce = 2;
	}
	salsa64_filters_options(file_exists(FILTER_SHOTS_PATH), passes);
	flt.spe = file_exists(FILTER_SPE_PATH);
	flt.cycle_spe = flt.cycle && flt.spe;
	salsa64_filters_spe(flt.spe);
	salsa64_log("filtros: %s%s%s", flt.cycle ? "ciclo de medición (todos cada 20 s, desde «Ninguno»)" : "",
		    flt.cycle ? (flt.cycle_reduce ? "; cada vuelta cambia la reducción (dos pasadas, una, …)"
				 : flt.cycle_spe ? "; cada vuelta cambia la señal del cable (SPE, RSX, …)" : "")
			      : salsa64_crt_name(flt.cur),
		    flt.full_shots ? "; capturas de la pantalla entera (pantalla-N.ppm)" : "");
}

/* Cambia el CRT en el juego (el menú de pausa): vale desde el próximo
 * cuadro; las tablas se rehacen solas. Con el ciclo de medición, no. */
static void filter_select(int crt)
{
	if (flt.cycle || crt < SALSA64_CRT_NONE || crt >= SALSA64_CRTS || crt == flt.cur)
		return;
	flt.sel = flt.cur = crt;
	flt.announced = false;
	salsa64_log("filtros: «%s»", salsa64_crt_name(crt));
}

/* Ciclo de medición: el siguiente filtro cada FILTER_CYCLE_US. */
static void filter_cycle(uint64_t now)
{
	if (!flt.cycle || now - flt.t_cycle < FILTER_CYCLE_US)
		return;
	flt.t_cycle = now;
	flt.cycle_i = (flt.cycle_i + 1) % flt.cycle_n;
	flt.cur = flt.cycle_list[flt.cycle_i];
	if (flt.cur == SALSA64_CRT_NONE && flt.cycle_reduce) {
		flt.cycle_reduce = 3 - flt.cycle_reduce;
		rsxkit_post_reduce_passes(flt.cycle_reduce);
		salsa64_log("filtros: ciclo, otra vuelta: reducir en %d pasada%s", flt.cycle_reduce,
			    flt.cycle_reduce == 1 ? "" : "s");
	}
	if (flt.cur == SALSA64_CRT_NONE && flt.cycle_spe) {
		flt.spe = !flt.spe;
		salsa64_filters_spe(flt.spe);
		salsa64_log("filtros: ciclo, otra vuelta: la señal del cable en %s", flt.spe ? "el SPE" : "el RSX");
	}
	salsa64_log("filtros: ciclo → «%s»", salsa64_crt_name(flt.cur));
	osd_notice(now, "Filtro: %s", tr(salsa64_crt_name(flt.cur)));
}

/* rsxkit_frame_begin y _end con el tiempo que se esperó el flip anterior:
 * si pasa de SLOW_FLIP_US, al log con los vblanks que pasaron desde que se
 * pidió (con el sistema encima, la PS3 retenía los flips segundos: por eso
 * entonces solo se presenta con rsxkit_frame_ready). */
static uint64_t flip_vbl;  /* vblanks al pedir el último flip */

static void flip_wait_note(uint64_t t0, const char *where)
{
	uint64_t dt = sysGetSystemTime() - t0;
	if (dt > SLOW_FLIP_US)
		salsa64_log("flip: %.0f ms esperando en «%s» (sistema %s; %llu vblanks desde que se pidió)", dt / 1e3,
			    where, pausectl_overlay(&pause_ctl) ? "encima" : "sin dibujar",
			    (unsigned long long)(rsxkit_vblank_count() - flip_vbl));
}

static void frame_begin(const char *where)
{
	uint64_t t0 = sysGetSystemTime(), tb0 = tb_now();
	beat.pres_at = where;
	busy_stop(&h1.busy, tb0);
	rsxkit_frame_begin(0x000000);
	uint64_t tb1 = tb_now();
	busy_start(&h1.busy, tb1);
	h1.t_flip += tb1 - tb0;
	flip_wait_note(t0, where);
}

/* salsa64_rdp_flush con su tiempo y lo de rdp.tb que cae dentro. */
static uint32_t h1_flush(uint32_t upto)
{
	struct salsa64_rdp_tb a, b;
	salsa64_rdp_get_tb(&a);
	uint64_t t0 = tb_now();
	const char *at = beat.pres_at;
	beat.pres_at = "traducción";  /* la tarea y la etapa: salsa64_rdp_translating */
	rt_begin(RT_EMU);
	uint32_t n = salsa64_rdp_flush(upto);
	rt_end();
	beat.pres_at = at;
	h1.t_flush += tb_now() - t0;
	salsa64_rdp_get_tb(&b);
	h1.in_flush.translate += b.translate - a.translate;
	h1.in_flush.copy += b.copy - a.copy;
	h1.in_flush.tex += b.tex - a.tex;
	h1.in_flush.fence += b.fence - a.fence;
	return n;
}

static void frame_end(const char *where)
{
	if (screen_probe && --screen_probe == 0) {
		uint32_t pitch;
		const rsxkit_screen *sc = rsxkit_screen_info();
		rsxkit_finish();
		const volatile uint32_t *px = rsxkit_back_buffer(&pitch);
		uint32_t w = pitch / 4;
		salsa64_log("pantalla: cuadro de «%s»: píxel central %08x, (%u,%u) %08x, (%u,%u) %08x", where,
			    px[sc->height / 2 * w + sc->width / 2], sc->width / 8, sc->height / 8,
			    px[sc->height / 8 * w + sc->width / 8], sc->width / 2, sc->height / 8,
			    px[sc->height / 8 * w + sc->width / 2]);
	}
	uint64_t t0 = sysGetSystemTime();
	beat.pres_at = where;
	rsxkit_frame_end();
	flip_wait_note(t0, where);
	flip_vbl = rsxkit_vblank_count();
}

/* Lo que va encima del juego: aviso, contador y, si está, el cuadro de
 * START + SELECT. */
static int scan_level;       /* 0 sin líneas, 1 suaves, 2 fuertes (filtro del juego) */
static rsxkit_rect scan_r;   /* dónde quedó la imagen, en puntos de la pantalla */
static uint32_t scan_lines;  /* líneas de la imagen del juego */
static struct salsa64_vi_frame still_f;  /* lo último presentado, para su recorte en la pausa */

/* La imagen del juego que corre (load_view; la página «Imagen» del menú de
 * pausa la cambia en vivo, image_events): aspecto, filtro, recorte
 * (MENU_OVERSCAN_*; CUSTOM es el de overscan.cfg) y escala interna, lo
 * global de cada uno (Opciones) y los ajustes del juego como están en
 * juegos/<ROM>.cfg. Sin `source` (las pruebas) no se guardan. */
static struct {
	int aspect, filter, crop, scale, density, interp;
	int g_aspect, g_filter, g_crop, g_scale, g_density, g_interp;
	struct menu_game game;
	const char *source;
} img;
/* La política decide con muestras de un segundo. El cambio real se hace
 * después de pausar en un límite de campo, por el mismo camino del menú. */
#define PAUSE_SCALE_AUTO 0x10u
static struct scale_auto scale_policy;
static struct {
	bool disabled;  /* SCALE_AUTO_OFF_PATH: comparación con el camino anterior */
	bool pending;
	uint64_t t0, rsx0, skipped0;
	uint32_t fields0, presented0;
	int from, to;
	struct scale_auto_result reason;
} auto_scale;
/* La «t» de INTERP_TEST_PATH (al abrir el juego), o < 0 sin el archivo. */
static float interp_test_t = -1.0f;
/* Prueba de la interpolación cambiada en vivo (INTERP_ALT_PATH, al abrir el
 * juego): cada `every` campos, una pausa como la del menú en la que se
 * cambia (interp_alt_step). */
static struct {
	uint32_t every, next, toggles;
	int scale[2];      /* con dos escalas, también se alterna entre ellas */
	int stage;         /* 0 apagada; 1 esperando el campo; 2 pidiendo la pausa; 3 en pausa hasta t */
	uint64_t t;
} interp_alt;
static int rdp_scale_req;  /* escala interna explícita pedida al RDP ahora */
static struct menu_settings video_cfg;  /* video y escala que corren (video.cfg al arrancar o lo último aplicado) */
static bool video_scale_configured;     /* video.cfg traía una escala explícita */
static int scale_wanted(void);
static const char *scale_label(char *buf, size_t n);
static void scale_control_configure(bool new_game);
static void scale_control_sample(uint64_t now);
static void scale_control_apply(void);

static void present_overlay(uint64_t now)
{
	if (!ui_ok)
		return;
	if (scan_level && scan_r.w && scan_lines) {
		const rsxkit_screen *sc = rsxkit_screen_info();
		float k = ui_lw / (float)sc->width;
		osd_scanlines_set(scan_level, (float)scan_r.x * k, (float)scan_r.y * k, (float)scan_r.w * k,
				  (float)scan_r.h * k, scan_lines > 300 ? scan_lines / 2 : scan_lines);
	} else {
		osd_scanlines_set(0, 0.0f, 0.0f, 0.0f, 0.0f, 0);
	}
	osd_draw(&fps, now, ui_lw, ui_lh);
	if (pause_ctl.menu) {
		bool hc = ra_hardcore_active();
		struct osd_pause_menu pm = { pause_ctl.menu_sel, state_slot, pm_slot_have, fps.on, in_menu,
					     hc ? 0 : game_cheats.n, cheats_active(&game_cheats), version_text,
					     ra_list_count(), ra_list_unlocked() };
		osd_pause_menu_draw(&pm, ui_lw, ui_lh);
	} else if (pause_ctl.accessory) {
		struct osd_accessory_menu am;
		am.sel = pause_ctl.accessory_sel;
		for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++) {
			int a = n64.bus.pif.accessory[i] == SALSA64_PIF_ACCESSORY_NONE ? MENU_ACC_NONE
				: n64.bus.pif.accessory[i] == SALSA64_PIF_ACCESSORY_RUMBLE_PAK ? MENU_ACC_RUMBLE
				: n64.bus.pif.accessory[i] == SALSA64_PIF_ACCESSORY_TRANSFER_PAK ? MENU_ACC_TRANSFER : MENU_ACC_PAK;
			am.value[i] = menu_accessory_name(a);
		}
		osd_accessory_menu_draw(&am, ui_lw, ui_lh);
	} else if (pause_ctl.image) {
		char sbuf[24];
		struct osd_image_menu im = {
			pause_ctl.image_sel,
			{ menu_filter_name(img.filter), menu_density_name(img.density), menu_aspect_name(img.aspect),
			  menu_overscan_name(img.crop), scale_label(sbuf, sizeof sbuf), menu_interp_name(img.interp) },
			menu_filter_has_density(img.filter),
			scale_wanted() != rdp_scale_req, img.source != NULL, menu_filter_credit(img.filter),
		};
		osd_image_menu_draw(&im, ui_lw, ui_lh);
	} else if (pause_ctl.dialog)
		osd_confirm_draw(pause_ctl.dialog_for == PAUSECTL_RESET ? "¿Seguro? Reiniciar (botón Reset)" :
				 pause_ctl.dialog_for == PAUSECTL_RESTART ? "¿Seguro? Reiniciar desde cero" :
				 in_menu ? "¿Volver al menú?" : "¿Salir al XMB?", "×: Sí     ○: No", ui_lw, ui_lh);
	else if (pause_ctl.panel && pause_ctl.panel_kind)
		osd_achievements_draw(pause_ctl.panel_sel, ui_lw, ui_lh);
	else if (pause_ctl.panel)
		osd_cheats_draw(&game_cheats, pause_ctl.panel_sel, ui_lw, ui_lh);
}

/* Prueba del seguro del RSX (HANG_TEST_PATH, rsx_hang_test). */
enum { HANG_NONE, HANG_LABEL, HANG_RSX, HANG_FIFO };
static struct {
	int mode;
	uint32_t field;
} hang_test;
static void rsx_hang_test(void);

/* Caja negra: lo que pasó con este campo (docs/cajanegra.md). */
static void cj_note_field(const struct field_msg *m, uint32_t flags)
{
	if (!cajanegra_enabled())
		return;
	struct cajanegra_field f = { m->field, m->tasks, flags, (uint32_t)m->f.type, m->f.origin, m->f.width,
				     m->f.height, m->f.stride, m->f.bpp, m->vi, n64.bus.vi.fields - m->field };
	cajanegra_field(&f);
}

/* Presenta (o salta) un campo encolado. */
static void present_field(const struct field_msg *m, uint64_t now)
{
	h1_flush(m->tasks);  /* las tareas de este campo, no las del siguiente */
	if (hang_test.mode && m->field >= hang_test.field)
		rsx_hang_test();
	cajanegra_set_field(m->field);
	if (m->f.width && m->f.origin != pres.last_origin) {
		pres.last_origin = m->f.origin;
		pres.game_frames++;
	}
	if (m->skip && !(m->flags & FIELD_KEEP)) {
		if (m->snap >= 0)
			snap_cas(m->snap, SNAP_QUEUED, SNAP_FREE);
		pres.skipped++;
		pres.skipped_total++;
		trace_vi_log(m, "saltado", 0, NULL, NULL, NULL, NULL);
		cj_note_field(m, BB_FLD_SKIPPED);
		return;
	}
	int slot = m->snap;
	bool have = false;
	if (slot >= 0) {
		have = snap_cas(slot, SNAP_QUEUED, SNAP_IN_USE);
		if (!have) {
			/* Emulación la soltó al encolar un campo más nuevo. */
			pres.skipped++;
			pres.skipped_total++;
			trace_vi_log(m, "copia descartada", 0, NULL, NULL, NULL, NULL);
			cj_note_field(m, BB_FLD_SKIPPED);
			return;
		}
	}
	rsxkit_rect r = { 0, 0, 0, 0 };
	bool check = false, check_gfx = false;
	uint64_t t0 = sysGetSystemTime();
	uint32_t behind = n64.bus.vi.fields - m->field;  /* lectura de 32 bits del otro hilo */
	pres.behind_sum += behind;
	if (behind > pres.behind_max)
		pres.behind_max = behind;
	frame_begin("flip anterior");
	beat.pres_at = "presentar";
	rt_begin(RT_PRES);
	/* Lo que se ve: la imagen del VI sin el overscan (las capturas van de
	 * ese tamaño). */
	struct salsa64_overscan crop = overscan_for(&m->f);
	struct salsa64_vi_frame shown = m->f;
	shown.width -= crop.left + crop.right;
	shown.height -= crop.top + crop.bottom;
	pres.still = STILL_NONE;
	bool filt = flt.cur != SALSA64_CRT_NONE && !filter_tests();
	flt.pending = flt.plain = false;
	salsa64_rdp_present_draw(!filt);
	if (m->f.width) {
		if (salsa64_rdp_present(&m->f, m->field, &pres.view, &crop, &r)) {
			pres.rsx_frames++;
			pres.still = STILL_RSX;
			if (cajanegra_enabled()) {
				/* Intermedio o repetido: la caja negra lo anota con su t, su k y
				 * las dos tareas (para repetirlo en el host). */
				struct salsa64_rdp_field_info fi;
				salsa64_rdp_field_info(&fi);
				if (fi.kind != SALSA64_FIELD_TRUE)
					cajanegra_field_interp(fi.kind == SALSA64_FIELD_INTERP ? BB_KIND_INTERP : BB_KIND_REPEAT, fi.t,
							       fi.k, fi.task, fi.prev_task);
			}
			flt.pending = filt && filter_source(true, &shown) && !filter_announce(now);
			if (filt && !flt.pending)
				salsa64_rdp_present_again(NULL, NULL, &r);
			if (gfxtest_on && pres.result == SELFTEST_RUNNING)
				check_gfx = gfxtest_want_screen();
		} else if (have) {
			check = (m->flags & FIELD_CHECK) != 0;
			uint32_t line = m->f.width * m->f.bpp;
			rsxkit_fb_upload(pres.fb, snap_buf[slot] + crop.top * line + crop.left * m->f.bpp, shown.width,
					 shown.height, line, m->f.bpp == 4 ? RSXKIT_RGBA8888 : RSXKIT_RGBA5551);
			pres.still = STILL_FB;
			flt.pending = filt && filter_source(false, &shown) && !filter_announce(now);
			if (!flt.pending)
				rsxkit_fb_draw(pres.fb, &pres.view, &r);
		} else {
			pres.blank++;
		}
	} else {
		pres.blank++;
	}
	if (pres.still == STILL_RSX) {
		struct salsa64_rdp_present_info p;
		struct salsa64_rdp_field_info fi;
		if (salsa64_rdp_last_present(&p)) {
			salsa64_rdp_field_info(&fi);
			trace_vi_log(m, "RSX", behind, &p, &fi, &crop, &r);
		} else {
			trace_vi_log(m, "RSX sin selección", behind, NULL, NULL, NULL, NULL);
		}
	} else {
		trace_vi_log(m, pres.still == STILL_FB ? "RDRAM" : m->f.width ? "nada" : "sin imagen", behind,
			     NULL, NULL, NULL, NULL);
	}
	if (have)
		snap_state[slot] = SNAP_FREE;  /* ya está en la memoria del RSX */
	if (check || check_gfx) {
		filter_plain(&r);
		rsxkit_finish();
		if (check)
			selftest_screen(&m->f, &r);
		else
			gfxtest_screen(&m->f, &r);
	} else if ((m->flags & FIELD_SHOT) && m->f.width) {
		char path[64];
		snprintf(path, sizeof path, SALSA64_DATA_DIR "/campo-%u.ppm", m->field);
		filter_plain(&r);
		rsxkit_finish();
		salsa64_screenshot(path, &shown, &r);
		salsa64_log("captura %s", path);
	} else if (pres.shots_on && !cap_every && !selftest_on && !gfxtest_on && !audiotest_on && m->f.width &&
		   (pres.autotest ? m->field / AUTO_SHOT_FIELDS != pres.shots : now - pres.t_shot >= SHOT_US)) {
		char path[64];
		snprintf(path, sizeof path, SALSA64_DATA_DIR "/screen-%u.ppm", pres.shots % SHOT_MAX + 1);
		bool with_filter = flt.pending;
		filter_plain(&r);
		rsxkit_finish();
		salsa64_screenshot(path, &shown, &r);
		salsa64_log("captura %s (campo %u)", path, m->field);
		if (with_filter && flt.full_shots) {
			/* La pantalla entera con el filtro (diagnóstico). */
			filter_finish(&r);
			rsxkit_finish();
			snprintf(path, sizeof path, SALSA64_DATA_DIR "/pantalla-%u.ppm", pres.shots % SHOT_MAX + 1);
			salsa64_screenshot_full(path);
			salsa64_log("captura %s (con «%s»)", path, salsa64_crt_name(flt.cur));
		}
		pres.shots = pres.autotest ? m->field / AUTO_SHOT_FIELDS : pres.shots + 1;
		pres.t_shot = now;
	}
	if ((m->flags & FIELD_STATE) && state_req == STATE_SAVE) {
		/* La miniatura del estado: la imagen sin el aviso encima. */
		filter_plain(&r);
		rsxkit_finish();
		states_thumb_capture(&r);
	}
	if (pres.dump_field && m->field >= pres.dump_field) {
		filter_plain(&r);
		rsxkit_finish();
		salsa64_log("volcado de la HLE en el campo %u: %s", m->field,
			    salsa64_rdp_dump(SALSA64_DATA_DIR "/gfxdump.bin") ? "bien" : "FALLO");
		pres.dump_field = 0;
	}
	if (dump_now && m->f.width) {
		char path[64], shot[64];
		dump_now = false;
		snprintf(path, sizeof path, SALSA64_DATA_DIR "/gfxdump-%u.bin", m->field);
		snprintf(shot, sizeof shot, SALSA64_DATA_DIR "/gfxdump-%u.ppm", m->field);
		filter_plain(&r);
		rsxkit_finish();
		bool ok = salsa64_rdp_dump(path);
		salsa64_screenshot(shot, &shown, &r);
		salsa64_log("volcado de la HLE (SELECT + R2) en %s con la captura %s: %s", path, shot,
			    ok ? "bien" : "FALLO");
		osd_notice(sysGetSystemTime(), ok ? "Volcado del campo %u" : "Volcado: falló", m->field);
	}
	filter_finish(&r);
	if (r.w) {
		scan_r = r;
		scan_lines = shown.height;
		still_f = m->f;
	}
	present_overlay(sysGetSystemTime());
	/* audio_out: una vez por campo presentado (estadísticas y volcado). */
	uint64_t t1 = sysGetSystemTime();
	audio_out_field(t1);
	rt_end();
	frame_end("flip");
	beat.pres_at = "cola";
	pres.frames_total++;
	uint64_t t2 = sysGetSystemTime();
	pres.t_flip = t2;
	pres.us_present += t1 - t0;
	pres.us_flip += t2 - t1;
	pres.presented++;
	cj_note_field(m, BB_FLD_SHOWN | (pres.still == STILL_RSX ? BB_FLD_RSX : pres.still == STILL_FB ? BB_FLD_RDRAM : 0) |
				 (m->f.width ? 0 : BB_FLD_BLANK));

	if (exit_field && m->field >= exit_field) {
		salsa64_log("salida en el campo %u (capturas)", m->field);
		/* En la prueba automática, con una ROM del disco, la salida pedida
		 * reemplaza a AUTO_FIELDS (un estado cargado salta los campos). */
		game_auto_ok = pres.autotest && !selftest_on && !gfxtest_on && !audiotest_on;
		running = 0;
	}
	if (pres.autotest && !selftest_on && !gfxtest_on && !audiotest_on && !exit_field && m->field >= AUTO_FIELDS) {
		salsa64_log("%s: OK (ROM del disco, %u campos)", res_tag, m->field);
		game_auto_ok = 1;
		running = 0;
	}
	if ((selftest_on || gfxtest_on || audiotest_on) && pres.result == SELFTEST_RUNNING) {
		pres.result = selftest_on ? selftest_state(m->field)
			    : gfxtest_on  ? gfxtest_state(m->field)
					  : audiotest_state(m->field);
		if (pres.result == SELFTEST_OK && state_test.stage && state_test.stage != ST_DONE) {
			salsa64_log("estados, prueba automática: la ROM terminó antes de cargar el estado");
			pres.result = SELFTEST_FAILED;
		}
		if (pres.result != SELFTEST_RUNNING) {
			salsa64_log("%s: %s (campo %u)", res_tag, pres.result == SELFTEST_OK ? "OK" : "FALLO",
				    m->field);
			if (pres.autotest)
				running = 0;
			else if (pres.result == SELFTEST_OK && selftest_on)
				bench_request = 1;
		}
	}
}

/* En pausa no llegan campos: se repite lo último que se vio (sin traducir
 * nada ni tocar la tabla de destinos ni la RDRAM) con lo de encima, y el
 * flip sigue al ritmo de la tele: el sistema lo necesita con su menú
 * abierto, y así el cuadro de salir se dibuja sobre el juego. */
static void present_still(void)
{
	uint64_t now = sysGetSystemTime();
	rsxkit_rect r;
	frame_begin("flip anterior (pausa)");
	beat.pres_at = "cuadro repetido";
	/* Con la vista y el recorte de ahora: la página «Imagen» los cambia en
	 * la pausa (el recorte de una imagen de la RDRAM, al seguir). */
	struct salsa64_overscan crop = overscan_for(&still_f);
	r.w = 0;
	bool filt = flt.cur != SALSA64_CRT_NONE && !filter_tests();
	if (pres.still == STILL_RSX) {
		if (!(filt && filter_source_again(&crop) && !filter_announce(now) && filter_draw(&r)) &&
		    !salsa64_rdp_present_again(&pres.view, &crop, &r))
			pres.still = STILL_NONE;  /* el destino se soltó: negro */
	} else if (pres.still == STILL_FB) {
		rsxkit_texture t;
		if (!(filt && rsxkit_fb_texture(pres.fb, &t) &&
		      (flt.src.tex = t, flt.src.factor = 1, flt.src.has_gen = false, !filter_announce(now)) &&
		      filter_draw(&r)))
			rsxkit_fb_draw(pres.fb, &pres.view, &r);
	}
	if (r.w)
		scan_r = r;
	present_overlay(now);
	frame_end("flip (pausa)");
	beat.pres_at = "pausa";
	pres.frames_total++;
	pres.still_frames++;
	pres.t_flip = sysGetSystemTime();
}

/* Consume lo encolado: campos en orden y, al final, las tareas gráficas que
 * ya terminaron y el RDP pudo dibujar (son del campo siguiente; no la que
 * espera a que el CPU descongele el RDP). Solo lo que había al entrar: con
 * el flip en vsync y la emulación al mismo ritmo, mientras se presenta un
 * campo llega el siguiente, la cola no se vacía y el bucle de presentación
 * no volvía a leer los mandos (7,4 s sin leerlos en la selección de ranura
 * de Zelda, PS3, 2026-10-11). */
static void drain(uint64_t now, int present)
{
	uint32_t head = field_head;
	while (field_tail != head) {
		lwsync();  /* el mensaje, después de ver head */
		struct field_msg m = field_q[field_tail % FIELD_QUEUE];
		if (present) {
			uint64_t t0 = tb_now(), flush0 = h1.t_flush, flip0 = h1.t_flip;
			present_field(&m, now);
			h1.t_present += tb_now() - t0 - (h1.t_flush - flush0) - (h1.t_flip - flip0);
		} else if (m.snap >= 0) {
			snap_cas(m.snap, SNAP_QUEUED, SNAP_FREE);
		}
		lwsync();
		field_tail++;
	}
	/* Al cerrar (sin presentar) se traduce todo: no queda nada en vuelo. */
	h1_flush(present ? tasks_visible : ~0u);
}

/* Estadísticas de los dos hilos, cada STATS_US: lo visto en la ventana
 * anterior (se vacía al empezar cada juego). */
static struct {
	uint64_t t_stats, us_emu0, us_copy0, us_pace0, cyc0, idle0;
	uint32_t fields0, pif_reads0;
	struct salsa64_rdp_stats rdp0;
	uint32_t gfx_waits0, gfx_polls0, gfx_cut0, gfx_tasks0, model_held0;
	uint64_t model_wait0, model_sum0;
	uint64_t gfx_skipped0, gfx_bg0;
	uint64_t gfx_ticks0, gfx_wait0, gfx_sleep0, gfx_vtx0, gfx_state0, gfx_tex0, gfx_load0;
	uint32_t lle_tasks0, lle_jobs0, lle_polls0;
	uint64_t lle_instr0, lle_ticks0, lle_wait0, lle_sleep0;
	uint64_t h1_busy0;
	struct salsa64_rdp_tb rdp_tb0;
	struct ovl_group ovl0[OVL_GROUPS], load0[LOAD_PHASES];
} sw;

/* Las ventanas del hilo 1 empiezan aquí. */
static void h1_window_reset(void)
{
	sw.h1_busy0 = busy_at(&h1.busy, tb_now());
	salsa64_rdp_get_tb(&sw.rdp_tb0);
	memcpy(sw.ovl0, h1.by_ovl, sizeof sw.ovl0);
	memcpy(sw.load0, h1.by_load, sizeof sw.load0);
	h1.t_flip = h1.t_flush = h1.t_present = h1.t_load = 0;
	memset(&h1.in_flush, 0, sizeof h1.in_flush);
}

/* Lo que trabajó presentación en la ventana y cómo se solapó con la
 * emulación (ver «Cuánto le quita el hilo de presentación»). */
static void h1_stats(uint32_t fields, uint64_t dt_us)
{
	double k = 1e3 / spukit_tick_hz() / (fields ? fields : 1);  /* ticks → ms por campo emulado */
	uint64_t busy = busy_at(&h1.busy, tb_now()) - sw.h1_busy0;
	struct salsa64_rdp_tb tb;
	salsa64_rdp_get_tb(&tb);
	uint64_t copy = tb.copy - sw.rdp_tb0.copy, tex = tb.tex - sw.rdp_tb0.tex, fence = tb.fence - sw.rdp_tb0.fence;
	int64_t translate = (int64_t)(h1.t_flush - h1.in_flush.copy - h1.in_flush.tex - h1.in_flush.fence);
	int64_t present = (int64_t)(h1.t_present - (copy - h1.in_flush.copy) - (tex - h1.in_flush.tex) -
				    (fence - h1.in_flush.fence));
	int64_t other = (int64_t)(busy - h1.t_flush - h1.t_present - h1.t_load);
	salsa64_log("hilo 1: despierto %.2f ms por campo emulado (%.1f %% del tiempo): traducción %.2f, copias a la "
		    "RDRAM %.2f, texturas en el PPE %.2f, presentar %.2f, carga de mentira %.2f, otros %.2f; dentro, "
		    "dormido esperando barreras del RSX %.2f; flip anterior %.2f (dormido)",
		    busy * k, dt_us ? busy * 1e8 / spukit_tick_hz() / dt_us : 0.0, translate * k, copy * k, tex * k,
		    present * k, h1.t_load * k, other * k, fence * k, h1.t_flip * k);
	char g[OVL_GROUPS][128];
	for (int i = 0; i < OVL_GROUPS; i++)
		ovl_text(g[i], sizeof g[i], &h1.by_ovl[i], &sw.ovl0[i], false);
	salsa64_log("hilo 1 sobre la emulación, campos por solape: < 0,1 ms %s; 0,1–1 ms %s; 1–3 ms %s; ≥ 3 ms %s", g[0],
		    g[1], g[2], g[3]);
	if (h1.load) {
		for (int i = 0; i < LOAD_PHASES; i++)
			ovl_text(g[i], sizeof g[i], &h1.by_load[i], &sw.load0[i], true);
		salsa64_log("carga-hilo1 por fase: 0 µs %s; %u µs %s; %u µs %s", g[0], h1.load_us[1], g[1], h1.load_us[2],
			    g[2]);
	}
	h1_window_reset();
}

/* Perfilado: la ventana de estadísticas como registro VENTANA (perfilado.h),
 * con lo mismo que va al log y lo del recompilador, el audio y el muestreo.
 * Antes de que stats() reinicie la ventana. */
static struct {
	uint64_t compile_ticks, rec_compiled, rec_retrans, rec_words, rec_evictions, rec_flushes, rec_killed, interp, jr;
	struct salsa64_rec_trace_prev slow;
	struct salsa64_rec_trace_compile_stats compiles;
	uint64_t smp_ticks;
	uint32_t marks[SALSA64_SMP_N];
	struct audio_sync_stats audio;
	uint64_t audio_in;
	uint32_t worst_us;
} pwin;

static void perf_window(uint64_t dt, uint32_t fields, uint64_t us_emu, uint64_t us_copy, uint64_t us_pace, uint64_t cyc,
			uint64_t idle, uint64_t h1_busy, uint64_t rt_emu_ns, uint64_t rt_pres_ns,
			const struct salsa64_rdp_stats *rs)
{
	const struct salsa64_rdp_stats *r0 = &sw.rdp0;
	uint64_t hz = spukit_tick_hz();
	uint32_t tasks = rs->tasks - r0->tasks;
	uint32_t v[PERF_W_N];
	memset(v, 0, sizeof v);
#define W(name, value) v[PERF_W_##name] = (uint32_t)(value)
	W(campo, n64.bus.vi.fields);
	W(dur_ms, dt / 1000);
	W(campos, fields);
	W(cps_x100, dt ? fields * 100000000ull / dt : 0);
	W(velocidad_pm, dt ? fields * 1e9 / dt * n64.bus.vi.field_cycles / 93750000.0 : 0);
	W(mips_x10, dt ? (cyc - idle) * 10 / dt : 0);
	W(espera_pm, cyc ? idle * 1000 / cyc : 0);
	W(emu_us_campo, fields ? us_emu / fields : 0);
	W(copia_vi_us_campo, fields ? us_copy / fields : 0);
	W(ritmo_us_campo, fields ? us_pace / fields : 0);
	W(emu_peor_us, perf_worst_us);
	perf_worst_us = 0;
	W(presentados, pres.presented);
	W(saltados, pres.skipped);
	W(descartados, emu_stats.dropped);
	W(sin_imagen, pres.blank);
	W(del_rsx, pres.rsx_frames);
	W(cola_x100, pres.presented ? pres.behind_sum * 100 / pres.presented : 0);
	W(cola_max, pres.behind_max);
	W(edad_x100, pres.rsx_frames ? (rs->age_sum - r0->age_sum) * 100 / pres.rsx_frames : 0);
	W(edad_max, rs->age_max);
	W(hilo1_pm, dt && hz ? h1_busy * 1000000000ull / hz / dt : 0);
	W(rsx_emu_us_campo, fields ? rt_emu_ns / 1000 / fields : 0);
	W(rsx_pres_us_cuadro, pres.presented ? rt_pres_ns / 1000 / pres.presented : 0);
	W(tareas, tasks);
	W(dibujos, rs->draws - r0->draws);
	W(triangulos, rs->triangles - r0->triangles);
	W(spe_us, gfx_on_spu && hz ? (gfx_spu.spu_ticks - sw.gfx_ticks0) * 1000000 / hz : 0);
	W(espera_spe_us, gfx_on_spu ? gfx_spu.wait_us - sw.gfx_wait0 : 0);
	W(traduccion_us, rs->us_translate - r0->us_translate);
	W(esperas_juego, rs->set_waits - r0->set_waits);
	W(espera_juego_us, rs->us_set_wait - r0->us_set_wait);
	W(copias_rdram, rs->readbacks - r0->readbacks);
	W(copias_rdram_us, rs->us_readback - r0->us_readback);
	W(copias_esperadas, rs->readback_waits - r0->readback_waits);
	W(copias_espera_us, rs->us_readback_wait - r0->us_readback_wait);
	W(copias_tarde, rs->readback_late - r0->readback_late);
	W(copias_perdidas, rs->readbacks_lost - r0->readbacks_lost);
	W(tareas_cortadas, n64.bus.rsp.stats.cut - sw.gfx_cut0);
	W(comandos_saltados, gfx_spu.skipped - sw.gfx_skipped0);
	W(fp_cambios, rs->fp_loads - r0->fp_loads);
	W(fp_generados, rs->fp_generated - r0->fp_generated);
	W(destinos_creados, rs->targets_created - r0->targets_created);
	W(destinos_reciclados, rs->targets_evicted - r0->targets_evicted);
	const struct audio_sync_stats *a = audio_out_stats();
	if (a) {
		uint64_t out = a->out_frames - pwin.audio.out_frames;
		W(audio_salida_ms, out * 1000 / AUDIO_OUT_RATE);
		W(audio_silencio_pm, out ? (a->silent_frames - pwin.audio.silent_frames) * 1000 / out : 0);
		W(audio_estirado_pm, out ? (a->stretch_frames - pwin.audio.stretch_frames) * 1000 / out : 0);
		W(audio_faltas, a->underruns - pwin.audio.underruns);
		W(audio_silencios, a->silences - pwin.audio.silences);
		W(audio_saltos, a->skips - pwin.audio.skips);
		W(audio_descartadas, a->dropped - pwin.audio.dropped);
		pwin.audio = *a;
	}
	if (use_rec && rec.trace) {
		struct salsa64_rec_trace *t = rec.trace;
		W(rec_bloques, t->compiled - pwin.rec_compiled);
		W(rec_retrad, t->retrans - pwin.rec_retrans);
		W(rec_palabras, t->words - pwin.rec_words);
		W(rec_desalojos, t->evictions - pwin.rec_evictions);
		W(rec_vaciados, t->flushes - pwin.rec_flushes);
		W(rec_muertos, t->killed - pwin.rec_killed);
		pwin.rec_compiled = t->compiled;
		pwin.rec_retrans = t->retrans;
		pwin.rec_words = t->words;
		pwin.rec_evictions = t->evictions;
		pwin.rec_flushes = t->flushes;
		pwin.rec_killed = t->killed;
		struct salsa64_rec_trace_slow_stats delta;
		perf_put_slow_window(&perf.emu, perf_ms(), t, &pwin.slow, &delta);
		struct salsa64_rec_trace_compile_stats cdelta = {0};
		for (unsigned k = 0; k < SALSA64_RT_COMPILE_N; k++) {
			cdelta.calls[k] = t->compiles.calls[k] - pwin.compiles.calls[k];
			cdelta.ticks[k] = t->compiles.ticks[k] - pwin.compiles.ticks[k];
			cdelta.words[k] = t->compiles.words[k] - pwin.compiles.words[k];
			pwin.compiles.calls[k] = t->compiles.calls[k];
			pwin.compiles.ticks[k] = t->compiles.ticks[k];
			pwin.compiles.words[k] = t->compiles.words[k];
		}
		cdelta.max_field_compiles = t->compiles.max_field_compiles;
		cdelta.max_field = t->compiles.max_field;
		perf_put_compile(&perf.emu, perf_ms(), &cdelta);
		static const char *const slow_names[SALSA64_RT_SLOW_N] = {
			"KSEG0", "KSEG1", "TLB RDRAM", "TLB E/S", "TLB excepción", "desalineado",
			"SP", "DP", "MI", "VI", "AI", "PI", "RI", "SI", "PIF", "cartucho",
			"escritura de código", "COP0", "FPU", "CACHE", "LL/SC", "trampa", "otra instrucción", "excepción",
			"INT", "TLB modificada", "TLB lectura", "TLB escritura", "ADEL", "ADES", "IBE", "DBE", "SYSCALL",
			"BREAK", "RI", "CPU", "OV", "TR", "FPE", "WATCH"
		};
		unsigned best[3] = { SALSA64_RT_SLOW_N, SALSA64_RT_SLOW_N, SALSA64_RT_SLOW_N };
		for (unsigned k = 0; k < SALSA64_RT_SLOW_N; k++)
			for (unsigned j = 0; j < 3; j++)
				if (best[j] == SALSA64_RT_SLOW_N || delta.ticks[k] > delta.ticks[best[j]]) {
					for (unsigned z = 2; z > j; z--) best[z] = best[z - 1];
					best[j] = k; break;
				}
		salsa64_log("camino lento (5 s): %s %.2f ms/%u; %s %.2f ms/%u; %s %.2f ms/%u",
			slow_names[best[0]], hz ? delta.ticks[best[0]] * 1000.0 / hz : 0.0, delta.calls[best[0]],
			slow_names[best[1]], hz ? delta.ticks[best[1]] * 1000.0 / hz : 0.0, delta.calls[best[1]],
			slow_names[best[2]], hz ? delta.ticks[best[2]] * 1000.0 / hz : 0.0, delta.calls[best[2]]);
	}
	if (use_rec) {
		W(rec_interp, rec.stats.interp_steps - pwin.interp);
		W(rec_jr, n64.cpu.rec_jr_count - pwin.jr);
		W(rec_compilar_us, hz ? (perf_compile_ticks - pwin.compile_ticks) * 1000000 / hz : 0);
		pwin.interp = rec.stats.interp_steps;
		pwin.jr = n64.cpu.rec_jr_count;
		pwin.compile_ticks = perf_compile_ticks;
	}
	if (rsmp.on) {
		uint32_t other = 0;
		for (unsigned m = 1; m < SALSA64_SMP_N; m++) {
			v[PERF_W_m_nativo + m] = rec_sample->marks[m] - pwin.marks[m];
			other += v[PERF_W_m_nativo + m];
			pwin.marks[m] = rec_sample->marks[m];
		}
		W(m_total, rec_sample->ticks - pwin.smp_ticks);
		W(m_nativo, v[PERF_W_m_total] > other ? v[PERF_W_m_total] - other : 0);
		pwin.smp_ticks = rec_sample->ticks;
	}
	W(mandos_hueco_us, pad_poll.max_gap_us);
	W(perdidos, perf.emu.lost + perf.pres.lost);
#undef W
	perf_put_values(&perf.pres, PERF_R_WINDOW, perf_ms(), v, PERF_W_N);
}

static void stats(uint64_t now)
{
	const struct salsa64_boot_info *info = &boot_info;

	if (!sw.t_stats) {
		sw.t_stats = now;
		h1_window_reset();
		salsa64_rdp_get_stats(&sw.rdp0);
		pad_poll.polls = 0;
		pad_poll.max_gap_us = 0;
		return;
	}
	uint64_t dt = now - sw.t_stats;
	if (dt < STATS_US)
		return;
	/* Los contadores de emulación se leen sin lock: palabras de 64 bits
	 * alineadas, una línea de estadísticas. */
	uint32_t fields = emu_stats.fields - sw.fields0;
	uint64_t us_emu = emu_stats.us_emu - sw.us_emu0, us_copy = emu_stats.us_copy - sw.us_copy0;
	uint64_t us_pace = emu_stats.us_pace - sw.us_pace0;
	uint64_t cyc = n64.cpu.cycles - sw.cyc0, idle = n64.cpu.idle_cycles - sw.idle0;
	salsa64_log("%.1f campos/s (%.0f %% de %s), %.1f MIPS, %.0f %% en bucles de espera; "
		    "emulación %.1f ms por campo (copia del VI %.2f), ritmo %.1f ms (%u reinicios); presentados %u "
		    "(%.1f ms + flip %.1f ms cada uno), %u saltados, %u descartados, %u sin imagen, %u del RSX",
		    fields * 1e6 / dt, fields * 1e6 / dt * 100.0 * n64.bus.vi.field_cycles / 93750000.0,
		    info->tv == SALSA64_TV_PAL ? "PAL" : "NTSC", (double)(cyc - idle) / dt,
		    cyc ? 100.0 * idle / cyc : 0.0, fields ? us_emu / 1000.0 / fields : 0.0,
		    fields ? us_copy / 1000.0 / fields : 0.0, fields ? us_pace / 1000.0 / fields : 0.0,
		    pacer.resets, pres.presented,
		    pres.presented ? pres.us_present / 1000.0 / pres.presented : 0.0,
		    pres.presented ? pres.us_flip / 1000.0 / pres.presented : 0.0, pres.skipped,
		    emu_stats.dropped, pres.blank, pres.rsx_frames);
	uint64_t h1_busy = busy_at(&h1.busy, tb_now()) - sw.h1_busy0;  /* para el perfilado (h1_stats lo reinicia) */
	h1_stats(fields, dt);
	/* El RSX: lo de la emulación por campo emulado y lo de presentar
	 * (imagen, filtro, avisos) por campo presentado, y el filtro por
	 * pasada. */
	rt_collect();
	if (rt.n[RT_EMU] || rt.n[RT_PRES])
		salsa64_log("rsx: emulación %.2f ms por campo emulado, presentación %.2f ms por campo presentado (%.0f %% "
			    "del tiempo ocupado en total; cota superior)",
			    fields ? rt.ns[RT_EMU] / 1e6 / fields : 0.0,
			    pres.presented ? rt.ns[RT_PRES] / 1e6 / pres.presented : 0.0,
			    dt ? (rt.ns[RT_EMU] + rt.ns[RT_PRES]) / 10.0 / dt : 0.0);
	uint64_t rt_emu_ns = rt.ns[RT_EMU], rt_pres_ns = rt.ns[RT_PRES];
	memset(rt.ns, 0, sizeof rt.ns);
	memset(rt.n, 0, sizeof rt.n);
	{
		char line[1024];
		if (salsa64_filters_report(line, sizeof line))
			for (char *p = line, *nl; p; p = nl) {
				nl = strchr(p, '\n');
				if (nl)
					*nl++ = '\0';
				salsa64_log("rsx: %s", p);
			}
	}
	/* Tareas gráficas: tiempo de la HLE (en el SPE), espera del PPE,
	 * traducción a comandos del RSX y esperas por un juego de búferes. */
	struct salsa64_rdp_stats rs;
	salsa64_rdp_get_stats(&rs);
	/* Retraso de lo que se ve, en campos: la cola entre hilos (campos que la
	 * emulación lleva de ventaja al presentar) y la antigüedad del destino
	 * (campos entre su dibujo y el campo que lo muestra: lo que tarda el
	 * juego en mostrar lo que dibuja, 2 con doble búfer a 30 cps). Para
	 * comparar la sensación de input lag entre compilaciones y juegos. */
	if (pres.presented)
		salsa64_log("retraso: cola %.2f campos (máx %u); destino %.2f campos de antigüedad (máx %u) en %u "
			    "campos del RSX",
			    (double)pres.behind_sum / pres.presented, pres.behind_max,
			    pres.rsx_frames ? (double)(rs.age_sum - sw.rdp0.age_sum) / pres.rsx_frames : 0.0,
			    rs.age_max, pres.rsx_frames);
	/* Controles: cada cuánto se leyeron los mandos (el hueco más largo
	 * es la pulsación más corta que puede perderse) y cuántas veces leyó
	 * el juego el control 1. */
	salsa64_log("controles: %u lecturas de los mandos (hueco máx %.1f ms); el juego leyó el control 1 "
		    "%u veces (%.1f/s)",
		    pad_poll.polls, pad_poll.max_gap_us / 1e3, n64.bus.pif.reads[0] - sw.pif_reads0,
		    (n64.bus.pif.reads[0] - sw.pif_reads0) * 1e6 / dt);
	if (n64.bus.rompg) {
		struct salsa64_rompg_win w;
		const struct salsa64_rompg *rp = n64.bus.rompg;
		salsa64_rompg_window(n64.bus.rompg, &w);
		salsa64_log("rom: por fragmentos%s, %llu lecturas en %.1f s: %llu precargas acertadas, %llu esperas a páginas en "
			    "camino (%.1f ms en total, la más larga %.1f ms), %llu fallos fríos (%.1f ms esperando), %llu "
			    "páginas pedidas por adelantado; disco: %.1f ms de lectura, %.1f ms por página, la más lenta "
			    "%.1f ms; %llu MB leídos desde el arranque del juego, %llu errores",
			    rp->as ? " (lector en segundo plano)" : " (síncrono)", (unsigned long long)w.acc, dt / 1e6,
			    (unsigned long long)w.pf_hit, (unsigned long long)w.wait_n, w.wait_us / 1e3,
			    w.wait_max_us / 1e3, (unsigned long long)w.miss, w.cold_us / 1e3, (unsigned long long)w.ra,
			    w.us / 1e3, w.miss + w.ra ? w.us / 1e3 / (w.miss + w.ra) : 0.0, rp->read_max_us / 1e3,
			    (unsigned long long)(rp->read_bytes >> 20), (unsigned long long)rp->errs);
		n64.bus.rompg->read_max_us = 0;
	}
	/* Cada microcódigo gráfico nuevo, una vez por juego: con su CRC, un
	 * reporte dice qué variante usa sin repetir la sesión. */
	while (n64.bus.rsp.ucodes_logged < n64.bus.rsp.ucodes_n) {
		const struct salsa64_ucode_seen *u = &n64.bus.rsp.ucodes[n64.bus.rsp.ucodes_logged++];
		salsa64_log("gfx: microcódigo %s «%s», código en 0x%06x, CRC-32 %08X (palabras al revés %08X)",
			    salsa64_gbi_name(u->gbi), u->text[0] ? u->text : "sin texto", (unsigned)u->code,
			    (unsigned)u->crc, (unsigned)u->crc_w);
	}
	uint32_t tasks = rs.tasks - sw.rdp0.tasks;
	if (tasks) {
		double spu_ms = 0, wait_ms = 0, sleep_ms = 0;
		uint32_t waits = 0, polls = 0;
		if (gfx_on_spu) {
			spu_ms = (gfx_spu.spu_ticks - sw.gfx_ticks0) * 1e3 / spukit_tick_hz() / tasks;
			wait_ms = (gfx_spu.wait_us - sw.gfx_wait0) / 1e3 / tasks;
			waits = gfx_spu.waits - sw.gfx_waits0;
			polls = gfx_spu.polls - sw.gfx_polls0;
			sleep_ms = (gfx_spu.sleep_us - sw.gfx_sleep0) / 1e3 / tasks;
		}
		salsa64_log("gfx: %u tareas (%.1f/s); por tarea: %.0f dibujos, %.0f triángulos, HLE en el "
			    "SPE %.2f ms, emulación esperando al SPE %.2f ms (%u esperas), %.1f sondeos y %.2f ms "
			    "dormida en bucles de espera, traducción al RSX %.2f ms; %u destinos creados (%u reciclados por "
			    "memoria), %u esperas por un juego de búferes (%.2f ms); %u tareas cortadas, %u comandos "
			    "de otro microcódigo saltados, %u fondos de S2DEX",
			    tasks, tasks * 1e6 / dt, (double)(rs.draws - sw.rdp0.draws) / tasks,
			    (double)(rs.triangles - sw.rdp0.triangles) / tasks, spu_ms, wait_ms, waits,
			    (double)polls / tasks, sleep_ms, (rs.us_translate - sw.rdp0.us_translate) / 1e3 / tasks,
			    rs.targets_created - sw.rdp0.targets_created, rs.targets_evicted - sw.rdp0.targets_evicted,
			    rs.set_waits - sw.rdp0.set_waits,
			    (rs.us_set_wait - sw.rdp0.us_set_wait) / 1e3, n64.bus.rsp.stats.cut - sw.gfx_cut0,
			    (unsigned)(gfx_spu.skipped - sw.gfx_skipped0), (unsigned)(gfx_spu.bg - sw.gfx_bg0));
		/* Ritmo del RCP (rsp-ritmo): lo estimado por tarea y cuántas terminaron
		 * después de lo que habrían terminado sin él (en tiempo de la N64). */
		uint32_t mtasks = n64.bus.rsp.stats.gfx - sw.gfx_tasks0;
		if (n64.bus.rsp.gfx_model && mtasks)
			salsa64_log("gfx: ritmo del RCP al %u %%: %.2f ms de N64 estimados por tarea; %u de %u tareas "
				    "retrasadas, %.2f ms de N64 cada una",
				    n64.bus.rsp.gfx_model, (n64.bus.rsp.stats.model_sum - sw.model_sum0) / 93750.0 / mtasks,
				    n64.bus.rsp.stats.model_held - sw.model_held0, mtasks,
				    n64.bus.rsp.stats.model_held - sw.model_held0
					    ? (n64.bus.rsp.stats.model_wait - sw.model_wait0) / 93750.0 /
						      (n64.bus.rsp.stats.model_held - sw.model_held0)
					    : 0.0);
		if (gfx_on_spu) {
			double k = 1e3 / spukit_tick_hz() / tasks;
			salsa64_log("gfx en el SPE por tarea: vértices %.2f ms, estado %.2f ms (texturas %.2f ms), "
				    "cargas a la TMEM %.2f ms", (gfx_spu.t_vtx - sw.gfx_vtx0) * k,
				    (gfx_spu.t_state - sw.gfx_state0) * k, (gfx_spu.t_tex - sw.gfx_tex0) * k,
				    (gfx_spu.t_load - sw.gfx_load0) * k);
			if (gfx_spu.dpc_jobs)
				salsa64_log("gfx: %u listas del CPU al RDP hechas en el SPE desde el arranque, %.2f ms "
					    "de espera cada una", gfx_spu.dpc_jobs, gfx_spu.dpc_us / 1e3 / gfx_spu.dpc_jobs);
			if (gfx_spu.xbus_runs || rs.tex_waits != sw.rdp0.tex_waits)
				salsa64_log("gfx: XBUS %u desde el arranque (%u con SPE en vuelo, %u esperas, %.2f ms); "
				    "espera diagnóstica %u veces, %.2f ms en la ventana", gfx_spu.xbus_runs,
				    gfx_spu.xbus_inflight, gfx_spu.xbus_waits, gfx_spu.xbus_wait_us / 1e3,
				    rs.tex_waits - sw.rdp0.tex_waits,
				    (rs.us_tex_wait - sw.rdp0.us_tex_wait) / 1e3);
			if (rs.dpc_cont)
				salsa64_log("gfx: listas del RDP: %u trabajos continuaban una lista abierta (GFX_JOB_DPC_CONT, sin "
				    "envejecer destinos; %u de ellos del SPE); %u destinos que el envejecimiento por tramo "
				    "habría vuelto a cebar desde la RDRAM",
				    rs.dpc_cont, (unsigned)gfx_spu.dpc_conts, rs.dpc_cont_saved);
		}
		salsa64_log("gfx: fragment programs: %.1f cambios por tarea, %u dibujos con programa generado "
			    "(%.1f por tarea), %u programas en la caché (%u generados, %u vaciados); %.1f dibujos con "
			    "aristas AA por tarea",
			    (double)(rs.fp_loads - sw.rdp0.fp_loads) / tasks, rs.draws_generated - sw.rdp0.draws_generated,
			    (double)(rs.draws_generated - sw.rdp0.draws_generated) / tasks, rs.fp_programs,
			    rs.fp_generated - sw.rdp0.fp_generated, rs.fp_evictions - sw.rdp0.fp_evictions,
			    (double)(rs.draws_edges - sw.rdp0.draws_edges) / tasks);
		uint32_t itasks = rs.interp_tasks - sw.rdp0.interp_tasks;
		uint32_t iskip = rs.interp_skipped - sw.rdp0.interp_skipped;
		if (itasks || iskip) {
			uint32_t iv = rs.interp_vertices - sw.rdp0.interp_vertices;
			uint32_t ip = rs.interp_paired - sw.rdp0.interp_paired;
			uint32_t ifl = rs.interp_fields - sw.rdp0.interp_fields;
			salsa64_log("gfx: interpolación: %u tareas con anterior (%u cortes, %u de ellos por un giro de la "
				    "cámara, %u sin lugar en las tablas, %u con "
				    "texturas de un destino), vértices de salida con pareja %.1f %% (%.0f por tarea), %u grupos "
				    "por el contenido, %u rechazados (saltaron, se deformaron o se aplastarían), %u sin "
				    "interpolar por su objeto o su malla, %u interpolados con su objeto, %u reordenados; "
				    "emparejar %.3f "
				    "ms del PPE por tarea (armar %.3f, emparejar %.3f, posiciones %.3f)",
				    itasks, rs.interp_cuts - sw.rdp0.interp_cuts,
				    rs.interp_cuts_camera - sw.rdp0.interp_cuts_camera, rs.interp_full - sw.rdp0.interp_full,
				    rs.interp_fbtex - sw.rdp0.interp_fbtex, iv ? 100.0 * ip / iv : 0.0,
				    itasks ? (double)iv / itasks : 0.0,
				    rs.interp_content - sw.rdp0.interp_content, rs.interp_rejected - sw.rdp0.interp_rejected,
				    rs.interp_atomic - sw.rdp0.interp_atomic, rs.interp_kept - sw.rdp0.interp_kept,
				    rs.interp_reordered - sw.rdp0.interp_reordered, (rs.us_interp - sw.rdp0.us_interp) / 1e3 / tasks,
				    (rs.us_interp_build - sw.rdp0.us_interp_build) / 1e3 / tasks,
				    (rs.us_interp_pair - sw.rdp0.us_interp_pair) / 1e3 / tasks,
				    (rs.us_interp_prev - sw.rdp0.us_interp_prev) / 1e3 / tasks);
			salsa64_log("gfx: interpolación: %u campos intermedios (%.1f/s), %u enteros (cuadro sin interpolar), %u "
				    "repetidos (no llegaron a tiempo: %u su juego de búferes ya en uso, %u la caché, %u sin "
				    "superficie); redibujar %.3f ms del PPE por campo intermedio; %u tareas sin pedir la "
				    "identidad de los vértices al SPE (el juego a 60 cuadros, o tras una que no cupo)",
				    ifl, ifl * 1e6 / dt, rs.interp_plain - sw.rdp0.interp_plain,
				    rs.interp_repeated - sw.rdp0.interp_repeated, rs.interp_rep_set - sw.rdp0.interp_rep_set,
				    rs.interp_rep_cache - sw.rdp0.interp_rep_cache, rs.interp_rep_surf - sw.rdp0.interp_rep_surf,
				    ifl ? (rs.us_interp_draw - sw.rdp0.us_interp_draw) / 1e3 / ifl : 0.0, iskip);
		}
		uint32_t rb = rs.readbacks - sw.rdp0.readbacks, rbw = rs.readback_waits - sw.rdp0.readback_waits;
		uint32_t trsx = rs.tex_rsx - sw.rdp0.tex_rsx, trd = rs.tex_rdram - sw.rdp0.tex_rdram;
		uint32_t tself = rs.tex_self - sw.rdp0.tex_self, primes = rs.primes - sw.rdp0.primes;
		uint32_t pwaits = rs.prime_waits - sw.rdp0.prime_waits;
		uint32_t mir = rs.mirrors - sw.rdp0.mirrors, mirs = rs.mirrors_skipped - sw.rdp0.mirrors_skipped;
		uint32_t fmir = rs.frames_mirrored - sw.rdp0.frames_mirrored, dmas = rs.dma_skipped - sw.rdp0.dma_skipped;
		uint32_t lost = rs.readbacks_lost - sw.rdp0.readbacks_lost, tfail = rs.targets_failed - sw.rdp0.targets_failed;
		uint32_t tabs = rs.targets_absurd - sw.rdp0.targets_absurd, bfail = rs.binds_failed - sw.rdp0.binds_failed;
		uint32_t tppe = rs.tex_ppe - sw.rdp0.tex_ppe, tppel = rs.tex_ppe_lost - sw.rdp0.tex_ppe_lost;
		uint32_t talpha = rs.targets_alpha - sw.rdp0.targets_alpha, xalpha = rs.tex_alpha - sw.rdp0.tex_alpha;
		uint32_t falpha = rs.alpha_fills - sw.rdp0.alpha_fills;
		uint32_t calpha = rs.alpha_copies - sw.rdp0.alpha_copies;
		uint32_t cvg[4];
		for (int k = 0; k < 4; k++)
			cvg[k] = rs.alpha_cvg[k] - sw.rdp0.alpha_cvg[k];
		/* Las pasadas del alfa de rsxkit (RSXKIT_DSTA_ONE), desde el informe anterior. */
		static uint32_t dsta_prev[3];
		uint32_t dsta[3];
		rsxkit_dsta_stats(&dsta[0], &dsta[1], &dsta[2]);
		uint32_t dp = dsta[0] - dsta_prev[0], ds = dsta[1] - dsta_prev[1], df = dsta[2] - dsta_prev[2];
		memcpy(dsta_prev, dsta, sizeof dsta);
		if (rb || rbw || mir || mirs || fmir || dmas || trsx || trd || tself || primes || lost || tfail || tabs ||
		    bfail || tppe || tppel)
			salsa64_log("gfx: framebuffer: %u copias a la RDRAM (%.2f ms c/u en presentación), la emulación "
				    "esperó %u tareas (%.2f ms c/u, %u tarde), %u copias espejo (%u sin búfer), %u cuadros "
				    "copiados, %u copias descartadas por un DMA posterior; texturas desde un destino: %u del "
				    "RSX, %u de la RDRAM, %u del ligado, %u decodificadas en el PPE (%u sin lugar); %u destinos "
				    "cebados (%u esperaron la barrera de su búfer de cebado); %u copias perdidas, %u destinos sin superficie, %u color images imposibles, %u "
				    "dibujos sin ligar (ninguna profundidad cubría el destino)",
				    rb, rb ? (rs.us_readback - sw.rdp0.us_readback) / 1e3 / rb : 0.0, rbw,
				    rbw ? (rs.us_readback_wait - sw.rdp0.us_readback_wait) / 1e3 / rbw : 0.0,
				    rs.readback_late - sw.rdp0.readback_late, mir, mirs, fmir, dmas, trsx, trd, tself, tppe,
				    tppel, primes, pwaits, lost, tfail, tabs, bfail);
		if (talpha || xalpha || falpha || calpha || cvg[0] || cvg[1] || cvg[2] || cvg[3] || dp || ds || df)
			salsa64_log("gfx: alfa en los destinos: %u destinos con alfa creados, %u texturas leídas de uno "
				    "de ellos, %u rellenos o copias con su alfa, %u copias a la RDRAM con su alfa; "
				    "cobertura de los dibujos: %u con alfa 1, %u con el de la memoria, %u con el del "
				    "fragmento, %u con el mayor; alfa 1 en el RSX: %u con la pasada del alfa, %u en una "
				    "pasada (el alfa ya era 1), %u sin poder (quedó el del fragmento)",
				    talpha, xalpha, falpha, calpha, cvg[0], cvg[1], cvg[2], cvg[3], dp, ds, df);
		if (rs.big8_sync != sw.rdp0.big8_sync || rs.big8_nosync != sw.rdp0.big8_nosync ||
		    rs.big8_primes != sw.rdp0.big8_primes)
			salsa64_log("gfx: copias big8: %u esperadas, %u sin esperar; %u hechas, cambiaron %u píxeles de la "
				    "RDRAM; %u capas cebadas desde la RDRAM",
				    rs.big8_sync - sw.rdp0.big8_sync, rs.big8_nosync - sw.rdp0.big8_nosync,
				    rs.big8_copies - sw.rdp0.big8_copies, rs.big8_changed - sw.rdp0.big8_changed,
				    rs.big8_primes - sw.rdp0.big8_primes);
		if (rs.layer8_tasks != sw.rdp0.layer8_tasks || rs.layer8_refused != sw.rdp0.layer8_refused) {
			uint32_t l8t = rs.layer8_tasks - sw.rdp0.layer8_tasks;
			uint32_t fpx = rs.layer8_fast_pixels - sw.rdp0.layer8_fast_pixels;
			uint32_t spx = rs.layer8_slow_pixels - sw.rdp0.layer8_slow_pixels;
			double fms = (rs.tb_layer8_fast - sw.rdp0.tb_layer8_fast) * 1e3 / spukit_tick_hz();
			double sms = (rs.tb_layer8_slow - sw.rdp0.tb_layer8_slow) * 1e3 / spukit_tick_hz();
			salsa64_log("gfx: capas de 8 bits en el PPE: %u tareas, %u dibujos, %u píxeles, %.3f ms por tarea "
				    "(rectángulos 1:1: %u píxeles, %.3f ms, %.1f ns/px; genérico: %u píxeles, %.3f ms, %.1f ns/px); "
				    "%u esperas por la copia evitadas; %u destinos quedaron para el RSX (dibujo no soportado)",
				    l8t, rs.layer8_draws - sw.rdp0.layer8_draws, rs.layer8_pixels - sw.rdp0.layer8_pixels,
				    l8t ? (rs.tb_layer8 - sw.rdp0.tb_layer8) * 1e3 / spukit_tick_hz() / l8t : 0.0, fpx, fms,
				    fpx ? fms * 1e6 / fpx : 0.0, spx, sms, spx ? sms * 1e6 / spx : 0.0,
				    rs.layer8_waits_avoided - sw.rdp0.layer8_waits_avoided, rs.layer8_refused - sw.rdp0.layer8_refused);
		}
		if (rs.mask32_tasks != sw.rdp0.mask32_tasks || rs.mask32_refused != sw.rdp0.mask32_refused ||
		    rs.mask32_restored != sw.rdp0.mask32_restored) {
			uint32_t m32t = rs.mask32_tasks - sw.rdp0.mask32_tasks;
			salsa64_log("gfx: imágenes chicas de 32 bits en el PPE: %u tareas, %u dibujos, %u píxeles, %.3f ms "
				    "por tarea; %u bytes a la RDRAM y %u repuestos tras un relleno; %u quedaron para el RSX "
				    "(dibujo no soportado)",
				    m32t, rs.mask32_draws - sw.rdp0.mask32_draws, rs.mask32_pixels - sw.rdp0.mask32_pixels,
				    m32t ? (rs.tb_mask32 - sw.rdp0.tb_mask32) * 1e3 / spukit_tick_hz() / m32t : 0.0,
				    rs.mask32_bytes - sw.rdp0.mask32_bytes, rs.mask32_restored - sw.rdp0.mask32_restored,
				    rs.mask32_refused - sw.rdp0.mask32_refused);
		}
		uint32_t gcop = rs.guard_copies - sw.rdp0.guard_copies, glin = rs.guard_lines - sw.rdp0.guard_lines;
		uint64_t gtb = rs.tb_guard_mark - sw.rdp0.tb_guard_mark;
		uint32_t ccop = rs.claim_copies - sw.rdp0.claim_copies, clin = rs.claim_lines - sw.rdp0.claim_lines;
		if (gcop || gtb)
			salsa64_log("gfx: guarda contra el CPU: %u copias tardías con %u líneas de 128 bytes sin escribir "
				    "(algo que no es el RDP las cambió después de su tarea); huellas %.3f ms por tarea en el "
				    "hilo de emulación; %u espejos con %u líneas sin escribir porque otra color image las "
				    "dibujó después",
				    gcop, glin, tasks ? gtb * 1e3 / spukit_tick_hz() / tasks : 0.0, ccop, clin);
		uint32_t zwo = rs.z_write_only - sw.rdp0.z_write_only, zcl = rs.zclears - sw.rdp0.zclears;
		uint32_t zpend = rs.zclears_pending - sw.rdp0.zclears_pending, zmrg = rs.zclears_merged - sw.rdp0.zclears_merged;
		uint32_t zcol = rs.z_as_color - sw.rdp0.z_as_color, zup = rs.z_uploads - sw.rdp0.z_uploads;
		uint32_t zaut = rs.z_auto - sw.rdp0.z_auto, palias = rs.primes_alias - sw.rdp0.primes_alias;
		uint32_t ppc = rs.ppe_copies - sw.rdp0.ppe_copies, pps = rs.ppe_copies_same - sw.rdp0.ppe_copies_same;
		uint32_t zpart = rs.z_uploads_partial - sw.rdp0.z_uploads_partial;
		if (palias || ppc)
			salsa64_log("gfx: %u destinos cebados desde la RDRAM porque su memoria se dibujó después con otro ancho; "
				    "%u rectángulos de copia escritos por el PPE en imágenes de más de 480 filas (%u iguales, sin "
				    "reescribir); %u z cargadas solo en las filas tocadas",
				    palias, ppc, pps, zpart);
		if (zwo || zcl || zcol || zup || zaut)
			salsa64_log("gfx: profundidad: %u dibujos escriben la z sin probarla (%.1f por tarea), %u borrados de la z "
				    "(%u pendientes hasta ligarla, %u juntados por no caber), %u color images en una z image, "
				    "%u z cargadas desde la RDRAM, %u borrados enteros puestos (la z que el juego no borra entera)",
				    zwo, tasks ? (double)zwo / tasks : 0.0, zcl, zpend, zmrg, zcol, zup, zaut);
	}
	if (lle_mode != SALSA64_LLE_OFF) {
		uint32_t t = rsp_spu.tasks - sw.lle_tasks0;
		salsa64_rsp_spu_stuck(&rsp_spu);
		if (t) {
			uint64_t instr = rsp_spu.instructions - sw.lle_instr0, ticks = rsp_spu.spu_ticks - sw.lle_ticks0;
			double us = ticks * 1e6 / spukit_tick_hz();
			salsa64_log("rsp lle: %u tareas (%u trabajos), %.0f instrucciones por tarea, %.2f ms en el SPE "
				    "por tarea, %.1f ns por instrucción (%.1f MIPS; el RSP real ~62), esperando %.2f ms, "
				    "%.1f sondeos y %.2f ms dormida por tarea, %u interrupciones, %u tramos del RDP",
				    t, rsp_spu.jobs - sw.lle_jobs0, (double)instr / t, us / 1e3 / t,
				    instr ? us * 1e3 / instr : 0.0, us ? instr / us : 0.0,
				    (rsp_spu.wait_us - sw.lle_wait0) / 1e3 / t, (double)(rsp_spu.polls - sw.lle_polls0) / t,
				    (rsp_spu.sleep_us - sw.lle_sleep0) / 1e3 / t, rsp_spu.irqs, rsp_spu.dpc_entries);
			salsa64_log("rsp lle COP0 por tarea: SP_STATUS %.0f lecturas/%.0f escrituras, semáforo %.0f/%.0f, "
				    "DMA %.0f filas dentro/%.0f fuera, DP %.0f lecturas, %.0f reintentos atómicos",
				    (double)rsp_spu.status_reads / rsp_spu.tasks, (double)rsp_spu.status_writes / rsp_spu.tasks,
				    (double)rsp_spu.sem_reads / rsp_spu.tasks, (double)rsp_spu.sem_writes / rsp_spu.tasks,
				    (double)rsp_spu.dma_rows_in / rsp_spu.tasks, (double)rsp_spu.dma_rows_out / rsp_spu.tasks,
				    (double)rsp_spu.dp_reads / rsp_spu.tasks, (double)rsp_spu.atomic_retries / rsp_spu.tasks);
		}
		sw.lle_tasks0 = rsp_spu.tasks;
		sw.lle_jobs0 = rsp_spu.jobs;
		sw.lle_polls0 = rsp_spu.polls;
		sw.lle_instr0 = rsp_spu.instructions;
		sw.lle_ticks0 = rsp_spu.spu_ticks;
		sw.lle_wait0 = rsp_spu.wait_us;
		sw.lle_sleep0 = rsp_spu.sleep_us;
	}
	if (use_rec) {
		const struct salsa64_rec_stats *st = &rec.stats;
		salsa64_log("recompilador: %llu bloques (%llu fuera de KSEG0, %llu desprendidos, %llu enganchados, %llu matados por ids), %u KB de caché, %llu entradas, "
			    "%llu enlaces, %llu invalidados, %llu vaciados (ids %llu, enlaces %llu, RDRAM %llu, bucles %llu, "
			    "ROM %llu, estado %llu, otros %llu), %llu desalojos, %llu traducciones, %llu dependen de FR "
			    "(%llu gemelos, %llu salidas por FR, %llu por CU1, %llu cortados por CU1), %llu instr. interpretadas",
			    (unsigned long long)st->compiled, (unsigned long long)st->compiled_x, (unsigned long long)st->detached,
			    (unsigned long long)st->rekeyed, (unsigned long long)st->reclaimed,
			    salsa64_rec_used(&rec) / 256, (unsigned long long)st->entries,
			    (unsigned long long)st->linked, (unsigned long long)st->invalidated,
			    (unsigned long long)st->flushes, (unsigned long long)st->flushes_by[SALSA64_REC_FLUSH_IDS],
			    (unsigned long long)st->flushes_by[SALSA64_REC_FLUSH_LINKS],
			    (unsigned long long)st->flushes_by[SALSA64_REC_FLUSH_RDRAM],
			    (unsigned long long)st->flushes_by[SALSA64_REC_FLUSH_IDLE],
			    (unsigned long long)st->flushes_by[SALSA64_REC_FLUSH_ROM],
			    (unsigned long long)st->flushes_by[SALSA64_REC_FLUSH_STATE],
			    (unsigned long long)st->flushes_by[SALSA64_REC_FLUSH_OTHER], (unsigned long long)st->evictions,
			    (unsigned long long)st->vt_probes, (unsigned long long)st->fr_dep,
			    (unsigned long long)st->fr_twins, (unsigned long long)st->fr_exits,
			    (unsigned long long)st->cu1_exits, (unsigned long long)st->cu1_cuts,
			    (unsigned long long)st->interp_steps);
		if (rec.opts & SALSA64_REC_OPT_JTC_STATS) {
			uint64_t h = rec.jtc_ctr[0], m = rec.jtc_ctr[1];  /* uint32_t: dan la vuelta a los 4 G */
			salsa64_log("caché de destinos de JR: %llu aciertos, %llu fallos (%.1f %% de acierto)",
				    (unsigned long long)h, (unsigned long long)m, h + m ? 100.0 * (double)h / (double)(h + m) : 0.0);
		}
		if (rec.fastmem || st->fm_off) {
			uint32_t fa, fu, fs;
			salsa64_ps3_fastmem_counts(&fa, &fu, &fs);
			salsa64_log("fastmem: %s; %llu accesos sin prueba, %llu parchados tras fallar; ayudante: %u fallos atendidos, "
				    "%u desconocidos, %u de SPE",
				    rec.fastmem ? "activo" : "apagado", (unsigned long long)st->fm_sites,
				    (unsigned long long)st->fm_faults, fa, fu, fs);
		}
		if (rsmp.on) {
			char text[320];
			salsa64_rec_sample_split(rec_sample, &rsmp.prev, text, sizeof text);
			salsa64_log("%s", text);
		}
		if (rprof.on)
			salsa64_log("perfil del recompilador: %u entradas (%u calientes, %u sin lugar) y %u sitios de salida "
				    "(%u sin lugar), %llu bloques en la zona caliente (%u de %u KB en la mitad %u, %llu no "
				    "cupieron, %llu desalojos de mitad), %llu entradas a bloques contadas, %u escrituras (%u "
				    "fallidas)",
				    rec_profile->count, rec_profile->hot_entries, rec_profile->full, rec_profile->nsites,
				    rec_profile->sites_full, (unsigned long long)st->hot_placed, salsa64_rec_hot_used(&rec) / 256,
				    rec.hot_words / 256, rec.cur_hot, (unsigned long long)st->hot_full,
				    (unsigned long long)st->hot_evictions, (unsigned long long)rec_profile->session, rprof.saves,
				    rprof.save_errors);
		if (rec.prof) {
			char text[2048];
			salsa64_rec_prof_report(rec.prof, &rec_prof_prev, spukit_tick_hz(), fields, text, sizeof text);
			for (char *l = text, *e; l && *l; l = e) {
				if ((e = strchr(l, '\n')))
					*e++ = '\0';
				salsa64_log("%s", l);
			}
		}
	}
	if (perf.on)
		perf_window(dt, fields, us_emu, us_copy, us_pace, cyc, idle, h1_busy, rt_emu_ns, rt_pres_ns, &rs);
	sw.gfx_vtx0 = gfx_spu.t_vtx;
	sw.gfx_state0 = gfx_spu.t_state;
	sw.gfx_tex0 = gfx_spu.t_tex;
	sw.gfx_load0 = gfx_spu.t_load;
	sw.rdp0 = rs;
	sw.gfx_ticks0 = gfx_spu.spu_ticks;
	sw.gfx_wait0 = gfx_spu.wait_us;
	sw.gfx_waits0 = gfx_spu.waits;
	sw.gfx_polls0 = gfx_spu.polls;
	sw.gfx_sleep0 = gfx_spu.sleep_us;
	sw.gfx_cut0 = n64.bus.rsp.stats.cut;
	sw.gfx_tasks0 = n64.bus.rsp.stats.gfx;
	sw.model_held0 = n64.bus.rsp.stats.model_held;
	sw.model_wait0 = n64.bus.rsp.stats.model_wait;
	sw.model_sum0 = n64.bus.rsp.stats.model_sum;
	sw.gfx_skipped0 = gfx_spu.skipped;
	sw.gfx_bg0 = gfx_spu.bg;
	sw.t_stats = now;
	sw.pif_reads0 = n64.bus.pif.reads[0];
	pad_poll.polls = 0;
	pad_poll.max_gap_us = 0;
	sw.fields0 = emu_stats.fields;
	sw.us_emu0 = emu_stats.us_emu;
	sw.us_copy0 = emu_stats.us_copy;
	sw.us_pace0 = emu_stats.us_pace;
	sw.cyc0 = n64.cpu.cycles;
	sw.idle0 = n64.cpu.idle_cycles;
	pres.us_present = pres.us_flip = 0;
	pres.presented = pres.blank = pres.rsx_frames = pres.skipped = 0;
	pres.behind_sum = pres.behind_max = 0;
}

/* ---- Sesión de juego ------------------------------------------------------------ */

enum game_end {
	GAME_TO_MENU,  /* START + SELECT con menú, o fin de una prueba abierta desde el menú */
	GAME_QUIT,     /* salir de la aplicación (sin menú, o «Salir del juego» del sistema) */
	GAME_FAILED,   /* no arrancó */
	GAME_RESTART,  /* «Reiniciar desde cero»: run_game lo atiende y abre el mismo juego otra vez */
};

/* Con la emulación quieta en el límite de un campo (state_parked): lo que
 * quedaba en las colas se presenta (el campo con FIELD_STATE deja la
 * miniatura), el RSX termina todo y se guarda o se carga. Al cargar se
 * olvida lo que era de la imagen anterior: destinos del RSX, audio en cola,
 * el último campo encolado y las ventanas que cuentan campos y ciclos (el
 * número de campo retrocede). La caché del recompilador la vacía
 * salsa64_state_load. */
static void state_barrier(void)
{
	salsa64_log("estado: barrera: emulación quieta en el campo %u (tarea del RSP %s)", n64.bus.vi.fields,
		    state_quiet ? "recogida" : "sin recoger");
	drain(sysGetSystemTime(), 1);
	rsxkit_finish();
	salsa64_log("estado: barrera: colas vacías y RSX terminado");
	/* Ninguna copia RSX → RDRAM en vuelo: al guardar quedan en el estado y
	 * al cargar no llegan tarde sobre la RDRAM cargada. */
	salsa64_rdp_settle();
	salsa64_log("estado: barrera: copias RSX → RDRAM en vuelo terminadas");
	int req = state_req;
	unsigned slot = state_req_slot;
	char err[160] = "";
	struct states_timing t;
	uint32_t field0 = n64.bus.vi.fields;
	bool ok = false;
	if (req == STATE_RESET) {
		/* NMI del botón Reset, con la emulación quieta en el límite del campo.
		 * La tarea del RSP en vuelo (HLE, LLE o audio) la espera
		 * salsa64_rsp_reset sin dejar el SPE a medias; lo que esa tarea
		 * dejó en las colas se traduce y se suelta como al cargar un estado.
		 * El SPE 0 conserva su imagen: si el juego vuelve a cargar otro
		 * microcódigo, cada tarea elige la suya como siempre. */
		struct salsa64_boot_info bi;
		ok = salsa64_boot_nmi(&n64, (int)boot_info.tv, &bi);
		if (ok) {
			boot_info = bi;
			salsa64_rdp_flush(~0u);
			salsa64_rdp_drop_targets();
			audio_out_flush();
			post_last = ~0u;
			for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++)
				pif_edge_seen[i] = n64.bus.pif.sent[i].buttons;
			sw.cyc0 = n64.cpu.cycles;
			sw.idle0 = n64.cpu.idle_cycles;
			sw.pif_reads0 = n64.bus.pif.reads[0];
			osd_fps_reset(&fps);
			salsa64_log("reinicio: NMI en el campo %u: entrada %08x, osResetType %u, RDRAM conservada", field0,
				    bi.entry, (unsigned)salsa64_rd32(n64.bus.rdram + 0x30C));
		} else {
			salsa64_log("reinicio: FALLO del NMI: %s (se cierra el juego)", bi.error ? bi.error : "?");
			running = 0;
		}
		soft_reset_req = 0;
	} else if (!state_quiet) {
		osd_notice(sysGetSystemTime(), "Estados no disponibles con lle");
	} else if (req == STATE_SAVE) {
		ok = states_save(&n64, slot, &t, err, sizeof err);
		if (ok) {
			osd_notice(sysGetSystemTime(), "Estado guardado en ranura %u", slot);
			if (t.async)
				salsa64_log("estado: ranura %u (campo %u): emulación detenida %.1f ms (la foto), miniatura %.1f ms; "
					    "el archivo se escribe en segundo plano", slot, field0, t.us_total / 1e3,
					    states_thumb_us() / 1e3);
			else
				salsa64_log("estado: guardado en la ranura %u (campo %u): %.1f ms, de ellos %.1f ms escribiendo; %u KB; "
					    "miniatura %.1f ms", slot, field0, t.us_total / 1e3, t.us_io / 1e3, t.bytes >> 10,
					    states_thumb_us() / 1e3);
		} else {
			osd_notice(sysGetSystemTime(), "No se pudo guardar: %s", err);
			salsa64_log("estado: FALLO al guardar en la ranura %u: %s", slot, err);
		}
	} else if (req == STATE_LOAD) {
		bool broken = false;
		ok = states_load(&n64, slot, &t, err, sizeof err, &broken);
		if (ok) {
			/* La tarea que esperaba al RDP congelado era de antes de
			 * cargar: se traduce ya, sobre destinos que se sueltan. */
			salsa64_rdp_flush(~0u);
			salsa64_rdp_drop_targets();
			audio_out_flush();
			post_last = ~0u;
			for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++)
				pif_edge_seen[i] = n64.bus.pif.sent[i].buttons;
			gwin.field0 = n64.bus.vi.fields;
			gwin.t0 = 0;
			sw.cyc0 = n64.cpu.cycles;
			sw.idle0 = n64.cpu.idle_cycles;
			sw.pif_reads0 = n64.bus.pif.reads[0];
			osd_fps_reset(&fps);
			osd_notice(sysGetSystemTime(), "Estado cargado");
			salsa64_log("estado: cargado de la ranura %u: campo %u → %u, %.1f ms, de ellos %.1f ms leyendo; %u KB",
				    slot, field0, n64.bus.vi.fields, t.us_total / 1e3, t.us_io / 1e3, t.bytes >> 10);
		} else {
			osd_notice(sysGetSystemTime(), "No se pudo cargar: %s", err);
			salsa64_log("estado: FALLO al cargar la ranura %u: %s%s", slot, err,
				    broken ? " (la emulación quedó a medias: se cierra el juego)" : "");
			if (broken)
				running = 0;
		}
	}
	states_thumb_clear();
	state_last_ok = ok;
	state_done++;
	lwsync();
	state_req = STATE_NONE;
}

/* Una escritura de estado en segundo plano terminó: al log y, si falló, al
 * aviso en pantalla (el de «Estado guardado» salió al tomar la foto). */
static void state_bg_poll(void)
{
	struct states_done d;
	if (!states_poll(&d))
		return;
	if (d.ok) {
		salsa64_log("estado: ranura %u escrita en segundo plano: %.1f ms de punta a punta, %.1f ms en el disco, "
			    "%.1f ms del SPE comprimiendo (%s); %u KB de %u KB (%.1f:1), %u bloques (%u sin comprimir, %u en el SPE)",
			    d.slot, d.us_total / 1e3, d.us_io / 1e3, d.us_spu / 1e3, d.spu ? "SPE" : "PPE", d.bytes >> 10,
			    d.raw_bytes >> 10, d.bytes ? (double)d.raw_bytes / d.bytes : 0.0, d.blocks, d.stored, d.spu_blocks);
	} else {
		osd_notice(sysGetSystemTime(), "No se pudo guardar: %s", d.err);
		salsa64_log("estado: FALLO al escribir la ranura %u en segundo plano: %s", d.slot, d.err);
	}
}

static void state_test_step(void)
{
	uint32_t field = n64.bus.vi.fields;
	switch (state_test.stage) {
	case ST_SAVE:
	case ST_LOAD:
		if (state_req || state_parked ||
		    field < (state_test.stage == ST_SAVE ? state_test.save_at : state_test.load_at))
			return;
		state_test.done0 = state_done;
		state_slot = 9;
		state_command(state_test.stage == ST_SAVE ? STATE_SAVE : STATE_LOAD);
		state_test.stage++;
		return;
	case ST_SAVING:
	case ST_LOADING:
		if (state_done == state_test.done0)
			return;
		salsa64_log("estados, prueba automática: %s %s (campo %u)",
			    state_test.stage == ST_SAVING ? "guardar" : "cargar", state_last_ok ? "bien" : "FALLO", field);
		state_test.stage = !state_last_ok ? ST_FAILED : state_test.stage == ST_SAVING ? ST_LOAD : ST_DONE;
		if (state_test.stage == ST_FAILED && pres.result == SELFTEST_RUNNING) {
			pres.result = SELFTEST_FAILED;
			salsa64_log("%s: FALLO (estado guardado, campo %u)", res_tag, field);
			if (pres.autotest)
				running = 0;
		}
		return;
	default:
		return;
	}
}

/* Lo que pide pause_ctl (menú del sistema, cuadro de salir) pasa a la
 * emulación y al audio; cada cambio, al log. */
static struct {
	uint64_t t0;      /* inicio de la pausa en curso */
	uint32_t still0;  /* cuadros repetidos al empezarla */
} pause_log;

static void image_scale_apply(void);

static void pause_update(void)
{
	uint32_t want = pausectl_reasons(&pause_ctl), was = pause_req;
	if (interp_alt.stage >= 2)
		want |= PAUSECTL_MENU;  /* la pausa de la prueba (interp_alt_step) */
	if (auto_scale.pending)
		want |= PAUSE_SCALE_AUTO;
	if (sys_no_pause)
		want &= ~PAUSECTL_SYSTEM;
	if (want == was)
		return;
	uint64_t now = sysGetSystemTime();
	if (want & ~was & PAUSECTL_CONFIRM)
		salsa64_log("salida: START + SELECT, cuadro «%s» (× sí, ○ no)",
			    pause_ctl.dialog_for == PAUSECTL_RESET ? "¿Seguro? Reiniciar (botón Reset)" :
			    pause_ctl.dialog_for == PAUSECTL_RESTART ? "¿Seguro? Reiniciar desde cero" :
			    in_menu ? "¿Volver al menú?" : "¿Salir al XMB?");
	if (was & ~want & PAUSECTL_CONFIRM)
		salsa64_log("salida: «No», el juego sigue");
	if (!was) {
		pause_log.t0 = now;
		pause_log.still0 = pres.still_frames;
		audio_out_pause(true);
		salsa64_log("pausa: emulación detenida en el campo %u (%s%s%s%s)", n64.bus.vi.fields,
			    want & PAUSECTL_SYSTEM ? "menú del sistema" : "",
			    (want & PAUSECTL_SYSTEM) && (want & ~PAUSECTL_SYSTEM) ? " y " : "",
			    want & PAUSECTL_CONFIRM ? "cuadro de salir" : "", want & PAUSECTL_PANEL ? "lista de trucos" :
			    want & PAUSECTL_MENU ? "menú de pausa" : want & PAUSE_SCALE_AUTO ? "escala automática" : "");
	} else if (!want) {
		if (scale_wanted() != rdp_scale_req)
			image_scale_apply();  /* con la emulación todavía quieta */
		audio_out_pause(false);
		salsa64_log("pausa: la emulación sigue tras %.1f s (%u cuadros repetidos, emulación %s)",
			    (now - pause_log.t0) / 1e6, pres.still_frames - pause_log.still0,
			    emu_paused ? "quieta en el límite del campo" : "todavía sin detenerse");
	}
	lwsync();
	pause_req = want;
}

static void settings_read(struct menu_settings *s);
static void rom_names(const char *path, const char **title, const char **code, uint32_t *crc1, uint32_t *crc2);

/* Los trucos del juego (game_cheats) se cargan al abrirlo. */
static void load_cheats(const char *source)
{
	const char *name = source ? strrchr(source, '/') : NULL;
	name = name ? name + 1 : source;
	memset(&game_cheats, 0, sizeof game_cheats);
	if (!name || !name[0])
		return;
	const char *title = NULL, *code = NULL;
	uint32_t crc1 = 0, crc2 = 0;
	rom_names(source, &title, &code, &crc1, &crc2);
	unsigned n = cheats_load(&game_cheats, CHEATS_DIR, name, title, code, crc1, crc2);
	if (!n) {
		salsa64_log("trucos: ninguno (%s)", game_cheats.cht_path);
		return;
	}
	salsa64_log("trucos: %u en %s, %u activos%s", n, game_cheats.cht_path, cheats_active(&game_cheats),
		    game_cheats.unsupported ? " (hay líneas que no se aplican)" : "");
	if (game_cheats.crc_mismatch)
		salsa64_log("trucos: el CRC de la ROM no es el de ninguna sección del .cht (se usó la primera)");
}

/* juegos/<ROM sin extensión>.cfg: lo que un juego cambia de lo global. */
static void game_cfg_path(const char *rom, char *out, size_t n)
{
	const char *name = strrchr(rom, '/');
	name = name ? name + 1 : rom;
	const char *dot = strrchr(name, '.');
	int len = (int)(dot && dot > name ? (size_t)(dot - name) : strlen(name));
	snprintf(out, n, GAMES_DIR "/%.*s.cfg", len, name);
}

static void game_cfg_read(const char *rom, struct menu_game *g)
{
	char path[320], text[1024];
	menu_game_default(g);
	game_cfg_path(rom, path, sizeof path);
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	size_t n = fread(text, 1, sizeof text - 1, f);
	fclose(f);
	menu_game_parse(g, text, n);
}

static bool game_cfg_write(const char *rom, const struct menu_game *g)
{
	char path[320], text[1024];
	game_cfg_path(rom, path, sizeof path);
	mkdir(GAMES_DIR, 0777);
	int n = menu_game_format(g, text, sizeof text);
	FILE *f = fopen(path, "w");
	bool ok = f && fwrite(text, 1, (size_t)n, f) == (size_t)n;
	if (f && fclose(f) != 0)
		ok = false;
	return ok;
}

/* El CRT de view_apply (filter_setup lo pone al abrir el juego). */
static int view_crt;

/* El filtro CRT (enum salsa64_crt) de un filtro del menú y su densidad (la
 * máscara fina es otro filtro para frontend/filters); SALSA64_CRT_NONE para
 * los demás. Para cambiarlo en vivo: filter_select(menu_filter_crt(…)). */
static int menu_filter_crt(int filter, int density)
{
	if (filter == MENU_FILTER_CRT_MASK && density == MENU_DENSITY_FINE)
		return SALSA64_CRT_MASK_FINE;
	return filter >= MENU_FILTER_CRT_MASK && filter <= MENU_FILTER_CRT_TRINITRON
		       ? SALSA64_CRT_MASK + (filter - MENU_FILTER_CRT_MASK)
		       : SALSA64_CRT_NONE;
}

/* Pide al RDP la escala interna `scale` si no es la que
 * tiene: sin juego corriendo o con la emulación quieta en el límite de un
 * campo (salsa64_rdp_set_scale suelta los destinos; el juego los vuelve a
 * crear a la escala nueva en su cuadro siguiente). */
static void rdp_scale_set(int scale, const char *why)
{
	if (scale == rdp_scale_req)
		return;
	bool cut = false;
	salsa64_rdp_set_scale((uint32_t)scale);
	rdp_scale_req = scale;
	uint32_t es = salsa64_rdp_scale(&cut);
	salsa64_log("escala interna: %u× (pedida%s), %s", es, cut ? ", reducida por memoria del RSX" : "", why);
}

/* Si el límite elegido pasa de CRT_SCALE_MAX con
 * un CRT en pantalla (o con el ciclo de medición, que los pasa todos). */
static bool scale_capped(void)
{
	bool crt = !filter_tests() && (flt.cycle || flt.cur != SALSA64_CRT_NONE);
	int chosen = menu_scale_limit(img.scale, video_cfg.resolution);
	return crt && chosen > CRT_SCALE_MAX;
}

static int scale_limit(void)
{
	return scale_capped() ? CRT_SCALE_MAX : menu_scale_limit(img.scale, video_cfg.resolution);
}

/* La escala que se le pide al RDP: la fija o la que lleva la política. */
static int scale_wanted(void)
{
	return scale_policy.maximum ? scale_policy.current : scale_limit();
}

/* La escala para mostrar: la fija, o la actual y el máximo de la automática. */
static const char *scale_label(char *buf, size_t n)
{
	if (menu_scale_is_auto(img.scale)) {
		snprintf(buf, n, tr("Automática: %d× (hasta %d×)"), scale_wanted(),
			 menu_scale_limit(img.scale, video_cfg.resolution));
		return buf;
	}
	return scale_capped() ? tr("3× (tope con CRT)") : menu_scale_name(img.scale, video_cfg.resolution, buf, n);
}

static void scale_control_configure(bool new_game)
{
	unsigned maximum = (unsigned)scale_limit();
	bool automatic = menu_scale_is_auto(img.scale) && !auto_scale.disabled;
	bool learned = automatic && img.game.stable_scale >= 1 && img.game.stable_scale <= MENU_SCALE_MAX;
	unsigned start = learned ? (unsigned)img.game.stable_scale : maximum;
	if (start > maximum)
		start = maximum;
	if (new_game || !scale_policy.maximum) {
		scale_auto_init(&scale_policy, automatic, start, maximum);
	} else {
		bool was_automatic = scale_policy.automatic;
		scale_policy.automatic = automatic;
		scale_policy.maximum = (uint8_t)maximum;
		if (!automatic) {
			scale_policy.ceiling = (uint8_t)maximum;
			scale_policy.failed = false;
		} else if (learned) {
			scale_policy.ceiling = (uint8_t)start;
			scale_policy.failed = start < maximum;
		} else if (!was_automatic || !scale_policy.failed) {
			scale_policy.ceiling = (uint8_t)maximum;
			scale_policy.failed = false;
		}
		unsigned wanted = automatic ? start : maximum;
		unsigned ceiling = scale_auto_ceiling(&scale_policy);
		if (wanted > ceiling)
			wanted = ceiling;
		scale_policy.current = (uint8_t)(!automatic || wanted > maximum ? maximum : wanted);
		scale_policy.overload_us = 0;
	}
	auto_scale.pending = false;
	auto_scale.t0 = 0;
}

static void scale_control_sample(uint64_t now)
{
	if (auto_scale.disabled || auto_scale.pending || pause_req || state_req || state_parked || filter_tests() ||
	    !pacer.enabled) {
		auto_scale.t0 = 0;
		scale_policy.overload_us = 0;
		return;
	}
	rt_collect();
	uint64_t rsx = rt.total_ns[RT_EMU] + rt.total_ns[RT_PRES];
	if (!auto_scale.t0) {
		auto_scale.t0 = now;
		auto_scale.rsx0 = rsx;
		auto_scale.skipped0 = pres.skipped_total;
		auto_scale.fields0 = n64.bus.vi.fields;
		auto_scale.presented0 = pres.frames_total;
		return;
	}
	uint64_t dt = now - auto_scale.t0;
	if (dt < 1000000u)
		return;
	struct scale_auto_sample s = {
		.duration_us = dt,
		.rsx_ns = rsx - auto_scale.rsx0,
		.period_ns = (n64.bus.vi.field_cycles * 32u + 2u) / 3u,
		.fields = n64.bus.vi.fields - auto_scale.fields0,
		.presented = pres.frames_total - auto_scale.presented0,
		.skipped = (uint32_t)(pres.skipped_total - auto_scale.skipped0),
	};
	auto_scale.t0 = now;
	auto_scale.rsx0 = rsx;
	auto_scale.skipped0 = pres.skipped_total;
	auto_scale.fields0 = n64.bus.vi.fields;
	auto_scale.presented0 = pres.frames_total;
	int from = scale_policy.current;
	struct scale_auto_result r = scale_auto_update(&scale_policy, &s);
	if (r.action == SCALE_AUTO_DOWN) {
		auto_scale.pending = true;
		auto_scale.from = from;
		auto_scale.to = scale_policy.current;
		auto_scale.reason = r;
		salsa64_log("escala automática: %d× → %d×, sin margen tras %.1f s (VI %.1f %%, RSX %.1f %% del período, "
			    "%u presentados, %u saltados); techo vigente %u×", from, scale_policy.current,
			    r.sustained_us / 1e6, r.emu_pm / 10.0, r.rsx_pm / 10.0, s.presented, s.skipped,
			    scale_auto_ceiling(&scale_policy));
	} else if (r.action == SCALE_AUTO_WARN) {
		salsa64_log("escala fija %d×: el RSX no alcanza tras %.1f s (VI %.1f %%, RSX %.1f %% del período, "
			    "%u presentados, %u saltados); se conserva la elección", scale_wanted(),
			    r.sustained_us / 1e6, r.emu_pm / 10.0, r.rsx_pm / 10.0, s.presented, s.skipped);
		osd_notice(now, "%s", tr("El RSX no alcanza; prueba una escala menor"));
	}
}

static void scale_control_apply(void)
{
	if (!auto_scale.pending || !emu_paused)
		return;
	rdp_scale_set(auto_scale.to, "escala automática, RSX sin margen");
	img.game.stable_scale = auto_scale.to;
	if (img.source) {
		bool saved = game_cfg_write(img.source, &img.game);
		salsa64_log("escala automática: estable %d× %s para este juego; techo vigente %u×", auto_scale.to,
			    saved ? "guardada" : "NO guardada", scale_auto_ceiling(&scale_policy));
	}
	char notice[64];
	snprintf(notice, sizeof notice, tr("Escala automática: %d×"), auto_scale.to);
	osd_notice(sysGetSystemTime(), "%s", notice);
	auto_scale.pending = false;
	auto_scale.t0 = 0;
	pause_update();
}

/* La interpolación de img (o la de la prueba, INTERP_TEST_PATH) al RDP. La
 * primera vez que se enciende reserva sus tablas: al abrir el juego o con
 * la emulación en pausa. */
static void interp_apply(void)
{
	int mode = img.interp == MENU_INTERP_GEOMETRY ? SALSA64_INTERP_FIELDS : SALSA64_INTERP_OFF;
	float t = 1.0f;
	if (interp_test_t >= 0.0f)
		mode = SALSA64_INTERP_FIXED, t = interp_test_t;
	if (salsa64_rdp_interp(mode, t) != 0) {
		salsa64_log("interpolación: sin memoria para sus tablas, apagada");
		img.interp = MENU_INTERP_OFF;
		salsa64_rdp_interp(SALSA64_INTERP_OFF, 1.0f);
	}
}

/* Lo de img a la presentación: vista (aspecto y filtro), líneas de barrido,
 * recorte e interpolación. La escala va aparte (rdp_scale_set). */
static void view_apply(void)
{
	pres.view.aspect = img.aspect == MENU_ASPECT_16_9 ? 16.0f / 9.0f : 4.0f / 3.0f;
	pres.view.filter = img.filter == MENU_FILTER_SHARP ? RSXKIT_FILTER_NEAREST : RSXKIT_FILTER_LINEAR;
	scan_level = img.filter == MENU_FILTER_SCAN_SOFT ? 1 : img.filter == MENU_FILTER_SCAN_STRONG ? 2 : 0;
	view_crt = menu_filter_crt(img.filter, img.density);
	pause_ctl.image_density = menu_filter_has_density(img.filter);
	filter_select(view_crt);
	if (img.crop >= MENU_OVERSCAN_OFF && img.crop <= MENU_OVERSCAN_WIDE) {
		uint32_t h = img.crop == MENU_OVERSCAN_WIDE ? 16 : img.crop == MENU_OVERSCAN_NORMAL ? 8 : 0;
		overscan.left = overscan.right = h;
		overscan.top = overscan.bottom = h * 3 / 4;
	}
	interp_apply();
}

static void view_log(const char *what)
{
	char sc[64];
	menu_scale_name(img.scale, video_cfg.resolution, sc, sizeof sc);
	salsa64_log("imagen: %s, filtro %s%s%s, recorte %u %u %u %u, escala %s%s%s%s",
		    img.aspect == MENU_ASPECT_16_9 ? "16:9 estirada" : "4:3", menu_filter_name(img.filter),
		    menu_filter_has_density(img.filter) ? ", densidad " : "",
		    menu_filter_has_density(img.filter) ? (img.density == MENU_DENSITY_FINE ? "fina" : "normal") : "",
		    overscan.left, overscan.right, overscan.top, overscan.bottom, sc,
		    scale_capped() ? " (con el CRT, 3×)" : "",
		    img.interp == MENU_INTERP_GEOMETRY ? ", interpolación por geometría" : "", what);
}

/* Aspecto, filtro, recorte y escala del juego: lo suyo (juegos/) o, si no,
 * lo de menu.cfg, overscan.cfg y video.cfg. Antes de cada juego, después de
 * load_overscan. */
static void load_view(const char *source)
{
	struct menu_settings s;
	struct menu_game g;
	settings_read(&s);
	s.overscan = overscan_preset();  /* el de overscan.cfg */
	s.scale = video_cfg.scale;
	img.source = source;
	if (source)
		game_cfg_read(source, &img.game);
	else
		menu_game_default(&img.game);
	g = img.game;
	if (selftest_on || gfxtest_on || audiotest_on) {
		/* Sus verificaciones leen la pantalla: aspecto, filtro y recorte de
		 * siempre, sin guardar. La escala del juego sí vale (verifican a
		 * cualquier escala): así se prueba el cambio al abrir y al cerrar. */
		int scale = g.scale;
		menu_game_default(&g);
		g.scale = scale;
		s.aspect = MENU_ASPECT_4_3;
		s.filter = MENU_FILTER_SMOOTH;
		s.interp = MENU_INTERP_OFF;
		img.source = NULL;
	}
	img.g_aspect = s.aspect;
	img.g_filter = s.filter;
	img.g_crop = s.overscan;
	img.g_scale = s.scale;
	img.g_density = s.density;
	img.g_interp = s.interp;
	menu_game_effective(&s, &g, &img.aspect, &img.filter, &img.crop, &img.scale);
	img.density = menu_game_density(&s, &g);
	img.interp = menu_game_interp(&s, &g);
	view_apply();
	scan_r.w = 0;
	scan_lines = 0;
	view_log(g.aspect >= 0 || g.filter >= 0 || g.overscan >= 0 || g.scale >= 0 || g.interp >= 0
			 ? " (con ajustes de este juego)"
			 : "");
}

static bool image_scale_save(void)
{
	if (!menu_game_scale_set(&img.game, img.scale) || !img.source)
		return false;
	bool ok = game_cfg_write(img.source, &img.game);
	char name[32];
	menu_scale_name(img.scale, video_cfg.resolution, name, sizeof name);
	salsa64_log("imagen: escala %s %s para este juego", name, ok ? "guardada" : "NO guardada");
	return ok;
}

/* «Guardar para este juego»: en juegos/<ROM>.cfg lo que se ve. Lo que
 * coincide con lo global y el juego no tenía propio queda «global» (sigue
 * a Opciones); un recorte personalizado de overscan.cfg, también. */
static void image_save(void)
{
	uint64_t now = sysGetSystemTime();
	if (!img.source) {
		osd_notice(now, "No se pudieron guardar los ajustes");
		return;
	}
	struct menu_game g = img.game;
	g.aspect = img.aspect == img.g_aspect && img.game.aspect < 0 ? -1 : img.aspect;
	g.filter = img.filter == img.g_filter && img.game.filter < 0 ? -1 : img.filter;
	g.overscan = img.crop > MENU_OVERSCAN_WIDE || (img.crop == img.g_crop && img.game.overscan < 0) ? -1 : img.crop;
	g.scale = img.scale == img.g_scale && img.game.scale < 0 ? -1 : img.scale;
	g.density = img.density == img.g_density && img.game.density < 0 ? -1 : img.density;
	g.interp = img.interp == img.g_interp && img.game.interp < 0 ? -1 : img.interp;
	bool ok = game_cfg_write(img.source, &g);
	if (ok)
		img.game = g;
	salsa64_log("imagen: ajustes de este juego %s (aspecto %d, filtro %d, densidad %d, recorte %d, escala %d, "
		    "interpolación %d)", ok ? "guardados" : "NO guardados", g.aspect, g.filter, g.density, g.overscan,
		    g.scale, g.interp);
	osd_notice(now, ok ? "Ajustes guardados para este juego" : "No se pudieron guardar los ajustes");
}

/* La página «Imagen» del menú de pausa (pausectl): filtro, aspecto y
 * recorte se ven al momento, también en el cuadro en pausa; la escala se
 * pone al seguir jugando (image_scale_apply), porque soltar los destinos
 * con el juego quieto dejaría la pantalla en negro. */
static void accessory_menu_events(void)
{
	int port = pause_ctl.accessory_do;
	if (port < 0)
		return;
	/* Presentación pide la pausa, pero solo emulación puede confirmar que ya
	 * terminó el campo. Mantener la petición pendiente hasta ese acuse evita
	 * tocar los registros del Transfer Pak mientras el juego los usa. */
	lwsync();
	if (!emu_paused)
		return;
	pause_ctl.accessory_do = -1;
	if ((unsigned)port >= SALSA64_PIF_PORTS)
		return;
	int delta = pause_ctl.accessory_arg < 0 ? -1 : 1;
	int current = n64.bus.pif.accessory[port] == SALSA64_PIF_ACCESSORY_NONE ? MENU_ACC_NONE
		      : n64.bus.pif.accessory[port] == SALSA64_PIF_ACCESSORY_RUMBLE_PAK ? MENU_ACC_RUMBLE
		      : n64.bus.pif.accessory[port] == SALSA64_PIF_ACCESSORY_TRANSFER_PAK ? MENU_ACC_TRANSFER : MENU_ACC_PAK;
	int next = (current + delta + MENU_ACCS) % MENU_ACCS;
	n64.bus.pif.accessory[port] = next == MENU_ACC_NONE ? SALSA64_PIF_ACCESSORY_NONE
					 : next == MENU_ACC_RUMBLE ? SALSA64_PIF_ACCESSORY_RUMBLE_PAK
					 : next == MENU_ACC_TRANSFER ? SALSA64_PIF_ACCESSORY_TRANSFER_PAK
					 : SALSA64_PIF_ACCESSORY_CONTROLLER_PAK;
	/* Swapping accessories resets the Transfer Pak control registers. */
	salsa64_transferpak_reset(&n64.bus.pif.transferpak_state[port]);
	/* Replacing a Rumble Pak drops its motor state immediately. */
	n64.bus.pif.rumble[port] = 0;
	n64.bus.pif.pak_err[port] = 0;
	/* The emulation thread owns rumble_box's seqlock writer. The UI already
	 * stops physical motors while this pause page is open. */
	osd_notice(sysGetSystemTime(), "%s", menu_accessory_name(next));
	salsa64_log("accesorio: control %u -> %s", (unsigned)port + 1, menu_accessory_name(next));
}

/* Lo que pide la página «Imagen» del menú de pausa (pausectl). */
static void image_events(void)
{
	int d = pause_ctl.image_do, k = pause_ctl.image_arg;
	if (d < 0)
		return;
	pause_ctl.image_do = -1;
	switch (d) {
	case IM_FILTER:
		img.filter = (img.filter + k + MENU_FILTERS) % MENU_FILTERS;
		break;
	case IM_DENSITY:
		img.density = (img.density + k + MENU_DENSITIES) % MENU_DENSITIES;
		break;
	case IM_ASPECT:
		img.aspect = img.aspect == MENU_ASPECT_16_9 ? MENU_ASPECT_4_3 : MENU_ASPECT_16_9;
		break;
	case IM_CROP:
		/* Apagado → normal → amplio; uno personalizado pasa al primero o al último. */
		img.crop = img.crop > MENU_OVERSCAN_WIDE ? (k > 0 ? MENU_OVERSCAN_OFF : MENU_OVERSCAN_WIDE)
							  : (img.crop + k + 3) % 3;
		break;
	case IM_SCALE:
		img.scale = menu_scale_step(img.scale, video_cfg.resolution, k);
		if (!image_scale_save() && img.source)
			osd_notice(sysGetSystemTime(), "No se pudo guardar la escala");
		scale_control_configure(false);
		return;  /* al seguir */
	case IM_INTERP:
		img.interp = (img.interp + k + MENU_INTERPS) % MENU_INTERPS;
		break;
	case IM_SAVE:
		image_save();
		return;
	default:
		return;
	}
	view_apply();
	scale_control_configure(false);
	view_log(" (menú de pausa)");
}

/* Al seguir jugando con otra escala elegida en «Imagen»: con la emulación
 * quieta en el límite del campo (antes de soltar pause_req). */
static void image_scale_apply(void)
{
	for (int i = 0; i < 100 && !emu_paused && running; i++)
		sysUsleep(2000);  /* una pausa cortísima: que termine su campo */
	if (!emu_paused) {
		salsa64_log("escala interna: la emulación no se detuvo; queda %d", rdp_scale_req);
		return;
	}
	rdp_scale_set(scale_wanted(), scale_capped() ? "menú de pausa, tope de 3× con un CRT" : "menú de pausa");
	char sbuf[64], notice[96];
	snprintf(notice, sizeof notice, tr("Escala interna: %s"), scale_label(sbuf, sizeof sbuf));
	osd_notice(sysGetSystemTime(), "%s", notice);
}

/* INTERP_ALT_PATH: lo mismo que hace el autor desde «Imagen» en el menú de
 * pausa, cada interp_alt.every campos. Con la emulación quieta en el
 * límite del campo (pause_update la pide en la etapa 2) se cambia la
 * interpolación y, si hay dos escalas, la escala; pause_update la suelta
 * INTERP_ALT_HOLD_US después y aplica la escala al seguir. */
static void interp_alt_step(void)
{
	uint64_t now = sysGetSystemTime();
	switch (interp_alt.stage) {
	case 1:
		if (n64.bus.vi.fields >= interp_alt.next && !pause_req && !state_req && !state_parked)
			interp_alt.stage = 2;
		return;
	case 2:
		if (!emu_paused)
			return;
		img.interp = img.interp == MENU_INTERP_GEOMETRY ? MENU_INTERP_OFF : MENU_INTERP_GEOMETRY;
		if (interp_alt.scale[0] > 0)
			img.scale = img.scale == interp_alt.scale[0] ? interp_alt.scale[1] : interp_alt.scale[0];
		view_apply();
		scale_control_configure(false);
		view_log(" (prueba, interp-alternar)");
		interp_alt.toggles++;
		interp_alt.t = now + INTERP_ALT_HOLD_US;
		interp_alt.stage = 3;
		return;
	case 3:
		if (now < interp_alt.t)
			return;
		interp_alt.next = n64.bus.vi.fields + interp_alt.every;
		interp_alt.stage = 1;
		return;
	default:
		return;
	}
}

/* Lo que es de un juego vuelve a como al arrancar el programa: colas entre
 * hilos, ranuras de la copia del VI, contadores y estadísticas, banderas de
 * las pruebas, capturas. Antes de cada run_game. */
static void game_reset(void)
{
	running = 1;
	emu_done = 0;
	selftest_on = gfxtest_on = audiotest_on = 0;
	game_auto_ok = 0;
	use_rec = false;
	lle_mode = SALSA64_LLE_OFF;
	gfx_on_spu = 0;
	memset(&boot_info, 0, sizeof boot_info);
	memset(&save_file, 0, sizeof save_file);
	memset(&mempak_file, 0, sizeof mempak_file);
	memset(field_q, 0, sizeof field_q);
	field_head = field_tail = 0;
	pif_edge_head = pif_edge_tail = pif_edge_lost = 0;
	memset(pif_edge_seen, 0, sizeof pif_edge_seen);
	for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++)
		pad_box.pressed[i] = 0;
	memset(pad_published, 0, sizeof pad_published);
	post_last = ~0u;
	for (int i = 0; i < SNAP_SLOTS; i++)
		snap_state[i] = SNAP_FREE;
	memset((void *)&emu_stats, 0, sizeof emu_stats);
	bench_request = 0;
	memset(&bench, 0, sizeof bench);
	memset(&gwin, 0, sizeof gwin);
	memset(&sw, 0, sizeof sw);
	memset(&pwin, 0, sizeof pwin);
	perf_compile_ticks = 0;
	perf_worst_us = 0;
	memset(h1.by_ovl, 0, sizeof h1.by_ovl);
	memset(h1.by_load, 0, sizeof h1.by_load);
	load_config();
	cap_every = exit_field = 0;
	isv_len = 0;
	unknown_gbi_next = 1;
	pres.shots_on = 0;
	pres.result = SELFTEST_RUNNING;
	pres.dump_field = 0;
	pres.t_shot = 0;
	pres.shots = 0;
	pres.us_present = pres.us_flip = 0;
	pres.presented = pres.blank = pres.rsx_frames = pres.skipped = 0;
	pres.skipped_total = 0;
	pres.behind_sum = pres.behind_max = 0;
	pres.last_origin = pres.game_frames = pres.frames_total = 0;
	pres.still = STILL_NONE;
	pres.t_flip = 0;
	pres.still_frames = 0;
	memset(&rt, 0, sizeof rt);
	rt.open = -1;
	memset(&scale_policy, 0, sizeof scale_policy);
	memset(&auto_scale, 0, sizeof auto_scale);
	pause_req = 0;
	emu_paused = 0;
	pausectl_game_end(&pause_ctl);
	state_req = STATE_NONE;
	state_parked = 0;
	state_quiet = 0;
	soft_reset_req = 0;
	restart_cold = 0;
	state_slot = 0;
	combos_on = 0;
	state_done = 0;
	state_last_ok = false;
	memset(&state_test, 0, sizeof state_test);
	osd_notice_clear();
	osd_fps_reset(&fps);
	states_thumb_clear();
}

/* Cierra lo que es del juego con la emulación ya detenida, en orden y con
 * cada paso al log: el guardado al disco, el audio (puerto, hilo y SPE), la
 * HLE gráfica (espera la tarea en vuelo) y el LLE con sus SPEs, el
 * recompilador, las colas entre hilos y los destinos del RSX. Termina con
 * una línea que dice si quedó algo en vuelo. */
/* ---- Perfil persistente del recompilador (cpu/rec_profile->h) ----------------- */

/* perfil.cfg: `caliente = KB` (0: contar sin zona caliente) y `saltos =
 * si|no` (registrar los sitios de salida de los bloques calientes). */
static uint32_t profile_hot_kb(bool *sites)
{
	uint32_t kb = PROFILE_HOT_KB;
	*sites = true;
	FILE *f = fopen(PROFILE_CFG_PATH, "r");
	if (!f)
		return kb;
	char line[128], word[16];
	while (fgets(line, sizeof line, f)) {
		unsigned v;
		if (sscanf(line, " caliente = %u", &v) == 1)
			kb = v;
		else if (sscanf(line, " saltos = %15s", word) == 1)
			*sites = word[0] != 'n' && word[0] != 'N' && word[0] != '0';
	}
	fclose(f);
	return kb;
}

static void profile_io_thread(void *arg);

/* ---- Muestreo por tiempo (rec_sample.h) ------------------------------------------- */

static void sample_thread(void *arg)
{
	(void)arg;
	while (!rsmp.stop) {
		sysUsleep(SAMPLE_US);
		salsa64_rec_sample_tick(rec_sample, &n64.cpu);
	}
	sysThreadExit(0);
}

static void sample_line(void *ctx, const char *text)
{
	(void)ctx;
	salsa64_log("%s", text);
}

/* Con el archivo `muestreo` y el recompilador activo: la tabla, el hilo y
 * rec.sample (desde entonces cada bloque compilado deja su id al entrar). */
static void sample_open(void)
{
	if (!use_rec || !(salsa64_ps3_rec_sample_wanted() || (perf.on && perf.cfg.samples)))
		return;
	if (!rec_sample && !(rec_sample = memalign(128, sizeof *rec_sample))) {
		salsa64_log("muestreo del recompilador: sin memoria para la tabla (%u KB)", (unsigned)(sizeof *rec_sample >> 10));
		return;
	}
	salsa64_rec_sample_init(rec_sample);
	memset(&rsmp, 0, sizeof rsmp);
	rec.sample = rec_sample;
	if (sysThreadCreate(&rsmp.tid, sample_thread, NULL, SAMPLE_PRIO, 0x4000, THREAD_JOINABLE, (char *)"salsa64_muestreo") !=
	    0) {
		rec.sample = NULL;
		salsa64_log("muestreo del recompilador: no se pudo crear el hilo");
		return;
	}
	rsmp.on = true;
	rsmp.t0 = sysGetSystemTime();
	salsa64_log("muestreo del recompilador: una muestra cada %u µs desde un hilo de prioridad %d (archivo muestreo); "
		    "la entrada de cada bloque deja su id (2 palabras)",
		    SAMPLE_US, SAMPLE_PRIO);
}

/* Al cerrar el juego, con el hilo de emulación ya terminado. */
static void sample_close(void)
{
	u64 rv;
	rsmp.stop = 1;
	sysThreadJoin(rsmp.tid, &rv);
	double secs = (sysGetSystemTime() - rsmp.t0) / 1e6;
	salsa64_log("cierre: muestreo: %llu muestras en %.1f s (%.0f por segundo; pedidas %.0f)",
		    (unsigned long long)rec_sample->ticks, secs, secs > 0 ? rec_sample->ticks / secs : 0.0, 1e6 / SAMPLE_US);
	salsa64_rec_sample_report(&rec, 30, sample_line, NULL);
	rec.sample = NULL;
	rsmp.on = false;
}

/* Al abrir un juego, con el recompilador ya iniciado: carga su perfil (o
 * empieza uno), marca los calientes y lo conecta. */
static void profile_open(const uint8_t *rom)
{
	char base[48];
	memset(&rprof, 0, sizeof rprof);
	memset(&rec_profile_w, 0, sizeof rec_profile_w);
	if (!use_rec)
		return;
	bool perf_profile = perf.on && perf.cfg.profile;
	if (!file_exists(PROFILE_ON_PATH) && !perf_profile) {
		salsa64_log("perfil del recompilador: apagado (sin %s)", PROFILE_ON_PATH);
		return;
	}
	/* La tabla, la primera vez que se enciende (fuera del bucle de
	 * emulación); queda para los juegos siguientes. */
	if (!rec_profile && !(rec_profile = memalign(128, sizeof *rec_profile))) {
		salsa64_log("perfil del recompilador: sin memoria para la tabla (%u bytes): apagado", (unsigned)sizeof *rec_profile);
		return;
	}
	mkdir(PROFILE_DIR, 0777);
	salsa64_save_basename(rom, base, sizeof base);
	if (perf_profile)  /* con el perfilado, junto a sus sesiones (sin zona caliente salvo perfil-activo) */
		snprintf(rprof.path, sizeof rprof.path, "%s/juego.prf", perf.ses.dir);
	else
		snprintf(rprof.path, sizeof rprof.path, "%s/%s.prf", PROFILE_DIR, base);
	struct salsa64_rec_profile_game game;
	salsa64_rec_profile_game_of(&game, rom);
	bool loaded = salsa64_rec_profile_load(rec_profile, rprof.path, &game);
	salsa64_rec_profile_mark_hot(rec_profile, salsa64_rec_hot_capacity(&rec) * 3 / 4);
	bool sites;
	profile_hot_kb(&sites);
	rec.profile_sites = sites;
	rec.profile = rec_profile;
	rprof.on = true;
	rprof.t_saved = sysGetSystemTime();
	{
		sys_sem_attr_t attr;
		memset(&attr, 0, sizeof attr);
		attr.attr_protocol = SYS_SEM_ATTR_PROTOCOL;
		attr.attr_pshared = SYS_SEM_ATTR_PSHARED;
		rprof.thread_ok = sysSemCreate(&rprof.sem, &attr, 0, 1) == 0;
		if (rprof.thread_ok && sysThreadCreate(&rprof.tid, profile_io_thread, NULL, PROFILE_IO_PRIO, PROFILE_IO_STACK,
						       THREAD_JOINABLE, (char *)"salsa64_perfil") != 0) {
			sysSemDestroy(rprof.sem);
			rprof.thread_ok = false;
		}
	}
	salsa64_log("perfil del recompilador: %s %s (%u entradas y %u sitios de salida; %u calientes con %u KB, "
		    "umbral %u); zona caliente de %u KB en dos mitades, sitios de salida %s, se guarda cada %llu s y "
		    "al cerrar (%s)",
		    loaded ? "cargado" : "nuevo", rprof.path, rec_profile->count, rec_profile->nsites,
		    rec_profile->hot_entries, rec_profile->hot_words / 256, rec_profile->hot_threshold,
		    salsa64_rec_hot_capacity(&rec) / 256, sites ? "sí" : "no (saltos = no)",
		    (unsigned long long)(PROFILE_SAVE_US / 1000000),
		    rprof.thread_ok ? "hilo propio de baja prioridad" : "por trozos desde la presentación");
}

/* El hilo que escribe el perfil: duerme en el semáforo y, cuando la
 * presentación le pide una escritura, escribe el archivo entero (la tabla
 * sigue recibiendo cuentas mientras tanto: lee count y nsites una vez). */
static void profile_io_thread(void *arg)
{
	(void)arg;
	while (!rprof.stop) {
		sysSemWait(rprof.sem, 1000000);
		if (rprof.stop)
			break;
		if (!rprof.due)
			continue;
		rprof.due = 0;
		if (salsa64_rec_profile_save(rec_profile, rprof.path)) {
			rprof.saves++;
		} else {
			rprof.save_errors++;
			salsa64_log("perfil del recompilador: FALLO al escribir %s", rprof.path);
		}
	}
	sysThreadExit(0);
}

/* Hilo de presentación, cada vuelta: si pasó el plazo y hubo cuentas
 * nuevas, pide una escritura al hilo del perfil (o, sin él, la sigue por
 * trozos: unas entradas por vuelta, para no detener la presentación). */
static void profile_write_step(uint64_t now)
{
	if (!rprof.on)
		return;
	if (rprof.thread_ok) {
		if (now - rprof.t_saved < PROFILE_SAVE_US || rec_profile->session == rprof.session_saved || rprof.due)
			return;
		rprof.t_saved = now;
		rprof.session_saved = rec_profile->session;
		rprof.due = 1;
		sysSemPost(rprof.sem, 1);  /* si ya está en 1, da error: no importa */
		return;
	}
	if (rec_profile_w.f) {
		int r = salsa64_rec_profile_save_step(rec_profile, &rec_profile_w, PROFILE_STEP_ENTRIES);
		if (r < 0) {
			rprof.save_errors++;
			salsa64_log("perfil del recompilador: FALLO al escribir %s", rprof.path);
		} else if (r > 0) {
			rprof.saves++;
		}
		return;
	}
	if (now - rprof.t_saved < PROFILE_SAVE_US || rec_profile->session == rprof.session_saved)
		return;
	rprof.t_saved = now;
	rprof.session_saved = rec_profile->session;
	if (!salsa64_rec_profile_save_begin(rec_profile, &rec_profile_w, rprof.path)) {
		rprof.save_errors++;
		salsa64_log("perfil del recompilador: FALLO al abrir %s.tmp", rprof.path);
	}
}

/* Al cerrar el juego (el hilo de emulación ya terminó): lo contado al
 * perfil, el archivo de una vez y el perfil suelto. */
static void profile_close(void)
{
	const struct salsa64_rec_stats *st = &rec.stats;
	if (rprof.thread_ok) {
		u64 rv;
		rprof.stop = 1;
		sysSemPost(rprof.sem, 1);
		sysThreadJoin(rprof.tid, &rv);
		sysSemDestroy(rprof.sem);
		rprof.thread_ok = false;
	}
	salsa64_rec_profile_save_abort(&rec_profile_w);
	salsa64_rec_profile_fold(&rec);
	bool ok = salsa64_rec_profile_save(rec_profile, rprof.path);
	salsa64_log("cierre: perfil del recompilador %s %s: %u entradas (%u cargadas, %u sin lugar) y %u sitios de "
		    "salida (%u sin lugar), %llu entradas a bloques contadas (sesión %s), %llu bloques en la zona "
		    "caliente (%llu no cupieron, %llu desalojos de mitad), %u escrituras periódicas (%u fallidas)",
		    ok ? "guardado en" : "NO se pudo guardar en", rprof.path, rec_profile->count, rec_profile->loaded,
		    rec_profile->full, rec_profile->nsites, rec_profile->sites_full, (unsigned long long)rec_profile->session,
		    rec_profile->session >= SALSA64_RPF_LONG_SESSION ? "larga" : "corta",
		    (unsigned long long)st->hot_placed, (unsigned long long)st->hot_full,
		    (unsigned long long)st->hot_evictions, rprof.saves, rprof.save_errors);
	salsa64_rec_profile_detach(&rec);
	rprof.on = false;
}

/* Cuánto se espera al hilo de emulación al cerrar antes de rendirse. */
#define QUIT_WAIT_US 5000000

/* El hilo de emulación no responde (girando en código generado o en C):
 * no hay cómo detenerlo desde fuera. Salir del juego por el XMB con los
 * grupos de SPU vivos colgó la consola entera (2026-10-13): se paran los
 * SPE que se pueden parar sin el hilo de emulación y se termina el
 * proceso con sys_process_exit, con lo que haya quedado en el log. */
static int exit_reboot;  /* forced_exit: 0 al XMB, 1 reinicio por el sistema, 2 directo (rsx_exit_policy) */

static void forced_exit(const char *why, unsigned secs)
{
	/* Mejor esfuerzo antes de cualquier espera de cierre: no dejar actuadores
	 * encendidos aunque la emulación o el RSX ya no respondan. */
	rumble_stop_all();
	salsa64_log("cierre forzado: %s tras %u s (pc 0x%08llx, ciclos %llu, marca %u, bloque %u); se paran los SPE y se sale",
		    why, secs, (unsigned long long)n64.cpu.cur_pc, (unsigned long long)n64.cpu.cycles,
		    n64.cpu.rec_mark, n64.cpu.rec_cur);
	audio_out_shutdown();
	cajanegra_shutdown(3000000);
	states_shutdown(3000000);
	if (jpeg_on_spu)
		salsa64_jpeg_spu_shutdown(&jpeg_spu);
	if (lle_mode != SALSA64_LLE_OFF)
		salsa64_rsp_spu_shutdown(&rsp_spu);
	if (gfx_on_spu)
		salsa64_gfx_spu_shutdown(&gfx_spu);
	if (exit_reboot) {
		/* Pedido con rsx-colgado (rsx_exit_policy): con el RSX colgado,
		 * volver al XMB dejó la consola congelada (Indiana Jones,
		 * 2026-10-29); reiniciar la evita si el sistema todavía responde.
		 * sys_sm_shutdown (379): 0x200 reinicio por el sistema, 0x1200
		 * reinicio directo. Si no vuelve, se sale como siempre. */
		salsa64_log("cierre forzado: SPE detenidos; se reinicia la consola (%s)",
			    exit_reboot == 2 ? "directo, 0x1200" : "por el sistema, 0x200");
		salsa64_log_shutdown();
		lv2syscall4(379, exit_reboot == 2 ? 0x1200 : 0x200, 0, 0, 0);
		sysProcessExit(1);
	}
	salsa64_log("cierre forzado: SPE detenidos; saliendo");
	salsa64_log_shutdown();
	sysProcessExit(1);
}

/* ---- El RSX no avanza --------------------------------------------------------------- */

/* Toda espera del PPE al RSX (barrera o etiqueta, rsxkit_finish, flip, lugar
 * en el búfer de comandos) tiene tope. rsxkit llama a rsx_wait_hook desde
 * el hilo que espera pasado RSX_HOOK_US y luego cada RSX_HOOK_PERIOD_US:
 * desde RSX_DIAG_US deja un diagnóstico en el log (una vez por espera), y
 * atiende al sistema (sysUtilCheckCallback) mientras tanto; con «Salir del
 * juego» pedido, o pasado RSX_EXIT_US (RSX_EXIT_OVERLAY_US con el sistema
 * encima, que puede retener flips), sale con forced_exit. La espera de lugar
 * en el búfer de comandos corre dentro del callback del sistema y no llama
 * al gancho: la ve el latido (watch_thread), que sale por ella. Antes, un
 * RSX que dejó de avanzar (F-Zero X en la PS3, 2026-10-14) dejaba la
 * presentación esperando para siempre, sin flips: ni el botón PS
 * respondía y la consola había que apagarla a la fuerza. */
#define RSX_HOOK_US          500000u
#define RSX_HOOK_PERIOD_US   100000u
#define RSX_DIAG_US         2000000u
#define RSX_EXIT_US        12000000u
#define RSX_EXIT_OVERLAY_US 45000000u
#define RSX_WATCH_EXTRA_US  3000000u  /* el latido, por si el gancho no sale */
static volatile int rsx_exiting;
static bool rsx_said;  /* hilo que espera: ya dio el diagnóstico de esta espera */

static uint64_t rsx_exit_limit(void)
{
	return pausectl_overlay(&pause_ctl) ? RSX_EXIT_OVERLAY_US : RSX_EXIT_US;
}

/* `n` palabras del búfer de comandos desde el offset de E/S `from` (que se
 * alinea a 32 bytes), de a 8 por línea; con «>» la línea de `mark`. */
static void rsx_log_words(const char *tag, uint32_t from, uint32_t n, uint32_t mark)
{
	from &= ~31u;
	uint32_t avail = 0;
	const uint32_t *w = rsxkit_cmd_at(from, &avail);
	if (!w) {
		salsa64_log("RSX fifo %s: 0x%08x fuera de la memoria mapeada", tag, from);
		return;
	}
	if (n > avail)
		n = avail;
	for (uint32_t i = 0; i < n; i += 8) {
		char line[128];
		int len = snprintf(line, sizeof line, "%s%08x:", from + i * 4 == (mark & ~31u) ? ">" : " ", from + i * 4);
		for (uint32_t k = i; k < i + 8 && k < n && len > 0 && len < (int)sizeof line; k++)
			len += snprintf(line + len, sizeof line - (size_t)len, " %08x", w[k]);
		salsa64_log("RSX fifo %s %s", tag, line);
	}
}

/* Todo lo que se sabe de un RSX que no avanza: registros y hasta qué
 * barrera llegó, las excepciones que avisó el sistema, el búfer de comandos
 * alrededor de GET y de PUT (en la PS3 con Indiana Jones GET quedó en 0, el
 * principio del búfer: solo lo de antes de PUT dice qué se mandó al final),
 * el rastro de rsxkit (cada operación con su offset en el búfer) y lo último
 * de los destinos y profundidades. Solo lee. */
static void rsx_diag_full(void)
{
	rsxkit_fifo_info fi;
	rsxkit_fifo_info_get(&fi);
	salsa64_log("RSX búfer de comandos: memoria desde 0x%08x (%u KB), segmento 0x%08x–0x%08x, rsxkit escribe en "
		    "0x%08x; GET 0x%08x PUT 0x%08x REF 0x%08x; barrera mandada %u y escrita %u, rsxkit_finish mandado %u y "
		    "escrito %u",
		    fi.base, fi.size >> 10, fi.begin, fi.end, fi.current, fi.get, fi.put, fi.ref, fi.fence_sent, fi.fence_done,
		    fi.finish_sent, fi.finish_done);
	rsxkit_rsx_error e;
	rsxkit_rsx_errors(&e);
	uint64_t now = sysGetSystemTime();
	if (e.count)
		salsa64_log("RSX: el sistema avisó %u excepciones del RSX: la primera 0x%08x hace %.1f s, la última 0x%08x "
			    "hace %.1f s (con GET 0x%08x PUT 0x%08x)",
			    e.count, e.first, (now - e.first_us) / 1e6, e.last, (now - e.last_us) / 1e6, e.get, e.put);
	else
		salsa64_log("RSX: el sistema no avisó ninguna excepción del RSX (gcmSetGraphicsHandler)");
	if (fi.get != fi.put)
		rsx_log_words("GET", fi.get >= 256 ? fi.get - 256 : 0, 96, fi.get);
	rsx_log_words("PUT", fi.put >= 640 ? fi.put - 640 : 0, 168, fi.put);
	rsxkit_trace_entry tr[RSXKIT_TRACE_LEN];
	unsigned n = rsxkit_trace_get(tr, RSXKIT_TRACE_LEN);
	for (unsigned i = 0; i < n; i++) {
		char line[160];
		rsxkit_trace_format(&tr[i], line, sizeof line);
		salsa64_log("RSX rastro %u/%u, 0x%08x: %s", i + 1, n, tr[i].fifo, line);
	}
	salsa64_rdp_diag_log(salsa64_log);
}

/* Dónde está el PPE, qué espera del RSX y por dónde va el RSX; `why`,
 * entre paréntesis después de «RSX» (o NULL). */
static void rsx_diag(const char *why, const rsxkit_wait_info *wi)
{
	char what[112], tr[96] = "";
	if (wi->kind == RSXKIT_WAIT_LABEL)
		snprintf(what, sizeof what, "«%s» (etiqueta %u: espera %u y va en %u)", wi->what, wi->label, wi->expected,
			 wi->current);
	else if (wi->kind == RSXKIT_WAIT_FLIP)
		snprintf(what, sizeof what, "el flip (estado %u)", wi->current);
	else
		snprintf(what, sizeof what, "lugar en el búfer de comandos (%u palabras; dentro del callback del sistema)",
			 wi->expected);
	const char *stage;
	uint32_t task = salsa64_rdp_translating(&stage);
	if (task)
		snprintf(tr, sizeof tr, ", traduciendo la tarea %u («%s»)", task, stage);
	salsa64_log("RSX%s%s%s: el PPE espera hace %.1f s %s; GET 0x%08x PUT 0x%08x, GET al empezar 0x%08x (%s); "
		    "presentación en «%s»%s; última tarea traducida %u, %u terminadas por la HLE; campo %u, pausa 0x%x, "
		    "sistema %s",
		    why ? " (" : "", why ? why : "", why ? ")" : "", wi->elapsed_us / 1e6, what, wi->get, wi->put, wi->get0,
		    wi->get != wi->get0 ? "el RSX avanzó" : wi->get == wi->put ? "el RSX está al día" : "el RSX no avanzó",
		    beat.pres_at, tr, salsa64_rdp_translated(), salsa64_rdp_tasks_done(), n64.bus.vi.fields, pause_req,
		    pausectl_overlay(&pause_ctl) ? "encima" : "sin dibujar");
	/* Lo demás, una vez por corrida y solo si el RSX no avanzó. */
	static volatile int full_said;
	if (wi->get == wi->get0 && wi->get != wi->put && __sync_bool_compare_and_swap(&full_said, 0, 1))
		rsx_diag_full();
}

/* Lo que queda en el disco de un RSX colgado, en SALSA64_DATA_DIR/
 * rsx-colgado-<campo>/: el búfer de comandos (fifo.bin, el primer MB de la
 * memoria mapeada que lo contiene, desde el offset de E/S que dice el log),
 * la tarea que se traducía (gfxdump.bin, para salsa64-run --raster-dump) y,
 * con la caja negra encendida, su anillo sin comprimir (caja.s4bb). Todo
 * pasa por sysLv2FsFsync: puede que la consola se apague a la fuerza. */
static bool write_synced(const char *path, const void *data, uint32_t len)
{
	s32 fd = -1;
	if (sysLv2FsOpen(path, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &fd, 0666, NULL, 0) != 0)
		return false;
	u64 done = 0;
	bool ok = sysLv2FsWrite(fd, data, len, &done) == 0 && done == len;
	ok = sysLv2FsFsync(fd) == 0 && ok;
	ok = sysLv2FsClose(fd) == 0 && ok;
	return ok;
}

static void rsx_stuck_files(void)
{
	char dir[96], path[160], err[160];
	snprintf(dir, sizeof dir, SALSA64_DATA_DIR "/rsx-colgado-%u", n64.bus.vi.fields);
	sysLv2FsMkdir(dir, 0777);
	uint64_t t0 = sysGetSystemTime();
	rsxkit_fifo_info fi;
	rsxkit_fifo_info_get(&fi);
	uint32_t avail = 0;
	const uint32_t *w = rsxkit_cmd_at(fi.base, &avail);
	uint32_t bytes = avail * 4 < (1u << 20) ? avail * 4 : (1u << 20);
	snprintf(path, sizeof path, "%s/fifo.bin", dir);
	bool fifo_ok = w && write_synced(path, w, bytes);
	snprintf(path, sizeof path, "%s/gfxdump.bin", dir);
	uint32_t task = 0;
	bool dump_ok = salsa64_rdp_dump_current(path, &task);
	if (dump_ok) {
		s32 fd = -1;
		if (sysLv2FsOpen(path, SYS_O_WRONLY, &fd, 0, NULL, 0) == 0) {
			sysLv2FsFsync(fd);
			sysLv2FsClose(fd);
		}
	}
	snprintf(path, sizeof path, "%s/" CAJANEGRA_FILE, dir);
	bool box_ok = cajanegra_write_now(path, 3000000, err, sizeof err);
	salsa64_log("RSX colgado: en %s: búfer de comandos %s (fifo.bin, %u KB desde 0x%08x), tarea %u %s (gfxdump.bin), "
		    "caja negra %s (%s); %.1f s",
		    dir, fifo_ok ? "escrito" : "NO escrito", bytes >> 10, fi.base, task, dump_ok ? "escrita" : "NO escrita",
		    box_ok ? "escrita" : "no escrita", err, (sysGetSystemTime() - t0) / 1e6);
}

/* Qué hacer al final de un cierre forzado por el RSX: el archivo
 * SALSA64_DATA_DIR/rsx-colgado dice «reinicio» (reiniciar la consola por el
 * sistema) o «reinicio-directo»; sin archivo, o con otra cosa, volver al
 * XMB como siempre. Experimental: para probar con colgar-rsx qué deja la
 * consola usable (docs/rsx-notes.md). */
static int rsx_exit_policy(void)
{
	char word[24] = "";
	FILE *f = fopen(SALSA64_DATA_DIR "/rsx-colgado", "r");
	if (!f)
		return 0;
	if (fscanf(f, "%23s", word) != 1)
		word[0] = '\0';
	fclose(f);
	int r = !strcmp(word, "reinicio") ? 1 : !strcmp(word, "reinicio-directo") ? 2 : 0;
	salsa64_log("RSX: rsx-colgado dice «%s»: %s", word, r ? "se reinicia la consola" : "se vuelve al XMB");
	return r;
}

/* Salida con el RSX sin avanzar: lo que se pueda guardar sin él (la
 * partida, la sesión del perfilado, y lo que hace falta para entender el
 * cuelgue: rsx_stuck_files), se abandona el RSX y forced_exit. Desde el
 * hilo que espera o desde el latido; solo uno sale. */
static void rsx_stuck_exit(const char *why, const rsxkit_wait_info *wi)
{
	if (!__sync_bool_compare_and_swap(&rsx_exiting, 0, 1))
		for (;;)
			sysUsleep(100000);  /* el otro hilo termina el proceso */
	rsx_diag(why, wi);
	if (wi->get == wi->get0 && wi->get != wi->put)
		rsx_stuck_files();
	if (emu_live) {
		savefile_write_staged(&save_file, ~0ull);  /* lo diferido que no llegó al disco */
		mempakfile_write_staged(&mempak_file, ~0ull);
		for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++)
			transferpakfile_write_staged(&transferpak_files[i], ~0ull);
		if (emu_paused || emu_done) {
			savefile_flush(&save_file);
			mempakfile_flush(&mempak_file);
		}
		else
			salsa64_log("RSX: la emulación sigue: solo se escribió el guardado diferido");
	}
	if (perf.on) {
		char end[160];
		snprintf(end, sizeof end, "duracion_ms=%u\ncampos=%u\nemulados=%u\nlimpio=no\nsalida=rsx\n", perf_ms(),
			 n64.bus.vi.fields, emu_stats.fields);
		perf_app_close(end);
	}
	/* Que nadie vuelva a esperarlo ni a mandarle órdenes, y que no le quede
	 * nada pendiente (docs/rsx-notes.md: con el RSX colgado, volver al XMB
	 * congeló la consola). La memoria del RSX no se suelta ni se esperan
	 * flips: forced_exit no pasa por rsxkit. */
	uint32_t get = 0, put = 0;
	bool moved = rsxkit_abandon(&get, &put);
	salsa64_log("RSX: abandonado (rsxkit no le manda nada más ni lo espera); %s (GET 0x%08x, PUT 0x%08x)",
		    moved ? "PUT = GET: nada pendiente" : "PUT sin tocar", get, put);
	exit_reboot = rsx_exit_policy();
	forced_exit("el RSX no avanzó", (unsigned)(wi->elapsed_us / 1000000u));
}

static bool rsx_wait_hook(void *ctx, const rsxkit_wait_info *wi)
{
	(void)ctx;
	if (wi->done) {
		if (rsx_said)
			salsa64_log("RSX: la espera de «%s» terminó tras %.1f s", wi->what, wi->elapsed_us / 1e6);
		rsx_said = false;
		return true;
	}
	if (wi->calls == 1)
		rsx_said = false;
	if (!rsx_said && wi->elapsed_us >= RSX_DIAG_US) {
		rsx_diag(NULL, wi);
		rsx_said = true;
	}
	rsx_in_wait = 1;
	sysUtilCheckCallback();
	rsx_in_wait = 0;
	if (app_quit && wi->elapsed_us >= RSX_DIAG_US)
		rsx_stuck_exit("«Salir del juego» del sistema", wi);
	if (wi->elapsed_us >= rsx_exit_limit())
		rsx_stuck_exit("tope de la espera: se sale", wi);
	return true;
}

/* Prueba del seguro (HANG_TEST_PATH): «etiqueta», el PPE espera una
 * barrera que nunca se pone (el RSX está al día); «rsx», el RSX se queda
 * girando (rsxkit_debug_stall) y el PPE espera que termine; «fifo», lo
 * mismo pero el PPE sigue mandando órdenes hasta que no caben y queda
 * dentro del callback del sistema (lo ve el latido). */
static void hang_test_load(void)
{
	char word[16] = "";
	hang_test.mode = HANG_NONE;
	FILE *f = fopen(HANG_TEST_PATH, "r");
	if (!f)
		return;
	if (fscanf(f, "%15s %u", word, &hang_test.field) == 2)
		hang_test.mode = !strcmp(word, "etiqueta") ? HANG_LABEL
			       : !strcmp(word, "rsx")	   ? HANG_RSX
			       : !strcmp(word, "fifo")	   ? HANG_FIFO
							   : HANG_NONE;
	fclose(f);
	if (hang_test.mode)
		salsa64_log("prueba del seguro del RSX: «%s» en el campo %u (%s)", word, hang_test.field, HANG_TEST_PATH);
}

static void rsx_hang_test(void)
{
	int mode = hang_test.mode;
	hang_test.mode = HANG_NONE;
	salsa64_log("prueba del seguro del RSX: se cuelga ahora (campo %u)", n64.bus.vi.fields);
	beat.pres_at = "prueba del seguro";
	if (mode == HANG_LABEL) {
		rsxkit_fence_wait(rsxkit_fence() + 1000000u);
		return;
	}
	rsxkit_debug_stall();
	if (mode == HANG_RSX) {
		rsxkit_finish();
		return;
	}
	for (uint32_t i = 0; i < 1000000u; i++)
		rsxkit_fence();
}

/* Desglose de lo que cada subsistema tiene reservado al cerrar un juego y
 * diferencia frente al cierre anterior. Las líneas «memoria …» dicen cuánto
 * queda; esta dice dónde está y qué creció entre dos aperturas (heap de
 * newlib en uso/libre/bloques/tope y memoria de usuario del sistema). Lo que
 * queda reservado «para siempre» (RDRAM, fotos, estados, el muestreo y el
 * perfil) debe salir igual en todas; lo que cambia de un cierre al
 * siguiente es una fuga o fragmentación. */
struct mem_close_in {
	uint32_t blackbox, interp;  /* reservado justo antes de soltarse en el cierre */
};

static void mem_close_report(const struct mem_close_in *in)
{
	static struct {
		bool have;
		int arena, used, free_, blocks;
		long top;
		u32 user;
		unsigned games;
	} prev;
	static struct {
		u32 total;
		u32 available;
	} um __attribute__((aligned(16)));
	struct mallinfo mi = mallinfo();
	lv2syscall1(352, (u64)(uintptr_t)&um);
	bool um_ok = (s32)p1 == 0;
	long top = (long)(uintptr_t)sbrk(0);
	unsigned kb_rdram = rdram ? SALSA64_RDRAM_MAX >> 10 : 0;
	unsigned kb_snap = 0;
	for (int i = 0; i < SNAP_SLOTS; i++)
		kb_snap += snap_buf[i] ? SNAP_BYTES >> 10 : 0;
	salsa64_log("memoria, reservado por subsistema (KB): RDRAM %u, instantáneas %u, estados %u, caja negra %u (soltada "
		    "al cerrar), tablas de la interpolación %u (soltadas al cerrar), muestreo %u, perfil %u, ROM en páginas %u",
		    kb_rdram, kb_snap, states_reserved() >> 10, in->blackbox >> 10, in->interp >> 10,
		    rec_sample ? (unsigned)(sizeof *rec_sample >> 10) : 0, rec_profile ? (unsigned)(sizeof *rec_profile >> 10) : 0,
		    rom_pages ? 1u : 0u);
	if (prev.have)
		salsa64_log("memoria, cambio frente al cierre anterior (juego %u tras el primero): heap en uso %+d KB (%+d bytes), libres %+d KB "
			    "en %+d bloques, arena %+d KB, tope %+ld KB, usuario libre %+d KB%s",
			    ++prev.games, (mi.uordblks - prev.used) >> 10, mi.uordblks - prev.used, (mi.fordblks - prev.free_) >> 10,
			    mi.ordblks - prev.blocks, (mi.arena - prev.arena) >> 10, (top - prev.top) >> 10,
			    um_ok ? ((int)um.available - (int)prev.user) >> 10 : 0,
			    mi.uordblks - prev.used > 1024 ? " (crecimiento de heap superior a 1 KB)" : "");
	prev.have = true;
	prev.arena = mi.arena, prev.used = mi.uordblks, prev.free_ = mi.fordblks, prev.blocks = mi.ordblks;
	prev.top = top;
	prev.user = um_ok ? um.available : 0;
}

static void game_close(void)
{
	ra_game_close();  /* espera la huella de la ROM, que se lee de su búfer */
	struct mem_close_in mem_in = { cajanegra_reserved(), (uint32_t)salsa64_rdp_interp_bytes() };
	/* Never leave a motor latched across game changes or a return to the menu. */
	memset(n64.bus.pif.rumble, 0, sizeof n64.bus.pif.rumble);
	rumble_publish();
	rumble_stop_all();
	cajanegra_shutdown(10000000);
	states_shutdown(10000000);  /* un estado a medio escribir se termina antes de soltar nada */
	salsa64_filters_release();
	salsa64_log("cierre: emulación detenida tras %u campos (%u emulados, %u descartados por la cola, %u sin ranura)",
		    n64.bus.vi.fields, emu_stats.fields, emu_stats.dropped, emu_stats.no_slot);
	if (n64.bus.gamefix_on)
		salsa64_log("cierre: arreglos de juego aplicados %u veces", n64.bus.gamefix_applied);
	if (n64.cpu.dcache_sweep_cycles)
		salsa64_log("cierre: %u barridos de la caché de datos", n64.cpu.dcache_sweeps);
	savefile_close(&save_file);
	mempakfile_close(&mempak_file);
	for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++)
		transferpakfile_close(&transferpak_files[i]);
	salsa64_log("cierre: guardado escrito y desconectado");
	/* audio_out: cierre (con el hilo de emulación ya terminado). */
	audio_out_shutdown();
	salsa64_log("cierre: audio cerrado (%u tareas de audio)", n64.bus.rsp.stats.audio);
	salsa64_rsp_set_gfx(&n64.bus, NULL);  /* espera la tarea en vuelo */
	salsa64_rsp_set_dpc(&n64.bus, NULL);
	salsa64_log("cierre: HLE gráfica suelta (%u tareas gráficas)", n64.bus.rsp.stats.gfx);
	if (jpeg_on_spu) {
		salsa64_rsp_set_jpeg(&n64.bus, NULL);
		salsa64_log("cierre: SPE de la tarea JPEG detenido (%u tareas, %u macrobloques, %u en el PPE; video %u + %u + %u "
			    "tareas, %u en el PPE; salida %d)", jpeg_spu.tasks, jpeg_spu.macroblocks, jpeg_spu.on_ppe,
			    jpeg_spu.v_tasks[0], jpeg_spu.v_tasks[1], jpeg_spu.v_tasks[2],
			    jpeg_spu.v_on_ppe[0] + jpeg_spu.v_on_ppe[1] + jpeg_spu.v_on_ppe[2],
			    salsa64_jpeg_spu_shutdown(&jpeg_spu));
		jpeg_on_spu = 0;
	}
	if (lle_mode != SALSA64_LLE_OFF) {
		salsa64_rsp_set_lle(&n64.bus, NULL, SALSA64_LLE_OFF);
		salsa64_log("rsp lle: %u tareas, %u trabajos, %llu instrucciones (la tarea más larga %u), %u fallidos, "
			    "%u tramos del RDP (%u perdidos); SPE detenido (salida %d)", rsp_spu.tasks, rsp_spu.jobs,
			    (unsigned long long)rsp_spu.instructions, rsp_spu.task_max, rsp_spu.failed,
			    rsp_spu.dpc_entries, rsp_spu.dpc_lost, salsa64_rsp_spu_shutdown(&rsp_spu));
		lle_mode = SALSA64_LLE_OFF;
	}
	if (gfx_on_spu) {
		salsa64_gfx_spu_shutdown(&gfx_spu);
		salsa64_log("cierre: SPE de la HLE gráfica detenido (%u tareas)", gfx_spu.tasks);
	}
	perf_app_close_rec(use_rec ? &rec : NULL, rsmp.on ? rec_sample : NULL);  /* antes de que el muestreo agote su tabla */
	if (rsmp.on)
		sample_close();
	if (rprof.on)
		profile_close();
	rec.trace = NULL;
	if (use_rec) {
		salsa64_rec_detach(&rec);
		salsa64_log("cierre: recompilador suelto (%u KB de caché usados; el próximo juego empieza vacío)",
			    salsa64_rec_used(&rec) / 256);
		use_rec = false;
	}
	/* Lo que quedó en las colas (sin presentar) y la memoria del RSX. */
	drain(sysGetSystemTime(), 0);
	uint32_t untranslated = salsa64_rdp_reset();
	salsa64_rdp_interp_release();  /* sus tablas (2,4 MB) no se quedan fijando el tope del heap */
	int snaps = 0;
	for (int i = 0; i < SNAP_SLOTS; i++)
		snaps += snap_state[i] != SNAP_FREE;
	uint32_t queued = field_head - field_tail;
	bool spes = gfx_spu.worker || rsp_spu.worker || jpeg_spu.worker;
	bool hooks = n64.bus.rsp.gfx || n64.bus.rsp.audio || n64.bus.rsp.lle || n64.bus.rsp.jpeg_be || n64.bus.rsp.busy;
	bool clean = emu_done && !queued && !snaps && !untranslated && !spes && !hooks;
	salsa64_log("cierre: %s (hilo de emulación %s, %u campos en cola, %d ranuras del VI ocupadas, %u tareas "
		    "sin traducir, destinos del RSX liberados, SPEs de la HLE %s, RSP %s)",
		    clean ? "estado limpio" : "ATENCIÓN, quedó algo en vuelo", emu_done ? "terminado" : "VIVO", queued,
		    snaps, untranslated, spes ? "VIVOS" : "detenidos", hooks ? "CON BACKENDS" : "sin backends");
	heap_report("al cerrar el juego");
	mem_close_report(&mem_in);
	if (perf.on) {
		char end[384];
		snprintf(end, sizeof end,
			 "duracion_ms=%u\ncampos=%u\nemulados=%u\ndescartados=%u\nciclos=%llu\nciclos_espera=%llu\n"
			 "rec_bloques=%llu\nrec_vaciados=%llu\nrec_desalojos=%llu\nrec_jr=%llu\nlimpio=%s\nsalida=%s\n",
			 perf_ms(), n64.bus.vi.fields, emu_stats.fields, emu_stats.dropped,
			 (unsigned long long)n64.cpu.cycles, (unsigned long long)n64.cpu.idle_cycles,
			 (unsigned long long)rec.stats.compiled, (unsigned long long)rec.stats.flushes,
			 (unsigned long long)rec.stats.evictions, (unsigned long long)n64.cpu.rec_jr_count, clean ? "si" : "no",
			 app_quit ? "xmb" : in_menu ? "menu" : "fin");
		perf_app_close(end);
	}
}

/* El ícono del cartucho de la lista (assets/menu/cartucho.png, 256 × 128:
 * el plástico a la izquierda y la etiqueta en blanco a la derecha) en la
 * ranura 1 del 2D; sin él, el menú dibuja el ícono plano. */
static bool ui_cart_icon;

static void ui_cart_icon_load(void)
{
	uint8_t *rgba = malloc(COVER_RGBA_BYTES);
	unsigned w = 0, h = 0;
	enum cover_result r = rgba ? cover_decode_png(cart_icon_bin, cart_icon_bin_size, rgba, &w, &h) : COVER_TOO_BIG;
	int rc = r == COVER_OK ? rsxkit_ui_image_set(MENU_CART_SLOT, rgba, w, h) : RSXKIT_EARG;
	free(rgba);
	ui_cart_icon = r == COVER_OK && rc == RSXKIT_OK && w == 2 * h;
	if (!ui_cart_icon)
		salsa64_log("menú: el ícono del cartucho no cargó (%s, %d, %ux%u): queda el plano", cover_result_name(r), rc, w, h);
}

/* El cartucho 3D de la fila elegida (cartucho3d.h): la malla y la textura del
 * plástico van embebidas; si algo falla, el menú usa el sprite. */
static void ui_cart3d_load(void)
{
	uint8_t *rgba = malloc(COVER_RGBA_BYTES), *lab = malloc(COVER_RGBA_BYTES);
	unsigned w = 0, h = 0, lw = 0, lh = 0;
	enum cover_result r = rgba && lab ? cover_decode_png(cart_plastic_bin, cart_plastic_bin_size, rgba, &w, &h)
					 : COVER_TOO_BIG;
	if (r == COVER_OK)
		r = cover_decode_png(cart_label_bin, cart_label_bin_size, lab, &lw, &lh);
	bool ok = r == COVER_OK && cartucho3d_init(cart_mesh_bin, cart_mesh_bin_size, rgba, w, h, lab, lw, lh);
	free(rgba);
	free(lab);
	if (!ok)
		salsa64_log("menú: el cartucho 3D no cargó (%s): queda el sprite", cover_result_name(r));
}

/* Los íconos de botones (assets/menu/prompts.png, Kenney CC0; prompts.h) en la
 * ranura PROMPTS_SLOT del 2D; sin ellos, el texto lleva los símbolos de siempre. */
static void ui_prompts_load(void)
{
	uint8_t *rgba = malloc(COVER_RGBA_BYTES);
	unsigned w = 0, h = 0;
	enum cover_result r = rgba ? cover_decode_png(prompts_bin, prompts_bin_size, rgba, &w, &h) : COVER_TOO_BIG;
	int rc = r == COVER_OK ? rsxkit_ui_image_set(PROMPTS_SLOT, rgba, w, h) : RSXKIT_EARG;
	free(rgba);
	bool ok = r == COVER_OK && rc == RSXKIT_OK && w == PROMPT_ATLAS_W && h == PROMPT_ATLAS_H;
	prompts_enable(ok);
	if (!ok)
		salsa64_log("menú: los íconos de botones no cargaron (%s, %d, %ux%u): quedan los símbolos de texto",
			    cover_result_name(r), rc, w, h);
}

/* rsxkit_ui: el menú y lo que se dibuja sobre el juego (aviso, contador);
 * se prepara una vez (6 MB mapeados, para siempre). */
static int ui_init_once(void)
{
	static int rc = 1;
	if (rc == 1) {
		/* Las carátulas llenan el sticker estiradas; con el archivo caratula-recorte, recortadas. */
		thumb_set_crop(file_exists(SALSA64_DATA_DIR "/caratula-recorte"));
		if (thumb_crop())
			salsa64_log("carátulas: se recortan al sticker (caratula-recorte)");
		rc = rsxkit_ui_init();
		if (rc != RSXKIT_OK)
			salsa64_log("2D del RSX no disponible (%d): sin avisos en pantalla", rc);
		else
		{
			ui_cart_icon_load();
			ui_cart3d_load();
			thumbs_init(COVERS_DIR);
			ui_prompts_load();
		}
	}
	/* La salida puede cambiar sin reiniciar («Aplicar ahora»). */
	const rsxkit_screen *sc = rsxkit_screen_info();
	ui_lh = 720.0f;
	ui_lw = sc->aspect > 1.5f ? 1280.0f : 960.0f;
	ui_ok = rc == RSXKIT_OK;
	return rc;
}

/* ---- La fuente del texto -------------------------------------------------
 * La del sistema (rsxkit/sysfont.h) para todo el texto si carga; si no, o
 * con el archivo fuente-propia, la 8x13 de rsxkit. Solo con la del sistema
 * se ofrecen el japonés, el coreano y el chino (i18n_set_cjk). Se abre una
 * vez, con el 2D listo y antes de cualquier ROM (se queda para siempre), y
 * se cambia de juego de fuentes con el idioma (los ideogramas del chino
 * salen de otra fuente que los del japonés). */
#define FONT_OWN_PATH SALSA64_DATA_DIR "/fuente-propia"
static int font_state;        /* 0 sin probar, 1 la del sistema, −1 la 8x13 */
static int font_script = -1;  /* rsxkit_sysfont_script abierto */

/* El registro de la apertura (rsxkit_sysfont_info.trace), una línea por
 * juego de fuentes intentado (van separados por «|»). */
static void font_trace_log(const char *t)
{
	while (*t) {
		const char *bar = strchr(t, '|');
		size_t n = bar ? (size_t)(bar - t) : strlen(t);
		salsa64_log("fuente: apertura: %.*s", (int)(n > 900 ? 900 : n), t);
		t += n;
		while (*t == '|' || *t == ' ')
			t++;
	}
}

static void font_log(void)
{
	if (font_state != 1) {
		salsa64_log("fuente: la propia 8x13");
		return;
	}
	rsxkit_sysfont_info fi;
	rsxkit_sysfont_info_get(&fi);
	font_trace_log(fi.trace);
	salsa64_log("fuente: la del sistema (juego 0x%x, modo %d, %.0f px, renglón %.1f y línea base %.1f según libfont, «H» en las "
		    "filas %d–%d, glifos faltantes %s), libfont %u KB ahora y %u KB de máximo, caché de archivos %u KB, "
		    "japonés/coreano/chino %s",
		    (unsigned)fi.type, fi.mode, (double)RSXKIT_SYSFONT_PX, (double)fi.line_height, (double)fi.baseline_y, fi.cal_top,
		    fi.cal_bottom, fi.id_check ? "por fontGetFontIdCode" : "por el error del dibujo",
		    (unsigned)(fi.lib_bytes >> 10), (unsigned)(fi.lib_peak >> 10), (unsigned)(fi.cache_bytes >> 10),
		    i18n_cjk() ? "sí" : "no");
}

/* El estado de la caché de glifos, para el log (al abrir un juego y al salir). */
static void font_stats_log(const char *when)
{
	if (!ui_ok)
		return;
	rsxkit_ui_font_stats st;
	rsxkit_ui_font_stats_get(&st);
	if (font_state != 1) {
		salsa64_log("fuente %s: la propia 8x13", when);
		return;
	}
	rsxkit_sysfont_info fi;
	rsxkit_sysfont_info_get(&fi);
	salsa64_log("fuente %s: caché de glifos %u/%u casillas, %u conocidos, %u aciertos, %u rasterizados, %u "
		    "reemplazos, %u diferidos, %u con la 8x13, %u vaciados; libfont %u KB (máx. %u KB)",
		    when, st.resident, st.slots, st.known, st.hits, st.renders, st.evictions, st.deferred, st.fallback,
		    st.resets, (unsigned)(fi.lib_bytes >> 10), (unsigned)(fi.lib_peak >> 10));
}

/* Abre la fuente del sistema (módulos, libfont, FreeType: ~0,9 s en la PS3)
 * sin tocar el 2D del RSX: puede correr en el hilo de la carga previa
 * (preload_thread) mientras la secuencia de arranque anima la pantalla.
 * font_init_once la pone en el 2D, ya en el hilo principal. */
static int font_attached;

static void font_open_raw(void)
{
	if (font_state)
		return;
	font_state = -1;
	i18n_set_cjk(0);
	if (!ui_ok)
		return;
	if (file_exists(FONT_OWN_PATH)) {
		salsa64_log("fuente: %s presente, uso la 8x13", FONT_OWN_PATH);
		return;
	}
	int rc = rsxkit_sysfont_open(RSXKIT_SYSFONT_JAPANESE, 0);
	if (rc != RSXKIT_OK) {
		rsxkit_sysfont_info fi;
		rsxkit_sysfont_info_get(&fi);
		font_trace_log(fi.trace);
		salsa64_log("fuente: la del sistema no abre (%d, paso %d, código 0x%08x): uso la 8x13", rc, fi.step,
			    (unsigned)fi.error);
		return;
	}
	font_state = 1;
	font_script = RSXKIT_SYSFONT_JAPANESE;
	/* Hiragana, hangul e ideogramas del chino tradicional y del simplificado. */
	int cjk = rsxkit_sysfont_has(0x3042) && rsxkit_sysfont_has(0xD55C) && rsxkit_sysfont_has(0x9AD4) &&
		  rsxkit_sysfont_has(0x7B80);
	i18n_set_cjk(cjk);
}

static void font_init_once(void)
{
	font_open_raw();
	if (font_attached)
		return;
	font_attached = 1;
	if (font_state == 1)
		rsxkit_ui_font(rsxkit_sysfont_source());
	font_log();
}

/* El juego de fuentes para el idioma en uso (`lang` ya resuelto). */
static void font_select(int lang)
{
	if (font_state != 1)
		return;
	int script = lang == I18N_ZH_HANT ? RSXKIT_SYSFONT_TCHINESE
		     : lang == I18N_ZH_HANS ? RSXKIT_SYSFONT_SCHINESE
		     : lang == I18N_JA || lang == I18N_KO ? RSXKIT_SYSFONT_JAPANESE : RSXKIT_SYSFONT_LATIN;
	if (script == font_script)
		return;
	rsxkit_ui_font(NULL);  /* vacía la caché */
	int rc = rsxkit_sysfont_open((rsxkit_sysfont_script)script, 0);
	if (rc != RSXKIT_OK && script != RSXKIT_SYSFONT_JAPANESE) {
		rsxkit_sysfont_info fi;
		rsxkit_sysfont_info_get(&fi);
		font_trace_log(fi.trace);
		salsa64_log("fuente: el juego para la escritura %d no abre (paso %d, código 0x%08x): uso el japonés", script,
			    fi.step, (unsigned)fi.error);
		script = RSXKIT_SYSFONT_JAPANESE;
		rc = rsxkit_sysfont_open(RSXKIT_SYSFONT_JAPANESE, 0);
	}
	if (rc != RSXKIT_OK) {
		salsa64_log("fuente: la del sistema dejó de abrir (%d): uso la 8x13", rc);
		rsxkit_sysfont_close();
		font_state = -1;
		font_script = -1;
		i18n_set_cjk(0);
		font_log();
		return;
	}
	font_script = script;
	rsxkit_ui_font(rsxkit_sysfont_source());
	font_log();
}

/* Arreglos por juego en el código de la RDRAM (core/gamefix.c), elegidos al
 * poner la ROM; los que tienen interruptor se apagan con su archivo en el
 * directorio de datos (p. ej. dk64-musica-vieja). */
static void gamefix_setup(void)
{
	for (unsigned k = 0; salsa64_gamefix_name(&n64.bus, k); k++) {
		const char *off = salsa64_gamefix_switch(&n64.bus, k);
		char path[128];
		if (off) {
			snprintf(path, sizeof path, SALSA64_DATA_DIR "/%s", off);
			if (file_exists(path)) {
				salsa64_gamefix_disable(&n64.bus, k);
				salsa64_log("arreglo de juego: %s: apagado (%s)", salsa64_gamefix_name(&n64.bus, k), off);
				continue;
			}
		}
		salsa64_log("arreglo de juego: %s (se aplica cuando el juego carga ese código%s%s%s)",
			    salsa64_gamefix_name(&n64.bus, k), off ? "; " : "", off ? off : "", off ? " lo apaga" : "");
	}
}

/* Ritmo del RCP (rsp.c, gfx_model; docs/ritmo-rcp.md): una tarea gráfica no
 * termina antes de lo que tardaría el RSP real en ella, estimado con lo que
 * hizo la HLE. Sin eso, un juego cuya lógica avanza por cuadro dibujado y que
 * no espera el retrazo corre más rápido que en la consola (Shadows of the
 * Empire: 150 tareas por segundo contra 85 en ares). Encendido por omisión solo
 * en los juegos de RITMO_RCP_JUEGOS (medidos); rsp-ritmo lo enciende en todos,
 * rsp-ritmo-<código> (los 4 caracteres del código del cartucho, p. ej.
 * rsp-ritmo-NSWE) en uno, y sin-rsp-ritmo lo apaga. El archivo puede traer un
 * porcentaje del modelo (100 por omisión; 150 = la consola un 50 % más lenta). */
static const char *const RITMO_RCP_JUEGOS[] = {
	"NSW",  /* Star Wars: Shadows of the Empire (todas las regiones) */
};

static bool ritmo_pct(const char *path, unsigned *pct)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	unsigned v = 0;
	*pct = fscanf(f, "%u", &v) == 1 && v >= 10 && v <= 400 ? v : 100;
	fclose(f);
	return true;
}

static void ritmo_rcp_setup(const uint8_t *rom)
{
	char code[5];
	for (int i = 0; i < 4; i++) {
		char c = (char)rom[0x3B + i];
		code[i] = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ? c : '_';
	}
	code[4] = '\0';
	unsigned pct = 0;
	const char *why = "apagado por omisión";
	for (size_t i = 0; i < sizeof RITMO_RCP_JUEGOS / sizeof RITMO_RCP_JUEGOS[0]; i++)
		if (!strncmp(code, RITMO_RCP_JUEGOS[i], strlen(RITMO_RCP_JUEGOS[i])))
			pct = 100, why = "encendido por omisión en este juego";
	char path[128];
	snprintf(path, sizeof path, SALSA64_DATA_DIR "/rsp-ritmo-%s", code);
	if (ritmo_pct(SALSA64_DATA_DIR "/rsp-ritmo", &pct))
		why = "rsp-ritmo";
	if (ritmo_pct(path, &pct))
		why = path + sizeof SALSA64_DATA_DIR;
	if (file_exists(SALSA64_DATA_DIR "/sin-rsp-ritmo"))
		pct = 0, why = "sin-rsp-ritmo";
	n64.bus.rsp.gfx_model = pct;
	if (pct)
		salsa64_log("RSP: ritmo del RCP al %u %% (%s; código %s)", pct, why, code);
	else
		salsa64_log("RSP: ritmo del RCP apagado (%s; código %s: rsp-ritmo-%s lo enciende)", why, code, code);
}

/* Una sesión de juego con la ROM ya en memoria (cualquier orden de bytes):
 * la arranca, la emula en dos hilos hasta START + SELECT, «Salir del
 * juego» o el fin de una prueba, y lo cierra todo (game_close). Llamar
 * después de game_reset y de poner las banderas de las ROMs de prueba. Si
 * no arranca, deja el motivo en `err`. */
static enum game_end run_game_once(uint8_t *rom, uint32_t rom_size, const char *source, char *err, size_t err_size)
{
	int rc;
	err[0] = '\0';
	if (rom != rom_head)  /* por fragmentos, la cabecera y cada página ya se normalizaron al leerlas */
		salsa64_rom_normalize(rom, rom_size);
	memset(rdram, 0, SALSA64_RDRAM_MAX);  /* cada juego arranca igual, venga después del que venga */
	salsa64_n64_init(&n64, rdram, SALSA64_RDRAM_MAX);
	n64.bus.vi.bob_full_frame = file_exists(SALSA64_DATA_DIR "/vi-campos");
	n64.bus.vi.bob_scaled_frame = file_exists(SALSA64_DATA_DIR "/vi-escalado-campos");
	salsa64_log("VI: cuadros entrelazados completos %s (vi-campos); escalados %s (vi-escalado-campos)",
		    n64.bus.vi.bob_full_frame ? "por campos" : "tejidos",
		    n64.bus.vi.bob_full_frame || n64.bus.vi.bob_scaled_frame ? "por campos" : "tejidos");
	bool si_immediate = file_exists(SALSA64_DATA_DIR "/sin-si-diferido");
	bool si_fixed = file_exists(SALSA64_DATA_DIR "/si-ritmo-viejo");
	salsa64_si_set_immediate(&n64.bus, si_immediate);
	salsa64_si_set_fixed(&n64.bus, si_fixed);
	salsa64_log("SI: transferencia %s%s", si_immediate ? "inmediata (sin-si-diferido)"
		    : si_fixed ? "diferida, 2 ms fijos (si-ritmo-viejo)"
		    : "diferida: escritura 65 µs, lectura según la PIF", si_immediate ? "" : " (si-ritmo-viejo)");
	/* Antes de que el recompilador compile nada: decide al compilar (vr4300.h). */
	bool dcache_sweep = !file_exists(SALSA64_DATA_DIR "/sin-barrido-cache");
	salsa64_cpu_set_dcache_sweep(&n64.cpu, dcache_sweep);
	salsa64_log("CPU: barridos de la caché de datos %s (sin-barrido-cache)",
		    dcache_sweep ? "con su costo" : "gratis");
	bool keep_snapshot = file_exists(SALSA64_DATA_DIR "/memoria-estados-vieja");
	states_keep_snapshot(keep_snapshot);
	salsa64_log("estados: foto %s al cerrar (memoria-estados-vieja)", keep_snapshot ? "retenida" : "liberada");
	states_init(SALSA64_RDRAM_MAX);  /* la foto de los estados y su hilo de escritura (docs/estados.md) */
	cajanegra_init(&n64.bus);        /* el anillo de la caja negra, si hay archivo cajanegra (docs/cajanegra.md) */
	n64.bus.rdram_written = front_rdram_written;
	salsa64_bus_set_isviewer(&n64.bus, isv_buf, isv_out, NULL);

	/* HLE gráfica: en un SPE, o en el PPE si se pide. */
	gfx_affine_chain = !file_exists(SALSA64_DATA_DIR "/sin-afin-cadena");
	gfx_s2dex_old_modes = file_exists(SALSA64_DATA_DIR "/gfx-s2dex-modos-viejos");
	gfx_no_fog_1cycle = file_exists(SALSA64_DATA_DIR "/sin-niebla-1ciclo");
	salsa64_log("gfx: niebla del blender en un ciclo %s (sin-niebla-1ciclo)",
		    gfx_no_fog_1cycle ? "apagada" : "activa");
	/* sin-sombra-afin: la HLE no recorta contra la caja del clip ratio y el
	 * RSX vuelve a interpolar el sombreado con perspectiva (docs/rsx-notes.md). */
	gfx_no_rsp_clip = file_exists(SALSA64_DATA_DIR "/sin-sombra-afin");
	salsa64_log("gfx: recorte del RSP y sombra afín %s (sin-sombra-afin)",
		    gfx_no_rsp_clip ? "apagados" : "activos");
	gfx_s2dex_bg_old = file_exists(SALSA64_DATA_DIR "/s2dex-marco-viejo");
	salsa64_log("gfx: fondos S2DEX con marco %s (s2dex-marco-viejo)",
		    gfx_s2dex_bg_old ? "antiguo" : "limitado");
	gfx_no_yuv = file_exists(SALSA64_DATA_DIR "/sin-yuv");
	salsa64_log("gfx: conversión YUV %s (sin-yuv)", gfx_no_yuv ? "apagada" : "encendida");
	gfx_blend_old = file_exists(SALSA64_DATA_DIR "/mezclas-viejas");
	salsa64_log("gfx: modos del blender %s (mezclas-viejas)", gfx_blend_old ? "los cuatro de antes" : "completos");
	gfx_s2dex_bg_row_old = file_exists(SALSA64_DATA_DIR "/s2dex-paso-viejo");
	salsa64_log("gfx: paso de las filas de los fondos de S2DEX %s (s2dex-paso-viejo)",
		    gfx_s2dex_bg_row_old ? "de imageW (el de antes)" : "en palabras de 64 bits, como el microcódigo");
	/* Indiana Jones: la lista secundaria de Factor 5 termina en cualquier
	 * G_ENDDL (f5indy.c, do_sub_dlist); f5-sublista-vieja vuelve a seguir
	 * hasta el B8 … FFFFFFFF, que ejecutaba vértices como comandos. */
	gfx_f5_sub_old = file_exists(SALSA64_DATA_DIR "/f5-sublista-vieja");
	salsa64_log("gfx: lista secundaria de Factor 5 %s (f5-sublista-vieja)",
		    gfx_f5_sub_old ? "hasta el B8 … FFFFFFFF (vieja)" : "hasta cualquier G_ENDDL");
	salsa64_log("gfx: escala afin de tramos 3D encadenados %s (sin-afin-cadena)",
		    gfx_affine_chain ? "activa" : "referencia");
	gfx_on_spu = !file_exists(GFX_PPE_PATH);
	bool turbo3d = !file_exists(SALSA64_DATA_DIR "/sin-turbo3d");
	salsa64_log("gfx: Turbo3D %s (sin-turbo3d)", turbo3d ? "activo" : "apagado");
	bool acclaim = !file_exists(SALSA64_DATA_DIR "/sin-acclaim");
	salsa64_log("gfx: HLE Acclaim %s (sin-acclaim)", acclaim ? "activa" : "apagada");
	bool eurocom = !file_exists(SALSA64_DATA_DIR "/sin-eurocom");
	salsa64_log("gfx: HLE Eurocom %s (sin-eurocom)", eurocom ? "activa" : "apagada");
	/* Conker (#68): sus variantes «F3DEX.NoN»… por el CRC del código. */
	bool conker_text_only = file_exists(SALSA64_DATA_DIR "/conker-ucode-viejo");
	salsa64_log("gfx: microcódigo de Conker %s (conker-ucode-viejo)",
		    conker_text_only ? "solo por su texto (viejo)" : "por su código, con todas sus variantes");
	bool xbus_old = file_exists(SALSA64_DATA_DIR "/texturas-xbus-viejo");
	bool texture_wait = file_exists(SALSA64_DATA_DIR "/texturas-espera-rsx");
	salsa64_rdp_set_texture_wait(texture_wait);
	salsa64_log("gfx: texturas XBUS %s (texturas-xbus-viejo); espera diagnóstica del RSX %s (texturas-espera-rsx)",
		    xbus_old ? "sin aislar" : "aisladas", texture_wait ? "activa" : "apagada");
	if (gfx_on_spu) {
		spukit_init(6);
		if ((rc = salsa64_gfx_spu_init(&gfx_spu, &n64.bus, &front_sink)) != 0) {
			salsa64_log("no arrancó el SPE de la HLE gráfica (%d): uso el PPE", rc);
			gfx_on_spu = 0;
		}
	}
	if (gfx_on_spu) {
		gfx_spu.backend.turbo3d = turbo3d;
		gfx_spu.backend.acclaim = acclaim;
		gfx_spu.backend.eurocom = eurocom;
		gfx_spu.backend.conker_text_only = conker_text_only;
		salsa64_rsp_set_gfx(&n64.bus, &gfx_spu.backend);
	} else {
		salsa64_gfx_sync_init(&gfx_ppe, &gfx_ppe_state, &n64.bus, &front_sink);
		gfx_ppe.backend.turbo3d = turbo3d;
		gfx_ppe.backend.acclaim = acclaim;
		gfx_ppe.backend.eurocom = eurocom;
		gfx_ppe.backend.conker_text_only = conker_text_only;
		salsa64_rsp_set_gfx(&n64.bus, &gfx_ppe.backend);
	}
	/* Duración mínima de una tarea gráfica, en ciclos de la N64 (rsp.c,
	 * gfx_cycles; 30000 por omisión). Con el RDP casi instantáneo, un juego
	 * que no espera el retrazo vuelve a dibujar en cuanto le llega la
	 * interrupción: Vigilante 8 arma 1,3 cuadros por campo y un tercio no
	 * se ve. Interruptor de prueba para medir en la consola; el host lo
	 * toma de SALSA64_GFX_CYCLES. */
	{
		FILE *f = fopen(SALSA64_DATA_DIR "/rsp-gfx-ciclos", "r");
		if (f) {
			unsigned cyc = 0;
			if (fscanf(f, "%u", &cyc) == 1 && cyc >= 1000 && cyc <= 4000000)
				n64.bus.rsp.gfx_cycles = cyc;
			fclose(f);
			salsa64_log("RSP: una tarea gráfica dura al menos %u ciclos (rsp-gfx-ciclos)", n64.bus.rsp.gfx_cycles);
		}
	}
	ritmo_rcp_setup(rom);
	/* Killer Instinct Gold (y otros) dibuja con listas del RDP que el CPU
	 * escribe por DPC_START/DPC_END, sin RSP, y espera la interrupción del
	 * DP de su SYNC_FULL antes de seguir: sin quien consuma esas listas el
	 * juego no manda ninguna tarea gráfica. Las consume la misma HLE que las
	 * tareas (en el PPE o en el SPE): con otro estado del RDP y otro
	 * directorio de texturas sobre la misma región de la caché, cada uno
	 * pisaba las texturas del otro. */
	if (gfx_on_spu) {
		/* Solo para las listas en la DMEM (xbus), que el SPE no lee. */
		salsa64_gfx_sync_init(&gfx_ppe, &gfx_ppe_state, &n64.bus, &front_sink);
		/* Este backend tiene un directorio distinto al del SPE sobre la misma
		 * región. Su arena de tarea sí comparte las barreras del consumidor. */
		gfx_ppe.nocache = !xbus_old;
		gfx_spu.xbus_safe = !xbus_old;
		gfx_spu.xbus = &gfx_ppe.dpc;
	}
	salsa64_rsp_set_dpc(&n64.bus, gfx_on_spu ? &gfx_spu.dpc : &gfx_ppe.dpc);
	salsa64_log("HLE gráfica en %s", gfx_on_spu ? "el SPE" : "el PPE");
	n64.bus.rsp.clock_us = clock_us;
	/* audio_out: inicio (HLE en el SPE 1, sumidero del AI, puerto de audio). */
	bool audio_old_ranges = file_exists(SALSA64_DATA_DIR "/audio-rangos-viejos");
	salsa64_log("audio: resumen de escrituras %s (audio-rangos-viejos)",
		    audio_old_ranges ? "anterior" : "por hueco menor");
	/* audio-polef-cero: el POLEF de ABI1 con A_INIT parte de cero, como antes, en vez de tomar las
	 * muestras previas de la zona de estados del microcódigo (docs/audio-censo.md). */
	bool audio_polef_zero = file_exists(SALSA64_DATA_DIR "/audio-polef-cero");
	salsa64_log("audio: POLEF de ABI1 con A_INIT %s (audio-polef-cero)",
		    audio_polef_zero ? "desde cero" : "como el microcódigo");
	audio_out_init(&n64, (audio_old_ranges ? AUD_JOB_RANGES_OLD : 0) | (audio_polef_zero ? AUD_JOB_POLEF_INIT_ZERO : 0));
	/* Tarea JPEG (fondos prerrenderizados de Zelda OoT) en otro SPE, salvo
	 * que se pida el PPE o que el SPE no dé lo mismo que el PPE. */
	jpeg_on_spu = 0;
	if (!file_exists(JPEG_PPE_PATH)) {
		bool same;
		spukit_init(6);
		if ((rc = salsa64_jpeg_spu_init(&jpeg_spu, &n64.bus, &same)) != 0) {
			salsa64_log("JPEG: no arrancó el SPE (%d)", rc);
		} else if (!same) {
			salsa64_jpeg_spu_shutdown(&jpeg_spu);
		} else {
			salsa64_rsp_set_jpeg(&n64.bus, &jpeg_spu.backend);
			jpeg_on_spu = 1;
		}
	}
	salsa64_log("JPEG: tareas en %s", jpeg_on_spu ? "un SPE"
						      : file_exists(JPEG_PPE_PATH) ? "el PPE (" JPEG_PPE_PATH ")" : "el PPE");
	/* RSP a bajo nivel en otro SPE. */
	lle_mode = lle_config();
	if (lle_mode != SALSA64_LLE_OFF) {
		spukit_init(6);
		if ((rc = salsa64_rsp_spu_init(&rsp_spu, &n64.bus)) != 0) {
			salsa64_log("rsp lle: no arrancó el SPE del microcódigo (%d): sin LLE", rc);
			lle_mode = SALSA64_LLE_OFF;
		} else {
			/* El microcódigo real entrega cientos de tramos por tarea: el RDP
			 * en el PPE, no una espera al SPE por cada uno. */
			salsa64_rsp_set_dpc(&n64.bus, &gfx_ppe.dpc);
			salsa64_rsp_set_lle(&n64.bus, &rsp_spu.backend, lle_mode);
			salsa64_log("rsp lle: microcódigo real en un SPE para %s; el RDP en el PPE",
				    lle_mode == SALSA64_LLE_ALL ? "todas las tareas" : "lo que no tiene HLE");
		}
	}

	salsa64_faultlog_install(&n64.cpu, salsa64_log);
	if (rom == rom_head)
		salsa64_bus_set_rom_paged(&n64.bus, rom, ROM_HEAD, rom_size, &rom_pg);
	else
		salsa64_bus_set_rom(&n64.bus, rom, rom_size);
	if (!salsa64_boot_hle(&n64, SALSA64_BOOT_TV_AUTO, &boot_info)) {
		salsa64_log("%s: FALLO al arrancar %s: %s", res_tag, source, boot_info.error);
		snprintf(err, err_size, tr("No arrancó %s: %s"), source, boot_info.error);
		emu_done = 1;
		game_close();
		return GAME_FAILED;
	}
	char title[21];
	memcpy(title, rom + 0x20, 20);
	title[20] = '\0';
	salsa64_log("ROM %s \"%s\", %u bytes, %s, entrada 0x%08X, TV %s", source, title, rom_size,
		    boot_info.kind == SALSA64_BOOT_ELF ? "ELF (IPL3 libre)" : "carga plana", boot_info.entry,
		    boot_info.tv == SALSA64_TV_PAL ? "PAL" : boot_info.tv == SALSA64_TV_MPAL ? "MPAL" : "NTSC");
	if (rom == rom_head)
		salsa64_log("ROM: por fragmentos, sin CRC-32 por megabyte (leería %u MB del disco)", rom_size >> 20);
	else if (!(selftest_on || gfxtest_on || audiotest_on))
		log_rom_crcs(rom, rom_size);
	savefile_open(&save_file, &save, &n64.bus, rom, SAVES_DIR, SAVE_CFG, SALSA64_SAVE_AUTO, salsa64_log);
	/* Yoshi's Story (NYS, todas las regiones) lee sus capas de fondo CI8 desde la RDRAM: espera sus copias. */
	salsa64_rdp_set_big8_wait(rom[0x3B] == 'N' && rom[0x3C] == 'Y' && rom[0x3D] == 'S');
	/* Las capas de 8 bits se ceban desde la RDRAM cuando su superficie es
	 * nueva o quedó vieja; `gfx-big8-sin-cebar` lo apaga (para comparar). */
	/* Las copias del RSX a la RDRAM a 1:1 (las capas de 8 bits) van con el
	 * filtro más cercano; `gfx-copia-lineal` vuelve al lineal (para comparar). */
	if (file_exists(SALSA64_DATA_DIR "/gfx-copia-lineal")) {
		rsxkit_target_read_exact(0);
		salsa64_log("copias del RSX a la RDRAM a 1:1 con filtro lineal (gfx-copia-lineal)");
	}
	/* Blindaje del RSX (docs/rsx-notes.md: Indiana Jones colgó la PS3 con
	 * una color image de 900000/455 y una copia de la z de 455 de ancho):
	 * las color images fuera de la RDRAM no tienen destino y las copias con
	 * el motor de imagen escalada van con ancho de entrada par.
	 * `rsx-sin-blindaje` lo apaga (para comparar). */
	{
		bool safe = !file_exists(SALSA64_DATA_DIR "/rsx-sin-blindaje");
		rsxkit_set_safe(safe ? RSXKIT_SAFE_ALL : 0);
		salsa64_rdp_set_rsx_safe(safe);
		salsa64_log("RSX: blindaje %s (color images fuera de la RDRAM sin destino, copias de la z con ancho par)%s",
			    safe ? "encendido" : "APAGADO", safe ? "" : " (rsx-sin-blindaje)");
	}
	/* Las capas de 8 bits las dibuja el PPE en la RDRAM (sin esperar la copia
	 * del RSX); `gfx-capas8-rsx` vuelve a dibujarlas en el RSX. */
	if (file_exists(SALSA64_DATA_DIR "/gfx-capas8-rsx")) {
		salsa64_rdp_set_layer8(false);
		salsa64_log("capas de 8 bits (big8): en el RSX con copia a la RDRAM (gfx-capas8-rsx)");
	}
	/* Las copias a la RDRAM de los destinos con alfa llevan su alfa al bit de
	 * RGBA5551; `copia-sin-alfa` vuelve a copiarlas con alfa 1 (para comparar). */
	{
		bool copy_alpha = !file_exists(SALSA64_DATA_DIR "/copia-sin-alfa");
		salsa64_rdp_set_copy_alpha(copy_alpha);
		if (!copy_alpha)
			salsa64_log("gfx: copias a la RDRAM de los destinos con alfa sin su alfa (copia-sin-alfa)");
	}
	/* Los dibujos dejan en los destinos con alfa la cobertura del RDP, no el
	 * alfa de la mezcla; `alfa-fragmento` vuelve al alfa del fragmento, como
	 * hasta la v0.2.15 (Dr. Mario 64, #62). */
	{
		bool cvg_alpha = !file_exists(SALSA64_DATA_DIR "/alfa-fragmento");
		salsa64_rdp_set_cvg_alpha(cvg_alpha);
		if (!cvg_alpha)
			salsa64_log("gfx: los destinos con alfa guardan el alfa del fragmento, como la v0.2.15 "
				    "(alfa-fragmento)");
	}
	/* Las imágenes chicas de 32 bits (la luz de los cuartos oscuros de Paper
	 * Mario) las dibuja el PPE en la RDRAM; `gfx-mascara32-rsx` las deja en
	 * el RSX como antes. */
	{
		bool m32 = !file_exists(SALSA64_DATA_DIR "/gfx-mascara32-rsx");
		salsa64_rdp_set_mask32(m32);
		if (!m32)
			salsa64_log("imágenes chicas de 32 bits: en el RSX (gfx-mascara32-rsx)");
	}
	{
		bool big8_prime = !file_exists(SALSA64_DATA_DIR "/gfx-big8-sin-cebar");
		salsa64_rdp_set_big8_prime(big8_prime);
		if (!big8_prime)
			salsa64_log("capas de 8 bits (big8): sin cebar desde la RDRAM (gfx-big8-sin-cebar)");
	}
	/* LEGO Racers (NLG) comparte la z image entre auxiliares de 32 x 32 y el
	 * framebuffer: cada borrado se difiere hasta ligar su color image. */
	bool lego_zclear = rom[0x3B] == 'N' && rom[0x3C] == 'L' && rom[0x3D] == 'G';
	bool zclear_old = file_exists(SALSA64_DATA_DIR "/zclear-inmediato");
	lego_zclear = lego_zclear && !zclear_old;
	salsa64_rdp_zclear_target(lego_zclear);
	if (rom[0x3B] == 'N' && rom[0x3C] == 'L' && rom[0x3D] == 'G')
		salsa64_log("profundidad: borrados %s (zclear-inmediato)",
			lego_zclear ? "ligados al próximo destino" : "inmediatos");
	{
		struct menu_game gc;
		menu_game_default(&gc);
		if (source && !(selftest_on || gfxtest_on || audiotest_on))
			game_cfg_read(source, &gc);
		/* Global defaults come from Opciones; per-game files may override them. */
		int configured_accessory = gc.accessory < 0 ? menu_accessory_default() : gc.accessory;
		enum salsa64_pif_accessory accessory =
			(selftest_on || gfxtest_on || audiotest_on || configured_accessory == MENU_ACC_NONE)
				? SALSA64_PIF_ACCESSORY_NONE
				: configured_accessory == MENU_ACC_RUMBLE ? SALSA64_PIF_ACCESSORY_RUMBLE_PAK
								   : configured_accessory == MENU_ACC_TRANSFER ? SALSA64_PIF_ACCESSORY_TRANSFER_PAK
								   : SALSA64_PIF_ACCESSORY_CONTROLLER_PAK;
		/* Load every Controller Pak up front so pause-menu swaps work in-place. */
		mempakfile_open(&mempak_file, &mempak, &n64.bus, rom, SAVES_DIR,
				(selftest_on || gfxtest_on || audiotest_on) ? 0u : 0xFu, salsa64_log);
		for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++) {
			transferpakfile_open(&transferpak_files[i], &n64.bus.pif, i, source, salsa64_log);
			salsa64_transferpak_reset(&n64.bus.pif.transferpak_state[i]);
			n64.bus.pif.accessory[i] = (uint8_t)accessory;
			n64.bus.pif.rumble[i] = 0;
		}
		if (accessory == SALSA64_PIF_ACCESSORY_RUMBLE_PAK)
			salsa64_log("accesorio: Rumble Pak emulado en los cuatro controles");
		else if (accessory == SALSA64_PIF_ACCESSORY_TRANSFER_PAK)
			salsa64_log("accesorio: Transfer Pak emulado en los cuatro controles");
		rumble_publish();
	}

	rec.opts = SALSA64_REC_OPT_PIN;  /* +7 % en nemu64-test (docs/perf-log.md) */
	if (file_exists(SALSA64_DATA_DIR "/jit-diag")) {
		rec.opts |= SALSA64_REC_OPT_DIAG;
		salsa64_log("recompilador: diagnóstico de huellas activo (jit-diag)");
	}
	if (file_exists(SALSA64_DATA_DIR "/jit-invalidacion-vieja"))
		rec.opts |= SALSA64_REC_OPT_IDLE_RECOMPILE;
	salsa64_log("recompilador: descartes de espera por %s (jit-invalidacion-vieja para recompilar)",
		    rec.opts & SALSA64_REC_OPT_IDLE_RECOMPILE ? "recompilacion de rangos" : "parche de una palabra PPC");
	rec.fm_window = NULL;
	if (salsa64_ps3_fastmem_ptr() && rdram == salsa64_ps3_fastmem_ptr()) {
		rec.opts |= SALSA64_REC_OPT_FASTMEM;
		rec.fm_window = rdram;
	}
	if (salsa64_ps3_rec_legacy_wanted())
		rec.opts |= SALSA64_REC_OPT_LEGACY;
	if (salsa64_ps3_rec_fpu_res_wanted())
		rec.opts |= SALSA64_REC_OPT_FPU_RES;
	if (salsa64_ps3_rec_cop0_exec_wanted())
		rec.opts |= SALSA64_REC_OPT_COP0_EXEC;
	if (salsa64_ps3_rec_no_evict_wanted())
		rec.opts |= SALSA64_REC_OPT_NO_EVICT;
	if (salsa64_ps3_rec_wide_wanted())
		rec.opts |= SALSA64_REC_OPT_WIDE;
	if (salsa64_ps3_rec_no_group_wanted())
		rec.opts |= SALSA64_REC_OPT_NO_GROUP;
	if (salsa64_ps3_rec_fpu_inline_wanted())
		rec.opts |= SALSA64_REC_OPT_FPU_INLINE;
	if (salsa64_ps3_rec_jtc_wanted())
		rec.opts |= SALSA64_REC_OPT_JTC;
	if (salsa64_ps3_rec_jtc_stats_wanted())
		rec.opts |= SALSA64_REC_OPT_JTC_STATS;
	if (salsa64_ps3_rec_old_copy_wanted())
		rec.opts |= SALSA64_REC_DBG_NO_COPY_FREE;
	rec.prof = salsa64_ps3_rec_prof_wanted() ? &rec_prof : NULL;
	/* Perfilado (perfilado_ps3.h): la sesión y rec.trace, antes de iniciar
	 * el recompilador (su preámbulo depende de rec.trace). */
	perf_app_open(rom, &rec, &n64.bus.vi.fields);
	{
		bool sites;
		rec.hot_bytes = file_exists(PROFILE_ON_PATH) ? profile_hot_kb(&sites) << 10 : 0;
	}
	char probe_why[160];
	bool rec_wanted = salsa64_ps3_rec_wanted(), probe_ok = false, probe_first = false;
	if (rec_wanted) {
		/* Antes de tocar la caché: con CFW el hilo podía quedarse trabado
		 * en salsa64_rec_init (issue #2; rec_cache.h). */
		probe_ok = salsa64_ps3_rec_probe_wait(probe_why, sizeof probe_why, &probe_first);
		if (!probe_ok)
			salsa64_log("recompilador no disponible en esta consola (%s): intérprete", probe_why);
		else if (probe_first)
			salsa64_log("recompilador: caché de código probada (%s)", probe_why);
	}
	if (!rec_wanted)
		salsa64_log("recompilador apagado (%s): intérprete", SALSA64_NOREC_FLAG);
	else if (!probe_ok)
		;
	else if (!(use_rec = salsa64_rec_init(&rec, &n64.cpu, salsa64_ps3_rec_cache(), SALSA64_REC_CACHE_BYTES)))
		salsa64_log("recompilador no disponible: intérprete");
	else {
		if (rec.fastmem && !salsa64_ps3_fastmem_start(&rec))
			salsa64_rec_fastmem_off(&rec);
		salsa64_log("páginas: RDRAM %u KB, rec %u KB, n64 %u KB, caché de código %u KB, pila %u KB (0 = sin dato)",
			    page_kb(rdram), page_kb(&rec), page_kb(&n64), page_kb(salsa64_ps3_rec_cache()),
			    page_kb(__builtin_frame_address(0)));
		salsa64_log("recompilador activo: caché de %u MB en %p, llamadas a C %s, opciones 0x%x%s%s%s%s%s%s%s%s%s%s%s",
			    SALSA64_REC_CACHE_BYTES >> 20, salsa64_ps3_rec_cache(), rec.calls_near ? "con bl" : "por mtctr",
			    rec.opts, rec.prof ? ", con perfil" : "",
			    rec.opts & SALSA64_REC_OPT_LEGACY ? ", camino viejo (jit-viejo)" : "",
			    rec.opts & SALSA64_REC_OPT_FPU_RES ? ", FPU residente (jit-fpu-residente)" : "",
			    rec.opts & SALSA64_REC_OPT_COP0_EXEC ? ", COP0 por el intérprete (jit-cop0-viejo)" : "",
			    rec.opts & SALSA64_REC_OPT_NO_EVICT ? ", sin tramos (jit-sin-tramos)" : "",
			    rec.opts & SALSA64_REC_OPT_WIDE ? ", código ancho (jit-ancho)" : "",
			    rec.fastmem ? ", fastmem (jit-fastmem)" : rec.opts & SALSA64_REC_OPT_FASTMEM ? ", fastmem APAGADO" : "",
			    rec.opts & SALSA64_REC_OPT_NO_GROUP ? ", sin base en caché (jit-sin-grupos)" : "",
			    rec.opts & SALSA64_REC_OPT_JTC ? (rec.opts & SALSA64_REC_OPT_JTC_STATS ? ", caché de destinos con cuentas (jit-destinos-cuentas)" : ", caché de destinos") : ", sin caché de destinos (jit-sin-destinos)",
			    rec.opts & SALSA64_REC_OPT_FPU_INLINE ? ", FPU en línea (jit-fpu-en-linea)" : rec.fpu_rtn_ok ? "" : ", FPU en línea (las rutinas no cupieron)",
			    rec.opts & SALSA64_REC_DBG_NO_COPY_FREE ? ", copias sin liberar (jit-copias-viejas)" : ", copias en ranuras");
	}
	profile_open(rom);
	sample_open();

	/* overscan.cfg: en cada juego (se cambia desde las Opciones del menú). */
	load_overscan();
	load_view(source);
	filter_setup(view_crt);  /* antes de la escala: el CRT la limita */
	auto_scale.disabled = file_exists(SCALE_AUTO_OFF_PATH);
	scale_control_configure(true);
	const char *scale_origin = img.game.scale >= 0 ? "por juego" : video_scale_configured ? "global" : "por omisión";
	const char *scale_mode = menu_scale_is_auto(img.scale) ? "automática" : "fija";
	salsa64_log("escala interna: %s, modo %s%s, arranca en %d×, máximo %d×, techo vigente %u×; "
		    "baja tras 3 s con VI ≥ 92 %% y RSX ≥ 100 %%; no sube automáticamente",
		    scale_origin, scale_mode, auto_scale.disabled && menu_scale_is_auto(img.scale)
					       ? " (control apagado por sin-escala-auto)" : "",
		    scale_policy.current, scale_policy.maximum, scale_auto_ceiling(&scale_policy));
	rdp_scale_set(scale_wanted(), scale_capped() ? "al abrir el juego, tope de 3× con un CRT" : "al abrir el juego");
	load_cheats(source);
	gamefix_setup();
	ra_game_open(rom, rom_size, rdram, SALSA64_RDRAM_MAX);  /* RetroAchievements, si está activado y hay sesión */
	if (selftest_on)
		selftest_start();
	if (gfxtest_on)
		gfxtest_start();
	if (audiotest_on)
		audiotest_start();
	{
		/* SALSA64_DATA_DIR/fbfx.cfg: 0 sin efectos de framebuffer, 1 solo
		 * render a textura, 2 también copias a la RDRAM (por omisión). */
		int fbfx = 2;
		FILE *ff = fopen(SALSA64_DATA_DIR "/fbfx.cfg", "r");
		if (ff) {
			if (fscanf(ff, "%d", &fbfx) != 1)
				fbfx = 2;
			fclose(ff);
		}
		salsa64_rdp_fbfx(fbfx);
		salsa64_log("efectos de framebuffer: %s", fbfx <= 0 ? "apagados" : fbfx == 1 ? "render a textura" :
								"render a textura y copias a la RDRAM");
	}
	{
		FILE *fi = fopen(INTERP_TEST_PATH, "r");
		interp_test_t = -1.0f;
		if (fi) {
			interp_test_t = 1.0f;
			if (fscanf(fi, "%f", &interp_test_t) != 1)
				interp_test_t = 1.0f;
			fclose(fi);
			salsa64_log("interpolación (prueba, " INTERP_TEST_PATH "): emparejar cada tarea con la anterior y "
				    "dibujarla en t = %.2f, sin campos intermedios", (double)interp_test_t);
		}
		interp_apply();
		memset(&interp_alt, 0, sizeof interp_alt);
		fi = fopen(INTERP_ALT_PATH, "r");
		if (fi) {
			int n = fscanf(fi, "%u %d %d", &interp_alt.every, &interp_alt.scale[0], &interp_alt.scale[1]);
			fclose(fi);
			if (n < 3 || interp_alt.scale[0] < 0 || interp_alt.scale[1] < 0)
				interp_alt.scale[0] = interp_alt.scale[1] = 0;
			if (n >= 1 && interp_alt.every) {
				interp_alt.next = interp_alt.every;
				interp_alt.stage = 1;
				salsa64_log("interpolación (prueba, " INTERP_ALT_PATH "): se cambia en una pausa cada %u campos%s",
					    interp_alt.every, interp_alt.scale[0] ? ", con la escala" : "");
			}
		}
		/* SALSA64_DATA_DIR/aristas-aa: la pasada de aristas de los dibujos
		 * con AA_EN (salsa64_rdp_aa_edges), experimental y apagada por
		 * omisión: en la PS3 engordaba los modelos (docs/rsx-notes.md). */
		bool aa_edges = file_exists(SALSA64_DATA_DIR "/aristas-aa");
		/* Guarda de las copias del RSX a la RDRAM contra lo que el CPU
		 * escribió después de la tarea (salsa64_rdp_guard): `sin-guarda-cpu`
		 * la apaga (para comparar); `vigilante.cfg` con dos direcciones
		 * físicas en hexadecimal («6b3000 749000») enciende el vigilante: el
		 * log dice cada escritura que no es del CPU en ese rango. */
		{
			bool guard_on = !file_exists(SALSA64_DATA_DIR "/sin-guarda-cpu");
			unsigned wlo = 0, whi = 0;
			FILE *wf = fopen(SALSA64_DATA_DIR "/vigilante.cfg", "r");
			if (wf) {
				if (fscanf(wf, "%x %x", &wlo, &whi) != 2 || whi <= wlo)
					wlo = whi = 0;
				fclose(wf);
			}
			salsa64_rdp_guard(guard_on, wlo, whi);
			if (!guard_on || whi)
				salsa64_log("guarda de las copias contra el CPU: %s; vigilante %s0x%06x–0x%06x",
					    guard_on ? "encendida" : "apagada (sin-guarda-cpu)", whi ? "en " : "apagado ",
					    wlo, whi);
			/* El espejo del framebuffer no escribe las líneas que otra color
			 * image dibujó después (salsa64_rdp_claims; FIFA 99 al acabar el
			 * partido): `espejo-pisa-otras` vuelve a como antes. */
			bool claims_on = !file_exists(SALSA64_DATA_DIR "/espejo-pisa-otras");
			salsa64_rdp_claims(claims_on);
			salsa64_log("gfx: el espejo del framebuffer %s",
				    claims_on ? "no pisa lo que otra color image dibujó después (espejo-pisa-otras)"
					      : "pisa lo que otra color image dibujó después, como antes (espejo-pisa-otras)");
		}
		/* Alfa en los destinos de render a textura (salsa64_rdp_target_alpha):
		 * `sin-alfa-destinos` vuelve a los destinos X8R8G8B8 de antes. */
		bool tgt_alpha = !file_exists(SALSA64_DATA_DIR "/sin-alfa-destinos");
		salsa64_rdp_target_alpha(tgt_alpha);
		salsa64_log("alfa en los destinos de render a textura: %s",
			    tgt_alpha ? "encendido" : "apagado (sin-alfa-destinos)");
		salsa64_rdp_aa_edges(aa_edges);
		salsa64_rdp_affine_shade(!gfx_no_rsp_clip);
		if (aa_edges)
			salsa64_log("aristas de los dibujos con AA: encendidas (aristas-aa, experimental)");
	}
	{
		/* SALSA64_DATA_DIR/trace-vi: "N" o "N INICIO" registra una
		 * ventana de campos, incluso si se salta o no hay imagen. */
		FILE *tf = fopen(SALSA64_DATA_DIR "/trace-vi", "r");
		trace_vi_count = 0;
		trace_vi_first = 1;
		if (tf) {
			if (fscanf(tf, "%u %u", &trace_vi_count, &trace_vi_first) < 1)
				trace_vi_count = 300;
			fclose(tf);
			if (trace_vi_count)
				salsa64_log("traza VI: %u campos desde el %u", trace_vi_count, trace_vi_first);
		}
	}
	{
		FILE *df = fopen(SALSA64_DATA_DIR "/gfxdump", "r");
		if (df) {
			if (fscanf(df, "%u", &pres.dump_field) != 1)
				pres.dump_field = 0;
			fclose(df);
		}
	}
	/* El puerto 1 con control neutro desde el primer campo. */
	update_pads(NULL);
	pacer_init(&pacer, !file_exists(TURBO_PATH), pacer_sleep, NULL);
	salsa64_log("ritmo: %s", pacer.enabled ? "el de la tele emulada (cada campo a su hora)" : "sin límite (turbo)");
	pres.shots_on = pres.autotest || file_exists(SHOTS_PATH);
	hang_test_load();
	{
		FILE *cf = fopen(SHOTS_PATH, "r");
		if (cf) {
			if (fscanf(cf, "%u %u", &cap_every, &exit_field) < 1)
				cap_every = exit_field = 0;
			fclose(cf);
		}
	}
	if (cap_every)
		salsa64_log("capturas: campo-N.ppm cada %u campos%s", cap_every, exit_field ? ", y salida" : "");
	else if (pres.shots_on && !pres.autotest)
		salsa64_log("capturas: screen-N.ppm cada %u s (%s)", SHOT_US / 1000000u, SHOTS_PATH);
	gwin.on = gfx_on_spu;
	if (perf.on) {
		/* Contexto de la sesión (docs/perfilado.md): qué app, qué juego y
		 * con qué ajustes corre. Sin bytes del juego: su nombre interno y
		 * sus CRC, como el nombre de las partidas. */
		char ctx[2048], cfg[320], name[21];
		uint64_t sec = 0, nsec = 0;
		sysGetCurrentTime(&sec, &nsec);
		memcpy(name, rom + 0x20, 20);
		name[20] = '\0';
		for (int k = 19; k >= 0 && name[k] == ' '; k--)
			name[k] = '\0';
		perf_cfg_text(&perf.cfg, cfg, sizeof cfg);
		snprintf(ctx, sizeof ctx,
			 "version=%s\ncommit=%s\napp=%s\nappid=%s\njuego=%s\ncarpeta=%s\ncodigo=%.4s\ncrc1=%08X\ncrc2=%08X\n"
			 "region=%c\ntv=%s\nrom_bytes=%u\narranque=%s\nrecompilador=%s\nrec_opciones=0x%x\ntraza=%s\n"
			 "hle_grafica=%s\njpeg=%s\nlle=%s\nescala=%d\nescala_rdp=%u\nfiltro=%s\noverscan=%u,%u,%u,%u\n"
			 "aspecto=%.3f\nresolucion=%d\ninterpolacion=%s\nritmo=%s\nperfil_persistente=%s\n"
			 "muestreo=%s\nfecha_unix=%llu\nperfilado=%s\n",
			 salsa64_version, salsa64_commit, salsa64_title, salsa64_appid, name, perf.game, (const char *)rom + 0x3B,
			 (unsigned)salsa64_rd32(rom + 0x10), (unsigned)salsa64_rd32(rom + 0x14),
			 rom[0x3E] >= 0x20 && rom[0x3E] < 0x7F ? rom[0x3E] : '?',
			 boot_info.tv == SALSA64_TV_PAL ? "PAL" : boot_info.tv == SALSA64_TV_MPAL ? "MPAL" : "NTSC", rom_size,
			 boot_info.kind == SALSA64_BOOT_ELF ? "elf" : "plana", use_rec ? "si" : "no (intérprete)", rec.opts,
			 use_rec && rec.trace && rec.trace->seq && !(rec.opts & SALSA64_REC_OPT_FPU_RES) ? "si" : "no",
			 gfx_on_spu ? "spe" : "ppe", jpeg_on_spu ? "spe" : "ppe",
			 lle_mode == SALSA64_LLE_OFF ? "no" : lle_mode == SALSA64_LLE_ALL ? "todo" : "auto", scale_wanted(),
			 salsa64_rdp_scale(NULL), salsa64_crt_name(flt.cur), overscan.left, overscan.right, overscan.top,
			 overscan.bottom, (double)pres.view.aspect, video_cfg.resolution,
			 interp_test_t >= 0.0f ? "prueba" : img.interp == MENU_INTERP_GEOMETRY ? "geometria" : "no",
			 pacer.enabled ? "tele" : "turbo",
			 rprof.on ? rprof.path : "no", rsmp.on ? "si" : "no", (unsigned long long)sec, cfg);
		perf_app_context(ctx);
	}
	{
		FILE *tf = fopen(STATE_TEST_PATH, "r");
		if (tf) {
			if (fscanf(tf, "%u %u", &state_test.save_at, &state_test.load_at) == 2 &&
			    state_test.save_at < state_test.load_at) {
				/* «0 B»: solo cargar en el campo B lo que ya tenga la
				 * ranura 9 (un estado hecho en el host, para llegar a
				 * una escena de un juego sin manos). */
				state_test.stage = state_test.save_at ? ST_SAVE : ST_LOAD;
				if (state_test.save_at)
					salsa64_log("estados, prueba automática: guardar en el campo %u y cargar en el %u (ranura 9)",
						    state_test.save_at, state_test.load_at);
				else
					salsa64_log("estados, prueba automática: cargar la ranura 9 en el campo %u",
						    state_test.load_at);
			}
			fclose(tf);
		}
	}

	/* El hilo de emulación. */
	reclaim_emu = file_exists(SALSA64_DATA_DIR "/memoria-hilos");
	salsa64_log("memoria: limpieza del hilo de emulación %s (memoria-hilos)",
		    reclaim_emu ? "experimental activa" : "referencia");
	sys_ppu_thread_t emu_tid;
	if (sysThreadCreate(&emu_tid, emu_thread, NULL, EMU_PRIO, EMU_STACK, THREAD_JOINABLE,
			    (char *)"salsa64_emu") != 0) {
		salsa64_log("%s: FALLO al crear el hilo de emulación", res_tag);
		snprintf(err, err_size, "%s", tr("No se pudo crear el hilo de emulación"));
		emu_done = 1;
		game_close();
		rdp_scale_set(menu_scale_limit(video_cfg.scale, video_cfg.resolution), "vuelve la global");
		return GAME_FAILED;
	}
	cuelgue_reset(&cuelgue, file_exists(SALSA64_DATA_DIR "/latido-siempre"));
	if (cuelgue.always)
		salsa64_log("latido: avisa y vuelca también si el juego nunca leyó los controles ni dibujó (latido-siempre)");
	emu_live = 1;
	salsa64_log("hilos: emulación con prioridad %d, presentación con 1001", EMU_PRIO);
	/* Avisos y atajos con SELECT (estados guardados, contador de FPS). */
	ui_init_once();
	fps.on = fps_setting_load();
	osd_fps_reset(&fps);
	pause_ctl.confirm = confirm_setting_load();
	sys_no_pause = file_exists(NO_PAUSE_PATH);
	if (sys_no_pause)
		salsa64_log("pausa: %s presente, el menú del sistema no pausa la emulación (diagnóstico)", NO_PAUSE_PATH);
	combos_on = 1;
	salsa64_log("estados: %s/<juego>.st0 … .st%d; SELECT + R1 guarda, + L1 carga, + izquierda/derecha cambia de "
		    "ranura; FPS %s (SELECT + △); SELECT + R2 vuelca la HLE; START + SELECT %s", STATES_DIR,
		    SALSA64_STATE_SLOTS - 1, fps.on ? "sí" : "no",
		    pause_ctl.confirm ? "pregunta antes de salir" : "sale directo (confirmar = no)");

	uint64_t now = sysGetSystemTime();
	pres.t_shot = now;
	stats(now);
	while (running) {
		beat.pres++;
		beat.pres_at = "espera";
		busy_stop(&h1.busy, tb_now());
		sysSemWait(wake_sem, WAKE_US);
		busy_start(&h1.busy, tb_now());
		beat.pres_at = "sysutil";
		sysutil_check();
		beat.pres_at = "mandos";
		if (update_pads(NULL)) {
			salsa64_log("menú de pausa%s: %s", pause_ctl.confirm ? " y confirmación" : "",
				    restart_cold ? "reinicio desde cero" : in_menu ? "vuelta al menú" : "salida");
			break;
		}
		pause_update();
		now = sysGetSystemTime();
		filter_cycle(now);
		beat.pres_at = "guardado";
		savefile_write_staged(&save_file, now);
		mempakfile_write_staged(&mempak_file, now);
		for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++)
			transferpakfile_write_staged(&transferpak_files[i], now);
		profile_write_step(now);
		ra_pump(pause_req != 0);
		for (char note[160]; ra_poll_notice(note, sizeof note);)
			osd_notice(now, "%s", note);
		/* Con el sistema encima (su menú, un aviso), el sistema dibuja lo
		 * suyo en cada flip de la aplicación: el último cuadro se sigue
		 * presentando, pero solo si el flip anterior ya salió, para no
		 * esperarlo. Los campos quedan en cola (emulación en pausa); los
		 * eventos y los mandos, cada vuelta. */
		bool overlay = pausectl_overlay(&pause_ctl);
		uint32_t shown = pres.frames_total;
		if (!overlay) {
			beat.pres_at = "cola";
			drain(now, 1);
			if (h1.load) {
				uint64_t t = tb_now();
				if (t >= h1.load_next) {
					h1.load_phase = (h1.load_phase + 1) % LOAD_PHASES;
					h1.load_next = t + spukit_tick_hz() / 2;
				}
				uint32_t us = h1.load_us[h1.load_phase] * (pres.frames_total - shown);
				if (us)
					h1_load_run(us);
			}
			if (state_parked && state_req) {
				beat.pres_at = "barrera";
				state_barrier();
			}
			scale_control_apply();
		} else {
			beat.pres_at = "sistema encima";
		}
		/* En pausa: el último cuadro otra vez si no llegó ninguno. */
		if (pause_req && pres.frames_total == shown && sysGetSystemTime() - pres.t_flip >= STILL_US &&
		    (!overlay || rsxkit_frame_ready()))
			present_still();
		if (state_test.stage)
			state_test_step();
		if (interp_alt.stage)
			interp_alt_step();
		state_bg_poll();
		cajanegra_pump(now);
		if (fps.on)
			osd_fps_sample(&fps, now, n64.bus.vi.fields, n64.cpu.cycles, pres.game_frames, pres.frames_total);
		scale_control_sample(now);
		stats(now);
	}
	beat.pres_at = "cierre";
	combos_on = 0;
	if (app_quit)
		salsa64_log("«Salir del juego» del sistema");
	if (pause_req)
		salsa64_log("pausa: el juego se cierra en pausa (%.1f s)", (sysGetSystemTime() - pause_log.t0) / 1e6);
	pausectl_game_end(&pause_ctl);

	/* Parar la emulación sin dejarla esperando un juego de búferes (si
	 * estaba en pausa, running = 0 la suelta). */
	running = 0;
	u64 stop_t0 = sysGetSystemTime();
	while (!emu_done) {
		drain(sysGetSystemTime(), 0);
		sysUsleep(500);
		if (sysGetSystemTime() - stop_t0 > QUIT_WAIT_US)
			forced_exit("la emulación no respondió al cierre", QUIT_WAIT_US / 1000000);
	}
	u64 ret;
	sysThreadJoin(emu_tid, &ret);
	emu_live = 0;
	drain(sysGetSystemTime(), 0);
	game_close();
	rdp_scale_set(menu_scale_limit(video_cfg.scale, video_cfg.resolution), "vuelve la global");
	if (restart_cold && !app_quit)
		return GAME_RESTART;
	return app_quit || !in_menu ? GAME_QUIT : GAME_TO_MENU;
}

/* Un juego, con «Reiniciar desde cero»: se cierra del todo (game_close escribe
 * las partidas, suelta los SPE y vacía la caché del recompilador) y se abre
 * otra vez la misma ROM, con los mismos ajustes y sin pasar por la lista. */
static enum game_end run_game(uint8_t *rom, uint32_t rom_size, const char *source, char *err, size_t err_size)
{
	for (;;) {
		enum game_end end = run_game_once(rom, rom_size, source, err, err_size);
		if (end != GAME_RESTART)
			return end;
		/* game_reset lo deja todo como al arrancar el programa: las banderas
		 * de las pruebas y la ranura de estados se conservan. */
		int st = selftest_on, gt = gfxtest_on, at = audiotest_on;
		unsigned slot = state_slot;
		salsa64_log("reinicio desde cero: %s (el juego se cerró y se abre otra vez)", source ? source : "ROM");
		game_reset();
		selftest_on = st;
		gfxtest_on = gt;
		audiotest_on = at;
		state_slot = slot;
	}
}

/* ---- Menú ------------------------------------------------------------------------- */

static const char *const rom_dirs[] = {
	SALSA64_DATA_DIR,
	"/dev_usb000/salsa64", "/dev_usb001/salsa64", "/dev_usb002/salsa64", "/dev_usb003/salsa64",
	"/dev_usb004/salsa64", "/dev_usb005/salsa64", "/dev_usb006/salsa64", "/dev_usb007/salsa64",
};
_Static_assert(sizeof rom_dirs / sizeof rom_dirs[0] == ROMLIST_SOURCES_MAX, "capacidad por cada ubicación de ROMs");

static bool video_load(struct menu_settings *set);
static void video_save(const struct menu_settings *set);

static struct romlist *roms;
static char rom_dir[ROMLIST_PATH_MAX];
static char opened_title[ROMLIST_NAME_MAX + 1], opened_code[5];
static uint32_t opened_crc1, opened_crc2;

/* Título y código de una ROM de la lista (NULL si no está: p. ej. sin menú). */
static void rom_names(const char *path, const char **title, const char **code, uint32_t *crc1, uint32_t *crc2)
{
	if (!roms) {
		*title = opened_title;
		*code = opened_code;
		*crc1 = opened_crc1;
		*crc2 = opened_crc2;
		return;
	}
	int i = romlist_find(roms, path, rom_dirs, ROMLIST_SOURCES_MAX);
	if (i < 0)
		return;
	*title = romlist_display_title(&roms->e[i]);
	*code = roms->e[i].code;
	*crc1 = roms->e[i].crc1;
	*crc2 = roms->e[i].crc2;
}
static struct menu menu;
static struct cheat_set menu_cheats;  /* los de la ROM cuya página está abierta */
static struct {
	float lw, lh;           /* espacio lógico del menú */
	struct menu_settings settings;
	char save_cfg[4096];
	size_t save_cfg_len;
	/* Prueba automática (menu-prueba). */
	char test[MENU_TEST_MAX][ROMLIST_PATH_MAX];
	unsigned ntest, next_test;
	uint32_t step, idle;
	int games, failures;
} mn;

static int menu_accessory_default(void)
{
	return mn.settings.accessory;
}

/* ROMs de prueba abiertas desde el menú: se verifican como las embebidas. */
static void test_flags_from_name(const char *name)
{
	if (!strncmp(name, "salsa64-vitest", 14))
		selftest_on = 1;
	else if (!strncmp(name, "salsa64-gfxtest", 15))
		gfxtest_on = 1;
	else if (!strncmp(name, "salsa64-audiotest", 17))
		audiotest_on = 1;
}

/* Arma la lista (con la fila de `keep` elegida, si sigue ahí). */
static unsigned scan_flags;  /* ROMLIST_SCAN_REUSE solo al volver de un juego */

/* Lee las carpetas y arma la lista: no toca el 2D ni el menú, así que puede
 * correr en el hilo de la carga previa. */
static void menu_scan_read(void)
{
	uint64_t scan_start = sysGetSystemTime();
	if (!roms)
		roms = romlist_create();
	if (!roms) {
		salsa64_log("menú: no hay memoria para la lista de ROMs");
		app_quit = 1;
		return;
	}
	FILE *f = fopen(SAVE_CFG, "rb");
	mn.save_cfg_len = f ? fread(mn.save_cfg, 1, sizeof mn.save_cfg - 1, f) : 0;
	if (f)
		fclose(f);
	romlist_cache_file(SALSA64_DATA_DIR "/romlist.cache");
	int n = romlist_scan_sources_ex(roms, rom_dirs, ROMLIST_SOURCES_MAX, rom_dir, mn.save_cfg, mn.save_cfg_len, scan_flags);
	if (n < 0 && rom_dir[0]) {
		rom_dir[0] = '\0';
		n = romlist_scan_sources_ex(roms, rom_dirs, ROMLIST_SOURCES_MAX, "", mn.save_cfg, mn.save_cfg_len, scan_flags);
	}
	scan_flags = 0;
	uint32_t c_hits, c_reads, c_fast;
	romlist_scan_stats(&c_hits, &c_reads, &c_fast);
	romlist_sort(roms, mn.settings.rom_order == MENU_ROM_ORDER_FILENAME ? ROMLIST_ORDER_FILENAME : ROMLIST_ORDER_TITLE);
	salsa64_log("menú: lectura del directorio en %llu us; lista dinámica %u bytes (%u filas)",
		    (unsigned long long)(sysGetSystemTime() - scan_start), (unsigned)romlist_memory_bytes(), roms->count);
	salsa64_log("menú: caché de cabeceras: %u de la caché, %u leídas del disco, %u carpetas sin releer", c_hits, c_reads,
		    c_fast);
	salsa64_log("menú: %u ROMs en %u carpetas, %u no legibles o fuera de capacidad", roms->rom_count,
	    roms->dir_count, roms->skipped);
	if (roms->skipped)
		salsa64_log("menú: %u archivos con extensión de ROM que no se pudieron leer, muy chicos o sin lugar",
			    roms->skipped);
}

/* El menú sobre la lista ya leída. */
static void menu_scan_finish(const char *keep)
{
	if (!roms)
		return;
	menu_init(&menu, roms, keep ? romlist_find(roms, keep, rom_dirs, ROMLIST_SOURCES_MAX) : -1, &mn.settings);
	menu.global_controls = padmap;
	if (roms->directory[0] && menu.sel == 0 && roms->count > 1 && roms->e[0].kind == ROMLIST_PARENT)
		menu.sel = 1, menu.pos = 1.0f;
	if (keep) {
		const char *name = strrchr(keep, '/');
		snprintf(menu.last, sizeof menu.last, "%s", name ? name + 1 : keep);
	}
	const rsxkit_screen *osc = rsxkit_screen_info();
	menu.out_w = osc->width;
	menu.out_h = osc->height;
	menu.res_run = video_cfg.resolution;
	menu.scale_run = video_cfg.scale;
	menu.dl_enabled = dl_enabled();  /* experimental: solo con descargas.cfg «activar = si» */
	menu.dirs[menu.ndirs++] = SALSA64_DATA_DIR;
	menu.dirs[menu.ndirs++] = "/dev_usb000/salsa64 … /dev_usb007/salsa64";
	menu.version = version_text;
	menu.build = build_text[0] ? build_text : NULL;
}

static void menu_scan(const char *keep)
{
	menu_scan_read();
	menu_scan_finish(keep);
}

static bool rom_root_for_path(const char *path, char *root, size_t size)
{
	for (unsigned i = 0; i < sizeof rom_dirs / sizeof rom_dirs[0]; i++) {
		size_t n = strlen(rom_dirs[i]);
		if (!strcmp(path, rom_dirs[i]) || (!strncmp(path, rom_dirs[i], n) && path[n] == '/')) {
			if (n >= size)
				return false;
			memcpy(root, rom_dirs[i], n + 1);
			return true;
		}
	}
	return false;
}

static bool rom_parent_directory(const char *path, char *parent, size_t size)
{
	size_t n = strlen(path);
	if (!n || n >= size)
		return false;
	memmove(parent, path, n + 1);
	char *slash = strrchr(parent, '/');
	if (!slash) {
		parent[0] = '\0';
		return true;
	}
	*slash = '\0';
	return true;
}

static bool rom_child_directory(char *directory, size_t size, const char *name)
{
	size_t used = strlen(directory), n = strlen(name);
	if (used + (used ? 1u : 0u) + n >= size)
		return false;
	if (used)
		directory[used++] = '/';
	memcpy(directory + used, name, n + 1);
	return true;
}

static const rsxkit_config *video_config_for(int resolution, rsxkit_config *cfg);

/* Espacio lógico del menú según la pantalla en curso. */
static void menu_layout(void)
{
	mn.lh = 720.0f;
	mn.lw = rsxkit_screen_info()->aspect > 1.5f ? 1280.0f : 960.0f;
}

/* «Aplicar ahora»: pone la salida y la escala elegidas sin cerrar Salsa64.
 * No hay juego abierto (el RDP quedó limpio en game_close). Se rehacen solo
 * los búferes de pantalla del RSX (rsxkit_video_reconfigure: el contexto, la
 * memoria mapeada, el 2D del menú y el framebuffer se conservan) y la escala
 * del RDP (salsa64_rdp_set_scale: conserva su memoria mapeada). Si la tele no
 * acepta el modo, rsxkit deja el anterior y la escala no se toca. */
static void menu_apply_video(void)
{
	const struct menu_settings want = menu.set;
	const rsxkit_screen *sc = rsxkit_screen_info();
	unsigned old_w = sc->width, old_h = sc->height;
	int rc = RSXKIT_OK;
	rsxkit_config vcfg;

	/* Solo se toca el video si cambió la resolución pedida. */
	if (want.resolution != video_cfg.resolution) {
		rsxkit_finish();
		rc = rsxkit_video_reconfigure(video_config_for(want.resolution, &vcfg));
	}
	if (rc != RSXKIT_OK) {
		salsa64_log("video: no se pudo cambiar a %s (%d); sigue %ux%u, escala %u×", want.resolution == MENU_RES_1080P ? "1080p" : "720p",
			    rc, old_w, old_h, salsa64_rdp_scale(NULL));
		snprintf(menu.note, sizeof menu.note, tr("No se pudo: sigue %ux%u"), old_w, old_h);
		return;
	}
	int requested_scale = menu_scale_limit(want.scale, want.resolution);
	salsa64_rdp_set_scale((uint32_t)requested_scale);
	rdp_scale_req = requested_scale;
	bool cut;
	uint32_t es = salsa64_rdp_scale(&cut);
	video_cfg.resolution = want.resolution;
	video_cfg.scale = want.scale;
	sc = rsxkit_screen_info();
	menu_layout();
	menu.out_w = sc->width;
	menu.out_h = sc->height;
	menu.res_run = want.resolution;
	menu.scale_run = want.scale;
	snprintf(menu.note, sizeof menu.note, tr("Aplicado: %ux%u, escala %u×"), sc->width, sc->height, es);
	salsa64_log("video: %ux%u, escala %u× (%s%s), aplicado sin reiniciar", sc->width, sc->height, es,
		    menu_scale_is_auto(want.scale) ? "máximo automático" : "pedida",
		    cut ? ", reducida por memoria del RSX" : "");
	if (want.resolution == MENU_RES_1080P && sc->height != 1080)
		salsa64_log("video: se pidió 1080p y la tele dio %ux%u", sc->width, sc->height);
}

/* Un cuadro del menú: todo lo dibuja el RSX (rsxkit/ui.h); el PPE solo
 * arma unos miles de vértices. Con `shot`, además una captura (a la mitad
 * de la resolución de 720p) en ese archivo. */
/* Carátula de la ROM elegida: cuando la selección lleva quieta un momento la
 * busca en COVERS_DIR, la decodifica y la sube a la ranura 0 de la UI (entre
 * cuadros, que es cuando rsxkit_ui_image_set puede esperar al RSX). */
#define COVER_SETTLE_US 200000u
static uint8_t cover_rgba[COVER_RGBA_BYTES];
static int cover_tried = -1;
static uint64_t cover_since;

/* La carátula completa de la fila elegida: con el hilo de thumbs.c, se pide y
 * se recoge cuando llega (el menú no se detiene); sin él, se decodifica aquí. */
static int cover_pending = -1;  /* la fila cuya carátula completa está pedida, o −1 */

static void menu_cover_upload(int s, const uint8_t *rgba, unsigned w, unsigned h, const char *what)
{
	int rc = rsxkit_ui_image_set(0, rgba, w, h);
	if (rc != RSXKIT_OK) {
		salsa64_log("carátula: %s: no se pudo subir (%d)", what, rc);
		return;
	}
	menu.cover_w = w;
	menu.cover_h = h;
	menu.cover_rom = s;
	salsa64_log("carátula: %s (%ux%u)", what, w, h);
}

static void menu_cover_update(void)
{
	if (thumbs_ready()) {
		int token = thumbs_big_ready(), sel = menu_selected(&menu);
		bool ok;
		unsigned w, h;
		/* Solo la de la fila elegida se copia: cover_rgba sigue siendo la de menu.cover_rom. */
		if (token >= 0 && thumbs_big_poll(token == sel ? cover_rgba : NULL, &token, &ok, &w, &h)) {
			if (token == cover_pending)
				cover_pending = -1;
			if (!ok)
				salsa64_log("carátula: ninguna para la fila %d", token);
			else if (token == sel)  /* si ya cambió de fila, no hace falta subirla */
				menu_cover_upload(token, cover_rgba, w, h, romlist_name(&roms->e[token]));
		}
	}
	if (!menu_settled(&menu)) {
		cover_since = 0;
		/* Se movió otra vez: la carátula completa pedida que no empezó se descarta. */
		if (cover_pending >= 0 && thumbs_big_cancel()) {
			cover_tried = -1;
			cover_pending = -1;
		}
		return;
	}
	uint64_t now = sysGetSystemTime();
	int s = menu_selected(&menu);
	if (!cover_since)
		cover_since = now;
	if (now - cover_since < COVER_SETTLE_US || s == menu.cover_rom || s == cover_tried)
		return;
	cover_tried = s;
	const struct rom_entry *e = &roms->e[s];
	if (thumbs_ready()) {
		cover_pending = s;
		thumbs_big_request(e, s);
		return;
	}
	char path[256];
	if (!cover_find(COVERS_DIR, romlist_name(e), romlist_display_title(e), e->code, path, sizeof path)) {
		salsa64_log("carátula: ninguna para %s (título «%s», código %s)", romlist_name(e), romlist_display_title(e),
			    e->code[0] ? e->code : "-");
		return;
	}
	unsigned w = 0, h = 0;
	enum cover_result r = cover_load_png(path, cover_rgba, &w, &h);
	if (r != COVER_OK) {
		salsa64_log("carátula: %s: %s", path, cover_result_name(r));
		return;
	}
	menu_cover_upload(s, cover_rgba, w, h, path);
}

/* Descargas (download.h): lo que se va viendo en el pie del menú. */
static bool dl_shown;

static void download_begin(int what)
{
	struct dl_rom *list = calloc(ROMLIST_MAX, sizeof *list);
	if (!list) {
		menu_set_status(&menu, "No hay memoria para preparar la descarga");
		return;
	}
	if (!roms->rom_count) {
		menu_set_status(&menu, "No encontré ROMs (.z64, .v64, .n64) en:");
		free(list);
		return;
	}
	unsigned n = 0;
	for (uint32_t i = 0; i < roms->count; i++) {
		const struct rom_entry *e = &roms->e[i];
		if (e->kind != ROMLIST_ROM)
			continue;
		snprintf(list[n].name, sizeof list[n].name, "%s", romlist_name(e));
		snprintf(list[n].title, sizeof list[n].title, "%s", romlist_display_title(e));
		snprintf(list[n].code, sizeof list[n].code, "%s", e->code);
		n++;
	}
	bool started = dl_start(what, list, n);
	free(list);
	if (!started) {
		menu_set_status(&menu, "No se pudo bajar el índice: %s", tr("hilo"));
		return;
	}
	dl_shown = true;
	menu_set_status(&menu, what ? "Descargando trucos…" : "Descargando carátulas…");
}

/* Una vez por cuadro del menú: avance, cancelación con ○ y resultado. */
static void download_poll(uint32_t buttons)
{
	if (!dl_shown)
		return;
	struct dl_status st;
	dl_get(&st);
	if (st.state == DL_RUNNING) {
		if (buttons & (1u << SALSA64_DS3_CIRCLE))
			dl_cancel();
		char line[MENU_STATUS_MAX];
		snprintf(line, sizeof line, tr("Descargando… %u de %u"), st.done, st.total);
		menu_set_status(&menu, "%s   %s", line, tr("○  Cancelar"));
		return;
	}
	dl_shown = false;
	dl_shutdown();
	if (st.state == DL_FAILED) {
		menu_set_status(&menu, "%s", st.err);
	} else if (st.state == DL_CANCELLED) {
		menu_set_status(&menu, "Descarga cancelada");
	} else {
		menu_set_status(&menu, "%s: %u nuevas, %u ya estaban, %u sin coincidencia", tr(st.what ? "Trucos" : "Carátulas"),
				st.got, st.have, st.missing);
		if (!st.what && st.got)
		{
			menu.cover_rom = cover_tried = -1;  /* a buscar las carátulas de nuevo */
			thumbs_forget_missing();
		}
	}
}

static void menu_show(const char *shot)
{
	frame_begin("menú: flip anterior");
	/* El fondo «Seda» va antes que el 2D del menú (docs/fondo-seda.md);
	 * si no carga, el menú dibuja las ondas. */
	bool seda = menu.set.waves && menu.set.seda;
	menu.bg_drawn = seda && seda_draw(menu.t);
	if (!seda)
		seda_release();
	menu.cart_icon = ui_cart_icon;
	thumbs_frame(!menu_settled(&menu));
	menu.thumb = thumbs_ready() ? thumbs_get : NULL;
	/* Cartucho 3D en la fila elegida; su etiqueta lleva la carátula cuando ya cargó. */
	menu.cart3d = cartucho3d_ready() ? cartucho3d_draw : NULL;
	if (menu.cart3d) {
		/* La etiqueta del cartucho: la carátula completa si ya llegó; si no, su miniatura. */
		int s = menu_selected(&menu);
		const uint8_t *src = NULL;
		unsigned w = 0, h = 0;
		if (s >= 0 && menu.cover_rom == s && menu.cover_w && menu.cover_h) {
			src = cover_rgba, w = menu.cover_w, h = menu.cover_h;
		} else if (s >= 0 && thumbs_ready() && (src = thumbs_pixels(&roms->e[s])) != NULL) {
			w = THUMB_W, h = THUMB_H;
		}
		cartucho3d_update(s, src, w, h, menu.t);
	}
	rsxkit_ui_begin(mn.lw, mn.lh);
	menu_frame(&menu, mn.lw, mn.lh, sysGetSystemTime());
	uint8_t intro_fade = intro_menu_fade();  /* del negro de la secuencia de arranque al menú */
	if (intro_fade)
		rsxkit_ui_rect(0.0f, 0.0f, mn.lw, mn.lh, intro_fade);
	rsxkit_ui_end();
	if (menu.cart3d) {
		/* Costo del cartucho 3D: cada 300 cuadros con él dibujado, el promedio. */
		static unsigned cart_n;
		static uint64_t cart_us;
		double us;
		unsigned n;
		cartucho3d_stats(&us, &n);
		cart_n += n;
		cart_us += (uint64_t)(us * n);
		if (cart_n >= 300) {
			salsa64_log("menú: cartucho 3D, %.0f us por cuadro en el PPE (vértices y envío, %u cuadros)",
				    (double)cart_us / cart_n, cart_n);
			cart_n = 0;
			cart_us = 0;
		}
	}
	if (menu.quit_ask)
		osd_confirm_draw("¿Salir al XMB?", "×: Sí     ○: No", mn.lw, mn.lh);
	if (shot) {
		const rsxkit_screen *sc = rsxkit_screen_info();
		struct salsa64_vi_frame f;
		memset(&f, 0, sizeof f);
		f.width = (uint32_t)(mn.lw / 2);
		f.height = 360;
		rsxkit_rect r = { 0, 0, sc->width, sc->height };
		rsxkit_finish();
		salsa64_screenshot(shot, &f, &r);
		salsa64_log("menú: captura %s", shot);
	}
	frame_end("menú: flip");
}

static void settings_read(struct menu_settings *s)
{
	char text[1024];
	menu_settings_default(s);
	FILE *f = fopen(MENU_CFG_PATH, "r");
	if (!f)
		return;
	size_t n = fread(text, 1, sizeof text - 1, f);
	fclose(f);
	menu_settings_parse(s, text, n);
}

static bool settings_write(const struct menu_settings *s)
{
	char text[1024];
	int n = menu_settings_format(s, text, sizeof text);
	FILE *f = fopen(MENU_CFG_PATH, "w");
	bool ok = f && fwrite(text, 1, (size_t)n, f) == (size_t)n;
	if (f && fclose(f) != 0)
		ok = false;
	return ok;
}

/* Idioma de la consola (para «Automático»): se lee una vez y se anota en el log. */
static void language_apply(void)
{
	s32 sys = -1;
	if (sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_LANG, &sys) != 0)
		sys = -1;
	i18n_set_system(sys);
	font_init_once();
	i18n_set(mn.settings.lang);
	font_select(i18n_lang());
	i18n_set(mn.settings.lang);  /* sin la fuente del sistema, el japonés, el coreano y el chino dan inglés */
	int want = i18n_from_system_cjk(sys);  /* el de la consola, con fuente o sin ella */
	salsa64_log("idioma: la consola dice %d (%s), ajuste «%s», en uso «%s»%s", sys, i18n_system_code(sys),
		    i18n_code(mn.settings.lang), i18n_code(i18n_lang()),
		    !i18n_cjk() && (i18n_is_cjk(want) || i18n_is_cjk(mn.settings.lang)) ? " (sin fuente CJK: inglés)" : "");
}

static void menu_settings_save(void)
{
	bool ok = settings_write(&mn.settings);
	salsa64_log("menú: fondo %s, FPS %s, confirmar al salir %s, filtro %s%s%s",
		    !mn.settings.waves ? "fijo" : mn.settings.seda ? "seda" : "ondas",
		    mn.settings.fps ? "sí" : "no", mn.settings.confirm ? "sí" : "no", menu_filter_name(mn.settings.filter),
		    menu_filter_has_density(mn.settings.filter) ? (mn.settings.density == MENU_DENSITY_FINE ? ", densidad fina" : ", densidad normal") : "",
		    ok ? "" : " (no se pudo guardar)");
}

/* El contador de velocidad se recuerda en menu.cfg («fps = si|no»), también
 * con embedded (sin menú): se relee el archivo y se cambia solo esa clave. */
static bool fps_setting_load(void)
{
	struct menu_settings s;
	settings_read(&s);
	return s.fps;
}

/* «Confirmar al salir» (menu.cfg, «confirmar = si|no»; sí sin el archivo),
 * también con embedded. */
static bool confirm_setting_load(void)
{
	struct menu_settings s;
	settings_read(&s);
	return s.confirm;
}

static void fps_setting_save(bool on)
{
	struct menu_settings s;
	settings_read(&s);
	s.fps = on;
	mn.settings.fps = on;
	salsa64_log("FPS: %s%s", on ? "sí" : "no", settings_write(&s) ? "" : " (no se pudo guardar en menu.cfg)");
}

/* Fila a la que va la prueba automática: la próxima ROM de menu-prueba
 * (por nombre de archivo) o «Salir al XMB»; -1 si falta. */
static int menu_test_target(void)
{
	if (mn.next_test >= mn.ntest)
		return menu_row_exit(&menu);
	for (uint32_t i = 0; i < roms->count; i++)
		if (!strcmp(romlist_name(&roms->e[i]), mn.test[mn.next_test]))
			return (int)i;
	return -1;
}

/* Línea «@video 720|1080 escala» de menu-prueba: «Aplicar ahora» con esos
 * valores, sin tocar video.cfg. Cuenta como un paso del conjunto. */
static void menu_test_video(const char *line)
{
	int res = 0, scale = 0;
	bool ok = sscanf(line, "@video %d %d", &res, &scale) == 2 && (res == 720 || res == 1080) && scale >= 0 &&
		  scale <= 5;
	if (ok) {
		menu.set.resolution = res == 1080 ? MENU_RES_1080P : MENU_RES_720P;
		menu.set.scale = scale;
		menu_apply_video();
		ok = !menu_video_pending(&menu) && (res == 1080 || rsxkit_screen_info()->height == 720) &&
		     (!scale || salsa64_rdp_scale(NULL) <= (uint32_t)scale);
	}
	salsa64_log("menú, prueba automática: «%s»: %s", line, ok ? "bien" : "MAL");
	mn.games++;
	mn.failures += !ok;
}

static void menu_test_load(void)
{
	FILE *f = fopen(MENU_TEST_PATH, "r");
	if (f) {
		char line[ROMLIST_PATH_MAX];
		while (mn.ntest < MENU_TEST_MAX && fgets(line, sizeof line, f)) {
			line[strcspn(line, "\r\n")] = '\0';
			if (line[0])
				memcpy(mn.test[mn.ntest++], line, sizeof line);
		}
		fclose(f);
	}
	salsa64_log("menú, prueba automática: %u ROMs que abrir (%s)", mn.ntest, MENU_TEST_PATH);
}

/* Suelta lo que el menú aún guarda y no necesita un juego: el hilo de miniaturas quieto y sin búfer y el
 * índice de las carátulas. true la primera vez que suelta algo (una segunda no libera más). */
static bool mem_released;  /* ya se soltó para el juego que se está abriendo */

static bool menu_release_memory(void)
{
	if (mem_released)
		return false;
	mem_released = true;
	if (thumbs_pause())  /* con el hilo ocupado, su índice se queda */
		cover_index_free();
	return true;
}

/* Abre la ROM `path` (de la lista del menú o del arranque directo) y vuelve
 * cuando el juego termina. */
static enum game_end menu_open_path(const char *path, const struct rom_entry *e)
{
	char err[160], filename[ROMLIST_NAME_MAX + 1], title[ROMLIST_NAME_MAX + 1];
	snprintf(filename, sizeof filename, "%s", romlist_name(e));
	snprintf(title, sizeof title, "%s", romlist_display_title(e));
	snprintf(opened_title, sizeof opened_title, "%s", title);
	snprintf(opened_code, sizeof opened_code, "%s", e->code);
	opened_crc1 = e->crc1;
	opened_crc2 = e->crc2;
	mem_released = false;
	salsa64_log("menú: abro %s (\"%s\")", path, title);
	font_stats_log("al abrir el juego");
	heap_report("al abrir");
	menu.loading = title;
	menu_show(NULL);
	menu.loading = NULL;
	seda_release();  /* la memoria del fondo «Seda» y su SPE, para el juego */
	/* El hilo de miniaturas, quieto y sin sus búferes, antes de medir y de pedir la ROM. */
	thumbs_pause();
	heap_report("con el menú quieto");
	romlist_free(roms);
	roms = NULL;
	romlist_cache_release();
	menu.list = NULL;
	uint32_t size = 0;
	/* Búfer a la medida de la ROM, solo mientras dura el juego. */
	beat.pres_at = "menú: leyendo la ROM";
	uint8_t *rom = read_rom_file(path, NULL, 0, &size);
	beat.pres_at = "arranque del juego";
	if (!rom) {
		/* Una vez más, con todo lo del menú suelto. */
		salsa64_log("menú: %s %s: reintento tras soltar lo del menú",
			    rom_no_memory ? "memoria insuficiente para abrir" : "no se pudo leer", path);
		heap_report("antes del reintento");
		menu_release_memory();
		rom = read_rom_file(path, NULL, 0, &size);
	}
	if (!rom) {
		salsa64_log("menú: %s %s", rom_no_memory ? "memoria insuficiente para abrir" : "no se pudo leer", path);
		heap_report(rom_no_memory ? "tras faltar memoria para la ROM" : "tras fallar la lectura");
		menu_set_status(&menu, rom_no_memory ? "Memoria insuficiente para abrir %s" : "No se pudo leer %s", title);
		mn.games++;
		mn.failures++;
		menu_scan(path);
		return GAME_TO_MENU;
	}
	heap_report("con la ROM leída");
	if (!romlist_save_last(LAST_ROM_PATH, path))
		salsa64_log("menú: no se pudo escribir %s", LAST_ROM_PATH);
	struct salsa64_padmap global_map = padmap;
	struct menu_game game_config;
	game_cfg_read(filename, &game_config);
	padmap = global_map;
	salsa64_padmap_overlay(&padmap, &game_config.controls);
	game_reset();
	test_flags_from_name(filename);
	enum game_end end = run_game(rom, size, path, err, sizeof err);
	padmap = global_map;
	rom_free(rom);  /* run_game ya cerró el juego: nada apunta a la ROM */
	heap_report("tras soltar la ROM");
	bool test = selftest_on || gfxtest_on || audiotest_on;
	bool ok = end != GAME_FAILED && (test ? pres.result == SELFTEST_OK : !pres.autotest || game_auto_ok);
	mn.games++;
	mn.failures += !ok;
	salsa64_log("menú: de vuelta de %s%s", path,
		    end == GAME_FAILED ? " (no arrancó)" : !pres.autotest ? "" : ok ? " (prueba bien)" : " (prueba MAL)");
	if (end != GAME_QUIT) {
		scan_flags = ROMLIST_SCAN_REUSE;  /* sin releer las carpetas que no cambiaron (L3 las relee) */
		menu_scan(path);  /* de nuevo: pudo entrar o salir una memoria USB */
		if (err[0])
			menu_set_status(&menu, "%s", err);
	}
	return end;
}

/* Abre la ROM elegida y vuelve cuando el juego termina. */
static enum game_end menu_open(const struct rom_entry *e)
{
	char path[ROMLIST_PATH_MAX];
	if (!romlist_build_path(roms, e, rom_dirs, ROMLIST_SOURCES_MAX, path, sizeof path)) {
		menu_set_status(&menu, "La ruta de la ROM es demasiado larga");
		return GAME_TO_MENU;
	}
	return menu_open_path(path, e);
}

/* Arranque directo (issue #25, docs/uso-ps3.md): la ROM que pide argv[1] o,
 * si no, la primera línea de /dev_hdd0/game/<APPID>/rom.txt (que se borra al
 * leerla). main() la deja en direct_rom; menu_loop la abre sin esperar a
 * ninguna tecla, y si no se puede, el menú sale normal con un aviso. */
#define DIRECT_ROM_MAX 250
static char direct_rom[DIRECT_ROM_MAX + 1];
static const char *direct_from = "";

static void direct_request(int argc, char **argv)
{
	direct_rom[0] = '\0';
	if (argc > 1 && argv && argv[1] && argv[1][0]) {
		snprintf(direct_rom, sizeof direct_rom, "%s", argv[1]);
		direct_from = "argv";
	} else {
		char txt[96];
		snprintf(txt, sizeof txt, "/dev_hdd0/game/%s/rom.txt", salsa64_appid);
		FILE *f = fopen(txt, "rb");
		if (!f)
			return;
		char raw[DIRECT_ROM_MAX + 8];
		size_t n = fread(raw, 1, sizeof raw - 1, f);
		fclose(f);
		if (unlink(txt) != 0)
			salsa64_log("arranque directo: no se pudo borrar %s", txt);
		raw[n] = '\0';
		const char *p = raw;
		if (!memcmp(p, "\xEF\xBB\xBF", 3))  /* BOM de UTF-8 */
			p += 3;
		size_t len = strcspn(p, "\r\n");
		if (len > DIRECT_ROM_MAX)
			len = DIRECT_ROM_MAX;
		while (len && (p[len - 1] == ' ' || p[len - 1] == '\t'))
			len--;
		memcpy(direct_rom, p, len);
		direct_rom[len] = '\0';
		direct_from = "rom.txt";
		if (!len) {
			salsa64_log("arranque directo: %s vacío, se ignora", txt);
			return;
		}
	}
	salsa64_log("arranque directo: %s (desde %s)", direct_rom, direct_from);
}

/* Devuelve true si la app debe terminar (GAME_QUIT). Si la ROM no sirve, deja
 * el aviso en el estado del menú. */
static bool direct_launch(void)
{
	if (!direct_rom[0])
		return false;
	const char *why = NULL;
	struct romlist *one = NULL;
	if (direct_rom[0] != '/')
		why = "la ruta debe ser absoluta";
	else if (!romlist_has_rom_ext(direct_rom))
		why = "extensión no admitida (.z64, .n64, .v64, .zip)";
	else if (!(one = romlist_create()))
		why = "sin memoria";
	else if (!romlist_add_file(one, direct_rom, 0, mn.save_cfg, mn.save_cfg_len) || !one->count)
		why = "no existe o no es una ROM válida";
	if (why) {
		salsa64_log("arranque directo: no se pudo abrir %s (%s)", direct_rom, why);
		menu_set_status(&menu, "No se pudo abrir %s", direct_rom);
		if (one)
			romlist_free(one);
		return false;
	}
	/* Que al volver el menú caiga en la carpeta de la ROM, si es de las conocidas. */
	char root[ROMLIST_PATH_MAX];
	if (rom_root_for_path(direct_rom, root, sizeof root)) {
		const char *rel = direct_rom + strlen(root);
		if (*rel == '/')
			rel++;
		snprintf(rom_dir, sizeof rom_dir, "%s", rel);
		rom_parent_directory(rom_dir, rom_dir, sizeof rom_dir);
	}
	char path[DIRECT_ROM_MAX + 1];
	snprintf(path, sizeof path, "%s", direct_rom);
	enum game_end end = menu_open_path(path, &one->e[0]);
	romlist_free(one);
	return end == GAME_QUIT || app_quit;
}

/* ---- Carga previa y secuencia de arranque ----------------------------------
 * Lo lento del arranque que no necesita el RSX (los ajustes, abrir la fuente
 * del sistema, ~0,9 s, y leer la lista de ROMs) va en un hilo mientras el
 * principal anima la secuencia de arranque (intro.h). Elegido sobre
 * intercalar cuadros entre los pasos porque la fuente es una sola llamada
 * larga a libfont que no se puede partir: sin hilo, la animación se
 * congelaría casi un segundo. El hilo no toca el RSX ni el 2D (los módulos
 * del sistema y los archivos van bien desde cualquier hilo); lo que sí los
 * toca (poner la fuente en el 2D, el menú) sigue en el principal, después
 * de que el hilo termine (preload_finish). Sin secuencia (sin-intro, prueba
 * automática, un cuadro de embedded), la misma carga corre en el principal,
 * en el mismo orden de siempre. */
#define INTRO_OFF_PATH  SALSA64_DATA_DIR "/sin-intro"
#define INTRO_TEST_PATH SALSA64_DATA_DIR "/intro-prueba"
#define INTRO_WAIT_PATH SALSA64_DATA_DIR "/intro-espera"  /* ms de negro tras videoConfigure antes del encendido */
#define INTRO_WAIT_DEFAULT_MS 0  /* con videoConfigure con los valores del XMB, el ProArt ve la intro entera sin espera; intro-espera sirve a una tele lenta */
#define PRELOAD_PRIO   1500
#define PRELOAD_STACK  0x100000

static struct {
	sys_ppu_thread_t tid;
	bool threaded;        /* hay hilo (y falta unirlo) */
	bool ran;             /* ya se cargó */
	volatile int done;    /* el hilo terminó */
	uint64_t t_start, t_done;
	bool have_last;
	char last[ROMLIST_PATH_MAX];
} pre;

static void preload_run(void)
{
	if (pre.ran)
		return;
	pre.ran = true;
	pre.t_start = sysGetSystemTime();
	settings_read(&mn.settings);
	font_open_raw();
	pre.have_last = romlist_load_last(LAST_ROM_PATH, pre.last, sizeof pre.last);
	if (pre.have_last) {
		char root[ROMLIST_PATH_MAX];
		if (rom_root_for_path(pre.last, root, sizeof root)) {
			const char *rel = pre.last + strlen(root);
			if (*rel == '/')
				rel++;
			snprintf(rom_dir, sizeof rom_dir, "%s", rel);
			rom_parent_directory(rom_dir, rom_dir, sizeof rom_dir);
		}
	}
	menu_scan_read();
	heap_report("al abrir el menú");
	pre.t_done = sysGetSystemTime();
}

static void preload_thread(void *arg)
{
	(void)arg;
	preload_run();
	__sync_synchronize();
	pre.done = 1;
	sysThreadExit(0);
}

/* Arranca la carga en su hilo (con el 2D listo: la fuente lo comprueba). */
static void preload_start(void)
{
	if (pre.ran || pre.threaded)
		return;
	if (sysThreadCreate(&pre.tid, preload_thread, NULL, PRELOAD_PRIO, PRELOAD_STACK, THREAD_JOINABLE,
			    (char *)"salsa64_carga") == 0) {
		pre.threaded = true;
		return;
	}
	salsa64_log("intro: no se pudo crear el hilo de la carga: sin secuencia de arranque");
}

/* Deja la carga hecha (esperando al hilo o haciéndola aquí). */
static void preload_finish(void)
{
	if (pre.threaded) {
		u64 rv;
		sysThreadJoin(pre.tid, &rv);
		pre.threaded = false;
	}
	preload_run();
}

/* La secuencia de arranque, si toca: hasta que acabe y la carga también.
 * X u O la saltan (los botones que ya estaban apretados al abrir la
 * aplicación desde el XMB no cuentan). */
static bool intro_was_shown;

/* Cuatro píxeles del búfer trasero (antes del flip) para el log: lo que
 * se presenta de verdad, en un momento de la secuencia. */
static void intro_probe(const char *what)
{
	const rsxkit_screen *sc = rsxkit_screen_info();
	uint32_t pitch;
	rsxkit_finish();
	const volatile uint32_t *px = rsxkit_back_buffer(&pitch);
	uint32_t w = pitch / 4, sum = 0, n = 0;
	for (uint32_t y = sc->height / 8; y < sc->height; y += sc->height / 8)
		for (uint32_t x = sc->width / 4; x < sc->width - sc->width / 4; x += sc->width / 8, n++)
			sum += (px[y * w + x] >> 8 & 0xFF) + (px[y * w + x] >> 16 & 0xFF) + (px[y * w + x] & 0xFF);
	salsa64_log("intro: píxeles de «%s»: centro %08x, (%u,%u) %08x, luz media de la rejilla %u/255", what,
		    px[sc->height / 2 * w + sc->width / 2], sc->width / 2, sc->height / 8, px[sc->height / 8 * w + sc->width / 2],
		    n ? sum / (3 * n) : 0);
}

/* Negro hasta `ms` después del videoConfigure de rsxkit_init: el monitor
 * puede quedarse sin imagen unos segundos al renegociar el HDMI. Sale al
 * saltarla (true) o al cerrar la aplicación. */
static bool intro_wait_black(void)
{
	const rsxkit_video_boot *vb = rsxkit_video_boot_info();
	int ms = INTRO_WAIT_DEFAULT_MS;
	FILE *f = fopen(INTRO_WAIT_PATH, "r");
	if (f) {
		int v;
		if (fscanf(f, "%d", &v) == 1 && v >= 0 && v <= 20000)
			ms = v;
		fclose(f);
	}
	uint64_t until = (vb->configured_us ? vb->configured_us : (uint64_t)sysGetSystemTime()) + (uint64_t)ms * 1000u;
	uint64_t t0 = sysGetSystemTime();
	uint32_t held = 0xFFFFFFFFu;
	bool first = true, skip = false;
	while (!app_quit && (uint64_t)sysGetSystemTime() < until && !skip) {
		beat.pres++;
		beat.pres_at = "intro: espera";
		sysutil_check();
		uint32_t buttons = 0;
		update_pads(&buttons);
		if (first)
			held = buttons;
		first = false;
		if (buttons & ~held & ((1u << SALSA64_DS3_CROSS) | (1u << SALSA64_DS3_CIRCLE)))
			skip = true;
		held = buttons;
		if (pausectl_overlay(&pause_ctl) && !rsxkit_frame_ready()) {
			sysUsleep(OVERLAY_US);
			continue;
		}
		frame_begin("intro: espera, flip anterior");
		frame_end("intro: espera, flip");
	}
	salsa64_log("intro: espera en negro de %d ms tras el video (videoConfigure con %s): esperó %llu ms%s", ms,
		    vb->used_current ? "los valores del XMB" : "los de siempre", (unsigned long long)((sysGetSystemTime() - t0) / 1000),
		    skip ? ", saltada" : "");
	return skip;
}

static void intro_run(void)
{
	if (!pre.threaded || !ui_ok)
		return;
	if (!intro_begin(cover_rgba, sizeof cover_rgba)) {
		salsa64_log("intro: no se pudo preparar, sin secuencia de arranque");
		return;
	}
	intro_was_shown = true;
	bool skip = intro_wait_black();
	uint32_t held = 0xFFFFFFFFu;
	bool first = true;
	unsigned shown = 0;
	bool probed_static = false, probed_logo = false;
	while (!app_quit) {
		beat.pres++;
		beat.pres_at = "intro: sysutil";
		sysutil_check();
		uint32_t buttons = 0;
		update_pads(&buttons);
		if (first)
			held = buttons;
		uint32_t pressed = buttons & ~held;
		held = buttons;
		if (pressed & ((1u << SALSA64_DS3_CROSS) | (1u << SALSA64_DS3_CIRCLE)))
			skip = true;
		if (pausectl_overlay(&pause_ctl) && !rsxkit_frame_ready()) {
			sysUsleep(OVERLAY_US);
			continue;
		}
		enum intro_phase cur = intro_update(pre.done != 0, skip);
		if (cur == INTRO_DONE)
			break;
		frame_begin("intro: flip anterior");
		bool ok = intro_draw();
		/* Sondeo de lo que va a la pantalla: encendido, estática y logo. */
		if (++shown == 20 && cur == INTRO_POWER)
			intro_probe("encendido");
		if (cur == INTRO_STATIC && !probed_static && (probed_static = true))
			intro_probe("estática");
		if (cur == INTRO_LOGO && !probed_logo && (probed_logo = true))
			intro_probe("logo");
		frame_end("intro: flip");
		first = false;
		if (!ok)
			break;
	}
	if (skip || app_quit) {
		/* Un cuadro negro y a esperar lo que falte de la carga. */
		frame_begin("intro: negro");
		frame_end("intro: negro, flip");
	}
	preload_finish();
	intro_end((uint32_t)((pre.t_done - pre.t_start) / 1000));
}

/* El menú: lista, elige, abre y vuelve, hasta «Salir al XMB» o «Salir del
 * juego» del sistema. Devuelve 0, o 1 si la prueba automática falló. */
static int menu_loop(void)
{
	const rsxkit_screen *sc = rsxkit_screen_info();
	int rc = ui_init_once();
	if (rc != RSXKIT_OK) {
		salsa64_log("%s: FALLO al preparar el menú (2D del RSX %d)", pres.autotest ? "RESULTADO frontend" : "menú",
			    rc);
		return 1;
	}
	menu_layout();
	intro_run();
	preload_finish();
	language_apply();  /* los ajustes y la fuente ya los dejó la carga previa */
	ra_apply(mn.settings.ra, mn.settings.ra_hardcore);
	load_overscan();
	mn.settings.overscan = overscan_preset();
	{
		struct menu_settings v = mn.settings;
		video_load(&v);
		mn.settings.resolution = v.resolution;
		mn.settings.scale = v.scale;
	}
	salsa64_log("menú: espacio lógico de %.0fx%.0f sobre %ux%u, ROM con búfer a su medida (máx. %u MB), fondo %s", mn.lw,
		    mn.lh, sc->width, sc->height, ROM_MAX >> 20,
		    !mn.settings.waves ? "fijo" : mn.settings.seda ? "seda" : "ondas");

	const char *last = pre.last;
	bool have_last = pre.have_last;
	menu_scan_finish(have_last ? last : NULL);
	if (!roms)
		return 1;
	salsa64_log("menú: listo a los %llu ms de empezar la carga previa (carga en %llu ms, %s)",
		    (unsigned long long)((sysGetSystemTime() - pre.t_start) / 1000),
		    (unsigned long long)((pre.t_done - pre.t_start) / 1000), intro_was_shown ? "con la secuencia de arranque" : "sin ella");
	if (have_last)
		salsa64_log("menú: última ROM %s (%s)", last, romlist_find(roms, last, rom_dirs, ROMLIST_SOURCES_MAX) >= 0 ? "elegida" : "ya no está");
	if (pres.autotest)
		menu_test_load();

	/* Captura del menú (prueba automática o SALSA64_DATA_DIR/capturas): una
	 * vez, cuando terminó de aparecer. */
	bool want_shot = pres.autotest || file_exists(SHOTS_PATH);
	uint32_t frames = 0;
	int failed = 0;
	bool direct_done = !pres.autotest && direct_launch();
	while (!app_quit && !direct_done) {
		beat.pres++;
		beat.pres_at = "menú: sysutil";
		sysutil_check();
		beat.pres_at = "menú: mandos";
		uint32_t buttons;
		update_pads(&buttons);
		/* El stick izquierdo también mueve el menú, salvo mientras se
		 * captura un botón para el mapeo. */
		if (!menu.control_capture)
			buttons |= pad_stick_dirs;
		beat.pres_at = "menú";
		/* Con el sistema encima el menú sigue presentándose (el sistema
		 * dibuja el suyo en cada flip de la aplicación: sin flips se queda
		 * en el reloj de espera), pero solo si el flip anterior ya salió,
		 * para no dejar de atender sus eventos. */
		if (pausectl_overlay(&pause_ctl) && !rsxkit_frame_ready()) {
			beat.pres_at = "menú: sistema encima";
			sysUsleep(OVERLAY_US);
			continue;
		}
		if (pres.autotest && mn.next_test < mn.ntest && mn.test[mn.next_test][0] == '@') {
			menu_test_video(mn.test[mn.next_test++]);
			continue;
		}
		if (pres.autotest) {
			int target = menu_test_target();
			if (target < 0) {
				salsa64_log("menú, prueba automática: FALLO, no está %s", mn.test[mn.next_test]);
				failed = 1;
				break;
			}
			if (++mn.idle > MENU_IDLE_FIELDS) {
				salsa64_log("menú, prueba automática: FALLO, el menú no avanzó en %u cuadros", MENU_IDLE_FIELDS);
				failed = 1;
				break;
			}
			/* Primero deja que el menú aparezca entero (para la captura). */
			buttons = frames < 50 ? 0 : menu_autopilot(&menu, target, mn.step++);
		}
		ra_pump(false);
		menu.ra_user = ra_user();
		menu.ra_busy = ra_logging_in();
		for (char note[160]; ra_poll_notice(note, sizeof note);)
			menu_set_status(&menu, "%s", note);
		enum menu_action a = menu_input(&menu, buttons, sysGetSystemTime());
		if (a == MENU_RA_LOGIN)
			ra_login_begin();
		if (a == MENU_RA_LOGOUT)
			ra_logout();
		if (a == MENU_REFRESH) {
			char selected[ROMLIST_PATH_MAX] = "";
			int selected_rom = menu_selected(&menu);
			if (selected_rom >= 0)
				romlist_build_path(roms, &roms->e[selected_rom], rom_dirs, ROMLIST_SOURCES_MAX,
						   selected, sizeof selected);
			menu_scan(selected[0] ? selected : NULL);
			if (roms)
				menu_set_status(&menu, "%u ROMs encontradas", roms->rom_count);
		}
		if (a == MENU_SETTINGS) {
			char selected[ROMLIST_PATH_MAX] = "";
			bool reorder = menu.set.rom_order != mn.settings.rom_order;
			if (reorder && menu.sel >= 0 && menu.sel < (int)roms->count && roms->e[menu.sel].kind == ROMLIST_ROM)
				romlist_build_path(roms, &roms->e[menu.sel], rom_dirs, ROMLIST_SOURCES_MAX, selected, sizeof selected);
			if (menu.set.overscan != mn.settings.overscan)
				overscan_save(menu.set.overscan);
			if (menu.set.resolution != mn.settings.resolution || menu.set.scale != mn.settings.scale)
				video_save(&menu.set);
			/* Todo lo de menu.cfg (antes, la densidad no estaba en la
			 * lista y se perdía al abrir el juego). */
			bool cfg = menu_settings_cfg_differs(&menu.set, &mn.settings);
			mn.settings = menu.set;
			ra_apply(mn.settings.ra, mn.settings.ra_hardcore);
			if (reorder) {
				romlist_sort(roms, mn.settings.rom_order == MENU_ROM_ORDER_FILENAME ? ROMLIST_ORDER_FILENAME
										 : ROMLIST_ORDER_TITLE);
				int at = selected[0] ? romlist_find(roms, selected, rom_dirs, ROMLIST_SOURCES_MAX) : -1;
				if (at >= 0)
					menu.sel = at, menu.pos = (float)at;
			}
			if (cfg) {
				language_apply();
				menu_settings_save();
			}
		}
		if (a == MENU_APPLY)
			menu_apply_video();
		if (a == MENU_CONTROLS) {
			bool for_game = menu.page == MENU_PAGE_GAME;
			if (!for_game && menu.game_rom >= 0 && menu.game_rom < (int)roms->count) {
				const struct rom_entry *e = &roms->e[menu.game_rom];
				game_cfg_read(romlist_name(e), &menu.game);
			}
			menu_controls_open(&menu, for_game);
		}
		if (a == MENU_CONTROLS_CHANGED) {
			bool ok;
			if (menu.control_for_game) {
				const struct rom_entry *e = menu.game_rom >= 0 && menu.game_rom < (int)roms->count ? &roms->e[menu.game_rom] : NULL;
				ok = e && game_cfg_write(romlist_name(e), &menu.game);
			} else {
				padmap = menu.global_controls;
				ok = save_padmap(&menu.global_controls);
			}
			if (!ok) menu_set_status(&menu, "No se pudieron guardar los controles");
		}
		if (a == MENU_CHEATS) {
			const struct rom_entry *e = &roms->e[menu_selected(&menu)];
			if (cheats_load(&menu_cheats, CHEATS_DIR, romlist_name(e), romlist_display_title(e), e->code, e->crc1, e->crc2)) {
				menu_cheats_open(&menu, &menu_cheats, romlist_display_title(e));
				if (menu_cheats.crc_mismatch)
					menu_set_status(&menu, "Ojo: el CRC de la ROM no es el de esos trucos");
			} else {
				const char *f = strrchr(menu_cheats.cht_path, '/');
				menu_set_status(&menu, "No hay trucos: pon cheats/%s", f ? f + 1 : menu_cheats.cht_path);
				salsa64_log("trucos: ninguno para %s (%s)", romlist_name(e), menu_cheats.cht_path);
			}
		}
		if (a == MENU_GAME) {
			const struct rom_entry *e = &roms->e[menu_selected(&menu)];
			struct menu_game g;
			game_cfg_read(romlist_name(e), &g);
			menu_game_open(&menu, &g, romlist_display_title(e));
		}
		if (a == MENU_GAME_CHANGED) {
			const struct rom_entry *e = &roms->e[menu.game_rom];
			bool ok = game_cfg_write(romlist_name(e), &menu.game);
			salsa64_log("ajustes de %s: aspecto %d, filtro %d, densidad %d, recorte %d, escala %d%s", romlist_name(e),
				    menu.game.aspect, menu.game.filter, menu.game.density, menu.game.overscan, menu.game.scale,
				    ok ? "" : " (no se pudo guardar)");
			if (!ok)
				menu_set_status(&menu, "No se pudieron guardar los ajustes");
		}
		if (a == MENU_CHEATS_CHANGED) {
			bool ok = cheats_save(&menu_cheats);
			salsa64_log("trucos: %u activos de %u%s", cheats_active(&menu_cheats), menu_cheats.n,
				    ok ? "" : " (no se pudo guardar)");
			if (!ok)
				menu_set_status(&menu, "No se pudieron guardar los trucos");
		}
		if (a == MENU_DOWNLOAD)
			download_begin(menu.dl_what);
		if (a == MENU_DIRECTORY) {
			const struct rom_entry *e = menu.sel >= 0 && menu.sel < (int)roms->count ? &roms->e[menu.sel] : NULL;
			if (e && (e->kind == ROMLIST_DIRECTORY || e->kind == ROMLIST_PARENT)) {
				bool going_up = e->kind == ROMLIST_PARENT;
				char returning[ROMLIST_NAME_MAX + 1] = "";
				if (going_up) {
					const char *slash = strrchr(rom_dir, '/');
					snprintf(returning, sizeof returning, "%s", slash ? slash + 1 : rom_dir);
				}
				bool ok;
				if (going_up)
					ok = rom_parent_directory(rom_dir, rom_dir, sizeof rom_dir);
				else
					ok = rom_child_directory(rom_dir, sizeof rom_dir, romlist_name(e));
				if (ok) {
					/* La última ROM es solo una preferencia al entrar al menú;
					 * al recorrer carpetas se conserva la posición del explorador. */
					menu_scan(NULL);
					if (going_up) {
						int at = menu_find_directory(roms, returning);
						if (at >= 0)
							menu.sel = at, menu.pos = (float)at;
					}
				} else
					menu_set_status(&menu, "La ruta de la carpeta es demasiado larga");
			}
		}
		download_poll(buttons);
		if (a == MENU_QUIT) {
			salsa64_log("menú: «Salir al XMB»");
			break;
		}
		if (a == MENU_OPEN) {
			dl_shutdown();
			dl_shown = false;
			if (menu_open(&roms->e[menu_selected(&menu)]) == GAME_QUIT)
				break;
			mn.next_test++;
			mn.step = mn.idle = 0;
			menu.cover_rom = cover_tried = -1;
			if (app_quit)
				break;
		}
		menu_cover_update();
		menu_show(want_shot && ++frames == 45 ? SALSA64_DATA_DIR "/menu.ppm" : NULL);
	}
	dl_shutdown();
	seda_release();
	romlist_free(roms);
	roms = NULL;
	menu.list = NULL;
	if (rsxkit_ui_dropped())
		salsa64_log("menú: %u quads no cupieron en algún cuadro", rsxkit_ui_dropped());
	if (!pres.autotest)
		return 0;
	bool ok = !failed && !mn.failures && mn.games == (int)mn.ntest;
	salsa64_log("RESULTADO frontend: %s (menú: %d juegos abiertos y cerrados, %d con fallas)", ok ? "OK" : "FALLO",
		    mn.games, mn.failures);
	return ok ? 0 : 1;
}

/* video.cfg: «salida = 1080» prefiere 1080p (también la línea sola «1080» del
 * formato viejo); «escala = auto|1..5» es el tamaño del render de la N64
 * (auto: 4× a 1080p, 3× a 720p). Sin archivo, 720p y escala automática.
 * Si la tele no acepta 1080, sigue con el siguiente modo de la lista. */
static const int32_t modes_1080[] = {
	VIDEO_RESOLUTION_1080, VIDEO_RESOLUTION_720, VIDEO_RESOLUTION_480, VIDEO_RESOLUTION_576,
};

static bool video_load(struct menu_settings *set)
{
	char text[256];
	size_t n = 0;
	FILE *f = fopen(VIDEO_CFG_PATH, "r");
	if (f) {
		n = fread(text, 1, sizeof text - 1, f);
		fclose(f);
	}
	return menu_video_parse(set, text, n);
}

/* Opciones «Resolución» y «Escala interna» del menú: video.cfg con
 * «salida» y «escala». Se leen al arrancar; «Aplicar ahora» los pone sin
 * reiniciar (menu_apply_video). */
static void video_save(const struct menu_settings *set)
{
	char text[256];
	int n = menu_video_format(set, text, sizeof text);
	FILE *f = fopen(VIDEO_CFG_PATH, "w");
	bool ok = f && fwrite(text, 1, (size_t)n, f) == (size_t)n;
	if (f && fclose(f) != 0)
		ok = false;
	if (!ok)
		salsa64_log("menú: no se pudo escribir %s", VIDEO_CFG_PATH);
	else
		video_scale_configured = true;
}

static const rsxkit_config *video_config_for(int resolution, rsxkit_config *cfg)
{
	if (resolution != MENU_RES_1080P)
		return NULL;
	memset(cfg, 0, sizeof *cfg);
	cfg->modes = modes_1080;
	cfg->num_modes = sizeof modes_1080 / sizeof modes_1080[0];
	return cfg;
}

/* Latido: cada WATCH_US mira que presentación (juego o menú) y emulación
 * (cuando corre: no en pausa ni en la barrera de un estado) sigan dando
 * vueltas. Si una se queda quieta más de STALL_US, al log dónde está cada
 * una (beat.*_at) y lo que hay en vuelo, y otra vez cada STALL_LOG_US: un
 * cuelgue (el del botón PS, 2026-10-11) deja su huella en el log UDP aunque
 * el de archivo pierda las últimas líneas. Solo lee; con más prioridad que
 * los dos para correr aunque uno gire. */
static uint32_t rsx_errors_said;  /* watch_thread: excepciones del RSX ya en el log */

static void watch_thread(void *arg)
{
	(void)arg;
	uint32_t pres0 = beat.pres, emu0 = beat.emu;
	uint64_t now = sysGetSystemTime(), pres_t = now, emu_t = now, pres_said = 0, emu_said = 0, fifo_said = 0;
	while (!beat.quit) {
		sysUsleep(WATCH_US);
		now = sysGetSystemTime();
		if (beat.pres != pres0 || !beat.pres) {  /* antes de la primera vuelta: el arranque */
			if (pres_said)
				salsa64_log("latido: presentación se movió de nuevo tras %.1f s", (now - pres_t) / 1e6);
			pres0 = beat.pres;
			pres_t = now;
			pres_said = 0;
		} else if (now - pres_t >= STALL_US && (!pres_said || now - pres_said >= STALL_LOG_US)) {
			pres_said = now;
			char at[160] = "";
			const char *stage;
			uint32_t task = salsa64_rdp_translating(&stage);
			rsxkit_wait_info wi;
			int len = 0;
			if (task)
				len = snprintf(at, sizeof at, ", tarea %u en «%s»", task, stage);
			if (rsxkit_wait_now(&wi) && len >= 0 && len < (int)sizeof at)
				snprintf(at + len, sizeof at - len, ", esperando al RSX «%s» hace %.1f s (GET 0x%08x PUT 0x%08x)",
					 wi.what, wi.elapsed_us / 1e6, wi.get, wi.put);
			salsa64_log("latido: presentación quieta hace %.1f s en «%s»%s (emulación en «%s»; campo %u, %u en "
				    "cola; sistema %s, pausa 0x%x, emulación %s; audio %llu bloques)",
				    (now - pres_t) / 1e6, beat.pres_at, at, emu_live ? beat.emu_at : "sin juego", n64.bus.vi.fields,
				    field_head - field_tail, pausectl_overlay(&pause_ctl) ? "encima" : "sin dibujar", pause_req,
				    emu_paused ? "quieta" : "andando", (unsigned long long)audio_out_blocks());
		}
		/* Excepciones del RSX que avisó el sistema (rsxkit_rsx_errors): al
		 * log en cuanto llegan, aunque nada espere al RSX todavía. */
		{
			rsxkit_rsx_error re;
			rsxkit_rsx_errors(&re);
			if (re.count != rsx_errors_said) {
				rsx_errors_said = re.count;
				salsa64_log("RSX: excepción del RSX avisada por el sistema: 0x%08x (%u en total; la primera 0x%08x), "
					    "GET 0x%08x PUT 0x%08x; presentación en «%s», campo %u",
					    re.last, re.count, re.first, re.get, re.put, beat.pres_at, n64.bus.vi.fields);
			}
		}
		/* Vigía del RSX: la espera de lugar en el búfer de comandos (dentro
		 * del callback del sistema, sin gancho) y, por si acaso, las otras. */
		rsxkit_wait_info rw;
		if (!rsx_exiting && rsxkit_wait_now(&rw)) {
			bool fifo = rw.kind == RSXKIT_WAIT_FIFO;
			if (fifo && rw.elapsed_us >= RSX_DIAG_US && rw.since_us != fifo_said) {
				fifo_said = rw.since_us;
				rsx_diag("visto por el latido", &rw);
			}
			if (rw.elapsed_us >= rsx_exit_limit() + (fifo ? 0 : RSX_WATCH_EXTRA_US))
				rsx_stuck_exit("tope de la espera, visto por el latido: se sale", &rw);
		}
		bool expect = emu_live && running && !emu_done && !emu_paused && !state_parked && !pause_req;
		if (!expect || beat.emu != emu0) {
			if (emu_said && expect)
				salsa64_log("latido: emulación se movió de nuevo tras %.1f s", (now - emu_t) / 1e6);
			emu0 = beat.emu;
			emu_t = now;
			emu_said = 0;
		} else if (now - emu_t >= STALL_US && (!emu_said || now - emu_said >= STALL_LOG_US)) {
			emu_said = now;
			salsa64_log("latido: emulación quieta hace %.1f s en «%s» (campo %u, %u emulados, %u en cola, RSP "
				    "%s, %u tareas gráficas terminadas, %u esperas al SPE; presentación en «%s»; pausa 0x%x; "
				    "cpu: pc 0x%08llx, ciclos %llu, marca %u, bloque %u)",
				    (now - emu_t) / 1e6, beat.emu_at, n64.bus.vi.fields, emu_stats.fields,
				    field_head - field_tail, n64.bus.rsp.busy ? "ocupado" : "libre", salsa64_rdp_tasks_done(),
				    gfx_spu.waits, beat.pres_at, pause_req, (unsigned long long)n64.cpu.cur_pc,
				    (unsigned long long)n64.cpu.cycles, n64.cpu.rec_mark, n64.cpu.rec_cur);
		}
		/* El juego mismo: dejó de leer los controles y de dibujar con el VI andando. */
		cuelgue_watch(&cuelgue, &n64, now, expect && !selftest_on && !gfxtest_on && !audiotest_on,
			      SALSA64_DATA_DIR, salsa64_log);
	}
	sysThreadExit(0);
}

int main(int argc, char **argv)
{
	char banner[192];
	version_banner(banner, sizeof banner);
	salsa64_log_init_banner("salsa64", banner);
	version_label(salsa64_version, salsa64_commit, version_text, sizeof version_text);
	if (!version_build(salsa64_title, salsa64_appid, build_text, sizeof build_text))
		build_text[0] = '\0';
	pres.autotest = salsa64_autotest();
	ra_init();  /* RetroAchievements: ra.cfg y el cliente; no toca la red hasta que haya sesión o juego */
	/* La caché del recompilador se prueba en otro hilo mientras arranca el
	 * menú (rec_cache.h); run_game espera el resultado. */
	if (salsa64_ps3_rec_wanted())
		salsa64_ps3_rec_probe_start(SALSA64_REC_PROBE_FILE, salsa64_version);
	sys_ppu_thread_t watch_tid;
	bool watching = sysThreadCreate(&watch_tid, watch_thread, NULL, WATCH_PRIO, 0x10000, THREAD_JOINABLE,
					(char *)"salsa64_latido") == 0;
	if (!watching)
		salsa64_log("latido: no se pudo crear su hilo");
	pausectl_init(&pause_ctl, true);

	menu_settings_default(&video_cfg);
	video_scale_configured = video_load(&video_cfg);
	rsxkit_config vcfg;
	const rsxkit_config *vc = video_config_for(video_cfg.resolution, &vcfg);
	int rc = rsxkit_init(vc);
	if (rc == RSXKIT_OK)
		rsxkit_set_wait_hook(rsx_wait_hook, NULL, RSX_HOOK_US, RSX_HOOK_PERIOD_US);
	pres.fb = rc == RSXKIT_OK ? rsxkit_fb_create(FB_MAX_W, FB_MAX_H) : NULL;
	if (!pres.fb) {
		salsa64_log("RESULTADO frontend: FALLO al iniciar el RSX (%d)", rc);
		salsa64_log_shutdown();
		return 1;
	}
	const rsxkit_screen *sc = rsxkit_screen_info();
	salsa64_log("RSX listo: %ux%u, tele %s%s", sc->width, sc->height, sc->aspect > 1.5f ? "16:9" : "4:3",
		    vc ? " (video.cfg: 1080)" : "");
	{
		const rsxkit_video_boot *vb = rsxkit_video_boot_info();
		salsa64_log("video: antes de configurar, estado %d (%s) modo %d, espacio de color %d, escaneo %d, conversión %d, "
			    "aspecto %d, frecuencias 0x%x; configuración vigente %s: resolución %d, formato %d, aspecto %d, pitch %u",
			    vb->state, vb->state == 1 ? "activo" : vb->state == 0 ? "desactivado" : "ocupado", vb->prev_mode,
			    vb->color_space, vb->scan_mode, vb->conversion, vb->aspect, (unsigned)vb->refresh,
			    vb->cfg_ok ? "leída" : "ILEGIBLE", vb->cfg_resolution, vb->cfg_format, vb->cfg_aspect,
			    (unsigned)vb->cfg_pitch);
		salsa64_log("video: se eligió el modo %d, pitch de nuestros búferes %u (el nuestro sería %u); %s; regiones de "
			    "mosaico del sistema: %d%s%s", vb->mode, (unsigned)vb->pitch, (unsigned)sc->width * 4,
			    vb->used_current ? "videoConfigure con los valores vigentes del XMB"
					     : "videoConfigure normal (otro modo que el vigente)",
			    vb->tiles_active, vb->tiles_active ? ": " : "", vb->tiles);
	}
	{
		/* El alfa que dejan los dibujos en un destino con alfa (rsxkit_draw.dst_alpha,
		 * docs/framebuffer.md): lo que se lee de vuelta contra lo esperado. */
		uint8_t a[RSXKIT_DSTA_TEST_N];
		int st = rsxkit_dsta_selftest(a);
		const uint8_t *e = rsxkit_dsta_expected;
		bool ok = st == RSXKIT_OK && !memcmp(a, e, sizeof a);
		salsa64_log("rsxkit: autoprueba dst_alpha ONE=%02x ONE+mezcla=%02x ONE+prueba=%02x ONE+descarte=%02x "
			    "FRAG=%02x KEEP=%02x MAX=%02x/%02x (esperado %02x %02x %02x %02x %02x %02x %02x/%02x): %s",
			    a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7],
			    st != RSXKIT_OK ? "no se pudo" : ok ? "bien" : "MAL");
	}
	if (file_exists(SALSA64_DATA_DIR "/filtros-autoprueba")) {
		bool ok = salsa64_filters_selftest();
		salsa64_log("RESULTADO filtros: %s", ok ? "OK" : "FALLO");
		rsxkit_fb_destroy(pres.fb);
		rsxkit_shutdown();
		salsa64_log_shutdown();
		return ok ? 0 : 1;
	}
	bool filters_opt = !file_exists(SALSA64_DATA_DIR "/filtros-sin-optimizar") &&
			   !file_exists(SALSA64_DATA_DIR "/filtros-opt-fallo");
	salsa64_filters_optimize(filters_opt);
	salsa64_log("filtros: caché de salida %s", filters_opt ? "activa" : "apagada");
	rdp_scale_req = menu_scale_limit(video_cfg.scale, video_cfg.resolution);
	if ((rc = salsa64_rdp_init((uint32_t)rdp_scale_req)) != RSXKIT_OK) {
		salsa64_log("RESULTADO frontend: FALLO al preparar el RDP (%d)", rc);
		rsxkit_shutdown();
		salsa64_log_shutdown();
		return 1;
	}
	{
		bool cut;
		uint32_t es = salsa64_rdp_scale(&cut);
		salsa64_log("RDP listo: escala-objetivo %u× (%u líneas por imagen; cada destino, la entera que llega ahí), %s%s", es, 240 * es,
			    menu_scale_is_auto(video_cfg.scale) ? "máximo automático" : "pedida",
			    cut ? ", reducida por memoria del RSX" : "");
	}
	rdp_sink = salsa64_rdp_sink();

	char name[80] = "";
	FILE *ef = fopen(EMBED_PATH, "r");
	bool have_embed = ef != NULL;
	if (ef) {
		if (!fgets(name, sizeof name, ef))
			name[0] = '\0';
		fclose(ef);
		name[strcspn(name, " \r\n")] = '\0';
	}
	if (!strncmp(name, "dump:", 5)) {
		char path[96];
		snprintf(path, sizeof path, SALSA64_DATA_DIR "/%s", name + 5);
		/* Como al abrir un juego: para comparar la pasada de aristas con el
		 * mismo volcado. */
		if (file_exists(SALSA64_DATA_DIR "/aristas-aa")) {
			salsa64_rdp_aa_edges(true);
			salsa64_log("aristas de los dibujos con AA: encendidas (aristas-aa, experimental)");
		}
		salsa64_rdp_set_log(salsa64_log);  /* los avisos del RDP también en la repetición */
		int rr = salsa64_replay(path);
		salsa64_log("RESULTADO frontend: %s (repetición)", rr == 0 ? "OK" : "FALLO");
		salsa64_rdp_shutdown();
		rsxkit_shutdown();
		salsa64_log_shutdown();
		return rr == 0 ? 0 : 1;
	}

	rdram = NULL;
	if (salsa64_ps3_fastmem_wanted() && salsa64_ps3_rec_wanted()) {
		/* fastmem (docs/fastmem.md): la RDRAM al principio de una ventana de
		 * 512 MB sin mapear; el recompilador la aprovecha si la autoprueba
		 * pasa (más abajo, al abrir el juego). */
		rdram = salsa64_ps3_fastmem_window(SALSA64_RDRAM_MAX);
		if (!rdram)
			salsa64_log("fastmem (jit-fastmem): sin ventana, la RDRAM va como siempre");
	}
	if (!rdram && file_exists(RDRAM_1M_PATH)) {
		sys_mem_addr_t addr = 0;
		if (sysMemoryAllocate(SALSA64_RDRAM_MAX, SYS_MEMORY_PAGE_SIZE_1M, &addr) == 0)
			rdram = (uint8_t *)(uintptr_t)addr;
		salsa64_log("RDRAM con sysMemoryAllocate de 1 MB (rdram-1m): %s", rdram ? "bien" : "FALLO, por el heap");
	}
	if (!rdram)
		rdram = memalign(128, SALSA64_RDRAM_MAX);
	for (int i = 0; i < SNAP_SLOTS; i++)
		snap_buf[i] = memalign(128, SNAP_BYTES);
	sys_sem_attr_t sem_attr = { SYS_SEM_ATTR_PROTOCOL, SYS_SEM_ATTR_PSHARED, 0, 0, 0, "wake" };
	if (!rdram || !snap_buf[SNAP_SLOTS - 1] || sysSemCreate(&wake_sem, &sem_attr, 0, 1) != 0) {
		salsa64_log("RESULTADO frontend: FALLO sin memoria");
		salsa64_rdp_shutdown();
		rsxkit_shutdown();
		salsa64_log_shutdown();
		return 1;
	}
	/* Efectos de framebuffer: copias RSX → RDRAM y el despertar del hilo de
	 * presentación cuando una tarea las necesita (docs/framebuffer.md). */
	salsa64_rdp_set_memory(rdram, SALSA64_RDRAM_MAX, wake);
	salsa64_rdp_set_log(salsa64_log);
	ioPadInit(7);
	load_padmap();
	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_cb, NULL);

	int status = 0;
	if (have_embed) {
		/* Directo, como siempre: las pruebas automáticas dependen de esto. */
		game_reset();
		uint32_t rom_size = 0;
		const char *source = NULL;
		uint8_t *rom = load_rom(&rom_size, &source);
		if (!rom) {
			salsa64_log("RESULTADO frontend: FALLO sin memoria");
			status = 1;
		} else {
			char err[160];
			status = run_game(rom, rom_size, source, err, sizeof err) == GAME_FAILED;
		}
	} else {
		in_menu = 1;
		res_tag = "prueba del juego";
		/* La carga del menú en su hilo mientras corre la secuencia de arranque
		 * (intro_run, en menu_loop); sin ella, todo en el principal. */
		/* En la prueba automática no hay secuencia, salvo con el archivo intro-prueba. */
		if ((!pres.autotest || file_exists(INTRO_TEST_PATH)) && !file_exists(INTRO_OFF_PATH) &&
		    ui_init_once() == RSXKIT_OK)
			preload_start();
		direct_request(argc, argv);
		salsa64_log("sin %s: menú", EMBED_PATH);
		status = menu_loop();
	}

	ra_shutdown();
	/* Después de soltar la HLE (game_close): su fin llama a front_done → wake(). */
	sysSemDestroy(wake_sem);
	salsa64_rdp_shutdown();
	rsxkit_shutdown();
	ioPadEnd();
	if (watching) {
		u64 rv;
		beat.quit = 1;
		sysThreadJoin(watch_tid, &rv);
	}
	font_stats_log("al salir");
	if (font_state == 1) {
		rsxkit_ui_font(NULL);
		rsxkit_sysfont_close();  /* libfont, su memoria y los tres módulos */
	}
	salsa64_log("salida normal%s", app_quit ? " («Salir del juego» del sistema)" : "");
	salsa64_ps3_fastmem_stop();
	salsa64_log_shutdown();
	return status;
}
