#include <libultraship.h>
#include <chrono>

#include <Fast3D/interpreter.h>
#include "Engine.h"

extern "C" {
#include <sf64mesg.h>
    void Main_SetVIMode(void);
    void Main_Initialize(void);
    void Main_ThreadEntry(void* arg);
    void Lib_FillScreen(u8 setFill);
    void Graphics_ThreadUpdate();
    void Graphics_ThreadUpdateNoPresent();
    void AudioThread_CreateTask();
    bool SFMod_FastForwardActive(void);
    void SFMod_SetFastForwardFrame(bool inFastForwardFrame);
}

extern "C"
void Graphics_PushFrame(Gfx* data) {
    GameEngine::ProcessGfxCommands(data);
}

extern "C" void Timer_Update();

// @mod: skipping a cutscene runs the game logic as fast as possible (within a time budget per
// displayed frame) without showing those frames or playing their sound.
static void SFMod_RunFastForward() {
    if (!SFMod_FastForwardActive()) {
        return;
    }

    const auto start = std::chrono::steady_clock::now();
    GameEngine::SetDiscardAudio(true);
    for (int i = 0; (i < 400) && SFMod_FastForwardActive(); i++) {
        SFMod_SetFastForwardFrame(true);
        Graphics_ThreadUpdateNoPresent();
        GameEngine::StartAudioFrame();
        Timer_Update();
        GameEngine::EndAudioFrame();
        SFMod_SetFastForwardFrame(false);

        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(25)) {
            break;
        }
    }
    GameEngine::SetDiscardAudio(false);
}

void push_frame() {
    SFMod_RunFastForward();
    Graphics_ThreadUpdate();
    GameEngine::StartAudioFrame();
    GameEngine::Instance->StartFrame();
    Timer_Update();
    // thread5_iteration();
    GameEngine::EndAudioFrame();
}

#ifdef _WIN32
int SDL_main(int argc, char **argv) {
#else
#if defined(__cplusplus) && defined(PLATFORM_IOS)
extern "C"
#endif
int main(int argc, char *argv[]) {
#endif
    GameEngine::Create();
    Main_SetVIMode();
    Lib_FillScreen(1);
    Main_Initialize();
    Main_ThreadEntry(NULL);
    while (WindowIsRunning()) {
        push_frame();
    }
    GameEngine::Instance->Destroy();
    return 0;
}