/* bc7_decompress.h - Minimal BC7 (BPTC) block decompressor
 * Public domain. Single header, no dependencies.
 * Decodes one 4x4 block (16 bytes in, 16 RGBA pixels out). */
#ifndef BC7_DECOMPRESS_H
#define BC7_DECOMPRESS_H

#include <string.h>

/* --- BC7 mode tables --- */
static const int bc7_num_subsets[]  = {3,2,3,2,1,1,1,2};
static const int bc7_part_bits[]    = {4,6,6,6,0,0,0,6};
static const int bc7_rot_bits[]     = {0,0,0,0,2,2,2,0};
static const int bc7_idx_sel_bits[] = {0,0,0,0,1,0,0,0};
static const int bc7_color_bits[]   = {4,6,5,7,5,7,7,5};
static const int bc7_alpha_bits[]   = {0,0,0,0,6,8,7,5};
static const int bc7_ep_pbits[]     = {1,1,0,1,0,0,1,1}; /* per-endpoint pbit */
static const int bc7_sp_pbits[]     = {0,0,1,0,0,0,0,0}; /* shared pbit */
static const int bc7_idx_bits[]     = {3,3,2,2,2,2,4,2};
static const int bc7_idx2_bits[]    = {0,0,0,0,3,2,0,0};

