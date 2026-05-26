// vita_platform.c - PS Vita platform init (VitaGL)
#ifdef TARGET_VITA

#include "pc_platform.h"
#include "pc_gx_internal.h"
#include "pc_settings.h"
#include "vita_shared.h"
#include "vita_gx_cmdbuf.h"

#include <psp2/power.h>
#include <psp2/appmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/message_dialog.h>
#include <psp2/common_dialog.h>
#include <vitaGL.h>
#include <string.h>
#include <time.h>
#include <dolphin/os.h>

// vita SDK "directory already exists"
#define VITA_EEXIST ((int)0x80010011)

// 120MB heap, 4MB stack (1MB caused corruption)
__attribute__((used)) unsigned int _newlib_heap_size_user = 120 * 1024 * 1024;
__attribute__((used)) unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024;

SDL_Window*   g_pc_window = NULL;     // unused on vita
SDL_GLContext  g_pc_gl_context = NULL; // unused on vita
int           g_pc_running = 1;
int           g_pc_no_framelimit = 0;
int           g_pc_verbose = 0;
int           g_pc_time_override = -1;
int           g_pc_window_w = 960;
int           g_pc_window_h = 544;
int           g_pc_widescreen_stretch = 0;

// BSS extends to ~0x82806000, past 0x82000000
// wrong bounds broke seg2k0 crash protection
unsigned int pc_image_base = 0x81000000;
unsigned int pc_image_end  = 0x82900000;

int g_pc_model_viewer = 0;
int g_pc_model_viewer_start = 0;
int g_pc_model_viewer_no_cull = 0;

// referenced by pc_os.c
int g_pc_min_override = -1;
int g_pc_sec_override = -1;

// crash protection
// no si_addr on vita, just longjmp recovery
static jmp_buf* pc_active_jmpbuf = NULL;
static volatile unsigned int pc_last_crash_addr = 0;
static volatile unsigned int pc_last_crash_data_addr = 0;

// scePower callback is the only pre-kill signal that actually fires.
// thread parks in sceKernelDelayThreadCB so the callback can dispatch.
static SceUID s_power_cb_uid    = -1;
static SceUID s_power_cb_thread = -1;
static volatile int s_power_cb_running = 1;

// APP_SUSPEND alone misses the PS-button path; PS_PRESS fires there.
#define PC_POWER_SAVE_MASK ( \
    SCE_POWER_CB_APP_SUSPEND        | \
    SCE_POWER_CB_BUTTON_PS_PRESS    | \
    SCE_POWER_CB_BUTTON_POWER_PRESS | \
    SCE_POWER_CB_BUTTON_POWER_HOLD)

static int vita_power_callback(int notifyId, int notifyCount, int powerInfo, void *userData) {
    (void)notifyId; (void)notifyCount; (void)userData;
    if (powerInfo & PC_POWER_SAVE_MASK) {
        extern int pc_auto_save_force(void);
        if (g_pc_settings.auto_save) {
            pc_auto_save_force();
        }
    }
    return 0;
}

static int vita_power_cb_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    s_power_cb_uid = sceKernelCreateCallback("ac_pwr_cb", 0, vita_power_callback, NULL);
    if (s_power_cb_uid < 0) return 0;
    scePowerRegisterCallback(s_power_cb_uid);
    // park here so SCE callbacks dispatch on us
    while (s_power_cb_running) {
        sceKernelDelayThreadCB(10 * 1000 * 1000);
    }
    if (s_power_cb_uid >= 0) {
        scePowerUnregisterCallback(s_power_cb_uid);
        sceKernelDeleteCallback(s_power_cb_uid);
        s_power_cb_uid = -1;
    }
    return 0;
}

static void vita_power_callback_init(void) {
    s_power_cb_thread = sceKernelCreateThread(
        "ac_pwr_cb_thr",
        vita_power_cb_thread,
        0x10000100,                 // just below main
        16 * 1024,                  // small stack, thread just sleeps
        0,
        SCE_KERNEL_CPU_MASK_USER_2, // keep off the render core
        NULL);
    if (s_power_cb_thread >= 0) {
        sceKernelStartThread(s_power_cb_thread, 0, NULL);
    }
}

