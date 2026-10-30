// Headless tests: controller logic without any UI, then the full UI rendered
// for every page/section without a GPU (ImGui only builds draw lists).

#include "app/Application.h"
#include "core/Controller.h"
#include "ui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

void testController() {
    core::AppState state = core::makeDemoState();
    core::Controller controller(state);
    using namespace core::action;

    // Values are validated before being committed.
    core::CombatSettings combat = state.combat;
    combat.attackSpeed = 99.0f;
    combat.difficulty = 42;
    controller.handle(UpdateCombat{combat});
    CHECK(state.combat.attackSpeed == 3.0f);
    CHECK(state.combat.difficulty == static_cast<int>(core::kDifficulties.size()) - 1);

    // Consumables heal and are consumed.
    state.player.health = 10.0f;
    const auto potion = std::ranges::find(state.inventory.items, std::string("Health Potion"), &core::Item::name);
    CHECK(potion != state.inventory.items.end());
    const int potionId = potion->id, potionQty = potion->quantity;
    controller.handle(UseItem{potionId});
    CHECK(state.player.health > 10.0f);
    CHECK(std::ranges::find(state.inventory.items, potionId, &core::Item::id)->quantity == potionQty - 1);

    // Quest items can't be dropped, others can.
    const auto quest = std::ranges::find(state.inventory.items, core::ItemCategory::Quest, &core::Item::category);
    const std::size_t count = state.inventory.items.size();
    controller.handle(DropItem{quest->id});
    CHECK(state.inventory.items.size() == count);
    controller.handle(DropItem{potionId});
    CHECK(state.inventory.items.size() == count - 1);

    // Only discovered locations are reachable.
    const auto hidden = std::ranges::find(state.explorer.locations, false, &core::Location::discovered);
    const int hiddenIndex = static_cast<int>(hidden - state.explorer.locations.begin());
    controller.handle(TeleportTo{hiddenIndex});
    CHECK(state.explorer.currentLocation == 0);
    controller.handle(RevealAllLocations{});
    controller.handle(TeleportTo{hiddenIndex});
    CHECK(state.explorer.currentLocation == hiddenIndex);

    // Appearance changes are broadcast.
    int notified = 0;
    controller.onAppearanceChanged = [&](const core::AppearanceSettings&) { ++notified; };
    core::AppearanceSettings appearance = state.settings.appearance;
    appearance.accent = 2;
    controller.handle(UpdateAppearance{appearance});
    controller.handle(UpdateAppearance{appearance});  // unchanged -> no notification
    CHECK(notified == 1);

    // Time advances unless frozen.
    const float t0 = state.explorer.world.timeOfDay;
    controller.tick(1.0f);
    CHECK(state.explorer.world.timeOfDay != t0);
    state.explorer.world.freezeTime = true;
    const float t1 = state.explorer.world.timeOfDay;
    controller.tick(1.0f);
    CHECK(state.explorer.world.timeOfDay == t1);
}

void renderFrame(app::Application& application) {
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    application.frame(1.0f / 60.0f);
    ImGui::Render();
}

void testUi() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1280, 800};
    ui::theme::loadFonts(io);
    io.Fonts->Build();

    {
        app::Application application;

        // Every page and every pill section renders.
        for (int p = 0; p < static_cast<int>(ui::PageId::Count); ++p) {
            application.window().setActivePage(static_cast<ui::PageId>(p));
            for (int section = 0; section < 3; ++section) {
                application.window().setActiveSection(section);
                for (int i = 0; i < 3; ++i) renderFrame(application);
                CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
            }
        }

        // Narrow window -> single-column layout still works.
        io.DisplaySize = {560, 700};
        for (int i = 0; i < 3; ++i) renderFrame(application);
        io.DisplaySize = {1280, 800};

        // Clicking the third sidebar icon navigates to Inventory.
        application.window().setActivePage(ui::PageId::Combat);
        renderFrame(application);
        io.AddMousePosEvent(39.0f, 205.0f);
        renderFrame(application);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        renderFrame(application);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        renderFrame(application);
        CHECK(application.window().activePage() == ui::PageId::Inventory);
    }

    ImGui::DestroyContext();
}

}  // namespace

int main() {
    testController();
    testUi();
    if (g_failures == 0) std::puts("All checks passed");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
