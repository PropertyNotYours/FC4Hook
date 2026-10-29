// Config, logging, crash reports, pattern scanning and MinHook wrappers.
#include "core.h"
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <deque>
#include <psapi.h>
#include "MinHook.h"

namespace fc4 {

std::wstring g_rootDir;
bool   g_menuOpen = false;
Config g_cfg;

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// ------------------------------------------------------------- config ----
static std::wstring IniPath() { return g_rootDir + L"fc4hook.ini"; }

static int IniInt(const wchar_t* sec, const wchar_t* key, int def) {
    return GetPrivateProfileIntW(sec, key, def, IniPath().c_str());
}
static std::wstring IniStr(const wchar_t* sec, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[128];
    GetPrivateProfileStringW(sec, key, def, buf, 128, IniPath().c_str());
    return buf;
}
static void IniSet(const wchar_t* sec, const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(sec, key, v.c_str(), IniPath().c_str());
}
static void IniSetInt(const wchar_t* sec, const wchar_t* key, int v) { IniSet(sec, key, std::to_wstring(v)); }

static int ParseLevel(const std::wstring& s) {
    std::wstring l = s;
    for (auto& c : l) c = (wchar_t)towlower(c);
    if (l == L"debug" || l == L"0") return LOG_DEBUG;
    if (l == L"warn" || l == L"warning" || l == L"2") return LOG_WARN;
    if (l == L"error" || l == L"3") return LOG_ERROR;
    return LOG_INFO;
}

void LoadConfig() {
    g_cfg.menuKey       = IniInt(L"General", L"MenuKey", VK_F8);
    g_cfg.showBanner    = IniInt(L"General", L"ShowLoadBanner", 1) != 0;
    g_cfg.uiScale       = (float)_wtof(IniStr(L"General", L"UIScale", L"1.0").c_str());
    if (!(g_cfg.uiScale >= 0.5f && g_cfg.uiScale <= 3.0f)) g_cfg.uiScale = 1.0f;
    g_cfg.developerMode = IniInt(L"General", L"DeveloperMode", 0) != 0;
    g_cfg.logLevel      = ParseLevel(IniStr(L"Log", L"Level", L"info"));
    g_cfg.keepPrevLog   = IniInt(L"Log", L"KeepPreviousLog", 1) != 0;
    g_cfg.crashReports  = IniInt(L"Log", L"CrashReports", 1) != 0;
    g_cfg.traceFiles    = IniInt(L"Debug", L"TraceFileAccess", 0) != 0;
    g_cfg.hkGodMode     = IniInt(L"Hotkeys", L"GodMode", 0);
    g_cfg.hkAmmo        = IniInt(L"Hotkeys", L"UnlimitedAmmo", 0);
    g_cfg.hkRefill      = IniInt(L"Hotkeys", L"RefillAmmo", 0);
    g_cfg.hkSlowMo      = IniInt(L"Hotkeys", L"SlowMotion", 0);
    g_cfg.hkMenuArmory  = IniInt(L"Hotkeys", L"OpenArmory", 0);
    g_cfg.hkFly         = IniInt(L"Hotkeys", L"Fly", 0);
    g_cfg.hkHud         = IniInt(L"Hotkeys", L"HideHud", 0);
    g_cfg.hkBrake       = IniInt(L"Hotkeys", L"Brake", 0);
    g_cfg.padMenu       = IniInt(L"Hotkeys", L"GamepadMenu", 1) != 0;
    if (g_cfg.menuKey <= 0 || g_cfg.menuKey > 254) g_cfg.menuKey = VK_F8;
}

void SaveConfig() {
    static const wchar_t* levels[] = { L"debug", L"info", L"warn", L"error" };
    wchar_t scale[32]; swprintf(scale, 32, L"%.2f", g_cfg.uiScale);
    IniSetInt(L"General", L"MenuKey", g_cfg.menuKey);
    IniSetInt(L"General", L"ShowLoadBanner", g_cfg.showBanner);
    IniSet   (L"General", L"UIScale", scale);
    IniSetInt(L"General", L"DeveloperMode", g_cfg.developerMode);
    IniSet   (L"Log", L"Level", levels[g_cfg.logLevel & 3]);
    IniSetInt(L"Log", L"KeepPreviousLog", g_cfg.keepPrevLog);
    IniSetInt(L"Log", L"CrashReports", g_cfg.crashReports);
    IniSetInt(L"Debug", L"TraceFileAccess", g_cfg.traceFiles);
    IniSetInt(L"Hotkeys", L"GodMode", g_cfg.hkGodMode);
    IniSetInt(L"Hotkeys", L"UnlimitedAmmo", g_cfg.hkAmmo);
    IniSetInt(L"Hotkeys", L"RefillAmmo", g_cfg.hkRefill);
    IniSetInt(L"Hotkeys", L"SlowMotion", g_cfg.hkSlowMo);
    IniSetInt(L"Hotkeys", L"OpenArmory", g_cfg.hkMenuArmory);
    IniSetInt(L"Hotkeys", L"Fly", g_cfg.hkFly);
    IniSetInt(L"Hotkeys", L"HideHud", g_cfg.hkHud);
    IniSetInt(L"Hotkeys", L"Brake", g_cfg.hkBrake);
    IniSetInt(L"Hotkeys", L"GamepadMenu", g_cfg.padMenu ? 1 : 0);
}

std::string KeyName(int vk) {
    if (vk <= 0) return "none";
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, (char)vk);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Numpad " + std::to_string(vk - VK_NUMPAD0);
    switch (vk) {
    case VK_INSERT: return "Insert"; case VK_DELETE: return "Delete"; case VK_HOME: return "Home";
    case VK_END: return "End"; case VK_PRIOR: return "Page Up"; case VK_NEXT: return "Page Down";
    case VK_OEM_3: return "` (tilde)"; case VK_PAUSE: return "Pause"; case VK_SCROLL: return "Scroll Lock";
    case VK_MULTIPLY: return "Numpad *"; case VK_ADD: return "Numpad +"; case VK_SUBTRACT: return "Numpad -";
    case VK_DIVIDE: return "Numpad /"; case VK_DECIMAL: return "Numpad ."; case VK_BACK: return "Backspace";
    }
    char b[24]; snprintf(b, sizeof(b), "key %d", vk); return b;
}

