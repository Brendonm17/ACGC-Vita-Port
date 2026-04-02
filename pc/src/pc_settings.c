/* pc_settings.c - runtime settings loaded from settings.ini */
#include "pc_settings.h"
#include "pc_platform.h"

PCSettings g_pc_settings = {
#ifdef TARGET_VITA
    .msaa          = 2,
#else
    .msaa          = 4,
#endif
    .preload_textures = 0,
    .window_width  = PC_SCREEN_WIDTH,
    .window_height = PC_SCREEN_HEIGHT,
    .fullscreen    = 0,
    .vsync         = 0,
#ifdef TARGET_VITA
    .render_scale  = 100,
    .render_w      = 960,
    .render_h      = 544,
    .aspect_mode   = 0,
    .banner_name   = "",
    .multithread   = 1,
    .texture_pack  = "",
#endif
};

#ifdef TARGET_VITA
static const char* SETTINGS_FILE = "ux0:data/AnimalCrossing/settings.ini";
#else
static const char* SETTINGS_FILE = "settings.ini";
#endif

#ifdef TARGET_VITA
static const char* DEFAULT_SETTINGS =
    "# Animal Crossing Vita Settings\n"
    "\n"
    "[Graphics]\n"
    "# render_scale: 100 (native 960x544), 75 (720x408), 50 (480x272)\n"
    "render_scale = 100\n"
    "\n"
    "# msaa: 0 (off), 2 (2x), 4 (4x) - anti-aliasing, costs GPU performance\n"
    "msaa = 2\n"
    "\n"
    "# aspect_mode: 0 = widescreen (16:9), 1 = original (4:3)\n"
    "aspect_mode = 0\n"
    "\n"
    "# banner: filename in ux0:data/AnimalCrossing/banners/ (empty = none)\n"
    "banner = \n"
    "\n"
    "[Performance]\n"
    "# multithread: 1 = emu64 on core 1 for ~2x performance, 0 = single-threaded\n"
    "multithread = 1\n"
    "\n"
    "[Textures]\n"
    "# preload_textures: 0 = off, 1 = load on demand, 2 = preload + cache (recommended)\n"
    "preload_textures = 0\n"
    "\n"
    "# texture_pack: name of .vtc file in ux0:data/AnimalCrossing/texture_packs/ (without .vtc)\n"
    "# Leave empty for no texture pack. Can also be set in-game Options menu.\n"
    "texture_pack = \n";
#else
static const char* DEFAULT_SETTINGS =
    "[Graphics]\n"
    "# Window size (ignored in fullscreen)\n"
    "window_width = 640\n"
    "window_height = 480\n"
    "\n"
    "# 0 = windowed, 1 = fullscreen, 2 = borderless fullscreen\n"
    "fullscreen = 0\n"
    "\n"
    "# Vertical sync: 0 = off, 1 = on\n"
    "vsync = 0\n"
    "\n"
    "# Anti-aliasing samples: 0 = off, 2, 4, or 8\n"
    "msaa = 4\n"
    "\n"
    "[Enhancements]\n"
    "# Preload HD textures at startup: 0 = off (load on demand), 1 = preload, 2 = preload + cache file (fastest)\n"
    "preload_textures = 0\n";
#endif

static const char* skip_ws(const char* s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static void trim_end(char* s) {
    int len = (int)strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' ||
                       s[len-1] == '\r' || s[len-1] == '\n')) {
        s[--len] = '\0';
    }
}

#ifdef TARGET_VITA
static void update_render_dims(int scale) {
    switch (scale) {
        case 75:  g_pc_settings.render_w = 720; g_pc_settings.render_h = 408; break;
        case 50:  g_pc_settings.render_w = 480; g_pc_settings.render_h = 272; break;
        default:  g_pc_settings.render_w = 960; g_pc_settings.render_h = 544; scale = 100; break;
    }
    g_pc_settings.render_scale = scale;
}
#endif

