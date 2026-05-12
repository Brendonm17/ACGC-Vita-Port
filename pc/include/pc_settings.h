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
    int disable_resetti;
    int nes_aspect;

#ifdef TARGET_VITA
    // Vita graphics
    int render_scale;     // 100/75/50 (%)
    int render_w;         // computed from render_scale
    int render_h;

    // Vita display
    int aspect_mode;      // 0=widescreen, 1=original 4:3
    char banner_name[32]; // banner filename in banners/, empty = none

    // Vita texture pack
    char texture_pack[32]; // VTC filename (without .vtc) in texture_packs/, empty = none

    // Vita gameplay
    int auto_save;        // 0=off, 1=periodic auto-save while in town
    int time_sync;        // 0=off, 1=resync in-game clock to RTC on resume from suspend
    int free_cam;         // 0=classic acre transitions, 1=seamless movement
    int boot_logo;        // 0=skip in-game nintendo logo on boot, 1=show it
    int text_speed;       // 0=slow (~half), 1=normal (vanilla), 2=fast (1 char/frame)
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
