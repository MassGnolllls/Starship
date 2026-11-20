#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "SFModGame.h"
#include "SFModAI.h"
#include "SFMod.h"
#include "global.h"

/*
 * Main game: setup screen, playthrough state and the difficulty levels.
 *
 * Difficulty 1 is the original game. Every level above it adds (k = level - 1):
 *  - bosses (and Star Wolf) take 1 / (1 + 0.2 k) of the damage, which is +20% health per level
 *  - aimed enemy lasers come with extra lasers fanned out around them
 *  - enemies that never shoot in the original start shooting, the others shoot more
 *  - more enemy fighters (copies fly next to the originals)
 *  - dogfight enemies (Star Wolf, Katina, Fortuna, ...) aim, turn and fire better
 * The player's own damage and wings are never touched.
 *
 * Every named pilot gets a personality for the whole playthrough (the same ones the VS CPUs use),
 * and allies and Star Wolf pick up and use power-ups in dogfights.
 */

// fox_option.c
extern void Option_SFMod_ReturnToMainMenu(void);
extern void Option_SFMod_StartMainGame(void);
extern void Option_SFMod_StartLevel(s32 level);
// fox_effect.c
extern void func_effect_8007E6B8(Effect* this, u32 objId, f32 xPos, f32 yPos, f32 zPos, f32 speed);

SFRunState gSFRun;
static bool sStoryAIDebug = false;

static const char* sDifficultyNames[7] = {
    "EASIEST", "VERY EASY", "EASY", "MEDIUM", "HARD", "VERY HARD", "HARDEST",
};

static const char* sModeNames[SFGAME_MODE_COUNT] = { "STORY", "GAUNTLET", "MISSION PRACTICE" };

#define CVAR_SFGAME_PREFIX "gSFMod.Game."

static u32 sRng = 0x2545F491;

static u32 SFModGame_Rand(void) {
    sRng ^= sRng << 13;
    sRng ^= sRng >> 17;
    sRng ^= sRng << 5;
    return sRng;
}

static f32 SFModGame_RandF(void) {
    return (SFModGame_Rand() & 0xFFFFFF) / (f32) 0x1000000;
}

// stochastic rounding keeps the average exact
static s32 SFModGame_RoundRandom(f32 v) {
    s32 base = (s32) v;

    return base + ((SFModGame_RandF() < (v - base)) ? 1 : 0);
}

int32_t SFModGame_Difficulty(void) {
    if (!gSFRun.active || gVersusMode || (gCurrentLevel == LEVEL_TRAINING)) {
        return 1;
    }
    return CLAMP(gSFRun.difficulty, 1, 7);
}

static f32 SFModGame_K(void) {
    return (f32) (SFModGame_Difficulty() - 1);
}

const char* SFModGame_DifficultyName(int32_t level) {
    return sDifficultyNames[CLAMP(level, 1, 7) - 1];
}

static u32 sSeedOverride = 0; // tests only

static void SFModGame_NewPlaythrough(void) {
    s32 i;

    gSFRun.seed = (sSeedOverride != 0) ? sSeedOverride : ((u32) osGetTime() ^ 0x9E3779B9u);
    sRng = gSFRun.seed | 1;
    for (i = 0; i < SFRUN_CHARACTER_COUNT; i++) {
        gSFRun.personality[i] = SFModGame_Rand() % SFAI_PersonalityCount();
    }
    gSFRun.redRoute = false;
    gSFRun.gauntletIndex = 0;
    gSFRun.gauntletCount = 0;
    if (gSFRun.mode == SFGAME_GAUNTLET) {
        // Corneria first, Venom (the long way: Venom 1, Star Wolf, the real Andross) last, the rest shuffled
        static const s32 sMiddle[] = {
            LEVEL_METEO,   LEVEL_FORTUNA, LEVEL_KATINA,  LEVEL_AQUAS,    LEVEL_SECTOR_X, LEVEL_SOLAR, LEVEL_SECTOR_Y,
            LEVEL_ZONESS,  LEVEL_TITANIA, LEVEL_MACBETH, LEVEL_SECTOR_Z, LEVEL_BOLSE,    LEVEL_AREA_6,
        };
        s32 order[ARRAY_COUNT(sMiddle)];
        s32 n;

        memcpy(order, sMiddle, sizeof(order));
        for (n = ARRAY_COUNT(order) - 1; n > 0; n--) {
            s32 j = SFModGame_Rand() % (n + 1);
            s32 t = order[n];

            order[n] = order[j];
            order[j] = t;
        }
        gSFRun.gauntlet[gSFRun.gauntletCount++] = LEVEL_CORNERIA;
        for (n = 0; n < ARRAY_COUNT(order); n++) {
            gSFRun.gauntlet[gSFRun.gauntletCount++] = order[n];
        }
        gSFRun.gauntlet[gSFRun.gauntletCount++] = LEVEL_VENOM_1;
        printf("[SFGame] gauntlet:");
        for (n = 0; n < gSFRun.gauntletCount; n++) {
            printf(" %d", gSFRun.gauntlet[n]);
        }
        printf("\n");
    }
    gSFRun.active = true;
    sStoryAIDebug = getenv("SFMOD_DEBUG_STORYAI") != NULL;
    printf("[SFGame] new %s, difficulty %d, expert %d\n", sModeNames[gSFRun.mode], gSFRun.difficulty, gSFRun.expert);
    fflush(stdout);
}

static s32 sEndedMode = -1; // mode of the playthrough that ended when the menu was entered (-1: none)
static void SFModGame_GauntletResult(const char* what);
static bool sReturnToSetup = false;

void SFModGame_OnMenuEntered(void) {
    // back in the main menu: the playthrough is over
    sEndedMode = gSFRun.active ? gSFRun.mode : -1;
    if (gSFRun.active && (gSFRun.mode == SFGAME_GAUNTLET) && (gLastGameState == GSTATE_ENDING)) {
        // the whole gauntlet is done: back to the setup screen with the result
        gSFRun.gauntletIndex = gSFRun.gauntletCount - 1;
        SFModGame_GauntletResult("GAUNTLET COMPLETE");
        sReturnToSetup = true;
    }
    gSFRun.active = false;
}

bool SFModGame_EndedRunIsRanked(void) {
    return (sEndedMode != SFGAME_GAUNTLET) && (sEndedMode != SFGAME_PRACTICE);
}

/* ------------------------------------------------------------------------------------------------
 * Personalities of the named pilots (all-range actors use their AI360 index as actor slot)
 * ------------------------------------------------------------------------------------------------ */

static s32 SFModGame_RunCharacter(s32 aiType) {
    switch (aiType) {
        case AI360_FALCO:
            return SFRUN_FALCO;
        case AI360_SLIPPY:
            return SFRUN_SLIPPY;
        case AI360_PEPPY:
            return SFRUN_PEPPY;
        case AI360_WOLF:
            return SFRUN_WOLF;
        case AI360_LEON:
            return SFRUN_LEON;
        case AI360_PIGMA:
            return SFRUN_PIGMA;
        case AI360_ANDREW:
            return SFRUN_ANDREW;
        case AI360_BILL:
            return SFRUN_BILL;
        case AI360_KATT:
            return SFRUN_KATT;
        default:
            return -1;
    }
}

