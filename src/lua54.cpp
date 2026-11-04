// Lua 5.4 runtime that runs alongside the game's own Lua 5.1.
//
// Why: the game's Lua uses 32-bit floats for numbers (no exact 64-bit ids/addresses) and
// has a low per-function register limit. FC4Hook embeds a stock Lua 5.4 with real 64-bit
// integers for mods, and bridges it to the game:
//
//   game.call("CAPI_Pawn.GetPawnHealth", game.player())   call any game Lua function
//   game.CAPI_Pawn.GetPawnHealth(game.player())           same thing, proxy syntax
//   game.eval("return GetTimeOfDay()")                     run game-Lua source
//   mem.u32(mem.module("FC64.dll") + 0x2DD8958)            memory with plain integers
//   fc4.on_tick(function(dt) ... end)                       per-frame callbacks
//
// The 5.4 state lives on the game's script thread (it's ticked from the same place as
// the game-Lua bridge), so game calls are synchronous and safe.
// Scripts: fc4hook\lua\autorun\*.lua (run in name order), modules in fc4hook\lua\ (require).
#include "core.h"
#include <mutex>
#include <cstring>
#include <cstdio>
#include <algorithm>

extern "C++" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

namespace fc4 {

static lua_State*   S = nullptr;          // the 5.4 state
static lua_State*   s_game = nullptr;     // game Lua state for the current call
static const char*  s_status = "not started";
static bool         s_wantReload = true;
static ULONGLONG    s_lastTick = 0;
static double       s_time = 0;

struct Job { std::string code, chunk; };
static std::mutex          s_jobMx;
static std::vector<Job>    s_jobs;
static std::vector<int>    s_pressed;
static std::vector<Lua54Button> s_buttons;  // guarded by s_jobMx
static int                 s_nextButton = 1;

const char* Lua54Status() { return s_status; }
void Lua54Queue(const std::string& code, const std::string& chunk) {
    std::lock_guard<std::mutex> lk(s_jobMx); s_jobs.push_back({ code, chunk });
}
void Lua54Reload() { s_wantReload = true; }
std::vector<Lua54Button> Lua54Buttons() { std::lock_guard<std::mutex> lk(s_jobMx); return s_buttons; }
void Lua54PressButton(int id) { std::lock_guard<std::mutex> lk(s_jobMx); s_pressed.push_back(id); }

// ------------------------------------------------------------ helpers ----
static std::string ArgsToString(lua_State* L, int from) {
    std::string s;
    int n = lua_gettop(L);
    for (int i = from; i <= n; ++i) {
        if (i > from) s += '\t';
        s += luaL_tolstring(L, i, nullptr);
        lua_pop(L, 1);
    }
    return s;
}

static int Traceback(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    luaL_traceback(L, L, msg ? msg : "(error object is not a string)", 1);
    return 1;
}

// pcall with traceback; logs and returns false on error. Leaves nresults on success.
static bool SafeCall(lua_State* L, int nargs, int nresults, const char* what) {
    int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, Traceback);
    lua_insert(L, base);
    int st = lua_pcall(L, nargs, nresults, base);
    lua_remove(L, base);
    if (st != LUA_OK) {
        LogL(LOG_ERROR, "[lua54] %s: %s", what, lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    return true;
}

// ------------------------------------------------------------ fc4.* ------
static int f_log(lua_State* L)   { LogL(LOG_INFO,  "[lua54] %s", ArgsToString(L, 1).c_str()); return 0; }
static int f_debug(lua_State* L) { LogL(LOG_DEBUG, "[lua54] %s", ArgsToString(L, 1).c_str()); return 0; }
static int f_warn(lua_State* L)  { LogL(LOG_WARN,  "[lua54] %s", ArgsToString(L, 1).c_str()); return 0; }
static int f_error(lua_State* L) { LogL(LOG_ERROR, "[lua54] %s", ArgsToString(L, 1).c_str()); return 0; }
static int f_time(lua_State* L)  { lua_pushnumber(L, s_time); return 1; }
static int f_file_id(lua_State* L) { lua_pushinteger(L, (lua_Integer)FileId(luaL_checkstring(L, 1))); return 1; }
static int f_file_log(lua_State* L) {
    if (!lua_isnone(L, 1)) FileLogSetEnabled(lua_toboolean(L, 1));
    lua_pushboolean(L, FileLogEnabled()); return 1;
}
static int f_loose_status(lua_State* L) { lua_pushstring(L, VfsStatus().c_str()); return 1; }
static int f_frame(lua_State* L) { lua_pushinteger(L, (lua_Integer)g_frameCounter.load()); return 1; }
static int f_version(lua_State* L) { lua_pushstring(L, FC4HOOK_VERSION); return 1; }

static int f_key_down(lua_State* L) {
    int vk = (int)luaL_checkinteger(L, 1);
    bool down = vk > 0 && vk < 256 && GameHasFocus() && !MenuWantsKeyboard() && (GetAsyncKeyState(vk) & 0x8000);
    lua_pushboolean(L, down);
    return 1;
}

static int f_state_set(lua_State* L) { SetState(luaL_checkstring(L, 1), luaL_tolstring(L, 2, nullptr)); return 0; }
static int f_state_get(lua_State* L) {
    std::string v = GetState(luaL_checkstring(L, 1), "\x01");
    if (v == "\x01") lua_pushnil(L); else lua_pushlstring(L, v.data(), v.size());
    return 1;
}

static bool SafeRel(const char* p) { return p && !strstr(p, "..") && !strchr(p, ':'); }
static std::wstring RootPath(const char* rel) {
    std::wstring w = g_rootDir + Widen(rel);
    for (auto& c : w) if (c == L'/') c = L'\\';
    return w;
}
static int f_read_file(lua_State* L) {
    const char* rel = luaL_checkstring(L, 1);
    if (!SafeRel(rel)) return luaL_error(L, "path must stay inside fc4hook\\");
    FILE* f = _wfopen(RootPath(rel).c_str(), L"rb");
    if (!f) { lua_pushnil(L); return 1; }
    luaL_Buffer b; luaL_buffinit(L, &b);
    char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) luaL_addlstring(&b, buf, n);
    fclose(f);
    luaL_pushresult(&b);
    return 1;
}
static int f_write_file(lua_State* L) {
    const char* rel = luaL_checkstring(L, 1);
    size_t len; const char* data = luaL_checklstring(L, 2, &len);
    bool append = lua_toboolean(L, 3);
    if (!SafeRel(rel)) return luaL_error(L, "path must stay inside fc4hook\\");
    std::wstring p = RootPath(rel);
    for (size_t i = g_rootDir.size(); i < p.size(); ++i) if (p[i] == L'\\') CreateDirectoryW(p.substr(0, i).c_str(), nullptr);
    FILE* f = _wfopen(p.c_str(), append ? L"ab" : L"wb");
    if (!f) { lua_pushboolean(L, 0); return 1; }
    fwrite(data, 1, len, f); fclose(f);
    lua_pushboolean(L, 1);
    return 1;
}

// fc4.menu.button(label, fn [, group]) -> id      shows in Developer > Lua 5.4
static int f_menu_button(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    const char* group = luaL_optstring(L, 3, "Scripts");
    lua_getfield(L, LUA_REGISTRYINDEX, "fc4.buttons");
    int id;
    { std::lock_guard<std::mutex> lk(s_jobMx); id = s_nextButton++; s_buttons.push_back({ id, label, group }); }
    lua_pushvalue(L, 2);
    lua_rawseti(L, -2, id);
    lua_pop(L, 1);
    lua_pushinteger(L, id);
    return 1;
}
static int f_menu_clear(lua_State* L) {
    { std::lock_guard<std::mutex> lk(s_jobMx); s_buttons.clear(); }
    lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, "fc4.buttons");
    return 0;
}

