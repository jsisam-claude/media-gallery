# Cross/native mingw-w64 build (verification / no-MSVC alternative).
# On Windows (MSYS2):  mingw32-make CXX=g++ WINDRES=windres
# make's built-in CXX default (g++) would defeat ?=, so assign directly;
# still overridable from the command line: mingw32-make CXX=g++
CXX     = x86_64-w64-mingw32-g++
WINDRES = x86_64-w64-mingw32-windres

# player_stub.cpp = image-only build; video needs the CMake/MSVC route (BUILDING.md).
SRC  = src/main.cpp src/decoder_gdiplus.cpp src/decoder_wic.cpp src/filmstrip.cpp \
       src/player_stub.cpp
CXXFLAGS = -std=c++17 -O2 -municode -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 \
           -Ithird_party/player-engine -Wall -Wextra -Werror
LIBS = -lgdiplus -lwindowscodecs -lshlwapi -lshell32 -lole32 -loleaut32 -luuid \
       -lcomdlg32 -lgdi32 -luser32
LDFLAGS = -municode -mwindows -static -s -Wl,--dynamicbase -Wl,--nxcompat

PhotoGallery.exe: $(SRC) src/decoder.h src/filmstrip.h src/resource.h \
                  third_party/player-engine/player.h res.o
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) res.o $(LDFLAGS) $(LIBS)

res.o: src/PhotoGallery.rc src/app.manifest src/resource.h
	$(WINDRES) --include-dir=src src/PhotoGallery.rc res.o

clean:
	rm -f PhotoGallery.exe res.o

.PHONY: clean
