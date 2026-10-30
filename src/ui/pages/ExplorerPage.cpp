#include "ui/pages/Page.h"

#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <array>

namespace ui {

namespace {

constexpr std::array kSections{"Locations", "World"};

const char* dayPhase(float hours) {
    if (hours < 5.0f) return "Night";
    if (hours < 8.0f) return "Dawn";
    if (hours < 17.0f) return "Day";
    if (hours < 20.0f) return "Dusk";
    return "Night";
}

}  // namespace

std::span<const char* const> ExplorerPage::sections() const { return kSections; }

void ExplorerPage::draw(int section, const core::AppState& state, core::ActionSink& actions) {
    if (section == 0) drawLocations(state, actions);
    else drawWorld(state, actions);
}

void ExplorerPage::drawLocations(const core::AppState& state, core::ActionSink& actions) {
    using namespace widgets;
    const auto& pal = theme::palette();
    const auto& ex = state.explorer;
    if (ex.locations.empty()) return;

    m_selected = std::clamp(m_selected, 0, static_cast<int>(ex.locations.size()) - 1);
    const core::Vec3& here = ex.locations[static_cast<std::size_t>(ex.currentLocation)].position;

    CardGrid grid("##explorer", 300.0f);
    {
        Card card("Known locations", "Double-click to travel. Your location is highlighted.");
        checkbox("Only discovered", &m_onlyDiscovered);

        constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##locations", 3, kFlags)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.2f);
            ImGui::TableSetupColumn("Region", ImGuiTableColumnFlags_WidthStretch, 1.5f);
            ImGui::TableSetupColumn("Distance", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableHeadersRow();

            for (int i = 0; i < static_cast<int>(ex.locations.size()); ++i) {
                const core::Location& loc = ex.locations[static_cast<std::size_t>(i)];
                if (m_onlyDiscovered && !loc.discovered) continue;

                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();

                const bool isHere = i == ex.currentLocation;
                ImGui::PushStyleColor(ImGuiCol_Text, isHere ? pal.accent : loc.discovered ? pal.text : pal.textDisabled);
                const char* name = loc.discovered ? loc.name.c_str() : "???";
                if (ImGui::Selectable(name, m_selected == i, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    m_selected = i;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) actions.dispatch(core::action::TeleportTo{i});
                }
                ImGui::PopStyleColor();
                if (isHere) ImGui::SetItemTooltip("You are here");

                ImGui::TableNextColumn();
                ImGui::TextColored(pal.textMuted, "%s", loc.region.c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(pal.textMuted, "%.0f m", core::distance(here, loc.position));
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    grid.next();
    {
        const core::Location& loc = ex.locations[static_cast<std::size_t>(m_selected)];
        const bool isHere = m_selected == ex.currentLocation;

        Card card("Details");
        ImGui::PushFont(theme::fonts().heading);
        ImGui::TextUnformatted(loc.discovered ? loc.name.c_str() : "Undiscovered");
        ImGui::PopFont();
        caption(loc.region.c_str());
        ImGui::Spacing();

        if (isHere) badge("Current location", pal.accent);
        else if (loc.discovered) badge("Discovered", pal.success);
        else badge("Hidden", pal.warning);

        ImGui::Spacing();
        ImGui::TextColored(pal.textMuted, "Coordinates");
        ImGui::Text("%.0f, %.0f, %.0f", loc.position.x, loc.position.y, loc.position.z);
        ImGui::TextColored(pal.textMuted, "Distance");
        ImGui::Text("%.0f m", core::distance(here, loc.position));
        ImGui::Spacing();

        ImGui::BeginDisabled(!loc.discovered || isHere);
        if (button("Fast travel", ButtonKind::Primary)) actions.dispatch(core::action::TeleportTo{m_selected});
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (button("Reveal all")) actions.dispatch(core::action::RevealAllLocations{});
    }
}

void ExplorerPage::drawWorld(const core::AppState& state, core::ActionSink& actions) {
    using namespace widgets;
    const auto& pal = theme::palette();

    core::WorldSettings draft = state.explorer.world;
    bool changed = false;

    CardGrid grid("##world");
    {
        Card card("Time of day");
        const int hours = static_cast<int>(draft.timeOfDay);
        const int minutes = static_cast<int>((draft.timeOfDay - static_cast<float>(hours)) * 60.0f);
        ImGui::PushFont(theme::fonts().heading);
        ImGui::Text("%02d:%02d", hours, minutes);
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(pal.textMuted, "%s", dayPhase(draft.timeOfDay));

        changed |= sliderFloat("Time", &draft.timeOfDay, 0.0f, 23.99f, "%.1f h");
        changed |= checkbox("Freeze time", &draft.freezeTime);
        ImGui::BeginDisabled(draft.freezeTime);
        changed |= sliderFloat("Time scale", &draft.timeScale, 1.0f, 600.0f, "%.0f min/s");
        ImGui::EndDisabled();
    }
    {
        Card card("Minimap");
        changed |= toggle("Show minimap", &draft.showMinimap);
        ImGui::BeginDisabled(!draft.showMinimap);
        changed |= sliderFloat("Zoom", &draft.minimapZoom, 0.5f, 4.0f, "%.1fx");
        ImGui::EndDisabled();
    }
    grid.next();
    {
        Card card("Environment");
        changed |= combo("Weather", &draft.weather, core::kWeatherTypes);
        changed |= toggle("Fog of war", &draft.fogOfWar, "Hide unexplored areas on the map");
        changed |= sliderFloat("Render distance", &draft.renderDistance, 100.0f, 3000.0f, "%.0f m");
    }

    if (changed) actions.dispatch(core::action::UpdateWorld{draft});
}

}  // namespace ui
