#include "ac_my_room.h"

#include "m_bgm.h"
#include "m_name_table.h"
#include "ac_furniture.h"
#include "f_furniture.h"
#include "m_common_data.h"
#include "libultra/libultra.h"
#include "m_malloc.h"
#include "m_cockroach.h"
#include "m_msg.h"
#include "dolphin/card.h"
#include "m_string.h"
#include "m_house.h"
#include "m_player_lib.h"
#include "GBA2/gba2.h"
#include "m_debug.h"
#include "m_mark_room.h"
#include "sys_matrix.h"
#include "m_rcp.h"
#ifdef VITA_MP
#include "pc_mp.h"
#ifdef VITA_MP
#include "m_demo.h"
#include "m_museum_display.h"
#include "m_submenu.h"
#include "sys_math3d.h"
#endif
#endif

enum {
    aMR_ICON_LEAF,
    aMR_ICON_HANIWA,
    aMR_ICON_CLOTH,
    aMR_ICON_UMBRELLA,
    aMR_ICON_BONE,
    aMR_ICON_DIARY,

    aMR_ICON_NUM
};

/* Inclusive bounds */
#define aMR_MIN_BOUND 1
#define aMR_MAX_BOUND 8

#define aMR_FTR_BANK_NUM 100
#define aMR_FTR_BANK_SIZE 0x800

#define aMR_BOUNDS_OK(x, z) \
    ((x) > (aMR_MIN_BOUND - 1) && (x) < (aMR_MAX_BOUND + 1) && (z) > (aMR_MIN_BOUND - 1) && (z) < (aMR_MAX_BOUND + 1))

enum {
    aMR_MSG_STATE_NONE,
    aMR_MSG_STATE_WAIT_MSG,
    aMR_MSG_STATE_OWNER_NO_ITEM,
    aMR_MSG_STATE_OWNER_EXIST_ITEM,
    aMR_MSG_STATE_OWNER_EXIST_ITEM_ITEM_COUNT2,
    aMR_MSG_STATE_OWNER_WHICH_PUT_OUT1,
    aMR_MSG_STATE_OWNER_WHICH_PUT_OUT2,
    aMR_MSG_STATE_OWNER_WHICH_PUT_OUT3,
    aMR_MSG_STATE_OWNER_WAIT_WHICH_PUT_OUT,
    aMR_MSG_STATE_OTHER_NO_ITEM,
    aMR_MSG_STATE_OTHER_EXIST_ITEM1,
    aMR_MSG_STATE_OTHER_EXIST_ITEM2,
    aMR_MSG_STATE_OTHER_EXIST_ITEM3,
    aMR_MSG_STATE_FULL_PLAYER_ITEM,
    aMR_MSG_STATE_OPEN_SUBMENU,
    aMR_MSG_STATE_ITEM_PUT_IN,
    aMR_MSG_STATE_WAIT_CLOSE_FTR,
    aMR_MSG_STATE_REQUEST_CLOSE_FTR,
    aMR_MSG_STATE_REQUEST_FULL_BAG,
    aMR_MSG_STATE_CAN_NOT_CLEAN,
    aMR_MSG_STATE_CAN_NOT_CLEAN_MD,
    aMR_MSG_STATE_OPEN_SET_ITEM_SUBMENU,
    aMR_MSG_STATE_ITEM_SET_SUBMENU,
    aMR_MSG_STATE_WAIT_MD,
    aMR_MSG_STATE_OWNER_NO_MD,
    aMR_MSG_STATE_OWNER_EXIST_MD,
    aMR_MSG_STATE_OWNER_EXIST_MD2,
    aMR_MSG_STATE_OTHER_NO_MD,
    aMR_MSG_STATE_OTHER_EXIST_MD,
    aMR_MSG_STATE_FULL_PLAYER_MD,
    aMR_MSG_STATE_OPENMD_SUBMENU,
    aMR_MSG_STATE_MD_PUT_IN,
    aMR_MSG_STATE_OPEN_MUSIC_BOX,
    aMR_MSG_STATE_MUSIC_BOX_MD_PUT_IN,
    aMR_MSG_STATE_WAIT_CLOSE_MD,
    aMR_MSG_STATE_REQUEST_CLOSE_MD,
    aMR_MSG_STATE_REQUEST_FULL_BAG_MD,
    aMR_MSG_STATE_CAN_NOT_MD_CLEAN,
    aMR_MSG_STATE_OPEN_EXCHANGE_MD_SUBMENU,
    aMR_MSG_STATE_MD_EXCHANGE_SUBMENU,
    aMR_MSG_STATE_QQQ_EMULATOR,
    aMR_MSG_STATE_EXPLAIN_EMULATOR,
    aMR_MSG_STATE_QQQ_EMULATOR_MEMORY1,
    aMR_MSG_STATE_EXPLAIN_EMULATOR_MEMORY,
    aMR_MSG_STATE_QQQ_EMULATOR_MEMORY2,
    aMR_MSG_STATE_QQQ_EMULATOR_MEMORY_OVER3,
    aMR_MSG_STATE_QQQ_REPEAT_DISPLAY1,
    aMR_MSG_STATE_QQQ_REPEAT_DISPLAY1_2,
    aMR_MSG_STATE_QQQ_REPEAT_DISPLAY2,
    aMR_MSG_STATE_QQQ_REPEAT_DISPLAY2_2,
    aMR_MSG_STATE_NO_PACK_NO_DATA,
    aMR_MSG_STATE_WARNING_CANNOT_MAKE_SAVE_FILE,
    aMR_MSG_STATE_NO_PACK_NO_DATA1,
    aMR_MSG_STATE_NO_PACK_NO_DATA2,
    aMR_MSG_STATE_HITOKOTO,
    aMR_MSG_STATE_HITOKOTO1,
    aMR_MSG_STATE_HITOKOTO2,
    aMR_MSG_STATE_SAVE_FAMICOM,
    aMR_MSG_STATE_WAIT_FAMICOM_BATU,
    aMR_MSG_STATE_PREPARE_COMMUNICATION,
    aMR_MSG_STATE_NOT_CONNECT_AGB,
    aMR_MSG_STATE_CHECK_AGB_PROGRAM,
    aMR_MSG_STATE_DELETE_AGB_PROGRAMQ,
    aMR_MSG_STATE_START_EMU_DOWN_LOAD,
    aMR_MSG_STATE_RECHECK_AGB_CONNECT,

    aMR_MSG_STATE_NUM
};

static void My_Room_Actor_ct(ACTOR*, GAME*);
static void My_Room_Actor_dt(ACTOR*, GAME*);
static void My_Room_Actor_move(ACTOR*, GAME*);
static void My_Room_Actor_draw(ACTOR*, GAME*);

// clang-format off
ACTOR_PROFILE My_Room_Profile = {
    mAc_PROFILE_MY_ROOM,
    ACTOR_PART_BG,
    ACTOR_STATE_CAN_MOVE_IN_DEMO_SCENES | ACTOR_STATE_NO_DRAW_WHILE_CULLED | ACTOR_STATE_NO_MOVE_WHILE_CULLED,
    ETC_MY_ROOM,
    ACTOR_OBJ_BANK_KEEP,
    sizeof(MY_ROOM_ACTOR),
    &My_Room_Actor_ct,
    &My_Room_Actor_dt,
    &My_Room_Actor_move,
    &My_Room_Actor_draw,
    NULL
};
// clang-format on

#include "../src/actor/ac_furniture_data.c_inc"
#include "../src/actor/ac_my_room_data.c_inc"

typedef struct my_room_work_s {
    FTR_ACTOR* ftr_actor_list;
    u8* used_list;
    int list_size;
} aMR_work_c;

static aMR_work_c l_aMR_work;

#ifdef VITA_MP
// another player's hand on the furniture in this room: storage it opened, moves glided into place
#define aMR_MP_FTR_MAX 256

static u8 aMR_mp_remote_demo[aMR_MP_FTR_MAX];
static u8 aMR_mp_rsv_remote[16]; // a reserved birth another screen asked for
static s16 aMR_mp_glide_left[aMR_MP_FTR_MAX];
static xyz_t aMR_mp_glide_to[aMR_MP_FTR_MAX];
static int aMR_mp_replaying;
static u8 aMR_mp_fossil_bit[mMmd_FOSSIL_BIT_NUM]; // the fossil record this room's exhibits were laid out from
// the island's cottage shared in a session: the room follows its cells, logic at once and drawing easing in
static u16 aMR_mp_shadow[mCoBG_LAYER_NUM * UT_TOTAL_NUM]; // the cells as the room last followed them
static int aMR_mp_retry;                 // something had to wait: look again shortly
static int aMR_mp_retry_wait;
static u8 aMR_mp_quiet[aMR_MP_FTR_MAX];  // coming or going by another player's hand: nothing of this player's
static xyz_t aMR_mp_vis[aMR_MP_FTR_MAX]; // drawn this far from where it now stands, easing in
static s16 aMR_mp_vis_rot[aMR_MP_FTR_MAX];
static xyz_t aMR_mp_hint[aMR_MP_FTR_MAX];    // another player's push or turn under way: the drawing leads there
static s16 aMR_mp_hint_rot[aMR_MP_FTR_MAX];
static s16 aMR_mp_hint_left[aMR_MP_FTR_MAX]; // frames of that glide to go
static s16 aMR_mp_hint_hold[aMR_MP_FTR_MAX]; // ...then how long it waits there for the cells
static u8 aMR_mp_demo_idle[aMR_MP_FTR_MAX];  // another player's storage open with nobody's hands on it: frames
static u8 aMR_mp_session[UT_TOTAL_NUM / 8];  // every unit the furniture in the player's hands has stood on since
static int aMR_mp_session_on;                // ...while it's in hand, or let go and still on the move
static int aMR_mp_session_id;                // ...the piece in hand, -1 none
static int aMR_mp_sounds;                    // sounds a pass of the room's catching up has made
#define aMR_MP_HANIWA_WAIT 8
// gyroids another screen switched on while this list was full
static int aMR_mp_haniwa_wait[aMR_MP_HANIWA_WAIT] = { -1, -1, -1, -1, -1, -1, -1, -1 };
static int aMR_mp_nosave;                    // the cells already hold what the room is building

static int aMR_PosType2FurniturePoccessUnitNo(int* ut_info, const xyz_t* pos, u8 type);
#ifdef TARGET_VITA
static int s_vita_nes_picker_ftrID;
#endif
static mActor_name_t aMR_GetSaveAngle(f32 angle, mActor_name_t item);
static void aMR_mp_follow_start(void);
static int aMR_mp_house_calm; // frames since the other screen last changed something in this house room
static void aMR_mp_hint_move(FTR_ACTOR* ftr_actor, const u8* body);
static void aMR_mp_hint_turn(FTR_ACTOR* ftr_actor, const u8* body);
static int aMR_mp_sound_ok(void);

// the island's cottage in a session: its rooms follow the cells, not each other's hands
static int aMR_mp_cot_shared(void) {
    return aMR_CLIP != NULL && aMR_CLIP->my_room_actor_p != NULL &&
           ((MY_ROOM_ACTOR*)aMR_CLIP->my_room_actor_p)->scene == SCENE_COTTAGE_MY && mp_cot_shared();
}

static int aMR_mp_ut_in_use(int ut) {
    return aMR_mp_cot_shared() && mp_cot_in_use(ut);
}

// a house's room in a session, where only its owner moves things
static int aMR_mp_house(void) {
    return mFI_GET_TYPE(mFI_GetFieldId()) == mFI_FIELD_PLAYER0_ROOM && mp_active();
}

// another player has this piece in hand (the cottage), or sits or lies on it (a house)
static int aMR_mp_in_use(FTR_ACTOR* ftr_actor) {
    int cot = aMR_mp_cot_shared();
    int ut[4];
    int n;
    int i;

    if (!cot && !aMR_mp_house()) {
        return FALSE;
    }
    n = aMR_PosType2FurniturePoccessUnitNo(ut, &ftr_actor->position, ftr_actor->shape_type);
    for (i = 0; i < n; i++) {
        if (cot ? mp_cot_in_use(ut[i]) : mp_player_on_unit(ut[i])) {
            return TRUE;
        }
    }
    return FALSE;
}

// a unit furniture can't go to: another player has it in hand, or stands there
static int aMR_mp_ut_blocked(int ut) {
    if (aMR_mp_cot_shared()) {
        return mp_cot_in_use(ut) || mp_cot_unit_taken(ut);
    }
    return aMR_mp_house() && mp_player_on_unit(ut);
}

static int aMR_mp_remote(FTR_ACTOR* ftr_actor) {
    return ftr_actor->id >= 0 && ftr_actor->id < aMR_MP_FTR_MAX && aMR_mp_remote_demo[ftr_actor->id];
}

static void aMR_mp_put(u8* p, const void* v, int n) {
    const u8* s = (const u8*)v;
    int i;

    for (i = 0; i < n; i++) {
        p[i] = s[i];
    }
}

static void aMR_mp_get(void* v, const u8* p, int n) {
    u8* d = (u8*)v;
    int i;

    for (i = 0; i < n; i++) {
        d[i] = p[i];
    }
}

// the local player did something to this furniture: the other screens in the room do it too
static void aMR_mp_send(int kind, FTR_ACTOR* ftr_actor, const void* extra, int extra_len) {
    u8 body[48];
    s16 id = (s16)ftr_actor->id;

    // (a shared cottage's rooms take furniture's comings and goings from its cells; a move only leads the drawing)
    if (aMR_mp_replaying || extra_len > 34 ||
        ((kind == MP_VFX_FTR_BIRTH || kind == MP_VFX_FTR_BYE) && aMR_mp_cot_shared())) {
        return;
    }
    body[0] = (u8)kind;
    aMR_mp_put(body + 1, &id, 2);
    aMR_mp_put(body + 3, &ftr_actor->name, 2);
    aMR_mp_put(body + 5, &ftr_actor->position.x, 4);
    aMR_mp_put(body + 9, &ftr_actor->position.z, 4);
    if (extra_len > 0) {
        aMR_mp_put(body + 13, extra, extra_len);
    }
    mp_vfx_send(body, 13 + extra_len);
}

// how a stereo's music changed: a record put in or swapped, a switch by hand, or the music box taking its record
enum { aMR_MP_REC_PUT, aMR_MP_REC_HAND, aMR_MP_REC_GONE };

static void aMR_mp_send_record(FTR_ACTOR* ftr_actor, int how) {
    u8 extra[4];

    extra[0] = (u8)(ftr_actor->switch_bit == TRUE);
    extra[1] = (u8)how;
    aMR_mp_put(extra + 2, &ftr_actor->items[0], 2);
    aMR_mp_send(MP_VFX_FTR_RECORD, ftr_actor, extra, 4);
}
#endif
#ifndef VITA_MP
#define aMR_mp_in_use(ftr_actor) FALSE
#define aMR_mp_ut_in_use(ut) FALSE
#define aMR_mp_ut_blocked(ut) FALSE
#endif
static u8 l_bank_index_table[FTR_NUM];
static u8* l_bank_address_table[aMR_FTR_BANK_NUM];

// clang-format off
const aMR_contact_info_c l_cntInf_default = {
    FALSE,
    100,
    0,
    0,
    0.0f,
    NULL,
    { 0.0f, 0.0f },
    { 0.0f, 0.0f },
    0
};
// clang-format on
const f32 aMR_angle_table[4] = { 0.0f, 90.0f, 180.0f, 270.0f };
const u8 l_typeB0_table[4] = { aFTR_SHAPE_TYPEB_0, aFTR_SHAPE_TYPEB_90, aFTR_SHAPE_TYPEB_180, aFTR_SHAPE_TYPEB_270 };

static int aMR_UnitNum2FtrItemNoFtrID(mActor_name_t* item_no, int* ftr_id, int ut_x, int ut_z, int layer);
static void aMR_ReserveDefaultBgm(ACTOR* actorx, FTR_ACTOR* ftr_actor);
static void aMR_ChangeMDBgm(ACTOR* actorx, FTR_ACTOR* ftr_actor);
static int aMR_SetFurniture2FG(FTR_ACTOR* ftr_actor, xyz_t pos, int on_flag);
static int aMR_FtrIdx2ChangeFtrSwitch(ACTOR* actorx, int ftr_id);
static aFTR_PROFILE* aMR_GetFurnitureProfile(u16 ftr_no);
static mActor_name_t* aMR_GetLayerTopFg(s16 layer);
static void aMR_TidyItemInFurniture(FTR_ACTOR* ftr_actor);
static int aMR_GetItemCountInFurniture(FTR_ACTOR* ftr_actor);
static int aMR_ItemPutInFurniture(FTR_ACTOR* ftr_actor, mActor_name_t item);
static void aMR_AllMDSwitchOff(void);
static void aMR_OneMDSwitchOn_TheOtherSwitchOff(FTR_ACTOR* ftr_actor);
static void aMR_ReserveBgm(ACTOR* actorx, int bgm_no, FTR_ACTOR* ftr_actor, s16 timer);
static void aMR_SetMDFtrDemoData(MY_ROOM_ACTOR* my_room, FTR_ACTOR* ftr_actor, aMR_contact_info_c* contact_info);
static int aMR_CheckHikidashi(FTR_ACTOR* ftr_actor, PLAYER_ACTOR* player, aMR_contact_info_c* contact_info);
static int aMR_JudgeDemoStart(MY_ROOM_ACTOR* my_room, aMR_contact_info_c* contact_info, GAME* game,
                              PLAYER_ACTOR* player);
static void aMR_PlacePushFurniture(MY_ROOM_ACTOR* my_room, aMR_contact_info_c* contact_info, f32* point,
                                   PLAYER_ACTOR* player, GAME* game);
static void aMR_PlacePullFurniture(MY_ROOM_ACTOR* my_room, aMR_contact_info_c* contact_info, PLAYER_ACTOR* player,
                                   GAME* game);
static void aMR_PlaceKurukuruFurniture(MY_ROOM_ACTOR* my_room, aMR_contact_info_c* contact_info, PLAYER_ACTOR* player,
                                       GAME* game);
static void aMR_SitDownFurniture(MY_ROOM_ACTOR* my_room, aMR_contact_info_c* contact_info, PLAYER_ACTOR* player,
                                 GAME* game);
static void aMR_JudgeGoToBed(MY_ROOM_ACTOR* my_room, aMR_contact_info_c* contact_info, PLAYER_ACTOR* player,
                             GAME* game);
static void aMR_SetMelodyData(u8* melody_data);
static void aMR_GokiInfoCt(ACTOR* actorx, GAME* game);
static void aMR_GokiInfoDt(void);
static void aMR_CheckFtrAndGoki(ACTOR* actorx, FTR_ACTOR* ftr_actor, GAME* game);
static void aMR_MakeGokiburi(xyz_t* pos, GAME* game, s16 arg);
static void aMR_RequestPlayerBikkuri(ACTOR* actorx, GAME* game);
static void aMR_GetFtrShape4Position(xyz_t* p0, xyz_t* p1, xyz_t* p2, xyz_t* p3, FTR_ACTOR* ftr_actor);
static u8 aMR_JudgeStickFull(int direct, GAME* game);
static int aMR_JudgeFurnitureMove(u8 type, int ut);
static int aMR_RequestItemToFitFurniture(ACTOR* actorx, FTR_ACTOR* ftr_actor);
static void aMR_SetPullMoveAnime(FTR_ACTOR* ftr_actor, GAME* game, MY_ROOM_ACTOR* my_room,
                                 aMR_contact_info_c* contact_info);
static int aMR_PullDirect2PushDirect(int pull_direct);
static void aMR_SetPushMoveAnime(FTR_ACTOR* ftr_actor, GAME* game, MY_ROOM_ACTOR* my_room,
                                 aMR_contact_info_c* contact_info);
static void aMR_MoveShapeCenter(FTR_ACTOR* ftr_actor);
static void aMR_RotateY(f32* xz, f32 amount);
static int aMR_3DStickNuetral(void);
static void aMR_SetNicePos(xyz_t* nice_pos, xyz_t player_pos, f32* col_start_xz, f32* col_end_xz,
                           aMR_contact_info_c* contact_info, int type);
static int aMR_GetPlayerDirect(const f32* normal_xz);
static int aMR_Get3dDirectStatus(int direct);

static aFTR_PROFILE* aMR_GetFurnitureProfile(u16 ftr_no) {
    if (ftr_no < FTR_NUM) {
        if (furniture_quality[ftr_no] != NULL) {
            return furniture_quality[ftr_no];
        }
    } else {
        return &iam_dummy;
    }

    return &iam_dummy;
}

static int aMR_GetItemCountInFurniture(FTR_ACTOR* ftr_actor) {
    int count = 0;
    int i;

    for (i = 0; i < aFTR_KEEP_ITEM_COUNT; i++) {
        if (ftr_actor->items[i] != EMPTY_NO) {
            count++;
        }
    }

    return count;
}

static void aMR_TidyItemInFurniture(FTR_ACTOR* ftr_actor) {
    mActor_name_t temp_items[aFTR_KEEP_ITEM_COUNT];
    mActor_name_t* dst_p;
    int i;

    for (i = 0; i < aFTR_KEEP_ITEM_COUNT; i++) {
        temp_items[i] = ftr_actor->items[i];
        ftr_actor->items[i] = EMPTY_NO;
    }

    dst_p = ftr_actor->items;
    for (i = 0; i < aFTR_KEEP_ITEM_COUNT; i++) {
        if (temp_items[i] != EMPTY_NO) {
            *dst_p++ = temp_items[i];
        }
    }
}

static int aMR_ItemPutInFurniture(FTR_ACTOR* ftr_actor, mActor_name_t item) {
    int i;

    for (i = 0; i < aFTR_KEEP_ITEM_COUNT; i++) {
        if (ftr_actor->items[i] == EMPTY_NO) {
            ftr_actor->items[i] = item;
            return TRUE;
        }
    }

    return FALSE;
}

static int aMR_ItemNo2IconNo(mActor_name_t item_no) {
    if (ITEM_IS_FTR(item_no)) {
        if (item_no >= FTR_START(FTR_NOG_COLLEGENOTE) && item_no <= FTR_END(FTR_IKE_NIKKI_WAFU1)) {
            return aMR_ICON_DIARY;
        }

        if (item_no >= HANIWA_START && item_no <= HANIWA_END) {
            return aMR_ICON_HANIWA;
        }

        if ((item_no >= FTR_UMBRELLA_START && item_no <= FTR_UMBRELLA_END) ||
            (item_no >= FTR_UMBRELLA_START && item_no <= FTR_MYUMBRELLA_END)) {
            return aMR_ICON_UMBRELLA;
        }

        if ((item_no >= FTR_CLOTH_START && item_no <= FTR_CLOTH_END) ||
            (item_no >= FTR_CLOTH_START && item_no <= FTR_CLOTH_MYMANNIQUIN_END)) {
            return aMR_ICON_CLOTH;
        }

        if (item_no >= FTR_DINO_START && item_no <= FTR_DINO_END) {
            return aMR_ICON_BONE;
        }

        return aMR_ICON_LEAF;
    }

    return aMR_ICON_LEAF;
}

extern Gfx* aMR_IconNo2Gfx1(int icon_no) {
    aMR_icon_display_data_c* icon;

    if (icon_no < 0) {
        icon_no = 0;
    } else if (icon_no >= aMR_ICON_NUM) {
        icon_no = aMR_ICON_NUM - 1;
    }

    return aMR_icon_display_data[icon_no].mat_gfx;
}

extern Gfx* aMR_IconNo2Gfx2(int icon_no) {
    if (icon_no < 0) {
        icon_no = 0;
    } else if (icon_no >= aMR_ICON_NUM) {
        icon_no = aMR_ICON_NUM - 1;
    }

    return aMR_icon_display_data[icon_no].vtx_gfx;
}

static mActor_name_t* aMR_GetLayerTopFg(s16 layer) {
    return mFI_BkNum2UtFGTop_layer(0, 0, layer);
}