// ------------------------------------------------------------ game bridge -
// 5.4 values -> game-Lua source text.
static void Quote51(std::string& out, const char* s, size_t n) {
    out += '"';
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c < 32 || c == 127) { char b[8]; snprintf(b, sizeof(b), "\\%03d", c); out += b; }
        else out += (char)c;
    }
    out += '"';
}

static void Ser51(lua_State* L, int idx, std::string& out, int depth) {
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TNIL: out += "nil"; return;
    case LUA_TBOOLEAN: out += lua_toboolean(L, idx) ? "true" : "false"; return;
    case LUA_TNUMBER: {
        char b[48];
        if (lua_isinteger(L, idx)) {
            lua_Integer v = lua_tointeger(L, idx);
            // the game's numbers are 32-bit floats: big integers (entity/template ids) go as
            // decimal strings, which is how the game's own functions read ids anyway
            if (v > 16777216 || v < -16777216) { snprintf(b, sizeof(b), "\"%lld\"", (long long)v); out += b; }
            else { snprintf(b, sizeof(b), "%lld", (long long)v); out += b; }
        } else {
            double d = lua_tonumber(L, idx);
            if (d != d) out += "0/0"; else if (d > 1e308) out += "1/0"; else if (d < -1e308) out += "-1/0";
            else { snprintf(b, sizeof(b), "%.9g", d); out += b; }
        }
        return;
    }
    case LUA_TSTRING: { size_t n; const char* s = lua_tolstring(L, idx, &n); Quote51(out, s, n); return; }
    case LUA_TTABLE: {
        if (lua_getfield(L, idx, "__gameref") == LUA_TNUMBER) {
            char b[64]; snprintf(b, sizeof(b), "FC4Bridge.refs[%lld]", (long long)lua_tointeger(L, -1));
            lua_pop(L, 1); out += b; return;
        }
        lua_pop(L, 1);
        if (depth > 4) luaL_error(L, "table nested too deep to pass to the game");
        out += "{";
        lua_pushnil(L);
        bool first = true;
        while (lua_next(L, idx)) {
            if (!first) out += ",";
            first = false;
            out += "["; Ser51(L, -2, out, depth + 1); out += "]=";
            Ser51(L, -1, out, depth + 1);
            lua_pop(L, 1);
        }
        out += "}";
        return;
    }
    default:
        luaL_error(L, "can't pass a %s to the game", luaL_typename(L, idx));
    }
}

