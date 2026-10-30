#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui/Widgets.h"

#include "ui/Theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui::widgets {

namespace {

ImU32 u32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

float scale() { return ImGui::GetIO().FontGlobalScale; }

// Starts a "label | control" row and sets the width for the control that follows.
void beginRow(const char* label) {
    const float x0 = ImGui::GetCursorScreenPos().x;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float labelWidth = std::floor(avail * 0.42f);

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetCursorScreenPos({x0 + labelWidth, ImGui::GetCursorScreenPos().y});
    ImGui::SetNextItemWidth(avail - labelWidth);
}

void hintUnderneath(const char* hint, float indent) {
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y * 0.6f);
    if (indent > 0) ImGui::Indent(indent);
    caption(hint);
    if (indent > 0) ImGui::Unindent(indent);
}

}  // namespace

float animate(ImGuiID id, float target, float speed) {
    float* value = ImGui::GetStateStorage()->GetFloatRef(id, target);
    if (!theme::animationsEnabled()) return *value = target;

    const float k = 1.0f - std::exp(-speed * ImGui::GetIO().DeltaTime);
    *value += (target - *value) * k;
    if (std::abs(target - *value) < 1e-3f) *value = target;
    return *value;
}

// ---------------------------------------------------------- Navigation ----

bool sidebarButton(const char* id, Icon icon, bool selected, const char* tooltip, float sidebarLeftX) {
    const auto& pal = theme::palette();
    const float size = 46.0f * scale();

    ImGui::PushID(id);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##btn", {size, size});
    const bool hovered = ImGui::IsItemHovered();

    const float sel = animate(ImGui::GetID("sel"), selected ? 1.0f : 0.0f);
    const float hov = animate(ImGui::GetID("hov"), hovered ? 1.0f : 0.0f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 max = pos + ImVec2{size, size};
    const float rounding = ImGui::GetStyle().FrameRounding + 4.0f * scale();

    if (hov > 0.01f) dl->AddRectFilled(pos, max, u32(theme::withAlpha(pal.surfaceTop, 0.6f * hov)), rounding);
    if (sel > 0.01f) dl->AddRectFilled(pos, max, u32(theme::withAlpha(pal.accent, 0.18f * sel)), rounding);

    // Active indicator glued to the left edge of the sidebar.
    if (sel > 0.01f) {
        const float h = size * 0.5f * sel;
        const float cy = pos.y + size * 0.5f;
        dl->AddRectFilled({sidebarLeftX, cy - h * 0.5f}, {sidebarLeftX + 3.0f * scale(), cy + h * 0.5f},
                          u32(pal.accent), 2.0f, ImDrawFlags_RoundCornersRight);
    }

    const ImVec4 iconColor = theme::lerp(theme::lerp(pal.textMuted, pal.text, hov), pal.accent, sel);
    drawIcon(dl, icon, pos + ImVec2{size, size} * 0.5f, size * 0.44f, u32(iconColor));

    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return clicked;
}

bool pillTabs(const char* id, std::span<const char* const> labels, int& current) {
    if (labels.empty()) return false;
    const auto& pal = theme::palette();
    const float s = scale();
    const ImVec2 pad{16.0f * s, 7.0f * s};
    const float inset = 4.0f * s;
    const float pillH = ImGui::GetTextLineHeight() + pad.y * 2.0f;

    ImGui::PushID(id);
    current = std::clamp(current, 0, static_cast<int>(labels.size()) - 1);

    // Measure.
    float totalW = inset;
    float targetX = 0, targetW = 0;
    for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
        const float w = ImGui::CalcTextSize(labels[i]).x + pad.x * 2.0f;
        if (i == current) targetX = totalW, targetW = w;
        totalW += w;
    }
    totalW += inset;

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size{totalW, pillH + inset * 2.0f};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, origin + size, u32(pal.surface), size.y * 0.5f);
    dl->AddRect(origin, origin + size, u32(pal.border), size.y * 0.5f);

    // Sliding highlight.
    const float hx = animate(ImGui::GetID("x"), targetX, 18.0f);
    const float hw = animate(ImGui::GetID("w"), targetW, 18.0f);
    const ImVec2 hMin = origin + ImVec2{hx, inset};
    dl->AddRectFilled(hMin, hMin + ImVec2{hw, pillH}, u32(pal.accent), pillH * 0.5f);

    bool changed = false;
    float x = inset;
    for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
        const ImVec2 textSize = ImGui::CalcTextSize(labels[i]);
        const float w = textSize.x + pad.x * 2.0f;
        const ImVec2 pMin = origin + ImVec2{x, inset};

        ImGui::SetCursorScreenPos(pMin);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##pill", {w, pillH}) && current != i) {
            current = i;
            changed = true;
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();

        const ImVec4 col = i == current ? ImVec4{1, 1, 1, 1} : hovered ? pal.text : pal.textMuted;
        dl->AddText(pMin + ImVec2{pad.x, pad.y}, u32(col), labels[i]);
        x += w;
    }

    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(size);
    ImGui::PopID();
    return changed;
}

