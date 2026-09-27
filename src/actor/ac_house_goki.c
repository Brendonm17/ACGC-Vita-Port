#include "ac_house_goki.h"

#include "m_actor_shadow.h"
#include "m_play.h"
#include "m_player_lib.h"
#include "m_common_data.h"
#include "m_cockroach.h"
#include "sys_matrix.h"
#include "m_rcp.h"
#ifdef VITA_MP
#include "pc_mp.h"
#include "m_field_info.h"
#include "m_scene_table.h"
#endif

enum {
    aHG_ACT_AWAY,
    aHG_ACT_JUMP_AWAY,
    aHG_ACT_WAIT,
    aHG_ACT_MOVE,
    aHG_ACT_DEAD,
    
    aHG_ACT_NUM
};

static void aHG_actor_ct(ACTOR* actorx, GAME* game);
static void aHG_actor_move(ACTOR* actorx, GAME* game);
static void aHG_actor_draw(ACTOR* actorx, GAME* game);

// clang-format off
ACTOR_PROFILE House_Goki_Profile = {
    mAc_PROFILE_HOUSE_GOKI,
    ACTOR_PART_BG,
    ACTOR_STATE_NO_MOVE_WHILE_CULLED,
    EMPTY_NO,
    ACTOR_OBJ_BANK_HOUSE_GOKI,
    sizeof(HOUSE_GOKI_ACTOR),
    &aHG_actor_ct,
    mActor_NONE_PROC1,
    &aHG_actor_move,
    &aHG_actor_draw,
    NULL,
};
// clang-format on

static void aHG_setupAction(HOUSE_GOKI_ACTOR* goki, GAME* game, int action);

#ifdef VITA_MP
// the lowest slot of the players in a room runs its cockroaches; the others' games show copies that follow them
#define aHG_MP_COPY_ARG  (-0x100) // actor_specific below this: a copy of the runner's cockroach (this less it)
#define aHG_MP_KILLED    4
#define aHG_MP_KILLED_MS 10000 // a copy stepped on here isn't brought back while its runner hasn't heard

static int aHG_mp_running = -1; // this game runs the room's cockroaches (-1: no room of them up)
static u8 aHG_mp_next_id;
static int aHG_mp_pup_moving; // the one a cockroach runs from this frame is another player on the move
static u8 aHG_mp_killed[aHG_MP_KILLED];
static u32 aHG_mp_killed_ms[aHG_MP_KILLED];
static int aHG_mp_killed_at;

static int aHG_mp_room(void) {
    return mFI_IS_PLAYER_ROOM(mFI_GetFieldId()) || Save_Get(scene_no) == SCENE_COTTAGE_MY;
}

static int aHG_mp_is(ACTOR* a) {
    return a->id == mAc_PROFILE_HOUSE_GOKI && a->ct_proc == NULL && a->mv_proc != NULL;
}

// a copy not yet built counts as one all the same (its id is still in its arg)
static int aHG_mp_copy_id(ACTOR* a) {
    if (a->id != mAc_PROFILE_HOUSE_GOKI || a->mv_proc == NULL) {
        return 0;
    }
    if (a->ct_proc != NULL) {
        return a->actor_specific < aHG_MP_COPY_ARG ? aHG_MP_COPY_ARG - a->actor_specific : 0;
    }
    return ((HOUSE_GOKI_ACTOR*)a)->mp_copy ? ((HOUSE_GOKI_ACTOR*)a)->mp_id : 0;
}

static u8 aHG_mp_new_id(GAME* game) {
    GAME_PLAY* play = (GAME_PLAY*)game;
    int tries;

    for (tries = 0; tries < 255; tries++) {
        ACTOR* a;
        int used = FALSE;

        if (++aHG_mp_next_id == 0) {
            aHG_mp_next_id = 1;
        }
        for (a = play->actor_info.list[ACTOR_PART_BG].actor; a != NULL; a = a->next_actor) {
            used |= aHG_mp_is(a) && ((HOUSE_GOKI_ACTOR*)a)->mp_id == aHG_mp_next_id;
        }
        if (!used) {
            break;
        }
    }
    return aHG_mp_next_id;
}

