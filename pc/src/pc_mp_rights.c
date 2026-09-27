// pc_mp_rights.c
// visitors doing what residents do (the host's switch): the museum takes their donations, noted by name
#include "pc_mp.h"

#ifdef VITA_MP

#include "m_common_data.h"
#include "m_house.h"
#include "m_mail.h"
#include "m_event.h"
#include "m_museum.h"
#include "m_museum_display.h"
#include "m_name_table.h"
#include "m_private.h"
#include "m_room_type.h"
#include "lb_rtc.h"
#include "dolphin/os.h"

#include <stddef.h>
#include <string.h>

#define MP_DONOR_NUM (mMmd_FOSSIL_NUM + mMmd_ART_NUM + mMmd_INSECT_NUM + mMmd_FISH_NUM)
#define MP_MD_ROSTER 15 // visitors who gave pieces, each a 4-bit number

// the museum's visitor donors ride in the save's spare tail (the bytes after saved_auto_nwrite_time, which the game
// never reads or writes): a tag, each piece's donor as a roster number, and the roster
typedef struct {
    u8 name[PLAYER_NAME_LEN];
    u8 player_id[2]; // high byte first, as on the card
    u8 land_id[2];
} mp_md_who_t;

typedef struct {
    u8 tag[4];
    u8 donor[(MP_DONOR_NUM + 1) / 2]; // 0: none noted, else a roster number
    mp_md_who_t roster[MP_MD_ROSTER];
} mp_md_tail_t;

typedef char mp_md_tail_fits[(sizeof(mp_md_tail_t) <= sizeof(((Save_t*)0)->_241A8)) ? 1 : -1];

static const u8 s_md_tag[4] = { 'M', 'P', 'M', 'D' };

static const u8 s_cat_num[mMmd_CATEGORY_NUM] = { mMmd_FOSSIL_NUM, mMmd_ART_NUM, mMmd_INSECT_NUM, mMmd_FISH_NUM };

static u8 s_home_bank;   // the traveller's own house opens savings back home
static s64 s_home_delta; // the home town's clock, against the hardware's

int mp_visitor_rights(void) {
    return mp_travel_state() == MP_TRAVEL_VISITING && mp_rule(MP_RULE_VISITOR_RIGHTS);
}

int mp_visitor_may(int rule) {
    return mp_travel_state() != MP_TRAVEL_VISITING || mp_rule(rule);
}

// network departure, the home town still loaded: what it allows the traveller that a visit can't look up
void mp_rights_departing(void) {
    int no = Common_Get(player_no);

    s_home_bank = FALSE;
    s_home_delta = Save_Get(time_delta);
    if (no < PLAYER_NUM) {
        mHm_rmsz_c* size_info = &Save_Get(homes)[mHS_get_arrange_idx(no)].size_info;

        s_home_bank = size_info->size >= 3 && size_info->renew == FALSE;
    }
}

// visitor: savings at the host's post office, as their own house allows at home
int mp_rights_home_bank(void) {
    return mp_visitor_rights() && s_home_bank;
}

// visitor: the host's town is on the same day as the traveller's own (a card dated elsewhere would be lost there)
int mp_rights_same_day(void) {
    OSCalendarTime ct;
    lbRTC_time_c* now = Common_GetPointer(time.rtc_time);

    OSTicksToCalendarTime(lbRTC_HardTime() + s_home_delta, &ct);
    return ct.year == now->year && ct.mon + 1 == now->month && ct.mday == now->day;
}

// passport marks, in the passport's spare bytes (never read by the game): each stamped with the town and the day,
// so one from anywhere else reads as nothing

#define MP_MARK_BYTES 7 // town id (2), a sum of its name, year, month, day; then the flags

typedef char mp_mark_fits[(MP_MARK_NUM * MP_MARK_BYTES <= sizeof(((Private_c*)0)->unused_2412)) ? 1 : -1];

static void mp_mark_stamp(u8* s) {
    lbRTC_time_c* now = Common_GetPointer(time.rtc_time);
    u8 sum = 0;
    int i;

    for (i = 0; i < LAND_NAME_SIZE; i++) {
        sum = (u8)(sum * 31 + Save_Get(land_info).name[i]);
    }
    s[0] = (u8)(Save_Get(land_info).id >> 8);
    s[1] = (u8)Save_Get(land_info).id;
    s[2] = sum;
    s[3] = (u8)now->year;
    s[4] = (u8)now->month;
    s[5] = (u8)now->day;
}

