#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "SFModVs.h"
#include "SFModAI.h"
#include "SFMod.h"
#include "global.h"
#include "assets/ast_common.h"
#include "port/mods/PortEnhancements.h"

/*
 * VS CPU mode
 *
 * Up to 10 pilots fight in the original VS arenas. Every pilot is a normal VS player of the
 * original game (same ship, same damage, same rules); the CPU pilots only get their controller
 * input from the AI (SFModAI.c). The screen always shows one pilot full screen: the human
 * player, or - when only CPUs fight - the pilot that is being watched (Z/R switches).
 *
 * This file replaces the original 4-player VS flow (Versus_Update/Versus_Draw) while such a
 * match runs: the setup screen on the VS card, countdown, respawns, scoring, winner, results.
 */

// from the original game (no header declares them)
extern void Versus_ReturnToMenu(void);
extern void func_versus_800BC9DC(f32 xPos, f32 yPos, f32 scale, s32 index);
extern void func_versus_800BCB44(f32 xPos, f32 yPos, f32 scale);
extern void HUD_ShieldGaugeBars_Draw(f32 xPos, f32 yPos, f32 xScale, f32 yScale, f32 arg4);
extern void HUD_ShieldGaugeEdgeLeft_Draw(f32 xPos, f32 yPos, f32 xScale, f32 yScale);
extern void HUD_ShieldGaugeEdgeRight_Draw(f32 xPos, f32 yPos, f32 xScale, f32 yScale);
extern void HUD_ShieldGaugeFrame_Draw(f32 xPos, f32 yPos, f32 xScale, f32 yScale);
extern void HUD_BoostGauge_Draw(f32 xPos, f32 yPos);

// fox_option.c
extern void Option_SFMod_ReturnToMainMenu(void);
extern void Option_SFMod_StartVersus(void);

// SFMod.c
extern void SFMod_DrawSwapMessage(void);

/* ------------------------------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------------------------------ */

SFVsConfig gSFVs = {
    6,                  // pilots
    true,               // the human plays pilot 1
    VS_STAGE_CORNERIA,  // stage
    SFVS_RULE_POINTS,   // rule
    3,                  // kills to win
    false,              // teams
    {
        { SFVS_FOX, 4, -1, 2 },
        { SFVS_FALCO, 4, -1, 2 },
        { SFVS_WOLF, 4, -1, 1 },
        { SFVS_PEPPY, 4, -1, 2 },
        { SFVS_LEON, 4, -1, 1 },
        { SFVS_SLIPPY, 4, -1, 2 },
        { SFVS_PIGMA, 4, -1, 1 },
        { SFVS_ANDREW, 4, -1, 1 },
        { SFVS_BILL, 4, -1, 3 },
        { SFVS_KATT, 4, -1, 4 },
    },
};

static const char* sCharacterNames[SFVS_CHARACTER_COUNT] = {
    "FOX", "FALCO", "PEPPY", "SLIPPY", "WOLF", "PIGMA", "ANDREW", "LEON", "BILL", "KATT",
};

static const char* sLevelNames[7] = {
    "EASIEST", "VERY EASY", "EASY", "MEDIUM", "HARD", "VERY HARD", "HARDEST",
};

static const char* sStageNames[3] = { "CORNERIA", "KATINA", "SECTOR Z" };
static const char* sRuleNames[SFVS_RULE_COUNT] = { "POINTS", "TIME", "SURVIVAL" };
static const char* sTeamNames[SFVS_TEAM_COUNT + 1] = { "-", "RED", "BLUE", "GREEN", "YELLOW" };

// clang-format off
static const u8 sPilotColors[MAX_PLAYERS][3] = {
    { 255, 255, 32 }, { 255, 48, 48 },   { 48, 255, 48 },   { 72, 112, 255 }, { 255, 144, 0 },
    { 255, 64, 255 }, { 48, 240, 255 },  { 240, 240, 240 }, { 176, 112, 255 }, { 255, 176, 208 },
};
static const u8 sTeamColors[SFVS_TEAM_COUNT + 1][3] = {
    { 255, 255, 255 }, { 255, 48, 48 }, { 72, 128, 255 }, { 48, 255, 48 }, { 255, 255, 32 },
};
// clang-format on

#define CVAR_SFVS_PREFIX "gSFMod.Vs."

static void SFModVs_LoadConfig(void) {
    char name[64];
    s32 i;

    gSFVs.numPilots = CLAMP(CVarGetInteger(CVAR_SFVS_PREFIX "Pilots", gSFVs.numPilots), 2, MAX_PLAYERS);
    gSFVs.humanPlays = CVarGetInteger(CVAR_SFVS_PREFIX "HumanPlays", gSFVs.humanPlays) != 0;
    gSFVs.stage = CLAMP(CVarGetInteger(CVAR_SFVS_PREFIX "Stage", gSFVs.stage), 0, 2);
    gSFVs.rule = CLAMP(CVarGetInteger(CVAR_SFVS_PREFIX "Rule", gSFVs.rule), 0, SFVS_RULE_COUNT - 1);
    gSFVs.target = CLAMP(CVarGetInteger(CVAR_SFVS_PREFIX "Target", gSFVs.target), 1, 20);
    gSFVs.teams = CVarGetInteger(CVAR_SFVS_PREFIX "Teams", gSFVs.teams) != 0;
    for (i = 0; i < MAX_PLAYERS; i++) {
        SFVsPilot* p = &gSFVs.pilots[i];

        snprintf(name, sizeof(name), CVAR_SFVS_PREFIX "Pilot%d.Character", i + 1);
        p->character = CLAMP(CVarGetInteger(name, p->character), 0, SFVS_CHARACTER_COUNT - 1);
        snprintf(name, sizeof(name), CVAR_SFVS_PREFIX "Pilot%d.Level", i + 1);
        p->level = CLAMP(CVarGetInteger(name, p->level), 1, 7);
        snprintf(name, sizeof(name), CVAR_SFVS_PREFIX "Pilot%d.Personality", i + 1);
        p->personality = CLAMP(CVarGetInteger(name, p->personality), -1, SFAI_PersonalityCount() - 1);
        snprintf(name, sizeof(name), CVAR_SFVS_PREFIX "Pilot%d.Team", i + 1);
        p->team = CLAMP(CVarGetInteger(name, p->team), 1, SFVS_TEAM_COUNT);
    }
}

static void SFModVs_SaveConfig(void) {
    char name[64];
    s32 i;

    CVarSetInteger(CVAR_SFVS_PREFIX "Pilots", gSFVs.numPilots);
    CVarSetInteger(CVAR_SFVS_PREFIX "HumanPlays", gSFVs.humanPlays);
    CVarSetInteger(CVAR_SFVS_PREFIX "Stage", gSFVs.stage);
    CVarSetInteger(CVAR_SFVS_PREFIX "Rule", gSFVs.rule);
    CVarSetInteger(CVAR_SFVS_PREFIX "Target", gSFVs.target);
    CVarSetInteger(CVAR_SFVS_PREFIX "Teams", gSFVs.teams);
    for (i = 0; i < MAX_PLAYERS; i++) {
        SFVsPilot* p = &gSFVs.pilots[i];

        snprintf(name, sizeof(name), CVAR_SFVS_PREFIX "Pilot%d.Character", i + 1);
        CVarSetInteger(name, p->character);
        snprintf(name, sizeof(name), CVAR_SFVS_PREFIX "Pilot%d.Level", i + 1);
        CVarSetInteger(name, p->level);
        snprintf(name, sizeof(name), CVAR_SFVS_PREFIX "Pilot%d.Personality", i + 1);
        CVarSetInteger(name, p->personality);
        snprintf(name, sizeof(name), CVAR_SFVS_PREFIX "Pilot%d.Team", i + 1);
        CVarSetInteger(name, p->team);
    }
    CVarSave();
}

/* ------------------------------------------------------------------------------------------------
 * Match state
 * ------------------------------------------------------------------------------------------------ */

typedef enum {
    SFVS_STATE_INIT,
    SFVS_STATE_BGM,
    SFVS_STATE_SPAWN,
    SFVS_STATE_COUNTDOWN,
    SFVS_STATE_FIGHT,
    SFVS_STATE_END,
    SFVS_STATE_REMATCH,
    SFVS_STATE_EXIT,
} SFVsState;

#define SFVS_NO_WINNER -1
#define SFVS_FEED_COUNT 4

typedef struct {
    char text[48];
    u8 r, g, b;
    s32 timer;
} SFVsFeed;