static void aHG_mp_ct(HOUSE_GOKI_ACTOR* goki, GAME* game) {
    ACTOR* actorx = (ACTOR*)goki;

    if (actorx->actor_specific < aHG_MP_COPY_ARG) {
        goki->mp_copy = TRUE;
        goki->mp_id = (u8)(aHG_MP_COPY_ARG - actorx->actor_specific);
        goki->mp_to = actorx->world.position;
        goki->mp_angle = actorx->world.angle.y;
        goki->mp_act = aHG_ACT_WAIT;
        goki->mp_alpha = 255;
    } else {
        goki->mp_id = aHG_mp_new_id(game);
    }
}
#endif

static void aHG_actor_ct(ACTOR* actorx, GAME* game) {
    HOUSE_GOKI_ACTOR* goki = (HOUSE_GOKI_ACTOR*)actorx;
    GAME_PLAY* play = (GAME_PLAY*)game;

#ifdef VITA_MP
    aHG_mp_ct(goki, game);
#endif
    goki->alpha = 30.0f;
    if (actorx->actor_specific <= 0) {
        actorx->actor_specific = 0;
        goki->alpha = 255.0f;
    }

    actorx->scale.x = 0.01f;
    actorx->scale.y = 0.01f;
    actorx->scale.z = 0.01f;

    Shape_Info_init(actorx, 0.0f, &mAc_ActorShadowCircle, 6.0f, 6.0f);
    xyz_t_move(&actorx->home.position, &actorx->world.position);
    actorx->gravity = -2.0f;
    actorx->shape_info.rotation.y = actorx->player_angle_y + DEG2SHORT_ANGLE2(180.0f);
    actorx->world.angle.y = actorx->shape_info.rotation.y;
    aHG_setupAction(goki, game, aHG_ACT_AWAY);
#ifdef VITA_MP
    if (goki->mp_copy) {
        actorx->shape_info.rotation.y = goki->mp_angle;
        actorx->world.angle.y = goki->mp_angle;
        goki->alpha = goki->mp_alpha;
    }
#endif
}

static void aHG_anime_proc(HOUSE_GOKI_ACTOR* goki) {
    goki->anm_no += 0.5f;
    if (goki->anm_no >= 2.0f) {
        goki->anm_no -= 2.0f;
    }
}

static void aHG_calc_timer(HOUSE_GOKI_ACTOR* goki) {
    goki->timer -= 0.5f;
    if (goki->timer < 0.0f) {
        goki->timer = 0.0f;
    }

    goki->timer2 -= 0.5f;
    if (goki->timer2 < 0.0f) {
        goki->timer2 = 0.0f;
    }
}

static s16 aHG_away_bg_hitangle_check_proc(ACTOR* actorx) {
    HOUSE_GOKI_ACTOR* goki = (HOUSE_GOKI_ACTOR*)actorx;
    s16 angle = 777; // 0x309
    s16 wall_angle[2];
    int i;

    goki->timer2 = 0.0f;
    if (actorx->player_distance_xz > 20.0f) {
        wall_angle[0] = 777;
        wall_angle[1] = 777;

        if (actorx->bg_collision_check.result.hit_wall & mCoBG_HIT_WALL_FRONT) {
            wall_angle[0] = DEG2SHORT_ANGLE2(-90.0f);
            wall_angle[1] = DEG2SHORT_ANGLE2(90.0f);
        }

        if ((actorx->bg_collision_check.result.hit_wall & mCoBG_HIT_WALL_RIGHT) || (actorx->bg_collision_check.result.hit_wall & mCoBG_HIT_WALL_LEFT)) {
            if (wall_angle[0] == 777) {
                wall_angle[0] = DEG2SHORT_ANGLE2(180.0f) - actorx->world.angle.y;
                if (wall_angle[0] > 0) {
                    wall_angle[0] = DEG2SHORT_ANGLE2(360.0f) - actorx->world.angle.y;
                }
            } else if (actorx->bg_collision_check.result.hit_wall & mCoBG_HIT_WALL_RIGHT) {
                wall_angle[0] = DEG2SHORT_ANGLE2(90.0f);
            } else {
                wall_angle[0] = DEG2SHORT_ANGLE2(-90.0f);
            }

            angle = wall_angle[0];
        } else {
            i = 0;

            if (wall_angle[0] != 777) {
                // wall_angle[0] -= actorx->player_angle_y;
                angle = ABS((s16)(wall_angle[0] - actorx->player_angle_y));
            }

            if (wall_angle[1] != 777) {
                // wall_angle[1] -= actorx->player_angle_y;


                if (angle < (s16)ABS((s16)(wall_angle[1] - actorx->player_angle_y))) {
                    i = 1;
                }
            }

            angle = wall_angle[i];
        }
    }

    return angle;
}

