// vita_ime.c
// the Vita's own keyboard (libime) over the running game, for multiplayer chat
#include "pc_mp.h"

#ifdef VITA_MP

#include <psp2/libime.h>
#include <psp2/sysmodule.h>

#include <string.h>

static SceUChar8 s_work[SCE_IME_WORK_BUFFER_SIZE] __attribute__((aligned(64)));
static SceWChar16 s_text[SCE_IME_MAX_PREEDIT_LENGTH + SCE_IME_MAX_TEXT_LENGTH + 1];
static SceWChar16 s_initial[4] = { 1 }; // (SDL's opening text; its U+0001 has no glyph and is dropped)
static int s_loaded;
static int s_open;
static int s_close; // MP_IME_ENTER or MP_IME_CLOSED: shut after the update that saw it
static int s_top = 272;

static int vita_ime_len(void) {
    int n = 0;

    while (n < SCE_IME_MAX_TEXT_LENGTH && s_text[n] != 0) {
        n++;
    }
    return n;
}

static void vita_ime_event(void* arg, const SceImeEventData* e);

// 0: SDL's settings with our length; 1: the same with SDL's length; 2: bare
static int vita_ime_try(int step) {
    SceImeParam param;

    memset(s_text, 0, sizeof(s_text));
    sceImeParamInit(&param);
    param.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH;
    param.languagesForced = SCE_FALSE;
    param.type = SCE_IME_TYPE_DEFAULT;
    param.option = step < 2 ? SCE_IME_OPTION_NO_ASSISTANCE : 0;
    param.work = s_work;
    param.handler = vita_ime_event;
    param.initialText = s_initial;
    param.maxTextLength = step == 0 ? MP_CHAT_MAX : SCE_IME_MAX_TEXT_LENGTH;
    param.inputTextBuffer = s_text;
    return sceImeOpen(&param);
}

static void vita_ime_event(void* arg, const SceImeEventData* e) {
    (void)arg;
    switch (e->id) {
        case SCE_IME_EVENT_OPEN:
        case SCE_IME_EVENT_CHANGE_SIZE:
            if (e->param.rect.height > 0 && e->param.rect.y < 544) {
                s_top = (int)e->param.rect.y;
            }
            break;
        case SCE_IME_EVENT_UPDATE_TEXT:
            mp_chat_ime_text(s_text, vita_ime_len(), MP_IME_TYPING);
            break;
        case SCE_IME_EVENT_PRESS_ENTER:
            // (a refused word keeps the keyboard up to fix it)
            if (mp_chat_ime_text(s_text, vita_ime_len(), MP_IME_ENTER)) {
                s_close = MP_IME_ENTER;
            }
            break;
        case SCE_IME_EVENT_PRESS_CLOSE:
            s_close = MP_IME_CLOSED;
            break;
    }
}

int vita_ime_open(void) {
    int rc = -1;
    int step;

    if (s_open) {
        return 1;
    }
    if (!s_loaded) {
        rc = sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
        if (rc < 0) {
            pc_mp_log("[MP] chat: no system keyboard module (%08X)", rc);
            return 0;
        }
        s_loaded = 1;
    }
    for (step = 0; step < 3; step++) {
        rc = vita_ime_try(step);
        if (rc >= 0) {
            break;
        }
    }
    if (rc < 0) {
        pc_mp_log("[MP] chat: system keyboard wouldn't open (%08X)", rc);
        return 0;
    }
    s_open = 1;
    s_close = 0;
    return 1;
}

void vita_ime_close(void) {
    if (s_open) {
        sceImeClose();
        s_open = 0;
        s_close = 0;
    }
}

int vita_ime_active(void) {
    return s_open;
}

int vita_ime_top(void) {
    return s_top;
}

// every frame from the platform poll: the keyboard's events come through here
void vita_ime_update(void) {
    int how;

    if (!s_open) {
        return;
    }
    // (the system can shut it on its own, across a sleep)
    if (sceImeUpdate() == (int)SCE_IME_ERROR_NOT_OPENED) {
        s_open = 0;
        s_close = 0;
        mp_chat_ime_text(NULL, 0, MP_IME_CLOSED);
        return;
    }
    how = s_close;
    if (how != 0) {
        vita_ime_close();
        if (how == MP_IME_CLOSED) {
            mp_chat_ime_text(NULL, 0, MP_IME_CLOSED);
        }
    }
}

#endif
