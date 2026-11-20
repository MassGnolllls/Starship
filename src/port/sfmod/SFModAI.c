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
};

// per skill level 1..7
static const s32 sReactionFrames[] = { 24, 18, 13, 9, 6, 4, 2 };
static const f32 sAimNoiseDeg[]    = { 9.0f, 6.5f, 4.5f, 3.2f, 2.0f, 1.1f, 0.4f };
static const f32 sLeadFactor[]     = { 0.0f, 0.35f, 0.55f, 0.75f, 0.9f, 1.0f, 1.0f };
static const f32 sManeuverSkill[]  = { 0.05f, 0.15f, 0.30f, 0.45f, 0.65f, 0.85f, 1.0f };
static const f32 sAwareness[]      = { 900.0f, 1300.0f, 1700.0f, 2100.0f, 2600.0f, 3100.0f, 3600.0f };
static const f32 sWander[]         = { 0.30f, 0.20f, 0.12f, 0.06f, 0.03f, 0.01f, 0.0f };
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
    u16 prevHold;
    s32 debugTimer;
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

    // every pilot is a little different, even with the same personality
    values = &ai->p.aggression;
    for (i = 0; i < 10; i++) {
        values[i] = SFAI_Clamp(values[i] + (SFAI_RandF(ai) - 0.5f) * 0.2f, 0.0f, 1.0f);
    }

    ai->target = -1;
    ai->itemTarget = -1;
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

