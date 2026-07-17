# Media Gallery — minimal Windows photo viewer (WinAPI + GDI+/WIC) with
# optional video playback (vendored player engine + FFmpeg, see
# third_party/ and BUILDING.md).
#
# Visual Studio:  open this folder ("Open a local folder"), or:
#   cmake --preset x64-release          (from an x64 Native Tools prompt)
#   cmake --build --preset x64-release
cmake_minimum_required(VERSION 3.24)
project(MediaGallery LANGUAGES C CXX)

# Video playback compiles the vendored FFmpeg + player engine (MSVC only:
# the committed FFmpeg config in third_party/ffmpeg-config is harvested
# from an MSVC win64 configure). OFF builds the image-only viewer with
# src/player_stub.cpp — same behavior as every non-CMake build route.
option(MEDIAGALLERY_VIDEO "Build with video playback (MSVC only)" ON)
if(NOT MSVC)
    set(MEDIAGALLERY_VIDEO OFF)
endif()

if(MSVC)
    # Static CRT, hoisted globally BEFORE any target: the ff_* libs created
    # inside cmake/ffmpeg.cmake would otherwise default to /MD and the final
    # link fails with LNK2038 (RuntimeLibrary mismatch).
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    # The default "Developer Command Prompt/PowerShell" targets x86; its LIB
    # paths make every x64 link fail with LNK4272/LNK2001 on all symbols.
    if(DEFINED ENV{LIB} AND "$ENV{LIB}" MATCHES "[\\\\/]x86(;|$)")
        message(FATAL_ERROR "This shell's LIB points at x86 libraries. "
            "Use the 'x64 Native Tools Command Prompt for VS' (or "
            "VsDevCmd.bat -arch=x64 -host_arch=x64), then reconfigure.")
    endif()
endif()

add_executable(MediaGallery WIN32
    src/main.cpp
    src/decoder_gdiplus.cpp
    src/decoder_wic.cpp
    src/filmstrip.cpp
    src/MediaGallery.rc
)

set_target_properties(MediaGallery PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_compile_definitions(MediaGallery PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00)

if(MSVC)
    # Hardened, static-CRT release binary; /W4 warnings.
    target_compile_options(MediaGallery PRIVATE /W4 /EHsc /guard:cf)
    target_link_options(MediaGallery PRIVATE /DYNAMICBASE /NXCOMPAT /guard:cf /MANIFEST:NO)
else()
    # clang/mingw
    target_compile_options(MediaGallery PRIVATE -Wall -Wextra -municode)
    target_link_options(MediaGallery PRIVATE -municode -mwindows -static)
endif()

target_link_libraries(MediaGallery PRIVATE
    gdiplus windowscodecs shlwapi shell32 ole32 oleaut32 uuid comdlg32 gdi32 user32
)

if(MEDIAGALLERY_VIDEO)
    # Vendored FFmpeg, compiled by this toolchain from source text only —
    # see cmake/ffmpeg.cmake (copied verbatim from the player repo).
    include(cmake/ffmpeg.cmake)

    # Vendored libass + FreeType + FriBidi + HarfBuzz, same source-only rules —
    # see cmake/libass.cmake (copied verbatim). Styled ASS/SSA subtitles.
    include(cmake/libass.cmake)

    # The embeddable engine; the app includes only player.h.
    add_library(player_engine STATIC
        third_party/player-engine/player.cpp
        third_party/player-engine/demux.cpp
        third_party/player-engine/decode.cpp
        third_party/player-engine/queue.cpp
        third_party/player-engine/audio_out.cpp
        third_party/player-engine/video_out.cpp
        third_party/player-engine/subs.cpp
        third_party/player-engine/ass_render.cpp
    )
    set_target_properties(player_engine PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
    target_compile_definitions(player_engine PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(player_engine PRIVATE /W4 /utf-8)
    target_include_directories(player_engine PUBLIC third_party/player-engine)
    target_link_libraries(player_engine
        PRIVATE ffmpeg libass
        PUBLIC d3d11 dxgi d3dcompiler d2d1 dwrite windowscodecs ole32 avrt winmm user32
    )

    target_link_libraries(MediaGallery PRIVATE player_engine)

    # The shell's version resource is a configure_file template upstream;
    # stamp the vendored player version (third_party/player-shell/VERSION)
    # into it exactly as the player repo's own CMakeLists does.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/third_party/player-shell/VERSION" PLAYER_VERSION)
    string(STRIP "${PLAYER_VERSION}" PLAYER_VERSION)
    string(REPLACE "." "," VER_COMMA "${PLAYER_VERSION}")
    set(VER_DOT "${PLAYER_VERSION}")
    configure_file(third_party/player-shell/version.rc.in
        "${CMAKE_BINARY_DIR}/player-version.rc" @ONLY)

    # The standalone player, built from its vendored Win32 shell against the
    # same engine — one branch, one build, both apps.
    add_executable(minimal-player WIN32
        third_party/player-shell/main.cpp
        "${CMAKE_BINARY_DIR}/player-version.rc"
        third_party/player-shell/app.manifest
    )
    set_target_properties(minimal-player PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
    target_compile_definitions(minimal-player PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(minimal-player PRIVATE /W4 /utf-8)
    target_link_libraries(minimal-player PRIVATE
        player_engine
        shell32 gdi32 comctl32 comdlg32 shlwapi dwmapi
    )
else()
    target_sources(MediaGallery PRIVATE src/player_stub.cpp)
    target_include_directories(MediaGallery PRIVATE third_party/player-engine)
endif()
