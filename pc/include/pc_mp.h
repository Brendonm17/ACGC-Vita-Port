// pc_mp.h - real-time multiplayer ("Friends' Towns"): host town + up to 3 visitors
#ifndef PC_MP_H
#define PC_MP_H

#ifdef VITA_MP
#include "pc_mp_link.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef VITA_MP

#define MP_MAX_PEERS     4
#define MP_PROTO_VERSION 14

typedef enum {
    MP_ROLE_NONE,
    MP_ROLE_HOST,
    MP_ROLE_GUEST,
} mp_role_t;

// every decomp hook checks mp_active() first so no session == vanilla
int mp_active(void);
int mp_is_host(void);
int mp_is_guest(void);
mp_role_t mp_role(void);
void mp_set_role(mp_role_t role);

// reliable message types (first payload byte); each module owns a range
enum {
    MP_M_SNAP_REQ = 0x10, // guest: send me the town
    MP_M_SNAP_INFO,       // host: raw size, packed size, crc; the bulk transfer follows
    MP_M_TIME,            // host: game clock
    MP_M_ARRIVED,         // guest: stepped off the train
    MP_M_LEAVING,         // guest: boarding the train home
    MP_M_CLOSING,         // host: last train, then the line closes
    MP_M_PULLING_IN,      // guest: its train is pulling into the host's station now
    MP_M_RULES,           // host: its switches for the shared town, on joining and on a change
    MP_M_TRAIN_STATE,     // the screen running the train: where it is and what it's doing, for the others

    MP_M_LOOK = 0x20, // one player's appearance; the host relays guests' looks
    MP_M_DESIGN,      // half of a custom cloth or umbrella design
    MP_M_GONE,        // host: a player left the town
    MP_M_DOOR,        // a player went through a building's door, in or out
    MP_M_VFX,         // something a player's action set moving in the town: drops, holes, tree sways
    MP_M_LIGHT,       // a room's ceiling light switched; the host answers with all of them
    MP_M_CHAT,        // a player's chat message; the host checks it and passes it on
    MP_M_CHAT_TYPING, // a player's keyboard opened or closed (the typing dots)

    MP_M_OP = 0x30, // guest: town cells it changed, with the values it saw before
    MP_M_ACK,       // host: accepted, moved or refused, with the host's values
    MP_M_DELTA,     // host: town cells that changed
    MP_M_COMMIT,    // host: ops up to here are in a finished save
    MP_M_BYTES,     // host: a run of the rest of the save
    MP_M_CELLS,     // host: a page of town cells
    MP_M_CRCS,      // host: page checksums
    MP_M_PAGE_REQ,  // guest: pages that didn't match
    MP_M_SOFT,      // guest: a merge (shop purchases)
    MP_M_HURRY,     // guest: leaving; save what my pickups need soon
    MP_M_COTTAGE,   // the island's cottage: whose hands are on what, and its lamps
    MP_M_CLAIM,     // host: a one-of-a-kind thing in a shop is the asker's to buy, or another's

    MP_M_TALK_LOCK = 0x40, // a player started talking to a villager
    MP_M_TALK_UNLOCK,      // ...and finished
    MP_M_TALK_RUNS,        // guest: bytes its talk changed
    MP_M_TALK_MEM,         // guest: its memory in the villager it talked to
    MP_M_TALK_END,         // guest: that was the whole result
    MP_M_TALK_DONE,        // host: merged
    MP_M_NPC_OWNERS,       // host: whose game moves each villager
    MP_M_NPC_LIST,         // host: where the villagers nobody is watching are
    MP_M_NPC_OWN,          // guest: mood, clothes and home its game gave a villager it moves
    MP_M_NPC_CLAIM,        // a player's game takes a villager over to answer that player (a net hit)
    MP_M_NPC_EVSTEP,       // one of an event's characters handed back after a talk: its step, for the event's screen
    MP_M_NPC_MAIL,         // guest: what its Pete did; the host's post office does the same
    MP_M_NPC_LETTER,       // guest: a letter handed in at its post office
    MP_M_NPC_LETTER_ANS,   // host: taken in, or Pelly's reason to hand it back

    MP_M_EVSTATE = 0x50, // host: today's event places and which events are running
    MP_M_AERO,           // where the radio exercise song is on a screen playing it
    MP_M_EVPLACE,        // guest: places its game gave events while the host is away from town
    MP_M_EVGONE,         // guest: an event of the host's ended here while the host plays an NES game
    MP_M_EVMOVE,         // guest: a talk here sent one of the host's event characters on to another spot
    MP_M_EVROSTER,       // guest: an event is on here and its villagers aren't picked yet; the host picks them

    MP_M_CR_WADE = 0x60, // guest: stepping into an acre; the host rolls its bugs and fish
    MP_M_CR_SPAWN,       // host: a bug or fish shadow is out in an acre
    MP_M_CR_GONE,        // caught, landed or flown off: every screen drops it
    MP_M_CR_OWNERS,      // host: whose game runs each one
    MP_M_CR_CLAIM,       // a player's game takes some over (its float settling near a fish)
    MP_M_CR_POP,         // guest: shook the tree the present balloon is caught in
    MP_M_CR_LOOK,        // guest: out a door or off the train; the host sends what's out in the acre
    MP_M_CR_SELF,        // host: can't roll that step right now; the visitor rolls these kinds itself
    MP_M_CR_ACT,         // a tree shaken, a hole dug or a rock hit, for the bugs other games run there
    MP_M_CR_SCARE,       // a float landed on a fish another game runs
    MP_M_CR_NOCOPY,      // guest: its setup refused a copy where it is; don't make it the runner for now
    MP_M_CR_NEW,         // guest: one its own game made (a roll left to it, one let go); the host lists it
    MP_M_CR_NET,         // guest: its net closed on one; the host says whose it is
    MP_M_CR_NETTED,      // host: that net keeps it, or another got it first
    MP_M_CR_SELFDONE,    // guest: rolled (or passed on) the acre the host left to it
    MP_M_CR_REID,        // host: a visitor left; what its game made stays out under one of the host's ids

    MP_M_MOD = 0x70, // a mod's message on its own channel; the host passes it on (pc_mp_mod.c)
    MP_M_MOD_REQ,    // guest: a request for the host's handler on a channel
    MP_M_MOD_ANS,    // host: its handler's answer
    MP_M_MOD_CLAIM,  // guest: take a key, or let it go
    MP_M_MOD_OWNER,  // host: who owns a key now
    MP_M_MOD_BLOB,   // host: a page of a block it mirrors to visitors
};

// state stream message types
#define MP_S_PLAYERS 0x01 // one or more player states
#define MP_S_NPCS    0x02 // villager poses from the games that move them
#define MP_S_CRITTERS 0x03 // bugs, fish shadows and the present balloon from the games that run them
#define MP_S_EVAREA   0x04 // a host's grouped event (a race, a tug of war) as the screen running it has it
#define MP_S_MOD      0x05 // mods' latest values, bundled (pc_mp_mod.c)
#define MP_S_PORTER   0x06 // the Porter as the screen whose player has him busy shows him

// lobby (pc_mp_lobby.c): discovery, handshake, line bookkeeping
void mp_lobby_tick(unsigned int now_ms);
int mp_lobby_host_conn(void);   // guest: connection to the host, or -1
void mp_lobby_guest_end(void);  // guest: say goodbye and drop the line
int mp_lobby_guest_slot(int conn); // host: guest slot on this connection, or -1
int mp_lobby_guest_conn(int slot); // host: connection of a guest slot, or -1
int mp_lobby_self_slot(void);      // 0 for the host, 1..3 for a guest
int mp_lobby_guest_names(int conn, unsigned char* name, unsigned char* town);
int mp_lobby_guest_pid(int conn, void* pid); // the PersonalID_c the guest on this connection travels as
void mp_lobby_guest_mark(int conn, int arrived, int leaving);
void mp_lobby_guest_boarding(int conn); // host: this guest has the town and is on its way
int mp_lobby_guest_arrived(int slot);
void mp_lobby_hold_line(void);          // guest: in town; a dropped line is held a while
int mp_lobby_conn_quiet(int conn);      // the peer went quiet: state that repeats can wait
int mp_lobby_rel_room(int conn);        // free reliable slots, for traffic that can wait
int mp_lobby_rel_fits(int conn, int bytes, int count); // room for these reliable messages, none dropped

