// Memory binder: read/write/scan game memory from Lua.
//
// Far Cry 4's Lua numbers are single-precision floats (24-bit mantissa), so a 64-bit
// address or integer passed as a Lua number would be silently rounded. Everything here
// therefore takes and returns addresses and 32/64-bit integers as STRINGS:
//   addresses:  "0x7FF6A1B20000"   or Cheat Engine style  "FC64+2DD8958",  "[FC64+2DD8958]+8"
//   integers:   decimal strings ("123456789"); f32/f64 and 8/16-bit values come back as numbers
// The Lua wrapper in scripts\autorun\05_memory.lua (Mem.*) makes this pleasant to use.
//
// Every read/write is checked with VirtualQuery first, so a bad address returns nil/an
// error message instead of crashing the game. (Writes to code pages need VirtualProtect,
// which FC64.dll's code refuses, so code patching isn't possible from here.)
#include "core.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace fc4 {

std::string Hex(uintptr_t v) { char b[24]; snprintf(b, sizeof(b), "0x%llX", (unsigned long long)v); return b; }

// ------------------------------------------------------------ safety -----
static bool RegionOk(uintptr_t a, size_t n, bool write) {
    while (n > 0) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)a, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
        DWORD p = mbi.Protect & 0xFF;
        if ((mbi.Protect & PAGE_GUARD) || p == PAGE_NOACCESS) return false;
        if (write && !(p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY)) return false;
        uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        size_t chunk = (size_t)(end - a) < n ? (size_t)(end - a) : n;
        a += chunk; n -= chunk;
    }
    return true;
}
bool MemReadable(uintptr_t a, size_t n) { return a >= 0x10000 && RegionOk(a, n, false); }
bool MemWritable(uintptr_t a, size_t n) { return a >= 0x10000 && RegionOk(a, n, true); }