static u64* aMR_GetBitSwitchTable(int layer, MY_ROOM_ACTOR* my_room) {
    mActor_name_t field_id = mFI_GetFieldId();

    if (my_room->scene == SCENE_COTTAGE_MY) {
        return &(&Save_Get(island).cottage.room.layer_main)[layer].ftr_switch;
    }

    if ((mFI_GET_TYPE(field_id) == mFI_FIELD_PLAYER0_ROOM)) {
        int idx = (mFI_GetFieldId() - mFI_FIELD_PLAYER0_ROOM) & 3;

        if (aMR_CLIP != NULL && aMR_CLIP->my_room_actor_p != NULL) {
            int floor_no = mFI_GetPlayerHouseFloorNo(((MY_ROOM_ACTOR*)aMR_CLIP->my_room_actor_p)->scene);

            if (floor_no != -1) {
                return &(&Save_Get(homes[idx]).floors[floor_no].layer_main)[layer].ftr_switch;
            }
        }
    }

    return NULL;
}

static u32* aMR_GetHaniwaStepSaveData(s16 layer, MY_ROOM_ACTOR* my_room) {
    mActor_name_t field_id = mFI_GetFieldId();

    if (my_room->scene == SCENE_COTTAGE_MY) {
        return (&Save_Get(island).cottage.room.layer_main)[layer].haniwa_step;
    }

    if ((mFI_GET_TYPE(field_id) == mFI_FIELD_PLAYER0_ROOM)) {
        int idx = (mFI_GetFieldId() - mFI_FIELD_PLAYER0_ROOM) & 3;
        int floor_no = mFI_GetPlayerHouseFloorNo(my_room->scene);

        if (floor_no != -1) {
            return (&Save_Get(homes[idx]).floors[floor_no].layer_main)[layer].haniwa_step;
        }
    }

    return NULL;
}

static void aMR_ClearBitSwitch(FTR_ACTOR* ftr_actor) {
    ftr_actor->switch_bit = FALSE;
    ftr_actor->haniwa_step = -1;
}

static void aMR_GetSwitchBit(FTR_ACTOR* ftr_actor, s16 placing_flag, MY_ROOM_ACTOR* my_room) {
    aFTR_PROFILE* profile = aMR_GetFurnitureProfile(ftr_actor->name);

    if (placing_flag == TRUE) {
        ftr_actor->switch_bit = TRUE;
    } else {
        u64* ftr_bit_switch_table = aMR_GetBitSwitchTable(ftr_actor->layer, my_room);
        xyz_t pos = ftr_actor->position;
        int ut_x;
        int ut_z;

        pos.x -= 1.0f;
        pos.z -= 1.0f;

        if (ftr_bit_switch_table != NULL && ftr_actor->layer < mCoBG_LAYER2 &&
            mFI_Wpos2UtNum_inBlock(&ut_x, &ut_z, pos) && aMR_BOUNDS_OK(ut_x, ut_z)) {
            if (((*ftr_bit_switch_table) >> ((ut_x - 1 + (ut_z - 1) * 8) & 0x3F) & 1)) {
                ftr_actor->switch_bit = TRUE;
            } else {
                ftr_actor->switch_bit = FALSE;
            }
        } else if ((mFI_CheckShop() == TRUE || mFI_GetFieldId() == mFI_FIELD_ROOM_BROKER_SHOP) && profile != NULL &&
                   aFTR_CHECK_INTERACTION(profile->interaction_type, aFTR_INTERACTION_TYPE_MUSIC_DISK)) {
            ftr_actor->switch_bit = FALSE; // disable music players in shops
        } else {
            ftr_actor->switch_bit = TRUE;
        }
    }
}

static void aMR_GetHaniwaStep(FTR_ACTOR* ftr_actor, s16 placing_flag, int* step_idx, MY_ROOM_ACTOR* my_room) {
    if (placing_flag == TRUE) {
        ftr_actor->haniwa_step = -1;
    } else {
        int layer = ftr_actor->layer;
        int ut_x;
        int ut_z;

        if (layer < mCoBG_LAYER2 && mFI_Wpos2UtNum_inBlock(&ut_x, &ut_z, ftr_actor->position) &&
            aMR_BOUNDS_OK(ut_x, ut_z)) {
            u32* step_table = aMR_GetHaniwaStepSaveData(layer, my_room);

            if (step_table != NULL) {
                int shift = (ut_x - 1) * 4;
                int idx = ut_z - 1;

                ftr_actor->haniwa_step = (step_table[idx] >> shift) & 0xF;
            } else if (*step_idx < mFM_HANIWA_STEP_NUM) {
                u8* step = mFI_GetHaniwaStepBlock(0, 0);

                ftr_actor->haniwa_step = step[*step_idx];
                (*step_idx)++;
            } else {
                ftr_actor->haniwa_step = -1;
            }
        } else {
            ftr_actor->haniwa_step = -1;
        }
    }
}

static void aMR_OperateSwitchBit(FTR_ACTOR* ftr_actor) {
    if (ftr_actor->switch_bit == FALSE) {
        ftr_actor->switch_bit = TRUE;
    } else {
        ftr_actor->switch_bit = FALSE;
    }
}

static void aMR_SaveHaniwaStepData(MY_ROOM_ACTOR* my_room) {
    FTR_ACTOR* ftr_actor = l_aMR_work.ftr_actor_list;
    u8* used = l_aMR_work.used_list;
    int i;

    for (i = 0; i < l_aMR_work.list_size; i++) {
        if (*used == TRUE && ftr_actor->name < FTR_NUM) {
            aFTR_PROFILE* profile = aMR_GetFurnitureProfile(ftr_actor->name);

            if (profile != NULL && aFTR_CHECK_INTERACTION(profile->interaction_type, aFTR_INTERACTION_TYPE_HANIWA)) {
                int ut_x;
                int ut_z;

                if (mFI_Wpos2UtNum_inBlock(&ut_x, &ut_z, ftr_actor->position) && aMR_BOUNDS_OK(ut_x, ut_z)) {
                    u32* step_data = aMR_GetHaniwaStepSaveData(ftr_actor->layer, my_room);

                    if (step_data != NULL) {
                        int shift = (ut_x - 1) * 4;
                        int idx = ut_z - 1;

                        step_data[idx] &= ~(0xF << shift);
                        step_data[idx] |= (ftr_actor->haniwa_step & 0xF) << shift;
                    }
                }
            }
        }

        ftr_actor++;
        used++;
    }
}

static void aMR_SaveOneFtrSwitchData(mActor_name_t ftr_name, int ut_x, int ut_z, s16 layer, u64* switch_bit_table) {
    if (ITEM_IS_FTR(ftr_name)) {
        mActor_name_t name;
        int ftr_id;

        if (aMR_UnitNum2FtrItemNoFtrID(&name, &ftr_id, ut_x, ut_z, layer)) {
            FTR_ACTOR* ftr_actor = &l_aMR_work.ftr_actor_list[ftr_id];

            if (aMR_BOUNDS_OK(ut_x, ut_z)) {
                int shift = (ut_x - 1 + (ut_z - 1) * 8) & 0x3F;

                if (ftr_actor->switch_bit) {
                    (*switch_bit_table) |= 1ull << shift;
                }
            }
        }
    }
}

static void aMR_SaveSwitchData(MY_ROOM_ACTOR* my_room) {
    if (l_aMR_work.ftr_actor_list != NULL && l_aMR_work.used_list != NULL) {
        u64* bit_switch_table;
        int ut_x;
        int ut_z;
        int layer;
        mActor_name_t* fg_p;

        for (layer = mCoBG_LAYER0; layer < mCoBG_LAYER2; layer++) {
            bit_switch_table = aMR_GetBitSwitchTable(layer, my_room);

            if (bit_switch_table != NULL) {
                *bit_switch_table = 0;
                fg_p = aMR_GetLayerTopFg(layer);

                if (fg_p != NULL) {
                    for (ut_z = aMR_MIN_BOUND; ut_z <= aMR_MAX_BOUND; ut_z++) {
                        for (ut_x = aMR_MIN_BOUND; ut_x <= aMR_MAX_BOUND; ut_x++) {
                            aMR_SaveOneFtrSwitchData(fg_p[ut_x + ut_z * UT_X_NUM], ut_x, ut_z, layer, bit_switch_table);
                        }
                    }
                }
            }
        }
    }
}

static int aMR_CountFriendFurniture(FTR_ACTOR* ftr_actor, u8 friend_type) {
    u8* used = l_aMR_work.used_list;
    FTR_ACTOR* check_ftr_actor = l_aMR_work.ftr_actor_list;
    int count = 0;
    int i = 0;

    if (check_ftr_actor != NULL && used != NULL) {
        for (i; i < l_aMR_work.list_size; i++) {
            if (*used == TRUE && i != ftr_actor->id) {
                if (check_ftr_actor->name == ftr_actor->name) {
                    if (friend_type == aMR_FRIEND_ALL) {
                        count++;
                    } else if (friend_type == aMR_FRIEND_ON) {
                        if (check_ftr_actor->switch_bit != FALSE) {
                            count++;
                        }
                    } else { /* aMR_FRIEND_OFF */
                        if (check_ftr_actor->switch_bit == FALSE) {
                            count++;
                        }
                    }
                }
            }

            check_ftr_actor++;
            used++;
        }

        return count;
    }

    return 0;
}

static int aMR_GetWeight(int type) {
    return 1;
}

static void aMR_PlussWeight(ACTOR* actorx, FTR_ACTOR* ftr_actor) {
    MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;

    my_room->weight += aMR_GetWeight(ftr_actor->shape_type);
}

static void aMR_MinusWeight(ACTOR* actorx, FTR_ACTOR* ftr_actor) {
    MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;

    my_room->weight -= aMR_GetWeight(ftr_actor->shape_type);
}

static int aMR_WeightPossible(ACTOR* actorx, int type) {
    MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;

    if (aMR_GetWeight(type) + my_room->weight <= l_aMR_work.list_size) {
        return TRUE;
    }

    return FALSE;
}

static int aMR_CheckFurnitureBankExist(u16 idx) {
    if (l_bank_index_table[idx] != 255) {
        return TRUE;
    }

    return FALSE;
}

static int aMR_GetFtrBankID(u16 idx) {
    if (l_bank_index_table[idx] == 255) {
        return -1;
    }

    return l_bank_index_table[idx];
}

static u8* aMR_BankNo2BankAddress(int bank_no) {
    if (bank_no == -1) {
        return NULL;
    }

    return l_bank_address_table[bank_no];
}

static u8* aMR_FtrNo2BankAddress(u16 ftr_no) {
    return aMR_BankNo2BankAddress(aMR_GetFtrBankID(ftr_no));
}

static int aMR_SearchFurnitureBankVacancy(void) {
    int i;
    int count = 0;

    for (i = 0; i < FTR_NUM; i++) {
        if (aMR_CheckFurnitureBankExist(i)) {
            count++;
        }
    }

    if (count < l_aMR_work.list_size) {
        return TRUE;
    }

    return FALSE;
}

static int aMR_SearchFreeFurnitureBankIdx(void) {
    int i;

    for (i = 0; i < l_aMR_work.list_size; i++) {
        int j;
        int count = 0;

        for (j = 0; j < FTR_NUM; j++) {
            if (l_bank_index_table[j] == i) {
                count++;
            }
        }

        if (count == 0) {
            return i;
        }
    }

    return -1;
}

static int aMR_SearchFreeFurnitureActorNumber(void) {
    int i;

    for (i = 0; i < l_aMR_work.list_size; i++) {
        if (l_aMR_work.used_list[i] == FALSE) {
            return i;
        }
    }

    return -1;
}

// Part 1

static int aMR_CountAppointFurniture(u16 ftr_no) {
    FTR_ACTOR* ftr_actor = l_aMR_work.ftr_actor_list;
    u8* used = l_aMR_work.used_list;
    int i;
    int count = 0;

    for (i = 0; i < l_aMR_work.list_size; i++) {
        if (used[i]) {
            if (ftr_no == ftr_actor[i].name) {
                count++;
            }
        }
    }

    return count;
}

static int aMR_CountFurniture(void) {
    FTR_ACTOR* ftr_actor = l_aMR_work.ftr_actor_list;
    u8* used = l_aMR_work.used_list;
    int i;
    int count = 0;

    if (ftr_actor != NULL && used != NULL) {
        for (i = 0; i < l_aMR_work.list_size; i++) {
            if (*used) {
                count++;
            }

            ftr_actor++;
            used++;
        }
    }

    return count;
}

static void aMR_UnitNumber2Position(xyz_t* pos, u8 type, int ut_x, int ut_z) {
    pos->x = (f32)ut_x * mFI_UT_WORLDSIZE_X_F + mFI_UT_WORLDSIZE_HALF_X_F;
    pos->y = 0.0f;
    pos->z = (f32)ut_z * mFI_UT_WORLDSIZE_Z_F + mFI_UT_WORLDSIZE_HALF_Z_F;

    if (type == aFTR_SHAPE_TYPEC) {
        pos->x += mFI_UT_WORLDSIZE_HALF_X_F;
        pos->z += mFI_UT_WORLDSIZE_HALF_Z_F;
    }
}

static int aMR_Wpos2PlaceNumber(int* ut_x, int* ut_z, xyz_t pos, u8 type) {
    if (type == aFTR_SHAPE_TYPEC) {
        pos.x -= mFI_UT_WORLDSIZE_HALF_X_F;
        pos.z -= mFI_UT_WORLDSIZE_HALF_Z_F;
    }

    *ut_x = (int)(pos.x / mFI_UT_WORLDSIZE_X_F);
    *ut_z = (int)(pos.z / mFI_UT_WORLDSIZE_Z_F);

    if (*ut_x < UT_X_NUM && *ut_z < UT_Z_NUM) {
        return TRUE;
    }

    return FALSE;
}

static u8* aMR_GetLayerPlaceTable(s16 layer) {
    if (layer == mCoBG_LAYER0) {
        return aMR_place_table[0];
    }

    if (layer == mCoBG_LAYER1) {
        return aMR_place_table[1];
    }

    return NULL;
}

static int aMR_Direct2PlussUnit(int* ut_x, int* ut_z, int direct) {
    switch (direct) {
        case aMR_DIRECT_UP:
            (*ut_z)--;
            return TRUE;
        case aMR_DIRECT_LEFT:
            (*ut_x)--;
            return TRUE;
        case aMR_DIRECT_DOWN:
            (*ut_z)++;
            return TRUE;
        case aMR_DIRECT_RIGHT:
            (*ut_x)++;
            return TRUE;
    }

    return FALSE;
}

static int aMR_GetTypeBPlaceInfo(int* x0, int* z0, int* x1, int* z1, u8 type, xyz_t pos) {
    *x0 = (int)(pos.x / mFI_UT_WORLDSIZE_X_F);
    *z0 = (int)(pos.z / mFI_UT_WORLDSIZE_Z_F);

    switch (type) {
        case aFTR_SHAPE_TYPEB_90:
            *x1 = *x0;
            *z1 = *z0 - 1;
            return TRUE;
        case aFTR_SHAPE_TYPEB_180:
            *x1 = *x0 - 1;
            *z1 = *z0;
            return TRUE;
        case aFTR_SHAPE_TYPEB_270:
            *x1 = *x0;
            *z1 = *z0 + 1;
            return TRUE;
        case aFTR_SHAPE_TYPEB_0:
            *x1 = *x0 + 1;
            *z1 = *z0;
            return TRUE;
    }

    return FALSE;
}

static void aMR_SetInfoFurnitureTable(u8 type, int idx, int id, s16 layer) {
    u8* place_table = aMR_GetLayerPlaceTable(layer);

    switch (type) {
        case aFTR_SHAPE_TYPEA:
            place_table[idx] = id;
            break;
        case aFTR_SHAPE_TYPEB_90:
            place_table[idx] = id;
            place_table[idx - UT_X_NUM] = id;
            break;
        case aFTR_SHAPE_TYPEB_180:
            place_table[idx] = id;
            place_table[idx - 1] = id;
            break;
        case aFTR_SHAPE_TYPEB_270:
            place_table[idx] = id;
            place_table[idx + UT_X_NUM] = id;
            break;
        case aFTR_SHAPE_TYPEB_0:
            place_table[idx] = id;
            place_table[idx + 1] = id;
            break;
        case aFTR_SHAPE_TYPEC:
            place_table[idx] = id;
            place_table[idx + 1] = id;
            place_table[idx + UT_X_NUM] = id;
            place_table[idx + UT_X_NUM + 1] = id;
            break;
    }
}

static void aMR_SetFurnitureType(FTR_ACTOR* ftr_actor, int angle) {
    aFTR_PROFILE* profile = aMR_GetFurnitureProfile(ftr_actor->name);

    if (profile->shape == aFTR_SHAPE_TYPEB_0) {
        ftr_actor->shape_type = l_typeB0_table[angle];
        ftr_actor->original_shape_type = profile->shape;
    } else {
        ftr_actor->shape_type = profile->shape;
        ftr_actor->original_shape_type = profile->shape;
    }
}

static void aMR_SetFirstScale(FTR_ACTOR* ftr_actor) {
    static xyz_t xyz0 = { 0.0f, 0.0f, 0.0f };
    static xyz_t xyz1 = { 1.0f, 1.0f, 1.0f };

    if (ftr_actor->state == aFTR_STATE_BIRTH || ftr_actor->state == aFTR_STATE_BIRTH_WAIT) {
        ftr_actor->scale = xyz0;
    } else {
        ftr_actor->scale = xyz1;
    }

    if (ftr_actor->shape_type >= aFTR_SHAPE_TYPEB_90 && ftr_actor->shape_type <= aFTR_SHAPE_TYPEB_0) {
        ftr_actor->birth_scale_modifier = 0.5f;
        ftr_actor->birth_anim_step = 6000;
    } else {
        ftr_actor->birth_scale_modifier = 0.5f;
        ftr_actor->birth_anim_step = 6000;
    }

    ftr_actor->birth_anim_counter = 0;
    ftr_actor->dust_timer = 0;
}

static void aMR_DeleteFurnitureBank(u16 ftr_no) {
    if (aMR_CountAppointFurniture(ftr_no) == 0) {
        l_bank_index_table[ftr_no] = 255;
    }
}

static void aMR_InitHaniwaOnTable(ACTOR* actorx) {
    MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;
    int* haniwa_on_table_p = my_room->haniwa_on_table;
    int i;

    for (i = 0; i < aMR_HANIWA_ON_TABLE_NUM; i++) {
        *haniwa_on_table_p++ = -1;
    }
}

static int aMR_GetHaniwaSwitchVac(ACTOR* actorx) {
    MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;
    int* haniwa_on_table_p = my_room->haniwa_on_table;
    int i;

    for (i = 0; i < aMR_HANIWA_ON_TABLE_NUM; i++) {
        if (*haniwa_on_table_p == -1) {
            return i;
        }
        haniwa_on_table_p++;
    }

    return -1;
}

static int aMR_TidyHaniwaOnTable(ACTOR* actorx, int idx) {
    MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;
    int* haniwa_on_table_p = my_room->haniwa_on_table;
    int i;

    if (idx >= 0 && idx < aMR_HANIWA_ON_TABLE_NUM) {
        if (idx == (aMR_HANIWA_ON_TABLE_NUM - 1)) {
            haniwa_on_table_p[idx] = -1;
        } else {
            for (i = idx; i < (aMR_HANIWA_ON_TABLE_NUM - 1); i++) {
                haniwa_on_table_p[i] = haniwa_on_table_p[i + 1];
            }

            haniwa_on_table_p[aMR_HANIWA_ON_TABLE_NUM - 1] = -1;
        }

        return TRUE;
    }

    return FALSE;
}

static int aMR_HaniwaOffReport(ACTOR* actorx, int ftr_idx) {
    MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;
    int* haniwa_on_table_p = my_room->haniwa_on_table;
    int i;

#ifdef VITA_MP
    for (i = 0; i < aMR_MP_HANIWA_WAIT; i++) {
        if (ftr_idx == aMR_mp_haniwa_wait[i]) {
            for (; i < aMR_MP_HANIWA_WAIT - 1; i++) {
                aMR_mp_haniwa_wait[i] = aMR_mp_haniwa_wait[i + 1];
            }
            aMR_mp_haniwa_wait[aMR_MP_HANIWA_WAIT - 1] = -1;
            return TRUE;
        }
    }
#endif
    for (i = 0; i < aMR_HANIWA_ON_TABLE_NUM; i++) {
        if (*haniwa_on_table_p == ftr_idx) {
            aMR_TidyHaniwaOnTable(actorx, i);
#ifdef VITA_MP
            // (the first switched on elsewhere while the list was full takes the place)
            if (aMR_mp_haniwa_wait[0] >= 0) {
                int w;

                my_room->haniwa_on_table[aMR_HANIWA_ON_TABLE_NUM - 1] = aMR_mp_haniwa_wait[0];
                for (w = 0; w < aMR_MP_HANIWA_WAIT - 1; w++) {
                    aMR_mp_haniwa_wait[w] = aMR_mp_haniwa_wait[w + 1];
                }
                aMR_mp_haniwa_wait[aMR_MP_HANIWA_WAIT - 1] = -1;
            }
#endif
            return TRUE;
        }

        haniwa_on_table_p++;
    }

    return FALSE;
}

static int aMR_ClearHaniwaSwitch(ACTOR* actorx, FTR_ACTOR* ftr_actor) {
    ftr_actor->switch_changed_flag = FALSE;
    ftr_actor->switch_bit = FALSE;
    return aMR_HaniwaOffReport(actorx, ftr_actor->id);
}

static void aMR_HaniwaSwitchOn(ACTOR* actorx, FTR_ACTOR* ftr_actor) {
    MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;
    int free_idx = aMR_GetHaniwaSwitchVac(actorx);
    int* haniwa_on_table_p = my_room->haniwa_on_table;

    if (free_idx == -1) {
        int ftr_id = haniwa_on_table_p[0];

#ifdef VITA_MP
        // (another screen's switch: that screen chose which one goes off, and says so)
        if (aMR_mp_replaying) {
            ftr_actor->switch_changed_flag = TRUE;
            ftr_actor->switch_bit = TRUE;
            {
                int w;

                for (w = 0; w < aMR_MP_HANIWA_WAIT && aMR_mp_haniwa_wait[w] >= 0; w++) {
                }
                // (the longest waiting never heard its turn off: it goes off now)
                if (w == aMR_MP_HANIWA_WAIT) {
                    FTR_ACTOR* old = l_aMR_work.ftr_actor_list + aMR_mp_haniwa_wait[0];

                    if (old->switch_bit && aFTR_CHECK_INTERACTION(aMR_GetFurnitureProfile(old->name)->interaction_type,
                                                                  aFTR_INTERACTION_TYPE_HANIWA)) {
                        old->switch_changed_flag = TRUE;
                        old->switch_bit = FALSE;
                    }
                    for (w = 0; w < aMR_MP_HANIWA_WAIT - 1; w++) {
                        aMR_mp_haniwa_wait[w] = aMR_mp_haniwa_wait[w + 1];
                    }
                }
                aMR_mp_haniwa_wait[w] = ftr_actor->id;
            }
            return;
        }
#endif
        if (ftr_id >= 0) {
            FTR_ACTOR* target_ftr_actor = l_aMR_work.ftr_actor_list + ftr_id;

            /* Turn off the old active gyroid */
            target_ftr_actor->switch_changed_flag = TRUE;
            target_ftr_actor->switch_bit = FALSE;
            aMR_HaniwaOffReport(actorx, target_ftr_actor->id);
#ifdef VITA_MP
            // (ones switched on elsewhere took the places freed: the next oldest make room)
            while (haniwa_on_table_p[aMR_HANIWA_ON_TABLE_NUM - 1] != -1 && haniwa_on_table_p[0] >= 0) {
                u8 off = FALSE;

                target_ftr_actor = l_aMR_work.ftr_actor_list + haniwa_on_table_p[0];
                target_ftr_actor->switch_changed_flag = TRUE;
                target_ftr_actor->switch_bit = FALSE;
                aMR_HaniwaOffReport(actorx, target_ftr_actor->id);
                aMR_mp_send(MP_VFX_FTR_SWITCH, target_ftr_actor, &off, 1);
            }
#endif

            /* Turn on new one */
            ftr_actor->switch_changed_flag = TRUE;
            ftr_actor->switch_bit = TRUE;
            haniwa_on_table_p[aMR_HANIWA_ON_TABLE_NUM - 1] = ftr_actor->id;
        }
    } else {
        ftr_actor->switch_changed_flag = TRUE;
        ftr_actor->switch_bit = TRUE;
        haniwa_on_table_p[free_idx] = ftr_actor->id;
    }
}

