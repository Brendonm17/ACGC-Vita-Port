// pc_mp_mod.c - multiplayer API for mods: the gameplay mod set, message channels, requests the host answers,
// ownership, host data mirrored to visitors, latest-value state and player events
#include "pc_mp.h"
#include "pc_mp_mod.h"

#ifdef VITA_MP

#include "types.h"

#include <string.h>

#define MP_MOD_REGS        32   // listeners of each kind
#define MP_MOD_HDR         8    // type, three small fields, then a channel or block id
#define MP_MOD_BURST       32   // messages a visitor may send at once...
#define MP_MOD_RATE_MS     50   // ...then one per this long
#define MP_MOD_PENDING     16   // requests waiting on the host
#define MP_MOD_OWNERS      128
#define MP_MOD_PLAYER_FNS  8
#define MP_MOD_BLOBS       8
#define MP_MOD_PAGE        256
#define MP_MOD_PAGES       (MP_MOD_BLOB_MAX / MP_MOD_PAGE)
#define MP_MOD_SCAN        4096 // block bytes the host compares a frame
#define MP_MOD_SEND_PAGES  8    // block pages a visitor is sent a frame, room allowing
#define MP_MOD_STATES      32   // latest values kept: this game's own, and on the host every visitor's
#define MP_MOD_FRESH_MS    500  // a new value rides along this long, so a lost packet or two loses nothing
#define MP_MOD_BUNDLE      600  // state bytes one packet carries
#define MP_MOD_STATE_EVERY 2    // frames between state bundles

typedef void (*mp_mod_fn)(void);

typedef struct {
    unsigned int channel;
    mp_mod_fn fn;
} mp_mod_reg_t;

typedef struct {
    unsigned int id;
    unsigned char* data;
    unsigned char* shadow; // host: what visitors were last sent
    int size;
    mp_mod_blob_fn changed;
    unsigned int need[MP_MAX_PEERS][(MP_MOD_PAGES + 31) / 32]; // host: pages each visitor still needs
} mp_mod_blob_t;

typedef struct {
    unsigned int channel;
    unsigned int ms; // when the value last changed
    unsigned char used;
    unsigned char from;
    unsigned char seq;
    unsigned char len;
    unsigned char data[MP_MOD_STATE_MAX];
} mp_mod_state_t;

static mp_mod_reg_t s_msg[MP_MOD_REGS];   // message listeners
static mp_mod_reg_t s_serve[MP_MOD_REGS]; // request handlers
static mp_mod_reg_t s_watch[MP_MOD_REGS]; // ownership watchers
static mp_mod_reg_t s_stl[MP_MOD_REGS];   // state listeners
static mp_mod_player_fn s_player_fns[MP_MOD_PLAYER_FNS];

static unsigned int s_set_hash;

// session as the tick last saw it
static int s_online;
static int s_self = -1;
static unsigned int s_present; // the other players in the town, a bit each

// host: each visitor's allowance, so one game can't flood the others through it
static struct {
    unsigned int ms;
    int tokens;
} s_bucket[MP_MAX_PEERS];

static struct {
    unsigned short id;
    unsigned char used;
    mp_mod_answer_fn done;
    void* user;
} s_pend[MP_MOD_PENDING];
static unsigned short s_req_id;

static struct {
    unsigned int channel;
    unsigned int key;
    unsigned char used;
    signed char owner;
} s_own[MP_MOD_OWNERS];

static mp_mod_blob_t s_blob[MP_MOD_BLOBS];
static int s_nblob;
static int s_scan_b;
static int s_scan_p;

static mp_mod_state_t s_st[MP_MOD_STATES];
static struct {
    unsigned int channel;
    unsigned char used;
    unsigned char from;
    unsigned char seq;
} s_seen[MP_MOD_STATES]; // the newest value heard per sender and channel
static unsigned int s_state_frame;

// small helpers

