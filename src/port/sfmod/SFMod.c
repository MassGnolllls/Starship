#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "SFMod.h"
#include "SFModVs.h"
#include "SFModGame.h"
#include "global.h"
#include "fox_map.h"
#include "port/hooks/Events.h"

// Accessors for the map overlay's state (fox_map.c keeps it static)
extern s32 Map_GetMapState(void);
extern s32 Map_GetLevelStartState(void);

/* ---------------------------------------------------------------------------------------------
 * Settings
 * --------------------------------------------------------------------------------------------- */

bool SFMod_BootToMenu(void) {
    return CVarGetInteger(CVAR_SFMOD_BOOT_TO_MENU, 1) != 0;
}

bool SFMod_SkipCutscenesEnabled(void) {
    return CVarGetInteger(CVAR_SFMOD_SKIP_CUTSCENES, 1) != 0;
}

bool SFMod_TankKeepsLaser(void) {
    return CVarGetInteger(CVAR_SFMOD_TANK_KEEPS_LASER, 1) != 0;
}

bool SFMod_TeamStaysBack(void) {
    return CVarGetInteger(CVAR_SFMOD_TEAM_STAYS_BACK, 1) != 0;
}

static void SFMod_FireSwapCVarName(char* buf, size_t size, s32 port) {
    snprintf(buf, size, "%s%d", CVAR_SFMOD_FIRE_SWAPPED_PREFIX, port + 1);
}

bool SFMod_FireButtonsSwapped(s32 port) {
    char name[64];

    SFMod_FireSwapCVarName(name, sizeof(name), port);
    return CVarGetInteger(name, 0) != 0;
}

static void SFMod_SetFireButtonsSwapped(s32 port, bool swapped) {
    char name[64];

    SFMod_FireSwapCVarName(name, sizeof(name), port);
    CVarSetInteger(name, swapped);
    CVarSave();
}

/* ---------------------------------------------------------------------------------------------
 * Cutscene skipping
 *
 * Skipping works by fast-forwarding: the game keeps running its normal logic, only many frames
 * per displayed frame and without sound. That way every cutscene finishes exactly like it would
 * have without skipping, so nothing (route flags, medals, team status, ...) can get lost.
 *
 * Level intros use the game's own intro skip instead (it restarts the level right after the
 * intro); the original only allows it for planets that were already cleared.
 * The ending/credits also use the game's own skip, which Starship only allows after the first
 * time the ending was seen.
 * --------------------------------------------------------------------------------------------- */

static bool sFastForward = false;
static bool sInFastForwardFrame = false;

// Player states that are cutscenes. Level intros are not in here: they use the game's own
// (instant) intro skip, and a skip that started somewhere else stops when a new level begins.
static bool SFMod_IsSkippablePlayerState(s32 state) {
    switch (state) {
        case PLAYERSTATE_LEVEL_COMPLETE:
        case PLAYERSTATE_ENTER_WARP_ZONE:
        case PLAYERSTATE_START_360:
        case PLAYERSTATE_GFOX_REPAIR:
            return true;
        default:
            return false;
    }
}

// Can a skip be started right now by pressing START?
static bool SFMod_CanStartSkip(void) {
    if (!SFMod_SkipCutscenesEnabled() || gVersusMode) {
        return false;
    }

    switch (gGameState) {
        case GSTATE_PLAY:
            if ((gPlayState != PLAY_UPDATE) || (gPlayer == NULL)) {
                return false;
            }
            return SFMod_IsSkippablePlayerState(gPlayer[0].state);

        case GSTATE_MAP:
            // the path to the next planet after a level, or zooming into a planet before its briefing
            return (gMapState == 2) &&
                   ((Map_GetMapState() == MAP_ZOOM_PLANET_PATH) || (Map_GetMapState() == MAP_ZOOM_PLANET));

        default:
            return false;
    }
}

