// pc_mp_link.c
// multiplayer wire protocol (see pc_mp_link.h); unacked messages ride in fresh packets
#include "pc_mp_link.h"

#include <stdlib.h>
#include <string.h>

#define MP_LINK_PROTO 1

#define MP_FLAG_CTRL 0x01
#define MP_FLAG_ACKS 0x02

#define MP_STREAM_STATE 0
#define MP_STREAM_REL   1
#define MP_STREAM_BULK  2

#define MP_REL_WINDOW    32
#define MP_SENT_RING     64
#define MP_SENT_ITEMS    16
#define MP_KEEPALIVE_MS  250
#define MP_RTO_MAX_MS    1000
#define MP_BULK_WINDOW   32
#define MP_BULK_CHUNKS   (MP_BULK_MAX / MP_BULK_CHUNK)
#define MP_BULK_BURST    (4 * MP_BULK_CHUNK) // fits an 8 KB ad hoc receive buffer
#define MP_BULK_RATE_KBS 128
#define MP_TICK_PKTS_IN  256
#define MP_TICK_PKTS_OUT 8

typedef struct {
    uint8_t stream;
    uint16_t id;
} mp_sent_item_t;

typedef struct {
    uint8_t valid;
    uint8_t n_items;
    uint16_t seq;
    uint32_t sent_ms;
    mp_sent_item_t items[MP_SENT_ITEMS];
} mp_sent_pkt_t;

typedef struct {
    uint8_t used;
    uint16_t id;
    uint16_t len;
    uint32_t last_sent_ms; // 0 = not sent yet
    uint8_t data[MP_REL_MAX];
} mp_rel_out_t;

typedef struct {
    uint8_t present;
    uint16_t len;
    uint8_t data[MP_REL_MAX];
} mp_rel_in_t;

typedef struct {
    int open;
    mp_addr_t addr;
    uint32_t conn_id;

    uint16_t next_seq;
    uint16_t recv_top;
    uint32_t recv_bits; // bit i: packet recv_top - 1 - i arrived
    int recv_any;
    int ack_pending;
    mp_sent_pkt_t sent[MP_SENT_RING];

    uint32_t last_recv_ms;
    uint32_t last_send_ms;
    int lost;
    int away;
    int hold_ms; // 0: timing.dead_ms
    float srtt;
    float rttvar;
    int rtt_valid;

    uint16_t rel_next_id;
    uint16_t rel_base;
    mp_rel_out_t rel_out[MP_REL_WINDOW];
    uint16_t rel_expect;
    mp_rel_in_t rel_in[MP_REL_WINDOW];

    int bl_head;
    int bl_used;
    int bl_lost; // the backlog overflowed: the peer needs a fresh picture
    uint8_t backlog[MP_REL_BACKLOG];

    int state_pending[MP_STATE_CHANS];
    uint16_t state_len[MP_STATE_CHANS];
    uint8_t state_data[MP_STATE_CHANS][MP_STATE_MAX];
    uint16_t state_in_seq[MP_STATE_CHANS];
    int state_in_any[MP_STATE_CHANS];

    const uint8_t* bulk_src;
    int bulk_len;
    int bulk_count;
    int bulk_acked_n;
    uint8_t bulk_xfer;
    uint8_t bulk_acked[MP_BULK_CHUNKS / 8];
    uint32_t bulk_sent_ms[MP_BULK_CHUNKS];
    float bulk_tokens;
    uint8_t bulk_rx_xfer;
    uint8_t bulk_rx_any;
    uint8_t bulk_seen[MP_BULK_CHUNKS / 8];
} mp_conn_t;

static struct {
    int running;
    int in_tick;
    int stop_requested;
    mp_transport_t tr;
    mp_link_timing_t timing;
    uint8_t self_slot;
    mp_event_cb_t cb;
    uint32_t now_ms;
    uint32_t last_tick_ms;
    mp_conn_t* conns; // MP_LINK_CONNS, heap while running
} s_link;

