// pc_mp_lobby.c - the part of a session Porter talks to: finding stations, ringing
// them (hello / welcome / refuse), and keeping the host's line and guest list.
#include "pc_mp.h"

#ifdef VITA_MP

#include "m_common_data.h"
#include "m_land.h"
#include "m_private.h"
#include "pc_mp_mod.h"
#include "pc_mp_text_data.h"
#include "pc_settings.h"

#include <string.h>

// below 49152: the Vita refuses an app's bind inside the automatic port range (EACCES)
#define MP_PORT_DISC    19519
#define MP_PORT_GAME    19520
#define MP_SCAN_MS      3000
#define MP_SCAN_NEW_MS  9000 // ...right after the ad hoc network formed; done a moment after the first station
#define MP_QUERY_MS     500
#define MP_HELLO_MS     250
#define MP_HELLO_LAN_MS 5000
#define MP_HELLO_WAN_MS 8000
#define MP_ASK_MS       45000 // a caller the host is asked about waits this long for the answer
#define MP_RING_MS      5000  // a caller quiet this long has hung up
#define MP_HOLD_MS      60000 // a visitor in town keeps their seat through a dropped line this long
#define MP_CTRL_RATE    8     // queries and hellos a second from one address before the rest are ignored
#define MP_HEAP_NEED    (640 * 1024) // a new visitor's queue and snapshot, from multiplayer's own memory

enum {
    MP_CTRL_QUERY = 'Q',
    MP_CTRL_ADVERT = 'A',
    MP_CTRL_HELLO = 'H',
    MP_CTRL_WELCOME = 'W',
    MP_CTRL_REFUSE = 'R',
    MP_CTRL_BYE = 'X',
    MP_CTRL_PENDING = 'P', // host: still ringing, the host is deciding
};

enum {
    MP_REFUSE_VERSION = 1,
    MP_REFUSE_FULL,
    MP_REFUSE_SAME_TOWN,
    MP_REFUSE_RESIDENT,
    MP_REFUSE_DUPLICATE,
    MP_REFUSE_CLOSED,
    MP_REFUSE_DENIED,
    MP_REFUSE_MODS,
};

typedef struct {
    unsigned char town[MP_NAME_LEN];
    unsigned char host[MP_NAME_LEN];
    int guests;
    unsigned short land_id;
    mp_addr_t addr;
    unsigned int session_id;
} mp_scan_entry_t;

typedef struct {
    int used;
    int boarding; // has the town and is on the train over
    int arrived;
    int leaving;
    int conn;
    unsigned int conn_id;
    unsigned int nonce;
    mp_addr_t addr;
    PersonalID_c pid;
} mp_guest_t;

static struct {
    mp_link_t link;
    int opening; // the radio is still coming up (ad hoc dialog)
    int fresh;   // ...it came up for this look
    unsigned int start_ms;
    unsigned int last_query_ms;
    unsigned int found_ms; // the first station heard
    unsigned short nonce;
    int result;
    int count;
    mp_scan_entry_t entries[MP_SCAN_MAX];
} s_scan;

static struct {
    int active;
    int slot;   // our player slot at the host, from the welcome
    unsigned int pending_ms; // last "still ringing" from the host
    int target; // scan index, or -1 for a ticket
    mp_link_t link;
    int opening; // the radio is still coming up
    mp_addr_t addr;
    unsigned int nonce;
    unsigned int start_ms;
    unsigned int last_hello_ms;
    unsigned int deadline_ms;
    int result;
    int conn;
    unsigned int conn_id;
    mp_scan_entry_t info;
} s_join;

// host: the far-away caller the host is being asked about, and the last one turned away
static struct {
    int active;
    mp_addr_t addr;
    unsigned int nonce;
    unsigned int start_ms;
    unsigned int last_ms;
    int answer; // -1 while the host decides
    unsigned int serial; // the PA question asking it
    mp_addr_t denied_addr;
    unsigned int denied_nonce;
} s_ask;

static struct {
    int hosting;
    int opening;
    int mapping; // far away: the router is being asked to forward the port
    int closing;
    unsigned int closing_ms;
    mp_link_t link;
    int result;
    unsigned int start_ms;
    unsigned int session_id;
    int port_idx;
    unsigned char ticket[MP_TICKET_LEN];
    mp_guest_t guests[MP_MAX_PEERS - 1];
    unsigned char thanks[MP_NAME_LEN]; // a visitor whose train home just left, for Porter's thanks
    int thanks_due;
} s_host;

static void mp_name_set(unsigned char* dst, const char* src) {
    int i;

    for (i = 0; i < MP_NAME_LEN; i++) {
        dst[i] = (*src != '\0') ? (unsigned char)*src++ : ' ';
    }
}

int mp_ui_hosting(void) {
    return s_host.hosting && !s_host.closing;
}

int mp_ui_host_closing(void) {
    return s_host.hosting && s_host.closing;
}

mp_link_t mp_ui_host_link(void) {
    return s_host.link;
}

int mp_ui_host_ticket(unsigned char* out9) {
    if (!s_host.hosting || s_host.link == MP_LINK_ADHOC) {
        return FALSE;
    }
    memcpy(out9, s_host.ticket, 4);
    out9[4] = ' ';
    memcpy(out9 + 5, s_host.ticket + 4, 4);
    return TRUE;
}

int mp_ui_host_port(void) {
    return MP_PORT_GAME + s_host.port_idx;
}

int mp_ui_host_guest_count(void) {
    int n = 0;
    int i;

    for (i = 0; i < MP_MAX_PEERS - 1; i++) {
        n += s_host.guests[i].used;
    }
    return n;
}

void mp_ui_host_guest(int i, unsigned char* name, unsigned char* town) {
    int k;

    for (k = 0; k < MP_MAX_PEERS - 1; k++) {
        if (s_host.guests[k].used && i-- == 0) {
            memcpy(name, s_host.guests[k].pid.player_name, MP_NAME_LEN);
            memcpy(town, s_host.guests[k].pid.land_name, MP_NAME_LEN);
            return;
        }
    }
    mp_name_set(name, "");
    mp_name_set(town, "");
}

int mp_ui_scan_count(void) {
    return s_scan.count;
}

void mp_ui_scan_entry(int i, unsigned char* town, unsigned char* host, int* guests) {
    if (i < 0 || i >= s_scan.count) {
        return;
    }
    memcpy(town, s_scan.entries[i].town, MP_NAME_LEN);
    memcpy(host, s_scan.entries[i].host, MP_NAME_LEN);
    *guests = s_scan.entries[i].guests;
}

