#include "ui/pages/Page.h"

#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <array>

namespace ui {

namespace {
constexpr std::array kSections{"Stats", "Movement", "Camera"};
}

std::span<const char* const> PlayerPage::sections() const { return kSections; }

void PlayerPage::draw(int section, const core::AppState& state, core::ActionSink& actions) {
    using namespace widgets;
    const auto& pal = theme::palette();

    core::PlayerSettings draft = state.player;
    bool changed = false;

    CardGrid grid("##player");
    switch (section) {
        case 0: {
            {
                Card card("Vitals");
                statBar("Health", draft.health, draft.maxHealth, pal.success);
                statBar("Stamina", draft.stamina, draft.maxStamina, pal.warning);
                ImGui::Spacing();
                if (button("Restore", ButtonKind::Primary)) actions.dispatch(core::action::HealPlayer{});
                ImGui::SameLine();
                if (button("Reset player", ButtonKind::Danger)) actions.dispatch(core::action::ResetPlayer{});
            }
            {
                Card card("Character");
                changed |= combo("Class", &draft.playerClass, core::kPlayerClasses);
                changed |= sliderInt("Level", &draft.level, 1, 100);
            }
            grid.next();
            {
                Card card("Modifiers", "Debug switches for testing");
                changed |= toggle("God mode", &draft.godMode, "Ignore all incoming damage");
                changed |= toggle("Infinite stamina", &draft.infiniteStamina);
                changed |= toggle("Health regeneration", &draft.healthRegen, "Restore 1.5 HP per second");
            }
            break;
        }
        case 1: {
            {
                Card card("Speed");
                changed |= sliderFloat("Move speed", &draft.moveSpeed, 0.25f, 3.0f, "%.2fx");
                changed |= sliderFloat("Sprint multiplier", &draft.sprintMultiplier, 1.0f, 3.0f, "%.1fx");
                changed |= sliderFloat("Jump height", &draft.jumpHeight, 0.5f, 3.0f, "%.1fx");
            }
            grid.next();
            {
                Card card("Behaviour");
                changed |= checkbox("Auto sprint", &draft.autoSprint, "Sprint whenever stamina is above 50%");
                changed |= checkbox("Fly mode", &draft.flyMode, "Free movement, ignores collision");
            }
            break;
        }
        default: {
            {
                Card card("View");
                changed |= combo("Camera mode", &draft.cameraMode, core::kCameraModes);
                changed |= sliderFloat("Field of view", &draft.fov, 60.0f, 120.0f, "%.0f°");
            }
            grid.next();
            {
                Card card("Input");
                changed |= sliderFloat("Sensitivity", &draft.sensitivity, 0.1f, 5.0f, "%.2f");
                changed |= checkbox("Invert Y axis", &draft.invertY);
                changed |= checkbox("Head bob", &draft.headBob);
            }
            break;
        }
    }

    if (changed) actions.dispatch(core::action::UpdatePlayer{draft});
}

}  // namespace ui
