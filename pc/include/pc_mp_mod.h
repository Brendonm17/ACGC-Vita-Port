// pc_mp_mod.h - multiplayer API for mods: the gameplay mod set, message channels, requests the host answers,
// ownership, host data mirrored to visitors, latest-value state and player events
#ifndef PC_MP_MOD_H
#define PC_MP_MOD_H

#define MP_MOD_API       1     // bumped when this header changes in a way a built mod would notice
#define MP_MOD_MSG_MAX   472   // payload bytes in one message, request or answer
#define MP_MOD_STATE_MAX 200   // bytes in one latest value
#define MP_MOD_BLOB_MAX  16384 // bytes in one mirrored block
#define MP_MOD_TO_ALL    (-1)  // every other player in the town
#define MP_MOD_TO_HOST   0

// request results
#define MP_MOD_REFUSED  0
#define MP_MOD_ACCEPTED 1
#define MP_MOD_UNSERVED (-1) // the host has no handler on that channel (it doesn't have the mod)
#define MP_MOD_LOST     (-2) // the session ended before the answer came

// player events: ONLINE and OFFLINE are this game's own session (slot = its own), JOIN and LEAVE the others
enum { MP_MOD_ONLINE, MP_MOD_OFFLINE, MP_MOD_JOIN, MP_MOD_LEAVE };

typedef void (*mp_mod_recv_fn)(int from_slot, const void* data, int len);
typedef int (*mp_mod_serve_fn)(int from_slot, const void* req, int len, void* reply, int* reply_len);
typedef void (*mp_mod_answer_fn)(int result, const void* reply, int len, void* user);
typedef void (*mp_mod_owner_fn)(unsigned int channel, unsigned int key, int owner_slot);
typedef void (*mp_mod_blob_fn)(unsigned int blob_id, int offset, int len);
typedef void (*mp_mod_player_fn)(int event, int slot);

#ifdef VITA_MP

// A mod that changes gameplay adds itself at boot, before any session. A town only takes visitors with the same
// set; client-only mods never call this
void mp_mod_gameplay_add(unsigned int mod_id, unsigned int mod_version);
unsigned int mp_mod_set_hash(void); // 0 with no gameplay mods

// Everything below runs on the game thread between frames. Slot 0 is the host, 1-3 visitors. Channel, key and
// block ids are the mod's own (FNV-1a of a unique name, say).

// messages: reliable, in order per sender; the host passes them on even without the mod
int mp_mod_listen(unsigned int channel, mp_mod_recv_fn fn); // NULL stops listening; FALSE when the table is full
int mp_mod_send(unsigned int channel, int to, const void* data, int len); // to: a slot or MP_MOD_TO_ALL

// requests: the host's handler decides and may fill reply (up to MP_MOD_MSG_MAX bytes). Any game may set a
// handler, only the host's runs. On the host itself done is called before mp_mod_request returns
int mp_mod_serve(unsigned int channel, mp_mod_serve_fn fn);
int mp_mod_request(unsigned int channel, const void* data, int len, mp_mod_answer_fn done, void* user);

// ownership: the host keeps who owns each key. A claim on a free key wins, a claim on another player's key is
// ignored, a leaving player's keys are let go, and the host may let go of any key. Offline nobody owns anything
int mp_mod_claim(unsigned int channel, unsigned int key);
void mp_mod_release(unsigned int channel, unsigned int key);
int mp_mod_owner(unsigned int channel, unsigned int key); // -1 when nobody owns it
int mp_mod_watch_owners(unsigned int channel, mp_mod_owner_fn fn);

// mirrored data: the host's copy of a block overwrites every visitor's. Register the same id and size on every
// game; the host's changes are found each frame and newcomers get the whole block. Visitors' writes don't travel
int mp_mod_blob(unsigned int blob_id, void* data, int size, mp_mod_blob_fn changed);

// latest value per player and channel, for things that change every frame: sent about 30 times a second, and a
// lost one is covered by the next; listeners only see values newer than the last
int mp_mod_state_send(unsigned int channel, const void* data, int len);
int mp_mod_state_listen(unsigned int channel, mp_mod_recv_fn fn);

// player events
int mp_mod_on_player(mp_mod_player_fn fn);

// session facts
int mp_mod_online(void);
int mp_mod_is_host(void);
int mp_mod_self(void);        // own slot, -1 offline
int mp_mod_present(int slot); // that player is in the town (FALSE for every slot offline)
int mp_mod_town_writer(void); // this game may change the town's saved state (offline, or the host)

#else

// single-player builds: everything answers as offline
static inline void mp_mod_gameplay_add(unsigned int mod_id, unsigned int mod_version) {
    (void)mod_id;
    (void)mod_version;
}
static inline unsigned int mp_mod_set_hash(void) { return 0; }
static inline int mp_mod_listen(unsigned int channel, mp_mod_recv_fn fn) {
    (void)channel;
    (void)fn;
    return 1;
}
static inline int mp_mod_send(unsigned int channel, int to, const void* data, int len) {
    (void)channel;
    (void)to;
    (void)data;
    (void)len;
    return 0;
}
static inline int mp_mod_serve(unsigned int channel, mp_mod_serve_fn fn) {
    (void)channel;
    (void)fn;
    return 1;
}
static inline int mp_mod_request(unsigned int channel, const void* data, int len, mp_mod_answer_fn done, void* user) {
    (void)channel;
    (void)data;
    (void)len;
    (void)done;
    (void)user;
    return 0;
}
static inline int mp_mod_claim(unsigned int channel, unsigned int key) {
    (void)channel;
    (void)key;
    return 0;
}
static inline void mp_mod_release(unsigned int channel, unsigned int key) {
    (void)channel;
    (void)key;
}
static inline int mp_mod_owner(unsigned int channel, unsigned int key) {
    (void)channel;
    (void)key;
    return -1;
}
static inline int mp_mod_watch_owners(unsigned int channel, mp_mod_owner_fn fn) {
    (void)channel;
    (void)fn;
    return 1;
}
static inline int mp_mod_blob(unsigned int blob_id, void* data, int size, mp_mod_blob_fn changed) {
    (void)blob_id;
    (void)data;
    (void)size;
    (void)changed;
    return 1;
}
static inline int mp_mod_state_send(unsigned int channel, const void* data, int len) {
    (void)channel;
    (void)data;
    (void)len;
    return 0;
}
static inline int mp_mod_state_listen(unsigned int channel, mp_mod_recv_fn fn) {
    (void)channel;
    (void)fn;
    return 1;
}
static inline int mp_mod_on_player(mp_mod_player_fn fn) {
    (void)fn;
    return 1;
}
static inline int mp_mod_online(void) { return 0; }
static inline int mp_mod_is_host(void) { return 0; }
static inline int mp_mod_self(void) { return -1; }
static inline int mp_mod_present(int slot) {
    (void)slot;
    return 0;
}
static inline int mp_mod_town_writer(void) { return 1; }

#endif

#endif
