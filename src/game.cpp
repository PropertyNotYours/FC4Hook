// Game bridge: resolves FC64.dll's embedded Lua 5.1 API by signature and runs our
// work on the game's script thread (Lua count/call hook + message-pump tick).
//
// FC64.dll's code pages refuse VirtualProtect, so there are no inline hooks in it.
// Reference addresses are from the FC64.dll in the Ghidra project (base 0x180000000);
// the installed Steam build differs slightly, signatures handle that.
#include "core.h"
#include <mutex>
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <algorithm>

namespace fc4 {

// ------------------------------------------------------------ types ------
typedef struct lua_Debug lua_Debug;
typedef void        (*lua_Hook)(lua_State*, lua_Debug*);
typedef void        (*lua_pushcclosure_t)(lua_State*, lua_CFunction, int);
typedef void        (*lua_setfield_t)(lua_State*, int, const char*);
typedef void        (*lua_getfield_t)(lua_State*, int, const char*);
typedef const char* (*lua_tolstring_t)(lua_State*, int, size_t*);
typedef int         (*luaL_loadbuffer_t)(lua_State*, const char*, size_t, const char*);
typedef int         (*lua_pcall_t)(lua_State*, int, int, int);

static lua_pushcclosure_t p_pushcclosure = nullptr;
static lua_setfield_t     p_setfield     = nullptr;
static lua_getfield_t     p_getfield     = nullptr;
static lua_tolstring_t    p_tolstring    = nullptr;
static luaL_loadbuffer_t  p_loadbuffer   = nullptr;
static lua_pcall_t        p_pcall        = nullptr;
static void**             p_ppScriptMgr  = nullptr;   // &g_pScriptManager
static void*              p_mgrPCall     = nullptr;   // located for reference only (not hooked)

struct Sig { const char* name; const char* pattern; void** out; int ripOff; int ripLen; bool required; };

static Sig s_sigs[] = {
    { "g_pScriptManager",
      "48 8B 05 ?? ?? ?? ?? 48 8B 40 08 C3 CC CC CC CC 48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B F2 41 8B F8",
      (void**)&p_ppScriptMgr, 3, 7, true },
    { "ScriptMgr_PCallWithHandler",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B F2 41 8B F8 BB FE FF FF FF 41 2B D8 44 8B 41 14 BA F0",
      (void**)&p_mgrPCall, 0, 0, false },
    { "luaL_loadbuffer",
      "4C 8B DC 49 89 5B 08 49 89 73 10 57 48 81 EC 90 00 00 00 48 8B D9 49 89 4B F0 4D 85 C9 49 89 53 98 48 8D 0D",
      (void**)&p_loadbuffer, 0, 0, true },
    { "lua_pcall",
      "48 89 5C 24 08 57 48 83 EC 40 41 8B F8 44 8B D2 48 8B D9 45 85 C9 75 04 33 C0 EB 0C",
      (void**)&p_pcall, 0, 0, true },
    { "lua_pushcclosure",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 4C 8B 49 20 49 63 F8 48 8B F2 49 8B 41 70 48 8B D9 49 39 41 78 72 05 E8",
      (void**)&p_pushcclosure, 0, 0, true },
    { "lua_setfield",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 4D 8B D0 48 8B F1 E8 ?? ?? ?? ?? 48 83 C9 FF 48 8B D8 49 8B FA 33 C0 49 8B D2 F2 AE 48 F7 D1 4C 8D 41 FF 48 8B CE E8 ?? ?? ?? ?? 4C 8B 4E 10 4C 8D 44 24 20 49 83 E9 10",
      (void**)&p_setfield, 0, 0, true },
    { "lua_getfield",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 4D 8B D0 48 8B F1 E8 ?? ?? ?? ?? 48 83 C9 FF 48 8B D8 49 8B FA 33 C0 49 8B D2 F2 AE 48 F7 D1 4C 8D 41 FF 48 8B CE E8 ?? ?? ?? ?? 4C 8B 4E 10 4C 8D 44 24 20 48 8B D3",
      (void**)&p_getfield, 0, 0, true },
    { "lua_tolstring",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 49 8B D8 8B F2 48 8B F9 E8 ?? ?? ?? ?? 4C 8B D0 83 78 08 04 74",
      (void**)&p_tolstring, 0, 0, true },
};

// ------------------------------------------------- lua_State layout -------
// Stock Lua 5.1 x64:
//   +0x08 tt(=8 thread)  +0x10 top  +0x18 base  +0x20 l_G  +0x28 ci  +0x40 stack
//   +0x50 base_ci  +0x5c size_ci  +0x60 nCcalls  +0x64 hookmask  +0x65 allowhook
//   +0x68 basehookcount  +0x6c hookcount  +0x70 hook  +0x78 l_gt  +0xb0 errfunc
struct TValue { void* value; int tt; int pad; };
static inline TValue*&  Top(lua_State* L)       { return *(TValue**)((char*)L + 0x10); }
static inline TValue*   Base(lua_State* L)      { return *(TValue**)((char*)L + 0x18); }
static inline void*     GState(lua_State* L)    { return *(void**)((char*)L + 0x20); }
static inline uint8_t&  ObjType(lua_State* L)   { return *(uint8_t*)((char*)L + 0x08); }
static inline uint8_t&  HookMask(lua_State* L)  { return *(uint8_t*)((char*)L + 0x64); }
static inline int&      BaseHookCount(lua_State* L) { return *(int*)((char*)L + 0x68); }
static inline int&      HookCount(lua_State* L) { return *(int*)((char*)L + 0x6c); }
static inline lua_Hook& HookFn(lua_State* L)    { return *(lua_Hook*)((char*)L + 0x70); }
static inline void*     CI(lua_State* L)        { return *(void**)((char*)L + 0x28); }
static inline void*     BaseCI(lua_State* L)    { return *(void**)((char*)L + 0x50); }
static inline uint16_t  NCcalls(lua_State* L)   { return *(uint16_t*)((char*)L + 0x60); }

enum { LUA_TTHREAD = 8, LUA_MASKCALL = 1, LUA_MASKCOUNT = 8 };
static const uint8_t kOurMask   = LUA_MASKCALL | LUA_MASKCOUNT;
static const int     kHookEvery = 200;

static int my_gettop(lua_State* L) { return (int)(Top(L) - Base(L)); }
static void my_settop(lua_State* L, int idx) {
    if (idx >= 0) {
        TValue* want = Base(L) + idx;
        while (Top(L) < want) { Top(L)->tt = 0; ++Top(L); }
        Top(L) = want;
    } else {
        Top(L) += idx + 1;
    }
}
static int TypeAtTop(lua_State* L) { return (Top(L) - 1)->tt; }   // 0 nil, 4 string, 5 table, 6 function

static lua_State* GetLuaState() {
    if (!p_ppScriptMgr || !*p_ppScriptMgr) return nullptr;
    return *(lua_State**)((char*)*p_ppScriptMgr + 8);
}

// ------------------------------------------------------- state ------------
struct LuaJob { std::string code, chunk; };
static std::mutex                                 s_qMx;
static std::vector<LuaJob>                        s_jobs;
static std::vector<std::pair<std::string, lua_CFunction>> s_funcs;
static size_t                                     s_funcsApplied = 0;
static std::vector<void (*)(lua_State*)>          s_tickCbs;
static std::atomic<bool>                          s_reloadAutorun{ true };
static std::atomic<bool>                          s_luaReady{ false };
static std::atomic<bool>                          s_pending{ true };
static std::atomic<void*>                         s_boundG{ nullptr };
static std::atomic<uint64_t>                      s_hookCalls{ 0 };
static bool                                       s_ready = false;
static const char*                                s_status = "starting";
static lua_Hook                                   s_prevHook = nullptr;

void QueueLua(const std::string& code, const std::string& chunk) {
    if (!s_luaReady) {
        LogL(LOG_WARN, "Lua isn't live yet (%s); ignored: %.60s", s_status, code.c_str());
        return;
    }
    std::lock_guard<std::mutex> lk(s_qMx);
    s_jobs.push_back({ code, chunk.empty() ? std::string("=fc4hook") : chunk });
    s_pending = true;
}
void RegisterLuaFunction(const char* name, lua_CFunction fn) {
    std::lock_guard<std::mutex> lk(s_qMx);
    s_funcs.emplace_back(name, fn);
    s_pending = true;
}
void AddTickCallback(void (*cb)(lua_State*)) {
    std::lock_guard<std::mutex> lk(s_qMx);
    s_tickCbs.push_back(cb);
}
void RequestAutorunReload() { s_reloadAutorun = true; s_pending = true; }
bool GameReady() { return s_ready; }
bool LuaReady()  { return s_luaReady; }
const char* BridgeStatus() { return s_luaReady ? "live" : s_status; }
uint64_t LuaHookCalls() { return s_hookCalls.load(); }

static bool RunChunk(lua_State* L, const std::string& code, const std::string& name) {
    int saved = my_gettop(L);
    int st = p_loadbuffer(L, code.data(), code.size(), name.c_str());
    if (st == 0) st = p_pcall(L, 0, 0, 0);
    if (st != 0) {
        const char* msg = my_gettop(L) > saved ? p_tolstring(L, -1, nullptr) : nullptr;
        LogL(LOG_ERROR, "[lua] error in %s: %s", name.c_str(), msg ? msg : "(no message)");
    }
    my_settop(L, saved);
    return st == 0;
}

// ------------------------------------------------------------ helpers -----
static bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

static uintptr_t FC64Base() { return (uintptr_t)GetModuleHandleA("FC64.dll"); }

// Leaves the result of `return <expr>` on the stack (no lua_pushstring available).
static int ReturnLua(lua_State* L, const std::string& expr) {
    std::string code = "return " + expr;
    int top = my_gettop(L);
    int st = p_loadbuffer(L, code.data(), code.size(), "=fc4hook");
    if (st == 0) st = p_pcall(L, 0, 1, 0);
    if (st != 0) {
        const char* msg = my_gettop(L) > top ? p_tolstring(L, -1, nullptr) : nullptr;
        LogL(LOG_WARN, "ReturnLua failed (%d bytes): %s", (int)code.size(), msg ? msg : "?");
        my_settop(L, top);
        return 0;
    }
    return 1;
}

static std::string Quote(const std::string& s) {
    std::string r = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { r += '\\'; r += c; }
        else if (c == '\n') r += "\\n";
        else if (c == '\r') r += "\\r";
        else if (c == 0) r += "\\0";
        else r += c;
    }
    return r + "\"";
}

