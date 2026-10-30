#include "ui/pages/Page.h"

#include "ui/Widgets.h"

#include <array>

namespace ui {

namespace {
constexpr std::array kSections{"General", "Targeting", "Feedback"};
}

std::span<const char* const> CombatPage::sections() const { return kSections; }

void CombatPage::draw(int section, const core::AppState& state, core::ActionSink& actions) {
    using namespace widgets;

    // Edit a draft copy; the controller validates and commits it.
    core::CombatSettings draft = state.combat;
    bool changed = false;

    CardGrid grid("##combat");
    switch (section) {
        case 0: {
            {
                Card card("Automation", "Let the character act on its own");
                changed |= toggle("Auto attack", &draft.autoAttack, "Attack the current target continuously");
                changed |= toggle("Auto block", &draft.autoBlock, "Raise the shield when an attack is incoming");
            }
            grid.next();
            {
                Card card("Tuning");
                changed |= combo("Difficulty", &draft.difficulty, core::kDifficulties);
                changed |= sliderFloat("Attack speed", &draft.attackSpeed, 0.25f, 3.0f, "%.2fx");
                changed |= sliderInt("Combo length", &draft.comboLength, 1, 10, "%d hits");
            }
            break;
        }
        case 1: {
            {
                Card card("Target selection");
                changed |= combo("Priority", &draft.targetPriority, core::kTargetPriorities);
                changed |= sliderFloat("Lock range", &draft.lockRange, 5.0f, 100.0f, "%.0f m");
            }
            grid.next();
            {
                Card card("Filters");
                changed |= checkbox("Lock on target", &draft.targetLock, "Camera follows the selected enemy");
                changed |= checkbox("Ignore neutral NPCs", &draft.ignoreNeutral);
            }
            break;
        }
        default: {
            {
                Card card("Indicators");
                changed |= checkbox("Damage numbers", &draft.damageNumbers);
                ImGui::BeginDisabled(!draft.damageNumbers);
                changed |= combo("Number style", &draft.damageNumberStyle, core::kDamageNumberStyles);
                ImGui::EndDisabled();
                changed |= checkbox("Hit markers", &draft.hitMarkers);
            }
            grid.next();
            {
                Card card("Impact", "How strongly hits are felt");
                changed |= sliderFloat("Screen shake", &draft.screenShake, 0.0f, 1.0f, "%.2f");
                changed |= sliderFloat("Hit stop", &draft.hitStopMs, 0.0f, 250.0f, "%.0f ms");
            }
            break;
        }
    }

    if (changed) actions.dispatch(core::action::UpdateCombat{draft});
}

}  // namespace ui