static s32 SFAI_ChooseItem(SFAIState* ai, s32 me) {
    Player* self = &gPlayer[me];
    Vec3f d;
    s32 best = -1;
    f32 bestDist = 1200.0f + 2300.0f * ai->p.greed;
    s32 i;

    for (i = 0; i < ARRAY_COUNT(gItems); i++) {
        Item* item = &gItems[i];
        bool wanted = false;
        f32 dist;

        if (item->obj.status != OBJ_ACTIVE) {
            continue;
        }
        switch (item->obj.id) {
            case OBJ_ITEM_LASERS:
                wanted = gLaserStrength[me] < LASERS_HYPER;
                break;
            case OBJ_ITEM_BOMB:
                wanted = gBombCount[me] < 9;
                break;
            case OBJ_ITEM_SILVER_RING:
            case OBJ_ITEM_SILVER_STAR:
            case OBJ_ITEM_GOLD_RING:
                wanted = self->shields < (s32) (140.0f + 100.0f * ai->p.greed);
                break;
            case OBJ_ITEM_WING_REPAIR:
                wanted = (self->arwing.rightWingState != WINGSTATE_INTACT) ||
                         (self->arwing.leftWingState != WINGSTATE_INTACT);
                break;
            default:
                break;
        }
        if (!wanted) {
            continue;
        }
        SFAI_Delta(&self->pos, &item->obj.pos, &d);
        dist = SFAI_Len(&d);
        if (dist < bestDist) {
            bestDist = dist;
            best = i;
        }
    }
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

    ai->wantBoost = false;
    ai->wantBrake = false;

    // stick to a plan for a while (a target, an item) instead of changing its mind every moment
    if (ai->modeTimer > 0) {
        ai->modeTimer -= sReactionFrames[ai->level];
    }
    if ((ai->modeTimer <= 0) || (ai->mode == MODE_PATROL) ||
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
    if ((threat >= 0) && (ai->maneuver == MANEUVER_NONE) && (ai->cooldown == 0) &&
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

    if ((ai->mode != MODE_ATTACK) && (ai->itemTarget >= 0) &&
        (ai->fleeing || (ai->mode == MODE_ITEM) || (SFAI_RandF(ai) < ai->p.greed * 0.35f) || (ai->target < 0))) {
        ai->mode = MODE_ITEM;
        ai->wantBoost = ai->wantBoost || (SFAI_RandF(ai) < ai->p.boost * 0.5f);
        return;
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

    // charged shot (it homes in and does four times the damage of a laser) from medium range
    {
        Vec3f fwd;

        SFAI_Forward(self, &fwd);
        if ((ai->maneuver == MANEUVER_NONE) && (ai->level >= 1) && (dist > 900.0f) && (dist < 7000.0f) &&
            (ai->cooldown == 0) && (SFAI_AngleTo(&fwd, &d) < 30.0f) &&
            (SFAI_RandF(ai) < (0.2f + ai->p.charge * 0.5f) * sManeuverSkill[ai->level] + 0.1f)) {
            SFAI_StartManeuver(ai, MANEUVER_CHARGE);
            ai->cooldown = 40;
        }
    }

    // U-turn when the target is far behind us
    {
        Vec3f fwd;

        SFAI_Forward(self, &fwd);
        if ((ai->maneuver == MANEUVER_NONE) && (ai->cooldown == 0) && (SFAI_AngleTo(&fwd, &d) > 120.0f) &&
            (dist > 700.0f) && (SFAI_RandF(ai) < skill * (0.5f + ai->p.uturn))) {
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
            ai->aimPoint = gItems[ai->itemTarget].obj.pos;
            break;

        case MODE_PATROL:
        default:
            ai->aimPoint.x = 0.0f;
            ai->aimPoint.y = SFAI_MAXF(gGroundHeight, self->pathFloor) + 350.0f;
            ai->aimPoint.z = 0.0f;
            break;
    }

    ai->aimPoint.x += ai->wander.x;
    ai->aimPoint.z += ai->wander.z;
}

/* ------------------------------------------------------------------------------------------------
 * Flying (every frame)
 * ------------------------------------------------------------------------------------------------ */

// Avoid the ground, the ceiling and buildings: adjusts the wanted direction.
static void SFAI_AvoidObstacles(Player* self, Vec3f* dir, bool* climb) {
    Vec3f fwd;
    s32 i;

    SFAI_Forward(self, &fwd);

    if (self->pos.y < gGroundHeight + 250.0f) {
        *climb = true;
    }

    if (gScenery360 == NULL) {
        return;
    }
    for (i = 0; i < 200; i++) {
        Scenery360* scn = &gScenery360[i];
        Vec3f d;
        f32 ahead;
        f32 side;
        f32 height;
        f32 radius;

        if (scn->obj.status != OBJ_ACTIVE) {
            continue;
        }
        SFAI_Delta(&self->pos, &scn->obj.pos, &d);
        ahead = fwd.x * d.x + fwd.z * d.z;
        if ((ahead < 0.0f) || (ahead > 1800.0f)) {
            continue;
        }
        side = fwd.x * d.z - fwd.z * d.x;
        radius = 450.0f;
        height = 1200.0f;
        if ((scn->info.hitbox != NULL) && ((s32) scn->info.hitbox[0] > 0)) {
            // hitbox: count, then z offset/size, y offset/size, x offset/size
            radius = SFAI_MAXF(scn->info.hitbox[6], scn->info.hitbox[2]) + 200.0f;
            height = scn->info.hitbox[3] + scn->info.hitbox[4] + 250.0f;
        }
        if ((fabsf(side) < radius) && (self->pos.y < scn->obj.pos.y + height)) {
            // steer to the side we are already on, and up
            f32 push = (side > 0.0f) ? -1.0f : 1.0f;

            dir->x += -fwd.z * push * 2.0f * SFAI_Len(dir);
            dir->z += fwd.x * push * 2.0f * SFAI_Len(dir);
            if (ahead < 900.0f) {
                *climb = true;
            }
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
    if (ai->thinkTimer-- <= 0) {
        ai->thinkTimer = sReactionFrames[ai->level];
        SFAI_Decide(ai, playerNum);
    }
    SFAI_UpdateAim(ai, playerNum);

    // where to fly
    SFAI_Delta(&self->pos, &ai->aimPoint, &dir);
    SFAI_AvoidObstacles(self, &dir, &climb);
    SFAI_Forward(self, &fwd);

    headingWanted = SFAI_Heading(dir.x, dir.z) + ai->aimNoiseX;
    horiz = sqrtf(SQ(dir.x) + SQ(dir.z));
    pitchWanted = atan2f(dir.y, horiz) * M_RTOD + ai->aimNoiseY;

    // keep inside the arena's height (VS arenas are flat: the ceiling is only ~650 above the floor)
    floorY = SFAI_MAXF(gGroundHeight, self->pathFloor);
    if (climb || (self->pos.y < floorY + 110.0f)) {
        pitchWanted = SFAI_MAXF(pitchWanted, climb ? 30.0f : 15.0f);
    }
    if (self->pos.y > self->pathHeight - 110.0f) {
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
            // hold fire to charge and lock on, let go, then press again to fire the charged shot
            // (or a smart bomb, which also homes in on a locked target)
            bool locked = (ai->target >= 0) && (gVsLockOnTimers[ai->target][playerNum] != 0);

            ai->wantFire = false;
            if ((ai->rollDir != 2) && (locked || (ai->maneuverFrame >= 50))) {
                ai->rollDir = 2; // released
                ai->maneuverFrame = 100;
            }
            if (ai->maneuverFrame < 100) {
                buttons |= A_BUTTON;
            } else if (ai->maneuverFrame == 102) {
                if (locked && (gBombCount[playerNum] > 0) && (SFAI_RandF(ai) < ai->p.bomb * 0.5f)) {
                    buttons |= B_BUTTON;
                } else {
                    buttons |= A_BUTTON;
                }
            } else if (ai->maneuverFrame > 104) {
                ai->maneuver = MANEUVER_NONE;
            }
            break;
        }

        default:
            break;
    }
    if (ai->maneuver != MANEUVER_NONE) {
        ai->maneuverFrame++;
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

    // smart bomb when locked on, or now and then at a close target in front
    if ((gBombCount[playerNum] > 0) && (ai->target >= 0) && (ai->cooldown == 0) && (ai->mode == MODE_ATTACK)) {
        bool locked = gVsLockOnTimers[ai->target][playerNum] != 0;

        if ((locked || (ai->wantFire && (ai->aimDist < 1500.0f) && (SFAI_RandF(ai) < 0.02f))) &&
            (SFAI_RandF(ai) < ai->p.bomb * 0.25f)) {
            buttons |= B_BUTTON;
            ai->cooldown = 45;
        }
    }

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
               "stick %d %d btn %04x\n",
               playerNum + 1, ai->mode, ai->target + 1, ai->aimDist, ai->aimTime, yawErr, pitchWanted, pitchNow,
               self->pos.y, floorY, self->pathHeight, ai->wantFire, ai->maneuver, *stickX, *stickY, buttons);
        fflush(stdout);
    }
}