static void mp_mod_put32(unsigned char* p, unsigned int v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static unsigned int mp_mod_get32(const unsigned char* p) {
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | p[3];
}

static void mp_mod_hdr(unsigned char* msg, int type, int a, int b, int c, unsigned int id) {
    msg[0] = (unsigned char)type;
    msg[1] = (unsigned char)a;
    msg[2] = (unsigned char)b;
    msg[3] = (unsigned char)c;
    mp_mod_put32(msg + 4, id);
}

static int mp_mod_reg_set(mp_mod_reg_t* t, unsigned int channel, mp_mod_fn fn) {
    int free_i = -1;
    int i;

    for (i = 0; i < MP_MOD_REGS; i++) {
        if (t[i].fn != NULL && t[i].channel == channel) {
            t[i].fn = fn;
            return TRUE;
        }
        if (t[i].fn == NULL && free_i < 0) {
            free_i = i;
        }
    }
    if (fn == NULL) {
        return TRUE;
    }
    if (free_i < 0) {
        return FALSE;
    }
    t[free_i].channel = channel;
    t[free_i].fn = fn;
    return TRUE;
}

static mp_mod_fn mp_mod_reg_get(const mp_mod_reg_t* t, unsigned int channel) {
    int i;

    for (i = 0; i < MP_MOD_REGS; i++) {
        if (t[i].fn != NULL && t[i].channel == channel) {
            return t[i].fn;
        }
    }
    return NULL;
}

// guest: to the host
static int mp_mod_to_host(const unsigned char* msg, int len) {
    int conn = mp_lobby_host_conn();

    return conn >= 0 && mp_lobby_send_rel(conn, msg, len);
}

// host: to one visitor
static int mp_mod_to_guest(int slot, const unsigned char* msg, int len) {
    int conn = mp_lobby_guest_conn(slot);

    return conn >= 0 && mp_lobby_send_rel(conn, msg, len);
}

static int mp_mod_take_token(int slot) {
    unsigned int now = pc_mp_now_ms();
    int back;

    if (s_bucket[slot].ms == 0) {
        s_bucket[slot].ms = now;
        s_bucket[slot].tokens = MP_MOD_BURST;
    }
    back = (int)((now - s_bucket[slot].ms) / MP_MOD_RATE_MS);
    if (back > 0) {
        s_bucket[slot].tokens = (s_bucket[slot].tokens + back > MP_MOD_BURST) ? MP_MOD_BURST
                                                                               : s_bucket[slot].tokens + back;
        s_bucket[slot].ms += (unsigned int)back * MP_MOD_RATE_MS;
    }
    if (s_bucket[slot].tokens <= 0) {
        return FALSE;
    }
    s_bucket[slot].tokens--;
    return TRUE;
}

// the mod set

void mp_mod_gameplay_add(unsigned int mod_id, unsigned int mod_version) {
    unsigned int h = 2166136261u;
    int i;

    if (mp_active()) {
        pc_mp_log("[MP] mod %08X added during a session; it counts from the next one", mod_id);
    }
    for (i = 0; i < 4; i++) {
        h = (h ^ ((mod_id >> (i * 8)) & 0xFF)) * 16777619u;
    }
    for (i = 0; i < 4; i++) {
        h = (h ^ ((mod_version >> (i * 8)) & 0xFF)) * 16777619u;
    }
    // each mod adds its own number, so the order mods load in doesn't matter
    s_set_hash += h | 1;
}

unsigned int mp_mod_set_hash(void) {
    return s_set_hash;
}

// messages

int mp_mod_listen(unsigned int channel, mp_mod_recv_fn fn) {
    return mp_mod_reg_set(s_msg, channel, (mp_mod_fn)fn);
}

static void mp_mod_deliver(int from, const unsigned char* msg, int len) {
    mp_mod_recv_fn fn = (mp_mod_recv_fn)mp_mod_reg_get(s_msg, mp_mod_get32(msg + 4));

    if (fn != NULL) {
        fn(from, msg + MP_MOD_HDR, len - MP_MOD_HDR);
    }
}

// host: to one player, or to everyone but the sender; the host's own listener runs when it is a target
static int mp_mod_route(int from, const unsigned char* msg, int len) {
    int to = (msg[2] == 0xFF) ? MP_MOD_TO_ALL : msg[2];
    int ok = TRUE;
    int s;

    if (to == 0 || (to == MP_MOD_TO_ALL && from != 0)) {
        mp_mod_deliver(from, msg, len);
    }
    for (s = 1; s < MP_MAX_PEERS; s++) {
        if (s != from && (to == MP_MOD_TO_ALL || to == s) && mp_lobby_guest_conn(s) >= 0) {
            ok &= mp_mod_to_guest(s, msg, len);
        }
    }
    return ok;
}

int mp_mod_send(unsigned int channel, int to, const void* data, int len) {
    unsigned char msg[MP_MOD_HDR + MP_MOD_MSG_MAX];
    int self = mp_lobby_self_slot();

    if (!mp_active() || len < 0 || len > MP_MOD_MSG_MAX || (len > 0 && data == NULL) || to == self ||
        to < MP_MOD_TO_ALL || to >= MP_MAX_PEERS) {
        return FALSE;
    }
    mp_mod_hdr(msg, MP_M_MOD, self, (to == MP_MOD_TO_ALL) ? 0xFF : to, 0, channel);
    if (len > 0) {
        memcpy(msg + MP_MOD_HDR, data, len);
    }
    if (mp_is_host()) {
        return mp_mod_route(0, msg, MP_MOD_HDR + len);
    }
    return mp_mod_to_host(msg, MP_MOD_HDR + len);
}

// requests

int mp_mod_serve(unsigned int channel, mp_mod_serve_fn fn) {
    return mp_mod_reg_set(s_serve, channel, (mp_mod_fn)fn);
}

// host: its handler's verdict, as the result the asker gets
static int mp_mod_decide(int from, unsigned int channel, const void* req, int len, unsigned char* reply,
                         int* reply_len) {
    mp_mod_serve_fn fn = (mp_mod_serve_fn)mp_mod_reg_get(s_serve, channel);
    int result;

    *reply_len = 0;
    if (fn == NULL) {
        return MP_MOD_UNSERVED;
    }
    result = fn(from, req, len, reply, reply_len) ? MP_MOD_ACCEPTED : MP_MOD_REFUSED;
    if (*reply_len < 0 || *reply_len > MP_MOD_MSG_MAX) {
        *reply_len = 0;
    }
    return result;
}

int mp_mod_request(unsigned int channel, const void* data, int len, mp_mod_answer_fn done, void* user) {
    unsigned char msg[MP_MOD_HDR + MP_MOD_MSG_MAX];
    int i;

    if (!mp_active() || len < 0 || len > MP_MOD_MSG_MAX || (len > 0 && data == NULL)) {
        return FALSE;
    }
    if (mp_is_host()) {
        int rlen;
        int result = mp_mod_decide(0, channel, data, len, msg, &rlen);

        if (done != NULL) {
            done(result, msg, rlen, user);
        }
        return TRUE;
    }
    for (i = 0; i < MP_MOD_PENDING && s_pend[i].used; i++) {
    }
    if (i == MP_MOD_PENDING) {
        return FALSE;
    }
    if (++s_req_id == 0) {
        s_req_id = 1;
    }
    mp_mod_hdr(msg, MP_M_MOD_REQ, 0, s_req_id >> 8, s_req_id & 0xFF, channel);
    if (len > 0) {
        memcpy(msg + MP_MOD_HDR, data, len);
    }
    if (!mp_mod_to_host(msg, MP_MOD_HDR + len)) {
        return FALSE;
    }
    s_pend[i].id = s_req_id;
    s_pend[i].used = TRUE;
    s_pend[i].done = done;
    s_pend[i].user = user;
    return TRUE;
}

static void mp_mod_on_req(int slot, const unsigned char* data, int len) {
    unsigned char ans[MP_MOD_HDR + MP_MOD_MSG_MAX];
    unsigned int channel = mp_mod_get32(data + 4);
    int rlen;
    int result = mp_mod_decide(slot, channel, data + MP_MOD_HDR, len - MP_MOD_HDR, ans + MP_MOD_HDR, &rlen);

    mp_mod_hdr(ans, MP_M_MOD_ANS, (unsigned char)(signed char)result, data[2], data[3], channel);
    mp_mod_to_guest(slot, ans, MP_MOD_HDR + rlen);
}

static void mp_mod_on_ans(const unsigned char* data, int len) {
    unsigned short id = (unsigned short)((data[2] << 8) | data[3]);
    int i;

    for (i = 0; i < MP_MOD_PENDING; i++) {
        if (s_pend[i].used && s_pend[i].id == id) {
            mp_mod_answer_fn done = s_pend[i].done;
            void* user = s_pend[i].user;

            s_pend[i].used = FALSE;
            if (done != NULL) {
                done((signed char)data[1], data + MP_MOD_HDR, len - MP_MOD_HDR, user);
            }
            return;
        }
    }
}

// ownership

int mp_mod_watch_owners(unsigned int channel, mp_mod_owner_fn fn) {
    return mp_mod_reg_set(s_watch, channel, (mp_mod_fn)fn);
}

static int mp_mod_own_find(unsigned int channel, unsigned int key) {
    int i;

    for (i = 0; i < MP_MOD_OWNERS; i++) {
        if (s_own[i].used && s_own[i].channel == channel && s_own[i].key == key) {
            return i;
        }
    }
    return -1;
}

int mp_mod_owner(unsigned int channel, unsigned int key) {
    int i = mp_mod_own_find(channel, key);

    return (i >= 0) ? s_own[i].owner : -1;
}

// this game's copy of who owns a key; the channel's watcher hears about a change
static void mp_mod_own_set(unsigned int channel, unsigned int key, int owner) {
    int i = mp_mod_own_find(channel, key);
    int old = (i >= 0) ? s_own[i].owner : -1;
    mp_mod_owner_fn fn;

    if (old == owner) {
        return;
    }
    if (owner < 0) {
        s_own[i].used = FALSE;
    } else {
        if (i < 0) {
            for (i = 0; i < MP_MOD_OWNERS && s_own[i].used; i++) {
            }
            if (i == MP_MOD_OWNERS) {
                pc_mp_log("[MP] mods: ownership table full");
                return;
            }
            s_own[i].used = TRUE;
            s_own[i].channel = channel;
            s_own[i].key = key;
        }
        s_own[i].owner = (signed char)owner;
    }
    fn = (mp_mod_owner_fn)mp_mod_reg_get(s_watch, channel);
    if (fn != NULL) {
        fn(channel, key, owner);
    }
}

// host: a key's owner, to one visitor or to every one in the town (-1)
static void mp_mod_own_tell(int to, unsigned int channel, unsigned int key, int owner) {
    unsigned char msg[MP_MOD_HDR + 4];
    int s;

    mp_mod_hdr(msg, MP_M_MOD_OWNER, (owner < 0) ? 0xFF : owner, 0, 0, channel);
    mp_mod_put32(msg + MP_MOD_HDR, key);
    for (s = 1; s < MP_MAX_PEERS; s++) {
        if ((to < 0 || to == s) && (s_present & (1u << s))) {
            mp_mod_to_guest(s, msg, sizeof(msg));
        }
    }
}

// host: a claim or a letting go from any player (0 is the host itself)
static void mp_mod_own_decide(int slot, unsigned int channel, unsigned int key, int claim) {
    int cur = mp_mod_owner(channel, key);
    int next;

    if (claim) {
        if (cur >= 0 && cur != slot) {
            return; // someone else holds it
        }
        next = slot;
    } else {
        if (cur < 0 || (cur != slot && slot != 0)) {
            return; // only its owner or the host lets go
        }
        next = -1;
    }
    if (next != cur) {
        mp_mod_own_set(channel, key, next);
        mp_mod_own_tell(-1, channel, key, next);
    }
}

static void mp_mod_own_ask(unsigned int channel, unsigned int key, int claim) {
    unsigned char msg[MP_MOD_HDR + 4];

    mp_mod_hdr(msg, MP_M_MOD_CLAIM, claim, 0, 0, channel);
    mp_mod_put32(msg + MP_MOD_HDR, key);
    mp_mod_to_host(msg, sizeof(msg));
}

int mp_mod_claim(unsigned int channel, unsigned int key) {
    if (!mp_active()) {
        return FALSE;
    }
    if (mp_is_host()) {
        mp_mod_own_decide(0, channel, key, TRUE);
        return mp_mod_owner(channel, key) == 0;
    }
    mp_mod_own_ask(channel, key, TRUE);
    return TRUE;
}

void mp_mod_release(unsigned int channel, unsigned int key) {
    if (!mp_active()) {
        return;
    }
    if (mp_is_host()) {
        mp_mod_own_decide(0, channel, key, FALSE);
    } else {
        mp_mod_own_ask(channel, key, FALSE);
    }
}

// mirrored data

int mp_mod_blob(unsigned int blob_id, void* data, int size, mp_mod_blob_fn changed) {
    mp_mod_blob_t* b;
    int i;

    if (data == NULL || size <= 0 || size > MP_MOD_BLOB_MAX) {
        return FALSE;
    }
    for (i = 0; i < s_nblob && s_blob[i].id != blob_id; i++) {
    }
    if (i == s_nblob && s_nblob == MP_MOD_BLOBS) {
        return FALSE;
    }
    b = &s_blob[i];
    if (i < s_nblob && b->size != size) {
        mp_free(b->shadow);
        b->shadow = NULL;
    }
    if (b->shadow == NULL) {
        b->shadow = (unsigned char*)mp_alloc((unsigned int)size);
        if (b->shadow == NULL) {
            return FALSE;
        }
    }
    b->id = blob_id;
    b->data = (unsigned char*)data;
    b->size = size;
    b->changed = changed;
    memcpy(b->shadow, data, size);
    // (visitors already here need all of it)
    memset(b->need, 0xFF, sizeof(b->need));
    if (i == s_nblob) {
        s_nblob++;
    }
    return TRUE;
}

static void mp_mod_blob_need_all(int slot) {
    int k;

    for (k = 0; k < s_nblob; k++) {
        memset(s_blob[k].need[slot], 0xFF, sizeof(s_blob[k].need[slot]));
    }
}

static void mp_mod_blob_tick(void) {
    unsigned char msg[MP_MOD_HDR + MP_MOD_PAGE];
    int budget = MP_MOD_SCAN;
    int s;

    if (!mp_is_host() || s_nblob == 0) {
        return;
    }
    // this frame's share of the host's blocks, compared with what visitors were sent
    while (budget > 0) {
        mp_mod_blob_t* b = &s_blob[s_scan_b];
        int off = s_scan_p * MP_MOD_PAGE;
        int n = (b->size - off < MP_MOD_PAGE) ? b->size - off : MP_MOD_PAGE;

        if (memcmp(b->data + off, b->shadow + off, n) != 0) {
            memcpy(b->shadow + off, b->data + off, n);
            for (s = 1; s < MP_MAX_PEERS; s++) {
                b->need[s][s_scan_p >> 5] |= 1u << (s_scan_p & 31);
            }
        }
        budget -= n;
        if (off + n >= b->size) {
            s_scan_p = 0;
            s_scan_b = (s_scan_b + 1) % s_nblob;
        } else {
            s_scan_p++;
        }
    }
    // each visitor in the town gets the pages it still needs while its reliable queue has room
    for (s = 1; s < MP_MAX_PEERS; s++) {
        int conn = mp_lobby_guest_conn(s);
        int sent = 0;
        int k;

        if (conn < 0 || !(s_present & (1u << s))) {
            continue;
        }
        for (k = 0; k < s_nblob && sent < MP_MOD_SEND_PAGES; k++) {
            mp_mod_blob_t* b = &s_blob[k];
            int pages = (b->size + MP_MOD_PAGE - 1) / MP_MOD_PAGE;
            int p;

            for (p = 0; p < pages && sent < MP_MOD_SEND_PAGES; p++) {
                int off = p * MP_MOD_PAGE;
                int n = (b->size - off < MP_MOD_PAGE) ? b->size - off : MP_MOD_PAGE;

                if (!(b->need[s][p >> 5] & (1u << (p & 31)))) {
                    continue;
                }
                if (!mp_lobby_rel_fits(conn, MP_MOD_HDR + n, 1)) {
                    sent = MP_MOD_SEND_PAGES;
                    break;
                }
                mp_mod_hdr(msg, MP_M_MOD_BLOB, p >> 8, p & 0xFF, 0, b->id);
                memcpy(msg + MP_MOD_HDR, b->shadow + off, n);
                if (!mp_lobby_send_rel(conn, msg, MP_MOD_HDR + n)) {
                    sent = MP_MOD_SEND_PAGES;
                    break;
                }
                b->need[s][p >> 5] &= ~(1u << (p & 31));
                sent++;
            }
        }
    }
}

static void mp_mod_on_blob(const unsigned char* data, int len) {
    unsigned int id = mp_mod_get32(data + 4);
    int off = ((data[1] << 8) | data[2]) * MP_MOD_PAGE;
    int n = len - MP_MOD_HDR;
    int k;

    for (k = 0; k < s_nblob; k++) {
        mp_mod_blob_t* b = &s_blob[k];

        if (b->id == id) {
            if (off + n > b->size) {
                return;
            }
            memcpy(b->data + off, data + MP_MOD_HDR, n);
            if (b->changed != NULL) {
                b->changed(id, off, n);
            }
            return;
        }
    }
}

// latest-value state

int mp_mod_state_listen(unsigned int channel, mp_mod_recv_fn fn) {
    return mp_mod_reg_set(s_stl, channel, (mp_mod_fn)fn);
}

static mp_mod_state_t* mp_mod_state_slot(int from, unsigned int channel, int make) {
    int free_i = -1;
    int i;

    for (i = 0; i < MP_MOD_STATES; i++) {
        if (s_st[i].used && s_st[i].from == from && s_st[i].channel == channel) {
            return &s_st[i];
        }
        if (!s_st[i].used && free_i < 0) {
            free_i = i;
        }
    }
    if (!make || free_i < 0) {
        return NULL;
    }
    s_st[free_i].used = TRUE;
    s_st[free_i].from = (unsigned char)from;
    s_st[free_i].channel = channel;
    s_st[free_i].seq = 0;
    s_st[free_i].len = 0;
    s_st[free_i].ms = 0;
    return &s_st[free_i];
}

int mp_mod_state_send(unsigned int channel, const void* data, int len) {
    mp_mod_state_t* st;

    if (!mp_active() || len < 0 || len > MP_MOD_STATE_MAX || (len > 0 && data == NULL)) {
        return FALSE;
    }
    st = mp_mod_state_slot(mp_lobby_self_slot(), channel, TRUE);
    if (st == NULL) {
        return FALSE;
    }
    if (len > 0) {
        memcpy(st->data, data, len);
    }
    st->len = (unsigned char)len;
    st->seq++;
    st->ms = pc_mp_now_ms() | 1;
    return TRUE;
}

// a newer value than the last one heard from that player on that channel
static int mp_mod_state_newer(int from, unsigned int channel, int seq) {
    int free_i = -1;
    int i;

    for (i = 0; i < MP_MOD_STATES; i++) {
        if (s_seen[i].used && s_seen[i].from == from && s_seen[i].channel == channel) {
            if ((signed char)(seq - s_seen[i].seq) <= 0) {
                return FALSE;
            }
            s_seen[i].seq = (unsigned char)seq;
            return TRUE;
        }
        if (!s_seen[i].used && free_i < 0) {
            free_i = i;
        }
    }
    if (free_i >= 0) {
        s_seen[free_i].used = TRUE;
        s_seen[free_i].from = (unsigned char)from;
        s_seen[free_i].channel = channel;
        s_seen[free_i].seq = (unsigned char)seq;
    }
    return TRUE;
}

// the fresh values this game passes on to one player (-1: a visitor's own, to the host)
static int mp_mod_state_pack(unsigned char* out, int to, unsigned int now) {
    int pos = 2;
    int count = 0;
    int i;

    out[0] = MP_S_MOD;
    for (i = 0; i < MP_MOD_STATES; i++) {
        mp_mod_state_t* st = &s_st[i];

        if (!st->used || st->ms == 0 || st->from == to || now - st->ms >= MP_MOD_FRESH_MS) {
            continue;
        }
        if (pos + 7 + st->len > MP_MOD_BUNDLE) {
            break;
        }
        out[pos] = st->from;
        out[pos + 1] = st->seq;
        out[pos + 2] = st->len;
        mp_mod_put32(out + pos + 3, st->channel);
        memcpy(out + pos + 7, st->data, st->len);
        pos += 7 + st->len;
        count++;
    }
    out[1] = (unsigned char)count;
    return (count > 0) ? pos : 0;
}

static void mp_mod_state_tick(void) {
    unsigned char buf[MP_MOD_BUNDLE];
    unsigned int now = pc_mp_now_ms();
    int n;
    int s;

    if (++s_state_frame % MP_MOD_STATE_EVERY != 0) {
        return;
    }
    if (mp_is_host()) {
        for (s = 1; s < MP_MAX_PEERS; s++) {
            int conn = mp_lobby_guest_conn(s);

            if (conn >= 0 && (s_present & (1u << s)) && (n = mp_mod_state_pack(buf, s, now)) > 0) {
                mp_lobby_send_state_to(conn, buf, n);
            }
        }
    } else if (mp_lobby_host_conn() >= 0 && (n = mp_mod_state_pack(buf, -1, now)) > 0) {
        mp_lobby_send_state_to(mp_lobby_host_conn(), buf, n);
    }
}

void mp_mod_on_state(int conn, const unsigned char* data, int len) {
    int host = mp_is_host();
    int g = host ? mp_lobby_guest_slot(conn) : -1;
    int self = mp_lobby_self_slot();
    int count;
    int pos = 2;

    if (len < 2 || (host && g < 0) || (!host && conn != mp_lobby_host_conn())) {
        return;
    }
    count = data[1];
    while (count-- > 0 && pos + 7 <= len) {
        // (the host trusts the connection, not the bytes)
        int from = host ? g + 1 : data[pos];
        int seq = data[pos + 1];
        int n = data[pos + 2];
        unsigned int channel = mp_mod_get32(data + pos + 3);
        const unsigned char* v = data + pos + 7;
        mp_mod_recv_fn fn;

        if (pos + 7 + n > len || n > MP_MOD_STATE_MAX || from >= MP_MAX_PEERS || from == self) {
            return;
        }
        pos += 7 + n;
        if (!mp_mod_state_newer(from, channel, seq)) {
            continue;
        }
        if (host) {
            // kept to pass on to the other visitors
            mp_mod_state_t* st = mp_mod_state_slot(from, channel, TRUE);

            if (st != NULL) {
                memcpy(st->data, v, n);
                st->len = (unsigned char)n;
                st->seq = (unsigned char)seq;
                st->ms = pc_mp_now_ms() | 1;
            }
        }
        fn = (mp_mod_recv_fn)mp_mod_reg_get(s_stl, channel);
        if (fn != NULL) {
            fn(from, v, n);
        }
    }
}

// player events

int mp_mod_on_player(mp_mod_player_fn fn) {
    int i;

    for (i = 0; i < MP_MOD_PLAYER_FNS; i++) {
        if (s_player_fns[i] == fn) {
            return TRUE;
        }
    }
    for (i = 0; i < MP_MOD_PLAYER_FNS; i++) {
        if (s_player_fns[i] == NULL) {
            s_player_fns[i] = fn;
            return TRUE;
        }
    }
    return FALSE;
}

static void mp_mod_fire(int event, int slot) {
    int i;

    for (i = 0; i < MP_MOD_PLAYER_FNS; i++) {
        if (s_player_fns[i] != NULL) {
            s_player_fns[i](event, slot);
        }
    }
}

static void mp_mod_forget(int slot) {
    int i;

    for (i = 0; i < MP_MOD_STATES; i++) {
        if (s_st[i].used && s_st[i].from == slot) {
            s_st[i].used = FALSE;
        }
        if (s_seen[i].used && s_seen[i].from == slot) {
            s_seen[i].used = FALSE;
        }
    }
}

// another player came into the town: the host brings it up to date
static void mp_mod_joined(int slot) {
    int i;

    if (!mp_is_host() || slot < 1) {
        return;
    }
    mp_mod_blob_need_all(slot);
    for (i = 0; i < MP_MOD_OWNERS; i++) {
        if (s_own[i].used) {
            mp_mod_own_tell(slot, s_own[i].channel, s_own[i].key, s_own[i].owner);
        }
    }
}

// another player left: its values go, and on the host its keys are let go
static void mp_mod_left(int slot) {
    int i;

    mp_mod_forget(slot);
    if (!mp_is_host() || slot < 1) {
        return;
    }
    for (i = 0; i < MP_MOD_OWNERS; i++) {
        if (s_own[i].used && s_own[i].owner == slot) {
            mp_mod_own_decide(0, s_own[i].channel, s_own[i].key, FALSE);
        }
    }
    for (i = 0; i < s_nblob; i++) {
        memset(s_blob[i].need[slot], 0, sizeof(s_blob[i].need[slot]));
    }
    s_bucket[slot].ms = 0;
}

// the session ended: the others are gone, answers won't come, nobody owns anything
static void mp_mod_offline(void) {
    int i;
    int s;

    if (!s_online) {
        return;
    }
    for (s = 0; s < MP_MAX_PEERS; s++) {
        if (s_present & (1u << s)) {
            s_present &= ~(1u << s);
            mp_mod_left(s);
            mp_mod_fire(MP_MOD_LEAVE, s);
        }
    }
    for (i = 0; i < MP_MOD_PENDING; i++) {
        if (s_pend[i].used) {
            mp_mod_answer_fn done = s_pend[i].done;

            s_pend[i].used = FALSE;
            if (done != NULL) {
                done(MP_MOD_LOST, NULL, 0, s_pend[i].user);
            }
        }
    }
    for (i = 0; i < MP_MOD_OWNERS; i++) {
        if (s_own[i].used) {
            mp_mod_own_set(s_own[i].channel, s_own[i].key, -1);
        }
    }
    memset(s_st, 0, sizeof(s_st));
    memset(s_seen, 0, sizeof(s_seen));
    memset(s_bucket, 0, sizeof(s_bucket));
    s_online = FALSE;
    mp_mod_fire(MP_MOD_OFFLINE, s_self);
    s_self = -1;
}

void mp_mod_reset(void) {
    mp_mod_offline();
}

// every frame: player events, then the host's mirrored data and everyone's state bundles
void mp_mod_tick(void) {
    unsigned int now_mask = 0;
    int s;

    if (!mp_active()) {
        mp_mod_offline();
        return;
    }
    if (!s_online) {
        s_online = TRUE;
        s_self = mp_lobby_self_slot();
        mp_mod_fire(MP_MOD_ONLINE, s_self);
    }
    for (s = 0; s < MP_MAX_PEERS; s++) {
        if (s != s_self && mp_player_in_town(s)) {
            now_mask |= 1u << s;
        }
    }
    for (s = 0; s < MP_MAX_PEERS; s++) {
        unsigned int bit = 1u << s;

        if ((now_mask & bit) && !(s_present & bit)) {
            s_present |= bit;
            mp_mod_joined(s);
            mp_mod_fire(MP_MOD_JOIN, s);
        } else if (!(now_mask & bit) && (s_present & bit)) {
            s_present &= ~bit;
            mp_mod_left(s);
            mp_mod_fire(MP_MOD_LEAVE, s);
        }
    }
    mp_mod_blob_tick();
    mp_mod_state_tick();
}

// reliable messages from the line

void mp_mod_on_rel(int conn, const unsigned char* data, int len) {
    unsigned char msg[MP_MOD_HDR + MP_MOD_MSG_MAX];
    int from;

    if (len < MP_MOD_HDR || len > MP_MOD_HDR + MP_MOD_MSG_MAX) {
        return;
    }
    if (!mp_is_host()) {
        if (conn != mp_lobby_host_conn()) {
            return;
        }
        switch (data[0]) {
            case MP_M_MOD:
                if (data[1] < MP_MAX_PEERS) {
                    mp_mod_deliver(data[1], data, len);
                }
                break;
            case MP_M_MOD_ANS:
                mp_mod_on_ans(data, len);
                break;
            case MP_M_MOD_OWNER:
                if (len >= MP_MOD_HDR + 4 && (data[1] < MP_MAX_PEERS || data[1] == 0xFF)) {
                    mp_mod_own_set(mp_mod_get32(data + 4), mp_mod_get32(data + MP_MOD_HDR),
                                   (data[1] == 0xFF) ? -1 : data[1]);
                }
                break;
            case MP_M_MOD_BLOB:
                mp_mod_on_blob(data, len);
                break;
        }
        return;
    }
    // (the sender is the connection's player, whatever the message says)
    from = mp_lobby_guest_slot(conn) + 1;
    if (from < 1 || !mp_mod_take_token(from)) {
        return;
    }
    switch (data[0]) {
        case MP_M_MOD:
            memcpy(msg, data, len);
            msg[1] = (unsigned char)from;
            mp_mod_route(from, msg, len);
            break;
        case MP_M_MOD_REQ:
            mp_mod_on_req(from, data, len);
            break;
        case MP_M_MOD_CLAIM:
            if (len >= MP_MOD_HDR + 4) {
                mp_mod_own_decide(from, mp_mod_get32(data + 4), mp_mod_get32(data + MP_MOD_HDR), data[1] != 0);
            }
            break;
    }
}

// session facts

int mp_mod_online(void) {
    return mp_active();
}

int mp_mod_is_host(void) {
    return mp_is_host();
}

int mp_mod_self(void) {
    return mp_active() ? mp_lobby_self_slot() : -1;
}

int mp_mod_present(int slot) {
    if (!mp_active() || slot < 0 || slot >= MP_MAX_PEERS) {
        return FALSE;
    }
    return slot == mp_lobby_self_slot() || mp_player_in_town(slot);
}

int mp_mod_town_writer(void) {
    return mp_town_writer_allowed();
}

#endif