// --------------------------------------------------------------- Layout ----

Card::Card(const char* title, const char* subtitle) {
    ImGui::BeginChild(title, {0, 0},
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushFont(theme::fonts().strong);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (subtitle) hintUnderneath(subtitle, 0.0f);
    ImGui::Dummy({0, 2.0f * scale()});
}

Card::~Card() {
    ImGui::EndChild();
    ImGui::Dummy({0, 2.0f * scale()});
}

CardGrid::CardGrid(const char* id, float minColumnWidth) {
    const int columns = ImGui::GetContentRegionAvail().x >= minColumnWidth * 2.0f * scale() ? 2 : 1;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {7.0f * scale(), 0.0f});
    m_open = ImGui::BeginTable(id, columns, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadOuterX);
    ImGui::PopStyleVar();
    if (m_open) ImGui::TableNextColumn();
}

CardGrid::~CardGrid() {
    if (m_open) ImGui::EndTable();
}

void CardGrid::next() {
    if (m_open) ImGui::TableNextColumn();
}

// --------------------------------------------------------------- Inputs ----

bool toggle(const char* label, bool* value, const char* hint) {
    const auto& pal = theme::palette();
    const ImGuiStyle& style = ImGui::GetStyle();

    ImGui::PushID(label);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float frameH = ImGui::GetFrameHeight();
    const float lineH = ImGui::GetTextLineHeight();

    float hintH = 0.0f;
    if (hint) {
        ImGui::PushFont(theme::fonts().small);
        hintH = ImGui::GetTextLineHeight() + 2.0f * scale();
        ImGui::PopFont();
    }
    const float rowH = std::max(frameH, lineH + hintH);

    const bool clicked = ImGui::InvisibleButton("##toggle", {width, rowH});
    if (clicked) *value = !*value;
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float textY = pos.y + (rowH - lineH - hintH) * 0.5f;
    dl->AddText({pos.x, textY}, u32(pal.text), label);
    if (hint) {
        ImGui::PushFont(theme::fonts().small);
        dl->AddText({pos.x, textY + lineH + 2.0f * scale()}, u32(pal.textMuted), hint);
        ImGui::PopFont();
    }

    // Switch.
    const float h = frameH * 0.78f;
    const float w = h * 1.8f;
    const ImVec2 sMin{pos.x + width - w - style.FramePadding.x * 0.25f, pos.y + (rowH - h) * 0.5f};
    const float t = animate(ImGui::GetID("knob"), *value ? 1.0f : 0.0f, 16.0f);

    ImVec4 track = theme::lerp(pal.surfaceTop, pal.accent, t);
    if (hovered) track = theme::lerp(track, ImVec4{1, 1, 1, 1}, 0.08f);
    dl->AddRectFilled(sMin, sMin + ImVec2{w, h}, u32(track), h * 0.5f);

    const float r = h * 0.5f - 3.0f * scale();
    const ImVec2 knob{sMin.x + h * 0.5f + (w - h) * t, sMin.y + h * 0.5f};
    dl->AddCircleFilled(knob + ImVec2{0, 1}, r, IM_COL32(0, 0, 0, 60));
    dl->AddCircleFilled(knob, r, IM_COL32(255, 255, 255, 255));

    ImGui::PopID();
    return clicked;
}

bool checkbox(const char* label, bool* value, const char* hint) {
    const auto& pal = theme::palette();
    const bool on = *value;
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, pal.accent);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, pal.accentHover);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, pal.accentActive);
    }
    const bool changed = ImGui::Checkbox(label, value);
    if (on) ImGui::PopStyleColor(3);

    if (hint) hintUnderneath(hint, ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
    return changed;
}

