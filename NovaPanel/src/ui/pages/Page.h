#pragma once

// A page is a pure view: it reads the state and reports user intent as actions.
// It may keep purely visual state (search text, selection), but never domain data.

#include "core/Actions.h"
#include "core/AppState.h"
#include "ui/Icons.h"

#include <memory>
#include <span>

namespace ui {

class Page {
public:
    virtual ~Page() = default;

    [[nodiscard]] virtual const char* title() const = 0;
    [[nodiscard]] virtual const char* subtitle() const = 0;
    [[nodiscard]] virtual Icon icon() const = 0;
    [[nodiscard]] virtual std::span<const char* const> sections() const = 0;  // pill tabs

    virtual void draw(int section, const core::AppState& state, core::ActionSink& actions) = 0;
};

class CombatPage final : public Page {
public:
    const char* title() const override { return "Combat"; }
    const char* subtitle() const override { return "Attack behaviour, targeting and hit feedback"; }
    Icon icon() const override { return Icon::Combat; }
    std::span<const char* const> sections() const override;
    void draw(int section, const core::AppState& state, core::ActionSink& actions) override;
};

class PlayerPage final : public Page {
public:
    const char* title() const override { return "Player"; }
    const char* subtitle() const override { return "Character stats, movement and camera"; }
    Icon icon() const override { return Icon::Player; }
    std::span<const char* const> sections() const override;
    void draw(int section, const core::AppState& state, core::ActionSink& actions) override;
};

class InventoryPage final : public Page {
public:
    const char* title() const override { return "Inventory"; }
    const char* subtitle() const override { return "Items, equipment and loot rules"; }
    Icon icon() const override { return Icon::Inventory; }
    std::span<const char* const> sections() const override;
    void draw(int section, const core::AppState& state, core::ActionSink& actions) override;

private:
    void drawItems(const core::AppState& state, core::ActionSink& actions);
    void drawLootRules(const core::AppState& state, core::ActionSink& actions);

    // View-only state.
    char m_search[64] = {};
    int m_categoryFilter = 0;  // 0 = all, otherwise ItemCategory + 1
    int m_pendingDropId = -1;
};

class ExplorerPage final : public Page {
public:
    const char* title() const override { return "Explorer"; }
    const char* subtitle() const override { return "Locations, fast travel and world state"; }
    Icon icon() const override { return Icon::Explorer; }
    std::span<const char* const> sections() const override;
    void draw(int section, const core::AppState& state, core::ActionSink& actions) override;

private:
    void drawLocations(const core::AppState& state, core::ActionSink& actions);
    void drawWorld(const core::AppState& state, core::ActionSink& actions);

    int m_selected = 0;
    bool m_onlyDiscovered = false;
};

class SettingsPage final : public Page {
public:
    const char* title() const override { return "Settings"; }
    const char* subtitle() const override { return "Appearance, application and activity log"; }
    Icon icon() const override { return Icon::Settings; }
    std::span<const char* const> sections() const override;
    void draw(int section, const core::AppState& state, core::ActionSink& actions) override;

private:
    void drawAppearance(const core::AppState& state, core::ActionSink& actions);
    void drawGeneral(const core::AppState& state, core::ActionSink& actions);
    void drawAbout(const core::AppState& state, core::ActionSink& actions);

    float m_scaleDraft = -1.0f;  // UI scale is applied on release, not while dragging
    bool m_confirmReset = false;
    bool m_previewCheck = true;
    float m_previewSlider = 0.6f;
};

}  // namespace ui