static bool ValidPath(const char* p) {
    if (!p || !*p) return false;
    for (const char* c = p; *c; ++c) if (!(isalnum((unsigned char)*c) || *c == '_' || *c == '.' || *c == ':')) return false;
    return true;
}

// R(n, "tostring") in results -> a handle table (metatable "fc4.gameref")
static int MakeRef(lua_State* L) {
    lua_Integer n = luaL_checkinteger(L, 1);
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, n); lua_setfield(L, -2, "__gameref");
    lua_pushvalue(L, 2); lua_setfield(L, -2, "text");
    luaL_setmetatable(L, "fc4.gameref");
    return 1;
}
static int RefToString(lua_State* L) {
    lua_getfield(L, 1, "text");
    if (!lua_isstring(L, -1)) { lua_pop(L, 1); lua_pushstring(L, "<game value>"); }
    return 1;
}

// Runs "return FC4Bridge.call(<f>, args...)" in the game and pushes the decoded results.
// Returns the number of results, first one being ok (boolean).
static int GameCall(lua_State* L, const char* path, int firstArg, int lastArg) {
    if (!s_game) return luaL_error(L, "the game's Lua isn't running (load into the world first)");
    if (!ValidPath(path)) return luaL_error(L, "bad game function name '%s'", path);
    std::string code = "return FC4Bridge and FC4Bridge.call(";
    std::string p = path, self;
    size_t colon = p.find(':');
    if (colon != std::string::npos) {                       // Table:Method(...) -> pass the table as self
        self = p.substr(0, colon);
        code += "FC4Bridge.get(\"" + self + "\")[\"" + p.substr(colon + 1) + "\"],FC4Bridge.get(\"" + self + "\")";
    } else {
        code += "FC4Bridge.get(\"" + p + "\")";
    }
    for (int i = firstArg; i <= lastArg; ++i) { code += ","; Ser51(L, i, code, 0); }
    code += ")";
    std::string res;
    if (!GameEvalString(s_game, code, "=lua54.call", &res) || res.empty())
        return luaL_error(L, "game call failed: %s (FC4Hook core scripts not loaded?)", res.empty() ? "no result" : res.c_str());
    int top = lua_gettop(L);
    if (luaL_loadbufferx(L, res.data(), res.size(), "=game result", "t") != LUA_OK) return lua_error(L);
    lua_newtable(L);                                         // _ENV for the result chunk: just R
    lua_pushcfunction(L, MakeRef); lua_setfield(L, -2, "R");
    lua_pushnumber(L, 0.0 / 0.0); lua_setfield(L, -2, "nan");
    lua_setupvalue(L, -2, 1);
    lua_call(L, 0, LUA_MULTRET);
    return lua_gettop(L) - top;
}

// game.call(name, ...) -> results (raises the game's error)
static int g_call(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    int n = GameCall(L, name, 2, lua_gettop(L));
    int first = lua_gettop(L) - n + 1;
    if (n == 0 || !lua_toboolean(L, first)) {
        const char* err = n >= 2 ? lua_tostring(L, first + 1) : "unknown error";
        return luaL_error(L, "%s: %s", name, err ? err : "?");
    }
    return n - 1;
}
// game.pcall(name, ...) -> ok, results... | false, error
static int g_pcall(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    return GameCall(L, name, 2, lua_gettop(L));
}
// game.eval(code) -> whatever the code returns
static int g_eval(lua_State* L) {
    size_t n; const char* src = luaL_checklstring(L, 1, &n);
    if (!s_game) return luaL_error(L, "the game's Lua isn't running");
    std::string code = "local f, e = loadstring(";
    Quote51(code, src, n);
    code += ", \"=lua54.eval\") if not f then return \"return false,\" .. string.format(\"%q\", e) end return FC4Bridge.call(f)";
    std::string res;
    if (!GameEvalString(s_game, code, "=lua54.eval", &res) || res.empty()) return luaL_error(L, "game eval failed: %s", res.c_str());
    int top = lua_gettop(L);
    if (luaL_loadbufferx(L, res.data(), res.size(), "=game result", "t") != LUA_OK) return lua_error(L);
    lua_newtable(L); lua_pushcfunction(L, MakeRef); lua_setfield(L, -2, "R"); lua_setupvalue(L, -2, 1);
    lua_call(L, 0, LUA_MULTRET);
    int nres = lua_gettop(L) - top;
    if (nres == 0 || !lua_toboolean(L, top + 1)) return luaL_error(L, "%s", nres >= 2 ? lua_tostring(L, top + 2) : "eval failed");
    return nres - 1;
}
// game.has(name) -> true if that game function exists
static int g_has(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    if (!s_game || !ValidPath(name)) { lua_pushboolean(L, 0); return 1; }
    std::string res;
    GameEvalString(s_game, std::string("return type(FC4Bridge and FC4Bridge.get(\"") + name + "\"))", "=lua54.has", &res);
    lua_pushboolean(L, res == "function");
    return 1;
}
static int g_ready(lua_State* L) { lua_pushboolean(L, s_game != nullptr && LuaReady()); return 1; }
static int g_free(lua_State* L) {
    if (lua_getfield(L, 1, "__gameref") == LUA_TNUMBER && s_game) {
        char b[64]; snprintf(b, sizeof(b), "FC4Bridge.free(%lld) return ''", (long long)lua_tointeger(L, -1));
        GameEvalString(s_game, b, "=lua54.free", nullptr);
    }
    return 0;
}