// mods (pc_mp_mod.c; their own API is pc_mp_mod.h)
void mp_mod_on_rel(int conn, const unsigned char* data, int len);
void mp_mod_on_state(int conn, const unsigned char* data, int len);
void mp_mod_tick(void); // every frame, sessions or not
void mp_mod_reset(void);
void mp_lobby_pump(void);               // a moment for the line: what it has to send and hear
unsigned int mp_lobby_conn_silence(int conn); // ms since the peer was last heard (huge if closed)
void mp_lobby_send_state_to(int conn, const void* msg, int len);
int mp_lobby_send_rel(int conn, const void* msg, int len);
void mp_lobby_send_rel_all(const void* msg, int len);
void mp_lobby_shutdown(void);   // process ending: hang up on everyone
void mp_lobby_host_hangup(void); // host left the town: close the line at once
void mp_lobby_on_resume(int certain); // back from sleep: sockets reopen, ad hoc is over

// the host's switches for its shared town (settings.ini [Online]); a visitor goes by the host's
enum {
    MP_RULE_VISITOR_RIGHTS, // visitors do what residents do: the museum, the bank and the rest
    MP_RULE_ITEMS,          // visitors pick up, drop, dig up, bury and plant things
    MP_RULE_DIG,            // visitors dig holes
    MP_RULE_AXE,            // visitors cut trees down
    MP_RULE_TUNE,           // visitors change the town tune
    MP_RULE_BOARD,          // visitors post on the bulletin board
    MP_RULE_COTTAGE,        // visitors rearrange the island's cottage
    MP_RULE_DESIGNS,        // visitors change the Able Sisters' displays and the island's flag
    MP_RULE_CHAT,           // anyone chats in the town
    MP_RULE_NUM,
};
int mp_rule(int rule);
int mp_visitor_may(int rule); // anyone but a visitor always may; a visitor as the host allows

// hash of protocol + save layout; peers must match exactly
unsigned int pc_mp_build_id(void);

void pc_mp_pump(void);        // main thread every frame, including loads
void pc_mp_frame_begin(void); // game_main, after mTM_time
void pc_mp_frame_end(void);   // game_main, after the game exec
void pc_mp_on_app_exit(void); // best-effort goodbye before the process ends
void pc_mp_on_resume(int certain); // back from sleep; certain when the system reported it

// errors only, into the game's one log (error.log)
void pc_mp_log(const char* fmt, ...);

// for the stall watchdog: frames the game thread finished and the last place it passed
extern const char* volatile g_mp_crumb;
extern volatile unsigned int g_mp_frame;
#define MP_CRUMB(s) (g_mp_crumb = (s))

// virtual dialogue (pc_mp_text.c): message ids from 0x7000, choice labels from 0x0300
int pc_mp_text_is_msg(int idx);
int pc_mp_text_load_msg(unsigned char* dst, int cap, int idx);
int pc_mp_text_is_pa(int idx);
int pc_mp_text_is_choice(int idx);
void pc_mp_text_load_choice(unsigned char* dst16, int idx);
unsigned int pc_mp_text_hash(void);

// Porter-facing session calls (pc_mp_lobby.c); names are 8-byte AC charset, space padded
#define MP_NAME_LEN   8
#define MP_SCAN_MAX   12
#define MP_TICKET_LEN 8

typedef enum {
    MP_LINK_ADHOC,
    MP_LINK_LAN,
    MP_LINK_ONLINE,
} mp_link_t;

typedef enum {
    MP_UI_BUSY,
    MP_UI_DONE,
    MP_UI_NO_WIFI,
    MP_UI_ADHOC_CANCEL,
    MP_UI_NO_ANSWER,
    MP_UI_NO_ANSWER_HOME,
    MP_UI_PENDING,
    MP_UI_REFUSE_FULL,
    MP_UI_REFUSE_VERSION,
    MP_UI_REFUSE_SAME_TOWN,
    MP_UI_REFUSE_RESIDENT,
    MP_UI_REFUSE_DUPLICATE,
    MP_UI_REFUSE_CLOSED,
    MP_UI_REFUSE_DENIED,
    MP_UI_REFUSE_MODS, // the town runs a different set of gameplay mods
    MP_UI_NOPORTMAP,
    MP_UI_CGNAT,
    MP_UI_NOBIND,
    MP_UI_WIFI_WAIT, // just left ad hoc; Wi-Fi is still coming back
} mp_ui_result_t;

int mp_ui_available(void);
void mp_ui_done(void); // Porter's talk ended: a guest radio nobody is using goes back to Wi-Fi

void mp_ui_scan_start(mp_link_t link);
int mp_ui_scan_poll(void);
int mp_ui_scan_count(void);
void mp_ui_scan_entry(int i, unsigned char* town, unsigned char* host, int* guests);

void mp_ui_join_scan(int i);
int mp_ui_join_ticket(const unsigned char* text, int len); // FALSE when it isn't a ticket
int mp_ui_join_poll(void);
void mp_ui_join_info(unsigned char* town, unsigned char* host, int* others);
unsigned short mp_ui_join_land_id(void);
int mp_ui_host_arrived(void);                    // visitors in town
int mp_ui_host_inbound(unsigned char* town);     // trains on their way (the first one's town)
void mp_ui_host_visitor(int i, unsigned char* name, unsigned char* town); // the i-th visitor in town
int mp_ui_host_thanks(unsigned char* name);      // a visitor whose train home just left, once
void mp_ui_join_cancel(void);

void mp_ui_host_start(mp_link_t link);
int mp_ui_host_poll(void);
int mp_ui_hosting(void);
int mp_ui_host_closing(void); // last train announced, waiting for visitors to leave
mp_link_t mp_ui_host_link(void);
int mp_ui_host_ticket(unsigned char* out9); // "XXXX XXXX"; FALSE when the link has none
int mp_ui_host_port(void);
int mp_ui_host_guest_count(void);
void mp_ui_host_guest(int i, unsigned char* name, unsigned char* town);
void mp_ui_host_close(void);

// ticket: 8 vowel-free chars carrying an IPv4 address, a port slot and a check
void mp_ticket_encode(unsigned int ip, int port_idx, unsigned char* out8);
int mp_ticket_decode(const unsigned char* text, int len, unsigned int* ip, int* port_idx);

unsigned int pc_mp_now_ms(void);
unsigned int pc_mp_random32(void);

// network travel (pc_mp_travel.c)
typedef enum {
    MP_TRAVEL_NONE,
    MP_TRAVEL_FETCHING,  // guest: snapshot requested
    MP_TRAVEL_READY,     // guest: snapshot verified in the travel buffer
    MP_TRAVEL_FAILED,    // guest: the line dropped before the departure save
    MP_TRAVEL_DEPARTED,  // guest: departure saved, riding the train over
    MP_TRAVEL_VISITING,  // guest: in the host's town
    MP_TRAVEL_RETURNING, // guest: return saved, riding the train home
} mp_travel_state_t;

mp_travel_state_t mp_travel_state(void);
int mp_travel_net_trip(void); // guest: departure saved and not home yet
void mp_travel_guest_fetch(void);
int mp_travel_guest_poll(void); // MP_UI_BUSY, MP_UI_DONE or MP_UI_NO_ANSWER
void mp_travel_guest_reset(void);
long long mp_travel_time_delta(void);
void mp_travel_departed(void);
void mp_travel_arrival_begin(void);
void mp_travel_arrival_end(void);
void mp_travel_pulling_in(void); // m_train_control.c: an arrival train set off on this screen
void mp_travel_returning(void);
void mp_travel_home(int saved); // the train home arrived; saved: the home save holds the traveller
int mp_travel_host_closing_left(void); // guest: seconds until the last train, or -1

void mp_travel_on_rel(int conn, const unsigned char* data, int len);
void mp_travel_on_bulk(const mp_event_t* ev);
void mp_travel_on_bulk_sent(int conn);
void mp_travel_on_guest_gone(int conn); // host: a guest's connection closed
void mp_travel_on_host_gone(void);      // guest: the line to the host closed
void mp_travel_on_host_away(void);      // guest: the host has been silent a long while
void mp_travel_on_host_back(void);
void mp_travel_host_tick(unsigned int now_ms);
void mp_travel_host_close(void);
// one train for everyone: the host runs it, or the screen whose own Porter or arrival holds it; the rest follow
int mp_train_follow(void);       // another screen runs the train: this one takes its reports
int mp_train_claim(void);        // this screen's Porter or arrival wants the train now: TRUE when it may run it
int mp_train_menu_guest(void);   // a visitor in a session: its train steps behind its menus too
void mp_train_take(void);        // follower: the newest report into the train
void mp_train_report(int start); // runner: a report when due (start >= 0: a train set off this frame, how)
void mTRC_mp_start_sound(int start); // m_train_control.c: a followed train's first sound
struct game_s;
void mTRC_mp_menu_move(struct game_s* game); // ...its steps behind the host's menu

// players (pc_mp_player.c): everyone's look, movement, sounds and effects; puppets read these
#define MP_FX_MAX 10 // the newest sounds and effects each state repeats, so a lost state loses none

