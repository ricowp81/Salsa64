# Changelog

All notable changes to Salsa64. Speeds are measured on a real PS3 (HEN,
NTSC backups); `docs/perf-log.md` has the detailed numbers and
`docs/estado.md` the work log (both in Spanish).

## [Unreleased]

### Fixed
- **Conker's Bad Fur Day (USA): the Great Mighty Poo did not open his mouth and
  Sweet Melody got stuck when entered straight from the Chapters menu** (#3).
  The game's music manager starts a song by sending PLAY to the sequence
  player and, on every game frame, treats a stopped player as a finished song
  and drops its record. With the emulated CPU (faster than the real one) the
  next game frame could arrive before the audio thread had processed the PLAY:
  the record of the cave song was dropped, the song played without it, and the
  markers that drive the Poo's mouth and the scene were lost. The emulator now
  patches the game's PLAY routine in RAM, only when the original code is there,
  so it waits for the audio thread to start the player (as the game already
  does when it stops one). Only the USA retail ROM is affected; the file
  `conker-musica-vieja` in `/dev_hdd0/salsa64/` turns the patch off.
- **Donkey Kong 64 (USA): the DK Rap intro had no music, and it came in about
  1.5 minutes late and out of sync with the lyrics** (#51). The game's music
  manager starts a song and, on the next game frame, expects the audio thread
  to have processed the PLAY; if the player is still stopped it stops it,
  reloads the song from the ROM and sends PLAY again. With the emulated CPU
  (faster than the real one) the switch to the rap scene fits a one-retrace
  frame, the main thread ends up on the opposite field phase and every reload
  lands just before the audio thread's frame: the song was reloaded 30 times
  per second for ~96 s and never played (on ares the song loads once and plays
  on the next frame). The emulator now patches that manager in RAM, only when
  the original code is there, so a song whose PLAY is pending waits for the
  player instead of being reloaded. Only the USA retail ROM is affected; the
  file `dk64-musica-vieja` in `/dev_hdd0/salsa64/` turns the patch off.

## [0.2.15] — 2026-10-29

### Changed
- **Recompiler: TLB-mapped code rewritten with the same bytes is no longer
  retranslated.** The byte copies that let a block be re-attached were a 1 MB
  pool that was never recycled; Perfect Dark filled it in the first minute and
  then retranslated every reloaded overlay (77 % of its compilations, 60–120 ms
  stutters). Copies now live in 64/128/256-byte slots (4 MB) freed when the
  block dies; detached blocks are reclaimed before a full cache flush. The
  flag file `jit-copias-viejas` restores the old behaviour for A/B tests.
- **Serial Interface timing:** a PIF RAM write DMA now takes about 65 µs and
  the read DMA what the PIF needs for the joybus exchanges it carries (0.66 ms
  for one controller or one Controller Pak block, 1.65 ms for four controllers;
  constants measured by the ares project), instead of a flat 2 ms each way.
  The file `si-ritmo-viejo` in `/dev_hdd0/salsa64/` restores the flat 2 ms.
- **Data cache write-back cost:** the emulated CPU does not model the VR4300
  caches, so writing back the whole data cache (`osWritebackDCacheAll`, or
  `osWritebackDCache`/`osInvalDCache` of 8 KB or more) took 2,000 cycles
  instead of the console's ~20 cycles per dirty line. Each full sweep now costs
  10,240 cycles (half of the 512 lines dirty, 40 cycles each, as measured by the
  ares project). The file `sin-barrido-cache` in `/dev_hdd0/salsa64/` makes it
  free again.
- **Heartbeat for games that never start:** the file `latido-siempre` in
  `/dev_hdd0/salsa64/` makes the heartbeat report (pc, registers, last RCP
  events, RDRAM dump) also for a game that never read the controller nor drew
  anything, such as Toy Story 2 (#54).
- **RetroAchievements Hardcore:** saving a state is now allowed in Hardcore
  (loading stays blocked everywhere); turning Hardcore on with a game open no
  longer switches the running game's mode: it applies from the next game.
  Hardcore is not yet approved by RetroAchievements (approval will be requested
  in April 2027, after 6 public months); regular achievements work meanwhile.

### Fixed
- **F-Zero X: white background and mis-shaded objects over the track.** The
  RSP clips every triangle against the clip-ratio box before the RDP sees it,
  and the RDP interpolates the shade linearly in screen space inside each
  clipped piece; the HLE sent the whole triangle and the RSX interpolated with
  perspective correction, so the background quads (two vertices near the
  camera, far off screen) lost their shade and showed the raw texture, and the
  horizon haze arrived late. The HLE now clips like the microcode (same
  polygon, same fan, verified against the real F3DEX2 running in the host LLE)
  and rsxkit interpolates the vertex shade linearly in screen space (the RSX
  input bit that skips perspective correction, with the vertex program
  pre-multiplying by w: no extra fragment instructions). Measured against ares
  on the demo races: 19–27 dB → 30–37 dB (4×4 blocks); other games unchanged
  within 0.1 dB. `sin-sombra-afin` in the data directory restores the previous
  behaviour.
- **Audio (ABI1, libultra synthesizer: Super Mario 64, Wave Race 64, GoldenEye,
  Diddy Kong Racing, Donald Duck: Quack Attack…): POLEF with A_INIT now starts
  like the real microcode.** The microcode does not zero the filter's two
  previous samples on A_INIT: it takes them from its state scratch area in DMEM,
  which holds what the last RESAMPLE (its last samples) or ENVMIXER (volume
  lanes) left there, so a filter that starts right after a resample carries a
  short decaying transient from those samples; mupen64plus-rsp-hle started
  from zero. Measured against the real microcode task by task: Donald Duck
  (Europe and USA) and Diddy Kong Racing go from one differing audio task to
  none; the other ABI1 games were already identical. `audio-polef-cero` in
  `/dev_hdd0/salsa64/` (`SALSA64_AUDIO_POLEF_ZERO=1` on the host) restores the
  zero start.
- **Worms Armageddon: slanted, striped image** (#57). Its S2DEX backgrounds
  declare an image one texel wider than each row in memory (321 for 320-byte
  rows); the microcode steps rows by the declared width rounded down to 8
  bytes, the HLE stepped by the full width, so every row slid one texel and
  the whole screen leaned. Measured against the real microcode: the logos,
  title and language menu are now identical. `s2dex-paso-viejo` in
  `/dev_hdd0/salsa64/` restores the old row step for comparison.
- **LEGO Racers: flickering box around distant cars** (#34). The game draws
  each distant car into a 32 × 32 image cleared with alpha 0 and pastes it
  with an alpha test. When that image was a few frames old it was decoded
  from RAM, and the emulator's RSX → RAM copies always set the alpha bit, so
  the car came with its square background (often at the start of a race).
  Copies of render-to-texture images now carry the RSX alpha into the
  RGBA5551 alpha bit, as the RDP does; `copia-sin-alfa` in
  `/dev_hdd0/salsa64/` restores the old copies for comparison.
- **Paper Mario: dark rooms stayed completely black (Watt's light).** The game
  builds the light mask with the RDP every frame: a 64 × 64 RGBA32 image on top
  of its z-buffer, the same memory reread as an 8-bit image, loaded into TMEM
  and then filled back with the z value before the full-screen darkness is
  drawn with it. The HLE read the mask from RAM, where only the z-buffer was,
  so everything outside the HUD was dark. Those small 32-bit images (up to
  64 × 64, and the 8-bit image on the same memory) are now drawn by the PPE
  straight into RAM, and the next frame's light uses them (one frame late);
  the light around Mario and Watt's ability (C-Down) match the reference
  emulator. The flag file `gfx-mascara32-rsx` restores the old behaviour
  (`SALSA64_MASCARA32_RSX=1` on the host).
- **Indiana Jones and the Infernal Machine: invisible Indy, half the screen
  black and a console freeze after leaving a waterfall.** The graphics HLE
  ran the game's secondary display lists (the ones Factor 5's microcode runs
  before drawing) until a `B8000000 FFFFFFFF`, but some end with a plain
  `B8000000 00000000`: it kept going through the vertex data behind them and
  executed vertices as RDP commands (a colour image at 0x880000 for Indy's
  model, a black rectangle over the right half). Like the real microcode
  (checked with the low-level RSP on the Mac), any end-of-list command now
  ends them; `f5-sublista-vieja` restores the old behaviour. One of those
  bogus colour images, at 0x900000, outside the 8 MB of RDRAM, 455 pixels
  wide, was followed by the first depth upload with an odd source width, and
  the RSX stopped right there. Colour images that start outside RDRAM are now
  ignored (the RDP has nowhere to write them), and depth uploads always use
  an even source width (the extra texel never reaches the output).
  `rsx-sin-blindaje` turns this off for comparison. If the RSX still hangs,
  Salsa64 now logs the system's RSX exception (if any), the command buffer
  around GET and PUT and the last rsxkit operations with their offsets, and
  leaves `rsx-colgado-<field>/` (command buffer, the task being translated
  and the black box) on disk before quitting; it no longer feeds or waits
  for the RSX on the way out. An experimental `rsx-colgado` flag reboots the
  console instead of returning to the XMB.
- **Yoshi's Story: camera stutter when walking fast.** The game draws its
  8-bit background layers with the RDP and reads them back from RAM in the
  next task, so the emulator waited for the RSX to draw and copy them back on
  every such task (8–13 ms each, the whole RSX queue and the CRT filter in
  front). Those layers are now rasterised directly into RAM by the PPE (1:1
  CI8 tiles, no filter, no blending; anything else falls back to the RSX for
  that task), so there is nothing to wait for. The common case (16 × 16 tiles
  copied 1:1) goes through an integer row-copy path; only odd triangles use
  the generic floating-point rasteriser. `gfx-capas8-rsx` in
  `/dev_hdd0/salsa64/` restores the RSX path; the log line "capas de 8 bits en
  el PPE" counts tasks, pixels, time per path and waits avoided.
- **Yoshi's Story: coloured dots on the edges of the background layers.** The
  RSX copied those 8-bit layers to RAM with a linear filter at 1:1, which on
  this hardware blends a fraction of the neighbouring pixel; the red channel is
  a palette index there, so an index off by one became a stray colour at every
  edge. 1:1 copies now use nearest sampling (`gfx-copia-lineal` in
  `/dev_hdd0/salsa64/` restores the old filter for comparison).
- **Yoshi's Story: 8-bit background layers are primed from RAM.** The game
  draws its sky, hills and trees into three 336 × 256 8-bit images in strips;
  when the RSX surface of such a layer was new or stale it started from
  garbage, and the next copy to RAM carried that garbage into the rows the RDP
  had not redrawn (black or old strips in the background). The layers are now
  primed from RAM with the full 8-bit index, as 16-bit images already were
  (`gfx-big8-sin-cebar` in `/dev_hdd0/salsa64/` disables it). The log line
  "copias big8" now also reports copies made, pixels changed and layers primed.
- **F-Zero X: distance fog on the track.** The game fogs the track with the
  blender in 1-cycle mode (`FOG * shade alpha + input * (1 - shade alpha)` with
  FORCE_BL, othermode L `0xFA006220`); the HLE only recognised that mix in
  2-cycle mode, so the far track showed no fog and the course could be seen
  building up at the horizon. Measured against the console (ares, demo races):
  the far track went from 20-66 units darker than the console to within a few
  units; the horizon haze still arrives later than on the console because the
  shade (and the fog factor) is interpolated with perspective correction
  instead of screen-linear as the RDP does. `sin-niebla-1ciclo` restores the
  old behaviour.
- **Forsaken 64 (USA): black screen after the intro** (#55). Right after the
  intro the main thread writes back the data cache and sends a request to its
  controller thread, which at that moment is waiting for the Serial Interface
  to finish a write; libultra takes the request as the end of that write, the
  request is lost and the main thread waits for its answer forever. On the
  console the write-back of ~480 dirty cache lines takes long enough for the
  write to finish first; with the data cache write-back cost above (and the
  new Serial Interface timing) it does here too.
- **Star Wars: Shadows of the Empire ran too fast** (#40). The game advances
  its logic once per rendered frame and does not wait for the vertical blank;
  the emulated graphics task finished in 0.3 ms, so it rendered about 75
  frames per second. A new RCP pacing model estimates how long the real RSP
  would take for each graphics task from what the HLE did (display list
  commands, triangles and vertices, per microcode family) and does not end
  the task earlier. It was calibrated against ares on 62 tasks of Super Mario
  64, Shadows of the Empire, GoldenEye, F-Zero X and Zelda: Ocarina of Time
  (average error 2–5 %). In the Battle of Hoth the game now draws about 47
  frames per second (94 graphics tasks/s) against 42 in ares and 75 before;
  the control games run as before or closer to ares, never slower. ares does
  not time the RDP, so heavy fill-rate scenes on a real N64 can be slower
  still. On by default only for Shadows of the Empire; `rsp-ritmo` turns it on
  for every game, `rsp-ritmo-<code>` (the 4-character cartridge code shown in
  the log) for one game, `sin-rsp-ritmo` turns it off, and the file may hold a
  percentage (150 = 50 % slower RCP; about 37 frames per second in Hoth, 200
  about 31). Details in `docs/ritmo-rcp.md`.
- **Vigilante 8: 2nd Offense: explosions, smoke and the sun glare drew as
  black squares.** The game paints each of them twice with the same quad: a
  `MEM·A_IN` pass that darkens the background through the texture and an
  additive `IN·A_FOG + MEM` pass (`G_RM_ADD`) with the colour texture. Both
  were approximated as `IN·α + MEM·(1 − α)`, which with the opaque texture
  alpha of the second pass left a black square. The RDP blender products the
  RSX can do directly (`dst·α`, `src + dst·α`, `src·(1 − α) + dst·α`,
  `src + dst`) now go through as such; `mezclas-viejas` restores the previous
  classification for comparison. The game also renders
  about 1.3 frames per field because the emulated RDP finishes almost at
  once; `rsp-gfx-ciclos` (a number: the minimum length of a graphics task in
  N64 cycles, default 30000) is a test switch to measure RDP pacing on the
  console.
- **Command & Conquer** (#38): limit oversized S2DEX backgrounds to their
  scaled image; `s2dex-marco-viejo` restores the previous repetition.
  Validated with synthetic lists and the real microcode.
- **Indiana Jones** (#39): keep a stable window when alternating complete
  interlaced frames. `vi-campos` restores the previous presentation.
- **Vigilante 8** (#30): decode YUV16 textures instead of returning black;
  `sin-yuv` restores the old path. The menu background uses this format;
  the separate character/missile defects remain open.
- **Memory between games** (#52/#47): release the 9 MiB save-state snapshot
  after its writer stops. `memoria-estados-vieja` keeps the old allocation.
  ROM loading now distinguishes insufficient memory from read errors,
  including ZIP files, and PS3 log sizes no longer use unsupported `%zu`.
- **Donkey Kong 64 (USA): black screen with music halfway through the new-game
  intro** (#51). When the intro switches scene, the emulated CPU (faster than
  the real one) fit two game frames in the same video retrace; the game
  measured 0 retraces, divided 0 by 0 with floating-point exceptions enabled
  and its main thread stopped. The emulator now rewrites, in RAM and only when
  the original code is there, the cutscene path of that measurement so it
  keeps a minimum of 1, as the game already does during gameplay. Only the
  USA retail ROM is affected; the log says "arreglo de juego" when it applies.

## [0.2.14] — 2026-10-28

### Added
- **Button icons in the menus:** the footer hints, panel help, pause menu and
  dialogs show DualShock 3 button icons (cross, circle, square, triangle, d-pad,
  START, SELECT, L1/R1...) instead of text symbols, and the controls page draws
  the N64 buttons with icons (A blue, B green, red Start, gray Z/L/R, yellow C
  buttons). Icons are Kenney "Input Prompts" (CC0). Translations are unchanged;
  if the icons fail to load the old text symbols are used.
- **RetroAchievements** (off by default): achievements for N64 games through the
  official rcheevos library (v12.5.0, MIT, vendored in `third_party/rcheevos`).
  Options > RetroAchievements, account sign-in with the system keyboard (only the
  token is stored, in `ra.cfg`; never the password), "RA: N achievements (X
  earned)" when a game opens, a notice when one unlocks, an achievements list in
  the pause menu, and an optional Hardcore mode (no save states, no cheats).
  Network requests run on their own thread over the PS3 HTTPS stack. See
  `docs/retroachievements.md`. Texts are machine-translated in all 18 languages.
- **ZIP ROM loading** (thanks to **kadorna1**, #48): the browser detects `.zip` archives, reads the embedded N64 header for the game list, and extracts the first supported `.z64`/`.v64`/`.n64` member using zlib. Deflate and stored entries are supported; full extraction checks the uncompressed size and CRC-32 before launch.
- **ROMs of more than 64 MB** (hacks and homebrew; up to the 252 MB the cartridge
  bus can address, issue #13): ROMs bigger than 64 MB are no longer rejected.
  They stay on the disk and the emulator reads 256 KB pages on demand into a
  32 MB LRU cache (PI DMA, CPU reads of the cartridge domain, the HLE boot and
  the recompiler's instruction fetch all go through it). ROMs of 64 MB or less
  still load entirely into memory, as before. z64, v64 and n64 byte orders are
  all accepted. The log reports the mode, the page and cache sizes and, every
  5 s, cache misses and disk read times. A `rom-paginada` file in the data
  folder forces the paged mode for any ROM, and `rom-cache` sets the cache size
  in MB. Tested on a real PS3 with B3313 (96 MB) at full speed. The disk is read
  on a background thread: a miss queues the next pages too, so the emulation
  thread no longer stops for the hard disk unless the data it needs is not on
  its way yet (a `rom-lectura-sincrona` file in the data folder goes back to
  blocking reads). On B3313 normal play does not touch the disk; entering a
  new area can still wait a few hundred ms when the hard disk is slow to seek.
- **"Silk" menu background** (Options > Background: Waves → Silk → No): three
  translucent fabric ribbons that twist and ripple, with glowing edges where
  the cloth turns sideways, small light specks drifting along them and a
  gradient that follows the console clock. An SPE computes the cloth every
  frame and the RSX draws it with additive blending: 3.4–3.7 ms of RSX and
  5.4 ms of one SPE at 1080p on a real PS3, nothing on the PPE. Own code, no
  firmware data. See `docs/fondo-seda.md`.
- **Cartridges in the game list:** each game is shown as an N64-style
  cartridge (a render of "Stylized N64" by AlessandroFortuna, CC BY 4.0,
  without any Nintendo artwork) with its box art on the sticker. The selected
  game is a real 3D model drawn by the RSX that slowly turns (about 0.2 ms of
  PPE per frame). Box art thumbnails are cached on disk in `covers/.mini`
  (about 2 ms to load after the first time), the covers folder is indexed in
  memory, PNGs are inflated with zlib, and nothing new is decoded while you
  scroll fast. Without box art the sticker stays blank.
- **CRT-style startup sequence:** instead of ~2 s of black screen, the app
  opens with a CRT power-on (dot, line, flash), TV snow with a green
  "VIDEO1" / "No Signal" on-screen display and a VCR-style date and clock,
  then the Salsa64 logo and a fade into the menu, all through the Trinitron
  filter. It runs while the font, settings and ROM list load (it does not
  add time) and lasts about 3 s; X or O skips it. The file
  `/dev_hdd0/salsa64/sin-intro` turns it off; `intro-espera` (ms) adds a
  black wait before the power-on for slow displays (0 by default).
- **One HDMI renegotiation instead of two at startup:** the video is now
  configured with exactly the values the XMB left (including its buffer
  pitch), so monitors that blank on every mode change blank only once.

### Fixed
- Opening a 64 MB ROM right after selecting it could fail once and return to
  the menu while a box art was being decoded; the cover thread now pauses and
  frees its memory before a game opens, and the ROM allocation is retried
  once after releasing the menu's memory.
- Audio HLE no longer reports one multi-megabyte write span when its fixed
  range list fills, avoiding needless code invalidation and recompilation in
  games with scattered audio buffers such as B3313. `audio-rangos-viejos`
  restores the previous range summary for comparison.

## [0.2.13] — 2026-10-26

### Added
- **PS3 system font for all text, and Japanese, Korean and Chinese menus:**
  menus, pause, notices and option panels now use the console's own XMB font
  (read from the PS3 at run time), with proportional spacing, word wrapping
  and a glyph cache. Japanese, Korean, Traditional Chinese and Simplified
  Chinese are offered (and picked by "Automatic") only when the system font
  loads; otherwise they fall back to English as before. The built-in 8x13
  font remains as a fallback, and the file `/dev_hdd0/salsa64/fuente-propia`
  forces it.
- **Eight more languages and an "Automatic" setting:** Dutch, Swedish, Danish,
  Norwegian, Finnish, Polish, Turkish and Russian (the menu font gains the
  Polish and Turkish letters and basic Cyrillic) join Spanish, English, French, German, Italian and
  Portuguese. "Automatic" (the default for new setups) follows the console's
  system language; languages without a translation fall back to English.
  An explicit choice already saved in `menu.cfg` is kept. Every on-screen
  text, including download, save-state and region messages, is now
  translated, and `make test-host` checks that none is missing.
- **Rumble Pak and live accessory selection:** choose Controller Pak, Rumble
  Pak, Transfer Pak or no accessory globally, per game and per controller
  from the pause menu. DualShock 3 vibration stops on pause and exit (thanks
  to kadorna1, #45/#46).
- **Experimental Transfer Pak:** load a validated `<n64-rom>.gb` or `.gbc`
  sidecar, use ROM-only/MBC1/MBC2/MBC3/MBC5 cartridges, persist their SRAM
  and keep Transfer Pak state in save states (thanks to kadorna1, #45/#46).

### Fixed
- **LEGO Racers:** cars and scenery no longer disappear when its 32/64-pixel
  auxiliary targets and 320-pixel framebuffer share one depth image. Depth
  clears now wait for the colour target they precede; `zclear-inmediato`
  restores the previous path for comparison. The game is also recognized as
  using no save chip, without affecting its Controller Pak support (#34).
- XBUS RDP lists no longer share decoded-texture cache storage with an
  in-flight graphics SPE task (`texturas-xbus-viejo` restores the old path).

### Changed
- **Faster CRT filters:** the filter output is reused when a frame repeats and
  full frames skip one copy, so the RSX does less work per frame (about 23 %
  less GPU time in Diddy Kong Racing on a real PS3).

## [0.2.12] — 2026-10-26

### Added
- The log names each graphics microcode a game uses, with its version text
  and CRC-32, so a bug report shows the exact variant.

### Changed
- **More room in the graphics SPE:** the colour combiner returns its results
  through output parameters instead of large structs, which frees about 2.5 KB
  of SPU stack (thanks to kadorna1, #41). The output is identical.
- Builds with GCC 13 and warnings enabled, and the directory scan test no
  longer depends on the file system order (thanks to Manfline).

### Changed
- CRT cache now reuses screen buffers without copying complete frames; on-console self-test checks 192 cases with a one-level RGB tolerance.

### Added
- CRT output cache with an exact on-console A/B self-test and an original-path switch (hardware validation pending).

### Fixed
- **Monaco Grand Prix:** no longer freezes after choosing Single when a slow graphics task finishes; SP interrupt delay is capped at one video field.
- **Midway's Greatest Arcade Hits Volume 1:** arcade games are no longer black; Fast3D Sprite2D commands now render their textured rectangles.
- **Cruis'n USA:** no longer freezes on the copyright screen; SI DMA now stays
  busy until its delayed completion and raises the interrupt afterwards.
- **Cruis'n Exotica:** recognize Eurocom's F3DEX2 variant and execute its
  counted display-list blocks instead of dropping most geometry and state.

## [0.2.11] — 2026-10-25

### Added
- **Automatic internal scale:** choose «Automatic (up to N×)» in Image. When
  the RSX cannot keep up (frames dropped while the game itself runs at full
  speed) Salsa64 steps the scale down and remembers the stable scale for that
  game, so next time it starts there. It never steps down when the game is
  slow because of the CPU, and fixed scales are never changed (you get a
  notice instead). The choice is saved per game from the pause menu.
- When the RSX stops responding, the log now shows the commands it was about
  to run, to make those hangs easier to diagnose.

### Changed
- **The maximum internal scale is now 4×.** At 5× the RSX cannot keep up in
  many games (for example Pokémon Snap), and 4× looks almost the same on a
  1080p TV. A 5× setting saved before is read as 4×.

### Fixed
- **Star Wars: Shadows of the Empire:** the intro and gameplay are no longer
  black; its microcode's quads were being dropped.

### Known issues
- Pokémon Snap still slows down with a CRT filter at 3×.
- Star Wars: Shadows of the Empire: the later Hoth sections run at 40–52 fps
  (CPU-bound).
- Conker's Bad Fur Day: the Great Mighty Poo does not open his mouth, so the
  boss cannot be beaten (#3).
- Donald Duck: Quack Attack can soft-lock when talking to a nephew (#16).

## [0.2.10] — 2026-10-22

### Added
- **Acclaim microcode HLE (F3DEX2ACCLAIM):** Turok 2 and Turok 3 now show
  their dynamic point lights, such as shots lighting up the walls (confirmed
  on a real PS3, same speed). Runs in SPE 0 image 5 together with Turbo3D.
  On by default; `sin-acclaim` turns it off. Translated from GLideN64
  (GPLv2), see `docs/THIRD_PARTY.md`.
- **Turbo3D microcode (HLE):** Dark Rift now draws its fighters, textures and
  animations. It runs in a new SPE 0 image (5) together with the F3D tasks
  the game also sends. On by default; `/dev_hdd0/salsa64/sin-turbo3d` turns it
  off. Based on Glide64mk2 (GPLv2+), see `docs/THIRD_PARTY.md`.
- **Affine texture scale for F3DEX2 loaded from S2DEX:** fixes the garbled
  letters of the Resident Evil 2 OPTIONS menu. On by default;
  `sin-afin-cadena` turns it off.
- **Vector clipping in the SPE (optional, `gfx-recorte-vector`):** F3DEX2
  vertices are tested against the clip planes four at a time. About 12 % less
  time per vertex in the batch on Road Rash; not a global FPS gain.

### Changed
- **Faster F3DEX2 in the SPE:** vertices without lights or TEXGEN that carry
  fog are processed four at a time. `gfx-escalar` keeps the old path; a
  self-test compares both bit for bit.
- **Recompiler and busy-wait loops:** dropping a busy-wait probe no longer
  throws away the translated block. Road Rash 64 used to recompile the same
  code tens of thousands of times; on a real PS3 it now invalidates no blocks
  at all. `jit-invalidacion-vieja` restores the old behaviour for comparison.
- **Auto low-level RSP mode** keeps the HLE microcodes of Perfect Dark and
  Conker.
- Recompiler fingerprint diagnostics are now optional (`jit-diag`) and cost
  nothing when off.

### Fixed
- **Yoshi's Story (and other CIC 6106 games, such as F-Zero X):** the HLE boot
  now runs the first stage of the cartridge's own IPL3 once, so low RDRAM
  holds what the real console leaves there (checked byte for byte against
  ares). Without it Yoshi's Story never freed its objects: enemies and items
  vanished, and the game hung when hitting a text block or after dying. All
  of that is fixed on a real PS3.
- **Turok: Dinosaur Hunter and Turok 2: Seeds of Evil** now start (they did
  not in 0.2.9) and run at full speed on a real PS3.
- **S2DEX 1.x:** the width of `G_SETOTHERMODE_H/L` was off by one (the 2.x
  encoding was used). `gfx-s2dex-modos-viejos` restores the old behaviour.
- **Pokémon Snap:** no more draw-order problems in the scenes tested on a real
  PS3.
- **Star Wars Episode I: Racer:** the LucasArts intro video no longer jumps
  (it was clipped by the RDP scissor of its task).
- **Memory between games (optional, `memoria-hilos`):** releases the newlib
  buffers of the emulation thread when a game closes.

### Known issues
- Yoshi's Story still shows some wrong dots and colours in its graphics.
- Resident Evil 2: thin vertical seams between the tiles of the OPTIONS menu
  (text fully readable); the statue room runs at 65-80 %.
- Racer and Road Rash 64 do not reach full speed in races (about 47 and 51
  fields/s). Star Wars Episode I: Battle for Naboo runs at about 31-33
  fields/s in heavy scenes.
- Pokémon Snap slows down hard at 5× internal resolution (down to about 40
  fields/s: the RSX cannot keep up); up to 4× it runs at full speed.
- F-Zero X has no fog.
- Mario Tennis and some Rogue Squadron menu logos have no transparency.
- Conker's final boss lacks textures and is slow. Mortal Kombat 4 has no
  health bars. Star Fox 64: the menu duplicates options. Majora's Mask:
  lighting. NBA Showtime runs sped up.
- Zelda: Ocarina of Time: torch glows show through walls, and some display
  lists are cut by the command budget.
- ROMs larger than 64 MB are not supported.
- Perfect Dark slows down in scenes of more than about 2300 triangles.
- Frame interpolation (experimental) still leaves a trail on 2D sprites (#17).

## [0.2.9] — 2026-10-19

### Added
- **Launch a ROM from outside** (issue #25): Salsa64 opens the ROM passed as
  `argv[1]` or, failing that, the first line of
  `/dev_hdd0/game/SLSA00064/rom.txt` (deleted once read), without showing the
  menu, so webMAN MOD and other frontends can start N64 games from the XMB. A
  missing ROM falls back to the normal menu with a notice. See
  `docs/uso-ps3.md`.

### Changed
- **Faster ROM list:** with a few hundred ROMs the menu spent about 8 s reading
  their headers at start-up and again every time it came back from a game. The
  headers are now cached in `romlist.cache` (name, size, date and the 64-byte
  header of each file), so start-up only reads the files that are new or
  changed, and coming back from a game does not read the folder again unless
  its date changed. L3 still rebuilds the list from the disk (and rewrites the
  cache). A damaged or older cache is ignored and rebuilt.

### Fixed
- **Memory between games:** the frame-interpolation tables of each game are
  released when it closes. Before, after about 40 games in a row the biggest
  ROMs (Resident Evil 2, Conker) failed to start until Salsa64 was restarted.
  The log now reports the memory of each subsystem when a game closes.
- **Interlaced video:** the image no longer moves up and down one line every
  field in Star Wars: Rogue Squadron.
- **Render-to-texture alpha:** 16-bit render targets that are not the VI image
  are created with an alpha channel, so what a game draws with alpha 0 and then
  uses as a texture stays transparent. The file
  `/dev_hdd0/salsa64/sin-alfa-destinos` turns it off.

### Known issues
- Mario Tennis and some Rogue Squadron menu logos still have no transparency
  (they do not go through render-to-texture). The Racer video jumps by about
  half a screen.
- Yoshi's Story can hang when hitting a block, and shows vanishing objects and
  dots on textures. Conker's final boss shows but lacks textures and is slow.
- Mortal Kombat 4 has no health bars. Pokémon Snap: texture order, slow intro.
  Star Fox 64: the menu duplicates options. Majora's Mask: lighting.
- NBA Showtime runs sped up. Road Rash 64 does not reach full speed.
- Zelda: Ocarina of Time: torch glows show through walls.
- Resident Evil 2: the letters of the OPTIONS menu are garbled, and the statue
  room runs at 65-80 %.
- ROMs larger than 64 MB are not supported.
- Perfect Dark slows down in scenes of more than about 2300 triangles.
- Frame interpolation (experimental) still leaves a trail on 2D sprites (#17).

## [0.2.8] — 2026-10-18

### Added
- **Restart from the pause menu:** two new rows, both with an "Are you sure?"
  box. "Restart (Reset button)" does what the console's Reset button does: the
  game first gets the pre-NMI interrupt (the one libultra uses to save and
  mute the audio), half a second later the NMI resets the CPU and the RCP
  with the RDRAM kept (the program is copied again from the ROM and the game
  sees a soft reset). "Restart from scratch" closes the game completely
  (saves are written first) and opens it again without going back to the list.
  They work with the HLE, the RSP in low-level mode, interpolation and the
  recompiler. On a computer, `salsa64-run --reset-at N` and `--nmi-at N` do
  the same at a given field; Super Mario 64, Zelda: Ocarina of Time and
  GoldenEye 007 go back to their logos after either.
- **MusyX audio** (issue #11): the games that use Factor 5's MusyX engine
  (v1 and v2) used to run mute because their audio task is not a command list.
  They now have sound: Hydro Thunder, Rush 2049, NBA Showtime, Resident
  Evil 2, Star Wars: Rogue Squadron and Indiana Jones and the Infernal
  Machine. Gauntlet Legends and Tarzan use the same engine but have not been
  tried. Checked on a computer against the real microcode, the mix matches
  byte for byte (Indiana Jones differs by 1 in one channel in 49 of its tasks).
- **Diddy Kong Racing is playable:** HLE for Rare's own graphics microcode
  (F3DDKR): logos, intro, title, menus, track selection and the races
  (Ancient Lake, the Timber's Island adventure) draw correctly in the test
  build on a computer; the console run is still to be confirmed. Jet Force
  Gemini and Mickey's Speedway USA use a variant of the same microcode that is
  recognised too and now run with every command implemented (the title
  screens, menus, cut-scenes and the first race of Mickey's Speedway draw in
  the test build; the start lights of that race change from red to green).
  Neither has been tried on a console yet.
- **Killer Instinct Gold draws:** the game writes part of every frame (the
  stage background and the frames around the screen) as RDP lists from the CPU
  and does not start its graphics until it gets the RDP interrupt of those
  lists. They are now drawn by the graphics SPE, in the same state and with
  the same texture cache as the rest of the frame, and the interrupts of the
  RDP are queued so that two close ones are not lost.
- **Star Wars: Rogue Squadron draws (HLE, 60 fields/s):** HLE for Factor 5's
  graphics microcode, translated from GLideN64 (GPLv2; the combined binary is
  distributed as GPLv2). Ships, terrain, menus and the missions draw; the
  ship in the mission and the double z clear are handled.
- **Indiana Jones and the Infernal Machine is playable (HLE):** same family of
  microcode, with the first mission running from the logos to the levels.
  The HLE also covers **Star Wars Episode I: Battle for Naboo**, which shares
  the microcode but **has not been tried** (no backup to test).
- **Resident Evil 2 is playable:** the intro videos (HLE of the RSP's video
  tasks, byte for byte the real microcode), menus, characters drawn with the
  background's z, and the "YOU DIED" screen. Uses S2DEX 2.x as separate tasks.
- **Yoshi's Story draws:** HLE of S2DEX 1.x as separate tasks and the 8-bit
  background layers (sky) copied to RDRAM with their index bits.
- **Earthworm Jim 3D and Mace: The Dark Age** draw correctly (the leftover
  Z_SOURCE_SEL from the RDP no longer put the whole scene at one depth).
- **Castlevania** (drawing order) and **F-Zero X** (drawing order) fixes;
  **FIFA 99** now starts the match; **Conker's Bad Fur Day:** the final boss
  now shows up.
- **Zelda: Ocarina of Time, Lens of Truth** draws (the z mask keeps its alpha
  for the alpha test and the RSX writes z without testing it).
- **Animal Forest** (Animal Crossing translation, issue #18): its microcodes
  are now recognised by their code CRC. Not tried (no backup).
- **Guard against the CPU:** late copies from the RSX to RDRAM no longer
  overwrite what the CPU wrote after the task (fixes Indiana Jones and Killer
  Instinct Gold, among others); copies never write the colour image to
  address 0.
- **Audio identical to the real microcode in 99.87 % of the tasks** (51 136
  tasks in 21 games, compared byte for byte; it was 13.1 %): pitch changes,
  mixing, envelopes, the filters and the MP3 decoder follow the rounding of
  the real RSP. The JPEG task of Ocarina of Time and Pokémon Stadium 1 and 2
  (pre-rendered backgrounds and images) is now exact too: 0 pixels different
  out of 1 075 200 (it was about 1 %). Listen for it in Conker, Perfect
  Dark, Banjo-Tooie, Majora's Mask, Yoshi's Story and 1080° Snowboarding.
- **Frozen-game detector:** if a game that was reading the controllers and
  drawing stops doing both for 6 seconds while the video keeps running, the
  log says what the hardware and the CPU were waiting for and
  `/dev_hdd0/salsa64/` gets a RAM dump (`cuelgue-<field>.rdram`, up to three
  per game) to attach to the bug report.

### Changed
- **The graphics SPE program comes in three images**, one per family of
  graphics microcodes (first generation: Fast3D, F3DEX 1.x and Rare's
  variants; F3DEX2 with S2DEX2 and F-Zero X's variant; Conker's). The one each
  game needs is loaded with its first graphics task and checked against the
  PPE before use. Each image has the texture cache back at 1024 textures and
  room in the SPE's local store for the microcodes still missing (S2DEX,
  Factor 5). The log says which image each game loaded, its size and its
  stack headroom.

### Known issues
- Memory is not fully released between games: if a big game (Resident Evil 2,
  Conker) does not load after playing many in a row, quit Salsa64 to the XMB
  and open it again.
- Yoshi's Story can hang when hitting a block, and shows vanishing objects and
  dots on textures. Conker's final boss shows but lacks textures and is slow.
- Mario Tennis and some Rogue Squadron menu logos have no transparency. Image
  shake in Racer's video and Rogue Squadron's menus.
- Mortal Kombat 4 has no health bars. Pokémon Snap: texture order, slow intro.
  Star Fox 64: the menu duplicates options. Majora's Mask: lighting.
- NBA Showtime runs sped up. Road Rash 64 does not reach full speed.
- Zelda: Ocarina of Time: torch glows show through walls.
- Resident Evil 2: the letters of the OPTIONS menu are garbled, and the statue
  room runs at 65-80 %.
- ROMs larger than 64 MB are not supported.
- Killer Instinct Gold: the second fight froze in an earlier version; the
  frozen-game detector is included and it is not confirmed fixed.
- Perfect Dark slows down in scenes of more than about 2300 triangles.
- Frame interpolation (experimental) still leaves a trail on 2D sprites (#17).

## [0.2.7] — 2026-10-16

### Added
- **Controller Pak:** emulated, with one `.mpk` file per game (four paks, one
  per controller) and a per-game «Accesorio» option; controller 1 carries a
  Controller Pak by default. Games that save only to the Pak (Mario Kart 64
  ghosts, Rush 2049, Hydro Thunder, Donald Duck…) can now save, and games
  that kept polling for a missing Pak no longer slow down (1080 Snowboarding).
- **Button remapping from the menu** (issue #6): Options → «Controles» edits
  the mapping for every game, and each ROM's settings edit the mapping for
  that game. Press the DualShock 3 button (or two at once, e.g. Z on L2 + R2)
  for each N64 button, and choose which stick moves the N64 stick; the C
  buttons come from the other one.
- **ROM list with folders** (issue #10): subfolders, thousands of entries
  instead of 256, sorting by header title or by file name, file names for
  titles that are not Latin-1 (Japanese ROMs), L3 to refresh the list without
  leaving, and going up returns to the folder you came from. Salsa64's own
  data folders (saves, states, covers…) are no longer listed.
- **The left stick moves the menu** like the d-pad, for controllers with a
  worn-out d-pad.

### Fixed
- **Wave Race 64:** it uses its own variant of the graphics microcode, which
  was read as the standard one (textures «like rays from the centre»).
- **Pokémon Stadium:** the «Game Pak Check» screen (player 1 panel, «OK?» and
  the WARNING notice, which turned into noise after going back to the title),
  the ghost of «VS Event Battle» on «Please Select», and a crash in VS.
- **Donald Duck: Quack Attack / Goin' Quackers** was given an EEPROM it does
  not have (it saves only to the Controller Pak).
- **FPU:** CVT.W / CVT.L follow the rounding mode and MUL.S stays in
  recompiled code instead of falling back to the interpreter.

### Faster
- **CPU recompiler:** libultra's idle loop (waiting through the scheduler) is
  now skipped like any other wait loop, and the COP0 writes most games repeat
  stay in recompiled code. On a real PS3: **Doom 64 21 → 60 fields/s,
  NFL Blitz 26 → 60, 1080 Snowboarding 31 → 60, Star Wars Episode I: Racer
  32 → 46**.

### Known issues
- Frame interpolation (experimental) makes 2D sprites and UI layers flicker or
  leave a trail (Mario Kart 64 items, Smash menus; issue #17). It can be turned
  off per game.
- Games with **MusyX** audio have no sound yet (Hydro Thunder, Rush 2049, NBA
  Showtime, Gauntlet Legends…; issue #11). It is the next item.
- Diddy Kong Racing, Indiana Jones, Rogue Squadron and Resident Evil 2 do not
  run yet (no HLE for their microcodes). The Rumble Pak is not emulated.

## [0.2.6] — 2026-10-15

### Added
- **Banjo-Tooie is playable:** the PIF now answers the game's CIC-6105
  challenge, which it uses to unlock its data.
- **F-Zero X races:** the game probes the 64DD (open bus on the cartridge
  port) and switches graphics microcode in the middle of every frame; both
  are handled, so the cars, the HUD and the cars' highlights (F-Zero's own
  F3DFLX microcode, alpha from a lookup table) are drawn.
- **Frame interpolation (experimental, off by default):** games that run at
  20 or 30 fps can show in-between frames rebuilt from the geometry, for
  smoother motion. «Interpolación: Por geometría (exp.)» in Options, per
  game, or live from «Imagen» in the pause menu. It looks best at 3× / 720p.
- **○ in the game list exits to the XMB**, with a confirmation, so you no
  longer need to scroll to the end of a long list.
- **Version, title and APPID** shown in the menu, the pause menu and the
  first line of the log (handy for bug reports).
- **Conker's shadows** (render to an 8-bit texture), **Pokémon Snap** (exact
  primitive depth, leaves with alpha threshold), **Kirby 64** (enemies were
  missing their bodies; S2DEX palettes) and **Pokémon Stadium 2** (its JPEG
  images).
- **Black box** for bug reports (SELECT + L2 with the `cajanegra` file): the
  last graphics tasks and frames, compressed in the background.
- **CFW fallback:** if the CPU recompiler cannot run generated code on a
  console (issue #2), Salsa64 falls back to the interpreter instead of
  hanging.

### Fixed
- **Console freeze in F-Zero X** at 5× (course selection): an off-screen
  scissor went past the RSX limit. Also, no wait on the GPU can be endless
  any more: after 2 s the log says what is stuck, the PS button keeps
  working, and after 12 s the game saves and closes on its own.
- **Large ROMs (40–64 MB) failed to open** («No se pudo leer») after playing
  other games in the same session: Conker, Paper Mario, Pokémon Stadium 2,
  Smash Remix, Resident Evil 2. The ROM now goes outside the fragmented heap.
- **Star Fox 64:** coloured noise at the top during the «Nintendo presents»
  logo.
- **Super Mario Odyssey 64 (hack)** ran far too fast (an unusual VI_V_SYNC).
- **Interpolation turned on mid-game** no longer slows the graphics SPE down
  (stretched audio in heavy scenes).

### Faster
- CPU recompiler: compact FPU code, a cached base register for memory
  accesses and a JR/JALR target cache (GoldenEye and Perfect Dark 2–8 %
  faster in the heaviest scenes).

### Known issues
- F-Zero X: some background objects are drawn over the track (shading
  interpolation after clipping; to be fixed in a later version).
- Diddy Kong Racing, Indiana Jones, Rogue Squadron and Resident Evil 2 do
  not run yet (their graphics microcode has no HLE); low-level RSP emulation
  is in progress.
- Super Mario Odyssey 64 may crash after a while (under investigation; the
  log now records what overwrote the code).
- GoldenEye: thin blue lines at some seams. An experimental fix (drawing the
  anti-aliased edges) is included but off by default, because on real
  hardware it made some model polygons overlap; the `aristas-aa` file turns
  it on.

## [0.2.5] — 2026-10-13

### Fixed
- **Console freeze in Options:** with «CRT Trinitron» chosen as the filter,
  moving to the Filter row in Options (main menu) froze the PS3 (its help
  text was missing an end marker and the menu read past it). The pause menu
  was not affected.
- **Fewer copies back to N64 memory:** after a change in the screen width
  (Perfect Dark's intro, GoldenEye after the mission screens) every frame
  was taken for an auxiliary image and copied back, with the emulation
  waiting each time; Perfect Dark's intro went from about one copy per
  frame to almost none.

### Added
- **Mask density** for the «CRT máscara» filter: Normal or Fine (smaller,
  closer dots: 9 px triads instead of 12), in Options, per game and live in
  «Imagen». The choice made in Options is now saved like the rest.

## [0.2.4] — 2026-10-13

### Fixed
- **Super Smash Bros.** (and Smash Remix) no longer freeze on the first KO.
  The game keeps code and pointers in the margins of its z-buffer and, in the
  KO, draws a mask there; copies back to N64 memory now write only what the
  RDP could have drawn (the box of each drawing inside its scissor). The
  `fbfx.cfg` workaround is no longer needed.
- Stretched textures when a tile with clamp and mask did not fit the decode
  area (Smash: Hyrule Castle stage, Kongo Jungle leaves): one period of the
  mask is decoded and repeated instead of cutting rows.

### Added
- **Five CRT filters** made for Salsa64, still experimental: shadow mask,
  a small Trinitron TV over RGB, S-Video or composite, and a big Trinitron
  (Sony KV-29). In Options, per game and live from «Imagen» in the pause
  menu. While a CRT filter is on, the internal scale is capped at 3× (no
  visible difference above it). Measured on a real PS3 at 1080p (GoldenEye,
  Dam): 7.4–13.2 ms of RSX per frame, every field presented. A short
  «Aplicando filtro…» notice shows while a filter is prepared.
- Experimental: the signal stage of the S-Video and composite filters on
  two SPEs (`filtros-spe` file, off by default; not faster yet).

### Changed
- `lib/rsxkit`: post-processing reduces the image in two passes (width, then
  height). The CRT filters redo only their final pass when the game repeats
  a frame.

## [0.2.3] — 2026-10-13

### Added — contributed by VIDEOGAMES SCZ
- **Pause menu** (START + SELECT): resume, save/load state, state slot,
  cheats, FPS counter and quit (with the «Confirmar al salir» question when it
  is on). It replaces the old «¿Volver al menú?» box; the SELECT shortcuts
  still work.
- **Cheats** per game: Project64 (`Config/Cheats`, section chosen by the ROM
  CRC, options with `??`) and RetroArch `.cht` files in
  `/dev_hdd0/salsa64/cheats`, found by ROM name or by a similar name of the
  same region. GameShark codes 80/81/A0/A1/F0/F1, D0–D3 and 50. Page in the
  menu (□ on a ROM) and list inside the game (SELECT + □), with the active
  ones remembered next to the `.cht`.
- **Box art** in the menu: PNG files in `/dev_hdd0/salsa64/covers` (any color
  type, interlaced too), by ROM name, game code or a RetroArch-style name.
  Built-in PNG decoder (no zlib needed).
- **Aspect** option: 4:3 (original) or 16:9 (stretched).
- **Simple image filters**: Smooth (as before), Sharp (nearest-neighbour) and
  soft or strong scanlines.
- **Per-game settings** (SELECT on a ROM): aspect, filter and overscan,
  saved in `juegos/<ROM>.cfg`.
- **Six menu languages**: Spanish, English, French, German, Italian and
  Portuguese.
- **Downloads of box art and cheats** from the menu — experimental and
  **off by default** (hidden unless `descargas.cfg` has `activar = si`): the
  PS3 only speaks old TLS and HTTPS certificates cannot be verified. Not yet
  tried on a real PS3.

### Added
- **«Imagen» in the pause menu**: filter, aspect, overscan and internal scale
  of the running game. Filter, aspect and overscan change live on the paused
  frame; the scale applies when the game resumes. «Guardar para este juego»
  stores them as per-game settings.
- **Per-game internal scale** (`escala` in `juegos/<ROM>.cfg`, also on the
  SELECT page): set when the game opens, back to the Options one when it
  closes.

### Changed
- `lib/rsxkit`: the 2D layer can draw an RGBA image slot
  (`rsxkit_ui_image_set` / `rsxkit_ui_image`); it maps 3 MB instead of 2.
- Games with Nintendo's boot code start with the CPU timer where the real
  boot code leaves it (about half a second), so those that waited in the
  controller setup start up to half a second sooner, as on the console.

### Fixed
- **Conker's Bad Fur Day** stayed black after the «For Mature» screen since
  0.2.1: the z-buffer clear written to RDRAM ignored the scissor and
  overwrote game data just past the z-buffer. It is now clipped like the
  RDP does.
- Conker no longer shows 25–30 s of black at start-up.

## [0.2.2] — 2026-10-12

### Performance
- **Smaller recompiled code.** The dynamic recompiler now emits about 23 %
  less PowerPC code per N64 instruction (one shared slow path per block
  instead of one per memory access, a 4-instruction KSEG0 check, 3-word block
  links). On a real PS3 the GoldenEye Dam runs 6–12 % more fields per second
  in its heaviest parts; Perfect Dark 3–7 % faster per instruction.

### Controls
- An N64 button can take one or two DualShock 3 buttons in `controles.cfg`:
  `Z = L2 R2` puts Z on both triggers.

### Diagnostics
- Time sampling of the recompiled code from the second PPE thread
  (`muestreo` file): logs where the emulation time goes, per category and per
  block.
- Optional per-game recompiler profile (`perfil-activo` file; off by
  default): block counts, successors and branch statistics saved in
  `perfiles/`, with no game code in the file.

## [0.2.1] — 2026-10-12

### Fixed
- **Donkey Kong 64:** the camera no longer zooms in and out every ~42 frames.
  The game reads the depth buffer from N64 memory to check whether DK is
  hidden; the depth clear is now written back to memory.
- **Zelda: Ocarina of Time:** Navi's glow and the torch halos are drawn again
  (the game also decides them by reading the depth buffer).
- **Zelda: Ocarina of Time:** no more hitch when entering pre-rendered rooms.
  The JPEG task now runs on a free SPE, vectorized (2.9 ms per room on the SPE
  and 0.3 ms on the emulation thread, down from 68 ms on the PPE).

### Diagnostics
- The presentation thread's time per field is always measured and logged;
  a synthetic load (`carga-hilo1` file) measures its cost to emulation
  (~2.5 % in the GoldenEye Dam).

## [0.2.0] — 2026-10-12

### Games
- **Donkey Kong 64 runs at full speed** (was ~20 %). The recompiler no longer
  flushes its code cache when the game switches the FPU register mode
  (`Status.FR`), which it does ~260 times per second; only blocks that use
  odd FPU registers depend on it and get a twin per mode.
- Donkey Kong 64: interlaced high-resolution logos shown whole, no more
  "zoom" every other frame (both framebuffers at the same scale), zipper
  scene no longer black.
- **Majora's Mask is playable:** 128 KB FlashRAM saves (also stored in save
  states), the end-of-day bell zoom, the Scarecrow dance and Zelda's memory
  blur, and the telescope lens (`MIRROR_ONCE` on the RSX, `G_TP_NONE` at half
  a texel).
- **Perfect Dark:** intro at 60 fps (texture decoding on the SPEs, trimmed to
  what is sampled); rooftop motion blur at full resolution, read straight
  from the RSX instead of decoded on the CPU.
- **Star Fox 64:** no freeze after 5 seconds (framebuffer copies no longer
  overwrite data a DMA wrote later), no static at the top of the screen
  (fills taller than 480 lines split among framebuffers), no flicker in game.
- **Banjo-Kazooie** boots like the real CIC 6103 (its copy protection used to
  scramble the camera planes); intro fade fixed (fog as the blender color).
- Conker's face no longer black (`G_SETCONVERT`, K4/K5). Zelda: lines on the
  N64 logo (TEXRECT clamped per tile) and Link's tunic (mask with mirror
  inside clamp). Mario Kart 64: black area above the Luigi Raceway wall.

### Features
- PS button: the game pauses while the XMB menu is open and resumes cleanly.
- START + SELECT asks before leaving the game («Confirmar al salir» in the
  menu options).
- Per-field presentation trace (`trace-vi` file) and graphics dumps
  (SELECT + R2) for bug reports.

### Performance and stability
- Combiner results are reused on the graphics SPE while their inputs don't
  change.
- Framebuffer copies waited on by the emulation are always translated, so
  they no longer hit the 200 ms timeout (Perfect Dark intro, Star Fox logo).
- No deadlock when a game sends more graphics tasks in one field than there
  are buffer sets (Paper Mario, Mario Party).
- The build checks the graphics SPE stack headroom (at least 1.5 KB).

### Known issues
- Super Smash Bros. (and Smash Remix) freeze on the first KO. Workaround:
  `/dev_hdd0/salsa64/fbfx.cfg` containing `1`.
- Banjo-Tooie, F-Zero X, Diddy Kong Racing and Indiana Jones show a black
  screen; Resident Evil 2 flickers; Doom 64 runs at ~40 %.
- GoldenEye blue seams, Banjo-Kazooie jigsaw screens, Conker square shadows,
  noisy textures in Kirby 64 and Pokémon Stadium 2, stretched textures in
  Pokémon Snap.

## [0.1.0] — 2026-10-11

First public preview.

- Dynamic recompiler MIPS → PowerPC64 on the PPE; graphics HLE (F3D, F3DEX,
  F3DEX2, S2DEX and Rare's microcodes) and audio HLE on the SPEs; runtime
  generated RSX fragment programs for the RDP combiners.
- Output at 720p or 1080p with internal render scale 1× to 5×, changeable
  from the menu without restarting.
- XMB-style ROM menu (`.z64`, `.v64`, `.n64`, internal drive or USB).
- Save states (10 slots per game), FPS counter, EEPROM and SRAM saves.
- Framebuffer effects: render to texture and copies back to N64 memory.
- Full speed: Zelda: Ocarina of Time, Super Mario 64, Mario Kart 64.
  Playable: GoldenEye, Perfect Dark, Banjo-Kazooie, Conker.

[0.2.2]: https://github.com/ricowp81/Salsa64/releases/tag/v0.2.2
[0.2.1]: https://github.com/ricowp81/Salsa64/releases/tag/v0.2.1
[0.2.0]: https://github.com/ricowp81/Salsa64/releases/tag/v0.2.0
[0.1.0]: https://github.com/ricowp81/Salsa64/releases/tag/v0.1.0