static const char* ArgStr(lua_State* L, int i) {
    return my_gettop(L) >= i ? p_tolstring(L, i, nullptr) : nullptr;
}

// Every game native is a Lua 5.1 CClosure; its C function pointer sits at +0x20.
static void* NativeCFunction(lua_State* L, const char* dotted) {
    std::string s = dotted;
    size_t dot = s.find('.');
    int top = my_gettop(L);
    if (dot == std::string::npos) {
        p_getfield(L, FC4_LUA_GLOBALSINDEX, s.c_str());
    } else {
        p_getfield(L, FC4_LUA_GLOBALSINDEX, s.substr(0, dot).c_str());
        if (TypeAtTop(L) != 5) { my_settop(L, top); return nullptr; }
        p_getfield(L, -1, s.substr(dot + 1).c_str());
    }
    void* fn = nullptr;
    TValue* tv = Top(L) - 1;
    if (tv->tt == 6) {
        uint8_t* cl = (uint8_t*)tv->value;
        if (Readable(cl, 0x28) && cl[10] /*isC*/) fn = *(void**)(cl + 0x20);
    }
    my_settop(L, top);
    return fn;
}

static uint8_t* CallTarget(uint8_t* at) { return at + 5 + *(int32_t*)(at + 1); }

// --------------------------------------------------- built-in natives -----
static std::string JoinArgs(lua_State* L) {
    std::string s;
    int n = my_gettop(L);
    for (int i = 1; i <= n; ++i) {
        TValue* tv = Base(L) + (i - 1);
        const char* p = nullptr;
        char tmp[48];
        if (tv->tt == 3 || tv->tt == 4) p = p_tolstring(L, i, nullptr);       // number / string
        else if (tv->tt == 0) p = "nil";
        else if (tv->tt == 1) p = *(int*)&tv->value ? "true" : "false";
        else { snprintf(tmp, sizeof(tmp), "<%s>", tv->tt == 5 ? "table" : tv->tt == 6 ? "function" : "userdata"); p = tmp; }
        if (i > 1) s += '\t';
        s += p ? p : "nil";
    }
    return s;
}
static int L_ModLog(lua_State* L)      { LogL(LOG_INFO,  "[lua] %s", JoinArgs(L).c_str()); return 0; }
static int L_ModLogDebug(lua_State* L) { LogL(LOG_DEBUG, "[lua] %s", JoinArgs(L).c_str()); return 0; }
static int L_ModLogWarn(lua_State* L)  { LogL(LOG_WARN,  "[lua] %s", JoinArgs(L).c_str()); return 0; }
static int L_ModLogError(lua_State* L) { LogL(LOG_ERROR, "[lua] %s", JoinArgs(L).c_str()); return 0; }

// ------------------------------------------------------------ state ------
static std::mutex s_stateMx;
static std::vector<std::pair<std::string, std::string>> s_state;
static std::atomic<uint64_t> s_stateSerial{ 0 };
void SetState(const std::string& k, const std::string& v) {
    std::lock_guard<std::mutex> lk(s_stateMx);
    for (auto& e : s_state) if (e.first == k) { if (e.second != v) { e.second = v; ++s_stateSerial; } return; }
    s_state.emplace_back(k, v); ++s_stateSerial;
}
std::string GetState(const std::string& k, const std::string& def) {
    std::lock_guard<std::mutex> lk(s_stateMx);
    for (auto& e : s_state) if (e.first == k) return e.second;
    return def;
}
uint64_t StateSerial() { return s_stateSerial.load(); }

// ModSetState(key, value) - publish a value for the menu (e.g. cheat toggles)
static int L_ModSetState(lua_State* L) {
    const char* k = ArgStr(L, 1);
    if (!k) return 0;
    std::string v;
    if (my_gettop(L) >= 2) {
        TValue* tv = Base(L) + 1;
        if (tv->tt == 1) v = *(int*)&tv->value ? "1" : "0";
        else if (tv->tt == 3 || tv->tt == 4) { const char* p = p_tolstring(L, 2, nullptr); v = p ? p : ""; }
    }
    SetState(k, v);
    return 0;
}
// ModGetState(key) -> string or nil
static int L_ModGetState(lua_State* L) {
    const char* k = ArgStr(L, 1);
    if (!k) return 0;
    std::string v = GetState(k, "\x01");
    if (v == "\x01") return 0;
    return ReturnLua(L, Quote(v));
}
// ModVersion() -> "1.3.0"
static int L_ModVersion(lua_State* L) { return ReturnLua(L, Quote(FC4HOOK_VERSION)); }
// ModFileExists(relPath) -> true/nil   (under fc4hook\)
static int L_ModFileExists(lua_State* L);

static bool SafeRelPath(const char* rel) { return rel && !strstr(rel, "..") && !strchr(rel, ':'); }

static std::wstring ModPath(const char* rel) {
    std::wstring path = g_rootDir + Widen(rel);
    for (auto& c : path) if (c == L'/') c = L'\\';
    return path;
}

static int L_ModFileExists(lua_State* L) {
    const char* rel = ArgStr(L, 1);
    if (!SafeRelPath(rel) || GetFileAttributesW(ModPath(rel).c_str()) == INVALID_FILE_ATTRIBUTES) return 0;
    return ReturnLua(L, "true");
}

// ModWriteFile(relPath, text [, append])  -> writes under fc4hook\  (no "..")
static int L_ModWriteFile(lua_State* L) {
    int n = my_gettop(L);
    const char* rel  = n >= 1 ? p_tolstring(L, 1, nullptr) : nullptr;
    size_t len = 0;
    const char* text = n >= 2 ? p_tolstring(L, 2, &len) : nullptr;
    bool append = n >= 3 && (Base(L) + 2)->tt == 1 && *(int*)&(Base(L) + 2)->value;
    if (!SafeRelPath(rel) || !text) { LogL(LOG_WARN, "[lua] ModWriteFile: bad args"); return 0; }
    std::wstring path = ModPath(rel);
    for (size_t i = g_rootDir.size(); i < path.size(); ++i)
        if (path[i] == L'\\') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
    FILE* f = _wfopen(path.c_str(), append ? L"ab" : L"wb");
    if (!f) { LogL(LOG_WARN, "[lua] ModWriteFile: cannot open %s", rel); return 0; }
    fwrite(text, 1, len, f);
    fclose(f);
    return 0;
}

// ModReadFile(relPath) -> contents or nil
// ModCopyArchiveRange("data_win32\\patch.dat", "offset", "size", "dumps\\x.bin") -> "ok" or error.
// Source is relative to the game folder (read-only), destination relative to fc4hook\.
// Offsets/sizes are strings because game Lua numbers are single-precision floats.
static int L_ModCopyArchiveRange(lua_State* L) {
    const char* src = ArgStr(L, 1); const char* so = ArgStr(L, 2);
    const char* ss = ArgStr(L, 3);  const char* dst = ArgStr(L, 4);
    if (!SafeRelPath(src) || !SafeRelPath(dst) || !so || !ss) return ReturnLua(L, Quote("bad args"));
    long long off = _strtoi64(so, nullptr, 10), size = _strtoi64(ss, nullptr, 10);
    if (off < 0 || size <= 0 || size > (512ll << 20)) return ReturnLua(L, Quote("bad range"));
    std::wstring game = g_rootDir + L"..\\..\\" + Widen(src);
    FILE* in = _wfopen(game.c_str(), L"rb");
    if (!in) return ReturnLua(L, Quote("cannot open source"));
    std::wstring path = ModPath(dst);
    for (size_t i = g_rootDir.size(); i < path.size(); ++i)
        if (path[i] == L'\\') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
    FILE* out = _wfopen(path.c_str(), L"wb");
    if (!out) { fclose(in); return ReturnLua(L, Quote("cannot open destination")); }
    std::string err = "ok";
    if (_fseeki64(in, off, SEEK_SET) != 0) err = "seek failed";
    std::vector<char> buf(1 << 20);
    while (err == "ok" && size > 0) {
        size_t want = (size_t)std::min<long long>(size, (long long)buf.size());
        size_t got = fread(buf.data(), 1, want, in);
        if (got == 0) { err = "short read"; break; }
        fwrite(buf.data(), 1, got, out);
        size -= (long long)got;
    }
    fclose(in); fclose(out);
    return ReturnLua(L, Quote(err));
}

// ModDumpStrings("dumps\\fc64_strings.txt") -> count. Unique printable ASCII strings (len>=3) from
// FC64.dll's non-code sections, read from memory (the file on disk is encrypted).
static int L_ModDumpStrings(lua_State* L) {
    const char* dst = ArgStr(L, 1);
    if (!SafeRelPath(dst)) return 0;
    uintptr_t base = FC64Base();
    if (!base) return 0;
    auto* nt = (IMAGE_NT_HEADERS64*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    std::wstring path = ModPath(dst);
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return 0;
    std::vector<std::string> all;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        const uint8_t* s = (const uint8_t*)(base + sec[i].VirtualAddress);
        size_t n = sec[i].Misc.VirtualSize;
        if (!Readable(s, n)) continue;
        size_t start = 0;
        for (size_t j = 0; j < n; ++j) {
            uint8_t c = s[j];
            if (c >= 0x20 && c < 0x7f) continue;
            if (c == 0 && j - start >= 3 && j - start < 400) all.emplace_back((const char*)s + start, j - start);
            start = j + 1;
        }
    }
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    for (auto& s : all) { fwrite(s.data(), 1, s.size(), f); fputc('\n', f); }
    fclose(f);
    char b[32]; snprintf(b, sizeof(b), "%u", (unsigned)all.size());
    return ReturnLua(L, b);
}

