#!/bin/bash
# Rebuild vitaGL with GLSL support + speedhacks + vglMul fix
# Pulls from the AC Vita port's fork of vitaGL which adds
# vglPrepareCompressedTexture2D / vglCommitPendingTexture for off-thread
# block-compressed texture uploads. Needed for smooth HD texture pack loads.

set -e
export VITASDK=/usr/local/vitasdk
export PATH="$VITASDK/bin:$PATH"

VITAGL_REPO=https://github.com/Brendonm17/vitaGL.git
VITAGL_BRANCH=async-compressed-tex-prep
VITAGL_DIR=/tmp/vitaGL-acvita
# Local fork directory for in-progress edits (overrides git clone if present)
VITAGL_LOCAL="/mnt/c/Users/Brendon Moncada/Desktop/vitaGL"

if [ -d "$VITAGL_LOCAL" ]; then
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

# Patch vglMul mat*mat: M1*M2 -> M2*M1
#
# vitaGL's GLSL-to-Cg translator wraps every matrix-times-matrix in a
# helper called vglMul, defined in source/shaders/glsl_translator_hdr.h.
# GLSL is column-major; psp2cgc compiles the translated shader as Cg
# row-major. In that mode, `M1 * M2` yields the transpose of what the
# original GLSL expression meant, so every MVP chain comes out inverted
# and geometry renders in the wrong place (or not at all).
#
# Swapping to `M2 * M1` cancels out the row-major/column-major flip and
# restores the intended GLSL semantics. Keeping the patch local here
# (instead of committing it to the vitaGL fork) lets the fork stay
# close to upstream - other projects using vitaGL without the shader
# translator rely on the unpatched order.
HDR_FILE="source/shaders/glsl_translator_hdr.h"
if [ -f "$HDR_FILE" ]; then
    if grep -q 'return M1 \* M2' "$HDR_FILE"; then
        echo "--- Patching vglMul mat*mat: M1*M2 -> M2*M1 ---"
        sed -i 's/return M1 \* M2/return M2 \* M1/g' "$HDR_FILE"
    else
        echo "--- vglMul already patched ---"
    fi
fi

# UNIFORM_VALUE_CACHE: TRIED AND REVERTED.
# The idea: track last uploaded uniform values, skip sceGxmSetUniformDataF
# when value unchanged, carry forward via bulk memcpy previous buffer into
# new buffer. FATAL FLAW: the uniform circular pool is GPU-visible memory
# (write-combined). CPU READS from WC memory are ~50 MB/s vs ~2 GB/s writes.
# The bulk memcpy read stalled catastrophically: unif phase went 4.5ms ->
# 10.3ms (-8 fps regression) in dense scenes. Do NOT re-enable without a
# different approach (e.g., CPU-cached shadow buffer + tracked offsets).
# TEXTURES_SPEEDHACK is required for us: vgl_fast_draw_mode bypasses
# VitaGL's texture loop so tex->last_frame never gets updated, and the
# default free path would take the immediate-free branch while the GPU
# may still be sampling. SPEEDHACK forces markAsDirty (4-frame deferred).
# USE_SCRATCH_MEMORY removed: routes DYNAMIC/STREAM VBOs through
# vgl_reserve_data_pool (the 256KB circular data pool from vglInitExtended).
# when the pool wraps before the GPU finishes reading last frame's data,
# old verts/textures get overwritten - corruption shows up as garbled
# framebuffer tiles on title screens / scene loads. our fork has no
# failsafe option, so removing the flag is the safe call.
VITAGL_FLAGS="BUFFERS_SPEEDHACK=1 DRAW_SPEEDHACK=1 SAMPLERS_SPEEDHACK=1 PRIMITIVES_SPEEDHACK=1 TEXTURES_SPEEDHACK=1 HAVE_SHADER_CACHE=1 NO_DEBUG=1 DRAW_STATE_CACHE=1"

echo "--- Building vitaGL ($VITAGL_FLAGS) ---"
make clean 2>/dev/null || true
make -j$(nproc) $VITAGL_FLAGS

echo "--- Installing ---"
make $VITAGL_FLAGS install

echo "--- Done ---"
