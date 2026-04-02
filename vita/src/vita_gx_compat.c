// vita_gx_compat.c
// GL 3.3 -> VitaGL shims and stubs
#ifdef TARGET_VITA

#include <vitaGL.h>
#include <stdio.h>

// stubs for GL features vita doesn't have

void vita_glDrawBuffers(int n, const unsigned int* bufs) {
    (void)n; (void)bufs;
}

void vita_glReadBuffer(unsigned int mode) {
    (void)mode;
}

// vitaGL crashes on glUniform with location -1

void vita_safe_uniform1i(int location, int value) {
    if (location >= 0) glUniform1i(location, value);
}

void vita_safe_uniform1f(int location, float value) {
    if (location >= 0) glUniform1f(location, value);
}

void vita_safe_uniform2f(int location, float x, float y) {
    if (location >= 0) glUniform2f(location, x, y);
}

void vita_safe_uniform3f(int location, float x, float y, float z) {
    if (location >= 0) glUniform3f(location, x, y, z);
}

void vita_safe_uniform4f(int location, float x, float y, float z, float w) {
    if (location >= 0) glUniform4f(location, x, y, z, w);
}

void vita_safe_uniform4fv(int location, int count, const float* value) {
    if (location >= 0) glUniform4fv(location, count, value);
}

void vita_safe_uniform3fv(int location, int count, const float* value) {
    if (location >= 0) glUniform3fv(location, count, value);
}

void vita_safe_uniform2fv(int location, int count, const float* value) {
    if (location >= 0) glUniform2fv(location, count, value);
}

void vita_safe_uniformMatrix4fv(int location, int count, unsigned char transpose, const float* value) {
    if (location >= 0) glUniformMatrix4fv(location, count, transpose, value);
}

void vita_safe_uniformMatrix3fv(int location, int count, unsigned char transpose, const float* value) {
    if (location >= 0) glUniformMatrix3fv(location, count, transpose, value);
}

// glClear() clobbers the active shader in vitaGL
// re-bind after every clear
static unsigned int vita_last_program = 0;

void vita_save_program(unsigned int program) {
    vita_last_program = program;
}

void vita_restore_program_after_clear(void) {
    if (vita_last_program != 0) {
        glUseProgram(vita_last_program);
    }
}

// texture packs: Vita uses VTC (Vita Texture Cache) system instead of PC's DDS/zip loader
// VTC provides pre-built DXT1/DXT5 textures for hardware-accelerated GPU decompression

#include "pc_texture_pack.h"

extern void vita_vtc_init(void);
extern void vita_vtc_shutdown(void);
extern int vita_vtc_active(void);
extern GLuint vita_vtc_lookup(const void* data, int data_size,
                               int w, int h, unsigned int fmt,
                               const void* tlut_data, int tlut_entries, int tlut_is_be,
                               int* out_w, int* out_h);

void pc_texture_pack_init(void) { vita_vtc_init(); }
void pc_texture_pack_preload_all(void) {}
void pc_texture_pack_shutdown(void) { vita_vtc_shutdown(); }

GLuint pc_texture_pack_lookup(const void* data, int data_size,
                              int w, int h, unsigned int fmt,
                              const void* tlut_data, int tlut_entries, int tlut_is_be,
                              int* out_w, int* out_h) {
    return vita_vtc_lookup(data, data_size, w, h, fmt,
                            tlut_data, tlut_entries, tlut_is_be, out_w, out_h);
}

int pc_texture_pack_active(void) { return vita_vtc_active(); }

// async API stubs
void pc_texture_pack_start_async(void) {}
void pc_texture_pack_stop_async(void) {}
int pc_texture_pack_queue_async(unsigned long long dh, unsigned long long th,
                                 unsigned int f, int w, int h,
                                 unsigned int dp, unsigned int cdh,
                                 unsigned int tk, unsigned int tp, unsigned int thk,
                                 unsigned int ws, unsigned int wt, unsigned int mf) {
    (void)dh;(void)th;(void)f;(void)w;(void)h;(void)dp;(void)cdh;
    (void)tk;(void)tp;(void)thk;(void)ws;(void)wt;(void)mf;
    return 0;
}
int pc_texture_pack_process_async(void) { return 0; }

unsigned char* pc_texture_pack_lookup_rgba(const void* data, int data_size,
                                           int w, int h, unsigned int fmt,
                                           const void* tlut_data, int tlut_entries, int tlut_is_be,
                                           int* out_w, int* out_h) {
    (void)data;(void)data_size;(void)w;(void)h;(void)fmt;
    (void)tlut_data;(void)tlut_entries;(void)tlut_is_be;(void)out_w;(void)out_h;
    return NULL;
}

// model viewer stubs
// m_game_dlftbls.c references these in the dispatch table

typedef struct GAME GAME;

void pc_model_viewer_init(GAME* game) {
    (void)game;
}

void pc_model_viewer_cleanup(GAME* game) {
    (void)game;
}

#endif // TARGET_VITA