static void aMR_SetSwitchStepData(FTR_ACTOR* ftr_actor, s16 placed_flag, MY_ROOM_ACTOR* my_room) {
    aFTR_PROFILE* profile = aMR_GetFurnitureProfile(ftr_actor->name);

    if (aFTR_CHECK_INTERACTION(profile->interaction_type, aFTR_INTERACTION_TYPE_HANIWA)) {
        ACTOR* actorx = aMR_CLIP->my_room_actor_p;

        if (actorx != NULL) {
            MY_ROOM_ACTOR* l_my_room = (MY_ROOM_ACTOR*)actorx;

            if (placed_flag == TRUE) {
                aMR_HaniwaSwitchOn(actorx, ftr_actor);
                aMR_GetHaniwaStep(ftr_actor, placed_flag, &l_my_room->haniwa_step_idx, l_my_room);
                return;
            } else {
                aMR_GetSwitchBit(ftr_actor, placed_flag, l_my_room);
                if (ftr_actor->switch_bit) {
                    aMR_HaniwaSwitchOn(actorx, ftr_actor);
                }

                aMR_GetHaniwaStep(ftr_actor, placed_flag, &l_my_room->haniwa_step_idx, l_my_room);
                return;
            }
        }
    } else {
        if (placed_flag == TRUE) {
            ftr_actor->switch_bit = TRUE;
            ftr_actor->haniwa_step = -1;

            if (aFTR_CHECK_INTERACTION(profile->interaction_type, aFTR_INTERACTION_TYPE_TOGGLE) ||
                aFTR_CHECK_INTERACTION(profile->interaction_type, aFTR_INTERACTION_TYPE_START_DISABLED)) {
                ftr_actor->switch_bit = FALSE;
            }
        } else {
            aMR_GetSwitchBit(ftr_actor, placed_flag, my_room);
            ftr_actor->haniwa_step = -1;
        }

        return;
    }

    ftr_actor->switch_bit = FALSE;
    ftr_actor->haniwa_step = 0;
}

static int aMR_SystemAnimeCt_UniqueCt(FTR_ACTOR* ftr_actor, aFTR_PROFILE* profile) {
    if (profile->rig != NULL) {
        cKF_SkeletonInfo_R_c* keyframe = &ftr_actor->keyframe;
        cKF_Skeleton_R_c* skeleton = profile->rig->skeleton;
        cKF_Animation_R_c* animation = profile->rig->animation;

        cKF_SkeletonInfo_R_ct(keyframe, skeleton, animation, ftr_actor->joint, ftr_actor->morph);
        cKF_SkeletonInfo_R_init_standard_repeat(keyframe, animation, NULL);
        cKF_SkeletonInfo_R_play(keyframe);

        keyframe->frame_control.speed = 0.5f;
    }

    if (profile->vtable != NULL && profile->vtable->ct_proc != NULL) {
        profile->vtable->ct_proc(ftr_actor, aMR_FtrNo2BankAddress(ftr_actor->name));
    }

    return TRUE;
}

static int aMR_RegistMoveBg(FTR_ACTOR* ftr_actor, aFTR_PROFILE* profile) {
    ftr_actor->move_bg_idx = -1;

    if (aFTR_CHECK_INTERACTION(profile->interaction_type, aFTR_INTERACTION_TYPE_NO_COLLISION) == FALSE ||
        mFI_CheckShop() == TRUE || mFI_GetFieldId() == mFI_FIELD_ROOM_BROKER_SHOP) {
        f32 height;

        if (aMR_layer_set_info[ftr_actor->name] == aFTR_SET_TYPE_SURFACE) {
            height = profile->height;
        } else {
            height = 100.0f;
        }

        ftr_actor->move_bg_idx =
            mCoBG_RegistMoveBg(&ftr_actor->bg_register, &ftr_actor->position, &ftr_actor->last_position,
                               &ftr_actor->s_angle_y, height, NULL, &ftr_actor->collision_scale, &ftr_actor->bg_contact,
                               &ftr_actor->base_position, profile->move_bg_type, mCoBG_ATTRIBUTE_STONE, 81.0f);

        if (ftr_actor->move_bg_idx != -1) {
            return TRUE;
        } else {
            return FALSE;
        }
    } else {
        return TRUE;
    }
}

static void aMR_MiniDiskCommonCt(FTR_ACTOR* ftr_actor, s16 placed_flag) {
    aFTR_PROFILE* profile = aMR_GetFurnitureProfile(ftr_actor->name);
    int count;
    int i;

    if (aFTR_CHECK_INTERACTION(profile->interaction_type, aFTR_INTERACTION_TYPE_MUSIC_DISK)) {
        if (placed_flag == FALSE) {
            if (ftr_actor->switch_bit == TRUE) {
                ftr_actor->haniwa_state = 1;
                ftr_actor->switch_changed_flag = FALSE;
            }
        } else {
            int home_idx = (mFI_GetFieldId() - mFI_FIELD_PLAYER0_ROOM) & 3;

            count = 0;
            for (i = 0; i < MINIDISK_NUM; i++) {
                u32* music_box = Save_Get(scene_no) == SCENE_COTTAGE_MY ? Save_Get(island).cottage.music_box
                                                                        : Save_Get(homes[home_idx]).music_box;

                if (((music_box[(i / 32) & 1] >> (i & 31)) & 1) != 0) {
                    count++;
                }
            }

            /* Pick a random song to play when placed */
            if (count > 0) {
                int sel = RANDOM(count);

                count = 0;
                for (i = 0; i < MINIDISK_NUM; i++) {
                    u32* music_box = Save_Get(scene_no) == SCENE_COTTAGE_MY ? Save_Get(island).cottage.music_box
                                                                            : Save_Get(homes[home_idx]).music_box;

                    if (((music_box[(i / 32) & 1] >> (i & 31)) & 1) != 0) {
                        if (sel == count) {
                            ftr_actor->items[0] = ITM_MINIDISK_START + i;
                            // @BUG - missing break;
                        }

                        count++;
                    }
                }
            }

            ftr_actor->switch_changed_flag = FALSE;
            ftr_actor->switch_bit = FALSE;
        }
    }
}

static void aMR_MiniDiskCommonDt(FTR_ACTOR* ftr_actor, ACTOR* actorx) {
    aFTR_PROFILE* profile = aMR_GetFurnitureProfile(ftr_actor->name);
    MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;

    if (aFTR_CHECK_INTERACTION(profile->interaction_type, aFTR_INTERACTION_TYPE_MUSIC_DISK) &&
        ftr_actor->switch_bit == TRUE) {
        mBGMPsComp_delete_ps_room(my_room->bgm_info.md_no, 0);
        my_room->bgm_info.active_flag = FALSE;
        my_room->bgm_info.active_ftr_actor = NULL;
        my_room->bgm_info.last_md_no = -1;

        /* Don't delete for aerobics radio music */
        if (my_room->bgm_info.md_no != BGM_SPORTSFAIR_AEROBICS) {
            mBGMPsComp_MDPlayerPos_delete();
        }
    }
}

static void aMR_RadioCommonCt(FTR_ACTOR* ftr_actor, s16 placed_flag) {
    aFTR_PROFILE* profile = aMR_GetFurnitureProfile(ftr_actor->name);

    if (aFTR_CHECK_INTERACTION(profile->interaction_type, aFTR_INTERACTION_TYPE_RADIO_AEROBICS)) {
        ftr_actor->switch_changed_flag = FALSE;
        ftr_actor->switch_bit = FALSE;
    }
}

static void aMR_RadioCommonDt(FTR_ACTOR* ftr_actor, ACTOR* actorx) {
    aFTR_PROFILE* profile = aMR_GetFurnitureProfile(ftr_actor->name);

    if (aFTR_CHECK_INTERACTION(profile->interaction_type, aFTR_INTERACTION_TYPE_RADIO_AEROBICS) &&
        ftr_actor->switch_bit == TRUE) {
        aMR_ReserveDefaultBgm(actorx, ftr_actor);
        aMR_ChangeMDBgm(actorx, ftr_actor);
    }
}

static void aMR_ChangeMDBgm(ACTOR* actorx, FTR_ACTOR* ftr_actor) {
    MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;

    if ((int)my_room->bgm_info.reserve_flag == TRUE) {
        if (my_room->bgm_info.active_flag == FALSE) {
            if (my_room->bgm_info.md_no != -1) {
                mBGMPsComp_make_ps_room(my_room->bgm_info.md_no, 0);

                if (my_room->bgm_info.md_no != BGM_SPORTSFAIR_AEROBICS) {
                    mBGMPsComp_MDPlayerPos_make();
                }

                my_room->bgm_info.active_flag = TRUE;
                my_room->bgm_info.active_ftr_actor = ftr_actor;
                my_room->bgm_info.last_md_no = my_room->bgm_info.md_no;
            }
        } else if (my_room->bgm_info.md_no == -1) {
            mBGMPsComp_delete_ps_room(my_room->bgm_info.last_md_no, 0);
            mBGMPsComp_MDPlayerPos_delete();
            my_room->bgm_info.active_flag = FALSE;
            my_room->bgm_info.active_ftr_actor = NULL;
            my_room->bgm_info.last_md_no = my_room->bgm_info.md_no;
        } else {
            mBGMPsComp_delete_ps_room(my_room->bgm_info.last_md_no, 0);
            if (my_room->bgm_info.last_md_no != BGM_SPORTSFAIR_AEROBICS) {
                mBGMPsComp_MDPlayerPos_delete();
            }

            mBGMPsComp_make_ps_room(my_room->bgm_info.md_no, 0);
            if (my_room->bgm_info.md_no != BGM_SPORTSFAIR_AEROBICS) {
                mBGMPsComp_MDPlayerPos_make();
            }

            my_room->bgm_info.active_flag = TRUE;
            my_room->bgm_info.active_ftr_actor = ftr_actor;
            my_room->bgm_info.last_md_no = my_room->bgm_info.md_no;
        }

        my_room->bgm_info.reserve_flag = FALSE;
        my_room->bgm_info.last_md_no = my_room->bgm_info.md_no;
    }
}

static void aMR_ReserveBgm(ACTOR* actorx, int bgm_no, FTR_ACTOR* ftr_actor, s16 timer) {
    MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;

    my_room->bgm_info.reserve_flag = TRUE;
    my_room->bgm_info.timer = timer;
    my_room->bgm_info.md_no = bgm_no;
    my_room->bgm_info.reserved_ftr_actor = ftr_actor;
}

static void aMR_ReserveDefaultBgm(ACTOR* actorx, FTR_ACTOR* ftr_actor) {
    MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;

    my_room->bgm_info.reserve_flag = TRUE;
    my_room->bgm_info.timer = 0;
    my_room->bgm_info.md_no = -1;
    my_room->bgm_info.reserved_ftr_actor = ftr_actor;
}

static void aMR_AllMDSwitchOff(void) {
    FTR_ACTOR* ftr_actor = l_aMR_work.ftr_actor_list;
    u8* used = l_aMR_work.used_list;
    int i;

    for (i = 0; i < l_aMR_work.list_size; i++) {
        if (*used) {
            aFTR_PROFILE* profile = aMR_GetFurnitureProfile(ftr_actor->name);

            if (aFTR_CHECK_INTERACTION(profile->interaction_type, aFTR_INTERACTION_TYPE_MUSIC_DISK) ||
                aFTR_CHECK_INTERACTION(profile->interaction_type, aFTR_INTERACTION_TYPE_RADIO_AEROBICS)) {
                ftr_actor->switch_bit = FALSE;
                ftr_actor->switch_changed_flag = TRUE;
            }
        }

        used++;
        ftr_actor++;
    }
}

static void aMR_OneMDSwitchOn_TheOtherSwitchOff(FTR_ACTOR* ftr_actor) {
    aMR_AllMDSwitchOff();
    ftr_actor->switch_bit = TRUE;
    ftr_actor->switch_changed_flag = TRUE;
}

// Part 2

static void aMR_Status2MoveBgCollisionScale(FTR_ACTOR* ftr_actor, s16 status) {
    if (status == aFTR_STATE_BIRTH_WAIT) {
        ftr_actor->collision_scale = 0.6f;
    } else {
        ftr_actor->collision_scale = 1.0f;
    }
}

static void aMR_FurnitureCt(FTR_ACTOR* ftr_actor, GAME* game, int ut_x, int ut_z, mActor_name_t item, int ftr_idx,
                            s16 status, s16 layer, s16 placed_flag) {
    int angle = FTR_GET_ROTATION(item);
    static xyz_t xyz0 = { 0.0f, 0.0f, 0.0f };
    aFTR_PROFILE* profile;
    xyz_t pos;

    bzero(ftr_actor, sizeof(FTR_ACTOR));
#ifdef VITA_MP
    // (a new piece in this slot inherits nothing of the last one's)
    if (ftr_idx >= 0 && ftr_idx < aMR_MP_FTR_MAX) {
        aMR_mp_remote_demo[ftr_idx] = FALSE;
        aMR_mp_glide_left[ftr_idx] = 0;
        aMR_mp_quiet[ftr_idx] = FALSE;
        aMR_mp_vis[ftr_idx].x = aMR_mp_vis[ftr_idx].y = aMR_mp_vis[ftr_idx].z = 0.0f;
        aMR_mp_vis_rot[ftr_idx] = 0;
        aMR_mp_hint_left[ftr_idx] = 0;
        aMR_mp_hint_hold[ftr_idx] = 0;
        aMR_mp_demo_idle[ftr_idx] = 0;
    }
#endif
    ftr_actor->ctr_type = aFTR_CTR_TYPE_GAME_PLAY;
    ftr_actor->base_position = xyz0;
    ftr_actor->layer = layer;
    ftr_actor->angle_y = aMR_angle_table[angle];
    ftr_actor->angle_y_target = ftr_actor->angle_y;
    ftr_actor->s_angle_y = RAD2SHORT_ANGLE2(DEG2RAD(ftr_actor->angle_y));
    ftr_actor->name = mRmTp_FtrItemNo2FtrIdx(item);

    profile = aMR_GetFurnitureProfile(ftr_actor->name);
    ftr_actor->state = status;
    aMR_SetFurnitureType(ftr_actor, angle);
    aMR_SetFirstScale(ftr_actor);
    aMR_UnitNumber2Position(&pos, ftr_actor->shape_type, ut_x, ut_z);
    pos.y = mCoBG_GetBgY_AngleS_FromWpos(NULL, pos, 0.0f);
    ftr_actor->position = pos;
    ftr_actor->last_position = pos;
    ftr_actor->id = ftr_idx;
    aMR_SetInfoFurnitureTable(ftr_actor->shape_type, ut_x + ut_z * UT_X_NUM, ftr_idx, ftr_actor->layer);
    ftr_actor->collision_direction = 0;

    if (aMR_CLIP != NULL && aMR_CLIP->my_room_actor_p != NULL) {
        aMR_SetSwitchStepData(ftr_actor, placed_flag, (MY_ROOM_ACTOR*)aMR_CLIP->my_room_actor_p);
    }

    aMR_MiniDiskCommonCt(ftr_actor, placed_flag);
    aMR_RadioCommonCt(ftr_actor, placed_flag);
    aMR_Status2MoveBgCollisionScale(ftr_actor, status);
    if (aFTR_CHECK_INTERACTION(profile->interaction_type, aFTR_INTERACTION_TYPE_HANIWA)) {
        ftr_actor->dynamic_work_s[3] = placed_flag;
    }

    if (aMR_RegistMoveBg(ftr_actor, profile) && aMR_SystemAnimeCt_UniqueCt(ftr_actor, profile)) {
        aMR_SetFurniture2FG(ftr_actor, ftr_actor->position, TRUE);
    }
}

static int aMR_DmaFurniture_Common(u16 ftr_id, mActor_name_t item, u8* bank_addr, int bank_idx) {
    if (bank_addr != NULL) {
        aFTR_PROFILE* profile = aMR_GetFurnitureProfile(ftr_id);

        if (bank_idx != -1) {
            l_bank_index_table[ftr_id] = bank_idx;
        }

        if (profile->vtable != NULL && profile->vtable->dma_proc != NULL) {
            profile->vtable->dma_proc(item, aMR_FtrNo2BankAddress(ftr_id));
            return TRUE;
        } else {
            return TRUE;
        }
    }

    return FALSE;
}

static int aMR_GetFurnitureBank(u16 ftr_id, GAME_PLAY* play, mActor_name_t item) {
    if (aMR_SearchFurnitureBankVacancy()) {
        int bank_idx = aMR_SearchFreeFurnitureBankIdx();

        if (bank_idx != -1) {
            u8* bank_addr = aMR_BankNo2BankAddress(bank_idx);

            return aMR_DmaFurniture_Common(ftr_id, item, bank_addr, bank_idx);
        }
    }

    return FALSE;
}

static int aMR_GetFurnitureBank2(u16 ftr_id, GAME* game, mActor_name_t item) {
    GAME_PLAY* play = (GAME_PLAY*)game;

    if (aMR_CheckFurnitureBankExist(ftr_id)) {
        return TRUE;
    }

    if (aMR_GetFurnitureBank(ftr_id, play, item)) {
        return TRUE;
    }

    return FALSE;
}

static void aMR_MakeOneFurniture(ACTOR* actorx, mActor_name_t item, GAME* game, int ut_x, int ut_z, s16 layer) {
    if (ITEM_IS_FTR(item) && aMR_CountFurniture() < l_aMR_work.list_size) {
        u16 ftr_idx = mRmTp_FtrItemNo2FtrIdx((mActor_name_t)item);
        int id = aMR_SearchFreeFurnitureActorNumber();

        if (id != -1) {
            u8* used = l_aMR_work.used_list;
            FTR_ACTOR* ftr_actor = l_aMR_work.ftr_actor_list + id;

            if (aMR_GetFurnitureBank2(ftr_idx, game, item)) {
                used[id] = TRUE;
                aMR_FurnitureCt(ftr_actor, game, ut_x, ut_z, item, id, aFTR_STATE_STOP, layer, FALSE);
                aMR_PlussWeight(actorx, ftr_actor);
            }
        }
    }
}

static void aMR_MakeFurnitureActor(ACTOR* actorx, GAME_PLAY* play, s16 layer) {
    GAME* game = (GAME*)play;
    mActor_name_t* fg_p = aMR_GetLayerTopFg(layer);
    int ut_x;
    int ut_z;

    if (fg_p != NULL) {
        for (ut_z = 0; ut_z < UT_Z_NUM; ut_z++) {
            for (ut_x = 0; ut_x < UT_X_NUM; ut_x++) {
                aMR_MakeOneFurniture(actorx, *fg_p, game, ut_x, ut_z, layer);
                fg_p++;
            }
        }
    }
}

static void aMR_InitFurnitureActorExistTable(void) {
    int i;

    for (i = 0; i < l_aMR_work.list_size; i++) {
        l_aMR_work.used_list[i] = FALSE;
    }
}

static void aMR_InitFurnitureBankTable(void) {
    int i;

    for (i = 0; i < FTR_NUM; i++) {
        l_bank_index_table[i] = 255;
    }
}

extern cKF_Skeleton_R_c cKF_bs_r_furniture_stop;
extern cKF_Animation_R_c cKF_ba_r_furniture_stop;

static void aMR_InitDummyKeyAnime(GAME_PLAY* play, ACTOR* actorx) {
    MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;

    cKF_SkeletonInfo_R_ct(&my_room->keyframe, &cKF_bs_r_furniture_stop, &cKF_ba_r_furniture_stop, my_room->joint,
                          my_room->morph);
}

static void aMR_GetBankSitu(int* unalloc_bank_num, int* alloc_bank_num, int* req_bank_num) {
    *unalloc_bank_num = 0;
    *alloc_bank_num = 0;

    if (*req_bank_num > aMR_FTR_BANK_NUM) {
        *req_bank_num = aMR_FTR_BANK_NUM;
    }

    *unalloc_bank_num = *req_bank_num <= aMR_FTR_BANK_NUM ? *req_bank_num : aMR_FTR_BANK_NUM;
    if (*req_bank_num > *unalloc_bank_num) {
        *alloc_bank_num = *req_bank_num - *unalloc_bank_num;
    }
}

static void aMR_SecureFurnitureBank(MY_ROOM_ACTOR* my_room, GAME* game) {
    u8* bank_p;
    size_t size;
    int i;

    aMR_GetBankSitu(&my_room->bank_count0, &my_room->bank_count1, &l_aMR_work.list_size);
    size = my_room->bank_count0 * aMR_FTR_BANK_SIZE;

    for (i = 0; i < aMR_FTR_BANK_NUM; i++) {
        l_bank_address_table[i] = NULL;
    }

    bank_p = (u8*)zelda_malloc_align(size, 32);
    my_room->bank0_p = bank_p;
    for (i = 0; i < my_room->bank_count0; i++) {
        int size = i * aMR_FTR_BANK_SIZE;
        l_bank_address_table[i] = bank_p + (int)size;
    }

    if (my_room->bank_count1 > 0) {
        for (i = 0; i < my_room->bank_count1; i++) {
            u8* bank = (u8*)zelda_malloc(aMR_FTR_BANK_SIZE);
            int idx = my_room->bank_count0 + i;

            if (bank != NULL) {
                l_bank_address_table[idx] = bank;
            }
        }
    }
}

static int aMR_GetSceneFurnitureMax(void) {
    static int scene_table[] = {
        SCENE_NPC_HOUSE,
        SCENE_FIELD_TOOL_INSIDE,
        SCENE_SHOP0,
        SCENE_CONVENI,
        SCENE_SUPER,
        SCENE_DEPART,
        SCENE_DEPART_2,
        SCENE_BROKER_SHOP,
        SCENE_MY_ROOM_S,
        SCENE_MY_ROOM_M,
        SCENE_MY_ROOM_L,
        SCENE_KAMAKURA,
        SCENE_MUSEUM_ROOM_PAINTING,
        SCENE_MUSEUM_ROOM_FOSSIL,
        SCENE_MY_ROOM_LL1,
        SCENE_MY_ROOM_LL2,
        SCENE_MY_ROOM_BASEMENT_S,
        SCENE_MY_ROOM_BASEMENT_M,
        SCENE_MY_ROOM_BASEMENT_L,
        SCENE_MY_ROOM_BASEMENT_LL1,
        SCENE_COTTAGE_MY,
        SCENE_COTTAGE_NPC,
        -1,
    };

    static int bank_count_table[] = {
        30, 30, 10, 10, 10, 10, 10, 10, 32, 48, 64, 10, 20, 25, 64, 48, 64, 64, 64, 64, 64, 30, 5,
    };

    int scene = Save_Get(scene_no);
    int i;

    for (i = 0; scene_table[i] != -1; i++) {
        if (scene == scene_table[i]) {
            return bank_count_table[i];
        }
    }

    return 3;
}

#include "../src/actor/ac_my_room_msg_ctrl.c_inc"

