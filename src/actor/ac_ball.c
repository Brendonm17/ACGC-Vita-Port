#include "ac_ball.h"
#include "m_actor_shadow.h"
#include "m_common_data.h"
#include "m_debug.h"
#include "m_field_info.h"
#include "m_lib.h"
#include "m_name_table.h"
#include "m_npc.h"
#include "m_player_lib.h"
#include "m_quest.h"
#include "m_random_field.h"
#include "m_roll_lib.h"
#include "sys_matrix.h"
#include "m_collision_bg.h"
#ifdef VITA_MP
#include "m_play.h"
#include "pc_mp.h"
#endif

extern Gfx act_ball_b_model[];
extern Gfx act_ball_d_model[];
extern Gfx act_ball_s_model[];

Gfx* ball_model_tbl[] = {
    act_ball_b_model,
    act_ball_d_model,
    act_ball_s_model,
};

static void aBALL_actor_ct(ACTOR* actor, GAME* game);
static void aBALL_actor_dt(ACTOR* actor, GAME* game);
static void aBALL_actor_move(ACTOR* actor, GAME* game);
static void aBALL_actor_draw(ACTOR* actor, GAME* game);

ACTOR_PROFILE Ball_Profile = {
    mAc_PROFILE_BALL,
    ACTOR_PART_BG,
    ACTOR_STATE_NO_MOVE_WHILE_CULLED,
    ETC_BALL,
    ACTOR_OBJ_BANK_KEEP,
    sizeof(BALL_ACTOR),
    &aBALL_actor_ct,
    &aBALL_actor_dt,
    &aBALL_actor_move,
    &aBALL_actor_draw,
    NULL,
};

BALL_ACTOR* Global_Actor_p;

ClObjPipeData_c aBALL_CoInfoData = {
    { 0x39, 0x20, ClObj_TYPE_PIPE }, // collision data
    { 1 },                           // element data
    // Pipe specs
    {
        13,  // radius
        30,  // height
        -10, // offset

        { 0, 0, 0 }, // center
    },
};

StatusData_c aBALL_StatusData = {
    0, 13, 30, -10, 100,
};

static void aBALL_process_ground_init(ACTOR*, GAME*);
static void aBALL_process_air_water(ACTOR*, GAME*);
static void aBALL_process_ground_water(ACTOR*, GAME*);
static void aBALL_process_ground(ACTOR*, GAME*);
static void aBALL_process_air(ACTOR*, GAME*);
static void aBALL_process_air_water_init(ACTOR* actor, GAME*);
static void aBALL_process_ground_water_init(ACTOR* actor, GAME*);

#ifdef VITA_MP
enum {
    aBALL_MP_GROUND,
    aBALL_MP_AIR,
    aBALL_MP_AIR_WATER,
    aBALL_MP_GROUND_WATER,
};

#define aBALL_MP_REST_EVERY 30 // frames between a resting ball's reports
#define aBALL_MP_SNAP       120.0f
#define aBALL_MP_AHEAD      12 // frames a report is rolled on at most; past that it waits for the next

// the game that kicked the ball last moves it; the others follow its reports
static u32 aBALL_mp_mine_ms; // shared clock at this game's last kick, 0 for never
static u8 aBALL_mp_snd;      // this game's thuds and bounces
static u16 aBALL_mp_snd_id;
static f32 aBALL_mp_snd_speed;
static u8 aBALL_mp_heard; // the mover's count last played here
static u8 aBALL_mp_heard_ok;
static int aBALL_mp_timer;
static int aBALL_mp_ripple;

static void aBALL_mp_claim(void) {
    unsigned int ms;

    if (mp_shared_clock_ms(&ms)) {
        aBALL_mp_mine_ms = ms | 1;
    }
}

// kick a came before kick b, the short way round the clock; 0 never kicked
static int aBALL_mp_older(u32 a, u32 b) {
    return b != 0 && (a == 0 || (s32)(a - b) < 0);
}

// the local player, or a villager this game moves
static int aBALL_mp_local_kicker(ACTOR* hit, GAME* game) {
    if (hit == NULL) {
        return FALSE;
    }
    return hit == GET_PLAYER_ACTOR_GAME_ACTOR(game) || (hit->part == ACTOR_PART_NPC && !mp_npc_is_puppet(hit));
}

// another player, or a villager another game moves: that game plays the bump out
static int aBALL_mp_remote_bump(ACTOR* hit) {
    return hit != NULL &&
           (hit->id == mAc_PROFILE_MP_PLAYER || (hit->part == ACTOR_PART_NPC && mp_npc_is_puppet(hit)));
}

// a thud or bounce, heard here and counted for the others
static void aBALL_mp_se(f32 speed, u16 se_no, xyz_t* pos) {
    if (speed > 0.0f) {
        sAdo_OngenTrgStartSpeed(speed, se_no, pos);
    } else {
        sAdo_OngenTrgStart(se_no, pos);
    }
    aBALL_mp_snd++;
    aBALL_mp_snd_id = se_no;
    aBALL_mp_snd_speed = speed;
}
#define aBALL_SE_SPEED(speed, se_no, pos) aBALL_mp_se((speed), (se_no), (pos))
#define aBALL_SE(se_no, pos)              aBALL_mp_se(0.0f, (se_no), (pos))
#else
#define aBALL_SE_SPEED(speed, se_no, pos) sAdo_OngenTrgStartSpeed((speed), (se_no), (pos))
#define aBALL_SE(se_no, pos)              sAdo_OngenTrgStart((se_no), (pos))
#endif

