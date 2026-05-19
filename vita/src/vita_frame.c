// vita_frame.c
// double buffer swap, emu64 dispatch, perf logging
#ifdef TARGET_VITA

#include "pc_platform.h"
#include "pc_gx_internal.h"
#include "vita_shared.h"
#include "vita_gx_cmdbuf.h"
#include "sys_ucode.h"
#include "libforest/emu64/emu64_wrapper.h"
#include <psp2/kernel/processmgr.h>
#include <stdio.h>

static int vita_first_frame = 1;
static int vita_worker_pending = 0;

// single mode swaps at the bottom; the next threaded frame's top swap
// would undo it. armed when leaving the dual-write window
static int vita_skip_next_top_swap = 0;

static int vita_perf_log_init_done = 0;

#ifdef VITA_DEBUG
static unsigned int vita_submit_us = 0;
static unsigned int perf_frame_count = 0;
static unsigned int perf_acc_submit = 0;
static unsigned int perf_acc_emu64 = 0;
static unsigned int perf_acc_swap = 0;
static unsigned int perf_acc_vgl_frag_tex = 0;
static unsigned int perf_acc_vgl_vert_tex = 0;
static unsigned int perf_acc_vgl_align = 0;
static unsigned int perf_acc_vgl_patch = 0;
static unsigned int perf_acc_vgl_unif = 0;
static unsigned int perf_acc_vgl_vstreams = 0;
static unsigned int perf_acc_submit_state = 0;
static unsigned int perf_acc_submit_draw = 0;
static unsigned int perf_acc_submit_efb = 0;
static unsigned int perf_acc_submit_uniform = 0;
static unsigned int perf_acc_submit_glstate = 0;
static unsigned int perf_acc_submit_waitpdd = 0;
static unsigned int perf_acc_submit_presub = 0;
static unsigned int perf_acc_submit_postsub = 0;
static unsigned int perf_acc_prededup = 0;
static unsigned int perf_acc_emu64_task = 0;
static unsigned int perf_acc_frustum_cull = 0;
static unsigned int perf_acc_flush_vtx = 0;
static unsigned int perf_acc_flush_tev = 0;
static unsigned int perf_acc_flush_state = 0;
static unsigned int perf_acc_flush_lighting = 0;
static unsigned int perf_acc_flush_textures = 0;
static unsigned int perf_acc_endframe = 0;
static unsigned int perf_acc_waitworker = 0;
static unsigned int perf_acc_gamemain = 0;
static unsigned int perf_acc_beginframe = 0;
static unsigned int perf_acc_audio = 0;

