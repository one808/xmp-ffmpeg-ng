#!/usr/bin/env bash
# build-plugin.sh - compile and link xmp-ffmpeg.dll (static, no FFmpeg DLLs).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
CROSS=$PWD/sandbox/cross_compilers/mingw-w64-i686
TRIPLE=i686-w64-mingw32
PREFIX=$CROSS/$TRIPLE
CFLAGS_XP="-O2 -march=pentium3 -mtune=athlon-xp -mfpmath=sse -msse"
DEFS_XP="-D_WIN32_WINNT=0x0501"

export PATH=$CROSS/bin:$PATH
mkdir -p build

echo "== compiling xmp-ffmpeg.cpp =="
$TRIPLE-g++ $CFLAGS_XP $DEFS_XP -Os -fno-exceptions -fno-rtti \
	-Ixmp-sdk -I"$PREFIX/include" \
	-c src/xmp-ffmpeg.cpp -o build/xmp-ffmpeg.o

echo "== linking xmp-ffmpeg.dll (static) =="
$TRIPLE-gcc -shared -o build/xmp-ffmpeg.dll build/xmp-ffmpeg.o \
	-Wl,--kill-at \
	-L"$PREFIX/lib" -Wl,--start-group \
	-lavformat -lavcodec -lswresample -lavutil \
	-lfdk-aac -lopencore-amrnb -lopencore-amrwb -lgme -lcodec2 \
	-lz -lbz2 -llzma \
	-Wl,--end-group \
	-lwinpthread -lssp -lws2_32 -lole32 -luuid -lshlwapi -lpsapi -lwinmm -lm -latomic

$TRIPLE-strip --strip-unneeded build/xmp-ffmpeg.dll
ls -la build/xmp-ffmpeg.dll
echo "done."