static int aBALL_Random_pos_set(xyz_t* pos) {
    int x_max;
    int z_max;
    int random_x;
    int random_z;
    int ut_x;
    int ut_z;
    int i;
    int j;

    x_max = mFI_GetBlockXMax();
    z_max = mFI_GetBlockZMax();

    random_x = RANDOM_F(x_max);
    random_z = RANDOM_F(z_max);

    for (i = 0; i < x_max; i++) {
        for (j = 0; j < z_max; j++) {
            if ((mFI_CheckBlockKind_OR(random_x, random_z,
                                       mRF_BLOCKKIND_PLAYER | mRF_BLOCKKIND_SHOP | mRF_BLOCKKIND_STATION |
                                           mRF_BLOCKKIND_POOL | mRF_BLOCKKIND_OCEAN | mRF_BLOCKKIND_ISLAND |
                                           mRF_BLOCKKIND_OFFING) == 0) &&
                (mNpc_GetMakeUtNuminBlock_hard_area(&ut_x, &ut_z, random_x, random_z, 2)) == TRUE) {
                mFI_BkandUtNum2CenterWpos(pos, random_x, random_z, ut_x, ut_z);
                return TRUE;
            }

            if (random_z == z_max - 1) {
                random_z = 0;
            } else {
                random_z += 1;
            }
        }

        if (random_x == x_max - 1) {
            random_x = 0;
        } else {
            random_x += 1;
        }
    }
    return FALSE;
}

static void aBALL_actor_ct(ACTOR* actor, GAME* game) {
    BALL_ACTOR* ball = (BALL_ACTOR*)actor;
    GAME_PLAY* play = (GAME_PLAY*)game;

    Global_Actor_p = ball;

    if ((0.0f == Common_Get(ball_pos).x) && (0.0f == Common_Get(ball_pos).y) && (0.0f == Common_Get(ball_pos).z)) {
        if (aBALL_Random_pos_set(&actor->world.position) == 0) {
            actor->world.position = actor->home.position;
        }
        actor->world.position.y = mCoBG_GetBgY_AngleS_FromWpos(NULL, actor->world.position, 0.0f);
        Common_Set(ball_type, RANDOM(3.0f));
        Common_Set(ball_pos, actor->world.position);
        ball->type = Common_Get(ball_type);
    } else {
        actor->world.position = Common_Get(ball_pos);
        ball->type = Common_Get(ball_type);
    }
    Common_Get(clip).ball_redma_proc = NULL;
    Shape_Info_init(actor, 0.0f, &mAc_ActorShadowEllipse, 9.0f, 17.0f);
    ClObjPipe_ct(game, &ball->ball_pipe);
    ClObjPipe_set5(game, &ball->ball_pipe, actor, &aBALL_CoInfoData);
    CollisionCheck_Status_set3(&actor->status_data, &aBALL_StatusData);
    ball->unk206 = 3;
    aBALL_process_ground_init(actor, game);
    ball->collider = NULL;
    actor->max_velocity_y = -20.0f;
    actor->gravity = 0.3f;
    actor->speed = 0.0f;

    ball->ball_max_speed = 0.0f;
    ball->ball_acceleration = 0.06f;
    ball->ball_speed = 0.0f;

    actor->scale.x = 0.01f;
    actor->scale.y = 0.01f;
    actor->scale.z = 0.01f;

    ball->angle.x = qrand();
    ball->angle.y = qrand();
    ball->angle.z = qrand();

    ball->unk20A = 0;
    ball->unk20C = 0;
#ifdef VITA_MP
    aBALL_mp_mine_ms = 0;
    aBALL_mp_heard_ok = FALSE;
    aBALL_mp_timer = 0;
    aBALL_mp_ripple = 0;
#endif
}

static void aBALL_actor_dt(ACTOR* actor, GAME* game) {
    BALL_ACTOR* ball = (BALL_ACTOR*)actor;

    if ((ball->state_flags & aBALL_STATE_DEAD) || (ball->state_flags & aBALL_STATE_IN_HOLE) ||
        (mRlib_Set_Position_Check(actor) == 0)) {
        Common_Set(ball_pos, ZeroVec);
    } else {
        Common_Set(ball_pos, actor->world.position);
    }
    Common_Get(clip).ball_redma_proc = NULL;
    ClObjPipe_dt(game, &ball->ball_pipe);
}

static void aBALL_position_move(BALL_ACTOR* actor) {
    xyz_t pos;
    s_xyz angle;

    pos = actor->actor_class.world.position;

    mCoBG_GetBgY_AngleS_FromWpos(&angle, pos, 0.0f);

    if ((actor->actor_class.bg_collision_check.result.on_ground) ||
        (actor->actor_class.bg_collision_check.result.is_in_water)) {
        chase_f(&actor->actor_class.speed, actor->ball_max_speed, actor->ball_acceleration);
    }

    if (!(actor->state_flags & aBALL_STATE_IN_HOLE)) {
        mRlib_spdF_Angle_to_spdXZ(&actor->actor_class.position_speed, &actor->actor_class.speed,
                                  &actor->actor_class.world.angle.y);
        chase_f(&actor->actor_class.position_speed.y, actor->actor_class.max_velocity_y, actor->actor_class.gravity);

        mRlib_position_move_for_sloop(&actor->actor_class, &angle);

        if (actor->actor_class.world.position.z < 840.0f) {
            actor->actor_class.world.position.z = 840.0f;
        }
    }
}

