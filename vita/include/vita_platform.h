// vita_platform.h
// PS Vita platform layer using VitaGL + SDL2
// included instead of pc_platform.h on Vita builds
#ifndef VITA_PLATFORM_H
#define VITA_PLATFORM_H

#include <stdint.h>
#if UINTPTR_MAX != 0xFFFFFFFFu
#error "Expected 32-bit target for PS Vita"
#endif

#include <vitaGL.h>

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

// Vita system headers
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/apputil.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>

#include "pc_types.h"

// configuration
#define VITA_SCREEN_WIDTH   960
#define VITA_SCREEN_HEIGHT  544

#define PC_GC_WIDTH       640
#define PC_GC_HEIGHT      480
#define PC_SCREEN_WIDTH   VITA_SCREEN_WIDTH
#define PC_SCREEN_HEIGHT  VITA_SCREEN_HEIGHT
#define PC_WINDOW_TITLE   "Animal Crossing"

#define PC_MAIN_MEMORY_SIZE   (24 * 1024 * 1024)
#define PC_ARAM_SIZE          (16 * 1024 * 1024)
#define PC_FIFO_SIZE          (256 * 1024)

#define PC_PI  3.14159265358979323846
#define PC_PIf 3.14159265358979323846f
#define PC_DEG_TO_RAD (PC_PI / 180.0)
#define PC_DEG_TO_RADf (PC_PIf / 180.0f)

// GC hardware clocks (needed by OS time functions)
#define GC_BUS_CLOCK          162000000u
#define GC_CORE_CLOCK         486000000u
#define GC_TIMER_CLOCK        (GC_BUS_CLOCK / 4)

#include <setjmp.h>
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

// global state (matching pc_platform.h interface)
// VitaGL manages the display, no SDL_Window or SDL_GLContext
extern SDL_Window*   g_pc_window;
extern SDL_GLContext  g_pc_gl_context;
extern int           g_pc_running;
extern int           g_pc_verbose;
extern int           g_pc_no_framelimit;
extern int           g_pc_time_override;

extern int g_pc_window_w;
extern int g_pc_window_h;
void pc_platform_update_window_size(void);

// widescreen: Vita is 960x544 (16:9), reuse PC widescreen system
#define PC_NOOP_WIDESCREEN_STRETCH     0xAC5701u
#define PC_NOOP_WIDESCREEN_STRETCH_OFF 0xAC5700u
extern int g_pc_widescreen_stretch;

void pc_platform_init(void);
void pc_platform_shutdown(void);
void pc_platform_swap_buffers(void);
int  pc_platform_poll_events(void);

// crash protection
void pc_crash_protection_init(void);
void pc_crash_set_jmpbuf(jmp_buf* buf);
unsigned int pc_crash_get_addr(void);
unsigned int pc_crash_get_data_addr(void);

extern unsigned int pc_image_base;
extern unsigned int pc_image_end;

extern int g_pc_model_viewer;
extern int g_pc_model_viewer_start;

// per-frame diagnostics
extern int pc_emu64_frame_cmds;
extern int pc_emu64_frame_crashes;
extern int pc_emu64_frame_noop_cmds;
extern int pc_emu64_frame_tri_cmds;
extern int pc_emu64_frame_vtx_cmds;
extern int pc_emu64_frame_dl_cmds;
extern int pc_emu64_frame_cull_visible;
extern int pc_emu64_frame_cull_rejected;

// audio
extern int pc_save_loaded;
int  pc_audio_get_buffer_fill(void);
int  pc_audio_is_active(void);
void pc_audio_shutdown(void);
void pc_audio_start_producer_thread(void);
void pc_audio_mq_init(void);
void pc_audio_mq_shutdown(void);

void vita_init(void);

#ifdef __cplusplus
}
#endif

#endif // VITA_PLATFORM_H
