#!/bin/bash
# Rebuild vitaGL with GLSL support + speedhacks + vglMul fix
# Pulls from the AC Vita port's fork of vitaGL which adds
# vglPrepareCompressedTexture2D / vglCommitPendingTexture for off-thread
# block-compressed texture uploads. Needed for smooth HD texture pack loads.

set -e
export VITASDK="${VITASDK:-/usr/local/vitasdk}"
export PATH="$VITASDK/bin:$PATH"

VITAGL_REPO=https://github.com/Brendonm17/vitaGL.git
VITAGL_BRANCH=async-compressed-tex-prep
VITAGL_DIR=/tmp/vitaGL-acvita
# optional: set VITAGL_LOCAL to a local checkout to build from instead of cloning
VITAGL_LOCAL="${VITAGL_LOCAL:-}"

if [ -n "$VITAGL_LOCAL" ] && [ -d "$VITAGL_LOCAL" ]; then
    echo "--- Using local vitaGL fork at $VITAGL_LOCAL ---"
    if [ -d "$VITAGL_DIR" ]; then rm -rf "$VITAGL_DIR"; fi
    rsync -a --exclude='.git' "$VITAGL_LOCAL/" "$VITAGL_DIR/"
else
    if [ -d "$VITAGL_DIR" ]; then
        echo "--- Removing old vitaGL source ---"
        rm -rf "$VITAGL_DIR"
    fi
    echo "--- Cloning vitaGL fork ($VITAGL_BRANCH) ---"
    git clone --depth 1 --branch "$VITAGL_BRANCH" "$VITAGL_REPO" "$VITAGL_DIR"
fi

cd "$VITAGL_DIR"

# patch vglMul mat*mat M1*M2 -> M2*M1: psp2cgc compiles Cg row-major, so the
# translated GLSL product comes out transposed. Kept local to stay near upstream.
HDR_FILE="source/shaders/glsl_translator_hdr.h"
if [ -f "$HDR_FILE" ]; then
    if grep -q 'return M1 \* M2' "$HDR_FILE"; then
        echo "--- Patching vglMul mat*mat: M1*M2 -> M2*M1 ---"
        sed -i 's/return M1 \* M2/return M2 \* M1/g' "$HDR_FILE"
    else
        echo "--- vglMul already patched ---"
    fi
fi

# TEXTURES_SPEEDHACK required: fast_draw_mode skips tex->last_frame, immediate
# frees would race in-flight GPU reads. USE_SCRATCH_MEMORY off: pool wrap corrupts.
VITAGL_FLAGS="BUFFERS_SPEEDHACK=1 DRAW_SPEEDHACK=1 SAMPLERS_SPEEDHACK=1 PRIMITIVES_SPEEDHACK=1 TEXTURES_SPEEDHACK=1 HAVE_SHADER_CACHE=1 NO_DEBUG=1 DRAW_STATE_CACHE=1 PHYCONT_ON_DEMAND=1"

echo "--- Building vitaGL ($VITAGL_FLAGS) ---"
make clean 2>/dev/null || true
make -j$(nproc) $VITAGL_FLAGS

echo "--- Installing ---"
make $VITAGL_FLAGS install

echo "--- Done ---"
