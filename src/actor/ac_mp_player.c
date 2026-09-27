// ac_mp_player.c
// another player in town, driven from the network and drawn like the inventory preview
#ifdef VITA_MP
#include "ac_mp_player.h"

#include "m_common_data.h"
#include "m_lib.h"
#include "m_player_lib.h"
#include "m_rcp.h"
#include "m_needlework.h"
#include "m_actor_shadow.h"
#include "m_field_info.h"
#include "m_play.h"
#include "ac_t_umbrella.h"
#include "ef_effect_control.h"
#include "sys_matrix.h"
#include "sys_math3d.h"
#include "jsyswrap.h"
#include "audio.h"
#include "libforest/gbi_extensions.h"
#include "pc_mp.h"
#ifdef TARGET_PC
#include "pc_bswap.h"
#endif

#include <math.h>
#include <string.h>

#define aMPP_JOINT_HAND 20
#define aMPP_JOINT_HEAD 24
#define aMPP_EXTRAP_MS  200
#define aMPP_NET_JOINT  3
#define aMPP_FX_TAG     0xFFF0 // a remote player's own effects, apart from the local player's RSV_NO ones

extern Gfx* mPlayer_mp_sponge_model(void);
extern Gfx* aGYO_mp_fish_dl(int type, int frame);
extern Gfx* aINS_mp_insect_dl(int type, int draw_type);
extern void aHOI_mp_draw(GRAPH* graph, int type, int is_fish);
extern Gfx tol_uki1_model[];
extern Gfx tol_uki2_model[];
extern cKF_Skeleton_R_c cKF_bs_r_act_bee;
extern cKF_Animation_R_c cKF_ba_r_act_bee;

static void aMPP_actor_ct(ACTOR* actorx, GAME* game);
static void aMPP_actor_dt(ACTOR* actorx, GAME* game);
static void aMPP_actor_move(ACTOR* actorx, GAME* game);
static void aMPP_actor_draw(ACTOR* actorx, GAME* game);

// clang-format off
ACTOR_PROFILE Mp_Player_Profile = {
    mAc_PROFILE_MP_PLAYER,
    ACTOR_PART_UNUSED,
    ACTOR_STATE_NO_DRAW_WHILE_CULLED | ACTOR_STATE_NO_MOVE_WHILE_CULLED,
    EMPTY_NO,
    ACTOR_OBJ_BANK_KEEP,
    sizeof(MP_PLAYER_ACTOR),
    &aMPP_actor_ct,
    &aMPP_actor_dt,
    &aMPP_actor_move,
    &aMPP_actor_draw,
    NULL,
};
// clang-format on

// same standing pipe as the player; the puppet is immovable, so only the local player is pushed
// clang-format off
static ClObjPipeData_c aMPP_pipe_data = {
    { 0x39, 0x08, ClObj_TYPE_PIPE },
    { 0x01 },
    { { 20, 60, 0, { 0, 0, 0 } } },
};
// clang-format on

extern cKF_Skeleton_R_c cKF_bs_r_boy_1;
extern cKF_Skeleton_R_c cKF_bs_r_grl_1;

extern Gfx e_umb01_model[];
extern Gfx kasa_umb01_model[];
extern Gfx e_umb02_model[];
extern Gfx kasa_umb02_model[];
extern Gfx e_umb03_model[];
extern Gfx kasa_umb03_model[];
extern Gfx e_umb04_model[];
extern Gfx kasa_umb04_model[];
extern Gfx e_umb05_model[];
extern Gfx kasa_umb05_model[];
extern Gfx e_umb06_model[];
extern Gfx kasa_umb06_model[];
extern Gfx e_umb07_model[];
extern Gfx kasa_umb07_model[];
extern Gfx e_umb08_model[];
extern Gfx kasa_umb08_model[];
extern Gfx e_umb09_model[];
extern Gfx kasa_umb09_model[];
extern Gfx e_umb10_model[];
extern Gfx kasa_umb10_model[];
extern Gfx e_umb11_model[];
extern Gfx kasa_umb11_model[];
extern Gfx e_umb12_model[];
extern Gfx kasa_umb12_model[];
extern Gfx e_umb13_model[];
extern Gfx kasa_umb13_model[];
extern Gfx e_umb14_model[];
extern Gfx kasa_umb14_model[];
extern Gfx e_umb15_model[];
extern Gfx kasa_umb15_model[];
extern Gfx e_umb16_model[];
extern Gfx kasa_umb16_model[];
extern Gfx e_umb17_model[];
extern Gfx kasa_umb17_model[];
extern Gfx e_umb18_model[];
extern Gfx kasa_umb18_model[];
extern Gfx e_umb19_model[];
extern Gfx kasa_umb19_model[];
extern Gfx e_umb20_model[];
extern Gfx kasa_umb20_model[];
extern Gfx e_umb21_model[];
extern Gfx kasa_umb21_model[];
extern Gfx e_umb22_model[];
extern Gfx kasa_umb22_model[];
extern Gfx e_umb23_model[];
extern Gfx kasa_umb23_model[];
extern Gfx e_umb24_model[];
extern Gfx kasa_umb24_model[];
extern Gfx e_umb25_model[];
extern Gfx kasa_umb25_model[];
extern Gfx e_umb26_model[];
extern Gfx kasa_umb26_model[];
extern Gfx e_umb27_model[];
extern Gfx kasa_umb27_model[];
extern Gfx e_umb28_model[];
extern Gfx kasa_umb28_model[];
extern Gfx e_umb29_model[];
extern Gfx kasa_umb29_model[];
extern Gfx e_umb30_model[];
extern Gfx kasa_umb30_model[];
extern Gfx e_umb31_model[];
extern Gfx kasa_umb31_model[];
extern Gfx e_umb32_model[];
extern Gfx kasa_umb32_model[];
extern Gfx e_umb_w_model[];
extern Gfx kasa_umb_w_model[];