static int mp_tr_send(const mp_addr_t* to, const void* pkt, int len) {
    return s_link.tr.send(to, pkt, len);
}

// byte order

static void wr16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t rd16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// helpers

int mp_link_addr_equal(const mp_addr_t* a, const mp_addr_t* b) {
    if (a->kind != b->kind || a->port != b->port) {
        return 0;
    }
    return a->kind == MP_ADDR_MAC ? memcmp(a->mac, b->mac, 6) == 0 : a->ip == b->ip;
}

static void mp_emit_bulk(int type, int conn, const mp_addr_t* from, const uint8_t* data, int len, int idx, int count,
                         int xfer) {
    mp_event_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.conn = conn;
    if (from != NULL) {
        ev.from = *from;
    }
    ev.data = data;
    ev.len = len;
    ev.bulk_idx = idx;
    ev.bulk_count = count;
    ev.bulk_xfer = xfer;
    if (s_link.cb != NULL) {
        s_link.cb(&ev);
    }
}

static void mp_emit(int type, int conn, const mp_addr_t* from, const uint8_t* data, int len) {
    mp_emit_bulk(type, conn, from, data, len, 0, 0, 0);
}

static int mp_rto_ms(const mp_conn_t* c) {
    float rto = c->rtt_valid ? c->srtt + 4.0f * c->rttvar + 33.0f : 300.0f;

    if (rto < (float)s_link.timing.rto_min_ms) {
        rto = (float)s_link.timing.rto_min_ms;
    }
    if (rto > (float)MP_RTO_MAX_MS) {
        rto = (float)MP_RTO_MAX_MS;
    }
    return (int)rto;
}

static int mp_bit(const uint8_t* bits, int i) {
    return (bits[i >> 3] >> (i & 7)) & 1;
}

static void mp_bit_set(uint8_t* bits, int i) {
    bits[i >> 3] |= (uint8_t)(1 << (i & 7));
}

// lifecycle

int mp_link_start(const mp_transport_t* tr, const mp_link_timing_t* timing, uint8_t self_slot, mp_event_cb_t cb) {
    mp_link_stop();
    s_link.conns = (mp_conn_t*)mp_calloc(MP_LINK_CONNS, sizeof(mp_conn_t));
    if (s_link.conns == NULL) {
        return 0;
    }
    s_link.tr = *tr;
    s_link.timing = *timing;
    s_link.self_slot = self_slot;
    s_link.cb = cb;
    s_link.running = 1;
    return 1;
}

// from inside an event callback the stop waits for the tick to unwind
void mp_link_stop(void) {
    if (s_link.in_tick) {
        s_link.stop_requested = 1;
        return;
    }
    mp_free(s_link.conns);
    memset(&s_link, 0, sizeof(s_link));
}

int mp_link_running(void) {
    return s_link.running && !s_link.stop_requested;
}

int mp_link_open(const mp_addr_t* to, uint32_t conn_id) {
    int i;

    if (!s_link.running) {
        return -1;
    }
    for (i = 0; i < MP_LINK_CONNS; i++) {
        mp_conn_t* c = &s_link.conns[i];

        if (!c->open) {
            memset(c, 0, sizeof(*c));
            c->open = 1;
            c->addr = *to;
            c->conn_id = conn_id;
            c->last_recv_ms = s_link.now_ms;
            c->last_send_ms = s_link.now_ms;
            c->ack_pending = 1; // say hello right away
            return i;
        }
    }
    return -1;
}

void mp_link_close(int conn) {
    if (s_link.running && conn >= 0 && conn < MP_LINK_CONNS) {
        memset(&s_link.conns[conn], 0, sizeof(mp_conn_t));
    }
}

int mp_link_is_open(int conn) {
    return mp_link_running() && conn >= 0 && conn < MP_LINK_CONNS && s_link.conns[conn].open;
}

const mp_addr_t* mp_link_addr(int conn) {
    return mp_link_is_open(conn) ? &s_link.conns[conn].addr : NULL;
}