static int L_ModReadFile(lua_State* L) {
    const char* rel = ArgStr(L, 1);
    if (!SafeRelPath(rel)) return 0;
    FILE* f = _wfopen(ModPath(rel).c_str(), L"rb");
    if (!f) return 0;
    std::string data; char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && data.size() < (1u << 20)) data.append(buf, n);
    fclose(f);
    return ReturnLua(L, Quote(data));
}

// ModNativeAddress("IsRadioTowerUnlocked") -> "FC64+0x18bad60" (or nil)
static int L_ModNativeAddress(lua_State* L) {
    const char* name = ArgStr(L, 1);
    if (!name) return 0;
    void* fn = NativeCFunction(L, name);
    if (!fn) return 0;
    char buf[64];
    snprintf(buf, sizeof(buf), "FC64+0x%llx", (unsigned long long)((uintptr_t)fn - FC64Base()));
    return ReturnLua(L, Quote(buf));
}

// ======================================================== towers ==========
// Open-world tower manager: Lua_IsRadioTowerUnlocked calls a thunk
//   sub rsp,28 / mov rdx,rcx / mov rcx,[g_mgr] / call TowerMgr_IsTowerUnlocked
static void** FindTowerManagerGlobal(lua_State* L) {
    static void** cached = nullptr;
    if (cached) return cached;
    uint8_t* fn = (uint8_t*)NativeCFunction(L, "IsRadioTowerUnlocked");
    if (!fn || !Readable(fn, 0x100)) return nullptr;
    for (int i = 0; i < 0xF0; ++i) {
        if (fn[i] != 0xE8) continue;
        uint8_t* t = CallTarget(fn + i);
        if (!Readable(t, 16)) continue;
        if (t[0] == 0x48 && t[1] == 0x83 && t[2] == 0xEC && t[4] == 0x48 && t[5] == 0x8B && t[6] == 0xD1 &&
            t[7] == 0x48 && t[8] == 0x8B && t[9] == 0x0D) {
            cached = (void**)(t + 14 + *(int32_t*)(t + 10));
            LogL(LOG_DEBUG, "tower manager global found at FC64+0x%llx", (unsigned long long)((uintptr_t)cached - FC64Base()));
            return cached;
        }
    }
    LogL(LOG_WARN, "tower manager global: pattern not found");
    return nullptr;
}

static uint8_t* TowerManager(lua_State* L) {
    void** g = FindTowerManagerGlobal(L);
    if (!g || !Readable(g, 8) || !*g) return nullptr;
    return (uint8_t*)*g;
}

// ModGetUnlockedTowers([offset]) -> "id:key;id:key;..."
// Manager's unlocked-tower vector: elements of 0x18 bytes at +0x2F8, count at +0x300.
static int L_ModGetUnlockedTowers(lua_State* L) {
    uint32_t off = 0x2F8;
    if (const char* s = ArgStr(L, 1)) off = (uint32_t)strtoul(s, nullptr, 0);
    uint8_t* mgr = TowerManager(L);
    if (!mgr) { LogL(LOG_WARN, "towers: manager not available"); return 0; }
    if (!Readable(mgr + off, 0x10)) { LogL(LOG_WARN, "towers: manager %p +0x%x not readable", mgr, off); return 0; }
    uint8_t* arr = *(uint8_t**)(mgr + off);
    uint32_t n = *(uint32_t*)(mgr + off + 8);
    LogL(LOG_DEBUG, "towers: manager=%p vec@+0x%x data=%p count=%u", mgr, off, arr, n);
    if (n > 4096) { LogL(LOG_WARN, "towers: count looks wrong, layout probably moved"); return 0; }
    if (n && !Readable(arr, (size_t)n * 0x18)) { LogL(LOG_WARN, "towers: element array not readable"); return 0; }
    std::string t;
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t* e = arr + i * 0x18;
        uint64_t* idp = *(uint64_t**)e;
        uint64_t id = Readable(idp, 8) ? *idp : ~0ull;
        uint64_t key = *(uint64_t*)(e + 8);
        char row[64];
        snprintf(row, sizeof(row), "%llu:%llu;", (unsigned long long)id, (unsigned long long)key);
        t += row;
    }
    return ReturnLua(L, Quote(t));
}

// --- helpers found by walking RevealWorldMap -> RevealWorldMap_Impl --------
typedef uint8_t* (*EntityLookup_t)(uint64_t id);            // FUN_180091190 -> refcounted handle
typedef uint8_t* (*CompType_t)();                           // FUN_180e43be0
typedef void*    (*GetComponent_t)(void* entity, void* key); // FUN_180724580
typedef void     (*HandleRelease_t)(void* handle);          // FUN_180091160
typedef void     (*TowerRelock_t)(void* comp);              // RadioTower_Relock
typedef void     (*TowerCapture_t)(void* comp, char showUi); // RadioTower_OnCaptured_GrantRewards
typedef void     (*GrantReward_t)(void* towerMgr, void** rewardRec, void** receiverHandle); // FUN_181505620

static EntityLookup_t  p_entityLookup  = nullptr;
static CompType_t      p_towerCompType = nullptr;
static GetComponent_t  p_getComponent  = nullptr;
static HandleRelease_t p_handleRelease = nullptr;
static TowerRelock_t   p_towerRelock   = nullptr;
static TowerCapture_t  p_towerCapture  = nullptr;
static GrantReward_t   p_grantReward   = nullptr;
static void**          p_ppRegistry    = nullptr;   // DAT_182e2e048: map trees of world records

static bool ResolveTowerHelpers(lua_State* L) {
    if (p_entityLookup && p_towerRelock && p_towerCapture) return true;
    uint8_t* native = (uint8_t*)NativeCFunction(L, "RevealWorldMap");
    if (!native || !Readable(native, 0x100)) { LogL(LOG_WARN, "towers: RevealWorldMap native not found"); return false; }
    uint8_t* impl = nullptr;
    static const uint8_t pro[] = { 0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x4C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0xE8 };
    for (int i = 0; i < 0xF0 && !impl; ++i) {
        if (native[i] != 0xE8) continue;
        uint8_t* t = CallTarget(native + i);
        if (Readable(t, sizeof(pro)) && memcmp(t, pro, sizeof(pro)) == 0) impl = t;
    }
    if (!impl || !Readable(impl, 0x80)) { LogL(LOG_WARN, "towers: RevealWorldMap_Impl not found"); return false; }
    uint8_t* calls[8] = {}; int nc = 0;
    for (int i = 0; i < 0x70 && nc < 8; ++i) if (impl[i] == 0xE8) { calls[nc++] = CallTarget(impl + i); i += 4; }
    if (nc < 6) { LogL(LOG_WARN, "towers: RevealWorldMap_Impl has %d calls, expected 6", nc); return false; }
    p_entityLookup  = (EntityLookup_t)calls[0];
    p_towerCompType = (CompType_t)calls[1];
    p_getComponent  = (GetComponent_t)calls[2];
    p_handleRelease = (HandleRelease_t)calls[5];

    p_towerRelock = (TowerRelock_t)FindPattern("FC64.dll",
        "40 53 55 41 54 41 55 41 56 48 83 EC 30 4C 8B F1 48 8D 51 08 33 ED 89 6C 24 60 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 26 49 8B 46 48 48 8B 0D");
    p_towerCapture = (TowerCapture_t)FindPattern("FC64.dll",
        "48 8B C4 88 50 10 48 89 48 08 55 53 57 48 8D 68 88 48 81 EC 60 01 00 00 48 89 70 E0 4C 89 60 D8 4C 89 70 C8 45 33 E4 4C 89 78 C0 0F B6 FA 45 8B F4");
    p_grantReward = (GrantReward_t)FindPattern("FC64.dll",
        "48 89 5C 24 08 57 48 83 EC 30 48 8B 02 49 8B D8 48 8B FA 48 85 C0 74 ?? 44 8B 88 98 00 00 00");

    // Registry global: first "mov rdx,[rip+X]; xor ebx,ebx; mov rax,[rdx+48]; add rdx,38" inside Relock
    if (p_towerRelock && Readable((void*)p_towerRelock, 0x200)) {
        uint8_t* f = (uint8_t*)p_towerRelock;
        static const uint8_t tail[] = { 0x33,0xDB,0x48,0x8B,0x42,0x48,0x48,0x83,0xC2,0x38 };
        for (int i = 0; i < 0x1F0 && !p_ppRegistry; ++i)
            if (f[i] == 0x48 && f[i + 1] == 0x8B && f[i + 2] == 0x15 && memcmp(f + i + 7, tail, sizeof(tail)) == 0)
                p_ppRegistry = (void**)(f + i + 7 + *(int32_t*)(f + i + 3));
    }

    uintptr_t base = FC64Base();
    auto rel = [base](void* p) { return p ? (unsigned long long)((uintptr_t)p - base) : 0ull; };
    LogL(LOG_DEBUG, "towers: lookup=+0x%llx compType=+0x%llx getComp=+0x%llx release=+0x%llx relock=+0x%llx capture=+0x%llx grant=+0x%llx registry=+0x%llx",
        rel((void*)p_entityLookup), rel((void*)p_towerCompType), rel((void*)p_getComponent), rel((void*)p_handleRelease),
        rel((void*)p_towerRelock), rel((void*)p_towerCapture), rel((void*)p_grantReward), rel((void*)p_ppRegistry));
    return p_towerRelock && p_towerCapture;
}