static void aBALL_BGcheck(BALL_ACTOR* actor) {
    f32 speed_y;
    s16 hit_angle;
    s16 rot;
    xyz_t pos_speed;
    f32 sin;
    f32 cos;
    f32 speed;
    f32 speed_factor;
    f32 sincos;

    speed_y = actor->actor_class.position_speed.y;

    if (((actor->process_proc == aBALL_process_air_water) || (actor->process_proc == aBALL_process_ground_water)) ||
        (actor->actor_class.bg_collision_check.result.unit_attribute == 11 ||
         actor->actor_class.bg_collision_check.result.unit_attribute == 22)) {
        mCoBG_BgCheckControll(&actor->bgpos, &actor->actor_class, 12.0f, -12.0f, 0, 1, 0);

        if ((actor->actor_class.bg_collision_check.result.unit_attribute == 11 ||
             actor->actor_class.bg_collision_check.result.unit_attribute == 22) &&
            (actor->actor_class.bg_collision_check.result.on_ground)) {
            f32 bg_y = actor->bgpos.y;

            bg_y *= (0.1f + GETREG(TAKREG, 7) * 0.01f);
            actor->actor_class.world.position.y += bg_y;
        }
        actor->actor_class.world.position.x += actor->bgpos.x;
        actor->actor_class.world.position.z += actor->bgpos.z;
        actor->actor_class.world.position.x += actor->bgpos.x;
        actor->actor_class.world.position.z += actor->bgpos.z;
    } else {
        mCoBG_BgCheckControll(&actor->bgpos, &actor->actor_class, 12.0f, -12.0f, 0, 0, 0);
        mRlib_Station_step_modify_to_wall(&actor->actor_class);
    }

    if (((actor->process_proc == aBALL_process_air) || (actor->process_proc == aBALL_process_air_water)) &&
        actor->actor_class.bg_collision_check.result.on_ground) {
        if (actor->unk206 < 3) {
            actor->unk206++;
            if (actor->actor_class.bg_collision_check.result.is_in_water) {
                actor->actor_class.position_speed.y = 0.2f;
            } else {
                actor->actor_class.position_speed.y = 0.7f * -speed_y;
            }
        }
    }

    if (actor->actor_class.bg_collision_check.result.hit_wall & mCoBG_HIT_WALL) {
        hit_angle = mRlib_Get_HitWallAngleY(&actor->actor_class);
        rot = actor->actor_class.world.angle.y - (hit_angle + 0x8000);
        if (ABS(rot) < 0x4000) {
            pos_speed = actor->actor_class.position_speed;
            sin = sin_s(hit_angle);
            cos = cos_s(hit_angle);
            sincos = sin * cos;
            speed = -((pos_speed.z * cos) + (pos_speed.x * sin));
            speed_factor = (speed * 0.07f) + 1.2f;
            if (speed > 1.0f) {
                aBALL_SE_SPEED(speed, 0x8026, &actor->actor_class.world.position);
            }

            actor->actor_class.position_speed.z =
                ((1.0f - (speed_factor * cos * cos)) * pos_speed.z) - (pos_speed.x * speed_factor * sincos);
            actor->actor_class.position_speed.x =
                (-pos_speed.z * speed_factor * sincos) + (pos_speed.x * (1.0f - (speed_factor * sin * sin)));
            mRlib_spdXZ_to_spdF_Angle(&actor->actor_class.position_speed, &actor->actor_class.speed,
                                      &actor->actor_class.world.angle.y);
        }
    }
}