static const SFAIPersonality* SFModGame_PersonalityOf(s32 aiType) {
    s32 c = SFModGame_RunCharacter(aiType);

    if (!gSFRun.active || (c < 0)) {
        return NULL;
    }
    return SFAI_GetPersonality(gSFRun.personality[c]);
}

static bool SFModGame_IsStarWolf(s32 aiType) {
    return (aiType >= AI360_WOLF) && (aiType <= AI360_ANDREW);
}

static bool SFModGame_IsAlly(s32 aiType) {
    return ((aiType >= AI360_FALCO) && (aiType <= AI360_PEPPY)) || (aiType == AI360_KATT) || (aiType == AI360_BILL);
}

/* ------------------------------------------------------------------------------------------------
 * Damage scaling: +20% health per level for bosses and Star Wolf
 * ------------------------------------------------------------------------------------------------ */

uint16_t SFModGame_ScaleBossDamage(uint16_t damage) {
    f32 factor = 1.0f / (1.0f + 0.2f * SFModGame_K());

    if (factor >= 0.999f) {
        return damage;
    }
    if (damage == 31) {
        // Aquas checks this exact value for charged shots: let some of them not count instead
        return (SFModGame_RandF() < factor) ? 31 : 0;
    }
    return (u16) SFModGame_RoundRandom(damage * factor);
}

uint16_t SFModGame_ScaleActorDamage(void* actorPtr, int32_t sourceId, uint16_t damage) {
    Actor* actor = (Actor*) actorPtr;

    if ((actor->obj.id == OBJ_ACTOR_ALLRANGE) && SFModGame_IsStarWolf(actor->aiType) && (sourceId < 4)) {
        return SFModGame_ScaleBossDamage(damage);
    }
    return damage;
}

/* ------------------------------------------------------------------------------------------------
 * Enemy fire
 * ------------------------------------------------------------------------------------------------ */

static void SFModGame_SpawnFannedLaser(s32 objId, f32 x, f32 y, f32 z, f32 speed, f32 yawOff, f32 pitchOff) {
    s32 i;

    for (i = ARRAY_COUNT(gEffects) - 1; i >= 0; i--) {
        if (gEffects[i].obj.status == OBJ_FREE) {
            Effect* e = &gEffects[i];
            f32 vx;
            f32 vy;
            f32 vz;
            f32 c = cosf(yawOff * M_DTOR);
            f32 s = sinf(yawOff * M_DTOR);
            f32 len;

            Matrix_Push(&gCalcMatrix);
            func_effect_8007E6B8(e, objId, x, y, z, speed);
            Matrix_Pop(&gCalcMatrix);

            // turn the shot's own velocity (the path velocity is added on top of it)
            vx = e->vel.x - gPathVelX;
            vy = e->vel.y - gPathVelY;
            vz = e->vel.z + gPathVelZ;
            len = sqrtf(SQ(vx) + SQ(vy) + SQ(vz));
            {
                f32 nx = vx * c + vz * s;
                f32 nz = -vx * s + vz * c;

                vx = nx;
                vz = nz;
            }
            vy += len * sinf(pitchOff * M_DTOR);
            e->vel.x = vx + gPathVelX;
            e->vel.y = vy + gPathVelY;
            e->vel.z = vz - gPathVelZ;
            e->obj.rot.y += yawOff;
            e->obj.rot.x -= pitchOff;
            break;
        }
    }
}

// Called by Effect_EnemyLaser after it fired an aimed laser
void SFModGame_OnEnemyLaser(int32_t objId, float xPos, float yPos, float zPos, float speed) {
    f32 k = SFModGame_K();
    s32 extra;
    s32 i;

    if ((k <= 0.0f) || (objId != OBJ_EFFECT_ENEMY_LASER_1)) {
        return;
    }
    extra = SFModGame_RoundRandom(k * 0.33f); // level 7: 2 extra lasers
    for (i = 0; i < extra; i++) {
        f32 side = (i & 1) ? -1.0f : 1.0f;
        f32 spread = 3.0f + 2.0f * (i / 2) + SFModGame_RandF() * 2.0f;

        SFModGame_SpawnFannedLaser(objId, xPos, yPos, zPos, speed, side * spread,
                                   (SFModGame_RandF() - 0.5f) * 3.0f);
    }
}

/* ------------------------------------------------------------------------------------------------
 * Per-frame effects
 * ------------------------------------------------------------------------------------------------ */

#define SFGAME_ACTORS ARRAY_COUNT(gActors)

static s32 sPrevStatus[SFGAME_ACTORS];
static bool sShooter[SFGAME_ACTORS];
static bool sIsClone[SFGAME_ACTORS];
static s32 sShootTimer[SFGAME_ACTORS];
static bool sTwinLaser[SFGAME_ACTORS];
static s32 sBombs[SFGAME_ACTORS];
static s32 sMaxHealth[SFGAME_ACTORS];
static s32 sBombTimer[SFGAME_ACTORS];

// enemy fighters of the levels: they may get copies and may shoot more
static bool SFModGame_IsEnemyFighter(s32 eventType) {
    switch (eventType) {
        case EVID_VENOM_FIGHTER_1:
        case EVID_VENOM_FIGHTER_2:
        case EVID_VENOM_FIGHTER_3:
        case EVID_GRANGA_FIGHTER_1:
        case EVID_GRANGA_FIGHTER_2:
        case EVID_FIREBIRD:
        case EVID_ATTACKER_1:
        case EVID_ATTACKER_2:
        case EVID_ATTACKER_3:
        case EVID_SX_CANINE:
        case EVID_SX_BORZOI_FIGHTER:
        case EVID_TI_BOMBER:
        case EVID_KILLER_BEE:
        case EVID_ME_ROCK_GULL:
        case EVID_ZO_BIRD:
            return true;
        default:
            return false;
    }
}

// other enemies that may start shooting
static bool SFModGame_IsEnemyShooter(s32 eventType) {
    switch (eventType) {
        case EVID_SPY_EYE:
        case EVID_SX_SPY_EYE:
        case EVID_ME_FLIP_BOT:
        case EVID_VENOM_TANK:
        case EVID_TRIPOD:
        case EVID_SPIDER:
            return true;
        default:
            return SFModGame_IsEnemyFighter(eventType);
    }
}

static void SFModGame_CloneActor(s32 i) {
    Actor* src = &gActors[i];
    s32 j;

    for (j = 4; j < SFGAME_ACTORS; j++) {
        if (gActors[j].obj.status == OBJ_FREE) {
            Actor* dst = &gActors[j];
            f32 side = (SFModGame_RandF() < 0.5f) ? -1.0f : 1.0f;
            s32 n;

            memcpy(dst, src, sizeof(Actor));
            dst->index = j;
            dst->obj.pos.x += side * (220.0f + SFModGame_RandF() * 120.0f);
            dst->obj.pos.y += 60.0f + SFModGame_RandF() * 80.0f;
            dst->obj.pos.z += 150.0f + SFModGame_RandF() * 150.0f;
            dst->itemDrop = 0;
            for (n = 0; n < MAX_PLAYERS; n++) {
                dst->lockOnTimers[n] = 0;
            }
            sPrevStatus[j] = OBJ_ACTIVE;
            sIsClone[j] = true;
            sShooter[j] = sShooter[i];
            sShootTimer[j] = sShootTimer[i] + 20;
            return;
        }
    }
}

