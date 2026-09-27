#ifndef AC_MP_NOTICE_H
#define AC_MP_NOTICE_H

#include "types.h"
#include "m_actor.h"

#ifdef __cplusplus
extern "C" {
#endif

// station PA announcement for multiplayer events, spoken like the train conductor
typedef struct mp_notice_actor_s {
    ACTOR actor_class;
    int stage;
    int wait;
    int answered;
    int msg_no;          // what it is saying, fixed when the window opens
    unsigned int serial; // which queued notice that is
} MP_NOTICE_ACTOR;

extern ACTOR_PROFILE Mp_Notice_Profile;

#ifdef __cplusplus
}
#endif

#endif