void mp_ui_join_info(unsigned char* town, unsigned char* host, int* others) {
    memcpy(town, s_join.info.town, MP_NAME_LEN);
    memcpy(host, s_join.info.host, MP_NAME_LEN);
    *others = s_join.info.guests;
}

unsigned short mp_ui_join_land_id(void) {
    return s_join.info.land_id;
}

// host: visitors in town, and trains on their way (the first one's town)
int mp_ui_host_arrived(void) {
    int n = 0;
    int i;

    for (i = 0; i < MP_MAX_PEERS - 1; i++) {
        n += s_host.guests[i].used && s_host.guests[i].arrived && !s_host.guests[i].leaving;
    }
    return n;
}

int mp_ui_host_inbound(unsigned char* town) {
    int n = 0;
    int i;

    for (i = 0; i < MP_MAX_PEERS - 1; i++) {
        if (s_host.guests[i].used && !s_host.guests[i].arrived) {
            if (n++ == 0) {
                memcpy(town, s_host.guests[i].pid.land_name, MP_NAME_LEN);
            }
        }
    }
    return n;
}

void mp_ui_host_visitor(int i, unsigned char* name, unsigned char* town) {
    int k;

    for (k = 0; k < MP_MAX_PEERS - 1; k++) {
        if (s_host.guests[k].used && s_host.guests[k].arrived && !s_host.guests[k].leaving && i-- == 0) {
            memcpy(name, s_host.guests[k].pid.player_name, MP_NAME_LEN);
            memcpy(town, s_host.guests[k].pid.land_name, MP_NAME_LEN);
            return;
        }
    }
    mp_name_set(name, "");
    mp_name_set(town, "");
}

// ...and a visitor whose train home just left, once
int mp_ui_host_thanks(unsigned char* name) {
    if (!s_host.thanks_due) {
        return FALSE;
    }
    s_host.thanks_due = FALSE;
    memcpy(name, s_host.thanks, MP_NAME_LEN);
    return TRUE;
}

// network backend

// which role currently owns the link and sockets
enum {
    MP_NET_OFF,
    MP_NET_GUEST,
    MP_NET_HOST,
};
static int s_net;

// wire helpers

typedef struct {
    unsigned char* p;
    int len;
    int cap;
} mp_wr_t;

typedef struct {
    const unsigned char* p;
    int len;
    int pos;
    int bad;
} mp_rd_t;

static void wr_u8(mp_wr_t* w, unsigned int v) {
    if (w->len < w->cap) {
        w->p[w->len++] = (unsigned char)v;
    }
}

static void wr_u16(mp_wr_t* w, unsigned int v) {
    wr_u8(w, v);
    wr_u8(w, v >> 8);
}

static void wr_u32(mp_wr_t* w, unsigned int v) {
    wr_u16(w, v);
    wr_u16(w, v >> 16);
}

static void wr_bytes(mp_wr_t* w, const void* src, int n) {
    int i;

    for (i = 0; i < n; i++) {
        wr_u8(w, ((const unsigned char*)src)[i]);
    }
}

static unsigned int rd_u8(mp_rd_t* r) {
    if (r->pos >= r->len) {
        r->bad = TRUE;
        return 0;
    }
    return r->p[r->pos++];
}

static unsigned int rd_u16(mp_rd_t* r) {
    unsigned int lo = rd_u8(r);

    return lo | (rd_u8(r) << 8);
}

static unsigned int rd_u32(mp_rd_t* r) {
    unsigned int lo = rd_u16(r);

    return lo | (rd_u16(r) << 16);
}

static void rd_bytes(mp_rd_t* r, void* dst, int n) {
    int i;

    for (i = 0; i < n; i++) {
        ((unsigned char*)dst)[i] = (unsigned char)rd_u8(r);
    }
}

static void wr_pid(mp_wr_t* w, const PersonalID_c* pid) {
    wr_bytes(w, pid->player_name, PLAYER_NAME_LEN);
    wr_bytes(w, pid->land_name, LAND_NAME_SIZE);
    wr_u16(w, pid->player_id);
    wr_u16(w, pid->land_id);
}

static void rd_pid(mp_rd_t* r, PersonalID_c* pid) {
    rd_bytes(r, pid->player_name, PLAYER_NAME_LEN);
    rd_bytes(r, pid->land_name, LAND_NAME_SIZE);
    pid->player_id = (u16)rd_u16(r);
    pid->land_id = (u16)rd_u16(r);
}

// names from the wire never carry control or newline codes into a message window
static void mp_sanitize_name(unsigned char* s, int n) {
    int i;

    for (i = 0; i < n; i++) {
        if (s[i] == 0x7F || s[i] == 0x80 || s[i] == 0xCD) {
            s[i] = ' ';
        }
    }
}

// net ownership

static void mp_on_event(const mp_event_t* ev);

static int s_net_adhoc; // the open transport is the ad hoc radio

static void mp_net_close(void) {
    mp_link_stop();
    if (s_net_adhoc) {
        vita_mp_adhoc_close();
    } else {
        vita_mp_udp_close();
    }
    s_net = MP_NET_OFF;
    s_net_adhoc = FALSE;
}

// MP_UI_BUSY while the ad hoc dialog is up; call again until it isn't
static int mp_net_open(int role, mp_link_t link) {
    static const mp_link_timing_t local = { 3000, 12000, 100 };
    static const mp_link_timing_t wan = { 5000, 20000, 200 };
    int adhoc = (link == MP_LINK_ADHOC);
    const mp_transport_t* tr;

    if (s_net == role && s_net_adhoc == adhoc) {
        return MP_UI_DONE;
    }
    if (s_net != MP_NET_OFF) {
        if (s_net != role) {
            return MP_UI_NOBIND;
        }
        mp_net_close(); // same role on the other radio
    }
    if (adhoc) {
        int res = vita_mp_adhoc_connect();

        if (res != MP_UI_DONE) {
            return res;
        }
        tr = vita_mp_adhoc_open(role == MP_NET_HOST, MP_PORT_DISC, MP_PORT_GAME);
        s_host.port_idx = 0;
    } else {
        int wifi;

        if (vita_mp_wifi_recovering()) {
            return MP_UI_WIFI_WAIT;
        }
        wifi = vita_mp_wifi_ready();
        if (wifi != MP_UI_DONE) {
            return wifi;
        }
        tr = vita_mp_udp_open(role == MP_NET_HOST, MP_PORT_DISC, MP_PORT_GAME, &s_host.port_idx);
    }
    s_net_adhoc = adhoc;
    if (tr == NULL || !mp_link_start(tr, link == MP_LINK_ONLINE ? &wan : &local, role == MP_NET_HOST ? 0 : 1,
                                     mp_on_event)) {
        pc_mp_log("[MP] %s", tr == NULL ? "no sockets" : "link: no memory");
        mp_net_close();
        return MP_UI_NOBIND;
    }
    s_net = role;
    return MP_UI_DONE;
}

