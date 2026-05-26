// vita_design_convert.h
// RGBA <-> Animal Crossing CI4 design conversion (pure, no platform deps)
#ifndef VITA_DESIGN_CONVERT_H
#define VITA_DESIGN_CONVERT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VDC_DESIGN_W       32
#define VDC_DESIGN_H       32
#define VDC_TEX_BYTES      512 // 32*32 at 4bpp
#define VDC_NAME_LEN       16
#define VDC_PALETTE_COUNT  16
#define VDC_PALETTE_COLORS 16

typedef struct {
    uint8_t tex[VDC_TEX_BYTES]; // CI4, GC-tiled (matches mDE_POS2TEXEL)
    uint8_t palette;            // 0..15
} vdc_design_t;

// 16 fixed AC palettes, 16 colors each, RGB888 (mirrors m_needlework.c)
extern const uint8_t vdc_palette_rgb[VDC_PALETTE_COUNT][VDC_PALETTE_COLORS][3];

// Any-size RGBA8 -> 32x32 CI4, auto-picking the best-fit palette. dither != 0 = Floyd-Steinberg.
// Returns 0 on success; *out_score (if non-NULL) is the palette's mean squared error (0 = exact).
int vdc_rgba_to_design(const uint8_t* rgba, int w, int h, int dither, vdc_design_t* out, long* out_score);

// Expand a design into 32x32 RGBA8 (out_rgba must hold 32*32*4 bytes, opaque)
void vdc_design_to_rgba(const uint8_t* tex, int palette, uint8_t* out_rgba);

// Encode w*h RGBA8 as PNG into out (cap out_cap); returns bytes written, 0 on error.
// Stored deflate, so only suitable for tiny images (the 32x32 design export).
int vdc_encode_png(const uint8_t* rgba, int w, int h, uint8_t* out, int out_cap);

// CI4 tiled nibble access, identical to the in-game editor addressing
int     vdc_pos_to_byte(int x, int y);
uint8_t vdc_get_index(const uint8_t* tex, int x, int y);
void    vdc_set_index(uint8_t* tex, int x, int y, uint8_t idx);

// Strip path + extension and clean a filename into a printable ASCII stem.
// AC-charset encoding for the stored name is applied by the caller.
void vdc_sanitize_name(const char* filename, char* out, int out_size);

#ifdef __cplusplus
}
#endif

#endif