static void SFModGame_NewActor(s32 i) {
    Actor* a = &gActors[i];
    f32 k = SFModGame_K();

    sShooter[i] = false;
    sIsClone[i] = false;
    sTwinLaser[i] = false;
    sBombs[i] = 0;
    sBombTimer[i] = 0;
    sMaxHealth[i] = a->health;
    sShootTimer[i] = 60 + (s32) (SFModGame_RandF() * 90.0f);

    if ((a->obj.id == OBJ_ACTOR_EVENT) && SFModGame_IsEnemyShooter(a->eventType)) {
        // the higher the level, the more of these enemies shoot (on top of what they do anyway)
        sShooter[i] = SFModGame_RandF() < k * 0.15f;
        if (SFModGame_IsEnemyFighter(a->eventType) && (a->itemDrop == 0) && (k >= 2.0f) &&
            (SFModGame_RandF() < (k - 1.0f) * 0.09f)) {
            SFModGame_CloneActor(i);
        }
    }
}

static void SFModGame_UpdateShooter(s32 i) {
    Actor* a = &gActors[i];
    Player* pl = &gPlayer[0];
    f32 dx;
    f32 dy;
    f32 dz;
    f32 k = SFModGame_K();

    if (!sShooter[i] || (a->health <= 0) || (a->timer_0C2 != 0) || (pl->state != PLAYERSTATE_ACTIVE)) {
        return;
    }
    if (sShootTimer[i] > 0) {
        sShootTimer[i]--;
        return;
    }
    sShootTimer[i] = (s32) ((150.0f - 15.0f * k) * (0.7f + SFModGame_RandF() * 0.6f));

    dx = a->obj.pos.x - pl->pos.x;
    dy = a->obj.pos.y - pl->pos.y;
    dz = a->obj.pos.z - pl->trueZpos;
    if (gLevelMode == LEVELMODE_ON_RAILS) {
        // only from in front of the player, inside the view
        if ((dz > -800.0f) || (dz < -7000.0f) || (fabsf(dx) > 3000.0f) || (fabsf(dy) > 2500.0f)) {
            return;
        }
    } else if ((SQ(dx) + SQ(dy) + SQ(dz)) > SQ(4500.0f)) {
        return;
    }
    Effect_EnemyLaser(OBJ_EFFECT_ENEMY_LASER_1, a->obj.pos.x, a->obj.pos.y, a->obj.pos.z, gEnemyShotSpeed);
}

static s32 SFModGame_TeamShieldIndex(s32 aiType) {
    switch (aiType) {
        case AI360_FALCO:
            return TEAM_ID_FALCO;
        case AI360_SLIPPY:
            return TEAM_ID_SLIPPY;
        case AI360_PEPPY:
            return TEAM_ID_PEPPY;
        case AI360_KATT:
            return TEAM_ID_KATT;
        case AI360_BILL:
            return TEAM_ID_BILL;
        default:
            return -1;
    }
}

// Allies and Star Wolf fly through power-ups and use them
static void SFModGame_UpdatePowerUps(s32 i) {
    Actor* a = &gActors[i];
    s32 n;

    if ((a->obj.id != OBJ_ACTOR_ALLRANGE) || !(SFModGame_IsAlly(a->aiType) || SFModGame_IsStarWolf(a->aiType)) ||
        (a->health <= 0)) {
        return;
    }

    for (n = 0; n < ARRAY_COUNT(gItems); n++) {
        Item* item = &gItems[n];
        f32 dist2;
        bool taken = true;

        if (item->obj.status != OBJ_ACTIVE) {
            continue;
        }
        dist2 = SQ(item->obj.pos.x - a->obj.pos.x) + SQ(item->obj.pos.y - a->obj.pos.y) +
                SQ(item->obj.pos.z - a->obj.pos.z);
        if (dist2 > SQ(220.0f)) {
            continue;
        }
        switch (item->obj.id) {
            case OBJ_ITEM_LASERS:
                sTwinLaser[i] = true;
                break;
            case OBJ_ITEM_BOMB:
                sBombs[i]++;
                break;
            case OBJ_ITEM_SILVER_RING:
            case OBJ_ITEM_SILVER_STAR:
            case OBJ_ITEM_GOLD_RING:
            case OBJ_ITEM_WING_REPAIR: {
                s32 heal = (item->obj.id == OBJ_ITEM_SILVER_RING) ? 32 : 96;
                s32 team = SFModGame_TeamShieldIndex(a->aiType);

                if ((team >= 0) && (gTeamShields[team] > 0)) {
                    gTeamShields[team] = MIN(gTeamShields[team] + heal, 255);
                } else if (SFModGame_IsStarWolf(a->aiType)) {
                    a->health = MIN(a->health + heal / 2, MAX(sMaxHealth[i], a->health));
                }
                break;
            }
            default:
                taken = false;
                break;
        }
        if (taken) {
            AUDIO_PLAY_SFX(NA_SE_SHIELD_RING_M, a->sfxSource, 4);
            Object_Kill(&item->obj, item->sfxSource);
            printf("[SFGame] actor %d (ai %d) took item %d\n", i, a->aiType, item->obj.id);
            fflush(stdout);
        }
    }

    // a smart bomb on the current target when it is close in front
    if ((sBombs[i] > 0) && (sBombTimer[i]-- <= 0) && (a->aiIndex >= 0)) {
        Vec3f tgt;
        f32 dist2;

        sBombTimer[i] = 90;
        if (a->aiIndex == AI360_FOX) {
            tgt.x = gPlayer[0].pos.x;
            tgt.y = gPlayer[0].pos.y;
            tgt.z = gPlayer[0].trueZpos;
        } else if ((a->aiIndex < SFGAME_ACTORS) && (gActors[a->aiIndex].obj.status == OBJ_ACTIVE)) {
            tgt = gActors[a->aiIndex].obj.pos;
        } else {
            return;
        }
        dist2 = SQ(tgt.x - a->obj.pos.x) + SQ(tgt.y - a->obj.pos.y) + SQ(tgt.z - a->obj.pos.z);
        if ((dist2 < SQ(1800.0f)) && (SFModGame_RandF() < 0.25f)) {
            sBombs[i]--;
            if (a->aiIndex == AI360_FOX) {
                // a burst of lasers from Star Wolf
                for (n = 0; n < 5; n++) {
                    SFModGame_SpawnFannedLaser(OBJ_EFFECT_ENEMY_LASER_1, a->obj.pos.x, a->obj.pos.y, a->obj.pos.z,
                                               gEnemyShotSpeed, (n - 2) * 4.0f, 0.0f);
                }
            } else {
                Actor* t = &gActors[a->aiIndex];

                func_effect_8007D0E0(t->obj.pos.x, t->obj.pos.y, t->obj.pos.z, 8.0f);
                t->dmgType = DMG_EXPLOSION;
                t->damage = 30;
                t->dmgSource = a->aiType + NPC_SHOT_ID + 1; // like a hit by that pilot's laser
                AUDIO_PLAY_SFX(NA_SE_EN_EXPLOSION_S, t->sfxSource, 4);
            }
        }
    }
}

static void SFModGame_UpdateRules(void);