bool sliderFloat(const char* label, float* value, float min, float max, const char* format) {
    ImGui::PushID(label);
    beginRow(label);
    const bool changed = ImGui::SliderFloat("##v", value, min, max, format, ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopID();
    return changed;
}

bool sliderInt(const char* label, int* value, int min, int max, const char* format) {
    ImGui::PushID(label);
    beginRow(label);
    const bool changed = ImGui::SliderInt("##v", value, min, max, format, ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopID();
    return changed;
}

bool combo(const char* label, int* current, std::span<const char* const> items) {
    ImGui::PushID(label);
    beginRow(label);
    const bool changed = ImGui::Combo("##v", current, items.data(), static_cast<int>(items.size()));
    ImGui::PopID();
    return changed;
}

// ------------------------------------------------------ Buttons & display ----

bool button(const char* label, ButtonKind kind, ImVec2 size) {
    const auto& pal = theme::palette();
    int colors = 0;
    switch (kind) {
        case ButtonKind::Primary:
            ImGui::PushStyleColor(ImGuiCol_Button, pal.accent);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, pal.accentHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, pal.accentActive);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1, 1, 1, 1});
            colors = 4;
            break;
        case ButtonKind::Danger:
            ImGui::PushStyleColor(ImGuiCol_Button, theme::withAlpha(pal.danger, 0.14f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::withAlpha(pal.danger, 0.24f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::withAlpha(pal.danger, 0.34f));
            ImGui::PushStyleColor(ImGuiCol_Text, pal.danger);
            colors = 4;
            break;
        case ButtonKind::Secondary: break;
    }
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(colors);
    return pressed;
}

void statBar(const char* label, float value, float max, ImVec4 color) {
    const auto& pal = theme::palette();
    const float s = scale();
    const float width = ImGui::GetContentRegionAvail().x;

    char valueText[32];
    std::snprintf(valueText, sizeof valueText, "%.0f / %.0f", value, max);

    ImGui::PushStyleColor(ImGuiCol_Text, pal.textMuted);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 0);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + width - ImGui::CalcTextSize(label).x - ImGui::CalcTextSize(valueText).x);
    ImGui::TextUnformatted(valueText);

    const float fraction = max > 0 ? std::clamp(value / max, 0.0f, 1.0f) : 0.0f;
    const float shown = animate(ImGui::GetID(label), fraction, 10.0f);

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float h = 8.0f * s;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, pos + ImVec2{width, h}, u32(pal.surfaceHi), h * 0.5f);
    if (shown > 0.0f) dl->AddRectFilled(pos, pos + ImVec2{std::max(h, width * shown), h}, u32(color), h * 0.5f);
    ImGui::Dummy({width, h});
}

void badge(const char* text, ImVec4 color) {
    const float s = scale();
    ImGui::PushFont(theme::fonts().small);
    const ImVec2 pad{7.0f * s, 2.0f * s};
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const ImVec2 size = textSize + pad * 2.0f;

    // Vertically centre on a frame-height line (tables, rows with buttons).
    const float offsetY = std::max(0.0f, (ImGui::GetFrameHeight() - size.y) * 0.5f);
    const ImVec2 pos = ImGui::GetCursorScreenPos() + ImVec2{0, offsetY};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, pos + size, u32(theme::withAlpha(color, 0.15f)), size.y * 0.5f);
    dl->AddText(pos + pad, u32(color), text);
    ImGui::Dummy({size.x, size.y + offsetY});
    ImGui::PopFont();
}

void caption(const char* text) {
    ImGui::PushFont(theme::fonts().small);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::palette().textMuted);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void helpMarker(const char* text) {
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

bool colorSwatches(const char* id, std::span<const ImVec4> colors, int& current) {
    const float s = scale();
    const float r = 11.0f * s;
    bool changed = false;

    ImGui::PushID(id);
    for (int i = 0; i < static_cast<int>(colors.size()); ++i) {
        if (i > 0) ImGui::SameLine(0, 10.0f * s);
        ImGui::PushID(i);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##swatch", {r * 2 + 6 * s, r * 2 + 6 * s}) && current != i) {
            current = i;
            changed = true;
        }
        const bool hovered = ImGui::IsItemHovered();
        const float sel = animate(ImGui::GetID("sel"), current == i ? 1.0f : 0.0f);
        ImGui::PopID();

        const ImVec2 c = pos + ImVec2{r + 3 * s, r + 3 * s};
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddCircleFilled(c, r * (hovered ? 1.0f : 0.9f), u32(colors[static_cast<std::size_t>(i)]), 32);
        if (sel > 0.01f) {
            dl->AddCircle(c, r + 2.0f * s, IM_COL32(255, 255, 255, static_cast<int>(220 * sel)), 32, 2.0f * s);
            dl->AddCircleFilled(c, r * 0.32f * sel, IM_COL32(255, 255, 255, 255), 16);
        }
    }
    ImGui::PopID();
    return changed;
}

}  // namespace ui::widgets