static void aHG_decide_next_act_idx_wait_move(HOUSE_GOKI_ACTOR* goki, GAME* game) {
    GAME_PLAY* play = (GAME_PLAY*)game;
    int next_act_idx = aHG_ACT_MOVE;

    goki->timer = 5.0f + RANDOM_F(5.0f);
    if (play->game_frame % 100 > 20 || RANDOM_F(1.0f) < 0.5f) {
        next_act_idx = aHG_ACT_WAIT;
    } else if (goki->actor_class.player_distance_xz < 60.0f) {
        next_act_idx = aHG_ACT_WAIT;
    }

    aHG_setupAction(goki, game, next_act_idx);
}

static void aHG_position_move(ACTOR* actorx) {
    s16 angleY = actorx->world.angle.y;

    actorx->position_speed.x = actorx->speed * sin_s(angleY);
    actorx->position_speed.z = actorx->speed * cos_s(angleY);
    actorx->position_speed.y += actorx->gravity;
    Actor_position_move(actorx);
}

static void aHG_BGcheck(ACTOR* actorx) {
    mCoBG_WallCheckOnly(NULL, actorx, 15.0f, 0.0f, 0, 0);
    if (actorx->world.position.y < actorx->home.position.y) {
        actorx->world.position.y = actorx->home.position.y;
    }
}

static int aHG_calc_add_alpha(HOUSE_GOKI_ACTOR* goki) {
    int ret = FALSE;

    goki->alpha += 3.5f;
    if (goki->alpha > 255.0f) {
        goki->alpha = 255.0f;
        ret = TRUE;
    }

    return ret;
}

static int aHG_check_dead(ACTOR* actorx, GAME* game) {
    ACTOR* playerx = GET_PLAYER_ACTOR_GAME_ACTOR(game);
    int ret = FALSE;

    if (aMR_CLIP != NULL && aMR_CLIP->check_danna_kill_proc(&actorx->world.position)) {
        ret = TRUE;
    }

    if (actorx->bg_collision_check.result.unk_flag2 || actorx->bg_collision_check.result.unk_flag3) {
        ret = TRUE;
    }

    if (playerx != NULL && !F32_IS_ZERO(playerx->speed) && actorx->player_distance_xz < 9.0f && actorx->world.position.y == actorx->home.position.y) {
        ret = TRUE;
    }

    return ret;
}

