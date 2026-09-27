// pc_mp_travel.c
// network travel: the host streams its town, the guest rides the ordinary train with it
#include "pc_mp.h"

#ifdef VITA_MP

#include "m_common_data.h"
#include "m_actor.h"
#include "m_bgm.h"
#include "m_card.h"
#include "m_field_info.h"
#include "m_field_make.h"
#include "m_land.h"
#include "m_msg.h"
#include "m_npc.h"
#include "m_play.h"
#include "m_player_lib.h"
#include "m_scene.h"
#include "m_scene_table.h"
#include "m_shop.h"
#include "m_start_data_init.h"
#include "m_submenu.h"
#include "m_kankyo.h"
#include "m_train_control.h"
#include "pc_mp_text_data.h"
#include "pc_settings.h"
#include "dolphin/os.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "c_keyframe.h"
#include <zlib.h>

#define MP_SNAP_STEP        (24 * 1024) // deflate input per frame
#define MP_SNAP_TIMEOUT_MS  45000
#define MP_TIME_PERIOD_MS   5000
#define MP_TIME_FINE_MS     40 // guest: a clock this far off the host's is set right (the hour's chime and music, shop hours)
#define MP_CLOSING_GRACE_S  60
#define MP_TICKS_PER_MS     ((OSTime)(OS_TIMER_CLOCK / 1000))

// what the guest can't rebuild from the save: today's weather and event bookkeeping
typedef struct {
    s16 weather;
    s16 weather_intensity;
    lbRTC_time_c weather_time;
    s_xyz wind;
    f32 wind_speed;
    s16 island_weather;
    s16 island_weather_intensity;
    mEv_event_common_u special_event_common;
    mEv_common_data_c event_common;
    u32 event_flags[mEv_EVENT_TYPE_NUM];
    int event_keep_flags[4];
    u8 fish_location;
    xyz_t ball_pos; // where the host's ball rests, so a visitor's isn't rolled somewhere new
    u8 ball_type;
} mp_common_pack_t;

#define MP_SNAP_RAW_LEN ((int)(sizeof(Save_t) + sizeof(mp_common_pack_t)))

// wire helpers