static bool sActive = false;   // a VS CPU match runs
static SFVsState sState;
static s32 sTimer;
static s32 sCountdown;
static s32 sCountdownTimer;
static s32 sGoTimer;
static s32 sViewed;
static s32 sTimeLeft;          // frames (time rule)
static bool sHurryBgm;
static bool sHurryBgmPlaying;
static s32 sStageBgm = -1;
static s32 sWinner;            // pilot index, or team (team mode), or SFVS_NO_WINNER (draw)
static bool sWinnerIsTeam;
static s32 sKills[MAX_PLAYERS];
static s32 sDeaths[MAX_PLAYERS];
static s32 sLives[MAX_PLAYERS];
static bool sOut[MAX_PLAYERS];
static s32 sRespawnTimer[MAX_PLAYERS];
static s32 sPrevState[MAX_PLAYERS];
static u16 sAiPrevHold[MAX_PLAYERS];
static u16 sHumanPrevHold;
static u16 sSpectatorPress;    // buttons pressed on the human controller this frame
static SFVsFeed sFeed[SFVS_FEED_COUNT];
static bool sDebug = false;

bool SFModVs_Active(void) {
    return sActive && gVersusMode;
}

int32_t SFModVs_ViewedPlayer(void) {
    if (!SFModVs_Active() || (sViewed >= gCamCount)) {
        return 0;
    }
    return sViewed;
}

bool SFModVs_IsHuman(int32_t playerNum) {
    return SFModVs_Active() && gSFVs.humanPlays && (playerNum == 0);
}

bool SFModVs_SameTeam(int32_t a, int32_t b) {
    if (!SFModVs_Active() || !gSFVs.teams || (a == b) || (a < 0) || (b < 0) || (a >= MAX_PLAYERS) ||
        (b >= MAX_PLAYERS)) {
        return false;
    }
    return gSFVs.pilots[a].team == gSFVs.pilots[b].team;
}

void SFModVs_GetPlayerColor(int32_t playerNum, uint8_t* r, uint8_t* g, uint8_t* b) {
    const u8* c;

    if ((playerNum < 0) || (playerNum >= MAX_PLAYERS)) {
        playerNum = 0;
    }
    if (SFModVs_Active() && gSFVs.teams) {
        c = sTeamColors[CLAMP(gSFVs.pilots[playerNum].team, 0, SFVS_TEAM_COUNT)];
    } else {
        c = sPilotColors[playerNum];
    }
    *r = c[0];
    *g = c[1];
    *b = c[2];
}

const char* SFModVs_CharacterName(int32_t character) {
    if ((character < 0) || (character >= SFVS_CHARACTER_COUNT)) {
        return "?";
    }
    return sCharacterNames[character];
}

int32_t SFModVs_Character(int32_t playerNum) {
    if ((playerNum < 0) || (playerNum >= MAX_PLAYERS)) {
        return SFVS_FOX;
    }
    return gSFVs.pilots[playerNum].character;
}

static const char* SFModVs_PilotName(s32 i) {
    return SFModVs_CharacterName(gSFVs.pilots[i].character);
}

static bool SFModVs_Alive(s32 i) {
    return (gPlayer[i].state == PLAYERSTATE_ACTIVE) || (gPlayer[i].state == PLAYERSTATE_U_TURN);
}

static void SFModVs_AddFeed(const char* text, s32 colorPilot) {
    s32 i;
    u8 r, g, b;

    for (i = SFVS_FEED_COUNT - 1; i > 0; i--) {
        sFeed[i] = sFeed[i - 1];
    }
    snprintf(sFeed[0].text, sizeof(sFeed[0].text), "%s", text);
    SFModVs_GetPlayerColor(colorPilot, &r, &g, &b);
    sFeed[0].r = r;
    sFeed[0].g = g;
    sFeed[0].b = b;
    sFeed[0].timer = 120;
}

// score of a pilot for the current rule
static s32 SFModVs_PilotScore(s32 i) {
    if (gSFVs.rule == SFVS_RULE_SURVIVAL) {
        return sLives[i];
    }
    return sKills[i];
}

static s32 SFModVs_TeamScore(s32 team) {
    s32 i;
    s32 score = 0;

    for (i = 0; i < gSFVs.numPilots; i++) {
        if (gSFVs.pilots[i].team == team) {
            score += SFModVs_PilotScore(i);
        }
    }
    return score;
}

static bool SFModVs_TeamUsed(s32 team) {
    s32 i;

    for (i = 0; i < gSFVs.numPilots; i++) {
        if (gSFVs.pilots[i].team == team) {
            return true;
        }
    }
    return false;
}

static void SFModVs_ResetStats(void) {
    s32 i;

    for (i = 0; i < MAX_PLAYERS; i++) {
        sKills[i] = 0;
        sDeaths[i] = 0;
        sLives[i] = (gSFVs.rule == SFVS_RULE_SURVIVAL) ? gSFVs.target : 0;
        sOut[i] = false;
        sRespawnTimer[i] = 0;
        sPrevState[i] = PLAYERSTATE_INIT;
        sAiPrevHold[i] = 0;
    }
    for (i = 0; i < SFVS_FEED_COUNT; i++) {
        sFeed[i].timer = 0;
    }
    sTimeLeft = gSFVs.target * 60 * 30;
    sHurryBgm = false;
    sWinner = SFVS_NO_WINNER;
    sWinnerIsTeam = false;
}

// Prepares everything for a match (the game state switch to GSTATE_VS_INIT is done by the caller).
static void SFModVs_PrepareMatch(void) {
    u32 seed = (u32) osGetTime() ^ ((u32) gGameFrameCount * 2654435761u);
    s32 i;

    gVersusStage = gSFVs.stage;
    // The original point match rules, but with a target nobody reaches: this mode decides the
    // winner itself (also for teams, time and survival).
    gVsMatchType = VS_MATCH_POINTS;
    gVsPointsToWin = 10000;
    gVsTimeTrialLimit = 0;

    for (i = 0; i < MAX_PLAYERS; i++) {
        gPlayerInactive[i] = (i >= gSFVs.numPilots);
        gHandicap[i] = 0;
        gPlayerForms[i] = FORM_ARWING;
        gShowReticles[i] = true;
        if ((i < gSFVs.numPilots) && !(gSFVs.humanPlays && (i == 0))) {
            SFAI_Reset(i, gSFVs.pilots[i].level, gSFVs.pilots[i].personality, seed + (u32) i * 7919u);
        }
    }

    sViewed = 0;
    sActive = true;
    sStageBgm = -1;
    sState = SFVS_STATE_INIT;
    sTimer = 0;
    SFModVs_ResetStats();
    if (sDebug) {
        printf("[SFVS] match: %d pilots, human %d, stage %d, rule %d, target %d, teams %d\n", gSFVs.numPilots,
               gSFVs.humanPlays, gSFVs.stage, gSFVs.rule, gSFVs.target, gSFVs.teams);
        for (i = 0; i < gSFVs.numPilots; i++) {
            printf("[SFVS]   pilot %d: %s level %d personality %d team %d\n", i + 1, SFModVs_PilotName(i),
                   gSFVs.pilots[i].level, SFAI_GetPersonalityIndex(i), gSFVs.pilots[i].team);
        }
        fflush(stdout);
    }
}

void SFModVs_OnMatchExit(void) {
    sActive = false;
}

/* ------------------------------------------------------------------------------------------------
 * Controller input
 * ------------------------------------------------------------------------------------------------ */

static s8 SFModVs_RawStick(s8 processed) {
    // the inverse of the game's dead zone (Controller_AddDeadZone)
    if (processed > 0) {
        return processed + 16;
    }
    if (processed < 0) {
        return processed - 16;
    }
    return 0;
}

void SFModVs_FillInputs(void) {
    OSContPad human;
    u16 humanPress;
    s32 i;
    bool aiRuns;

    if (!SFModVs_Active() || (gGameState != GSTATE_PLAY) || (gPlayer == NULL)) {
        return;
    }

    human = gControllerHold[gMainController];
    humanPress = human.button & ~sHumanPrevHold;
    sHumanPrevHold = human.button;
    sSpectatorPress = humanPress;

    aiRuns = (gPlayState == PLAY_UPDATE) && ((sState == SFVS_STATE_FIGHT) || (sState == SFVS_STATE_COUNTDOWN));

    for (i = 0; i < MAX_PLAYERS; i++) {
        u16 hold = 0;
        s8 sx = 0;
        s8 sy = 0;

        if (gSFVs.humanPlays && (i == 0)) {
            if (gMainController != 0) {
                gControllerHold[0] = human;
                gControllerPress[0].button = humanPress;
                gControllerPress[0].stick_x = gControllerPress[gMainController].stick_x;
                gControllerPress[0].stick_y = gControllerPress[gMainController].stick_y;
            }
            continue;
        }

        if ((i < gCamCount) && aiRuns && (sState == SFVS_STATE_FIGHT) && (sWinner == SFVS_NO_WINNER)) {
            SFAI_Think(i, &hold, &sx, &sy);
        }

        if (i == gMainController) {
            // keep the human's START so the match can still be paused
            hold |= human.button & START_BUTTON;
        }

        gControllerHold[i].button = hold;
        gControllerHold[i].stick_x = SFModVs_RawStick(sx);
        gControllerHold[i].stick_y = SFModVs_RawStick(sy);
        gControllerHold[i].err_no = 0;
        gControllerPress[i].button = hold & ~sAiPrevHold[i];
        gControllerPress[i].stick_x = sx;
        gControllerPress[i].stick_y = sy;
        gControllerPress[i].err_no = 0;
        sAiPrevHold[i] = hold;
    }
}