bool SFModGame_ActorHasTwinLaser(void* actorPtr) {
    Actor* a = (Actor*) actorPtr;
    s32 i = a - gActors;

    return gSFRun.active && (i >= 0) && (i < SFGAME_ACTORS) && sTwinLaser[i];
}

void SFModGame_Update(void) {
    s32 i;

    if (!gSFRun.active || gVersusMode || (gPlayer == NULL)) {
        return;
    }
    SFModGame_UpdateRules();
    if (sStoryAIDebug && ((gGameFrameCount % 300) == 0)) {
        printf("[SFGame] frame %d: Fox shields %d lives %d state %d\n", gGameFrameCount, gPlayer[0].shields,
               gLifeCount[0], gPlayer[0].state);
        fflush(stdout);
    }
    for (i = 0; i < SFGAME_ACTORS; i++) {
        Actor* a = &gActors[i];

        if (a->obj.status != OBJ_ACTIVE) {
            sPrevStatus[i] = a->obj.status;
            continue;
        }
        if (sPrevStatus[i] != OBJ_ACTIVE) {
            sPrevStatus[i] = OBJ_ACTIVE;
            SFModGame_NewActor(i);
        }
        if (a->obj.id == OBJ_ACTOR_EVENT) {
            SFModGame_UpdateShooter(i);
        } else if (a->obj.id == OBJ_ACTOR_ALLRANGE) {
            SFModGame_UpdatePowerUps(i);
        }
    }
}

// The new AI in the dogfights: Star Wolf and the enemy fighters on the chosen level, the team as
// decent wingmen. Difficulty 1 keeps the original AI.
bool SFModGame_StoryAI(void* actorPtr, bool fighting, int32_t* fire, int32_t* move) {
    Actor* a = (Actor*) actorPtr;
    s32 level = SFModGame_Difficulty();
    s32 skill;
    s32 personality = 0;
    bool evade;
    SFAIActorOrder order;

    *fire = 0;
    *move = SFAI_MOVE_NONE;
    if (!gSFRun.active || gVersusMode || (level <= 1) || (a->obj.id != OBJ_ACTOR_ALLRANGE)) {
        return false;
    }
    if ((a->aiType == AI360_FOX) || (a->aiType >= AI360_GREAT_FOX)) {
        return false; // missiles, the Great Fox, event handlers
    }
    if (a->aiIndex == AI360_FOX) {
        // chasing Fox
    } else if ((a->aiIndex <= 0) || (a->aiIndex >= SFGAME_ACTORS) || (a->aiIndex == AI360_GREAT_FOX) ||
               (gActors[a->aiIndex].obj.status != OBJ_ACTIVE) || (gActors[a->aiIndex].obj.id != OBJ_ACTOR_ALLRANGE) ||
               (gActors[a->aiIndex].aiType == AI360_MISSILE)) {
        return false; // Sector Z missiles and other special targets keep the original behavior
    }

    if (SFModGame_IsStarWolf(a->aiType)) {
        skill = level;
        evade = true;
        personality = gSFRun.personality[SFModGame_RunCharacter(a->aiType)];
    } else if (SFModGame_IsAlly(a->aiType)) {
        // the team flies well, whatever the level
        skill = 4;
        evade = false;
        personality = gSFRun.personality[SFModGame_RunCharacter(a->aiType)];
    } else {
        skill = level;
        evade = level >= 4;
    }
    SFAI_ActorThink(a, skill, personality, evade, fighting, &order);
    *fire = order.fire ? 1 : 0;
    *move = order.move;

    if (sStoryAIDebug) {
        static s32 sShots;
        static s32 sMoves[5];
        static s32 sFrames;
        static s32 sLastFrame;

        sShots += *fire;
        sMoves[CLAMP(*move, 0, 4)]++;
        if (gGameFrameCount != sLastFrame) {
            sLastFrame = gGameFrameCount;
            if (++sFrames >= 300) {
                printf("[SFGame] story AI (level %d): %d shots, moves loop %d uturn %d break %d roll %d, Fox shields %d\n",
                       level, sShots, sMoves[1], sMoves[2], sMoves[3], sMoves[4], gPlayer[0].shields);
                fflush(stdout);
                sFrames = sShots = 0;
                memset(sMoves, 0, sizeof(sMoves));
            }
        }
    }
    return true;
}

// Strength of the dogfight AI (ActorAllRange) and the personalities of the named pilots
void SFModGame_AdjustAllRange(void* actorPtr, float* fireChance, uint8_t* fireMask, float* fireRange) {
    Actor* a = (Actor*) actorPtr;
    const SFAIPersonality* p = SFModGame_PersonalityOf(a->aiType);
    f32 k = SFModGame_K();
    bool enemy = (a->aiType >= AI360_ENEMY) || SFModGame_IsStarWolf(a->aiType);

    if (!gSFRun.active) {
        return;
    }
    // (only this frame's local firing values: the actor's own speed/turn work values persist between frames)
    if (p != NULL) {
        // a personal touch only: difficulty 1 stays close to the original
        *fireChance *= 0.85f + 0.3f * p->aggression;
        *fireRange *= 0.95f + 0.1f * p->aggression;
    }
    if (enemy && (k > 0.0f)) {
        *fireChance *= 1.0f + 0.12f * k;
        *fireRange *= 1.0f + 0.05f * k;
        if (k >= 4.0f) {
            *fireMask &= 3;
        } else if (k >= 2.0f) {
            *fireMask &= 7;
        }
    }
    if (*fireChance > 1.0f) {
        *fireChance = 1.0f;
    }
}

/* ------------------------------------------------------------------------------------------------
 * Game flow: gauntlet, mission practice, red route to Venom
 *
 * Story:    Area 6 leads to Venom 1 (instead of straight to Star Wolf); after the boss of Venom 1 comes the
 *           Star Wolf fight of Venom 2 and then the real Andross. Bolse -> Venom 1 -> fake Andross as before.
 * Gauntlet: Corneria, the other planets in a random order, Venom (the long way) last. Lives work like in the
 *           story (no continues), in Sector Z a missile that hits the Great Fox costs the try.
 * Practice: one mission, endless tries, the chosen laser and bombs at every try, back to the setup screen
 *           when the mission ends. Z in the pause screen ends a practice mission or the gauntlet.
 * ------------------------------------------------------------------------------------------------ */

typedef struct {
    s32 level;
    const char* name;
    bool redRoute;
} SFPracticeMission;

static const SFPracticeMission sPracticeMissions[] = {
    { LEVEL_CORNERIA, "CORNERIA", false },
    { LEVEL_METEO, "METEO", false },
    { LEVEL_FORTUNA, "FORTUNA", false },
    { LEVEL_KATINA, "KATINA", false },
    { LEVEL_AQUAS, "AQUAS", false },
    { LEVEL_SECTOR_X, "SECTOR X", false },
    { LEVEL_SOLAR, "SOLAR", false },
    { LEVEL_SECTOR_Y, "SECTOR Y", false },
    { LEVEL_ZONESS, "ZONESS", false },
    { LEVEL_TITANIA, "TITANIA", false },
    { LEVEL_MACBETH, "MACBETH", false },
    { LEVEL_SECTOR_Z, "SECTOR Z", false },
    { LEVEL_BOLSE, "BOLSE", false },
    { LEVEL_AREA_6, "AREA 6", false },
    { LEVEL_VENOM_1, "VENOM 1", false },         // Bolse route: Venom 1, fake Andross
    { LEVEL_VENOM_1, "VENOM RED ROUTE", true },  // Area 6 route: Venom 1, Star Wolf, real Andross
    { LEVEL_VENOM_2, "VENOM 2", false },         // the original Area 6 route: Star Wolf, real Andross
};

