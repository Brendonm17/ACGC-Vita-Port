// pc_mp_world.c
// the shared town: cells change through ops the host judges; the rest of the save streams out
#include "pc_mp.h"

#ifdef VITA_MP

#include "m_common_data.h"
#include "ac_my_room.h"
#include "ac_npc_shop_common.h"
#include "ac_shop_manekin.h"
#include "ac_shop_umbrella.h"
#include "m_actor.h"
#include "m_demo.h"
#include "m_notice.h"
#include "m_font.h"
#include "ac_shop_goods_h.h"
#include "m_collision_bg.h"
#include "m_field_info.h"
#include "m_field_make.h"
#include "m_house.h"
#include "m_island.h"
#include "m_needlework.h"
#include "m_npc.h"
#include "m_room_type.h"
#include "m_scene_table.h"
#include "m_name_table.h"
#include "m_ftr_def.h"
#include "m_play.h"
#include "m_player_lib.h"
#include "m_police_box.h"
#include "m_private.h"
#include "m_shop.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// cell space
// every town cell a player can change, as one array of u16 values

enum {
    MP_RG_FG,
    MP_RG_IFG,
    MP_RG_POLICE,
    MP_RG_DEP,
    MP_RG_IDEP,
    MP_RG_HANIWA, // what each house's gyroid has out for sale
    MP_RG_COT,    // the island cottage's four item layers
    MP_RG_CMUS,   // its music box: a cell per record, holding the record while it's in
    MP_RG_CWF,    // its carpet and wallpaper, as the items they were laid from
    MP_RG_NUM,
};

enum {
    MP_MAP_NONE,
    MP_MAP_DISC, // a bit per record
    MP_MAP_WF,   // an index per carpet and wallpaper
};

#define MP_FG_WORDS   (FG_BLOCK_Z_NUM * FG_BLOCK_X_NUM * UT_TOTAL_NUM)
#define MP_IFG_WORDS  (mISL_FG_BLOCK_Z_NUM * mISL_FG_BLOCK_X_NUM * UT_TOTAL_NUM)
#define MP_POL_WORDS  mPB_POLICE_BOX_ITEM_STORAGE_COUNT
#define MP_DEP_WORDS  (FG_BLOCK_X_NUM * FG_BLOCK_Z_NUM * UT_Z_NUM)
#define MP_IDEP_WORDS (mISL_FG_BLOCK_X_NUM * mISL_FG_BLOCK_Z_NUM * UT_Z_NUM)
#define MP_HNW_WORDS  (PLAYER_NUM * HANIWA_ITEM_HOLD_NUM)
#define MP_HNW_BASE   (MP_FG_WORDS + MP_IFG_WORDS + MP_POL_WORDS + (MP_DEP_WORDS + MP_IDEP_WORDS) * 16)
#define MP_COT_WORDS  (4 * UT_TOTAL_NUM)
#define MP_COT_BASE   (MP_HNW_BASE + MP_HNW_WORDS)
#define MP_CMUS_BASE  (MP_COT_BASE + MP_COT_WORDS)
#define MP_CWF_BASE   (MP_CMUS_BASE + MINIDISK_NUM)
#define MP_CELLS      (MP_CWF_BASE + 2)
#define MP_CELL_PLAIN 0x8000 // an op's cell whose change moved nothing between a pocket and the town
#define MP_CELL_PAGE  128
#define MP_CELL_PAGES ((MP_CELLS + MP_CELL_PAGE - 1) / MP_CELL_PAGE)

typedef struct {
    u32 off; // Save_t offset of the u16 words
    int words;
    int bits; // one cell per bit of each word (buried-item flags)
    int base;
    u32 stride;       // bytes between words, 0 for packed ones (a gyroid's slots)
    int group;        // ...in groups of this many,
    u32 group_stride; // each this far apart (one per house)
    int map;          // cells read from the save another way
} mp_crgn_t;

typedef char mp_cells_fit[(MP_CELLS < MP_CELL_PLAIN) ? 1 : -1];

static mp_crgn_t s_rg[MP_RG_NUM] = {
    { offsetof(Save_t, fg), MP_FG_WORDS, 0, 0 },
    { offsetof(Save_t, island.fgblock), MP_IFG_WORDS, 0, MP_FG_WORDS },
    { offsetof(Save_t, police_box.keep_items), MP_POL_WORDS, 0, MP_FG_WORDS + MP_IFG_WORDS },
    { offsetof(Save_t, deposit), MP_DEP_WORDS, 1, MP_FG_WORDS + MP_IFG_WORDS + MP_POL_WORDS },
    { offsetof(Save_t, island.deposit), MP_IDEP_WORDS, 1,
      MP_FG_WORDS + MP_IFG_WORDS + MP_POL_WORDS + MP_DEP_WORDS * 16 },
    { offsetof(Save_t, homes) + offsetof(mHm_hs_c, haniwa) + offsetof(Haniwa_c, items) + offsetof(Haniwa_Item_c, item),
      MP_HNW_WORDS, 0, MP_HNW_BASE, sizeof(Haniwa_Item_c), HANIWA_ITEM_HOLD_NUM, sizeof(mHm_hs_c) },
    { offsetof(Save_t, island.cottage.room.layer_main.items), MP_COT_WORDS, 0, MP_COT_BASE, sizeof(mActor_name_t),
      UT_TOTAL_NUM, sizeof(mHm_lyr_c) },
    { offsetof(Save_t, island.cottage.music_box), MINIDISK_NUM, 0, MP_CMUS_BASE, 0, 0, 0, MP_MAP_DISC },
    { offsetof(Save_t, island.cottage.room.wall_floor), 2, 0, MP_CWF_BASE, 0, 0, 0, MP_MAP_WF },
};

static u8* s_base; // host: a joiner's snapshot, read in place of the live save

static u8* mp_save_base(void) {
    return s_base != NULL ? s_base : (u8*)&common_data.save.save;
}

static int mp_rg_of(int cell) {
    int r;

    for (r = MP_RG_NUM - 1; r > 0; r--) {
        if (cell >= s_rg[r].base) {
            break;
        }
    }
    return r;
}

// the bells a gyroid asks for the item in this slot (0 for any other cell)
static u32 mp_hnw_price(int cell) {
    int k = cell - MP_HNW_BASE;

    if (k < 0 || k >= MP_HNW_WORDS) {
        return 0;
    }
    return Save_Get(homes[k / HANIWA_ITEM_HOLD_NUM]).haniwa.items[k % HANIWA_ITEM_HOLD_NUM].extra_data;
}

static u16* mp_word(int r, int w) {
    if (s_rg[r].stride != 0) {
        return (u16*)(mp_save_base() + s_rg[r].off + (u32)(w / s_rg[r].group) * s_rg[r].group_stride +
                      (u32)(w % s_rg[r].group) * s_rg[r].stride);
    }
    return (u16*)(mp_save_base() + s_rg[r].off) + w;
}

// the cottage's music box and floor as items: a record while it's in the box, the carpet and wallpaper laid (a
// design laid there is no item)
static u16 mp_map_get(int r, int k) {
    mHm_cottage_c* cot = (mHm_cottage_c*)(mp_save_base() + offsetof(Save_t, island.cottage));

    if (s_rg[r].map == MP_MAP_DISC) {
        return ChkMusicBox(cot->music_box, k) ? (u16)(ITM_MINIDISK_START + k) : EMPTY_NO;
    }
    if (k == 0) {
        return cot->room.floor_bit_info.floor_original ? RSV_NO
                                                       : (u16)(ITM_CARPET00 + cot->room.wall_floor.flooring_idx);
    }
    return cot->room.floor_bit_info.wall_original ? RSV_NO : (u16)(ITM_WALL00 + cot->room.wall_floor.wallpaper_idx);
}

// (only what the game itself can lay there)
static void mp_map_set(int r, int k, u16 v) {
    mHm_cottage_c* cot = Save_GetPointer(island.cottage);

    if (s_rg[r].map == MP_MAP_DISC) {
        if (v == EMPTY_NO) {
            ClrMusicBox(cot->music_box, k);
        } else if (v == ITM_MINIDISK_START + k) {
            SetMusicBox(cot->music_box, k);
        }
    } else if (k == 0 && v >= ITM_CARPET00 && v < ITM_CARPET00 + FLOOR_PLAYER_ROOM_END) {
        cot->room.wall_floor.flooring_idx = (u8)(v - ITM_CARPET00);
    } else if (k == 1 && v >= ITM_WALL00 && v < ITM_WALL00 + WALL_ETC_END) {
        cot->room.wall_floor.wallpaper_idx = (u8)(v - ITM_WALL00);
    }
}

static u16 mp_raw_get(int cell) {
    int r = mp_rg_of(cell);
    int k = cell - s_rg[r].base;

    if (s_rg[r].map != MP_MAP_NONE) {
        return mp_map_get(r, k);
    }
    if (s_rg[r].bits) {
        return (u16)((*mp_word(r, k >> 4) >> (k & 15)) & 1);
    }
    return *mp_word(r, k);
}

static int s_refresh;
static int mp_cot_kept(int k);
static u16 mp_town_take(u16 v);
static int mp_take_matches(u16 took, u16 item);

static void mp_raw_set(int cell, u16 v) {
    int r = mp_rg_of(cell);
    int k = cell - s_rg[r].base;

    if (s_rg[r].map != MP_MAP_NONE) {
        mp_map_set(r, k, v);
    } else if (s_rg[r].bits) {
        u16* w = mp_word(r, k >> 4);

        *w = (u16)((v ? (*w | (1u << (k & 15))) : (*w & ~(1u << (k & 15)))));
    } else if (r != MP_RG_COT || !mp_cot_kept(k)) {
        *mp_word(r, k) = v;
        // (the town's fg shows it; the cottage's room keeps its furniture to itself)
        s_refresh |= r < MP_RG_COT;
    }
}

// the cottage cell a Save_t offset falls in
static int mp_cot_cell_of_off(u32 off) {
    u32 rel;
    u32 layer;
    u32 in;

    if (off < s_rg[MP_RG_COT].off) {
        return -1;
    }
    rel = off - s_rg[MP_RG_COT].off;
    layer = rel / sizeof(mHm_lyr_c);
    in = rel % sizeof(mHm_lyr_c);
    if (layer >= 4 || in >= UT_TOTAL_NUM * sizeof(mActor_name_t) || (in & 1) != 0) {
        return -1;
    }
    return MP_COT_BASE + (int)(layer * UT_TOTAL_NUM + in / sizeof(mActor_name_t));
}

// the town cell a Save_t offset falls in, for fg regions only
static int mp_cell_of_save_off(u32 off) {
    int r;

    for (r = MP_RG_FG; r <= MP_RG_IFG; r++) {
        if (off >= s_rg[r].off && off < s_rg[r].off + (u32)s_rg[r].words * 2) {
            return s_rg[r].base + (int)((off - s_rg[r].off) >> 1);
        }
    }
    return -1;
}

// town coordinates of an fg cell, for distances; FALSE for other regions
static int mp_cell_pos(int cell, int* gx, int* gz, int* region) {
    int r = mp_rg_of(cell);
    int k = cell - s_rg[r].base;
    int blocks_x = (r == MP_RG_FG) ? FG_BLOCK_X_NUM : mISL_FG_BLOCK_X_NUM;
    int block;

    if (r != MP_RG_FG && r != MP_RG_IFG) {
        return FALSE;
    }
    block = k / UT_TOTAL_NUM;
    k %= UT_TOTAL_NUM;
    *gx = (block % blocks_x) * UT_X_NUM + (k % UT_X_NUM);
    *gz = (block / blocks_x) * UT_Z_NUM + (k / UT_X_NUM);
    *region = r;
    return TRUE;
}

static int mp_cell_at(int region, int gx, int gz) {
    int blocks_x = (region == MP_RG_FG) ? FG_BLOCK_X_NUM : mISL_FG_BLOCK_X_NUM;
    int blocks_z = (region == MP_RG_FG) ? FG_BLOCK_Z_NUM : mISL_FG_BLOCK_Z_NUM;
    int bx = gx / UT_X_NUM;
    int bz = gz / UT_Z_NUM;

    if (gx < 0 || gz < 0 || bx >= blocks_x || bz >= blocks_z) {
        return -1;
    }
    return s_rg[region].base + (bz * blocks_x + bx) * UT_TOTAL_NUM + (gz % UT_Z_NUM) * UT_X_NUM + (gx % UT_X_NUM);
}

// the rest of the save
// host-owned bytes that stream out as they change (everything but players, villagers and cells)

typedef struct {
    u32 off;
    u32 size;
} mp_span_t;

#define MP_GEN_SPANS 48
#define MP_GEN_PAGE  256
#define MP_GEN_STEP  4096 // bytes compared per frame
#define MP_GEN_PAGES_MAX ((sizeof(Save_t) + MP_GEN_PAGE - 1) / MP_GEN_PAGE)

static mp_span_t s_gen[MP_GEN_SPANS];
static int s_gen_count;
static u32 s_gen_bytes;

#define MP_SKIP(f) { offsetof(Save_t, f), sizeof(((Save_t*)0)->f) }
#define MP_SKIP_HNW(h, s)                                                                                       \
    { offsetof(Save_t, homes) + (h) * sizeof(mHm_hs_c) + offsetof(mHm_hs_c, haniwa) + offsetof(Haniwa_c, items) + \
          (s) * sizeof(Haniwa_Item_c) + offsetof(Haniwa_Item_c, item),                                          \
      sizeof(mActor_name_t) }

// the residents' own data stays out but for their designs, which a house's wallpaper or carpet can show
#define MP_PRV(i) (offsetof(Save_t, private_data) + (i) * sizeof(Private_c))
#define MP_ORG_END (offsetof(Private_c, my_org) + sizeof(((Private_c*)0)->my_org))
#define MP_SKIP_PRV(i) { MP_PRV(i), offsetof(Private_c, my_org) }, { MP_PRV(i) + MP_ORG_END, sizeof(Private_c) - MP_ORG_END }

static void mp_gen_build(void) {
    // (in Save_t order; the gyroids' items are town cells)
    static const mp_span_t skip[] = {
        MP_SKIP(save_check),     MP_SKIP(scene_no),         MP_SKIP(copy_protect),
        MP_SKIP_PRV(0),          MP_SKIP_PRV(1),            MP_SKIP_PRV(2),            MP_SKIP_PRV(3),
        MP_SKIP_HNW(0, 0),       MP_SKIP_HNW(0, 1),         MP_SKIP_HNW(0, 2),         MP_SKIP_HNW(0, 3),
        MP_SKIP_HNW(1, 0),       MP_SKIP_HNW(1, 1),         MP_SKIP_HNW(1, 2),         MP_SKIP_HNW(1, 3),
        MP_SKIP_HNW(2, 0),       MP_SKIP_HNW(2, 1),         MP_SKIP_HNW(2, 2),         MP_SKIP_HNW(2, 3),
        MP_SKIP_HNW(3, 0),       MP_SKIP_HNW(3, 1),         MP_SKIP_HNW(3, 2),         MP_SKIP_HNW(3, 3),
        MP_SKIP(fg),             MP_SKIP(police_box),       MP_SKIP(config),           MP_SKIP(save_exist),
        MP_SKIP(deposit),        MP_SKIP(saved_rom_debug),  MP_SKIP(time_delta),       MP_SKIP(island.fgblock),
        MP_SKIP(island.cottage.room.layer_main.items),     MP_SKIP(island.cottage.room.layer_secondary.items),
        MP_SKIP(island.cottage.room.layer_storage1.items), MP_SKIP(island.cottage.room.layer_storage2.items),
        MP_SKIP(island.cottage.room.wall_floor),           MP_SKIP(island.cottage.music_box),
        MP_SKIP(island.deposit), MP_SKIP(travel_hard_time),
    };
    int n = (int)(sizeof(skip) / sizeof(skip[0]));
    u32 at = 0;
    int i;

    if (s_gen_count != 0) {
        return;
    }
    // the table is in Save_t order, so the gaps between skips are what streams
    for (i = 0; i <= n; i++) {
        u32 end = (i < n) ? skip[i].off : (u32)sizeof(Save_t);

        if (end > at && s_gen_count < MP_GEN_SPANS) {
            s_gen[s_gen_count].off = at;
            s_gen[s_gen_count].size = end - at;
            s_gen_bytes += end - at;
            s_gen_count++;
        }
        if (i < n) {
            at = skip[i].off + skip[i].size;
        }
    }
}

// linear position in the streamed bytes -> Save_t offset and bytes left in that span
static u32 mp_gen_save_off(u32 lin, u32* left) {
    int i;

    for (i = 0; i < s_gen_count; i++) {
        if (lin < s_gen[i].size) {
            *left = s_gen[i].size - lin;
            return s_gen[i].off + lin;
        }
        lin -= s_gen[i].size;
    }
    *left = 0;
    return 0;
}

// Save_t offset of a streamed byte -> its linear position
static u32 mp_gen_lin_of(u32 off) {
    u32 lin = 0;
    int i;

    for (i = 0; i < s_gen_count; i++) {
        if (off >= s_gen[i].off && off - s_gen[i].off < s_gen[i].size) {
            return lin + (off - s_gen[i].off);
        }
        lin += s_gen[i].size;
    }
    return lin;
}

static int mp_gen_contains(u32 off, u32 len) {
    int i;

    for (i = 0; i < s_gen_count; i++) {
        if (off >= s_gen[i].off && off - s_gen[i].off <= s_gen[i].size && len <= s_gen[i].size - (off - s_gen[i].off)) {
            return TRUE;
        }
    }
    return FALSE;
}

// state

#define MP_OPS          64
#define MP_OP_CELLS     64 // an op's cells at most: the cottage's, so furniture moved at once goes whole
#define MP_OP_TOWN      40 // ...the town's
#define MP_XFERS        96
#define MP_LOSSES       16
#define MP_ZONES        8
#define MP_QUEUE_BYTES  (48 * 1024)
#define MP_INBOX_BYTES  (32 * 1024)
#define MP_CRC_MS       5000
#define MP_COMMIT_MS    8000
#define MP_PICKUP_MS    10000
#define MP_PICKUP_WAIT_MS 60000 // a pickup the host hasn't saved still waits this long for its pocket
#define MP_DROP_MS      5000
#define MP_ZONE_MS      3000
#define MP_ZONE_UNITS   3
#define MP_MSG_MAX      MP_REL_MAX
#define MP_ARRIVALS     8
#define MP_PICKS        128 // at least the guest's MP_XFERS, so nothing the guest tracks is forgotten here
#define MP_OWED         8
#define MP_DOUBT_MS     1500  // the other side quiet this long: no new item may cross over
#define MP_ANSWER_MS    3000  // ...or its game this slow to answer an op (it's elsewhere: an NES game, say)
#define MP_COT_HELD_MAX 16    // held cottage ops past this are undone here, and the room comes up again
#define MP_PP_TRIES     3     // passport writes that fail in a row before the cottage's held ops give up
#define MP_SENT_KEEP_MS 10000 // a commit this old on a live line has surely arrived
#define MP_REFUSED      8
#define MP_CAPTURES     4
#define MP_REL_RESERVE  8 // reliable window slots town traffic leaves for everything else
#define MP_SHOP_SHOWS   16

enum {
    MP_ACK_ACCEPT,
    MP_ACK_REJECT,
    MP_ACK_MOVED,
};

enum {
    MP_OPS_FREE,
    MP_OPS_HELD, // waiting for the passport that dropped its items
    MP_OPS_SENT,
    MP_OPS_ACCEPTED,
    MP_OPS_REJECTED,
};

enum {
    MP_XFER_PICKUP,
    MP_XFER_DROP,
};

typedef struct {
    u16 id;
    u8 state;
    u8 n;
    u16 cell[MP_OP_CELLS];
    u16 oldv[MP_OP_CELLS];
    u16 newv[MP_OP_CELLS];
    u64 plain;   // the cottage's cells that moved nothing between a pocket and the town
    u32 sent_ms;
    u32 gen;     // guest: its drops' place in line for the passport (0: none)
} mp_op_t;

typedef struct {
    u8 used;
    u8 kind;
    u8 in_pocket; // the pocket event has been matched
    u8 committed;
    u8 cot;       // a pickup from the cottage, named to the host by item
    u8 settled;   // an unsent pickup whose item went back to the town through the drop undone after it
    u16 op;
    mActor_name_t item;
    u32 t_ms;
    u32 price; // bells a gyroid purchase paid for it
} mp_xfer_t;

typedef struct {
    mActor_name_t item;
    u32 t_ms;
} mp_loss_t;

typedef struct {
    u16 op;
    u16 cell;
    mActor_name_t item;   // as the cell had it
    mActor_name_t pocket; // ...and as a pocket has it (the cottage's furniture turned about)
    u32 price;            // what a gyroid's sale took for it
} mp_pick_t;

typedef struct {
    u16 op;
    u32 ms;
} mp_sent_t;

// something another player bought, still to be shown sold on this shop floor
typedef struct {
    mActor_name_t was;
    mActor_name_t now;
} mp_shop_show_t;

// a town save's snapshot: the newest item transfer of each guest it holds
typedef struct {
    u32 serial;
    u16 op[MP_MAX_PEERS];
    u8 any[MP_MAX_PEERS];
} mp_capture_t;

typedef struct {
    u8 region;
    s16 gx;
    s16 gz;
    u32 until_ms;
} mp_zone_t;

typedef struct {
    u8 data[MP_QUEUE_BYTES];
    int head;
    int tail;
    int overflow;
} mp_queue_t;

typedef struct {
    int conn;
    u16 last_op;  // last op accepted from this guest
    u16 rx_op;    // ...and the newest it sent at all
    u8 rx_any;
    u16 xfer_op;  // newest accepted op that moved an item between a pocket and the town
    int xfer_any;
    u16 done_op;  // newest commit sent; the guest keeps pickups up to here
    int done_any;
    mp_sent_t sent[4]; // recent commits, newest first
    int nsent;
    u16 due_op;   // a landed commit waiting for the guest's line to come back
    int due_any;
    int left;     // boarded home: nothing more is committed or kept for it
    u8 hold;      // its passport stands still (a pickup not yet saved was used up): its pickups stay here
    u8 hold_any;
    u16 hold_mark; // ...the newest of the host's saves that passport has
    mp_pick_t picks[MP_PICKS]; // accepted pickups the guest may not keep yet: back to the town if it doesn't
    int npicks;
    mActor_name_t refused[MP_REFUSED]; // refused drops, which may come back as lost & found claims
    mp_zone_t zones[MP_ZONES];
    mp_queue_t* out;
    mNW_original_design_c design; // a design coming in two halves
    u8 design_half;               // 1 + the target of the first half in, 0 none
} mp_guest_w_t;

static struct {
    int live; // buffers allocated and views valid
    u16* cur;
    u16* view;
    u16* pred;
    u16* pend;
    u16* exp;
    u8* held;
    u8* force;
    u8* lock;   // cells the local player is in the middle of changing, as last looked
    u8* hold;   // ...of those, the ones whose changes wait till it's done
    u16* locks; // ...as a list
    int nlocks;
    u16* pins;  // guest: cells held at what the player saw while it works on them
    int npins;
    u8* gen_shadow;
    u32 gen_scan;
    u16 next_op;
    mp_op_t ops[MP_OPS];
    mp_xfer_t xfers[MP_XFERS];
    mp_loss_t losses[MP_LOSSES];
    mp_loss_t arrivals[MP_ARRIVALS]; // pocket gains waiting for the pickup they came from
    mActor_name_t pocket_prev[mPr_POCKETS_SLOT_COUNT];
    int pocket_valid;
    mp_guest_w_t guests[MP_MAX_PEERS];
    mp_queue_t* host_out; // guest: messages to the host
    u8 inbox[MP_INBOX_BYTES];
    int inbox_len;
    int inbox_conn[64];
    int host_pickup_cell; // the host player's queued pickup, not yet taken from the cell
    Shop_c shop_last;     // guest: the shop as the host last had it, to spot our purchases
    u64 melody_last;      // guest: the town tune as the host last had it
    u32 last_crc_ms;
    u32 last_commit_ms;
    mp_capture_t caps[MP_CAPTURES]; // newest first, dropped once their save is done
    int ncaps;
    u32 landed_seen;
    int hurry; // host: a guest is waiting at Porter for its pickups to be saved
    int save_asked; // host: a visitor handed the town something outside the cells
    mp_shop_show_t shop_show[MP_SHOP_SHOWS];
    int nshop_show;
    u32 pp_wait;     // guest: ops hold, in order, until this passport is on the card
    int pp_again;    // guest: ...and the cottage's wait for another one, after one that didn't make it
    int pp_fails;    // guest: writes that didn't make it, in a row
    u32 drop_gen;    // guest: drops so far, numbered
    u32 pp_wait_gen; // guest: ...the newest that passport leaves out
    u32 pp_done_gen; // guest: ...and the newest one on the card
    int recall_pp;   // guest: heading home as the passport last stood
    int recalling;   // guest: every held op is being undone
    int home_pp;     // guest: ...and the host learns which of its saves that passport has
    int hold_told;   // guest: the host knows its passport stands still
    u32 hold_mark;   // guest: ...and which of its saves that passport has
    int hold_open;   // guest: ...the host has heard the save the next one will have: it may be written
    int hold_closing; // guest: ...and the word it's over is on its way
    int exiting;      // guest: the app is closing: the host hears the card's own mark last
    u32 hold_off_seq; // guest: ...the passport written since, before the host hears it moves again
    u32 hold_retry_ms;
    u16 held_ops[MP_OPS];
    int nheld;
    mActor_name_t owed[MP_OWED]; // guest: refused pickups already set down again
    int nowed;
    u16 seen_op; // guest: newest commit heard
    int seen_any;
    u32 fg_back_ms; // host: when its player came back out into town
} s_w;

// the island's cottage: everyone in it rearranges it at once. Each room follows the cells as the host has them (a
// visitor's own ops counting as they go); what a player is in the middle of stays as it saw it until it's done, and
// furniture someone has in hand is left to them.

enum {
    MP_COT_UNITS,  // units whose furniture a player has in hand: a visitor's own, or from the host everyone else's
                   // (and those of the players it gives way to)
    MP_COT_META,   // guest: its room's lamps, gyroids' steps and tempo, for the host's save while its own room isn't up
    MP_COT_ANGLES, // which way table-top things face (each screen keeps its own): a player's changes, passed round
};

// what a room saves as it goes besides its cells: each item layer's lamps and gyroids' steps, and the tempo
#define MP_COT_LAYER      (sizeof(u64) + sizeof(((mHm_lyr_c*)0)->haniwa_step))
#define MP_COT_META_BYTES (2 * MP_COT_LAYER + sizeof(TempoBeat_c))
#define MP_COT_UNIT_BYTES (UT_TOTAL_NUM / 8)
#define MP_COT_META_MS    500
#define MP_COT_PIECES     64     // what a room of it holds
#define MP_PIN_OP         0xFFFF // a cell's pend while held at what the player saw
#define MP_LOCKS_MAX      (MP_COT_WORDS + 8)
#define MP_COT_INSIDE     8 // the room's inside, units 1 to 8 each way

static struct {
    u8 inside;                                 // this game's player is in the cottage
    u8 busy[MP_COT_WORDS / 8];                 // cells its room leaves be: in hand here, or held as the player saw them
    u8 mine[MP_COT_UNIT_BYTES];                // units whose furniture this player has in hand
    u8 told[MP_COT_UNIT_BYTES];                // guest: ...as the host last heard
    u8 others[MP_COT_UNIT_BYTES];              // guest: everyone else's
    u8 yield[MP_COT_UNIT_BYTES];               // guest: ...of those this player gives way to (the host, lower slots)
    u8 away[MP_COT_UNIT_BYTES];                // host: what it had in hand as its room went for an NES game
    u8 units[MP_MAX_PEERS][MP_COT_UNIT_BYTES]; // host: each visitor's
    u8 sent[MP_MAX_PEERS][2 * MP_COT_UNIT_BYTES]; // host: everyone else's, as each visitor last heard
    u8 sent_ok[MP_MAX_PEERS];
    u8 meta[MP_COT_META_BYTES]; // guest: the room's lamps and steps as it came up, or as last sent
    s16 angles[UT_TOTAL_NUM];   // table-top things' facings, as every screen in the room shows them
    u8 angles_apply;            // ...for a room just up: it takes them on first
    u8 resync;  // an op of the cottage's undone here: its room looks again, the cells unchanged or not
    u8 holding; // ...in the middle of something there whose changes wait for it
    u8 room_in[MP_MAX_PEERS]; // host: each visitor's room of it is up
    u8 told_inside;           // guest: ...this one's, as the host last heard
    u32 meta_ms;
    u32 hold_since;
    int nlocked; // cottage cells whose changes wait, at the last look
} s_cot;

static void mp_cot_send_meta(int leaving);

#define MP_BIT_GET(a, i) (((a)[(i) >> 3] >> ((i) & 7)) & 1)
#define MP_BIT_SET(a, i) ((a)[(i) >> 3] |= (u8)(1 << ((i) & 7)))
#define MP_BIT_CLR(a, i) ((a)[(i) >> 3] &= (u8)~(1 << ((i) & 7)))