static Gfx* const aMPP_umb_model[40][2] = {
    { e_umb01_model, kasa_umb01_model }, { e_umb02_model, kasa_umb02_model }, { e_umb03_model, kasa_umb03_model },
    { e_umb04_model, kasa_umb04_model }, { e_umb05_model, kasa_umb05_model }, { e_umb06_model, kasa_umb06_model },
    { e_umb07_model, kasa_umb07_model }, { e_umb08_model, kasa_umb08_model }, { e_umb09_model, kasa_umb09_model },
    { e_umb10_model, kasa_umb10_model }, { e_umb11_model, kasa_umb11_model }, { e_umb12_model, kasa_umb12_model },
    { e_umb13_model, kasa_umb13_model }, { e_umb14_model, kasa_umb14_model }, { e_umb15_model, kasa_umb15_model },
    { e_umb16_model, kasa_umb16_model }, { e_umb17_model, kasa_umb17_model }, { e_umb18_model, kasa_umb18_model },
    { e_umb19_model, kasa_umb19_model }, { e_umb20_model, kasa_umb20_model }, { e_umb21_model, kasa_umb21_model },
    { e_umb22_model, kasa_umb22_model }, { e_umb23_model, kasa_umb23_model }, { e_umb24_model, kasa_umb24_model },
    { e_umb25_model, kasa_umb25_model }, { e_umb26_model, kasa_umb26_model }, { e_umb27_model, kasa_umb27_model },
    { e_umb28_model, kasa_umb28_model }, { e_umb29_model, kasa_umb29_model }, { e_umb30_model, kasa_umb30_model },
    { e_umb31_model, kasa_umb31_model }, { e_umb32_model, kasa_umb32_model }, { e_umb_w_model, kasa_umb_w_model },
    { e_umb_w_model, kasa_umb_w_model }, { e_umb_w_model, kasa_umb_w_model }, { e_umb_w_model, kasa_umb_w_model },
    { e_umb_w_model, kasa_umb_w_model }, { e_umb_w_model, kasa_umb_w_model }, { e_umb_w_model, kasa_umb_w_model },
    { e_umb_w_model, kasa_umb_w_model },
};

static void aMPP_actor_ct(ACTOR* actorx, GAME* game) {
    MP_PLAYER_ACTOR* pup = (MP_PLAYER_ACTOR*)actorx;

    pup->slot = actorx->actor_specific;
    pup->gender = -1;
    pup->anim0 = -1;
    pup->anim1 = -1;
    pup->part_idx = -1;
    pup->item_shape = -1;
    pup->item_anim = -1;
    pup->umbrella = 0xFF;
    pup->hidden = TRUE;
    pup->item_scale = 1.0f;
    pup->umb_action = -1;
    pup->target = actorx->world.position;
    pup->target_rot = actorx->shape_info.rotation.y;
    actorx->scale.x = 0.01f;
    actorx->scale.y = 0.01f;
    actorx->scale.z = 0.01f;
    actorx->status_data.weight = MASSTYPE_IMMOVABLE;
    Shape_Info_init(actorx, 0.0f, &mAc_ActorShadowCircle, 18.0f, 18.0f);
    actorx->shape_info.ofs_y = 200.0f;
    ClObjPipe_ct(game, &pup->pipe);
    ClObjPipe_set5(game, &pup->pipe, actorx, &aMPP_pipe_data);
}

static void aMPP_actor_dt(ACTOR* actorx, GAME* game) {
    MP_PLAYER_ACTOR* pup = (MP_PLAYER_ACTOR*)actorx;

    ClObjPipe_dt(game, &pup->pipe);
    mp_player_puppet_gone(pup->slot, actorx);
}

// ARAM loads need 32-byte aligned targets and actors are only 16-byte aligned, so they land here first
static u8 aMPP_aram_buf[0xE00] ATTRIBUTE_ALIGN(32);

static void aMPP_aram_get(u32 rom, void* dst, u32 size) {
    _JW_GetResourceAram(rom, aMPP_aram_buf, size);
    memcpy(dst, aMPP_aram_buf, size);
}

// face and clothes for this player's look; stock clothes come from ROM like the player's own
static void aMPP_load_look(MP_PLAYER_ACTOR* pup, const mp_look_t* look) {
    u32 tex_rom;
    u32 pal_rom;

    mPlib_Get_FaceRom_forLook(look->gender, look->face, look->swell, look->decoy, look->sunburn, &tex_rom, &pal_rom);
    aMPP_aram_get(tex_rom, pup->face_tex, sizeof(pup->face_tex));
    aMPP_aram_get(pal_rom, pup->face_pal, sizeof(pup->face_pal));
#ifdef TARGET_PC
    pc_bswap16_array(pup->face_pal, 16);
#endif

    if (look->cloth_idx <= CLOTH_NUM) {
        mPlib_Load_PlayerTexAndPallet(aMPP_aram_buf, aMPP_aram_buf + sizeof(pup->cloth_tex), look->cloth_idx);
        memcpy(pup->cloth_tex, aMPP_aram_buf, sizeof(pup->cloth_tex));
        memcpy(pup->cloth_pal, aMPP_aram_buf + sizeof(pup->cloth_tex), sizeof(pup->cloth_pal));
    } else {
        memcpy(pup->cloth_tex, look->cloth_tex, sizeof(pup->cloth_tex));
        memcpy(pup->cloth_pal, mNW_PaletteIdx2Palette(look->cloth_pal_idx), sizeof(pup->cloth_pal));
    }

    pup->umbrella = look->umbrella;
    if (look->umbrella >= 32 && look->umbrella < 40) {
        memcpy(pup->umb_tex, look->umb_tex, sizeof(pup->umb_tex));
        memcpy(pup->umb_pal, mNW_PaletteIdx2Palette(look->umb_pal_idx), sizeof(pup->umb_pal));
    }

    if (pup->gender != look->gender) {
        pup->gender = look->gender;
        cKF_SkeletonInfo_R_ct(&pup->kf0, look->gender == mPr_SEX_MALE ? &cKF_bs_r_boy_1 : &cKF_bs_r_grl_1, NULL,
                              pup->work, pup->morph);
        cKF_SkeletonInfo_R_ct(&pup->kf1, look->gender == mPr_SEX_MALE ? &cKF_bs_r_boy_1 : &cKF_bs_r_grl_1, NULL,
                              pup->work, pup->morph);
        pup->anim0 = -1;
        pup->anim1 = -1;
    }
    pup->look_ver = look->ver;
}

// the sender's frame drifts from ours between updates; only a visible gap is corrected
static void aMPP_sync_frame(cKF_FrameControl_c* fc, f32 frame, f32 speed) {
    f32 diff = frame - fc->current_frame;

    if (fc->mode == cKF_FRAMECONTROL_REPEAT && fc->end_frame > fc->start_frame) {
        f32 len = fc->end_frame - fc->start_frame;

        if (diff > len * 0.5f) {
            diff -= len;
        } else if (diff < -len * 0.5f) {
            diff += len;
        }
    }
    if (diff > 2.0f || diff < -2.0f) {
        fc->current_frame = frame;
    }
    fc->speed = speed;
}

// an animation as the sender plays it: forward, or from its end back
static void aMPP_init_anim(cKF_SkeletonInfo_R_c* kf, cKF_Animation_R_c* anim, f32 frame, f32 speed, f32 morph,
                           int repeat) {
    int mode = repeat ? cKF_FRAMECONTROL_REPEAT : cKF_FRAMECONTROL_STOP;

    if (speed < 0.0f) {
        cKF_SkeletonInfo_R_init(kf, kf->skeleton, anim, anim->frames, 1.0f, frame, -speed, morph, mode, NULL);
    } else {
        cKF_SkeletonInfo_R_init_standard_setframeandspeedandmorphandmode(kf, anim, NULL, frame, speed, morph, mode);
    }
}