static void aBALL_OBJcheck(BALL_ACTOR* actor, GAME* _p1) {
    int wade;
    ACTOR* collided;
    xyz_t pos_speed;
    s16 angle;
    f32 sin;
    f32 cos;
    f32 fact;
    f32 colliderSpeed;
    f32 collidedSpeed;
    f32 abs;
    f32 newSins;
    f32 newCos;
    f32 newSpeedX;
    f32 newSpeedZ;
    f32 newFact;
    f32 speedFactor;
    int speedAngle;
    xyz_t collisionSpeed;
    xyz_t collision;

    wade = mFI_GetPlayerWade();

    if (ClObj_DID_COLLIDE(actor->ball_pipe.collision_obj)) {
        collided = actor->ball_pipe.collision_obj.collided_actor;
        actor->ball_pipe.collision_obj.collision_flags0 &= ~ClObj_FLAG_COLLIDED;

#ifdef VITA_MP
        if (aBALL_mp_remote_bump(collided)) {
            // another player's bump plays out on that player's screen
        } else
#endif
        if (mQst_CheckSoccerTarget(collided) != 0) {
            mQst_NextSoccer(collided);
            actor->actor_class.speed = 0.0f;
            actor->actor_class.position_speed = ZeroVec;
        } else if ((collided != NULL) && (!(actor->state_flags & aBALL_STATE_IN_HOLE)) && (wade != mFI_WADE_START) &&
                   (wade != mFI_WADE_INPROGRESS)) {
#ifdef VITA_MP
            if (aBALL_mp_local_kicker(collided, _p1)) {
                aBALL_mp_claim();
            }
#endif
            if (actor->collider != collided) {
                pos_speed = collided->position_speed;
                actor->collider = collided;
                actor->unk20C = GETREG(TAKREG, 15) + 30;
                angle = atans_table(actor->actor_class.world.position.z - collided->world.position.z,
                                    actor->actor_class.world.position.x - collided->world.position.x);
                sin = sin_s(angle);
                cos = cos_s(angle);
                colliderSpeed =
                    (sin * actor->actor_class.position_speed.x) + (cos * actor->actor_class.position_speed.z);

                fact = sqrtf((pos_speed.x * pos_speed.x) + (pos_speed.z * pos_speed.z));

                xyz_t_mult_v(&pos_speed, ((24.0f / 180.0f) * fact) * 0.9f + 0.1f);
                collidedSpeed = (sin * pos_speed.x) + (cos * pos_speed.z);
                abs = ABS(colliderSpeed + collidedSpeed);

                newSins = abs * sin_s(angle);
                newCos = abs * cos_s(angle);

                newSpeedX = actor->actor_class.position_speed.x + newSins;
                newSpeedZ = actor->actor_class.position_speed.z + newCos;

                newFact = sqrtf((newSpeedX * newSpeedX) + (newSpeedZ * newSpeedZ));
                newFact = CLAMP_MAX(newFact, 11.0f);

                speedFactor = newFact / 11.0f;
                if (actor->actor_class.bg_collision_check.result.on_ground) {
                    /* TODO: this is fakematch right? */
                    if (!actor->actor_class.speed) {
                        f32 angle = ((speedFactor * 90.0f) + (speedFactor * 35.0f) * fqrand2());
                        speedAngle = DEG2SHORT_ANGLE(angle);

                        actor->actor_class.speed = cos_s(speedAngle) * newFact;
                        actor->actor_class.position_speed.y = sin_s(speedAngle) * newFact;
                    } else {
                        actor->actor_class.speed = newFact * 0.75f;
                    }
                } else {
                    actor->actor_class.speed = newFact * 0.75f;
                }

                actor->actor_class.world.angle.y = atans_table(newSpeedZ, newSpeedX);
                actor->actor_class.speed *= 0.9f;
                aBALL_SE_SPEED(actor->actor_class.speed, NA_SE_25, &actor->actor_class.world.position);
                actor->unk20C = GETREG(TAKREG, 15) + 30;
            } else {
                collision = actor->actor_class.status_data.collision_vec;

                xyz_t_add(&actor->actor_class.position_speed, &collision, &collisionSpeed);

                if ((wade != mFI_WADE_START) && (wade != mFI_WADE_INPROGRESS)) {
                    actor->actor_class.speed =
                        sqrtf((collisionSpeed.x * collisionSpeed.x) + (collisionSpeed.z * collisionSpeed.z));
                    actor->actor_class.speed = CLAMP_MAX(actor->actor_class.speed, 11.0f);
                    actor->actor_class.world.angle.y = atans_table(collisionSpeed.z, collisionSpeed.x);
                }
            }

        } else {
            if (actor->unk20C <= 0) {
                actor->collider = NULL;
            } else {
                actor->unk20C--;
            }
        }
    } else {
        if (actor->unk20C <= 0) {
            actor->collider = NULL;
        } else {
            actor->unk20C--;
        }
    }
}

static void aBALL_House_Tree_Rev_Check(BALL_ACTOR* actor) {
    if (mRlib_HeightGapCheck_And_ReversePos(&actor->actor_class) != 1) {
        actor->state_flags |= aBALL_STATE_DEAD;
        Actor_delete(&actor->actor_class);
    }
}

static void aBALL_process_air_init(ACTOR* actor, GAME* game) {
    BALL_ACTOR* ball = (BALL_ACTOR*)actor;
    f32 bg_y;

    bg_y = mCoBG_GetBgY_AngleS_FromWpos(NULL, actor->world.position, 0.0f);
    actor->shape_info.draw_shadow = TRUE;

    if ((ball->process_proc == aBALL_process_ground) && ((actor->world.position.y - bg_y) > 20.0f)) {
        aBALL_SE(NA_SE_43D, &actor->world.position);
    }

    ball->process_proc = aBALL_process_air;
}

static void aBALL_process_air(ACTOR* actor, GAME* game) {
    BALL_ACTOR* ball = (BALL_ACTOR*)actor;

    ball->ball_acceleration = 0.0f;
    add_calc0(&ball->ball_y, 0.5f, 100.0f);
    actor->max_velocity_y = -20.0f;
    actor->gravity = 0.3f;
    ball->ball_speed = actor->speed;
    if (actor->bg_collision_check.result.on_ground) {
        if (actor->bg_collision_check.result.is_in_water) {
            aBALL_process_ground_water_init(actor, game);
        } else {
            aBALL_process_ground_init(actor, game);
        }
    } else if (actor->bg_collision_check.result.is_in_water) {
        aBALL_process_air_water_init(actor, game);
    }
}

static void aBALL_process_ground_init(ACTOR* actor, GAME* game) {
    BALL_ACTOR* ball = (BALL_ACTOR*)actor;

    actor->shape_info.draw_shadow = TRUE;
    if (actor->position_speed.y > 0.0f) {
        ball->process_proc = aBALL_process_air;
    } else {
        ball->process_proc = aBALL_process_ground;
    }
}

