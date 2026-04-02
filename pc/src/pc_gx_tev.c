/* pc_gx_tev.c - TEV shader: GLSL program loading and uniform upload */
#include "pc_gx_internal.h"
#include <dolphin/gx/GXEnum.h>
#include <stdarg.h>

#ifdef TARGET_VITA
#include "vita_shared.h"
extern int g_pc_verbose;
#ifndef VITA_DEBUG_RENDERING
static const int vita_force_uber = 0;
#else
int vita_force_uber = 0;
#endif

// set bit N to disable cfgN, falls through to uber
static unsigned long long vita_cfg_disable = 0;
#define VITA_CFG_DISABLED(n) (vita_cfg_disable & (1ULL << (n)))

const char* vita_get_shader_type_name(int type) {
    (void)type;
    return "";
}

void vita_log(const char* fmt, ...) {
#ifdef VITA_PERF_LOG
    FILE* f = fopen("ux0:data/AnimalCrossing/ac_perf.txt", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
#else
    (void)fmt;
#endif
}
#endif

/* --- file I/O --- */

static char* load_text_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len <= 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);

    char* buf = (char*)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t read = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (read != (size_t)len) {
        fprintf(stderr, "WARNING: Partial read of %s (got %zu of %ld bytes)\n", path, read, len);
        free(buf);
        return NULL;
    }
    buf[read] = '\0';
    return buf;
}

static char* load_shader(const char* filename) {
    char path[512];
#ifdef TARGET_VITA
    // bundled in VPK at app0:shaders/
    snprintf(path, sizeof(path), "app0:shaders/%s", filename);
#else
    snprintf(path, sizeof(path), "shaders/%s", filename);
#endif
    char* src = load_text_file(path);
    if (src) {
        printf("[TEV] Loaded shader: %s\n", path);
    } else {
        fprintf(stderr, "FATAL: Could not load shader: %s\n", path);
    }
    return src;
}

static GLuint compile_shader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "FATAL: Shader compile error: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint link_program(GLuint vert, GLuint frag) {
    if (!vert || !frag) {
        fprintf(stderr, "FATAL: Cannot link program -shader compilation failed\n");
        if (vert) glDeleteShader(vert);
        if (frag) glDeleteShader(frag);
        return 0;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);

    glBindAttribLocation(prog, 0, "a_position");
    glBindAttribLocation(prog, 1, "a_normal");
    glBindAttribLocation(prog, 2, "a_color0");
    glBindAttribLocation(prog, 3, "a_texcoord0");

    glLinkProgram(prog);

    GLint success;
    glGetProgramiv(prog, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "FATAL: Program link error: %s\n", log);
        glDeleteProgram(prog);
        prog = 0;
    }

    glDeleteShader(vert);
    glDeleteShader(frag);
    return prog;
}

// Vita: pre-compiled shader variants (L/F/A/TEV2 permutations)

#ifdef TARGET_VITA

#include "vita_gxp_shaders.h"

#define VITA_VF_LIGHTING   (1 << 0)
#define VITA_VF_FOG        (1 << 1)
#define VITA_VF_ALPHA_TEST (1 << 2)
#define VITA_VF_TEV2       (1 << 3)
#define VITA_VF_COUNT      16
#define VITA_SIMPLE_COUNT  8

static GLuint vita_variants[VITA_VF_COUNT];
static GLuint vita_simple[VITA_SIMPLE_COUNT];

GLuint vita_get_simple_shader(void) { return vita_simple[0]; }

// specialized shaders for top TEV configs
#define VITA_SPEC_COUNT 8  // L/F/A = 3 bits = 8 variants each
#define VITA_CFG_COUNT  40

typedef struct {
    const char* name;
    GLuint programs[VITA_SPEC_COUNT];
    const unsigned char** variants;
    const unsigned int* sizes;
    int available;
} VitaCfgDesc;

// default unavailable configs to 0 (vita_gxp_shaders.h sets available ones to 1)
#ifndef VITA_HAS_CFG4
#define VITA_HAS_CFG4  0
#endif
#ifndef VITA_HAS_CFG5
#define VITA_HAS_CFG5  0
#endif
#ifndef VITA_HAS_CFG6
#define VITA_HAS_CFG6  0
#endif
#ifndef VITA_HAS_CFG7
#define VITA_HAS_CFG7  0
#endif
#ifndef VITA_HAS_CFG8
#define VITA_HAS_CFG8  0
#endif
#ifndef VITA_HAS_CFG9
#define VITA_HAS_CFG9  0
#endif
#ifndef VITA_HAS_CFG10
#define VITA_HAS_CFG10 0
#endif
#ifndef VITA_HAS_CFG11
#define VITA_HAS_CFG11 0
#endif
#ifndef VITA_HAS_CFG12
#define VITA_HAS_CFG12 0
#endif
#ifndef VITA_HAS_CFG13
#define VITA_HAS_CFG13 0
#endif
#ifndef VITA_HAS_CFG14
#define VITA_HAS_CFG14 0
#endif
#ifndef VITA_HAS_CFG15
#define VITA_HAS_CFG15 0
#endif
#ifndef VITA_HAS_CFG16
#define VITA_HAS_CFG16 0
#endif
#ifndef VITA_HAS_CFG17
#define VITA_HAS_CFG17 0
#endif
#ifndef VITA_HAS_CFG18
#define VITA_HAS_CFG18 0
#endif
#ifndef VITA_HAS_CFG19
#define VITA_HAS_CFG19 0
#endif
#ifndef VITA_HAS_CFG20
#define VITA_HAS_CFG20 0
#endif
#ifndef VITA_HAS_CFG21
#define VITA_HAS_CFG21 0
#endif
#ifndef VITA_HAS_CFG22
#define VITA_HAS_CFG22 0
#endif
#ifndef VITA_HAS_CFG23
#define VITA_HAS_CFG23 0
#endif
#ifndef VITA_HAS_CFG24
#define VITA_HAS_CFG24 0
#endif
#ifndef VITA_HAS_CFG25
#define VITA_HAS_CFG25 0
#endif
#ifndef VITA_HAS_CFG26
#define VITA_HAS_CFG26 0
#endif
#ifndef VITA_HAS_CFG27
#define VITA_HAS_CFG27 0
#endif
#ifndef VITA_HAS_CFG28
#define VITA_HAS_CFG28 0
#endif
#ifndef VITA_HAS_CFG29
#define VITA_HAS_CFG29 0
#endif
#ifndef VITA_HAS_CFG30
#define VITA_HAS_CFG30 0
#endif
#ifndef VITA_HAS_CFG31
#define VITA_HAS_CFG31 0
#endif
#ifndef VITA_HAS_CFG32
#define VITA_HAS_CFG32 0
#endif
#ifndef VITA_HAS_CFG33
#define VITA_HAS_CFG33 0
#endif
#ifndef VITA_HAS_CFG34
#define VITA_HAS_CFG34 0
#endif
#ifndef VITA_HAS_CFG35
#define VITA_HAS_CFG35 0
#endif
#ifndef VITA_HAS_CFG36
#define VITA_HAS_CFG36 0
#endif
#ifndef VITA_HAS_CFG37
#define VITA_HAS_CFG37 0
#endif
#ifndef VITA_HAS_CFG38
#define VITA_HAS_CFG38 0
#endif
#ifndef VITA_HAS_CFG39
#define VITA_HAS_CFG39 0
#endif

// dummy data for unavailable configs
static const unsigned char* vita_gxp_dummy_variants[8] = {0};
static const unsigned int vita_gxp_dummy_sizes[8] = {0};

