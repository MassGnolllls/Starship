#include <math.h>
#include <string.h>
#include "SFModAI.h"
#include "SFModVs.h"
#include "SFMod.h"
#include "global.h"
#include <stdio.h>
#include <stdlib.h>

/*
 * The AI looks at the arena (like a player looking at the screen and the radar), decides what to do
 * every few frames (its reaction time) and turns that into controller input. Skill levels change how
 * fast it reacts, how well it aims and leads, and how well it uses the advanced moves; personalities
 * change what it likes to do.
 */

// clang-format off
static const SFAIPersonality sPersonalities[] = {
    //  name         aggr  caut  turbo charge bomb  roll  loop  uturn boost greed
    { "BALANCED",    0.60f, 0.45f, 0.50f, 0.35f, 0.40f, 0.50f, 0.40f, 0.40f, 0.50f, 0.40f },
    { "HOTHEAD",     0.95f, 0.10f, 0.90f, 0.10f, 0.50f, 0.20f, 0.20f, 0.20f, 0.80f, 0.20f },
    { "COWARD",      0.25f, 0.95f, 0.40f, 0.20f, 0.60f, 0.80f, 0.50f, 0.60f, 0.70f, 0.80f },
    { "SNIPER",      0.55f, 0.55f, 0.10f, 0.95f, 0.30f, 0.30f, 0.20f, 0.30f, 0.30f, 0.30f },
    { "BOMBER",      0.70f, 0.40f, 0.50f, 0.40f, 1.00f, 0.30f, 0.30f, 0.30f, 0.50f, 0.90f },
    { "ACROBAT",     0.65f, 0.50f, 0.50f, 0.30f, 0.30f, 1.00f, 1.00f, 0.90f, 0.70f, 0.30f },
    { "HUNTER",      0.85f, 0.30f, 0.60f, 0.50f, 0.50f, 0.40f, 0.60f, 0.70f, 0.90f, 0.20f },
    { "SCAVENGER",   0.45f, 0.60f, 0.50f, 0.30f, 0.70f, 0.40f, 0.30f, 0.40f, 0.60f, 1.00f },
    // an all-rounder; on level 7 it plays as strong as it can (see SFAI_BEST)
    { "BEST",        0.65f, 0.50f, 0.50f, 0.50f, 0.50f, 0.50f, 0.40f, 0.50f, 0.50f, 0.50f },
};
#define SFAI_BEST_INDEX 8
// "BEST" on level 7: the strongest values (rolls only to reflect lasers, so no random rolls)
static const SFAIPersonality sBestMax = {
    "BEST", 0.85f, 0.55f, 0.30f, 1.00f, 0.80f, 0.90f, 0.30f, 0.90f, 0.70f, 0.55f,
};

// per skill level 1..7
static const s32 sReactionFrames[] = { 24, 18, 13, 9, 6, 4, 2 };
static const f32 sAimNoiseDeg[]    = { 9.0f, 6.5f, 4.5f, 3.2f, 2.0f, 1.1f, 0.4f };
static const f32 sLeadFactor[]     = { 0.0f, 0.35f, 0.55f, 0.75f, 0.9f, 1.0f, 1.0f };
static const f32 sManeuverSkill[]  = { 0.05f, 0.15f, 0.30f, 0.45f, 0.65f, 0.85f, 1.0f };
static const f32 sAwareness[]      = { 900.0f, 1300.0f, 1700.0f, 2100.0f, 2600.0f, 3100.0f, 3600.0f };
static const f32 sWander[]         = { 0.30f, 0.20f, 0.12f, 0.06f, 0.03f, 0.01f, 0.0f };
// how often it goes for a charged lock-on shot instead of plain lasers, and how fast it can lock on
static const f32 sChargeUse[]      = { 0.03f, 0.06f, 0.10f, 0.14f, 0.18f, 0.22f, 0.26f };
static const s32 sChargePatience[] = { 30, 45, 60, 80, 100, 130, 160 };
// chance per frame to barrel roll when lasers fly at it (rolling ships reflect lasers)
static const f32 sRollReflex[]     = { 0.0f, 0.0f, 0.01f, 0.03f, 0.06f, 0.12f, 0.25f };
// clang-format on

#define SFAI_ARENA_SIZE 24000.0f
#define SFAI_MAXF(a, b) (((a) > (b)) ? (a) : (b))
#define SFAI_MINF(a, b) (((a) < (b)) ? (a) : (b))
// VS lasers: Player_SetupArwingShot halves the speed in VS, and they live 37 frames
#define SFAI_SHOT_SPEED (400.0f / 3.0f * 0.5f)
#define SFAI_SHOT_LIFE 37.0f
// the game's stick to angle factor (gPlayerTurnStickMod)
#define SFAI_STICK_TO_DEG 0.68f

typedef enum {
    MODE_PATROL,
    MODE_ATTACK,
    MODE_ITEM,
    MODE_FLEE,
} SFAIMode;

typedef enum {
    MANEUVER_NONE,
    MANEUVER_ROLL,
    MANEUVER_LOOP,
    MANEUVER_UTURN,
    MANEUVER_CHARGE,
} SFAIManeuver;

typedef struct SFAIState {
    bool active;
    s32 level; // 0..6
    s32 personality;
    SFAIPersonality p;
    u32 rng;

    s32 thinkTimer;
    s32 modeTimer;   // frames until it may change its mind about what to do
    SFAIMode mode;
    s32 target;      // player index or -1
    s32 itemTarget;  // gItems index or -1
    f32 itemDesire;  // how much it wants that item (SFAI_ChooseItem)
    f32 itemDist;
    s32 approachK;   // the line it flies along into an item that can't be reached straight (-1: none)
    bool onApproach; // on that line
    f32 bombNearest; // own smart bomb in flight: the nearest enemy last frame (detonation timing)
    s32 bombTimer;   // frames until it may press the bomb button again
    bool fleeing;
    Vec3f aimPoint;  // where it wants to fly (world), updated every frame
    Vec3f wander;    // a misjudged offset of the aim point (low levels)
    f32 aimNoiseX;   // aim error of the current decision (degrees)
    f32 aimNoiseY;
    f32 aimDist;
    f32 aimTime;     // frames a laser needs to reach the target
    bool wantFire;
    bool wantTurbo;
    bool wantBoost;
    bool wantBrake;
    s32 bankDir;     // -1 left (Z), 1 right (R), 0 none

    SFAIManeuver maneuver;
    s32 maneuverFrame;
    s32 rollDir;
    s32 tapTimer;
    s32 cooldown;    // frames until the next special maneuver
    s32 rollCooldown;
    s32 reflexRoll;  // frames left of a barrel roll against incoming lasers
    s32 reflexDir;
    u16 prevHold;
    s32 debugTimer;
    bool best;       // the "BEST" personality on level 7: no random hesitation
} SFAIState;

static SFAIState sAI[MAX_PLAYERS];
static bool sAIDebug = false;

static u32 SFAI_Rand(SFAIState* ai) {
    u32 x = ai->rng;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    ai->rng = x;
    return x;
}

static f32 SFAI_RandF(SFAIState* ai) {
    return (SFAI_Rand(ai) & 0xFFFFFF) / (f32) 0x1000000;
}