static void vita_signal_handler(int sig) {
    (void)sig;
    if (pc_active_jmpbuf != NULL) {
        pc_last_crash_addr = 0;
        pc_last_crash_data_addr = 0;
        jmp_buf* buf = pc_active_jmpbuf;
        pc_active_jmpbuf = NULL;
        longjmp(*buf, 1);
    }
    // no jmpbuf set, let it crash
}

unsigned int pc_crash_get_data_addr(void) {
    return pc_last_crash_data_addr;
}

void pc_crash_protection_init(void) {
    static int installed = 0;
    if (!installed) {
        if (signal(SIGSEGV, vita_signal_handler) == SIG_ERR)
            fprintf(stderr, "[VITA] WARNING: Failed to install SIGSEGV handler\n");
        if (signal(SIGILL, vita_signal_handler) == SIG_ERR)
            fprintf(stderr, "[VITA] WARNING: Failed to install SIGILL handler\n");
        if (signal(SIGFPE, vita_signal_handler) == SIG_ERR)
            fprintf(stderr, "[VITA] WARNING: Failed to install SIGFPE handler\n");
        installed = 1;
    }
}

void pc_crash_set_jmpbuf(jmp_buf* buf) {
    pc_active_jmpbuf = buf;
}

unsigned int pc_crash_get_addr(void) {
    return pc_last_crash_addr;
}

// vita_init runs before vitaGL, so it can't draw the dialog yet. record the
// message; main() surfaces it via the dialog once pc_platform_init brings
// vitaGL up.
static char g_early_fatal_msg[256];
static int  g_early_fatal_set = 0;

static void vita_record_early_fatal(const char* msg) {
    strncpy(g_early_fatal_msg, msg, sizeof(g_early_fatal_msg) - 1);
    g_early_fatal_msg[sizeof(g_early_fatal_msg) - 1] = '\0';
    g_early_fatal_set = 1;
}

const char* vita_get_early_fatal(void) {
    return g_early_fatal_set ? g_early_fatal_msg : NULL;
}

// caller must have vitaGL initialized; the system overlay composites on top
// of whatever the app last drew, so we swap a black frame each loop.
void vita_fatal_dialog_and_exit(const char* msg) {
    fprintf(stderr, "[VITA] FATAL: %s\n", msg ? msg : "(no message)");
    fflush(stderr);

    if (!msg) sceKernelExitProcess(1);

    SceMsgDialogParam param;
    sceMsgDialogParamInit(&param);
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;

    SceMsgDialogUserMessageParam userMsg;
    memset(&userMsg, 0, sizeof(userMsg));
    userMsg.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    userMsg.msg = (const SceChar8*)msg;
    param.userMsgParam = &userMsg;

    if (sceMsgDialogInit(&param) < 0) {
        sceKernelExitProcess(1);
    }

    while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        vglSwapBuffers(GL_TRUE);
    }

    sceMsgDialogTerm();
    sceKernelExitProcess(1);
}

// mkdir that tolerates "already exists" regardless of the exact code the
// firmware returns: on failure, accept it when the path is already a directory.
// returns 0 on success, the negative mkdir error otherwise.
static int vita_ensure_dir(const char* path) {
    int r = sceIoMkdir(path, 0777);
    if (r >= 0 || r == VITA_EEXIST) return 0;
    SceIoStat st;
    if (sceIoGetstat(path, &st) >= 0 && SCE_S_ISDIR(st.st_mode)) return 0;
    return r;
}

