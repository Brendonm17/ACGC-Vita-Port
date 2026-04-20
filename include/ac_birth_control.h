#ifndef AC_BIRTH_CONTROL_H
#define AC_BIRTH_CONTROL_H

#include "types.h"
#include "m_actor.h"
#include "m_field_make.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct birth_control_s BIRTH_CONTROL_ACTOR;

struct birth_control_s {
  ACTOR actor_class;
  int setup_actor_flag;
  mFM_move_actor_c* move_actor_data;
  u16 move_actor_bitfield;
  s16 move_actor_list_exists_flag;
  int boat_spawned;
};

extern ACTOR_PROFILE Birth_Control_Profile;

#ifdef TARGET_VITA
// called from Actor_delete when an actor dies. flips the block's
// "fully prespawned" bit off so the next prespawn pass will re-scan
// it. no-op for out-of-range / unassigned block coords.
extern void aBC_vita_clear_block_spawned(s8 bx, s8 bz);
#endif

#ifdef __cplusplus
}
#endif

#endif