// Refcounted entity handle: +0 id, +8 refcount (int), +0x10 entity (null when unloaded)
struct HandleRef {
    uint8_t* h = nullptr;
    explicit HandleRef(uint64_t id) {
        h = p_entityLookup ? p_entityLookup(id) : nullptr;
        if (h && Readable(h, 0x18)) ++*(int*)(h + 8); else h = nullptr;
    }
    ~HandleRef() { if (h && --*(int*)(h + 8) == 0) p_handleRelease(h); }
    void* entity() const { return h ? *(void**)(h + 0x10) : nullptr; }
};

static uint8_t* TowerComponent(void* entity) {
    if (!entity) return nullptr;
    uint8_t* type = p_towerCompType();
    return type ? (uint8_t*)p_getComponent(entity, type + 0x18) : nullptr;
}

// std::map lower_bound over the registry's trees (header at +hdr, root at +hdr+0x10,
// node: [0]=left [8]=right +0x20=key +0x28=record).  Tree A: hdr 0x38 (towers etc),
// tree B: hdr 0x68 (reward/loot records).
static uint8_t* RegistryFind(size_t hdr, uint64_t key) {
    if (!p_ppRegistry || !Readable(p_ppRegistry, 8) || !*p_ppRegistry) return nullptr;
    uint8_t* reg = (uint8_t*)*p_ppRegistry;
    if (!Readable(reg + hdr, 0x18)) return nullptr;
    uint8_t* node = *(uint8_t**)(reg + hdr + 0x10);
    uint8_t* best = nullptr;
    for (int guard = 0; node && guard < 4096; ++guard) {
        if (!Readable(node, 0x30)) return nullptr;
        if (*(uint64_t*)(node + 0x20) < key) node = *(uint8_t**)(node + 8);
        else { best = node; node = *(uint8_t**)node; }
    }
    if (!best || key < *(uint64_t*)(best + 0x20)) return nullptr;
    uint8_t* rec = *(uint8_t**)(best + 0x28);
    return Readable(rec, 0xC0) ? rec : nullptr;
}

// ModGrantTowerRewards("<towerId>") -> "ok" or an error string (tower must be loaded)
static int L_ModGrantTowerRewards(lua_State* L) {
    const char* s = ArgStr(L, 1);
    if (!s) return ReturnLua(L, Quote("no tower id"));
    if (!ResolveTowerHelpers(L)) return ReturnLua(L, Quote("helpers not found (see log)"));
    HandleRef h(strtoull(s, nullptr, 10));
    if (!h.h) return ReturnLua(L, Quote("entity handle not found"));
    if (!h.entity()) return ReturnLua(L, Quote("tower entity not loaded"));
    uint8_t* comp = TowerComponent(h.entity());
    if (!comp) return ReturnLua(L, Quote("entity has no tower component"));
    p_towerRelock(comp);
    p_towerCapture(comp, 1);
    Log("towers: granted rewards for tower %s", s);
    return ReturnLua(L, Quote("ok"));
}

// ModTowerRewardKeys("<towerId>") -> "k1,k2,..." from the loaded tower component
// (comp+0x70: entries of 0x18 bytes, reward key at +8; count at +0x78), or nil.
static std::string RewardKeysOf(uint8_t* comp) {
    std::string out;
    if (!Readable(comp + 0x70, 0x10)) return out;
    uint8_t* arr = *(uint8_t**)(comp + 0x70);
    uint32_t n = *(uint32_t*)(comp + 0x78);
    if (n > 256 || (n && !Readable(arr, (size_t)n * 0x18))) return out;
    for (uint32_t i = 0; i < n; ++i) {
        char b[32]; snprintf(b, sizeof(b), "%s%llu", i ? "," : "", (unsigned long long)*(uint64_t*)(arr + i * 0x18 + 8));
        out += b;
    }
    return out;
}

static int L_ModTowerRewardKeys(lua_State* L) {
    const char* s = ArgStr(L, 1);
    if (!s || !ResolveTowerHelpers(L)) return 0;
    HandleRef h(strtoull(s, nullptr, 10));
    uint8_t* comp = TowerComponent(h.entity());
    if (!comp) return 0;
    return ReturnLua(L, Quote(RewardKeysOf(comp)));
}

// ModRewardState("<rewardKey>") -> "missing" | "<int state>"  (tree B record +0xb0; 0 = not granted)
static int L_ModRewardState(lua_State* L) {
    const char* s = ArgStr(L, 1);
    if (!s || !ResolveTowerHelpers(L)) return 0;
    uint8_t* rec = RegistryFind(0x68, strtoull(s, nullptr, 10));
    if (!rec) return ReturnLua(L, Quote("missing"));
    char b[32]; snprintf(b, sizeof(b), "%d", *(int*)(rec + 0xb0));
    return ReturnLua(L, Quote(b));
}

// ModGrantRewardKey("<rewardKey>", "<receiverPlayerId>") -> "ok" | "owned" | error
// Grants one tower reward straight from the global registry; no tower entity needed.
static int L_ModGrantRewardKey(lua_State* L) {
    const char* ks = ArgStr(L, 1);
    const char* ps = ArgStr(L, 2);
    if (!ks || !ps) return ReturnLua(L, Quote("bad args"));
    if (!ResolveTowerHelpers(L) || !p_grantReward || !p_ppRegistry) return ReturnLua(L, Quote("helpers not found (see log)"));
    uint8_t* mgr = TowerManager(L);
    if (!mgr) return ReturnLua(L, Quote("tower manager not available"));
    uint8_t* rec = RegistryFind(0x68, strtoull(ks, nullptr, 10));
    if (!rec) return ReturnLua(L, Quote("reward record not found"));
    if (*(int*)(rec + 0xb0) != 0) return ReturnLua(L, Quote("owned"));
    HandleRef player(strtoull(ps, nullptr, 10));
    if (!player.entity()) return ReturnLua(L, Quote("player entity not loaded"));
    // FUN_181505620 consumes one reference of each argument
    ++*(int*)(rec + 0x10);
    ++*(int*)(player.h + 8);
    void* recArg = rec; void* playerArg = player.h;
    p_grantReward(mgr, &recArg, &playerArg);
    Log("towers: granted reward key %s (state now %d)", ks, *(int*)(rec + 0xb0));
    return ReturnLua(L, Quote("ok"));
}

// ModInspectTower("<towerId>", "<towerKey>") -> writes a report to dumps\tower_inspect.txt
// Checks whether the always-loaded tower record (registry tree A, by tower key) also
// holds the reward keys, which would let a guest catch up without visiting towers.
// ------------------------------------------------------ tower travel ------
// The always-loaded tower record (registry tree A, keyed by tower key) has the
// tower key at +0x20 and the tower's world position (x, y, z floats) at +0x3c.
static std::mutex              s_travelMx;
static std::vector<TravelSpot> s_travel;

std::vector<TravelSpot> TravelList() {
    std::lock_guard<std::mutex> lk(s_travelMx);
    return s_travel;
}

// ModTowerPositions() -> "id:x,y,z;..." for every unlocked tower (also feeds the Travel tab)
static int L_ModTowerPositions(lua_State* L) {
    ResolveTowerHelpers(L);   // makes sure the registry global is resolved
    uint8_t* mgr = TowerManager(L);
    if (!mgr || !Readable(mgr + 0x2F8, 0x10)) return 0;
    uint8_t* arr = *(uint8_t**)(mgr + 0x2F8);
    uint32_t n = *(uint32_t*)(mgr + 0x300);
    if (n > 4096 || (n && !Readable(arr, (size_t)n * 0x18))) return 0;
    std::vector<TravelSpot> spots;
    std::string out;
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t* e = arr + i * 0x18;
        uint64_t* idp = *(uint64_t**)e;
        uint64_t key = *(uint64_t*)(e + 8);
        uint8_t* rec = RegistryFind(0x38, key);
        if (!rec || !Readable(idp, 8) || *(uint64_t*)(rec + 0x20) != key) continue;
        float* p = (float*)(rec + 0x3c);
        if (!(p[0] == p[0]) || p[0] < -20000 || p[0] > 20000 || p[1] < -20000 || p[1] > 20000) continue;
        if (p[0] == 0 && p[1] == 0) continue;
        TravelSpot s;
        snprintf(s.id, sizeof(s.id), "%llu", (unsigned long long)*idp);
        s.x = p[0]; s.y = p[1]; s.z = p[2];
        spots.push_back(s);
        char row[96];
        snprintf(row, sizeof(row), "%s:%.2f,%.2f,%.2f;", s.id, s.x, s.y, s.z);
        out += row;
    }
    LogL(LOG_DEBUG, "travel: %u unlocked towers, %u with positions", n, (unsigned)spots.size());
    { std::lock_guard<std::mutex> lk(s_travelMx); s_travel = spots; }
    return ReturnLua(L, Quote(out));
}

static void Hex(std::string& out, const uint8_t* p, size_t n) {
    char b[128];
    for (size_t i = 0; i < n; i += 16) {
        int k = snprintf(b, sizeof(b), "    +%03zx ", i);
        for (size_t j = 0; j < 16 && i + j < n; ++j) k += snprintf(b + k, sizeof(b) - k, "%02x ", p[i + j]);
        out += b; out += "\n";
    }
}

