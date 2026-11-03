# Official 7-Zip LZMA SDK (C decoder subset)

This directory vendors the unmodified ANSI-C sources and headers required by
Salsa64's 7z ROM reader. It is taken from the official **LZMA SDK 26.05**
release, not from a third-party port or wrapper.

- Official project: https://github.com/ip7z/7zip
- Release: https://github.com/ip7z/7zip/releases/tag/26.05
- Pinned source commit: `b88c6bc09339de32f7d18bbab900686b244f42ba`
- Official SDK page and license: https://www.7-zip.org/sdk.html

Only the C decoder implementation (7z container parsing, stream/file helpers,
CRC, LZMA/LZMA2, PPMd and required filters) and its directly needed headers
are included under `C/`. The copied files retain their upstream contents and
public-domain notices. The exact public-domain grant from the official SDK
page is reproduced in [LICENSE](LICENSE).

Salsa64's adapter is `src/frontend/sevenziprom.c`; it uses the SDK's
`SzArEx` API, keeps archive data in memory, bounds metadata and solid-block
allocations, and extracts only ROM members. No 7-Zip Extra binaries, 7z.dll,
UnRAR code, or external submodules are used.
