#ifndef VITA_TEXPACK_H
#define VITA_TEXPACK_H

#ifdef __cplusplus
extern "C" {
#endif

#define TEXPACK_MAX_ENTRIES 16
#define TEXPACK_NAME_MAX 32
#define TEXPACK_DIR "ux0:data/AnimalCrossing/texture_packs/"

typedef struct {
    char name[TEXPACK_NAME_MAX];
    int valid;
} TexPackEntry;

extern TexPackEntry g_texpack_list[TEXPACK_MAX_ENTRIES];
extern int g_texpack_count;

void texpack_scan_folder(void);
int texpack_find_index(const char* name);

#ifdef __cplusplus
}
#endif

#endif // VITA_TEXPACK_H