static void aBALL_process_ground(ACTOR* actor, GAME* game) {
    BALL_ACTOR* ball = (BALL_ACTOR*)actor;
    f32 temp;
    xyz_t norm;
    s16 angle_rate;
    s16 angle;
    f32 distance;

    f32 speed_x;
    f32 speed_z;
    s16 effect_type;

    mCoBG_GetBgNorm_FromWpos(&norm, actor->world.position);

    if (mRlib_Get_ground_norm_inHole(actor, &norm, &distance, &angle, &angle_rate, 1.0f) != 0) {
        f32 dist;
        f32 distance_add;

        distance_add = (distance - 40.0f) - 5.0f;
        dist = 0.0f;

        if (distance_add < 0.0f) {
            dist = distance_add;
        }

        dist *= 25.0f;
        add_calc(&ball->ball_y, dist, 0.5f, 200.0f, 5.0f);
        actor->position_speed.x *= 0.83666f;
        actor->position_speed.z *= 0.83666f;
        if (distance < 1.0f) {
            speed_x = ABS(actor->position_speed.x);
            if (speed_x < 1.0f) {
                speed_z = ABS(actor->position_speed.z);
                if (speed_z < 1.0f) {
                    ball->state_flags |= aBALL_STATE_IN_HOLE;
                    ball->ball_pipe.attribute.pipe.height = 20;
                    ball->ball_pipe.attribute.pipe.radius = 18;
                    actor->status_data.weight = MASSTYPE_HEAVY;
                    actor->speed = 0.0f;
                    return;
                }
            }
        }
    } else {
        mRlib_Get_norm_Clif(actor, &norm);
        add_calc0(&ball->ball_y, 0.5f, 100.0f);
    }

    if (!F32_IS_ZERO(norm.x) || !F32_IS_ZERO(norm.z)) {
        if (Math3d_normalizeXyz_t(&norm)) {
            actor->position_speed.x += 1.35f * norm.x;
            actor->position_speed.z += 1.35f * norm.z;
            mRlib_spdXZ_to_spdF_Angle(&actor->position_speed, &ball->ball_max_speed, &actor->world.angle.y);
            ball->ball_max_speed = (ball->ball_max_speed < 8.0f) ? ball->ball_max_speed : 8.0f;
            ball->ball_acceleration = 0.05f;
        }
    } else {
        ball->ball_max_speed = 0.0f;
        ball->ball_acceleration = 0.06f;
    }

    actor->max_velocity_y = -20.0f;
    actor->gravity = 0.3f;
    ball->ball_speed = actor->speed;

    if ((actor->bg_collision_check.result.is_in_water) || (actor->bg_collision_check.result.unit_attribute) == 11) {
        if (actor->bg_collision_check.result.on_ground != 0) {
            aBALL_process_ground_water_init(actor, game);
        } else {
            ball->unk206 = 0;
            aBALL_process_air_water_init(actor, game);
        }
    } else if (actor->bg_collision_check.result.on_ground == 0) {
        ball->unk206 = 0;
        aBALL_process_air_init(actor, game);
        return;
    }

    if (!(game->frame_counter & 7) && (actor->bg_collision_check.result.unit_attribute) == 9) {
        if (actor->speed > 1.0f) {
            if (actor->speed > 4.0f) {
                effect_type = 1;
            } else {
                effect_type = 0;
            }

            Common_Get(clip).effect_clip->effect_make_proc(eEC_EFFECT_BUSH_HAPPA, actor->world.position, 1,
                                                           actor->world.angle.y, game, actor->npc_id, 0, effect_type);
        }
    }
}

static void aBALL_set_spd_relations_in_water(ACTOR* actor, GAME* game) {
    static s16 angl_add_table[] = {
        0x100,
        0x400,
    };
    BALL_ACTOR* ball = (BALL_ACTOR*)actor;
    xyz_t pos_flow;
    f32 height;

    s16 angle;

    int apply_angle;

    height = mCoBG_GetWaterHeight_File(actor->world.position, __FILE__, 0x361);
    add_calc0(&ball->ball_y, 0.5f, 100.0f);
    mCoBG_GetWaterFlow(&pos_flow, actor->bg_collision_check.result.unit_attribute);

    angle = atans_table(pos_flow.z, pos_flow.x);
    apply_angle = ABS((s16)(actor->world.angle.y - angle));

    chase_angle(&actor->world.angle.y, angle, angl_add_table[apply_angle > 0x4000]);

    if (actor->world.position.y < height) {
        actor->max_velocity_y = 1.0f;
    } else {
        actor->max_velocity_y = -1.0f;
    }

    if (ball->timer < 0x20) {
        if (!(game->frame_counter & 3) && (ball->timer < 0x10) || !(game->frame_counter & 7)) {
            Common_Get(clip).effect_clip->effect_make_proc(eEC_EFFECT_TURI_HAMON, actor->world.position, 1,
                                                           actor->world.angle.y, game, actor->npc_id, 1, 0);
        }
        ball->timer++;
    }

    actor->gravity = 0.1f;
    ball->ball_max_speed = 1.0f;
    ball->ball_acceleration = 0.1f;
}

static void aBALL_process_air_water_init(ACTOR* actor, GAME* game) {
    BALL_ACTOR* ball = (BALL_ACTOR*)actor;

    actor->shape_info.draw_shadow = 0;
    ball->process_proc = aBALL_process_air_water;
}

static void aBALL_process_air_water(ACTOR* actor, GAME* game) {
    BALL_ACTOR* ball = (BALL_ACTOR*)actor;
    GAME_PLAY* play = (GAME_PLAY*)game;
    f32 ball_speed;

    aBALL_set_spd_relations_in_water(actor, game);
    add_calc0(&ball->ball_y, 0.5f, 100.0f);

    ball_speed = ball->ball_speed;
    ball_speed -= 0.5f;
    if (ball_speed < 0.0f) {
        ball_speed = 0.0f;
    }
    ball->ball_speed = ball_speed;

    if (Common_Get(clip).gyo_clip != NULL) {
        Common_Get(clip).gyo_clip->ballcheck_gyoei_proc(&actor->world.position, 20.0f, 1);
    }
    if (actor->bg_collision_check.result.on_ground) {
        if (actor->bg_collision_check.result.is_in_water) {
            aBALL_process_ground_water_init(actor, game);
        } else if (actor->bg_collision_check.result.unit_attribute != 11) {
            aBALL_process_ground_init(actor, game);
        }
    } else if (!actor->bg_collision_check.result.is_in_water) {
        aBALL_process_air_init(actor, game);
    }
}

