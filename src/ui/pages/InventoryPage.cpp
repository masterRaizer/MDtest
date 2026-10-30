#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui/pages/Page.h"

#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace ui {

namespace {

constexpr std::array kSections{"Items", "Loot rules"};
constexpr std::array kCategoryFilter{"All categories", "Weapon", "Armor", "Consumable", "Material", "Quest"};

bool containsIgnoreCase(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    const auto it = std::ranges::search(haystack, needle, [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return !it.empty();
}

const char* useLabel(const core::Item& item) {
    switch (item.category) {
        case core::ItemCategory::Weapon:
        case core::ItemCategory::Armor: return item.equipped ? "Unequip" : "Equip";
        case core::ItemCategory::Consumable: return "Use";
        default: return nullptr;
    }
}

}  // namespace

std::span<const char* const> InventoryPage::sections() const { return kSections; }

void InventoryPage::draw(int section, const core::AppState& state, core::ActionSink& actions) {
    if (section == 0) drawItems(state, actions);
    else drawLootRules(state, actions);
}

void InventoryPage::drawItems(const core::AppState& state, core::ActionSink& actions) {
    using namespace widgets;
    const auto& pal = theme::palette();
    const auto& inv = state.inventory;
    const float s = ImGui::GetIO().FontGlobalScale;

    Card card("Backpack", "Use, equip or drop items. Quest items are protected.");

    // ---- Toolbar ----
    ImGui::SetNextItemWidth(200.0f * s);
    ImGui::InputTextWithHint("##search", "Search items...", m_search, sizeof m_search);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f * s);
    ImGui::Combo("##category", &m_categoryFilter, kCategoryFilter.data(), static_cast<int>(kCategoryFilter.size()));
    ImGui::SameLine();

    core::InventoryPrefs prefs = inv.prefs;
    ImGui::SetNextItemWidth(120.0f * s);
    bool prefsChanged = ImGui::Combo("##sort", &prefs.sortMode, core::kSortModes.data(), static_cast<int>(core::kSortModes.size()));
    ImGui::SetItemTooltip("Sort by");
    ImGui::SameLine();
    if (button(prefs.sortDescending ? "Desc" : "Asc")) {
        prefs.sortDescending = !prefs.sortDescending;
        prefsChanged = true;
    }
    if (prefsChanged) actions.dispatch(core::action::UpdateInventoryPrefs{prefs});

    const char* lootLabel = "+ Loot random item";
    const float lootW = ImGui::CalcTextSize(lootLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine(0, 0);
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + ImGui::GetStyle().ItemSpacing.x,
                                  ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - lootW));
    if (button(lootLabel, ButtonKind::Primary)) actions.dispatch(core::action::SpawnRandomItem{});

    ImGui::Spacing();
    const float weight = core::totalWeight(inv);
    statBar("Carry weight (kg)", weight, inv.maxWeight, weight > inv.maxWeight * 0.85f ? pal.danger : pal.accent);
    ImGui::Spacing();

    // ---- Table ----
    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                       ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;
    if (ImGui::BeginTable("##items", 6, kFlags)) {
        ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Rarity", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Qty", ImGuiTableColumnFlags_WidthStretch, 0.7f);
        ImGui::TableSetupColumn("Weight", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, 160.0f * s);
        ImGui::TableHeadersRow();

        int shown = 0;
        for (const core::Item& item : inv.items) {
            if (m_categoryFilter > 0 && static_cast<int>(item.category) != m_categoryFilter - 1) continue;
            if (!containsIgnoreCase(item.name, m_search)) continue;
            ++shown;

            ImGui::PushID(item.id);
            ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeightWithSpacing());
            const ImVec4 rarity = theme::rarityColor(item.rarity);

            ImGui::TableNextColumn();
            {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float h = ImGui::GetFrameHeight();
                ImGui::GetWindowDrawList()->AddCircleFilled(p + ImVec2{4.0f * s, h * 0.5f}, 4.0f * s,
                                                            ImGui::ColorConvertFloat4ToU32(rarity));
                ImGui::SetCursorScreenPos(p + ImVec2{14.0f * s, 0});
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(item.name.c_str());
                if (item.equipped) {
                    ImGui::SameLine();
                    badge("Equipped", pal.accent);
                }
            }

            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(pal.textMuted, "%s", core::kItemCategoryNames[static_cast<std::size_t>(item.category)]);

            ImGui::TableNextColumn();
            badge(core::kRarityNames[static_cast<std::size_t>(item.rarity)], rarity);

            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("x%d", item.quantity);

            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(pal.textMuted, "%.1f kg", item.weight * static_cast<float>(item.quantity));

            ImGui::TableNextColumn();
            if (const char* label = useLabel(item)) {
                if (button(label, ButtonKind::Secondary, {80.0f * s, 0})) actions.dispatch(core::action::UseItem{item.id});
                ImGui::SameLine();
            }
            if (button("Drop", ButtonKind::Danger)) {
                if (inv.prefs.confirmDrop) m_pendingDropId = item.id;
                else actions.dispatch(core::action::DropItem{item.id});
            }
            ImGui::PopID();
        }
        ImGui::EndTable();

        if (shown == 0) caption("No items match the current filter.");
    }

    // ---- Confirmation modal (opened outside the per-row ID scope) ----
    if (m_pendingDropId != -1 && !ImGui::IsPopupOpen("Drop item?")) ImGui::OpenPopup("Drop item?");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    if (ImGui::BeginPopupModal("Drop item?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto it = std::ranges::find(inv.items, m_pendingDropId, &core::Item::id);
        if (it == inv.items.end()) {
            m_pendingDropId = -1;
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::Text("Drop %s x%d?", it->name.c_str(), it->quantity);
            caption("Dropped items are gone for good.");
            ImGui::Spacing();
            if (button("Drop", ButtonKind::Danger, {110.0f * s, 0})) {
                actions.dispatch(core::action::DropItem{m_pendingDropId});
                m_pendingDropId = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (button("Cancel", ButtonKind::Secondary, {110.0f * s, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                m_pendingDropId = -1;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
}

void InventoryPage::drawLootRules(const core::AppState& state, core::ActionSink& actions) {
    using namespace widgets;

    core::InventoryPrefs prefs = state.inventory.prefs;
    bool changed = false;

    CardGrid grid("##loot");
    {
        Card card("Pickup");
        changed |= toggle("Auto loot", &prefs.autoLoot, "Collect drops when walking over them");
        changed |= combo("Loot filter", &prefs.lootFilter, core::kLootFilters);
        changed |= checkbox("Sort on pickup", &prefs.autoSortOnPickup);
    }
    grid.next();
    {
        Card card("Organisation");
        changed |= combo("Sort by", &prefs.sortMode, core::kSortModes);
        changed |= checkbox("Descending order", &prefs.sortDescending);
        changed |= checkbox("Confirm before dropping", &prefs.confirmDrop);
        ImGui::Spacing();
        if (button("Sort now", ButtonKind::Primary)) actions.dispatch(core::action::SortInventory{});
    }

    if (changed) actions.dispatch(core::action::UpdateInventoryPrefs{prefs});
}

}  // namespace ui
