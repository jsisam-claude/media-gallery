# Photo Gallery — minimal Windows photo viewer (WinAPI + GDI+/WIC, zero external deps)
#
# Visual Studio:  open this folder ("Open a local folder"), or:
#   cmake -B build -G "Visual Studio 17 2022" -A x64
#   cmake --build build --config Release
cmake_minimum_required(VERSION 3.15)
project(PhotoGallery LANGUAGES CXX)

add_executable(PhotoGallery WIN32
    src/main.cpp
    src/decoder_gdiplus.cpp
    src/decoder_wic.cpp
    src/filmstrip.cpp
    src/PhotoGallery.rc
)

set_target_properties(PhotoGallery PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_compile_definitions(PhotoGallery PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00)

if(MSVC)
    # Hardened, static-CRT release binary; /W4 warnings.
    target_compile_options(PhotoGallery PRIVATE /W4 /EHsc /guard:cf)
    set_property(TARGET PhotoGallery PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(PhotoGallery PRIVATE /DYNAMICBASE /NXCOMPAT /guard:cf /MANIFEST:NO)
else()
    # clang/mingw
    target_compile_options(PhotoGallery PRIVATE -Wall -Wextra -municode)
    target_link_options(PhotoGallery PRIVATE -municode -mwindows -static)
endif()

target_link_libraries(PhotoGallery PRIVATE
    gdiplus windowscodecs shlwapi shell32 ole32 oleaut32 uuid comdlg32 gdi32 user32
)