static void aBALL_process_ground_water_init(ACTOR* actor, GAME* game) {
    BALL_ACTOR* ball = (BALL_ACTOR*)actor;

    actor->shape_info.draw_shadow = 0;
    ball->timer = 0;
    ball->process_proc = aBALL_process_ground_water;
}

static void aBALL_process_ground_water(ACTOR* actor, GAME* game) {
    BALL_ACTOR* ball = (BALL_ACTOR*)actor;

    u32 currentUT;
    xyz_t* pos;
    f32 height;

    currentUT = actor->bg_collision_check.result.unit_attribute;
    aBALL_set_spd_relations_in_water(actor, game);
    ball->ball_speed = actor->speed;

    if (Common_Get(clip).gyo_clip != NULL) {
        Common_Get(clip).gyo_clip->ballcheck_gyoei_proc(&actor->world.position, 20.0f, 1);
    }

    if (actor->bg_collision_check.result.on_ground) {
        if (!(actor->bg_collision_check.result.is_in_water) && (currentUT != 11) && currentUT != 22) {
            aBALL_process_ground_init(actor, game);
        }
    } else if (!ball->actor_class.bg_collision_check.result.is_in_water) {
        aBALL_process_air_init(actor, game);
    } else {
        aBALL_process_air_water_init(actor, game);
    }

    if (currentUT == 11 || currentUT == 22) {
        pos = &ball->bgpos;

        actor->world.position.y += (0.5f * pos->y);

        if (currentUT == 22) {
            height = ABS(pos->y);

            if (height < 1.0f) {
                aBALL_process_ground_init(actor, game);
            }
        }
    }
}

static void aBALL_calc_axis(ACTOR* actor) {
    BALL_ACTOR* ball = (BALL_ACTOR*)actor;
    s16 angle;
    f32 speed_fact;

    angle = (actor->speed * 434.81952f);

    if (ball->process_proc == aBALL_process_air_water || ball->process_proc == aBALL_process_ground_water) {
        speed_fact = ((-1.0f) - actor->position_speed.y) / (-2.0f);
        angle *= sin_s(DEG2SHORT_ANGLE2(30.0f + (60.0f * speed_fact)));
    }

    mRlib_Roll_Matrix_to_s_xyz(actor, &ball->angle, angle);
}

static int aBALL_player_angle_distance_check(ACTOR* actor, PLAYER_ACTOR* player) {
    f32 distance;
    s16 angle;
    int abs_angle;

    distance = search_position_distance(&actor->world.position, &player->actor_class.world.position);
    angle = player->actor_class.shape_info.rotation.y -
            search_position_angleY(&player->actor_class.world.position, &actor->world.position);

    if (distance < 60.0f) {
        abs_angle = ABS(angle);

        if (abs_angle < 0x2000) {
            return 1;
        }
    }

    return 0;
}

static void aBALL_status_check(ACTOR* actor, GAME* game) {
    BALL_ACTOR* ball = (BALL_ACTOR*)actor;
    GAME_PLAY* play = (GAME_PLAY*)game;
    PLAYER_ACTOR* player;
    PLAYER_ACTOR* player2;
    int i;

    if (ball->state_flags & aBALL_STATE_PLAYER_HIT_SCOOP) {
        player = GET_PLAYER_ACTOR(play);
        ball->state_flags &= ~aBALL_STATE_PLAYER_HIT_SCOOP;
        if (aBALL_player_angle_distance_check(actor, player) || F32_IS_ZERO(actor->speed)) {
            actor->world.angle.y = player->actor_class.shape_info.rotation.y;
            actor->speed = 2.0f;
            actor->position_speed.y = 4.5f;
            if (ball->state_flags & aBALL_STATE_IN_HOLE) {
                ball->ball_pipe.attribute.pipe.height = 30;
                ball->ball_pipe.attribute.pipe.radius = 13;
                ball->state_flags &= ~aBALL_STATE_IN_HOLE;
                actor->status_data.weight = 0x64;
            }
        }
    }
    if (ball->state_flags & aBALL_STATE_PLAYER_HIT_AXE) {
        ball->state_flags &= ~aBALL_STATE_PLAYER_HIT_AXE;
        if (!(ball->state_flags & aBALL_STATE_IN_HOLE)) {
            player2 = GET_PLAYER_ACTOR(play);
            if (aBALL_player_angle_distance_check(actor, player2) || F32_IS_ZERO(actor->speed)) {
                actor->world.angle.y = player2->actor_class.shape_info.rotation.y + 0x2000;
                actor->speed = 4.5f;
                actor->position_speed.y = 3.0f;
            }
        }
    }

    if (!(ball->state_flags & aBALL_STATE_DEAD)) {
        if (actor->bg_collision_check.result.is_in_water) {
            aBALL_SE(NA_SE_27, &actor->world.position);
            ball->state_flags |= aBALL_STATE_DEAD;
            if (Common_Get(clip).gyo_clip != NULL) {
                Common_Get(clip).gyo_clip->ballcheck_gyoei_proc(&actor->world.position, 20.0f, 0);
            }
            ball->ball_pipe.attribute.pipe.height = 10;
            Common_Get(clip).effect_clip->effect_make_proc(eEC_EFFECT_AMI_MIZU, actor->world.position, 1, 0, game,
                                                           actor->npc_id, 1, 0);

            for (i = 2; i < 6; i++) {
                Common_Get(clip).effect_clip->effect_make_proc(eEC_EFFECT_MIZUTAMA, actor->world.position, 1,
                                                               actor->world.angle.y, game, actor->npc_id, 0,
                                                               i | 0x3000);
            }
        }
    }
}