// Should a running skip keep going?
static bool SFMod_SkipShouldContinue(void) {
    if (gNextGameState != GSTATE_NONE) {
        return true; // a state change is about to happen (e.g. level -> map)
    }

    switch (gGameState) {
        case GSTATE_PLAY:
            if (gPlayState == PLAY_PAUSE) {
                return false;
            }
            if ((gPlayState != PLAY_UPDATE) || (gPlayer == NULL)) {
                return true; // level is (re)loading
            }
            return SFMod_IsSkippablePlayerState(gPlayer[0].state) || (gPlayer[0].state == PLAYERSTATE_NEXT);

        case GSTATE_MAP:
            // keep going while the map is setting up, drawing the path to the next planet, zooming into a
            // planet or showing its briefing (which gets skipped, see SFMod_ProcessInput)
            if (gMapState != 2) {
                return true;
            }
            switch (Map_GetMapState()) {
                case MAP_ZOOM_PLANET_PATH:
                case MAP_ZOOM_PLANET:
                case MAP_LEVEL_START:
                    return true;
                default:
                    return false;
            }

        default:
            return false;
    }
}

bool SFMod_FastForwardActive(void) {
    if (!sFastForward) {
        return false;
    }
    if (!SFMod_SkipShouldContinue()) {
        sFastForward = false;
    }
    return sFastForward;
}

void SFMod_SetFastForwardFrame(bool inFastForwardFrame) {
    sInFastForwardFrame = inFastForwardFrame;
}

bool SFMod_IsFastForwardFrame(void) {
    return sInFastForwardFrame;
}

/* ---------------------------------------------------------------------------------------------
 * Turbo fire / swapping the fire buttons
 *
 * Turbo fire is implemented as perfectly timed taps of the normal fire button: one tap every
 * SFMOD_TURBO_INTERVAL frames and never held. Because the button is never held, the lock-on
 * never charges, and every vehicle (Arwing, Landmaster, Blue Marine, on foot) fires exactly
 * like it does for a player tapping very fast - no special rules for anyone.
 * --------------------------------------------------------------------------------------------- */

static u8 sTurboPhase[MAX_PLAYERS];
static u32 sPrevRawHold[MAX_PLAYERS];
static s32 sSwapMsgTimer = 0;
static bool sSwapMsgSwapped = false;

// The ship this controller port currently flies, NULL when it flies none
static Player* SFMod_PortShip(s32 port) {
    Player* player;

    if ((gGameState != GSTATE_PLAY) || (gPlayState != PLAY_UPDATE) || (gPlayer == NULL)) {
        return NULL;
    }

    if (gVersusMode) {
        if (port >= gCamCount) {
            return NULL;
        }
        player = &gPlayer[port];
    } else {
        if (port != gMainController) {
            return NULL;
        }
        player = &gPlayer[0];
    }

    if ((player->state == PLAYERSTATE_ACTIVE) || (player->state == PLAYERSTATE_U_TURN)) {
        return player;
    }
    return NULL;
}

// Does this controller port currently fly a ship?
static bool SFMod_PortControlsShip(s32 port) {
    return SFMod_PortShip(port) != NULL;
}

