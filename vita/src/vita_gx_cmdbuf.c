// vita_gx_cmdbuf.c
// double-buffered draw command queue, GL state cache, submit_frame replay

#ifdef TARGET_VITA

#include "pc_gx_internal.h"
#include "pc_settings.h"
#include "vita_shared.h"
#include "vita_banner.h"
#include "vita_gx_cmdbuf.h"
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/gxm.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <arm_neon.h>
#include <dolphin/gx/GXEnum.h>

static inline float vita_color_src(int gx_input, int stage);
static inline float vita_alpha_src(int gx_input, int stage);
static inline void  vita_resolve_cc_cmd(int input, const PCGXTevStage* ts,
                                        const float colors[4][4],
                                        const float k_colors[4][4],
                                        float out[3]);
static inline float vita_resolve_ca_cmd(int input, const PCGXTevStage* ts,
                                        const float colors[4][4],
                                        const float k_colors[4][4]);

void vita_cmdbuf_prededup(void) {
    extern volatile int pdd_buffer_idx;
    int wr = pdd_buffer_idx;
    int count = cmd_queue_count_db[wr];
    if (count <= 0) return;

    static PCGXTevStage last_stages[PC_GX_MAX_TEV_STAGES];
    static float        last_resolve_colors[4][4];
    static float        last_resolve_k_colors[4][4];
    static int          last_resolve_num = -1;
    static int          last_resolve_valid = 0;
    static float        last_resolved[360 / sizeof(float)];
    static int          last_resolve_tc_src[PC_GX_MAX_TEV_STAGES];
    static const float  r_bias_lut[]  = {0.0f, 0.5f, -0.5f};
    static const float  r_scale_lut[] = {1.0f, 2.0f, 4.0f, 0.5f};

    extern int g_vita_force_state_resync;
    if (g_vita_force_state_resync) {
        last_resolve_valid = 0;
    }

    float            last_proj[16];
    float            last_pos_mtx[16];
    float            last_nrm_mtx[9];
    PCGXCmdTextures  last_tex;
    PCGXCmdFog       last_fog;
    float            last_tev_colors[4][4];
    float            last_konst[4][4];
    PCGXCmdGLState   last_glstate;
    PCGXCmdAlpha     last_alpha;
    PCGXCmdTexGen    last_texgen;
    PCGXCmdTEV       last_tev;
    PCGXCmdIndirect  last_indirect;

    unsigned int valid = 0;
    #define V_PROJ      (1u << 0)
    #define V_MV        (1u << 1)
    #define V_TEX       (1u << 2)
    #define V_FOG       (1u << 3)
    #define V_TEVCOL    (1u << 4)
    #define V_KONST     (1u << 5)
    #define V_GLSTATE   (1u << 7)
    #define V_ALPHA     (1u << 8)
    #define V_TEXGEN    (1u << 9)
    #define V_TEV       (1u << 10)
    #define V_INDIRECT  (1u << 11)

    GLuint last_shader = 0;

    #define GL_STATE_MASK (PC_GX_DIRTY_DEPTH | PC_GX_DIRTY_COLOR_MASK | \
                           PC_GX_DIRTY_CULL  | PC_GX_DIRTY_BLEND)

    for (int i = 0; i < count; i++) {
        PCGXDrawCmd* c = &cmd_queue_db[wr][i];
        if (c->shader == 0) continue;
        if (i + 1 < count) {
            PCGXDrawCmd* nc = &cmd_queue_db[wr][i + 1];
            __builtin_prefetch(nc, 0, 0);
            __builtin_prefetch((char*)nc + 64, 0, 0);
            __builtin_prefetch((char*)nc + 128, 0, 0);
        }

        // tc_src is resolved here from cmd->tev.stages[s].tex_coord; gate
        // includes DIRTY_TEXTURES so the deferred-upload patch path (which
        // sets DIRTY_TEXTURES alone) refreshes tc_src.
        if (c->dirty & (PC_GX_DIRTY_TEV_STAGES | PC_GX_DIRTY_TEV_COLORS | PC_GX_DIRTY_KONST | PC_GX_DIRTY_TEXTURES)) {
            int rn = c->tev.num_stages;
            if (rn > PC_GX_MAX_TEV_STAGES) rn = PC_GX_MAX_TEV_STAGES;
            int r_matches = last_resolve_valid && rn == last_resolve_num &&
                (rn == 0 ||
                 __builtin_memcmp(c->tev.stages, last_stages, rn * sizeof(PCGXTevStage)) == 0) &&
                __builtin_memcmp(c->tev.colors, last_resolve_colors, sizeof(c->tev.colors)) == 0 &&
                __builtin_memcmp(c->tev.k_colors, last_resolve_k_colors, sizeof(c->tev.k_colors)) == 0;
            if (r_matches) {
                __builtin_memcpy(&c->tev.ca[0][0], last_resolved, 360);
                for (int s = 0; s < PC_GX_MAX_TEV_STAGES; s++) c->tev.tc_src[s] = last_resolve_tc_src[s];
            } else {
                for (int s = 0; s < rn; s++) {
                    PCGXTevStage* ts = &c->tev.stages[s];
                    vita_resolve_cc_cmd(ts->color_a, ts, c->tev.colors, c->tev.k_colors, c->tev.ca[s]);
                    vita_resolve_cc_cmd(ts->color_b, ts, c->tev.colors, c->tev.k_colors, c->tev.cb[s]);
                    vita_resolve_cc_cmd(ts->color_c, ts, c->tev.colors, c->tev.k_colors, c->tev.cc[s]);
                    vita_resolve_cc_cmd(ts->color_d, ts, c->tev.colors, c->tev.k_colors, c->tev.cd[s]);
                    c->tev.aval[s][0] = vita_resolve_ca_cmd(ts->alpha_a, ts, c->tev.colors, c->tev.k_colors);
                    c->tev.aval[s][1] = vita_resolve_ca_cmd(ts->alpha_b, ts, c->tev.colors, c->tev.k_colors);
                    c->tev.aval[s][2] = vita_resolve_ca_cmd(ts->alpha_c, ts, c->tev.colors, c->tev.k_colors);
                    c->tev.aval[s][3] = vita_resolve_ca_cmd(ts->alpha_d, ts, c->tev.colors, c->tev.k_colors);
                    c->tev.csrc[s][0] = vita_color_src(ts->color_a, s);
                    c->tev.csrc[s][1] = vita_color_src(ts->color_b, s);
                    c->tev.csrc[s][2] = vita_color_src(ts->color_c, s);
                    c->tev.csrc[s][3] = vita_color_src(ts->color_d, s);
                    c->tev.asrc[s][0] = vita_alpha_src(ts->alpha_a, s);
                    c->tev.asrc[s][1] = vita_alpha_src(ts->alpha_b, s);
                    c->tev.asrc[s][2] = vita_alpha_src(ts->alpha_c, s);
                    c->tev.asrc[s][3] = vita_alpha_src(ts->alpha_d, s);
                    c->tev.param[s][0] = r_bias_lut[ts->color_bias < 3 ? ts->color_bias : 0];
                    c->tev.param[s][1] = r_bias_lut[ts->alpha_bias < 3 ? ts->alpha_bias : 0];
                    c->tev.param[s][2] = r_scale_lut[ts->color_scale < 4 ? ts->color_scale : 0];
                    c->tev.param[s][3] = ts->color_op ? -1.0f : 1.0f;
                    c->tev.aparam[s][0] = r_scale_lut[ts->alpha_scale < 4 ? ts->alpha_scale : 0];
                    c->tev.aparam[s][1] = ts->alpha_op ? -1.0f : 1.0f;
                }
                for (int s = 0; s < PC_GX_MAX_TEV_STAGES; s++) {
                    int tc_src = 0;
                    if (s < rn) {
                        int tc = c->tev.stages[s].tex_coord;
                        tc_src = (tc >= 0 && tc < 8) ? tc : s;
                    }
                    c->tev.tc_src[s] = tc_src;
                }
                last_resolve_num = rn;
                if (rn > 0)
                    __builtin_memcpy(last_stages, c->tev.stages, rn * sizeof(PCGXTevStage));
                __builtin_memcpy(last_resolve_colors,   c->tev.colors,   sizeof(last_resolve_colors));
                __builtin_memcpy(last_resolve_k_colors, c->tev.k_colors, sizeof(last_resolve_k_colors));
                __builtin_memcpy(last_resolved, &c->tev.ca[0][0], 360);
                for (int s = 0; s < PC_GX_MAX_TEV_STAGES; s++) last_resolve_tc_src[s] = c->tev.tc_src[s];
                last_resolve_valid = 1;
            }
            // cfg20: stage 1 samples via unit 0; mirror tc_src[0].
            if (c->tev_tex_remap) {
                c->tev.tc_src[0] = c->tev.tc_src[1];
            }
        }

        if (c->shader_changed || c->shader != last_shader) {
            valid = 0;
            last_shader = c->shader;
        }

        unsigned int d = c->dirty;
        unsigned int nd = d;

        if (d & PC_GX_DIRTY_PROJECTION) {
            if ((valid & V_PROJ) &&
                __builtin_memcmp(c->transform.projection_mtx_t, last_proj, 64) == 0) {
                nd &= ~PC_GX_DIRTY_PROJECTION;
            } else {
                __builtin_memcpy(last_proj, c->transform.projection_mtx_t, 64);
                valid |= V_PROJ;
            }
        }

        if (d & PC_GX_DIRTY_MODELVIEW) {
            if ((valid & V_MV) &&
                __builtin_memcmp(c->transform.pos_mtx_t, last_pos_mtx, 64) == 0 &&
                __builtin_memcmp(c->transform.nrm_mtx_t, last_nrm_mtx, 36) == 0) {
                nd &= ~PC_GX_DIRTY_MODELVIEW;
            } else {
                __builtin_memcpy(last_pos_mtx, c->transform.pos_mtx_t, 64);
                __builtin_memcpy(last_nrm_mtx, c->transform.nrm_mtx_t, 36);
                valid |= V_MV;
            }
        }

        if (d & PC_GX_DIRTY_TEXTURES) {
            if ((valid & V_TEX) &&
                __builtin_memcmp(&c->textures, &last_tex, sizeof(PCGXCmdTextures)) == 0) {
                nd &= ~PC_GX_DIRTY_TEXTURES;
            } else {
                last_tex = c->textures;
                valid |= V_TEX;
            }
        }

        if (d & PC_GX_DIRTY_FOG) {
            if ((valid & V_FOG) &&
                __builtin_memcmp(&c->fog, &last_fog, sizeof(PCGXCmdFog)) == 0) {
                nd &= ~PC_GX_DIRTY_FOG;
            } else {
                last_fog = c->fog;
                valid |= V_FOG;
            }
        }

        if (d & PC_GX_DIRTY_TEV_COLORS) {
            if ((valid & V_TEVCOL) &&
                __builtin_memcmp(c->tev.colors, last_tev_colors, 64) == 0) {
                nd &= ~PC_GX_DIRTY_TEV_COLORS;
            } else {
                __builtin_memcpy(last_tev_colors, c->tev.colors, 64);
                valid |= V_TEVCOL;
            }
        }

        if (d & PC_GX_DIRTY_KONST) {
            if ((valid & V_KONST) &&
                __builtin_memcmp(c->tev.k_colors, last_konst, 64) == 0) {
                nd &= ~PC_GX_DIRTY_KONST;
            } else {
                __builtin_memcpy(last_konst, c->tev.k_colors, 64);
                valid |= V_KONST;
            }
        }

        if (d & GL_STATE_MASK) {
            if ((valid & V_GLSTATE) &&
                __builtin_memcmp(&c->gl_state, &last_glstate, sizeof(PCGXCmdGLState)) == 0) {
                nd &= ~(d & GL_STATE_MASK);
            } else {
                last_glstate = c->gl_state;
                valid |= V_GLSTATE;
            }
        }

        if (d & PC_GX_DIRTY_ALPHA_CMP) {
            if ((valid & V_ALPHA) &&
                __builtin_memcmp(&c->alpha, &last_alpha, sizeof(PCGXCmdAlpha)) == 0) {
                nd &= ~PC_GX_DIRTY_ALPHA_CMP;
            } else {
                last_alpha = c->alpha;
                valid |= V_ALPHA;
            }
        }

        if (d & PC_GX_DIRTY_TEXGEN) {
            if ((valid & V_TEXGEN) &&
                __builtin_memcmp(&c->texgen, &last_texgen, sizeof(PCGXCmdTexGen)) == 0) {
                nd &= ~PC_GX_DIRTY_TEXGEN;
            } else {
                last_texgen = c->texgen;
                valid |= V_TEXGEN;
            }
        }

        if (d & (PC_GX_DIRTY_TEV_STAGES | PC_GX_DIRTY_SWAP_TABLES)) {
            if ((valid & V_TEV) &&
                __builtin_memcmp(&c->tev, &last_tev, offsetof(PCGXCmdTEV, colors)) == 0) {
                nd &= ~(PC_GX_DIRTY_TEV_STAGES | PC_GX_DIRTY_SWAP_TABLES);
            } else {
                __builtin_memcpy(&last_tev, &c->tev, offsetof(PCGXCmdTEV, colors));
                valid |= V_TEV;
            }
        }

        if (d & PC_GX_DIRTY_INDIRECT) {
            if ((valid & V_INDIRECT) &&
                __builtin_memcmp(&c->indirect, &last_indirect, sizeof(PCGXCmdIndirect)) == 0) {
                nd &= ~PC_GX_DIRTY_INDIRECT;
            } else {
                last_indirect = c->indirect;
                valid |= V_INDIRECT;
            }
        }

        c->dirty = nd;
    }

    #undef V_PROJ
    #undef V_MV
    #undef V_TEX
    #undef V_FOG
    #undef V_TEVCOL
    #undef V_KONST
    #undef V_GLSTATE
    #undef V_ALPHA
    #undef V_TEXGEN
    #undef V_TEV
    #undef V_INDIRECT
    #undef GL_STATE_MASK
}

// stub; prior cull attempts produced false positives.
void vita_cmdbuf_frustum_cull(void) {
    vita_stats.culled_draws = 0;
}

// GL state cache

