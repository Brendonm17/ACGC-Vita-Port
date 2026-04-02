// pc_gx_backend.h
// platform dispatch macros for Vita vs PC GL backends
// concentrates platform-conditional logic to minimize upstream diff
#ifndef PC_GX_BACKEND_H
#define PC_GX_BACKEND_H

#ifdef TARGET_VITA

#include "vita_shared.h"
#include "vita_gx_cmdbuf.h"

// GL API shims (VitaGL)
#define PC_CLEAR_DEPTH(d)       glClearDepthf(d)
#define PC_DEPTH_RANGE(n, f)    glDepthRangef((n), (f))
#define PC_DELETE_TEXTURE(tex)  vita_defer_tex_delete(tex)
#define PC_HAS_VAO              0

// VitaGL is single-thread-only for GL calls
#define PC_WORKER_GUARD()       do { if (vita_on_worker_thread) return; } while(0)

// draw stats
#define PC_STAT_DRAW_INC()      (vita_stats.draw_calls++)
#define PC_STAT_SHADER_INC()    (vita_stats.shader_switches++)

// redundancy skip: avoid dirty-flag churn (no-op on PC)
#define GX_SKIP_IF_SAME_1(f1, v1) \
    do { if ((f1) == (int)(v1)) return; } while(0)

#define GX_SKIP_IF_SAME_2(f1, v1, f2, v2) \
    do { if ((f1) == (int)(v1) && (f2) == (int)(v2)) return; } while(0)

#define GX_SKIP_IF_SAME_3(f1, v1, f2, v2, f3, v3) \
    do { if ((f1) == (int)(v1) && (f2) == (int)(v2) && (f3) == (int)(v3)) return; } while(0)

#define GX_SKIP_IF_SAME_4(f1, v1, f2, v2, f3, v3, f4, v4) \
    do { if ((f1) == (int)(v1) && (f2) == (int)(v2) && \
             (f3) == (int)(v3) && (f4) == (int)(v4)) return; } while(0)

#define GX_SKIP_IF_SAME_5(f1, v1, f2, v2, f3, v3, f4, v4, f5, v5) \
    do { if ((f1) == (int)(v1) && (f2) == (int)(v2) && (f3) == (int)(v3) && \
             (f4) == (int)(v4) && (f5) == (int)(v5)) return; } while(0)

// Vita TEV pre-resolution: mark TEV_STAGES dirty when colors/konst change
#define PC_DIRTY_TEV_IF_VITA()  DIRTY(PC_GX_DIRTY_TEV_STAGES)

void vita_normalize_light(int slot);

#else // PC

// GL API (desktop GL 3.3)
#define PC_CLEAR_DEPTH(d)       glClearDepth(d)
#define PC_DEPTH_RANGE(n, f)    glDepthRange((double)(n), (double)(f))
#define PC_DELETE_TEXTURE(tex)  do { GLuint t_ = (tex); glDeleteTextures(1, &t_); } while(0)
#define PC_HAS_VAO              1

#define PC_WORKER_GUARD()       ((void)0)

extern int pc_gx_draw_call_count;
extern int pc_gx_shader_switch_count;
#define PC_STAT_DRAW_INC()      (pc_gx_draw_call_count++)
#define PC_STAT_SHADER_INC()    (pc_gx_shader_switch_count++)

// redundancy skip: no-op on PC
#define GX_SKIP_IF_SAME_1(f1, v1)                                      ((void)0)
#define GX_SKIP_IF_SAME_2(f1, v1, f2, v2)                              ((void)0)
#define GX_SKIP_IF_SAME_3(f1, v1, f2, v2, f3, v3)                      ((void)0)
#define GX_SKIP_IF_SAME_4(f1, v1, f2, v2, f3, v3, f4, v4)              ((void)0)
#define GX_SKIP_IF_SAME_5(f1, v1, f2, v2, f3, v3, f4, v4, f5, v5)      ((void)0)

#define PC_DIRTY_TEV_IF_VITA()  ((void)0)

#endif // TARGET_VITA

#endif // PC_GX_BACKEND_H