int mp_link_rtt_ms(int conn) {
    return mp_link_is_open(conn) && s_link.conns[conn].rtt_valid ? (int)s_link.conns[conn].srtt : -1;
}

// sending

int mp_link_send_ctrl(const mp_addr_t* to, const void* msg, int len) {
    uint8_t pkt[MP_PKT_MAX];

    if (!s_link.running || len < 0 || len > MP_CTRL_MAX) {
        return 0;
    }
    memset(pkt, 0, MP_PKT_HDR);
    pkt[0] = MP_PROTO_MAGIC;
    pkt[1] = MP_FLAG_CTRL;
    pkt[2] = s_link.self_slot;
    pkt[3] = MP_LINK_PROTO;
    memcpy(pkt + MP_PKT_HDR, msg, len);
    return mp_tr_send(to, pkt, MP_PKT_HDR + len) > 0;
}

int mp_link_bcast_ctrl(uint16_t port, const void* msg, int len) {
    uint8_t pkt[MP_PKT_MAX];

    if (!s_link.running || len < 0 || len > MP_CTRL_MAX) {
        return 0;
    }
    memset(pkt, 0, MP_PKT_HDR);
    pkt[0] = MP_PROTO_MAGIC;
    pkt[1] = MP_FLAG_CTRL;
    pkt[2] = s_link.self_slot;
    pkt[3] = MP_LINK_PROTO;
    memcpy(pkt + MP_PKT_HDR, msg, len);
    return s_link.tr.broadcast(port, pkt, MP_PKT_HDR + len) > 0;
}

// reliable backlog
// messages past the window wait here in order, so nothing reliable is dropped short of this filling

static int mp_rel_window_free(const mp_conn_t* c) {
    return MP_REL_WINDOW - (uint16_t)(c->rel_next_id - c->rel_base);
}

static void mp_rel_enqueue(mp_conn_t* c, const uint8_t* msg, int len) {
    mp_rel_out_t* slot = &c->rel_out[c->rel_next_id % MP_REL_WINDOW];

    slot->used = 1;
    slot->id = c->rel_next_id;
    slot->len = (uint16_t)len;
    slot->last_sent_ms = 0;
    memcpy(slot->data, msg, len);
    c->rel_next_id++;
}

static int mp_bl_push(mp_conn_t* c, const uint8_t* msg, int len) {
    int at;
    int i;

    if (c->bl_used + 2 + len > MP_REL_BACKLOG) {
        return 0;
    }
    at = (c->bl_head + c->bl_used) % MP_REL_BACKLOG;
    c->backlog[at] = (uint8_t)len;
    c->backlog[(at + 1) % MP_REL_BACKLOG] = (uint8_t)(len >> 8);
    for (i = 0; i < len; i++) {
        c->backlog[(at + 2 + i) % MP_REL_BACKLOG] = msg[i];
    }
    c->bl_used += 2 + len;
    return 1;
}

static void mp_bl_drain(mp_conn_t* c) {
    uint8_t msg[MP_REL_MAX];

    while (c->bl_used > 0 && mp_rel_window_free(c) > 0) {
        int len = c->backlog[c->bl_head] | (c->backlog[(c->bl_head + 1) % MP_REL_BACKLOG] << 8);
        int i;

        for (i = 0; i < len; i++) {
            msg[i] = c->backlog[(c->bl_head + 2 + i) % MP_REL_BACKLOG];
        }
        c->bl_head = (c->bl_head + 2 + len) % MP_REL_BACKLOG;
        c->bl_used -= 2 + len;
        mp_rel_enqueue(c, msg, len);
    }
}

int mp_link_send_rel(int conn, const void* msg, int len) {
    mp_conn_t* c;

    if (!mp_link_is_open(conn) || len <= 0 || len > MP_REL_MAX) {
        return 0;
    }
    c = &s_link.conns[conn];
    if (c->bl_used == 0 && mp_rel_window_free(c) > 0) {
        mp_rel_enqueue(c, (const uint8_t*)msg, len);
        return 1;
    }
    if (!mp_bl_push(c, (const uint8_t*)msg, len)) {
        c->bl_lost = 1;
        return 0;
    }
    return 1;
}

