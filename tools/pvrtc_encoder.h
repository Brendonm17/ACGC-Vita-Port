/*
 * pvrtc_encoder.h - Minimal PVRTC 4bpp encoder
 *
 * Single-header PVRTC 4bpp texture compressor for PowerVR GPUs (PS Vita).
 * Based on the algorithm described in Simon Fenney's "Texture Compression
 * using Low-Frequency Signal Modulation" (Graphics Hardware 2003) and
 * Jeffrey Lim's pvrtccompressor (BSD license).
 *
 * Usage:
 *   #define PVRTC_ENCODER_IMPLEMENTATION  // in ONE .cpp file
 *   #include "pvrtc_encoder.h"
 *
 *   // Compress RGBA8 to PVRTC 4bpp (texture must be power-of-2 AND square)
 *   int outSize = width * height / 2;
 *   unsigned char* pvrtcData = (unsigned char*)malloc(outSize);
 *   pvrtc_encode_4bpp(rgbaPixels, pvrtcData, width, height);
 *   // Upload with glCompressedTexImage2D(GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG, ...)
 *
 * License: BSD-2-Clause (algorithm from public academic paper + BSD-licensed reference)
 */

#ifndef PVRTC_ENCODER_H
#define PVRTC_ENCODER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Encode RGBA8 pixels to PVRTC 4bpp compressed format.
 *
 * @param rgba_in   Input pixel data, 4 bytes per pixel (R,G,B,A order)
 * @param pvrtc_out Output buffer, must be at least (width * height / 2) bytes
 * @param width     Texture width in pixels (must be power-of-2, >= 8)
 * @param height    Texture height in pixels (must equal width for PVRTC)
 */
void pvrtc_encode_4bpp(const unsigned char* rgba_in, unsigned char* pvrtc_out,
                        int width, int height);

#ifdef __cplusplus
}
#endif

/* ======================================================================== */
/*                          IMPLEMENTATION                                  */
/* ======================================================================== */

#ifdef PVRTC_ENCODER_IMPLEMENTATION

#include <string.h>

/* Morton (Z-order) table: interleaves bits of a single byte */
static const uint16_t pvrtc_morton_table[256] = {
    0x0000, 0x0001, 0x0004, 0x0005, 0x0010, 0x0011, 0x0014, 0x0015,
    0x0040, 0x0041, 0x0044, 0x0045, 0x0050, 0x0051, 0x0054, 0x0055,
    0x0100, 0x0101, 0x0104, 0x0105, 0x0110, 0x0111, 0x0114, 0x0115,
    0x0140, 0x0141, 0x0144, 0x0145, 0x0150, 0x0151, 0x0154, 0x0155,
    0x0400, 0x0401, 0x0404, 0x0405, 0x0410, 0x0411, 0x0414, 0x0415,
    0x0440, 0x0441, 0x0444, 0x0445, 0x0450, 0x0451, 0x0454, 0x0455,
    0x0500, 0x0501, 0x0504, 0x0505, 0x0510, 0x0511, 0x0514, 0x0515,
    0x0540, 0x0541, 0x0544, 0x0545, 0x0550, 0x0551, 0x0554, 0x0555,
    0x1000, 0x1001, 0x1004, 0x1005, 0x1010, 0x1011, 0x1014, 0x1015,
    0x1040, 0x1041, 0x1044, 0x1045, 0x1050, 0x1051, 0x1054, 0x1055,
    0x1100, 0x1101, 0x1104, 0x1105, 0x1110, 0x1111, 0x1114, 0x1115,
    0x1140, 0x1141, 0x1144, 0x1145, 0x1150, 0x1151, 0x1154, 0x1155,
    0x1400, 0x1401, 0x1404, 0x1405, 0x1410, 0x1411, 0x1414, 0x1415,
    0x1440, 0x1441, 0x1444, 0x1445, 0x1450, 0x1451, 0x1454, 0x1455,
    0x1500, 0x1501, 0x1504, 0x1505, 0x1510, 0x1511, 0x1514, 0x1515,
    0x1540, 0x1541, 0x1544, 0x1545, 0x1550, 0x1551, 0x1554, 0x1555,
    0x4000, 0x4001, 0x4004, 0x4005, 0x4010, 0x4011, 0x4014, 0x4015,
    0x4040, 0x4041, 0x4044, 0x4045, 0x4050, 0x4051, 0x4054, 0x4055,
    0x4100, 0x4101, 0x4104, 0x4105, 0x4110, 0x4111, 0x4114, 0x4115,
    0x4140, 0x4141, 0x4144, 0x4145, 0x4150, 0x4151, 0x4154, 0x4155,
    0x4400, 0x4401, 0x4404, 0x4405, 0x4410, 0x4411, 0x4414, 0x4415,
    0x4440, 0x4441, 0x4444, 0x4445, 0x4450, 0x4451, 0x4454, 0x4455,
    0x4500, 0x4501, 0x4504, 0x4505, 0x4510, 0x4511, 0x4514, 0x4515,
    0x4540, 0x4541, 0x4544, 0x4545, 0x4550, 0x4551, 0x4554, 0x4555,
    0x5000, 0x5001, 0x5004, 0x5005, 0x5010, 0x5011, 0x5014, 0x5015,
    0x5040, 0x5041, 0x5044, 0x5045, 0x5050, 0x5051, 0x5054, 0x5055,
    0x5100, 0x5101, 0x5104, 0x5105, 0x5110, 0x5111, 0x5114, 0x5115,
    0x5140, 0x5141, 0x5144, 0x5145, 0x5150, 0x5151, 0x5154, 0x5155,
    0x5400, 0x5401, 0x5404, 0x5405, 0x5410, 0x5411, 0x5414, 0x5415,
    0x5440, 0x5441, 0x5444, 0x5445, 0x5450, 0x5451, 0x5454, 0x5455,
    0x5500, 0x5501, 0x5504, 0x5505, 0x5510, 0x5511, 0x5514, 0x5515,
    0x5540, 0x5541, 0x5544, 0x5545, 0x5550, 0x5551, 0x5554, 0x5555
};

