# Vendor the minimal-player video engine + its vendored FFmpeg into this
# repo as plain committed source text:
#   cmake -P tools/vendor_player.cmake <path-to-vlc-light-win64>
#
# Copies the engine sources into third_party/player-engine/, the player's
# own Win32 shell into third_party/player-shell/ (built here as the
# standalone minimal-player.exe, so one branch yields both apps), and the
# player repo's already-flattened FFmpeg subset + committed config + build
# description verbatim, mirroring its layout so cmake/ffmpeg.cmake works
# unmodified from this repo's root.
# This script is the ONLY way content enters those paths; re-running it
# against a newer player checkout is the entire upgrade story. The player
# repo is only needed when vendoring/upgrading, never to build.
cmake_minimum_required(VERSION 3.24)

if(NOT CMAKE_ARGV3)
  message(FATAL_ERROR "usage: cmake -P tools/vendor_player.cmake <player-repo>")
endif()
get_filename_component(SRC "${CMAKE_ARGV3}" ABSOLUTE)
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# Engine translation units + headers (the library both apps link), and the
# player's bundled Win32 shell (kept in its own directory — this repo's
# gallery host code lives in src/main.cpp, unrelated to the shell's).
set(ENGINE player.h player_int.h player.cpp demux.cpp decode.cpp
    queue.cpp audio_out.cpp video_out.cpp subs.cpp)
set(SHELL main.cpp version.rc app.manifest)

# Sanity: refuse to run against anything but a player repo checkout.
set(checks LICENSE cmake/ffmpeg.cmake cmake/ffmpeg_sources.txt
    third_party/ffmpeg-src/libavcodec/avcodec.h
    third_party/ffmpeg-config/config.h)
foreach(f ${ENGINE} ${SHELL})
  list(APPEND checks "src/${f}")
endforeach()
foreach(f ${checks})
  if(NOT EXISTS "${SRC}/${f}")
    message(FATAL_ERROR "not a player repo: missing ${SRC}/${f}")
  endif()
endforeach()
if(SRC STREQUAL ROOT)
  message(FATAL_ERROR "source and destination are the same tree")
endif()

# 1) Engine -> third_party/player-engine (wipe + copy so upstream
#    deletions propagate), plus the engine's license text.
file(REMOVE_RECURSE "${ROOT}/third_party/player-engine")
file(MAKE_DIRECTORY "${ROOT}/third_party/player-engine")
foreach(f ${ENGINE})
  file(COPY_FILE "${SRC}/src/${f}" "${ROOT}/third_party/player-engine/${f}")
endforeach()
file(COPY_FILE "${SRC}/LICENSE" "${ROOT}/third_party/player-engine/LICENSE")

# 1b) The player's own Win32 shell -> third_party/player-shell (compiled
#     here as the standalone minimal-player.exe alongside the gallery).
file(REMOVE_RECURSE "${ROOT}/third_party/player-shell")
file(MAKE_DIRECTORY "${ROOT}/third_party/player-shell")
foreach(f ${SHELL})
  file(COPY_FILE "${SRC}/src/${f}" "${ROOT}/third_party/player-shell/${f}")
endforeach()

# 2) FFmpeg subset + committed config, verbatim and layout-mirrored so the
#    CMAKE_SOURCE_DIR-relative paths inside ffmpeg.cmake keep resolving.
set(ffcount 0)
foreach(dir ffmpeg-src ffmpeg-config)
  file(REMOVE_RECURSE "${ROOT}/third_party/${dir}")
  file(GLOB_RECURSE rels RELATIVE "${SRC}/third_party/${dir}"
       "${SRC}/third_party/${dir}/*")
  list(SORT rels)
  foreach(rel ${rels})
    get_filename_component(d "${ROOT}/third_party/${dir}/${rel}" DIRECTORY)
    file(MAKE_DIRECTORY "${d}")
    file(COPY_FILE "${SRC}/third_party/${dir}/${rel}"
                   "${ROOT}/third_party/${dir}/${rel}")
    math(EXPR ffcount "${ffcount} + 1")
  endforeach()
endforeach()

# 3) The FFmpeg build description, verbatim.
file(MAKE_DIRECTORY "${ROOT}/cmake")
file(COPY_FILE "${SRC}/cmake/ffmpeg.cmake"       "${ROOT}/cmake/ffmpeg.cmake")
file(COPY_FILE "${SRC}/cmake/ffmpeg_sources.txt" "${ROOT}/cmake/ffmpeg_sources.txt")

# 4) Provenance.
set(commit "unknown")
execute_process(COMMAND git -C "${SRC}" rev-parse HEAD
                OUTPUT_VARIABLE gitout OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET RESULT_VARIABLE gitres)
if(gitres EQUAL 0 AND NOT gitout STREQUAL "")
  set(commit "${gitout}")
endif()
list(LENGTH ENGINE ncount)
list(LENGTH SHELL scount)
file(WRITE "${ROOT}/third_party/player-engine/PROVENANCE.txt"
  "Origin: vlc-light-win64 (minimal-player engine)\n"
  "Commit: ${commit}\n"
  "Files: ${ncount} engine sources (+ LICENSE), ${scount} shell files in "
  "../player-shell, plus ${ffcount} files "
  "mirrored verbatim into third_party/ffmpeg-src and "
  "third_party/ffmpeg-config (see their own PROVENANCE.txt) and "
  "cmake/ffmpeg.cmake + cmake/ffmpeg_sources.txt.\n"
  "Vendored by tools/vendor_player.cmake; unmodified upstream text.\n"
  "License: LGPL-2.1-or-later (LICENSE in this directory).\n")

message(STATUS "vendored ${ncount} engine + ${scount} shell + ${ffcount} ffmpeg files -> ${ROOT}/third_party")