// host

static void mp_host_drop_guest(int slot, const char* why);

// anyone can send station queries and hellos; a flood from one address is ignored
static int mp_ctrl_allowed(const mp_addr_t* from) {
    static struct {
        mp_addr_t addr;
        unsigned int since_ms;
        int count;
    } seen[8];
    unsigned int now = pc_mp_now_ms();
    int oldest = 0;
    int i;

    for (i = 0; i < 8; i++) {
        if (seen[i].count > 0 && mp_link_addr_equal(&seen[i].addr, from)) {
            if (now - seen[i].since_ms >= 1000) {
                seen[i].since_ms = now;
                seen[i].count = 0;
            }
            return ++seen[i].count <= MP_CTRL_RATE;
        }
        if ((s32)(seen[i].since_ms - seen[oldest].since_ms) < 0) {
            oldest = i;
        }
    }
    seen[oldest].addr = *from;
    seen[oldest].since_ms = now;
    seen[oldest].count = 1;
    return TRUE;
}

static int mp_host_check(const PersonalID_c* pid, const mp_addr_t* from, unsigned int nonce, unsigned int build_id,
                         unsigned int mod_hash, int* slot_out) {
    PersonalID_c probe;
    int free_slot = -1;
    int i;

    *slot_out = -1;
    if (build_id != pc_mp_build_id()) {
        return MP_REFUSE_VERSION;
    }
    if (mod_hash != mp_mod_set_hash()) {
        return MP_REFUSE_MODS;
    }
    if (!s_host.hosting || s_host.closing) {
        return MP_REFUSE_CLOSED;
    }
    probe = *pid;
    if (mLd_CheckCmpLand(probe.land_name, probe.land_id, Save_Get(land_info).name, Save_Get(land_info).id)) {
        return MP_REFUSE_SAME_TOWN;
    }
    for (i = 0; i < PLAYER_NUM; i++) {
        if (mPr_CheckCmpPersonalID(&probe, &Save_Get(private_data[i]).player_ID)) {
            return MP_REFUSE_RESIDENT;
        }
    }
    for (i = 0; i < MP_MAX_PEERS - 1; i++) {
        mp_guest_t* g = &s_host.guests[i];

        if (!g->used) {
            free_slot = (free_slot < 0) ? i : free_slot;
            continue;
        }
        if (mPr_CheckCmpPersonalID(&probe, &g->pid)) {
            // a resent hello from the same guest just gets its welcome again
            if (mp_link_addr_equal(&g->addr, from) && g->nonce == nonce) {
                *slot_out = i;
                return 0;
            }
            if (mp_link_quiet(g->conn)) {
                *slot_out = i; // their old line went silent: most likely they restarted
            }
            return MP_REFUSE_DUPLICATE;
        }
    }
    if (free_slot < 0) {
        return MP_REFUSE_FULL;
    }
    if (mp_mem_room() < MP_HEAP_NEED) {
        pc_mp_log("[MP] no room for a visitor: %u KB free", mp_mem_room() / 1024);
        return MP_REFUSE_FULL;
    }
    *slot_out = free_slot;
    return 0;
}

static void mp_host_send_advert(const mp_addr_t* to) {
    unsigned char buf[64];
    mp_wr_t w = { buf, 0, sizeof(buf) };

    wr_u8(&w, MP_CTRL_ADVERT);
    wr_u8(&w, MP_PROTO_VERSION);
    wr_u32(&w, pc_mp_build_id());
    wr_u16(&w, MP_PORT_GAME + s_host.port_idx);
    wr_u32(&w, s_host.session_id);
    wr_u16(&w, Save_Get(land_info).id);
    wr_bytes(&w, Save_Get(land_info).name, LAND_NAME_SIZE);
    wr_bytes(&w, Now_Private->player_ID.player_name, PLAYER_NAME_LEN);
    wr_u8(&w, 1 + mp_ui_host_guest_count());
    wr_u8(&w, MP_MAX_PEERS);
    wr_u8(&w, 1);
    mp_link_send_ctrl(to, buf, w.len);
}

// -1 while the host decides (one caller at a time), then 1 to let them in or 0
static int mp_host_ask(const mp_addr_t* from, unsigned int nonce, const PersonalID_c* pid) {
    int yes;

    if (s_ask.denied_nonce == nonce && mp_link_addr_equal(&s_ask.denied_addr, from)) {
        return 0;
    }
    if (s_ask.active && (s_ask.nonce != nonce || !mp_link_addr_equal(&s_ask.addr, from))) {
        return -1;
    }
    if (!s_ask.active) {
        s_ask.active = TRUE;
        s_ask.addr = *from;
        s_ask.nonce = nonce;
        s_ask.start_ms = pc_mp_now_ms();
        s_ask.answer = -1;
        s_ask.serial = mp_notice_push(MP_MSG_N_REQUEST, pid->land_name, pid->player_name);
    }
    s_ask.last_ms = pc_mp_now_ms();
    if (s_ask.answer < 0) {
        return -1;
    }
    yes = s_ask.answer;
    s_ask.active = FALSE;
    if (!yes) {
        s_ask.denied_addr = *from;
        s_ask.denied_nonce = nonce;
    }
    return yes;
}

// time since a stamp; one taken after this tick read its clock counts as just now
static unsigned int mp_since(unsigned int now_ms, unsigned int stamp) {
    return (int)(now_ms - stamp) > 0 ? now_ms - stamp : 0;
}

static void mp_host_ask_tick(unsigned int now_ms) {
    int answer;

    if (!s_ask.active) {
        return;
    }
    if (mp_since(now_ms, s_ask.last_ms) >= MP_RING_MS) {
        // the caller gave up; so does the question
        s_ask.active = FALSE;
        mp_notice_cancel(s_ask.serial);
        return;
    }
    answer = mp_notice_take_answer(s_ask.serial);
    if (answer >= 0 && s_ask.answer < 0) {
        s_ask.answer = answer;
    } else if (s_ask.answer < 0 && mp_since(now_ms, s_ask.start_ms) >= MP_ASK_MS) {
        s_ask.answer = 0;
        mp_notice_cancel(s_ask.serial);
    }
}