static int L_ModInspectTower(lua_State* L) {
    const char* is = ArgStr(L, 1);
    const char* ks = ArgStr(L, 2);
    if (!is || !ks || !ResolveTowerHelpers(L)) return 0;
    uint64_t towerId = strtoull(is, nullptr, 10), towerKey = strtoull(ks, nullptr, 10);
    std::string r;
    char b[256];
    snprintf(b, sizeof(b), "tower id=%llu key=%llu\n", (unsigned long long)towerId, (unsigned long long)towerKey); r += b;

    std::vector<uint64_t> keys;
    HandleRef h(towerId);
    uint8_t* comp = TowerComponent(h.entity());
    if (comp) {
        snprintf(b, sizeof(b), "component=%p  +0x48 key=%llu\n", comp, (unsigned long long)*(uint64_t*)(comp + 0x48)); r += b;
        std::string list = RewardKeysOf(comp);
        r += "  reward keys (comp+0x70): " + list + "\n";
        for (const char* p = list.c_str(); *p;) { keys.push_back(strtoull(p, (char**)&p, 10)); if (*p == ',') ++p; }
        uint8_t* recv = *(uint8_t**)(comp + 0x130);
        if (Readable(recv, 0x18)) { snprintf(b, sizeof(b), "  comp+0x130 handle id=%llu entity=%p\n", (unsigned long long)*(uint64_t*)recv, *(void**)(recv + 0x10)); r += b; }
        for (uint64_t k : keys) {
            uint8_t* a = RegistryFind(0x38, k); uint8_t* bb = RegistryFind(0x68, k);
            snprintf(b, sizeof(b), "  key %llu: treeA=%p (+a0=%d) treeB=%p (+b0=%d +98=%d)\n", (unsigned long long)k,
                     a, a ? a[0xa0] : -1, bb, bb ? *(int*)(bb + 0xb0) : -1, bb ? *(int*)(bb + 0x98) : -1); r += b;
        }
    } else {
        r += "tower entity not loaded (stand near it for the reward-key comparison)\n";
    }

    uint8_t* rec = RegistryFind(0x38, towerKey);
    snprintf(b, sizeof(b), "tower record (tree A)=%p  +b0=%d\n", rec, rec ? *(int*)(rec + 0xb0) : -1); r += b;
    if (rec) {
        Hex(r, rec, 0x200);
        // Search the record and one level of pointers for the reward keys
        for (size_t off = 0; off + 8 <= 0x200; off += 8) {
            uint64_t v = *(uint64_t*)(rec + off);
            for (uint64_t k : keys) if (v == k) { snprintf(b, sizeof(b), "  HIT key %llu at record+0x%zx\n", (unsigned long long)k, off); r += b; }
            uint8_t* p = (uint8_t*)v;
            if (!keys.empty() && Readable(p, 0x400)) {
                for (size_t o2 = 0; o2 + 8 <= 0x400; o2 += 8)
                    for (uint64_t k : keys) if (*(uint64_t*)(p + o2) == k) {
                        snprintf(b, sizeof(b), "  HIT key %llu at *(record+0x%zx)+0x%zx\n", (unsigned long long)k, off, o2); r += b;
                    }
            }
        }
    }
    std::wstring path = ModPath("dumps/tower_inspect.txt");
    if (FILE* f = _wfopen(path.c_str(), L"ab")) { fwrite(r.data(), 1, r.size(), f); fputs("\n", f); fclose(f); }
    Log("towers: inspect report for %s written to dumps\\tower_inspect.txt (%d reward keys)", is, (int)keys.size());
    return ReturnLua(L, Quote(comp ? "loaded" : "not loaded"));
}

// --------------------------------------------------------- autorun --------
static void RunAutorun(lua_State* L) {
    std::wstring dir = g_rootDir + L"scripts\\autorun\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.lua").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> files;
    do { files.push_back(fd.cFileName); } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(files.begin(), files.end());
    for (auto& f : files) {
        FILE* fp = _wfopen((dir + f).c_str(), L"rb");
        if (!fp) continue;
        std::string code; char buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) code.append(buf, n);
        fclose(fp);
        LogL(LOG_DEBUG, "autorun: %s", Narrow(f).c_str());
        RunChunk(L, code, "@autorun/" + Narrow(f));
    }
}

// Some natives (TeleportEntityPos, GetEntityPositionVec3, ...) are registered by FC64
// into a different Lua state than the one we run in. Their registration code is
//   48 8D 15 <fn>   lea rdx, fn
//   45 33 C0        xor r8d, r8d
//   48 8B CB        mov rcx, rbx
//   E8 <pushcclosure>
//   4C 8D 05 <name> lea r8, "Name"
// so find the name string, then the lea that loads it, then the function 0x12 bytes before.
static lua_CFunction FindRegisteredNative(const char* name) {
    uintptr_t base = FC64Base();
    if (!base) return nullptr;
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    size_t len = strlen(name) + 1;
    uintptr_t str = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections && !str; ++i) {
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        uint8_t* s = (uint8_t*)(base + sec[i].VirtualAddress);
        size_t n = sec[i].Misc.VirtualSize;
        if (n < len + 1 || !Readable(s, n)) continue;
        for (size_t j = 1; j + len <= n; ++j)
            if (s[j] == (uint8_t)name[0] && s[j - 1] == 0 && memcmp(s + j, name, len) == 0) { str = (uintptr_t)(s + j); break; }
    }
    if (!str) { LogL(LOG_WARN, "native %s: name string not found", name); return nullptr; }
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* c = (uint8_t*)(base + sec[i].VirtualAddress);
        size_t n = sec[i].Misc.VirtualSize;
        for (size_t j = 0x12; j + 7 <= n; ++j) {
            if (c[j] != 0x4C || c[j + 1] != 0x8D || c[j + 2] != 0x05) continue;
            uintptr_t tgt = (uintptr_t)(c + j + 7) + *(int32_t*)(c + j + 3);
            if (tgt != str) continue;
            uint8_t* f = c + j - 0x12;
            if (f[0] != 0x48 || f[1] != 0x8D || f[2] != 0x15 || f[7] != 0x45 || f[8] != 0x33) continue;
            uintptr_t fn = (uintptr_t)(f + 7) + *(int32_t*)(f + 3);
            LogL(LOG_DEBUG, "native %s found at FC64+0x%llx", name, (unsigned long long)(fn - base));
            return (lua_CFunction)fn;
        }
    }
    LogL(LOG_WARN, "native %s: registration not found", name);
    return nullptr;
}

// Exposes those natives as Mod<Name> in our state (only if the game didn't already give us <Name>).
static void RegisterHiddenNatives(lua_State* L) {
    static const char* names[] = { "TeleportEntityPos", "GetEntityPositionVec3" };
    static lua_CFunction found[2] = {};
    static bool searched = false;
    if (!searched) { searched = true; for (int i = 0; i < 2; ++i) found[i] = FindRegisteredNative(names[i]); }
    for (int i = 0; i < 2; ++i) {
        if (!found[i]) continue;
        std::string mod = std::string("Mod") + names[i];
        p_pushcclosure(L, found[i], 0);
        p_setfield(L, FC4_LUA_GLOBALSINDEX, mod.c_str());
    }
}

// ---------------------------------------------------------- the tick -----
// Runs game-Lua source that returns one string; used by the Lua 5.4 bridge.
bool GameEvalString(lua_State* L, const std::string& code, const char* chunk, std::string* out) {
    int top = my_gettop(L);
    int st = p_loadbuffer(L, code.data(), code.size(), chunk ? chunk : "=lua54");
    if (st == 0) st = p_pcall(L, 0, 1, 0);
    size_t len = 0;
    const char* s = my_gettop(L) > top ? p_tolstring(L, -1, &len) : nullptr;
    if (out) out->assign(s ? s : "", s ? len : 0);
    my_settop(L, top);
    return st == 0 && s != nullptr;
}
lua_State* GameLuaState() { return GetLuaState(); }

// ModKeyDown(vk) -> true while that key is held (only when the game window has focus
// and you're not typing in the menu). For script hotkeys and fly controls.
static int L_ModKeyDown(lua_State* L) {
    const char* s = ArgStr(L, 1);
    int vk = s ? atoi(s) : 0;
    if (vk <= 0 || vk > 255 || !GameHasFocus() || MenuWantsKeyboard()) return ReturnLua(L, "false");
    return ReturnLua(L, (GetAsyncKeyState(vk) & 0x8000) ? "true" : "false");
}

// ModPadTrigger("left" | "right") -> 0..1, how far that gamepad trigger is pressed (0 with
// no pad, when the game isn't focused or while the menu is open).
static int L_ModPadTrigger(lua_State* L) {
    const char* s = ArgStr(L, 1);
    int lt = 0, rt = 0;
    if (!GameHasFocus() || g_menuOpen || !PadTriggers(0, &lt, &rt)) return ReturnLua(L, "0");
    int v = (s && s[0] == 'l') ? lt : rt;
    char b[32]; snprintf(b, sizeof(b), "%.3f", v / 255.0);
    return ReturnLua(L, b);
}

// ModSetAudioSpeed(f): play all game sound at f x speed (0.05 .. 1), for slow motion.
static int L_ModSetAudioSpeed(lua_State* L) {
    const char* s = ArgStr(L, 1);
    AudioSetSpeed(s ? (float)atof(s) : 1.f);
    return 0;
}

// ModArchetypeRef("archetypeId", "arkPickupArchetype" | "#hash") -> "id" or nil
static int L_ModArchetypeRef(lua_State* L) {
    const char* a = ArgStr(L, 1); const char* f = ArgStr(L, 2);
    if (!a || !f) return ReturnLua(L, "nil");
    uint64_t v = ArchetypeRefField(strtoull(a, nullptr, 10), f);
    if (!v || v == ~0ull) return ReturnLua(L, "nil");
    char b[32]; snprintf(b, sizeof(b), "\"%llu\"", (unsigned long long)v);
    return ReturnLua(L, b);
}

// ModWorkshopRegister() -> number of Workshop weapons the game now knows (registers missing ones)
static int L_ModWorkshopRegister(lua_State* L) { char b[16]; snprintf(b, sizeof(b), "%d", WorkshopRegister()); return ReturnLua(L, b); }
// ModWorkshopList() -> "item,props,pickup;..." of every Workshop weapon (mods\*\workshop.txt)
static int L_ModWorkshopList(lua_State* L) { return ReturnLua(L, Quote(WorkshopIdList())); }

