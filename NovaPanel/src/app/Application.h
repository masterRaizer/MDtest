#pragma once

// Wires model, logic and view together and defines the order of a frame:
//   1. Controller::tick      - simulation
//   2. MainWindow::draw      - read-only rendering, user intent -> ActionQueue
//   3. Controller::process   - actions are applied to the state

#include "core/Actions.h"
#include "core/AppState.h"
#include "core/Controller.h"
#include "ui/MainWindow.h"

namespace app {

class Application {
public:
    Application();

    // Call between ImGui::NewFrame() and ImGui::Render().
    void frame(float dt);

    [[nodiscard]] const core::AppState& state() const { return m_state; }
    [[nodiscard]] ui::MainWindow& window() { return m_window; }
    [[nodiscard]] core::Controller& controller() { return m_controller; }

private:
    core::AppState m_state;
    core::ActionQueue m_actions;
    core::Controller m_controller;
    ui::MainWindow m_window;
};

}  // namespace app
