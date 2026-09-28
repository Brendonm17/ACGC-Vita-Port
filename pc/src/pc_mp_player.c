// pc_mp_player.c
// everyone in town as seen by everyone else: looks, states and sounds, relayed by the host
#include "pc_mp.h"

#ifdef VITA_MP

#include "m_common_data.h"
#include "m_actor.h"
#include "m_field_info.h"
#include "m_name_table.h"
#include "m_needlework.h"
#include "m_play.h"
#include "m_player.h"
#include "m_player_lib.h"
#include "sys_matrix.h"
#include "ac_t_umbrella.h"
#include "m_kankyo.h"
#include "m_room_type.h"
#include "m_random_field_h.h"
#include "pc_mp_text_data.h"

#include <math.h>
#include <string.h>

#define MP_STATE_EVERY   2     // frames between state sends
#define MP_STATE_DROP_MS 12000 // a puppet this quiet is gone
#define MP_LOOK_WIRE     28
#define MP_DOORS         4
#define MP_DOOR_MS       3000 // a door call this old is stale: the building wasn't on this screen
#define MP_FX_RING       32
#define MP_FX_WINDOW_MS  400 // a sound or effect rides along this long, so a lost state or two loses nothing
#define MP_FXQ           24
#define MP_FX_WIRE_MAX   18
#define MP_PSTATE_MIN    59
#define MP_ATT_WIRE      31
#define MP_SNOW_WIRE     38
#define MP_SNOW_FRESH_MS 400 // a rolled snowball's state this old means nobody rolls it any more
#define MP_BALL_WIRE     32
#define MP_RELAY_FRESH_MS 250 // host: a guest's state older than this passes on without the ball or snowballs
#define MP_STILL_EVERY   10   // frames between states while a menu has the world stopped
#define MP_QUIET_MS      2000 // a player unheard this long moves nothing for the others
#define MP_BALL_FRESH_MS 2500 // its mover repeats a ball at rest every half second
#define MP_PSTATE_WIRE                                                                                                 \
    (MP_PSTATE_MIN + 33 + 5 + MP_ATT_MAX * MP_ATT_WIRE + 1 + MP_SNOW_MAX * MP_SNOW_WIRE + MP_BALL_WIRE +              \
     MP_FX_MAX * MP_FX_WIRE_MAX + 1 + MP_LVL_MAX * 7 + 1 + 3 + 1 + MP_GOKI_MAX * 12)
#define MP_VFX_BODY      64
#define MP_VFX_HDR       8
#define MP_VFXQ          48
#define MP_VFX_STALE_MS  2000 // a passing effect this late is past showing
#define MP_VFX_ROOM_MS   8000 // a room change still waiting its turn this long is given up
#define MP_KK_AGAIN_MS   1500 // a show running here is told again this often, for players who come by late

typedef struct {
    int valid;      // look known: the player is in town
    int state_valid;
    mp_look_t look;
    mp_pstate_t st;
    unsigned int st_ms;
    ACTOR* puppet;
    int fx_any; // a numbered sound or effect was heard from this player
    unsigned short fx_heard;
    unsigned char fxq_at;
    unsigned char fxq_n;
    mp_fx_t fxq[MP_FXQ];
} mp_peer_t;

static mp_peer_t s_peer[MP_MAX_PEERS];
static mp_look_t s_my_look;
static int s_my_look_valid;
static unsigned char s_seq;
static unsigned char s_last_anim0;
static unsigned char s_last_anim1;
static int s_frame;

static void mp_host_relay(int from_slot, const unsigned char* data, int len);

static ACTOR* s_door_used; // the building door the local player is going through
static struct {
    u16 name;
    s16 x;
    s16 z;
    u8 out;
    u8 used;
    unsigned int ms;
} s_door[MP_DOORS];

static int s_fx_on;
static int s_fx_hush;
static xyz_t s_fx_origin;
static struct {
    mp_fx_t fx;
    unsigned int ms;
} s_fx_ring[MP_FX_RING];
static unsigned short s_fx_next; // number of the next sound or effect
static unsigned short s_fx_told; // s_fx_next when the last state went out
static struct {
    mp_att_t att;
    unsigned int frame;
    int valid;
} s_att[MP_ATT_KINDS];
static struct {
    mp_snow_t snow;
    unsigned int frame;
    int valid;
} s_snow_mine[MP_SNOW_MAX];
static struct {
    mp_snow_t snow;
    unsigned int ms;
    int valid;
} s_snow_theirs[MP_SNOW_MAX];
static struct {
    mp_ball_t ball;
    unsigned int frame;
    int valid;
} s_ball_mine;
static struct {
    mp_ball_t ball;
    unsigned int ms;
    int slot;
    int valid;
} s_ball_theirs;

// town visuals from the other screens, shown on the next play frame
static struct {
    int len;
    unsigned int ms;
    unsigned char msg[MP_VFX_HDR + MP_VFX_BODY];
} s_vfxq[MP_VFXQ];
static int s_vfxq_n;

// this game's K.K. show as told: its lights, song and weather
static struct {
    u8 live;
    u8 have;
    u8 again;
    u8 body[MP_KK_SONG_OFF][4];
    u16 scene;
    unsigned int ms;
} s_kk;

// wire

