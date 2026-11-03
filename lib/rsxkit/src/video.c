/* SPDX-License-Identifier: MIT */
/*
 * rsxkit — inicialización del RSX, salida de video y ciclo de cuadro.
 *
 * Basado en la inicialización de tests/phase0/rsx-triangle de Salsa64, que
 * a su vez sigue el sample rsxtest de PSL1GHT (MIT): búfer de comandos en
 * memoria principal mapeada, dos búferes de color X8R8G8B8 y uno de
 * profundidad en la memoria local del RSX, flip en vsync.
 */
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <sys/systime.h>
#include <sysutil/video.h>

#include "internal.h"

#define DEFAULT_CMD_SIZE  0x80000u
#define DEFAULT_HOST_SIZE (16u << 20)

struct rsxkit_state rsxkit_g;

static uint64_t screen_ticket;
static uint32_t screen_saved;
static bool screen_saved_valid;

void rsxkit_screen_texture_invalidate(void)
{
	if (screen_saved == rsxkit_g.cur)
		screen_saved_valid = false;
}

uint64_t rsxkit_screen_texture(rsxkit_texture *out)
{
	struct rsxkit_state *g = &rsxkit_g;
	memset(out, 0, sizeof *out);
	out->offset = g->color_offset[g->cur];
	out->pitch = g->color_pitch;
	out->width = g->screen.width;
	out->height = g->screen.height;
	out->format = RSXKIT_XRGB8888;
	out->location = RSXKIT_MEM_LOCAL;
	screen_saved = g->cur;
	screen_saved_valid = true;
	return ++screen_ticket;
}

bool rsxkit_screen_texture_valid(uint64_t ticket)
{
	return screen_saved_valid && ticket == screen_ticket && screen_saved != rsxkit_g.cur;
}

static const int32_t default_modes[] = {
	VIDEO_RESOLUTION_720, VIDEO_RESOLUTION_1080, VIDEO_RESOLUTION_480, VIDEO_RESOLUTION_576,
};

/* Espera en el PPE a que ocurra el flip en vuelo: hasta entonces, el búfer
 * que se va a dibujar es el que está en pantalla. Antes era un
 * gcmSetWaitFlip en el búfer de comandos, pero con el RSX parado en esa
 * espera el sistema de la PS3 no dibujaba su menú encima a tiempo y cada
 * flip tardaba segundos (también después de cerrarlo). `max_us` 0: sin
 * límite. Devuelve false si se acabó el tiempo. */
static bool wait_flip(uint64_t max_us)
{
	struct rsxkit_state *g = &rsxkit_g;
	if (!g->flip_pending)
		return true;
	if (rsxkit_lost())
		return true;
	uint64_t t0 = sysGetSystemTime();
	bool waited = false, ok = true;
	u32 st;
	while ((st = gcmGetFlipStatus()) != 0) {
		if (max_us && sysGetSystemTime() - t0 > max_us) {
			ok = false;
			break;
		}
		if (!waited) {
			rsxkit_wait_begin(RSXKIT_WAIT_FLIP, "flip", 0, 0, st);
			waited = true;
		}
		sysUsleep(200);
		if (!rsxkit_wait_tick(st))
			break;
	}
	if (waited)
		rsxkit_wait_end();
	if (ok)
		g->flip_pending = 0;
	return ok;
}

void rsxkit_wait_label(uint8_t index, uint32_t value)
{
	volatile uint32_t *label = gcmGetLabelAddress(index);
	if ((int32_t)(*label - value) >= 0)
		return;
	if (rsxkit_lost())
		return;
	rsxFlushBuffer(rsxkit_g.ctx);  /* por si la escritura sigue en el búfer */
	rsxkit_wait_begin(RSXKIT_WAIT_LABEL, rsxkit_label_name(index), index, value, *label);
	uint32_t v;
	while ((int32_t)((v = *label) - value) < 0) {
		sysUsleep(30);
		if (!rsxkit_wait_tick(v))
			break;
	}
	rsxkit_wait_end();
}