enum {
    MP_SND_ONESHOT,
    MP_SND_WALK,
    MP_SND_WALK_ROOM,
    MP_FX_EFFECT, // effect_make_proc
    MP_FX_KILL,   // effect_kill_proc
};

typedef struct {
    unsigned int ver; // changes whenever anything below changes
    unsigned char gender;
    unsigned char face;
    unsigned char sunburn;
    unsigned char swell;
    unsigned char decoy;
    unsigned char umbrella; // design 0..39 of the equipped umbrella, 0xFF for none
    unsigned short cloth_idx;
    unsigned char cloth_pal_idx;
    unsigned char umb_pal_idx;
    unsigned char name[MP_NAME_LEN];
    unsigned char town[MP_NAME_LEN];
    unsigned char cloth_tex[0x200];
    unsigned char umb_tex[0x200];
} mp_look_t;

// a sound or effect, placed from the player that made it
typedef struct {
    unsigned char kind;
    unsigned char prio;
    unsigned short id;
    unsigned short item;
    short angle;
    short arg0;
    short arg1;
    short dx;
    short dy;
    short dz;
} mp_fx_t;

#define MP_PF_REPEAT0     0x0001
#define MP_PF_REPEAT1     0x0002
#define MP_PF_HIDDEN      0x0004
#define MP_PF_ITEM_REPEAT 0x0008
#define MP_PF_ROOT        0x0010 // the animation moves the body: its root is pinned as the sender's is
#define MP_PF_UMB         0x0020 // an umbrella opening, open or closing
#define MP_PF_NET         0x0040 // the net's head swings
#define MP_PF_HOLD        0x0080 // an item growing into or out of the hand (pickups, digging)
#define MP_PF_STAR        0x0100 // Super Star colors
#define MP_PF_WASH        0x0200 // the car-wash sponge in hand
#define MP_PF_ROD         0x0400 // the fishing rod tilted
#define MP_PF_ATTACH      0x0800 // things this screen draws for the player: float, hooked fish, caught bug, gift
#define MP_PF_BALLOON     0x1000 // a balloon held, swaying
#define MP_PF_SNOW        0x2000 // snowballs this player rolls; the others follow them
#define MP_PF_BALL        0x4000 // the town's ball, which this player's game kicked last
#define MP_PF_STILL       0x8000 // a menu has this player's world stopped; the others take over what it moved

// a snowball as the game rolling it has it (the town has two: part 0 and 1)
typedef struct {
    unsigned char part;
    unsigned char proc;
    unsigned short flags;
    float pos[3];
    float y_ofs;
    float ofs_y; // the head's jump onto the body
    float move_dist;
    float body_scale;
    short head_vec[3];
    short result;
    float fg_x;
    float fg_z;
} mp_snow_t;
#define MP_SNOW_MAX 2

// the town's ball as the game that kicked it last has it
typedef struct {
    float pos[3];
    float speed;
    float vel_y;
    float ball_y;        // sunk into a hole
    short angle_y;       // heading
    unsigned char mode;  // on the ground, in the air, afloat, in shallows
    unsigned char flags; // in a hole, gone under
    unsigned char type;
    unsigned char snd;   // counts its thuds and bounces; the newest one's sound plays on the others
    unsigned short snd_id;
    float snd_speed;       // 0 for a plain sound
    unsigned int kick_ms; // shared clock at the kick that made that game its mover
} mp_ball_t;

// what the local player's screen draws for it apart from its body, redrawn at its puppet from the same matrix
enum {
    MP_ATT_UKI,    // the fishing float; arg: 1 for the golden rod's; rgba: its nibble tint
    MP_ATT_FISH,   // the fish on the line; arg: fish type, arg2: frame
    MP_ATT_INSECT, // the bug in the net; arg: insect type, arg2: draw type, rgba[3]: alpha
    MP_ATT_HOI,    // an item handed over in a talk; arg: fish or item draw type, arg2: 1 for a fish
    MP_ATT_BEE,    // the bees chasing the player; arg: pose frame, rgba[3]: alpha
    MP_ATT_KINDS,
};
#define MP_ATT_MAX 3

typedef struct {
    unsigned char kind;
    unsigned char arg;
    unsigned char arg2;
    unsigned char rgba[4];
    short rot[3];
    short dx; // from the player
    short dy;
    short dz;
    float scale[3];
} mp_att_t;

#define MP_LVL_MAX 2
#define MP_GOKI_MAX 6 // a room's cockroaches out at once (aMR_GOKI_MAX), and as many dying

// a house cockroach as the game running the room's has it
typedef struct {
    unsigned char id;
    unsigned char act;
    unsigned char anm; // in halves
    unsigned char alpha;
    short x; // in quarters
    short y;
    short z;
    short angle;
} mp_goki_t;

typedef struct {
    unsigned char seq;
    unsigned short flags;
    unsigned short scene;
    unsigned short field;
    unsigned short owner;
    float x;
    float y;
    float z;
    short rot_x; // leaning, lying in bed
    short rot_y;
    short rot_z;
    short roll; // rocking in Kapp'n's boat
    short head_x;
    short head_y;
    unsigned char anim0;
    unsigned char anim1;
    signed char part_table;
    unsigned char main_index;
    float frame0;
    float frame1;
    float speed0;
    float speed1;
    unsigned char eye;
    unsigned char mouth;
    unsigned char item_main;
    signed char item_shape;
    signed char item_anim;
    float item_frame;
    float item_speed;
    float item_scale;
    unsigned char shadow; // drawn (0x80) and how big and solid, of 127
    // MP_PF_ROOT
    unsigned char root_flags;
    short root_trans[3];
    short root_rot_x;
    short root_rot_y;
    short root_rot_z;
    // MP_PF_UMB
    unsigned char umb_action;
    float umb_frame;
    unsigned char umb_idx; // the umbrella in hand (the one being put away as the next is picked)
    // MP_PF_NET
    short net_angle[3];
    // MP_PF_HOLD
    unsigned short hold_item;
    short hold_angle; // a thing off a table: the way it faced there
    unsigned char hold_jump;
    float hold_scale;
    short hold_dx;
    short hold_dy;
    short hold_dz;
    // MP_PF_ROD
    short rod_angle_z;
    // MP_PF_ATTACH
    unsigned char natt;
    mp_att_t att[MP_ATT_MAX];
    // MP_PF_BALLOON
    short balloon_x;
    short balloon_z;
    // MP_PF_SNOW
    unsigned char nsnow;
    mp_snow_t snow[MP_SNOW_MAX];
    // MP_PF_BALL
    mp_ball_t ball;
    // the newest sounds and effects; fx[0] is number fx_seq
    unsigned short fx_seq;
    unsigned char nfx;
    mp_fx_t fx[MP_FX_MAX];
    // sounds this player's doings keep up this frame
    unsigned char nlvl;
    unsigned char lvl[MP_LVL_MAX];
    short lvl_d[MP_LVL_MAX][3];
    // the room's cockroaches, when this player's game runs them
    unsigned char goki_run;
    unsigned char ngoki;
    mp_goki_t goki[MP_GOKI_MAX];
} mp_pstate_t;

const mp_look_t* mp_player_look(int slot);
const mp_pstate_t* mp_player_state(int slot, unsigned int* age_ms);
int mp_player_take_fx(int slot, mp_fx_t* fx); // the puppet: next sound or effect to replay, FALSE when none
void mp_player_puppet_gone(int slot, void* actor);
void mp_player_on_rel(int conn, const unsigned char* data, int len);
void mp_player_on_state(int conn, const unsigned char* data, int len);
void mp_player_on_arrived(int conn); // host: a guest stepped off the train
void mp_player_on_back(int conn);    // a quiet peer is back: looks go out again
void mp_player_on_gone(int slot);     // host: a guest left the town
void mp_player_reset(void);
void* mp_player_puppet(int slot);     // that player's puppet on this screen, or NULL
void mp_player_relay(int from_slot, const unsigned char* data, int len); // host: to every other guest in town