static void vita_perf_log_frame(void) {
    if (!vita_perf_log_init_done) {
        vita_log("[PERF_INIT] perf logging active\n");
        vita_perf_log_init_done = 1;
    }
    perf_acc_submit += vita_submit_us;
    perf_acc_emu64 += vita_timing.emu64_us;
    perf_acc_swap += vita_timing.swap_us;
    perf_acc_vgl_frag_tex += vita_timing.vgl_frag_tex_us;
    perf_acc_vgl_vert_tex += vita_timing.vgl_vert_tex_us;
    perf_acc_vgl_align    += vita_timing.vgl_align_attrs_us;
    perf_acc_vgl_patch    += vita_timing.vgl_patch_vprog_us;
    perf_acc_vgl_unif     += vita_timing.vgl_upload_unif_us;
    perf_acc_vgl_vstreams += vita_timing.vgl_vstreams_us;
    perf_acc_submit_state += vita_timing.submit_state_us;
    perf_acc_submit_draw  += vita_timing.submit_draw_us;
    perf_acc_submit_efb   += vita_timing.submit_efb_us;
    perf_acc_submit_uniform += vita_timing.submit_uniform_us;
    perf_acc_submit_glstate += vita_timing.submit_glstate_us;
    perf_acc_submit_waitpdd += vita_timing.submit_waitpdd_us;
    perf_acc_submit_presub  += vita_timing.submit_presub_us;
    perf_acc_submit_postsub += vita_timing.submit_postsub_us;
    perf_acc_prededup       += vita_timing.prededup_us;
    perf_acc_emu64_task     += vita_timing.emu64_task_us;
    perf_acc_frustum_cull   += vita_timing.frustum_cull_us;
    perf_acc_flush_vtx    += vita_timing.flush_vtx_us;
    perf_acc_flush_tev    += vita_timing.flush_tev_us;
    perf_acc_flush_state  += vita_timing.flush_state_us;
    perf_acc_flush_lighting += vita_timing.flush_state_lighting_us;
    perf_acc_flush_textures += vita_timing.flush_state_textures_us;
    perf_acc_endframe     += vita_timing.endframe_us;
    perf_acc_waitworker   += vita_timing.waitworker_us;
    perf_acc_gamemain     += vita_timing.gamemain_us;
    perf_acc_beginframe   += vita_timing.beginframe_us;
    perf_acc_audio        += vita_timing.audio_us;
    perf_frame_count++;
    if (perf_frame_count >= 120) {
        vita_log("[PERF] submit=%.1fms emu64=%.1fms swap=%.1fms begin=%.1fms flush=%.1fms tev=%.1fms draws=%d drop=%d cull=%d shswitch=%d spec=%d uber=%d atest=%d texup=%d opq=%d bld=%d smerge=%d\n",
                (float)perf_acc_submit / perf_frame_count / 1000.0f,
                (float)perf_acc_emu64 / perf_frame_count / 1000.0f,
                (float)perf_acc_swap / perf_frame_count / 1000.0f,
                (float)vita_timing.beginframe_us / 1000.0f,
                (float)vita_timing.flush_us / 1000.0f,
                (float)vita_timing.tevmatch_us / 1000.0f,
                vita_stats.merged_draws,
                vita_stats.dropped_draws,
                vita_stats.culled_draws,
                vita_stats.shader_switches,
                vita_tev_specialized_draws,
                vita_tev_complex_draws,
                vita_tev_alpha_test_draws,
                vita_stats.deferred_tex_uploads,
                vita_stats.opaque_draws,
                vita_stats.blended_draws,
                vita_stats.sort_merged);
        vita_log("[PERF_VGL] ftex=%.2fms vtex=%.2fms align=%.2fms patch=%.2fms unif=%.2fms vstream=%.2fms\n",
                (float)perf_acc_vgl_frag_tex / perf_frame_count / 1000.0f,
                (float)perf_acc_vgl_vert_tex / perf_frame_count / 1000.0f,
                (float)perf_acc_vgl_align    / perf_frame_count / 1000.0f,
                (float)perf_acc_vgl_patch    / perf_frame_count / 1000.0f,
                (float)perf_acc_vgl_unif     / perf_frame_count / 1000.0f,
                (float)perf_acc_vgl_vstreams / perf_frame_count / 1000.0f);
        vita_log("[PERF_SUB] state=%.2fms draw=%.2fms efb=%.2fms\n",
                (float)perf_acc_submit_state / perf_frame_count / 1000.0f,
                (float)perf_acc_submit_draw  / perf_frame_count / 1000.0f,
                (float)perf_acc_submit_efb   / perf_frame_count / 1000.0f);
        {
            unsigned int swp = perf_acc_submit_waitpdd / perf_frame_count;
            unsigned int spr = perf_acc_submit_presub / perf_frame_count;
            unsigned int spo = perf_acc_submit_postsub / perf_frame_count;
            unsigned int stot_out = swp + spr + spo;
            unsigned int sub_tot = perf_acc_submit / perf_frame_count;
            unsigned int sper_tot = (perf_acc_submit_state + perf_acc_submit_draw +
                                     perf_acc_submit_efb) / perf_frame_count;
            unsigned int gap = (sub_tot > stot_out + sper_tot) ? (sub_tot - stot_out - sper_tot) : 0;
            unsigned int pddwork = perf_acc_prededup / perf_frame_count;
            vita_log("[PERF_SUB_OUT] waitpdd=%.2fms presub=%.2fms postsub=%.2fms gap=%.2fms pddwork=%.2fms\n",
                    (float)swp / 1000.0f, (float)spr / 1000.0f,
                    (float)spo / 1000.0f, (float)gap / 1000.0f,
                    (float)pddwork / 1000.0f);
        }
        {
            unsigned int sun = perf_acc_submit_uniform / perf_frame_count;
            unsigned int sgs = perf_acc_submit_glstate / perf_frame_count;
            unsigned int stot = perf_acc_submit_state / perf_frame_count;
            unsigned int sother = (stot > sun + sgs) ? (stot - sun - sgs) : 0;
            vita_log("[PERF_STATE_SPLIT] unif=%.2fms glstate=%.2fms other=%.2fms\n",
                    (float)sun / 1000.0f, (float)sgs / 1000.0f, (float)sother / 1000.0f);
        }
        vita_log("[PERF_FLUSH] vtx=%.2fms tev=%.2fms state=%.2fms\n",
                (float)perf_acc_flush_vtx   / perf_frame_count / 1000.0f,
                (float)perf_acc_flush_tev   / perf_frame_count / 1000.0f,
                (float)perf_acc_flush_state / perf_frame_count / 1000.0f);
        // Core 1 worker split. task = N64 DL interpreter + cmd buffer
        // build. cull = frustum cull pass. emu64 = sum of both.
        vita_log("[PERF_WORKER] emu64_task=%.2fms frustum_cull=%.2fms\n",
                (float)perf_acc_emu64_task / perf_frame_count / 1000.0f,
                (float)perf_acc_frustum_cull / perf_frame_count / 1000.0f);
        // full main-thread timeline. sum these + swap + any other gap
        // should equal frame time. anything unaccounted is stalled/idle.
        vita_log("[PERF_MAIN] endF=%.2fms waitW=%.2fms game=%.2fms beginF=%.2fms submit=%.2fms audio=%.2fms\n",
                (float)perf_acc_endframe   / perf_frame_count / 1000.0f,
                (float)perf_acc_waitworker / perf_frame_count / 1000.0f,
                (float)perf_acc_gamemain   / perf_frame_count / 1000.0f,
                (float)perf_acc_beginframe / perf_frame_count / 1000.0f,
                (float)perf_acc_submit     / perf_frame_count / 1000.0f,
                (float)perf_acc_audio      / perf_frame_count / 1000.0f);
        if (vita_tev_complex_draws > 0) {
            vita_dump_tev_configs();
        }
        perf_acc_submit = 0;
        perf_acc_emu64 = 0;
        perf_acc_swap = 0;
        perf_acc_vgl_frag_tex = 0;
        perf_acc_vgl_vert_tex = 0;
        perf_acc_vgl_align = 0;
        perf_acc_vgl_patch = 0;
        perf_acc_vgl_unif = 0;
        perf_acc_vgl_vstreams = 0;
        perf_acc_submit_state = 0;
        perf_acc_submit_draw = 0;
        perf_acc_submit_efb = 0;
        perf_acc_submit_uniform = 0;
        perf_acc_submit_glstate = 0;
        perf_acc_submit_waitpdd = 0;
        perf_acc_submit_presub = 0;
        perf_acc_submit_postsub = 0;
        perf_acc_prededup = 0;
        perf_acc_emu64_task = 0;
        perf_acc_frustum_cull = 0;
        perf_acc_flush_vtx = 0;
        perf_acc_flush_tev = 0;
        perf_acc_flush_state = 0;
        perf_acc_flush_lighting = 0;
        perf_acc_flush_textures = 0;
        perf_acc_endframe = 0;
        perf_acc_waitworker = 0;
        perf_acc_gamemain = 0;
        perf_acc_beginframe = 0;
        perf_acc_audio = 0;
        perf_frame_count = 0;
    }
}
#endif

