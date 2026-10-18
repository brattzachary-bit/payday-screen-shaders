#!/bin/sh
# Cross-builds native/pss.dll (64-bit) with MinGW-w64.
set -e
cd "$(dirname "$0")/src"
CXX=x86_64-w64-mingw32-g++
CC=x86_64-w64-mingw32-gcc
$CC -O2 -c vtbl.c -o /tmp/vtbl.o
$CXX -O2 -std=c++17 -shared -static -static-libgcc -static-libstdc++ \
  -DMODULE_VERSION="\"${1:-0.5.0}\"" \
  bridge.cpp effects.cpp hooks.cpp log.cpp params.cpp plugin.cpp state.cpp /tmp/vtbl.o \
  -ld3d11 -luuid -s -o ../pss.dll
