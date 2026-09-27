// pc_mp_event.c
// today's events as the host set them up; a visitor shows them and never rolls its own
#include "pc_mp.h"

#ifdef VITA_MP

#include "m_common_data.h"
#include "m_event.h"
#include "m_field_info.h"
#include "m_event_map_npc.h"
#include "ac_set_npc_manager.h"

#include <string.h>

#define MP_EV_RUN_BYTES ((mEv_EVENT_NUM + 7) / 8)
#define MP_EV_RESEND_MS 10000
#define MP_EV_OFFER_MS  500   // a visitor's places go to the host this often until it has them
#define MP_EV_SLOW_MS   5000  // ...and this often after a while (the host in a menu, where events wait)
#define MP_EV_MADE_MS   10000

#define MP_AERO_SEQ      0xDA // the radio exercise song
#define MP_AERO_EVERY_MS 1000
#define MP_AERO_FRESH_MS 3000
#define MP_AERO_LEAD     2 // updates before a start is under way
#define MP_AERO_LEN      15

// the song's updates in some ms: 4 an audio frame, one frame each 560 samples at 32 kHz
#define MP_AERO_UPDATES(ms) ((int)(ms) * 8 / 35)

typedef struct {
    s16 place_use;
    mEv_place_c place[mEv_PLACE_NUM];
    u8 run[MP_EV_RUN_BYTES];
} mp_evstate_t;

typedef char mp_evstate_fits[(1 + sizeof(mp_evstate_t) <= MP_REL_MAX) ? 1 : -1];
typedef char mp_evplace_fits[(2 + mEv_PLACE_NUM * (1 + sizeof(mEv_place_c)) <= MP_REL_MAX) ? 1 : -1];

// a place this game gave an event for the host, until the host has it
typedef struct {
    u8 have;
    u8 slot;
    mEv_place_c place;
    u32 ms;
} mp_evmade_t;

// a place this game took down: gone for good by the frame's end, or put up elsewhere (moved)
typedef struct {
    u8 type;
    u8 id;
    u8 tent;    // one of the host's tents or stalls: the host takes it down too
    u8 decided;
    u8 sent;
} mp_evgone_t;

static struct {
    mp_evstate_t sent; // host: what the visitors were last told
    int sent_valid;
    u32 sent_ms;
    mp_evstate_t host; // guest: the host's latest
    int have;
    mp_evmade_t made[mEv_PLACE_NUM]; // visitor placing events while the host is away from town
    mp_evgone_t gone[mEv_PLACE_NUM]; // ...and places it took down
    int ngone;
    u32 offer_ms;
    u32 gone_ms;
} s_ev;

// the song on this screen, and the others' as last heard
static struct {
    int playing;
    int starts;     // the audio side's count of songs begun, to spot it begun again
    int period;     // updates a time round, once it has gone round anywhere
    u16 loop;       // times round since its timeline began
    u16 base_loop;  // the time round this screen's start joined
    u16 start_loop; // ...as the latest start set it
    u32 sent_ms;
    int have;
    u32 phase;
    u32 ref_period;
    u16 ref_loop;
    u32 at; // shared clock at that phase
    u32 heard_ms;
} s_aero;