void vita_init(void) {
    int ret;
    ret = scePowerSetArmClockFrequency(444);
    if (ret < 0) fprintf(stderr, "[VITA] WARNING: ARM overclock failed: 0x%08X\n", ret);
    ret = scePowerSetGpuClockFrequency(222);
    if (ret < 0) fprintf(stderr, "[VITA] WARNING: GPU overclock failed: 0x%08X\n", ret);
    ret = scePowerSetBusClockFrequency(222);
    if (ret < 0) fprintf(stderr, "[VITA] WARNING: Bus overclock failed: 0x%08X\n", ret);

    {
        int err = vita_ensure_dir("ux0:data/AnimalCrossing");
        if (err < 0) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "Could not create the data folder.\n\n"
                "ux0:data/AnimalCrossing\nError 0x%08X\n\n"
                "Your memory card may be read-only or corrupted. Try a filesystem\n"
                "check (chkdsk/fsck) from a PC, or a different card.", (unsigned)err);
            fprintf(stderr, "[VITA] FATAL: mkdir data dir: 0x%08X\n", (unsigned)err);
            vita_record_early_fatal(buf);
            return;
        }
    }
    {
        int err = vita_ensure_dir("ux0:data/AnimalCrossing/saves");
        if (err < 0) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "Could not create the saves folder.\n\n"
                "ux0:data/AnimalCrossing/saves\nError 0x%08X\n\n"
                "Your memory card may be read-only or corrupted. Try a filesystem\n"
                "check (chkdsk/fsck) from a PC, or a different card.", (unsigned)err);
            fprintf(stderr, "[VITA] FATAL: mkdir saves dir: 0x%08X\n", (unsigned)err);
            vita_record_early_fatal(buf);
            return;
        }
    }
    {
        int err = vita_ensure_dir("ux0:data/AnimalCrossing/rom");
        if (err < 0) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "Could not create the rom folder.\n\n"
                "ux0:data/AnimalCrossing/rom\nError 0x%08X\n\n"
                "Your memory card may be read-only or corrupted. Try a filesystem\n"
                "check (chkdsk/fsck) from a PC, or a different card.", (unsigned)err);
            fprintf(stderr, "[VITA] FATAL: mkdir rom dir: 0x%08X\n", (unsigned)err);
            vita_record_early_fatal(buf);
            return;
        }
    }
    {
        int mk_ret4 = sceIoMkdir("ux0:data/AnimalCrossing/banners", 0777);
        if (mk_ret4 < 0 && mk_ret4 != VITA_EEXIST)
            printf("[VITA] WARNING: banners dir failed: 0x%08X\n", mk_ret4);
    }
#ifdef PC_DESIGN_IMPORT
    sceIoMkdir("ux0:data/AnimalCrossing/designs", 0777);
#endif
    // clean up old texture_pack dir (renamed to texture_packs)
    sceIoRmdir("ux0:data/AnimalCrossing/texture_pack");
    {
        int mk_ret6 = sceIoMkdir("ux0:data/AnimalCrossing/texture_packs", 0777);
        if (mk_ret6 < 0 && mk_ret6 != VITA_EEXIST)
            printf("[VITA] WARNING: texture_packs dir failed: 0x%08X\n", mk_ret6);
    }

    // truncate each launch; OSReport in pc_os.c opens the same file in
    // append mode so everything from this session accumulates cleanly.
    if (!freopen("ux0:data/AnimalCrossing/error.log", "w", stderr))
        stderr = stdout;  // fallback to psp2link
    setvbuf(stderr, NULL, _IONBF, 0);

    // after stderr is redirected so any failures inside the callback init
    // land in error.log
    vita_power_callback_init();

    printf("[VITA] Main thread stack: %u KB\n", sceUserMainThreadStackSize / 1024);
}

static SceUID emu64_work_ready_sema = -1;
static SceUID emu64_work_done_sema = -1;
static SceUID emu64_worker_tid = -1;
static volatile int emu64_worker_running = 0;

static volatile void* emu64_work_dl = NULL;

extern void emu64_taskstart(void* dl);

// skip GL calls on worker thread (VitaGL not thread-safe)
volatile int vita_on_worker_thread = 0;

