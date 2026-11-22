#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "SFModVs.h"
#include "SFModAI.h"
#include "SFMod.h"
#include "global.h"
#include "assets/ast_common.h"
#include "assets/ast_radio.h"
#include "port/interpolation/FrameInterpolation.h"
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
    true, // oshaberi: the CPU pilots talk
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
    gSFVs.oshaberi = CVarGetInteger(CVAR_SFVS_PREFIX "Oshaberi", gSFVs.oshaberi) != 0;
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
    CVarSetInteger(CVAR_SFVS_PREFIX "Oshaberi", gSFVs.oshaberi);
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
static u32 sHumanPrevHold;
static u32 sSpectatorPress;    // buttons pressed on the human controller this frame
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

static s32 sSfxMuted = 0; // personal sounds of other pilots left out (debug statistics)

bool SFModVs_HearsSfx(int32_t playerNum, uint32_t sfxId) {
    if (!SFModVs_Active() || (playerNum == SFModVs_ViewedPlayer())) {
        return true;
    }
    switch (sfxId) {
        case NA_SE_DAMAGE_S:
        case NA_SE_DAMAGE_L:
        case NA_SE_SHIELD_WARNING0:
        case NA_SE_SHIELD_WARNING1:
        case NA_SE_SHIELD_BUZZER:
        case NA_SE_LOCK_SEARCH:
        case NA_SE_LOCK_ON:
        case NA_SE_ONE_UP:
        case NA_SE_BOMB_GET:
        case NA_SE_TWIN_LASER_GET:
        case NA_SE_SHIELD_RING:
        case NA_SE_SHIELD_RING_M:
        case NA_SE_GOLD_RING:
            sSfxMuted++;
            return false;
        default:
            return true;
    }
}

// The original VS mode ignores every laser hit for 20 frames after a hit, so most hits of a fast
// shooter (turbo fire) do nothing. In the VS CPU mode a laser hit counts once 6 frames have passed
// since the last one, like the hits on Star Wolf in the missions.
bool SFModVs_LaserHitCounts(int32_t hitTimer) {
    return SFModVs_Active() && (hitTimer <= 20 - 6);
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

// Testing switches (environment variables), looked up once
static bool SFModVs_TestFlag(const char* name) {
    static const char* sNames[4];
    static bool sValues[4];
    s32 i;

    for (i = 0; i < ARRAY_COUNT(sNames); i++) {
        if (sNames[i] == name) {
            return sValues[i];
        }
        if (sNames[i] == NULL) {
            sNames[i] = name;
            sValues[i] = getenv(name) != NULL;
            return sValues[i];
        }
    }
    return getenv(name) != NULL;
}

// Hits on Star Wolf in the missions play the enemy damage sound at the enemy. The VS mode only has the
// hit pilot's own damage sound, which is only heard for the watched pilot (SFModVs_HearsSfx).
static s32 sHitSfx = 0; // debug statistics

void SFModVs_OnPilotDamaged(int32_t playerNum) {
    if (SFModVs_Active() && (playerNum >= 0) && (playerNum < gCamCount) && (playerNum != SFModVs_ViewedPlayer())) {
        AUDIO_PLAY_SFX(NA_SE_EN_DAMAGE_S, gPlayer[playerNum].sfxSource, 4);
        sHitSfx++;
    }
}

/* ------------------------------------------------------------------------------------------------
 * Team marks: the arrows the story mode shows above Falco, Slippy and Peppy, here above the watched
 * pilot's teammates: an arrow in the team color with the pilot's initial, drawn like Display_ActorMarks.
 * ------------------------------------------------------------------------------------------------ */

static Matrix sViewMtx;
static bool sViewMtxSet = false;
static s32 sMarksDrawn = 0; // debug statistics

void SFModVs_SetViewMatrix(const void* mtx) {
    if (SFModVs_Active() && (mtx != NULL)) {
        memcpy(&sViewMtx, mtx, sizeof(Matrix));
        sViewMtxSet = true;
    }
}

// 5x5 letters like the ones of the story marks (bit 4 = left column)
static const struct {
    char letter;
    u8 rows[5];
} sMarkGlyphs[] = {
    { 'F', { 0x1F, 0x10, 0x1E, 0x10, 0x10 } }, { 'P', { 0x1F, 0x11, 0x1F, 0x10, 0x10 } },
    { 'S', { 0x1F, 0x10, 0x1F, 0x01, 0x1F } }, { 'W', { 0x11, 0x11, 0x15, 0x1B, 0x11 } },
    { 'A', { 0x0E, 0x11, 0x1F, 0x11, 0x11 } }, { 'L', { 0x10, 0x10, 0x10, 0x10, 0x1F } },
    { 'B', { 0x1E, 0x11, 0x1F, 0x11, 0x1E } }, { 'K', { 0x12, 0x14, 0x18, 0x14, 0x12 } },
};

#define SFVS_MARK_TEX 16
// RGBA16 textures (big endian like the N64's), one per team color and character
static u8 sMarkTex[SFVS_TEAM_COUNT + 1][SFVS_CHARACTER_COUNT][SFVS_MARK_TEX * SFVS_MARK_TEX * 2];
static bool sMarkTexReady[SFVS_TEAM_COUNT + 1][SFVS_CHARACTER_COUNT];

// the triangle of the story marks (pointing down), texture coordinates in 1/32 texels
static Vtx sMarkVtx[3] = {
    { { { 0, -107, 0 }, 0, { 240, 627 }, { 255, 255, 255, 255 } } },
    { { { 80, 53, 0 }, 0, { 496, 115 }, { 255, 255, 255, 255 } } },
    { { { -80, 53, 0 }, 0, { -15, 115 }, { 255, 255, 255, 255 } } },
};

static u16 SFModVs_Rgba16(u8 r, u8 g, u8 b) {
    return ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1;
}

static u8* SFModVs_MarkTexture(s32 team, s32 character) {
    enum { BG, OUTLINE, WHITE, GRAY };
    u8 kind[SFVS_MARK_TEX][SFVS_MARK_TEX];
    const u8* rows = sMarkGlyphs[0].rows;
    const u8* c;
    u8* tex;
    u16 colors[4];
    char letter;
    s32 x;
    s32 y;
    s32 i;

    team = CLAMP(team, 0, SFVS_TEAM_COUNT);
    character = CLAMP(character, 0, SFVS_CHARACTER_COUNT - 1);
    tex = sMarkTex[team][character];
    if (sMarkTexReady[team][character]) {
        return tex;
    }

    letter = SFModVs_CharacterName(character)[0];
    for (i = 0; i < ARRAY_COUNT(sMarkGlyphs); i++) {
        if (sMarkGlyphs[i].letter == letter) {
            rows = sMarkGlyphs[i].rows;
        }
    }
    memset(kind, BG, sizeof(kind));
    // the letter (white) at 5..9 with a gray shadow on the right of every run
    for (y = 0; y < 5; y++) {
        for (x = 0; x < 5; x++) {
            if (rows[y] & (0x10 >> x)) {
                kind[5 + y][5 + x] = WHITE;
            }
        }
        for (x = 0; x < 5; x++) {
            if ((rows[y] & (0x10 >> x)) && ((x == 4) || !(rows[y] & (0x10 >> (x + 1))))) {
                kind[5 + y][6 + x] = GRAY;
            }
        }
    }
    // a black outline around it
    for (y = 1; y < SFVS_MARK_TEX - 1; y++) {
        for (x = 1; x < SFVS_MARK_TEX - 1; x++) {
            s32 dx;
            s32 dy;

            if (kind[y][x] != BG) {
                continue;
            }
            for (dy = -1; dy <= 1; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    if ((kind[y + dy][x + dx] == WHITE) || (kind[y + dy][x + dx] == GRAY)) {
                        kind[y][x] = OUTLINE;
                    }
                }
            }
        }
    }

    c = sTeamColors[team];
    colors[BG] = SFModVs_Rgba16(c[0], c[1], c[2]);
    colors[OUTLINE] = SFModVs_Rgba16(0, 0, 0);
    colors[WHITE] = SFModVs_Rgba16(248, 248, 248);
    colors[GRAY] = SFModVs_Rgba16(136, 136, 136);
    for (y = 0; y < SFVS_MARK_TEX; y++) {
        for (x = 0; x < SFVS_MARK_TEX; x++) {
            u16 col = colors[kind[y][x]];

            tex[(y * SFVS_MARK_TEX + x) * 2] = col >> 8;
            tex[(y * SFVS_MARK_TEX + x) * 2 + 1] = col & 0xFF;
        }
    }
    sMarkTexReady[team][character] = true;
    return tex;
}