// chat (pc_mp_chat.c): balloons over heads, typing dots, a line at the top for players off this screen
#define MP_CHAT_MAX     32  // characters in a message
#define MP_CHAT_COL_MAX 182 // the widest a message may be, in unscaled font pixels (the keyboard's line)
void mp_text_clean(unsigned char* s, int len); // control codes, tags and line breaks become spaces
int mp_chat_fit(const unsigned char* s, int len);
int mp_chat_ng_word(const unsigned char* s, int len);
void mp_chat_say(const unsigned char* text, int len); // a keyboard's text: checked, sent and shown
void mp_chat_on_rel(int slot, const unsigned char* data, int len);
void mp_chat_reset(void);
void mp_chat_gone(int slot);
struct game_play_s;
void pc_mp_chat_move(struct game_play_s* play);
void pc_mp_chat_draw(struct game_play_s* play); // m_play.c, before the talk windows
int pc_mp_chat_self_showing(void);              // m_watch_my_step.c: the item-name balloon waits
// the game keyboard's field (m_ledit_ovl.c): a balloon centred at MP_CHAT_FIELD_Y, text from MP_CHAT_FIELD_TEXT_X
#define MP_CHAT_FIELD_Y      70.0f
#define MP_CHAT_FIELD_W      186.0f
#define MP_CHAT_FIELD_TEXT_X (160.0f - MP_CHAT_FIELD_W * 0.5f + 13.0f)
struct graph_s;
void pc_mp_chat_field_art(struct graph_s* g, float pos_x, float pos_y);
int pc_mp_chat_typing(void);                    // a chat keyboard is up
int pc_mp_chat_cancel_take(void);               // m_editor_ovl.c: the button closed the chat keyboard (once)
void pc_mp_chat_draw_over(struct game_play_s* play); // m_play.c, after the menus: the balloon button
void pc_mp_chat_tap(int x, int y);              // vita_platform.c: a new touch, in UI coordinates
enum { MP_IME_TYPING, MP_IME_ENTER, MP_IME_CLOSED }; // what the Vita keyboard reports
int mp_chat_ime_text(const unsigned short* text, int n, int what); // FALSE: Send refused, keep typing
void mp_light_switched(int idx, int on); // m_kankyo.c: this player pressed Z for the room's light
void mp_light_boat(void);                // ac_boat_demo: the cottage's light as the boat reaches the island
struct game_play_s;
void mp_player_frame(struct game_play_s* play); // start of a play frame, menus included
int mp_world_still(void);                        // a menu has this game's world stopped
int mp_player_still(int slot);                   // that player's world is stopped, or it has gone quiet
int pc_mp_host_runs_on(void);  // host with visitors in town: its town keeps running behind its menus
int pc_mp_runs_on(void);       // this game's menus stop only its own player: the host, or a visitor running things for all
int mp_npc_runs_events(void);  // visitor: it runs one of the host's event characters for everyone
int mp_npc_walker(void);       // the screen stepping the villagers nobody watches (the host's while it's out in town)
int mp_npc_mail_here(void);    // Pete comes by for this game's player: the host, or a visitor at the houses
// a visitor's Pete: the host's own post office does what he did there, so only the host's game writes it
enum {
    MP_MAIL_CAME,  // he set out: the next round moves on
    MP_MAIL_HOUSE, // he left one house's mail
    MP_MAIL_ALL,   // he left the rest as the scene ended
    MP_MAIL_FORCE, // someone found the desk full: he comes early (host to the game bringing him, too)
};
void pc_mp_mail_proc(struct game_play_s* play); // m_play.c: in place of mPO_business_proc
void pc_mp_mail_told(int what, int house);
void mPO_mp_post_man_came(void); // m_post_office.c: the next round, as when this game brings him
struct mail_s;
struct private_s;
int pc_mp_post_letter(void* mail, int dest); // Pelly's answer to a visitor's letter is the host's; -1 until it comes
int pc_mp_letter_taken(const void* mail);    // the letter the host's post office just took in
int mPO_mp_receipt_traveller(struct mail_s* mail, struct private_s* traveller); // m_post_office.c
int aNPC_mp_puppet_frame(void* actorx, void* game); // ac_npc.c: one with a move of its own follows its copy elsewhere
int pc_mp_menu_running(void);  // what goes on behind this game's menu is running right now
void pc_mp_menu_run(int on);
int pc_mp_menu_runs(void* actor); // one of the characters that goes on behind this game's menu
void mp_npc_on_evarea(int conn, const unsigned char* data, int len);
// the host's events run by a group of characters (a race, a tug of war, the shrine line): one screen runs
// each for everyone and the others follow, a visitor keeping its own copy of the event's record
unsigned char* pc_mp_ev_record(int type, int id); // m_event.c: a visitor's own copy of that record (NULL: the save's)
int pc_mp_ev_follows(int type);        // another screen runs it
int pc_mp_ev_npc0_gone(int type);      // its first character left this screen; TRUE: leave the record be
int pc_mp_ev_torn(int type);           // -1: no shared town (the game's own check); else this screen's copies go
void pc_mp_ev_begun(int type);         // its first character came out on this screen
unsigned char* mEv_mp_real_area(int type, int id); // m_event.c: the save's own record
unsigned char* mEv_mp_stand_in(int type, int n, unsigned char* id); // m_event.c: a visitor's n-th record of its own
void aTKN0_mp_torn(void);              // each event's own ending of its record, for the host
void aTNN0_mp_torn(void);
void aHN0_mp_torn(void);

// the step an event character's (or control's) logic is on and what that step keeps, as the screen running
// the event has it: another screen's copy talks as it would, and carries on from there on taking it over
typedef struct {
    unsigned char think; // 0xFF: not out on the runner's screen
    unsigned char a;
    unsigned char b;
    unsigned char c;
    short timer;
    short p0;
    short p1;
    short t2;
    float speed;
} mp_evstep_t;

#define MP_EVS_GET  0
#define MP_EVS_SHOW 1 // following: what a talk here would say and lead to
#define MP_EVS_TAKE 2 // taken over: carry on as the runner's copy was
#define MP_EVS_HUSH 3 // following while a player's own turn is under way there: no talks here

#define MP_EVC_RUN    0 // pc_mp_ev_ctl: run the event here
#define MP_EVC_FOLLOW 1 // another screen runs it and keeps its record
#define MP_EVC_TAKE   2 // this screen just took it over: carry on from the step given

int pc_mp_ev_ctl(int type, mp_evstep_t* st);   // an event's control, each frame
int pc_mp_ev_talk_bits(int type);              // its characters another player is talking to (bit per character)
void mp_npc_resumed(void* actor, void* game); // a shared character this game moves again
void aTKN0_mp_step(void* actor, void* play, mp_evstep_t* st, int mode);
void aTKN1_mp_step(void* actor, void* play, mp_evstep_t* st, int mode);
void aTNN0_mp_step(void* actor, void* play, mp_evstep_t* st, int mode);
void aTNN1_mp_step(void* actor, void* play, mp_evstep_t* st, int mode);
void aHN0_mp_step(void* actor, void* play, mp_evstep_t* st, int mode);
void aTKC_mp_step(void* actor, mp_evstep_t* st, int mode);
void aES2_mp_step(void* actor, void* play, mp_evstep_t* st, int mode); // single event characters too
void aNTT_mp_step(void* actor, void* play, mp_evstep_t* st, int mode);
void pc_mp_menu_pre(struct game_play_s* play);
void pc_mp_menu_post(struct game_play_s* play);
void mp_npc_menu_post(struct game_play_s* play);

// the shared town (pc_mp_world.c)
struct game_play_s;
void mp_world_frame(struct game_play_s* play);
void mp_world_post(struct game_play_s* play);
void mp_world_menu_post(struct game_play_s* play); // visitor running on behind its menu: the town's cells only
int mFI_SetFGStructure_cells(unsigned short* fg_items, unsigned short structure_name, int bx, int bz, int ut_x,
                             int ut_z); // m_field_info.c: a structure written into a copy of a block's cells