// ---------------------------------------------------------------- log ----
static std::mutex          s_logMx;
static std::deque<LogLine> s_logLines;
static FILE*               s_logFile = nullptr;
static std::atomic<uint64_t> s_logSerial{ 0 };
static std::atomic<int>    s_counts[4];

const char* LevelName(int level) {
    switch (level) { case LOG_DEBUG: return "DEBUG"; case LOG_WARN: return "WARN "; case LOG_ERROR: return "ERROR"; default: return "INFO "; }
}

void LogInit() {
    std::wstring p = g_rootDir + L"fc4hook.log";
    if (g_cfg.keepPrevLog) {
        std::wstring prev = g_rootDir + L"fc4hook.prev.log";
        DeleteFileW(prev.c_str());
        MoveFileW(p.c_str(), prev.c_str());
    }
    s_logFile = _wfopen(p.c_str(), L"w");
}

void LogV(int level, const char* fmt, va_list ap) {
    if (level < LOG_DEBUG || level > LOG_ERROR) level = LOG_INFO;
    ++s_counts[level];
    if (level < g_cfg.logLevel) return;
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, ap);

    SYSTEMTIME t; GetLocalTime(&t);
    char line[2140];
    snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s %s", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
             LevelName(level), buf);

    std::lock_guard<std::mutex> lk(s_logMx);
    if (s_logFile) { fputs(line, s_logFile); fputc('\n', s_logFile); fflush(s_logFile); }
    s_logLines.push_back({ level, line });
    while (s_logLines.size() > 3000) s_logLines.pop_front();
    ++s_logSerial;
}

void LogL(int level, const char* fmt, ...) { va_list ap; va_start(ap, fmt); LogV(level, fmt, ap); va_end(ap); }
void Log(const char* fmt, ...)             { va_list ap; va_start(ap, fmt); LogV(LOG_INFO, fmt, ap); va_end(ap); }

std::vector<LogLine> LogSnapshot() {
    std::lock_guard<std::mutex> lk(s_logMx);
    return std::vector<LogLine>(s_logLines.begin(), s_logLines.end());
}
uint64_t LogSerial() { return s_logSerial.load(); }
void LogClear() { std::lock_guard<std::mutex> lk(s_logMx); s_logLines.clear(); ++s_logSerial; }
int  LogCount(int level) { return (level >= 0 && level < 4) ? s_counts[level].load() : 0; }

// ------------------------------------------------------- crash reports ----
// Writes fc4hook\crashes\crash_<date>_<time>.txt when the game dies from an unhandled
// exception: exception code, faulting module+offset, registers, and every code address
// found on the stack (module+offset), then hands over to whatever filter was there before
// (usually the game's own crash reporter).
static LPTOP_LEVEL_EXCEPTION_FILTER s_prevFilter = nullptr;
static std::atomic<bool> s_inCrash{ false };

