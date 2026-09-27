// pc_mp_critter.c
// the bugs, fish shadows and present balloon every screen in a shared town sees alike: the host rolls
// them, the nearest player's game runs each one and the others follow it by dead reckoning
#include "pc_mp.h"

#ifdef VITA_MP

#include "m_common_data.h"
#include "m_actor.h"
#include "m_field_info.h"
#include "m_play.h"
#include "m_player.h"
#include "m_player_lib.h"
#include "ac_insect_h.h"
#include "ac_gyoei.h"
#include "ac_fuusen.h"

#include <string.h>

#define MP_CR_MAX          96    // creatures out in the town at once
#define MP_CR_EVERY        3     // frames between sends
#define MP_CR_STALE_MS     1500  // a creature unheard this long: each game runs its own copy again
#define MP_CR_KEEP_MS      500   // a steady creature is repeated this often all the same
#define MP_CR_AHEAD        45    // frames a follower carries a report on at most
#define MP_CR_DRIFT        3.0f  // units a follower's guess may stray before a fresh report goes out
#define MP_CR_REACH        600.0f // a game keeps its copy this close to its player (the games' own cull)
#define MP_CR_OWNER_EVERY  15    // host: frames between ownership passes
#define MP_CR_OWNER_MS     4000  // host: the list goes out at least this often
#define MP_CR_QUIET_MS     2000  // host: an owner that never reports this long after taking one gives it up
#define MP_CR_ALONE_MS     15000 // guest: a host silent this long can't answer a step; this game rolls its own
#define MP_CR_ROLL_MS      1500  // host: what one player's roll lists comes in together, within this
#define MP_CR_KEEP         2.25f // (1.5 squared) a challenger has to be this much closer
#define MP_CR_HOLD_MS      8000  // a claim (a float in the water) holds its creatures this long
#define MP_CR_RECLAIM_MS   2000  // a claim is asked for, or renewed, this often
#define MP_CR_WADE_MS      1500  // guest: one request per acre step
#define MP_CR_SETTLE_MS    1500  // host: a new owner keeps it this long (a player still stepping in)
#define MP_CR_REMAKE_EVERY 30    // frames between checks for this acre's creatures missing here
#define MP_CR_ADOPT_EVERY  30    // host: frames between looks for its own creatures not on the list
#define MP_CR_WATCH_MS     20000 // a player unheard this long no longer keeps its acre's creatures out
#define MP_CR_LOOK_MS      2000  // guest: one look at its acre's list per this long
#define MP_CR_ACTS         4
#define MP_CR_ACT_MS       200   // another player's act this old is past answering
#define MP_CR_RETRY_MS     5000  // a copy this game's setup refused is tried again after this
#define MP_CR_LEASE_MS     4000  // host: a visitor rolling an acre itself has it this long
#define MP_CR_NET_MAX_MS   6000  // a net waits on the host's word this long at most, then comes up empty
#define MP_CR_GRANT_MS     10000 // host: a net given one keeps it against any other this long
#define MP_CR_OWN_MS       2000  // a visitor's own new one isn't dropped for missing from a list this soon
#define MP_CR_SCARE_MS     250   // one bolt request per fish per this long
#define MP_CR_BL_STALE_MS  5000  // a report this old no longer counts as flying (for snags and new copies)
#define MP_CR_BL_HOLD_MS   30000 // a copy waits this long for word from a runner gone quiet, or a new one
#define MP_CR_BL_QUIET_MS  1500  // host: a runner silent this long hands the balloon to someone who can fly it
#define MP_CR_POP_GONE     1     // MP_M_CR_POP's second byte: the runner's balloon flew off for good
#define MP_CR_BL_HOME_MS   1000  // host: back out, it takes the balloon over once its screen has it
#define MP_CR_ALIAS_MS     10000 // host: a creature's old id still finds it this long after it's renamed
#define MP_CR_GRANTS       16
#define MP_CR_BOLT         2     // GONE: darted off
#define MP_CR_BOLT_START   3     // GONE: darted off with a start
#define MP_CR_HDR          4
#define MP_CR_NOBODY       0xFF

// one moving thing, as its runner last saw it and as the others were last told
typedef struct {
    xyz_t pos;
    xyz_t vel; // per frame
    u8 blob[MP_CR_BLOB];
    u32 key;
    int frame;  // runner: when pos was taken; follower: when it was heard
    u32 ms;
    int have;
    // runner: what the others were told, and to whom it still has to go
    xyz_t sent_pos;
    xyz_t sent_vel;
    u32 sent_key;
    int sent_frame;
    u32 sent_ms;
    u8 due;   // bit per slot
    u8 again; // a change goes out twice, in case one is lost
} mp_cr_track_t;

typedef struct {
    u16 id; // 0: free
    u8 kind;
    u8 type;
    s8 bx;
    s8 bz;
    u16 field;
    s16 extra;
    u8 owner;
    u32 owner_ms;
    u32 hold_ms; // host: a float's claim keeps it with its owner until this is old
    u32 ask_ms;  // this game last asked for it
    u32 retry_ms; // this game's copy was refused where it is; not made again before this
    xyz_t spawn;  // where it came out, a spot its setup accepts on every screen
    u8 made_here; // this game's own roll, not a copy of another's
    u8 nocopy;    // host: players whose games refused a copy lately (bit per slot)
    u32 nocopy_ms;
    u32 scare_ms; // this game last asked its runner to bolt it
    u16 old_id;   // host: its id before its maker left, which messages on their way may still carry
    u32 old_ms;
    u32 pp_seq;   // guest: one let go from the pockets waits for the passport without it before it's listed
    u8 unlisted;
    u32 pp_retry_ms;
    u32 listed_ms; // host: when it went on the list, and whose roll it was
    u8 lister;
    mp_cr_track_t tr;
    ACTOR* actor; // this game's copy
} mp_cr_t;

// fish left at this player's float after they were caught or went elsewhere: never listed again, gone once off it
#define MP_CR_ORPHANS 4
static void* s_cr_orphan[MP_CR_ORPHANS];

static void mp_cr_orphan_add(void* actor) {
    int i;

    for (i = 0; i < MP_CR_ORPHANS; i++) {
        if (s_cr_orphan[i] == NULL || s_cr_orphan[i] == actor) {
            s_cr_orphan[i] = actor;
            return;
        }
    }
    s_cr_orphan[0] = actor;
}

static int mp_cr_orphan_take(void* actor) {
    int i;

    for (i = 0; i < MP_CR_ORPHANS; i++) {
        if (actor != NULL && s_cr_orphan[i] == actor) {
            s_cr_orphan[i] = NULL;
            return TRUE;
        }
    }
    return FALSE;
}

typedef struct {
    s8 action;
    s16 ut_x;
    s16 ut_z;
    u32 ms;
} mp_cr_act_t;

static struct {
    mp_cr_t cr[MP_CR_MAX];
    u16 next_id;
    int frame;
    int still_frame;
    int menu_ran; // the balloon flew on behind this game's menu last frame
    u32 owners_ms;
    int owners_dirty;
    // the set manager rolling an acre's bugs and fish
    int rolling;
    int remote; // host: the roll is another player's step; tell it, make nothing here
    int remote_slot;
    s8 roll_bx;
    s8 roll_bz;
    u16 binding;  // making this game's copy of that creature now
    int caught;   // removing copies of one somebody caught
    // guest: the step already asked about, and the host's say on rolling one itself
    s8 wade_bx;
    s8 wade_bz;
    u32 wade_ms;
    int self_roll; // kinds of this roll that are this game's to make
    int self_req;
    s8 self_bx;
    s8 self_bz;
    int was_out;
    int look_due;
    u32 look_ms;
    // host: steps and looks the others asked about, answered after the actors
    u8 req[MP_MAX_PEERS];
    u8 req_roll[MP_MAX_PEERS];
    s8 req_bx[MP_MAX_PEERS];
    s8 req_bz[MP_MAX_PEERS];
    u16 req_field[MP_MAX_PEERS];
    // a tree shaken, a hole dug or a rock hit on another screen, for the bugs this game runs
    mp_cr_act_t act[MP_CR_ACTS];
    int nact;
    // the present balloon: its runner flies it, the others follow
    mp_cr_track_t bl;
    u16 bl_field;
    int bl_pop;    // runner: another player shook the tree it's caught in
    u8 bl_runner;  // the host's say
    u32 bl_out_ms; // host: when it came out into town
    u32 bl_gone_ms; // its runner's balloon flew off for good (a late report of it counts for nothing)
    // guest: ids for what its own game makes
    u16 own_next;
    // host: an acre a visitor rolls itself, while the host can't; and the host's own step it held for one
    struct {
        s8 bx;
        s8 bz;
        u16 field;
        u32 ms;
    } lease[MP_MAX_PEERS];
    int host_wait;
    s8 host_wait_bx;
    s8 host_wait_bz;
    u16 host_wait_field;
    // this player's net: what's in it, and whether the host has said whose it is
    ACTOR* net_actor;
    int net_ant;
    u16 net_id;
    u32 net_ms;
    int net_wait;
    int net_lost; // host: its own net lost to a visitor's that came first
    // host: catches it gave lately, so a second net on the same one comes up empty
    struct {
        u16 id;
        u8 slot;
        u32 ms;
    } grant[MP_CR_GRANTS];
    int grant_at;
} s_cr;

int aSetMgr_mp_roll(struct game_play_s* play, int bx, int bz); // ac_set_manager.c