static void mp_put32(u8* p, u32 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static u32 mp_get32(const u8* p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void mp_put64(u8* p, s64 v) {
    mp_put32(p, (u32)v);
    mp_put32(p + 4, (u32)((u64)v >> 32));
}

static s64 mp_get64(const u8* p) {
    return (s64)((u64)mp_get32(p) | ((u64)mp_get32(p + 4) << 32));
}

static s64 mp_game_ticks(void) {
    return OSGetTime() + Save_Get(time_delta);
}

// the host's switches: its own settings; a visitor keeps the host's as last heard
static u8 s_rules[MP_RULE_NUM] = { [0 ... MP_RULE_NUM - 1] = 1 };
static u8 s_rules_sent[MP_RULE_NUM];

static void mp_rules_mine(u8* out) {
    out[MP_RULE_VISITOR_RIGHTS] = (u8)g_pc_settings.mp_visitor_rights;
    out[MP_RULE_ITEMS] = (u8)g_pc_settings.mp_visitor_items;
    out[MP_RULE_DIG] = (u8)g_pc_settings.mp_visitor_dig;
    out[MP_RULE_AXE] = (u8)g_pc_settings.mp_visitor_axe;
    out[MP_RULE_TUNE] = (u8)g_pc_settings.mp_visitor_tune;
    out[MP_RULE_BOARD] = (u8)g_pc_settings.mp_visitor_board;
    out[MP_RULE_COTTAGE] = (u8)g_pc_settings.mp_visitor_cottage;
    out[MP_RULE_DESIGNS] = (u8)g_pc_settings.mp_visitor_designs;
    out[MP_RULE_CHAT] = (u8)g_pc_settings.mp_chat;
}

int mp_rule(int rule) {
    u8 mine[MP_RULE_NUM];

    if (rule < 0 || rule >= MP_RULE_NUM) {
        return FALSE;
    }
    if (mp_is_guest()) {
        return s_rules[rule];
    }
    mp_rules_mine(mine);
    return mine[rule];
}

static void mp_send_rules(int conn) {
    u8 msg[2 + MP_RULE_NUM];

    msg[0] = MP_M_RULES;
    msg[1] = MP_RULE_NUM;
    mp_rules_mine(msg + 2);
    if (conn < 0) {
        memcpy(s_rules_sent, msg + 2, MP_RULE_NUM);
        mp_lobby_send_rel_all(msg, sizeof(msg));
    } else {
        mp_lobby_send_rel(conn, msg, sizeof(msg));
    }
}

// common pack

static void mp_pack_capture(mp_common_pack_t* p) {
    memset(p, 0, sizeof(*p));
    p->weather = Common_Get(weather);
    p->weather_intensity = Common_Get(weather_intensity);
    // (a host out on the island has the island's weather about it; a visitor comes in at the station)
    if (mFI_CheckInIsland()) {
        p->weather = mEnv_SAVE_GET_WEATHER_TYPE(Save_Get(weather));
        p->weather_intensity = mEnv_SAVE_GET_WEATHER_INTENSITY(Save_Get(weather));
    }
    p->weather_time = Common_Get(weather_time);
    p->wind = Common_Get(wind);
    p->wind_speed = Common_Get(wind_speed);
    p->island_weather = Common_Get(island_weather);
    p->island_weather_intensity = Common_Get(island_weather_intensity);
    p->special_event_common = Common_Get(special_event_common);
    p->event_common = Common_Get(event_common);
    memcpy(p->event_flags, Common_Get(event_flags), sizeof(p->event_flags));
    memcpy(p->event_keep_flags, Common_Get(event_keep_flags), sizeof(p->event_keep_flags));
    p->fish_location = Common_Get(fish_location);
    p->ball_pos = Common_Get(ball_pos);
    p->ball_type = Common_Get(ball_type);
}

static void mp_pack_apply(const mp_common_pack_t* p) {
    Common_Set(weather, p->weather);
    Common_Set(weather_intensity, p->weather_intensity);
    Common_Set(weather_time, p->weather_time);
    Common_Set(wind, p->wind);
    Common_Set(wind_speed, p->wind_speed);
    Common_Set(island_weather, p->island_weather);
    Common_Set(island_weather_intensity, p->island_weather_intensity);
    Common_Set(special_event_common, p->special_event_common);
    Common_Set(event_common, p->event_common);
    memcpy(Common_Get(event_flags), p->event_flags, sizeof(p->event_flags));
    memcpy(Common_Get(event_keep_flags), p->event_keep_flags, sizeof(p->event_keep_flags));
    Common_Set(fish_location, p->fish_location);
    Common_Set(ball_pos, p->ball_pos);
    Common_Set(ball_type, p->ball_type);
}

// host: snapshot

enum {
    MP_SNAP_IDLE,
    MP_SNAP_WANTED,  // a guest asked; capture at the next play frame
    MP_SNAP_PACKING, // deflating a few KB per frame
    MP_SNAP_SENDING, // bulk transfer in flight
};

static struct {
    int stage;
    int conn;
    int queue[MP_LINK_CONNS];
    int queued;
    z_stream zs;
    int zs_live;
    int fed;
    unsigned int crc;
    u8* comp;
    int comp_len;
    int info_sent;
    mp_common_pack_t pack;
    unsigned int last_time_ms;
} s_snap;

// cells whose items live in actors right now (dropped items, buildings, snowmen) get
// their saved names back, the way restore_fgdata writes them when a scene ends
static void mp_snap_fix_cell(unsigned int off, unsigned short name, void* arg) {
    mActor_name_t* out = (mActor_name_t*)((u8*)arg + off);

    if (off > sizeof(Save_t) - sizeof(mActor_name_t)) {
        return;
    }
    if (ITEM_NAME_GET_TYPE(name) == NAME_TYPE_ITEM2 && *out != EMPTY_NO) {
        return;
    }
    *out = name;
}

static void mp_snap_fix_switch(unsigned int off, unsigned long long bits, void* arg) {
    if (off + sizeof(bits) <= sizeof(Save_t)) {
        memcpy((u8*)arg + off, &bits, sizeof(bits));
    }
}

static void mp_snap_fix_actors(Save_t* dst, GAME_PLAY* play) {
    if (play != NULL) {
        mp_world_actor_cells(play, mp_snap_fix_cell, dst);
    }
    // a room the host is in keeps what's stored in its furniture there, and its lamps, not in the save
    aMR_pc_stored_cells(mp_snap_fix_cell, dst);
    aMR_pc_switch_tables(mp_snap_fix_switch, dst);
}

static void mp_snap_free(void) {
    if (s_snap.stage == MP_SNAP_SENDING) {
        mp_link_bulk_cancel(s_snap.conn); // the link reads straight from comp
    }
    if (s_snap.zs_live) {
        deflateEnd(&s_snap.zs);
        s_snap.zs_live = FALSE;
    }
    mp_free(s_snap.comp);
    s_snap.comp = NULL;
    s_snap.comp_len = 0;
}

static void mp_snap_next(void) {
    mp_snap_free();
    s_snap.stage = MP_SNAP_IDLE;
    s_snap.conn = -1;
    if (s_snap.queued > 0) {
        s_snap.conn = s_snap.queue[0];
        memmove(s_snap.queue, s_snap.queue + 1, (s_snap.queued - 1) * sizeof(int));
        s_snap.queued--;
        s_snap.stage = MP_SNAP_WANTED;
    }
}

static void mp_snap_request(int conn) {
    int i;

    if (s_snap.stage != MP_SNAP_IDLE && s_snap.conn == conn) {
        return;
    }
    for (i = 0; i < s_snap.queued; i++) {
        if (s_snap.queue[i] == conn) {
            return;
        }
    }
    if (s_snap.stage == MP_SNAP_IDLE) {
        s_snap.conn = conn;
        s_snap.stage = MP_SNAP_WANTED;
    } else if (s_snap.queued < MP_LINK_CONNS) {
        s_snap.queue[s_snap.queued++] = conn;
    }
}

static void mp_snap_forget(int conn) {
    int i;

    for (i = 0; i < s_snap.queued; i++) {
        if (s_snap.queue[i] == conn) {
            memmove(s_snap.queue + i, s_snap.queue + i + 1, (s_snap.queued - i - 1) * sizeof(int));
            s_snap.queued--;
            break;
        }
    }
    if (s_snap.stage != MP_SNAP_IDLE && s_snap.conn == conn) {
        mp_snap_next();
    }
}

static void mp_snap_capture(GAME_PLAY* play) {
    Save_t* raw = &((Save*)pc_mc_travel_save())->save;
    int cap;

    memcpy(raw, &common_data.save.save, sizeof(Save_t));
    mp_snap_fix_actors(raw, play);
    mp_world_snap_taken(s_snap.conn, raw);
    mp_pack_capture(&s_snap.pack);

    memset(&s_snap.zs, 0, sizeof(s_snap.zs));
    s_snap.zs.zalloc = mp_zalloc;
    s_snap.zs.zfree = mp_zfree;
    if (deflateInit2(&s_snap.zs, 3, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        pc_mp_log("[MP] snapshot: deflateInit failed");
        mp_snap_next();
        return;
    }
    s_snap.zs_live = TRUE;
    cap = (int)deflateBound(&s_snap.zs, MP_SNAP_RAW_LEN);
    s_snap.comp = (u8*)mp_alloc(cap);
    if (s_snap.comp == NULL || cap > MP_BULK_MAX) {
        pc_mp_log("[MP] snapshot: no room (%d bytes)", cap);
        mp_snap_next();
        return;
    }
    s_snap.zs.next_out = s_snap.comp;
    s_snap.zs.avail_out = (uInt)cap;
    s_snap.fed = 0;
    s_snap.crc = mp_crc32(raw, sizeof(Save_t), 0);
    s_snap.crc = mp_crc32(&s_snap.pack, sizeof(s_snap.pack), s_snap.crc);
    s_snap.info_sent = FALSE;
    s_snap.stage = MP_SNAP_PACKING;
}

static void mp_snap_pack_step(void) {
    const u8* raw = (const u8*)&((Save*)pc_mc_travel_save())->save;
    int budget = MP_SNAP_STEP;

    while (budget > 0) {
        const u8* src;
        int n;
        int left;
        int ret;

        if (s_snap.fed < (int)sizeof(Save_t)) {
            src = raw + s_snap.fed;
            left = (int)sizeof(Save_t) - s_snap.fed;
        } else {
            src = (const u8*)&s_snap.pack + (s_snap.fed - (int)sizeof(Save_t));
            left = MP_SNAP_RAW_LEN - s_snap.fed;
        }
        n = left < budget ? left : budget;
        s_snap.zs.next_in = (Bytef*)src;
        s_snap.zs.avail_in = (uInt)n;
        ret = deflate(&s_snap.zs, (s_snap.fed + n == MP_SNAP_RAW_LEN) ? Z_FINISH : Z_NO_FLUSH);
        n -= (int)s_snap.zs.avail_in;
        s_snap.fed += n;
        budget -= n;
        if (ret == Z_STREAM_END) {
            s_snap.comp_len = (int)s_snap.zs.total_out;
            deflateEnd(&s_snap.zs);
            s_snap.zs_live = FALSE;
            s_snap.stage = MP_SNAP_SENDING;
            return;
        }
        if (ret != Z_OK || n == 0) {
            pc_mp_log("[MP] snapshot: deflate stalled (%d)", ret);
            mp_snap_next();
            return;
        }
    }
}

// the day's rainbow: the host's comes out the day it's reserved for; each visitor's once, the same day
static u32 s_rb_host_day; // host: the day its rainbow was due
static u8 s_rb_heard;     // visitor: the host's clock says its rainbow is today's
static u32 s_rb_shown;    // visitor: the day its own came out

static u32 mp_rb_today(void) {
    const lbRTC_time_c* t = Common_GetPointer(time.rtc_time);

    return ((u32)t->year << 16) | ((u32)t->month << 8) | t->day;
}

int mp_rainbow_due(void) {
    return mp_is_guest() && s_rb_heard && s_rb_shown != mp_rb_today();
}

void mp_rainbow_shown(void) {
    if (mp_is_guest()) {
        s_rb_shown = mp_rb_today();
    }
}

static void mp_send_time(int conn) {
    const lbRTC_time_c* t = Common_GetPointer(time.rtc_time);
    u8 msg[10];

    msg[0] = MP_M_TIME;
    mp_put64(msg + 1, mp_game_ticks());
    // (shown already, it's still today's: a visitor's screen hasn't had it)
    msg[9] = Save_Get(rainbow_month) == t->month && Save_Get(rainbow_day) == t->day &&
             (s_rb_host_day == mp_rb_today() || Common_Get(rainbow_opacity) > 0.0f);
    if (conn < 0) {
        mp_lobby_send_rel_all(msg, sizeof(msg));
    } else {
        mp_lobby_send_rel(conn, msg, sizeof(msg));
    }
}

static void mp_snap_send_step(void) {
    u8 msg[16];

    if (s_snap.info_sent) {
        return;
    }
    msg[0] = MP_M_SNAP_INFO;
    mp_put32(msg + 1, MP_SNAP_RAW_LEN);
    mp_put32(msg + 5, (u32)s_snap.comp_len);
    mp_put32(msg + 9, s_snap.crc);
    if (!mp_lobby_send_rel(s_snap.conn, msg, 13)) {
        return; // window full; try again next frame
    }
    mp_send_time(s_snap.conn);
    mp_send_rules(s_snap.conn);
    if (!mp_link_bulk_send(s_snap.conn, s_snap.comp, s_snap.comp_len)) {
        pc_mp_log("[MP] snapshot: bulk refused");
        mp_snap_next();
        return;
    }
    s_snap.info_sent = TRUE;
}

// guest: trip

static struct {
    mp_travel_state_t state;
    unsigned int start_ms;
    int have_info;
    u32 raw_len;
    u32 comp_len;
    u32 crc;
    u8* comp;
    int xfer;
    int count;
    int chunks;
    int bytes;
    u8 seen[MP_BULK_MAX / MP_BULK_CHUNK / 8];
    mp_common_pack_t pack;
    s64 time_delta;
    int time_valid;
    s64 clock_delta; // the host's clock over ours, filtered for the shows that run by it
    int clock_valid;
    int closing;
    unsigned int closing_deadline_ms;
    int force_return;
    int force_notice; // announcement played before a forced return
    int noticed;
    int boarding; // forced return saved; waiting for the scene change to take
    int gave_up;  // the home save couldn't be read: no more forced returns this trip
    int pull_in;  // this arrival's train: 1 set off here, 2 the host heard
    unsigned int trouble; // serial of the "trouble on the line" notice, 0 if none
} s_trip = { MP_TRAVEL_NONE };

static void mp_trip_set(mp_travel_state_t state) {
    if (s_trip.state != state) {
        s_trip.state = state;
    }
}

static void mp_trip_drop_download(void) {
    mp_free(s_trip.comp);
    s_trip.comp = NULL;
    s_trip.have_info = FALSE;
    s_trip.count = 0;
    s_trip.chunks = 0;
    s_trip.bytes = 0;
    s_trip.xfer = -1;
    memset(s_trip.seen, 0, sizeof(s_trip.seen));
}

static void mp_trip_fail(const char* why) {
    pc_mp_log("[MP] trip failed: %s", why);
    mp_trip_drop_download();
    mp_trip_set(MP_TRAVEL_FAILED);
}

mp_travel_state_t mp_travel_state(void) {
    return s_trip.state;
}

int mp_travel_net_trip(void) {
    return s_trip.state >= MP_TRAVEL_DEPARTED;
}

long long mp_travel_time_delta(void) {
    return s_trip.time_delta;
}

// the shared clock in frames (60 a second): the town's looping sights run on it look alike on every screen
unsigned int pc_mp_world_frame(unsigned int local) {
    unsigned int ms;

    if (!mp_active() || !mp_shared_clock_ms(&ms)) {
        return local;
    }
    return (ms / 50u) * 3u + ((ms % 50u) * 3u) / 50u;
}

void pc_mp_kf_phase(void* frame_control) {
    cKF_FrameControl_c* fc = (cKF_FrameControl_c*)frame_control;
    unsigned int ms;
    f32 len = fc->end_frame - fc->start_frame;

    if (fc->mode != cKF_FRAMECONTROL_REPEAT || fc->speed <= 0.0f || len <= 0.0f || !mp_active() ||
        !mp_shared_clock_ms(&ms)) {
        return;
    }
    fc->current_frame = fc->start_frame + (f32)fmod((double)pc_mp_world_frame(0) * fc->speed, (double)len);
}

int mp_shared_clock_ms(unsigned int* ms) {
    s64 t;
    int g;

    if (mp_is_host()) {
        for (g = 1; g < MP_MAX_PEERS && !mp_lobby_guest_arrived(g); g++) {
        }
        if (g == MP_MAX_PEERS) {
            return FALSE;
        }
        t = OSGetTime() + Save_Get(time_delta);
    } else if (s_trip.state == MP_TRAVEL_VISITING && s_trip.clock_valid) {
        t = OSGetTime() + s_trip.clock_delta;
    } else {
        return FALSE;
    }
    *ms = (unsigned int)(t / MP_TICKS_PER_MS);
    return TRUE;
}

unsigned int mp_shared_hash(unsigned int seed, unsigned int a, unsigned int b) {
    unsigned int h = seed * 0x9E3779B1u ^ a * 0x85EBCA77u ^ b * 0xC2B2AE3Du;

    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

int mp_travel_host_closing_left(void) {
    int left;

    if (!s_trip.closing) {
        return -1;
    }
    left = (int)(s_trip.closing_deadline_ms - pc_mp_now_ms()) / 1000;
    return left > 0 ? left : 0;
}

void mp_travel_guest_fetch(void) {
    u8 msg[1];
    int conn = mp_lobby_host_conn();

    mp_trip_drop_download();
    s_trip.time_valid = FALSE;
    s_trip.clock_valid = FALSE;
    s_rb_heard = FALSE;
    s_rb_shown = 0;
    s_trip.closing = FALSE;
    s_trip.force_return = FALSE;
    s_trip.noticed = FALSE;
    s_trip.boarding = FALSE;
    s_trip.gave_up = FALSE;
    s_trip.trouble = 0;
    s_trip.start_ms = pc_mp_now_ms();
    mp_trip_set(MP_TRAVEL_FETCHING);
    msg[0] = MP_M_SNAP_REQ;
    if (conn < 0 || !mp_lobby_send_rel(conn, msg, 1)) {
        mp_trip_fail("no line");
    }
}

// every chunk and the size header are in; unpack straight into the travel buffer
static void mp_trip_unpack(void) {
    Save_t* dst = &((Save*)pc_mc_travel_save())->save;
    z_stream zs;
    unsigned int crc;
    int ret;

    memset(&zs, 0, sizeof(zs));
    zs.zalloc = mp_zalloc;
    zs.zfree = mp_zfree;
    if (s_trip.raw_len != (u32)MP_SNAP_RAW_LEN || inflateInit(&zs) != Z_OK) {
        mp_trip_fail("bad snapshot size");
        return;
    }
    zs.next_in = s_trip.comp;
    zs.avail_in = s_trip.comp_len;
    zs.next_out = (Bytef*)dst;
    zs.avail_out = sizeof(Save_t);
    ret = inflate(&zs, Z_SYNC_FLUSH);
    if (ret == Z_OK && zs.avail_out == 0) {
        zs.next_out = (Bytef*)&s_trip.pack;
        zs.avail_out = sizeof(s_trip.pack);
        ret = inflate(&zs, Z_FINISH);
    }
    inflateEnd(&zs);
    if (ret != Z_STREAM_END || zs.total_out != (uLong)MP_SNAP_RAW_LEN) {
        mp_trip_fail("inflate");
        return;
    }
    crc = mp_crc32(dst, sizeof(Save_t), 0);
    crc = mp_crc32(&s_trip.pack, sizeof(s_trip.pack), crc);
    if (crc != s_trip.crc || !mLd_CheckId(dst->land_info.id) || dst->land_info.id != mp_ui_join_land_id()) {
        mp_trip_fail("snapshot check");
        return;
    }
    mp_trip_drop_download();
    mp_trip_set(MP_TRAVEL_READY);
}

static void mp_trip_check_done(void) {
    if (s_trip.state == MP_TRAVEL_FETCHING && s_trip.have_info && s_trip.comp != NULL && s_trip.count > 0 &&
        s_trip.chunks == s_trip.count) {
        if ((u32)s_trip.bytes != s_trip.comp_len) {
            mp_trip_fail("short snapshot");
            return;
        }
        mp_trip_unpack();
    }
}

int mp_travel_guest_poll(void) {
    switch (s_trip.state) {
        case MP_TRAVEL_READY:
            // the clock rides along with the town
            return s_trip.time_valid ? MP_UI_DONE : MP_UI_BUSY;
        case MP_TRAVEL_FETCHING:
            if (pc_mp_now_ms() - s_trip.start_ms > MP_SNAP_TIMEOUT_MS) {
                mp_trip_fail("timed out");
                return MP_UI_NO_ANSWER;
            }
            return MP_UI_BUSY;
        default:
            return MP_UI_NO_ANSWER;
    }
}

void mp_travel_guest_reset(void) {
    mp_trip_drop_download();
    mp_trip_set(MP_TRAVEL_NONE);
    memset(s_rules, 1, sizeof(s_rules));
}

void mp_travel_departed(void) {
    mp_trip_set(MP_TRAVEL_DEPARTED);
}

// arrival

static void mp_train_reset(void);

// the stock start-up runs on the host's town like any visit, then the town is put back as it came off the line
// (the snapshot rode along from the departure, so what the ride itself wrote goes too): only the host changes it
void mp_travel_arrival_begin(void) {
    Save_t* host = &((Save*)pc_mc_travel_save())->save;

    if (host->land_info.id != Save_Get(land_info).id) {
        memcpy(host, &common_data.save.save, sizeof(Save_t));
    }
}

void mp_travel_arrival_end(void) {
    Save_t* host = &((Save*)pc_mc_travel_save())->save;
    Save_t* now = &common_data.save.save;
    int npcs_changed = memcmp(host->animals, now->animals, sizeof(now->animals)) != 0;

    // the visit's own bookkeeping stays; everything else is the host's
    host->scene_no = now->scene_no;
    host->time_delta = now->time_delta;
    host->copy_protect = now->copy_protect;
    host->save_exist = now->save_exist;
    host->travel_hard_time = now->travel_hard_time;
    memcpy(now, host, sizeof(Save_t));
    memset(host, 0, sizeof(Save_t));

    if (npcs_changed) {
        mNpc_InitNpcList(Common_Get(npclist), ANIMAL_NUM_MAX);
        mNpc_SetNpcList(Common_Get(npclist), Save_Get(animals), ANIMAL_NUM_MAX, mSDI_MALLOC_FLAG_ZELDA);
    }
    mSP_SetTanukiShopStatus();
    mp_pack_apply(&s_trip.pack);

    mEv_mp_areas_reset();
    mp_set_role(MP_ROLE_GUEST);
    mp_train_reset();
    s_trip.pull_in = 0;
    mp_trip_set(MP_TRAVEL_VISITING);
    mp_world_guest_arrived();
    mp_passport_write();
    {
        u8 msg[1] = { MP_M_ARRIVED };
        int conn = mp_lobby_host_conn();

        if (conn >= 0) {
            mp_lobby_send_rel(conn, msg, 1);
            mp_lobby_hold_line();
        } else {
            // the line dropped during the ride
            s_trip.force_return = TRUE;
            s_trip.force_notice = MP_MSG_N_HOST_CUT;
        }
    }
}

// the ride-off demo's train set off; the host's train and its announcement go with it
void mp_travel_pulling_in(void) {
    if (s_trip.state == MP_TRAVEL_VISITING && s_trip.pull_in == 0) {
        s_trip.pull_in = 1;
    }
}

void mp_travel_returning(void) {
    u8 msg[4];
    unsigned short seen = 0;
    int conn = mp_lobby_host_conn();
    int i;

    // the host learns which pickups came along; then the town stops listening to this visitor
    msg[0] = MP_M_LEAVING;
    msg[1] = (u8)mp_world_commit_mark(&seen);
    msg[2] = (u8)seen;
    msg[3] = (u8)(seen >> 8);
    mp_world_end();
    // (a full line gets a few moments to make room)
    for (i = 0; i < 30 && conn >= 0 && !mp_lobby_send_rel(conn, msg, 4); i++) {
        mp_lobby_pump();
        conn = mp_lobby_host_conn();
    }
    s_trip.force_return = FALSE;
    mp_trip_set(MP_TRAVEL_RETURNING);
}

// the train home arrived: the trip is over; the passport stays until a home save holds the traveller
void mp_travel_home(int saved) {
    if (saved) {
        mp_passport_delete();
    }
    mp_player_reset();
    mp_notice_clear(); // announcements about the town left behind
    mp_set_role(MP_ROLE_NONE);
    mp_lobby_guest_end();
    s_trip.closing = FALSE;
    mp_trip_set(MP_TRAVEL_NONE);
}

// forced trip home

static int mp_scene_out_of_town(int scene);

int mp_quiet_moment(GAME_PLAY* play) {
    int scene = Save_Get(scene_no);

    if (scene == SCENE_START_DEMO3 || mp_scene_out_of_town(scene)) {
        return FALSE;
    }
    if (!pc_player_free_control() || mMsg_Check_MainHide(mMsg_Get_base_window_p()) != TRUE) {
        return FALSE;
    }
    if (play->submenu.open_flag || play->submenu.process_status != mSM_PROCESS_WAIT || pc_mp_chat_typing()) {
        return FALSE;
    }
    return play->fb_wipe_mode == WIPE_MODE_NONE && play->fb_mode == FBDEMO_MODE_NONE;
}

// same door the station master opens when a traveller boards (aNPS2_make_door_data)
static int mp_trip_board_train(GAME_PLAY* play) {
    Door_data_c door;

    memset(&door, 0, sizeof(door));
    door.next_scene_id = SCENE_START_DEMO3;
    door.exit_orientation = mSc_DIRECT_NORTH;
    door.exit_type = 0;
    door.extra_data = 0;
    door.exit_position.x = 120;
    door.exit_position.y = 0;
    door.exit_position.z = 340;
    door.door_actor_name = EMPTY_NO;
    door.wipe_type = WIPE_TYPE_CIRCLE_RIGHT;
    Common_Get(transition).wipe_type = WIPE_TYPE_FADE_BLACK;
    if (goto_other_scene(play, &door, TRUE) != TRUE) {
        return FALSE;
    }
    mBGMPsComp_make_ps_wipe(0x328A);
    mBGMPsComp_scene_mode(11);
    return TRUE;
}

static void mp_trip_force_home(GAME_PLAY* play) {
    if (!mp_quiet_moment(play)) {
        return;
    }
    if (pc_mc_net_return() != mCD_TRANS_ERR_NONE) {
        pc_mp_log("[MP] forced return: home save unreadable, staying put");
        s_trip.force_return = FALSE;
        s_trip.gave_up = TRUE;
        return;
    }
    s_trip.boarding = !mp_trip_board_train(play);
}

// host: a visitor's train is pulling in; every Porter but theirs steps off the spot they land on
static void mp_trip_train(int conn) {
    u8 msg[2] = { MP_M_TRAIN, 1 };
    int g;

    aSTM_mp_arriving();
    for (g = 1; g < MP_MAX_PEERS; g++) {
        int c = mp_lobby_guest_conn(g);

        if (c >= 0 && c != conn && mp_lobby_guest_arrived(g)) {
            mp_lobby_send_rel(c, msg, (int)sizeof(msg));
        }
    }
}

// one train for everyone: the host runs it, and while a screen's own Porter or arrival holds it, that screen runs it;
// the others take its reports and step it the same way between them (only the runner decides when it leaves)
#define MP_TRAIN_LEN     24
#define MP_TRAIN_EVERY   6    // frames between reports (no train about: MP_TRAIN_IDLE)
#define MP_TRAIN_IDLE    60
#define MP_TRAIN_LOST_MS 3000 // host: a visitor running it this quiet has let it go
#define MP_TRAIN_FRESH   1    // a train just set off: its first sound
#define MP_TRAIN_DONE    2    // the runner lets it go

static struct {
    u8 drive;  // guest: this screen runs it now
    u8 driver; // host: the visitor running it (0 itself)
    u8 have;   // follower: a report waits
    u8 msg[MP_TRAIN_LEN];
    u8 run;    // runner: trains set off
    u8 start;  // ...how the newest began
    u8 fresh;  // ...reports still carrying that
    u8 got;          // follower: a report heard (msg holds the newest)
    u8 heard_ok;
    u8 heard_driver; // follower: the start last sounded
    u8 heard_run;
    u8 sent[4];      // runner: action, control states and signal as last reported
    u32 next_frame;
    u32 heard_ms;    // host: the visitor running it, last heard
} s_train;

static void mp_train_reset(void) {
    memset(&s_train, 0, sizeof(s_train));
}

static int mp_train_live(void) {
    int s;

    if (mp_is_host()) {
        for (s = 1; s < MP_MAX_PEERS; s++) {
            if (mp_lobby_guest_conn(s) >= 0) {
                return TRUE;
            }
        }
        return FALSE;
    }
    return (s_trip.state == MP_TRAVEL_VISITING || s_trip.state == MP_TRAVEL_RETURNING) && mp_lobby_host_conn() >= 0;
}

// the runner still has the train standing for its player: a Porter's call or hold, or an arrival
static int mp_train_held(void) {
    u8 a = Common_Get(train_action);

    return Common_Get(train_coming_flag) != 0 ||
           ((Common_Get(train_control_state) != 0 || Common_Get(train_last_control_state) != 0) &&
            a != mTRC_ACTION_NONE && a < mTRC_ACTION_SIGNAL_STARTING);
}

// host: the visitor running it is gone or silent; a train it held for its player leaves, as when seen off
static void mp_train_host_resume(void) {
    u8 a = Common_Get(train_action);

    if (s_train.driver == 0) {
        return;
    }
    s_train.driver = 0;
    if (a != mTRC_ACTION_NONE && a < mTRC_ACTION_SIGNAL_STARTING &&
        (Common_Get(train_control_state) != 0 || Common_Get(train_last_control_state) != 0)) {
        Common_Set(train_control_state, 0);
        Common_Set(train_last_control_state, 1);
    }
}

int mp_train_follow(void) {
    if (!mp_train_live()) {
        mp_train_host_resume(); // (the last visitor left: a train it held for its player leaves, as when seen off)
        s_train.drive = FALSE;
        s_train.driver = 0;
        return FALSE;
    }
    if (mp_is_host()) {
        if (s_train.driver != 0 &&
            (mp_lobby_guest_conn(s_train.driver) < 0 || pc_mp_now_ms() - s_train.heard_ms > MP_TRAIN_LOST_MS)) {
            mp_train_host_resume();
        }
        return s_train.driver != 0;
    }
    return !s_train.drive;
}

// a visitor's train keeps pace behind its own menus, so the host never takes the quiet for leaving
int mp_train_menu_guest(void) {
    return !mp_is_host() && mp_train_live();
}

int mp_train_claim(void) {
    if (mp_is_host()) {
        return s_train.driver == 0; // (a visitor holding it goes first; the host's own Porter waits)
    }
    if (!s_train.drive) {
        // (another visitor holding it goes first)
        if (s_train.got && s_train.msg[1] != 0 && s_train.msg[1] != (u8)mp_lobby_self_slot() &&
            !(s_train.msg[2] & MP_TRAIN_DONE)) {
            return FALSE;
        }
        s_train.drive = TRUE;
        s_train.have = FALSE;
        s_train.next_frame = g_mp_frame;
    }
    return TRUE;
}

static void mp_train_send(const u8* m) {
    int g;

    if (!mp_is_host()) {
        if (mp_lobby_host_conn() >= 0) {
            mp_lobby_send_rel(mp_lobby_host_conn(), m, MP_TRAIN_LEN);
        }
        return;
    }
    for (g = 1; g < MP_MAX_PEERS; g++) {
        int c = mp_lobby_guest_conn(g);

        if (c >= 0 && g != m[1] && (mp_lobby_guest_arrived(g) || g == s_train.driver)) {
            mp_lobby_send_rel(c, m, MP_TRAIN_LEN);
        }
    }
}

void mp_train_report(int start) {
    xyz_t pos = Common_Get(train_position);
    f32 speed = Common_Get(train_speed);
    u8 now[4];
    u8 m[MP_TRAIN_LEN];
    int flags = 0;

    if (!mp_train_live() || (!mp_is_host() && !s_train.drive)) {
        return;
    }
    if (start >= 0) {
        s_train.run++;
        s_train.start = (u8)start;
        s_train.fresh = 4;
    }
    now[0] = Common_Get(train_action);
    now[1] = Common_Get(train_control_state);
    now[2] = Common_Get(train_last_control_state);
    now[3] = Common_Get(train_signal);
    if (!mp_is_host() && !mp_train_held()) {
        flags |= MP_TRAIN_DONE;
    }
    if (memcmp(now, s_train.sent, sizeof(now)) == 0 && s_train.fresh == 0 && !(flags & MP_TRAIN_DONE) &&
        (int)(g_mp_frame - s_train.next_frame) < 0) {
        return;
    }
    if (s_train.fresh > 0) {
        flags |= MP_TRAIN_FRESH;
        s_train.fresh--;
    }
    m[0] = MP_M_TRAIN_STATE;
    m[1] = (u8)mp_lobby_self_slot();
    m[2] = (u8)flags;
    m[3] = s_train.run;
    m[4] = s_train.start;
    m[5] = now[0];
    m[6] = Common_Get(train_timer);
    m[7] = now[3];
    m[8] = now[1];
    m[9] = now[2];
    m[10] = Common_Get(train_day);
    m[11] = 0;
    memcpy(m + 12, &speed, sizeof(f32));
    memcpy(m + 16, &pos.x, sizeof(f32));
    mp_put32(m + 20, Common_Get(train_start_timer));
    mp_train_send(m);
    memcpy(s_train.sent, now, sizeof(now));
    s_train.next_frame = g_mp_frame + (now[0] == mTRC_ACTION_NONE ? MP_TRAIN_IDLE : MP_TRAIN_EVERY);
    if (flags & MP_TRAIN_DONE) {
        s_train.drive = FALSE;
    }
}

// how far along a train is (0 none)
static int mp_train_order(u8 action) {
    return action < mTRC_ACTION_NUM ? action : 0;
}

void mp_train_take(void) {
    const u8* m = s_train.msg;
    u8 action;
    u8 local = Common_Get(train_action);
    xyz_t pos = Common_Get(train_position);
    f32 speed;
    f32 x;
    int fresh;

    if (!s_train.have) {
        return;
    }
    s_train.have = FALSE;
    action = m[5];
    memcpy(&speed, m + 12, sizeof(f32));
    memcpy(&x, m + 16, sizeof(f32));
    fresh = (m[2] & MP_TRAIN_FRESH) &&
            !(s_train.heard_ok && s_train.heard_driver == m[1] && s_train.heard_run == m[3]);
    if (fresh) {
        s_train.heard_ok = TRUE;
        s_train.heard_driver = m[1];
        s_train.heard_run = m[3];
    }
    // (where it is and how fast, when the runner is ahead or this one has drifted; one leaving here runs out)
    if (action != mTRC_ACTION_NONE &&
        (fresh || local == mTRC_ACTION_NONE || mp_train_order(action) > mp_train_order(local) ||
         (action == local && (x - pos.x > 24.0f || pos.x - x > 24.0f)))) {
        if (pos.z == 0.0f) {
            pos.y = 180.0f;
            pos.z = 740.0f;
        }
        pos.x = x;
        Common_Set(train_action, action);
        Common_Set(train_timer, m[6]);
        Common_Set(train_signal, m[7]);
        Common_Set(train_speed, speed);
        Common_Set(train_position, pos);
    }
    Common_Set(train_control_state, m[8]);
    Common_Set(train_last_control_state, m[9]);
    Common_Set(train_day, m[10]);
    Common_Set(train_start_timer, mp_get32(m + 20));
    if (fresh) {
        mTRC_mp_start_sound(m[4]);
    }
}

static void mp_train_heard(int conn, const u8* m) {
    if (mp_is_host()) {
        int slot = mp_lobby_guest_slot(conn) + 1;
        u8 fwd[MP_TRAIN_LEN];

        // (one visitor at a time: a second one's Porter waits for the first to let go)
        if (slot <= 0 || (s_train.driver != 0 && s_train.driver != slot)) {
            return;
        }
        s_train.driver = (m[2] & MP_TRAIN_DONE) ? 0 : (u8)slot;
        s_train.heard_ms = pc_mp_now_ms();
        memcpy(s_train.msg, m, MP_TRAIN_LEN);
        s_train.have = TRUE;
        memcpy(fwd, m, MP_TRAIN_LEN);
        fwd[1] = (u8)slot;
        mp_train_send(fwd);
        // (let go: the host runs it on from there, its own reports next)
        if (s_train.driver == 0) {
            mp_train_take();
            s_train.next_frame = g_mp_frame;
        }
        return;
    }
    if (conn != mp_lobby_host_conn()) {
        return;
    }
    if (s_train.drive) {
        // (another visitor holds it: this one follows; the host's own reports from before it heard this one pass)
        if (m[1] == 0 || m[1] == (u8)mp_lobby_self_slot() || (m[2] & MP_TRAIN_DONE)) {
            return;
        }
        s_train.drive = FALSE;
    }
    memcpy(s_train.msg, m, MP_TRAIN_LEN);
    s_train.have = TRUE;
    s_train.got = TRUE;
}

// events

void mp_travel_on_rel(int conn, const unsigned char* data, int len) {
    if (len < 1) {
        return;
    }
    switch (data[0]) {
        case MP_M_SNAP_REQ:
            if (mp_is_host() && mp_lobby_guest_slot(conn) >= 0 && !mp_ui_host_closing()) {
                mp_snap_request(conn);
            }
            break;
        case MP_M_SNAP_INFO:
            if (s_trip.state == MP_TRAVEL_FETCHING && len >= 13 && conn == mp_lobby_host_conn()) {
                s_trip.raw_len = mp_get32(data + 1);
                s_trip.comp_len = mp_get32(data + 5);
                s_trip.crc = mp_get32(data + 9);
                if (s_trip.comp_len == 0 || s_trip.comp_len > MP_BULK_MAX) {
                    mp_trip_fail("bad size");
                    break;
                }
                s_trip.have_info = TRUE;
                mp_trip_check_done();
            }
            break;
        case MP_M_TIME:
            if (len >= 10 && conn == mp_lobby_host_conn()) {
                s_rb_heard = data[9];
            }
            if (len >= 9 && conn == mp_lobby_host_conn()) {
                int rtt = mp_link_rtt_ms(conn);
                s64 delta = mp_get64(data + 1) + (s64)(rtt > 0 ? rtt / 2 : 0) * MP_TICKS_PER_MS - OSGetTime();

                s_trip.time_delta = delta;
                s_trip.time_valid = TRUE;
                // a message held up on the way reads the host's clock as behind: rise at once, sink slowly
                if (!s_trip.clock_valid || delta > s_trip.clock_delta) {
                    s_trip.clock_delta = delta;
                } else {
                    s_trip.clock_delta += (delta - s_trip.clock_delta) / 8;
                }
                s_trip.clock_valid = TRUE;
                if (s_trip.state == MP_TRAVEL_VISITING) {
                    s64 drift = s_trip.clock_delta - Save_Get(time_delta);
                    int min = Common_Get(time.rtc_time.min);

                    // (by the filtered clock, so a message held up on the way can't turn time back; never back about
                    // the hour, where that would sound the chime twice or start the hour's music over)
                    if (drift > MP_TIME_FINE_MS * MP_TICKS_PER_MS ||
                        (drift < -MP_TIME_FINE_MS * MP_TICKS_PER_MS && min != 59 && min != 0)) {
                        Save_Set(time_delta, s_trip.clock_delta);
                    }
                }
            }
            break;
        case MP_M_ARRIVED:
            if (mp_lobby_guest_slot(conn) >= 0) {
                mp_lobby_guest_mark(conn, TRUE, FALSE);
                mp_world_on_arrived(conn);
                mp_player_on_arrived(conn);
                mp_npc_on_arrived(conn);
                mp_event_on_arrived(conn);
            }
            break;
        case MP_M_PULLING_IN:
            // its town data came over during the ride; the train is what's on screen now
            if (mp_lobby_guest_arrived(mp_lobby_guest_slot(conn) + 1)) {
                u8 name[MP_NAME_LEN];
                u8 town[MP_NAME_LEN];

                mp_trip_train(conn);
                if (mp_lobby_guest_names(conn, name, town)) {
                    mp_notice_push(MP_MSG_N_ARRIVED, town, name);
                }
            }
            break;
        case MP_M_LEAVING:
            if (mp_lobby_guest_slot(conn) >= 0) {
                u8 name[MP_NAME_LEN];
                u8 town[MP_NAME_LEN];

                mp_world_on_rel(conn, data, len); // after its ops, in order
                mp_npc_on_guest_gone(mp_lobby_guest_slot(conn) + 1);
                mp_lobby_guest_mark(conn, FALSE, TRUE);
                if (mp_lobby_guest_names(conn, name, town)) {
                    mp_notice_push(MP_MSG_N_LEFT, town, name);
                }
            }
            break;
        case MP_M_CLOSING:
            if (len >= 2 && conn == mp_lobby_host_conn()) {
                s_trip.closing = TRUE;
                s_trip.closing_deadline_ms = pc_mp_now_ms() + (unsigned int)data[1] * 1000u;
                if (s_trip.state == MP_TRAVEL_VISITING) {
                    mp_notice_push(MP_MSG_N_LAST_TRAIN, NULL, NULL);
                }
            }
            break;
        case MP_M_TRAIN:
            if (len >= 2 && conn == mp_lobby_host_conn() && s_trip.state == MP_TRAVEL_VISITING && data[1]) {
                aSTM_mp_arriving();
            }
            break;
        case MP_M_TRAIN_STATE:
            if (len >= MP_TRAIN_LEN) {
                mp_train_heard(conn, data);
            }
            break;
        case MP_M_RULES:
            if (len >= 2 && conn == mp_lobby_host_conn()) {
                int n = data[1] < MP_RULE_NUM ? data[1] : MP_RULE_NUM;

                if (n > len - 2) {
                    n = len - 2;
                }
                memcpy(s_rules, data + 2, (size_t)n);
            }
            break;
    }
}

void mp_travel_on_bulk(const mp_event_t* ev) {
    int size;

    if (s_trip.state != MP_TRAVEL_FETCHING || ev->conn != mp_lobby_host_conn()) {
        return;
    }
    if (ev->bulk_xfer != s_trip.xfer) {
        // chunks can beat the size header; restart assembly for a new transfer
        mp_free(s_trip.comp);
        s_trip.comp = NULL;
        s_trip.xfer = ev->bulk_xfer;
        s_trip.count = 0;
        s_trip.chunks = 0;
        s_trip.bytes = 0;
        memset(s_trip.seen, 0, sizeof(s_trip.seen));
    }
    if (s_trip.comp == NULL) {
        size = ev->bulk_count * MP_BULK_CHUNK;
        s_trip.comp = (u8*)mp_alloc(size);
        if (s_trip.comp == NULL) {
            mp_trip_fail("no memory");
            return;
        }
        s_trip.count = ev->bulk_count;
    }
    if (ev->bulk_count != s_trip.count || ev->bulk_idx >= s_trip.count ||
        (s_trip.seen[ev->bulk_idx >> 3] & (1 << (ev->bulk_idx & 7)))) {
        return;
    }
    s_trip.seen[ev->bulk_idx >> 3] |= (u8)(1 << (ev->bulk_idx & 7));
    memcpy(s_trip.comp + ev->bulk_idx * MP_BULK_CHUNK, ev->data, ev->len);
    s_trip.chunks++;
    s_trip.bytes += ev->len;
    mp_trip_check_done();
}

void mp_travel_on_bulk_sent(int conn) {
    if (s_snap.stage == MP_SNAP_SENDING && s_snap.conn == conn) {
        mp_lobby_guest_boarding(conn);
        s_snap.stage = MP_SNAP_IDLE; // fully acknowledged: nothing left to cancel
        mp_snap_next();
    }
}

void mp_travel_on_guest_gone(int conn) {
    mp_snap_forget(conn);
}

void mp_travel_on_host_gone(void) {
    switch (s_trip.state) {
        case MP_TRAVEL_FETCHING:
        case MP_TRAVEL_READY:
            mp_trip_fail("line cut");
            break;
        case MP_TRAVEL_DEPARTED:
        case MP_TRAVEL_VISITING:
            // the town is local now; head home at the next quiet moment
            if (s_trip.trouble != 0) {
                mp_notice_cancel(s_trip.trouble); // the goodbye below says it all
                s_trip.trouble = 0;
            }
            if (!s_trip.force_return && !s_trip.gave_up) {
                s_trip.force_return = TRUE;
                s_trip.force_notice = s_trip.closing ? MP_MSG_N_LINE_CLOSED : MP_MSG_N_HOST_CUT;
            }
            break;
        default:
            break;
    }
}

// the host went silent past the usual timeout; the line is held a while longer
void mp_travel_on_host_away(void) {
    if (s_trip.state == MP_TRAVEL_VISITING && !s_trip.force_return && s_trip.trouble == 0) {
        s_trip.trouble = mp_notice_push(MP_MSG_N_LINE_TROUBLE, NULL, NULL);
    }
}

void mp_travel_on_host_back(void) {
    if (s_trip.trouble == 0) {
        return;
    }
    // the all-clear only follows a trouble call that was actually heard
    if (!mp_notice_cancel(s_trip.trouble) && s_trip.state == MP_TRAVEL_VISITING && !s_trip.force_return) {
        mp_notice_push(MP_MSG_N_LINE_BACK, NULL, NULL);
    }
    s_trip.trouble = 0;
}

// ticks

void mp_travel_host_tick(unsigned int now_ms) {
    if (!mp_is_host()) {
        return;
    }
    if (s_snap.stage == MP_SNAP_PACKING) {
        mp_snap_pack_step();
    }
    if (s_snap.stage == MP_SNAP_SENDING) {
        mp_snap_send_step();
    }
    if (Save_Get(rainbow_reserved) && Save_Get(rainbow_month) == Common_Get(time.rtc_time.month) &&
        Save_Get(rainbow_day) == Common_Get(time.rtc_time.day)) {
        s_rb_host_day = mp_rb_today();
    }
    if (now_ms - s_snap.last_time_ms >= MP_TIME_PERIOD_MS) {
        s_snap.last_time_ms = now_ms;
        mp_send_time(-1);
    }
    {
        u8 mine[MP_RULE_NUM];

        // a switch changed while hosting
        mp_rules_mine(mine);
        if (memcmp(mine, s_rules_sent, MP_RULE_NUM) != 0) {
            mp_send_rules(-1);
        }
    }
}

// the host is closing: every visitor hears the last-train call
void mp_travel_host_close(void) {
    u8 msg[2] = { MP_M_CLOSING, MP_CLOSING_GRACE_S };

    mp_lobby_send_rel_all(msg, sizeof(msg));
    s_snap.queued = 0;
    mp_snap_next();
}

static int mp_scene_out_of_town(int scene) {
    return scene == SCENE_PLAYERSELECT || scene == SCENE_PLAYERSELECT_2 || scene == SCENE_PLAYERSELECT_3 ||
           scene == SCENE_PLAYERSELECT_SAVE || scene == SCENE_TITLE_DEMO;
}

// the host in an NES game: a joiner's snapshot is taken from the save as it stands
void mp_travel_headless(void) {
    if (mp_is_host() && s_snap.stage == MP_SNAP_WANTED) {
        mp_snap_capture(NULL);
    }
}

void pc_mp_play_frame(GAME_PLAY* play) {
    mp_player_frame(play);
    pc_mp_chat_move(play);
    mp_aero_frame();
    if (mp_world_still()) {
        mp_npc_still(play);
        mp_cr_still(play);
    }
    mp_world_frame(play);
    if (mp_is_host()) {
        // saved and quit: the town stops running, so its visitors head home now
        if (mp_scene_out_of_town(Save_Get(scene_no))) {
            mp_lobby_host_hangup();
            return;
        }
        if (s_snap.stage == MP_SNAP_WANTED) {
            mp_snap_capture(play);
        }
    }
    if (s_trip.state == MP_TRAVEL_VISITING) {
        if (s_trip.pull_in == 1) {
            u8 msg[1] = { MP_M_PULLING_IN };
            int conn = mp_lobby_host_conn();

            if (conn >= 0 && mp_lobby_send_rel(conn, msg, 1)) {
                s_trip.pull_in = 2;
            }
        }
        if (s_trip.closing && mp_travel_host_closing_left() == 0 && !s_trip.force_return && !s_trip.gave_up) {
            s_trip.force_return = TRUE;
            s_trip.force_notice = MP_MSG_N_LINE_CLOSED;
        }
        if (s_trip.force_return) {
            // the announcement first, then the train
            if (!s_trip.noticed) {
                mp_notice_push(s_trip.force_notice, NULL, NULL);
                s_trip.noticed = TRUE;
            }
            if (!mp_notice_busy()) {
                mp_trip_force_home(play);
            }
        }
    } else if (s_trip.state == MP_TRAVEL_RETURNING && s_trip.boarding) {
        // home is already loaded to ride; any scene change now carries the player there
        if (play->fb_wipe_mode == WIPE_MODE_NONE && mp_trip_board_train(play)) {
            s_trip.boarding = FALSE;
        }
    }
    mp_notice_tick(play);
}

#endif