static void SFMod_ProcessFireButtons(s32 port, u32 rawHold, u32 rawPress) {
    OSContPad* hold = &gControllerHold[port];
    OSContPad* press = &gControllerPress[port];
    bool swapped;
    bool fireHold;
    bool firePress;
    bool turboHold;
    bool turboPress;

    if (rawPress & SFMOD_BTN_SWAP) {
        swapped = !SFMod_FireButtonsSwapped(port);
        SFMod_SetFireButtonsSwapped(port, swapped);
        AUDIO_PLAY_SFX(swapped ? NA_SE_MAP_WINDOW_OPEN : NA_SE_MAP_WINDOW_CLOSE, gDefaultSfxSource, 4);
        sSwapMsgTimer = 45;
        sSwapMsgSwapped = swapped;
    }

    swapped = SFMod_FireButtonsSwapped(port);

    if (!swapped) {
        fireHold = (rawHold & A_BUTTON) != 0;
        firePress = (rawPress & A_BUTTON) != 0;
        turboHold = (rawHold & SFMOD_BTN_TURBO) != 0;
        turboPress = (rawPress & SFMOD_BTN_TURBO) != 0;
    } else {
        fireHold = (rawHold & SFMOD_BTN_TURBO) != 0;
        firePress = (rawPress & SFMOD_BTN_TURBO) != 0;
        turboHold = (rawHold & A_BUTTON) != 0;
        turboPress = (rawPress & A_BUTTON) != 0;
    }
    // "Turbo fire + auto tilt" always fires turbo (the swap is about the two fire buttons)
    turboHold = turboHold || ((rawHold & SFMOD_BTN_TURBO_TILT) != 0);
    turboPress = turboPress || ((rawPress & SFMOD_BTN_TURBO_TILT) != 0);

    hold->button &= ~A_BUTTON;
    press->button &= ~A_BUTTON;

    if (fireHold) {
        // The normal fire button wins, so charging a lock-on still works while holding both.
        hold->button |= A_BUTTON;
        if (firePress) {
            press->button |= A_BUTTON;
        }
        sTurboPhase[port] = 0;
    } else if (turboHold) {
        if (turboPress) {
            sTurboPhase[port] = 0;
        }
        if (sTurboPhase[port] == 0) {
            hold->button |= A_BUTTON;
            press->button |= A_BUTTON;
        }
        sTurboPhase[port] = (sTurboPhase[port] + 1) % SFMOD_TURBO_INTERVAL;
    } else {
        sTurboPhase[port] = 0;
    }
}

/* ---------------------------------------------------------------------------------------------
 * Auto tilt (L button, and the "turbo fire + auto tilt" / "brake + auto tilt" inputs)
 *
 * While an auto tilt input is held, the Arwing tilts toward the side it is steered to, exactly as
 * if Z (left) or R (right) were held: tilted, it turns tighter. Without steering it stays level,
 * and without the inputs everything is as usual. The game only sees held Z / R buttons; the first
 * frame of a tilt also gets a single "press" for the game's tilt sound, which is then taken back
 * from the barrel roll check so it never counts as the first tap of a barrel roll. Pressing an
 * auto tilt input twice quickly does a barrel roll (to the side steered to), like Z or R twice.
 * Real Z / R presses always win.
 * --------------------------------------------------------------------------------------------- */

#define SFMOD_AUTOTILT_START 10 // stick (after the game's dead zone, up to 60) that starts a tilt
#define SFMOD_AUTOTILT_STOP 4   // ... and below which it ends again
#define SFMOD_AUTOTILT_DOUBLE_TAP 10 // frames between the two presses of a barrel roll (as Z / R)

static s8 sAutoTiltSide[4];      // -1 left (Z), 1 right (R), 0 level
static s8 sAutoTiltLastSide[4];  // the side of the last tilt (barrel roll without steering)
static u32 sAutoTiltAdded[4];    // buttons the auto tilt added to last frame's hold
static u32 sAutoTiltTapped[4];   // the button it "pressed" last frame (tilt sound)
static bool sAutoTiltWasHeld[4]; // an auto tilt input was held last frame
static s32 sAutoTiltTapFrame[4] = { -1000, -1000, -1000, -1000 }; // frame of the last press of an auto tilt input

static u32 SFMod_AutoTiltInputs(void) {
    u32 inputs = SFMOD_BTN_TURBO_TILT | SFMOD_BTN_BRAKE_TILT | SFMOD_BTN_BOOST_TILT;

    if (CVarGetInteger("gLtoCharge", 0) != 1) {
        inputs |= L_TRIG; // with "Hold L to Charge" L is the charge button
    }
    return inputs;
}