static void aMR_SecureFurnitureRam(ACTOR* actorx) {
    l_aMR_work.ftr_actor_list = (FTR_ACTOR*)zelda_malloc(l_aMR_work.list_size * sizeof(FTR_ACTOR));
    l_aMR_work.used_list = (u8*)zelda_malloc(l_aMR_work.list_size * sizeof(u8));

    if (l_aMR_work.ftr_actor_list == NULL || l_aMR_work.used_list == NULL) {
        l_aMR_work.list_size = 0;
    }
}

static void aMR_InitFurnitureTable(FTR_ACTOR* ftr_actor, int count) {
    if (ftr_actor != NULL) {
        int i;

        for (i = 0; i < count; i++) {
            bzero(&ftr_actor[i], sizeof(FTR_ACTOR));
        }
    }
}

static int aMR_JudgeBreedNewFurniture(GAME* game, u16 ftr_no, int* ut_x, int* ut_z, u16* rotation, int* square_offset,
                                      int* layer);
static mActor_name_t aMR_SearchPickupFurniture(GAME* game);
static void aMR_Furniture2ItemBag(GAME* game);
static int aMR_JudgePlayerAction(xyz_t* wpos0, xyz_t* wpos1, int ftr_actor_idx);
static void aMR_PlayerMoveFurniture(int ftr_actor_idx, const xyz_t* wpos);
static int aMR_ftrID2Wpos(xyz_t* wpos, int ftr_id);
static int aMR_UnitNum2FtrItemNoFtrID(mActor_name_t* ftr_item_no, int* ftr_id, int ut_x, int ut_z, int layer);
static void aMR_FtrID2ExtinguishFurniture(int ftr_id);
static void aMR_RedmaFtrBank(void);
static int aMR_ReserveFurniture(GAME* game, u16 ftr_no, int free_idx, int ut_x, int ut_z, u16 rotation,
                                int square_offset, int layer);
static int aMR_CountFriendFurniture(FTR_ACTOR* ftr_actor, u8 switch_on);
static int aMR_JudgePlace2ndLayer(int ut_x, int ut_z);
static void aMR_OpenCloseCommonMove(FTR_ACTOR* ftr_actor, ACTOR* actor, GAME* game, f32 start_frame, f32 end_frame);
static int aMR_GetBedAction(ACTOR* actorx, int bed_move_dir);
static void aMR_MiniDiskCommonMove(FTR_ACTOR* ftr_actor, ACTOR* my_room_actorx, GAME* game, f32 start_frame,
                                   f32 end_frame);
static void aMR_FamicomEmuCommonMove(FTR_ACTOR* ftr_actor, ACTOR* my_room_actor, GAME* game, int rom_no,
                                     int agb_rom_no);
static int aMR_SetLeaf(const xyz_t* pos, f32 scale);
static int aMR_Ftr2Leaf(void);
static void aMR_LeafStartPos(xyz_t* pos);
static int aMR_PickupFtrLayer(void);
static void aMR_LeafPickuped(void);
static u8* aMR_FtrNo2BankAddress(u16 ftr_no);
static void aMR_CallSitDownOngenPosSE(const xyz_t* pos);
static void aMR_SoundMelody(FTR_ACTOR* ftr_actor, ACTOR* my_room_actor, int idx);
static int aMR_CheckDannaKill(xyz_t* pos);

static void aMR_SetClip(ACTOR* actorx) {
    MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;

    my_room->clip.my_room_actor_p = actorx;
    my_room->clip.judge_breed_new_ftr_proc = &aMR_JudgeBreedNewFurniture;
    my_room->clip.search_pickup_ftr_proc = &aMR_SearchPickupFurniture;
    my_room->clip.ftr2itemBag_proc = &aMR_Furniture2ItemBag;
    my_room->clip.judge_player_action_proc = &aMR_JudgePlayerAction;
    my_room->clip.player_move_ftr_proc = &aMR_PlayerMoveFurniture;
    my_room->clip.ftrId2wpos_proc = &aMR_ftrID2Wpos;
    my_room->clip.unitNum2ftrItemNoftrId_proc = &aMR_UnitNum2FtrItemNoFtrID;
    my_room->clip.ftrId2extinguishFtr_proc = &aMR_FtrID2ExtinguishFurniture;
    my_room->clip.redma_ftr_bank_proc = &aMR_RedmaFtrBank;
    my_room->clip.reserve_ftr_proc = &aMR_ReserveFurniture;
    my_room->clip.count_friend_ftr_proc = &aMR_CountFriendFurniture;
    my_room->clip.judge_place_2nd_layer_proc = &aMR_JudgePlace2ndLayer;
    my_room->clip.open_close_common_move_proc = &aMR_OpenCloseCommonMove;
    my_room->clip.get_bed_action_proc = &aMR_GetBedAction;
    my_room->clip.mini_disk_common_move_proc = &aMR_MiniDiskCommonMove;
    my_room->clip.famicom_emu_common_move_proc = &aMR_FamicomEmuCommonMove;
    my_room->clip.set_leaf_proc = &aMR_SetLeaf;
    my_room->clip.ftr2leaf_proc = &aMR_Ftr2Leaf;
    my_room->clip.leaf_start_pos_proc = &aMR_LeafStartPos;
    my_room->clip.pickup_ftr_layer_proc = &aMR_PickupFtrLayer;
    my_room->clip.leaf_pickuped_proc = &aMR_LeafPickuped;
    my_room->clip.ftrNo2bankAddress_proc = &aMR_FtrNo2BankAddress;
    my_room->clip.call_sit_down_ongen_pos_se_proc = &aMR_CallSitDownOngenPosSE;
    my_room->clip.clock_info_p = &my_room->clock_info;
    my_room->clip.sound_melody_proc = &aMR_SoundMelody;
    my_room->clip.check_danna_kill_proc = &aMR_CheckDannaKill;

    aMR_CLIP = &my_room->clip;
}

static void aMR_MakeItemDataInFurniture(void) {
    mActor_name_t* layer_top_table[mCoBG_LAYER_NUM];
    int idx;
    int j;
    int ut_x;
    int ut_z;
    int i;
    int k;

    for (k = 0; k < mCoBG_LAYER_NUM; k++) {
        layer_top_table[k] = aMR_GetLayerTopFg(k);
    }

    for (i = mCoBG_LAYER0; i < mCoBG_LAYER2; i++) {
        if (layer_top_table[i] != NULL) {
            idx = 0;

            for (j = i + 1; j < mCoBG_LAYER_NUM; j++) {
                mActor_name_t* fg_top_p = layer_top_table[j];

                if (fg_top_p != NULL) {
                    for (ut_z = 0; ut_z < UT_Z_NUM; ut_z++) {
                        for (ut_x = 0; ut_x < UT_X_NUM; ut_x++) {
                            if (*fg_top_p != EMPTY_NO && *fg_top_p != RSV_NO && *fg_top_p != RSV_WALL_NO) {
                                mActor_name_t item;
                                int ftr_id;

                                if (aMR_UnitNum2FtrItemNoFtrID(&item, &ftr_id, ut_x, ut_z, i)) {
                                    FTR_ACTOR* ftr_actor = l_aMR_work.ftr_actor_list + ftr_id;
                                    aFTR_PROFILE* profile = aMR_GetFurnitureProfile(ftr_actor->name);

                                    if (aFTR_IS_STORAGE(profile)) {
                                        ftr_actor->items[idx] = *fg_top_p;
                                        *fg_top_p = EMPTY_NO;
                                    }
                                }
                            }

                            fg_top_p++;
                        }
                    }
                }

                idx++;
            }
        }
    }
}

static void aMR_ClearSwitchSaveData(MY_ROOM_ACTOR* my_room) {
    u64* switch_bit_table[2];
    int i;

    for (i = mCoBG_LAYER0; i < mCoBG_LAYER2; i++) {
        switch_bit_table[i] = aMR_GetBitSwitchTable(i, my_room);

        if (switch_bit_table[i] != NULL) {
            *switch_bit_table[i] = 0;
        }
    }
}

static TempoBeat_c* aMR_NowSceneWaltzTempo(MY_ROOM_ACTOR* my_room) {
    mActor_name_t field_id = mFI_GetFieldId();

    if (my_room->scene == SCENE_COTTAGE_MY) {
        return &Save_Get(island).cottage.room.tempo_beat;
    } else if (mFI_GET_TYPE(field_id) == mFI_FIELD_PLAYER0_ROOM) {
        int idx = (mFI_GetFieldId() - mFI_FIELD_PLAYER0_ROOM) & 3;
        int floor_no = mFI_GetPlayerHouseFloorNo(my_room->scene);

        if (floor_no != -1) {
            return &Save_Get(homes[idx]).floors[floor_no].tempo_beat;
        }
    }

    return NULL;
}

static void aMR_GetSavedWaltzTempo(MY_ROOM_ACTOR* my_room) {
    sAdo_SetRhythmInfo(aMR_NowSceneWaltzTempo(my_room));
}

static void aMR_SaveWaltzTempo(MY_ROOM_ACTOR* my_room) {
    TempoBeat_c* rhythm = aMR_NowSceneWaltzTempo(my_room);

    if (rhythm != NULL && Common_Get(rhythym_updated) == FALSE) {
        sAdo_GetRhythmInfo(rhythm);
        Common_Set(rhythym_updated, TRUE);
    }
}

