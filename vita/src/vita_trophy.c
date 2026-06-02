// vita_trophy.c
// PSN trophies via sceNpTrophy. Needs NoTrpDrm.suprx; degrades to no-op without it.

#ifdef TARGET_VITA

#include "vita_trophy.h"

#if defined(VITA_TROPHIES)

#include <psp2/sysmodule.h>
#include <psp2/common_dialog.h>
#include <psp2/kernel/threadmgr.h>
#include <vitaGL.h>
#include <string.h>

#include "m_common_data.h"
#include "m_private.h"
#include "m_npc.h"
#include "m_home_h.h"
#include "m_field_assessment.h"
#include "m_island.h"
#include "m_museum_display.h"
#include "m_room_type.h"
#include "m_name_table.h"
#include "m_personal_id.h"

#ifndef PSP2_SDK_VERSION
#define PSP2_SDK_VERSION 0x03570011
#endif

// sceNpTrophy is shipped as a stub lib but has no vitasdk header.
typedef struct {
    int sdkVersion;
    SceCommonDialogParam commonParam;
    int context;
    int options;
    uint8_t reserved[128];
} SceNpTrophySetupDialogParam;

typedef struct {
    uint32_t flag[4];
} SceNpTrophyUnlockState;

extern int sceNpTrophyInit(void* opt);
extern int sceNpTrophyCreateContext(int* context, const char* commId, const char* commSign, uint64_t options);
extern int sceNpTrophySetupDialogInit(SceNpTrophySetupDialogParam* param);
extern SceCommonDialogStatus sceNpTrophySetupDialogGetStatus(void);
extern int sceNpTrophySetupDialogTerm(void);
extern int sceNpTrophyCreateHandle(int* handle);
extern int sceNpTrophyDestroyHandle(int handle);
extern int sceNpTrophyUnlockTrophy(int ctx, int handle, int id, int* platId);
extern int sceNpTrophyGetTrophyUnlockState(int ctx, int handle, SceNpTrophyUnlockState* state, uint32_t* count);

#define TROPHY_FRIENDSHIP_MAX 127 // s8 cap enforced by mNpc_AddFriendship
#define TROPHY_GOLDEN_ALL_MASK 0x0F // axe|net|rod|shovel = mPlayer_GOLDEN_ITEM_TYPE_NUM bits
#define TROPHY_POLL_INTERVAL 60 // frames between save-state scans

static char s_comm_id[12] = { 0 };
// NP Communication Signature for the NoTrpDrm homebrew trophy workflow.
static char s_signature[160] = {
    0xb9, 0xdd, 0xe1, 0x3b, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xca, 0xb0, 0xab, 0x0f,
    0x86, 0xb4, 0x53, 0x52, 0xe8, 0x28, 0x36, 0x91,
    0xea, 0x2e, 0xa6, 0x33, 0x4d, 0xbd, 0x9f, 0x68,
    0x16, 0x8a, 0xba, 0x2b, 0xe1, 0xbc, 0x06, 0xb2,
    0x84, 0xa4, 0xdd, 0x61, 0x9e, 0xb0, 0xb8, 0xfb,
    0xf8, 0xcf, 0xee, 0x7e, 0x1d, 0xd2, 0xc1, 0xc2,
    0xc9, 0x8a, 0x7b, 0x0a, 0xa5, 0x59, 0xd1, 0xd2,
    0x18, 0xd5, 0xea, 0xb9, 0x07, 0x1c, 0xbb, 0x64,
    0x98, 0x9a, 0xc2, 0xe1, 0x98, 0x66, 0xb0, 0x7c,
    0xee, 0xb0, 0x2e, 0x16, 0x58, 0x63, 0x77, 0xab,
    0x8d, 0x68, 0x52, 0x38, 0x4c, 0x4a, 0xf3, 0x8a,
    0x7c, 0x93, 0x33, 0xec, 0x37, 0xdf, 0x66, 0xe0,
    0x17, 0x92, 0xde, 0xdd, 0x36, 0x05, 0xd4, 0x85,
    0x18, 0x1b, 0x5f, 0x1e, 0x23, 0x78, 0x36, 0x21,
    0x72, 0x69, 0xbd, 0x3d, 0x7f, 0x16, 0xba, 0x2b,
    0x14, 0x6d, 0x87, 0xac, 0x4d, 0x08, 0xc3, 0xd1,
    0x8b, 0x9b, 0x7e, 0x0b, 0xd4, 0x90, 0xf5, 0x6c,
    0xe3, 0x2e, 0x08, 0xba, 0x11, 0xb8, 0xb0, 0x81,
    0x2a, 0x5b, 0xdf, 0x33, 0x68, 0x55, 0x94, 0x6c,
};
static int s_ctx;
static int s_plat_id = -1;
static int s_available = 0;
static volatile int s_req_id;
static SceUID s_req_sema, s_done_sema;
static SceNpTrophyUnlockState s_unlocks;
static int s_poll_div = 0;

