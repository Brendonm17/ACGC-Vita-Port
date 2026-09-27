#include "ac_balloon.h"

#include "m_name_table.h"
#include "sys_matrix.h"
#include "m_player_lib.h"
#include "m_rcp.h"
#ifdef VITA_MP
#include "m_play.h"
#include "pc_mp.h"

#define Ac_Balloon_MP_MAX 4

// another player's balloons floating off here, each gone once it has hidden
static ACTOR* Ac_Balloon_mp_actor[Ac_Balloon_MP_MAX];

static void Ac_Balloon_mp_put(void* dst, const void* src, int n) {
    u8* d = (u8*)dst;
    const u8* s = (const u8*)src;
    int i;

    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
}
#endif

enum {
    Ac_Balloon_MODE_HIDE,
    Ac_Balloon_MODE_FLY,

    Ac_Balloon_MODE_NUM
};

static void Ac_Balloon_dt(ACTOR* actorx, GAME* game) {
#ifdef VITA_MP
    int i;

    for (i = 0; i < Ac_Balloon_MP_MAX; i++) {
        if (Ac_Balloon_mp_actor[i] == actorx) {
            Ac_Balloon_mp_actor[i] = NULL;
        }
    }
#endif
}

extern void Ac_Balloon_request_hide(ACTOR* actorx, GAME* game) {
    BALLOON_ACTOR* balloon = (BALLOON_ACTOR*)actorx;

    balloon->setup_mode = Ac_Balloon_MODE_HIDE;
}

static void Ac_Balloon_setup_hide(ACTOR* actorx, GAME* game) {
    BALLOON_ACTOR* balloon = (BALLOON_ACTOR*)actorx;

    balloon->main_mode = Ac_Balloon_MODE_HIDE;
    balloon->setup_mode = -1;
}

static void Ac_Balloon_Movement_hide(ACTOR* actorx, GAME* game) {
    ACTOR* player_actor = GET_PLAYER_ACTOR_GAME_ACTOR(game);

    if (player_actor != NULL) {
        actorx->world.position = player_actor->world.position;
    }
}

static void Ac_Balloon_main_hide(ACTOR* actorx, GAME* game) {
    Ac_Balloon_Movement_hide(actorx, game);
}

extern void Ac_Balloon_request_fly(ACTOR* actorx, GAME* game, int balloon_type, const s_xyz* angle_p, s16 lean,
                                   const xyz_t* pos_p, f32 start_frame, f32 speed) {
    BALLOON_ACTOR* balloon = (BALLOON_ACTOR*)actorx;

    balloon->setup_mode = Ac_Balloon_MODE_FLY;
    balloon->balloon_type = balloon_type;
    balloon->angle = *angle_p;
    balloon->lean = lean;
    balloon->start_frame = start_frame;
    balloon->speed = speed;
    balloon->pos = *pos_p;
#ifdef VITA_MP
    // the local player let it go: the other screens see it float off too
    if (mp_vfx_recording()) {
        u8 body[30];

        body[0] = MP_VFX_BALLOON;
        body[1] = (u8)balloon_type;
        Ac_Balloon_mp_put(body + 2, angle_p, 6);
        Ac_Balloon_mp_put(body + 8, &lean, 2);
        Ac_Balloon_mp_put(body + 10, pos_p, 12);
        Ac_Balloon_mp_put(body + 22, &start_frame, 4);
        Ac_Balloon_mp_put(body + 26, &speed, 4);
        mp_vfx_send(body, sizeof(body));
    }
#endif
}

#ifdef VITA_MP
void Ac_Balloon_mp_replay(struct game_play_s* play_s, const u8* body, int len) {
    GAME_PLAY* play = (GAME_PLAY*)play_s;
    s_xyz angle;
    s16 lean;
    xyz_t pos;
    f32 start_frame;
    f32 speed;
    int i;

    if (len < 30) {
        return;
    }
    for (i = 0; i < Ac_Balloon_MP_MAX && Ac_Balloon_mp_actor[i] != NULL; i++) {
    }
    if (i == Ac_Balloon_MP_MAX) {
        return;
    }
    Ac_Balloon_mp_put(&angle, body + 2, 6);
    Ac_Balloon_mp_put(&lean, body + 8, 2);
    Ac_Balloon_mp_put(&pos, body + 10, 12);
    Ac_Balloon_mp_put(&start_frame, body + 22, 4);
    Ac_Balloon_mp_put(&speed, body + 26, 4);
    Ac_Balloon_mp_actor[i] = Actor_info_make_actor(&play->actor_info, (GAME*)play, mAc_PROFILE_BALLOON, pos.x, pos.y,
                                                   pos.z, 0, 0, 0, -1, -1, -1, EMPTY_NO, -1, -1, -1);
    if (Ac_Balloon_mp_actor[i] != NULL) {
        Ac_Balloon_request_fly(Ac_Balloon_mp_actor[i], (GAME*)play, body[1], &angle, lean, &pos, start_frame, speed);
    }
}

