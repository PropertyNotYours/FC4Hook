#!/usr/bin/env bash
# Cross-compile (Linux -> Windows x64) using zig's clang + mingw:  pip install ziglang && ./build.sh
# On Windows, use CMakeLists.txt with Visual Studio instead.
# Third-party objects are cached in build/obj; FC4Hook's own sources are always rebuilt.
set -euo pipefail
cd "$(dirname "$0")"
ZIG="python3 -m ziglang"
OUT=build; mkdir -p $OUT/obj
CXXFLAGS="-target x86_64-windows-gnu -O2 -std=c++17 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -Iexternal/minhook/include -Iexternal/imgui -Iexternal/imgui/backends -Iexternal/lua54 -Iinclude"
CFLAGS="-target x86_64-windows-gnu -O2 -Iexternal/minhook/include"

objs=()
for f in external/minhook/src/buffer.c external/minhook/src/hook.c external/minhook/src/trampoline.c external/minhook/src/hde/hde64.c; do
  o=$OUT/obj/$(basename "$f").o; [ -f "$o" ] || $ZIG cc $CFLAGS -c "$f" -o "$o"; objs+=("$o"); done
for f in external/imgui/imgui.cpp external/imgui/imgui_draw.cpp external/imgui/imgui_tables.cpp external/imgui/imgui_widgets.cpp \
         external/imgui/backends/imgui_impl_win32.cpp external/imgui/backends/imgui_impl_dx11.cpp; do
  o=$OUT/obj/$(basename "$f").o; [ -f "$o" ] || $ZIG c++ $CXXFLAGS -c "$f" -o "$o"; objs+=("$o"); done
LUAFLAGS="-target x86_64-windows-gnu -O2 -x c++ -std=c++17 -Wno-everything"
for f in external/lua54/*.c; do
  o=$OUT/obj/lua54_$(basename "$f").o; [ -f "$o" ] || $ZIG c++ $LUAFLAGS -c "$f" -o "$o" & objs+=("$o"); done
wait
pids=()
for f in src/*.cpp; do
  o=$OUT/obj/$(basename "$f").o; rm -f "$o"; $ZIG c++ $CXXFLAGS -Wall -Wno-unused-function -c "$f" -o "$o" & pids+=($!); objs+=("$o"); done
for p in "${pids[@]}"; do wait $p; done

$ZIG c++ -target x86_64-windows-gnu -shared -s -o $OUT/fc4hook.asi "${objs[@]}" \
  -ld3d11 -ldxgi -ld3dcompiler_47 -ldwmapi -lgdi32 -luser32 -lshell32 -lpsapi
$ZIG c++ -target x86_64-windows-gnu -O2 -shared -s -Iinclude -Iexternal/imgui -o $OUT/example_plugin.dll examples/example_plugin.cpp
$ZIG c++ -target x86_64-windows-gnu -O2 -s -std=c++17 -static -o $OUT/fc4tex.exe tools/fc4tex.cpp src/texconv.cpp
ls -la $OUT/*.asi $OUT/*.dll $OUT/*.exe
