// FC4Hook entry point. Built as fc4hook.asi; loaded by Ultimate ASI Loader.
#include "core.h"

using namespace fc4;

static const char* kDefaultIni =
    "; FC4Hook settings. Most of these can also be changed in game: F8 > Settings.\r\n"
    "[General]\r\n"
    "; Virtual-key code that opens the menu. 119 = F8, 45 = Insert, 36 = Home, 192 = ` (tilde)\r\n"
    "MenuKey=119\r\n"
    "; Show the \"FC4Hook loaded\" banner for the first 20 seconds\r\n"
    "ShowLoadBanner=1\r\n"
    "; Menu size (0.5 - 3.0)\r\n"
    "UIScale=1.00\r\n"
    "; 1 = show the Developer tab (native browser, script runner, diagnostics)\r\n"
    "DeveloperMode=0\r\n"
    "\r\n"
    "[Log]\r\n"
    "; debug, info, warn or error. The log is bin\\fc4hook\\fc4hook.log\r\n"
    "Level=info\r\n"
    "; Keep the previous session's log as fc4hook.prev.log\r\n"
    "KeepPreviousLog=1\r\n"
    "; Write a report to bin\\fc4hook\\crashes\\ if the game crashes\r\n"
    "CrashReports=1\r\n"
    "\r\n"
    "[Hotkeys]\r\n"
    "; Virtual-key codes (0 = off). Example: 112 = F1, 113 = F2 ... 123 = F12, 96-105 = Numpad 0-9\r\n"
    "GodMode=0\r\n"
    "UnlimitedAmmo=0\r\n"
    "RefillAmmo=0\r\n"
    "SlowMotion=0\r\n"
    "OpenArmory=0\r\n"
    "Fly=0\r\n"
    "HideHud=0\r\n"
    "Brake=0\r\n"
    "; 1 = hold both sticks in (L3 + R3) for half a second to open/close the menu with a gamepad\r\n"
    "GamepadMenu=1\r\n"
    "\r\n"
    "[Debug]\r\n"
    "; Log every file the game opens during the first 2 minutes (for modders)\r\n"
    "TraceFileAccess=0\r\n";

static void SetupPaths(HMODULE self) {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(self, buf, MAX_PATH);
    std::wstring p = buf;
    p = p.substr(0, p.find_last_of(L"\\/") + 1) + L"fc4hook\\";
    g_rootDir = p;
    for (const wchar_t* d : { L"", L"scripts", L"scripts\\autorun", L"scripts\\coop", L"plugins", L"dumps",
                              L"coop", L"mods", L"armory", L"crashes" })
        CreateDirectoryW((p + d).c_str(), nullptr);

    std::wstring ini = p + L"fc4hook.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (FILE* f = _wfopen(ini.c_str(), L"wb")) { fputs(kDefaultIni, f); fclose(f); }
    }
    LoadConfig();
}

static DWORD WINAPI InitThread(LPVOID self) {
    SetupPaths((HMODULE)self);
    LogInit();
    InstallCrashHandler();
    Log("%s starting (root: %s)", FC4HOOK_TITLE, Narrow(g_rootDir).c_str());
    Log("log level %s, menu key %s, developer mode %s", LevelName(g_cfg.logLevel), KeyName(g_cfg.menuKey).c_str(),
        g_cfg.developerMode ? "on" : "off");

    // FarCry4.exe is a launcher; the engine lives in FC64.dll, which loads a bit later.
    for (int i = 0; i < 600 && !GetModuleHandleA("FC64.dll"); ++i) Sleep(100);
    if (!GetModuleHandleA("FC64.dll")) { LogL(LOG_ERROR, "FC64.dll never loaded; giving up"); return 0; }
    Log("FC64.dll at %p", GetModuleHandleA("FC64.dll"));

    OverlayInit();
    InputInit();
    LoadPlugins();
    GameInit();
    Log("Init done. Press %s in game for the menu.", KeyName(g_cfg.menuKey).c_str());

    for (;;) {
        Sleep(15000);
        CheckCrashHandler();
        LogL(LOG_DEBUG, "heartbeat: frames=%llu luaHookCalls=%llu pumpTicks=%llu pumpSkippedMidLua=%llu lua=%s",
             (unsigned long long)g_frameCounter.load(), (unsigned long long)LuaHookCalls(),
             (unsigned long long)PumpTicks(), (unsigned long long)PumpRejectsMidLua(), BridgeStatus());
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        HANDLE t = CreateThread(nullptr, 0, InitThread, mod, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
