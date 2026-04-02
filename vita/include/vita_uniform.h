// vita_uniform.h
// VitaGL direct uniform bypass wrappers
// all coupling to VitaGL's internal uniform struct lives here
#ifndef VITA_UNIFORM_H
#define VITA_UNIFORM_H

#ifdef TARGET_VITA

#include "pc_platform.h"
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// VitaGL stores uniform data at negative locations as pointers to this struct
typedef struct {
    const void* ptr;
    float* data;
    unsigned int size;
    unsigned char is_frag;
    unsigned char is_vert;
} VitaGLUniform;

// VitaGL dirty flags
// set after writing uniforms
extern unsigned char dirty_vert_unifs;
extern unsigned char dirty_frag_unifs;

// convert negative GL location to VitaGL uniform pointer
static inline VitaGLUniform* vita_uni_ptr(GLint loc) {
    return (VitaGLUniform*)((intptr_t)(-(loc)));
}

// mark all uniforms dirty (call after uniform writes)
static inline void vita_uni_mark_dirty(void) {
    dirty_vert_unifs = 1;
    dirty_frag_unifs = 1;
}

// scalar / vector writers

static inline void vita_uni_1i(GLint loc, int v) {
    vita_uni_ptr(loc)->data[0] = (float)v;
}

static inline void vita_uni_2i(GLint loc, int a, int b) {
    float* d = vita_uni_ptr(loc)->data;
    d[0] = (float)a; d[1] = (float)b;
}

static inline void vita_uni_3i(GLint loc, int a, int b, int c) {
    float* d = vita_uni_ptr(loc)->data;
    d[0] = (float)a; d[1] = (float)b; d[2] = (float)c;
}

static inline void vita_uni_4i(GLint loc, int a, int b, int c, int d_) {
    float* d = vita_uni_ptr(loc)->data;
    d[0] = (float)a; d[1] = (float)b; d[2] = (float)c; d[3] = (float)d_;
}

static inline void vita_uni_1f(GLint loc, float v) {
    vita_uni_ptr(loc)->data[0] = v;
}

// bulk copy

static inline void vita_uni_copy(GLint loc, const void* src, int bytes) {
    __builtin_memcpy(vita_uni_ptr(loc)->data, src, bytes);
}

#ifdef __cplusplus
}
#endif

#endif // TARGET_VITA
#endif // VITA_UNIFORM_H