// Stub symbols for removed cfg27-35 so CFG_ENTRY ternary compiles
#if !VITA_HAS_CFG27
#define gxp_cfg27_variants vita_gxp_dummy_variants
#define gxp_cfg27_variant_sizes vita_gxp_dummy_sizes
#endif
#if !VITA_HAS_CFG28
#define gxp_cfg28_variants vita_gxp_dummy_variants
#define gxp_cfg28_variant_sizes vita_gxp_dummy_sizes
#endif
#if !VITA_HAS_CFG29
#define gxp_cfg29_variants vita_gxp_dummy_variants
#define gxp_cfg29_variant_sizes vita_gxp_dummy_sizes
#endif
#if !VITA_HAS_CFG30
#define gxp_cfg30_variants vita_gxp_dummy_variants
#define gxp_cfg30_variant_sizes vita_gxp_dummy_sizes
#endif
#if !VITA_HAS_CFG31
#define gxp_cfg31_variants vita_gxp_dummy_variants
#define gxp_cfg31_variant_sizes vita_gxp_dummy_sizes
#endif
#if !VITA_HAS_CFG32
#define gxp_cfg32_variants vita_gxp_dummy_variants
#define gxp_cfg32_variant_sizes vita_gxp_dummy_sizes
#endif
#if !VITA_HAS_CFG33
#define gxp_cfg33_variants vita_gxp_dummy_variants
#define gxp_cfg33_variant_sizes vita_gxp_dummy_sizes
#endif
#if !VITA_HAS_CFG34
#define gxp_cfg34_variants vita_gxp_dummy_variants
#define gxp_cfg34_variant_sizes vita_gxp_dummy_sizes
#endif
#if !VITA_HAS_CFG35
#define gxp_cfg35_variants vita_gxp_dummy_variants
#define gxp_cfg35_variant_sizes vita_gxp_dummy_sizes
#endif

#define CFG_ENTRY(n, avail) { "CFG" #n, {0}, \
    avail ? (const unsigned char**)gxp_cfg##n##_variants : vita_gxp_dummy_variants, \
    avail ? (const unsigned int*)gxp_cfg##n##_variant_sizes : vita_gxp_dummy_sizes, \
    avail }

static VitaCfgDesc vita_cfgs[VITA_CFG_COUNT] = {
    CFG_ENTRY(0, 1),
    CFG_ENTRY(2, 1),
    CFG_ENTRY(3, 1),
    CFG_ENTRY(4, VITA_HAS_CFG4),
    CFG_ENTRY(5, VITA_HAS_CFG5),
    CFG_ENTRY(6, VITA_HAS_CFG6),
    CFG_ENTRY(7, VITA_HAS_CFG7),
    CFG_ENTRY(8, VITA_HAS_CFG8),
    CFG_ENTRY(9, VITA_HAS_CFG9),
    CFG_ENTRY(10, VITA_HAS_CFG10),
    CFG_ENTRY(11, VITA_HAS_CFG11),
    CFG_ENTRY(12, VITA_HAS_CFG12),
    CFG_ENTRY(13, VITA_HAS_CFG13),
    CFG_ENTRY(14, VITA_HAS_CFG14),
    CFG_ENTRY(15, VITA_HAS_CFG15),
    CFG_ENTRY(16, VITA_HAS_CFG16),
    CFG_ENTRY(17, VITA_HAS_CFG17),
    CFG_ENTRY(18, VITA_HAS_CFG18),
    CFG_ENTRY(19, VITA_HAS_CFG19),
    CFG_ENTRY(20, VITA_HAS_CFG20),
    CFG_ENTRY(21, VITA_HAS_CFG21),
    CFG_ENTRY(22, VITA_HAS_CFG22),
    CFG_ENTRY(23, VITA_HAS_CFG23),
    CFG_ENTRY(24, VITA_HAS_CFG24),
    CFG_ENTRY(25, VITA_HAS_CFG25),
    CFG_ENTRY(26, VITA_HAS_CFG26),
    CFG_ENTRY(27, VITA_HAS_CFG27),
    CFG_ENTRY(28, VITA_HAS_CFG28),
    CFG_ENTRY(29, VITA_HAS_CFG29),
    CFG_ENTRY(30, VITA_HAS_CFG30),
    CFG_ENTRY(31, VITA_HAS_CFG31),
    CFG_ENTRY(32, VITA_HAS_CFG32),
    CFG_ENTRY(33, VITA_HAS_CFG33),
    CFG_ENTRY(34, VITA_HAS_CFG34),
    CFG_ENTRY(35, VITA_HAS_CFG35),
    CFG_ENTRY(36, VITA_HAS_CFG36),
    CFG_ENTRY(37, VITA_HAS_CFG37),
    CFG_ENTRY(38, VITA_HAS_CFG38),
    CFG_ENTRY(39, VITA_HAS_CFG39),
};

// map old vita_cfgN[] names to table entries
#define vita_cfg0  vita_cfgs[0].programs
#define vita_cfg2  vita_cfgs[1].programs
#define vita_cfg3  vita_cfgs[2].programs
#define vita_cfg4  vita_cfgs[3].programs
#define vita_cfg5  vita_cfgs[4].programs
#define vita_cfg6  vita_cfgs[5].programs
#define vita_cfg7  vita_cfgs[6].programs
#define vita_cfg8  vita_cfgs[7].programs
#define vita_cfg9  vita_cfgs[8].programs
#define vita_cfg10 vita_cfgs[9].programs
#define vita_cfg11 vita_cfgs[10].programs
#define vita_cfg12 vita_cfgs[11].programs
#define vita_cfg13 vita_cfgs[12].programs
#define vita_cfg14 vita_cfgs[13].programs
#define vita_cfg15 vita_cfgs[14].programs
#define vita_cfg16 vita_cfgs[15].programs
#define vita_cfg17 vita_cfgs[16].programs
#define vita_cfg18 vita_cfgs[17].programs
#define vita_cfg19 vita_cfgs[18].programs
#define vita_cfg20 vita_cfgs[19].programs
#define vita_cfg21 vita_cfgs[20].programs
#define vita_cfg22 vita_cfgs[21].programs
#define vita_cfg23 vita_cfgs[22].programs
#define vita_cfg24 vita_cfgs[23].programs
#define vita_cfg25 vita_cfgs[24].programs
#define vita_cfg26 vita_cfgs[25].programs
#define vita_cfg27 vita_cfgs[26].programs
#define vita_cfg28 vita_cfgs[27].programs
#define vita_cfg29 vita_cfgs[28].programs
#define vita_cfg30 vita_cfgs[29].programs
#define vita_cfg31 vita_cfgs[30].programs
#define vita_cfg32 vita_cfgs[31].programs
#define vita_cfg33 vita_cfgs[32].programs
#define vita_cfg34 vita_cfgs[33].programs
#define vita_cfg35 vita_cfgs[34].programs
#define vita_cfg36 vita_cfgs[35].programs
#define vita_cfg37 vita_cfgs[36].programs
#define vita_cfg38 vita_cfgs[37].programs
#define vita_cfg39 vita_cfgs[38].programs