/* Partition tables (64 entries each for 2-subset and 3-subset) */
static const unsigned char bc7_partition2[64][16] = {
    {0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1},{0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1},
    {0,1,1,1,0,1,1,1,0,1,1,1,0,1,1,1},{0,0,0,1,0,0,1,1,0,0,1,1,0,1,1,1},
    {0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,1},{0,0,1,1,0,1,1,1,0,1,1,1,1,1,1,1},
    {0,0,0,1,0,0,1,1,0,1,1,1,1,1,1,1},{0,0,0,0,0,0,0,1,0,0,1,1,0,1,1,1},
    {0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,1},{0,0,1,1,0,1,1,1,1,1,1,1,1,1,1,1},
    {0,0,0,0,0,0,0,1,0,1,1,1,1,1,1,1},{0,0,0,0,0,0,0,0,0,0,0,1,0,1,1,1},
    {0,0,0,1,0,1,1,1,1,1,1,1,1,1,1,1},{0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1},
    {0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1},{0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1},
    {0,0,0,0,1,0,0,0,1,1,1,0,1,1,1,1},{0,1,1,1,0,0,0,1,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,1,0,0,0,1,1,1,0},{0,1,1,1,0,0,1,1,0,0,0,1,0,0,0,0},
    {0,0,1,1,0,0,0,1,0,0,0,0,0,0,0,0},{0,0,0,0,1,0,0,0,1,1,0,0,1,1,1,0},
    {0,0,0,0,0,0,0,0,1,0,0,0,1,1,0,0},{0,1,1,1,0,0,1,1,0,0,1,1,0,0,0,1},
    {0,0,1,1,0,0,0,1,0,0,0,1,0,0,0,0},{0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,0},
    {0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0},{0,0,1,1,0,1,1,0,0,1,1,0,1,1,0,0},
    {0,0,0,1,0,1,1,1,1,1,1,0,1,0,0,0},{0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0},
    {0,1,1,0,0,0,1,1,0,0,1,1,0,1,1,0},{0,0,1,1,1,0,0,1,1,0,0,1,1,1,0,0},
    {0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1},{0,0,0,0,1,1,1,1,0,0,0,0,1,1,1,1},
    {0,1,0,1,1,0,1,0,0,1,0,1,1,0,1,0},{0,0,1,1,0,0,1,1,1,1,0,0,1,1,0,0},
    {0,0,1,1,1,1,0,0,0,0,1,1,1,1,0,0},{0,1,0,1,0,1,0,1,1,0,1,0,1,0,1,0},
    {0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1},{0,1,0,1,1,0,1,0,1,0,1,0,0,1,0,1},
    {0,1,1,1,0,0,1,1,1,1,0,0,1,1,1,0},{0,0,0,1,0,0,1,1,1,1,0,0,1,0,0,0},
    {0,0,1,1,0,0,1,0,0,1,0,0,1,1,0,0},{0,0,1,1,1,0,1,1,1,1,0,1,1,1,0,0},
    {0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0},{0,0,1,1,1,1,0,0,1,1,0,0,0,0,1,1},
    {0,1,1,0,0,1,1,0,1,0,0,1,1,0,0,1},{0,0,0,0,0,1,1,0,0,1,1,0,0,0,0,0},
    {0,1,0,0,1,1,1,0,0,1,0,0,0,0,0,0},{0,0,1,0,0,1,1,1,0,0,1,0,0,0,0,0},
    {0,0,0,0,0,0,1,0,0,1,1,1,0,0,1,0},{0,0,0,0,0,1,0,0,1,1,1,0,0,1,0,0},
    {0,1,1,0,1,1,0,0,1,0,0,1,0,0,1,1},{0,0,1,1,0,1,1,0,1,1,0,0,1,0,0,1},
    {0,1,1,0,0,0,1,1,1,0,0,1,1,1,0,0},{0,0,1,1,1,0,0,1,1,1,0,0,0,1,1,0},
    {0,1,1,0,1,1,0,0,1,1,0,0,0,0,1,1},{0,1,1,0,0,0,1,1,0,0,1,1,1,0,0,1},
    {0,1,1,1,1,1,1,0,1,0,0,0,0,0,0,1},{0,0,0,1,1,0,0,0,1,1,1,0,0,1,1,1},
};
static const unsigned char bc7_partition3[64][16] = {
    {0,0,1,1,0,0,1,1,0,2,2,1,2,2,2,2},{0,0,0,1,0,0,1,1,2,2,1,1,2,2,2,1},
    {0,0,0,0,2,0,0,1,2,2,1,1,2,2,1,1},{0,2,2,2,0,0,2,2,0,0,1,1,0,1,1,1},
    {0,0,0,0,0,0,0,0,1,1,2,2,1,1,2,2},{0,0,1,1,0,0,1,1,0,0,2,2,0,0,2,2},
    {0,0,2,2,0,0,2,2,1,1,1,1,1,1,1,1},{0,0,1,1,0,0,1,1,2,2,1,1,2,2,1,1},
    {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2},{0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2},
    {0,0,0,0,1,1,1,1,2,2,2,2,2,2,2,2},{0,0,1,2,0,0,1,2,0,0,1,2,0,0,1,2},
    {0,1,1,2,0,1,1,2,0,1,1,2,0,1,1,2},{0,1,2,2,0,1,2,2,0,1,2,2,0,1,2,2},
    {0,0,1,1,0,1,1,2,1,1,2,2,1,2,2,2},{0,0,1,1,2,0,0,1,2,2,0,0,2,2,2,0},
    {0,0,0,1,0,0,1,1,0,1,1,2,1,1,2,2},{0,1,1,1,0,0,1,1,2,0,0,1,2,2,0,0},
    {0,0,0,0,1,1,2,2,1,1,2,2,1,1,2,2},{0,0,2,2,0,0,2,2,0,0,2,2,1,1,1,1},
    {0,1,1,1,0,1,1,1,0,2,2,2,0,2,2,2},{0,0,0,1,0,0,0,1,2,2,2,1,2,2,2,1},
    {0,0,0,0,0,0,1,1,0,1,2,2,0,1,2,2},{0,0,0,0,1,1,0,0,2,2,1,0,2,2,1,0},
    {0,1,2,2,0,1,2,2,0,0,1,1,0,0,0,0},{0,0,1,2,0,0,1,2,1,1,2,2,2,2,2,2},
    {0,1,1,0,1,2,2,1,1,2,2,1,0,1,1,0},{0,0,0,0,0,1,1,0,1,2,2,1,1,2,2,1},
    {0,0,2,2,1,1,0,2,1,1,0,2,0,0,2,2},{0,1,1,0,0,1,1,0,2,0,0,2,2,2,2,2},
    {0,0,1,1,0,1,2,2,0,1,2,2,0,0,1,1},{0,0,0,0,2,0,0,0,2,2,1,1,2,2,2,1},
    {0,0,0,0,0,0,0,2,1,1,2,2,1,2,2,2},{0,2,2,2,0,0,2,2,0,0,1,2,0,0,1,1},
    {0,0,1,1,0,0,1,2,0,0,2,2,0,2,2,2},{0,1,2,0,0,1,2,0,0,1,2,0,0,1,2,0},
    {0,0,0,0,1,1,1,1,2,2,2,2,0,0,0,0},{0,1,2,0,1,2,0,1,2,0,1,2,0,1,2,0},
    {0,1,2,0,2,0,1,2,1,2,0,1,0,1,2,0},{0,0,1,1,2,2,0,0,1,1,2,2,0,0,1,1},
    {0,0,1,1,1,1,2,2,2,2,0,0,0,0,1,1},{0,1,0,1,0,1,0,1,2,2,2,2,2,2,2,2},
    {0,0,0,0,0,0,0,0,2,1,2,1,2,1,2,1},{0,0,2,2,1,1,2,2,0,0,2,2,1,1,2,2},
    {0,0,2,2,0,0,1,1,0,0,2,2,0,0,1,1},{0,2,2,0,1,2,2,1,0,2,2,0,1,2,2,1},
    {0,1,0,1,2,2,2,2,2,2,2,2,0,1,0,1},{0,0,0,0,2,1,2,1,2,1,2,1,2,1,2,1},
    {0,1,0,1,0,1,0,1,0,1,0,1,2,2,2,2},{0,2,2,2,0,1,1,1,0,2,2,2,0,1,1,1},
    {0,0,0,2,1,1,1,2,0,0,0,2,1,1,1,2},{0,0,0,0,2,1,1,2,2,1,1,2,2,1,1,2},
    {0,2,2,2,0,1,1,1,0,1,1,1,0,2,2,2},{0,0,0,2,1,1,1,2,1,1,1,2,0,0,0,2},
    {0,1,1,0,0,1,1,0,0,1,1,0,2,2,2,2},{0,0,0,0,0,0,0,0,2,1,1,2,2,1,1,2},
    {0,1,1,0,0,1,1,0,2,2,2,2,2,2,2,2},{0,0,2,2,0,0,1,1,0,0,1,1,0,0,2,2},
    {0,0,2,2,1,1,2,2,1,1,2,2,0,0,2,2},{0,0,0,0,0,0,0,0,0,0,0,0,2,1,1,2},
    {0,0,0,2,0,0,0,1,0,0,0,2,0,0,0,1},{0,2,2,2,1,2,2,2,0,2,2,2,1,2,2,2},
    {0,1,0,1,2,2,2,2,2,2,2,2,2,2,2,2},{0,1,1,1,2,0,1,1,2,2,0,1,2,2,2,0},
};

