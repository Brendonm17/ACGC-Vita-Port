// pc_controls.c
// loader for controls.ini

#include "pc_controls.h"
#include "pc_platform.h"

#include <dolphin/pad.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

PCControls g_pc_controls = {
    .main_map = {
        [PCV_CROSS]      = PAD_BUTTON_A,
        [PCV_CIRCLE]     = PAD_BUTTON_B,
        [PCV_SQUARE]     = PAD_BUTTON_X,
        [PCV_TRIANGLE]   = PAD_BUTTON_Y,
        [PCV_SELECT]     = PAD_TRIGGER_Z,
        [PCV_START]      = PAD_BUTTON_START,
        [PCV_L]          = PAD_TRIGGER_L,
        [PCV_R]          = PAD_TRIGGER_R,
        [PCV_DPAD_UP]    = PAD_BUTTON_UP,
        [PCV_DPAD_DOWN]  = PAD_BUTTON_DOWN,
        [PCV_DPAD_LEFT]  = PAD_BUTTON_LEFT,
        [PCV_DPAD_RIGHT] = PAD_BUTTON_RIGHT,
        // stick virtual buttons stay 0; analog flow is in main_axis_map
    },
    .nes_map = {
        [PCV_CROSS]        = NES_BIT_A,
        [PCV_CIRCLE]       = NES_BIT_B,
        [PCV_SELECT]       = NES_BIT_SELECT,
        [PCV_START]        = NES_BIT_START,
        [PCV_DPAD_UP]      = NES_BIT_UP,
        [PCV_DPAD_DOWN]    = NES_BIT_DOWN,
        [PCV_DPAD_LEFT]    = NES_BIT_LEFT,
        [PCV_DPAD_RIGHT]   = NES_BIT_RIGHT,
        // left stick mirrors d-pad by default
        [PCV_LSTICK_UP]    = NES_BIT_UP,
        [PCV_LSTICK_DOWN]  = NES_BIT_DOWN,
        [PCV_LSTICK_LEFT]  = NES_BIT_LEFT,
        [PCV_LSTICK_RIGHT] = NES_BIT_RIGHT,
    },
    .nes_turbo_map = {
        [PCV_SQUARE]   = NES_TURBO_A,
        [PCV_TRIANGLE] = NES_TURBO_B,
    },
    .main_axis_map = {
        [PCA_LSTICK_X] = { PCG_AXIS_MAIN_X,   +1 },
        [PCA_LSTICK_Y] = { PCG_AXIS_MAIN_Y,   +1 },
        [PCA_RSTICK_X] = { PCG_AXIS_CSTICK_X, +1 },
        [PCA_RSTICK_Y] = { PCG_AXIS_CSTICK_Y, +1 },
    },
    .analog_deadzone   = 4000,
    .digital_threshold = 12000,
};

#ifdef TARGET_VITA
static const char* CONTROLS_FILE = "ux0:data/AnimalCrossing/controls.ini";
#else
static const char* CONTROLS_FILE = "controls.ini";
#endif

static const char* DEFAULT_CONTROLS =
    "# Vita buttons: cross, circle, square, triangle, select, start, l, r,\n"
    "#               dpad_up, dpad_down, dpad_left, dpad_right\n"
    "# Values can be a single name, a comma-separated chord, or 'none'.\n"
    "# Stick directions (lstick_up/down/left/right, rstick_*) can also bind.\n"
    "\n"
    "[Controls.Main]\n"
    "# GC: a, b, x, y, start, z, l, r, dup, ddown, dleft, dright, none\n"
    "cross      = a\n"
    "circle     = b\n"
    "square     = x\n"
    "triangle   = y\n"
    "select     = z\n"
    "start      = start\n"
    "l          = l\n"
    "r          = r\n"
    "dpad_up    = dup\n"
    "dpad_down  = ddown\n"
    "dpad_left  = dleft\n"
    "dpad_right = dright\n"
    "\n"
    "# Analog sticks. GC axes: main_x, main_y, cstick_x, cstick_y, none.\n"
    "# Prefix with - to invert (e.g. rstick_y = -cstick_y).\n"
    "lstick_x = main_x\n"
    "lstick_y = main_y\n"
    "rstick_x = cstick_x\n"
    "rstick_y = cstick_y\n"
    "\n"
    "[Controls.NES]\n"
    "# NES: a, b, select, start, up, down, left, right, turbo_a, turbo_b, none\n"
    "cross        = a\n"
    "circle       = b\n"
    "square       = turbo_a\n"
    "triangle     = turbo_b\n"
    "select       = select\n"
    "start        = start\n"
    "l            = none\n"
    "r            = none\n"
    "dpad_up      = up\n"
    "dpad_down    = down\n"
    "dpad_left    = left\n"
    "dpad_right   = right\n"
    "# Left stick mirrors the D-pad so you can play NES with either.\n"
    "lstick_up    = up\n"
    "lstick_down  = down\n"
    "lstick_left  = left\n"
    "lstick_right = right\n"
    "\n"
    "[Sticks]\n"
    "# Raw stick values are 0-32767. Below 'deadzone' the analog axis is treated\n"
    "# as centered. 'digital_threshold' is how far you must push for the virtual\n"
    "# stick directions (lstick_up etc.) to fire.\n"
    "deadzone          = 4000\n"
    "digital_threshold = 12000\n";

