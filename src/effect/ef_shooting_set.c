#include "ef_effect_control.h"

#include "m_common_data.h"
#include "m_player_lib.h"
#ifdef VITA_MP
#include "pc_mp.h"
#endif

static void eShootingSet_init(xyz_t pos, int prio, s16 angle, GAME* game, u16 item_name, s16 arg0, s16 arg1);
static void eShootingSet_ct(eEC_Effect_c* effect, GAME* game, void* ct_arg);
static void eShootingSet_mv(eEC_Effect_c* effect, GAME* game);
static void eShootingSet_dw(eEC_Effect_c* effect, GAME* game);

eEC_PROFILE_c iam_ef_shooting_set = {
    // clang-format off
    &eShootingSet_init,
    &eShootingSet_ct,
    &eShootingSet_mv,
    &eShootingSet_dw,
    eEC_IMMEDIATE_DEATH,
    eEC_NO_CHILD_ID,
    eEC_IGNORE_DEATH_DIST,
    // clang-format on
};

static void eShootingSet_SearchNicePos(xyz_t* out_pos, GAME* game) {
    int bx, bz;
    xyz_t zero_vec = { 0.0f, 0.0f, 0.0f };
    xyz_t wpos = { 0.0f, 0.0f, 0.0f };
    *out_pos = zero_vec;

    if (mFI_BlockKind2BkNum(&bx, &bz, mRF_BLOCKKIND_POOL) && mFI_BkNum2WposXZ(&wpos.x, &wpos.z, bx, bz)) {
        out_pos->x = wpos.x + 320.0f;
        out_pos->y = mFI_BkNum2BaseHeight(bx, bz) + 20.0f;
        out_pos->z = wpos.z + 320.0f;
    }
}

static void eShootingSet_init(xyz_t pos, int prio, s16 angle, GAME* game, u16 item_name, s16 arg0, s16 arg1) {
    xyz_t out_pos;
    eShootingSet_SearchNicePos(&out_pos, game);

    eEC_CLIP->make_effect_proc(eEC_EFFECT_SHOOTING_SET, out_pos, NULL, game, NULL, item_name, prio, arg0, arg1);
}

static f32 eShooting_AdjustValue(int now, int start, int end, f32 start_val, f32 end_val) {
    if (start == end) {
        return start_val;
    }

    if (now <= start) {
        return start_val;
    }

    if (now >= end) {
        return end_val;
    }

    return start_val + (now - start) * ((end_val - start_val) / (end - start));
}

static s16 eShootingSet_Adjust(void);

static s16 eShootingSet_GetFrame_MakeNextShooting() {
    return eShootingSet_Adjust() + (RANDOM_F(120.0f) - 60.0f);
}

// the frames between stars at this hour of the night, on average
static s16 eShootingSet_Adjust(void) {
    s16 adjust;

    if ((Common_Get(time).now_sec < 64800) || (Common_Get(time).now_sec >= 75600)) {
        adjust = 600;
    } else if (Common_Get(time).now_sec < 70200) {
        adjust = eShooting_AdjustValue(Common_Get(time).now_sec, 64800, 70200, 600.0f, 120.0f);
    } else {
        adjust = eShooting_AdjustValue(Common_Get(time).now_sec, 70200, 75600, 120.0f, 600.0f);
    }

    return adjust;
}