/* Anchor index for 2nd subset (2-subset modes) */
static const int bc7_anchor2[64] = {
    15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,
    15, 2, 8, 2, 2, 8, 8,15, 2, 8, 2, 2, 8, 8, 2, 2,
    15,15, 6, 8, 2, 8,15,15, 2, 8, 2, 2, 2,15,15, 6,
     6, 2, 6, 8,15,15, 2, 2,15,15,15,15, 3, 6, 6,15,
};
/* Anchor index for 2nd subset (3-subset modes) */
static const int bc7_anchor3a[64] = {
     3, 3,15,15, 8, 3,15,15, 8, 8, 6, 6, 6, 5, 3, 3,
     3, 3, 8,15, 3, 3, 6,10, 5, 8, 8, 6, 8, 5,15,15,
     8,15, 3, 5, 6,10, 8,15,15, 3,15, 5,15,15,15,15,
     3,15, 5, 5, 5, 8, 5,10, 5,10, 8,13,15,12, 3, 3,
};
/* Anchor index for 3rd subset (3-subset modes) */
static const int bc7_anchor3b[64] = {
    15, 8, 8, 3,15,15, 3, 8,15,15,15,15,15,15,15, 8,
    15, 8,15, 3,15, 8,15, 8, 3,15, 6,10,15,15,10, 8,
    15, 3,15,10,10, 8, 9,10, 6,15, 8,15, 3, 6, 6, 8,
    15, 3,15,15,15,15,15,15,15,15,15,15, 3,15,15, 8,
};