static void mp_host_on_hello(const mp_addr_t* from, mp_rd_t* r) {
    unsigned char buf[64];
    mp_wr_t w = { buf, 0, sizeof(buf) };
    PersonalID_c pid;
    unsigned int build_id;
    unsigned int nonce;
    unsigned int mod_hash;
    int reason;
    int slot;

    rd_u8(r); // proto
    build_id = rd_u32(r);
    nonce = rd_u32(r);
    rd_pid(r, &pid);
    if (r->bad) {
        return;
    }
    // (older builds send no mod set; they still hear the version refusal)
    mod_hash = (r->pos + 4 <= r->len) ? rd_u32(r) : 0;
    mp_sanitize_name(pid.player_name, PLAYER_NAME_LEN);
    mp_sanitize_name(pid.land_name, LAND_NAME_SIZE);

    reason = mp_host_check(&pid, from, nonce, build_id, mod_hash, &slot);
    if (reason == MP_REFUSE_DUPLICATE && slot >= 0) {
        mp_host_drop_guest(slot, "came back");
        reason = mp_host_check(&pid, from, nonce, build_id, mod_hash, &slot);
    }
    // far-away callers are always asked about; nearby ones when the host's switch says so
    if (reason == 0 && (s_host.link == MP_LINK_ONLINE || g_pc_settings.mp_ask_join) && !s_host.guests[slot].used) {
        int verdict = mp_host_ask(from, nonce, &pid);

        if (verdict < 0) {
            wr_u8(&w, MP_CTRL_PENDING);
            wr_u32(&w, nonce);
            mp_link_send_ctrl(from, buf, w.len);
            return;
        }
        if (verdict == 0) {
            reason = MP_REFUSE_DENIED;
        }
    }
    if (reason != 0) {
        wr_u8(&w, MP_CTRL_REFUSE);
        wr_u32(&w, nonce);
        wr_u8(&w, reason);
        wr_u16(&w, Save_Get(land_info).id);
        wr_bytes(&w, Save_Get(land_info).name, LAND_NAME_SIZE);
        mp_link_send_ctrl(from, buf, w.len);
        return;
    }

    if (!s_host.guests[slot].used) {
        mp_guest_t* g = &s_host.guests[slot];
        unsigned int conn_id = pc_mp_random32() | 1;
        int conn = mp_link_open(from, conn_id);

        if (conn < 0) {
            pc_mp_log("[MP] no free line for %.8s", pid.player_name);
            return;
        }
        memset(g, 0, sizeof(*g));
        g->used = TRUE;
        g->conn = conn;
        g->conn_id = conn_id;
        g->nonce = nonce;
        g->addr = *from;
        g->pid = pid;
    }

    wr_u8(&w, MP_CTRL_WELCOME);
    wr_u32(&w, nonce);
    wr_u32(&w, s_host.guests[slot].conn_id);
    wr_u32(&w, s_host.session_id);
    wr_u8(&w, slot + 1);
    wr_u16(&w, Save_Get(land_info).id);
    wr_bytes(&w, Save_Get(land_info).name, LAND_NAME_SIZE);
    wr_bytes(&w, Now_Private->player_ID.player_name, PLAYER_NAME_LEN);
    wr_u8(&w, mp_ui_host_guest_count() - 1);
    mp_link_send_ctrl(from, buf, w.len);
}

static void mp_host_drop_guest(int slot, const char* why) {
    mp_guest_t* g = &s_host.guests[slot];

    if (!g->used) {
        return;
    }
    if (g->arrived && !g->leaving) {
        mp_notice_push(MP_MSG_N_GUEST_LOST, g->pid.land_name, g->pid.player_name);
    }
    if (g->arrived && g->leaving) {
        memcpy(s_host.thanks, g->pid.player_name, MP_NAME_LEN);
        s_host.thanks_due = TRUE;
    }
    mp_travel_on_guest_gone(g->conn);
    mp_player_on_gone(slot + 1);
    mp_world_on_guest_gone(slot + 1, g->conn);
    mp_npc_on_guest_gone(slot + 1);
    mp_cr_on_guest_gone(slot + 1);
    mp_link_close(g->conn);
    memset(g, 0, sizeof(*g));
}

static void mp_send_bye(const mp_addr_t* to, unsigned int conn_id) {
    unsigned char buf[8];
    mp_wr_t w = { buf, 0, sizeof(buf) };
    int i;

    wr_u8(&w, MP_CTRL_BYE);
    wr_u32(&w, conn_id);
    // unreliable, so say it a few times
    for (i = 0; i < 3; i++) {
        mp_link_send_ctrl(to, buf, w.len);
    }
}

// guest

static int mp_guest_send_hello(void) {
    unsigned char buf[64];
    mp_wr_t w = { buf, 0, sizeof(buf) };

    wr_u8(&w, MP_CTRL_HELLO);
    wr_u8(&w, MP_PROTO_VERSION);
    wr_u32(&w, pc_mp_build_id());
    wr_u32(&w, s_join.nonce);
    wr_pid(&w, &Now_Private->player_ID);
    wr_u32(&w, mp_mod_set_hash());
    return mp_link_send_ctrl(&s_join.addr, buf, w.len);
}

static void mp_guest_on_advert(const mp_addr_t* from, mp_rd_t* r) {
    mp_scan_entry_t e;
    int i;

    memset(&e, 0, sizeof(e));
    rd_u8(r); // proto
    rd_u32(r); // build id: a mismatch is refused politely when ringing
    e.addr = *from;
    e.addr.port = (unsigned short)rd_u16(r);
    e.session_id = rd_u32(r);
    e.land_id = (unsigned short)rd_u16(r);
    rd_bytes(r, e.town, MP_NAME_LEN);
    rd_bytes(r, e.host, MP_NAME_LEN);
    e.guests = (int)rd_u8(r) - 1;
    if (r->bad || s_scan.result != MP_UI_BUSY) {
        return;
    }
    mp_sanitize_name(e.town, MP_NAME_LEN);
    mp_sanitize_name(e.host, MP_NAME_LEN);

    for (i = 0; i < s_scan.count; i++) {
        if (s_scan.entries[i].session_id == e.session_id) {
            s_scan.entries[i] = e;
            return;
        }
    }
    if (s_scan.count < MP_SCAN_MAX) {
        s_scan.entries[s_scan.count++] = e;
        if (s_scan.found_ms == 0) {
            s_scan.found_ms = pc_mp_now_ms() | 1;
        }
    }
}