/* Primer modo de la lista que la tele da; -1 si ninguno. */
static int32_t pick_mode(const rsxkit_config *cfg, videoResolution *res)
{
	const int32_t *modes = cfg && cfg->modes ? cfg->modes : default_modes;
	uint32_t n = cfg && cfg->modes ? cfg->num_modes : sizeof default_modes / sizeof default_modes[0];

	for (uint32_t i = 0; i < n; i++) {
		if (videoGetResolutionAvailability(VIDEO_PRIMARY, modes[i], VIDEO_ASPECT_AUTO, 0) == 1 &&
		    videoGetResolution(modes[i], res) == 0)
			return modes[i];
	}
	return -1;
}

/* videoConfigure del modo `id` y la superficie de salida resultante. */
static uint32_t pitch_override;  /* pitch vigente del XMB (init_video), 0 = el nuestro */

static int set_mode(int32_t id, const videoResolution *res)
{
	pitch_override = 0;
	videoConfiguration vc;
	memset(&vc, 0, sizeof vc);
	vc.resolution = (u8)id;
	vc.format = VIDEO_BUFFER_FORMAT_XRGB;
	vc.aspect = VIDEO_ASPECT_AUTO;
	vc.pitch = res->width * 4;
	if (videoConfigure(VIDEO_PRIMARY, &vc, NULL, 0) != 0)
		return RSXKIT_EVIDEO;

	videoState st;
	int wide = id == VIDEO_RESOLUTION_720 || id == VIDEO_RESOLUTION_1080;
	if (videoGetState(VIDEO_PRIMARY, 0, &st) == 0 && st.displayMode.aspect != VIDEO_ASPECT_AUTO)
		wide = st.displayMode.aspect == VIDEO_ASPECT_16_9;

	rsxkit_g.screen.width = res->width;
	rsxkit_g.screen.height = res->height;
	rsxkit_g.screen.aspect = wide ? 16.0f / 9.0f : 4.0f / 3.0f;
	rsxkit_g.mode_id = id;
	return RSXKIT_OK;
}

static rsxkit_video_boot boot_info = { .prev_mode = -1, .mode = -1 };
const rsxkit_video_boot *rsxkit_video_boot_info(void)
{
	return &boot_info;
}

/* Regiones de mosaico (tiles) con algo puesto, para el log. */
static void scan_tiles(void)
{
	const gcmTileInfo *ti = gcmGetTileInfo();
	size_t n = 0;
	boot_info.tiles[0] = '\0';
	for (int i = 0; ti && i < 15; i++) {
		if (!ti[i].tile && !ti[i].limit && !ti[i].pitch)
			continue;
		boot_info.tiles_active++;
		if (n + 40 < sizeof boot_info.tiles)
			n += (size_t)snprintf(boot_info.tiles + n, sizeof boot_info.tiles - n, "%d:%x/%x/%x ", i,
					      (unsigned)ti[i].tile, (unsigned)ti[i].limit, (unsigned)ti[i].pitch);
	}
}

