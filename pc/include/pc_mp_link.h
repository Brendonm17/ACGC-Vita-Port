// pc_mp_link.h
// multiplayer wire protocol: framing, acks, reliable and bulk streams; ticked, single-threaded
#ifndef PC_MP_LINK_H
#define PC_MP_LINK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MP_PROTO_MAGIC 0xAC
#define MP_PKT_MAX     1200
#define MP_PKT_HDR     16
#define MP_REL_MAX     480  // largest reliable message
#define MP_REL_BACKLOG (16 * 1024) // reliable messages waiting for a window slot
#define MP_STATE_MAX   1100 // largest state message
#define MP_STATE_CHANS 8    // state messages keep a newest-wins slot per first byte (mod this)
#define MP_BULK_CHUNK  1024
#define MP_BULK_MAX    (256 * MP_BULK_CHUNK)
#define MP_CTRL_MAX    (MP_PKT_MAX - MP_PKT_HDR)
#define MP_LINK_CONNS  3    // host: one per guest; guest: just the host

// the platform's memory for link buffers (vita_mp_net.c keeps it off the game's heap)
void* mp_calloc(unsigned int count, unsigned int size);
void mp_free(void* p);

typedef enum {
    MP_ADDR_NONE,
    MP_ADDR_IP,
    MP_ADDR_MAC,
} mp_addr_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t mac[6];
    uint16_t port;
    uint32_t ip; // host byte order
} mp_addr_t;

typedef struct {
    int (*send)(const mp_addr_t* to, const void* data, int len);
    int (*recv)(mp_addr_t* from, void* buf, int cap); // 0 when nothing is waiting
    int (*broadcast)(uint16_t port, const void* data, int len);
} mp_transport_t;

typedef enum {
    MP_EV_CTRL,      // connectionless message from ev->from
    MP_EV_REL,       // reliable message, delivered in order
    MP_EV_STATE,     // newest state message
    MP_EV_BULK,      // one bulk chunk (idx / count)
    MP_EV_BULK_SENT, // our bulk transfer is fully acknowledged
    MP_EV_LOST,      // peer went quiet
    MP_EV_AWAY,      // quiet past the usual timeout on a held connection
    MP_EV_BACK,      // talking again after LOST, or an overflowed backlog drained: resend state
    MP_EV_DEAD,      // peer gone; the connection is already closed
} mp_ev_type_t;

typedef struct {
    int type;
    int conn;
    mp_addr_t from;
    const uint8_t* data;
    int len;
    int bulk_idx;
    int bulk_count;
    int bulk_xfer; // changes with every new transfer on the connection
} mp_event_t;

typedef void (*mp_event_cb_t)(const mp_event_t* ev);

typedef struct {
    int lost_ms;    // freeze puppets
    int dead_ms;    // drop the peer
    int rto_min_ms; // resend floor: 100 local, 200 online
} mp_link_timing_t;

int mp_link_start(const mp_transport_t* tr, const mp_link_timing_t* timing, uint8_t self_slot, mp_event_cb_t cb);
void mp_link_stop(void);
int mp_link_running(void);
void mp_link_tick(uint32_t now_ms);

int mp_link_send_ctrl(const mp_addr_t* to, const void* msg, int len);
int mp_link_bcast_ctrl(uint16_t port, const void* msg, int len);

int mp_link_open(const mp_addr_t* to, uint32_t conn_id); // connection index, or -1
void mp_link_close(int conn);
int mp_link_is_open(int conn);
const mp_addr_t* mp_link_addr(int conn);
int mp_link_send_rel(int conn, const void* msg, int len);   // FALSE only once the backlog is full too
int mp_link_rel_room(int conn);                             // free window slots with nothing backlogged
int mp_link_rel_fits(int conn, int bytes, int count);       // room for these without any dropped
int mp_link_send_state(int conn, const void* msg, int len); // replaces any unsent state
int mp_link_bulk_send(int conn, const void* data, int len); // data must outlive the transfer
int mp_link_bulk_pending(int conn);
void mp_link_bulk_cancel(int conn);          // before freeing the data of a transfer in flight
void mp_link_hold(int conn, int dead_ms);    // how long a quiet peer keeps its connection (0: the usual)
int mp_link_quiet(int conn);                 // LOST and not heard from since
uint32_t mp_link_silence(int conn);          // ms since the peer was last heard
int mp_link_rel_idle(int conn);              // every reliable message sent has been acknowledged
int mp_link_rtt_ms(int conn);
int mp_link_addr_equal(const mp_addr_t* a, const mp_addr_t* b);

#ifdef __cplusplus
}
#endif

#endif
