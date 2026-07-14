# Building Photo Gallery

The app is plain C++17 / WinAPI. Everything it links against (GDI+, WIC,
shell, common dialogs) ships with Windows — there are **no third-party
libraries, no vcpkg, no NuGet packages** to restore. Any one of the routes
below produces the same single `PhotoGallery.exe` that runs on stock
Windows 10 and 11 (x64) with nothing to install.

## Prerequisites

One of:

- **Visual Studio 2022** (any edition, Community is fine) with the
  **"Desktop development with C++"** workload. That workload already includes
  the MSVC v143 compiler and a Windows 10/11 SDK. *(MFC/ATL components are
  NOT required.)*
- **Build Tools for Visual Studio 2022** (no IDE) with the same workload —
  if you only want command-line builds.
- **MinGW-w64 GCC 12+** (MSYS2: `pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make`,
  or a winlibs.com zip) — if you want to avoid Microsoft tooling entirely.

Optional: **CMake 3.15+** (bundled with Visual Studio; standalone works too).

## 1. Visual Studio IDE (easiest)

1. Open `PhotoGallery.sln`.
2. Pick `Release | x64` in the toolbar.
3. Build → *Build Solution* (Ctrl+Shift+B).
4. Output: `x64\Release\PhotoGallery.exe`.

Press F5 to run/debug. To view a specific image while debugging, set
*Project → Properties → Debugging → Command Arguments* to the image path.

## 2. MSBuild command line

From a **"Developer Command Prompt for VS 2022"** (or any shell where
`msbuild` is on PATH), in the repo root:

    msbuild PhotoGallery.sln -p:Configuration=Release -p:Platform=x64

Output: `x64\Release\PhotoGallery.exe`.

## 3. Plain MSVC, no project files, no CMake

From an **"x64 Native Tools Command Prompt for VS 2022"**, in the repo root:

    build.bat

Compiles all sources with `cl` in one shot and links `PhotoGallery.exe`
into the repo root. Fastest way to build if you don't care about a project.

## 4. CMake (Visual Studio generator or Open-Folder)

    cmake -B build -G "Visual Studio 17 2022" -A x64
    cmake --build build --config Release

Output: `build\Release\PhotoGallery.exe`. Alternatively use
*File → Open → Folder* in Visual Studio, which picks up `CMakeLists.txt`
automatically.

## 5. MinGW-w64 (native on Windows, or cross-compile from Linux)

Native (MSYS2 *MINGW64* shell):

    mingw32-make CXX=g++ WINDRES=windres

Cross-compile from Linux (package `g++-mingw-w64-x86-64`):

    make

Output: `PhotoGallery.exe` in the repo root (fully static, no MinGW DLLs
needed at runtime).

## Build settings worth knowing

- **Release binaries use the static CRT** (`/MT`, or `-static` for MinGW),
  so the exe is self-contained — no VC++ Redistributable needed.
- Hardening is on by default: `/W4`, SDL checks, Control Flow Guard
  (`/guard:cf`), DEP/ASLR; the MinGW build uses `-Wall -Wextra -Werror`.
- The application manifest (per-monitor-v2 DPI awareness, `asInvoker`) is
  embedded via `src/PhotoGallery.rc`. For this reason the `.vcxproj` and the
  linker invocations set *GenerateManifest = false* / `/MANIFEST:NO` — if you
  create your own project file, do the same or the linker will error with a
  duplicate manifest resource (CVT1100/LNK1123).
- Only x64 configurations are provided. A 32-bit build would work (nothing
  is x64-specific) but is not configured.

## Troubleshooting

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