static void ModuleOf(uintptr_t addr, char* out, size_t n) {
    HMODULE m = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &m) && m) {
        char path[MAX_PATH] = "?";
        GetModuleFileNameA(m, path, MAX_PATH);
        const char* base = strrchr(path, '\\'); base = base ? base + 1 : path;
        snprintf(out, n, "%s+0x%llx", base, (unsigned long long)(addr - (uintptr_t)m));
    } else {
        snprintf(out, n, "0x%llx", (unsigned long long)addr);
    }
}

static bool IsCodeAddress(uintptr_t a) {
    if (a < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)a, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE) return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

static const char* ExceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
    case 0xC0000409: return "STACK_BUFFER_OVERRUN";
    case 0xE06D7363: return "C++ exception";
    }
    return "unknown";
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    if (!s_inCrash.exchange(true) && ep && ep->ExceptionRecord && ep->ContextRecord) {
        auto* er = ep->ExceptionRecord; auto* c = ep->ContextRecord;
        SYSTEMTIME t; GetLocalTime(&t);
        wchar_t name[64];
        swprintf(name, 64, L"crash_%04d-%02d-%02d_%02d%02d%02d.txt", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        std::wstring dir = g_rootDir + L"crashes\\";
        CreateDirectoryW(dir.c_str(), nullptr);
        FILE* f = _wfopen((dir + name).c_str(), L"w");
        char where[160];
        ModuleOf((uintptr_t)er->ExceptionAddress, where, sizeof(where));
        // The crash may have happened while the log lock was held: never block on it here.
        std::vector<LogLine> lines;
        bool locked = s_logMx.try_lock();
        if (locked) {
            lines.assign(s_logLines.begin(), s_logLines.end());
            if (s_logFile) {
                fprintf(s_logFile, "[crash] ERROR GAME CRASH: %s (0x%08lx) at %s  -> report fc4hook\\crashes\\%ls\n",
                        ExceptionName(er->ExceptionCode), er->ExceptionCode, where, name);
                fflush(s_logFile);
            }
            s_logMx.unlock();
        }
        if (f) {
            fprintf(f, "%s crash report\n%04d-%02d-%02d %02d:%02d:%02d\n\n", FC4HOOK_TITLE, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
            fprintf(f, "Exception: %s (0x%08lx)\nAddress:   %s\n", ExceptionName(er->ExceptionCode), er->ExceptionCode, where);
            if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
                fprintf(f, "Access:    %s 0x%llx\n", er->ExceptionInformation[0] == 0 ? "read of" :
                        er->ExceptionInformation[0] == 1 ? "write to" : "execute at", (unsigned long long)er->ExceptionInformation[1]);
            fprintf(f, "Thread:    %lu\nLua bridge: %s\nFrames:    %llu\n\n", GetCurrentThreadId(), BridgeStatus(),
                    (unsigned long long)g_frameCounter.load());
            fprintf(f, "RAX %016llx RBX %016llx RCX %016llx RDX %016llx\nRSI %016llx RDI %016llx RBP %016llx RSP %016llx\n"
                       "R8  %016llx R9  %016llx R10 %016llx R11 %016llx\nR12 %016llx R13 %016llx R14 %016llx R15 %016llx\nRIP %016llx\n\n",
                    c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->Rsp, c->R8, c->R9, c->R10, c->R11,
                    c->R12, c->R13, c->R14, c->R15, c->Rip);
            fprintf(f, "Code addresses on the stack (most recent first):\n");
            uintptr_t* sp = (uintptr_t*)c->Rsp;
            int found = 0;
            for (int i = 0; i < 4096 && found < 48; ++i) {
                MEMORY_BASIC_INFORMATION mbi;
                if (!VirtualQuery(sp + i, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) break;
                uintptr_t v = sp[i];
                if (!IsCodeAddress(v)) continue;
                char m[160]; ModuleOf(v, m, sizeof(m));
                fprintf(f, "  [rsp+0x%04x] %s\n", i * 8, m);
                ++found;
            }
            fprintf(f, "\nLoaded modules:\n");
            HMODULE mods[512]; DWORD need = 0;
            if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
                for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 512; ++i) {
                    char p[MAX_PATH]; MODULEINFO mi = {};
                    GetModuleFileNameA(mods[i], p, MAX_PATH);
                    GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi));
                    fprintf(f, "  %016llx %8lx %s\n", (unsigned long long)(uintptr_t)mi.lpBaseOfDll, mi.SizeOfImage, p);
                }
            }
            fprintf(f, "\nLast log lines:\n");
            if (!locked) fprintf(f, "  (log was busy at the time of the crash; see fc4hook.log)\n");
            for (size_t i = lines.size() > 60 ? lines.size() - 60 : 0; i < lines.size(); ++i) fprintf(f, "%s\n", lines[i].text.c_str());
            fclose(f);
        }
    }
    return s_prevFilter ? s_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashHandler() {
    if (!g_cfg.crashReports) return;
    LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(CrashFilter);
    if (prev != CrashFilter) s_prevFilter = prev;
}

