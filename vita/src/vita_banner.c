// vita_banner.c
// pillarbox banner rendering for 4:3 mode

#ifdef TARGET_VITA

#include "pc_gx_internal.h"
#include "pc_settings.h"
#include "vita_shared.h"
#include "vita_banner.h"
#include "vita_gx_cmdbuf.h"
#include "stb_image.h"
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#define BANNER_DIR "ux0:data/AnimalCrossing/banners/"
#define BANNER_NAME_MAX 32

BannerEntry g_banner_list[BANNER_MAX_ENTRIES];
int g_banner_count = 0;

static GLuint g_banner_tex = 0;
static int    g_banner_loaded = 0;
static char   g_banner_loaded_name[BANNER_NAME_MAX] = "";

// Scan banners/ folder for valid PNGs
void banner_scan_folder(void) {
    g_banner_count = 0;

    DIR* dir = opendir(BANNER_DIR);
    if (!dir) {
        fprintf(stderr, "[VITA] banner_scan_folder: cannot open %s (errno=%d)\n", BANNER_DIR, errno);
        return;
    }

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL && g_banner_count < BANNER_MAX_ENTRIES) {
        const char* name = ent->d_name;
        int len = (int)strlen(name);
        if (len < 5 || len >= BANNER_NAME_MAX) continue;

        // Only .png files
        if (strcmp(name + len - 4, ".png") != 0 && strcmp(name + len - 4, ".PNG") != 0)
            continue;

        BannerEntry* e = &g_banner_list[g_banner_count];
        strncpy(e->name, name, BANNER_NAME_MAX - 1);
        e->name[BANNER_NAME_MAX - 1] = '\0';

        // Validate: try to decode the PNG
        char path[128];
        snprintf(path, sizeof(path), "%s%s", BANNER_DIR, name);
        int w, h, ch;
        unsigned char* data = stbi_load(path, &w, &h, &ch, 4);
        if (data) {
            stbi_image_free(data);
            e->valid = 1;
        } else {
            e->valid = 0;
        }

        g_banner_count++;
    }
    closedir(dir);
}

// Get banner index by name. Returns -1 if not found.
int banner_find_index(const char* name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < g_banner_count; i++) {
        if (strcmp(g_banner_list[i].name, name) == 0) return i;
    }
    return -1;
}

static void banner_unload(void) {
    if (!g_banner_loaded) return;
    glDeleteTextures(1, &g_banner_tex);
    g_banner_tex = 0;
    g_banner_loaded = 0;
    g_banner_loaded_name[0] = '\0';
}

