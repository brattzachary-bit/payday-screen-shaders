#!/bin/sh
# Builds Payday Screen Shaders' native plugin (and the offline test) with MinGW inside the
# pd2-mingw podman image (see Containerfile). Nothing is installed on the host.
#
#   sh build_pss.sh          -> native/pss.dll
#   sh build_pss.sh test     -> also dev/nativebuild/out/pss_test.exe (stale, see dev/README.md)
#   sh build_pss.sh memory   -> also dev/nativebuild/out/test-memory.exe (the memory bridge test)
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
MOD="$ROOT"

podman image exists pd2-mingw || podman build -t pd2-mingw -f "$HERE/Containerfile" "$HERE"

mkdir -p "$HERE/out"

# The plugin reports the version in mod.txt, so the two can never disagree.
MOD_VERSION=$(sed -n 's/.*"version": *"\([^"]*\)".*/\1/p' "$MOD/mod.txt")

podman run --rm --security-opt label=disable \
	-v "$MOD:/mod" -v "$HERE:/build" \
	-e WANT_TEST="$1" -e MOD_VERSION="$MOD_VERSION" \
	pd2-mingw sh -c '
set -e
CXX=x86_64-w64-mingw32-g++
CC=x86_64-w64-mingw32-gcc
SRC=/mod/native/src
OUT=/build/out
FLAGS="-O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -DMODULE_VERSION=\"$MOD_VERSION\""

$CC -O2 -Wall -c $SRC/vtbl.c -o $OUT/vtbl.o
for f in log params state effects hooks bridge plugin; do
	$CXX -std=c++17 $FLAGS -c $SRC/$f.cpp -o $OUT/$f.o
done

$CXX -shared -static -static-libgcc -static-libstdc++ -s \
	$OUT/vtbl.o $OUT/log.o $OUT/params.o $OUT/state.o $OUT/effects.o $OUT/hooks.o $OUT/bridge.o $OUT/plugin.o \
	-o /mod/native/pss.dll -ld3d11 -ldxgi -lgdi32 -luser32

echo "built native/pss.dll"
x86_64-w64-mingw32-objdump -p /mod/native/pss.dll | sed -n "/Export Address/,/^$/p" | grep -E "SuperBLT|MODULE|SBLT" || true
x86_64-w64-mingw32-objdump -p /mod/native/pss.dll | grep "DLL Name" || true

if [ "$WANT_TEST" = "memory" ]; then
	# The memory bridge test (dev/nativebuild/test/memory): no D3D, runs under plain Wine.
	MT=/build/test/memory
	$CXX -std=c++17 $FLAGS -I$SRC -DState=ReferenceState -c $SRC/state.cpp -o $OUT/reference-state.o
	$CXX -std=c++17 $FLAGS -I$SRC -I$MT -Wno-unused-function -Wno-misleading-indentation \
		$MT/test-memory.cpp $MT/reference-bridge.cpp $SRC/state.cpp $SRC/params.cpp $OUT/reference-state.o \
		-static -static-libgcc -static-libstdc++ -o $OUT/test-memory.exe -lgdi32 -luser32
	echo "built out/test-memory.exe"
fi

if [ "$WANT_TEST" = "test" ]; then
	$CXX -std=c++17 $FLAGS -I$SRC -c /build/test/pss_test.cpp -o $OUT/pss_test.o
	$CXX -static -static-libgcc -static-libstdc++ \
		$OUT/pss_test.o $OUT/vtbl.o $OUT/log.o $OUT/params.o $OUT/state.o $OUT/effects.o $OUT/hooks.o $OUT/bridge.o \
		-o $OUT/pss_test.exe -ld3d11 -ldxgi -lgdi32 -luser32
	echo "built out/pss_test.exe"
fi
'
