#include "ui/Theme.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <string_view>

namespace ui::theme {

namespace {

Palette g_palette;
Fonts g_fonts;
bool g_animations = true;

struct FontPair {
    std::string_view regular;
    std::string_view bold;
};

// First existing pair wins. Drop Inter into assets/fonts/ for the intended look.
constexpr std::array kFontCandidates{
    FontPair{"assets/fonts/Inter-Regular.ttf", "assets/fonts/Inter-SemiBold.ttf"},
    FontPair{"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/seguisb.ttf"},
    FontPair{"/System/Library/Fonts/Supplemental/Arial.ttf", "/System/Library/Fonts/Supplemental/Arial Bold.ttf"},
    FontPair{"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/noto/NotoSans-SemiBold.ttf"},
    FontPair{"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"},
};

constexpr std::array kAccents{
    ImVec4{0.486f, 0.361f, 1.000f, 1.0f},  // Violet  #7C5CFF
    ImVec4{0.231f, 0.510f, 0.965f, 1.0f},  // Azure   #3B82F6
    ImVec4{0.063f, 0.725f, 0.506f, 1.0f},  // Emerald #10B981
    ImVec4{0.961f, 0.620f, 0.043f, 1.0f},  // Amber   #F59E0B
    ImVec4{0.957f, 0.247f, 0.369f, 1.0f},  // Rose    #F43F5E
};
static_assert(kAccents.size() == core::kAccentNames.size());

bool exists(std::string_view path) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(path), ec);
}

}  // namespace

ImVec4 accentPreset(int index) {
    return kAccents[static_cast<std::size_t>(std::clamp(index, 0, static_cast<int>(kAccents.size()) - 1))];
}

const Palette& palette() { return g_palette; }
const Fonts& fonts() { return g_fonts; }
bool animationsEnabled() { return g_animations; }

ImVec4 withAlpha(ImVec4 c, float alpha) {
    c.w = alpha;
    return c;
}

ImVec4 lerp(const ImVec4& a, const ImVec4& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
}

ImVec4 rarityColor(core::Rarity rarity) {
    switch (rarity) {
        case core::Rarity::Common: return {0.70f, 0.72f, 0.78f, 1.0f};
        case core::Rarity::Uncommon: return {0.30f, 0.82f, 0.45f, 1.0f};
        case core::Rarity::Rare: return {0.29f, 0.60f, 1.00f, 1.0f};
        case core::Rarity::Epic: return {0.70f, 0.42f, 1.00f, 1.0f};
        case core::Rarity::Legendary: return {1.00f, 0.66f, 0.18f, 1.0f};
        default: return g_palette.text;
    }
}

void loadFonts(ImGuiIO& io) {
    const ImWchar* ranges = io.Fonts->GetGlyphRangesCyrillic();  // Latin + Cyrillic

    for (const FontPair& pair : kFontCandidates) {
        if (!exists(pair.regular)) continue;
        const char* bold = exists(pair.bold) ? pair.bold.data() : pair.regular.data();

        g_fonts.body = io.Fonts->AddFontFromFileTTF(pair.regular.data(), 16.0f, nullptr, ranges);
        g_fonts.strong = io.Fonts->AddFontFromFileTTF(bold, 16.0f, nullptr, ranges);
        g_fonts.heading = io.Fonts->AddFontFromFileTTF(bold, 24.0f, nullptr, ranges);
        g_fonts.small = io.Fonts->AddFontFromFileTTF(pair.regular.data(), 13.0f, nullptr, ranges);
        if (g_fonts.body && g_fonts.strong && g_fonts.heading && g_fonts.small) {
            io.FontDefault = g_fonts.body;
            return;
        }
        io.Fonts->Clear();
    }

    // Fallback: the built-in ProggyClean bitmap font at a few sizes.
    auto makeDefault = [&io](float size) {
        ImFontConfig cfg;
        cfg.SizePixels = size;
        return io.Fonts->AddFontDefault(&cfg);
    };
    g_fonts.body = makeDefault(13.0f);
    g_fonts.strong = g_fonts.body;
    g_fonts.heading = makeDefault(26.0f);
    g_fonts.small = g_fonts.body;
    io.FontDefault = g_fonts.body;
}