/* ------------------------------------------------------------------------------------------------
 * Match logic
 * ------------------------------------------------------------------------------------------------ */

// The original only drops a lock-on when the locked target leaves the locking player's view, and it
// only checks that while drawing that player's view. Only one view is drawn here, so the same test
// is done for every other pilot from its camera.
static void SFModVs_CheckLockOnViews(void) {
    s32 locker;
    s32 target;

    for (locker = 0; locker < gCamCount; locker++) {
        Player* lp = &gPlayer[locker];
        Vec3f fwd;
        Vec3f right;
        Vec3f up;
        f32 len;

        if (locker == sViewed) {
            continue; // checked by the drawing code
        }

        fwd.x = lp->cam.at.x - lp->cam.eye.x;
        fwd.y = lp->cam.at.y - lp->cam.eye.y;
        fwd.z = lp->cam.at.z - lp->cam.eye.z;
        len = sqrtf(SQ(fwd.x) + SQ(fwd.y) + SQ(fwd.z));
        if (len < 0.01f) {
            continue;
        }
        fwd.x /= len;
        fwd.y /= len;
        fwd.z /= len;
        // right = fwd x worldUp, up = right x fwd
        right.x = -fwd.z;
        right.y = 0.0f;
        right.z = fwd.x;
        len = sqrtf(SQ(right.x) + SQ(right.z));
        if (len < 0.01f) {
            right.x = 1.0f;
            right.z = 0.0f;
        } else {
            right.x /= len;
            right.z /= len;
        }
        up.x = right.y * fwd.z - right.z * fwd.y;
        up.y = right.z * fwd.x - right.x * fwd.z;
        up.z = right.x * fwd.y - right.y * fwd.x;

        for (target = 0; target < gCamCount; target++) {
            Player* tp = &gPlayer[target];
            Vec3f d;
            f32 vx;
            f32 vy;
            f32 vz;

            if (gVsLockOnTimers[target][locker] == 0) {
                continue;
            }
            d.x = tp->pos.x - lp->cam.eye.x;
            d.y = tp->pos.y - lp->cam.eye.y;
            d.z = tp->trueZpos - lp->cam.eye.z;
            vz = -(d.x * fwd.x + d.y * fwd.y + d.z * fwd.z); // view space: negative z is in front
            vx = d.x * right.x + d.y * right.y + d.z * right.z;
            vy = d.x * up.x + d.y * up.y + d.z * up.z;
            if ((vz > 300.0f) || (vz < -8000.0f) || (fabsf(vx) > (fabsf(vz * 0.5f) + 100.0f)) ||
                (fabsf(vy) > (fabsf(vz * 0.5f) + 100.0f))) {
                gVsLockOnTimers[target][locker] = 0;
            }
        }
    }
}

static s32 SFModVs_RandomAlivePilot(s32 except) {
    s32 alive[MAX_PLAYERS];
    s32 count = 0;
    s32 i;

    for (i = 0; i < gCamCount; i++) {
        if ((i != except) && SFModVs_Alive(i)) {
            alive[count++] = i;
        }
    }
    if (count == 0) {
        return -1;
    }
    return alive[RAND_INT(count - 0.01f)];
}

static void SFModVs_OnDeath(s32 i) {
    Player* pl = &gPlayer[i];
    char text[48];
    s32 killer = (pl->attacker > 0) ? (pl->attacker - 1) : -1;

    sDeaths[i]++;
    if ((killer >= 0) && (killer < gCamCount) && (killer != i)) {
        sKills[killer]++;
        snprintf(text, sizeof(text), "%s DOWNED %s", SFModVs_PilotName(killer), SFModVs_PilotName(i));
        SFModVs_AddFeed(text, killer);
    } else {
        snprintf(text, sizeof(text), "%s CRASHED", SFModVs_PilotName(i));
        SFModVs_AddFeed(text, i);
    }
    if (gSFVs.rule == SFVS_RULE_SURVIVAL) {
        if (sLives[i] > 0) {
            sLives[i]--;
        }
        if (sLives[i] == 0) {
            sOut[i] = true;
            snprintf(text, sizeof(text), "%s IS OUT", SFModVs_PilotName(i));
            SFModVs_AddFeed(text, i);
        }
    }
    sRespawnTimer[i] = 45;
    if (sDebug) {
        printf("[SFVS] frame %d: %s down (killer %d) kills %d deaths %d\n", gGameFrameCount, SFModVs_PilotName(i),
               killer + 1, (killer >= 0) ? sKills[killer] : 0, sDeaths[i]);
        fflush(stdout);
    }
}

static void SFModVs_UpdatePilots(void) {
    s32 i;

    for (i = 0; i < gCamCount; i++) {
        Player* pl = &gPlayer[i];

        // Cutscene_KillPlayer switches a destroyed ship to PLAYERSTATE_NEXT (and scores the kill)
        if ((pl->state == PLAYERSTATE_NEXT) && (sPrevState[i] != PLAYERSTATE_NEXT)) {
            SFModVs_OnDeath(i);
        }
        sPrevState[i] = pl->state;

        if (pl->state != PLAYERSTATE_VS_STANDBY) {
            continue;
        }
        if (sOut[i]) {
            // out of lives: keep watching somebody who still flies
            pl->csState = 0;
            if ((pl->attacker <= 0) || (pl->attacker > gCamCount) || !SFModVs_Alive(pl->attacker - 1)) {
                s32 other = SFModVs_RandomAlivePilot(i);

                if (other >= 0) {
                    pl->attacker = other + 1;
                }
            }
            continue;
        }
        if (gVsMatchWon) {
            continue;
        }
        if (sRespawnTimer[i] > 0) {
            sRespawnTimer[i]--;
            pl->csState = 0;
        } else {
            pl->csState = 1; // respawn (handled by the player code)
        }
    }
}

// Decides the winner. Returns true when the match is over.
static bool SFModVs_CheckWinner(void) {
    s32 i;
    s32 best = -1;
    s32 bestScore = -1;
    bool tie = false;

    switch (gSFVs.rule) {
        case SFVS_RULE_POINTS:
            if (gSFVs.teams) {
                for (i = 1; i <= SFVS_TEAM_COUNT; i++) {
                    if (SFModVs_TeamUsed(i) && (SFModVs_TeamScore(i) >= gSFVs.target)) {
                        sWinner = i;
                        sWinnerIsTeam = true;
                        return true;
                    }
                    if (SFModVs_TeamUsed(i) && (SFModVs_TeamScore(i) == gSFVs.target - 1)) {
                        sHurryBgm = true;
                    }
                }
            } else {
                for (i = 0; i < gCamCount; i++) {
                    if (sKills[i] >= gSFVs.target) {
                        sWinner = i;
                        return true;
                    }
                    if ((sKills[i] == gSFVs.target - 1) && (gSFVs.target > 1)) {
                        sHurryBgm = true;
                    }
                }
            }
            return false;

        case SFVS_RULE_TIME:
            if (sTimeLeft < 30 * 30) {
                sHurryBgm = true;
            }
            if (sTimeLeft > 0) {
                return false;
            }
            if (gSFVs.teams) {
                for (i = 1; i <= SFVS_TEAM_COUNT; i++) {
                    if (!SFModVs_TeamUsed(i)) {
                        continue;
                    }
                    if (SFModVs_TeamScore(i) > bestScore) {
                        bestScore = SFModVs_TeamScore(i);
                        best = i;
                        tie = false;
                    } else if (SFModVs_TeamScore(i) == bestScore) {
                        tie = true;
                    }
                }
                sWinnerIsTeam = true;
            } else {
                for (i = 0; i < gCamCount; i++) {
                    // more kills wins, fewer deaths breaks a tie
                    s32 score = sKills[i] * 1000 - sDeaths[i];

                    if (score > bestScore) {
                        bestScore = score;
                        best = i;
                        tie = false;
                    } else if (score == bestScore) {
                        tie = true;
                    }
                }
            }
            sWinner = tie ? SFVS_NO_WINNER : best;
            return true;

        case SFVS_RULE_SURVIVAL: {
            s32 sides = 0;
            s32 lastSide = SFVS_NO_WINNER;
            bool sideAlive[MAX_PLAYERS + SFVS_TEAM_COUNT + 1];

            memset(sideAlive, 0, sizeof(sideAlive));
            for (i = 0; i < gCamCount; i++) {
                s32 side = gSFVs.teams ? gSFVs.pilots[i].team : i;

                if (sOut[i] || sideAlive[side]) {
                    continue;
                }
                sideAlive[side] = true;
                sides++;
                lastSide = side;
            }
            if ((sides == 2) && (gCamCount > 2)) {
                sHurryBgm = true;
            }
            if (sides > 1) {
                return false;
            }
            sWinner = lastSide;
            sWinnerIsTeam = gSFVs.teams && (lastSide != SFVS_NO_WINNER);
            return true;
        }
    }
    return false;
}