int32_t SFModGame_PracticeMissionCount(void) {
    return ARRAY_COUNT(sPracticeMissions);
}

int32_t SFModGame_PracticeLevel(int32_t index) {
    return sPracticeMissions[CLAMP(index, 0, ARRAY_COUNT(sPracticeMissions) - 1)].level;
}

static const char* SFModGame_LevelName(s32 level) {
    s32 i;

    if ((level == LEVEL_VENOM_2) || (level == LEVEL_VENOM_ANDROSS)) {
        return "VENOM";
    }
    for (i = 0; i < ARRAY_COUNT(sPracticeMissions); i++) {
        if (sPracticeMissions[i].level == level) {
            return (level == LEVEL_VENOM_1) ? "VENOM" : sPracticeMissions[i].name;
        }
    }
    return "";
}

static bool sPracticeApplied = false;
static bool sSectorZLost = false;
// red route: Venom 1 and the Star Wolf fight are one mission, like Venom 1 and Andross on the Bolse route
static bool sRedCarry = false;
static s32 sCarry[7]; // shields + 1, gold rings, hits, wing states, wing health
static char sResultTitle[48];
static char sResultText[64];

static void SFModGame_ClearResult(void) {
    sResultTitle[0] = '\0';
    sResultText[0] = '\0';
}

static void SFModGame_GauntletResult(const char* what) {
    s32 stage = CLAMP(gSFRun.gauntletIndex, 0, gSFRun.gauntletCount - 1);

    snprintf(sResultTitle, sizeof(sResultTitle), "%s", what);
    snprintf(sResultText, sizeof(sResultText), "STAGE %d OF %d  %s   %d HITS", stage + 1, gSFRun.gauntletCount,
             SFModGame_LevelName(gSFRun.gauntlet[stage]), gTotalHits);
    printf("[SFGame] %s: %s\n", sResultTitle, sResultText);
    fflush(stdout);
}

int32_t SFModGame_FirstLevel(void) {
    if (gSFRun.mode == SFGAME_GAUNTLET) {
        s32 stage = CLAMP(gSFRun.gauntletIndex, 0, gSFRun.gauntletCount - 1);

        gSFRun.redRoute = (gSFRun.gauntlet[stage] == LEVEL_VENOM_1);
        return gSFRun.gauntlet[stage];
    }
    gSFRun.redRoute = sPracticeMissions[CLAMP(gSFRun.practiceMission, 0, ARRAY_COUNT(sPracticeMissions) - 1)].redRoute;
    return SFModGame_PracticeLevel(gSFRun.practiceMission);
}

bool SFModGame_TakeReturnToSetup(void) {
    bool r = sReturnToSetup;

    sReturnToSetup = false;
    return r;
}

static void SFModGame_ToMenu(void) {
    SEQCMD_STOP_SEQUENCE(SEQ_PLAYER_BGM, 1);
    SEQCMD_STOP_SEQUENCE(SEQ_PLAYER_FANFARE, 1);
    gNextGameState = GSTATE_MENU;
    gOptionMenuStatus = OPTION_WAIT;
    gDrawMode = DRAW_NONE;
    gLastGameState = GSTATE_PLAY;
    gStarCount = 0;
    gCamCount = 1;
    gFillScreenAlphaTarget = 0;
    sReturnToSetup = true;
}

static void SFModGame_GoToLevel(s32 level) {
    gNextGameState = GSTATE_PLAY;
    gNextLevel = level;
    gNextLevelPhase = 0;
    gSavedObjectLoadIndex = 0;
    gSavedPathProgress = 0.0f;
    gSavedHitCount = 0;
    gHitCount = 0;
    D_ctx_8017782C = true;
}

// what the map does after a mission (the gauntlet goes straight to the next planet)
static void SFModGame_SaveMissionResult(void) {
    s32 i;

    for (i = 0; i < TEAM_ID_MAX; i++) {
        gPrevPlanetTeamShields[i] = gTeamShields[i];
        gPrevPlanetSavedTeamShields[i] = gSavedTeamShields[i];
        gSavedTeamShields[i] = gTeamShields[i];
    }
    for (i = TEAM_ID_FALCO; i <= TEAM_ID_PEPPY; i++) {
        if (gPrevPlanetTeamShields[i] == 0) {
            gPrevPlanetTeamShields[i] = 255;
        }
    }
    gSavedGoldRingCount[0] = gGoldRingCount[0];
}

void SFModGame_OnNextGameState(void) {
    bool levelChange;

    if (!gSFRun.active || gVersusMode) {
        return;
    }
    levelChange = (gNextGameState == GSTATE_PLAY) && (gGameState == GSTATE_PLAY) && (gNextLevel != gCurrentLevel);

    // Venom via Area 6: Venom 1 (with its boss) first ...
    if (levelChange && (gCurrentLevel == LEVEL_AREA_6) && (gNextLevel == LEVEL_VENOM_2) &&
        (gSFRun.mode == SFGAME_STORY)) {
        gNextLevel = LEVEL_VENOM_1;
        gNextLevelPhase = 0;
        gSFRun.redRoute = true;
        printf("[SFGame] red route: Area 6 -> Venom 1\n");
    }
    if (levelChange && (gCurrentLevel == LEVEL_BOLSE) && (gNextLevel == LEVEL_VENOM_1) && (gSFRun.mode == SFGAME_STORY)) {
        gSFRun.redRoute = false;
    }
    // ... then Star Wolf (Venom 2), which leads to the real Andross
    if (levelChange && gSFRun.redRoute && (gCurrentLevel == LEVEL_VENOM_1) && (gNextLevel == LEVEL_VENOM_2) &&
        (gNextLevelPhase == 2)) {
        gNextLevelPhase = 0;
        gBgColor = 0;
        // what Venom 1 handed on (see Venom1_80198414): used when the Star Wolf fight starts
        sCarry[0] = D_ctx_80177C9C;
        sCarry[1] = D_ctx_80177C94;
        sCarry[2] = D_ctx_80177CA4;
        sCarry[3] = D_ctx_80177CAC;
        sCarry[4] = D_ctx_80177CB4;
        sCarry[5] = D_ctx_80177CBC;
        sCarry[6] = D_ctx_80177CC4;
        sRedCarry = true;
        printf("[SFGame] red route: Venom 1 -> Star Wolf\n");
    }

    switch (gSFRun.mode) {
        case SFGAME_PRACTICE:
            // one mission: back to the setup screen when it ends (the Venom missions end after Andross)
            if ((gNextGameState == GSTATE_MAP) || (gNextGameState == GSTATE_ENDING) ||
                (gNextGameState == GSTATE_GAME_OVER) ||
                (levelChange && (gNextLevel != LEVEL_VENOM_ANDROSS) &&
                 !((gCurrentLevel == LEVEL_VENOM_1) && (gNextLevel == LEVEL_VENOM_2)))) {
                SFModGame_ClearResult();
                if ((gNextGameState != GSTATE_GAME_OVER) && (gGameState == GSTATE_PLAY)) {
                    snprintf(sResultTitle, sizeof(sResultTitle), "MISSION COMPLETE");
                    snprintf(sResultText, sizeof(sResultText), "%s   %d HITS", SFModGame_LevelName(gCurrentLevel),
                             gHitCount);
                }
                printf("[SFGame] practice mission over\n");
                SFModGame_ToMenu();
            }
            break;

        case SFGAME_GAUNTLET: {
            bool lastStage = gSFRun.gauntletIndex >= gSFRun.gauntletCount - 1;
            bool stageDone = (gNextGameState == GSTATE_MAP) && (gGameState == GSTATE_PLAY);

            if ((gGameState == GSTATE_GAME_OVER) && (gNextGameState == GSTATE_MAP)) {
                // like the story: game over ends the gauntlet (instead of the map, the setup screen shows the result)
                SFModGame_GauntletResult("GAUNTLET OVER");
                SFModGame_ToMenu();
                break;
            }
            // Bolse and Area 6 lead straight to Venom: in the gauntlet that is a finished stage
            if (levelChange && !lastStage && ((gCurrentLevel == LEVEL_BOLSE) || (gCurrentLevel == LEVEL_AREA_6))) {
                stageDone = true;
            } else if (stageDone) {
                SFModGame_SaveMissionResult();
            }
            if (stageDone) {
                if (lastStage) {
                    SFModGame_GauntletResult("GAUNTLET COMPLETE");
                    SFModGame_ToMenu();
                    break;
                }
                gSFRun.gauntletIndex++;
                SFModGame_GoToLevel(gSFRun.gauntlet[gSFRun.gauntletIndex]);
                gSFRun.redRoute = (gSFRun.gauntlet[gSFRun.gauntletIndex] == LEVEL_VENOM_1);
                // the mission tables of the story have room for 7 missions
                gMissionNumber = MIN(gMissionNumber, 5);
                printf("[SFGame] gauntlet stage %d/%d: level %d\n", gSFRun.gauntletIndex + 1, gSFRun.gauntletCount,
                       gNextLevel);
            }
            break;
        }

        default:
            break;
    }
    fflush(stdout);
}

