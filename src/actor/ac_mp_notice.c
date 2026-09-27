// ac_mp_notice.c - a station PA announcement: the head of the multiplayer notice queue
// spoken through the stock SPEAK demo, then the actor removes itself.
#ifdef VITA_MP
#include "ac_mp_notice.h"

#include "m_common_data.h"
#include "m_demo.h"
#include "m_msg.h"
#include "m_camera2.h"
#include "m_choice.h"
#include "pc_mp.h"
#include "pc_mp_text_data.h"

#define aMPN_GIVE_UP_FRAMES 300

enum {
    aMPN_STAGE_REQUEST,
    aMPN_STAGE_SPEAK,
};

static void aMPN_actor_ct(ACTOR* actorx, GAME* game);
static void aMPN_actor_dt(ACTOR* actorx, GAME* game);
static void aMPN_actor_move(ACTOR* actorx, GAME* game);

// clang-format off
ACTOR_PROFILE Mp_Notice_Profile = {
    mAc_PROFILE_MP_NOTICE,
    ACTOR_PART_CONTROL,
    ACTOR_STATE_NO_MOVE_WHILE_CULLED | ACTOR_STATE_NO_DRAW_WHILE_CULLED,
    EMPTY_NO,
    ACTOR_OBJ_BANK_KEEP,
    sizeof(MP_NOTICE_ACTOR),
    &aMPN_actor_ct,
    &aMPN_actor_dt,
    &aMPN_actor_move,
    mActor_NONE_PROC1,
    NULL,
};
// clang-format on

static void aMPN_actor_ct(ACTOR* actorx, GAME* game) {
    MP_NOTICE_ACTOR* notice = (MP_NOTICE_ACTOR*)actorx;

    notice->stage = aMPN_STAGE_REQUEST;
    notice->wait = 0;
    notice->answered = FALSE;
    notice->msg_no = 0;
    notice->serial = 0;
}

static void aMPN_actor_dt(ACTOR* actorx, GAME* game) {
    mp_notice_actor_gone(actorx, ((MP_NOTICE_ACTOR*)actorx)->stage == aMPN_STAGE_SPEAK);
}

// the conductor's announcement window: no name tag, train green, camera held
static void aMPN_set_talk_info(ACTOR* actorx) {
    MP_NOTICE_ACTOR* notice = (MP_NOTICE_ACTOR*)actorx;
    unsigned char town[MP_NAME_LEN];
    unsigned char name[MP_NAME_LEN];
    rgba_t color;

    notice->msg_no = mp_notice_begin(actorx, &notice->serial, town, name);
    mMsg_SET_FREE_STR(mMsg_FREE_STR16, town, MP_NAME_LEN);
    mMsg_SET_FREE_STR(mMsg_FREE_STR17, name, MP_NAME_LEN);
    mDemo_Set_msg_num(notice->msg_no);
    mDemo_Set_talk_display_name(FALSE);
    mDemo_Set_camera(CAMERA2_PROCESS_STOP);
    mDemo_Set_talk_turn(FALSE);
    color.r = 175;
    color.g = 255;
    color.b = 175;
    color.a = 255;
    mDemo_Set_talk_window_color(&color);
}

static void aMPN_actor_move(ACTOR* actorx, GAME* game) {
    MP_NOTICE_ACTOR* notice = (MP_NOTICE_ACTOR*)actorx;

    if (notice->stage == aMPN_STAGE_REQUEST) {
        if (mDemo_Check(mDemo_TYPE_SPEAK, actorx) == TRUE) {
            if (!mDemo_Check_ListenAble()) {
                mDemo_Set_ListenAble();
            }
            notice->stage = aMPN_STAGE_SPEAK;
        } else if (mp_notice_withdrawn(actorx) || ++notice->wait > aMPN_GIVE_UP_FRAMES) {
            // taken back, or something else holds the demo; the queue tries again later
            Actor_delete(actorx);
        } else {
            mDemo_Request(mDemo_TYPE_SPEAK, actorx, &aMPN_set_talk_info);
        }
    } else if (mDemo_Check(mDemo_TYPE_SPEAK, actorx) == FALSE) {
        Actor_delete(actorx);
    } else if (!notice->answered && mMsg_CHECK_MAINNORMALCONTINUE() == TRUE) {
        // a caller's request: the choice goes back to the line, then the window closes
        if (notice->msg_no == MP_MSG_N_REQUEST) {
            mMsg_Window_c* msg_p = mMsg_Get_base_window_p();

            mp_notice_answer(notice->serial, mChoice_GET_CHOSENUM() == mChoice_CHOICE0);
            notice->answered = TRUE;
            mMsg_Set_CancelNormalContinue(msg_p);
            mMsg_Unset_LockContinue(msg_p);
        }
    }
}

#endif