// room for these reliable messages without any of them dropped
int mp_link_rel_fits(int conn, int bytes, int count) {
    return mp_link_is_open(conn) &&
           s_link.conns[conn].bl_used + bytes + 2 * count + 2048 <= MP_REL_BACKLOG;
}

int mp_link_rel_room(int conn) {
    if (!mp_link_is_open(conn) || s_link.conns[conn].bl_used != 0) {
        return 0;
    }
    return mp_rel_window_free(&s_link.conns[conn]);
}

int mp_link_send_state(int conn, const void* msg, int len) {
    mp_conn_t* c;

    if (!mp_link_is_open(conn) || len <= 0 || len > MP_STATE_MAX) {
        return 0;
    }
    c = &s_link.conns[conn];
    {
        int ch = ((const uint8_t*)msg)[0] % MP_STATE_CHANS;

        memcpy(c->state_data[ch], msg, len);
        c->state_len[ch] = (uint16_t)len;
        c->state_pending[ch] = 1;
    }
    return 1;
}

int mp_link_bulk_send(int conn, const void* data, int len) {
    mp_conn_t* c;
    int i;

    if (!mp_link_is_open(conn) || len <= 0 || len > MP_BULK_MAX) {
        return 0;
    }
    c = &s_link.conns[conn];
    c->bulk_src = (const uint8_t*)data;
    c->bulk_len = len;
    c->bulk_count = (len + MP_BULK_CHUNK - 1) / MP_BULK_CHUNK;
    c->bulk_acked_n = 0;
    c->bulk_xfer++;
    c->bulk_tokens = (float)MP_BULK_BURST;
    memset(c->bulk_acked, 0, sizeof(c->bulk_acked));
    memset(c->bulk_sent_ms, 0, sizeof(c->bulk_sent_ms));
    // late acks for the previous transfer must not tick off this one's chunks
    for (i = 0; i < MP_SENT_RING; i++) {
        int k;

        for (k = 0; k < c->sent[i].n_items; k++) {
            if (c->sent[i].items[k].stream == MP_STREAM_BULK) {
                c->sent[i].items[k].stream = 0xFF;
            }
        }
    }
    return 1;
}

int mp_link_bulk_pending(int conn) {
    return mp_link_is_open(conn) && s_link.conns[conn].bulk_src != NULL;
}

void mp_link_bulk_cancel(int conn) {
    if (mp_link_is_open(conn)) {
        s_link.conns[conn].bulk_src = NULL;
    }
}

void mp_link_hold(int conn, int dead_ms) {
    if (mp_link_is_open(conn)) {
        s_link.conns[conn].hold_ms = dead_ms;
    }
}

int mp_link_quiet(int conn) {
    return mp_link_is_open(conn) && s_link.conns[conn].lost;
}

uint32_t mp_link_silence(int conn) {
    return mp_link_is_open(conn) ? s_link.now_ms - s_link.conns[conn].last_recv_ms : 0xFFFFFFFFu;
}

int mp_link_rel_idle(int conn) {
    return !mp_link_is_open(conn) ||
           (s_link.conns[conn].bl_used == 0 && s_link.conns[conn].rel_base == s_link.conns[conn].rel_next_id);
}