// load pre-compiled GXP binary (4-byte header + GXP data)
static GLuint vita_load_gxp(GLenum type, const unsigned char* gxp, unsigned int gxp_size) {
    unsigned int total = 4 + gxp_size;
    unsigned char* buf = (unsigned char*)malloc(total);
    if (!buf) return 0;

    buf[0] = buf[1] = buf[2] = buf[3] = 0; // matrix_uniforms_num = 0
    memcpy(buf + 4, gxp, gxp_size);

    GLuint shader = glCreateShader(type);
    glShaderBinary(1, &shader, 0, buf, total);
    free(buf);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        fprintf(stderr, "[TEV] glShaderBinary failed (GL error 0x%04X, size=%u)\n", err, gxp_size);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint vita_load_vertex_shader(int flags) {
    (void)flags;
    return vita_load_gxp(GL_VERTEX_SHADER, gxp_vertex, gxp_vertex_size);
}

static GLuint vita_load_variant(int flags) {
    GLuint vs = vita_load_vertex_shader(flags);
    int frag_flags = flags & ~VITA_VF_LIGHTING; // VS handles lighting
    GLuint fs = vita_load_gxp(GL_FRAGMENT_SHADER,
                              gxp_frag_variants[frag_flags],
                              gxp_frag_variant_sizes[frag_flags]);
    GLuint prog = link_program(vs, fs);

    printf("[TEV] Vita variant %2d loaded (L=%d F=%d A=%d T2=%d) -> program %u\n",
           flags,
           !!(flags & VITA_VF_LIGHTING),
           !!(flags & VITA_VF_FOG),
           !!(flags & VITA_VF_ALPHA_TEST),
           !!(flags & VITA_VF_TEV2),
           prog);
    return prog;
}

static GLuint vita_load_simple(int flags) {
    GLuint vs = vita_load_vertex_shader(flags);
    int frag_flags = flags & ~VITA_VF_LIGHTING;
    GLuint fs = vita_load_gxp(GL_FRAGMENT_SHADER,
                              gxp_simple_variants[frag_flags],
                              gxp_simple_variant_sizes[frag_flags]);
    GLuint prog = link_program(vs, fs);
    printf("[TEV] Simple variant %d loaded (L=%d F=%d A=%d) -> program %u\n",
           flags,
           !!(flags & VITA_VF_LIGHTING),
           !!(flags & VITA_VF_FOG),
           !!(flags & VITA_VF_ALPHA_TEST),
           prog);
    return prog;
}

static GLuint vita_load_specialized(const char* name,
                                     const unsigned char** variants,
                                     const unsigned int* sizes,
                                     int flags) {
    GLuint vs = vita_load_vertex_shader(flags);
    int frag_flags = flags & ~VITA_VF_LIGHTING;
    GLuint fs = vita_load_gxp(GL_FRAGMENT_SHADER, variants[frag_flags], sizes[frag_flags]);
    GLuint prog = link_program(vs, fs);
    printf("[TEV] %s variant %d loaded (L=%d F=%d A=%d) -> program %u\n",
           name, flags,
           !!(flags & VITA_VF_LIGHTING),
           !!(flags & VITA_VF_FOG),
           !!(flags & VITA_VF_ALPHA_TEST),
           prog);
    return prog;
}

void pc_gx_tev_init(void) {
    memset(vita_variants, 0, sizeof(vita_variants));
    memset(vita_simple, 0, sizeof(vita_simple));
    // zero all specialized config program arrays
    for (int c = 0; c < VITA_CFG_COUNT; c++)
        memset(vita_cfgs[c].programs, 0, sizeof(vita_cfgs[c].programs));

    int ok = 0;

    for (int i = 0; i < VITA_SIMPLE_COUNT; i += 2) {
        vita_simple[i] = vita_load_simple(i);
        if (vita_simple[i]) ok++;
        glFinish();
    }

    for (int c = 0; c < VITA_CFG_COUNT; c++) {
        if (!vita_cfgs[c].available) continue;
        for (int i = 0; i < VITA_SPEC_COUNT; i += 2) {
            vita_cfgs[c].programs[i] = vita_load_specialized(
                vita_cfgs[c].name, vita_cfgs[c].variants, vita_cfgs[c].sizes, i);
            if (vita_cfgs[c].programs[i]) ok++;
            glFinish();
        }
    }

    // uber-shader fallback
    for (int i = 0; i < VITA_VF_COUNT; i++) {
        vita_variants[i] = vita_load_variant(i);
        if (vita_variants[i]) ok++;
        glFinish();
    }

    if (ok == 0) {
        fprintf(stderr, "FATAL: All shader variants failed!\n");
        sceKernelExitProcess(1);
        return;
    }
    int total = VITA_SIMPLE_COUNT + VITA_VF_COUNT;
    int spec_count = 0;
    for (int c = 0; c < VITA_CFG_COUNT; c++)
        for (int i = 0; i < VITA_SPEC_COUNT; i += 2)
            if (vita_cfgs[c].programs[i]) spec_count++;
    total += spec_count;
#ifdef VITA_PERF_LOG
    { FILE* cf = fopen("ux0:data/AnimalCrossing/ac_perf.txt", "w"); if (cf) fclose(cf); }
    vita_log("=== AC Vita TEV Init (build %s %s) ===\n", __DATE__, __TIME__);
#endif
    vita_log("Shader system: %d/%d programs OK (8 simple + %d specialized + 16 complex)\n", ok, total, spec_count);
}

GLuint pc_gx_tev_load_composite_shader(void) {
#if VITA_HAS_COMPOSITE
    GLuint vs = vita_load_gxp(GL_VERTEX_SHADER, gxp_composite_vert, gxp_composite_vert_size);
    GLuint fs = vita_load_gxp(GL_FRAGMENT_SHADER, gxp_composite_frag, gxp_composite_frag_size);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        printf("[TEV] Composite shader GXP load failed\n");
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "a_position");
    glLinkProgram(prog);
    GLint success;
    glGetProgramiv(prog, GL_LINK_STATUS, &success);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!success) {
        printf("[TEV] Composite shader link failed\n");
        glDeleteProgram(prog);
        return 0;
    }
    printf("[TEV] Composite shader loaded -> program %u\n", prog);
    return prog;
#else
    printf("[TEV] Composite shader GXP not compiled\n");
    return 0;
#endif
}

void pc_gx_tev_shutdown(void) {
    for (int i = 0; i < VITA_VF_COUNT; i++) {
        if (vita_variants[i]) { glDeleteProgram(vita_variants[i]); vita_variants[i] = 0; }
    }
    for (int i = 0; i < VITA_SIMPLE_COUNT; i++) {
        if (vita_simple[i]) { glDeleteProgram(vita_simple[i]); vita_simple[i] = 0; }
    }
    for (int c = 0; c < VITA_CFG_COUNT; c++) {
        for (int i = 0; i < VITA_SPEC_COUNT; i += 2) {
            if (vita_cfgs[c].programs[i]) {
                glDeleteProgram(vita_cfgs[c].programs[i]);
                vita_cfgs[c].programs[i] = 0;
            }
        }
    }
}

// forward declaration (defined below)
static int vita_tev_ops_trivial(PCGXState* s);

// TEV config hash table: tracks unique configs for shader specialization
#define TEV_HASH_MAX 128
static struct {
    unsigned int hash;
    int stages;
    int cin[3][4], ain[3][4]; // color/alpha inputs per stage (A,B,C,D)
    int cop[3], aop[3], cbias[3], abias[3], cscl[3], ascl[3];
    int count;
    int non_trivial; // 1 if ops were non-trivial (bypassed specialized matching entirely)
} tev_hash_table[TEV_HASH_MAX];
static int tev_hash_count = 0;
static int tev_hash_logged = 0;

// per-frame uber draw logging: logs each unique uber config per frame (for waterfall debugging)
static int uber_log_enabled = 1; // set to 0 after enough data collected
static int uber_log_frame_count = 0;
#define UBER_LOG_FRAMES 600 // log for ~10 seconds of gameplay at 60fps

static unsigned int tev_config_hash(PCGXState* s) {
    unsigned int h = s->num_tev_stages * 31;
    for (int i = 0; i < s->num_tev_stages && i < 3; i++) {
        PCGXTevStage* t = &s->tev_stages[i];
        h = h * 65599 + t->color_a + t->color_b*17 + t->color_c*257 + t->color_d*4097;
        h = h * 65599 + t->alpha_a + t->alpha_b*17 + t->alpha_c*257 + t->alpha_d*4097;
        h = h * 65599 + t->color_op + t->alpha_op*5 + t->color_bias*25 + t->alpha_bias*125;
        h = h * 65599 + t->color_scale + t->alpha_scale*5;
    }
    return h;
}