static void SFMod_ProcessAutoTilt(s32 port, u32 rawHold, u32 rawPress) {
    OSContPad* hold = &gControllerHold[port];
    OSContPad* press = &gControllerPress[port];
    Player* player = SFMod_PortShip(port);
    s32 stickX = press->stick_x; // what the game steers with (dead zone applied)
    s32 side = sAutoTiltSide[port];
    bool held = (rawHold & SFMod_AutoTiltInputs()) != 0;
    bool tapped = held && !sAutoTiltWasHeld[port];
    bool roll = false;

    sAutoTiltWasHeld[port] = held;

    // The game computes presses against last frame's hold, which had the added tilt button in it:
    // a real press of that button would get lost.
    press->button |= rawPress & sAutoTiltAdded[port];
    sAutoTiltAdded[port] = 0;

    // last frame's tilt sound press is no first tap of a barrel roll
    if (player != NULL) {
        if (sAutoTiltTapped[port] & Z_TRIG) {
            player->rollInputTimerL = 0;
        }
        if (sAutoTiltTapped[port] & R_TRIG) {
            player->rollInputTimerR = 0;
        }
    }
    sAutoTiltTapped[port] = 0;

    // "brake + auto tilt" brakes, "boost + auto tilt" boosts
    if ((player != NULL) && (rawHold & SFMOD_BTN_BRAKE_TILT)) {
        hold->button |= gBrakeButton[player->num];
        if (rawPress & SFMOD_BTN_BRAKE_TILT) {
            press->button |= gBrakeButton[player->num];
        }
    }
    if ((player != NULL) && (rawHold & SFMOD_BTN_BOOST_TILT)) {
        hold->button |= gBoostButton[player->num];
        if (rawPress & SFMOD_BTN_BOOST_TILT) {
            press->button |= gBoostButton[player->num];
        }
    }

    if (tapped) {
        roll = (gGameFrameCount - sAutoTiltTapFrame[port]) <= SFMOD_AUTOTILT_DOUBLE_TAP;
        sAutoTiltTapFrame[port] = roll ? -1000 : gGameFrameCount; // a third press starts over
    }

    if ((player == NULL) || (player->form != FORM_ARWING) || !held || (rawHold & (Z_TRIG | R_TRIG))) {
        // not flying an Arwing, no auto tilt input held, or tilting by hand
        sAutoTiltSide[port] = 0;
        return;
    }

    if (roll && (player->rollState == 0)) {
        // the second press: a barrel roll, like pressing Z or R twice
        s32 rollSide = (stickX <= -SFMOD_AUTOTILT_STOP)  ? -1
                       : (stickX >= SFMOD_AUTOTILT_STOP) ? 1
                       : (sAutoTiltLastSide[port] != 0)  ? sAutoTiltLastSide[port]
                                                         : 1;
        if (rollSide < 0) {
            player->rollInputTimerL = 5;
            press->button |= Z_TRIG;
        } else {
            player->rollInputTimerR = 5;
            press->button |= R_TRIG;
        }
    }

    if (((side < 0) && (stickX > -SFMOD_AUTOTILT_STOP)) || ((side > 0) && (stickX < SFMOD_AUTOTILT_STOP))) {
        side = 0;
    }
    if (side == 0) {
        if (stickX <= -SFMOD_AUTOTILT_START) {
            side = -1;
        } else if (stickX >= SFMOD_AUTOTILT_START) {
            side = 1;
        }
        // a new tilt: one press for the tilt sound, but never while a real tap could make it a barrel roll
        if (!roll && (side < 0) && (player->rollState == 0) && (player->rollInputTimerL == 0)) {
            press->button |= Z_TRIG;
            sAutoTiltTapped[port] = Z_TRIG;
        } else if (!roll && (side > 0) && (player->rollState == 0) && (player->rollInputTimerR == 0)) {
            press->button |= R_TRIG;
            sAutoTiltTapped[port] = R_TRIG;
        }
    }
    sAutoTiltSide[port] = side;

    if (side != 0) {
        sAutoTiltLastSide[port] = side;
        sAutoTiltAdded[port] = (side < 0) ? Z_TRIG : R_TRIG;
        hold->button |= sAutoTiltAdded[port];
    }
}