// core 2 prededup thread state.
static SceUID prededup_start_sema = -1;
static SceUID prededup_done_sema = -1;
static SceUID prededup_thread_tid = -1;
static volatile int prededup_thread_running = 0;
static volatile int prededup_pending = 0;
// captured by the worker before signaling core 2; main may flip
// cmd_write before core 2 wakes.
volatile int pdd_buffer_idx = 0;

static int prededup_thread_func(SceSize args, void* argp) {
    (void)args; (void)argp;
    while (prededup_thread_running) {
        sceKernelWaitSema(prededup_start_sema, 1, NULL);
        if (!prededup_thread_running) break;
#ifdef VITA_DEBUG
        unsigned int _pdd_t0 = sceKernelGetProcessTimeLow();
#endif
        vita_cmdbuf_prededup();
#ifdef VITA_DEBUG
        vita_timing.prededup_us = sceKernelGetProcessTimeLow() - _pdd_t0;
#endif
        sceKernelSignalSema(prededup_done_sema, 1);
    }
    return 0;
}

static int emu64_worker_func(SceSize args, void* argp) {
    (void)args; (void)argp;

    while (emu64_worker_running) {
        sceKernelWaitSema(emu64_work_ready_sema, 1, NULL);
        if (!emu64_worker_running) break;

        void* dl = (void*)emu64_work_dl;
        if (dl) {
            vita_on_worker_thread = 1;
#ifdef VITA_DEBUG
            unsigned int t0 = sceKernelGetProcessTimeLow();
#endif
            emu64_taskstart(dl);
#ifdef VITA_DEBUG
            unsigned int t_after_emu64 = sceKernelGetProcessTimeLow();
#endif
            vita_cmdbuf_frustum_cull();
            // cpu xform pass tried here: net loss, so skipped.
#ifdef VITA_DEBUG
            unsigned int t_end = sceKernelGetProcessTimeLow();
            vita_timing.emu64_us = t_end - t0;
            vita_timing.frustum_cull_us = t_end - t_after_emu64;
            vita_timing.emu64_task_us = t_after_emu64 - t0;
#endif
            // dispatch prededup to core 2 (runs in parallel with
            // game logic on core 0 while core 1 signals done).
            if (prededup_thread_running) {
                pdd_buffer_idx = cmd_write;
                prededup_pending = 1;
                sceKernelSignalSema(prededup_start_sema, 1);
            } else {
                vita_cmdbuf_prededup();
            }
            vita_on_worker_thread = 0;
            {
                extern unsigned int vita_texload_us;
                vita_timing.texload_us = vita_texload_us;
                vita_texload_us = 0;
            }
        }

        sceKernelSignalSema(emu64_work_done_sema, 1);
        // prefetch runs on main thread after wait_worker returns. running
        // it here caused the worker to miss wake ups when main signaled
        // the next frame mid prefetch
    }

    return 0;
}