static void tev_config_record(PCGXState* s) {
    unsigned int h = tev_config_hash(s);
    int is_new = 1;
    for (int i = 0; i < tev_hash_count; i++) {
        if (tev_hash_table[i].hash == h) { tev_hash_table[i].count++; is_new = 0; break; }
    }
    if (is_new && tev_hash_count < TEV_HASH_MAX) {
        int idx = tev_hash_count++;
        tev_hash_table[idx].hash = h;
        tev_hash_table[idx].stages = s->num_tev_stages;
        tev_hash_table[idx].count = 1;
        tev_hash_table[idx].non_trivial = !vita_tev_ops_trivial(s);
        for (int i = 0; i < s->num_tev_stages && i < 3; i++) {
            PCGXTevStage* t = &s->tev_stages[i];
            tev_hash_table[idx].cin[i][0] = t->color_a; tev_hash_table[idx].cin[i][1] = t->color_b;
            tev_hash_table[idx].cin[i][2] = t->color_c; tev_hash_table[idx].cin[i][3] = t->color_d;
            tev_hash_table[idx].ain[i][0] = t->alpha_a; tev_hash_table[idx].ain[i][1] = t->alpha_b;
            tev_hash_table[idx].ain[i][2] = t->alpha_c; tev_hash_table[idx].ain[i][3] = t->alpha_d;
            tev_hash_table[idx].cop[i] = t->color_op; tev_hash_table[idx].aop[i] = t->alpha_op;
            tev_hash_table[idx].cbias[i] = t->color_bias; tev_hash_table[idx].abias[i] = t->alpha_bias;
            tev_hash_table[idx].cscl[i] = t->color_scale; tev_hash_table[idx].ascl[i] = t->alpha_scale;
        }
    }

    // per-frame logging: log NEW uber configs as they first appear
    if (is_new && uber_log_enabled) {
        vita_log("[UBER_NEW] stages=%d hash=0x%08x non_trivial=%d\n", s->num_tev_stages, h, !vita_tev_ops_trivial(s));
        for (int i = 0; i < s->num_tev_stages && i < 3; i++) {
            PCGXTevStage* t = &s->tev_stages[i];
            vita_log("  S%d c(%d,%d,%d,%d) a(%d,%d,%d,%d) op(%d,%d) bias(%d,%d) scl(%d,%d)\n",
                     i, t->color_a, t->color_b, t->color_c, t->color_d,
                     t->alpha_a, t->alpha_b, t->alpha_c, t->alpha_d,
                     t->color_op, t->alpha_op,
                     t->color_bias, t->alpha_bias,
                     t->color_scale, t->alpha_scale);
        }
    }
}

void vita_dump_tev_configs(void) {
    // allow re-logging each time uber configs are detected (not just once)
    vita_log("\n=== Uber TEV configs (total unique: %d) ===\n", tev_hash_count);
    // sort by count (descending)
    for (int i = 0; i < tev_hash_count - 1; i++)
        for (int j = i + 1; j < tev_hash_count; j++)
            if (tev_hash_table[j].count > tev_hash_table[i].count) {
                typeof(tev_hash_table[0]) tmp = tev_hash_table[i];
                tev_hash_table[i] = tev_hash_table[j];
                tev_hash_table[j] = tmp;
            }
    for (int i = 0; i < tev_hash_count; i++) {
        vita_log("UBER%d: stages=%d count=%d%s\n", i, tev_hash_table[i].stages,
                 tev_hash_table[i].count,
                 tev_hash_table[i].non_trivial ? " NON_TRIVIAL" : " trivial_ops");
        for (int s = 0; s < tev_hash_table[i].stages && s < 3; s++) {
            vita_log("  S%d c(%d,%d,%d,%d) a(%d,%d,%d,%d) op(%d,%d) bias(%d,%d) scl(%d,%d)\n",
                     s,
                     tev_hash_table[i].cin[s][0], tev_hash_table[i].cin[s][1],
                     tev_hash_table[i].cin[s][2], tev_hash_table[i].cin[s][3],
                     tev_hash_table[i].ain[s][0], tev_hash_table[i].ain[s][1],
                     tev_hash_table[i].ain[s][2], tev_hash_table[i].ain[s][3],
                     tev_hash_table[i].cop[s], tev_hash_table[i].aop[s],
                     tev_hash_table[i].cbias[s], tev_hash_table[i].abias[s],
                     tev_hash_table[i].cscl[s], tev_hash_table[i].ascl[s]);
        }
    }
    tev_hash_logged = 1;
}

// performance counters for shader draw classification
int vita_tev_specialized_draws = 0;
int vita_tev_complex_draws = 0;
int vita_tev_alpha_test_draws = 0;

// 1 when current draw uses ocean/water shader (cfg7/cfg8)
int vita_tev_is_ocean = 0;
int vita_tev_tex_remap = 0; // cfg20: remap stage 1 texture to unit 0
int vita_tev_passthrough = 0; // simple shader PASSTHROUGH: neutralize ras (num_chans=0)

// check if all stages have trivial ops (op=0, bias=0, scale=0)
static int vita_tev_ops_trivial(PCGXState* s) {
    for (int i = 0; i < s->num_tev_stages; i++) {
        PCGXTevStage* t = &s->tev_stages[i];
        if (t->color_op != 0 || t->alpha_op != 0 ||
            t->color_bias != 0 || t->alpha_bias != 0 ||
            t->color_scale != 0 || t->alpha_scale != 0)
            return 0;
    }
    return 1;
}

// force uber-shader for all draws (debug: 1 = bypass specialized matching)
#define VITA_FORCE_UBER_SHADER 0

// vita_cpu_lit_active declared in vita_shared.h

// pre-resolvable register (not TEXC/TEXA/RASC/RASA)
static inline int is_preresolvable(int input) {
    return input >= 2 && input != 8 && input != 9 && input != 10 && input != 11;
}

// one of (b,c) is TEXA and the other is a register
static inline int is_texa_times_register(int b, int c) {
    int is_reg_b = (b >= 1 && b <= 3) || b == 6;
    int is_reg_c = (c >= 1 && c <= 3) || c == 6;
    return (b == 4 && is_reg_c) || (c == 4 && is_reg_b);
}