#ifdef VITA_MP
static int aBALL_mp_mode(BALL_ACTOR* ball) {
    if (ball->process_proc == aBALL_process_air) {
        return aBALL_MP_AIR;
    }
    if (ball->process_proc == aBALL_process_air_water) {
        return aBALL_MP_AIR_WATER;
    }
    if (ball->process_proc == aBALL_process_ground_water) {
        return aBALL_MP_GROUND_WATER;
    }
    return aBALL_MP_GROUND;
}

// this game moves the ball: while it rolls every frame, at rest now and then (the host speaks for a ball
// nobody has kicked)
static void aBALL_mp_report(BALL_ACTOR* ball) {
    ACTOR* actor = (ACTOR*)ball;
    int mode = aBALL_mp_mode(ball);
    mp_ball_t b;

    if (aBALL_mp_mine_ms == 0 && !mp_is_host()) {
        return;
    }
    if (actor->speed == 0.0f && mode == aBALL_MP_GROUND && ++aBALL_mp_timer < aBALL_MP_REST_EVERY) {
        return;
    }
    aBALL_mp_timer = 0;
    b.pos[0] = actor->world.position.x;
    b.pos[1] = actor->world.position.y;
    b.pos[2] = actor->world.position.z;
    b.speed = actor->speed;
    b.vel_y = actor->position_speed.y;
    b.ball_y = ball->ball_y;
    b.angle_y = actor->world.angle.y;
    b.mode = (u8)mode;
    b.flags = (u8)(ball->state_flags & (aBALL_STATE_DEAD | aBALL_STATE_IN_HOLE));
    b.type = (u8)ball->type;
    b.snd = aBALL_mp_snd;
    b.snd_id = aBALL_mp_snd_id;
    b.snd_speed = aBALL_mp_snd_speed;
    b.kick_ms = aBALL_mp_mine_ms;
    pc_mp_ball_report(&b);
}

// the ball as another game moves it; its thuds, splashes and leaves happen here too
static void aBALL_mp_apply(BALL_ACTOR* ball, GAME* game, const mp_ball_t* b, u32 age_ms) {
    static BALL_PROCESS_PROC mode_proc[] = { aBALL_process_ground, aBALL_process_air, aBALL_process_air_water,
                                             aBALL_process_ground_water };
    ACTOR* actor = (ACTOR*)ball;
    xyz_t to;
    f32 ahead = (f32)age_ms * (60.0f / 1000.0f);
    f32 dx;
    f32 dz;

    if (ahead > aBALL_MP_AHEAD) {
        ahead = aBALL_MP_AHEAD;
    }

    // where it has rolled on to since the report
    to.x = b->pos[0] + b->speed * sin_s(b->angle_y) * ahead;
    to.y = b->pos[1];
    to.z = b->pos[2] + b->speed * cos_s(b->angle_y) * ahead;
    dx = to.x - actor->world.position.x;
    dz = to.z - actor->world.position.z;
    if (dx * dx + dz * dz > aBALL_MP_SNAP * aBALL_MP_SNAP) {
        actor->world.position = to;
    } else {
        actor->world.position.x += dx * 0.5f;
        actor->world.position.y += (to.y - actor->world.position.y) * 0.5f;
        actor->world.position.z += dz * 0.5f;
    }
    actor->speed = b->speed;
    actor->position_speed.y = b->vel_y;
    actor->world.angle.y = b->angle_y;
    ball->ball_y = b->ball_y;
    if (b->mode < 4) {
        ball->process_proc = mode_proc[b->mode];
        actor->shape_info.draw_shadow = b->mode == aBALL_MP_GROUND || b->mode == aBALL_MP_AIR;
    }
    if (b->type < 3) {
        ball->type = b->type;
        Common_Set(ball_type, b->type);
    }
    if ((b->flags & aBALL_STATE_IN_HOLE) && !(ball->state_flags & aBALL_STATE_IN_HOLE)) {
        ball->state_flags |= aBALL_STATE_IN_HOLE;
        ball->ball_pipe.attribute.pipe.height = 20;
        ball->ball_pipe.attribute.pipe.radius = 18;
        actor->status_data.weight = MASSTYPE_HEAVY;
    } else if (!(b->flags & aBALL_STATE_IN_HOLE) && (ball->state_flags & aBALL_STATE_IN_HOLE)) {
        ball->state_flags &= ~aBALL_STATE_IN_HOLE;
        ball->ball_pipe.attribute.pipe.height = 30;
        ball->ball_pipe.attribute.pipe.radius = 13;
        actor->status_data.weight = 0x64;
    }
    if (!(b->flags & aBALL_STATE_DEAD)) {
        ball->state_flags &= ~aBALL_STATE_DEAD;
    } else if (!(ball->state_flags & aBALL_STATE_DEAD)) {
        int i;

        ball->state_flags |= aBALL_STATE_DEAD;
        ball->ball_pipe.attribute.pipe.height = 10;
        Common_Get(clip).effect_clip->effect_make_proc(eEC_EFFECT_AMI_MIZU, actor->world.position, 1, 0, game,
                                                       actor->npc_id, 1, 0);
        for (i = 2; i < 6; i++) {
            Common_Get(clip).effect_clip->effect_make_proc(eEC_EFFECT_MIZUTAMA, actor->world.position, 1,
                                                           actor->world.angle.y, game, actor->npc_id, 0,
                                                           i | 0x3000);
        }
    }
    if (aBALL_mp_heard_ok && b->snd != aBALL_mp_heard) {
        if (b->snd_speed > 0.0f) {
            sAdo_OngenTrgStartSpeed(b->snd_speed, b->snd_id, &actor->world.position);
        } else {
            sAdo_OngenTrgStart(b->snd_id, &actor->world.position);
        }
    }
    aBALL_mp_heard = b->snd;
    aBALL_mp_heard_ok = TRUE;
    if (b->mode == aBALL_MP_GROUND && !(game->frame_counter & 7) && actor->speed > 1.0f &&
        mCoBG_Wpos2Attribute(actor->world.position, NULL) == 9) {
        Common_Get(clip).effect_clip->effect_make_proc(eEC_EFFECT_BUSH_HAPPA, actor->world.position, 1,
                                                       actor->world.angle.y, game, actor->npc_id, 0,
                                                       actor->speed > 4.0f);
    }
    if (b->mode == aBALL_MP_AIR_WATER || b->mode == aBALL_MP_GROUND_WATER) {
        if (aBALL_mp_ripple < 0x20 && !(game->frame_counter & 7)) {
            Common_Get(clip).effect_clip->effect_make_proc(eEC_EFFECT_TURI_HAMON, actor->world.position, 1,
                                                           actor->world.angle.y, game, actor->npc_id, 1, 0);
        }
        if (aBALL_mp_ripple < 0x20) {
            aBALL_mp_ripple++;
        }
    } else {
        aBALL_mp_ripple = 0;
    }
    Common_Set(ball_pos, actor->world.position);
}