static void apply_setting(const char* key, const char* value) {
    int val = atoi(value);

    if (strcmp(key, "window_width") == 0) {
        if (val >= 640) g_pc_settings.window_width = val;
    } else if (strcmp(key, "window_height") == 0) {
        if (val >= 480) g_pc_settings.window_height = val;
    } else if (strcmp(key, "fullscreen") == 0) {
        if (val >= 0 && val <= 2) g_pc_settings.fullscreen = val;
    } else if (strcmp(key, "vsync") == 0) {
        if (val == 0 || val == 1) g_pc_settings.vsync = val;
    } else if (strcmp(key, "msaa") == 0) {
#ifdef TARGET_VITA
        if (val == 4) g_pc_settings.msaa = 4;
        else if (val == 2) g_pc_settings.msaa = 2;
        else g_pc_settings.msaa = 0;
#else
        if (val == 0 || val == 2 || val == 4 || val == 8)
            g_pc_settings.msaa = val;
#endif
    } else if (strcmp(key, "preload_textures") == 0) {
        if (val >= 0 && val <= 2) g_pc_settings.preload_textures = val;
    }
#ifdef TARGET_VITA
    else if (strcmp(key, "render_scale") == 0) {
        update_render_dims(val);
    } else if (strcmp(key, "aspect_mode") == 0) {
        g_pc_settings.aspect_mode = (val == 1) ? 1 : 0;
    } else if (strcmp(key, "banner") == 0) {
        strncpy(g_pc_settings.banner_name, value, sizeof(g_pc_settings.banner_name) - 1);
        g_pc_settings.banner_name[sizeof(g_pc_settings.banner_name) - 1] = '\0';
    } else if (strcmp(key, "multithread") == 0) {
        g_pc_settings.multithread = (val != 0) ? 1 : 0;
    } else if (strcmp(key, "texture_pack") == 0) {
        strncpy(g_pc_settings.texture_pack, value, sizeof(g_pc_settings.texture_pack) - 1);
        g_pc_settings.texture_pack[sizeof(g_pc_settings.texture_pack) - 1] = '\0';
    }
#endif
}

static void write_defaults(const char* path) {
    FILE* f = fopen(path, "w");
    if (f) {
        fputs(DEFAULT_SETTINGS, f);
        fclose(f);
    }
}

void pc_settings_save(void) {
    FILE* f = fopen(SETTINGS_FILE, "w");
    if (!f) {
        printf("[Settings] Failed to write %s\n", SETTINGS_FILE);
        return;
    }
#ifdef TARGET_VITA
    fprintf(f, "# Animal Crossing Vita Settings\n\n");
    fprintf(f, "[Graphics]\n");
    fprintf(f, "# render_scale: 100 (native 960x544), 75 (720x408), 50 (480x272)\n");
    fprintf(f, "render_scale = %d\n\n", g_pc_settings.render_scale);
    fprintf(f, "# msaa: 0 (off), 2 (2x), 4 (4x) - anti-aliasing, costs GPU performance\n");
    fprintf(f, "msaa = %d\n\n", g_pc_settings.msaa);
    fprintf(f, "# aspect_mode: 0 = widescreen (16:9), 1 = original (4:3)\n");
    fprintf(f, "aspect_mode = %d\n\n", g_pc_settings.aspect_mode);
    fprintf(f, "# banner: filename in ux0:data/AnimalCrossing/banners/ (empty = none)\n");
    fprintf(f, "banner = %s\n\n", g_pc_settings.banner_name);
    fprintf(f, "[Performance]\n");
    fprintf(f, "# multithread: 1 = emu64 on core 1 for ~2x performance, 0 = single-threaded\n");
    fprintf(f, "multithread = %d\n\n", g_pc_settings.multithread);
    fprintf(f, "[Textures]\n");
    fprintf(f, "# texture_pack: name of .vtc file in texture_packs/ (without .vtc), empty = none\n");
    fprintf(f, "texture_pack = %s\n", g_pc_settings.texture_pack);
#else
    fprintf(f, "[Graphics]\n");
    fprintf(f, "# Window size (ignored in fullscreen)\n");
    fprintf(f, "window_width = %d\n", g_pc_settings.window_width);
    fprintf(f, "window_height = %d\n", g_pc_settings.window_height);
    fprintf(f, "\n");
    fprintf(f, "# 0 = windowed, 1 = fullscreen, 2 = borderless fullscreen\n");
    fprintf(f, "fullscreen = %d\n", g_pc_settings.fullscreen);
    fprintf(f, "\n");
    fprintf(f, "# Vertical sync: 0 = off, 1 = on\n");
    fprintf(f, "vsync = %d\n", g_pc_settings.vsync);
    fprintf(f, "\n");
    fprintf(f, "# Anti-aliasing samples: 0 = off, 2, 4, or 8\n");
    fprintf(f, "msaa = %d\n", g_pc_settings.msaa);
    fprintf(f, "\n");
    fprintf(f, "[Enhancements]\n");
    fprintf(f, "# Preload HD textures at startup: 0 = off (load on demand), 1 = preload, 2 = preload + cache file (fastest)\n");
    fprintf(f, "preload_textures = %d\n", g_pc_settings.preload_textures);
#endif
    fclose(f);
    printf("[Settings] Saved %s\n", SETTINGS_FILE);
}