void SFModVs_DrawTeamMarks(void) {
    s32 viewed = SFModVs_ViewedPlayer();
    s32 i;

    if (!SFModVs_Active() || !gSFVs.teams || !sViewMtxSet || (gPlayer == NULL) || (sState != SFVS_STATE_FIGHT) ||
        (((gGameFrameCount & 4) != 0) && !SFModVs_TestFlag("SFMOD_TEST_VS_MARKS_SOLID"))) { // they blink like this
        return;
    }

    RCP_SetupDL_40();
    gDPSetTextureFilter(gMasterDisp++, G_TF_POINT);
    for (i = 0; i < gCamCount; i++) {
        Player* pl = &gPlayer[i];
        Vec3f world;
        Vec3f view;
        f32 scale;

        if ((i == viewed) || !SFModVs_SameTeam(i, viewed) || (pl->state != PLAYERSTATE_ACTIVE && pl->state != PLAYERSTATE_U_TURN)) {
            continue;
        }
        world.x = pl->pos.x;
        world.y = pl->pos.y;
        world.z = pl->trueZpos + gPathProgress;
        Matrix_MultVec3f(&sViewMtx, &world, &view);
        if ((view.z > -300.0f) || (view.z < -12000.0f) || (fabsf(view.x) > -view.z * 1.6f) ||
            (fabsf(view.y) > -view.z)) {
            continue; // behind or right next to the camera, too far away, or out of the picture
        }
        scale = sqrtf(SQ(view.x) + SQ(view.y) + SQ(view.z)) * 0.0015f;
        scale = CLAMP(scale, 1.0f, 100.0f);

        sMarksDrawn++;
        FrameInterpolation_RecordOpenChild("SFVsTeamMark", i);
        Matrix_Push(&gGfxMatrix);
        Matrix_Translate(gGfxMatrix, view.x, view.y, view.z, MTXF_APPLY);
        Matrix_Scale(gGfxMatrix, scale * 0.25f, scale * 0.25f, 1.0f, MTXF_APPLY);
        Matrix_Translate(gGfxMatrix, 0.0f, 150.0f, 0.0f, MTXF_APPLY);
        Matrix_SetGfxMtx(&gMasterDisp);
        gDPSetTextureLUT(gMasterDisp++, G_TT_NONE);
        gDPLoadTextureBlock(gMasterDisp++, SFModVs_MarkTexture(gSFVs.pilots[i].team, gSFVs.pilots[i].character),
                            G_IM_FMT_RGBA, G_IM_SIZ_16b, SFVS_MARK_TEX, SFVS_MARK_TEX, 0, G_TX_NOMIRROR | G_TX_CLAMP,
                            G_TX_NOMIRROR | G_TX_CLAMP, 4, 4, G_TX_NOLOD, G_TX_NOLOD);
        gSPVertex(gMasterDisp++, sMarkVtx, 3, 0);
        gSP1Triangle(gMasterDisp++, 0, 1, 2, 0);
        Matrix_Pop(&gGfxMatrix);
        FrameInterpolation_RecordCloseChild();
    }
    gDPSetTextureFilter(gMasterDisp++, G_TF_BILERP);
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
    u32 humanPress;
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

/* ------------------------------------------------------------------------------------------------
 * Oshaberi: the CPU pilots talk on the radio during the match, in character, with the lines they
 * say in the missions (taunts, calls for help, being shot down ...). Lines that name somebody are
 * only used when that one is meant (Fox as the target, Fox as a teammate, ...).
 * ------------------------------------------------------------------------------------------------ */

typedef enum {
    TALK_START,  // the match begins
    TALK_ATTACK, // on somebody's tail
    TALK_CHASED, // somebody on the own tail
    TALK_KILL,   // shot somebody down
    TALK_DOWN,   // was shot down
    TALK_HIT,    // took a hit
    TALK_SAVED,  // a teammate shot down the one on the own tail
    TALK_WARN,   // a teammate (Fox) has somebody on his tail
    TALK_NICE,   // a teammate shot somebody down
} SFVsTalkEvent;

// what a line needs
#define TN_OTHER_FOX (1 << 0)     // the other pilot of the event is Fox
#define TN_OTHER_FALCO (1 << 1)   // ... Falco
#define TN_OTHER_PEPPY (1 << 2)   // ... Peppy
#define TN_OTHER_SLIPPY (1 << 3)  // ... Slippy
#define TN_OTHER_STARFOX (1 << 4) // ... one of Fox, Falco, Peppy, Slippy
#define TN_OTHER_MALE (1 << 5)    // ... not Katt
#define TN_ALLY_FOX (1 << 6)      // Fox flies in the speaker's team
#define TN_FOE_STARFOX (1 << 7)   // the speaker fights one of Fox, Falco, Peppy, Slippy
#define TN_FOE_STARWOLF (1 << 8)  // ... one of Wolf, Leon, Pigma, Andrew
#define TN_FOE_PEPPY (1 << 9)     // ... Peppy
#define TN_FOX_THERE (1 << 10)    // Fox flies in this match
#define TN_TEAMMATES (1 << 11)    // the speaker has teammates left
#define TN_LAST (1 << 12)         // no opponent of the speaker left (survival)
#define TN_ONE_LEFT (1 << 13)     // one opponent left (survival)
#define TN_OUT (1 << 14)          // the speaker is out of the match (survival)
#define TN_PERSONAL (TN_OTHER_FOX | TN_OTHER_FALCO | TN_OTHER_PEPPY | TN_OTHER_SLIPPY)

typedef struct {
    u8 character; // SFVsCharacter
    u8 event;     // SFVsTalkEvent
    u16 needs;
    char* msg;
} SFVsLine;

// clang-format off
static const SFVsLine sLines[] = {
    // Fox
    { SFVS_FOX, TALK_START, 0, gMsg_ID_2050 },                       // I see 'em up ahead! Let's rock and roll!
    { SFVS_FOX, TALK_START, TN_FOE_STARWOLF, gMsg_ID_9285 },         // Just what I need to see. Star Wolf.
    { SFVS_FOX, TALK_START, TN_FOE_STARWOLF, gMsg_ID_19240 },        // We'll just see about that, Star Wolf.
    { SFVS_FOX, TALK_CHASED, TN_TEAMMATES, gMsg_ID_9380 },           // Geez! Can anyone take care of it?
    { SFVS_FOX, TALK_KILL, TN_ONE_LEFT, gMsg_ID_19465 },             // There's one more to go.
    { SFVS_FOX, TALK_DOWN, 0, gMsg_ID_20318 },                       // Aaaaahhh!
    { SFVS_FOX, TALK_DOWN, 0, gMsg_ID_18080 },                       // Dang........
    // Falco
    { SFVS_FALCO, TALK_START, 0, gMsg_ID_17320 },                    // Now it's our turn to kick some tail!
    { SFVS_FALCO, TALK_START, 0, gMsg_ID_18035 },                    // Here they come!
    { SFVS_FALCO, TALK_START, TN_ALLY_FOX, gMsg_ID_20298 },          // Come on, Fox. Let's kick some tail.
    { SFVS_FALCO, TALK_START, TN_ALLY_FOX, gMsg_ID_4094 },           // Let me get a piece of that action, Fox!
    { SFVS_FALCO, TALK_ATTACK, 0, gMsg_ID_9190 },                    // Time for a little payback!
    { SFVS_FALCO, TALK_ATTACK, 0, gMsg_ID_9211 },                    // You're dead meat, pal!
    { SFVS_FALCO, TALK_ATTACK, 0, gMsg_ID_20309 },                   // You're not leavin', yet!
    { SFVS_FALCO, TALK_CHASED, TN_OTHER_MALE, gMsg_ID_9100 },        // Shoot! He's right behind me!
    { SFVS_FALCO, TALK_CHASED, TN_OTHER_MALE, gMsg_ID_9130 },        // I can't shake this guy!
    { SFVS_FALCO, TALK_KILL, 0, gMsg_ID_9220 },                      // Scratch one bogey!
    { SFVS_FALCO, TALK_KILL, 0, gMsg_ID_7011 },                      // Too late! Game over, pal!
    { SFVS_FALCO, TALK_KILL, TN_ONE_LEFT, gMsg_ID_5504 },            // YEAH! Just one more.
    { SFVS_FALCO, TALK_KILL, TN_LAST, gMsg_ID_9438 },                // I got the last one!
    { SFVS_FALCO, TALK_DOWN, 0, gMsg_ID_20090 },                     // No!!!!
    { SFVS_FALCO, TALK_DOWN, 0, gMsg_ID_9151 },                      // Dang, I'm hit!
    { SFVS_FALCO, TALK_DOWN, TN_OUT, gMsg_ID_20220 },                // Oh man! I'm gonna have to back off.
    { SFVS_FALCO, TALK_HIT, 0, gMsg_ID_20310 },                      // Crud!
    { SFVS_FALCO, TALK_HIT, 0, gMsg_ID_20030 },                      // Shoot! I've taken damage!
    { SFVS_FALCO, TALK_SAVED, TN_OTHER_FOX, gMsg_ID_9160 },          // Gee, I've been saved by Fox. How swell.
    { SFVS_FALCO, TALK_SAVED, TN_OTHER_FOX, gMsg_ID_9430 },          // That was a close call, Fox!
    { SFVS_FALCO, TALK_WARN, TN_OTHER_FOX, gMsg_ID_2233 },           // Move it, Fox! He's right behind you!
    { SFVS_FALCO, TALK_WARN, TN_OTHER_FOX, gMsg_ID_20315 },          // Watch out, Fox!
    { SFVS_FALCO, TALK_NICE, TN_OTHER_FOX, gMsg_ID_20017 },          // Ah, you're getting better, Fox.
    { SFVS_FALCO, TALK_NICE, TN_OTHER_FOX, gMsg_ID_8320 },           // OK, I'll admit it. You did good, Fox.
    { SFVS_FALCO, TALK_NICE, 0, gMsg_ID_20262 },                     // I guess you're good for something.
    // Slippy
    { SFVS_SLIPPY, TALK_START, 0, gMsg_ID_3005 },                    // Things are starting to heat up!
    { SFVS_SLIPPY, TALK_START, TN_ALLY_FOX, gMsg_ID_20300 },         // Let's take it to 'em, Fox.
    { SFVS_SLIPPY, TALK_START, TN_TEAMMATES, gMsg_ID_16280 },        // Don't worry. Slippy's here!
    { SFVS_SLIPPY, TALK_ATTACK, 0, gMsg_ID_9200 },                   // You're not gettin' away that easy!
    { SFVS_SLIPPY, TALK_ATTACK, 0, gMsg_ID_9212 },                   // I've got you now!
    { SFVS_SLIPPY, TALK_ATTACK, 0, gMsg_ID_11040 },                  // Your carcass is mine!
    { SFVS_SLIPPY, TALK_CHASED, TN_ALLY_FOX, gMsg_ID_9110 },         // Fox, get this guy off me!
    { SFVS_SLIPPY, TALK_CHASED, 0, gMsg_ID_20013 },                  // I'm havin' some trouble here!
    { SFVS_SLIPPY, TALK_CHASED, 0, gMsg_ID_9140 },                   // This is really starting to tick me off!
    { SFVS_SLIPPY, TALK_KILL, 0, gMsg_ID_9230 },                     // I got one!
    { SFVS_SLIPPY, TALK_KILL, 0, gMsg_ID_7086 },                     // Yeah! Yeah!
    { SFVS_SLIPPY, TALK_KILL, TN_LAST, gMsg_ID_9436 },               // I got the last one!
    { SFVS_SLIPPY, TALK_DOWN, 0, gMsg_ID_20092 },                    // No!!!!
    { SFVS_SLIPPY, TALK_DOWN, 0, gMsg_ID_9152 },                     // Ahhh!! I'm hit!
    { SFVS_SLIPPY, TALK_DOWN, TN_OUT, gMsg_ID_20222 },               // I'm monkey food if I don't leave!
    { SFVS_SLIPPY, TALK_HIT, 0, gMsg_ID_20311 },                     // Oh no!
    { SFVS_SLIPPY, TALK_HIT, 0, gMsg_ID_20050 },                     // Slippy's hit!
    { SFVS_SLIPPY, TALK_SAVED, 0, gMsg_ID_9170 },                    // Hoo! That was TOO close.
    { SFVS_SLIPPY, TALK_SAVED, 0, gMsg_ID_4112 },                    // I knew you'd come help me!
    { SFVS_SLIPPY, TALK_SAVED, 0, gMsg_ID_4100 },                    // I thought I was a goner!
    { SFVS_SLIPPY, TALK_WARN, TN_OTHER_FOX, gMsg_ID_20316 },         // Watch out, Fox!
    { SFVS_SLIPPY, TALK_NICE, TN_OTHER_FOX, gMsg_ID_19410 },         // Way to go, Fox!
    { SFVS_SLIPPY, TALK_NICE, TN_OTHER_FOX, gMsg_ID_20263 },         // Great, Fox!
    { SFVS_SLIPPY, TALK_NICE, 0, gMsg_ID_2305 },                     // There ya go!
    // Peppy
    { SFVS_PEPPY, TALK_START, 0, gMsg_ID_20304 },                    // Never give up. Trust your instincts.
    { SFVS_PEPPY, TALK_START, TN_ALLY_FOX, gMsg_ID_20296 },          // Come on, Fox. You can do it.
    { SFVS_PEPPY, TALK_ATTACK, 0, gMsg_ID_9210 },                    // I won't let you get away from me!
    { SFVS_PEPPY, TALK_ATTACK, 0, gMsg_ID_9213 },                    // You're mine!
    { SFVS_PEPPY, TALK_CHASED, TN_ALLY_FOX, gMsg_ID_9120 },          // Take care of the guy behind me, Fox!
    { SFVS_PEPPY, TALK_CHASED, 0, gMsg_ID_19464 },                   // I can't break free!
    { SFVS_PEPPY, TALK_KILL, 0, gMsg_ID_9240 },                      // There's one down!
    { SFVS_PEPPY, TALK_KILL, TN_LAST, gMsg_ID_9437 },                // I got the last one!
    { SFVS_PEPPY, TALK_DOWN, 0, gMsg_ID_20091 },                     // No!!!!
    { SFVS_PEPPY, TALK_DOWN, 0, gMsg_ID_9153 },                      // Shoot! They got me!
    { SFVS_PEPPY, TALK_DOWN, TN_OUT, gMsg_ID_20221 },                // Sorry, guys, I've gotta sit this one out.
    { SFVS_PEPPY, TALK_HIT, 0, gMsg_ID_20312 },                      // Doggone it!
    { SFVS_PEPPY, TALK_HIT, 0, gMsg_ID_20040 },                      // Dang! I'm hit!
    { SFVS_PEPPY, TALK_SAVED, TN_OTHER_FOX, gMsg_ID_9180 },          // Your father helped me like that, too.
    { SFVS_PEPPY, TALK_SAVED, 0, gMsg_ID_20016 },                    // I saw my life flash before my eyes!
    { SFVS_PEPPY, TALK_WARN, TN_OTHER_FOX, gMsg_ID_20317 },          // Watch out, Fox!
    { SFVS_PEPPY, TALK_NICE, TN_OTHER_FOX, gMsg_ID_17160 },          // Good going, Fox.
    { SFVS_PEPPY, TALK_NICE, TN_OTHER_FOX, gMsg_ID_20281 },          // I see your father in you, Fox.
    { SFVS_PEPPY, TALK_NICE, 0, gMsg_ID_14230 },                     // Good job. Keep up the good work.
    // Wolf
    { SFVS_WOLF, TALK_START, TN_FOE_STARFOX, gMsg_ID_9250 },         // Can't let you do that, Star Fox!
    { SFVS_WOLF, TALK_START, TN_FOE_STARFOX, gMsg_ID_11100 },        // Play time is over, Star Fox!
    { SFVS_WOLF, TALK_START, TN_FOE_STARFOX, gMsg_ID_19200 },        // Don't get too cocky, Star Fox!
    { SFVS_WOLF, TALK_ATTACK, 0, gMsg_ID_9290 },                     // You're in my sights! You're going down!
    { SFVS_WOLF, TALK_ATTACK, 0, gMsg_ID_19450 },                    // What?! Is that all you got?
    { SFVS_WOLF, TALK_ATTACK, TN_OTHER_FOX, gMsg_ID_9289 },          // You'll be seeing your dad soon, Fox!
    { SFVS_WOLF, TALK_CHASED, 0, gMsg_ID_9330 },                     // I will not be defeated by this trash!
    { SFVS_WOLF, TALK_CHASED, 0, gMsg_ID_19454 },                    // You're good, but I'm better.
    { SFVS_WOLF, TALK_KILL, 0, gMsg_ID_9322 },                       // You're not so tough!
    { SFVS_WOLF, TALK_KILL, TN_LAST | TN_OTHER_STARFOX, gMsg_ID_9431 }, // Looks like we win today, Star Fox.
    { SFVS_WOLF, TALK_DOWN, 0, gMsg_ID_9365 },                       // I...can't...lose!
    { SFVS_WOLF, TALK_DOWN, 0, gMsg_ID_19458 },                      // No way! I don't believe it!
    { SFVS_WOLF, TALK_HIT, 0, gMsg_ID_9369 },                        // What the heck?!
    // Leon
    { SFVS_LEON, TALK_START, TN_FOE_STARFOX, gMsg_ID_9260 },         // Andross has ordered us to take you down.
    { SFVS_LEON, TALK_START, 0, gMsg_ID_19210 },                     // Let's see how you handle our new ships!
    { SFVS_LEON, TALK_START, 0, gMsg_ID_11110 },                     // I think I'll torture you for awhile!
    { SFVS_LEON, TALK_ATTACK, 0, gMsg_ID_19451 },                    // I'll take care of you!
    { SFVS_LEON, TALK_ATTACK, TN_OTHER_FALCO, gMsg_ID_9323 },        // Annoying bird! I am the great Leon!
    { SFVS_LEON, TALK_CHASED, 0, gMsg_ID_9340 },                     // Shoot me down if you can!
    { SFVS_LEON, TALK_CHASED, 0, gMsg_ID_19455 },                    // Not yet. The party's just begun!
    { SFVS_LEON, TALK_KILL, 0, gMsg_ID_9300 },                       // You're not as tough as I thought.
    { SFVS_LEON, TALK_KILL, 0, gMsg_ID_9432 },                       // Close, but no cigar! HA HA!
    { SFVS_LEON, TALK_DOWN, 0, gMsg_ID_9366 },                       // This can't be happening!
    { SFVS_LEON, TALK_DOWN, TN_OTHER_MALE, gMsg_ID_19459 },          // He's too strong!
    // Pigma
    { SFVS_PIGMA, TALK_START, 0, gMsg_ID_9270 },                     // We're gettin' paid a lot of cash for this.
    { SFVS_PIGMA, TALK_START, TN_FOE_PEPPY, gMsg_ID_9275 },          // Peppy! Long time no see!
    { SFVS_PIGMA, TALK_ATTACK, 0, gMsg_ID_19452 },                   // I'm gonna bust you up REAL good!
    { SFVS_PIGMA, TALK_ATTACK, TN_OTHER_FOX, gMsg_ID_9310 },         // That reward's as good as mine!
    { SFVS_PIGMA, TALK_ATTACK, TN_OTHER_FOX, gMsg_ID_19220 },        // Too bad dad's not here to see ya FAIL!
    { SFVS_PIGMA, TALK_ATTACK, TN_OTHER_PEPPY, gMsg_ID_9324 },       // I'll do you fast, Peppy old pal.
    { SFVS_PIGMA, TALK_CHASED, TN_OTHER_MALE, gMsg_ID_9350 },        // Come on, little man! Shoot me!
    { SFVS_PIGMA, TALK_CHASED, 0, gMsg_ID_19456 },                   // You can't beat me. I've got a better ship!
    { SFVS_PIGMA, TALK_KILL, 0, gMsg_ID_9433 },                      // I've got two words for you: LO-SER!!!
    { SFVS_PIGMA, TALK_DOWN, 0, gMsg_ID_9367 },                      // My beautiful reward! Aaarrrggghhh!
    { SFVS_PIGMA, TALK_DOWN, 0, gMsg_ID_19460 },                     // This can't be happening!
    // Andrew
    { SFVS_ANDREW, TALK_START, 0, gMsg_ID_9280 },                    // Andross' enemy is my enemy.
    { SFVS_ANDREW, TALK_START, 0, gMsg_ID_11130 },                   // You'll be sorry you crossed us!
    { SFVS_ANDREW, TALK_ATTACK, 0, gMsg_ID_19453 },                  // You're not welcome here!
    { SFVS_ANDREW, TALK_ATTACK, 0, gMsg_ID_9320 },                   // Bow before the great Andross!
    { SFVS_ANDREW, TALK_ATTACK, 0, gMsg_ID_19457 },                  // Give it up. You can't win!
    { SFVS_ANDREW, TALK_ATTACK, TN_OTHER_SLIPPY, gMsg_ID_9325 },     // Stick to the pond, Froggie.
    { SFVS_ANDREW, TALK_CHASED, 0, gMsg_ID_9360 },                   // I'm not afraid of YOU!
    { SFVS_ANDREW, TALK_KILL, 0, gMsg_ID_9434 },                     // Score one for Andross!
    { SFVS_ANDREW, TALK_DOWN, 0, gMsg_ID_9368 },                     // Uncle Andross!
    { SFVS_ANDREW, TALK_DOWN, 0, gMsg_ID_19461 },                    // Ahhhh!!!!
    // Bill
    { SFVS_BILL, TALK_START, TN_FOX_THERE, gMsg_ID_18120 },          // Just like old times, eh Fox?
    { SFVS_BILL, TALK_START, TN_ALLY_FOX, gMsg_ID_18100 },           // I'm glad we're on the same team, Fox.
    { SFVS_BILL, TALK_START, TN_TEAMMATES, gMsg_ID_18000 },          // Hang on, guys. Help is on the way!
    { SFVS_BILL, TALK_NICE, 0, gMsg_ID_18066 },                      // Yee haaaa! You did it!
    { SFVS_BILL, TALK_NICE, TN_OTHER_FOX, gMsg_ID_18031 },           // You've become quite a pilot.
    // Katt
    { SFVS_KATT, TALK_START, 0, gMsg_ID_16135 },                     // Make way for Katt!
    { SFVS_KATT, TALK_START, 0, gMsg_ID_16120 },                     // Are you gonna hog all the fun?
    { SFVS_KATT, TALK_ATTACK, 0, gMsg_ID_16135 },                    // Make way for Katt!
    { SFVS_KATT, TALK_CHASED, 0, gMsg_ID_20084 },                    // You tryin' to damage my pretty face?!
    { SFVS_KATT, TALK_KILL, TN_OTHER_MALE, gMsg_ID_16165 },          // Too bad, little man.
    { SFVS_KATT, TALK_DOWN, TN_OTHER_MALE, gMsg_ID_17300 },          // You're pretty good, tiger. See you again!
    { SFVS_KATT, TALK_HIT, 0, gMsg_ID_20084 },                       // You tryin' to damage my pretty face?!
    { SFVS_KATT, TALK_SAVED, 0, gMsg_ID_16140 },                     // Beautiful! I could kiss you for that!
    { SFVS_KATT, TALK_NICE, 0, gMsg_ID_16140 },                      // Beautiful! I could kiss you for that!
};
// clang-format on

#define TALK_GAP 150      // frames at least between two lines (kills: TALK_KILL_GAP)
#define TALK_KILL_GAP 60
#define TALK_COOLDOWN 600 // the same pilot doesn't talk about small things that often

static s32 sTalkSince;  // frames since the last line
static s32 sTalkGap;    // frames until the next small line
static s32 sTalkStart;  // frames until the first line of the match
static s32 sTalkCooldown[MAX_PLAYERS];
static s32 sAttackTicks[MAX_PLAYERS];
static s32 sAttackTarget[MAX_PLAYERS];
static s32 sChasedTicks[MAX_PLAYERS];
static s32 sChasedBy[MAX_PLAYERS];
static s32 sLastChaser[MAX_PLAYERS];
static s32 sLastChasedFrame[MAX_PLAYERS];
static s32 sTalkPrevHitTimer[MAX_PLAYERS];
static u32 sTalkRng = 0x2545F491;
static s32 sTalker = -1; // the pilot who talks on the radio

// the shields of the pilot who talks (the radio shows them), -1 when that pilot is down
int32_t SFModVs_TalkerShields(void) {
    if ((sTalker < 0) || (sTalker >= gCamCount)) {
        return 0;
    }
    if (!SFModVs_Alive(sTalker)) {
        return -1;
    }
    return gPlayer[sTalker].shields;
}

static u32 SFModVs_TalkRand(u32 n) {
    sTalkRng ^= sTalkRng << 13;
    sTalkRng ^= sTalkRng >> 17;
    sTalkRng ^= sTalkRng << 5;
    return (n == 0) ? 0 : (sTalkRng % n);
}

// a CPU pilot of this match (the human doesn't get words put in his mouth)
static bool SFModVs_Speaks(s32 i) {
    return gSFVs.oshaberi && (i >= 0) && (i < gCamCount) && (i < gSFVs.numPilots) && !(gSFVs.humanPlays && (i == 0));
}

static RadioCharacterId SFModVs_Rcid(s32 character) {
    switch (character) {
        case SFVS_FOX:
            return RCID_FOX;
        case SFVS_FALCO:
            return RCID_FALCO;
        case SFVS_PEPPY:
            return RCID_PEPPY;
        case SFVS_SLIPPY:
            return RCID_SLIPPY;
        case SFVS_WOLF:
            return RCID_WOLF;
        case SFVS_PIGMA:
            return RCID_PIGMA;
        case SFVS_ANDREW:
            return RCID_ANDREW;
        case SFVS_LEON:
            return RCID_LEON;
        case SFVS_BILL:
            return RCID_BILL;
        default:
            return RCID_KATT;
    }
}

static u32 SFModVs_TalkFlags(s32 speaker, s32 other) {
    u32 f = 0;
    s32 foes = 0;
    s32 i;

    if ((other >= 0) && (other < gCamCount)) {
        s32 c = SFModVs_Character(other);

        if (c == SFVS_FOX) {
            f |= TN_OTHER_FOX;
        } else if (c == SFVS_FALCO) {
            f |= TN_OTHER_FALCO;
        } else if (c == SFVS_PEPPY) {
            f |= TN_OTHER_PEPPY;
        } else if (c == SFVS_SLIPPY) {
            f |= TN_OTHER_SLIPPY;
        }
        if (c <= SFVS_SLIPPY) {
            f |= TN_OTHER_STARFOX;
        }
        if (c != SFVS_KATT) {
            f |= TN_OTHER_MALE;
        }
    }
    for (i = 0; i < gCamCount; i++) {
        s32 c = SFModVs_Character(i);

        if (i == speaker) {
            continue;
        }
        if (c == SFVS_FOX) {
            f |= TN_FOX_THERE;
        }
        if (SFModVs_SameTeam(speaker, i)) {
            if (!sOut[i]) {
                f |= TN_TEAMMATES;
            }
            if (c == SFVS_FOX) {
                f |= TN_ALLY_FOX;
            }
        } else {
            if (c <= SFVS_SLIPPY) {
                f |= TN_FOE_STARFOX;
            }
            if ((c >= SFVS_WOLF) && (c <= SFVS_LEON)) {
                f |= TN_FOE_STARWOLF;
            }
            if (c == SFVS_PEPPY) {
                f |= TN_FOE_PEPPY;
            }
            if (!sOut[i]) {
                foes++;
            }
        }
    }
    if (gSFVs.rule == SFVS_RULE_SURVIVAL) {
        if (foes == 0) {
            f |= TN_LAST;
        } else if (foes == 1) {
            f |= TN_ONE_LEFT;
        }
        if (sOut[speaker]) {
            f |= TN_OUT;
        }
    }
    return f;
}

// One line of this pilot for this event (radio idle only, so nobody gets cut off)
static bool SFModVs_Say(s32 speaker, s32 event, s32 other) {
    const SFVsLine* pick[32];
    s32 n = 0;
    s32 c;
    u32 flags;
    s32 i;

    if (!SFModVs_Speaks(speaker) || (gRadioState != 0) || (sWinner != SFVS_NO_WINNER)) {
        return false;
    }
    c = SFModVs_Character(speaker);
    flags = SFModVs_TalkFlags(speaker, other);
    for (i = 0; i < ARRAY_COUNT(sLines); i++) {
        const SFVsLine* l = &sLines[i];

        if ((l->character != c) || (l->event != event) || ((l->needs & ~flags) != 0)) {
            continue;
        }
        if (n < ARRAY_COUNT(pick)) {
            pick[n++] = l;
        }
        if ((l->needs & (TN_PERSONAL | TN_LAST | TN_ONE_LEFT | TN_OUT)) && (n < ARRAY_COUNT(pick))) {
            pick[n++] = l; // the lines that fit the moment come up more often
        }
    }
    if (n == 0) {
        return false;
    }
    Radio_PlayMessage((u16*) pick[SFModVs_TalkRand(n)]->msg, SFModVs_Rcid(c));
    sTalker = speaker;
    sTalkSince = 0;
    sTalkGap = TALK_GAP + SFModVs_TalkRand(90);
    sTalkCooldown[speaker] = TALK_COOLDOWN + SFModVs_TalkRand(300);
    if (sDebug) {
        printf("[SFVS] %s says something (event %d, about %d)\n", SFModVs_PilotName(speaker), event, other + 1);
        fflush(stdout);
    }
    return true;
}

static void SFModVs_TalkReset(void) {
    s32 i;

    sTalkSince = 1000;
    sTalkGap = 0;
    sTalkStart = 0;
    for (i = 0; i < MAX_PLAYERS; i++) {
        sTalkCooldown[i] = 0;
        sAttackTicks[i] = sChasedTicks[i] = 0;
        sAttackTarget[i] = sChasedBy[i] = sLastChaser[i] = -1;
        sLastChasedFrame[i] = -1000;
        sTalkPrevHitTimer[i] = 0;
    }
    sTalkRng ^= (u32) osGetTime() | 1;
    // the radio only lets the team talk while their (story) shields are up
    gTeamShields[TEAM_ID_FALCO] = gTeamShields[TEAM_ID_SLIPPY] = gTeamShields[TEAM_ID_PEPPY] = 255;
}

// is pilot a on pilot b's tail (close, nose pointing at b)?
static bool SFModVs_OnTail(s32 a, s32 b) {
    Player* pa = &gPlayer[a];
    Player* pb = &gPlayer[b];
    f32 dx = pb->pos.x - pa->pos.x;
    f32 dy = pb->pos.y - pa->pos.y;
    f32 dz = pb->trueZpos - pa->trueZpos;
    f32 dist = sqrtf(SQ(dx) + SQ(dy) + SQ(dz));
    f32 speed = sqrtf(SQ(pa->vel.x) + SQ(pa->vel.y) + SQ(pa->vel.z));

    if ((dist > 1800.0f) || (dist < 50.0f) || (speed < 1.0f)) {
        return false;
    }
    return ((dx * pa->vel.x + dy * pa->vel.y + dz * pa->vel.z) / (dist * speed)) > 0.94f;
}

// someone was shot down (or crashed: killer -1)
static void SFModVs_TalkOnDeath(s32 victim, s32 killer) {
    s32 i;

    if (!gSFVs.oshaberi || (sTalkSince < TALK_KILL_GAP)) {
        return;
    }
    // the victim was on a teammate's tail: the teammate thanks the one who saved him
    if ((killer >= 0) && gSFVs.teams) {
        for (i = 0; i < gCamCount; i++) {
            if ((i != killer) && SFModVs_SameTeam(i, killer) && SFModVs_Alive(i) && (sLastChaser[i] == victim) &&
                ((gGameFrameCount - sLastChasedFrame[i]) < 90) && (SFModVs_TalkRand(100) < 70) &&
                SFModVs_Say(i, TALK_SAVED, killer)) {
                return;
            }
        }
    }
    if ((killer >= 0) && (SFModVs_TalkRand(100) < 60) && SFModVs_Say(killer, TALK_KILL, victim)) {
        return;
    }
    if ((SFModVs_TalkRand(100) < (sOut[victim] ? 85 : 55)) && SFModVs_Say(victim, TALK_DOWN, killer)) {
        return;
    }
    // a teammate cheers
    if ((killer >= 0) && gSFVs.teams && (SFModVs_TalkRand(100) < 40)) {
        for (i = 0; i < gCamCount; i++) {
            if ((i != killer) && SFModVs_SameTeam(i, killer) && SFModVs_Alive(i) && SFModVs_Say(i, TALK_NICE, killer)) {
                return;
            }
        }
    }
}

static void SFModVs_UpdateTalk(void) {
    s32 cand[MAX_PLAYERS * 3];
    s32 candEvent[MAX_PLAYERS * 3];
    s32 candOther[MAX_PLAYERS * 3];
    s32 n = 0;
    s32 i;
    s32 j;

    if (!gSFVs.oshaberi) {
        return;
    }
    sTalkSince++;
    if (sTalkGap > 0) {
        sTalkGap--;
    }
    for (i = 0; i < MAX_PLAYERS; i++) {
        if (sTalkCooldown[i] > 0) {
            sTalkCooldown[i]--;
        }
    }

    // the match begins: a Star Wolf pilot taunts first, otherwise somebody
    if ((sTalkStart > 0) && (--sTalkStart == 0)) {
        for (i = 0; i < gCamCount; i++) {
            s32 c = SFModVs_Character(i);

            if (SFModVs_Speaks(i) && (c >= SFVS_WOLF) && (c <= SFVS_LEON)) {
                cand[n++] = i;
            }
        }
        if (n == 0) {
            for (i = 0; i < gCamCount; i++) {
                if (SFModVs_Speaks(i)) {
                    cand[n++] = i;
                }
            }
        }
        if (n > 0) {
            SFModVs_Say(cand[SFModVs_TalkRand(n)], TALK_START, -1);
        }
        return;
    }

    // hits
    for (i = 0; i < gCamCount; i++) {
        Player* pl = &gPlayer[i];

        if ((pl->hitTimer > sTalkPrevHitTimer[i]) && (pl->shields < 160) && (sTalkGap == 0) &&
            (sTalkCooldown[i] == 0) && (SFModVs_TalkRand(100) < 25)) {
            SFModVs_Say(i, TALK_HIT, pl->attacker - 1);
        }
        sTalkPrevHitTimer[i] = pl->hitTimer;
    }

    if ((gGameFrameCount % 15) != 0) {
        return;
    }
    // who is on whose tail
    for (i = 0; i < gCamCount; i++) {
        s32 target = -1;
        s32 chaser = -1;

        if (SFModVs_Alive(i)) {
            for (j = 0; j < gCamCount; j++) {
                if ((j == i) || !SFModVs_Alive(j) || SFModVs_SameTeam(i, j)) {
                    continue;
                }
                if (SFModVs_OnTail(i, j)) {
                    target = j;
                }
                if (SFModVs_OnTail(j, i)) {
                    chaser = j;
                }
            }
        }
        sAttackTicks[i] = ((target >= 0) && (target == sAttackTarget[i])) ? (sAttackTicks[i] + 1) : 0;
        sAttackTarget[i] = target;
        sChasedTicks[i] = ((chaser >= 0) && (chaser == sChasedBy[i])) ? (sChasedTicks[i] + 1) : 0;
        sChasedBy[i] = chaser;
        if (chaser >= 0) {
            sLastChaser[i] = chaser;
            sLastChasedFrame[i] = gGameFrameCount;
        }
    }
    if ((sTalkGap > 0) || (gRadioState != 0) || (SFModVs_TalkRand(100) >= 35)) {
        return;
    }
    for (i = 0; i < gCamCount; i++) {
        if (!SFModVs_Speaks(i) || (sTalkCooldown[i] > 0) || !SFModVs_Alive(i)) {
            continue;
        }
        if (sAttackTicks[i] >= 3) {
            cand[n] = i;
            candEvent[n] = TALK_ATTACK;
            candOther[n++] = sAttackTarget[i];
        }
        if (sChasedTicks[i] >= 3) {
            cand[n] = i;
            candEvent[n] = TALK_CHASED;
            candOther[n++] = sChasedBy[i];
        }
        // a teammate of the human has somebody on his tail
        if (gSFVs.humanPlays && gSFVs.teams && SFModVs_SameTeam(i, 0) && (sChasedTicks[0] >= 2)) {
            cand[n] = i;
            candEvent[n] = TALK_WARN;
            candOther[n++] = 0;
        }
    }
    if (n > 0) {
        i = SFModVs_TalkRand(n);
        SFModVs_Say(cand[i], candEvent[i], candOther[i]);
    }
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
    SFModVs_TalkOnDeath(i, ((killer >= 0) && (killer < gCamCount) && (killer != i)) ? killer : -1);
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
    static bool sShotWasActive[ARRAY_COUNT(gPlayerShots)];
    static s32 sPrevHitTimer[MAX_PLAYERS];
    static s32 sShots[MAX_PLAYERS];
    static s32 sHits[MAX_PLAYERS];
    static s32 sCharged[MAX_PLAYERS];
    static s32 sBigHits[MAX_PLAYERS];
    s32 i;

    if (!sDebug) {
        return;
    }
    // shots fired and hits landed per pilot (lasers and charged shots)
    for (i = 0; i < ARRAY_COUNT(gPlayerShots); i++) {
        PlayerShot* shot = &gPlayerShots[i];
        bool active = (shot->obj.status == SHOT_ACTIVE) &&
                      ((shot->obj.id == PLAYERSHOT_SINGLE_LASER) || (shot->obj.id == PLAYERSHOT_TWIN_LASER) ||
                       (shot->obj.id == PLAYERSHOT_LOCK_ON));

        if (active && !sShotWasActive[i] && (shot->sourceId >= 0) && (shot->sourceId < gCamCount)) {
            if (shot->obj.id == PLAYERSHOT_LOCK_ON) {
                sCharged[shot->sourceId]++;
            } else {
                sShots[shot->sourceId]++;
            }
        }
        sShotWasActive[i] = active;
    }
    for (i = 0; i < gCamCount; i++) {
        s32 attacker = gPlayer[i].attacker - 1;

        if ((gPlayer[i].hitTimer > sPrevHitTimer[i]) && (attacker >= 0) && (attacker < gCamCount) && (attacker != i)) {
            if (gPlayer[i].dmgType >= 40) {
                sBigHits[attacker]++;
                printf("[SFVS] big hit: %d hit %d (damage %d)\n", attacker + 1, i + 1, gPlayer[i].dmgType);
            } else {
                sHits[attacker]++;
            }
        }
        sPrevHitTimer[i] = gPlayer[i].hitTimer;
    }

    // what happens to the homing shots
    {
        static s32 sHomingState[MAX_PLAYERS]; // 0 none, 1 homing, 2 lost its lock
        static s32 sHomingAge[MAX_PLAYERS];

        for (i = 0; i < gCamCount; i++) {
            PlayerShot* shot = &gPlayerShots[PLAYERSHOT_LOCKON_SLOT(i)];
            bool active = (shot->obj.status == SHOT_ACTIVE) && (shot->obj.id == PLAYERSHOT_LOCK_ON);

            if (active && (sHomingState[i] == 0)) {
                sHomingState[i] = (shot->unk_60 == 0) ? 1 : 2;
                sHomingAge[i] = 0;
            } else if (active) {
                sHomingAge[i]++;
                if ((sHomingState[i] == 1) && (shot->unk_60 != 0)) {
                    printf("[SFVS] homing shot of %d lost its lock after %d frames\n", i + 1, sHomingAge[i]);
                    sHomingState[i] = 2;
                }
            } else if (sHomingState[i] != 0) {
                printf("[SFVS] homing shot of %d ended after %d frames (%s)\n", i + 1, sHomingAge[i],
                       (sHomingState[i] == 1) ? "homing" : "unlocked");
                sHomingState[i] = 0;
            }
        }
    }

    // items: when they appear and who gets them
    {
        static s32 sItemStatus[ARRAY_COUNT(gItems)];
        static s32 sItemAge[ARRAY_COUNT(gItems)];

        for (i = 0; i < ARRAY_COUNT(gItems); i++) {
            Item* item = &gItems[i];
            s32 status = item->obj.status;

            if ((status == OBJ_ACTIVE) && (sItemStatus[i] != OBJ_ACTIVE)) {
                sItemAge[i] = 0;
                printf("[SFVS] item %d (%s) appears at %.0f %.0f %.0f\n", i,
                       (item->obj.id == OBJ_ITEM_BOMB) ? "bomb" : "lasers", item->obj.pos.x, item->obj.pos.y,
                       item->obj.pos.z);
            } else if (status == OBJ_ACTIVE) {
                sItemAge[i]++;
                if (item->collected && (item->state == 1) && (item->timer_48 == 19)) {
                    printf("[SFVS] item %d (%s) collected by %d after %d frames\n", i,
                           (item->obj.id == OBJ_ITEM_BOMB) ? "bomb" : "lasers", item->playerNum + 1, sItemAge[i]);
                }
            }
            sItemStatus[i] = status;
        }
    }
    // smart bombs: fired (homing or not), where they explode
    {
        static s32 sBombId = -1;
        static s32 sBombPhase = -1;
        static s32 sBombAge;
        PlayerShot* bomb = &gPlayerShots[PLAYERSHOT_BOMB_SLOT];
        s32 id = (bomb->obj.status == SHOT_ACTIVE) ? bomb->obj.id : -1;
        s32 phase = (id >= 0) ? bomb->unk_5C : -1;

        if ((id >= 0) && (sBombId < 0)) {
            sBombAge = 0;
            printf("[SFVS] bomb fired by %d (%s)\n", bomb->sourceId + 1, (id == PLAYERSHOT_LOCK_ON) ? "homing" : "straight");
        } else if (id >= 0) {
            sBombAge++;
        }
        if ((id == PLAYERSHOT_BOMB) && (phase == 1) && (sBombPhase != 1)) {
            f32 nearest = 99999.0f;
            s32 j;

            for (j = 0; j < gCamCount; j++) {
                f32 dx = gPlayer[j].pos.x - bomb->obj.pos.x;
                f32 dy = gPlayer[j].pos.y - bomb->obj.pos.y;
                f32 dz = gPlayer[j].trueZpos - bomb->obj.pos.z;
                f32 dist = sqrtf(SQ(dx) + SQ(dy) + SQ(dz));

                if ((j != bomb->sourceId) && !SFModVs_SameTeam(j, bomb->sourceId) && SFModVs_Alive(j) && (dist < nearest)) {
                    nearest = dist;
                }
            }
            printf("[SFVS] bomb of %d explodes after %d frames, nearest enemy %.0f away\n", bomb->sourceId + 1, sBombAge,
                   nearest);
        }
        sBombId = id;
        sBombPhase = (id == PLAYERSHOT_BOMB) ? phase : -1;
    }

    if ((gGameFrameCount % 150) != 0) {
        return;
    }
    printf("[SFVS] frame %d time %d (other pilots' personal sounds left out: %d, team marks drawn: %d, hit sounds: %d):",
           gGameFrameCount, sTimeLeft / 30, sSfxMuted, sMarksDrawn, sHitSfx);
    for (i = 0; i < gCamCount; i++) {
        printf(" %s(sh%d k%d d%d s%d h%d c%d H%d b%d l%d)", SFModVs_PilotName(i), gPlayer[i].shields, sKills[i],
               sDeaths[i], sShots[i], sHits[i], sCharged[i], sBigHits[i], gBombCount[i], gLaserStrength[i]);
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
                // every match: the stage's VS music, or the music of Fortuna, Katina or Sector Z
                static const u16 sMatchBgms[] = { NA_BGM_STAGE_FO, NA_BGM_STAGE_KA, NA_BGM_STAGE_SZ };
                s32 pick = (s32) (Rand_ZeroOne() * (ARRAY_COUNT(sMatchBgms) + 1));
                const char* v = getenv("SFMOD_TEST_VS_BGM"); // testing: 0..2 one of those, 3 the stage's

                if (sStageBgm < 0) {
                    sStageBgm = gBgmSeqId;
                }
                gBgmSeqId = ((pick >= 0) && (pick < ARRAY_COUNT(sMatchBgms))) ? sMatchBgms[pick] : sStageBgm;
                if (v != NULL) {
                    gBgmSeqId = (atoi(v) < ARRAY_COUNT(sMatchBgms)) ? sMatchBgms[atoi(v)] : sStageBgm;
                }
                if (sDebug) {
                    printf("[SFVS] match music 0x%X (stage music 0x%X)\n", gBgmSeqId, sStageBgm);
                    if (getenv("SFMOD_DEBUG_SCENERY") != NULL) {
                        for (i = 0; i < 200; i++) {
                            Scenery360* scn = &gScenery360[i];

                            if (scn->obj.status == OBJ_ACTIVE) {
                                printf("[SFVS] scenery %d id %d pos %.0f %.0f %.0f rotY %.0f hitbox %s", i, scn->obj.id,
                                       scn->obj.pos.x, scn->obj.pos.y, scn->obj.pos.z, scn->obj.rot.y,
                                       (scn->info.hitbox == NULL) ? "null" : "");
                                if ((scn->info.hitbox != NULL) && ((s32) scn->info.hitbox[0] > 0)) {
                                    printf("n%d z%.0f/%.0f y%.0f/%.0f x%.0f/%.0f", (s32) scn->info.hitbox[0],
                                           scn->info.hitbox[1], scn->info.hitbox[2], scn->info.hitbox[3],
                                           scn->info.hitbox[4], scn->info.hitbox[5], scn->info.hitbox[6]);
                                }
                                printf("\n");
                            }
                        }
                    }
                    fflush(stdout);
                }
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
                    SFModVs_TalkReset();
                    sTalkStart = 75;
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
            if (SFModVs_TestFlag("SFMOD_TEST_VS_ALLY_AHEAD")) {
                // testing the team marks: the first teammate of the watched pilot flies right in front of it
                s32 v = SFModVs_ViewedPlayer();

                for (i = 0; i < gCamCount; i++) {
                    if ((i != v) && SFModVs_SameTeam(i, v)) {
                        f32 yaw = (gPlayer[v].yRot_114 + gPlayer[v].rot.y) * M_DTOR;

                        gPlayer[i].pos.x = gPlayer[v].pos.x - sinf(yaw) * 1500.0f + cosf(yaw) * 350.0f;
                        gPlayer[i].pos.y = gPlayer[v].pos.y - 40.0f;
                        gPlayer[i].pos.z = gPlayer[v].pos.z - cosf(yaw) * 1500.0f - sinf(yaw) * 350.0f;
                        gPlayer[i].trueZpos = gPlayer[i].pos.z;
                        break;
                    }
                }
            }
            SFModVs_UpdatePilots();
            SFModVs_UpdateTalk();
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

    // The pilots: arrows that point where they fly, like the marks on the story radar. They are drawn
    // in eye space (like HUD_RadarMarks_Update) at the screen position of the map.
    {
        static u8 sRadarTags[MAX_PLAYERS];
        const f32 depth = 1000.0f;
        f32 k = depth * tanf(gFovY * 0.5f * M_DTOR) / (SCREEN_HEIGHT / 2.0f); // units per screen pixel
        s32 n;

        for (n = 0; n < gCamCount; n++) {
            // the watched pilot last, on top of the others
            s32 i = (n < sViewed) ? n : ((n + 1 < gCamCount) ? n + 1 : sViewed);
            Player* pl = &gPlayer[i];
            f32 px;
            f32 pz;
            f32 sx;
            f32 sy;
            f32 pixels;
            u8 r, g, b;
            u8 a = 255;

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
            sx = x0 + (px + 12000.0f) / 24000.0f * size;
            sy = y0 + (pz + 12000.0f) / 24000.0f * size;
            pixels = (i == sViewed) ? 9.0f : 7.0f;
            if ((i == sViewed) && (gGameFrameCount & 2)) {
                a = 128; // the story radar's own mark blinks like this
            }
            if (pl->state == PLAYERSTATE_DOWN) {
                a = 96;
            }
            SFModVs_GetPlayerColor(i, &r, &g, &b);

            FrameInterpolation_RecordOpenChild(&sRadarTags[i], i);
            Matrix_Push(&gGfxMatrix);
            Matrix_Translate(gGfxMatrix, (sx - SCREEN_WIDTH / 2.0f) * k, (SCREEN_HEIGHT / 2.0f - sy) * k, -depth,
                             MTXF_APPLY);
            Matrix_RotateZ(gGfxMatrix, (pl->yRot_114 + pl->rot.y) * M_DTOR, MTXF_APPLY);
            Matrix_Scale(gGfxMatrix, pixels * k / 44.0f, pixels * k / 44.0f, 1.0f, MTXF_APPLY); // the mark is 44 high
            Matrix_SetGfxMtx(&gMasterDisp);
            RCP_SetupDL(&gMasterDisp, SETUPDL_62);
            gDPSetPrimColor(gMasterDisp++, 0, 0, r, g, b, a);
            gSPDisplayList(gMasterDisp++, aRadarMarkArwingDL);
            Matrix_Pop(&gGfxMatrix);
            FrameInterpolation_RecordCloseChild();
        }
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
                // above the radio while somebody talks
                s32 y = (gRadioState != 0) ? 146 : 206;

                SFModVs_PilotTextColor(p, 255);
                SFModVs_TextCentered(y, 0.9f, text);
                SFModVs_TextColor(200, 200, 200, 255);
                SFModVs_TextCentered(y + 12, 0.8f, "Z R SWITCH PILOT");
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
    MENU_ROW_OSHABERI,
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
        case MENU_ROW_OSHABERI:
            gSFVs.oshaberi = !gSFVs.oshaberi;
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
    const s32 lineH = 9;
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
            case MENU_ROW_OSHABERI:
                label = "OSHABERI";
                snprintf(text, sizeof(text), "%s", gSFVs.oshaberi ? "ON - THE PILOTS TALK" : "OFF");
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

static bool sTestBootPending = false;

int32_t SFModVs_TestItemSpawn(void) {
    static s32 sSpawn = -2;

    if (sSpawn == -2) {
        const char* v = getenv("SFMOD_TEST_VS_ITEMSPAWN");

        sSpawn = (v != NULL) ? CLAMP(atoi(v), 0, 4) : -1;
    }
    return sSpawn;
}

// The boot code resets the VS stage and rules after the test boot: set them up again.
void SFModVs_AfterBootInit(void) {
    if (sTestBootPending) {
        sTestBootPending = false;
        SFModVs_PrepareMatch();
    }
}

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
        // one level for all, or a list: "7,7,3,3"
        for (i = 0; i < MAX_PLAYERS; i++) {
            gSFVs.pilots[i].level = CLAMP(atoi(v), 1, 7);
            if (strchr(v, ',') != NULL) {
                v = strchr(v, ',') + 1;
            }
        }
    }
    if ((v = getenv("SFMOD_TEST_VS_PERS")) != NULL) {
        // personality index for all, or a list ("8,0,1,2"); -1 = random
        for (i = 0; i < MAX_PLAYERS; i++) {
            gSFVs.pilots[i].personality = CLAMP(atoi(v), -1, SFAI_PersonalityCount() - 1);
            if (strchr(v, ',') != NULL) {
                v = strchr(v, ',') + 1;
            }
        }
    }
    SFModVs_PrepareMatch();
    sTestBootPending = true;
    gNextGameState = GSTATE_VS_INIT;
    gOptionMenuStatus = OPTION_WAIT;
    printf("[SFMod] test boot into a VS CPU match with %d pilots\n", gSFVs.numPilots);
    fflush(stdout);
    return true;
}