// mission practice and gauntlet rules while playing
static void SFModGame_UpdateRules(void) {
    Player* pl = &gPlayer[0];

    if ((pl->state == PLAYERSTATE_INIT) || (pl->state == PLAYERSTATE_DOWN) || (pl->state == PLAYERSTATE_NEXT)) {
        sPracticeApplied = false;
    }
    // red route: shields, wings, gold rings and hits of Venom 1 go on into the Star Wolf fight
    if (sRedCarry && ((pl->state == PLAYERSTATE_LEVEL_INTRO) || (pl->state == PLAYERSTATE_ACTIVE))) {
        sRedCarry = false;
        if ((gCurrentLevel == LEVEL_VENOM_2) && (gLevelPhase == 0) && (sCarry[0] != 0)) {
            pl->shields = sCarry[0] - 1;
            gGoldRingCount[0] = gSavedGoldRingCount[0] = sCarry[1];
            gHitCount = gSavedHitCount = sCarry[2];
            pl->arwing.rightWingState = sCarry[3];
            pl->arwing.leftWingState = sCarry[4];
            gRightWingHealth[0] = sCarry[5];
            gLeftWingHealth[0] = sCarry[6];
            printf("[SFGame] red route: carried over shields %d, hits %d\n", pl->shields, gHitCount);
            fflush(stdout);
        }
    }
    if (gSFRun.mode == SFGAME_PRACTICE) {
        static const s32 sLasers[] = { LASERS_SINGLE, LASERS_TWIN, LASERS_HYPER };

        // endless tries
        if (gLifeCount[0] < 2) {
            gLifeCount[0] = 2;
        }
        // the chosen upgrades at every try
        if (!sPracticeApplied && ((pl->state == PLAYERSTATE_LEVEL_INTRO) || (pl->state == PLAYERSTATE_ACTIVE))) {
            sPracticeApplied = true;
            gLaserStrength[0] = sLasers[CLAMP(gSFRun.practiceLaser, 0, 2)];
            gBombCount[0] = gSFRun.practiceBombs;
        }
    }
    // gauntlet: in Sector Z every missile has to be stopped
    if (gGreatFoxIntact) {
        sSectorZLost = false;
    } else if ((gSFRun.mode == SFGAME_GAUNTLET) && (gCurrentLevel == LEVEL_SECTOR_Z) && !sSectorZLost &&
               (pl->state == PLAYERSTATE_ACTIVE)) {
        sSectorZLost = true;
        pl->shields = 0;
        Player_ApplyDamage(pl, 0, 10);
        printf("[SFGame] gauntlet: a missile hit the Great Fox - this try is lost\n");
        fflush(stdout);
    }
}

void SFModGame_PauseUpdate(void) {
    if (!gSFRun.active || gVersusMode || (gSFRun.mode == SFGAME_STORY)) {
        return;
    }
    if (gControllerPress[gMainController].button & Z_TRIG) {
        Audio_PlayPauseSfx(0);
        Audio_StopPlayerNoise(0);
        Audio_KillSfxBySource(gPlayer[0].sfxSource);
        gPlayState = PLAY_UPDATE;
        SFModGame_ClearResult();
        if (gSFRun.mode == SFGAME_GAUNTLET) {
            SFModGame_GauntletResult("GAUNTLET ENDED");
        }
        printf("[SFGame] quit from the pause screen\n");
        fflush(stdout);
        SFModGame_ToMenu();
    }
}

static void SFModGame_HudText(s32 y, f32 scale, const char* text) {
    s32 width = (s32) (Graphics_GetSmallTextWidth((char*) text) * scale);

    Graphics_DisplaySmallText((SCREEN_WIDTH - width) / 2, y, scale, scale, (char*) text);
}

void SFModGame_DrawHud(void) {
    char text[48];
    bool paused;

    if (!gSFRun.active || gVersusMode || (gGameState != GSTATE_PLAY) || (gSFRun.mode == SFGAME_STORY)) {
        return;
    }
    paused = (gPlayState == PLAY_PAUSE);
    if (!paused && (gLevelStartStatusScreenTimer == 0)) {
        return;
    }
    gDPSetTextureFilter(gMasterDisp++, G_TF_POINT);
    RCP_SetupDL(&gMasterDisp, SETUPDL_83);
    gDPSetPrimColor(gMasterDisp++, 0, 0, 255, 255, 0, 255);
    if (gSFRun.mode == SFGAME_GAUNTLET) {
        snprintf(text, sizeof(text), "GAUNTLET  STAGE %d OF %d", gSFRun.gauntletIndex + 1, gSFRun.gauntletCount);
    } else {
        snprintf(text, sizeof(text), "MISSION PRACTICE");
    }
    SFModGame_HudText(36, 1.0f, text);
    if (paused) {
        gDPSetPrimColor(gMasterDisp++, 0, 0, 200, 200, 200, 255);
        SFModGame_HudText(226, 0.85f, (gSFRun.mode == SFGAME_GAUNTLET) ? "Z  END THE GAUNTLET" : "Z  END PRACTICE");
    }
    gDPSetTextureFilter(gMasterDisp++, G_TF_BILERP);
}

/* ------------------------------------------------------------------------------------------------
 * Setup screen of the "Main Game" card
 * ------------------------------------------------------------------------------------------------ */