// Proxy: game.CAPI_Pawn.SpawnWeapon(...) == game.call("CAPI_Pawn.SpawnWeapon", ...)
static int ProxyCall(lua_State* L) {
    lua_getfield(L, 1, "__path");
    std::string path = lua_tostring(L, -1); lua_pop(L, 1);
    lua_remove(L, 1);                                         // drop the proxy itself
    lua_pushstring(L, path.c_str()); lua_insert(L, 1);
    return g_call(L);
}
static int ProxyIndex(lua_State* L) {
    const char* key = luaL_checkstring(L, 2);
    lua_getfield(L, 1, "__path");
    std::string path = lua_tostring(L, -1); lua_pop(L, 1);
    lua_createtable(L, 0, 1);
    lua_pushstring(L, (path + "." + key).c_str()); lua_setfield(L, -2, "__path");
    luaL_setmetatable(L, "fc4.proxy");
    return 1;
}
static int GameIndex(lua_State* L) {                          // game.<Name> for names that aren't bridge functions
    const char* key = luaL_checkstring(L, 2);
    lua_createtable(L, 0, 1);
    lua_pushstring(L, key); lua_setfield(L, -2, "__path");
    luaL_setmetatable(L, "fc4.proxy");
    return 1;
}

// game.player() -> local player id (as a string; pass it straight back to game functions)
static int g_player(lua_State* L) {
    if (!s_game) { lua_pushnil(L); return 1; }
    std::string res;
    if (!GameEvalString(s_game, "local ok, id = pcall(GetLocalPlayerId) if ok and id ~= nil then return tostring(id) end return ''", "=lua54.player", &res) || res.empty()) {
        lua_pushnil(L); return 1;
    }
    lua_pushlstring(L, res.data(), res.size());
    return 1;
}

// ------------------------------------------------------------ mem.* ------
static uintptr_t AddrArg(lua_State* L, int i) {
    if (lua_type(L, i) == LUA_TSTRING) {
        uintptr_t a; std::string err;
        if (!ParseAddress(lua_tostring(L, i), &a, &err)) luaL_error(L, "bad address: %s", err.c_str());
        return a;
    }
    return (uintptr_t)luaL_checkinteger(L, i);
}

template <typename T> static int ReadT(lua_State* L) {
    uintptr_t a = AddrArg(L, 1);
    if (!MemReadable(a, sizeof(T))) { lua_pushnil(L); return 1; }
    T v; memcpy(&v, (void*)a, sizeof(T));
    if constexpr (std::is_floating_point<T>::value) lua_pushnumber(L, (lua_Number)v);
    else lua_pushinteger(L, (lua_Integer)v);
    return 1;
}
template <typename T> static int WriteT(lua_State* L) {
    uintptr_t a = AddrArg(L, 1);
    T v;
    if constexpr (std::is_floating_point<T>::value) v = (T)luaL_checknumber(L, 2);
    else v = (T)(lua_isinteger(L, 2) ? lua_tointeger(L, 2) : (lua_Integer)luaL_checknumber(L, 2));
    if (!MemWritable(a, sizeof(T))) { lua_pushboolean(L, 0); return 1; }
    memcpy((void*)a, &v, sizeof(T));
    lua_pushboolean(L, 1);
    return 1;
}

static int m_addr(lua_State* L) { lua_pushinteger(L, (lua_Integer)AddrArg(L, 1)); return 1; }
static int m_hex(lua_State* L) { lua_pushstring(L, Hex((uintptr_t)luaL_checkinteger(L, 1)).c_str()); return 1; }
static int m_readable(lua_State* L) { lua_pushboolean(L, MemReadable(AddrArg(L, 1), (size_t)luaL_optinteger(L, 2, 1))); return 1; }
static int m_writable(lua_State* L) { lua_pushboolean(L, MemWritable(AddrArg(L, 1), (size_t)luaL_optinteger(L, 2, 1))); return 1; }

