# Building Photo Gallery

The app is plain C++17 / WinAPI. Image viewing links only against
components that ship with Windows (GDI+, WIC, shell, common dialogs).
Video playback is optional and comes from source vendored into this repo
(`third_party/`: the minimal-player engine + an FFmpeg subset + libass, all
LGPL-2.1+) — still **no vcpkg, no NuGet, no downloads, no prebuilt
binaries**; your toolchain compiles every byte from committed text.

Two kinds of build:

- **Video-enabled** (`PhotoGallery.exe` plays mp4/m4v/mov/mkv/webm/avi):
  **CMake + MSVC only** — route 4 below. The vendored FFmpeg config is
  harvested from an MSVC x64 configure and cannot be built by MinGW, and
  the ~500 FFmpeg translation units are only described in
  `cmake/ffmpeg.cmake` (libass + FreeType + FriBidi + HarfBuzz likewise in
  `cmake/libass.cmake`).
- **Image-only** (identical to the pre-video viewer): every other route.
  They compile `src/player_stub.cpp` (a no-op implementation of the
  engine API) and treat video files as unsupported.

## Prerequisites

One of:

- **Visual Studio 2022** (any edition, Community is fine) with the
  **"Desktop development with C++"** workload. That workload already includes
  the MSVC v143 compiler, CMake/Ninja, and a Windows 10/11 SDK. *(MFC/ATL
  components are NOT required.)*
- **Build Tools for Visual Studio 2022** (no IDE) with the same workload —
  if you only want command-line builds.
- **MinGW-w64 GCC 12+** (MSYS2: `pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make`,
  or a winlibs.com zip) — image-only builds without Microsoft tooling.

CMake 3.24+ is required for the video-enabled build (VS 2022 bundles a
new-enough one).

## 1. Visual Studio IDE — image-only

1. Open `PhotoGallery.sln`.
2. Pick `Release | x64` in the toolbar.
3. Build → *Build Solution* (Ctrl+Shift+B).
4. Output: `x64\Release\PhotoGallery.exe`.

Press F5 to run/debug. To view a specific image while debugging, set
*Project → Properties → Debugging → Command Arguments* to the image path.
(For the video-enabled build in the IDE, use *File → Open → Folder* on the
repo instead — that picks up `CMakeLists.txt` and its presets.)

## 2. MSBuild command line — image-only

From a **"Developer Command Prompt for VS 2022"** (or any shell where
`msbuild` is on PATH), in the repo root:

    msbuild PhotoGallery.sln -p:Configuration=Release -p:Platform=x64

Output: `x64\Release\PhotoGallery.exe`.

## 3. Plain MSVC, no project files, no CMake — image-only

From an **"x64 Native Tools Command Prompt for VS 2022"**, in the repo root:

    build.bat

Compiles all sources with `cl` in one shot and links `PhotoGallery.exe`
into the repo root. Fastest way to build if you don't care about a project.

## 4. CMake — the video-enabled build

From an **"x64 Native Tools Command Prompt for VS 2022"** (the configure
step rejects the default x86 developer prompt):

    cmake --preset x64-release
    cmake --build --preset x64-release

Output: `build\x64-release\PhotoGallery.exe` and, from the same build,
`build\x64-release\minimal-player.exe` — the standalone player compiled
from its vendored shell (`third_party/player-shell/`) against the same
engine, so this one branch yields both apps. Alternatively use
*File → Open → Folder* in Visual Studio, or the classic generator:

    cmake -B build -G "Visual Studio 17 2022" -A x64
    cmake --build build --config Release

The first build compiles the vendored FFmpeg subset (~500 C files) plus the
libass/FreeType/FriBidi/HarfBuzz subtitle stack once; afterwards it's cached. Pass `-DPHOTOGALLERY_VIDEO=OFF` to get the
image-only viewer from CMake too (fast, and the only CMake mode for
non-MSVC compilers, where it is forced OFF automatically).

## 5. MinGW-w64 (native on Windows, or cross-compile from Linux) — image-only

Native (MSYS2 *MINGW64* shell):

    mingw32-make CXX=g++ WINDRES=windres

Cross-compile from Linux (package `g++-mingw-w64-x86-64`):

    make

Output: `PhotoGallery.exe` in the repo root (fully static, no MinGW DLLs
needed at runtime).

## Build settings worth knowing

- **Release binaries use the static CRT** (`/MT`, or `-static` for MinGW),
  so the exe is self-contained — no VC++ Redistributable needed. The CMake
  build sets this globally (`CMAKE_MSVC_RUNTIME_LIBRARY`) so the vendored
  FFmpeg libs match; mixing `/MD` objects in fails with LNK2038.
- Hardening is on by default for the app sources: `/W4`, SDL checks,
  Control Flow Guard (`/guard:cf`), DEP/ASLR; the MinGW build uses
  `-Wall -Wextra -Werror`. Vendored third-party code compiles at `/W0`
  (FFmpeg, libass) and `/W4` (engine).
- The application manifest (per-monitor-v2 DPI awareness, `asInvoker`) is
  embedded via `src/PhotoGallery.rc`. For this reason the `.vcxproj` and the
  linker invocations set *GenerateManifest = false* / `/MANIFEST:NO` — if you
  create your own project file, do the same or the linker will error with a
  duplicate manifest resource (CVT1100/LNK1123).
- Only x64 configurations are provided. The image-only viewer would build
  as 32-bit, but the vendored FFmpeg config is x64-specific.
- `third_party/` is committed source text, produced only by
  `tools/vendor_player.cmake` (run against a checkout of the player repo).
  Re-running that script is the entire upgrade story; never edit
  `third_party/` by hand.

## Troubleshooting

- **CMake configure fails with "LIB points at x86 libraries"** — you are in
  the default (x86) Developer Prompt. Use the **x64** Native Tools prompt
  and reconfigure.
- **`rc.exe` / `windres` can't find `resource.h` or `app.manifest`** — build
  from the repo root; the resource script resolves those relative to `src\`
  (`build.bat` passes `/I src`, the Makefile passes `--include-dir=src`).
- **Duplicate manifest error (CVT1100)** — the linker's automatic manifest
  collided with the one in the `.rc`; set *Generate Manifest: No* (see above).
- **`GetDpiForWindow` unresolved / not declared** — the code targets
  Windows 10+ (`_WIN32_WINNT=0x0A00`, already set by every provided build
  file); make sure that define isn't lowered and the Windows SDK is 10+.
- **MSYS2: `make: command not found`** — install `mingw-w64-x86_64-make` and
  invoke `mingw32-make`.

## Quick functional check after building

1. Double-click a JPEG in a folder that Explorer has sorted by date —
   `←`/`→` should walk that order.
2. `Del` should send the file to the Recycle Bin (recoverable).
3. `E` should open the image in Paint; after saving there, the viewer
   refreshes when refocused.
4. Open a multi-page TIFF — a `◀ 1 / N ▶` overlay appears; `↑`/`↓` switch pages.
5. Rotate with `Ctrl+R` — a *Save* button appears; saving a JPEG is lossless.
6. *(Video build)* Open a folder containing an `.mp4` — it appears in the
   filmstrip, plays with sound, `Space` pauses, `Ctrl+←`/`Ctrl+→` seek,
   the wheel changes volume, and `I` shows duration/codec details.
