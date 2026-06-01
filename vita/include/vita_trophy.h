// vita_trophy.h
// PSN trophy support (sceNpTrophy + NoTrpDrm): save-state polling + event hooks.

#ifndef VITA_TROPHY_H
#define VITA_TROPHY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Trophy IDs. MUST match the id= attributes in sce_sys/trophy TROPCONF.SFM.
enum {
    TROPHY_MAYORS_COMMENDATION = 0, // platinum, auto-unlocks when all below earned
    TROPHY_WELCOME_TO_TOWN = 1,
    TROPHY_PAID_IN_FULL = 2,
    TROPHY_FIRST_DONATION = 3,
    TROPHY_MASTER_ANGLER = 4,
    TROPHY_ENTOMOLOGIST = 5,
    TROPHY_FOSSIL_HUNTER = 6,
    TROPHY_ART_CONNOISSEUR = 7,
    TROPHY_THE_CURATOR = 8,
    TROPHY_FIRST_CATCH = 9,
    TROPHY_NET_GAIN = 10,
    TROPHY_THE_BIG_ONE = 11,
    TROPHY_TOURNEY_CHAMP = 12,
    TROPHY_RETRO_GAMER = 13,
    TROPHY_FULL_LIBRARY = 14,
    TROPHY_PENPAL = 15,
    TROPHY_BEST_FRIENDS = 16,
    TROPHY_NEW_NEIGHBOR = 17,
    TROPHY_SATURDAY_NIGHT = 18,
    TROPHY_BOOTLEG_COLLECTOR = 19,
    TROPHY_BLACK_MARKET = 20,
    TROPHY_RESETTIS_WRATH = 21,
    TROPHY_FESTIVAL_SPIRIT = 22,
    TROPHY_GOING_FOR_GOLD = 23,
    TROPHY_ALL_THAT_GLITTERS = 24,
    TROPHY_PERFECT_TOWN = 25,
    TROPHY_PATTERN_DESIGNER = 26,
    TROPHY_SOUND_OF_HOME = 27,
    TROPHY_NOUVEAU_RICHE = 28,
    TROPHY_BELL_BARON = 29,
    TROPHY_STALK_MARKET = 30,
    TROPHY_ISLAND_GETAWAY = 31,
    TROPHY_GOING_COCONUTS = 32,
    TROPHY_ISLAND_HOSPITALITY = 33,
    TROPHY_INTERIOR_DESIGNER = 34,
    TROPHY_GYROID_FOUND = 35,
    TROPHY_LET_IT_SNOW = 36,

    TROPHY_COUNT = 37
};

// Boots the NP trophy context + setup dialog. Call once after vitaGL init and
// before the game loop. No-op (safe) if VITA_TROPHIES is not defined.
void vita_trophy_init(void);

// Runs the Phase 1 save-state checks. Call each frame; self-rate-limited.
void vita_trophy_poll(void);

// Unlocks a trophy by id (idempotent). Phase 2 event hooks call this.
void vita_trophy_unlock(uint32_t id);

// Non-zero if the trophy is already unlocked.
int vita_trophy_is_unlocked(uint32_t id);

#ifdef __cplusplus
}
#endif

#endif // VITA_TROPHY_H