static int m_module(lua_State* L) {
    const char* n = luaL_optstring(L, 1, "FC64.dll");
    HMODULE m = GetModuleHandleA(n);
    if (!m && !strchr(n, '.')) m = GetModuleHandleA((std::string(n) + ".dll").c_str());
    if (!m) { lua_pushnil(L); return 1; }
    auto* nt = (IMAGE_NT_HEADERS64*)((uint8_t*)m + ((IMAGE_DOS_HEADER*)m)->e_lfanew);
    lua_pushinteger(L, (lua_Integer)(uintptr_t)m);
    lua_pushinteger(L, (lua_Integer)nt->OptionalHeader.SizeOfImage);
    return 2;
}
static int m_scan(lua_State* L) {
    uintptr_t r = FindPattern(luaL_optstring(L, 2, "FC64.dll"), luaL_checkstring(L, 1));
    if (r) lua_pushinteger(L, (lua_Integer)r); else lua_pushnil(L);
    return 1;
}
static int m_rip(lua_State* L) {
    uintptr_t a = AddrArg(L, 1);
    int off = (int)luaL_optinteger(L, 2, 3), len = (int)luaL_optinteger(L, 3, 7);
    if (!MemReadable(a + off, 4)) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, (lua_Integer)ResolveRip(a, off, len));
    return 1;
}
static int m_chain(lua_State* L) {                            // mem.chain(base, off1, off2, ...) = [[base]+off1]+off2...
    uintptr_t a = AddrArg(L, 1);
    for (int i = 2; i <= lua_gettop(L); ++i) {
        if (!MemReadable(a, 8)) { lua_pushnil(L); return 1; }
        a = *(uintptr_t*)a + (uintptr_t)luaL_checkinteger(L, i);
    }
    lua_pushinteger(L, (lua_Integer)a);
    return 1;
}
static int m_str(lua_State* L) {
    uintptr_t a = AddrArg(L, 1);
    lua_Integer max = luaL_optinteger(L, 2, 256);
    std::string s;
    for (lua_Integer i = 0; i < max && MemReadable(a + i, 1) && *(char*)(a + i); ++i) s += *(char*)(a + i);
    lua_pushlstring(L, s.data(), s.size());
    return 1;
}
static int m_bytes(lua_State* L) {                            // raw bytes as a Lua string
    uintptr_t a = AddrArg(L, 1);
    lua_Integer n = luaL_checkinteger(L, 2);
    if (n <= 0 || n > (16 << 20) || !MemReadable(a, (size_t)n)) { lua_pushnil(L); return 1; }
    lua_pushlstring(L, (const char*)a, (size_t)n);
    return 1;
}
static int m_write_bytes(lua_State* L) {
    uintptr_t a = AddrArg(L, 1);
    size_t n; const char* d = luaL_checklstring(L, 2, &n);
    if (!MemWritable(a, n)) { lua_pushboolean(L, 0); return 1; }
    memcpy((void*)a, d, n);
    lua_pushboolean(L, 1);
    return 1;
}
static int m_alloc(lua_State* L) {
    lua_Integer n = luaL_checkinteger(L, 1);
    void* p = (n > 0 && n <= (64 << 20)) ? VirtualAlloc(nullptr, (SIZE_T)n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) : nullptr;
    if (p) lua_pushinteger(L, (lua_Integer)(uintptr_t)p); else lua_pushnil(L);
    return 1;
}
typedef uintptr_t (*Call6_t)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static int m_call(lua_State* L) {                             // ADVANCED: native call, up to 6 integer args
    uintptr_t fn = AddrArg(L, 1);
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)fn, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
        return luaL_error(L, "not executable: %s", Hex(fn).c_str());
    uintptr_t a[6] = {};
    for (int i = 0; i < 6; ++i) if (!lua_isnoneornil(L, i + 2)) a[i] = AddrArg(L, i + 2);
    lua_pushinteger(L, (lua_Integer)((Call6_t)fn)(a[0], a[1], a[2], a[3], a[4], a[5]));
    return 1;
}
static int m_vtable(lua_State* L) {                           // mem.vtable(obj, i) -> address of virtual function i
    uintptr_t o = AddrArg(L, 1);
    lua_Integer i = luaL_checkinteger(L, 2);
    if (!MemReadable(o, 8)) { lua_pushnil(L); return 1; }
    uintptr_t vt = *(uintptr_t*)o;
    if (i < 0 || i > 4096 || !MemReadable(vt + (uintptr_t)i * 8, 8)) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, (lua_Integer)*(uintptr_t*)(vt + (uintptr_t)i * 8));
    return 1;
}
static int m_entity(lua_State* L) {                           // mem.entity(id) -> engine object address
    if (!s_game) { lua_pushnil(L); return 1; }
    uint64_t id;
    if (lua_isinteger(L, 1)) id = (uint64_t)lua_tointeger(L, 1);
    else id = _strtoui64(luaL_checkstring(L, 1), nullptr, 10);
    void* o = EntityObject(s_game, id);
    if (o) lua_pushinteger(L, (lua_Integer)(uintptr_t)o); else lua_pushnil(L);
    return 1;
}
static int m_player(lua_State* L) {
    g_player(L);
    if (lua_isnil(L, -1)) return 1;
    return m_entity(L);
}
static int m_native(lua_State* L) {
    void* fn = s_game ? LuaNativeFn(s_game, luaL_checkstring(L, 1)) : nullptr;
    if (fn) lua_pushinteger(L, (lua_Integer)(uintptr_t)fn); else lua_pushnil(L);
    return 1;
}

