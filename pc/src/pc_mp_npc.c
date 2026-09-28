// pc_mp_npc.c
// villagers in a shared town: talk locks, visitors' talk results, who moves each villager
#include "pc_mp.h"

#ifdef VITA_MP

#include "m_common_data.h"
#include "m_actor.h"
#include "m_demo.h"
#include "m_msg.h"
#include "m_npc.h"
#include "m_play.h"
#include "m_player.h"
#include "m_player_lib.h"
#include "m_post_office.h"
#include "m_private.h"
#include "m_name_table.h"
#include "ac_npc.h"
#include "ac_npc_post_man.h"
#include "ac_tools.h"
#include "m_field_make.h"
#include "m_needlework.h"
#include "m_random_field.h"
#include "m_fishrecord.h"
#include "m_font.h"
#include "audio.h"
#include "ef_effect_control.h"

#include <stddef.h>
#include <string.h>

#define MP_TALK_SETTLE_FRAMES 30    // the villager's own wrap-up after the window closes
#define MP_TALK_DONE_MS       10000 // past this the page checksums finish the job
#define MP_ANM_ISLAND         0xFE
#define MP_ANM_NONE           0xFF
#define MP_MEM_BEGIN          offsetof(Animal_c, memories)
#define MP_MEM_END            (offsetof(Animal_c, memories) + sizeof(((Animal_c*)0)->memories))
#define MP_RUN_PAYLOAD        380

enum {
    MP_TALK_IDLE,
    MP_TALK_ON,
    MP_TALK_SETTLING,
};

typedef struct {
    u32 off;
    u32 size;
} mp_trgn_t;

#define MP_FSIZE(f) sizeof(((Save_t*)0)->f)
#define MP_FIELD(f) { offsetof(Save_t, f), MP_FSIZE(f) }

// save regions a talk may change besides the villager: event records (Chip's fish, Redd's sales),
// the tourney board, a villager talked out of moving, Blanca's new face, a visitor's museum donation, Wisp's weeding,
// a bridge Tortimer was asked for
static const mp_trgn_t s_talk_rgn[] = {
    MP_FIELD(remove_animal_idx), MP_FIELD(event_save_data), MP_FIELD(event_save_common),
    MP_FIELD(fishRecord),        MP_FIELD(mask_cat),        MP_FIELD(museum_display),
    MP_FIELD(clear_grass),       MP_FIELD(bridge),
};

#define MP_TALK_RGN (int)(sizeof(s_talk_rgn) / sizeof(s_talk_rgn[0]))
#define MP_TALK_RGN_BYTES                                                                                  \
    (MP_FSIZE(remove_animal_idx) + MP_FSIZE(event_save_data) + MP_FSIZE(event_save_common) +             \
     MP_FSIZE(fishRecord) + MP_FSIZE(mask_cat) + MP_FSIZE(museum_display) + MP_FSIZE(clear_grass) +              MP_FSIZE(bridge))

#define MP_TALK_EXTRA_BYTES 256
#define MP_PLOCKS           8    // villagers one player's talks still have results on their way for
#define MP_PLOCK_ALL        0xFF // (a lock message's pending byte: every one of that player's let go)
// the most one talk's result takes (every byte of its villager and regions changed), and room for many
#define MP_TALK_RESULT_MAX  (2 * ((int)sizeof(Animal_c) + MP_TALK_RGN_BYTES) + 2048)
#define MP_TALK_PEND_BYTES  (8 * MP_TALK_RESULT_MAX)
#define MP_PP_RETRY_MS      1000 // a passport write that didn't make it is tried again this often

static struct {
    u16 lock[MP_MAX_PEERS];             // villager each player is talking to
    u16 plock[MP_MAX_PEERS][MP_PLOCKS]; // ...and ones whose talks' results are still on their way
    int state;
    ACTOR* actor; // only compared once the talk is over
    int anm;
    u16 npc_id;
    int settle;
    Animal_c base;
    u8 rgn_base[MP_TALK_RGN_BYTES];
    int result_due;  // guest: the talk's result waits for the passport that has what it handed over
    u32 result_seq;
    u32 result_retry_ms; // guest: ...a write that didn't make it is tried again from then
    u32 result_floor;    // guest: ...the newest passport staged as it ended (any later one leaves its changes out)
    int result_live;  // guest: ...the talk just over has its result still to put together
    int buffering;    // guest: messages go into pend, not out
    int draining;     // guest: pend is going out
    u8* pend;         // guest: results put together ahead of their passport: {len lo, len hi, message}
    int pend_len;
    int plock_next;   // guest: the pending lock slot to use next
    int pend_at;      // guest: how far pend has gone out
    int pend_safe;    // guest: ...and how far a passport on the card covers it (nothing past it goes)
    int pend_live;    // guest: the last talk's own result is in pend, behind any put together earlier
    int defer_sent;   // guest: the talk's own result went: its host's word ends the wait
    int res_start;    // guest: where the result being put together began in pend
    int dropping;     // guest: ...it didn't fit, and none of it goes
    // guest: villagers of results put together ahead, whose bytes from the host still wait
    struct {
        int anm;
        u16 seq;
        u32 until_ms; // 0 till the result has gone
        int pend_end; // where its result ends in pend
    } held[MP_PLOCKS];
    u8 extra[MP_TALK_EXTRA_BYTES]; // guest: what the talk handed over outside the save's talk regions
    int extra_len;
    int settle_still; // wind-down frames a menu stopped
    u16 seq;
    // guest: host bytes for what our talk touched wait until our result has landed
    int deferring;
    int defer_anm;
    u16 defer_seq;
    u32 defer_until_ms;
    int refresh;
} s_npc;

static u8* mp_save_base(void) {
    return (u8*)&common_data.save.save;
}