static void mp_ev_put32(u8* p, u32 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static u32 mp_ev_get32(const u8* p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

// a host with a visitor off the train, or a visitor in town
static int mp_ev_shared(void) {
    int g;

    if (mp_is_host()) {
        for (g = 1; g < MP_MAX_PEERS; g++) {
            if (mp_lobby_guest_arrived(g)) {
                return TRUE;
            }
        }
        return FALSE;
    }
    return mp_travel_state() == MP_TRAVEL_VISITING;
}

static void mp_ev_capture(mp_evstate_t* st) {
    mEv_common_data_c* ev = Common_GetPointer(event_common);
    int t;

    memset(st, 0, sizeof(*st));
    st->place_use = ev->place_use_bitfield;
    memcpy(st->place, ev->place, sizeof(st->place));
    // statuses only mean something while the host's town is loaded; its places outlast a door
    for (t = 0; t < mEv_EVENT_NUM; t++) {
        if (mEv_check_status(t, mEv_STATUS_RUN)) {
            st->run[t >> 3] |= (u8)(1 << (t & 7));
        }
    }
}

static void mp_ev_send(int conn, const mp_evstate_t* st) {
    u8 msg[1 + sizeof(mp_evstate_t)];

    msg[0] = MP_M_EVSTATE;
    memcpy(msg + 1, st, sizeof(*st));
    if (conn >= 0) {
        mp_lobby_send_rel(conn, msg, (int)sizeof(msg));
    } else {
        int g;

        // a visitor gone quiet gets the picture again when they're back
        for (g = 1; g < MP_MAX_PEERS; g++) {
            int to = mp_lobby_guest_conn(g);

            if (mp_lobby_guest_arrived(g) && to >= 0 && !mp_lobby_conn_quiet(to)) {
                mp_lobby_send_rel(to, msg, (int)sizeof(msg));
            }
        }
    }
}

// guest: the host's slot for this event's place, -1 none
static int mp_ev_host_slot(int type, int id) {
    int i;

    for (i = 0; i < mEv_PLACE_NUM; i++) {
        if (((s_ev.host.place_use >> i) & 1) && s_ev.host.place[i].info.type == type &&
            s_ev.host.place[i].info.id == id) {
            return i;
        }
    }
    return -1;
}

static int mp_ev_host_has(int type, int id) {
    return mp_ev_host_slot(type, id) >= 0;
}

// a visitor's talk sent a character of the host's event on to another spot (the host never saw the talk): the host
// takes the spot, and this game keeps it until the host's word shows it
#define MP_EV_MOVED    4
#define MP_EV_MOVED_MS 10000

static struct {
    u8 have;
    u8 type;
    u8 id;
    mEv_place_data_c data;
    u32 ms;
} s_ev_moved[MP_EV_MOVED];

// guest: the host's places, over whatever this machine's event manager did with its own, but for the ones this
// game gave events for the host until the host has them
static void mp_ev_enforce(void) {
    mEv_common_data_c* ev = Common_GetPointer(event_common);
    mEv_place_c place[mEv_PLACE_NUM];
    s16 use = s_ev.host.place_use;
    int i;

    memcpy(place, s_ev.host.place, sizeof(place));
    // (a spot a talk here moved a character to, till the host has it)
    for (i = 0; i < MP_EV_MOVED; i++) {
        int slot = s_ev_moved[i].have ? mp_ev_host_slot(s_ev_moved[i].type, s_ev_moved[i].id) : -1;

        if (slot < 0 || memcmp(&place[slot].data, &s_ev_moved[i].data, sizeof(mEv_place_data_c)) == 0 ||
            pc_mp_now_ms() - s_ev_moved[i].ms > MP_EV_MOVED_MS) {
            s_ev_moved[i].have = FALSE;
            continue;
        }
        place[slot].data = s_ev_moved[i].data;
    }
    for (i = 0; i < mEv_PLACE_NUM; i++) {
        mp_evmade_t* m = &s_ev.made[i];
        int slot;

        if (!m->have) {
            continue;
        }
        // (one of the host's this game moved stands where it went)
        slot = mp_ev_host_slot(m->place.info.type, m->place.info.id);
        if (slot >= 0) {
            place[slot] = m->place;
            continue;
        }
        slot = m->slot;
        if ((use >> slot) & 1) {
            for (slot = 0; slot < mEv_PLACE_NUM && ((use >> slot) & 1); slot++) {
            }
        }
        if (slot < mEv_PLACE_NUM) {
            use |= (s16)(1 << slot);
            place[slot] = m->place;
        }
    }
    if (ev->place_use_bitfield != use || memcmp(ev->place, place, sizeof(ev->place)) != 0) {
        ev->place_use_bitfield = use;
        memcpy(ev->place, place, sizeof(ev->place));
    }
}

// visitor out in town while the host isn't: it sets the day's events up for everyone (the screen that also steps
// the villagers nobody watches), and the host takes the places it gives them
static int mp_ev_placing(void) {
    return mp_travel_state() == MP_TRAVEL_VISITING && mp_npc_walker() == mp_lobby_self_slot();
}

static const mEv_place_c* mp_ev_place_here(int type, int id);

// a place this game took down this frame, not yet known gone for good
static int mp_ev_just_gone(int type, int id) {
    int i;

    for (i = 0; i < s_ev.ngone; i++) {
        if (!s_ev.gone[i].decided && s_ev.gone[i].type == type && s_ev.gone[i].id == id) {
            return TRUE;
        }
    }
    return FALSE;
}

void pc_mp_ev_moved(int type, int id, const void* data) {
    u8 msg[3 + sizeof(mEv_place_data_c)];
    int k;

    if (mp_is_host() || mp_travel_state() != MP_TRAVEL_VISITING || mp_lobby_host_conn() < 0 ||
        !mp_ev_host_has(type, id)) {
        return;
    }
    for (k = 0; k < MP_EV_MOVED && !(s_ev_moved[k].have && s_ev_moved[k].type == type && s_ev_moved[k].id == id); k++) {
    }
    if (k == MP_EV_MOVED) {
        for (k = 0; k < MP_EV_MOVED - 1 && s_ev_moved[k].have; k++) {
        }
    }
    s_ev_moved[k].have = TRUE;
    s_ev_moved[k].type = (u8)type;
    s_ev_moved[k].id = (u8)id;
    memcpy(&s_ev_moved[k].data, data, sizeof(mEv_place_data_c));
    s_ev_moved[k].ms = pc_mp_now_ms();
    msg[0] = MP_M_EVMOVE;
    msg[1] = (u8)type;
    msg[2] = (u8)id;
    memcpy(msg + 3, data, sizeof(mEv_place_data_c));
    mp_lobby_send_rel(mp_lobby_host_conn(), msg, (int)sizeof(msg));
}

void pc_mp_ev_place_made(int type, int id, int slot) {
    mp_evmade_t* m;

    // (one the host has goes only as a move: taken down and put up elsewhere at once)
    if (!mp_ev_placing() || slot < 0 || slot >= mEv_PLACE_NUM ||
        (mp_ev_host_has(type, id) && !mp_ev_just_gone(type, id))) {
        return;
    }
    // (its spot is filled in after this; the next offer reads it)
    m = &s_ev.made[slot];
    memset(m, 0, sizeof(*m));
    m->have = TRUE;
    m->slot = (u8)slot;
    m->place.info.type = (u8)type;
    m->place.info.id = (u8)id;
    m->ms = pc_mp_now_ms();
}

void pc_mp_ev_place_gone(int type, int id) {
    int i;

    for (i = 0; i < mEv_PLACE_NUM; i++) {
        if (s_ev.made[i].have && s_ev.made[i].place.info.type == type && s_ev.made[i].place.info.id == id) {
            s_ev.made[i].have = FALSE;
        }
    }
    // the screen running the day's events for the host: what it takes down, the frame's end tells gone or moved
    if (!mp_ev_placing() || s_ev.ngone >= mEv_PLACE_NUM) {
        return;
    }
    for (i = 0; i < s_ev.ngone; i++) {
        if (s_ev.gone[i].type == type && s_ev.gone[i].id == id) {
            s_ev.gone[i].decided = FALSE; // (taken down again: gone or moved, anew)
            return;
        }
    }
    i = mp_ev_host_slot(type, id);
    memset(&s_ev.gone[s_ev.ngone], 0, sizeof(s_ev.gone[0]));
    s_ev.gone[s_ev.ngone].type = (u8)type;
    s_ev.gone[s_ev.ngone].id = (u8)id;
    s_ev.gone[s_ev.ngone].tent = i >= 0 && ITEM_NAME_GET_TYPE(s_ev.host.place[i].data.actor_name) == NAME_TYPE_STRUCT;
    s_ev.ngone++;
}

// a tent or stall of the host's this game took down for good goes on the host's side too, until its places show it
// gone (only heeded while the host plays an NES game and its own events stand still); a moved one goes as a place
static void mp_ev_gone_send(void) {
    u32 now = pc_mp_now_ms();
    int again = now - s_ev.gone_ms >= MP_EV_SLOW_MS;
    int kept = 0;
    int i;

    for (i = 0; i < s_ev.ngone; i++) {
        mp_evgone_t g = s_ev.gone[i];

        if (!g.decided) {
            g.decided = TRUE;
            if (mp_ev_place_here(g.type, g.id) != NULL) {
                continue;
            }
        }
        if (!g.tent || !mp_ev_placing() || !mp_ev_host_has(g.type, g.id) || mp_lobby_host_conn() < 0) {
            continue;
        }
        if (!g.sent || again) {
            u8 msg[3];

            msg[0] = MP_M_EVGONE;
            msg[1] = g.type;
            msg[2] = g.id;
            mp_lobby_send_rel(mp_lobby_host_conn(), msg, 3);
            g.sent = TRUE;
            s_ev.gone_ms = now;
        }
        s_ev.gone[kept++] = g;
    }
    s_ev.ngone = kept;
}

static const mEv_place_c* mp_ev_place_here(int type, int id) {
    mEv_common_data_c* ev = Common_GetPointer(event_common);
    int i;

    for (i = 0; i < mEv_PLACE_NUM; i++) {
        if (((ev->place_use_bitfield >> i) & 1) && ev->place[i].info.type == type && ev->place[i].info.id == id) {
            return &ev->place[i];
        }
    }
    return NULL;
}

#define MP_EV_AREA_DATA (int)sizeof(((mEv_area_c*)0)->data)
#define MP_EV_OFFER_MAX (2 + mEv_PLACE_NUM * (2 + (int)sizeof(mEv_place_c)) + mEv_AREA_NUM * (1 + MP_EV_AREA_DATA))

// visitor: the places it gave events go to the host until the host has them, and with each event's first, the
// records it keeps in the save (who's camping, what Redd sells), which the host takes when it has none of its own
static void mp_ev_offer(void) {
    static int s_from; // (a message too small for them all: the next one starts further on)
    u8 msg[MP_EV_OFFER_MAX];
    u8 told[(mEv_EVENT_NUM + 7) / 8];
    u32 now = pc_mp_now_ms();
    u32 every = MP_EV_SLOW_MS;
    int at = 2;
    int n = 0;
    int j;

    memset(told, 0, sizeof(told));
    // (until the host has it as it is here, or the event ends here; a move of the host's own only while this game
    // runs the day's events)
    for (j = 0; j < mEv_PLACE_NUM; j++) {
        int i = (s_from + j) % mEv_PLACE_NUM;
        mp_evmade_t* m = &s_ev.made[i];
        const mEv_place_c* pl;
        int start = at;
        int k;

        if (!m->have) {
            continue;
        }
        pl = mp_ev_place_here(m->place.info.type, m->place.info.id);
        k = mp_ev_host_slot(m->place.info.type, m->place.info.id);
        if (pl == NULL ||
            (k >= 0 && (memcmp(&s_ev.host.place[k].data, &pl->data, sizeof(pl->data)) == 0 || !mp_ev_placing()))) {
            m->have = FALSE;
            continue;
        }
        // the whole place as this game's event manager left it
        m->place = *pl;
        msg[at++] = m->slot;
        memcpy(msg + at, &m->place, sizeof(mEv_place_c));
        at += sizeof(mEv_place_c);
        msg[at] = 0;
        if (m->place.info.type < mEv_EVENT_NUM && !(told[m->place.info.type >> 3] & (1 << (m->place.info.type & 7)))) {
            u8* d;
            u8 aid;
            int a;

            told[m->place.info.type >> 3] |= (u8)(1 << (m->place.info.type & 7));
            for (a = 0; (d = mEv_mp_stand_in(m->place.info.type, a, &aid)) != NULL; a++) {
                u8* e = msg + at + 1 + msg[at] * (1 + MP_EV_AREA_DATA);

                e[0] = aid;
                memcpy(e + 1, d, MP_EV_AREA_DATA);
                msg[at]++;
            }
        }
        at += 1 + msg[at] * (1 + MP_EV_AREA_DATA);
        if (at > MP_REL_MAX) {
            at = start; // (the next message has it)
            if (m->place.info.type < mEv_EVENT_NUM) {
                told[m->place.info.type >> 3] &= (u8)~(1 << (m->place.info.type & 7));
            }
            continue;
        }
        if (now - m->ms < MP_EV_MADE_MS) {
            every = MP_EV_OFFER_MS;
        }
        n++;
    }
    if (n == 0 || now - s_ev.offer_ms < every || mp_lobby_host_conn() < 0) {
        return;
    }
    s_ev.offer_ms = now;
    s_from = (s_from + 1) % mEv_PLACE_NUM;
    msg[0] = MP_M_EVPLACE;
    msg[1] = (u8)n;
    mp_lobby_send_rel(mp_lobby_host_conn(), msg, at);
}

static u8 s_ev_away[(mEv_EVENT_NUM + 7) / 8]; // host: events visitors put up while it played an NES game

// host: an event a visitor put up while the host played an NES game (its own statuses stood still meanwhile)
int mp_ev_placed_away(int type) {
    if (!mp_emu_active()) {
        memset(s_ev_away, 0, sizeof(s_ev_away));
        return FALSE;
    }
    return type >= 0 && type < mEv_EVENT_NUM && ((s_ev_away[type >> 3] >> (type & 7)) & 1);
}

// host: its own game isn't placing the day's events (an NES game, or no town up)
static int mp_ev_host_away(void) {
    return mp_emu_active() || !mFI_CheckFieldData() || mFI_GET_TYPE(mFI_GetFieldId()) != mFI_FIELD_FG;
}

// host: a place a visitor gave an event while the host couldn't; taken while the event is on here and has none (the
// host in an NES game has no status of its own to check), or moved by the visitor running the day's events
static void mp_ev_take_place(const mEv_place_c* pl, int slot, int walker) {
    mEv_common_data_c* ev = Common_GetPointer(event_common);
    const mEv_place_data_c* d = &pl->data;
    mEv_place_data_c* to;

    // (only what the game's own placing makes: a building its table knows, in the town, on a unit)
    if (pl->info.type >= mEv_EVENT_NUM || (!mp_emu_active() && !mEv_check_status(pl->info.type, mEv_STATUS_ACTIVE)) ||
        (ITEM_NAME_GET_TYPE(d->actor_name) == NAME_TYPE_STRUCT && d->actor_name >= STRUCTURE_END) ||
        d->block.x < 0 || d->block.x >= BLOCK_X_NUM || d->block.z < 0 || d->block.z >= BLOCK_Z_NUM || d->unit.x < 0 ||
        d->unit.x >= UT_X_NUM || d->unit.z < 0 || d->unit.z >= UT_Z_NUM) {
        return;
    }
    to = mEv_get_common_place(pl->info.type, pl->info.id);
    if (to != NULL) {
        if (walker && mp_ev_host_away()) {
            *to = *d;
        }
        return;
    }
    if (mp_emu_active()) {
        s_ev_away[pl->info.type >> 3] |= (u8)(1 << (pl->info.type & 7));
    }
    if (slot >= 0 && slot < mEv_PLACE_NUM && ((ev->place_use_bitfield >> slot) & 1) == 0) {
        ev->place_use_bitfield |= (s16)(1 << slot);
        ev->place[slot] = *pl;
        return;
    }
    to = mEv_reserve_common_place(pl->info.type, pl->info.id);
    if (to != NULL) {
        *to = *d;
    }
}

// guest: an event starts here once the host has started it or placed it (or this game sets them up for the
// host away from town)
int mp_event_may_start(int type) {
    int i;

    if (mp_town_writer_allowed() || mp_ev_placing()) {
        return TRUE;
    }
    if (!s_ev.have || type < 0 || type >= mEv_EVENT_NUM) {
        return FALSE;
    }
    if (s_ev.host.run[type >> 3] & (1 << (type & 7))) {
        return TRUE;
    }
    for (i = 0; i < mEv_PLACE_NUM; i++) {
        if (((s_ev.host.place_use >> i) & 1) && s_ev.host.place[i].info.type == type) {
            return TRUE;
        }
    }
    return FALSE;
}

// after the actors: the host tells visitors what changed, a visitor holds to the host's places
void mp_event_tick(struct game_play_s* play) {
    (void)play;
    if (mp_is_host()) {
        mp_evstate_t now;
        u32 ms = pc_mp_now_ms();
        int anyone = FALSE;
        int g;

        for (g = 1; g < MP_MAX_PEERS; g++) {
            anyone |= mp_lobby_guest_arrived(g);
        }
        if (!anyone) {
            s_ev.sent_valid = FALSE;
            return;
        }
        mp_ev_capture(&now);
        if (!s_ev.sent_valid || memcmp(&now, &s_ev.sent, sizeof(now)) != 0 || ms - s_ev.sent_ms >= MP_EV_RESEND_MS) {
            mp_ev_send(-1, &now);
            s_ev.sent = now;
            s_ev.sent_valid = TRUE;
            s_ev.sent_ms = ms;
        }
    } else if (mp_travel_state() == MP_TRAVEL_VISITING && s_ev.have) {
        mp_ev_gone_send();
        mp_ev_offer();
        mp_ev_enforce();
    }
}

// guest: an event's villagers are the host's picks; they're asked for (once a second) till its list streams in
void mp_event_roster_ask(int type) {
    static u32 s_ask_ms;
    static int s_ask_type = -1;
    u8 msg[2];
    int conn = mp_lobby_host_conn();

    if (conn < 0 || type < 0 || type >= mEv_EVENT_NUM || (type == s_ask_type && pc_mp_now_ms() - s_ask_ms < 1000)) {
        return;
    }
    s_ask_type = type;
    s_ask_ms = pc_mp_now_ms();
    msg[0] = MP_M_EVROSTER;
    msg[1] = (u8)type;
    mp_lobby_send_rel(conn, msg, 2);
}

// host: a visitor's event is on before this game's own set-up picked its villagers (indoors, say): picked now, as
// that set-up would, into the save that streams to everyone
static void mp_ev_roster_make(int type) {
    aSNMgr_event_save_c* save_p;
    int i;

    if (type < 0 || type >= mEv_EVENT_NUM || !mEv_check_status(type, mEv_STATUS_ACTIVE) ||
        mEvMN_GetMapIdx(type) == -1 || mEv_get_save_area(type, 0xF) != NULL) {
        return;
    }
    save_p = (aSNMgr_event_save_c*)mEv_reserve_save_area(type, 0xF);
    if (save_p == NULL) {
        return;
    }
    for (i = 0; i < aSNMgr_EVENT_NORMAL_NPC_NUM; i++) {
        save_p->animal_idx[i] = 0xFF;
    }
    mEvMN_GetNpcJointEv(save_p, type);
}

void mp_event_on_rel(int conn, const unsigned char* data, int len) {
    if (data[0] == MP_M_EVROSTER && mp_is_host() && mp_lobby_guest_slot(conn) >= 0 && len >= 2) {
        mp_ev_roster_make(data[1]);
        return;
    }
    if (len >= MP_AERO_LEN && data[0] == MP_M_AERO) {
        int from = mp_is_host() ? mp_lobby_guest_slot(conn) + 1 : 0;

        if (mp_is_host()) {
            int g;

            // while the host plays it, its own is the town's
            if (from <= 0 || s_aero.playing) {
                return;
            }
            for (g = 1; g < MP_MAX_PEERS; g++) {
                int to = mp_lobby_guest_conn(g);

                if (g != from && mp_lobby_guest_arrived(g) && to >= 0) {
                    mp_lobby_send_rel(to, data, MP_AERO_LEN);
                }
            }
        } else if (conn != mp_lobby_host_conn()) {
            return;
        }
        s_aero.have = TRUE;
        s_aero.phase = mp_ev_get32(data + 1);
        s_aero.ref_period = mp_ev_get32(data + 5);
        s_aero.ref_loop = (u16)(data[9] | (data[10] << 8));
        s_aero.at = mp_ev_get32(data + 11);
        s_aero.heard_ms = pc_mp_now_ms();
        return;
    }
    if (data[0] == MP_M_EVGONE && mp_is_host() && mp_lobby_guest_slot(conn) + 1 == mp_npc_walker() && len >= 3) {
        const mEv_place_data_c* d = (data[1] < mEv_EVENT_NUM) ? mEv_get_common_place(data[1], data[2]) : NULL;

        if (mp_emu_active() && d != NULL && ITEM_NAME_GET_TYPE(d->actor_name) == NAME_TYPE_STRUCT) {
            mEv_clear_common_place(data[1], data[2]);
            s_ev_away[data[1] >> 3] &= (u8) ~(1 << (data[1] & 7));
        }
        return;
    }
    if (data[0] == MP_M_EVMOVE && mp_is_host() && mp_lobby_guest_slot(conn) >= 0 &&
        len >= 3 + (int)sizeof(mEv_place_data_c)) {
        mEv_place_data_c d;
        mEv_place_data_c* to;

        memcpy(&d, data + 3, sizeof(d));
        to = data[1] < mEv_EVENT_NUM ? mEv_get_common_place(data[1], data[2]) : NULL;
        if (to != NULL && mEv_check_status(data[1], mEv_STATUS_ACTIVE) && d.actor_name == to->actor_name &&
            d.block.x >= 0 && d.block.x < BLOCK_X_NUM && d.block.z >= 0 && d.block.z < BLOCK_Z_NUM && d.unit.x >= 0 &&
            d.unit.x < UT_X_NUM && d.unit.z >= 0 && d.unit.z < UT_Z_NUM) {
            *to = d;
        }
        return;
    }
    if (data[0] == MP_M_EVPLACE && mp_is_host() && mp_lobby_guest_slot(conn) >= 0 && len >= 2) {
        int n = data[1];
        int at = 2;
        int k;

        for (k = 0; k < n && at + 2 + (int)sizeof(mEv_place_c) <= len; k++) {
            const u8* e = data + at;
            int na = e[1 + sizeof(mEv_place_c)];
            mEv_place_c pl;
            int a;

            if (at + 2 + (int)sizeof(mEv_place_c) + na * (1 + MP_EV_AREA_DATA) > len) {
                break;
            }
            memcpy(&pl, e + 1, sizeof(pl));
            mp_ev_take_place(&pl, e[0], mp_lobby_guest_slot(conn) + 1 == mp_npc_walker());
            // (the records the visitor's game made for it, where the host's has none yet)
            for (a = 0; a < na && pl.info.type < mEv_EVENT_NUM; a++) {
                const u8* ar = e + 2 + sizeof(mEv_place_c) + a * (1 + MP_EV_AREA_DATA);

                if (mEv_mp_real_area(pl.info.type, ar[0]) == NULL) {
                    u8* to = mEv_reserve_save_area(pl.info.type, ar[0]);

                    if (to != NULL) {
                        memcpy(to, ar + 1, MP_EV_AREA_DATA);
                    }
                }
            }
            at += 2 + sizeof(mEv_place_c) + na * (1 + MP_EV_AREA_DATA);
        }
        return;
    }
    if (mp_is_host() || conn != mp_lobby_host_conn() || data[0] != MP_M_EVSTATE ||
        len < 1 + (int)sizeof(mp_evstate_t)) {
        return;
    }
    memcpy(&s_ev.host, data + 1, sizeof(s_ev.host));
    s_ev.have = TRUE;
    if (mp_travel_state() == MP_TRAVEL_VISITING) {
        mp_ev_enforce();
    }
}

// host: a visitor off the train gets the current picture at once
void mp_event_on_arrived(int conn) {
    mp_evstate_t now;

    mp_ev_capture(&now);
    mp_ev_send(conn, &now);
}

void mp_event_reset(void) {
    memset(&s_ev, 0, sizeof(s_ev));
    memset(&s_aero, 0, sizeof(s_aero));
}

// aerobics

// the song as this screen plays it (the audio side counts its times round exactly, a join's catching up
// included), and its place told to the others each second: the host's is the town's; a visitor's goes to
// the host, which passes it on while it has none of its own
void mp_aero_frame(void) {
    int upd;
    int loop_sc;
    int loops;
    int period;
    int starts;
    int got;
    unsigned int now;
    u8 msg[MP_AERO_LEN];
    int g;

    if (!mp_ev_shared()) {
        // how long the song takes to go round is the song's own
        period = s_aero.period;
        memset(&s_aero, 0, sizeof(s_aero));
        s_aero.period = period;
        return;
    }
    got = Na_MpAeroState(&upd, &loop_sc, &loops, &period, &starts);
    if (got < 0) {
        return; // the audio side was counting: next frame
    }
    if (got == 0) {
        s_aero.playing = FALSE;
        return;
    }
    if (!s_aero.playing || starts != s_aero.starts) {
        int fresh = starts != s_aero.starts;

        // begun (or begun again): its times round count on from the one its start joined
        s_aero.playing = TRUE;
        s_aero.starts = starts;
        s_aero.base_loop = s_aero.start_loop;
        // (in the very first moment of a start its catch-up may not be set yet)
        if (fresh) {
            return;
        }
    }
    s_aero.loop = (u16)(s_aero.base_loop + loops);
    if (period > 0) {
        s_aero.period = period;
    }
    if (pc_mp_now_ms() - s_aero.sent_ms < MP_AERO_EVERY_MS || !mp_shared_clock_ms(&now)) {
        return;
    }
    s_aero.sent_ms = pc_mp_now_ms();
    msg[0] = MP_M_AERO;
    mp_ev_put32(msg + 1, (u32)(upd - loop_sc));
    mp_ev_put32(msg + 5, (u32)s_aero.period);
    msg[9] = (u8)s_aero.loop;
    msg[10] = (u8)(s_aero.loop >> 8);
    mp_ev_put32(msg + 11, now);
    if (mp_is_host()) {
        for (g = 1; g < MP_MAX_PEERS; g++) {
            int to = mp_lobby_guest_conn(g);

            if (mp_lobby_guest_arrived(g) && to >= 0) {
                mp_lobby_send_rel(to, msg, MP_AERO_LEN);
            }
        }
    } else if (mp_lobby_host_conn() >= 0) {
        mp_lobby_send_rel(mp_lobby_host_conn(), msg, MP_AERO_LEN);
    }
}

// the song starts on this screen: with another playing it lately it joins that one's place (and time
// round); alone, it begins at the top as ever
int pc_mp_aero_skip(int seq_id) {
    unsigned int now;
    int period;
    int target;

    s_aero.start_loop = 0;
    if (seq_id != MP_AERO_SEQ || !mp_ev_shared() || !s_aero.have ||
        pc_mp_now_ms() - s_aero.heard_ms > MP_AERO_FRESH_MS || !mp_shared_clock_ms(&now)) {
        return 0;
    }
    target = (int)s_aero.phase + MP_AERO_UPDATES((int)(now - s_aero.at)) + MP_AERO_LEAD;
    if (target <= 0) {
        return 0;
    }
    // (a catch-up past the end goes round on its own, and the audio side counts it)
    period = s_aero.ref_period > 0 ? (int)s_aero.ref_period : s_aero.period;
    s_aero.start_loop = s_aero.ref_loop;
    if (period > 0) {
        s_aero.start_loop += (u16)(target / period);
        target %= period;
    }
    return target;
}

int pc_mp_aero_shared(void) {
    return mp_ev_shared() && s_aero.playing;
}

// the same on every screen playing the song in step: the day, the time round and the measure
unsigned int pc_mp_aero_seed(int measure, int salt) {
    lbRTC_time_c* t = Common_GetPointer(time.rtc_time);

    return mp_shared_hash((u32)t->year * 416u + (u32)t->month * 32u + t->day,
                          ((u32)s_aero.loop << 4) | ((u32)measure & 0xF), (u32)salt);
}

#endif
