// vita_nes_fixnes.c
// NES emulation via Nofrendo, provides the pc_fixnes_* API for famicom.cpp
#ifdef TARGET_VITA

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <vitaGL.h>

#include <noftypes.h>
#include <bitmap.h>
#include <nes.h>
#include <nes_pal.h>
#include <vid_drv.h>
#include <osd.h>
#include <gui.h>
#include <event.h>
#include <nofrendo.h>
#include <nofconfig.h>
#include <nesinput.h>
#include <log.h>

extern int g_pc_window_w;
extern int g_pc_window_h;

uint8_t *emuPrgRAM = NULL;
uint32_t emuPrgRAMsize = 0;

static int fixnes_initialized = 0;
int g_custom_rom_selected = -1;
static int vita_nes_load_custom_rom(uint8_t *buf, int buf_size);
static GLuint fixnes_texture = 0;
static int fixnes_frame_ready = 0;
static uint32_t rgba_buf[NES_SCREEN_WIDTH * NES_VISIBLE_HEIGHT];
static uint8_t nof_buttons = 0;
static nes_t *s_machine = NULL;
static uint8_t *s_rom_data = NULL;
static int s_rom_size = 0;
static void (*s_apu_process)(void *buffer, int size) = NULL;

// video driver: static bitmap, no heap alloc
static uint8_t s_hw_data[NES_SCREEN_WIDTH * NES_SCREEN_HEIGHT];
static struct { bitmap_t bmp; uint8 *lines[NES_SCREEN_HEIGHT]; } s_hw_store;
static bitmap_t *s_hw_bitmap = NULL;
static rgb_t s_palette[256];
static nesinput_t s_pad1;

// osd callbacks for nofrendo

static int vita_vid_init(int width, int height);
static void vita_vid_shutdown(void);
static void vita_vid_set_palette(rgb_t *palette);
static bitmap_t *vita_vid_lock_write(void);

char *osd_getromdata(void) { return (char *)s_rom_data; }

void osd_getvideoinfo(vidinfo_t *info) {
    static viddriver_t drv;
    memset(&drv, 0, sizeof(drv));
    drv.name = "vita";
    drv.init = vita_vid_init;
    drv.shutdown = vita_vid_shutdown;
    drv.set_palette = vita_vid_set_palette;
    drv.lock_write = vita_vid_lock_write;
    drv.invalidate = false;
    info->default_width = NES_SCREEN_WIDTH;
    info->default_height = NES_VISIBLE_HEIGHT;
    info->driver = &drv;
}

static int vita_vid_init(int width, int height) {
    s_hw_bitmap = &s_hw_store.bmp;
    s_hw_bitmap->width = width;
    s_hw_bitmap->height = height;
    s_hw_bitmap->pitch = NES_SCREEN_WIDTH;
    s_hw_bitmap->hardware = true;
    s_hw_bitmap->data = s_hw_data;
    for (int i = 0; i < NES_SCREEN_HEIGHT; i++)
        s_hw_bitmap->line[i] = s_hw_data + i * NES_SCREEN_WIDTH;
    memset(s_hw_data, 0, sizeof(s_hw_data));
    return 0;
}

static void vita_vid_shutdown(void) { s_hw_bitmap = NULL; }
static void vita_vid_set_palette(rgb_t *palette) { if (palette) memcpy(s_palette, palette, sizeof(s_palette)); }
static bitmap_t *vita_vid_lock_write(void) { return s_hw_bitmap; }

void osd_getsoundinfo(sndinfo_t *info) { info->sample_rate = 32000; info->bps = 16; }
int osd_init(void) { return 0; }
void osd_shutdown(void) {}
void osd_setsound(void (*playfunc)(void *buffer, int size)) { s_apu_process = playfunc; }

int osd_installtimer(int frequency, void *func, int funcsize, void *counter, int countersize) {
    (void)frequency; (void)func; (void)funcsize; (void)counter; (void)countersize;
    return 0;
}

void osd_fullname(char *fullname, const char *shortname) {
    strncpy(fullname, shortname, PATH_MAX - 1);
    fullname[PATH_MAX - 1] = '\0';
}

char *osd_newextension(char *string, char *ext) {
    int i = strlen(string);
    while (i > 0 && string[i] != '.' && string[i] != PATH_SEP) i--;
    if (string[i] == '.') strcpy(string + i, ext);
    else strcat(string, ext);
    return string;
}