typedef enum {
    GROW_MODE,
    GROW_DIFFICULTY,
    GROW_EXPERT,
    GROW_MISSION,
    GROW_LASER,
    GROW_BOMBS,
    GROW_START,
    GROW_COUNT
} SFGameMenuRow;

static bool sMenuOpen = false;
static s32 sMenuRow;
static s32 sRepeatDir;
static s32 sRepeatTimer;
static s32 sRepeatDirX;
static s32 sRepeatTimerX;
static s32 sStarting;

static bool SFModGame_RowVisible(s32 row) {
    if ((row == GROW_MISSION) || (row == GROW_LASER) || (row == GROW_BOMBS)) {
        return gSFRun.mode == SFGAME_PRACTICE;
    }
    return true;
}

static s32 SFModGame_Axis(s32 stick, u16 buttons, u16 neg, u16 pos, s32* dir, s32* timer) {
    s32 d = 0;

    if ((stick < -30) || (buttons & neg)) {
        d = -1;
    } else if ((stick > 30) || (buttons & pos)) {
        d = 1;
    }
    if (d == 0) {
        *dir = 0;
        return 0;
    }
    if (d != *dir) {
        *dir = d;
        *timer = 12;
        return d;
    }
    if (--(*timer) <= 0) {
        *timer = 4;
        return d;
    }
    return 0;
}

static s32 SFModGame_Wrap(s32 v, s32 lo, s32 hi) {
    return (v < lo) ? hi : ((v > hi) ? lo : v);
}

static void SFModGame_Change(s32 dir) {
    switch (sMenuRow) {
        case GROW_MODE:
            gSFRun.mode = SFModGame_Wrap(gSFRun.mode + dir, 0, SFGAME_MODE_COUNT - 1);
            break;
        case GROW_DIFFICULTY:
            gSFRun.difficulty = SFModGame_Wrap(gSFRun.difficulty + dir, 1, 7);
            break;
        case GROW_EXPERT:
            gSFRun.expert = !gSFRun.expert;
            break;
        case GROW_MISSION:
            gSFRun.practiceMission = SFModGame_Wrap(gSFRun.practiceMission + dir, 0, SFModGame_PracticeMissionCount() - 1);
            break;
        case GROW_LASER:
            gSFRun.practiceLaser = SFModGame_Wrap(gSFRun.practiceLaser + dir, 0, 2);
            break;
        case GROW_BOMBS:
            gSFRun.practiceBombs = SFModGame_Wrap(gSFRun.practiceBombs + dir, 0, 9);
            break;
    }
}

static void SFModGame_LoadSettings(void) {
    gSFRun.mode = CLAMP(CVarGetInteger(CVAR_SFGAME_PREFIX "Mode", SFGAME_STORY), 0, SFGAME_MODE_COUNT - 1);
    gSFRun.difficulty = CLAMP(CVarGetInteger(CVAR_SFGAME_PREFIX "Difficulty", 1), 1, 7);
    gSFRun.practiceMission = CLAMP(CVarGetInteger(CVAR_SFGAME_PREFIX "PracticeMission", 0), 0,
                                   SFModGame_PracticeMissionCount() - 1);
    gSFRun.practiceLaser = CLAMP(CVarGetInteger(CVAR_SFGAME_PREFIX "PracticeLaser", 0), 0, 2);
    gSFRun.practiceBombs = CLAMP(CVarGetInteger(CVAR_SFGAME_PREFIX "PracticeBombs", 3), 0, 9);
    gSFRun.expert = false; // never remembered: expert mode is off unless chosen again
}

static void SFModGame_SaveSettings(void) {
    CVarSetInteger(CVAR_SFGAME_PREFIX "Mode", gSFRun.mode);
    CVarSetInteger(CVAR_SFGAME_PREFIX "Difficulty", gSFRun.difficulty);
    CVarSetInteger(CVAR_SFGAME_PREFIX "PracticeMission", gSFRun.practiceMission);
    CVarSetInteger(CVAR_SFGAME_PREFIX "PracticeLaser", gSFRun.practiceLaser);
    CVarSetInteger(CVAR_SFGAME_PREFIX "PracticeBombs", gSFRun.practiceBombs);
    CVarSave();
}

static void SFModGame_Start(void) {
    SFModGame_SaveSettings();
    SFModGame_ClearResult();
    SFModGame_NewPlaythrough();
    gExpertMode = gSFRun.expert;
    sMenuOpen = false;
    if (gSFRun.mode == SFGAME_STORY) {
        Option_SFMod_StartMainGame();
    } else {
        Option_SFMod_StartLevel(SFModGame_FirstLevel());
    }
}

void SFModGame_MenuUpdate(void) {
    OSContPad* hold = &gControllerHold[gMainController];
    OSContPad* press = &gControllerPress[gMainController];
    s32 dy;
    s32 dx;

    if (!sMenuOpen) {
        SFModGame_LoadSettings();
        sMenuOpen = true;
        sMenuRow = GROW_START;
        sStarting = 0;
        sRepeatDir = sRepeatDirX = 0;
        gStarCount = 800;
        gBlurAlpha = 255;
        return;
    }
    if (sStarting > 0) {
        if (--sStarting == 0) {
            SFModGame_Start();
        }
        return;
    }

    dy = SFModGame_Axis(-press->stick_y, hold->button, U_JPAD, D_JPAD, &sRepeatDir, &sRepeatTimer);
    dx = SFModGame_Axis(press->stick_x, hold->button, L_JPAD, R_JPAD, &sRepeatDirX, &sRepeatTimerX);
    if (dy != 0) {
        do {
            sMenuRow = SFModGame_Wrap(sMenuRow + dy, 0, GROW_COUNT - 1);
        } while (!SFModGame_RowVisible(sMenuRow));
        AUDIO_PLAY_SFX(NA_SE_CURSOR, gDefaultSfxSource, 4);
    }
    if ((dx != 0) && (sMenuRow != GROW_START)) {
        SFModGame_Change(dx);
        AUDIO_PLAY_SFX(NA_SE_ARWING_CURSOR, gDefaultSfxSource, 4);
    }
    if ((press->button & A_BUTTON) && (sMenuRow != GROW_START)) {
        SFModGame_Change(1);
        AUDIO_PLAY_SFX(NA_SE_ARWING_CURSOR, gDefaultSfxSource, 4);
    } else if (press->button & (START_BUTTON | A_BUTTON)) {
        AUDIO_PLAY_SFX(NA_SE_ARWING_DECIDE, gDefaultSfxSource, 4);
        sStarting = 12;
        return;
    }
    if (press->button & B_BUTTON) {
        SFModGame_SaveSettings();
        SFModGame_ClearResult();
        sMenuOpen = false;
        Option_SFMod_ReturnToMainMenu();
    }
}

static void SFModGame_TextColor(u8 r, u8 g, u8 b) {
    RCP_SetupDL(&gMasterDisp, SETUPDL_83);
    gDPSetPrimColor(gMasterDisp++, 0, 0, r, g, b, 255);
}

static void SFModGame_TextCentered(s32 y, f32 scale, const char* text) {
    s32 width = (s32) (Graphics_GetSmallTextWidth((char*) text) * scale);

    Graphics_DisplaySmallText((SCREEN_WIDTH - width) / 2, y, scale, scale, (char*) text);
}

