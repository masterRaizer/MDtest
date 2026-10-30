#include "core/AppState.h"

#include <cmath>
#include <numeric>

namespace core {

AppState makeDemoState() {
    AppState state;

    auto& inv = state.inventory;
    auto add = [&inv](std::string name, ItemCategory cat, Rarity rarity, int qty, float weight, bool equipped = false) {
        inv.items.push_back(Item{
            .id = inv.nextItemId++,
            .name = std::move(name),
            .category = cat,
            .rarity = rarity,
            .quantity = qty,
            .weight = weight,
            .equipped = equipped,
        });
    };
    add("Stormcaller Blade", ItemCategory::Weapon, Rarity::Legendary, 1, 4.2f, true);
    add("Hunter's Longbow", ItemCategory::Weapon, Rarity::Rare, 1, 2.1f);
    add("Reinforced Leather", ItemCategory::Armor, Rarity::Uncommon, 1, 6.5f, true);
    add("Health Potion", ItemCategory::Consumable, Rarity::Common, 6, 0.3f);
    add("Elixir of Vigor", ItemCategory::Consumable, Rarity::Epic, 2, 0.4f);
    add("Iron Ore", ItemCategory::Material, Rarity::Common, 14, 0.8f);
    add("Dragon Scale", ItemCategory::Material, Rarity::Epic, 3, 1.2f);
    add("Ancient Map Fragment", ItemCategory::Quest, Rarity::Rare, 1, 0.1f);

    auto& locs = state.explorer.locations;
    locs = {
        {"Silverbrook Village", "Meadowlands", {0, 0, 0}, true},
        {"Whispering Woods", "Meadowlands", {320, 12, -180}, true},
        {"Ashen Crossing", "Badlands", {-540, 4, 260}, true},
        {"Sunken Temple", "Mirefen Marsh", {780, -18, 640}, false},
        {"Frostpeak Summit", "Highlands", {-120, 940, -1320}, true},
        {"Ironhold Keep", "Highlands", {210, 380, -960}, false},
        {"Obsidian Spire", "Badlands", {-1180, 220, 880}, false},
    };

    state.log.push_back({0.0, LogLevel::Info, "Session started"});
    return state;
}

float totalWeight(const InventoryState& inventory) {
    return std::accumulate(inventory.items.begin(), inventory.items.end(), 0.0f,
                           [](float sum, const Item& item) { return sum + item.weight * static_cast<float>(item.quantity); });
}

float distance(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace core