void osd_getinput(void) { input_event(&s_pad1, INP_STATE_MAKE, nof_buttons); }
void osd_getmouse(int *x, int *y, int *button) { *x = 0; *y = 0; *button = 0; }
int osd_makesnapname(char *filename, int len) { (void)filename; (void)len; return -1; }

static bool cfg_open(void) { return false; }
static void cfg_close(void) {}
static int cfg_read_int(const char *g, const char *k, int def) { (void)g; (void)k; return def; }
static const char *cfg_read_string(const char *g, const char *k, const char *def) { (void)g; (void)k; return def; }
static void cfg_write_int(const char *g, const char *k, int v) { (void)g; (void)k; (void)v; }
static void cfg_write_string(const char *g, const char *k, const char *v) { (void)g; (void)k; (void)v; }

config_t config = { cfg_open, cfg_close, cfg_read_int, cfg_read_string, cfg_write_int, cfg_write_string, NULL };

int osd_main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    return main_loop("rom.nes", system_nes);
}

// timing from nes.c (static there, replicated here for frame-by-frame driving)
#define NES_FIQ_PERIOD_INT 29781
#define NES_SCANLINE_CYCLES_F 113.667f

// pc_fixnes API

void pc_fixnes_set_result_ptr(void* ptr) { (void)ptr; }

void pc_fixnes_init(uint8_t *ines_data, int ines_size) {
    if (s_machine || fixnes_initialized)
        pc_fixnes_cleanup();
    fixnes_initialized = 0;

    if (g_custom_rom_selected >= 0) {
        // on failure the buffer is left untouched so nofrendo can fall back
        // to the built-in cluclu rom that famicom_rom_load preloaded.
        vita_nes_load_custom_rom(ines_data, ines_size);
    }

    s_rom_data = ines_data;
    s_rom_size = ines_size;

    memset(&s_pad1, 0, sizeof(s_pad1));
    s_pad1.type = INP_JOYPAD0;
    input_reset();

    log_init();
    event_init();
    config.open();
    osd_init();
    gui_init();

    vidinfo_t video;
    osd_getvideoinfo(&video);
    if (vid_init(video.default_width, video.default_height, video.driver))
        return;

    event_set_system(system_nes);
    gui_setrefresh(NES_REFRESH_RATE);

    s_machine = nes_create();
    if (!s_machine) return;

    if (nes_insertcart("rom.nes", s_machine)) {
        s_machine = NULL;
        return;
    }

    vid_setmode(NES_SCREEN_WIDTH, NES_SCREEN_HEIGHT);
    input_register(&s_pad1);

    nes_t *ctx = nes_getcontextptr();
    if (ctx) {
        ctx->scanline_cycles = 0;
        ctx->fiq_cycles = NES_FIQ_PERIOD_INT;
    }

    if (ctx && ctx->rominfo && ctx->rominfo->sram) {
        emuPrgRAM = ctx->rominfo->sram;
        emuPrgRAMsize = 0x2000;
    } else {
        emuPrgRAM = NULL;
        emuPrgRAMsize = 0;
    }

    if (ctx && ctx->apu)
        osd_setsound(ctx->apu->process);

    fixnes_initialized = 1;
    nof_buttons = 0;
}

void pc_fixnes_write_wram(unsigned int ofs, const uint8_t *data, unsigned int size) {
    nes_t *ctx = nes_getcontextptr();
    if (!ctx || !ctx->cpu || !ctx->cpu->mem_page[0]) return;
    if (ofs + size > 0x800) return;
    memcpy(ctx->cpu->mem_page[0] + ofs, data, size);
}

void pc_fixnes_sync_wram(uint8_t *dst_wram) {
    nes_t *ctx = nes_getcontextptr();
    if (!ctx || !ctx->cpu || !ctx->cpu->mem_page[0] || !dst_wram) return;
    memcpy(dst_wram, ctx->cpu->mem_page[0], 0x800);
}

void pc_fixnes_set_input(uint8_t buttons) {
    nof_buttons = buttons;
    s_pad1.data = buttons;
    input_event(&s_pad1, INP_STATE_MAKE, buttons);
}