static void aMPP_apply_anim(MP_PLAYER_ACTOR* pup, const mp_pstate_t* st) {
    int rev0 = st->speed0 < 0.0f;
    int rev1 = st->speed1 < 0.0f;

    if (st->part_table != pup->part_idx) {
        pup->part_idx = st->part_table;
        mPlib_DMA_player_Part_Table(pup->part_table, st->part_table);
    }

    if (st->anim0 != pup->anim0 || st->anim1 != pup->anim1 || rev0 != pup->rev0 || rev1 != pup->rev1) {
        cKF_Animation_R_c* anim0 = mPlib_Get_Pointer_Animation(st->anim0);
        cKF_Animation_R_c* anim1 = mPlib_Get_Pointer_Animation(st->anim1);
        f32 morph = (pup->anim0 < 0) ? 0.0f : -5.0f;

        if (anim0 == NULL || anim1 == NULL || pup->kf0.skeleton == NULL) {
            return; // keep the last pose
        }
        aMPP_init_anim(&pup->kf0, anim0, st->frame0, st->speed0, morph, (st->flags & MP_PF_REPEAT0) != 0);
        aMPP_init_anim(&pup->kf1, anim1, st->frame1, st->speed1, morph, (st->flags & MP_PF_REPEAT1) != 0);
        pup->anim0 = st->anim0;
        pup->anim1 = st->anim1;
        pup->rev0 = (u8)rev0;
        pup->rev1 = (u8)rev1;
    } else {
        aMPP_sync_frame(&pup->kf0.frame_control, st->frame0, rev0 ? -st->speed0 : st->speed0);
        aMPP_sync_frame(&pup->kf1.frame_control, st->frame1, rev1 ? -st->speed1 : st->speed1);
    }

    // tools with their own skeleton (net, rod, pinwheel) animate alongside
    if (st->item_shape != pup->item_shape || st->item_anim != pup->item_anim) {
        pup->item_shape = st->item_shape;
        pup->item_anim = st->item_anim;
        memset(&pup->item_kf, 0, sizeof(pup->item_kf));
        if (mPlayer_ITEM_DATA_VALID(st->item_shape) && mPlayer_ITEM_DATA_VALID(st->item_anim) &&
            mPlib_Get_Item_DataPointerType(st->item_shape) == mPlayer_ITEM_DATA_TYPE_SKELETON &&
            mPlib_Get_Item_DataPointerType(st->item_anim) > mPlayer_ITEM_DATA_TYPE_SKELETON) {
            cKF_SkeletonInfo_R_ct(&pup->item_kf, (cKF_Skeleton_R_c*)mPlib_Get_Item_DataPointer(st->item_shape), NULL,
                                  pup->item_work, pup->item_morph);
            cKF_SkeletonInfo_R_init_standard_setframeandspeedandmorphandmode(
                &pup->item_kf, (cKF_Animation_R_c*)mPlib_Get_Item_DataPointer(st->item_anim), NULL, st->item_frame,
                st->item_speed, 0.0f, (st->flags & MP_PF_ITEM_REPEAT) ? cKF_FRAMECONTROL_REPEAT : cKF_FRAMECONTROL_STOP);
        }
    } else if (pup->item_kf.skeleton != NULL) {
        aMPP_sync_frame(&pup->item_kf.frame_control, st->item_frame, st->item_speed);
    }
}

static u16 aMPP_fx_tag(MP_PLAYER_ACTOR* pup, u16 item) {
    return item == RSV_NO ? (u16)(aMPP_FX_TAG | pup->slot) : item;
}

// effects that look at "the player" while they start see this one
static void aMPP_make_effect(MP_PLAYER_ACTOR* pup, GAME* game, const mp_fx_t* fx, xyz_t pos) {
    PLAYER_ACTOR* player = GET_PLAYER_ACTOR((GAME_PLAY*)game);
    xyz_t keep_pos;
    s16 keep_angle;
    s16 keep_rot;

    if (player == NULL) {
        return;
    }
    keep_pos = player->actor_class.world.position;
    keep_angle = player->actor_class.world.angle.y;
    keep_rot = player->actor_class.shape_info.rotation.y;
    player->actor_class.world.position = pup->actor_class.world.position;
    player->actor_class.world.angle.y = pup->actor_class.shape_info.rotation.y;
    player->actor_class.shape_info.rotation.y = pup->actor_class.shape_info.rotation.y;
    eEC_CLIP->effect_make_proc(fx->id, pos, fx->prio, fx->angle, game, aMPP_fx_tag(pup, fx->item), fx->arg0, fx->arg1);
    player->actor_class.world.position = keep_pos;
    player->actor_class.world.angle.y = keep_angle;
    player->actor_class.shape_info.rotation.y = keep_rot;
}

static void aMPP_play_fx(MP_PLAYER_ACTOR* pup, GAME* game, const mp_fx_t* fx) {
    xyz_t pos;

    pos.x = pup->actor_class.world.position.x + fx->dx;
    pos.y = pup->actor_class.world.position.y + fx->dy;
    pos.z = pup->actor_class.world.position.z + fx->dz;
    switch (fx->kind) {
        case MP_SND_WALK:
            sAdo_mp_walk_as((u8)((fx->id >> 10) & 3), fx->id & ~0x0C00, &pos, FALSE);
            break;
        case MP_SND_WALK_ROOM:
            sAdo_mp_walk_as((u8)(fx->id >> 14), fx->id & 0xFF, &pos, TRUE);
            break;
        case MP_FX_EFFECT:
            if (eEC_CLIP != NULL && fx->id < eEC_EFFECT_NUM) {
                aMPP_make_effect(pup, game, fx, pos);
            }
            break;
        case MP_FX_KILL:
            if (eEC_CLIP != NULL && fx->id < eEC_EFFECT_NUM) {
                eEC_CLIP->effect_kill_proc(fx->id, aMPP_fx_tag(pup, fx->item));
            }
            break;
        default:
            sAdo_OngenTrgStart(fx->id, &pos);
            break;
    }
}