static u8* mp_mark_at(int kind) {
    if (kind < 0 || kind >= MP_MARK_NUM || Now_Private == NULL || mp_travel_state() != MP_TRAVEL_VISITING) {
        return NULL;
    }
    return Now_Private->unused_2412 + kind * MP_MARK_BYTES;
}

int mp_mark_get(int kind) {
    u8* m = mp_mark_at(kind);
    u8 stamp[MP_MARK_BYTES - 1];

    if (m == NULL) {
        return 0;
    }
    mp_mark_stamp(stamp);
    return memcmp(m, stamp, sizeof(stamp)) == 0 ? m[MP_MARK_BYTES - 1] : 0;
}

void mp_mark_set(int kind, int flags) {
    u8* m = mp_mark_at(kind);
    u8 stamp[MP_MARK_BYTES - 1];

    if (m == NULL) {
        return;
    }
    mp_mark_stamp(stamp);
    if (memcmp(m, stamp, sizeof(stamp)) != 0) {
        memcpy(m, stamp, sizeof(stamp));
        m[MP_MARK_BYTES - 1] = 0;
    }
    m[MP_MARK_BYTES - 1] |= (u8)flags;
}

void mp_mark_clr(int kind, int flags) {
    u8* m = mp_mark_at(kind);
    u8 stamp[MP_MARK_BYTES - 1];

    if (m == NULL) {
        return;
    }
    mp_mark_stamp(stamp);
    if (memcmp(m, stamp, sizeof(stamp)) == 0) {
        m[MP_MARK_BYTES - 1] &= (u8)~flags;
    }
}

// Wisp: who met him tonight (the host's player by its resident bit, a visitor by its slot's note), from the town's
// record as renewed today
static mEv_gst_c* mp_wisp_record(void) {
    mEv_gst_c* gst = (mEv_gst_c*)mEv_mp_real_area(mEv_EVENT_GHOST, 54);
    lbRTC_time_c* now = Common_GetPointer(time.rtc_time);

    if (!mp_is_host() || gst == NULL || gst->renew_time.year != now->year || gst->renew_time.month != now->month ||
        gst->renew_time.day != now->day) {
        return NULL;
    }
    return gst;
}

int mp_rights_wisp_met(int slot) {
    mEv_gst_c* gst = mp_wisp_record();
    int no = Common_Get(player_no);

    if (gst == NULL) {
        return FALSE;
    }
    if (slot == 0) {
        return no < PLAYER_NUM && (gst->flags & (1 << no)) != 0;
    }
    return slot < MP_MAX_PEERS && mp_rule(MP_RULE_VISITOR_RIGHTS) && mp_lobby_guest_arrived(slot) &&
           (gst->flags & mEv_GHOST_FLAG_MP_GUEST(slot)) != 0;
}

// host: a visitor new to that slot hasn't met Wisp tonight, whoever had the slot before
void mp_rights_slot_fresh(int slot) {
    mEv_gst_c* gst = (mEv_gst_c*)mEv_mp_real_area(mEv_EVENT_GHOST, 54);

    if (mp_is_host() && gst != NULL && slot >= 1 && slot < MP_MAX_PEERS) {
        gst->flags &= (u16)~mEv_GHOST_FLAG_MP_GUEST(slot);
    }
}

int mp_rights_wisp_hunters(void) {
    int n = 0;
    int s;

    for (s = 0; s < MP_MAX_PEERS; s++) {
        n += mp_rights_wisp_met(s) != 0;
    }
    return n;
}

// visitor: a prize letter a resident finds in the mailbox (the passport has none) comes into the pouch, else its
// present into the pockets; with both full it's missed, as with a full post office desk. The mark goes in with it.
int pc_mp_prize_mail(void* mail_p, int mark, int flags) {
    Mail_c* mail = (Mail_c*)mail_p;
    int idx;

    if (Now_Private == NULL) {
        return FALSE;
    }
    idx = mMl_chk_mail_free_space(Now_Private->mail, mPr_INVENTORY_MAIL_COUNT);
    if (idx >= 0) {
        mMl_copy_mail(&Now_Private->mail[idx], mail);
    } else {
        int ok;

        // (a talk under way keeps it too: it isn't the talk's)
        mp_world_inv_mark();
        ok = mail->present != EMPTY_NO && mPr_SetFreePossessionItem(Now_Private, mail->present, mPr_ITEM_COND_PRESENT);
        mp_world_inv_follow();
        if (!ok) {
            return FALSE;
        }
    }
    if (mark >= 0) {
        mp_mark_set(mark, flags);
    }
    mp_passport_write_async();
    return TRUE;
}