static int init_video(const rsxkit_config *cfg)
{
	videoResolution res;
	int32_t id = pick_mode(cfg, &res);
	if (id < 0)
		return RSXKIT_EVIDEO;
	memset(&boot_info, 0, sizeof boot_info);
	boot_info.prev_mode = -1;
	boot_info.mode = id;
	boot_info.configured = 1;
	pitch_override = 0;
	videoState st;
	if (videoGetState(VIDEO_PRIMARY, 0, &st) == 0) {
		boot_info.state_ok = 1;
		boot_info.prev_mode = st.displayMode.resolution;
		boot_info.state = st.state;
		boot_info.color_space = st.colorSpace;
		boot_info.scan_mode = st.displayMode.scanMode;
		boot_info.conversion = st.displayMode.conversion;
		boot_info.aspect = st.displayMode.aspect;
		boot_info.refresh = st.displayMode.refreshRates;
	}
	videoConfiguration cur;
	memset(&cur, 0, sizeof cur);
	if (videoGetConfiguration(VIDEO_PRIMARY, &cur, NULL) == 0) {
		boot_info.cfg_ok = 1;
		boot_info.cfg_resolution = cur.resolution;
		boot_info.cfg_format = cur.format;
		boot_info.cfg_aspect = cur.aspect;
		boot_info.cfg_pitch = cur.pitch;
	}
	scan_tiles();
	if (boot_info.cfg_ok && cur.resolution == id && cur.pitch >= res.width * 4 && cur.format == VIDEO_BUFFER_FORMAT_XRGB) {
		/* Los mismos valores que dejó el XMB: un cambio real de formato,
		 * aspecto o pitch hace que el monitor renegocie el HDMI. */
		videoConfiguration vc = cur;
		if (videoConfigure(VIDEO_PRIMARY, &vc, NULL, 0) != 0)
			return RSXKIT_EVIDEO;
		boot_info.configured_us = (uint64_t)sysGetSystemTime();
		pitch_override = cur.pitch;
		boot_info.used_current = 1;
		int wide = id == VIDEO_RESOLUTION_720 || id == VIDEO_RESOLUTION_1080;
		if (boot_info.aspect != VIDEO_ASPECT_AUTO)
			wide = boot_info.aspect == VIDEO_ASPECT_16_9;
		rsxkit_g.screen.width = res.width;
		rsxkit_g.screen.height = res.height;
		rsxkit_g.screen.aspect = wide ? 16.0f / 9.0f : 4.0f / 3.0f;
		rsxkit_g.mode_id = id;
		return RSXKIT_OK;
	}
	int rc = set_mode(id, &res);
	boot_info.configured_us = (uint64_t)sysGetSystemTime();
	return rc;
}

static void free_screen_buffers(void)
{
	screen_saved_valid = false;
	struct rsxkit_state *g = &rsxkit_g;
	for (int i = 0; i < 2; i++) {
		if (g->color_buf[i])
			rsxFree(g->color_buf[i]);
		g->color_buf[i] = NULL;
	}
	if (g->depth_buf)
		rsxFree(g->depth_buf);
	g->depth_buf = NULL;
}

/* Profundidad de la pantalla: del tamaño de la salida o de lo reservado
 * con rsxkit_depth_reserve, lo mayor en cada lado. */
static int alloc_screen_depth(void)
{
	struct rsxkit_state *g = &rsxkit_g;
	uint32_t w = g->screen.width > g->depth_min_w ? g->screen.width : g->depth_min_w;
	uint32_t h = g->screen.height > g->depth_min_h ? g->screen.height : g->depth_min_h;
	g->depth_pitch = (w * 4 + 63) & ~63u;
	g->depth_buf = rsxMemalign(64, h * g->depth_pitch);
	if (!g->depth_buf) {
		g->depth_w = g->depth_h = 0;
		return RSXKIT_ENOMEM;
	}
	rsxAddressToOffset(g->depth_buf, &g->depth_offset);
	g->depth_w = g->depth_pitch / 4;
	g->depth_h = h;
	return RSXKIT_OK;
}

/* Búferes de color (con su display buffer) y de profundidad del tamaño de
 * la superficie de salida en curso. */
static int alloc_screen_buffers(void)
{
	struct rsxkit_state *g = &rsxkit_g;
	uint32_t w = g->screen.width, h = g->screen.height;
	g->color_pitch = pitch_override ? pitch_override : w * 4;
	boot_info.pitch = g->color_pitch;
	for (int i = 0; i < 2; i++) {
		g->color_buf[i] = rsxMemalign(64, h * g->color_pitch);
		if (!g->color_buf[i])
			return RSXKIT_ENOMEM;
		rsxAddressToOffset(g->color_buf[i], &g->color_offset[i]);
		gcmSetDisplayBuffer(i, g->color_offset[i], g->color_pitch, w, h);
	}
	return alloc_screen_depth();
}