static struct {
    int depth_test;         // -1=unknown, 0=disabled, 1=enabled
    GLenum depth_func;
    int depth_mask;         // -1=unknown, 0=GL_FALSE, 1=GL_TRUE
    int cull_face;          // -1=unknown, 0=disabled, 1=enabled
    GLenum cull_mode;
    int blend;              // -1=unknown, 0=disabled, 1=enabled
    GLenum blend_src, blend_dst;
    GLenum blend_eq;
    int color_mask_rgb;     // -1=unknown
    int color_mask_a;
    GLenum active_texture;
    // Viewport/scissor cache (submit_frame replay)
    int vp_x, vp_y, vp_w, vp_h;
    float depth_near, depth_far;
    int sc_x, sc_y, sc_w, sc_h;
    int scissor_test_enabled; // -1 unknown, 0 disabled, 1 enabled
} gl_cache;

static GLuint gl_cache_bound_tex[8];
static u32 gl_cache_wrap_s[8]; // GX wrap currently set on bound texture (0xFF = unknown)
static u32 gl_cache_wrap_t[8];

static void gl_cache_reset_textures(void) {
    for (int i = 0; i < 8; i++) gl_cache_bound_tex[i] = 0;
    for (int i = 0; i < 8; i++) { gl_cache_wrap_s[i] = 0xFF; gl_cache_wrap_t[i] = 0xFF; }
}

static inline void gl_cache_active_texture(GLenum unit) {
    if (gl_cache.active_texture != unit) {
        glActiveTexture(unit);
        gl_cache.active_texture = unit;
    }
}

static inline void gl_cache_bind_texture(GLenum unit, GLuint tex) {
    int idx = unit - GL_TEXTURE0;
    if (gl_cache_bound_tex[idx] != tex) {
        gl_cache_active_texture(unit);
        glBindTexture(GL_TEXTURE_2D, tex);
        gl_cache_bound_tex[idx] = tex;
        gl_cache_wrap_s[idx] = 0xFF; // wrap unknown for new texture
        gl_cache_wrap_t[idx] = 0xFF;
    }
}

void gl_cache_reset(void) {
    gl_cache.depth_test = -1;
    gl_cache.depth_func = 0;
    gl_cache.depth_mask = -1;
    gl_cache.cull_face = -1;
    gl_cache.cull_mode = 0;
    gl_cache.blend = -1;
    gl_cache.blend_src = 0;
    gl_cache.blend_dst = 0;
    gl_cache.blend_eq = GL_FUNC_ADD;
    gl_cache.color_mask_rgb = -1;
    gl_cache.color_mask_a = -1;
    gl_cache.active_texture = GL_TEXTURE0;
    gl_cache.vp_x = -1; gl_cache.vp_y = -1;
    gl_cache.vp_w = -1; gl_cache.vp_h = -1;
    gl_cache.depth_near = -1.0f; gl_cache.depth_far = -1.0f;
    gl_cache.sc_x = -1; gl_cache.sc_y = -1;
    gl_cache.sc_w = -1; gl_cache.sc_h = -1;
    gl_cache.scissor_test_enabled = -1;
    gl_cache_reset_textures();
}

// PCGXVertex attrib layout
void vita_set_vertex_attrib_pointers(void) {
    size_t stride = sizeof(PCGXVertex);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(PCGXVertex, position));
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(PCGXVertex, normal));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*)offsetof(PCGXVertex, color0));
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(PCGXVertex, texcoord));
}

// GX compare function -> GL compare function lookup
static const GLenum gx_compare_to_gl[] = {
    GL_NEVER,    // GX_NEVER  = 0
    GL_LESS,     // GX_LESS   = 1
    GL_EQUAL,    // GX_EQUAL  = 2
    GL_LEQUAL,   // GX_LEQUAL = 3
    GL_GREATER,  // GX_GREATER= 4
    GL_NOTEQUAL, // GX_NEQUAL = 5
    GL_GEQUAL,   // GX_GEQUAL = 6
    GL_ALWAYS,   // GX_ALWAYS = 7
};

// GX blend src factor -> GL blend factor lookup
static const GLenum gx_blend_src_to_gl[] = {
    GL_ZERO,                  // GX_BL_ZERO       = 0
    GL_ONE,                   // GX_BL_ONE        = 1
    GL_DST_COLOR,             // GX_BL_DSTCLR     = 2
    GL_ONE_MINUS_DST_COLOR,   // GX_BL_INVDSTCLR  = 3
    GL_SRC_ALPHA,             // GX_BL_SRCALPHA   = 4
    GL_ONE_MINUS_SRC_ALPHA,   // GX_BL_INVSRCALPHA= 5
    GL_DST_ALPHA,             // GX_BL_DSTALPHA   = 6
    GL_ONE_MINUS_DST_ALPHA,   // GX_BL_INVDSTALPHA= 7
};

// GX blend dst factor -> GL blend factor lookup
static const GLenum gx_blend_dst_to_gl[] = {
    GL_ZERO,                  // GX_BL_ZERO       = 0
    GL_ONE,                   // GX_BL_ONE        = 1
    GL_SRC_COLOR,             // GX_BL_SRCCLR     = 2
    GL_ONE_MINUS_SRC_COLOR,   // GX_BL_INVSRCCLR  = 3
    GL_SRC_ALPHA,             // GX_BL_SRCALPHA   = 4
    GL_ONE_MINUS_SRC_ALPHA,   // GX_BL_INVSRCALPHA= 5
    GL_DST_ALPHA,             // GX_BL_DSTALPHA   = 6
    GL_ONE_MINUS_DST_ALPHA,   // GX_BL_INVDSTALPHA= 7
};


VitaFrameTiming vita_timing = {0};
VitaFrameStats vita_stats = {0};

int vita_current_zmode = 0;
int pc_gx_current_queue = 0;
int vita_gpu_skip_draws = 0;


#ifndef VITA_DEBUG_RENDERING
static const int vita_fog_disable = 0;
static const int vita_debug_notex = 0;
#else
int vita_fog_disable = 0;
int vita_debug_notex = 0;
#endif

int vita_disable_merge = 0;


PCGXDrawCmd* cmd_queue_db[2] = {NULL, NULL};
int cmd_queue_count_db[2] = {0, 0};
PCGXVertex* cmd_verts_db[2] = {NULL, NULL};
int cmd_vert_count_db[2] = {0, 0};
GLuint cmd_last_shader_db[2] = {0, 0};
int cmd_write = 0;

PCGXEfbCapture efb_capture_db[2][EFB_CAPTURE_MAX];
int efb_capture_count_db[2] = {0, 0};

#define FRAME_IDX_MAX (PC_GX_MAX_VERTS * 3)
static GLushort* frame_indices_db[2] = {NULL, NULL};
static int       frame_idx_count_db[2] = {0, 0};
#define frame_indices      frame_indices_db[cmd_write]
#define frame_idx_count    frame_idx_count_db[cmd_write]


void vita_cmdbuf_init(void) {
    for (int i = 0; i < 2; i++) {
        cmd_queue_db[i] = (PCGXDrawCmd*)calloc(CMD_QUEUE_MAX, sizeof(PCGXDrawCmd));
        // vglMalloc gives GPU-mapped memory so submit can use vglBufferData.
        // +2048 vertex guard for GXBegin's nverts hint underestimating.
        extern void *vglMalloc(uint32_t size);
        cmd_verts_db[i] = (PCGXVertex*)vglMalloc((PC_GX_MAX_VERTS + 2048) * sizeof(PCGXVertex));
        frame_indices_db[i] = (GLushort*)vglMalloc(FRAME_IDX_MAX * sizeof(GLushort));
        if (!cmd_queue_db[i] || !cmd_verts_db[i] || !frame_indices_db[i]) {
            fprintf(stderr, "[GX] Failed to allocate double-buffer command queue %d\n", i);
            exit(1);
        }
        cmd_queue_count_db[i] = 0;
        cmd_vert_count_db[i] = 0;
        cmd_last_shader_db[i] = 0;
        frame_idx_count_db[i] = 0;
    }
    cmd_write = 0;
}

void vita_cmdbuf_shutdown(void) {
    extern void vglFree(void *ptr);
    for (int i = 0; i < 2; i++) {
        free(cmd_queue_db[i]);         cmd_queue_db[i] = NULL;
        if (cmd_verts_db[i])    { vglFree(cmd_verts_db[i]);    cmd_verts_db[i] = NULL; }
        if (frame_indices_db[i]){ vglFree(frame_indices_db[i]); frame_indices_db[i] = NULL; }
    }
}

// vertex write target for a new batch. in the common case returns a
// pointer directly into cmd_verts so GXPosition/Normal/Color/TexCoord
// write straight into the per-frame vertex buffer (no memcpy on flush).
// overflowing batches get the global scratch buffer instead and will be
// caught by the flush overflow check and dropped.
PCGXVertex* vita_cmdbuf_begin_vertex_batch(int nverts) {
    if (nverts <= 0 || cmd_vert_count + nverts > PC_GX_MAX_VERTS) {
        return g_gx.vertex_buffer;
    }
    return cmd_verts + cmd_vert_count;
}

void vita_cmdbuf_begin_frame(void) {
    vita_timing.flush_us = 0;
    vita_timing.texload_us = 0;
    vita_timing.tevmatch_us = 0;
    vita_timing.flush_vtx_us = 0;
    vita_timing.flush_tev_us = 0;
    vita_timing.flush_state_us = 0;
    vita_timing.flush_state_lighting_us = 0;
    vita_timing.flush_state_textures_us = 0;
    // DO NOT reset endframe_us, waitworker_us, gamemain_us, beginframe_us
    // here: these are set by main thread and read at end of frame; this
    // function runs on Core 1 worker during emu64 and would clobber them
    // BEFORE main thread's perf_log_frame reads them.

    pc_gx_current_queue = GFX_QUEUE_WORK;
    cmd_queue_count = 0;
    cmd_vert_count = 0;
    cmd_last_shader = 0;
    efb_capture_count = 0;
    frame_idx_count = 0;
}

// Setup EFB capture texture after glCopyTexImage2D
void vita_efb_setup_texture(u32 dest_ptr, GLuint tex) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    pc_gx_efb_capture_store(dest_ptr, tex);
    glBindTexture(GL_TEXTURE_2D, 0);
}


int vita_cpu_lit_active = 0;

// cached normalized light directions
// recomputed only when lights change
static float vita_light_norm[8][3];

void vita_normalize_light(int i) {
    pc_normalize_light_dir(g_gx.lights[i].pos, vita_light_norm[i]);
}

// TEV resolve helpers

// 0=fixed, 1=tex.rgb, 2=tex.aaa, 3=ras.rgb, 4=ras.aaa, 5=prev.rgb, 6=prev.aaa
static inline float vita_color_src(int gx_input, int stage) {
    switch (gx_input) {
        case GX_CC_TEXC:  return 1.0f;
        case GX_CC_TEXA:  return 2.0f;
        case GX_CC_RASC:  return 3.0f;
        case GX_CC_RASA:  return 4.0f;
        case GX_CC_CPREV: return (stage > 0) ? 5.0f : 0.0f;
        case GX_CC_APREV: return (stage > 0) ? 6.0f : 0.0f;
        default: return 0.0f;
    }
}

// Convert GX_CA_* alpha input enum to shader source type
// Encoding: 0=fixed, 1=tex.a, 2=ras.a, 3=prev.a
static inline float vita_alpha_src(int gx_input, int stage) {
    switch (gx_input) {
        case GX_CA_TEXA:  return 1.0f;
        case GX_CA_RASA:  return 2.0f;
        case GX_CA_APREV: return (stage > 0) ? 3.0f : 0.0f;
        default: return 0.0f;
    }
}

// Resolve a GX_CC_* color input to float3 value
static void vita_resolve_cc(int input, const PCGXTevStage* ts, float out[3]) {
    switch (input) {
    case GX_CC_CPREV: out[0]=g_gx.tev_colors[0][0]; out[1]=g_gx.tev_colors[0][1]; out[2]=g_gx.tev_colors[0][2]; return;
    case GX_CC_APREV: { float a=g_gx.tev_colors[0][3]; out[0]=out[1]=out[2]=a; return; }
    case GX_CC_C0:    out[0]=g_gx.tev_colors[1][0]; out[1]=g_gx.tev_colors[1][1]; out[2]=g_gx.tev_colors[1][2]; return;
    case GX_CC_A0:    { float a=g_gx.tev_colors[1][3]; out[0]=out[1]=out[2]=a; return; }
    case GX_CC_C1:    out[0]=g_gx.tev_colors[2][0]; out[1]=g_gx.tev_colors[2][1]; out[2]=g_gx.tev_colors[2][2]; return;
    case GX_CC_A1:    { float a=g_gx.tev_colors[2][3]; out[0]=out[1]=out[2]=a; return; }
    case GX_CC_C2:    out[0]=g_gx.tev_colors[3][0]; out[1]=g_gx.tev_colors[3][1]; out[2]=g_gx.tev_colors[3][2]; return;
    case GX_CC_A2:    { float a=g_gx.tev_colors[3][3]; out[0]=out[1]=out[2]=a; return; }
    case GX_CC_TEXC:  out[0]=out[1]=out[2]=0.0f; return;
    case GX_CC_TEXA:  out[0]=out[1]=out[2]=0.0f; return;
    case GX_CC_RASC:  out[0]=out[1]=out[2]=0.0f; return;
    case GX_CC_RASA:  out[0]=out[1]=out[2]=0.0f; return;
    case GX_CC_ONE:   out[0]=out[1]=out[2]=1.0f; return;
    case GX_CC_HALF:  out[0]=out[1]=out[2]=0.5f; return;
    case GX_CC_KONST: {
        int ksel = ts->k_color_sel;
        if (ksel <= 7) {
            static const float kc[] = {1.0f, 0.875f, 0.75f, 0.625f, 0.5f, 0.375f, 0.25f, 0.125f};
            out[0]=out[1]=out[2]=kc[ksel]; return;
        }
        if (ksel >= 0x0C && ksel <= 0x0F) {
            int ki = ksel - 0x0C;
            out[0]=g_gx.tev_k_colors[ki][0]; out[1]=g_gx.tev_k_colors[ki][1]; out[2]=g_gx.tev_k_colors[ki][2]; return;
        }
        if (ksel >= 0x10 && ksel <= 0x1F) {
            int ki = (ksel - 0x10) & 3;
            int ch = (ksel - 0x10) >> 2;
            out[0]=out[1]=out[2]=g_gx.tev_k_colors[ki][ch]; return;
        }
        out[0]=out[1]=out[2]=1.0f; return;
    }
    default: out[0]=out[1]=out[2]=0.0f; return;
    }
}

