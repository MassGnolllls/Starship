#pragma once

/*
 * Star Fox 64 Mod (MassGnolllls/Starship fork)
 *
 * Central place for the mod's settings and small cross-cutting features:
 *  - booting straight into the main menu
 *  - turbo fire + swapping the fire buttons
 *  - skipping cutscenes (fast-forwarding them, so no progress is lost)
 *  - keeping the laser level in Landmaster missions
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

// Extra controller bits. The N64 pad never sets these two bits, so the game ignores them;
// libultraship maps them like any other button (see Engine.cpp).
#define SFMOD_BTN_TURBO 0x0040
#define SFMOD_BTN_SWAP 0x0080
// beyond the N64's 16 buttons (CONTROLLERBUTTONS_T is 32 bit, see CMakeLists.txt)
#define SFMOD_BTN_TURBO_TILT 0x10000 // turbo fire + auto tilt
#define SFMOD_BTN_BRAKE_TILT 0x20000 // brake + auto tilt
#define SFMOD_EXTRA_BUTTONS (SFMOD_BTN_TURBO | SFMOD_BTN_SWAP | SFMOD_BTN_TURBO_TILT | SFMOD_BTN_BRAKE_TILT)

// Game logic frames between two turbo shots. The game logic runs at 30 frames per second,
// so this gives 10 shots per second - the same rate as Starship's rapid-fire cheat.
#define SFMOD_TURBO_INTERVAL 3

#define CVAR_SFMOD_BOOT_TO_MENU "gSFMod.BootToMainMenu"
#define CVAR_SFMOD_SKIP_CUTSCENES "gSFMod.SkipCutscenes"
#define CVAR_SFMOD_TANK_KEEPS_LASER "gSFMod.TankKeepsLaser"
#define CVAR_SFMOD_TEAM_STAYS_BACK "gSFMod.TeamStaysBack"
#define CVAR_SFMOD_FIRE_SWAPPED_PREFIX "gSFMod.FireButtonsSwapped.Port"

void SFMod_Init(void);

// Called once per game frame right after the controllers were read.
void SFMod_ProcessInput(void);

bool SFMod_BootToMenu(void);
bool SFMod_SkipCutscenesEnabled(void);
bool SFMod_TankKeepsLaser(void);
bool SFMod_TeamStaysBack(void);
bool SFMod_FireButtonsSwapped(int32_t port);

// Cutscene fast-forward, driven by the frame loop in Game.cpp
bool SFMod_FastForwardActive(void);
void SFMod_SetFastForwardFrame(bool inFastForwardFrame);
bool SFMod_IsFastForwardFrame(void);

// Automated testing only (environment variables), see SFMod.c
bool SFMod_TestBoot(void);

#ifdef __cplusplus
}
#endif
