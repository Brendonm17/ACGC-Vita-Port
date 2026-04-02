// vita_texpack.c - Scan texture_packs/ folder for VTC files.
// Each .vtc file is a named texture pack; filename (minus extension) = display name.

#ifdef TARGET_VITA

#include "vita_texpack.h"
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#define VTC_MAGIC   0x56544331 // "VTC1"
#define VTC_VERSION 2

TexPackEntry g_texpack_list[TEXPACK_MAX_ENTRIES];
int g_texpack_count = 0;

void texpack_scan_folder(void) {
    g_texpack_count = 0;

    DIR* dir = opendir(TEXPACK_DIR);
    if (!dir) {
        fprintf(stderr, "[VITA] texpack_scan_folder: cannot open %s (errno=%d)\n", TEXPACK_DIR, errno);
        return;
    }

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL && g_texpack_count < TEXPACK_MAX_ENTRIES) {
        const char* name = ent->d_name;
        int len = (int)strlen(name);

        if (len < 5 || len >= TEXPACK_NAME_MAX + 4) continue;
        if (strcmp(name + len - 4, ".vtc") != 0 && strcmp(name + len - 4, ".VTC") != 0)
            continue;

        TexPackEntry* e = &g_texpack_list[g_texpack_count];

        int name_len = len - 4;
        if (name_len >= TEXPACK_NAME_MAX) name_len = TEXPACK_NAME_MAX - 1;
        memcpy(e->name, name, name_len);
        e->name[name_len] = '\0';

        char path[128];
        snprintf(path, sizeof(path), "%s%s", TEXPACK_DIR, name);
        FILE* f = fopen(path, "rb");
        if (f) {
            unsigned int header[4];
            if (fread(header, 4, 4, f) == 4 &&
                header[0] == VTC_MAGIC && header[1] == VTC_VERSION) {
                e->valid = 1;
            } else {
                e->valid = 0;
            }
            fclose(f);
        } else {
            e->valid = 0;
        }

        g_texpack_count++;
    }
    closedir(dir);
}

int texpack_find_index(const char* name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < g_texpack_count; i++) {
        if (strcmp(g_texpack_list[i].name, name) == 0) return i;
    }
    return -1;
}

#endif /* TARGET_VITA */
