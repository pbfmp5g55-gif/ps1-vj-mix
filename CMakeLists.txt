cmake_minimum_required(VERSION 3.20)
project(ps1_vj_mix CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# libvj (in-tree submodule, MIT)
# ---------------------------------------------------------------------------
set(VJ_BUILD_DEMO  OFF CACHE BOOL "" FORCE)
set(VJ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/libvj)

# ---------------------------------------------------------------------------
# Dependencies via FetchContent
# ---------------------------------------------------------------------------
include(FetchContent)

# GLFW for the window + GL context.
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
)
FetchContent_MakeAvailable(glfw)

# Dear ImGui (sources we'll compile in directly + GLFW/OpenGL3 backends).
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.0
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC glfw)

# OpenGL loader — GLAD is overkill for the M1 milestone (we won't draw
# anything yet); use the OS GL library directly and call only the GL
# functions ImGui's GLFW+OpenGL3 backend hands us.
find_package(OpenGL REQUIRED)
target_link_libraries(imgui PUBLIC OpenGL::GL)

# ---------------------------------------------------------------------------
# Mixer executable
# ---------------------------------------------------------------------------
add_executable(vj-mix-spike1
    src/mixer/main.cpp
    src/mixer/gl_loader.cpp
)
target_link_libraries(vj-mix-spike1 PRIVATE vj imgui vj_ipc_ring)
target_include_directories(vj-mix-spike1 PRIVATE src)

# ---------------------------------------------------------------------------
# IPC ring (Windows shared-memory SPSC). POSIX impl deferred.
# ---------------------------------------------------------------------------
add_library(vj_ipc_ring STATIC
    src/mixer/ipc/ipc_ring.cpp
)
target_include_directories(vj_ipc_ring PUBLIC src)

add_executable(vj-mix-ipc-selftest
    src/mixer/ipc/selftest.cpp
)
target_link_libraries(vj-mix-ipc-selftest PRIVATE vj_ipc_ring)