static void mp_put16(u8* p, u32 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static u32 mp_get16(const u8* p) {
    return (u32)p[0] | ((u32)p[1] << 8);
}

static void mp_put32(u8* p, u32 v) {
    mp_put16(p, v);
    mp_put16(p + 2, v >> 16);
}

static u32 mp_get32(const u8* p) {
    return mp_get16(p) | (mp_get16(p + 2) << 16);
}

static Animal_c* mp_anm_ptr(int anm) {
    if (anm >= 0 && anm < ANIMAL_NUM_MAX) {
        return Save_GetPointer(animals[anm]);
    }
    return anm == MP_ANM_ISLAND ? Save_GetPointer(island.animal) : NULL;
}

static u32 mp_anm_off(int anm) {
    return (u32)((u8*)mp_anm_ptr(anm) - mp_save_base());
}

static int mp_anm_of(ACTOR* actor) {
    Animal_c* animal = ((NPC_ACTOR*)actor)->npc_info.animal;
    Animal_c* first = Save_GetPointer(animals[0]);

    if (animal >= first && animal < first + ANIMAL_NUM_MAX) {
        return (int)(animal - first);
    }
    return animal == Save_GetPointer(island.animal) ? MP_ANM_ISLAND : MP_ANM_NONE;
}

// the town is shared: a host with a visitor off the train, or a visitor in it
static int mp_town_shared(void) {
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

static void mp_host_send_guests(const u8* msg, int len, int except_slot) {
    int g;

    for (g = 1; g < MP_MAX_PEERS; g++) {
        if (g != except_slot && mp_lobby_guest_arrived(g) && mp_lobby_guest_conn(g) >= 0) {
            mp_lobby_send_rel(mp_lobby_guest_conn(g), msg, len);
        }
    }
}

// what repeats anyway skips a visitor gone quiet; the resync on their return catches them up
static void mp_host_send_live(const u8* msg, int len) {
    int g;

    for (g = 1; g < MP_MAX_PEERS; g++) {
        int conn = mp_lobby_guest_conn(g);

        if (mp_lobby_guest_arrived(g) && conn >= 0 && !mp_lobby_conn_quiet(conn)) {
            mp_lobby_send_rel(conn, msg, len);
        }
    }
}

static int mp_send_now(const u8* msg, int len) {
    int conn = mp_lobby_host_conn();

    if (conn < 0 || !mp_lobby_send_rel(conn, msg, len)) {
        if (conn >= 0) {
            pc_mp_log("[MP] npc: the line didn't take %02X", msg[0]);
        }
        return FALSE;
    }
    // (a talk's result in the line: what it handed over is the host's now)
    if (msg[0] == MP_M_TALK_END && len >= 6) {
        mp_world_talk_gone((u16)mp_get16(msg + 4));
    }
    return TRUE;
}

// the next result in pend whole, up to its end: FALSE when the line can't take all of it now
static int mp_pend_send_one(int conn) {
    int at = s_npc.pend_at;
    int bytes = 0;
    int n = 0;

    while (at + 2 <= s_npc.pend_safe) {
        int len = s_npc.pend[at] | (s_npc.pend[at + 1] << 8);
        int last = s_npc.pend[at + 2] == MP_M_TALK_END;

        at += 2 + len;
        bytes += len;
        n++;
        if (last) {
            break;
        }
    }
    if (n == 0 || !mp_lobby_rel_fits(conn, bytes, n)) {
        return FALSE;
    }
    while (s_npc.pend_at < at) {
        int len = s_npc.pend[s_npc.pend_at] | (s_npc.pend[s_npc.pend_at + 1] << 8);

        mp_send_now(s_npc.pend + s_npc.pend_at + 2, len);
        s_npc.pend_at += 2 + len;
    }
    return TRUE;
}

static void mp_plock_set_mine(u16 npc_id);
static void mp_lock_set_mine(u16 npc_id);

// results set down ahead of their passport go whole as the line has room, in order, while the host is heard from;
// once all are out their villagers are let go, and their bytes wait on the host's word from then
static void mp_pend_flush(void) {
    int conn = mp_lobby_host_conn();
    int k;

    while (s_npc.pend != NULL && s_npc.pend_at + 2 <= s_npc.pend_safe && mp_world_line_ok() &&
           mp_lobby_rel_room(conn) > 8 && mp_pend_send_one(conn)) {
    }
    for (k = 0; k < MP_PLOCKS; k++) {
        if (s_npc.held[k].anm != MP_ANM_NONE && s_npc.held[k].until_ms == 0 &&
            s_npc.held[k].pend_end <= s_npc.pend_at) {
            s_npc.held[k].until_ms = pc_mp_now_ms() + MP_TALK_DONE_MS;
        }
    }
    if (s_npc.pend_at < s_npc.pend_safe) {
        return;
    }
    s_npc.draining = FALSE;
    // (results put together since, which no card covers yet, wait their turn at the front: their villagers stay held)
    if (s_npc.pend_at < s_npc.pend_len) {
        memmove(s_npc.pend, s_npc.pend + s_npc.pend_at, s_npc.pend_len - s_npc.pend_at);
        for (k = 0; k < MP_PLOCKS; k++) {
            if (s_npc.held[k].anm != MP_ANM_NONE && s_npc.held[k].until_ms == 0) {
                s_npc.held[k].pend_end -= s_npc.pend_at;
            }
        }
        s_npc.pend_len -= s_npc.pend_at;
        s_npc.pend_at = 0;
        s_npc.pend_safe = 0;
        return;
    }
    s_npc.pend_len = 0;
    s_npc.pend_at = 0;
    s_npc.pend_safe = 0;
    // (the last talk's own result gone at last: the host's word on it ends that talk's wait)
    if (s_npc.pend_live) {
        s_npc.pend_live = FALSE;
        s_npc.defer_seq = s_npc.seq;
        s_npc.defer_until_ms = pc_mp_now_ms() + MP_TALK_DONE_MS;
        s_npc.defer_sent = TRUE;
    }
    if (!s_npc.result_due) {
        mp_plock_set_mine(0);
        if (s_npc.state == MP_TALK_IDLE) {
            mp_lock_set_mine(0);
        }
    }
}

// guest: no room to set down another result ahead of its passport (nor a villager slot to hold for it): a next talk
// waits
int mp_npc_talk_full(void) {
    int n = 0;
    int k;

    if (!s_npc.result_due && !s_npc.draining) {
        return FALSE;
    }
    if (s_npc.pend == NULL) {
        s_npc.pend = (u8*)mp_alloc(MP_TALK_PEND_BYTES);
    }
    for (k = 0; k < MP_PLOCKS; k++) {
        n += s_npc.held[k].anm != MP_ANM_NONE;
    }
    return s_npc.pend == NULL || s_npc.pend_len > MP_TALK_PEND_BYTES - 2 * MP_TALK_RESULT_MAX ||
           n + (s_npc.result_live ? 1 : 0) >= MP_PLOCKS;
}

int mp_npc_result_pending(void) {
    return s_npc.result_due || s_npc.draining;
}

static void mp_send_up(const u8* msg, int len) {
    if (s_npc.buffering) {
        if (!s_npc.dropping && s_npc.pend != NULL && s_npc.pend_len + 2 + len <= MP_TALK_PEND_BYTES) {
            s_npc.pend[s_npc.pend_len] = (u8)len;
            s_npc.pend[s_npc.pend_len + 1] = (u8)(len >> 8);
            memcpy(s_npc.pend + s_npc.pend_len + 2, msg, len);
            s_npc.pend_len += 2 + len;
            return;
        }
        // (no room to hold all of it: none of it goes, and what its talk handed over comes back)
        if (!s_npc.dropping) {
            pc_mp_log("[MP] npc: a talk's result too big to hold; its hand-overs come back");
            s_npc.pend_len = s_npc.res_start;
            s_npc.dropping = TRUE;
        }
        if (msg[0] == MP_M_TALK_END && len >= 6) {
            s_npc.dropping = FALSE;
            mp_world_talk_drop((u16)mp_get16(msg + 4));
        }
        return;
    }
    mp_send_now(msg, len);
}

// talk locks

// pending: the villager held for a talk's result on its way
static void mp_lock_send_ex(int slot, u16 npc_id, int except_slot, int pending) {
    u8 msg[5];

    msg[0] = npc_id != 0 ? MP_M_TALK_LOCK : MP_M_TALK_UNLOCK;
    msg[1] = (u8)slot;
    mp_put16(msg + 2, npc_id);
    msg[4] = (u8)pending;
    if (mp_is_host()) {
        mp_world_queue_guests(msg, 5, except_slot); // (behind the bytes the talk changed)
    } else {
        mp_send_now(msg, 5);
    }
}

static void mp_lock_send(int slot, u16 npc_id, int except_slot) {
    mp_lock_send_ex(slot, npc_id, except_slot, FALSE);
}

// a villager held for a result of this player's on its way; 0 lets all of them go
static void mp_plock_set_mine(u16 npc_id) {
    int self = mp_lobby_self_slot();
    int k;

    if (self < 0 || self >= MP_MAX_PEERS) {
        return;
    }
    if (npc_id == 0) {
        for (k = 0; k < MP_PLOCKS && s_npc.plock[self][k] == 0; k++) {
        }
        if (k < MP_PLOCKS) {
            memset(s_npc.plock[self], 0, sizeof(s_npc.plock[self]));
            mp_lock_send_ex(self, 0, -1, MP_PLOCK_ALL);
        }
        return;
    }
    for (k = 0; k < MP_PLOCKS; k++) {
        if (s_npc.plock[self][k] == npc_id) {
            return;
        }
    }
    k = s_npc.plock_next;
    s_npc.plock_next = (k + 1) % MP_PLOCKS;
    s_npc.plock[self][k] = npc_id;
    mp_lock_send_ex(self, npc_id, -1, 1 + k);
}

static int mp_plocked(int slot, u16 npc_id) {
    int k;

    for (k = 0; k < MP_PLOCKS; k++) {
        if (s_npc.plock[slot][k] == npc_id) {
            return TRUE;
        }
    }
    return FALSE;
}

static void mp_s2_own(u16 npc_id, int slot);

static void mp_lock_set_mine(u16 npc_id) {
    int self = mp_lobby_self_slot();

    if (self >= 0 && self < MP_MAX_PEERS && s_npc.lock[self] != npc_id) {
        s_npc.lock[self] = npc_id;
        mp_lock_send(self, npc_id, -1);
        mp_s2_own(npc_id, self);
    }
}

// another player talks to this character (two talks opened on it at once: the lower slot's goes on)
int mp_npc_talk_locked(unsigned short npc_id) {
    int self = mp_lobby_self_slot();
    int s;

    if (npc_id == 0 || !mp_town_shared()) {
        return FALSE;
    }
    for (s = 0; s < MP_MAX_PEERS; s++) {
        if (s != self && s_npc.lock[s] == npc_id &&
            (self < 0 || self >= MP_MAX_PEERS || s_npc.lock[self] != npc_id || s < self)) {
            return TRUE;
        }
        if (s != self && mp_plocked(s, npc_id)) {
            return TRUE;
        }
    }
    return FALSE;
}

// a hand-over a talk made outside the save's talk regions (the islander's furniture): with its result
int mp_npc_talk_extra(const unsigned char* msg, int len) {
    if (!mp_is_guest() || s_npc.state != MP_TALK_ON || len > 255 ||
        s_npc.extra_len + 1 + len > MP_TALK_EXTRA_BYTES) {
        return FALSE;
    }
    s_npc.extra[s_npc.extra_len] = (u8)len;
    memcpy(s_npc.extra + s_npc.extra_len + 1, msg, len);
    s_npc.extra_len += 1 + len;
    return TRUE;
}

// guest: the talk's result

static void mp_rgn_capture(u8* out) {
    int r;

    for (r = 0; r < MP_TALK_RGN; r++) {
        memcpy(out, mp_save_base() + s_talk_rgn[r].off, s_talk_rgn[r].size);
        out += s_talk_rgn[r].size;
    }
}

// runs of bytes that differ between base and now, as {save offset, length, bytes before, bytes after}: the host takes
// a change only where it still has what the talk began from (several characters share bytes of event records, and
// another talk may have changed one meanwhile)
static void mp_send_runs(u32 save_off, const u8* base, const u8* now, u32 size) {
    u8 msg[8 + MP_RUN_PAYLOAD + 8];
    int n = 0;
    u32 i = 0;

    msg[0] = MP_M_TALK_RUNS;
    msg[1] = (u8)s_npc.anm;
    mp_put16(msg + 2, s_npc.npc_id);
    msg[4] = 0;
    while (i < size) {
        u32 len = 0;

        if (base[i] == now[i]) {
            i++;
            continue;
        }
        while (i + len < size && len < 150 && base[i + len] != now[i + len]) {
            len++;
        }
        if (5 + n + 5 + 2 * len > MP_RUN_PAYLOAD) {
            mp_send_up(msg, 5 + n);
            n = 0;
        }
        mp_put32(msg + 5 + n, save_off + i);
        msg[5 + n + 4] = (u8)len;
        memcpy(msg + 5 + n + 5, base + i, len);
        memcpy(msg + 5 + n + 5 + len, now + i, len);
        mp_world_heard_mine(save_off + i, len);
        n += 5 + 2 * (int)len;
        i += len;
    }
    if (n > 0) {
        mp_send_up(msg, 5 + n);
    }
}

// guest: what the host changed while this game held its bytes back comes in now, around what the talk
// itself changed (the host has that already)
static void mp_catch_up_span(u32 off, const u8* base, u32 size) {
    const u8* heard = mp_world_heard(off, size);
    u8* live = mp_save_base() + off;
    u32 k;

    for (k = 0; heard != NULL && k < size; k++) {
        if (live[k] == base[k] && live[k] != heard[k]) {
            live[k] = heard[k];
            s_npc.refresh = TRUE;
        }
    }
}

static void mp_talk_catch_up(void) {
    const u8* base = s_npc.rgn_base;
    int r;

    for (r = 0; r < MP_TALK_RGN; r++) {
        mp_catch_up_span(s_talk_rgn[r].off, base, s_talk_rgn[r].size);
        base += s_talk_rgn[r].size;
    }
    if (s_npc.defer_anm != MP_ANM_NONE && mp_anm_ptr(s_npc.defer_anm) != NULL) {
        mp_catch_up_span(mp_anm_off(s_npc.defer_anm), (const u8*)&s_npc.base, sizeof(Animal_c));
    }
}

// the talk changed something the host keeps: its villager, the save's talk regions, a hand-over outside them
static int mp_talk_shared_change(void) {
    Animal_c* animal = mp_anm_ptr(s_npc.anm);
    u8 now[MP_TALK_RGN_BYTES];

    if (s_npc.extra_len > 0 || (animal != NULL && memcmp(&s_npc.base, animal, sizeof(Animal_c)) != 0)) {
        return TRUE;
    }
    mp_rgn_capture(now);
    return memcmp(now, s_npc.rgn_base, sizeof(now)) != 0;
}

static void mp_talk_send_result(void) {
    Animal_c* animal = mp_anm_ptr(s_npc.anm);
    u8 now[MP_TALK_RGN_BYTES];
    u8 msg[9 + sizeof(Anmmem_c)];
    int r;
    int at = 0;

    s_npc.res_start = s_npc.pend_len;
    s_npc.dropping = FALSE;
    if (animal != NULL) {
        int mine = mp_travel_state() == MP_TRAVEL_VISITING && Now_Private != NULL
                       ? mNpc_GetAnimalMemoryIdx(&Now_Private->player_ID, animal->memories, ANIMAL_MEMORY_NUM)
                       : -1;

        // the villager's own fields; its memories belong to the host except our own
        mp_send_runs(mp_anm_off(s_npc.anm) + MP_MEM_END, (const u8*)&s_npc.base + MP_MEM_END,
                     (const u8*)animal + MP_MEM_END, sizeof(Animal_c) - MP_MEM_END);
        if (mine >= 0 && memcmp(&s_npc.base.memories[mine], &animal->memories[mine], sizeof(Anmmem_c)) != 0) {
            msg[0] = MP_M_TALK_MEM;
            msg[1] = (u8)s_npc.anm;
            mp_put16(msg + 2, s_npc.npc_id);
            msg[4] = (u8)mine;
            mp_put32(msg + 5, mp_crc32(&s_npc.base.memories[mine], sizeof(Anmmem_c), 0));
            memcpy(msg + 9, &animal->memories[mine], sizeof(Anmmem_c));
            mp_send_up(msg, (int)sizeof(msg));
        }
    }
    mp_rgn_capture(now);
    for (r = 0; r < MP_TALK_RGN; r++) {
        mp_send_runs(s_talk_rgn[r].off, s_npc.rgn_base + at, now + at, s_talk_rgn[r].size);
        at += (int)s_talk_rgn[r].size;
    }
    for (r = 0; r < s_npc.extra_len; r += 1 + s_npc.extra[r]) {
        mp_send_up(s_npc.extra + r + 1, s_npc.extra[r]);
    }
    s_npc.extra_len = 0;
    msg[0] = MP_M_TALK_END;
    msg[1] = (u8)s_npc.anm;
    mp_put16(msg + 2, s_npc.npc_id);
    mp_put16(msg + 4, s_npc.seq);
    mp_send_up(msg, 6);
    if (!s_npc.buffering) {
        s_npc.defer_seq = s_npc.seq;
        s_npc.defer_until_ms = pc_mp_now_ms() + MP_TALK_DONE_MS;
        s_npc.defer_sent = TRUE;
    }
}

// talk tracking

static int mp_npc_idx(ACTOR* actor);

static void mp_talk_begin(ACTOR* actor) {
    Animal_c* animal;

    // (the last talk's result still going out: its villager's bytes keep waiting, as for one put together ahead)
    if (s_npc.pend_live) {
        int k;

        s_npc.pend_live = FALSE;
        for (k = 0; k < MP_PLOCKS - 1 && s_npc.held[k].anm != MP_ANM_NONE; k++) {
        }
        s_npc.held[k].anm = s_npc.anm;
        s_npc.held[k].seq = s_npc.seq;
        s_npc.held[k].until_ms = 0;
        s_npc.held[k].pend_end = s_npc.pend_len;
    }
    // (a result still waiting on the passport is put together now, to go once it can; its villager stays held, and
    // its bytes from the host keep waiting)
    if (s_npc.result_due && s_npc.result_live) {
        int self = mp_lobby_self_slot();
        int k;

        if (s_npc.pend == NULL) {
            s_npc.pend = (u8*)mp_alloc(MP_TALK_PEND_BYTES);
        }
        s_npc.buffering = TRUE;
        mp_talk_send_result();
        s_npc.buffering = FALSE;
        s_npc.result_live = FALSE;
        if (self >= 0 && self < MP_MAX_PEERS && s_npc.lock[self] != 0) {
            mp_plock_set_mine(s_npc.lock[self]);
        }
        for (k = 0; k < MP_PLOCKS - 1 && s_npc.held[k].anm != MP_ANM_NONE; k++) {
        }
        s_npc.held[k].anm = s_npc.anm;
        s_npc.held[k].seq = s_npc.seq;
        s_npc.held[k].until_ms = 0;
        s_npc.held[k].pend_end = s_npc.pend_len;
    }
    s_npc.extra_len = 0;
    s_npc.defer_sent = FALSE;
    s_npc.state = MP_TALK_ON;
    s_npc.settle_still = 0;
    s_npc.actor = actor;
    s_npc.npc_id = actor->npc_id;
    s_npc.anm = mp_anm_of(actor);
    s_npc.seq++;
    animal = mp_anm_ptr(s_npc.anm);
    if (animal != NULL) {
        s_npc.base = *animal;
    }
    mp_rgn_capture(s_npc.rgn_base);
    // (each game keeps its own Porter, Kapp'n, shopkeepers and such: nobody else waits on them)
    if (mp_npc_idx(actor) >= 0) {
        mp_lock_set_mine(s_npc.npc_id);
    }
    if (mp_is_guest()) {
        s_npc.deferring = TRUE;
        s_npc.defer_anm = s_npc.anm;
        s_npc.defer_until_ms = 0;
        mp_world_talk_begin();
    }
}

static void mp_talk_send_pending(void);

// guest: the talk's result (and its lock) go once the passport has what the talk handed over, so a game lost meanwhile
// can't have given something the host went on to take
static void mp_talk_result_tick(void) {
    int ok;

    if (!s_npc.result_due) {
        return;
    }
    if ((s32)(mp_passport_good_seq() - s_npc.result_floor) > 0) {
        mp_talk_send_pending();
        return;
    }
    // (one is written; none staged meanwhile can be, while the passport waits on the host's save; one that doesn't
    // make it is tried again a while on, and the result never goes ahead of it)
    if (s_npc.result_seq != 0 && mp_passport_written(s_npc.result_seq, &ok)) {
        s_npc.result_seq = 0;
        s_npc.result_retry_ms = ok ? pc_mp_now_ms() : pc_mp_now_ms() + MP_PP_RETRY_MS;
    }
    if (s_npc.result_seq == 0 && (s32)(pc_mp_now_ms() - s_npc.result_retry_ms) >= 0) {
        s_npc.result_seq = mp_passport_write_async();
    }
}

// leaving: the results a passport on the card covers go into the line now, whole (a few moments for a full line to
// make room)
void mp_npc_drain_all(void) {
    int i;

    if (!mp_is_guest()) {
        return;
    }
    if (s_npc.result_due && (s32)(mp_passport_good_seq() - s_npc.result_floor) > 0) {
        mp_talk_send_pending();
    }
    for (i = 0; i < 30 && s_npc.draining && s_npc.pend != NULL && mp_world_line_ok(); i++) {
        while (s_npc.pend_at + 2 <= s_npc.pend_safe && mp_pend_send_one(mp_lobby_host_conn())) {
        }
        if (s_npc.pend_at + 2 > s_npc.pend_safe) {
            mp_pend_flush(); // (all out: its villagers let go)
            break;
        }
        mp_lobby_pump();
    }
}

// the way home or the app closing: the talks whose results never went give back what they handed over, and those
// results stay here (card_seq: priv is that passport on the card, not the pockets)
int mp_npc_undo(void* priv, unsigned int card_seq) {
    int changed;

    if (!mp_is_guest()) {
        return FALSE;
    }
    changed = mp_world_talk_undo(priv, card_seq);
    s_npc.result_due = FALSE;
    s_npc.result_live = FALSE;
    s_npc.draining = FALSE;
    s_npc.pend_live = FALSE;
    s_npc.pend_len = 0;
    s_npc.pend_at = 0;
    s_npc.pend_safe = 0;
    return changed;
}

// the results the passport now covers go (put together ones first, the talk just over after them); their villagers
// are let go once the line has taken them all (a talk under way or winding down keeps its own villager)
static void mp_talk_send_pending(void) {
    s_npc.result_due = FALSE;
    // (nowhere to hold it: straight out)
    if (s_npc.pend == NULL) {
        if (s_npc.result_live) {
            s_npc.result_live = FALSE;
            mp_talk_send_result();
        }
        mp_plock_set_mine(0);
        if (s_npc.state == MP_TALK_IDLE) {
            mp_lock_set_mine(0);
        }
        return;
    }
    if (s_npc.result_live) {
        s_npc.result_live = FALSE;
        s_npc.buffering = TRUE;
        mp_talk_send_result();
        s_npc.buffering = FALSE;
        s_npc.pend_live = TRUE;
    }
    s_npc.pend_safe = s_npc.pend_len;
    s_npc.draining = TRUE;
    mp_pend_flush();
}

// host: what its own talk changed goes out ahead of the unlock, so a next talk anywhere starts from it
static void mp_host_talk_push(void) {
    int r;
    int at = 0;

    if (mp_anm_ptr(s_npc.anm) != NULL) {
        mp_world_host_took_diff(mp_anm_off(s_npc.anm), &s_npc.base, sizeof(Animal_c), -1);
    }
    for (r = 0; r < MP_TALK_RGN; r++) {
        mp_world_host_took_diff(s_talk_rgn[r].off, s_npc.rgn_base + at, s_talk_rgn[r].size, -1);
        at += (int)s_talk_rgn[r].size;
    }
}

static void mp_talk_finish(void) {
    s_npc.state = MP_TALK_IDLE;
    s_npc.actor = NULL;
    if (mp_is_guest()) {
        s_npc.result_due = TRUE;
        s_npc.result_live = TRUE;
        s_npc.result_seq = 0;
        s_npc.result_retry_ms = pc_mp_now_ms();
        s_npc.result_floor = mp_passport_staged_seq();
        mp_world_talk_end(s_npc.seq, s_npc.result_floor, mp_talk_shared_change());
        mp_talk_result_tick();
    } else {
        if (mp_is_host()) {
            mp_host_talk_push();
        }
        mp_lock_set_mine(0);
    }
}

// m_demo.c: a talk with a character opens this frame; its set-up callback can write the save before the frame's
// look at the talks, so the before-picture is taken now (a talk still winding down is finished first)
void mp_npc_talk_opening(void) {
    ACTOR* talk = mDemo_Get_talk_actor();

    if (!mp_town_shared() || talk == NULL || talk->part != ACTOR_PART_NPC ||
        (s_npc.state == MP_TALK_ON && s_npc.actor == talk)) {
        return;
    }
    if (s_npc.state != MP_TALK_IDLE) {
        mp_talk_finish();
    }
    mp_talk_begin(talk);
}

// a guest's villagers point at their memory of the local player; keep them on it
static void mp_refresh_friendship(GAME_PLAY* play) {
    ACTOR* actor;

    for (actor = play->actor_info.list[ACTOR_PART_NPC].actor; actor != NULL; actor = actor->next_actor) {
        NPC_ACTOR* npc = (NPC_ACTOR*)actor;
        Animal_c* animal;
        int idx;

        if (ITEM_NAME_GET_TYPE(actor->npc_id) != NAME_TYPE_NPC || mp_anm_of(actor) == MP_ANM_NONE ||
            Now_Private == NULL) {
            continue;
        }
        animal = npc->npc_info.animal;
        idx = mNpc_GetAnimalMemoryIdx(&Now_Private->player_ID, animal->memories, ANIMAL_MEMORY_NUM);
        npc->condition_info.friendship = (idx != -1) ? &animal->memories[idx].friendship : NULL;
    }
}

// a talk's show goes on after its window closes (K.K.'s set, a gift handed over): the talker's game
// keeps the character, and holds the host's bytes for it, until the player is free again
static int mp_npc_show_held(GAME_PLAY* play) {
    int idx = mPlib_get_player_actor_main_index((GAME*)play);

    return mEv_CheckTitleDemo() == mEv_TITLEDEMO_STAFFROLL || idx == mPlayer_INDEX_DEMO_WAIT ||
           idx == mPlayer_INDEX_WASH_CAR;
}

// before the actors: the talk window mDemo_Main just opened or closed
void mp_npc_frame(GAME_PLAY* play) {
    ACTOR* talk;

    if (!mp_town_shared()) {
        int self = mp_lobby_self_slot();

        if (s_npc.state != MP_TALK_IDLE || s_npc.deferring) {
            s_npc.state = MP_TALK_IDLE;
            s_npc.actor = NULL;
            s_npc.deferring = FALSE;
        }
        // (the trip's over: the way home waited for these, or went home as the passport last stood without them)
        s_npc.result_due = FALSE;
        s_npc.result_live = FALSE;
        s_npc.draining = FALSE;
        s_npc.pend_live = FALSE;
        s_npc.pend_len = 0;
        s_npc.pend_at = 0;
        s_npc.pend_safe = 0;
        s_npc.extra_len = 0;
        s_npc.dropping = FALSE;
        mp_world_talk_reset();
        {
            int k;

            for (k = 0; k < MP_PLOCKS; k++) {
                s_npc.held[k].anm = MP_ANM_NONE;
            }
        }
        // a visitor boarding home in Porter's talk lets the others talk to him again
        if (mp_is_guest() && self > 0 && self < MP_MAX_PEERS && s_npc.lock[self] != 0) {
            mp_lock_send(self, 0, -1);
        }
        if (mp_is_guest() && self > 0 && self < MP_MAX_PEERS) {
            mp_plock_set_mine(0);
        }
        memset(s_npc.lock, 0, sizeof(s_npc.lock));
        memset(s_npc.plock, 0, sizeof(s_npc.plock));
        return;
    }
    if (s_npc.pend == NULL && mp_is_guest()) {
        s_npc.pend = (u8*)mp_alloc(MP_TALK_PEND_BYTES);
    }
    mp_talk_result_tick();
    talk = mDemo_Get_talk_actor();
    if (talk != NULL && talk->part != ACTOR_PART_NPC) {
        talk = NULL;
    }
    // (a talk opened on a character another player's talk took first ends before it begins)
    if (talk != NULL && s_npc.state == MP_TALK_ON && talk == s_npc.actor && mp_npc_idx(talk) >= 0 &&
        mp_npc_talk_locked(talk->npc_id) &&
        ((NPC_ACTOR*)talk)->condition_info.talk_condition == aNPC_TALK_TYPE_NONE) {
        mDemo_End(talk);
    }
    if (talk == NULL && s_npc.state == MP_TALK_ON && mp_npc_show_held(play)) {
        talk = s_npc.actor;
    }
    if (s_npc.state == MP_TALK_ON && talk != s_npc.actor) {
        s_npc.state = MP_TALK_SETTLING;
        s_npc.settle = MP_TALK_SETTLE_FRAMES;
    }
    if (s_npc.state == MP_TALK_SETTLING && (talk != NULL || --s_npc.settle <= 0)) {
        mp_talk_finish();
    }
    if (s_npc.state == MP_TALK_IDLE && talk != NULL) {
        mp_talk_begin(talk);
    }
    if (s_npc.draining) {
        mp_pend_flush();
    }
    if (s_npc.deferring && s_npc.state == MP_TALK_IDLE && s_npc.defer_until_ms != 0 && s_npc.defer_sent &&
        (s32)(pc_mp_now_ms() - s_npc.defer_until_ms) > 0) {
        mp_talk_catch_up();
        s_npc.deferring = FALSE;
    }
    {
        int k;

        for (k = 0; k < MP_PLOCKS; k++) {
            if (s_npc.held[k].anm != MP_ANM_NONE && s_npc.held[k].until_ms != 0 &&
                (s32)(pc_mp_now_ms() - s_npc.held[k].until_ms) > 0) {
                s_npc.held[k].anm = MP_ANM_NONE;
            }
        }
    }
    if (s_npc.refresh) {
        s_npc.refresh = FALSE;
        mp_refresh_friendship(play);
    }
}

// guest: bytes from the host that would undo our unsent talk
int mp_npc_defers(unsigned int save_off, unsigned int len) {
    int r;

    for (r = 0; r < MP_PLOCKS; r++) {
        if (s_npc.held[r].anm != MP_ANM_NONE) {
            u32 at = mp_anm_off(s_npc.held[r].anm);

            if (save_off < at + sizeof(Animal_c) && save_off + len > at) {
                return TRUE;
            }
        }
    }
    if (!s_npc.deferring) {
        return FALSE;
    }
    for (r = 0; r < MP_TALK_RGN; r++) {
        if (save_off < s_talk_rgn[r].off + s_talk_rgn[r].size && save_off + len > s_talk_rgn[r].off) {
            return TRUE;
        }
    }
    if (s_npc.defer_anm != MP_ANM_NONE) {
        u32 at = mp_anm_off(s_npc.defer_anm);

        if (save_off < at + sizeof(Animal_c) && save_off + len > at) {
            return TRUE;
        }
    }
    return FALSE;
}

void mp_npc_animals_changed(void) {
    s_npc.refresh = TRUE;
}

// host: merging a result

static int mp_span_in(u32 off, u32 len, u32 lo, u32 hi) {
    return off >= lo && off <= hi && len <= hi - off;
}

static int mp_host_run_allowed(int anm, u32 off, u32 len) {
    int r;

    for (r = 0; r < MP_TALK_RGN; r++) {
        if (mp_span_in(off, len, s_talk_rgn[r].off, s_talk_rgn[r].off + s_talk_rgn[r].size)) {
            return TRUE;
        }
    }
    if (mp_anm_ptr(anm) != NULL) {
        u32 at = mp_anm_off(anm);

        // never the villager's identity or anyone's memories; those go through TALK_MEM
        return mp_span_in(off, len, at + MP_MEM_END, at + sizeof(Animal_c));
    }
    return FALSE;
}

static int mp_host_villager_ok(int anm, u16 npc_id) {
    Animal_c* animal = mp_anm_ptr(anm);

    return animal == NULL || animal->id.npc_id == npc_id;
}

// a visitor's talk never leaves codes in the town's text (shown on a GameCube they'd run as a message's own)
static void mp_host_text_scrub(int anm) {
    Animal_c* animal = mp_anm_ptr(anm);
    int i;

    if (animal != NULL) {
        mp_text_clean(animal->catchphrase, ANIMAL_CATCHPHRASE_LEN);
        mp_text_clean(animal->parent_name, PLAYER_NAME_LEN);
    }
    for (i = 0; i < mFR_RECORD_NUM; i++) {
        mp_text_clean(Save_Get(fishRecord[i]).pid.player_name, PLAYER_NAME_LEN);
        mp_text_clean(Save_Get(fishRecord[i]).pid.land_name, LAND_NAME_SIZE);
    }
}

static void mp_host_on_runs(int from, const u8* p, int len) {
    int anm = p[1];
    int at = 5;

    if (!mp_host_villager_ok(anm, (u16)mp_get16(p + 2))) {
        return;
    }
    while (at + 5 <= len) {
        u32 off = mp_get32(p + at);
        u32 n = p[at + 4];
        const u8* was = p + at + 5;
        const u8* now = was + n;
        u32 k;

        if (at + 5 + 2 * (int)n > len) {
            break;
        }
        if (mp_span_in(off, n, offsetof(Save_t, museum_display), offsetof(Save_t, museum_display) +
                                                                     MP_FSIZE(museum_display))) {
            u8 d[256];

            // a donation fills a spot only while it's still empty here, and never clears one
            for (k = 0; k < n; k++) {
                d[k] = was[k] ^ now[k];
            }
            mp_rights_museum_run(from, off, d, n);
        } else if (mp_host_run_allowed(anm, off, n)) {
            u8* live = mp_save_base() + off;
            int same = TRUE;

            // the talk's changes land where the host still has what it began from (a change made here meanwhile
            // stays, and goes back to the talker); the others have it before they hear the talk is over
            for (k = 0; k < n; k++) {
                live[k] ^= (u8)((was[k] ^ now[k]) & ~(live[k] ^ was[k]));
                same &= live[k] == now[k];
            }
            mp_host_text_scrub(anm);
            same &= memcmp(live, now, n) == 0;
            mp_world_host_took(off, n, same ? from : -1);
        }
        at += 5 + 2 * (int)n;
    }
}

// a visitor's memory lands in its own slot: the one it already has, the one it took if nobody
// else has since, or a fresh one the host picks
static void mp_host_on_mem(const u8* p, int len) {
    Animal_c* animal = mp_anm_ptr(p[1]);
    Anmmem_c entry;
    int idx;

    if (len < 9 + (int)sizeof(Anmmem_c) || animal == NULL || !mp_host_villager_ok(p[1], (u16)mp_get16(p + 2)) ||
        p[4] >= ANIMAL_MEMORY_NUM) {
        return;
    }
    memcpy(&entry, p + 9, sizeof(entry));
    // (its name, town and letter are text only in the town's save)
    mp_text_clean(entry.memory_player_id.player_name, PLAYER_NAME_LEN);
    mp_text_clean(entry.memory_player_id.land_name, LAND_NAME_SIZE);
    if (p[1] < ANIMAL_NUM_MAX) {
        mp_text_clean(entry.memuni.land.name, LAND_NAME_SIZE);
    }
    mFont_clean_save_text(entry.letter.header, MAIL_HEADER_LEN);
    mFont_clean_save_text(entry.letter.body, MAIL_BODY_LEN);
    mFont_clean_save_text(entry.letter.footer, MAIL_FOOTER_LEN);
    idx = mNpc_GetAnimalMemoryIdx(&entry.memory_player_id, animal->memories, ANIMAL_MEMORY_NUM);
    if (idx < 0) {
        idx = (mp_crc32(&animal->memories[p[4]], sizeof(Anmmem_c), 0) == mp_get32(p + 5))
                  ? p[4]
                  : mNpc_ForceGetFreeAnimalMemoryIdx(animal, animal->memories, ANIMAL_MEMORY_NUM);
    }
    if (idx >= 0 && idx < ANIMAL_MEMORY_NUM) {
        animal->memories[idx] = entry;
        s_npc.refresh = TRUE;
    }
}


// who moves each villager: the nearest player in its acre runs its AI, the rest play its poses;
// visitors and event characters out in town are shared the same way

#define MP_NPC_N         (ANIMAL_NUM_MAX + 1) // the island's villager is the last
#define MP_NOBODY        0xFF                 // each game moves its own copy
#define MP_POSE_TICK     3                    // frames between looks at the poses; a changed one goes out
#define MP_POSE_KEEP_MS  400                  // an unchanged one goes out this often
#define MP_POSE_DRIFT    4.0f                 // units a follower's guess may stray before a fresh report
#define MP_POSE_AHEAD    40                   // frames a follower carries a report on at most
#define MP_HAVE_EVERY    60                   // visitor: frames between repeats of an unchanged list
#define MP_HAVE_MS       1000                 // ...or this long, a menu standing the frames still
#define MP_CLAIM_HOLD_MS 1000                 // guest: a claim stands this long against older owner lists
#define MP_CLAIM_KEEP_MS 3000                 // host: a granted claim stands this long against the distance pass
#define MP_STILL_SETTLE  300                  // frames a talk's wind-down waits through a menu at most
#define MP_POSE_STALE_MS 3000                 // silent this long: this game moves it again
#define MP_OWNER_EVERY   15                   // host: frames between ownership passes
#define MP_OWNER_RESEND  5000
#define MP_OWNER_KEEP    2.25f // (1.5 squared) a challenger has to be this much closer
#define MP_LIST_MS       2000
#define MP_POSE_WIRE     39
#define MP_NPC_FX_BODY   26

// what the game moving a villager decides for it; the rest of Animal_c is the host's or a talk's
#define MP_OWN_BEGIN offsetof(Animal_c, mood)
#define MP_OWN_LEN   (offsetof(Animal_c, cloth_original_id) + 1 - MP_OWN_BEGIN)
#define MP_OWN_AT(f) (offsetof(Animal_c, f) - MP_OWN_BEGIN)
#define MP_OWN_N     6

static const u8 s_own_at[MP_OWN_N] = {
    MP_OWN_AT(mood), MP_OWN_AT(mood_time), MP_OWN_AT(cloth), MP_OWN_AT(cloth) + 1, MP_OWN_AT(is_home),
    MP_OWN_AT(cloth_original_id),
};
#define MP_OWN_HOME 4 // is_home, in the list above
#define MP_OWN_CLOTH 2 // cloth's two bytes, in the list above
#define MP_OWN_ORIG  5 // cloth_original_id
#define MP_STAND_STALE_MS 3000

typedef struct {
    u16 id;
    u8 host_only; // its own logic keeps the event's save record, which only the host's copy may write
    u16 scene;    // where it's shared: out in town (0), or inside the igloo or the summer camp's tent
} mp_sp_t;

#define MP_SP5(a, h) { (a), (h) }, { (a) + 1, (h) }, { (a) + 2, (h) }, { (a) + 3, (h) }, { (a) + 4, (h) }

// shared besides villagers; those with their own camera, train, boat or shop (Porter, Kapp'n, the shopkeepers,
// Resetti) stay with each game, as does aerobics, danced to each game's own music, and the ones whose scenes are each
// player's own (the countdown's announcer, Groundhog Day's speaker)
static const mp_sp_t s_sp[] = {
    { SP_NPC_ARTIST, 0 },
    { SP_NPC_CARPETPEDDLER, 0 },
    { SP_NPC_KABUPEDDLER, 0 },
    { SP_NPC_SANTA, 0 },
    { SP_NPC_HALLOWEEN, 0 },
    { SP_NPC_MASK_CAT, 0 },
    { SP_NPC_EV_DOKUTU, 0 },
    { SP_NPC_EV_DOZAEMON, 0 },
    { SP_NPC_DESIGNER, 0 },
    { SP_NPC_ANGLER, 0 },
    { SP_NPC_TURKEY, 0 },
    { SP_NPC_EV_SONCHO, 0 },
    { SP_NPC_SONCHO, 0 },
    { SP_NPC_EV_MIKO, 0 },
    { SP_NPC_HEM, 0 },
    { SP_NPC_EV_GHOST, 1 },
    { SP_NPC_TOTAKEKE, 1 },
    { SP_NPC_EV_SONCHO2, 1 },
    { SP_NPC_SONCHO_D079, 1 },
    { SP_NPC_EV_YOMISE, 1 },
    { SP_NPC_EV_YOMISE2, 1 },
    { SP_NPC_POST_MAN, 1 }, // his deliveries write the post office and mailboxes
    { SP_NPC_BROKER, 0 },       // Redd outside his tent
    { SP_NPC_GO_HONE_NPC, 0 },  // a villager who moved away, back for a visit
    { SP_NPC_PRESENT_NPC, 0 },  // a present brought to one player's door: the others see it come
    { SP_NPC_POLICE, 0 },       // Copper at his post: his dozing and his morning exercises are one for everyone
    { SP_NPC_EV_SUMMERCAMP_0, 0, SCENE_TENT },
    MP_SP5(SP_NPC_EV_HALLOWEEN_0, 0),
    MP_SP5(SP_NPC_EV_HANABI_0, 0),
    MP_SP5(SP_NPC_EV_HANAMI_0, 0),
    MP_SP5(SP_NPC_EV_TUKIMI_0, 0),
    { SP_NPC_EV_COUNTDOWN_0 + 1, 0 },
    { SP_NPC_EV_COUNTDOWN_0 + 2, 0 },
    { SP_NPC_EV_COUNTDOWN_0 + 3, 0 },
    { SP_NPC_EV_COUNTDOWN_0 + 4, 0 },
    MP_SP5(SP_NPC_EV_TURI_0, 0),
    MP_SP5(SP_NPC_EV_TAMAIRE_0, 0),
    MP_SP5(SP_NPC_EV_HARVEST_0, 0),
    MP_SP5(SP_NPC_EV_GROUNDHOG_0, 0),
    MP_SP5(SP_NPC_EV_TOKYOSO_0, 1),
    MP_SP5(SP_NPC_EV_TUNAHIKI_0, 1),
    MP_SP5(SP_NPC_EV_HATUMODE_0, 1),
    { SP_NPC_EV_KAMAKURA_0, 0, SCENE_KAMAKURA },
};

#define MP_SP_N     (int)(sizeof(s_sp) / sizeof(s_sp[0]))
#define MP_NPC_ALL  (MP_NPC_N + MP_SP_N)
#define MP_SP_BYTES ((MP_SP_N + 7) / 8)
#define MP_POSE_HDR (5 + MP_SP_BYTES)

typedef char mp_npc_all_fits[(MP_NPC_ALL < MP_NOBODY && 2 + MP_NPC_ALL + 3 + 2 <= MP_REL_MAX) ? 1 : -1];

typedef struct {
    mp_npose_t pose;
    u8 valid;
    u8 due;    // host: the visitors it still goes to
    u16 scene; // where its owner's game shows it
    u32 ms;
    int frame; // when it came in
} mp_seen_t;

// a character this game moves: as it looks now, and as the others last heard it
typedef struct {
    mp_npose_t pose;
    u8 have;
    u8 due;   // the peers it still goes to
    u8 again; // a change goes out twice, in case one is lost
    u32 key;
    int frame;
    mp_npose_t sent;
    int sent_frame;
    u32 sent_ms;
} mp_mine_t;

static struct {
    u8 owner[MP_NPC_ALL];
    u8 owner_sent[MP_NPC_ALL];
    u32 owner_sent_ms;
    mp_seen_t seen[MP_NPC_ALL]; // latest report of each character from its owner
    mp_mine_t mine[MP_NPC_ALL]; // the ones this game moves
    u8 have_sent[MP_SP_BYTES];  // visitor: the event characters last listed to the host
    int have_sent_frame;
    u32 have_sent_ms;
    u32 claim_ms[MP_NPC_ALL];   // guest: when this game claimed it, until the host's list agrees; host: granted
    ACTOR* standin[MP_NPC_ALL]; // a hidden copy of another player's character: this screen doesn't have it
    u8 owner_heard[MP_NPC_ALL]; // guest: the host's latest list, for a claim it turned down
    int still_frame;            // frames run while a menu has the world stopped
    u8 menu_ran;                // host: its event characters went on behind its menu last frame
    u8 ev_owner[MP_NPC_ALL];    // host: the screen running each of its event characters for everyone
    u8 ev_group[3];             // the screen running each grouped event (MP_NOBODY: none has it out), the host's say
    u8 ev_group_sent[3];
    u8 walker;                  // the screen stepping the villagers nobody is watching, the host's say
    u8 walker_sent;
    u8 mail;                    // the screen whose game brings Pete by, the host's say
    u8 mail_sent;
    u8 force_told;              // host: the screen told Pete is due early
    u32 stand_ms[mNW_CLOTH_DESIGN_NUM]; // host: when each cloth stand at the Able Sisters' last changed
    u32 walk_ms;                // visitor stepping them: when it last told the host where they are
    u8 ev_rec[3][sizeof(((mEv_area_c*)0)->data)]; // visitor: its own copy of each grouped event's record
    u8 ev_rec_have[3];
    u8 ev_sent[3][sizeof(((mEv_area_c*)0)->data) + 6 * 8]; // what a follower would notice of each, as last sent
    int ev_sent_frame;
    u32 ev_sent_ms;
    u8 ev_torn[3]; // this screen's copies of the event are going (its first character left)
    mp_evstep_t ev_step[3][6]; // each grouped event's control [0] and characters as the screen running it has them
    u8 ev_step_have[3];
    u8 ev_step_new[3];            // in since this screen's following copies were set to them
    u8 ev_ctl_follow[3];          // this screen's control followed another screen last frame
    u8 ev_npc_follow[MP_NPC_ALL]; // this screen's copy of an event's character followed its runner elsewhere
    mp_evstep_t ev_held[MP_NPC_ALL]; // talker: an event character of another screen's, as its talk here left it
    u8 ev_held_have[MP_NPC_ALL];
    mp_evstep_t ev_pend[MP_NPC_ALL]; // runner: one handed back after a talk elsewhere, to carry on from
    u32 ev_pend_ms[MP_NPC_ALL];
    // single event characters (K.K., Tortimer): the step each had as the screen running it last told
    mp_evstep_t ev_sstep[MP_NPC_ALL];
    u8 ev_sstep_have[MP_NPC_ALL];
    u8 ev_sstep_new[MP_NPC_ALL];
    u32 ev_sstep_ms[MP_NPC_ALL];
    u8 ev_ssent[MP_NPC_ALL][8]; // what a follower would notice of each, as last sent
    ACTOR* actor[MP_NPC_ALL];   // this game's actor for each
    u8 puppet[MP_NPC_ALL];
    u8 lsnd[MP_NPC_ALL];                // a sound its own logic kept up this frame
    u8 have[MP_MAX_PEERS][MP_SP_BYTES]; // host: the event characters out on each visitor's screen
    u32 have_ms[MP_MAX_PEERS];
    int fx_idx; // the character whose own logic runs right now, when this game moves it for everyone
    int frame;
    u32 list_ms;
} s_s2;

static void mp_s2_reset(void) {
    memset(&s_s2, 0, sizeof(s_s2));
    memset(s_s2.owner, MP_NOBODY, sizeof(s_s2.owner));
    memset(s_s2.owner_sent, MP_NOBODY, sizeof(s_s2.owner_sent));
    memset(s_s2.ev_owner, MP_NOBODY, sizeof(s_s2.ev_owner));
    memset(s_s2.ev_group, MP_NOBODY, sizeof(s_s2.ev_group));
    s_s2.walker = s_s2.walker_sent = MP_NOBODY;
    s_s2.fx_idx = -1;
}

// the host's events run by a group of characters (a race, a tug of war, the shrine line): the group runs
// whole on one screen, whose record of the event is the town's
static const struct {
    u16 first; // the event's characters, from here
    u8 n;
    u8 id; // its save area
    s16 type;
} s_ev_area[] = {
    { SP_NPC_EV_TOKYOSO_0, 5, 8, mEv_EVENT_SPORTS_FAIR_FOOT_RACE },
    { SP_NPC_EV_TUNAHIKI_0, 5, 9, mEv_EVENT_SPORTS_FAIR_TUG_OF_WAR },
    { SP_NPC_EV_HATUMODE_0, 5, 7, 1 },
};

#define MP_EV_AREAS       (int)(sizeof(s_ev_area) / sizeof(s_ev_area[0]))
#define MP_EV_AREA_BYTES  (int)sizeof(((mEv_area_c*)0)->data)
#define MP_EV_STEPS       6 // the control's, then each character's
#define MP_EV_GROUP_BYTES (MP_EV_AREA_BYTES + MP_EV_STEPS * (int)sizeof(mp_evstep_t))
#define MP_EV_KEY_BYTES   (MP_EV_AREA_BYTES + MP_EV_STEPS * 8)
#define MP_EV_SINGLES     8 // single event characters in one message at most

// the shrine line's steps of a player's own turn: walked up, paying, praying, then wished a happy new year
#define MP_EV_TURN_FIRST 34
#define MP_EV_TURN_LAST  45

typedef char mp_ev_areas_fit[(MP_EV_AREAS <= 3 &&
                               2 + 3 * MP_EV_GROUP_BYTES + 1 + MP_EV_SINGLES * (1 + (int)sizeof(mp_evstep_t)) <=
                                   MP_STATE_MAX &&
                               MP_EV_KEY_BYTES <= (int)sizeof(s_s2.ev_sent[0]))
                                  ? 1
                                  : -1];

static int mp_ev_k(int type, int id) {
    int k;

    for (k = 0; k < MP_EV_AREAS; k++) {
        if (s_ev_area[k].type == type && (id < 0 || s_ev_area[k].id == id)) {
            return k;
        }
    }
    return -1;
}

static int mp_ev_group_of(int i);
static int mp_ev_single(int i);
static u8 mp_ev_runner(int k);
static void mp_ev_torn_record(int k);

// where each character is shared: villagers outdoors and in their own houses, the rest where they stand
static u16 mp_sp_scene(int i) {
    return s_sp[i - MP_NPC_N].scene != 0 ? s_sp[i - MP_NPC_N].scene : SCENE_FG;
}

static int mp_npc_scene_ok(int i) {
    if (i >= MP_NPC_N) {
        return Save_Get(scene_no) == mp_sp_scene(i);
    }
    return Save_Get(scene_no) == SCENE_FG || Save_Get(scene_no) == SCENE_NPC_HOUSE ||
           (i == ANIMAL_NUM_MAX && Save_Get(scene_no) == SCENE_COTTAGE_NPC);
}

// the house a player stands in, by its villager (the island's cottage is its villager's), 0 for none
static u16 mp_npc_house_of(u16 scene, u16 owner) {
    if (scene == SCENE_NPC_HOUSE) {
        return owner;
    }
    return scene == SCENE_COTTAGE_NPC ? Save_Get(island).animal.id.npc_id : 0;
}

static int mp_sp_find(u16 npc_id) {
    int k;

    for (k = 0; k < MP_SP_N; k++) {
        if (s_sp[k].id == npc_id) {
            return MP_NPC_N + k;
        }
    }
    return -1;
}

// villagers going about their day, and the visitors and event characters above
static int mp_npc_idx(ACTOR* actor) {
    int anm;

    if (ITEM_NAME_GET_TYPE(actor->npc_id) == NAME_TYPE_SPNPC) {
        return actor->part == ACTOR_PART_NPC ? mp_sp_find(actor->npc_id) : -1;
    }
    if (actor->id != mAc_PROFILE_NORMAL_NPC || ITEM_NAME_GET_TYPE(actor->npc_id) != NAME_TYPE_NPC) {
        return -1;
    }
    anm = mp_anm_of(actor);
    if (anm == MP_ANM_ISLAND) {
        return ANIMAL_NUM_MAX;
    }
    return anm < ANIMAL_NUM_MAX ? anm : -1;
}

static Animal_c* mp_npc_animal(int i) {
    if (i >= MP_NPC_N) {
        return NULL;
    }
    return mp_anm_ptr(i == ANIMAL_NUM_MAX ? MP_ANM_ISLAND : i);
}

static mNpc_NpcList_c* mp_npc_list(int i) {
    return i == ANIMAL_NUM_MAX ? Common_GetPointer(island_npclist[0]) : Common_GetPointer(npclist[i]);
}

static u16 mp_npc_id(int i) {
    Animal_c* animal;

    if (i >= MP_NPC_N) {
        return s_sp[i - MP_NPC_N].id;
    }
    animal = mp_npc_animal(i);
    return animal != NULL ? animal->id.npc_id : 0;
}

// this game's own copy of the character is out (a stand-in for another player's doesn't count)
static int mp_npc_out_here(int i) {
    return s_s2.actor[i] != NULL && s_s2.actor[i] != s_s2.standin[i];
}

void mp_npc_standin(void* actor) {
    int i = mp_npc_idx((ACTOR*)actor);

    if (i >= 0) {
        s_s2.standin[i] = (ACTOR*)actor;
    }
}

static int mp_seen_fresh(int i) {
    return s_s2.seen[i].valid && pc_mp_now_ms() - s_s2.seen[i].ms < MP_POSE_STALE_MS;
}

static void mp_pose_look(ACTOR* actor, mp_npose_t* q) {
    NPC_ACTOR* npc = (NPC_ACTOR*)actor;
    cKF_FrameControl_c* fc = &npc->draw.main_animation.keyframe.frame_control;
    TOOLS_ACTOR* tool = (TOOLS_ACTOR*)npc->right_hand.item_actor_p;

    q->x = actor->world.position.x;
    q->y = actor->world.position.y;
    q->z = actor->world.position.z;
    q->rot_y = actor->shape_info.rotation.y;
    q->head_x = npc->head.angle_x;
    q->head_y = npc->head.angle_y;
    q->anim = (unsigned char)npc->draw.animation_id;
    q->sub_anim = (signed char)npc->draw.sub_anim_type;
    q->talk = npc->talk_info.type == 1;
    q->hidden = npc->condition_info.hide_flg != 0;
    // what aNPC_check_kutipaku sees: a talk being spoken, or a greeting between villagers
    q->kutipaku = ((mDemo_Check(mDemo_TYPE_SPEAK, actor) || mDemo_Check(mDemo_TYPE_SPEECH, actor) ||
                    mDemo_Check(mDemo_TYPE_TALK, actor)) &&
                   mMsg_Check_NowUtter()) ||
                  npc->condition_info.greeting_flag;
    q->cloth = npc->draw.cloth_no;
    q->org_idx = npc->draw.org_idx;
    q->mouth = (unsigned char)npc->draw.tex_anim[aNPC_TEX_ANIM_MOUTH].pattern;
    // the right hand: an umbrella, or a prop an event handed it (a fan, a popper, a rod, a flag)
    q->umb = FALSE;
    q->prop = FALSE;
    q->tool = 0xFF;
    q->tool_act = 0xFF;
    if (tool != NULL && tool->tool_name >= 0 && tool->tool_name < aTOL_NUM) {
        q->tool = (unsigned char)tool->tool_name;
        if (npc->right_hand.item_type == aNPC_ITEM_TYPE_UMBRELLA) {
            q->umb = TRUE;
        } else if (tool->tool_name > TOOL_ORG_UMBRELLA7) {
            q->prop = TRUE;
            q->tool_act = (unsigned char)tool->work0;
        }
    }
    q->lsnd = 0;
    q->frame = fc->current_frame;
    q->speed = fc->speed;
}

static void mp_pose_capture(ACTOR* actor, int i, mp_npose_t* q) {
    mp_pose_look(actor, q);
    q->lsnd = s_s2.lsnd[i];
}

static u16 mp_vel16(f32 v) {
    s32 k = (s32)(v * 256.0f);

    return (u16)(s16)(k > 32767 ? 32767 : (k < -32768 ? -32768 : k));
}

static int mp_pose_pack(u8* p, int i, const mp_npose_t* q) {
    s32 speed = (s32)(q->speed * 64.0f);

    p[0] = (u8)i;
    p[1] = (u8)(q->hidden | (q->talk << 1) | (q->kutipaku << 2) | (q->umb << 3) | (q->prop << 4));
    p[2] = q->anim;
    p[3] = (u8)q->sub_anim;
    memcpy(p + 4, &q->x, 4);
    memcpy(p + 8, &q->y, 4);
    memcpy(p + 12, &q->z, 4);
    mp_put16(p + 16, (u16)q->rot_y);
    mp_put16(p + 18, (u16)q->head_x);
    mp_put16(p + 20, (u16)q->head_y);
    mp_put16(p + 22, (u16)(q->frame < 0.0f ? 0 : (q->frame * 16.0f > 65535.0f ? 65535 : (int)(q->frame * 16.0f))));
    mp_put16(p + 24, (u16)(s16)(speed > 32767 ? 32767 : (speed < -32768 ? -32768 : speed)));
    p[26] = q->mouth;
    p[27] = q->tool;
    mp_put16(p + 28, q->cloth);
    p[30] = q->org_idx;
    p[31] = q->tool_act;
    p[32] = q->lsnd;
    mp_put16(p + 33, mp_vel16(q->vx));
    mp_put16(p + 35, mp_vel16(q->vy));
    mp_put16(p + 37, mp_vel16(q->vz));
    return MP_POSE_WIRE;
}

static void mp_pose_unpack(const u8* p, mp_npose_t* q) {
    q->hidden = p[1] & 1;
    q->talk = (p[1] >> 1) & 1;
    q->kutipaku = (p[1] >> 2) & 1;
    q->umb = (p[1] >> 3) & 1;
    q->prop = (p[1] >> 4) & 1;
    q->tool = p[27];
    q->anim = p[2];
    q->sub_anim = (signed char)p[3];
    memcpy(&q->x, p + 4, 4);
    memcpy(&q->y, p + 8, 4);
    memcpy(&q->z, p + 12, 4);
    q->rot_y = (short)mp_get16(p + 16);
    q->head_x = (short)mp_get16(p + 18);
    q->head_y = (short)mp_get16(p + 20);
    q->frame = (float)mp_get16(p + 22) / 16.0f;
    q->speed = (float)(s16)mp_get16(p + 24) / 64.0f;
    q->mouth = p[26];
    q->cloth = (unsigned short)mp_get16(p + 28);
    q->org_idx = p[30];
    q->tool_act = p[31];
    q->lsnd = p[32];
    q->vx = (f32)(s16)mp_get16(p + 33) / 256.0f;
    q->vy = (f32)(s16)mp_get16(p + 35) / 256.0f;
    q->vz = (f32)(s16)mp_get16(p + 37) / 256.0f;
}

// where a report has walked to by now
static void mp_pose_carry(mp_npose_t* q, int frames) {
    // (a sound the owner's copy kept up stops with its reports)
    if (frames > 20) {
        q->lsnd = 0;
    }
    if (frames > MP_POSE_AHEAD) {
        frames = MP_POSE_AHEAD;
    }
    q->x += q->vx * frames;
    q->y += q->vy * frames;
    q->z += q->vz * frames;
}

// this game's say on one of its characters
static int mp_npc_held_by(int owner, int i) {
    u16 id = mp_npc_id(i);

    return owner < MP_MAX_PEERS && id != 0 && s_npc.lock[owner] == id;
}

// the Porter: each game runs its own (his logic drives that game's train, camera and boarding); while one screen's
// player arrives, talks to him or boards, the other screens show that screen's Porter, and then he rests
#define MP_PORTER_EVERY    3     // frames between reports while he's busy
#define MP_PORTER_TAIL_MS  500   // ...and on this long after, so the others see where he came to rest
#define MP_PORTER_QUIET_MS 1500  // follower: no report this long and he's this game's again
#define MP_PORTER_PACE     1.0f  // an NPC's walk, per frame
#define MP_PORTER_FAR      20.0f // this far from where he should stand, he walks there
#define MP_PORTER_NEAR     3.0f  // ...until this close

static struct {
    u8 busy;         // this screen's player has him
    u32 busy_ms;     // ...last time
    int tick;
    u8 have;         // his spot last frame, for his pace
    f32 x;
    f32 y;
    f32 z;
    mp_npose_t sent; // the last report this screen sent
    u8 sent_have;
    u8 heard_any;    // follower: the screen whose Porter this one shows, and its latest report
    u8 from;
    mp_npose_t pose;
    u32 ms;
    int heard;
    u8 following;    // he showed another screen's; after, he goes back to his post if it left him away
    u8 homing;
    u8 walking;      // a walk of his own to where he should stand
} s_porter;

void mp_porter_reset(void) {
    s_porter.busy = FALSE;
    s_porter.busy_ms = 0;
    s_porter.have = FALSE;
    s_porter.sent_have = FALSE;
    s_porter.following = FALSE;
    s_porter.homing = FALSE;
    s_porter.walking = FALSE;
}

void mp_porter_busy(int busy) {
    s_porter.busy = busy != 0;
    if (busy) {
        s_porter.busy_ms = pc_mp_now_ms();
    }
}

// the host's goes to every visitor in town, a visitor's to the host, which passes it on
static void mp_porter_to_guests(const u8* msg, int len, int except) {
    int g;

    for (g = 1; g < MP_MAX_PEERS; g++) {
        if (g != except && mp_lobby_guest_arrived(g) && mp_lobby_guest_conn(g) >= 0) {
            mp_lobby_send_state_to(mp_lobby_guest_conn(g), msg, len);
        }
    }
}

static void mp_porter_send(const mp_npose_t* q) {
    u8 msg[2 + MP_POSE_WIRE];

    msg[0] = MP_S_PORTER;
    msg[1] = (u8)mp_lobby_self_slot();
    mp_pose_pack(msg + 2, 0, q);
    if (mp_is_host()) {
        mp_porter_to_guests(msg, (int)sizeof(msg), 0);
    } else if (mp_lobby_host_conn() >= 0) {
        mp_lobby_send_state_to(mp_lobby_host_conn(), msg, (int)sizeof(msg));
    }
}

void mp_porter_moved(void* actorx) {
    ACTOR* actor = (ACTOR*)actorx;
    mp_npose_t q;

    if (mp_town_shared() && s_porter.busy_ms != 0 &&
        (s_porter.busy || pc_mp_now_ms() - s_porter.busy_ms < MP_PORTER_TAIL_MS) &&
        ((NPC_ACTOR*)actor)->draw.animation_id < aNPC_ANIM_NUM && ++s_porter.tick % MP_PORTER_EVERY == 0) {
        mp_pose_look(actor, &q);
        q.vx = q.vy = q.vz = 0.0f;
        if (s_porter.have) {
            q.vx = actor->world.position.x - s_porter.x;
            q.vy = actor->world.position.y - s_porter.y;
            q.vz = actor->world.position.z - s_porter.z;
        }
        s_porter.sent = q;
        s_porter.sent_have = TRUE;
        mp_porter_send(&q);
    }
    s_porter.have = TRUE;
    s_porter.x = actor->world.position.x;
    s_porter.y = actor->world.position.y;
    s_porter.z = actor->world.position.z;
}

// a menu stopped this game mid-talk with him (the travel menu): the others keep showing him as he stands
static void mp_porter_still(void) {
    mp_npose_t q;

    if (s_porter.busy && s_porter.sent_have && ++s_porter.tick % MP_PORTER_EVERY == 0) {
        q = s_porter.sent;
        q.vx = q.vy = q.vz = 0.0f;
        mp_porter_send(&q);
    }
}

void mp_porter_on_state(int conn, const unsigned char* data, int len) {
    int from;

    if (len < 2 + MP_POSE_WIRE || data[0] != MP_S_PORTER) {
        return;
    }
    from = data[1];
    if (mp_is_host()) {
        if (from <= 0 || from != mp_lobby_guest_slot(conn) + 1) {
            return;
        }
        mp_porter_to_guests(data, 2 + MP_POSE_WIRE, from);
    } else if (conn != mp_lobby_host_conn() || from == mp_lobby_self_slot()) {
        return;
    }
    // one screen's at a time: another busy one waits until that one goes quiet
    if (s_porter.heard_any && s_porter.from != from && pc_mp_now_ms() - s_porter.ms < MP_PORTER_QUIET_MS) {
        return;
    }
    mp_pose_unpack(data + 2, &s_porter.pose);
    s_porter.heard_any = TRUE;
    s_porter.from = (u8)from;
    s_porter.ms = pc_mp_now_ms();
    s_porter.heard = s_s2.frame;
}

static int mp_porter_control(ACTOR* actor, mp_npose_t* pose) {
    NPC_ACTOR* npc = (NPC_ACTOR*)actor;
    int live = s_porter.heard_any && pc_mp_now_ms() - s_porter.ms < MP_PORTER_QUIET_MS && mp_town_shared();
    f32 tx;
    f32 tz;
    f32 dx;
    f32 dz;
    f32 d;
    int still;

    if (s_porter.busy || (!live && !s_porter.following)) {
        if (s_porter.following) {
            s_porter.following = FALSE;
            s_porter.homing = FALSE;
            s_porter.walking = FALSE;
            return MP_NPC_RESUME;
        }
        return MP_NPC_LOCAL;
    }
    if (live) {
        *pose = s_porter.pose;
        pose->fresh = s_porter.heard == s_s2.frame;
        mp_pose_carry(pose, s_s2.frame - s_porter.heard);
        s_porter.following = TRUE;
        s_porter.homing = FALSE;
        tx = pose->x;
        tz = pose->z;
    } else {
        // that screen's done with him: back to his post (where he stands after greeting an arrival) if it left him
        // away from it, and from there he's this game's again
        tx = actor->home.position.x + mFI_UT_WORLDSIZE_X_F;
        tz = actor->home.position.z;
        dx = tx - actor->world.position.x;
        dz = tz - actor->world.position.z;
        if (!s_porter.homing && dx * dx + dz * dz > MP_PORTER_FAR * MP_PORTER_FAR) {
            s_porter.homing = TRUE;
            s_porter.walking = TRUE;
        }
        if (!s_porter.homing) {
            s_porter.following = FALSE;
            s_porter.walking = FALSE;
            return MP_NPC_RESUME;
        }
    }
    dx = tx - actor->world.position.x;
    dz = tz - actor->world.position.z;
    d = sqrtf(dx * dx + dz * dz);
    // (one standing somewhere else, as a visitor's Porter waits where their train stops: he walks over; one walking
    // is followed as he goes)
    still = !live || pose->vx * pose->vx + pose->vz * pose->vz < 0.01f;
    if (d > MP_PORTER_FAR && still) {
        s_porter.walking = TRUE;
    } else if (d < MP_PORTER_NEAR || !still) {
        s_porter.walking = FALSE;
    }
    if (!s_porter.walking) {
        if (!live) {
            s_porter.following = FALSE;
            s_porter.homing = FALSE;
            return MP_NPC_RESUME;
        }
        return MP_NPC_PUPPET;
    }
    // his own walk there, a step a frame
    mp_pose_look(actor, pose);
    actor->world.position.x += dx / d * (d < MP_PORTER_PACE ? d : MP_PORTER_PACE);
    actor->world.position.z += dz / d * (d < MP_PORTER_PACE ? d : MP_PORTER_PACE);
    pose->x = actor->world.position.x;
    pose->y = actor->world.position.y;
    pose->z = actor->world.position.z;
    pose->rot_y = atans_table(dz, dx);
    pose->head_x = 0;
    pose->head_y = 0;
    if (npc->draw.animation_id != aNPC_ANIM_WALK1) {
        pose->frame = 1.0f;
    }
    pose->anim = aNPC_ANIM_WALK1;
    pose->sub_anim = aNPC_SUB_ANIM_NONE;
    pose->talk = FALSE;
    pose->kutipaku = FALSE;
    pose->hidden = FALSE;
    pose->umb = FALSE;
    pose->prop = FALSE;
    pose->tool = 0xFF;
    pose->tool_act = 0xFF;
    pose->speed = npc->draw.frame_speed;
    pose->fresh = FALSE;
    pose->vx = pose->vy = pose->vz = 0.0f;
    return MP_NPC_PUPPET;
}

int mp_npc_control(void* actorx, mp_npose_t* pose) {
    ACTOR* actor = (ACTOR*)actorx;
    int self = mp_lobby_self_slot();
    int i;
    int k;
    int owner;

    if (actor->part == ACTOR_PART_NPC && actor->npc_id == SP_NPC_STATION_MASTER) {
        return mp_porter_control(actor, pose);
    }
    if (!mp_town_shared()) {
        // the others gone: what followed them is this game's again
        if (mp_active() && (i = mp_npc_idx(actor)) >= 0 && s_s2.puppet[i]) {
            s_s2.puppet[i] = FALSE;
            return MP_NPC_RESUME;
        }
        return MP_NPC_LOCAL;
    }
    if ((i = mp_npc_idx(actor)) < 0 || !mp_npc_scene_ok(i)) {
        return MP_NPC_LOCAL;
    }
    owner = s_s2.owner[i];
    k = mp_ev_group_of(i);
    if (owner == MP_NOBODY || owner == self || !s_s2.seen[i].valid ||
        s_s2.seen[i].scene != (u16)Save_Get(scene_no) ||
        (pc_mp_now_ms() - s_s2.seen[i].ms > MP_POSE_STALE_MS && !mp_npc_held_by(owner, i))) {
        // an event's character another screen runs waits where it stands for word from there (or, having
        // followed one, for the host to name the next): on its own it would run its event apart
        if ((k >= 0 || mp_ev_single(i)) && owner != self && (owner != MP_NOBODY || s_s2.ev_npc_follow[i])) {
            mp_pose_capture(actor, i, pose);
            pose->vx = pose->vy = pose->vz = 0.0f;
            pose->fresh = FALSE;
            s_s2.puppet[i] = TRUE;
            s_s2.ev_npc_follow[i] = k < 0 || owner == mp_ev_runner(k);
            return MP_NPC_PUPPET;
        }
        if (s_s2.puppet[i]) {
            s_s2.puppet[i] = FALSE;
            return MP_NPC_RESUME;
        }
        return MP_NPC_LOCAL;
    }
    *pose = s_s2.seen[i].pose;
    pose->fresh = s_s2.seen[i].frame == s_s2.frame;
    mp_pose_carry(pose, s_s2.frame - s_s2.seen[i].frame);
    s_s2.puppet[i] = TRUE;
    if (k >= 0) {
        s_s2.ev_npc_follow[i] = owner == mp_ev_runner(k);
    } else if (mp_ev_single(i)) {
        s_s2.ev_npc_follow[i] = TRUE;
    }
    return MP_NPC_PUPPET;
}

int mp_npc_is_puppet(void* actorx) {
    int i = mp_npc_idx((ACTOR*)actorx);

    return i >= 0 && s_s2.puppet[i] && s_s2.actor[i] == (ACTOR*)actorx && mp_town_shared();
}

// the island's villager has a walker list entry only on a screen that sailed out there: its house is where the save
// has it
static void mp_islander_home(xyz_t* pos) {
    Animal_c* animal = Save_GetPointer(island.animal);

    pos->x = animal->home_info.block_x * mFI_BK_WORLDSIZE_X_F + animal->home_info.ut_x * mFI_UT_WORLDSIZE_X_F +
             mFI_UT_WORLDSIZE_HALF_X_F;
    pos->y = 0.0f;
    pos->z = animal->home_info.block_z * mFI_BK_WORLDSIZE_Z_F + animal->home_info.ut_z * mFI_UT_WORLDSIZE_Z_F +
             mFI_UT_WORLDSIZE_HALF_Z_F;
}

// best known spot of a character: its actor here, its owner's report, or the town's walker list
static int mp_npc_where(int i, xyz_t* pos) {
    Animal_c* animal = mp_npc_animal(i);
    int unlisted = i == ANIMAL_NUM_MAX && mp_npc_list(i)->name == EMPTY_NO;

    if (i >= MP_NPC_N) {
        // an event character is only anywhere while a game has it out
        if (s_s2.actor[i] != NULL) {
            *pos = s_s2.actor[i]->world.position;
            return TRUE;
        }
        if (mp_seen_fresh(i) && s_s2.seen[i].scene == mp_sp_scene(i)) {
            pos->x = s_s2.seen[i].pose.x;
            pos->y = s_s2.seen[i].pose.y;
            pos->z = s_s2.seen[i].pose.z;
            return TRUE;
        }
        return FALSE;
    }
    if (animal == NULL || mNpc_CheckFreeAnimalPersonalID(&animal->id)) {
        return FALSE;
    }
    if (animal->is_home) {
        // the player nearest the house decides when they come out
        *pos = mp_npc_list(i)->house_position;
        if (unlisted) {
            mp_islander_home(pos);
        }
        return TRUE;
    }
    if (s_s2.actor[i] != NULL) {
        *pos = s_s2.actor[i]->world.position;
    } else if (mp_seen_fresh(i)) {
        pos->x = s_s2.seen[i].pose.x;
        pos->y = s_s2.seen[i].pose.y;
        pos->z = s_s2.seen[i].pose.z;
    } else if (unlisted) {
        mp_islander_home(pos);
    } else {
        *pos = mp_npc_list(i)->position;
    }
    return TRUE;
}

// host: this player's game has the event character out
static int mp_sp_have(int s, int i) {
    int k = i - MP_NPC_N;

    if (s == 0) {
        return mp_npc_out_here(i);
    }
    return pc_mp_now_ms() - s_s2.have_ms[s] < MP_POSE_STALE_MS && (s_s2.have[s][k >> 3] & (1 << (k & 7))) != 0;
}

// the list, then the screen running each grouped event (a talk may hold one of its characters apart), then the
// screen stepping the villagers nobody watches and the one bringing Pete by
static void mp_host_send_owners(int conn) {
    u8 msg[2 + MP_NPC_ALL + MP_EV_AREAS + 2];

    msg[0] = MP_M_NPC_OWNERS;
    msg[1] = MP_NPC_ALL;
    memcpy(msg + 2, s_s2.owner, MP_NPC_ALL);
    memcpy(msg + 2 + MP_NPC_ALL, s_s2.ev_group, MP_EV_AREAS);
    msg[2 + MP_NPC_ALL + MP_EV_AREAS] = s_s2.walker;
    msg[2 + MP_NPC_ALL + MP_EV_AREAS + 1] = s_s2.mail;
    if (conn >= 0) {
        mp_lobby_send_rel(conn, msg, (int)sizeof(msg));
    } else {
        // everyone has this list now, so the next pass only speaks up for a change from it
        memcpy(s_s2.owner_sent, s_s2.owner, MP_NPC_ALL);
        memcpy(s_s2.ev_group_sent, s_s2.ev_group, MP_EV_AREAS);
        s_s2.walker_sent = s_s2.walker;
        s_s2.mail_sent = s_s2.mail;
        s_s2.owner_sent_ms = pc_mp_now_ms();
        mp_host_send_live(msg, (int)sizeof(msg));
    }
}

static int mp_ev_has(int s, int i) {
    return s == 0 ? mp_sp_have(0, i) : (s < MP_MAX_PEERS && mp_lobby_guest_arrived(s) && mp_sp_have(s, i));
}

// host: an event character whose logic keeps the event's record runs on one screen for everyone, sending
// that record back if it isn't the host's: whoever runs it keeps it while its screen has it out (a visitor's
// game runs it on behind its menus, as the host's does), else the host if it has it, else the first visitor
// out with it; that way an event isn't split between screens, nor handed about
static u8 mp_host_ev_owner(int i) {
    u8 cur = s_s2.ev_owner[i];
    int s;

    if (cur != MP_NOBODY && mp_ev_has(cur, i)) {
        return cur;
    }
    for (s = 0; s < MP_MAX_PEERS; s++) {
        if (mp_ev_has(s, i)) {
            return s_s2.ev_owner[i] = (u8)s;
        }
    }
    return s_s2.ev_owner[i] = MP_NOBODY;
}

// the group's first character, whose coming and going is the event's on a screen
static int mp_ev_npc0(int k) {
    return mp_sp_find(s_ev_area[k].first);
}

static int mp_ev_group_of(int i) {
    u16 id;
    int k;

    if (i < MP_NPC_N) {
        return -1;
    }
    id = s_sp[i - MP_NPC_N].id;
    for (k = 0; k < MP_EV_AREAS; k++) {
        if (id >= s_ev_area[k].first && id < s_ev_area[k].first + s_ev_area[k].n) {
            return k;
        }
    }
    return -1;
}

// one of the host's event characters that runs on one screen alone (K.K., Tortimer, Wisp, the night stalls)
static int mp_ev_single(int i) {
    return i >= MP_NPC_N && i < MP_NPC_ALL && s_sp[i - MP_NPC_N].host_only && mp_ev_group_of(i) < 0;
}

// the screen running a grouped event, as this game has heard (the host's list)
static u8 mp_ev_runner(int k) {
    return s_s2.ev_group[k];
}

// the shrine line has a player's own turn under way on the screen running it
static int mp_ev_turn_on(int k) {
    int j;

    for (j = 1; j < MP_EV_STEPS && s_s2.ev_step_have[k] && s_ev_area[k].type == 1; j++) {
        u8 t = s_s2.ev_step[k][j].think;

        if (t >= MP_EV_TURN_FIRST && t <= MP_EV_TURN_LAST) {
            return TRUE;
        }
    }
    return FALSE;
}

// host: each grouped event runs whole on one screen: it stays where it runs while that screen has it out (a
// visitor's game runs it on behind its menus), else the host if it has it, else the first visitor out with
// it; the shrine line goes to a player talking their way into it, unless someone's turn is under way; once
// nobody has it out its record ends as it would for one player walking off
static void mp_host_ev_groups(void) {
    int k;

    for (k = 0; k < MP_EV_AREAS; k++) {
        int n0 = mp_ev_npc0(k);
        u8 cur = s_s2.ev_group[k];
        u8 own = MP_NOBODY;
        int j;
        int s;

        if (n0 < 0) {
            continue;
        }
        // (the runner's own player talking to one of the line keeps it: the offering question may be up)
        for (j = 0; j < s_ev_area[k].n && s_ev_area[k].type == 1 && cur < MP_MAX_PEERS && mp_ev_has(cur, n0); j++) {
            if (s_npc.lock[cur] == (u16)(s_ev_area[k].first + j)) {
                own = cur;
            }
        }
        for (j = 0; j < s_ev_area[k].n && s_ev_area[k].type == 1 && !mp_ev_turn_on(k) && own == MP_NOBODY; j++) {
            for (s = 0; s < MP_MAX_PEERS; s++) {
                if (s_npc.lock[s] == (u16)(s_ev_area[k].first + j) && mp_ev_has(s, n0)) {
                    own = (u8)s;
                    break;
                }
            }
        }
        if (own == MP_NOBODY && cur != MP_NOBODY && mp_ev_has(cur, n0)) {
            own = cur;
        }
        for (s = 0; s < MP_MAX_PEERS && own == MP_NOBODY; s++) {
            if (mp_ev_has(s, n0)) {
                own = (u8)s;
            }
        }
        // (the host's own leaving may have left it for a visitor who then left too)
        if (own == MP_NOBODY && cur != MP_NOBODY) {
            mp_ev_torn_record(k);
        }
        s_s2.ev_group[k] = own;
    }
}

// host: the nearest player whose screen has an event character out moves it; one that keeps the
// event's record runs on one screen for everyone
static u8 mp_host_sp_owner(int i, const xyz_t* ppos, const u16* pscene) {
    xyz_t vpos;
    int known = mp_npc_where(i, &vpos);
    u8 cur = s_s2.owner[i];
    u8 best = MP_NOBODY;
    f32 best_d = 0.0f;
    int s;

    if (s_sp[i - MP_NPC_N].host_only) {
        return mp_host_ev_owner(i);
    }
    for (s = 0; s < MP_MAX_PEERS; s++) {
        f32 d;

        if (pscene[s] != mp_sp_scene(i) || !mp_sp_have(s, i)) {
            continue;
        }
        // nowhere known yet: whoever has it keeps it, else the first to have it out
        d = known ? (ppos[s].x - vpos.x) * (ppos[s].x - vpos.x) + (ppos[s].z - vpos.z) * (ppos[s].z - vpos.z)
                  : (f32)(s == cur ? 0 : s + 1);
        if (s == cur) {
            d /= MP_OWNER_KEEP;
        }
        if (best == MP_NOBODY || d < best_d) {
            best = (u8)s;
            best_d = d;
        }
    }
    return best;
}

// host: the villagers nobody is watching are stepped about the town (the set manager's walker) by the host's game
// while its player is out in town, else by a visitor's out there, so the town goes on while the host is inside
// or in a menu
static u8 mp_host_walker(const u16* pscene) {
    u8 cur = s_s2.walker;
    int s;

    if (pscene[0] == SCENE_FG) {
        return 0;
    }
    if (cur != 0 && cur < MP_MAX_PEERS && pscene[cur] == SCENE_FG) {
        return cur;
    }
    for (s = 1; s < MP_MAX_PEERS; s++) {
        if (pscene[s] == SCENE_FG) {
            return (u8)s;
        }
    }
    return MP_NOBODY;
}

// the screen stepping the villagers nobody watches, as this game has heard (0 alone: its own)
int mp_npc_walker(void) {
    return mp_town_shared() ? s_s2.walker : 0;
}

// the town's houses (and the post office, where Pete sometimes waits) by the town's own block table
static int mp_at_houses(const xyz_t* pos) {
    int bx;
    int bz;

    if (g_block_kind_p == NULL || !mp_town_block(pos, &bx, &bz) || bx >= BLOCK_X_NUM || bz >= BLOCK_Z_NUM) {
        return FALSE;
    }
    return (((u32*)g_block_kind_p)[bz * BLOCK_X_NUM + bx] & (mRF_BLOCKKIND_PLAYER | mRF_BLOCKKIND_POSTOFFICE)) != 0;
}

// host: Pete comes by for one player: the one whose game has him out keeps him, else the host out at the houses,
// else a visitor there, else the host (whose game only brings him at the houses)
static u8 mp_host_mail(const xyz_t* ppos, const int* in_town) {
    int i = mp_sp_find(SP_NPC_POST_MAN);
    int s;

    for (s = 0; s < MP_MAX_PEERS && i >= 0; s++) {
        if (mp_ev_has(s, i)) {
            return (u8)s;
        }
    }
    for (s = 0; s < MP_MAX_PEERS; s++) {
        if (in_town[s] && mp_at_houses(&ppos[s])) {
            return (u8)s;
        }
    }
    return 0;
}

int mp_npc_mail_here(void) {
    return !mp_town_shared() || s_s2.mail == mp_lobby_self_slot();
}

// Pete comes by for whichever player is at the houses; what he does on a visitor's game is told to the host
void pc_mp_mail_proc(struct game_play_s* play_s) {
    lbRTC_time_c due;
    int force;

    if (!mp_npc_mail_here()) {
        // Pete coming early is the host's to keep; the game bringing him hears it
        if (mp_is_guest()) {
            Common_Set(force_mail_delivery_flag, FALSE);
        }
        return;
    }
    due = Save_Get(post_office).delivery_time;
    force = Common_Get(force_mail_delivery_flag);
    mPO_business_proc((GAME_PLAY*)play_s);
    if (memcmp(&due, &Save_Get(post_office).delivery_time, sizeof(due)) != 0 ||
        force != Common_Get(force_mail_delivery_flag)) {
        pc_mp_mail_told(MP_MAIL_CAME, 0);
    }
}

void pc_mp_mail_told(int what, int house) {
    u8 msg[3];

    if (mp_is_host() || !mp_town_shared()) {
        return;
    }
    msg[0] = MP_M_NPC_MAIL;
    msg[1] = (u8)what;
    msg[2] = (u8)house;
    mp_send_up(msg, sizeof(msg));
}

// letters: a visitor's goes to the host's post office, which takes it in as a traveller's

enum {
    MP_LET_IDLE,
    MP_LET_WRITING, // out of the pockets; the passport is being written without it
    MP_LET_SENT,
    MP_LET_ANSWERED,
};

#define MP_LET_AGAIN_MS 3000 // no answer this long: the letter goes up again (the host answers a repeat as before)

static struct {
    int state;
    u32 pp_seq;
    u32 sent_ms;
    u16 serial;
    u8 dest;  // the host's answer: 0 taken in, else Pelly's reason for handing it back
    u8 taken; // the host has it: the receipt here only sees it off
    u8 has_remail;
    Anmremail_c remail; // the villager's reply, for when the traveller is home
    Mail_c mail;
} s_let;

static struct {
    Animal_c anm;
    PostOffice_c po;
    Private_c trav; // the traveller's slot, which may be someone's own
} s_let_was; // host: what a visitor's letter changes, as it was before

// host: each visitor's last letter and the answer it got, for a repeat of it
static struct {
    u8 have;
    u16 serial;
    u32 crc;
    u8 ans[6 + sizeof(Anmremail_c)];
} s_let_last[MP_MAX_PEERS];

static void mp_let_send(void) {
    u8 msg[4 + sizeof(Mail_c)];

    msg[0] = MP_M_NPC_LETTER;
    msg[1] = 0;
    mp_put16(msg + 2, s_let.serial);
    memcpy(msg + 4, &s_let.mail, sizeof(Mail_c));
    mp_send_up(msg, (int)sizeof(msg));
    s_let.sent_ms = pc_mp_now_ms();
}

// visitor at Pelly's desk: the letter goes up once the passport is written without it, and Pelly answers as the
// host's post office did. A letter to the museum is taken in here (its reply rides in the passport) and only
// counted on the host's desk.
int pc_mp_post_letter(void* mail_p, int dest) {
    Mail_c* mail = (Mail_c*)mail_p;
    int visiting = mp_is_guest() && mp_travel_state() == MP_TRAVEL_VISITING;
    int line = visiting && mp_lobby_host_conn() >= 0;
    int ok = FALSE;

    switch (s_let.state) {
        case MP_LET_IDLE:
            if (!visiting || dest != 0) {
                return dest;
            }
            s_let.serial++;
            s_let.mail = *mail;
            if (mail->header.recipient.type == mMl_NAME_TYPE_MUSEUM) {
                mp_let_send();
                return 0;
            }
            s_let.pp_seq = mp_passport_write_async();
            if (s_let.pp_seq == 0) {
                return 2;
            }
            s_let.state = MP_LET_WRITING;
            return -1;
        case MP_LET_WRITING:
            if (!mp_passport_written(s_let.pp_seq, &ok) && line) {
                return -1;
            }
            // the passport still holds it, or the line is down: it never left
            if (!ok || !line) {
                s_let.state = MP_LET_IDLE;
                return 2;
            }
            mp_let_send();
            s_let.state = MP_LET_SENT;
            return -1;
        case MP_LET_SENT:
            if (line) {
                if (pc_mp_now_ms() - s_let.sent_ms >= MP_LET_AGAIN_MS && !mp_lobby_conn_quiet(mp_lobby_host_conn())) {
                    mp_let_send();
                }
                return -1;
            }
            // the line went down with it on the way: it may have arrived, so it isn't handed back
            s_let.dest = 0;
            s_let.has_remail = FALSE;
            break;
        default:
            break;
    }
    s_let.state = MP_LET_IDLE;
    if (s_let.dest == 0) {
        s_let.taken = TRUE;
        if (s_let.has_remail && Now_Private != NULL) {
            Now_Private->remail = s_let.remail;
        }
    }
    return s_let.dest;
}

int pc_mp_letter_taken(const void* mail) {
    if (!s_let.taken || memcmp(mail, &s_let.mail, sizeof(Mail_c)) != 0) {
        return FALSE;
    }
    s_let.taken = FALSE;
    return TRUE;
}

// host: a visitor's letter, checked as Pelly checks one (everyone's letters share the desk). Only a letter the
// visitor's game could have written passes: theirs, sealed as sent, on real paper.
static int mp_letter_dest(int from, Mail_c* mail, PersonalID_c* pid) {
    mActor_name_t present = mail->present;

    if (!mp_lobby_guest_pid(mp_lobby_guest_conn(from), pid) || mail->header.sender.type != mMl_NAME_TYPE_PLAYER ||
        !mPr_CheckCmpPersonalID(&mail->header.sender.personalID, pid) || mail->content.font != mMl_FONT_RECV ||
        mail->content.mail_type != 0 || mail->content.paper_type >= PAPER_UNIQUE_NUM ||
        mail->content.header_back_start > MAIL_HEADER_LEN ||
        (present != EMPTY_NO && !ITEM_IS_ITEM1(present) && !ITEM_IS_FTR(present))) {
        return 1;
    }
    switch (mail->header.recipient.type) {
        case mMl_NAME_TYPE_PLAYER: {
            int addr = mMl_hunt_for_send_address(mail);

            if (addr == -1) {
                return 1;
            }
            if (mMl_chk_mail_free_space(Save_Get(homes)[addr].mailbox, HOME_MAILBOX_SIZE) == -1) {
                return 2;
            }
            if (mPO_count_mail(addr) >= HOME_MAILBOX_SIZE) {
                return 3;
            }
            return mPO_get_keep_mail_sum() >= mPO_MAIL_STORAGE_SIZE ? 2 : 0;
        }
        case mMl_NAME_TYPE_NPC: {
            AnmPersonalID_c anm_pid;

            mMl_get_npcinfo_from_mail_name(&anm_pid, &mail->header.recipient);
            return mNpc_SearchAnimalPersonalID(&anm_pid) == -1 ? 1 : 0;
        }
        case mMl_NAME_TYPE_MUSEUM:
            return 0;
    }
    return 1;
}

// host: the post office takes it in as the card trip's would, the foreigner's slot standing in for the visitor
// (their villager's reply is written there and sent back); the villager and the desk go out to everyone at once
static int mp_host_take_letter(Mail_c* mail, PersonalID_c* pid, u8* out) {
    Private_c* trav = mPr_GetForeignerP();
    int anm = -1;
    int dest = 0;

    s_let_was.po = Save_Get(post_office);
    s_let_was.trav = *trav;
    if (mail->header.recipient.type == mMl_NAME_TYPE_MUSEUM) {
        Save_Get(post_office).keep_mail_sum_npcs++;
    } else {
        if (mail->header.recipient.type == mMl_NAME_TYPE_NPC) {
            AnmPersonalID_c anm_pid;

            mMl_get_npcinfo_from_mail_name(&anm_pid, &mail->header.recipient);
            anm = mNpc_SearchAnimalPersonalID(&anm_pid);
            if (anm >= 0) {
                s_let_was.anm = Save_Get(animals[anm]);
            }
        }
        mPr_CopyPersonalID(&trav->player_ID, pid);
        mNpc_ClearRemail(&trav->remail);
        if (!mPO_mp_receipt_traveller(mail, trav) && mail->header.recipient.type == mMl_NAME_TYPE_PLAYER) {
            dest = 2;
        }
        if (trav->remail.flags.looks != 0x7F) {
            out[0] = TRUE;
            memcpy(out + 1, &trav->remail, sizeof(Anmremail_c));
        }
        *trav = s_let_was.trav;
        if (anm >= 0) {
            mp_world_host_took_diff(mp_anm_off(anm), &s_let_was.anm, sizeof(Animal_c), -1);
            s_npc.refresh = TRUE;
        }
    }
    mp_world_host_took_diff(offsetof(Save_t, post_office), &s_let_was.po, sizeof(PostOffice_c), -1);
    if (dest == 0) {
        mp_world_ask_commit();
    }
    return dest;
}

static void mp_host_on_letter(int from, const u8* p, int len) {
    u8 ans[6 + sizeof(Anmremail_c)];
    PersonalID_c pid;
    Mail_c mail;
    int dest;

    if (len < 4 + (int)sizeof(Mail_c)) {
        return;
    }
    memcpy(&mail, p + 4, sizeof(mail));
    if (from > 0 && from < MP_MAX_PEERS && s_let_last[from].have && s_let_last[from].serial == mp_get16(p + 2) &&
        s_let_last[from].crc == mp_crc32(&mail, sizeof(mail), 0)) {
        // (the same letter again: its answer went astray)
        mp_lobby_send_rel(mp_lobby_guest_conn(from), s_let_last[from].ans, (int)sizeof(ans));
        return;
    }
    memset(ans, 0, sizeof(ans));
    ans[0] = MP_M_NPC_LETTER_ANS;
    ans[2] = p[2];
    ans[3] = p[3];
    dest = mp_letter_dest(from, &mail, &pid);
    if (dest == 0) {
        dest = mp_host_take_letter(&mail, &pid, ans + 5);
    }
    ans[4] = (u8)dest;
    if (from > 0 && from < MP_MAX_PEERS) {
        s_let_last[from].have = TRUE;
        s_let_last[from].serial = (u16)mp_get16(p + 2);
        s_let_last[from].crc = mp_crc32(&mail, sizeof(mail), 0);
        memcpy(s_let_last[from].ans, ans, sizeof(ans));
    }
    mp_lobby_send_rel(mp_lobby_guest_conn(from), ans, (int)sizeof(ans));
}

static void mp_guest_on_letter_ans(const u8* p, int len) {
    if (len < 6 + (int)sizeof(Anmremail_c) || s_let.state != MP_LET_SENT || mp_get16(p + 2) != s_let.serial) {
        return;
    }
    s_let.dest = p[4] <= 3 ? p[4] : 2;
    s_let.has_remail = p[5] != 0;
    memcpy(&s_let.remail, p + 6, sizeof(Anmremail_c));
    s_let.state = MP_LET_ANSWERED;
}

// host: Chip's and Wisp's records go in the town's save when only a visitor's game has built them (a stand-in there
// would keep a record of its own); Chip's is set where he stands on that screen
static void mp_host_ev_records(void) {
    int chip = mp_sp_find(SP_NPC_ANGLER);
    int wisp = mp_sp_find(SP_NPC_EV_GHOST);
    int s;

    for (s = 1; s < MP_MAX_PEERS; s++) {
        if (chip >= 0 && s_s2.seen[chip].valid && mp_ev_has(s, chip)) {
            aEANG_mp_record((int)s_s2.seen[chip].pose.x, (int)s_s2.seen[chip].pose.z);
            chip = -1;
        }
        if (wisp >= 0 && mp_ev_has(s, wisp)) {
            aEGH_mp_record();
            wisp = -1;
        }
    }
}

// host: nearest player in or next to the villager's acre; a talk holds it; the current owner keeps it
// unless someone is clearly closer
static void mp_host_owners(GAME_PLAY* play) {
    PLAYER_ACTOR* me = (play != NULL) ? GET_PLAYER_ACTOR(play) : NULL;
    xyz_t ppos[MP_MAX_PEERS];
    int in_town[MP_MAX_PEERS];
    u16 in_house[MP_MAX_PEERS]; // the villager whose house the player stands in
    u16 pscene[MP_MAX_PEERS];   // where each player is, 0xFFFF when unknown or leaving
    int s;
    int i;

    // a visitor boarding home is still seen walking to the train but no longer moves villagers
    for (s = 0; s < MP_MAX_PEERS; s++) {
        unsigned int age;
        const mp_pstate_t* st = (s == 0) ? NULL : mp_player_state(s, &age);

        in_town[s] = FALSE;
        in_house[s] = 0;
        pscene[s] = 0xFFFF;
        if (mp_player_still(s)) {
            continue;
        }
        if (s == 0 && me != NULL) {
            pscene[0] = (u16)Save_Get(scene_no);
        } else if (s != 0 && st != NULL && age < MP_POSE_STALE_MS && !(st->flags & MP_PF_HIDDEN) &&
                   mp_lobby_guest_arrived(s)) {
            pscene[s] = st->scene;
        }
        if (s == 0 && me != NULL && mp_npc_house_of((u16)Save_Get(scene_no), mp_player_place()) != 0) {
            ppos[0] = me->actor_class.world.position;
            in_house[0] = mp_npc_house_of((u16)Save_Get(scene_no), mp_player_place());
        } else if (s != 0 && st != NULL && age < MP_POSE_STALE_MS && mp_npc_house_of(st->scene, st->owner) != 0 &&
                   mp_lobby_guest_arrived(s)) {
            ppos[s].x = st->x;
            ppos[s].y = st->y;
            ppos[s].z = st->z;
            in_house[s] = mp_npc_house_of(st->scene, st->owner);
        } else if (s == 0 && me != NULL && Save_Get(scene_no) == SCENE_FG) {
            ppos[0] = me->actor_class.world.position;
            in_town[0] = TRUE;
        } else if (st != NULL && age < MP_POSE_STALE_MS && st->scene == SCENE_FG && !(st->flags & MP_PF_HIDDEN) &&
                   mp_lobby_guest_arrived(s)) {
            ppos[s].x = st->x;
            ppos[s].y = st->y;
            ppos[s].z = st->z;
            in_town[s] = TRUE;
        } else if (s == 0 && me != NULL) {
            ppos[0] = me->actor_class.world.position;
        } else if (pscene[s] != 0xFFFF) {
            ppos[s].x = st->x;
            ppos[s].y = st->y;
            ppos[s].z = st->z;
        }
    }
    mp_host_ev_groups();
    mp_host_ev_records();
    s_s2.walker = mp_host_walker(pscene);
    s_s2.mail = mp_host_mail(ppos, in_town);
    // Pete is due early (someone found the desk full): the game bringing him hears it once
    if (!Common_Get(force_mail_delivery_flag)) {
        s_s2.force_told = MP_NOBODY;
    } else if (s_s2.mail != 0 && s_s2.mail != s_s2.force_told && mp_lobby_guest_conn(s_s2.mail) >= 0) {
        u8 msg[3] = { MP_M_NPC_MAIL, MP_MAIL_FORCE, 0 };

        mp_lobby_send_rel(mp_lobby_guest_conn(s_s2.mail), msg, (int)sizeof(msg));
        s_s2.force_told = s_s2.mail;
    }
    for (i = 0; i < MP_NPC_ALL; i++) {
        Animal_c* animal = mp_npc_animal(i);
        u16 id = mp_npc_id(i);
        u8 owner = MP_NOBODY;
        xyz_t vpos;
        int k = mp_ev_group_of(i);

        if (k >= 0) {
            owner = s_s2.ev_group[k];
            // a talk still holds the one it's with
            for (s = MP_MAX_PEERS - 1; s >= 0; s--) {
                if (s_npc.lock[s] == id) {
                    owner = (u8)s;
                }
            }
        } else {
            for (s = MP_MAX_PEERS - 1; s >= 0 && id != 0; s--) {
                if (s_npc.lock[s] == id) {
                    owner = (u8)s;
                }
            }
            if (owner == MP_NOBODY && i >= MP_NPC_N) {
                owner = mp_host_sp_owner(i, ppos, pscene);
                // the host's own event characters wait for it through a menu rather than run everywhere
                if (owner == MP_NOBODY && mp_npc_out_here(i)) {
                    owner = 0;
                }
            }
        }
        // at home with someone visiting: the nearest visitor in the room moves the villager there
        if (owner == MP_NOBODY && animal != NULL && animal->is_home) {
            int have = FALSE;
            f32 best_d = 0.0f;
            u8 cur = s_s2.owner[i];

            if (s_s2.actor[i] != NULL && mp_npc_house_of((u16)Save_Get(scene_no), mp_player_place()) != 0) {
                vpos = s_s2.actor[i]->world.position;
                have = TRUE;
            } else if (mp_seen_fresh(i)) {
                vpos.x = s_s2.seen[i].pose.x;
                vpos.y = s_s2.seen[i].pose.y;
                vpos.z = s_s2.seen[i].pose.z;
                have = TRUE;
            }
            for (s = 0; s < MP_MAX_PEERS; s++) {
                f32 d = 0.0f;

                if (in_house[s] == 0 || in_house[s] != animal->id.npc_id) {
                    continue;
                }
                if (have) {
                    d = (ppos[s].x - vpos.x) * (ppos[s].x - vpos.x) + (ppos[s].z - vpos.z) * (ppos[s].z - vpos.z);
                }
                if (s == cur) {
                    d /= MP_OWNER_KEEP;
                }
                if (owner == MP_NOBODY || d < best_d) {
                    owner = (u8)s;
                    best_d = d;
                }
            }
        }
        if (owner == MP_NOBODY && animal != NULL && mp_npc_where(i, &vpos)) {
            int vbx;
            int vbz;
            f32 best_d = 0.0f;
            u8 best = MP_NOBODY;
            u8 cur = s_s2.owner[i];

            mp_town_block(&vpos, &vbx, &vbz);
            for (s = 0; s < MP_MAX_PEERS; s++) {
                int bx;
                int bz;
                f32 d;

                // a villager one acre over is on that player's screen too, so it can't be left to each game
                if (!in_town[s] || !mp_town_block(&ppos[s], &bx, &bz) || bx - vbx > 1 || vbx - bx > 1 ||
                    bz - vbz > 1 || vbz - bz > 1) {
                    continue;
                }
                d = (ppos[s].x - vpos.x) * (ppos[s].x - vpos.x) + (ppos[s].z - vpos.z) * (ppos[s].z - vpos.z);
                if (s == cur) {
                    d /= MP_OWNER_KEEP;
                }
                if (best == MP_NOBODY || d < best_d) {
                    best = (u8)s;
                    best_d = d;
                }
            }
            owner = best;
        }
        // the player who netted it keeps it while its game plays out the reaction
        if (s_s2.claim_ms[i] != 0 && pc_mp_now_ms() - s_s2.claim_ms[i] < MP_CLAIM_KEEP_MS &&
            s_s2.owner[i] != MP_NOBODY && !mp_player_still(s_s2.owner[i]) &&
            (id == 0 || !mp_npc_talk_locked(id) || s_npc.lock[s_s2.owner[i]] == id)) {
            owner = s_s2.owner[i];
        }
        if (s_s2.owner[i] != owner) {
            s_s2.seen[i].valid = FALSE;
        }
        s_s2.owner[i] = owner;
    }
    if (memcmp(s_s2.owner, s_s2.owner_sent, MP_NPC_ALL) != 0 ||
        memcmp(s_s2.ev_group, s_s2.ev_group_sent, MP_EV_AREAS) != 0 || s_s2.walker != s_s2.walker_sent ||
        s_s2.mail != s_s2.mail_sent ||
        pc_mp_now_ms() - s_s2.owner_sent_ms >= MP_OWNER_RESEND) {
        mp_host_send_owners(-1);
    }
}

// host: where the villagers nobody is looking at are, so a visitor's game spawns them there
static void mp_host_send_list(int conn) {
    u8 msg[2 + MP_NPC_N * 13];
    int n = 0;
    int i;

    msg[0] = MP_M_NPC_LIST;
    for (i = 0; i < MP_NPC_N; i++) {
        xyz_t pos;

        if (mp_npc_where(i, &pos)) {
            u8* p = msg + 2 + n * 13;

            p[0] = (u8)i;
            memcpy(p + 1, &pos.x, 4);
            memcpy(p + 5, &pos.y, 4);
            memcpy(p + 9, &pos.z, 4);
            n++;
        }
    }
    msg[1] = (u8)n;
    if (conn >= 0) {
        mp_lobby_send_rel(conn, msg, 2 + n * 13);
    } else {
        mp_host_send_live(msg, 2 + n * 13);
    }
}

// what a follower would notice: the animation and its speed, the hands, clothes, turns of body and head
static u32 mp_pose_key(const mp_npose_t* q) {
    u32 v[6];
    u32 h = 2166136261u;
    int k;

    v[0] = q->anim | ((u32)(u8)q->sub_anim << 8) | ((u32)q->talk << 16) | ((u32)q->hidden << 17) |
           ((u32)q->kutipaku << 18) | ((u32)q->umb << 19) | ((u32)q->prop << 20);
    v[1] = q->tool | ((u32)q->tool_act << 8) | ((u32)q->lsnd << 16);
    v[2] = q->cloth | ((u32)q->org_idx << 16);
    v[3] = ((u32)(u16)q->rot_y >> 11) | (((u32)(u16)q->head_x >> 11) << 5) | (((u32)(u16)q->head_y >> 11) << 10);
    v[4] = (u32)(s32)(q->speed * 8.0f);
    v[5] = (q->vx * q->vx + q->vz * q->vz) > 0.01f; // walking or standing
    for (k = 0; k < 6; k++) {
        h = (h ^ v[k]) * 16777619u;
    }
    return h;
}

// this game's characters looked at again; any a follower would now show wrong are due to everyone
static void mp_mine_update(void) {
    int self = mp_lobby_self_slot();
    int i;

    for (i = 0; i < MP_NPC_ALL; i++) {
        mp_mine_t* m = &s_s2.mine[i];
        ACTOR* actor = s_s2.actor[i];
        cKF_FrameControl_c* fc;
        mp_npose_t q;
        mp_npose_t guess;
        f32 dx;
        f32 dy;
        f32 dz;
        int df;
        int send;

        if (s_s2.owner[i] != self || actor == NULL || s_s2.puppet[i] || actor->ct_proc != NULL ||
            ((NPC_ACTOR*)actor)->draw.animation_id >= aNPC_ANIM_NUM) {
            m->have = FALSE;
            continue;
        }
        mp_pose_capture(actor, i, &q);
        fc = &((NPC_ACTOR*)actor)->draw.main_animation.keyframe.frame_control;
        df = s_s2.frame - m->frame;
        q.fresh = FALSE;
        q.vx = q.vy = q.vz = 0.0f;
        if (m->have && df > 0 && df <= MP_POSE_TICK * 2) {
            q.vx = (q.x - m->pose.x) / (f32)df;
            q.vy = (q.y - m->pose.y) / (f32)df;
            q.vz = (q.z - m->pose.z) / (f32)df;
        }
        guess = m->sent;
        mp_pose_carry(&guess, s_s2.frame - m->sent_frame);
        dx = guess.x - q.x;
        dy = guess.y - q.y;
        dz = guess.z - q.z;
        send = !m->have || mp_pose_key(&q) != m->key || dx * dx + dz * dz > MP_POSE_DRIFT * MP_POSE_DRIFT ||
               dy * dy > MP_POSE_DRIFT * MP_POSE_DRIFT;
        // the same one-shot animation begun again (a second nod)
        if (m->have && q.anim == m->pose.anim && fc->mode == cKF_FRAMECONTROL_STOP && q.frame + 1.0f < m->pose.frame) {
            send = TRUE;
        }
        m->key = mp_pose_key(&q);
        m->pose = q;
        m->frame = s_s2.frame;
        m->have = TRUE;
        if (send) {
            m->again = TRUE;
        } else if (m->again) {
            m->again = FALSE;
            send = TRUE;
        } else {
            send = pc_mp_now_ms() - m->sent_ms >= MP_POSE_KEEP_MS;
        }
        if (send) {
            m->sent = q;
            m->sent_frame = s_s2.frame;
            m->sent_ms = pc_mp_now_ms();
            m->due = 0xFF;
        }
    }
}

// the poses one peer in scene hasn't heard yet: this game's characters, and on the host everyone else's;
// a visitor also tells the host which event characters its screen has out
static int mp_pose_msg(u8* msg, int to_slot, u16 scene) {
    u8 bit = (u8)(1 << (to_slot < 0 ? 0 : to_slot));
    int n = 0;
    int at = MP_POSE_HDR;
    int i;

    msg[0] = MP_S_NPCS;
    mp_put16(msg + 1, scene);
    msg[4] = MP_SP_BYTES;
    memset(msg + 5, 0, MP_SP_BYTES);
    for (i = MP_NPC_N; i < MP_NPC_ALL; i++) {
        if (mp_npc_out_here(i)) {
            msg[5 + ((i - MP_NPC_N) >> 3)] |= (u8)(1 << ((i - MP_NPC_N) & 7));
        }
    }
    for (i = 0; i < MP_NPC_ALL && at + MP_POSE_WIRE <= MP_STATE_MAX; i++) {
        mp_mine_t* m = &s_s2.mine[i];
        mp_seen_t* s = &s_s2.seen[i];

        if (m->have && (m->due & bit) && s_s2.owner[i] == mp_lobby_self_slot() && scene == (u16)Save_Get(scene_no)) {
            at += mp_pose_pack(msg + at, i, &m->sent);
            m->due &= (u8)~bit;
        } else if (mp_is_host() && s_s2.owner[i] != MP_NOBODY && s_s2.owner[i] != to_slot && mp_seen_fresh(i) &&
                   s->scene == scene && (s->due & bit)) {
            // passed on as its owner sent it, walked on to now
            mp_npose_t q = s->pose;

            mp_pose_carry(&q, s_s2.frame - s->frame);
            at += mp_pose_pack(msg + at, i, &q);
            s->due &= (u8)~bit;
        } else {
            continue;
        }
        n++;
    }
    msg[3] = (u8)n;
    if (mp_is_host()) {
        return n > 0 ? at : 0;
    }
    // the host keeps a visitor's list of event characters: it goes when it changes, and now and then (behind a
    // menu too, where the frames stand still)
    if (n == 0 && memcmp(msg + 5, s_s2.have_sent, MP_SP_BYTES) == 0 &&
        s_s2.frame - s_s2.have_sent_frame < MP_HAVE_EVERY && pc_mp_now_ms() - s_s2.have_sent_ms < MP_HAVE_MS) {
        return 0;
    }
    memcpy(s_s2.have_sent, msg + 5, MP_SP_BYTES);
    s_s2.have_sent_frame = s_s2.frame;
    s_s2.have_sent_ms = pc_mp_now_ms();
    return at;
}

static int mp_own_differs(const u8* a, const u8* b) {
    int k;

    for (k = 0; k < MP_OWN_N; k++) {
        if (a[s_own_at[k]] != b[s_own_at[k]]) {
            return TRUE;
        }
    }
    return FALSE;
}

// the villagers nobody watches that this game steps about the town
static int mp_npc_walked_here(int i) {
    return i < ANIMAL_NUM_MAX && s_s2.owner[i] == MP_NOBODY && s_s2.walker == mp_lobby_self_slot();
}

// guest: what this game decided for the villagers it moves goes to the host, which keeps it (for the ones it
// only steps about unseen, just whether they're home)
static void mp_guest_send_own(void) {
    int self = mp_lobby_self_slot();
    int i;

    for (i = 0; i < MP_NPC_N; i++) {
        Animal_c* animal = mp_npc_animal(i);
        const u8* now;
        const u8* heard;
        u32 off;
        u8 msg[4 + MP_OWN_N];
        int k;

        if (animal == NULL || s_s2.puppet[i] || (s_s2.owner[i] != self && !mp_npc_walked_here(i))) {
            continue;
        }
        now = (const u8*)animal + MP_OWN_BEGIN;
        off = (u32)((u8*)animal - mp_save_base()) + MP_OWN_BEGIN;
        heard = mp_world_heard(off, MP_OWN_LEN);
        if (heard == NULL || (s_s2.owner[i] == self ? !mp_own_differs(now, heard)
                                                    : now[MP_OWN_AT(is_home)] == heard[MP_OWN_AT(is_home)])) {
            continue;
        }
        msg[0] = MP_M_NPC_OWN;
        msg[1] = (u8)i;
        mp_put16(msg + 2, animal->id.npc_id);
        for (k = 0; k < MP_OWN_N; k++) {
            msg[4 + k] = now[s_own_at[k]];
            mp_world_heard_mine(off + s_own_at[k], 1);
        }
        mp_send_up(msg, (int)sizeof(msg));
    }
}

void mp_npc_stand_replaced(int slot) {
    if (slot >= 0 && slot < mNW_CLOTH_DESIGN_NUM) {
        s_s2.stand_ms[slot] = pc_mp_now_ms() | 1;
    }
}

// a design from a stand that changed since, on a villager the host has already changed back
static int mp_own_stale_design(const Animal_c* animal, const u8* p) {
    mActor_name_t cloth = (mActor_name_t)(p[4 + MP_OWN_CLOTH] | (p[4 + MP_OWN_CLOTH + 1] << 8));
    int slot = p[4 + MP_OWN_ORIG] & 3;

    return cloth == RSV_CLOTH && animal->cloth != RSV_CLOTH && s_s2.stand_ms[slot] != 0 &&
           pc_mp_now_ms() - s_s2.stand_ms[slot] < MP_STAND_STALE_MS;
}

// host: the game moving this villager took it home, brought it out, or changed its mood or clothes (the game
// stepping it about unseen only takes it home)
static void mp_host_on_own(int from, const u8* p) {
    int i = p[1];
    Animal_c* animal = (i < MP_NPC_N) ? mp_npc_animal(i) : NULL;
    u8* dst;
    int stale;
    int k;

    if (animal == NULL || animal->id.npc_id != (u16)mp_get16(p + 2)) {
        return;
    }
    if (s_s2.owner[i] == MP_NOBODY && s_s2.walker == from && i < ANIMAL_NUM_MAX) {
        animal->is_home = p[4 + MP_OWN_HOME] != 0;
        mp_world_host_took((u32)((u8*)&animal->is_home - mp_save_base()), 1, from);
        return;
    }
    if (s_s2.owner[i] != from && !mp_npc_held_by(from, i)) {
        return;
    }
    dst = (u8*)animal + MP_OWN_BEGIN;
    stale = mp_own_stale_design(animal, p);
    for (k = 0; k < MP_OWN_N; k++) {
        // a report sent before that game heard a stand changed would put the old design back on
        if (stale && (s_own_at[k] == MP_OWN_AT(cloth) || s_own_at[k] == MP_OWN_AT(cloth) + 1 ||
                      s_own_at[k] == MP_OWN_AT(cloth_original_id))) {
            continue;
        }
        dst[s_own_at[k]] = p[4 + k];
    }
    animal->is_home = animal->is_home != 0;
    mp_world_host_took((u32)((u8*)animal - mp_save_base()) + MP_OWN_BEGIN, MP_OWN_LEN, from);
}

// after the actors: map this game's characters, report the ones it moves, run the host's passes
static void mp_ev_send_records(GAME_PLAY* play);
static void mp_ev_show(GAME_PLAY* play);
static void mp_ev_held_steps(GAME_PLAY* play);
static void mp_ev_apply_pend(GAME_PLAY* play);
static void mp_guest_send_walk(void);

static void mp_npc_collect(GAME_PLAY* play) {
    ACTOR* actor;
    int i;

    memset(s_s2.actor, 0, sizeof(s_s2.actor));
    s_s2.fx_idx = -1;
    for (actor = play->actor_info.list[ACTOR_PART_NPC].actor; actor != NULL; actor = actor->next_actor) {
        i = mp_npc_idx(actor);
        // (one deleted this frame is gone, though the list still holds it until the next actor pass)
        if (i >= 0 && mp_npc_scene_ok(i) && (actor->mv_proc != NULL || actor->dw_proc != NULL)) {
            s_s2.actor[i] = actor;
        }
    }
    for (i = 0; i < MP_NPC_ALL; i++) {
        if (s_s2.standin[i] != NULL && s_s2.standin[i] != s_s2.actor[i]) {
            s_s2.standin[i] = NULL;
        }
    }
}

// the look of the one bringing another player a present: this screen's copy of it wears the same
void mp_npc_present_look(const unsigned char* body, int len) {
    mNpc_MaskNpc_c* mask;
    u16 look;

    if (len < 3) {
        return;
    }
    look = (u16)mp_get16(body + 1);
    mask = mNpc_GetSameMaskNpc(SP_NPC_PRESENT_NPC);
    if (mask != NULL && mask->npc_id == look) {
        return;
    }
    // (a copy up already in another look goes; the next one comes in this one)
    if (mask != NULL) {
        int i = mp_sp_find(SP_NPC_PRESENT_NPC);

        if (i >= 0 && s_s2.standin[i] != NULL) {
            Actor_delete(s_s2.standin[i]);
            s_s2.standin[i] = NULL;
            s_s2.actor[i] = NULL;
        }
        mask->in_use = FALSE;
        mask->mask_id = EMPTY_NO;
    }
    mNpc_RegistMaskNpc(SP_NPC_PRESENT_NPC, look, EMPTY_NO);
}

// Pete comes by for one player's game, and a present to one player's door; a copy on the others' screens follows
// either while it's near them
static void mp_npc_copy(GAME_PLAY* play, u16 sp_id, int birth);

static void mp_npc_copies(GAME_PLAY* play) {
    mp_npc_copy(play, SP_NPC_POST_MAN, aPMAN_BIRTH_PLAYER_HOUSE);
    // (only once its look is known here)
    if (mNpc_GetSameMaskNpc(SP_NPC_PRESENT_NPC) != NULL) {
        mp_npc_copy(play, SP_NPC_PRESENT_NPC, -1);
    }
}

static void mp_npc_copy(GAME_PLAY* play, u16 sp_id, int birth) {
    PLAYER_ACTOR* me = GET_PLAYER_ACTOR(play);
    int i = mp_sp_find(sp_id);
    ACTOR* copy;
    xyz_t pos;
    int bx = 0;
    int bz = 0;
    int pbx = 0;
    int pbz = 0;
    int near;

    if (i < 0 || me == NULL || Save_Get(scene_no) != SCENE_FG) {
        return;
    }
    copy = s_s2.standin[i];
    near = s_s2.owner[i] != MP_NOBODY && s_s2.owner[i] != mp_lobby_self_slot() && mp_seen_fresh(i) &&
           s_s2.seen[i].scene == SCENE_FG;
    if (near) {
        pos.x = s_s2.seen[i].pose.x;
        pos.y = s_s2.seen[i].pose.y;
        pos.z = s_s2.seen[i].pose.z;
        near = mp_town_block(&pos, &bx, &bz) && mp_town_block(&me->actor_class.world.position, &pbx, &pbz) &&
               bx - pbx <= 1 && pbx - bx <= 1 && bz - pbz <= 1 && pbz - bz <= 1;
    }
    if (copy != NULL) {
        // gone on its screen, or this player walked off as his own check would see
        if (!near) {
            Actor_delete(copy);
            s_s2.standin[i] = NULL;
            s_s2.actor[i] = NULL;
        }
        return;
    }
    // (made in this player's own acre, where the game makes him)
    if (near && s_s2.actor[i] == NULL && bx == pbx && bz == pbz && CLIP(npc_clip) != NULL &&
        CLIP(npc_clip)->setupActor_proc != NULL) {
        int ux = (int)((pos.x - bx * mFI_BK_WORLDSIZE_X_F) / mFI_UT_WORLDSIZE_X_F);
        int uz = (int)((pos.z - bz * mFI_BK_WORLDSIZE_Z_F) / mFI_UT_WORLDSIZE_Z_F);

        ux = ux < 0 ? 0 : (ux >= UT_X_NUM ? UT_X_NUM - 1 : ux);
        uz = uz < 0 ? 0 : (uz >= UT_Z_NUM ? UT_Z_NUM - 1 : uz);
        if (CLIP(npc_clip)->setupActor_proc(play, sp_id, -1, -1, birth, bx, bz, ux, uz)) {
            copy = Actor_info_fgName_search(&play->actor_info, sp_id, ACTOR_PART_NPC);
            if (copy != NULL) {
                mp_npc_standin(copy);
                s_s2.actor[i] = copy;
            }
        }
    }
}

static void mp_ev_torn_record(int k) {
    if (k == 0) {
        aTKN0_mp_torn();
    } else if (k == 1) {
        aTNN0_mp_torn();
    } else {
        aHN0_mp_torn();
    }
}

// the others gone: what this screen followed has carried on here (its characters and controls took over as
// they resumed); an event it didn't have out ends as if its runner walked off
static void mp_ev_settle(GAME_PLAY* play) {
    int k;

    for (k = 0; k < MP_EV_AREAS; k++) {
        u8 own = s_s2.ev_group[k];
        ACTOR* actor;

        if (own == MP_NOBODY) {
            continue;
        }
        s_s2.ev_group[k] = MP_NOBODY;
        s_s2.ev_step_have[k] = FALSE;
        for (actor = play->actor_info.list[ACTOR_PART_NPC].actor; actor != NULL; actor = actor->next_actor) {
            if (actor->npc_id == s_ev_area[k].first) {
                break;
            }
        }
        if (mp_is_host() && own != 0 && actor == NULL) {
            mp_ev_torn_record(k);
        }
    }
}

void mp_npc_post(GAME_PLAY* play) {
    u8 msg[MP_STATE_MAX];
    int i;

    if (!mp_town_shared()) {
        ACTOR* actor;

        mp_ev_settle(play);
        // copies standing in for another player's characters (Pete, the fairy) go with the others; only ones still
        // out in this scene, as the character they stood in for
        for (actor = play->actor_info.list[ACTOR_PART_NPC].actor; actor != NULL; actor = actor->next_actor) {
            int k = mp_npc_idx(actor);

            if (k >= 0 && s_s2.standin[k] == actor) {
                Actor_delete(actor);
            }
        }
        memset(s_s2.standin, 0, sizeof(s_s2.standin));
        memset(s_s2.actor, 0, sizeof(s_s2.actor));
        s_s2.fx_idx = -1;
        return;
    }
    mp_npc_collect(play);
    mp_npc_copies(play);
    s_s2.frame++;
    mp_ev_send_records(play);
    mp_ev_show(play);
    mp_ev_held_steps(play);
    mp_ev_apply_pend(play);
    if (!mp_is_host()) {
        mp_guest_send_own();
        if (s_s2.walker == mp_lobby_self_slot() && pc_mp_now_ms() - s_s2.walk_ms >= MP_LIST_MS) {
            s_s2.walk_ms = pc_mp_now_ms();
            mp_guest_send_walk();
        }
        // a claim the host never agreed to gives way to its list
        for (i = 0; i < MP_NPC_ALL; i++) {
            if (s_s2.claim_ms[i] != 0 && pc_mp_now_ms() - s_s2.claim_ms[i] >= MP_CLAIM_HOLD_MS) {
                s_s2.claim_ms[i] = 0;
                if (s_s2.owner[i] != s_s2.owner_heard[i] && s_npc.lock[mp_lobby_self_slot()] != mp_npc_id(i)) {
                    s_s2.seen[i].valid = FALSE;
                    s_s2.owner[i] = s_s2.owner_heard[i];
                }
            }
        }
    }
    if (mp_is_host()) {
        // a villager seen only on a visitor's screen is where that visitor's game says
        for (i = 0; i < MP_NPC_N; i++) {
            if (s_s2.actor[i] == NULL && mp_seen_fresh(i) && s_s2.seen[i].scene == SCENE_FG) {
                mNpc_NpcList_c* list = mp_npc_list(i);

                list->position.x = s_s2.seen[i].pose.x;
                list->position.y = s_s2.seen[i].pose.y;
                list->position.z = s_s2.seen[i].pose.z;
            }
        }
        if (s_s2.frame % MP_OWNER_EVERY == 0) {
            mp_host_owners(play);
        }
        if (pc_mp_now_ms() - s_s2.list_ms >= MP_LIST_MS) {
            s_s2.list_ms = pc_mp_now_ms();
            mp_host_send_list(-1);
        }
    }
    if (s_s2.frame % MP_POSE_TICK != 0) {
        return;
    }
    mp_mine_update();
    if (mp_is_host()) {
        int g;

        for (g = 1; g < MP_MAX_PEERS; g++) {
            unsigned int age;
            const mp_pstate_t* st;
            u16 scene = (u16)Save_Get(scene_no);
            int len;

            if (!mp_lobby_guest_arrived(g) || mp_lobby_guest_conn(g) < 0) {
                continue;
            }
            // each visitor hears about the characters where it stands
            st = mp_player_state(g, &age);
            if (st != NULL && age < MP_POSE_STALE_MS) {
                scene = st->scene;
            }
            len = mp_pose_msg(msg, g, scene);
            if (len > 0) {
                mp_lobby_send_state_to(mp_lobby_guest_conn(g), msg, len);
            }
        }
    } else if (mp_lobby_host_conn() >= 0) {
        int len = mp_pose_msg(msg, -1, (u16)Save_Get(scene_no));

        if (len > 0) {
            mp_lobby_send_state_to(mp_lobby_host_conn(), msg, len);
        }
    }
}

// the event characters this game runs for everyone, which go on behind its menu (not one in its own talk),
// and the copies of the other players and of what their games move, which stand where they really are
int pc_mp_menu_runs(void* actorx) {
    ACTOR* actor = (ACTOR*)actorx;
    int owner;
    int i;

    if (actor->id == mAc_PROFILE_MP_PLAYER) {
        return TRUE;
    }
    if (actor->part == ACTOR_PART_CONTROL) {
        // the race, the tug-of-war and the shrine line are run by these, and the town's balloon flies on
        return actor->id == mAc_PROFILE_TOKYOSO_CONTROL || actor->id == mAc_PROFILE_TUNAHIKI_CONTROL ||
               actor->id == mAc_PROFILE_HATUMODE_CONTROL ||
               (actor->id == mAc_PROFILE_FUUSEN && mp_cr_balloon_runner_here());
    }
    if (actor->part != ACTOR_PART_NPC || actor == mDemo_Get_talk_actor() || !mp_town_shared()) {
        return FALSE;
    }
    i = mp_npc_idx(actor);
    if (i < 0 || !mp_npc_scene_ok(i) || s_s2.standin[i] == actor) {
        return FALSE;
    }
    owner = s_s2.owner[i];
    // a copy just handed to another player (this one being in its menu) follows it too, once word is in
    return s_s2.puppet[i] || (i >= MP_NPC_N && owner == mp_lobby_self_slot()) ||
           (owner != MP_NOBODY && owner != mp_lobby_self_slot() && mp_seen_fresh(i) &&
            s_s2.seen[i].scene == (u16)Save_Get(scene_no));
}

// a visitor's own copy of a grouped event's record: first taken from the town's as the save stream had it,
// then the screen running the event keeps it
u8* pc_mp_ev_record(int type, int id) {
    int k;

    if (mp_is_host() || !mp_town_shared() || (k = mp_ev_k(type, id)) < 0) {
        return NULL;
    }
    if (!s_s2.ev_rec_have[k]) {
        u8* save = mEv_mp_real_area(type, id);

        if (save != NULL) {
            memcpy(s_s2.ev_rec[k], save, MP_EV_AREA_BYTES);
        } else {
            memset(s_s2.ev_rec[k], 0, MP_EV_AREA_BYTES);
        }
        s_s2.ev_rec_have[k] = TRUE;
    }
    return s_s2.ev_rec[k];
}

int pc_mp_ev_follows(int type) {
    int k = mp_ev_k(type, -1);
    u8 own;

    if (k < 0 || !mp_town_shared()) {
        return FALSE;
    }
    own = mp_ev_runner(k);
    return own != MP_NOBODY && own != mp_lobby_self_slot();
}

// its first character left this screen: this screen's copies go (the game's own teardown writes the record,
// which is the town's): the host ends the record only when nobody else has the event, a visitor never
int pc_mp_ev_npc0_gone(int type) {
    int k = mp_ev_k(type, -1);
    int n0;
    int s;

    if (k < 0 || !mp_town_shared()) {
        return FALSE;
    }
    s_s2.ev_torn[k] = TRUE;
    if (!mp_is_host()) {
        s_s2.ev_rec_have[k] = FALSE; // next time out, from the town's record again
        return TRUE;
    }
    n0 = mp_ev_npc0(k);
    if (s_s2.ev_group[k] != MP_NOBODY && s_s2.ev_group[k] != 0) {
        return TRUE;
    }
    for (s = 1; s < MP_MAX_PEERS; s++) {
        if (n0 >= 0 && mp_ev_has(s, n0)) {
            return TRUE;
        }
    }
    return FALSE;
}

int pc_mp_ev_torn(int type) {
    int k = mp_ev_k(type, -1);

    if (k < 0 || !mp_town_shared()) {
        return -1;
    }
    return s_s2.ev_torn[k];
}

void pc_mp_ev_begun(int type) {
    int k = mp_ev_k(type, -1);

    if (k >= 0) {
        int j;

        // the steps heard before this came out were another time's
        s_s2.ev_torn[k] = FALSE;
        s_s2.ev_step_have[k] = FALSE;
        s_s2.ev_ctl_follow[k] = FALSE;
        for (j = 0; j < s_ev_area[k].n; j++) {
            int i = mp_sp_find((u16)(s_ev_area[k].first + j));

            if (i >= 0) {
                s_s2.ev_npc_follow[i] = FALSE;
            }
        }
    }
}

static void mp_ev_step_call(ACTOR* actor, GAME_PLAY* play, mp_evstep_t* st, int mode) {
    NPC_ACTOR* npc = (NPC_ACTOR*)actor;
    cKF_FrameControl_c* fc = &npc->draw.main_animation.keyframe.frame_control;
    int anim = npc->draw.animation_id;
    f32 frame = fc->current_frame;

    switch (actor->id) {
        case mAc_PROFILE_TOKYOSO_NPC0:
            aTKN0_mp_step(actor, play, st, mode);
            break;
        case mAc_PROFILE_TOKYOSO_NPC1:
            aTKN1_mp_step(actor, play, st, mode);
            break;
        case mAc_PROFILE_TUNAHIKI_NPC0:
            aTNN0_mp_step(actor, play, st, mode);
            break;
        case mAc_PROFILE_TUNAHIKI_NPC1:
            aTNN1_mp_step(actor, play, st, mode);
            break;
        case mAc_PROFILE_HATUMODE_NPC0:
            aHN0_mp_step(actor, play, st, mode);
            break;
        case mAc_PROFILE_EV_SONCHO2:
            aES2_mp_step(actor, play, st, mode);
            break;
        case mAc_PROFILE_NPC_TOTAKEKE:
            aNTT_mp_step(actor, play, st, mode);
            break;
    }
    // the animation the step starts is the one this copy already plays from the runner's poses: it plays on
    if (mode == MP_EVS_TAKE && npc->draw.animation_id == anim) {
        fc->current_frame = frame;
    }
}

// this screen runs the event: its control's and characters' steps as they are now
static void mp_ev_capture(int k, GAME_PLAY* play) {
    mp_evstep_t* st = s_s2.ev_step[k];
    ACTOR* actor;
    int j;

    memset(st, 0, sizeof(s_s2.ev_step[k]));
    for (j = 0; j < MP_EV_STEPS; j++) {
        st[j].think = 0xFF;
    }
    for (actor = play->actor_info.list[ACTOR_PART_CONTROL].actor; actor != NULL; actor = actor->next_actor) {
        if (actor->id == mAc_PROFILE_TOKYOSO_CONTROL && s_ev_area[k].type == mEv_EVENT_SPORTS_FAIR_FOOT_RACE &&
            actor->ct_proc == NULL) {
            aTKC_mp_step(actor, &st[0], MP_EVS_GET);
        }
    }
    for (j = 0; j < s_ev_area[k].n && j + 1 < MP_EV_STEPS; j++) {
        int i = mp_sp_find((u16)(s_ev_area[k].first + j));

        actor = i >= 0 ? s_s2.actor[i] : NULL;
        if (actor != NULL && actor->ct_proc == NULL && !s_s2.puppet[i]) {
            mp_ev_step_call(actor, play, &st[1 + j], MP_EVS_GET);
        }
    }
    s_s2.ev_step_have[k] = TRUE;
}

// what a follower would notice of an event: its record, and where each step is and heads, not its timers
static void mp_ev_key(u8* key, const u8* rec, const mp_evstep_t* st) {
    int j;

    memcpy(key, rec, MP_EV_AREA_BYTES);
    key += MP_EV_AREA_BYTES;
    for (j = 0; j < MP_EV_STEPS; j++, key += 8) {
        key[0] = st[j].think;
        key[1] = st[j].a;
        key[2] = st[j].b;
        key[3] = st[j].c;
        memcpy(key + 4, &st[j].p0, 2);
        memcpy(key + 6, &st[j].p1, 2);
    }
}

// a single event character's step as a follower would notice it (not its timers)
static void mp_ev_skey(u8* key, const mp_evstep_t* st) {
    key[0] = st->think;
    key[1] = st->a;
    key[2] = st->b;
    key[3] = st->c;
    memcpy(key + 4, &st->p0, 2);
    memcpy(key + 6, &st->p1, 2);
}

// a single event character's step this screen sends: its own, or on the host one a visitor running it sent
static int mp_ev_single_out(int i, GAME_PLAY* play) {
    int self = mp_lobby_self_slot();
    ACTOR* actor = s_s2.actor[i];

    if (s_s2.owner[i] == self && actor != NULL && actor->ct_proc == NULL && !s_s2.puppet[i]) {
        memset(&s_s2.ev_sstep[i], 0, sizeof(mp_evstep_t));
        s_s2.ev_sstep[i].think = 0xFF;
        mp_ev_step_call(actor, play, &s_s2.ev_sstep[i], MP_EVS_GET);
        s_s2.ev_sstep_have[i] = s_s2.ev_sstep[i].think != 0xFF;
        s_s2.ev_sstep_ms[i] = pc_mp_now_ms();
        return s_s2.ev_sstep_have[i];
    }
    return mp_is_host() && s_s2.owner[i] != MP_NOBODY && s_s2.owner[i] != self && s_s2.ev_sstep_have[i] &&
           pc_mp_now_ms() - s_s2.ev_sstep_ms[i] < 1000;
}

// the screen running an event sends its record and steps: a visitor to the host, and the host to everyone (its
// own, and those of a visitor running one); a change a couple of frames apart
static void mp_ev_send_records(GAME_PLAY* play) {
    u8 msg[2 + 3 * MP_EV_GROUP_BYTES + 1 + MP_EV_SINGLES * (1 + sizeof(mp_evstep_t))];
    u8 key[3][MP_EV_KEY_BYTES];
    u8 skey[MP_EV_SINGLES][8];
    int self = mp_lobby_self_slot();
    int n = 2;
    int changed = FALSE;
    int singles_at;
    int i;
    int k;

    msg[0] = MP_S_EVAREA;
    msg[1] = 0;
    for (k = 0; k < MP_EV_AREAS; k++) {
        u8 own = mp_ev_runner(k);
        int n0 = mp_ev_npc0(k);
        u8* rec;

        if (own == MP_NOBODY) {
            continue;
        }
        if (own == self) {
            // while the event's first character is still out here
            if (n0 < 0 || s_s2.actor[n0] == NULL || s_s2.actor[n0]->mv_proc == NULL) {
                continue;
            }
            rec = mp_is_host() ? mEv_mp_real_area(s_ev_area[k].type, s_ev_area[k].id)
                               : (s_s2.ev_rec_have[k] ? s_s2.ev_rec[k] : NULL);
            if (rec == NULL) {
                continue;
            }
            mp_ev_capture(k, play);
        } else if (mp_is_host() && s_s2.ev_step_have[k]) {
            rec = mEv_mp_real_area(s_ev_area[k].type, s_ev_area[k].id);
            if (rec == NULL) {
                continue;
            }
        } else {
            continue;
        }
        mp_ev_key(key[k], rec, s_s2.ev_step[k]);
        changed |= memcmp(key[k], s_s2.ev_sent[k], MP_EV_KEY_BYTES) != 0;
        msg[1] |= (u8)(1 << k);
        memcpy(msg + n, rec, MP_EV_AREA_BYTES);
        memcpy(msg + n + MP_EV_AREA_BYTES, s_s2.ev_step[k], sizeof(s_s2.ev_step[k]));
        n += MP_EV_GROUP_BYTES;
    }
    // then the single event characters
    singles_at = n++;
    msg[singles_at] = 0;
    for (i = MP_NPC_N; i < MP_NPC_ALL && msg[singles_at] < MP_EV_SINGLES; i++) {
        if (!mp_ev_single(i) || !mp_ev_single_out(i, play)) {
            continue;
        }
        mp_ev_skey(skey[msg[singles_at]], &s_s2.ev_sstep[i]);
        changed |= memcmp(skey[msg[singles_at]], s_s2.ev_ssent[i], 8) != 0;
        msg[n] = (u8)i;
        memcpy(msg + n + 1, &s_s2.ev_sstep[i], sizeof(mp_evstep_t));
        n += 1 + (int)sizeof(mp_evstep_t);
        msg[singles_at]++;
    }
    // (steady, it still goes ten times a second: a screen taking the event over has timers near enough exact)
    if ((msg[1] == 0 && msg[singles_at] == 0) ||
        !(changed ? s_s2.frame - s_s2.ev_sent_frame >= 2 : pc_mp_now_ms() - s_s2.ev_sent_ms >= 100)) {
        return;
    }
    for (k = 0; k < MP_EV_AREAS; k++) {
        if (msg[1] & (1 << k)) {
            memcpy(s_s2.ev_sent[k], key[k], MP_EV_KEY_BYTES);
        }
    }
    for (k = 0, i = singles_at + 1; k < msg[singles_at]; k++, i += 1 + (int)sizeof(mp_evstep_t)) {
        memcpy(s_s2.ev_ssent[msg[i]], skey[k], 8);
    }
    s_s2.ev_sent_frame = s_s2.frame;
    s_s2.ev_sent_ms = pc_mp_now_ms();
    if (mp_is_host()) {
        int g;

        for (g = 1; g < MP_MAX_PEERS; g++) {
            if (mp_lobby_guest_arrived(g) && mp_lobby_guest_conn(g) >= 0) {
                mp_lobby_send_state_to(mp_lobby_guest_conn(g), msg, n);
            }
        }
    } else if (mp_lobby_host_conn() >= 0) {
        mp_lobby_send_state_to(mp_lobby_host_conn(), msg, n);
    }
}

// the host takes the record of an event a visitor runs as the town's; a visitor follows the one running it
void mp_npc_on_evarea(int conn, const unsigned char* data, int len) {
    int from = mp_is_host() ? mp_lobby_guest_slot(conn) + 1 : 0;
    int at = 2;
    int k;

    if (len < 2 || !mp_town_shared() || (mp_is_host() ? from <= 0 : conn != mp_lobby_host_conn())) {
        return;
    }
    for (k = 0; k < MP_EV_AREAS; k++) {
        u8 own;
        u8* rec = NULL;

        if (!(data[1] & (1 << k))) {
            continue;
        }
        if (at + MP_EV_GROUP_BYTES > len) {
            return;
        }
        own = mp_ev_runner(k);
        if (mp_is_host() ? own == from : own != mp_lobby_self_slot()) {
            if (!mp_is_host()) {
                rec = s_s2.ev_rec[k];
                s_s2.ev_rec_have[k] = TRUE;
            } else if ((rec = mEv_mp_real_area(s_ev_area[k].type, s_ev_area[k].id)) == NULL) {
                rec = mEv_reserve_save_area(s_ev_area[k].type, s_ev_area[k].id);
            }
            if (rec != NULL) {
                memcpy(rec, data + at, MP_EV_AREA_BYTES);
            }
            memcpy(s_s2.ev_step[k], data + at + MP_EV_AREA_BYTES, sizeof(s_s2.ev_step[k]));
            s_s2.ev_step_have[k] = TRUE;
            s_s2.ev_step_new[k] = TRUE;
        }
        at += MP_EV_GROUP_BYTES;
    }
    // the single event characters, each from the screen running it (the host only takes one from that screen)
    if (at < len) {
        int n = data[at++];

        for (k = 0; k < n && at + 1 + (int)sizeof(mp_evstep_t) <= len; k++, at += 1 + (int)sizeof(mp_evstep_t)) {
            int i = data[at];

            if (!mp_ev_single(i) || (mp_is_host() ? s_s2.owner[i] != from : s_s2.owner[i] == mp_lobby_self_slot())) {
                continue;
            }
            memcpy(&s_s2.ev_sstep[i], data + at + 1, sizeof(mp_evstep_t));
            s_s2.ev_sstep_have[i] = TRUE;
            s_s2.ev_sstep_new[i] = TRUE;
            s_s2.ev_sstep_ms[i] = pc_mp_now_ms();
        }
    }
}

// this screen's copies following another screen's event talk as the runner's do, and lead where its would
static void mp_ev_show(GAME_PLAY* play) {
    int k;
    int j;

    for (k = 0; k < MP_EV_AREAS; k++) {
        u8 own = mp_ev_runner(k);
        int mode = mp_ev_turn_on(k) ? MP_EVS_HUSH : MP_EVS_SHOW;

        if (!s_s2.ev_step_new[k] || own == MP_NOBODY || own == mp_lobby_self_slot()) {
            continue;
        }
        s_s2.ev_step_new[k] = FALSE;
        for (j = 0; j < s_ev_area[k].n && j + 1 < MP_EV_STEPS; j++) {
            int i = mp_sp_find((u16)(s_ev_area[k].first + j));
            ACTOR* actor = i >= 0 ? s_s2.actor[i] : NULL;

            if (actor != NULL && actor->ct_proc == NULL && s_s2.puppet[i] && s_s2.owner[i] == own &&
                actor != mDemo_Get_talk_actor()) {
                mp_ev_step_call(actor, play, &s_s2.ev_step[k][1 + j], mode);
            }
        }
    }
    for (j = MP_NPC_N; j < MP_NPC_ALL; j++) {
        ACTOR* actor = s_s2.actor[j];

        if (s_s2.ev_sstep_new[j] && actor != NULL && actor->ct_proc == NULL && s_s2.puppet[j] &&
            s_s2.owner[j] != mp_lobby_self_slot() && actor != mDemo_Get_talk_actor()) {
            s_s2.ev_sstep_new[j] = FALSE;
            mp_ev_step_call(actor, play, &s_s2.ev_sstep[j], MP_EVS_SHOW);
        }
    }
}

// a talk here held one of the characters of an event another screen runs: its step as the talk left it goes to
// that screen once it's handed back, and that screen carries on from there
static void mp_ev_held_steps(GAME_PLAY* play) {
    int self = mp_lobby_self_slot();
    int i;

    for (i = MP_NPC_N; i < MP_NPC_ALL; i++) {
        int k = mp_ev_group_of(i);
        ACTOR* actor = s_s2.actor[i];
        u8 own;

        if (k < 0) {
            continue;
        }
        own = mp_ev_runner(k);
        if (s_s2.owner[i] == self && own != self && own != MP_NOBODY && actor != NULL && actor->ct_proc == NULL &&
            !s_s2.puppet[i]) {
            mp_ev_step_call(actor, play, &s_s2.ev_held[i], MP_EVS_GET);
            s_s2.ev_held_have[i] = TRUE;
        } else if (s_s2.ev_held_have[i]) {
            u8 msg[3 + sizeof(mp_evstep_t)];

            s_s2.ev_held_have[i] = FALSE;
            if (own == self || own == MP_NOBODY) {
                continue;
            }
            msg[0] = MP_M_NPC_EVSTEP;
            msg[1] = (u8)k;
            msg[2] = (u8)(s_sp[i - MP_NPC_N].id - s_ev_area[k].first);
            memcpy(msg + 3, &s_s2.ev_held[i], sizeof(mp_evstep_t));
            if (!mp_is_host()) {
                mp_send_up(msg, (int)sizeof(msg));
            } else if (mp_lobby_guest_conn(own) >= 0) {
                mp_lobby_send_rel(mp_lobby_guest_conn(own), msg, (int)sizeof(msg));
            }
        }
    }
}

// runner: a character handed back after a talk on another screen carries on from where that talk left it
static int mp_ev_take_pend(ACTOR* actor, GAME_PLAY* play, int i) {
    if (s_s2.ev_pend_ms[i] == 0) {
        return FALSE;
    }
    if (pc_mp_now_ms() - s_s2.ev_pend_ms[i] > 3000) {
        s_s2.ev_pend_ms[i] = 0;
        return FALSE;
    }
    if (actor == mDemo_Get_talk_actor()) {
        return FALSE;
    }
    s_s2.ev_pend_ms[i] = 0;
    mp_ev_step_call(actor, play, &s_s2.ev_pend[i], MP_EVS_TAKE);
    return TRUE;
}

// (one whose word came after it was handed back)
static void mp_ev_apply_pend(GAME_PLAY* play) {
    int self = mp_lobby_self_slot();
    int i;

    for (i = MP_NPC_N; i < MP_NPC_ALL; i++) {
        int k = mp_ev_group_of(i);
        ACTOR* actor = s_s2.actor[i];

        if (k >= 0 && s_s2.ev_pend_ms[i] != 0 && s_s2.owner[i] == self && mp_ev_runner(k) == self &&
            actor != NULL && actor->ct_proc == NULL && !s_s2.puppet[i]) {
            mp_ev_take_pend(actor, play, i);
        }
    }
}

// an event's control: another screen may run the event, or this one may have just taken it over (the
// others gone included)
int pc_mp_ev_ctl(int type, mp_evstep_t* st) {
    int k = mp_ev_k(type, -1);
    int follow;
    int was;

    if (k < 0) {
        return MP_EVC_RUN;
    }
    follow = pc_mp_ev_follows(type);
    was = s_s2.ev_ctl_follow[k];
    s_s2.ev_ctl_follow[k] = (u8)follow;
    if (follow) {
        return MP_EVC_FOLLOW;
    }
    if (was && s_s2.ev_step_have[k]) {
        *st = s_s2.ev_step[k][0];
        return MP_EVC_TAKE;
    }
    return MP_EVC_RUN;
}

// a shared character this game moves again: one of an event's characters this screen now runs the event with
// carries on from the step the runner last had (or, back from a talk elsewhere, from where that talk left it)
void mp_npc_resumed(void* actorx, void* game) {
    ACTOR* actor = (ACTOR*)actorx;
    int i = mp_npc_idx(actor);
    int k = i >= 0 ? mp_ev_group_of(i) : -1;
    int follow;
    int j;

    if (i >= 0 && k < 0 && mp_ev_single(i)) {
        // a single one carries on from the step its last runner had (not one heard long ago: that starts over)
        follow = s_s2.ev_npc_follow[i];
        s_s2.ev_npc_follow[i] = FALSE;
        if (follow && s_s2.ev_sstep_have[i] && pc_mp_now_ms() - s_s2.ev_sstep_ms[i] < 60000 &&
            (s_s2.owner[i] == mp_lobby_self_slot() || !mp_town_shared()) && actor != mDemo_Get_talk_actor()) {
            mp_ev_step_call(actor, (GAME_PLAY*)game, &s_s2.ev_sstep[i], MP_EVS_TAKE);
        }
        return;
    }
    if (k < 0) {
        return;
    }
    follow = s_s2.ev_npc_follow[i];
    s_s2.ev_npc_follow[i] = FALSE;
    if (mp_town_shared() && mp_ev_runner(k) != mp_lobby_self_slot()) {
        return;
    }
    if (mp_ev_take_pend(actor, (GAME_PLAY*)game, i)) {
        return;
    }
    j = 1 + (mp_npc_id(i) - s_ev_area[k].first);
    if (follow && s_s2.ev_step_have[k] && j < MP_EV_STEPS && actor != mDemo_Get_talk_actor()) {
        mp_ev_step_call(actor, (GAME_PLAY*)game, &s_s2.ev_step[k][j], MP_EVS_TAKE);
    }
}

// a grouped event's characters another player is talking to right now (bit per character, first = bit 0)
int pc_mp_ev_talk_bits(int type) {
    int k = mp_ev_k(type, -1);
    int self = mp_lobby_self_slot();
    int bits = 0;
    int j;
    int s;

    if (k < 0 || !mp_town_shared()) {
        return 0;
    }
    for (j = 0; j < s_ev_area[k].n; j++) {
        for (s = 0; s < MP_MAX_PEERS; s++) {
            if (s != self && s_npc.lock[s] == (u16)(s_ev_area[k].first + j)) {
                bits |= 1 << j;
            }
        }
    }
    return bits;
}

// visitor: it runs one of the host's event characters for everyone, so its game runs on behind its menus
int mp_npc_runs_events(void) {
    int self = mp_lobby_self_slot();
    int i;

    if (mp_is_host() || !mp_town_shared()) {
        return FALSE;
    }
    for (i = MP_NPC_N; i < MP_NPC_ALL; i++) {
        if (s_sp[i - MP_NPC_N].host_only && s_s2.owner[i] == self && mp_npc_out_here(i)) {
            return TRUE;
        }
    }
    return FALSE;
}

// what the characters that went on behind this game's menu got up to
void mp_npc_menu_post(struct game_play_s* play_s) {
    if (!mp_town_shared()) {
        return;
    }
    mp_talk_result_tick();
    mp_npc_collect((GAME_PLAY*)play_s);
    s_s2.menu_ran = TRUE;
    s_s2.frame++;
    mp_ev_send_records((GAME_PLAY*)play_s);
    mp_ev_show((GAME_PLAY*)play_s);
    mp_ev_held_steps((GAME_PLAY*)play_s);
    mp_ev_apply_pend((GAME_PLAY*)play_s);
    if (s_s2.frame % MP_POSE_TICK == 0) {
        mp_mine_update();
    }
}

// a menu has this game's world stopped: a talk still winds down, and the host still decides who moves
// whom and passes the visitors' poses on
void mp_npc_still(GAME_PLAY* play) {
    u8 msg[MP_STATE_MAX];
    int ran = s_s2.menu_ran;
    int g;

    s_s2.menu_ran = FALSE;
    if (!mp_town_shared()) {
        return;
    }
    mp_porter_still();
    if (s_npc.state == MP_TALK_SETTLING && ++s_npc.settle_still > MP_STILL_SETTLE) {
        mp_talk_finish();
    }
    if (!mp_is_host()) {
        // a visitor tells the host what it has out behind its menu too (and how what it runs on there moves)
        if (++s_s2.still_frame % MP_POSE_TICK == 0 && mp_lobby_host_conn() >= 0) {
            int len = mp_pose_msg(msg, -1, (u16)Save_Get(scene_no));

            if (len > 0) {
                mp_lobby_send_state_to(mp_lobby_host_conn(), msg, len);
            }
        }
        return;
    }
    s_s2.still_frame++;
    // the host's own event characters wait for it, standing still on every screen, unless they go on
    if (!ran) {
        int i;

        for (i = MP_NPC_N; i < MP_NPC_ALL; i++) {
            mp_mine_t* m = &s_s2.mine[i];

            if (m->have && s_s2.owner[i] == 0 && pc_mp_now_ms() - m->sent_ms >= MP_POSE_KEEP_MS) {
                m->sent = m->pose;
                m->sent.vx = m->sent.vy = m->sent.vz = 0.0f;
                m->sent.speed = 0.0f;
                m->sent_frame = s_s2.frame;
                m->sent_ms = pc_mp_now_ms();
                m->due = 0xFF;
            }
        }
    }
    if (s_s2.still_frame % MP_OWNER_EVERY == 0) {
        mp_host_owners(play);
    }
    if (s_s2.still_frame % MP_POSE_TICK != 0) {
        return;
    }
    for (g = 1; g < MP_MAX_PEERS; g++) {
        unsigned int age;
        const mp_pstate_t* st = mp_player_state(g, &age);
        int len;

        if (!mp_lobby_guest_arrived(g) || mp_lobby_guest_conn(g) < 0 || st == NULL) {
            continue;
        }
        len = mp_pose_msg(msg, g, st->scene);
        if (len > 0) {
            mp_lobby_send_state_to(mp_lobby_guest_conn(g), msg, len);
        }
    }
}

void mp_npc_on_state(int conn, const unsigned char* data, int len) {
    int from = mp_is_host() ? mp_lobby_guest_slot(conn) + 1 : -1;
    int self = mp_lobby_self_slot();
    u16 scene;
    int n;
    int at;
    int k;

    if (len < MP_POSE_HDR || data[0] != MP_S_NPCS || data[4] != MP_SP_BYTES || (mp_is_host() && from <= 0)) {
        return;
    }
    scene = (u16)mp_get16(data + 1);
    if (mp_is_host() && from < MP_MAX_PEERS) {
        memcpy(s_s2.have[from], data + 5, MP_SP_BYTES);
        s_s2.have_ms[from] = pc_mp_now_ms();
    }
    n = data[3];
    at = MP_POSE_HDR;
    for (k = 0; k < n && at + MP_POSE_WIRE <= len; k++, at += MP_POSE_WIRE) {
        int i = data[at];

        // the host only takes a character's pose from the guest that owns it
        if (i >= MP_NPC_ALL || (mp_is_host() && s_s2.owner[i] != from) || s_s2.owner[i] == self) {
            continue;
        }
        mp_pose_unpack(data + at, &s_s2.seen[i].pose);
        s_s2.seen[i].valid = TRUE;
        s_s2.seen[i].scene = scene;
        s_s2.seen[i].ms = pc_mp_now_ms();
        s_s2.seen[i].frame = s_s2.frame;
        // the host passes it on to everyone but its owner
        s_s2.seen[i].due = (u8)~(1 << (from < 0 ? 0 : from));
    }
}

static void mp_guest_on_owners(const u8* p, int len) {
    int self = mp_lobby_self_slot();
    int i;

    if (len < 2 + MP_NPC_ALL + MP_EV_AREAS + 2 || p[1] != MP_NPC_ALL) {
        return;
    }
    s_s2.walker = p[2 + MP_NPC_ALL + MP_EV_AREAS];
    s_s2.mail = p[2 + MP_NPC_ALL + MP_EV_AREAS + 1];
    for (i = 0; i < MP_EV_AREAS; i++) {
        u8 was = s_s2.ev_group[i];

        s_s2.ev_group[i] = p[2 + MP_NPC_ALL + i];
        // an event nobody has out any more ended: this screen's copy of its record is the town's again, ended
        if (was != MP_NOBODY && s_s2.ev_group[i] == MP_NOBODY) {
            s_s2.ev_rec_have[i] = FALSE;
            mp_ev_torn_record(i);
        }
    }
    for (i = 0; i < MP_NPC_ALL; i++) {
        u16 id = mp_npc_id(i);

        s_s2.owner_heard[i] = p[2 + i];
        // our own talk holds its character until the host has heard about it, and so does a claim
        if (id != 0 && s_npc.lock[self] == id) {
            continue;
        }
        if (p[2 + i] == self) {
            s_s2.claim_ms[i] = 0;
        } else if (s_s2.claim_ms[i] != 0 && pc_mp_now_ms() - s_s2.claim_ms[i] < MP_CLAIM_HOLD_MS) {
            continue;
        }
        if (s_s2.owner[i] != p[2 + i]) {
            s_s2.seen[i].valid = FALSE;
        }
        s_s2.owner[i] = p[2 + i];
    }
}

static void mp_guest_on_list(const u8* p, int len) {
    int n = p[1];
    int k;

    for (k = 0; k < n && 2 + (k + 1) * 13 <= len; k++) {
        const u8* e = p + 2 + k * 13;
        int i = e[0];
        mNpc_NpcList_c* list;

        // a villager standing on this screen keeps its own spot, and so does one this screen steps unseen
        if (i >= MP_NPC_N || s_s2.actor[i] != NULL || mp_npc_walked_here(i)) {
            continue;
        }
        list = mp_npc_list(i);
        memcpy(&list->position.x, e + 1, 4);
        memcpy(&list->position.y, e + 5, 4);
        memcpy(&list->position.z, e + 9, 4);
    }
}

// host: where the visitor stepping the villagers nobody watches has them now; the host's list passes it on
static void mp_host_on_walk(const u8* p, int len) {
    int n = p[1];
    int k;

    for (k = 0; k < n && 2 + (k + 1) * 13 <= len; k++) {
        const u8* e = p + 2 + k * 13;
        int i = e[0];
        mNpc_NpcList_c* list;

        if (i >= ANIMAL_NUM_MAX || s_s2.owner[i] != MP_NOBODY || s_s2.actor[i] != NULL) {
            continue;
        }
        list = mp_npc_list(i);
        memcpy(&list->position.x, e + 1, 4);
        memcpy(&list->position.y, e + 5, 4);
        memcpy(&list->position.z, e + 9, 4);
    }
}

// visitor stepping the villagers nobody watches: where it has them, now and then
static void mp_guest_send_walk(void) {
    u8 msg[2 + ANIMAL_NUM_MAX * 13];
    int n = 0;
    int i;

    for (i = 0; i < ANIMAL_NUM_MAX; i++) {
        Animal_c* animal = mp_npc_animal(i);
        u8* e = msg + 2 + n * 13;

        if (animal == NULL || mNpc_CheckFreeAnimalPersonalID(&animal->id) || !mp_npc_walked_here(i) ||
            s_s2.actor[i] != NULL) {
            continue;
        }
        e[0] = (u8)i;
        memcpy(e + 1, &mp_npc_list(i)->position.x, 4);
        memcpy(e + 5, &mp_npc_list(i)->position.y, 4);
        memcpy(e + 9, &mp_npc_list(i)->position.z, 4);
        n++;
    }
    msg[0] = MP_M_NPC_LIST;
    msg[1] = (u8)n;
    if (n > 0) {
        mp_send_up(msg, 2 + n * 13);
    }
}

void mp_npc_claim(void* actorx) {
    int i = mp_npc_idx((ACTOR*)actorx);
    int self = mp_lobby_self_slot();
    u16 id = (i >= 0) ? mp_npc_id(i) : 0;
    u8 msg[4];

    // an event's group of characters stays whole on the screen running it
    if (id == 0 || !mp_town_shared() || s_s2.owner[i] == self || mp_npc_talk_locked(id) ||
        (i >= MP_NPC_N && s_sp[i - MP_NPC_N].host_only && !mp_is_host()) || mp_ev_group_of(i) >= 0) {
        return;
    }
    s_s2.owner[i] = (u8)self;
    s_s2.claim_ms[i] = pc_mp_now_ms() | 1;
    msg[0] = MP_M_NPC_CLAIM;
    msg[1] = (u8)self;
    mp_put16(msg + 2, id);
    if (mp_is_host()) {
        s_s2.seen[i].valid = FALSE;
        mp_host_send_owners(-1);
    } else {
        mp_send_up(msg, sizeof(msg));
    }
}

// a talk makes its talker the character's owner at once
static void mp_s2_own(u16 npc_id, int slot) {
    int i;

    for (i = 0; i < MP_NPC_ALL && npc_id != 0; i++) {
        if (mp_npc_id(i) == npc_id) {
            if (s_s2.owner[i] != (u8)slot) {
                s_s2.seen[i].valid = FALSE;
            }
            s_s2.owner[i] = (u8)slot;
        }
    }
}

// effects and sounds a shared character's own logic makes

void pc_mp_npc_fx(void* actorx) {
    int i;

    s_s2.fx_idx = -1;
    if (actorx == NULL || !mp_town_shared()) {
        return;
    }
    i = mp_npc_idx((ACTOR*)actorx);
    if (i < 0 || !mp_npc_scene_ok(i) || s_s2.owner[i] != mp_lobby_self_slot()) {
        return;
    }
    s_s2.fx_idx = i;
    s_s2.lsnd[i] = 0;
}

static void mp_npc_fx_send(int sub, int id, const void* pos, int prio, short angle, u16 item, short arg0,
                           short arg1) {
    u8 body[MP_NPC_FX_BODY];

    body[0] = MP_VFX_NPC_FX;
    body[1] = (u8)s_s2.fx_idx;
    body[2] = (u8)sub;
    body[3] = (u8)prio;
    mp_put16(body + 4, (u32)id);
    mp_put16(body + 6, (u16)angle);
    mp_put16(body + 8, item);
    mp_put16(body + 10, (u16)arg0);
    mp_put16(body + 12, (u16)arg1);
    if (pos != NULL) {
        memcpy(body + 14, pos, 12);
    } else {
        memset(body + 14, 0, 12);
    }
    mp_vfx_send(body, MP_NPC_FX_BODY);
}

int mp_npc_fx_effect(int id, const void* pos, int prio, short angle, unsigned short item, short arg0, short arg1) {
    if (s_s2.fx_idx < 0 || pos == NULL || id < 0) {
        return FALSE;
    }
    mp_npc_fx_send(0, id, pos, prio, angle, item, arg0, arg1);
    return TRUE;
}

int mp_npc_fx_sound(unsigned short id, const void* pos) {
    if (s_s2.fx_idx < 0 || pos == NULL) {
        return FALSE;
    }
    mp_npc_fx_send(1, id, pos, 0, 0, 0, 0, 0);
    return TRUE;
}

int mp_npc_fx_kill(int id, unsigned short item) {
    if (s_s2.fx_idx < 0 || id < 0) {
        return FALSE;
    }
    mp_npc_fx_send(2, id, NULL, 0, 0, item, 0, 0);
    return TRUE;
}

void pc_mp_npc_sys_sound(unsigned short id) {
    ACTOR* actor;

    // (not a talk's own: that's the talker's)
    if (s_s2.fx_idx < 0 || (actor = s_s2.actor[s_s2.fx_idx]) == NULL || mDemo_Check(mDemo_TYPE_TALK, actor) ||
        mDemo_Check(mDemo_TYPE_SPEAK, actor)) {
        return;
    }
    mp_npc_fx_send(3, id, &actor->world.position, 0, 0, 0, 0, 0);
}

void pc_mp_npc_level_sound(unsigned char id) {
    if (s_s2.fx_idx >= 0) {
        s_s2.lsnd[s_s2.fx_idx] = id;
    }
}

// only over a copy that follows the owner's; one this game moves makes its own
void mp_npc_fx_replay(struct game_play_s* play_s, const unsigned char* body, int len) {
    GAME_PLAY* play = (GAME_PLAY*)play_s;
    int i;
    xyz_t pos;

    if (len < MP_NPC_FX_BODY || (i = body[1]) >= MP_NPC_ALL || s_s2.actor[i] == NULL || !s_s2.puppet[i]) {
        return;
    }
    memcpy(&pos, body + 14, 12);
    switch (body[2]) {
        case 0:
            if (eEC_CLIP != NULL) {
                eEC_CLIP->effect_make_proc((int)mp_get16(body + 4), pos, body[3], (s16)mp_get16(body + 6),
                                           (GAME*)play, (u16)mp_get16(body + 8), (s16)mp_get16(body + 10),
                                           (s16)mp_get16(body + 12));
            }
            break;
        case 1:
            sAdo_OngenTrgStart((u16)mp_get16(body + 4), &pos);
            break;
        case 3: {
            PLAYER_ACTOR* me = GET_PLAYER_ACTOR(play);

            if (me != NULL && (me->actor_class.world.position.x - pos.x) * (me->actor_class.world.position.x - pos.x) +
                                      (me->actor_class.world.position.z - pos.z) *
                                          (me->actor_class.world.position.z - pos.z) <
                                  400.0f * 400.0f) {
                sAdo_SysTrgStart((u16)mp_get16(body + 4));
            }
            break;
        }
        case 2:
            if (eEC_CLIP != NULL) {
                eEC_CLIP->effect_kill_proc((int)mp_get16(body + 4), (u16)mp_get16(body + 8));
            }
            break;
    }
}

// messages

void mp_npc_on_rel(int conn, const unsigned char* data, int len) {
    int from = mp_is_host() ? mp_lobby_guest_slot(conn) + 1 : 0;

    if (len < 2 || (mp_is_host() && from <= 0) || (!mp_is_host() && conn != mp_lobby_host_conn())) {
        return;
    }
    switch (data[0]) {
        case MP_M_TALK_LOCK:
        case MP_M_TALK_UNLOCK:
            if (len >= 4 && data[1] < MP_MAX_PEERS && data[1] != mp_lobby_self_slot() &&
                (!mp_is_host() || data[1] == from)) {
                if (len >= 5 && data[4] == MP_PLOCK_ALL) {
                    memset(s_npc.plock[data[1]], 0, sizeof(s_npc.plock[data[1]]));
                } else if (len >= 5 && data[4] >= 1 && data[4] <= MP_PLOCKS) {
                    s_npc.plock[data[1]][data[4] - 1] = data[0] == MP_M_TALK_LOCK ? (u16)mp_get16(data + 2) : 0;
                } else {
                    s_npc.lock[data[1]] = data[0] == MP_M_TALK_LOCK ? (u16)mp_get16(data + 2) : 0;
                    if (data[0] == MP_M_TALK_LOCK && s_npc.lock[data[1]] != s_npc.lock[mp_lobby_self_slot()]) {
                        mp_s2_own(s_npc.lock[data[1]], data[1]);
                    }
                }
                if (mp_is_host()) {
                    mp_world_queue_guests(data, len, from); // (behind the talk's bytes)
                }
            }
            break;
        case MP_M_NPC_OWNERS:
            if (!mp_is_host()) {
                mp_guest_on_owners(data, len);
            }
            break;
        case MP_M_NPC_CLAIM:
            // guest: the host turned this game's claim down
            if (!mp_is_host() && len >= 4 && data[1] == MP_NOBODY) {
                int i;

                for (i = 0; i < MP_NPC_ALL; i++) {
                    if (mp_npc_id(i) == (u16)mp_get16(data + 2) && s_s2.claim_ms[i] != 0) {
                        s_s2.claim_ms[i] = 0;
                        s_s2.seen[i].valid = FALSE;
                        s_s2.owner[i] = s_s2.owner_heard[i];
                    }
                }
                break;
            }
            if (mp_is_host() && len >= 4 && data[1] == from) {
                u16 id = (u16)mp_get16(data + 2);
                int i = mp_sp_find(id);

                // an event record keeper stays with the host, and so does anyone talking
                if ((i >= 0 && s_sp[i - MP_NPC_N].host_only) || mp_npc_talk_locked(id) || s_npc.lock[0] == id) {
                    u8 no[4];

                    no[0] = MP_M_NPC_CLAIM;
                    no[1] = MP_NOBODY;
                    mp_put16(no + 2, id);
                    mp_lobby_send_rel(conn, no, 4);
                    break;
                }
                mp_s2_own(id, from);
                for (i = 0; i < MP_NPC_ALL; i++) {
                    if (mp_npc_id(i) == id) {
                        s_s2.claim_ms[i] = pc_mp_now_ms() | 1;
                    }
                }
                mp_host_send_owners(-1);
            }
            break;
        case MP_M_NPC_LIST:
            if (!mp_is_host() && len >= 2) {
                mp_guest_on_list(data, len);
            } else if (mp_is_host() && len >= 2 && from == s_s2.walker) {
                mp_host_on_walk(data, len);
            }
            break;
        case MP_M_TALK_RUNS:
            if (mp_is_host() && len >= 5) {
                mp_host_on_runs(from, data, len);
            }
            break;
        case MP_M_TALK_MEM:
            if (mp_is_host()) {
                mp_host_on_mem(data, len);
            }
            break;
        case MP_M_TALK_END:
            if (mp_is_host() && len >= 6) {
                u8 done[3];

                done[0] = MP_M_TALK_DONE;
                done[1] = data[4];
                done[2] = data[5];
                mp_lobby_send_rel(conn, done, 3);
            }
            break;
        case MP_M_TALK_DONE:
            if (!mp_is_host() && len >= 3) {
                int k;

                for (k = 0; k < MP_PLOCKS; k++) {
                    if (s_npc.held[k].anm != MP_ANM_NONE && s_npc.held[k].seq == (u16)mp_get16(data + 1)) {
                        s_npc.held[k].anm = MP_ANM_NONE;
                    }
                }
            }
            if (!mp_is_host() && len >= 3 && (u16)mp_get16(data + 1) == s_npc.defer_seq &&
                s_npc.state == MP_TALK_IDLE && s_npc.deferring && s_npc.defer_sent) {
                mp_talk_catch_up();
                s_npc.deferring = FALSE;
            }
            break;
        case MP_M_NPC_OWN:
            if (mp_is_host() && len >= 4 + MP_OWN_N) {
                mp_host_on_own(from, data);
            }
            break;
        case MP_M_NPC_MAIL:
            // the host's own post office does what a visitor's Pete did
            if (mp_is_host() && len >= 3) {
                if (data[1] == MP_MAIL_CAME) {
                    mPO_mp_post_man_came();
                } else if (data[1] == MP_MAIL_HOUSE &&
                           data[2] < sizeof(((Save_t*)0)->homes) / sizeof(((Save_t*)0)->homes[0])) {
                    mPO_delivery_one_address(data[2]);
                } else if (data[1] == MP_MAIL_ALL) {
                    mPO_delivery_all_address_proc();
                } else if (data[1] == MP_MAIL_FORCE) {
                    Common_Set(force_mail_delivery_flag, TRUE);
                }
            } else if (!mp_is_host() && len >= 3 && data[1] == MP_MAIL_FORCE && mp_npc_mail_here()) {
                Common_Set(force_mail_delivery_flag, TRUE);
            }
            break;
        case MP_M_NPC_LETTER:
            if (mp_is_host()) {
                mp_host_on_letter(from, data, len);
            }
            break;
        case MP_M_NPC_LETTER_ANS:
            if (!mp_is_host()) {
                mp_guest_on_letter_ans(data, len);
            }
            break;
        case MP_M_NPC_EVSTEP:
            if (len >= 3 + (int)sizeof(mp_evstep_t) && data[1] < MP_EV_AREAS && data[2] < s_ev_area[data[1]].n) {
                int k = data[1];
                int i = mp_sp_find((u16)(s_ev_area[k].first + data[2]));
                u8 own = mp_ev_runner(k);

                if (i < 0) {
                    break;
                }
                if (own == mp_lobby_self_slot()) {
                    memcpy(&s_s2.ev_pend[i], data + 3, sizeof(mp_evstep_t));
                    s_s2.ev_pend_ms[i] = pc_mp_now_ms() | 1;
                } else if (mp_is_host() && own != MP_NOBODY && own != from && mp_lobby_guest_conn(own) >= 0) {
                    mp_lobby_send_rel(mp_lobby_guest_conn(own), data, 3 + (int)sizeof(mp_evstep_t));
                }
            }
            break;
    }
}

// host: a new (or returning) visitor learns who is busy talking, who moves which villager and
// where they are; the list goes to everyone from the next frame, once the actors are current
void mp_npc_on_arrived(int conn) {
    int s;
    int k;

    mp_host_send_owners(conn);
    s_s2.list_ms = pc_mp_now_ms() - MP_LIST_MS;
    // every pose it should see goes to it next, changed or not
    s = mp_lobby_guest_slot(conn) + 1;
    if (s > 0 && s < MP_MAX_PEERS) {
        int i;

        for (i = 0; i < MP_NPC_ALL; i++) {
            s_s2.mine[i].due |= (u8)(1 << s);
            s_s2.seen[i].due |= (u8)(1 << s);
        }
    }
    for (s = 0; s < MP_MAX_PEERS; s++) {
        u8 msg[5];

        msg[0] = s_npc.lock[s] != 0 ? MP_M_TALK_LOCK : MP_M_TALK_UNLOCK;
        msg[1] = (u8)s;
        mp_put16(msg + 2, s_npc.lock[s]);
        msg[4] = 0;
        mp_lobby_send_rel(conn, msg, 5);
        for (k = 0; k < MP_PLOCKS; k++) {
            if (s_npc.plock[s][k] != 0) {
                msg[0] = MP_M_TALK_LOCK;
                mp_put16(msg + 2, s_npc.plock[s][k]);
                msg[4] = (u8)(1 + k);
                mp_lobby_send_rel(conn, msg, 5);
            }
        }
    }
}

void mp_npc_on_guest_gone(int slot) {
    if (slot > 0 && slot < MP_MAX_PEERS && s_npc.lock[slot] != 0) {
        s_npc.lock[slot] = 0;
        mp_lock_send(slot, 0, slot);
    }
    if (slot > 0 && slot < MP_MAX_PEERS) {
        memset(s_npc.plock[slot], 0, sizeof(s_npc.plock[slot]));
        mp_lock_send_ex(slot, 0, slot, MP_PLOCK_ALL);
    }
    if (slot > 0 && slot < MP_MAX_PEERS) {
        s_s2.have_ms[slot] = 0;
        s_let_last[slot].have = FALSE;
    }
}

// the play game ended: its characters, stand-ins and talk are gone from this screen
void mp_npc_play_gone(void) {
    int i;

    memset(s_s2.actor, 0, sizeof(s_s2.actor));
    memset(s_s2.standin, 0, sizeof(s_s2.standin));
    for (i = 0; i < MP_NPC_ALL; i++) {
        s_s2.mine[i].have = FALSE;
    }
    s_s2.fx_idx = -1;
    // (a talk the scene change cut off is over: its result goes out, its lock comes off)
    if (s_npc.state != MP_TALK_IDLE) {
        mp_talk_finish();
    }
    s_npc.actor = NULL;
}

// the host in an NES game: who moves whom is still decided (all visitors now), the villagers seen on visitors'
// screens stay where those games have them, and poses and the villager list still go round
void mp_npc_headless(void) {
    int i;

    if (!mp_is_host() || !mp_town_shared()) {
        return;
    }
    for (i = 0; i < MP_NPC_N; i++) {
        if (mp_seen_fresh(i) && s_s2.seen[i].scene == SCENE_FG) {
            mNpc_NpcList_c* list = mp_npc_list(i);

            list->position.x = s_s2.seen[i].pose.x;
            list->position.y = s_s2.seen[i].pose.y;
            list->position.z = s_s2.seen[i].pose.z;
        }
    }
    mp_npc_still(NULL);
    if (pc_mp_now_ms() - s_s2.list_ms >= MP_LIST_MS) {
        s_s2.list_ms = pc_mp_now_ms();
        mp_host_send_list(-1);
    }
}

void mp_npc_reset(void) {
    u8* pend = s_npc.pend;
    int k;

    memset(&s_npc, 0, sizeof(s_npc));
    s_npc.pend = pend; // (kept for the next session)
    for (k = 0; k < MP_PLOCKS; k++) {
        s_npc.held[k].anm = MP_ANM_NONE;
    }
    s_let.state = MP_LET_IDLE;
    s_let.taken = FALSE;
    memset(s_let_last, 0, sizeof(s_let_last));
    mp_s2_reset();
}

#endif
