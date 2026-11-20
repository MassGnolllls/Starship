#ifndef SFMOD_CONFIG_H
#define SFMOD_CONFIG_H

// @mod: maximum number of players in a match (the original game supports 4).
// Normal modes still use 1 (story) or 4 (VS); the VS CPU mode can use up to 10.
#define MAX_PLAYERS 10

// @mod: more player shot slots so up to 10 players can fire at the same time.
// Slots 0-15 keep their original meaning: 15 is the (shared) smart bomb slot and 11-14 are the
// charged shots of players 0-3. Players 4-9 get their charged shot slot at 16-21.
#define PLAYERSHOT_COUNT 128
#define PLAYERSHOT_BOMB_SLOT 15
#define PLAYERSHOT_LOCKON_SLOT(num) (((num) < 4) ? (14 - (num)) : (16 + (num) - 4))

// @mod: VS lock-ons of all MAX_PLAYERS players (the original cleared the four entries by hand).
// gVsLockOnTimers[target][locker]
#define SFMOD_CLEAR_LOCKONS_ON(target)                     \
    do {                                                   \
        int sfmodLockIdx_;                                 \
        for (sfmodLockIdx_ = 0; sfmodLockIdx_ < MAX_PLAYERS; sfmodLockIdx_++) { \
            gVsLockOnTimers[target][sfmodLockIdx_] = 0;   \
        }                                                  \
    } while (0)
#define SFMOD_CLEAR_LOCKONS_BY(locker)                     \
    do {                                                   \
        int sfmodLockIdx_;                                 \
        for (sfmodLockIdx_ = 0; sfmodLockIdx_ < MAX_PLAYERS; sfmodLockIdx_++) { \
            gVsLockOnTimers[sfmodLockIdx_][locker] = 0;   \
        }                                                  \
    } while (0)

#endif