void SFMod_ProcessInput(void) {
    s32 i;

    if (sInFastForwardFrame) {
        // Nobody is "playing" while a cutscene is being skipped.
        for (i = 0; i < 4; i++) {
            sPrevRawHold[i] = gControllerHold[i].button;
            gControllerHold[i].button = gControllerPress[i].button = 0;
            gControllerHold[i].stick_x = gControllerHold[i].stick_y = 0;
            gControllerPress[i].stick_x = gControllerPress[i].stick_y = 0;
        }
        // A briefing that comes up while skipping is skipped with the game's own START skip.
        if ((gGameState == GSTATE_MAP) && (gMapState == 2) && (Map_GetMapState() == MAP_LEVEL_START) &&
            (Map_GetLevelStartState() == 1)) {
            gControllerPress[gMainController].button |= START_BUTTON;
        }
        return;
    }

    if (sFastForward && !SFMod_SkipShouldContinue()) {
        sFastForward = false;
    }
    if (!sFastForward && (gControllerPress[gMainController].button & START_BUTTON) && SFMod_CanStartSkip()) {
        sFastForward = true;
        // the START press only starts the skip, it must not pause or confirm anything
        gControllerPress[gMainController].button &= ~START_BUTTON;
    }

    // VS CPU mode: the CPU pilots' controllers (ports 2-10) are filled by the AI
    SFModVs_FillInputs();

    for (i = 0; i < (SFModVs_Active() ? MAX_PLAYERS : 4); i++) {
        // gControllerPress is computed against last frame's (already modified) hold state,
        // so the mod tracks the real button state itself.
        u32 rawHold = gControllerHold[i].button;
        u32 rawPress = rawHold & ~sPrevRawHold[i];

        sPrevRawHold[i] = rawHold;

        // The original game never sees the extra buttons.
        gControllerHold[i].button &= ~SFMOD_EXTRA_BUTTONS;
        gControllerPress[i].button &= ~SFMOD_EXTRA_BUTTONS;

        if (SFMod_PortControlsShip(i)) {
            SFMod_ProcessFireButtons(i, rawHold, rawPress);
        } else {
            gControllerPress[i].button = (gControllerPress[i].button & ~A_BUTTON) | (rawPress & A_BUTTON);
            sTurboPhase[i] = 0;
        }

        if (i < 4) { // the CPU pilots of the VS CPU mode steer themselves
            SFMod_ProcessAutoTilt(i, rawHold, rawPress);
        }
    }

    if (sSwapMsgTimer > 0) {
        sSwapMsgTimer--;
    }
}

/* ---------------------------------------------------------------------------------------------
 * HUD
 * --------------------------------------------------------------------------------------------- */

static void SFMod_DrawCenteredSmallText(s32 y, const char* text) {
    s32 width = Graphics_GetSmallTextWidth((char*) text);

    Graphics_DisplaySmallText((SCREEN_WIDTH - width) / 2, y, 1.0f, 1.0f, (char*) text);
}

void SFMod_DrawSwapMessage(void) {
    if ((sSwapMsgTimer > 0) && (gPlayState != PLAY_PAUSE)) {
        RCP_SetupDL(&gMasterDisp, SETUPDL_83);
        gDPSetPrimColor(gMasterDisp++, 0, 0, 255, 255, 0, 255);
        SFMod_DrawCenteredSmallText(44, sSwapMsgSwapped ? "FIRE BUTTONS SWAPPED" : "FIRE BUTTONS NORMAL");
    }
}

static void SFMod_OnHudDrawPost(IEvent* event) {
    SFMod_DrawSwapMessage();
    SFModGame_DrawHud();
}

/* ---------------------------------------------------------------------------------------------
 * Debug log (only when the environment variable SFMOD_DEBUG is set)
 * --------------------------------------------------------------------------------------------- */

static void SFMod_TestUpdate(void);

static bool sDebug = false;
static s32 sDebugShots = 0;
static s32 sDebugRolls = 0;
static s32 sDebugFrames = 0;
static s32 sDebugFastFrames = 0;
static s32 sDebugLast[6] = { -1, -1, -1, -1, -1, -1 };

static void SFMod_OnShootPost(IEvent* event) {
    sDebugShots++;
}