int mp_town_block(const void* pos, int* bx, int* bz); // the acre of a spot outdoors, whatever scene is loaded
void mp_world_on_rel(int conn, const unsigned char* data, int len);
void mp_world_on_arrived(int conn);
void mp_world_on_back(int conn); // host: a quiet guest is back
void mp_world_on_guest_gone(int slot, int conn);
void mp_world_flush_out(void);   // queued town traffic into the link: the host hanging up, a visitor heading home
void mp_world_on_save_capture(unsigned int serial); // a town save took its snapshot
void mp_world_host_begin(void);
void mp_world_snap_taken(int conn, const void* raw); // host: a joiner's snapshot, to catch it up on arrival
const unsigned char* mp_world_heard(unsigned int save_off, unsigned int len); // guest: host bytes as last heard
void mp_world_heard_mine(unsigned int save_off, unsigned int len); // guest: our write, already told to the host
void mp_world_host_took(unsigned int save_off, unsigned int len, int from_slot); // host: a visitor's write goes out now
void mp_world_queue_guests(const unsigned char* msg, int len, int except_slot); // host: behind the town traffic
void mp_world_host_took_diff(unsigned int save_off, const void* before, unsigned int size, int from_slot); // ...all of it
void mp_world_ask_commit(void); // host: a visitor handed the town something; save it soon
void mp_world_guest_arrived(void);
void mp_world_end(void);
int mp_world_settled(void);
// what the game moves out of the way to the lost & found: a visitor leaves the cells to the host (TRUE: leave them)
enum { MP_KEEP_NONE, MP_KEEP_UNIT, MP_KEEP_FLAT, MP_KEEP_BLOCK };
int mp_town_keep_at(const void* wpos, int kind);
int mp_town_keep_block(int bx, int bz);
int mp_world_passport_hold(void);
// a one-of-a-kind thing for sale (Nook's shelves, Redd's tent): one player at a time buys it, and once
int mp_shop_claim(unsigned short item);
int mp_shop_claim_state(unsigned short item); // 0 waiting on the host, 1 this player's to buy, 2 another's
void mp_shop_claim_done(unsigned short item); // bought
void mp_raffle_won(int slot, unsigned short item); // Nook's raffle gave that prize here: every shop floor loses it
void mp_island_ftr(int set, const void* pid, unsigned short ftr); // m_npc.c: a visitor's furniture to or from the islander
void mp_island_check(const void* pid, unsigned int check); // m_island.c: what the islander saw a visitor do there
void mp_island_named(void); // ac_npc_sendo: a visitor named the island on the town's first trip out
void mp_world_eaten(unsigned short item); // a pocket's fruit or fish eaten: gone for good, set down nowhere // guest: a pickup not yet saved by the host was used up; the passport waits
int mp_passport_last_priv(void* priv); // the traveller as the newest passport written has them
void mp_world_hurry(void); // guest at Porter: ask the host for its next save now
void mp_world_recall_held(int from_pp); // guest leaving: drops still waiting on the passport are undone
unsigned int mp_world_pp_mark(void);    // guest: the host's newest save heard (bit 16: any), for a passport staged now
unsigned int mp_passport_mark(void);    // pc_mp_journal.c: ...as the newest passport on the card has it
unsigned int mp_passport_mark_staged(void); // pc_mp_journal.c: ...as the newest one staged has it
unsigned int mp_passport_staged_seq(void);  // pc_mp_journal.c: the newest passport staged so far
unsigned int mp_passport_good_seq(void);    // pc_mp_journal.c: ...and the newest on the card
void mp_npc_drain_all(void);     // pc_mp_npc.c: leaving: results the card covers go out at once
int mp_npc_undo(void* priv, unsigned int card_seq);  // pc_mp_npc.c: ...those that never went give back what their
                                                     // talks handed over (card_seq: priv is that passport)
int mp_npc_talk_full(void);      // pc_mp_npc.c: no room to hold another talk's result: a next talk waits
int mp_npc_result_pending(void); // pc_mp_npc.c: a talk's result still waits on the passport, or is going out
int mp_world_pp_frozen(void);    // pc_mp_world.c: no passport may be staged now
int mp_world_hold_told(void);    // pc_mp_world.c: the host holds this visitor's pickups for a passport standing still
int mp_world_exit(void);          // pc_mp_world.c: the app closing mid-visit (FALSE: no host's town here)
int mp_world_line_ok(void);       // pc_mp_world.c: guest: the host was heard from a moment ago
// pc_mp_world.c, guest talks: the pockets as one began, for passports staged meanwhile, and what finished ones changed
// till their results go
void mp_world_talk_begin(void);
void mp_world_talk_end(unsigned short seq, unsigned int floor, int changed);
void mp_world_talk_gone(unsigned short seq);  // its result went into the line
void mp_world_talk_drop(unsigned short seq);  // its result can't be held: its hand-overs come back now
int mp_world_talk_undo(void* priv, unsigned int card_seq);
void mp_world_flush_all(void); // guest: queued messages into the line, a few moments for a full one
void mp_world_talk_stage(void* priv);
void mp_world_talk_reset(void);
void mp_world_inv_mark(void);   // a change to the pockets that isn't a talk's is coming...
void mp_world_inv_follow(void); // ...and made
int mp_npc_talk_extra(const unsigned char* msg, int len); // pc_mp_npc.c: a talk's hand-over, sent with its result
void mp_world_keep(unsigned short item); // host: an item for Copper's lost and found
#define MP_DESIGN_FLAG 8 // after the Able Sisters' eight stands
void mp_world_design(int target);   // a stand's design traded, or the island's flag set: the host's copy takes it
// the island's cottage: everyone in it rearranges it at once, each room following the cells the host keeps
void mp_cot_room_enter(void); // ac_my_room.c: its room came up here
void mp_cot_room_leave(void);
int mp_cot_owner(void);       // this player may rearrange it (the host, or a visitor the host lets)
int mp_cot_shared(void);      // a session shares it: its rooms follow the cells
const unsigned short* mp_cot_truth(const unsigned char** busy); // its item layers as the host has them, and the
                                                                // cells the room here is to leave be
int mp_cot_in_use(int ut);    // another player has the furniture on that unit in hand
int mp_cot_unit_taken(int ut); // another player stands on that unit
int aMR_mp_music_slot(unsigned int save_off); // ac_my_room.c: a stereo's record cell, not an item
void aMR_pc_stored_cells(void (*cb)(unsigned int save_off, unsigned short name, void* arg), void* arg);
void aMR_pc_switch_tables(void (*cb)(unsigned int save_off, unsigned long long bits, void* arg), void* arg);
unsigned int aMR_mp_house_floor_off(void); // ac_my_room.c: the house floor this room is (0: none)
const unsigned short* mp_house_truth(void); // guest: its house room's furniture layers as the host has them, settled
// ac_my_room.c: what the player is in the middle of (cells others keep off, those of them whose changes wait, units)
void aMR_mp_locks(unsigned char* cells, unsigned char* hold, unsigned char* units);
int aMR_mp_meta(unsigned char* out, int max); // ac_my_room.c: its lamps, gyroids' steps and tempo
int aMR_mp_holds_cell(int layer, int ut);     // ac_my_room.c: that cell's thing is kept in furniture here
void aMR_mp_reconcile(void);  // ac_my_room.c: the room takes on what the others did
int mp_cot_take_resync(void); // an op of the cottage's was undone here since the last look
int mp_cot_must_yield(int ut); // a player this one gives way to has that unit's furniture in hand
void aWeather_mp_headless_roll(void); // ac_weather.c: the host in an NES game still rolls the day's weather
int aMI_mp_reserved(void);    // ac_my_indoor.c: a carpet or wallpaper chosen and not laid yet
// an NES game on this Vita ends the play game until it's over (famicom_emu.c); the host keeps serving its town
void pc_mp_emu_enter(void);
void pc_mp_emu_leave(void);
int mp_emu_active(void);
void pc_mp_play_gone(int to_nes); // m_play.c play_cleanup: its actors are gone (for an NES game, or a new scene)
void mp_host_headless_tick(void);
struct game_play_s* mp_live_play(void); // the running play game, or NULL (between scenes, the NES game)
void mp_world_play_gone(void);
void mp_world_drain(void);   // messages in without a play frame (an NES game, the app closing)
int mp_ev_placed_away(int type); // host: an event a visitor put up while it played an NES game
void mp_npc_play_gone(void);
void mp_cr_play_gone(void);
void mp_npc_headless(void);
void mp_travel_headless(void);
void aMI_mp_refresh(void);   // ac_my_indoor.c: the room's carpet and wallpaper as the save has them
void aNI_mp_refresh(int slot);      // ac_needlework_indoor.c: a stand shows the town's copy again
void aNNW_mp_trend_delete(int slot); // ac_npc_needlework_talk.c_inc: villagers drop the design a stand lost
void mp_npc_stand_replaced(int slot); // host: a stand's design changed; a villager report still wearing it is stale
void mp_world_board_post(const void* post, int len); // m_notice.c: a player posted on the board
void mp_world_lighthouse(int day); // m_soncho.c: a visitor lit the lighthouse tonight
int mp_world_commit_mark(unsigned short* op); // guest boarding home: newest commit heard, if any
void mp_world_passport_filter(void* priv);
void mp_world_actor_cells(struct game_play_s* play, void (*cb)(unsigned int save_off, unsigned short name, void* arg),
                          void* arg);
int mp_town_writer_allowed(void);          // FALSE on a visitor: the host grows, clears and buries
int mp_host_block_watched(int bx, int bz);  // a visitor is near this block
int mp_block_occupied(int bx, int bz);      // another player stands in this very block

// villagers in a shared town (pc_mp_npc.c)
int mp_npc_talk_locked(unsigned short npc_id);              // someone else is talking to this villager
int mp_npc_defers(unsigned int save_off, unsigned int len); // guest: host bytes held back for our talk
void mp_npc_animals_changed(void);
void mp_npc_frame(struct game_play_s* play);
void mp_npc_on_rel(int conn, const unsigned char* data, int len);
void mp_npc_on_arrived(int conn); // also when a quiet guest is back
void mp_npc_on_guest_gone(int slot);
void mp_npc_still(struct game_play_s* play); // a frame a menu has this game's world stopped
void mp_npc_standin(void* actor); // a hidden copy standing in for another player's character (the fairy)
void mp_npc_reset(void);