void vita_emu64_worker_init(void) {
    emu64_work_ready_sema = sceKernelCreateSema("emu64_ready", 0, 0, 1, NULL);
    emu64_work_done_sema = sceKernelCreateSema("emu64_done", 0, 0, 1, NULL);

    if (emu64_work_ready_sema < 0 || emu64_work_done_sema < 0) {
        fprintf(stderr, "[VITA] Failed to create emu64 semaphores: ready=0x%08X done=0x%08X\n",
                emu64_work_ready_sema, emu64_work_done_sema);
        if (emu64_work_ready_sema >= 0) sceKernelDeleteSema(emu64_work_ready_sema);
        if (emu64_work_done_sema >= 0) sceKernelDeleteSema(emu64_work_done_sema);
        emu64_work_ready_sema = -1;
        emu64_work_done_sema = -1;
        fprintf(stderr, "[VITA] Falling back to single-threaded.\n");
        return;
    }

    emu64_worker_running = 1;

    // core 1, slightly below main thread priority, 512KB stack for deep call chains
#define EMU64_WORKER_PRIORITY   0x10000100
#define EMU64_WORKER_STACK_SIZE (512 * 1024)
    emu64_worker_tid = sceKernelCreateThread(
        "emu64_worker",
        emu64_worker_func,
        EMU64_WORKER_PRIORITY,
        EMU64_WORKER_STACK_SIZE,
        0,
        SCE_KERNEL_CPU_MASK_USER_1,
        NULL
    );

    if (emu64_worker_tid < 0) {
        fprintf(stderr, "[VITA] Failed to create emu64 worker thread: 0x%08X\n", emu64_worker_tid);
        emu64_worker_running = 0;
        sceKernelDeleteSema(emu64_work_ready_sema);
        sceKernelDeleteSema(emu64_work_done_sema);
        emu64_work_ready_sema = -1;
        emu64_work_done_sema = -1;
        return;
    }

    {
        int start_ret = sceKernelStartThread(emu64_worker_tid, 0, NULL);
        if (start_ret < 0) {
            fprintf(stderr, "[VITA] Failed to start emu64 worker thread: 0x%08X\n", start_ret);
            emu64_worker_running = 0;
            sceKernelDeleteThread(emu64_worker_tid);
            sceKernelDeleteSema(emu64_work_ready_sema);
            sceKernelDeleteSema(emu64_work_done_sema);
            emu64_worker_tid = -1;
            emu64_work_ready_sema = -1;
            emu64_work_done_sema = -1;
            return;
        }
    }
    printf("[VITA] emu64 worker thread started on core 1 (tid=0x%08X)\n", emu64_worker_tid);

    // core 2 prededup thread: lightweight, runs prededup in parallel
    // with game logic so it's off the emu64 critical path.
    prededup_start_sema = sceKernelCreateSema("pdd_start", 0, 0, 1, NULL);
    prededup_done_sema = sceKernelCreateSema("pdd_done", 0, 0, 1, NULL);
    if (prededup_start_sema >= 0 && prededup_done_sema >= 0) {
        prededup_thread_running = 1;
        prededup_thread_tid = sceKernelCreateThread(
            "prededup", prededup_thread_func,
            0x10000100, 32 * 1024, 0,
            SCE_KERNEL_CPU_MASK_USER_2, NULL);
        if (prededup_thread_tid >= 0) {
            sceKernelStartThread(prededup_thread_tid, 0, NULL);
            printf("[VITA] prededup thread started on core 2 (tid=0x%08X)\n", prededup_thread_tid);
        } else {
            prededup_thread_running = 0;
        }
    }
}

void vita_emu64_worker_shutdown(void) {
    if (emu64_worker_tid < 0) return;

    emu64_worker_running = 0;
    sceKernelSignalSema(emu64_work_ready_sema, 1); // wake so it exits
    sceKernelWaitThreadEnd(emu64_worker_tid, NULL, NULL);
    sceKernelDeleteThread(emu64_worker_tid);
    sceKernelDeleteSema(emu64_work_ready_sema);
    sceKernelDeleteSema(emu64_work_done_sema);
    emu64_worker_tid = -1;
    printf("[VITA] emu64 worker thread shut down.\n");
}

void vita_emu64_wait_done(void) {
    if (emu64_worker_tid < 0) return;
    // block directly; the previous spin-poll cost ~2ms/frame when worker was late.
    sceKernelWaitSema(emu64_work_done_sema, 1, NULL);
}

// wait for core 2 prededup to finish. called before submit reads
// the cmd buffer. usually a no-op since prededup (~0.5ms) finishes
// well before game_logic + swap (~4ms) completes.
void vita_wait_prededup(void) {
    if (!prededup_pending) return;
    sceKernelWaitSema(prededup_done_sema, 1, NULL);
    prededup_pending = 0;
}

void vita_emu64_signal_work(void* dl) {
    if (emu64_worker_tid < 0) return;
    emu64_work_dl = dl;
    sceKernelSignalSema(emu64_work_ready_sema, 1);
}

int vita_emu64_worker_active(void) {
    return (emu64_worker_tid >= 0);
}