// a replayed balloon that has floated out of sight is done
static void Ac_Balloon_mp_done(ACTOR* actorx) {
    BALLOON_ACTOR* balloon = (BALLOON_ACTOR*)actorx;
    int i;

    for (i = 0; i < Ac_Balloon_MP_MAX; i++) {
        if (Ac_Balloon_mp_actor[i] == actorx) {
            if (balloon->main_mode == Ac_Balloon_MODE_HIDE && balloon->setup_mode < 0) {
                Ac_Balloon_mp_actor[i] = NULL;
                Actor_delete(actorx);
            }
            return;
        }
    }
}
#endif

static void Ac_Balloon_setup_fly(ACTOR* actorx, GAME* game) {
    static int data[] = {
        mPlayer_ITEM_DATA_BALLOON1, mPlayer_ITEM_DATA_BALLOON2, mPlayer_ITEM_DATA_BALLOON3, mPlayer_ITEM_DATA_BALLOON4,
        mPlayer_ITEM_DATA_BALLOON5, mPlayer_ITEM_DATA_BALLOON6, mPlayer_ITEM_DATA_BALLOON7, mPlayer_ITEM_DATA_BALLOON8,
    };

    BALLOON_ACTOR* balloon = (BALLOON_ACTOR*)actorx;
    f32 start_frame;
    int data_type;
    int anim_type;
    cKF_SkeletonInfo_R_c* kf_p;
    cKF_Skeleton_R_c* skeleton;
    cKF_Animation_R_c* animation;

    balloon->main_mode = Ac_Balloon_MODE_FLY;
    balloon->saved_type = balloon->balloon_type;
    data_type = Ac_Balloon_TYPE_VALID(balloon->balloon_type) != FALSE ? data[balloon->balloon_type]
                                                                      : mPlayer_ITEM_DATA_BALLOON1;
    start_frame = balloon->start_frame;
    anim_type = start_frame < 1.0f ? mPlayer_ITEM_DATA_BALLOON_WAIT : mPlayer_ITEM_DATA_BALLOON_GYAZA;
    kf_p = &balloon->keyframe;
    skeleton = (cKF_Skeleton_R_c*)mPlib_Get_Item_DataPointer(data_type);
    animation = (cKF_Animation_R_c*)mPlib_Get_Item_DataPointer(anim_type);
    cKF_SkeletonInfo_R_ct(kf_p, skeleton, NULL, balloon->work, balloon->morph);
    cKF_SkeletonInfo_R_init_standard_repeat_setframeandspeedandmorph(kf_p, animation, NULL, start_frame, 0.0f, 0.0f);
    cKF_SkeletonInfo_R_play(kf_p);
    cKF_SkeletonInfo_R_init_standard_repeat_setframeandspeedandmorph(
        kf_p, mPlib_Get_Item_DataPointer(mPlayer_ITEM_DATA_BALLOON_WAIT), NULL, 1.0f, 0.5f, -5.0f);

    actorx->shape_info.rotation = balloon->angle;
    actorx->position_speed.y = 0.0f;
    actorx->max_velocity_y = balloon->speed;
    actorx->gravity = 0.2f;
    actorx->world.position = balloon->pos;
    balloon->setup_mode = -1;
}

static void Ac_Balloon_CulcAnimation_fly(ACTOR* actorx) {
    BALLOON_ACTOR* balloon = (BALLOON_ACTOR*)actorx;

    cKF_SkeletonInfo_R_play(&balloon->keyframe);
}

static void Ac_Balloon_Movement_fly(BALLOON_ACTOR* balloon) {
    add_calc_short_angle2(&balloon->actor_class.shape_info.rotation.x, 0, 1.0f - sqrtf(0.5f), 50, 5);
    add_calc_short_angle2(&balloon->actor_class.shape_info.rotation.z, 0, 1.0f - sqrtf(0.5f), 50, 5);
    add_calc_short_angle2(&balloon->lean, 0, 1.0f - sqrtf(0.5f), 50, 5);
    Actor_position_moveF(&balloon->actor_class);
}

static void Ac_Balloon_request_change_mode_fromFly(ACTOR* actorx, GAME* game) {
    ACTOR* player_actor = GET_PLAYER_ACTOR_GAME_ACTOR(game);

    if (player_actor != NULL && actorx->world.position.y - player_actor->world.position.y > 200.0f) {
        Ac_Balloon_request_hide(actorx, game);
    }
}

static void Ac_Balloon_main_fly(ACTOR* actorx, GAME* game) {
    BALLOON_ACTOR* balloon = (BALLOON_ACTOR*)actorx;

    Ac_Balloon_CulcAnimation_fly(actorx);
    Ac_Balloon_Movement_fly(balloon);
    Ac_Balloon_request_change_mode_fromFly(actorx, game);
}

typedef void (*Ac_Balloon_PROC)(ACTOR*, GAME*);

