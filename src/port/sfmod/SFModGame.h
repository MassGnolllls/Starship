#pragma once

/*
 * Main game of the mod: the setup screen of the "Main Game" card (mode, difficulty 1-7, expert mode,
 * mission practice settings), the state of the current playthrough, and the difficulty effects.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

typedef enum SFGameMode {
    SFGAME_STORY,
    SFGAME_GAUNTLET,
    SFGAME_PRACTICE,
    SFGAME_MODE_COUNT
} SFGameMode;

// characters that get a personality for the whole playthrough
typedef enum SFRunCharacter {
    SFRUN_FALCO,
    SFRUN_SLIPPY,
    SFRUN_PEPPY,
    SFRUN_WOLF,
    SFRUN_LEON,
    SFRUN_PIGMA,
    SFRUN_ANDREW,
    SFRUN_BILL,
    SFRUN_KATT,
    SFRUN_CHARACTER_COUNT
} SFRunCharacter;

typedef struct SFRunState {
    bool active;          // a playthrough started from the setup screen is running
    int32_t mode;         // SFGameMode
    int32_t difficulty;   // 1 (original) .. 7
    bool expert;
    int32_t practiceMission; // index into the practice mission list
    int32_t practiceLaser;   // 0 single, 1 twin, 2 hyper
    int32_t practiceBombs;
    uint32_t seed;
    int32_t personality[SFRUN_CHARACTER_COUNT];
    // gauntlet
    int32_t gauntlet[16];   // levels in the order they are played
    int32_t gauntletCount;
    int32_t gauntletIndex;
    // Venom via Area 6: Venom 1, then Star Wolf, then the real Andross
    bool redRoute;
} SFRunState;

extern SFRunState gSFRun;

// difficulty 1..7 of what is being played right now (1 outside of the main game)
int32_t SFModGame_Difficulty(void);
const char* SFModGame_DifficultyName(int32_t level);

// setup screen (the main menu's "Main Game" card)
void SFModGame_MenuUpdate(void);
void SFModGame_MenuDraw(void);
void SFModGame_OnMenuEntered(void);
int32_t SFModGame_PracticeMissionCount(void);
int32_t SFModGame_PracticeLevel(int32_t index);

// game flow (gauntlet, practice, red route): called before the game switches to gNextGameState
void SFModGame_OnNextGameState(void);
bool SFModGame_TakeReturnToSetup(void);
int32_t SFModGame_FirstLevel(void);
// false after a gauntlet or a practice mission: they don't go into the ranking
bool SFModGame_EndedRunIsRanked(void);
// while paused: Z quits a practice mission or the gauntlet
void SFModGame_PauseUpdate(void);
// gauntlet stage / practice texts over the HUD
void SFModGame_DrawHud(void);
// automated testing: SFMOD_TEST_GAME=<mode> (see SFModGame.c)
bool SFModGame_TestBoot(void);

// difficulty effects
uint16_t SFModGame_ScaleBossDamage(uint16_t damage);
uint16_t SFModGame_ScaleActorDamage(void* actor, int32_t sourceId, uint16_t damage);
void SFModGame_OnEnemyLaser(int32_t objId, float xPos, float yPos, float zPos, float speed);
void SFModGame_Update(void); // once per game frame while playing
void SFModGame_AdjustAllRange(void* actor, float* fireChance, uint8_t* fireMask, float* fireRange);
bool SFModGame_ActorHasTwinLaser(void* actor);
// new AI for a chasing all-range actor: true when it flies this actor (fire 0/1, move SFAIActorMove)
bool SFModGame_StoryAI(void* actor, bool fighting, int32_t* fire, int32_t* move);

#ifdef __cplusplus
}
#endif