static void vita_nes_checkfiq(nes_t *ctx, int cycles) {
    ctx->fiq_cycles -= cycles;
    if (ctx->fiq_cycles <= 0) {
        ctx->fiq_cycles += NES_FIQ_PERIOD_INT;
        if (0 == (ctx->fiq_state & 0xC0)) {
            ctx->fiq_occurred = true;
            nes6502_irq();
        }
    }
}

uint16_t *pc_fixnes_frame(void) {
    if (!fixnes_initialized) return NULL;

    nes_t *ctx = nes_getcontextptr();
    if (!ctx) return NULL;

    osd_getinput();

    bitmap_t *vidbuf = vid_getbuffer();
    if (!vidbuf) return NULL;

    int elapsed_cycles;
    int in_vblank = 0;

    while (262 != ctx->scanline) {
        ppu_scanline(vidbuf, ctx->scanline, true);

        if (241 == ctx->scanline) {
            elapsed_cycles = nes6502_execute(7);
            ctx->scanline_cycles -= elapsed_cycles;
            vita_nes_checkfiq(ctx, elapsed_cycles);
            ppu_checknmi();
            if (ctx->mmc && ctx->mmc->intf && ctx->mmc->intf->vblank)
                ctx->mmc->intf->vblank();
            in_vblank = 1;
        }

        if (ctx->mmc && ctx->mmc->intf && ctx->mmc->intf->hblank)
            ctx->mmc->intf->hblank(in_vblank);

        ctx->scanline_cycles += NES_SCANLINE_CYCLES_F;
        elapsed_cycles = nes6502_execute((int)ctx->scanline_cycles);
        ctx->scanline_cycles -= (float)elapsed_cycles;
        vita_nes_checkfiq(ctx, elapsed_cycles);

        ppu_endscanline(ctx->scanline);
        ctx->scanline++;
    }
    ctx->scanline = 0;

    // indexed PPU output -> RGBA via palette
    int vis_start = (NES_SCREEN_HEIGHT - NES_VISIBLE_HEIGHT) / 2;
    for (int y = 0; y < NES_VISIBLE_HEIGHT; y++) {
        uint32_t *dst = &rgba_buf[y * NES_SCREEN_WIDTH];
        uint8 *src = vidbuf->line[y + vis_start];
        for (int x = 0; x < NES_SCREEN_WIDTH; x++) {
            rgb_t c = s_palette[src[x]];
            dst[x] = 0xFF000000 | ((uint32_t)c.b << 16) | ((uint32_t)c.g << 8) | c.r;
        }
    }

    // mono s16 APU -> stereo s16 for AIInitDMA
    if (s_apu_process) {
        static int16_t mono_buf[1024];
        static int16_t stereo_buf[1024 * 2];
        int samples = 32000 / 60;
        if (samples > 1024) samples = 1024;
        s_apu_process(mono_buf, samples);
        for (int i = 0; i < samples; i++) {
            stereo_buf[i * 2 + 0] = mono_buf[i];
            stereo_buf[i * 2 + 1] = mono_buf[i];
        }
        extern void AIInitDMA(uint32_t addr, uint32_t size);
        AIInitDMA((uint32_t)(uintptr_t)stereo_buf, samples * 4);
    }

    fixnes_frame_ready = 1;
    return (uint16_t *)rgba_buf;
}

void pc_fixnes_cleanup(void) {
    fixnes_initialized = 0;
    s_apu_process = NULL;
    emuPrgRAM = NULL;
    emuPrgRAMsize = 0;
    fixnes_frame_ready = 0;

    if (s_machine) {
        nes_t *ctx = nes_getcontextptr();
        if (ctx && ctx->rominfo) {
            ctx->rominfo->rom = NULL;
            ctx->rominfo->vrom = NULL;
        }
        if (s_machine->rominfo) {
            s_machine->rominfo->rom = NULL;
            s_machine->rominfo->vrom = NULL;
        }
        nes_destroy(&s_machine);
        vid_shutdown();
        s_hw_bitmap = NULL;
    }

    s_rom_data = NULL;
    s_rom_size = 0;
    s_machine = NULL;

    if (fixnes_texture) {
        glDeleteTextures(1, &fixnes_texture);
        fixnes_texture = 0;
    }
}

// custom ROM picker