static void banner_load_file(const char* name) {
    // Already loaded this one?
    if (g_banner_loaded && strcmp(g_banner_loaded_name, name) == 0) return;

    banner_unload();

    if (!name || !name[0]) return;

    // Check validity
    int idx = banner_find_index(name);
    if (idx < 0 || !g_banner_list[idx].valid) return;

    char path[128];
    snprintf(path, sizeof(path), "%s%s", BANNER_DIR, name);

    int w, h, channels;
    unsigned char* data = stbi_load(path, &w, &h, &channels, 4);
    if (!data) {
        fprintf(stderr, "[VITA] banner_load_file: failed to load %s: %s\n", path, stbi_failure_reason());
        return;
    }

    glGenTextures(1, &g_banner_tex);
    glBindTexture(GL_TEXTURE_2D, g_banner_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    stbi_image_free(data);
    g_banner_loaded = 1;
    strncpy(g_banner_loaded_name, name, BANNER_NAME_MAX - 1);
}

void banner_update(void) {
    if (g_pc_settings.aspect_mode != 1) {
        // widescreen, free banner texture
        banner_unload();
        return;
    }
    // 4:3 mode, load the selected banner or unload if none
    const char* want = g_pc_settings.banner_name;
    if (!want[0]) {
        banner_unload();
    } else {
        banner_load_file(want);
    }
}

// Draw banner in pillarbox bars using the SAME GL pipeline as the game.
// After submit_frame finishes all game draws, we re-upload the VBO with
// banner vertices and call glDrawArrays.
void banner_draw_bars(void) {
    if (!g_banner_loaded || !g_banner_tex) return;
    if (g_pc_settings.aspect_mode != 1 || !g_aspect_active) return;

    int content_w, bar_left;
    vita_get_43_layout(&content_w, &bar_left);
    int bar_right = g_pc_window_w - content_w - bar_left; // absorb rounding remainder
    if (bar_left <= 0) return;

    GLuint prog = vita_get_simple_shader();
    if (!prog) return;

    float W = (float)g_pc_window_w;

    // NDC positions
    // identity matrices mean these go straight to clip space
    // right bar uses bar_right width to close any 1px rounding gap
    float l0 = -1.0f;
    float l1 = (float)bar_left / W * 2.0f - 1.0f;
    float r0 = (float)(g_pc_window_w - bar_right) / W * 2.0f - 1.0f;
    float r1 = 1.0f;

    // Must match PCGXVertex layout exactly: pos(3f) + nrm(3f) + col(4ub) + uv(2f) = 36 bytes
    PCGXVertex verts[12];
    memset(verts, 0, sizeof(verts));

    // Helper to fill a vertex
    #define BV(i, px,py, u,v) do { \
        verts[i].position[0]=px; verts[i].position[1]=py; verts[i].position[2]=0; \
        verts[i].normal[2]=1.0f; \
        verts[i].color0[0]=255; verts[i].color0[1]=255; verts[i].color0[2]=255; verts[i].color0[3]=255; \
        verts[i].texcoord[0][0]=u; verts[i].texcoord[0][1]=v; \
    } while(0)

    // Left bar
    BV(0,  l0,-1, 0,1); BV(1,  l1,-1, 1,1); BV(2,  l1,1, 1,0);
    BV(3,  l0,-1, 0,1); BV(4,  l1, 1, 1,0); BV(5,  l0,1, 0,0);
    // Right bar (mirrored UV)
    BV(6,  r0,-1, 1,1); BV(7,  r1,-1, 0,1); BV(8,  r1,1, 0,0);
    BV(9,  r0,-1, 1,1); BV(10, r1, 1, 0,0); BV(11, r0,1, 1,0);
    #undef BV

    // Re-upload VBO with banner data (game is done with its data)
    glBindBuffer(GL_ARRAY_BUFFER, g_gx.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);

    // Re-set attrib pointers (same layout as game, pointing to VBO offset 0)
    vita_set_vertex_attrib_pointers();

    // Bind shader and set uniforms (locations cached per shader)
    glUseProgram(prog);

    static struct {
        GLuint shader;
        GLint proj, mv, use_tex, tc_src, nchans, tmtx, tgsrc, tex0;
    } s_uloc;

    if (s_uloc.shader != prog) {
        s_uloc.shader  = prog;
        s_uloc.proj    = glGetUniformLocation(prog, "u_projection");
        s_uloc.mv      = glGetUniformLocation(prog, "u_modelview");
        s_uloc.use_tex = glGetUniformLocation(prog, "u_use_texture0");
        s_uloc.tc_src  = glGetUniformLocation(prog, "u_tev0_tc_src");
        s_uloc.nchans  = glGetUniformLocation(prog, "u_num_chans");
        s_uloc.tmtx    = glGetUniformLocation(prog, "u_texmtx_enable");
        s_uloc.tgsrc   = glGetUniformLocation(prog, "u_texgen_src0");
        s_uloc.tex0    = glGetUniformLocation(prog, "u_texture0");
    }

    float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    if (s_uloc.proj >= 0) glUniformMatrix4fv(s_uloc.proj, 1, GL_FALSE, ident);
    if (s_uloc.mv >= 0) glUniformMatrix4fv(s_uloc.mv, 1, GL_FALSE, ident);

    if (s_uloc.use_tex >= 0) glUniform1f(s_uloc.use_tex, 1.0f);
    if (s_uloc.tc_src >= 0) glUniform1f(s_uloc.tc_src, 0.0f);
    if (s_uloc.nchans >= 0) glUniform1f(s_uloc.nchans, 0.0f);

    GLint lte = s_uloc.tmtx;
    GLint ltg = s_uloc.tgsrc;
    if (lte >= 0) glUniform1f(lte, 0.0f);
    if (ltg >= 0) glUniform1f(ltg, 0.0f);

    // Bind banner texture to unit 0
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_banner_tex);
    if (s_uloc.tex0 >= 0) glUniform1i(s_uloc.tex0, 0);

    // State: no depth, no cull, no blend, no scissor
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_FALSE);
    glViewport(0, 0, g_pc_window_w, g_pc_window_h);

    glDrawArrays(GL_TRIANGLES, 0, 12);

    // Restore
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
}

#endif // TARGET_VITA