// ------------------------------------------------------------ setup ------
static const char* kPrelude = R"LUA(
-- FC4Hook Lua 5.4 prelude
print = fc4.log
os.exit = nil
local ticks, timers, nextId = {}, {}, 1
function fc4.on_tick(fn) local id = nextId; nextId = nextId + 1; ticks[id] = fn; return id end
function fc4.off(id) ticks[id] = nil; timers[id] = nil end
function fc4.after(sec, fn) local id = nextId; nextId = nextId + 1; timers[id] = { at = fc4.time() + sec, fn = fn }; return id end
function fc4.every(sec, fn) local id = nextId; nextId = nextId + 1; timers[id] = { at = fc4.time() + sec, fn = fn, every = sec }; return id end
local handlers = {}
function fc4.on(event, fn) handlers[event] = handlers[event] or {}; table.insert(handlers[event], fn) end
function fc4._emit(event, ...)
  for _, fn in ipairs(handlers[event] or {}) do
    local ok, err = xpcall(fn, debug.traceback, ...)
    if not ok then fc4.error("event " .. event .. ": " .. tostring(err)) end
  end
end
function fc4._tick(dt)
  for id, fn in pairs(ticks) do
    local ok, err = xpcall(fn, debug.traceback, dt)
    if not ok then fc4.error("tick disabled after error: " .. tostring(err)); ticks[id] = nil end
  end
  local now = fc4.time()
  for id, t in pairs(timers) do
    if now >= t.at then
      if t.every then t.at = now + t.every else timers[id] = nil end
      local ok, err = xpcall(t.fn, debug.traceback)
      if not ok then fc4.error("timer error: " .. tostring(err)); timers[id] = nil end
    end
  end
end
function fc4._press(id)
  local fn = fc4._buttons()[id]
  if fn then local ok, err = xpcall(fn, debug.traceback) if not ok then fc4.error(tostring(err)) end end
end
-- fc4.ui: your own windows in FC4Hook's overlay (see lua\\README.txt)
do
  local U = fc4.ui
  local cbs, kinds = {}, {}
  local Win = {}; Win.__index = Win
  local Wid = {}; Wid.__index = Wid
  function U.window(title, opts)
    opts = opts or {}
    return setmetatable({ id = U._window(tostring(title), opts.overlay == true, opts.width or 0, opts.height or 0) }, Win)
  end
  local function add(w, kind, label, a, b, c, fn)
    local id = U._add(w.id, kind, label ~= nil and tostring(label) or "", a, b, c, w._same == true)
    w._same = nil
    cbs[id] = fn; kinds[id] = kind
    return setmetatable({ id = id }, Wid)
  end
  function Win:text(s) return add(self, "text", s) end
  function Win:button(label, fn) return add(self, "button", label, 0, 0, 0, fn) end
  function Win:checkbox(label, v, fn) return add(self, "checkbox", label, v and 1 or 0, 0, 0, fn) end
  function Win:slider(label, lo, hi, v, fn) return add(self, "slider", label, lo, hi, v or lo, fn) end
  function Win:input(label, s, fn) local w = add(self, "input", label, 0, 0, 0, fn); U._set(w.id, s or ""); return w end
  function Win:combo(label, items, idx, fn)
    local w = add(self, "combo", label, idx or 1, 0, 0, fn)
    local t = {} for i, v in ipairs(items or {}) do t[i] = tostring(v) end
    U._items(w.id, table.concat(t, "\n"))
    return w
  end
  function Win:progress(v, label) return add(self, "progress", label, v or 0) end
  function Win:separator(label) return add(self, "separator", label) end
  function Win:same_line() self._same = true return self end
  function Win:show(v) U._show(self.id, v ~= false) end
  function Win:hide() U._show(self.id, false) end
  function Win:close() U._remove(self.id) end
  function Wid:set(v) U._set(self.id, v) end
  function Wid:get() return U._get(self.id) end
  function Wid:on(fn) cbs[self.id] = fn end
  function U._event(id, v)
    local fn = cbs[id]
    if not fn then return end
    if kinds[id] == "checkbox" then v = v ~= 0 elseif kinds[id] == "combo" then v = math.tointeger(v) or v
    elseif kinds[id] == "button" then v = nil end
    local ok, err = xpcall(fn, debug.traceback, v)
    if not ok then fc4.error("ui callback: " .. tostring(err)) end
  end