static void vita_frame_run_threaded(ucode_info* ucode, void* gfx_list) {
    if (!vita_first_frame) {
        int skip_top = vita_skip_next_top_swap;
        vita_skip_next_top_swap = 0;
        if (!vita_gpu_skip_draws && !skip_top) {
#ifdef VITA_DEBUG
            unsigned int ef0 = sceKernelGetProcessTimeLow();
#endif
            JW_EndFrame();
#ifdef VITA_DEBUG
            vita_timing.endframe_us = sceKernelGetProcessTimeLow() - ef0;
#endif
        }
        vita_gpu_skip_draws = 0;
    }

    cmd_write = 1 - cmd_write;

    {
#ifdef VITA_DEBUG
        unsigned int bf0 = sceKernelGetProcessTimeLow();
#endif
        JW_BeginFrame();
#ifdef VITA_DEBUG
        vita_timing.beginframe_us = sceKernelGetProcessTimeLow() - bf0;
#endif
    }

    emu64_init();
    emu64_set_ucode_info(2, ucode);
    emu64_set_first_ucode(ucode[0].ucode_p);

    vita_emu64_signal_work(gfx_list);
    vita_worker_pending = 1;

    if (!vita_first_frame) {
#ifdef VITA_DEBUG
        unsigned int st0 = sceKernelGetProcessTimeLow();
#endif
        pc_gx_submit_frame();
#ifdef VITA_DEBUG
        vita_timing.submit_us = sceKernelGetProcessTimeLow() - st0;
        vita_submit_us = vita_timing.submit_us;
#endif
    }
#ifdef VITA_DEBUG
    else {
        vita_timing.submit_us = 0;
    }
#endif

#ifdef VITA_DEBUG
    vita_perf_log_frame();
#endif

    vita_first_frame = 0;
}

