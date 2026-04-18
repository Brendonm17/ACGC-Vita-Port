// vita_shared.h
// cross-module externs for Vita port
#ifndef VITA_SHARED_H
#define VITA_SHARED_H

#ifdef TARGET_VITA

#include "pc_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned int swap_us;
    unsigned int emu64_us;
    unsigned int flush_us;
    unsigned int texload_us;
    unsigned int tevmatch_us;
    unsigned int flush_vtx_us;
    unsigned int flush_tev_us;
    unsigned int flush_state_us;
    unsigned int flush_state_lighting_us;
    unsigned int flush_state_textures_us;
    unsigned int submit_us;
    unsigned int vgl_frag_tex_us;
    unsigned int vgl_vert_tex_us;
    unsigned int vgl_align_attrs_us;
    unsigned int vgl_patch_vprog_us;
    unsigned int vgl_upload_unif_us;
    unsigned int vgl_vstreams_us;
    unsigned int submit_state_us;
    unsigned int submit_uniform_us;
    unsigned int submit_texbind_us;
    unsigned int submit_draw_us;
    unsigned int submit_efb_us;
    unsigned int submit_glstate_us;
    unsigned int submit_waitpdd_us;
    unsigned int submit_presub_us;
    unsigned int submit_postsub_us;
    unsigned int prededup_us;
    unsigned int emu64_task_us;
    unsigned int frustum_cull_us;
    unsigned int beginframe_us;
    unsigned int endframe_us;
    unsigned int waitworker_us;
    unsigned int gamemain_us;
    unsigned int audio_us;
} VitaFrameTiming;
extern VitaFrameTiming vita_timing;

// command buffer state
extern int cmd_write;
extern int vita_gpu_skip_draws;
extern int vita_current_zmode;
extern int pc_gx_current_queue;

// worker thread
extern volatile int vita_on_worker_thread;
void vita_emu64_signal_work(void* dl);
void vita_emu64_wait_done(void);
int  vita_emu64_worker_active(void);

// TEV shader counters
extern int vita_tev_specialized_draws;
extern int vita_tev_complex_draws;
extern int vita_tev_alpha_test_draws;
extern int vita_tev_is_ocean;
extern int vita_tev_tex_remap;
extern int vita_tev_passthrough;

// performance counters
typedef struct {
    int draw_calls;
    int merged_draws;
    int dropped_draws;
    int shader_switches;
    int deferred_tex_uploads;
    int opaque_draws;
    int blended_draws;
    int sort_merged;
    int culled_draws;
} VitaFrameStats;
extern VitaFrameStats vita_stats;

// deferred texture uploads
extern GLuint vita_deferred_uploaded[];
extern int vita_deferred_uploaded_count;

// aspect correction
extern float g_aspect_factor;
extern float g_aspect_offset;
extern int   g_aspect_active;

// CPU lighting
extern int vita_cpu_lit_active;

void pc_gx_submit_frame(void);
void vita_dump_tev_configs(void);
GLuint vita_get_simple_shader(void);
void vita_log(const char* fmt, ...);

static inline void vita_get_43_layout(int* content_w, int* offset_x) {
    *content_w = (int)((float)g_pc_window_h * 4.0f / 3.0f);
    *offset_x = (g_pc_window_w - *content_w) / 2;
}

#ifdef __cplusplus
}
#endif

#endif // TARGET_VITA
#endif // VITA_SHARED_H
