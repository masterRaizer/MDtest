#pragma once

// Application logic. The only place where core::AppState is modified.
// Has no dependency on ImGui, so it can be unit-tested headlessly.

#include "core/Actions.h"
#include "core/AppState.h"

#include <cstddef>
#include <functional>
#include <random>
#include <string>

namespace core {

class Controller {
public:
    explicit Controller(AppState& state);

    // Called whenever the appearance settings change (the platform layer re-applies the theme).
    std::function<void(const AppearanceSettings&)> onAppearanceChanged;

    void handle(const Action& action);
    void process(ActionQueue& queue);

    // Per-frame simulation (time of day, regeneration, ...).
    void tick(float dt);

private:
    void apply(const action::UpdateCombat& a);
    void apply(const action::UpdatePlayer& a);
    void apply(const action::UpdateWorld& a);
    void apply(const action::UpdateInventoryPrefs& a);
    void apply(const action::UpdateAppearance& a);
    void apply(const action::UpdateGeneral& a);
    void apply(const action::HealPlayer& a);
    void apply(const action::ResetPlayer& a);
    void apply(const action::UseItem& a);
    void apply(const action::DropItem& a);
    void apply(const action::SpawnRandomItem& a);
    void apply(const action::SortInventory& a);
    void apply(const action::TeleportTo& a);
    void apply(const action::RevealAllLocations& a);
    void apply(const action::ResetAllSettings& a);
    void apply(const action::ClearLog& a);

    void sortInventory();
    void log(LogLevel level, std::string message);

    static constexpr std::size_t kMaxLogEntries = 200;

    AppState& m_state;
    std::mt19937 m_rng{42};
};

}  // namespace core