// ------------------------------------------------- address expressions ----
struct Parser {
    const char* p; std::string err;
    void ws() { while (*p == ' ' || *p == '\t') ++p; }
    bool term(uintptr_t* out) {
        ws();
        if (*p == '[') {
            ++p;
            uintptr_t inner;
            if (!expr(&inner)) return false;
            ws();
            if (*p != ']') { err = "missing ]"; return false; }
            ++p;
            if (!MemReadable(inner, 8)) { err = "can't read pointer at " + Hex(inner); return false; }
            *out = *(uintptr_t*)inner;
            return true;
        }
        const char* s = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_' || *p == '.')) ++p;
        std::string tok(s, p - s);
        if (tok.empty()) { err = std::string("unexpected '") + (*p ? std::string(1, *p) : std::string("end")) + "'"; return false; }
        // module name? (FC64, FC64.dll, FarCry4.exe, ...)
        bool allHex = true;
        std::string num = tok;
        if (num.size() > 2 && num[0] == '0' && (num[1] == 'x' || num[1] == 'X')) num = num.substr(2);
        for (char c : num) if (!isxdigit((unsigned char)c)) { allHex = false; break; }
        HMODULE m = GetModuleHandleA(tok.c_str());
        if (!m && tok.find('.') == std::string::npos) m = GetModuleHandleA((tok + ".dll").c_str());
        if (m && !(allHex && tok.size() > 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X'))) { *out = (uintptr_t)m; return true; }
        if (!allHex || num.empty()) { err = "unknown module or bad number: " + tok; return false; }
        *out = (uintptr_t)_strtoui64(num.c_str(), nullptr, 16);
        return true;
    }
    bool expr(uintptr_t* out) {
        uintptr_t v;
        if (!term(&v)) return false;
        for (;;) {
            ws();
            char op = *p;
            if (op != '+' && op != '-') break;
            ++p;
            uintptr_t r;
            if (!term(&r)) return false;
            v = op == '+' ? v + r : v - r;
        }
        *out = v;
        return true;
    }
};

bool ParseAddress(const std::string& e, uintptr_t* out, std::string* err) {
    Parser ps{ e.c_str(), {} };
    uintptr_t v = 0;
    bool ok = ps.expr(&v);
    if (ok) { ps.ws(); if (*ps.p) { ok = false; ps.err = std::string("unexpected '") + *ps.p + "'"; } }
    if (!ok) { if (err) *err = ps.err; return false; }
    *out = v;
    return true;
}

// Argument i as an address. Lua numbers are accepted only when they're small enough to be exact.
static bool ArgAddr(lua_State* L, int i, uintptr_t* out, std::string* err) {
    const char* s = LuaArg(L, i);
    if (!s) { *err = "missing address"; return false; }
    if (LuaArgType(L, i) == 3) {                       // number: decimal
        double d = atof(s);
        if (d < 0 || d > 16777216.0) { *err = "pass addresses as strings (Lua numbers lose precision)"; return false; }
        *out = (uintptr_t)d;
        return true;
    }
    return ParseAddress(s, out, err);
}

// Integer argument: decimal or 0x hex string (or a small Lua number). Supports negatives.
static bool ParseInt(const char* s, int64_t* out);
static bool ArgInt(lua_State* L, int i, int64_t* out) { return ParseInt(LuaArg(L, i), out); }

// Failures return nil; the reason is kept for MemError() (and logged at debug level).
static int Fail(lua_State* L, const std::string& why) {
    LogL(LOG_DEBUG, "[mem] %s", why.c_str());
    SetState("mem.error", why);
    return LuaReturn(L, "nil");
}

// ------------------------------------------------------------ types ------
struct TypeInfo { const char* name; int size; char kind; };   // kind: i signed, u unsigned, f float, p pointer
static const TypeInfo kTypes[] = {
    { "i8", 1, 'i' }, { "u8", 1, 'u' }, { "i16", 2, 'i' }, { "u16", 2, 'u' }, { "i32", 4, 'i' }, { "u32", 4, 'u' },
    { "i64", 8, 'i' }, { "u64", 8, 'u' }, { "f32", 4, 'f' }, { "f64", 8, 'f' }, { "ptr", 8, 'p' },
    { "byte", 1, 'u' }, { "short", 2, 'i' }, { "int", 4, 'i' }, { "float", 4, 'f' }, { "double", 8, 'f' },
    { "bool", 1, 'b' },
};
static const TypeInfo* FindType(const char* n) {
    if (!n) return &kTypes[5];
    for (auto& t : kTypes) if (_stricmp(t.name, n) == 0) return &t;
    return nullptr;
}

// Value at a as a Lua expression.
static std::string ReadAsLua(uintptr_t a, const TypeInfo* t) {
    char b[64];
    switch (t->kind) {
    case 'b': return *(uint8_t*)a ? "true" : "false";
    case 'f':
        if (t->size == 4) { float f = *(float*)a; if (!(f == f)) return "0/0"; snprintf(b, sizeof(b), "%.9g", f); }
        else { double d = *(double*)a; if (!(d == d)) return "0/0"; snprintf(b, sizeof(b), "%.17g", d); }
        if (strstr(b, "inf")) return b[0] == '-' ? "-math.huge" : "math.huge";
        return b;
    case 'p': return LuaQuote(Hex(*(uintptr_t*)a));
    }
    int64_t sv = 0; uint64_t uv = 0;
    switch (t->size) {
    case 1: sv = *(int8_t*)a;  uv = *(uint8_t*)a;  break;
    case 2: sv = *(int16_t*)a; uv = *(uint16_t*)a; break;
    case 4: sv = *(int32_t*)a; uv = *(uint32_t*)a; break;
    default: sv = *(int64_t*)a; uv = *(uint64_t*)a; break;
    }
    if (t->kind == 'i') snprintf(b, sizeof(b), "%lld", (long long)sv); else snprintf(b, sizeof(b), "%llu", (unsigned long long)uv);
    if (t->size <= 2) return b;                 // exact as a float: plain number
    return LuaQuote(b);                          // 32/64-bit: string, so nothing is rounded
}

// ------------------------------------------------------------ functions --
// MemAddress(expr) -> "0x..."            resolve an address expression
static int L_MemAddress(lua_State* L) {
    uintptr_t a; std::string e;
    if (!ArgAddr(L, 1, &a, &e)) return Fail(L, e);
    return LuaReturn(L, LuaQuote(Hex(a)));
}

// MemModule("FC64.dll") -> { base = "0x...", size = n, path = "..." }
static int L_MemModule(lua_State* L) {
    const char* n = LuaArg(L, 1);
    HMODULE m = GetModuleHandleA(n);
    if (!m && n && !strchr(n, '.')) m = GetModuleHandleA((std::string(n) + ".dll").c_str());
    if (!m) return Fail(L, std::string("module not loaded: ") + (n ? n : "(exe)"));
    auto* nt = (IMAGE_NT_HEADERS64*)((uint8_t*)m + ((IMAGE_DOS_HEADER*)m)->e_lfanew);
    char path[MAX_PATH] = ""; GetModuleFileNameA(m, path, MAX_PATH);
    char sz[24]; snprintf(sz, sizeof(sz), "%lu", (unsigned long)nt->OptionalHeader.SizeOfImage);
    return LuaReturn(L, "{ base = " + LuaQuote(Hex((uintptr_t)m)) + ", size = " + sz + ", path = " + LuaQuote(path) + " }");
}

// MemRead(addr [, type="u32"]) -> value   types: i8 u8 i16 u16 i32 u32 i64 u64 f32 f64 ptr bool
static int L_MemRead(lua_State* L) {
    uintptr_t a; std::string e;
    if (!ArgAddr(L, 1, &a, &e)) return Fail(L, e);
    const TypeInfo* t = FindType(LuaArg(L, 2));
    if (!t) return Fail(L, std::string("unknown type ") + LuaArg(L, 2));
    if (!MemReadable(a, t->size)) return Fail(L, "can't read " + Hex(a));
    return LuaReturn(L, ReadAsLua(a, t));
}

static bool ParseInt(const char* s, int64_t* out) {
    if (!s) return false;
    while (*s == ' ') ++s;
    bool neg = *s == '-'; if (neg || *s == '+') ++s;
    uint64_t v;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) v = _strtoui64(s + 2, nullptr, 16);
    else if (strchr(s, '.') || strchr(s, 'e')) v = (uint64_t)atof(s);
    else v = _strtoui64(s, nullptr, 10);
    *out = neg ? -(int64_t)v : (int64_t)v;
    return true;
}

// Write a value given as text ("123", "-5", "0x10", "1.5", "true", an address for ptr).
bool MemWriteValue(uintptr_t a, const char* type, const char* vs, std::string* err) {
    const TypeInfo* t = FindType(type);
    if (!t) { *err = "unknown type"; return false; }
    if (!vs) { *err = "missing value"; return false; }
    if (!MemWritable(a, t->size)) { *err = "not writable: " + Hex(a); return false; }
    if (t->kind == 'f') {
        double d = atof(vs);
        if (t->size == 4) *(float*)a = (float)d; else *(double*)a = d;
    } else if (t->kind == 'b') {
        *(uint8_t*)a = (uint8_t)(_stricmp(vs, "true") == 0 || atoi(vs) != 0);
    } else {
        int64_t v = 0;
        if (t->kind == 'p') { uintptr_t pv; if (!ParseAddress(vs, &pv, err)) return false; v = (int64_t)pv; }
        else if (!ParseInt(vs, &v)) { *err = "bad integer"; return false; }
        switch (t->size) { case 1: *(uint8_t*)a = (uint8_t)v; break; case 2: *(uint16_t*)a = (uint16_t)v; break;
                           case 4: *(uint32_t*)a = (uint32_t)v; break; default: *(uint64_t*)a = (uint64_t)v; }
    }
    return true;
}

// Value as display text (for the Memory tab), or "??" if unreadable.
std::string MemFormat(uintptr_t a, const char* type) {
    const TypeInfo* t = FindType(type);
    if (!t || !MemReadable(a, t->size)) return "??";
    std::string v = ReadAsLua(a, t);
    if (v.size() >= 2 && v[0] == '"') v = v.substr(1, v.size() - 2);
    return v;
}

// MemWrite(addr, type, value) -> true | nil  (value: number, string or boolean)
static int L_MemWrite(lua_State* L) {
    uintptr_t a; std::string e;
    if (!ArgAddr(L, 1, &a, &e)) return Fail(L, e);
    const char* vs = LuaArg(L, 3);
    if (LuaArgType(L, 3) == 1) vs = LuaArgBool(L, 3) ? "1" : "0";
    if (!MemWriteValue(a, LuaArg(L, 2) ? LuaArg(L, 2) : "u32", vs, &e)) return Fail(L, e);
    return LuaReturn(L, "true");
}

// MemError() -> reason the last Mem* call returned nil
static int L_MemError(lua_State* L) { return LuaReturn(L, LuaQuote(GetState("mem.error"))); }

// MemReadString(addr [, maxLen=256 [, "utf16"]]) -> string
static int L_MemReadString(lua_State* L) {
    uintptr_t a; std::string e;
    if (!ArgAddr(L, 1, &a, &e)) return Fail(L, e);
    int64_t max = 256; if (LuaArg(L, 2)) ArgInt(L, 2, &max);
    if (max <= 0 || max > 65536) max = 256;
    bool wide = LuaArg(L, 3) && _stricmp(LuaArg(L, 3), "utf16") == 0;
    std::string out;
    for (int64_t i = 0; i < max; ++i) {
        if (wide) {
            if (!MemReadable(a + i * 2, 2)) break;
            wchar_t c = *(wchar_t*)(a + i * 2); if (!c) break;
            out += c < 0x80 ? (char)c : '?';
        } else {
            if (!MemReadable(a + i, 1)) break;
            char c = *(char*)(a + i); if (!c) break;
            out += c;
        }
    }
    return LuaReturn(L, LuaQuote(out));
}

// MemReadBytes(addr, n) -> "48 8B 05 ..."
static int L_MemReadBytes(lua_State* L) {
    uintptr_t a; std::string e;
    if (!ArgAddr(L, 1, &a, &e)) return Fail(L, e);
    int64_t n = 16; if (LuaArg(L, 2)) ArgInt(L, 2, &n);
    if (n <= 0 || n > 4096) return Fail(L, "size must be 1-4096");
    if (!MemReadable(a, (size_t)n)) return Fail(L, "can't read " + Hex(a));
    std::string out; char b[4];
    for (int64_t i = 0; i < n; ++i) { snprintf(b, sizeof(b), i ? " %02X" : "%02X", ((uint8_t*)a)[i]); out += b; }
    return LuaReturn(L, LuaQuote(out));
}

// MemWriteBytes(addr, "90 90 ...") -> true | nil, error
static int L_MemWriteBytes(lua_State* L) {
    uintptr_t a; std::string e;
    if (!ArgAddr(L, 1, &a, &e)) return Fail(L, e);
    const char* s = LuaArg(L, 2);
    if (!s) return Fail(L, "missing bytes");
    std::vector<uint8_t> bytes;
    for (const char* p = s; *p;) {
        if (*p == ' ' || *p == ',') { ++p; continue; }
        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) return Fail(L, "bad hex bytes");
        char h[3] = { p[0], p[1], 0 }; bytes.push_back((uint8_t)strtoul(h, nullptr, 16)); p += 2;
    }
    if (bytes.empty()) return Fail(L, "no bytes");
    if (!MemWritable(a, bytes.size())) return Fail(L, "not writable: " + Hex(a));
    memcpy((void*)a, bytes.data(), bytes.size());
    return LuaReturn(L, "true");
}

