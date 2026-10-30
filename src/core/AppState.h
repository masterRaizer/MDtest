#pragma once

// Domain model. Plain data only: no ImGui, no rendering, no behaviour.
// The UI reads it through a const reference; only core::Controller writes it.

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace core {

// ---------------------------------------------------------------- Combat ----

inline constexpr std::array kDifficulties{"Story", "Normal", "Hard", "Nightmare"};
inline constexpr std::array kTargetPriorities{"Nearest", "Lowest health", "Strongest", "Last attacker"};
inline constexpr std::array kDamageNumberStyles{"Classic", "Floating", "Stacked", "Minimal"};

struct CombatSettings {
    // General
    bool autoAttack = false;
    bool autoBlock = true;
    int difficulty = 1;
    float attackSpeed = 1.0f;  // x multiplier
    int comboLength = 3;

    // Targeting
    int targetPriority = 0;
    bool targetLock = true;
    bool ignoreNeutral = true;
    float lockRange = 25.0f;  // metres

    // Feedback
    bool damageNumbers = true;
    bool hitMarkers = true;
    int damageNumberStyle = 1;
    float screenShake = 0.5f;  // 0..1
    float hitStopMs = 60.0f;
};

// ---------------------------------------------------------------- Player ----

inline constexpr std::array kPlayerClasses{"Warrior", "Ranger", "Mage", "Rogue"};
inline constexpr std::array kCameraModes{"Third person", "First person", "Top-down"};

struct PlayerSettings {
    // Stats
    int playerClass = 0;
    int level = 10;  // maxHealth = 80 + level * 2
    float health = 74.0f;
    float maxHealth = 100.0f;
    float stamina = 100.0f;
    float maxStamina = 100.0f;
    bool godMode = false;
    bool infiniteStamina = false;
    bool healthRegen = true;

    // Movement
    float moveSpeed = 1.0f;
    float jumpHeight = 1.0f;
    float sprintMultiplier = 1.6f;
    bool autoSprint = false;
    bool flyMode = false;

    // Camera
    int cameraMode = 0;
    float fov = 90.0f;
    float sensitivity = 1.0f;
    bool invertY = false;
    bool headBob = true;
};

// ------------------------------------------------------------- Inventory ----

enum class ItemCategory : std::uint8_t { Weapon, Armor, Consumable, Material, Quest, Count };
enum class Rarity : std::uint8_t { Common, Uncommon, Rare, Epic, Legendary, Count };

inline constexpr std::array kItemCategoryNames{"Weapon", "Armor", "Consumable", "Material", "Quest"};
inline constexpr std::array kRarityNames{"Common", "Uncommon", "Rare", "Epic", "Legendary"};
inline constexpr std::array kSortModes{"Name", "Rarity", "Category", "Weight"};
inline constexpr std::array kLootFilters{"Everything", "Uncommon+", "Rare+", "Epic+"};

struct Item {
    int id = 0;
    std::string name;
    ItemCategory category = ItemCategory::Material;
    Rarity rarity = Rarity::Common;
    int quantity = 1;
    float weight = 0.0f;  // per unit, kg
    bool equipped = false;
};

struct InventoryPrefs {
    int sortMode = 1;
    bool sortDescending = true;
    bool autoLoot = true;
    bool autoSortOnPickup = true;
    int lootFilter = 0;
    bool confirmDrop = true;
};

struct InventoryState {
    std::vector<Item> items;
    float maxWeight = 60.0f;
    InventoryPrefs prefs;
    int nextItemId = 1;
};

// -------------------------------------------------------------- Explorer ----

inline constexpr std::array kWeatherTypes{"Clear", "Cloudy", "Rain", "Storm", "Snow", "Fog"};

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

struct Location {
    std::string name;
    std::string region;
    Vec3 position;
    bool discovered = false;
};

struct WorldSettings {
    float timeOfDay = 14.5f;  // hours, [0, 24)
    float timeScale = 60.0f;  // in-game minutes per real second
    bool freezeTime = false;
    int weather = 0;
    bool fogOfWar = true;
    bool showMinimap = true;
    float minimapZoom = 1.0f;
    float renderDistance = 800.0f;  // metres
};

struct ExplorerState {
    std::vector<Location> locations;
    int currentLocation = 0;
    WorldSettings world;
};

// -------------------------------------------------------------- Settings ----

inline constexpr std::array kAccentNames{"Violet", "Azure", "Emerald", "Amber", "Rose"};
inline constexpr std::array kLanguages{"English", "Russian", "Deutsch", "Español"};
inline constexpr std::array kFpsLimits{"30", "60", "120", "144", "Unlimited"};
inline constexpr std::array kFpsLimitValues{30, 60, 120, 144, 0};

struct AppearanceSettings {
    int accent = 0;
    float uiScale = 1.0f;
    float rounding = 8.0f;
    bool animations = true;

    bool operator==(const AppearanceSettings&) const = default;
};

struct GeneralSettings {
    int language = 0;
    bool vsync = true;
    int fpsLimit = 1;
    bool showFpsOverlay = false;
    bool confirmDestructive = true;
};

struct AppSettings {
    AppearanceSettings appearance;
    GeneralSettings general;
};

// ------------------------------------------------------------------- Log ----

enum class LogLevel : std::uint8_t { Info, Success, Warning, Error };

struct LogEntry {
    double timestamp = 0.0;  // seconds since start
    LogLevel level = LogLevel::Info;
    std::string message;
};

// ------------------------------------------------------------------ Root ----

struct AppState {
    CombatSettings combat;
    PlayerSettings player;
    InventoryState inventory;
    ExplorerState explorer;
    AppSettings settings;

    std::deque<LogEntry> log;  // newest at the back
    double clock = 0.0;        // seconds since start
};

// Initial state populated with demo content.
[[nodiscard]] AppState makeDemoState();

// Derived values. Pure functions of the model, usable from UI and logic alike.
[[nodiscard]] float totalWeight(const InventoryState& inventory);
[[nodiscard]] float distance(const Vec3& a, const Vec3& b);

}  // namespace core
