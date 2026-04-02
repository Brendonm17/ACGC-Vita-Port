/* pc_gx_internal.h - GX state machine, vertex format, TEV config, GL objects */
#ifndef PC_GX_INTERNAL_H
#define PC_GX_INTERNAL_H

#include "pc_platform.h"

/* Define PC_GL_DEBUG to check for GL errors after significant calls */
#ifdef PC_GL_DEBUG
#define PC_GL_CHECK(label) do { \
    GLenum err_ = glGetError(); \
    if (err_ != GL_NO_ERROR) \
        printf("[GL ERR] %s: 0x%04X at %s:%d\n", label, err_, __FILE__, __LINE__); \
} while(0)
#else
#define PC_GL_CHECK(label) ((void)0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* --- Dirty flags for conditional uniform upload --- */
#define PC_GX_DIRTY_PROJECTION  (1u << 0)
#define PC_GX_DIRTY_MODELVIEW   (1u << 1)
#define PC_GX_DIRTY_TEV_COLORS  (1u << 2)
#define PC_GX_DIRTY_TEV_STAGES  (1u << 3)
#define PC_GX_DIRTY_SWAP_TABLES (1u << 4)
#define PC_GX_DIRTY_KONST       (1u << 5)
#define PC_GX_DIRTY_ALPHA_CMP   (1u << 6)
#define PC_GX_DIRTY_LIGHTING    (1u << 7)
#define PC_GX_DIRTY_TEXGEN      (1u << 8)
#define PC_GX_DIRTY_TEXTURES    (1u << 9)
#define PC_GX_DIRTY_INDIRECT    (1u << 10)
#define PC_GX_DIRTY_FOG         (1u << 11)
#define PC_GX_DIRTY_DEPTH       (1u << 12)
#define PC_GX_DIRTY_COLOR_MASK  (1u << 13)
#define PC_GX_DIRTY_CULL        (1u << 14)
#define PC_GX_DIRTY_BLEND       (1u << 15)
#define PC_GX_DIRTY_ALL         0xFFFFu
#define PC_GX_DIRTY_SET(flag) (g_gx.dirty |= (flag))
#define DIRTY(flag) PC_GX_DIRTY_SET(flag)

/* --- Vertex buffer --- */
#ifdef TARGET_VITA
#define PC_GX_MAX_VERTS       32768  // overworld needs ~16K+; 8K caused drops
#else
#define PC_GX_MAX_VERTS       65536
#endif
#define PC_GX_MAX_ATTRIB_SIZE 64
#define PC_GX_MAX_ATTR        26
#define PC_GX_MAX_VTXFMT      8
#define PC_GX_MAX_TEV_STAGES  3

typedef struct {
    int has_position;
    int has_normal;
    int has_color0;
    int has_color1;
    int has_texcoord[8];
    int texcoord_frac[8];
    int position_size;
    int color_size;
    int texcoord_size;
    int stride;
} PCGXVertexFormat;

typedef struct {
    float position[3];
    float normal[3];
    unsigned char color0[4];
#ifndef TARGET_VITA
    unsigned char color1[4];
#endif
#ifdef TARGET_VITA
    float texcoord[2][2];
#else
    float texcoord[8][2];
#endif
} PCGXVertex;

typedef struct {
    int color_a, color_b, color_c, color_d;
    int alpha_a, alpha_b, alpha_c, alpha_d;
    int color_op, color_bias, color_scale, color_clamp, color_out;
    int alpha_op, alpha_bias, alpha_scale, alpha_clamp, alpha_out;
    int tex_coord, tex_map, color_chan;
    int k_color_sel, k_alpha_sel;
    int ras_swap, tex_swap;
    int ind_stage, ind_format, ind_bias, ind_mtx, ind_wrap_s, ind_wrap_t;
    int ind_add_prev, ind_lod, ind_alpha;
} PCGXTevStage;

typedef struct {
    int r, g, b, a;  /* channel indices: 0=R, 1=G, 2=B, 3=A */
} PCGXTevSwapTable;