GLuint pc_gx_tev_get_shader(PCGXState* state) {
    vita_tev_is_ocean = 0;
    vita_tev_tex_remap = 0;
    vita_tev_passthrough = 0;
    int key = 0;
    if (state->chan_ctrl_enable[0]) key |= VITA_VF_LIGHTING;
    if (state->fog_type != 0)      key |= VITA_VF_FOG;
    if (state->alpha_comp0 != 7 || state->alpha_comp1 != 7) {
        // on TBDR GPUs (SGX543), discard kills HSR for entire tiles
        // skip ALPHA_TEST when effective ref <= 0 since alpha can't fail
        float ref0, ref1;
        switch (state->alpha_comp0) {
            case 0: ref0 = 2.0f; break;   // NEVER
            case 7: ref0 = -1.0f; break;  // ALWAYS
            default: ref0 = (float)state->alpha_ref0 / 255.0f; break;
        }
        switch (state->alpha_comp1) {
            case 0: ref1 = 2.0f; break;
            case 7: ref1 = -1.0f; break;
            default: ref1 = (float)state->alpha_ref1 / 255.0f; break;
        }
        float eff_ref = (ref0 > ref1) ? ref0 : ref1; // AND (op=0)
        if (state->alpha_op == 1) eff_ref = (ref0 < ref1) ? ref0 : ref1; // OR
        // skip alpha test for blended draws with depth write off
        // discard kills SGX543 HSR per-tile
        int blended_no_zwrite = (state->blend_mode == GX_BM_BLEND &&
            !state->z_update_enable &&
            (state->blend_src == GX_BL_SRCALPHA || state->blend_src == GX_BL_DSTALPHA) &&
            (state->blend_dst == GX_BL_INVSRCALPHA || state->blend_dst == GX_BL_INVDSTALPHA));
        if (!blended_no_zwrite && eff_ref > 4.0f/255.0f) key |= VITA_VF_ALPHA_TEST;
    }
    int lfa = key & 0x7; // L/F/A bits
    int fa = lfa & ~VITA_VF_LIGHTING; // F/A only, VS handles LIGHTING
    if (key & VITA_VF_ALPHA_TEST) vita_tev_alpha_test_draws++;

    // zero-alpha skip: alpha=0 + SRC_ALPHA blend = invisible, safe to skip
    if (state->blend_mode == GX_BM_BLEND && !state->z_update_enable &&
        (state->blend_src == GX_BL_SRCALPHA || state->blend_src == GX_BL_DSTALPHA) &&
        (state->blend_dst == GX_BL_INVSRCALPHA || state->blend_dst == GX_BL_INVDSTALPHA)) {
        // check if all TEV stages produce alpha=0
        int alpha_is_zero = 1;
        for (int s = 0; s < state->num_tev_stages && s < 2; s++) {
            PCGXTevStage* t = &state->tev_stages[s];
            int aa = t->alpha_a, ab = t->alpha_b, ac = t->alpha_c, ad = t->alpha_d;
            if (s == 0) {
                // all inputs must be ZERO(7) for result=0
                if (!(aa == 7 && ab == 7 && ac == 7 && ad == 7)) {
                    alpha_is_zero = 0; break;
                }
            } else {
                // APREV(0) carries 0 from stage 0; accept ZERO(7) or APREV(0)
                int za = (aa == 7 || aa == 0);
                int zb = (ab == 7 || ab == 0);
                int zc = (ac == 7 || ac == 0);
                int zd = (ad == 7 || ad == 0);
                if (!(za && zb && zc && zd)) {
                    alpha_is_zero = 0; break;
                }
            }
        }
        if (alpha_is_zero && state->num_tev_stages > 0) {
            return 0;
        }
    }

#if VITA_FORCE_UBER_SHADER
    goto use_complex;
#endif
    if (vita_force_uber) goto use_complex;
    // specialized shader matching
    if (vita_tev_ops_trivial(state)) {
        PCGXTevStage* s0 = &state->tev_stages[0];

        if (state->num_tev_stages == 2) {
            PCGXTevStage* s1 = &state->tev_stages[1];

            // CFG0 (77%): tex*ras*reg1, alpha=tex.a
            if (s0->color_a==15 && s0->color_b==8 && s0->color_c==10 && s0->color_d==15 &&
                s0->alpha_a==7  && s0->alpha_b==7 && s0->alpha_c==7  && s0->alpha_d==4 &&
                s1->color_a==15 && s1->color_b==4 && s1->color_c==0  && s1->color_d==15 &&
                s1->alpha_a==7  && s1->alpha_b==7 && s1->alpha_c==7  && s1->alpha_d==0 &&
                vita_cfg0[fa]) {
                vita_tev_specialized_draws++;
                return vita_cfg0[fa];
            }

            // CFG22: tex*register color + TEXA alpha (no RASC)
            if (s0->color_a==15 && s0->color_b==15 && s0->color_c==15 && s0->color_d==8 &&
                s0->alpha_a==7  && s0->alpha_b==7  && s0->alpha_c==7  && s0->alpha_d==4 &&
                s1->color_a==15 && s1->color_b==4  && s1->color_c==0  && s1->color_d==15 &&
                s1->alpha_a==7  && s1->alpha_b==7  && s1->alpha_c==7  && s1->alpha_d==0 &&
                vita_cfg22[fa]) {
                vita_tev_specialized_draws++;
                return vita_cfg22[fa];
            }

            // CFG23: tex0 color + tex1 alpha (no RASC)
            if (s0->color_a==15 && s0->color_b==15 && s0->color_c==15 && s0->color_d==8 &&
                s0->alpha_a==7  && s0->alpha_b==7  && s0->alpha_c==7  && s0->alpha_d==7 &&
                s1->color_a==15 && s1->color_b==15 && s1->color_c==15 && s1->color_d==0 &&
                s1->alpha_a==7  && s1->alpha_b==7  && s1->alpha_c==7  && s1->alpha_d==4 &&
                vita_cfg23[fa]) {
                vita_tev_specialized_draws++;
                return vita_cfg23[fa];
            }

            // CFG2 (7.7%): lerp(ras,tex,A0) then lerp(C2,C1,prev)
            if (s0->color_a==10 && s0->color_b==8 && s0->color_c==3  && s0->color_d==15 &&
                s0->alpha_a==7  && s0->alpha_b==4 && s0->alpha_c==2  && s0->alpha_d==7 &&
                s1->color_a==6  && s1->color_b==4 && s1->color_c==0  && s1->color_d==15 &&
                s1->alpha_a==7  && s1->alpha_b==7 && s1->alpha_c==7  && s1->alpha_d==0 &&
                vita_cfg2[fa]) {
                vita_tev_specialized_draws++;
                return vita_cfg2[fa];
            }

            // CFG10: tex + ras*lerp(C2,C1,tex), alpha = tex.a*A0
            if (s0->color_a==6  && s0->color_b==4  && s0->color_c==8  && s0->color_d==15 &&
                s0->alpha_a==7  && s0->alpha_b==4  && s0->alpha_c==1  && s0->alpha_d==2 &&
                s1->color_a==15 && s1->color_b==10 && s1->color_c==0  && s1->color_d==8 &&
                s1->alpha_a==7  && s1->alpha_b==4  && s1->alpha_c==1  && s1->alpha_d==0 &&
                vita_cfg10[fa]) {
                vita_tev_specialized_draws++;
                return vita_cfg10[fa];
            }

            // CFG7: lerp(reg,reg,tex)*ras ocean pattern
            if (s0->color_c==8 && s0->color_d==15 &&
                is_preresolvable(s0->color_a) &&
                is_preresolvable(s0->color_b) &&
                s0->alpha_a==7 && s0->alpha_b==7 && s0->alpha_c==7 &&
                (s0->alpha_d==7 || s0->alpha_d==4) &&
                s1->color_a==15 && s1->color_b==0 && s1->color_c==10 && s1->color_d==15 &&
                s1->alpha_a==7 && s1->alpha_b==7 && s1->alpha_c==7 && s1->alpha_d==0 &&
                vita_cfg7[fa]) {
                vita_tev_specialized_draws++;
                vita_tev_is_ocean = 1;
                return vita_cfg7[fa];
            }

            // CFG38: TEX0*(1+RAS) + RAS*KONST, TEX1A*APREV alpha (2-stage, 2-texture, exact match)
            if (s0->color_a==15 && s0->color_b==8  && s0->color_c==10 && s0->color_d==8 &&
                s0->alpha_a==7  && s0->alpha_b==7  && s0->alpha_c==7  && s0->alpha_d==2 &&
                s1->color_a==15 && s1->color_b==4  && s1->color_c==10 && s1->color_d==0 &&
                s1->alpha_a==7  && s1->alpha_b==0  && s1->alpha_c==4  && s1->alpha_d==7 &&
                vita_cfg38[fa] && !VITA_CFG_DISABLED(38)) {
                vita_tev_specialized_draws++;
                return vita_cfg38[fa];
            }

            // CFG39: lerp(C2,C1,TEX0) + TEX1A*APREV alpha chain (2-stage, 2-texture, exact match)
            if (s0->color_a==6  && s0->color_b==4  && s0->color_c==8  && s0->color_d==15 &&
                s0->alpha_a==7  && s0->alpha_b==4  && s0->alpha_c==3  && s0->alpha_d==7 &&
                s1->color_a==15 && s1->color_b==15 && s1->color_c==15 && s1->color_d==0 &&
                s1->alpha_a==7  && s1->alpha_b==0  && s1->alpha_c==4  && s1->alpha_d==7 &&
                vita_cfg39[fa] && !VITA_CFG_DISABLED(39)) {
                vita_tev_specialized_draws++;
                return vita_cfg39[fa];
            }

            // CFG36: (KONST*TEX)*RAS island ocean (2-stage)
            if (s0->color_a==15 && s0->color_b==8  && s0->color_c==4  && s0->color_d==15 &&
                ((s0->alpha_a==7 && s0->alpha_b==4 && s0->alpha_c==3 && s0->alpha_d==7) ||
                 (s0->alpha_a==7 && s0->alpha_b==7 && s0->alpha_c==7 && s0->alpha_d==4)) &&
                s1->color_a==15 && s1->color_b==0  && s1->color_c==10 && s1->color_d==15 &&
                s1->alpha_a==7  && s1->alpha_b==7  && s1->alpha_c==7  && s1->alpha_d==0 &&
                vita_cfg36[fa] && !VITA_CFG_DISABLED(36)) {
                vita_tev_specialized_draws++;
                vita_tev_is_ocean = 1;
                return vita_cfg36[fa];
            }

            // CFG27: tex*ras*TEXA_clr color + A1+A0*TEXA alpha (similar to CFG0 but different S1 alpha)
            if (s0->color_a==15 && s0->color_b==8  && s0->color_c==10 && s0->color_d==15 &&
                s0->alpha_a==7  && s0->alpha_b==7  && s0->alpha_c==7  && s0->alpha_d==4 &&
                s1->color_a==15 && s1->color_b==4  && s1->color_c==0  && s1->color_d==15 &&
                s1->alpha_a==7  && s1->alpha_b==0  && s1->alpha_c==1  && s1->alpha_d==2 &&
                vita_cfg27[fa] && !VITA_CFG_DISABLED(27)) {
                vita_tev_specialized_draws++;
                return vita_cfg27[fa];
            }

            // CFG34: TEXA_clr color + A1 register alpha (was old uber CFG0, 650+ hits)
            if (s0->color_a==15 && s0->color_b==15 && s0->color_c==15 && s0->color_d==4 &&
                s0->alpha_a==7  && s0->alpha_b==7  && s0->alpha_c==7  && s0->alpha_d==7 &&
                s1->color_a==15 && s1->color_b==15 && s1->color_c==15 && s1->color_d==0 &&
                s1->alpha_a==7  && s1->alpha_b==7  && s1->alpha_c==7  && s1->alpha_d==2 &&
                vita_cfg34[fa] && !VITA_CFG_DISABLED(34)) {
                vita_tev_specialized_draws++;
                return vita_cfg34[fa];
            }

            // CFG35: TEXC*RASC*TEXA_clr color + A1*TEXA alpha (was old uber CFG1, 650+ hits)
            if (s0->color_a==15 && s0->color_b==8  && s0->color_c==10 && s0->color_d==15 &&
                s0->alpha_a==7  && s0->alpha_b==7  && s0->alpha_c==7  && s0->alpha_d==4 &&
                s1->color_a==15 && s1->color_b==0  && s1->color_c==4  && s1->color_d==15 &&
                s1->alpha_a==7  && s1->alpha_b==0  && s1->alpha_c==2  && s1->alpha_d==7 &&
                vita_cfg35[fa] && !VITA_CFG_DISABLED(35)) {
                vita_tev_specialized_draws++;
                return vita_cfg35[fa];
            }

            // CFG6: register color + TEXA*register alpha chain (2-stage)
            if (s0->color_a==15 && s0->color_b==15 && s0->color_c==15 &&
                is_preresolvable(s0->color_d) &&
                s0->alpha_a==7  && s0->alpha_b==4 &&
                ((s0->alpha_c >= 1 && s0->alpha_c <= 3) || s0->alpha_c == 6) &&
                s0->alpha_d==7 &&
                s1->color_a==15 && s1->color_b==15 && s1->color_c==15 && s1->color_d==0 &&
                s1->alpha_a==7  && s1->alpha_b==4  && s1->alpha_c==0  && s1->alpha_d==7 &&
                vita_cfg6[fa]) {
                vita_tev_specialized_draws++;
                return vita_cfg6[fa];
            }
        }
        else if (state->num_tev_stages == 3) {
            PCGXTevStage* s1 = &state->tev_stages[1];
            PCGXTevStage* s2 = &state->tev_stages[2];

            // CFG12: 3-stage ocean pattern A (76% of ocean draws)
            if (s0->color_a==15 && s0->color_b==15 && s0->color_c==15 && s0->color_d==8 &&
                s0->alpha_a==7  && s0->alpha_b==7  && s0->alpha_c==7  && s0->alpha_d==4 &&
                s1->color_a==15 && s1->color_b==0  && s1->color_c==8  && s1->color_d==0 &&
                s1->alpha_a==7  && s1->alpha_b==0  && s1->alpha_c==4  && s1->alpha_d==7 &&
                s2->color_a==15 && s2->color_b==10 && s2->color_c==4  && s2->color_d==0 &&
                s2->alpha_a==7  && s2->alpha_b==7  && s2->alpha_c==7  && s2->alpha_d==0 &&
                vita_cfg12[fa]) {
                vita_tev_specialized_draws++;
                vita_tev_is_ocean = 1;
                return vita_cfg12[fa];
            }

            // CFG13: 3-stage ocean pattern B (24% of ocean draws)
            if (s0->color_a==15 && s0->color_b==15 && s0->color_c==15 && s0->color_d==8 &&
                s0->alpha_a==7  && s0->alpha_b==7  && s0->alpha_c==7  && s0->alpha_d==4 &&
                s1->color_a==15 && s1->color_b==0  && s1->color_c==9  && s1->color_d==15 &&
                s1->alpha_a==7  && s1->alpha_b==0  && s1->alpha_c==4  && s1->alpha_d==7 &&
                s2->color_a==15 && s2->color_b==4  && s2->color_c==10 && s2->color_d==0 &&
                s2->alpha_a==7  && s2->alpha_b==0  && s2->alpha_c==1  && s2->alpha_d==2 &&
                vita_cfg13[fa]) {
                vita_tev_specialized_draws++;
                vita_tev_is_ocean = 1;
                return vita_cfg13[fa];
            }

            // CFG24: 3-stage waterfall: C1*ras + two-tex alpha blend
            if (s0->color_a==15 && s0->color_b==10 && s0->color_c==4  && s0->color_d==15 &&
                s0->alpha_a==7  && s0->alpha_b==7  && s0->alpha_c==7  && s0->alpha_d==4 &&
                s1->color_a==15 && s1->color_b==15 && s1->color_c==15 && s1->color_d==0 &&
                s1->alpha_a==0  && s1->alpha_b==4  && s1->alpha_c==1  && s1->alpha_d==7 &&
                s2->color_a==15 && s2->color_b==15 && s2->color_c==15 && s2->color_d==0 &&
                s2->alpha_a==7  && s2->alpha_b==0  && s2->alpha_c==2  && s2->alpha_d==7 &&
                vita_cfg24[fa]) {
                vita_tev_specialized_draws++;
                return vita_cfg24[fa];
            }

            // CFG28: 3-stage waterfall foam: TEXA_clr color + A1*lerp(tex0a,tex1a,A0) alpha
            if (s0->color_a==15 && s0->color_b==15 && s0->color_c==15 && s0->color_d==4 &&
                s0->alpha_a==7  && s0->alpha_b==7  && s0->alpha_c==7  && s0->alpha_d==4 &&
                s1->color_a==15 && s1->color_b==15 && s1->color_c==15 && s1->color_d==0 &&
                s1->alpha_a==0  && s1->alpha_b==4  && s1->alpha_c==1  && s1->alpha_d==7 &&
                s2->color_a==15 && s2->color_b==15 && s2->color_c==15 && s2->color_d==0 &&
                s2->alpha_a==7  && s2->alpha_b==0  && s2->alpha_c==2  && s2->alpha_d==7 &&
                vita_cfg28[fa] && !VITA_CFG_DISABLED(28)) {
                vita_tev_specialized_draws++;
                return vita_cfg28[fa];
            }

            // CFG25: 3-stage tex*TEXA blend + register lerp
            if (s0->color_a==15 && s0->color_b==15 && s0->color_c==15 && s0->color_d==8 &&
                s0->alpha_a==7  && s0->alpha_b==7  && s0->alpha_c==7  && s0->alpha_d==4 &&
                s1->color_a==15 && s1->color_b==0  && s1->color_c==9  && s1->color_d==15 &&
                s1->alpha_a==7  && s1->alpha_b==0  && s1->alpha_c==4  && s1->alpha_d==7 &&
                s2->color_a==6  && s2->color_b==4  && s2->color_c==0  && s2->color_d==15 &&
                s2->alpha_a==7  && s2->alpha_b==0  && s2->alpha_c==2  && s2->alpha_d==7 &&
                vita_cfg25[fa]) {
                vita_tev_specialized_draws++;
                return vita_cfg25[fa];
            }
        }
        else if (state->num_tev_stages <= 1) {
            // CFG11: tex*ras color, tex.a*A1 alpha
            if (s0->color_a==15 && s0->color_b==8  && s0->color_c==10 && s0->color_d==15 &&
                s0->alpha_a==7  && s0->alpha_b==4  && s0->alpha_c==2  && s0->alpha_d==7 &&
                vita_cfg11[fa]) {
                vita_tev_specialized_draws++;
                return vita_cfg11[fa];
            }

            // CFG26: register color + RASA*register alpha (per-pixel vertex alpha)
            if (s0->color_a==15 && s0->color_b==15 && s0->color_c==15 &&
                is_preresolvable(s0->color_d) &&
                s0->alpha_a==7 && s0->alpha_d==7 &&
                ((s0->alpha_b==5 && ((s0->alpha_c >= 1 && s0->alpha_c <= 3) || s0->alpha_c == 6)) ||
                 (s0->alpha_c==5 && ((s0->alpha_b >= 1 && s0->alpha_b <= 3) || s0->alpha_b == 6))) &&
                vita_cfg26[fa]) {
                vita_tev_specialized_draws++;
                return vita_cfg26[fa];
            }

            // CFG3: register color + TEXA*register alpha
            if (s0->color_a==15 && s0->color_b==15 && s0->color_c==15 &&
                is_preresolvable(s0->color_d) &&
                s0->alpha_a==7 && s0->alpha_d==7 &&
                is_texa_times_register(s0->alpha_b, s0->alpha_c) &&
                vita_cfg3[fa]) {
                vita_tev_specialized_draws++;
                return vita_cfg3[fa];
            }

            // CFG4: register color + alpha D passthrough
            if (s0->color_a==15 && s0->color_b==15 && s0->color_c==15 &&
                is_preresolvable(s0->color_d) &&
                s0->alpha_a==7 && s0->alpha_b==7 && s0->alpha_c==7 &&
                s0->alpha_d != 5 &&
                vita_cfg4[fa]) {
                vita_tev_specialized_draws++;
                return vita_cfg4[fa];
            }

            // CFG5: lerp(reg,reg,tex) color + TEXA*register alpha
            if (s0->color_c==8 && s0->color_d==15 &&
                is_preresolvable(s0->color_a) &&
                is_preresolvable(s0->color_b) &&
                s0->alpha_a==7 && s0->alpha_d==7 &&
                is_texa_times_register(s0->alpha_b, s0->alpha_c) &&
                vita_cfg5[fa]) {
                vita_tev_specialized_draws++;
                return vita_cfg5[fa];
            }

            // CFG9: D + B*ras color, TEXA*register alpha
            if (s0->color_a==15 && s0->color_c==10 &&
                is_preresolvable(s0->color_b) &&
                is_preresolvable(s0->color_d) &&
                s0->alpha_a==7 && s0->alpha_d==7 &&
                is_texa_times_register(s0->alpha_b, s0->alpha_c) &&
                vita_cfg9[fa]) {
                vita_tev_specialized_draws++;
                return vita_cfg9[fa];
            }

            // CFG8: ocean edge lerp(reg,reg,tex) color + register*TEXA alpha
            if (s0->color_c==8 && s0->color_d==15 &&
                is_preresolvable(s0->color_a) &&
                is_preresolvable(s0->color_b) &&
                s0->alpha_a==7 && s0->alpha_c==4 && s0->alpha_d==7 &&
                ((s0->alpha_b >= 1 && s0->alpha_b <= 3) || s0->alpha_b == 6) &&
                vita_cfg8[fa]) {
                vita_tev_specialized_draws++;
                vita_tev_is_ocean = 1;
                return vita_cfg8[fa];
            }

            // CFG33: lerp(TEXA_clr, TEXC, A2) color + TEXA alpha (1-stage)
            if (s0->color_a==4  && s0->color_b==8  && s0->color_c==3  && s0->color_d==15 &&
                s0->alpha_a==7  && s0->alpha_b==7  && s0->alpha_c==7  && s0->alpha_d==4 &&
                vita_cfg33[fa] && !VITA_CFG_DISABLED(33)) {
                vita_tev_specialized_draws++;
                return vita_cfg33[fa];
            }

            // simple shader (tex*ras): MODULATE and PASSTHROUGH patterns
            {
                int ca = s0->color_a, cb = s0->color_b, cc = s0->color_c, cd = s0->color_d;
                int aa = s0->alpha_a, ab = s0->alpha_b, ac = s0->alpha_c, ad = s0->alpha_d;

                // PASSTHROUGH: result=D
                // Simple shader does tex*ras; for pure-texture passthrough
                // (D=TEXC/TEXA, no B*C), ras must be neutralized to (1,1,1,1)
                // so output = tex*1 = tex. Set vita_tev_passthrough flag to
                // force num_chans=0 in the command snapshot.
                if (ca == 15 && (cb == 15 || cc == 15) &&
                    aa == 7  && (ab == 7  || ac == 7) &&
                    (cd == 8 || cd == 10 || cd == 15) &&
                    (ad == 4 || ad == 5  || ad == 7) &&
                    vita_simple[fa]) {
                    // Flag: D references texture but not ras → neutralize ras
                    if (cd == 8 || ad == 4)
                        vita_tev_passthrough = 1;
                    return vita_simple[fa];
                }

                // MODULATE: result = B*C
                if (cd == 15 && ca == 15 && ad == 7 && aa == 7 &&
                    (cb == 8 || cb == 10 || cb == 15) &&
                    (cc == 8 || cc == 10 || cc == 15) &&
                    (ab == 4 || ab == 5  || ab == 7) &&
                    (ac == 4 || ac == 5  || ac == 7) &&
                    vita_simple[fa]) {
                    return vita_simple[fa];
                }
            }
        }
    }

    // CFG14-21: catch remaining configs (~3-5 ALU vs ~35 uber)
    if (vita_tev_ops_trivial(state)) {
        PCGXTevStage* t0 = &state->tev_stages[0];

        if (state->num_tev_stages <= 1) {
            // CFG14: tex*ras color + register alpha D
            if (t0->color_a==15 && t0->color_d==15 &&
                ((t0->color_b==8 && t0->color_c==10) || (t0->color_b==10 && t0->color_c==8)) &&
                t0->alpha_a==7 && t0->alpha_b==7 && t0->alpha_c==7 &&
                t0->alpha_d != 4 && t0->alpha_d != 5 &&
                vita_cfg14[fa]) {
                vita_tev_specialized_draws++;
                return vita_cfg14[fa];
            }

            // CFG37: tex*ras color + TEXA alpha (island terrain)
            if (t0->color_a==15 && t0->color_d==15 &&
                ((t0->color_b==8 && t0->color_c==10) || (t0->color_b==10 && t0->color_c==8)) &&
                t0->alpha_a==7 && t0->alpha_b==7 && t0->alpha_c==7 && t0->alpha_d==4 &&
                vita_cfg37[fa] && !VITA_CFG_DISABLED(37)) {
                vita_tev_specialized_draws++;
                return vita_cfg37[fa];
            }

            // CFG17: lerp(reg,reg,tex) color + TEXA alpha
            if (t0->color_c==8 && t0->color_d==15 &&
                is_preresolvable(t0->color_a) &&
                is_preresolvable(t0->color_b) &&
                t0->alpha_a==7 && t0->alpha_b==7 && t0->alpha_c==7 && t0->alpha_d==4 &&
                vita_cfg17[fa]) {
                vita_tev_specialized_draws++;
                return vita_cfg17[fa];
            }

            // CFG18: register*tex color + TEXA alpha
            if (t0->color_a==15 && t0->color_b==8 && t0->color_d==15 &&
                is_preresolvable(t0->color_c) &&
                t0->alpha_a==7 && t0->alpha_b==7 && t0->alpha_c==7 && t0->alpha_d==4 &&
                vita_cfg18[fa]) {
                vita_tev_specialized_draws++;
                return vita_cfg18[fa];
            }

            // CFG19: reg+reg*reg color + reg*TEXA alpha
            if (t0->color_a==15 &&
                is_preresolvable(t0->color_b) &&
                is_preresolvable(t0->color_c) && t0->color_c != 15 &&
                is_preresolvable(t0->color_d) &&
                t0->alpha_a==7 && t0->alpha_d==7 &&
                is_texa_times_register(t0->alpha_b, t0->alpha_c) &&
                vita_cfg19[fa]) {
                vita_tev_specialized_draws++;
                return vita_cfg19[fa];
            }
        } else if (state->num_tev_stages == 2) {
            PCGXTevStage* t1 = &state->tev_stages[1];

            // CFG15: lerp(reg,reg,tex) + TEXA chain alpha (2-stage)
            if (t0->color_c==8 && t0->color_d==15 &&
                t0->color_a >= 2 && t0->color_a <= 7 &&
                t0->color_b >= 2 && t0->color_b <= 7 &&
                t0->alpha_a==7 && t0->alpha_b==7 && t0->alpha_c==7 && t0->alpha_d==4 &&
                t1->color_a==15 && t1->color_b==15 && t1->color_c==15 && t1->color_d==0 &&
                t1->alpha_a==7 && t1->alpha_b==0 && t1->alpha_c==4 && t1->alpha_d==4 &&
                vita_cfg15[fa]) {
                vita_tev_specialized_draws++;
                return vita_cfg15[fa];
            }

            // CFG16: register color + reg*TEXA chain alpha (2-stage)
            if (t0->color_a==15 && t0->color_b==15 && t0->color_c==15 &&
                t0->color_d >= 2 && t0->color_d <= 7 &&
                t0->alpha_a==7 && t0->alpha_d==7 &&
                ((t0->alpha_b==4 && t0->alpha_c >= 1 && t0->alpha_c <= 3) ||
                 (t0->alpha_c==4 && t0->alpha_b >= 1 && t0->alpha_b <= 3)) &&
                t1->color_a==15 && t1->color_b==15 && t1->color_c==15 && t1->color_d==0 &&
                t1->alpha_a==7 && t1->alpha_d==7 &&
                t1->alpha_b==0 && t1->alpha_c >= 1 && t1->alpha_c <= 3 &&
                vita_cfg16[fa]) {
                vita_tev_specialized_draws++;
                return vita_cfg16[fa];
            }

            // CFG20: reg*(1+tex) color + TEXA alpha (uses TEXUNIT0 only, CPU remaps)
            if (t0->color_a==15 &&
                is_preresolvable(t0->color_b) &&
                is_preresolvable(t0->color_c) && t0->color_c != 15 &&
                is_preresolvable(t0->color_d) &&
                t0->alpha_a==7 && t0->alpha_b==7 && t0->alpha_c==7 && t0->alpha_d==7 &&
                t1->color_a==15 && t1->color_b==0 && t1->color_c==8 && t1->color_d==0 &&
                t1->alpha_a==4 && t1->alpha_b==7 && t1->alpha_c==7 && t1->alpha_d==7 &&
                vita_cfg20[fa]) {
                vita_tev_specialized_draws++;
                vita_tev_tex_remap = 1;
                return vita_cfg20[fa];
            }

            // CFG21: reg color (A-slot) + TEXA*reg*TEXA alpha (2-stage)
            if (t0->color_b==15 && t0->color_c==15 && t0->color_d==15 &&
                is_preresolvable(t0->color_a) &&
                t0->alpha_a==7 && t0->alpha_d==7 &&
                is_texa_times_register(t0->alpha_b, t0->alpha_c) &&
                t1->color_a==15 && t1->color_b==15 && t1->color_c==15 && t1->color_d==0 &&
                t1->alpha_a==7 && t1->alpha_b==0 && t1->alpha_c==4 && t1->alpha_d==7 &&
                vita_cfg21[fa]) {
                vita_tev_specialized_draws++;
                return vita_cfg21[fa];
            }
        }
    }

    // CFG29-32: remaining 2-stage waterfall configs
    if (vita_tev_ops_trivial(state) && state->num_tev_stages == 2) {
        PCGXTevStage* w0 = &state->tev_stages[0];
        PCGXTevStage* w1 = &state->tev_stages[1];

        // CFG29: white color (saturated: ONE+RASC*TEXA), tex1a*tex0a alpha
        if (w0->color_a==15 && w0->color_b==10 && w0->color_c==4  && w0->color_d==6 &&
            w0->alpha_a==7  && w0->alpha_b==7  && w0->alpha_c==7  && w0->alpha_d==4 &&
            w1->color_a==15 && w1->color_b==8  && w1->color_c==3  && w1->color_d==0 &&
            w1->alpha_a==7  && w1->alpha_b==0  && w1->alpha_c==4  && w1->alpha_d==7 &&
            vita_cfg29[fa] && !VITA_CFG_DISABLED(29)) {
            vita_tev_specialized_draws++;
            return vita_cfg29[fa];
        }

        // CFG30: TEXA_clr+TEXC additive, tex1+ras*(TEXA_clr+TEXC) color, tex1a*A0*tex0a alpha
        if (w0->color_a==8  && w0->color_b==15 && w0->color_c==15 && w0->color_d==4 &&
            w0->alpha_a==7  && w0->alpha_b==4  && w0->alpha_c==1  && w0->alpha_d==7 &&
            w1->color_a==15 && w1->color_b==0  && w1->color_c==10 && w1->color_d==8 &&
            w1->alpha_a==7  && w1->alpha_b==0  && w1->alpha_c==4  && w1->alpha_d==7 &&
            vita_cfg30[fa] && !VITA_CFG_DISABLED(30)) {
            vita_tev_specialized_draws++;
            return vita_cfg30[fa];
        }

        // CFG31: white color (saturated: ONE+A2*TEXC), tex1a*(1+tex0a) alpha
        if (w0->color_a==15 && w0->color_b==8  && w0->color_c==3  && w0->color_d==6 &&
            w0->alpha_a==7  && w0->alpha_b==7  && w0->alpha_c==7  && w0->alpha_d==4 &&
            w1->color_a==15 && w1->color_b==10 && w1->color_c==4  && w1->color_d==0 &&
            w1->alpha_a==7  && w1->alpha_b==0  && w1->alpha_c==4  && w1->alpha_d==4 &&
            vita_cfg31[fa] && !VITA_CFG_DISABLED(31)) {
            vita_tev_specialized_draws++;
            return vita_cfg31[fa];
        }

        // CFG32: TEXC*(1+RASC)+TEXA_clr*RASC color, tex1a*tex0a alpha
        if (w0->color_a==15 && w0->color_b==8  && w0->color_c==10 && w0->color_d==8 &&
            w0->alpha_a==7  && w0->alpha_b==7  && w0->alpha_c==7  && w0->alpha_d==4 &&
            w1->color_a==15 && w1->color_b==10 && w1->color_c==4  && w1->color_d==0 &&
            w1->alpha_a==7  && w1->alpha_b==0  && w1->alpha_c==4  && w1->alpha_d==7 &&
            vita_cfg32[fa] && !VITA_CFG_DISABLED(32)) {
            vita_tev_specialized_draws++;
            return vita_cfg32[fa];
        }
    }

    // complex TEV fallback (uber-shader); 3-stage = ocean, else rain/particles
use_complex:
    vita_tev_complex_draws++;
    if (state->num_tev_stages >= 3)
        vita_tev_is_ocean = 1;
    tev_config_record(state);
    if (state->num_tev_stages > 1) key |= VITA_VF_TEV2;
    if (vita_variants[key] != 0) return vita_variants[key];
    return vita_variants[0];
}

#else
// PC: single uber-shader

static GLuint default_program = 0;

void pc_gx_tev_init(void) {
    char* vs_src = load_shader("default.vert");
    char* fs_src = load_shader("default.frag");

    if (!vs_src || !fs_src) {
        fprintf(stderr, "FATAL: Shader files missing from shaders/ directory.\n"
                        "Expected: shaders/default.vert and shaders/default.frag\n"
                        "Make sure shader files are next to the executable.\n");
        free(vs_src);
        free(fs_src);
        exit(1);
    }

    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    default_program = link_program(vs, fs);

    free(vs_src);
    free(fs_src);
}

void pc_gx_tev_shutdown(void) {
    if (default_program) {
        glDeleteProgram(default_program);
        default_program = 0;
    }
}

GLuint pc_gx_tev_get_shader(PCGXState* state) {
    return default_program;
}

#endif /* TARGET_VITA */