static bool SFModVs_IsWinner(s32 i) {
    if (sWinner == SFVS_NO_WINNER) {
        return false;
    }
    if (sWinnerIsTeam) {
        return gSFVs.pilots[i].team == sWinner;
    }
    return i == sWinner;
}

static void SFModVs_EndMatch(void) {
    s32 i;
    s32 follow = -1;

    gVsMatchWon = true;
    gVsMatchOver = true;
    SEQCMD_STOP_SEQUENCE(SEQ_PLAYER_BGM, 1);
    SEQCMD_STOP_SEQUENCE(SEQ_PLAYER_FANFARE, 1);

    for (i = 0; i < gCamCount; i++) {
        if (SFModVs_IsWinner(i) && SFModVs_Alive(i)) {
            follow = i;
            break;
        }
    }
    if (follow >= 0) {
        // like the original: the losers leave the arena and their cameras show a winner
        for (i = 0; i < gCamCount; i++) {
            if (SFModVs_IsWinner(i)) {
                continue;
            }
            gPlayer[i].attacker = follow + 1;
            if (gPlayer[i].state != PLAYERSTATE_VS_STANDBY) {
                gPlayer[i].state = PLAYERSTATE_VS_STANDBY;
            }
        }
    }
    sState = SFVS_STATE_END;
    sTimer = 0;
    if (sDebug) {
        printf("[SFVS] match over at frame %d: winner %d (team %d)\n", gGameFrameCount, sWinner, sWinnerIsTeam);
        for (i = 0; i < gCamCount; i++) {
            printf("[SFVS]   %s kills %d deaths %d lives %d\n", SFModVs_PilotName(i), sKills[i], sDeaths[i], sLives[i]);
        }
        fflush(stdout);
    }
}

static void SFModVs_DebugLog(void) {
    s32 i;

    if (!sDebug || ((gGameFrameCount % 150) != 0)) {
        return;
    }
    printf("[SFVS] frame %d time %d:", gGameFrameCount, sTimeLeft / 30);
    for (i = 0; i < gCamCount; i++) {
        printf(" %s(st%d sh%d k%d d%d)", SFModVs_PilotName(i), gPlayer[i].state, gPlayer[i].shields, sKills[i],
               sDeaths[i]);
    }
    printf("\n");
    fflush(stdout);
}

static void SFModVs_UpdateSpectator(void) {
    s32 step = 0;
    s32 i;

    if (gSFVs.humanPlays) {
        sViewed = 0;
        // out of lives: Z/R choose whom to watch
        if (sOut[0] && (gPlayer[0].state == PLAYERSTATE_VS_STANDBY)) {
            if (sSpectatorPress & Z_TRIG) {
                step = -1;
            } else if (sSpectatorPress & R_TRIG) {
                step = 1;
            }
            if (step != 0) {
                s32 cur = gPlayer[0].attacker - 1;

                for (i = 0; i < gCamCount; i++) {
                    cur = (cur + step + gCamCount) % gCamCount;
                    if ((cur != 0) && SFModVs_Alive(cur)) {
                        gPlayer[0].attacker = cur + 1;
                        break;
                    }
                }
            }
        }
        return;
    }
    if (sSpectatorPress & Z_TRIG) {
        step = -1;
    } else if (sSpectatorPress & R_TRIG) {
        step = 1;
    }
    if (step != 0) {
        sViewed = (sViewed + step + gCamCount) % gCamCount;
        AUDIO_PLAY_SFX(NA_SE_CURSOR, gDefaultSfxSource, 4);
    }
}

static void SFModVs_SetStageBgColor(void) {
    switch (gVersusStage) {
        case VS_STAGE_KATINA:
            gBgColor = 0xADA7;
            break;
        case VS_STAGE_SECTOR_Z:
            gBgColor = 0x0001;
            break;
        case VS_STAGE_CORNERIA:
        default:
            gBgColor = 0x8FBD;
            break;
    }
}

static void SFModVs_ExitToMenu(void) {
    SEQCMD_STOP_SEQUENCE(SEQ_PLAYER_BGM, 1);
    SEQCMD_STOP_SEQUENCE(SEQ_PLAYER_FANFARE, 1);
    sActive = false;
    gFillScreenAlphaTarget = 0;
    Versus_ReturnToMenu();
}

void SFModVs_Update(void) {
    s32 i;

    if (!SFModVs_Active()) {
        return;
    }

    for (i = 0; i < SFVS_FEED_COUNT; i++) {
        if (sFeed[i].timer > 0) {
            sFeed[i].timer--;
        }
    }
    if (sGoTimer > 0) {
        sGoTimer--;
    }

    switch (sState) {
        case SFVS_STATE_INIT:
            SFModVs_ResetStats();
            gVsMatchOver = false;
            gBgColor = 0;
            gFillScreenAlphaTarget = 0;
            sTimer = 0;
            sState = SFVS_STATE_BGM;
            break;

        case SFVS_STATE_BGM:
            SFModVs_SetStageBgColor();
            if (++sTimer >= 20) {
                if (sStageBgm < 0) {
                    sStageBgm = gBgmSeqId;
                }
                gBgmSeqId = sStageBgm;
                sHurryBgmPlaying = false;
                AUDIO_PLAY_BGM(gBgmSeqId);
                sTimer = 0;
                sState = SFVS_STATE_SPAWN;
            }
            break;

        case SFVS_STATE_SPAWN:
            if (++sTimer >= 20) {
                Player_InitVersus();
                SFModVs_ResetStats();
                sCountdown = 4;
                sCountdownTimer = 1;
                sTimer = 0;
                sState = SFVS_STATE_COUNTDOWN;
            }
            break;

        case SFVS_STATE_COUNTDOWN:
            if (--sCountdownTimer == 0) {
                sCountdown--;
                if (sCountdown != 0) {
                    AUDIO_PLAY_SFX(NA_SE_READY, gDefaultSfxSource, 4);
                    sCountdownTimer = 30;
                } else {
                    AUDIO_PLAY_SFX(NA_SE_GO, gDefaultSfxSource, 4);
                    gVsMatchStart = true;
                    sGoTimer = 30;
                    sState = SFVS_STATE_FIGHT;
                }
            }
            break;

        case SFVS_STATE_FIGHT:
            if (gSFVs.rule == SFVS_RULE_TIME) {
                if (sTimeLeft > 0) {
                    sTimeLeft--;
                    if ((sTimeLeft <= 10 * 30) && (sTimeLeft > 0) && ((sTimeLeft % 30) == 0)) {
                        AUDIO_PLAY_SFX(NA_SE_COUNTDOWN, gDefaultSfxSource, 4);
                    }
                    if (sTimeLeft == 0) {
                        AUDIO_PLAY_SFX(NA_SE_TIME_OVER, gDefaultSfxSource, 4);
                    }
                }
            }
            SFModVs_UpdatePilots();
            SFModVs_CheckLockOnViews();
            if (SFModVs_CheckWinner()) {
                SFModVs_EndMatch();
                break;
            }
            if (sHurryBgm && !sHurryBgmPlaying) {
                sHurryBgmPlaying = true;
                AUDIO_PLAY_BGM(NA_BGM_BATTLE_LAST);
            }
            SFModVs_DebugLog();
            break;

        case SFVS_STATE_END:
            gPauseEnabled = false;
            sTimer++;
            if (sTimer == 60) {
                bool good = !gSFVs.humanPlays || SFModVs_IsWinner(0);

                AUDIO_PLAY_BGM((good && (sWinner != SFVS_NO_WINNER)) ? NA_BGM_COURSE_CLEAR : NA_BGM_COURSE_FAILURE);
            }
            if (sTimer > 90) {
                if (sSpectatorPress & (A_BUTTON | START_BUTTON)) {
                    AUDIO_PLAY_SFX(NA_SE_DECIDE, gDefaultSfxSource, 4);
                    gFillScreenRed = gFillScreenGreen = gFillScreenBlue = 0;
                    gFillScreenAlphaTarget = 255;
                    gFillScreenAlphaStep = 16;
                    sState = SFVS_STATE_REMATCH;
                } else if (sSpectatorPress & B_BUTTON) {
                    AUDIO_PLAY_SFX(NA_SE_CANCEL, gDefaultSfxSource, 4);
                    gFillScreenRed = gFillScreenGreen = gFillScreenBlue = 0;
                    gFillScreenAlphaTarget = 255;
                    gFillScreenAlphaStep = 16;
                    sState = SFVS_STATE_EXIT;
                }
            }
            break;

        case SFVS_STATE_REMATCH:
            gPauseEnabled = false;
            if (gFillScreenAlpha >= 255) {
                SEQCMD_STOP_SEQUENCE(SEQ_PLAYER_BGM, 1);
                SEQCMD_STOP_SEQUENCE(SEQ_PLAYER_FANFARE, 1);
                gFillScreenAlphaTarget = 0;
                // like Versus_PlayAgain
                Player_InitVersus();
                sState = SFVS_STATE_INIT;
            }
            break;

        case SFVS_STATE_EXIT:
            gPauseEnabled = false;
            if (gFillScreenAlpha >= 255) {
                SFModVs_ExitToMenu();
                return;
            }
            break;
    }

    SFModVs_UpdateSpectator();
}