static void vita_frame_run_single(ucode_info* ucode, void* gfx_list) {
    JW_BeginFrame();
    emu64_init();
    emu64_set_ucode_info(2, ucode);
    emu64_set_first_ucode(ucode[0].ucode_p);
    {
#ifdef VITA_DEBUG
        unsigned int t0 = sceKernelGetProcessTimeLow();
#endif
        emu64_taskstart(gfx_list);
        // TEV resolve is inlined into prededup's per-cmd loop. Single-
        // threaded fallback has to do all core 2 work inline here.
        extern volatile int pdd_buffer_idx;
        pdd_buffer_idx = cmd_write;
        vita_cmdbuf_prededup();
        vita_cmdbuf_frustum_cull();
#ifdef VITA_DEBUG
        vita_timing.emu64_us = sceKernelGetProcessTimeLow() - t0;
#endif
    }
    // swap so submit reads what we just wrote
    cmd_write = 1 - cmd_write;
#ifdef VITA_DEBUG
    {
        unsigned int st0 = sceKernelGetProcessTimeLow();
#endif
    pc_gx_submit_frame();
#ifdef VITA_DEBUG
        vita_submit_us = sceKernelGetProcessTimeLow() - st0;
    }
#endif
    cmd_write = 1 - cmd_write;
    emu64_cleanup();
    // vglSwapBuffers can queue an in-flight FBO for display before the
    // GPU finishes writing it; flash during the title->intro fade
    glFinish();
    // scene-break frames render with a partially-populated matrix pool
    // (invalidate fires mid-game_main, earlier draws this frame already
    // captured stale identity matrices). the iris transition wants
    // black here anyway, so wipe the back buffer before swap. gated on
    // active dual-write so the clear can't fire outside transitions.
    extern int g_pc_gx_skip_display_frames;
    extern int g_pc_gx_dual_write_frames;
    if (g_pc_gx_skip_display_frames > 0 && g_pc_gx_dual_write_frames > 0) {
        glDepthMask(GL_TRUE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        g_pc_gx_skip_display_frames--;
    }
    JW_EndFrame();
#ifdef VITA_DEBUG
    vita_perf_log_frame();
#endif
}

void vita_frame_run(ucode_info* ucode, void* gfx_list) {
    int worker_active = vita_emu64_worker_active();
    extern int g_pc_gx_dual_write_frames;
    int mode = (worker_active && g_pc_gx_dual_write_frames > 0) ? 2 :
               worker_active ? 1 : 0;
    // dual-write: drain the worker and run single-mode inline so submit
    // reads the cmd queue we just wrote (no 2-frame display lag during
    // wipe/fade/menu transitions)
    if (mode == 2) {
        g_pc_gx_dual_write_frames--;
        if (vita_worker_pending) {
            vita_emu64_wait_done();
            emu64_cleanup();
            vita_worker_pending = 0;
        }
        // worker dispatches prededup before signaling done, so the core 2
        // thread can still be running when we reach here. without this
        // wait, single mode's inline prededup races it on shared state
        extern void vita_wait_prededup(void);
        vita_wait_prededup();
        // entry seam fence: belt-and-suspenders for ARM weak ordering
        // over sceKernelWaitSema's implied acquire on g_gx writes.
        __sync_synchronize();
        vita_frame_run_single(ucode, gfx_list);
        if (g_pc_gx_dual_write_frames == 0) {
            vita_skip_next_top_swap = 1;
            // exit seam fence: pairs with the next worker dispatch's
            // sceKernelSignalSema release.
            __sync_synchronize();
            // reset so the counter can't leak into a later transition.
            extern int g_pc_gx_skip_display_frames;
            g_pc_gx_skip_display_frames = 0;
        }
        return;
    }
    if (mode == 1) {
        vita_frame_run_threaded(ucode, gfx_list);
    } else {
        vita_frame_run_single(ucode, gfx_list);
    }
}

void vita_frame_wait_worker(void) {
    if (vita_emu64_worker_active() && vita_worker_pending) {
#ifdef VITA_DEBUG
        unsigned int ww0 = sceKernelGetProcessTimeLow();
#endif
        vita_emu64_wait_done();
#ifdef VITA_DEBUG
        vita_timing.waitworker_us = sceKernelGetProcessTimeLow() - ww0;
#endif
        emu64_cleanup();
        vita_worker_pending = 0;
        extern void vita_vtc_prefetch(void);
        vita_vtc_prefetch();
    } else {
#ifdef VITA_DEBUG
        vita_timing.waitworker_us = 0;
#endif
    }
}

#endif // TARGET_VITA