// Resolve a GX_CA_* alpha input to float value
static float vita_resolve_ca(int input, const PCGXTevStage* ts) {
    switch (input) {
    case GX_CA_APREV: return g_gx.tev_colors[0][3];
    case GX_CA_A0:    return g_gx.tev_colors[1][3];
    case GX_CA_A1:    return g_gx.tev_colors[2][3];
    case GX_CA_A2:    return g_gx.tev_colors[3][3];
    case GX_CA_TEXA:  return 0.0f;
    case GX_CA_RASA:  return 0.0f;
    case GX_CA_KONST: {
        int ksel = ts->k_alpha_sel;
        if (ksel <= 7) {
            static const float kc[] = {1.0f, 0.875f, 0.75f, 0.625f, 0.5f, 0.375f, 0.25f, 0.125f};
            return kc[ksel];
        }
        if (ksel >= 0x10 && ksel <= 0x1F) {
            int ki = (ksel - 0x10) & 3;
            int ch = (ksel - 0x10) >> 2;
            return g_gx.tev_k_colors[ki][ch];
        }
        return 1.0f;
    }
    default: return 0.0f;
    }
}

// Resolve helpers that read from EXPLICIT color arrays instead of g_gx.
// Used by the core 2 resolve pass so it can work on snapshotted cmd->tev
// inputs without touching g_gx (which core 1 may be mutating).
static inline void vita_resolve_cc_cmd(int input, const PCGXTevStage* ts,
                                       const float tev_colors[4][4],
                                       const float tev_k_colors[4][4],
                                       float out[3]) {
    switch (input) {
    case GX_CC_CPREV: out[0]=tev_colors[0][0]; out[1]=tev_colors[0][1]; out[2]=tev_colors[0][2]; return;
    case GX_CC_APREV: { float a=tev_colors[0][3]; out[0]=out[1]=out[2]=a; return; }
    case GX_CC_C0:    out[0]=tev_colors[1][0]; out[1]=tev_colors[1][1]; out[2]=tev_colors[1][2]; return;
    case GX_CC_A0:    { float a=tev_colors[1][3]; out[0]=out[1]=out[2]=a; return; }
    case GX_CC_C1:    out[0]=tev_colors[2][0]; out[1]=tev_colors[2][1]; out[2]=tev_colors[2][2]; return;
    case GX_CC_A1:    { float a=tev_colors[2][3]; out[0]=out[1]=out[2]=a; return; }
    case GX_CC_C2:    out[0]=tev_colors[3][0]; out[1]=tev_colors[3][1]; out[2]=tev_colors[3][2]; return;
    case GX_CC_A2:    { float a=tev_colors[3][3]; out[0]=out[1]=out[2]=a; return; }
    case GX_CC_TEXC:  out[0]=out[1]=out[2]=0.0f; return;
    case GX_CC_TEXA:  out[0]=out[1]=out[2]=0.0f; return;
    case GX_CC_RASC:  out[0]=out[1]=out[2]=0.0f; return;
    case GX_CC_RASA:  out[0]=out[1]=out[2]=0.0f; return;
    case GX_CC_ONE:   out[0]=out[1]=out[2]=1.0f; return;
    case GX_CC_HALF:  out[0]=out[1]=out[2]=0.5f; return;
    case GX_CC_KONST: {
        int ksel = ts->k_color_sel;
        if (ksel <= 7) {
            static const float kc[] = {1.0f, 0.875f, 0.75f, 0.625f, 0.5f, 0.375f, 0.25f, 0.125f};
            out[0]=out[1]=out[2]=kc[ksel]; return;
        }
        if (ksel >= 0x0C && ksel <= 0x0F) {
            int ki = ksel - 0x0C;
            out[0]=tev_k_colors[ki][0]; out[1]=tev_k_colors[ki][1]; out[2]=tev_k_colors[ki][2]; return;
        }
        if (ksel >= 0x10 && ksel <= 0x1F) {
            int ki = (ksel - 0x10) & 3;
            int ch = (ksel - 0x10) >> 2;
            out[0]=out[1]=out[2]=tev_k_colors[ki][ch]; return;
        }
        out[0]=out[1]=out[2]=1.0f; return;
    }
    default: out[0]=out[1]=out[2]=0.0f; return;
    }
}

static inline float vita_resolve_ca_cmd(int input, const PCGXTevStage* ts,
                                        const float tev_colors[4][4],
                                        const float tev_k_colors[4][4]) {
    switch (input) {
    case GX_CA_APREV: return tev_colors[0][3];
    case GX_CA_A0:    return tev_colors[1][3];
    case GX_CA_A1:    return tev_colors[2][3];
    case GX_CA_A2:    return tev_colors[3][3];
    case GX_CA_TEXA:  return 0.0f;
    case GX_CA_RASA:  return 0.0f;
    case GX_CA_KONST: {
        int ksel = ts->k_alpha_sel;
        if (ksel <= 7) {
            static const float kc[] = {1.0f, 0.875f, 0.75f, 0.625f, 0.5f, 0.375f, 0.25f, 0.125f};
            return kc[ksel];
        }
        if (ksel >= 0x10 && ksel <= 0x1F) {
            int ki = (ksel - 0x10) & 3;
            int ch = (ksel - 0x10) >> 2;
            return tev_k_colors[ki][ch];
        }
        return 1.0f;
    }
    default: return 0.0f;
    }
}

// Map tex matrix ID to slot (from pc_gx.c)
static int pc_tex_mtx_id_to_slot(int id) {
    if (id == GX_IDENTITY) return -1;
    if (id >= 0 && id < 10) return id;
    if (id >= GX_TEXMTX0 && id < GX_IDENTITY) return (id - GX_TEXMTX0) / 3;
    return -1;
}


void pc_gx_submit_frame(void) {
#ifdef VITA_DEBUG
    uint32_t t_submit_entry = sceKernelGetProcessTimeLow();
    // reset vitaGL per-phase counters for this frame's draws. read-back
    // happens at the end of this function. no-op when vitaGL built without
    // DRAW_PHASE_PROFILING.
    extern void vgl_reset_draw_phases(void);
    vgl_reset_draw_phases();
    // our-side submit phase accumulators (us). setup covers all per-cmd
    // prep up to and including viewport/scissor. draw covers the
    // glDrawElements call stack. efb covers post-draw capture work.
    uint32_t submit_setup_acc = 0;
    uint32_t submit_draw_acc = 0;
    uint32_t submit_efb_acc = 0;
    // Zero all sub-phase fields up front so early-return paths don't
    // leave stale values from the previous frame.
    vita_timing.submit_uniform_us = 0;
    vita_timing.submit_glstate_us = 0;
    vita_timing.submit_state_us   = 0;
    vita_timing.submit_draw_us    = 0;
    vita_timing.submit_efb_us     = 0;
    vita_timing.submit_presub_us  = 0;
    vita_timing.submit_postsub_us = 0;
    vita_timing.submit_waitpdd_us = 0;
#endif

    // Texture cache work: doesn't touch cmd buffer, so runs in parallel
    // with core 2 prededup. moves ~0.3-0.5ms off the critical path before
    // we block on wait_prededup.
    pc_gx_texture_process_deferred_uploads();
    pc_gx_texture_process_deferred_params();

    if (vita_stats.deferred_tex_uploads > 0)
        gl_cache_reset_textures();

    // Now block on core 2 prededup (the cmd buffer reads below need it
    // complete). timing this here captures the REAL wait, not the
    // fraction overlapping with the texture work above.
#ifdef VITA_DEBUG
    uint32_t t_waitpdd_start = sceKernelGetProcessTimeLow();
#endif
    extern void vita_wait_prededup(void);
    vita_wait_prededup();
#ifdef VITA_DEBUG
    uint32_t t_after_waitpdd = sceKernelGetProcessTimeLow();
    vita_timing.submit_waitpdd_us = t_after_waitpdd - t_waitpdd_start;
#endif

    // late texture re-resolve after deferred uploads completed.
    extern int pc_gx_deferred_tex_uploads;
    if (pc_gx_deferred_tex_uploads > 0) {
        int rd_tmp = 1 - cmd_write;
        int cnt = cmd_queue_count_db[rd_tmp];
        int rr_patched = 0, rr_failed = 0, rr_noidx = 0;
        for (int i = 0; i < cnt; i++) {
            PCGXDrawCmd* c = &cmd_queue_db[rd_tmp][i];
            if (c->shader == 0) continue;
            for (int s = 0; s < PC_GX_MAX_TEV_STAGES; s++) {
                int idx = c->textures.deferred_idx[s];
                if (c->textures.obj_stage[s] == 0 && idx >= 0) {
                    if (idx < vita_deferred_uploaded_count) {
                        GLuint tex = vita_deferred_uploaded[idx];
                        if (tex) {
                            c->textures.obj_stage[s] = tex;
                            c->textures.use_stage[s] = 1;
                            // only set DIRTY_TEXTURES here; setting
                            // DIRTY_TEV_STAGES as well caused a black
                            // flash on first-time texture loads.
                            c->dirty |= PC_GX_DIRTY_TEXTURES;
                            rr_patched++;
                        } else {
                            rr_failed++;
                        }
                    } else if (idx >= vita_deferred_uploaded_count) {
                        rr_failed++;
                    }
                } else if (c->textures.obj_stage[s] == 0 && c->textures.map_stage[s] >= 0 &&
                           idx == -1 && c->textures.use_stage[s]) {
                    rr_noidx++;
                }
            }
        }
        if (rr_failed > 0 || rr_noidx > 0) {
            vita_log("[TEXDBG] uploads=%d patched=%d failed=%d noIdx=%d\n",
                     vita_stats.deferred_tex_uploads, rr_patched, rr_failed, rr_noidx);
        }
    }

    int rd = 1 - cmd_write;
    int rd_count = cmd_queue_count_db[rd];
    int rd_verts = cmd_vert_count_db[rd];

    int rd_efb_count = efb_capture_count_db[rd];
    int rd_efb_next = 0;

    // empty frame: no draws to replay but still process queued EFB
    // captures, otherwise pause-menu re-open drops the capture and the
    // menu renders against a stale framebuffer.
    if (rd_verts == 0 || rd_count == 0) {
        for (int e = 0; e < rd_efb_count; e++) {
            PCGXEfbCapture* cap = &efb_capture_db[rd][e];
            int gl_y = g_pc_window_h - (cap->src_top + cap->src_h);
            if (gl_y < 0 || cap->src_w <= 0 || cap->src_h <= 0) continue;
            GLuint efb_tex = pc_gx_efb_capture_get_or_create(cap->dest_ptr);
            if (efb_tex) {
                gl_cache_active_texture(GL_TEXTURE7);
                glBindTexture(GL_TEXTURE_2D, efb_tex);
                glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                                 cap->src_left, gl_y, cap->src_w, cap->src_h, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                gl_cache_active_texture(GL_TEXTURE0);
            }
        }
        cmd_queue_count_db[rd] = 0;
        cmd_vert_count_db[rd] = 0;
        cmd_last_shader_db[rd] = 0;
        efb_capture_count_db[rd] = 0;
        vita_gpu_skip_draws = 1;
#ifdef VITA_DEBUG
        // empty-frame path: total elapsed minus the waitpdd block.
        vita_timing.submit_presub_us =
            (sceKernelGetProcessTimeLow() - t_submit_entry) - vita_timing.submit_waitpdd_us;
#endif
        return;
    }

    // zero-copy vertex upload: cmd_verts_db lives in GPU-mapped memory
    // (vglMalloc at init), so vglBufferData just points GL at our buffer.
    // 2 buffers is enough because cmd_write flips once per frame and
    // VitaGL's display queue is never more than one frame behind here.
    extern void vglBufferData(GLenum target, const GLvoid *data);
    vglBufferData(GL_ARRAY_BUFFER, cmd_verts_db[rd]);

    vita_set_vertex_attrib_pointers();

    // index buffer is built inline by the worker as it flushes each draw
    {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_gx.ebo);
        vglBufferData(GL_ELEMENT_ARRAY_BUFFER, frame_indices_db[rd]);
    }

    // main-thread pre-dedup safety net. only needed when deferred texture
    // uploads modified cmd state after the worker's pass. on stable frames
    // (no deferred uploads) the worker pre-dedup already cleared everything.
    if (pc_gx_deferred_tex_uploads > 0) {
        PCGXCmdTextures  last_textures_pm;
        float            last_proj_mtx_pm[16];
        float            last_pos_mtx_pm[16];
        int last_pm_proj_valid = 0;
        int last_pm_modelview_valid = 0;
        int last_pm_textures_valid = 0;
        GLuint last_pm_shader = 0;

        for (int i = 0; i < rd_count; i++) {
            PCGXDrawCmd* c = &cmd_queue_db[rd][i];
            if (c->shader == 0) continue;

            if (c->shader_changed || c->shader != last_pm_shader) {
                last_pm_proj_valid = 0;
                last_pm_modelview_valid = 0;
                last_pm_textures_valid = 0;
                last_pm_shader = c->shader;
            }

            unsigned int orig_dirty = c->dirty;
            unsigned int new_dirty = orig_dirty;

            if ((orig_dirty & PC_GX_DIRTY_PROJECTION) && last_pm_proj_valid &&
                __builtin_memcmp(c->transform.projection_mtx_t, last_proj_mtx_pm, 64) == 0)
                new_dirty &= ~PC_GX_DIRTY_PROJECTION;
            if ((orig_dirty & PC_GX_DIRTY_MODELVIEW) && last_pm_modelview_valid &&
                __builtin_memcmp(c->transform.pos_mtx_t, last_pos_mtx_pm, 64) == 0) {
                new_dirty &= ~PC_GX_DIRTY_MODELVIEW;
            }
            if ((orig_dirty & PC_GX_DIRTY_TEXTURES) && last_pm_textures_valid &&
                __builtin_memcmp(&c->textures, &last_textures_pm, sizeof(PCGXCmdTextures)) == 0)
                new_dirty &= ~PC_GX_DIRTY_TEXTURES;
            c->dirty = new_dirty;

            if (orig_dirty & PC_GX_DIRTY_PROJECTION) {
                __builtin_memcpy(last_proj_mtx_pm, c->transform.projection_mtx_t, 64);
                last_pm_proj_valid = 1;
            }
            if (orig_dirty & PC_GX_DIRTY_MODELVIEW) {
                __builtin_memcpy(last_pos_mtx_pm, c->transform.pos_mtx_t, 64);
                last_pm_modelview_valid = 1;
            }
            if (orig_dirty & PC_GX_DIRTY_TEXTURES) {
                last_textures_pm = c->textures;
                last_pm_textures_valid = 1;
            }
        }
    }

    vita_stats.opaque_draws = 0;
    vita_stats.blended_draws = 0;
    vita_stats.merged_draws = rd_count;
    vita_stats.sort_merged = 0;

    // vitaGL uniform bypass
    typedef struct { const void* ptr; float* data; unsigned int size; unsigned char is_frag; unsigned char is_vert; } vgl_uni_t;
    #define VU(loc)  ((vgl_uni_t*)((intptr_t)(-(loc))))
    #define UL(field) g_gx.uloc.field
    #define UNI1I(v)         do { VU(loc)->data[0] = (float)(v); } while(0)
    #define UNI2I(a,b)       do { float*_d=VU(loc)->data; _d[0]=(float)(a); _d[1]=(float)(b); } while(0)
    #define UNI3I(a,b,c)     do { float*_d=VU(loc)->data; _d[0]=(float)(a); _d[1]=(float)(b); _d[2]=(float)(c); } while(0)
    #define UNI4I(a,b,c,d)   do { float*_dd=VU(loc)->data; _dd[0]=(float)(a); _dd[1]=(float)(b); _dd[2]=(float)(c); _dd[3]=(float)(d); } while(0)
    extern unsigned char dirty_vert_unifs;
    extern unsigned char dirty_frag_unifs;

    int actual_draws = 0;
    vita_stats.merged_draws = rd_count;

    int last_num_ind_stages = -1;
    static float last_lpos[8][3];
    static float last_lcol[8][4];
    int last_lights_valid = 0;
    static unsigned char last_light_scalars[76];
    int last_light_scalars_valid = 0;
    const size_t LIGHT_SCALAR_BYTES = offsetof(PCGXCmdLighting, light_pos);

    extern int g_vita_force_state_resync;
    if (g_vita_force_state_resync) {
        gl_cache_reset();
        gl_cache_reset_textures();
        g_vita_force_state_resync = 0;
    }

    float last_vp_input[6] = { -9999, -9999, -9999, -9999, -9999, -9999 };
    int   last_sc_input[4] = { -9999, -9999, -9999, -9999 };
    int   last_vp_ws = -1;
    int   last_vp_input_valid = 0;

    // vgl_fast_draw_mode: we set textures directly via sceGxm, so
    // VitaGL's per-draw texture loops can be skipped.
    extern int vgl_fast_draw_mode;
    extern SceGxmContext* vglGetGxmContext(void);
    extern const SceGxmTexture* vglGetGxmTextureById(GLuint id);
    SceGxmContext* gxm_ctx = vglGetGxmContext();
    vgl_fast_draw_mode = 1;
    GLuint gxm_frag_tex[8] = {0};

    static int dbg_ctr = 0;
    if ((dbg_ctr++ & 0xFF) == 0) {
        int dbg_dirty0 = 0, dbg_only_lighting = 0, dbg_has_mv = 0, dbg_has_proj = 0, dbg_has_other = 0;
        for (int i = 0; i < rd_count; i++) {
            PCGXDrawCmd* cmd = &cmd_queue_db[rd][i];
            if (cmd->shader == 0) continue;
            if (cmd->dirty == 0) dbg_dirty0++;
            else if ((cmd->dirty & ~(unsigned int)PC_GX_DIRTY_LIGHTING) == 0) dbg_only_lighting++;
            if (cmd->dirty & PC_GX_DIRTY_MODELVIEW) dbg_has_mv++;
            if (cmd->dirty & PC_GX_DIRTY_PROJECTION) dbg_has_proj++;
            if (cmd->dirty & ~(unsigned int)(PC_GX_DIRTY_LIGHTING|PC_GX_DIRTY_MODELVIEW|PC_GX_DIRTY_PROJECTION)) dbg_has_other++;
        }
        vita_log("[DIRTYBITS] cmds=%d dirty0=%d only_light=%d has_mv=%d has_proj=%d has_other=%d\n",
                 rd_count, dbg_dirty0, dbg_only_lighting, dbg_has_mv, dbg_has_proj, dbg_has_other);
    }

