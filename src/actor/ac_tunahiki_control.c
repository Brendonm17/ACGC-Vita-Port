#include "ac_tunahiki_control.h"

#include "m_actor.h"
#include "m_event.h"
#include "m_name_table.h"
#include "m_play.h"
#ifdef VITA_MP
#include "pc_mp.h"
#endif

enum {
    aTNC_ACT_WAIT,

    aTNC_ACT_NUM
};

static void aTNC_actor_ct(ACTOR* actorx, GAME* game);
static void aTNC_actor_dt(ACTOR* actorx, GAME* game);
static void aTNC_actor_move(ACTOR* actorx, GAME* game);


// clang-format off
ACTOR_PROFILE Tunahiki_Control_Profile = {
    mAc_PROFILE_TUNAHIKI_CONTROL,
    ACTOR_PART_CONTROL,
    ACTOR_STATE_NO_MOVE_WHILE_CULLED,
    EMPTY_NO,
    ACTOR_OBJ_BANK_KEEP,
    sizeof(TUNAHIKI_CONTROL_ACTOR),
    aTNC_actor_ct,
    aTNC_actor_dt,
    aTNC_actor_move,
    mActor_NONE_PROC1,
    NULL,
};
// clang-format on

static void aTNC_setupAction(TUNAHIKI_CONTROL_ACTOR* actor, int action);

static void aTNC_actor_ct(ACTOR* actorx, GAME* game) {
    TUNAHIKI_CONTROL_ACTOR* actor = (TUNAHIKI_CONTROL_ACTOR*)actorx;
    aEv_tunahiki_c* tunahiki = (aEv_tunahiki_c*)mEv_get_save_area(mEv_EVENT_SPORTS_FAIR_TUG_OF_WAR, 9);

    aTNC_setupAction(actor, aTNC_ACT_WAIT);
    if (tunahiki == NULL) {
        tunahiki = (aEv_tunahiki_c*)mEv_reserve_save_area(mEv_EVENT_SPORTS_FAIR_TUG_OF_WAR, 9);
        if (tunahiki != NULL) {
            tunahiki->rope = 0.0f;
            tunahiki->rope_base = 0.0f;
            tunahiki->flag = 0;
            tunahiki->npc_state = aTNC_NPC_STATE0;
            tunahiki->_14 = 0;
        }
    }
}

static void aTNC_actor_dt(ACTOR* actorx, GAME* game) {
    mEv_actor_dying_message(mEv_EVENT_SPORTS_FAIR_TUG_OF_WAR, actorx);
}

static void aTNC_actor_move(ACTOR* actorx, GAME* game) {
    TUNAHIKI_CONTROL_ACTOR* actor = (TUNAHIKI_CONTROL_ACTOR*)actorx;
    GAME_PLAY* play = (GAME_PLAY*)game;

#ifdef VITA_MP
    // another screen runs the tug of war and keeps its record; the rope here strains as it's pulled there
    if (pc_mp_ev_follows(mEv_EVENT_SPORTS_FAIR_TUG_OF_WAR)) {
        aEv_tunahiki_c* tunahiki = (aEv_tunahiki_c*)mEv_get_save_area(mEv_EVENT_SPORTS_FAIR_TUG_OF_WAR, 9);

        if (tunahiki != NULL) {
            tunahiki->rope = tunahiki->rope_base;
            if (tunahiki->flag & aTNC_FLAG_SHAKE) {
                tunahiki->rope += (play->game_frame & 1) ? 0.3f : -0.3f;
            }
        }
        return;
    }
    // a puller another player is talking to holds the rope still here too, as this player's own talks do
    {
        static u16 mp_talk; // the talk bits this screen set for the others
        aEv_tunahiki_c* tunahiki = (aEv_tunahiki_c*)mEv_get_save_area(mEv_EVENT_SPORTS_FAIR_TUG_OF_WAR, 9);
        u16 now = (u16)((pc_mp_ev_talk_bits(mEv_EVENT_SPORTS_FAIR_TUG_OF_WAR) & 0x1E) * aTNC_FLAG_NPC_TALK0);

        if (tunahiki != NULL) {
            tunahiki->flag = (u16)((tunahiki->flag & ~(mp_talk & ~now)) | now);
            mp_talk = now;
        }
    }
#endif
    actor->act_proc(actor, play);
}

static void aTNC_wait(TUNAHIKI_CONTROL_ACTOR* actor, GAME_PLAY* play) {
    aEv_tunahiki_c* tunahiki = (aEv_tunahiki_c*)mEv_get_save_area(mEv_EVENT_SPORTS_FAIR_TUG_OF_WAR, 9);
    f32 shake = 0.0f;

    if (tunahiki->flag & aTNC_FLAG_SHAKE) {
        if (play->game_frame & 1) {
            shake = 0.3f;
        } else {
            shake = -0.3f;
        }

        if ((tunahiki->flag & aTNC_FLAG_NPC_TALK_ALL) == 0) {
            tunahiki->rope_base += tunahiki->speed * 0.025f;
            if (tunahiki->rope_base < -10.0f) {
                tunahiki->rope_base = -10.0f;
                tunahiki->speed = 0.0f;
            } else if (tunahiki->rope_base > 10.0f) {
                tunahiki->rope_base = 10.0f;
                tunahiki->speed = 0.0f;
            }
        }
    }

    {
        f32 rope = tunahiki->rope_base + shake;
        aEv_tunahiki_c* _tunahiki = (aEv_tunahiki_c*)mEv_get_save_area(mEv_EVENT_SPORTS_FAIR_TUG_OF_WAR, 9);

        _tunahiki->rope = rope;
    }

    tunahiki->counter++;
    if (tunahiki->counter >= 65) {
        tunahiki->counter = 2;
    }
}

static void aTNC_setupAction(TUNAHIKI_CONTROL_ACTOR* actor, int action) {
    static aTNC_PROC process[] = { aTNC_wait };

    actor->action = action;
    actor->act_proc = process[action];
}
