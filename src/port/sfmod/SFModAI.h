#pragma once

/*
 * CPU pilot AI. It never moves a ship directly: every frame it produces controller input
 * (stick + buttons) for its player, exactly like a human holding a controller.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

typedef struct SFAIPersonality {
    const char* name;
    float aggression; // how eagerly it attacks (0..1)
    float caution;    // how early it evades and how much it fears low shields (0..1)
    float turbo;      // prefers turbo fire over tapping (0..1)
    float charge;     // likes charged lock-on shots (0..1)
    float bomb;       // smart bomb usage (0..1)
    float roll;       // barrel rolls
    float loop;       // somersaults
    float uturn;      // U-turns
    float boost;      // boost / brake usage
    float greed;      // goes for items
} SFAIPersonality;

int32_t SFAI_PersonalityCount(void);
const SFAIPersonality* SFAI_GetPersonality(int32_t index);

// Prepare a CPU pilot. level 1..7, personality index (or -1 for a random one).
void SFAI_Reset(int32_t playerNum, int32_t level, int32_t personality, uint32_t seed);
int32_t SFAI_GetPersonalityIndex(int32_t playerNum);

// Computes this frame's controller state for the player (buttons + stick, already post dead zone).
// hold/press receive the button bits, stickX/stickY the stick (-60..60 like the game's processed stick).
void SFAI_Think(int32_t playerNum, uint16_t* hold, int8_t* stickX, int8_t* stickY);

// "BEST": an all-rounder that plays as strong as it can on level 7
#define SFAI_PERSONALITY_BEST 8
// the personality's values on a level (BEST changes on level 7)
const SFAIPersonality* SFAI_EffectivePersonality(int32_t index, int32_t level);

/*
 * Story dogfights: the same way of thinking for the all-range pilots of the levels (Star Wolf, enemy
 * fighters, the team). They are actors, not players, so the AI decides where the actor flies (its
 * destination, speed and turn rate), when it shoots and when it evades, and the actor code flies.
 */
typedef enum SFAIActorMove {
    SFAI_MOVE_NONE,
    SFAI_MOVE_LOOP,  // somersault
    SFAI_MOVE_UTURN,
    SFAI_MOVE_BREAK, // hard turn to the side
    SFAI_MOVE_ROLL,  // barrel roll (lasers bounce off)
} SFAIActorMove;

typedef struct SFAIActorOrder {
    bool fire;
    int32_t move; // SFAIActorMove
} SFAIActorOrder;

// actor: an all-range Actor that chases its target (aiIndex). level 1..7, personality index or -1.
// evade: may evade Fox's attacks (enemies). mayFire: the fight is on.
void SFAI_ActorThink(void* actor, int32_t level, int32_t personality, bool evade, bool mayFire, SFAIActorOrder* order);

#ifdef __cplusplus
}
#endif
