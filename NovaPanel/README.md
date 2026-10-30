# Nova Panel — современный интерфейс на Dear ImGui (C++20)

Тёмная тема, боковая панель с векторными иконками, вкладки-«пилюли» с анимированным
переключением и пять разделов: **Combat, Player, Inventory, Explorer, Settings**.

## Сборка

Нужны CMake ≥ 3.20, компилятор с C++20 (GCC 13+, Clang 16+, MSVC 2022) и OpenGL.
ImGui (v1.91.9) скачивается автоматически; GLFW берётся из системы, а если его нет — тоже скачивается.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/nova_panel
```

Тесты работают без окна и без GPU:

```bash
ctest --test-dir build --output-on-failure
```

## Архитектура

```
src/
├── core/                 ← модель и логика, НЕ зависят от ImGui
│   ├── AppState.h/.cpp   данные: настройки, предметы, локации, журнал
│   ├── Actions.h         std::variant всех действий + ActionQueue
│   └── Controller.h/.cpp единственное место, где меняется AppState
├── ui/                   ← только отрисовка
│   ├── Theme.h/.cpp      палитра, ImGuiStyle, шрифты
│   ├── Icons.h/.cpp      векторные иконки через ImDrawList
│   ├── Widgets.h/.cpp    toggle, pillTabs, sidebarButton, Card, CardGrid…
│   ├── MainWindow.h/.cpp сайдбар + заголовок + пилюли + контент + статус-бар
│   └── pages/            по одному классу на вкладку (интерфейс Page)
├── app/Application.*     связывает core и ui, задаёт порядок кадра
└── main.cpp              платформа: GLFW + OpenGL3
```

Поток данных за кадр (однонаправленный):

```
Controller::tick(dt) ──► MainWindow::draw(const AppState&, ActionSink&) ──► Controller::process(queue)
     симуляция              чтение состояния, намерения → actions          применение + валидация
```

* **Страницы только читают** `const core::AppState&`. Чтобы что-то поменять, страница
  правит *черновик* (копию блока настроек) и отправляет `action::UpdateCombat{draft}`
  либо команду (`UseItem`, `TeleportTo`, `ResetAllSettings`, …).
* **Controller** проверяет и ограничивает значения (`std::clamp`), реализует
  бизнес-правила (квестовые предметы нельзя выбросить, в неоткрытую локацию нельзя
  переместиться, зелья лечат игрока), пишет журнал событий.
* Действия копятся в `ActionQueue` и применяются **после** отрисовки, поэтому
  весь кадр видит согласованное состояние.
* Чисто визуальное состояние (строка поиска, выбранная пилюля, открытый модальный диалог)
  живёт во view — в доменную модель оно не попадает.
* Смена темы идёт через наблюдатель `Controller::onAppearanceChanged` → `theme::apply()`.

Добавить новую вкладку: класс-наследник `ui::Page` в `src/ui/pages/`, новый `PageId`
и строка в конструкторе `MainWindow`.

## Параметры стиля (`ui::theme::apply`)

| Параметр | Значение | | Цвет | Значение |
|---|---|---|---|---|
| `WindowRounding` | radius + 4 | | `WindowBg` | `#0D0E12` |
| `ChildRounding` | radius + 2 | | сайдбар | `#111217` |
| `FrameRounding`, `GrabRounding`, `PopupRounding`, `TabRounding` | radius (8 по умолчанию) | | `ChildBg` (карточки) | `#17181E` |
| `ScrollbarRounding` | 12 | | `FrameBg` | `#21222A` |
| `WindowPadding` | 18 × 16 | | `FrameBgHovered` | `#2B2D37` |
| `FramePadding` | 10 × 6 | | `Text` / приглушённый | `#ECEEF3` / `#8A8FA0` |
| `ItemSpacing` | 10 × 9 | | акцент (по умолчанию) | `#7C5CFF` |
| `ScrollbarSize` / `GrabMinSize` | 10 / 12 | | success / warning / danger | `#3DD68C` / `#F5B544` / `#F2555A` |
| `FrameBorderSize` / `ChildBorderSize` | 0 / 1 | | `Border` | белый, 6 % |

Акцент (Violet, Azure, Emerald, Amber, Rose), радиус скругления, масштаб UI и анимации
меняются на вкладке **Settings → Appearance** прямо во время работы.

Шрифт выбирается автоматически (Segoe UI / Arial / Noto Sans / DejaVu Sans). Чтобы
получить задуманный вид, положите `Inter-Regular.ttf` и `Inter-SemiBold.ttf` в `assets/fonts/`.