#ifdef VITA_DEBUG
    uint32_t t_loop_start = sceKernelGetProcessTimeLow();
    // presub = all pre-loop work except the waitpdd block. this now
    // spans two segments (texture-cache work before waitpdd, then the
    // rest after waitpdd) because we moved waitpdd lower to overlap with
    // texture processing.
    vita_timing.submit_presub_us = (t_loop_start - t_submit_entry) - vita_timing.submit_waitpdd_us;
#endif

    for (int i = 0; i < rd_count; i++) {
        PCGXDrawCmd* cmd = &cmd_queue_db[rd][i];
        if (cmd->shader == 0) continue;
        // Prefetch next cmd's hot fields while we process this one. Each
        // PCGXDrawCmd is ~1.5KB spanning ~48 cache lines; the uniform+
        // state block below touches many of those lines. Prefetching the
        // next cmd 2-3 cache lines ahead warms the cache before we need
        // it, hiding some of the L2 latency.
        if (i + 1 < rd_count) {
            PCGXDrawCmd* next_cmd = &cmd_queue_db[rd][i + 1];
            __builtin_prefetch(next_cmd, 0, 0);
            __builtin_prefetch((char*)next_cmd + 64, 0, 0);
            __builtin_prefetch((char*)next_cmd + 128, 0, 0);
        }
#ifdef VITA_DEBUG
        uint32_t ts_setup_start = sceKernelGetProcessTimeLow();
#endif

        // late EFB re-resolve. get_or_create (not find) so an evicted or
        // never-created entry still returns a valid tex id; the capture
        // replay below reuses the same id and fills it with content.
        if (cmd->textures.efb_src_ptr[0] | cmd->textures.efb_src_ptr[1] | cmd->textures.efb_src_ptr[2]) {
            for (int s = 0; s < PC_GX_MAX_TEV_STAGES; s++) {
                if (cmd->textures.efb_src_ptr[s] != 0) {
                    GLuint fresh = pc_gx_efb_capture_get_or_create(cmd->textures.efb_src_ptr[s]);
                    if (fresh && fresh != cmd->textures.obj_stage[s]) {
                        cmd->textures.obj_stage[s] = fresh;
                        cmd->textures.use_stage[s] = 1;
                        cmd->dirty |= PC_GX_DIRTY_TEXTURES;
                    }
                }
            }
        }

        {
            int tex_missing = 0;
            for (int s = 0; s < cmd->tev.num_stages; s++) {
                if (cmd->textures.obj_stage[s] == 0 && cmd->textures.use_stage[s] == 1) {
                    tex_missing = 1; break;
                }
                if (cmd->textures.map_stage[s] >= 0 && cmd->textures.obj_stage[s] == 0 &&
                    cmd->textures.deferred_idx[s] >= 0) {
                    tex_missing = 1; break;
                }
            }
            if (tex_missing) {
                static int skip_tex_count = 0;
                skip_tex_count++;
                if (skip_tex_count <= 20 || (skip_tex_count & 0xFF) == 0)
                    vita_log("[TEXSKIP] draw %d skipped: missing tex (shader=%u) [total=%d]\n", i, cmd->shader, skip_tex_count);
                continue;
            }
        }

        if (cmd->shader_changed) {
            vita_stats.shader_switches++;
            glUseProgram(cmd->shader);
            g_gx.current_shader = cmd->shader;
            pc_gx_cache_uniform_locations(cmd->shader);
            GLint sloc;
            sloc = UL(texture0); if (sloc >= 0) glUniform1i(sloc, 0);
            sloc = UL(texture1); if (sloc >= 0) glUniform1i(sloc, 1);
            sloc = UL(texture2); if (sloc >= 0) glUniform1i(sloc, 2);
            // scissor stays enabled in GXM across glUseProgram. keeping
            // our cache flag avoids ~41 redundant glEnable(GL_SCISSOR_TEST)
            // per frame (each triggers VitaGL's update_scissor_test).
            int preserved_scissor = gl_cache.scissor_test_enabled;
            gl_cache_reset();
            if (preserved_scissor == 1) gl_cache.scissor_test_enabled = 1;
            last_lights_valid = 0;
            last_light_scalars_valid = 0;
            last_vp_input_valid = 0;
            last_num_ind_stages = -1;
        }

#ifdef VITA_DEBUG
        uint32_t ts_unif_start = sceKernelGetProcessTimeLow();
#endif
        // direct writes to VitaGL's uniform shadow.
        if (cmd->dirty != 0)
        {
            GLint loc;
            unsigned int dirty = cmd->dirty;
            #define DFVN(loc, src, bytes) __builtin_memcpy(VU(loc)->data, (src), (bytes))

            if (dirty & PC_GX_DIRTY_PROJECTION) {
                loc = UL(projection);
                if (loc >= 0) DFVN(loc, cmd->transform.projection_mtx_t, 64);
            }

            if (dirty & PC_GX_DIRTY_MODELVIEW) {
                loc = UL(modelview);
                if (loc >= 0) DFVN(loc, cmd->transform.pos_mtx_t, 64);
                loc = UL(normal_mtx);
                if (loc >= 0) DFVN(loc, cmd->transform.nrm_mtx_t, 36);
            }

            if (dirty & PC_GX_DIRTY_TEV_COLORS) {
                loc = UL(tev_prev); if (loc >= 0) DFVN(loc, cmd->tev.colors[0], 16);
                loc = UL(tev_reg0); if (loc >= 0) DFVN(loc, cmd->tev.colors[1], 16);
                loc = UL(tev_reg1); if (loc >= 0) DFVN(loc, cmd->tev.colors[2], 16);
                loc = UL(tev_reg2); if (loc >= 0) DFVN(loc, cmd->tev.colors[3], 16);
            }

            if (dirty & PC_GX_DIRTY_TEV_STAGES) {
                loc = UL(num_tev_stages); if (loc >= 0) UNI1I(cmd->tev.num_stages);
                for (int s = 0; s < PC_GX_MAX_TEV_STAGES && s < cmd->tev.num_stages; s++) {
                    PCGXTevStage* ts = &cmd->tev.stages[s];
                    loc = UL(tev_color_in[s]); if (loc >= 0) UNI4I(ts->color_a, ts->color_b, ts->color_c, ts->color_d);
                    loc = UL(tev_alpha_in[s]); if (loc >= 0) UNI4I(ts->alpha_a, ts->alpha_b, ts->alpha_c, ts->alpha_d);
                    loc = UL(tev_color_op[s]); if (loc >= 0) UNI1I(ts->color_op);
                    loc = UL(tev_alpha_op[s]); if (loc >= 0) UNI1I(ts->alpha_op);
                    loc = UL(tev_bsc[s]);  if (loc >= 0) UNI4I(ts->color_bias, ts->color_scale, ts->alpha_bias, ts->alpha_scale);
                    loc = UL(tev_out[s]);  if (loc >= 0) UNI4I(ts->color_clamp, ts->alpha_clamp, ts->color_out, ts->alpha_out);
                    loc = UL(tev_swap[s]); if (loc >= 0) UNI2I(ts->ras_swap, ts->tex_swap);
                }
            }
            // upload tev_tc_src on TEV_STAGES OR TEXTURES so the deferred-
            // upload patch path (DIRTY_TEXTURES alone) refreshes it.
            if (dirty & (PC_GX_DIRTY_TEV_STAGES | PC_GX_DIRTY_TEXTURES)) {
                for (int s = 0; s < PC_GX_MAX_TEV_STAGES; s++) {
                    loc = UL(tev_tc_src[s]); if (loc >= 0) UNI1I(cmd->tev.tc_src[s]);
                }
            }
            // Pre-resolved TEV input values (ca/cb/cc/cd/aval/...) depend
            // on register colors, KONST, AND stage config. The resolve at
            // the top of this function (line ~93) re-runs whenever any
            // of those dirty bits fire — but the upload below was gated
            // on TEV_STAGES alone, so a primitive_color change between
            // two cfg22 draws (same stages, different C1) would silently
            // re-resolve cb[1] on the cmd side without ever pushing it
            // to the GPU uniform. The shader kept the previous draw's
            // primitive color, which is exactly the post office "POST
            // OFFICE" / Nookway-class invisibility bug. Match this gate
            // to the resolve gate so any time the resolved values
            // change, the GPU sees the new values.
            if (dirty & (PC_GX_DIRTY_TEV_STAGES | PC_GX_DIRTY_TEV_COLORS | PC_GX_DIRTY_KONST)) {
                for (int s = 0; s < PC_GX_MAX_TEV_STAGES && s < cmd->tev.num_stages; s++) {
                    loc = UL(tev_ca[s]); if (loc >= 0) DFVN(loc, cmd->tev.ca[s], 12);
                    loc = UL(tev_cb[s]); if (loc >= 0) DFVN(loc, cmd->tev.cb[s], 12);
                    loc = UL(tev_cc[s]); if (loc >= 0) DFVN(loc, cmd->tev.cc[s], 12);
                    loc = UL(tev_cd[s]); if (loc >= 0) DFVN(loc, cmd->tev.cd[s], 12);
                    loc = UL(tev_aval[s]); if (loc >= 0) DFVN(loc, cmd->tev.aval[s], 16);
                    loc = UL(tev_csrc[s]); if (loc >= 0) DFVN(loc, cmd->tev.csrc[s], 16);
                    loc = UL(tev_asrc[s]); if (loc >= 0) DFVN(loc, cmd->tev.asrc[s], 16);
                    loc = UL(tev_param[s]); if (loc >= 0) DFVN(loc, cmd->tev.param[s], 16);
                    loc = UL(tev_aparam[s]); if (loc >= 0) DFVN(loc, cmd->tev.aparam[s], 8);
                }
            }

            if (dirty & PC_GX_DIRTY_KONST) {
                loc = UL(kcolor); if (loc >= 0) DFVN(loc, cmd->tev.k_colors, 64);
            }

            if (dirty & PC_GX_DIRTY_ALPHA_CMP) {
                loc = UL(alpha_ref0);  if (loc >= 0) { VU(loc)->data[0] = cmd->alpha.eff_ref; }
                loc = UL(alpha_comp0); if (loc >= 0) { VU(loc)->data[0] = cmd->alpha.comp0_f; }
            }

            if (dirty & PC_GX_DIRTY_LIGHTING) {
                int scalars_changed = !last_light_scalars_valid ||
                    __builtin_memcmp(&cmd->lighting, last_light_scalars,
                                     LIGHT_SCALAR_BYTES) != 0;
                if (scalars_changed) {
                    loc = UL(lighting_enabled); if (loc >= 0) UNI1I(cmd->lighting.chan_ctrl_enable_0);
                    loc = UL(mat_color);  if (loc >= 0) DFVN(loc, cmd->lighting.mat_color_0, 16);
                    loc = UL(chan_mat_src); if (loc >= 0) UNI1I(cmd->lighting.mat_src_0);
                    loc = UL(num_chans);  if (loc >= 0) UNI1I(cmd->lighting.num_chans);
                    loc = UL(alpha_lighting_enabled); if (loc >= 0) UNI1I(cmd->lighting.chan_ctrl_enable_1);
                    loc = UL(alpha_mat_src); if (loc >= 0) UNI1I(cmd->lighting.mat_src_1);
                    loc = UL(amb_color);  if (loc >= 0) DFVN(loc, cmd->lighting.amb_color_0, 16);
                    loc = UL(chan_amb_src); if (loc >= 0) UNI1I(cmd->lighting.amb_src_0);
                    loc = UL(light_mask); if (loc >= 0) UNI1I(cmd->lighting.light_mask_0);
                    __builtin_memcpy(last_light_scalars, &cmd->lighting, LIGHT_SCALAR_BYTES);
                    last_light_scalars_valid = 1;
                }
                {
                    {
                        int lights_changed = !last_lights_valid ||
                            __builtin_memcmp(cmd->lighting.light_pos, last_lpos, 96) != 0 ||
                            __builtin_memcmp(cmd->lighting.light_color, last_lcol, 128) != 0;
                        if (lights_changed) {
                            loc = UL(light_pos[0]);   if (loc >= 0) DFVN(loc, &cmd->lighting.light_pos[0][0], 96);
                            loc = UL(light_color[0]); if (loc >= 0) DFVN(loc, &cmd->lighting.light_color[0][0], 128);
                            __builtin_memcpy(last_lpos, cmd->lighting.light_pos, 96);
                            __builtin_memcpy(last_lcol, cmd->lighting.light_color, 128);
                            last_lights_valid = 1;
                        }
                    if (cmd->lighting.vs_lit_enable) {
                        if (scalars_changed) {
                            loc = UL(vs_mat_color);  if (loc >= 0) DFVN(loc, cmd->lighting.mat_color_0, 16);
                            loc = UL(vs_amb_color);  if (loc >= 0) DFVN(loc, cmd->lighting.amb_color_0, 16);
                            loc = UL(vs_chan_mat_src); if (loc >= 0) { VU(loc)->data[0] = (float)cmd->lighting.vs_mat_src; }
                            loc = UL(vs_chan_amb_src); if (loc >= 0) { VU(loc)->data[0] = (float)cmd->lighting.amb_src_0; }
                            loc = UL(vs_alpha_mat_src); if (loc >= 0) { VU(loc)->data[0] = (float)cmd->lighting.vs_alpha_mat_src; }
                            loc = UL(vs_alpha_lit); if (loc >= 0) { VU(loc)->data[0] = (float)cmd->lighting.chan_ctrl_enable_1; }
                        }
                        if (lights_changed) {
                            for (int li = 0; li < 8; li++) {
                                loc = UL(vs_ldir[li]);
                                if (loc >= 0) { VU(loc)->data[0] = cmd->lighting.light_pos[li][0]; VU(loc)->data[1] = cmd->lighting.light_pos[li][1]; VU(loc)->data[2] = cmd->lighting.light_pos[li][2]; }
                                loc = UL(vs_lcol[li]);
                                if (loc >= 0) DFVN(loc, cmd->lighting.light_color[li], 16);
                            }
                        }
                    } else {
                        static const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                        static const float zero4[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                        if (scalars_changed) {
                            loc = UL(vs_mat_color);  if (loc >= 0) DFVN(loc, white, 16);
                            loc = UL(vs_amb_color);  if (loc >= 0) DFVN(loc, white, 16);
                            loc = UL(vs_chan_mat_src); if (loc >= 0) { VU(loc)->data[0] = 1.0f; }
                            loc = UL(vs_chan_amb_src); if (loc >= 0) { VU(loc)->data[0] = 1.0f; }
                            loc = UL(vs_alpha_mat_src); if (loc >= 0) { VU(loc)->data[0] = 1.0f; }
                            loc = UL(vs_alpha_lit); if (loc >= 0) { VU(loc)->data[0] = 0.0f; }
                        }
                        if (lights_changed) {
                            for (int li = 0; li < 8; li++) {
                                loc = UL(vs_ldir[li]); if (loc >= 0) { VU(loc)->data[0] = 0; VU(loc)->data[1] = 0; VU(loc)->data[2] = 1; }
                                loc = UL(vs_lcol[li]); if (loc >= 0) DFVN(loc, zero4, 16);
                            }
                        }
                    }
                    }
                }
            }

            if (dirty & PC_GX_DIRTY_TEXGEN) {
                for (int tg = 0; tg < 2; tg++) {
                    loc = UL(texmtx_enable[tg]); if (loc >= 0) UNI1I(cmd->texgen.mtx_enable[tg]);
                    if (cmd->texgen.mtx_enable[tg]) {
                        loc = UL(texmtx_row0[tg]); if (loc >= 0) DFVN(loc, cmd->texgen.mtx_row0[tg], 16);
                        loc = UL(texmtx_row1[tg]); if (loc >= 0) DFVN(loc, cmd->texgen.mtx_row1[tg], 16);
                    }
                    loc = UL(texgen_src[tg]); if (loc >= 0) UNI1I(cmd->texgen.gen_src[tg]);
                }
            }

            if (dirty & (PC_GX_DIRTY_TEXTURES | PC_GX_DIRTY_TEV_STAGES)) {
                // direct GXM texture path: bypasses glBindTexture,
                // glActiveTexture, glTexParameteri entirely. sets wrap
                // modes directly on the SceGxmTexture struct.
                // use glBindTexture + glTexParameteri for wrap (proven
                // working) + direct sceGxm push for the fast-path bypass.
                int ns = cmd->tev.num_stages;
                if (ns > PC_GX_MAX_TEV_STAGES) ns = PC_GX_MAX_TEV_STAGES;
                for (int s = 0; s < ns; s++) {
                    if (!cmd->textures.use_stage[s]) continue;
                    GLuint tid = cmd->textures.obj_stage[s];
                    // skip texture binding entirely when unchanged for this
                    // GXM unit. consecutive draws of same material/texture
                    // (very common for batched geometry) hit this fast path.
                    if (gxm_frag_tex[s] == tid) {
                        // still need wrap mode refresh if game changed it
                        if (cmd->textures.wrap_s[s] != 0xFF &&
                            (gl_cache_wrap_s[s] != cmd->textures.wrap_s[s] ||
                             gl_cache_wrap_t[s] != cmd->textures.wrap_t[s])) {
                            gl_cache_bind_texture(GL_TEXTURE0 + s, tid);
                            gl_cache_active_texture(GL_TEXTURE0 + s);
                            GLenum ws = (cmd->textures.wrap_s[s] == 2) ? GL_MIRRORED_REPEAT :
                                        (cmd->textures.wrap_s[s] == 0) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
                            GLenum wt = (cmd->textures.wrap_t[s] == 2) ? GL_MIRRORED_REPEAT :
                                        (cmd->textures.wrap_t[s] == 0) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ws);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wt);
                            gl_cache_wrap_s[s] = cmd->textures.wrap_s[s];
                            gl_cache_wrap_t[s] = cmd->textures.wrap_t[s];
                        }
                        continue;
                    }
                    gl_cache_bind_texture(GL_TEXTURE0 + s, tid);
                    if (cmd->textures.wrap_s[s] != 0xFF &&
                        (gl_cache_wrap_s[s] != cmd->textures.wrap_s[s] ||
                         gl_cache_wrap_t[s] != cmd->textures.wrap_t[s])) {
                        gl_cache_active_texture(GL_TEXTURE0 + s);
                        GLenum ws = (cmd->textures.wrap_s[s] == 2) ? GL_MIRRORED_REPEAT :
                                    (cmd->textures.wrap_s[s] == 0) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
                        GLenum wt = (cmd->textures.wrap_t[s] == 2) ? GL_MIRRORED_REPEAT :
                                    (cmd->textures.wrap_t[s] == 0) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ws);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wt);
                        gl_cache_wrap_s[s] = cmd->textures.wrap_s[s];
                        gl_cache_wrap_t[s] = cmd->textures.wrap_t[s];
                    }
                    // direct GXM push (bypasses VitaGL texture loop)
                    const SceGxmTexture* gt = vglGetGxmTextureById(tid);
                    if (gt) sceGxmSetFragmentTexture(gxm_ctx, s, gt);
                    gxm_frag_tex[s] = tid;
                }
                loc = UL(use_texture0); if (loc >= 0) UNI1I(cmd->textures.use_stage[0]);
                loc = UL(use_texture1); if (loc >= 0) UNI1I(cmd->textures.use_stage[1]);
                loc = UL(use_texture2); if (loc >= 0) UNI1I(cmd->textures.use_stage[2]);
            }

            if (dirty & (PC_GX_DIRTY_INDIRECT | PC_GX_DIRTY_TEXTURES)) {
                // cache num_ind_stages to avoid redundant upload every draw
                // (triggered by DIRTY_TEXTURES even when count doesn't change)
                if (cmd->indirect.num_stages != last_num_ind_stages) {
                    loc = UL(num_ind_stages); if (loc >= 0) UNI1I(cmd->indirect.num_stages);
                    last_num_ind_stages = cmd->indirect.num_stages;
                }
                if (cmd->indirect.num_stages > 0) {
                    for (int ii = 0; ii < cmd->indirect.num_stages && ii < 4; ii++) {
                        if (cmd->indirect.tex[ii]) {
                            // direct GXM for indirect textures (no glBindTexture)
                            int gu = 3 + ii;
                            if (gxm_frag_tex[gu] != cmd->indirect.tex[ii]) {
                                const SceGxmTexture* gt = vglGetGxmTextureById(cmd->indirect.tex[ii]);
                                if (gt) sceGxmSetFragmentTexture(gxm_ctx, gu, gt);
                                gxm_frag_tex[gu] = cmd->indirect.tex[ii];
                            }
                        }
                        loc = UL(ind_tex[ii]); if (loc >= 0) { VU(loc)->data[0] = (float)(3 + ii); }
                        loc = UL(ind_scale[ii]); if (loc >= 0) DFVN(loc, cmd->indirect.scale[ii], 8);
                    }
                    for (int ii = 0; ii < 3; ii++) {
                        loc = UL(ind_mtx_r0[ii]); if (loc >= 0) UNI3I(cmd->indirect.mtx_packed[ii][0], cmd->indirect.mtx_packed[ii][1], cmd->indirect.mtx_packed[ii][2]);
                        loc = UL(ind_mtx_r1[ii]); if (loc >= 0) UNI3I(cmd->indirect.mtx_packed[ii][3], cmd->indirect.mtx_packed[ii][4], cmd->indirect.mtx_packed[ii][5]);
                    }
                    for (int s = 0; s < cmd->tev.num_stages && s < PC_GX_MAX_TEV_STAGES; s++) {
                        loc = UL(tev_ind_cfg[s]);
                        if (loc >= 0) UNI4I(cmd->indirect.cfg[s][0], cmd->indirect.cfg[s][1], cmd->indirect.cfg[s][2], cmd->indirect.cfg[s][3]);
                        loc = UL(tev_ind_wrap[s]);
                        if (loc >= 0) UNI3I(cmd->indirect.wrap[s][0], cmd->indirect.wrap[s][1], cmd->indirect.wrap[s][2]);
                    }
                }
            }
            gl_cache_active_texture(GL_TEXTURE0);

            if (dirty & PC_GX_DIRTY_FOG) {
                loc = UL(fog_type);  if (loc >= 0) UNI1I(cmd->fog.type);
                loc = UL(fog_start); if (loc >= 0) { VU(loc)->data[0] = cmd->fog.start; }
                loc = UL(fog_end);   if (loc >= 0) { VU(loc)->data[0] = cmd->fog.end; }
                loc = UL(fog_color); if (loc >= 0) DFVN(loc, cmd->fog.color, 16);
            }

            // flag VitaGL's uniform flush only for the side that changed.
            #define VERT_DIRTY_MASK (PC_GX_DIRTY_PROJECTION | PC_GX_DIRTY_MODELVIEW | \
                                    PC_GX_DIRTY_TEXGEN | PC_GX_DIRTY_LIGHTING)
            #define FRAG_DIRTY_MASK (PC_GX_DIRTY_TEV_STAGES | PC_GX_DIRTY_TEV_COLORS | \
                                    PC_GX_DIRTY_KONST | PC_GX_DIRTY_ALPHA_CMP | \
                                    PC_GX_DIRTY_TEXTURES | PC_GX_DIRTY_INDIRECT | \
                                    PC_GX_DIRTY_FOG | PC_GX_DIRTY_LIGHTING)
            if (dirty & VERT_DIRTY_MASK) dirty_vert_unifs = 1;
            if (dirty & FRAG_DIRTY_MASK) dirty_frag_unifs = 1;
            #undef DFVN
        }