/*
 * Bilinear interpolation weights for each pixel position in a 4x4 block.
 * 4 weights per pixel (for the 4 neighboring block centers), summing to 16.
 * Indexed by py*4 + px.
 */
static const int pvrtc_bilinear_factors[16][4] = {
    { 4,  4,  4,  4},  /* (0,0) */
    { 2,  6,  2,  6},  /* (1,0) */
    { 8,  0,  8,  0},  /* (2,0) */
    { 6,  2,  6,  2},  /* (3,0) */
    { 2,  2,  6,  6},  /* (0,1) */
    { 1,  3,  3,  9},  /* (1,1) */
    { 4,  0, 12,  0},  /* (2,1) */
    { 3,  1,  9,  3},  /* (3,1) */
    { 8,  8,  0,  0},  /* (0,2) */
    { 4, 12,  0,  0},  /* (1,2) */
    {16,  0,  0,  0},  /* (2,2) */
    {12,  4,  0,  0},  /* (3,2) */
    { 6,  6,  2,  2},  /* (0,3) */
    { 3,  9,  1,  3},  /* (1,3) */
    {12,  0,  4,  0},  /* (2,3) */
    { 9,  3,  3,  1},  /* (3,3) */
};

/* 8-byte PVRTC block (little-endian) */
typedef struct {
    uint32_t modulation;  /* 2-bit per pixel modulation, bits 0-31 */
    uint32_t color_data;  /* colors + flags, bits 32-63 */
} pvrtc_block_t;

/* Decoded block colors in 8-bit RGBA */
typedef struct {
    uint8_t r, g, b, a;
} pvrtc_color_t;

/* --- Morton index --- */
static uint32_t pvrtc_morton(int bx, int by) {
    return ((uint32_t)pvrtc_morton_table[bx & 0xFF] << 1) |
            (uint32_t)pvrtc_morton_table[by & 0xFF];
}

/* --- Color A: RGB 5-5-4 (opaque), floor-quantized --- */
static uint16_t pvrtc_encode_color_a(int r, int g, int b) {
    /* Floor quantize to 5-5-4 bits */
    int r5 = (r * 31) / 255;
    int g5 = (g * 31) / 255;
    int b4 = (b * 15) / 255;
    /* Pack: bits [13:9]=R, [8:4]=G, [3:0]=B */
    return (uint16_t)((r5 << 9) | (g5 << 4) | b4);
}

/* --- Color B: RGB 5-5-5 (opaque), ceil-quantized --- */
static uint16_t pvrtc_encode_color_b(int r, int g, int b) {
    /* Ceil quantize to 5-5-5 bits */
    int r5 = (r * 31 + 254) / 255;
    int g5 = (g * 31 + 254) / 255;
    int b5 = (b * 31 + 254) / 255;
    if (r5 > 31) r5 = 31;
    if (g5 > 31) g5 = 31;
    if (b5 > 31) b5 = 31;
    /* Pack: bits [14:10]=R, [9:5]=G, [4:0]=B */
    return (uint16_t)((r5 << 10) | (g5 << 5) | b5);
}

/* --- Decode Color A (RGB554 opaque) to 8-bit --- */
static pvrtc_color_t pvrtc_decode_color_a(uint16_t packed) {
    pvrtc_color_t c;
    int r5 = (packed >> 9) & 0x1F;
    int g5 = (packed >> 4) & 0x1F;
    int b4 = packed & 0x0F;
    c.r = (uint8_t)((r5 << 3) | (r5 >> 2));
    c.g = (uint8_t)((g5 << 3) | (g5 >> 2));
    c.b = (uint8_t)((b4 << 4) | b4);
    c.a = 255;
    return c;
}