static void mp_w16(unsigned char* p, unsigned int v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static unsigned int mp_r16(const unsigned char* p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static void mp_wf(unsigned char* p, float v) {
    memcpy(p, &v, 4);
}

static float mp_rf(const unsigned char* p) {
    float v;

    memcpy(&v, p, 4);
    return v;
}

static int mp_fx16(float v, float scale) {
    float s = v * scale;

    if (s > 65535.0f) {
        s = 65535.0f;
    } else if (s < 0.0f) {
        s = 0.0f;
    }
    return (int)(s + 0.5f);
}

static int mp_sfx16(float v, float scale) {
    float s = v * scale;

    if (s > 32767.0f) {
        s = 32767.0f;
    } else if (s < -32768.0f) {
        s = -32768.0f;
    }
    return (int)(s < 0.0f ? s - 0.5f : s + 0.5f);
}

static int mp_fx8(float v, float scale) {
    int n = mp_fx16(v, scale);

    return n > 255 ? 255 : n;
}

static void mp_ws16(unsigned char* p, int v) {
    mp_w16(p, (unsigned int)(unsigned short)(short)v);
}

static short mp_rs16(const unsigned char* p) {
    return (short)mp_r16(p);
}

static int mp_fx_is_sound(int kind) {
    return kind == MP_SND_ONESHOT || kind == MP_SND_WALK || kind == MP_SND_WALK_ROOM;
}

static int mp_fx_pack(unsigned char* p, const mp_fx_t* fx) {
    int n = 0;

    p[n++] = fx->kind;
    if (fx->kind == MP_FX_KILL) {
        mp_w16(p + n, fx->id);
        mp_w16(p + n + 2, fx->item);
        return n + 4;
    }
    mp_w16(p + n, fx->id);
    n += 2;
    if (fx->kind == MP_FX_EFFECT) {
        p[n++] = fx->prio;
        mp_ws16(p + n, fx->angle);
        mp_w16(p + n + 2, fx->item);
        mp_ws16(p + n + 4, fx->arg0);
        mp_ws16(p + n + 6, fx->arg1);
        n += 8;
    }
    mp_ws16(p + n, fx->dx);
    mp_ws16(p + n + 2, fx->dy);
    mp_ws16(p + n + 4, fx->dz);
    return n + 6;
}

static int mp_fx_unpack(const unsigned char* p, int len, mp_fx_t* fx) {
    int n = 1;

    memset(fx, 0, sizeof(*fx));
    if (len < 1) {
        return -1;
    }
    fx->kind = p[0];
    if (fx->kind == MP_FX_KILL) {
        if (len < 5) {
            return -1;
        }
        fx->id = (unsigned short)mp_r16(p + 1);
        fx->item = (unsigned short)mp_r16(p + 3);
        return 5;
    }
    if (!mp_fx_is_sound(fx->kind) && fx->kind != MP_FX_EFFECT) {
        return -1;
    }
    if (len < (fx->kind == MP_FX_EFFECT ? 18 : 9)) {
        return -1;
    }
    fx->id = (unsigned short)mp_r16(p + n);
    n += 2;
    if (fx->kind == MP_FX_EFFECT) {
        fx->prio = p[n++];
        fx->angle = mp_rs16(p + n);
        fx->item = (unsigned short)mp_r16(p + n + 2);
        fx->arg0 = mp_rs16(p + n + 4);
        fx->arg1 = mp_rs16(p + n + 6);
        n += 8;
    }
    fx->dx = mp_rs16(p + n);
    fx->dy = mp_rs16(p + n + 2);
    fx->dz = mp_rs16(p + n + 4);
    return n + 6;
}

// fixed point frames (x16), speeds (x256) and scales (x255); positions stay full floats
static int mp_pstate_pack(unsigned char* p, const mp_pstate_t* st) {
    int n = 0;
    int i;

    p[n++] = st->seq;
    mp_w16(p + n, st->flags);
    n += 2;
    mp_w16(p + n, st->scene);
    mp_w16(p + n + 2, st->field);
    mp_w16(p + n + 4, st->owner);
    n += 6;
    mp_wf(p + n, st->x);
    mp_wf(p + n + 4, st->y);
    mp_wf(p + n + 8, st->z);
    n += 12;
    mp_ws16(p + n, st->rot_x);
    mp_ws16(p + n + 2, st->rot_y);
    mp_ws16(p + n + 4, st->rot_z);
    mp_ws16(p + n + 6, st->roll);
    mp_ws16(p + n + 8, st->head_x);
    mp_ws16(p + n + 10, st->head_y);
    n += 12;
    p[n++] = st->anim0;
    p[n++] = st->anim1;
    p[n++] = (unsigned char)st->part_table;
    p[n++] = st->main_index;
    mp_w16(p + n, mp_fx16(st->frame0, 16.0f));
    mp_w16(p + n + 2, mp_fx16(st->frame1, 16.0f));
    mp_ws16(p + n + 4, mp_sfx16(st->speed0, 256.0f));
    mp_ws16(p + n + 6, mp_sfx16(st->speed1, 256.0f));
    n += 8;
    p[n++] = st->eye;
    p[n++] = st->mouth;
    p[n++] = st->item_main;
    p[n++] = (unsigned char)st->item_shape;
    p[n++] = (unsigned char)st->item_anim;
    mp_w16(p + n, mp_fx16(st->item_frame, 16.0f));
    mp_ws16(p + n + 2, mp_sfx16(st->item_speed, 256.0f));
    n += 4;
    p[n++] = (unsigned char)mp_fx8(st->item_scale, 255.0f);
    p[n++] = st->shadow;
    if (st->flags & MP_PF_ROOT) {
        p[n++] = st->root_flags;
        for (i = 0; i < 3; i++) {
            mp_ws16(p + n + i * 2, st->root_trans[i]);
        }
        mp_ws16(p + n + 6, st->root_rot_x);
        mp_ws16(p + n + 8, st->root_rot_y);
        mp_ws16(p + n + 10, st->root_rot_z);
        n += 12;
    }
    if (st->flags & MP_PF_UMB) {
        p[n++] = st->umb_action;
        p[n++] = (unsigned char)mp_fx8(st->umb_frame, 2.0f);
        p[n++] = st->umb_idx;
    }
    if (st->flags & MP_PF_NET) {
        for (i = 0; i < 3; i++) {
            mp_ws16(p + n + i * 2, st->net_angle[i]);
        }
        n += 6;
    }
    if (st->flags & MP_PF_HOLD) {
        mp_w16(p + n, st->hold_item);
        mp_w16(p + n + 2, mp_fx16(st->hold_scale, 1000000.0f));
        mp_ws16(p + n + 4, st->hold_dx);
        mp_ws16(p + n + 6, st->hold_dy);
        mp_ws16(p + n + 8, st->hold_dz);
        mp_ws16(p + n + 10, st->hold_angle);
        p[n + 12] = st->hold_jump;
        n += 13;
    }
    if (st->flags & MP_PF_ROD) {
        mp_ws16(p + n, st->rod_angle_z);
        n += 2;
    }
    if (st->flags & MP_PF_ATTACH) {
        p[n++] = st->natt;
        for (i = 0; i < st->natt && i < MP_ATT_MAX; i++) {
            const mp_att_t* a = &st->att[i];
            int k;

            p[n++] = a->kind;
            p[n++] = a->arg;
            p[n++] = a->arg2;
            memcpy(p + n, a->rgba, 4);
            n += 4;
            for (k = 0; k < 3; k++) {
                mp_ws16(p + n + k * 2, a->rot[k]);
            }
            mp_ws16(p + n + 6, a->dx);
            mp_ws16(p + n + 8, a->dy);
            mp_ws16(p + n + 10, a->dz);
            for (k = 0; k < 3; k++) {
                mp_wf(p + n + 12 + k * 4, a->scale[k]);
            }
            n += 24;
        }
    }
    if (st->flags & MP_PF_BALLOON) {
        mp_ws16(p + n, st->balloon_x);
        mp_ws16(p + n + 2, st->balloon_z);
        n += 4;
    }
    if (st->flags & MP_PF_SNOW) {
        p[n++] = st->nsnow;
        for (i = 0; i < st->nsnow && i < MP_SNOW_MAX; i++) {
            const mp_snow_t* s = &st->snow[i];
            int k;

            p[n++] = s->part;
            p[n++] = s->proc;
            mp_w16(p + n, s->flags);
            n += 2;
            for (k = 0; k < 3; k++) {
                mp_wf(p + n + k * 4, s->pos[k]);
            }
            n += 12;
            mp_ws16(p + n, mp_sfx16(s->y_ofs, 16.0f));
            mp_ws16(p + n + 2, mp_sfx16(s->ofs_y, 1.0f));
            mp_w16(p + n + 4, mp_fx16(s->move_dist, 8.0f));
            p[n + 6] = (unsigned char)mp_fx8(s->body_scale, 255.0f);
            n += 7;
            for (k = 0; k < 3; k++) {
                mp_ws16(p + n + k * 2, s->head_vec[k]);
            }
            n += 6;
            p[n++] = (unsigned char)s->result;
            mp_wf(p + n, s->fg_x);
            mp_wf(p + n + 4, s->fg_z);
            n += 8;
        }
    }
    if (st->flags & MP_PF_BALL) {
        const mp_ball_t* b = &st->ball;

        for (i = 0; i < 3; i++) {
            mp_wf(p + n + i * 4, b->pos[i]);
        }
        n += 12;
        mp_ws16(p + n, mp_sfx16(b->speed, 256.0f));
        mp_ws16(p + n + 2, mp_sfx16(b->vel_y, 256.0f));
        mp_ws16(p + n + 4, mp_sfx16(b->ball_y, 16.0f));
        mp_ws16(p + n + 6, b->angle_y);
        n += 8;
        p[n++] = b->mode;
        p[n++] = b->flags;
        p[n++] = b->type;
        p[n++] = b->snd;
        mp_w16(p + n, b->snd_id);
        mp_w16(p + n + 2, (unsigned int)mp_fx16(b->snd_speed, 256.0f));
        mp_w16(p + n + 4, b->kick_ms & 0xFFFF);
        mp_w16(p + n + 6, b->kick_ms >> 16);
        n += 8;
    }
    mp_w16(p + n, st->fx_seq);
    n += 2;
    p[n++] = st->nfx;
    for (i = 0; i < st->nfx && i < MP_FX_MAX; i++) {
        n += mp_fx_pack(p + n, &st->fx[i]);
    }
    p[n++] = st->nlvl;
    for (i = 0; i < st->nlvl && i < MP_LVL_MAX; i++) {
        p[n++] = st->lvl[i];
        mp_ws16(p + n, st->lvl_d[i][0]);
        mp_ws16(p + n + 2, st->lvl_d[i][1]);
        mp_ws16(p + n + 4, st->lvl_d[i][2]);
        n += 6;
    }
    p[n++] = (unsigned char)((st->goki_run ? 0x80 : 0) | st->ngoki);
    for (i = 0; i < st->ngoki && i < MP_GOKI_MAX; i++) {
        const mp_goki_t* g = &st->goki[i];

        p[n++] = g->id;
        p[n++] = g->act;
        p[n++] = g->anm;
        p[n++] = g->alpha;
        mp_ws16(p + n, g->x);
        mp_ws16(p + n + 2, g->y);
        mp_ws16(p + n + 4, g->z);
        mp_ws16(p + n + 6, g->angle);
        n += 8;
    }
    return n;
}

static int mp_pstate_unpack(const unsigned char* p, int len, mp_pstate_t* st) {
    int n = 0;
    int i;

    memset(st, 0, sizeof(*st));
    if (len < MP_PSTATE_MIN) {
        return -1;
    }
    st->seq = p[n++];
    st->flags = (unsigned short)mp_r16(p + n);
    n += 2;
    st->scene = (unsigned short)mp_r16(p + n);
    st->field = (unsigned short)mp_r16(p + n + 2);
    st->owner = (unsigned short)mp_r16(p + n + 4);
    n += 6;
    st->x = mp_rf(p + n);
    st->y = mp_rf(p + n + 4);
    st->z = mp_rf(p + n + 8);
    n += 12;
    st->rot_x = mp_rs16(p + n);
    st->rot_y = mp_rs16(p + n + 2);
    st->rot_z = mp_rs16(p + n + 4);
    st->roll = mp_rs16(p + n + 6);
    st->head_x = mp_rs16(p + n + 8);
    st->head_y = mp_rs16(p + n + 10);
    n += 12;
    st->anim0 = p[n++];
    st->anim1 = p[n++];
    st->part_table = (signed char)p[n++];
    st->main_index = p[n++];
    st->frame0 = (float)mp_r16(p + n) / 16.0f;
    st->frame1 = (float)mp_r16(p + n + 2) / 16.0f;
    st->speed0 = (float)mp_rs16(p + n + 4) / 256.0f;
    st->speed1 = (float)mp_rs16(p + n + 6) / 256.0f;
    n += 8;
    st->eye = p[n++];
    st->mouth = p[n++];
    st->item_main = p[n++];
    st->item_shape = (signed char)p[n++];
    st->item_anim = (signed char)p[n++];
    st->item_frame = (float)mp_r16(p + n) / 16.0f;
    st->item_speed = (float)mp_rs16(p + n + 2) / 256.0f;
    n += 4;
    st->item_scale = (float)p[n++] / 255.0f;
    st->shadow = p[n++];
    if ((st->flags & MP_PF_ROOT) && n + 13 <= len) {
        st->root_flags = p[n++];
        for (i = 0; i < 3; i++) {
            st->root_trans[i] = mp_rs16(p + n + i * 2);
        }
        st->root_rot_x = mp_rs16(p + n + 6);
        st->root_rot_y = mp_rs16(p + n + 8);
        st->root_rot_z = mp_rs16(p + n + 10);
        n += 12;
    } else if (st->flags & MP_PF_ROOT) {
        return -1;
    }
    if (st->flags & MP_PF_UMB) {
        if (n + 3 > len) {
            return -1;
        }
        st->umb_action = p[n++];
        st->umb_frame = (float)p[n++] / 2.0f;
        st->umb_idx = p[n++];
    }
    if (st->flags & MP_PF_NET) {
        if (n + 6 > len) {
            return -1;
        }
        for (i = 0; i < 3; i++) {
            st->net_angle[i] = mp_rs16(p + n + i * 2);
        }
        n += 6;
    }
    if (st->flags & MP_PF_HOLD) {
        if (n + 13 > len) {
            return -1;
        }
        st->hold_item = (unsigned short)mp_r16(p + n);
        st->hold_scale = (float)mp_r16(p + n + 2) / 1000000.0f;
        st->hold_dx = mp_rs16(p + n + 4);
        st->hold_dy = mp_rs16(p + n + 6);
        st->hold_dz = mp_rs16(p + n + 8);
        st->hold_angle = mp_rs16(p + n + 10);
        st->hold_jump = p[n + 12];
        n += 13;
    }
    if (st->flags & MP_PF_ROD) {
        if (n + 2 > len) {
            return -1;
        }
        st->rod_angle_z = mp_rs16(p + n);
        n += 2;
    }
    if (st->flags & MP_PF_ATTACH) {
        if (n + 1 > len || p[n] > MP_ATT_MAX || n + 1 + p[n] * MP_ATT_WIRE > len) {
            return -1;
        }
        st->natt = p[n++];
        for (i = 0; i < st->natt; i++) {
            mp_att_t* a = &st->att[i];
            int k;

            a->kind = p[n++];
            a->arg = p[n++];
            a->arg2 = p[n++];
            memcpy(a->rgba, p + n, 4);
            n += 4;
            for (k = 0; k < 3; k++) {
                a->rot[k] = mp_rs16(p + n + k * 2);
            }
            a->dx = mp_rs16(p + n + 6);
            a->dy = mp_rs16(p + n + 8);
            a->dz = mp_rs16(p + n + 10);
            for (k = 0; k < 3; k++) {
                a->scale[k] = mp_rf(p + n + 12 + k * 4);
            }
            n += 24;
        }
    }
    if (st->flags & MP_PF_BALLOON) {
        if (n + 4 > len) {
            return -1;
        }
        st->balloon_x = mp_rs16(p + n);
        st->balloon_z = mp_rs16(p + n + 2);
        n += 4;
    }
    if (st->flags & MP_PF_SNOW) {
        if (n + 1 > len || p[n] > MP_SNOW_MAX || n + 1 + p[n] * MP_SNOW_WIRE > len) {
            return -1;
        }
        st->nsnow = p[n++];
        for (i = 0; i < st->nsnow; i++) {
            mp_snow_t* s = &st->snow[i];
            int k;

            s->part = p[n++];
            s->proc = p[n++];
            s->flags = (unsigned short)mp_r16(p + n);
            n += 2;
            for (k = 0; k < 3; k++) {
                s->pos[k] = mp_rf(p + n + k * 4);
            }
            n += 12;
            s->y_ofs = (float)mp_rs16(p + n) / 16.0f;
            s->ofs_y = (float)mp_rs16(p + n + 2);
            s->move_dist = (float)mp_r16(p + n + 4) / 8.0f;
            s->body_scale = (float)p[n + 6] / 255.0f;
            n += 7;
            for (k = 0; k < 3; k++) {
                s->head_vec[k] = mp_rs16(p + n + k * 2);
            }
            n += 6;
            s->result = p[n++];
            s->fg_x = mp_rf(p + n);
            s->fg_z = mp_rf(p + n + 4);
            n += 8;
        }
    }
    if (st->flags & MP_PF_BALL) {
        mp_ball_t* b = &st->ball;

        if (n + MP_BALL_WIRE > len) {
            return -1;
        }
        for (i = 0; i < 3; i++) {
            b->pos[i] = mp_rf(p + n + i * 4);
        }
        n += 12;
        b->speed = (float)mp_rs16(p + n) / 256.0f;
        b->vel_y = (float)mp_rs16(p + n + 2) / 256.0f;
        b->ball_y = (float)mp_rs16(p + n + 4) / 16.0f;
        b->angle_y = mp_rs16(p + n + 6);
        n += 8;
        b->mode = p[n++];
        b->flags = p[n++];
        b->type = p[n++];
        b->snd = p[n++];
        b->snd_id = (unsigned short)mp_r16(p + n);
        b->snd_speed = (float)mp_r16(p + n + 2) / 256.0f;
        b->kick_ms = mp_r16(p + n + 4) | (mp_r16(p + n + 6) << 16);
        n += 8;
    }
    if (n + 3 > len) {
        return -1;
    }
    st->fx_seq = (unsigned short)mp_r16(p + n);
    n += 2;
    st->nfx = p[n++];
    if (st->nfx > MP_FX_MAX) {
        return -1;
    }
    for (i = 0; i < st->nfx; i++) {
        int used = mp_fx_unpack(p + n, len - n, &st->fx[i]);

        if (used < 0) {
            return -1;
        }
        n += used;
    }
    if (n + 1 > len || p[n] > MP_LVL_MAX || n + 1 + p[n] * 7 > len) {
        return -1;
    }
    st->nlvl = p[n++];
    for (i = 0; i < st->nlvl; i++) {
        st->lvl[i] = p[n++];
        st->lvl_d[i][0] = mp_rs16(p + n);
        st->lvl_d[i][1] = mp_rs16(p + n + 2);
        st->lvl_d[i][2] = mp_rs16(p + n + 4);
        n += 6;
    }
    if (n + 1 > len || (p[n] & 0x7F) > MP_GOKI_MAX || n + 1 + (p[n] & 0x7F) * 12 > len) {
        return -1;
    }
    st->goki_run = (p[n] & 0x80) != 0;
    st->ngoki = p[n++] & 0x7F;
    for (i = 0; i < st->ngoki; i++) {
        mp_goki_t* g = &st->goki[i];

        g->id = p[n];
        g->act = p[n + 1];
        g->anm = p[n + 2];
        g->alpha = p[n + 3];
        g->x = mp_rs16(p + n + 4);
        g->y = mp_rs16(p + n + 6);
        g->z = mp_rs16(p + n + 8);
        g->angle = mp_rs16(p + n + 10);
        n += 12;
    }
    return n;
}

// looks

static int s_shown_cloth = -1; // the cloth drawn on this player, when it isn't the one saved (a try-on)

void pc_mp_shown_cloth(unsigned short idx) {
    s_shown_cloth = idx;
}

static void mp_look_capture(mp_look_t* l) {
    Private_c* priv = Now_Private;
    mActor_name_t item;

    memset(l, 0, sizeof(*l));
    l->umbrella = 0xFF;
    if (priv == NULL) {
        return;
    }
    l->gender = (unsigned char)priv->gender;
    l->face = (unsigned char)priv->face;
    l->sunburn = (unsigned char)(priv->sunburn.rank > 0 ? priv->sunburn.rank : 0);
    l->swell = Common_Get(player_bee_swell_flag) != 0;
    l->decoy = Common_Get(player_decoy_flag) != 0;
    l->cloth_idx = priv->cloth.idx;
    // (a shop's try-on: the others see it too)
    if (s_shown_cloth >= 0 && s_shown_cloth <= CLOTH_NUM && s_shown_cloth != priv->cloth.idx) {
        l->cloth_idx = (unsigned short)s_shown_cloth;
    } else if (priv->cloth.idx > CLOTH_NUM) {
        const mNW_original_design_c* org = &priv->my_org[(priv->cloth.idx - (CLOTH_NUM + 1)) & 7];

        l->cloth_pal_idx = org->palette;
        memcpy(l->cloth_tex, org->design.data, sizeof(l->cloth_tex));
    }
    item = priv->equipment;
    if (ITEM_IS_UMBRELLA2(item)) {
        l->umbrella = (unsigned char)(item - ITM_UMBRELLA_START);
        if (l->umbrella >= 32) {
            const mNW_original_design_c* org = &priv->my_org[(l->umbrella - 32) & 7];

            l->umb_pal_idx = org->palette;
            memcpy(l->umb_tex, org->design.data, sizeof(l->umb_tex));
        }
    }
    memcpy(l->name, priv->player_ID.player_name, MP_NAME_LEN);
    memcpy(l->town, priv->player_ID.land_name, MP_NAME_LEN);
}

static int mp_look_same(const mp_look_t* a, const mp_look_t* b) {
    return memcmp((const unsigned char*)a + sizeof(a->ver), (const unsigned char*)b + sizeof(b->ver),
                  sizeof(*a) - sizeof(a->ver)) == 0;
}

static int mp_look_pack(unsigned char* p, int slot, const mp_look_t* l) {
    p[0] = MP_M_LOOK;
    p[1] = (unsigned char)slot;
    p[2] = l->gender;
    p[3] = l->face;
    p[4] = l->sunburn;
    p[5] = (unsigned char)(l->swell | (l->decoy << 1));
    p[6] = l->umbrella;
    mp_w16(p + 7, l->cloth_idx);
    p[9] = l->cloth_pal_idx;
    p[10] = l->umb_pal_idx;
    memcpy(p + 11, l->name, MP_NAME_LEN);
    memcpy(p + 19, l->town, MP_NAME_LEN);
    p[27] = 0;
    return MP_LOOK_WIRE;
}

static void mp_look_unpack(const unsigned char* p, mp_look_t* l) {
    l->gender = p[2] ? 1 : 0;
    l->face = p[3] & 7;
    l->sunburn = p[4] > 8 ? 8 : p[4];
    l->swell = p[5] & 1;
    l->decoy = (p[5] >> 1) & 1;
    l->umbrella = p[6] < 40 ? p[6] : 0xFF;
    l->cloth_idx = (unsigned short)mp_r16(p + 7);
    l->cloth_pal_idx = p[9];
    l->umb_pal_idx = p[10];
    memcpy(l->name, p + 11, MP_NAME_LEN);
    memcpy(l->town, p + 19, MP_NAME_LEN);
    mp_text_clean(l->name, MP_NAME_LEN); // (they're printed in announcements and chat)
    mp_text_clean(l->town, MP_NAME_LEN);
    l->ver++;
}

// a look plus any custom designs it wears, as a burst of reliable messages
static void mp_look_send(int conn, int slot, const mp_look_t* l) {
    unsigned char msg[4 + 256];
    int which;
    int half;

    mp_look_pack(msg, slot, l);
    mp_lobby_send_rel(conn, msg, MP_LOOK_WIRE);
    for (which = 0; which < 2; which++) {
        const unsigned char* tex;

        if (which == 0 && l->cloth_idx <= CLOTH_NUM) {
            continue;
        }
        if (which == 1 && (l->umbrella == 0xFF || l->umbrella < 32)) {
            continue;
        }
        tex = which == 0 ? l->cloth_tex : l->umb_tex;
        for (half = 0; half < 2; half++) {
            msg[0] = MP_M_DESIGN;
            msg[1] = (unsigned char)slot;
            msg[2] = (unsigned char)which;
            msg[3] = (unsigned char)half;
            memcpy(msg + 4, tex + half * 256, 256);
            mp_lobby_send_rel(conn, msg, sizeof(msg));
        }
    }
}

// local capture

void pc_mp_fx_capture(int on) {
    GAME_PLAY* play = mp_live_play();
    PLAYER_ACTOR* player = (play != NULL) ? GET_PLAYER_ACTOR(play) : NULL;

    s_fx_on = on && mp_active() && player != NULL;
    if (s_fx_on) {
        s_fx_origin = player->actor_class.world.position;
    }
}

void pc_mp_fx_hush(int on) {
    s_fx_hush += on ? 1 : -1;
    if (s_fx_hush < 0) {
        s_fx_hush = 0;
    }
}

static int mp_fx_taking(void) {
    return s_fx_on && s_fx_hush == 0;
}

static void mp_fx_push(mp_fx_t* fx, const xyz_t* pos) {
    if (pos != NULL) {
        fx->dx = (short)mp_sfx16(pos->x - s_fx_origin.x, 1.0f);
        fx->dy = (short)mp_sfx16(pos->y - s_fx_origin.y, 1.0f);
        fx->dz = (short)mp_sfx16(pos->z - s_fx_origin.z, 1.0f);
    }
    s_fx_ring[s_fx_next % MP_FX_RING].fx = *fx;
    s_fx_ring[s_fx_next % MP_FX_RING].ms = pc_mp_now_ms() | 1;
    s_fx_next++;
}

// the looping sounds this player's doings keep up this frame, for its next state
static unsigned char s_lvl_n;
static unsigned char s_lvl[MP_LVL_MAX];
static short s_lvl_d[MP_LVL_MAX][3];

void pc_mp_fx_level(unsigned char id, const void* pos_v) {
    const xyz_t* pos = (const xyz_t*)pos_v;
    int k;

    // (a pinwheel's whir the puppet makes itself, at the pitch the nearest one sets)
    if (!mp_fx_taking() || pos == NULL || id == NA_SE_TEMOCHI_KAZAGURUMA) {
        return;
    }
    for (k = 0; k < s_lvl_n && s_lvl[k] != id; k++) {
    }
    if (k == MP_LVL_MAX) {
        return;
    }
    s_lvl[k] = id;
    s_lvl_d[k][0] = (short)mp_sfx16(pos->x - s_fx_origin.x, 1.0f);
    s_lvl_d[k][1] = (short)mp_sfx16(pos->y - s_fx_origin.y, 1.0f);
    s_lvl_d[k][2] = (short)mp_sfx16(pos->z - s_fx_origin.z, 1.0f);
    if (k == s_lvl_n) {
        s_lvl_n++;
    }
}

void pc_mp_fx_sound(int kind, unsigned short id, const void* pos) {
    mp_fx_t fx;

    if (kind == MP_SND_ONESHOT && s_fx_hush == 0 && mp_npc_fx_sound(id, pos)) {
        return;
    }
    if (!mp_fx_taking() || pos == NULL) {
        return;
    }
    memset(&fx, 0, sizeof(fx));
    fx.kind = (unsigned char)kind;
    fx.id = id;
    mp_fx_push(&fx, (const xyz_t*)pos);
}

void pc_mp_fx_effect(int id, const void* pos, int prio, short angle, unsigned short item, short arg0, short arg1) {
    mp_fx_t fx;

    if (s_fx_hush == 0 && mp_npc_fx_effect(id, pos, prio, angle, item, arg0, arg1)) {
        return;
    }
    if (!mp_fx_taking() || pos == NULL || id < 0) {
        return;
    }
    memset(&fx, 0, sizeof(fx));
    fx.kind = MP_FX_EFFECT;
    fx.id = (unsigned short)id;
    fx.prio = (unsigned char)prio;
    fx.angle = angle;
    fx.item = item;
    fx.arg0 = arg0;
    fx.arg1 = arg1;
    mp_fx_push(&fx, (const xyz_t*)pos);
}

void pc_mp_fx_kill(int id, unsigned short item) {
    mp_fx_t fx;

    if (s_fx_hush == 0 && mp_npc_fx_kill(id, item)) {
        return;
    }
    if (!mp_fx_taking() || id < 0) {
        return;
    }
    memset(&fx, 0, sizeof(fx));
    fx.kind = MP_FX_KILL;
    fx.id = (unsigned short)id;
    fx.item = item;
    mp_fx_push(&fx, NULL);
}

// a thing drawn for the local player: its matrix, placed from the player, taken apart for the wire
void pc_mp_attach(int kind, int arg, int arg2, const unsigned char* rgba, const void* mtx) {
    GAME_PLAY* play = mp_live_play();
    PLAYER_ACTOR* player = (play != NULL) ? GET_PLAYER_ACTOR(play) : NULL;
    MtxF m;
    mp_att_t* a;

    if (!mp_active() || player == NULL || mtx == NULL || kind < 0 || kind >= MP_ATT_KINDS) {
        return;
    }
    m = *(const MtxF*)mtx;
    a = &s_att[kind].att;
    a->kind = (unsigned char)kind;
    a->arg = (unsigned char)arg;
    a->arg2 = (unsigned char)arg2;
    if (rgba != NULL) {
        memcpy(a->rgba, rgba, 4);
    } else {
        memset(a->rgba, 0xFF, 4);
    }
    a->dx = (short)mp_sfx16(m.xw - player->actor_class.world.position.x, 1.0f);
    a->dy = (short)mp_sfx16(m.yw - player->actor_class.world.position.y, 1.0f);
    a->dz = (short)mp_sfx16(m.zw - player->actor_class.world.position.z, 1.0f);
    a->scale[0] = sqrtf(m.xx * m.xx + m.yx * m.yx + m.zx * m.zx);
    a->scale[1] = sqrtf(m.xy * m.xy + m.yy * m.yy + m.zy * m.zy);
    a->scale[2] = sqrtf(m.xz * m.xz + m.yz * m.yz + m.zz * m.zz);
    {
        s_xyz rot;

        Matrix_to_rotate_new(&m, &rot, MTX_MULT);
        a->rot[0] = rot.x;
        a->rot[1] = rot.y;
        a->rot[2] = rot.z;
    }
    s_att[kind].frame = g_mp_frame;
    s_att[kind].valid = TRUE;
}

void pc_mp_snow_report(const mp_snow_t* snow) {
    if (!mp_active() || snow->part >= MP_SNOW_MAX) {
        return;
    }
    s_snow_mine[snow->part].snow = *snow;
    s_snow_mine[snow->part].frame = g_mp_frame;
    s_snow_mine[snow->part].valid = TRUE;
}

void pc_mp_ball_report(const mp_ball_t* ball) {
    if (!mp_active()) {
        return;
    }
    s_ball_mine.ball = *ball;
    s_ball_mine.frame = g_mp_frame;
    s_ball_mine.valid = TRUE;
}

int mp_ball_remote(mp_ball_t* ball, int* slot, unsigned int* age_ms) {
    unsigned int age = pc_mp_now_ms() - s_ball_theirs.ms;

    if (!s_ball_theirs.valid || age > MP_BALL_FRESH_MS) {
        return FALSE;
    }
    *ball = s_ball_theirs.ball;
    *slot = s_ball_theirs.slot;
    *age_ms = age;
    return TRUE;
}

int mp_snow_remote(int part, mp_snow_t* snow) {
    if (part < 0 || part >= MP_SNOW_MAX || !s_snow_theirs[part].valid ||
        pc_mp_now_ms() - s_snow_theirs[part].ms > MP_SNOW_FRESH_MS) {
        return FALSE;
    }
    *snow = s_snow_theirs[part].snow;
    return TRUE;
}

static void mp_snow_capture(mp_pstate_t* st) {
    int k;

    st->nsnow = 0;
    for (k = 0; k < MP_SNOW_MAX; k++) {
        if (s_snow_mine[k].valid && g_mp_frame - s_snow_mine[k].frame <= 2) {
            st->snow[st->nsnow++] = s_snow_mine[k].snow;
        }
    }
    if (st->nsnow > 0) {
        st->flags |= MP_PF_SNOW;
    }
    if (s_ball_mine.valid && g_mp_frame - s_ball_mine.frame <= 2) {
        st->ball = s_ball_mine.ball;
        st->flags |= MP_PF_BALL;
    }
}

// the things drawn for the player in the last couple of frames
static void mp_att_capture(mp_pstate_t* st) {
    int k;

    st->natt = 0;
    for (k = 0; k < MP_ATT_KINDS && st->natt < MP_ATT_MAX; k++) {
        if (s_att[k].valid && g_mp_frame - s_att[k].frame <= 2) {
            st->att[st->natt++] = s_att[k].att;
        }
    }
    if (st->natt > 0) {
        st->flags |= MP_PF_ATTACH;
    }
}

// the sounds and effects still young enough to repeat, oldest first
static void mp_fx_window(mp_pstate_t* st) {
    unsigned int now = pc_mp_now_ms();
    int n;
    int i;

    for (n = 0; n < MP_FX_MAX; n++) {
        unsigned short s = (unsigned short)(s_fx_next - 1 - n);
        unsigned int ms = s_fx_ring[s % MP_FX_RING].ms;

        if (ms == 0 || now - ms > MP_FX_WINDOW_MS) {
            break;
        }
    }
    st->fx_seq = (unsigned short)(s_fx_next - n);
    st->nfx = (unsigned char)n;
    for (i = 0; i < n; i++) {
        st->fx[i] = s_fx_ring[(unsigned short)(st->fx_seq + i) % MP_FX_RING].fx;
    }
}

// a peer's state repeats its newest sounds and effects; only unheard ones queue for its puppet
static void mp_fx_heard(mp_peer_t* p, const mp_pstate_t* st) {
    int i;

    for (i = 0; i < st->nfx; i++) {
        unsigned short seq = (unsigned short)(st->fx_seq + i);
        short ahead = (short)(seq - p->fx_heard);

        // far behind means the sender started counting over
        if (p->fx_any && ahead <= 0 && ahead > -1000) {
            continue;
        }
        p->fx_any = TRUE;
        p->fx_heard = seq;
        if (p->fxq_n == MP_FXQ) {
            p->fxq_at = (unsigned char)((p->fxq_at + 1) % MP_FXQ);
            p->fxq_n--;
        }
        p->fxq[(p->fxq_at + p->fxq_n) % MP_FXQ] = st->fx[i];
        p->fxq_n++;
    }
}

int mp_player_take_fx(int slot, mp_fx_t* fx) {
    mp_peer_t* p;

    if (slot < 0 || slot >= MP_MAX_PEERS || s_peer[slot].fxq_n == 0) {
        return FALSE;
    }
    p = &s_peer[slot];
    *fx = p->fxq[p->fxq_at];
    p->fxq_at = (unsigned char)((p->fxq_at + 1) % MP_FXQ);
    p->fxq_n--;
    return TRUE;
}

// whose house a room is; the game keeps the last house entered after leaving it, so only rooms count
static unsigned short mp_place_owner(void) {
    int type = mFI_GET_TYPE(mFI_GetFieldId());

    if (type == mFI_FIELD_NPCROOM0 || type == mFI_FIELD_PLAYER0_ROOM) {
        return (unsigned short)Common_Get(house_owner_name);
    }
    return 0;
}

// an item growing into or out of the hand, as Player_actor_draw_Normal shows it
static void mp_hold_capture(PLAYER_ACTOR* player, mp_pstate_t* st) {
    mActor_name_t item = EMPTY_NO;
    f32 scale = 0.0f;
    const xyz_t* pos = NULL;

    switch (player->now_main_index) {
        case mPlayer_INDEX_PICKUP:
            if (!player->main_data.pickup.signboard_flag) {
                item = player->main_data.pickup.item;
                scale = player->main_data.pickup.scale;
                pos = &player->main_data.pickup.item_pos;
            }
            break;
        case mPlayer_INDEX_PICKUP_JUMP:
            item = player->main_data.pickup_jump.item;
            scale = player->main_data.pickup_jump.scale;
            pos = &player->main_data.pickup_jump.item_pos;
            st->hold_jump = TRUE;
            // (off a table indoors: turned the way it stood there)
            if (!player->main_data.pickup_jump.ftr_flag && mFI_GET_TYPE(mFI_GetFieldId()) != mFI_FIELD_FG &&
                Common_Get(clip).shop_goods_clip != NULL) {
                int ux;
                int uz;

                if (mFI_Wpos2UtNum(&ux, &uz, player->main_data.pickup_jump.target_pos)) {
                    st->hold_angle = Common_Get(clip).shop_goods_clip->single_get_angle_y_proc(uz, ux, TRUE);
                }
            }
            break;
        case mPlayer_INDEX_PICKUP_FURNITURE:
            item = player->main_data.pickup_furniture.item;
            scale = player->main_data.pickup_furniture.scale;
            pos = &player->main_data.pickup_furniture.item_pos;
            break;
        case mPlayer_INDEX_GET_SCOOP:
            item = player->main_data.get_scoop.item;
            scale = player->main_data.get_scoop.scale;
            pos = player->keyframe0.frame_control.current_frame <= 42.0f ? &player->scoop_pos : &player->left_hand_pos;
            break;
        case mPlayer_INDEX_PUTAWAY_SCOOP:
            item = player->main_data.putaway_scoop.item;
            scale = player->main_data.putaway_scoop.scale;
            pos = &player->left_hand_pos;
            break;
    }
    if (scale > 0.0f && item != EMPTY_NO && pos != NULL) {
        st->flags |= MP_PF_HOLD;
        st->hold_item = item;
        st->hold_scale = scale;
        st->hold_dx = (short)mp_sfx16(pos->x - st->x, 1.0f);
        st->hold_dy = (short)mp_sfx16(pos->y - st->y, 1.0f);
        st->hold_dz = (short)mp_sfx16(pos->z - st->z, 1.0f);
    }
}

// what the player's draw adds on top of the body: pinned root, tool poses, flashes
static void mp_state_capture_extra(PLAYER_ACTOR* player, mp_pstate_t* st) {
    cKF_SkeletonInfo_R_c* kf = &player->keyframe0;
    int idx = player->now_main_index;

    st->rot_x = player->actor_class.shape_info.rotation.x;
    st->rot_z = player->actor_class.shape_info.rotation.z;
    if (idx == mPlayer_INDEX_DEMO_GETON_BOAT_SITDOWN) {
        st->roll = (short)-player->main_data.demo_geton_boat_sitdown.angle_z;
    } else if (idx == mPlayer_INDEX_DEMO_GETOFF_BOAT_STANDUP) {
        st->roll = (short)-player->main_data.demo_getoff_boat_standup.angle_z;
    } else if (idx == mPlayer_INDEX_DEMO_GETON_BOAT_WAIT || idx == mPlayer_INDEX_DEMO_GETON_BOAT_WADE) {
        st->roll = (short)-player->boat_angleZ;
    }
    st->item_scale = player->item_scale;
    if (kf->animation_enabled != 0) {
        st->flags |= MP_PF_ROOT;
        st->root_flags = (unsigned char)kf->animation_enabled;
        st->root_trans[0] = (short)mp_sfx16(kf->base_model_translation.x, 1.0f);
        st->root_trans[1] = (short)mp_sfx16(kf->base_model_translation.y, 1.0f);
        st->root_trans[2] = (short)mp_sfx16(kf->base_model_translation.z, 1.0f);
        st->root_rot_x = kf->base_model_rotation.x;
        st->root_rot_y = kf->updated_base_model_rotation.y;
        st->root_rot_z = kf->updated_base_model_rotation.z;
    }
    if (player->now_item_main_index == mPlayer_ITEM_MAIN_UMBRELLA_NORMAL && player->umbrella_actor != NULL) {
        UMBRELLA_ACTOR* umb = (UMBRELLA_ACTOR*)player->umbrella_actor;

        st->flags |= MP_PF_UMB;
        st->umb_action = (unsigned char)umb->action;
        st->umb_frame = umb->frame;
        st->umb_idx = (unsigned char)umb->tools_class.tool_name;
    }
    if (player->now_item_main_index >= mPlayer_ITEM_MAIN_NET_NORMAL &&
        player->now_item_main_index <= mPlayer_ITEM_MAIN_NET_COMPLETE_COLLECTION) {
        st->flags |= MP_PF_NET;
        st->net_angle[0] = player->net_angle.x;
        st->net_angle[1] = player->net_angle.y;
        st->net_angle[2] = player->net_angle.z;
    }
    if (player->now_item_main_index >= mPlayer_ITEM_MAIN_ROD_NORMAL &&
        player->now_item_main_index <= mPlayer_ITEM_MAIN_ROD_PUTAWAY && player->item_rod_angle_z != 0) {
        st->flags |= MP_PF_ROD;
        st->rod_angle_z = player->item_rod_angle_z;
    }
    if (player->change_color_flag) {
        st->flags |= MP_PF_STAR;
    }
    if (idx == mPlayer_INDEX_WASH_CAR && player->now_item_main_index == mPlayer_ITEM_MAIN_NONE) {
        st->flags |= MP_PF_WASH;
    }
    if (player->now_item_main_index == mPlayer_ITEM_MAIN_BALLOON_NORMAL) {
        st->flags |= MP_PF_BALLOON;
        st->balloon_x = (short)(player->balloon_lean_angle + player->balloon_angle.x + player->ballon_add_rot_x);
        st->balloon_z = player->balloon_angle.z;
    }
    mp_hold_capture(player, st);
    mp_att_capture(st);
    mp_snow_capture(st);
}

unsigned short mp_player_place(void) {
    return mp_place_owner();
}

static void mp_state_capture(GAME_PLAY* play, mp_pstate_t* st) {
    PLAYER_ACTOR* player = GET_PLAYER_ACTOR(play);
    int bank;

    memset(st, 0, sizeof(*st));
    st->scene = (unsigned short)Save_Get(scene_no);
    st->field = (unsigned short)mFI_GetFieldId();
    st->owner = mp_place_owner();
    st->x = player->actor_class.world.position.x;
    st->y = player->actor_class.world.position.y;
    st->z = player->actor_class.world.position.z;
    st->rot_y = player->actor_class.shape_info.rotation.y;
    st->head_x = player->head_angle.x;
    st->head_y = player->head_angle.y;
    st->main_index = (unsigned char)player->now_main_index;
    // at sea with Kapp'n or riding a train, the vehicle only exists on the rider's screen;
    // boarding and stepping off show, beside the watcher's own
    if (player->animation0_idx < 0 || player->animation1_idx < 0 || player->now_main_index == mPlayer_INDEX_DMA ||
        player->now_main_index == mPlayer_INDEX_HIDE || player->now_main_index == mPlayer_INDEX_DEMO_GETON_BOAT_SITDOWN ||
        player->now_main_index == mPlayer_INDEX_DEMO_GETON_BOAT_WAIT ||
        player->now_main_index == mPlayer_INDEX_DEMO_GETON_BOAT_WADE ||
        player->now_main_index == mPlayer_INDEX_DEMO_STANDING_TRAIN ||
        player->now_main_index == mPlayer_INDEX_DEMO_GETON_TRAIN_WAIT) {
        st->flags |= MP_PF_HIDDEN;
    } else {
        st->anim0 = (unsigned char)player->animation0_idx;
        st->anim1 = (unsigned char)player->animation1_idx;
    }
    st->part_table = (signed char)player->part_table_idx;
    st->frame0 = player->keyframe0.frame_control.current_frame;
    st->frame1 = player->keyframe1.frame_control.current_frame;
    st->speed0 = player->keyframe0.frame_control.speed;
    st->speed1 = player->keyframe1.frame_control.speed;
    // (played from the end back: the speed goes negative on the wire)
    if (player->keyframe0.frame_control.start_frame > player->keyframe0.frame_control.end_frame) {
        st->speed0 = -st->speed0;
    }
    if (player->keyframe1.frame_control.start_frame > player->keyframe1.frame_control.end_frame) {
        st->speed1 = -st->speed1;
    }
    if (player->keyframe0.frame_control.mode == cKF_FRAMECONTROL_REPEAT) {
        st->flags |= MP_PF_REPEAT0;
    }
    if (player->keyframe1.frame_control.mode == cKF_FRAMECONTROL_REPEAT) {
        st->flags |= MP_PF_REPEAT1;
    }
    {
        f32 rate = player->actor_class.shape_info.shadow_size_change_rate;

        rate = rate < 0.0f ? 0.0f : (rate > 1.0f ? 1.0f : rate);
        st->shadow = (unsigned char)((player->actor_class.shape_info.draw_shadow ? 0x80 : 0) | (int)(rate * 127.0f));
    }
    st->eye = (unsigned char)player->eye_tex_idx;
    st->mouth = (unsigned char)player->mouth_tex_idx;
    st->item_main = (unsigned char)player->now_item_main_index;
    bank = player->item_bank_idx & 1;
    st->item_shape = (signed char)player->item_shape_type[bank];
    st->item_anim = (signed char)player->item_animation_idx[bank];
    st->item_frame = player->item_keyframe.frame_control.current_frame;
    st->item_speed = player->item_keyframe.frame_control.speed;
    if (player->item_keyframe.frame_control.mode == cKF_FRAMECONTROL_REPEAT) {
        st->flags |= MP_PF_ITEM_REPEAT;
    }
    mp_state_capture_extra(player, st);
}

// presence

static int mp_self(void) {
    return mp_lobby_self_slot();
}

static int mp_peer_in_town(int slot) {
    return slot != mp_self() && s_peer[slot].valid;
}

int mp_player_in_town(int slot) {
    return slot >= 0 && slot < MP_MAX_PEERS && mp_peer_in_town(slot);
}

// host: every guest standing in town gets the others' latest states in one message
static void mp_host_send_states(void) {
    unsigned char msg[MP_STATE_MAX];
    int g;

    for (g = 1; g < MP_MAX_PEERS; g++) {
        int conn = mp_lobby_guest_conn(g);
        int n = 2;
        int count = 0;
        int s;

        if (conn < 0 || !s_peer[g].valid) {
            continue;
        }
        msg[0] = MP_S_PLAYERS;
        for (s = 0; s < MP_MAX_PEERS; s++) {
            // the host's own state, and guests' once they're in town
            if (s == g || !s_peer[s].state_valid || (s != 0 && !s_peer[s].valid)) {
                continue;
            }
            {
                unsigned char one[MP_PSTATE_WIRE];
                mp_pstate_t st = s_peer[s].st;
                int len;

                if (s != 0 && pc_mp_now_ms() - s_peer[s].st_ms > MP_RELAY_FRESH_MS) {
                    st.flags &= ~(MP_PF_BALL | MP_PF_SNOW);
                    st.nsnow = 0;
                }
                len = mp_pstate_pack(one, &st);

                // (too much for one message: what's gathered goes, and the rest follow in the next)
                if (n + 1 + len > (int)sizeof(msg) && count > 0) {
                    msg[1] = (unsigned char)count;
                    mp_lobby_send_state_to(conn, msg, n);
                    n = 2;
                    count = 0;
                }
                if (n + 1 + len > (int)sizeof(msg)) {
                    continue;
                }
                msg[n++] = (unsigned char)s;
                memcpy(msg + n, one, len);
                n += len;
            }
            count++;
        }
        msg[1] = (unsigned char)count;
        if (count > 0) {
            mp_lobby_send_state_to(conn, msg, n);
        }
    }
}

static void mp_send_my_look_to(int conn) {
    mp_look_send(conn, mp_self(), &s_my_look);
}

// the local player going in or out of a building: that door opens on the other screens too
static void mp_door_watch(GAME_PLAY* play) {
    PLAYER_ACTOR* player = GET_PLAYER_ACTOR(play);
    ACTOR* door = NULL;
    int out = FALSE;
    u8 msg[9];

    if (mFI_GET_TYPE(mFI_GetFieldId()) == mFI_FIELD_FG) {
        door = (ACTOR*)player->get_door_label_proc((GAME*)play);
        if (door == NULL && mPlib_check_player_outdoor_start((GAME*)play)) {
            door = Actor_info_fgName_search(&play->actor_info, Common_Get(door_data).door_actor_name, ACTOR_PART_ITEM);
            out = TRUE;
        }
    }
    if (door == s_door_used) {
        return;
    }
    s_door_used = door;
    if (door == NULL || door->part != ACTOR_PART_ITEM || ITEM_NAME_GET_TYPE(door->npc_id) != NAME_TYPE_STRUCT) {
        return;
    }
    msg[0] = MP_M_DOOR;
    msg[1] = (u8)mp_self();
    msg[2] = (u8)out;
    mp_w16(msg + 3, door->npc_id);
    mp_w16(msg + 5, (u16)(s16)door->home.position.x);
    mp_w16(msg + 7, (u16)(s16)door->home.position.z);
    if (mp_is_host()) {
        mp_host_relay(0, msg, (int)sizeof(msg));
    } else if (mp_lobby_host_conn() >= 0) {
        mp_lobby_send_rel(mp_lobby_host_conn(), msg, (int)sizeof(msg));
    }
}

static void mp_door_push(const unsigned char* p) {
    int k;
    int free_k = 0;

    for (k = 0; k < MP_DOORS; k++) {
        if (!s_door[k].used || pc_mp_now_ms() - s_door[k].ms >= MP_DOOR_MS) {
            free_k = k;
            break;
        }
        if ((s32)(s_door[k].ms - s_door[free_k].ms) < 0) {
            free_k = k;
        }
    }
    s_door[free_k].used = TRUE;
    s_door[free_k].out = p[2];
    s_door[free_k].name = (u16)mp_r16(p + 3);
    s_door[free_k].x = (s16)mp_r16(p + 5);
    s_door[free_k].z = (s16)mp_r16(p + 7);
    s_door[free_k].ms = pc_mp_now_ms();
}

int mp_door_take(void* structure, int* coming_out) {
    ACTOR* actor = (ACTOR*)structure;
    int k;

    for (k = 0; k < MP_DOORS; k++) {
        int dx = s_door[k].x - (int)actor->home.position.x;
        int dz = s_door[k].z - (int)actor->home.position.z;

        if (s_door[k].used && pc_mp_now_ms() - s_door[k].ms < MP_DOOR_MS && s_door[k].name == actor->npc_id &&
            dx >= -4 && dx <= 4 && dz >= -4 && dz <= 4) {
            s_door[k].used = FALSE;
            if (coming_out != NULL) {
                *coming_out = s_door[k].out;
            }
            return TRUE;
        }
    }
    return FALSE;
}

// the local player's look and state go out; the host also relays everyone else's
static void mp_player_send(GAME_PLAY* play) {
    mp_pstate_t* mine = &s_peer[mp_self()].st;
    mp_look_t look;
    int anim_changed;

    mp_door_watch(play);
    mp_look_capture(&look);
    if (!s_my_look_valid || !mp_look_same(&look, &s_my_look)) {
        unsigned int ver = s_my_look.ver + 1;

        s_my_look = look;
        s_my_look.ver = ver;
        s_my_look_valid = TRUE;
        if (mp_is_host()) {
            int g;

            for (g = 1; g < MP_MAX_PEERS; g++) {
                if (s_peer[g].valid && mp_lobby_guest_conn(g) >= 0) {
                    mp_send_my_look_to(mp_lobby_guest_conn(g));
                }
            }
        } else if (mp_lobby_host_conn() >= 0) {
            mp_send_my_look_to(mp_lobby_host_conn());
        }
    }

    mp_state_capture(play, mine);
    mine->nlvl = s_lvl_n;
    memcpy(mine->lvl, s_lvl, sizeof(s_lvl));
    memcpy(mine->lvl_d, s_lvl_d, sizeof(s_lvl_d));
    s_lvl_n = 0;
    {
        int ng = aHG_mp_capture(play, mine->goki, MP_GOKI_MAX);

        mine->goki_run = ng >= 0;
        mine->ngoki = (unsigned char)(ng > 0 ? ng : 0);
    }
    anim_changed = mine->anim0 != s_last_anim0 || mine->anim1 != s_last_anim1;
    if (++s_frame < MP_STATE_EVERY && !anim_changed && s_fx_next == s_fx_told) {
        return;
    }
    s_frame = 0;
    s_last_anim0 = mine->anim0;
    s_last_anim1 = mine->anim1;
    mine->seq = ++s_seq;
    mp_fx_window(mine);
    s_fx_told = s_fx_next;
    s_peer[mp_self()].state_valid = TRUE;
    s_peer[mp_self()].st_ms = pc_mp_now_ms();

    if (mp_is_host()) {
        mp_host_send_states();
    } else if (mp_lobby_host_conn() >= 0) {
        unsigned char msg[3 + MP_PSTATE_WIRE];
        int n = 2;

        msg[0] = MP_S_PLAYERS;
        msg[1] = 1;
        msg[n++] = (unsigned char)mp_self();
        n += mp_pstate_pack(msg + n, mine);
        mp_lobby_send_state_to(mp_lobby_host_conn(), msg, n);
    }
}

static int mp_same_place(const mp_pstate_t* st) {
    return st->scene == (unsigned short)Save_Get(scene_no) && st->field == (unsigned short)mFI_GetFieldId() &&
           st->owner == mp_place_owner();
}

// puppets come and go with who is standing in this scene
static void mp_player_presence(GAME_PLAY* play) {
    unsigned int now = pc_mp_now_ms();
    int s;

    for (s = 0; s < MP_MAX_PEERS; s++) {
        mp_peer_t* p = &s_peer[s];
        int want = mp_peer_in_town(s) && p->state_valid && now - p->st_ms < MP_STATE_DROP_MS && mp_same_place(&p->st);

        if (want && p->puppet == NULL) {
            p->fxq_n = 0;
            p->puppet = Actor_info_make_actor(&play->actor_info, (GAME*)play, mAc_PROFILE_MP_PLAYER, p->st.x, p->st.y,
                                              p->st.z, 0, p->st.rot_y, 0, -1, -1, -1, EMPTY_NO, (s16)s, -1, -1);
        } else if (!want && p->puppet != NULL) {
            Actor_delete(p->puppet);
            p->puppet = NULL;
        }
        if (p->puppet == NULL) {
            p->fxq_n = 0; // nobody here to show them
        }
    }
}

// town visuals

// a visitor on the train or at Porter is out of town: nothing it does shows here
static int mp_players_live(void) {
    return mp_is_host() || mp_travel_state() == MP_TRAVEL_VISITING || mp_travel_state() == MP_TRAVEL_RETURNING;
}

int mp_vfx_recording(void) {
    return mp_fx_taking();
}

void mp_vfx_send(const unsigned char* body, int len) {
    unsigned char msg[MP_VFX_HDR + MP_VFX_BODY];

    if (len <= 0 || len > MP_VFX_BODY || !mp_active() || !mp_players_live()) {
        return;
    }
    if (body[0] == MP_VFX_KK && len == 4 && !s_kk.again) {
        if (body[1] == MP_KK_SHOW) {
            s_kk.live = TRUE;
            s_kk.have = 0;
            s_kk.scene = (u16)Save_Get(scene_no);
            s_kk.ms = pc_mp_now_ms();
        }
        if (body[1] < MP_KK_SONG_OFF) {
            memcpy(s_kk.body[body[1]], body, 4);
            s_kk.have |= (u8)(1 << body[1]);
        } else {
            s_kk.live = FALSE;
        }
    }
    msg[0] = MP_M_VFX;
    msg[1] = (unsigned char)mp_self();
    mp_w16(msg + 2, (unsigned int)Save_Get(scene_no));
    mp_w16(msg + 4, (unsigned int)mFI_GetFieldId());
    mp_w16(msg + 6, mp_place_owner());
    memcpy(msg + MP_VFX_HDR, body, len);
    if (mp_is_host()) {
        mp_host_relay(0, msg, MP_VFX_HDR + len);
    } else if (mp_lobby_host_conn() >= 0) {
        mp_lobby_send_rel(mp_lobby_host_conn(), msg, MP_VFX_HDR + len);
    }
}

// what a screen that missed it would show wrong until rebuilt; the rest pass (sounds, puffs, sways)
static int mp_vfx_lasting(int kind) {
    return (kind >= MP_VFX_FTR_SWITCH && kind <= MP_VFX_FTR_RECORD) || kind == MP_VFX_KK || kind == MP_VFX_FLAG ||
           kind == MP_VFX_SNOW_BREAK || kind == MP_VFX_SNOW_DONE || kind == MP_VFX_ANGLES || kind == MP_VFX_PRESENT ||
           kind == MP_VFX_GOKI_KILL;
}

static void mp_vfx_queue(const unsigned char* msg, int len) {
    if (len > MP_VFX_HDR + MP_VFX_BODY) {
        return;
    }
    if (s_vfxq_n == MP_VFXQ) {
        // the oldest passing effect goes, else the oldest of the rest; a snowman's record waits for the host outdoors
        int k = 0;

        while (k < MP_VFXQ && mp_vfx_lasting(s_vfxq[k].msg[MP_VFX_HDR])) {
            k++;
        }
        if (k == MP_VFXQ) {
            k = 0;
            while (k < MP_VFXQ - 1 && s_vfxq[k].msg[MP_VFX_HDR] == MP_VFX_SNOW_DONE) {
                k++;
            }
        }
        memmove(&s_vfxq[k], &s_vfxq[k + 1], sizeof(s_vfxq[0]) * (MP_VFXQ - 1 - k));
        s_vfxq_n--;
    }
    s_vfxq[s_vfxq_n].len = len;
    s_vfxq[s_vfxq_n].ms = pc_mp_now_ms();
    memcpy(s_vfxq[s_vfxq_n].msg, msg, len);
    s_vfxq_n++;
}

static int mp_vfx_here(const unsigned char* msg) {
    return (unsigned short)mp_r16(msg + 2) == (unsigned short)Save_Get(scene_no) &&
           (unsigned short)mp_r16(msg + 4) == (unsigned short)mFI_GetFieldId() &&
           (unsigned short)mp_r16(msg + 6) == mp_place_owner() && mp_players_live();
}

// behind a menu the room's furniture follows the other screens as it goes (a turn or a new piece waits for play, and
// what comes after it in the room with it); what's for other places goes, as play would drop it
static void mp_vfx_menu(void) {
    int keep[MP_VFXQ];
    int nkeep = 0;
    int held = FALSE;
    int i;

    for (i = 0; i < s_vfxq_n; i++) {
        const unsigned char* msg = s_vfxq[i].msg;
        int kind = msg[MP_VFX_HDR];
        int room = kind >= MP_VFX_FTR_SWITCH && kind <= MP_VFX_FTR_RECORD;

        if (kind == MP_VFX_SNOW_DONE) {
            keep[nkeep++] = i;
            continue;
        }
        if (!mp_vfx_here(msg)) {
            continue;
        }
        if (room && !held && kind != MP_VFX_FTR_ROTATE && kind != MP_VFX_FTR_BIRTH) {
            aMR_mp_replay(msg + MP_VFX_HDR, s_vfxq[i].len - MP_VFX_HDR);
            continue;
        }
        if (room) {
            held = TRUE;
            s_vfxq[i].ms = pc_mp_now_ms(); // (its wait counts from when play can take it)
        }
        keep[nkeep++] = i;
    }
    for (i = 0; i < nkeep; i++) {
        s_vfxq[i] = s_vfxq[keep[i]];
    }
    s_vfxq_n = nkeep;
}

// a show running here is told again now and then: a player coming by the stage late still gets it
static void mp_kk_again(void) {
    int k;

    if (!s_kk.live) {
        return;
    }
    if (s_kk.scene != (u16)Save_Get(scene_no)) {
        s_kk.live = FALSE;
        return;
    }
    if (pc_mp_now_ms() - s_kk.ms < MP_KK_AGAIN_MS) {
        return;
    }
    s_kk.ms = pc_mp_now_ms();
    s_kk.again = TRUE;
    for (k = 0; k < MP_KK_SONG_OFF; k++) {
        if (s_kk.have & (1 << k)) {
            mp_vfx_send(s_kk.body[k], 4);
        }
    }
    s_kk.again = FALSE;
}

static void mp_vfx_tree(GAME_PLAY* play, const unsigned char* body, int len) {
    xyz_t pos;

    if (len < 17 || Common_Get(clip).make_effect_bg_proc == NULL) {
        return;
    }
    pos.x = mp_rf(body + 5);
    pos.y = mp_rf(body + 9);
    pos.z = mp_rf(body + 13);
    Common_Get(clip).make_effect_bg_proc((GAME*)play, mp_rs16(body + 1), mp_rs16(body + 3), &pos);
}

// the other screens' town visuals, where this player stands in the same place
static void mp_vfx_play(GAME_PLAY* play) {
    int keep[MP_VFXQ];
    int nkeep = 0;
    int held = FALSE; // a room change waits, and what follows it in the room with it
    int i;

    for (i = 0; i < s_vfxq_n; i++) {
        const unsigned char* msg = s_vfxq[i].msg;
        int len = s_vfxq[i].len;

        const unsigned char* body = msg + MP_VFX_HDR;
        int blen = len - MP_VFX_HDR;

        if (body[0] == MP_VFX_SNOW_DONE) {
            // the town's own record: the host keeps it until it stands outdoors to write it
            if (mp_is_host() && mFI_GET_TYPE(mFI_GetFieldId()) != mFI_FIELD_FG) {
                keep[nkeep++] = i;
            } else if (mp_is_host()) {
                aSMAN_mp_replay(play, body, blen);
            }
            continue;
        }
        if (!mp_vfx_here(msg) ||
            (!mp_vfx_lasting(body[0]) && pc_mp_now_ms() - s_vfxq[i].ms > MP_VFX_STALE_MS)) {
            continue;
        }
        if (body[0] == MP_VFX_TREE) {
            mp_vfx_tree(play, body, blen);
        } else if (body[0] == MP_VFX_NPC_FX) {
            mp_npc_fx_replay(play, body, blen);
        } else if (body[0] == MP_VFX_HEM) {
            aNHM_mp_replay(play, body, blen);
        } else if (body[0] == MP_VFX_KK) {
            aNTT_mp_replay(play, body, blen);
        } else if (body[0] == MP_VFX_ANGLES) {
            mp_house_angles_in(body, blen);
        } else if (body[0] == MP_VFX_PRESENT) {
            mp_npc_present_look(body, blen);
        } else if (body[0] == MP_VFX_BALLOON) {
            Ac_Balloon_mp_replay(play, body, blen);
        } else if (body[0] == MP_VFX_FISH) {
            aGYR_mp_replay(play, body, blen);
        } else if (body[0] >= MP_VFX_FTR_SWITCH && body[0] <= MP_VFX_FTR_RECORD) {
            if (pc_mp_now_ms() - s_vfxq[i].ms > MP_VFX_ROOM_MS) {
                continue;
            }
            if (held || !aMR_mp_replay(body, blen)) {
                held = TRUE;
                keep[nkeep++] = i;
            } else if (body[0] == MP_VFX_FTR_BIRTH) {
                held = TRUE; // (the new piece is up next frame, for what comes after it)
            }
        } else if (body[0] == MP_VFX_MAILBOX) {
            aMBX_mp_replay(play, body, blen);
        } else if (body[0] == MP_VFX_FLAG) {
            aFLAG_mp_replay(play, body, blen);
        } else if (body[0] == MP_VFX_SIGN) {
            aSIGN_mp_replay(body, blen);
        } else if (body[0] == MP_VFX_SNOW_BREAK) {
            aSMAN_mp_replay(play, body, blen);
        } else if (body[0] == MP_VFX_GOKI_KILL) {
            aHG_mp_replay(play, body, blen);
        } else if (body[0] == MP_VFX_GOKI_MAKE) {
            aMR_mp_goki_make(body, blen);
        } else if (Common_Get(clip).bg_item_clip != NULL && Common_Get(clip).bg_item_clip->mp_replay_proc != NULL) {
            Common_Get(clip).bg_item_clip->mp_replay_proc(body, blen);
        }
    }
    for (i = 0; i < nkeep; i++) {
        s_vfxq[i] = s_vfxq[keep[i]];
    }
    s_vfxq_n = nkeep;
}

// puppet access

int mp_puppets(void** actors, int max) {
    int n = 0;
    int s;

    if (!mp_active()) {
        return 0;
    }
    for (s = 0; s < MP_MAX_PEERS && n < max; s++) {
        if (s_peer[s].puppet != NULL && !(s_peer[s].st.flags & MP_PF_HIDDEN)) {
            actors[n++] = s_peer[s].puppet;
        }
    }
    return n;
}

int mp_puppet_catching(void* actor) {
    int s;

    for (s = 0; s < MP_MAX_PEERS; s++) {
        if (s_peer[s].puppet == (ACTOR*)actor && s_peer[s].state_valid) {
            const mp_pstate_t* st = &s_peer[s].st;
            int k;

            switch (st->main_index) {
                case mPlayer_INDEX_PULL_NET:
                case mPlayer_INDEX_NOTICE_NET:
                case mPlayer_INDEX_PUTAWAY_NET:
                case mPlayer_INDEX_FLY_ROD:
                case mPlayer_INDEX_NOTICE_ROD:
                case mPlayer_INDEX_PUTAWAY_ROD:
                    return TRUE;
                case mPlayer_INDEX_SWING_NET:
                    for (k = 0; k < st->natt; k++) {
                        if (st->att[k].kind == MP_ATT_INSECT) {
                            return TRUE;
                        }
                    }
                    break;
            }
        }
    }
    return FALSE;
}

void mp_nearest_player_pos(const void* pos, void* out_pos) {
    const xyz_t* p = (const xyz_t*)pos;
    xyz_t* out = (xyz_t*)out_pos;
    PLAYER_ACTOR* player = GET_PLAYER_ACTOR_NOW();
    void* puppets[MP_MAX_PEERS];
    int n = mp_puppets(puppets, MP_MAX_PEERS);
    f32 best;
    int i;

    *out = player->actor_class.world.position;
    best = search_position_distanceXZ((xyz_t*)p, out);
    for (i = 0; i < n; i++) {
        xyz_t* q = &((ACTOR*)puppets[i])->world.position;
        f32 d = search_position_distanceXZ((xyz_t*)p, q);

        if (d < best) {
            best = d;
            *out = *q;
        }
    }
}

const mp_look_t* mp_player_look(int slot) {
    if (slot < 0 || slot >= MP_MAX_PEERS || !s_peer[slot].valid) {
        return NULL;
    }
    return &s_peer[slot].look;
}

const mp_pstate_t* mp_player_state(int slot, unsigned int* age_ms) {
    if (slot < 0 || slot >= MP_MAX_PEERS || !s_peer[slot].state_valid) {
        return NULL;
    }
    *age_ms = pc_mp_now_ms() - s_peer[slot].st_ms;
    return &s_peer[slot].st;
}

void* mp_player_puppet(int slot) {
    return (slot >= 0 && slot < MP_MAX_PEERS) ? s_peer[slot].puppet : NULL;
}

void mp_player_puppet_gone(int slot, void* actor) {
    if (slot >= 0 && slot < MP_MAX_PEERS && s_peer[slot].puppet == (ACTOR*)actor) {
        s_peer[slot].puppet = NULL;
    }
}

// the houses' and the island cottage's ceiling lights: the host keeps them for everyone
#define MP_LIGHTS    (mRmTp_LIGHT_SWITCH_COTTAGE_MY + 1)
#define MP_LIGHT_ALL 0xFF

static struct {
    int valid;              // the lights here follow the host's
    u16 want;               // as the host has them
    u8 seq[MP_MAX_PEERS];   // host: each visitor's last switch it took
    u8 my_seq;              // guest: my last switch
} s_light;

static u16 mp_light_now(void) {
    u16 bits = 0;
    int i;

    for (i = 0; i < MP_LIGHTS; i++) {
        bits |= (u16)(mRmTp_Index2LightSwitchStatus(i) ? 1 << i : 0);
    }
    return bits;
}

// host: all the lights, to one visitor or to everyone
static void mp_light_send(int conn) {
    u8 msg[5 + MP_MAX_PEERS];
    int g;

    msg[0] = MP_M_LIGHT;
    msg[1] = 0;
    msg[2] = MP_LIGHT_ALL;
    msg[3] = (u8)s_light.want;
    msg[4] = (u8)(s_light.want >> 8);
    memcpy(msg + 5, s_light.seq, MP_MAX_PEERS);
    if (conn >= 0) {
        mp_lobby_send_rel(conn, msg, sizeof(msg));
        return;
    }
    for (g = 1; g < MP_MAX_PEERS; g++) {
        if (mp_lobby_guest_conn(g) >= 0) {
            mp_lobby_send_rel(mp_lobby_guest_conn(g), msg, sizeof(msg));
        }
    }
}

static void mp_light_host_start(void) {
    if (mp_is_host() && !s_light.valid) {
        s_light.want = mp_light_now();
        s_light.valid = TRUE;
    }
}

void mp_light_switched(int idx, int on) {
    if (!mp_active() || idx < 0 || idx >= MP_LIGHTS) {
        return;
    }
    mp_light_host_start();
    s_light.want = (u16)(on ? s_light.want | (1 << idx) : s_light.want & ~(1 << idx));
    if (mp_is_host()) {
        mp_light_send(-1);
    } else {
        u8 msg[5] = { MP_M_LIGHT, (u8)mp_self(), (u8)idx, (u8)(on != 0), ++s_light.my_seq };

        mp_lobby_send_rel(mp_lobby_host_conn(), msg, sizeof(msg));
    }
}

// another player is out on the island or in a cottage there
static int mp_others_on_island(void) {
    int s;

    for (s = 0; s < MP_MAX_PEERS; s++) {
        const mp_peer_t* p = &s_peer[s];
        xyz_t pos;
        int bx;
        int bz;

        if (s == mp_self() || !mp_peer_in_town(s) || !p->state_valid) {
            continue;
        }
        if (p->st.scene == SCENE_COTTAGE_MY || p->st.scene == SCENE_COTTAGE_NPC) {
            return TRUE;
        }
        pos.x = p->st.x;
        pos.y = 0.0f;
        pos.z = p->st.z;
        if (p->st.scene == SCENE_FG && mFI_Wpos2BlockNum(&bx, &bz, pos) &&
            (mFI_BkNum2BlockKind(bx, bz) & mRF_BLOCKKIND_ISLAND)) {
            return TRUE;
        }
    }
    return FALSE;
}

unsigned int mp_player_here_mask(void) {
    unsigned int now = pc_mp_now_ms();
    unsigned int mask = 0;
    int s;

    for (s = 0; s < MP_MAX_PEERS; s++) {
        const mp_peer_t* p = &s_peer[s];

        if (s != mp_self() && mp_peer_in_town(s) && p->state_valid && now - p->st_ms < 3000 && mp_same_place(&p->st)) {
            mask |= 1u << s;
        }
    }
    return mask;
}

// a room's cockroaches: the lowest slot in it runs them, the others' games show copies
int mp_goki_runner(void) {
    return !mp_active() || (mp_player_here_mask() & ((1u << mp_self()) - 1)) == 0;
}

const mp_goki_t* mp_goki_heard(int* n) {
    unsigned int mask = mp_player_here_mask();
    int s;

    for (s = 0; s < MP_MAX_PEERS; s++) {
        if (((mask >> s) & 1) && s_peer[s].st.goki_run) {
            *n = s_peer[s].st.ngoki;
            return s_peer[s].st.goki;
        }
    }
    *n = 0;
    return NULL;
}

int mp_others_in_block(int bx, int bz) {
    unsigned int now = pc_mp_now_ms();
    int s;

    for (s = 0; s < MP_MAX_PEERS; s++) {
        const mp_peer_t* p = &s_peer[s];
        xyz_t pos;
        int pbx;
        int pbz;

        if (s == mp_self() || !mp_peer_in_town(s) || !p->state_valid || now - p->st_ms > 3000 ||
            p->st.scene != SCENE_FG || mFI_GET_TYPE(p->st.field) != mFI_FIELD_FG) {
            continue;
        }
        pos.x = p->st.x;
        pos.y = 0.0f;
        pos.z = p->st.z;
        if (mp_town_block(&pos, &pbx, &pbz) && pbx == bx && pbz == bz) {
            return TRUE;
        }
    }
    return FALSE;
}

// another player in this room stands, sits or lies on that unit of it
int mp_player_on_unit(int ut) {
    unsigned int now = pc_mp_now_ms();
    int s;

    for (s = 0; s < MP_MAX_PEERS; s++) {
        const mp_peer_t* p = &s_peer[s];

        if (s == mp_self() || !mp_peer_in_town(s) || !p->state_valid || now - p->st_ms > 3000 ||
            !mp_same_place(&p->st) || p->st.x < 0.0f || p->st.z < 0.0f) {
            continue;
        }
        if ((int)(p->st.x / mFI_UT_WORLDSIZE_X_F) + (int)(p->st.z / mFI_UT_WORLDSIZE_Z_F) * UT_X_NUM == ut) {
            return TRUE;
        }
    }
    return FALSE;
}

// the boat sets the cottage's light by the hour; with someone already out there it stays as they have it
void mp_light_boat(void) {
    int idx = mRmTp_LIGHT_SWITCH_COTTAGE_MY;

    if (!mp_active() || !s_light.valid) {
        return;
    }
    if (mp_others_on_island()) {
        if ((s_light.want >> idx) & 1) {
            mRmTp_IndexLightSwitchON(idx);
        } else {
            mRmTp_IndexLightSwitchOFF(idx);
        }
    } else {
        mp_light_switched(idx, mRmTp_Index2LightSwitchStatus(idx));
    }
}

// the lights as the host has them; the room this player stands in switches as if by hand once play runs
static void mp_light_apply(GAME_PLAY* play) {
    int here;
    int i;

    mp_light_host_start();
    if (!s_light.valid) {
        return;
    }
    here = mRmTp_GetNowSceneLightSwitchIndex();
    for (i = 0; i < MP_LIGHTS; i++) {
        int want = (s_light.want >> i) & 1;

        if ((mRmTp_Index2LightSwitchStatus(i) != 0) == want) {
            continue;
        }
        if (i == here) {
            if (play == NULL) {
                continue;
            }
            if (want) {
                mEnv_RequestChangeLightON(play, mEnv_LIGHT_TYPE_PLAYER, TRUE);
            } else {
                mEnv_RequestChangeLightOFF(play, mEnv_LIGHT_TYPE_PLAYER, 0.0f);
            }
        } else if (want) {
            mRmTp_IndexLightSwitchON(i);
        } else {
            mRmTp_IndexLightSwitchOFF(i);
        }
    }
}

static void mp_light_on_rel(int slot, const unsigned char* data, int len) {
    if (mp_is_host()) {
        if (len >= 5 && data[2] < MP_LIGHTS) {
            mp_light_host_start();
            s_light.seq[slot] = data[4];
            s_light.want = (u16)(data[3] ? s_light.want | (1 << data[2]) : s_light.want & ~(1 << data[2]));
            mp_light_send(-1);
        }
    } else if (len >= 5 + MP_MAX_PEERS && data[2] == MP_LIGHT_ALL && mp_self() > 0 && mp_self() < MP_MAX_PEERS) {
        // (until the host has taken my last switch, its word is older than my room's)
        if (data[5 + mp_self()] == s_light.my_seq) {
            s_light.want = (u16)(data[3] | (data[4] << 8));
            s_light.valid = TRUE;
        }
    }
}

void mp_player_reset(void) {
    int s;

    for (s = 0; s < MP_MAX_PEERS; s++) {
        if (s_peer[s].puppet != NULL) {
            Actor_delete(s_peer[s].puppet);
        }
    }
    memset(s_peer, 0, sizeof(s_peer));
    memset(&s_light, 0, sizeof(s_light));
    memset(&s_kk, 0, sizeof(s_kk));
    mp_chat_reset();
    s_my_look_valid = FALSE;
    s_fx_told = s_fx_next;
    s_vfxq_n = 0;
}

// network

static void mp_host_relay(int from_slot, const unsigned char* data, int len) {
    int g;

    for (g = 1; g < MP_MAX_PEERS; g++) {
        if (g != from_slot && s_peer[g].valid && mp_lobby_guest_conn(g) >= 0) {
            mp_lobby_send_rel(mp_lobby_guest_conn(g), data, len);
        }
    }
}

void mp_player_relay(int from_slot, const unsigned char* data, int len) {
    mp_host_relay(from_slot, data, len);
}

void mp_player_on_rel(int conn, const unsigned char* data, int len) {
    int slot;

    if (len < 2) {
        return;
    }
    slot = data[1];
    if (slot >= MP_MAX_PEERS || slot == mp_self()) {
        return;
    }
    // a guest may only speak for itself; the host speaks for everyone
    if (mp_is_host() && mp_lobby_guest_slot(conn) + 1 != slot) {
        return;
    }
    if (!mp_is_host() && conn != mp_lobby_host_conn()) {
        return;
    }

    switch (data[0]) {
        case MP_M_LOOK:
            if (len >= MP_LOOK_WIRE) {
                int first = !s_peer[slot].valid;

                mp_look_unpack(data, &s_peer[slot].look);
                s_peer[slot].valid = TRUE;
                // guests hear about fellow visitors here; the host announces from the arrival
                if (first && !mp_is_host() && slot != 0) {
                    mp_notice_push(MP_MSG_N_ARRIVED, s_peer[slot].look.town, s_peer[slot].look.name);
                }
            }
            break;
        case MP_M_DESIGN:
            if (len >= 4 + 256 && data[2] < 2 && data[3] < 2) {
                unsigned char* tex = data[2] == 0 ? s_peer[slot].look.cloth_tex : s_peer[slot].look.umb_tex;

                memcpy(tex + data[3] * 256, data + 4, 256);
                s_peer[slot].look.ver++;
            }
            break;
        case MP_M_DOOR:
            if (len >= 9) {
                mp_door_push(data);
            }
            break;
        case MP_M_VFX:
            if (len >= MP_VFX_HDR + 1) {
                // (a visitor's switch in a house's floor the host isn't in: the floor keeps it)
                if (mp_is_host() && len >= MP_VFX_HDR + 14 && !mp_vfx_here(data) &&
                    (data[MP_VFX_HDR] == MP_VFX_FTR_SWITCH || data[MP_VFX_HDR] == MP_VFX_FTR_RECORD)) {
                    aMR_mp_keep_switch(mp_r16(data + 2), mp_r16(data + 4), data + MP_VFX_HDR, len - MP_VFX_HDR);
                }
                mp_vfx_queue(data, len);
            }
            break;
        case MP_M_LIGHT:
            mp_light_on_rel(slot, data, len);
            return;
        case MP_M_CHAT:
        case MP_M_CHAT_TYPING:
            mp_chat_on_rel(slot, data, len); // (passes on what it keeps itself)
            return;
        case MP_M_GONE:
            if (!mp_is_host() && s_peer[slot].valid) {
                mp_notice_push(MP_MSG_N_LEFT, s_peer[slot].look.town, s_peer[slot].look.name);
                mp_player_on_gone(slot);
            }
            return;
        default:
            return;
    }
    if (mp_is_host()) {
        mp_host_relay(slot, data, len);
    }
}

void mp_player_on_state(int conn, const unsigned char* data, int len) {
    int count;
    int pos = 2;
    int i;

    if (len < 2 || data[0] != MP_S_PLAYERS) {
        return;
    }
    count = data[1];
    for (i = 0; i < count && pos < len; i++) {
        int slot = data[pos++];
        mp_pstate_t st;
        int used = mp_pstate_unpack(data + pos, len - pos, &st);

        if (used < 0) {
            return;
        }
        pos += used;
        if (slot >= MP_MAX_PEERS || slot == mp_self()) {
            continue;
        }
        if (mp_is_host() && mp_lobby_guest_slot(conn) + 1 != slot) {
            continue;
        }
        // (the host passes a player's last state on while it has no newer one: that's no news of the player, and
        // one overtaken on the way is old news)
        if (s_peer[slot].state_valid && (signed char)(st.seq - s_peer[slot].st.seq) < 0 &&
            pc_mp_now_ms() - s_peer[slot].st_ms < 1000) {
            continue;
        }
        if (!s_peer[slot].state_valid || st.seq != s_peer[slot].st.seq) {
            s_peer[slot].st_ms = pc_mp_now_ms();
        }
        s_peer[slot].st = st;
        s_peer[slot].state_valid = TRUE;
        mp_fx_heard(&s_peer[slot], &st);
        if (st.flags & MP_PF_SNOW) {
            int k;

            for (k = 0; k < st.nsnow; k++) {
                if (st.snow[k].part < MP_SNOW_MAX && mp_same_place(&st)) {
                    s_snow_theirs[st.snow[k].part].snow = st.snow[k];
                    s_snow_theirs[st.snow[k].part].ms = pc_mp_now_ms();
                    s_snow_theirs[st.snow[k].part].valid = TRUE;
                }
            }
        }
        // the newest kick moves the ball; a tie goes to the lower slot
        if ((st.flags & MP_PF_BALL) && mp_same_place(&st) &&
            (!s_ball_theirs.valid || pc_mp_now_ms() - s_ball_theirs.ms > MP_BALL_FRESH_MS ||
             s_ball_theirs.slot == slot ||
             (st.ball.kick_ms != 0 &&
              (s_ball_theirs.ball.kick_ms == 0 || (s32)(st.ball.kick_ms - s_ball_theirs.ball.kick_ms) > 0)) ||
             (st.ball.kick_ms == s_ball_theirs.ball.kick_ms && slot < s_ball_theirs.slot))) {
            s_ball_theirs.ball = st.ball;
            s_ball_theirs.ms = pc_mp_now_ms();
            s_ball_theirs.slot = slot;
            s_ball_theirs.valid = TRUE;
        }
    }
}

// host: a guest stepped off the train; it learns everyone, everyone learns it
void mp_player_on_arrived(int conn) {
    int slot = mp_lobby_guest_slot(conn) + 1;
    int s;

    if (slot <= 0) {
        return;
    }
    if (s_my_look_valid) {
        mp_send_my_look_to(conn);
    }
    for (s = 1; s < MP_MAX_PEERS; s++) {
        if (s != slot && s_peer[s].valid) {
            mp_look_send(conn, s, &s_peer[s].look);
        }
    }
    mp_light_host_start();
    mp_light_send(conn);
}

// a quiet peer is back: the host sends that guest everyone's look again, a guest its own
void mp_player_on_back(int conn) {
    if (mp_is_host()) {
        mp_player_on_arrived(conn);
    } else if (s_my_look_valid) {
        mp_send_my_look_to(conn);
    }
}

void mp_player_on_gone(int slot) {
    if (slot < 0 || slot >= MP_MAX_PEERS) {
        return;
    }
    if (s_peer[slot].puppet != NULL) {
        Actor_delete(s_peer[slot].puppet);
    }
    memset(&s_peer[slot], 0, sizeof(s_peer[slot]));
    s_light.seq[slot] = 0; // (a visitor in this seat next counts its switches from the start)
    mp_chat_gone(slot);
    if (mp_is_host()) {
        unsigned char msg[2] = { MP_M_GONE, (unsigned char)slot };

        mp_host_relay(slot, msg, sizeof(msg));
    }
}

// frame

static int s_posted; // play_post ran since this frame began
static int s_still;  // frames in a row a menu has kept it from running
static int s_menu_run;

// host: the town is everyone's, so a menu only stops the host's own player
int pc_mp_host_runs_on(void) {
    int s;

    if (!mp_is_host()) {
        return FALSE;
    }
    for (s = 1; s < MP_MAX_PEERS; s++) {
        if (mp_lobby_guest_arrived(s)) {
            return TRUE;
        }
    }
    return FALSE;
}

// a visitor running the host's events or flying the town's balloon runs them on behind its menus too
int pc_mp_runs_on(void) {
    return pc_mp_host_runs_on() || mp_npc_runs_events() || mp_cr_balloon_flying_here();
}

int pc_mp_menu_running(void) {
    return s_menu_run;
}

void pc_mp_menu_run(int on) {
    s_menu_run = on;
}

// behind this game's menu the other players still come and go
void pc_mp_menu_pre(struct game_play_s* play) {
    mp_player_presence((GAME_PLAY*)play);
    aMR_mp_reconcile();
    if (mp_active()) {
        mp_vfx_menu();
        aMI_mp_refresh();
        mp_light_apply(NULL);
    }
}

// behind this game's menu: the characters' poses go out as ever, and whatever changed in the town (a
// visitor's pockets wait for its menu to close)
void pc_mp_menu_post(struct game_play_s* play) {
    mp_npc_menu_post(play);
    pc_mp_cr_menu_frame();
    if (mp_is_host()) {
        mp_world_post((GAME_PLAY*)play);
    } else {
        mp_world_menu_post((GAME_PLAY*)play);
    }
}

// the frame a menu closes runs the world again, though it began still
int mp_world_still(void) {
    return s_still > 0 && !s_posted;
}

int mp_player_still(int slot) {
    unsigned int age;
    const mp_pstate_t* st;

    if (slot == mp_self()) {
        return mp_world_still();
    }
    st = mp_player_state(slot, &age);
    return st == NULL || age > MP_QUIET_MS || (st->flags & MP_PF_STILL);
}

// this player's state as it stands, out to the others at once
static void mp_player_send_mine(void) {
    if (mp_is_host()) {
        mp_host_send_states();
    } else if (mp_lobby_host_conn() >= 0) {
        unsigned char msg[3 + MP_PSTATE_WIRE];
        int n = 2;

        msg[0] = MP_S_PLAYERS;
        msg[1] = 1;
        msg[n++] = (unsigned char)mp_self();
        n += mp_pstate_pack(msg + n, &s_peer[mp_self()].st);
        mp_lobby_send_state_to(mp_lobby_host_conn(), msg, n);
    }
}

// start of a play frame: while a menu stops this game's world, the others see this player paused
// and the host keeps passing everyone's states on
void mp_player_frame(GAME_PLAY* play) {
    mp_pstate_t* mine = &s_peer[mp_self()].st;

    s_still = s_posted ? 0 : s_still + 1;
    s_posted = FALSE;
    if (s_still == 0 || (s_still - 1) % MP_STILL_EVERY != 0 || !mp_players_live() ||
        !s_peer[mp_self()].state_valid) {
        return;
    }
    mine->flags = (mine->flags | MP_PF_STILL) & ~(MP_PF_BALL | MP_PF_SNOW);
    mine->nsnow = 0;
    mine->nfx = 0;
    mine->speed0 = 0.0f;
    mine->speed1 = 0.0f;
    mine->item_speed = 0.0f;
    mine->seq = ++s_seq;
    s_peer[mp_self()].st_ms = pc_mp_now_ms();
    mp_player_send_mine();
}

// the play game ended (a scene change, or an NES game taking over): nothing here points at its actors
void pc_mp_play_gone(int to_nes) {
    mp_pstate_t* mine = &s_peer[mp_self()].st;

    s_shown_cloth = -1;
    // (this player's puppet leaves the others' screens with the scene, not left standing in the doorway; at an NES
    // game it stays at the console)
    if (!to_nes && mp_active() && mp_players_live() && s_peer[mp_self()].state_valid &&
        !(mine->flags & MP_PF_HIDDEN)) {
        mine->flags = (mine->flags | MP_PF_HIDDEN) & ~(MP_PF_BALL | MP_PF_SNOW);
        mine->nsnow = 0;
        mine->nfx = 0;
        mine->seq = ++s_seq;
        s_peer[mp_self()].st_ms = pc_mp_now_ms();
        mp_player_send_mine();
    }
    s_still = 0;
    s_posted = TRUE;
    mp_world_play_gone();
    mp_npc_play_gone();
    mp_cr_play_gone();
}

// the host in an NES game has no play game: what it does for the town each frame goes on without one (villagers,
// events and creatures run on its visitors' screens, as when it's indoors), and everyone's moves still go round
void mp_host_headless_tick(void) {
    static int s_tick;

    if (!mp_is_host()) {
        return;
    }
    s_tick++;
    mp_player_frame(NULL);
    if ((s_tick & 1) == 0) {
        mp_host_send_states();
    }
    mp_world_frame(NULL);
    mp_npc_headless();
    mp_cr_still(NULL);
    mp_event_tick(NULL);
    aWeather_mp_headless_roll();
    // (an NES game only starts in a room: the post office's town checks want a play game)
    if (mp_npc_mail_here() && Save_Get(scene_no) != SCENE_FG) {
        pc_mp_mail_proc(NULL);
    }
    // (nothing of the host's own changes the town meanwhile: its diff and checksum rounds needn't run each tick)
    if ((s_tick & 3) == 0) {
        mp_world_post(NULL);
    }
    mp_travel_headless();
}

void pc_mp_play_pre(GAME_PLAY* play) {
    mp_player_presence(play);
    mp_vfx_play(play);
    aMR_mp_reconcile(); // (after the town's messages, before anything in the room moves)
    if (mp_active()) {
        aMI_mp_refresh(); // (a carpet or wallpaper laid on another screen)
        mp_light_apply(play);
        aHG_mp_frame(play);
    }
    mp_npc_frame(play);
}

void pc_mp_play_post(GAME_PLAY* play) {
    int anyone = FALSE;
    int s;

    s_posted = TRUE;
    mp_world_post(play);
    mp_event_tick(play);
    mp_npc_post(play);
    mp_cr_post(play);
    mp_kk_again();
    // a visitor saved for home still walks to the train on everyone's screen until its scene ends
    if (!mp_players_live() || GET_PLAYER_ACTOR(play) == NULL) {
        s_fx_told = s_fx_next;
        return;
    }
    for (s = 0; s < MP_MAX_PEERS; s++) {
        anyone |= mp_peer_in_town(s);
    }
    if (!anyone && !mp_is_guest()) {
        s_fx_told = s_fx_next;
        return;
    }
    mp_player_send(play);
}

#endif