#include "Famicom/ks_nes_common.h"
#include "dolphin/gx.h"
#include "dolphin/mtx.h"
#include <dirent.h>
#include <psp2/io/stat.h>
#include <psp2/ctrl.h>
#include <strings.h>
#include "jsyswrap.h"
#include "libjsys/jsyswrapper.h"
extern void VIWaitForRetrace(void);

#define VITA_NES_ROM_DIR "ux0:data/AnimalCrossing/rom/nes"
#define VITA_NES_MAX_ROMS 64

static char g_custom_rom_paths[VITA_NES_MAX_ROMS][256];
static char g_custom_rom_names[VITA_NES_MAX_ROMS][48];
static int g_custom_rom_count = 0;

static int str_ends_nes(const char* s) {
    int len = strlen(s);
    if (len < 4) return 0;
    return (s[len-4]=='.' && (s[len-3]=='n'||s[len-3]=='N') &&
            (s[len-2]=='e'||s[len-2]=='E') && (s[len-1]=='s'||s[len-1]=='S'));
}

// accessors used by the my_room picker overlay (separate file so it can
// include game headers without colliding with nofrendo's BUTTON_A et al.)
int vita_nes_get_rom_count(void) { return g_custom_rom_count; }

const char* vita_nes_get_rom_name(int idx) {
    if (idx < 0 || idx >= g_custom_rom_count) return "";
    return g_custom_rom_names[idx];
}

void vita_nes_set_selected(int idx) {
    if (idx >= 0 && idx < g_custom_rom_count) g_custom_rom_selected = idx;
}

void vita_nes_sort_roms(void);  // forward decl, defined below

int vita_nes_scan_roms(void) {
    g_custom_rom_count = 0;
    // belt-and-suspenders: vita_platform.c creates the first two on boot, but
    // defend against init order changes. errors (incl. already-exists) ignored.
    sceIoMkdir("ux0:data/AnimalCrossing", 0777);
    sceIoMkdir("ux0:data/AnimalCrossing/rom", 0777);
    sceIoMkdir(VITA_NES_ROM_DIR, 0777);

    DIR* dir = opendir(VITA_NES_ROM_DIR);
    if (!dir) return 0;

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL && g_custom_rom_count < VITA_NES_MAX_ROMS) {
        if (!str_ends_nes(ent->d_name)) continue;
        snprintf(g_custom_rom_paths[g_custom_rom_count], 256, "%s/%s", VITA_NES_ROM_DIR, ent->d_name);
        strncpy(g_custom_rom_names[g_custom_rom_count], ent->d_name, 47);
        g_custom_rom_names[g_custom_rom_count][47] = '\0';
        char* dot = strrchr(g_custom_rom_names[g_custom_rom_count], '.');
        if (dot) *dot = '\0';
        g_custom_rom_count++;
    }
    closedir(dir);
    return g_custom_rom_count;
}

void vita_nes_sort_roms(void) {
    for (int i = 0; i < g_custom_rom_count - 1; i++) {
        for (int j = i + 1; j < g_custom_rom_count; j++) {
            if (strcasecmp(g_custom_rom_names[i], g_custom_rom_names[j]) > 0) {
                char tmp_path[256], tmp_name[48];
                memcpy(tmp_path, g_custom_rom_paths[i], 256);
                memcpy(tmp_name, g_custom_rom_names[i], 48);
                memcpy(g_custom_rom_paths[i], g_custom_rom_paths[j], 256);
                memcpy(g_custom_rom_names[i], g_custom_rom_names[j], 48);
                memcpy(g_custom_rom_paths[j], tmp_path, 256);
                memcpy(g_custom_rom_names[j], tmp_name, 48);
            }
        }
    }
}

