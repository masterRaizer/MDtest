#pragma once

// Reusable, stateless-from-the-caller's-view widgets built on top of ImGui.
// They only render and report interaction; they know nothing about the domain.

#include "ui/Icons.h"

#include <imgui.h>

#include <span>

namespace ui::widgets {

// Smoothly moves a per-widget value towards `target` (frame-rate independent).
float animate(ImGuiID id, float target, float speed = 14.0f);

// ---- Navigation -------------------------------------------------------------

// Square icon button for the sidebar with an animated selection indicator.
bool sidebarButton(const char* id, Icon icon, bool selected, const char* tooltip, float sidebarLeftX);

// Horizontal "pill" tab bar. Returns true when the selection changed.
bool pillTabs(const char* id, std::span<const char* const> labels, int& current);

// ---- Layout -----------------------------------------------------------------

// Rounded panel with a title. RAII: the panel is closed when the object goes out of scope.
class Card {
public:
    explicit Card(const char* title, const char* subtitle = nullptr);
    ~Card();
    Card(const Card&) = delete;
    Card& operator=(const Card&) = delete;
};

// Responsive grid of cards: two columns on wide windows, one on narrow ones.
class CardGrid {
public:
    explicit CardGrid(const char* id, float minColumnWidth = 340.0f);
    ~CardGrid();
    CardGrid(const CardGrid&) = delete;
    CardGrid& operator=(const CardGrid&) = delete;

    void next();  // move to the next cell

private:
    bool m_open = false;
};

// ---- Inputs (label on the left, control on the right) ------------------------

bool toggle(const char* label, bool* value, const char* hint = nullptr);
bool checkbox(const char* label, bool* value, const char* hint = nullptr);
bool sliderFloat(const char* label, float* value, float min, float max, const char* format = "%.2f");
bool sliderInt(const char* label, int* value, int min, int max, const char* format = "%d");
bool combo(const char* label, int* current, std::span<const char* const> items);

// ---- Buttons & display --------------------------------------------------------

enum class ButtonKind { Primary, Secondary, Danger };
bool button(const char* label, ButtonKind kind = ButtonKind::Secondary, ImVec2 size = {0, 0});

// Labelled horizontal progress bar ("Health  74 / 100").
void statBar(const char* label, float value, float max, ImVec4 color);

// Small rounded badge with coloured text.
void badge(const char* text, ImVec4 color);

// Muted caption text.
void caption(const char* text);

// "(?)" marker with a tooltip.
void helpMarker(const char* text);

// Row of clickable colour dots. Returns true when the selection changed.
bool colorSwatches(const char* id, std::span<const ImVec4> colors, int& current);

}  // namespace ui::widgets
