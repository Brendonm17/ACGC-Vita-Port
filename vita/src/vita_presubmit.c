// vita_presubmit.c
// core 2 pass that writes TEV_STAGES uniforms into the shadow before submit.
#ifdef TARGET_VITA

#include "pc_gx_internal.h"
#include "vita_shared.h"
#include "vita_gx_cmdbuf.h"
#include <string.h>

volatile int vita_presubmit_done = 0;

// must match the vgl_uni_t used in submit.
typedef struct {
    const void* ptr;
    float* data;
    unsigned int size;
    unsigned char is_frag;
    unsigned char is_vert;
} vgl_uni_t;

#define VU(loc) ((vgl_uni_t*)((intptr_t)(-(loc))))
#define DFVN(loc, src, bytes) __builtin_memcpy(VU(loc)->data, (src), (bytes))
#define UNI1I(v) do { VU(loc)->data[0] = (float)(v); } while(0)
#define UNI2I(a,b) do { float*_d=VU(loc)->data; _d[0]=(float)(a); _d[1]=(float)(b); } while(0)
#define UNI4I(a,b,c,d) do { float*_dd=VU(loc)->data; _dd[0]=(float)(a); _dd[1]=(float)(b); _dd[2]=(float)(c); _dd[3]=(float)(d); } while(0)
#define UL(field) (up->field)

static inline void write_tev_stages(const PCGXUloc* up, PCGXDrawCmd* cmd) {
    int loc;
    loc = UL(num_tev_stages);
    if (loc >= 0) UNI1I(cmd->tev.num_stages);
    for (int s = 0; s < PC_GX_MAX_TEV_STAGES && s < cmd->tev.num_stages; s++) {
        PCGXTevStage* ts = &cmd->tev.stages[s];
        loc = UL(tev_color_in[s]); if (loc >= 0) UNI4I(ts->color_a, ts->color_b, ts->color_c, ts->color_d);
        loc = UL(tev_alpha_in[s]); if (loc >= 0) UNI4I(ts->alpha_a, ts->alpha_b, ts->alpha_c, ts->alpha_d);
        loc = UL(tev_color_op[s]); if (loc >= 0) UNI1I(ts->color_op);
        loc = UL(tev_alpha_op[s]); if (loc >= 0) UNI1I(ts->alpha_op);
        loc = UL(tev_bsc[s]); if (loc >= 0) UNI4I(ts->color_bias, ts->color_scale, ts->alpha_bias, ts->alpha_scale);
        loc = UL(tev_out[s]); if (loc >= 0) UNI4I(ts->color_clamp, ts->alpha_clamp, ts->color_out, ts->alpha_out);
        loc = UL(tev_swap[s]); if (loc >= 0) UNI2I(ts->ras_swap, ts->tex_swap);
    }
    for (int s = 0; s < PC_GX_MAX_TEV_STAGES; s++) {
        loc = UL(tev_tc_src[s]);
        if (loc >= 0) UNI1I(cmd->tev.tc_src[s]);
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

void vita_cmdbuf_presubmit(void) {
    int rd = cmd_write;
    int count = cmd_queue_count_db[rd];
    if (count <= 0) return;

    GLuint current_shader = 0;
    const PCGXUloc* up = NULL;

    for (int i = 0; i < count; i++) {
        PCGXDrawCmd* cmd = &cmd_queue_db[rd][i];
        if (cmd->shader == 0 || cmd->idx_count == 0) continue;

        if (cmd->shader != current_shader) {
            current_shader = cmd->shader;
            if (current_shader < PC_GX_MAX_CACHED_SHADERS &&
                pc_gx_shader_uloc_cached[current_shader]) {
                up = &pc_gx_shader_uloc_cache[current_shader];
            } else {
                up = NULL;
            }
        }
        if (!up) continue;

        // late EFB re-resolve; can set DIRTY_TEXTURES so runs before the bit check.
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

        unsigned int dirty = cmd->dirty;
        if (dirty & PC_GX_DIRTY_TEV_STAGES) {
            write_tev_stages(up, cmd);
        }
    }

    vita_presubmit_done = 1;
}

#endif // TARGET_VITA