extern void aMR_SaveWaltzTempo2(void) {
    if (aMR_CLIP != NULL) {
        ACTOR* actorx = aMR_CLIP->my_room_actor_p;

#ifdef VITA_MP
        // (a visitor's go to the host with the cottage's lamps)
        if (actorx != NULL && !(aMR_mp_cot_shared() && mp_is_guest())) {
#else
        if (actorx != NULL) {
#endif
            MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;

            aMR_SaveWaltzTempo(my_room);
            aMR_SaveHaniwaStepData(my_room);
        }
    }
}

static int aMR_CheckRoomOwner(u32 player_no, MY_ROOM_ACTOR* my_room) {
#ifdef VITA_MP
    // the island's cottage is shared: whether visitors rearrange it is the host's say
    if (my_room->scene == SCENE_COTTAGE_MY && !mp_cot_owner()) {
        return FALSE;
    }
#endif
    if (Common_Get(field_type) == mFI_FIELDTYPE2_PLAYER_ROOM || my_room->scene == SCENE_COTTAGE_MY) {
        return TRUE;
    }

    return FALSE;
}

static void aMR_MakeRoomInfo(MY_ROOM_ACTOR* my_room) {
    u32 player_no = Common_Get(player_no);

    my_room->room_info.shop_flag = FALSE;
    my_room->room_info.owner_flag = FALSE;

    if (mFI_CheckShop() == TRUE || mFI_GetFieldId() == mFI_FIELD_ROOM_BROKER_SHOP) {
        my_room->room_info.shop_flag = TRUE;
    }

    if (aMR_CheckRoomOwner(player_no, my_room)) {
        my_room->room_info.owner_flag = TRUE;
    }
}

static void aMR_OneMDFurnitureSwitchOn(void) {
    FTR_ACTOR* ftr_actor = l_aMR_work.ftr_actor_list;
    u8* used = l_aMR_work.used_list;
    int i;

    for (i = 0; i < l_aMR_work.list_size; i++) {
        if (*used) {
            aFTR_PROFILE* profile = aMR_GetFurnitureProfile(ftr_actor->name);

            if (profile != NULL &&
                aFTR_CHECK_INTERACTION(profile->interaction_type, aFTR_INTERACTION_TYPE_MUSIC_DISK) &&
                !(ftr_actor->items[0] >= ITM_MINIDISK_START && ftr_actor->items[0] < ITM_MINIDISK_END)) {
                ftr_actor->switch_bit = FALSE;
                ftr_actor->switch_changed_flag = FALSE;
                ftr_actor->haniwa_state = 0;
            }
        }

        ftr_actor++;
        used++;
    }
}

static void aMR_InitFurnitureWork(void) {
    int i;
    int j;

    for (i = mCoBG_LAYER0; i < mCoBG_LAYER2; i++) {
        u8* place_p = aMR_place_table[i & 1];

        for (j = 0; j < UT_TOTAL_NUM; j++) {
            if (*place_p != 201 && *place_p != 200) {
                *place_p = 200;
            }

            place_p++;
        }
    }
}

static void aMR_DeleteMusicWhichMusicBoxDontHave(void) {
    FTR_ACTOR* ftr_actor = l_aMR_work.ftr_actor_list;
    u8* used = l_aMR_work.used_list;
    int i;

    if (mFI_GET_TYPE(mFI_GetFieldId()) == mFI_FIELD_PLAYER0_ROOM) {
        for (i = 0; i < l_aMR_work.list_size; i++) {
            if (*used == TRUE) {
                aFTR_PROFILE* profile = aMR_GetFurnitureProfile(ftr_actor->name);

                if (profile != NULL &&
                    aFTR_CHECK_INTERACTION(profile->interaction_type, aFTR_INTERACTION_TYPE_MUSIC_DISK) &&
                    (ftr_actor->items[0] >= ITM_MINIDISK_START && ftr_actor->items[0] < ITM_MINIDISK_END)) {
                    int idx = (mFI_GetFieldId() - mFI_FIELD_PLAYER0_ROOM) & 3;
                    int music_idx = (mActor_name_t)(ftr_actor->items[0] - ITM_MINIDISK_START);
                    u32* music_box = Save_Get(scene_no) == SCENE_COTTAGE_MY ? Save_Get(island).cottage.music_box
                                                                            : Save_Get(homes[idx]).music_box;

                    if (((music_box[(music_idx / 32) & 1] >> (music_idx & 31)) & 1) == 0) {
                        ftr_actor->items[0] = EMPTY_NO;

                        if (ftr_actor->switch_bit == TRUE) {
                            ftr_actor->switch_bit = FALSE;
                            ftr_actor->haniwa_state = 0;
                        }
                    }
                }
            }

            ftr_actor++;
            used++;
        }
    }
}

static void aMR_SetMDIslandNPC(void) {
    FTR_ACTOR* ftr_actor = l_aMR_work.ftr_actor_list;
    u8* used = l_aMR_work.used_list;
    int i;
    int md_set = FALSE;

    if (Save_Get(scene_no) == SCENE_COTTAGE_NPC) {
        for (i = 0; i < l_aMR_work.list_size; i++) {
            if (*used) {
                aFTR_PROFILE* profile = aMR_GetFurnitureProfile(ftr_actor->name);

                if (profile != NULL &&
                    aFTR_CHECK_INTERACTION(profile->interaction_type, aFTR_INTERACTION_TYPE_MUSIC_DISK)) {
                    if (md_set == FALSE) {
                        ftr_actor->items[0] = ITM_MINIDISK_START + mNpc_GetIslandMDIdx();
                        ftr_actor->switch_bit = TRUE;
                        ftr_actor->haniwa_state = 1;
                        md_set = TRUE;
                    } else {
                        ftr_actor->switch_bit = FALSE;
                        ftr_actor->haniwa_state = 0;
                    }
                }
            }

            ftr_actor++;
            used++;
        }
    }
}

static void My_Room_Actor_ct(ACTOR* actorx, GAME* game) {
    MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;
    GAME_PLAY* play = (GAME_PLAY*)game;

    bzero(&l_aMR_work, sizeof(l_aMR_work));
    my_room->scene = Save_Get(scene_no);
#ifdef VITA_MP
    if (my_room->scene == SCENE_COTTAGE_MY) {
        mp_cot_room_enter();
    }
#endif
    aMR_SetClip(actorx);
    aMR_InitFurnitureWork();
    aMR_SetMelodyData(my_room->melody);
    my_room->goki_ct_proc = &aMR_GokiInfoCt;
    aMR_MakeRoomInfo(my_room);
    aMR_GetSavedWaltzTempo(my_room);
    l_aMR_work.list_size = aMR_GetSceneFurnitureMax();
    aMR_SecureFurnitureRam(actorx);
    aMR_InitFurnitureTable(l_aMR_work.ftr_actor_list, l_aMR_work.list_size);
    aMR_InitFurnitureActorExistTable();
    aMR_SecureFurnitureBank(my_room, game);
    aMR_InitHaniwaOnTable(actorx);
    my_room->state = 0;
    aMR_InitFurnitureBankTable();
#ifdef VITA_MP
    bzero(aMR_mp_remote_demo, sizeof(aMR_mp_remote_demo));
    bzero(aMR_mp_glide_left, sizeof(aMR_mp_glide_left));
    bzero(aMR_mp_quiet, sizeof(aMR_mp_quiet));
    bzero(aMR_mp_vis, sizeof(aMR_mp_vis));
    bzero(aMR_mp_vis_rot, sizeof(aMR_mp_vis_rot));
    bzero(aMR_mp_hint_left, sizeof(aMR_mp_hint_left));
    bzero(aMR_mp_hint_hold, sizeof(aMR_mp_hint_hold));
    bzero(aMR_mp_session, sizeof(aMR_mp_session));
    aMR_mp_session_on = FALSE;
    aMR_mp_session_id = -1;
    aMR_mp_nosave = FALSE;
#ifdef TARGET_VITA
    s_vita_nes_picker_ftrID = -1; // (a picker left open as the last room went holds nothing here)
#endif
    mem_copy(aMR_mp_fossil_bit, Save_Get(museum_display).fossil_bit, sizeof(aMR_mp_fossil_bit));
    {
        int w;

        for (w = 0; w < aMR_MP_HANIWA_WAIT; w++) {
            aMR_mp_haniwa_wait[w] = -1;
        }
    }
#endif
    aMR_MakeFurnitureActor(actorx, play, mCoBG_LAYER0);
    aMR_MakeFurnitureActor(actorx, play, mCoBG_LAYER1);
    my_room->parent_ftr.ftrID = -1;
    Common_Set(make_npc2_actor, TRUE);
    aMR_MakeItemDataInFurniture();
    aMR_DeleteMusicWhichMusicBoxDontHave();
    aMR_InitDummyKeyAnime(play, actorx);
    my_room->allow_rotation_flag = TRUE;
    my_room->sit_timer = 0;
    my_room->bed_timer = 0;
    my_room->msg_type = 0;
    my_room->requested_msg_type = my_room->msg_type;
    my_room->bgm_info.reserve_flag = FALSE;
    my_room->bgm_info.md_no = -1;
    my_room->bgm_info.last_md_no = -1;
#ifdef VITA_MP
    // (a shared cottage's save keeps its lamps while its rooms are up, for players coming in)
    if (!aMR_mp_cot_shared())
#endif
    aMR_ClearSwitchSaveData(my_room);
    aMR_OneMDFurnitureSwitchOn();
    aMR_SetMDIslandNPC();
    my_room->emu_info.request_flag = FALSE;
    my_room->emu_info.explaination_given_flag = FALSE;
    mCkRh_InitCanLookGokiCount();
#ifdef VITA_MP
    aMR_mp_follow_start();
#endif
}

// Part 3

static void aMR_FreeMallocBank(ACTOR* actorx) {
    MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;
    int i;

    if (my_room->bank_count1 != 0) {
        for (i = 0; i < my_room->bank_count1; i++) {
            zelda_free(l_bank_address_table[my_room->bank_count0 + i]);
        }
    }
}

static void aMR_FreeHeapArea(ACTOR* actorx) {
    MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;

    if (l_aMR_work.ftr_actor_list != NULL) {
        zelda_free(l_aMR_work.ftr_actor_list);
    }

    if (l_aMR_work.used_list != NULL) {
        zelda_free(l_aMR_work.used_list);
    }

    aMR_FreeMallocBank(actorx);

    if (my_room->emu_info.famicom_names_p != NULL) {
        zelda_free(my_room->emu_info.famicom_names_p);
    }

    if (my_room->bank0_p != NULL) {
        zelda_free(my_room->bank0_p);
    }
}

#ifdef VITA_MP
// the unit a piece's item sits in, as aMR_SetFurniture2FG writes it
static int aMR_mp_main_ut(FTR_ACTOR* ftr_actor) {
    xyz_t base = ftr_actor->base_position;
    int ut_x;
    int ut_z;

    if (ftr_actor->shape_type == aFTR_SHAPE_TYPEA || ftr_actor->shape_type == aFTR_SHAPE_TYPEC) {
        int ut[4];

        return aMR_PosType2FurniturePoccessUnitNo(ut, &ftr_actor->position, ftr_actor->shape_type) > 0 ? ut[0] : -1;
    }
    sMath_RotateY(&base, DEG2RAD(ftr_actor->angle_y_target));
    ut_x = (int)((ftr_actor->position.x + base.x) / mFI_UT_WORLDSIZE_X_F);
    ut_z = (int)((ftr_actor->position.z + base.z) / mFI_UT_WORLDSIZE_Z_F);
    return (ut_x >= 0 && ut_x < UT_X_NUM && ut_z >= 0 && ut_z < UT_Z_NUM) ? ut_x + ut_z * UT_X_NUM : -1;
}

static int aMR_mp_is_storage(FTR_ACTOR* ftr_actor) {
    aFTR_PROFILE* profile = aMR_GetFurnitureProfile(ftr_actor->name);

    return profile != NULL && aFTR_IS_STORAGE(profile);
}

// a piece's things back in the save: over its main unit, and nothing over the rest of its units
static void aMR_mp_keep_canon(FTR_ACTOR* ftr_actor) {
    int ut[4];
    int n;
    int main_ut;
    int layer;
    int idx;
    int i;

    if (!aMR_mp_is_storage(ftr_actor) || ftr_actor->state == aFTR_STATE_BYE || ftr_actor->state == aFTR_STATE_DEATH) {
        return;
    }
    n = aMR_PosType2FurniturePoccessUnitNo(ut, &ftr_actor->position, ftr_actor->shape_type);
    main_ut = aMR_mp_main_ut(ftr_actor);
    for (layer = ftr_actor->layer + 1, idx = 0; layer < mCoBG_LAYER_NUM; layer++, idx++) {
        mActor_name_t* fg_p = aMR_GetLayerTopFg(layer);

        if (fg_p == NULL) {
            continue;
        }
        for (i = 0; i < n; i++) {
            fg_p[ut[i]] = EMPTY_NO;
        }
        if (main_ut >= 0) {
            fg_p[main_ut] = ftr_actor->items[idx];
        }
        ftr_actor->items[idx] = EMPTY_NO;
    }
}

// ...and as the capture takes them while the room is up
static void aMR_mp_report_canon(FTR_ACTOR* ftr_actor, void (*cb)(unsigned int save_off, unsigned short name, void* arg),
                                void* arg) {
    const u8* base = (const u8*)&common_data.save.save;
    int ut[4];
    int n;
    int main_ut;
    int layer;
    int idx;
    int i;

    if (ftr_actor->state == aFTR_STATE_BYE || ftr_actor->state == aFTR_STATE_DEATH || !aMR_mp_is_storage(ftr_actor) ||
        (main_ut = aMR_mp_main_ut(ftr_actor)) < 0) {
        return;
    }
    n = aMR_PosType2FurniturePoccessUnitNo(ut, &ftr_actor->position, ftr_actor->shape_type);
    for (layer = ftr_actor->layer + 1, idx = 0; layer < mCoBG_LAYER_NUM; layer++, idx++) {
        mActor_name_t* fg_p = aMR_GetLayerTopFg(layer);

        if (fg_p == NULL) {
            continue;
        }
        for (i = 0; i < n; i++) {
            if (ut[i] != main_ut) {
                cb((unsigned int)((const u8*)&fg_p[ut[i]] - base), EMPTY_NO, arg);
            }
        }
        cb((unsigned int)((const u8*)&fg_p[main_ut] - base), ftr_actor->items[idx], arg);
    }
}

// the piece is still in the cells where it stands
static int aMR_mp_ftr_in_cells(FTR_ACTOR* ftr_actor) {
    mActor_name_t* fg_p = aMR_GetLayerTopFg(ftr_actor->layer);
    int main_ut = aMR_mp_main_ut(ftr_actor);

    return fg_p != NULL && main_ut >= 0 && ftr_actor->state != aFTR_STATE_BYE &&
           fg_p[main_ut] ==
               aMR_GetSaveAngle(ftr_actor->angle_y_target, mRmTp_FtrIdx2FtrItemNo(ftr_actor->name, mRmTp_DIRECT_SOUTH));
}

// a pick-up that found its piece gone: the pocket gives back what it just took (from the slot it went to)
static void aMR_mp_unpocket(FTR_ACTOR* ftr_actor) {
    mActor_name_t item =
        mRmTp_FtrItemNo2Item1ItemNo(mRmTp_FtrIdx2FtrItemNo(ftr_actor->name, mRmTp_DIRECT_SOUTH), TRUE);
    PLAYER_ACTOR* player = GET_PLAYER_ACTOR((GAME_PLAY*)gamePT);
    Private_c* priv = Now_Private;
    int slot = -1;

    if (priv == NULL || ITEM_IS_MYMANNIQUIN(item) || ITEM_IS_MYUMBRELLA(item)) {
        return;
    }
    if (player != NULL) {
        slot = player->requested_main_index_data.pickup_furniture.inv_slot;
    }
    if (slot < 0 || slot >= mPr_POCKETS_SLOT_COUNT || priv->inventory.pockets[slot] != item) {
        slot = mPr_GetPossessionItemIdxWithCond(priv, item, mPr_ITEM_COND_NORMAL);
    }
    if (slot >= 0) {
        mPr_SetPossessionItem(priv, slot, EMPTY_NO, mPr_ITEM_COND_NORMAL);
    }
}
#endif

static void aMR_KeepItem2Fg(FTR_ACTOR* ftr_actor) {
    int idx = 0;
    int i;

#ifdef VITA_MP
    // a shared cottage keeps a piece's things over its main unit, where every screen looks for them
    if (aMR_mp_cot_shared()) {
        aMR_mp_keep_canon(ftr_actor);
        return;
    }
#endif
    for (i = ftr_actor->layer + 1; i < mCoBG_LAYER_NUM; i++) {
        if (ftr_actor->items[idx] != EMPTY_NO) {
            mActor_name_t* fg_p = aMR_GetLayerTopFg(i);

            if (fg_p != NULL) {
                int ut_x;
                int ut_z;

                if (aMR_Wpos2PlaceNumber(&ut_x, &ut_z, ftr_actor->position, ftr_actor->shape_type)) {
                    fg_p[ut_x + ut_z * UT_X_NUM] = ftr_actor->items[idx];
                    ftr_actor->items[idx] = EMPTY_NO;
                }
            }
        }

        idx++;
    }
}

#ifdef TARGET_VITA
// what the room's furniture holds right now (a dresser's contents, a stereo's record), as leaving puts it back
void aMR_pc_stored_cells(void (*cb)(unsigned int save_off, unsigned short name, void* arg), void* arg) {
    const u8* base = (const u8*)&common_data.save.save;
    FTR_ACTOR* ftr_actor = l_aMR_work.ftr_actor_list;
    u8* used = l_aMR_work.used_list;
    int i;

    if (aMR_CLIP == NULL || aMR_CLIP->my_room_actor_p == NULL || ftr_actor == NULL || used == NULL) {
        return;
    }
    for (i = 0; i < l_aMR_work.list_size; i++, ftr_actor++, used++) {
        int idx = 0;
        int layer;
        int ut_x;
        int ut_z;

#ifdef VITA_MP
        if (*used && aMR_mp_cot_shared()) {
            aMR_mp_report_canon(ftr_actor, cb, arg);
            continue;
        }
#endif
        if (!*used || !aMR_Wpos2PlaceNumber(&ut_x, &ut_z, ftr_actor->position, ftr_actor->shape_type)) {
            continue;
        }
        for (layer = ftr_actor->layer + 1; layer < mCoBG_LAYER_NUM; layer++, idx++) {
            mActor_name_t* fg_p = aMR_GetLayerTopFg(layer);
            const u8* cell;

            if (ftr_actor->items[idx] == EMPTY_NO || fg_p == NULL) {
                continue;
            }
            cell = (const u8*)&fg_p[ut_x + ut_z * UT_X_NUM];
            if (cell >= base && cell < base + sizeof(Save_t)) {
                cb((unsigned int)(cell - base), ftr_actor->items[idx], arg);
            }
        }
    }
}

// ...and which lamps and the like are on, as leaving saves it (the room clears that from the save while it's up)
void aMR_pc_switch_tables(void (*cb)(unsigned int save_off, unsigned long long bits, void* arg), void* arg) {
    const u8* base = (const u8*)&common_data.save.save;
    MY_ROOM_ACTOR* my_room;
    int layer;

    if (aMR_CLIP == NULL || (my_room = (MY_ROOM_ACTOR*)aMR_CLIP->my_room_actor_p) == NULL ||
        l_aMR_work.ftr_actor_list == NULL || l_aMR_work.used_list == NULL) {
        return;
    }
    for (layer = mCoBG_LAYER0; layer < mCoBG_LAYER2; layer++) {
        u64* table = aMR_GetBitSwitchTable(layer, my_room);
        mActor_name_t* fg_p = aMR_GetLayerTopFg(layer);
        u64 bits = 0;
        int ut_x;
        int ut_z;

        if (table == NULL || fg_p == NULL || (const u8*)table < base || (const u8*)table >= base + sizeof(Save_t)) {
            continue;
        }
        for (ut_z = aMR_MIN_BOUND; ut_z <= aMR_MAX_BOUND; ut_z++) {
            for (ut_x = aMR_MIN_BOUND; ut_x <= aMR_MAX_BOUND; ut_x++) {
                aMR_SaveOneFtrSwitchData(fg_p[ut_x + ut_z * UT_X_NUM], ut_x, ut_z, layer, &bits);
            }
        }
        cb((unsigned int)((const u8*)table - base), bits, arg);
    }
}
#endif

#ifdef VITA_MP
// that save cell is where a stereo in this room keeps its record: no item of its own (the music box has the record)
int aMR_mp_music_slot(unsigned int save_off) {
    const u8* base = (const u8*)&common_data.save.save;
    FTR_ACTOR* ftr_actor = l_aMR_work.ftr_actor_list;
    u8* used = l_aMR_work.used_list;
    int i;

    if (aMR_CLIP == NULL || aMR_CLIP->my_room_actor_p == NULL || ftr_actor == NULL || used == NULL) {
        return FALSE;
    }
    for (i = 0; i < l_aMR_work.list_size; i++, ftr_actor++, used++) {
        aFTR_PROFILE* profile;
        mActor_name_t* fg_p;
        int ut_x;
        int ut_z;
        int ut;

        if (!*used || ftr_actor->state == aFTR_STATE_BYE || (profile = aMR_GetFurnitureProfile(ftr_actor->name)) == NULL ||
            !aFTR_CHECK_INTERACTION(profile->interaction_type, aFTR_INTERACTION_TYPE_MUSIC_DISK) ||
            ftr_actor->layer + 1 >= mCoBG_LAYER_NUM || (fg_p = aMR_GetLayerTopFg(ftr_actor->layer + 1)) == NULL) {
            continue;
        }
        if (aMR_mp_cot_shared()) {
            ut = aMR_mp_main_ut(ftr_actor);
        } else if (aMR_Wpos2PlaceNumber(&ut_x, &ut_z, ftr_actor->position, ftr_actor->shape_type)) {
            ut = ut_x + ut_z * UT_X_NUM;
        } else {
            continue;
        }
        if (ut >= 0 && (const u8*)&fg_p[ut] == base + save_off) {
            return TRUE;
        }
    }
    return FALSE;
}
#endif

static void aMR_AllFurnitureDestruct(ACTOR* actorx, GAME* game) {
    FTR_ACTOR* ftr_actor = l_aMR_work.ftr_actor_list;
    u8* used = l_aMR_work.used_list;
    int i;

    sAdo_RhythmAllStop();
    if (ftr_actor != NULL && used != NULL) {
        for (i = 0; i < l_aMR_work.list_size; i++) {
            if (*used) {
                aFTR_PROFILE* profile = aMR_GetFurnitureProfile(ftr_actor->name);

                mCoBG_CrossOffMoveBg(ftr_actor->move_bg_idx);
                aMR_KeepItem2Fg(ftr_actor);
                aMR_MiniDiskCommonDt(ftr_actor, actorx);
                aMR_RadioCommonDt(ftr_actor, actorx);
                aMR_MinusWeight(actorx, ftr_actor);

                if (profile != NULL && profile->vtable != NULL && profile->vtable->dt_proc != NULL) {
                    profile->vtable->dt_proc(ftr_actor, aMR_FtrNo2BankAddress(ftr_actor->name));
                }

                *used = FALSE;
            }

            used++;
            ftr_actor++;
        }
    }
}

static void aMR_LeafPickuped(void) {
    if (aMR_CLIP != NULL) {
        ACTOR* actorx = aMR_CLIP->my_room_actor_p;

        if (actorx != NULL) {
            MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;

            my_room->pickup_info.pickup_flag = FALSE;
        }
    }
}

static int aMR_PickupFtrLayer(void) {
    if (aMR_CLIP != NULL) {
        ACTOR* actorx = aMR_CLIP->my_room_actor_p;

        if (actorx != NULL) {
            MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;

            return my_room->pickup_info.layer;
        }
    }

    return mCoBG_LAYER0;
}

#include "../src/actor/ac_my_room_goki.c_inc"

static void aMR_LeafStartPos(xyz_t* pos) {
    static xyz_t leaf_start0 = { 0.0f, 0.0f, 0.0f };

    *pos = leaf_start0;
    if (aMR_CLIP != NULL) {
        ACTOR* actorx = aMR_CLIP->my_room_actor_p;

        if (actorx != NULL) {
            MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;

            *pos = my_room->pickup_info.leaf_pos;
        }
    }
}

static int aMR_Ftr2Leaf(void) {
    if (aMR_CLIP != NULL) {
        ACTOR* actorx = aMR_CLIP->my_room_actor_p;

        if (actorx != NULL) {
            MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;

            return my_room->pickup_info.picking_up_flag;
        }
    }

    return FALSE;
}

static int aMR_SetLeaf(const xyz_t* pos, f32 scale) {
    if (aMR_CLIP != NULL) {
        ACTOR* actorx = aMR_CLIP->my_room_actor_p;

        if (actorx != NULL) {
            MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;

            if (my_room->leaf_info.exist_flag == FALSE) {
                my_room->leaf_info.pos = *pos;
                my_room->leaf_info.scale = scale;
                my_room->leaf_info.exist_flag = TRUE;
                return TRUE;
            }

            return FALSE;
        }
    }

    return FALSE;
}

static void My_Room_Actor_dt(ACTOR* actorx, GAME* game) {
    MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;

#ifdef VITA_MP
    // (a visitor's lamps go to the host as it leaves; its copy of the town keeps nothing of them)
    if (my_room->scene == SCENE_COTTAGE_MY) {
        mp_cot_room_leave();
    }
    if (!(aMR_mp_cot_shared() && mp_is_guest()))
#endif
    aMR_SaveSwitchData(my_room);
    aMR_AllFurnitureDestruct(actorx, game);
    aMR_FreeHeapArea(actorx);
    aMR_GokiInfoDt();
    aMR_CLIP = NULL;
    mCkRh_SetGoingOutCottageTime(my_room->scene);

    /* TODO: enums for this field */
    if ((Common_Get(my_room_message_control_flags) >> 3) & 1) {
        Common_Set(my_room_message_control_flags, 0);
        Common_Get(my_room_message_control_flags) &= ~2;
        Common_Get(my_room_message_control_flags) &= ~1;
        Common_Get(my_room_message_control_flags) |= 8;
        sAdo_SubGameStart();
    } else if ((Common_Get(my_room_message_control_flags) & 0x10)) {
        Common_Set(my_room_message_control_flags, 0);
        Common_Get(my_room_message_control_flags) &= ~2;
        Common_Get(my_room_message_control_flags) &= ~1;
        Common_Get(my_room_message_control_flags) |= 0x10;
        sAdo_SubGameStart();
    } else {
        Common_Set(my_room_message_control_flags, 0);
        Common_Get(my_room_message_control_flags) &= ~2;
        Common_Get(my_room_message_control_flags) &= ~1;
    }
}

#ifdef TARGET_VITA
// must be visible to My_Room_Actor_move (defined in ac_my_room_move.c_inc).
static int s_vita_nes_picker_ftrID = -1;
#endif

#ifdef VITA_MP
// a stand-in leaves at once and quietly, the way the room drops furniture on leaving (no pick-up puff or sound)
static void aMR_mp_fossil_drop(ACTOR* actorx, int ftrID) {
    FTR_ACTOR* ftr_actor = &l_aMR_work.ftr_actor_list[ftrID];
    aFTR_PROFILE* profile = aMR_GetFurnitureProfile(ftr_actor->name);
    int ut_x;
    int ut_z;

    aMR_SetFurniture2FG(ftr_actor, ftr_actor->position, FALSE);
    if (aMR_Wpos2PlaceNumber(&ut_x, &ut_z, ftr_actor->position, ftr_actor->shape_type)) {
        aMR_SetInfoFurnitureTable(ftr_actor->shape_type, ut_x + ut_z * UT_X_NUM, aMR_NO_FTR_ID, ftr_actor->layer);
    }
    mCoBG_CrossOffMoveBg(ftr_actor->move_bg_idx);
    aMR_MinusWeight(actorx, ftr_actor);
    if (profile != NULL && profile->vtable != NULL && profile->vtable->dt_proc != NULL) {
        profile->vtable->dt_proc(ftr_actor, aMR_FtrNo2BankAddress(ftr_actor->name));
    }

    l_aMR_work.used_list[ftrID] = FALSE;
    aMR_DeleteFurnitureBank(ftr_actor->name);
    aMR_ClearBitSwitch(ftr_actor);
    if (ftrID < aMR_MP_FTR_MAX) {
        aMR_mp_remote_demo[ftrID] = FALSE;
        aMR_mp_glide_left[ftrID] = 0;
    }
}

// a part donated while this screen is in the fossil room goes up in its stand-in's place, as entering would show it
static void aMR_mp_fossil_refresh(ACTOR* actorx, GAME* game) {
    MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;
    GAME_PLAY* play = (GAME_PLAY*)game;
    int i;

    if (my_room->scene != SCENE_MUSEUM_ROOM_FOSSIL || !mp_active() ||
        mem_cmp(aMR_mp_fossil_bit, Save_Get(museum_display).fossil_bit, sizeof(aMR_mp_fossil_bit))) {
        return;
    }

    // not under a talk, a door or a menu, nor mid furniture action: the old record keeps it pending
    if (mDemo_CheckDemo() || my_room->state != 0 || play->submenu.process_status != mSM_PROCESS_WAIT ||
        l_aMR_work.ftr_actor_list == NULL || l_aMR_work.used_list == NULL) {
        return;
    }

    mem_copy(aMR_mp_fossil_bit, Save_Get(museum_display).fossil_bit, sizeof(aMR_mp_fossil_bit));
    aMR_mp_replaying = TRUE; // each screen makes this swap itself: nothing goes out
    for (i = 0; i < mMmd_FOSSIL_NUM; i++) {
        mActor_name_t item;
        int ut_x;
        int ut_z;
        int ftrID;

        if (mMmd_FossilInfo(i) == mMmd_DONATOR_NONE || !mMmd_mp_fossil_unit(i, &ut_x, &ut_z, &item)) {
            continue;
        }

        if (aMR_UnitNum2FtrItemNoFtrID(NULL, &ftrID, ut_x, ut_z, mCoBG_LAYER0) && l_aMR_work.used_list[ftrID]) {
            if (l_aMR_work.ftr_actor_list[ftrID].name == mRmTp_FtrItemNo2FtrIdx(item)) {
                continue; // up already
            }

            aMR_mp_fossil_drop(actorx, ftrID);
        }

        aMR_MakeOneFurniture(actorx, item, game, ut_x, ut_z, mCoBG_LAYER0);
    }
    aMR_mp_replaying = FALSE;
}
#endif

#include "../src/actor/ac_my_room_melody.c_inc"
#include "../src/actor/ac_my_room_move.c_inc"
#include "../src/actor/ac_my_room_draw.c_inc"

#ifdef VITA_MP
// the same furniture on this screen: same slot and name, or the nearest of that name
static FTR_ACTOR* aMR_mp_find(s16 id, u16 name, f32 x, f32 z) {
    FTR_ACTOR* best = NULL;
    f32 best_d = 60.0f * 60.0f;
    int i;

    if (l_aMR_work.ftr_actor_list == NULL || l_aMR_work.used_list == NULL) {
        return NULL;
    }
    for (i = 0; i < l_aMR_work.list_size; i++) {
        FTR_ACTOR* ftr_actor = &l_aMR_work.ftr_actor_list[i];
        f32 dx = ftr_actor->position.x - x;
        f32 dz = ftr_actor->position.z - z;
        f32 d;

        if (!l_aMR_work.used_list[i] || ftr_actor->name != name || ftr_actor->state == aFTR_STATE_BYE) {
            continue;
        }
        // (another player's hands still on it: that player's game has it where the drawing leads it)
        if (i < aMR_MP_FTR_MAX && (aMR_mp_hint_left[i] > 0 || aMR_mp_hint_hold[i] > 0)) {
            dx += aMR_mp_hint[i].x;
            dz += aMR_mp_hint[i].z;
        }
        d = dx * dx + dz * dz;
        if (i == id && d < best_d) {
            return ftr_actor;
        }
        if (d < best_d) {
            best_d = d;
            best = ftr_actor;
        }
    }
    return best;
}

// a stereo as another player left it: its record, and its music on or off
static void aMR_mp_set_record(ACTOR* actorx, FTR_ACTOR* ftr_actor, int on, mActor_name_t record, int how) {
    aFTR_PROFILE* profile = aMR_GetFurnitureProfile(ftr_actor->name);

    if (profile == NULL || !aFTR_CHECK_INTERACTION(profile->interaction_type, aFTR_INTERACTION_TYPE_MUSIC_DISK)) {
        return;
    }
    on = on && record >= ITM_MINIDISK_START && record < ITM_MINIDISK_END;
    if (ftr_actor->items[0] == record && (ftr_actor->switch_bit == TRUE) == on) {
        return;
    }
    ftr_actor->items[0] = record;
    if (on) {
        aMR_OneMDSwitchOn_TheOtherSwitchOff(ftr_actor);
        aMR_ReserveBgm(actorx, BGM_MD0 + (record - ITM_MINIDISK_START), ftr_actor, how == aMR_MP_REC_HAND ? 30 : 0);
        if (how == aMR_MP_REC_HAND) {
            sAdo_OngenTrgStart(NA_SE_LIGHT_ON, &ftr_actor->position);
        } else {
            aMR_ChangeMDBgm(actorx, ftr_actor);
        }
        return;
    }
    if (ftr_actor->switch_bit == TRUE) {
        ftr_actor->switch_bit = FALSE;
        ftr_actor->switch_changed_flag = TRUE;
        aMR_AllMDSwitchOff();
        aMR_ReserveDefaultBgm(actorx, ftr_actor);
        aMR_ChangeMDBgm(actorx, ftr_actor);
    }
    if (how == aMR_MP_REC_HAND) {
        sAdo_OngenTrgStart(NA_SE_LIGHT_OFF, &ftr_actor->position);
    } else if (how == aMR_MP_REC_GONE) {
        sAdo_OngenTrgStart(0x17, &ftr_actor->position);
    }
}

// host: another player switched a piece in a floor of a house this game isn't in: the floor keeps it, as its room
// would have on leaving (the piece's own unit is the one naming it nearest where that screen had it)
void aMR_mp_keep_switch(int scene, int field, const u8* body, int len) {
    int floor_no = mFI_GetPlayerHouseFloorNo(scene);
    mHm_flr_c* flr;
    u64* table = NULL;
    f32 best = 1e9f;
    int shift = 0;
    u16 name;
    f32 x;
    f32 z;
    int layer;

    if (mFI_GET_TYPE(field) != mFI_FIELD_PLAYER0_ROOM || floor_no < 0 || floor_no >= mHm_ROOM_NUM || len < 14) {
        return;
    }
    flr = &Save_Get(homes[(field - mFI_FIELD_PLAYER0_ROOM) & 3]).floors[floor_no];
    aMR_mp_get(&name, body + 3, 2);
    aMR_mp_get(&x, body + 5, 4);
    aMR_mp_get(&z, body + 9, 4);
    for (layer = mCoBG_LAYER0; layer < mCoBG_LAYER2; layer++) {
        mHm_lyr_c* lyr = &(&flr->layer_main)[layer];
        int ut_x;
        int ut_z;

        for (ut_z = aMR_MIN_BOUND; ut_z <= aMR_MAX_BOUND; ut_z++) {
            for (ut_x = aMR_MIN_BOUND; ut_x <= aMR_MAX_BOUND; ut_x++) {
                mActor_name_t item = lyr->items[ut_z][ut_x];
                f32 dx = (ut_x + 0.5f) * mFI_UT_WORLDSIZE_X_F - x;
                f32 dz = (ut_z + 0.5f) * mFI_UT_WORLDSIZE_Z_F - z;

                if (ITEM_IS_FTR(item) && mRmTp_FtrItemNo2FtrIdx(item) == name && dx * dx + dz * dz < best) {
                    best = dx * dx + dz * dz;
                    table = &lyr->ftr_switch;
                    shift = (ut_x - 1 + (ut_z - 1) * 8) & 0x3F;
                }
            }
        }
    }
    if (table != NULL) {
        *table = body[13] ? (*table | (1ull << shift)) : (*table & ~(1ull << shift));
    }
}

// a furniture slot neither in use nor waiting on a piece still to grow in
static int aMR_mp_free_slot(MY_ROOM_ACTOR* my_room) {
    int slot;
    int k;

    for (slot = 0; slot < l_aMR_work.list_size; slot++) {
        if (l_aMR_work.used_list[slot]) {
            continue;
        }
        for (k = 0; k < aMR_RSV_FTR_NUM; k++) {
            if (my_room->rsv_ftr[k].exist_flag && my_room->rsv_ftr[k].free_no == slot) {
                break;
            }
        }
        if (k == aMR_RSV_FTR_NUM) {
            return slot;
        }
    }
    return -1;
}

// that piece already stands (or grows in) there: the room came up with it
static int aMR_mp_birth_here(MY_ROOM_ACTOR* my_room, u16 ftr_no, int ut_x, int ut_z, int layer) {
    FTR_ACTOR* ftr_actor = l_aMR_work.ftr_actor_list;
    int i;

    for (i = 0; i < aMR_RSV_FTR_NUM; i++) {
        aMR_rsv_ftr_c* rsv = &my_room->rsv_ftr[i];

        if (rsv->exist_flag && rsv->ftr_name == ftr_no && rsv->ut_x == ut_x && rsv->ut_z == ut_z && rsv->layer == layer) {
            return TRUE;
        }
    }
    for (i = 0; i < l_aMR_work.list_size; i++, ftr_actor++) {
        int px;
        int pz;

        if (l_aMR_work.used_list[i] && ftr_actor->name == ftr_no && ftr_actor->layer == layer &&
            ftr_actor->state != aFTR_STATE_BYE && ftr_actor->state != aFTR_STATE_DEATH &&
            aMR_Wpos2PlaceNumber(&px, &pz, ftr_actor->position, ftr_actor->shape_type) && px == ut_x && pz == ut_z) {
            return TRUE;
        }
    }
    return FALSE;
}

int aMR_mp_replay(const u8* body, int len) {
    ACTOR* my_room_actorx = (aMR_CLIP != NULL) ? aMR_CLIP->my_room_actor_p : NULL;
    FTR_ACTOR* ftr_actor;
    s16 id;
    u16 name;
    f32 x;
    f32 z;
    int idx;

    aMR_mp_house_calm = 0;
    if (my_room_actorx == NULL || len < 10) {
        return TRUE;
    }
    // (a shared cottage's rooms take comings and goings from its cells; a move or turn only leads the drawing)
    if (aMR_mp_cot_shared() && (body[0] == MP_VFX_FTR_BIRTH || body[0] == MP_VFX_FTR_BYE)) {
        return TRUE;
    }
    if (body[0] == MP_VFX_FTR_BIRTH) {
        MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)my_room_actorx;
        u16 ftr_no;
        u16 rotation;
        int free_idx;
        int ok;

        aMR_mp_get(&ftr_no, body + 1, 2);
        aMR_mp_get(&rotation, body + 5, 2);
        if (ftr_no >= FTR_NUM || body[3] >= UT_X_NUM || body[4] >= UT_Z_NUM || rotation > 3 || body[7] > 3 ||
            body[8] > mCoBG_LAYER1 || aMR_mp_birth_here(my_room, ftr_no, body[3], body[4], body[8])) {
            return TRUE;
        }
        // (with every slot or reservation taken it waits for the pieces growing in now)
        if ((free_idx = aMR_mp_free_slot(my_room)) < 0) {
            return FALSE;
        }
        aMR_mp_replaying = TRUE;
        ok = aMR_ReserveFurniture(gamePT, ftr_no, free_idx, body[3], body[4], rotation, body[7], body[8]);
        aMR_mp_replaying = FALSE;
        return ok;
    }
    if (len < 13) {
        return TRUE;
    }
    aMR_mp_get(&id, body + 1, 2);
    aMR_mp_get(&name, body + 3, 2);
    aMR_mp_get(&x, body + 5, 4);
    aMR_mp_get(&z, body + 9, 4);
    ftr_actor = aMR_mp_find(id, name, x, z);
    if (ftr_actor == NULL) {
        return TRUE;
    }
    idx = ftr_actor - l_aMR_work.ftr_actor_list;
    if (idx < 0 || idx >= aMR_MP_FTR_MAX) {
        return TRUE;
    }
    switch (body[0]) {
        case MP_VFX_FTR_SWITCH:
            if (len >= 14 && ftr_actor->switch_bit != body[13]) {
                aFTR_PROFILE* profile = aMR_GetFurnitureProfile(ftr_actor->name);

                aMR_mp_replaying = TRUE;
                aMR_FtrIdx2ChangeFtrSwitch_main(my_room_actorx, idx);
                aMR_mp_replaying = FALSE;
                // (the sounds the switching player's room made at the piece)
                if (!aMR_mp_sound_ok()) {
                } else if (ftr_actor->name == FTR_IKE_K_OTOME01) {
                    sAdo_OngenTrgStart(NA_SE_166, &ftr_actor->position);
                } else if (profile != NULL &&
                           aFTR_CHECK_INTERACTION(profile->interaction_type, aFTR_INTERACTION_TYPE_TOGGLE)) {
                    sAdo_OngenTrgStart(ftr_actor->switch_bit ? NA_SE_LIGHT_ON : NA_SE_LIGHT_OFF, &ftr_actor->position);
                }
            }
            break;
        case MP_VFX_FTR_RECORD:
            if (len >= 17) {
                mActor_name_t record;

                aMR_mp_get(&record, body + 15, 2);
                aMR_mp_replaying = TRUE;
                aMR_mp_set_record(my_room_actorx, ftr_actor, body[13], record, body[14]);
                aMR_mp_replaying = FALSE;
            }
            break;
        case MP_VFX_FTR_OPEN:
            if (ftr_actor->demo_status == 0) {
                aMR_mp_remote_demo[idx] = TRUE;
                ftr_actor->demo_status = 1;
            }
            break;
        case MP_VFX_FTR_CLOSE:
            if (aMR_mp_remote_demo[idx] && ftr_actor->demo_status >= 1 && ftr_actor->demo_status <= 4) {
                ftr_actor->demo_status = 5;
            }
            break;
        case MP_VFX_FTR_MOVE:
            if (len >= 27 && aMR_mp_cot_shared()) {
                aMR_mp_hint_move(ftr_actor, body);
            } else if (len >= 27) {
                s16 frames;

                // (a push before it that hasn't finished here ends where it was going)
                if (aMR_mp_glide_left[idx] > 0) {
                    aMR_mp_glide_left[idx] = 0;
                    ftr_actor->position = aMR_mp_glide_to[idx];
                    aMR_mp_ride_end(my_room_actorx, ftr_actor);
                }
                aMR_mp_ride_start(my_room_actorx, ftr_actor);
                aMR_mp_get(&aMR_mp_glide_to[idx], body + 13, 12);
                aMR_mp_get(&frames, body + 25, 2);
                aMR_mp_glide_left[idx] = frames < 1 ? 1 : (frames > 120 ? 120 : frames);
                if (aMR_mp_sound_ok()) {
                    aMR_SetMoveSE(ftr_actor);
                }
            }
            break;
        case MP_VFX_FTR_BYE:
            aMR_mp_ride_drop(my_room_actorx, ftr_actor);
            aMR_mp_glide_left[idx] = 0;
            aMR_MiniDiskCommonDt(ftr_actor, my_room_actorx);
            aMR_RadioCommonDt(ftr_actor, my_room_actorx);
            aMR_ClearHaniwaSwitch(my_room_actorx, ftr_actor);
            aMR_FtrID2ExtinguishFurniture(idx);
            break;
        case MP_VFX_FTR_ROTATE:
            if (len >= 19 && aMR_mp_cot_shared()) {
                aMR_mp_hint_turn(ftr_actor, body);
            } else if (len >= 19 && body[14] < aFTR_SHAPE_TYPE_NUM) {
                // (a turn before the last one here has finished waits for it)
                if (ftr_actor->state == aFTR_STATE_RROTATE || ftr_actor->state == aFTR_STATE_LROTATE) {
                    return FALSE;
                }
                if (ftr_actor->state == aFTR_STATE_STOP) {
                    int ut_x;
                    int ut_z;

                    // (what's on it turns along, set down again as the turn ends)
                    aMR_mp_ride_start(my_room_actorx, ftr_actor);
                    aMR_mp_get(&ftr_actor->angle_y_target, body + 15, 4);
                    ftr_actor->shape_type = body[14];
                    ftr_actor->state = body[13] ? aFTR_STATE_RROTATE : aFTR_STATE_LROTATE;
                    if (aMR_Wpos2PlaceNumber(&ut_x, &ut_z, ftr_actor->position, ftr_actor->shape_type)) {
                        aMR_SetInfoFurnitureTable(ftr_actor->shape_type, ut_x + ut_z * UT_X_NUM, ftr_actor->id,
                                                  ftr_actor->layer);
                    }
                    aMR_SetFurniture2FG(ftr_actor, ftr_actor->position, TRUE);
                    if (aMR_mp_sound_ok()) {
                        aMR_SetRotateSE(ftr_actor);
                    }
                }
            }
            break;
    }
    return TRUE;
}

