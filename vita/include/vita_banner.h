// vita_banner.h - 4:3 pillarbox banner system interface
#ifndef VITA_BANNER_H
#define VITA_BANNER_H

#ifdef TARGET_VITA

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char name[32];
    int valid;
} BannerEntry;

#define BANNER_MAX_ENTRIES 16

extern BannerEntry g_banner_list[];
extern int g_banner_count;
void banner_scan_folder(void);
int  banner_find_index(const char* name);
void banner_update(void);
void banner_draw_bars(void);

#ifdef __cplusplus
}
#endif

#endif // TARGET_VITA
#endif // VITA_BANNER_H