// ---- small Lua helpers shared with memory.cpp ---------------------------
int         LuaTop(lua_State* L) { return my_gettop(L); }
const char* LuaArg(lua_State* L, int i) { return ArgStr(L, i); }
int         LuaArgType(lua_State* L, int i) { return my_gettop(L) >= i ? (Base(L) + (i - 1))->tt : -1; }
bool        LuaArgBool(lua_State* L, int i) { return LuaArgType(L, i) == 1 && *(int*)&(Base(L) + (i - 1))->value != 0; }
int         LuaReturn(lua_State* L, const std::string& expr) { return ReturnLua(L, expr); }
std::string LuaQuote(const std::string& s) { return Quote(s); }
void*       LuaNativeFn(lua_State* L, const char* name) { return NativeCFunction(L, name); }
// Game entity (pawn, vehicle, ...) object for an entity id, or nullptr if not loaded.
void* EntityObject(lua_State* L, uint64_t id) {
    ResolveTowerHelpers(L);
    if (!p_entityLookup) return nullptr;
    HandleRef h(id);
    return h.entity();
}

static void RegisterBuiltins(lua_State* L) {
    struct { const char* n; lua_CFunction f; } fns[] = {
        { "ModLog", L_ModLog }, { "ModLogDebug", L_ModLogDebug }, { "ModLogWarn", L_ModLogWarn }, { "ModLogError", L_ModLogError },
        { "ModSetState", L_ModSetState }, { "ModKeyDown", L_ModKeyDown }, { "ModPadTrigger", L_ModPadTrigger }, { "ModSetAudioSpeed", L_ModSetAudioSpeed }, { "ModArchetypeRef", L_ModArchetypeRef }, { "ModWorkshopList", L_ModWorkshopList }, { "ModWorkshopRegister", L_ModWorkshopRegister }, { "ModGetState", L_ModGetState }, { "ModVersion", L_ModVersion }, { "ModFileExists", L_ModFileExists },
        { "ModWriteFile", L_ModWriteFile }, { "ModReadFile", L_ModReadFile }, { "ModCopyArchiveRange", L_ModCopyArchiveRange }, { "ModDumpStrings", L_ModDumpStrings },
        { "ModNativeAddress", L_ModNativeAddress }, { "ModGetUnlockedTowers", L_ModGetUnlockedTowers },
        { "ModGrantTowerRewards", L_ModGrantTowerRewards }, { "ModTowerRewardKeys", L_ModTowerRewardKeys },
        { "ModRewardState", L_ModRewardState }, { "ModGrantRewardKey", L_ModGrantRewardKey },
        { "ModInspectTower", L_ModInspectTower }, { "ModTowerPositions", L_ModTowerPositions },
    };
    for (auto& e : fns) { p_pushcclosure(L, e.f, 0); p_setfield(L, FC4_LUA_GLOBALSINDEX, e.n); }
    int n = 0;
    const LuaReg* mem = MemoryFunctions(&n);
    for (int i = 0; i < n; ++i) { p_pushcclosure(L, mem[i].fn, 0); p_setfield(L, FC4_LUA_GLOBALSINDEX, mem[i].name); }
    RegisterHiddenNatives(L);
}

static std::atomic<uint64_t> s_lastTickFrame{ ~0ull };

static void LoadModStringsOnce();
static void ScriptThreadTick(lua_State* L) {
    if (GState(L) != s_boundG.load()) {
        RegisterBuiltins(L);
        s_funcsApplied = 0;
        s_reloadAutorun = true;
        s_boundG = GState(L);
        s_luaReady = true;
        Log("Lua bridge live (lua_State=%p)", L);
        Lua54OnGameState(L);
        SetState("world.loads", std::to_string(atoi(GetState("world.loads", "0").c_str()) + 1));
        LoadModStringsOnce();
    }

    std::vector<LuaJob> jobs;
    std::vector<std::pair<std::string, lua_CFunction>> newFuncs;
    std::vector<void (*)(lua_State*)> cbs;
    {
        std::lock_guard<std::mutex> lk(s_qMx);
        jobs.swap(s_jobs);
        s_pending = false;
        newFuncs.assign(s_funcs.begin() + s_funcsApplied, s_funcs.end());
        s_funcsApplied = s_funcs.size();
        cbs = s_tickCbs;
    }
    for (auto& f : newFuncs) { p_pushcclosure(L, f.second, 0); p_setfield(L, FC4_LUA_GLOBALSINDEX, f.first.c_str()); }
    if (s_reloadAutorun.exchange(false)) RunAutorun(L);
    for (auto& j : jobs) RunChunk(L, j.code, j.chunk);

    static uint64_t lastFrame = ~0ull;
    uint64_t frame = g_frameCounter.load();
    if (frame != lastFrame) {
        lastFrame = frame;
        for (auto cb : cbs) cb(L);
        int saved = my_gettop(L);
        p_getfield(L, FC4_LUA_GLOBALSINDEX, "OnModTick");
        if (TypeAtTop(L) == 6 && p_pcall(L, 0, 0, 0) != 0) {
            const char* msg = p_tolstring(L, -1, nullptr);
            LogL(LOG_ERROR, "[lua] OnModTick error: %s (callback disabled)", msg ? msg : "?");
            my_settop(L, saved);
            RunChunk(L, "OnModTick = nil", "=fc4hook");
        }
        my_settop(L, saved);
        Lua54Tick(L);
    }
    Lua54RunJobs(L);
}

static thread_local bool t_inTick = false;

// The whole tick runs inside lua_pcall, so a Lua error can't longjmp out of our
// C++ and leave t_inTick stuck true.
static int L_TickBody(lua_State* L) { ScriptThreadTick(L); return 0; }

static void ProtectedTick(lua_State* L) {
    t_inTick = true;
    int top = my_gettop(L);
    p_pushcclosure(L, L_TickBody, 0);
    if (p_pcall(L, 0, 0, 0) != 0) {
        static int errors = 0;
        const char* msg = my_gettop(L) > top ? p_tolstring(L, -1, nullptr) : nullptr;
        if (++errors <= 20) LogL(LOG_ERROR, "[fc4hook] tick error (recovered): %s", msg ? msg : "?");
    }
    my_settop(L, top);
    t_inTick = false;
}

static std::atomic<DWORD>    s_luaThread{ 0 };
static std::atomic<uint64_t> s_pumpTicks{ 0 };
static std::atomic<uint64_t> s_pumpMidLua{ 0 };
uint64_t PumpTicks() { return s_pumpTicks.load(); }
uint64_t PumpRejectsMidLua() { return s_pumpMidLua.load(); }

// Called by the Lua VM on calls and every kHookEvery instructions.
static void LuaCountHook(lua_State* L, lua_Debug* ar) {
    if (s_prevHook) s_prevHook(L, ar);
    ++s_hookCalls;
    DWORD tid = GetCurrentThreadId();
    if (s_luaThread.load() != tid) {
        DWORD old = s_luaThread.exchange(tid);
        LogL(LOG_DEBUG, "Lua runs on thread %lu%s", tid, old ? " (changed!)" : "");
    }
    if (t_inTick) return;
    uint64_t f = g_frameCounter.load(std::memory_order_relaxed);
    if (!s_pending.load(std::memory_order_relaxed) && f == s_lastTickFrame.load(std::memory_order_relaxed) &&
        GState(L) == s_boundG.load(std::memory_order_relaxed))
        return;
    s_lastTickFrame = f;
    lua_State* mainL = GetLuaState();
    if (!mainL || GState(L) != GState(mainL)) return;
    ProtectedTick(L);
}

// Message-pump tick: same thread as Lua, and Lua idle (call stack at base_ci).
static void TryPumpTick() {
    if (!s_luaReady) return;
    if (GetCurrentThreadId() != s_luaThread.load(std::memory_order_relaxed)) return;
    if (t_inTick) return;
    lua_State* L = GetLuaState();
    if (!L || GState(L) != s_boundG.load()) return;
    if (CI(L) != BaseCI(L) || NCcalls(L) != 0) { ++s_pumpMidLua; return; }
    uint64_t f = g_frameCounter.load(std::memory_order_relaxed);
    if (!s_pending.load(std::memory_order_relaxed) && f == s_lastTickFrame.load(std::memory_order_relaxed)) return;
    s_lastTickFrame = f;
    ++s_pumpTicks;
    ProtectedTick(L);
}

typedef BOOL (WINAPI* PeekMessage_t)(LPMSG, HWND, UINT, UINT, UINT);
static PeekMessage_t o_PeekA = nullptr, o_PeekW = nullptr;
static void NotePumpThread() {
    static std::atomic<DWORD> seen[8] = {};
    DWORD t = GetCurrentThreadId();
    for (auto& s : seen) {
        DWORD v = s.load(std::memory_order_relaxed);
        if (v == t) return;
        if (v == 0) {
            DWORD expected = 0;
            if (s.compare_exchange_strong(expected, t)) { LogL(LOG_DEBUG, "PeekMessage called on thread %lu", t); return; }
            if (expected == t) return;
        }
    }
}
static BOOL WINAPI hk_PeekA(LPMSG m, HWND h, UINT a, UINT b, UINT r) { NotePumpThread(); TryPumpTick(); return o_PeekA(m, h, a, b, r); }
static BOOL WINAPI hk_PeekW(LPMSG m, HWND h, UINT a, UINT b, UINT r) { NotePumpThread(); TryPumpTick(); return o_PeekW(m, h, a, b, r); }