// The game (or its crash reporter) may install its own filter after us; put ours back
// in front and keep theirs as the next one in line.
void CheckCrashHandler() {
    if (!g_cfg.crashReports) return;
    LPTOP_LEVEL_EXCEPTION_FILTER cur = SetUnhandledExceptionFilter(CrashFilter);
    if (cur != CrashFilter && cur) {
        s_prevFilter = cur;
        LogL(LOG_DEBUG, "crash handler re-chained in front of %p", cur);
    }
}

// ------------------------------------------------------- pattern scan ----
static bool ParsePattern(const char* pat, std::vector<int>& out) {
    out.clear();
    for (const char* p = pat; *p;) {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?') { out.push_back(-1); ++p; if (*p == '?') ++p; continue; }
        char hex[3] = { p[0], p[1], 0 };
        if (!isxdigit((unsigned char)hex[0]) || !isxdigit((unsigned char)hex[1])) return false;
        out.push_back((int)strtoul(hex, nullptr, 16));
        p += 2;
    }
    return !out.empty();
}

uintptr_t FindPattern(const char* module, const char* pattern) {
    HMODULE mod = GetModuleHandleA(module ? module : "FC64.dll");
    if (!mod) return 0;
    std::vector<int> bytes;
    if (!ParsePattern(pattern, bytes)) return 0;

    auto base = (uint8_t*)mod;
    auto dos  = (IMAGE_DOS_HEADER*)base;
    auto nt   = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto sec  = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* start = base + sec->VirtualAddress;
        size_t   size  = sec->Misc.VirtualSize;
        size_t   n     = bytes.size();
        if (size < n) continue;
        for (size_t off = 0; off + n <= size; ++off) {
            size_t j = 0;
            for (; j < n; ++j)
                if (bytes[j] != -1 && start[off + j] != (uint8_t)bytes[j]) break;
            if (j == n) return (uintptr_t)(start + off);
        }
    }
    return 0;
}

uintptr_t ResolveRip(uintptr_t instr, int dispOffset, int instrLen) {
    if (!instr) return 0;
    int32_t disp = *(int32_t*)(instr + dispOffset);
    return instr + instrLen + disp;
}

// ------------------------------------------------------------ hooks ------
int CreateHook(void* target, void* detour, void** original) {
    static std::once_flag init;
    if (!target) { LogL(LOG_WARN, "CreateHook: null target"); return 0; }
    std::call_once(init, [] { MH_Initialize(); });     // hooks get created from several threads
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) { LogL(LOG_ERROR, "MH_CreateHook(%p) failed: %s", target, MH_StatusToString(s)); return 0; }
    s = MH_EnableHook(target);
    if (s == MH_OK || s == MH_ERROR_ENABLED) return 1;
    MEMORY_BASIC_INFORMATION mbi = {};
    VirtualQuery(target, &mbi, sizeof(mbi));
    LogL(LOG_ERROR, "MH_EnableHook(%p) failed: %s  [page protect=0x%lx type=0x%lx state=0x%lx]",
         target, MH_StatusToString(s), mbi.Protect, mbi.Type, mbi.State);
    return 0;
}

// Swap one entry of an object's virtual table. Every object of that class shares the table,
// so this hooks the method for all of them. Returns 1 on success; *original gets the old entry.
int HookVtable(void* object, int index, void* detour, void** original) {
    if (!object || index < 0 || index > 4096 || !detour) return 0;
    void** vt = nullptr;
    if (!MemReadable((uintptr_t)object, 8)) return 0;
    vt = *(void***)object;
    if (!MemReadable((uintptr_t)(vt + index), 8)) return 0;
    DWORD old;
    if (!VirtualProtect(vt + index, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return 0;
    void* prev = InterlockedExchangePointer(vt + index, detour);
    VirtualProtect(vt + index, sizeof(void*), old, &old);
    if (original) *original = prev;
    LogL(LOG_DEBUG, "vtable hook: %p[%d] %p -> %p", (void*)vt, index, prev, detour);
    return 1;
}

int UnhookVtable(void* object, int index, void* original) {
    if (!original) return 0;
    void* dummy = nullptr;
    return HookVtable(object, index, original, &dummy);
}

int RemoveHook(void* target) {
    MH_DisableHook(target);
    return MH_RemoveHook(target) == MH_OK;
}

} // namespace fc4
