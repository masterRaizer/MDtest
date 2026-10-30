#pragma once

// Top-level layout: sidebar navigation, page header with pill tabs, scrollable
// content area and status bar. Owns the pages and the navigation state.

#include "core/Actions.h"
#include "core/AppState.h"
#include "ui/pages/Page.h"

#include <array>
#include <memory>

namespace ui {

enum class PageId : int { Combat, Player, Inventory, Explorer, Settings, Count };

class MainWindow {
public:
    MainWindow();

    void draw(const core::AppState& state, core::ActionSink& actions);

    void setActivePage(PageId page);
    void setActiveSection(int section);
    [[nodiscard]] PageId activePage() const { return m_active; }

private:
    static constexpr std::size_t kPageCount = static_cast<std::size_t>(PageId::Count);

    void drawSidebar();
    void drawHeader(const core::AppState& state);
    void drawStatusBar(const core::AppState& state);
    void drawFpsOverlay();

    [[nodiscard]] Page& activePageRef() { return *m_pages[static_cast<std::size_t>(m_active)]; }
    [[nodiscard]] int& activeSection() { return m_sections[static_cast<std::size_t>(m_active)]; }

    std::array<std::unique_ptr<Page>, kPageCount> m_pages;
    std::array<int, kPageCount> m_sections{};  // selected pill tab per page
    PageId m_active = PageId::Combat;
    float m_fade = 1.0f;  // content fade-in after navigation
};

}  // namespace ui