/* Weight tables */
static const int bc7_weights2[] = {0,21,43,64};
static const int bc7_weights3[] = {0,9,18,27,37,46,55,64};
static const int bc7_weights4[] = {0,4,9,13,17,21,26,30,34,38,43,47,51,55,60,64};

/* --- Bit reader --- */
typedef struct { const unsigned char* data; int pos; } bc7_bits;

static unsigned int bc7_read(bc7_bits* b, int n) {
    unsigned int val = 0;
    for (int i = 0; i < n; i++) {
        int byte_idx = (b->pos + i) >> 3;
        int bit_idx = (b->pos + i) & 7;
        val |= ((b->data[byte_idx] >> bit_idx) & 1) << i;
    }
    b->pos += n;
    return val;
}

/* --- Interpolation --- */
static int bc7_interp(int e0, int e1, int w, int bits) {
    /* Unquantize endpoints to 8 bits */
    if (bits < 8) {
        e0 = (e0 << (8 - bits)) | (e0 >> (2 * bits - 8));
        e1 = (e1 << (8 - bits)) | (e1 >> (2 * bits - 8));
    }
    return (e0 * (64 - w) + e1 * w + 32) >> 6;
}

/* --- Main BC7 block decoder --- */
static void decode_bc7_block(const unsigned char* src, unsigned char* dst, int stride) {
    bc7_bits bits = { src, 0 };

    /* Determine mode (unary: count trailing zeros) */
    int mode = 0;
    while (mode < 8 && !(src[0] & (1 << mode))) mode++;
    if (mode >= 8) {
        /* Invalid block — fill with black */
        for (int i = 0; i < 4; i++)
            memset(dst + i * stride, 0, 16);
        return;
    }
    bits.pos = mode + 1; /* skip mode bits */

    int ns = bc7_num_subsets[mode];
    int pb = bc7_part_bits[mode];
    int rb = bc7_rot_bits[mode];
    int isb = bc7_idx_sel_bits[mode];
    int cb = bc7_color_bits[mode];
    int ab = bc7_alpha_bits[mode];
    int epb = bc7_ep_pbits[mode];
    int spb = bc7_sp_pbits[mode];
    int ib = bc7_idx_bits[mode];
    int ib2 = bc7_idx2_bits[mode];

    /* Read partition, rotation, index selection */
    int partition = pb ? (int)bc7_read(&bits, pb) : 0;
    int rotation = rb ? (int)bc7_read(&bits, rb) : 0;
    int idx_sel = isb ? (int)bc7_read(&bits, isb) : 0;

    /* Read endpoints: color then alpha */
    int ep[3][2][4]; /* [subset][lo/hi][rgba] */
    memset(ep, 0, sizeof(ep));
    for (int ch = 0; ch < 3; ch++)
        for (int s = 0; s < ns; s++)
            for (int e = 0; e < 2; e++)
                ep[s][e][ch] = (int)bc7_read(&bits, cb);
    if (ab > 0)
        for (int s = 0; s < ns; s++)
            for (int e = 0; e < 2; e++)
                ep[s][e][3] = (int)bc7_read(&bits, ab);
    else
        for (int s = 0; s < ns; s++)
            for (int e = 0; e < 2; e++)
                ep[s][e][3] = (1 << cb) - 1; /* opaque */

    /* P-bits */
    if (epb) {
        for (int s = 0; s < ns; s++)
            for (int e = 0; e < 2; e++) {
                int pbit = (int)bc7_read(&bits, 1);
                for (int ch = 0; ch < 4; ch++) {
                    int nbits = (ch < 3) ? cb : (ab ? ab : cb);
                    ep[s][e][ch] = (ep[s][e][ch] << 1) | pbit;
                }
            }
        /* Adjust bit counts for interpolation */
        cb++; if (ab) ab++;
    } else if (spb) {
        for (int s = 0; s < ns; s++) {
            int pbit = (int)bc7_read(&bits, 1);
            for (int e = 0; e < 2; e++)
                for (int ch = 0; ch < 4; ch++) {
                    int nbits = (ch < 3) ? cb : (ab ? ab : cb);
                    ep[s][e][ch] = (ep[s][e][ch] << 1) | pbit;
                }
        }
        cb++; if (ab) ab++;
    }

    /* Read primary indices */
    int idx[16];
    const int* weights = (ib == 2) ? bc7_weights2 : (ib == 3) ? bc7_weights3 : bc7_weights4;
    for (int i = 0; i < 16; i++) {
        int anchor = 0;
        if (i == 0) anchor = 1;
        else if (ns >= 2 && i == bc7_anchor2[partition]) anchor = 1;
        else if (ns >= 3 && i == bc7_anchor3a[partition]) anchor = 1;
        else if (ns >= 3 && i == bc7_anchor3b[partition]) anchor = 1;
        /* Wait — anchor check is wrong for 3-subset when i==0 is always anchor */
        /* Fix: i==0 is always anchor for subset 0 */
        idx[i] = (int)bc7_read(&bits, ib - anchor);
    }

    /* Read secondary indices (modes 4/5) */
    int idx2[16];
    if (ib2 > 0) {
        const int* weights2 = (ib2 == 2) ? bc7_weights2 : (ib2 == 3) ? bc7_weights3 : bc7_weights4;
        for (int i = 0; i < 16; i++) {
            int anchor = (i == 0) ? 1 : 0;
            idx2[i] = (int)bc7_read(&bits, ib2 - anchor);
        }
    }

    /* Determine partition-to-subset mapping */
    const unsigned char* part_table;
    if (ns == 1) part_table = NULL;
    else if (ns == 2) part_table = bc7_partition2[partition];
    else part_table = bc7_partition3[partition];

    /* Interpolate and output */
    const int* weights1 = weights;
    const int* weights_2 = (ib2 > 0) ? ((ib2 == 2) ? bc7_weights2 : (ib2 == 3) ? bc7_weights3 : bc7_weights4) : NULL;

    for (int i = 0; i < 16; i++) {
        int row = i / 4, col = i % 4;
        int s = part_table ? part_table[i] : 0;

        int ci = idx[i], ai = idx[i];
        const int* cw = weights1;
        const int* aw = weights1;
        if (ib2 > 0) {
            if (idx_sel == 0) { ai = idx2[i]; aw = weights_2; }
            else { ci = idx2[i]; cw = weights_2; }
        }

        int r = bc7_interp(ep[s][0][0], ep[s][1][0], cw[ci], cb);
        int g = bc7_interp(ep[s][0][1], ep[s][1][1], cw[ci], cb);
        int b = bc7_interp(ep[s][0][2], ep[s][1][2], cw[ci], cb);
        int a = bc7_interp(ep[s][0][3], ep[s][1][3], aw[ai], ab ? ab : cb);

        /* Apply rotation */
        if (rotation == 1) { int t = a; a = r; r = t; }
        else if (rotation == 2) { int t = a; a = g; g = t; }
        else if (rotation == 3) { int t = a; a = b; b = t; }

        unsigned char* p = dst + row * stride + col * 4;
        p[0] = (unsigned char)(r > 255 ? 255 : (r < 0 ? 0 : r));
        p[1] = (unsigned char)(g > 255 ? 255 : (g < 0 ? 0 : g));
        p[2] = (unsigned char)(b > 255 ? 255 : (b < 0 ? 0 : b));
        p[3] = (unsigned char)(a > 255 ? 255 : (a < 0 ? 0 : a));
    }
}

#endif /* BC7_DECOMPRESS_H */
