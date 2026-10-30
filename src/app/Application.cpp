#include "app/Application.h"

#include "ui/Theme.h"

namespace app {

Application::Application() : m_state(core::makeDemoState()), m_controller(m_state) {
    m_controller.onAppearanceChanged = [](const core::AppearanceSettings& appearance) { ui::theme::apply(appearance); };
    ui::theme::apply(m_state.settings.appearance);
}

void Application::frame(float dt) {
    m_controller.tick(dt);
    m_window.draw(m_state, m_actions);
    m_controller.process(m_actions);
}

}  // namespace app
