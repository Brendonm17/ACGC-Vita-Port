#include "jaudio_NES/radio.h"
#include "jaudio_NES/game64.h"
#include "jaudio_NES/audiowork.h"

int Na_GetRadioCounter(Radio_c* radio) {
    int counter;
    u16 tempo = 0;

    group* group = nullptr;
    int unused = 0;

    if (AG.groups[sou_now_bgm_handle].flags.enabled != 0 && AG.groups[sou_now_bgm_handle].seq_id == 0xDA
#ifdef VITA_MP
        // still catching up to the other players' place in it
        && AG.groups[sou_now_bgm_handle].skip_ticks == 0
#endif
    ) {
        group = &(AG.groups)[sou_now_bgm_handle];

    } else {
        return -1;
    }

    counter = group->counter;

    (void)unused;

    if (counter < 744) {
        radio->measure = 0;
    } else {
        counter -= 744;
        radio->measure = (s8)(counter / 768);
        counter = counter - (radio->measure * 768);
        radio->measure++;
    }
    if (radio->measure == 9) {
        radio->measure_progress = counter / 216.0f;

    } else {
        radio->measure_progress = counter / 768.0f;
    }

    radio->tempo = AG.groups[sou_now_bgm_handle].tempo / AUDIO_TATUMS_PER_BEAT;
    tempo = radio->tempo;
    return 0;
}

#ifdef VITA_MP
// each music group's times round, counted by the song itself as it goes round (track.c); the audio thread
// writes these, so a reader takes a set between two equal, even Na_mp_seq values
static volatile s32 Na_mp_seq;
static volatile s32 Na_mp_loop_sc[AUDIO_GROUP_MAX]; // updates at the latest time round's start
static volatile s32 Na_mp_loops[AUDIO_GROUP_MAX];
static volatile s32 Na_mp_starts[AUDIO_GROUP_MAX]; // songs begun on each group
static volatile u8 Na_mp_origin[AUDIO_GROUP_MAX];  // the song's first counter set, its opening, is still to come
static volatile s32 Na_mp_period; // updates the radio song takes to go round (the same every time)

static void Na_MpWriteBegin(void) {
    Na_mp_seq++;
    __sync_synchronize();
}

static void Na_MpWriteEnd(void) {
    __sync_synchronize();
    Na_mp_seq++;
}

void Na_MpNoteStart(group* grp) {
    int g = (int)(grp - AG.groups);

    if (g >= 0 && g < AUDIO_GROUP_MAX) {
        Na_MpWriteBegin();
        Na_mp_loops[g] = 0;
        Na_mp_loop_sc[g] = 0;
        Na_mp_origin[g] = TRUE;
        Na_mp_starts[g]++;
        Na_MpWriteEnd();
    }
}

// the song set its counter: the first time as it opens, after that each time it goes round
void Na_MpNoteCounter(group* grp, u16 was) {
    int g = (int)(grp - AG.groups);

    if (g < 0 || g >= AUDIO_GROUP_MAX || (!Na_mp_origin[g] && grp->counter >= was)) {
        return;
    }
    Na_MpWriteBegin();
    if (Na_mp_origin[g]) {
        Na_mp_origin[g] = FALSE;
    } else {
        if (grp->seq_id == 0xDA && grp->script_counter > Na_mp_loop_sc[g]) {
            Na_mp_period = grp->script_counter - Na_mp_loop_sc[g];
        }
        Na_mp_loops[g]++;
    }
    Na_mp_loop_sc[g] = grp->script_counter;
    Na_MpWriteEnd();
}

// the radio song on the field music now, for the shared timeline (pc_mp_event.c): 1, or 0 while it isn't
// playing, or -1 while the audio thread was writing its counts
int Na_MpAeroState(int* updates, int* loop_sc, int* loops, int* period, int* starts) {
    int g = sou_now_bgm_handle;
    group* grp;
    int tries;

    if (g < 0 || g >= AUDIO_GROUP_MAX) {
        return 0;
    }
    grp = &AG.groups[g];
    if (grp->flags.enabled == 0 || grp->seq_id != 0xDA || grp->skip_ticks != 0) {
        return 0;
    }
    for (tries = 0; tries < 8; tries++) {
        s32 seq = Na_mp_seq;

        __sync_synchronize();
        if (seq & 1) {
            continue;
        }
        *updates = grp->script_counter;
        *loop_sc = Na_mp_loop_sc[g];
        *loops = Na_mp_loops[g];
        *starts = Na_mp_starts[g] * AUDIO_GROUP_MAX + g;
        *period = Na_mp_period;
        __sync_synchronize();
        // (a song starting over clears its counter a moment before its counts)
        if (Na_mp_seq == seq && *updates >= *loop_sc) {
            return 1;
        }
    }
    return -1;
}
#endif
