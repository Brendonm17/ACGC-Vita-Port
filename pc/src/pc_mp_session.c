// pc_mp_session.c - multiplayer session core: role, build id, log, ticket codec, frame hooks
#include "pc_mp.h"

#ifdef VITA_MP

#include "dolphin/os.h"
#include "m_common_data.h"
#include "m_card.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char* volatile g_mp_crumb = "boot";
volatile unsigned int g_mp_frame;

static mp_role_t s_role = MP_ROLE_NONE;

int mp_active(void) { return s_role != MP_ROLE_NONE; }
int mp_is_host(void) { return s_role == MP_ROLE_HOST; }
int mp_is_guest(void) { return s_role == MP_ROLE_GUEST; }
mp_role_t mp_role(void) { return s_role; }

void mp_set_role(mp_role_t role) {
    if (role != s_role) {
        if (s_role == MP_ROLE_NONE) {
            mp_npc_reset();
            mp_cr_reset();
        }
        if (role == MP_ROLE_NONE) {
            mp_player_reset();
            mp_npc_reset();
            mp_event_reset();
            mp_cr_reset();
            mp_mod_reset();
        }
        s_role = role;
    }
}

// FNV-1a over everything two peers must agree on byte for byte
unsigned int pc_mp_build_id(void) {
    unsigned int layout[7] = {
        MP_PROTO_VERSION, sizeof(Save_t), sizeof(Private_c), sizeof(Animal_c),
        sizeof(mCD_keep_original_c), sizeof(common_data_t), 0,
    };
    const unsigned char* p = (const unsigned char*)layout;
    unsigned int h = 2166136261u;
    unsigned int i;

    // dialogue ids are part of the protocol (they cross the wire in refusals)
    layout[6] = pc_mp_text_hash();
    for (i = 0; i < sizeof(layout); i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

// errors only, one line each, into the game's one log (error.log); any thread
void pc_mp_log(const char* fmt, ...) {
    char line[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    pc_log_error("%s\n", line);
}

// ticket codec

// no vowels, so the name-entry bad-word filter can never match a ticket
static const char s_ticket_alpha[] = "0123456789BCDFGHJKLMNPQRSTVWXZ";
#define MP_TICKET_BASE 30

static unsigned int mp_ticket_check(unsigned int ip, int port_idx) {
    unsigned char bytes[5];
    unsigned int crc = 0;
    int i;
    int b;

    bytes[0] = (unsigned char)(ip >> 24);
    bytes[1] = (unsigned char)(ip >> 16);
    bytes[2] = (unsigned char)(ip >> 8);
    bytes[3] = (unsigned char)ip;
    bytes[4] = (unsigned char)port_idx;
    for (i = 0; i < 5; i++) {
        crc ^= bytes[i];
        for (b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? ((crc << 1) ^ 0x07) & 0xFF : (crc << 1) & 0xFF;
        }
    }
    return crc & 31;
}

void mp_ticket_encode(unsigned int ip, int port_idx, unsigned char* out8) {
    unsigned long long v = ((unsigned long long)ip << 7) | ((unsigned)(port_idx & 3) << 5) |
                           mp_ticket_check(ip, port_idx & 3);
    int i;

    for (i = MP_TICKET_LEN - 1; i >= 0; i--) {
        out8[i] = (unsigned char)s_ticket_alpha[v % MP_TICKET_BASE];
        v /= MP_TICKET_BASE;
    }
}

// accepts any case and spacing, and the O/0 and I/1 mix-ups
int mp_ticket_decode(const unsigned char* text, int len, unsigned int* ip, int* port_idx) {
    unsigned long long v = 0;
    int digits = 0;
    int i;

    for (i = 0; i < len; i++) {
        int c = text[i];
        const char* hit;

        if (c == ' ') {
            continue;
        }
        if (c >= 'a' && c <= 'z') {
            c -= 'a' - 'A';
        }
        if (c == 'O') {
            c = '0';
        } else if (c == 'I') {
            c = '1';
        }
        hit = (c != 0) ? strchr(s_ticket_alpha, c) : NULL;
        if (hit == NULL || digits == MP_TICKET_LEN) {
            return FALSE;
        }
        v = v * MP_TICKET_BASE + (unsigned long long)(hit - s_ticket_alpha);
        digits++;
    }
    if (digits != MP_TICKET_LEN || (v >> 39) != 0) {
        return FALSE;
    }
    *ip = (unsigned int)(v >> 7);
    *port_idx = (int)((v >> 5) & 3);
    return mp_ticket_check(*ip, *port_idx) == (unsigned int)(v & 31);
}

// frame hooks

static int s_emu;           // this game is in an NES game: no play game runs meanwhile
static u32 s_headless_ms;

void pc_mp_emu_enter(void) {
    s_emu = TRUE;
}

void pc_mp_emu_leave(void) {
    s_emu = FALSE;
}

int mp_emu_active(void) {
    return s_emu;
}

// runs every frame and through loading loops, so keepalives never stall; in an NES game the host keeps serving its
// town from here (at half the NES's frame rate), and a visitor keeps taking the host's word and standing there
void pc_mp_pump(void) {
    unsigned int now = pc_mp_now_ms();

    g_mp_frame++;
    if (mp_link_running()) {
        MP_CRUMB("link");
        mp_link_tick(now);
    }
    MP_CRUMB("lobby");
    mp_lobby_tick(now);
    if (s_emu && mp_is_host() && now - s_headless_ms >= 30) {
        s_headless_ms = now;
        MP_CRUMB("headless");
        mp_host_headless_tick();
    } else if (s_emu && mp_is_guest() && now - s_headless_ms >= 30) {
        s_headless_ms = now;
        MP_CRUMB("headless");
        mp_player_frame(NULL);
        mp_world_drain();
    }
    MP_CRUMB("mods");
    mp_mod_tick();
    MP_CRUMB("game");
}

void pc_mp_frame_begin(void) {
}

void pc_mp_on_app_exit(void) {
    if (mp_travel_net_trip()) {
        mp_lobby_pump();  // (the line's word first: after a long sleep it's long gone)
        mp_world_drain(); // (a commit already in keeps its pickups)
        if (!mp_world_exit()) {
            mp_passport_write();
        }
    }
    mp_lobby_shutdown();
}

void pc_mp_on_resume(int certain) {
    mp_lobby_on_resume(certain);
}

void pc_mp_frame_end(void) {
}

#endif