static void vita_atexit_cleanup(void) {
    vita_emu64_worker_shutdown();
}

// pin SDL/VitaGL/system threads to fixed cores. idempotent.
// core 0: main (submit_frame), SDL audio callback
// core 1: emu64 worker, SceGxmDisplayQueue, SDLTimer, SceCommonDialogWorker
// core 2: vtc_io, AudioProducer, VitaGL GC
void vita_pin_hidden_threads(void) {
    SceKernelThreadInfo info;
    int misses_in_row = 0;
    static int dump_done = 0;

    for (SceUID uid = 0x40010001; uid < 0x40020000 && misses_in_row < 500; uid++) {
        if (sceKernelGetThreadmgrUIDClass(uid) != SCE_KERNEL_TMID_Thread) {
            misses_in_row++;
            continue;
        }
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(uid, &info) != 0) {
            misses_in_row++;
            continue;
        }
        misses_in_row = 0;

        // skip threads already pinned at creation or by pc_platform_init
        if (strcmp(info.name, "emu64_worker") == 0 ||
            strcmp(info.name, "vtc_io") == 0 ||
            strcmp(info.name, "TexPackLoader") == 0 ||
            strcmp(info.name, "ACGC00001") == 0) continue;

        // core 0: SDLAudio callback (tiny)
        // core 1: SceGxmDisplayQueue, SDLTimer, SceCommonDialogWorker
        // core 2: GC, AudioProducer
        int target = 0;
        if (strcmp(info.name, "Garbage Collector") == 0 ||
            strcmp(info.name, "AudioProducer") == 0) {
            target = SCE_KERNEL_CPU_MASK_USER_2;
        } else if (strcmp(info.name, "SceGxmDisplayQueue") == 0 ||
                   strcmp(info.name, "SDLTimer") == 0 ||
                   strcmp(info.name, "SceCommonDialogWorker") == 0) {
            target = SCE_KERNEL_CPU_MASK_USER_1;
        } else if (strncmp(info.name, "SDLAudio", 8) == 0) {
            target = SCE_KERNEL_CPU_MASK_USER_0;
        }

        if (target && info.currentCpuAffinityMask != (SceUInt32)target) {
            sceKernelChangeThreadCpuAffinityMask(uid, target);
        }
    }

    dump_done = 1;
}

void pc_platform_init(void) {
    vglSetupShaderPatcher(4 * 1024 * 1024, 2 * 1024 * 1024, 2 * 1024 * 1024);
    vglSetCircularPoolSize(32 * 1024 * 1024);
    {
        SceGxmMultisampleMode msaa_mode = SCE_GXM_MULTISAMPLE_NONE;
        if (g_pc_settings.msaa == 4) msaa_mode = SCE_GXM_MULTISAMPLE_4X;
        else if (g_pc_settings.msaa == 2) msaa_mode = SCE_GXM_MULTISAMPLE_2X;
        vglInitExtended(256 * 1024, g_pc_settings.render_w, g_pc_settings.render_h,
                        32 * 1024 * 1024, msaa_mode);
        printf("[VITA] MSAA: %s\n", g_pc_settings.msaa == 4 ? "4X" : g_pc_settings.msaa == 2 ? "2X" : "OFF");
    }

    vglUseVram(GL_TRUE);

    // flush all 3 back buffers to clear garbage
    for (int i = 0; i < 3; i++) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        vglSwapBuffers(GL_FALSE);
    }

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    g_pc_window_w = g_pc_settings.render_w;
    g_pc_window_h = g_pc_settings.render_h;

    printf("[VITA] Render resolution: %dx%d\n", g_pc_settings.render_w, g_pc_settings.render_h);
    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) {
        fprintf(stderr, "[VITA] FATAL: SDL_Init failed: %s\n", SDL_GetError());
        vita_fatal_dialog_and_exit("Failed to initialize SDL (input and audio).\n\n"
            "Try rebooting your Vita and launching again.");
    }

    pc_gx_init();
    vita_emu64_worker_init();
    atexit(vita_atexit_cleanup);

    vita_pin_hidden_threads();

    // pin main to core 0. rerun scan for late created threads happens
    // from VIWaitForRetrace at frames 5, 30, 120.
    {
        SceUID main_tid = sceKernelGetThreadId();
        sceKernelChangeThreadCpuAffinityMask(main_tid, SCE_KERNEL_CPU_MASK_USER_0);
    }
}