// While paused: B quits to the VS setup.
void SFModVs_PauseUpdate(void) {
    if (!SFModVs_Active()) {
        return;
    }
    if (sSpectatorPress & B_BUTTON) {
        Audio_PlayPauseSfx(0);
        gPlayState = PLAY_UPDATE;
        SFModVs_ExitToMenu();
    }
}

/* ------------------------------------------------------------------------------------------------
 * Drawing
 * ------------------------------------------------------------------------------------------------ */

static void SFModVs_TextColor(u8 r, u8 g, u8 b, u8 a) {
    RCP_SetupDL(&gMasterDisp, SETUPDL_83);
    gDPSetPrimColor(gMasterDisp++, 0, 0, r, g, b, a);
}

static void SFModVs_PilotTextColor(s32 i, u8 a) {
    u8 r, g, b;

    SFModVs_GetPlayerColor(i, &r, &g, &b);
    SFModVs_TextColor(r, g, b, a);
}

static void SFModVs_Text(s32 x, s32 y, f32 scale, const char* text) {
    Graphics_DisplaySmallText(x, y, scale, scale, (char*) text);
}

static s32 SFModVs_TextWidth(const char* text, f32 scale) {
    return (s32) (Graphics_GetSmallTextWidth((char*) text) * scale);
}

static void SFModVs_TextCentered(s32 y, f32 scale, const char* text) {
    SFModVs_Text((SCREEN_WIDTH - SFModVs_TextWidth(text, scale)) / 2, y, scale, text);
}

static void SFModVs_LargeTextCentered(s32 y, f32 scale, const char* text) {
    s32 width = (s32) (Graphics_GetLargeTextWidth((char*) text) * scale);

    Graphics_DisplayLargeText((SCREEN_WIDTH - width) / 2, y, scale, scale, (char*) text);
}

static void SFModVs_DrawShieldGauge(s32 player, f32 x, f32 y) {
    f32 fill = gPlayer[player].shields / 255.0f;

    if (fill < 0.0f) {
        fill = 0.0f;
    }
    if (fill > 1.0f) {
        fill = 1.0f;
    }
    x = OTRGetRectDimensionFromLeftEdgeOverride(x);
    RCP_SetupDL(&gMasterDisp, SETUPDL_75);
    gDPSetPrimColor(gMasterDisp++, 0, 0, 255, 255, 255, 255);
    HUD_ShieldGaugeBars_Draw(x + 7.0f, y + 2.0f, 1.0f, 1.0f, fill);
    RCP_SetupDL(&gMasterDisp, SETUPDL_76);
    gDPSetPrimColor(gMasterDisp++, 0, 0, 255, 255, 255, 255);
    HUD_ShieldGaugeEdgeLeft_Draw(x, y, 1.0f, 1.0f);
    HUD_ShieldGaugeEdgeRight_Draw(x + 7.0f + 48.0f, y, 1.0f, 1.0f);
    HUD_ShieldGaugeFrame_Draw(x + 7.0f, y, 6.0f, 1.0f);
}

static void SFModVs_DrawBombs(s32 player, f32 x, f32 y) {
    s32 i;
    s32 count = gBombCount[player];

    if (count <= 0) {
        return;
    }
    x = OTRGetDimensionFromRightEdgeOverride(x);
    RCP_SetupDL(&gMasterDisp, SETUPDL_78_OPTIONAL);
    gDPSetPrimColor(gMasterDisp++, 0, 0, 255, 255, 255, 255);
    for (i = 0; (i < count) && (i < 5); i++) {
        Lib_TextureRect_CI4(&gMasterDisp, (u8*) aVsBombIconTex, (u16*) aVsBombIconTLUT, 16, 16, x + 40.0f - i * 10.0f,
                            y, 1.0f, 1.0f);
    }
}

// A simple map of the arena: the VS arenas wrap around at +-12000.
static void SFModVs_DrawRadar(void) {
    const s32 size = 64;
    s32 x0 = OTRGetRectDimensionFromRightEdgeOverride(SCREEN_WIDTH - 12 - size);
    s32 y0 = SCREEN_HEIGHT - 14 - size;
    s32 i;

    Graphics_FillRectangle(&gMasterDisp, x0 - 1, y0 - 1, x0 + size + 1, y0 + size + 1, 255, 255, 255, 96);
    Graphics_FillRectangle(&gMasterDisp, x0, y0, x0 + size, y0 + size, 0, 0, 40, 160);

    for (i = 0; i < ARRAY_COUNT(gItems); i++) {
        Item* item = &gItems[i];
        f32 px;
        f32 pz;
        s32 sx;
        s32 sy;

        if (item->obj.status < OBJ_ACTIVE) {
            continue;
        }
        px = item->obj.pos.x;
        pz = item->obj.pos.z;
        sx = x0 + (s32) ((px + 12000.0f) / 24000.0f * size);
        sy = y0 + (s32) ((pz + 12000.0f) / 24000.0f * size);
        if ((sx >= x0) && (sx < x0 + size) && (sy >= y0) && (sy < y0 + size)) {
            Graphics_FillRectangle(&gMasterDisp, sx - 1, sy - 1, sx + 1, sy + 1, 255, 255, 255,
                                   (gGameFrameCount & 8) ? 255 : 128);
        }
    }

    for (i = 0; i < gCamCount; i++) {
        Player* pl = &gPlayer[i];
        f32 px;
        f32 pz;
        s32 sx;
        s32 sy;
        s32 half;
        u8 r, g, b;

        if (!SFModVs_Alive(i) && (pl->state != PLAYERSTATE_DOWN)) {
            continue;
        }
        px = pl->pos.x;
        pz = pl->trueZpos;
        while (px > 12000.0f) {
            px -= 24000.0f;
        }
        while (px < -12000.0f) {
            px += 24000.0f;
        }
        while (pz > 12000.0f) {
            pz -= 24000.0f;
        }
        while (pz < -12000.0f) {
            pz += 24000.0f;
        }
        sx = x0 + (s32) ((px + 12000.0f) / 24000.0f * size);
        sy = y0 + (s32) ((pz + 12000.0f) / 24000.0f * size);
        half = (i == sViewed) ? 2 : 1;
        if ((i == sViewed) && (gGameFrameCount & 4)) {
            half = 1;
        }
        SFModVs_GetPlayerColor(i, &r, &g, &b);
        Graphics_FillRectangle(&gMasterDisp, sx - half, sy - half, sx + half, sy + half, r, g, b, 255);
    }
}

typedef struct {
    s32 index; // pilot or team
    s32 score;
    s32 tieBreak;
} SFVsRank;

static void SFModVs_SortRanks(SFVsRank* ranks, s32 count) {
    s32 i;
    s32 j;

    for (i = 1; i < count; i++) {
        SFVsRank r = ranks[i];

        for (j = i - 1; (j >= 0) && ((ranks[j].score < r.score) ||
                                     ((ranks[j].score == r.score) && (ranks[j].tieBreak > r.tieBreak)));
             j--) {
            ranks[j + 1] = ranks[j];
        }
        ranks[j + 1] = r;
    }
}

static s32 SFModVs_GetPilotRanks(SFVsRank* ranks) {
    s32 i;

    for (i = 0; i < gSFVs.numPilots; i++) {
        ranks[i].index = i;
        ranks[i].score = SFModVs_PilotScore(i);
        ranks[i].tieBreak = sDeaths[i];
    }
    SFModVs_SortRanks(ranks, gSFVs.numPilots);
    return gSFVs.numPilots;
}

