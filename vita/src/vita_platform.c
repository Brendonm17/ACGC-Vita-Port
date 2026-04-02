// vita_platform.c - PS Vita platform init (VitaGL)
#ifdef TARGET_VITA

#include "pc_platform.h"
#include "pc_gx_internal.h"
#include "pc_settings.h"
#include "vita_shared.h"
#include "vita_gx_cmdbuf.h"

#include <psp2/power.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <vitaGL.h>

// vita SDK "directory already exists"
#define VITA_EEXIST ((int)0x80010011)

// 128MB heap, 4MB stack (1MB caused corruption)
__attribute__((used)) unsigned int _newlib_heap_size_user = 128 * 1024 * 1024;
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

void vita_init(void) {
    int ret;
    ret = scePowerSetArmClockFrequency(444);
    if (ret < 0) fprintf(stderr, "[VITA] WARNING: ARM overclock failed: 0x%08X\n", ret);
    ret = scePowerSetGpuClockFrequency(222);
    if (ret < 0) fprintf(stderr, "[VITA] WARNING: GPU overclock failed: 0x%08X\n", ret);
    ret = scePowerSetBusClockFrequency(222);
    if (ret < 0) fprintf(stderr, "[VITA] WARNING: Bus overclock failed: 0x%08X\n", ret);

    {
        int mk_ret = sceIoMkdir("ux0:data/AnimalCrossing", 0777);
        if (mk_ret < 0 && mk_ret != VITA_EEXIST) {
            fprintf(stderr, "[VITA] FATAL: Cannot create data directory: 0x%08X\n", mk_ret);
            sceKernelExitProcess(1);
        }
    }
    {
        int mk_ret2 = sceIoMkdir("ux0:data/AnimalCrossing/saves", 0777);
        if (mk_ret2 < 0 && mk_ret2 != VITA_EEXIST) {
            fprintf(stderr, "[VITA] FATAL: Cannot create saves directory: 0x%08X\n", mk_ret2);
            sceKernelExitProcess(1);
        }
    }
    {
        int mk_ret3 = sceIoMkdir("ux0:data/AnimalCrossing/rom", 0777);
        if (mk_ret3 < 0 && mk_ret3 != VITA_EEXIST) {
            fprintf(stderr, "[VITA] FATAL: Cannot create rom directory: 0x%08X\n", mk_ret3);
            sceKernelExitProcess(1);
        }
    }
    {
        int mk_ret4 = sceIoMkdir("ux0:data/AnimalCrossing/banners", 0777);
        if (mk_ret4 < 0 && mk_ret4 != VITA_EEXIST)
            printf("[VITA] WARNING: banners dir failed: 0x%08X\n", mk_ret4);
    }
    {
        int mk_ret5 = sceIoMkdir("ux0:data/AnimalCrossing/texture_pack", 0777);
        if (mk_ret5 < 0 && mk_ret5 != VITA_EEXIST)
            printf("[VITA] WARNING: texture_pack dir failed: 0x%08X\n", mk_ret5);
    }
    {
        int mk_ret6 = sceIoMkdir("ux0:data/AnimalCrossing/texture_packs", 0777);
        if (mk_ret6 < 0 && mk_ret6 != VITA_EEXIST)
            printf("[VITA] WARNING: texture_packs dir failed: 0x%08X\n", mk_ret6);
    }

    if (!freopen("ux0:data/AnimalCrossing/error.log", "w", stderr))
        stderr = stdout;  // fallback to psp2link
    setvbuf(stderr, NULL, _IONBF, 0);

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

static int emu64_worker_func(SceSize args, void* argp) {
    (void)args; (void)argp;

    while (emu64_worker_running) {
        sceKernelWaitSema(emu64_work_ready_sema, 1, NULL);
        if (!emu64_worker_running) break;

        void* dl = (void*)emu64_work_dl;
        if (dl) {
            vita_on_worker_thread = 1;
            unsigned int t0 = sceKernelGetProcessTimeLow();
            emu64_taskstart(dl);
            vita_timing.emu64_us = sceKernelGetProcessTimeLow() - t0;
            vita_on_worker_thread = 0;
            // copy worker-thread texture load timing into frame timing struct
            {
                extern unsigned int vita_texload_us;
                vita_timing.texload_us = vita_texload_us;
                vita_texload_us = 0;
            }
        }

        sceKernelSignalSema(emu64_work_done_sema, 1);

        // pre-read VTC textures during idle time
        {
            extern void vita_vtc_prefetch(void);
            unsigned int _pf_t0 = sceKernelGetProcessTimeLow();
            vita_vtc_prefetch();
            extern unsigned int vita_vtc_prefetch_us;
            vita_vtc_prefetch_us += sceKernelGetProcessTimeLow() - _pf_t0;
        }
    }

    return 0;
}

void vita_emu64_worker_init(void) {
    if (!g_pc_settings.multithread) {
        printf("[VITA] Multithreading disabled via settings.\n");
        return;
    }
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
    sceKernelWaitSema(emu64_work_done_sema, 1, NULL);
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

void pc_platform_init(void) {
    vglSetupShaderPatcher(4 * 1024 * 1024, 2 * 1024 * 1024, 2 * 1024 * 1024);
    vglSetVertexPoolSize(32 * 1024 * 1024);
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
        sceKernelExitProcess(1);
    }

    pc_gx_init();
    vita_emu64_worker_init();
    atexit(vita_atexit_cleanup);
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
    unsigned int t0 = sceKernelGetProcessTimeLow();
    vglSwapBuffers(GL_FALSE);
    vita_timing.swap_us = sceKernelGetProcessTimeLow() - t0;
}

int pc_platform_poll_events(void) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                g_pc_running = 0;
                return 0;
        }
    }

    return 1;
}

#endif // TARGET_VITA
