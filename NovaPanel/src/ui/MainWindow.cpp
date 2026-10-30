#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui/MainWindow.h"

#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <algorithm>
#include <cstdio>

namespace ui {

namespace {

float scale() { return ImGui::GetIO().FontGlobalScale; }

ImU32 u32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

}  // namespace

MainWindow::MainWindow()
    : m_pages{
          std::make_unique<CombatPage>(),
          std::make_unique<PlayerPage>(),
          std::make_unique<InventoryPage>(),
          std::make_unique<ExplorerPage>(),
          std::make_unique<SettingsPage>(),
      } {}

void MainWindow::setActivePage(PageId page) {
    if (page == m_active || page == PageId::Count) return;
    m_active = page;
    m_fade = 0.0f;
}

void MainWindow::setActiveSection(int section) {
    activeSection() = std::clamp(section, 0, static_cast<int>(activePageRef().sections().size()) - 1);
}

void MainWindow::draw(const core::AppState& state, core::ActionSink& actions) {
    const auto& pal = theme::palette();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    constexpr ImGuiWindowFlags kRootFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##root", nullptr, kRootFlags);
    ImGui::PopStyleVar(2);

    drawSidebar();
    ImGui::SameLine(0, 0);

    // ---- Main column ----
    ImGui::PushStyleColor(ImGuiCol_ChildBg, pal.background);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{28.0f, 22.0f} * scale());
    ImGui::BeginChild("##main", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    drawHeader(state);

    const float statusH = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
    m_fade = theme::animationsEnabled() ? std::min(1.0f, m_fade + ImGui::GetIO().DeltaTime * 5.0f) : 1.0f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0, 0, 0, 0});
    ImGui::BeginChild("##content", {0, -statusH}, ImGuiChildFlags_None);
    ImGui::PopStyleColor();
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, m_fade);
    ImGui::PushID(static_cast<int>(m_active));
    activePageRef().draw(activeSection(), state, actions);
    ImGui::PopID();
    ImGui::PopStyleVar();
    ImGui::EndChild();

    drawStatusBar(state);
    ImGui::EndChild();
    ImGui::End();

    if (state.settings.general.showFpsOverlay) drawFpsOverlay();
}

void MainWindow::drawSidebar() {
    const auto& pal = theme::palette();
    const float s = scale();
    const float width = 78.0f * s;
    const float buttonSize = 46.0f * s;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, pal.sidebar);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0, 18.0f * s});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{0, 8.0f * s});
    ImGui::BeginChild("##sidebar", {width, 0}, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 winPos = ImGui::GetWindowPos();
    const ImVec2 winSize = ImGui::GetWindowSize();
    dl->AddLine({winPos.x + winSize.x - 1, winPos.y}, {winPos.x + winSize.x - 1, winPos.y + winSize.y}, u32(pal.border));

    // Logo.
    const float logoSize = 30.0f * s;
    const ImVec2 logoPos = ImGui::GetCursorScreenPos() + ImVec2{(width - logoSize) * 0.5f, 0};
    drawIcon(dl, Icon::Logo, logoPos + ImVec2{logoSize, logoSize} * 0.5f, logoSize, u32(pal.accent));
    ImGui::Dummy({width, logoSize + 18.0f * s});

    auto navButton = [&](PageId id) {
        Page& page = *m_pages[static_cast<std::size_t>(id)];
        ImGui::SetCursorPosX((width - buttonSize) * 0.5f);
        if (widgets::sidebarButton(page.title(), page.icon(), m_active == id, page.title(), winPos.x))
            setActivePage(id);
    };

    for (PageId id : {PageId::Combat, PageId::Player, PageId::Inventory, PageId::Explorer}) navButton(id);

    // Settings pinned to the bottom.
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), winSize.y - buttonSize - 18.0f * s));
    navButton(PageId::Settings);

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void MainWindow::drawHeader(const core::AppState& state) {
    const auto& pal = theme::palette();
    Page& page = activePageRef();

    const float top = ImGui::GetCursorPosY();
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::PushFont(theme::fonts().heading);
    ImGui::TextUnformatted(page.title());
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, pal.textMuted);
    ImGui::TextUnformatted(page.subtitle());
    ImGui::PopStyleColor();

    // Player summary in the top-right corner.
    char summary[64];
    std::snprintf(summary, sizeof summary, "%s  |  Lv %d",
                  core::kPlayerClasses[static_cast<std::size_t>(state.player.playerClass)], state.player.level);
    ImGui::PushFont(theme::fonts().small);  // badge font
    const float summaryW = ImGui::CalcTextSize(summary).x + 14.0f * scale();
    ImGui::PopFont();
    const ImVec2 restore = ImGui::GetCursorPos();
    ImGui::SetCursorPos({right - summaryW, top + 6.0f * scale()});
    widgets::badge(summary, pal.accent);
    ImGui::SetCursorPos(restore);

    ImGui::Dummy({0, 6.0f * scale()});
    widgets::pillTabs("##sections", page.sections(), activeSection());
    ImGui::Dummy({0, 6.0f * scale()});
}