// MemScan(pattern [, module="FC64.dll"]) -> "0x..." or nil      IDA style: "48 8B 05 ?? ?? ?? ??"
static int L_MemScan(lua_State* L) {
    const char* pat = LuaArg(L, 1);
    if (!pat) return Fail(L, "missing pattern");
    const char* mod = LuaArg(L, 2);
    uintptr_t r = FindPattern(mod ? mod : "FC64.dll", pat);
    if (!r) return Fail(L, "pattern not found");
    return LuaReturn(L, LuaQuote(Hex(r)));
}

// MemRip(addr [, dispOffset=3, instrLen=7]) -> target of a RIP-relative instruction
static int L_MemRip(lua_State* L) {
    uintptr_t a; std::string e;
    if (!ArgAddr(L, 1, &a, &e)) return Fail(L, e);
    int64_t off = 3, len = 7;
    if (LuaArg(L, 2)) ArgInt(L, 2, &off);
    if (LuaArg(L, 3)) ArgInt(L, 3, &len);
    if (!MemReadable(a + off, 4)) return Fail(L, "can't read " + Hex(a));
    return LuaReturn(L, LuaQuote(Hex(ResolveRip(a, (int)off, (int)len))));
}

// MemOffset(addr, off1 [, off2...]) -> addr + off1 + off2 ...  (64-bit math Lua can't do)
static int L_MemOffset(lua_State* L) {
    uintptr_t a; std::string e;
    if (!ArgAddr(L, 1, &a, &e)) return Fail(L, e);
    for (int i = 2; i <= LuaTop(L); ++i) { int64_t o; if (ArgInt(L, i, &o)) a += (uintptr_t)o; }
    return LuaReturn(L, LuaQuote(Hex(a)));
}