void SFModGame_MenuDraw(void) {
    static const char* sLaserNames[] = { "SINGLE", "TWIN", "HYPER" };
    static const char* sModeHelp[SFGAME_MODE_COUNT] = {
        "THE NORMAL GAME",
        "ALL PLANETS IN A RANDOM ORDER",
        "ANY MISSION - ENDLESS TRIES",
    };
    char text[48];
    s32 row;
    s32 y = 60;
    const s32 labelX = 56;
    const s32 valueX = 168;
    s32 width;

    if (!sMenuOpen) {
        return;
    }
    gDPSetTextureFilter(gMasterDisp++, G_TF_POINT);

    SFModGame_TextColor(255, 255, 255);
    width = Graphics_GetLargeTextWidth("MAIN GAME");
    Graphics_DisplayLargeText((SCREEN_WIDTH - width) / 2, 16, 1.0f, 1.0f, "MAIN GAME");

    // result of the gauntlet or practice mission that just ended
    if (sResultTitle[0] != '\0') {
        SFModGame_TextColor(255, 200, 60);
        SFModGame_TextCentered(36, 0.85f, sResultTitle);
        SFModGame_TextColor(220, 220, 220);
        SFModGame_TextCentered(46, 0.85f, sResultText);
    }

    for (row = 0; row < GROW_COUNT; row++) {
        const char* label = "";
        bool selected = (row == sMenuRow);

        if (!SFModGame_RowVisible(row)) {
            continue;
        }
        text[0] = '\0';
        switch (row) {
            case GROW_MODE:
                label = "MODE";
                snprintf(text, sizeof(text), "%s", sModeNames[gSFRun.mode]);
                break;
            case GROW_DIFFICULTY:
                label = "DIFFICULTY";
                snprintf(text, sizeof(text), "%d %s", gSFRun.difficulty, SFModGame_DifficultyName(gSFRun.difficulty));
                break;
            case GROW_EXPERT:
                label = "EXPERT MODE";
                snprintf(text, sizeof(text), "%s", gSFRun.expert ? "ON" : "OFF");
                break;
            case GROW_MISSION:
                label = "MISSION";
                snprintf(text, sizeof(text), "%s", sPracticeMissions[gSFRun.practiceMission].name);
                break;
            case GROW_LASER:
                label = "LASER";
                snprintf(text, sizeof(text), "%s", sLaserNames[gSFRun.practiceLaser]);
                break;
            case GROW_BOMBS:
                label = "SMART BOMBS";
                snprintf(text, sizeof(text), "%d", gSFRun.practiceBombs);
                break;
            case GROW_START:
                y += 8;
                if (selected) {
                    SFModGame_TextColor(255, 255, 0);
                } else {
                    SFModGame_TextColor(200, 200, 200);
                }
                if (!selected || (gGameFrameCount & 8) || (sStarting > 0)) {
                    SFModGame_TextCentered(y, 1.0f, "START");
                }
                y += 14;
                continue;
        }
        if (selected) {
            SFModGame_TextColor(255, 255, 160);
        } else {
            SFModGame_TextColor(200, 200, 200);
        }
        Graphics_DisplaySmallText(labelX, y, 1.0f, 1.0f, (char*) label);
        if (selected) {
            SFModGame_TextColor(255, 255, 0);
        }
        Graphics_DisplaySmallText(valueX, y, 1.0f, 1.0f, text);
        y += 14;
    }

    SFModGame_TextColor(150, 150, 150);
    SFModGame_TextCentered(y + 10, 0.85f, sModeHelp[gSFRun.mode]);
    if (gSFRun.difficulty == 1) {
        SFModGame_TextCentered(y + 22, 0.85f, "DIFFICULTY 1 IS THE ORIGINAL GAME");
    }
    SFModGame_TextCentered(SCREEN_HEIGHT - 22, 0.75f, "UP DOWN SELECT   LEFT RIGHT CHANGE   START BEGIN   B BACK");

    gDPSetTextureFilter(gMasterDisp++, G_TF_BILERP);
}

/* ------------------------------------------------------------------------------------------------
 * Automated testing (environment variables, never set in normal play)
 *   SFMOD_TEST_GAME=<0 story, 1 gauntlet, 2 practice>   start a playthrough right after booting
 *   SFMOD_TEST_DIFFICULTY=<1-7>  SFMOD_TEST_SEED=<n>  SFMOD_TEST_GAUNTLET_STAGE=<index>
 *   SFMOD_TEST_PRACTICE=<mission index>  SFMOD_TEST_PRACTICE_LASER=<0-2>  SFMOD_TEST_PRACTICE_BOMBS=<n>
 *   story: SFMOD_TEST_LEVEL=<level id> is the first level (default Corneria)
 * ------------------------------------------------------------------------------------------------ */

static s32 SFModGame_EnvInt(const char* name, s32 def, s32 lo, s32 hi) {
    const char* v = getenv(name);

    return (v != NULL) ? CLAMP(atoi(v), lo, hi) : def;
}

bool SFModGame_TestBoot(void) {
    const char* mode = getenv("SFMOD_TEST_GAME");
    s32 level;

    if (mode == NULL) {
        return false;
    }
    gSFRun.mode = CLAMP(atoi(mode), 0, SFGAME_MODE_COUNT - 1);
    gSFRun.difficulty = SFModGame_EnvInt("SFMOD_TEST_DIFFICULTY", 1, 1, 7);
    gSFRun.practiceMission = SFModGame_EnvInt("SFMOD_TEST_PRACTICE", 0, 0, SFModGame_PracticeMissionCount() - 1);
    gSFRun.practiceLaser = SFModGame_EnvInt("SFMOD_TEST_PRACTICE_LASER", 0, 0, 2);
    gSFRun.practiceBombs = SFModGame_EnvInt("SFMOD_TEST_PRACTICE_BOMBS", 3, 0, 9);
    gSFRun.expert = false;
    sSeedOverride = (u32) SFModGame_EnvInt("SFMOD_TEST_SEED", 0, 0, 0x7FFFFFFF);
    SFModGame_ClearResult();
    SFModGame_NewPlaythrough();
    sSeedOverride = 0;
    if (gSFRun.mode == SFGAME_GAUNTLET) {
        gSFRun.gauntletIndex = SFModGame_EnvInt("SFMOD_TEST_GAUNTLET_STAGE", 0, 0, gSFRun.gauntletCount - 1);
    }
    if (gSFRun.mode == SFGAME_STORY) {
        level = SFModGame_EnvInt("SFMOD_TEST_LEVEL", LEVEL_CORNERIA, 0, LEVEL_VERSUS);
    } else {
        level = SFModGame_FirstLevel();
    }
    gExpertMode = false;
    gClearPlayerInfo = true;
    gNextLevel = level;
    gNextLevelPhase = 0;
    gNextGameState = GSTATE_PLAY;
    gSavedObjectLoadIndex = 0;
    gSavedPathProgress = 0.0f;
    D_ctx_8017782C = true;
    gHitCount = 0;
    gTotalHits = 0;
    gMissionNumber = 0;
    printf("[SFGame] test boot: %s, level %d, stage %d\n", sModeNames[gSFRun.mode], level, gSFRun.gauntletIndex);
    fflush(stdout);
    return true;
}