// what the sender's draw adds on top of the pose
static void aMPP_take_extra(MP_PLAYER_ACTOR* pup, const mp_pstate_t* st) {
    pup->flags = st->flags;
    pup->target_rot_x = st->rot_x;
    pup->target_rot_z = st->rot_z;
    pup->roll = st->roll;
    pup->item_scale = st->item_scale;
    pup->root_flags = st->root_flags;
    pup->root_trans.x = st->root_trans[0];
    pup->root_trans.y = st->root_trans[1];
    pup->root_trans.z = st->root_trans[2];
    pup->root_rot.x = st->root_rot_x;
    pup->root_rot.y = st->root_rot_y;
    pup->root_rot.z = st->root_rot_z;
    pup->net_angle.x = st->net_angle[0];
    pup->net_angle.y = st->net_angle[1];
    pup->net_angle.z = st->net_angle[2];
    pup->rod_angle_z = (st->flags & MP_PF_ROD) ? st->rod_angle_z : 0;
    if (st->flags & MP_PF_UMB) {
        f32 diff = st->umb_frame - pup->umb_frame;

        // our own stepping keeps it smooth; only a new action or a real gap resyncs
        pup->umb_idx = st->umb_idx;
        if (st->umb_action != pup->umb_action || diff > 2.0f || diff < -2.0f) {
            pup->umb_action = st->umb_action;
            pup->umb_frame = st->umb_frame;
        }
    } else {
        pup->umb_action = -1;
    }
    pup->hold_item = st->hold_item;
    pup->hold_scale = st->hold_scale;
    pup->hold_d.x = st->hold_dx;
    pup->hold_d.y = st->hold_dy;
    pup->hold_d.z = st->hold_dz;
    pup->hold_angle = st->hold_angle;
    pup->hold_jump = st->hold_jump;
    pup->natt = (st->flags & MP_PF_ATTACH) ? st->natt : 0;
    memcpy(pup->att, st->att, sizeof(pup->att));
    pup->balloon_x = st->balloon_x;
    pup->balloon_z = st->balloon_z;
    pup->balloon_frame = st->item_frame;
}

// every frame: the pinned root, the umbrella's shape, the star's colors, the pinwheel's whir
static void aMPP_step_extra(MP_PLAYER_ACTOR* pup) {
    ACTOR* actorx = &pup->actor_class;

    if (pup->flags & MP_PF_ROOT) {
        pup->kf0.animation_enabled = pup->root_flags;
        pup->kf0.base_model_translation = pup->root_trans;
        pup->kf0.base_model_rotation.x = pup->root_rot.x;
        pup->kf0.updated_base_model_rotation.y = pup->root_rot.y;
        pup->kf0.updated_base_model_rotation.z = pup->root_rot.z;
    } else {
        pup->kf0.animation_enabled = 0;
    }
    actorx->shape_info.rotation.x += (s16)((s16)(pup->target_rot_x - actorx->shape_info.rotation.x) * 0.5f);
    actorx->shape_info.rotation.z += (s16)((s16)(pup->target_rot_z - actorx->shape_info.rotation.z) * 0.5f);
    if (pup->umb_action >= 0) {
        aTUMB_mp_scales(pup->umb_action, &pup->umb_frame, &pup->umb_e, &pup->umb_kasa);
    }
    if (pup->flags & MP_PF_STAR) {
        pup->star_timer += 1.0f;
        if (pup->star_timer >= 79.68f) {
            pup->star_timer = 0.0f;
        }
    } else {
        pup->star_timer = 0.0f;
    }
    {
        int i;

        for (i = 0; i < pup->natt && i < MP_ATT_MAX; i++) {
            if (pup->att[i].kind == MP_ATT_BEE && !pup->hidden) {
                xyz_t pos;

                pos.x = actorx->world.position.x + pup->att[i].dx;
                pos.y = actorx->world.position.y + pup->att[i].dy;
                pos.z = actorx->world.position.z + pup->att[i].dz;
                sAdo_OngenPos((u32)&pup->bee_kf, NA_SE_B0, &pos);
            }
        }
    }
    if (!pup->hidden && pup->item_main == mPlayer_ITEM_MAIN_WINDMILL_NORMAL && pup->item_kf.skeleton != NULL) {
        f32 speed = pup->item_kf.frame_control.speed / 44.0f;

        speed = ABS(speed);
        if (speed > 1.0f) {
            speed = 1.0f;
        }
        if (speed != 0.0f) {
            // (the whir's pitch is one for all: this player's own pinwheel sets it, else the nearest other's)
            static int kaza_frame = -1;
            static f32 kaza_d;
            PLAYER_ACTOR* me = GET_PLAYER_ACTOR((GAME_PLAY*)gamePT);

            if (me != NULL && me->now_item_main_index != mPlayer_ITEM_MAIN_WINDMILL_NORMAL) {
                f32 dx = actorx->world.position.x - me->actor_class.world.position.x;
                f32 dz = actorx->world.position.z - me->actor_class.world.position.z;

                if (kaza_frame != ((GAME_PLAY*)gamePT)->game_frame || dx * dx + dz * dz < kaza_d) {
                    kaza_frame = ((GAME_PLAY*)gamePT)->game_frame;
                    kaza_d = dx * dx + dz * dz;
                    sAdo_kazagurumaLevel(speed);
                }
            }
            sAdo_OngenPos((u32)pup, NA_SE_TEMOCHI_KAZAGURUMA, &actorx->world.position);
        }
    }
}

// the game walking this screen's player somewhere itself (off the train, onto a boat, to the raffle): nothing of the
// other players' stands in the way, or the walk never reaches its spot
static int aMPP_local_scripted(GAME* game) {
    int idx = mPlib_get_player_actor_main_index(game);

    return (idx >= mPlayer_INDEX_DEMO_WAIT && idx <= mPlayer_INDEX_DEMO_WADE) ||
           (idx >= mPlayer_INDEX_DEMO_GETON_BOAT && idx <= mPlayer_INDEX_DEMO_GET_GOLDEN_AXE_WAIT);
}

