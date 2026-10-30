#pragma once

// Everything the UI can ask the application to do.
// Views never mutate core::AppState directly: they emit one of these actions
// and core::Controller decides how (and whether) to apply it.

#include "core/AppState.h"

#include <utility>
#include <variant>
#include <vector>

namespace core {

namespace action {

// "Draft" updates: the view edits a copy of a settings block and hands it back.
// The controller validates (clamps) it before committing.
struct UpdateCombat { CombatSettings value; };
struct UpdatePlayer { PlayerSettings value; };
struct UpdateWorld { WorldSettings value; };
struct UpdateInventoryPrefs { InventoryPrefs value; };
struct UpdateAppearance { AppearanceSettings value; };
struct UpdateGeneral { GeneralSettings value; };

// Commands.
struct HealPlayer {};
struct ResetPlayer {};
struct UseItem { int itemId; };
struct DropItem { int itemId; };
struct SpawnRandomItem {};
struct SortInventory {};
struct TeleportTo { int locationIndex; };
struct RevealAllLocations {};
struct ResetAllSettings {};
struct ClearLog {};

}  // namespace action

using Action = std::variant<
    action::UpdateCombat,
    action::UpdatePlayer,
    action::UpdateWorld,
    action::UpdateInventoryPrefs,
    action::UpdateAppearance,
    action::UpdateGeneral,
    action::HealPlayer,
    action::ResetPlayer,
    action::UseItem,
    action::DropItem,
    action::SpawnRandomItem,
    action::SortInventory,
    action::TeleportTo,
    action::RevealAllLocations,
    action::ResetAllSettings,
    action::ClearLog>;

// Write-only interface given to views. Keeps the UI unaware of who handles actions.
class ActionSink {
public:
    virtual ~ActionSink() = default;
    virtual void dispatch(Action action) = 0;
};

// Actions are queued during the frame and applied after the UI has been drawn,
// so the state stays consistent for the whole frame.
class ActionQueue final : public ActionSink {
public:
    void dispatch(Action action) override { m_pending.push_back(std::move(action)); }

    [[nodiscard]] std::vector<Action> drain() { return std::exchange(m_pending, {}); }
    [[nodiscard]] bool empty() const { return m_pending.empty(); }

private:
    std::vector<Action> m_pending;
};

}  // namespace core