/* --- Decode Color B (RGB555 opaque) to 8-bit --- */
static pvrtc_color_t pvrtc_decode_color_b(uint16_t packed) {
    pvrtc_color_t c;
    int r5 = (packed >> 10) & 0x1F;
    int g5 = (packed >> 5) & 0x1F;
    int b5 = packed & 0x1F;
    c.r = (uint8_t)((r5 << 3) | (r5 >> 2));
    c.g = (uint8_t)((g5 << 3) | (g5 >> 2));
    c.b = (uint8_t)((b5 << 3) | (b5 >> 2));
    c.a = 255;
    return c;
}

/* --- Extract Color A from block's color_data word --- */
static uint16_t pvrtc_get_color_a(const pvrtc_block_t* blk) {
    return (uint16_t)((blk->color_data >> 1) & 0x3FFF);  /* bits 1-14 */
}

/* --- Extract Color B from block's color_data word --- */
static uint16_t pvrtc_get_color_b(const pvrtc_block_t* blk) {
    return (uint16_t)((blk->color_data >> 16) & 0x7FFF);  /* bits 16-30 */
}

/* --- Pack a block's color_data word --- */
static uint32_t pvrtc_pack_color_data(uint16_t colorA, uint16_t colorB) {
    /*
     * Bit layout of color_data (uint32):
     * bit 0:     usePunchthroughAlpha = 0
     * bits 1-14: colorA (14 bits)
     * bit 15:    colorAIsOpaque = 1
     * bits 16-30: colorB (15 bits)
     * bit 31:    colorBIsOpaque = 1
     */
    uint32_t w = 0;
    w |= ((uint32_t)colorA & 0x3FFF) << 1;   /* colorA at bits 1-14 */
    w |= (1u << 15);                           /* colorAIsOpaque = 1 */
    w |= ((uint32_t)colorB & 0x7FFF) << 16;   /* colorB at bits 16-30 */
    w |= (1u << 31);                           /* colorBIsOpaque = 1 */
    return w;
}

static int pvrtc_clamp_block(int v, int max_v) {
    if (v < 0) return 0;
    if (v > max_v) return max_v;
    return v;
}