end
-- typed memory helpers: mem.u32(addr), mem.f32(addr) ... and mem.read(addr, "u32")
function mem.read(a, t) return mem[t or "u32"](a) end
function mem.write(a, t, v) return mem["set_" .. (t or "u32")](a, v) end
-- vectors from the game come back as {x, y, z} tables
-- (the game registers GetEntityPositionVec3 in another Lua state; FC4Hook exposes it as ModGetEntityPositionVec3)
function game.pos(id)
  local f = game.has("GetEntityPositionVec3") and "GetEntityPositionVec3" or "ModGetEntityPositionVec3"
  local p = game.call(f, id or game.player())
  if type(p) ~= "table" then return nil end
  return p[1], p[2], p[3]
end
)LUA";

static void SetFuncs(lua_State* L, const luaL_Reg* r) { luaL_setfuncs(L, r, 0); }

static void OpenApi(lua_State* L) {
    luaL_openlibs(L);
    // package.path -> fc4hook\lua\?.lua ; fc4hook\lua\?\init.lua ; no C modules
    lua_getglobal(L, "package");
    std::string root = Narrow(g_rootDir);
    lua_pushstring(L, (root + "lua\\?.lua;" + root + "lua\\?\\init.lua").c_str()); lua_setfield(L, -2, "path");
    lua_pushstring(L, ""); lua_setfield(L, -2, "cpath");
    lua_pop(L, 1);

    static const luaL_Reg fc4f[] = {
        { "log", f_log }, { "debug", f_debug }, { "warn", f_warn }, { "error", f_error }, { "time", f_time },
        { "frame", f_frame }, { "version", f_version }, { "key_down", f_key_down }, { "set_state", f_state_set },
        { "get_state", f_state_get }, { "read_file", f_read_file }, { "write_file", f_write_file },
        { "file_id", f_file_id }, { "file_log", f_file_log }, { "loose_status", f_loose_status }, { nullptr, nullptr } };
    lua_newtable(L); SetFuncs(L, fc4f);
    lua_newtable(L);
    static const luaL_Reg menuf[] = { { "button", f_menu_button }, { "clear", f_menu_clear }, { nullptr, nullptr } };
    SetFuncs(L, menuf); lua_setfield(L, -2, "menu");
    lua_pushcfunction(L, [](lua_State* L) -> int { lua_getfield(L, LUA_REGISTRYINDEX, "fc4.buttons"); return 1; });
    lua_setfield(L, -2, "_buttons");
    LuaUiOpen(L);
    lua_setglobal(L, "fc4");
    lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, "fc4.buttons");

    static const luaL_Reg gamef[] = {
        { "call", g_call }, { "pcall", g_pcall }, { "eval", g_eval }, { "has", g_has }, { "ready", g_ready },
        { "player", g_player }, { "free", g_free }, { nullptr, nullptr } };
    lua_newtable(L); SetFuncs(L, gamef);
    lua_newtable(L); lua_pushcfunction(L, GameIndex); lua_setfield(L, -2, "__index"); lua_setmetatable(L, -2);
    lua_setglobal(L, "game");

    luaL_newmetatable(L, "fc4.proxy");
    lua_pushcfunction(L, ProxyIndex); lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, ProxyCall); lua_setfield(L, -2, "__call");
    lua_pop(L, 1);
    luaL_newmetatable(L, "fc4.gameref");
    lua_pushcfunction(L, RefToString); lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);

    static const luaL_Reg memf[] = {
        { "i8", ReadT<int8_t> }, { "u8", ReadT<uint8_t> }, { "i16", ReadT<int16_t> }, { "u16", ReadT<uint16_t> },
        { "i32", ReadT<int32_t> }, { "u32", ReadT<uint32_t> }, { "i64", ReadT<int64_t> }, { "u64", ReadT<uint64_t> },
        { "f32", ReadT<float> }, { "f64", ReadT<double> }, { "ptr", ReadT<uint64_t> },
        { "set_i8", WriteT<int8_t> }, { "set_u8", WriteT<uint8_t> }, { "set_i16", WriteT<int16_t> }, { "set_u16", WriteT<uint16_t> },
        { "set_i32", WriteT<int32_t> }, { "set_u32", WriteT<uint32_t> }, { "set_i64", WriteT<int64_t> }, { "set_u64", WriteT<uint64_t> },
        { "set_f32", WriteT<float> }, { "set_f64", WriteT<double> }, { "set_ptr", WriteT<uint64_t> },
        { "addr", m_addr }, { "hex", m_hex }, { "readable", m_readable }, { "writable", m_writable }, { "module", m_module },
        { "scan", m_scan }, { "rip", m_rip }, { "chain", m_chain }, { "str", m_str }, { "bytes", m_bytes },
        { "write_bytes", m_write_bytes }, { "alloc", m_alloc }, { "call", m_call }, { "entity", m_entity },
        { "player", m_player }, { "native", m_native }, { "vtable", m_vtable }, { nullptr, nullptr } };
    lua_newtable(L); SetFuncs(L, memf); lua_setglobal(L, "mem");

    if (luaL_loadbuffer(L, kPrelude, strlen(kPrelude), "=fc4.prelude") != LUA_OK || !SafeCall(L, 0, 0, "prelude")) {
        if (lua_gettop(L)) { LogL(LOG_ERROR, "[lua54] prelude: %s", lua_tostring(L, -1)); lua_settop(L, 0); }
    }
}