int rsxkit_depth_reserve(uint32_t width, uint32_t height)
{
	struct rsxkit_state *g = &rsxkit_g;
	if (!g->ctx)
		return RSXKIT_EINIT;
	if (width > 4096 || height > 4096)
		return RSXKIT_EARG;
	uint32_t old_w = g->depth_min_w, old_h = g->depth_min_h;
	uint32_t w = g->screen.width > width ? g->screen.width : width;
	uint32_t h = g->screen.height > height ? g->screen.height : height;
	g->depth_min_w = width;
	g->depth_min_h = height;
	if (g->depth_buf && g->depth_w == ((w * 4 + 63) & ~63u) / 4 && g->depth_h == h)
		return RSXKIT_OK;
	/* Que el RSX no use la vieja; lo ligado deja de valer. */
	rsxkit_finish();
	if (g->depth_buf)
		rsxFree(g->depth_buf);
	g->depth_buf = NULL;
	rsxkit_bound.target = NULL;
	rsxkit_bound.depth = NULL;
	g->screen_bound = 0;
	g->state_valid = 0;
	int rc = alloc_screen_depth();
	if (rc != RSXKIT_OK) {
		/* La de antes cabe en lo que se acaba de soltar. */
		g->depth_min_w = old_w;
		g->depth_min_h = old_h;
		if (alloc_screen_depth() != RSXKIT_OK)
			return RSXKIT_ENOMEM;
	}
	return rc;
}

int rsxkit_init(const rsxkit_config *cfg)
{
	struct rsxkit_state *g = &rsxkit_g;
	uint32_t cmd = cfg && cfg->cmd_size ? cfg->cmd_size : DEFAULT_CMD_SIZE;
	uint32_t host = cfg && cfg->host_size ? cfg->host_size : DEFAULT_HOST_SIZE;

	memset(g, 0, sizeof *g);
	g->fp_3d = -1;
	g->host = memalign(1024 * 1024, host);
	g->host_size = host;
	if (!g->host || rsxInit(&g->ctx, cmd, host, g->host) != 0 || !g->ctx)
		return RSXKIT_EINIT;
	/* Las etiquetas de GCM no están inicializadas. La comparación modular
	 * de las esperas exige partir del mismo cero que los contadores. */
	*(volatile u32 *)gcmGetLabelAddress(RSXKIT_LABEL_FINISH) = 0;
	*(volatile u32 *)gcmGetLabelAddress(RSXKIT_LABEL_FENCE) = 0;
	__sync_synchronize();
	rsxkit_diag_init();  /* aviso de excepciones del RSX y rastro (diag.c) */
	int rc = init_video(cfg);
	if (rc != RSXKIT_OK)
		return rc;

	rsxkit_finish();
	gcmSetFlipMode(GCM_FLIP_VSYNC);

	rc = alloc_screen_buffers();
	if (rc != RSXKIT_OK)
		return rc;

	/* Sin arreglos de vértices: rsxkit dibuja en modo inmediato. */
	for (int i = 0; i < 16; i++)
		rsxBindVertexArrayAttrib(g->ctx, i, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	return RSXKIT_OK;
}

void rsxkit_shutdown(void)
{
	struct rsxkit_state *g = &rsxkit_g;
	if (!g->ctx)
		return;
	wait_flip(1000000);  /* con el sistema encima puede no llegar: no esperarlo para siempre */
	rsxFinish(g->ctx, 1);
	rsxkit_fb_shutdown();
	rsxkit_3d_shutdown();
	rsxkit_target_shutdown();
	rsxkit_post_shutdown();
	free_screen_buffers();
	rsxkit_diag_shutdown();
	g->ctx = NULL;
}

int rsxkit_video_reconfigure(const rsxkit_config *cfg)
{
	struct rsxkit_state *g = &rsxkit_g;
	if (!g->ctx)
		return RSXKIT_EINIT;
	videoResolution res, old_res;
	int32_t id = pick_mode(cfg, &res);
	if (id < 0)
		return RSXKIT_EVIDEO;
	int32_t old_id = g->mode_id;
	if (videoGetResolution(old_id, &old_res) != 0)
		return RSXKIT_EVIDEO;

	/* Que el RSX no esté usando nada de lo que se libera: ni el flip en
	 * vuelo ni los comandos pendientes. */
	wait_flip(0);
	rsxkit_finish();

	int rc = set_mode(id, &res);
	if (rc == RSXKIT_OK) {
		free_screen_buffers();
		rc = alloc_screen_buffers();
	}
	if (rc != RSXKIT_OK) {
		/* Vuelve al modo anterior: el libre de recién cabe de nuevo. */
		free_screen_buffers();
		int back = set_mode(old_id, &old_res);
		if (back == RSXKIT_OK)
			back = alloc_screen_buffers();
		if (back != RSXKIT_OK)
			rc = back;
	}
	g->cur = 0;
	g->flip_pending = 0;
	g->screen_bound = 0;
	g->state_valid = 0;
	gcmResetFlipStatus();
	return rc;
}

const rsxkit_screen *rsxkit_screen_info(void)
{
	return &rsxkit_g.screen;
}

void rsxkit_finish(void)
{
	struct rsxkit_state *g = &rsxkit_g;
	/* Abandonado el RSX (rsxkit_abandon) no se le manda nada más: un
	 * vaciado movería PUT de nuevo. */
	if (rsxkit_lost())
		return;
	uint32_t v = ++g->finish_seq;
	rsxkit_trace_note("finish", "%u", v, 0, 0, 0);
	rsxkit_reserve(16);
	rsxSetWriteBackendLabel(g->ctx, RSXKIT_LABEL_FINISH, v);
	rsxFlushBuffer(g->ctx);
	rsxkit_wait_label(RSXKIT_LABEL_FINISH, v);
}

void rsxkit_bind_screen(void)
{
	rsxkit_screen_texture_invalidate();
	struct rsxkit_state *g = &rsxkit_g;
	gcmSurface sf;
	memset(&sf, 0, sizeof sf);
	sf.colorFormat = GCM_SURFACE_X8R8G8B8;
	sf.colorTarget = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0] = g->color_offset[g->cur];
	sf.colorPitch[0] = g->color_pitch;
	for (int i = 1; i < GCM_MAX_MRT_COUNT; i++) {
		sf.colorLocation[i] = GCM_LOCATION_RSX;
		sf.colorPitch[i] = 64;
	}
	sf.depthFormat = GCM_SURFACE_ZETA_Z24S8;
	sf.depthLocation = GCM_LOCATION_RSX;
	sf.depthOffset = g->depth_offset;
	sf.depthPitch = g->depth_pitch;
	sf.type = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias = GCM_SURFACE_CENTER_1;
	sf.width = g->screen.width;
	sf.height = g->screen.height;
	rsxSetSurface(g->ctx, &sf);
	rsxkit_set_2d_state();
	g->screen_bound = 1;
}

