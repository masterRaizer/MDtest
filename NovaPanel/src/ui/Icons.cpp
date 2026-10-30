#include "ui/Icons.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace ui {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

ImVec2 operator+(ImVec2 a, ImVec2 b) { return {a.x + b.x, a.y + b.y}; }

ImVec2 polar(ImVec2 c, float radius, float angle) {
    return {c.x + std::cos(angle) * radius, c.y + std::sin(angle) * radius};
}

void drawLogo(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    // Faceted diamond.
    const float r = s * 0.5f;
    const ImVec2 top = c + ImVec2{0, -r}, right = c + ImVec2{r * 0.8f, 0};
    const ImVec2 bottom = c + ImVec2{0, r}, left = c + ImVec2{-r * 0.8f, 0};
    dl->AddQuadFilled(top, right, bottom, left, col);
    const ImU32 facet = IM_COL32(255, 255, 255, 70);
    dl->AddTriangleFilled(top, right, c, facet);
    dl->AddTriangleFilled(left, c, bottom, IM_COL32(0, 0, 0, 50));
}

void drawCrosshair(ImDrawList* dl, ImVec2 c, float s, ImU32 col, float t) {
    const float r = s * 0.36f;
    dl->AddCircle(c, r, col, 32, t);
    const float in = r * 0.45f, out = s * 0.5f;
    for (int i = 0; i < 4; ++i) {
        const float a = static_cast<float>(i) * kPi * 0.5f;
        dl->AddLine(polar(c, in, a), polar(c, out, a), col, t);
    }
    dl->AddCircleFilled(c, t * 1.1f, col);
}

void drawPlayer(ImDrawList* dl, ImVec2 c, float s, ImU32 col, float t) {
    dl->AddCircle(c + ImVec2{0, -s * 0.18f}, s * 0.2f, col, 24, t);
    // Shoulders: upper half of an ellipse-ish arc.
    dl->PathArcTo(c + ImVec2{0, s * 0.48f}, s * 0.36f, kPi * 1.08f, kPi * 1.92f, 24);
    dl->PathStroke(col, ImDrawFlags_None, t);
}

void drawInventory(ImDrawList* dl, ImVec2 c, float s, ImU32 col, float t) {
    // Backpack: body, flap and handle.
    const ImVec2 min = c + ImVec2{-s * 0.34f, -s * 0.22f};
    const ImVec2 max = c + ImVec2{s * 0.34f, s * 0.42f};
    dl->AddRect(min, max, col, s * 0.12f, ImDrawFlags_None, t);
    dl->AddLine({min.x, c.y + s * 0.06f}, {max.x, c.y + s * 0.06f}, col, t);
    dl->AddRectFilled(c + ImVec2{-s * 0.07f, s * 0.02f}, c + ImVec2{s * 0.07f, s * 0.14f}, col, 1.0f);
    dl->PathArcTo(c + ImVec2{0, -s * 0.22f}, s * 0.15f, kPi, kPi * 2.0f, 12);
    dl->PathStroke(col, ImDrawFlags_None, t);
}

void drawCompass(ImDrawList* dl, ImVec2 c, float s, ImU32 col, float t) {
    const float r = s * 0.44f;
    dl->AddCircle(c, r, col, 32, t);
    const float a = -kPi * 0.25f;  // needle points north-east
    const ImVec2 tip = polar(c, r * 0.7f, a), tail = polar(c, r * 0.7f, a + kPi);
    const ImVec2 l = polar(c, r * 0.22f, a - kPi * 0.5f), rr = polar(c, r * 0.22f, a + kPi * 0.5f);
    dl->AddTriangleFilled(tip, rr, l, col);
    dl->AddTriangle(tail, l, rr, col, t * 0.8f);
}

void drawGear(ImDrawList* dl, ImVec2 c, float s, ImU32 col, float t) {
    constexpr int kTeeth = 8;
    const float outer = s * 0.46f, inner = s * 0.34f;
    const float step = 2.0f * kPi / kTeeth;

    std::array<ImVec2, kTeeth * 4> pts{};
    for (int i = 0; i < kTeeth; ++i) {
        const float a = static_cast<float>(i) * step;
        pts[i * 4 + 0] = polar(c, inner, a - step * 0.30f);
        pts[i * 4 + 1] = polar(c, outer, a - step * 0.18f);
        pts[i * 4 + 2] = polar(c, outer, a + step * 0.18f);
        pts[i * 4 + 3] = polar(c, inner, a + step * 0.30f);
    }
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col, ImDrawFlags_Closed, t);
    dl->AddCircle(c, s * 0.14f, col, 20, t);
}

}  // namespace

void drawIcon(ImDrawList* dl, Icon icon, ImVec2 center, float size, ImU32 color) {
    const float thickness = std::max(1.5f, size * 0.085f);
    switch (icon) {
        case Icon::Logo: drawLogo(dl, center, size, color); break;
        case Icon::Combat: drawCrosshair(dl, center, size, color, thickness); break;
        case Icon::Player: drawPlayer(dl, center, size, color, thickness); break;
        case Icon::Inventory: drawInventory(dl, center, size, color, thickness); break;
        case Icon::Explorer: drawCompass(dl, center, size, color, thickness); break;
        case Icon::Settings: drawGear(dl, center, size, color, thickness); break;
    }
}

}  // namespace ui