#ifdef VITA_DEBUG
        uint32_t ts_glstate_start = sceKernelGetProcessTimeLow();
        vita_timing.submit_uniform_us += ts_glstate_start - ts_unif_start;
#endif

        #define GL_STATE_ANY (PC_GX_DIRTY_DEPTH | PC_GX_DIRTY_COLOR_MASK | \
                              PC_GX_DIRTY_CULL | PC_GX_DIRTY_BLEND)
        if (cmd->dirty & GL_STATE_ANY) {
        if (cmd->dirty & PC_GX_DIRTY_DEPTH) {
            if (cmd->gl_state.z_compare_enable) {
                if (gl_cache.depth_test != 1) { glEnable(GL_DEPTH_TEST); gl_cache.depth_test = 1; }
                GLenum zfunc = ((unsigned)cmd->gl_state.z_compare_func < 8) ? gx_compare_to_gl[cmd->gl_state.z_compare_func] : GL_LEQUAL;
                if (gl_cache.depth_func != zfunc) { glDepthFunc(zfunc); gl_cache.depth_func = zfunc; }
            } else {
                if (gl_cache.depth_test != 0) { glDisable(GL_DEPTH_TEST); gl_cache.depth_test = 0; }
            }
            int dm = cmd->gl_state.z_update_enable ? 1 : 0;
            if (gl_cache.depth_mask != dm) { glDepthMask(dm ? GL_TRUE : GL_FALSE); gl_cache.depth_mask = dm; }
        }

        if (cmd->dirty & PC_GX_DIRTY_COLOR_MASK) {
            int rgb = cmd->gl_state.color_update_enable ? 1 : 0;
            int a = cmd->gl_state.alpha_update_enable ? 1 : 0;
            if (gl_cache.color_mask_rgb != rgb || gl_cache.color_mask_a != a) {
                glColorMask(rgb, rgb, rgb, a);
                gl_cache.color_mask_rgb = rgb;
                gl_cache.color_mask_a = a;
            }
        }

        if (cmd->dirty & PC_GX_DIRTY_CULL) {
            switch (cmd->gl_state.cull_mode) {
                case GX_CULL_NONE:
                    if (gl_cache.cull_face != 0) { glDisable(GL_CULL_FACE); gl_cache.cull_face = 0; }
                    break;
                case GX_CULL_FRONT:
                    if (gl_cache.cull_face != 1) { glEnable(GL_CULL_FACE); gl_cache.cull_face = 1; }
                    if (gl_cache.cull_mode != GL_FRONT) { glCullFace(GL_FRONT); gl_cache.cull_mode = GL_FRONT; }
                    break;
                case GX_CULL_BACK:
                    if (gl_cache.cull_face != 1) { glEnable(GL_CULL_FACE); gl_cache.cull_face = 1; }
                    if (gl_cache.cull_mode != GL_BACK) { glCullFace(GL_BACK); gl_cache.cull_mode = GL_BACK; }
                    break;
                case GX_CULL_ALL:
                    if (gl_cache.cull_face != 1) { glEnable(GL_CULL_FACE); gl_cache.cull_face = 1; }
                    if (gl_cache.cull_mode != GL_FRONT_AND_BACK) { glCullFace(GL_FRONT_AND_BACK); gl_cache.cull_mode = GL_FRONT_AND_BACK; }
                    break;
            }
        }

        if (cmd->dirty & PC_GX_DIRTY_BLEND) {
            switch (cmd->gl_state.blend_mode) {
                case GX_BM_NONE:
                    if (gl_cache.blend != 0) { glDisable(GL_BLEND); gl_cache.blend = 0; }
                    break;
                case GX_BM_BLEND: {
                    GLenum src = ((unsigned)cmd->gl_state.blend_src < 8) ? gx_blend_src_to_gl[cmd->gl_state.blend_src] : GL_ONE;
                    GLenum dst = ((unsigned)cmd->gl_state.blend_dst < 8) ? gx_blend_dst_to_gl[cmd->gl_state.blend_dst] : GL_ZERO;
                    if (cmd->gl_state.blend_src == GX_BL_DSTALPHA && cmd->gl_state.blend_dst == GX_BL_INVDSTALPHA) {
                        src = GL_SRC_ALPHA; dst = GL_ONE_MINUS_SRC_ALPHA;
                    }
                    if (src == GL_ONE && dst == GL_ZERO) {
                        if (gl_cache.blend != 0) { glDisable(GL_BLEND); gl_cache.blend = 0; }
                    } else {
                        if (gl_cache.blend != 1) { glEnable(GL_BLEND); gl_cache.blend = 1; }
                        if (gl_cache.blend_eq != GL_FUNC_ADD) { glBlendEquation(GL_FUNC_ADD); gl_cache.blend_eq = GL_FUNC_ADD; }
                        if (gl_cache.blend_src != src || gl_cache.blend_dst != dst) {
                            glBlendFunc(src, dst);
                            gl_cache.blend_src = src;
                            gl_cache.blend_dst = dst;
                        }
                    }
                    break;
                }
                case GX_BM_LOGIC:
                    if (gl_cache.blend != 0) { glDisable(GL_BLEND); gl_cache.blend = 0; }
                    break;
                case GX_BM_SUBTRACT:
                    if (gl_cache.blend != 1) { glEnable(GL_BLEND); gl_cache.blend = 1; }
                    if (gl_cache.blend_eq != GL_FUNC_REVERSE_SUBTRACT) {
                        glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
                        gl_cache.blend_eq = GL_FUNC_REVERSE_SUBTRACT;
                    }
                    if (gl_cache.blend_src != GL_ONE || gl_cache.blend_dst != GL_ONE) {
                        glBlendFunc(GL_ONE, GL_ONE);
                        gl_cache.blend_src = GL_ONE;
                        gl_cache.blend_dst = GL_ONE;
                    }
                    break;
            }
        }
        } // end GL_STATE_ANY
        #undef GL_STATE_ANY

        // viewport/scissor short-circuit when raw inputs are unchanged.
        if (last_vp_input_valid &&
            __builtin_memcmp(cmd->viewport, last_vp_input, 24) == 0 &&
            __builtin_memcmp(cmd->scissor, last_sc_input, 16) == 0 &&
            cmd->widescreen_stretch == last_vp_ws) {
            if (gl_cache.scissor_test_enabled != 1) {
                glEnable(GL_SCISSOR_TEST);
                gl_cache.scissor_test_enabled = 1;
            }
        } else {
            float vp_left = cmd->viewport[0];
            float vp_top  = cmd->viewport[1];
            float vp_wd   = cmd->viewport[2];
            float vp_ht   = cmd->viewport[3];
            int target_w = g_pc_window_w;
            int target_h = g_pc_window_h;
            int vp_offset_x = 0;
            if (g_pc_settings.aspect_mode == 1 && g_aspect_active) {
                vita_get_43_layout(&target_w, &vp_offset_x);
            }
            float sx = (float)target_w / (float)PC_GC_WIDTH;
            float sy = (float)target_h / (float)PC_GC_HEIGHT;
            float adj_left = vp_left, adj_wd = vp_wd;
#ifdef PC_ENHANCEMENTS
            if (cmd->widescreen_stretch == 2 && g_aspect_active && g_pc_settings.aspect_mode == 0) {
                int is_full = (vp_left < 1.0f && vp_top < 1.0f &&
                               vp_wd > (float)(PC_GC_WIDTH - 1) &&
                               vp_ht > (float)(PC_GC_HEIGHT - 1));
                if (!is_full) {
                    adj_left = g_aspect_offset + vp_left * g_aspect_factor;
                    adj_wd = vp_wd * g_aspect_factor;
                }
            }
#endif
            int gl_x = vp_offset_x + (int)(adj_left * sx);
            int gl_w = (int)(adj_wd * sx);
            int gl_h = (int)(vp_ht * sy);
            int gl_y_vp = target_h - (int)(vp_top * sy) - gl_h;
            if (gl_cache.vp_x != gl_x || gl_cache.vp_y != gl_y_vp ||
                gl_cache.vp_w != gl_w || gl_cache.vp_h != gl_h) {
                glViewport(gl_x, gl_y_vp, gl_w, gl_h);
                gl_cache.vp_x = gl_x; gl_cache.vp_y = gl_y_vp;
                gl_cache.vp_w = gl_w; gl_cache.vp_h = gl_h;
            }
            if (gl_cache.depth_near != cmd->viewport[4] ||
                gl_cache.depth_far != cmd->viewport[5]) {
                glDepthRangef(cmd->viewport[4], cmd->viewport[5]);
                gl_cache.depth_near = cmd->viewport[4];
                gl_cache.depth_far = cmd->viewport[5];
            }

            // Scissor
            int sc_left = cmd->scissor[0];
            int sc_top  = cmd->scissor[1];
            int sc_wd   = cmd->scissor[2];
            int sc_ht   = cmd->scissor[3];
#ifdef PC_ENHANCEMENTS
            int sc_gl_x = vp_offset_x + (int)((float)sc_left * sx);
            int sc_gl_w = (int)((float)sc_wd * sx);
            int sc_gl_h = (int)((float)sc_ht * sy);
            int sc_gl_y = target_h - (int)((float)sc_top * sy) - sc_gl_h;
#else
            int sc_gl_x = sc_left;
            int sc_gl_w = sc_wd;
            int sc_gl_h = sc_ht;
            int sc_gl_y = PC_GC_HEIGHT - sc_top - sc_ht;
#endif
            if (gl_cache.scissor_test_enabled != 1) {
                glEnable(GL_SCISSOR_TEST);
                gl_cache.scissor_test_enabled = 1;
            }
            if (gl_cache.sc_x != sc_gl_x || gl_cache.sc_y != sc_gl_y ||
                gl_cache.sc_w != sc_gl_w || gl_cache.sc_h != sc_gl_h) {
                glScissor(sc_gl_x, sc_gl_y, sc_gl_w, sc_gl_h);
                gl_cache.sc_x = sc_gl_x; gl_cache.sc_y = sc_gl_y;
                gl_cache.sc_w = sc_gl_w; gl_cache.sc_h = sc_gl_h;
            }

            __builtin_memcpy(last_vp_input, cmd->viewport, 24);
            __builtin_memcpy(last_sc_input, cmd->scissor, 16);
            last_vp_ws = cmd->widescreen_stretch;
            last_vp_input_valid = 1;
        }


        {
            GLenum gl_prim;
            switch (cmd->primitive) {
                case GX_QUADS:
                case GX_TRIANGLES:
                case GX_TRIANGLESTRIP:
                case GX_TRIANGLEFAN:   gl_prim = GL_TRIANGLES; break;
                case GX_LINES:
                case GX_LINESTRIP:     gl_prim = GL_LINES; break;
                case GX_POINTS:        gl_prim = GL_POINTS; break;
                default:               gl_prim = GL_TRIANGLES; break;
            }

            // fold consecutive cmds with the same state into one
            // glDrawElements. needs same shader, same primitive,
            // contiguous indices, no EFB refs or captures between them,
            // and either no dirty bits or only DIRTY_LIGHTING where the
            // lighting bytes actually match. prededup skips the lighting
            // dedup frame-wide (hit rate too low), but adjacent same-
            // material draws do share lighting, so verify here.
            int merged_count = cmd->idx_count;
            int merged_until = i;
            extern int vita_disable_merge;
            if (!vita_disable_merge && cmd->idx_count > 0 && gl_prim == GL_TRIANGLES) {
                while (merged_until + 1 < rd_count) {
                    int next_idx_pos = merged_until + 1;
                    PCGXDrawCmd* next = &cmd_queue_db[rd][next_idx_pos];
                    if (next->shader == 0) break;
                    if (next->shader != cmd->shader) break;
                    if (next->shader_changed) break;
                    if (next->dirty & ~(unsigned int)PC_GX_DIRTY_LIGHTING) break;
                    if (next->dirty & PC_GX_DIRTY_LIGHTING) {
                        if (__builtin_memcmp(&next->lighting, &cmd->lighting,
                                             sizeof(PCGXCmdLighting)) != 0) break;
                    }
                    if (next->idx_count <= 0) break;
                    if (next->idx_offset != cmd->idx_offset + merged_count) break;
                    GLenum nprim;
                    switch (next->primitive) {
                        case GX_QUADS:
                        case GX_TRIANGLES:
                        case GX_TRIANGLESTRIP:
                        case GX_TRIANGLEFAN: nprim = GL_TRIANGLES; break;
                        default: nprim = GL_POINTS; break;
                    }
                    if (nprim != GL_TRIANGLES) break;
                    if (next->textures.efb_src_ptr[0] | next->textures.efb_src_ptr[1] | next->textures.efb_src_ptr[2]) break;
                    if (rd_efb_next < rd_efb_count &&
                        efb_capture_db[rd][rd_efb_next].after_draw_idx <= merged_until) break;
                    merged_count += next->idx_count;
                    merged_until++;
                }
            }

#ifdef VITA_DEBUG
            uint32_t ts_draw_start = sceKernelGetProcessTimeLow();
            submit_setup_acc += ts_draw_start - ts_setup_start;
            vita_timing.submit_glstate_us += ts_draw_start - ts_glstate_start;
#endif
            if (merged_count > 0) {
                glDrawElements(gl_prim, merged_count, GL_UNSIGNED_SHORT,
                               (void*)(uintptr_t)(cmd->idx_offset * sizeof(GLushort)));
                actual_draws++;
            }
#ifdef VITA_DEBUG
            ts_setup_start = sceKernelGetProcessTimeLow();
            submit_draw_acc += ts_setup_start - ts_draw_start;
#endif

            if (merged_until > i) {
                vita_stats.sort_merged += (merged_until - i);
                i = merged_until;
            }
        }

        if (cmd->gl_state.blend_mode == GX_BM_SUBTRACT) {
            glBlendEquation(GL_FUNC_ADD);
            gl_cache.blend_eq = GL_FUNC_ADD;
        }

        // EFB captures queued to fire after this draw. one persistent GL
        // texture per dest_ptr; glCopyTexImage2D overwrites it in place
        // so previously snapshotted obj_stages stay valid.
        while (rd_efb_next < rd_efb_count &&
               efb_capture_db[rd][rd_efb_next].after_draw_idx <= i) {
            PCGXEfbCapture* cap = &efb_capture_db[rd][rd_efb_next];
            int gl_y = g_pc_window_h - (cap->src_top + cap->src_h);
            if (gl_y >= 0 && cap->src_w > 0 && cap->src_h > 0) {
                GLuint efb_tex = pc_gx_efb_capture_get_or_create(cap->dest_ptr);
                if (efb_tex) {
                    gl_cache_active_texture(GL_TEXTURE7);
                    glBindTexture(GL_TEXTURE_2D, efb_tex);
                    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                                     cap->src_left, gl_y, cap->src_w, cap->src_h, 0);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    gl_cache_active_texture(GL_TEXTURE0);
                }

                if (cap->clear_after) {
                    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                    if (g_gx.current_shader)
                        glUseProgram(g_gx.current_shader);
                    gl_cache_reset();
                    g_gx.dirty = PC_GX_DIRTY_ALL;
                    last_vp_input_valid = 0;
                }
            }
            rd_efb_next++;
        }
