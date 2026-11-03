# Salsa64

A Nintendo 64 emulator for the PlayStation 3 (homebrew, runs on HEN or CFW),
designed around the Cell processor: the PPE runs the VR4300 CPU with a
dynamic recompiler, the SPEs do the vector work (RSP graphics and audio
HLE), and the RSX draws.

Why another one: RetroArch on the PS3 has no working N64 core (the existing
cores need OpenGL, which the PS3 does not offer), and no N64 emulator for the
PS3 uses the SPEs. Both machines are big-endian, so emulated RAM needs no
byte swapping, and the N64's RSP (MIPS + vector unit + local memory with DMA)
maps naturally onto an SPE.

> Most project documentation (`docs/`) is written in Spanish. This README and
> [CONTRIBUTING.md](CONTRIBUTING.md) are in English.

## Status

Work in progress, but already playable on a real PS3:

- **Playable on real hardware:** Zelda: Ocarina of Time and
  Majora's Mask, Super Mario 64, Mario Kart 64, Donkey Kong 64, Paper Mario,
  Mario Party and Super Smash Bros. (also Smash Remix) at full speed; Perfect
  Dark, GoldenEye 007, Conker, Star Fox 64, Banjo-Kazooie, Banjo-Tooie,
  F-Zero X, Kirby 64, Pokémon Snap and Pokémon Stadium 2 playable (GoldenEye
  45–60 fps in the Dam). New in 0.2.6: Banjo-Tooie and F-Zero X, experimental
  frame interpolation, and a safety net so a stuck GPU can no longer freeze
  the console. Full list (Spanish):
  [docs/compatibilidad.md](docs/compatibilidad.md); release notes:
  [CHANGELOG.md](CHANGELOG.md).
- **CPU:** VR4300 interpreter and a MIPS → PPC64 dynamic recompiler on the
  PPE (with TLB-mapped code, FPU and idle-loop detection).
- **RSP:** high-level emulation of graphics microcodes (Fast3D, F3DEX 0.95/1.x,
  F3DEX2 and F3DZEX2, Rare's variants for Perfect Dark and Conker, S2DEX
  backgrounds, the JPEG task of Zelda) on SPE 0, and of audio microcodes (ABI1,
  nead, naudio with MP3, MusyX) on SPE 1. An experimental low-level RSP interpreter
  (real microcode on an SPE) exists for microcodes without HLE.
- **RDP on the RSX:** combiners translated to RSX fragment programs generated
  at run time (there is no Cg compiler on the console).
- **Frontend:** XMB-style ROM menu, DualShock 3 → N64 controller mapping,
  EEPROM, SRAM and FlashRAM saves.
- **Video:** 720p or 1080p output with internal render scale 1× to 5×;
  framebuffer effects (render to texture, copies back to N64 memory, motion
  blur read from the RSX); save states, FPS counter, PS button pause.
- **Filters:** five CRT filters made for this project (shadow mask, a small
  Trinitron TV over RGB, S-Video or composite, and a big Trinitron), still
  experimental; on a real PS3 all five fit in the frame time at 1080p.
- **Accessories:** Controller Pak, Rumble Pak (DualShock 3 vibration) and an
  experimental Transfer Pak (a Game Boy backup next to the ROM), chosen globally,
  per game or per controller.
- **Not yet:** large SRAM saves (768 Kbit, 1 Mbit; recognized but not
  emulated).

Performance numbers are logged in [docs/perf-log.md](docs/perf-log.md). Only
measurements on a real PS3 count; RPCS3 is used for functional tests.

## Using it on a PS3

Full guide (in Spanish): [docs/uso-ps3.md](docs/uso-ps3.md).