typedef struct { const char* name; int      value; } NameEntry;
typedef struct { const char* name; uint16_t bit;   } GCBitEntry;
typedef struct { const char* name; uint8_t  bit; uint8_t is_turbo; } NesBitEntry;

static const NameEntry s_vita_names[] = {
    { "cross",        PCV_CROSS },
    { "circle",       PCV_CIRCLE },
    { "square",       PCV_SQUARE },
    { "triangle",     PCV_TRIANGLE },
    { "select",       PCV_SELECT },
    { "back",         PCV_SELECT },
    { "start",        PCV_START },
    { "l",            PCV_L },
    { "lb",           PCV_L },
    { "l1",           PCV_L },
    { "r",            PCV_R },
    { "rb",           PCV_R },
    { "r1",           PCV_R },
    { "dpad_up",      PCV_DPAD_UP },
    { "dpad_down",    PCV_DPAD_DOWN },
    { "dpad_left",    PCV_DPAD_LEFT },
    { "dpad_right",   PCV_DPAD_RIGHT },
    { "lstick_up",    PCV_LSTICK_UP },
    { "lstick_down",  PCV_LSTICK_DOWN },
    { "lstick_left",  PCV_LSTICK_LEFT },
    { "lstick_right", PCV_LSTICK_RIGHT },
    { "rstick_up",    PCV_RSTICK_UP },
    { "rstick_down",  PCV_RSTICK_DOWN },
    { "rstick_left",  PCV_RSTICK_LEFT },
    { "rstick_right", PCV_RSTICK_RIGHT },
};

static const NameEntry s_vita_axis_names[] = {
    { "lstick_x", PCA_LSTICK_X },
    { "lstick_y", PCA_LSTICK_Y },
    { "rstick_x", PCA_RSTICK_X },
    { "rstick_y", PCA_RSTICK_Y },
};

static const NameEntry s_gc_axis_names[] = {
    { "none",     PCG_AXIS_NONE },
    { "main_x",   PCG_AXIS_MAIN_X },
    { "main_y",   PCG_AXIS_MAIN_Y },
    { "cstick_x", PCG_AXIS_CSTICK_X },
    { "cstick_y", PCG_AXIS_CSTICK_Y },
};

static const GCBitEntry s_gc_bits[] = {
    { "none",   0 },
    { "a",      PAD_BUTTON_A },
    { "b",      PAD_BUTTON_B },
    { "x",      PAD_BUTTON_X },
    { "y",      PAD_BUTTON_Y },
    { "start",  PAD_BUTTON_START },
    { "z",      PAD_TRIGGER_Z },
    { "l",      PAD_TRIGGER_L },
    { "r",      PAD_TRIGGER_R },
    { "dup",    PAD_BUTTON_UP },
    { "ddown",  PAD_BUTTON_DOWN },
    { "dleft",  PAD_BUTTON_LEFT },
    { "dright", PAD_BUTTON_RIGHT },
};

static const NesBitEntry s_nes_bits[] = {
    { "none",    0,              0 },
    { "a",       NES_BIT_A,      0 },
    { "b",       NES_BIT_B,      0 },
    { "select",  NES_BIT_SELECT, 0 },
    { "start",   NES_BIT_START,  0 },
    { "up",      NES_BIT_UP,     0 },
    { "down",    NES_BIT_DOWN,   0 },
    { "left",    NES_BIT_LEFT,   0 },
    { "right",   NES_BIT_RIGHT,  0 },
    { "turbo_a", NES_TURBO_A,    1 },
    { "turbo_b", NES_TURBO_B,    1 },
};