// MemPointer(base, off1, off2, ...) -> address at the end of a pointer chain
// (Cheat Engine semantics: [[base]+off1]+off2 ...)  or nil if any link is unreadable
static int L_MemPointer(lua_State* L) {
    uintptr_t a; std::string e;
    if (!ArgAddr(L, 1, &a, &e)) return Fail(L, e);
    for (int i = 2; i <= LuaTop(L); ++i) {
        if (!MemReadable(a, 8)) return Fail(L, "broken chain at " + Hex(a));
        a = *(uintptr_t*)a;
        int64_t o = 0; ArgInt(L, i, &o);
        a += (uintptr_t)o;
    }
    return LuaReturn(L, LuaQuote(Hex(a)));
}

// MemReadable(addr [, n=1]) -> true / false
static int L_MemReadable(lua_State* L) {
    uintptr_t a; std::string e;
    if (!ArgAddr(L, 1, &a, &e)) return LuaReturn(L, "false");
    int64_t n = 1; if (LuaArg(L, 2)) ArgInt(L, 2, &n);
    return LuaReturn(L, MemReadable(a, (size_t)(n > 0 ? n : 1)) ? "true" : "false");
}

// MemDump(addr, n, "dumps/x.txt") -> writes a hex+ASCII dump under fc4hook\  (n up to 1 MB)
static int L_MemDump(lua_State* L) {
    uintptr_t a; std::string e;
    if (!ArgAddr(L, 1, &a, &e)) return Fail(L, e);
    int64_t n = 256; if (LuaArg(L, 2)) ArgInt(L, 2, &n);
    const char* rel = LuaArg(L, 3);
    if (n <= 0 || n > (1 << 20)) return Fail(L, "size must be 1 - 1048576");
    if (!rel || strstr(rel, "..") || strchr(rel, ':')) return Fail(L, "bad file name");
    std::wstring path = g_rootDir + Widen(rel);
    for (auto& c : path) if (c == L'/') c = L'\\';
    FILE* f = _wfopen(path.c_str(), L"w");
    if (!f) return Fail(L, "can't open file");
    int rows = 0;
    for (int64_t off = 0; off < n; off += 16, ++rows) {
        uintptr_t row = a + off;
        fprintf(f, "%016llX  ", (unsigned long long)row);
        char asc[17] = {};
        for (int j = 0; j < 16; ++j) {
            if (off + j < n && MemReadable(row + j, 1)) { uint8_t c = *(uint8_t*)(row + j); fprintf(f, "%02X ", c); asc[j] = (c >= 32 && c < 127) ? (char)c : '.'; }
            else { fputs("?? ", f); asc[j] = ' '; }
        }
        fprintf(f, " %s\n", asc);
    }
    fclose(f);
    return LuaReturn(L, "true");
}

