#!/bin/bash
# Rebuild vitaGL with GLSL support + performance speedhacks + vglMul fix
# Based on Barony vita port script with additional speedhacks for AC

set -e
export VITASDK=/usr/local/vitasdk
export PATH="$VITASDK/bin:$PATH"

VITAGL_DIR=/tmp/vitaGL-master

# Always re-download to get latest source
if [ -d "$VITAGL_DIR" ]; then
    echo "--- Removing old vitaGL source ---"
    rm -rf "$VITAGL_DIR"
fi

if [ ! -d "$VITAGL_DIR" ]; then
    echo "--- Downloading vitaGL source ---"
    cd /tmp
    wget -q https://github.com/Rinnegatamante/vitaGL/archive/refs/heads/master.zip -O vitaGL.zip
    unzip -q vitaGL.zip
    rm vitaGL.zip
fi

cd "$VITAGL_DIR"

# Patch vglMul mat*mat: M1*M2 -> M2*M1 (CG row-major fix)
HDR_FILE="source/shaders/glsl_translator_hdr.h"
if [ -f "$HDR_FILE" ]; then
    if grep -q 'return M1 \* M2' "$HDR_FILE"; then
        echo "--- Patching vglMul mat*mat: M1*M2 -> M2*M1 ---"
        sed -i 's/return M1 \* M2/return M2 \* M1/g' "$HDR_FILE"
    else
        echo "--- vglMul already patched ---"
    fi
fi

VITAGL_FLAGS="HAVE_GLSL_SUPPORT=1 CIRCULAR_VERTEX_POOL=2 BUFFERS_SPEEDHACK=1 SHADER_COMPILER_SPEEDHACK=1 DRAW_SPEEDHACK=1 SAMPLERS_SPEEDHACK=1 PRIMITIVES_SPEEDHACK=1 USE_SCRATCH_MEMORY=1 HAVE_SHADER_CACHE=1 NO_DEBUG=1"

echo "--- Building vitaGL ($VITAGL_FLAGS) ---"
make clean 2>/dev/null || true
make -j$(nproc) $VITAGL_FLAGS

echo "--- Installing ---"
make $VITAGL_FLAGS install

echo "--- Done ---"