static void SFMod_OnGameUpdatePost(IEvent* event) {
    s32 now[6];
    s32 i;
    bool changed = false;

    SFMod_TestUpdate();

    if (!sDebug) {
        return;
    }

    if (sInFastForwardFrame) {
        sDebugFastFrames++;
    }

    now[0] = gGameState;
    now[1] = gPlayState;
    now[2] = (gPlayer != NULL) ? gPlayer[0].state : -1;
    now[3] = gCurrentLevel;
    now[4] = gLaserStrength[0];
    now[5] = (gGameState == GSTATE_MAP) ? Map_GetMapState() : -1;
    for (i = 0; i < 6; i++) {
        if (now[i] != sDebugLast[i]) {
            changed = true;
        }
        sDebugLast[i] = now[i];
    }
    if (changed) {
        printf("[SFMod] frame %d: gameState=%d playState=%d playerState=%d level=%d laser=%d mapState=%d ff=%d "
               "phase=%d mode=%d lives=%d bombs=%d\n",
               sDebugFrames, now[0], now[1], now[2], now[3], now[4], now[5], sFastForward, gLevelPhase, gLevelMode,
               gLifeCount[0], gBombCount[0]);
        fflush(stdout);
    }

    // barrel rolls of player 1 (auto tilt must never cause one)
    if ((gGameState == GSTATE_PLAY) && (gPlayer != NULL)) {
        static s32 sPrevRollState = 0;
        if ((gPlayer[0].rollState != 0) && (sPrevRollState == 0)) {
            sDebugRolls++;
        }
        sPrevRollState = gPlayer[0].rollState;
    }

    sDebugFrames++;
    if ((sDebugFrames % 30) == 0) {
        printf("[SFMod] second %d: shots=%d fastForwardFrames=%d rolls=%d bank=%.0f tilt=%d boost=%.0f\n",
               sDebugFrames / 30, sDebugShots, sDebugFastFrames, sDebugRolls,
               ((gGameState == GSTATE_PLAY) && (gPlayer != NULL)) ? gPlayer[0].zRotBank : 0.0f, sAutoTiltSide[0],
               ((gGameState == GSTATE_PLAY) && (gPlayer != NULL)) ? gPlayer[0].boostMeter : 0.0f);
        fflush(stdout);
        sDebugShots = 0;
        sDebugFastFrames = 0;
        sDebugRolls = 0;
    }
}

/* ---------------------------------------------------------------------------------------------
 * Test hooks for automated testing (environment variables, never used in normal play)
 *   SFMOD_TEST_LEVEL=<level id>        boot straight into a level
 *   SFMOD_TEST_LASER=<0..2>            laser strength at the start
 *   SFMOD_TEST_COMPLETE_AFTER=<frames> complete the level after that many frames of active play
 * --------------------------------------------------------------------------------------------- */

static s32 sTestCompleteAfter = -1;
static s32 sTestActiveFrames = 0;
static s32 sTestLaser = -1;

bool SFMod_TestBoot(void) {
    static bool sDone = false;
    const char* level = getenv("SFMOD_TEST_LEVEL");
    const char* laser = getenv("SFMOD_TEST_LASER");

    if (!sDone && SFModVs_TestBoot()) {
        sDone = true;
        return true;
    }
    if (!sDone && SFModGame_TestBoot()) {
        sDone = true;
        if (laser != NULL) {
            sTestLaser = atoi(laser);
        }
        return true;
    }

    if (!sDone && (getenv("SFMOD_TEST_ENDING") != NULL)) {
        sDone = true;
        // like the end of the Andross fight: switch directly, keeping the starfield allocated
        gNextGameState = GSTATE_NONE;
        gGameState = GSTATE_ENDING;
        gNextGameStateTimer = 2;
        Play_GenerateStarfield();
        gGreatFoxIntact = 1;
        gLeveLClearStatus[LEVEL_ZONESS] = 1;
        gLeveLClearStatus[LEVEL_KATINA] = 1;
        printf("[SFMod] test boot into the ending\n");
        fflush(stdout);
        return true;
    }

    if (sDone || (level == NULL)) {
        return false;
    }
    sDone = true;

    gNextLevel = atoi(level);
    gNextLevelPhase = 0;
    gNextGameState = GSTATE_PLAY;
    gSavedObjectLoadIndex = 0;
    gSavedPathProgress = 0.0f;
    D_ctx_8017782C = true;
    gHitCount = 0;
    if (laser != NULL) {
        sTestLaser = atoi(laser); // applied once the level has set up the player
    }
    printf("[SFMod] test boot into level %d (laser %d)\n", gNextLevel, sTestLaser);
    fflush(stdout);
    return true;
}