#define COUNT_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))

static int lookup_vita(const char* s) {
    for (int i = 0; i < COUNT_OF(s_vita_names); i++) {
        if (strcasecmp(s_vita_names[i].name, s) == 0) return s_vita_names[i].value;
    }
    return -1;
}

static int lookup_vita_axis(const char* s) {
    for (int i = 0; i < COUNT_OF(s_vita_axis_names); i++) {
        if (strcasecmp(s_vita_axis_names[i].name, s) == 0) return s_vita_axis_names[i].value;
    }
    return -1;
}

static int lookup_gc_axis(const char* s) {
    for (int i = 0; i < COUNT_OF(s_gc_axis_names); i++) {
        if (strcasecmp(s_gc_axis_names[i].name, s) == 0) return s_gc_axis_names[i].value;
    }
    return -1;
}

static const GCBitEntry* lookup_gc(const char* s) {
    for (int i = 0; i < COUNT_OF(s_gc_bits); i++) {
        if (strcasecmp(s_gc_bits[i].name, s) == 0) return &s_gc_bits[i];
    }
    return NULL;
}

static const NesBitEntry* lookup_nes(const char* s) {
    for (int i = 0; i < COUNT_OF(s_nes_bits); i++) {
        if (strcasecmp(s_nes_bits[i].name, s) == 0) return &s_nes_bits[i];
    }
    return NULL;
}

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

typedef enum { SECTION_NONE, SECTION_MAIN, SECTION_NES, SECTION_STICKS } Section;

static Section parse_section(const char* line) {
    if (strncasecmp(line, "[controls.main]", 15) == 0) return SECTION_MAIN;
    if (strncasecmp(line, "[controls.nes]",  14) == 0) return SECTION_NES;
    if (strncasecmp(line, "[sticks]",         8) == 0) return SECTION_STICKS;
    return SECTION_NONE;
}

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// cb returns 0 on accept, -1 on unknown token
static void for_each_token(char* value, int (*cb)(const char* tok, void* ctx), void* ctx) {
    char* save = NULL;
    char* tok;
    char* s = value;
    while ((tok = strtok_r(s, ",", &save)) != NULL) {
        s = NULL;
        while (*tok == ' ' || *tok == '\t') tok++;
        trim_end(tok);
        if (*tok == '\0') continue;
        if (cb(tok, ctx) < 0) {
            printf("[Controls] WARNING: unknown binding '%s'\n", tok);
        }
    }
}

static int gc_token_cb(const char* tok, void* ctx) {
    const GCBitEntry* e = lookup_gc(tok);
    if (!e) return -1;
    *(uint16_t*)ctx |= e->bit;
    return 0;
}

typedef struct { uint8_t bits; uint8_t turbo; } NesAccum;

static int nes_token_cb(const char* tok, void* ctx) {
    const NesBitEntry* e = lookup_nes(tok);
    if (!e) return -1;
    NesAccum* acc = (NesAccum*)ctx;
    if (e->is_turbo) acc->turbo |= e->bit;
    else             acc->bits  |= e->bit;
    return 0;
}

// nes section ignores axis lines (it uses the stick virtual buttons instead)
static void apply_axis_binding(int vaxis, const char* value) {
    const char* p = value;
    int invert = +1;
    if (*p == '-') { invert = -1; p++; while (*p == ' ' || *p == '\t') p++; }
    else if (*p == '+') { p++; while (*p == ' ' || *p == '\t') p++; }

    int gc_axis = lookup_gc_axis(p);
    if (gc_axis < 0) {
        printf("[Controls] WARNING: unknown GC axis '%s'\n", p);
        return;
    }
    g_pc_controls.main_axis_map[vaxis].target = (PCGCAxis)gc_axis;
    g_pc_controls.main_axis_map[vaxis].invert = (gc_axis == PCG_AXIS_NONE) ? +1 : (int8_t)invert;
}

static void apply_sticks_setting(const char* key, const char* value) {
    int v = atoi(value);
    if (strcasecmp(key, "deadzone") == 0) {
        g_pc_controls.analog_deadzone = clamp_int(v, 0, 32000);
    } else if (strcasecmp(key, "digital_threshold") == 0) {
        g_pc_controls.digital_threshold = clamp_int(v, 1, 32000);
    } else {
        printf("[Controls] WARNING: unknown [Sticks] key '%s'\n", key);
    }
}

