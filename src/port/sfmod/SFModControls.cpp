#include "SFModControls.h"

#include <imgui.h>
#include <string>
#include "libultraship/src/Context.h"
#include "window/gui/IconsFontAwesome4.h"
#include "controller/controldevice/controller/mapping/ControllerAxisDirectionMapping.h"
#include "port/sfmod/SFMod.h"

// Same idea as libultraship's input editor: block the game's input while waiting for a key.
#define SFMOD_CONTROLS_GAME_INPUT_BLOCK_ID 64064064

namespace SFMod {

static const uint8_t sPort = 0; // the mod's settings are for player 1

static std::shared_ptr<Ship::Controller> GetController() {
    return Ship::Context::GetInstance()->GetControlDeck()->GetControllerByPort(sPort);
}

static const char* DeviceIcon(int8_t mappingType) {
    switch (mappingType) {
        case MAPPING_TYPE_GAMEPAD:
            return ICON_FA_GAMEPAD;
        case MAPPING_TYPE_KEYBOARD:
        case MAPPING_TYPE_MOUSE:
            return ICON_FA_KEYBOARD_O;
        default:
            return "?";
    }
}

void ControlsWindow::InitElement() {
}

void ControlsWindow::UpdateElement() {
    auto controlDeck = Ship::Context::GetInstance()->GetControlDeck();

    if (mPopupOpen && ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        controlDeck->BlockGameInput(SFMOD_CONTROLS_GAME_INPUT_BLOCK_ID);
        // keep blocking for a moment after the new key was set
        mGameInputBlockTimer = ImGui::GetIO().Framerate / 3;
        if (mMappingInputBlockTimer != INT32_MAX) {
            mMappingInputBlockTimer--;
            if (mMappingInputBlockTimer <= 0) {
                mMappingInputBlockTimer = INT32_MAX;
            }
        }
    } else {
        mPopupOpen = false;
        if (mGameInputBlockTimer != INT32_MAX) {
            mGameInputBlockTimer--;
            if (mGameInputBlockTimer <= 0) {
                controlDeck->UnblockGameInput(SFMOD_CONTROLS_GAME_INPUT_BLOCK_ID);
                mGameInputBlockTimer = INT32_MAX;
            }
        }
        // ignore the mouse click / controller press that opened the popup
        mMappingInputBlockTimer = ImGui::GetIO().Framerate / 6;
    }
}

void ControlsWindow::DrawButtonAction(const char* label, const char* hint, CONTROLLERBUTTONS_T bitmask) {
    auto button = GetController()->GetButton(bitmask);
    if (button == nullptr) {
        return;
    }

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if ((hint != nullptr) && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", hint);
    }

    ImGui::TableNextColumn();
    for (auto [id, mapping] : button->GetAllButtonMappings()) {
        std::string text = std::string(DeviceIcon(mapping->GetMappingType())) + " " + mapping->GetPhysicalInputName();
        std::string popupId = "SetKey##" + id;

        ImGui::PushID(id.c_str());
        if (ImGui::Button(text.c_str())) {
            ImGui::OpenPopup(popupId.c_str());
            mPopupOpen = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Click to change. Right click to remove.");
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            button->ClearButtonMapping(id);
            ImGui::PopID();
            return; // the mapping list changed
        }
        if (ImGui::BeginPopup(popupId.c_str())) {
            mPopupOpen = true;
            ImGui::Text("%s\n\nPress a key or a controller button...", label);
            if (ImGui::Button("Cancel")) {
                ImGui::CloseCurrentPopup();
            } else if ((mMappingInputBlockTimer == INT32_MAX) &&
                       button->AddOrEditButtonMappingFromRawPress(bitmask, id)) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
        ImGui::SameLine();
    }

    std::string addId = std::string("AddKey##") + std::to_string(bitmask);
    ImGui::PushID(addId.c_str());
    if (ImGui::Button(ICON_FA_PLUS)) {
        ImGui::OpenPopup(addId.c_str());
        mPopupOpen = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Add another key or button for this move");
    }
    if (ImGui::BeginPopup(addId.c_str())) {
        mPopupOpen = true;
        ImGui::Text("%s\n\nPress a key or a controller button...", label);
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        } else if ((mMappingInputBlockTimer == INT32_MAX) && button->AddOrEditButtonMappingFromRawPress(bitmask, "")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void ControlsWindow::DrawStickAction(const char* label, Ship::Direction direction) {
    auto stick = GetController()->GetLeftStick();

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);

    ImGui::TableNextColumn();
    for (auto [id, mapping] : stick->GetAllAxisDirectionMappingByDirection(direction)) {
        std::string text = std::string(DeviceIcon(mapping->GetMappingType())) + " " + mapping->GetPhysicalInputName();
        std::string popupId = "SetStick##" + id;

        ImGui::PushID(id.c_str());
        if (ImGui::Button(text.c_str())) {
            ImGui::OpenPopup(popupId.c_str());
            mPopupOpen = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Click to change. Right click to remove.");
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            stick->ClearAxisDirectionMapping(direction, id);
            ImGui::PopID();
            return;
        }
        if (ImGui::BeginPopup(popupId.c_str())) {
            mPopupOpen = true;
            ImGui::Text("%s\n\nPress a key, move a stick or press a button...", label);
            if (ImGui::Button("Cancel")) {
                ImGui::CloseCurrentPopup();
            } else if ((mMappingInputBlockTimer == INT32_MAX) &&
                       stick->AddOrEditAxisDirectionMappingFromRawPress(direction, id)) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
        ImGui::SameLine();
    }

    std::string addId = std::string("AddStick##") + std::to_string((int) direction);
    ImGui::PushID(addId.c_str());
    if (ImGui::Button(ICON_FA_PLUS)) {
        ImGui::OpenPopup(addId.c_str());
        mPopupOpen = true;
    }
    if (ImGui::BeginPopup(addId.c_str())) {
        mPopupOpen = true;
        ImGui::Text("%s\n\nPress a key, move a stick or press a button...", label);
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        } else if ((mMappingInputBlockTimer == INT32_MAX) &&
                   stick->AddOrEditAxisDirectionMappingFromRawPress(direction, "")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void ControlsWindow::DrawResetButtons() {
    auto controller = GetController();

    ImGui::TextUnformatted("Reset to the default keys:");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_KEYBOARD_O " Keyboard")) {
        controller->ClearAllMappingsForDeviceType(Ship::PhysicalDeviceType::Keyboard);
        controller->AddDefaultMappings(Ship::PhysicalDeviceType::Keyboard);
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_GAMEPAD " Controller")) {
        controller->ClearAllMappingsForDeviceType(Ship::PhysicalDeviceType::SDLGamepad);
        controller->AddDefaultMappings(Ship::PhysicalDeviceType::SDLGamepad);
    }
}

void ControlsWindow::DrawElement() {
    ImGui::TextWrapped("Click a key to change it, then press the new key or controller button. "
                       "\"+\" adds a second key for the same move, right click removes a key.");
    ImGui::Spacing();

    if (ImGui::BeginTable("SFModControls", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Move", ImGuiTableColumnFlags_WidthFixed, 230.0f);
        ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthStretch);

        DrawStickAction("Steer up", Ship::Direction::UP);
        DrawStickAction("Steer down", Ship::Direction::DOWN);
        DrawStickAction("Steer left", Ship::Direction::LEFT);
        DrawStickAction("Steer right", Ship::Direction::RIGHT);

        DrawButtonAction("Fire", "Tap to shoot, hold to charge a lock-on shot", BTN_A);
        DrawButtonAction("Turbo fire", "Fires continuously while held, never charges or locks on",
                         SFMOD_BTN_TURBO);
        DrawButtonAction("Swap fire / turbo", "Swaps what the Fire and Turbo fire buttons do (during play)",
                         SFMOD_BTN_SWAP);
        DrawButtonAction("Smart bomb", nullptr, BTN_B);
        DrawButtonAction("Boost", "Pull back while boosting for a somersault (loop)", BTN_CLEFT);
        DrawButtonAction("Brake", "Pull back while braking for a U-turn (All-Range Mode)", BTN_CDOWN);
        DrawButtonAction("Tilt left / barrel roll", "Hold to tilt, press twice quickly for a barrel roll", BTN_Z);
        DrawButtonAction("Tilt right / barrel roll", "Hold to tilt, press twice quickly for a barrel roll", BTN_R);
        DrawButtonAction("Change view", "Cockpit view / behind the ship", BTN_CUP);
        DrawButtonAction("Answer radio", "Talk to ROB / answer incoming messages", BTN_CRIGHT);
        DrawButtonAction("Pause / skip cutscene", nullptr, BTN_START);

        ImGui::EndTable();
    }

    ImGui::Spacing();
    DrawResetButtons();
    ImGui::Spacing();
    ImGui::TextDisabled("Menus use Fire (A) to confirm and Smart bomb (B) to go back.");
}

} // namespace SFMod