typedef struct {
    /* Primitive assembly */
    int current_primitive;
    int current_vtxfmt;
    int vertex_count;
    int expected_vertex_count;
    int in_begin;
    PCGXVertex vertex_buffer[PC_GX_MAX_VERTS];
    int current_vertex_idx;
    PCGXVertex current_vertex;

    /* Vertex descriptor */
    int vtx_desc[PC_GX_MAX_ATTR];
    PCGXVertexFormat vtx_fmt[PC_GX_MAX_VTXFMT];

    /* Transforms */
    float projection_mtx[4][4];
    int projection_type;
    float pos_mtx[10][3][4];
    float nrm_mtx[10][3][3];
    float tex_mtx[10][3][4];
    int current_mtx;
#ifdef TARGET_VITA
    // pre-transposed for VitaGL column-major upload
    float pos_mtx_t[10][16];   // 4x4 column-major
    float nrm_mtx_t[10][9];   // 3x3 column-major
    float projection_mtx_t[16]; // 4x4 column-major
#endif

    /* Viewport & scissor */
    float viewport[6];  /* x, y, w, h, near, far */
    int scissor[4];     /* left, top, w, h */

    /* TEV */
    int num_tev_stages;
    PCGXTevStage tev_stages[16];
    float tev_colors[4][4];    /* PREV, REG0, REG1, REG2 */
    float tev_k_colors[4][4];
#ifdef TARGET_VITA
    // cached pre-resolved TEV inputs
    float tev_resolved_ca[3][3], tev_resolved_cb[3][3], tev_resolved_cc[3][3], tev_resolved_cd[3][3];
    float tev_resolved_aval[3][4];
    float tev_resolved_csrc[3][4], tev_resolved_asrc[3][4];
    float tev_resolved_param[3][4], tev_resolved_aparam[3][2];
    int tev_resolved_tc_src[3];
    int tev_resolve_valid; // 0=stale, 1=valid
#endif
    PCGXTevSwapTable tev_swap_table[4];

    /* Textures */
    int num_tex_gens;
    int tex_gen_type[8];
    int tex_gen_src[8];
    int tex_gen_mtx[8];
    GLuint gl_textures[8];
    int gl_tex_deferred[8];
    int tex_obj_w[8];
    int tex_obj_h[8];
    int tex_obj_fmt[8];
    u32 tex_obj_wrap_s[8];
    u32 tex_obj_wrap_t[8];

    /* Lighting */
    int num_chans;
    float chan_amb_color[2][4];
    float chan_mat_color[2][4];
    int chan_ctrl_enable[4];
    int chan_ctrl_amb_src[4];
    int chan_ctrl_mat_src[4];
    int chan_ctrl_light_mask[4];
    int chan_ctrl_diff_fn[4];
    int chan_ctrl_attn_fn[4];

    struct {
        float pos[3];
        float dir[3];
        float color[4];
        float a0, a1, a2;  /* angular attenuation */
        float k0, k1, k2;  /* distance attenuation */
    } lights[8];

    /* Blend & depth */
    int blend_mode;
    int blend_src;
    int blend_dst;
    int blend_logic_op;
    int z_compare_enable;
    int z_compare_func;
    int z_update_enable;
    int color_update_enable;
    int alpha_update_enable;

    /* Alpha compare */
    int alpha_comp0;
    int alpha_ref0;
    int alpha_op;
    int alpha_comp1;
    int alpha_ref1;

    int cull_mode;

    /* Fog */
    int fog_type;
    float fog_start, fog_end, fog_near, fog_far;
    float fog_color[4];

    /* TLUT palette storage for CI4/CI8 textures */
    struct {
        const void* data;
        int format;      /* GX_TL_IA8=0, GX_TL_RGB5A3=1 */
        int n_entries;
        int is_be;       /* 1=big-endian (ROM/JSystem), 0=native LE (emu64 tlutconv) */
    } tlut[16];

    /* Indirect textures */
    int num_ind_stages;
    struct {
        int tex_coord;
        int tex_map;
        int scale_s;
        int scale_t;
    } ind_order[4];
    float ind_mtx[3][2][3];
    int   ind_mtx_scale[3];

    /* Deferred vertex commit: position starts vertex, commit on next position or GXEnd */
    int vertex_pending;

    /* GL objects */
    GLuint vao;
    GLuint vbo;
    GLuint ebo;
    GLuint current_shader;
#ifdef TARGET_VITA
    int vbo_ring_offset;
#endif

    /* Uniform locations (looked up once per shader change) */
    struct {
        GLint projection, modelview, normal_mtx;
        GLint tev_prev, tev_reg0, tev_reg1, tev_reg2;
        GLint num_tev_stages;
        GLint tev_color_in[PC_GX_MAX_TEV_STAGES], tev_alpha_in[PC_GX_MAX_TEV_STAGES];
        GLint tev_color_op[PC_GX_MAX_TEV_STAGES], tev_alpha_op[PC_GX_MAX_TEV_STAGES];
        GLint kcolor, tev_ksel;
        GLint alpha_comp0, alpha_ref0, alpha_op, alpha_comp1, alpha_ref1;
        GLint lighting_enabled, mat_color, amb_color;
        GLint chan_mat_src, chan_amb_src, num_chans;
        GLint alpha_lighting_enabled, alpha_mat_src;
        GLint light_mask, light_pos[8], light_color[8];
        // vertex shader lighting uniforms (u_vs_ prefix)
        GLint vs_mat_color, vs_amb_color, vs_chan_mat_src, vs_chan_amb_src;
        GLint vs_alpha_mat_src, vs_alpha_lit;
        GLint vs_ldir[8], vs_lcol[8];
        GLint texmtx_enable[2], texmtx_row0[2], texmtx_row1[2], texgen_src[2];
        GLint use_texture0, use_texture1, use_texture2;
        GLint texture0, texture1, texture2;
        GLint tev_tc_src[PC_GX_MAX_TEV_STAGES];
        GLint num_ind_stages;
        GLint ind_tex[4], ind_scale[4];
        GLint ind_mtx_r0[PC_GX_MAX_TEV_STAGES], ind_mtx_r1[PC_GX_MAX_TEV_STAGES];
        GLint tev_ind_cfg[PC_GX_MAX_TEV_STAGES], tev_ind_wrap[PC_GX_MAX_TEV_STAGES];
        GLint fog_type, fog_start, fog_end, fog_color;
        GLint tev_bsc[PC_GX_MAX_TEV_STAGES], tev_out[PC_GX_MAX_TEV_STAGES];
        GLint swap_table;
        GLint tev_swap[PC_GX_MAX_TEV_STAGES];
#ifdef TARGET_VITA
        // CPU-resolved TEV inputs
        GLint tev_csrc[3];
        GLint tev_ca[3];
        GLint tev_cb[3];
        GLint tev_cc[3];
        GLint tev_cd[3];
        GLint tev_asrc[3];
        GLint tev_aval[3];
        GLint tev_param[3];
        GLint tev_aparam[3];
#endif
    } uloc;

    float clear_color[4];
    float clear_depth;

    /* Copy/framebuffer */
    int copy_src[4];       /* left, top, w, h */
    int copy_dst[2];       /* w, h */
    int tex_copy_src[4];
    int tex_copy_dst[2];
    unsigned int tex_copy_fmt;
    int tex_copy_mipmap;

    /* Indexed vertex data */
    const void* array_base[PC_GX_MAX_ATTR];
    unsigned char array_stride[PC_GX_MAX_ATTR];

    unsigned int dirty;

#ifdef TARGET_VITA
    int efb_v_flip;
    u32 tev_color_packed_cache[4];
#endif

} PCGXState;

