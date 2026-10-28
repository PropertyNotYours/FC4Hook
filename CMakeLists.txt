cmake_minimum_required(VERSION 3.16)
project(fc4hook CXX C)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # static CRT

# Dependencies are bundled in external/ (MinHook, Dear ImGui). To refresh them:
#   git clone https://github.com/TsudaKageyu/minhook external/minhook
#   git clone https://github.com/ocornut/imgui external/imgui
set(MH external/minhook)
set(IG external/imgui)

file(GLOB LUA54_SRC external/lua54/*.c)
set_source_files_properties(${LUA54_SRC} PROPERTIES LANGUAGE CXX)   # Lua errors as C++ exceptions
add_library(fc4hook SHARED
  src/main.cpp src/util.cpp src/game.cpp src/overlay.cpp src/plugins.cpp src/memory.cpp src/lua54.cpp src/input.cpp src/vfs.cpp src/extract.cpp src/texconv.cpp
  ${LUA54_SRC}
  ${MH}/src/buffer.c ${MH}/src/hook.c ${MH}/src/trampoline.c ${MH}/src/hde/hde64.c
  ${IG}/imgui.cpp ${IG}/imgui_draw.cpp ${IG}/imgui_tables.cpp ${IG}/imgui_widgets.cpp
  ${IG}/backends/imgui_impl_win32.cpp ${IG}/backends/imgui_impl_dx11.cpp)
target_include_directories(fc4hook PRIVATE ${MH}/include ${IG} ${IG}/backends external/lua54 include)
target_compile_definitions(fc4hook PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(fc4hook PRIVATE d3d11 dxgi d3dcompiler dwmapi shell32 psapi)
set_target_properties(fc4hook PROPERTIES SUFFIX ".asi")

add_library(example_plugin SHARED examples/example_plugin.cpp)
target_include_directories(example_plugin PRIVATE include)
add_executable(fc4tex tools/fc4tex.cpp src/texconv.cpp)