// the island's cottage shared in a session: the room follows the cells as the host has them. What they say changed is
// done here at once (the drawing eases into it); what this player is in the middle of is left be till it's done.

#define aMR_MP_PIECES (2 * UT_TOTAL_NUM / 4)
#define aMR_MP_EASE   0.7f

typedef struct {
    u16 name;
    u8 layer;
    u8 dir;
    u8 ut;
    s16 actor; // the piece standing for it here: -1 none, -2 left be for now
} aMR_mp_piece_c;

static int aMR_mp_ut_busy(const u8* busy, int ut) {
    int layer;

    for (layer = 0; layer < mCoBG_LAYER_NUM; layer++) {
        int c = layer * UT_TOTAL_NUM + ut;

        if ((busy[c >> 3] >> (c & 7)) & 1) {
            return TRUE;
        }
    }
    return FALSE;
}

static u8 aMR_mp_shape(u16 name, int dir) {
    aFTR_PROFILE* profile = aMR_GetFurnitureProfile(name);

    return profile->shape == aFTR_SHAPE_TYPEB_0 ? l_typeB0_table[dir & 3] : profile->shape;
}

// any of a piece's units the room here is to leave be
static int aMR_mp_units_busy(const u8* busy, int ut, u8 shape) {
    int units[4];
    int n = aMR_GetFurniturePoccessUnitNo(units, ut & 15, (ut >> 4) & 15, shape);
    int i;

    for (i = 0; i < n; i++) {
        if (units[i] >= 0 && units[i] < UT_TOTAL_NUM && aMR_mp_ut_busy(busy, units[i])) {
            return TRUE;
        }
    }
    return FALSE;
}

static int aMR_mp_actor_busy(const u8* busy, FTR_ACTOR* ftr_actor) {
    int ut_x;
    int ut_z;

    return aMR_Wpos2PlaceNumber(&ut_x, &ut_z, ftr_actor->position, ftr_actor->shape_type) &&
           aMR_mp_units_busy(busy, ut_x + ut_z * UT_X_NUM, ftr_actor->shape_type);
}

// the units a piece would take are free in this room
static int aMR_mp_free_for(int ut, u8 shape, int layer) {
    u8* place_table = aMR_GetLayerPlaceTable(layer);
    int units[4];
    int n = aMR_GetFurniturePoccessUnitNo(units, ut & 15, (ut >> 4) & 15, shape);
    int i;

    if (place_table == NULL || n == 0) {
        return FALSE;
    }
    for (i = 0; i < n; i++) {
        if (units[i] < 0 || units[i] >= UT_TOTAL_NUM || place_table[units[i]] != aMR_NO_FTR_ID) {
            return FALSE;
        }
    }
    return TRUE;
}

static int aMR_mp_carried(MY_ROOM_ACTOR* my_room, int id) {
    int i;

    for (i = 0; my_room->parent_ftr.ftrID != -1 && i < aMR_FIT_FTR_MAX; i++) {
        if (my_room->parent_ftr.fit_ftr_table[i].exist_flag && my_room->parent_ftr.fit_ftr_table[i].ftr_ID == id) {
            return TRUE;
        }
    }
    return FALSE;
}

// a table under this unit for a table-top piece
static int aMR_mp_on_table(int ut) {
    int id = aMR_place_table[0][ut];

    return id < l_aMR_work.list_size && l_aMR_work.used_list[id] &&
           aMR_layer_set_info[l_aMR_work.ftr_actor_list[id].name] == aFTR_SET_TYPE_SURFACE;
}

// how high a piece stands on its unit: the floor (not the tops of furniture there, which may be on their way out),
// or the top of the table under it
static f32 aMR_mp_stand_y(int layer, xyz_t pos, int ut) {
    if (layer == mCoBG_LAYER1 && aMR_mp_on_table(ut)) {
        FTR_ACTOR* under = &l_aMR_work.ftr_actor_list[aMR_place_table[0][ut]];

        return under->position.y + aMR_GetFurnitureProfile(under->name)->height;
    }
    return mCoBG_GetBgY_OnlyCenter_FromWpos2(pos, 0.0f);
}

// a piece's sound as the room catches up, a few a pass
static int aMR_mp_sound_ok(void) {
    static u32 frame;

    if (gamePT != NULL && frame != (u32)((GAME_PLAY*)gamePT)->game_frame) {
        frame = (u32)((GAME_PLAY*)gamePT)->game_frame;
        aMR_mp_sounds = 0;
    }
    return aMR_mp_sounds++ < 3;
}

// another player's storage left open with nobody's hands on it any more (they went, or warped away): it closes
static void aMR_mp_demo_watch(void) {
    int i;

    for (i = 0; i < l_aMR_work.list_size && i < aMR_MP_FTR_MAX; i++) {
        FTR_ACTOR* ftr_actor = &l_aMR_work.ftr_actor_list[i];

        if (!l_aMR_work.used_list[i] || !aMR_mp_remote_demo[i] || ftr_actor->demo_status < 1 ||
            ftr_actor->demo_status > 4 || aMR_mp_in_use(ftr_actor)) {
            aMR_mp_demo_idle[i] = 0;
        } else if (++aMR_mp_demo_idle[i] > 90) {
            aMR_mp_demo_idle[i] = 0;
            ftr_actor->demo_status = 5;
        }
    }
}

// the cells name a piece the player here has in hand elsewhere (it moved under the player): made once it's let go
static int aMR_mp_twin_busy(const aMR_mp_piece_c* p, const u8* kind, const s16* match, int size) {
    int i;

    for (i = 0; i < size; i++) {
        if (kind[i] == 2 && match[i] < 0 && l_aMR_work.ftr_actor_list[i].name == p->name &&
            l_aMR_work.ftr_actor_list[i].layer == p->layer) {
            return TRUE;
        }
    }
    return FALSE;
}

// the save under the room holds what the cells say (a value held off while the player was at it, or taken in behind a
// menu), but for what's kept in furniture here and items in the air
static void aMR_mp_raw_follow(const u16* truth, const u8* busy) {
    int i;

    for (i = 0; i < mCoBG_LAYER_NUM * UT_TOTAL_NUM; i++) {
        int layer = i / UT_TOTAL_NUM;
        int ut = i % UT_TOTAL_NUM;
        mActor_name_t* fg_p;

        if (((busy[i >> 3] >> (i & 7)) & 1) || (fg_p = aMR_GetLayerTopFg(layer)) == NULL || fg_p[ut] == truth[i] ||
            fg_p[ut] == RSV_NO || (layer > mCoBG_LAYER0 && aMR_mp_holds_cell(layer, ut))) {
            continue;
        }
        fg_p[ut] = truth[i];
    }
}

// another player's hands on it: the drawing starts on from where its last move leaves it (else from the cells)
static void aMR_mp_hint_start(int id) {
    if (aMR_mp_hint_left[id] == 0 && aMR_mp_hint_hold[id] == 0) {
        aMR_mp_hint[id].x = aMR_mp_hint[id].y = aMR_mp_hint[id].z = 0.0f;
        aMR_mp_hint_rot[id] = 0;
    }
}

// another player's push under way: the drawing leads the piece where it's going, ahead of its cells
static void aMR_mp_hint_move(FTR_ACTOR* ftr_actor, const u8* body) {
    int id = ftr_actor->id;
    xyz_t to;
    f32 at_x;
    f32 at_z;
    s16 frames;

    if (id < 0 || id >= aMR_MP_FTR_MAX || ftr_actor->state != aFTR_STATE_STOP) {
        return;
    }
    aMR_mp_get(&at_x, body + 5, 4);
    aMR_mp_get(&at_z, body + 9, 4);
    aMR_mp_get(&to, body + 13, 12);
    aMR_mp_get(&frames, body + 25, 2);
    // (a push goes a unit at a time)
    if (ABS(to.x - at_x) > 1.5f * mFI_UT_WORLDSIZE_X_F || ABS(to.z - at_z) > 1.5f * mFI_UT_WORLDSIZE_Z_F) {
        return;
    }
    aMR_mp_hint_start(id);
    aMR_mp_hint[id].x += to.x - at_x;
    aMR_mp_hint[id].z += to.z - at_z;
    aMR_mp_hint_left[id] = frames < 1 ? 1 : (frames > 120 ? 120 : frames);
    aMR_mp_hint_hold[id] = 90;
}

// ...and a turn, about the end it was taken hold of by: the other screen has the piece stand there
static void aMR_mp_hint_turn(FTR_ACTOR* ftr_actor, const u8* body) {
    int id = ftr_actor->id;
    xyz_t shift;
    f32 target;
    f32 at_x;
    f32 at_z;
    s16 turn;

    if (id < 0 || id >= aMR_MP_FTR_MAX || ftr_actor->state != aFTR_STATE_STOP) {
        return;
    }
    aMR_mp_get(&at_x, body + 5, 4);
    aMR_mp_get(&at_z, body + 9, 4);
    aMR_mp_get(&target, body + 15, 4);
    aMR_mp_hint_start(id);
    turn = (s16)(RAD2SHORT_ANGLE2(DEG2RAD(target)) - ftr_actor->s_angle_y - aMR_mp_hint_rot[id]);
    shift.x = ftr_actor->position.x + aMR_mp_hint[id].x - at_x;
    shift.y = 0.0f;
    shift.z = ftr_actor->position.z + aMR_mp_hint[id].z - at_z;
    if (ABS(shift.x) <= mFI_UT_WORLDSIZE_X_F && ABS(shift.z) <= mFI_UT_WORLDSIZE_Z_F) {
        xyz_t turned = shift;

        sMath_RotateY(&turned, SHORT2RAD_ANGLE2(turn));
        aMR_mp_hint[id].x += turned.x - shift.x;
        aMR_mp_hint[id].z += turned.z - shift.z;
    }
    aMR_mp_hint_rot[id] += turn;
    aMR_mp_hint_left[id] = 16;
    aMR_mp_hint_hold[id] = 90;
}

static void aMR_mp_ease(void) {
    int i;

    for (i = 0; i < l_aMR_work.list_size && i < aMR_MP_FTR_MAX; i++) {
        xyz_t* vis = &aMR_mp_vis[i];

        if (aMR_mp_hint_left[i] > 0) {
            f32 t = 1.0f / (f32)aMR_mp_hint_left[i];

            vis->x += (aMR_mp_hint[i].x - vis->x) * t;
            vis->y += (aMR_mp_hint[i].y - vis->y) * t;
            vis->z += (aMR_mp_hint[i].z - vis->z) * t;
            aMR_mp_vis_rot[i] += (s16)((f32)(s16)(aMR_mp_hint_rot[i] - aMR_mp_vis_rot[i]) * t);
            aMR_mp_hint_left[i]--;
            continue;
        }
        // (there already: it waits for the cells to say so while the other player still has it in hand, and a while
        // after, else it goes back)
        if (aMR_mp_hint_hold[i] > 0) {
            if (!l_aMR_work.used_list[i] || !aMR_mp_in_use(&l_aMR_work.ftr_actor_list[i])) {
                aMR_mp_hint_hold[i]--;
            }
            continue;
        }
        vis->x *= aMR_MP_EASE;
        vis->y *= aMR_MP_EASE;
        vis->z *= aMR_MP_EASE;
        if (ABS(vis->x) < 0.5f && ABS(vis->y) < 0.5f && ABS(vis->z) < 0.5f) {
            vis->x = vis->y = vis->z = 0.0f;
        }
        aMR_mp_vis_rot[i] = (s16)((int)aMR_mp_vis_rot[i] * 7 / 10);
    }
}

// the piece takes its new place and facing at once; the drawing eases over from where it stood
static void aMR_mp_reseat(FTR_ACTOR* ftr_actor, int ut, int dir) {
    xyz_t from = ftr_actor->position;
    xyz_t base = ftr_actor->base_position;
    s16 old_angle = ftr_actor->s_angle_y;
    int moved = aMR_mp_main_ut(ftr_actor) != ut;
    xyz_t pos;
    int id = ftr_actor->id;

    // (turned as it's drawn)
    sMath_RotateY(&base, DEG2RAD(ftr_actor->angle_y) +
                             ((id >= 0 && id < aMR_MP_FTR_MAX) ? SHORT2RAD_ANGLE2(aMR_mp_vis_rot[id]) : 0.0f));
    from.x += base.x;
    from.y += base.y;
    from.z += base.z;
    ftr_actor->base_position.x = 0.0f;
    ftr_actor->base_position.y = 0.0f;
    ftr_actor->base_position.z = 0.0f;
    aMR_SetFurnitureType(ftr_actor, dir);
    ftr_actor->angle_y = aMR_angle_table[dir & 3];
    ftr_actor->angle_y_target = ftr_actor->angle_y;
    ftr_actor->s_angle_y = RAD2SHORT_ANGLE2(DEG2RAD(ftr_actor->angle_y));
    aMR_UnitNumber2Position(&pos, ftr_actor->shape_type, ut & 15, ut >> 4);
    pos.y = aMR_mp_stand_y(ftr_actor->layer, pos, ut);
    // (a floor piece keeps its height: the floor's)
    if (ftr_actor->layer == mCoBG_LAYER0) {
        pos.y = ftr_actor->position.y;
    }
    ftr_actor->position = pos;
    ftr_actor->last_position = pos;
    ftr_actor->target_position = pos;
    aMR_SetInfoFurnitureTable(ftr_actor->shape_type, ut, id, ftr_actor->layer);
    if (id >= 0 && id < aMR_MP_FTR_MAX) {
        aMR_mp_hint_left[id] = 0;
        aMR_mp_hint_hold[id] = 0;
        aMR_mp_vis[id].x += from.x - pos.x;
        aMR_mp_vis[id].y += from.y - pos.y;
        aMR_mp_vis[id].z += from.z - pos.z;
        aMR_mp_vis_rot[id] += (s16)(old_angle - ftr_actor->s_angle_y);
    }
    if (!aMR_mp_sound_ok()) {
    } else if (moved) {
        aMR_SetMoveSE(ftr_actor);
    } else {
        aMR_SetRotateSE(ftr_actor);
    }
}

// gone from the cells: it goes here, tidied up the way leaving the room would, nothing written back
static void aMR_mp_drop(ACTOR* actorx, FTR_ACTOR* ftr_actor) {
    MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;
    int ut_x;
    int ut_z;

    aMR_mp_ride_drop(actorx, ftr_actor);

    aMR_MiniDiskCommonDt(ftr_actor, actorx);
    aMR_RadioCommonDt(ftr_actor, actorx);
    aMR_ClearHaniwaSwitch(actorx, ftr_actor);
    if (my_room->bgm_info.reserved_ftr_actor == ftr_actor) {
        my_room->bgm_info.reserve_flag = FALSE;
        my_room->bgm_info.reserved_ftr_actor = NULL;
    }
    if (aMR_Wpos2PlaceNumber(&ut_x, &ut_z, ftr_actor->position, ftr_actor->shape_type)) {
        aMR_SetInfoFurnitureTable(ftr_actor->shape_type, ut_x + ut_z * UT_X_NUM, aMR_NO_FTR_ID, ftr_actor->layer);
    }
    mCoBG_CrossOffMoveBg(ftr_actor->move_bg_idx);
    ftr_actor->move_bg_idx = -1;
    bzero(ftr_actor->items, sizeof(ftr_actor->items));
    ftr_actor->state = aFTR_STATE_BYE;
    ftr_actor->dust_timer = 2;
    if (ftr_actor->id >= 0 && ftr_actor->id < aMR_MP_FTR_MAX) {
        aMR_mp_quiet[ftr_actor->id] = TRUE;
        aMR_mp_remote_demo[ftr_actor->id] = FALSE;
        aMR_mp_glide_left[ftr_actor->id] = 0;
        aMR_mp_hint_left[ftr_actor->id] = 0;
        aMR_mp_hint_hold[ftr_actor->id] = 0;
    }
    if (aMR_mp_sound_ok()) {
        aMR_SetCleanUpFtrSE(ftr_actor->position);
    }
}