// fills one packet with whatever is due; FALSE when there was nothing to say
static int mp_flush_one(int conn, int force) {
    mp_conn_t* c = &s_link.conns[conn];
    uint32_t now = s_link.now_ms;
    uint8_t pkt[MP_PKT_MAX];
    int pos = MP_PKT_HDR;
    mp_sent_pkt_t* rec = &c->sent[c->next_seq % MP_SENT_RING];
    int rto = mp_rto_ms(c);
    uint16_t id;

    rec->valid = 1;
    rec->seq = c->next_seq;
    rec->n_items = 0;
    rec->sent_ms = now;

    // state rides first and is never resent: a newer one supersedes it
    {
        int ch;

        for (ch = 0; ch < MP_STATE_CHANS; ch++) {
            if (c->state_pending[ch] && pos + 3 + c->state_len[ch] <= MP_PKT_MAX) {
                pkt[pos] = MP_STREAM_STATE;
                wr16(pkt + pos + 1, c->state_len[ch]);
                memcpy(pkt + pos + 3, c->state_data[ch], c->state_len[ch]);
                pos += 3 + c->state_len[ch];
                c->state_pending[ch] = 0;
            }
        }
    }

    for (id = c->rel_base; id != c->rel_next_id; id++) {
        mp_rel_out_t* slot = &c->rel_out[id % MP_REL_WINDOW];

        if (!slot->used || (slot->last_sent_ms != 0 && (int)(now - slot->last_sent_ms) < rto)) {
            continue;
        }
        if (pos + 5 + slot->len > MP_PKT_MAX || rec->n_items == MP_SENT_ITEMS) {
            break;
        }
        pkt[pos] = MP_STREAM_REL;
        wr16(pkt + pos + 1, slot->len);
        wr16(pkt + pos + 3, slot->id);
        memcpy(pkt + pos + 5, slot->data, slot->len);
        pos += 5 + slot->len;
        slot->last_sent_ms = now;
        rec->items[rec->n_items].stream = MP_STREAM_REL;
        rec->items[rec->n_items].id = slot->id;
        rec->n_items++;
    }

    if (c->bulk_src != NULL) {
        int in_flight = 0;
        int i;

        for (i = 0; i < c->bulk_count; i++) {
            if (!mp_bit(c->bulk_acked, i) && c->bulk_sent_ms[i] != 0 && (int)(now - c->bulk_sent_ms[i]) < rto) {
                in_flight++;
            }
        }
        for (i = 0; i < c->bulk_count && in_flight < MP_BULK_WINDOW && rec->n_items < MP_SENT_ITEMS; i++) {
            int chunk_len;

            if (mp_bit(c->bulk_acked, i) || (c->bulk_sent_ms[i] != 0 && (int)(now - c->bulk_sent_ms[i]) < rto)) {
                continue;
            }
            chunk_len = c->bulk_len - i * MP_BULK_CHUNK;
            if (chunk_len > MP_BULK_CHUNK) {
                chunk_len = MP_BULK_CHUNK;
            }
            if (pos + 8 + chunk_len > MP_PKT_MAX || c->bulk_tokens < (float)chunk_len) {
                break;
            }
            pkt[pos] = MP_STREAM_BULK;
            wr16(pkt + pos + 1, (uint16_t)chunk_len);
            wr16(pkt + pos + 3, (uint16_t)i);
            wr16(pkt + pos + 5, (uint16_t)c->bulk_count);
            pkt[pos + 7] = c->bulk_xfer;
            memcpy(pkt + pos + 8, c->bulk_src + i * MP_BULK_CHUNK, chunk_len);
            pos += 8 + chunk_len;
            c->bulk_sent_ms[i] = now;
            c->bulk_tokens -= (float)chunk_len;
            in_flight++;
            rec->items[rec->n_items].stream = MP_STREAM_BULK;
            rec->items[rec->n_items].id = (uint16_t)i;
            rec->n_items++;
        }
    }

    if (pos == MP_PKT_HDR && !force) {
        rec->valid = 0;
        return 0;
    }

    pkt[0] = MP_PROTO_MAGIC;
    pkt[1] = c->recv_any ? MP_FLAG_ACKS : 0;
    pkt[2] = s_link.self_slot;
    pkt[3] = MP_LINK_PROTO;
    wr32(pkt + 4, c->conn_id);
    wr16(pkt + 8, c->next_seq);
    wr16(pkt + 10, c->recv_top);
    wr32(pkt + 12, c->recv_bits);
    mp_tr_send(&c->addr, pkt, pos);
    c->next_seq++;
    c->last_send_ms = now;
    c->ack_pending = 0;
    return 1;
}

