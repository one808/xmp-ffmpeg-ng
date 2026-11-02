#!/usr/bin/env bash
# build-plugin.sh - compile and link xmp-ffmpeg.dll (static, no FFmpeg DLLs).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
CROSS=$PWD/sandbox/cross_compilers/mingw-w64-i686
TRIPLE=i686-w64-mingw32
PREFIX=$CROSS/$TRIPLE
CFLAGS_XP="-O2 -march=pentium3 -mtune=athlon-xp -mfpmath=sse -msse"
DEFS_XP=""

export PATH=$CROSS/bin:$PATH
mkdir -p build

echo "== compiling xmp-ffmpeg.cpp =="
$TRIPLE-g++ $CFLAGS_XP $DEFS_XP -Os -fno-exceptions -fno-rtti \
	-Ixmp-sdk -I"$PREFIX/include" \
	-c src/xmp-ffmpeg.cpp -o build/xmp-ffmpeg.o

echo "== linking xmp-ffmpeg.dll (static) =="
# pkg-config resolves the FULL static dependency set (incl. Windows system libs
# like -ladvapi32 for the wincrypt fallback) from the FFmpeg/dep .pc files.
# Note: our toolchain is the win32 thread model + pthreads4w => the library is
# -lpthread (there is no -lwinpthread in this toolchain).
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR=
FFLIBS=$(pkg-config --static --libs libavformat libavcodec libswresample libavutil)
echo "link libs: $FFLIBS"
# g++ driver + -static-libstdc++/-static-libgcc: the dep .pc files pull in
# -lstdc++ (gme is C++), and we must NOT end up importing libstdc++-6.dll /
# libgcc_s_*.dll - everything goes into this single DLL.
$TRIPLE-g++ -shared -o build/xmp-ffmpeg.dll build/xmp-ffmpeg.o \
	-Wl,--kill-at -static-libstdc++ -static-libgcc $FFLIBS \
	-lssp -latomic

# verify the exact export name XMPlay looks for (objdump lists "[index] name")
if ! $TRIPLE-objdump -p build/xmp-ffmpeg.dll | grep -qw "XMPIN_GetInterface"; then
	echo "ERROR: export 'XMPIN_GetInterface' missing:"; $TRIPLE-objdump -p build/xmp-ffmpeg.dll | sed -n '/Export Address Table/,/PE File Base Relocations/p'; exit 1
fi
echo "export check: XMPIN_GetInterface OK"

$TRIPLE-strip --strip-unneeded build/xmp-ffmpeg.dll
ls -la build/xmp-ffmpeg.dll
echo "done."