// the museum

static int mp_md_slot(int cat, int idx) {
    int base = 0;
    int c;

    if (cat < 0 || cat >= mMmd_CATEGORY_NUM || idx < 0 || idx >= s_cat_num[cat]) {
        return -1;
    }
    for (c = 0; c < cat; c++) {
        base += s_cat_num[c];
    }
    return base + idx;
}

static int mp_md_value(int cat, int idx) {
    switch (cat) {
        case mMmd_CATEGORY_FOSSIL:
            return mMmd_FossilInfo(idx);
        case mMmd_CATEGORY_ART:
            return mMmd_ArtInfo(idx);
        case mMmd_CATEGORY_INSECT:
            return mMmd_InsectInfo(idx);
        default:
            return mMmd_FishInfo(idx);
    }
}

static mActor_name_t mp_md_item(int cat, int idx) {
    switch (cat) {
        case mMmd_CATEGORY_FOSSIL:
            return (mActor_name_t)(FTR_DINO_START + FTR_NO_2_IDX(idx));
        case mMmd_CATEGORY_ART:
            return (mActor_name_t)(FTR_START(FTR_SUM_ART01) + FTR_NO_2_IDX(idx));
        case mMmd_CATEGORY_INSECT:
            return (mActor_name_t)(ITM_INSECT_START + idx);
        default:
            return (mActor_name_t)(ITM_FISH_START + idx);
    }
}

// the exhibit one half of a byte of the museum's record holds
static int mp_md_exhibit(u32 at, int half, int* cat, int* idx) {
    static const struct {
        u8 cat;
        u8 first;
        u8 bytes;
    } map[] = {
        { mMmd_CATEGORY_FOSSIL, offsetof(mMmd_info_c, fossil_bit), mMmd_FOSSIL_BIT_NUM },
        { mMmd_CATEGORY_ART, offsetof(mMmd_info_c, art_bit), mMmd_ART_BIT_NUM },
        { mMmd_CATEGORY_FISH, offsetof(mMmd_info_c, fish_bit), mMmd_FISH_BIT_NUM },
        { mMmd_CATEGORY_INSECT, offsetof(mMmd_info_c, insect_bit), mMmd_INSECT_BIT_NUM },
    };
    int m;

    for (m = 0; m < (int)(sizeof(map) / sizeof(map[0])); m++) {
        if (at >= map[m].first && at < (u32)(map[m].first + map[m].bytes)) {
            *cat = map[m].cat;
            *idx = (int)(at - map[m].first) * 2 + half;
            return *idx < s_cat_num[*cat];
        }
    }
    return FALSE;
}

static mp_md_tail_t* mp_md_tail(void) {
    return (mp_md_tail_t*)Save_Get(_241A8);
}

static int mp_md_tagged(const mp_md_tail_t* t) {
    return memcmp(t->tag, s_md_tag, sizeof(s_md_tag)) == 0;
}

static int mp_md_same(const mp_md_who_t* w, const PersonalID_c* pid) {
    return memcmp(w->name, pid->player_name, PLAYER_NAME_LEN) == 0 &&
           ((w->player_id[0] << 8) | w->player_id[1]) == pid->player_id &&
           ((w->land_id[0] << 8) | w->land_id[1]) == pid->land_id;
}

// the roster entry noted for a piece, while the save still holds it as a former resident's
static const mp_md_who_t* mp_md_who(int cat, int idx) {
    const mp_md_tail_t* t = mp_md_tail();
    int k = mp_md_slot(cat, idx);
    int r;

    if (k < 0 || !mp_md_tagged(t) || mp_md_value(cat, idx) != mMmd_DONATOR_DELETED_PLAYER) {
        return NULL;
    }
    r = (t->donor[k >> 1] >> ((k & 1) * 4)) & 0xF;
    return (r >= 1 && r <= MP_MD_ROSTER) ? &t->roster[r - 1] : NULL;
}