extern void Venom1_80198414(void); // fox_ve1.c: end of Venom 1

static s32 sTestKillAfter = -1;
static s32 sTestGreatFoxHitAfter = -1;

static void SFMod_TestUpdate(void) {
    static s32 sTestLevel = -1;
    static s32 sTestPhase = -1;
    static s32 sTestLives = -1;

    if ((gGameState != GSTATE_PLAY) || (gPlayState != PLAY_UPDATE) || (gPlayer == NULL)) {
        return;
    }
    if ((sTestLaser >= 0) &&
        ((gPlayer[0].state == PLAYERSTATE_LEVEL_INTRO) || (gPlayer[0].state == PLAYERSTATE_ACTIVE))) {
        gLaserStrength[0] = sTestLaser;
        sTestLaser = -1;
    }
    if ((sTestCompleteAfter < 0) && (sTestKillAfter < 0) && (sTestGreatFoxHitAfter < 0)) {
        return;
    }
    // count again for every level, every phase and every try
    if ((gCurrentLevel != sTestLevel) || (gLevelPhase != sTestPhase) || (gLifeCount[0] != sTestLives) ||
        (gPlayer[0].state == PLAYERSTATE_LEVEL_INTRO)) {
        sTestLevel = gCurrentLevel;
        sTestPhase = gLevelPhase;
        sTestLives = gLifeCount[0];
        sTestActiveFrames = 0;
    }
    if (gPlayer[0].state != PLAYERSTATE_ACTIVE) {
        return;
    }
    sTestActiveFrames++;
    if (sTestActiveFrames == sTestGreatFoxHitAfter) {
        printf("[SFMod] test: a missile hits the Great Fox\n");
        fflush(stdout);
        gGreatFoxIntact = false;
    }
    if (sTestActiveFrames == sTestKillAfter) {
        printf("[SFMod] test: shooting the player down (lives %d)\n", gLifeCount[0]);
        fflush(stdout);
        gPlayer[0].shields = 0;
        Player_ApplyDamage(&gPlayer[0], 0, 10);
    }
    if (sTestActiveFrames == sTestCompleteAfter) {
        printf("[SFMod] test: completing level %d phase %d\n", gCurrentLevel, gLevelPhase);
        fflush(stdout);
        gMissionStatus = MISSION_ACCOMPLISHED;
        if (gCurrentLevel == LEVEL_FORTUNA) {
            gMissionStatus = MISSION_COMPLETE;
            gAllRangeEventTimer = 9200;
        } else if (gCurrentLevel == LEVEL_VENOM_1) {
            Venom1_80198414(); // like the end of the boss
        } else {
            if (gCurrentLevel == LEVEL_VENOM_2) {
                gLeveLClearStatus[LEVEL_VENOM_2] = 2; // like a defeated Star Wolf
            }
            gPlayer[0].state = PLAYERSTATE_LEVEL_COMPLETE;
            gPlayer[0].csState = 0;
        }
    }
}

void SFMod_Init(void) {
    const char* completeAfter = getenv("SFMOD_TEST_COMPLETE_AFTER");
    const char* killAfter = getenv("SFMOD_TEST_KILL_AFTER");
    const char* greatFoxHitAfter = getenv("SFMOD_TEST_GREATFOX_HIT_AFTER");

    if (killAfter != NULL) {
        sTestKillAfter = atoi(killAfter);
    }
    if (greatFoxHitAfter != NULL) {
        sTestGreatFoxHitAfter = atoi(greatFoxHitAfter);
    }

    sDebug = getenv("SFMOD_DEBUG") != NULL;
    if (completeAfter != NULL) {
        sTestCompleteAfter = atoi(completeAfter);
    }

    REGISTER_LISTENER(DrawGlobalHUDPostEvent, SFMod_OnHudDrawPost, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(PlayerActionPostShootEvent, SFMod_OnShootPost, EVENT_PRIORITY_NORMAL);
    REGISTER_LISTENER(GamePostUpdateEvent, SFMod_OnGameUpdatePost, EVENT_PRIORITY_NORMAL);
}