// MemNative("CAPI_Pawn.SpawnWeapon") -> "0x..." address of a game Lua function's C code
static int L_MemNative(lua_State* L) {
    const char* n = LuaArg(L, 1);
    void* fn = n ? LuaNativeFn(L, n) : nullptr;
    if (!fn) return Fail(L, std::string("not a C native: ") + (n ? n : "?"));
    return LuaReturn(L, LuaQuote(Hex((uintptr_t)fn)));
}

// MemEntity(entityId) -> "0x..." the engine object of an entity (e.g. GetLocalPlayerId()),
// or nil if it isn't loaded. Entity ids are 64-bit: pass them as strings or as returned by the game.
static int L_MemEntity(lua_State* L) {
    int64_t id;
    if (!ArgInt(L, 1, &id)) return Fail(L, "missing entity id");
    void* obj = EntityObject(L, (uint64_t)id);
    if (!obj) return Fail(L, "entity not loaded (or entity lookup not found)");
    return LuaReturn(L, LuaQuote(Hex((uintptr_t)obj)));
}

// MemAlloc(size) -> "0x..." zeroed read/write memory (scratch space; lives until the game exits)
static int L_MemAlloc(lua_State* L) {
    int64_t n = 0;
    if (!ArgInt(L, 1, &n) || n <= 0 || n > (64 << 20)) return Fail(L, "size must be 1 byte - 64 MB");
    void* p = VirtualAlloc(nullptr, (SIZE_T)n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p) return Fail(L, "VirtualAlloc failed");
    return LuaReturn(L, LuaQuote(Hex((uintptr_t)p)));
}