// receiving

// FALSE for a duplicate or hopelessly old packet
static int mp_note_recv(mp_conn_t* c, uint16_t seq) {
    int16_t d;
    int back;

    if (!c->recv_any) {
        c->recv_any = 1;
        c->recv_top = seq;
        c->recv_bits = 0;
        return 1;
    }

    d = (int16_t)(seq - c->recv_top);
    if (d > 0) {
        if (d > 32) {
            c->recv_bits = 0;
        } else {
            c->recv_bits = (d == 32 ? 0 : c->recv_bits << d) | (1u << (d - 1));
        }
        c->recv_top = seq;
        return 1;
    }
    if (d == 0) {
        return 0;
    }
    back = -d - 1;
    if (back >= 32 || (c->recv_bits & (1u << back))) {
        return 0;
    }
    c->recv_bits |= 1u << back;
    return 1;
}

static void mp_ack_one(int conn, uint16_t seq, int newest) {
    mp_conn_t* c = &s_link.conns[conn];
    mp_sent_pkt_t* rec = &c->sent[seq % MP_SENT_RING];
    int i;

    if (!rec->valid || rec->seq != seq) {
        return;
    }
    rec->valid = 0;

    if (newest) {
        float sample = (float)(s_link.now_ms - rec->sent_ms);

        if (!c->rtt_valid) {
            c->srtt = sample;
            c->rttvar = sample * 0.5f;
            c->rtt_valid = 1;
        } else {
            float err = sample - c->srtt;

            c->srtt += 0.125f * err;
            c->rttvar += 0.25f * ((err < 0 ? -err : err) - c->rttvar);
        }
    }

    for (i = 0; i < rec->n_items; i++) {
        uint16_t id = rec->items[i].id;

        if (rec->items[i].stream == MP_STREAM_REL) {
            mp_rel_out_t* slot = &c->rel_out[id % MP_REL_WINDOW];

            if (slot->used && slot->id == id) {
                slot->used = 0;
            }
        } else if (rec->items[i].stream == MP_STREAM_BULK && c->bulk_src != NULL && id < c->bulk_count &&
                   !mp_bit(c->bulk_acked, id)) {
            mp_bit_set(c->bulk_acked, id);
            c->bulk_acked_n++;
        }
    }
}

static void mp_apply_acks(int conn, uint16_t ack, uint32_t bits) {
    mp_conn_t* c = &s_link.conns[conn];
    int i;

    mp_ack_one(conn, ack, 1);
    for (i = 0; i < 32; i++) {
        if (bits & (1u << i)) {
            mp_ack_one(conn, (uint16_t)(ack - 1 - i), 0);
        }
    }
    while (c->rel_base != c->rel_next_id && !c->rel_out[c->rel_base % MP_REL_WINDOW].used) {
        c->rel_base++;
    }
    if (c->bulk_src != NULL && c->bulk_acked_n == c->bulk_count) {
        c->bulk_src = NULL;
        mp_emit(MP_EV_BULK_SENT, conn, &c->addr, NULL, 0);
    }
}

static void mp_rel_receive(int conn, uint16_t id, const uint8_t* data, int len) {
    mp_conn_t* c = &s_link.conns[conn];
    int16_t d = (int16_t)(id - c->rel_expect);
    mp_rel_in_t* slot;

    if (d < 0 || d >= MP_REL_WINDOW) {
        return; // already delivered
    }
    if (d > 0) {
        slot = &c->rel_in[id % MP_REL_WINDOW];
        if (!slot->present) {
            slot->present = 1;
            slot->len = (uint16_t)len;
            memcpy(slot->data, data, len);
        }
        return;
    }

    c->rel_expect++;
    mp_emit(MP_EV_REL, conn, &c->addr, data, len);
    // the callback may have closed us
    while (c->open && c->rel_in[c->rel_expect % MP_REL_WINDOW].present) {
        slot = &c->rel_in[c->rel_expect % MP_REL_WINDOW];
        slot->present = 0;
        c->rel_expect++;
        mp_emit(MP_EV_REL, conn, &c->addr, slot->data, slot->len);
    }
}