static void Ac_Balloon_main(ACTOR* actorx, GAME* game) {
    BALLOON_ACTOR* balloon = (BALLOON_ACTOR*)actorx;

    if (balloon->setup_mode >= 0 && balloon->setup_mode < Ac_Balloon_MODE_NUM) {
        static Ac_Balloon_PROC data[] = { &Ac_Balloon_setup_hide, &Ac_Balloon_setup_fly };

        if (data[balloon->setup_mode] != NULL) {
            (*data[balloon->setup_mode])(actorx, game);
        }
    }

    if (balloon->main_mode >= 0 && balloon->main_mode < Ac_Balloon_MODE_NUM) {
        static Ac_Balloon_PROC data[] = { &Ac_Balloon_main_hide, &Ac_Balloon_main_fly };

        if (data[balloon->main_mode] != NULL) {
            (*data[balloon->main_mode])(actorx, game);
        }
    }
#ifdef VITA_MP
    Ac_Balloon_mp_done(actorx);
#endif
}

static void Ac_Balloon_ct(ACTOR* actorx, GAME* game) {
    Ac_Balloon_request_hide(actorx, game);
}

static int Ac_Balloon_draw_Before(GAME* game, cKF_SkeletonInfo_R_c* keyframe, int joint_idx, Gfx** joint_shape,
                                  u8* joint_flags, void* arg, s_xyz* joint_rot, xyz_t* joint_pos) {
    switch (joint_idx) {
        case 1:
        case 2:
        case 3:
            OPEN_POLY_OPA_DISP(game->graph);

            gDPPipeSync(POLY_OPA_DISP++);
            gDPSetTexEdgeAlpha(POLY_OPA_DISP++, 80);

            CLOSE_POLY_OPA_DISP(game->graph);
            break;
    }

    return TRUE;
}

static int Ac_Balloon_draw_After(GAME* game, cKF_SkeletonInfo_R_c* keyframe, int joint_idx, Gfx** joint_shape,
                                 u8* joint_flags, void* arg, s_xyz* joint_rot, xyz_t* joint_pos) {
    switch (joint_idx) {
        case 1:
        case 2:
        case 3:
            OPEN_POLY_OPA_DISP(game->graph);

            gDPPipeSync(POLY_OPA_DISP++);
            gDPSetTexEdgeAlpha(POLY_OPA_DISP++, 144);

            CLOSE_POLY_OPA_DISP(game->graph);
            break;
    }

    return TRUE;
}

static void Ac_Balloon_draw_normal(ACTOR* actorx, GAME* game) {
    GAME_PLAY* play = (GAME_PLAY*)game;
    s16 angle_x;
    s16 angle_y;
    BALLOON_ACTOR* balloon = (BALLOON_ACTOR*)actorx;
    int idx = game->frame_counter & 1;
    Mtx* mtx = balloon->mtx[idx];
    GRAPH* graph = game->graph;
    xyz_t* pos = &actorx->world.position;

    Matrix_push();
    angle_x = actorx->shape_info.rotation.x - DEG2SHORT_ANGLE2(90.0f);
    angle_y = actorx->shape_info.rotation.y;
    Matrix_translate(pos->x, pos->y, pos->z, MTX_LOAD);
    Matrix_RotateY(angle_y, MTX_MULT);
    Matrix_RotateX(angle_x, MTX_MULT);
    Matrix_RotateZ(DEG2SHORT_ANGLE2(90.0f), MTX_MULT);
    Matrix_RotateX(balloon->lean, MTX_MULT);
    Matrix_scale(actorx->scale.x, actorx->scale.y, actorx->scale.z, MTX_MULT);

    OPEN_POLY_OPA_DISP(graph);

    gSPMatrix(POLY_OPA_DISP++, _Matrix_to_Mtx_new(graph), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

    CLOSE_POLY_OPA_DISP(graph);

    _texture_z_light_fog_prim(graph);
    Setpos_HiliteReflect_init(pos, play);
    cKF_Si3_draw_R_SV((GAME*)play, &balloon->keyframe, mtx, &Ac_Balloon_draw_Before, &Ac_Balloon_draw_After, balloon);
    Matrix_pull();
}

static void Ac_Balloon_draw(ACTOR* actorx, GAME* game) {
    BALLOON_ACTOR* balloon = (BALLOON_ACTOR*)actorx;

    if (balloon->main_mode != Ac_Balloon_MODE_HIDE) {
        Ac_Balloon_draw_normal(actorx, game);
    }
}

ACTOR_PROFILE Balloon_Profile = {
    mAc_PROFILE_BALLOON,
    ACTOR_PART_BG,
    ACTOR_STATE_NO_DRAW_WHILE_CULLED | ACTOR_STATE_NO_MOVE_WHILE_CULLED,
    ITM_BALLOON_START,
    ACTOR_OBJ_BANK_KEEP,
    sizeof(BALLOON_ACTOR),
    &Ac_Balloon_ct,
    &Ac_Balloon_dt,
    &Ac_Balloon_main,
    &Ac_Balloon_draw,
    NULL,
};
