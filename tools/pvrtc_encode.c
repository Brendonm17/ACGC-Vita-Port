/*
 * pvrtc_encode.c — CLI tool for PVRTC 4bpp encoding
 *
 * Usage: pvrtc_encode <width> <height>
 *   Reads raw RGBA (4 bytes/pixel) from stdin.
 *   Writes PVRTC 4bpp compressed data to stdout.
 *   Width must equal height (square), both must be power-of-2, >= 8.
 *   Output size: width * height / 2 bytes.
 *
 * Build:
 *   gcc -O2 -o pvrtc_encode pvrtc_encode.c
 *   cl.exe /O2 /Fe:pvrtc_encode.exe pvrtc_encode.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#define PVRTC_ENCODER_IMPLEMENTATION
#include "pvrtc_encoder.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <width> <height>\n", argv[0]);
        fprintf(stderr, "  Reads RGBA from stdin, writes PVRTC 4bpp to stdout.\n");
        fprintf(stderr, "  Width must equal height, both power-of-2, >= 8.\n");
        return 1;
    }

    int width = atoi(argv[1]);
    int height = atoi(argv[2]);

    if (width != height || width < 8 || (width & (width - 1)) != 0) {
        fprintf(stderr, "Error: dimensions must be equal, power-of-2, >= 8 (got %dx%d)\n",
                width, height);
        return 1;
    }

#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    int rgba_size = width * height * 4;
    unsigned char* rgba = (unsigned char*)malloc(rgba_size);
    if (!rgba) {
        fprintf(stderr, "Error: failed to allocate %d bytes for RGBA\n", rgba_size);
        return 1;
    }

    /* Read RGBA from stdin */
    int total_read = 0;
    while (total_read < rgba_size) {
        size_t n = fread(rgba + total_read, 1, rgba_size - total_read, stdin);
        if (n == 0) break;
        total_read += (int)n;
    }
    if (total_read != rgba_size) {
        fprintf(stderr, "Error: expected %d bytes, got %d\n", rgba_size, total_read);
        free(rgba);
        return 1;
    }

    int pvrtc_size = width * height / 2;
    unsigned char* pvrtc = (unsigned char*)malloc(pvrtc_size);
    if (!pvrtc) {
        fprintf(stderr, "Error: failed to allocate %d bytes for PVRTC\n", pvrtc_size);
        free(rgba);
        return 1;
    }

    pvrtc_encode_4bpp(rgba, pvrtc, width, height);

    fwrite(pvrtc, 1, pvrtc_size, stdout);

    free(rgba);
    free(pvrtc);
    return 0;
}