1. Download the latest `salsa64.pkg`
   ([direct link](https://github.com/ricowp81/Salsa64/releases/latest/download/salsa64.pkg),
   always the newest version) and install it with the XMB Package Manager
   (HEN or CFW). It shows up as "Salsa64" under Games.
2. Copy your own ROM dumps (`.z64`, `.v64` or `.n64`, any byte order; ROMs
   over 64 MB, such as hacks, are read from the disk in pages) or standard `.zip`, `.rar` or `.7z` archives containing one of those formats to
   `/dev_hdd0/salsa64` (for example over webMAN MOD's FTP) or to a `salsa64`
   folder at the root of a FAT32 USB stick.

   RAR and 7z archives must be single-volume and unencrypted. Encrypted 7z
   headers, password-protected members and multi-volume RAR sets are not
   supported. A 7z solid block and the preceding data in a solid RAR archive
   are limited to 64 MiB.
3. Start Salsa64 and pick a game from the menu.

| N64 | DualShock 3 |
|---|---|
| Analog stick | Left stick |
| C buttons | Right stick |
| D-pad | D-pad |
| A / B | × / □ |
| Z | L2 |
| L / R | L1 / R1 |
| Start | START |

- **START + SELECT** opens the pause menu: resume, save or load a state,
  cheats, «Imagen» (filter, aspect, overscan and internal scale of the
  running game, saved per game if you want), FPS counter and quit. Quitting asks first; the question can be
  turned off in Options («Confirmar al salir»).
- In the game list, **□** opens the cheats of the selected ROM (Project64 or
  RetroArch `.cht` files in `/dev_hdd0/salsa64/cheats`) and **SELECT** its
  own settings (aspect, filter, overscan). PNG box art in
  `/dev_hdd0/salsa64/covers` shows next to the game info.
- Options also set the aspect (4:3 or 16:9), the image filter (smooth,
  sharp, scanlines, or one of the five experimental CRT filters) and the menu
  language: 18 languages (Spanish, English, French, German, Italian,
  Portuguese, Dutch, Swedish, Danish, Norwegian, Finnish, Polish, Turkish,
  Russian, Japanese, Korean, Traditional and Simplified Chinese) or
  "Automatic", which follows the console. Menus use the PS3 system font; the
  Asian languages need it to load.
- The **PS button** pauses emulation and sound while the system menu is open;
  the game resumes where it was when the menu closes.
- Saves go to `/dev_hdd0/salsa64/saves`, one file per game.
- Optional text files in `/dev_hdd0/salsa64` change the button mapping
  (`controles.cfg`), the save type per game (`guardado.cfg`), audio
  (`audio.cfg`) and overscan cropping (`overscan.cfg`); see the guide.
- **RetroAchievements** (off by default, Options). Regular achievements work.
  Salsa64's Hardcore mode is **not yet approved by RetroAchievements**: approval
  requires 6 public months and will be requested in April 2027.
  See `docs/retroachievements.md`.

## Reporting problems and compatibility

- **A bug in a game** (black screen, freeze, wrong graphics): open an
  [issue](https://github.com/ricowp81/Salsa64/issues/new/choose) with the
  form; attach `/dev_hdd0/salsa64/salsa64.log` copied right after quitting
  (it's rewritten every time Salsa64 starts).
- **How a game runs on your PS3**: post it in the
  [compatibility thread](https://github.com/ricowp81/Salsa64/discussions/1).
- **Questions**: [Discussions → Q&A](https://github.com/ricowp81/Salsa64/discussions/categories/q-a).

Please don't share ROMs or links to them.

## Building

Everything builds inside a Docker image with
[ps3toolchain](https://github.com/ps3dev/ps3toolchain) and
[PSL1GHT](https://github.com/ps3dev/PSL1GHT); nothing from Sony's official SDK
is used. Requirements:

- Docker (or a compatible engine) with several GB free. On arm64 hosts
  (Apple Silicon) the engine must also be able to run `linux/amd64`
  containers (Docker Desktop does; with colima, enable Rosetta or binfmt),
  because NVIDIA's Cg Toolkit, used by `cgcomp`, only exists for x86_64.
- Python 3 for the helper scripts on the host.

```sh
tools/docker-image.sh                  # once: builds salsa64-toolchain:dev (GCC for PPU/SPU; 15–60 min)
tools/build.sh                         # all PS3 programs: test programs and the emulator
tools/build.sh -C src/frontend pkg     # build/salsa64/salsa64.pkg
tools/build.sh test-host               # core unit tests with the native gcc (ASan/UBSan)
tools/build.sh host                    # build/host/salsa64-run: run a ROM on the host
tools/test-roms.sh                     # homebrew ROM suites on the host, against the baseline
tools/build.sh -C tests/fuzz run       # short fuzzing of the PNG and .cht readers
```

Outputs go to `build/<program>/`: `.elf` (RPCS3, ps3load), `.self`,
`.fake.self` and, with `pkg`, `.pkg`. Tests in RPCS3, deploying to a PS3 and
the other test scripts are described in [CONTRIBUTING.md](CONTRIBUTING.md).

## Repository layout

```
src/core/      N64 core: memory, interfaces (PI, SI, VI, AI), PIF, boot HLE
src/cpu/       VR4300 interpreter and MIPS → PPC64 recompiler
src/rsp/       RSP graphics, audio and JPEG HLE; low-level RSP interpreter
src/spu/       SPE programs (graphics HLE on SPE 0, audio on SPE 1, LLE)
src/rdp/       N64 RDP and combiners on top of lib/rsxkit
src/frontend/  PS3 application: menu, input, audio output, saves
src/host/      salsa64-run, the host-side runner used for tests
lib/rsxkit/    generic RSX graphics layer, no N64 code (MIT; also github.com/ricowp81/rsxkit)
lib/spukit/    batched SPE jobs: queue + mailbox doorbell (MIT; also github.com/ricowp81/spukit)
tests/         unit tests, fuzzing, PS3 test programs, homebrew test ROMs
tools/         build, test and deploy scripts
docs/          design notes, measurements, third-party sources (Spanish)
```

Goals, architecture and roadmap: [docs/proyecto.md](docs/proyecto.md).

## Support

If you enjoy Salsa64, you can buy me a coffee at
[buymeacoffee.com/ricowp81](https://buymeacoffee.com/ricowp81). Thanks!

## AI disclosure

Salsa64 is an AI-assisted project. The emulator itself is tested on real PS3 hardware.

## Credits

- **VIDEOGAMES SCZ** contributed the pause menu, cheats, box art, the 16:9
  aspect option, the simple image filters (Smooth, Sharp, Scanlines),
  per-game settings, the first six menu languages and the experimental downloader.
- The cartridge icon in the game list is a render of **"Stylized N64" by
  AlessandroFortuna** (Sketchfab), licensed under CC BY 4.0, modified to remove
  Nintendo artwork.
- The button icons in the menus (DualShock 3 prompts and the N64 buttons on
  the controls page, drawn with GameCube icons) are from **Kenney** "Input
  Prompts" 1.5A ([kenney.nl](https://kenney.nl)), CC0.

## License

Copyright (C) 2026 ricowp81.

Salsa64 is free software under the **GNU General Public License, version 2 or
(at your option) any later version** ([LICENSE](LICENSE)). Files that adapt
code from other projects keep their original copyright notices (see
[docs/THIRD_PARTY.md](docs/THIRD_PARTY.md)).

**The combined binary is distributed under GPLv2.** `src/rsp/gfx/f5rogue.c`
(the graphics microcode HLE for Star Wars: Rogue Squadron),
`src/rsp/gfx/f5indy.c` (the one for Indiana Jones and the Infernal Machine and
Battle for Naboo) and `src/rsp/gfx/f3dex2acclaim.c` are translated from
[GLideN64](https://github.com/gonetz/GLideN64), which is GPL version 2
only (no "or later"), and keep that license
(`SPDX-License-Identifier: GPL-2.0-only`). Everything else in the tree stays
GPL-2.0-or-later, so you can still reuse those files under either version
on their own, but a build that links either file (the host runner, the PS3
program and its SPU images) is GPLv2. Code under GPLv3 (gopher64, simple64,
RMG) can no longer be mixed in. `lib/rsxkit/` and `lib/spukit/` do not
contain or link any of it.

`lib/rsxkit/` and `lib/spukit/` are under the **MIT license** (their own
`LICENSE` files) so that other emulators and homebrew, including projects
whose licenses cannot link GPL code, can reuse them. Third-party sources and
references are listed in [docs/THIRD_PARTY.md](docs/THIRD_PARTY.md).

## Libraries for other projects

The two MIT libraries also live in their own repositories, so that other PS3
homebrew can use them without taking the emulator:

- **[rsxkit](https://github.com/ricowp81/rsxkit):** RSX graphics layer
  (video setup, double-buffered frames, 2D and 3D drawing, a run-time
  fragment program generator, a post-processing chain).
- **[spukit](https://github.com/ricowp81/spukit):** batch jobs on the SPEs
  (work queue with a mailbox doorbell, persistent workers, LZ block
  compression on the PPE, the host and an SPE).

Salsa64 keeps them in `lib/` with `git subtree`, so building Salsa64 needs
nothing extra. How changes go back and forth: [docs/bibliotecas.md](docs/bibliotecas.md)
(Spanish).

## Legal notice

Salsa64 is not affiliated with, endorsed or sponsored by Nintendo or Sony.
"Nintendo 64" and "PlayStation" are trademarks of their respective owners.

This repository does not contain, and will not accept, commercial ROMs,
BIOS or PIF ROM images, IPL3 boot code from Nintendo, encryption keys, or any
other material copyrighted by Nintendo or Sony, nor any part of Sony's
official PS3 SDK. The emulator boots games without a PIF ROM (high-level
emulation of the boot process). Use it only with dumps of games you own.
The test ROMs in `tests/roms/homebrew/` are freely licensed homebrew built
from source (see their README).