// new in the cells: it comes up here the way furniture set down does, nothing written (the cells have it)
static int aMR_mp_add(ACTOR* actorx, GAME* game, const aMR_mp_piece_c* p) {
    MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;
    mActor_name_t item = mRmTp_FtrIdx2FtrItemNo(p->name, p->dir);
    u8 shape = aMR_mp_shape(p->name, p->dir);
    PLAYER_ACTOR* player = GET_PLAYER_ACTOR((GAME_PLAY*)game);
    FTR_ACTOR* ftr_actor;
    int units[4];
    int n;
    int slot;
    int i;

    // (a table-top piece waits for its table)
    if (!aMR_mp_free_for(p->ut, shape, p->layer) || (p->layer == mCoBG_LAYER1 && !aMR_mp_on_table(p->ut))) {
        return FALSE;
    }
    if (!aMR_WeightPossible(actorx, shape)) {
        return FALSE;
    }
    for (slot = 0; slot < l_aMR_work.list_size && slot < aMR_MP_FTR_MAX; slot++) {
        int k;

        for (k = 0; k < aMR_RSV_FTR_NUM; k++) {
            if (my_room->rsv_ftr[k].exist_flag && my_room->rsv_ftr[k].free_no == slot) {
                break;
            }
        }
        if (!l_aMR_work.used_list[slot] && k == aMR_RSV_FTR_NUM) {
            break;
        }
    }
    if (slot >= l_aMR_work.list_size || slot >= aMR_MP_FTR_MAX || !aMR_GetFurnitureBank2(p->name, game, item)) {
        return FALSE;
    }
    ftr_actor = &l_aMR_work.ftr_actor_list[slot];
    l_aMR_work.used_list[slot] = TRUE;
    aMR_mp_nosave = TRUE;
    aMR_FurnitureCt(ftr_actor, game, p->ut & 15, p->ut >> 4, item, slot, aFTR_STATE_BIRTH, p->layer, TRUE);
    aMR_mp_nosave = FALSE;
    aMR_PlussWeight(actorx, ftr_actor);
    bzero(ftr_actor->items, sizeof(ftr_actor->items));
    ftr_actor->position.y = aMR_mp_stand_y(p->layer, ftr_actor->position, p->ut);
    ftr_actor->last_position = ftr_actor->position;
    aMR_mp_quiet[slot] = TRUE;
    aMR_mp_vis[slot].x = aMR_mp_vis[slot].y = aMR_mp_vis[slot].z = 0.0f;
    aMR_mp_vis_rot[slot] = 0;
    aMR_mp_hint_left[slot] = 0;
    aMR_mp_hint_hold[slot] = 0;
    // (a player where it comes up is eased out of it, as by furniture of their own)
    n = aMR_GetFurniturePoccessUnitNo(units, p->ut & 15, p->ut >> 4, shape);
    if (player != NULL && p->layer == mCoBG_LAYER0) {
        int px;
        int pz;

        aMR_Wpos2PlaceNumber(&px, &pz, player->actor_class.world.position, 0);
        for (i = 0; i < n; i++) {
            if (units[i] == px + pz * UT_X_NUM) {
                ftr_actor->collision_scale = 0.6f;
            }
        }
    }
    if (aMR_mp_sound_ok()) {
        sAdo_OngenTrgStart(NA_SE_ITEM_HORIDASHI, &ftr_actor->position);
    }
    return TRUE;
}

// a stereo's record changed by another hand: its music follows if it's playing
static void aMR_mp_record(ACTOR* actorx, FTR_ACTOR* ftr_actor, mActor_name_t record) {
    if (!ftr_actor->switch_bit) {
        return;
    }
    if (record >= ITM_MINIDISK_START && record < ITM_MINIDISK_END) {
        aMR_ReserveBgm(actorx, BGM_MD0 + (record - ITM_MINIDISK_START), ftr_actor, 0);
    } else {
        ftr_actor->switch_bit = FALSE;
        ftr_actor->switch_changed_flag = TRUE;
        aMR_AllMDSwitchOff();
        aMR_ReserveDefaultBgm(actorx, ftr_actor);
        aMR_ChangeMDBgm(actorx, ftr_actor);
    }
}

// what a piece holds, as the cells have it over its units; the save under it stays as a room keeps it (empty while
// its things are in the furniture)
static void aMR_mp_contents(ACTOR* actorx, FTR_ACTOR* ftr_actor, const u16* truth) {
    aFTR_PROFILE* profile = aMR_GetFurnitureProfile(ftr_actor->name);
    int units[4];
    int n;
    int main_ut;
    int layer;
    int idx;
    int i;

    if (profile == NULL || !aFTR_IS_STORAGE(profile) || (main_ut = aMR_mp_main_ut(ftr_actor)) < 0) {
        return;
    }
    n = aMR_PosType2FurniturePoccessUnitNo(units, &ftr_actor->position, ftr_actor->shape_type);
    for (layer = ftr_actor->layer + 1, idx = 0; layer < mCoBG_LAYER_NUM; layer++, idx++) {
        mActor_name_t* fg_p = aMR_GetLayerTopFg(layer);
        mActor_name_t item = truth[layer * UT_TOTAL_NUM + main_ut];

        // (things the save kept over another of its units, as a room coming up takes them in)
        for (i = 0; i < n && item == EMPTY_NO; i++) {
            item = truth[layer * UT_TOTAL_NUM + units[i]];
        }
        if (ftr_actor->items[idx] != item) {
            if (idx == 0 && aFTR_CHECK_INTERACTION(profile->interaction_type, aFTR_INTERACTION_TYPE_MUSIC_DISK)) {
                aMR_mp_record(actorx, ftr_actor, item);
            }
            ftr_actor->items[idx] = item;
        }
        for (i = 0; i < n && fg_p != NULL; i++) {
            fg_p[units[i]] = EMPTY_NO;
        }
    }
}

static void aMR_mp_follow_start(void) {
    const u16* truth = mp_cot_truth(NULL);

    aMR_mp_retry = FALSE;
    aMR_mp_retry_wait = 0;
    aMR_mp_house_calm = 0;
    if (truth != NULL) {
        mem_copy((u8*)aMR_mp_shadow, (u8*)truth, sizeof(aMR_mp_shadow));
    } else {
        bzero(aMR_mp_shadow, sizeof(aMR_mp_shadow));
    }
}

// two players took hold of the same piece at once: the one who gives way lets go before anything moves (in a house,
// its owner lets go of a piece another player sat or lay down on)
static void aMR_mp_give_way(MY_ROOM_ACTOR* my_room) {
    PLAYER_ACTOR* player = GET_PLAYER_ACTOR((GAME_PLAY*)gamePT);
    FTR_ACTOR* ftr_actor;
    int units[4];
    int n;
    int i;

    if (player == NULL || player->now_main_index != mPlayer_INDEX_HOLD || my_room->demo_flag ||
        my_room->force_open_demo_flag || (my_room->state != 1 && my_room->state != 6 && my_room->state != 7 &&
                                          my_room->state != 8)) {
        return;
    }
    i = player->main_data.hold.ftr_no;
    if (i < 0 || i >= l_aMR_work.list_size || !l_aMR_work.used_list[i]) {
        return;
    }
    ftr_actor = &l_aMR_work.ftr_actor_list[i];
    n = aMR_PosType2FurniturePoccessUnitNo(units, &ftr_actor->position, ftr_actor->shape_type);
    for (i = 0; i < n; i++) {
        if (aMR_mp_cot_shared() ? mp_cot_must_yield(units[i]) : mp_player_on_unit(units[i])) {
            my_room->state = 0;
            my_room->push_timer = 0;
            my_room->pull_timer = 0;
            my_room->keep_push_flag = FALSE;
            my_room->keep_pull_flag = FALSE;
            return;
        }
    }
}

// a visitor's house room with something of the owner's under way: a piece gliding, turning, growing in or open, or
// word of one just in (the owner's floor, which comes slower, is taken on once all is still)
static int aMR_mp_house_moving(MY_ROOM_ACTOR* my_room) {
    int i;

    if (aMR_mp_house_calm < 90 || my_room->parent_ftr.ftrID != -1) {
        return TRUE;
    }
    for (i = 0; i < aMR_RSV_FTR_NUM; i++) {
        if (my_room->rsv_ftr[i].exist_flag) {
            return TRUE;
        }
    }
    for (i = 0; i < l_aMR_work.list_size && i < aMR_MP_FTR_MAX; i++) {
        if (l_aMR_work.used_list[i] && (aMR_mp_glide_left[i] > 0 || aMR_mp_remote_demo[i] ||
                                        l_aMR_work.ftr_actor_list[i].state != aFTR_STATE_STOP)) {
            return TRUE;
        }
    }
    return FALSE;
}

void aMR_mp_reconcile(void) {
    static const u8 no_busy[(mCoBG_LAYER_NUM * UT_TOTAL_NUM + 7) / 8];
    ACTOR* actorx = (aMR_CLIP != NULL) ? aMR_CLIP->my_room_actor_p : NULL;
    MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;
    FTR_ACTOR* list = l_aMR_work.ftr_actor_list;
    u8* used = l_aMR_work.used_list;
    int size = l_aMR_work.list_size < aMR_MP_FTR_MAX ? l_aMR_work.list_size : aMR_MP_FTR_MAX;
    aMR_mp_piece_c pcs[aMR_MP_PIECES];
    s16 match[aMR_MP_FTR_MAX];
    u8 kind[aMR_MP_FTR_MAX]; // 0 the room's to change, 1 going or gone, 2 left be, 3 moving
    const u16* truth;
    const u8* busy;
    int resync = FALSE;
    int npc = 0;
    int deferred = FALSE;
    int layer;
    int i;
    int j;

    if (my_room == NULL || list == NULL || used == NULL) {
        return;
    }
    if (my_room->scene != SCENE_COTTAGE_MY) {
        // a house: its owner (the host) lets go of what another player sits on; a visitor's room follows the owner's
        // floor
        if (!aMR_mp_house()) {
            return;
        }
        aMR_mp_sounds = 0;
        if (mp_is_host()) {
            aMR_mp_give_way(my_room);
            return;
        }
        aMR_mp_ease();
        if (aMR_mp_house_calm < 0x7FFF) {
            aMR_mp_house_calm++;
        }
        if ((truth = mp_house_truth()) == NULL || aMR_mp_house_moving(my_room)) {
            return;
        }
        busy = no_busy;
    } else {
        aMR_mp_ease();
        if ((truth = mp_cot_truth(&busy)) == NULL) {
            return;
        }
        aMR_mp_demo_watch();
        aMR_mp_give_way(my_room);
        resync = mp_cot_take_resync();
    }
    for (i = 0; i < mCoBG_LAYER_NUM * UT_TOTAL_NUM && truth[i] == aMR_mp_shadow[i]; i++) {
    }
    if (i == mCoBG_LAYER_NUM * UT_TOTAL_NUM && !resync && (!aMR_mp_retry || ++aMR_mp_retry_wait < 10)) {
        return;
    }
    mem_copy((u8*)aMR_mp_shadow, (u8*)truth, sizeof(aMR_mp_shadow));
    aMR_mp_retry = FALSE;
    aMR_mp_retry_wait = 0;
    aMR_mp_sounds = 0;

    // the furniture the cells name, floors first
    for (i = 0; i < 2 * UT_TOTAL_NUM && npc < aMR_MP_PIECES; i++) {
        mActor_name_t v = truth[i];

        if (ITEM_IS_FTR(v) && mRmTp_FtrItemNo2FtrIdx(v) < FTR_NUM) {
            pcs[npc].name = mRmTp_FtrItemNo2FtrIdx(v);
            pcs[npc].layer = (u8)(i / UT_TOTAL_NUM);
            pcs[npc].dir = (u8)FTR_GET_ROTATION(v);
            pcs[npc].ut = (u8)(i % UT_TOTAL_NUM);
            pcs[npc].actor = -1;
            npc++;
        }
    }
    // ...and what stands here
    for (i = 0; i < size; i++) {
        FTR_ACTOR* ftr_actor = &list[i];

        match[i] = -1;
        if (!used[i] || ftr_actor->state == aFTR_STATE_BYE || ftr_actor->state == aFTR_STATE_DEATH) {
            kind[i] = 1;
        } else if (aMR_mp_carried(my_room, i) || aMR_mp_actor_busy(busy, ftr_actor) || aMR_mp_main_ut(ftr_actor) < 0) {
            kind[i] = 2;
        } else {
            kind[i] = 0;
        }
    }
    // the same piece in the same place
    for (j = 0; j < npc; j++) {
        for (i = 0; i < size; i++) {
            FTR_ACTOR* ftr_actor = &list[i];

            if (kind[i] != 1 && match[i] < 0 && ftr_actor->name == pcs[j].name && ftr_actor->layer == pcs[j].layer &&
                aMR_mp_main_ut(ftr_actor) == pcs[j].ut &&
                (aMR_GetSaveAngle(ftr_actor->angle_y_target, 0) & 3) == pcs[j].dir) {
                match[i] = (s16)j;
                pcs[j].actor = (s16)i;
                break;
            }
        }
    }
    // what this player is in the middle of stays as it is for now
    for (j = 0; j < npc; j++) {
        if (pcs[j].actor == -1 && aMR_mp_units_busy(busy, pcs[j].ut, aMR_mp_shape(pcs[j].name, pcs[j].dir))) {
            pcs[j].actor = -2;
            deferred = TRUE;
        }
    }
    for (i = 0; i < size; i++) {
        deferred |= kind[i] == 2 && match[i] < 0;
    }
    // the same piece moved or turned: the nearest of its kind
    for (j = 0; j < npc; j++) {
        int best = -1;
        int best_d = 1000;

        for (i = 0; pcs[j].actor == -1 && i < size; i++) {
            FTR_ACTOR* ftr_actor = &list[i];
            int m;
            int d;

            if (kind[i] != 0 || match[i] >= 0 || ftr_actor->name != pcs[j].name || ftr_actor->layer != pcs[j].layer) {
                continue;
            }
            m = aMR_mp_main_ut(ftr_actor);
            d = ABS((m & 15) - (pcs[j].ut & 15)) + ABS((m >> 4) - (pcs[j].ut >> 4));
            if (d < best_d) {
                best = i;
                best_d = d;
            }
        }
        if (best >= 0) {
            match[best] = (s16)j;
            pcs[j].actor = (s16)best;
        }
    }
    // gone from the cells
    for (i = 0; i < size; i++) {
        if (kind[i] == 0 && match[i] < 0) {
            aMR_mp_drop(actorx, &list[i]);
            kind[i] = 1;
        }
    }
    // moved or turned: all of them leave their old units first
    for (i = 0; i < size; i++) {
        FTR_ACTOR* ftr_actor = &list[i];
        int ut_x;
        int ut_z;

        if (kind[i] != 0 || match[i] < 0 ||
            (aMR_mp_main_ut(ftr_actor) == pcs[match[i]].ut &&
             (aMR_GetSaveAngle(ftr_actor->angle_y_target, 0) & 3) == pcs[match[i]].dir)) {
            continue;
        }
        if (aMR_Wpos2PlaceNumber(&ut_x, &ut_z, ftr_actor->position, ftr_actor->shape_type)) {
            aMR_SetInfoFurnitureTable(ftr_actor->shape_type, ut_x + ut_z * UT_X_NUM, aMR_NO_FTR_ID, ftr_actor->layer);
        }
        kind[i] = 3;
    }
    // then the floor's take their new places and new ones come, then the table-tops' on what's under them now (one in
    // the way of something left be goes, and comes back once it can; one the player has in hand elsewhere isn't made
    // twice)
    for (layer = mCoBG_LAYER0; layer <= mCoBG_LAYER1; layer++) {
        for (i = 0; i < size; i++) {
            aMR_mp_piece_c* p;

            if (kind[i] != 3 || list[i].layer != layer) {
                continue;
            }
            p = &pcs[match[i]];
            if (aMR_mp_free_for(p->ut, aMR_mp_shape(p->name, p->dir), p->layer) &&
                (layer == mCoBG_LAYER0 || aMR_mp_on_table(p->ut))) {
                aMR_mp_reseat(&list[i], p->ut, p->dir);
                kind[i] = 0;
            } else {
                aMR_mp_drop(actorx, &list[i]);
                p->actor = -1;
                match[i] = -1;
                kind[i] = 1;
                deferred = TRUE;
            }
        }
        for (j = 0; j < npc; j++) {
            if (pcs[j].layer == layer && pcs[j].actor == -1 &&
                (aMR_mp_twin_busy(&pcs[j], kind, match, size) || !aMR_mp_add(actorx, gamePT, &pcs[j]))) {
                deferred = TRUE;
            }
        }
    }
    // what storage holds
    for (i = 0; i < size; i++) {
        FTR_ACTOR* ftr_actor = &list[i];

        int main_ut = aMR_mp_main_ut(ftr_actor);

        if (used[i] && ftr_actor->state != aFTR_STATE_BYE && ftr_actor->state != aFTR_STATE_DEATH &&
            !aMR_mp_carried(my_room, i) && main_ut >= 0 && !aMR_mp_ut_busy(busy, main_ut)) {
            aMR_mp_contents(actorx, ftr_actor, truth);
        }
    }
    aMR_mp_raw_follow(truth, busy);
    aMR_mp_retry = deferred;
}

// what the local player is in the middle of in the cottage: cells others keep off, those whose changes wait till it's
// done (what it only uses goes out as it happens), and the units whose furniture it has in hand
static u8* aMR_mp_lk_cells;
static u8* aMR_mp_lk_hold;
static u8* aMR_mp_lk_units;
static int aMR_mp_lk_use;

static void aMR_mp_lock_ut(int ut, int pad) {
    int x0 = ut & 15;
    int z0 = (ut >> 4) & 15;
    int dx;
    int dz;
    int layer;

    if (ut < 0 || ut >= UT_TOTAL_NUM) {
        return;
    }
    for (dz = -pad; dz <= pad; dz++) {
        for (dx = -pad; dx <= pad; dx++) {
            int x = x0 + dx;
            int z = z0 + dz;

            for (layer = 0; x >= 0 && x < UT_X_NUM && z >= 0 && z < UT_Z_NUM && layer < mCoBG_LAYER_NUM; layer++) {
                int c = layer * UT_TOTAL_NUM + x + z * UT_X_NUM;

                aMR_mp_lk_cells[c >> 3] |= (u8)(1 << (c & 7));
                if (!aMR_mp_lk_use) {
                    aMR_mp_lk_hold[c >> 3] |= (u8)(1 << (c & 7));
                }
            }
        }
    }
    aMR_mp_lk_units[ut >> 3] |= (u8)(1 << (ut & 7));
}

// the units a piece stands on, and those a push or pull under way takes it to
static int aMR_mp_ftr_units(FTR_ACTOR* ftr_actor, int* units) {
    int n = aMR_PosType2FurniturePoccessUnitNo(units, &ftr_actor->position, ftr_actor->shape_type);

    if (ftr_actor->state >= aFTR_STATE_WAIT_PUSH && ftr_actor->state <= aFTR_STATE_PULL) {
        n += aMR_PosType2FurniturePoccessUnitNo(units + n, &ftr_actor->target_position, ftr_actor->shape_type);
    }
    return n;
}

static void aMR_mp_lock_ftr(int id, int pad) {
    int units[8];
    int n;
    int i;

    if (id < 0 || id >= l_aMR_work.list_size || !l_aMR_work.used_list[id]) {
        return;
    }
    n = aMR_mp_ftr_units(&l_aMR_work.ftr_actor_list[id], units);
    for (i = 0; i < n; i++) {
        aMR_mp_lock_ut(units[i], pad);
    }
}

static void aMR_mp_session_add(FTR_ACTOR* ftr_actor) {
    int units[8];
    int n = aMR_mp_ftr_units(ftr_actor, units);
    int i;

    for (i = 0; i < n; i++) {
        if (units[i] >= 0 && units[i] < UT_TOTAL_NUM) {
            aMR_mp_session[units[i] >> 3] |= (u8)(1 << (units[i] & 7));
        }
    }
}

static int aMR_mp_session_has(FTR_ACTOR* ftr_actor) {
    int units[8];
    int n = aMR_mp_ftr_units(ftr_actor, units);
    int i;

    for (i = 0; i < n; i++) {
        if (units[i] >= 0 && units[i] < UT_TOTAL_NUM && ((aMR_mp_session[units[i] >> 3] >> (units[i] & 7)) & 1)) {
            return TRUE;
        }
    }
    return FALSE;
}

// the piece in the player's hands, from taking hold of it till it's let go
static int aMR_mp_in_hand(MY_ROOM_ACTOR* my_room, PLAYER_ACTOR* player) {
    int id = -1;

    if (player != NULL) {
        switch (player->now_main_index) {
            case mPlayer_INDEX_HOLD:
                id = player->main_data.hold.ftr_no;
                break;
            case mPlayer_INDEX_PUSH:
                id = player->main_data.push.ftr_no;
                break;
            case mPlayer_INDEX_PULL:
                id = player->main_data.pull.ftr_no;
                break;
            case mPlayer_INDEX_ROTATE_FURNITURE:
                id = player->main_data.rotate_furniture.ftr_no;
                break;
        }
    }
    if (id < 0 && my_room->parent_ftr.ftrID != -1) {
        id = my_room->parent_ftr.ftrID;
    }
    if (id < 0 && my_room->state != 0 && my_room->contact0.contact_flag && !my_room->demo_flag) {
        id = my_room->contact0.ftrID;
    }
    return (id >= 0 && id < l_aMR_work.list_size && l_aMR_work.used_list[id]) ? id : -1;
}

static void aMR_mp_lock_pos(xyz_t pos, int under) {
    int ut_x;
    int ut_z;

    if (aMR_Wpos2PlaceNumber(&ut_x, &ut_z, pos, 0)) {
        if (under) {
            aMR_mp_lock_ftr(aMR_place_table[0][ut_x + ut_z * UT_X_NUM], 0);
        } else {
            aMR_mp_lock_ut(ut_x + ut_z * UT_X_NUM, 0);
        }
    }
}