// a villager as the game moving it shows it; the others play it back
typedef struct {
    float x;
    float y;
    float z;
    short rot_y;
    short head_x;
    short head_y;
    unsigned char anim;
    signed char sub_anim;
    unsigned char talk;
    unsigned char hidden;
    unsigned char kutipaku; // talking right now, to a player or in a greeting: the mouth moves
    unsigned char mouth;
    unsigned char umb;      // an umbrella in hand
    unsigned char prop;     // or a prop an event handed it (a fan, a popper, a rod, a flag)
    unsigned char tool;     // which one: its tool number
    unsigned char org_idx;  // a custom design worn
    unsigned short cloth;   // the shirt on right now
    unsigned char tool_act; // what the prop in hand is doing (a fan, a popper, a flag), 0xFF for none
    unsigned char lsnd;     // a sound it keeps making (clapping), 0 for none
    unsigned char fresh;    // this report just came in
    float frame;
    float speed;
    float vx; // its walk, per frame: followers carry it on between reports
    float vy;
    float vz;
} mp_npose_t;

enum {
    MP_NPC_LOCAL,  // this game moves it
    MP_NPC_PUPPET, // another game does; pose is filled in
    MP_NPC_RESUME, // this game moves it again after following another
};

int mp_npc_control(void* actor, mp_npose_t* pose);
// the Porter: each game runs its own, and while one screen's player arrives, talks to him or boards, the others show
// that screen's (each frame: busy before he moves, what he did after)
void mp_porter_busy(int busy);
void mp_porter_moved(void* actor);
void mp_porter_reset(void);
void mp_porter_on_state(int conn, const unsigned char* data, int len);
int mp_door_take(void* structure, int* coming_out); // a building: another player just used this door
int mp_npc_is_puppet(void* actor);
unsigned short mp_player_place(void); // whose house this player stands in (0 outdoors)
void mp_npc_claim(void* actor);     // the local player did something only this game's copy can answer
int mp_puppet_catching(void* actor); // a puppet's player is showing off a catch (villagers clap)
void mp_npc_post(struct game_play_s* play);
void mp_npc_on_state(int conn, const unsigned char* data, int len);
// around a character's own logic: what it makes shows where other games follow it (NULL ends)
void pc_mp_npc_fx(void* actor);
int mp_npc_fx_effect(int id, const void* pos, int prio, short angle, unsigned short item, short arg0, short arg1);
int mp_npc_fx_sound(unsigned short id, const void* pos);
int mp_npc_fx_kill(int id, unsigned short item);
void pc_mp_npc_level_sound(unsigned char id); // sAdo_OngenPos: a sound kept up every frame
void pc_mp_cr_level_sound(unsigned int key, unsigned char id); // ...and the creature it's kept up for
unsigned char pc_mp_cr_level_of(const void* actor); // that creature's this frame, taken
void pc_mp_fx_level(unsigned char id, const void* pos); // ...and one this player's own doings keep up
void pc_mp_npc_sys_sound(unsigned short id); // sAdo_SysTrgStart: in a shared character's own logic
void pc_mp_shown_cloth(unsigned short idx); // m_player_lib.c: the cloth now worn, a try-on's included
void aMR_mp_draw_leaf(void* game, unsigned short item, const void* pos, float scale); // ac_my_room.c
void sAdo_mp_walk_as(unsigned char dash, unsigned short walk, const void* pos, int room); // audio.c: another's step
struct game_play_s;
void mp_npc_fx_replay(struct game_play_s* play, const unsigned char* body, int len);
void aNHM_mp_replay(struct game_play_s* play, const unsigned char* body, int len); // ac_npc_hem.c

// the town's clock to the millisecond, the same on every game while the town is shared (pc_mp_travel.c)
int mp_shared_clock_ms(unsigned int* ms);
unsigned int pc_mp_world_frame(unsigned int local); // the shared clock in frames while others are here, else local
void pc_mp_kf_phase(void* frame_control); // a looping animation's place taken from the shared clock
unsigned int mp_shared_hash(unsigned int seed, unsigned int a, unsigned int b); // shows every screen draws alike

// bugs, fish shadows and the present balloon (pc_mp_critter.c): the host rolls them, the nearest
// player's game runs each one, the others follow it
enum {
    MP_CR_INSECT,
    MP_CR_FISH,
};
#define MP_CR_BLOB 24 // each kind's own state beyond its position
struct game_play_s;
void pc_mp_cr_roll(int begin, int bx, int bz); // around the set manager's rolls for an acre
int mp_cr_remote_roll(void);                    // host: this roll is another player's; tell, don't make
int mp_cr_guest_step(int bx, int bz, int kind); // guest: the host rolls this step; TRUE: don't roll here
int mp_cr_host_step(struct game_play_s* play, int bx, int bz, int kind); // host: already out; made from the list
void pc_mp_cr_remote_spawn(int kind, int type, const void* pos, int extra);
void pc_mp_cr_made(int kind, int type, const void* pos, int extra, void* actor); // a copy was made here
void pc_mp_cr_adopt(int kind, int type, const void* pos, int extra, void* actor); // host: one not listed yet
int mp_cr_adopting(void);                   // host: this frame looks for creatures to list
int mp_cr_binding_block(int* bx, int* bz);  // the acre of the creature a copy is being made for
void pc_mp_cr_gone(void* actor);   // caught, landed or gone for good: every screen drops it
void pc_mp_cr_bolted(void* actor, int start); // a fish darted off: every screen sees its shadow go
void pc_mp_cr_ant_born(void* actor, int bx, int bz); // the ants listed for that acre came out here
void pc_mp_cr_ant_gone(void* actor);          // their candy went: the ants go on every screen
void pc_mp_cr_netted(void* actor);  // this player's net closed on one
void mPlib_mp_forget_catch(unsigned int label); // m_player_lib.c: a creature gone here leaves no net request behind
int mPlib_mp_net_catch_type(void);     // 0: a bug's slot in the net, else an ant or bee actor
int mp_cr_balloon_last(void* pos, void* blob); // the runner's balloon as last heard, for taking it over
void bIT_mp_menu_move(void);           // bg_item.c: falling items land behind the host's menu
void aWeather_mp_menu_roll(void);      // ac_weather.c: the host rolls the day's weather behind its menus too
int pc_mp_cr_net_waiting(void);     // ...and the host hasn't said yet whose it is
void pc_mp_cr_menu_frame(void);     // host: its balloon flew on behind its menu this frame
int mp_cr_balloon_runner_here(void); // this game flies the town's present balloon
int mp_cr_balloon_flying_here(void); // visitor: ...and it's in the air here, so its menus don't stop it
void pc_mp_cr_balloon_gone(void);    // the balloon this game flies flew off for good
int pc_mp_cr_ended(void* actor);   // a copy ended on its own; TRUE when the creature is still out elsewhere
int pc_mp_cr_refused(void* actor); // a copy's own setup threw it out; TRUE when it's still out elsewhere
void pc_mp_cr_unbind(void* actor); // this game let its copy go
void pc_mp_cr_unbind_kind(int kind); // the scene's control actor went, and every copy of that kind with it
int pc_mp_cr_listed(void* actor);    // another game may still need this creature
int pc_mp_cr_caught_elsewhere(void); // the copy going now was caught on another screen
int mp_cr_puppet(void* actor, void* pos, void* blob, unsigned int* age_ms); // another game runs it
int mp_cr_mine(void* actor);                                                // this game runs it for all
void pc_mp_cr_report(void* actor, const void* pos, const void* blob, unsigned int key);
void pc_mp_cr_claim_near(int kind, const void* pos, float radius);
void pc_mp_cr_scare_near(const void* pos, float radius); // a float landed on fish another game runs
void pc_mp_cr_scare_at(const void* pos, float radius);   // a ball or a thrashing fish in the water here
int mp_cr_dash_near(const void* pos, float dist);        // another player runs this close
void pc_mp_cr_act(int action, int ut_x, int ut_z);       // the local player shook, dug or hit something
int mp_cr_take_act(int* action, int* ut_x, int* ut_z);   // another player's, for the bugs this game runs
void aINS_mp_spirit_taken(int bx, int bz); // host: a spirit is out in that acre (ac_insect_clip.c_inc)
void aINS_mp_spirit_away(int bx, int bz);  // host: one went unseen and turns up in another acre
void aGYO_mp_scare(void* actor);           // this fish bolts (ac_gyoei_clip.c_inc)
void aGYO_mp_bolt_copy(void* actor, void* game, int start); // its runner's fish darted off
void aGYO_mp_bolt(void* ctrl, int start);  // this game's fish darts off (a start: it made a ripple)
void aINS_mp_lose_catch(void* netted, int ant); // another net got it first (ac_insect_clip.c_inc)
void aANT_mp_vanish(void* actor);          // caught or gone on another screen (ac_ant.c)
void pc_mp_cr_balloon_report(const void* pos, const void* blob, unsigned int key); // host
int mp_cr_balloon(void* pos, void* blob, unsigned int* age_ms);                     // guest
int mp_cr_balloon_elsewhere(void); // another game flies the town's balloon; this one follows it
void pc_mp_cr_balloon_pop(void);
int mp_cr_balloon_popped(void);
int mp_cr_near_player(const void* pos, float dist); // the balloon's runner: another player stands this close
void mp_cr_post(struct game_play_s* play);
void mp_cr_still(struct game_play_s* play); // a frame a menu has this game's world stopped
void mp_cr_on_state(int conn, const unsigned char* data, int len);
void mp_cr_on_rel(int conn, const unsigned char* data, int len);
void mp_cr_on_guest_gone(int slot);
void mp_cr_reset(void);