static void aMPP_actor_move(ACTOR* actorx, GAME* game) {
    MP_PLAYER_ACTOR* pup = (MP_PLAYER_ACTOR*)actorx;
    GAME_PLAY* play = (GAME_PLAY*)game;
    const mp_look_t* look = mp_player_look(pup->slot);
    unsigned int age_ms = 0;
    const mp_pstate_t* st = mp_player_state(pup->slot, &age_ms);
    xyz_t goal;
    f32 ahead;

    if (look == NULL || st == NULL) {
        pup->hidden = TRUE;
        return;
    }
    if (look->ver != pup->look_ver) {
        aMPP_load_look(pup, look);
    }

    if (st->seq != pup->seq) {
        xyz_t pos;

        pos.x = st->x;
        pos.y = st->y;
        pos.z = st->z;
        if (pup->target_ms != 0 && age_ms < 1000) {
            unsigned int now = pc_mp_now_ms();
            f32 dt = (f32)(now - pup->target_ms) * 0.001f;

            if (dt > 0.01f) {
                pup->velocity.x = (pos.x - pup->target.x) / dt;
                pup->velocity.y = (pos.y - pup->target.y) / dt;
                pup->velocity.z = (pos.z - pup->target.z) / dt;
            }
        }
        pup->target = pos;
        pup->target_ms = pc_mp_now_ms() - age_ms;
        pup->target_rot = st->rot_y;
        pup->head_x = st->head_x;
        pup->head_y = st->head_y;
        pup->eye = st->eye;
        pup->mouth = st->mouth;
        pup->item_main = st->item_main;
        pup->seq = st->seq;
        {
            int was_root = (pup->flags & MP_PF_ROOT) != 0;

            aMPP_apply_anim(pup, st);
            aMPP_take_extra(pup, st);
            // (a pinned root let go: the body is already where the new spot is)
            if (was_root && !(pup->flags & MP_PF_ROOT)) {
                actorx->world.position = pos;
                actorx->shape_info.rotation.y = pup->target_rot;
                pup->velocity.x = pup->velocity.y = pup->velocity.z = 0.0f;
            }
        }
        // big jumps (doors, warps) snap instead of gliding across the town
        if (search_position_distance(&actorx->world.position, &pos) > 200.0f) {
            actorx->world.position = pos;
            pup->velocity.x = pup->velocity.y = pup->velocity.z = 0.0f;
        }
    }
    pup->hidden = (st->flags & MP_PF_HIDDEN) != 0 || pup->anim0 < 0 || pup->gender < 0;

    // glide toward where the player is now, extrapolating a little while updates are late
    ahead = (f32)((age_ms < aMPP_EXTRAP_MS) ? age_ms : aMPP_EXTRAP_MS) * 0.001f;
    goal.x = pup->target.x + pup->velocity.x * ahead;
    goal.y = pup->target.y + pup->velocity.y * ahead;
    goal.z = pup->target.z + pup->velocity.z * ahead;
    if (age_ms > aMPP_EXTRAP_MS) {
        pup->velocity.x = pup->velocity.y = pup->velocity.z = 0.0f;
    }
    {
        f32 step_x = (goal.x - actorx->world.position.x) * 0.35f;
        f32 step_z = (goal.z - actorx->world.position.z) * 0.35f;

        actorx->world.position.x += step_x;
        actorx->world.position.z += step_z;
        actorx->speed = sqrtf(step_x * step_x + step_z * step_z);
    }
    actorx->world.position.y += (goal.y - actorx->world.position.y) * 0.35f;
    actorx->shape_info.rotation.y += (s16)((s16)(pup->target_rot - actorx->shape_info.rotation.y) * 0.35f);
    actorx->world.angle.y = actorx->shape_info.rotation.y;

    if (pup->anim0 >= 0) {
        cKF_SkeletonInfo_R_combine_play(&pup->kf0, &pup->kf1, pup->part_table);
    }
    if (pup->item_kf.skeleton != NULL && pup->item_shape >= 0) {
        if (pup->item_main == mPlayer_ITEM_MAIN_BALLOON_NORMAL) {
            // the balloon's swing sets its pose on the sender; ease toward it
            cKF_FrameControl_c* fc = &pup->item_kf.frame_control;

            fc->current_frame += (pup->balloon_frame - fc->current_frame) * 0.5f;
            fc->speed = 0.0f;
        }
        cKF_SkeletonInfo_R_play(&pup->item_kf);
    }
    aMPP_step_extra(pup);
    {
        mp_fx_t fx;

        while (mp_player_take_fx(pup->slot, &fx)) {
            if (!pup->hidden) {
                aMPP_play_fx(pup, game, &fx);
            }
        }
    }

    // (sitting, lying down, fading in or out: the shadow as the player's own is)
    actorx->shape_info.draw_shadow = !pup->hidden && (st->shadow & 0x80) != 0;
    actorx->shape_info.shadow_size_change_rate = (f32)(st->shadow & 0x7F) / 127.0f;
    actorx->shape_info.shadow_alpha_change_rate = actorx->shape_info.shadow_size_change_rate;
    // (a hooked fish thrashing, a massage chair's hum: kept up here while the player's word is fresh)
    if (!pup->hidden && age_ms < 250) {
        int k;

        for (k = 0; k < st->nlvl && k < MP_LVL_MAX; k++) {
            xyz_t at;

            at.x = actorx->world.position.x + st->lvl_d[k][0];
            at.y = actorx->world.position.y + st->lvl_d[k][1];
            at.z = actorx->world.position.z + st->lvl_d[k][2];
            sAdo_OngenPos((u32)actorx + 1 + k, st->lvl[k], &at);
        }
    }
    // (one gone quiet stands where it was last heard of, in nobody's way)
    if (!pup->hidden && age_ms < 3000 && !aMPP_local_scripted(game)) {
        CollisionCheck_Uty_ActorWorldPosSetPipeC(actorx, &pup->pipe);
        CollisionCheck_setOC(game, &play->collision_check, &pup->pipe.collision_obj);
    }
}

static int aMPP_draw_before(GAME* game, cKF_SkeletonInfo_R_c* kf, int joint_no, Gfx** gfx_pp, u8* work_flag, void* arg,
                            s_xyz* rot, xyz_t* pos) {
    MP_PLAYER_ACTOR* pup = (MP_PLAYER_ACTOR*)arg;

    if (joint_no == aMPP_JOINT_HEAD && (pup->head_x != 0 || pup->head_y != 0)) {
        Matrix_push();
        Matrix_mult(&MtxF_clear, MTX_LOAD);
        Matrix_softcv3_mult(&ZeroVec, rot);
        Matrix_RotateX(pup->head_x, MTX_MULT);
        Matrix_RotateY(pup->head_y, MTX_MULT);
        Matrix_to_rotate2_new(get_Matrix_now(), rot, MTX_LOAD);
        Matrix_pull();
    }
    return TRUE;
}

static int aMPP_draw_after(GAME* game, cKF_SkeletonInfo_R_c* kf, int joint_no, Gfx** gfx_pp, u8* work_flag, void* arg,
                           s_xyz* rot, xyz_t* pos) {
    MP_PLAYER_ACTOR* pup = (MP_PLAYER_ACTOR*)arg;

    if (joint_no == aMPP_JOINT_HAND) {
        Matrix_get(&pup->hand_mtx);
        pup->hand_valid = TRUE;
    }
    return TRUE;
}

