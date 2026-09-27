#include "ac_npc_totakeke.h"

#include "m_event.h"
#include "m_common_data.h"
#include "m_npc.h"
#include "m_msg.h"
#include "m_choice.h"
#include "m_bgm.h"
#include "m_player_lib.h"
#include "m_string_data.h"
#include "m_ledit_ovl.h"
#ifdef VITA_MP
#include "m_kankyo.h"
#include "pc_mp.h"

enum {
    aNTT_MP_SHOW,     // the lights go down
    aNTT_MP_SONG_ON,  // arg: the song
    aNTT_MP_WEATHER,  // arg: weather, arg2: intensity
    aNTT_MP_SONG_OFF, // the lights come back up
};

// another player's show seen from the crowd
static struct {
    u8 quiet;  // the field music hushed for it
    u8 song;   // the song playing for it, 0 for none
    u8 lights; // 1 going down, 2 coming back up
    u8 w_set;  // the weather it called for, as last told
    u8 w_type;
    u8 w_int;
} aNTT_mp_show;

static void aNTT_mp_weather_tell(int type, int intensity) {
    u8 body[4];

    body[0] = MP_VFX_KK;
    body[1] = aNTT_MP_WEATHER;
    body[2] = (u8)type;
    body[3] = (u8)intensity;
    mp_vfx_send(body, sizeof(body));
}

#define aNTT_CHANGE_WEATHER(w, t, i) (aNTT_mp_weather_tell((t), (i)), (w)->change_weather((w)->actor, (t), (i)))
#else
#define aNTT_CHANGE_WEATHER(w, t, i) (w)->change_weather((w)->actor, (t), (i))
#endif

#ifdef TARGET_PC
static void aNTT_schedule_proc();
static int aNTT_change_talk_proc(NPC_TOTAKEKE_ACTOR*, int);
static void aNTT_setup_think_proc(NPC_TOTAKEKE_ACTOR*, GAME_PLAY*, u8);
static int aNTT_enso_init(NPC_TOTAKEKE_ACTOR*);
#else
void aNTT_schedule_proc();
int aNTT_change_talk_proc(NPC_TOTAKEKE_ACTOR*, int);
void aNTT_setup_think_proc(NPC_TOTAKEKE_ACTOR*, GAME_PLAY*, u8);
int aNTT_enso_init(NPC_TOTAKEKE_ACTOR*);
#endif
#ifdef TARGET_PC
static void aNTT_actor_ct(ACTOR* actorx, GAME* game);
static void aNTT_actor_dt(ACTOR* actorx, GAME* game);
static void aNTT_actor_init(ACTOR* actorx, GAME* game);
static void aNTT_actor_save(ACTOR* actorx, GAME* game);
static void aNTT_actor_move(ACTOR* actorx, GAME* game);
static void aNTT_actor_draw(ACTOR* actorx, GAME* game);
static int aNTT_talk_init(ACTOR* actorx, GAME* game);
static int aNTT_talk_end_chk(ACTOR* actorx, GAME* game);
#else
void aNTT_actor_ct(ACTOR* actorx, GAME* game);
void aNTT_actor_dt(ACTOR* actorx, GAME* game);
void aNTT_actor_init(ACTOR* actorx, GAME* game);
void aNTT_actor_save(ACTOR* actorx, GAME* game);
void aNTT_actor_move(ACTOR* actorx, GAME* game);
void aNTT_actor_draw(ACTOR* actorx, GAME* game);
int aNTT_talk_init(ACTOR* actorx, GAME* game);
int aNTT_talk_end_chk(ACTOR* actorx, GAME* game);
#endif

// clang-format off
ACTOR_PROFILE Npc_Totakeke_Profile = {
    mAc_PROFILE_NPC_TOTAKEKE,
    ACTOR_PART_NPC,
    ACTOR_STATE_NONE,
    SP_NPC_TOTAKEKE,
    ACTOR_OBJ_BANK_KEEP,
    sizeof(NPC_TOTAKEKE_ACTOR),
    &aNTT_actor_ct,
    &aNTT_actor_dt,
    &aNTT_actor_init,
    (mActor_proc)none_proc1,
    &aNTT_actor_save,
};