#ifdef VITA_MP
// a shared town's stars fall by its clock (each second a chance, as often as the hour has them), each from the side
// the town's hash picks: everyone out under the sky sees one at the same moment
static int eShootingSet_mp_mv(eEC_Effect_c* effect, GAME* game) {
    unsigned int ms;
    unsigned int slot;

    if (!mp_shared_clock_ms(&ms)) {
        return FALSE;
    }
    slot = ms / 1000u;
    if ((s16)slot == effect->effect_specific[2]) {
        return TRUE;
    }
    effect->effect_specific[2] = (s16)slot;
    if ((mp_shared_hash(slot, 0x5354u, 0) & 0xFFFF) * (u32)eShootingSet_Adjust() >= 65536u * 60u ||
        !eEC_CLIP->check_lookat_block_proc(effect->position)) {
        return TRUE;
    }
    {
        u32 h1 = mp_shared_hash(slot, 0x5354u, 1);
        u32 h2 = mp_shared_hash(slot, 0x5354u, 2);
        PLAYER_ACTOR* actor = GET_PLAYER_ACTOR_GAME(game);
        s16 turn = (s16)((h1 >> 1) % 6144u);
        int angle = (h1 & 1) ? DEG2SHORT_ANGLE(315.0f) + turn : DEG2SHORT_ANGLE(315.0f) - turn;
        f32 temp = (f32)(h2 & 0xFFFF) * (80.0f / 65536.0f);
        s16 rnd_angle = (s16)(h2 >> 16);
        xyz_t pos = effect->position;

        pos.x += actor->actor_class.world.position.x;
        pos.z += actor->actor_class.world.position.z;
        pos.x *= 0.5f;
        pos.z *= 0.5f;
        pos.x += temp * cos_s(rnd_angle);
        pos.z += temp * cos_s(rnd_angle);
        eEC_CLIP->effect_make_proc(eEC_EFFECT_SHOOTING, pos, effect->prio, angle, game, (u16)effect->item_name, 0, 0);
    }
    return TRUE;
}
#endif

static void eShootingSet_ct(eEC_Effect_c* effect, GAME* game, void* ct_arg) {
    effect->timer = 100;

    effect->effect_specific[0] = 0;
    effect->effect_specific[1] = eShootingSet_GetFrame_MakeNextShooting();
}

static void eShootingSet_mv(eEC_Effect_c* effect, GAME* game) {
    f32 temp;
    s16 rng, rnd_angle;
    int angle;
    PLAYER_ACTOR* actor;
    xyz_t pos, pos2;

    eEC_CLIP->set_continious_env_proc(effect, 100, 100);

    if (mEv_CheckTitleDemo() != mEv_TITLEDEMO_STAFFROLL) {
#ifdef VITA_MP
        int shared = eShootingSet_mp_mv(effect, game);
#else
        int shared = FALSE;
#endif

        if (!shared && effect->effect_specific[0] >= effect->effect_specific[1]) {
            effect->effect_specific[0] = 0;
            effect->effect_specific[1] = eShootingSet_GetFrame_MakeNextShooting();

            if (eEC_CLIP->check_lookat_block_proc(effect->position)) {
                rng = RANDOM(10.0f) & 1;
                if (rng == 1) {
                    angle = DEG2SHORT_ANGLE(315.0f) + (s16)RANDOM_F(6144.0f);
                } else {
                    angle = DEG2SHORT_ANGLE(315.0f) - (s16)RANDOM_F(6144.0f);
                }

                pos = effect->position;

                actor = GET_PLAYER_ACTOR_GAME(game);

                temp = RANDOM_F(80.0f);
                rnd_angle = RANDOM_F(65535.0f);

                pos.x += actor->actor_class.world.position.x;
                pos.z += actor->actor_class.world.position.z;
                pos.x *= 0.5f;
                pos.z *= 0.5f;
                pos.x += temp * cos_s(rnd_angle);
                pos.z += temp * cos_s(rnd_angle);

                eEC_CLIP->effect_make_proc(eEC_EFFECT_SHOOTING, pos, effect->prio, angle, game, (u16)effect->item_name,
                                           0, 0);
            }
        }

        if (!shared) {
            effect->effect_specific[0]++;
        }

        if (eEC_CLIP->check_lookat_block_proc(effect->position)) {
            pos2 = effect->position;
            pos2.y += 200.0f;
            NPC_CLIP->set_attention_request_proc(aNPC_ATTENTION_TYPE_POSITION, NULL, &pos2);
        }
    }
}

static void eShootingSet_dw(eEC_Effect_c* effect, GAME* game) {
    return;
}