#ifdef VITA_DEBUG
        submit_efb_acc += sceKernelGetProcessTimeLow() - ts_setup_start;
#endif
    }

    #undef UNI4I
    #undef UNI3I
    #undef UNI2I
    #undef UNI1I
    #undef UL

#ifdef VITA_DEBUG
    uint32_t t_loop_end = sceKernelGetProcessTimeLow();
#endif

    // fallback for captures whose after_draw_idx overshoots rd_count;
    // without this the game would sample a stale EFB on menu transitions.
    while (rd_efb_next < rd_efb_count) {
        PCGXEfbCapture* cap = &efb_capture_db[rd][rd_efb_next];
        int gl_y = g_pc_window_h - (cap->src_top + cap->src_h);
        if (gl_y >= 0 && cap->src_w > 0 && cap->src_h > 0) {
            GLuint efb_tex = pc_gx_efb_capture_get_or_create(cap->dest_ptr);
            if (efb_tex) {
                gl_cache_active_texture(GL_TEXTURE7);
                glBindTexture(GL_TEXTURE_2D, efb_tex);
                glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                                 cap->src_left, gl_y, cap->src_w, cap->src_h, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                gl_cache_active_texture(GL_TEXTURE0);
            }
        }
        rd_efb_next++;
    }

    vita_stats.merged_draws = actual_draws;
    vgl_fast_draw_mode = 0;
    // clear presubmit flag - next frame's core 2 pass will set it
    vita_presubmit_done = 0;

    cmd_queue_count_db[rd] = 0;
    cmd_vert_count_db[rd] = 0;
    cmd_last_shader_db[rd] = 0;
    efb_capture_count_db[rd] = 0;

    // draw pillarbox banners after all game draws, before swap
    banner_draw_bars();
    // banner_draw_bars disables scissor + blend without notifying gl_cache;
    // invalidate so the next frame's first draw correctly re-enables.
    gl_cache.scissor_test_enabled = -1;
    gl_cache.blend = -1;

    pc_gx_texture_flush_deferred_deletes();

