# Building the PS4 package

Players do not need this: download the ready `.pkg` instead (see
[PS4.md](../../PS4.md)). This page is for building it yourself on Linux.

## Requirements

- Linux (tested on Arch Linux, x86-64)
- the normal desktop build requirements of Golden Balloon (CMake, a C/C++
  compiler, SDL2; see the main README)
- `clang` and `ld.lld` (LLVM)
- the [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases)
  v0.5.2, unpacked anywhere
- the [.NET runtime](https://dotnet.microsoft.com/download) 8 or newer (the
  toolchain's package tool needs it)
- Python 3

## Build

```bash
git clone https://github.com/palrat/goldenballoon.git
cd goldenballoon

# 1. Configure the desktop build once. The PS4 build reuses its source list
#    and the third-party sources CMake downloads here; nothing has to be built.
cmake -B build-gl -DCMAKE_BUILD_TYPE=Release -DMDKR_WEBGPU_BACKEND=OFF

# 2. Tell the scripts where the toolchain is.
export OO_PS4_TOOLCHAIN=/path/to/OpenOrbis/PS4Toolchain

# 3. Build and package. The number is the app version shown on the PS4.
./platform/ps4/release.sh 04.00
```

The result is `build-ps4/DKR-04.00.pkg`.

To install it on the console in the same step, open the **Remote Package
Installer** app on the PS4, make sure GoldHEN's FTP server is on, and run:

```bash
PS4_IP=<your PS4's IP address> ./platform/ps4/release.sh 04.00
```

## What is PS4-specific

| File | Purpose |
|---|---|
| `ps4_entry.c` | `main()`: log file, ROM and module lookup (also from USB), on-screen error messages |
| `sdl_ps4.c`, `SDL.h` | the part of SDL2 the game uses, on top of Piglet (Sony's OpenGL ES 2.0), libScePad and libSceAudioOut |
| `build_ps4.sh` | compiles the game's sources with the OpenOrbis toolchain and links `eboot.bin` |
| `make_pkg.py`, `release.sh` | build the `.pkg` (and optionally install it) |
| `sce_sys_template/` | `param.sfo`, menu tile `icon0.png`, launch screen `pic1.png` |
| `art/make_art.py` | draws the menu tile and launch screen |

Outside this folder the PS4 port changes only what the console needs:

- `platform/stubs_dkr.c`: the PS4 loads the game below 4 GB, so pointers into
  the game's own data must be registered like any other host pointer.
- `platform/fast3d/gfx_opengl.c`: OpenGL ES 2.0 support (no VAOs, GLSL ES
  1.00, unsized texture formats, no render-to-texture scene target).
- `platform/fast3d/gfx_pc_dkr.c`: no render scaling without a scene target.
- `platform/math_util_native.c`: the math tables are filled by an explicit
  call, because the console build has no constructors.
- `platform/platform_sdl_min.c`: GL function pointers are looked up again
  through SDL, because glad (generated for desktop GL) leaves them empty on an
  OpenGL ES context.
- `platform/online/compatibility_identity.c`: a platform tag for the PS4.

## Debug switches

A file `/data/dkr/flags.txt` on the console can set environment variables,
one `NAME=value` per line:

| Line | Effect |
|---|---|
| `MDKR_PERF=1` | writes frame rate and frame times to the log every 5 seconds |
| `MDKR_CAPTURE=1` | creating the file `/data/dkr/capture_now` saves one screenshot as `shot_<frame>.ppm` |
| `MDKR_TRACE=2` and `MDKR_DL_FRAME=<n>` | logs every draw of frame `n`, to compare with the desktop build |

The log is `/data/dkr/ps4_boot.log`.

## Reproducing console bugs on the desktop

Linking the desktop build without PIE places it at the same low address as the
PS4 `eboot.bin`, which reproduces address-layout bugs without a console:

```bash
cd build-gl && make mdkr64
sed -e 's# -o mdkr64 # -no-pie -o mdkr64_nopie #' CMakeFiles/mdkr64.dir/link.txt | bash
```