static void mp_guest_on_reply(const mp_addr_t* from, int type, mp_rd_t* r) {
    static const int refuse_result[] = {
        MP_UI_NO_ANSWER,        MP_UI_REFUSE_VERSION,   MP_UI_REFUSE_FULL,   MP_UI_REFUSE_SAME_TOWN,
        MP_UI_REFUSE_RESIDENT,  MP_UI_REFUSE_DUPLICATE, MP_UI_REFUSE_CLOSED, MP_UI_REFUSE_DENIED,
        MP_UI_REFUSE_MODS,
    };
    unsigned int nonce = rd_u32(r);

    if (!s_join.active || (s_join.result != MP_UI_BUSY && s_join.result != MP_UI_PENDING) || nonce != s_join.nonce ||
        !mp_link_addr_equal(from, &s_join.addr)) {
        return;
    }

    if (type == MP_CTRL_PENDING) {
        s_join.pending_ms = pc_mp_now_ms();
        s_join.result = MP_UI_PENDING;
        return;
    }
    if (type == MP_CTRL_REFUSE) {
        unsigned int reason = rd_u8(r);

        rd_u16(r);
        rd_bytes(r, s_join.info.town, MP_NAME_LEN);
        mp_sanitize_name(s_join.info.town, MP_NAME_LEN);
        s_join.result = (reason < sizeof(refuse_result) / sizeof(refuse_result[0])) ? refuse_result[reason]
                                                                                     : MP_UI_REFUSE_DENIED;
        return;
    }

    {
        unsigned int conn_id = rd_u32(r);

        rd_u32(r); // session id
        s_join.slot = (int)rd_u8(r);
        s_join.info.land_id = (unsigned short)rd_u16(r);
        rd_bytes(r, s_join.info.town, MP_NAME_LEN);
        rd_bytes(r, s_join.info.host, MP_NAME_LEN);
        s_join.info.guests = (int)rd_u8(r);
        if (r->bad || s_join.slot < 1 || s_join.slot >= MP_MAX_PEERS) {
            return;
        }
        mp_sanitize_name(s_join.info.town, MP_NAME_LEN);
        mp_sanitize_name(s_join.info.host, MP_NAME_LEN);
        s_join.conn_id = conn_id;
        s_join.conn = mp_link_open(from, conn_id);
        s_join.result = (s_join.conn >= 0) ? MP_UI_DONE : MP_UI_NO_ANSWER;
    }
}

// events

// a quiet peer is talking again, maybe from a new address: what repeats goes out now
static void mp_on_back(int conn) {
    int i;

    for (i = 0; i < MP_MAX_PEERS - 1; i++) {
        mp_guest_t* g = &s_host.guests[i];

        if (g->used && g->conn == conn && mp_link_addr(conn) != NULL) {
            g->addr = *mp_link_addr(conn);
            if (g->arrived && !g->leaving) {
                mp_player_on_back(conn);
                mp_npc_on_arrived(conn);
                mp_event_on_arrived(conn);
                mp_world_on_back(conn);
            }
        }
    }
    if (s_join.active && s_join.conn == conn) {
        mp_player_on_back(conn);
        mp_travel_on_host_back();
    }
}

static void mp_on_event(const mp_event_t* ev) {
    mp_rd_t r;
    int type;
    int i;

    switch (ev->type) {
        case MP_EV_CTRL:
            break;
        case MP_EV_REL:
            if (ev->len > 0 && ev->data[0] >= MP_M_LOOK && ev->data[0] < MP_M_LOOK + 0x10) {
                mp_player_on_rel(ev->conn, ev->data, ev->len);
            } else if (ev->len > 0 && ev->data[0] >= MP_M_OP && ev->data[0] < MP_M_OP + 0x10) {
                mp_world_on_rel(ev->conn, ev->data, ev->len);
            } else if (ev->len > 0 && ev->data[0] >= MP_M_TALK_LOCK && ev->data[0] < MP_M_TALK_LOCK + 0x10) {
                mp_npc_on_rel(ev->conn, ev->data, ev->len);
            } else if (ev->len > 0 && ev->data[0] >= MP_M_EVSTATE && ev->data[0] < MP_M_EVSTATE + 0x10) {
                mp_event_on_rel(ev->conn, ev->data, ev->len);
            } else if (ev->len > 0 && ev->data[0] >= MP_M_CR_WADE && ev->data[0] < MP_M_CR_WADE + 0x10) {
                mp_cr_on_rel(ev->conn, ev->data, ev->len);
            } else if (ev->len > 0 && ev->data[0] >= MP_M_MOD && ev->data[0] < MP_M_MOD + 0x10) {
                mp_mod_on_rel(ev->conn, ev->data, ev->len);
            } else {
                mp_travel_on_rel(ev->conn, ev->data, ev->len);
            }
            return;
        case MP_EV_STATE:
            if (ev->len > 0 && ev->data[0] == MP_S_NPCS) {
                mp_npc_on_state(ev->conn, ev->data, ev->len);
            } else if (ev->len > 0 && ev->data[0] == MP_S_CRITTERS) {
                mp_cr_on_state(ev->conn, ev->data, ev->len);
            } else if (ev->len > 0 && ev->data[0] == MP_S_EVAREA) {
                mp_npc_on_evarea(ev->conn, ev->data, ev->len);
            } else if (ev->len > 0 && ev->data[0] == MP_S_MOD) {
                mp_mod_on_state(ev->conn, ev->data, ev->len);
            } else if (ev->len > 0 && ev->data[0] == MP_S_PORTER) {
                mp_porter_on_state(ev->conn, ev->data, ev->len);
            } else {
                mp_player_on_state(ev->conn, ev->data, ev->len);
            }
            return;
        case MP_EV_BULK:
            mp_travel_on_bulk(ev);
            return;
        case MP_EV_BULK_SENT:
            mp_travel_on_bulk_sent(ev->conn);
            return;
        case MP_EV_DEAD:
            pc_mp_log("[MP] line %d timed out", ev->conn);
            for (i = 0; i < MP_MAX_PEERS - 1; i++) {
                if (s_host.guests[i].used && s_host.guests[i].conn == ev->conn) {
                    mp_host_drop_guest(i, "timed out");
                }
            }
            if (s_join.active && s_join.conn == ev->conn) {
                s_join.conn = -1;
                mp_travel_on_host_gone();
            }
            return;
        case MP_EV_LOST:
            return;
        case MP_EV_AWAY:
            if (s_join.active && s_join.conn == ev->conn) {
                mp_travel_on_host_away();
            }
            return;
        case MP_EV_BACK:
            mp_on_back(ev->conn);
            return;
        default:
            return;
    }

    r.p = ev->data;
    r.len = ev->len;
    r.pos = 0;
    r.bad = FALSE;
    type = (int)rd_u8(&r);

    switch (type) {
        case MP_CTRL_QUERY:
            if (s_net == MP_NET_HOST && s_host.hosting && !s_host.closing && mp_ctrl_allowed(&ev->from)) {
                mp_host_send_advert(&ev->from);
            }
            break;
        case MP_CTRL_HELLO:
            if (s_net == MP_NET_HOST && mp_ctrl_allowed(&ev->from)) {
                mp_host_on_hello(&ev->from, &r);
            }
            break;
        case MP_CTRL_ADVERT:
            if (s_net == MP_NET_GUEST) {
                mp_guest_on_advert(&ev->from, &r);
            }
            break;
        case MP_CTRL_WELCOME:
        case MP_CTRL_REFUSE:
        case MP_CTRL_PENDING:
            if (s_net == MP_NET_GUEST) {
                mp_guest_on_reply(&ev->from, type, &r);
            }
            break;
        case MP_CTRL_BYE: {
            unsigned int conn_id = rd_u32(&r);

            for (i = 0; i < MP_MAX_PEERS - 1; i++) {
                mp_guest_t* g = &s_host.guests[i];

                if (g->used && g->conn_id == conn_id && mp_link_addr_equal(&g->addr, &ev->from)) {
                    mp_host_drop_guest(i, "bye");
                }
            }
            if (s_join.active && mp_link_is_open(s_join.conn) && s_join.conn_id == conn_id &&
                mp_link_addr_equal(mp_link_addr(s_join.conn), &ev->from)) {
                mp_link_close(s_join.conn);
                s_join.conn = -1;
                mp_travel_on_host_gone();
            }
            break;
        }
    }
}