static void mp_process_packet(const mp_addr_t* from, const uint8_t* pkt, int len) {
    mp_conn_t* c = NULL;
    uint32_t conn_id;
    uint16_t seq;
    int conn;
    int pos;

    if (len < MP_PKT_HDR || pkt[0] != MP_PROTO_MAGIC) {
        return;
    }
    // connectionless traffic goes up even across versions so HELLO can be refused politely
    if (pkt[1] & MP_FLAG_CTRL) {
        mp_emit(MP_EV_CTRL, -1, from, pkt + MP_PKT_HDR, len - MP_PKT_HDR);
        return;
    }
    if (pkt[3] != MP_LINK_PROTO) {
        return;
    }

    conn_id = rd32(pkt + 4);
    for (conn = 0; conn < MP_LINK_CONNS; conn++) {
        if (s_link.conns[conn].open && s_link.conns[conn].conn_id == conn_id &&
            mp_link_addr_equal(&s_link.conns[conn].addr, from)) {
            c = &s_link.conns[conn];
            break;
        }
    }
    if (c == NULL) {
        // a quiet peer turning up at a new address (new lease, NAT remap) keeps its connection
        for (conn = 0; conn < MP_LINK_CONNS; conn++) {
            mp_conn_t* m = &s_link.conns[conn];

            if (m->open && m->lost && m->conn_id == conn_id && m->addr.kind == from->kind) {
                m->addr = *from;
                c = m;
                break;
            }
        }
        if (c == NULL) {
            return;
        }
    }

    c->last_recv_ms = s_link.now_ms;
    if (c->lost) {
        c->lost = 0;
        c->away = 0;
        c->bl_lost = 0; // the resync this triggers covers anything dropped
        mp_emit(MP_EV_BACK, conn, from, NULL, 0);
        if (!c->open) {
            return;
        }
    }

    seq = rd16(pkt + 8);
    if (pkt[1] & MP_FLAG_ACKS) {
        mp_apply_acks(conn, rd16(pkt + 10), rd32(pkt + 12));
    }
    if (!mp_note_recv(c, seq)) {
        return;
    }

    pos = MP_PKT_HDR;
    while (c->open && pos + 3 <= len) {
        int stream = pkt[pos];
        int mlen = rd16(pkt + pos + 1);

        c->ack_pending = 1;
        if (stream == MP_STREAM_STATE) {
            if (pos + 3 + mlen > len) {
                break;
            }
            int ch = (mlen > 0) ? pkt[pos + 3] % MP_STATE_CHANS : 0;

            if (!c->state_in_any[ch] || (int16_t)(seq - c->state_in_seq[ch]) > 0) {
                c->state_in_any[ch] = 1;
                c->state_in_seq[ch] = seq;
                mp_emit(MP_EV_STATE, conn, from, pkt + pos + 3, mlen);
            }
            pos += 3 + mlen;
        } else if (stream == MP_STREAM_REL) {
            if (pos + 5 + mlen > len || mlen > MP_REL_MAX) {
                break;
            }
            mp_rel_receive(conn, rd16(pkt + pos + 3), pkt + pos + 5, mlen);
            pos += 5 + mlen;
        } else if (stream == MP_STREAM_BULK) {
            int idx;
            int count;
            uint8_t xfer;

            if (pos + 8 + mlen > len || mlen > MP_BULK_CHUNK) {
                break;
            }
            idx = rd16(pkt + pos + 3);
            count = rd16(pkt + pos + 5);
            xfer = pkt[pos + 7];
            // a newer transfer starts a fresh chunk map; stragglers from older ones are dropped
            if (!c->bulk_rx_any || (int8_t)(xfer - c->bulk_rx_xfer) > 0) {
                c->bulk_rx_any = 1;
                c->bulk_rx_xfer = xfer;
                memset(c->bulk_seen, 0, sizeof(c->bulk_seen));
            }
            if (xfer == c->bulk_rx_xfer && count > 0 && count <= MP_BULK_CHUNKS && idx < count &&
                !mp_bit(c->bulk_seen, idx)) {
                mp_bit_set(c->bulk_seen, idx);
                mp_emit_bulk(MP_EV_BULK, conn, from, pkt + pos + 8, mlen, idx, count, xfer);
            }
            pos += 8 + mlen;
        } else {
            break;
        }
    }
}

