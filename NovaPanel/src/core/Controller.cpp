#include "core/Controller.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>

namespace core {

namespace {

template <class T>
constexpr T clampIndex(T value, std::size_t count) {
    return std::clamp<T>(value, 0, static_cast<T>(count) - 1);
}

constexpr auto idx(auto e) { return static_cast<std::size_t>(e); }

}  // namespace

Controller::Controller(AppState& state) : m_state(state) { sortInventory(); }

void Controller::handle(const Action& action) {
    std::visit([this](const auto& a) { apply(a); }, action);
}

void Controller::process(ActionQueue& queue) {
    for (const Action& action : queue.drain()) handle(action);
}

void Controller::tick(float dt) {
    m_state.clock += dt;

    auto& world = m_state.explorer.world;
    if (!world.freezeTime) {
        world.timeOfDay = std::fmod(world.timeOfDay + dt * world.timeScale / 60.0f, 24.0f);
    }

    auto& p = m_state.player;
    if (p.healthRegen && p.health < p.maxHealth) p.health = std::min(p.maxHealth, p.health + 1.5f * dt);
    if (p.infiniteStamina) p.stamina = p.maxStamina;
    else if (p.stamina < p.maxStamina) p.stamina = std::min(p.maxStamina, p.stamina + 8.0f * dt);
}

// ------------------------------------------------------------- Settings ----

void Controller::apply(const action::UpdateCombat& a) {
    CombatSettings v = a.value;
    v.difficulty = clampIndex(v.difficulty, kDifficulties.size());
    v.targetPriority = clampIndex(v.targetPriority, kTargetPriorities.size());
    v.damageNumberStyle = clampIndex(v.damageNumberStyle, kDamageNumberStyles.size());
    v.attackSpeed = std::clamp(v.attackSpeed, 0.25f, 3.0f);
    v.comboLength = std::clamp(v.comboLength, 1, 10);
    v.lockRange = std::clamp(v.lockRange, 5.0f, 100.0f);
    v.screenShake = std::clamp(v.screenShake, 0.0f, 1.0f);
    v.hitStopMs = std::clamp(v.hitStopMs, 0.0f, 250.0f);

    if (v.difficulty != m_state.combat.difficulty)
        log(LogLevel::Info, std::format("Difficulty set to {}", kDifficulties[idx(v.difficulty)]));
    m_state.combat = v;
}

void Controller::apply(const action::UpdatePlayer& a) {
    PlayerSettings v = a.value;
    v.playerClass = clampIndex(v.playerClass, kPlayerClasses.size());
    v.cameraMode = clampIndex(v.cameraMode, kCameraModes.size());
    v.level = std::clamp(v.level, 1, 100);
    v.maxHealth = 80.0f + static_cast<float>(v.level) * 2.0f;
    v.health = std::clamp(v.health, 0.0f, v.maxHealth);
    v.stamina = std::clamp(v.stamina, 0.0f, v.maxStamina);
    v.moveSpeed = std::clamp(v.moveSpeed, 0.25f, 3.0f);
    v.jumpHeight = std::clamp(v.jumpHeight, 0.5f, 3.0f);
    v.sprintMultiplier = std::clamp(v.sprintMultiplier, 1.0f, 3.0f);
    v.fov = std::clamp(v.fov, 60.0f, 120.0f);
    v.sensitivity = std::clamp(v.sensitivity, 0.1f, 5.0f);

    if (v.godMode != m_state.player.godMode)
        log(LogLevel::Warning, v.godMode ? "God mode enabled" : "God mode disabled");
    if (v.playerClass != m_state.player.playerClass)
        log(LogLevel::Info, std::format("Class changed to {}", kPlayerClasses[idx(v.playerClass)]));
    m_state.player = v;
}

void Controller::apply(const action::UpdateWorld& a) {
    WorldSettings v = a.value;
    v.weather = clampIndex(v.weather, kWeatherTypes.size());
    v.timeOfDay = std::clamp(v.timeOfDay, 0.0f, 23.99f);
    v.timeScale = std::clamp(v.timeScale, 1.0f, 600.0f);
    v.minimapZoom = std::clamp(v.minimapZoom, 0.5f, 4.0f);
    v.renderDistance = std::clamp(v.renderDistance, 100.0f, 3000.0f);

    if (v.weather != m_state.explorer.world.weather)
        log(LogLevel::Info, std::format("Weather changed to {}", kWeatherTypes[idx(v.weather)]));
    m_state.explorer.world = v;
}

void Controller::apply(const action::UpdateInventoryPrefs& a) {
    InventoryPrefs v = a.value;
    v.sortMode = clampIndex(v.sortMode, kSortModes.size());
    v.lootFilter = clampIndex(v.lootFilter, kLootFilters.size());

    const bool resort = v.sortMode != m_state.inventory.prefs.sortMode ||
                        v.sortDescending != m_state.inventory.prefs.sortDescending;
    m_state.inventory.prefs = v;
    if (resort) sortInventory();
}

void Controller::apply(const action::UpdateAppearance& a) {
    AppearanceSettings v = a.value;
    v.accent = clampIndex(v.accent, kAccentNames.size());
    v.uiScale = std::clamp(v.uiScale, 0.75f, 1.75f);
    v.rounding = std::clamp(v.rounding, 0.0f, 16.0f);

    if (v == m_state.settings.appearance) return;
    m_state.settings.appearance = v;
    if (onAppearanceChanged) onAppearanceChanged(v);
}

void Controller::apply(const action::UpdateGeneral& a) {
    GeneralSettings v = a.value;
    v.language = clampIndex(v.language, kLanguages.size());
    v.fpsLimit = clampIndex(v.fpsLimit, kFpsLimits.size());
    m_state.settings.general = v;
}

void Controller::apply(const action::ResetAllSettings&) {
    const AppState defaults;
    m_state.combat = defaults.combat;
    m_state.inventory.prefs = defaults.inventory.prefs;
    m_state.explorer.world = defaults.explorer.world;
    m_state.settings = defaults.settings;
    if (onAppearanceChanged) onAppearanceChanged(m_state.settings.appearance);
    log(LogLevel::Success, "All settings restored to defaults");
}

// --------------------------------------------------------------- Player ----

void Controller::apply(const action::HealPlayer&) {
    m_state.player.health = m_state.player.maxHealth;
    m_state.player.stamina = m_state.player.maxStamina;
    log(LogLevel::Success, "Player fully restored");
}

void Controller::apply(const action::ResetPlayer&) {
    m_state.player = PlayerSettings{};
    log(LogLevel::Info, "Player settings reset");
}

// ------------------------------------------------------------ Inventory ----

void Controller::apply(const action::UseItem& a) {
    auto& items = m_state.inventory.items;
    const auto it = std::ranges::find(items, a.itemId, &Item::id);
    if (it == items.end()) return;

    switch (it->category) {
        case ItemCategory::Weapon:
        case ItemCategory::Armor:
            // Only one item per slot can be equipped.
            if (!it->equipped)
                for (Item& other : items)
                    if (other.category == it->category) other.equipped = false;
            it->equipped = !it->equipped;
            log(LogLevel::Info, std::format("{} {}", it->equipped ? "Equipped" : "Unequipped", it->name));
            break;

        case ItemCategory::Consumable: {
            const float heal = 15.0f * static_cast<float>(idx(it->rarity) + 1);
            auto& p = m_state.player;
            p.health = std::min(p.maxHealth, p.health + heal);
            log(LogLevel::Success, std::format("Used {} (+{:.0f} HP)", it->name, heal));
            if (--it->quantity <= 0) items.erase(it);
            break;
        }

        default:
            log(LogLevel::Warning, std::format("{} can't be used directly", it->name));
            break;
    }
}

void Controller::apply(const action::DropItem& a) {
    auto& items = m_state.inventory.items;
    const auto it = std::ranges::find(items, a.itemId, &Item::id);
    if (it == items.end()) return;

    if (it->category == ItemCategory::Quest) {
        log(LogLevel::Error, std::format("Quest item '{}' can't be dropped", it->name));
        return;
    }
    log(LogLevel::Info, std::format("Dropped {} x{}", it->name, it->quantity));
    items.erase(it);
}

void Controller::apply(const action::SpawnRandomItem&) {
    static constexpr std::array kNames{
        "Rusty Dagger", "Oak Shield", "Moonpetal Herb", "Silver Ingot", "Phoenix Feather",
        "Warden's Helm", "Bread Loaf", "Runed Gemstone", "Venom Flask", "Spellbound Staff",
    };
    static constexpr std::array kCategories{
        ItemCategory::Weapon, ItemCategory::Armor, ItemCategory::Consumable, ItemCategory::Material, ItemCategory::Material,
        ItemCategory::Armor, ItemCategory::Consumable, ItemCategory::Material, ItemCategory::Consumable, ItemCategory::Weapon,
    };
    static_assert(kNames.size() == kCategories.size());

    auto& inv = m_state.inventory;
    const std::size_t pick = std::uniform_int_distribution<std::size_t>(0, kNames.size() - 1)(m_rng);
    // Skewed towards common items: 40/28/18/10/4 %.
    const auto rarity = static_cast<Rarity>(std::discrete_distribution<int>({40, 28, 18, 10, 4})(m_rng));

    if (static_cast<int>(rarity) < inv.prefs.lootFilter) {
        log(LogLevel::Info, std::format("Skipped {} ({}) - below loot filter", kNames[pick], kRarityNames[idx(rarity)]));
        return;
    }

    const float weight = std::uniform_real_distribution<float>(0.2f, 5.0f)(m_rng);
    if (totalWeight(inv) + weight > inv.maxWeight) {
        log(LogLevel::Error, "Inventory is too heavy to pick up more items");
        return;
    }

    inv.items.push_back(Item{
        .id = inv.nextItemId++,
        .name = kNames[pick],
        .category = kCategories[pick],
        .rarity = rarity,
        .quantity = 1,
        .weight = weight,
    });
    log(LogLevel::Success, std::format("Picked up {} ({})", kNames[pick], kRarityNames[idx(rarity)]));
    if (inv.prefs.autoSortOnPickup) sortInventory();
}

void Controller::apply(const action::SortInventory&) {
    sortInventory();
    log(LogLevel::Info, std::format("Inventory sorted by {}", kSortModes[idx(m_state.inventory.prefs.sortMode)]));
}

void Controller::sortInventory() {
    const auto& prefs = m_state.inventory.prefs;
    auto less = [mode = prefs.sortMode](const Item& a, const Item& b) {
        switch (mode) {
            case 0: return a.name < b.name;
            case 1: return a.rarity < b.rarity;
            case 2: return a.category < b.category;
            default: return a.weight * a.quantity < b.weight * b.quantity;
        }
    };
    auto& items = m_state.inventory.items;
    if (prefs.sortDescending)
        std::ranges::stable_sort(items, [&](const Item& a, const Item& b) { return less(b, a); });
    else
        std::ranges::stable_sort(items, less);
}

// ------------------------------------------------------------- Explorer ----

void Controller::apply(const action::TeleportTo& a) {
    auto& ex = m_state.explorer;
    if (a.locationIndex < 0 || a.locationIndex >= static_cast<int>(ex.locations.size())) return;

    const Location& target = ex.locations[static_cast<std::size_t>(a.locationIndex)];
    if (!target.discovered) {
        log(LogLevel::Error, std::format("{} is not discovered yet", target.name));
        return;
    }
    if (a.locationIndex == ex.currentLocation) return;

    ex.currentLocation = a.locationIndex;
    log(LogLevel::Success, std::format("Travelled to {}", target.name));
}

void Controller::apply(const action::RevealAllLocations&) {
    auto& locations = m_state.explorer.locations;
    const auto hidden = std::ranges::count(locations, false, &Location::discovered);
    for (Location& l : locations) l.discovered = true;
    log(LogLevel::Success, std::format("Revealed {} location(s)", hidden));
}

// ------------------------------------------------------------------ Log ----

void Controller::apply(const action::ClearLog&) { m_state.log.clear(); }

void Controller::log(LogLevel level, std::string message) {
    m_state.log.push_back({m_state.clock, level, std::move(message)});
    while (m_state.log.size() > kMaxLogEntries) m_state.log.pop_front();
}

}  // namespace core
