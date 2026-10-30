cmake_minimum_required(VERSION 3.20)
project(NovaPanel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(NOVA_BUILD_APP "Build the GLFW/OpenGL executable" ON)
option(NOVA_BUILD_TESTS "Build headless tests" ON)

include(FetchContent)

# ---- Dear ImGui ---------------------------------------------------------------
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.9
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR})

# ---- Warnings -----------------------------------------------------------------
add_library(nova_warnings INTERFACE)
if(MSVC)
    target_compile_options(nova_warnings INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(nova_warnings INTERFACE -Wall -Wextra -Wpedantic)
endif()

# ---- Core: model + actions + logic (no ImGui) -----------------------------------
add_library(nova_core STATIC
    src/core/AppState.cpp
    src/core/Controller.cpp)
target_include_directories(nova_core PUBLIC src)
target_link_libraries(nova_core PRIVATE nova_warnings)

# ---- UI: theme, widgets, pages, layout (ImGui only, no backend) ----------------
add_library(nova_ui STATIC
    src/ui/Theme.cpp
    src/ui/Icons.cpp
    src/ui/Widgets.cpp
    src/ui/MainWindow.cpp
    src/ui/pages/CombatPage.cpp
    src/ui/pages/PlayerPage.cpp
    src/ui/pages/InventoryPage.cpp
    src/ui/pages/ExplorerPage.cpp
    src/ui/pages/SettingsPage.cpp)
target_link_libraries(nova_ui PUBLIC nova_core imgui PRIVATE nova_warnings)

# ---- App: wires core and ui together -------------------------------------------
add_library(nova_app STATIC src/app/Application.cpp)
target_link_libraries(nova_app PUBLIC nova_ui PRIVATE nova_warnings)

# ---- Executable: GLFW + OpenGL3 backend ---------------------------------------
if(NOVA_BUILD_APP)
    find_package(OpenGL REQUIRED)
    find_package(glfw3 3.3 QUIET)
    if(NOT glfw3_FOUND)
        set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(glfw
            GIT_REPOSITORY https://github.com/glfw/glfw.git
            GIT_TAG        3.4
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(glfw)
    endif()

    add_library(imgui_backend STATIC
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
    target_include_directories(imgui_backend PUBLIC ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(imgui_backend PUBLIC imgui glfw OpenGL::GL)

    add_executable(nova_panel src/main.cpp)
    target_link_libraries(nova_panel PRIVATE nova_app imgui_backend nova_warnings)
endif()

# ---- Tests ----------------------------------------------------------------------
if(NOVA_BUILD_TESTS)
    enable_testing()
    add_executable(nova_smoke_test tests/smoke_test.cpp)
    target_link_libraries(nova_smoke_test PRIVATE nova_app nova_warnings)
    add_test(NAME smoke COMMAND nova_smoke_test)
endif()