#ifdef VITA_DEBUG
    // readback vitaGL per-phase counters accumulated across this frame's
    // draws. no-op returns zeros when vitaGL built without
    // DRAW_PHASE_PROFILING.
    extern void vgl_get_draw_phases(unsigned int out[6]);
    unsigned int ph[6];
    vgl_get_draw_phases(ph);
    vita_timing.vgl_frag_tex_us    = ph[0];
    vita_timing.vgl_vert_tex_us    = ph[1];
    vita_timing.vgl_align_attrs_us = ph[2];
    vita_timing.vgl_patch_vprog_us = ph[3];
    vita_timing.vgl_upload_unif_us = ph[4];
    vita_timing.vgl_vstreams_us    = ph[5];
    // our-side per-cmd phase accumulators.
    vita_timing.submit_state_us    = submit_setup_acc;   // all prep before glDrawElements
    vita_timing.submit_draw_us     = submit_draw_acc;    // the glDrawElements call stack
    vita_timing.submit_efb_us      = submit_efb_acc;     // post-draw + EFB capture
    vita_timing.submit_texbind_us  = 0;
    // submit_uniform_us + submit_glstate_us are accumulated inline in the cmd loop
    vita_timing.submit_postsub_us  = sceKernelGetProcessTimeLow() - t_loop_end;
#endif
}


