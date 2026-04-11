// vita_nes_ppu.c
// reads pixels from agnes, uploads to gl texture, registers as efb capture
// so famicom_draw() displays it through the normal gx path
#ifdef TARGET_VITA

#include "Famicom/ks_nes_common.h"
#include "dolphin/gx.h"
#include "dolphin/mtx.h"
#include "pc_gx_internal.h"
#include "agnes.h"
#include <vitaGL.h>
#include <string.h>

extern void pc_gx_efb_capture_store(u32 dest_ptr, GLuint gl_tex);
extern agnes_t* g_agnes;

// original decomp palette data, referenced by famicom.cpp
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

// gx state setup for famicom_draw() display pass
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

static GLuint nes_tex = 0;

void ksNesDraw(ksNesCommonWorkObj* wp, ksNesStateObj* sp) {
    (void)sp;
    if (!g_agnes) return;

    if (!nes_tex) {
        glGenTextures(1, &nes_tex);
        glBindTexture(GL_TEXTURE_2D, nes_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, KS_NES_WIDTH, KS_NES_HEIGHT, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }

    // read pixels from agnes, v flipped to compensate for efb_v_flip correction
    static u32 upload_buf[KS_NES_WIDTH * KS_NES_HEIGHT];
    for (int y = 0; y < KS_NES_HEIGHT; y++) {
        int src_y = 8 + (KS_NES_HEIGHT - 1 - y);
        u32* row = &upload_buf[y * KS_NES_WIDTH];
        for (int x = 0; x < KS_NES_WIDTH; x++) {
            agnes_color_t c = agnes_get_screen_pixel(g_agnes, x, src_y);
            row[x] = 0xFF000000 | ((u32)c.b << 16) | ((u32)c.g << 8) | c.r;
        }
    }

    glBindTexture(GL_TEXTURE_2D, nes_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, KS_NES_WIDTH, KS_NES_HEIGHT,
                    GL_RGBA, GL_UNSIGNED_BYTE, upload_buf);

    pc_gx_efb_capture_store((u32)(uintptr_t)wp->result_bufp, nes_tex);
}

void vita_nes_ppu_cleanup(void) {
    if (nes_tex) {
        glDeleteTextures(1, &nes_tex);
        nes_tex = 0;
    }
}

#endif