static void aNTT_actor_ct(ACTOR *actorx, GAME *game) {
    static aNPC_ct_data_c ct_data = {
        &aNTT_actor_move,
        &aNTT_actor_draw,
        aNPC_CT_SCHED_TYPE_SPECIAL,
        (mActor_proc)none_proc1,
        &aNTT_talk_init,
        &aNTT_talk_end_chk,
        0x0
    };
    aNTT_event_save_c *save = (aNTT_event_save_c *)mEv_get_save_area(mEv_EVENT_KK_SLIDER, 0xa);
    aNTT_event_common_c *common = (aNTT_event_common_c *)mEv_get_common_area(mEv_EVENT_KK_SLIDER, 0x10);

    if (Common_Get(reset_flag) == TRUE) {
        Actor_delete(actorx);
        actorx->sv_proc = NULL;
        actorx->dt_proc = NULL;
        mNpc_RenewalSetNpc(actorx);
        mEv_actor_dying_message(mEv_EVENT_KK_SLIDER, actorx);
    } else if (CLIP(npc_clip)->birth_check_proc(actorx, game) == TRUE) {
        xyz_t wpos;
        NPC_TOTAKEKE_ACTOR *totakeke = (NPC_TOTAKEKE_ACTOR *)actorx;

        totakeke->npc_class.schedule.schedule_proc = aNTT_schedule_proc;
        CLIP(npc_clip)->ct_proc(actorx, game, &ct_data);
        totakeke->npc_class.palActorIgnoreTimer = -1;
        totakeke->npc_class.condition_info.hide_flg = FALSE;
        totakeke->npc_class.collision.check_kind = aNPC_BG_CHECK_TYPE_NONE;
        actorx->world.position.y = mCoBG_GetBgY_OnlyCenter_FromWpos2(actorx->world.position, 0.0f);
        actorx->position_speed.y = 0.0f;
        actorx->gravity = 0.0f;
        actorx->max_velocity_y = 0.0f;
        totakeke->npc_class.talk_info.default_animation = aNPC_ANIM_WAIT_E1;
        totakeke->npc_class.talk_info.default_act = aNPC_ACT_TALK2;
        totakeke->npc_class.talk_info.turn = aNPC_TALK_TURN_NONE;

        aNTT_enso_init(totakeke);
        totakeke->_9a1 = FALSE;

        if (save == NULL) {
            save = (aNTT_event_save_c *)mEv_reserve_save_area(mEv_EVENT_KK_SLIDER, 0xa);
            save->bitfield = 0;
        }

        save->bitfield &= ~(aNTT_FLAG_SP_ROLL_END | aNTT_FLAG_SP_ROLL_DRAW);

        {
            int i;
            for (i = 0; i < aNTT_REQUEST_STR_LEN; i++) {
                save->request_str[i] = 0;
            }
        }

        save->roll_flag = FALSE;
        save->copyright_alpha = 0;

        if (common == NULL) {
            common = (aNTT_event_common_c *)mEv_reserve_common_area(mEv_EVENT_KK_SLIDER, 0x10);
            common->foreigner_bitfield = 0;
        }
        totakeke->npc_class.collision.pipe.attribute.pipe.radius = 30;
        totakeke->melody_inst = 0;
        totakeke->majin_flag = 0;
        totakeke->_99f = 0xff;
        mCoBG_SetPlussOffset(actorx->world.position, 3, mCoBG_ATTRIBUTE_NONE);
    }
}

static void aNTT_actor_save(ACTOR *actorx, GAME *game) {
    mNpc_RenewalSetNpc(actorx);
}

#ifdef VITA_MP
// the crowd's side of a show ends: music, weather and (at once, or by fading) the lights back
static void aNTT_mp_show_end(GAME_PLAY* play, int at_once) {
    aWeather_Clip_c* weather = CLIP(weather_clip);

    if (aNTT_mp_show.song != 0) {
        mBGMPsComp_delete_ps_demo(aNTT_mp_show.song, 0x168);
        aNTT_mp_show.song = 0;
    }
    if (aNTT_mp_show.quiet) {
        mBGMPsComp_delete_ps_quiet();
        aNTT_mp_show.quiet = FALSE;
    }
    if (aNTT_mp_show.lights != 0) {
        if (weather != NULL) {
            weather->change_weather_instance(weather->actor, mEnv_SAVE_GET_WEATHER_TYPE(Save_Get(weather)),
                                             mEnv_SAVE_GET_WEATHER_INTENSITY(Save_Get(weather)));
        }
        if (at_once) {
            staffroll_light_dt(play);
            aNTT_mp_show.lights = 0;
        } else {
            aNTT_mp_show.lights = 2;
        }
    }
}