void aMR_mp_locks(unsigned char* cells, unsigned char* hold, unsigned char* units) {
    ACTOR* actorx = (aMR_CLIP != NULL) ? aMR_CLIP->my_room_actor_p : NULL;
    MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;
    FTR_ACTOR* list = l_aMR_work.ftr_actor_list;
    u8* used = l_aMR_work.used_list;
    PLAYER_ACTOR* player;
    int requested;
    int i;

    bzero(cells, mCoBG_LAYER_NUM * UT_TOTAL_NUM / 8);
    bzero(hold, mCoBG_LAYER_NUM * UT_TOTAL_NUM / 8);
    bzero(units, UT_TOTAL_NUM / 8);
    if (my_room == NULL || my_room->scene != SCENE_COTTAGE_MY || list == NULL || used == NULL) {
        return;
    }
    aMR_mp_lk_cells = cells;
    aMR_mp_lk_hold = hold;
    aMR_mp_lk_units = units;
    aMR_mp_lk_use = FALSE;
    player = GET_PLAYER_ACTOR((GAME_PLAY*)gamePT);
    // the furniture in the player's hands: every unit it has stood on since it was taken hold of (what it carries on
    // top too), till it's let go and still, so all it did goes out at once
    i = aMR_mp_in_hand(my_room, player);
    if (aMR_mp_session_on && (i < 0 || i != aMR_mp_session_id)) {
        int moving = FALSE;
        int k;

        // (let go while it still moves, or on its way into the pockets: the player's till then, and furniture taken
        // hold of meanwhile goes out with it)
        for (k = 0; k < l_aMR_work.list_size; k++) {
            if (used[k] && list[k].state != aFTR_STATE_STOP && !(k < aMR_MP_FTR_MAX && aMR_mp_quiet[k]) &&
                aMR_mp_session_has(&list[k])) {
                aMR_mp_session_add(&list[k]);
                moving = TRUE;
            }
        }
        if (!moving) {
            bzero(aMR_mp_session, sizeof(aMR_mp_session));
            aMR_mp_session_on = FALSE;
        }
    }
    aMR_mp_session_id = i;
    if (i >= 0) {
        aMR_mp_session_add(&list[i]);
        aMR_mp_session_on = TRUE;
    }
    for (i = 0; aMR_mp_session_on && i < UT_TOTAL_NUM; i++) {
        if ((aMR_mp_session[i >> 3] >> (i & 7)) & 1) {
            aMR_mp_lock_ut(i, 0);
        }
    }
    // furniture coming or going by this player's hand
    for (i = 0; i < l_aMR_work.list_size; i++) {
        if (used[i] && list[i].state != aFTR_STATE_STOP && !(i < aMR_MP_FTR_MAX && aMR_mp_quiet[i])) {
            aMR_mp_lock_ftr(i, 0);
        }
    }
    for (i = 0; i < aMR_RSV_FTR_NUM; i++) {
        aMR_rsv_ftr_c* rsv = &my_room->rsv_ftr[i];
        int units[4];
        int n;
        int k;

        if (!rsv->exist_flag || aMR_mp_rsv_remote[i] || rsv->ftr_name >= FTR_NUM) {
            continue;
        }
        n = aMR_GetFurniturePoccessUnitNo(units, rsv->ut_x, rsv->ut_z, aMR_mp_shape(rsv->ftr_name, rsv->angle_idx));
        for (k = 0; k < n; k++) {
            aMR_mp_lock_ut(units[k], 0);
        }
    }
    // storage or a stereo open, the NES: others keep off, and what the player does there goes out as it happens
    aMR_mp_lk_use = TRUE;
    if (my_room->demo_flag || my_room->msg_type != aMR_MSG_STATE_NONE) {
        aMR_mp_lock_ftr(my_room->demo_ftrID, 0);
    }
    for (i = 0; i < l_aMR_work.list_size; i++) {
        if (used[i] && list[i].demo_status != 0 && !aMR_mp_remote(&list[i])) {
            aMR_mp_lock_ftr(i, 0);
        }
    }
    if (my_room->emu_info.request_flag) {
        aMR_mp_lock_ftr(my_room->emu_ftrID, 0);
    }
#ifdef TARGET_VITA
    aMR_mp_lock_ftr(s_vita_nes_picker_ftrID, 0);
#endif
    if (my_room->state != 0 && my_room->contact0.contact_flag) {
        aMR_mp_lock_ftr(my_room->contact0.ftrID, 0);
    }
    aMR_mp_lk_use = FALSE;
    // an item in flight, where it lands
    if (my_room->throw_item_lock_flag) {
        for (i = 0; i < 2 * UT_TOTAL_NUM; i++) {
            mActor_name_t* fg_p = aMR_GetLayerTopFg(i / UT_TOTAL_NUM);

            if (fg_p != NULL && fg_p[i % UT_TOTAL_NUM] == RSV_NO) {
                aMR_mp_lock_ut(i % UT_TOTAL_NUM, 0);
            }
        }
    }
    // the player's own seat or bed (used, as above), or pick-up
    if (player == NULL) {
        return;
    }
    aMR_mp_lk_use = TRUE;
    switch (player->now_main_index) {
        case mPlayer_INDEX_SITDOWN:
        case mPlayer_INDEX_SITDOWN_WAIT:
        case mPlayer_INDEX_STANDUP:
            aMR_mp_lock_pos(player->actor_class.world.position, TRUE);
            if (my_room->contact0.contact_flag) {
                aMR_mp_lock_ftr(my_room->contact0.ftrID, 0);
            }
            break;
        case mPlayer_INDEX_LIE_BED:
        case mPlayer_INDEX_WAIT_BED:
        case mPlayer_INDEX_ROLL_BED:
        case mPlayer_INDEX_STANDUP_BED:
            aMR_mp_lock_ftr(my_room->bed_ftr_actor_idx, 0);
            aMR_mp_lock_pos(player->actor_class.world.position, TRUE);
            break;
    }
    requested = player->requested_main_index_changed ? player->requested_main_index : -1;
    if (requested == mPlayer_INDEX_SITDOWN && my_room->contact0.contact_flag) {
        aMR_mp_lock_ftr(my_room->contact0.ftrID, 0);
    }
    aMR_mp_lk_use = FALSE;
    if (player->now_main_index == mPlayer_INDEX_PICKUP_FURNITURE || player->now_main_index == mPlayer_INDEX_PICKUP_JUMP ||
        requested == mPlayer_INDEX_PICKUP_FURNITURE || requested == mPlayer_INDEX_PICKUP_JUMP) {
        aMR_mp_lock_ftr(my_room->pickup_info.ftrID, 0);
        aMR_mp_lock_pos(my_room->pickup_info.leaf_pos, FALSE);
    }
}

// another player's furniture on its way into their pocket: the leaf it shows as, as the room draws this player's own
void aMR_mp_draw_leaf(void* game_v, unsigned short item, const void* pos_v, float scale) {
    GAME* game = (GAME*)game_v;
    const xyz_t* pos = (const xyz_t*)pos_v;
    int icon = aMR_ItemNo2IconNo(item);

    OPEN_DISP(game->graph);
    _texture_z_light_fog_prim(game->graph);
    Matrix_translate(pos->x, pos->y, pos->z, MTX_LOAD);
    Matrix_scale(scale, scale, scale, MTX_MULT);
    gSPMatrix(NEXT_POLY_OPA_DISP, _Matrix_to_Mtx_new(game->graph), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPDisplayList(NEXT_POLY_OPA_DISP, aMR_IconNo2Gfx1(icon));
    gSPDisplayList(NEXT_POLY_OPA_DISP, aMR_IconNo2Gfx2(icon));
    CLOSE_DISP(game->graph);
}

// the save's floor of a player's house this room is, 0 for the cottage and anywhere else
unsigned int aMR_mp_house_floor_off(void) {
    MY_ROOM_ACTOR* my_room = (aMR_CLIP != NULL) ? (MY_ROOM_ACTOR*)aMR_CLIP->my_room_actor_p : NULL;
    int floor_no;

    if (my_room == NULL || mFI_GET_TYPE(mFI_GetFieldId()) != mFI_FIELD_PLAYER0_ROOM ||
        (floor_no = mFI_GetPlayerHouseFloorNo(my_room->scene)) < 0 || floor_no >= mHm_ROOM_NUM) {
        return 0;
    }
    return (unsigned int)((u8*)&Save_Get(homes[(mFI_GetFieldId() - mFI_FIELD_PLAYER0_ROOM) & 3]).floors[floor_no] -
                          (u8*)&common_data.save.save);
}

// a stored thing's cell, kept in the furniture under it while the room is up
int aMR_mp_holds_cell(int layer, int ut) {
    int below;

    if (aMR_CLIP == NULL || aMR_CLIP->my_room_actor_p == NULL || l_aMR_work.used_list == NULL || ut < 0 ||
        ut >= UT_TOTAL_NUM) {
        return FALSE;
    }
    for (below = mCoBG_LAYER0; below < layer && below <= mCoBG_LAYER1; below++) {
        int id = aMR_place_table[below][ut];

        if (id < l_aMR_work.list_size && l_aMR_work.used_list[id] &&
            aMR_mp_is_storage(&l_aMR_work.ftr_actor_list[id])) {
            return TRUE;
        }
    }
    return FALSE;
}

// the room's lamps and gyroids' steps as its leaving would save them, and the tempo
int aMR_mp_meta(unsigned char* out, int max) {
    ACTOR* actorx = (aMR_CLIP != NULL) ? aMR_CLIP->my_room_actor_p : NULL;
    mHm_flr_c* room = Save_GetPointer(island.cottage.room);
    FTR_ACTOR* list = l_aMR_work.ftr_actor_list;
    int layer_bytes = sizeof(u64) + sizeof(room->layer_main.haniwa_step);
    int size = 2 * layer_bytes + sizeof(TempoBeat_c);
    TempoBeat_c tempo;
    int layer;
    int i;

    if (actorx == NULL || ((MY_ROOM_ACTOR*)actorx)->scene != SCENE_COTTAGE_MY || list == NULL ||
        l_aMR_work.used_list == NULL || max < size) {
        return 0;
    }
    for (layer = mCoBG_LAYER0; layer < mCoBG_LAYER2; layer++) {
        mActor_name_t* fg_p = aMR_GetLayerTopFg(layer);
        u64 bits = 0;
        u32 steps[8];
        int ut_x;
        int ut_z;

        mem_copy((u8*)steps, (u8*)(&room->layer_main)[layer].haniwa_step, sizeof(steps));
        for (ut_z = aMR_MIN_BOUND; ut_z <= aMR_MAX_BOUND && fg_p != NULL; ut_z++) {
            for (ut_x = aMR_MIN_BOUND; ut_x <= aMR_MAX_BOUND; ut_x++) {
                aMR_SaveOneFtrSwitchData(fg_p[ut_x + ut_z * UT_X_NUM], ut_x, ut_z, layer, &bits);
            }
        }
        for (i = 0; i < l_aMR_work.list_size; i++) {
            FTR_ACTOR* ftr_actor = &list[i];
            aFTR_PROFILE* profile;

            if (!l_aMR_work.used_list[i] || ftr_actor->layer != layer || ftr_actor->name >= FTR_NUM ||
                (profile = aMR_GetFurnitureProfile(ftr_actor->name)) == NULL ||
                !aFTR_CHECK_INTERACTION(profile->interaction_type, aFTR_INTERACTION_TYPE_HANIWA) ||
                !mFI_Wpos2UtNum_inBlock(&ut_x, &ut_z, ftr_actor->position) || !aMR_BOUNDS_OK(ut_x, ut_z)) {
                continue;
            }
            steps[ut_z - 1] &= ~(0xFu << ((ut_x - 1) * 4));
            steps[ut_z - 1] |= (u32)(ftr_actor->haniwa_step & 0xF) << ((ut_x - 1) * 4);
        }
        mem_copy(out + layer * layer_bytes, (u8*)&bits, sizeof(bits));
        mem_copy(out + layer * layer_bytes + sizeof(bits), (u8*)steps, sizeof(steps));
    }
    sAdo_GetRhythmInfo(&tempo);
    mem_copy(out + 2 * layer_bytes, (u8*)&tempo, sizeof(tempo));
    return size;
}
#endif

static void aMR_RedmaFtrBank(void) {
    int i;

    for (i = 0; i < FTR_NUM; i++) {
        if (l_bank_index_table[i] != 255) {
            u8* bank_addr = aMR_BankNo2BankAddress(l_bank_index_table[i]);
            mActor_name_t item = mRmTp_FtrIdx2FtrItemNo(i, mRmTp_DIRECT_SOUTH);

            aMR_DmaFurniture_Common(i, item, bank_addr, -1);
        }
    }
}

static void aMR_RequestStartEmu(MY_ROOM_ACTOR* my_room, FTR_ACTOR* ftr_actor, int game_idx, int agb_game_idx) {
    if (my_room->emu_info.request_flag == FALSE && mMsg_Check_MainHide(mMsg_Get_base_window_p())) {
        my_room->emu_info.request_flag = TRUE;
        my_room->emu_info.rom_no = game_idx;
        my_room->emu_info.agb_rom_no = agb_game_idx;
        my_room->emu_info.explaination_given_flag = FALSE;
        my_room->emu_info._10 = 0;
        my_room->requested_msg_type = aMR_MSG_STATE_QQQ_EMULATOR;
        my_room->emu_ftrID = ftr_actor->id;
    }
}

static void aMR_RequestStartEmu_MemoryC(MY_ROOM_ACTOR* my_room, FTR_ACTOR* ftr_actor, int game_idx) {
    if (my_room->emu_info.request_flag == FALSE && mMsg_Check_MainHide(mMsg_Get_base_window_p())) {
#ifdef TARGET_VITA
        // Vita has no memcard. If the user has roms in ux0:data/AnimalCrossing/rom/nes,
        // open the in-room picker overlay. Confirmation flips the emu_info flags from
        // My_Room_Actor_move so msg_ctrl calls goto_emu_game(rom_no--) on the next
        // frame; rom_no = -1 becomes current_famicom_rom = -2, caught by famicom_emu_init.
        {
            extern int vita_nes_scan_roms(void);
            extern void vita_nes_picker_open(void);
            if (vita_nes_scan_roms() > 0) {
                vita_nes_picker_open();
                s_vita_nes_picker_ftrID = ftr_actor->id;
                return;
            }
        }
#endif
        int card_count = aMR_GetCardFamicomCount();

        my_room->emu_info.card_famicom_count = card_count;
        my_room->emu_info.memory_game_select = 0;

        if (card_count > 0) {
            size_t namebuf_size = card_count * mIN_ITEM_NAME_LEN;

            if (my_room->emu_info.famicom_names_p != NULL) {
                zelda_free(my_room->emu_info.famicom_names_p);
            }

            my_room->emu_info.famicom_names_p = (char*)zelda_malloc(namebuf_size);

            if (my_room->emu_info.famicom_names_p != NULL) {
                int n_games = 0;

                if (famicom_get_disksystem_titles(&n_games, my_room->emu_info.famicom_names_p, namebuf_size) == FALSE) {
                    my_room->requested_msg_type = aMR_MSG_STATE_NO_PACK_NO_DATA;
                    my_room->room_msg_flag = TRUE;
                    return;
                }
            }

            if (card_count == 1) {
                my_room->emu_info.request_flag = TRUE;
                my_room->emu_info.rom_no = game_idx;
                my_room->emu_info.explaination_given_flag = FALSE;
                my_room->requested_msg_type = aMR_MSG_STATE_QQQ_EMULATOR_MEMORY1;
                my_room->emu_ftrID = ftr_actor->id;
                my_room->emu_info._10 = 0;
            } else if (card_count == 2) {
                my_room->emu_info.request_flag = TRUE;
                my_room->emu_info.rom_no = game_idx;
                my_room->emu_info.explaination_given_flag = FALSE;
                my_room->requested_msg_type = aMR_MSG_STATE_QQQ_EMULATOR_MEMORY2;
                my_room->emu_ftrID = ftr_actor->id;
                my_room->emu_info._10 = 0;
            } else {
                my_room->emu_info.request_flag = TRUE;
                my_room->emu_info.rom_no = game_idx;
                my_room->emu_info.explaination_given_flag = FALSE;
                my_room->requested_msg_type = aMR_MSG_STATE_QQQ_EMULATOR_MEMORY_OVER3;
                my_room->emu_ftrID = ftr_actor->id;
                my_room->emu_info._10 = 0;
            }
        } else {
            my_room->requested_msg_type = aMR_MSG_STATE_NO_PACK_NO_DATA;
            my_room->room_msg_flag = TRUE;
        }
    }
}

static void aMR_FamicomEmuCommonMove(FTR_ACTOR* ftr_actor, ACTOR* actorx, GAME* game, int rom_no, int agb_rom_no) {
    MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;

    if (ftr_actor->switch_changed_flag) {
        if (rom_no == 0) {
            aMR_RequestStartEmu_MemoryC(my_room, ftr_actor, 0);
        } else {
            aMR_RequestStartEmu(my_room, ftr_actor, rom_no, agb_rom_no);
        }
    }
}

static void aMR_CallSitDownOngenPosSE(const xyz_t* pos) {
    mActor_name_t item_no;
    int ftrID;
    xyz_t sit_pos = *pos;
    int ut_x = 0;
    int ut_z = 0;

    sit_pos.y = 0.0f;
    sit_pos.y = mCoBG_GetBgY_OnlyCenter_FromWpos(sit_pos, 0.0f);
    if (mFI_Wpos2UtNum(&ut_x, &ut_z, sit_pos) &&
        aMR_UnitNum2FtrItemNoFtrID(&item_no, &ftrID, ut_x, ut_z, mCoBG_LAYER0)) {
        FTR_ACTOR* ftr_actor = l_aMR_work.ftr_actor_list + ftrID;

        /* Check for massage chair */
        switch (ftr_actor->name) {
            case FTR_KON_MASAJI:
                sAdo_OngenPos((u32)ftr_actor, 39, &ftr_actor->position);
                break;
        }
    }
}

static int aMR_CheckDannaKill(xyz_t* pos) {
    mActor_name_t* fg_p = mFI_GetUnitFG(*pos);

    /* @BUG - this doesn't check for newer furniture in the 0x3XXX range */
    if (fg_p != NULL && ((*fg_p >= FTR0_START && *fg_p <= ITEM1_NO_START) || *fg_p == RSV_FE1F)) {
        int ut_x;
        int ut_z;

        if (mFI_Wpos2UtNum(&ut_x, &ut_z, *pos) && aMR_CLIP != NULL) {
            mActor_name_t item_no;
            int ftrID;

            if (aMR_UnitNum2FtrItemNoFtrID(&item_no, &ftrID, ut_x & 0xF, ut_z & 0xF, mCoBG_LAYER0)) {
                FTR_ACTOR* ftr_actor = l_aMR_work.ftr_actor_list + ftrID;
                aFTR_PROFILE* profile = aMR_GetFurnitureProfile(ftr_actor->name);

                if (profile != NULL &&
                    aFTR_CHECK_INTERACTION(profile->interaction_type, aFTR_INTERACTION_TYPE_NO_COLLISION) == FALSE &&
                    aMR_CheckFtrAndGoki2(ftr_actor->edge_collision, pos)) {
                    return TRUE;
                }
            }
        }
    }

    return FALSE;
}

extern int aMR_GetFurnitureUnit(mActor_name_t item) {
    if (ITEM_IS_FTR(item)) {
        int ftr_idx = mRmTp_FtrItemNo2FtrIdx(item);
        aFTR_PROFILE* profile = aMR_GetFurnitureProfile(ftr_idx);

        if (profile != NULL) {
            switch (profile->shape) {
                case aFTR_SHAPE_TYPEA:
                    return 0;
                case aFTR_SHAPE_TYPEC:
                    return 2;
                default:
                    return 1;
            }
        }
    }

    return -1;
}

extern int aMR_CorrespondFurniture(mActor_name_t ftr0, mActor_name_t ftr1) {
    if (ITEM_IS_FTR(ftr0) && ITEM_IS_FTR(ftr1)) {
        int ftr_idx0 = mRmTp_FtrItemNo2FtrIdx(ftr0);
        int ftr_idx1 = mRmTp_FtrItemNo2FtrIdx(ftr1);

        if (ftr_idx0 == ftr_idx1) {
            return TRUE;
        }
    }

    return FALSE;
}

extern mActor_name_t aMR_FurnitureFg_to_FurnitureFgWithDirect(mActor_name_t item, int direct) {
    if (ITEM_IS_FTR(item)) {
        return mRmTp_FtrIdx2FtrItemNo(mRmTp_FtrItemNo2FtrIdx(item), direct);
    }

    return item;
}

extern void aMR_RadioCommonMove(FTR_ACTOR* ftr_actor, ACTOR* actorx) {
    if (ftr_actor->haniwa_state == 1) {
        aMR_ReserveBgm(actorx, BGM_SPORTSFAIR_AEROBICS, ftr_actor, 0);
        ftr_actor->haniwa_state = 0;
    } else if (ftr_actor->switch_changed_flag) {
        if (ftr_actor->switch_bit == FALSE) {
            aMR_OneMDSwitchOn_TheOtherSwitchOff(ftr_actor);
            aMR_ReserveDefaultBgm(actorx, ftr_actor);
            aMR_ChangeMDBgm(actorx, ftr_actor);
            ftr_actor->switch_bit = FALSE;
        } else {
            aMR_OneMDSwitchOn_TheOtherSwitchOff(ftr_actor);
            aMR_ReserveBgm(actorx, BGM_SPORTSFAIR_AEROBICS, ftr_actor, 0);
            aMR_ChangeMDBgm(actorx, ftr_actor);
            ftr_actor->switch_bit = TRUE;
        }
    }
}

extern int aMR_RadioBgmNow(void) {
    if (mBGMPsComp_execute_bgm_num_get() == BGM_SPORTSFAIR_AEROBICS) {
        return TRUE;
    }

    return FALSE;
}

extern aMR_contact_info_c* aMR_GetContactInfoLayer1(void) {
    if (aMR_CLIP != NULL) {
        ACTOR* actorx = aMR_CLIP->my_room_actor_p;

        if (actorx != NULL) {
            MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;

            return &my_room->contact0;
        }
    }

    return NULL;
}

extern FTR_ACTOR* aMR_GetParentFactor(FTR_ACTOR* ftr_actor, ACTOR* actorx) {
    if (actorx != NULL) {
        MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;

        if (my_room->parent_ftr.ftrID != -1) {
            FTR_ACTOR* parent_ftr_actor = l_aMR_work.ftr_actor_list + my_room->parent_ftr.ftrID;

            if (my_room->parent_ftr.fit_ftr_table[0].ftr_ID == ftr_actor->id ||
                my_room->parent_ftr.fit_ftr_table[1].ftr_ID == ftr_actor->id ||
                my_room->parent_ftr.fit_ftr_table[2].ftr_ID == ftr_actor->id ||
                my_room->parent_ftr.fit_ftr_table[3].ftr_ID == ftr_actor->id) {
                return parent_ftr_actor;
            }
        }
    }

    return NULL;
}

extern s16 aMR_GetParentAngleOffset(FTR_ACTOR* ftr_actor, ACTOR* actorx) {
    if (actorx != NULL) {
        MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;
        if (my_room->parent_ftr.ftrID != -1) {
            FTR_ACTOR* parent_ftr_actor = l_aMR_work.ftr_actor_list + my_room->parent_ftr.ftrID;

            if (my_room->parent_ftr.fit_ftr_table[0].ftr_ID == ftr_actor->id ||
                my_room->parent_ftr.fit_ftr_table[1].ftr_ID == ftr_actor->id ||
                my_room->parent_ftr.fit_ftr_table[2].ftr_ID == ftr_actor->id ||
                my_room->parent_ftr.fit_ftr_table[3].ftr_ID == ftr_actor->id) {
                return parent_ftr_actor->s_angle_y - my_room->parent_ftr.angle_y;
            }
        }
    }

    return 0;
}

extern u8 aMR_GetAlphaEdge(u16 ftr_name) {
    switch (ftr_name) {
        case FTR_SUM_CLASSICWARDROPE01:  // classic cabinet
        case FTR_SUM_VIOLA01: // violin
        case FTR_SUM_BASS01: // bass (instrument)
        case FTR_SUM_CELLO01: // cello
            return 11;
        case FTR_KON_AMECLOCK: // kitschy clock
        case FTR_KON_ATQCLOCK: // antique clock
            return 250;
        case FTR_NOG_BALLOON_COMMON0: // red balloon
        case FTR_NOG_BALLOON_COMMON1: // yellow balloon
        case FTR_NOG_BALLOON_COMMON2: // blue balloon
        case FTR_NOG_BALLOON_COMMON3: // green balloon
        case FTR_NOG_BALLOON_COMMON4: // purple balloon
        case FTR_NOG_BALLOON_COMMON5: // bunny p. balloon
        case FTR_NOG_BALLOON_COMMON6: // bunny b. balloon
        case FTR_NOG_BALLOON_COMMON7: // bunny o. balloon
            return 96;
        case FTR_IKE_K_TANABATA01:
            return 127; // tanabata palm
        case FTR_YOS_WHEEL:
            return 20; // wagon wheel
        default:
            return 127;
    }
}

extern int aMR_DrawDolphinMode(u16 ftr_name) {
    switch (ftr_name) {
        case FTR_TAK_MONEY: // stone coin
        case FTR_TAK_HAM1: // hamster cage
        case FTR_IKE_PRORES_LING01: // neutral corner
        case FTR_IKE_PRORES_LING02: // red corner
        case FTR_IKE_PRORES_LING03: // blue corner
            return TRUE;
        default:
            return FALSE;
    }
}

extern void aMR_ThrowItem_FurnitureLock(void) {
    if (aMR_CLIP != NULL) {
        ACTOR* actorx = aMR_CLIP->my_room_actor_p;

        if (actorx != NULL) {
            MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;

            my_room->throw_item_lock_flag = TRUE;
        }
    }
}

extern void aMR_ThrowItem_FurnitureUnlock(void) {
    if (aMR_CLIP != NULL) {
        ACTOR* actorx = aMR_CLIP->my_room_actor_p;

        if (actorx != NULL) {
            MY_ROOM_ACTOR* my_room = (MY_ROOM_ACTOR*)actorx;

            my_room->throw_item_lock_flag = FALSE;
        }
    }
}

extern void aMR_SameFurnitureSwitchOFF(u16 ftr_name) {
    FTR_ACTOR* ftr_actor = l_aMR_work.ftr_actor_list;
    u8* used = l_aMR_work.used_list;
    int i;

    for (i = 0; i < l_aMR_work.list_size; i++) {
        if (*used && ftr_actor->name == ftr_name) {
            ftr_actor->switch_bit = FALSE;
            ftr_actor->switch_changed_flag = TRUE;
        }

        ftr_actor++;
        used++;
    }
}
