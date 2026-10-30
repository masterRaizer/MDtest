#pragma once

// Visual identity: colour palette, ImGuiStyle parameters and fonts.

#include "core/AppState.h"

#include <imgui.h>

namespace ui::theme {

struct Palette {
    // Neutrals (dark theme)
    ImVec4 background{0.051f, 0.055f, 0.071f, 1.00f};  // #0D0E12  app background
    ImVec4 sidebar{0.067f, 0.071f, 0.090f, 1.00f};     // #111217  sidebar
    ImVec4 surface{0.090f, 0.094f, 0.118f, 1.00f};     // #17181E  cards
    ImVec4 surfaceHi{0.129f, 0.133f, 0.165f, 1.00f};   // #21222A  inputs / frames
    ImVec4 surfaceTop{0.169f, 0.176f, 0.216f, 1.00f};  // #2B2D37  hovered frames
    ImVec4 border{1.00f, 1.00f, 1.00f, 0.06f};
    ImVec4 text{0.925f, 0.933f, 0.953f, 1.00f};       // #ECEEF3
    ImVec4 textMuted{0.541f, 0.561f, 0.627f, 1.00f};  // #8A8FA0
    ImVec4 textDisabled{0.353f, 0.369f, 0.420f, 1.00f};

    // Accent (driven by AppearanceSettings::accent)
    ImVec4 accent{0.486f, 0.361f, 1.000f, 1.00f};  // #7C5CFF
    ImVec4 accentHover;
    ImVec4 accentActive;
    ImVec4 accentSoft;  // translucent accent for selected backgrounds

    // Semantic
    ImVec4 success{0.239f, 0.839f, 0.549f, 1.00f};  // #3DD68C
    ImVec4 warning{0.961f, 0.710f, 0.267f, 1.00f};  // #F5B544
    ImVec4 danger{0.949f, 0.333f, 0.353f, 1.00f};   // #F2555A
};

struct Fonts {
    ImFont* body = nullptr;
    ImFont* heading = nullptr;  // page titles
    ImFont* strong = nullptr;   // card titles
    ImFont* small = nullptr;    // captions, badges
};

// Accent colour presets, indexed by AppearanceSettings::accent (see core::kAccentNames).
[[nodiscard]] ImVec4 accentPreset(int index);

[[nodiscard]] const Palette& palette();
[[nodiscard]] const Fonts& fonts();
[[nodiscard]] bool animationsEnabled();

// Loads a system UI font (falls back to the ImGui default font). Call once before the first frame.
void loadFonts(ImGuiIO& io);

// Rebuilds the whole ImGuiStyle from scratch. Safe to call at any time.
void apply(const core::AppearanceSettings& appearance);

// Colour of a rarity tier (Common..Legendary).
[[nodiscard]] ImVec4 rarityColor(core::Rarity rarity);

[[nodiscard]] ImVec4 withAlpha(ImVec4 c, float alpha);
[[nodiscard]] ImVec4 lerp(const ImVec4& a, const ImVec4& b, float t);

}  // namespace ui::theme