// tick

static void mp_link_tick_body(uint32_t now_ms);

void mp_link_tick(uint32_t now_ms) {
    if (!s_link.running) {
        return;
    }
    s_link.in_tick = 1;
    mp_link_tick_body(now_ms);
    s_link.in_tick = 0;
    if (s_link.stop_requested) {
        mp_link_stop();
    }
}

static void mp_link_tick_body(uint32_t now_ms) {
    static uint8_t buf[MP_PKT_MAX];
    mp_addr_t from;
    uint32_t dt;
    int conn;
    int n;
    int i;

    dt = s_link.last_tick_ms != 0 ? now_ms - s_link.last_tick_ms : 0;
    s_link.now_ms = now_ms;
    s_link.last_tick_ms = now_ms;

    for (i = 0; i < MP_TICK_PKTS_IN; i++) {
        n = s_link.tr.recv(&from, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        mp_process_packet(&from, buf, n);
        if (s_link.stop_requested) {
            return;
        }
    }

    for (conn = 0; conn < MP_LINK_CONNS; conn++) {
        mp_conn_t* c = &s_link.conns[conn];
        uint32_t quiet;

        if (!c->open) {
            continue;
        }

        quiet = now_ms - c->last_recv_ms;
        if ((int)quiet >= (c->hold_ms != 0 ? c->hold_ms : s_link.timing.dead_ms)) {
            mp_addr_t addr = c->addr;

            memset(c, 0, sizeof(*c));
            mp_emit(MP_EV_DEAD, conn, &addr, NULL, 0);
            if (s_link.stop_requested) {
                return;
            }
            continue;
        }
        if ((int)quiet >= s_link.timing.lost_ms && !c->lost) {
            c->lost = 1;
            mp_emit(MP_EV_LOST, conn, &c->addr, NULL, 0);
            if (s_link.stop_requested) {
                return;
            }
            if (!c->open) {
                continue;
            }
        }
        if ((int)quiet >= s_link.timing.dead_ms && c->lost && !c->away) {
            c->away = 1;
            mp_emit(MP_EV_AWAY, conn, &c->addr, NULL, 0);
            if (s_link.stop_requested) {
                return;
            }
            if (!c->open) {
                continue;
            }
        }

        mp_bl_drain(c);
        if (c->bl_lost && c->bl_used == 0 && !c->lost) {
            mp_addr_t addr = c->addr;

            c->bl_lost = 0;
            mp_emit(MP_EV_BACK, conn, &addr, NULL, 0);
            if (s_link.stop_requested) {
                return;
            }
            if (!c->open) {
                continue;
            }
        }

        if (c->bulk_src != NULL) {
            c->bulk_tokens += (float)dt * (float)MP_BULK_RATE_KBS * 1.024f;
            if (c->bulk_tokens > (float)MP_BULK_BURST) {
                c->bulk_tokens = (float)MP_BULK_BURST;
            }
        }

        for (i = 0; i < MP_TICK_PKTS_OUT; i++) {
            int force = (i == 0) && (c->ack_pending || (int)(now_ms - c->last_send_ms) >= MP_KEEPALIVE_MS);

            if (!mp_flush_one(conn, force)) {
                break;
            }
        }
    }
}
