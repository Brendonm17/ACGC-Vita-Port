#ifndef AC_MP_PLAYER_H
#define AC_MP_PLAYER_H

#include "types.h"
#include "m_actor.h"
#include "c_keyframe.h"
#include "m_collision_obj.h"
#include "m_player.h"
#include "pc_mp.h"

#ifdef __cplusplus
extern "C" {
#endif

// another player in town, drawn from the network (multiplayer)
typedef struct mp_player_actor_s {
    ACTOR actor_class;
    int slot;
    unsigned int look_ver;
    int gender;
    int anim0;
    int anim1;
    unsigned char rev0; // the animation plays from its end back (as putting a tool away does)
    unsigned char rev1;
    int part_idx;
    int item_shape;
    int item_anim;
    unsigned char seq;
    unsigned char hidden;
    unsigned char umbrella;
    unsigned char hand_valid;
    xyz_t target;
    xyz_t velocity;
    unsigned int target_ms;
    s16 target_rot;
    s16 head_x;
    s16 head_y;
    unsigned char eye;
    unsigned char mouth;
    unsigned char item_main;
    unsigned short flags; // MP_PF_ bits of the newest state
    s16 target_rot_x;
    s16 target_rot_z;
    s16 roll;
    s16 rod_angle_z;
    s_xyz net_angle;
    f32 item_scale;
    int root_flags;
    xyz_t root_trans;
    s_xyz root_rot;
    int umb_action;
    f32 umb_frame;
    unsigned char umb_idx;
    xyz_t umb_e;
    xyz_t umb_kasa;
    u16 hold_item;
    f32 hold_scale;
    xyz_t hold_d;
    s16 hold_angle;
    u8 hold_jump;
    f32 star_timer;
    unsigned char natt;
    mp_att_t att[MP_ATT_MAX];
    s16 balloon_x;
    s16 balloon_z;
    f32 balloon_frame; // the sender's balloon pose, followed rather than played
    cKF_SkeletonInfo_R_c bee_kf; // the swarm chasing this player, posed as the sender's
    s_xyz bee_work[4];
    s_xyz bee_morph[4];
    s16 bee_fly[2];
    unsigned char bee_ready;
    cKF_SkeletonInfo_R_c kf0;
    cKF_SkeletonInfo_R_c kf1;
    s_xyz work[mPlayer_JOINT_NUM + 1];
    s_xyz morph[mPlayer_JOINT_NUM + 1];
    s8 part_table[mPlayer_JOINT_NUM + 1];
    cKF_SkeletonInfo_R_c item_kf;
    s_xyz item_work[8];
    s_xyz item_morph[8];
    MtxF hand_mtx;
    ClObjPipe_c pipe;
    u8 face_tex[0xE00] ATTRIBUTE_ALIGN(32);
    u16 face_pal[16] ATTRIBUTE_ALIGN(32);
    u8 cloth_tex[0x200] ATTRIBUTE_ALIGN(32);
    u16 cloth_pal[16] ATTRIBUTE_ALIGN(32);
    u8 umb_tex[0x200] ATTRIBUTE_ALIGN(32);
    u16 umb_pal[16] ATTRIBUTE_ALIGN(32);
} MP_PLAYER_ACTOR;

extern ACTOR_PROFILE Mp_Player_Profile;

#ifdef __cplusplus
}
#endif

#endif
