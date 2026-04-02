#!/bin/bash
# incremental build (run from ext4)
# usage: ./build_vita.sh [reconfig|clean]
set -e

export VITASDK=/usr/local/vitasdk
export PATH="$VITASDK/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

BUILD_DIR="$HOME/ac_vita_build/ACGC-PC-Port/vita/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ "$1" = "clean" ]; then
    rm -rf "$BUILD_DIR"/*
fi

if [ ! -f "CMakeCache.txt" ] || [ "$1" = "reconfig" ] || [ "$1" = "clean" ]; then
    rm -f CMakeCache.txt
    cmake .. -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" 2>&1
fi

cmake --build . -j$(nproc) 2>&1
ls -lh ac_vita.vpk 2>/dev/null || echo "No VPK generated"