void aNTT_mp_replay(struct game_play_s* play_s, const u8* body, int len) {
    GAME_PLAY* play = (GAME_PLAY*)play_s;
    ACTOR* kk = Actor_info_fgName_search(&play->actor_info, SP_NPC_TOTAKEKE, ACTOR_PART_NPC);
    aWeather_Clip_c* weather = CLIP(weather_clip);

    // only where the show is followed from another game, not one this game is putting on
    if (len < 4 || kk == NULL || !mp_npc_is_puppet(kk)) {
        return;
    }
    switch (body[1]) {
        case aNTT_MP_SHOW:
            // (told again for players who come by late: a show already on here stays as it is)
            if (aNTT_mp_show.lights == 1) {
                break;
            }
            if (aNTT_mp_show.song == 0 && !aNTT_mp_show.quiet) {
                mBGMPsComp_make_ps_quiet(0x168);
                aNTT_mp_show.quiet = TRUE;
            }
            if (aNTT_mp_show.lights == 0) {
                staffroll_light_init(play);
            }
            aNTT_mp_show.lights = 1;
            aNTT_mp_show.w_set = FALSE;
            if (weather != NULL) {
                weather->change_weather(weather->actor, mEnv_WEATHER_CLEAR, mEnv_WEATHER_INTENSITY_NONE);
            }
            break;
        case aNTT_MP_SONG_ON:
            if (aNTT_mp_show.song == 0) {
                aNTT_mp_show.song = (u8)(BGM_TOTAKEKE_LIVE0 + body[2]);
                mBGMPsComp_make_ps_demo(aNTT_mp_show.song, 0x168);
            }
            if (aNTT_mp_show.quiet) {
                mBGMPsComp_delete_ps_quiet();
                aNTT_mp_show.quiet = FALSE;
            }
            break;
        case aNTT_MP_WEATHER:
            if (weather != NULL && aNTT_mp_show.lights == 1 &&
                (!aNTT_mp_show.w_set || aNTT_mp_show.w_type != body[2] || aNTT_mp_show.w_int != body[3])) {
                aNTT_mp_show.w_set = TRUE;
                aNTT_mp_show.w_type = body[2];
                aNTT_mp_show.w_int = body[3];
                weather->change_weather(weather->actor, body[2], body[3]);
            }
            break;
        case aNTT_MP_SONG_OFF:
            aNTT_mp_show_end(play, FALSE);
            break;
    }
}
#endif

static void aNTT_actor_dt(ACTOR *actorx, GAME *game) {
#ifdef VITA_MP
    // walked away from another player's show: it ends here
    aNTT_mp_show_end((GAME_PLAY*)game, TRUE);
#endif
    mEv_actor_dying_message(mEv_EVENT_KK_SLIDER, actorx);
    CLIP(npc_clip)->dt_proc(actorx, game);
    mCoBG_SetPlussOffset(actorx->world.position, 0x0, 0x64);
}

static void aNTT_actor_init(ACTOR *actorx, GAME *game) {
    CLIP(npc_clip)->init_proc(actorx, game);
}

#ifdef VITA_MP
static void aNTT_wait(NPC_TOTAKEKE_ACTOR *totakeke, GAME_PLAY *play);
#endif

static void aNTT_actor_move(ACTOR *actorx, GAME *game) {
    NPC_TOTAKEKE_ACTOR *totakeke = (NPC_TOTAKEKE_ACTOR *)actorx;

#ifdef VITA_MP
    // this game has K.K. again (or the session is over): another player's show can't still be on here, nor for the
    // rest of the crowd (the player it was for left)
    if (!mp_npc_is_puppet(actorx) && (aNTT_mp_show.song != 0 || aNTT_mp_show.quiet || aNTT_mp_show.lights == 1)) {
        u8 body[4] = { MP_VFX_KK, aNTT_MP_SONG_OFF, 0, 0 };

        aNTT_mp_show_end((GAME_PLAY*)game, TRUE);
        mp_vfx_send(body, sizeof(body));
    }
#endif
    CLIP(npc_clip)->move_proc(actorx, game);
    if (totakeke->npc_class.draw.animation_id == aNPC_ANIM_ENSOU_E1) {
        totakeke->npc_class.draw.main_animation.keyframe.frame_control.mode = 1;
    }
#ifdef VITA_MP
    // another game has him: what he says to this player still depends on where this player stands
    if (mp_npc_is_puppet(actorx) && totakeke->think_proc_id <= 1) {
        aNTT_wait(totakeke, (GAME_PLAY*)game);
    }
    // the stage lights of another player's show
    if (aNTT_mp_show.lights == 1) {
        staffroll_light_proc_start((GAME_PLAY*)game);
    } else if (aNTT_mp_show.lights == 2 && staffroll_light_proc_end((GAME_PLAY*)game)) {
        staffroll_light_dt((GAME_PLAY*)game);
        aNTT_mp_show.lights = 0;
    }
#endif
}

static void aNTT_actor_draw(ACTOR *actorx, GAME *game) {
    NPC_TOTAKEKE_ACTOR *totakeke = (NPC_TOTAKEKE_ACTOR *)actorx;
    aNTT_event_save_c *save = (aNTT_event_save_c *)mEv_get_save_area(mEv_EVENT_KK_SLIDER, 0xa);
    GAME_PLAY* play = (GAME_PLAY*)game;
    
    CLIP(npc_clip)->draw_proc(actorx, game);
    if (save == NULL || (save->bitfield & 0x4000) == 0 || CLIP(mikanbox_clip) == NULL) {
        return;
    }
#ifdef VITA_MP
    // the credits roll belongs to the one who asked for the song
    if (mp_npc_is_puppet(actorx)) {
        return;
    }
#endif
    CLIP(mikanbox_clip)->roll_draw_proc(play, totakeke->roll2_count, totakeke->_9a2);
}

#include "../src/actor/npc/ac_npc_totakeke_talk.c_inc"
#include "../src/actor/npc/ac_npc_totakeke_think.c_inc"
