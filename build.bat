@echo off
rem Photo Gallery — plain MSVC build, no CMake needed.
rem Run from a "x64 Native Tools Command Prompt for VS" in the repo root.
setlocal

rc /nologo /I src /fo PhotoGallery.res src\PhotoGallery.rc || exit /b 1

rem player_stub.cpp = image-only build; video needs the CMake route (BUILDING.md).
cl /nologo /std:c++17 /O2 /W4 /EHsc /MT /GS /guard:cf ^
   /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 ^
   /I third_party\player-engine ^
   src\main.cpp src\decoder_gdiplus.cpp src\decoder_wic.cpp src\filmstrip.cpp ^
   src\player_stub.cpp PhotoGallery.res /Fe:PhotoGallery.exe ^
   /link /SUBSYSTEM:WINDOWS /DYNAMICBASE /NXCOMPAT /guard:cf /MANIFEST:NO ^
   gdiplus.lib windowscodecs.lib shlwapi.lib shell32.lib ole32.lib oleaut32.lib ^
   uuid.lib comdlg32.lib gdi32.lib user32.lib || exit /b 1

del /q main.obj decoder_gdiplus.obj decoder_wic.obj filmstrip.obj player_stub.obj PhotoGallery.res 2>nul
echo Built PhotoGallery.exe
