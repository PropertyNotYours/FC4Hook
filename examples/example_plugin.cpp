// Minimal native FC4Hook plugin. Build as a DLL, drop into fc4hook\plugins\.
#include "../include/fc4hook.h"

static const FC4HookAPI* g;

// Exposed to Lua as  ExampleHello("name")
static int L_ExampleHello(lua_State* L) {
    const char* who = g->lua_gettop(L) >= 1 ? g->lua_tolstring(L, 1, nullptr) : "nobody";
    g->Log("[example] hello, %s!", who ? who : "nil");
    return 0;
}

static void OnTick(lua_State*) {
    // Runs on the script thread about once per frame. Safe to use Lua here.
}

// API v3: runs on the render thread every frame, right before the game's Present.
// GetD3D11Device()/GetD3D11Context() are the game's own ID3D11Device / immediate context, so a
// plugin can create textures/shaders and draw its own geometry here (save and restore any
// pipeline state you touch). To hook one of the game's own D3D or engine objects, use
// HookVtable(object, index, detour, &original); for plain functions, FindPattern + CreateHook.
static void OnPresent(void* swapChain) {
    static bool said = false;
    if (said || !g->GetD3D11Device()) return;
    said = true;
    g->LogLevel(FC4_LOG_DEBUG, "[example] render callback: swapchain %p, device %p, context %p",
                swapChain, g->GetD3D11Device(), g->GetD3D11Context());
}

extern "C" __declspec(dllexport) int FC4ModInit(const FC4HookAPI* api) {
    if (api->version < FC4HOOK_API_VERSION) return 0;   // needs FC4Hook 1.1+ (API v3)
    g = api;
    g->RegisterLuaFunction("ExampleHello", L_ExampleHello);
    g->OnGameTick(OnTick);
    g->OnPresent(OnPresent);
    // The game's file id for a path, as its archives use it (handy for loose-file mods):
    unsigned long long id = (unsigned long long)g->FileId("entityarchetypeslibrary\\7602852352217.ark.fcb");
    g->LogLevel(FC4_LOG_INFO, "[example] plugin initialised on FC4Hook %s (file id demo: %016llx); "
                "try ExampleHello(\"Ajay\") in the console", g->VersionString(), id);
    return 1;
}