void apply(const core::AppearanceSettings& appearance) {
    g_animations = appearance.animations;

    Palette& p = g_palette;
    p.accent = accentPreset(appearance.accent);
    p.accentHover = lerp(p.accent, ImVec4{1, 1, 1, 1}, 0.12f);
    p.accentActive = lerp(p.accent, ImVec4{0, 0, 0, 1}, 0.15f);
    p.accentSoft = withAlpha(p.accent, 0.16f);

    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle{};  // start from a clean slate so ScaleAllSizes() doesn't accumulate

    // ---- Geometry -----------------------------------------------------------
    const float r = appearance.rounding;
    style.WindowPadding = {18.0f, 16.0f};
    style.FramePadding = {10.0f, 6.0f};
    style.CellPadding = {8.0f, 6.0f};
    style.ItemSpacing = {10.0f, 9.0f};
    style.ItemInnerSpacing = {8.0f, 6.0f};
    style.IndentSpacing = 18.0f;
    style.ScrollbarSize = 10.0f;
    style.GrabMinSize = 12.0f;

    style.WindowRounding = r + 4.0f;
    style.ChildRounding = r + 2.0f;
    style.FrameRounding = r;
    style.PopupRounding = r;
    style.ScrollbarRounding = 12.0f;
    style.GrabRounding = r;
    style.TabRounding = r;

    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;

    style.WindowTitleAlign = {0.5f, 0.5f};
    style.ButtonTextAlign = {0.5f, 0.5f};
    style.SelectableTextAlign = {0.0f, 0.5f};
    style.SeparatorTextBorderSize = 1.0f;
    style.AntiAliasedLines = true;
    style.AntiAliasedFill = true;

    // ---- Colours ------------------------------------------------------------
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = p.text;
    c[ImGuiCol_TextDisabled] = p.textDisabled;
    c[ImGuiCol_WindowBg] = p.background;
    c[ImGuiCol_ChildBg] = p.surface;
    c[ImGuiCol_PopupBg] = lerp(p.surface, p.surfaceHi, 0.5f);
    c[ImGuiCol_Border] = p.border;
    c[ImGuiCol_BorderShadow] = {0, 0, 0, 0};

    c[ImGuiCol_FrameBg] = p.surfaceHi;
    c[ImGuiCol_FrameBgHovered] = p.surfaceTop;
    c[ImGuiCol_FrameBgActive] = lerp(p.surfaceTop, p.accent, 0.15f);

    c[ImGuiCol_TitleBg] = p.sidebar;
    c[ImGuiCol_TitleBgActive] = p.sidebar;
    c[ImGuiCol_TitleBgCollapsed] = p.sidebar;
    c[ImGuiCol_MenuBarBg] = p.sidebar;

    c[ImGuiCol_ScrollbarBg] = {0, 0, 0, 0};
    c[ImGuiCol_ScrollbarGrab] = p.surfaceTop;
    c[ImGuiCol_ScrollbarGrabHovered] = lerp(p.surfaceTop, p.text, 0.15f);
    c[ImGuiCol_ScrollbarGrabActive] = p.accent;

    c[ImGuiCol_CheckMark] = {1, 1, 1, 1};
    c[ImGuiCol_SliderGrab] = p.accent;
    c[ImGuiCol_SliderGrabActive] = p.accentHover;

    c[ImGuiCol_Button] = p.surfaceHi;
    c[ImGuiCol_ButtonHovered] = p.surfaceTop;
    c[ImGuiCol_ButtonActive] = p.accentActive;

    c[ImGuiCol_Header] = p.accentSoft;
    c[ImGuiCol_HeaderHovered] = withAlpha(p.accent, 0.22f);
    c[ImGuiCol_HeaderActive] = withAlpha(p.accent, 0.30f);

    c[ImGuiCol_Separator] = p.border;
    c[ImGuiCol_SeparatorHovered] = p.accent;
    c[ImGuiCol_SeparatorActive] = p.accent;

    c[ImGuiCol_ResizeGrip] = {0, 0, 0, 0};
    c[ImGuiCol_ResizeGripHovered] = p.accentSoft;
    c[ImGuiCol_ResizeGripActive] = p.accent;

    c[ImGuiCol_Tab] = p.surface;
    c[ImGuiCol_TabHovered] = p.surfaceTop;
    c[ImGuiCol_TabSelected] = p.accentSoft;
    c[ImGuiCol_TabSelectedOverline] = p.accent;
    c[ImGuiCol_TabDimmed] = p.surface;
    c[ImGuiCol_TabDimmedSelected] = p.surfaceHi;

    c[ImGuiCol_PlotLines] = p.accent;
    c[ImGuiCol_PlotLinesHovered] = p.accentHover;
    c[ImGuiCol_PlotHistogram] = p.accent;
    c[ImGuiCol_PlotHistogramHovered] = p.accentHover;

    c[ImGuiCol_TableHeaderBg] = p.surfaceHi;
    c[ImGuiCol_TableBorderStrong] = p.border;
    c[ImGuiCol_TableBorderLight] = p.border;
    c[ImGuiCol_TableRowBg] = {0, 0, 0, 0};
    c[ImGuiCol_TableRowBgAlt] = {1, 1, 1, 0.02f};

    c[ImGuiCol_TextLink] = p.accentHover;
    c[ImGuiCol_TextSelectedBg] = withAlpha(p.accent, 0.35f);
    c[ImGuiCol_DragDropTarget] = p.accent;
    c[ImGuiCol_NavCursor] = p.accent;
    c[ImGuiCol_NavWindowingHighlight] = p.accent;
    c[ImGuiCol_NavWindowingDimBg] = {0, 0, 0, 0.5f};
    c[ImGuiCol_ModalWindowDimBg] = {0, 0, 0, 0.55f};

    // ---- Scale --------------------------------------------------------------
    style.ScaleAllSizes(appearance.uiScale);
    ImGui::GetIO().FontGlobalScale = appearance.uiScale;
}

}  // namespace ui::theme
