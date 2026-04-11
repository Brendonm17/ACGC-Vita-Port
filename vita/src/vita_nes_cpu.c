// vita_nes_cpu.c
// nes emulation via agnes, provides symbols famicom.cpp expects from ks_nes_core.cpp
#ifdef TARGET_VITA

#include "Famicom/ks_nes_common.h"
#include "Famicom/ks_nes_core.h"
#include "jaudio_NES/emusound.h"
#include "agnes.h"
#include "dolphin/os/OSTime.h"
#include <string.h>
#include <stdio.h>

extern u8 ksNesPaletteNormal[];

// shared with vita_nes_ppu.c
agnes_t* g_agnes = NULL;
static ksNesCommonWorkObj* g_wp;
static ksNesStateObj* g_sp;
static void* g_rom_data;
static size_t g_rom_size;

static void apu_write_hook(uint16_t addr, uint8_t val, void* ud) {
    (void)ud;
    Sound_Write(addr, val, 0);
}

static uint8_t apu_read_hook(uint16_t addr, void* ud) {
    (void)ud;
    return Sound_Read(addr);
}

// linker stubs: referenced by decomp tables, never called on vita
typedef void (*STORE_FUNC)(u32, u32);
typedef void (*LOAD_FUNC)(void);

void ksNesLinecntIrqDefault(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStoreWRAM(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStorePPU(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStoreIO(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStoreBBRAM(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStoreInvalid(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore2000(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStorePPURam(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore2004(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore2005(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore2006(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore2007ChrRom(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore4000(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore4003(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore4011(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore4014(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore4015(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore4016(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore4017(u32 a, u32 b) { (void)a; (void)b; }
void ksNesLoadWRAM(void) {}
void ksNesLoadPPU(void) {}
void ksNesLoadIO(void) {}
void ksNesLoadIgnore(void) {}
void ksNesLoadBBRAM(void) {}
void ksNesLoad4015(void) {}
void ksNesLoad4016(void) {}
void ksNesLoad4017(void) {}
void ksNesLoadInvalid(void) {}
void ksNesLoad05_4000(void) {}
void ksNesLoad13_4000(void) {}
void ksNesStoreQD_4020(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStoreQD_4022(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStoreQD_4023(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStoreQD_4024(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStoreQD_4025(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStoreQD_4026(u32 a, u32 b) { (void)a; (void)b; }
void ksNesLinecntIrqQD(u32 a, u32 b) { (void)a; (void)b; }
void ksNesLinecntIrqMMC3(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore05_5100(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore05_5101(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore05_5102(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore05_5104(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore05_5105(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore05_5106(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore05_5113(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore05_5120(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore05_5128(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore05_5130(u32 a, u32 b) { (void)a; (void)b; }

void ksNesInit01(void) {}
void ksNesStore01_8000(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore02_8000(u32 a, u32 b) { (void)a; (void)b; }
void ksNesInit03(void) {}
void ksNesStore03_6000(u32 a, u32 b) { (void)a; (void)b; }
void ksNesInit04(void) {}
void ksNesStore04_8000(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore04_a000(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore04_c000(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore04_e000(u32 a, u32 b) { (void)a; (void)b; }
void ksNesInit05(void) {}
void ksNesInit07(void) {}
void ksNesStore07_8000(u32 a, u32 b) { (void)a; (void)b; }
void ksNesInit09(void) {}
void ksNesStore09_8000(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore09_a000(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore09_c000(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore09_e000(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore0a_8000(u32 a, u32 b) { (void)a; (void)b; }
void ksNesStore0a_a000(u32 a, u32 b) { (void)a; (void)b; }
void ksNesInit12(void) {}
void ksNesInit13(void) {}
void ksNesInit15(void) {}
void ksNesInit18(void) {}
void ksNesInit1a(void) {}
void ksNesInit42(void) {}
void ksNesInit43(void) {}
void ksNesInit45(void) {}
void ksNesInit49(void) {}
void ksNesInit56(void) {}

STORE_FUNC ksNesStoreFuncTblDefault[] = {
    ksNesStoreWRAM, ksNesStorePPU, ksNesStoreIO,
    (STORE_FUNC)ksNesLinecntIrqDefault, (STORE_FUNC)ksNesLinecntIrqDefault,
    (STORE_FUNC)ksNesLinecntIrqDefault, (STORE_FUNC)ksNesLinecntIrqDefault,
    (STORE_FUNC)ksNesLinecntIrqDefault,
};
void* ksNesStorePPUFuncTblDefault[] = {
    ksNesStore2000, ksNesStorePPURam, (void*)ksNesLinecntIrqDefault, ksNesStorePPURam,
    ksNesStore2004, ksNesStore2005, ksNesStore2006, ksNesStore2007ChrRom,
};
void* ksNesStoreIOFuncTblDefault[40];
LOAD_FUNC ksNesLoadFuncTblDefault[] = {
    ksNesLoadWRAM, ksNesLoadPPU, ksNesLoadIO, ksNesLoadIgnore,
    ksNesLoadBBRAM, ksNesLoadBBRAM, ksNesLoadBBRAM, ksNesLoadBBRAM,
};
void* ksNesLoadIOFuncTblDefault[24];
void* ksNesStoreQDFuncTbl[8];
void* ksNesMapperInitFuncTbl[185][5];

void ksNesConvertChrToI8(ksNesCommonWorkObj* wp, const unsigned char* c, unsigned long t) { (void)wp; (void)c; (void)t; }
void ksNesConvertChrToI8MMC5(ksNesCommonWorkObj* wp, const unsigned char* c, unsigned long t) { (void)wp; (void)c; (void)t; }
void ksNesDrawMakeOBJIndTex(ksNesCommonWorkObj* wp) { (void)wp; }
void ksNesDrawMakeOBJIndTexMMC5(ksNesCommonWorkObj* wp) { (void)wp; }
void ksNesQDSoundSync(void) {}
int ksNesQDFastLoad(ksNesCommonWorkObj* wp, ksNesStateObj* sp) { (void)wp; (void)sp; return -1; }
int ksNesQDFastSave(ksNesCommonWorkObj* wp, ksNesStateObj* sp) { (void)wp; (void)sp; return -1; }

u8 ksNesVoiceIdTable_12[] = {
    0x04, 0x05, 0x06, 0x07, 0x01, 0x02, 0x03, 0x08,
    0x0a, 0x0b, 0x09, 0x0c, 0x0e, 0x0d, 0x0f, 0x0f,
};
u8 ksNesInitQDDataTbl[11] = {
    0x00, 0x2f, 0x00, 0x00, 0x00, 0x06, 0x10, 0xc0, 0x80, 0x35, 0xac
};

// custom rom loading from ux0:data/AnimalCrossing/rom/nes/
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
static int g_custom_rom_selected = -1;

static int str_ends_nes(const char* s) {
    int len = strlen(s);
    if (len < 4) return 0;
    return (s[len-4]=='.' && (s[len-3]=='n'||s[len-3]=='N') &&
            (s[len-2]=='e'||s[len-2]=='E') && (s[len-1]=='s'||s[len-1]=='S'));
}

int vita_nes_scan_roms(void) {
    g_custom_rom_count = 0;
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

static void sort_rom_list(void) {
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

int vita_nes_show_picker(void) {
    if (g_custom_rom_count <= 0) return -1;
    if (g_custom_rom_count == 1) { g_custom_rom_selected = 0; return 0; }

    sort_rom_list();

    int sel = 0;
    int scroll = 0;
    int max_visible = 16;

    SceCtrlData ctrl, prev_ctrl;
    sceCtrlPeekBufferPositive(0, &prev_ctrl, 1);

    while (1) {
        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        u32 pressed = ctrl.buttons & ~prev_ctrl.buttons;
        prev_ctrl = ctrl;

        if (pressed & SCE_CTRL_CROSS) { g_custom_rom_selected = sel; return sel; }
        if (pressed & SCE_CTRL_CIRCLE) { g_custom_rom_selected = -1; return -1; }

        if (pressed & SCE_CTRL_UP) {
            if (sel > 0) sel--;
            if (sel < scroll) scroll = sel;
        }
        if (pressed & SCE_CTRL_DOWN) {
            if (sel < g_custom_rom_count - 1) sel++;
            if (sel >= scroll + max_visible) scroll = sel - max_visible + 1;
        }

        JW_BeginFrame();

        // dark overlay so text is readable
        GXSetNumChans(1);
        GXSetNumTexGens(0);
        GXSetNumTevStages(1);
        GXSetNumIndStages(0);
        GXSetTevDirect(GX_TEVSTAGE0);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
        GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
        GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP);
        GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
        GXClearVtxDesc();
        GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
        GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_S16, 0);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
        {
            Mtx44 ortho;
            C_MTXOrtho(ortho, 0, -480.f, 0.f, 640.f, 0.f, 100.f);
            GXSetProjection(ortho, GX_ORTHOGRAPHIC);
        }
        GXBegin(GX_QUADS, GX_VTXFMT0, 4);
        GXPosition2s16(0, 0);       GXColor1u32(0x000000C8);
        GXPosition2s16(640, 0);     GXColor1u32(0x000000C8);
        GXPosition2s16(640, -480);  GXColor1u32(0x000000C8);
        GXPosition2s16(0, -480);    GXColor1u32(0x000000C8);
        GXEnd();

        JW_JUTReport(20, 40, 1, "NES Game Select");
        JW_JUTReport(20, 60, 1, "[%d/%d]  Cross: play  Circle: cancel", sel + 1, g_custom_rom_count);

        for (int i = 0; i < max_visible && scroll + i < g_custom_rom_count; i++) {
            int idx = scroll + i;
            JW_JUTReport(30, 90 + i * 22, 1, "%s %s",
                      idx == sel ? "->" : "  ",
                      g_custom_rom_names[idx]);
        }

        JW_EndFrame();
        VIWaitForRetrace();
    }
}

static int agnes_init_from_rom(void* rom_data, size_t rom_size) {
    if (g_agnes) { agnes_destroy(g_agnes); g_agnes = NULL; }
    g_agnes = agnes_make();
    if (!g_agnes) return -1;
    if (!agnes_load_ines_data(g_agnes, rom_data, rom_size)) {
        agnes_destroy(g_agnes); g_agnes = NULL;
        return -2;
    }
    agnes_set_apu_callback(g_agnes, apu_write_hook, apu_read_hook, NULL);
    return 0;
}

void ksNesPushResetButton(ksNesStateObj* sp) {
    (void)sp;
    if (g_rom_data && g_rom_size > 0)
        agnes_init_from_rom(g_rom_data, g_rom_size);
}

u32 ksNesResetAsm(ksNesCommonWorkObj* wp, ksNesStateObj* sp) {
    (void)wp; (void)sp;
    return 0;
}

void ksNesEmuFrame(ksNesCommonWorkObj* wp, ksNesStateObj* sp, u32 flags) {
    (void)sp; (void)flags;
    if (!g_agnes) return;

    u32 p0 = wp->pads[0] >> 24;
    u32 p1 = wp->pads[2] >> 24;
    agnes_input_t in1 = {
        .a      = (p0 >> 7) & 1,
        .b      = (p0 >> 6) & 1,
        .select = (p0 >> 5) & 1,
        .start  = (p0 >> 4) & 1,
        .up     = (p0 >> 3) & 1,
        .down   = (p0 >> 2) & 1,
        .left   = (p0 >> 1) & 1,
        .right  = (p0 >> 0) & 1,
    };
    agnes_input_t in2 = {
        .a      = (p1 >> 7) & 1,
        .b      = (p1 >> 6) & 1,
        .select = (p1 >> 5) & 1,
        .start  = (p1 >> 4) & 1,
        .up     = (p1 >> 3) & 1,
        .down   = (p1 >> 2) & 1,
        .left   = (p1 >> 1) & 1,
        .right  = (p1 >> 0) & 1,
    };
    agnes_set_input(g_agnes, &in1, &in2);
    agnes_next_frame(g_agnes);
}

int ksNesReset(ksNesCommonWorkObj* wp, ksNesStateObj* sp, u32 flags, u8* chrramp, u8* bbramp) {
    g_wp = wp;
    g_sp = sp;
    (void)chrramp; (void)bbramp;

    if ((flags & 0x40) == 0) {
        static u16 sound_init_data[] = {
            0x4015, 0x0000, 0x4008, 0x0000, 0x4080, 0x0080, 0x5015, 0x0000,
            0x4010, 0x000f, 0x4011, 0x0000, 0x4012, 0x0000, 0x4013, 0x0000,
            0x4015, 0x0000, 0x5015, 0x0007,
        };
        Sound_SetC000(sp->wram);
        Sound_SetE000(sp->wram);
        for (u32 i = 0; i < 0x106; i++) {
            if (!(i & 7) && i >= 0x40 && i < 0x90) {
                u32 idx = ((i - 0x40) >> 3) & 0x3fffffff;
                Sound_Write(sound_init_data[idx], (u8)sound_init_data[idx + 1], i * 0x72);
            }
            Sound_Write(0, 0, i * 0x72);
        }
    }

    memset(sp, 0, sizeof(ksNesStateObj));

    // load custom rom from filesystem if one was selected in the picker
    if (g_custom_rom_selected >= 0 && g_custom_rom_selected < g_custom_rom_count) {
        int ok = 0;
        FILE* f = fopen(g_custom_rom_paths[g_custom_rom_selected], "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            size_t fsize = ftell(f);
            fseek(f, 0, SEEK_SET);
            size_t max_rom = KS_NES_NESFILE_HEADER_SIZE + KS_NES_PRGROM_SIZE + KS_NES_CHRROM_SIZE;
            if (fsize <= max_rom && wp->nesromp) {
                memset(wp->nesromp, 0, max_rom);
                fread(wp->nesromp, 1, fsize, f);
                ok = 1;
            }
            fclose(f);
        }
        g_custom_rom_selected = -1;
        if (!ok) return 0x57a;
    }

    u8* rom = wp->nesromp;
    if (!rom) return -1;

    if (rom[0] == 'N' && rom[1] == 'E' && rom[2] == 'S' && rom[3] == 0x1A) {
        size_t prg = (size_t)rom[4] * 0x4000;
        size_t chr = (size_t)rom[5] * 0x2000;
        size_t trainer = (rom[6] & 0x04) ? 512 : 0;
        g_rom_size = 16 + trainer + prg + chr;
    } else if (rom[0] == 1) {
        return 0x570;
    } else {
        g_rom_size = KS_NES_NESFILE_HEADER_SIZE + (size_t)rom[4] * 0x4000 + (size_t)rom[5] * 0x2000;
    }
    g_rom_data = rom;

    int res = agnes_init_from_rom(g_rom_data, g_rom_size);
    if (res != 0) return 0x57a;

    if ((flags & 0x40) == 0) {
        size_t prg_size = (size_t)rom[4] * 0x4000;
        u8* prg_start = rom + KS_NES_NESFILE_HEADER_SIZE;
        u8* c000_bank = prg_start + (prg_size > 0x4000 ? prg_size - 0x4000 : 0);
        u8* e000_bank = prg_start + (prg_size > 0x4000 ? prg_size - 0x2000 : 0x2000 > prg_size ? 0 : 0x2000);
        Sound_SetC000(c000_bank);
        Sound_SetE000(e000_bank);
        Sound_SetMMC(3);
    }

    sp->nesromp = rom;
    sp->mapper = (rom[7] & 0xf0) | (rom[6] >> 4);
    sp->prg_size = (u32)rom[4] << 14;
    sp->chr_size = (u32)rom[5] << 13;
    sp->PC = 0;

    sp->os_tick = OSGetTick();
    return 0;
}

#endif