// MemCall(addr, a1, a2, a3, a4) -> rax as "0x..."
// ADVANCED: calls native code with up to 4 integer/pointer arguments (Windows x64 ABI).
// Wrong address or arguments = game crash. Float arguments aren't supported.
typedef uintptr_t (*Call4_t)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static int L_MemCall(lua_State* L) {
    uintptr_t fn; std::string e;
    if (!ArgAddr(L, 1, &fn, &e)) return Fail(L, e);
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)fn, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
        return Fail(L, "not executable: " + Hex(fn));
    uintptr_t a[4] = {};
    for (int i = 0; i < 4; ++i) {
        const char* s = LuaArg(L, i + 2);
        if (!s) continue;
        int64_t v;
        if (LuaArgType(L, i + 2) == 4 && (strchr(s, '+') || strchr(s, '[') || isalpha((unsigned char)s[0]))) {
            uintptr_t pv; if (!ParseAddress(s, &pv, &e)) return Fail(L, e); a[i] = pv;
        } else if (ArgInt(L, i + 2, &v)) a[i] = (uintptr_t)v;
    }
    LogL(LOG_DEBUG, "[mem] call %s(%s, %s, %s, %s)", Hex(fn).c_str(), Hex(a[0]).c_str(), Hex(a[1]).c_str(), Hex(a[2]).c_str(), Hex(a[3]).c_str());
    uintptr_t r = ((Call4_t)fn)(a[0], a[1], a[2], a[3]);
    return LuaReturn(L, LuaQuote(Hex(r)));
}

static const LuaReg kMem[] = {
    { "MemAddress", L_MemAddress }, { "MemModule", L_MemModule }, { "MemRead", L_MemRead }, { "MemWrite", L_MemWrite },
    { "MemReadString", L_MemReadString }, { "MemReadBytes", L_MemReadBytes }, { "MemWriteBytes", L_MemWriteBytes },
    { "MemScan", L_MemScan }, { "MemRip", L_MemRip }, { "MemOffset", L_MemOffset }, { "MemPointer", L_MemPointer },
    { "MemReadable", L_MemReadable }, { "MemDump", L_MemDump }, { "MemNative", L_MemNative }, { "MemEntity", L_MemEntity },
    { "MemAlloc", L_MemAlloc }, { "MemCall", L_MemCall }, { "MemError", L_MemError },
};
const LuaReg* MemoryFunctions(int* count) { *count = (int)(sizeof(kMem) / sizeof(kMem[0])); return kMem; }

} // namespace fc4