// the 19 common Famicom games; special-event NES titles are excluded so
// "Full Library" stays attainable. FTR_START maps each to its _SOUTH item-no.
static const mActor_name_t s_nes_items[] = {
    FTR_START(FTR_FAMICOM_COMMON00), FTR_START(FTR_FAMICOM_COMMON01), FTR_START(FTR_FAMICOM_COMMON02),
    FTR_START(FTR_FAMICOM_COMMON03), FTR_START(FTR_FAMICOM_COMMON04), FTR_START(FTR_FAMICOM_COMMON05),
    FTR_START(FTR_FAMICOM_COMMON06), FTR_START(FTR_FAMICOM_COMMON07), FTR_START(FTR_FAMICOM_COMMON08),
    FTR_START(FTR_FAMICOM_COMMON09), FTR_START(FTR_FAMICOM_COMMON10), FTR_START(FTR_FAMICOM_COMMON11),
    FTR_START(FTR_FAMICOM_COMMON12), FTR_START(FTR_FAMICOM_COMMON13), FTR_START(FTR_FAMICOM_COMMON14),
    FTR_START(FTR_FAMICOM_COMMON15), FTR_START(FTR_FAMICOM_COMMON16), FTR_START(FTR_FAMICOM_COMMON17),
    FTR_START(FTR_FAMICOM_COMMON18),
};

static int trophies_unlocker(SceSize args, void* argp) {
    (void)args;
    (void)argp;
    for (;;) {
        sceKernelWaitSema(s_req_sema, 1, NULL);
        int id = s_req_id;
        int handle;
        sceNpTrophyCreateHandle(&handle);
        sceNpTrophyUnlockTrophy(s_ctx, handle, id, &s_plat_id);
        sceNpTrophyDestroyHandle(handle);
        sceKernelSignalSema(s_done_sema, 1);
    }
    return 0;
}

int vita_trophy_is_unlocked(uint32_t id) {
    if (!s_available || id >= TROPHY_COUNT) {
        return 0;
    }
    return (s_unlocks.flag[id >> 5] & (1u << (id & 31))) != 0;
}

void vita_trophy_unlock(uint32_t id) {
    if (!s_available || id >= TROPHY_COUNT || vita_trophy_is_unlocked(id)) {
        return;
    }
    s_unlocks.flag[id >> 5] |= (1u << (id & 31));
    sceKernelWaitSema(s_done_sema, 1, NULL);
    s_req_id = (int)id;
    sceKernelSignalSema(s_req_sema, 1);
}

void vita_trophy_init(void) {
    strcpy(s_comm_id, "ACGC00001");
    sceSysmoduleLoadModule(SCE_SYSMODULE_NP_TROPHY);
    sceNpTrophyInit(NULL);

    if (sceNpTrophyCreateContext(&s_ctx, s_comm_id, s_signature, 0) < 0) {
        return; // NoTrpDrm missing or registration failed; run without trophies
    }

    SceNpTrophySetupDialogParam setup;
    memset(&setup, 0, sizeof(setup));
    _sceCommonDialogSetMagicNumber(&setup.commonParam);
    setup.sdkVersion = PSP2_SDK_VERSION;
    setup.context = s_ctx;
    sceNpTrophySetupDialogInit(&setup);
    while (sceNpTrophySetupDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING) {
        vglSwapBuffers(GL_TRUE);
    }
    sceNpTrophySetupDialogTerm();

    s_done_sema = sceKernelCreateSema("trophy_done", 0, 1, 1, NULL);
    s_req_sema = sceKernelCreateSema("trophy_req", 0, 0, 1, NULL);
    SceUID thd = sceKernelCreateThread("trophy_unlocker", &trophies_unlocker, 0x10000100, 0x10000, 0, 0, NULL);
    sceKernelStartThread(thd, 0, NULL);

    int handle;
    uint32_t count;
    sceNpTrophyCreateHandle(&handle);
    sceNpTrophyGetTrophyUnlockState(s_ctx, handle, &s_unlocks, &count);
    sceNpTrophyDestroyHandle(handle);

    s_available = 1;
}

