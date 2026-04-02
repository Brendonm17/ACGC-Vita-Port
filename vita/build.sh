#!/bin/bash
# build on ext4 for fast I/O (10-50x vs NTFS)
# usage: ./build.sh [clean|sync|vpk]
set -e

VITASDK=/usr/local/vitasdk
export VITASDK
export PATH=$VITASDK/bin:$PATH

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WIN_SRC="${WIN_SRC:-$(cd "$SCRIPT_DIR/.." && pwd)}"
EXT4_SRC="${EXT4_SRC:-$HOME/ac_vita_build/ACGC-PC-Port}"
EXT4_BUILD="$EXT4_SRC/vita/build"
NPROC=$(nproc)

export CCACHE_DIR="$HOME/.ccache_vita"
export CCACHE_MAXSIZE="4G"
export CCACHE_COMPRESS=1
export CCACHE_COMPRESSLEVEL=6

sync_sources() {
    echo "Syncing to ext4..."
    mkdir -p "$EXT4_SRC"
    rsync -a --delete \
        --exclude='vita/build/' --exclude='pc/build*/' --exclude='.git/' \
        --exclude='*.o' --exclude='*.obj' --exclude='*.vpk' \
        --exclude='*.elf' --exclude='*.velf' --exclude='*.bin' \
        "$WIN_SRC/" "$EXT4_SRC/"
}

configure() {
    mkdir -p "$EXT4_BUILD"
    cd "$EXT4_BUILD"
    cmake .. -DCMAKE_BUILD_TYPE=Release
}

build() {
    cd "$EXT4_BUILD"
    [ ! -f Makefile ] && configure
    echo "Building ($NPROC cores)..."
    time make -j$NPROC 2>&1
}

copy_vpk() {
    local vpk="$EXT4_BUILD/ac_vita.vpk"
    local dest="$WIN_SRC/vita/build/"
    [ ! -f "$vpk" ] && echo "VPK not found" && return 1
    mkdir -p "$dest"
    cp "$vpk" "$dest/"
    echo "VPK: $dest/ac_vita.vpk"
}

case "${1:-build}" in
    clean)   rm -rf "$EXT4_BUILD"; sync_sources; configure; build ;;
    sync)    sync_sources ;;
    vpk)     sync_sources; build; copy_vpk ;;
    build|"") sync_sources; build ;;
    *)       echo "Usage: $0 [build|clean|sync|vpk]"; exit 1 ;;
esac

ccache -s 2>/dev/null | grep -E "Hits|Misses|cache size" || true
