#pragma once

// Vector icons drawn with ImDrawList. No icon font or texture is required,
// and they stay crisp at any UI scale.

#include <imgui.h>

#include <cstdint>

namespace ui {

enum class Icon : std::uint8_t { Logo, Combat, Player, Inventory, Explorer, Settings };

// Draws `icon` centred at `center`, fitting a `size` x `size` box.
void drawIcon(ImDrawList* dl, Icon icon, ImVec2 center, float size, ImU32 color);

}  // namespace ui
