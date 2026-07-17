# Media Gallery

A minimal, secure Windows photo viewer in the spirit of the stock Windows Photo
Viewer — single small native executable, **no packages, no DLLs, no
downloads**. Pure C++ / WinAPI with GDI+ and WIC (both part of Windows) doing
all *image* decoding, so every image parser is OS-maintained and
security-patched by Windows Update. Optional *video* playback is the one
exception to "zero third-party code": an embedded player engine plus an
FFmpeg subset and libass (styled ASS/SSA subtitles) — all LGPL-2.1+,
vendored into `third_party/` as plain committed source and compiled by your
own toolchain — nothing is fetched or prebuilt, but those parsers are
updated by re-vendoring, not by Windows Update. Builds
without the engine (`src/player_stub.cpp`) remain image-only. Runs on stock
Windows 10 and 11.

## Features

- **Formats:** JPEG, GIF, PNG, BMP, TIFF, ICO (GDI+) plus JPEG XR and DDS (WIC).
  HEIC/HEIF, AVIF, WebP and camera RAW light up automatically when the free
  Microsoft Store codec extensions are installed — no code changes needed.
- **Video (CMake/MSVC builds):** MP4, M4V, MOV, MKV, WebM and AVI play in
  place with audio via the embedded player engine (D3D11 + WASAPI, vendored
  FFmpeg demux/decode). Videos appear in the folder list, filmstrip and grid
  like any image; a transport bar (play/pause, seek) sits above the
  filmstrip, `Space` pauses, `Ctrl+←`/`Ctrl+→` seek ±10 s, the wheel adjusts
  volume, and `I` shows duration/codec details. *View → Auto-Advance After
  Video* (off by default) moves to the next item when playback ends.
- **Explorer-order navigation:** if the folder is open in an Explorer window,
  `←`/`→` follow *that window's current sort order* (read live via
  `IShellWindows`/`IFolderView`); otherwise Explorer's default natural name
  sort (`StrCmpLogicalW`).
- **Filmstrip:** thumbnail ribbon (Explorer's own thumbnail pipeline; in video
  builds the engine decodes a real frame for video thumbnails) along the
  bottom; click a thumbnail to jump to it.
- **Thumbnail grid:** `G` switches to a full-window grid of the whole folder —
  8 columns by default; `Ctrl` `+`/`-` or Ctrl+wheel zoom the grid (8×8 → 4×4 →
  2×2 → 1, bigger cells fetch sharper thumbnails), wheel scrolls, arrows move
  the selection, click or `Enter` opens the image, `Del` deletes the selected
  one, `Esc`/`G` returns to the viewer.
- **Slideshow:** `F5` (or *View → Slideshow*) advances through the current
  scope — the folder or a dropped selection — every 5 seconds; a video plays
  to its end first, then moves on. `Esc` or `F5` exits. *View → Slideshow
  Options* picks the interval (2 / 5 / 10 seconds) and a Shuffle mode that
  jumps to a random other item instead of the next one.
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
| `Space` | Play/pause the current video |
| `Ctrl+→` / `Ctrl+←` | Seek ±10 s in the current video |
| Mouse wheel | Zoom at cursor (over the filmstrip: scroll it; over a video: volume) |
| `Ctrl` `+` / `Ctrl` `-` / `Ctrl+0` | Zoom in / out / fit |
| Left-drag | Pan when zoomed in |
| `Ctrl+R` / `Ctrl+Shift+R` | Rotate right / left |
| `Ctrl+S` | Save rotation (when available) |
| `Del` | Delete to Recycle Bin |
| `E` | Edit in Paint |
| `I` / `Alt+Enter` | Details pane |
| `T` | Toggle filmstrip |
| `G` | Thumbnail grid view (arrows select, `Enter`/click opens, `Ctrl` `+`/`-` or Ctrl+wheel changes density, `Esc` returns) |
| `F5` | Slideshow (2/5/10 s per image and Shuffle via *View → Slideshow Options*; videos play to the end; `Esc`/`F5` exits) |
| `Ctrl+O` | Open… |
| `Esc` | Exit (leaves the slideshow or grid first) |

## Building

Full step-by-step instructions (all toolchains, output paths,
troubleshooting): **[BUILDING.md](BUILDING.md)**. Short version — no
third-party SDKs, no vcpkg/NuGet, no downloads. **Video playback is built
only by the CMake + MSVC route** (it compiles the vendored FFmpeg subset);
every other route produces the image-only viewer:

**Full app with video (recommended)** — VS 2022+ with the *Desktop
development with C++* workload (includes CMake and Ninja). From an
**x64 Native Tools Command Prompt for VS** (the plain Developer Prompt
targets x86 and the link fails):

    cmake --preset x64-release
    cmake --build --preset x64-release

Output: `build\x64-release\MediaGallery.exe` **and**
`build\x64-release\minimal-player.exe` — this one branch builds both the
gallery and the standalone player (same engine, the player's Win32 shell
is vendored under `third_party/player-shell/`). The first build compiles
the vendored FFmpeg sources (a few minutes, once). In the VS IDE, use
*File → Open → Folder* on the repo — it picks up the presets. This is
what CI builds and uploads as artifacts on every push.

**Image-only routes** (no video, identical to the pre-video viewer):

Visual Studio solution — open **`MediaGallery.sln`** and build (x64), or:

    msbuild MediaGallery.sln -p:Configuration=Release -p:Platform=x64

Plain MSVC, no CMake — from an *x64 Native Tools Command Prompt*:

    build.bat

MinGW-w64 / cross-compile (also used for CI-style verification):

    make            # cross from Linux, or: mingw32-make CXX=g++ WINDRES=windres on MSYS2

The output is a single self-contained `MediaGallery.exe` (static CRT) that
runs on stock Windows 10/11 — ~370 KB image-only, a few MB with the video
engine linked in.

## Security notes

- All image decoding is done by OS codecs (GDI+/WIC) that receive Windows
  Update patches; no third-party or hand-written image parsers. Video
  decoding (when built in) uses the vendored FFmpeg, and styled subtitles the
  vendored libass — in-process parsers of untrusted media, subtitle and font
  data whose patch story is re-vendoring, not Windows Update.
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
- The gallery performs no network I/O: it only ever opens local file paths.
  Image-only builds contain no network code at all. Video-enabled builds do
  embed FFmpeg's network protocols (http/https/tcp/udp, TLS via Windows
  Schannel) as part of the vendored engine — that code runs only if a caller
  passes a URL, which the gallery never does. The vendored libass adds no
  network path; it only parses subtitle and font bytes already in hand.
- No registry writes, no config files.

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