static void SFModVs_DrawScoreboard(void) {
    SFVsRank ranks[MAX_PLAYERS];
    char line[32];
    s32 x = OTRGetRectDimensionFromLeftEdgeOverride(20.0f);
    s32 y = 34;
    s32 count;
    s32 i;
    const f32 scale = 0.8f;

    if (gSFVs.teams) {
        SFVsRank teams[SFVS_TEAM_COUNT];
        s32 numTeams = 0;

        for (i = 1; i <= SFVS_TEAM_COUNT; i++) {
            if (SFModVs_TeamUsed(i)) {
                teams[numTeams].index = i;
                teams[numTeams].score = SFModVs_TeamScore(i);
                teams[numTeams].tieBreak = 0;
                numTeams++;
            }
        }
        SFModVs_SortRanks(teams, numTeams);
        for (i = 0; i < numTeams; i++) {
            const u8* c = sTeamColors[teams[i].index];

            snprintf(line, sizeof(line), "%s %d", sTeamNames[teams[i].index], teams[i].score);
            SFModVs_TextColor(c[0], c[1], c[2], 255);
            SFModVs_Text(x, y, 0.9f, line);
            y += 9;
        }
        y += 3;
    }

    count = SFModVs_GetPilotRanks(ranks);
    for (i = 0; i < count; i++) {
        s32 p = ranks[i].index;
        u8 alpha = (sOut[p] || !SFModVs_Alive(p)) ? 140 : 255;

        snprintf(line, sizeof(line), "%s %d", SFModVs_PilotName(p), ranks[i].score);
        SFModVs_PilotTextColor(p, alpha);
        SFModVs_Text(x + ((p == sViewed) ? 0 : 4), y, scale, line);
        if (p == sViewed) {
            SFModVs_Text(x - 6, y, scale, "-");
        }
        y += 8;
    }
}

void SFModVs_DrawHud(void) {
    s32 p = SFModVs_ViewedPlayer();
    char text[48];

    if (!SFModVs_Active() || (gPlayer == NULL)) {
        return;
    }

    gDPSetTextureFilter(gMasterDisp++, G_TF_POINT);

    if (gVsMatchStart && (sState == SFVS_STATE_FIGHT)) {
        if (SFModVs_Alive(p)) {
            s32 savedVs = gVersusMode;

            SFModVs_DrawShieldGauge(p, 20.0f, 18.0f);
            // the single player boost gauge (its VS layout only knows 4 screen quarters)
            gVersusMode = 0;
            RCP_SetupDL_36();
            HUD_BoostGauge_Draw(246.0f, 28.0f);
            gVersusMode = savedVs;
            SFModVs_DrawBombs(p, 250.0f, 38.0f);
        }
        SFModVs_DrawScoreboard();
        SFModVs_DrawRadar();

        if (gSFVs.rule == SFVS_RULE_TIME) {
            s32 secs = (sTimeLeft + 29) / 30;

            snprintf(text, sizeof(text), "%d:%02d", secs / 60, secs % 60);
            if (secs <= 10) {
                SFModVs_TextColor(255, 64, 64, 255);
            } else {
                SFModVs_TextColor(255, 255, 255, 255);
            }
            SFModVs_TextCentered(12, 1.2f, text);
        }
        if (gSFVs.rule == SFVS_RULE_SURVIVAL) {
            snprintf(text, sizeof(text), "LIVES %d", sLives[p]);
            SFModVs_PilotTextColor(p, 255);
            SFModVs_TextCentered(12, 1.0f, text);
        }
    }
    gDPSetTextureFilter(gMasterDisp++, G_TF_BILERP);
}

static void SFModVs_DrawCountdown(void) {
    RCP_SetupDL(&gMasterDisp, SETUPDL_78);
    gDPSetPrimColor(gMasterDisp++, 0, 0, 255, 255, 255, 255);
    if ((sState == SFVS_STATE_COUNTDOWN) && (sCountdown >= 1) && (sCountdown <= 3)) {
        static const f32 widths[] = { 40.0f, 64.0f, 64.0f };
        s32 idx = sCountdown - 1;

        func_versus_800BC9DC(160.0f - widths[idx], 80.0f, 2.0f, idx);
    } else if (sGoTimer > 0) {
        func_versus_800BCB44(160.0f - 96.0f, 90.0f, 2.0f);
    }
}

static void SFModVs_DrawFeed(void) {
    s32 i;
    s32 y = 52;

    for (i = 0; i < SFVS_FEED_COUNT; i++) {
        if (sFeed[i].timer <= 0) {
            continue;
        }
        SFModVs_TextColor(sFeed[i].r, sFeed[i].g, sFeed[i].b, (sFeed[i].timer < 30) ? sFeed[i].timer * 8 : 255);
        SFModVs_TextCentered(y, 0.9f, sFeed[i].text);
        y += 10;
    }
}

static void SFModVs_DrawResults(void) {
    SFVsRank ranks[MAX_PLAYERS];
    char text[48];
    s32 count;
    s32 i;
    s32 y;
    s32 x0 = 40;

    Graphics_FillRectangle(&gMasterDisp, OTRGetRectDimensionFromLeftEdge(0), 24,
                           OTRGetRectDimensionFromRightEdge(SCREEN_WIDTH), 216, 0, 0, 0, 150);

    if (sWinner == SFVS_NO_WINNER) {
        SFModVs_TextColor(255, 255, 255, 255);
        snprintf(text, sizeof(text), "DRAW");
    } else if (sWinnerIsTeam) {
        const u8* c = sTeamColors[sWinner];

        SFModVs_TextColor(c[0], c[1], c[2], 255);
        snprintf(text, sizeof(text), "TEAM %s WINS", sTeamNames[sWinner]);
    } else {
        SFModVs_PilotTextColor(sWinner, 255);
        snprintf(text, sizeof(text), "%s WINS", SFModVs_PilotName(sWinner));
    }
    SFModVs_LargeTextCentered(34, 1.0f, text);

    SFModVs_TextColor(200, 200, 200, 255);
    SFModVs_Text(x0, 62, 0.9f, "PILOT");
    SFModVs_Text(x0 + 120, 62, 0.9f, "KILLS");
    SFModVs_Text(x0 + 175, 62, 0.9f, (gSFVs.rule == SFVS_RULE_SURVIVAL) ? "LIVES" : "DEATHS");

    count = SFModVs_GetPilotRanks(ranks);
    if (gSFVs.rule != SFVS_RULE_SURVIVAL) {
        for (i = 0; i < count; i++) {
            ranks[i].score = sKills[ranks[i].index] * 1000 - sDeaths[ranks[i].index];
        }
        SFModVs_SortRanks(ranks, count);
    }
    y = 74;
    for (i = 0; i < count; i++) {
        s32 p = ranks[i].index;

        SFModVs_PilotTextColor(p, 255);
        snprintf(text, sizeof(text), "%d %s", i + 1, SFModVs_PilotName(p));
        SFModVs_Text(x0, y, 0.9f, text);
        snprintf(text, sizeof(text), "%d", sKills[p]);
        SFModVs_Text(x0 + 128, y, 0.9f, text);
        snprintf(text, sizeof(text), "%d", (gSFVs.rule == SFVS_RULE_SURVIVAL) ? sLives[p] : sDeaths[p]);
        SFModVs_Text(x0 + 185, y, 0.9f, text);
        y += 11;
    }

    if ((sTimer > 90) && (gGameFrameCount & 16)) {
        SFModVs_TextColor(255, 255, 0, 255);
        SFModVs_TextCentered(200, 1.0f, "A REMATCH   B MENU");
    }
}

// Drawn once per frame on top of everything (replaces Versus_Draw).
void SFModVs_Draw(void) {
    if (!SFModVs_Active() || (gPlayer == NULL)) {
        return;
    }
    gDPSetTextureFilter(gMasterDisp++, G_TF_POINT);

    SFModVs_DrawCountdown();
    if (sState == SFVS_STATE_FIGHT) {
        SFModVs_DrawFeed();

        if (!gSFVs.humanPlays || sOut[0]) {
            char text[48];
            s32 p = gSFVs.humanPlays ? (gPlayer[0].attacker - 1) : sViewed;

            if ((p >= 0) && (p < gCamCount)) {
                if (SFModVs_IsHuman(p)) {
                    snprintf(text, sizeof(text), "WATCHING %s", SFModVs_PilotName(p));
                } else {
                    snprintf(text, sizeof(text), "WATCHING %s  LV%d %s", SFModVs_PilotName(p), gSFVs.pilots[p].level,
                             SFAI_GetPersonality(SFAI_GetPersonalityIndex(p))->name);
                }
                SFModVs_PilotTextColor(p, 255);
                SFModVs_TextCentered(206, 0.9f, text);
                SFModVs_TextColor(200, 200, 200, 255);
                SFModVs_TextCentered(218, 0.8f, "Z R SWITCH PILOT");
            }
        } else if (gPlayer[0].state == PLAYERSTATE_VS_STANDBY) {
            SFModVs_TextColor(255, 255, 255, 255);
            SFModVs_TextCentered(200, 1.0f, "GET READY");
        }

        // somebody has locked on to the viewed pilot
        {
            s32 j;
            s32 p = SFModVs_ViewedPlayer();

            for (j = 0; j < gCamCount; j++) {
                if ((j != p) && (gVsLockOnTimers[p][j] != 0) && SFModVs_Alive(p)) {
                    if (gGameFrameCount & 4) {
                        SFModVs_TextColor(255, 48, 48, 255);
                        SFModVs_TextCentered(150, 1.0f, "LOCKED ON");
                    }
                    break;
                }
            }
        }
    }
    if ((sState == SFVS_STATE_END) && (sTimer >= 50)) {
        SFModVs_DrawResults();
    } else if (sState == SFVS_STATE_END) {
        SFModVs_TextColor(255, 255, 255, 255);
        SFModVs_LargeTextCentered(100, 1.0f, (gSFVs.rule == SFVS_RULE_TIME) ? "TIME UP" : "FINISH");
    }

    if (gPlayState == PLAY_PAUSE) {
        SFModVs_TextColor(255, 255, 255, 255);
        SFModVs_LargeTextCentered(96, 1.0f, "PAUSE");
        SFModVs_TextColor(255, 255, 0, 255);
        SFModVs_TextCentered(124, 1.0f, "START RESUME   B QUIT");
    }

    SFMod_DrawSwapMessage();
    gDPSetTextureFilter(gMasterDisp++, G_TF_BILERP);
}