void rsxkit_set_2d_state(void)
{
	rsxkit_set_2d_state_size(rsxkit_g.screen.width, rsxkit_g.screen.height);
}

void rsxkit_set_2d_state_size(uint32_t w, uint32_t h)
{
	struct rsxkit_state *g = &rsxkit_g;
	f32 scale[4] = { w * 0.5f, h * -0.5f, 0.5f, 0.0f };
	f32 offset[4] = { w * 0.5f, h * 0.5f, 0.5f, 0.0f };

	rsxSetColorMask(g->ctx, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
	rsxSetColorMaskMrt(g->ctx, 0);
	rsxSetStencilTestEnable(g->ctx, GCM_FALSE);
	rsxSetBlendEquation(g->ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);  /* el 3D la deja en MAX o MIN */
	rsxSetViewport(g->ctx, 0, 0, w, h, 0.0f, 1.0f, scale, offset);
	rsxSetScissor(g->ctx, 0, 0, w, h);
	rsxSetZMinMaxControl(g->ctx, GCM_FALSE, GCM_TRUE, GCM_FALSE);
	for (int i = 0; i < 8; i++)
		rsxSetViewportClip(g->ctx, i, w, h);
	rsxSetDepthTestEnable(g->ctx, GCM_FALSE);
	rsxSetDepthWriteEnable(g->ctx, GCM_FALSE);
	rsxSetCullFaceEnable(g->ctx, GCM_FALSE);
	rsxSetBlendEnable(g->ctx, GCM_FALSE);
	rsxSetShadeModel(g->ctx, GCM_SHADE_MODEL_SMOOTH);
}

void rsxkit_frame_begin(uint32_t clear_rgb)
{
	struct rsxkit_state *g = &rsxkit_g;
	wait_flip(0);  /* el búfer de ahora estuvo en pantalla hasta el flip en vuelo */
	rsxkit_reserve(256);
	rsxkit_bind_screen();
	rsxSetClearColor(g->ctx, clear_rgb & 0x00FFFFFFu);
	rsxSetClearDepthStencil(g->ctx, 0xFFFFFF00u);
	rsxClearSurface(g->ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A |
				      GCM_CLEAR_S | GCM_CLEAR_Z);
}

bool rsxkit_frame_ready(void)
{
	return !rsxkit_g.flip_pending || gcmGetFlipStatus() == 0;
}

void rsxkit_frame_end(void)
{
	struct rsxkit_state *g = &rsxkit_g;
	/* Un flip en vuelo como máximo (por si no hubo rsxkit_frame_begin). El
	 * RSX no espera el flip: lo que se le mande después no toca la
	 * pantalla hasta el próximo rsxkit_frame_begin, que espera en el PPE. */
	if (rsxkit_lost())
		return;  /* ni flip ni vaciado (rsxkit_abandon) */
	wait_flip(0);
	gcmResetFlipStatus();
	rsxkit_trace_note("flip", "búfer %u", g->cur, 0, 0, 0);
	rsxkit_reserve(64);
	gcmSetFlip(g->ctx, g->cur);
	rsxFlushBuffer(g->ctx);
	g->flip_pending = 1;
	g->cur ^= 1;
	/* En la PS3 el sistema dibuja su menú (y sus avisos) en los flips de la
	 * aplicación y pisa estado del RSX: programas, constantes, arreglos.
	 * Nada de lo puesto antes del flip se da por bueno después (unos
	 * cientos de bytes por cuadro). Sin esto, con su menú encima y un rato
	 * después de cerrarlo, todo lo dibujado salía negro. */
	rsxkit_forget_state();
}

uint64_t rsxkit_vblank_count(void)
{
	return gcmGetVBlankCount();
}

int rsxkit_after_system(void)
{
	screen_saved_valid = false;
	struct rsxkit_state *g = &rsxkit_g;
	if (!g->ctx)
		return 0;
	int bad = rsxkit_fp_verify(1);
	if (bad)
		rsxkit_fp_flush();
	/* Lo que rsxkit da por puesto, de nuevo en el próximo dibujo (también
	 * se olvida en cada flip: el sistema sigue pisándolo un rato después de
	 * avisar que terminó). */
	rsxkit_forget_state();
	rsxkit_reserve(16);
	rsxInvalidateVertexCache(g->ctx);
	rsxInvalidateTextureCache(g->ctx, GCM_INVALIDATE_TEXTURE);
	gcmSetFlipMode(GCM_FLIP_VSYNC);
	return bad;
}

const uint32_t *rsxkit_back_buffer(uint32_t *pitch_bytes)
{
	if (pitch_bytes)
		*pitch_bytes = rsxkit_g.color_pitch;
	return rsxkit_g.color_buf[rsxkit_g.cur];
}

/* Diagnóstico: la dirección en memoria principal de un offset de E/S del
 * búfer de comandos (el GET o el PUT del RSX), o NULL si cae fuera de la
 * memoria mapeada. `avail` recibe cuántas palabras quedan hasta el final. */
const uint32_t *rsxkit_cmd_at(uint32_t io_offset, uint32_t *avail)
{
	struct rsxkit_state *g = &rsxkit_g;
	uint32_t base;
	if (!g->host || rsxAddressToOffset(g->host, &base) != 0 || io_offset < base ||
	    io_offset - base >= g->host_size || (io_offset & 3))
		return NULL;
	if (avail)
		*avail = (g->host_size - (io_offset - base)) / 4;
	return (const uint32_t *)((const uint8_t *)g->host + (io_offset - base));
}
