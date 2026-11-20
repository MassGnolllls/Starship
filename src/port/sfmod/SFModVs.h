#pragma once

/*
 * VS CPU mode: up to 10 fliers (1 human + CPUs, or CPUs only) in the VS arenas.
 * The CPU pilots are normal players whose controller input comes from the AI (SFModAI.c),
 * so they follow exactly the same rules as the human player.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "sfmod_config.h"

typedef enum SFVsCharacter {
    SFVS_FOX,
    SFVS_FALCO,
    SFVS_PEPPY,
    SFVS_SLIPPY,
    SFVS_WOLF,
    SFVS_PIGMA,
    SFVS_ANDREW,
    SFVS_LEON,
    SFVS_BILL,
    SFVS_KATT,
    SFVS_CHARACTER_COUNT
} SFVsCharacter;

typedef enum SFVsRule {
    SFVS_RULE_POINTS,  // first pilot/team to reach the kill target wins
    SFVS_RULE_TIME,    // most kills when the time is up
    SFVS_RULE_SURVIVAL, // every pilot has a number of lives, last pilot/team flying wins
    SFVS_RULE_COUNT
} SFVsRule;

#define SFVS_TEAM_NONE 0
#define SFVS_TEAM_COUNT 4 // red, blue, green, yellow

typedef struct SFVsPilot {
    int32_t character; // SFVsCharacter
    int32_t level;     // 1..7 (CPU only)
    int32_t personality; // -1 = random, otherwise SFAI personality index (CPU only)
    int32_t team;      // 0 = none (free for all), 1..4
} SFVsPilot;

typedef struct SFVsConfig {
    int32_t numPilots;  // 2..10
    bool humanPlays;    // pilot 1 is the human player, otherwise CPUs only
    int32_t stage;      // VS_STAGE_*
    int32_t rule;       // SFVsRule
    int32_t target;     // kills to win / minutes / lives
    bool teams;
    SFVsPilot pilots[MAX_PLAYERS];
} SFVsConfig;

extern SFVsConfig gSFVs;

bool SFModVs_Active(void);
int32_t SFModVs_ViewedPlayer(void);
bool SFModVs_IsHuman(int32_t playerNum);
// Personal sounds (being hit, shield warnings, lock-ons, item pickups) are only heard for the watched
// pilot; false when this pilot's sound should stay silent
bool SFModVs_HearsSfx(int32_t playerNum, uint32_t sfxId);
bool SFModVs_SameTeam(int32_t a, int32_t b);
void SFModVs_GetPlayerColor(int32_t playerNum, uint8_t* r, uint8_t* g, uint8_t* b);
const char* SFModVs_CharacterName(int32_t character);
int32_t SFModVs_Character(int32_t playerNum);

// main menu screen (VS card)
void SFModVs_MenuUpdate(void);
void SFModVs_MenuDraw(void);

// match
void SFModVs_Update(void);   // replaces Versus_Main while a VS CPU match runs
void SFModVs_Draw(void);     // replaces Versus_Draw
void SFModVs_DrawHud(void);  // HUD of the viewed pilot
void SFModVs_FillInputs(void); // controller input of the CPU pilots, once per frame
void SFModVs_PauseUpdate(void); // while the match is paused
void SFModVs_OnMatchExit(void);

// Automated testing (environment variables), see SFModVs.c
bool SFModVs_TestBoot(void);

#ifdef __cplusplus
}
#endif
