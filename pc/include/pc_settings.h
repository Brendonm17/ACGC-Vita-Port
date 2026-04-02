#ifndef PC_SETTINGS_H
#define PC_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int msaa;
    int preload_textures;

    int window_width;
    int window_height;
    int fullscreen;
    int vsync;

#ifdef TARGET_VITA
    // Vita graphics
    int render_scale;     // 100/75/50 (%)
    int render_w;         // computed from render_scale
    int render_h;

    // Vita display
    int aspect_mode;      // 0=widescreen, 1=original 4:3
    char banner_name[32]; // banner filename in banners/, empty = none

    // Vita performance
    int multithread;      // 1=worker on core 1, 0=single-threaded

    // Vita texture pack
    char texture_pack[32]; // VTC filename (without .vtc) in texture_packs/, empty = none
#endif
} PCSettings;

extern PCSettings g_pc_settings;

void pc_settings_load(void);
void pc_settings_save(void);
void pc_settings_apply(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_SETTINGS_H */