static void mp_put16(u8* p, u32 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static u32 mp_get16(const u8* p) {
    return (u32)p[0] | ((u32)p[1] << 8);
}

static void mp_put_fix(u8* p, f32 v, f32 scale) {
    s32 i = (s32)(v * scale);

    mp_put16(p, (u32)(s16)(i > 32767 ? 32767 : (i < -32768 ? -32768 : i)));
}

static f32 mp_get_fix(const u8* p, f32 scale) {
    return (f32)(s16)mp_get16(p) / scale;
}

// the town is shared: a host with a visitor off the train, or a visitor in it
static int mp_cr_shared(void) {
    int s;

    if (mp_is_host()) {
        for (s = 1; s < MP_MAX_PEERS; s++) {
            if (mp_lobby_guest_arrived(s)) {
                return TRUE;
            }
        }
        return FALSE;
    }
    return mp_travel_state() == MP_TRAVEL_VISITING;
}

static int mp_cr_outdoors(void) {
    return mp_cr_shared() && Save_Get(scene_no) == SCENE_FG;
}

static mp_cr_t* mp_cr_by_id(u16 id) {
    int i;

    for (i = 0; i < MP_CR_MAX && id != 0; i++) {
        if (s_cr.cr[i].id == id) {
            return &s_cr.cr[i];
        }
    }
    return NULL;
}

// host: the creature a visitor's message means, which may still carry an id from before a rename
static mp_cr_t* mp_cr_by_msg_id(u16 id) {
    mp_cr_t* c = mp_cr_by_id(id);
    int i;

    for (i = 0; i < MP_CR_MAX && c == NULL && id != 0 && mp_is_host(); i++) {
        if (s_cr.cr[i].id != 0 && s_cr.cr[i].old_id == id && pc_mp_now_ms() - s_cr.cr[i].old_ms < MP_CR_ALIAS_MS) {
            c = &s_cr.cr[i];
        }
    }
    return c;
}

static mp_cr_t* mp_cr_by_actor(void* actor) {
    int i;

    for (i = 0; i < MP_CR_MAX && actor != NULL; i++) {
        if (s_cr.cr[i].id != 0 && s_cr.cr[i].actor == (ACTOR*)actor) {
            return &s_cr.cr[i];
        }
    }
    return NULL;
}

static mp_cr_t* mp_cr_new(u16 id) {
    int i;

    for (i = 0; i < MP_CR_MAX; i++) {
        if (s_cr.cr[i].id == 0) {
            memset(&s_cr.cr[i], 0, sizeof(s_cr.cr[i]));
            s_cr.cr[i].id = id;
            s_cr.cr[i].owner_ms = pc_mp_now_ms();
            s_cr.cr[i].listed_ms = s_cr.cr[i].owner_ms;
            return &s_cr.cr[i];
        }
    }
    return NULL;
}

// the host's ids; a visitor's own come from a range of its slot's
static u16 mp_cr_next_id(void) {
    do {
        s_cr.next_id = (u16)((s_cr.next_id + 1) & 0x7FFF);
    } while (s_cr.next_id == 0 || mp_cr_by_id(s_cr.next_id) != NULL);
    return s_cr.next_id;
}

static void mp_cr_wipe(void) {
    u16 next = s_cr.next_id;

    memset(&s_cr, 0, sizeof(s_cr));
    memset(s_cr_orphan, 0, sizeof(s_cr_orphan));
    s_cr.next_id = next;
    // a visitor back in the same slot doesn't reuse its last visit's ids
    s_cr.own_next = (u16)(pc_mp_now_ms() & 0xFFF);
}

static u16 mp_cr_own_id(void) {
    u16 id;

    do {
        s_cr.own_next = (u16)((s_cr.own_next + 1) & 0xFFF);
        id = (u16)(0x8000 | ((mp_lobby_self_slot() & 7) << 12) | s_cr.own_next);
    } while (s_cr.own_next == 0 || mp_cr_by_id(id) != NULL);
    return id;
}

// dead reckoning

// where a report has got to after some frames; the runner's check and the follower's guess agree
static void mp_cr_carry(const xyz_t* pos, const xyz_t* vel, int ahead, xyz_t* out) {
    if (ahead > MP_CR_AHEAD) {
        ahead = MP_CR_AHEAD;
    }
    out->x = pos->x + vel->x * ahead;
    out->y = pos->y + vel->y * ahead;
    out->z = pos->z + vel->z * ahead;
}

// runner: the newest look; a follower's guess from the last report is checked against it
static void mp_cr_track_take(mp_cr_track_t* t, const void* pos, const void* blob, u32 key) {
    xyz_t p;
    xyz_t guess;
    int ahead;

    memcpy(&p, pos, sizeof(p));
    if (t->have && s_cr.frame - t->frame == 1) {
        t->vel.x = p.x - t->pos.x;
        t->vel.y = p.y - t->pos.y;
        t->vel.z = p.z - t->pos.z;
    } else {
        t->vel.x = t->vel.y = t->vel.z = 0.0f;
    }
    t->pos = p;
    memcpy(t->blob, blob, MP_CR_BLOB);
    t->key = key;
    t->frame = s_cr.frame;
    t->ms = pc_mp_now_ms();
    t->have = TRUE;
    ahead = s_cr.frame - t->sent_frame;
    mp_cr_carry(&t->sent_pos, &t->sent_vel, ahead, &guess);
    if (t->key != t->sent_key ||
        (guess.x - p.x) * (guess.x - p.x) + (guess.y - p.y) * (guess.y - p.y) + (guess.z - p.z) * (guess.z - p.z) >
            MP_CR_DRIFT * MP_CR_DRIFT) {
        t->again = TRUE;
    } else if (t->again && ahead >= MP_CR_EVERY) {
        t->again = FALSE;
    } else if (pc_mp_now_ms() - t->sent_ms <= MP_CR_KEEP_MS) {
        return;
    }
    t->sent_pos = p;
    t->sent_vel = t->vel;
    t->sent_key = key;
    t->sent_frame = s_cr.frame;
    t->sent_ms = pc_mp_now_ms();
    t->due = 0xFF;
}

// follower: where it has got to since the report
static void mp_cr_track_guess(const mp_cr_track_t* t, xyz_t* pos) {
    mp_cr_carry(&t->pos, &t->vel, s_cr.frame - t->frame, pos);
}

static int mp_cr_track_put(u8* p, const mp_cr_track_t* t, int blob_len) {
    mp_put_fix(p, t->pos.x, 4.0f);
    mp_put_fix(p + 2, t->pos.y, 4.0f);
    mp_put_fix(p + 4, t->pos.z, 4.0f);
    mp_put_fix(p + 6, t->vel.x, 256.0f);
    mp_put_fix(p + 8, t->vel.y, 256.0f);
    mp_put_fix(p + 10, t->vel.z, 256.0f);
    p[12] = (u8)blob_len;
    memcpy(p + 13, t->blob, blob_len);
    return 13 + blob_len;
}

static int mp_cr_track_get(const u8* p, int left, mp_cr_track_t* t) {
    int blob_len;

    if (left < 13 || (blob_len = p[12]) > MP_CR_BLOB || left < 13 + blob_len) {
        return -1;
    }
    t->pos.x = mp_get_fix(p, 4.0f);
    t->pos.y = mp_get_fix(p + 2, 4.0f);
    t->pos.z = mp_get_fix(p + 4, 4.0f);
    t->vel.x = mp_get_fix(p + 6, 256.0f);
    t->vel.y = mp_get_fix(p + 8, 256.0f);
    t->vel.z = mp_get_fix(p + 10, 256.0f);
    memset(t->blob, 0, MP_CR_BLOB);
    memcpy(t->blob, p + 13, blob_len);
    t->frame = s_cr.frame;
    t->ms = pc_mp_now_ms();
    t->have = TRUE;
    t->due = 0xFF; // the host passes it on
    return 13 + blob_len;
}

// players

// where a player stands outdoors; running a creature takes a player whose world runs, keeping one out
// only a player still around (a menu open, a short silence)
static int mp_cr_player_at(GAME_PLAY* play, int s, xyz_t* pos, u16* field, int watching) {
    if (s == mp_lobby_self_slot()) {
        PLAYER_ACTOR* me = (play != NULL) ? GET_PLAYER_ACTOR(play) : NULL;

        if (me == NULL || Save_Get(scene_no) != SCENE_FG || (!watching && mp_world_still())) {
            return FALSE;
        }
        *pos = me->actor_class.world.position;
        *field = (u16)mFI_GetFieldId();
        return TRUE;
    } else {
        unsigned int age;
        const mp_pstate_t* st = mp_player_state(s, &age);

        if (st == NULL || st->scene != SCENE_FG || (st->flags & MP_PF_HIDDEN) ||
            (mp_is_host() && !mp_lobby_guest_arrived(s)) ||
            (watching ? age > MP_CR_WATCH_MS : (age > MP_CR_STALE_MS || (st->flags & MP_PF_STILL)))) {
            return FALSE;
        }
        pos->x = st->x;
        pos->y = st->y;
        pos->z = st->z;
        *field = st->field;
        return TRUE;
    }
}

// the player stands in or next to the acre
static int mp_cr_player_near_acre(GAME_PLAY* play, int s, int bx, int bz, u16 field) {
    xyz_t pos;
    u16 pf;
    int pbx;
    int pbz;

    return mp_cr_player_at(play, s, &pos, &pf, TRUE) && pf == field && mp_town_block(&pos, &pbx, &pbz) &&
           pbx - bx <= 1 && bx - pbx <= 1 && pbz - bz <= 1 && bz - pbz <= 1;
}

// sending

static void mp_cr_send_host(const u8* msg, int len) {
    if (mp_lobby_host_conn() >= 0) {
        mp_lobby_send_rel(mp_lobby_host_conn(), msg, len);
    }
}

static void mp_cr_send_guest(int g, const u8* msg, int len) {
    if (g > 0 && g < MP_MAX_PEERS && mp_lobby_guest_arrived(g) && mp_lobby_guest_conn(g) >= 0) {
        mp_lobby_send_rel(mp_lobby_guest_conn(g), msg, len);
    }
}

static void mp_cr_send_guests(const u8* msg, int len, int except_slot) {
    int g;

    for (g = 1; g < MP_MAX_PEERS; g++) {
        if (g != except_slot) {
            mp_cr_send_guest(g, msg, len);
        }
    }
}

// the host to whoever runs one, a guest to the host (which passes it on)
static void mp_cr_send_owner(const mp_cr_t* c, const u8* msg, int len) {
    if (mp_is_host()) {
        mp_cr_send_guest(c->owner, msg, len);
    } else {
        mp_cr_send_host(msg, len);
    }
}

static int mp_cr_spawn_msg(u8* msg, const mp_cr_t* c) {
    xyz_t at = c->spawn;

    msg[0] = MP_M_CR_SPAWN;
    msg[1] = c->kind;
    msg[2] = c->type;
    msg[3] = c->owner;
    mp_put16(msg + 4, c->id);
    msg[6] = (u8)c->bx;
    msg[7] = (u8)c->bz;
    mp_put16(msg + 8, (u16)c->extra);
    mp_put16(msg + 10, c->field);
    memcpy(msg + 12, &at, 12);
    return 24;
}

// host: a new creature goes to the visitors near its acre (not the one whose game made it)
static void mp_cr_tell_spawn(GAME_PLAY* play, const mp_cr_t* c, int also_slot, int except_slot) {
    u8 msg[24];
    int len = mp_cr_spawn_msg(msg, c);
    int g;

    for (g = 1; g < MP_MAX_PEERS; g++) {
        if (g != except_slot && (g == also_slot || mp_cr_player_near_acre(play, g, c->bx, c->bz, c->field))) {
            mp_cr_send_guest(g, msg, len);
        }
    }
}

static void mp_cr_send_gone(u16 id, int caught) {
    u8 msg[4];

    msg[0] = MP_M_CR_GONE;
    mp_put16(msg + 1, id);
    msg[3] = (u8)caught;
    if (mp_is_host()) {
        mp_cr_send_guests(msg, 4, -1);
    } else {
        mp_cr_send_host(msg, 4);
    }
}

// copies

// this game's copy of a creature someone else rolled
static void mp_cr_materialize(GAME_PLAY* play, mp_cr_t* c) {
    if (c->actor != NULL || Save_Get(scene_no) != SCENE_FG || c->field != (u16)mFI_GetFieldId() ||
        (c->retry_ms != 0 && (s32)(pc_mp_now_ms() - c->retry_ms) < 0)) {
        return;
    }
    s_cr.binding = c->id;
    if (c->kind == MP_CR_INSECT && CLIP(insect_clip) != NULL) {
        aINS_Init_c init;

        init.insect_type = c->type;
        init.position = c->spawn;
        init.extra_data = c->extra;
        init.game = (GAME*)play;
        if (c->type == aINS_INSECT_TYPE_ANT) {
            // an ant is its own actor, made next frame, and stays each game's own
            CLIP(insect_clip)->make_ant_proc(&init, c->bx, c->bz);
        } else {
            CLIP(insect_clip)->make_insect_proc(&init, aINS_MAKE_NEW);
        }
    } else if (c->kind == MP_CR_FISH && CLIP(gyo_clip) != NULL) {
        aGYO_Init_c init;

        init.fish_type = c->type;
        init.position = c->spawn;
        init.extra_data = c->extra;
        init.game = (GAME*)play;
        CLIP(gyo_clip)->make_gyoei_proc(&init);
    }
    s_cr.binding = 0;
}

// this game's copy goes with the creature; caught, it doesn't move on elsewhere (a spirit); a fish that
// darted off leaves its shadow going here too
static void mp_cr_remove(GAME_PLAY* play, mp_cr_t* c, int caught) {
    ACTOR* actor = c->actor;
    int kind = c->kind;

    memset(c, 0, sizeof(*c));
    if (actor == NULL || play == NULL) {
        return;
    }
    s_cr.caught = caught == TRUE;
    if (kind == MP_CR_INSECT && CLIP(insect_clip) != NULL) {
        // one in this player's net waits on the host's word
        if ((ACTOR*)mPlib_Get_item_net_catch_label() != actor) {
            mPlib_mp_forget_catch((u32)actor);
            if (actor->id == mAc_PROFILE_ANT) {
                aANT_mp_vanish(actor);
            } else {
                CLIP(insect_clip)->dt_proc(actor, (GAME*)play);
            }
        }
    } else if (kind == MP_CR_FISH && CLIP(gyo_clip) != NULL) {
        // and one at this player's float (nosing it, biting, on the line) is theirs; one only nosing darts off, and
        // none is ever listed again (it goes once it's off the float)
        if ((((aGYO_CTRL_ACTOR*)actor)->gyo_flags & (2 | 4 | 8)) == 0) {
            if (caught >= MP_CR_BOLT) {
                aGYO_mp_bolt_copy(actor, play, caught == MP_CR_BOLT_START);
            }
            CLIP(gyo_clip)->dt_gyoei_proc(actor, (GAME*)play);
        } else {
            if ((((aGYO_CTRL_ACTOR*)actor)->gyo_flags & (4 | 8)) == 0) {
                aGYO_mp_scare(actor);
            }
            mp_cr_orphan_add(actor);
        }
    }
    s_cr.caught = FALSE;
}

int pc_mp_cr_caught_elsewhere(void) {
    return s_cr.caught;
}

int pc_mp_cr_listed(void* actor) {
    return mp_cr_by_actor(actor) != NULL;
}

// rolls

// host: a visitor other than that slot is rolling the acre itself
static int mp_cr_leased(int bx, int bz, u16 field, int except_slot) {
    int g;

    for (g = 1; g < MP_MAX_PEERS; g++) {
        if (g != except_slot && s_cr.lease[g].ms != 0 && pc_mp_now_ms() - s_cr.lease[g].ms < MP_CR_LEASE_MS &&
            s_cr.lease[g].bx == bx && s_cr.lease[g].bz == bz && s_cr.lease[g].field == field) {
            return TRUE;
        }
    }
    return FALSE;
}

void pc_mp_cr_roll(int begin, int bx, int bz) {
    s_cr.rolling = begin && mp_cr_outdoors();
    s_cr.roll_bx = (s8)bx;
    s_cr.roll_bz = (s8)bz;
}

int mp_cr_remote_roll(void) {
    return s_cr.rolling && s_cr.remote;
}

// guest: the host has the say on every step's bugs and fish (it rolls them, or lets this game roll and
// list them while it can't); ask once a step
int mp_cr_guest_step(int bx, int bz, int kind) {
    unsigned int age;
    const mp_pstate_t* host;
    u8 msg[5];

    if (mp_is_host() || !mp_cr_outdoors()) {
        return FALSE;
    }
    // the host said to make these here
    if (s_cr.self_roll != 0) {
        return (s_cr.self_roll & (1 << kind)) == 0;
    }
    // (a host long gone quiet can't answer: this game rolls its own; one quiet a moment hears of the step once back)
    host = mp_player_state(0, &age);
    if (host == NULL || age > MP_CR_ALONE_MS || mp_lobby_host_conn() < 0) {
        return FALSE;
    }
    if (bx != s_cr.wade_bx || bz != s_cr.wade_bz || pc_mp_now_ms() - s_cr.wade_ms > MP_CR_WADE_MS) {
        s_cr.wade_bx = (s8)bx;
        s_cr.wade_bz = (s8)bz;
        s_cr.wade_ms = pc_mp_now_ms();
        msg[0] = MP_M_CR_WADE;
        msg[1] = (u8)bx;
        msg[2] = (u8)bz;
        mp_put16(msg + 3, (u16)mFI_GetFieldId());
        mp_cr_send_host(msg, 5);
    }
    return TRUE;
}

static int mp_cr_remakes(const mp_cr_t* c) {
    return !(c->kind == MP_CR_INSECT && (c->type == aINS_INSECT_TYPE_ANT || c->extra == aINS_INIT_RELEASE));
}

// host: an acre already out comes from the list; for another player's step, the kinds out skip their roll
int mp_cr_host_step(struct game_play_s* play_s, int bx, int bz, int kind) {
    GAME_PLAY* play = (GAME_PLAY*)play_s;
    int found = FALSE;
    int i;

    if (!mp_is_host() || !mp_cr_outdoors()) {
        return FALSE;
    }
    for (i = 0; i < MP_CR_MAX; i++) {
        mp_cr_t* c = &s_cr.cr[i];

        if (c->id != 0 && c->kind == kind && c->bx == bx && c->bz == bz && c->field == (u16)mFI_GetFieldId()) {
            found = TRUE;
            if (!s_cr.remote && (mp_cr_remakes(c) || c->type == aINS_INSECT_TYPE_ANT)) {
                mp_cr_materialize(play, c);
            }
        }
    }
    // a visitor is rolling it: what it makes comes here as it's listed, and the step rolls after if not
    if (!found && !s_cr.remote && mp_cr_leased(bx, bz, (u16)mFI_GetFieldId(), 0)) {
        s_cr.host_wait = TRUE;
        s_cr.host_wait_bx = (s8)bx;
        s_cr.host_wait_bz = (s8)bz;
        s_cr.host_wait_field = (u16)mFI_GetFieldId();
        return TRUE;
    }
    return found;
}

// host: that acre's lot of this kind is out already (a released bug and ants aside; one roll's many come together)
static int mp_cr_acre_taken(int kind, int type, int extra, int bx, int bz, u16 field, int from) {
    int i;

    if (kind == MP_CR_INSECT && (type == aINS_INSECT_TYPE_ANT || extra == aINS_INIT_RELEASE)) {
        return FALSE;
    }
    for (i = 0; i < MP_CR_MAX; i++) {
        const mp_cr_t* c = &s_cr.cr[i];

        if (c->id != 0 && c->kind == kind && c->bx == bx && c->bz == bz && c->field == field && mp_cr_remakes(c) &&
            !(c->lister == from && pc_mp_now_ms() - c->listed_ms < MP_CR_ROLL_MS)) {
            return TRUE;
        }
    }
    return FALSE;
}

static mp_cr_t* mp_cr_host_new(int kind, int type, const void* pos, int extra, int owner, int bx, int bz) {
    mp_cr_t* c = mp_cr_new(mp_cr_next_id());

    if (c == NULL) {
        return NULL;
    }
    c->kind = (u8)kind;
    c->type = (u8)type;
    memcpy(&c->tr.pos, pos, sizeof(xyz_t));
    memcpy(&c->spawn, pos, sizeof(xyz_t));
    c->bx = (s8)bx;
    c->bz = (s8)bz;
    c->extra = (s16)extra;
    c->field = (u16)mFI_GetFieldId();
    c->owner = (u8)owner;
    s_cr.owners_dirty = TRUE;
    return c;
}

// host: rolled for another player's step; only the others hear of it
void pc_mp_cr_remote_spawn(int kind, int type, const void* pos, int extra) {
    mp_cr_t* c;

    if (!mp_is_host() ||
        (c = mp_cr_host_new(kind, type, pos, extra, s_cr.remote_slot, s_cr.roll_bx, s_cr.roll_bz)) == NULL) {
        return;
    }
    // the ghost event's record of where its spirits are is the host's, and this one is out now
    if (kind == MP_CR_INSECT && type == aINS_INSECT_TYPE_SPIRIT) {
        aINS_mp_spirit_taken(c->bx, c->bz);
    }
    mp_cr_tell_spawn(mp_live_play(), c, s_cr.remote_slot, -1);
}

// one this game made itself goes on the list, run here: the host lists it at once, a visitor has the
// host list it
static void mp_cr_send_new(const mp_cr_t* c);

static mp_cr_t* mp_cr_own_new(int kind, int type, const void* pos, int extra, void* actor, int bx, int bz) {
    mp_cr_t* c;

    if (mp_is_host()) {
        if ((c = mp_cr_host_new(kind, type, pos, extra, 0, bx, bz)) != NULL) {
            c->actor = (ACTOR*)actor;
            c->made_here = TRUE;
            mp_cr_tell_spawn(mp_live_play(), c, -1, -1);
        }
        return c;
    }
    if (mp_lobby_host_conn() < 0 || (c = mp_cr_new(mp_cr_own_id())) == NULL) {
        return NULL;
    }
    c->kind = (u8)kind;
    c->type = (u8)type;
    memcpy(&c->spawn, pos, sizeof(xyz_t));
    c->tr.pos = c->spawn;
    c->bx = (s8)bx;
    c->bz = (s8)bz;
    c->extra = (s16)extra;
    c->field = (u16)mFI_GetFieldId();
    c->owner = (u8)mp_lobby_self_slot();
    c->actor = (ACTOR*)actor;
    c->made_here = TRUE;
    // (one let go from the pockets: the others can catch it once the passport no longer has it)
    if (extra == aINS_INIT_RELEASE && kind == MP_CR_INSECT) {
        c->unlisted = TRUE;
        c->pp_seq = mp_passport_write_async();
        return c;
    }
    mp_cr_send_new(c);
    return c;
}

// guest: the host lists one this game made
static void mp_cr_send_new(const mp_cr_t* c) {
    u8 msg[23];

    msg[0] = MP_M_CR_NEW;
    msg[1] = c->kind;
    msg[2] = c->type;
    mp_put16(msg + 3, c->id);
    msg[5] = (u8)c->bx;
    msg[6] = (u8)c->bz;
    mp_put16(msg + 7, (u16)c->extra);
    mp_put16(msg + 9, c->field);
    memcpy(msg + 11, &c->spawn, 12);
    mp_cr_send_host(msg, 23);
}

// the looping sound a creature's own logic keeps up this frame (sAdo_OngenPos), for its report to carry
static unsigned int s_cr_lv_key;
static unsigned char s_cr_lv_id;

void pc_mp_cr_level_sound(unsigned int key, unsigned char id) {
    s_cr_lv_key = key;
    s_cr_lv_id = id;
}

unsigned char pc_mp_cr_level_of(const void* actor) {
    unsigned char id = s_cr_lv_key == (unsigned int)actor ? s_cr_lv_id : 0;

    s_cr_lv_key = 0;
    return id;
}

// guest: bugs let go wait on the passport, then go on the list
static void mp_cr_list_released(void) {
    int i;

    for (i = 0; i < MP_CR_MAX; i++) {
        mp_cr_t* c = &s_cr.cr[i];
        int ok;

        if (c->id == 0 || !c->unlisted) {
            continue;
        }
        if (c->pp_seq == 0) {
            if ((s32)(pc_mp_now_ms() - c->pp_retry_ms) < 0 || (c->pp_seq = mp_passport_write_async()) == 0) {
                continue;
            }
        }
        if (!mp_passport_written(c->pp_seq, &ok)) {
            continue;
        }
        if (!ok) {
            c->pp_seq = 0;
            c->pp_retry_ms = pc_mp_now_ms() + 1000;
            continue;
        }
        c->unlisted = FALSE;
        c->owner_ms = pc_mp_now_ms();
        mp_cr_send_new(c);
    }
}

// a copy was just made here: one made for a known creature is bound to it; one rolled for this player's
// step (by the host, or by a visitor the host left it to) or let go from a pocket is new, run here
void pc_mp_cr_made(int kind, int type, const void* pos, int extra, void* actor) {
    mp_cr_t* c;
    int bx;
    int bz;

    mp_cr_orphan_take(actor); // (a new fish in that slot)
    if (s_cr.binding != 0) {
        c = mp_cr_by_id(s_cr.binding);
        if (c != NULL) {
            c->actor = (ACTOR*)actor;
            c->made_here = FALSE;
        }
        return;
    }
    if (!mp_cr_outdoors()) {
        return;
    }
    if (s_cr.rolling && ((mp_is_host() && !s_cr.remote) || (!mp_is_host() && s_cr.self_roll != 0))) {
        c = mp_cr_own_new(kind, type, pos, extra, actor, s_cr.roll_bx, s_cr.roll_bz);
        if (c != NULL && mp_is_host() && kind == MP_CR_INSECT && type == aINS_INSECT_TYPE_SPIRIT) {
            aINS_mp_spirit_taken(c->bx, c->bz);
        }
    } else if (kind == MP_CR_INSECT && type != aINS_INSECT_TYPE_ANT && extra == aINS_INIT_RELEASE && actor != NULL &&
               mp_town_block(pos, &bx, &bz)) {
        mp_cr_own_new(kind, type, pos, extra, actor, bx, bz);
    }
}

// a creature of this game's own the list missed (out before anyone came, let go from a net, a full
// list) joins it
void pc_mp_cr_adopt(int kind, int type, const void* pos, int extra, void* actor) {
    const xyz_t* home = &((ACTOR*)actor)->home.position;
    mp_cr_t* c;
    int bx;
    int bz;

    // (a fish caught or gone elsewhere while at this player's float: gone now it's off it)
    if (kind == MP_CR_FISH && mp_cr_orphan_take(actor)) {
        aGYO_mp_bolt(actor, FALSE);
        ((aGYO_CTRL_ACTOR*)actor)->gyo_flags |= 0x20;
        return;
    }
    // one this player's net closed on waits for the host's word: it may be another's
    if (!mp_cr_outdoors() || mp_cr_by_actor(actor) != NULL || (s_cr.net_wait && (ACTOR*)actor == s_cr.net_actor) ||
        !mp_town_block(home, &bx, &bz) || (c = mp_cr_own_new(kind, type, home, extra, actor, bx, bz)) == NULL) {
        return;
    }
    memcpy(&c->tr.pos, pos, sizeof(xyz_t));
}

int mp_cr_adopting(void) {
    return mp_cr_outdoors() && s_cr.frame % MP_CR_ADOPT_EVERY == 0;
}

// the ants listed for an acre came out here; a candy has one lot
void pc_mp_cr_ant_born(void* actor, int bx, int bz) {
    int i;

    for (i = 0; i < MP_CR_MAX; i++) {
        mp_cr_t* c = &s_cr.cr[i];

        if (c->id != 0 && c->kind == MP_CR_INSECT && c->type == aINS_INSECT_TYPE_ANT && c->actor == NULL &&
            c->bx == bx && c->bz == bz && c->field == (u16)mFI_GetFieldId()) {
            c->actor = (ACTOR*)actor;
            return;
        }
    }
}

// a copy being made for a known creature belongs to that creature's acre, not this player's
int mp_cr_binding_block(int* bx, int* bz) {
    mp_cr_t* c = mp_cr_by_id(s_cr.binding);

    if (c == NULL) {
        return FALSE;
    }
    *bx = c->bx;
    *bz = c->bz;
    return TRUE;
}

// caught, landed or gone for good: every screen drops it
static void mp_cr_gone_as(void* actor, int caught) {
    mp_cr_t* c = mp_cr_by_actor(actor);

    mp_cr_orphan_take(actor);
    if (c != NULL) {
        mp_cr_send_gone(c->id, caught);
        memset(c, 0, sizeof(*c)); // this game's copy is already on its way out
    }
}

void pc_mp_cr_gone(void* actor) {
    mp_cr_gone_as(actor, TRUE);
}

void pc_mp_cr_bolted(void* actor, int start) {
    mp_cr_gone_as(actor, start ? MP_CR_BOLT_START : MP_CR_BOLT);
}

void pc_mp_cr_ant_gone(void* actor) {
    mp_cr_gone_as(actor, FALSE);
}

// the net

// host: the first net on one keeps it; everyone else's copy goes, and a later net comes up empty
// host: a spirit netted by someone who hasn't met Wisp, or while two or more hunt, moves on to another acre, so no
// hunter's five are split
static void mp_cr_spirit_share(const mp_cr_t* c, int slot) {
    if (c->kind == MP_CR_INSECT && c->type == aINS_INSECT_TYPE_SPIRIT &&
        (mp_rights_wisp_hunters() >= 2 || !mp_rights_wisp_met(slot))) {
        aINS_mp_spirit_away(c->bx, c->bz);
    }
}

static int mp_cr_host_grant(GAME_PLAY* play, u16 id, int slot) {
    mp_cr_t* c = mp_cr_by_id(id);
    u8 msg[4];
    int k;

    for (k = 0; k < MP_CR_GRANTS; k++) {
        if (s_cr.grant[k].id == id && s_cr.grant[k].slot != slot &&
            pc_mp_now_ms() - s_cr.grant[k].ms < MP_CR_GRANT_MS) {
            return FALSE;
        }
    }
    s_cr.grant[s_cr.grant_at].id = id;
    s_cr.grant[s_cr.grant_at].slot = (u8)slot;
    s_cr.grant[s_cr.grant_at].ms = pc_mp_now_ms();
    s_cr.grant_at = (s_cr.grant_at + 1) % MP_CR_GRANTS;
    if (c != NULL) {
        mp_cr_spirit_share(c, slot);
        msg[0] = MP_M_CR_GONE;
        mp_put16(msg + 1, id);
        msg[3] = TRUE;
        mp_cr_send_guests(msg, 4, slot);
        mp_cr_remove(play, c, TRUE);
    }
    return TRUE;
}

// this player's net closed on one another game may hold too: the host has the say (a visitor's catch
// waits on its word, well inside the pull before the report)
void pc_mp_cr_netted(void* actor) {
    mp_cr_t* c = mp_cr_by_actor(actor);
    u8 msg[3];

    s_cr.net_wait = FALSE;
    s_cr.net_lost = FALSE;
    s_cr.net_actor = (ACTOR*)actor;
    s_cr.net_ant = actor != NULL && ((ACTOR*)actor)->id == mAc_PROFILE_ANT;
    if (c == NULL) {
        return;
    }
    if (mp_is_host()) {
        // (a visitor's net got there first: this one comes up empty, after the actors)
        if (!mp_cr_host_grant(mp_live_play(), c->id, 0)) {
            s_cr.net_id = c->id;
            s_cr.net_wait = TRUE;
            s_cr.net_lost = TRUE;
        }
        return;
    }
    s_cr.net_id = c->id;
    s_cr.net_ms = pc_mp_now_ms();
    s_cr.net_wait = TRUE;
    msg[0] = MP_M_CR_NET;
    mp_put16(msg + 1, c->id);
    mp_cr_send_host(msg, 3);
}

int pc_mp_cr_net_waiting(void) {
    return s_cr.net_wait;
}

// guest: with no host left the net keeps what it has; a host that never answers may have given it to another net,
// so this one comes up empty (a quiet host is waited on meanwhile)
static void mp_cr_net_timeout(GAME_PLAY* play) {
    mp_cr_t* c;
    int conn = mp_lobby_host_conn();

    if (!s_cr.net_wait || (conn >= 0 && pc_mp_now_ms() - s_cr.net_ms < MP_CR_NET_MAX_MS)) {
        return;
    }
    s_cr.net_wait = FALSE;
    if (conn < 0) {
        if ((c = mp_cr_by_id(s_cr.net_id)) != NULL) {
            memset(c, 0, sizeof(*c));
        }
    } else if (s_cr.net_actor != NULL && play != NULL && Save_Get(scene_no) == SCENE_FG) {
        aINS_mp_lose_catch(s_cr.net_actor, s_cr.net_ant);
    }
    s_cr.net_actor = NULL;
}

// a copy ended on its own: flown off or faded where this game runs it, which ends it everywhere, or
// refused by its own setup where it is on this screen, which ends only the copy (tried again later)
int pc_mp_cr_ended(void* actor) {
    mp_cr_t* c = mp_cr_by_actor(actor);

    if (c == NULL) {
        return FALSE;
    }
    if (c->owner == mp_lobby_self_slot() || c->owner == MP_CR_NOBODY) {
        mp_cr_gone_as(actor, FALSE);
        return FALSE;
    }
    c->actor = NULL;
    c->retry_ms = (pc_mp_now_ms() + MP_CR_RETRY_MS) | 1;
    return TRUE;
}

// a copy's own setup refused it where it came out on this screen: one this game rolled never really
// came out; a copy of another game's is tried again later, and the host doesn't give it this game to run
int pc_mp_cr_refused(void* actor) {
    mp_cr_t* c = mp_cr_by_actor(actor);
    u8 msg[3];

    if (c == NULL) {
        return FALSE;
    }
    if (c->made_here) {
        mp_cr_gone_as(actor, FALSE);
        return FALSE;
    }
    c->actor = NULL;
    c->retry_ms = (pc_mp_now_ms() + MP_CR_RETRY_MS) | 1;
    if (mp_is_host()) {
        c->nocopy |= 1;
        c->nocopy_ms = pc_mp_now_ms();
    } else {
        msg[0] = MP_M_CR_NOCOPY;
        mp_put16(msg + 1, c->id);
        mp_cr_send_host(msg, 3);
    }
    return TRUE;
}

// this game let its copy go (out of reach); the creature stays for the others
void pc_mp_cr_unbind(void* actor) {
    mp_cr_t* c = mp_cr_by_actor(actor);

    if (c != NULL) {
        c->actor = NULL;
    }
}

// the play game ended: its copies went with it (the list stays for the way back)
void mp_cr_play_gone(void) {
    int i;

    s_cr.net_wait = FALSE;
    s_cr.net_lost = FALSE;
    s_cr.net_actor = NULL;
    for (i = 0; i < MP_CR_MAX; i++) {
        s_cr.cr[i].actor = NULL;
    }
    s_cr.nact = 0;
}

// a scene's bugs or fish go all at once
void pc_mp_cr_unbind_kind(int kind) {
    int i;

    if (kind == MP_CR_INSECT) {
        s_cr.net_wait = FALSE;
        s_cr.net_lost = FALSE;
        s_cr.net_actor = NULL;
    }

    for (i = 0; i < MP_CR_MAX; i++) {
        if (s_cr.cr[i].kind == kind) {
            s_cr.cr[i].actor = NULL;
        }
    }
}

// running a creature

// another game runs it, and it isn't in this player's net
int mp_cr_puppet(void* actor, void* pos, void* blob, unsigned int* age_ms) {
    mp_cr_t* c = mp_cr_by_actor(actor);

    if (c == NULL || c->owner == mp_lobby_self_slot() || c->owner == MP_CR_NOBODY || !c->tr.have ||
        pc_mp_now_ms() - c->tr.ms > MP_CR_STALE_MS || !mp_cr_outdoors() ||
        (void*)mPlib_Get_item_net_catch_label() == actor) {
        return FALSE;
    }
    mp_cr_track_guess(&c->tr, (xyz_t*)pos);
    memcpy(blob, c->tr.blob, MP_CR_BLOB);
    *age_ms = pc_mp_now_ms() - c->tr.ms;
    return TRUE;
}

int mp_cr_mine(void* actor) {
    mp_cr_t* c = mp_cr_by_actor(actor);

    return c != NULL && c->owner == mp_lobby_self_slot() && mp_cr_outdoors();
}

void pc_mp_cr_report(void* actor, const void* pos, const void* blob, unsigned int key) {
    mp_cr_t* c = mp_cr_by_actor(actor);

    if (c != NULL && c->owner == mp_lobby_self_slot()) {
        mp_cr_track_take(&c->tr, pos, blob, key);
    }
}

// the local player needs these answering here (a float settling near a fish, or one on its line); a
// claim another player's float already holds waits its turn
void pc_mp_cr_claim_near(int kind, const void* pos, float radius) {
    const xyz_t* p = (const xyz_t*)pos;
    int self = mp_lobby_self_slot();
    int i;

    if (!mp_cr_outdoors()) {
        return;
    }
    for (i = 0; i < MP_CR_MAX; i++) {
        mp_cr_t* c = &s_cr.cr[i];
        f32 dx;
        f32 dz;
        u8 msg[4];

        if (c->id == 0 || c->kind != kind || c->actor == NULL ||
            (c->ask_ms != 0 && pc_mp_now_ms() - c->ask_ms < MP_CR_RECLAIM_MS)) {
            continue;
        }
        dx = c->actor->world.position.x - p->x;
        dz = c->actor->world.position.z - p->z;
        if (dx * dx + dz * dz > radius * radius) {
            continue;
        }
        c->ask_ms = pc_mp_now_ms() | 1;
        if (!mp_is_host()) {
            msg[0] = MP_M_CR_CLAIM;
            msg[1] = (u8)self;
            mp_put16(msg + 2, c->id);
            mp_cr_send_host(msg, 4);
        } else if (c->owner == self) {
            c->hold_ms = pc_mp_now_ms();
        } else if (c->owner == MP_CR_NOBODY || pc_mp_now_ms() - c->hold_ms >= MP_CR_HOLD_MS) {
            c->owner = (u8)self;
            c->owner_ms = pc_mp_now_ms();
            c->hold_ms = pc_mp_now_ms();
            c->tr.have = FALSE;
            s_cr.owners_dirty = TRUE;
        }
    }
}

// fish another game runs, this close to something that spooks fish here: they bolt on its screen
static void mp_cr_scare_within(const xyz_t* p, f32 radius, int with_y) {
    int i;

    if (!mp_cr_outdoors()) {
        return;
    }
    for (i = 0; i < MP_CR_MAX; i++) {
        mp_cr_t* c = &s_cr.cr[i];
        f32 dx;
        f32 dy;
        f32 dz;
        u8 msg[3];

        if (c->id == 0 || c->kind != MP_CR_FISH || c->actor == NULL || c->owner == mp_lobby_self_slot() ||
            c->owner == MP_CR_NOBODY || (c->scare_ms != 0 && pc_mp_now_ms() - c->scare_ms < MP_CR_SCARE_MS) ||
            (((aGYO_CTRL_ACTOR*)c->actor)->gyo_flags & (2 | 4 | 8))) {
            continue;
        }
        dx = c->actor->world.position.x - p->x;
        dy = with_y ? c->actor->world.position.y - p->y : 0.0f;
        dz = c->actor->world.position.z - p->z;
        if (dx * dx + dy * dy + dz * dz < radius * radius) {
            c->scare_ms = pc_mp_now_ms() | 1;
            msg[0] = MP_M_CR_SCARE;
            mp_put16(msg + 1, c->id);
            mp_cr_send_owner(c, msg, 3);
        }
    }
}

// a float landed right on top of them
void pc_mp_cr_scare_near(const void* pos, float radius) {
    mp_cr_scare_within((const xyz_t*)pos, radius, FALSE);
}

// a ball or snowball in the water, or a hooked fish thrashing
void pc_mp_cr_scare_at(const void* pos, float radius) {
    mp_cr_scare_within((const xyz_t*)pos, radius, TRUE);
}

// this player's tool hit something (axe, net, shovel): fish within reach of it bolt, as its own do
static void mp_cr_tool_scare(GAME_PLAY* play) {
    PLAYER_ACTOR* me = GET_PLAYER_ACTOR(play);
    xyz_t hit;

    if (me != NULL && (mPlib_Check_HitAxe(&hit) || mPlib_Check_StopNet(&hit) || mPlib_Check_HitScoop(&hit))) {
        mp_cr_scare_within(&me->actor_class.world.position, 150.0f, TRUE);
    }
}

// another player running near the water spooks the fish this game runs
int mp_cr_dash_near(const void* pos, float dist) {
    GAME_PLAY* play = mp_live_play();
    const xyz_t* p = (const xyz_t*)pos;
    int s;

    if (!mp_cr_outdoors()) {
        return FALSE;
    }
    for (s = 0; s < MP_MAX_PEERS; s++) {
        unsigned int age;
        const mp_pstate_t* st;
        xyz_t at;
        u16 field;

        if (s == mp_lobby_self_slot() || !mp_cr_player_at(play, s, &at, &field, FALSE) ||
            field != (u16)mFI_GetFieldId() || (st = mp_player_state(s, &age)) == NULL ||
            st->main_index != mPlayer_INDEX_DASH) {
            continue;
        }
        if ((at.x - p->x) * (at.x - p->x) + (at.z - p->z) * (at.z - p->z) < dist * dist) {
            return TRUE;
        }
    }
    return FALSE;
}

// a tree shaken, a hole dug or a rock hit here: the bugs another game runs there answer as well
void pc_mp_cr_act(int action, int ut_x, int ut_z) {
    u8 msg[8];

    if (action == 0 || !mp_cr_outdoors()) {
        return;
    }
    msg[0] = MP_M_CR_ACT;
    msg[1] = (u8)action;
    mp_put16(msg + 2, (u16)ut_x);
    mp_put16(msg + 4, (u16)ut_z);
    mp_put16(msg + 6, (u16)mFI_GetFieldId());
    if (mp_is_host()) {
        mp_cr_send_guests(msg, 8, -1);
    } else {
        mp_cr_send_host(msg, 8);
    }
}

// the insect control, before its bugs move: another player's act to play to them, as its own
int mp_cr_take_act(int* action, int* ut_x, int* ut_z) {
    int k;

    while (s_cr.nact > 0 && pc_mp_now_ms() - s_cr.act[0].ms > MP_CR_ACT_MS) {
        for (k = 1; k < s_cr.nact; k++) {
            s_cr.act[k - 1] = s_cr.act[k];
        }
        s_cr.nact--;
    }
    if (s_cr.nact <= 0) {
        return FALSE;
    }
    *action = s_cr.act[0].action;
    *ut_x = s_cr.act[0].ut_x;
    *ut_z = s_cr.act[0].ut_z;
    for (k = 1; k < s_cr.nact; k++) {
        s_cr.act[k - 1] = s_cr.act[k];
    }
    s_cr.nact--;
    return TRUE;
}

// the present balloon: one game flies it for the town (the host out there, else a visitor out there),
// the others follow it

int mp_cr_balloon_runner_here(void) {
    return mp_cr_shared() && s_cr.bl_runner == mp_lobby_self_slot();
}

int mp_cr_balloon_elsewhere(void) {
    return mp_cr_shared() && s_cr.bl_runner != mp_lobby_self_slot();
}

// visitor: the town's balloon flies on this screen, so its game flies it on behind its menus
int mp_cr_balloon_flying_here(void) {
    return !mp_is_host() && mp_cr_balloon_runner_here() && s_cr.bl.have && pc_mp_now_ms() - s_cr.bl.ms < 500;
}

// a balloon is out in town: its runner's reports come, or came lately enough that its copies still wait on it
static int mp_cr_bl_out(void) {
    return s_cr.bl.have && pc_mp_now_ms() - s_cr.bl.ms <= MP_CR_BL_HOLD_MS;
}

void pc_mp_cr_balloon_report(const void* pos, const void* blob, unsigned int key) {
    if (mp_cr_balloon_runner_here() && mp_cr_outdoors()) {
        mp_cr_track_take(&s_cr.bl, pos, blob, key);
        s_cr.bl_field = (u16)mFI_GetFieldId();
    }
}

// a copy follows the runner's newest report; one gone quiet leaves it where it was last seen
int mp_cr_balloon(void* pos, void* blob, unsigned int* age_ms) {
    if (!mp_cr_balloon_elsewhere() || !mp_cr_bl_out() || !mp_cr_outdoors() || s_cr.bl_field != (u16)mFI_GetFieldId()) {
        return FALSE;
    }
    mp_cr_track_guess(&s_cr.bl, (xyz_t*)pos);
    memcpy(blob, s_cr.bl.blob, MP_CR_BLOB);
    *age_ms = pc_mp_now_ms() - s_cr.bl.ms;
    return TRUE;
}

int mp_cr_balloon_last(void* pos, void* blob) {
    if (!mp_cr_bl_out() || s_cr.bl_field != (u16)mFI_GetFieldId()) {
        return FALSE;
    }
    mp_cr_track_guess(&s_cr.bl, (xyz_t*)pos);
    memcpy(blob, s_cr.bl.blob, MP_CR_BLOB);
    return TRUE;
}

// a hard shake of the tree it's caught in frees it on the runner's screen
void pc_mp_cr_balloon_pop(void) {
    u8 msg[1];

    msg[0] = MP_M_CR_POP;
    if (mp_is_host()) {
        mp_cr_send_guest(s_cr.bl_runner, msg, 1);
    } else {
        mp_cr_send_host(msg, 1);
    }
}

static void mp_cr_bl_gone(void) {
    s_cr.bl.have = FALSE;
    s_cr.bl_gone_ms = pc_mp_now_ms() | 1;
}

// the runner's balloon flew off for good: every screen's copy goes with it (a copy that only lost sight of its
// runner waits instead)
void pc_mp_cr_balloon_gone(void) {
    u8 msg[2];

    if (!mp_cr_balloon_runner_here()) {
        return;
    }
    mp_cr_bl_gone();
    msg[0] = MP_M_CR_POP;
    msg[1] = MP_CR_POP_GONE;
    if (mp_is_host()) {
        mp_cr_send_guests(msg, 2, -1);
    } else {
        mp_cr_send_host(msg, 2);
    }
}

int mp_cr_balloon_popped(void) {
    int pop = s_cr.bl_pop;

    s_cr.bl_pop = FALSE;
    return pop;
}

// the balloon's runner: another player stands this close, so it counts as seen (it only snags trees in view)
int mp_cr_near_player(const void* pos, float dist) {
    GAME_PLAY* play = mp_live_play();
    const xyz_t* p = (const xyz_t*)pos;
    int s;

    if (!mp_cr_balloon_runner_here() || !mp_cr_outdoors() || play == NULL) {
        return FALSE;
    }
    for (s = 0; s < MP_MAX_PEERS; s++) {
        xyz_t at;
        u16 field;

        if (s != mp_lobby_self_slot() && mp_cr_player_at(play, s, &at, &field, FALSE) &&
            field == (u16)mFI_GetFieldId() &&
            (at.x - p->x) * (at.x - p->x) + (at.z - p->z) * (at.z - p->z) < dist * dist) {
            return TRUE;
        }
    }
    return FALSE;
}

// the balloon on this screen: the runner's own flies itself; elsewhere one follows the runner's reports, and
// waits where it was last seen while its runner is quiet (another screen may take it on from there)
static void mp_cr_balloon_mirror(GAME_PLAY* play) {
    ACTOR* fuusen;
    int out;

    if (!mp_cr_balloon_elsewhere() || Save_Get(scene_no) != SCENE_FG ||
        mFI_GET_TYPE(mFI_GetFieldId()) != mFI_FIELDTYPE_FG) {
        return;
    }
    fuusen = Actor_info_name_search(&play->actor_info, mAc_PROFILE_FUUSEN, ACTOR_PART_CONTROL);
    out = mp_cr_bl_out() && s_cr.bl_field == (u16)mFI_GetFieldId();
    if (out && fuusen == NULL) {
        Actor_info_make_actor(&play->actor_info, (GAME*)play, mAc_PROFILE_FUUSEN, 0.0f, 0.0f, 0.0f, 0, 0, 0, -1, -1,
                              -1, EMPTY_NO, 0, -1, -1);
    } else if (!out && fuusen != NULL && s_cr.bl_runner != MP_CR_NOBODY) {
        // (with nobody out to fly it, it waits where it is)
        Actor_delete(fuusen);
    }
}

// a player out in the town's own field: running, or (watching) just paused a while in a menu
static int mp_cr_player_in_town(GAME_PLAY* play, int s, int watching) {
    xyz_t at;
    u16 field;

    return mp_cr_player_at(play, s, &at, &field, watching) && mFI_GET_TYPE(field) == mFI_FIELDTYPE_FG;
}

// caught in a tree, the balloon is flown by a player with that spot loaded (whoever flies it now if it has,
// else the nearest who has): its shake and its present land where that game has the town
static u8 mp_cr_bl_snag_runner(GAME_PLAY* play) {
    u8 best = MP_CR_NOBODY;
    f32 best_d = 0.0f;
    int bbx;
    int bbz;
    int s;

    if (!s_cr.bl.have || pc_mp_now_ms() - s_cr.bl.ms > MP_CR_BL_STALE_MS ||
        s_cr.bl.blob[0] != aFSN_ACTION_WOOD_STOP || !mp_town_block(&s_cr.bl.pos, &bbx, &bbz)) {
        return MP_CR_NOBODY;
    }
    for (s = 0; s < MP_MAX_PEERS; s++) {
        xyz_t at;
        u16 field;
        int pbx;
        int pbz;
        f32 d;

        if (!mp_cr_player_at(play, s, &at, &field, FALSE) || field != s_cr.bl_field ||
            !mp_town_block(&at, &pbx, &pbz) || pbx - bbx > 1 || bbx - pbx > 1 || pbz - bbz > 1 || bbz - pbz > 1) {
            continue;
        }
        if (s == s_cr.bl_runner) {
            return (u8)s;
        }
        d = (at.x - s_cr.bl.pos.x) * (at.x - s_cr.bl.pos.x) + (at.z - s_cr.bl.pos.z) * (at.z - s_cr.bl.pos.z);
        if (best == MP_CR_NOBODY || d < best_d) {
            best = (u8)s;
            best_d = d;
        }
    }
    return best;
}

// host: who flies the balloon; coming back out, the host takes it over once its screen follows it
static void mp_cr_host_balloon_runner(GAME_PLAY* play) {
    u8 runner = MP_CR_NOBODY;
    u8 snag;
    int s;

    if (Save_Get(scene_no) == SCENE_FG && mFI_GET_TYPE(mFI_GetFieldId()) == mFI_FIELDTYPE_FG) {
        if (s_cr.bl_out_ms == 0) {
            s_cr.bl_out_ms = pc_mp_now_ms() | 1;
        }
        u32 out = pc_mp_now_ms() - s_cr.bl_out_ms;
        int flying = mp_cr_bl_out();

        // a balloon in the air is taken over only once this screen follows it (a visitor flying it flies it
        // on behind its menus)
        if (s_cr.bl_runner == 0 || s_cr.bl_runner == MP_CR_NOBODY ||
            (out >= MP_CR_BL_HOME_MS &&
             (!flying || Actor_info_name_search(&play->actor_info, mAc_PROFILE_FUUSEN, ACTOR_PART_CONTROL) != NULL))) {
            runner = 0;
        }
    } else {
        s_cr.bl_out_ms = 0;
    }
    if ((snag = mp_cr_bl_snag_runner(play)) != MP_CR_NOBODY) {
        runner = snag;
    }
    if (runner == MP_CR_NOBODY) {
        // the one flying it keeps it through a menu (it flies on there), not while its game has gone quiet;
        // else the first visitor out in town, which takes it on from where its copy last saw it
        if (s_cr.bl_runner != 0 && s_cr.bl_runner != MP_CR_NOBODY &&
            mp_cr_player_in_town(play, s_cr.bl_runner, TRUE) &&
            !(mp_cr_bl_out() && pc_mp_now_ms() - s_cr.bl.ms > MP_CR_BL_QUIET_MS)) {
            runner = s_cr.bl_runner;
        } else {
            for (s = 1; s < MP_MAX_PEERS && runner == MP_CR_NOBODY; s++) {
                if (mp_cr_player_in_town(play, s, FALSE)) {
                    runner = (u8)s;
                }
            }
        }
    }
    if (runner != s_cr.bl_runner) {
        s_cr.bl_runner = runner;
        s_cr.owners_dirty = TRUE;
    }
}

// the balloon flew on behind this game's menu; its reports count the frames
void pc_mp_cr_menu_frame(void) {
    s_cr.frame++;
    s_cr.menu_ran = TRUE;
}

// host passes

// the nearest running game in each creature's acre runs it; one nobody is near any more went with them
static void mp_cr_host_owners(GAME_PLAY* play) {
    u8 msg[3 + MP_CR_MAX * 3];
    int n = 0;
    int i;

    for (i = 0; i < MP_CR_MAX; i++) {
        mp_cr_t* c = &s_cr.cr[i];
        u8 best = MP_CR_NOBODY;
        f32 best_d = 0.0f;
        int watched = FALSE;
        int quiet;
        int s;

        if (c->id == 0) {
            continue;
        }
        if (c->owner == 0 && c->actor != NULL) {
            c->tr.pos = c->actor->world.position;
        }
        // an owner whose copy never came out gives it up
        quiet = c->owner != 0 && c->owner != MP_CR_NOBODY && !c->tr.have &&
                pc_mp_now_ms() - c->owner_ms > MP_CR_QUIET_MS;
        for (s = 0; s < MP_MAX_PEERS; s++) {
            xyz_t at;
            u16 field;
            int pbx;
            int pbz;
            f32 d;

            if (!mp_cr_player_near_acre(play, s, c->bx, c->bz, c->field)) {
                continue;
            }
            watched = TRUE;
            // only a running game standing in its acre has made it
            if (!mp_cr_player_at(play, s, &at, &field, FALSE) || field != c->field ||
                !mp_town_block(&at, &pbx, &pbz) || pbx != c->bx || pbz != c->bz || (s == 0 && c->actor == NULL) ||
                (quiet && s == c->owner) ||
                (((c->nocopy >> s) & 1) && pc_mp_now_ms() - c->nocopy_ms < MP_CR_RETRY_MS + 1000)) {
                continue;
            }
            d = (at.x - c->tr.pos.x) * (at.x - c->tr.pos.x) + (at.z - c->tr.pos.z) * (at.z - c->tr.pos.z);
            if (d > MP_CR_REACH * MP_CR_REACH) {
                continue;
            }
            if (s == c->owner) {
                d /= MP_CR_KEEP;
            }
            if (best == MP_CR_NOBODY || d < best_d) {
                best = (u8)s;
                best_d = d;
            }
        }
        if (!watched) {
            // everyone has walked away: it went with them, as it would for one player; a spirit moves on
            if (c->kind == MP_CR_INSECT && c->type == aINS_INSECT_TYPE_SPIRIT && c->actor == NULL) {
                aINS_mp_spirit_away(c->bx, c->bz);
            }
            mp_cr_send_gone(c->id, FALSE);
            mp_cr_remove(play, c, FALSE);
            continue;
        }
        // ants stay put, so each game runs its own copy
        if (c->kind == MP_CR_INSECT && c->type == aINS_INSECT_TYPE_ANT) {
            continue;
        }
        // a fish near someone's float stays with them, and a new owner still stepping in keeps it
        if (c->owner != MP_CR_NOBODY && (pc_mp_now_ms() - c->hold_ms < MP_CR_HOLD_MS ||
                                          (!quiet && pc_mp_now_ms() - c->owner_ms < MP_CR_SETTLE_MS))) {
            continue;
        }
        if (best != c->owner) {
            c->owner = best;
            c->owner_ms = pc_mp_now_ms();
            c->hold_ms = 0;
            c->tr.have = FALSE;
            s_cr.owners_dirty = TRUE;
        }
    }
    mp_cr_host_balloon_runner(play);
    if (!s_cr.owners_dirty && pc_mp_now_ms() - s_cr.owners_ms < MP_CR_OWNER_MS) {
        return;
    }
    s_cr.owners_dirty = FALSE;
    s_cr.owners_ms = pc_mp_now_ms();
    // the whole list: a visitor drops whatever isn't on it; the balloon's runner follows it
    msg[0] = MP_M_CR_OWNERS;
    for (i = 0; i < MP_CR_MAX; i++) {
        if (s_cr.cr[i].id != 0) {
            mp_put16(msg + 2 + n * 3, s_cr.cr[i].id);
            msg[2 + n * 3 + 2] = s_cr.cr[i].owner;
            n++;
        }
    }
    msg[1] = (u8)n;
    msg[2 + n * 3] = s_cr.bl_runner;
    mp_cr_send_guests(msg, 3 + n * 3, -1);
}

// host: another player's step or look; the acre's creatures go to it, and a step rolls the kinds it
// has none of here, or has the player roll them itself when this game can't. A step into an acre another
// visitor is rolling waits for that (FALSE: ask again)
static int mp_cr_host_answer(GAME_PLAY* play, int slot, int bx, int bz, u16 field, int roll) {
    int have = 0;
    int i;

    if (roll && mp_cr_leased(bx, bz, field, slot)) {
        return FALSE;
    }
    for (i = 0; i < MP_CR_MAX; i++) {
        mp_cr_t* c = &s_cr.cr[i];

        if (c->id != 0 && c->bx == bx && c->bz == bz && c->field == field) {
            u8 msg[24];

            have |= 1 << c->kind;
            mp_cr_send_guest(slot, msg, mp_cr_spawn_msg(msg, c));
        }
    }
    if (!roll || have == ((1 << MP_CR_INSECT) | (1 << MP_CR_FISH))) {
        return TRUE;
    }
    if (Save_Get(scene_no) == SCENE_FG && field == (u16)mFI_GetFieldId()) {
        int rolled;

        s_cr.remote = TRUE;
        s_cr.remote_slot = slot;
        rolled = aSetMgr_mp_roll(play, bx, bz);
        s_cr.remote = FALSE;
        s_cr.rolling = FALSE;
        if (rolled) {
            return TRUE;
        }
    }
    s_cr.lease[slot].bx = (s8)bx;
    s_cr.lease[slot].bz = (s8)bz;
    s_cr.lease[slot].field = field;
    s_cr.lease[slot].ms = pc_mp_now_ms() | 1;
    {
        u8 msg[4];

        msg[0] = MP_M_CR_SELF;
        msg[1] = (u8)bx;
        msg[2] = (u8)bz;
        msg[3] = (u8)(~have & ((1 << MP_CR_INSECT) | (1 << MP_CR_FISH)));
        mp_cr_send_guest(slot, msg, 4);
    }
    return TRUE;
}

static void mp_cr_host_requests(GAME_PLAY* play) {
    int g;

    for (g = 1; g < MP_MAX_PEERS; g++) {
        if (s_cr.req[g] &&
            mp_cr_host_answer(play, g, s_cr.req_bx[g], s_cr.req_bz[g], s_cr.req_field[g], s_cr.req_roll[g])) {
            s_cr.req[g] = FALSE;
        }
    }
}

// host: its own step waited for a visitor rolling that acre; if it's still there, it rolls what's missing
static void mp_cr_host_wait_step(GAME_PLAY* play) {
    PLAYER_ACTOR* me = GET_PLAYER_ACTOR(play);
    int bx;
    int bz;

    if (!s_cr.host_wait || mp_cr_leased(s_cr.host_wait_bx, s_cr.host_wait_bz, s_cr.host_wait_field, 0)) {
        return;
    }
    s_cr.host_wait = FALSE;
    if (Save_Get(scene_no) == SCENE_FG && me != NULL && s_cr.host_wait_field == (u16)mFI_GetFieldId() &&
        mp_town_block(&me->actor_class.world.position, &bx, &bz) && bx == s_cr.host_wait_bx &&
        bz == s_cr.host_wait_bz) {
        aSetMgr_mp_roll(play, bx, bz);
        s_cr.rolling = FALSE;
    }
}

// states: each runner's creatures that moved off their last report, the host's balloon; the host
// passes the others' on

static int mp_cr_state_msg(u8* msg, int to_slot, u16 field) {
    int self = mp_lobby_self_slot();
    u8 bit = (u8)(1 << to_slot);
    int at = MP_CR_HDR;
    int n = 0;
    int i;

    msg[0] = MP_S_CRITTERS;
    mp_put16(msg + 1, field);
    msg[3] = 0;
    // the balloon: the runner's own to the host, and on from the host to everyone else
    if ((mp_is_host() || mp_cr_balloon_runner_here()) && to_slot != s_cr.bl_runner && s_cr.bl.have &&
        (s_cr.bl.due & bit) && field == s_cr.bl_field && pc_mp_now_ms() - s_cr.bl.ms < 200) {
        msg[3] |= 1;
        at += mp_cr_track_put(msg + at, &s_cr.bl, MP_CR_BLOB);
        s_cr.bl.due &= (u8)~bit;
    }
    at++; // the count
    for (i = 0; i < MP_CR_MAX; i++) {
        mp_cr_t* c = &s_cr.cr[i];

        if (c->id == 0 || !c->tr.have || !(c->tr.due & bit) || c->field != field || c->owner == to_slot ||
            (c->owner != self && !mp_is_host()) || pc_mp_now_ms() - c->tr.ms > MP_CR_STALE_MS) {
            continue;
        }
        if (at + 2 + 13 + MP_CR_BLOB > MP_STATE_MAX) {
            break;
        }
        mp_put16(msg + at, c->id);
        at += 2 + mp_cr_track_put(msg + at + 2, &c->tr, MP_CR_BLOB);
        c->tr.due &= (u8)~bit;
        n++;
    }
    msg[MP_CR_HDR + ((msg[3] & 1) ? 13 + MP_CR_BLOB : 0)] = (u8)n;
    return (n > 0 || (msg[3] & 1)) ? at : 0;
}

// host: each visitor outdoors hears about the field it stands in, wherever the host is
static void mp_cr_host_send_states(void) {
    u8 msg[MP_STATE_MAX];
    int g;

    for (g = 1; g < MP_MAX_PEERS; g++) {
        unsigned int age;
        const mp_pstate_t* st = mp_player_state(g, &age);
        int len;

        if (!mp_lobby_guest_arrived(g) || mp_lobby_guest_conn(g) < 0 || st == NULL || st->scene != SCENE_FG) {
            continue;
        }
        len = mp_cr_state_msg(msg, g, st->field);
        if (len > 0) {
            mp_lobby_send_state_to(mp_lobby_guest_conn(g), msg, len);
        }
    }
}

void mp_cr_on_state(int conn, const unsigned char* data, int len) {
    int from = mp_is_host() ? mp_lobby_guest_slot(conn) + 1 : 0;
    int self = mp_lobby_self_slot();
    u16 field;
    int at = MP_CR_HDR;
    int n;
    int k;

    if (len < MP_CR_HDR + 1 || data[0] != MP_S_CRITTERS || (mp_is_host() && from <= 0) || !mp_cr_shared()) {
        return;
    }
    field = (u16)mp_get16(data + 1);
    // a visitor only follows the field it stands in; the host keeps every field's for passing on
    if (!mp_is_host() && (field != (u16)mFI_GetFieldId() || Save_Get(scene_no) != SCENE_FG)) {
        return;
    }
    if (data[3] & 1) {
        mp_cr_track_t scratch;
        // the host takes it from its runner only; a follower from the host (not a late one of a flown-off balloon)
        int take = (mp_is_host() ? from == s_cr.bl_runner : !mp_cr_balloon_runner_here()) &&
                   (s_cr.bl_gone_ms == 0 || pc_mp_now_ms() - s_cr.bl_gone_ms > 1000);
        int used = mp_cr_track_get(data + at, len - at, take ? &s_cr.bl : &scratch);

        if (used < 0) {
            return;
        }
        if (take) {
            s_cr.bl_field = field;
            s_cr.bl.due = (u8)(0xFF & ~(1 << from));
        }
        at += used;
    }
    if (at >= len) {
        return;
    }
    n = data[at++];
    for (k = 0; k < n && at + 2 <= len; k++) {
        mp_cr_t* c = mp_cr_by_msg_id((u16)mp_get16(data + at));
        mp_cr_track_t scratch;
        int used;

        // the host only takes a creature's state from the game running it
        if (c == NULL || c->owner == self || c->field != field || (mp_is_host() && c->owner != from)) {
            used = mp_cr_track_get(data + at + 2, len - at - 2, &scratch);
        } else {
            used = mp_cr_track_get(data + at + 2, len - at - 2, &c->tr);
            // passed on to everyone but the runner
            c->tr.due = (u8)(0xFF & ~(1 << from));
        }
        if (used < 0) {
            return;
        }
        at += 2 + used;
    }
}

// guest: the host's say on a roll this game makes itself, now that the actors are done
static void mp_cr_guest_self_roll(GAME_PLAY* play) {
    PLAYER_ACTOR* me = GET_PLAYER_ACTOR(play);
    int bx;
    int bz;

    if (s_cr.self_req == 0) {
        return;
    }
    if (me != NULL && Save_Get(scene_no) == SCENE_FG &&
        mp_town_block(&me->actor_class.world.position, &bx, &bz) && bx == s_cr.self_bx && bz == s_cr.self_bz) {
        s_cr.self_roll = s_cr.self_req;
        aSetMgr_mp_roll(play, bx, bz);
        s_cr.self_roll = 0;
    }
    s_cr.self_req = 0;
    {
        u8 msg[3];

        msg[0] = MP_M_CR_SELFDONE;
        msg[1] = (u8)s_cr.self_bx;
        msg[2] = (u8)s_cr.self_bz;
        mp_cr_send_host(msg, 3);
    }
}

// guest: out of a building or off the train, the acre's creatures already out come from the host
static void mp_cr_guest_look(GAME_PLAY* play) {
    PLAYER_ACTOR* me = GET_PLAYER_ACTOR(play);
    int bx;
    int bz;
    u8 msg[5];

    if (Save_Get(scene_no) != SCENE_FG) {
        s_cr.was_out = FALSE;
        return;
    }
    if ((s_cr.was_out && !s_cr.look_due) || me == NULL ||
        !mp_town_block(&me->actor_class.world.position, &bx, &bz) ||
        (s_cr.look_ms != 0 && pc_mp_now_ms() - s_cr.look_ms < MP_CR_LOOK_MS)) {
        return;
    }
    s_cr.was_out = TRUE;
    s_cr.look_due = FALSE;
    s_cr.look_ms = pc_mp_now_ms() | 1;
    msg[0] = MP_M_CR_LOOK;
    msg[1] = (u8)bx;
    msg[2] = (u8)bz;
    mp_put16(msg + 3, (u16)mFI_GetFieldId());
    mp_cr_send_host(msg, 5);
}

// after the actors
void mp_cr_post(struct game_play_s* play_s) {
    GAME_PLAY* play = (GAME_PLAY*)play_s;
    int i;

    if (!mp_cr_shared()) {
        if (s_cr.frame != 0) {
            mp_cr_wipe();
        }
        return;
    }
    s_cr.frame++;
    if (Save_Get(scene_no) != SCENE_FG) {
        // indoors this game has no copies; the list stays for the way back out
        for (i = 0; i < MP_CR_MAX; i++) {
            s_cr.cr[i].actor = NULL;
        }
        s_cr.nact = 0;
    } else if (s_cr.frame % MP_CR_REMAKE_EVERY == 0) {
        // this player's acre shows what's still out in it (back through a door, say)
        PLAYER_ACTOR* me = GET_PLAYER_ACTOR(play);
        int bx;
        int bz;

        if (me != NULL && mp_town_block(&me->actor_class.world.position, &bx, &bz)) {
            for (i = 0; i < MP_CR_MAX; i++) {
                mp_cr_t* c = &s_cr.cr[i];

                if (c->id != 0 && c->actor == NULL && c->bx == bx && c->bz == bz && mp_cr_remakes(c)) {
                    mp_cr_materialize(play, c);
                }
            }
        }
    }
    if (Save_Get(scene_no) == SCENE_FG) {
        mp_cr_tool_scare(play);
    }
    if (mp_is_host()) {
        if (s_cr.net_lost) {
            s_cr.net_lost = FALSE;
            s_cr.net_wait = FALSE;
            if (s_cr.net_actor != NULL && Save_Get(scene_no) == SCENE_FG) {
                aINS_mp_lose_catch(s_cr.net_actor, s_cr.net_ant);
            }
            s_cr.net_actor = NULL;
        }
        mp_cr_host_requests(play);
        mp_cr_host_wait_step(play);
        if (s_cr.frame % MP_CR_OWNER_EVERY == 0) {
            mp_cr_host_owners(play);
        }
    } else {
        mp_cr_net_timeout(play);
        mp_cr_list_released();
        mp_cr_guest_self_roll(play);
        mp_cr_guest_look(play);
    }
    // a shake for a balloon that has since gone doesn't wait for the next one
    if (!mp_cr_balloon_runner_here() || !s_cr.bl.have || pc_mp_now_ms() - s_cr.bl.ms > 200) {
        s_cr.bl_pop = FALSE;
    }
    mp_cr_balloon_mirror(play);
    if (s_cr.frame % MP_CR_EVERY != 0) {
        return;
    }
    if (mp_is_host()) {
        mp_cr_host_send_states();
    } else if (Save_Get(scene_no) == SCENE_FG && mp_lobby_host_conn() >= 0) {
        u8 msg[MP_STATE_MAX];
        int len = mp_cr_state_msg(msg, 0, (u16)mFI_GetFieldId());

        if (len > 0) {
            mp_lobby_send_state_to(mp_lobby_host_conn(), msg, len);
        }
    }
}

// a menu has this game's world stopped: the host still hands out what it can't run, answers steps and
// passes the visitors' creatures on; a visitor flying the balloon on behind its menu still sends it
void mp_cr_still(struct game_play_s* play_s) {
    GAME_PLAY* play = (GAME_PLAY*)play_s;
    int ran = s_cr.menu_ran;

    s_cr.menu_ran = FALSE;
    if (!mp_cr_shared()) {
        return;
    }
    if (!mp_is_host()) {
        if (ran && ++s_cr.still_frame % MP_CR_EVERY == 0 && Save_Get(scene_no) == SCENE_FG &&
            mp_lobby_host_conn() >= 0) {
            u8 msg[MP_STATE_MAX];
            int len = mp_cr_state_msg(msg, 0, (u16)mFI_GetFieldId());

            if (len > 0) {
                mp_lobby_send_state_to(mp_lobby_host_conn(), msg, len);
            }
        }
        return;
    }
    s_cr.still_frame++;
    mp_cr_host_requests(play);
    if (s_cr.still_frame % MP_CR_OWNER_EVERY == 0) {
        mp_cr_host_owners(play);
    }
    if (s_cr.still_frame % MP_CR_EVERY == 0) {
        mp_cr_host_send_states();
    }
}

// messages

void mp_cr_on_rel(int conn, const unsigned char* data, int len) {
    GAME_PLAY* play = mp_live_play();
    int from = mp_is_host() ? mp_lobby_guest_slot(conn) + 1 : 0;
    mp_cr_t* c;

    if (len < 1 || (mp_is_host() && from <= 0) || (!mp_is_host() && conn != mp_lobby_host_conn()) ||
        !mp_cr_shared()) {
        return;
    }
    switch (data[0]) {
        case MP_M_CR_WADE:
        case MP_M_CR_LOOK:
            if (mp_is_host() && len >= 5 && from < MP_MAX_PEERS) {
                // a step asks for a roll; a look (out a door, off the train) only for what's there
                s_cr.req_roll[from] = data[0] == MP_M_CR_WADE || (s_cr.req[from] && s_cr.req_roll[from]);
                s_cr.req[from] = TRUE;
                s_cr.req_bx[from] = (s8)data[1];
                s_cr.req_bz[from] = (s8)data[2];
                s_cr.req_field[from] = (u16)mp_get16(data + 3);
            }
            break;
        case MP_M_CR_SELF:
            if (!mp_is_host() && len >= 4) {
                s_cr.self_req = data[3];
                s_cr.self_bx = (s8)data[1];
                s_cr.self_bz = (s8)data[2];
            }
            break;
        case MP_M_CR_SPAWN:
            if (!mp_is_host() && len >= 24) {
                u16 id = (u16)mp_get16(data + 4);
                int fresh = FALSE;

                if ((c = mp_cr_by_id(id)) == NULL) {
                    if ((c = mp_cr_new(id)) == NULL) {
                        break;
                    }
                    fresh = TRUE;
                }
                c->kind = data[1];
                c->type = data[2];
                c->bx = (s8)data[6];
                c->bz = (s8)data[7];
                c->extra = (s16)mp_get16(data + 8);
                c->field = (u16)mp_get16(data + 10);
                memcpy(&c->spawn, data + 12, 12);
                if (fresh) {
                    c->owner = data[3];
                    c->tr.pos = c->spawn;
                } else if (c->owner != data[3]) {
                    c->owner = data[3];
                    c->tr.have = FALSE;
                }
                if (play != NULL) {
                    mp_cr_materialize(play, c);
                }
            }
            break;
        case MP_M_CR_GONE:
            if (len >= 4 && (c = mp_cr_by_msg_id((u16)mp_get16(data + 1))) != NULL) {
                u16 id = c->id;
                int caught = data[3];

                // a spirit that got away on another screen turns up in another acre; the record is the host's
                if (mp_is_host() && !caught && c->kind == MP_CR_INSECT && c->type == aINS_INSECT_TYPE_SPIRIT &&
                    c->actor == NULL) {
                    aINS_mp_spirit_away(c->bx, c->bz);
                } else if (mp_is_host() && caught && from < MP_MAX_PEERS) {
                    mp_cr_spirit_share(c, from);
                }
                mp_cr_remove(play, c, caught);
                // everyone hears, the catcher too (a spawn may have crossed its catch)
                if (mp_is_host()) {
                    mp_cr_send_gone(id, caught);
                }
            }
            break;
        case MP_M_CR_NOCOPY:
            if (mp_is_host() && len >= 3 && from < MP_MAX_PEERS && (c = mp_cr_by_msg_id((u16)mp_get16(data + 1))) != NULL) {
                c->nocopy |= (u8)(1 << from);
                c->nocopy_ms = pc_mp_now_ms();
            }
            break;
        case MP_M_CR_OWNERS:
            if (!mp_is_host() && len >= 2) {
                u8 listed[MP_CR_MAX];
                int n = data[1];
                int k;
                int i;

                memset(listed, 0, sizeof(listed));
                for (k = 0; k < n && 2 + (k + 1) * 3 <= len; k++) {
                    c = mp_cr_by_id((u16)mp_get16(data + 2 + k * 3));
                    if (c == NULL) {
                        // one this game is to run but never heard of
                        if (data[2 + k * 3 + 2] == mp_lobby_self_slot()) {
                            s_cr.look_due = TRUE;
                        }
                        continue;
                    }
                    listed[c - s_cr.cr] = TRUE;
                    if (c->owner != data[2 + k * 3 + 2]) {
                        c->owner = data[2 + k * 3 + 2];
                        c->tr.have = FALSE;
                    }
                }
                if (2 + n * 3 < len) {
                    s_cr.bl_runner = data[2 + n * 3];
                }
                // what the host no longer has is gone here too (not this game's new one on its way there)
                for (i = 0; i < MP_CR_MAX; i++) {
                    c = &s_cr.cr[i];
                    if (c->id != 0 && !listed[i] && !c->unlisted &&
                        !(((c->id >> 12) & 0xF) == (0x8 | mp_lobby_self_slot()) &&
                          pc_mp_now_ms() - c->owner_ms < MP_CR_OWN_MS)) {
                        mp_cr_remove(play, c, TRUE);
                    }
                }
            }
            break;
        case MP_M_CR_CLAIM:
            if (mp_is_host() && len >= 4 && data[1] == from && (c = mp_cr_by_msg_id((u16)mp_get16(data + 2))) != NULL) {
                if (c->owner == from) {
                    c->hold_ms = pc_mp_now_ms();
                } else if (c->owner == MP_CR_NOBODY || pc_mp_now_ms() - c->hold_ms >= MP_CR_HOLD_MS) {
                    c->owner = (u8)from;
                    c->owner_ms = pc_mp_now_ms();
                    c->hold_ms = pc_mp_now_ms();
                    c->tr.have = FALSE;
                    s_cr.owners_dirty = TRUE;
                }
            }
            break;
        case MP_M_CR_SCARE:
            if (len >= 3 && (c = mp_cr_by_msg_id((u16)mp_get16(data + 1))) != NULL) {
                if (c->owner == mp_lobby_self_slot()) {
                    if (c->actor != NULL && c->kind == MP_CR_FISH) {
                        aGYO_mp_scare(c->actor);
                    }
                } else if (mp_is_host()) {
                    u8 msg[3];

                    msg[0] = MP_M_CR_SCARE;
                    mp_put16(msg + 1, c->id);
                    mp_cr_send_guest(c->owner, msg, 3);
                }
            }
            break;
        case MP_M_CR_ACT:
            if (len >= 8) {
                if (mp_is_host()) {
                    mp_cr_send_guests(data, 8, from);
                }
                if (Save_Get(scene_no) == SCENE_FG && (u16)mp_get16(data + 6) == (u16)mFI_GetFieldId() &&
                    !mp_world_still() && s_cr.nact < MP_CR_ACTS) {
                    s_cr.act[s_cr.nact].action = (s8)data[1];
                    s_cr.act[s_cr.nact].ut_x = (s16)mp_get16(data + 2);
                    s_cr.act[s_cr.nact].ut_z = (s16)mp_get16(data + 4);
                    s_cr.act[s_cr.nact].ms = pc_mp_now_ms();
                    s_cr.nact++;
                }
            }
            break;
        case MP_M_CR_POP:
            if (len >= 2 && data[1] == MP_CR_POP_GONE) {
                // its runner's balloon flew off: the host has that from the runner only, and passes it on
                if (!mp_is_host() || from == s_cr.bl_runner) {
                    mp_cr_bl_gone();
                    if (mp_is_host()) {
                        mp_cr_send_guests(data, 2, from);
                    }
                }
            } else if (mp_cr_balloon_runner_here()) {
                if (s_cr.bl.have && pc_mp_now_ms() - s_cr.bl.ms <= 200) {
                    s_cr.bl_pop = TRUE;
                }
            } else if (mp_is_host()) {
                mp_cr_send_guest(s_cr.bl_runner, data, 1);
            }
            break;
        case MP_M_CR_NEW:
            // a visitor's own: listed with its id from that visitor's range, run by that visitor
            if (mp_is_host() && len >= 23 && from < MP_MAX_PEERS) {
                u16 id = (u16)mp_get16(data + 3);

                if (((id >> 12) & 0xF) != (0x8 | from) || mp_cr_by_id(id) != NULL) {
                    break;
                }
                // (an acre that already has its lot keeps it: another roll there goes, on that visitor's screen too)
                if (mp_cr_acre_taken(data[1], data[2], (s16)mp_get16(data + 7), (s8)data[5], (s8)data[6],
                                     (u16)mp_get16(data + 9), from)) {
                    u8 no[4];

                    no[0] = MP_M_CR_GONE;
                    mp_put16(no + 1, id);
                    no[3] = FALSE;
                    mp_cr_send_guest(from, no, 4);
                    break;
                }
                if ((c = mp_cr_new(id)) == NULL) {
                    break;
                }
                c->lister = (u8)from;
                c->kind = data[1];
                c->type = data[2];
                c->bx = (s8)data[5];
                c->bz = (s8)data[6];
                c->extra = (s16)mp_get16(data + 7);
                c->field = (u16)mp_get16(data + 9);
                memcpy(&c->spawn, data + 11, 12);
                c->tr.pos = c->spawn;
                c->owner = (u8)from;
                s_cr.owners_dirty = TRUE;
                if (c->kind == MP_CR_INSECT && c->type == aINS_INSECT_TYPE_SPIRIT) {
                    aINS_mp_spirit_taken(c->bx, c->bz);
                }
                if (play != NULL && mp_cr_player_near_acre(play, 0, c->bx, c->bz, c->field)) {
                    mp_cr_materialize(play, c);
                }
                mp_cr_tell_spawn(play, c, -1, from);
            }
            break;
        case MP_M_CR_NET:
            if (mp_is_host() && len >= 3 && from < MP_MAX_PEERS) {
                u16 id = (u16)mp_get16(data + 1);
                u8 msg[4];

                // (the answer comes after the rename, which that visitor's net has followed)
                if ((c = mp_cr_by_msg_id(id)) != NULL) {
                    id = c->id;
                }
                msg[0] = MP_M_CR_NETTED;
                mp_put16(msg + 1, id);
                msg[3] = (u8)mp_cr_host_grant(play, id, from);
                mp_cr_send_guest(from, msg, 4);
            }
            break;
        case MP_M_CR_NETTED:
            if (!mp_is_host() && len >= 4 && s_cr.net_wait && (u16)mp_get16(data + 1) == s_cr.net_id) {
                s_cr.net_wait = FALSE;
                if (data[3]) {
                    // this net keeps it; the others already heard
                    if ((c = mp_cr_by_id(s_cr.net_id)) != NULL) {
                        memset(c, 0, sizeof(*c));
                    }
                } else if (s_cr.net_actor != NULL && play != NULL && Save_Get(scene_no) == SCENE_FG) {
                    // another net got there first: this one comes up empty
                    aINS_mp_lose_catch(s_cr.net_actor, s_cr.net_ant);
                }
                s_cr.net_actor = NULL;
            }
            break;
        case MP_M_CR_SELFDONE:
            if (mp_is_host() && len >= 3 && from < MP_MAX_PEERS && s_cr.lease[from].bx == (s8)data[1] &&
                s_cr.lease[from].bz == (s8)data[2]) {
                s_cr.lease[from].ms = 0;
            }
            break;
        case MP_M_CR_REID:
            if (!mp_is_host() && len >= 5 && (c = mp_cr_by_id((u16)mp_get16(data + 1))) != NULL &&
                mp_cr_by_id((u16)mp_get16(data + 3)) == NULL) {
                c->id = (u16)mp_get16(data + 3);
                if (s_cr.net_id == (u16)mp_get16(data + 1)) {
                    s_cr.net_id = c->id;
                }
            }
            break;
    }
}

void mp_cr_on_guest_gone(int slot) {
    int i;

    for (i = 0; i < MP_CR_MAX; i++) {
        if (s_cr.cr[i].id != 0 && s_cr.cr[i].owner == slot) {
            s_cr.cr[i].owner = MP_CR_NOBODY;
            s_cr.cr[i].hold_ms = 0;
            s_cr.owners_dirty = TRUE;
        }
    }
    if (slot > 0 && slot < MP_MAX_PEERS) {
        s_cr.req[slot] = FALSE;
        s_cr.lease[slot].ms = 0;
        for (i = 0; i < MP_CR_MAX; i++) {
            mp_cr_t* c = &s_cr.cr[i];

            // what the visitor's game made stays out (a spirit, one let go) under one of the host's ids, so
            // the next to take its slot starts afresh
            if (c->id != 0 && ((c->id >> 12) & 0xF) == (u16)(0x8 | slot)) {
                u16 old = c->id;
                u8 msg[5];
                int g;

                c->old_id = old;
                c->old_ms = pc_mp_now_ms();
                c->id = mp_cr_next_id();
                for (g = 0; g < MP_CR_GRANTS; g++) {
                    if (s_cr.grant[g].id == old) {
                        s_cr.grant[g].id = c->id;
                    }
                }
                msg[0] = MP_M_CR_REID;
                mp_put16(msg + 1, old);
                mp_put16(msg + 3, c->id);
                mp_cr_send_guests(msg, 5, slot);
            }
        }
    }
    if (s_cr.bl_runner == slot) {
        s_cr.bl_runner = MP_CR_NOBODY;
        s_cr.owners_dirty = TRUE;
    }
}

void mp_cr_reset(void) {
    mp_cr_wipe();
}

#endif