static void poll_save_state(void) {
    int fish = mMmd_CountDisplayedFish();
    int insect = mMmd_CountDisplayedInsect();
    int fossil = mMmd_CountDisplayedFossil();
    int art = mMmd_CountDisplayedArt();

    if (fish || insect || fossil || art) {
        vita_trophy_unlock(TROPHY_FIRST_DONATION);
    }
    if (fish >= mMmd_FISH_NUM) {
        vita_trophy_unlock(TROPHY_MASTER_ANGLER);
    }
    if (insect >= mMmd_INSECT_NUM) {
        vita_trophy_unlock(TROPHY_ENTOMOLOGIST);
    }
    if (fossil >= mMmd_FOSSIL_NUM) {
        vita_trophy_unlock(TROPHY_FOSSIL_HUNTER);
    }
    if (art >= mMmd_ART_NUM) {
        vita_trophy_unlock(TROPHY_ART_CONNOISSEUR);
    }
    if (fish >= mMmd_FISH_NUM && insect >= mMmd_INSECT_NUM && fossil >= mMmd_FOSSIL_NUM && art >= mMmd_ART_NUM) {
        vita_trophy_unlock(TROPHY_THE_CURATOR);
    }

    for (int p = 0; p < PLAYER_NUM; p++) {
        Private_c* pr = &Save_Get(private_data[p]);
        mHm_hs_c* hm = &Save_Get(homes[p]);

        if (pr->inventory.loan != 0 || hm->size_info.size > mHm_HOMESIZE_SMALL) {
            vita_trophy_unlock(TROPHY_WELCOME_TO_TOWN);
        }
        if (hm->size_info.size >= mHm_HOMESIZE_STATUE || hm->size_info.statue_ordered) {
            vita_trophy_unlock(TROPHY_PAID_IN_FULL);
        }
        if (pr->reset_count > 0) {
            vita_trophy_unlock(TROPHY_RESETTIS_WRATH);
        }
        if (pr->golden_items_collected != 0) {
            vita_trophy_unlock(TROPHY_GOING_FOR_GOLD);
        }
        if ((pr->golden_items_collected & TROPHY_GOLDEN_ALL_MASK) == TROPHY_GOLDEN_ALL_MASK) {
            vita_trophy_unlock(TROPHY_ALL_THAT_GLITTERS);
        }
        if (pr->state_flags & mPr_FLAG_TOTAKEKE_INTRODUCTION) {
            vita_trophy_unlock(TROPHY_SATURDAY_NIGHT);
        }
        if (pr->bank_account >= 100000u) {
            vita_trophy_unlock(TROPHY_NOUVEAU_RICHE);
        }
        if (pr->bank_account >= 999999u) {
            vita_trophy_unlock(TROPHY_BELL_BARON);
        }
        if (hm->flags.hra_reward1) {
            vita_trophy_unlock(TROPHY_INTERIOR_DESIGNER);
        }

        const u32* air = pr->aircheck_collect_bitfield;
        if (air[0] == 0xFFFFFFFFu && (air[1] & 0xFFFFFu) == 0xFFFFFu) {
            vita_trophy_unlock(TROPHY_BOOTLEG_COLLECTOR);
        }

        int nes_any = 0;
        int nes_all = 1;
        for (int k = 0; k < (int)(sizeof(s_nes_items) / sizeof(s_nes_items[0])); k++) {
            int idx = mRmTp_FtrItemNo2FtrIdx(s_nes_items[k]);
            int got = idx >= 0 && (pr->furniture_collected_bitfield[idx >> 5] & (1u << (idx & 31)));
            if (got) {
                nes_any = 1;
            } else {
                nes_all = 0;
            }
        }
        if (nes_any) {
            vita_trophy_unlock(TROPHY_RETRO_GAMER);
        }
        if (nes_all) {
            vita_trophy_unlock(TROPHY_FULL_LIBRARY);
        }

        // designs init to "blank"; slots 0..3 carry ROM defaults, so a created
        // design shows as a non-"blank" name in slots 4..7.
        for (int d = mNW_DEFAULT_ORIGINAL_TEX_NUM; d < mPr_ORIGINAL_DESIGN_COUNT; d++) {
            const u8* nm = pr->my_org[d].name;
            if (!(nm[0] == 'b' && nm[1] == 'l' && nm[2] == 'a' && nm[3] == 'n' && nm[4] == 'k')) {
                vita_trophy_unlock(TROPHY_PATTERN_DESIGNER);
                break;
            }
        }

        for (mActor_name_t it = HANIWA_START; it <= HANIWA_END; it += 4) {
            int idx = mRmTp_FtrItemNo2FtrIdx(it);
            if (idx >= 0 && (pr->furniture_collected_bitfield[idx >> 5] & (1u << (idx & 31)))) {
                vita_trophy_unlock(TROPHY_GYROID_FOUND);
                break;
            }
        }

        int furn_count = 0;
        for (int k = 0; k < (int)(sizeof(pr->furniture_collected_bitfield) / sizeof(u32)); k++) {
            furn_count += __builtin_popcount(pr->furniture_collected_bitfield[k]);
        }
        if (furn_count >= 50) {
            vita_trophy_unlock(TROPHY_PACK_RAT);
        }
        if (pr->reset_count >= 5) {
            vita_trophy_unlock(TROPHY_RESETTIS_NEMESIS);
        }
        if (pr->sunburn.rank > 0) {
            vita_trophy_unlock(TROPHY_BEACH_BUM);
        }
    }

    if (Save_Get(num_statues) > 0) {
        vita_trophy_unlock(TROPHY_PAID_IN_FULL);
    }
    if (Save_Get(good_field).perfect_day_streak > 0) {
        vita_trophy_unlock(TROPHY_PERFECT_TOWN);
    }

    // 0x7CF7...EE is the default town tune set by mMld_SetDefaultMelody.
    if (Save_Get(melody) != 0x7CF76BF9AEDE3FEEULL && Save_Get(island.flag_design).flag_design_set) {
        vita_trophy_unlock(TROPHY_SOUND_OF_HOME);
    }

    for (int a = 0; a < ANIMAL_NUM_MAX; a++) {
        Animal_c* an = &Save_Get(animals[a]);
        if (an->moved_in) {
            vita_trophy_unlock(TROPHY_NEW_NEIGHBOR);
        }
        for (int m = 0; m < ANIMAL_MEMORY_NUM; m++) {
            if (an->memories[m].friendship >= TROPHY_FRIENDSHIP_MAX) {
                vita_trophy_unlock(TROPHY_BEST_FRIENDS);
                break;
            }
        }
    }

    Animal_c* islander = &Get_Island().animal;
    for (int m = 0; m < ANIMAL_MEMORY_NUM; m++) {
        if (islander->memories[m].friendship >= TROPHY_FRIENDSHIP_MAX) {
            vita_trophy_unlock(TROPHY_ISLAND_HOSPITALITY);
            break;
        }
    }
    for (int m = 0; m < ANIMAL_MEMORY_NUM; m++) {
        if (mPr_NullCheckPersonalID(&islander->memories[m].memory_player_id) == FALSE) {
            vita_trophy_unlock(TROPHY_ISLAND_GETAWAY);
            break;
        }
    }
}

static void poll_platinum(void) {
    for (int i = 1; i < TROPHY_COUNT; i++) {
        if (!vita_trophy_is_unlocked(i)) {
            return;
        }
    }
    vita_trophy_unlock(TROPHY_MAYORS_COMMENDATION);
}

void vita_trophy_poll(void) {
    if (!s_available) {
        return;
    }
    if (++s_poll_div < TROPHY_POLL_INTERVAL) {
        return;
    }
    s_poll_div = 0;

    poll_save_state();
    poll_platinum();
}

#else // !VITA_TROPHIES

void vita_trophy_init(void) {}
void vita_trophy_poll(void) {}
void vita_trophy_unlock(uint32_t id) { (void)id; }
int vita_trophy_is_unlocked(uint32_t id) {
    (void)id;
    return 0;
}

#endif // VITA_TROPHIES

#endif // TARGET_VITA
