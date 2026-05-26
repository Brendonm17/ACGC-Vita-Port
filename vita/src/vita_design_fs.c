// vita_design_fs.c
// scan / import / export of design images under ux0:data/AnimalCrossing/designs/

#ifdef TARGET_VITA

#include "vita_design_fs.h"

#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "stb_image.h"

#define DESIGN_DIR "ux0:data/AnimalCrossing/designs/"

static char g_files[DESIGN_FS_MAX_FILES][DESIGN_FS_NAME_MAX];
static int  g_count = 0;

static int has_image_ext(const char* name) {
    const char* dot = NULL;
    for (const char* p = name; *p; p++) {
        if (*p == '.') {
            dot = p;
        }
    }
    if (!dot) {
        return 0;
    }
    char ext[8];
    int n = 0;
    for (const char* p = dot + 1; *p && n < 7; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') {
            c += 32;
        }
        ext[n++] = c;
    }
    ext[n] = '\0';
    return !strcmp(ext, "png") || !strcmp(ext, "bmp") || !strcmp(ext, "jpg") || !strcmp(ext, "jpeg") ||
           !strcmp(ext, "tga");
}

void design_fs_scan(void) {
    g_count = 0;
    DIR* dir = opendir(DESIGN_DIR);
    if (!dir) {
        fprintf(stderr, "[VITA] design_fs_scan: cannot open %s (errno=%d)\n", DESIGN_DIR, errno);
        return;
    }
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL && g_count < DESIGN_FS_MAX_FILES) {
        const char* name = ent->d_name;
        if (name[0] == '.') {
            continue; // skip ., .., hidden
        }
        if ((int)strlen(name) >= DESIGN_FS_NAME_MAX) {
            continue;
        }
        if (!has_image_ext(name)) {
            continue; // skip subdirs and non-image files
        }
        strncpy(g_files[g_count], name, DESIGN_FS_NAME_MAX - 1);
        g_files[g_count][DESIGN_FS_NAME_MAX - 1] = '\0';
        g_count++;
    }
    closedir(dir);
}

int design_fs_count(void) {
    return g_count;
}

const char* design_fs_filename(int i) {
    if (i < 0 || i >= g_count) {
        return "";
    }
    return g_files[i];
}

int design_fs_import(int i, int dither, vdc_design_t* out, char* name_ascii, int name_cap, long* score) {
    if (i < 0 || i >= g_count || !out) {
        return -1;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s%s", DESIGN_DIR, g_files[i]);

    int w, h, ch;
    unsigned char* data = stbi_load(path, &w, &h, &ch, 4);
    if (!data) {
        fprintf(stderr, "[VITA] design_fs_import: decode failed %s: %s\n", path, stbi_failure_reason());
        return -2;
    }
    int r = vdc_rgba_to_design(data, w, h, dither, out, score);
    stbi_image_free(data);
    if (r != 0) {
        return -3;
    }
    if (name_ascii && name_cap > 0) {
        vdc_sanitize_name(g_files[i], name_ascii, name_cap);
    }
    return 0;
}

static int file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) {
        fclose(f);
        return 1;
    }
    return 0;
}

int design_fs_export(const uint8_t* tex, int palette, const char* name_ascii) {
    if (!tex) {
        return -1;
    }

    char stem[VDC_NAME_LEN];
    vdc_sanitize_name((name_ascii && name_ascii[0]) ? name_ascii : "design", stem, sizeof(stem));

    // exports go in the same folder imports read from, so a save is instantly re-importable
    char path[256];
    snprintf(path, sizeof(path), "%s%s.png", DESIGN_DIR, stem);
    for (int n = 2; file_exists(path) && n < 1000; n++) {
        snprintf(path, sizeof(path), "%s%s %d.png", DESIGN_DIR, stem, n);
    }

    uint8_t rgba[VDC_DESIGN_W * VDC_DESIGN_H * 4];
    vdc_design_to_rgba(tex, palette, rgba);

    uint8_t png[8192];
    int len = vdc_encode_png(rgba, VDC_DESIGN_W, VDC_DESIGN_H, png, sizeof(png));
    if (len <= 0) {
        return -4;
    }

    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[VITA] design_fs_export: cannot write %s (errno=%d)\n", path, errno);
        return -5;
    }
    fwrite(png, 1, (size_t)len, f);
    fclose(f);
    return 0;
}

#endif // TARGET_VITA
