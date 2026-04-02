#!/bin/bash
# full pipeline: shaders -> header -> sync -> build -> copy VPK
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

echo "Compiling shaders..."
cd "$WIN_SRC/vita/shaders"
bash compile_new_shaders.sh

echo "Generating GXP header..."
python3 gen_header.py

echo "Syncing to ext4..."
mkdir -p "$EXT4_SRC"
rsync -a --delete \
    --exclude='vita/build/' --exclude='pc/build*/' --exclude='.git/' \
    --exclude='*.o' --exclude='*.obj' --exclude='*.vpk' \
    --exclude='*.elf' --exclude='*.velf' --exclude='*.bin' \
    "$WIN_SRC/" "$EXT4_SRC/"

echo "Building ($NPROC cores)..."
mkdir -p "$EXT4_BUILD"
cd "$EXT4_BUILD"
cmake .. -DCMAKE_BUILD_TYPE=Release
time make -j$NPROC 2>&1

VPK="$EXT4_BUILD/ac_vita.vpk"
DEST="$WIN_SRC/vita/build/"
if [ -f "$VPK" ]; then
    mkdir -p "$DEST"
    cp "$VPK" "$DEST/"
    echo "VPK: $DEST/ac_vita.vpk"
    ls -lh "$DEST/ac_vita.vpk"
else
    echo "VPK not found!"
    exit 1
fi

ccache -s 2>/dev/null | grep -E "Hits|Misses|cache size" || true