// Porter calls

int mp_ui_available(void) {
    return TRUE;
}

// the listening window starts once the radio is up
static void mp_scan_open_step(void) {
    int res = mp_net_open(MP_NET_GUEST, s_scan.link);

    if (res == MP_UI_BUSY) {
        s_scan.fresh = s_scan.link == MP_LINK_ADHOC;
        return;
    }
    s_scan.opening = FALSE;
    s_scan.start_ms = pc_mp_now_ms();
    if (res != MP_UI_DONE) {
        s_scan.result = res;
    }
}

void mp_ui_scan_start(mp_link_t link) {
    memset(&s_scan, 0, sizeof(s_scan));
    s_scan.link = link;
    s_scan.nonce = (unsigned short)pc_mp_random32();
    s_scan.result = MP_UI_BUSY;
    s_scan.opening = TRUE;
    mp_scan_open_step();
}

int mp_ui_scan_poll(void) {
    return s_scan.result;
}

// the radio first, then the first hello; the answer timer runs from that hello
static void mp_join_open_step(void) {
    int res;

    if (!s_join.active) {
        s_join.opening = FALSE;
        return;
    }
    res = mp_net_open(MP_NET_GUEST, s_join.link);
    if (res == MP_UI_BUSY) {
        s_join.opening = TRUE;
        return;
    }
    s_join.opening = FALSE;
    if (res != MP_UI_DONE) {
        s_join.result = res;
        return;
    }
    s_join.start_ms = pc_mp_now_ms();
    if (!mp_guest_send_hello()) {
        pc_mp_log("[MP] the first hello didn't go out");
    }
    s_join.last_hello_ms = s_join.start_ms;
    if (s_join.target < 0) {
        if (s_join.addr.ip == vita_mp_local_ip_u32()) {
            s_join.result = MP_UI_NO_ANSWER_HOME; // their own ticket
        } else {
            vita_mp_nat_lookup();
        }
    }
}

static void mp_join_begin(int target, const mp_addr_t* addr) {
    if (s_join.active && mp_link_is_open(s_join.conn)) {
        mp_send_bye(mp_link_addr(s_join.conn), s_join.conn_id);
        mp_link_close(s_join.conn);
    }
    memset(&s_join, 0, sizeof(s_join));
    s_join.active = TRUE;
    s_join.target = target;
    s_join.link = target < 0 ? MP_LINK_ONLINE : s_scan.link;
    s_join.addr = *addr;
    s_join.conn = -1;
    s_join.nonce = pc_mp_random32();
    s_join.start_ms = pc_mp_now_ms();
    s_join.deadline_ms = (s_scan.link == MP_LINK_ONLINE || target < 0) ? MP_HELLO_WAN_MS : MP_HELLO_LAN_MS;
    s_join.result = MP_UI_BUSY;
    mp_join_open_step();
}

void mp_ui_join_scan(int i) {
    if (i < 0 || i >= s_scan.count) {
        return;
    }
    mp_join_begin(i, &s_scan.entries[i].addr);
    s_join.info = s_scan.entries[i];
}

int mp_ui_join_ticket(const unsigned char* text, int len) {
    mp_addr_t addr;
    unsigned int ip;
    int port_idx;

    if (!mp_ticket_decode(text, len, &ip, &port_idx)) {
        return FALSE;
    }
    memset(&addr, 0, sizeof(addr));
    addr.kind = MP_ADDR_IP;
    addr.ip = ip;
    addr.port = (unsigned short)(MP_PORT_GAME + port_idx);
    mp_join_begin(-1, &addr);
    return TRUE;
}

int mp_ui_join_poll(void) {
    return s_join.result;
}

void mp_ui_join_cancel(void) {
    if (!s_join.active) {
        return;
    }
    if (s_join.conn >= 0 && mp_link_is_open(s_join.conn)) {
        mp_send_bye(mp_link_addr(s_join.conn), s_join.conn_id);
        mp_link_close(s_join.conn);
    }
    s_join.active = FALSE;
    s_join.conn = -1;
    if (s_net == MP_NET_GUEST) {
        mp_net_close();
    }
}

static void mp_host_open_step(void) {
    unsigned int ip = 0;
    int res;

    if (!s_host.mapping) {
        res = mp_net_open(MP_NET_HOST, s_host.link);
        if (res == MP_UI_BUSY) {
            return;
        }
        if (res != MP_UI_DONE) {
            s_host.opening = FALSE;
            s_host.result = res;
            return;
        }
        if (s_host.link == MP_LINK_ONLINE) {
            if (!vita_mp_nat_idle()) {
                return; // the last line's worker is still giving its port back
            }
            // (a ticket has room for four ports)
            vita_mp_nat_open((unsigned short)(MP_PORT_GAME + s_host.port_idx), MP_PORT_GAME, 4);
            s_host.mapping = TRUE;
            return;
        }
    } else {
        res = vita_mp_nat_poll(&ip);
        if (res == MP_UI_BUSY) {
            return;
        }
        // the router forwards another of the game's ports: the line moves there before any ticket names the old one
        if (res == MP_UI_DONE && vita_mp_nat_port() != MP_PORT_GAME + s_host.port_idx) {
            if (vita_mp_udp_move_game(vita_mp_nat_port())) {
                s_host.port_idx = vita_mp_nat_port() - MP_PORT_GAME;
            } else {
                vita_mp_nat_close();
                res = MP_UI_NOPORTMAP;
            }
        }
    }
    // no outside address known: a same-Wi-Fi ticket still helps
    if (ip == 0) {
        ip = vita_mp_local_ip_u32();
    }
    s_host.opening = FALSE;
    mp_ticket_encode(ip, s_host.port_idx, s_host.ticket);
    s_host.session_id = pc_mp_random32();
    s_host.hosting = TRUE;
    mp_set_role(MP_ROLE_HOST);
    s_host.result = res;
}