// visitors doing what residents do (pc_mp_rights.c), when the host lets them
int mp_visitor_rights(void);
// what a visitor did with a character in this town today, kept in the passport so a return trip remembers it
enum {
    MP_MARK_KK,      // K.K.'s greeting and aircheck, his own flags
    MP_MARK_KABU,    // Joan greeted it
    MP_MARK_TOURNEY, // Chip: below
    MP_MARK_WISP,    // met Wisp tonight, his own flag
    MP_MARK_NUM,
};
#define MP_MARK_TOURNEY_ENTERED 0x01
#define MP_MARK_TOURNEY_PRIZE   0x02
int mp_mark_get(int kind);
void mp_mark_set(int kind, int flags);
void mp_mark_clr(int kind, int flags);
int mp_rights_wisp_met(int slot); // host: that player met Wisp tonight
int mp_rights_wisp_hunters(void); // host: how many players in town have
void mp_rights_slot_fresh(int slot); // host: a visitor new to that slot
void aEGH_mp_record(void);        // ac_ev_ghost.c: host: Wisp's record, for a Wisp only a visitor has out
void pc_mp_text_patch(int idx, unsigned char* data, int size); // m_msg_main.c_inc: ROM lines a visitor sees otherwise
int pc_mp_prize_mail(void* mail, int mark, int flags); // visitor: a resident's prize letter, into its pouch
void mFR_mp_visitor_prize(void); // m_fishrecord.c: a visitor holding the tourney's final record finds its prize
void mp_npc_talk_opening(void);  // m_demo.c: a talk just chose its character, before its set-up callback
void aEANG_mp_record(int x, int z); // ac_ev_angler_move.c_inc: host: Chip's record, for a Chip only a visitor has
void mp_rights_departing(void); // network departure, the home town still loaded
int mp_rights_home_bank(void);  // visitor: their own house opens savings back home
int mp_rights_same_day(void);   // visitor: the host's town is on the traveller's own day
int pc_mp_museum_donor(int cat, int idx, unsigned char* name); // a visitor's donation: TRUE, with their name
int pc_mp_museum_mine(int cat, int idx);                        // ...and it was this player's
void mp_rights_museum_run(int from, unsigned int off, const unsigned char* bytes, unsigned int n);

// today's events (pc_mp_event.c)
int mp_event_may_start(int type); // guest: the host has started or placed this event
void mp_event_roster_ask(int type); // ac_set_npc_manager.c: a visitor's event waits for the host's villagers
// m_event.c: this game reserved or cleared an event's place (a visitor placing for the host offers its own)
void pc_mp_ev_place_made(int type, int id, int slot);
void pc_mp_ev_place_gone(int type, int id);
void pc_mp_ev_moved(int type, int id, const void* data); // ac_event_manager.c: a character walked on after a talk
void mp_event_tick(struct game_play_s* play);
void mp_event_on_rel(int conn, const unsigned char* data, int len);
// aerobics: the radio song plays in step on every screen that has it on, and the dance with it
void mp_aero_frame(void);
int pc_mp_aero_skip(int seq_id);                      // the song starts here: updates to skip to join the others'
int pc_mp_aero_shared(void);                          // the dancers go by the song's shared seeds
unsigned int pc_mp_aero_seed(int measure, int salt);  // alike on every screen for this loop and measure
// jaudio radio.c: the song as it plays here (updates since it began, where its latest time round began,
// times round, and updates a time round once known); FALSE while it isn't on or is still catching up
int Na_MpAeroState(int* updates, int* loop_sc, int* loops, int* period, int* starts);
void mp_event_on_arrived(int conn); // also when a quiet guest is back
void mp_event_reset(void);
void mEv_mp_areas_reset(void); // m_event.c: a new visit drops the last one's stand-in save areas

// sounds and effects the local player makes while it updates or draws are replayed at its puppets
void pc_mp_fx_capture(int on);
void pc_mp_fx_hush(int on); // inside a captured call: what it makes comes along with it
void pc_mp_fx_sound(int kind, unsigned short id, const void* pos);
void pc_mp_fx_effect(int id, const void* pos, int prio, short angle, unsigned short item, short arg0, short arg1);
void pc_mp_fx_kill(int id, unsigned short item);
void pc_mp_attach(int kind, int arg, int arg2, const unsigned char* rgba, const void* mtx); // drawn now with mtx

// town visuals the local player's actions set off, shown on the other screens without touching the town
int mp_vfx_recording(void);
void mp_vfx_send(const unsigned char* body, int len);
enum { MP_KK_SHOW, MP_KK_SONG_ON, MP_KK_WEATHER, MP_KK_SONG_OFF }; // MP_VFX_KK's steps
enum {
    MP_VFX_DROP,     // an item flying or dropping to the ground
    MP_VFX_HOLE,     // a hole dug or filled
    MP_VFX_PIT,      // a pitfall opening or closing
    MP_VFX_PIT_DEL,  // a pitfall fall stopped early
    MP_VFX_TEN_COIN, // a money rock hit
    MP_VFX_FADE,     // a flower trampled
    MP_VFX_CUT,      // an axe hit counted on a tree
    MP_VFX_TREE,     // a tree swaying or losing leaves
    MP_VFX_BALLOON,  // a balloon let go, floating off
    MP_VFX_FISH,     // a fish let go, swimming off
    MP_VFX_FTR_SWITCH, // room furniture switched on or off (lamps, TVs, stereos, gyroids)
    MP_VFX_FTR_OPEN,   // a dresser, wardrobe or closet opened for its storage
    MP_VFX_FTR_CLOSE,
    MP_VFX_FTR_MOVE,   // furniture pushed or pulled
    MP_VFX_FTR_ROTATE,
    MP_VFX_FTR_BIRTH,  // furniture put down in the room
    MP_VFX_FTR_BYE,    // furniture picked up
    MP_VFX_FTR_RECORD, // a stereo switched, or a record put in, swapped or taken back to the music box
    MP_VFX_MAILBOX, // a player opened their mailbox
    MP_VFX_FLAG,    // the island flag lowered or raised
    MP_VFX_SIGN,    // a signboard put up or pulled out
    MP_VFX_SNOW_BREAK, // a finished snowman knocked over
    MP_VFX_SNOW_DONE,  // guest to host: a snowman it built has settled into the town
    MP_VFX_NPC_FX,     // an effect or sound a shared character's own logic made
    MP_VFX_HEM,        // the wishing well's fairy rose for a player
    MP_VFX_KK,         // K.K.'s show for a player: its song, lights and weather reach the crowd
    MP_VFX_ANGLES,     // host: which way the things on tables face in the house floor it's in
    MP_VFX_PRESENT,    // a villager (or Tortimer) coming to a player's door with a present: whose look it has
    MP_VFX_GOKI_KILL,  // to the game running the room's cockroaches: one of them stepped on
    MP_VFX_GOKI_MAKE,  // to the game running the room's cockroaches: one a furniture move turned up
};
void mp_npc_present_look(const unsigned char* body, int len); // pc_mp_npc.c
void mp_house_angles_in(const unsigned char* body, int len); // pc_mp_world.c
unsigned int mp_player_here_mask(void); // the other players in this room with this one, a bit each
int mp_player_in_town(int slot);        // another player is in the town (their look arrived, they haven't left)
int mp_goki_runner(void); // this game runs the room's cockroaches: nobody here has a lower slot
const mp_goki_t* mp_goki_heard(int* n); // the room's cockroaches as another player here runs them, else NULL
int aHG_mp_capture(void* play, mp_goki_t* out, int max); // ac_house_goki.c: -1 when this game doesn't run them
void aHG_mp_frame(void* play);                            // ac_house_goki.c: before the room moves
void aHG_mp_replay(void* play, const unsigned char* body, int len); // ac_house_goki.c: one stepped on elsewhere
int aHG_mp_room_ct(void* game);   // ac_house_goki.c: TRUE when this game brings none of its own out
int aHG_mp_follows(void);         // ac_house_goki.c: another player's game runs this room's cockroaches
void aHG_mp_room_end(void);       // ac_house_goki.c
void aMR_mp_goki_make(const unsigned char* body, int len); // ac_my_room.c: one another player's move turned up
int mp_rainbow_due(void);   // a visitor: the host's rainbow is today's and hasn't shown here yet
void mp_rainbow_shown(void); // m_kankyo.c: this screen's rainbow came out
void aNTT_mp_replay(struct game_play_s* play, const unsigned char* body, int len); // ac_npc_totakeke.c
int aMR_mp_replay(const unsigned char* body, int len); // ac_my_room.c: FALSE to try again next frame
void aMR_mp_keep_switch(int scene, int field, const unsigned char* body, int len); // host: a floor it isn't in
int mp_player_on_unit(int ut); // another player in this room stands, sits or lies on that unit
int mp_others_in_block(int bx, int bz); // another player stands outdoors in that acre (mFI_Wpos2BlockNum numbers)
void aMBX_mp_replay(struct game_play_s* play, const unsigned char* body, int len); // ac_mailbox.c
void aFLAG_mp_replay(struct game_play_s* play, const unsigned char* body, int len); // ac_flag.c
void aSIGN_mp_replay(const unsigned char* body, int len);                           // ac_sign.c
void aSMAN_mp_replay(struct game_play_s* play, const unsigned char* body, int len); // ac_snowman.c
void pc_mp_snow_report(const mp_snow_t* snow); // the local game rolls this snowball now
int mp_snow_remote(int part, mp_snow_t* snow);  // another player rolls it: its newest state
void pc_mp_ball_report(const mp_ball_t* ball);   // the local game moves the ball now
int mp_ball_remote(mp_ball_t* ball, int* slot, unsigned int* age_ms); // the newest mover's ball, if fresh

