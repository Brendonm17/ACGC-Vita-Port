#ifndef AC_STATION_CLIP_H
#define AC_STATION_CLIP_H

#include "types.h"
#include "m_actor.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    aSTM_TALK_CHK_LEAVE_TALK,
    aSTM_TALK_CHK_LEAVE_TALK2,
    aSTM_TALK_SAYONARA,
    aSTM_TALK_CHK_TRAIN_TALK,
    aSTM_TALK_CHK_TRAIN2_TALK,
    aSTM_TALK_BEFORE_SAVE_TALK,
    aSTM_TALK_SAVE_TALK,
    aSTM_TALK_CHK_OVER_SAVE_TALK,
    aSTM_TALK_SAVE_TALK_END,
    aSTM_TALK_SAVE_ERROR,
    aSTM_TALK_CHK_REPAIRID,
    aSTM_TALK_REPAIRID_BF,
    aSTM_TALK_REPAIRID,
    aSTM_TALK_REPAIRID_AFTER,
    aSTM_TALK_CARDPROC,
    aSTM_TALK_END_WAIT,
#ifdef VITA_MP
    // friends' towns (ac_station_clip_net.c_inc)
    aSTM_TALK_NET_RES_MENU,
    aSTM_TALK_NET_MENU,
    aSTM_TALK_NET_WHERE,
    aSTM_TALK_NET_WAIT,
    aSTM_TALK_NET_LIST,
    aSTM_TALK_NET_RETRY,
    aSTM_TALK_NET_LEDIT_CLOSE,
    aSTM_TALK_NET_LEDIT_OPEN,
    aSTM_TALK_NET_LEDIT_END,
    aSTM_TALK_NET_LEDIT_REOPEN,
    aSTM_TALK_NET_WELCOME,
    aSTM_TALK_NET_DEPART,
    aSTM_TALK_NET_HOST_STATUS,
    aSTM_TALK_NET_HOST_CONFIRM,
    aSTM_TALK_NET_HOST_PORTMAP,
    aSTM_TALK_NET_HOST_THANKS,
#endif

    aSTM_TALK_NUM
};

typedef int (*aSTC_CHANGE_TALK_PROC)(ACTOR*, int);
typedef int (*aSTC_NET_ROUTE_PROC)(ACTOR*);

typedef struct station_clip_s {
    aSTC_CHANGE_TALK_PROC change_talk_proc;
#ifdef VITA_MP
    // resident travel prompt; TRUE when friends' towns took the conversation
    aSTC_NET_ROUTE_PROC net_route_proc;
#endif
} aSTC_clip_c;

#ifdef __cplusplus
}
#endif

#endif
