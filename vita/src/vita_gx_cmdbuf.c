// vita_gx_cmdbuf.c
// double-buffered draw command queue, GL state cache, submit_frame replay

#ifdef TARGET_VITA

#include "pc_gx_internal.h"
#include "pc_settings.h"
#include "vita_shared.h"
#include "vita_banner.h"
#include "vita_gx_cmdbuf.h"
#include <psp2/kernel/processmgr.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dolphin/gx/GXEnum.h>

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


PCGXDrawCmd* cmd_queue_db[2] = {NULL, NULL};
int cmd_queue_count_db[2] = {0, 0};
PCGXVertex* cmd_verts_db[2] = {NULL, NULL};
int cmd_vert_count_db[2] = {0, 0};
GLuint cmd_last_shader_db[2] = {0, 0};
int cmd_write = 0;

PCGXEfbCapture efb_capture_db[2][EFB_CAPTURE_MAX];
int efb_capture_count_db[2] = {0, 0};


void vita_cmdbuf_init(void) {
    for (int i = 0; i < 2; i++) {
        cmd_queue_db[i] = (PCGXDrawCmd*)malloc(CMD_QUEUE_MAX * sizeof(PCGXDrawCmd));
        cmd_verts_db[i] = (PCGXVertex*)malloc(PC_GX_MAX_VERTS * sizeof(PCGXVertex));
        if (!cmd_queue_db[i] || !cmd_verts_db[i]) {
            fprintf(stderr, "[GX] Failed to allocate double-buffer command queue %d\n", i);
            exit(1);
        }
        cmd_queue_count_db[i] = 0;
        cmd_vert_count_db[i] = 0;
        cmd_last_shader_db[i] = 0;
    }
    cmd_write = 0;
}

void vita_cmdbuf_shutdown(void) {
    for (int i = 0; i < 2; i++) {
        free(cmd_queue_db[i]);  cmd_queue_db[i] = NULL;
        free(cmd_verts_db[i]);  cmd_verts_db[i] = NULL;
    }
}