static int vita_nes_load_custom_rom(uint8_t *buf, int buf_size) {
    // always clear selection so a failure can't be retried with stale state.
    int rom_idx = g_custom_rom_selected;
    g_custom_rom_selected = -1;

    if (rom_idx < 0 || rom_idx >= g_custom_rom_count) return 0;
    if (!buf || buf_size < 16) return 0;

    FILE *f = fopen(g_custom_rom_paths[rom_idx], "rb");
    if (!f) return 0;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long fsize = ftell(f);
    if (fsize < 16 || fsize > buf_size) { fclose(f); return 0; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }

    // validate iNES magic on the file BEFORE writing to buf, so a bad file
    // leaves the caller's built-in rom intact and fallback works.
    uint8_t header[16];
    if (fread(header, 1, 16, f) != 16) { fclose(f); return 0; }
    if (header[0] != 'N' || header[1] != 'E' || header[2] != 'S' || header[3] != 0x1A) {
        fclose(f);
        return 0;
    }

    // header is valid - commit. write header then rest. on partial read the
    // buffer is left zero-padded so nofrendo will reject it cleanly rather
    // than mixing leftover built-in rom bytes into a corrupt cart image.
    memcpy(buf, header, 16);
    size_t remaining = (size_t)(fsize - 16);
    size_t got = fread(buf + 16, 1, remaining, f);
    fclose(f);

    if (got != remaining) {
        memset(buf, 0, buf_size);
        return 0;
    }

    // clear trailing area so the mapper doesn't see stale built-in rom data
    // past the new prg+chr regions.
    if ((size_t)fsize < (size_t)buf_size)
        memset(buf + fsize, 0, buf_size - fsize);

    return 1;
}

// ks_nes_draw.cpp replacements (excluded from build, we provide these)

u8 ksNesPaletteNormal[] = {
    0xc2, 0x10, 0x80, 0x17, 0x98, 0x17, 0xc0, 0x14, 0xdc, 0x0d, 0xd8, 0x03, 0xd8, 0x00, 0xc8, 0x80,
    0xbc, 0xa0, 0x80, 0xe0, 0x81, 0x21, 0x80, 0xe4, 0x80, 0xac, 0x80, 0x00, 0x80, 0x00, 0x80, 0x00,
    0xe7, 0x39, 0x81, 0x7f, 0xa0, 0xff, 0xd8, 0xd9, 0xfc, 0xd5, 0xfc, 0xcb, 0xfc, 0xc3, 0xe9, 0x20,
    0xe1, 0x80, 0x9d, 0xe0, 0x8e, 0x02, 0x82, 0x4c, 0x82, 0x18, 0x88, 0x42, 0x80, 0x00, 0x80, 0x00,
    0xff, 0xff, 0x82, 0x5f, 0xb6, 0x1f, 0xe9, 0xbf, 0xfd, 0xd9, 0xfd, 0xb3, 0xfd, 0xeb, 0xfe, 0x4b,
    0xfe, 0x86, 0xd2, 0xe0, 0xab, 0x6d, 0xa7, 0x55, 0x83, 0x7f, 0xb1, 0x8c, 0x80, 0x00, 0x80, 0x00,
    0xff, 0xff, 0xc2, 0xff, 0xde, 0xff, 0xea, 0xff, 0xfe, 0xfd, 0xfe, 0xf9, 0xff, 0x16, 0xff, 0x35,
    0xff, 0x74, 0xe7, 0x93, 0xd7, 0xb6, 0xd7, 0xdd, 0xdb, 0xbf, 0xef, 0x7b, 0x80, 0x00, 0x80, 0x00,
};

void ksNesDrawInit(ksNesCommonWorkObj* wp) {
    Mtx44 mtx;
    Vec v1 = { 0.f, 0.f, 800.f };
    Vec v3 = { 0.f, 0.f, -100.f };
    Vec v2 = { 0.f, 1.f, 0.f };
    GXInvalidateTexAll();
    GXInvalidateVtxCache();
    GXSetClipMode(GX_CLIP_DISABLE);
    GXSetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GXSetCopyFilter(GX_FALSE, NULL, GX_FALSE, NULL);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GXSetZTexture(GX_ZT_DISABLE, GX_TF_Z8, 0);
    GXSetZCompLoc(GX_FALSE);
    GXSetColorUpdate(GX_TRUE);
    GXSetBlendMode(GX_BM_NONE, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_COPY);
    GXSetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
    GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
    C_MTXOrtho(mtx, 0, -480.f, 0.f, 640.f, 0.f, 2000.f);
    GXSetProjection(mtx, GX_ORTHOGRAPHIC);
    C_MTXLookAt(wp->draw_ctx.draw_mtx, &v1, &v2, &v3);
    GXLoadPosMtxImm(wp->draw_ctx.draw_mtx, 0);
    GXSetCurrentMtx(0);
}

void ksNesDraw(ksNesCommonWorkObj* wp, ksNesStateObj* sp) { (void)wp; (void)sp; }