void pc_settings_apply(void) {
    if (!g_pc_window) return;

    if (g_pc_settings.fullscreen == 1) {
        SDL_SetWindowFullscreen(g_pc_window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    } else if (g_pc_settings.fullscreen == 2) {
        SDL_SetWindowFullscreen(g_pc_window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    } else {
        SDL_SetWindowFullscreen(g_pc_window, 0);
        SDL_SetWindowSize(g_pc_window, g_pc_settings.window_width, g_pc_settings.window_height);
        SDL_SetWindowPosition(g_pc_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }

    SDL_GL_SetSwapInterval(g_pc_settings.vsync);
    pc_platform_update_window_size();

    printf("[Settings] Applied: %dx%d fullscreen=%d vsync=%d msaa=%d\n",
           g_pc_settings.window_width, g_pc_settings.window_height,
           g_pc_settings.fullscreen, g_pc_settings.vsync, g_pc_settings.msaa);
}

void pc_settings_load(void) {
    FILE* f = fopen(SETTINGS_FILE, "r");
    if (!f) {
        write_defaults(SETTINGS_FILE);
        printf("[Settings] Created default %s\n", SETTINGS_FILE);
#ifdef TARGET_VITA
        // apply defaults that need computed fields
        update_render_dims(g_pc_settings.render_scale);
#endif
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        const char* p = skip_ws(line);

        if (*p == '#' || *p == ';' || *p == '\0' || *p == '\n' || *p == '[')
            continue;

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = (char*)skip_ws(line);
        trim_end(key);
        char* value = (char*)skip_ws(eq + 1);
        trim_end(value);

        if (*key && *value) {
            apply_setting(key, value);
        }
    }
    fclose(f);

#ifdef TARGET_VITA
    printf("[Settings] Loaded %s: render=%dx%d msaa=%d multithread=%d texpack=%s\n",
           SETTINGS_FILE, g_pc_settings.render_w, g_pc_settings.render_h,
           g_pc_settings.msaa, g_pc_settings.multithread,
           g_pc_settings.texture_pack[0] ? g_pc_settings.texture_pack : "(none)");
#else
    printf("[Settings] Loaded %s: %dx%d fullscreen=%d vsync=%d msaa=%d preload_textures=%d\n",
           SETTINGS_FILE, g_pc_settings.window_width, g_pc_settings.window_height,
           g_pc_settings.fullscreen, g_pc_settings.vsync, g_pc_settings.msaa,
           g_pc_settings.preload_textures);
#endif
}
