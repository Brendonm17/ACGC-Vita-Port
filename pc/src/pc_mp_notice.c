// pc_mp_notice.c - queue of station PA announcements (arrivals, departures, the last
// train). Each one plays at the next quiet moment through the notice actor.
#include "pc_mp.h"

#ifdef VITA_MP

#include "m_common_data.h"
#include "m_actor.h"
#include "m_font.h"
#include "m_play.h"

#include <string.h>

#define MP_NOTICE_MAX 4

typedef struct {
    int msg_no;
    unsigned int serial;
    int withdrawn; // cancelled after the actor took it on
    unsigned char town[MP_NAME_LEN];
    unsigned char name[MP_NAME_LEN];
} mp_notice_t;

static mp_notice_t s_queue[MP_NOTICE_MAX];
static int s_count;
static ACTOR* s_actor; // speaks s_queue[0]; that entry stays put while it exists
static int s_on_air;   // its window is open
static unsigned int s_serial;
static unsigned int s_answer_serial;
static int s_answer = -1;

static void mp_notice_name(unsigned char* dst, const unsigned char* src) {
    if (src != NULL) {
        memcpy(dst, src, MP_NAME_LEN);
    } else {
        memset(dst, CHAR_SPACE, MP_NAME_LEN);
    }
}

static void mp_notice_remove(int i) {
    memmove(&s_queue[i], &s_queue[i + 1], sizeof(s_queue[0]) * (s_count - 1 - i));
    s_count--;
}

unsigned int mp_notice_push(int msg_no, const unsigned char* town, const unsigned char* name) {
    mp_notice_t* n;

    if (s_count > 0) {
        mp_notice_t* last = &s_queue[s_count - 1];

        // the same call twice in a row is said once
        if (last->msg_no == msg_no && !last->withdrawn && (name == NULL || memcmp(last->name, name, MP_NAME_LEN) == 0)) {
            return last->serial;
        }
    }
    if (s_count == MP_NOTICE_MAX) {
        // the oldest waiting one makes room, never the one being spoken
        mp_notice_remove(s_actor != NULL ? 1 : 0);
    }
    n = &s_queue[s_count++];
    n->msg_no = msg_no;
    if (++s_serial == 0) {
        s_serial = 1;
    }
    n->serial = s_serial;
    n->withdrawn = FALSE;
    mp_notice_name(n->town, town);
    mp_notice_name(n->name, name);
    return n->serial;
}

int mp_notice_busy(void) {
    return s_count > 0 || s_actor != NULL;
}

// the window opens: the actor takes the head of the queue
int mp_notice_begin(void* actor, unsigned int* serial, unsigned char* town, unsigned char* name) {
    if (actor != s_actor || s_count == 0) {
        *serial = 0;
        mp_notice_name(town, NULL);
        mp_notice_name(name, NULL);
        return 0;
    }
    s_on_air = TRUE;
    *serial = s_queue[0].serial;
    memcpy(town, s_queue[0].town, MP_NAME_LEN);
    memcpy(name, s_queue[0].name, MP_NAME_LEN);
    return s_queue[0].msg_no;
}

int mp_notice_withdrawn(void* actor) {
    return actor == s_actor && (s_count == 0 || s_queue[0].withdrawn);
}

// the actor is gone; a notice it spoke, or one taken back, leaves the queue
void mp_notice_actor_gone(void* actor, int spoken) {
    if (actor != s_actor) {
        return;
    }
    s_actor = NULL;
    s_on_air = FALSE;
    if (s_count > 0 && (spoken || s_queue[0].withdrawn)) {
        mp_notice_remove(0);
    }
}

void mp_notice_answer(unsigned int serial, int yes) {
    s_answer_serial = serial;
    s_answer = yes ? 1 : 0;
}

int mp_notice_take_answer(unsigned int serial) {
    int answer = -1;

    if (s_answer >= 0) {
        if (s_answer_serial == serial) {
            answer = s_answer;
        }
        s_answer = -1; // an answer to anyone else's question is stale
    }
    return answer;
}

// a notice nobody needs any more; TRUE when it can't be heard now. One the actor has already
// asked a window for may still open next frame, so it's withdrawn but counts as heard
int mp_notice_cancel(unsigned int serial) {
    int unheard = FALSE;
    int i;

    for (i = s_count - 1; i >= 0; i--) {
        if (s_queue[i].serial != serial) {
            continue;
        }
        if (i == 0 && s_actor != NULL) {
            s_queue[0].withdrawn = TRUE;
        } else {
            mp_notice_remove(i);
            unheard = TRUE;
        }
    }
    return unheard;
}

void mp_notice_clear(void) {
    if (s_actor != NULL && s_count > 0) {
        s_queue[0].withdrawn = TRUE;
        s_count = 1; // the actor lets go of its own entry
    } else {
        s_count = 0;
    }
    s_answer = -1;
}

void mp_notice_tick(GAME_PLAY* play) {
    if (s_count == 0 || s_actor != NULL || !mp_quiet_moment(play)) {
        return;
    }
    s_actor = Actor_info_make_actor(&play->actor_info, (GAME*)play, mAc_PROFILE_MP_NOTICE, 0.0f, 0.0f, 0.0f, 0, 0, 0,
                                    -1, -1, -1, EMPTY_NO, 0, -1, -1);
}

#endif