static bool LooksLikeLuaState(lua_State* L) {
    if (!L) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(L, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
        return false;
    return ObjType(L) == LUA_TTHREAD && GState(L) != nullptr && Top(L) >= Base(L);
}

static void InstallLuaHook(lua_State* L) {
    lua_Hook cur = HookFn(L);
    if (cur != LuaCountHook) s_prevHook = cur;
    HookFn(L) = LuaCountHook;
    BaseHookCount(L) = kHookEvery;
    HookCount(L) = kHookEvery;
    MemoryBarrier();
    HookMask(L) = (uint8_t)(HookMask(L) | kOurMask);
}

static DWORD WINAPI HookWatcher(LPVOID) {
    lua_State* lastL = nullptr;
    bool warned = false;
    for (;;) {
        lua_State* L = GetLuaState();
        if (L && LooksLikeLuaState(L)) {
            if (L != lastL || HookFn(L) != LuaCountHook || (HookMask(L) & kOurMask) != kOurMask) {
                InstallLuaHook(L);
                if (L != lastL)
                    LogL(LOG_DEBUG, "Lua debug hook installed on lua_State %p (prev hook %p, mask 0x%02x)", L, s_prevHook, HookMask(L));
                lastL = L;
                s_status = "hook installed, waiting for scripts";
            }
        } else if (L && !warned) {
            LogL(LOG_DEBUG, "g_pScriptManager->L = %p doesn't look like a lua_State yet", L);
            warned = true;
        } else if (!L) {
            s_status = "waiting for game's Lua state";
        }
        Sleep(500);
    }
    return 0;
}

// ---------------------------------------------------------- init ----------
std::vector<SigStatus> SignatureReport() {
    std::vector<SigStatus> r;
    for (auto& s : s_sigs) r.push_back({ s.name, (uintptr_t)*s.out });
    return r;
}

// ------------------------------------------------------ mod archive ------
// FC64 keeps a table of archive slots (0x90 bytes each: name @+0x18, archive @+0x90,
// flags @+0x98, priority @+0x9c relative to the table) and a priority-sorted, locked list
// of mounted archives (lower priority value = searched first). The PC build never loads
// slot 6 ("patch_%lang%.dat"), so we use it to mount data_win32\fc4hook_mods.dat with
// priority -2, above patch (1) and patch_hd (0). Found via the startup loop
// (FUN_18007ced0) that walks the slots and calls the slot loader (FUN_18007ca70).
typedef void* (*SlotLoad_t)(void* table, int slot, const char* name, int lang, int p5, char p6, int prio);
static void**     p_ppSlots   = nullptr;
static SlotLoad_t p_slotLoad  = nullptr;
static std::atomic<int> s_modArchive{ 0 };   // 0 pending, 1 mounted, -1 not found/failed, 2 no file
static int        kModSlot = -1;
static const int  kModPrio = -2;

static uint8_t* SlotBase(void* table, int i) { return (uint8_t*)table + i * 0x90; }

static void LogSlots(void* table) {
    for (int i = 0; i < 0x43; ++i) {
        uint8_t* s = SlotBase(table, i);
        const char* name = (const char*)(s + 0x18);
        if (!name[0] && !*(void**)(s + 0x90)) continue;
        LogL(LOG_DEBUG, "  archive slot %2d %-28s flags=0x%02x prio=%d %s", i, name, *(uint32_t*)(s + 0x98),
            *(int32_t*)(s + 0x9c), *(void**)(s + 0x90) ? "loaded" : "-");
    }
}

// --- file access tracing for the mod archive (diagnostics) ---------------
// Logs opens of any fc4hook_mods.* / entitylibrary-related file, and the first reads made
// on the fc4hook_mods.dat handle (offset + size), so we can see what the game pulls from it.
typedef HANDLE (WINAPI* CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL (WINAPI* ReadFile_t)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static CreateFileW_t o_CreateFileW = nullptr;
static ReadFile_t    o_ReadFile    = nullptr;
static std::atomic<HANDLE> s_modDatHandle{ INVALID_HANDLE_VALUE };
static std::atomic<int> s_modReads{ 0 }, s_dataOpens{ 0 };
static ULONGLONG s_bootTick = GetTickCount64();

static HANDLE WINAPI hk_CreateFileW(LPCWSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD fl, HANDLE tmpl) {
    HANDLE h = o_CreateFileW(name, acc, share, sa, disp, fl, tmpl);
    if (name) {
        std::wstring n = name;
        std::transform(n.begin(), n.end(), n.begin(), ::towlower);
        if (n.find(L"fc4hook_mods") != std::wstring::npos) {
            LogL(LOG_DEBUG, "trace: open %s -> %p (err %lu)", Narrow(name).c_str(), h, h == INVALID_HANDLE_VALUE ? GetLastError() : 0);
            if (h != INVALID_HANDLE_VALUE && n.size() > 4 && n.compare(n.size() - 4, 4, L".dat") == 0) s_modDatHandle = h;
        } else if (s_dataOpens < 400 && GetTickCount64() - s_bootTick < 120000) {
            // first 2 minutes: log every file the game opens (except our own log), to find intro videos
            bool skip = n.find(L"fc4hook.log") != std::wstring::npos || n.find(L"\\.\\") == 0;
            if (!skip) { ++s_dataOpens; LogL(LOG_DEBUG, "trace: open %s", Narrow(name).c_str()); }
        }
    }
    return h;
}

static BOOL WINAPI hk_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    if (h == s_modDatHandle.load() && h != INVALID_HANDLE_VALUE && s_modReads < 200) {
        ++s_modReads;
        unsigned long long off = ov ? ((unsigned long long)ov->OffsetHigh << 32 | ov->Offset) : ~0ull;
        LogL(LOG_DEBUG, "trace: read fc4hook_mods.dat offset=%llu size=%lu", off, n);
    }
    return o_ReadFile(h, buf, n, got, ov);
}

typedef HANDLE (WINAPI* CreateFileMappingW_t)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR);
static CreateFileMappingW_t o_CreateFileMappingW = nullptr;
static HANDLE WINAPI hk_CreateFileMappingW(HANDLE f, LPSECURITY_ATTRIBUTES sa, DWORD prot, DWORD hi, DWORD lo, LPCWSTR nm) {
    if (f == s_modDatHandle.load() && f != INVALID_HANDLE_VALUE) Log("trace: fc4hook_mods.dat is memory-mapped");
    return o_CreateFileMappingW(f, sa, prot, hi, lo, nm);
}

static void InstallFileTrace() {
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE kb  = GetModuleHandleW(L"kernelbase.dll");
    void* cf = kb ? (void*)GetProcAddress(kb, "CreateFileW") : nullptr;
    void* rf = kb ? (void*)GetProcAddress(kb, "ReadFile") : nullptr;
    if (!cf) cf = (void*)GetProcAddress(k32, "CreateFileW");
    if (!rf) rf = (void*)GetProcAddress(k32, "ReadFile");
    bool a = CreateHook(cf, (void*)hk_CreateFileW, (void**)&o_CreateFileW);
    bool b = CreateHook(rf, (void*)hk_ReadFile, (void**)&o_ReadFile);
    void* fm = kb ? (void*)GetProcAddress(kb, "CreateFileMappingW") : nullptr;
    if (!fm) fm = (void*)GetProcAddress(k32, "CreateFileMappingW");
    bool c = fm && CreateHook(fm, (void*)hk_CreateFileMappingW, (void**)&o_CreateFileMappingW);
    Log("trace: file hooks %s/%s/%s", a ? "ok" : "FAILED", b ? "ok" : "FAILED", c ? "ok" : "FAILED");
}

// Mount <name> (in data_win32) into the last free archive slot. Returns the slot or -1.
static int MountArchive(void* t, const char* name, int prio) {
    int slot = -1;
    for (int i = 0x42; i > 23 && slot < 0; --i) {   // last free slot: least likely used by DLC/maps
        uint8_t* s = SlotBase(t, i);
        if (!*(const char*)(s + 0x18) && !*(void**)(s + 0x90) && *(uint32_t*)(s + 0x98) == 0) slot = i;
    }
    if (slot < 0) { LogL(LOG_WARN, "mods: no free archive slot for %s", name); LogSlots(t); return -1; }
    void* a = p_slotLoad(t, slot, name, 0, 1, 1, prio);
    LogL(a ? LOG_INFO : LOG_ERROR, a ? "mods: mounted data_win32\\%s in slot %d (priority %d)" : "mods: FAILED to mount %s (slot %d, priority %d)",
         name, slot, prio);
    return a ? slot : -1;
}

static const int kLoosePrio = -3;       // loose-file mods win over fc4hook_mods.dat (-2), patch (1), patch_hd (0)