// a visitor's donation (the save holds it as a former resident's): TRUE, with their name
int pc_mp_museum_donor(int cat, int idx, unsigned char* name) {
    const mp_md_who_t* w = mp_md_who(cat, idx);

    if (w == NULL) {
        return FALSE;
    }
    if (name != NULL) {
        memcpy(name, w->name, PLAYER_NAME_LEN);
    }
    return TRUE;
}

// ...and it was this player's
int pc_mp_museum_mine(int cat, int idx) {
    const mp_md_who_t* w = mp_md_who(cat, idx);

    return w != NULL && Now_Private != NULL && mp_md_same(w, &Now_Private->player_ID);
}

// a visitor's place on the roster, a newcomer taking a free one; -1 with none left
static int mp_md_place(mp_md_tail_t* t, const PersonalID_c* pid) {
    static const u8 none[PLAYER_NAME_LEN] = { 0 };
    int r;

    for (r = 0; r < MP_MD_ROSTER; r++) {
        if (mp_md_same(&t->roster[r], pid)) {
            return r;
        }
    }
    for (r = 0; r < MP_MD_ROSTER; r++) {
        if (memcmp(t->roster[r].name, none, PLAYER_NAME_LEN) == 0) {
            memcpy(t->roster[r].name, pid->player_name, PLAYER_NAME_LEN);
            t->roster[r].player_id[0] = (u8)(pid->player_id >> 8);
            t->roster[r].player_id[1] = (u8)pid->player_id;
            t->roster[r].land_id[0] = (u8)(pid->land_id >> 8);
            t->roster[r].land_id[1] = (u8)pid->land_id;
            return r;
        }
    }
    return -1;
}

// host: the visitor who gave a piece is noted in the save's tail (with the roster full the piece just reads as a
// former resident's), which goes out to everyone like any change of the host's
static void mp_md_note(int cat, int idx, int slot) {
    mp_md_tail_t* t = mp_md_tail();
    PersonalID_c pid;
    int k = mp_md_slot(cat, idx);
    int r;

    if (k < 0 || !mp_lobby_guest_pid(mp_lobby_guest_conn(slot), &pid)) {
        return;
    }
    if (!mp_md_tagged(t)) {
        memset(t, 0, sizeof(*t));
        memcpy(t->tag, s_md_tag, sizeof(s_md_tag));
    }
    r = mp_md_place(t, &pid);
    if (r < 0) {
        return;
    }
    t->donor[k >> 1] = (u8)((t->donor[k >> 1] & ~(0xF << ((k & 1) * 4))) | ((r + 1) << ((k & 1) * 4)));
    mp_world_host_took(offsetof(Save_t, _241A8), sizeof(*t), -1);
}

// host: the museum bits a visitor's talk with Blathers flipped: an empty spot takes their donation (noted as theirs);
// one someone filled first hands their piece back through the lost and found
void mp_rights_museum_run(int from, unsigned int off, const unsigned char* bytes, unsigned int n) {
    u32 base = offsetof(Save_t, museum_display);
    u8* disp = (u8*)Save_GetPointer(museum_display);
    u32 k;

    for (k = 0; k < n; k++) {
        u32 at = off + k - base;
        int changed = FALSE;
        int h;

        if (off + k < base || at >= sizeof(mMmd_info_c)) {
            continue;
        }
        for (h = 0; h < 2; h++) {
            int theirs = (bytes[k] >> (h * 4)) & 0xF;
            int here = (disp[at] >> (h * 4)) & 0xF;
            int cat;
            int idx;

            // (a visitor's donation can only read as a former resident's, the save's own value for one)
            if (theirs != mMmd_DONATOR_DELETED_PLAYER || !mp_md_exhibit(at, h, &cat, &idx)) {
                continue;
            }
            if (here != 0) {
                mp_world_keep(mp_md_item(cat, idx));
                continue;
            }
            disp[at] |= (u8)(theirs << (h * 4));
            changed = TRUE;
            mp_md_note(cat, idx, from);
        }
        if (changed) {
            mp_world_host_took(base + at, 1, from);
        }
    }
    // a visitor's last piece finishes the museum for the town's residents
    mMsm_SetCompMail();
}

#endif