// TRUE while another game's newer kick moves the ball; the local player's own kick takes it back
static int aBALL_mp_follow(BALL_ACTOR* ball, GAME* game) {
    ACTOR* actor = (ACTOR*)ball;
    GAME_PLAY* play = (GAME_PLAY*)game;
    mp_ball_t b;
    int slot;
    unsigned int age_ms;

    if (!mp_ball_remote(&b, &slot, &age_ms)) {
        // the mover went quiet (a menu, a lost line): this game takes the ball on from where it is
        if (aBALL_mp_heard_ok) {
            aBALL_mp_claim();
        }
        aBALL_mp_heard_ok = FALSE;
        return FALSE;
    }
    if (aBALL_mp_older(b.kick_ms, aBALL_mp_mine_ms) || (b.kick_ms == aBALL_mp_mine_ms && slot > mp_lobby_self_slot())) {
        aBALL_mp_heard_ok = FALSE;
        return FALSE;
    }
    if ((ClObj_DID_COLLIDE(ball->ball_pipe.collision_obj) &&
         aBALL_mp_local_kicker(ball->ball_pipe.collision_obj.collided_actor, game)) ||
        (ball->state_flags & (aBALL_STATE_PLAYER_HIT_SCOOP | aBALL_STATE_PLAYER_HIT_AXE))) {
        // this frame's physics plays the kick out from where the mover had it
        aBALL_mp_claim();
        aBALL_mp_heard_ok = FALSE;
        return FALSE;
    }
    aBALL_mp_apply(ball, game, &b, age_ms);
    if (ball->unk20C <= 0) {
        ball->collider = NULL;
    } else {
        ball->unk20C--;
    }
    ball->ball_pipe.collision_obj.collision_flags0 &= ~ClObj_FLAG_COLLIDED;
    CollisionCheck_Uty_ActorWorldPosSetPipeC(actor, &ball->ball_pipe);
    CollisionCheck_setOC(game, &play->collision_check, &ball->ball_pipe.collision_obj);
    aBALL_calc_axis(actor);
    return TRUE;
}
#endif

static void aBALL_actor_move(ACTOR* actor, GAME* game) {
    BALL_ACTOR* ball = (BALL_ACTOR*)actor;
    GAME_PLAY* play = (GAME_PLAY*)game;

    aBALL_House_Tree_Rev_Check(ball);
#ifdef VITA_MP
    if (aBALL_mp_follow(ball, game)) {
        return;
    }
#endif

    if (!(actor->state_bitfield & ACTOR_STATE_NO_CULL)) {
        if (actor->bg_collision_check.result.is_in_water || (ball->state_flags & aBALL_STATE_IN_HOLE)) {
            Actor_delete(actor);
        }
        if (actor->speed == 0.0f) {
            return;
        }
    }
    Common_Set(ball_pos, actor->world.position);
    aBALL_position_move(ball);
    ball->process_proc(actor, game);
    aBALL_BGcheck(ball);
    aBALL_OBJcheck(ball, game);

    CollisionCheck_Uty_ActorWorldPosSetPipeC(&ball->actor_class, &ball->ball_pipe);
    CollisionCheck_setOC(game, &play->collision_check, &ball->ball_pipe.collision_obj);
    aBALL_calc_axis(actor);
#ifdef VITA_MP
    if (ball->state_flags & (aBALL_STATE_PLAYER_HIT_SCOOP | aBALL_STATE_PLAYER_HIT_AXE)) {
        aBALL_mp_claim();
    }
#endif
    aBALL_status_check(actor, game);
#ifdef VITA_MP
    aBALL_mp_report(ball);
#endif
}

static void aBALL_actor_draw(ACTOR* actor, GAME* game) {
    BALL_ACTOR* ball = (BALL_ACTOR*)actor;
    GRAPH* graph;
    Gfx* gfx;

    graph = game->graph;

    OPEN_DISP(graph);
    gfx = NOW_POLY_OPA_DISP;
    Matrix_translate(0.0f, ball->ball_y, 0.0f, MTX_MULT);
    Matrix_rotateXYZ(ball->angle.x, ball->angle.y, ball->angle.z, MTX_MULT);
    gDPPipeSync(gfx++);
    gSPMatrix(gfx++, _Matrix_to_Mtx_new(graph), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPDisplayList(gfx++, ball_model_tbl[ball->type]);
    NOW_POLY_OPA_DISP = gfx;
    CLOSE_DISP(graph);
}
