// pc_mp_chat.c - chat between the players in town: speech balloons over heads (the item-name balloon's art),
// typing dots, a line at the top for players off this screen, and the balloon button that opens a keyboard
#include "pc_mp.h"

#ifdef VITA_MP

#include "m_common_data.h"
#include "m_actor.h"
#include "m_editor_ovl.h"
#include "m_event.h"
#include "m_font.h"
#include "m_ledit_ovl.h"
#include "m_lib.h"
#include "m_msg.h"
#include "m_play.h"
#include "m_player_lib.h"
#include "m_skin_matrix.h"
#include "m_string.h"
#include "m_submenu.h"
#include "m_watch_my_step.h"
#include "sys_matrix.h"
#include "ac_mp_player.h"
#include "pc_settings.h"
#include "libforest/gbi_extensions.h"

#include <math.h>
#include <string.h>

#define MP_CHAT_GAP_MS    1500  // host: a player's next message this soon after its last is dropped
#define MP_CHAT_TYPING_MS 30000 // typing dots nobody switched off go after this long
#define MP_CHAT_SCALE     0.875f
#define MP_CHAT_LINE_W    140.0f // a balloon line on screen: the item-name balloon's widest text
#define MP_CHAT_LINE_H    14.0f
#define MP_CHAT_TOPS      2
#define MP_CHAT_EDGE_OUT  24.0f // a balloon goes to the screen's edge once its speaker's head is this far off it...
#define MP_CHAT_EDGE_IN   32.0f // ...and back over them once they're this far inside it (between: no change)
#define MP_CHAT_EDGE_MS   300   // ...for this long
#define MP_CHAT_SWAP_MS   750   // and never sooner than this after the last change
#define MP_CHAT_TOP_LEN   (MP_NAME_LEN + 2 + MP_CHAT_MAX)
#define MP_CHAT_WHERE     7     // after a message's text: sent indoors, the building's doorway out in town (1, x y z)
#define MP_CHAT_DOOR_BACK 40.0f // the doorway, behind where a player comes back out
#define MP_CHAT_DOOR_UP   40.0f
#define MP_CHAT_NG_NUM    mED_NG_WORD_NUM

extern float g_aspect_factor; // pc_gx.c: widescreen shows more of the UI plane than 0..320
extern int g_aspect_active;

extern u8 fki_win_w2_tex[];
extern Vtx fki_win_v[];

// the item-name balloon's own art (w3T) with the fade's combine (w4): a whole balloon that can fade
static Gfx mp_chat_balloon_model[] = {
    gsDPSetCombineLERP(0, 0, 0, PRIMITIVE, TEXEL0, 0, PRIM_LOD_FRAC, 0, 0, 0, 0, PRIMITIVE, TEXEL0, 0, PRIM_LOD_FRAC,
                       0),
    gsDPSetTextureImage_Dolphin(G_IM_FMT_I, G_IM_SIZ_4b, 64, 32, fki_win_w2_tex),
    gsDPSetTile_Dolphin(G_DOLPHIN_TLUT_DEFAULT_MODE, 0, 15, GX_MIRROR, GX_MIRROR, 0, 0),
    gsSPVertex(&fki_win_v[4], 4, 0),
    gsSPNTrianglesInit_5b(2, 0, 1, 2, 1, 3, 2, 0, 0, 0),
    gsSPEndDisplayList(),
};

enum { MP_KB_IDLE, MP_KB_OPENING, MP_KB_OPEN };
enum { MP_BTN_OFF, MP_BTN_OPEN, MP_BTN_CLOSE }; // the balloon button: hidden, opens a keyboard, closes the one up

typedef struct {
    int start[2];
    int n[2];
    f32 w[2];
    int lines;
} mp_chat_lines_t;

typedef struct {
    u8 text[MP_CHAT_MAX];
    int len;
    int fresh;            // heard, not yet placed: a balloon here, or the line at the top
    unsigned int ms;      // when it came; the balloon shows while it's fresh
    unsigned int show_ms;
    int typing;
    unsigned int typing_ms;
    unsigned int host_ms; // host: when this player's last message went out
    int mode;             // the item-name balloon's steps: 1-2 dots, 3 grown, 4 fading
    int far;              // speaker past the outer line since far_ms
    int near;             // speaker past the inner line since near_ms
    unsigned int far_ms;
    unsigned int near_ms;
    unsigned int swap_ms; // last change between balloon and edge line
    int timer;
    f32 opacity;
    int where; // the message came from inside a building: its doorway in town
    f32 wx;
    f32 wy;
    f32 wz;
} mp_chat_t;

typedef struct {
    u8 text[MP_CHAT_TOP_LEN];
    int len;
    u8 msg[MP_CHAT_MAX]; // the message alone, for its balloon if the speaker comes onto the screen
    int msg_len;
    int slot;
    unsigned int ms;
    unsigned int show_ms;
    f32 opacity;
    int placed;
    f32 cx; // where it sits, easing along the screen's edge as the two players move
    f32 cy;
    int where; // (its speaker's building, as mp_chat_t has it)
    f32 wx;
    f32 wy;
    f32 wz;
} mp_chat_top_t;

static mp_chat_t s_chat[MP_MAX_PEERS];
static mp_chat_top_t s_top[MP_CHAT_TOPS]; // newest first
static struct {
    int state;
    int cancel;     // closed by the button: nothing is sent
    int cancel_req; // ...and the editor hasn't taken the close yet
    u8 buf[MP_CHAT_MAX];
} s_kb;
static struct {
    int on;
    u8 text[MP_CHAT_MAX]; // what the Vita keyboard holds, shown in the player's balloon as it's typed
    int len;
} s_ime;
static struct {
    int waiting;
    u8 text[MP_CHAT_MAX];
    int len;
    unsigned int last_ms;
} s_out;
static u8 s_ng[MP_CHAT_NG_NUM][10];
static int s_ng_loaded;
static f32 s_button_opacity;
static int s_open_next; // tapped: the button goes this frame, the keyboard opens on the next
static volatile int s_tap_new;
static volatile int s_tap_x;
static volatile int s_tap_y;

static int mp_chat_self(void) {
    int s = mp_lobby_self_slot();

    return (s >= 0 && s < MP_MAX_PEERS) ? s : 0;
}

// the part of the UI plane on screen: widescreen shows past 0..320 at the sides
static f32 mp_chat_half_w(void) {
    return (g_pc_settings.aspect_mode == 0 && g_aspect_active && g_aspect_factor > 0.1f) ? 160.0f / g_aspect_factor
                                                                                           : 160.0f;
}

// text

// control codes, message tags and line breaks would steer the font; they arrive as spaces
void mp_text_clean(unsigned char* s, int len) {
    int i;

    for (i = 0; i < len; i++) {
        if (s[i] == CHAR_CONTROL_CODE || s[i] == CHAR_MESSAGE_TAG || s[i] == CHAR_NEW_LINE) {
            s[i] = CHAR_SPACE;
        }
    }
}

static int mp_chat_trim(const u8* s, int len) {
    while (len > 0 && (s[len - 1] == CHAR_SPACE || s[len - 1] == CHAR_SPACE_2 || s[len - 1] == CHAR_SPACE_3)) {
        len--;
    }
    return len;
}

// as much as fits the keyboard's line, so every message wraps to two balloon lines
int mp_chat_fit(const unsigned char* s, int len) {
    int w = 0;
    int i;

    if (len > MP_CHAT_MAX) {
        len = MP_CHAT_MAX;
    }
    for (i = 0; i < len; i++) {
        w += mFont_GetCodeWidth(s[i], TRUE);
        if (w > MP_CHAT_COL_MAX) {
            return i;
        }
    }
    return len;
}