static void apply_binding(Section section, const char* key, char* value) {
    if (section == SECTION_STICKS) { apply_sticks_setting(key, value); return; }

    if (section == SECTION_MAIN) {
        int vaxis = lookup_vita_axis(key);
        if (vaxis >= 0) { apply_axis_binding(vaxis, value); return; }
    }

    int vbtn = lookup_vita(key);
    if (vbtn < 0) {
        printf("[Controls] WARNING: unknown vita button '%s'\n", key);
        return;
    }
    if (section == SECTION_MAIN) {
        uint16_t out = 0;
        for_each_token(value, gc_token_cb, &out);
        g_pc_controls.main_map[vbtn] = out;
    } else if (section == SECTION_NES) {
        NesAccum acc = {0, 0};
        for_each_token(value, nes_token_cb, &acc);
        g_pc_controls.nes_map[vbtn]       = acc.bits;
        g_pc_controls.nes_turbo_map[vbtn] = acc.turbo;
    }
}

static void write_defaults(const char* path) {
    FILE* f = fopen(path, "w");
    if (f) {
        fputs(DEFAULT_CONTROLS, f);
        fclose(f);
    }
}

void pc_controls_load(void) {
    FILE* f = fopen(CONTROLS_FILE, "r");
    if (!f) {
        write_defaults(CONTROLS_FILE);
        printf("[Controls] Created default %s\n", CONTROLS_FILE);
        return;
    }

    Section section = SECTION_NONE;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        const char* p = skip_ws(line);
        if (*p == '#' || *p == ';' || *p == '\0' || *p == '\n') continue;
        if (*p == '[') {
            char buf[64];
            size_t n = strlen(p);
            if (n >= sizeof(buf)) n = sizeof(buf) - 1;
            memcpy(buf, p, n);
            buf[n] = '\0';
            trim_end(buf);
            section = parse_section(buf);
            continue;
        }

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key   = (char*)skip_ws(line);
        trim_end(key);
        char* value = (char*)skip_ws(eq + 1);
        trim_end(value);
        if (*key) apply_binding(section, key, value);
    }
    fclose(f);
    printf("[Controls] Loaded %s\n", CONTROLS_FILE);
}

// Multi-bit chords get re-encoded as comma-separated names.
static void gc_bits_to_name(uint16_t bits, char* out, int out_size) {
    if (bits == 0) { snprintf(out, out_size, "none"); return; }
    out[0] = '\0';
    int written = 0;
    for (int i = 1; i < COUNT_OF(s_gc_bits); i++) {  // skip "none" at index 0
        if (bits & s_gc_bits[i].bit) {
            int n = snprintf(out + written, out_size - written,
                             "%s%s", (written > 0) ? ", " : "", s_gc_bits[i].name);
            if (n > 0 && written + n < out_size) written += n;
        }
    }
    if (written == 0) snprintf(out, out_size, "none");
}

static void nes_bits_to_name(uint8_t bits, uint8_t turbo, char* out, int out_size) {
    if (bits == 0 && turbo == 0) { snprintf(out, out_size, "none"); return; }
    out[0] = '\0';
    int written = 0;
    for (int i = 1; i < COUNT_OF(s_nes_bits); i++) {
        const NesBitEntry* e = &s_nes_bits[i];
        uint8_t src = e->is_turbo ? turbo : bits;
        if (src & e->bit) {
            int n = snprintf(out + written, out_size - written,
                             "%s%s", (written > 0) ? ", " : "", e->name);
            if (n > 0 && written + n < out_size) written += n;
        }
    }
    if (written == 0) snprintf(out, out_size, "none");
}

// PCAxisBind -> "main_x" or "-cstick_y".
static void gc_axis_to_name(PCAxisBind bind, char* out, int out_size) {
    const char* axis = "none";
    for (int i = 0; i < COUNT_OF(s_gc_axis_names); i++) {
        if ((PCGCAxis)s_gc_axis_names[i].value == bind.target) {
            axis = s_gc_axis_names[i].name;
            break;
        }
    }
    snprintf(out, out_size, "%s%s", (bind.invert < 0) ? "-" : "", axis);
}

const char* pc_controls_vita_name(int vbtn) {
    if (vbtn < 0 || vbtn >= PCV_COUNT) return "?";
    // first entry per button is the canonical name; aliases come later
    for (int i = 0; i < COUNT_OF(s_vita_names); i++) {
        if (s_vita_names[i].value == vbtn) return s_vita_names[i].name;
    }
    return "?";
}