static int aHG_player_check(ACTOR* actorx, GAME* game) {
    ACTOR* playerx = GET_PLAYER_ACTOR_GAME_ACTOR(game);
    int ret = FALSE;

#ifdef VITA_MP
    if (playerx != NULL && (!F32_IS_ZERO(playerx->speed) || aHG_mp_pup_moving) && actorx->player_distance_xz < 60.0f) {
#else
    if (playerx != NULL && !F32_IS_ZERO(playerx->speed) && actorx->player_distance_xz < 60.0f) {
#endif
        ret = TRUE;
    }

    return ret;
}

static int aHG_check_threshold(ACTOR* actorx) {
    mActor_name_t* fg_p = mFI_GetUnitFG(actorx->world.position);

    if (fg_p != NULL && *fg_p == RSV_DOOR) {
        actorx->shape_info.rotation.y = DEG2SHORT_ANGLE2(180.0f);
        actorx->world.angle.y = DEG2SHORT_ANGLE2(180.0f);
    }

    return FALSE;
}

static void aHG_away(ACTOR* actorx, GAME* game) {
    HOUSE_GOKI_ACTOR* goki = (HOUSE_GOKI_ACTOR*)actorx;
    GAME_PLAY* play = (GAME_PLAY*)game;

    if (goki->timer <= 0.0f && !goki->jump_flag && !aHG_player_check(actorx, game)) {
        goki->timer = 20.0f + RANDOM_F(20.0f);
        aHG_setupAction(goki, game, aHG_ACT_WAIT);
    } else {
        if (aHG_check_threshold(actorx) == TRUE) {
            goki->jump_flag = TRUE;
            goki->timer2 = 4.0f + RANDOM_F(4.0f);
        } else {
            goki->jump_flag = FALSE;

            if (goki->timer2 <= 0.0f) {
                if (actorx->bg_collision_check.result.hit_wall != mCoBG_DIDNT_HIT_WALL) {
                    f32 chance = 0.2f;
                    s16 angle = aHG_away_bg_hitangle_check_proc(actorx);

                    if (angle != 777) {
                        actorx->world.angle.y += angle;
                        actorx->world.angle.y &= 0xC000; // only allow movement on pure X & Z axes: 0, 90, 180, 270
                        actorx->shape_info.rotation.y = actorx->world.angle.y;
                        goki->timer2 = 5.0f + RANDOM_F(5.0f);
                    } else {
                        chance = 0.5f;
                    }

                    if (RANDOM_F(1.0f) < chance) {
                        aHG_setupAction(goki, game, aHG_ACT_JUMP_AWAY);
                    }
                } else {
                    actorx->world.angle.y = actorx->player_angle_y + DEG2SHORT_ANGLE2(180.0f);
                    actorx->shape_info.rotation.y = actorx->world.angle.y;
                }
            }
        }

        sAdo_OngenPos((u32)goki, NA_SE_GOKI_MOVE, &actorx->world.position);
    }
}

static void aHG_jump_away(ACTOR* actorx, GAME* game) {
    HOUSE_GOKI_ACTOR* goki = (HOUSE_GOKI_ACTOR*)actorx;
    GAME_PLAY* play = (GAME_PLAY*)game;

    if (actorx->world.position.y == actorx->home.position.y) {
        aHG_setupAction(goki, game, aHG_ACT_AWAY);
    } else {
        aHG_anime_proc(goki);
        if (actorx->position_speed.y < 0.0f) {
            actorx->gravity = -7.0f;
        }
        aHG_check_threshold(actorx);
    }
}

static void aHG_wait(ACTOR* actorx, GAME* game) {
    HOUSE_GOKI_ACTOR* goki = (HOUSE_GOKI_ACTOR*)actorx;
    ACTOR* playerx = GET_PLAYER_ACTOR_GAME_ACTOR(game); // @unused

    if (aHG_player_check(actorx, game) == TRUE) {
        aHG_setupAction(goki, game, aHG_ACT_AWAY);
    } else if (goki->timer <= 0.0f) {
        aHG_decide_next_act_idx_wait_move(goki, game);
    }
}

static void aHG_move(ACTOR* actorx, GAME* game) {
    HOUSE_GOKI_ACTOR* goki = (HOUSE_GOKI_ACTOR*)actorx;

    if (aHG_player_check(actorx, game) == TRUE) {
        aHG_setupAction(goki, game, aHG_ACT_AWAY);
    } else {
        if (mAc_CHK_HIT_WALL(actorx, mCoBG_HIT_WALL_FRONT) ||
            mAc_CHK_HIT_ATR_WALL(actorx, mCoBG_HIT_WALL_FRONT) ||
            mAc_CHK_HIT_WALL(actorx, mCoBG_HIT_WALL_RIGHT) ||
            mAc_CHK_HIT_ATR_WALL(actorx, mCoBG_HIT_WALL_RIGHT) ||
            mAc_CHK_HIT_WALL(actorx, mCoBG_HIT_WALL_LEFT) ||
            mAc_CHK_HIT_ATR_WALL(actorx, mCoBG_HIT_WALL_LEFT)) {
            if (!goki->jump_flag) {
                actorx->shape_info.rotation.y += DEG2SHORT_ANGLE2(180.0f);
                actorx->world.angle.y = actorx->shape_info.rotation.y;
            }

            goki->jump_flag = TRUE;
        } else {
            if (aHG_check_threshold(actorx) == TRUE) {
                goki->timer = 5.0f + RANDOM_F(5.0f);
            }

            goki->jump_flag = FALSE;
        }

        if (goki->timer <= 0.0f) {
            aHG_decide_next_act_idx_wait_move(goki, game);
        } else {
            sAdo_OngenPos((u32)goki, NA_SE_GOKI_MOVE, &actorx->world.position);
        }
    }
}

static void aHG_dead(ACTOR* actorx, GAME* game) {
    HOUSE_GOKI_ACTOR* goki = (HOUSE_GOKI_ACTOR*)actorx;

    goki->alpha = goki->shadow_alpha;
    if (((int)goki->timer & 2) == 0) {
        goki->alpha = 0.0f;
    }

    goki->shadow_alpha -= 2.5f;
    if (goki->shadow_alpha < 0.0f) {
        goki->shadow_alpha = 0.0f;
    }

    actorx->shape_info.shadow_alpha_change_rate = goki->shadow_alpha * 0.0015f;
    if (goki->timer <= 0.0f) {
        Actor_delete(actorx);
    }
}

static void aHG_away_init(HOUSE_GOKI_ACTOR* goki, GAME* game) {
    goki->timer = 20.0f + RANDOM_F(20.0f);
    goki->timer2 = 0.0f;
    goki->jump_flag = FALSE;
    goki->anm_no = 0.0f;
    goki->actor_class.speed = 8.0f;
}

static void aHG_jump_away_init(HOUSE_GOKI_ACTOR* goki, GAME* game) {
    // @BUG - incorrect usage of random generator again. Devs clearly intended this to be a random
    // binary value (0 or 1), but calling RANDOM(1) will always result in 0 when aliasing to integer.
    // Bug results in the cockroach jump movement always being 22.5 degrees more than
    // the player angle.
#ifndef BUGFIXES
    goki->actor_class.shape_info.rotation.y = goki->actor_class.player_angle_y + (s16)(DEG2SHORT_ANGLE2(22.5f) - ((s16)RANDOM(1) * DEG2SHORT_ANGLE2(45.0f)));
#else
    goki->actor_class.shape_info.rotation.y = goki->actor_class.player_angle_y + (s16)(DEG2SHORT_ANGLE2(22.5f) - ((s16)RANDOM(2) * DEG2SHORT_ANGLE2(45.0f)));
#endif
    goki->actor_class.world.angle.y = goki->actor_class.shape_info.rotation.y;
    goki->actor_class.position_speed.y = 17.0f;
    goki->actor_class.gravity = -2.0f;
    goki->actor_class.speed = 5.0f;
    sAdo_OngenTrgStart(NA_SE_GOKI_JUMP_AWAY, &goki->actor_class.world.position);
}

static void aHG_wait_init(HOUSE_GOKI_ACTOR* goki, GAME* game) {
    goki->actor_class.shape_info.rotation.y = goki->actor_class.player_angle_y + DEG2SHORT_ANGLE2(180.0f);
    goki->actor_class.world.angle.y = goki->actor_class.shape_info.rotation.y;
    goki->actor_class.speed = 0.0f;
    goki->anm_no = 0.0f;
}

static void aHG_move_init(HOUSE_GOKI_ACTOR* goki, GAME* game) {
    xyz_t pos;

    goki->timer = 5.0f + RANDOM_F(5.0f);
    goki->jump_flag = FALSE;
    xyz_t_move(&pos, &goki->actor_class.world.position);
    pos.x += 80.0f - RANDOM_F(160.0f);
    pos.z += 80.0f - RANDOM_F(160.0f);
    goki->actor_class.shape_info.rotation.y = search_position_angleY(&goki->actor_class.world.position, &pos);
    goki->actor_class.world.angle.y = goki->actor_class.shape_info.rotation.y;
    goki->actor_class.speed = 4.0f;
}

static void aHG_dead_init(HOUSE_GOKI_ACTOR* goki, GAME* game) {
    eEC_CLIP->effect_make_proc(eEC_EFFECT_GOKI, goki->actor_class.world.position, 1, 0, game, EMPTY_NO, 0, 0);
#ifdef VITA_MP
    if (!goki->mp_copy)
#endif
    mCkRh_CalcCanLookGokiCount(-1);
    sAdo_OngenTrgStart(NA_SE_GOKI_DEAD, &goki->actor_class.world.position);
    goki->shadow_alpha = 255.0f;
    goki->actor_class.speed = 0.0f;
    goki->timer = 40.0f;
}

typedef void (*aHG_INIT_PROC)(HOUSE_GOKI_ACTOR* goki, GAME* game);

static void aHG_setupAction(HOUSE_GOKI_ACTOR* goki, GAME* game, int act) {
    static aHG_INIT_PROC init_proc[] = {
        &aHG_away_init,
        &aHG_jump_away_init,
        &aHG_wait_init,
        &aHG_move_init,
        &aHG_dead_init,
    };

    static aHG_ACT_PROC act_proc[] = {
        &aHG_away,
        &aHG_jump_away,
        &aHG_wait,
        &aHG_move,
        &aHG_dead,
    };

    goki->action = act;
    goki->act_proc = act_proc[act];
    (*init_proc[act])(goki, game);
}

#ifdef VITA_MP
static void aHG_mp_note_killed(u8 id) {
    aHG_mp_killed[aHG_mp_killed_at] = id;
    aHG_mp_killed_ms[aHG_mp_killed_at] = pc_mp_now_ms();
    aHG_mp_killed_at = (aHG_mp_killed_at + 1) % aHG_MP_KILLED;
}

static int aHG_mp_was_killed(u8 id) {
    int i;

    for (i = 0; i < aHG_MP_KILLED; i++) {
        if (aHG_mp_killed[i] == id && pc_mp_now_ms() - aHG_mp_killed_ms[i] < aHG_MP_KILLED_MS) {
            return TRUE;
        }
    }
    return FALSE;
}

// the nearest player on the move, the others' included, is who a cockroach runs from
static void aHG_mp_chase(ACTOR* actorx, GAME* game) {
    ACTOR* playerx = GET_PLAYER_ACTOR_GAME_ACTOR(game);
    void* pups[4];
    int n = mp_puppets(pups, 4);
    f32 best = (playerx != NULL && !F32_IS_ZERO(playerx->speed)) ? actorx->player_distance_xz : 100000.0f;
    int i;

    for (i = 0; i < n; i++) {
        ACTOR* p = (ACTOR*)pups[i];
        f32 d = search_position_distanceXZ(&actorx->world.position, &p->world.position);

        if ((p->world.position.x != p->last_world_position.x || p->world.position.z != p->last_world_position.z) &&
            d < best) {
            best = d;
            actorx->player_distance_xz = d;
            actorx->player_angle_y = search_position_angleY(&actorx->world.position, &p->world.position);
            aHG_mp_pup_moving = TRUE;
        }
    }
}

// a copy goes where the runner's game has its cockroach, and dies at once when this player steps on it
static void aHG_mp_copy_move(HOUSE_GOKI_ACTOR* goki, GAME* game) {
    ACTOR* actorx = (ACTOR*)goki;
    ACTOR* playerx = GET_PLAYER_ACTOR_GAME_ACTOR(game);

    aHG_calc_timer(goki);
    if (goki->action == aHG_ACT_DEAD) {
        aHG_dead(actorx, game);
        return;
    }
    actorx->world.position.x += (goki->mp_to.x - actorx->world.position.x) * 0.5f;
    actorx->world.position.y += (goki->mp_to.y - actorx->world.position.y) * 0.5f;
    actorx->world.position.z += (goki->mp_to.z - actorx->world.position.z) * 0.5f;
    actorx->world.angle.y = goki->mp_angle;
    actorx->shape_info.rotation.y = goki->mp_angle;
    goki->anm_no = goki->mp_anm * 0.5f;
    goki->alpha = goki->mp_alpha;
    if (goki->mp_act == aHG_ACT_AWAY || goki->mp_act == aHG_ACT_MOVE) {
        sAdo_OngenPos((u32)goki, NA_SE_GOKI_MOVE, &actorx->world.position);
    }
    if (goki->mp_alpha == 255 && playerx != NULL && !F32_IS_ZERO(playerx->speed) && actorx->player_distance_xz < 9.0f &&
        actorx->world.position.y < actorx->home.position.y + 1.0f) {
        u8 body[2];

        body[0] = MP_VFX_GOKI_KILL;
        body[1] = goki->mp_id;
        mp_vfx_send(body, 2);
        aHG_mp_note_killed(goki->mp_id);
        aHG_setupAction(goki, game, aHG_ACT_DEAD);
    }
}

// a copy takes what the runner's game last said of its cockroach
static void aHG_mp_take(HOUSE_GOKI_ACTOR* goki, const mp_goki_t* e, GAME* game) {
    goki->mp_to.x = e->x * 0.25f;
    goki->mp_to.y = e->y * 0.25f;
    goki->mp_to.z = e->z * 0.25f;
    goki->mp_angle = e->angle;
    goki->mp_anm = e->anm;
    goki->mp_alpha = e->alpha;
    if (e->act != aHG_ACT_JUMP_AWAY) {
        goki->actor_class.home.position.y = goki->mp_to.y;
    } else if (goki->mp_act != aHG_ACT_JUMP_AWAY) {
        sAdo_OngenTrgStart(NA_SE_GOKI_JUMP_AWAY, &goki->actor_class.world.position);
    }
    goki->mp_act = e->act;
    if (e->act == aHG_ACT_DEAD) {
        goki->actor_class.world.position = goki->mp_to;
        aHG_setupAction(goki, game, aHG_ACT_DEAD);
    }
}

// copies follow the runner's cockroaches as last heard; one it no longer has goes, one dying here dies out
static void aHG_mp_follow(GAME_PLAY* play, const mp_goki_t* heard, int n) {
    u8 have[MP_GOKI_MAX] = { 0 };
    ACTOR* a;
    int i;

    for (a = play->actor_info.list[ACTOR_PART_BG].actor; a != NULL; a = a->next_actor) {
        int id = aHG_mp_copy_id(a);
        const mp_goki_t* e = NULL;

        if (id == 0) {
            continue;
        }
        for (i = 0; i < n; i++) {
            if (heard[i].id == id) {
                e = &heard[i];
                have[i] = TRUE;
            }
        }
        if (a->ct_proc != NULL || ((HOUSE_GOKI_ACTOR*)a)->action == aHG_ACT_DEAD) {
            continue;
        }
        if (e == NULL) {
            Actor_delete(a);
        } else {
            aHG_mp_take((HOUSE_GOKI_ACTOR*)a, e, (GAME*)play);
        }
    }
    for (i = 0; i < n; i++) {
        if (!have[i] && heard[i].id != 0 && heard[i].act != aHG_ACT_DEAD && !aHG_mp_was_killed(heard[i].id)) {
            Actor_info_make_actor(&play->actor_info, (GAME*)play, mAc_PROFILE_HOUSE_GOKI, heard[i].x * 0.25f,
                                  heard[i].y * 0.25f, heard[i].z * 0.25f, 0, heard[i].angle, 0, -1, -1, -1, EMPTY_NO,
                                  (s16)(aHG_MP_COPY_ARG - heard[i].id), -1, -1);
        }
    }
}

// the room's cockroaches changing hands: a game taking them over makes its copies real, one handing them over lets
// its own go (the new runner's copies come in their place); the save's count moves as on leaving or entering
static void aHG_mp_settle(GAME_PLAY* play, int run) {
    ACTOR* a;

    for (a = play->actor_info.list[ACTOR_PART_BG].actor; a != NULL; a = a->next_actor) {
        HOUSE_GOKI_ACTOR* g = (HOUSE_GOKI_ACTOR*)a;

        if (!aHG_mp_is(a) || (int)g->mp_copy != run) {
            continue;
        }
        if (g->action == aHG_ACT_DEAD) {
            if (!run) {
                Actor_delete(a);
            }
        } else if (run && !mp_is_guest() && mCkRh_NowSceneGokiFamilyCount() <= 0) {
            Actor_delete(a); // (more than the save's count holds: a visitor's own, which never counted)
        } else if (run) {
            g->mp_copy = FALSE;
            mCkRh_CalcCanLookGokiCount(1);
            mCkRh_MinusGokiN_NowRoom(1, Save_Get(scene_no));
            aHG_setupAction(g, (GAME*)play, g->mp_act == aHG_ACT_WAIT ? aHG_ACT_WAIT : aHG_ACT_AWAY);
        } else {
            mCkRh_CalcCanLookGokiCount(-1);
            mCkRh_PlussGokiN_NowRoom(1, Save_Get(scene_no));
            Actor_delete(a);
        }
    }
}

int aHG_mp_room_ct(void* game) {
    const mp_goki_t* heard;
    int n;

    aHG_mp_running = -1;
    if (!mp_active() || !aHG_mp_room()) {
        return FALSE;
    }
    heard = mp_goki_heard(&n);
    if (heard == NULL && mp_goki_runner()) {
        aHG_mp_running = TRUE;
        return FALSE;
    }
    // (another player here already runs them: copies first, which the next frame takes over if this game is to)
    aHG_mp_running = FALSE;
    aHG_mp_follow((GAME_PLAY*)game, heard, n);
    return TRUE;
}

int aHG_mp_follows(void) {
    return mp_active() && aHG_mp_running == FALSE;
}

void aHG_mp_room_end(void) {
    aHG_mp_running = -1;
}

void aHG_mp_frame(void* play_v) {
    GAME_PLAY* play = (GAME_PLAY*)play_v;
    const mp_goki_t* heard;
    int n;

    if (aHG_mp_running < 0) {
        return;
    }
    aHG_mp_running = mp_goki_runner();
    aHG_mp_settle(play, aHG_mp_running);
    if (!aHG_mp_running) {
        heard = mp_goki_heard(&n);
        aHG_mp_follow(play, heard, n);
    }
}

int aHG_mp_capture(void* play_v, mp_goki_t* out, int max) {
    GAME_PLAY* play = (GAME_PLAY*)play_v;
    int n = 0;
    int dying;

    if (aHG_mp_running != TRUE || play == NULL) {
        return -1;
    }
    // (the living first: a dying one never crowds one out)
    for (dying = 0; dying < 2; dying++) {
        ACTOR* a;

        for (a = play->actor_info.list[ACTOR_PART_BG].actor; a != NULL && n < max; a = a->next_actor) {
            HOUSE_GOKI_ACTOR* g = (HOUSE_GOKI_ACTOR*)a;

            if (!aHG_mp_is(a) || g->mp_copy || (g->action == aHG_ACT_DEAD) != dying) {
                continue;
            }
            out[n].id = g->mp_id;
            out[n].act = (u8)g->action;
            out[n].anm = (u8)(g->anm_no * 2.0f);
            out[n].alpha = (u8)g->alpha;
            out[n].x = (s16)(a->world.position.x * 4.0f);
            out[n].y = (s16)(a->world.position.y * 4.0f);
            out[n].z = (s16)(a->world.position.z * 4.0f);
            out[n].angle = a->shape_info.rotation.y;
            n++;
        }
    }
    return n;
}

void aHG_mp_replay(void* play_v, const unsigned char* body, int len) {
    GAME_PLAY* play = (GAME_PLAY*)play_v;
    ACTOR* a;

    if (len < 2 || aHG_mp_running != TRUE) {
        return;
    }
    for (a = play->actor_info.list[ACTOR_PART_BG].actor; a != NULL; a = a->next_actor) {
        HOUSE_GOKI_ACTOR* g = (HOUSE_GOKI_ACTOR*)a;

        if (aHG_mp_is(a) && !g->mp_copy && g->mp_id == body[1] && g->action != aHG_ACT_DEAD) {
            aHG_setupAction(g, (GAME*)play, aHG_ACT_DEAD);
        }
    }
}
#endif

static void aHG_actor_move(ACTOR* actorx, GAME* game) {
    HOUSE_GOKI_ACTOR* goki = (HOUSE_GOKI_ACTOR*)actorx;

#ifdef VITA_MP
    if (goki->mp_copy) {
        aHG_mp_copy_move(goki, game);
        return;
    }
#endif
    aHG_position_move(actorx);
    aHG_BGcheck(actorx);
    aHG_calc_timer(goki);

    if (goki->action != aHG_ACT_DEAD && aHG_calc_add_alpha(goki) == TRUE && aHG_check_dead(actorx, game) == TRUE) {
        aHG_setupAction(goki, game, aHG_ACT_DEAD);
    }

#ifdef VITA_MP
    aHG_mp_pup_moving = FALSE;
    if (mp_active()) {
        aHG_mp_chase(actorx, game);
    }
#endif
    (*goki->act_proc)(actorx, game);
}

extern Gfx act_m_house_goki1T_model[];
extern Gfx act_m_house_goki2T_model[];

static void aHG_actor_draw(ACTOR* actorx, GAME* game) {
    static Gfx* aHG_displayList[] = { act_m_house_goki1T_model, act_m_house_goki2T_model };
    GRAPH* graph = game->graph;
    HOUSE_GOKI_ACTOR* goki = (HOUSE_GOKI_ACTOR*)actorx;

    Matrix_push();
    Matrix_translate(actorx->world.position.x, actorx->world.position.y + 2.0f, actorx->world.position.z, MTX_LOAD);
    Matrix_scale(actorx->scale.x, actorx->scale.y, actorx->scale.z, MTX_MULT);
    Matrix_RotateX(actorx->shape_info.rotation.x, MTX_MULT);
    Matrix_RotateY(actorx->shape_info.rotation.y, MTX_MULT);
    _texture_z_light_fog_prim_xlu(graph);

    OPEN_POLY_XLU_DISP(graph);

    gSPMatrix(POLY_XLU_DISP++, _Matrix_to_Mtx_new(graph), G_MTX_LOAD | G_MTX_NOPUSH);
    gDPSetEnvColor(POLY_XLU_DISP++, 255, 255, 255, (u32)goki->alpha);
    gSPDisplayList(POLY_XLU_DISP++, aHG_displayList[(int)goki->anm_no]);

    CLOSE_POLY_XLU_DISP(graph);

    Matrix_pull();
}