void mp_ui_host_start(mp_link_t link) {
    memset(&s_host, 0, sizeof(s_host));
    s_host.link = link;
    s_host.start_ms = pc_mp_now_ms();
    s_host.result = MP_UI_BUSY;
    s_host.opening = TRUE;
    mp_host_open_step();
}

void mp_ui_done(void) {
    if (s_net == MP_NET_GUEST && !s_join.active && mp_travel_state() == MP_TRAVEL_NONE) {
        mp_net_close();
    }
}

int mp_ui_host_poll(void) {
    return s_host.result;
}

static void mp_host_finish(void) {
    int i;

    // what the town still owes its visitors (commits above all) goes out, and is heard, before the goodbye
    mp_world_flush_out();
    for (i = 0; i < 30 && mp_link_running(); i++) {
        int k;
        int idle = TRUE;

        mp_link_tick(pc_mp_now_ms());
        for (k = 0; k < MP_MAX_PEERS - 1; k++) {
            if (s_host.guests[k].used && !mp_link_rel_idle(s_host.guests[k].conn)) {
                idle = FALSE;
            }
        }
        if (idle) {
            break;
        }
        vita_mp_sleep_ms(10);
    }
    for (i = 0; i < MP_MAX_PEERS - 1; i++) {
        if (s_host.guests[i].used) {
            mp_send_bye(&s_host.guests[i].addr, s_host.guests[i].conn_id);
            mp_host_drop_guest(i, "line closed");
        }
    }
    s_host.hosting = FALSE;
    s_host.closing = FALSE;
    if (s_host.mapping) {
        vita_mp_nat_close();
        s_host.mapping = FALSE;
    }
    if (s_ask.active) {
        mp_notice_cancel(s_ask.serial);
    }
    memset(&s_ask, 0, sizeof(s_ask));
    mp_world_end();
    mp_set_role(MP_ROLE_NONE);
    if (s_net == MP_NET_HOST) {
        mp_net_close();
    }
}

void mp_lobby_host_hangup(void) {
    if (s_host.hosting) {
        mp_host_finish();
    }
}

// visitors get a last train; the line stays up until they're gone or the grace runs out
void mp_ui_host_close(void) {
    if (!s_host.hosting) {
        return;
    }
    if (mp_ui_host_guest_count() == 0) {
        mp_host_finish();
        return;
    }
    if (!s_host.closing) {
        int i;

        s_host.closing = TRUE;
        s_host.closing_ms = pc_mp_now_ms();
        if (s_ask.active) {
            mp_notice_cancel(s_ask.serial);
            s_ask.active = FALSE;
        }
        // callers still at Porter or loading the town are turned back; travellers get the last train
        for (i = 0; i < MP_MAX_PEERS - 1; i++) {
            mp_guest_t* g = &s_host.guests[i];

            if (g->used && !g->arrived && !g->boarding) {
                mp_send_bye(&g->addr, g->conn_id);
                mp_host_drop_guest(i, "line closing");
            }
        }
        mp_travel_host_close();
    }
}

// module plumbing

int mp_lobby_host_conn(void) {
    return (s_join.active && s_join.result == MP_UI_DONE && mp_link_is_open(s_join.conn)) ? s_join.conn : -1;
}

void mp_lobby_guest_end(void) {
    if (s_join.active && mp_link_is_open(s_join.conn)) {
        mp_send_bye(mp_link_addr(s_join.conn), s_join.conn_id);
        mp_link_close(s_join.conn);
    }
    s_join.active = FALSE;
    s_join.conn = -1;
    if (s_net == MP_NET_GUEST) {
        mp_net_close();
    }
}

int mp_lobby_guest_slot(int conn) {
    int i;

    for (i = 0; i < MP_MAX_PEERS - 1; i++) {
        if (s_host.guests[i].used && s_host.guests[i].conn == conn) {
            return i;
        }
    }
    return -1;
}

void mp_lobby_shutdown(void) {
    int i;

    for (i = 0; i < MP_MAX_PEERS - 1; i++) {
        if (s_host.guests[i].used) {
            mp_send_bye(&s_host.guests[i].addr, s_host.guests[i].conn_id);
        }
    }
    if (s_join.active && mp_link_is_open(s_join.conn)) {
        for (i = 0; i < 30 && mp_link_running() && !mp_link_rel_idle(s_join.conn); i++) {
            mp_link_tick(pc_mp_now_ms());
            vita_mp_sleep_ms(10);
        }
        // (the line may have ended while it waited)
        if (s_join.active && mp_link_is_open(s_join.conn)) {
            mp_send_bye(mp_link_addr(s_join.conn), s_join.conn_id);
        }
    }
    if (mp_link_running()) {
        mp_link_tick(pc_mp_now_ms()); // let the goodbyes out
    }
    // an ad hoc radio left up would stay in ad hoc after the game quits
    if (s_net != MP_NET_OFF) {
        mp_net_close();
    }
    // a line closed a moment ago may still be giving its port back
    if (s_host.mapping || !vita_mp_nat_idle()) {
        vita_mp_nat_close_wait(1500);
        s_host.mapping = FALSE;
    }
}

// back from sleep (certain when the system said so): UDP sockets are reopened, and an ad hoc
// network ends with a real sleep
void mp_lobby_on_resume(int certain) {
    if (s_net == MP_NET_OFF) {
        return;
    }
    if (!s_net_adhoc) {
        vita_mp_udp_reopen();
        return;
    }
    if (!certain) {
        return;
    }
    // whatever Porter is waiting on gets its answer now
    if (s_scan.result == MP_UI_BUSY) {
        s_scan.opening = FALSE;
        s_scan.result = MP_UI_DONE;
    }
    if (s_join.active && (s_join.result == MP_UI_BUSY || s_join.result == MP_UI_PENDING)) {
        s_join.result = MP_UI_NO_ANSWER;
    }
    if (s_host.opening) {
        s_host.opening = FALSE;
        s_host.result = MP_UI_ADHOC_CANCEL;
    }
    if (s_net == MP_NET_HOST && s_host.hosting) {
        mp_host_finish();
        return;
    }
    if (s_join.active && s_join.conn >= 0) {
        s_join.conn = -1;
        mp_travel_on_host_gone();
    }
    mp_net_close();
}