void pc_controls_format_main(int vbtn, char* out, int out_size) {
    if (vbtn < 0 || vbtn >= PCV_COUNT) { snprintf(out, out_size, "?"); return; }
    gc_bits_to_name(g_pc_controls.main_map[vbtn], out, out_size);
}

void pc_controls_format_nes(int vbtn, char* out, int out_size) {
    if (vbtn < 0 || vbtn >= PCV_COUNT) { snprintf(out, out_size, "?"); return; }
    nes_bits_to_name(g_pc_controls.nes_map[vbtn], g_pc_controls.nes_turbo_map[vbtn], out, out_size);
}

void pc_controls_set_main(int vbtn, uint16_t bits) {
    if (vbtn < 0 || vbtn >= PCV_COUNT) return;
    g_pc_controls.main_map[vbtn] = bits;
}

void pc_controls_set_nes(int vbtn, uint8_t bits, uint8_t turbo) {
    if (vbtn < 0 || vbtn >= PCV_COUNT) return;
    g_pc_controls.nes_map[vbtn] = bits;
    g_pc_controls.nes_turbo_map[vbtn] = turbo;
}

// Comments are regenerated each time so the file stays self-documenting.
void pc_controls_save(void) {
    FILE* f = fopen(CONTROLS_FILE, "w");
    if (!f) {
        printf("[Controls] Failed to write %s\n", CONTROLS_FILE);
        return;
    }
    char buf[64];

    fputs("# Vita buttons: cross, circle, square, triangle, select, start, l, r,\n", f);
    fputs("#               dpad_up, dpad_down, dpad_left, dpad_right\n", f);
    fputs("# Values can be a single name, a comma-separated chord, or 'none'.\n", f);
    fputs("# Stick directions (lstick_up/down/left/right, rstick_*) can also bind.\n\n", f);

    fputs("[Controls.Main]\n", f);
    fputs("# GC: a, b, x, y, start, z, l, r, dup, ddown, dleft, dright, none\n", f);
    for (int i = 0; i < PCV_COUNT; i++) {
        gc_bits_to_name(g_pc_controls.main_map[i], buf, sizeof(buf));
        fprintf(f, "%-12s = %s\n", pc_controls_vita_name(i), buf);
    }
    fputs("\n# Analog sticks. GC axes: main_x, main_y, cstick_x, cstick_y, none.\n", f);
    fputs("# Prefix with - to invert (e.g. rstick_y = -cstick_y).\n", f);
    {
        static const char* axis_names[PCA_COUNT] = { "lstick_x", "lstick_y", "rstick_x", "rstick_y" };
        for (int i = 0; i < PCA_COUNT; i++) {
            gc_axis_to_name(g_pc_controls.main_axis_map[i], buf, sizeof(buf));
            fprintf(f, "%-12s = %s\n", axis_names[i], buf);
        }
    }

    fputs("\n[Controls.NES]\n", f);
    fputs("# NES: a, b, select, start, up, down, left, right, turbo_a, turbo_b, none\n", f);
    for (int i = 0; i < PCV_COUNT; i++) {
        nes_bits_to_name(g_pc_controls.nes_map[i], g_pc_controls.nes_turbo_map[i], buf, sizeof(buf));
        fprintf(f, "%-12s = %s\n", pc_controls_vita_name(i), buf);
    }

    fputs("\n[Sticks]\n", f);
    fprintf(f, "deadzone          = %d\n", g_pc_controls.analog_deadzone);
    fprintf(f, "digital_threshold = %d\n", g_pc_controls.digital_threshold);

    fclose(f);
    printf("[Controls] Saved %s\n", CONTROLS_FILE);
}

uint16_t pc_controls_apply_main(const uint8_t pressed[PCV_COUNT]) {
    uint16_t out = 0;
    for (int i = 0; i < PCV_COUNT; i++) {
        if (pressed[i]) out |= g_pc_controls.main_map[i];
    }
    return out;
}

uint8_t pc_controls_apply_nes(const uint8_t pressed[PCV_COUNT], int turbo_phase) {
    uint8_t out = 0;
    for (int i = 0; i < PCV_COUNT; i++) {
        if (!pressed[i]) continue;
        out |= g_pc_controls.nes_map[i];
        if (turbo_phase) {
            uint8_t t = g_pc_controls.nes_turbo_map[i];
            if (t & NES_TURBO_A) out |= NES_BIT_A;
            if (t & NES_TURBO_B) out |= NES_BIT_B;
        }
    }
    return out;
}