static int mp_chat_lower(int c) {
    return (c >= CHAR_A && c <= CHAR_Z) ? c + (CHAR_a - CHAR_A) : c;
}

// the ROM's list of words names can't have (the name editor's check), in any case
int mp_chat_ng_word(const unsigned char* s, int len) {
    static const int ng_len[MP_CHAT_NG_NUM] = { 4, 4, 4, 4, 4, 5, 5, 7, 7, 7, 7, 7, 9, 10 };
    int i;
    int at;
    int k;

    if (!s_ng_loaded) {
        for (i = 0; i < MP_CHAT_NG_NUM; i++) {
            mString_Load_StringFromRom(s_ng[i], sizeof(s_ng[i]), mED_NG_WORD_START + i);
        }
        s_ng_loaded = TRUE;
    }
    for (i = 0; i < MP_CHAT_NG_NUM; i++) {
        for (at = 0; at + ng_len[i] <= len; at++) {
            for (k = 0; k < ng_len[i]; k++) {
                if (mp_chat_lower(s[at + k]) != mp_chat_lower(s_ng[i][k])) {
                    break;
                }
            }
            if (k == ng_len[i]) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

// what the Vita keyboard types, in the game's letters; -1 has no glyph and is dropped
static int mp_chat_glyph(unsigned int c) {
    static const short latin1[0x60] = {
        CHAR_SPACE, CHAR_INVERT_EXCLAMATION, -1, -1, -1, -1, CHAR_BROKEN_BAR, CHAR_SILCROW, // A0
        -1, -1, CHAR_FEMININE_ORDINAL, CHAR_GUILLEMET_OPEN, CHAR_LOGICAL_NEGATION, -1, -1, CHAR_MACRON_SYMBOL,
        -1, -1, CHAR_SUPERSCRIPT_TWO, CHAR_SUPERSCRIPT_THREE, -1, CHAR_LATIN_MU, -1, CHAR_INTERPUNCT, // B0
        -1, CHAR_SUPRESCRIPT_ONE, CHAR_MASCULINE_ORDINAL, CHAR_GUILLEMET_CLOSE, -1, -1, -1, CHAR_INVERT_QUESTIONMARK,
        CHAR_GRAVE_A, CHAR_ACUTE_A, CHAR_CIRCUMFLEX_A, CHAR_TILDE_A, CHAR_DIAERESIS_A, CHAR_ANGSTROM_A, CHAR_ASH,
        CHAR_CEDILLA, // C0
        CHAR_GRAVE_E, CHAR_ACUTE_E, CHAR_CIRCUMFLEX_E, CHAR_DIARESIS_E, CHAR_GRAVE_I, CHAR_ACUTE_I, CHAR_CIRCUMFLEX_I,
        CHAR_DIARESIS_I,
        CHAR_ETH, CHAR_TILDE_N, CHAR_GRAVE_O, CHAR_ACUTE_O, CHAR_CIRCUMFLEX_O, CHAR_TILDE_O, CHAR_DIARESIS_O,
        CHAR_DIMENSION_SIGN, // D0
        -1, CHAR_GRAVE_U, CHAR_ACUTE_U, CHAR_CIRCUMFLEX_U, CHAR_DIARESIS_U, CHAR_ACUTE_Y, CHAR_THORN, CHAR_LOWER_BETA,
        CHAR_GRAVE_a, CHAR_ACUTE_a, CHAR_CIRCUMFLEX_a, CHAR_TILDE_a, CHAR_DIARESIS_a, CHAR_ANGSTROM_a, CHAR_LOWER_ASH,
        CHAR_LOWER_CEDILLA, // E0
        CHAR_GRAVE_e, CHAR_ACUTE_e, CHAR_CIRCUMFLEX_e, CHAR_DIARESIS_e, CHAR_GRAVE_i, CHAR_ACUTE_i, CHAR_CIRCUMFLEX_i,
        CHAR_DIARESIS_i,
        CHAR_LOWER_ETH, CHAR_TILDE_n, CHAR_GRAVE_o, CHAR_ACUTE_o, CHAR_CIRCUMFLEX_o, CHAR_TILDE_o, CHAR_DIARESIS_o,
        CHAR_OBELUS_SIGN, // F0
        -1, CHAR_GRAVE_u, CHAR_ACUTE_u, CHAR_CIRCUMFLEX_u, CHAR_DIARESIS_u, CHAR_ACUTE_y, CHAR_LOWER_THORN,
        CHAR_DIARESIS_y,
    };

    if (c >= 0x20 && c < 0x7F) {
        switch (c) {
            case '#': return CHAR_HASHTAG;
            case '*': return CHAR_SYMBOL_STAR;
            case '+': return CHAR_PLUS;
            case '/': return CHAR_FORWARD_SLASH;
            case ';': return CHAR_SEMICOLON;
            case '[': case '{': return CHAR_OPEN_PARENTHESIS;
            case ']': case '}': return CHAR_CLOSE_PARENTHESIS;
            case '`': return CHAR_APOSTROPHE;
            case '|': return CHAR_BROKEN_BAR;
            case '~': return CHAR_TILDE;
            case '$': case '\\': case '^': return -1;
            default: return (int)c;
        }
    }
    if (c >= 0xA0 && c <= 0xFF) {
        return latin1[c - 0xA0];
    }
    switch (c) {
        case 0x0152: return CHAR_OE;
        case 0x0153: return CHAR_oe;
        case 0x2010: return CHAR_HYPHEN;
        case 0x2013: case 0x2014: return CHAR_DASH;
        case 0x2018: case 0x2019: return CHAR_APOSTROPHE;
        case 0x201C: case 0x201D: return CHAR_QUOTATION;
        case 0x201E: return CHAR_INVERT_QUOTATION;
        case 0x2026: return CHAR_PERIOD;
        case 0x2016: return CHAR_DOUBLE_VERTICAL_BAR;
        case 0x221E: return CHAR_INFINITY;
        case 0x25CB: return CHAR_CIRCLE;
        case 0x2715: return CHAR_CROSS;
        case 0x25A1: return CHAR_SQUARE;
        case 0x25B3: return CHAR_TRIANGLE;
        case 0x2600: return CHAR_SYMBOL_SUN;
        case 0x2601: return CHAR_SYMBOL_CLOUD;
        case 0x2602: case 0x2614: return CHAR_SYMBOL_UMBRELLA;
        case 0x2603: return CHAR_SYMBOL_SNOWMAN;
        case 0x2605: case 0x2606: return CHAR_SYMBOL_STAR;
        case 0x2620: return CHAR_SYMBOL_SKULL;
        case 0x2639: return CHAR_SYMBOL_SAD;
        case 0x263A: return CHAR_SYMBOL_SMILE;
        case 0x2640: return CHAR_VENUS_SYMBOL;
        case 0x2642: return CHAR_MARS_SYMBOL;
        case 0x2661: case 0x2665: return CHAR_SYMBOL_HEART;
        case 0x266A: case 0x266B: return CHAR_SYMBOL_MUSIC_NOTE;
        case 0x26A1: return CHAR_SYMBOL_LIGTNING;
        case 0x273F: case 0x2740: return CHAR_SYMBOL_FLOWER;
    }
    return -1;
}

static int mp_chat_from_utf16(const unsigned short* in, int n, u8* out) {
    int len = 0;
    int i;

    for (i = 0; i < n && len < MP_CHAT_MAX; i++) {
        int g = mp_chat_glyph(in[i]);

        if (g >= 0) {
            out[len++] = (u8)g;
        }
    }
    return len;
}

static f32 mp_chat_width(const u8* s, int n) {
    return n > 0 ? (f32)mFont_GetStringWidth((u8*)s, n, TRUE) * MP_CHAT_SCALE : 0.0f;
}

static int mp_chat_is_space(u8 c) {
    return c == CHAR_SPACE || c == CHAR_SPACE_2 || c == CHAR_SPACE_3;
}

// the longest run from `at` that fits a line; a line ends at a space when one fits, else mid-word
static int mp_chat_line(const u8* s, int len, int at, int words, int* next) {
    f32 w = 0.0f;
    int last_space = -1;
    int i;

    for (i = at; i < len; i++) {
        w += (f32)mFont_GetCodeWidth(s[i], TRUE) * MP_CHAT_SCALE;
        if (w > MP_CHAT_LINE_W) {
            break;
        }
        if (mp_chat_is_space(s[i])) {
            last_space = i;
        }
    }
    if (i == len) {
        *next = len;
        return len - at;
    }
    if (words && last_space > at) {
        *next = last_space;
        return last_space - at;
    }
    *next = i > at ? i : at + 1;
    return *next - at;
}

// two lines at most: whole words when they fit, else filled to the edge
static void mp_chat_wrap(const u8* s, int len, mp_chat_lines_t* out) {
    int pass;

    for (pass = 0; pass < 2; pass++) {
        int at = 0;
        int k;

        out->lines = 0;
        for (k = 0; k < 2; k++) {
            int next;
            int n;

            while (at < len && mp_chat_is_space(s[at])) {
                at++;
            }
            if (at >= len) {
                break;
            }
            n = mp_chat_line(s, len, at, pass == 0, &next);
            out->start[k] = at;
            out->n[k] = mp_chat_trim(s + at, n);
            out->w[k] = mp_chat_width(s + at, out->n[k]);
            out->lines++;
            at = next;
        }
        while (at < len && mp_chat_is_space(s[at])) {
            at++;
        }
        if (at >= len) {
            return;
        }
    }
    // (still too long for two full lines: the second is cut at its edge)
}

// state

static unsigned int mp_chat_show_ms(int len, int per_char) {
    return 4000 + (unsigned int)(len * per_char);
}

static void mp_chat_top_push(int slot, const u8* text, int len) {
    const mp_look_t* look = mp_player_look(slot);
    mp_chat_top_t* t;
    int n;

    if (look == NULL) {
        return;
    }
    memmove(&s_top[1], &s_top[0], sizeof(s_top[0]) * (MP_CHAT_TOPS - 1));
    t = &s_top[0];
    memset(t, 0, sizeof(*t));
    t->slot = slot;
    n = mp_chat_trim(look->name, MP_NAME_LEN);
    memcpy(t->text, look->name, n);
    t->text[n++] = CHAR_COLON;
    t->text[n++] = CHAR_SPACE;
    memcpy(t->text + n, text, len);
    t->len = n + len;
    memcpy(t->msg, text, len);
    t->msg_len = len;
    t->ms = pc_mp_now_ms();
    t->show_ms = mp_chat_show_ms(len, 60);
    t->where = s_chat[slot].where;
    t->wx = s_chat[slot].wx;
    t->wy = s_chat[slot].wy;
    t->wz = s_chat[slot].wz;
}

// a message to show; the next play frame places it (messages come in during loads too)
static void mp_chat_put(int slot, const u8* text, int len) {
    mp_chat_t* c = &s_chat[slot];

    memcpy(c->text, text, len);
    c->len = len;
    c->fresh = TRUE;
    c->ms = pc_mp_now_ms();
    c->show_ms = mp_chat_show_ms(len, 80);
}

void mp_chat_reset(void) {
    memset(s_chat, 0, sizeof(s_chat));
    memset(s_top, 0, sizeof(s_top));
    s_out.waiting = FALSE;
}

void mp_chat_gone(int slot) {
    if (slot >= 0 && slot < MP_MAX_PEERS) {
        memset(&s_chat[slot], 0, sizeof(s_chat[slot]));
    }
}

// network

static int mp_chat_allowed(void) {
    return mp_active() && mp_rule(MP_RULE_CHAT);
}

static void mp_chat_send_msg(const u8* msg, int len) {
    if (mp_is_host()) {
        mp_player_relay(0, msg, len);
    } else if (mp_lobby_host_conn() >= 0) {
        mp_lobby_send_rel(mp_lobby_host_conn(), msg, len);
    }
}

static void mp_chat_send_typing(int on) {
    u8 msg[3];

    if (!mp_active()) {
        return;
    }
    msg[0] = MP_M_CHAT_TYPING;
    msg[1] = (u8)mp_chat_self();
    msg[2] = (u8)(on != 0);
    mp_chat_send_msg(msg, sizeof(msg));
}

static void mp_chat_w16(u8* p, f32 v) {
    s16 k = (s16)v;

    p[0] = (u8)k;
    p[1] = (u8)((u16)k >> 8);
}

static f32 mp_chat_r16(const u8* p) {
    return (f32)(s16)(p[0] | (p[1] << 8));
}

// indoors: the doorway of the building out in town (behind the spot a player comes back out on), so the players out
// there see the message come from it
static void mp_chat_where_out(u8* p) {
    const Door_data_c* d = Common_GetPointer(structure_exit_door_data);
    s16 angle = (s16)(d->exit_orientation * DEG2SHORT_ANGLE(45.0f));

    memset(p, 0, MP_CHAT_WHERE);
    if (Save_Get(scene_no) == SCENE_FG || d->next_scene_id != SCENE_FG) {
        return;
    }
    p[0] = 1;
    mp_chat_w16(p + 1, d->exit_position.x - sin_s(angle) * MP_CHAT_DOOR_BACK);
    mp_chat_w16(p + 3, d->exit_position.y);
    mp_chat_w16(p + 5, d->exit_position.z - cos_s(angle) * MP_CHAT_DOOR_BACK);
}

static void mp_chat_where_in(mp_chat_t* c, const u8* p) {
    c->where = p != NULL && p[0] == 1;
    if (c->where) {
        c->wx = mp_chat_r16(p + 1);
        c->wy = mp_chat_r16(p + 3);
        c->wz = mp_chat_r16(p + 5);
    }
}

static void mp_chat_send_now(const u8* text, int len) {
    u8 msg[3 + MP_CHAT_MAX + MP_CHAT_WHERE];

    msg[0] = MP_M_CHAT;
    msg[1] = (u8)mp_chat_self();
    msg[2] = (u8)len;
    memcpy(msg + 3, text, len);
    mp_chat_where_out(msg + 3 + len);
    mp_chat_send_msg(msg, 3 + len + MP_CHAT_WHERE);
    s_out.last_ms = pc_mp_now_ms();
    mp_chat_put(mp_chat_self(), text, len);
}

// what a keyboard gave: cleaned, cut to the line, and sent (after the gap the host keeps between messages)
void mp_chat_say(const unsigned char* text, int len) {
    u8 buf[MP_CHAT_MAX];

    if (len > MP_CHAT_MAX) {
        len = MP_CHAT_MAX;
    }
    memcpy(buf, text, len);
    mp_text_clean(buf, len);
    len = mp_chat_trim(buf, mp_chat_fit(buf, len));
    while (len > 0 && mp_chat_is_space(buf[0])) {
        memmove(buf, buf + 1, --len);
    }
    if (len == 0 || !mp_chat_allowed() || mp_chat_ng_word(buf, len)) {
        return;
    }
    if (s_out.last_ms != 0 && pc_mp_now_ms() - s_out.last_ms < MP_CHAT_GAP_MS) {
        memcpy(s_out.text, buf, len);
        s_out.len = len;
        s_out.waiting = TRUE;
        return;
    }
    mp_chat_send_now(buf, len);
}

// MP_M_CHAT / MP_M_CHAT_TYPING from `slot` (already checked to be the sender's own); the host passes on what it keeps
void mp_chat_on_rel(int slot, const unsigned char* data, int len) {
    mp_chat_t* c = &s_chat[slot];

    if (mp_player_look(slot) == NULL) {
        return;
    }
    if (data[0] == MP_M_CHAT_TYPING) {
        if (len < 3 || !mp_rule(MP_RULE_CHAT)) {
            return;
        }
        c->typing = data[2] != 0;
        c->typing_ms = pc_mp_now_ms();
        if (mp_is_host()) {
            mp_player_relay(slot, data, 3);
        }
        return;
    }
    if (len < 3 || data[2] == 0 || data[2] > MP_CHAT_MAX || len < 3 + data[2]) {
        return;
    }
    {
        u8 msg[3 + MP_CHAT_MAX + MP_CHAT_WHERE];
        int n = data[2];
        const u8* where = len >= 3 + n + MP_CHAT_WHERE ? data + 3 + n : NULL;

        memcpy(msg, data, 3 + n);
        mp_text_clean(msg + 3, n);
        n = mp_chat_fit(msg + 3, n);
        if (mp_is_host()) {
            const char* why = NULL;

            if (!mp_rule(MP_RULE_CHAT)) {
                why = "chat is off";
            } else if (c->host_ms != 0 && pc_mp_now_ms() - c->host_ms < MP_CHAT_GAP_MS) {
                why = "too soon";
            } else if (n == 0 || mp_chat_ng_word(msg + 3, n)) {
                why = "word list";
            }
            if (why != NULL) {
                return;
            }
            c->host_ms = pc_mp_now_ms();
            msg[2] = (u8)n;
            if (where != NULL) {
                memcpy(msg + 3 + n, where, MP_CHAT_WHERE);
            } else {
                memset(msg + 3 + n, 0, MP_CHAT_WHERE);
            }
            mp_player_relay(slot, msg, 3 + n + MP_CHAT_WHERE);
        }
        c->typing = FALSE;
        mp_chat_where_in(c, where);
        mp_chat_put(slot, msg + 3, n);
    }
}

// screen

// where a player's head is on this screen and how far inside its edges (negative: off it); FALSE when not here
static int mp_chat_head(GAME_PLAY* play, int slot, f32* x, f32* y, f32* in) {
    ACTOR* a;
    xyz_t pos;
    xyz_t clip;
    f32 w;
    f32 half = mp_chat_half_w();
    f32 d;

    if (slot == mp_chat_self()) {
        a = (ACTOR*)GET_PLAYER_ACTOR(play);
    } else {
        MP_PLAYER_ACTOR* pup = (MP_PLAYER_ACTOR*)mp_player_puppet(slot);

        if (pup == NULL || pup->hidden) {
            return FALSE;
        }
        a = &pup->actor_class;
    }
    if (a == NULL) {
        return FALSE;
    }
    pos = a->world.position;
    pos.y += 30.0f;
    Skin_Matrix_PrjMulVector(&play->projection_matrix, &pos, &clip, &w);
    if (w < 1.0f) {
        return FALSE;
    }
    *x = 160.0f + (clip.x / w) * 160.0f;
    *y = 120.0f - (clip.y / w) * 120.0f;
    *in = *x - (160.0f - half);
    d = (160.0f + half) - *x;
    *in = d < *in ? d : *in;
    *in = *y < *in ? *y : *in;
    d = 240.0f - *y;
    *in = d < *in ? d : *in;
    return TRUE;
}

// where a player's head is on this screen; FALSE when it isn't on it
static int mp_chat_anchor(GAME_PLAY* play, int slot, f32* x, f32* y) {
    f32 in;

    return mp_chat_head(play, slot, x, y, &in) && in > 0.0f;
}

static int mp_chat_live(void) {
    int s;

    if (!mp_active() || (!mp_is_host() && mp_travel_state() != MP_TRAVEL_VISITING)) {
        return FALSE;
    }
    for (s = 0; s < MP_MAX_PEERS; s++) {
        if (s != mp_chat_self() && mp_player_look(s) != NULL) {
            return TRUE;
        }
    }
    return FALSE;
}

static int mp_chat_hidden(GAME_PLAY* play) {
    return play->submenu.process_status != mSM_PROCESS_WAIT || play->fb_wipe_mode != WIPE_MODE_NONE ||
           !mEv_IsNotTitleDemo();
}

// a keyboard is up (announcements and trips home wait for it)
int pc_mp_chat_typing(void) {
    return s_kb.state != MP_KB_IDLE || s_ime.on;
}

static int mp_chat_button_up(GAME_PLAY* play) {
    return !pc_mp_chat_typing() && mp_chat_live() && mp_rule(MP_RULE_CHAT) && mp_quiet_moment(play);
}

static int mp_chat_button_mode(GAME_PLAY* play) {
    if (s_ime.on || (s_kb.state == MP_KB_OPEN && play->submenu.open_flag && !s_kb.cancel)) {
        return MP_BTN_CLOSE;
    }
    return (!s_open_next && mp_chat_button_up(play)) ? MP_BTN_OPEN : MP_BTN_OFF;
}

static f32 mp_chat_button_x(void) {
    return 160.0f - mp_chat_half_w() + 30.0f;
}

#define MP_CHAT_BUTTON_Y 206.0f

// the bottom-left corner, or just above the Vita keyboard while it covers that corner
static f32 mp_chat_button_y(void) {
    f32 y;

    if (!s_ime.on) {
        return MP_CHAT_BUTTON_Y;
    }
    y = (f32)vita_ime_top() * (240.0f / 544.0f) - 16.0f;
    return y < 16.0f ? 16.0f : (y > MP_CHAT_BUTTON_Y ? MP_CHAT_BUTTON_Y : y);
}

void pc_mp_chat_tap(int x, int y) {
    s_tap_x = x;
    s_tap_y = y;
    s_tap_new = TRUE;
}

// the game's own keyboard: the "Say it!" window, opened as the inventory is
static void mp_chat_kb_open(GAME_PLAY* play) {
    Submenu* submenu = &play->submenu;

    if (submenu->start_refuse || submenu->start_refuse_timer != 0 || Common_Get(reset_flag) ||
        play->fb_fade_type != FADE_TYPE_NONE || !mPlib_able_submenu_type1((GAME*)play) || mEv_CheckFirstIntro()) {
        return;
    }
    mem_clear(s_kb.buf, sizeof(s_kb.buf), CHAR_SPACE);
    mSM_open_submenu_new(submenu, mSM_OVL_LEDIT, mLE_TYPE_MP_CHAT, 0, s_kb.buf);
    s_kb.state = MP_KB_OPENING;
    s_kb.cancel = FALSE;
    s_kb.cancel_req = FALSE;
    mp_chat_send_typing(TRUE);
}

// the Vita's keyboard over the running game; the player stands while it's up
static int mp_chat_ime_open(void) {
    if (!vita_ime_open()) {
        return FALSE;
    }
    s_ime.on = TRUE;
    s_ime.len = 0;
    mp_chat_send_typing(TRUE);
    return TRUE;
}

static void mp_chat_ime_end(void) {
    if (s_ime.on) {
        s_ime.on = FALSE;
        s_ime.len = 0;
        mp_chat_send_typing(FALSE);
    }
}

// vita_ime.c: the text as it's typed, Send (FALSE keeps the keyboard up for a refused word), or closed
int mp_chat_ime_text(const unsigned short* text, int n, int what) {
    u8 buf[MP_CHAT_MAX];
    int len;

    if (what == MP_IME_CLOSED || text == NULL) {
        mp_chat_ime_end();
        return TRUE;
    }
    len = mp_chat_from_utf16(text, n, buf);
    mp_text_clean(buf, len);
    len = mp_chat_fit(buf, len);
    if (what == MP_IME_TYPING) {
        memcpy(s_ime.text, buf, len);
        s_ime.len = len;
        return TRUE;
    }
    if (mp_chat_ng_word(buf, len)) {
        sAdo_SysTrgStart(0x1003);
        return FALSE;
    }
    mp_chat_ime_end();
    mp_chat_say(buf, len);
    return TRUE;
}

// a talk, menu or scene change came up while typing: the Vita keyboard gives way
static int mp_chat_interrupted(GAME_PLAY* play) {
    return mp_chat_hidden(play) || play->submenu.open_flag || play->fb_mode != FBDEMO_MODE_NONE ||
           mMsg_Check_MainHide(mMsg_Get_base_window_p()) != TRUE;
}

// the keyboard up closes with nothing sent (the game's own keyboard closes itself as its end command does)
static void mp_chat_close_keyboard(void) {
    if (s_ime.on) {
        vita_ime_close();
        mp_chat_ime_end();
        sAdo_SysTrgStart(0x5F);
    } else if (s_kb.state == MP_KB_OPEN) {
        s_kb.cancel = TRUE;
        s_kb.cancel_req = TRUE;
    }
}

// m_editor_ovl.c: TRUE once when the button closed the chat keyboard
int pc_mp_chat_cancel_take(void) {
    int take = s_kb.cancel_req;

    s_kb.cancel_req = FALSE;
    return take;
}

static void mp_chat_kb_move(GAME_PLAY* play) {
    switch (s_kb.state) {
        case MP_KB_OPENING:
            if (play->submenu.open_flag) {
                s_kb.state = MP_KB_OPEN;
            } else if (play->submenu.menu_type != mSM_OVL_LEDIT && play->submenu.process_status == mSM_PROCESS_WAIT) {
                s_kb.state = MP_KB_IDLE; // (the request was dropped unopened)
                mp_chat_send_typing(FALSE);
            }
            break;
        case MP_KB_OPEN:
            if (!play->submenu.open_flag && play->submenu.process_status == mSM_PROCESS_WAIT) {
                s_kb.state = MP_KB_IDLE;
                mp_chat_send_typing(FALSE);
                if (!s_kb.cancel) {
                    mp_chat_say(s_kb.buf, MP_CHAT_MAX);
                }
                s_kb.cancel = FALSE;
                s_kb.cancel_req = FALSE;
            }
            break;
    }
}

void pc_mp_chat_move(GAME_PLAY* play) {
    unsigned int now = pc_mp_now_ms();
    int s;

    mp_chat_kb_move(play);
    if (s_ime.on && (!vita_ime_active() || !mp_chat_allowed() || !mp_chat_live() || mp_chat_interrupted(play))) {
        vita_ime_close();
        mp_chat_ime_end();
    }
    if (s_tap_new) {
        int mode = mp_chat_button_mode(play);
        f32 bx = mp_chat_button_x();
        f32 by = mp_chat_button_y();
        int hit = s_tap_x > bx - 30 && s_tap_x < bx + 30 && s_tap_y > by - 24 && s_tap_y < by + 34;

        s_tap_new = FALSE;
        if (hit && mode == MP_BTN_CLOSE && s_button_opacity > 0.2f) {
            mp_chat_close_keyboard();
        } else if (hit && mode == MP_BTN_OPEN && s_button_opacity > 0.5f) {
            s_open_next = TRUE;
            s_button_opacity = 0.0f;
        }
    } else if (s_open_next) {
        s_open_next = FALSE;
        // (the game's own keyboard when the Vita's won't open)
        if (mp_chat_button_up(play) && (!g_pc_settings.mp_chat_keyboard || !mp_chat_ime_open())) {
            mp_chat_kb_open(play);
        }
    }
    if (s_out.waiting && now - s_out.last_ms >= MP_CHAT_GAP_MS) {
        s_out.waiting = FALSE;
        if (mp_chat_allowed()) {
            mp_chat_send_now(s_out.text, s_out.len);
        }
    }
    if (s_open_next) {
        s_button_opacity = 0.0f;
    } else {
        add_calc(&s_button_opacity, mp_chat_button_mode(play) != MP_BTN_OFF ? 1.0f : 0.0f, 1.0f - sqrtf(0.8f),
                 0.075f, 0.005f);
    }

    for (s = 0; s < MP_MAX_PEERS; s++) {
        mp_chat_t* c = &s_chat[s];

        if (c->typing && now - c->typing_ms > MP_CHAT_TYPING_MS) {
            c->typing = FALSE;
        }
        // a balloon over a speaker on this screen, else the line at the top (heard during a load: too late)
        if (c->fresh) {
            f32 x;
            f32 y;

            c->fresh = FALSE;
            if (now - c->ms >= c->show_ms) {
                // (gone by)
            } else if (s != mp_chat_self() && !mp_chat_anchor(play, s, &x, &y)) {
                mp_chat_top_push(s, c->text, c->len);
                c->swap_ms = now;
            } else if (c->mode == 0 || c->mode == 4) {
                c->swap_ms = now;
                c->mode = 1;
                c->timer = 2;
                c->opacity = 0.0f;
            }
        }
        // a speaker well off this screen for a moment (or gone from this place): their balloon carries on at the
        // edge facing them. The band between the two lines and the pause after a change keep it from flickering.
        if (s != mp_chat_self() && c->mode >= 1 && c->mode <= 3 && now - c->ms < c->show_ms) {
            f32 x;
            f32 y;
            f32 in;
            int here = mp_chat_head(play, s, &x, &y, &in);

            if (here && in >= -MP_CHAT_EDGE_OUT) {
                c->far = FALSE;
            } else if (!c->far) {
                c->far = TRUE;
                c->far_ms = now;
            }
            if (c->far && (!here || (now - c->far_ms >= MP_CHAT_EDGE_MS && now - c->swap_ms >= MP_CHAT_SWAP_MS))) {
                unsigned int left = c->ms + c->show_ms - now;

                mp_chat_top_push(s, c->text, c->len);
                s_top[0].show_ms = left < 2000 ? 2000 : left;
                c->mode = 0;
                c->opacity = 0.0f;
                c->far = FALSE;
                c->near = FALSE;
                c->swap_ms = now;
            }
        }
        switch (c->mode) {
            case 1:
            case 2:
                if (c->timer-- == 0) {
                    c->timer = 2;
                    c->mode++;
                }
                break;
            case 3:
                add_calc(&c->opacity, 1.0f, 1.0f - sqrtf(0.5f), 0.25f, 0.15f);
                if (now - c->ms >= c->show_ms) {
                    c->mode = 4;
                }
                break;
            case 4:
                add_calc(&c->opacity, 0.0f, 1.0f - sqrtf(0.5f), 0.1f, 0.05f);
                add_calc(&c->opacity, 0.0f, 1.0f - sqrtf(0.5f), 0.005f, 0.005f);
                if (c->opacity < 0.01f) {
                    c->mode = 0;
                }
                break;
        }
    }
    for (s = 0; s < MP_CHAT_TOPS; s++) {
        mp_chat_top_t* t = &s_top[s];
        f32 x;
        f32 y;

        // the speaker came well onto this screen for a moment: the edge line becomes their balloon for its time left
        if (t->len != 0 && now - t->ms < t->show_ms) {
            mp_chat_t* sc = &s_chat[t->slot];
            f32 in;

            if (mp_chat_head(play, t->slot, &x, &y, &in) && in > MP_CHAT_EDGE_IN) {
                if (!sc->near) {
                    sc->near = TRUE;
                    sc->near_ms = now;
                }
            } else {
                sc->near = FALSE;
            }
        }
        if (t->len != 0 && now - t->ms < t->show_ms && s_chat[t->slot].near &&
            now - s_chat[t->slot].near_ms >= MP_CHAT_EDGE_MS && now - s_chat[t->slot].swap_ms >= MP_CHAT_SWAP_MS) {
            mp_chat_t* c = &s_chat[t->slot];
            unsigned int left = t->ms + t->show_ms - now;

            c->near = FALSE;
            c->far = FALSE;
            c->swap_ms = now;
            if (c->mode == 0 || (int)(c->ms - t->ms) < 0) {
                memcpy(c->text, t->msg, t->msg_len);
                c->len = t->msg_len;
                c->fresh = FALSE;
                c->ms = now;
                c->show_ms = left < 2000 ? 2000 : left;
                if (c->mode == 0 || c->mode == 4) {
                    c->mode = 1;
                    c->timer = 2;
                    c->opacity = 0.0f;
                }
            }
            t->len = 0;
            continue;
        }
        if (t->len != 0) {
            int live = now - t->ms < t->show_ms;

            add_calc(&t->opacity, live ? 1.0f : 0.0f, 1.0f - sqrtf(0.8f), 0.075f, 0.005f);
            if (!live && t->opacity < 0.01f) {
                t->len = 0;
            }
        }
    }
}

// the local player's balloon is up: the item-name balloon waits
int pc_mp_chat_self_showing(void) {
    return mp_active() && (s_chat[mp_chat_self()].mode != 0 || s_ime.on);
}

// drawing

typedef struct {
    f32 cx;
    f32 cy;
    f32 w;
    f32 h;
} mp_chat_box_t;

static void mp_chat_box(const mp_chat_lines_t* l, mp_chat_box_t* b) {
    f32 wmax = l->w[0];

    if (l->lines > 1 && l->w[1] > wmax) {
        wmax = l->w[1];
    }

    b->w = wmax + 20.0f < 40.0f ? 40.0f : wmax + 20.0f;
    if (l->lines < 2) {
        f32 s = (wmax - 17.5f) / 122.5f;

        s = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
        b->h = 23.0f + 7.0f * s;
    } else {
        b->h = 30.0f + MP_CHAT_LINE_H;
    }
}

static void mp_chat_font_begin(GRAPH* g) {
    Mtx* font_mtx = GRAPH_ALLOC_TYPE(g, Mtx, 1);

    if (font_mtx != NULL) {
        OPEN_DISP(g);
        mFont_CulcOrthoMatrix(font_mtx);
        gSPMatrix(NOW_FONT_DISP++, font_mtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
        CLOSE_DISP(g);
    }
}

#define MP_CHAT_ART_FADE 5

// the item-name balloon at a size: growing in with its dots (mode 1-3), popping away (4), or whole at `alpha` (5)
static void mp_chat_draw_art(GRAPH* g, const mp_chat_box_t* b, int mode, f32 opacity, f32 tx, f32 ty, int alpha) {
    f32 sx = b->w / 160.0f;
    f32 sy = b->h / 30.0f;
    f32 drop = (b->h - 30.0f) * 0.5f;
    Gfx* gfx;

    OPEN_DISP(g);
    Matrix_scale(16.0f, 16.0f, 16.0f, MTX_LOAD);
    Matrix_translate(b->cx - 160.0f, -(b->cy - 120.0f), 0.0f, MTX_MULT);
    gfx = NOW_FONT_DISP;
    gSPMatrix(gfx++, _Matrix_to_Mtx_new(g), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPDisplayList(gfx++, fki_win_mode);
    gDPSetRenderMode(gfx++, G_RM_CLD_SURF, G_RM_CLD_SURF2);
    switch (mode) {
        case 3:
            Matrix_push();
            Matrix_scale(opacity * sx, opacity * sy, opacity, MTX_MULT);
            gSPMatrix(gfx++, _Matrix_to_Mtx_new(g), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
            Matrix_pull();
            gSPDisplayList(gfx++, fki_win_w3T_model);
            // fallthrough
        case 2:
            Matrix_push();
            Matrix_translate(tx * -1.0f, ty * -(20.0f + drop), 0.0f, MTX_MULT);
            gSPMatrix(gfx++, _Matrix_to_Mtx_new(g), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
            Matrix_pull();
            gSPDisplayList(gfx++, fki_win_w2T_model);
            // fallthrough
        case 1:
            Matrix_push();
            Matrix_translate(tx * -13.0f, ty * -(30.0f + drop), 0.0f, MTX_MULT);
            gSPMatrix(gfx++, _Matrix_to_Mtx_new(g), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
            Matrix_pull();
            gSPDisplayList(gfx++, fki_win_w1T_model);
            break;
        case 4:
            gDPSetPrimColor(gfx++, 0, alpha, 255, 255, 215, alpha);
            Matrix_push();
            Matrix_scale(sx, sy, 1.0f, MTX_MULT);
            gSPMatrix(gfx++, _Matrix_to_Mtx_new(g), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
            Matrix_pull();
            gSPDisplayList(gfx++, fki_win_w4_model);
            break;
        case MP_CHAT_ART_FADE:
            gDPSetPrimColor(gfx++, 0, alpha, 255, 255, 215, alpha);
            Matrix_push();
            Matrix_scale(sx, sy, 1.0f, MTX_MULT);
            gSPMatrix(gfx++, _Matrix_to_Mtx_new(g), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
            Matrix_pull();
            gSPDisplayList(gfx++, mp_chat_balloon_model);
            break;
    }
    Matrix_scale(1.0f, 1.0f, 1.0f, MTX_LOAD);
    gSPMatrix(gfx++, _Matrix_to_Mtx_new(g), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    SET_FONT_DISP(gfx);
    CLOSE_DISP(g);
}

static void mp_chat_draw_text(GAME* game, const u8* s, const mp_chat_lines_t* l, const mp_chat_box_t* b, int a) {
    f32 y = b->cy - MP_CHAT_LINE_H * 0.5f * (f32)l->lines;
    int k;

    for (k = 0; k < l->lines; k++) {
        if (l->n[k] > 0) {
            mFont_SetLineStrings(game, (u8*)s + l->start[k], l->n[k], b->cx - l->w[k] * 0.5f, y, 45, 45, 35, a, FALSE,
                                 TRUE, MP_CHAT_SCALE, MP_CHAT_SCALE, mFont_MODE_FONT);
        }
        y += MP_CHAT_LINE_H;
    }
}

// over the head, its dots trailing down to it; under it when the head is near the top
static void mp_chat_place(f32 hx, f32 hy, mp_chat_box_t* b, f32* ty) {
    f32 half = mp_chat_half_w();
    f32 lo = 160.0f - half + b->w * 0.5f + 2.0f;
    f32 hi = 160.0f + half - b->w * 0.5f - 2.0f;

    *ty = 1.0f;
    b->cy = hy - 42.0f - (b->h - 30.0f) * 0.5f;
    if (b->cy - b->h * 0.5f < 2.0f) {
        *ty = -1.0f;
        b->cy = hy + 42.0f + (b->h - 30.0f) * 0.5f;
    }
    b->cx = hx + 12.0f;
    b->cx = b->cx < lo ? lo : (b->cx > hi ? hi : b->cx);
}

static int mp_chat_overlap(const mp_chat_box_t* a, const mp_chat_box_t* b) {
    return fabsf(a->cx - b->cx) < (a->w + b->w) * 0.5f && fabsf(a->cy - b->cy) < (a->h + b->h) * 0.5f;
}

static void mp_chat_draw_balloons(GAME_PLAY* play) {
    mp_chat_box_t done[MP_MAX_PEERS];
    int ndone = 0;
    int s;

    for (s = 0; s < MP_MAX_PEERS; s++) {
        mp_chat_t* c = &s_chat[s];
        const u8* text = c->text;
        int mode = c->mode;
        f32 opacity = c->opacity;
        mp_chat_lines_t l;
        mp_chat_box_t b;
        f32 hx;
        f32 hy;
        f32 ty;
        f32 tx;
        int k;

        l.lines = 0;
        if (s == mp_chat_self() && s_ime.on) {
            // (the Vita keyboard draws only itself: what's typed shows over the player)
            mode = s_ime.len > 0 ? 3 : 0;
            opacity = 1.0f;
            text = s_ime.text;
            mp_chat_wrap(s_ime.text, s_ime.len, &l);
        } else if (mode != 0) {
            mp_chat_wrap(c->text, c->len, &l);
        }
        {
            f32 in;

            if (mode == 0 || !mp_chat_head(play, s, &hx, &hy, &in) || in < -(MP_CHAT_EDGE_OUT + 16.0f)) {
                continue;
            }
        }
        if (l.lines == 0) {
            continue;
        }
        mp_chat_box(&l, &b);
        mp_chat_place(hx, hy, &b, &ty);
        // (players side by side: a later balloon stacks above the earlier)
        for (k = 0; k < ndone; k++) {
            if (mp_chat_overlap(&b, &done[k])) {
                b.cy = done[k].cy - (done[k].h + b.h) * 0.5f - 2.0f;
            }
        }
        done[ndone++] = b;
        tx = hx < b.cx ? 1.0f : -1.0f;
        mp_chat_draw_art(play->game.graph, &b, mode, opacity, tx, ty, (int)(opacity * 255.0f));
        if (mode >= 3) {
            f32 a = (opacity - 0.5f) * 2.0f;

            if (a > 0.0f) {
                mp_chat_draw_text((GAME*)play, text, &l, &b, (int)(a * 255.0f));
            }
        }
    }
}

// a player at a keyboard: the balloon's two dots coming and going over the head
static void mp_chat_draw_typing(GAME_PLAY* play) {
    int s;

    for (s = 0; s < MP_MAX_PEERS; s++) {
        mp_chat_box_t b;
        f32 hx;
        f32 hy;
        f32 ty;
        int step = (play->game_frame / 8) % 4;

        int typing = s == mp_chat_self() ? s_ime.on && s_ime.len == 0 : s_chat[s].typing && s_chat[s].mode == 0;

        if (!typing || step == 0 || !mp_chat_anchor(play, s, &hx, &hy)) {
            continue;
        }
        b.w = 40.0f;
        b.h = 23.0f;
        mp_chat_place(hx, hy, &b, &ty);
        mp_chat_draw_art(play->game.graph, &b, step == 1 ? 1 : 2, 0.0f, 1.0f, ty, 255);
    }
}

// which way a spot is from this player, as the camera looks: screen right and down; FALSE when right here
static int mp_chat_way(GAME_PLAY* play, f32 x, f32 z, f32* ux, f32* uy) {
    PLAYER_ACTOR* me = GET_PLAYER_ACTOR(play);
    f32 fx = play->camera.lookat.center.x - play->camera.lookat.eye.x;
    f32 fz = play->camera.lookat.center.z - play->camera.lookat.eye.z;
    f32 dx;
    f32 dz;
    f32 f;
    f32 d;

    if (me == NULL) {
        return FALSE;
    }
    f = sqrtf(fx * fx + fz * fz);
    if (f < 0.001f) {
        fx = 0.0f; // (looking straight down: north is up)
        fz = -1.0f;
    } else {
        fx /= f;
        fz /= f;
    }
    dx = x - me->actor_class.world.position.x;
    dz = z - me->actor_class.world.position.z;
    *ux = dx * -fz + dz * fx;
    *uy = -(dx * fx + dz * fz);
    d = sqrtf(*ux * *ux + *uy * *uy);
    if (d < 0.001f) {
        return FALSE;
    }
    *ux /= d;
    *uy /= d;
    return TRUE;
}

// which way a speaker off this screen is; FALSE when not in this place
static int mp_chat_toward(GAME_PLAY* play, int slot, f32* ux, f32* uy) {
    MP_PLAYER_ACTOR* pup = (MP_PLAYER_ACTOR*)mp_player_puppet(slot);

    return pup != NULL && mp_chat_way(play, pup->actor_class.world.position.x, pup->actor_class.world.position.z, ux, uy);
}

// the doorway of the building a speaker is in, on this screen outdoors; FALSE when it's off it
static int mp_chat_door(GAME_PLAY* play, const mp_chat_top_t* t, f32* x, f32* y) {
    xyz_t pos;
    xyz_t clip;
    f32 w;
    f32 half = mp_chat_half_w();

    pos.x = t->wx;
    pos.y = t->wy + MP_CHAT_DOOR_UP;
    pos.z = t->wz;
    Skin_Matrix_PrjMulVector(&play->projection_matrix, &pos, &clip, &w);
    if (w < 1.0f) {
        return FALSE;
    }
    *x = 160.0f + (clip.x / w) * 160.0f;
    *y = 120.0f - (clip.y / w) * 120.0f;
    return *x > 160.0f - half + 16.0f && *x < 160.0f + half - 16.0f && *y > 24.0f && *y < 232.0f;
}

static int mp_chat_level(int scene) {
    if (scene == SCENE_MY_ROOM_LL2 || scene == SCENE_DEPART_2) {
        return 1;
    }
    return mSc_IS_SCENE_BASEMENT(scene) ? -1 : 0;
}

// a speaker on another floor of the building this player is in: above (1), below (-1), else 0
static int mp_chat_floor(int slot) {
    unsigned int age;
    const mp_pstate_t* st = mp_player_state(slot, &age);
    int here = Save_Get(scene_no);
    int d;

    if (st == NULL || st->scene == here) {
        return 0;
    }
    if (!(mSc_IS_SCENE_PLAYER_HOUSE_ROOM(st->scene) && mSc_IS_SCENE_PLAYER_HOUSE_ROOM(here) &&
          st->owner == mp_player_place()) &&
        !((st->scene == SCENE_DEPART || st->scene == SCENE_DEPART_2) && (here == SCENE_DEPART || here == SCENE_DEPART_2))) {
        return 0;
    }
    d = mp_chat_level(st->scene) - mp_chat_level(here);
    return d > 0 ? 1 : (d < 0 ? -1 : 0);
}

// out along a way from the middle until the balloon meets the screen's edge
static void mp_chat_edge(const mp_chat_box_t* b, f32 ux, f32 uy, f32* tx, f32* ty) {
    f32 hx = mp_chat_half_w() - b->w * 0.5f - 4.0f;
    f32 hy = 120.0f - b->h * 0.5f - 4.0f;
    f32 s = 1.0e6f;

    if (ux != 0.0f && hx / fabsf(ux) < s) {
        s = hx / fabsf(ux);
    }
    if (uy != 0.0f && hy / fabsf(uy) < s) {
        s = hy / fabsf(uy);
    }
    *tx = 160.0f + ux * s;
    *ty = 120.0f + uy * s;
}

// players not on this screen: "Name: message" at the screen's edge facing them (the top when that's unknown); from
// inside a building, over its door or facing it for the players outdoors, and from above or below on another floor
static void mp_chat_draw_tops(GAME_PLAY* play) {
    f32 top_y = 8.0f;
    f32 bottom_y = 232.0f;
    mp_chat_box_t box[MP_CHAT_TOPS];
    mp_chat_lines_t lines[MP_CHAT_TOPS];
    f32 in_x[MP_CHAT_TOPS];
    f32 in_y[MP_CHAT_TOPS];
    int k;

    for (k = 0; k < MP_CHAT_TOPS; k++) {
        mp_chat_top_t* t = &s_top[k];
        mp_chat_box_t* b = &box[k];
        f32 ux;
        f32 uy;
        f32 tx;
        f32 ty;
        f32 hx;
        f32 hy;
        int door;
        int on;

        lines[k].lines = 0;
        if (t->len == 0 || t->opacity <= 0.0f) {
            continue;
        }
        mp_chat_wrap(t->text, t->len, &lines[k]);
        if (lines[k].lines == 0) {
            continue;
        }
        mp_chat_box(&lines[k], b);
        door = t->where && Save_Get(scene_no) == SCENE_FG;
        on = door && mp_chat_door(play, t, &hx, &hy);
        if (mp_chat_toward(play, t->slot, &ux, &uy) || (door && !on && mp_chat_way(play, t->wx, t->wz, &ux, &uy))) {
            mp_chat_edge(b, ux, uy, &tx, &ty);
            in_x[k] = -ux;
            in_y[k] = -uy;
        } else if (on) {
            f32 dots;

            mp_chat_place(hx, hy, b, &dots);
            tx = b->cx;
            ty = b->cy;
            in_x[k] = 0.0f;
            in_y[k] = -1.0f;
        } else if (mp_chat_floor(t->slot) < 0) {
            tx = 160.0f;
            ty = bottom_y - b->h * 0.5f;
            bottom_y -= b->h + 2.0f;
            in_x[k] = 0.0f;
            in_y[k] = -1.0f;
        } else {
            tx = 160.0f;
            ty = top_y + b->h * 0.5f;
            top_y += b->h + 2.0f;
            in_x[k] = 0.0f;
            in_y[k] = 1.0f;
        }
        if (!t->placed) {
            t->cx = tx;
            t->cy = ty;
            t->placed = TRUE;
        } else {
            add_calc(&t->cx, tx, 0.25f, 8.0f, 0.1f);
            add_calc(&t->cy, ty, 0.25f, 8.0f, 0.1f);
        }
        b->cx = t->cx;
        b->cy = t->cy;
    }
    // (two at the same edge: the older steps in toward the middle)
    if (lines[0].lines != 0 && lines[1].lines != 0 && mp_chat_overlap(&box[0], &box[1])) {
        if (fabsf(in_y[1]) >= fabsf(in_x[1])) {
            box[1].cy += (in_y[1] >= 0.0f ? 1.0f : -1.0f) * ((box[0].h + box[1].h) * 0.5f + 2.0f);
        } else {
            box[1].cx += (in_x[1] >= 0.0f ? 1.0f : -1.0f) * ((box[0].w + box[1].w) * 0.5f + 2.0f);
        }
    }
    for (k = MP_CHAT_TOPS - 1; k >= 0; k--) {
        int a = (int)(s_top[k].opacity * 255.0f);
        mp_chat_box_t button;

        if (lines[k].lines == 0 || a <= 0) {
            continue;
        }
        // (not over the balloon button in the corner)
        button.cx = mp_chat_button_x();
        button.cy = mp_chat_button_y();
        button.w = 44.0f;
        button.h = 26.0f;
        if (s_button_opacity > 0.0f && mp_chat_overlap(&box[k], &button)) {
            box[k].cy = button.cy - (button.h + box[k].h) * 0.5f - 2.0f;
        }
        mp_chat_draw_art(play->game.graph, &box[k], MP_CHAT_ART_FADE, 1.0f, 0.0f, 0.0f, a);
        mp_chat_draw_text((GAME*)play, s_top[k].text, &lines[k], &box[k], a);
    }
}

static void mp_chat_draw_button(GAME_PLAY* play) {
    static u8 dots[3] = { CHAR_PERIOD, CHAR_PERIOD, CHAR_PERIOD };
    mp_chat_lines_t l;
    mp_chat_box_t b;
    int a = (int)(s_button_opacity * 255.0f);

    if (a <= 0) {
        return;
    }
    l.lines = 1;
    l.start[0] = 0;
    l.n[0] = 3;
    l.w[0] = mp_chat_width(dots, 3);
    b.w = 44.0f;
    b.h = 26.0f;
    b.cx = mp_chat_button_x();
    b.cy = mp_chat_button_y();
    mp_chat_draw_art(play->game.graph, &b, MP_CHAT_ART_FADE, 1.0f, 0.0f, 0.0f, a);
    mp_chat_draw_text((GAME*)play, dots, &l, &b, a);
}

// the game keyboard's field (m_ledit_ovl.c): a balloon like the ones over heads, with its dots, in the menu's list
void pc_mp_chat_field_art(GRAPH* g, f32 pos_x, f32 pos_y) {
    Gfx* gfx;

    OPEN_DISP(g);
    gfx = NOW_POLY_OPA_DISP;
    Matrix_scale(16.0f, 16.0f, 1.0f, MTX_LOAD);
    Matrix_translate(pos_x, pos_y + 120.0f - MP_CHAT_FIELD_Y, 140.0f, MTX_MULT);
    gSPDisplayList(gfx++, fki_win_mode);
    gDPSetRenderMode(gfx++, G_RM_CLD_SURF, G_RM_CLD_SURF2);
    Matrix_push();
    Matrix_scale(MP_CHAT_FIELD_W / 160.0f, 1.0f, 1.0f, MTX_MULT);
    gSPMatrix(gfx++, _Matrix_to_Mtx_new(g), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    Matrix_pull();
    gSPDisplayList(gfx++, fki_win_w3T_model);
    Matrix_push();
    Matrix_translate(-MP_CHAT_FIELD_W * 0.5f + 34.0f, -20.0f, 0.0f, MTX_MULT);
    gSPMatrix(gfx++, _Matrix_to_Mtx_new(g), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    Matrix_pull();
    gSPDisplayList(gfx++, fki_win_w2T_model);
    Matrix_translate(-MP_CHAT_FIELD_W * 0.5f + 22.0f, -30.0f, 0.0f, MTX_MULT);
    gSPMatrix(gfx++, _Matrix_to_Mtx_new(g), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPDisplayList(gfx++, fki_win_w1T_model);
    SET_POLY_OPA_DISP(gfx);
    CLOSE_DISP(g);
}

// m_play.c, before the talk windows so they cover the balloons
void pc_mp_chat_draw(GAME_PLAY* play) {
    if (!mp_active() || mp_chat_hidden(play)) {
        return;
    }
    mp_chat_font_begin(play->game.graph);
    mp_chat_draw_typing(play);
    mp_chat_draw_balloons(play);
    mp_chat_draw_tops(play);
}

// m_play.c, after the menus: the balloon button, which also closes a keyboard that's up
void pc_mp_chat_draw_over(GAME_PLAY* play) {
    if (!mp_active() || s_button_opacity <= 0.0f || !mEv_IsNotTitleDemo() ||
        (play->fb_wipe_mode != WIPE_MODE_NONE && mp_chat_button_mode(play) != MP_BTN_CLOSE)) {
        return;
    }
    mp_chat_font_begin(play->game.graph);
    mp_chat_draw_button(play);
}

#endif
