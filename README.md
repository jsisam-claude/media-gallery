# Photo Gallery

A minimal, secure Windows photo viewer in the spirit of the stock Windows Photo
Viewer — single small native executable, **zero third-party dependencies**.
Pure C++ / WinAPI with GDI+ and WIC (both part of Windows) doing all image
decoding, so every parser is OS-maintained and security-patched by Windows
Update. Runs on stock Windows 10 and 11.

## Features

- **Formats:** JPEG, GIF, PNG, BMP, TIFF, ICO (GDI+) plus JPEG XR and DDS (WIC).
  HEIC/HEIF, AVIF, WebP and camera RAW light up automatically when the free
  Microsoft Store codec extensions are installed — no code changes needed.
- **Explorer-order navigation:** if the folder is open in an Explorer window,
  `←`/`→` follow *that window's current sort order* (read live via
  `IShellWindows`/`IFolderView`); otherwise Explorer's default natural name
  sort (`StrCmpLogicalW`).
- **Filmstrip:** thumbnail ribbon (Explorer's own thumbnail pipeline) along the
  bottom; click a thumbnail to jump to it.
- **Thumbnail grid:** `G` switches to a full-window grid of the whole folder —
  8 columns by default; `Ctrl` `+`/`-` or Ctrl+wheel zoom the grid (8×8 → 4×4 →
  2×2 → 1, bigger cells fetch sharper thumbnails), wheel scrolls, arrows move
  the selection, click or `Enter` opens the image, `Del` deletes the selected
  one, `Esc`/`G` returns to the viewer.
- **Zoom & pan:** mouse wheel zooms at the cursor, `Ctrl` `+`/`-` step zoom,
  `Ctrl+0` fit; drag to pan when zoomed in. Aspect ratio is always preserved;
  small images display at 100%, never stretched.
- **Rotate & save:** on-screen ↺/↻ buttons (or `Ctrl+R`/`Ctrl+Shift+R`); a Save
  button appears only when the rotation is unsaved. JPEG rotation is lossless;
  writes are atomic (temp file + `ReplaceFileW`).
- **Multi-page TIFF:** ◀ page x/y ▶ overlay; `↑`/`↓` or `PgUp`/`PgDn` switch pages.
- **Delete:** `Del` moves the file to the **Recycle Bin** (recoverable).
- **Edit:** `E` opens the image in **mspaint**; the view refreshes when you
  come back after saving.
- **Details:** `I` toggles a pane with file info (name, folder, size, dates)
  and metadata (dimensions, format, EXIF: date taken, camera, exposure,
  f-number, ISO, focal length).
- **Drag & drop:** drop an image, a folder, or a multi-selection of images
  onto the window. A dropped multi-selection becomes the navigation scope.

## Keys

| Key | Action |
| --- | --- |
| `→` / `←` | Next / previous image (wraps around) |
| `↑` / `↓`, `PgUp` / `PgDn` | Previous / next page (multi-page files) |
| Mouse wheel | Zoom at cursor (over the filmstrip: scroll it) |
| `Ctrl` `+` / `Ctrl` `-` / `Ctrl+0` | Zoom in / out / fit |
| Left-drag | Pan when zoomed in |
| `Ctrl+R` / `Ctrl+Shift+R` | Rotate right / left |
| `Ctrl+S` | Save rotation (when available) |
| `Del` | Delete to Recycle Bin |
| `E` | Edit in Paint |
| `I` / `Alt+Enter` | Details pane |
| `T` | Toggle filmstrip |
| `G` | Thumbnail grid view (arrows select, `Enter`/click opens, `Ctrl` `+`/`-` or Ctrl+wheel changes density, `Esc` returns) |
| `Ctrl+O` | Open… |
| `Esc` | Exit |

## Building

Full step-by-step instructions (all toolchains, output paths,
troubleshooting): **[BUILDING.md](BUILDING.md)**. Short version — no
third-party SDKs, no vcpkg/NuGet, everything links against Windows system
libraries, and any one of these works:

**Visual Studio (recommended)** — VS 2022 with the *Desktop development with
C++* workload (MFC not required). Open **`PhotoGallery.sln`** and build
(x64, Debug/Release), or from a command prompt:

    msbuild PhotoGallery.sln -p:Configuration=Release -p:Platform=x64

CMake also works if you prefer it (*File → Open → Folder*, or):

    cmake -B build -G "Visual Studio 17 2022" -A x64
    cmake --build build --config Release

**Plain MSVC, no CMake** — from an *x64 Native Tools Command Prompt*:

    build.bat

**MinGW-w64 / cross-compile** (also used for CI-style verification):

    make            # cross from Linux, or: mingw32-make CXX=g++ WINDRES=windres on MSYS2

The output is a single self-contained `PhotoGallery.exe` (~370 KB, static CRT)
that runs on stock Windows 10/11.

## Security notes

- All decoding is done by OS codecs (GDI+/WIC) that receive Windows Update
  patches; no third-party or hand-written parsers.
- A decompression-bomb guard caps decoded size (~134 MP) regardless of codec.
- Wide-character APIs throughout; paths are canonicalized and checked against
  the decoder-derived extension allowlist before any use.
- No command strings: Paint is launched via `ShellExecuteW` with a fixed
  executable name and the file passed as a quoted parameter; deletion goes
  through the shell's `IFileOperation` (Recycle Bin), never a shell command.
- File writes (save rotation) go to a temp file first and are swapped in with
  `ReplaceFileW`, so the original survives any failure.
- Runs `asInvoker` (never elevates), per-monitor-v2 DPI aware, heap corruption
  termination enabled, `/GS /guard:cf /DYNAMICBASE /NXCOMPAT` hardened build.
- No network, no registry writes, no config files.

## Adding an image format

Formats are pluggable (`src/decoder.h`): implement `ImageDecoder` in a new
`decoder_*.cpp` (claim extensions, decode to 32-bpp BGRA, optionally support
`SaveRotation`) and register it in `InitDecoders()` in
`src/decoder_gdiplus.cpp`. Folder scanning, Explorer-order filtering and the
Open dialog derive their extension list from the registry automatically. Note
that most formats need no new code at all: anything with a WIC codec installed
(e.g. from the Microsoft Store) is picked up at runtime by the generic WIC
decoder.

## Known limits (by design, to stay minimal)

- Animated GIFs show their first frame (same as the stock Windows Photo Viewer).
- Rotation save is disabled for animated GIFs, multi-page TIFFs and WIC-only
  formats (it would flatten or re-encode them destructively).
- Paths longer than `MAX_PATH` are not specially handled.
