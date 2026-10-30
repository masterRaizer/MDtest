#include "ui/pages/Page.h"

#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <array>

namespace ui {

namespace {

constexpr std::array kSections{"Appearance", "General", "About"};

ImVec4 levelColor(core::LogLevel level) {
    const auto& pal = theme::palette();
    switch (level) {
        case core::LogLevel::Success: return pal.success;
        case core::LogLevel::Warning: return pal.warning;
        case core::LogLevel::Error: return pal.danger;
        default: return pal.textMuted;
    }
}

}  // namespace

std::span<const char* const> SettingsPage::sections() const { return kSections; }

void SettingsPage::draw(int section, const core::AppState& state, core::ActionSink& actions) {
    switch (section) {
        case 0: drawAppearance(state, actions); break;
        case 1: drawGeneral(state, actions); break;
        default: drawAbout(state, actions); break;
    }
}

void SettingsPage::drawAppearance(const core::AppState& state, core::ActionSink& actions) {
    using namespace widgets;

    core::AppearanceSettings draft = state.settings.appearance;
    bool changed = false;

    CardGrid grid("##appearance");
    {
        Card card("Accent colour", "Used for highlights, sliders and the active tab");
        std::array<ImVec4, core::kAccentNames.size()> swatches{};
        for (int i = 0; i < static_cast<int>(swatches.size()); ++i) swatches[static_cast<std::size_t>(i)] = theme::accentPreset(i);
        changed |= colorSwatches("##accent", swatches, draft.accent);
        caption(core::kAccentNames[static_cast<std::size_t>(draft.accent)]);
    }
    {
        Card card("Interface");
        float scale = m_scaleDraft >= 0.0f ? m_scaleDraft : draft.uiScale;
        if (sliderFloat("UI scale", &scale, 0.75f, 1.75f, "%.2fx")) m_scaleDraft = scale;
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            draft.uiScale = scale;
            m_scaleDraft = -1.0f;
            changed = true;
        }
        changed |= sliderFloat("Corner radius", &draft.rounding, 0.0f, 16.0f, "%.0f px");
        changed |= toggle("Animations", &draft.animations, "Smooth transitions for tabs and switches");
    }
    grid.next();
    {
        Card card("Preview");
        button("Primary", ButtonKind::Primary);
        ImGui::SameLine();
        button("Secondary");
        ImGui::SameLine();
        button("Danger", ButtonKind::Danger);
        checkbox("Checkbox", &m_previewCheck);
        toggle("Toggle", &m_previewCheck);
        sliderFloat("Slider", &m_previewSlider, 0.0f, 1.0f);
        ImGui::ProgressBar(m_previewSlider, {-FLT_MIN, 0});
    }

    if (changed) actions.dispatch(core::action::UpdateAppearance{draft});
}

void SettingsPage::drawGeneral(const core::AppState& state, core::ActionSink& actions) {
    using namespace widgets;

    core::GeneralSettings draft = state.settings.general;
    bool changed = false;

    CardGrid grid("##general");
    {
        Card card("Application");
        changed |= combo("Language", &draft.language, core::kLanguages);
        changed |= checkbox("Vertical sync", &draft.vsync, "Match the monitor refresh rate");
        ImGui::BeginDisabled(draft.vsync);
        changed |= combo("FPS limit", &draft.fpsLimit, core::kFpsLimits);
        ImGui::EndDisabled();
        changed |= toggle("FPS overlay", &draft.showFpsOverlay);
    }
    grid.next();
    {
        Card card("Safety");
        changed |= checkbox("Confirm destructive actions", &draft.confirmDestructive);
        ImGui::Spacing();
        if (button("Restore defaults", ButtonKind::Danger)) {
            if (draft.confirmDestructive) m_confirmReset = true;
            else actions.dispatch(core::action::ResetAllSettings{});
        }
        caption("Resets combat, world, loot and application settings.");
    }

    if (changed) actions.dispatch(core::action::UpdateGeneral{draft});

    if (m_confirmReset && !ImGui::IsPopupOpen("Restore defaults?")) ImGui::OpenPopup("Restore defaults?");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    if (ImGui::BeginPopupModal("Restore defaults?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("All settings will be reset. Continue?");
        ImGui::Spacing();
        const float w = 120.0f * ImGui::GetIO().FontGlobalScale;
        if (button("Reset", ButtonKind::Danger, {w, 0})) {
            actions.dispatch(core::action::ResetAllSettings{});
            m_confirmReset = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (button("Cancel", ButtonKind::Secondary, {w, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_confirmReset = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void SettingsPage::drawAbout(const core::AppState& state, core::ActionSink& actions) {
    using namespace widgets;
    const auto& pal = theme::palette();

    CardGrid grid("##about", 260.0f);
    {
        Card card("About");
        ImGui::TextUnformatted("Nova Panel 1.0");
        ImGui::TextColored(pal.textMuted, "Dear ImGui %s", IMGUI_VERSION);
        ImGui::Spacing();
        caption("Views render a read-only snapshot of the state and emit actions. "
                "The controller is the only component that changes the state.");
    }
    grid.next();
    {
        Card card("Activity log");
        if (button("Clear")) actions.dispatch(core::action::ClearLog{});
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(pal.textMuted, "%zu entries", state.log.size());

        ImGui::PushStyleColor(ImGuiCol_ChildBg, pal.background);
        ImGui::BeginChild("##log", {0, 220.0f * ImGui::GetIO().FontGlobalScale}, ImGuiChildFlags_AlwaysUseWindowPadding);
        for (auto it = state.log.rbegin(); it != state.log.rend(); ++it) {
            ImGui::TextColored(pal.textDisabled, "%7.1fs", it->timestamp);
            ImGui::SameLine();
            ImGui::TextColored(levelColor(it->level), "%s", it->message.c_str());
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
}

}  // namespace ui
