#pragma once

#include <libultraship/libultraship.h>

namespace SFMod {

// Action based control setup: click a move, then press the key/button that should trigger it.
class ControlsWindow : public Ship::GuiWindow {
  public:
    using Ship::GuiWindow::GuiWindow;

    void InitElement() override;
    void DrawElement() override;
    void UpdateElement() override;

  private:
    void DrawButtonAction(const char* label, const char* hint, CONTROLLERBUTTONS_T bitmask);
    void DrawStickAction(const char* label, Ship::Direction direction);
    void DrawResetButtons();

    bool mPopupOpen = false;
    int32_t mGameInputBlockTimer = INT32_MAX;
    int32_t mMappingInputBlockTimer = INT32_MAX;
};

} // namespace SFMod
