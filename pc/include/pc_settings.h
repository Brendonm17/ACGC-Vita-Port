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

    // where the disc image is: a folder or the image file, empty = the default folders
    char rom_path[256];

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
    int text_speed;       // 0=slow (~half), 1=normal (vanilla), 2=fast (~2 chars/frame over plain text)

    // Which folder is treated as home: 0=card_a (default), 1=card_b. Read
    // at boot; save+restart to switch.
    int save_slot;

    // Vita online: the host's rules for its shared town (visitors go by the host's)
    int mp_visitor_rights; // 1=visitors can do what residents do (museum, bank...), 0=original visitor rules
    int mp_visitor_items;   // 1=visitors can pick up, drop, dig up, bury and plant things
    int mp_visitor_dig;     // 1=visitors can dig holes
    int mp_visitor_axe;     // 1=visitors can cut down trees
    int mp_visitor_tune;    // 1=visitors can change the town tune
    int mp_visitor_board;   // 1=visitors can post on the bulletin board
    int mp_visitor_cottage; // 1=visitors can rearrange the island cottage
    int mp_visitor_designs; // 1=visitors can change the Able Sisters' displays and the island flag
    int mp_ask_join;        // 1=ask before anyone joins (far-away callers are always asked about)
    int mp_chat;            // 1=players chat in your town (visitors go by the host's)
    int mp_chat_keyboard;   // 0=the game's own keyboard, 1=the Vita's
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