void pvrtc_encode_4bpp(const unsigned char* rgba_in, unsigned char* pvrtc_out,
                        int width, int height)
{
    int bw = width / 4;   /* number of blocks horizontally */
    int bh = height / 4;  /* number of blocks vertically */
    int total_blocks = bw * bh;

    pvrtc_block_t* blocks = (pvrtc_block_t*)pvrtc_out;

    /* Clear output */
    memset(pvrtc_out, 0, (size_t)total_blocks * 8);

    /*
     * Pass 1: For each block, compute bounding box of its 4x4 pixels.
     *         Set colorA = floor-quantized min, colorB = ceil-quantized max.
     */
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            int minR = 255, minG = 255, minB = 255;
            int maxR = 0, maxG = 0, maxB = 0;

            for (int py = 0; py < 4; py++) {
                for (int px = 0; px < 4; px++) {
                    int idx = ((by * 4 + py) * width + (bx * 4 + px)) * 4;
                    int r = rgba_in[idx + 0];
                    int g = rgba_in[idx + 1];
                    int b = rgba_in[idx + 2];
                    if (r < minR) minR = r;
                    if (g < minG) minG = g;
                    if (b < minB) minB = b;
                    if (r > maxR) maxR = r;
                    if (g > maxG) maxG = g;
                    if (b > maxB) maxB = b;
                }
            }

            uint16_t ca = pvrtc_encode_color_a(minR, minG, minB);
            uint16_t cb = pvrtc_encode_color_b(maxR, maxG, maxB);

            uint32_t morton_idx = pvrtc_morton(bx, by);
            blocks[morton_idx].modulation = 0;
            blocks[morton_idx].color_data = pvrtc_pack_color_data(ca, cb);
        }
    }

    /*
     * Pass 2: For each pixel, compute its bilinearly-interpolated endpoints
     *         from the 4 surrounding blocks, then choose the best 2-bit
     *         modulation index.
     */
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            uint32_t morton_idx = pvrtc_morton(bx, by);
            uint32_t mod_word = 0;

            for (int py = 0; py < 4; py++) {
                for (int px = 0; px < 4; px++) {
                    /* Determine which 4 blocks surround this pixel */
                    int xOff = (px < 2) ? -1 : 0;
                    int yOff = (py < 2) ? -1 : 0;

                    int n0x = pvrtc_clamp_block(bx + xOff,     bw - 1);
                    int n0y = pvrtc_clamp_block(by + yOff,     bh - 1);
                    int n1x = pvrtc_clamp_block(bx + xOff + 1, bw - 1);
                    int n1y = pvrtc_clamp_block(by + yOff,     bh - 1);
                    int n2x = pvrtc_clamp_block(bx + xOff,     bw - 1);
                    int n2y = pvrtc_clamp_block(by + yOff + 1, bh - 1);
                    int n3x = pvrtc_clamp_block(bx + xOff + 1, bw - 1);
                    int n3y = pvrtc_clamp_block(by + yOff + 1, bh - 1);

                    const pvrtc_block_t* p0 = &blocks[pvrtc_morton(n0x, n0y)];
                    const pvrtc_block_t* p1 = &blocks[pvrtc_morton(n1x, n1y)];
                    const pvrtc_block_t* p2 = &blocks[pvrtc_morton(n2x, n2y)];
                    const pvrtc_block_t* p3 = &blocks[pvrtc_morton(n3x, n3y)];

                    int fi = py * 4 + px;
                    const int* f = pvrtc_bilinear_factors[fi];

                    /* Decode the 4 neighbors' Color A and Color B */
                    pvrtc_color_t ca0 = pvrtc_decode_color_a(pvrtc_get_color_a(p0));
                    pvrtc_color_t ca1 = pvrtc_decode_color_a(pvrtc_get_color_a(p1));
                    pvrtc_color_t ca2 = pvrtc_decode_color_a(pvrtc_get_color_a(p2));
                    pvrtc_color_t ca3 = pvrtc_decode_color_a(pvrtc_get_color_a(p3));

                    pvrtc_color_t cb0 = pvrtc_decode_color_b(pvrtc_get_color_b(p0));
                    pvrtc_color_t cb1 = pvrtc_decode_color_b(pvrtc_get_color_b(p1));
                    pvrtc_color_t cb2 = pvrtc_decode_color_b(pvrtc_get_color_b(p2));
                    pvrtc_color_t cb3 = pvrtc_decode_color_b(pvrtc_get_color_b(p3));

                    /* Bilinear interpolation (scale 16) */
                    int caR = ca0.r * f[0] + ca1.r * f[1] + ca2.r * f[2] + ca3.r * f[3];
                    int caG = ca0.g * f[0] + ca1.g * f[1] + ca2.g * f[2] + ca3.g * f[3];
                    int caB = ca0.b * f[0] + ca1.b * f[1] + ca2.b * f[2] + ca3.b * f[3];

                    int cbR = cb0.r * f[0] + cb1.r * f[1] + cb2.r * f[2] + cb3.r * f[3];
                    int cbG = cb0.g * f[0] + cb1.g * f[1] + cb2.g * f[2] + cb3.g * f[3];
                    int cbB = cb0.b * f[0] + cb1.b * f[1] + cb2.b * f[2] + cb3.b * f[3];
                    /* Note: all values are in [0, 255*16] = [0, 4080] range */

                    /* Source pixel (scaled to match) */
                    int pidx = ((by * 4 + py) * width + (bx * 4 + px)) * 4;
                    int pR = rgba_in[pidx + 0] * 16;
                    int pG = rgba_in[pidx + 1] * 16;
                    int pB = rgba_in[pidx + 2] * 16;

                    /* Vector from ca_interp to pixel */
                    int vR = pR - caR;
                    int vG = pG - caG;
                    int vB = pB - caB;

                    /* Direction: cb_interp - ca_interp */
                    int dR = cbR - caR;
                    int dG = cbG - caG;
                    int dB = cbB - caB;

                    /* Project pixel onto A→B line */
                    int v_dot_d = vR * dR + vG * dG + vB * dB;
                    int d_dot_d = dR * dR + dG * dG + dB * dB;

                    /*
                     * Modulation thresholds (×16 to avoid division):
                     *   index 0: proj < 3/16 of line length²
                     *   index 1: proj < 8/16 of line length²
                     *   index 2: proj < 13/16 of line length²
                     *   index 3: otherwise
                     */
                    int mod_idx;
                    if (d_dot_d == 0) {
                        mod_idx = 0;  /* degenerate: A == B */
                    } else {
                        int proj16 = v_dot_d * 16;
                        if      (proj16 <  3 * d_dot_d) mod_idx = 0;
                        else if (proj16 <  8 * d_dot_d) mod_idx = 1;
                        else if (proj16 < 13 * d_dot_d) mod_idx = 2;
                        else                            mod_idx = 3;
                    }

                    int bit_pos = fi * 2;
                    mod_word |= ((uint32_t)mod_idx << bit_pos);
                }
            }

            blocks[morton_idx].modulation = mod_word;
        }
    }
}

#endif /* PVRTC_ENCODER_IMPLEMENTATION */
#endif /* PVRTC_ENCODER_H */