static void RunFile(const std::wstring& path, const std::string& chunk) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return;
    std::string code; char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) code.append(buf, n);
    fclose(f);
    if (luaL_loadbuffer(S, code.data(), code.size(), ("@" + chunk).c_str()) != LUA_OK) {
        LogL(LOG_ERROR, "[lua54] %s", lua_tostring(S, -1)); lua_pop(S, 1); return;
    }
    SafeCall(S, 0, 0, chunk.c_str());
}

static void StartState() {
    if (S) {
        { std::lock_guard<std::mutex> lk(s_jobMx); s_buttons.clear(); }
        LuaUiReset();
        lua_close(S);
        S = nullptr;
    }
    S = luaL_newstate();
    if (!S) { s_status = "failed to start"; LogL(LOG_ERROR, "[lua54] luaL_newstate failed"); return; }
    OpenApi(S);
    s_status = "running";
    CreateDirectoryW((g_rootDir + L"lua").c_str(), nullptr);
    CreateDirectoryW((g_rootDir + L"lua\\autorun").c_str(), nullptr);
    std::wstring dir = g_rootDir + L"lua\\autorun\\";
    WIN32_FIND_DATAW fd;
    std::vector<std::wstring> files;
    HANDLE h = FindFirstFileW((dir + L"*.lua").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) { do { files.push_back(fd.cFileName); } while (FindNextFileW(h, &fd)); FindClose(h); }
    std::sort(files.begin(), files.end());
    for (auto& f : files) { LogL(LOG_DEBUG, "[lua54] autorun %s", Narrow(f).c_str()); RunFile(dir + f, "lua/autorun/" + Narrow(f)); }
    Log("Lua 5.4 runtime started (%s, %d autorun scripts)", LUA_RELEASE, (int)files.size());
}

static void Emit(const char* event) {
    if (!S) return;
    lua_getglobal(S, "fc4");
    lua_getfield(S, -1, "_emit");
    lua_remove(S, -2);
    lua_pushstring(S, event);
    SafeCall(S, 1, 0, event);
}

void Lua54OnGameState(lua_State* gameL) {
    s_game = gameL;
    if (S && !s_wantReload) Emit("game_lua_reset");
}

void Lua54Tick(lua_State* gameL) {
    s_game = gameL;
    ULONGLONG now = GetTickCount64();
    double dt = s_lastTick ? (now - s_lastTick) / 1000.0 : 0.0;
    s_lastTick = now;
    s_time += dt;
    if (s_wantReload) { s_wantReload = false; StartState(); Emit("start"); }
    if (!S) return;
    lua_getglobal(S, "fc4");
    lua_getfield(S, -1, "_tick");
    lua_remove(S, -2);
    lua_pushnumber(S, dt);
    SafeCall(S, 1, 0, "tick");
    if (lua_gettop(S) > 0) lua_settop(S, 0);
}

void Lua54RunJobs(lua_State* gameL) {
    s_game = gameL;
    if (!S) return;
    std::vector<Job> jobs; std::vector<int> pressed;
    { std::lock_guard<std::mutex> lk(s_jobMx); jobs.swap(s_jobs); pressed.swap(s_pressed); }
    for (auto& j : jobs) {
        // "=expr" prints the value(s)
        std::string code = j.code;
        if (!code.empty() && code[0] == '=') code = "print(" + code.substr(1) + ")";
        if (luaL_loadbuffer(S, code.data(), code.size(), j.chunk.c_str()) != LUA_OK) {
            LogL(LOG_ERROR, "[lua54] %s", lua_tostring(S, -1)); lua_pop(S, 1); continue;
        }
        SafeCall(S, 0, 0, j.chunk.c_str());
    }
    for (auto& e : LuaUiTakeEvents()) {
        lua_getglobal(S, "fc4");
        lua_getfield(S, -1, "ui");
        lua_getfield(S, -1, "_event");
        lua_remove(S, -2); lua_remove(S, -2);
        if (!lua_isfunction(S, -1)) { lua_pop(S, 1); break; }
        lua_pushinteger(S, e.id);
        if (e.isText) lua_pushstring(S, e.text.c_str()); else lua_pushnumber(S, e.num);
        SafeCall(S, 2, 0, "ui");
    }
    for (int id : pressed) {
        lua_getglobal(S, "fc4"); lua_getfield(S, -1, "_press"); lua_remove(S, -2);
        lua_pushinteger(S, id);
        SafeCall(S, 1, 0, "menu button");
    }
}

} // namespace fc4