static void aMPP_draw_umbrella(MP_PLAYER_ACTOR* pup, GRAPH* graph) {
    // (the one in hand, which may be going away as the next is picked)
    int idx = (pup->umb_action >= 0 && pup->umb_idx < 40) ? pup->umb_idx : (pup->umbrella < 40 ? pup->umbrella : 32);

    _texture_z_light_fog_prim_npc(graph);
    OPEN_POLY_OPA_DISP(graph);

    Matrix_rotateXYZ(0, -0x4000, 0, MTX_MULT);
    if (pup->umb_action >= 0) {
        Matrix_scale(pup->umb_e.x, pup->umb_e.y, pup->umb_e.z, MTX_MULT);
    }
    gSPMatrix(POLY_OPA_DISP++, _Matrix_to_Mtx_new(graph), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPDisplayList(POLY_OPA_DISP++, aMPP_umb_model[idx][0]);
    Matrix_translate(4500.0f, 0.0f, 0.0f, MTX_MULT);
    if (pup->umb_action >= 0) {
        Matrix_scale(pup->umb_kasa.x, pup->umb_kasa.y, pup->umb_kasa.z, MTX_MULT);
    }
    gSPMatrix(POLY_OPA_DISP++, _Matrix_to_Mtx_new(graph), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    if (idx >= 32) {
        gSPSegment(POLY_OPA_DISP++, G_MWO_SEGMENT_8, pup->umb_pal);
        gSPSegment(POLY_OPA_DISP++, G_MWO_SEGMENT_9, pup->umb_tex);
    }
    gSPDisplayList(POLY_OPA_DISP++, aMPP_umb_model[idx][1]);

    CLOSE_POLY_OPA_DISP(graph);
}

static int aMPP_balloon_edge(GAME* game, int joint_no, int alpha) {
    if (joint_no >= 1 && joint_no <= 3) {
        OPEN_POLY_OPA_DISP(game->graph);
        gDPPipeSync(POLY_OPA_DISP++);
        gDPSetTexEdgeAlpha(POLY_OPA_DISP++, alpha);
        CLOSE_POLY_OPA_DISP(game->graph);
    }
    return TRUE;
}

static int aMPP_balloon_before(GAME* game, cKF_SkeletonInfo_R_c* kf, int joint_no, Gfx** gfx_pp, u8* work_flag,
                               void* arg, s_xyz* rot, xyz_t* pos) {
    return aMPP_balloon_edge(game, joint_no, 80);
}

static int aMPP_balloon_after(GAME* game, cKF_SkeletonInfo_R_c* kf, int joint_no, Gfx** gfx_pp, u8* work_flag,
                              void* arg, s_xyz* rot, xyz_t* pos) {
    return aMPP_balloon_edge(game, joint_no, 144);
}

// a balloon held up, swaying as the sender's (Player_actor_Item_draw_balloon)
static void aMPP_draw_balloon(MP_PLAYER_ACTOR* pup, GAME* game) {
    GRAPH* graph = game->graph;
    f32 scale = pup->actor_class.scale.x * pup->item_scale;
    xyz_t hand;
    Mtx* item_mtx;

    if (!(pup->flags & MP_PF_BALLOON) || pup->item_kf.skeleton == NULL) {
        return;
    }
    item_mtx = (Mtx*)GRAPH_ALLOC_TYPE(graph, Mtx, pup->item_kf.skeleton->num_shown_joints);
    if (item_mtx == NULL) {
        return;
    }
    hand.x = pup->hand_mtx.xw;
    hand.y = pup->hand_mtx.yw;
    hand.z = pup->hand_mtx.zw;
    Matrix_push();
    Matrix_translate(hand.x, hand.y, hand.z, MTX_LOAD);
    Matrix_RotateY(pup->actor_class.shape_info.rotation.y, MTX_MULT);
    Matrix_RotateX((s16)(DEG2SHORT_ANGLE2(-90.0f) + pup->balloon_x), MTX_MULT);
    Matrix_RotateZ(0x4000, MTX_MULT);
    Matrix_RotateX(pup->balloon_z, MTX_MULT);
    Matrix_scale(scale, scale, scale, MTX_MULT);
    _texture_z_light_fog_prim(graph);
    OPEN_POLY_OPA_DISP(graph);
    gSPMatrix(POLY_OPA_DISP++, _Matrix_to_Mtx_new(graph), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    CLOSE_POLY_OPA_DISP(graph);
    Setpos_HiliteReflect_init(&hand, (GAME_PLAY*)game);
    cKF_Si3_draw_R_SV(game, &pup->item_kf, item_mtx, &aMPP_balloon_before, &aMPP_balloon_after, pup);
    Matrix_pull();
}

// the bees chasing this player, as aBEE_actor_draw draws them from the sender's pose
static void aMPP_draw_bee(MP_PLAYER_ACTOR* pup, GAME* game, const mp_att_t* a) {
    GRAPH* graph = game->graph;
    Mtx* mtx;
    Gfx* gfx;
    xyz_t pos;

    if (!pup->bee_ready) {
        cKF_SkeletonInfo_R_ct(&pup->bee_kf, &cKF_bs_r_act_bee, &cKF_ba_r_act_bee, pup->bee_work, pup->bee_morph);
        cKF_SkeletonInfo_R_init_standard_repeat(&pup->bee_kf, &cKF_ba_r_act_bee, NULL);
        pup->bee_fly[1] = DEG2SHORT_ANGLE2(180.0f);
        pup->bee_ready = TRUE;
    }
    pup->bee_kf.frame_control.current_frame = (f32)a->arg;
    pup->bee_kf.frame_control.speed = 0.0f;
    cKF_SkeletonInfo_R_play(&pup->bee_kf);
    pup->bee_fly[0] += 500;
    pup->bee_fly[1] -= 500;
    gfx = two_tex_scroll_dolphin(graph, 0, 180.0f * sin_s(pup->bee_fly[0]), 180.0f * cos_s(pup->bee_fly[0]), 32, 32, 1,
                                 180.0f * cos_s(pup->bee_fly[1]), 180.0f * sin_s(pup->bee_fly[1]), 32, 32);
    mtx = (Mtx*)GRAPH_ALLOC_TYPE(graph, Mtx, pup->bee_kf.skeleton->num_shown_joints);
    if (gfx == NULL || mtx == NULL) {
        return;
    }
    pos.x = pup->actor_class.world.position.x + a->dx;
    pos.y = pup->actor_class.world.position.y + a->dy;
    pos.z = pup->actor_class.world.position.z + a->dz;
    _texture_z_light_fog_prim_xlu(graph);
    Setpos_HiliteReflect_xlu_init(&pos, (GAME_PLAY*)game);
    OPEN_POLY_XLU_DISP(graph);
    gDPSetPrimColor(POLY_XLU_DISP++, 0, 255, 0, 0, 0, a->rgba[3]);
    gSPSegment(POLY_XLU_DISP++, ANIME_1_TXT_SEG, gfx);
    CLOSE_POLY_XLU_DISP(graph);
    cKF_Si3_draw_R_SV(game, &pup->bee_kf, mtx, NULL, NULL, pup);
}

// what the sender's screen drew for the player (float, hooked fish, caught bug, handed item), from its matrix
static void aMPP_draw_attach(MP_PLAYER_ACTOR* pup, GAME* game, const mp_att_t* a) {
    GRAPH* graph = game->graph;
    s_xyz rot;
    Gfx* dl;
    int layer;

    rot.x = a->rot[0];
    rot.y = a->rot[1];
    rot.z = a->rot[2];
    Matrix_push();
    Matrix_softcv3_load(&rot, pup->actor_class.world.position.x + a->dx, pup->actor_class.world.position.y + a->dy,
                        pup->actor_class.world.position.z + a->dz);
    Matrix_scale(a->scale[0], a->scale[1], a->scale[2], MTX_MULT);
    switch (a->kind) {
        case MP_ATT_UKI:
            _texture_z_light_fog_prim(graph);
            OPEN_POLY_OPA_DISP(graph);
            gDPPipeSync(POLY_OPA_DISP++);
            gSPMatrix(POLY_OPA_DISP++, _Matrix_to_Mtx_new(graph), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
            gDPSetPrimColor(POLY_OPA_DISP++, 0, 128, a->rgba[0], a->rgba[1], a->rgba[2], 255);
            gSPDisplayList(POLY_OPA_DISP++, a->arg ? tol_uki2_model : tol_uki1_model);
            CLOSE_POLY_OPA_DISP(graph);
            break;
        case MP_ATT_FISH:
            dl = aGYO_mp_fish_dl(a->arg, a->arg2);
            if (dl != NULL) {
                _texture_z_light_fog_prim(graph);
                OPEN_POLY_OPA_DISP(graph);
                gSPMatrix(POLY_OPA_DISP++, _Matrix_to_Mtx_new(graph), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
                gSPDisplayList(POLY_OPA_DISP++, dl);
                CLOSE_POLY_OPA_DISP(graph);
            }
            break;
        case MP_ATT_INSECT:
            _texture_z_light_fog_prim_xlu(graph);
            for (layer = 0; layer < 2; layer++) {
                dl = aINS_mp_insect_dl(a->arg, a->arg2 + layer);
                if (dl == NULL) {
                    break;
                }
                OPEN_DISP(graph);
                gDPPipeSync(NEXT_POLY_XLU_DISP);
                gSPMatrix(NEXT_POLY_XLU_DISP, _Matrix_to_Mtx_new(graph), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
                gDPSetEnvColor(NEXT_POLY_XLU_DISP, 255, 255, 255, a->rgba[3]);
                gSPDisplayList(NEXT_POLY_XLU_DISP, dl);
                CLOSE_DISP(graph);
            }
            break;
        case MP_ATT_HOI:
            aHOI_mp_draw(graph, a->arg, a->arg2);
            break;
        case MP_ATT_BEE:
            aMPP_draw_bee(pup, game, a);
            break;
    }
    Matrix_pull();
}

// the net's head swings as the sender's does (Player_actor_Item_draw_net_After_dummy_net)
static int aMPP_draw_net_after(GAME* game, cKF_SkeletonInfo_R_c* kf, int joint_no, Gfx** gfx_pp, u8* work_flag,
                               void* arg, s_xyz* rot, xyz_t* pos) {
    MP_PLAYER_ACTOR* pup = (MP_PLAYER_ACTOR*)arg;

    if (joint_no == aMPP_NET_JOINT) {
        Matrix_rotateXYZ(pup->net_angle.x, pup->net_angle.y, pup->net_angle.z, MTX_MULT);
    }
    return TRUE;
}

static void aMPP_draw_sponge(MP_PLAYER_ACTOR* pup, GRAPH* graph) {
    Mtx* mtx;

    _texture_z_light_fog_prim(graph);
    Matrix_push();
    Matrix_put(&pup->hand_mtx);
    mtx = _Matrix_to_Mtx_new(graph);
    if (mtx != NULL) {
        OPEN_POLY_OPA_DISP(graph);
        gSPMatrix(POLY_OPA_DISP++, mtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        gSPDisplayList(POLY_OPA_DISP++, mPlayer_mp_sponge_model());
        CLOSE_POLY_OPA_DISP(graph);
    }
    Matrix_pull();
}

// the tool in the right hand, drawn the way Player_actor_Item_draw does
static void aMPP_draw_item(MP_PLAYER_ACTOR* pup, GAME* game) {
    GRAPH* graph = game->graph;
    int main = pup->item_main;
    Mtx* mtx;

    if (pup->hand_valid && main == mPlayer_ITEM_MAIN_NONE && (pup->flags & MP_PF_WASH)) {
        aMPP_draw_sponge(pup, graph);
        return;
    }
    if (pup->hand_valid && main == mPlayer_ITEM_MAIN_BALLOON_NORMAL) {
        aMPP_draw_balloon(pup, game);
        return;
    }
    if (!pup->hand_valid || main == mPlayer_ITEM_MAIN_NONE || main >= mPlayer_ITEM_MAIN_NUM) {
        return;
    }

    Matrix_push();
    Matrix_put(&pup->hand_mtx);
    if (pup->item_scale != 1.0f) {
        Matrix_scale(pup->item_scale, pup->item_scale, pup->item_scale, MTX_MULT);
    }
    if (main == mPlayer_ITEM_MAIN_UMBRELLA_NORMAL) {
        aMPP_draw_umbrella(pup, graph);
        Matrix_pull();
        return;
    }
    if (main >= mPlayer_ITEM_MAIN_ROD_NORMAL && main <= mPlayer_ITEM_MAIN_ROD_PUTAWAY && pup->rod_angle_z != 0) {
        Matrix_RotateZ(pup->rod_angle_z, MTX_MULT);
    } else if (main == mPlayer_ITEM_MAIN_WINDMILL_NORMAL) {
        Matrix_RotateY((s16)(-0.5f * pup->actor_class.shape_info.rotation.x), MTX_MULT);
    }

    mtx = _Matrix_to_Mtx_new(graph);
    if (mtx != NULL && mPlayer_ITEM_DATA_VALID(pup->item_shape)) {
        _texture_z_light_fog_prim(graph);
        OPEN_POLY_OPA_DISP(graph);
        gSPMatrix(POLY_OPA_DISP++, mtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        CLOSE_POLY_OPA_DISP(graph);

        if (mPlib_Get_Item_DataPointerType(pup->item_shape) == mPlayer_ITEM_DATA_TYPE_SKELETON) {
            if (pup->item_kf.skeleton != NULL) {
                Mtx* item_mtx = (Mtx*)GRAPH_ALLOC_TYPE(graph, Mtx, pup->item_kf.skeleton->num_shown_joints);

                if (item_mtx != NULL) {
                    cKF_Si3_draw_R_SV(game, &pup->item_kf, item_mtx, NULL,
                                      (pup->flags & MP_PF_NET) ? &aMPP_draw_net_after : NULL, pup);
                }
            }
        } else {
            OPEN_POLY_OPA_DISP(graph);
            gSPDisplayList(POLY_OPA_DISP++, mPlib_Get_Item_DataPointer(pup->item_shape));
            CLOSE_POLY_OPA_DISP(graph);
        }
    }
    Matrix_pull();
}

// Super Star colors, as Player_actor_draw_Normal sets them through the fog
static void aMPP_star_fog(MP_PLAYER_ACTOR* pup, GAME_PLAY* play, int on) {
    // the player's table read row by row: r, g, b for each of the four colors
    static const int color_data[3][4] = {
        { 255, 255, 100, 100 },
        { 100, 255, 100, 255 },
        { 255, 255, 100, 255 },
    };
    GRAPH* graph = play->game.graph;
    int color_frame = (int)(pup->star_timer / 9.96f);
    int idx = color_frame / 2;
    int near = 1;
    int far = 1;
    int r = 0;
    int g = 0;
    int b = 0;

    if (!on) {
        OPEN_DISP(graph);
        SET_POLY_OPA_DISP(gfx_set_fog_nosync(NOW_POLY_OPA_DISP, play->global_light.fogColor[0],
                                             play->global_light.fogColor[1], play->global_light.fogColor[2], 0,
                                             play->global_light.fogNear, play->global_light.fogFar));
        CLOSE_DISP(graph);
        return;
    }
    if (idx >= 0 && idx < 4) {
        r = color_data[0][idx];
        g = color_data[1][idx];
        b = color_data[2][idx];
    }
    if (!(color_frame & 1) && idx >= 0 && idx < 4) {
        View* v = &play->view;
        xyz_t diff;
        f32 diff_len;

        diff.x = v->center.x - v->eye.x;
        diff.y = v->center.y - v->eye.y;
        diff.z = v->center.z - v->eye.z;
        diff_len = Math3DVecLength(&diff);
        if (diff_len > 0.0f) {
            f32 dx = pup->actor_class.world.position.x - v->eye.x;
            f32 dy = pup->actor_class.world.position.y - v->eye.y;
            f32 dz = pup->actor_class.world.position.z - v->eye.z;
            f32 percent = (dx * diff.x + dy * diff.y + dz * diff.z) / diff_len;
            f32 t0 = diff_len - 352.0f;
            f32 t1 = diff_len * 0.25f;
            f32 t2 = 2.0f / 14.1f;

            near = (int)(210.0f + (diff_len - percent) / diff_len);
            far = near + (int)(780.0f + t0 * (t2 / 2.0f) + t1 * (t2 / 2.0f));
        }
    }
    OPEN_DISP(graph);
    SET_POLY_OPA_DISP(gfx_set_fog_nosync(NOW_POLY_OPA_DISP, r, g, b, 255, near, far));
    CLOSE_DISP(graph);
}

// something growing into or out of the hand: a pickup, a dug-up find, a shovel's catch
static void aMPP_draw_hold(MP_PLAYER_ACTOR* pup, GAME* game) {
    xyz_t pos;

    if (!(pup->flags & MP_PF_HOLD) || pup->hold_item == EMPTY_NO || pup->hold_scale <= 0.0f) {
        return;
    }
    pos.x = pup->actor_class.world.position.x + pup->hold_d.x;
    pos.y = pup->actor_class.world.position.y + pup->hold_d.y;
    pos.z = pup->actor_class.world.position.z + pup->hold_d.z;
    // (furniture indoors goes into the pocket as its leaf, as the room shows the player's own)
    if (ITEM_IS_FTR(pup->hold_item) && mFI_GET_TYPE(mFI_GetFieldId()) != mFI_FIELD_FG) {
        aMR_mp_draw_leaf(game, pup->hold_item, &pos, pup->hold_scale);
        return;
    }
    if (mFI_GET_TYPE(mFI_GetFieldId()) == mFI_FIELD_FG) {
        if (Common_Get(clip).bg_item_clip != NULL && Common_Get(clip).bg_item_clip->single_draw_proc != NULL) {
            Common_Get(clip).bg_item_clip->single_draw_proc(game, pup->hold_item, &pos, pup->hold_scale, NULL, NULL,
                                                            NULL);
        }
    } else if (Common_Get(clip).shop_goods_clip != NULL && Common_Get(clip).shop_goods_clip->single_draw_proc != NULL) {
        Common_Get(clip).shop_goods_clip->single_draw_proc(game, pup->hold_item, &pos, pup->hold_scale, pup->hold_angle,
                                                           pup->hold_jump);
    }
}

static void aMPP_actor_draw(ACTOR* actorx, GAME* game) {
    MP_PLAYER_ACTOR* pup = (MP_PLAYER_ACTOR*)actorx;
    GRAPH* graph = game->graph;
    Mtx* mtx;

    if (pup->hidden || pup->kf0.skeleton == NULL) {
        return;
    }
    mtx = (Mtx*)GRAPH_ALLOC_TYPE(graph, Mtx, pup->kf0.skeleton->num_shown_joints);
    if (mtx == NULL) {
        return;
    }

    _texture_z_light_fog_prim(graph);
    OPEN_POLY_OPA_DISP(graph);
    gSPSegment(POLY_OPA_DISP++, ANIME_1_TXT_SEG, pup->face_tex + (pup->eye % mPlayer_EYE_TEX_NUM) * 0x100);
    gSPSegment(POLY_OPA_DISP++, ANIME_2_TXT_SEG,
               pup->face_tex + (mPlayer_EYE_TEX_NUM + pup->mouth % mPlayer_MOUTH_TEX_NUM) * 0x100);
    gSPSegment(POLY_OPA_DISP++, ANIME_3_TXT_SEG, pup->cloth_tex);
    gSPSegment(POLY_OPA_DISP++, ANIME_4_TXT_SEG, pup->cloth_pal);
    gSPSegment(POLY_OPA_DISP++, ANIME_5_TXT_SEG, pup->face_pal);
    CLOSE_POLY_OPA_DISP(graph);

    if (pup->roll != 0) {
        Matrix_push();
        Matrix_RotateZ(pup->roll, MTX_MULT);
    }
    if (pup->flags & MP_PF_STAR) {
        aMPP_star_fog(pup, (GAME_PLAY*)game, TRUE);
    }
    pup->hand_valid = FALSE;
    cKF_Si3_draw_R_SV(game, &pup->kf0, mtx, &aMPP_draw_before, &aMPP_draw_after, pup);
    if (pup->flags & MP_PF_STAR) {
        aMPP_star_fog(pup, (GAME_PLAY*)game, FALSE);
    }
    aMPP_draw_item(pup, game);
    aMPP_draw_hold(pup, game);
    if (pup->roll != 0) {
        Matrix_pull();
    }
    {
        int i;

        for (i = 0; i < pup->natt && i < MP_ATT_MAX; i++) {
            aMPP_draw_attach(pup, game, &pup->att[i]);
        }
    }
}

#endif