// other players on this screen, for things that react to whoever is near (bridges, gyroids, doors, fish)
int mp_puppets(void** actors, int max);
void mp_nearest_player_pos(const void* pos, void* out_pos); // this screen's player unless a puppet is closer
struct game_play_s;
void Ac_Balloon_mp_replay(struct game_play_s* play, const unsigned char* body, int len); // ac_balloon.c
void aGYR_mp_replay(struct game_play_s* play, const unsigned char* body, int len);       // ac_gyo_release.c

// station PA announcements (pc_mp_notice.c), spoken by the notice actor at quiet moments
struct game_play_s;
unsigned int mp_notice_push(int msg_no, const unsigned char* town, const unsigned char* name); // its serial
int mp_notice_busy(void);
int mp_notice_begin(void* actor, unsigned int* serial, unsigned char* town, unsigned char* name); // msg, 0 if none
int mp_notice_withdrawn(void* actor); // the actor's notice was cancelled before it was spoken
void mp_notice_actor_gone(void* actor, int spoken);
void mp_notice_answer(unsigned int serial, int yes); // the notice actor: what the host chose for a caller
int mp_notice_take_answer(unsigned int serial);      // -1 until that question is answered
int mp_notice_cancel(unsigned int serial); // TRUE when it can't be heard any more
void mp_notice_clear(void);                // the trip is over: nothing left is about this town
void mp_notice_tick(struct game_play_s* play);
int mp_quiet_moment(struct game_play_s* play); // free control, no window, menu or wipe

// play hooks (m_play.c): every play frame (menus too), then around the actor update
struct game_play_s;
void pc_mp_play_frame(struct game_play_s* play);
void pc_mp_play_pre(struct game_play_s* play);
void pc_mp_play_post(struct game_play_s* play);

// passport side file (pc_mp_journal.c): the traveller's own data while away over the network
void mp_passport_begin(int player_no); // departure: this player's files; stale ones go
int mp_passport_write(void);                        // written and synced before it returns
int mp_passport_settle(void);                       // what's staged is on the card before it returns
int mp_passport_rewrite(const void* priv);          // the card's passport again, this traveller, same host mark
unsigned int mp_passport_write_async(void);         // on the writer thread; 0 when there's no trip
int mp_passport_written(unsigned int seq, int* ok); // that write (or a newer one) is done
int mp_passport_recover(int player_no); // TRUE when a lost trip was restored into the save
int mp_passport_restored_any(void);     // ...and it had something in its pockets or wallet
void mp_passport_delete(void);
unsigned int mp_crc32(const void* data, int len, unsigned int crc);

// multiplayer's own memory, a system block apart from the game's heap (vita_mp_net.c)
void* mp_alloc(unsigned int n);
void* mp_calloc(unsigned int count, unsigned int size);
void mp_free(void* p);
void* mp_zalloc(void* opaque, unsigned int items, unsigned int size); // zlib's hooks onto the same
void mp_zfree(void* opaque, void* p);
unsigned int mp_mem_room(void); // free bytes left for another visitor

// vita net stack and UDP transport (vita_mp_net.c)
int vita_mp_stack_up(void);
int vita_mp_local_ip(char* out, int out_size);
unsigned int vita_mp_local_ip_u32(void);
unsigned int vita_mp_netctl_ipv4(int code);
int vita_mp_wifi_connected(void);
int vita_mp_wifi_ready(void); // MP_UI_BUSY while the console associates, then DONE or NO_WIFI
void vita_mp_sleep_ms(int ms);
unsigned int vita_mp_heap_free(void);
const mp_transport_t* vita_mp_udp_open(int host, unsigned short disc_port, unsigned short game_port, int* port_idx);
void vita_mp_udp_close(void);
void vita_mp_udp_reopen(void);              // after sleep: fresh sockets (a host's on the same ports)
int vita_mp_udp_move_game(unsigned short port); // host: the game socket moves to this port
void vita_mp_udp_tick(unsigned int now_ms); // retries a reopen that found no network yet

// ad hoc (vita_mp_adhoc.c): the system's connection dialog, then PDP by MAC
int vita_mp_adhoc_connect(void); // MP_UI_BUSY while the dialog is up, then DONE / ADHOC_CANCEL / NOBIND
const mp_transport_t* vita_mp_adhoc_open(int host, unsigned short disc_port, unsigned short game_port);
void vita_mp_adhoc_close(void);
int vita_mp_dialog_active(void); // the frame swaps with it and the buttons are its

// the Vita's own keyboard for chat (vita_ime.c): over the running game, its events come through vita_ime_update
int vita_ime_open(void);
void vita_ime_close(void);
int vita_ime_active(void); // the buttons are its while it's up
int vita_ime_top(void);    // the top of its keyboard on screen, in 544-line pixels
void vita_ime_update(void);
int vita_mp_wifi_recovering(void);

// far away (vita_mp_nat.c): the router forwards the game port; worker thread, polled
void vita_mp_nat_open(unsigned short port, unsigned short first, int count); // port, else another from first on
unsigned short vita_mp_nat_port(void);      // the port forwarded, once poll says DONE
void vita_mp_nat_lookup(void);              // guest: just this house's outside address
int vita_mp_nat_poll(unsigned int* ext_ip); // MP_UI_BUSY, then DONE / NOPORTMAP / CGNAT
int vita_mp_nat_idle(void);                 // no worker left from an earlier line
void vita_mp_nat_close(void);
void vita_mp_nat_close_wait(int max_ms);    // app exit: give the port back before the process ends

// pc_m_card.c
void pc_mc_lock(void);            // the save mutex (recursive)
void pc_mc_unlock(void);
int pc_player_free_control(void); // player is idle / walking / running
void* pc_mc_travel_save(void);    // the Save that rides the train; host snapshot scratch
const char* pc_mc_home_dir(void);
void* pc_mc_passport(void);       // mCD_foreigner_c of the current trip
int pc_mc_net_return(void);       // return-trip save for a network visitor
int pc_mc_commit_save_begin(void);    // host: full save of the town on the writer thread; -1 if not now
unsigned int pc_mc_save_landed(void); // serial of the newest town save known to be on disk
unsigned int pc_mc_save_done(void);   // serial of the newest town save that finished, landed or not
void pc_save_actor_cells(struct game_play_s* play, void (*cb)(unsigned int save_off, unsigned short name, void* arg),
                         void* arg);

#else

#define mp_active()         0
#define mp_is_host()        0
#define mp_is_guest()       0
#define pc_mp_pump()        ((void)0)
#define pc_mp_frame_begin() ((void)0)
#define pc_mp_frame_end()   ((void)0)
#define pc_mp_on_app_exit() ((void)0)
#define pc_mp_on_resume(c)  ((void)0)
#define MP_CRUMB(s)         ((void)0)

#endif

#ifdef __cplusplus
}
#endif

#endif