void vita_gx_flush_vertices_cmdbuf(int count) {
#ifdef VITA_DEBUG
    unsigned int _flush_t0 = sceKernelGetProcessTimeLow();
#endif

    vita_cpu_lit_active = 0;
    int saved_enable = g_gx.chan_ctrl_enable[0];
    int saved_mat = g_gx.chan_ctrl_mat_src[0];
    int saved_amat = g_gx.chan_ctrl_mat_src[1];
    if (g_gx.chan_ctrl_enable[0] && g_gx.num_chans >= 1) {
        g_gx.chan_ctrl_enable[0] = 0;
        g_gx.chan_ctrl_mat_src[0] = 1;
        g_gx.chan_ctrl_mat_src[1] = 1;
        g_gx.dirty |= PC_GX_DIRTY_LIGHTING;
        vita_cpu_lit_active = 1;
    }

    // Shader lookup (pure CPU, no GL calls)
#ifdef VITA_DEBUG
    unsigned int _tev_t0 = sceKernelGetProcessTimeLow();
#endif
    GLuint shader = pc_gx_tev_get_shader(&g_gx);
#ifdef VITA_DEBUG
    vita_timing.tevmatch_us += sceKernelGetProcessTimeLow() - _tev_t0;
#endif
    if (shader == 0) {
        if (vita_cpu_lit_active) {
            g_gx.chan_ctrl_enable[0] = saved_enable;
            g_gx.chan_ctrl_mat_src[0] = saved_mat;
            g_gx.chan_ctrl_mat_src[1] = saved_amat;
            g_gx.dirty |= PC_GX_DIRTY_LIGHTING;
        }
        return;
    }

    int shader_changed = (shader != cmd_last_shader);
    if (shader_changed) cmd_last_shader = shader;
    unsigned int dirty = shader_changed ? PC_GX_DIRTY_ALL : g_gx.dirty;

    // Buffer overflow check
    if (cmd_vert_count + count > PC_GX_MAX_VERTS || cmd_queue_count >= CMD_QUEUE_MAX) {
        vita_stats.dropped_draws++;
        if (vita_stats.dropped_draws <= 5)
            vita_log("[GX] OVERFLOW: dropping draw (verts=%d+%d/%d, cmds=%d/%d)\n",
                     cmd_vert_count, count, PC_GX_MAX_VERTS, cmd_queue_count, CMD_QUEUE_MAX);
        if (vita_cpu_lit_active) {
            g_gx.chan_ctrl_enable[0] = saved_enable;
            g_gx.chan_ctrl_mat_src[0] = saved_mat;
            g_gx.chan_ctrl_mat_src[1] = saved_amat;
            g_gx.dirty |= PC_GX_DIRTY_LIGHTING;
        }
        return;
    }

    // vertices for this batch were written directly into cmd_verts via
    // g_gx.vertex_write_ptr (set in vita_cmdbuf_begin_vertex_batch).
    // no memcpy needed in the common case. overflowing batches landed
    // in the scratch vertex_buffer and need to be copied in if they fit.
    if (g_gx.vertex_write_ptr == &g_gx.vertex_buffer[0]) {
        memcpy(cmd_verts + cmd_vert_count, g_gx.vertex_buffer, count * sizeof(PCGXVertex));
    }

#ifdef VITA_DEBUG
    unsigned int _vtx_t0 = sceKernelGetProcessTimeLow();
#endif

    PCGXDrawCmd* cmd = &cmd_queue[cmd_queue_count];
    cmd->vert_offset = cmd_vert_count;
    cmd->vert_count = count;
    cmd->primitive = g_gx.current_primitive;
    cmd->shader = shader;
    cmd->shader_changed = shader_changed;
    cmd->dirty = dirty;
    cmd->textures.deferred_idx[0] = -1;
    cmd->textures.deferred_idx[1] = -1;
    cmd->textures.deferred_idx[2] = -1;
    // efb_src_ptr must be cleared every flush. if a prior frame set it
    // on this cmd slot and the current cmd doesn't touch textures, the
    // stale dest_ptr would trigger a wrong late-resolve at replay.
    cmd->textures.efb_src_ptr[0] = 0;
    cmd->textures.efb_src_ptr[1] = 0;
    cmd->textures.efb_src_ptr[2] = 0;

    // build per-draw indices inline on the worker so submit_frame only
    // needs a single glBufferData call and no per-draw index assembly.
    {
        int base = cmd_vert_count;
        int n = count;
        int total = frame_idx_count;
        GLushort* out_base = frame_indices + total;
        int emitted = 0;
        switch (g_gx.current_primitive) {
            case GX_QUADS: {
                int nq = n / 4;
                int need = nq * 6;
                if (total + need > FRAME_IDX_MAX) {
                    nq = (FRAME_IDX_MAX - total) / 6;
                    need = nq * 6;
                    if (nq < 0) nq = 0;
                }
                GLushort* out = out_base;
                for (int q = 0; q < nq; q++) {
                    int v = base + q * 4;
                    out[0] = (GLushort)v;
                    out[1] = (GLushort)(v + 1);
                    out[2] = (GLushort)(v + 2);
                    out[3] = (GLushort)v;
                    out[4] = (GLushort)(v + 2);
                    out[5] = (GLushort)(v + 3);
                    out += 6;
                }
                emitted = need;
                break;
            }
            case GX_TRIANGLESTRIP: {
                int ntri = n - 2;
                if (ntri < 0) ntri = 0;
                int need = ntri * 3;
                if (total + need > FRAME_IDX_MAX) {
                    ntri = (FRAME_IDX_MAX - total) / 3;
                    need = ntri * 3;
                    if (ntri < 0) ntri = 0;
                }
                GLushort* out = out_base;
                for (int t = 0; t < ntri; t++) {
                    if (t & 1) {
                        out[0] = (GLushort)(base + t + 1);
                        out[1] = (GLushort)(base + t);
                        out[2] = (GLushort)(base + t + 2);
                    } else {
                        out[0] = (GLushort)(base + t);
                        out[1] = (GLushort)(base + t + 1);
                        out[2] = (GLushort)(base + t + 2);
                    }
                    out += 3;
                }
                emitted = need;
                break;
            }
            case GX_TRIANGLEFAN: {
                int ntri = n - 2;
                if (ntri < 0) ntri = 0;
                int need = ntri * 3;
                if (total + need > FRAME_IDX_MAX) {
                    ntri = (FRAME_IDX_MAX - total) / 3;
                    need = ntri * 3;
                    if (ntri < 0) ntri = 0;
                }
                GLushort* out = out_base;
                GLushort base16 = (GLushort)base;
                for (int t = 0; t < ntri; t++) {
                    out[0] = base16;
                    out[1] = (GLushort)(base + t + 1);
                    out[2] = (GLushort)(base + t + 2);
                    out += 3;
                }
                emitted = need;
                break;
            }
            case GX_LINESTRIP: {
                int nseg = n - 1;
                if (nseg < 0) nseg = 0;
                int need = nseg * 2;
                if (total + need > FRAME_IDX_MAX) {
                    nseg = (FRAME_IDX_MAX - total) / 2;
                    need = nseg * 2;
                    if (nseg < 0) nseg = 0;
                }
                GLushort* out = out_base;
                for (int s = 0; s < nseg; s++) {
                    out[0] = (GLushort)(base + s);
                    out[1] = (GLushort)(base + s + 1);
                    out += 2;
                }
                emitted = need;
                break;
            }
            default: {
                int nn = n;
                if (total + nn > FRAME_IDX_MAX) {
                    nn = FRAME_IDX_MAX - total;
                    if (nn < 0) nn = 0;
                }
                GLushort* out = out_base;
                for (int v = 0; v < nn; v++) {
                    out[v] = (GLushort)(base + v);
                }
                emitted = nn;
                break;
            }
        }
        cmd->idx_offset = total;
        cmd->idx_count = emitted;
        frame_idx_count = total + emitted;
    }

#ifdef VITA_DEBUG
    unsigned int _tev_phase_t0 = sceKernelGetProcessTimeLow();
    vita_timing.flush_vtx_us += _tev_phase_t0 - _vtx_t0;
#endif

    if (dirty & PC_GX_DIRTY_PROJECTION)
        memcpy(cmd->transform.projection_mtx_t, g_gx.projection_mtx_t, 16 * sizeof(float));

    if (dirty & PC_GX_DIRTY_MODELVIEW) {
        cmd->transform.current_mtx = g_gx.current_mtx;
        memcpy(cmd->transform.pos_mtx_t, g_gx.pos_mtx_t[g_gx.current_mtx], 16 * sizeof(float));
        memcpy(cmd->transform.nrm_mtx_t, g_gx.nrm_mtx_t[g_gx.current_mtx], 9 * sizeof(float));
    }

    // TEV input snapshot only. resolution (ca/cb/cc/cd/aval/csrc/asrc/
    // param/aparam + tc_src) now runs inline at the top of prededup on
    // core 2, which reads cmd->tev.stages + colors + k_colors; those
    // must all be valid here, so we snapshot them inside the same gate.
    if (dirty & (PC_GX_DIRTY_TEV_STAGES | PC_GX_DIRTY_TEXTURES | PC_GX_DIRTY_TEV_COLORS)) {
        int ns = g_gx.num_tev_stages;
        if (ns > PC_GX_MAX_TEV_STAGES) ns = PC_GX_MAX_TEV_STAGES;
        if (ns < 0) ns = 0;
        // Store the clamped count so downstream loops stay in-bounds.
        cmd->tev.num_stages = ns;
        // Narrow: only copy active stages. Submit + resolve both loop
        // up to cmd->tev.num_stages, so stages[ns..] can stay stale.
        // Dense scenes often have num_stages=1 or 2, saving 136-272B.
        if (ns > 0) {
            memcpy(cmd->tev.stages, g_gx.tev_stages, ns * sizeof(PCGXTevStage));
        }

        if (dirty & (PC_GX_DIRTY_TEV_STAGES | PC_GX_DIRTY_TEV_COLORS)) {
            // Snapshot colors + k_colors UNCONDITIONALLY inside this gate.
            // Needed even when DIRTY_TEV_COLORS isn't set: core 2 resolve
            // reads cmd->tev.colors/k_colors, and cmd slot may have stale
            // values from a different frame's cmd that reused this slot.
            memcpy(cmd->tev.colors, g_gx.tev_colors, sizeof(cmd->tev.colors));
            memcpy(cmd->tev.k_colors, g_gx.tev_k_colors, sizeof(cmd->tev.k_colors));
        }
    }
    // cfg20/cfg48 tex remap bit. captured UNCONDITIONALLY: gating it on
    // DIRTY_TEV_STAGES/COLORS would zero the flag for cfg48 draws whose
    // TEV state is identical to the prior draw (no dirty bits) or that
    // only changed textures, silently dropping the swizzle and breaking
    // Nookway/post office sign rendering. The global is set by shader
    // selection (pc_gx_tev_get_shader) which always runs before this.
    cmd->tev_tex_remap = vita_tev_tex_remap ? 1 : 0;

#ifdef VITA_DEBUG
    unsigned int _state_phase_t0 = sceKernelGetProcessTimeLow();
    vita_timing.flush_tev_us += _state_phase_t0 - _tev_phase_t0;
#endif

    // KONST dirty: submit's KONST branch uploads cmd->tev.k_colors.
    // Skip redundant memcpy when the TEV gate above already copied it
    // (happens when DIRTY_KONST fires with DIRTY_TEV_STAGES/COLORS,
    // which is common on Vita since GXSetTevKColor sets both bits).
    if ((dirty & PC_GX_DIRTY_KONST) &&
        !(dirty & (PC_GX_DIRTY_TEV_STAGES | PC_GX_DIRTY_TEV_COLORS))) {
        memcpy(cmd->tev.k_colors, g_gx.tev_k_colors, sizeof(cmd->tev.k_colors));
    }

    if (dirty & PC_GX_DIRTY_ALPHA_CMP) {
        float ref0 = (float)g_gx.alpha_ref0 / 255.0f;
        float ref1 = (float)g_gx.alpha_ref1 / 255.0f;
        float comp0 = 0.0f;
        switch (g_gx.alpha_comp0) {
            case GX_NEVER:   ref0 = 2.0f; break;
            case GX_LESS:    comp0 = 1.0f; break;
            case GX_EQUAL:   break;
            case GX_LEQUAL:  comp0 = 1.0f; ref0 += 1.0f / 255.0f; break;
            case GX_GREATER: ref0 += 1.0f / 255.0f; break;
            case GX_NEQUAL:  ref0 = -1.0f; break;
            case GX_GEQUAL:  break;
            case GX_ALWAYS:  ref0 = -1.0f; break;
        }
        switch (g_gx.alpha_comp1) {
            case GX_NEVER:  ref1 = 2.0f; break;
            case GX_GEQUAL: break;
            case GX_ALWAYS: ref1 = -1.0f; break;
            default: break;
        }
        float eff_ref = (ref0 > ref1) ? ref0 : ref1;
        if (g_gx.alpha_op == 1) eff_ref = (ref0 < ref1) ? ref0 : ref1;
        cmd->alpha.eff_ref = eff_ref;
        cmd->alpha.comp0_f = comp0;
    }

    if (dirty & PC_GX_DIRTY_LIGHTING) {
        cmd->lighting.chan_ctrl_enable_0 = g_gx.chan_ctrl_enable[0];
        cmd->lighting.chan_ctrl_enable_1 = g_gx.chan_ctrl_enable[1];
        memcpy(cmd->lighting.mat_color_0, g_gx.chan_mat_color[0], sizeof(cmd->lighting.mat_color_0));
        cmd->lighting.mat_src_0 = g_gx.chan_ctrl_mat_src[0];
        cmd->lighting.mat_src_1 = g_gx.chan_ctrl_mat_src[1];
        cmd->lighting.vs_lit_enable = saved_enable;
        cmd->lighting.vs_mat_src = saved_mat;
        cmd->lighting.vs_alpha_mat_src = saved_amat;
        cmd->lighting.num_chans = g_gx.num_chans;
        cmd->lighting.cpu_lit_active = 0;
        memcpy(cmd->lighting.amb_color_0, g_gx.chan_amb_color[0], sizeof(cmd->lighting.amb_color_0));
        cmd->lighting.amb_src_0 = g_gx.chan_ctrl_amb_src[0];
        cmd->lighting.light_mask_0 = g_gx.chan_ctrl_light_mask[0];
        // bulk-zero both arrays (compiles to NEON memset), then write
        // only active lights. inactive slots keep color=0 so position
        // doesn't matter (0 * anything = 0).
        __builtin_memset(cmd->lighting.light_pos, 0, sizeof(cmd->lighting.light_pos));
        __builtin_memset(cmd->lighting.light_color, 0, sizeof(cmd->lighting.light_color));
        int mask = g_gx.chan_ctrl_light_mask[0];
        while (mask) {
            int i = __builtin_ctz(mask);  // index of lowest set bit
            mask &= mask - 1;              // clear that bit
            __builtin_memcpy(cmd->lighting.light_pos[i], vita_light_norm[i], 12);
            __builtin_memcpy(cmd->lighting.light_color[i], g_gx.lights[i].color, 16);
        }
    }

    if (dirty & PC_GX_DIRTY_TEXGEN) {
        for (int tg = 0; tg < 2; tg++) {
            int mtx_id = g_gx.tex_gen_mtx[tg];
            int slot = pc_tex_mtx_id_to_slot(mtx_id);
            int has_mtx = (slot >= 0 && slot < 10);
            cmd->texgen.mtx_enable[tg] = has_mtx;
            if (has_mtx) {
                const float* tm = (const float*)g_gx.tex_mtx[slot];
                cmd->texgen.mtx_row0[tg][0] = tm[0]; cmd->texgen.mtx_row0[tg][1] = tm[1];
                cmd->texgen.mtx_row0[tg][2] = tm[2]; cmd->texgen.mtx_row0[tg][3] = tm[3];
                cmd->texgen.mtx_row1[tg][0] = tm[4]; cmd->texgen.mtx_row1[tg][1] = tm[5];
                cmd->texgen.mtx_row1[tg][2] = tm[6]; cmd->texgen.mtx_row1[tg][3] = tm[7];
            }
            cmd->texgen.gen_src[tg] = g_gx.tex_gen_src[tg];
        }
    }

    // snapshot GL texture ids and wrap modes for each active stage.
    if (dirty & (PC_GX_DIRTY_TEXTURES | PC_GX_DIRTY_TEV_STAGES)) {
        // bulk-zero first (one NEON memset instead of per-field scalar
        // stores), then set the "unset" defaults (-1, 0xFF) + active stages.
        __builtin_memset(&cmd->textures, 0, sizeof(cmd->textures));
        int ns = g_gx.num_tev_stages;
        if (ns > PC_GX_MAX_TEV_STAGES) ns = PC_GX_MAX_TEV_STAGES;
        for (int s = 0; s < PC_GX_MAX_TEV_STAGES; s++) {
            cmd->textures.map_stage[s] = -1;
            cmd->textures.deferred_idx[s] = -1;
            cmd->textures.wrap_s[s] = 0xFF;
            cmd->textures.wrap_t[s] = 0xFF;
            if (s < ns) {
                int tex_map = g_gx.tev_stages[s].tex_map;
                if (tex_map >= 0 && tex_map < 8) {
                    GLuint tid = g_gx.gl_textures[tex_map];
                    cmd->textures.obj_stage[s] = tid;
                    cmd->textures.map_stage[s] = tex_map;
                    if (tid == 0 && g_gx.gl_tex_deferred[tex_map] >= 0)
                        cmd->textures.deferred_idx[s] = g_gx.gl_tex_deferred[tex_map];
                    cmd->textures.wrap_s[s] = g_gx.tex_obj_wrap_s[tex_map];
                    cmd->textures.wrap_t[s] = g_gx.tex_obj_wrap_t[tex_map];
                    cmd->textures.efb_src_ptr[s] = g_gx.efb_src_ptr[tex_map];
                    if (vita_debug_notex) cmd->textures.obj_stage[s] = 0;
                    if (cmd->textures.obj_stage[s] != 0) cmd->textures.use_stage[s] = 1;
                }
            }
        }
        // cfg20/cfg48 tex remap. read the per-cmd snapshot, not the
        // global: the global is reset at the top of every shader match
        // (pc_gx_tev.c:1034) and re-set only if the matched cfg wants
        // remap. between shader selection and this snapshot block, the
        // shader-match cache fast path can restore a stale cached
        // value, so by the time we're here the global may be 0 even
        // though THIS draw needs the swizzle. cmd->tev_tex_remap is
        // captured at line 1908 right after shader selection, so it's
        // the authoritative per-draw value.
        if (cmd->tev_tex_remap) {
            cmd->textures.obj_stage[0] = cmd->textures.obj_stage[1];
            cmd->textures.use_stage[0] = cmd->textures.use_stage[1];
            cmd->textures.deferred_idx[0] = cmd->textures.deferred_idx[1];
            cmd->textures.wrap_s[0] = cmd->textures.wrap_s[1];
            cmd->textures.wrap_t[0] = cmd->textures.wrap_t[1];
            cmd->textures.efb_src_ptr[0] = cmd->textures.efb_src_ptr[1];
            cmd->textures.obj_stage[1] = 0;
            cmd->textures.use_stage[1] = 0;
            cmd->textures.deferred_idx[1] = -1;
            cmd->textures.efb_src_ptr[1] = 0;
        }
    }

    if (dirty & (PC_GX_DIRTY_INDIRECT | PC_GX_DIRTY_TEXTURES)) {
        cmd->indirect.num_stages = g_gx.num_ind_stages;
        if (g_gx.num_ind_stages > 0) {
            for (int i = 0; i < g_gx.num_ind_stages && i < 4; i++) {
                int ind_tex_map = g_gx.ind_order[i].tex_map;
                cmd->indirect.tex[i] = 0;
                if (ind_tex_map >= 0 && ind_tex_map < 8)
                    cmd->indirect.tex[i] = g_gx.gl_textures[ind_tex_map];
                cmd->indirect.scale[i][0] = 1.0f / (float)(1 << g_gx.ind_order[i].scale_s);
                cmd->indirect.scale[i][1] = 1.0f / (float)(1 << g_gx.ind_order[i].scale_t);
            }
            for (int i = 0; i < 3; i++) {
                float sv = ldexpf(1.0f, g_gx.ind_mtx_scale[i] + 17) / 1024.0f;
                for (int j = 0; j < 6; j++)
                    cmd->indirect.mtx_packed[i][j] = ((float*)g_gx.ind_mtx[i])[j] * sv;
            }
            for (int s = 0; s < g_gx.num_tev_stages && s < PC_GX_MAX_TEV_STAGES; s++) {
                PCGXTevStage* ts = &g_gx.tev_stages[s];
                cmd->indirect.cfg[s][0] = ts->ind_stage; cmd->indirect.cfg[s][1] = ts->ind_mtx;
                cmd->indirect.cfg[s][2] = ts->ind_bias;  cmd->indirect.cfg[s][3] = ts->ind_alpha;
                cmd->indirect.wrap[s][0] = ts->ind_wrap_s; cmd->indirect.wrap[s][1] = ts->ind_wrap_t;
                cmd->indirect.wrap[s][2] = ts->ind_add_prev;
            }
        }
    }

    if (dirty & PC_GX_DIRTY_FOG) {
        cmd->fog.type = vita_fog_disable ? 0 : g_gx.fog_type;
        cmd->fog.start = g_gx.fog_start;
        cmd->fog.end = g_gx.fog_end;
        memcpy(cmd->fog.color, g_gx.fog_color, sizeof(cmd->fog.color));
    }

    if (dirty & PC_GX_DIRTY_DEPTH) {
        cmd->gl_state.z_compare_enable = g_gx.z_compare_enable;
        cmd->gl_state.z_compare_func = g_gx.z_compare_func;
        cmd->gl_state.z_update_enable = g_gx.z_update_enable;
    }
    if (dirty & PC_GX_DIRTY_COLOR_MASK) {
        cmd->gl_state.color_update_enable = g_gx.color_update_enable;
        cmd->gl_state.alpha_update_enable = g_gx.alpha_update_enable;
    }
    if (dirty & PC_GX_DIRTY_CULL)
        cmd->gl_state.cull_mode = g_gx.cull_mode;
    cmd->gl_state.blend_mode = g_gx.blend_mode;
    if (dirty & PC_GX_DIRTY_BLEND) {
        cmd->gl_state.blend_src = g_gx.blend_src;
        cmd->gl_state.blend_dst = g_gx.blend_dst;
    }

    // viewport/scissor: copy unconditionally on shader change, otherwise
    // compare against last snapshot and skip if unchanged. saves ~40 bytes
    // memcpy per non-viewport-changing draw (~90% of draws).
    {
        static float last_vp[6];
        static int last_sc[4];
        static int last_ws = -1;
        static int last_vpsc_valid = 0;
        if (!shader_changed && last_vpsc_valid &&
            __builtin_memcmp(g_gx.viewport, last_vp, 24) == 0 &&
            __builtin_memcmp(g_gx.scissor, last_sc, 16) == 0 &&
            g_pc_widescreen_stretch == last_ws) {
            memcpy(cmd->viewport, last_vp, 24);
            memcpy(cmd->scissor, last_sc, 16);
            cmd->widescreen_stretch = last_ws;
        } else {
            memcpy(cmd->viewport, g_gx.viewport, 24);
            memcpy(cmd->scissor, g_gx.scissor, 16);
            cmd->widescreen_stretch = g_pc_widescreen_stretch;
            __builtin_memcpy(last_vp, g_gx.viewport, 24);
            __builtin_memcpy(last_sc, g_gx.scissor, 16);
            last_ws = g_pc_widescreen_stretch;
            last_vpsc_valid = 1;
        }
    }

    // PASSTHROUGH: simple shader does tex*ras, but pure-texture passthrough
    // TEV (D=TEXC, no B*C) must not multiply by ras. Force num_chans=0 so
    // ras_c=(1,1,1) and ras_a=1, making output = tex * 1 = tex.
    // Must force DIRTY_LIGHTING so the uniform gets uploaded during replay,
    // and snapshot the full lighting state if it wasn't already captured.
    if (vita_tev_passthrough) {
        if (!(dirty & PC_GX_DIRTY_LIGHTING)) {
            // lighting wasn't dirty, snapshot current state so cmd has valid data
            cmd->lighting.chan_ctrl_enable_0 = g_gx.chan_ctrl_enable[0];
            cmd->lighting.chan_ctrl_enable_1 = g_gx.chan_ctrl_enable[1];
            memcpy(cmd->lighting.mat_color_0, g_gx.chan_mat_color[0], sizeof(cmd->lighting.mat_color_0));
            cmd->lighting.mat_src_0 = g_gx.chan_ctrl_mat_src[0];
            cmd->lighting.mat_src_1 = g_gx.chan_ctrl_mat_src[1];
            cmd->lighting.vs_lit_enable = saved_enable;
            cmd->lighting.vs_mat_src = saved_mat;
            cmd->lighting.vs_alpha_mat_src = saved_amat;
            cmd->lighting.cpu_lit_active = 0;
        }
        cmd->lighting.num_chans = 0;
        cmd->dirty |= PC_GX_DIRTY_LIGHTING;
    }

    cmd_vert_count += count;
    cmd_queue_count++;

    // clear dirty before the cpu-lit restore so the next cmd picks up
    // the lighting re-mark. previously `g_gx.dirty = 0` was below the
    // restore, which erased the DIRTY_LIGHTING bit we just set, so the
    // next non-cpu-lit draw kept the cpu-lit lighting snapshot state
    // and rendered with wrong lighting uniforms for one frame.
    g_gx.dirty = 0;
    if (vita_cpu_lit_active) {
        g_gx.chan_ctrl_enable[0] = saved_enable;
        g_gx.chan_ctrl_mat_src[0] = saved_mat;
        g_gx.chan_ctrl_mat_src[1] = saved_amat;
        g_gx.dirty |= PC_GX_DIRTY_LIGHTING;
    }

#ifdef VITA_DEBUG
    unsigned int _flush_t_end = sceKernelGetProcessTimeLow();
    vita_timing.flush_state_us += _flush_t_end - _state_phase_t0;
    vita_timing.flush_us += _flush_t_end - _flush_t0;
#endif
}

#endif // TARGET_VITA