static f32 SFAI_Clamp(f32 v, f32 lo, f32 hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static f32 SFAI_Wrap180(f32 a) {
    while (a > 180.0f) {
        a -= 360.0f;
    }
    while (a < -180.0f) {
        a += 360.0f;
    }
    return a;
}

static f32 SFAI_WrapArena(f32 d) {
    if (!gVersusMode) {
        return d;
    }
    while (d > SFAI_ARENA_SIZE / 2) {
        d -= SFAI_ARENA_SIZE;
    }
    while (d < -SFAI_ARENA_SIZE / 2) {
        d += SFAI_ARENA_SIZE;
    }
    return d;
}

// displacement from a to b (the VS arenas wrap around)
static void SFAI_Delta(Vec3f* a, Vec3f* b, Vec3f* out) {
    out->x = SFAI_WrapArena(b->x - a->x);
    out->y = b->y - a->y;
    out->z = SFAI_WrapArena(b->z - a->z);
}

static f32 SFAI_Len(Vec3f* v) {
    return sqrtf(SQ(v->x) + SQ(v->y) + SQ(v->z));
}

// heading in degrees of a horizontal direction, using the game's convention (forward = (-sin, -cos))
static f32 SFAI_Heading(f32 x, f32 z) {
    return atan2f(-x, -z) * M_RTOD;
}

static void SFAI_Forward(Player* pl, Vec3f* out) {
    f32 len = SFAI_Len(&pl->vel);

    if (len < 0.01f) {
        f32 yaw = pl->yRot_114 * M_DTOR;
        out->x = -sinf(yaw);
        out->y = 0.0f;
        out->z = -cosf(yaw);
        return;
    }
    out->x = pl->vel.x / len;
    out->y = pl->vel.y / len;
    out->z = pl->vel.z / len;
}

static f32 SFAI_AngleTo(Vec3f* fwd, Vec3f* dir) {
    f32 len = SFAI_Len(dir);
    f32 d;

    if (len < 0.01f) {
        return 0.0f;
    }
    d = (fwd->x * dir->x + fwd->y * dir->y + fwd->z * dir->z) / len;
    d = SFAI_Clamp(d, -1.0f, 1.0f);
    return acosf(d) * M_RTOD;
}

static bool SFAI_BombSlotFree(void);
static void SFAI_ItemAim(SFAIState* ai, s32 me, Vec3f* itemPos, Vec3f* aim);

static bool SFAI_PlayerAlive(s32 i) {
    Player* pl = &gPlayer[i];

    return (pl->state == PLAYERSTATE_ACTIVE) || (pl->state == PLAYERSTATE_U_TURN);
}

int32_t SFAI_PersonalityCount(void) {
    return ARRAY_COUNT(sPersonalities);
}

const SFAIPersonality* SFAI_GetPersonality(int32_t index) {
    if ((index < 0) || (index >= ARRAY_COUNT(sPersonalities))) {
        index = 0;
    }
    return &sPersonalities[index];
}

int32_t SFAI_GetPersonalityIndex(int32_t playerNum) {
    return sAI[playerNum].personality;
}

void SFAI_Reset(int32_t playerNum, int32_t level, int32_t personality, uint32_t seed) {
    SFAIState* ai = &sAI[playerNum];
    s32 i;
    f32* values;

    memset(ai, 0, sizeof(*ai));
    sAIDebug = getenv("SFMOD_DEBUG_AI") != NULL;
    ai->active = true;
    ai->rng = seed | 1;
    ai->level = (s32) SFAI_Clamp(level, 1, 7) - 1;
    if ((personality < 0) || (personality >= ARRAY_COUNT(sPersonalities))) {
        personality = SFAI_Rand(ai) % ARRAY_COUNT(sPersonalities);
    }
    ai->personality = personality;
    ai->p = sPersonalities[personality];

    if (personality == SFAI_BEST_INDEX) {
        // the all-rounder; on the highest level the strongest it can be
        ai->best = (ai->level == 6);
        if (ai->best) {
            ai->p = sBestMax;
        }
    } else {
        // every pilot is a little different, even with the same personality
        values = &ai->p.aggression;
        for (i = 0; i < 10; i++) {
            values[i] = SFAI_Clamp(values[i] + (SFAI_RandF(ai) - 0.5f) * 0.2f, 0.0f, 1.0f);
        }
    }

    ai->target = -1;
    ai->itemTarget = -1;
    ai->approachK = -1;
    ai->bombNearest = 99999.0f;
    ai->thinkTimer = SFAI_Rand(ai) % 8;
    ai->cooldown = 60 + SFAI_Rand(ai) % 60;
}

/* ------------------------------------------------------------------------------------------------
 * Perception / decisions (run every "reaction time" frames)
 * ------------------------------------------------------------------------------------------------ */

static s32 SFAI_ChooseTarget(SFAIState* ai, s32 me) {
    Player* self = &gPlayer[me];
    Vec3f fwd;
    Vec3f d;
    s32 best = -1;
    f32 bestScore = 1e9f;
    s32 i;

    SFAI_Forward(self, &fwd);

    for (i = 0; i < gCamCount; i++) {
        f32 dist;
        f32 angle;
        f32 score;

        if ((i == me) || !SFAI_PlayerAlive(i) || SFModVs_SameTeam(i, me)) {
            continue;
        }
        SFAI_Delta(&self->pos, &gPlayer[i].pos, &d);
        dist = SFAI_Len(&d);
        angle = SFAI_AngleTo(&fwd, &d);
        score = dist + angle * 25.0f;
        // revenge: prefer whoever shot it last
        if (self->attacker == i + 1) {
            score -= 1500.0f * ai->p.aggression;
        }
        // weak targets are attractive for aggressive pilots
        score -= (255 - gPlayer[i].shields) * 6.0f * ai->p.aggression;
        // stick to the current target a bit
        if (i == ai->target) {
            score -= 800.0f;
        }
        if (score < bestScore) {
            bestScore = score;
            best = i;
        }
    }
    return best;
}

// What an item is worth to a pilot (0: nothing). In VS a smart bomb uses up all bombs and a bomb only
// appears while nobody has one, so one bomb is all a pilot needs. A bomb does 80 damage to every enemy
// in its blast; twin lasers have a hit box twice as wide.
static f32 SFAI_ItemValue(s32 pilot, Item* item) {
    Player* pl = &gPlayer[pilot];

    switch (item->obj.id) {
        case OBJ_ITEM_BOMB:
            return (gBombCount[pilot] == 0) ? 1.0f : 0.0f;
        case OBJ_ITEM_LASERS:
            if (gLaserStrength[pilot] == LASERS_SINGLE) {
                return 0.8f;
            }
            return (gLaserStrength[pilot] < LASERS_HYPER) ? 0.4f : 0.0f;
        case OBJ_ITEM_SILVER_RING:
        case OBJ_ITEM_SILVER_STAR:
        case OBJ_ITEM_GOLD_RING:
            return (pl->shields < 200) ? ((255 - pl->shields) / 255.0f) : 0.0f;
        case OBJ_ITEM_WING_REPAIR:
            return ((pl->arwing.rightWingState != WINGSTATE_INTACT) || (pl->arwing.leftWingState != WINGSTATE_INTACT))
                       ? 0.5f
                       : 0.0f;
        default:
            return 0.0f;
    }
}

// The item most worth a detour: its value, how greedy and how good the pilot is, how far away it is.
// Good pilots don't race for an item another pilot will clearly get first.
static s32 SFAI_ChooseItem(SFAIState* ai, s32 me) {
    Player* self = &gPlayer[me];
    f32 levelFrac = ai->level / 6.0f;
    s32 best = -1;
    f32 bestDesire = 0.0f;
    f32 bestDist = 0.0f;
    s32 i;
    s32 j;

    for (i = 0; i < ARRAY_COUNT(gItems); i++) {
        Item* item = &gItems[i];
        Vec3f d;
        f32 value;
        f32 dist;
        f32 desire;
        bool lost = false;

        if ((item->obj.status != OBJ_ACTIVE) || item->collected) {
            continue;
        }
        value = SFAI_ItemValue(me, item);
        if (value <= 0.0f) {
            continue;
        }
        SFAI_Delta(&self->pos, &item->obj.pos, &d);
        dist = SFAI_Len(&d);
        if (ai->level >= 2) {
            for (j = 0; j < gCamCount; j++) {
                Vec3f dj;
                f32 other;

                if ((j == me) || !SFAI_PlayerAlive(j) || (SFAI_ItemValue(j, item) <= 0.0f)) {
                    continue;
                }
                SFAI_Delta(&gPlayer[j].pos, &item->obj.pos, &dj);
                other = SFAI_Len(&dj);
                if ((other < dist * 0.6f) && (dist - other > 1500.0f)) {
                    lost = true;
                }
            }
        }
        if (lost) {
            continue;
        }
        desire = value * (0.4f + ai->p.greed) * (0.5f + 0.5f * levelFrac) * 6000.0f / (dist + 1000.0f);
        if (desire > bestDesire) {
            bestDesire = desire;
            bestDist = dist;
            best = i;
        }
    }
    ai->itemDesire = bestDesire;
    ai->itemDist = bestDist;
    return best;
}

// Is an enemy behind us and aiming at us? Returns its index or -1.
static s32 SFAI_FindThreat(SFAIState* ai, s32 me, f32* threatDist) {
    Player* self = &gPlayer[me];
    Vec3f myFwd;
    s32 i;

    SFAI_Forward(self, &myFwd);
    for (i = 0; i < gCamCount; i++) {
        Vec3f d;
        Vec3f back;
        Vec3f hisFwd;
        f32 dist;

        if ((i == me) || !SFAI_PlayerAlive(i) || SFModVs_SameTeam(i, me)) {
            continue;
        }
        SFAI_Delta(&self->pos, &gPlayer[i].pos, &d);
        dist = SFAI_Len(&d);
        if (dist > sAwareness[ai->level] * (0.6f + ai->p.caution * 0.6f)) {
            continue;
        }
        // behind us?
        if ((myFwd.x * d.x + myFwd.y * d.y + myFwd.z * d.z) > 0.0f) {
            continue;
        }
        // aiming at us?
        SFAI_Forward(&gPlayer[i], &hisFwd);
        back.x = -d.x;
        back.y = -d.y;
        back.z = -d.z;
        if (SFAI_AngleTo(&hisFwd, &back) < 25.0f) {
            *threatDist = dist;
            return i;
        }
    }
    return -1;
}

// Is an enemy laser or charged shot about to hit us? (what a player sees coming)
static bool SFAI_IncomingFire(s32 me) {
    Player* self = &gPlayer[me];
    s32 i;

    for (i = 0; i < ARRAY_COUNT(gPlayerShots); i++) {
        PlayerShot* shot = &gPlayerShots[i];
        Vec3f d;
        f32 dist;
        f32 speed;
        f32 cosAngle;

        if ((shot->obj.status != SHOT_ACTIVE) || (shot->sourceId == me) || (shot->sourceId >= gCamCount) ||
            SFModVs_SameTeam(shot->sourceId, me)) {
            continue;
        }
        if ((shot->obj.id != PLAYERSHOT_SINGLE_LASER) && (shot->obj.id != PLAYERSHOT_TWIN_LASER) &&
            (shot->obj.id != PLAYERSHOT_LOCK_ON)) {
            continue;
        }
        SFAI_Delta(&shot->obj.pos, &self->pos, &d);
        dist = SFAI_Len(&d);
        if ((dist > 1200.0f) || (dist < 1.0f)) {
            continue;
        }
        speed = SFAI_Len(&shot->vel);
        if (speed < 1.0f) {
            continue;
        }
        cosAngle = (shot->vel.x * d.x + shot->vel.y * d.y + shot->vel.z * d.z) / (speed * dist);
        if (cosAngle > 0.985f) { // within ~10 degrees
            return true;
        }
    }
    return false;
}

// Other enemies close to the target (the blast of a charged shot hits them too)
static bool SFAI_TargetHasCompany(s32 target, s32 me) {
    s32 i;

    if (target < 0) {
        return false;
    }
    for (i = 0; i < gCamCount; i++) {
        Vec3f d;

        if ((i == target) || (i == me) || !SFAI_PlayerAlive(i) || SFModVs_SameTeam(i, me)) {
            continue;
        }
        SFAI_Delta(&gPlayer[target].pos, &gPlayer[i].pos, &d);
        if (SFAI_Len(&d) < 400.0f) {
            return true;
        }
    }
    return false;
}

// Our homing shot is on its way: the lock (and the homing) is lost when the target leaves our view,
// so keep the nose on the target until it hits.
static bool SFAI_Guiding(s32 me) {
    PlayerShot* homing = &gPlayerShots[PLAYERSHOT_LOCKON_SLOT(me)];
    PlayerShot* bomb = &gPlayerShots[PLAYERSHOT_BOMB_SLOT]; // a smart bomb fired at a locked target homes in too
    s32 i;

    if (!((homing->obj.status == SHOT_ACTIVE) && (homing->obj.id == PLAYERSHOT_LOCK_ON) && (homing->unk_60 == 0)) &&
        !((bomb->obj.status == SHOT_ACTIVE) && (bomb->obj.id == PLAYERSHOT_LOCK_ON) && (bomb->sourceId == me) &&
          (bomb->unk_60 == 0))) {
        return false;
    }
    for (i = 0; i < gCamCount; i++) {
        if (gVsLockOnTimers[i][me] != 0) {
            return true;
        }
    }
    return false;
}

static void SFAI_StartManeuver(SFAIState* ai, SFAIManeuver maneuver) {
    ai->maneuver = maneuver;
    ai->maneuverFrame = 0;
    ai->rollDir = (SFAI_Rand(ai) & 1) ? 1 : -1;
}

static void SFAI_Decide(SFAIState* ai, s32 me) {
    Player* self = &gPlayer[me];
    f32 threatDist = 0.0f;
    s32 threat;
    f32 skill = sManeuverSkill[ai->level];
    Vec3f d;
    f32 dist = 0.0f;
    bool guiding;

    ai->wantBoost = false;
    ai->wantBrake = false;
    guiding = SFAI_Guiding(me) && (ai->level >= 2);

    // stick to a plan for a while (a target, an item) instead of changing its mind every moment
    if (ai->modeTimer > 0) {
        ai->modeTimer -= sReactionFrames[ai->level];
    }
    if (guiding && (ai->target >= 0) && SFAI_PlayerAlive(ai->target)) {
        // the homing shot is on its way: keep looking at the target
        ai->mode = MODE_ATTACK;
    } else if ((ai->modeTimer <= 0) || (ai->mode == MODE_PATROL) ||
        ((ai->target >= 0) && !SFAI_PlayerAlive(ai->target)) ||
        ((ai->mode == MODE_ITEM) && ((ai->itemTarget < 0) || (gItems[ai->itemTarget].obj.status != OBJ_ACTIVE)))) {
        s32 oldTarget = ai->target;

        ai->target = SFAI_ChooseTarget(ai, me);
        ai->itemTarget = SFAI_ChooseItem(ai, me);
        ai->modeTimer = 45 + (s32) (ai->p.aggression * 75.0f) + (s32) (SFAI_RandF(ai) * 30.0f);
        // low shields make careful pilots run for items or away
        ai->fleeing = (self->shields < (s32) (40 + 80 * ai->p.caution)) && (SFAI_RandF(ai) < ai->p.caution * 0.7f);
        if ((ai->mode == MODE_ATTACK) && (ai->target == oldTarget) && (ai->target >= 0)) {
            ai->mode = MODE_ATTACK; // keep fighting
        } else {
            ai->mode = MODE_PATROL; // decided below
        }
    } else if (ai->target >= 0) {
        // revenge: switch to whoever just shot us
        s32 attacker = gPlayer[me].attacker - 1;

        if ((attacker >= 0) && (attacker < gCamCount) && (attacker != ai->target) && SFAI_PlayerAlive(attacker) &&
            !SFModVs_SameTeam(attacker, me) && (gPlayer[me].hitTimer > 0) && (SFAI_RandF(ai) < ai->p.aggression)) {
            ai->target = attacker;
            ai->mode = MODE_ATTACK;
        }
    }


    // aim error of this decision (like a player who doesn't aim perfectly)
    ai->aimNoiseX = (SFAI_RandF(ai) - 0.5f) * 2.0f * sAimNoiseDeg[ai->level];
    ai->aimNoiseY = (SFAI_RandF(ai) - 0.5f) * 2.0f * sAimNoiseDeg[ai->level];

    // low skill pilots sometimes lose focus
    if (SFAI_RandF(ai) < sWander[ai->level]) {
        ai->wander.x = (SFAI_RandF(ai) - 0.5f) * 5000.0f;
        ai->wander.z = (SFAI_RandF(ai) - 0.5f) * 5000.0f;
    } else {
        ai->wander.x = ai->wander.z = 0.0f;
    }
    ai->wander.y = 0.0f;

    if (ai->target >= 0) {
        SFAI_Delta(&self->pos, &gPlayer[ai->target].pos, &d);
        dist = SFAI_Len(&d);
    }

    // evading: an attacker behind us shoots at us, or somebody locked on
    threat = SFAI_FindThreat(ai, me, &threatDist);
    {
        bool lockedOn = false;
        s32 j;

        for (j = 0; j < gCamCount; j++) {
            if ((j != me) && (gVsLockOnTimers[me][j] != 0)) {
                lockedOn = true;
                threat = j;
            }
        }
        if (!lockedOn && !((threat >= 0) && SFAI_IncomingFire(me))) {
            threat = -1;
        }
    }
    // BEST: somebody close behind that shoots or locked on: a somersault puts it behind the attacker
    if (guiding) {
        threat = -1;
    }
    if (ai->best && (threat >= 0) && (threatDist < 1600.0f) && (ai->maneuver == MANEUVER_NONE) && (ai->cooldown == 0)) {
        SFAI_StartManeuver(ai, MANEUVER_LOOP);
        ai->cooldown = 70;
    }
    if ((threat >= 0) && (ai->maneuver == MANEUVER_NONE) && (ai->cooldown == 0) && !ai->best &&
        (SFAI_RandF(ai) < skill * (0.15f + 0.35f * ai->p.caution))) {
        f32 r = SFAI_RandF(ai) * (ai->p.roll + ai->p.loop + ai->p.uturn + 0.3f);

        if (r < ai->p.roll) {
            SFAI_StartManeuver(ai, MANEUVER_ROLL);
        } else if ((r < ai->p.roll + ai->p.loop) && (threatDist < 1800.0f)) {
            SFAI_StartManeuver(ai, MANEUVER_LOOP);
        } else if (r < ai->p.roll + ai->p.loop + ai->p.uturn) {
            SFAI_StartManeuver(ai, MANEUVER_UTURN);
        } else {
            ai->wantBoost = true; // just run
        }
        ai->cooldown = 75 + (s32) ((1.0f - skill) * 150.0f);
    }

    // items: worth a detour unless a fight is about to pay off (a target close in front)
    ai->itemTarget = SFAI_ChooseItem(ai, me);
    if ((ai->itemTarget >= 0) && !guiding) {
        f32 need = 0.5f;
        bool busy = false;

        if ((ai->mode == MODE_ATTACK) && (ai->target >= 0)) {
            Vec3f fwd;

            SFAI_Forward(self, &fwd);
            busy = (dist < 1400.0f) && (SFAI_AngleTo(&fwd, &d) < 30.0f) && (SFAI_RandF(ai) > ai->p.greed * 0.5f);
        }
        if (ai->fleeing || (ai->target < 0)) {
            need = 0.25f;
        }
        if (ai->mode == MODE_ITEM) {
            need *= 0.6f; // stay on it
        }
        if (!busy && (ai->itemDesire > need)) {
            ai->mode = MODE_ITEM;
            ai->wantBoost = ai->wantBoost || ((ai->itemDist > 1500.0f) && (SFAI_RandF(ai) < 0.3f + 0.7f * ai->p.boost));
            return;
        }
    }
    if (ai->mode == MODE_ITEM) {
        ai->mode = MODE_PATROL; // decided below
    }
    if (ai->target < 0) {
        ai->mode = MODE_PATROL;
        return;
    }
    if (ai->fleeing && (dist < 3000.0f)) {
        ai->mode = MODE_FLEE;
        ai->wantBoost = true;
        return;
    }
    ai->mode = MODE_ATTACK;

    // speed: catch up when the target is ahead and out of laser range, don't overshoot when close behind
    {
        Vec3f fwd;
        f32 angle;

        SFAI_Forward(self, &fwd);
        angle = SFAI_AngleTo(&fwd, &d);
        if ((dist > 1000.0f) && (angle < 60.0f) && (SFAI_RandF(ai) < ai->p.boost + ai->p.aggression * 0.3f)) {
            ai->wantBoost = true;
        } else if ((dist > 2500.0f) && (SFAI_RandF(ai) < ai->p.boost)) {
            ai->wantBoost = true;
        } else if ((dist < 600.0f) && (angle < 40.0f) && (SFAI_RandF(ai) < 0.3f + ai->p.boost * skill)) {
            ai->wantBrake = true;
        }
    }

    // charged shot: it homes in on a locked target. A direct hit does laser damage, its blast also
    // hurts the other pilots close to the target. Lasers while the homing shot flies.
    {
        Vec3f fwd;
        PlayerShot* homing = &gPlayerShots[PLAYERSHOT_LOCKON_SLOT(me)];
        bool slotFree = (homing->obj.status == SHOT_FREE) || (homing->obj.id != PLAYERSHOT_LOCK_ON);
        f32 chance = sChargeUse[ai->level] * (0.5f + ai->p.charge);

        SFAI_Forward(self, &fwd);
        if (ai->best) {
            // a direct hit only does laser damage (the target is then immune to the blast), but the
            // blast hurts everybody else near it, and the homing reaches targets lasers can't
            chance = SFAI_TargetHasCompany(ai->target, me) ? 1.0f : 0.0f;
        }
        if ((gBombCount[me] > 0) && SFAI_BombSlotFree()) {
            // a smart bomb fired at a locked target homes in: the best use of a bomb
            chance = SFAI_MAXF(chance, ai->best ? 1.0f : (0.15f + 0.35f * ai->p.bomb) * (0.5f + 0.5f * ai->level / 6.0f));
        }
        if ((ai->maneuver == MANEUVER_NONE) && slotFree && (dist > 450.0f) && (dist < 9000.0f) &&
            (ai->cooldown == 0) && (SFAI_AngleTo(&fwd, &d) < 75.0f) && (SFAI_RandF(ai) < chance)) {
            SFAI_StartManeuver(ai, MANEUVER_CHARGE);
        }
    }

    // U-turn when the target is far behind us
    {
        Vec3f fwd;

        SFAI_Forward(self, &fwd);
        if ((ai->maneuver == MANEUVER_NONE) && (ai->cooldown == 0) && (SFAI_AngleTo(&fwd, &d) > 120.0f) &&
            (dist > 700.0f) && !guiding && (ai->best || (SFAI_RandF(ai) < skill * (0.5f + ai->p.uturn)))) {
            SFAI_StartManeuver(ai, MANEUVER_UTURN);
            ai->cooldown = 90;
        }
    }

    ai->wantTurbo = SFAI_RandF(ai) < ai->p.turbo;
}

// Time a laser needs to hit a target at relative position d moving with v (-1: can't catch it).
static f32 SFAI_InterceptTime(Vec3f* d, Vec3f* v, f32 speed) {
    f32 a = SQ(v->x) + SQ(v->y) + SQ(v->z) - SQ(speed);
    f32 b = 2.0f * (d->x * v->x + d->y * v->y + d->z * v->z);
    f32 c = SQ(d->x) + SQ(d->y) + SQ(d->z);
    f32 disc;
    f32 t1;
    f32 t2;

    if (fabsf(a) < 0.001f) {
        return (b < 0.0f) ? (-c / b) : -1.0f;
    }
    disc = SQ(b) - 4.0f * a * c;
    if (disc < 0.0f) {
        return -1.0f;
    }
    disc = sqrtf(disc);
    t1 = (-b - disc) / (2.0f * a);
    t2 = (-b + disc) / (2.0f * a);
    if ((t1 > 0.0f) && ((t1 < t2) || (t2 <= 0.0f))) {
        return t1;
    }
    return (t2 > 0.0f) ? t2 : -1.0f;
}

// Where to fly this frame
static void SFAI_UpdateAim(SFAIState* ai, s32 me) {
    Player* self = &gPlayer[me];
    Vec3f d;

    ai->aimDist = 99999.0f;
    ai->aimTime = 999.0f;

    if ((ai->mode == MODE_ATTACK) || (ai->mode == MODE_FLEE)) {
        if ((ai->target < 0) || !SFAI_PlayerAlive(ai->target)) {
            ai->mode = MODE_PATROL;
            ai->thinkTimer = SFAI_MINF(ai->thinkTimer, 2);
        }
    }
    if ((ai->mode == MODE_ITEM) &&
        ((ai->itemTarget < 0) || (gItems[ai->itemTarget].obj.status != OBJ_ACTIVE))) {
        ai->mode = MODE_PATROL;
        ai->thinkTimer = SFAI_MINF(ai->thinkTimer, 2);
    }

    switch (ai->mode) {
        case MODE_ATTACK: {
            Player* tgt = &gPlayer[ai->target];
            f32 t;
            f32 lead = sLeadFactor[ai->level];

            SFAI_Delta(&self->pos, &tgt->pos, &d);
            ai->aimDist = SFAI_Len(&d);
            t = SFAI_InterceptTime(&d, &tgt->vel, SFAI_SHOT_SPEED);
            ai->aimTime = (t > 0.0f) ? t : 999.0f;
            if (t <= 0.0f) {
                t = ai->aimDist / SFAI_SHOT_SPEED;
            }
            t = SFAI_MINF(t, 60.0f);
            ai->aimPoint.x = self->pos.x + d.x + tgt->vel.x * t * lead;
            ai->aimPoint.y = self->pos.y + d.y + tgt->vel.y * t * lead;
            ai->aimPoint.z = self->pos.z + d.z + tgt->vel.z * t * lead;
            break;
        }

        case MODE_FLEE:
            SFAI_Delta(&self->pos, &gPlayer[ai->target].pos, &d);
            ai->aimPoint.x = self->pos.x - d.x;
            ai->aimPoint.y = self->pos.y + 200.0f;
            ai->aimPoint.z = self->pos.z - d.z;
            break;

        case MODE_ITEM:
            SFAI_ItemAim(ai, me, &gItems[ai->itemTarget].obj.pos, &ai->aimPoint);
            break;

        case MODE_PATROL:
        default:
            ai->aimPoint.x = 0.0f;
            ai->aimPoint.y = SFAI_MAXF(gGroundHeight, self->pathFloor) + 350.0f;
            ai->aimPoint.z = 0.0f;
            break;
    }

    if (ai->mode != MODE_ITEM) {
        ai->aimPoint.x += ai->wander.x;
        ai->aimPoint.z += ai->wander.z;
    }
}

/* ------------------------------------------------------------------------------------------------
 * Flying (every frame)
 * ------------------------------------------------------------------------------------------------ */

// The boxes things collide with (the scenery's hitboxes in world space). Scenery without hitboxes
// (the VS pyramids, which use polygon collision) is one box as wide as it is at the height y.
typedef struct {
    f32 cx, cy, cz; // center (world)
    f32 hx, hy, hz; // half sizes (in the box's own directions)
    f32 c, s;       // the box's yaw: local (x, z) = (wx * c - wz * s, wx * s + wz * c)
} SFAIBox;

#define SFAI_MAX_BOXES 8

static s32 SFAI_SceneryBoxes(Scenery360* scn, SFAIBox* out, f32 y) {
    f32* data = scn->info.hitbox;
    f32 rot = scn->obj.rot.y * M_DTOR;
    f32 c = cosf(rot);
    f32 sn = sinf(rot);
    s32 count;
    s32 n = 0;
    s32 i;

    if ((data == NULL) || ((s32) data[0] <= 0)) {
        // polygon collision: the pyramids get narrower towards the top (sizes of their models)
        f32 half = 450.0f;
        f32 height = 1200.0f;
        f32 up;

        if (scn->obj.id == OBJ_SCENERY_VS_PYRAMID_1) {
            half = 498.0f;
            height = 705.0f;
        } else if (scn->obj.id == OBJ_SCENERY_VS_PYRAMID_2) {
            half = 623.0f;
            height = 200.0f;
        }
        up = SFAI_Clamp((y - 50.0f - scn->obj.pos.y) / height, 0.0f, 1.0f);
        out[0].cx = scn->obj.pos.x;
        out[0].cy = scn->obj.pos.y + height * 0.5f;
        out[0].cz = scn->obj.pos.z;
        out[0].hx = out[0].hz = half * (1.0f - up) + 30.0f;
        out[0].hy = height * 0.5f;
        out[0].c = c;
        out[0].s = sn;
        return 1;
    }
    count = (s32) data[0];
    data++;
    for (i = 0; (i < count) && (n < SFAI_MAX_BOXES); i++) {
        bool rotated = false;
        f32 ox;
        f32 oz;
        SFAIBox* b = &out[n];

        if (data[0] == HITBOX_ROTATED) {
            rotated = true;
            data += 4;
        } else if (data[0] >= HITBOX_SHADOW) {
            data++;
        }
        // z offset/size, y offset/size, x offset/size
        ox = data[4];
        oz = data[0];
        b->cx = scn->obj.pos.x + ox * c + oz * sn;
        b->cz = scn->obj.pos.z - ox * sn + oz * c;
        b->cy = scn->obj.pos.y + data[2];
        b->hy = data[3];
        b->c = c;
        b->s = sn;
        if (rotated) {
            // a box with its own rotation: a cube around it
            b->hx = b->hz = SFAI_MAXF(SFAI_MAXF(data[5], data[1]), data[3]) * 1.42f;
            b->hy = b->hx;
            b->c = 1.0f;
            b->s = 0.0f;
        } else {
            b->hx = data[5];
            b->hz = data[1];
        }
        data += 6;
        n++;
    }
    return n;
}

// Where along the segment p -> p + d (0..1) it enters the rectangle (XZ), or -1
static f32 SFAI_SegmentRect(f32 px, f32 pz, f32 dx, f32 dz, f32 minX, f32 maxX, f32 minZ, f32 maxZ) {
    f32 t0 = 0.0f;
    f32 t1 = 1.0f;
    f32 p[2] = { px, pz };
    f32 d[2] = { dx, dz };
    f32 lo[2] = { minX, minZ };
    f32 hi[2] = { maxX, maxZ };
    s32 k;

    for (k = 0; k < 2; k++) {
        if (fabsf(d[k]) < 0.0001f) {
            if ((p[k] < lo[k]) || (p[k] > hi[k])) {
                return -1.0f;
            }
        } else {
            f32 a = (lo[k] - p[k]) / d[k];
            f32 b = (hi[k] - p[k]) / d[k];

            if (a > b) {
                f32 tmp = a;

                a = b;
                b = tmp;
            }
            t0 = SFAI_MAXF(t0, a);
            t1 = SFAI_MINF(t1, b);
            if (t0 > t1) {
                return -1.0f;
            }
        }
    }
    return t0;
}

// Where the segment p -> p + d (XZ) enters the box grown by margin (0..1), or -1
static f32 SFAI_SegmentBox(SFAIBox* b, f32 px, f32 pz, f32 dx, f32 dz, f32 margin) {
    f32 wx = SFAI_WrapArena(px - b->cx); // the box as seen from p (the arena wraps)
    f32 wz = SFAI_WrapArena(pz - b->cz);
    f32 lx = wx * b->c - wz * b->s;
    f32 lz = wx * b->s + wz * b->c;
    f32 ldx = dx * b->c - dz * b->s;
    f32 ldz = dx * b->s + dz * b->c;

    return SFAI_SegmentRect(lx, lz, ldx, ldz, -b->hx - margin, b->hx + margin, -b->hz - margin, b->hz + margin);
}

// Is the straight way from a (at height y) by the vector d blocked by scenery? margin: room to keep.
static bool SFAI_PathBlocked(Vec3f* a, Vec3f* d, f32 y, f32 margin) {
    s32 i;

    if (gScenery360 == NULL) {
        return false;
    }
    for (i = 0; i < 200; i++) {
        Scenery360* scn = &gScenery360[i];
        SFAIBox boxes[SFAI_MAX_BOXES];
        s32 n;
        s32 j;

        if ((scn->obj.status != OBJ_ACTIVE) || (scn->obj.id == OBJ_SCENERY_LEVEL_OBJECTS)) {
            continue;
        }
        n = SFAI_SceneryBoxes(scn, boxes, y);
        for (j = 0; j < n; j++) {
            SFAIBox* b = &boxes[j];

            if ((y > b->cy + b->hy + 40.0f) || (y < b->cy - b->hy - 40.0f)) {
                continue; // above or below it
            }
            if (SFAI_SegmentBox(b, a->x, a->z, d->x, d->z, margin) >= 0.0f) {
                return true;
            }
        }
    }
    return false;
}

// Avoid the ground, the ceiling and the scenery: adjusts the wanted direction. Only what is in the way
// before the aim point counts, so it flies between buildings and under arches to an item there.
static void SFAI_AvoidObstacles(Player* self, Vec3f* dir, f32 aimLen, bool lowOk, bool* climb, bool* dive) {
    Vec3f fwd;
    f32 floorY = SFAI_MAXF(gGroundHeight, self->pathFloor);
    f32 fx;
    f32 fz;
    f32 flen;
    f32 look = SFAI_MINF(1800.0f, aimLen + 150.0f);
    f32 dirLen = SFAI_Len(dir);
    s32 i;

    SFAI_Forward(self, &fwd);

    if (self->pos.y < floorY + (lowOk ? 40.0f : 210.0f)) {
        *climb = true;
    }

    if (gScenery360 == NULL) {
        return;
    }
    flen = sqrtf(SQ(fwd.x) + SQ(fwd.z));
    if (flen < 0.01f) {
        return;
    }
    fx = fwd.x / flen;
    fz = fwd.z / flen;
    for (i = 0; i < 200; i++) {
        Scenery360* scn = &gScenery360[i];
        SFAIBox boxes[SFAI_MAX_BOXES];
        s32 n;
        s32 j;

        if ((scn->obj.status != OBJ_ACTIVE) || (scn->obj.id == OBJ_SCENERY_LEVEL_OBJECTS)) {
            continue; // the VS arenas' item spawn points: invisible, nothing to fly into
        }
        n = SFAI_SceneryBoxes(scn, boxes, self->pos.y);
        for (j = 0; j < n; j++) {
            SFAIBox* b = &boxes[j];
            f32 bx = self->pos.x + SFAI_WrapArena(b->cx - self->pos.x);
            f32 bz = self->pos.z + SFAI_WrapArena(b->cz - self->pos.z);
            const f32 margin = 70.0f;
            f32 t;
            f32 dist;
            f32 side;
            f32 push;
            f32 strength;

            if ((self->pos.y > b->cy + b->hy + 40.0f) || (self->pos.y < b->cy - b->hy - 40.0f)) {
                continue; // flies above or below it
            }
            t = SFAI_SegmentBox(b, self->pos.x, self->pos.z, fx * look, fz * look, margin);
            if (t < 0.0f) {
                continue;
            }
            dist = t * look;
            // steer away from the box's center, harder the closer it is
            side = fx * (bz - self->pos.z) - fz * (bx - self->pos.x);
            push = (side > 0.0f) ? -1.0f : 1.0f;
            strength = 0.6f + 2.0f * (1.0f - dist / look);
            dir->x += -fz * push * strength * dirLen;
            dir->z += fx * push * strength * dirLen;
            if (dist < 900.0f) {
                // over it or under it, if there is room
                f32 top = b->cy + b->hy + 80.0f;
                f32 bottom = b->cy - b->hy - 80.0f;
                bool over = top < self->pathHeight - 80.0f;
                bool under = bottom > floorY + (lowOk ? 70.0f : 140.0f);

                if (under && (!over || (self->pos.y - bottom < top - self->pos.y))) {
                    *dive = true;
                } else if (over || !under) {
                    *climb = true;
                }
            }
        }
    }
}

// The way to an item: straight if nothing is in the way, otherwise along a clear line into it (an item
// under an arch is reached by flying through the arches). It flies onto the line like a car onto a road:
// at a point ahead on the line, farther ahead the farther away it is, so it joins it smoothly.
#define SFAI_APPROACH_LEN 3000.0f

// An item that lies in a piece of scenery (Sector Z's space junk) is picked up from beside it: the
// point just outside the scenery (an item is picked up within ~120 of it).
static void SFAI_ItemPickupPoint(Vec3f* item, Vec3f* out) {
    s32 i;

    *out = *item;
    if (gScenery360 == NULL) {
        return;
    }
    for (i = 0; i < 200; i++) {
        Scenery360* scn = &gScenery360[i];
        SFAIBox boxes[SFAI_MAX_BOXES];
        s32 n;
        s32 j;

        if ((scn->obj.status != OBJ_ACTIVE) || (scn->obj.id == OBJ_SCENERY_LEVEL_OBJECTS)) {
            continue;
        }
        n = SFAI_SceneryBoxes(scn, boxes, out->y);
        for (j = 0; j < n; j++) {
            SFAIBox* b = &boxes[j];
            f32 wx = SFAI_WrapArena(out->x - b->cx);
            f32 wz = SFAI_WrapArena(out->z - b->cz);
            f32 dx = wx * b->c - wz * b->s; // in the box's frame
            f32 dy = out->y - b->cy;
            f32 dz = wx * b->s + wz * b->c;
            f32 ex;
            f32 ey;
            f32 ez;

            if ((fabsf(dx) > b->hx + 50.0f) || (fabsf(dy) > b->hy + 40.0f) || (fabsf(dz) > b->hz + 50.0f)) {
                continue;
            }
            // leave it by the shortest way (sideways rather than up or down)
            ex = b->hx + 80.0f - fabsf(dx);
            ey = b->hy + 60.0f - fabsf(dy) + 40.0f;
            ez = b->hz + 80.0f - fabsf(dz);
            if ((ex <= ey) && (ex <= ez)) {
                dx += (dx >= 0.0f) ? ex : -ex;
            } else if (ez <= ey) {
                dz += (dz >= 0.0f) ? ez : -ez;
            } else {
                out->y += (dy >= 0.0f) ? (ey - 40.0f) : -(ey - 40.0f);
            }
            // back to the world: (wx, wz) = (dx * c + dz * s, -dx * s + dz * c)
            out->x = b->cx + dx * b->c + dz * b->s;
            out->z = b->cz - dx * b->s + dz * b->c;
        }
    }
}

static void SFAI_ItemAim(SFAIState* ai, s32 me, Vec3f* itemObjPos, Vec3f* aim) {
    Player* self = &gPlayer[me];
    Vec3f pickup;
    Vec3f* itemPos = &pickup;
    Vec3f d;
    Vec3f fwd;
    f32 bestCost = 1e9f;
    s32 bestK = -1;
    f32 bestAlong = 0.0f;
    f32 bestPerp = 0.0f;
    f32 flen;
    s32 k;

    ai->onApproach = false;
    SFAI_ItemPickupPoint(itemObjPos, &pickup);
    SFAI_Delta(&self->pos, itemPos, &d);
    if (!SFAI_PathBlocked(&self->pos, &d, itemPos->y, 60.0f)) {
        *aim = *itemPos;
        ai->approachK = -1;
        return;
    }
    SFAI_Forward(self, &fwd);
    flen = sqrtf(SQ(fwd.x) + SQ(fwd.z));
    if (flen > 0.01f) {
        fwd.x /= flen;
        fwd.z /= flen;
    }
    for (k = 0; k < 8; k++) {
        f32 ang = k * 45.0f * M_DTOR;
        f32 ux = -sinf(ang); // into the item
        f32 uz = -cosf(ang);
        Vec3f start;
        Vec3f in;
        Vec3f rel;
        f32 along;
        f32 perp;
        f32 cost;

        start.x = itemPos->x - ux * SFAI_APPROACH_LEN;
        start.y = itemPos->y;
        start.z = itemPos->z - uz * SFAI_APPROACH_LEN;
        in.x = ux * SFAI_APPROACH_LEN;
        in.y = 0.0f;
        in.z = uz * SFAI_APPROACH_LEN;
        if (SFAI_PathBlocked(&start, &in, itemPos->y, 60.0f)) {
            continue;
        }
        SFAI_Delta(&start, &self->pos, &rel);
        along = rel.x * ux + rel.z * uz;
        perp = fabsf(rel.x * uz - rel.z * ux);
        // close to the line, with room to join it before the item, already heading along it
        cost = perp + SFAI_MAXF(-along, 0.0f) * 0.5f + (1.0f - (fwd.x * ux + fwd.z * uz)) * 800.0f;
        if (along > 1800.0f) {
            cost += 3000.0f + (along - 1800.0f); // too close to the item to join the line: go back out
        }
        if (k != ai->approachK) {
            cost += 500.0f; // keep the line it already flies to
        }
        if (cost < bestCost) {
            bestCost = cost;
            bestK = k;
            bestAlong = along;
            bestPerp = perp;
        }
    }
    if (bestK < 0) {
        *aim = *itemPos; // no clear line: try anyway
        ai->approachK = -1;
        return;
    }
    ai->approachK = bestK;
    {
        f32 ang = bestK * 45.0f * M_DTOR;
        f32 ux = -sinf(ang);
        f32 uz = -cosf(ang);
        f32 at;

        if ((bestPerp < 900.0f) && (bestAlong < 1800.0f + bestPerp)) {
            // join the line at a point ahead on it, farther ahead the farther off it is
            at = SFAI_MINF(bestAlong + SFAI_MAXF(800.0f, bestPerp * 1.5f), SFAI_APPROACH_LEN + 600.0f);
            at = SFAI_MAXF(at, 0.0f);
        } else {
            at = 0.0f; // to the start of the line
        }
        aim->x = itemPos->x - ux * (SFAI_APPROACH_LEN - at);
        aim->y = itemPos->y;
        aim->z = itemPos->z - uz * (SFAI_APPROACH_LEN - at);
        ai->onApproach = (bestPerp < 150.0f) && (bestAlong > -300.0f) && (bestAlong < SFAI_APPROACH_LEN + 200.0f);
    }
}

/* ------------------------------------------------------------------------------------------------
 * Smart bombs. In VS a bomb flies 180 units a frame along the nose and explodes when it hits a pilot,
 * when its time is up (37 frames) or when its owner presses the bomb button again (from its 13th frame
 * on). The blast grows to a radius of 1800 and does 80 damage to every enemy in it. Fired while locked
 * on, the bomb homes in on the locked target. Only one bomb can fly at a time.
 * ------------------------------------------------------------------------------------------------ */

#define SFAI_BOMB_SPEED 180.0f
#define SFAI_BOMB_DETONATE_AFTER 13.0f // frames (the game takes the button from shot->timer < 25 on)

static bool SFAI_BombSlotFree(void) {
    return gPlayerShots[PLAYERSHOT_BOMB_SLOT].obj.status == SHOT_FREE;
}

// Would a bomb fired now along the nose catch somebody? It is set off next to the target, which works
// from the bomb's 13th frame on (~2400 away). (At 180 units a frame it mostly flies through a ship
// without hitting it, so it isn't aimed for a direct hit.) Never into scenery.
static bool SFAI_BombShotGood(SFAIState* ai, s32 me, Vec3f* fwd) {
    Player* self = &gPlayer[me];
    f32 levelFrac = ai->level / 6.0f;
    s32 j;

    for (j = 0; j < gCamCount; j++) {
        Vec3f d;
        Vec3f path;
        f32 t;
        f32 along;
        f32 px;
        f32 py;
        f32 pz;
        f32 perp;
        bool good;

        if ((j == me) || !SFAI_PlayerAlive(j) || SFModVs_SameTeam(j, me)) {
            continue;
        }
        SFAI_Delta(&self->pos, &gPlayer[j].pos, &d);
        t = SFAI_Len(&d) / SFAI_BOMB_SPEED;
        d.x += gPlayer[j].vel.x * t;
        d.y += gPlayer[j].vel.y * t;
        d.z += gPlayer[j].vel.z * t;
        along = d.x * fwd->x + d.y * fwd->y + d.z * fwd->z;
        if (along < 400.0f) {
            continue;
        }
        px = d.x - fwd->x * along;
        py = d.y - fwd->y * along;
        pz = d.z - fwd->z * along;
        perp = sqrtf(SQ(px) + SQ(py) + SQ(pz));
        // worse pilots take worse chances
        good = (along >= SFAI_BOMB_SPEED * (SFAI_BOMB_DETONATE_AFTER + 1.0f)) && (along < 4200.0f) &&
               (perp < 450.0f + 400.0f * (1.0f - levelFrac));
        if (!good) {
            continue;
        }
        path.x = fwd->x * along;
        path.y = 0.0f;
        path.z = fwd->z * along;
        if (!SFAI_PathBlocked(&self->pos, &path, self->pos.y, 0.0f)) {
            return true;
        }
    }
    return false;
}

static void SFAI_Bombs(SFAIState* ai, s32 me, u16* buttons, Vec3f* fwd) {
    PlayerShot* bomb = &gPlayerShots[PLAYERSHOT_BOMB_SLOT];
    f32 levelFrac = ai->level / 6.0f;
    u16 bombButton = gBombButton[me];
    s32 j;

    if (ai->bombTimer > 0) {
        ai->bombTimer--;
        return; // let go of the button between two presses
    }

    // our bomb flies: set it off where it catches the most (the blast keeps growing for a while,
    // so an enemy up to ~1100 away from it is caught)
    if ((bomb->obj.status == SHOT_ACTIVE) && (bomb->obj.id == PLAYERSHOT_BOMB) && (bomb->sourceId == me) &&
        (bomb->unk_5C == 0)) {
        f32 nearest = 99999.0f;

        for (j = 0; j < gCamCount; j++) {
            Vec3f d;
            f32 dist;

            if ((j == me) || !SFAI_PlayerAlive(j) || SFModVs_SameTeam(j, me)) {
                continue;
            }
            SFAI_Delta(&bomb->obj.pos, &gPlayer[j].pos, &d);
            dist = SFAI_Len(&d);
            if (dist < nearest) {
                nearest = dist;
            }
        }
        if ((bomb->timer < 25) && (ai->level >= 2) &&
            ((nearest < 450.0f) || ((nearest < 1100.0f) && (nearest > ai->bombNearest + 5.0f)))) {
            *buttons |= bombButton;
            ai->bombTimer = 4;
            if (sAIDebug) {
                printf("[SFAI] %d sets off its bomb, nearest enemy %.0f\n", me + 1, nearest);
                fflush(stdout);
            }
        }
        ai->bombNearest = nearest;
        return;
    }
    ai->bombNearest = 99999.0f;

    if ((gBombCount[me] <= 0) || !SFAI_BombSlotFree() || (ai->maneuver == MANEUVER_CHARGE)) {
        return;
    }

    // locked on (a charged shot found somebody): the bomb homes in on that pilot
    for (j = 0; j < gCamCount; j++) {
        if ((j != me) && !SFModVs_SameTeam(j, me) && (gVsLockOnTimers[j][me] != 0)) {
            if (ai->best || (SFAI_RandF(ai) < (0.2f + 0.3f * ai->p.bomb) * (0.4f + 0.6f * levelFrac))) {
                *buttons |= bombButton;
                ai->bombTimer = 4;
            }
            return;
        }
    }

    // without a lock: straight at a target in front, or close to it and set it off
    if ((ai->mode == MODE_ATTACK) && (ai->target >= 0) && SFAI_BombShotGood(ai, me, fwd) &&
        (ai->best || (SFAI_RandF(ai) < (0.03f + 0.12f * ai->p.bomb) * (0.4f + 0.6f * levelFrac)))) {
        *buttons |= bombButton;
        ai->bombTimer = 4;
        if (sAIDebug) {
            printf("[SFAI] %d fires a bomb without lock, target %d dist %.0f\n", me + 1, ai->target + 1, ai->aimDist);
            fflush(stdout);
        }
    }
}

void SFAI_Think(int32_t playerNum, uint16_t* hold, int8_t* stickX, int8_t* stickY) {
    SFAIState* ai = &sAI[playerNum];
    Player* self = &gPlayer[playerNum];
    u16 buttons = 0;
    f32 sx;
    f32 sy; // the game's pitch input (positive = nose down), before the invert setting
    Vec3f dir;
    Vec3f fwd;
    f32 headingWanted;
    f32 yawErr;
    f32 velYawErr;
    f32 pitchWanted;
    f32 pitchNow;
    f32 horiz;
    f32 floorY;
    f32 rotYWanted;
    bool climb = false;
    bool dive = false;
    bool lowOk;
    bool highOk;
    f32 aimLen;
    f32 rawHeading;
    f32 invert = (CVarGetInteger("gInvertYAxis", 0) == 1) ? -1.0f : 1.0f;

    *hold = 0;
    *stickX = 0;
    *stickY = 0;

    if (!ai->active || (self->state != PLAYERSTATE_ACTIVE && self->state != PLAYERSTATE_U_TURN)) {
        ai->maneuver = MANEUVER_NONE;
        ai->prevHold = 0;
        ai->thinkTimer = 0;
        return;
    }

    if (ai->cooldown > 0) {
        ai->cooldown--;
    }
    if (ai->rollCooldown > 0) {
        ai->rollCooldown--;
    }
    if (ai->thinkTimer-- <= 0) {
        ai->thinkTimer = sReactionFrames[ai->level];
        SFAI_Decide(ai, playerNum);
    }
    SFAI_UpdateAim(ai, playerNum);

    // where to fly
    SFAI_Delta(&self->pos, &ai->aimPoint, &dir);
    aimLen = SFAI_Len(&dir);
    rawHeading = SFAI_Heading(dir.x, dir.z);
    floorY = SFAI_MAXF(gGroundHeight, self->pathFloor);
    // items lie low (or high): fly down (up) to them
    lowOk = (ai->mode == MODE_ITEM) && (ai->aimPoint.y < floorY + 250.0f) && (aimLen < 3500.0f);
    highOk = (ai->mode == MODE_ITEM) && (ai->aimPoint.y > self->pathHeight - 200.0f) && (aimLen < 3500.0f);
    SFAI_AvoidObstacles(self, &dir, (ai->mode == MODE_ITEM) && ai->onApproach ? SFAI_MINF(aimLen, 700.0f) : aimLen,
                        lowOk, &climb, &dive);
    SFAI_Forward(self, &fwd);

    headingWanted = SFAI_Heading(dir.x, dir.z) + ((ai->mode == MODE_ITEM) ? 0.0f : ai->aimNoiseX);
    horiz = sqrtf(SQ(dir.x) + SQ(dir.z));
    pitchWanted = atan2f(dir.y, horiz) * M_RTOD + ((ai->mode == MODE_ITEM) ? 0.0f : ai->aimNoiseY);

    // keep inside the arena's height (VS arenas are flat: the ceiling is only ~650 above the floor)
    if (dive && !climb) {
        pitchWanted = SFAI_MINF(pitchWanted, -20.0f);
    }
    if (climb || (self->pos.y < floorY + (lowOk ? 60.0f : 110.0f))) {
        pitchWanted = SFAI_MAXF(pitchWanted, climb ? 30.0f : 15.0f);
    }
    if (self->pos.y > self->pathHeight - (highOk ? 50.0f : 110.0f)) {
        pitchWanted = SFAI_MINF(pitchWanted, -5.0f);
    }
    pitchWanted = SFAI_Clamp(pitchWanted, -38.0f, 38.0f);
    pitchNow = asinf(SFAI_Clamp(fwd.y, -1.0f, 1.0f)) * M_RTOD;

    // The ship turns its nose (yRot_114) at a rate set by rot.y, and flies rot.y off its nose. Asking for
    // rot.y = (wanted heading - nose) makes it fly straight at the target while the nose catches up.
    yawErr = SFAI_Wrap180(headingWanted - self->yRot_114);
    rotYWanted = SFAI_Clamp(yawErr, -40.8f, 40.8f);
    sx = SFAI_Clamp(-rotYWanted / SFAI_STICK_TO_DEG, -60.0f, 60.0f);
    // the climb angle simply follows the stick
    sy = SFAI_Clamp(-pitchWanted / SFAI_STICK_TO_DEG, -60.0f, 60.0f);

    // Turn fights: both ships turn at the same rate, so whoever is slower turns the tighter circle and
    // gets the nose on the other one first. Good pilots brake in a turn fight.
    if ((ai->mode == MODE_ATTACK) && (ai->aimDist < 1600.0f) && (fabsf(yawErr) > 55.0f) && (ai->maneuver == MANEUVER_NONE) &&
        (ai->level >= 2) && (SFAI_RandF(ai) < 0.35f + 0.6f * sManeuverSkill[ai->level])) {
        ai->wantBrake = true;
        ai->wantBoost = false;
    } else if ((ai->mode == MODE_ATTACK) && (fabsf(yawErr) < 30.0f)) {
        ai->wantBrake = ai->wantBrake && (ai->aimDist < 600.0f);
    }

    // an item close by but off to the side: brake to turn tighter instead of circling around it
    if ((ai->mode == MODE_ITEM) && (ai->maneuver == MANEUVER_NONE) && (ai->level >= 1)) {
        if ((aimLen < 1300.0f) && (fabsf(yawErr) > 35.0f)) {
            ai->wantBrake = true;
            ai->wantBoost = false;
        } else if (aimLen < 700.0f) {
            ai->wantBoost = false;
        }
    }

    // long turns: tilt into the turn like a good player (turns twice as fast)
    if ((fabsf(yawErr) > 35.0f) && ((ai->level >= 3) || ((ai->level >= 1) && (SFAI_RandF(ai) < 0.05f)))) {
        ai->bankDir = (yawErr > 0.0f) ? -1 : 1;
    } else if (fabsf(yawErr) < 15.0f) {
        ai->bankDir = 0;
    }

    // fire when the guns point where it thinks the target will be
    ai->wantFire = false;
    velYawErr = SFAI_Wrap180(headingWanted - SFAI_Heading(fwd.x, fwd.z));
    if ((ai->mode == MODE_ATTACK) && (ai->aimTime < SFAI_SHOT_LIFE * 0.95f)) {
        f32 err = sqrtf(SQ(velYawErr) + SQ(pitchWanted - pitchNow));
        f32 allowed = atan2f(60.0f + 90.0f * ai->p.aggression, SFAI_MAXF(ai->aimDist, 1.0f)) * M_RTOD + 1.5f;

        ai->wantFire = (err < allowed) || ((ai->aimDist < 400.0f) && (err < 25.0f));
    }

    if (ai->wantBoost && !self->boostCooldown) {
        buttons |= gBoostButton[playerNum];
    } else if (ai->wantBrake && !self->boostCooldown) {
        buttons |= gBrakeButton[playerNum];
    }
    if (ai->bankDir < 0) {
        buttons |= Z_TRIG;
    } else if (ai->bankDir > 0) {
        buttons |= R_TRIG;
    }

    // a barrel roll reflects lasers: skilled pilots roll when lasers come at them (also while charging)
    if ((ai->reflexRoll == 0) && (ai->rollCooldown == 0) && (ai->maneuver != MANEUVER_LOOP) &&
        (ai->maneuver != MANEUVER_UTURN) && (ai->maneuver != MANEUVER_ROLL) && (sRollReflex[ai->level] > 0.0f) &&
        SFAI_IncomingFire(playerNum) &&
        (ai->best || (SFAI_RandF(ai) < sRollReflex[ai->level] * (0.4f + 0.6f * ai->p.roll)))) {
        ai->reflexRoll = 22;
        ai->reflexDir = (SFAI_Rand(ai) & 1) ? 1 : -1;
        ai->rollCooldown = 30;
    }

    // special maneuvers override the normal flying
    switch (ai->maneuver) {
        case MANEUVER_ROLL:
            // double tap Z or R
            buttons &= ~(Z_TRIG | R_TRIG);
            if ((ai->maneuverFrame < 2) || ((ai->maneuverFrame >= 4) && (ai->maneuverFrame < 6))) {
                buttons |= (ai->rollDir < 0) ? Z_TRIG : R_TRIG;
            }
            if (ai->maneuverFrame > 20) {
                ai->maneuver = MANEUVER_NONE;
            }
            break;

        case MANEUVER_LOOP:
            // pull back, then boost
            sx = 0.0f;
            buttons &= ~(gBoostButton[playerNum] | gBrakeButton[playerNum] | Z_TRIG | R_TRIG);
            if ((ai->maneuverFrame >= 2) && (ai->maneuverFrame < 4)) {
                buttons |= gBoostButton[playerNum];
            }
            if (((ai->maneuverFrame > 6) && !self->somersault) || (ai->maneuverFrame > 90)) {
                ai->maneuver = MANEUVER_NONE;
            }
            break;

        case MANEUVER_UTURN:
            // pull back, then brake
            sx = 0.0f;
            buttons &= ~(gBoostButton[playerNum] | gBrakeButton[playerNum] | Z_TRIG | R_TRIG);
            if ((ai->maneuverFrame >= 2) && (ai->maneuverFrame < 4)) {
                buttons |= gBrakeButton[playerNum];
            }
            if (((ai->maneuverFrame > 6) && (self->state != PLAYERSTATE_U_TURN)) || (ai->maneuverFrame > 90)) {
                ai->maneuver = MANEUVER_NONE;
            }
            break;

        case MANEUVER_CHARGE: {
            // hold fire to charge: the ship then sends out lock-on searches along its nose. Once one
            // finds the target, let go and press again to fire the homing shot (or a smart bomb, which
            // also homes in on a locked target).
            bool locked = (ai->target >= 0) && (gVsLockOnTimers[ai->target][playerNum] != 0);
            Vec3f tdir;
            f32 tangle = 180.0f;

            ai->wantFire = false;
            if (ai->target >= 0) {
                SFAI_Delta(&self->pos, &gPlayer[ai->target].pos, &tdir);
                tangle = SFAI_AngleTo(&fwd, &tdir);
            }
            if (ai->rollDir != 2) {
                if (locked) {
                    ai->rollDir = 2; // let go, fire in two frames
                    ai->maneuverFrame = 100;
                } else if ((ai->maneuverFrame > sChargePatience[ai->level]) || (ai->target < 0) || (tangle > 110.0f) ||
                           (ai->mode != MODE_ATTACK) ||
                           ((ai->level >= 3) && (ai->aimDist < 1100.0f) && (gBombCount[playerNum] == 0))) {
                    // (in laser range lasers do the same damage without waiting for a lock)
                    // no lock: fire the charged shot anyway only when it points at a close target
                    if ((ai->maneuverFrame > 21) && (tangle < 5.0f) && (ai->aimDist < 1500.0f)) {
                        ai->rollDir = 2;
                        ai->maneuverFrame = 100;
                    } else {
                        ai->maneuver = MANEUVER_NONE;
                        ai->cooldown = SFAI_MAXF(ai->cooldown, 10);
                        break;
                    }
                }
            }
            if (ai->maneuverFrame < 100) {
                buttons |= A_BUTTON;
            } else if (ai->maneuverFrame == 102) {
                if (locked && (gBombCount[playerNum] > 0) && SFAI_BombSlotFree() &&
                    (ai->best || (SFAI_RandF(ai) < (0.4f + 0.6f * ai->p.bomb) * (0.6f + 0.4f * ai->level / 6.0f)))) {
                    buttons |= gBombButton[playerNum]; // the homing smart bomb
                    ai->bombTimer = 4;
                } else {
                    buttons |= A_BUTTON;
                }
                if (sAIDebug) {
                    printf("[SFAI] %d charged shot: locked %d target %d dist %.0f angle %.0f charge %d\n", playerNum + 1,
                           locked, ai->target + 1, ai->aimDist, tangle, gChargeTimers[playerNum]);
                    fflush(stdout);
                }
            } else if (ai->maneuverFrame > 104) {
                ai->maneuver = MANEUVER_NONE;
                ai->cooldown = SFAI_MAXF(ai->cooldown, 6);
            }
            break;
        }

        default:
            break;
    }
    if (ai->maneuver != MANEUVER_NONE) {
        ai->maneuverFrame++;
    }
    if (ai->reflexRoll > 0) {
        // double tap Z or R
        s32 f = 22 - ai->reflexRoll;

        buttons &= ~(Z_TRIG | R_TRIG);
        if ((f < 2) || ((f >= 4) && (f < 6))) {
            buttons |= (ai->reflexDir < 0) ? Z_TRIG : R_TRIG;
        }
        ai->reflexRoll--;
    }

    if (ai->wantFire && (ai->maneuver != MANEUVER_CHARGE)) {
        if (ai->wantTurbo) {
            buttons |= SFMOD_BTN_TURBO;
        } else {
            // tap the fire button like a player would
            ai->tapTimer = (ai->tapTimer + 1) % (3 + (6 - ai->level) / 2);
            if (ai->tapTimer == 0) {
                buttons |= A_BUTTON;
            }
        }
    }

    SFAI_Bombs(ai, playerNum, &buttons, &fwd);

    *hold = buttons;
    *stickX = (s8) SFAI_Clamp(sx, -60.0f, 60.0f);
    if ((ai->maneuver == MANEUVER_LOOP) || (ai->maneuver == MANEUVER_UTURN)) {
        *stickY = -60; // these moves look at the stick without the invert setting: pulled back
    } else {
        *stickY = (s8) SFAI_Clamp(sy * invert, -60.0f, 60.0f);
    }
    ai->prevHold = buttons;

    if (sAIDebug && (++ai->debugTimer >= 30)) {
        ai->debugTimer = 0;
        printf("[SFAI] %d mode %d tgt %d dist %.0f t %.0f yawErr %.1f pitch %.1f/%.1f y %.0f (%.0f..%.0f) fire %d man %d "
               "stick %d %d btn %04x item %d desire %.2f idist %.0f aim %.0f\n",
               playerNum + 1, ai->mode, ai->target + 1, ai->aimDist, ai->aimTime, yawErr, pitchWanted, pitchNow,
               self->pos.y, floorY, self->pathHeight, ai->wantFire, ai->maneuver, *stickX, *stickY, buttons,
               ai->itemTarget, ai->itemDesire, ai->itemDist, aimLen);
        printf("[SFAI] %d avoid %.1f pos %.0f %.0f %.0f climb %d\n", playerNum + 1,
               SFAI_Wrap180(SFAI_Heading(dir.x, dir.z) - rawHeading), self->pos.x, self->pos.y, self->pos.z, climb);
        fflush(stdout);
    }
}

const SFAIPersonality* SFAI_EffectivePersonality(int32_t index, int32_t level) {
    if ((index == SFAI_BEST_INDEX) && (level >= 7)) {
        return &sBestMax;
    }
    return SFAI_GetPersonality(index);
}

/* ------------------------------------------------------------------------------------------------
 * Story dogfights: all-range actors (Star Wolf, enemy fighters, the team)
 *
 * The actor code flies towards a destination (fwork[4..6]) with a speed (fwork[1]) and a turn rate
 * (fwork[3]) and shoots when iwork[0] is set. The AI sets those like it would move a stick: it leads
 * its target, fires when its guns point where the target will be, and evades attacks with rolls,
 * somersaults, U-turns and breaks. Skill changes reaction time, aim, lead, turn rate and how well it
 * evades; the personality changes what it likes to do.
 * ------------------------------------------------------------------------------------------------ */

// actor lasers: 100 units per frame, 30 frames
#define SFAI_ACTOR_SHOT_SPEED 100.0f
#define SFAI_ACTOR_SHOT_LIFE 30.0f

// clang-format off
static const f32 sActorTurnRate[] = { 1.2f, 1.3f, 1.42f, 1.55f, 1.68f, 1.82f, 1.95f };
static const s32 sActorFireGap[]  = { 14, 12, 10, 9, 7, 6, 5 };
static const f32 sActorLead[]     = { 0.0f, 0.25f, 0.45f, 0.6f, 0.75f, 0.9f, 1.0f };
static const f32 sActorNoise[]    = { 9.0f, 7.0f, 5.5f, 4.2f, 3.0f, 2.0f, 1.2f };
// the original pilots weave around their target's position; skilled ones less
static const f32 sActorWeave[]    = { 120.0f, 100.0f, 85.0f, 70.0f, 50.0f, 30.0f, 15.0f };
// clang-format on

typedef struct SFActorAIState {
    s32 lastFrame;
    s32 think;
    f32 noiseYaw;
    f32 noisePitch;
    s32 fireTimer;
    s32 evadeCooldown;
    u32 rng;
} SFActorAIState;

static SFActorAIState sActorAI[ARRAY_COUNT(gActors)];

static u32 SFAI_ActorRand(SFActorAIState* st) {
    u32 x = st->rng;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    st->rng = x;
    return x;
}

static f32 SFAI_ActorRandF(SFActorAIState* st) {
    return (SFAI_ActorRand(st) & 0xFFFFFF) / (f32) 0x1000000;
}

static void SFAI_ActorForward(Actor* a, Vec3f* out) {
    f32 rx = a->obj.rot.x * M_DTOR;
    f32 ry = a->obj.rot.y * M_DTOR;

    out->x = sinf(ry) * cosf(rx);
    out->y = -sinf(rx);
    out->z = cosf(ry) * cosf(rx);
}

// Is this all-range actor on Fox's side?
static bool SFAI_ActorIsAlly(s32 aiType) {
    return ((aiType >= AI360_FALCO) && (aiType <= AI360_PEPPY)) || (aiType == AI360_KATT) || (aiType == AI360_BILL);
}

// Can this laser hurt the actor? Fox and the team shoot at Star Wolf and the enemies, they shoot back.
static bool SFAI_HostileShot(PlayerShot* shot, bool ally) {
    s32 src;

    if (shot->sourceId == 0) {
        return !ally; // Fox's lasers
    }
    src = shot->sourceId - NPC_SHOT_ID;
    if ((src <= 0) || (src >= ARRAY_COUNT(gActors)) || (gActors[src].obj.id != OBJ_ACTOR_ALLRANGE)) {
        return false;
    }
    return SFAI_ActorIsAlly(gActors[src].aiType) != ally;
}

// lasers that will hit (what a pilot sees coming)
static bool SFAI_ActorIncomingFire(Actor* a, bool ally) {
    s32 i;

    for (i = 0; i < ARRAY_COUNT(gPlayerShots); i++) {
        PlayerShot* shot = &gPlayerShots[i];
        Vec3f d;
        f32 dist;
        f32 speed;

        if ((shot->obj.status != SHOT_ACTIVE) || !SFAI_HostileShot(shot, ally) ||
            ((shot->obj.id != PLAYERSHOT_SINGLE_LASER) && (shot->obj.id != PLAYERSHOT_TWIN_LASER))) {
            continue;
        }
        d.x = a->obj.pos.x - shot->obj.pos.x;
        d.y = a->obj.pos.y - shot->obj.pos.y;
        d.z = a->obj.pos.z - shot->obj.pos.z;
        dist = SFAI_Len(&d);
        if ((dist > 1400.0f) || (dist < 1.0f)) {
            continue;
        }
        speed = SFAI_Len(&shot->vel);
        if ((speed > 1.0f) && (((shot->vel.x * d.x + shot->vel.y * d.y + shot->vel.z * d.z) / (speed * dist)) > 0.99f)) {
            return true;
        }
    }
    return false;
}

// A pursuer of the actor: Fox or an all-range actor of the other side, close behind it and aiming at
// it. Returns its distance (0: none).
static f32 SFAI_ActorChaser(Actor* a, bool ally, const Vec3f* fwd, f32 awareness) {
    f32 best = 0.0f;
    s32 i;

    // Fox
    if (!ally && (gPlayer[0].state == PLAYERSTATE_ACTIVE)) {
        Vec3f toFox;
        Vec3f back;
        Vec3f foxFwd = gPlayer[0].vel;
        f32 foxDist;

        toFox.x = gPlayer[0].pos.x - a->obj.pos.x;
        toFox.y = gPlayer[0].pos.y - a->obj.pos.y;
        toFox.z = gPlayer[0].trueZpos - a->obj.pos.z;
        foxDist = SFAI_Len(&toFox);
        back.x = -toFox.x;
        back.y = -toFox.y;
        back.z = -toFox.z;
        if ((foxDist < awareness) && ((fwd->x * toFox.x + fwd->y * toFox.y + fwd->z * toFox.z) < 0.0f) &&
            (SFAI_Len(&foxFwd) > 1.0f) && (SFAI_AngleTo(&foxFwd, &back) < 20.0f)) {
            best = foxDist;
        }
    }
    // the other side's pilots
    for (i = 1; i < AI360_GREAT_FOX; i++) {
        Actor* o = &gActors[i];
        Vec3f to;
        Vec3f back;
        Vec3f oFwd;
        f32 dist;

        if ((o->obj.status != OBJ_ACTIVE) || (o->obj.id != OBJ_ACTOR_ALLRANGE) || (o == a) ||
            (SFAI_ActorIsAlly(o->aiType) == ally) || (o->aiType >= AI360_GREAT_FOX) || (o->state == SF360_STATE_RETREAT)) {
            continue;
        }
        to.x = o->obj.pos.x - a->obj.pos.x;
        to.y = o->obj.pos.y - a->obj.pos.y;
        to.z = o->obj.pos.z - a->obj.pos.z;
        dist = SFAI_Len(&to);
        if ((dist >= awareness) || ((fwd->x * to.x + fwd->y * to.y + fwd->z * to.z) >= 0.0f)) {
            continue;
        }
        back.x = -to.x;
        back.y = -to.y;
        back.z = -to.z;
        SFAI_ActorForward(o, &oFwd);
        if ((SFAI_AngleTo(&oFwd, &back) < 20.0f) && ((best == 0.0f) || (dist < best))) {
            best = dist;
        }
    }
    return best;
}

void SFAI_ActorThink(void* actorPtr, int32_t level, int32_t personality, bool evade, bool mayFire, bool ally,
                     SFAIActorOrder* order) {
    Actor* a = (Actor*) actorPtr;
    s32 idx = a - gActors;
    SFActorAIState* st;
    const SFAIPersonality* p;
    s32 sk = (s32) SFAI_Clamp(level, 1, 7) - 1;
    bool best;
    Vec3f tpos;
    Vec3f tvel;
    Vec3f d;
    Vec3f fwd;
    Vec3f aim;
    f32 dist;
    f32 t;
    f32 angle;
    f32 allowed;

    order->fire = false;
    order->move = SFAI_MOVE_NONE;
    if ((idx < 0) || (idx >= ARRAY_COUNT(gActors))) {
        return;
    }
    st = &sActorAI[idx];
    if ((st->lastFrame != gGameFrameCount) && (st->lastFrame != gGameFrameCount - 1)) {
        // a new pilot (or one that wasn't flying a moment ago)
        memset(st, 0, sizeof(*st));
        st->rng = (u32) (idx * 2654435761u) ^ (u32) gGameFrameCount ^ 0x5bd1e995u;
        st->rng |= 1;
        st->evadeCooldown = 60;
        sAIDebug = getenv("SFMOD_DEBUG_AI") != NULL;
    }
    st->lastFrame = gGameFrameCount;
    if (personality < 0) {
        personality = 0;
    }
    p = SFAI_EffectivePersonality(personality, sk + 1);
    best = (personality == SFAI_BEST_INDEX) && (sk == 6);

    // the target
    if (a->aiIndex == AI360_FOX) {
        if ((gPlayer[0].state != PLAYERSTATE_ACTIVE) && (gPlayer[0].state != PLAYERSTATE_U_TURN)) {
            return;
        }
        tpos.x = gPlayer[0].pos.x;
        tpos.y = gPlayer[0].pos.y;
        tpos.z = gPlayer[0].trueZpos;
        tvel = gPlayer[0].vel;
    } else if ((a->aiIndex > 0) && (a->aiIndex < ARRAY_COUNT(gActors)) &&
               (gActors[a->aiIndex].obj.status == OBJ_ACTIVE)) {
        tpos = gActors[a->aiIndex].obj.pos;
        tvel = gActors[a->aiIndex].vel;
    } else {
        return;
    }

    if (st->fireTimer > 0) {
        st->fireTimer--;
    }
    if (st->evadeCooldown > 0) {
        st->evadeCooldown--;
    }
    if (st->think-- <= 0) {
        st->think = sReactionFrames[sk];
        st->noiseYaw = (SFAI_ActorRandF(st) - 0.5f) * 2.0f * sActorNoise[sk];
        st->noisePitch = (SFAI_ActorRandF(st) - 0.5f) * 2.0f * sActorNoise[sk];
    }

    // lead the target like a pilot who knows where it will be when the laser gets there
    d.x = tpos.x - a->obj.pos.x;
    d.y = tpos.y - a->obj.pos.y;
    d.z = tpos.z - a->obj.pos.z;
    dist = SFAI_Len(&d);
    t = SFAI_InterceptTime(&d, &tvel, SFAI_ACTOR_SHOT_SPEED);
    if (t <= 0.0f) {
        t = dist / SFAI_ACTOR_SHOT_SPEED;
    }
    t = SFAI_MINF(t, 45.0f);
    aim.x = d.x + tvel.x * t * sActorLead[sk] + SIN_DEG((idx * 45) + gGameFrameCount) * sActorWeave[sk];
    aim.y = d.y + tvel.y * t * sActorLead[sk] + COS_DEG((idx * 45) + (gGameFrameCount * 2)) * sActorWeave[sk];
    aim.z = d.z + tvel.z * t * sActorLead[sk] + SIN_DEG((idx * 45) + gGameFrameCount) * sActorWeave[sk];
    {
        // aim error of this decision: turn the aim direction a little
        f32 cy = cosf(st->noiseYaw * M_DTOR);
        f32 sy = sinf(st->noiseYaw * M_DTOR);
        f32 x = aim.x * cy + aim.z * sy;
        f32 z = -aim.x * sy + aim.z * cy;

        aim.x = x;
        aim.z = z;
        aim.y += SFAI_Len(&aim) * sinf(st->noisePitch * M_DTOR);
    }
    a->fwork[4] = a->obj.pos.x + aim.x;
    a->fwork[5] = a->obj.pos.y + aim.y;
    a->fwork[6] = a->obj.pos.z + aim.z;

    // turns like a good pilot (never worse than the original)
    a->fwork[3] = SFAI_MAXF(a->fwork[3], sActorTurnRate[sk]);

    SFAI_ActorForward(a, &fwd);
    angle = SFAI_AngleTo(&fwd, &aim);

    // speed: close in on a target far ahead
    if ((sk >= 3) && (dist > 2200.0f) && (angle < 50.0f)) {
        a->fwork[1] = SFAI_MAXF(a->fwork[1], 50.0f + 12.0f * p->boost);
    }

    // shoot when the guns point at where the target will be
    allowed = atan2f(40.0f + 100.0f * p->aggression, SFAI_MAXF(dist, 1.0f)) * M_RTOD + (6 - sk) * 0.8f;
    if (mayFire && (st->fireTimer == 0) && (t < SFAI_ACTOR_SHOT_LIFE * 0.95f) && (angle < allowed)) {
        order->fire = true;
        st->fireTimer = sActorFireGap[sk];
    }

    // evade: lasers coming in, or somebody of the other side behind and aiming (Fox, the team, Star Wolf ...)
    if (evade && (st->evadeCooldown == 0) && (gPlayer[0].state == PLAYERSTATE_ACTIVE)) {
        bool incoming = (sk >= 2) && SFAI_ActorIncomingFire(a, ally);
        f32 foxDist = SFAI_ActorChaser(a, ally, &fwd, sAwareness[sk]); // (the pursuer, Fox or not)
        bool chased = foxDist > 0.0f;

        if (incoming || chased) {
            f32 chance = sManeuverSkill[sk] * (0.15f + 0.45f * p->caution);

            if (sAIDebug) {
                printf("[SFAI] actor %d (ai %d) threatened: incoming %d chased %d dist %.0f\n", idx, a->aiType, incoming,
                       chased, foxDist);
                fflush(stdout);
            }

            if (best) {
                chance = 1.0f;
            }
            if (SFAI_ActorRandF(st) < chance) {
                if (incoming && (best || (SFAI_ActorRandF(st) < 0.3f + 0.7f * p->roll))) {
                    order->move = SFAI_MOVE_ROLL;
                } else if (chased && (foxDist < 1500.0f) &&
                           (best || (SFAI_ActorRandF(st) < p->loop / (p->loop + p->uturn + 0.3f)))) {
                    order->move = SFAI_MOVE_LOOP;
                } else if (chased && (best || (SFAI_ActorRandF(st) < p->uturn))) {
                    order->move = SFAI_MOVE_UTURN;
                } else {
                    order->move = SFAI_MOVE_BREAK;
                }
                st->evadeCooldown = 70 + (s32) ((1.0f - sManeuverSkill[sk]) * 140.0f);
            } else {
                st->evadeCooldown = sReactionFrames[sk];
            }
        }
    }
}