void MainWindow::drawStatusBar(const core::AppState& state) {
    const auto& pal = theme::palette();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float right = ImGui::GetCursorPosX() + width;
    ImGui::GetWindowDrawList()->AddLine({p.x, p.y}, {p.x + width, p.y}, u32(pal.border));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetStyle().ItemSpacing.y);

    ImGui::AlignTextToFramePadding();
    if (!state.log.empty()) {
        const core::LogEntry& last = state.log.back();
        const float age = static_cast<float>(state.clock - last.timestamp);
        const float alpha = std::clamp(1.0f - (age - 4.0f) / 2.0f, 0.45f, 1.0f);  // dims after ~5 s
        ImVec4 dot = pal.textMuted;
        switch (last.level) {
            case core::LogLevel::Success: dot = pal.success; break;
            case core::LogLevel::Warning: dot = pal.warning; break;
            case core::LogLevel::Error: dot = pal.danger; break;
            default: break;
        }
        const float r = 3.5f * scale();
        const ImVec2 c = ImGui::GetCursorScreenPos() + ImVec2{r, ImGui::GetFrameHeight() * 0.5f};
        ImGui::GetWindowDrawList()->AddCircleFilled(c, r, u32(theme::withAlpha(dot, alpha)));
        ImGui::Dummy({r * 2.0f, 0});
        ImGui::SameLine();
        ImGui::TextColored(theme::withAlpha(pal.text, alpha * 0.85f), "%s", last.message.c_str());
    }

    const auto& ex = state.explorer;
    char info[96];
    std::snprintf(info, sizeof info, "%s   %.0f FPS",
                  ex.locations.empty() ? "-" : ex.locations[static_cast<std::size_t>(ex.currentLocation)].name.c_str(),
                  ImGui::GetIO().Framerate);
    ImGui::SameLine(0, 0);
    ImGui::SetCursorPosX(right - ImGui::CalcTextSize(info).x);
    ImGui::TextColored(pal.textMuted, "%s", info);
}

void MainWindow::drawFpsOverlay() {
    const auto& pal = theme::palette();
    const float s = scale();
    char text[32];
    std::snprintf(text, sizeof text, "%.0f FPS  %.2f ms", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 pad{10.0f * s, 6.0f * s};
    const ImVec2 size = ImGui::CalcTextSize(text) + pad * 2.0f;
    const ImVec2 pos{vp->WorkPos.x + vp->WorkSize.x - size.x - 12.0f * s, vp->WorkPos.y + 12.0f * s};

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(pos, pos + size, u32(theme::withAlpha(pal.sidebar, 0.92f)), size.y * 0.5f);
    dl->AddRect(pos, pos + size, u32(pal.border), size.y * 0.5f);
    dl->AddText(pos + pad, u32(pal.success), text);
}

}  // namespace ui