static void mp_put16(u8* p, u32 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static u32 mp_get16(const u8* p) {
    return (u32)p[0] | ((u32)p[1] << 8);
}

static void mp_put32w(u8* p, u32 v) {
    mp_put16(p, v);
    mp_put16(p + 2, v >> 16);
}

static u32 mp_get32w(const u8* p) {
    return mp_get16(p) | (mp_get16(p + 2) << 16);
}

// outgoing queues
// world traffic can outrun the reliable window; it waits here and drains every frame

static mp_queue_t* mp_queue_new(void) {
    mp_queue_t* q = (mp_queue_t*)mp_alloc(sizeof(mp_queue_t));

    if (q != NULL) {
        q->head = q->tail = q->overflow = 0;
    }
    return q;
}

// what the checksum rounds can't repair: a lost one leaves an op or a transfer hanging forever
static int mp_msg_critical(const u8* msg) {
    return msg[0] == MP_M_OP || msg[0] == MP_M_ACK || msg[0] == MP_M_COMMIT || msg[0] == MP_M_SOFT ||
           msg[0] == MP_M_COTTAGE || msg[0] == MP_M_CLAIM || msg[0] == MP_M_TALK_LOCK || msg[0] == MP_M_TALK_UNLOCK ||
           msg[0] == MP_M_LEAVING;
}

static int mp_queue_used(const mp_queue_t* q) {
    return (q->tail - q->head + MP_QUEUE_BYTES) % MP_QUEUE_BYTES;
}

// only what can't be repaired stays, compacted towards the head
static void mp_queue_purge(mp_queue_t* q) {
    int rd = q->head;
    int wr = q->head;

    while (rd != q->tail) {
        int len = q->data[rd] | (q->data[(rd + 1) % MP_QUEUE_BYTES] << 8);

        if (mp_msg_critical(&q->data[(rd + 2) % MP_QUEUE_BYTES])) {
            int i;

            for (i = 0; i < len + 2; i++) {
                q->data[(wr + i) % MP_QUEUE_BYTES] = q->data[(rd + i) % MP_QUEUE_BYTES];
            }
            wr = (wr + len + 2) % MP_QUEUE_BYTES;
        }
        rd = (rd + len + 2) % MP_QUEUE_BYTES;
    }
    q->tail = wr;
}

static void mp_queue_push(mp_queue_t* q, const u8* msg, int len) {
    if (q == NULL) {
        return;
    }
    if (mp_queue_used(q) + len + 2 >= MP_QUEUE_BYTES) {
        q->overflow = TRUE; // the next checksum round repairs whatever is dropped here
        if (!mp_msg_critical(msg)) {
            return;
        }
        mp_queue_purge(q);
        if (mp_queue_used(q) + len + 2 >= MP_QUEUE_BYTES) {
            pc_mp_log("[MP] world: queue full of acks, dropped %02X", msg[0]);
            return;
        }
    }
    q->data[q->tail] = (u8)len;
    q->data[(q->tail + 1) % MP_QUEUE_BYTES] = (u8)(len >> 8);
    q->tail = (q->tail + 2) % MP_QUEUE_BYTES;
    while (len-- > 0) {
        q->data[q->tail] = *msg++;
        q->tail = (q->tail + 1) % MP_QUEUE_BYTES;
    }
}

static void mp_queue_flush(mp_queue_t* q, int conn, int reserve) {
    u8 msg[MP_MSG_MAX];

    while (q != NULL && q->head != q->tail && mp_lobby_rel_room(conn) > reserve) {
        int len = q->data[q->head] | (q->data[(q->head + 1) % MP_QUEUE_BYTES] << 8);
        int at = (q->head + 2) % MP_QUEUE_BYTES;
        int i;

        for (i = 0; i < len && i < MP_MSG_MAX; i++) {
            msg[i] = q->data[(at + i) % MP_QUEUE_BYTES];
        }
        if (!mp_lobby_send_rel(conn, msg, len)) {
            break;
        }
        q->head = (at + len) % MP_QUEUE_BYTES;
    }
}

static void mp_guest_release_held(void);

// queued town traffic into the link; reserve leaves reliable slots for everything else
static void mp_world_flush_queues(int reserve) {
    int s;

    if (mp_is_host()) {
        for (s = 1; s < MP_MAX_PEERS; s++) {
            if (s_w.guests[s].out != NULL && mp_lobby_guest_conn(s) >= 0) {
                mp_queue_flush(s_w.guests[s].out, mp_lobby_guest_conn(s), reserve);
            }
        }
    } else if (s_w.host_out != NULL && mp_lobby_host_conn() >= 0) {
        mp_queue_flush(s_w.host_out, mp_lobby_host_conn(), reserve);
    }
}

static mp_guest_w_t* mp_gw_of_conn(int conn) {
    int slot = mp_lobby_guest_slot(conn) + 1;

    if (slot <= 0 || slot >= MP_MAX_PEERS) {
        return NULL;
    }
    return &s_w.guests[slot];
}

static void mp_send_guest(int slot, const u8* msg, int len) {
    mp_guest_w_t* g = &s_w.guests[slot];

    if (g->out == NULL) {
        g->out = mp_queue_new();
    }
    mp_queue_push(g->out, msg, len);
}

static int mp_guest_in_town(int slot) {
    return mp_lobby_guest_arrived(slot) && mp_lobby_guest_conn(slot) >= 0;
}

// host: to every guest in town except one (a quiet one's waits in its queue till it's back)
static void mp_send_all(const u8* msg, int len, int except_slot) {
    int s;

    for (s = 1; s < MP_MAX_PEERS; s++) {
        if (s != except_slot && mp_guest_in_town(s)) {
            mp_send_guest(s, msg, len);
        }
    }
}

// the cottage

// this game's player may rearrange it: the host, or a visitor the host lets
int mp_cot_owner(void) {
    return !s_w.live || mp_is_host() || mp_rule(MP_RULE_COTTAGE);
}

int mp_cot_shared(void) {
    return s_w.live;
}

// its four item layers as the host has them (a visitor's own ops counting as they go), and the cells its room here is
// to leave be
const unsigned short* mp_cot_truth(const unsigned char** busy) {
    if (!s_w.live || !s_cot.inside) {
        return NULL;
    }
    if (busy != NULL) {
        *busy = s_cot.busy;
    }
    return mp_is_host() ? &s_w.view[MP_COT_BASE] : &s_w.exp[MP_COT_BASE];
}

// another player has the furniture on this unit in hand
int mp_cot_in_use(int ut) {
    int s;

    if (!s_w.live || ut < 0 || ut >= UT_TOTAL_NUM) {
        return FALSE;
    }
    if (!mp_is_host()) {
        return MP_BIT_GET(s_cot.others, ut);
    }
    for (s = 1; s < MP_MAX_PEERS; s++) {
        if (MP_BIT_GET(s_cot.units[s], ut)) {
            return TRUE;
        }
    }
    return FALSE;
}

// a player stands on this unit of the cottage, other than this game's own and `except`'s (a visitor's op: the
// host's player counts too)
static int mp_cot_unit_stood(int ut, int except) {
    int self = mp_is_host() ? 0 : mp_lobby_self_slot();
    int s;

    for (s = 0; s < MP_MAX_PEERS; s++) {
        unsigned int age;
        const mp_pstate_t* st;

        if (s == except || s == self || (st = mp_player_state(s, &age)) == NULL) {
            continue;
        }
        if (age < 3000 && st->scene == SCENE_COTTAGE_MY && st->x >= 0.0f && st->z >= 0.0f &&
            (int)(st->x / mFI_UT_WORLDSIZE_X_F) + (int)(st->z / mFI_UT_WORLDSIZE_Z_F) * UT_X_NUM == ut) {
            return TRUE;
        }
    }
    if (except >= 0 && mp_is_host() && s_cot.inside && Save_Get(scene_no) == SCENE_COTTAGE_MY) {
        GAME_PLAY* play = mp_live_play();
        PLAYER_ACTOR* player = (play != NULL) ? GET_PLAYER_ACTOR(play) : NULL;

        if (player != NULL) {
            xyz_t* p = &player->actor_class.world.position;

            return (int)(p->x / mFI_UT_WORLDSIZE_X_F) + (int)(p->z / mFI_UT_WORLDSIZE_Z_F) * UT_X_NUM == ut;
        }
    }
    return FALSE;
}

int mp_cot_unit_taken(int ut) {
    return s_w.live && mp_cot_unit_stood(ut, -1);
}

// a cottage cell its room up here keeps in a piece of furniture (as a room does with stored things): the save gets it
// as the room goes
static int mp_cot_kept(int k) {
    return s_cot.inside && k >= UT_TOTAL_NUM && k < MP_COT_WORDS &&
           aMR_mp_holds_cell(k / UT_TOTAL_NUM, k % UT_TOTAL_NUM);
}

void mp_cot_room_enter(void) {
    mHm_flr_c* room = Save_GetPointer(island.cottage.room);

    s_cot.inside = TRUE;
    // (the lamps and steps as the room comes up with them: only what changes after goes out)
    memcpy(s_cot.meta, &room->layer_main.ftr_switch, MP_COT_LAYER);
    memcpy(s_cot.meta + MP_COT_LAYER, &room->layer_secondary.ftr_switch, MP_COT_LAYER);
    s_cot.meta_ms = pc_mp_now_ms();
    s_cot.angles_apply = TRUE;
    memset(s_cot.busy, 0, sizeof(s_cot.busy));
}

// guest: another player's hands are on this unit's furniture, and it's theirs over this player's (the host's, or a
// lower slot's): two who took hold of it at once, the one who gives way lets go before anything moves
int mp_cot_must_yield(int ut) {
    return s_w.live && !mp_is_host() && ut >= 0 && ut < UT_TOTAL_NUM && MP_BIT_GET(s_cot.yield, ut);
}

// host: another visitor than this one has a room of the cottage up
static int mp_cot_others_inside(int slot) {
    int s;

    for (s = 1; s < MP_MAX_PEERS; s++) {
        if (s != slot && s_cot.room_in[s] && mp_guest_in_town(s)) {
            return TRUE;
        }
    }
    return FALSE;
}

// table-top facings, as a list of unit and facing: a guest's go to the host, the host's (and those it passes on) to
// every visitor, the one they came from too, so two changed at once end the same everywhere
static void mp_cot_angles_out(const u8* list, int n, int to_slot) {
    u8 msg[3 + MP_COT_INSIDE * MP_COT_INSIDE * 3];
    int s;

    if (n <= 0 || n > MP_COT_INSIDE * MP_COT_INSIDE) {
        return;
    }
    msg[0] = MP_M_COTTAGE;
    msg[1] = MP_COT_ANGLES;
    msg[2] = (u8)n;
    memcpy(msg + 3, list, n * 3);
    if (!mp_is_host()) {
        mp_queue_push(s_w.host_out, msg, 3 + n * 3);
        return;
    }
    for (s = 1; s < MP_MAX_PEERS; s++) {
        if ((to_slot < 0 || s == to_slot) && mp_guest_in_town(s)) {
            mp_send_guest(s, msg, 3 + n * 3);
        }
    }
}

// the facings this room's player changed (a thing set down faces ahead, a table turned turns what's on it)
static void mp_cot_angles_poll(void) {
    aSG_Clip_c* sg = Common_Get(clip).shop_goods_clip;
    u8 list[MP_COT_INSIDE * MP_COT_INSIDE * 3];
    int n = 0;
    int x;
    int z;

    if (!s_cot.inside || Save_Get(scene_no) != SCENE_COTTAGE_MY || sg == NULL) {
        return;
    }
    for (z = 1; z <= MP_COT_INSIDE; z++) {
        for (x = 1; x <= MP_COT_INSIDE; x++) {
            int ut = x + z * UT_X_NUM;
            s16 a;

            // (a room just up takes on everyone's first)
            if (s_cot.angles_apply) {
                sg->single_set_angle_y_proc(z, x, mCoBG_LAYER1, s_cot.angles[ut]);
                continue;
            }
            // (not while the player is at it: a thing on furniture in hand faces ahead till it's set down)
            if (MP_BIT_GET(s_cot.busy, mCoBG_LAYER1 * UT_TOTAL_NUM + ut)) {
                continue;
            }
            a = sg->single_get_angle_y_proc(z, x, mCoBG_LAYER1);
            if (a != s_cot.angles[ut]) {
                s_cot.angles[ut] = a;
                list[n * 3] = (u8)ut;
                mp_put16(list + n * 3 + 1, (u16)a);
                n++;
            }
        }
    }
    s_cot.angles_apply = FALSE;
    mp_cot_angles_out(list, n, -1);
}

// facings another player's game set: this room shows them too (the host passes them round)
static void mp_cot_angles_in(const u8* p, int len) {
    aSG_Clip_c* sg = Common_Get(clip).shop_goods_clip;
    int n = p[2];
    int i;

    if (n > MP_COT_INSIDE * MP_COT_INSIDE || len < 3 + n * 3) {
        return;
    }
    for (i = 0; i < n; i++) {
        int ut = p[3 + i * 3];
        int x = ut & 15;
        int z = ut >> 4;
        s16 a = (s16)mp_get16(p + 4 + i * 3);

        if (x < 1 || x > MP_COT_INSIDE || z < 1 || z > MP_COT_INSIDE) {
            continue;
        }
        s_cot.angles[ut] = a;
        if (s_cot.inside && !s_cot.angles_apply && Save_Get(scene_no) == SCENE_COTTAGE_MY && sg != NULL) {
            sg->single_set_angle_y_proc(z, x, mCoBG_LAYER1, a);
        }
    }
    if (mp_is_host()) {
        mp_cot_angles_out(p + 3, n, -1);
    }
}

// host: the facings of the things on tables in the house floor it's in, for the players in there with it (each game
// keeps its own, wherever it last set them), as they change or someone comes in
#define MP_ANGLES_PER 20

static void mp_house_angles_out(void) {
    static u32 sum_sent;
    static u32 here_sent;
    static int wait;
    aSG_Clip_c* sg = Common_Get(clip).shop_goods_clip;
    mActor_name_t* fg1 = mFI_BkNum2UtFGTop_layer(0, 0, mCoBG_LAYER1);
    u8 body[2 + MP_ANGLES_PER * 3];
    u32 here = mp_player_here_mask();
    u32 sum = 0;
    int n = 0;
    int x;
    int z;

    if (sg == NULL || fg1 == NULL || aMR_mp_house_floor_off() == 0 || here == 0) {
        sum_sent = 0;
        here_sent = 0;
        return;
    }
    here_sent &= here;
    if (++wait < 30) {
        return;
    }
    wait = 0;
    for (z = 1; z <= MP_COT_INSIDE; z++) {
        for (x = 1; x <= MP_COT_INSIDE; x++) {
            mActor_name_t item = fg1[x + z * UT_X_NUM];

            if (item != EMPTY_NO && item != RSV_NO && !ITEM_IS_FTR(item)) {
                sum = sum * 31 + (u32)(x + z * UT_X_NUM) * 65536u + (u16)sg->single_get_angle_y_proc(z, x, mCoBG_LAYER1);
            }
        }
    }
    if (sum == sum_sent && (here & ~here_sent) == 0) {
        return;
    }
    sum_sent = sum;
    here_sent = here;
    body[0] = MP_VFX_ANGLES;
    for (z = 1; z <= MP_COT_INSIDE; z++) {
        for (x = 1; x <= MP_COT_INSIDE; x++) {
            mActor_name_t item = fg1[x + z * UT_X_NUM];

            if (item == EMPTY_NO || item == RSV_NO || ITEM_IS_FTR(item)) {
                continue;
            }
            body[2 + n * 3] = (u8)(x + z * UT_X_NUM);
            mp_put16(body + 3 + n * 3, (u16)sg->single_get_angle_y_proc(z, x, mCoBG_LAYER1));
            if (++n == MP_ANGLES_PER) {
                body[1] = (u8)n;
                mp_vfx_send(body, 2 + n * 3);
                n = 0;
            }
        }
    }
    if (n > 0) {
        body[1] = (u8)n;
        mp_vfx_send(body, 2 + n * 3);
    }
}

// ...and a visitor in there takes them on
void mp_house_angles_in(const unsigned char* body, int len) {
    aSG_Clip_c* sg = Common_Get(clip).shop_goods_clip;
    int n = len >= 2 ? body[1] : 0;
    int i;

    if (sg == NULL || mp_is_host() || n > MP_ANGLES_PER || len < 2 + n * 3) {
        return;
    }
    for (i = 0; i < n; i++) {
        int ut = body[2 + i * 3];

        sg->single_set_angle_y_proc(ut >> 4, ut & 15, mCoBG_LAYER1, (s16)mp_get16(body + 3 + i * 3));
    }
}

// host: a visitor off the train hears the facings there are
static void mp_cot_angles_to(int slot) {
    u8 list[MP_COT_INSIDE * MP_COT_INSIDE * 3];
    int n = 0;
    int x;
    int z;

    for (z = 1; z <= MP_COT_INSIDE; z++) {
        for (x = 1; x <= MP_COT_INSIDE; x++) {
            int ut = x + z * UT_X_NUM;

            if (s_cot.angles[ut] != 0) {
                list[n * 3] = (u8)ut;
                mp_put16(list + n * 3 + 1, (u16)s_cot.angles[ut]);
                n++;
            }
        }
    }
    mp_cot_angles_out(list, n, slot);
}

// guest: the lamps, steps and tempo its room leaves go to the host (whose save keeps them unless its own room is up)
void mp_cot_room_leave(void) {
    if (s_cot.inside && s_w.live && !mp_is_host() && mp_travel_state() == MP_TRAVEL_VISITING) {
        mp_cot_send_meta(TRUE);
    }
    memcpy(s_cot.away, s_cot.mine, sizeof(s_cot.away));
    s_cot.inside = FALSE;
    memset(s_cot.busy, 0, sizeof(s_cot.busy));
    memset(s_cot.mine, 0, sizeof(s_cot.mine));
}

// guest: the room's lamps and steps go to the host as they change, and once more (with the tempo) as it goes
static void mp_cot_send_meta(int leaving) {
    u8 msg[3 + MP_COT_META_BYTES];

    if (aMR_mp_meta(msg + 3, MP_COT_META_BYTES) != (int)MP_COT_META_BYTES) {
        return;
    }
    if (!leaving && memcmp(msg + 3, s_cot.meta, 2 * MP_COT_LAYER) == 0) {
        return;
    }
    memcpy(s_cot.meta, msg + 3, MP_COT_META_BYTES);
    msg[0] = MP_M_COTTAGE;
    msg[1] = MP_COT_META;
    msg[2] = (u8)leaving;
    mp_queue_push(s_w.host_out, msg, (int)sizeof(msg));
}

// host: its own room's lamps and steps go in its save as they change (the room otherwise saves them only as it
// goes), so a visitor coming in finds them as they are
static void mp_cot_host_meta(void) {
    mHm_flr_c* room = Save_GetPointer(island.cottage.room);
    u8 meta[MP_COT_META_BYTES];

    if (aMR_mp_meta(meta, MP_COT_META_BYTES) != (int)MP_COT_META_BYTES) {
        return;
    }
    if (memcmp(meta, &room->layer_main.ftr_switch, MP_COT_LAYER) != 0) {
        memcpy(&room->layer_main.ftr_switch, meta, MP_COT_LAYER);
    }
    if (memcmp(meta + MP_COT_LAYER, &room->layer_secondary.ftr_switch, MP_COT_LAYER) != 0) {
        memcpy(&room->layer_secondary.ftr_switch, meta + MP_COT_LAYER, MP_COT_LAYER);
    }
}

static void mp_cot_host_on(int slot, const u8* p, int len) {
    mHm_flr_c* room = Save_GetPointer(island.cottage.room);

    if (len < 2 || slot <= 0 || slot >= MP_MAX_PEERS) {
        return;
    }
    if (p[1] == MP_COT_UNITS && len >= 3 + MP_COT_UNIT_BYTES) {
        memcpy(s_cot.units[slot], p + 2, MP_COT_UNIT_BYTES);
        s_cot.room_in[slot] = p[2 + MP_COT_UNIT_BYTES] != 0;
    } else if (p[1] == MP_COT_ANGLES && len >= 3) {
        mp_cot_angles_in(p, len);
    } else if (p[1] == MP_COT_META && len >= 3 + (int)MP_COT_META_BYTES) {
        if (p[2]) {
            s_cot.room_in[slot] = FALSE;
        }
        // (a room still up elsewhere keeps its own, so one going doesn't undo what it did since)
        if (s_cot.inside || (p[2] && mp_cot_others_inside(slot))) {
            return;
        }
        memcpy(&room->layer_main.ftr_switch, p + 3, MP_COT_LAYER);
        memcpy(&room->layer_secondary.ftr_switch, p + 3 + MP_COT_LAYER, MP_COT_LAYER);
        memcpy(&room->tempo_beat, p + 3 + 2 * MP_COT_LAYER, sizeof(TempoBeat_c));
        mp_world_host_took(offsetof(Save_t, island.cottage.room.layer_main.ftr_switch), MP_COT_LAYER, slot);
        mp_world_host_took(offsetof(Save_t, island.cottage.room.layer_secondary.ftr_switch), MP_COT_LAYER, slot);
        mp_world_host_took(offsetof(Save_t, island.cottage.room.tempo_beat), sizeof(TempoBeat_c), slot);
    }
}

static void mp_cot_guest_on(const u8* p, int len) {
    if (len >= 2 + MP_COT_UNIT_BYTES && p[1] == MP_COT_UNITS) {
        memcpy(s_cot.others, p + 2, MP_COT_UNIT_BYTES);
        if (len >= 2 + 2 * MP_COT_UNIT_BYTES) {
            memcpy(s_cot.yield, p + 2 + MP_COT_UNIT_BYTES, MP_COT_UNIT_BYTES);
        } else {
            memset(s_cot.yield, 0, sizeof(s_cot.yield));
        }
    } else if (len >= 3 && p[1] == MP_COT_ANGLES) {
        mp_cot_angles_in(p, len);
    }
}

// a player there who may not rearrange it has it as someone else's room (nothing taken up, set down or laid), and
// every screen lays the carpet and wallpaper the save has
static void mp_cot_frame(void) {
    if (!s_cot.inside || Save_Get(scene_no) != SCENE_COTTAGE_MY) {
        return;
    }
    Common_Set(field_type, mp_cot_owner() ? mFI_FIELDTYPE2_PLAYER_ROOM : mFI_FIELDTYPE2_ROOM);
    aMI_mp_refresh();
}

// locks

static void mp_locks_clear(void) {
    int i;

    for (i = 0; i < s_w.nlocks; i++) {
        MP_BIT_CLR(s_w.lock, s_w.locks[i]);
        MP_BIT_CLR(s_w.hold, s_w.locks[i]);
    }
    s_w.nlocks = 0;
    s_cot.nlocked = 0;
}

// others keep off the cell (what the player does with what's there goes out as it happens)
static void mp_lock(int c) {
    if (c < 0 || c >= MP_CELLS || MP_BIT_GET(s_w.lock, c) || s_w.nlocks >= MP_LOCKS_MAX) {
        return;
    }
    MP_BIT_SET(s_w.lock, c);
    s_w.locks[s_w.nlocks++] = (u16)c;
}

// ...and its changes wait till the player is done, to go out whole
static void mp_lock_hold(int c) {
    mp_lock(c);
    if (c >= 0 && c < MP_CELLS && MP_BIT_GET(s_w.lock, c) && !MP_BIT_GET(s_w.hold, c)) {
        MP_BIT_SET(s_w.hold, c);
        s_cot.nlocked += c >= MP_COT_BASE && c < MP_COT_BASE + MP_COT_WORDS;
    }
}

// a pick-up the player asked for: its cell stays as the player saw it until the pocket has the item
static void mp_pickup_lock(GAME_PLAY* play) {
    PLAYER_ACTOR* player = GET_PLAYER_ACTOR(play);
    const xyz_t* pos = NULL;
    mActor_name_t* fg;
    int c;

    if (player == NULL) {
        return;
    }
    if (player->requested_main_index_changed && player->requested_main_index == mPlayer_INDEX_PICKUP) {
        pos = &player->requested_main_index_data.pickup.target_pos;
    } else if (player->requested_main_index_changed && player->requested_main_index == mPlayer_INDEX_PICKUP_EXCHANGE) {
        pos = &player->requested_main_index_data.pickup_exchange.target_pos;
    } else if (player->now_main_index == mPlayer_INDEX_PICKUP_EXCHANGE) {
        pos = &player->main_data.pickup_exchange.target_pos;
    }
    if (pos == NULL || (fg = mFI_GetUnitFG(*pos)) == NULL) {
        return;
    }
    c = mp_cell_of_save_off((u32)((u8*)fg - mp_save_base()));
    mp_lock_hold(c >= 0 ? c : mp_cot_cell_of_off((u32)((u8*)fg - mp_save_base())));
}

// the cottage's cells its room is to leave be: in hand now, or still held as the player saw them
static void mp_cot_busy_calc(int post) {
    int unsent = FALSE;
    int i;

    memset(s_cot.busy, 0, sizeof(s_cot.busy));
    for (i = 0; i < s_w.nlocks; i++) {
        int k = s_w.locks[i] - MP_COT_BASE;

        if (k >= 0 && k < MP_COT_WORDS) {
            MP_BIT_SET(s_cot.busy, k);
        }
    }
    for (i = 0; i < s_w.npins; i++) {
        int c = s_w.pins[i];
        int k = c - MP_COT_BASE;

        if (k >= 0 && k < MP_COT_WORDS && s_w.pend[c] == MP_PIN_OP) {
            MP_BIT_SET(s_cot.busy, k);
            unsent |= s_w.cur[c] != s_w.exp[c];
        }
    }
    // (over once its op has gone, which what the pockets gave or took meanwhile still counts for: a menu up or too
    // much in flight holds it back)
    if (s_cot.nlocked > 0 ? !s_cot.holding : (s_cot.holding && post && !unsent)) {
        s_cot.holding = !s_cot.holding;
        s_cot.hold_since = pc_mp_now_ms();
    }
}

// the player is in the middle of something in the cottage: what left or came to its pockets since just before waits
// for it
static int mp_cot_keeps(u32 t_ms, u32 window) {
    return s_cot.holding && (s32)(t_ms - (s_cot.hold_since - window)) >= 0;
}

int mp_cot_take_resync(void) {
    int r = s_cot.resync;

    s_cot.resync = FALSE;
    return r;
}

// the cottage's share of the frame: what's in hand here and elsewhere, the room's lamps and steps
static void mp_cot_post(void) {
    u32 now = pc_mp_now_ms();
    int s;

    mp_cot_busy_calc(TRUE);
    mp_cot_angles_poll();
    if (s_cot.inside && Save_Get(scene_no) == SCENE_COTTAGE_MY && now - s_cot.meta_ms >= MP_COT_META_MS) {
        s_cot.meta_ms = now;
        if (mp_is_host()) {
            mp_cot_host_meta();
        } else if (mp_travel_state() == MP_TRAVEL_VISITING) {
            mp_cot_send_meta(FALSE);
        }
    }
    if (!mp_is_host()) {
        if (mp_travel_state() == MP_TRAVEL_VISITING &&
            (memcmp(s_cot.mine, s_cot.told, MP_COT_UNIT_BYTES) != 0 || s_cot.told_inside != s_cot.inside)) {
            u8 msg[3 + MP_COT_UNIT_BYTES];

            memcpy(s_cot.told, s_cot.mine, MP_COT_UNIT_BYTES);
            s_cot.told_inside = s_cot.inside;
            msg[0] = MP_M_COTTAGE;
            msg[1] = MP_COT_UNITS;
            memcpy(msg + 2, s_cot.mine, MP_COT_UNIT_BYTES);
            msg[2 + MP_COT_UNIT_BYTES] = s_cot.inside;
            mp_queue_push(s_w.host_out, msg, (int)sizeof(msg));
        }
        return;
    }
    // host: each visitor hears of everyone else's, and whose it gives way to (the host's, and lower slots')
    for (s = 1; s < MP_MAX_PEERS; s++) {
        u8 msg[2 + 2 * MP_COT_UNIT_BYTES];
        int o;
        int k;

        if (!mp_guest_in_town(s)) {
            continue;
        }
        memcpy(msg + 2, s_cot.mine, MP_COT_UNIT_BYTES);
        memcpy(msg + 2 + MP_COT_UNIT_BYTES, s_cot.mine, MP_COT_UNIT_BYTES);
        for (o = 1; o < MP_MAX_PEERS; o++) {
            for (k = 0; o != s && k < MP_COT_UNIT_BYTES; k++) {
                msg[2 + k] |= s_cot.units[o][k];
                if (o < s) {
                    msg[2 + MP_COT_UNIT_BYTES + k] |= s_cot.units[o][k];
                }
            }
        }
        if (s_cot.sent_ok[s] && memcmp(msg + 2, s_cot.sent[s], 2 * MP_COT_UNIT_BYTES) == 0) {
            continue;
        }
        memcpy(s_cot.sent[s], msg + 2, 2 * MP_COT_UNIT_BYTES);
        s_cot.sent_ok[s] = TRUE;
        msg[0] = MP_M_COTTAGE;
        msg[1] = MP_COT_UNITS;
        mp_send_guest(s, msg, (int)sizeof(msg));
    }
}

// capture

static void mp_actor_cell(unsigned int off, unsigned short name, void* arg) {
    int cell = mp_cell_of_save_off(off);

    (void)arg;
    if (cell < 0) {
        return;
    }
    if (ITEM_NAME_GET_TYPE(name) == NAME_TYPE_ITEM2 && s_w.cur[cell] != EMPTY_NO) {
        return;
    }
    s_w.cur[cell] = name;
    MP_BIT_SET(s_w.held, cell);
}

// cells whose items live in actors right now, as restore_fgdata would write them back
void mp_world_actor_cells(GAME_PLAY* play, void (*cb)(unsigned int save_off, unsigned short name, void* arg),
                          void* arg) {
    pc_save_actor_cells(play, cb, arg);
}

#define MP_STRUCT_BACK_MS 3000 // host back out in town: its events set their tents up again meanwhile

// host away from town: its game lifted the events' tents and stands out of the town for the scene, as the
// original does (and sets them up again once back out); the others still see them where the events running have
// them, and one that ends meanwhile comes down for them too
static void mp_host_struct_overlay(void) {
    mEv_common_data_c* ev = Common_GetPointer(event_common);
    int p;

    // (no field up at all, an NES game say, is away too)
    if (!mFI_CheckFieldData() || mFI_GET_TYPE(mFI_GetFieldId()) != mFI_FIELD_FG) {
        s_w.fg_back_ms = 0;
    } else if (s_w.fg_back_ms == 0) {
        s_w.fg_back_ms = pc_mp_now_ms() | 1;
    }
    if (s_w.fg_back_ms != 0 && pc_mp_now_ms() - s_w.fg_back_ms >= MP_STRUCT_BACK_MS) {
        return;
    }
    for (p = 0; p < mEv_PLACE_NUM; p++) {
        mEv_place_data_c* d = &ev->place[p].data;
        mActor_name_t items[UT_TOTAL_NUM];
        int cell0;
        int k;

        // (a place outlasts its event until the host's game is back out to clear it)
        if (!(ev->place_use_bitfield & (1 << p)) || ITEM_NAME_GET_TYPE(d->actor_name) != NAME_TYPE_STRUCT ||
            !(mEv_check_status(ev->place[p].info.type, mEv_STATUS_ACTIVE) || mp_ev_placed_away(ev->place[p].info.type)) ||
            d->block.x < 1 ||
            d->block.x > FG_BLOCK_X_NUM || d->block.z < 1 || d->block.z > FG_BLOCK_Z_NUM) {
            continue;
        }
        cell0 = s_rg[MP_RG_FG].base + ((d->block.z - 1) * FG_BLOCK_X_NUM + (d->block.x - 1)) * UT_TOTAL_NUM;
        for (k = 0; k < UT_TOTAL_NUM; k++) {
            items[k] = s_w.cur[cell0 + k];
        }
        if (mFI_SetFGStructure_cells(items, d->actor_name, d->block.x, d->block.z, d->unit.x, d->unit.z)) {
            for (k = 0; k < UT_TOTAL_NUM; k++) {
                s_w.cur[cell0 + k] = items[k];
            }
        }
    }
}

static void mp_cot_stored(unsigned int off, unsigned short name, void* arg) {
    int c = mp_cot_cell_of_off(off);

    (void)arg;
    if (c >= 0) {
        s_w.cur[c] = name;
    }
}

// guest: the cell stays as the player saw it while the player is in the middle of it
static void mp_pin(int c) {
    if (s_w.pend[c] == 0 && s_w.npins < MP_LOCKS_MAX) {
        s_w.pend[c] = MP_PIN_OP;
        s_w.pred[c] = s_w.exp[c];
        s_w.pins[s_w.npins++] = (u16)c;
    }
}

// what the local player is in the middle of: those cells stay as it saw them (a guest pins them, the host refuses
// others' ops on them). Looked at before the messages each frame too, so a menu opened meanwhile counts.
static void mp_locks_take(GAME_PLAY* play) {
    int i;

    mp_locks_clear();
    memset(s_cot.mine, 0, sizeof(s_cot.mine));
    if (play == NULL) {
        // (an NES game keeps what the host had in hand as it sat down)
        for (i = 0; mp_emu_active() && i < UT_TOTAL_NUM; i++) {
            if (MP_BIT_GET(s_cot.away, i)) {
                int layer;

                MP_BIT_SET(s_cot.mine, i);
                for (layer = 0; layer < 4; layer++) {
                    mp_lock_hold(MP_COT_BASE + layer * UT_TOTAL_NUM + i);
                }
            }
        }
        mp_cot_busy_calc(FALSE);
        return;
    }
    memset(s_cot.away, 0, sizeof(s_cot.away));
    mp_pickup_lock(play);
    if (s_cot.inside && Save_Get(scene_no) == SCENE_COTTAGE_MY) {
        u8 cells[MP_COT_WORDS / 8];
        u8 hold[MP_COT_WORDS / 8];

        aMR_mp_locks(cells, hold, s_cot.mine);
        for (i = 0; i < MP_COT_WORDS; i++) {
            if (cells[i >> 3] == 0) {
                i |= 7;
            } else if (MP_BIT_GET(hold, i)) {
                mp_lock_hold(MP_COT_BASE + i);
            } else if (MP_BIT_GET(cells, i)) {
                mp_lock(MP_COT_BASE + i);
            }
        }
        // (the carpet, wallpaper and music box change inside the pockets' and the music's menus; a carpet or
        // wallpaper is laid just after)
        if (play->submenu.menu_type == mSM_OVL_INVENTORY || play->submenu.menu_type == mSM_OVL_MUSIC ||
            aMI_mp_reserved()) {
            for (i = MP_CMUS_BASE; i < MP_CELLS; i++) {
                mp_lock_hold(i);
            }
        }
    }
    for (i = 0; !mp_is_host() && i < s_w.nlocks; i++) {
        mp_pin(s_w.locks[i]);
    }
    mp_cot_busy_calc(FALSE);
}

static void mp_capture(GAME_PLAY* play) {
    int r;

    for (r = 0; r < MP_RG_NUM; r++) {
        if (s_rg[r].map != MP_MAP_NONE) {
            int w;

            for (w = 0; w < s_rg[r].words; w++) {
                s_w.cur[s_rg[r].base + w] = mp_map_get(r, w);
            }
        } else if (!s_rg[r].bits && s_rg[r].stride != 0) {
            int w;

            for (w = 0; w < s_rg[r].words; w++) {
                s_w.cur[s_rg[r].base + w] = *mp_word(r, w);
            }
        } else if (!s_rg[r].bits) {
            memcpy(&s_w.cur[s_rg[r].base], mp_word(r, 0), (size_t)s_rg[r].words * 2);
        } else {
            int w;

            for (w = 0; w < s_rg[r].words; w++) {
                u16 word = *mp_word(r, w);
                u16* out = &s_w.cur[s_rg[r].base + w * 16];
                int b;

                for (b = 0; b < 16; b++) {
                    out[b] = (u16)((word >> b) & 1);
                }
            }
        }
    }
    memset(s_w.held, 0, (MP_CELLS + 7) / 8);
    if (play != NULL) {
        mp_world_actor_cells(play, mp_actor_cell, NULL);
    }
    mp_locks_take(play);
    // what the cottage's furniture holds while its room is up
    if (s_cot.inside && Save_Get(scene_no) == SCENE_COTTAGE_MY) {
        aMR_pc_stored_cells(mp_cot_stored, NULL);
    }
    if (mp_is_host()) {
        mp_host_struct_overlay();
    }
}

extern void play_main(GAME* game);

// the running play game, or NULL between scenes and in other game states
GAME_PLAY* mp_live_play(void) {
    GAME* game = gamePT;

    return (game != NULL && game->exec == play_main) ? (GAME_PLAY*)game : NULL;
}

static void mp_refresh_field(void) {
    if (s_refresh) {
        s_refresh = FALSE;
        mFI_SetFGUpData();
    }
}

// lifecycle

static int mp_world_alloc(void) {
    if (s_w.cur != NULL) {
        return TRUE;
    }
    mp_gen_build();
    s_w.cur = (u16*)mp_alloc(MP_CELLS * 2);
    s_w.view = (u16*)mp_alloc(MP_CELLS * 2);
    s_w.pred = (u16*)mp_alloc(MP_CELLS * 2);
    s_w.pend = (u16*)mp_calloc(MP_CELLS, 2);
    s_w.exp = (u16*)mp_alloc(MP_CELLS * 2);
    s_w.held = (u8*)mp_calloc((MP_CELLS + 7) / 8, 1);
    s_w.force = (u8*)mp_calloc((MP_CELLS + 7) / 8, 1);
    s_w.lock = (u8*)mp_calloc((MP_CELLS + 7) / 8, 1);
    s_w.hold = (u8*)mp_calloc((MP_CELLS + 7) / 8, 1);
    s_w.locks = (u16*)mp_alloc(MP_LOCKS_MAX * 2);
    s_w.pins = (u16*)mp_alloc(MP_LOCKS_MAX * 2);
    s_w.gen_shadow = (u8*)mp_alloc(s_gen_bytes);
    if (s_w.cur == NULL || s_w.view == NULL || s_w.pred == NULL || s_w.pend == NULL || s_w.exp == NULL ||
        s_w.held == NULL || s_w.force == NULL || s_w.lock == NULL || s_w.hold == NULL || s_w.locks == NULL || s_w.pins == NULL ||
        s_w.gen_shadow == NULL) {
        pc_mp_log("[MP] world: out of memory");
        return FALSE;
    }
    return TRUE;
}

static void mp_tk_reset(void);
static void mp_tk_mark(void);
static void mp_tk_follow(void);
static void mp_tk_arrived(mActor_name_t item);

static void mp_world_free(void) {
    int s;

    mp_free(s_w.cur);
    mp_free(s_w.view);
    mp_free(s_w.pred);
    mp_free(s_w.pend);
    mp_free(s_w.exp);
    mp_free(s_w.held);
    mp_free(s_w.force);
    mp_free(s_w.lock);
    mp_free(s_w.hold);
    mp_free(s_w.locks);
    mp_free(s_w.pins);
    mp_free(s_w.gen_shadow);
    for (s = 0; s < MP_MAX_PEERS; s++) {
        mp_free(s_w.guests[s].out);
    }
    mp_free(s_w.host_out);
    memset(&s_w, 0, sizeof(s_w));
    mp_tk_reset();
    // (a room of the cottage up stays up)
    s = s_cot.inside;
    memset(&s_cot, 0, sizeof(s_cot));
    s_cot.inside = (u8)s;
}

static void mp_host_floor_take(void);
static const u8* mp_gen_live(u32 off, u32 len, u8* tmp);

// both sides start from the save as it stands: the host's own (the floor it's in as leaving it would save it, the
// way its snapshots have it), or the snapshot the guest rode in on
static void mp_world_begin(GAME_PLAY* play) {
    u8 tmp[400];
    u32 lin;

    if (!mp_world_alloc()) {
        mp_world_free();
        return;
    }
    mp_capture(play);
    memcpy(s_w.view, s_w.cur, MP_CELLS * 2);
    memcpy(s_w.exp, s_w.cur, MP_CELLS * 2);
    memset(s_w.pend, 0, MP_CELLS * 2);
    if (mp_is_host()) {
        mp_host_floor_take();
    }
    for (lin = 0; lin < s_gen_bytes;) {
        u32 left;
        u32 off = mp_gen_save_off(lin, &left);
        u32 n = left < sizeof(tmp) ? left : sizeof(tmp);

        memcpy(s_w.gen_shadow + lin, mp_is_host() ? mp_gen_live(off, n, tmp) : mp_save_base() + off, n);
        lin += n;
    }
    s_w.last_crc_ms = pc_mp_now_ms(); // (an arrival has its own round)
    s_w.shop_last = Save_Get(shop);
    s_w.melody_last = Save_Get(melody);
    s_w.next_op = 1;
    // (a host already in its cottage keeps its table-tops' facings: they become everyone's)
    s_cot.angles_apply = FALSE;
    s_w.live = TRUE;
}

void mp_world_host_begin(void) {
    if (!s_w.live) {
        mp_world_begin(mp_live_play());
    }
}

const unsigned char* mp_world_heard(unsigned int save_off, unsigned int len) {
    if (!s_w.live || s_w.gen_shadow == NULL || !mp_gen_contains(save_off, len)) {
        return NULL;
    }
    return s_w.gen_shadow + mp_gen_lin_of(save_off);
}

// the host takes it from us, so the checksums in between aren't a difference
void mp_world_heard_mine(unsigned int save_off, unsigned int len) {
    if (s_w.live && s_w.gen_shadow != NULL && mp_gen_contains(save_off, len)) {
        memcpy(s_w.gen_shadow + mp_gen_lin_of(save_off), mp_save_base() + save_off, len);
    }
}

// like a change of the host's own, but at once: the sender already has it
void mp_world_host_took(unsigned int save_off, unsigned int len, int from_slot) {
    u8 msg[MP_MSG_MAX];

    if (!s_w.live || s_w.gen_shadow == NULL || len > MP_MSG_MAX - 7 || !mp_gen_contains(save_off, len)) {
        return;
    }
    memcpy(s_w.gen_shadow + mp_gen_lin_of(save_off), mp_save_base() + save_off, len);
    msg[0] = MP_M_BYTES;
    mp_put32w(msg + 1, save_off);
    mp_put16(msg + 5, len);
    memcpy(msg + 7, mp_save_base() + save_off, len);
    mp_send_all(msg, 7 + (int)len, from_slot);
}

// host: a message for the visitors in town, queued behind the town traffic already going to them
void mp_world_queue_guests(const unsigned char* msg, int len, int except_slot) {
    if (s_w.live) {
        mp_send_all(msg, len, except_slot);
    }
}

// the bytes of a span that differ from an earlier copy of it, at once, in runs that fit a message
void mp_world_host_took_diff(unsigned int save_off, const void* before, unsigned int size, int from_slot) {
    const u8* was = (const u8*)before;
    const u8* now = mp_save_base() + save_off;
    u32 i = 0;

    while (i < size) {
        u32 last = i;
        u32 k;

        if (was[i] == now[i]) {
            i++;
            continue;
        }
        // a run takes in short stretches of equal bytes rather than start another message
        for (k = i + 1; k < size && k - i < MP_MSG_MAX - 7 && k - last <= 8; k++) {
            if (was[k] != now[k]) {
                last = k;
            }
        }
        mp_world_host_took(save_off + i, last + 1 - i, from_slot);
        i = last + 1;
    }
}

void mp_world_ask_commit(void) {
    if (mp_is_host() && s_w.live) {
        s_w.save_asked = TRUE;
    }
}

// the train scene has no town actors, so the replica's raw cells are the logical ones
void mp_world_guest_arrived(void) {
    mp_world_free();
    mp_world_begin(NULL);
    s_w.host_out = mp_queue_new();
}

static void mp_guest_floor_release(void);

void mp_world_end(void) {
    mp_guest_floor_release();
    mp_world_free();
}

// guest: ops and pockets

static mp_op_t* mp_op_find(u16 id) {
    int i;

    for (i = 0; i < MP_OPS; i++) {
        if (s_w.ops[i].state != MP_OPS_FREE && s_w.ops[i].id == id) {
            return &s_w.ops[i];
        }
    }
    return NULL;
}

static mp_op_t* mp_op_new(void) {
    mp_op_t* oldest = NULL;
    int i;

    for (i = 0; i < MP_OPS; i++) {
        if (s_w.ops[i].state == MP_OPS_FREE) {
            memset(&s_w.ops[i], 0, sizeof(s_w.ops[i]));
            s_w.ops[i].id = s_w.next_op++;
            if (s_w.next_op == 0 || s_w.next_op == MP_PIN_OP) {
                s_w.next_op = 1;
            }
            return &s_w.ops[i];
        }
        if (s_w.ops[i].state != MP_OPS_SENT && s_w.ops[i].state != MP_OPS_HELD &&
            (oldest == NULL || (s16)(s_w.ops[i].id - oldest->id) < 0)) {
            oldest = &s_w.ops[i];
        }
    }
    if (oldest == NULL) {
        return NULL; // everything is still in flight
    }
    memset(oldest, 0, sizeof(*oldest));
    oldest->id = s_w.next_op++;
    if (s_w.next_op == 0 || s_w.next_op == MP_PIN_OP) {
        s_w.next_op = 1;
    }
    return oldest;
}

static void mp_set_exp(int cell) {
    s_w.exp[cell] = s_w.pend[cell] ? s_w.pred[cell] : s_w.view[cell];
}

static mp_xfer_t* mp_xfer_new(int kind, u16 op, mActor_name_t item) {
    mp_xfer_t* x = NULL;
    int i;

    for (i = 0; i < MP_XFERS; i++) {
        if (!s_w.xfers[i].used) {
            x = &s_w.xfers[i];
            break;
        }
    }
    // (else a saved one makes room, then the oldest drop, then a pickup that never reached the pockets)
    for (i = 0; x == NULL && i < MP_XFERS; i++) {
        if (s_w.xfers[i].committed) {
            x = &s_w.xfers[i];
        }
    }
    for (i = 0; x == NULL && i < MP_XFERS; i++) {
        mp_xfer_t* y = &s_w.xfers[i];

        if (y->kind == MP_XFER_DROP || !y->in_pocket) {
            x = y;
        }
    }
    if (x == NULL) {
        x = &s_w.xfers[0];
        pc_mp_log("[MP] world: transfer records full, one unsaved pickup forgotten");
    }
    memset(x, 0, sizeof(*x));
    x->used = TRUE;
    x->kind = (u8)kind;
    x->op = op;
    x->item = item;
    x->t_ms = pc_mp_now_ms();
    return x;
}

static int mp_pocket_take(mActor_name_t item) {
    Private_c* priv = Now_Private;
    int i;

    for (i = 0; priv != NULL && i < mPr_POCKETS_SLOT_COUNT; i++) {
        if (priv->inventory.pockets[i] == item) {
            mPr_SetPossessionItem(priv, i, EMPTY_NO, mPr_ITEM_COND_NORMAL);
            s_w.pocket_prev[i] = EMPTY_NO;
            return TRUE;
        }
    }
    return FALSE;
}

static int mp_pocket_give(mActor_name_t item) {
    Private_c* priv = Now_Private;
    int i;

    for (i = 0; priv != NULL && i < mPr_POCKETS_SLOT_COUNT; i++) {
        if (priv->inventory.pockets[i] == EMPTY_NO) {
            mPr_SetPossessionItem(priv, i, item, mPr_ITEM_COND_NORMAL);
            s_w.pocket_prev[i] = item;
            return TRUE;
        }
    }
    return FALSE;
}

static int mp_is_item(mActor_name_t v) {
    return v != EMPTY_NO && v != RSV_NO && !ITEM_IS_HOLE(v);
}

// what a cell's item is in a pocket: the cottage's furniture without its facing (clothes and such as themselves);
// nothing for a design stand (it goes nowhere when taken up) or a stereo's record, whose record is the music box's
static u16 mp_cell_pocket(int cell, u16 v, int here) {
    if (mp_rg_of(cell) != MP_RG_COT || !mp_is_item(v)) {
        return v;
    }
    if (ITEM_IS_FTR(v)) {
        u16 base = (u16)(v & ~3);
        u16 f = mRmTp_FtrItemNo2Item1ItemNo(base, TRUE);

        return (ITEM_IS_MYMANNIQUIN(base) || ITEM_IS_MYUMBRELLA(base) || ITEM_IS_MYMANNIQUIN(f) ||
                ITEM_IS_MYUMBRELLA(f))
                   ? EMPTY_NO
                   : f;
    }
    if (here && v >= ITM_MINIDISK_START && v < ITM_MINIDISK_END &&
        aMR_mp_music_slot((u32)((u8*)mp_word(MP_RG_COT, cell - MP_COT_BASE) - mp_save_base()))) {
        return EMPTY_NO;
    }
    return v;
}

// eaten things leave the pockets for good: no drop claims them
#define MP_EATEN 4
static struct {
    mActor_name_t item;
    u32 t_ms;
} s_eaten[MP_EATEN];

void mp_world_eaten(unsigned short item) {
    memmove(&s_eaten[1], &s_eaten[0], sizeof(s_eaten[0]) * (MP_EATEN - 1));
    s_eaten[0].item = item;
    s_eaten[0].t_ms = pc_mp_now_ms();
}

static int mp_eaten_take(mActor_name_t item) {
    int k;

    for (k = 0; k < MP_EATEN; k++) {
        if (s_eaten[k].item == item && pc_mp_now_ms() - s_eaten[k].t_ms < 30000) {
            s_eaten[k].item = EMPTY_NO;
            return TRUE;
        }
    }
    return FALSE;
}

static int mp_arrivals_of(mActor_name_t item) {
    int n = 0;
    int a;

    for (a = 0; a < MP_ARRIVALS; a++) {
        n += s_w.arrivals[a].item == item;
    }
    return n;
}

static int mp_op_cottage(const mp_op_t* op) {
    int i;

    for (i = 0; i < op->n; i++) {
        if (op->cell[i] >= MP_COT_BASE) {
            return TRUE;
        }
    }
    return FALSE;
}

static int mp_cot_held(void) {
    int n = 0;
    int k;

    for (k = 0; k < s_w.nheld; k++) {
        mp_op_t* op = mp_op_find(s_w.held_ops[k]);

        n += op != NULL && op->state == MP_OPS_HELD && mp_op_cottage(op);
    }
    return n;
}

// before the diff: what left the pockets (drops need it) and what came in this frame
static void mp_pockets_scan(void) {
    Private_c* priv = Now_Private;
    u32 now = pc_mp_now_ms();
    int i;

    if (priv == NULL) {
        return;
    }
    if (!s_w.pocket_valid) {
        memcpy(s_w.pocket_prev, priv->inventory.pockets, sizeof(s_w.pocket_prev));
        s_w.pocket_valid = TRUE;
        return;
    }
    for (i = 0; i < mPr_POCKETS_SLOT_COUNT; i++) {
        mActor_name_t was = s_w.pocket_prev[i];
        mActor_name_t now_item = priv->inventory.pockets[i];

        if (was == now_item) {
            continue;
        }
        s_w.pocket_prev[i] = now_item;
        if (was != EMPTY_NO && !mp_eaten_take(was)) {
            memmove(&s_w.losses[1], &s_w.losses[0], sizeof(s_w.losses[0]) * (MP_LOSSES - 1));
            s_w.losses[0].item = was;
            s_w.losses[0].t_ms = now;
        }
        if (now_item != EMPTY_NO) {
            memmove(&s_w.arrivals[1], &s_w.arrivals[0], sizeof(s_w.arrivals[0]) * (MP_ARRIVALS - 1));
            s_w.arrivals[0].item = now_item;
            s_w.arrivals[0].t_ms = now;
        }
    }
}

// an arrival's pickup: the oldest waiting for this very item, else (loose) any fresh town one, for items that change
// on the way (a cottage pickup only ever takes its own item)
static mp_xfer_t* mp_pickup_for(mActor_name_t item, int loose, u32 now) {
    mp_xfer_t* best = NULL;
    int k;

    for (k = 0; k < MP_XFERS; k++) {
        mp_xfer_t* x = &s_w.xfers[k];

        if (!x->used || x->kind != MP_XFER_PICKUP || x->in_pocket ||
            now - x->t_ms > (x->committed ? MP_PICKUP_MS : MP_PICKUP_WAIT_MS)) {
            continue;
        }
        if (!loose ? mp_take_matches(x->item, item) && (best == NULL || x->t_ms < best->t_ms)
                   : best == NULL && !x->cot && now - x->t_ms < 2000) {
            best = x;
        }
    }
    return best;
}

// after the diff: each arrival goes to the pickup it came from, which may have been made this frame (every arrival of
// the very item first)
static void mp_pockets_match(void) {
    u32 now = pc_mp_now_ms();
    int loose;
    int a;

    for (loose = FALSE; loose <= TRUE; loose++) {
        for (a = MP_ARRIVALS - 1; a >= 0; a--) {
            mp_loss_t* in = &s_w.arrivals[a];
            mp_xfer_t* best;

            if (in->item == EMPTY_NO) {
                continue;
            }
            best = mp_pickup_for(in->item, loose, now);
            if (best != NULL) {
                mp_op_t* op = mp_op_find(best->op);

                best->in_pocket = TRUE;
                best->item = in->item;
                mp_tk_arrived(best->item);
                if (op != NULL && op->state == MP_OPS_REJECTED) {
                    // the host said no before the pickup finished
                    mp_tk_mark();
                    mp_pocket_take(best->item);
                    mp_tk_follow();
                    best->used = FALSE;
                }
                in->item = EMPTY_NO;
            } else if (loose && now - in->t_ms > 2000 && !mp_cot_keeps(in->t_ms, 2000)) {
                in->item = EMPTY_NO; // bought, given or dug up: not from the town's ground
            }
        }
    }
}

enum {
    MP_SOFT_SLOT,
    MP_SOFT_BAGS,
    MP_SOFT_SALES,
    MP_SOFT_VISITOR,
    MP_SOFT_KEEP, // a refused drop with no pocket left to return to: the lost & found takes it
    MP_SOFT_MELODY, // a visitor set the town tune, as they could by card
    MP_SOFT_BOARD,  // a visitor's bulletin board post; the host's board is the one that stays
    MP_SOFT_DESIGN, // half of a visitor's design for a stand or the island's flag
    MP_SOFT_TOWNKEEP, // what a visitor's game would have moved out of the way to the lost & found
    MP_SOFT_CLAIM,    // a visitor's purchase of a one-of-a-kind thing begins
    MP_SOFT_CLAIMED,  // ...and went through
    MP_SOFT_RAFFLE,   // a visitor won a prize of Nook's raffle
    MP_SOFT_ISLAND,   // furniture a visitor gave the islander, or had from it
    MP_SOFT_UNCLAIM,  // a visitor's purchase begun and not made
    MP_SOFT_HOLD,     // a visitor's passport stands still, or moves again
    MP_SOFT_LIGHTHOUSE, // a visitor lit the lighthouse tonight: the town's record has the night
};

#define MP_DESIGN_HALF (sizeof(mNW_original_design_c) / 2)

static mNW_original_design_c* mp_design_of(int target) {
    return target < mNW_TOTAL_DESIGN_NUM ? &Save_Get(needlework).original_design[target]
                                         : Save_GetPointer(island.flag_design);
}

static int mp_pocketable(mActor_name_t v) {
    return ITEM_IS_ITEM1(v) || ITEM_IS_FTR(v);
}

// what a pocket gets from taking a town cell: a buried pitfall digs up as its seed, a shining spot as a bag of bells
// (whichever it rolls), a signboard comes up as one; EMPTY_NO for nothing
static u16 mp_town_take(u16 v) {
    if (ITEM_IS_BURIED_PITFALL_HOLE(v)) {
        return ITM_PITFALL;
    }
    if (v == SHINE_SPOT) {
        return SHINE_SPOT;
    }
    if (ITEM_IS_SIGNBOARD(v)) {
        return ITM_SIGNBOARD;
    }
    return mp_pocketable(v) ? v : EMPTY_NO;
}

static int mp_take_matches(u16 took, u16 item) {
    return took == item || (took == SHINE_SPOT && item >= ITM_MONEY_1000 && item <= ITM_MONEY_30000);
}

// a refused pickup whose item was already set down again: the drop that did it gives nothing back
static void mp_owe(mActor_name_t item) {
    if (s_w.nowed < MP_OWED) {
        s_w.owed[s_w.nowed++] = item;
    }
}

static int mp_owed(mActor_name_t item) {
    int i;

    for (i = 0; i < s_w.nowed; i++) {
        if (s_w.owed[i] == item) {
            s_w.owed[i] = s_w.owed[--s_w.nowed];
            return TRUE;
        }
    }
    return FALSE;
}

static int mp_xfer_unsaved(mActor_name_t item);

static int mp_pocket_count(mActor_name_t item) {
    Private_c* priv = Now_Private;
    int n = 0;
    int i;

    for (i = 0; priv != NULL && i < mPr_POCKETS_SLOT_COUNT; i++) {
        n += priv->inventory.pockets[i] == item;
    }
    return n;
}

// a pickup of the item still waiting to go to the host: the item is the town's till it goes (on the way home it's
// undone, and takes nothing back from the pockets)
static int mp_xfer_unsent(mActor_name_t item, int settle) {
    int k;

    for (k = 0; k < MP_XFERS; k++) {
        mp_xfer_t* x = &s_w.xfers[k];
        mp_op_t* o;

        if (x->used && x->kind == MP_XFER_PICKUP && x->in_pocket && !x->committed && !x->settled &&
            x->item == item && (o = mp_op_find(x->op)) != NULL &&
            (o->state == MP_OPS_HELD || (settle && o->state == MP_OPS_SENT))) {
            x->settled = (u8)settle;
            return TRUE;
        }
    }
    return FALSE;
}

// a pickup of the item after the host save the passport on the card has (the host gives those back)
static int mp_xfer_since_card(mActor_name_t item) {
    u32 mark = mp_passport_mark();
    int k;

    for (k = 0; k < MP_XFERS; k++) {
        mp_xfer_t* x = &s_w.xfers[k];

        if (x->used && x->kind == MP_XFER_PICKUP && x->in_pocket && x->item == item &&
            ((mark >> 16) == 0 || (s16)(x->op - (u16)mark) > 0)) {
            return TRUE;
        }
    }
    return FALSE;
}

static int mp_xfer_unsaved_pickup(const mp_xfer_t* x);

// the newest pickup of the item the host has but hasn't saved: settled here as the host drops its record
static void mp_xfer_settle_sent(mActor_name_t item) {
    mp_xfer_t* best = NULL;
    int k;

    for (k = 0; k < MP_XFERS; k++) {
        mp_xfer_t* x = &s_w.xfers[k];
        mp_op_t* o;

        if (mp_xfer_unsaved_pickup(x) && x->item == item && (o = mp_op_find(x->op)) != NULL &&
            o->state == MP_OPS_ACCEPTED && (best == NULL || (s16)(x->op - best->op) > 0)) {
            best = x;
        }
    }
    if (best != NULL) {
        best->settled = TRUE;
    }
}

// an item the host didn't take: back in the pocket, else its lost & found; TRUE when pocketed
static int mp_give_back(mActor_name_t item, u16 op) {
    u8 msg[7];
    int tied = FALSE;

    if (mp_owed(item)) {
        return FALSE;
    }
    if (s_w.recall_pp) {
        mp_op_t* o = mp_op_find(op);

        if (o == NULL || o->gen == 0 || o->gen > s_w.pp_done_gen || mp_xfer_since_card(item)) {
            return FALSE;
        }
    } else {
        if (mp_pocket_give(item)) {
            return TRUE;
        }
        if (s_w.recalling && mp_xfer_unsent(item, TRUE)) {
            return FALSE;
        }
        tied = mp_xfer_unsaved(item) > mp_pocket_count(item);
        if (tied) {
            mp_xfer_settle_sent(item);
        }
    }
    msg[0] = MP_M_SOFT;
    msg[1] = MP_SOFT_KEEP;
    mp_put16(msg + 2, item);
    mp_put16(msg + 4, op);
    msg[6] = (u8)tied; // (in place of the host's record of its pickup)
    mp_queue_push(s_w.host_out, msg, 7);
    return FALSE;
}

// bells back into a wallet, and what it can't hold as bags of bells into free pockets
static void mp_wallet_add(Private_c* priv, u32 bells) {
    static const u32 bag[] = { 30000, 10000, 1000, 100 };
    static const mActor_name_t bag_item[] = { ITM_MONEY_30000, ITM_MONEY_10000, ITM_MONEY_1000, ITM_MONEY_100 };
    u32 room = priv->inventory.wallet < mPr_WALLET_MAX ? mPr_WALLET_MAX - priv->inventory.wallet : 0;
    u32 in = bells < room ? bells : room;
    int b;
    int i;

    priv->inventory.wallet += in;
    bells -= in;
    for (b = 0; b < 4 && bells > 0; b++) {
        for (i = 0; i < mPr_POCKETS_SLOT_COUNT && bells >= bag[b]; i++) {
            if (priv->inventory.pockets[i] == EMPTY_NO) {
                priv->inventory.pockets[i] = bag_item[b];
                bells -= bag[b];
                if (priv == Now_Private) {
                    s_w.pocket_prev[i] = bag_item[b];
                }
            }
        }
    }
    if (bells > 0) {
        pc_mp_log("[MP] world: no room for %u bells of a refund", (unsigned)bells);
    }
}

// a refused bag of bells already emptied into the wallet: its bells come back out
static int mp_wallet_take_bag(mActor_name_t item) {
    static const u32 bells[] = { 1000, 10000, 30000, 100 };
    Private_c* priv = Now_Private;
    u32 b;

    if (priv == NULL || item < ITM_MONEY_1000 || item > ITM_MONEY_100) {
        return FALSE;
    }
    b = bells[item - ITM_MONEY_1000];
    priv->inventory.wallet = priv->inventory.wallet > b ? priv->inventory.wallet - b : 0;
    return TRUE;
}

// talks: what a visitor's talk hands over is the host's only once the talk's result reaches it. A passport staged
// while one is under way has the pockets as it began (with what the network changed since), and a finished talk's
// changes to the pockets come back if its result never goes.
typedef struct {
    mActor_name_t pockets[mPr_POCKETS_SLOT_COUNT];
    u32 cond;
    s32 wallet; // (a refund may take it past the wallet's limit for a while)
} mp_tinv_t;

typedef struct {
    u16 seq;       // the talk's number, as its end message has it
    u8 abandoned;  // its result was dropped whole: only an undo ends it
    u32 floor;     // the newest passport staged as it ended (one staged later has its changes)
    u8 ngave;
    u8 ngot;
    mActor_name_t gave[mPr_POCKETS_SLOT_COUNT];
    u8 gave_cond[mPr_POCKETS_SLOT_COUNT];
    mActor_name_t got[mPr_POCKETS_SLOT_COUNT];
    u8 got_cond[mPr_POCKETS_SLOT_COUNT];
    s32 bells; // what the wallet gained, less what it paid
    mQst_delivery_c deliveries[mPr_DELIVERY_QUEST_NUM]; // the errands as the talk began
    mQst_errand_c errands[mPr_ERRAND_QUEST_NUM];
} mp_tdelta_t;

#define MP_TDELTAS  16
#define MP_TK_LOOSE 16

static struct {
    int on;        // a talk is under way
    mp_tinv_t inv; // ...and the pockets as it began
    mQst_delivery_c deliveries[mPr_DELIVERY_QUEST_NUM];
    mQst_errand_c errands[mPr_ERRAND_QUEST_NUM];
    mActor_name_t loose[MP_TK_LOOSE]; // ...what the network gave that its picture had no room for
    int nloose;
    mActor_name_t pre[MP_TK_LOOSE];   // ...arrivals it already had as it began
    int npre;
    int marking;    // the network is changing the pockets
    mp_tinv_t mark; // ...which were these before
    mp_tdelta_t td[MP_TDELTAS]; // finished talks whose results haven't gone into the line, oldest first
    int ntd;
} s_tk;

static int mp_guest_visiting(void);

static void mp_tinv_get(mp_tinv_t* t, const Private_c* p) {
    memcpy(t->pockets, p->inventory.pockets, sizeof(t->pockets));
    t->cond = p->inventory.item_conditions;
    t->wallet = (s32)p->inventory.wallet;
}

static int mp_tinv_count(const mActor_name_t* pockets, mActor_name_t item) {
    int n = 0;
    int i;

    for (i = 0; i < mPr_POCKETS_SLOT_COUNT; i++) {
        n += pockets[i] == item;
    }
    return n;
}

// the slots of an item one side has more of than the other, the ones it moved out of first
static int mp_tinv_more(const mp_tinv_t* a, const mp_tinv_t* b, mActor_name_t it, mActor_name_t* out, u8* cond,
                        int n) {
    int more = mp_tinv_count(a->pockets, it) - mp_tinv_count(b->pockets, it);
    int pass;
    int k;

    for (pass = 0; pass < 2; pass++) {
        for (k = 0; k < mPr_POCKETS_SLOT_COUNT && more > 0; k++) {
            if (a->pockets[k] == it && (b->pockets[k] == it) == pass) {
                out[n] = it;
                cond[n] = (u8)mPr_GET_ITEM_COND(a->cond, k);
                n++;
                more--;
            }
        }
    }
    return n;
}

// what changed from one to the other: items gone (each with its condition), items come, bells gained
static void mp_tdelta_make(const mp_tinv_t* from, const mp_tinv_t* to, mp_tdelta_t* d) {
    int gave = 0;
    int got = 0;
    int i;
    int k;

    for (i = 0; i < mPr_POCKETS_SLOT_COUNT; i++) {
        mActor_name_t it = from->pockets[i];

        for (k = 0; k < i && from->pockets[k] != it; k++) {
        }
        if (it != EMPTY_NO && k == i) {
            gave = mp_tinv_more(from, to, it, d->gave, d->gave_cond, gave);
        }
        it = to->pockets[i];
        for (k = 0; k < i && to->pockets[k] != it; k++) {
        }
        if (it != EMPTY_NO && k == i) {
            got = mp_tinv_more(to, from, it, d->got, d->got_cond, got);
        }
    }
    d->ngave = (u8)gave;
    d->ngot = (u8)got;
    d->bells = to->wallet - from->wallet;
}

static int mp_tinv_take(mp_tinv_t* t, mActor_name_t item) {
    int k;

    for (k = 0; k < mPr_POCKETS_SLOT_COUNT && t->pockets[k] != item; k++) {
    }
    if (k == mPr_POCKETS_SLOT_COUNT) {
        return FALSE;
    }
    t->pockets[k] = EMPTY_NO;
    t->cond = mPr_SET_ITEM_COND(t->cond, k, mPr_ITEM_COND_NORMAL);
    return TRUE;
}

static void mp_tinv_put(mp_tinv_t* t, mActor_name_t item, u32 cond) {
    int k;

    for (k = 0; k < mPr_POCKETS_SLOT_COUNT && t->pockets[k] != EMPTY_NO; k++) {
    }
    if (k < mPr_POCKETS_SLOT_COUNT) {
        t->pockets[k] = item;
        t->cond = mPr_SET_ITEM_COND(t->cond, k, cond);
    } else if (s_tk.nloose < MP_TK_LOOSE) {
        s_tk.loose[s_tk.nloose++] = item; // (not the talk's: its delta leaves it out)
    }
}

// the network's change to the pockets, onto a talk's picture of them
static void mp_tdelta_forward(mp_tinv_t* t, const mp_tdelta_t* d) {
    int i;

    for (i = 0; i < d->ngave; i++) {
        mp_tinv_take(t, d->gave[i]);
    }
    for (i = 0; i < d->ngot; i++) {
        mp_tinv_put(t, d->got[i], d->got_cond[i]);
    }
    t->wallet += d->bells;
}

// what the talk under way changed so far (the network's own additions it had no room for left out)
static void mp_tk_delta_now(mp_tdelta_t* d) {
    mp_tinv_t now;
    int i;

    mp_tinv_get(&now, Now_Private);
    mp_tdelta_make(&s_tk.inv, &now, d);
    for (i = 0; i < s_tk.nloose; i++) {
        int k;

        for (k = 0; k < d->ngot && d->got[k] != s_tk.loose[i]; k++) {
        }
        if (k < d->ngot) {
            d->ngot--;
            d->got[k] = d->got[d->ngot];
            d->got_cond[k] = d->got_cond[d->ngot];
        }
    }
    memcpy(d->deliveries, s_tk.deliveries, sizeof(d->deliveries));
    memcpy(d->errands, s_tk.errands, sizeof(d->errands));
}

// something a talk handed over whose result never went, with no pocket to come back to: the host's lost & found takes
// it, as for a drop undone before it went
static void mp_keep_unsent(mActor_name_t item) {
    u8 msg[7];
    u16 id = s_w.next_op++;

    if (s_w.next_op == 0 || s_w.next_op == MP_PIN_OP) {
        s_w.next_op = 1;
    }
    msg[0] = MP_M_SOFT;
    msg[1] = MP_SOFT_KEEP;
    mp_put16(msg + 2, item);
    mp_put16(msg + 4, id);
    msg[6] = 0;
    mp_queue_push(s_w.host_out, msg, 7);
}

// a talk's change to the pockets undone: what it took comes back, what it gave goes. All or nothing: when what it gave
// is spent already, what it took stays gone (an item lost, never one twice)
static int mp_tdelta_back(Private_c* p, const mp_tdelta_t* d) {
    int live = p == Now_Private;
    int i;
    int k;

    for (i = 0; i < d->ngot; i++) {
        int want = 0;

        for (k = 0; k < d->ngot; k++) {
            want += d->got[k] == d->got[i];
        }
        if (mp_tinv_count(p->inventory.pockets, d->got[i]) < want) {
            pc_mp_log("[MP] world: a talk's %04X is spent; what it took can't come back", d->got[i]);
            return FALSE;
        }
    }
    if (d->bells > 0 && p->inventory.wallet < (u32)d->bells) {
        pc_mp_log("[MP] world: a talk's bells are spent; what it took can't come back");
        return FALSE;
    }
    for (i = 0; i < d->ngot; i++) {
        for (k = 0; k < mPr_POCKETS_SLOT_COUNT && p->inventory.pockets[k] != d->got[i]; k++) {
        }
        mPr_SetPossessionItem(p, k, EMPTY_NO, mPr_ITEM_COND_NORMAL);
        if (live) {
            s_w.pocket_prev[k] = EMPTY_NO;
        }
    }
    for (i = 0; i < d->ngave; i++) {
        for (k = 0; k < mPr_POCKETS_SLOT_COUNT && p->inventory.pockets[k] != EMPTY_NO; k++) {
        }
        if (k == mPr_POCKETS_SLOT_COUNT) {
            mp_keep_unsent(d->gave[i]);
            continue;
        }
        mPr_SetPossessionItem(p, k, d->gave[i], d->gave_cond[i]);
        if (live) {
            s_w.pocket_prev[k] = d->gave[i];
        }
    }
    if (d->bells > 0) {
        p->inventory.wallet -= (u32)d->bells;
    } else if (d->bells < 0) {
        mp_wallet_add(p, (u32)-d->bells);
    }
    return TRUE;
}

static void mp_tk_reset(void) {
    if (s_tk.on || s_tk.ntd != 0 || s_tk.marking != 0 || s_tk.nloose != 0) {
        memset(&s_tk, 0, sizeof(s_tk));
    }
}

// changes to the pockets that aren't a talk's (the network's): a talk under way has them too
static void mp_tk_mark(void) {
    if (s_tk.on && Now_Private != NULL && s_tk.marking++ == 0) {
        mp_tinv_get(&s_tk.mark, Now_Private);
    }
}

static void mp_tk_follow(void) {
    mp_tinv_t now;
    mp_tdelta_t d;

    if (s_tk.marking == 0 || --s_tk.marking != 0 || !s_tk.on || Now_Private == NULL) {
        return;
    }
    mp_tinv_get(&now, Now_Private);
    mp_tdelta_make(&s_tk.mark, &now, &d);
    mp_tdelta_forward(&s_tk.inv, &d);
}

// an op sent during a talk: what it set down is gone from the talk's picture too, and what it picked up is in it
static void mp_tk_op(mp_op_t* op) {
    int k;

    if (!s_tk.on) {
        return;
    }
    for (k = 0; k < MP_XFERS; k++) {
        mp_xfer_t* x = &s_w.xfers[k];

        if (x->used && x->op == op->id && x->kind == MP_XFER_DROP && Now_Private != NULL &&
            mp_tinv_count(s_tk.inv.pockets, x->item) > mp_tinv_count(Now_Private->inventory.pockets, x->item)) {
            mp_tinv_take(&s_tk.inv, x->item);
        }
    }
}

// ...a pickup of one arriving in the pockets
static void mp_tk_arrived(mActor_name_t item) {
    int k;

    if (!s_tk.on) {
        return;
    }
    for (k = 0; k < s_tk.npre && s_tk.pre[k] != item; k++) {
    }
    if (k < s_tk.npre) {
        s_tk.pre[k] = s_tk.pre[--s_tk.npre];
        return;
    }
    mp_tinv_put(&s_tk.inv, item, mPr_ITEM_COND_NORMAL);
}

// ...and a refused pickup's item a talk had already handed over: it was the town's all along, so no undo gives it back
static void mp_tk_owed(mActor_name_t item) {
    int k;

    if (s_tk.on && Now_Private != NULL &&
        mp_tinv_count(s_tk.inv.pockets, item) > mp_tinv_count(Now_Private->inventory.pockets, item) &&
        mp_tinv_take(&s_tk.inv, item)) {
        return;
    }
    for (k = s_tk.ntd - 1; k >= 0; k--) {
        mp_tdelta_t* d = &s_tk.td[k];
        int i;

        for (i = 0; i < d->ngave && d->gave[i] != item; i++) {
        }
        if (i < d->ngave) {
            d->ngave--;
            d->gave[i] = d->gave[d->ngave];
            d->gave_cond[i] = d->gave_cond[d->ngave];
            return;
        }
    }
}

void mp_world_inv_mark(void) {
    mp_tk_mark();
}

void mp_world_inv_follow(void) {
    mp_tk_follow();
}

// guest: a talk begins
void mp_world_talk_begin(void) {
    int k;

    if (!mp_guest_visiting() || Now_Private == NULL) {
        return;
    }
    mp_tinv_get(&s_tk.inv, Now_Private);
    memcpy(s_tk.deliveries, Now_Private->deliveries, sizeof(s_tk.deliveries));
    memcpy(s_tk.errands, Now_Private->errands, sizeof(s_tk.errands));
    s_tk.nloose = 0;
    s_tk.npre = 0;
    // (gains the pockets have whose pickups haven't been matched yet are in the picture already)
    for (k = 0; k < MP_ARRIVALS && s_tk.npre < MP_TK_LOOSE; k++) {
        if (s_w.arrivals[k].item != EMPTY_NO) {
            s_tk.pre[s_tk.npre++] = s_w.arrivals[k].item;
        }
    }
    for (k = 0; s_w.pocket_valid && k < mPr_POCKETS_SLOT_COUNT && s_tk.npre < MP_TK_LOOSE; k++) {
        if (Now_Private->inventory.pockets[k] != EMPTY_NO &&
            Now_Private->inventory.pockets[k] != s_w.pocket_prev[k]) {
            s_tk.pre[s_tk.npre++] = Now_Private->inventory.pockets[k];
        }
    }
    s_tk.on = TRUE;
}

// ...and ends; one that changed what the host keeps holds on to its change to the pockets till its result goes
void mp_world_talk_end(unsigned short seq, unsigned int floor, int changed) {
    mp_tdelta_t* d;

    if (!s_tk.on) {
        return;
    }
    s_tk.on = FALSE;
    if (!changed || Now_Private == NULL) {
        return;
    }
    if (s_tk.ntd == MP_TDELTAS) {
        pc_mp_log("[MP] world: talks' results piling up; the oldest one's hand-overs can't come back");
        memmove(&s_tk.td[0], &s_tk.td[1], sizeof(s_tk.td[0]) * (MP_TDELTAS - 1));
        s_tk.ntd--;
    }
    d = &s_tk.td[s_tk.ntd];
    mp_tk_delta_now(d);
    d->seq = seq;
    d->floor = floor;
    d->abandoned = FALSE;
    if (d->ngave != 0 || d->ngot != 0 || d->bells != 0) {
        s_tk.ntd++;
    }
}

// a talk's result went into the line: it and those before it are the host's now
void mp_world_talk_gone(unsigned short seq) {
    int kept = 0;
    int k;

    for (k = 0; k < s_tk.ntd; k++) {
        if (s_tk.td[k].abandoned || (s16)(s_tk.td[k].seq - seq) > 0) {
            if (kept != k) {
                s_tk.td[kept] = s_tk.td[k];
            }
            kept++;
        }
    }
    s_tk.ntd = kept;
}

// a talk's result with no room to be held: none of it goes, and what the talk handed over comes back now (on the
// way home it waits for the undo there, which knows which passport the traveller goes home as)
void mp_world_talk_drop(unsigned short seq) {
    int k;

    for (k = 0; k < s_tk.ntd; k++) {
        if (s_tk.td[k].seq != seq || s_tk.td[k].abandoned) {
            continue;
        }
        if (s_w.home_pp || s_w.exiting || Now_Private == NULL) {
            s_tk.td[k].abandoned = TRUE;
            return;
        }
        mp_tk_mark();
        if (mp_tdelta_back(Now_Private, &s_tk.td[k])) {
            memcpy(Now_Private->deliveries, s_tk.td[k].deliveries, sizeof(s_tk.td[k].deliveries));
            memcpy(Now_Private->errands, s_tk.td[k].errands, sizeof(s_tk.td[k].errands));
        }
        mp_tk_follow();
        mp_passport_write_async();
        memmove(&s_tk.td[k], &s_tk.td[k + 1], sizeof(s_tk.td[0]) * (s_tk.ntd - k - 1));
        s_tk.ntd--;
        return;
    }
}

// the way home or the app closing: talks whose results never went into the line give back what they handed over (the
// one under way too). card_seq: priv is that passport on the card, which has only what talks ended before it changed.
// TRUE when anything changed.
int mp_world_talk_undo(void* priv_p, unsigned int card_seq) {
    Private_c* priv = (Private_c*)priv_p;
    const mp_tdelta_t* first = NULL; // (the oldest undone: the errands as it began)
    int changed = FALSE;
    int all = TRUE;
    int k;

    if (priv != NULL && s_tk.on && card_seq == 0 && priv == Now_Private) {
        static mp_tdelta_t s_now;

        mp_tk_delta_now(&s_now);
        if (mp_tdelta_back(priv, &s_now)) {
            changed = TRUE;
            first = &s_now;
        } else {
            all = FALSE;
        }
    }
    for (k = s_tk.ntd - 1; priv != NULL && k >= 0; k--) {
        if (card_seq == 0 || (s32)(card_seq - s_tk.td[k].floor) > 0) {
            if (mp_tdelta_back(priv, &s_tk.td[k])) {
                changed = TRUE;
                first = &s_tk.td[k];
            } else {
                all = FALSE;
            }
        }
    }
    // (errands go back only when every talk after that one did too)
    if (first != NULL && all) {
        memcpy(priv->deliveries, first->deliveries, sizeof(first->deliveries));
        memcpy(priv->errands, first->errands, sizeof(first->errands));
    }
    s_tk.on = FALSE;
    s_tk.ntd = 0;
    return changed;
}

// a passport staged while a talk is under way has the pockets as it began
void mp_world_talk_stage(void* priv_p) {
    Private_c* priv = (Private_c*)priv_p;

    if (!s_tk.on || priv == NULL) {
        return;
    }
    memcpy(priv->inventory.pockets, s_tk.inv.pockets, sizeof(s_tk.inv.pockets));
    priv->inventory.item_conditions = s_tk.inv.cond;
    priv->inventory.wallet = s_tk.inv.wallet < 0              ? 0
                             : s_tk.inv.wallet > mPr_WALLET_MAX ? mPr_WALLET_MAX
                                                                : (u32)s_tk.inv.wallet;
    memcpy(priv->deliveries, s_tk.deliveries, sizeof(s_tk.deliveries));
    memcpy(priv->errands, s_tk.errands, sizeof(s_tk.errands));
}

void mp_world_talk_reset(void) {
    mp_tk_reset();
}

// a drop of that item still waiting to go to the host
static int mp_xfer_dropping(mActor_name_t item) {
    int k;

    for (k = 0; k < MP_XFERS; k++) {
        mp_xfer_t* x = &s_w.xfers[k];
        mp_op_t* op;

        if (x->used && x->kind == MP_XFER_DROP && x->item == item && (op = mp_op_find(x->op)) != NULL &&
            op->state == MP_OPS_HELD) {
            return TRUE;
        }
    }
    return FALSE;
}

// a refused op undoes itself in the pockets (what it took goes first, so what it set down has room to come back); the
// cells take the host's values elsewhere
static void mp_op_compensate(mp_op_t* op) {
    int returned = FALSE;
    int k;

    mp_tk_mark(); // (a talk under way keeps what this changes: it isn't the talk's doing)
    for (k = 0; k < MP_XFERS; k++) {
        mp_xfer_t* x = &s_w.xfers[k];

        if (!x->used || x->op != op->id || x->kind != MP_XFER_PICKUP) {
            continue;
        }
        // a gyroid that didn't sell it: what it cost comes back
        if (x->price != 0 && Now_Private != NULL) {
            mp_wallet_add(Now_Private, x->price);
            x->price = 0;
            returned = TRUE;
        }
        if (x->settled) {
            x->used = FALSE;
        } else if (x->in_pocket) {
            if (!mp_pocket_take(x->item)) {
                if (mp_xfer_dropping(x->item)) {
                    mp_owe(x->item);
                } else if (!mp_wallet_take_bag(x->item)) {
                    mp_owe(x->item);
                    mp_tk_owed(x->item);
                }
            }
            x->used = FALSE;
        }
        // (not in the pocket yet: mp_pockets_match removes it on arrival)
    }
    for (k = 0; k < MP_XFERS; k++) {
        mp_xfer_t* x = &s_w.xfers[k];

        if (x->used && x->op == op->id && x->kind == MP_XFER_DROP) {
            returned |= mp_give_back(x->item, op->id);
            x->used = FALSE;
        }
    }
    mp_tk_follow();
    // the passport left it out when it was dropped
    if (returned) {
        mp_passport_write_async();
    }
}

static int mp_xfer_unsaved_pickup(const mp_xfer_t* x) {
    return x->used && x->kind == MP_XFER_PICKUP && x->in_pocket && !x->committed && !x->settled;
}

// (the first unsaved pickup of its item)
static int mp_xfer_seen_before(int k) {
    int i;

    for (i = 0; i < k; i++) {
        if (mp_xfer_unsaved_pickup(&s_w.xfers[i]) && s_w.xfers[i].item == s_w.xfers[k].item) {
            return TRUE;
        }
    }
    return FALSE;
}

// pickups of an item the host hasn't saved, less those set down again since (the host has those back)
static int mp_xfer_unsaved(mActor_name_t item) {
    int n = 0;
    int k;

    for (k = 0; k < MP_XFERS; k++) {
        mp_xfer_t* x = &s_w.xfers[k];
        mp_op_t* op;

        if (!x->used || x->committed || x->settled || x->item != item) {
            continue;
        }
        if (x->kind == MP_XFER_PICKUP && x->in_pocket) {
            n++;
        } else if (x->kind == MP_XFER_DROP && ((op = mp_op_find(x->op)) == NULL || op->state != MP_OPS_REJECTED)) {
            n--;
        }
    }
    return n > 0 ? n : 0;
}

// guest: a pickup the host hasn't saved was used up here (a bag in the wallet, sold, given, opened): nothing the
// passport could leave out stands for it, so the passport stays as it last was until the host saves it
int mp_world_passport_hold(void) {
    Private_c* priv = Now_Private;
    int k;

    if (!s_w.live || mp_is_host() || priv == NULL) {
        return FALSE;
    }
    for (k = 0; k < MP_XFERS; k++) {
        int held;
        int i;

        if (!mp_xfer_unsaved_pickup(&s_w.xfers[k]) || mp_xfer_seen_before(k)) {
            continue;
        }
        held = mp_xfer_unsaved(s_w.xfers[k].item);
        for (i = 0; i < mPr_POCKETS_SLOT_COUNT; i++) {
            held -= priv->inventory.pockets[i] == s_w.xfers[k].item;
        }
        if (held > 0) {
            return TRUE;
        }
    }
    return FALSE;
}

// the passport never carries a pickup the host hasn't saved yet
void mp_world_passport_filter(void* priv_p) {
    Private_c* priv = (Private_c*)priv_p;
    mActor_name_t live[mPr_POCKETS_SLOT_COUNT];
    int k;

    if (!s_w.live || priv == NULL) {
        return;
    }
    memcpy(live, priv->inventory.pockets, sizeof(live));
    for (k = 0; k < MP_XFERS; k++) {
        mp_xfer_t* x = &s_w.xfers[k];
        int left;
        int i;

        if (!mp_xfer_unsaved_pickup(x) || mp_xfer_seen_before(k)) {
            continue;
        }
        left = mp_xfer_unsaved(x->item);
        for (i = 0; i < mPr_POCKETS_SLOT_COUNT && left > 0; i++) {
            if (priv->inventory.pockets[i] == x->item) {
                priv->inventory.pockets[i] = EMPTY_NO;
                left--;
            }
        }
        // (nor the bells a gyroid purchase not yet saved paid for one it left out)
        left = mp_xfer_unsaved(x->item) - left;
        for (i = k; i < MP_XFERS && left > 0; i++) {
            if (mp_xfer_unsaved_pickup(&s_w.xfers[i]) && s_w.xfers[i].item == x->item) {
                mp_wallet_add(priv, s_w.xfers[i].price);
                left--;
            }
        }
    }
    // (nor what came to the pockets while the player is at something in the cottage whose op hasn't gone yet, less
    // what left them meanwhile, as an item moved between pockets does: a game closed meanwhile leaves it there)
    for (k = 0; k < MP_ARRIVALS; k++) {
        mActor_name_t item = s_w.arrivals[k].item;
        int gained = 0;
        int i;

        if (item == EMPTY_NO || !mp_cot_keeps(s_w.arrivals[k].t_ms, 2000)) {
            continue;
        }
        // (each item once, at its first)
        for (i = 0; i < MP_ARRIVALS; i++) {
            if (s_w.arrivals[i].item == item && mp_cot_keeps(s_w.arrivals[i].t_ms, 2000)) {
                if (i < k) {
                    break;
                }
                gained++;
            }
        }
        if (i < k) {
            continue;
        }
        for (i = 0; i < MP_LOSSES; i++) {
            gained -= s_w.losses[i].item == item && mp_cot_keeps(s_w.losses[i].t_ms, 2000);
        }
        for (i = 0; i < mPr_POCKETS_SLOT_COUNT && gained > 0; i++) {
            if (priv->inventory.pockets[i] == item) {
                priv->inventory.pockets[i] = EMPTY_NO;
                gained--;
            }
        }
    }
    // Wisp's spirits belong to his night in this town
    for (k = 0; k < mPr_POCKETS_SLOT_COUNT; k++) {
        if (ITEM_IS_WISP(priv->inventory.pockets[k])) {
            priv->inventory.pockets[k] = EMPTY_NO;
        }
    }
    // (nor what a menu of the cottage's handed the pockets since they were last looked at: its op waits for the menu
    // to close, and a game closed meanwhile leaves it there)
    if (!s_w.pocket_valid || !s_cot.inside || Save_Get(scene_no) != SCENE_COTTAGE_MY) {
        return;
    }
    for (k = 0; k < mPr_POCKETS_SLOT_COUNT; k++) {
        int gained = 0;
        int i;

        if (live[k] == EMPTY_NO) {
            continue;
        }
        for (i = 0; i < k && live[i] != live[k]; i++) {
        }
        if (i < k) {
            continue; // (each item once, at its first)
        }
        for (i = 0; i < mPr_POCKETS_SLOT_COUNT; i++) {
            gained += (live[i] == live[k]) - (s_w.pocket_prev[i] == live[k]);
        }
        for (i = 0; i < mPr_POCKETS_SLOT_COUNT && gained > 0; i++) {
            if (priv->inventory.pockets[i] == live[k]) {
                priv->inventory.pockets[i] = EMPTY_NO;
                gained--;
            }
        }
    }
}

// which cells the local player could have touched: fg blocks around them, or the lost & found
static void mp_player_reach(GAME_PLAY* play, int* region, int* pgx, int* pgz) {
    PLAYER_ACTOR* player = GET_PLAYER_ACTOR(play);
    int bx;
    int bz;
    int ut_x;
    int ut_z;
    mActor_name_t* top;

    *region = -1;
    if (mFI_GetFieldId() == mFI_FIELD_ROOM_POLICE_BOX) {
        *region = MP_RG_POLICE;
        return;
    }
    // the cottage: its players', if the host lets them
    if (Save_Get(scene_no) == SCENE_COTTAGE_MY) {
        *region = mp_cot_owner() ? MP_RG_COT : -1;
        return;
    }
    if (player == NULL ||
        !mFI_Wpos2BkandUtNuminBlock(&bx, &bz, &ut_x, &ut_z, player->actor_class.world.position)) {
        return;
    }
    top = mFI_BkNumtoUtFGTop(bx, bz);
    if (top != NULL) {
        int cell = mp_cell_of_save_off((u32)((u8*)(top + ut_x + ut_z * UT_X_NUM) - mp_save_base()));
        int r;

        if (cell >= 0 && mp_cell_pos(cell, pgx, pgz, &r)) {
            *region = r;
        }
    }
}

static int mp_in_reach(int cell, int region, int pgx, int pgz) {
    int r = mp_rg_of(cell);
    int gx;
    int gz;
    int cr;

    if (region < 0) {
        return FALSE;
    }
    if (r >= MP_RG_COT || region == MP_RG_COT) {
        return r >= MP_RG_COT && region == MP_RG_COT;
    }
    if (r == MP_RG_HANIWA) {
        return region == MP_RG_FG;
    }
    if (region == MP_RG_POLICE) {
        return r == MP_RG_POLICE;
    }
    if (r == MP_RG_DEP || r == MP_RG_IDEP) {
        // buried flags follow the fg cell they belong to
        int k = cell - s_rg[r].base;
        int fg_r = (r == MP_RG_DEP) ? MP_RG_FG : MP_RG_IFG;

        cell = s_rg[fg_r].base + (k / (UT_Z_NUM * 16)) * UT_TOTAL_NUM + ((k / 16) % UT_Z_NUM) * UT_X_NUM + (k % 16);
    }
    if (!mp_cell_pos(cell, &gx, &gz, &cr) || cr != region) {
        return FALSE;
    }
    // the three blocks around the player, i.e. what's loaded
    return (gx / UT_X_NUM) - (pgx / UT_X_NUM) >= -1 && (gx / UT_X_NUM) - (pgx / UT_X_NUM) <= 1 &&
           (gz / UT_Z_NUM) - (pgz / UT_Z_NUM) >= -1 && (gz / UT_Z_NUM) - (pgz / UT_Z_NUM) <= 1;
}

// guest: the host was heard from a moment ago
int mp_world_line_ok(void) {
    int conn = mp_lobby_host_conn();

    return conn >= 0 && mp_lobby_conn_silence(conn) <= MP_DOUBT_MS;
}

// the host's line has been silent too long to trust with an item, or its game isn't taking ops (its line still talks)
static int mp_host_doubtful(void) {
    int conn = mp_lobby_host_conn();
    u32 now = pc_mp_now_ms();
    int i;

    if (conn < 0 || mp_lobby_conn_silence(conn) > MP_DOUBT_MS) {
        return TRUE;
    }
    for (i = 0; i < MP_OPS; i++) {
        if (s_w.ops[i].state == MP_OPS_SENT && now - s_w.ops[i].sent_ms > MP_ANSWER_MS) {
            return TRUE;
        }
    }
    return FALSE;
}

// the host never hears of this op: its cells keep the host's values and the pockets are squared
// guest: a cell of an op now answered that the player is back at stays as its room shows it; the player's next op on
// it carries that, for the host to judge
static int mp_repin(int c, u16 v) {
    if (!MP_BIT_GET(s_w.lock, c) || s_w.npins >= MP_LOCKS_MAX) {
        return FALSE;
    }
    s_w.pend[c] = MP_PIN_OP;
    s_w.pred[c] = v;
    s_w.pins[s_w.npins++] = (u16)c;
    return TRUE;
}

static void mp_op_reject_here(mp_op_t* op) {
    int i;

    for (i = 0; i < op->n; i++) {
        int c = op->cell[i];

        if (s_w.pend[c] == op->id) {
            s_w.pend[c] = 0;
            if (mp_repin(c, op->newv[i])) {
                continue;
            }
        }
        mp_set_exp(c);
        if (MP_BIT_GET(s_w.held, c) || MP_BIT_GET(s_w.lock, c)) {
            MP_BIT_SET(s_w.force, c);
        } else if (mp_raw_get(c) != s_w.view[c]) {
            mp_raw_set(c, s_w.view[c]);
        }
    }
    op->state = MP_OPS_REJECTED;
    s_cot.resync |= (u8)mp_op_cottage(op);
    mp_op_compensate(op);
}

// a pocket loss that became this cell: the same item, or what it turned into (a planted fruit, a signboard going up
// as its reserve cell)
static int mp_loss_claim(u16 newv) {
    u32 now = pc_mp_now_ms();
    int best = -1;
    int k;

    for (k = 0; k < MP_LOSSES; k++) {
        u32 age = now - s_w.losses[k].t_ms;

        if (s_w.losses[k].item == EMPTY_NO || (age >= MP_DROP_MS && !mp_cot_keeps(s_w.losses[k].t_ms, MP_DROP_MS))) {
            continue;
        }
        if (s_w.losses[k].item == newv || (newv == RSV_SIGNBOARD && s_w.losses[k].item == ITM_SIGNBOARD)) {
            return k;
        }
        if (best < 0 && age < 1000 && ITEM_NAME_GET_TYPE(newv) == NAME_TYPE_ITEM0) {
            best = k;
        }
    }
    return best;
}

static int mp_op_claim_drop(mp_op_t* op, u16 item) {
    int k = mp_loss_claim(item);
    mp_xfer_t* x;

    if (k < 0) {
        // (a pickup that never reached the pockets, set down beside where it was: full pockets turning a dug-up thing
        // down, say; the host takes that pickup back)
        for (k = 0; k < MP_XFERS; k++) {
            x = &s_w.xfers[k];
            if (x->used && x->kind == MP_XFER_PICKUP && !x->in_pocket && !x->committed && x->item == item &&
                pc_mp_now_ms() - x->t_ms < MP_PICKUP_MS) {
                x->kind = MP_XFER_DROP;
                x->op = op->id;
                x->in_pocket = TRUE;
                x->price = 0;
                return TRUE;
            }
        }
        return FALSE;
    }
    x = mp_xfer_new(MP_XFER_DROP, op->id, s_w.losses[k].item);
    x->in_pocket = TRUE;
    s_w.losses[k].item = EMPTY_NO;
    return TRUE;
}

#define MP_TR_DROP     1 // pocket items went down
#define MP_TR_UNSQUARE 2 // the cottage gained or lost an item no pocket accounts for

// cells that move items between a pocket and the town (MP_TR_*). The cottage's cells count as a whole, as only the
// pockets tell (a dresser closes up behind what was taken out, furniture pushed about or turned is in both, a carpet
// laid over another takes that one up): what's gone from it and came to the pockets was picked up, what's new there
// and left them was set down, and nothing else comes or goes there.
static int mp_op_note_transfers(mp_op_t* op) {
    u16 tgone[MP_OP_CELLS];
    u16 tcame[MP_OP_CELLS];
    u8 tgone_at[MP_OP_CELLS];
    int ntgone = 0;
    int ntcame = 0;
    u16 gone[MP_OP_CELLS];
    u16 came[MP_OP_CELLS];
    u8 gone_at[MP_OP_CELLS];
    u8 came_at[MP_OP_CELLS];
    u8 picked[MP_OP_CELLS];
    u8 claimed[MP_OP_CELLS];
    int ngone = 0;
    int ncame = 0;
    int has_drop = FALSE;
    int unsquare = FALSE;
    int i;
    int j;

    op->plain = 0;
    for (i = 0; i < op->n; i++) {
        int c = op->cell[i];
        u16 oldv = mp_cell_pocket(c, op->oldv[i], TRUE);
        u16 newv = mp_cell_pocket(c, op->newv[i], TRUE);

        if (s_rg[mp_rg_of(c)].bits) {
            continue;
        }
        if (c >= MP_COT_BASE) {
            op->plain |= 1ull << i;
            if (mp_is_item(oldv)) {
                gone[ngone] = oldv;
                gone_at[ngone++] = (u8)i;
            }
            if (mp_is_item(newv)) {
                came[ncame] = newv;
                came_at[ncame++] = (u8)i;
            }
            continue;
        }
        if (mp_town_take(oldv) != mp_town_take(newv)) {
            if (mp_town_take(oldv) != EMPTY_NO) {
                tgone[ntgone] = mp_town_take(oldv);
                tgone_at[ntgone++] = (u8)i;
            }
            if (mp_town_take(newv) != EMPTY_NO) {
                tcame[ntcame++] = mp_town_take(newv);
            }
        }
    }
    // (the town's: what only moved from one of its cells to another)
    for (i = 0; i < ntgone; i++) {
        for (j = 0; j < ntcame; j++) {
            if (tcame[j] != EMPTY_NO && tcame[j] == tgone[i]) {
                tgone[i] = tcame[j] = EMPTY_NO;
                break;
            }
        }
    }
    for (i = 0; i < ntgone; i++) {
        if (tgone[i] != EMPTY_NO) {
            mp_xfer_new(MP_XFER_PICKUP, op->id, tgone[i])->price = mp_hnw_price(op->cell[tgone_at[i]]);
        }
    }
    for (j = 0; j < ntcame; j++) {
        if (tcame[j] != EMPTY_NO && mp_visitor_may(MP_RULE_ITEMS) && mp_op_claim_drop(op, tcame[j])) {
            has_drop = TRUE;
        }
    }
    // (what only moved about inside it)
    for (i = 0; i < ngone; i++) {
        for (j = 0; j < ncame; j++) {
            if (came[j] != EMPTY_NO && came[j] == gone[i]) {
                gone[i] = came[j] = EMPTY_NO;
                break;
            }
        }
    }
    for (i = 0; i < ngone; i++) {
        int before = 0;

        picked[i] = FALSE;
        if (gone[i] == EMPTY_NO || !mp_pocketable(gone[i])) {
            continue;
        }
        for (j = 0; j < i; j++) {
            before += picked[j] && gone[j] == gone[i];
        }
        if (mp_arrivals_of(gone[i]) > before) {
            mp_xfer_new(MP_XFER_PICKUP, op->id, gone[i])->cot = TRUE;
            picked[i] = TRUE;
            op->plain &= ~(1ull << gone_at[i]);
        }
    }
    for (j = 0; j < ncame; j++) {
        claimed[j] = FALSE;
        if (came[j] != EMPTY_NO && mp_op_claim_drop(op, came[j])) {
            has_drop = TRUE;
            claimed[j] = TRUE;
            op->plain &= ~(1ull << came_at[j]);
        }
    }
    // (a record moves with its stereo, which only ever names one of the music box's)
    for (i = 0; i < ngone; i++) {
        unsquare |= gone[i] != EMPTY_NO && mp_pocketable(gone[i]) && !picked[i] &&
                    !(gone[i] >= ITM_MINIDISK_START && gone[i] < ITM_MINIDISK_END);
    }
    for (j = 0; j < ncame; j++) {
        unsquare |= came[j] != EMPTY_NO && mp_pocketable(came[j]) && !claimed[j] &&
                    !(came[j] >= ITM_MINIDISK_START && came[j] < ITM_MINIDISK_END);
    }
    return (has_drop ? MP_TR_DROP : 0) | (unsquare ? MP_TR_UNSQUARE : 0);
}

// a drop of an item whose pickup the host hasn't answered yet: it waits for that answer
static int mp_op_waits(mp_op_t* op) {
    int k;
    int j;

    for (k = 0; k < MP_XFERS; k++) {
        mp_xfer_t* d = &s_w.xfers[k];

        if (!d->used || d->kind != MP_XFER_DROP || d->op != op->id) {
            continue;
        }
        for (j = 0; j < MP_XFERS; j++) {
            mp_xfer_t* u = &s_w.xfers[j];
            mp_op_t* uop;

            if (!u->used || u->kind != MP_XFER_PICKUP || u->item != d->item || u->op == op->id) {
                continue;
            }
            uop = mp_op_find(u->op);
            if (uop != NULL && uop->state == MP_OPS_SENT) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

// a drop of an item a refused pickup couldn't take back: it was the host's, and never lands
static int mp_op_owes(mp_op_t* op) {
    int k;
    int i;

    for (k = 0; k < MP_XFERS; k++) {
        mp_xfer_t* d = &s_w.xfers[k];

        if (!d->used || d->kind != MP_XFER_DROP || d->op != op->id) {
            continue;
        }
        for (i = 0; i < s_w.nowed; i++) {
            if (s_w.owed[i] == d->item) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

static void mp_guest_push_op(mp_op_t* op) {
    u8 msg[6 + MP_OP_CELLS * 10];
    int n = 5;
    int nd = 0;
    int np = 0;
    int np_at;
    int i;
    int k;

    msg[0] = MP_M_OP;
    mp_put16(msg + 1, op->id);
    msg[3] = op->n;
    for (i = 0; i < op->n; i++) {
        mp_put16(msg + n, op->cell[i] | (((op->plain >> i) & 1) ? MP_CELL_PLAIN : 0));
        mp_put16(msg + n + 2, op->oldv[i]);
        mp_put16(msg + n + 4, op->newv[i]);
        n += 6;
    }
    // the pocket items it set down, so the host can square its unsaved pickups
    for (k = 0; k < MP_XFERS && nd < MP_OP_CELLS && n + 3 <= MP_MSG_MAX; k++) {
        mp_xfer_t* x = &s_w.xfers[k];

        if (x->used && x->kind == MP_XFER_DROP && x->op == op->id) {
            mp_put16(msg + n, x->item);
            n += 2;
            nd++;
        }
    }
    msg[4] = (u8)nd;
    // ...and what it took from the cottage, which the cells alone can't tell
    np_at = n++;
    for (k = 0; k < MP_XFERS && np < MP_OP_CELLS && n + 2 <= MP_MSG_MAX; k++) {
        mp_xfer_t* x = &s_w.xfers[k];

        if (x->used && x->kind == MP_XFER_PICKUP && x->cot && x->op == op->id) {
            mp_put16(msg + n, x->item);
            n += 2;
            np++;
        }
    }
    msg[np_at] = (u8)np;
    mp_queue_push(s_w.host_out, msg, n);
    op->state = MP_OPS_SENT;
    op->sent_ms = pc_mp_now_ms();
}

// held ops go out in order: after the passport without their drops and the answers they wait on. Only the cottage's
// wait out a doubtful line or a passport write that failed (its room shows them already), a few tries and a few ops
// at most; everything else is undone as it comes.
static void mp_guest_release_held(void) {
    int pp_ok = TRUE;
    int doubtful = mp_host_doubtful();
    int give_up;
    int stop = FALSE;
    int kept = 0;
    int k;

    if (s_w.pp_wait != 0) {
        if (!mp_passport_written(s_w.pp_wait, &pp_ok)) {
            return;
        }
        s_w.pp_wait = 0;
        if (pp_ok) {
            s_w.pp_done_gen = s_w.pp_wait_gen;
            // (a drop since that one was staged still wants one of its own)
            if (s_w.pp_done_gen == s_w.drop_gen) {
                s_w.pp_again = FALSE;
                s_w.pp_fails = 0;
            }
        } else {
            s_w.pp_fails++;
            pc_mp_log("[MP] world: passport not written, drops taken back");
        }
    }
    if (s_w.pp_again && s_w.pp_fails < MP_PP_TRIES) {
        u32 seq = mp_passport_write_async();

        if (seq != 0) {
            s_w.pp_wait = seq;
            s_w.pp_wait_gen = s_w.drop_gen;
            return;
        }
        s_w.pp_fails = MP_PP_TRIES;
    }
    give_up = s_w.pp_again || mp_cot_held() > MP_COT_HELD_MAX;
    for (k = 0; k < s_w.nheld; k++) {
        mp_op_t* op = mp_op_find(s_w.held_ops[k]);

        if (op == NULL || op->state != MP_OPS_HELD) {
            continue;
        }
        // held pickups ahead of it went out first, so only a sent one can still be deciding
        if (stop || mp_op_waits(op)) {
            stop = TRUE;
            s_w.held_ops[kept++] = op->id;
            continue;
        }
        if (mp_op_cottage(op) && !mp_op_owes(op) && (op->gen > s_w.pp_done_gen || doubtful)) {
            if (!give_up) {
                s_w.pp_again |= op->gen > s_w.pp_done_gen;
                s_w.held_ops[kept++] = op->id;
            } else {
                mp_op_reject_here(op);
            }
            continue;
        }
        // a refused pickup's item, a drop no passport on the card covers or a doubtful line: this op never happened
        if (mp_op_owes(op) || doubtful || op->gen > s_w.pp_done_gen) {
            mp_op_reject_here(op);
        } else {
            mp_guest_push_op(op);
        }
    }
    if (give_up) {
        s_w.pp_again = FALSE;
        s_w.pp_fails = 0;
    }
    if (kept != s_w.nheld) {
        s_w.nheld = kept;
        mp_refresh_field();
    }
}

// guest heading home: ops the host never got are undone, their drops back in the pockets
void mp_world_recall_held(int from_pp) {
    int ok;
    int k;

    // (a passport already on the card covers its drops)
    if (s_w.pp_wait != 0 && mp_passport_written(s_w.pp_wait, &ok)) {
        if (ok) {
            s_w.pp_done_gen = s_w.pp_wait_gen;
        }
        s_w.pp_wait = 0;
    }
    s_w.recall_pp = from_pp;
    s_w.recalling = TRUE;
    s_w.home_pp = from_pp; // (no passport moves past that one now)
    for (k = s_w.nheld - 1; k >= 0; k--) {
        mp_op_t* op = mp_op_find(s_w.held_ops[k]);

        if (op != NULL && op->state == MP_OPS_HELD) {
            mp_op_reject_here(op);
        }
    }
    s_w.recall_pp = FALSE;
    s_w.recalling = FALSE;
    s_w.nheld = 0;
    s_w.pp_wait = 0;
    s_w.pp_again = FALSE;
    s_w.pp_fails = 0;
}

static void mp_guest_send_op(mp_op_t* op) {
    int tr = mp_op_note_transfers(op);

    mp_tk_op(op);

    // an item the cottage would gain or lose from nowhere: this room was behind the town, and the op never happens
    if (tr & MP_TR_UNSQUARE) {
        static u32 s_said_ms;

        if (pc_mp_now_ms() - s_said_ms >= 1000) {
            s_said_ms = pc_mp_now_ms();
            pc_mp_log("[MP] world: cottage op %u doesn't add up, undone", op->id);
        }
        mp_op_reject_here(op);
        return;
    }
    // the item leaves the passport before the host may hand it to anyone else; the write runs on
    // the writer thread, and every op after it waits its turn
    if (tr & MP_TR_DROP) {
        u32 seq = mp_passport_write_async();

        if (seq != 0) {
            op->gen = ++s_w.drop_gen;
            s_w.pp_wait = seq;
            s_w.pp_wait_gen = s_w.drop_gen;
        } else {
            mp_op_reject_here(op);
            return;
        }
    }
    if (s_w.nheld >= MP_OPS) {
        mp_op_reject_here(op);
        return;
    }
    op->state = MP_OPS_HELD;
    s_w.held_ops[s_w.nheld++] = op->id;
    mp_guest_release_held();
}

// guest: cells the player was in the middle of are let go once it's done with them (an op of its own carries
// whatever it changed there): the host's value lands
static void mp_guest_unpins(void) {
    int kept = 0;
    int i;

    for (i = 0; i < s_w.npins; i++) {
        int c = s_w.pins[i];

        if (s_w.pend[c] != MP_PIN_OP) {
            continue; // (an op of its own has it now)
        }
        if (MP_BIT_GET(s_w.lock, c) || s_w.cur[c] != s_w.exp[c]) {
            s_w.pins[kept++] = (u16)c;
            continue;
        }
        s_w.pend[c] = 0;
        mp_set_exp(c);
        if (MP_BIT_GET(s_w.held, c)) {
            MP_BIT_SET(s_w.force, c);
        } else if (mp_raw_get(c) != s_w.exp[c]) {
            mp_raw_set(c, s_w.exp[c]);
        }
    }
    s_w.npins = kept;
}

static void mp_guest_diff(GAME_PLAY* play, int in_menu) {
    mp_op_t* op = NULL;
    int region;
    int pgx = 0;
    int pgz = 0;
    int page;

    mp_capture(play);
    mp_player_reach(play, &region, &pgx, &pgz);

    for (page = 0; page < MP_CELL_PAGES; page++) {
        int first = page * MP_CELL_PAGE;
        int count = (first + MP_CELL_PAGE <= MP_CELLS) ? MP_CELL_PAGE : MP_CELLS - first;
        int c;

        if (memcmp(&s_w.cur[first], &s_w.exp[first], (size_t)count * 2) == 0) {
            continue;
        }
        for (c = first; c < first + count; c++) {
            u16 now_v = s_w.cur[c];
            u16 want = s_w.exp[c];

            if (MP_BIT_GET(s_w.force, c) && !MP_BIT_GET(s_w.held, c) && !MP_BIT_GET(s_w.lock, c) &&
                s_w.pend[c] != MP_PIN_OP) {
                // a host value that arrived while an actor (or the player) held the cell
                mp_raw_set(c, s_w.view[c]);
                MP_BIT_CLR(s_w.force, c);
                continue;
            }
            if (now_v == want || MP_BIT_GET(s_w.held, c) || MP_BIT_GET(s_w.hold, c)) {
                continue;
            }
            if (now_v == RSV_NO && want == EMPTY_NO) {
                continue; // an item in mid-air; wait for where it lands
            }
            if (in_menu && c >= MP_COT_BASE) {
                continue; // the cottage's menus trade with the pockets, which are read once the menu is down
            }
            // (what the player was in the middle of goes out even if it's since walked off)
            if (!mp_in_reach(c, region, pgx, pgz) && s_w.pend[c] != MP_PIN_OP) {
                // the local game doesn't own the town; undo whatever changed it
                mp_raw_set(c, want);
                continue;
            }
            if (op == NULL || op->n == (op->cell[0] >= MP_COT_BASE ? MP_OP_CELLS : MP_OP_TOWN)) {
                if (op != NULL) {
                    mp_guest_send_op(op);
                }
                op = mp_op_new();
                if (op == NULL) {
                    continue; // too much in flight (a host this slow is doubted first): the change waits its turn
                }
            }
            op->cell[op->n] = (u16)c;
            op->oldv[op->n] = want;
            op->newv[op->n] = now_v;
            op->n++;
            s_w.pred[c] = now_v;
            s_w.pend[c] = op->id;
            mp_set_exp(c);
        }
    }
    if (op != NULL && op->n > 0) {
        mp_guest_send_op(op);
    }
    mp_guest_unpins();
    mp_refresh_field();
}

// a host value for a cell: it lands unless our own op on it is still deciding (or the player is in the middle of it)
static void mp_guest_take_value(int cell, u16 v) {
    s_w.view[cell] = v;
    if (s_w.pend[cell] == 0) {
        if (MP_BIT_GET(s_w.held, cell) || MP_BIT_GET(s_w.lock, cell)) {
            MP_BIT_SET(s_w.force, cell);
        } else if (mp_raw_get(cell) != v) {
            mp_raw_set(cell, v);
        }
    }
    mp_set_exp(cell);
}

static void mp_guest_on_ack(const u8* p, int len) {
    u16 id;
    int result;
    int n;
    mp_op_t* op;
    int i;

    if (len < 5) {
        return;
    }
    id = (u16)mp_get16(p + 1);
    result = p[3];
    n = p[4];
    op = mp_op_find(id);
    if (op == NULL || 5 + n * 4 > len) {
        return;
    }
    // our op's cells settle first, so the host's values below land on them
    for (i = 0; i < op->n; i++) {
        int c = op->cell[i];

        if (result != MP_ACK_REJECT) {
            s_w.view[c] = op->newv[i]; // moved cells are corrected by the values below
        }
        if (s_w.pend[c] == id) {
            s_w.pend[c] = 0;
            mp_repin(c, op->newv[i]);
        }
        mp_set_exp(c);
    }
    for (i = 0; i < n; i++) {
        int c = (int)mp_get16(p + 5 + i * 4);

        if (c < MP_CELLS) {
            mp_guest_take_value(c, (u16)mp_get16(p + 7 + i * 4));
        }
    }
    if (result == MP_ACK_REJECT) {
        op->state = MP_OPS_REJECTED;
        s_cot.resync |= (u8)mp_op_cottage(op);
        mp_op_compensate(op);
    } else {
        op->state = MP_OPS_ACCEPTED;
    }
    mp_guest_release_held(); // drops that waited on this answer
    mp_refresh_field();
}

static void mp_guest_on_commit(const u8* p, int len) {
    u16 id;
    int k;

    if (len < 3) {
        return;
    }
    id = (u16)mp_get16(p + 1);
    for (k = 0; k < MP_XFERS; k++) {
        mp_xfer_t* x = &s_w.xfers[k];
        mp_op_t* op = mp_op_find(x->op);

        if (x->used && (s16)(x->op - id) <= 0 && (op == NULL || op->state != MP_OPS_REJECTED)) {
            x->committed = TRUE;
        }
    }
    if (!s_w.seen_any || (s16)(id - s_w.seen_op) > 0) {
        s_w.seen_op = id;
        s_w.seen_any = TRUE;
    }
    // what the host saved is ours now: into the passport before anything can go wrong
    mp_passport_write_async();
}

// guest boarding home: the newest commit heard, so the host knows which pickups came along (heading home as the
// passport last stood: the newest that passport has)
int mp_world_commit_mark(unsigned short* op) {
    if (s_w.home_pp) {
        u32 mark = mp_passport_mark();

        *op = (unsigned short)mark;
        return s_w.live && (mark >> 16) != 0;
    }
    *op = s_w.seen_op;
    return s_w.live && s_w.seen_any;
}

unsigned int mp_world_pp_mark(void) {
    return (s_w.live && s_w.seen_any ? 0x10000u : 0) | s_w.seen_op;
}

// guest: the host knows while the passport stands still, so it keeps this player's pickups to give back and saves
// soon. Once it's over, the host hears which of its saves the next passport has before that one may be written (so
// it never gives back what a card already holds), then hears the passport moves again once that one is on the card.
static void mp_hold_send(int on, u32 mark) {
    u8 msg[6];

    msg[0] = MP_M_SOFT;
    msg[1] = MP_SOFT_HOLD;
    msg[2] = (u8)on;
    msg[3] = (u8)((mark >> 16) != 0);
    msg[4] = (u8)mark;
    msg[5] = (u8)(mark >> 8);
    mp_queue_push(s_w.host_out, msg, 6);
}

static void mp_hold_watch(void) {
    u32 mark;
    int ok;

    if (s_w.hold_closing) {
        if (s_w.host_out != NULL && s_w.host_out->head == s_w.host_out->tail && mp_lobby_host_conn() >= 0 &&
            mp_link_rel_idle(mp_lobby_host_conn())) {
            s_w.hold_closing = FALSE;
            s_w.hold_told = FALSE;
            s_w.hold_open = FALSE;
        }
        return;
    }
    if (mp_world_passport_hold()) {
        int ok2;

        // (the newest staged, unless it's been tried and didn't make it: then the card's)
        mark = mp_passport_written(mp_passport_staged_seq(), &ok2) ? mp_passport_mark() : mp_passport_mark_staged();
        s_w.hold_off_seq = 0;
        s_w.hold_open = FALSE;
        if (!s_w.hold_told || mark != s_w.hold_mark) {
            mp_hold_send(TRUE, mark);
            s_w.hold_told = TRUE;
            s_w.hold_mark = mark;
        }
        return;
    }
    if (!s_w.hold_told) {
        return;
    }
    mark = mp_world_pp_mark();
    if (mark != s_w.hold_mark) {
        mp_hold_send(TRUE, mark);
        s_w.hold_mark = mark;
        s_w.hold_open = FALSE;
        s_w.hold_off_seq = 0;
        return;
    }
    // (heard: everything queued is out, and the line has it)
    if (!s_w.hold_open) {
        if (s_w.host_out == NULL || s_w.host_out->head != s_w.host_out->tail || mp_lobby_host_conn() < 0 ||
            !mp_link_rel_idle(mp_lobby_host_conn())) {
            return;
        }
        s_w.hold_open = TRUE;
    }
    if (s_w.hold_off_seq == 0) {
        if ((s32)(pc_mp_now_ms() - s_w.hold_retry_ms) < 0 || (s_w.hold_off_seq = mp_passport_write_async()) == 0) {
            return;
        }
    }
    if (!mp_passport_written(s_w.hold_off_seq, &ok)) {
        return;
    }
    if (!ok) {
        s_w.hold_off_seq = 0;
        s_w.hold_retry_ms = pc_mp_now_ms() + 1000;
        return;
    }
    mp_hold_send(FALSE, 0);
    s_w.hold_closing = TRUE; // (no newer save gets on the card before the host hears this)
}

int mp_world_hold_told(void) {
    return s_w.live && !mp_is_host() && s_w.hold_told;
}

// guest: what's queued for the host into the line, a few moments for a full line to take it; TRUE once the host has
// all of it
static int mp_world_flush_heard(void) {
    int conn;
    int i;

    for (i = 0; i < 100 && s_w.live && mp_world_line_ok(); i++) {
        mp_world_flush_out();
        conn = mp_lobby_host_conn();
        if (s_w.host_out != NULL && s_w.host_out->head == s_w.host_out->tail && mp_link_rel_idle(conn)) {
            return TRUE;
        }
        mp_lobby_pump();
    }
    mp_world_flush_out();
    return FALSE;
}

void mp_world_flush_all(void) {
    if (s_w.live && !mp_is_host()) {
        mp_world_flush_heard();
    }
}

// guest: an op still waits on the host's answer
static int mp_world_ops_out(void) {
    int i;

    for (i = 0; i < MP_OPS; i++) {
        if (s_w.ops[i].state == MP_OPS_SENT) {
            return TRUE;
        }
    }
    return FALSE;
}

// the app closing mid-visit (FALSE: no town of the host's here). Ops the host never got are undone, talks' results the
// line takes go and the rest give back what they handed over, then the passport has the traveller as the host will
// know them; with the line up, the host last hears which of its saves the card has.
int mp_world_exit(void) {
    int line;
    int stand;
    int i;

    if (!s_w.live || mp_is_host()) {
        return FALSE;
    }
    // (a few moments for the answers to ops on their way)
    for (i = 0; i < 50 && s_w.live && mp_world_line_ok() && mp_world_ops_out(); i++) {
        mp_lobby_pump();
        mp_world_drain();
    }
    if (!s_w.live) {
        return FALSE;
    }
    line = mp_world_line_ok();
    mp_passport_settle();
    // (a pickup used up and unsaved, or a dead line the host may have heard another mark on: the card stays)
    stand = mp_world_passport_hold() || (!line && s_w.hold_told);
    // (the host hears the next passport's mark first, and has it, before one goes past what it heard last)
    if (!stand && s_w.hold_told) {
        mp_hold_send(TRUE, mp_world_pp_mark());
        stand = !mp_world_flush_heard();
    }
    mp_world_recall_held(stand);
    s_w.exiting = TRUE;
    if (line && !stand) {
        mp_passport_write(); // (the talks over are covered: their results can go)
    }
    if (line) {
        mp_npc_drain_all();
    }
    if (!stand) {
        mp_npc_undo(Now_Private, 0);
        mp_passport_write();
    } else {
        // (the card as it stands, with what talks whose results never went gave back; its host mark kept)
        static Private_c s_card;

        if (mp_passport_last_priv(&s_card) && mp_npc_undo(&s_card, mp_passport_good_seq())) {
            mp_passport_rewrite(&s_card);
        }
    }
    if (line) {
        mp_hold_send(TRUE, mp_passport_mark());
    }
    mp_world_flush_heard();
    return TRUE;
}

// guest: no passport may be staged now: one used-up pickup waits on the host's save, the traveller goes home as the
// card has them, or the host hasn't yet heard which of its saves the next one will have (unless the app is closing:
// the host hears the card's own last)
int mp_world_pp_frozen(void) {
    if (!s_w.live || mp_is_host()) {
        return FALSE;
    }
    if (s_w.home_pp || mp_world_passport_hold()) {
        return TRUE;
    }
    return !s_w.exiting && s_w.hold_told && (!s_w.hold_open || mp_world_pp_mark() != s_w.hold_mark);
}

// host: ops

static mp_zone_t* mp_zone_hit(mp_guest_w_t* g, int cell) {
    u32 now = pc_mp_now_ms();
    int gx;
    int gz;
    int r;
    int i;

    if (!mp_cell_pos(cell, &gx, &gz, &r)) {
        return NULL;
    }
    for (i = 0; i < MP_ZONES; i++) {
        mp_zone_t* z = &g->zones[i];

        if (z->until_ms != 0 && (s32)(z->until_ms - now) > 0 && z->region == r && gx - z->gx <= MP_ZONE_UNITS &&
            z->gx - gx <= MP_ZONE_UNITS && gz - z->gz <= MP_ZONE_UNITS && z->gz - gz <= MP_ZONE_UNITS) {
            return z;
        }
    }
    return NULL;
}

static void mp_zone_add(mp_guest_w_t* g, int cell) {
    int gx;
    int gz;
    int r;

    if (mp_cell_pos(cell, &gx, &gz, &r)) {
        memmove(&g->zones[1], &g->zones[0], sizeof(g->zones[0]) * (MP_ZONES - 1));
        g->zones[0].region = (u8)r;
        g->zones[0].gx = (s16)gx;
        g->zones[0].gz = (s16)gz;
        g->zones[0].until_ms = pc_mp_now_ms() + MP_ZONE_MS;
    }
}

// host: an item for the lost & found, in its first free slot; a full one drops its oldest
static int mp_host_keep(mActor_name_t item, int full_ok);

void mp_world_keep(unsigned short item) {
    if (mp_is_host() && s_w.live) {
        mp_host_keep((mActor_name_t)item, TRUE);
        s_w.save_asked = TRUE;
    }
}

static int mp_host_keep(mActor_name_t item, int full_ok) {
    int i;

    if (!ITEM_IS_ITEM1(item) && !ITEM_IS_FTR(item)) {
        return TRUE;
    }
    for (i = 0; i < MP_POL_WORDS; i++) {
        int c = s_rg[MP_RG_POLICE].base + i;

        if (mp_raw_get(c) == EMPTY_NO && s_w.view[c] == EMPTY_NO) {
            mp_raw_set(c, item);
            return TRUE;
        }
    }
    if (!full_ok) {
        return FALSE;
    }
    mPB_keep_item(item);
    return TRUE;
}

// a free neighbouring cell for a drop that found its spot taken
static int mp_host_find_spot(int cell, const u16* taken, int ntaken) {
    static const s8 around[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 } };
    int gx;
    int gz;
    int r;
    int i;

    if (!mp_cell_pos(cell, &gx, &gz, &r)) {
        return -1;
    }
    for (i = 0; i < 8; i++) {
        int c = mp_cell_at(r, gx + around[i][0], gz + around[i][1]);
        xyz_t pos;
        int k;
        int used = FALSE;

        if (c < 0 || s_w.view[c] != EMPTY_NO || MP_BIT_GET(s_w.held, c) || MP_BIT_GET(s_w.lock, c) ||
            s_w.cur[c] == RSV_NO) {
            continue;
        }
        for (k = 0; k < ntaken; k++) {
            used |= taken[k] == c;
        }
        if (used) {
            continue;
        }
        // only where the host can see the ground is it safe to set something down
        pos.x = (f32)(gx + around[i][0]) * mFI_UT_WORLDSIZE_X_F + mFI_UT_WORLDSIZE_HALF_X_F;
        pos.y = 0.0f;
        pos.z = (f32)(gz + around[i][1]) * mFI_UT_WORLDSIZE_Z_F + mFI_UT_WORLDSIZE_HALF_Z_F;
        if (r == MP_RG_FG) {
            pos.x += mFI_BK_WORLDSIZE_X_F;
            pos.z += mFI_BK_WORLDSIZE_Z_F;
        } else {
            // (the island's two acres lie out at sea in the town's field)
            pos.x += mISL_BLOCK_X0 * mFI_BK_WORLDSIZE_X_F;
            pos.z += mISL_BLOCK_Z * mFI_BK_WORLDSIZE_Z_F;
        }
        if (mFI_GetUnitFG(pos) == mp_word(r, 0) + (c - s_rg[r].base) && mCoBG_CheckPlant(pos)) {
            return c;
        }
    }
    return -1;
}

// an op's town cell whose item left it for a pocket: not one only moved to another of its cells (the lost & found
// closing up behind what was claimed, a dug-up thing set down beside its hole)
static int mp_op_town_gone(const u8* p, int n, int at) {
    u16 took = mp_town_take((u16)mp_get16(p + 7 + at * 6));
    int before = 0;
    int moved = 0;
    int i;

    if ((int)(mp_get16(p + 5 + at * 6) & ~MP_CELL_PLAIN) >= MP_COT_BASE ||
        s_rg[mp_rg_of((int)(mp_get16(p + 5 + at * 6) & ~MP_CELL_PLAIN))].bits || took == EMPTY_NO ||
        mp_town_take((u16)mp_get16(p + 9 + at * 6)) == took) {
        return FALSE;
    }
    for (i = 0; i < n; i++) {
        int c = (int)(mp_get16(p + 5 + i * 6) & ~MP_CELL_PLAIN);
        u16 o = mp_town_take((u16)mp_get16(p + 7 + i * 6));
        u16 v = mp_town_take((u16)mp_get16(p + 9 + i * 6));

        if (c >= MP_COT_BASE || s_rg[mp_rg_of(c)].bits || o == v) {
            continue;
        }
        before += i < at && o == took;
        moved += v == took;
    }
    return before >= moved;
}

static void mp_pick_add(mp_guest_w_t* g, u16 op, int cell, mActor_name_t item, mActor_name_t pocket, u32 price) {
    if (g->npicks == MP_PICKS) {
        memmove(&g->picks[0], &g->picks[1], sizeof(g->picks[0]) * (MP_PICKS - 1));
        g->npicks--;
        pc_mp_log("[MP] world: pickup list full, oldest forgotten");
    }
    g->picks[g->npicks].op = op;
    g->picks[g->npicks].cell = (u16)cell;
    g->picks[g->npicks].item = item;
    g->picks[g->npicks].pocket = pocket;
    g->picks[g->npicks].price = price;
    g->npicks++;
}

// host: a gyroid's slot sold (or a sale undone): its house's gyroid holds the bells; what an undone sale finds
// already handed over comes back out of the owner's wallet
static void mp_hnw_bells(int cell, s32 bells) {
    int k = cell - MP_HNW_BASE;
    int pl;
    u32* held;
    u32 owed;

    if (k < 0 || k >= MP_HNW_WORDS || bells == 0) {
        return;
    }
    held = &Save_Get(homes[k / HANIWA_ITEM_HOLD_NUM]).haniwa.bells;
    if (bells > 0 || (u32)-bells <= *held) {
        *held += (u32)bells;
        return;
    }
    owed = (u32)-bells - *held;
    *held = 0;
    pl = mHS_get_pl_no_detail(k / HANIWA_ITEM_HOLD_NUM);
    if (pl >= 0) {
        u32* wallet = &Save_Get(private_data[pl]).inventory.wallet;

        *wallet = owed < *wallet ? *wallet - owed : 0;
    }
}

static void mp_pick_cancel(mp_guest_w_t* g, mActor_name_t item) {
    int i;

    for (i = g->npicks - 1; i >= 0; i--) {
        if (mp_take_matches(g->picks[i].pocket, item)) {
            memmove(&g->picks[i], &g->picks[i + 1], sizeof(g->picks[0]) * (g->npicks - 1 - i));
            g->npicks--;
            return;
        }
    }
}

// host: a visitor's change the host's switches don't allow on the town's ground (a pocket item set down, an item
// taken, dug up or buried, a hole dug in bare ground, a felled tree)
static int mp_host_ruled_out(int c, u16 oldv, u16 newv, int nd) {
    int r = mp_rg_of(c);

    if (r == MP_RG_DEP || r == MP_RG_IDEP) {
        return !mp_rule(MP_RULE_ITEMS);
    }
    if (r != MP_RG_FG && r != MP_RG_IFG) {
        return FALSE;
    }
    if (!mp_rule(MP_RULE_ITEMS) && (nd > 0 || (mp_pocketable(oldv) && !mp_is_item(newv)))) {
        return TRUE;
    }
    if (!mp_rule(MP_RULE_DIG) && oldv == EMPTY_NO && ITEM_IS_HOLE(newv)) {
        return TRUE;
    }
    return !mp_rule(MP_RULE_AXE) && IS_ITEM_TREE_STUMP(newv) && !IS_ITEM_TREE_STUMP(oldv);
}

// host: a visitor's change to the cottage that someone else's hands or feet are in the way of
static int mp_cot_op_blocked(int slot, int c, u16 oldv, u16 newv) {
    int k = c - MP_COT_BASE;
    int ut = k % UT_TOTAL_NUM;
    int s;

    if (k < 0 || k >= MP_COT_WORDS) {
        return FALSE;
    }
    if (ITEM_IS_FTR(newv) && mRmTp_FtrItemNo2FtrIdx(newv) >= FTR_NUM) {
        return TRUE;
    }
    for (s = 1; s < MP_MAX_PEERS; s++) {
        if (s != slot && MP_BIT_GET(s_cot.units[s], ut)) {
            return TRUE;
        }
    }
    // (furniture set down on a player)
    return k < UT_TOTAL_NUM && oldv == EMPTY_NO && (ITEM_IS_FTR(newv) || newv == RSV_FE1F) &&
           mp_cot_unit_stood(ut, slot);
}

// host: furniture in the cottage, as many as a room of it can hold
static int mp_cot_pieces(void) {
    int n = 0;
    int k;

    for (k = 0; k < 2 * UT_TOTAL_NUM; k++) {
        n += ITEM_IS_FTR(s_w.view[MP_COT_BASE + k]) != 0;
    }
    return n;
}

static void mp_host_on_op(int conn, const u8* p, int len) {
    mp_guest_w_t* g = mp_gw_of_conn(conn);
    int slot = mp_lobby_guest_slot(conn) + 1;
    u8 ack[5 + MP_OP_CELLS * 2 * 4];
    u8 delta[2 + MP_OP_CELLS * 2 * 4];
    u16 moved_to[MP_OP_CELLS];
    u16 id;
    int n;
    int nd;
    int np;
    int np_at;
    int transfer = FALSE;
    int ok = TRUE;
    int drop_only = TRUE;
    int zone = FALSE;
    int ruled = FALSE;
    int pieces = 0;
    int gone = 0;
    int na = 0;
    int i;

    if (g == NULL || len < 5) {
        return;
    }
    // (the cottage is its visitors' to rearrange only if the host lets them)
    for (i = 0; i < p[3] && 5 + i * 6 + 2 <= len; i++) {
        if ((int)(mp_get16(p + 5 + i * 6) & ~MP_CELL_PLAIN) >= MP_COT_BASE && !mp_rule(MP_RULE_COTTAGE)) {
            ok = FALSE;
        }
    }
    id = (u16)mp_get16(p + 1);
    n = p[3];
    if (n > MP_OP_CELLS || 5 + n * 6 > len) {
        return;
    }
    if (!g->rx_any || (s16)(id - g->rx_op) > 0) {
        g->rx_op = id;
        g->rx_any = TRUE;
    }
    nd = p[4];
    if (nd > MP_OP_CELLS || 5 + n * 6 + nd * 2 > len) {
        nd = 0;
    }
    np_at = 5 + n * 6 + nd * 2;
    np = np_at < len ? p[np_at] : 0;
    if (np > MP_OP_CELLS || np_at + 1 + np * 2 > len) {
        np = 0;
    }
    for (i = 0; i < n; i++) {
        u32 raw = mp_get16(p + 5 + i * 6);
        int c = (int)(raw & ~MP_CELL_PLAIN);
        u16 oldv = (u16)mp_get16(p + 7 + i * 6);
        u16 newv = (u16)mp_get16(p + 9 + i * 6);

        if (c >= MP_CELLS) {
            return;
        }
        moved_to[i] = (u16)c;
        // taken: changed already, held by an actor, in the host player's hands, reserved by a falling item, about to
        // be picked up here, or (the cottage) someone else's
        if (s_w.view[c] != oldv || MP_BIT_GET(s_w.held, c) || MP_BIT_GET(s_w.lock, c) || s_w.cur[c] == RSV_NO ||
            c == s_w.host_pickup_cell || mp_cot_op_blocked(slot, c, oldv, newv)) {
            ok = FALSE;
        }
        if (c >= MP_COT_BASE && c < MP_COT_BASE + 2 * UT_TOTAL_NUM) {
            pieces += (ITEM_IS_FTR(newv) != 0) - (ITEM_IS_FTR(oldv) != 0);
        }
        if (c >= MP_COT_BASE) {
            transfer |= !(raw & MP_CELL_PLAIN);
        } else {
            transfer |= mp_is_item(oldv) != mp_is_item(newv);
            if (mp_host_ruled_out(c, oldv, newv, nd)) {
                ruled = TRUE;
            }
        }
        // (a signboard going up stays where its game puts it up: a taken spot refuses it back to the pocket)
        if (oldv != EMPTY_NO || !mp_is_item(newv) || mp_rg_of(c) > MP_RG_IFG || newv == RSV_SIGNBOARD) {
            drop_only = FALSE;
        } else if (mp_zone_hit(g, c) != NULL) {
            zone = TRUE; // lands from something already refused (a shaken tree, a dig)
        }
    }

    if (pieces > 0 && mp_cot_pieces() + pieces > MP_COT_PIECES) {
        ok = FALSE;
    }
    for (i = 0; i < n; i++) {
        gone += mp_op_town_gone(p, n, i);
    }
    if (g->npicks + gone + np > MP_PICKS) {
        ok = FALSE;
    }
    if (ruled) {
        ok = FALSE;
    } else if (!ok && drop_only && !zone && n <= MP_OP_TOWN) {
        // a drop onto a taken spot rolls to the nearest free one instead
        ok = TRUE;
        for (i = 0; i < n; i++) {
            int c = moved_to[i];

            if (s_w.view[c] != EMPTY_NO || MP_BIT_GET(s_w.held, c) || MP_BIT_GET(s_w.lock, c) ||
                s_w.cur[c] == RSV_NO || c == s_w.host_pickup_cell) {
                int spot = mp_host_find_spot(c, moved_to, n);

                if (spot < 0) {
                    ok = FALSE;
                    break;
                }
                moved_to[i] = (u16)spot;
            }
        }
    }
    if (zone) {
        ok = FALSE;
    }

    ack[0] = MP_M_ACK;
    mp_put16(ack + 1, id);
    delta[0] = MP_M_DELTA;
    if (ok) {
        int moved = FALSE;
        int nland = 0;

        for (i = 0; i < n; i++) {
            int c = (int)(mp_get16(p + 5 + i * 6) & ~MP_CELL_PLAIN);
            u16 newv = (u16)mp_get16(p + 9 + i * 6);
            int to = moved_to[i];

            if (mp_is_item((u16)mp_get16(p + 7 + i * 6)) && !mp_is_item(newv)) {
                mp_hnw_bells(c, (s32)mp_hnw_price(c));
            }
            mp_raw_set(to, newv);
            s_w.view[to] = newv;
            mp_put16(delta + 2 + nland * 4, (u32)to);
            mp_put16(delta + 4 + nland * 4, newv);
            nland++;
            if (to != c) {
                // the guest learns both the refused spot and where it landed
                moved = TRUE;
                mp_put16(ack + 5 + na * 4, (u32)c);
                mp_put16(ack + 7 + na * 4, s_w.view[c]);
                na++;
                mp_put16(ack + 5 + na * 4, (u32)to);
                mp_put16(ack + 7 + na * 4, newv);
                na++;
            }
        }
        ack[3] = moved ? MP_ACK_MOVED : MP_ACK_ACCEPT;
        delta[1] = (u8)nland;
        mp_send_all(delta, 2 + nland * 4, slot);
        g->last_op = id;
        // an item crossed between a pocket and the town: it's final once a save has it
        if ((transfer || np > 0) && !g->left) {
            g->xfer_op = id;
            g->xfer_any = TRUE;
            for (i = 0; i < n; i++) {
                int c = (int)(mp_get16(p + 5 + i * 6) & ~MP_CELL_PLAIN);
                u16 oldv = (u16)mp_get16(p + 7 + i * 6);

                if (mp_op_town_gone(p, n, i)) {
                    mp_pick_add(g, id, c, oldv, mp_town_take(oldv), mp_hnw_price(c));
                }
            }
            // the cottage's, as its player's game matched them to pockets (each one a cell of the op held)
            {
                u8 taken[MP_OP_CELLS];

                memset(taken, 0, sizeof(taken));
                for (i = 0; i < np; i++) {
                    mActor_name_t item = (mActor_name_t)mp_get16(p + np_at + 1 + i * 2);
                    int k;

                    for (k = 0; k < n && mp_pocketable(item); k++) {
                        int c = (int)(mp_get16(p + 5 + k * 6) & ~MP_CELL_PLAIN);

                        if (c >= MP_COT_BASE && !taken[k] &&
                            mp_cell_pocket(c, (u16)mp_get16(p + 7 + k * 6), FALSE) == item) {
                            taken[k] = TRUE;
                            mp_pick_add(g, id, MP_COT_BASE, item, item, 0);
                            break;
                        }
                    }
                }
            }
            // the pocket items it set down: an unsaved pickup of one is back in the town
            for (i = 0; i < nd; i++) {
                mp_pick_cancel(g, (mActor_name_t)mp_get16(p + 5 + n * 6 + i * 2));
            }
        }
    } else {
        ack[3] = MP_ACK_REJECT;
        for (i = 0; i < n; i++) {
            int c = (int)(mp_get16(p + 5 + i * 6) & ~MP_CELL_PLAIN);

            mp_put16(ack + 5 + na * 4, (u32)c);
            mp_put16(ack + 7 + na * 4, s_w.view[c]);
            na++;
            if (c < MP_COT_BASE) {
                mp_zone_add(g, c);
            }
        }
        // the pocket items it set down go back to it, or come back as lost & found claims
        for (i = 0; i < nd; i++) {
            memmove(&g->refused[1], &g->refused[0], sizeof(g->refused[0]) * (MP_REFUSED - 1));
            g->refused[0] = (mActor_name_t)mp_get16(p + 5 + n * 6 + i * 2);
        }
    }
    ack[4] = (u8)na;
    mp_send_guest(slot, ack, 5 + na * 4);
    mp_refresh_field();
}

// the host's own changes, and anything its game wrote, go out as deltas
static void mp_host_diff(GAME_PLAY* play) {
    u8 msg[2 + 110 * 4];
    int n = 0;
    int page;

    mp_capture(play);
    for (page = 0; page < MP_CELL_PAGES; page++) {
        int first = page * MP_CELL_PAGE;
        int count = (first + MP_CELL_PAGE <= MP_CELLS) ? MP_CELL_PAGE : MP_CELLS - first;
        int c;

        if (memcmp(&s_w.cur[first], &s_w.view[first], (size_t)count * 2) == 0) {
            continue;
        }
        for (c = first; c < first + count; c++) {
            if (s_w.cur[c] == s_w.view[c] || (s_w.cur[c] == RSV_NO && s_w.view[c] == EMPTY_NO) ||
                MP_BIT_GET(s_w.hold, c)) {
                continue;
            }
            s_w.view[c] = s_w.cur[c];
            mp_put16(msg + 2 + n * 4, (u32)c);
            mp_put16(msg + 4 + n * 4, s_w.cur[c]);
            if (++n == 110) {
                msg[0] = MP_M_DELTA;
                msg[1] = (u8)n;
                mp_send_all(msg, 2 + n * 4, -1);
                n = 0;
            }
        }
    }
    if (n > 0) {
        msg[0] = MP_M_DELTA;
        msg[1] = (u8)n;
        mp_send_all(msg, 2 + n * 4, -1);
    }
}

// the rest of the save: a slice per frame compared against what guests were last sent
// guest: the house floor its room is up in keeps the host's bytes for its furniture here, for the room to take on
// as it can (they'd land under its pieces, and put what furniture holds on top of it); the save gets them once the
// room is gone
#define MP_FLOOR_BYTES     (mCoBG_LAYER_NUM * sizeof(mHm_lyr_c))
#define MP_FLOOR_SETTLE_MS 500
static u32 s_floor_held;
static u32 s_floor_heard_ms;
static u16 s_floor_truth[mCoBG_LAYER_NUM * UT_TOTAL_NUM];

static void mp_guest_floor_track(void) {
    u32 now = (s_w.live && !mp_is_host()) ? aMR_mp_house_floor_off() : 0;

    if (now == s_floor_held) {
        return;
    }
    if (s_floor_held != 0 && s_w.gen_shadow != NULL && mp_gen_contains(s_floor_held, MP_FLOOR_BYTES)) {
        memcpy(mp_save_base() + s_floor_held, s_w.gen_shadow + mp_gen_lin_of(s_floor_held), MP_FLOOR_BYTES);
    }
    s_floor_held = now;
    s_floor_heard_ms = pc_mp_now_ms();
}

static void mp_guest_floor_release(void) {
    s_floor_held = 0;
}

const unsigned short* mp_house_truth(void) {
    int layer;

    if (s_floor_held == 0 || pc_mp_now_ms() - s_floor_heard_ms < MP_FLOOR_SETTLE_MS) {
        return NULL;
    }
    for (layer = 0; layer < mCoBG_LAYER_NUM; layer++) {
        const u8* src = mp_world_heard(s_floor_held + layer * sizeof(mHm_lyr_c) + offsetof(mHm_lyr_c, items),
                                       UT_TOTAL_NUM * sizeof(u16));

        if (src == NULL) {
            return NULL;
        }
        memcpy(&s_floor_truth[layer * UT_TOTAL_NUM], src, UT_TOTAL_NUM * sizeof(u16));
    }
    return s_floor_truth;
}

// host: the house floor it's in, as leaving it would save it (what its furniture holds, its lamps): the floor streams
// so, and a visitor coming in finds it as it is
static u32 s_floor_off;
static u8 s_floor_live[sizeof(mHm_flr_c)];

static void mp_floor_cell(unsigned int off, unsigned short name, void* arg) {
    (void)arg;
    if (off >= s_floor_off && off + sizeof(name) <= s_floor_off + sizeof(s_floor_live)) {
        memcpy(s_floor_live + (off - s_floor_off), &name, sizeof(name));
    }
}

static void mp_floor_switch(unsigned int off, unsigned long long bits, void* arg) {
    (void)arg;
    if (off >= s_floor_off && off + sizeof(bits) <= s_floor_off + sizeof(s_floor_live)) {
        memcpy(s_floor_live + (off - s_floor_off), &bits, sizeof(bits));
    }
}

static void mp_host_floor_take(void) {
    s_floor_off = aMR_mp_house_floor_off();
    if (s_floor_off == 0) {
        return;
    }
    memcpy(s_floor_live, mp_save_base() + s_floor_off, sizeof(s_floor_live));
    aMR_pc_stored_cells(mp_floor_cell, NULL);
    aMR_pc_switch_tables(mp_floor_switch, NULL);
}

// the save's bytes as they stream: the floor the host is in as it stands
static const u8* mp_gen_live(u32 off, u32 len, u8* tmp) {
    const u8* raw = mp_save_base() + off;
    u32 a;
    u32 b;

    if (s_floor_off == 0 || off + len <= s_floor_off || off >= s_floor_off + sizeof(s_floor_live)) {
        return raw;
    }
    memcpy(tmp, raw, len);
    a = off > s_floor_off ? off : s_floor_off;
    b = (off + len < s_floor_off + sizeof(s_floor_live)) ? off + len : s_floor_off + sizeof(s_floor_live);
    memcpy(tmp + (a - off), s_floor_live + (a - s_floor_off), b - a);
    return tmp;
}

static void mp_host_gen_scan(void) {
    u8 msg[8 + 400];
    u8 tmp[400 + 64];
    u32 budget = MP_GEN_STEP;

    mp_host_floor_take();
    while (budget > 0 && s_gen_bytes > 0) {
        u32 left;
        u32 off = mp_gen_save_off(s_w.gen_scan, &left);
        u32 len = left < 64 ? left : 64;
        const u8* live = mp_gen_live(off, left < sizeof(tmp) ? left : sizeof(tmp), tmp);
        u8* shadow = s_w.gen_shadow + s_w.gen_scan;

        if (memcmp(live, shadow, len) != 0) {
            // coalesce the changed run up to one message
            u32 run = len;

            while (run < 400 && run < left && memcmp(live + run, shadow + run, (run + 64 <= left ? 64 : left - run)) != 0) {
                run += (run + 64 <= left) ? 64 : left - run;
            }
            if (run > 400) {
                run = 400;
            }
            msg[0] = MP_M_BYTES;
            mp_put32w(msg + 1, off);
            mp_put16(msg + 5, run);
            memcpy(msg + 7, live, run);
            mp_send_all(msg, 7 + run, -1);
            memcpy(shadow, live, run);
            len = run;
        }
        s_w.gen_scan += len;
        budget = (budget > len) ? budget - len : 0;
        if (s_w.gen_scan >= s_gen_bytes) {
            s_w.gen_scan = 0;
        }
    }
}

// anti-entropy

enum {
    MP_SPACE_CELLS,
    MP_SPACE_BYTES,
};

static u32 mp_page_crc(int space, int page) {
    if (space == MP_SPACE_CELLS) {
        int first = page * MP_CELL_PAGE;
        int count = (first + MP_CELL_PAGE <= MP_CELLS) ? MP_CELL_PAGE : MP_CELLS - first;

        return mp_crc32(&s_w.view[first], count * 2, 0);
    } else {
        u32 lin = (u32)page * MP_GEN_PAGE;
        u32 crc = 0;
        u32 want = (lin + MP_GEN_PAGE <= s_gen_bytes) ? MP_GEN_PAGE : s_gen_bytes - lin;

        if (mp_is_host() && s_base == NULL) {
            // what guests were sent: a change the scan hasn't reached yet isn't a difference
            return mp_crc32(s_w.gen_shadow + lin, (int)want, 0);
        }
        while (want > 0) {
            u32 left;
            u32 off = mp_gen_save_off(lin, &left);
            u32 n = left < want ? left : want;

            // (the house floor held for this game's room is the host's as heard, not the room's)
            if (s_floor_held != 0 && s_base == NULL && off < s_floor_held + MP_FLOOR_BYTES && off + n > s_floor_held) {
                u32 a = off > s_floor_held ? off : s_floor_held;
                u32 b = off + n < s_floor_held + MP_FLOOR_BYTES ? off + n : s_floor_held + MP_FLOOR_BYTES;

                crc = mp_crc32(mp_save_base() + off, (int)(a - off), crc);
                crc = mp_crc32(s_w.gen_shadow + lin + (a - off), (int)(b - a), crc);
                crc = mp_crc32(mp_save_base() + b, (int)(off + n - b), crc);
            } else {
                crc = mp_crc32(mp_save_base() + off, (int)n, crc);
            }
            lin += n;
            want -= n;
        }
        return crc;
    }
}

static int mp_pages(int space) {
    return space == MP_SPACE_CELLS ? MP_CELL_PAGES : (int)((s_gen_bytes + MP_GEN_PAGE - 1) / MP_GEN_PAGE);
}

static void mp_host_send_crcs(int slot) {
    u8 msg[5 + 100 * 4];
    int space;
    int s;

    for (space = MP_SPACE_CELLS; space <= MP_SPACE_BYTES; space++) {
        int pages = mp_pages(space);
        int first;

        for (first = 0; first < pages; first += 100) {
            int n = pages - first < 100 ? pages - first : 100;
            int i;

            msg[0] = MP_M_CRCS;
            msg[1] = (u8)space;
            mp_put16(msg + 2, (u32)first);
            msg[4] = (u8)n;
            for (i = 0; i < n; i++) {
                mp_put32w(msg + 5 + i * 4, mp_page_crc(space, first + i));
            }
            if (slot > 0) {
                mp_send_guest(slot, msg, 5 + n * 4);
                continue;
            }
            // (a quiet guest gets a round of its own once it's back)
            for (s = 1; s < MP_MAX_PEERS; s++) {
                if (mp_guest_in_town(s) && !mp_lobby_conn_quiet(mp_lobby_guest_conn(s))) {
                    mp_send_guest(s, msg, 5 + n * 4);
                }
            }
        }
    }
}

static void mp_host_send_page(int slot, int space, int page) {
    u8 msg[8 + MP_GEN_PAGE];

    if (page < 0 || page >= mp_pages(space)) {
        return;
    }
    if (space == MP_SPACE_CELLS) {
        int first = page * MP_CELL_PAGE;
        int count = (first + MP_CELL_PAGE <= MP_CELLS) ? MP_CELL_PAGE : MP_CELLS - first;
        int i;

        msg[0] = MP_M_CELLS;
        mp_put16(msg + 1, (u32)first);
        msg[3] = (u8)count;
        for (i = 0; i < count; i++) {
            mp_put16(msg + 4 + i * 2, s_w.view[first + i]);
        }
        mp_send_guest(slot, msg, 4 + count * 2);
    } else {
        u32 lin = (u32)page * MP_GEN_PAGE;
        u32 want = (lin + MP_GEN_PAGE <= s_gen_bytes) ? MP_GEN_PAGE : s_gen_bytes - lin;

        while (want > 0) {
            u32 left;
            u32 off = mp_gen_save_off(lin, &left);
            u32 n = left < want ? left : want;

            msg[0] = MP_M_BYTES;
            mp_put32w(msg + 1, off);
            mp_put16(msg + 5, n);
            memcpy(msg + 7, s_w.gen_shadow + lin, n);
            mp_send_guest(slot, msg, 7 + (int)n);
            lin += n;
            want -= n;
        }
    }
}

// host: each joiner's snapshot page by page as it was taken; what changed by the time it arrives goes to it first,
// so the checksums after find nothing to repair
static struct {
    u8 have;
    u32 cells[MP_CELL_PAGES];
    u32 bytes[MP_GEN_PAGES_MAX];
} s_snap_crc[MP_MAX_PEERS];

void mp_world_snap_taken(int conn, const void* raw) {
    int slot = mp_lobby_guest_slot(conn) + 1;
    u16 cells[MP_CELL_PAGE];
    int p;

    if (slot <= 0 || slot >= MP_MAX_PEERS) {
        return;
    }
    mp_gen_build();
    s_base = (u8*)raw;
    for (p = 0; p < MP_CELL_PAGES; p++) {
        int first = p * MP_CELL_PAGE;
        int count = (first + MP_CELL_PAGE <= MP_CELLS) ? MP_CELL_PAGE : MP_CELLS - first;
        int i;

        // (as the guest's world will take them from it)
        for (i = 0; i < count; i++) {
            cells[i] = mp_raw_get(first + i);
        }
        s_snap_crc[slot].cells[p] = mp_crc32(cells, count * 2, 0);
    }
    for (p = 0; p < mp_pages(MP_SPACE_BYTES); p++) {
        s_snap_crc[slot].bytes[p] = mp_page_crc(MP_SPACE_BYTES, p);
    }
    s_base = NULL;
    s_snap_crc[slot].have = TRUE;
}

static void mp_host_catch_up(int slot) {
    int space;

    if (!s_snap_crc[slot].have) {
        return;
    }
    s_snap_crc[slot].have = FALSE;
    for (space = MP_SPACE_CELLS; space <= MP_SPACE_BYTES; space++) {
        const u32* crc = (space == MP_SPACE_CELLS) ? s_snap_crc[slot].cells : s_snap_crc[slot].bytes;
        int p;

        for (p = 0; p < mp_pages(space); p++) {
            if (mp_page_crc(space, p) != crc[p]) {
                mp_host_send_page(slot, space, p);
            }
        }
    }
}

static void mp_guest_on_crcs(const u8* p, int len) {
    u8 req[4 + 100 * 2];
    int space;
    int first;
    int n;
    int nr = 0;
    int i;

    if (len < 5) {
        return;
    }
    space = p[1];
    first = (int)mp_get16(p + 2);
    n = p[4];
    if (space > MP_SPACE_BYTES || 5 + n * 4 > len || first + n > mp_pages(space)) {
        return;
    }
    for (i = 0; i < n; i++) {
        if (nr < 32 && mp_page_crc(space, first + i) != mp_get32w(p + 5 + i * 4)) {
            mp_put16(req + 4 + nr * 2, (u32)(first + i));
            nr++;
        }
    }
    if (nr > 0) {
        req[0] = MP_M_PAGE_REQ;
        req[1] = (u8)space;
        req[2] = (u8)nr;
        req[3] = 0;
        mp_queue_push(s_w.host_out, req, 4 + nr * 2);
        pc_mp_log("[MP] world: %d %s pages differ from the host's", nr, space == MP_SPACE_CELLS ? "cell" : "byte");
    }
}

// messages

static void mp_guest_on_cells(const u8* p, int len) {
    int first;
    int n;
    int i;

    if (len < 4) {
        return;
    }
    first = (int)mp_get16(p + 1);
    n = p[3];
    if (4 + n * 2 > len || first + n > MP_CELLS) {
        return;
    }
    for (i = 0; i < n; i++) {
        mp_guest_take_value(first + i, (u16)mp_get16(p + 4 + i * 2));
    }
    mp_refresh_field();
}

static void mp_guest_on_delta(const u8* p, int len) {
    int n;
    int i;

    if (len < 2) {
        return;
    }
    n = p[1];
    if (2 + n * 4 > len) {
        return;
    }
    for (i = 0; i < n; i++) {
        int c = (int)mp_get16(p + 2 + i * 4);

        if (c < MP_CELLS) {
            mp_guest_take_value(c, (u16)mp_get16(p + 4 + i * 4));
        }
    }
    mp_refresh_field();
}

static void mp_shop_note_all(const Shop_c* before, const Shop_c* after);

// raffle prizes won on another screen, to go from this shop floor (the prizes stand there on raffle day)
static u16 s_raffle_gone[mSP_LOTTERY_ITEM_COUNT];
static int s_nraffle_gone;

static void mp_raffle_gone(u16 item) {
    if (s_nraffle_gone < mSP_LOTTERY_ITEM_COUNT) {
        s_raffle_gone[s_nraffle_gone++] = item;
    }
}

static void mp_guest_on_bytes(const u8* p, int len) {
    u32 off;
    u32 n;
    u32 shop = offsetof(Save_t, shop);

    if (len < 7) {
        return;
    }
    off = mp_get32w(p + 1);
    n = mp_get16(p + 5);
    if (7 + (int)n > len || !mp_gen_contains(off, n)) {
        return;
    }
    memcpy(s_w.gen_shadow + mp_gen_lin_of(off), p + 7, n); // the host's bytes as last heard
    if (mp_npc_defers(off, n)) {
        return;
    }
    // (the floor this game's room is up in: its furniture waits for the room)
    mp_guest_floor_track();
    if (s_floor_held != 0 && off < s_floor_held + MP_FLOOR_BYTES && off + n > s_floor_held) {
        u32 a = off > s_floor_held ? off : s_floor_held;
        u32 b = off + n < s_floor_held + MP_FLOOR_BYTES ? off + n : s_floor_held + MP_FLOOR_BYTES;

        memcpy(mp_save_base() + off, p + 7, a - off);
        memcpy(mp_save_base() + b, p + 7 + (b - off), off + n - b);
        s_floor_heard_ms = pc_mp_now_ms();
        return;
    }
    if (off < shop + sizeof(Shop_c) && off + n > shop) {
        Shop_c before = Save_Get(shop);
        int k;

        memcpy(mp_save_base() + off, p + 7, n);
        mp_shop_note_all(&before, Save_GetPointer(shop));
        for (k = 0; k < mSP_LOTTERY_ITEM_COUNT; k++) {
            if (ITEM_IS_FTR(before.lottery_items[k]) && Save_Get(shop).lottery_items[k] == RSV_SHOP_SOLD_FTR) {
                mp_raffle_gone(before.lottery_items[k]);
            }
        }
        s_w.shop_last = Save_Get(shop); // the host's shop, not a purchase of ours
    } else {
        memcpy(mp_save_base() + off, p + 7, n);
    }
    if (off < offsetof(Save_t, animals) + sizeof(((Save_t*)0)->animals) && off + n > offsetof(Save_t, animals)) {
        mp_npc_animals_changed();
    }
    if (off < offsetof(Save_t, melody) + sizeof(u64) && off + n > offsetof(Save_t, melody)) {
        s_w.melody_last = Save_Get(melody);
    }
    // another player's trade at the Able Sisters': the stands here show it
    if (off < offsetof(Save_t, needlework) + sizeof(mNW_needlework_c) && off + n > offsetof(Save_t, needlework)) {
        int first = (int)((off > offsetof(Save_t, needlework) ? off - offsetof(Save_t, needlework) : 0) /
                          sizeof(mNW_original_design_c));
        int last = (int)((off + n - 1 - offsetof(Save_t, needlework)) / sizeof(mNW_original_design_c));
        int k;

        for (k = first; k <= last && k < mNW_TOTAL_DESIGN_NUM; k++) {
            aNI_mp_refresh(k);
        }
    }
}

// the shop
// purchases merge: a sold slot stays sold, sales add up, and nobody is refused

static int mp_shop_goods(mActor_name_t v) {
    return v != EMPTY_NO && v != RSV_NO && (v & 0xFF00) != 0xFE00;
}

static void mp_shop_note(mActor_name_t was, mActor_name_t now) {
    if (mp_shop_goods(was) && now >= RSV_SHOP_SOLD_PAPER && now <= RSV_SHOP_SOLD_SIGNBOARD &&
        s_w.nshop_show < MP_SHOP_SHOWS) {
        s_w.shop_show[s_w.nshop_show].was = was;
        s_w.shop_show[s_w.nshop_show].now = now;
        s_w.nshop_show++;
    }
}

// what another player bought, to be shown sold on this floor too
static void mp_shop_note_all(const Shop_c* before, const Shop_c* after) {
    int i;

    for (i = 0; i < mSP_GOODS_COUNT; i++) {
        mp_shop_note(before->items[i], after->items[i]);
    }
    mp_shop_note(before->rare_item, after->rare_item);
    for (i = after->flowers_candy_grab_bag_count; i < before->flowers_candy_grab_bag_count; i++) {
        mp_shop_note(ITM_HUKUBUKURO_BAG, RSV_SHOP_SOLD_PLANT); // candy days are looked up at the shelf
    }
}

// a clerk mid-sale holds the item and its spot: the floor changes after
static int mp_shop_busy(GAME_PLAY* play) {
    ACTOR* a;

    if (mDemo_CheckDemo()) {
        return TRUE;
    }
    for (a = play->actor_info.list[ACTOR_PART_NPC].actor; a != NULL; a = a->next_actor) {
        switch (a->id) {
            case mAc_PROFILE_NPC_SHOP_MASTER:
            case mAc_PROFILE_NPC_CONV_MASTER:
            case mAc_PROFILE_NPC_SUPER_MASTER:
            case mAc_PROFILE_NPC_DEPART_MASTER:
            case mAc_PROFILE_NPC_MAMEDANUKI:
                if (((NPC_SHOP_COMMON_ACTOR*)a)->sell_item != EMPTY_NO) {
                    return TRUE;
                }
                break;
        }
    }
    return FALSE;
}

// the display half of a sale (aSD_ReportGoodsSales), without its sales and save bookkeeping
static void mp_shop_show_sold(mActor_name_t was, mActor_name_t now) {
    mActor_name_t* fg = mFI_BkNumtoUtFGTop(0, 0);
    int i;
    int ux;
    int uz;
    int id;

    if (fg == NULL) {
        return;
    }
    for (i = 0; i < UT_TOTAL_NUM && fg[i] != was; i++) {
    }
    if (i == UT_TOTAL_NUM && was == ITM_HUKUBUKURO_BAG) {
        was = ITM_FOOD_CANDY;
        for (i = 0; i < UT_TOTAL_NUM && fg[i] != was; i++) {
        }
    }
    if (i == UT_TOTAL_NUM) {
        return; // on the other floor, or already shown
    }
    ux = i % UT_X_NUM;
    uz = i / UT_X_NUM;
    if (ITEM_IS_FTR(was)) {
        if (aMR_CLIP != NULL && aMR_CLIP->unitNum2ftrItemNoftrId_proc(NULL, &id, ux, uz, mCoBG_LAYER0)) {
            aMR_CLIP->ftrId2extinguishFtr_proc(id); // while the unit still names it
        }
        mFI_UtNumtoFGSet_common(now, ux, uz, FALSE);
    } else if (ITEM_IS_CLOTH(was)) {
        if (CLIP(shop_manekin_clip) != NULL) {
            CLIP(shop_manekin_clip)->change2naked_manekin_proc(ux, uz);
        }
        mFI_UtNumtoFGSet_common(RSV_NO, ux, uz, FALSE);
    } else {
        if (ITEM_IS_UMBRELLA2(was) && CLIP(shop_umbrella_clip) != NULL) {
            CLIP(shop_umbrella_clip)->delete_umbrella_proc(ux, uz);
        }
        mFI_UtNumtoFGSet_common(now, ux, uz, FALSE);
    }
}

static void mp_shop_apply(GAME_PLAY* play) {
    int k;

    if (s_nraffle_gone > 0 && !mFI_CheckShop()) {
        s_nraffle_gone = 0; // (elsewhere the floor comes from the save next time)
    } else if (s_nraffle_gone > 0 && play != NULL && !mp_shop_busy(play)) {
        for (k = 0; k < s_nraffle_gone; k++) {
            mp_shop_show_sold(s_raffle_gone[k], EMPTY_NO);
        }
        s_nraffle_gone = 0;
    }
    if (s_w.nshop_show == 0) {
        return;
    }
    // elsewhere the shelves come from the save at the next visit; event days lay out their own
    if (!mFI_CheckShop() || Common_Get(tanuki_shop_status) == mSP_TANUKI_SHOP_STATUS_EVENT ||
        Common_Get(tanuki_shop_status) == mSP_TANUKI_SHOP_STATUS_FUKUBIKI) {
        s_w.nshop_show = 0;
        return;
    }
    if (mp_shop_busy(play)) {
        return;
    }
    for (k = 0; k < s_w.nshop_show; k++) {
        mp_shop_show_sold(s_w.shop_show[k].was, s_w.shop_show[k].now);
    }
    s_w.nshop_show = 0;
}

#define MP_SLOT_RARE 0xFE

static void mp_guest_shop_scan(void) {
    Shop_c* live = Save_GetPointer(shop);
    Shop_c* last = &s_w.shop_last;
    u8 msg[8];
    int i;

    for (i = 0; i <= mSP_GOODS_COUNT; i++) {
        mActor_name_t was = (i < mSP_GOODS_COUNT) ? last->items[i] : last->rare_item;
        mActor_name_t now_item = (i < mSP_GOODS_COUNT) ? live->items[i] : live->rare_item;

        if (was != now_item) {
            msg[0] = MP_M_SOFT;
            msg[1] = MP_SOFT_SLOT;
            msg[2] = (u8)(i < mSP_GOODS_COUNT ? i : MP_SLOT_RARE);
            mp_put16(msg + 3, was);
            mp_put16(msg + 5, now_item);
            mp_queue_push(s_w.host_out, msg, 7);
        }
    }
    if (live->flowers_candy_grab_bag_count != last->flowers_candy_grab_bag_count) {
        msg[0] = MP_M_SOFT;
        msg[1] = MP_SOFT_BAGS;
        msg[2] = (u8)(s8)(live->flowers_candy_grab_bag_count - last->flowers_candy_grab_bag_count);
        mp_queue_push(s_w.host_out, msg, 3);
    }
    if (live->sales_sum > last->sales_sum) {
        msg[0] = MP_M_SOFT;
        msg[1] = MP_SOFT_SALES;
        mp_put32w(msg + 2, live->sales_sum - last->sales_sum);
        mp_queue_push(s_w.host_out, msg, 6);
    }
    if (live->visitor_flag && !last->visitor_flag) {
        msg[0] = MP_M_SOFT;
        msg[1] = MP_SOFT_VISITOR;
        mp_queue_push(s_w.host_out, msg, 2);
    }
    *last = *live;
    if (Save_Get(melody) != s_w.melody_last) {
        u8 tune[10];

        s_w.melody_last = Save_Get(melody);
        tune[0] = MP_M_SOFT;
        tune[1] = MP_SOFT_MELODY;
        mp_put32w(tune + 2, (u32)s_w.melody_last);
        mp_put32w(tune + 6, (u32)(s_w.melody_last >> 32));
        mp_queue_push(s_w.host_out, tune, 10);
    }
}

// host: a visitor's design as the game itself writes it (the save check takes palettes 0-15 only); a stand's old
// design leaves the villagers wearing it, and the islander remembers who set the flag
static void mp_host_design(int slot, int target, const mNW_original_design_c* d) {
    mNW_original_design_c* dst = mp_design_of(target);
    u32 off = (u32)((u8*)dst - mp_save_base());

    if (d->palette >= 16) {
        return;
    }
    if (target < mNW_TOTAL_DESIGN_NUM) {
        mNW_CopyOriginalTextureClass(dst, (mNW_original_design_c*)d);
        aNNW_mp_trend_delete(target);
        aNI_mp_refresh(target);
        mp_npc_stand_replaced(target);
    } else {
        Anmmem_c* mem = Save_Get(island).animal.memories;
        PersonalID_c pid;
        int idx;

        bcopy(&d->design, &dst->design, sizeof(mNW_original_tex_c));
        dst->flag_design_set = TRUE;
        dst->palette = d->palette;
        if (mp_lobby_guest_pid(mp_lobby_guest_conn(slot), &pid) &&
            (idx = mNpc_GetAnimalMemoryIdx(&pid, mem, ANIMAL_MEMORY_NUM)) != -1) {
            mem[idx].memuni.island.check |= mISL_PLAYER_ACTION_CHANGE_FLAG;
            mp_world_host_took((u32)((u8*)&mem[idx].memuni.island.check - mp_save_base()), sizeof(u32), slot);
        }
    }
    mp_world_host_took(off, MP_DESIGN_HALF, slot);
    mp_world_host_took(off + MP_DESIGN_HALF, MP_DESIGN_HALF, slot);
}

static int mp_guest_visiting(void);

// shops' one-of-a-kind things: furniture and what's worn or laid out (tools, paint, seeds and such never run out)
static int mp_shop_claimable(u16 item) {
    int cat;

    if (ITEM_IS_FTR(item)) {
        return TRUE;
    }
    if (ITEM_NAME_GET_TYPE(item) != NAME_TYPE_ITEM1) {
        return FALSE;
    }
    cat = ITEM_NAME_GET_CAT(item);
    return cat == ITEM1_CAT_CLOTH || cat == ITEM1_CAT_CARPET || cat == ITEM1_CAT_WALL;
}

#define MP_CLAIMS        16
#define MP_CLAIM_MS      60000 // a purchase begun and not made lapses
#define MP_CLAIM_DONE_MS 10000 // one made still counts till the shelf has it sold
#define MP_SOLD          64

static struct {
    // host: purchases under way, and what's gone today that no shelf of Nook's shows (Redd's, a sale day's)
    struct {
        u16 item;
        u8 slot;
        u8 done;
        u32 ms;
    } claim[MP_CLAIMS];
    u16 sold[MP_SOLD];
    int nsold;
    int day;
    // guest: its own
    u16 item;
    u8 state;
    u32 ms;
} s_claim;

// host: unsold spots of Nook's with the thing
static int mp_host_stock(u16 item) {
    Shop_c* shop = Save_GetPointer(shop);
    int n = shop->rare_item == item;
    int i;

    for (i = 0; i < mSP_GOODS_COUNT; i++) {
        n += shop->items[i] == item;
    }
    return n;
}

// host: the thing is this player's to buy while nobody else is buying it and some is left
static int mp_host_claim(u16 item, int slot) {
    u32 now = pc_mp_now_ms();
    int stock = mp_host_stock(item);
    int held = 0;
    int k;

    if (s_claim.day != Common_Get(time).rtc_time.day) {
        s_claim.day = Common_Get(time).rtc_time.day;
        s_claim.nsold = 0;
    }
    for (k = 0; k < MP_CLAIMS; k++) {
        if (s_claim.claim[k].item != EMPTY_NO &&
            now - s_claim.claim[k].ms > (s_claim.claim[k].done ? MP_CLAIM_DONE_MS : MP_CLAIM_MS)) {
            s_claim.claim[k].item = EMPTY_NO;
        }
    }
    for (k = 0; k < MP_CLAIMS; k++) {
        if (s_claim.claim[k].item == item) {
            if (s_claim.claim[k].slot == slot && !s_claim.claim[k].done) {
                return TRUE;
            }
            held++;
        }
    }
    for (k = 0; stock == 0 && k < s_claim.nsold; k++) {
        held += s_claim.sold[k] == item;
    }
    if (held >= (stock > 0 ? stock : 1)) {
        return FALSE;
    }
    for (k = 0; k < MP_CLAIMS && s_claim.claim[k].item != EMPTY_NO; k++) {
    }
    if (k == MP_CLAIMS) {
        return FALSE;
    }
    s_claim.claim[k].item = item;
    s_claim.claim[k].slot = (u8)slot;
    s_claim.claim[k].done = FALSE;
    s_claim.claim[k].ms = now;
    return TRUE;
}

static void mp_host_unclaim(u16 item, int slot) {
    int k;

    for (k = 0; k < MP_CLAIMS; k++) {
        if (s_claim.claim[k].item == item && s_claim.claim[k].slot == slot && !s_claim.claim[k].done) {
            s_claim.claim[k].item = EMPTY_NO;
        }
    }
}

static void mp_host_claim_done(u16 item, int slot) {
    int k;

    for (k = 0; k < MP_CLAIMS; k++) {
        if (s_claim.claim[k].item == item && s_claim.claim[k].slot == slot && !s_claim.claim[k].done) {
            s_claim.claim[k].done = TRUE;
            s_claim.claim[k].ms = pc_mp_now_ms();
            break;
        }
    }
    if (mp_host_stock(item) == 0 && s_claim.nsold < MP_SOLD) {
        s_claim.sold[s_claim.nsold++] = item;
    }
}

static void mp_shop_claim_drop(void);

int mp_shop_claim(unsigned short item) {
    u8 msg[4];

    if (!s_w.live || !mp_shop_claimable(item)) {
        return TRUE;
    }
    if (s_claim.item != EMPTY_NO && s_claim.item != item) {
        mp_shop_claim_drop(); // (one thing bought at a time: the last one asked about is free again)
    }
    s_claim.item = item;
    s_claim.ms = pc_mp_now_ms();
    if (mp_is_host()) {
        s_claim.state = mp_host_claim(item, 0) ? 1 : 2;
        return s_claim.state == 1;
    }
    if (!mp_guest_visiting()) {
        s_claim.state = 1;
        return TRUE;
    }
    s_claim.state = 0;
    msg[0] = MP_M_SOFT;
    msg[1] = MP_SOFT_CLAIM;
    mp_put16(msg + 2, item);
    mp_queue_push(s_w.host_out, msg, 4);
    return TRUE;
}

int mp_shop_claim_state(unsigned short item) {
    if (!s_w.live || !mp_shop_claimable(item)) {
        return 1;
    }
    if (s_claim.item != item) {
        mp_shop_claim(item);
    }
    // (no word from the host: nothing is bought)
    if (s_claim.state == 0 && pc_mp_now_ms() - s_claim.ms > 6000) {
        s_claim.state = 2;
    }
    return s_claim.state;
}

// the islander's things are the host's: what a visitor hands it or takes from it, the host's islander does too
void mp_island_ftr(int set, const void* pid, unsigned short ftr) {
    u8 msg[5 + sizeof(PersonalID_c)];

    if (!s_w.live || mp_is_host() || !mp_guest_visiting() || (set && pid == NULL)) {
        return;
    }
    msg[0] = MP_M_SOFT;
    msg[1] = MP_SOFT_ISLAND;
    msg[2] = (u8)(set != 0);
    mp_put16(msg + 3, ftr);
    if (set) {
        memcpy(msg + 5, pid, sizeof(PersonalID_c));
    }
    // (with the talk's result, once the passport has what changed hands)
    if (!mp_npc_talk_extra(msg, set ? (int)sizeof(msg) : 5)) {
        mp_queue_push(s_w.host_out, msg, set ? (int)sizeof(msg) : 5);
    }
}

void mp_island_check(const void* pid, unsigned int check) {
    u8 msg[7 + sizeof(PersonalID_c)];

    if (!s_w.live || mp_is_host() || !mp_guest_visiting() || pid == NULL) {
        return;
    }
    msg[0] = MP_M_SOFT;
    msg[1] = MP_SOFT_ISLAND;
    msg[2] = 2;
    mp_put32w(msg + 3, check);
    memcpy(msg + 7, pid, sizeof(PersonalID_c));
    mp_queue_push(s_w.host_out, msg, (int)sizeof(msg));
}

void mp_island_named(void) {
    u8 msg[3 + mISL_ISLAND_NAME_LEN + sizeof(lbRTC_time_c)];

    if (!s_w.live || mp_is_host() || !mp_guest_visiting()) {
        return;
    }
    msg[0] = MP_M_SOFT;
    msg[1] = MP_SOFT_ISLAND;
    msg[2] = 3;
    memcpy(msg + 3, Save_Get(island).name, mISL_ISLAND_NAME_LEN);
    memcpy(msg + 3 + mISL_ISLAND_NAME_LEN, Save_GetPointer(island.renew_time), sizeof(lbRTC_time_c));
    mp_queue_push(s_w.host_out, msg, (int)sizeof(msg));
}

void mp_raffle_won(int slot, unsigned short item) {
    u8 msg[5];

    if (!s_w.live) {
        return;
    }
    mp_shop_claim_done(item);
    if (!mp_is_host() && mp_guest_visiting()) {
        msg[0] = MP_M_SOFT;
        msg[1] = MP_SOFT_RAFFLE;
        msg[2] = (u8)slot;
        mp_put16(msg + 3, item);
        mp_queue_push(s_w.host_out, msg, 5);
    }
}

void mp_shop_claim_done(unsigned short item) {
    u8 msg[4];

    if (!s_w.live || !mp_shop_claimable(item)) {
        return;
    }
    s_claim.item = EMPTY_NO;
    if (mp_is_host()) {
        mp_host_claim_done(item, 0);
    } else if (mp_guest_visiting()) {
        msg[0] = MP_M_SOFT;
        msg[1] = MP_SOFT_CLAIMED;
        mp_put16(msg + 2, item);
        mp_queue_push(s_w.host_out, msg, 4);
    }
}

// a purchase begun and not made: the thing is free for the others again
static void mp_shop_claim_drop(void) {
    u16 item = s_claim.item;
    u8 msg[4];

    s_claim.item = EMPTY_NO;
    if (item == EMPTY_NO) {
        return;
    }
    if (mp_is_host()) {
        mp_host_unclaim(item, 0);
    } else if (mp_guest_visiting()) {
        msg[0] = MP_M_SOFT;
        msg[1] = MP_SOFT_UNCLAIM;
        mp_put16(msg + 2, item);
        mp_queue_push(s_w.host_out, msg, 4);
    }
}

// ...once the talk it was begun in is over
static void mp_claim_watch(void) {
    static u32 s_talk_ms;

    if (s_claim.item == EMPTY_NO || mDemo_CheckDemo()) {
        s_talk_ms = pc_mp_now_ms();
    } else if (pc_mp_now_ms() - s_talk_ms > 1000) {
        mp_shop_claim_drop();
    }
}

static void mp_guest_on_claim(const u8* p, int len) {
    if (len >= 4 && s_claim.item == (u16)mp_get16(p + 1) && s_claim.state == 0) {
        s_claim.state = p[3] ? 1 : 2;
    }
}

// host: what a visitor's game would have moved out of the way to the lost & found (an event taking its acre, a
// building's doorstep, a spot someone comes out on) goes there on the host's side instead
static void mp_host_town_keep(int kind, int bx, int bz, int ux, int uz) {
    int c0;
    int k;

    if (kind < MP_KEEP_UNIT || kind > MP_KEEP_BLOCK || bx < 1 || bx > FG_BLOCK_X_NUM || bz < 1 ||
        bz > FG_BLOCK_Z_NUM || ux >= UT_X_NUM || uz >= UT_Z_NUM) {
        return;
    }
    c0 = s_rg[MP_RG_FG].base + ((bz - 1) * FG_BLOCK_X_NUM + (bx - 1)) * UT_TOTAL_NUM;
    for (k = 0; k < UT_TOTAL_NUM; k++) {
        int c = c0 + k;
        u16 v = mp_raw_get(c);
        int type = ITEM_NAME_GET_TYPE(v);

        // (what an actor or the host's player has in hand stays)
        if ((kind != MP_KEEP_BLOCK && k != ux + uz * UT_X_NUM) || MP_BIT_GET(s_w.held, c) ||
            MP_BIT_GET(s_w.lock, c)) {
            continue;
        }
        if ((ITEM_IS_BURIED_PITFALL_HOLE(v) || v == SHINE_SPOT) && kind != MP_KEEP_BLOCK) {
            mPB_keep_item(bg_item_fg_sub_dig2take_conv(v));
        } else if (ITEM_IS_SIGNBOARD(v) && kind != MP_KEEP_BLOCK) {
            mPB_keep_item(ITM_SIGNBOARD);
        } else if ((type == NAME_TYPE_FTR0 || type == NAME_TYPE_ITEM1 || type == NAME_TYPE_FTR1) &&
                   kind != MP_KEEP_FLAT) {
            mPB_keep_item(v);
        } else {
            continue;
        }
        mp_raw_set(c, EMPTY_NO);
        mp_raw_set(s_rg[MP_RG_DEP].base + (c - s_rg[MP_RG_FG].base), 0);
    }
    mp_refresh_field();
}

// host: a visitor's item for the lost & found; one standing for a pickup of its the host hasn't saved takes that
// pickup's place (it isn't given back to the town as well)
static void mp_host_keep_from(mp_guest_w_t* g, const u8* p, int len) {
    if (len >= 7 && p[6]) {
        mp_pick_cancel(g, (mActor_name_t)mp_get16(p + 2));
    }
    mp_host_keep((mActor_name_t)mp_get16(p + 2), TRUE);
}

static void mp_host_on_soft(mp_guest_w_t* g, const u8* p, int len) {
    Shop_c* shop = Save_GetPointer(shop);
    int i;

    if (len < 2) {
        return;
    }
    switch (p[1]) {
        case MP_SOFT_KEEP:
            // only an item this guest was just refused setting down, or set down in an op it undid before sending
            for (i = 0; i < MP_REFUSED && len >= 4; i++) {
                if (g->refused[i] != EMPTY_NO && g->refused[i] == (mActor_name_t)mp_get16(p + 2)) {
                    g->refused[i] = EMPTY_NO;
                    mp_host_keep_from(g, p, len);
                    break;
                }
            }
            if (i == MP_REFUSED && len >= 6 && (!g->rx_any || (s16)((u16)mp_get16(p + 4) - g->rx_op) > 0) &&
                mp_pocketable((mActor_name_t)mp_get16(p + 2))) {
                mp_host_keep_from(g, p, len);
            }
            break;
        case MP_SOFT_SLOT:
            if (len >= 7) {
                mActor_name_t was = (mActor_name_t)mp_get16(p + 3);
                mActor_name_t now_item = (mActor_name_t)mp_get16(p + 5);
                mActor_name_t* slot = (p[2] == MP_SLOT_RARE) ? &shop->rare_item
                                      : (p[2] < mSP_GOODS_COUNT) ? &shop->items[p[2]] : NULL;

                if (slot != NULL && *slot == was) {
                    *slot = now_item;
                    mp_shop_note(was, now_item);
                }
            }
            break;
        case MP_SOFT_BAGS:
            if (len >= 3) {
                int count = shop->flowers_candy_grab_bag_count + (s8)p[2];

                for (i = count < 0 ? 0 : count; i < shop->flowers_candy_grab_bag_count; i++) {
                    mp_shop_note(ITM_HUKUBUKURO_BAG, RSV_SHOP_SOLD_PLANT);
                }
                shop->flowers_candy_grab_bag_count = (s8)(count < 0 ? 0 : count);
            }
            break;
        case MP_SOFT_SALES:
            if (len >= 6) {
                mSP_PlusSales(mp_get32w(p + 2));
            }
            break;
        case MP_SOFT_VISITOR:
            shop->visitor_flag = TRUE;
            break;
        case MP_SOFT_MELODY:
            if (len >= 10 && mp_rule(MP_RULE_TUNE)) {
                Save_Set(melody, (u64)mp_get32w(p + 2) | ((u64)mp_get32w(p + 6) << 32));
            }
            break;
        case MP_SOFT_LIGHTHOUSE:
            if (len >= 3 && p[2] < 8) {
                Save_Get(LightHouse).days_switched_on |= (u8)(1 << p[2]);
                mp_world_host_took(offsetof(Save_t, LightHouse), sizeof(LightHouse_c), (int)(g - s_w.guests));
            }
            break;
        case MP_SOFT_BOARD:
            if (len >= 2 + (int)sizeof(mNtc_board_post_c) && mp_rule(MP_RULE_BOARD)) {
                mNtc_board_post_c post;

                memcpy(&post, p + 2, sizeof(post));
                mFont_clean_save_text(post.message, sizeof(post.message)); // (it stays in the town's save)
                mNtc_notice_write(&post);
            }
            break;
        case MP_SOFT_TOWNKEEP:
            if (len >= 7) {
                mp_host_town_keep(p[2], p[3], p[4], p[5], p[6]);
            }
            break;
        case MP_SOFT_CLAIM:
            if (len >= 4) {
                u8 ans[4];

                ans[0] = MP_M_CLAIM;
                mp_put16(ans + 1, mp_get16(p + 2));
                ans[3] = (u8)mp_host_claim((u16)mp_get16(p + 2), (int)(g - s_w.guests));
                mp_send_guest((int)(g - s_w.guests), ans, 4);
            }
            break;
        case MP_SOFT_CLAIMED:
            if (len >= 4) {
                mp_host_claim_done((u16)mp_get16(p + 2), (int)(g - s_w.guests));
            }
            break;
        case MP_SOFT_UNCLAIM:
            if (len >= 4) {
                mp_host_unclaim((u16)mp_get16(p + 2), (int)(g - s_w.guests));
            }
            break;
        case MP_SOFT_HOLD:
            if (len >= 6) {
                g->hold = p[2] != 0;
                g->hold_any = p[3] != 0;
                g->hold_mark = (u16)mp_get16(p + 4);
                s_w.hurry |= g->hold;
            }
            break;
        case MP_SOFT_ISLAND:
            if (len >= 5 && p[2] == 0) {
                mNpc_EraseIslandFtr((mActor_name_t)mp_get16(p + 3));
            } else if (len >= 5 + (int)sizeof(PersonalID_c) && p[2] == 1) {
                PersonalID_c pid;

                memcpy(&pid, p + 5, sizeof(pid));
                mNpc_SetIslandFtr(&pid, (mActor_name_t)mp_get16(p + 3));
            } else if (len >= 7 + (int)sizeof(PersonalID_c) && p[2] == 2) {
                Anmmem_c* memory = Save_Get(island).animal.memories;
                PersonalID_c pid;
                int idx;

                memcpy(&pid, p + 7, sizeof(pid));
                idx = mNpc_GetAnimalMemoryIdx(&pid, memory, ANIMAL_MEMORY_NUM);
                if (idx != -1) {
                    memory[idx].memuni.island.check = mp_get32w(p + 3);
                }
            } else if (len >= 3 + mISL_ISLAND_NAME_LEN + (int)sizeof(lbRTC_time_c) && p[2] == 3 &&
                       Save_Get(island).renew_time.year == 0) {
                // (the first to go out names it; the host's own trip, or another visitor's, may have already)
                memcpy(Save_Get(island).name, p + 3, mISL_ISLAND_NAME_LEN);
                memcpy(Save_GetPointer(island.renew_time), p + 3 + mISL_ISLAND_NAME_LEN, sizeof(lbRTC_time_c));
                mISL_KeepIsland(Save_GetPointer(island));
            }
            break;
        case MP_SOFT_RAFFLE:
            // (the prize is that visitor's: the host's raffle has it gone, and its shop floor shows it)
            if (len >= 5 && p[2] < mSP_LOTTERY_ITEM_COUNT && Save_Get(shop).lottery_items[p[2]] == (u16)mp_get16(p + 3)) {
                Save_Get(shop).lottery_items[p[2]] = RSV_SHOP_SOLD_FTR;
                mp_raffle_gone((u16)mp_get16(p + 3));
            }
            break;
        case MP_SOFT_DESIGN:
            if (len >= 4 + (int)MP_DESIGN_HALF && p[2] <= MP_DESIGN_FLAG && p[3] < 2 && mp_rule(MP_RULE_DESIGNS)) {
                memcpy((u8*)&g->design + p[3] * MP_DESIGN_HALF, p + 4, MP_DESIGN_HALF);
                if (p[3] == 0) {
                    g->design_half = (u8)(p[2] + 1);
                } else if (g->design_half == p[2] + 1) {
                    g->design_half = 0;
                    mp_text_clean(g->design.name, sizeof(g->design.name));
                    mp_host_design((int)(g - s_w.guests), p[2], &g->design);
                }
            }
            break;
    }
}

static void mp_host_on_leaving(mp_guest_w_t* g, int slot, const u8* p, int len);

// one queued message, now that the frame is at a quiet point
static void mp_world_dispatch(int conn, const u8* p, int len) {
    if (mp_is_host()) {
        mp_guest_w_t* g = mp_gw_of_conn(conn);
        int slot = mp_lobby_guest_slot(conn) + 1;

        if (g == NULL) {
            return;
        }
        switch (p[0]) {
            case MP_M_OP:
                mp_host_on_op(conn, p, len);
                break;
            case MP_M_SOFT:
                mp_host_on_soft(g, p, len);
                break;
            case MP_M_PAGE_REQ:
                if (len >= 4) {
                    int n = p[2];
                    int i;

                    for (i = 0; i < n && 4 + i * 2 + 1 < len; i++) {
                        mp_host_send_page(slot, p[1], (int)mp_get16(p + 4 + i * 2));
                    }
                }
                break;
            case MP_M_HURRY:
                s_w.hurry = TRUE;
                break;
            case MP_M_LEAVING:
                mp_host_on_leaving(g, slot, p, len);
                break;
            case MP_M_COTTAGE:
                mp_cot_host_on(slot, p, len);
                break;
        }
    } else {
        switch (p[0]) {
            case MP_M_ACK:
                mp_guest_on_ack(p, len);
                break;
            case MP_M_DELTA:
                mp_guest_on_delta(p, len);
                break;
            case MP_M_COMMIT:
                mp_guest_on_commit(p, len);
                break;
            case MP_M_CLAIM:
                mp_guest_on_claim(p, len);
                break;
            case MP_M_BYTES:
                mp_guest_on_bytes(p, len);
                break;
            case MP_M_CELLS:
                mp_guest_on_cells(p, len);
                break;
            case MP_M_CRCS:
                mp_guest_on_crcs(p, len);
                break;
            case MP_M_COTTAGE:
                mp_cot_guest_on(p, len);
                break;
        }
    }
}

// the link delivers mid-pump; the world takes messages at the top of the next play frame
void mp_world_on_rel(int conn, const unsigned char* data, int len) {
    int i;

    if (!s_w.live || len <= 0 || len > MP_MSG_MAX) {
        return;
    }
    if (s_w.inbox_len + len + 3 > MP_INBOX_BYTES) {
        int rd = 0;
        int wr = 0;

        // what the checksums repair makes room for what they can't
        while (rd < s_w.inbox_len) {
            int n = s_w.inbox[rd + 1] | (s_w.inbox[rd + 2] << 8);

            if (mp_msg_critical(s_w.inbox + rd + 3)) {
                memmove(s_w.inbox + wr, s_w.inbox + rd, n + 3);
                wr += n + 3;
            }
            rd += n + 3;
        }
        s_w.inbox_len = wr;
        if (s_w.inbox_len + len + 3 > MP_INBOX_BYTES || !mp_msg_critical(data)) {
            pc_mp_log("[MP] world: inbox full, dropped %02X", data[0]);
            return;
        }
    }
    i = s_w.inbox_len;
    s_w.inbox[i] = (u8)conn;
    s_w.inbox[i + 1] = (u8)len;
    s_w.inbox[i + 2] = (u8)(len >> 8);
    memcpy(s_w.inbox + i + 3, data, len);
    s_w.inbox_len += len + 3;
}

static void mp_world_drain_inbox(void) {
    int at = 0;

    while (at < s_w.inbox_len) {
        int conn = s_w.inbox[at];
        int len = s_w.inbox[at + 1] | (s_w.inbox[at + 2] << 8);

        mp_world_dispatch(conn, s_w.inbox + at + 3, len);
        at += len + 3;
        if (!s_w.live) {
            return; // the session ended inside a handler
        }
    }
    s_w.inbox_len = 0;
}

// commits (host): a guest's transfers are final once any durable save holding them lands

// pc_m_card: a town save just took its snapshot
void mp_world_on_save_capture(unsigned int serial) {
    mp_capture_t* c;
    int s;

    if (!s_w.live || !mp_is_host()) {
        return;
    }
    memmove(&s_w.caps[1], &s_w.caps[0], sizeof(s_w.caps[0]) * (MP_CAPTURES - 1));
    c = &s_w.caps[0];
    c->serial = serial;
    for (s = 1; s < MP_MAX_PEERS; s++) {
        c->op[s] = s_w.guests[s].xfer_op;
        c->any[s] = (u8)s_w.guests[s].xfer_any;
    }
    if (s_w.ncaps < MP_CAPTURES) {
        s_w.ncaps++;
    }
}

static void mp_host_commit(int s, u16 op) {
    mp_guest_w_t* g = &s_w.guests[s];
    int conn = mp_lobby_guest_conn(s);
    u8 msg[3];

    if (g->left || (g->done_any && (s16)(op - g->done_op) <= 0)) {
        return;
    }
    // a guest that might not hear it keeps the pickups restorable until its line is back
    if (conn < 0 || mp_lobby_conn_silence(conn) > MP_DOUBT_MS) {
        if (!g->due_any || (s16)(op - g->due_op) > 0) {
            g->due_op = op;
            g->due_any = TRUE;
        }
        return;
    }
    g->due_any = FALSE;
    g->done_op = op;
    g->done_any = TRUE;
    memmove(&g->sent[1], &g->sent[0], sizeof(g->sent[0]) * 3);
    g->sent[0].op = op;
    g->sent[0].ms = pc_mp_now_ms();
    if (g->nsent < 4) {
        g->nsent++;
    }
    msg[0] = MP_M_COMMIT;
    mp_put16(msg + 1, op);
    mp_send_guest(s, msg, 3);
}

static void mp_pick_forget(mp_guest_w_t* g, u16 upto) {
    int i;

    for (i = 0; i < g->npicks;) {
        if ((s16)(g->picks[i].op - upto) <= 0) {
            memmove(&g->picks[i], &g->picks[i + 1], sizeof(g->picks[0]) * (g->npicks - 1 - i));
            g->npicks--;
        } else {
            i++;
        }
    }
}

// pickups under a commit the guest has surely heard by now are its own for good
static void mp_host_commit_settle(void) {
    u32 now = pc_mp_now_ms();
    int s;

    for (s = 1; s < MP_MAX_PEERS; s++) {
        mp_guest_w_t* g = &s_w.guests[s];
        int conn = mp_lobby_guest_conn(s);
        int k;

        if (g->nsent == 0 || conn < 0 || mp_lobby_conn_silence(conn) > MP_DOUBT_MS || g->hold ||
            (g->out != NULL && g->out->head != g->out->tail)) {
            continue;
        }
        for (k = 0; k < g->nsent; k++) {
            if (now - g->sent[k].ms >= MP_SENT_KEEP_MS) {
                mp_pick_forget(g, g->sent[k].op);
                g->nsent = k;
                break;
            }
        }
    }
}

// saves that finished: a landed one commits what it held, a failed one is forgotten
static void mp_host_commit_landed(void) {
    u32 landed = pc_mc_save_landed();
    u32 done = pc_mc_save_done();
    int k;
    int s;

    for (k = 0; k < s_w.ncaps; k++) {
        if ((s32)(landed - s_w.caps[k].serial) >= 0) {
            for (s = 1; s < MP_MAX_PEERS; s++) {
                if (s_w.caps[k].any[s]) {
                    mp_host_commit(s, s_w.caps[k].op[s]);
                }
            }
            s_w.ncaps = k; // older snapshots are covered by this one
            break;
        }
    }
    while (s_w.ncaps > 0 && (s32)(done - s_w.caps[s_w.ncaps - 1].serial) >= 0) {
        s_w.ncaps--;
    }
}

static int mp_host_commit_wanted(void) {
    int s;

    if (s_w.save_asked) {
        return TRUE;
    }
    for (s = 1; s < MP_MAX_PEERS; s++) {
        mp_guest_w_t* g = &s_w.guests[s];
        int have_any = g->done_any;
        u16 have = g->done_op;

        if (!g->xfer_any) {
            continue;
        }
        if (s_w.ncaps > 0 && s_w.caps[0].any[s]) {
            have_any = TRUE;
            have = s_w.caps[0].op[s];
        }
        if (!have_any || (s16)(g->xfer_op - have) > 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static void mp_host_commit_tick(void) {
    u32 now = pc_mp_now_ms();

    mp_host_commit_landed();
    mp_host_commit_settle();
    if ((now - s_w.last_commit_ms < MP_COMMIT_MS && !s_w.hurry) || !mp_host_commit_wanted()) {
        s_w.hurry = FALSE;
        return;
    }
    if (pc_mc_commit_save_begin() < 0) {
        return; // a save is running, or the player is busy; next frame
    }
    s_w.last_commit_ms = now;
    s_w.hurry = FALSE;
    s_w.save_asked = FALSE;
}

// frame hooks

// the cell the host's player asked to pick up; the pocket takes it next frame
static int mp_host_pending_pickup(GAME_PLAY* play) {
    PLAYER_ACTOR* player = (play != NULL) ? GET_PLAYER_ACTOR(play) : NULL;
    mActor_name_t* fg;

    if (player == NULL || !player->requested_main_index_changed ||
        player->requested_main_index != mPlayer_INDEX_PICKUP) {
        return -1;
    }
    fg = mFI_GetUnitFG(player->requested_main_index_data.pickup.target_pos);
    if (fg == NULL) {
        return -1;
    }
    return mp_cell_of_save_off((u32)((u8*)fg - mp_save_base()));
}

// messages in without a play frame (an NES game on this screen, the app closing): they land in the save, and the
// town's actors catch up once a play frame runs
void mp_world_drain(void) {
    if (s_w.live) {
        mp_world_drain_inbox();
        if (s_w.live) {
            mp_world_flush_queues(MP_REL_RESERVE);
        }
    }
}

// top of the play frame: messages that arrived since the last one
void mp_world_frame(GAME_PLAY* play) {
    if (s_w.live) {
        mp_guest_floor_track();
        s_w.host_pickup_cell = mp_is_host() ? mp_host_pending_pickup(play) : -1;
        mp_locks_take(play);
        mp_world_drain_inbox();
        if (s_w.live) {
            mp_cot_frame();
            mp_shop_apply(play);
            mp_claim_watch();
            if (!mp_is_host() && mp_guest_visiting()) {
                mp_hold_watch();
            }
            // this runs through menus, which stop the frame's second half: acks and commits still go
            if (mp_is_host()) {
                mp_host_commit_landed();
            }
            mp_world_flush_queues(MP_REL_RESERVE);
        }
    }
}

// after the actors: this frame's own changes go out, queues drain
void mp_world_post(GAME_PLAY* play) {
    if (!s_w.live) {
        return;
    }
    if (mp_is_host()) {
        mp_host_diff(play);
        mp_host_gen_scan();
        mp_house_angles_out();
        if (pc_mp_now_ms() - s_w.last_crc_ms >= MP_CRC_MS) {
            s_w.last_crc_ms = pc_mp_now_ms();
            mp_host_send_crcs(-1);
        }
        mp_host_commit_tick();
    } else if (mp_travel_state() == MP_TRAVEL_VISITING) {
        mp_pockets_scan();
        mp_guest_diff(play, FALSE);
        mp_pockets_match();
        mp_guest_release_held();
        mp_guest_shop_scan();
    }
    mp_cot_post();
    mp_world_flush_queues(MP_REL_RESERVE);
}

// visitor behind its menu: what landed in the town meanwhile (a present, fruit it shook down) goes to the host
// at once; its pockets wait for the menu to close
void mp_world_menu_post(GAME_PLAY* play) {
    if (!s_w.live || mp_is_host() || mp_travel_state() != MP_TRAVEL_VISITING) {
        return;
    }
    mp_guest_diff(play, TRUE);
    mp_cot_post();
    mp_world_flush_queues(MP_REL_RESERVE);
}

// host: a new guest stepped off the train: what changed since its snapshot goes to it, then the checksums
void mp_world_on_arrived(int conn) {
    int slot = mp_lobby_guest_slot(conn) + 1;

    if (slot <= 0) {
        return;
    }
    mp_world_host_begin();
    if (!s_w.live) {
        return;
    }
    memset(&s_w.guests[slot].zones, 0, sizeof(s_w.guests[slot].zones));
    mp_rights_slot_fresh(slot);
    mp_host_catch_up(slot);
    mp_host_send_crcs(slot);
    memset(s_cot.units[slot], 0, MP_COT_UNIT_BYTES);
    s_cot.sent_ok[slot] = FALSE;
    s_cot.room_in[slot] = FALSE;
    mp_cot_angles_to(slot);
}

// host: a quiet guest is back: what waited for it goes now, and the checksums find anything its queue dropped
void mp_world_on_back(int conn) {
    int slot = mp_lobby_guest_slot(conn) + 1;

    if (slot > 0 && s_w.live) {
        mp_host_send_crcs(slot);
        s_cot.sent_ok[slot] = FALSE;
        if (s_w.guests[slot].due_any) {
            mp_host_commit(slot, s_w.guests[slot].due_op);
        }
    }
}

// host: pickups no save holds go back where they were, or to the lost & found (the cottage's always: a room of it up
// would write over them, and a piece of furniture is more than its one cell)
static void mp_host_restore_picks(mp_guest_w_t* g) {
    int i;

    for (i = 0; i < g->npicks; i++) {
        int c = g->picks[i].cell;

        mp_hnw_bells(c, -(s32)g->picks[i].price);
        // (a shining spot or a buried pitfall comes back over the hole it left)
        if (c < MP_COT_BASE && !MP_BIT_GET(s_w.held, c) &&
            ((s_w.view[c] == EMPTY_NO && mp_raw_get(c) == EMPTY_NO) ||
             ((g->picks[i].item == SHINE_SPOT || ITEM_IS_BURIED_PITFALL_HOLE(g->picks[i].item)) &&
              ITEM_IS_HOLE(s_w.view[c])))) {
            mp_raw_set(c, g->picks[i].item);
        } else if (g->picks[i].pocket != SHINE_SPOT && !mp_host_keep(g->picks[i].pocket, FALSE)) {
            // (a full lost & found: beside where it was, rather than pushing out its oldest)
            int to = c < MP_COT_BASE && mp_rg_of(c) <= MP_RG_IFG ? mp_host_find_spot(c, NULL, 0) : -1;

            if (to >= 0) {
                mp_raw_set(to, g->picks[i].pocket);
            } else {
                mp_host_keep(g->picks[i].pocket, TRUE);
            }
        }
    }
    g->npicks = 0;
    mp_refresh_field();
}

// host: a guest boarding home named the newest commit it heard; it kept pickups up to there
static void mp_host_on_leaving(mp_guest_w_t* g, int slot, const u8* p, int len) {
    int k;

    if (g->left) {
        return;
    }
    if (len >= 4 && p[1]) {
        mp_pick_forget(g, (u16)mp_get16(p + 2));
    }
    mp_host_restore_picks(g);
    g->left = TRUE;
    g->xfer_any = FALSE;
    g->due_any = FALSE;
    for (k = 0; k < MP_CAPTURES; k++) {
        s_w.caps[k].any[slot] = FALSE;
    }
}

// what a gone guest sent that the world hasn't taken yet goes with it (a later guest may get its line)
static void mp_world_purge_conn(int conn) {
    int rd = 0;
    int wr = 0;

    if (conn < 0) {
        return;
    }
    while (rd < s_w.inbox_len) {
        int n = s_w.inbox[rd + 1] | (s_w.inbox[rd + 2] << 8);

        if (s_w.inbox[rd] != (u8)conn) {
            memmove(s_w.inbox + wr, s_w.inbox + rd, n + 3);
            wr += n + 3;
        }
        rd += n + 3;
    }
    s_w.inbox_len = wr;
}

// host: a gone guest's messages the town hadn't taken yet, taken now in order (its link acknowledged them)
static void mp_world_take_conn(int conn) {
    int at = 0;

    while (at < s_w.inbox_len && s_w.live) {
        int from = s_w.inbox[at];
        int len = s_w.inbox[at + 1] | (s_w.inbox[at + 2] << 8);

        if (from == conn) {
            u8 msg[MP_MSG_MAX];

            memcpy(msg, s_w.inbox + at + 3, len < MP_MSG_MAX ? len : MP_MSG_MAX);
            memmove(s_w.inbox + at, s_w.inbox + at + 3 + len, s_w.inbox_len - (at + 3 + len));
            s_w.inbox_len -= 3 + len;
            mp_world_dispatch(conn, msg, len < MP_MSG_MAX ? len : MP_MSG_MAX);
        } else {
            at += 3 + len;
        }
    }
}

// the play game ended: the host's pickup and its way back out go with it
void mp_world_play_gone(void) {
    s_w.host_pickup_cell = -1;
    s_w.fg_back_ms = 0;
}

void mp_world_on_guest_gone(int slot, int conn) {
    int k;

    if (slot <= 0 || slot >= MP_MAX_PEERS) {
        return;
    }
    s_snap_crc[slot].have = FALSE;
    if (mp_is_host()) {
        mp_world_take_conn(conn);
        mp_world_purge_conn(conn);
    }
    memset(s_cot.units[slot], 0, MP_COT_UNIT_BYTES);
    s_cot.sent_ok[slot] = FALSE;
    s_cot.room_in[slot] = FALSE;
    if (s_w.live && mp_is_host()) {
        // vanished: pickups under a commit it was sent may be in its passport (one standing still has only those it
        // said); the rest come back
        if (s_w.guests[slot].hold) {
            if (s_w.guests[slot].hold_any) {
                mp_pick_forget(&s_w.guests[slot], s_w.guests[slot].hold_mark);
            }
        } else if (s_w.guests[slot].done_any) {
            mp_pick_forget(&s_w.guests[slot], s_w.guests[slot].done_op);
        }
        mp_host_restore_picks(&s_w.guests[slot]);
    }
    // a later guest in this slot starts its op ids over
    for (k = 0; k < MP_CAPTURES; k++) {
        s_w.caps[k].any[slot] = FALSE;
    }
    mp_free(s_w.guests[slot].out);
    memset(&s_w.guests[slot], 0, sizeof(s_w.guests[slot]));
}

// before hanging up (or a visitor heading home), whatever is still queued goes to the link; the host's with what a
// landed save already made final
void mp_world_flush_out(void) {
    if (!s_w.live) {
        return;
    }
    if (mp_is_host()) {
        mp_host_commit_landed();
    }
    mp_world_flush_queues(-1);
}

// visitor: a stand's design traded, or the flag set; host: a stand of its own changed
void mp_world_design(int target) {
    u8 msg[4 + MP_DESIGN_HALF];
    const u8* d;
    int h;

    if (target < 0 || target > MP_DESIGN_FLAG || !s_w.live) {
        return;
    }
    if (mp_is_host()) {
        if (target < mNW_TOTAL_DESIGN_NUM) {
            mp_npc_stand_replaced(target);
        }
        return;
    }
    if (mp_travel_state() != MP_TRAVEL_VISITING) {
        return;
    }
    d = (const u8*)mp_design_of(target);
    for (h = 0; h < 2; h++) {
        msg[0] = MP_M_SOFT;
        msg[1] = MP_SOFT_DESIGN;
        msg[2] = (u8)target;
        msg[3] = (u8)h;
        memcpy(msg + 4, d + h * MP_DESIGN_HALF, MP_DESIGN_HALF);
        mp_queue_push(s_w.host_out, msg, (int)sizeof(msg));
    }
    mp_world_heard_mine((u32)(d - mp_save_base()), sizeof(mNW_original_design_c));
}

// m_notice.c: a visitor posted on the board; it goes up on the host's too
void mp_world_board_post(const void* post, int len) {
    u8 msg[2 + sizeof(mNtc_board_post_c)];

    if (!s_w.live || mp_is_host() || mp_travel_state() != MP_TRAVEL_VISITING || len != (int)sizeof(mNtc_board_post_c)) {
        return;
    }
    msg[0] = MP_M_SOFT;
    msg[1] = MP_SOFT_BOARD;
    memcpy(msg + 2, post, sizeof(mNtc_board_post_c));
    mp_queue_push(s_w.host_out, msg, (int)sizeof(msg));
}

// visitor: it lit the lighthouse; its own save has the night already, the host's gets it too
void mp_world_lighthouse(int day) {
    u8 msg[3];

    if (!s_w.live || mp_is_host() || mp_travel_state() != MP_TRAVEL_VISITING || day < 0 || day >= 8) {
        return;
    }
    mp_world_heard_mine(offsetof(Save_t, LightHouse), sizeof(LightHouse_c));
    msg[0] = MP_M_SOFT;
    msg[1] = MP_SOFT_LIGHTHOUSE;
    msg[2] = (u8)day;
    mp_queue_push(s_w.host_out, msg, (int)sizeof(msg));
}

void mp_world_hurry(void) {
    u8 msg[1] = { MP_M_HURRY };

    if (s_w.live) {
        mp_queue_push(s_w.host_out, msg, 1);
    }
}

static void mp_town_keep_send(int kind, int bx, int bz, int ux, int uz) {
    u8 msg[7];

    msg[0] = MP_M_SOFT;
    msg[1] = MP_SOFT_TOWNKEEP;
    msg[2] = (u8)kind;
    msg[3] = (u8)bx;
    msg[4] = (u8)bz;
    msg[5] = (u8)ux;
    msg[6] = (u8)uz;
    mp_queue_push(s_w.host_out, msg, sizeof(msg));
}

static int mp_guest_visiting(void) {
    return s_w.live && !mp_is_host() && mp_travel_state() == MP_TRAVEL_VISITING;
}

// guest: what the game would move out of the way to the lost & found (an event taking its acre, a building's doorstep,
// a spot someone comes out on) is the host's to move; the visitor's own writes there would only lose it
int mp_town_keep_at(const void* wpos, int kind) {
    const xyz_t* p = (const xyz_t*)wpos;
    mActor_name_t* fg;
    int bx;
    int bz;
    int ux;
    int uz;

    if (!mp_guest_visiting()) {
        return FALSE;
    }
    if (kind != MP_KEEP_NONE && mFI_Wpos2BlockNum(&bx, &bz, *p) && mFI_Wpos2UtNum_inBlock(&ux, &uz, *p) &&
        (fg = mFI_GetUnitFG(*p)) != NULL && *fg != EMPTY_NO && *fg != RSV_NO) {
        mp_town_keep_send(kind, bx, bz, ux, uz);
    }
    return TRUE;
}

int mp_town_keep_block(int bx, int bz) {
    if (!mp_guest_visiting()) {
        return FALSE;
    }
    mp_town_keep_send(MP_KEEP_BLOCK, bx, bz, 0, 0);
    return TRUE;
}

// guest: nothing in flight and every pickup saved by the host
int mp_world_settled(void) {
    int i;

    if (!s_w.live) {
        return TRUE;
    }
    for (i = 0; i < MP_OPS; i++) {
        if (s_w.ops[i].state == MP_OPS_SENT || s_w.ops[i].state == MP_OPS_HELD) {
            return FALSE;
        }
    }
    for (i = 0; i < MP_XFERS; i++) {
        if (s_w.xfers[i].used && s_w.xfers[i].kind == MP_XFER_PICKUP && s_w.xfers[i].in_pocket &&
            !s_w.xfers[i].committed) {
            return FALSE;
        }
    }
    return TRUE;
}

// town writers
// the host owns the town: a visitor's game doesn't grow, clear or bury anything in it

int mp_town_writer_allowed(void) {
    return mp_travel_state() != MP_TRAVEL_VISITING;
}

// the acre of a spot outdoors; mFI_Wpos2BlockNum checks the loaded scene's bounds, a room's indoors
int mp_town_block(const void* pos, int* bx, int* bz) {
    const xyz_t* p = (const xyz_t*)pos;

    *bx = (int)(p->x / mFI_BK_WORLDSIZE_X_F);
    *bz = (int)(p->z / mFI_BK_WORLDSIZE_Z_F);
    return p->x >= 0.0f && p->z >= 0.0f && *bx < 32 && *bz < 32;
}

// the acre a player is walking in
static int mp_player_block(int s, int* bx, int* bz) {
    unsigned int age;
    const mp_pstate_t* st = mp_player_state(s, &age);
    xyz_t pos;

    if (st == NULL || age > 12000 || st->scene != SCENE_FG) {
        return FALSE;
    }
    pos.x = st->x;
    pos.y = st->y;
    pos.z = st->z;
    return mp_town_block(&pos, bx, bz);
}

// host: holes near a visitor stay put until nobody is looking
int mp_host_block_watched(int bx, int bz) {
    int s;
    int gbx;
    int gbz;

    if (!mp_is_host()) {
        return FALSE;
    }
    for (s = 1; s < MP_MAX_PEERS; s++) {
        if (mp_player_block(s, &gbx, &gbz) && gbx - bx >= -1 && gbx - bx <= 1 && gbz - bz >= -1 &&
            gbz - bz <= 1) {
            return TRUE;
        }
    }
    return FALSE;
}

// the screen stepping the villagers nobody watches keeps them out of every other player's acre, as out of its own
int mp_block_occupied(int bx, int bz) {
    int self = mp_lobby_self_slot();
    int s;
    int gbx;
    int gbz;

    if (!mp_active()) {
        return FALSE;
    }
    for (s = 0; s < MP_MAX_PEERS; s++) {
        if (s != self && mp_player_block(s, &gbx, &gbz) && gbx == bx && gbz == bz) {
            return TRUE;
        }
    }
    return FALSE;
}

#endif