static DWORD WINAPI ModArchiveThread(LPVOID) {
    if (g_cfg.traceFiles) InstallFileTrace();
    bool loose = VfsPrepare();          // build data_win32\fc4hook_loose.* while the game boots
    uintptr_t loop = FindPattern("FC64.dll",
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 48 83 EC 40 48 C7 C6 68 FF FF FF");
    if (!loop || *(uint8_t*)(loop + 0x73) != 0xE8 || *(uint16_t*)(loop + 0x33) != 0x8B48) {
        LogL(LOG_WARN, "mods: archive slot loader not found; mod archives disabled"); s_modArchive = -1;
        if (loose) VfsMounted(false);
        return 0;
    }
    p_ppSlots  = (void**)ResolveRip(loop + 0x33, 3, 7);
    p_slotLoad = (SlotLoad_t)ResolveRip(loop + 0x73, 1, 5);
    LogL(LOG_DEBUG, "mods: slot table ptr FC64+0x%llx, loader FC64+0x%llx",
        (unsigned long long)((uintptr_t)p_ppSlots - FC64Base()), (unsigned long long)((uintptr_t)p_slotLoad - FC64Base()));
    std::wstring dat = g_rootDir + L"..\\..\\data_win32\\fc4hook_mods.dat";
    bool modDat = GetFileAttributesW(dat.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!modDat) {
        Log("mods: no data_win32\\fc4hook_mods.dat installed (that's fine, it's only needed for custom-content mods)"); s_modArchive = 2;
        if (!loose) return 0;
    }
    // Wait for the game to create the table and mount its own patch.dat (slot 3).
    for (int i = 0; i < 120000; ++i) {
        void* t = *p_ppSlots;
        if (t && *(void**)(SlotBase(t, 3) + 0x90)) break;
        Sleep(5);
    }
    void* t = *p_ppSlots;
    if (!t) { LogL(LOG_WARN, "mods: archive table never appeared"); if (modDat) s_modArchive = -1; if (loose) VfsMounted(false); return 0; }
    // Unnamed, unused slots only, so the game never reloads something else into them
    // (slot 6 "patch_%lang%" gets reloaded by the language setup ~15 s after boot).
    if (modDat) {
        kModSlot = MountArchive(t, "fc4hook_mods.dat", kModPrio);
        s_modArchive = kModSlot >= 0 ? 1 : -1;
    }
    if (loose) VfsMounted(MountArchive(t, "fc4hook_loose.dat", kLoosePrio) >= 0);
    LogSlots(t);
    return 0;
}


// ------------------------------------------------------- mod strings ------
// The game loads its own UI text before we can mount the mod archive, so instead of
// replacing oasisstrings_compressed.bin we ADD a small one, the same way DLCs do
// (FUN_18086dc50(stringMgr, path, &datasetId), found via the DLC language loader).
typedef char (*AddStrings_t)(void* mgr, const char* path, uint32_t* dataset);
static void**       p_ppStringMgr = nullptr;
static AddStrings_t p_addStrings  = nullptr;
static std::atomic<int> s_modStrings{ 0 };   // 0 not tried, 1 loaded, -1 failed

static bool ResolveStringLoader() {
    if (p_addStrings) return true;
    uintptr_t a = FindPattern("FC64.dll", "48 8B 0D ?? ?? ?? ?? 4C 8D 44 24 70 48 8B D3 E8 ?? ?? ?? ?? 48 8D 4C 24 30 E8");
    if (!a) { LogL(LOG_WARN, "mods: DLC string loader not found"); return false; }
    p_ppStringMgr = (void**)ResolveRip(a, 3, 7);
    p_addStrings  = (AddStrings_t)ResolveRip(a + 0xF, 1, 5);
    LogL(LOG_DEBUG, "mods: string manager ptr FC64+0x%llx, add-strings FC64+0x%llx",
        (unsigned long long)((uintptr_t)p_ppStringMgr - FC64Base()), (unsigned long long)((uintptr_t)p_addStrings - FC64Base()));
    return true;
}

static bool LoadModStrings(const char* path) {
    if (!ResolveStringLoader() || !*p_ppStringMgr) return false;
    uint32_t dataset = 0xFC440001;
    char ok = p_addStrings(*p_ppStringMgr, path, &dataset);
    LogL(LOG_DEBUG, "mods: add strings %s -> %s", path, ok ? "ok" : "not found / failed");
    return ok != 0;
}

// ModLoadStrings("fc4hook\\languages\\english\\strings.bin") -> "ok" / "failed"
static int L_ModLoadStrings(lua_State* L) {
    const char* p = ArgStr(L, 1);
    if (!p) return 0;
    return ReturnLua(L, Quote(LoadModStrings(p) ? "ok" : "failed"));
}

static void LoadModStringsOnce() {
    // re-run whenever a new Lua state comes up (world load); reloading the same dataset replaces it
    // A mod archive can carry fc4hook\languages\english\strings.bin with its own UI text.
    if (s_modArchive.load() != 1) return;
    s_modStrings = LoadModStrings("fc4hook\\languages\\english\\strings.bin") ? 1 : -1;
    if (s_modStrings == 1) Log("mods: loaded custom text from the mod archive");
}

const char* ModArchiveStatus() {
    switch (s_modArchive.load()) {
    case 1: return "mounted"; case 2: return "not installed"; case 0: return "waiting"; default: return "failed (see log)";
    }
}

static int L_ModArchiveStatus(lua_State* L) { return ReturnLua(L, Quote(ModArchiveStatus())); }

// ModFileId("graphics\\foo.xbt") -> "1234..." (decimal string: game Lua numbers are 32-bit floats)
static int L_ModFileId(lua_State* L) {
    const char* p = ArgStr(L, 1);
    if (!p) return 0;
    char buf[32]; snprintf(buf, sizeof(buf), "%llu", (unsigned long long)FileId(p));
    return ReturnLua(L, Quote(buf));
}
// ModArchetypeFile("7602852352217") -> "entityarchetypeslibrary\\7602852352217.ark.fcb"
static int L_ModArchetypeFile(lua_State* L) {
    const char* p = ArgStr(L, 1);
    if (!p) return 0;
    return ReturnLua(L, Quote(std::string("entityarchetypeslibrary\\") + p + ".ark.fcb"));
}
static int L_ModLooseStatus(lua_State* L) { return ReturnLua(L, Quote(VfsStatus())); }
static int L_ModFileLog(lua_State* L) {
    if (LuaTop(L) >= 1) FileLogSetEnabled(LuaArgBool(L, 1));
    return ReturnLua(L, FileLogEnabled() ? "true" : "false");
}

bool GameInit() {
    { HANDLE mt = CreateThread(nullptr, 0, ModArchiveThread, nullptr, 0, nullptr); if (mt) CloseHandle(mt); }
    RegisterLuaFunction("ModArchiveStatus", L_ModArchiveStatus);
    RegisterLuaFunction("ModLoadStrings", L_ModLoadStrings);
    RegisterLuaFunction("ModFileId", L_ModFileId);
    RegisterLuaFunction("ModArchetypeFile", L_ModArchetypeFile);
    RegisterLuaFunction("ModLooseStatus", L_ModLooseStatus);
    RegisterLuaFunction("ModFileLog", L_ModFileLog);
    FileLogInstall();
    WorkshopInstallGuard();
    ClipMonitorInstall();
    if (FileLogAtStartup() && FileLogAvailable()) FileLogSetEnabled(true);
    uintptr_t fc64 = FC64Base();
    bool ok = true;
    for (auto& s : s_sigs) {
        uintptr_t a = FindPattern("FC64.dll", s.pattern);
        if (a && s.ripLen) a = ResolveRip(a, s.ripOff, s.ripLen);
        *s.out = (void*)a;
        if (a) LogL(LOG_DEBUG, "sig %-28s -> FC64+0x%llx", s.name, (unsigned long long)(a - fc64));
        else { LogL(s.required ? LOG_ERROR : LOG_DEBUG, "sig %-28s -> NOT FOUND%s", s.name, s.required ? "" : " (optional)"); if (s.required) ok = false; }
    }
    if (!ok) { s_status = "signatures missing (unsupported game version?)"; LogL(LOG_ERROR, "Required signatures missing; Lua bridge disabled. FC4Hook supports the Steam/Uplay PC build 1.10 (FC64.dll 0x5c6150fe)."); return false; }
    Log("Game functions found (%d signatures)", (int)(sizeof(s_sigs) / sizeof(s_sigs[0])));
    s_ready = true;
    s_status = "waiting for game's Lua state";
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    CreateHook((void*)GetProcAddress(u32, "PeekMessageA"), (void*)hk_PeekA, (void**)&o_PeekA);
    CreateHook((void*)GetProcAddress(u32, "PeekMessageW"), (void*)hk_PeekW, (void**)&o_PeekW);
    HANDLE t = CreateThread(nullptr, 0, HookWatcher, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    return true;
}

// ------------------------------------------------------- plugin API ------
static void ApiLog(const char* fmt, ...) {
    char buf[2048]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    Log("%s", buf);
}
static void ApiQueueLua(const char* c, const char* n) { QueueLua(c ? c : "", n ? n : ""); }
static void ApiTrampPush(lua_State* L, lua_CFunction f, int n) { p_pushcclosure(L, f, n); }
static void ApiTrampSet(lua_State* L, int i, const char* k)    { p_setfield(L, i, k); }
static void ApiTrampGet(lua_State* L, int i, const char* k)    { p_getfield(L, i, k); }
static const char* ApiTrampTo(lua_State* L, int i, size_t* l)  { return p_tolstring(L, i, l); }
static int ApiTrampLoad(lua_State* L, const char* b, size_t s, const char* n) { return p_loadbuffer(L, b, s, n); }
static int ApiTrampPCall(lua_State* L, int a, int r, int e)    { return p_pcall(L, a, r, e); }

static void ApiLogLevel(int level, const char* fmt, ...) { va_list ap; va_start(ap, fmt); LogV(level, fmt, ap); va_end(ap); }
static void ApiSetState(const char* k, const char* v) { if (k) SetState(k, v ? v : ""); }
static int  ApiGetState(const char* k, char* buf, size_t n) {
    if (!k || !buf || !n) return 0;
    std::string v = GetState(k, "\x01");
    if (v == "\x01") { buf[0] = 0; return 0; }
    snprintf(buf, n, "%s", v.c_str());
    return 1;
}
static const wchar_t* ApiRootDir() { return g_rootDir.c_str(); }
static const char* ApiVersion() { return FC4HOOK_VERSION; }
static void ApiQueueLua54(const char* code, const char* chunk) { if (code) Lua54Queue(code, chunk ? chunk : "=plugin"); }
static int  ApiLooseStatus(char* buf, size_t n) { if (!buf || !n) return 0; snprintf(buf, n, "%s", VfsStatus().c_str()); return 1; }

const FC4HookAPI* Api() {
    static FC4HookAPI api = {
        FC4HOOK_API_VERSION,
        ApiLog, FindPattern, CreateHook, RemoveHook,
        ApiQueueLua, RegisterLuaFunction,
        AddTickCallback, AddMenuCallback, GetImGuiContextPtr,
        GetLuaState, my_gettop, my_settop,
        ApiTrampPush, ApiTrampSet, ApiTrampGet, ApiTrampTo, ApiTrampLoad, ApiTrampPCall,
        ApiLogLevel, ApiSetState, ApiGetState, ApiRootDir, ApiVersion,
        HookVtable, UnhookVtable, AddPresentCallback, D3DDevice, D3DContext, D3DSwapChain,
        FileId, ApiQueueLua54, ApiLooseStatus,
    };
    return &api;
}

} // namespace fc4