int mp_lobby_send_rel(int conn, const void* msg, int len) {
    return mp_link_is_open(conn) && mp_link_send_rel(conn, msg, len);
}

void mp_lobby_send_state_to(int conn, const void* msg, int len) {
    if (mp_link_is_open(conn)) {
        mp_link_send_state(conn, msg, len);
    }
}

int mp_lobby_guest_conn(int slot) {
    if (slot < 1 || slot >= MP_MAX_PEERS || !s_host.guests[slot - 1].used) {
        return -1;
    }
    return mp_link_is_open(s_host.guests[slot - 1].conn) ? s_host.guests[slot - 1].conn : -1;
}

int mp_lobby_self_slot(void) {
    return (s_net == MP_NET_GUEST && s_join.active) ? s_join.slot : 0;
}

void mp_lobby_guest_mark(int conn, int arrived, int leaving) {
    int slot = mp_lobby_guest_slot(conn);

    if (slot >= 0) {
        s_host.guests[slot].arrived |= arrived;
        s_host.guests[slot].leaving |= leaving;
        if (arrived) {
            mp_link_hold(conn, MP_HOLD_MS);
        }
    }
}

void mp_lobby_guest_boarding(int conn) {
    int slot = mp_lobby_guest_slot(conn);

    if (slot >= 0) {
        s_host.guests[slot].boarding = TRUE;
    }
}

void mp_lobby_hold_line(void) {
    if (s_join.active) {
        mp_link_hold(s_join.conn, MP_HOLD_MS);
    }
}

int mp_lobby_conn_quiet(int conn) {
    return mp_link_quiet(conn);
}

int mp_lobby_rel_room(int conn) {
    return mp_link_rel_room(conn);
}

int mp_lobby_rel_fits(int conn, int bytes, int count) {
    return mp_link_rel_fits(conn, bytes, count);
}

void mp_lobby_pump(void) {
    if (mp_link_running()) {
        mp_link_tick(pc_mp_now_ms());
        vita_mp_sleep_ms(10);
    }
}

unsigned int mp_lobby_conn_silence(int conn) {
    return mp_link_silence(conn);
}

int mp_lobby_guest_arrived(int slot) {
    return slot >= 1 && slot < MP_MAX_PEERS && s_host.guests[slot - 1].used && s_host.guests[slot - 1].arrived &&
           !s_host.guests[slot - 1].leaving;
}

int mp_lobby_guest_names(int conn, unsigned char* name, unsigned char* town) {
    int slot = mp_lobby_guest_slot(conn);

    if (slot < 0) {
        return FALSE;
    }
    memcpy(name, s_host.guests[slot].pid.player_name, MP_NAME_LEN);
    memcpy(town, s_host.guests[slot].pid.land_name, MP_NAME_LEN);
    return TRUE;
}

int mp_lobby_guest_pid(int conn, void* pid) {
    int slot = mp_lobby_guest_slot(conn);

    if (slot < 0) {
        return FALSE;
    }
    *(PersonalID_c*)pid = s_host.guests[slot].pid;
    return TRUE;
}

void mp_lobby_send_rel_all(const void* msg, int len) {
    int i;

    for (i = 0; i < MP_MAX_PEERS - 1; i++) {
        if (s_host.guests[i].used) {
            mp_lobby_send_rel(s_host.guests[i].conn, msg, len);
        }
    }
}

// timers

#define MP_CLOSING_WAIT_MS 75000 // the 60 s last train plus the ride out

void mp_lobby_tick(unsigned int now_ms) {
    if (s_net != MP_NET_OFF && !s_net_adhoc) {
        vita_mp_udp_tick(now_ms);
    }
    if (s_host.opening) {
        mp_host_open_step();
    }
    mp_host_ask_tick(now_ms);
    if (s_scan.opening) {
        mp_scan_open_step();
    }
    if (s_join.opening) {
        mp_join_open_step();
    }
    if (s_net == MP_NET_HOST && s_host.hosting) {
        mp_travel_host_tick(now_ms);
        if (s_host.closing &&
            (mp_ui_host_guest_count() == 0 || mp_since(now_ms, s_host.closing_ms) >= MP_CLOSING_WAIT_MS)) {
            mp_host_finish();
        }
    }

    if (s_net == MP_NET_GUEST && s_scan.result == MP_UI_BUSY && !s_scan.opening) {
        if (s_scan.fresh ? (mp_since(now_ms, s_scan.start_ms) >= MP_SCAN_NEW_MS ||
                            (s_scan.found_ms != 0 && mp_since(now_ms, s_scan.found_ms) >= 1500))
                         : mp_since(now_ms, s_scan.start_ms) >= MP_SCAN_MS) {
            s_scan.result = MP_UI_DONE;
        } else if (s_scan.last_query_ms == 0 || mp_since(now_ms, s_scan.last_query_ms) >= MP_QUERY_MS) {
            unsigned char buf[16];
            mp_wr_t w = { buf, 0, sizeof(buf) };

            wr_u8(&w, MP_CTRL_QUERY);
            wr_u8(&w, MP_PROTO_VERSION);
            wr_u32(&w, pc_mp_build_id());
            wr_u16(&w, s_scan.nonce);
            mp_link_bcast_ctrl(MP_PORT_DISC, buf, w.len);
            s_scan.last_query_ms = now_ms;
        }
    }

    if (s_net == MP_NET_GUEST && s_join.active && (s_join.result == MP_UI_BUSY || s_join.result == MP_UI_PENDING)) {
        unsigned int outside = 0;

        if ((s_join.result == MP_UI_BUSY && mp_since(now_ms, s_join.start_ms) >= s_join.deadline_ms) ||
            (s_join.result == MP_UI_PENDING && mp_since(now_ms, s_join.pending_ms) >= MP_RING_MS)) {
            // a ticket for this same house needs "On my Wi-Fi." instead
            s_join.result = (s_join.target < 0 && vita_mp_nat_poll(&outside) != MP_UI_BUSY && outside != 0 &&
                             outside == s_join.addr.ip)
                                ? MP_UI_NO_ANSWER_HOME
                                : MP_UI_NO_ANSWER;
        } else if (mp_since(now_ms, s_join.last_hello_ms) >= MP_HELLO_MS) {
            mp_guest_send_hello();
            s_join.last_hello_ms = now_ms;
        }
    }
}


#endif