extern void PADCleanup(void);

void pc_platform_shutdown(void) {
    vita_emu64_worker_shutdown();
    pc_audio_shutdown();
    pc_audio_mq_shutdown();
    PADCleanup();
    pc_gx_shutdown();
    SDL_Quit();
    sceKernelExitProcess(0);
}

void pc_platform_update_window_size(void) {
    g_pc_window_w = g_pc_settings.render_w;
    g_pc_window_h = g_pc_settings.render_h;
}

void pc_platform_swap_buffers(void) {
#ifdef VITA_DEBUG
    unsigned int t0 = sceKernelGetProcessTimeLow();
#endif
    vglSwapBuffers(GL_FALSE);
#ifdef VITA_DEBUG
    vita_timing.swap_us = sceKernelGetProcessTimeLow() - t0;
#endif
}

// a wall-clock gap between polls this big can only be a sleep/resume.
// normal frames are 16-33ms, we look at time(NULL) from the previous
// poll and note any multi-second jump.
#define PC_SUSPEND_DETECT_SEC 2

// suspends this long likely crossed a day boundary; save and reload so
// AC's at-boot daily catch-up (NPC moveouts, mail, weeds, turnip prices,
// snowman, etc.) runs against the new date. shorter wakes just re-anchor
// the clock in place.
#define PC_LONG_SUSPEND_SEC (30 * 60)

static time_t s_pc_last_alive = 0;

int pc_platform_poll_events(void) {
    extern void pc_auto_save_tick(void);
    extern int  pc_auto_save_force(void);

    time_t poll_now = time(NULL);
    time_t elapsed = (s_pc_last_alive > 0 && poll_now > s_pc_last_alive)
                     ? (poll_now - s_pc_last_alive) : 0;
    int just_resumed = (s_pc_last_alive > 0) && (elapsed >= PC_SUSPEND_DETECT_SEC);

    if (just_resumed) {
        OSReport("[PC] Resumed from suspend after %ld s\n", (long)elapsed);

        // memory is preserved across vita sleep, so writing on resume
        // captures the same state a pre-suspend save would have
        if (g_pc_settings.auto_save) pc_auto_save_force();

        if (g_pc_settings.time_sync) {
            // reload during intro respawns intro_demo into train arrival
            // and softlocks, so just resync the clock in place
            extern int mEv_CheckFirstIntro(void);
            if (elapsed >= PC_LONG_SUSPEND_SEC && !mEv_CheckFirstIntro()) {
                OSReport("[PC] Long suspend, reloading app\n");
                sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
                // not reached
            } else {
                if (elapsed >= PC_LONG_SUSPEND_SEC) {
                    OSReport("[PC] Long suspend during intro, skipping reload\n");
                }
                pc_os_time_resync();
            }
        }
    }

    // safety-net periodic save. no-op when feature off or not yet due.
    pc_auto_save_tick();

    // these SDL lifecycle events rarely fire on vita (the power callback
    // above is the real hook), but wire them up anyway as belt-and-braces.
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_APP_WILLENTERBACKGROUND:
                if (g_pc_settings.auto_save) pc_auto_save_force();
                break;

            case SDL_APP_TERMINATING:
                if (g_pc_settings.auto_save) pc_auto_save_force();
                g_pc_running = 0;
                return 0;

            case SDL_QUIT:
                if (g_pc_settings.auto_save) pc_auto_save_force();
                g_pc_running = 0;
                return 0;
        }
    }

    s_pc_last_alive = poll_now;
    return 1;
}

#endif // TARGET_VITA