void vita_cmdbuf_begin_frame(void) {
    vita_timing.flush_us = 0;
    vita_timing.texload_us = 0;
    vita_timing.tevmatch_us = 0;

    pc_gx_current_queue = GFX_QUEUE_WORK;
    cmd_queue_count = 0;
    cmd_vert_count = 0;
    cmd_last_shader = 0;
    efb_capture_count = 0;
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

// Map tex matrix ID to slot (from pc_gx.c)
static int pc_tex_mtx_id_to_slot(int id) {
    if (id == GX_IDENTITY) return -1;
    if (id >= 0 && id < 10) return id;
    if (id >= GX_TEXMTX0 && id < GX_IDENTITY) return (id - GX_TEXMTX0) / 3;
    return -1;
}


void pc_gx_submit_frame(void) {
    pc_gx_texture_process_deferred_uploads();
    pc_gx_texture_process_deferred_params();

    if (vita_stats.deferred_tex_uploads > 0)
        gl_cache_reset_textures();

    // Late texture re-resolve: deferred uploads just completed
    {
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
                            c->dirty |= (PC_GX_DIRTY_TEXTURES | PC_GX_DIRTY_TEV_STAGES);
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

    // frame dump removed

    if (rd_verts == 0 || rd_count == 0) {
        cmd_queue_count_db[rd] = 0;
        cmd_vert_count_db[rd] = 0;
        cmd_last_shader_db[rd] = 0;
        efb_capture_count_db[rd] = 0;
        vita_gpu_skip_draws = 1;
        return;
    }

    // ONE vertex upload for the entire frame
    glBufferData(GL_ARRAY_BUFFER, rd_verts * sizeof(PCGXVertex),
                 cmd_verts_db[rd], GL_STREAM_DRAW);

    vita_set_vertex_attrib_pointers();

    // Build per-frame index buffer
    {
        #define FRAME_IDX_MAX (PC_GX_MAX_VERTS * 3)
        static GLushort frame_indices[FRAME_IDX_MAX];
        int total_indices = 0;

        for (int i = 0; i < rd_count; i++) {
            PCGXDrawCmd* c = &cmd_queue_db[rd][i];
            int base = c->vert_offset;
            int n = c->vert_count;
            c->idx_offset = total_indices;

            if (c->primitive == GX_QUADS) {
                int nq = n / 4;
                for (int q = 0; q < nq && total_indices + 6 <= FRAME_IDX_MAX; q++) {
                    int v = base + q * 4;
                    frame_indices[total_indices++] = (GLushort)v;
                    frame_indices[total_indices++] = (GLushort)(v + 1);
                    frame_indices[total_indices++] = (GLushort)(v + 2);
                    frame_indices[total_indices++] = (GLushort)(v + 0);
                    frame_indices[total_indices++] = (GLushort)(v + 2);
                    frame_indices[total_indices++] = (GLushort)(v + 3);
                }
                c->idx_count = (n / 4) * 6;
            } else if (c->primitive == GX_TRIANGLESTRIP) {
                int ntri = n - 2;
                for (int t = 0; t < ntri && total_indices + 3 <= FRAME_IDX_MAX; t++) {
                    if (t & 1) {
                        frame_indices[total_indices++] = (GLushort)(base + t + 1);
                        frame_indices[total_indices++] = (GLushort)(base + t);
                        frame_indices[total_indices++] = (GLushort)(base + t + 2);
                    } else {
                        frame_indices[total_indices++] = (GLushort)(base + t);
                        frame_indices[total_indices++] = (GLushort)(base + t + 1);
                        frame_indices[total_indices++] = (GLushort)(base + t + 2);
                    }
                }
                c->idx_count = ntri * 3;
            } else if (c->primitive == GX_TRIANGLEFAN) {
                int ntri = n - 2;
                for (int t = 0; t < ntri && total_indices + 3 <= FRAME_IDX_MAX; t++) {
                    frame_indices[total_indices++] = (GLushort)base;
                    frame_indices[total_indices++] = (GLushort)(base + t + 1);
                    frame_indices[total_indices++] = (GLushort)(base + t + 2);
                }
                c->idx_count = ntri * 3;
            } else if (c->primitive == GX_LINESTRIP) {
                int nseg = n - 1;
                for (int s = 0; s < nseg && total_indices + 2 <= FRAME_IDX_MAX; s++) {
                    frame_indices[total_indices++] = (GLushort)(base + s);
                    frame_indices[total_indices++] = (GLushort)(base + s + 1);
                }
                c->idx_count = nseg * 2;
            } else {
                for (int v = 0; v < n && total_indices < FRAME_IDX_MAX; v++)
                    frame_indices[total_indices++] = (GLushort)(base + v);
                c->idx_count = n;
            }
        }

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_gx.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, total_indices * sizeof(GLushort),
                     frame_indices, GL_STREAM_DRAW);
        #undef FRAME_IDX_MAX
    }

    // Draw call merging
    {
        int merged = 0;
        for (int i = 0; i < rd_count - 1; i++) {
            PCGXDrawCmd* cur = &cmd_queue_db[rd][i];
            if (cur->shader == 0 || cur->idx_count == 0) continue;
            while (i + 1 < rd_count) {
                PCGXDrawCmd* nxt = &cmd_queue_db[rd][i + 1];
                if (nxt->shader == 0) { i++; continue; }
                if (nxt->shader_changed || nxt->dirty != 0) break;
                if (nxt->primitive != cur->primitive) break;
                if (cur->idx_offset + cur->idx_count != nxt->idx_offset) break;
                cur->idx_count += nxt->idx_count;
                nxt->shader = 0;
                nxt->idx_count = 0;
                merged++;
                i++;
            }
        }
        vita_stats.merged_draws = rd_count - merged;
    }

    // VitaGL uniform bypass macros
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

    for (int i = 0; i < rd_count; i++) {
        PCGXDrawCmd* cmd = &cmd_queue_db[rd][i];
        if (cmd->shader == 0) continue;

        // Skip draws with missing textures
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

        // Shader switch
        if (cmd->shader_changed) {
            vita_stats.shader_switches++;
            glUseProgram(cmd->shader);
            g_gx.current_shader = cmd->shader;
            pc_gx_cache_uniform_locations(cmd->shader);
            GLint sloc;
            sloc = UL(texture0); if (sloc >= 0) glUniform1i(sloc, 0);
            sloc = UL(texture1); if (sloc >= 0) glUniform1i(sloc, 1);
            sloc = UL(texture2); if (sloc >= 0) glUniform1i(sloc, 2);
            gl_cache_reset();
        }

        // Upload dirty uniforms: direct write to VitaGL u->data
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
                for (int s = 0; s < PC_GX_MAX_TEV_STAGES; s++) {
                    loc = UL(tev_tc_src[s]); if (loc >= 0) UNI1I(cmd->tev.tc_src[s]);
                }
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
                loc = UL(lighting_enabled); if (loc >= 0) UNI1I(cmd->lighting.chan_ctrl_enable_0);
                loc = UL(mat_color);  if (loc >= 0) DFVN(loc, cmd->lighting.mat_color_0, 16);
                loc = UL(chan_mat_src); if (loc >= 0) UNI1I(cmd->lighting.mat_src_0);
                loc = UL(num_chans);  if (loc >= 0) UNI1I(cmd->lighting.num_chans);
                loc = UL(alpha_lighting_enabled); if (loc >= 0) UNI1I(cmd->lighting.chan_ctrl_enable_1);
                loc = UL(alpha_mat_src); if (loc >= 0) UNI1I(cmd->lighting.mat_src_1);
                loc = UL(amb_color);  if (loc >= 0) DFVN(loc, cmd->lighting.amb_color_0, 16);
                loc = UL(chan_amb_src); if (loc >= 0) UNI1I(cmd->lighting.amb_src_0);
                loc = UL(light_mask); if (loc >= 0) UNI1I(cmd->lighting.light_mask_0);
                loc = UL(light_pos[0]);   if (loc >= 0) DFVN(loc, &cmd->lighting.light_pos[0][0], 96);
                loc = UL(light_color[0]); if (loc >= 0) DFVN(loc, &cmd->lighting.light_color[0][0], 128);
                if (cmd->lighting.vs_lit_enable) {
                    loc = UL(vs_mat_color);  if (loc >= 0) DFVN(loc, cmd->lighting.mat_color_0, 16);
                    loc = UL(vs_amb_color);  if (loc >= 0) DFVN(loc, cmd->lighting.amb_color_0, 16);
                    loc = UL(vs_chan_mat_src); if (loc >= 0) { VU(loc)->data[0] = (float)cmd->lighting.vs_mat_src; }
                    loc = UL(vs_chan_amb_src); if (loc >= 0) { VU(loc)->data[0] = (float)cmd->lighting.amb_src_0; }
                    loc = UL(vs_alpha_mat_src); if (loc >= 0) { VU(loc)->data[0] = (float)cmd->lighting.vs_alpha_mat_src; }
                    loc = UL(vs_alpha_lit); if (loc >= 0) { VU(loc)->data[0] = (float)cmd->lighting.chan_ctrl_enable_1; }
                    for (int li = 0; li < 8; li++) {
                        loc = UL(vs_ldir[li]);
                        if (loc >= 0) { VU(loc)->data[0] = cmd->lighting.light_pos[li][0]; VU(loc)->data[1] = cmd->lighting.light_pos[li][1]; VU(loc)->data[2] = cmd->lighting.light_pos[li][2]; }
                        loc = UL(vs_lcol[li]);
                        if (loc >= 0) DFVN(loc, cmd->lighting.light_color[li], 16);
                    }
                } else {
                    static const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                    static const float zero4[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                    loc = UL(vs_mat_color);  if (loc >= 0) DFVN(loc, white, 16);
                    loc = UL(vs_amb_color);  if (loc >= 0) DFVN(loc, white, 16);
                    loc = UL(vs_chan_mat_src); if (loc >= 0) { VU(loc)->data[0] = 1.0f; }
                    loc = UL(vs_chan_amb_src); if (loc >= 0) { VU(loc)->data[0] = 1.0f; }
                    loc = UL(vs_alpha_mat_src); if (loc >= 0) { VU(loc)->data[0] = 1.0f; }
                    loc = UL(vs_alpha_lit); if (loc >= 0) { VU(loc)->data[0] = 0.0f; }
                    for (int li = 0; li < 8; li++) {
                        loc = UL(vs_ldir[li]); if (loc >= 0) { VU(loc)->data[0] = 0; VU(loc)->data[1] = 0; VU(loc)->data[2] = 1; }
                        loc = UL(vs_lcol[li]); if (loc >= 0) DFVN(loc, zero4, 16);
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
                for (int s = 0; s < PC_GX_MAX_TEV_STAGES; s++) {
                    if (cmd->textures.use_stage[s])
                        gl_cache_bind_texture(GL_TEXTURE0 + s, cmd->textures.obj_stage[s]);
                }
                // Per-draw wrap: same GL texture may need different wrap modes
                // for different draws. Only call glTexParameteri when wrap differs
                // from what's currently set on the bound texture for this unit.
                for (int s = 0; s < PC_GX_MAX_TEV_STAGES; s++) {
                    if (cmd->textures.use_stage[s] && cmd->textures.wrap_s[s] != 0xFF) {
                        if (gl_cache_wrap_s[s] != cmd->textures.wrap_s[s] ||
                            gl_cache_wrap_t[s] != cmd->textures.wrap_t[s]) {
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
                    }
                }
                loc = UL(use_texture0); if (loc >= 0) UNI1I(cmd->textures.use_stage[0]);
                loc = UL(use_texture1); if (loc >= 0) UNI1I(cmd->textures.use_stage[1]);
                loc = UL(use_texture2); if (loc >= 0) UNI1I(cmd->textures.use_stage[2]);
            }

            if (dirty & (PC_GX_DIRTY_INDIRECT | PC_GX_DIRTY_TEXTURES)) {
                loc = UL(num_ind_stages); if (loc >= 0) UNI1I(cmd->indirect.num_stages);
                if (cmd->indirect.num_stages > 0) {
                    for (int ii = 0; ii < cmd->indirect.num_stages && ii < 4; ii++) {
                        if (cmd->indirect.tex[ii])
                            gl_cache_bind_texture(GL_TEXTURE3 + ii, cmd->indirect.tex[ii]);
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

            dirty_vert_unifs = 1;
            dirty_frag_unifs = 1;
            #undef DFVN
        }

        // GL state from snapshot
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

        // Viewport & Scissor from snapshot (cached)
        {
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
            if (cmd->widescreen_stretch == 2 && g_aspect_active) {
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
            if (gl_cache.sc_x != sc_gl_x || gl_cache.sc_y != sc_gl_y ||
                gl_cache.sc_w != sc_gl_w || gl_cache.sc_h != sc_gl_h) {
                glEnable(GL_SCISSOR_TEST);
                glScissor(sc_gl_x, sc_gl_y, sc_gl_w, sc_gl_h);
                gl_cache.sc_x = sc_gl_x; gl_cache.sc_y = sc_gl_y;
                gl_cache.sc_w = sc_gl_w; gl_cache.sc_h = sc_gl_h;
            }
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
            if (cmd->idx_count > 0) {
                glDrawElements(gl_prim, cmd->idx_count, GL_UNSIGNED_SHORT,
                               (void*)(uintptr_t)(cmd->idx_offset * sizeof(GLushort)));
                actual_draws++;
            }
        }

        // Post-draw blend reset
        if (cmd->gl_state.blend_mode == GX_BM_SUBTRACT) {
            glBlendEquation(GL_FUNC_ADD);
            gl_cache.blend_eq = GL_FUNC_ADD;
        }

        // EFB captures
        while (rd_efb_next < rd_efb_count &&
               efb_capture_db[rd][rd_efb_next].after_draw_idx <= i) {
            PCGXEfbCapture* cap = &efb_capture_db[rd][rd_efb_next];
            int gl_y = g_pc_window_h - (cap->src_top + cap->src_h);
            if (gl_y >= 0 && cap->src_w > 0 && cap->src_h > 0) {
                GLuint efb_tex;
                glGenTextures(1, &efb_tex);
                gl_cache_active_texture(GL_TEXTURE7);
                glBindTexture(GL_TEXTURE_2D, efb_tex);
                glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                                 cap->src_left, gl_y, cap->src_w, cap->src_h, 0);
                vita_efb_setup_texture(cap->dest_ptr, efb_tex);
                gl_cache_active_texture(GL_TEXTURE0);

                if (cap->clear_after) {
                    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                    if (g_gx.current_shader)
                        glUseProgram(g_gx.current_shader);
                    gl_cache_reset();
                }
            }
            rd_efb_next++;
        }
    }

    #undef UNI4I
    #undef UNI3I
    #undef UNI2I
    #undef UNI1I
    #undef UL

    vita_stats.merged_draws = actual_draws;

    cmd_queue_count_db[rd] = 0;
    cmd_vert_count_db[rd] = 0;
    cmd_last_shader_db[rd] = 0;
    efb_capture_count_db[rd] = 0;

    // Draw pillarbox banners after all game draws (before swap)
    banner_draw_bars();

    pc_gx_texture_flush_deferred_deletes();
}


void vita_gx_flush_vertices_cmdbuf(int count) {
    unsigned int _flush_t0 = sceKernelGetProcessTimeLow();

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
    unsigned int _tev_t0 = sceKernelGetProcessTimeLow();
    GLuint shader = pc_gx_tev_get_shader(&g_gx);
    vita_timing.tevmatch_us += sceKernelGetProcessTimeLow() - _tev_t0;
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

    memcpy(cmd_verts + cmd_vert_count, g_gx.vertex_buffer, count * sizeof(PCGXVertex));

    PCGXDrawCmd* cmd = &cmd_queue[cmd_queue_count];
    cmd->vert_offset = cmd_vert_count;
    cmd->vert_count = count;
    cmd->primitive = g_gx.current_primitive;
    cmd->shader = shader;
    cmd->shader_changed = shader_changed;
    cmd->textures.deferred_idx[0] = -1;
    cmd->textures.deferred_idx[1] = -1;
    cmd->textures.deferred_idx[2] = -1;
    cmd->dirty = dirty;

    if (dirty & PC_GX_DIRTY_PROJECTION)
        memcpy(cmd->transform.projection_mtx_t, g_gx.projection_mtx_t, 16 * sizeof(float));

    if (dirty & PC_GX_DIRTY_MODELVIEW) {
        cmd->transform.current_mtx = g_gx.current_mtx;
        memcpy(cmd->transform.pos_mtx_t, g_gx.pos_mtx_t[g_gx.current_mtx], 16 * sizeof(float));
        memcpy(cmd->transform.nrm_mtx_t, g_gx.nrm_mtx_t[g_gx.current_mtx], 9 * sizeof(float));
    }

    if (dirty & PC_GX_DIRTY_TEV_COLORS)
        memcpy(cmd->tev.colors, g_gx.tev_colors, sizeof(cmd->tev.colors));

    if (dirty & (PC_GX_DIRTY_TEV_STAGES | PC_GX_DIRTY_TEXTURES | PC_GX_DIRTY_TEV_COLORS)) {
        cmd->tev.num_stages = g_gx.num_tev_stages;
        for (int s = 0; s < PC_GX_MAX_TEV_STAGES; s++)
            cmd->tev.stages[s] = g_gx.tev_stages[s];

        if (dirty & (PC_GX_DIRTY_TEV_STAGES | PC_GX_DIRTY_TEV_COLORS)) {
            for (int s = 0; s < PC_GX_MAX_TEV_STAGES && s < g_gx.num_tev_stages; s++) {
                PCGXTevStage* ts = &g_gx.tev_stages[s];
                vita_resolve_cc(ts->color_a, ts, g_gx.tev_resolved_ca[s]);
                vita_resolve_cc(ts->color_b, ts, g_gx.tev_resolved_cb[s]);
                vita_resolve_cc(ts->color_c, ts, g_gx.tev_resolved_cc[s]);
                vita_resolve_cc(ts->color_d, ts, g_gx.tev_resolved_cd[s]);
                g_gx.tev_resolved_aval[s][0] = vita_resolve_ca(ts->alpha_a, ts);
                g_gx.tev_resolved_aval[s][1] = vita_resolve_ca(ts->alpha_b, ts);
                g_gx.tev_resolved_aval[s][2] = vita_resolve_ca(ts->alpha_c, ts);
                g_gx.tev_resolved_aval[s][3] = vita_resolve_ca(ts->alpha_d, ts);
                g_gx.tev_resolved_csrc[s][0] = vita_color_src(ts->color_a, s);
                g_gx.tev_resolved_csrc[s][1] = vita_color_src(ts->color_b, s);
                g_gx.tev_resolved_csrc[s][2] = vita_color_src(ts->color_c, s);
                g_gx.tev_resolved_csrc[s][3] = vita_color_src(ts->color_d, s);
                g_gx.tev_resolved_asrc[s][0] = vita_alpha_src(ts->alpha_a, s);
                g_gx.tev_resolved_asrc[s][1] = vita_alpha_src(ts->alpha_b, s);
                g_gx.tev_resolved_asrc[s][2] = vita_alpha_src(ts->alpha_c, s);
                g_gx.tev_resolved_asrc[s][3] = vita_alpha_src(ts->alpha_d, s);
                {
                    static const float bias_lut[]  = {0.0f, 0.5f, -0.5f};
                    static const float scale_lut[] = {1.0f, 2.0f, 4.0f, 0.5f};
                    g_gx.tev_resolved_param[s][0] = bias_lut[ts->color_bias < 3 ? ts->color_bias : 0];
                    g_gx.tev_resolved_param[s][1] = bias_lut[ts->alpha_bias < 3 ? ts->alpha_bias : 0];
                    g_gx.tev_resolved_param[s][2] = scale_lut[ts->color_scale < 4 ? ts->color_scale : 0];
                    g_gx.tev_resolved_param[s][3] = ts->color_op ? -1.0f : 1.0f;
                    g_gx.tev_resolved_aparam[s][0] = scale_lut[ts->alpha_scale < 4 ? ts->alpha_scale : 0];
                    g_gx.tev_resolved_aparam[s][1] = ts->alpha_op ? -1.0f : 1.0f;
                }
            }
            for (int s = 0; s < PC_GX_MAX_TEV_STAGES; s++) {
                int tc_src = 0;
                if (s < g_gx.num_tev_stages) {
                    int tc = g_gx.tev_stages[s].tex_coord;
                    tc_src = (tc >= 0 && tc < 8) ? tc : s;
                }
                g_gx.tev_resolved_tc_src[s] = tc_src;
            }
            g_gx.tev_resolve_valid = 1;
        }

        if (g_gx.tev_resolve_valid) {
            for (int s = 0; s < PC_GX_MAX_TEV_STAGES && s < g_gx.num_tev_stages; s++) {
                memcpy(cmd->tev.ca[s], g_gx.tev_resolved_ca[s], 12);
                memcpy(cmd->tev.cb[s], g_gx.tev_resolved_cb[s], 12);
                memcpy(cmd->tev.cc[s], g_gx.tev_resolved_cc[s], 12);
                memcpy(cmd->tev.cd[s], g_gx.tev_resolved_cd[s], 12);
                memcpy(cmd->tev.aval[s], g_gx.tev_resolved_aval[s], 16);
                memcpy(cmd->tev.csrc[s], g_gx.tev_resolved_csrc[s], 16);
                memcpy(cmd->tev.asrc[s], g_gx.tev_resolved_asrc[s], 16);
                memcpy(cmd->tev.param[s], g_gx.tev_resolved_param[s], 16);
                memcpy(cmd->tev.aparam[s], g_gx.tev_resolved_aparam[s], 8);
            }
            for (int s = 0; s < PC_GX_MAX_TEV_STAGES; s++) {
                cmd->tev.tc_src[s] = g_gx.tev_resolved_tc_src[s];
            }
        }

        // cfg20 tex remap
        if (vita_tev_tex_remap) {
            cmd->tev.tc_src[0] = cmd->tev.tc_src[1];
        }
    }

    if (dirty & PC_GX_DIRTY_KONST)
        memcpy(cmd->tev.k_colors, g_gx.tev_k_colors, sizeof(cmd->tev.k_colors));

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
        for (int i = 0; i < 8; i++) {
            if (g_gx.chan_ctrl_light_mask[0] & (1 << i)) {
                memcpy(cmd->lighting.light_pos[i], vita_light_norm[i], 3 * sizeof(float));
                memcpy(cmd->lighting.light_color[i], g_gx.lights[i].color, 4 * sizeof(float));
            } else {
                cmd->lighting.light_pos[i][0] = 0.0f; cmd->lighting.light_pos[i][1] = 0.0f; cmd->lighting.light_pos[i][2] = 1.0f;
                cmd->lighting.light_color[i][0] = cmd->lighting.light_color[i][1] = cmd->lighting.light_color[i][2] = cmd->lighting.light_color[i][3] = 0.0f;
            }
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

    // TEXTURES
    // resolve GL texture IDs and wrap modes at snapshot time
    if (dirty & (PC_GX_DIRTY_TEXTURES | PC_GX_DIRTY_TEV_STAGES)) {
        for (int s = 0; s < PC_GX_MAX_TEV_STAGES; s++) {
            cmd->textures.obj_stage[s] = 0;
            cmd->textures.use_stage[s] = 0;
            cmd->textures.map_stage[s] = -1;
            cmd->textures.deferred_idx[s] = -1;
            cmd->textures.wrap_s[s] = 0xFF; // unset
            cmd->textures.wrap_t[s] = 0xFF;
            if (s < g_gx.num_tev_stages) {
                int tex_map = g_gx.tev_stages[s].tex_map;
                if (tex_map >= 0 && tex_map < 8) {
                    cmd->textures.obj_stage[s] = g_gx.gl_textures[tex_map];
                    cmd->textures.map_stage[s] = tex_map;
                    if (cmd->textures.obj_stage[s] == 0 && g_gx.gl_tex_deferred[tex_map] >= 0)
                        cmd->textures.deferred_idx[s] = g_gx.gl_tex_deferred[tex_map];
                    cmd->textures.wrap_s[s] = g_gx.tex_obj_wrap_s[tex_map];
                    cmd->textures.wrap_t[s] = g_gx.tex_obj_wrap_t[tex_map];
                }
            }
            if (vita_debug_notex) cmd->textures.obj_stage[s] = 0;
            if (cmd->textures.obj_stage[s] != 0) cmd->textures.use_stage[s] = 1;
        }
        // cfg20 tex remap
        if (vita_tev_tex_remap) {
            cmd->textures.obj_stage[0] = cmd->textures.obj_stage[1];
            cmd->textures.use_stage[0] = cmd->textures.use_stage[1];
            cmd->textures.deferred_idx[0] = cmd->textures.deferred_idx[1];
            cmd->textures.wrap_s[0] = cmd->textures.wrap_s[1];
            cmd->textures.wrap_t[0] = cmd->textures.wrap_t[1];
            cmd->textures.obj_stage[1] = 0;
            cmd->textures.use_stage[1] = 0;
            cmd->textures.deferred_idx[1] = -1;
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

    memcpy(cmd->viewport, g_gx.viewport, sizeof(cmd->viewport));
    memcpy(cmd->scissor, g_gx.scissor, sizeof(cmd->scissor));
    cmd->widescreen_stretch = g_pc_widescreen_stretch;

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

    if (vita_cpu_lit_active) {
        g_gx.chan_ctrl_enable[0] = saved_enable;
        g_gx.chan_ctrl_mat_src[0] = saved_mat;
        g_gx.chan_ctrl_mat_src[1] = saved_amat;
        g_gx.dirty |= PC_GX_DIRTY_LIGHTING;
    }
    g_gx.dirty = 0;

    vita_timing.flush_us += sceKernelGetProcessTimeLow() - _flush_t0;
}

#endif // TARGET_VITA
