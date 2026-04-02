// vita_gx_cmdbuf.h
// double-buffered draw command queue
#ifndef VITA_GX_CMDBUF_H
#define VITA_GX_CMDBUF_H

#ifdef TARGET_VITA

#include "pc_gx_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int PCGXDirtyFlags;

typedef struct {
    float projection_mtx_t[16];
    int current_mtx;
    float pos_mtx_t[16];
    float nrm_mtx_t[9];
} PCGXCmdTransform;

typedef struct {
    int num_stages;
    PCGXTevStage stages[3];
    float ca[3][3], cb[3][3], cc[3][3], cd[3][3];
    float aval[3][4];
    float csrc[3][4], asrc[3][4];
    float param[3][4], aparam[3][2];
    int tc_src[3];
    float colors[4][4];
    float k_colors[4][4];
} PCGXCmdTEV;

typedef struct {
    float eff_ref;
    float comp0_f;
} PCGXCmdAlpha;

typedef struct {
    int chan_ctrl_enable_0;
    int chan_ctrl_enable_1;
    float mat_color_0[4];
    int mat_src_0;
    int mat_src_1;
    int num_chans;
    int cpu_lit_active;
    int vs_lit_enable;
    int vs_mat_src;
    int vs_alpha_mat_src;
    float amb_color_0[4];
    int amb_src_0;
    int light_mask_0;
    float light_pos[8][3];
    float light_color[8][4];
} PCGXCmdLighting;

typedef struct {
    int gen_mtx[2];
    float mtx_row0[2][4];
    float mtx_row1[2][4];
    int mtx_enable[2];
    int gen_src[2];
} PCGXCmdTexGen;

typedef struct {
    GLuint obj_stage[3];
    int use_stage[3];
    int map_stage[3];
    int deferred_idx[3];
    u32 wrap_s[3], wrap_t[3];
} PCGXCmdTextures;

typedef struct {
    int num_stages;
    int tex_map[4];
    GLuint tex[4];
    float scale[4][2];
    float mtx_packed[3][6];
    int cfg[3][4];
    int wrap[3][3];
} PCGXCmdIndirect;

typedef struct {
    int type;
    float start, end;
    float color[4];
} PCGXCmdFog;

typedef struct {
    int z_compare_enable, z_compare_func, z_update_enable;
    int color_update_enable, alpha_update_enable;
    int cull_mode;
    int blend_mode, blend_src, blend_dst;
} PCGXCmdGLState;

typedef struct {
    int vert_offset;
    int vert_count;
    int primitive;
    int idx_offset;
    int idx_count;

    GLuint shader;
    int shader_changed;

    PCGXDirtyFlags dirty;

    PCGXCmdTransform transform;
    PCGXCmdTEV tev;
    PCGXCmdAlpha alpha;
    PCGXCmdLighting lighting;
    PCGXCmdTexGen texgen;
    PCGXCmdTextures textures;
    PCGXCmdIndirect indirect;
    PCGXCmdFog fog;
    PCGXCmdGLState gl_state;

    float viewport[6];
    int scissor[4];
    int widescreen_stretch;
} PCGXDrawCmd;

typedef struct {
    int after_draw_idx;
    u32 dest_ptr;
    int src_left, src_top, src_w, src_h;
    int clear_after;
} PCGXEfbCapture;

#define CMD_QUEUE_MAX 512
#define EFB_CAPTURE_MAX 8
#define GFX_QUEUE_WORK   0
#define GFX_QUEUE_SHADOW 2

extern PCGXDrawCmd* cmd_queue_db[2];
extern int cmd_queue_count_db[2];
extern PCGXVertex* cmd_verts_db[2];
extern int cmd_vert_count_db[2];
extern GLuint cmd_last_shader_db[2];
extern PCGXEfbCapture efb_capture_db[2][EFB_CAPTURE_MAX];
extern int efb_capture_count_db[2];

#define cmd_queue       cmd_queue_db[cmd_write]
#define cmd_queue_count cmd_queue_count_db[cmd_write]
#define cmd_verts       cmd_verts_db[cmd_write]
#define cmd_vert_count  cmd_vert_count_db[cmd_write]
#define cmd_last_shader cmd_last_shader_db[cmd_write]
#define efb_captures      efb_capture_db[cmd_write]
#define efb_capture_count  efb_capture_count_db[cmd_write]

void vita_cmdbuf_init(void);
void vita_cmdbuf_shutdown(void);
void vita_cmdbuf_begin_frame(void);
void vita_gx_flush_vertices_cmdbuf(int count);
void vita_normalize_light(int i);
void vita_set_vertex_attrib_pointers(void);
void vita_efb_setup_texture(u32 dest_ptr, GLuint tex);

#ifdef __cplusplus
}
#endif

#endif // TARGET_VITA
#endif // VITA_GX_CMDBUF_H
