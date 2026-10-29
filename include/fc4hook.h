// fc4hook.h - public plugin API for FC4Hook (Far Cry 4 mod framework)
//
// A plugin is a normal x64 DLL placed in  <game>\bin\fc4hook\plugins\
// that exports:
//
//     extern "C" __declspec(dllexport) bool FC4ModInit(const FC4HookAPI* api);
//
// Return false to be unloaded. Keep the api pointer; it stays valid for the
// lifetime of the process. Check  api->version >= FC4HOOK_API_VERSION  (newer
// FC4Hook builds only ever add fields to the end of FC4HookAPI).
//
// THREADING RULES (important):
//   * Lua is NOT thread safe. Only touch lua_State inside an OnGameTick
//     callback (runs on the game's script thread) or a function you registered
//     with RegisterLuaFunction (called by the game's Lua VM).
//   * OnDrawMenu callbacks run on the render thread inside an ImGui frame.
//     From there, use QueueLua() to ask the script thread to do things.
//   * OnPresent callbacks run on the render thread every frame (menu open or not), right
//     before the game's IDXGISwapChain::Present. The device/context are the game's own:
//     save and restore any pipeline state you change.
#pragma once
#include <stdint.h>
#include <stddef.h>

// 1 = FC4Hook 0.x, 2 = FC4Hook 1.0 (adds LogLevel, SetState/GetState, GetRootDir, VersionString)
// 3 = FC4Hook 1.2 (adds vtable hooks, render callback + D3D11 objects, file ids, Lua 5.4 queue)
#define FC4HOOK_API_VERSION 3

// Log levels for LogLevel()
#define FC4_LOG_DEBUG 0
#define FC4_LOG_INFO  1
#define FC4_LOG_WARN  2
#define FC4_LOG_ERROR 3

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lua_State lua_State;
typedef int (*lua_CFunction)(lua_State* L);

// Lua 5.1 pseudo-indices
#define FC4_LUA_REGISTRYINDEX (-10000)
#define FC4_LUA_GLOBALSINDEX  (-10002)

typedef struct FC4HookAPI {
    int version;                       // FC4HOOK_API_VERSION

    // --- utilities -------------------------------------------------------
    void      (*Log)(const char* fmt, ...);
    // IDA-style pattern ("48 8B ?? ?? E8"), scanned in the given module
    // (NULL = FC64.dll). Returns 0 if not found.
    uintptr_t (*FindPattern)(const char* module, const char* pattern);
    // MinHook wrappers. CreateHook also enables the hook.
    int       (*CreateHook)(void* target, void* detour, void** original);
    int       (*RemoveHook)(void* target);

    // --- scripting ------------------------------------------------------
    // Queue Lua source to run on the script thread at the next tick.
    void      (*QueueLua)(const char* code, const char* chunkName);
    // Register a global Lua function (applied on the script thread).
    void      (*RegisterLuaFunction)(const char* name, lua_CFunction fn);

    // --- callbacks --------------------------------------------------------
    void      (*OnGameTick)(void (*cb)(lua_State* L));  // script thread, ~once per frame
    void      (*OnDrawMenu)(void (*cb)(void));          // render thread, inside ImGui frame
    void*     (*GetImGuiContext)(void);                 // ImGui::SetCurrentContext((ImGuiContext*)...)

    // --- raw game Lua API (resolved by signature from FC64.dll) ----------
    lua_State*  (*GetLuaState)(void);
    int         (*lua_gettop)(lua_State* L);
    void        (*lua_settop)(lua_State* L, int idx);
    void        (*lua_pushcclosure)(lua_State* L, lua_CFunction fn, int n);
    void        (*lua_setfield)(lua_State* L, int idx, const char* k);
    void        (*lua_getfield)(lua_State* L, int idx, const char* k);
    const char* (*lua_tolstring)(lua_State* L, int idx, size_t* len);
    int         (*luaL_loadbuffer)(lua_State* L, const char* buf, size_t sz, const char* name);
    int         (*lua_pcall)(lua_State* L, int nargs, int nresults, int errfunc);

    // --- API version 2 (FC4Hook 1.0) --------------------------------------
    void        (*LogLevel)(int level, const char* fmt, ...);     // FC4_LOG_*
    // Shared key/value store, also visible to Lua via ModSetState/ModGetState.
    // GetState copies the value into buf (always NUL-terminated); returns 0 if missing.
    void        (*SetState)(const char* key, const char* value);
    int         (*GetState)(const char* key, char* buf, size_t bufSize);
    const wchar_t* (*GetRootDir)(void);                          // ...\bin\fc4hook\  (trailing slash)
    const char* (*VersionString)(void);                          // "1.3.0"

    // --- API version 3 (FC4Hook 1.2) --------------------------------------
    // Replace entry `index` of an object's virtual table (affects every object of that class).
    // *original receives the old function; call it from your detour. Returns 1 on success.
    int         (*HookVtable)(void* object, int index, void* detour, void** original);
    int         (*UnhookVtable)(void* object, int index, void* original);
    // Render: called every frame before Present. swapChain is an IDXGISwapChain*.
    void        (*OnPresent)(void (*cb)(void* swapChain));
    void*       (*GetD3D11Device)(void);                         // ID3D11Device*, NULL until the overlay attached
    void*       (*GetD3D11Context)(void);                        // ID3D11DeviceContext* (immediate)
    void*       (*GetSwapChain)(void);                           // IDXGISwapChain*
    // The game's file id for a path inside its data (CRC-64 of the lower-case path), as used by
    // its archives. Archetype files are "entityarchetypeslibrary\\<id>.ark.fcb".
    uint64_t    (*FileId)(const char* path);
    // Run Lua 5.4 code (FC4Hook's embedded runtime) on the script thread.
    void        (*QueueLua54)(const char* code, const char* chunkName);
    // Loose-file mod status text ("12 files (unchanged, reused), mounted").
    int         (*GetLooseStatus)(char* buf, size_t bufSize);
} FC4HookAPI;

typedef int (*FC4ModInitFn)(const FC4HookAPI* api);

#ifdef __cplusplus
}
#endif
