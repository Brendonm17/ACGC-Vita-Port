// vita_nes_picker.c
// In-game ROM picker overlay for the empty NES console. Drawn from
// my_room's actor draw as a translucent panel on top of the scene, in
// the same style as the title screen options menu.

#ifdef TARGET_VITA

#include "main.h"
#include "game.h"
#include "graph.h"
#include "padmgr.h"
#include "m_font.h"
#include "m_common_data.h"
#include "m_player_lib.h"
#include "vita_platform.h"

extern int vita_nes_scan_roms(void);
extern int vita_nes_get_rom_count(void);
extern const char* vita_nes_get_rom_name(int idx);
extern void vita_nes_set_selected(int idx);
extern void vita_nes_sort_roms(void);

static int s_picker_open = 0;
static int s_picker_sel = 0;
static int s_picker_cooldown = 0;  // input debounce
static int s_picker_result = 0;    // 0=active, 1=confirmed, -1=cancelled

int vita_nes_picker_is_active(void) { return s_picker_open; }
int vita_nes_picker_consume_result(void) { int r = s_picker_result; s_picker_result = 0; return r; }

void vita_nes_picker_open(void) {
    vita_nes_scan_roms();
    vita_nes_sort_roms();
    s_picker_open = 1;
    s_picker_sel = 0;
    s_picker_cooldown = 6;
    s_picker_result = 0;
}

static void vita_nes_picker_close(int confirmed) {
    s_picker_open = 0;
    s_picker_result = confirmed ? 1 : -1;
    if (confirmed) vita_nes_set_selected(s_picker_sel);
}

void vita_nes_picker_update(GAME* game) {
    if (!s_picker_open) return;

    int count = vita_nes_get_rom_count();
    if (count <= 0) { vita_nes_picker_close(0); return; }

    // Refuse the player's main input each frame the picker is up. Same call
    // goto_emu_game uses to stop movement during the scene-exit fade.
    mPlib_request_main_refuse_type1(game);

    u16 on_btn = gamePT->pads[PAD0].on.button;
    s8 stick_y = gamePT->pads[PAD0].now.stick_y;

    if (s_picker_cooldown > 0) { s_picker_cooldown--; }

    if (on_btn & BUTTON_A) { vita_nes_picker_close(1); return; }
    if (on_btn & BUTTON_B) { vita_nes_picker_close(0); return; }

    if (s_picker_cooldown == 0) {
        int dir = 0;
        if ((on_btn & BUTTON_DUP)   || stick_y >  40) dir = -1;
        if ((on_btn & BUTTON_DDOWN) || stick_y < -40) dir = +1;
        if (dir) {
            s_picker_sel += dir;
            if (s_picker_sel < 0) s_picker_sel = count - 1;
            if (s_picker_sel >= count) s_picker_sel = 0;
            s_picker_cooldown = 7;
        }
    }
}