void ksNesDrawEnd(void) {
    GXSetClipMode(GX_CLIP_ENABLE);
    GXSetZCompLoc(GX_TRUE);
    GXSetNumIndStages(0);
    GXSetTevDirect(GX_TEVSTAGE0);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
    GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
    GXSetTexCoordScaleManually(GX_TEXCOORD0, GX_FALSE, 0, 0);
    GXSetTexCoordScaleManually(GX_TEXCOORD1, GX_FALSE, 0, 0);
}

void pc_fixnes_render_frame(uint16_t *fb) { (void)fb; }

// draw NES framebuffer using the game's shader pipeline (same as vita_banner.c)

#include "pc_gx_internal.h"
extern GLuint vita_get_simple_shader(void);
extern void vita_set_vertex_attrib_pointers(void);

void vita_fixnes_draw_screen(void) {
    if (!fixnes_frame_ready || !fixnes_initialized) return;

    GLuint prog = vita_get_simple_shader();
    if (!prog) return;

    if (!fixnes_texture) {
        glGenTextures(1, &fixnes_texture);
        glBindTexture(GL_TEXTURE_2D, fixnes_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, fixnes_texture);
    }

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, NES_SCREEN_WIDTH, NES_VISIBLE_HEIGHT, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba_buf);

    // centered 4:3 filling display height
    int disp_w = 960, disp_h = 544;
    int vp_h = disp_h;
    int vp_w = (disp_h * 4) / 3;
    int vp_x = (disp_w - vp_w) / 2;

    PCGXVertex verts[6];
    memset(verts, 0, sizeof(verts));
    #define NV(i, px, py, u, v) do { \
        verts[i].position[0]=px; verts[i].position[1]=py; verts[i].position[2]=0; \
        verts[i].color0[0]=255; verts[i].color0[1]=255; verts[i].color0[2]=255; verts[i].color0[3]=255; \
        verts[i].texcoord[0][0]=u; verts[i].texcoord[0][1]=v; \
    } while(0)
    NV(0, -1,-1, 0,1); NV(1, 1,-1, 1,1); NV(2, 1,1, 1,0);
    NV(3, -1,-1, 0,1); NV(4, 1, 1, 1,0); NV(5,-1,1, 0,0);
    #undef NV

    // Dedicated VBO; same reason as banner_draw_bars.
    static GLuint s_nes_vbo = 0;
    if (!s_nes_vbo) glGenBuffers(1, &s_nes_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_nes_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);
    vita_set_vertex_attrib_pointers();
    glUseProgram(prog);

    static struct { GLuint shader; GLint proj, mv, use_tex, tc_src, nchans, tmtx, tgsrc, tex0; } ul;
    if (ul.shader != prog) {
        ul.shader  = prog;
        ul.proj    = glGetUniformLocation(prog, "u_projection");
        ul.mv      = glGetUniformLocation(prog, "u_modelview");
        ul.use_tex = glGetUniformLocation(prog, "u_use_texture0");
        ul.tc_src  = glGetUniformLocation(prog, "u_tev0_tc_src");
        ul.nchans  = glGetUniformLocation(prog, "u_num_chans");
        ul.tmtx    = glGetUniformLocation(prog, "u_texmtx_enable");
        ul.tgsrc   = glGetUniformLocation(prog, "u_texgen_src0");
        ul.tex0    = glGetUniformLocation(prog, "u_texture0");
    }

    float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    if (ul.proj >= 0)    glUniformMatrix4fv(ul.proj, 1, GL_FALSE, ident);
    if (ul.mv >= 0)      glUniformMatrix4fv(ul.mv, 1, GL_FALSE, ident);
    if (ul.use_tex >= 0) glUniform1f(ul.use_tex, 1.0f);
    if (ul.tc_src >= 0)  glUniform1f(ul.tc_src, 0.0f);
    if (ul.nchans >= 0)  glUniform1f(ul.nchans, 0.0f);
    if (ul.tmtx >= 0)    glUniform1f(ul.tmtx, 0.0f);
    if (ul.tgsrc >= 0)   glUniform1f(ul.tgsrc, 0.0f);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fixnes_texture);
    if (ul.tex0 >= 0) glUniform1i(ul.tex0, 0);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_FALSE);

    glViewport(0, 0, disp_w, disp_h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glViewport(vp_x, 0, vp_w, vp_h);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glViewport(0, 0, disp_w, disp_h);

    fixnes_frame_ready = 0;
}

#endif
