// vita frame orchestration
// double-buffer swap, emu64 dispatch, perf logging
#ifdef TARGET_VITA

#include "pc_platform.h"
#include "pc_gx_internal.h"
#include "vita_shared.h"
#include "vita_gx_cmdbuf.h"
#include "sys_ucode.h"
#include "libforest/emu64/emu64_wrapper.h"
#include <psp2/kernel/processmgr.h>
#include <stdio.h>

// frame state
static int vita_first_frame = 1;
static int vita_worker_pending = 0;

// perf logging (compile-time optional)

static int vita_perf_log_init_done = 0;

#ifdef VITA_DEBUG
static unsigned int vita_submit_us = 0;
static unsigned int perf_frame_count = 0;
static unsigned int perf_acc_submit = 0;
static unsigned int perf_acc_emu64 = 0;
static unsigned int perf_acc_swap = 0;

static void vita_perf_log_frame(void) {
    if (!vita_perf_log_init_done) {
        vita_log("[PERF_INIT] perf logging active\n");
        vita_perf_log_init_done = 1;
    }
    perf_acc_submit += vita_submit_us;
    perf_acc_emu64 += vita_timing.emu64_us;
    perf_acc_swap += vita_timing.swap_us;
    perf_frame_count++;
    if (perf_frame_count >= 120) {
        vita_log("[PERF] submit=%.1fms emu64=%.1fms swap=%.1fms draws=%d drop=%d shswitch=%d spec=%d uber=%d atest=%d texup=%d\n",
                (float)perf_acc_submit / perf_frame_count / 1000.0f,
                (float)perf_acc_emu64 / perf_frame_count / 1000.0f,
                (float)perf_acc_swap / perf_frame_count / 1000.0f,
                vita_stats.merged_draws,
                vita_stats.dropped_draws,
                vita_stats.shader_switches,
                vita_tev_specialized_draws,
                vita_tev_complex_draws,
                vita_tev_alpha_test_draws,
                vita_stats.deferred_tex_uploads);
        if (vita_tev_complex_draws > 0) {
            vita_dump_tev_configs();
        }
        perf_acc_submit = 0;
        perf_acc_emu64 = 0;
        perf_acc_swap = 0;
        perf_frame_count = 0;
    }
}
#endif

// threaded frame path

static void vita_frame_run_threaded(ucode_info* ucode, void* gfx_list) {
    if (!vita_first_frame) {
        if (!vita_gpu_skip_draws) {
            JW_EndFrame();
        }
        vita_gpu_skip_draws = 0;
    }

    cmd_write = 1 - cmd_write;

    JW_BeginFrame();

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
        vita_submit_us = sceKernelGetProcessTimeLow() - st0;
#endif
    }

#ifdef VITA_DEBUG
    vita_perf_log_frame();
#endif

    vita_first_frame = 0;
}

// single-threaded fallback

static void vita_frame_run_single(ucode_info* ucode, void* gfx_list) {
    JW_BeginFrame();
    emu64_init();
    emu64_set_ucode_info(2, ucode);
    emu64_set_first_ucode(ucode[0].ucode_p);
    {
        unsigned int t0 = sceKernelGetProcessTimeLow();
        emu64_taskstart(gfx_list);
        vita_timing.emu64_us = sceKernelGetProcessTimeLow() - t0;
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
    JW_EndFrame();
#ifdef VITA_DEBUG
    vita_perf_log_frame();
#endif
}

// public API

void vita_frame_run(ucode_info* ucode, void* gfx_list) {
    if (vita_emu64_worker_active()) {
        vita_frame_run_threaded(ucode, gfx_list);
    } else {
        vita_frame_run_single(ucode, gfx_list);
    }
}

void vita_frame_wait_worker(void) {
    if (vita_emu64_worker_active() && vita_worker_pending) {
        vita_emu64_wait_done();
        emu64_cleanup();
        vita_worker_pending = 0;
    }
}

#endif // TARGET_VITA