/* ------------------------------------------------------------------------------------------------
 * Setup screen (VS card of the main menu)
 * ------------------------------------------------------------------------------------------------ */

typedef enum {
    MENU_ROW_PILOTS,
    MENU_ROW_HUMAN,
    MENU_ROW_STAGE,
    MENU_ROW_RULE,
    MENU_ROW_TARGET,
    MENU_ROW_TEAMS,
    MENU_ROW_ALL_LEVELS,
    MENU_ROW_FIRST_PILOT,
} SFVsMenuRow;

static bool sMenuOpen = false;
static s32 sMenuRow;
static s32 sMenuColumn; // pilot rows: 0 character, 1 level, 2 personality, 3 team
static s32 sMenuRepeatTimer;
static s32 sMenuRepeatDir;
static s32 sMenuRepeatTimerX;
static s32 sMenuRepeatDirX;
static s32 sMenuStarting;

static s32 SFModVs_MenuRowCount(void) {
    return MENU_ROW_FIRST_PILOT + gSFVs.numPilots + 1; // + START
}

static s32 SFModVs_MenuStartRow(void) {
    return MENU_ROW_FIRST_PILOT + gSFVs.numPilots;
}

static s32 SFModVs_Wrap(s32 v, s32 lo, s32 hi) {
    if (v < lo) {
        return hi;
    }
    if (v > hi) {
        return lo;
    }
    return v;
}

static s32 SFModVs_AllLevels(void) {
    s32 i;
    s32 level = -1;

    for (i = 0; i < gSFVs.numPilots; i++) {
        if (gSFVs.humanPlays && (i == 0)) {
            continue;
        }
        if (level < 0) {
            level = gSFVs.pilots[i].level;
        } else if (level != gSFVs.pilots[i].level) {
            return 0; // mixed
        }
    }
    return (level < 0) ? 4 : level;
}

static s32 SFModVs_TargetMax(void) {
    switch (gSFVs.rule) {
        case SFVS_RULE_TIME:
            return 10;
        case SFVS_RULE_SURVIVAL:
            return 9;
        default:
            return 20;
    }
}

static bool SFModVs_PilotColumnUsed(s32 pilot, s32 column) {
    if (gSFVs.humanPlays && (pilot == 0) && ((column == 1) || (column == 2))) {
        return false; // the human has no CPU level/personality
    }
    if ((column == 3) && !gSFVs.teams) {
        return false;
    }
    return true;
}

static void SFModVs_MenuChange(s32 dir) {
    s32 row = sMenuRow;

    switch (row) {
        case MENU_ROW_PILOTS:
            gSFVs.numPilots = SFModVs_Wrap(gSFVs.numPilots + dir, 2, MAX_PLAYERS);
            break;
        case MENU_ROW_HUMAN:
            gSFVs.humanPlays = !gSFVs.humanPlays;
            break;
        case MENU_ROW_STAGE:
            gSFVs.stage = SFModVs_Wrap(gSFVs.stage + dir, 0, 2);
            break;
        case MENU_ROW_RULE:
            gSFVs.rule = SFModVs_Wrap(gSFVs.rule + dir, 0, SFVS_RULE_COUNT - 1);
            gSFVs.target = CLAMP(gSFVs.target, 1, SFModVs_TargetMax());
            if (gSFVs.rule == SFVS_RULE_TIME) {
                gSFVs.target = CLAMP(gSFVs.target, 1, 10);
            }
            break;
        case MENU_ROW_TARGET:
            gSFVs.target = SFModVs_Wrap(gSFVs.target + dir, 1, SFModVs_TargetMax());
            break;
        case MENU_ROW_TEAMS:
            gSFVs.teams = !gSFVs.teams;
            break;
        case MENU_ROW_ALL_LEVELS: {
            s32 level = SFModVs_AllLevels();
            s32 i;

            level = (level == 0) ? 4 : SFModVs_Wrap(level + dir, 1, 7);
            for (i = 0; i < MAX_PLAYERS; i++) {
                gSFVs.pilots[i].level = level;
            }
            break;
        }
        default:
            if ((row >= MENU_ROW_FIRST_PILOT) && (row < SFModVs_MenuStartRow())) {
                SFVsPilot* p = &gSFVs.pilots[row - MENU_ROW_FIRST_PILOT];

                switch (sMenuColumn) {
                    case 0:
                        p->character = SFModVs_Wrap(p->character + dir, 0, SFVS_CHARACTER_COUNT - 1);
                        break;
                    case 1:
                        p->level = SFModVs_Wrap(p->level + dir, 1, 7);
                        break;
                    case 2:
                        p->personality = SFModVs_Wrap(p->personality + dir, -1, SFAI_PersonalityCount() - 1);
                        break;
                    case 3:
                        p->team = SFModVs_Wrap(p->team + dir, 1, SFVS_TEAM_COUNT);
                        break;
                }
            }
            return;
    }
}