extern PCGXState g_gx;

typedef struct PCGXShaderCacheEntry {
    uint64_t key;
    GLuint program;
} PCGXShaderCacheEntry;

/* --- Internal functions --- */
void pc_gx_init(void);
void pc_gx_shutdown(void);
void pc_gx_flush_vertices(void);
void pc_gx_flush_if_begin_complete(void);
void pc_gx_abort_batch(void);

/* TEV shader */
GLuint pc_gx_tev_get_shader(PCGXState* state);
void   pc_gx_tev_init(void);
void   pc_gx_tev_shutdown(void);

/* Texture cache */
GLuint pc_gx_texture_upload(void* data, int width, int height, int format, int ci_format,
                            void* tlut, int tlut_format, int tlut_count);
void   pc_gx_texture_init(void);
void   pc_gx_texture_shutdown(void);
void   pc_gx_texture_cache_invalidate(void);
void   pc_gx_texture_flush_deferred_deletes(void);
#ifdef TARGET_VITA
void   vita_defer_tex_delete(GLuint tex);
void   pc_gx_flush_draw_batch(void);
void   pc_gx_submit_frame(void);
void   pc_gx_cache_uniform_locations(GLuint shader);
void   pc_gx_texture_process_deferred_uploads(void);
void   pc_gx_texture_process_deferred_params(void);
#endif

#ifdef PC_ENHANCEMENTS
/* EFB capture: store full-res GL texture from GXCopyTex, retrieve on texture load */
void   pc_gx_efb_capture_store(u32 dest_ptr, GLuint gl_tex);
GLuint pc_gx_efb_capture_find(u32 data_ptr);
void   pc_gx_efb_capture_cleanup(void);
#endif

// Normalize light direction vector (shared by PC and Vita paths)
static inline void pc_normalize_light_dir(const float pos[3], float out[3]) {
    float x = pos[0], y = pos[1], z = pos[2];
    float len = sqrtf(x * x + y * y + z * z);
    if (len > 0.0f) {
        float inv = 1.0f / len;
        out[0] = x * inv; out[1] = y * inv; out[2] = z * inv;
    } else {
        out[0] = 0.0f; out[1] = 0.0f; out[2] = 1.0f;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* PC_GX_INTERNAL_H */