// Same overlay style as the title screen options menu (aAL_pc_options_draw):
// fill rect via gDPFillRectangle into NOW_FONT_DISP, then mFont_SetLineStrings
// for text. font_thaga is drawn last in the frame so the panel lands on top.
void vita_nes_picker_draw(GAME* game) {
    if (!s_picker_open) return;
    int count = vita_nes_get_rom_count();
    if (count <= 0) return;

    GRAPH* graph = game->graph;

    // font setup. m_msg.c does the same sequence around its text draws; without
    // this mFont_SetLineStrings emits glyphs into a context that never lands
    // on screen (title menu wraps its entire actor draw with this).
    mFont_SetMatrix(graph, mFont_MODE_FONT);

    // semi-transparent black background covering the whole screen
    {
        Gfx* gfx;
        OPEN_DISP(graph);
        gfx = NOW_FONT_DISP;
        gDPPipeSync(gfx++);
#ifdef TARGET_PC
        gDPNoOpTag(gfx++, PC_NOOP_WIDESCREEN_STRETCH);
#endif
        gDPSetOtherMode(gfx++,
            G_AD_DISABLE | G_CD_MAGICSQ | G_CK_NONE | G_TC_FILT |
            G_TF_POINT | G_TT_NONE | G_TL_TILE | G_TD_CLAMP |
            G_TP_NONE | G_CYC_1CYCLE | G_PM_NPRIMITIVE,
            G_AC_NONE | G_ZS_PRIM | G_RM_XLU_SURF | G_RM_XLU_SURF2);
        gDPSetCombineMode(gfx++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
        gDPSetPrimColor(gfx++, 0, 0, 0, 0, 0, 200);
        gDPFillRectangle(gfx++, 0, 0, SCREEN_WIDTH * 2, SCREEN_HEIGHT * 2);
        gDPPipeSync(gfx++);
#ifdef TARGET_PC
        gDPNoOpTag(gfx++, PC_NOOP_WIDESCREEN_STRETCH_OFF);
#endif
        SET_FONT_DISP(gfx);
        CLOSE_DISP(graph);
    }

    // title
    f32 y = 28.0f;
    f32 line_h = 13.0f;
    {
        static u8 str_title[] = "- Select NES Game -";
        f32 tw = (f32)mFont_GetStringWidth(str_title, sizeof(str_title) - 1, TRUE);
        mFont_SetLineStrings(game, str_title, sizeof(str_title) - 1,
            (SCREEN_WIDTH_F - tw) * 0.5f, y,
            255, 255, 255, 255, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);
    }
    y += line_h * 1.6f;

    // scrollable list, selected row centered when possible
    int visible = 12;
    if (visible > count) visible = count;
    int scroll = s_picker_sel - visible / 2;
    if (scroll > count - visible) scroll = count - visible;
    if (scroll < 0) scroll = 0;

    f32 list_start_y = y;

    if (scroll > 0) {
        static u8 up_dots[] = { '.', '.', '.' };
        f32 ud_scale = 0.7f;
        f32 ud_w = (f32)mFont_GetStringWidth(up_dots, sizeof(up_dots), TRUE) * ud_scale;
        mFont_SetLineStrings(game, up_dots, sizeof(up_dots),
            (SCREEN_WIDTH_F - ud_w) * 0.5f, list_start_y - 6.0f,
            210, 210, 210, 230, FALSE, TRUE, ud_scale, ud_scale, mFont_MODE_FONT);
    }

    for (int i = 0; i < visible && (scroll + i) < count; i++) {
        int row = scroll + i;
        const char* nm = vita_nes_get_rom_name(row);
        int nlen = 0;
        while (nm[nlen] && nlen < 40) nlen++;
        int is_sel = (row == s_picker_sel);
        int bright = is_sel ? 255 : 180;
        int alpha  = is_sel ? 255 : 160;

        f32 nw = (f32)mFont_GetStringWidth((u8*)nm, nlen, TRUE);
        f32 nx = (SCREEN_WIDTH_F - nw) * 0.5f;

        mFont_SetLineStrings(game, (u8*)nm, nlen, nx, y,
            bright, bright, bright, alpha, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);

        if (is_sel) {
            static u8 arrow_l[] = { '>' };
            static u8 arrow_r[] = { '<' };
            mFont_SetLineStrings(game, arrow_l, 1, nx - 12.0f, y,
                255, 255, 255, 255, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);
            mFont_SetLineStrings(game, arrow_r, 1, nx + nw + 4.0f, y,
                255, 255, 255, 255, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);
        }
        y += line_h;
    }

    if (scroll + visible < count) {
        static u8 dn_dots[] = { '.', '.', '.' };
        f32 dd_scale = 0.7f;
        f32 dd_w = (f32)mFont_GetStringWidth(dn_dots, sizeof(dn_dots), TRUE) * dd_scale;
        mFont_SetLineStrings(game, dn_dots, sizeof(dn_dots),
            (SCREEN_WIDTH_F - dd_w) * 0.5f, y + 2.0f,
            210, 210, 210, 230, FALSE, TRUE, dd_scale, dd_scale, mFont_MODE_FONT);
        y += line_h * 0.7f;
    }

    // footer with hint text
    y = SCREEN_HEIGHT_F - line_h * 1.8f;
    {
        static u8 hint[] = "A: play   B: cancel   D-pad or stick: move";
        f32 hw = (f32)mFont_GetStringWidth(hint, sizeof(hint) - 1, TRUE);
        mFont_SetLineStrings(game, hint, sizeof(hint) - 1,
            (SCREEN_WIDTH_F - hw) * 0.5f, y,
            220, 220, 220, 220, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);
    }

    mFont_UnSetMatrix(graph, mFont_MODE_FONT);
}

#endif