static s32 SFModVs_MenuAxis(s32 stick, u16 button, u16 negButton, u16 posButton, s32* dir, s32* timer) {
    s32 d = 0;

    if ((stick < -30) || (button & negButton)) {
        d = -1;
    } else if ((stick > 30) || (button & posButton)) {
        d = 1;
    }
    if (d == 0) {
        *dir = 0;
        *timer = 0;
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

static void SFModVs_MenuStart(void) {
    SFModVs_SaveConfig();
    SFModVs_PrepareMatch();
    sMenuOpen = false;
    sMenuStarting = 0;
    SEQCMD_STOP_SEQUENCE(SEQ_PLAYER_BGM, 1);
    Option_SFMod_StartVersus();
}

void SFModVs_MenuUpdate(void) {
    OSContPad* hold = &gControllerHold[gMainController];
    OSContPad* press = &gControllerPress[gMainController];
    s32 dy;
    s32 dx;

    if (!sMenuOpen) {
        SFModVs_LoadConfig();
        sMenuOpen = true;
        sMenuRow = 0;
        sMenuColumn = 0;
        sMenuRepeatDir = sMenuRepeatDirX = 0;
        sMenuStarting = 0;
        gStarCount = 800;
        gBlurAlpha = 255;
        return;
    }

    if (sMenuStarting > 0) {
        if (--sMenuStarting == 0) {
            SFModVs_MenuStart();
        }
        return;
    }

    dy = SFModVs_MenuAxis(-press->stick_y, hold->button, U_JPAD, D_JPAD, &sMenuRepeatDir, &sMenuRepeatTimer);
    dx = SFModVs_MenuAxis(press->stick_x, hold->button, L_JPAD, R_JPAD, &sMenuRepeatDirX, &sMenuRepeatTimerX);

    if (dy != 0) {
        sMenuRow = SFModVs_Wrap(sMenuRow + dy, 0, SFModVs_MenuRowCount() - 1);
        AUDIO_PLAY_SFX(NA_SE_CURSOR, gDefaultSfxSource, 4);
    }

    if ((sMenuRow >= MENU_ROW_FIRST_PILOT) && (sMenuRow < SFModVs_MenuStartRow())) {
        s32 pilot = sMenuRow - MENU_ROW_FIRST_PILOT;
        s32 step = 0;
        s32 tries;

        if (press->button & (A_BUTTON | R_TRIG)) {
            step = 1;
        } else if (press->button & Z_TRIG) {
            step = -1;
        }
        if (step != 0) {
            AUDIO_PLAY_SFX(NA_SE_CURSOR, gDefaultSfxSource, 4);
            sMenuColumn = (sMenuColumn + step + 4) % 4;
        }
        // skip fields that don't apply to this pilot
        for (tries = 0; (tries < 4) && !SFModVs_PilotColumnUsed(pilot, sMenuColumn); tries++) {
            sMenuColumn = (sMenuColumn + ((step < 0) ? 3 : 1)) % 4;
        }
    }

    if (dx != 0) {
        SFModVs_MenuChange(dx);
        AUDIO_PLAY_SFX(NA_SE_ARWING_CURSOR, gDefaultSfxSource, 4);
        sMenuRow = CLAMP(sMenuRow, 0, SFModVs_MenuRowCount() - 1);
    }
    if ((press->button & A_BUTTON) && (sMenuRow < MENU_ROW_FIRST_PILOT)) {
        SFModVs_MenuChange(1);
        AUDIO_PLAY_SFX(NA_SE_ARWING_CURSOR, gDefaultSfxSource, 4);
    }

    if ((press->button & START_BUTTON) || ((press->button & A_BUTTON) && (sMenuRow == SFModVs_MenuStartRow()))) {
        AUDIO_PLAY_SFX(NA_SE_ARWING_DECIDE, gDefaultSfxSource, 4);
        sMenuStarting = 15;
        return;
    }

    if (press->button & B_BUTTON) {
        SFModVs_SaveConfig();
        sMenuOpen = false;
        Option_SFMod_ReturnToMainMenu();
    }
}

static void SFModVs_MenuRowColor(bool selected, bool field) {
    if (selected && field) {
        SFModVs_TextColor(255, 255, 0, 255);
    } else if (selected) {
        SFModVs_TextColor(255, 255, 160, 255);
    } else {
        SFModVs_TextColor(200, 200, 200, 255);
    }
}

void SFModVs_MenuDraw(void) {
    char text[48];
    s32 y;
    s32 i;
    const s32 labelX = 40;
    const s32 valueX = 168;
    const s32 lineH = 10;
    const f32 scale = 0.9f;

    if (!sMenuOpen) {
        return;
    }

    gDPSetTextureFilter(gMasterDisp++, G_TF_POINT);

    Graphics_FillRectangle(&gMasterDisp, 8, 6, SCREEN_WIDTH - 8, SCREEN_HEIGHT - 6, 0, 0, 0, 150);

    SFModVs_TextColor(255, 255, 255, 255);
    SFModVs_LargeTextCentered(12, 0.8f, "VS CPU BATTLE");

    y = 32;
    for (i = 0; i < MENU_ROW_FIRST_PILOT; i++) {
        const char* label = "";
        bool selected = (sMenuRow == i);

        text[0] = '\0';
        switch (i) {
            case MENU_ROW_PILOTS:
                label = "PILOTS";
                snprintf(text, sizeof(text), "%d", gSFVs.numPilots);
                break;
            case MENU_ROW_HUMAN:
                label = "PILOT 1";
                snprintf(text, sizeof(text), "%s", gSFVs.humanPlays ? "YOU" : "CPU - JUST WATCH");
                break;
            case MENU_ROW_STAGE:
                label = "STAGE";
                snprintf(text, sizeof(text), "%s", sStageNames[gSFVs.stage]);
                break;
            case MENU_ROW_RULE:
                label = "RULE";
                snprintf(text, sizeof(text), "%s", sRuleNames[gSFVs.rule]);
                break;
            case MENU_ROW_TARGET:
                label = (gSFVs.rule == SFVS_RULE_TIME)       ? "MINUTES"
                        : (gSFVs.rule == SFVS_RULE_SURVIVAL) ? "LIVES"
                                                             : "KILLS TO WIN";
                snprintf(text, sizeof(text), "%d", gSFVs.target);
                break;
            case MENU_ROW_TEAMS:
                label = "TEAMS";
                snprintf(text, sizeof(text), "%s", gSFVs.teams ? "ON" : "OFF");
                break;
            case MENU_ROW_ALL_LEVELS: {
                s32 level = SFModVs_AllLevels();

                label = "ALL CPU LEVELS";
                if (level == 0) {
                    snprintf(text, sizeof(text), "MIXED");
                } else {
                    snprintf(text, sizeof(text), "%d %s", level, sLevelNames[level - 1]);
                }
                break;
            }
        }
        SFModVs_MenuRowColor(selected, false);
        SFModVs_Text(labelX, y, scale, label);
        SFModVs_MenuRowColor(selected, true);
        SFModVs_Text(valueX, y, scale, text);
        y += lineH;
    }

    y += 3;
    for (i = 0; i < gSFVs.numPilots; i++) {
        SFVsPilot* p = &gSFVs.pilots[i];
        bool selected = (sMenuRow == MENU_ROW_FIRST_PILOT + i);
        bool isHuman = gSFVs.humanPlays && (i == 0);
        u8 r, g, b;
        const u8* c = gSFVs.teams ? sTeamColors[p->team] : sPilotColors[i];

        r = c[0];
        g = c[1];
        b = c[2];
        SFModVs_TextColor(r, g, b, 255);
        snprintf(text, sizeof(text), "%d", i + 1);
        SFModVs_Text(labelX - 12 - ((i >= 9) ? 6 : 0), y, scale, text);

        SFModVs_MenuRowColor(selected, sMenuColumn == 0);
        SFModVs_Text(labelX, y, scale, SFModVs_CharacterName(p->character));

        if (isHuman) {
            SFModVs_MenuRowColor(selected, false);
            SFModVs_Text(labelX + 60, y, scale, "YOU");
        } else {
            SFModVs_MenuRowColor(selected, sMenuColumn == 1);
            snprintf(text, sizeof(text), "%d %s", p->level, sLevelNames[p->level - 1]);
            SFModVs_Text(labelX + 60, y, scale, text);
            SFModVs_MenuRowColor(selected, sMenuColumn == 2);
            SFModVs_Text(labelX + 150, y, scale,
                         (p->personality < 0) ? "RANDOM" : SFAI_GetPersonality(p->personality)->name);
        }
        if (gSFVs.teams) {
            SFModVs_MenuRowColor(selected, sMenuColumn == 3);
            if (selected && (sMenuColumn == 3)) {
                SFModVs_TextColor(r, g, b, 255);
            }
            SFModVs_Text(labelX + 228, y, scale, sTeamNames[p->team]);
        }
        y += lineH;
    }

    y += 4;
    SFModVs_MenuRowColor(sMenuRow == SFModVs_MenuStartRow(), true);
    if ((sMenuRow != SFModVs_MenuStartRow()) || (gGameFrameCount & 8) || (sMenuStarting > 0)) {
        SFModVs_TextCentered(y, 1.0f, "START MATCH");
    }

    SFModVs_TextColor(150, 150, 150, 255);
    if ((sMenuRow >= MENU_ROW_FIRST_PILOT) && (sMenuRow < SFModVs_MenuStartRow())) {
        SFModVs_TextCentered(SCREEN_HEIGHT - 20, 0.75f, "LEFT RIGHT CHANGE   A Z R FIELD   B BACK");
    } else {
        SFModVs_TextCentered(SCREEN_HEIGHT - 20, 0.75f, "LEFT RIGHT CHANGE   START BEGIN   B BACK");
    }

    gDPSetTextureFilter(gMasterDisp++, G_TF_BILERP);
}

/* ------------------------------------------------------------------------------------------------
 * Automated testing: SFMOD_TEST_VS=<pilots> starts a match right after booting.
 *   SFMOD_TEST_VS_CPU=1      only CPUs fight
 *   SFMOD_TEST_VS_RULE=<0-2> points, time, survival
 *   SFMOD_TEST_VS_TARGET=<n> SFMOD_TEST_VS_TEAMS=1 SFMOD_TEST_VS_STAGE=<0-2> SFMOD_TEST_VS_LEVEL=<1-7>
 * ------------------------------------------------------------------------------------------------ */

bool SFModVs_TestBoot(void) {
    const char* pilots = getenv("SFMOD_TEST_VS");
    const char* v;
    s32 i;

    sDebug = getenv("SFMOD_DEBUG") != NULL;
    if (pilots == NULL) {
        return false;
    }
    gSFVs.numPilots = CLAMP(atoi(pilots), 2, MAX_PLAYERS);
    gSFVs.humanPlays = getenv("SFMOD_TEST_VS_CPU") == NULL;
    if ((v = getenv("SFMOD_TEST_VS_RULE")) != NULL) {
        gSFVs.rule = CLAMP(atoi(v), 0, SFVS_RULE_COUNT - 1);
    }
    if ((v = getenv("SFMOD_TEST_VS_TARGET")) != NULL) {
        gSFVs.target = CLAMP(atoi(v), 1, 20);
    }
    if ((v = getenv("SFMOD_TEST_VS_STAGE")) != NULL) {
        gSFVs.stage = CLAMP(atoi(v), 0, 2);
    }
    gSFVs.teams = getenv("SFMOD_TEST_VS_TEAMS") != NULL;
    if ((v = getenv("SFMOD_TEST_VS_LEVEL")) != NULL) {
        for (i = 0; i < MAX_PLAYERS; i++) {
            gSFVs.pilots[i].level = CLAMP(atoi(v), 1, 7);
        }
    }
    SFModVs_PrepareMatch();
    gNextGameState = GSTATE_VS_INIT;
    gOptionMenuStatus = OPTION_WAIT;
    printf("[SFMod] test boot into a VS CPU match with %d pilots\n", gSFVs.numPilots);
    fflush(stdout);
    return true;
}
