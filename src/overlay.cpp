// D3D11 Present hook + the FC4Hook ImGui menu.
#include "core.h"
#include <d3d11.h>
#include <dxgi.h>
#include <shellapi.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace fc4 {

std::atomic<uint64_t> g_frameCounter{ 0 };

typedef HRESULT (STDMETHODCALLTYPE* Present_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* ResizeBuffers_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static Present_t        o_Present = nullptr;
static ResizeBuffers_t  o_Resize  = nullptr;

static ID3D11Device*           s_dev = nullptr;
static ID3D11DeviceContext*    s_ctx = nullptr;
static ID3D11RenderTargetView* s_rtv = nullptr;
static HWND                    s_hwnd = nullptr;
static WNDPROC                 s_origWndProc = nullptr;
static bool                    s_imguiInit = false;
static IDXGISwapChain*         s_sc = nullptr;          // swapchain we're attached to
static IDXGISwapChain*         s_candidate = nullptr;   // a different one that's presenting
static int                     s_candidateFrames = 0;
static std::vector<void (*)()> s_menuCbs;
static std::mutex              s_menuMx;
static int                     s_captureKey = 0;        // >0: Settings is waiting for a key press for slot N
static const char*             s_selectTab = nullptr;   // tab to jump to on the next frame

void AddMenuCallback(void (*cb)()) { std::lock_guard<std::mutex> lk(s_menuMx); s_menuCbs.push_back(cb); }
void* GetImGuiContextPtr() { return ImGui::GetCurrentContext(); }

bool MenuWantsKeyboard() { return g_menuOpen && ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput; }
bool GameHasFocus() {
    DWORD fgPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
    return fgPid == GetCurrentProcessId();
}
static bool GameFocused() {
    DWORD fgPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
    return fgPid == GetCurrentProcessId();
}

// ------------------------------------------------------------ input ------
// Keys and mouse are polled (the game uses raw input, so window messages don't always arrive).
// Edge-detected key press; safe to ask about the same key several times in one frame.
static bool KeyPressed(int vk) {
    static bool was[256] = {};
    static bool edge[256] = {};
    static uint64_t frame[256] = {};
    if (vk <= 0 || vk > 255) return false;
    uint64_t f = g_frameCounter.load();
    if (frame[vk] != f) {
        frame[vk] = f;
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        edge[vk] = down && !was[vk];
        was[vk] = down;
    }
    return edge[vk];
}

static void Lua(const char* code, const char* chunk) { QueueLua(code, chunk); }

// Hold L3 + R3 (both sticks pressed) for half a second to open / close the menu with a pad.
static void PollGamepadCombo(bool focused) {
    static ULONGLONG since = 0; static bool fired = false;
    uint16_t b = 0;
    bool combo = g_cfg.padMenu && focused && PadRead(0, &b) && (b & 0x0040) && (b & 0x0080);   // LEFT_THUMB | RIGHT_THUMB
    if (!combo) { since = 0; fired = false; return; }
    ULONGLONG now = GetTickCount64();
    if (!since) since = now;
    if (!fired && now - since >= 500) { fired = true; g_menuOpen = !g_menuOpen; LogL(LOG_DEBUG, "Menu %s (gamepad)", g_menuOpen ? "opened" : "closed"); }
}

static void PollHotkeys() {
    bool focused = GameFocused();
    PollGamepadCombo(focused);
    // Keep the edge state of every bound key fresh even when unfocused.
    bool menu = KeyPressed(g_cfg.menuKey);
    bool god = KeyPressed(g_cfg.hkGodMode), ammo = KeyPressed(g_cfg.hkAmmo), refill = KeyPressed(g_cfg.hkRefill);
    bool slow = KeyPressed(g_cfg.hkSlowMo), armory = KeyPressed(g_cfg.hkMenuArmory), fly = KeyPressed(g_cfg.hkFly);
    bool hud = KeyPressed(g_cfg.hkHud), brake = KeyPressed(g_cfg.hkBrake);
    if (!focused || s_captureKey) return;
    if (menu) { g_menuOpen = !g_menuOpen; LogL(LOG_DEBUG, "Menu %s", g_menuOpen ? "opened" : "closed"); }
    bool typing = g_menuOpen && ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput;
    if (typing || !LuaReady()) return;
    if (god)    Lua("if FC4Cheats then FC4Cheats.toggle('god') end", "=hotkey");
    if (ammo)   Lua("if FC4Cheats then FC4Cheats.toggle('ammo') end", "=hotkey");
    if (refill) Lua("if FC4Cheats then FC4Cheats.refill() end", "=hotkey");
    if (slow)   Lua("if FC4Cheats then FC4Cheats.toggle('slowmo') end", "=hotkey");
    if (armory) { g_menuOpen = true; s_selectTab = "Armory"; }
    if (fly)    Lua("if FC4Cheats then FC4Cheats.toggle('fly') end", "=hotkey");
    if (hud)    Lua("if FC4Cheats then FC4Cheats.toggle('hud') end", "=hotkey");
    if (brake)  Lua("if FC4Cheats then FC4Cheats.brake() end", "=hotkey");
}

static void PollMouse() {
    ImGuiIO& io = ImGui::GetIO();
    POINT p;
    if (GetCursorPos(&p) && ScreenToClient(s_hwnd, &p)) io.AddMousePosEvent((float)p.x, (float)p.y);
    io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(2, (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);
}

typedef BOOL (WINAPI* SetCursorPos_t)(int, int);
typedef BOOL (WINAPI* ClipCursor_t)(const RECT*);
static SetCursorPos_t o_SetCursorPos = nullptr;
static ClipCursor_t   o_ClipCursor   = nullptr;
static BOOL WINAPI hk_SetCursorPos(int x, int y) { return g_menuOpen ? TRUE : o_SetCursorPos(x, y); }
static BOOL WINAPI hk_ClipCursor(const RECT* r)   { return o_ClipCursor(g_menuOpen ? nullptr : r); }

// While the Fly cheat is on, Space (up) and Ctrl (down) belong to FC4Hook: in the air the game
// would use them for the wingsuit/parachute and crouch. FC4Hook itself reads them with
// GetAsyncKeyState, which this doesn't affect.
static bool FlyKey(USHORT vk) { return vk == VK_SPACE || vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL; }
static bool SwallowFlyInput(UINT msg, WPARAM w, LPARAM l) {
    static uint64_t serial = ~0ull; static bool flying = false;
    if (StateSerial() != serial) { serial = StateSerial(); flying = GetState("cheat.flying") == "1"; }
    if (!flying || g_menuOpen) return false;
    if (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) return FlyKey((USHORT)w);
    if (msg != WM_INPUT) return false;
    RAWINPUT ri; UINT size = sizeof(ri);
    if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) return false;
    return ri.header.dwType == RIM_TYPEKEYBOARD && FlyKey(ri.data.keyboard.VKey);
}

static LRESULT CALLBACK HookWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if ((msg == WM_KEYDOWN || msg == WM_KEYUP) && (int)w == g_cfg.menuKey) return 0;
    if (msg == WM_DEVICECHANGE && s_imguiInit && !g_menuOpen) ImGui_ImplWin32_WndProcHandler(h, msg, w, l);   // gamepad plugged in
    if (SwallowFlyInput(msg, w, l)) return msg == WM_INPUT ? DefWindowProcW(h, msg, w, l) : 0;
    if (g_menuOpen && s_imguiInit) {
        ImGui_ImplWin32_WndProcHandler(h, msg, w, l);
        switch (msg) {
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR: case WM_INPUT:
            return msg == WM_INPUT ? DefWindowProcW(h, msg, w, l) : 0;
        }
    }
    return CallWindowProcW(s_origWndProc, h, msg, w, l);
}

// ------------------------------------------------------------ helpers ----
static bool Debounced(const char* label) {
    static std::string lastLabel; static ULONGLONG lastClick = 0;
    ULONGLONG now = GetTickCount64();
    if (lastLabel == label && now - lastClick < 400) return false;
    lastLabel = label; lastClick = now;
    return true;
}

static bool Avail(const char* native) { return GetState(std::string("avail.") + native) == "1"; }
static bool ScriptsLoaded() { return !GetState("avail.EnableGodMode").empty(); }

static void Help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30); ImGui::TextUnformatted(text); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
}

// A button that's greyed out when its native is missing (or Lua isn't live yet).
static bool ActionButton(const char* label, const char* native, ImVec2 size = ImVec2(0, 0)) {
    bool ok = LuaReady() && (!native || Avail(native));
    ImGui::BeginDisabled(!ok);
    bool pressed = ImGui::Button(label, size) && Debounced(label);
    ImGui::EndDisabled();
    if (!ok && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", !LuaReady() ? "Load into the world first." : "Not available in this game build.");
    return pressed;
}

static void OpenFolder(const std::wstring& path) { ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

static std::string LuaSafe(const std::string& s) {     // for use inside [[ ]] long strings
    std::string r; for (char c : s) if (c != ']' && c != '[' && c != '\n' && c != '\r') r += c; return r;
}

static std::string Lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; }

static void NeedWorldNote() {
    if (!LuaReady()) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Game scripts aren't running yet: load a save and these unlock.");
    else if (!ScriptsLoaded()) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "FC4Hook scripts not loaded: check the Console for errors, then Settings > Reload scripts.");
}

// ------------------------------------------------------------ cheats -----
static void CheatSwitch(const char* label, const char* key, const char* native, const char* help) {
    bool on = GetState(std::string("cheat.") + key) == "1";
    bool ok = LuaReady() && Avail(native);
    ImGui::BeginDisabled(!ok);
    if (ImGui::Checkbox(label, &on) && Debounced(label)) {
        char lua[128]; snprintf(lua, sizeof(lua), "FC4Cheats.set('%s', %s)", key, on ? "true" : "false");
        Lua(lua, "=cheats");
        SetState(std::string("cheat.") + key, on ? "1" : "0");   // optimistic; script republishes
    }
    ImGui::EndDisabled();
    if (help) Help(help);
}

// Slider bound to a cheats.txt value: shows the live value, sends FC4Cheats.<fn>(<args>, v) on release.
static void CfgSlider(const char* id, const char* stateKey, const char* def, float lo, float hi, const char* fmt,
                      const char* luaFmt, float width = 14) {
    static std::map<std::string, float> vals;
    float& v = vals.emplace(id, -1e9f).first->second;
    if (v < -1e8f || !ImGui::IsAnyItemActive()) v = (float)atof(GetState(stateKey, def).c_str());
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * width);
    ImGui::BeginDisabled(!LuaReady());
    ImGui::SliderFloat(id, &v, lo, hi, fmt);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        char lua[128]; snprintf(lua, sizeof(lua), luaFmt, v); Lua(lua, "=cheats");
        snprintf(lua, sizeof(lua), "%.2f", v); SetState(stateKey, lua);
    }
    ImGui::EndDisabled();
}

static void TabCheats() {
    NeedWorldNote();
    ImGui::SeparatorText("Player");
    CheatSwitch("God mode", "god", "EnableGodMode", "You can't die. Stays on through deaths, cutscenes and loads until you switch it off.");
    ImGui::SameLine(ImGui::GetFontSize() * 14);
    CheatSwitch("Unlimited ammo", "ammo", "EnableUnlimitedAmmo", "Never run out of ammo or throwables.");
    CheatSwitch("Infinite magazine", "mag", "CAPI_Pawn.SetWeaponAmmoClip", "Your clip never empties: shoot forever without reloading.");
    ImGui::SameLine(ImGui::GetFontSize() * 14);
    CheatSwitch("No fall damage", "nofall", "CAPI_Player.SetLandingDamageEnabled", "Land from any height.");
    if (ActionButton("Refill ammo", "RefillPlayerAmmo")) Lua("FC4Cheats.refill()", "=cheats");
    ImGui::SameLine();
    if (ActionButton("Unstick (up 30 m)", "teleport")) Lua("FC4Cheats.up(30)", "=cheats");
    Help("Teleports you 30 m straight up with god mode on for 20 seconds. For when you're stuck in a rock.");

    ImGui::SeparatorText("Movement");
    CheatSwitch("Speed boost", "speed", "CAPI_Entity.SetEntityPositionVec3", "Moves you further every step (on foot).");
    ImGui::SameLine(ImGui::GetFontSize() * 14);
    {
        static float sx = -1;
        if (sx < 0 || !ImGui::IsAnyItemActive()) sx = (float)atof(GetState("cheat.speedx", "2").c_str());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        ImGui::BeginDisabled(!LuaReady());
        ImGui::SliderFloat("##speedx", &sx, 1.25f, 6.0f, "%.2fx speed");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            char lua[64]; snprintf(lua, sizeof(lua), "FC4Cheats.setCfg('speedx', %.2f)", sx); Lua(lua, "=cheats");
            snprintf(lua, sizeof(lua), "%.2f", sx); SetState("cheat.speedx", lua);
        }
        ImGui::EndDisabled();
    }
    CheatSwitch("Fly", "fly", "CAPI_Entity.SetEntityPositionVec3", "W/A/S/D to move, Space up, Ctrl down, hold Shift to go fast.");
    ImGui::SameLine(ImGui::GetFontSize() * 14);
    CheatSwitch("Noclip", "noclip", "CAPI_Entity.SetImmuneToCollisions", "Fly through walls and terrain (turns fly on).");
    {
        static float fs = -1;
        if (fs < 0 || !ImGui::IsAnyItemActive()) fs = (float)atof(GetState("cheat.fly_speed", "12").c_str());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        ImGui::BeginDisabled(!LuaReady());
        ImGui::SliderFloat("fly speed", &fs, 2.0f, 60.0f, "%.0f m/s");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            char lua[64]; snprintf(lua, sizeof(lua), "FC4Cheats.setCfg('fly_speed', %.0f)", fs); Lua(lua, "=cheats");
            snprintf(lua, sizeof(lua), "%.0f", fs); SetState("cheat.fly_speed", lua);
        }
        ImGui::EndDisabled();
    }
    {
        bool freeze = GetState("cheat.fly_freeze", "0") == "1";
        ImGui::BeginDisabled(!LuaReady());
        if (ImGui::Checkbox("Freeze body while flying", &freeze)) {
            Lua(freeze ? "FC4Cheats.setCfg('fly_freeze', 1)" : "FC4Cheats.setCfg('fly_freeze', 0)", "=cheats");
            SetState("cheat.fly_freeze", freeze ? "1" : "0");
        }
        Help("Stops your arms/body animating while you fly (no flailing). Your gun won't animate either. Works straight away, also mid-flight.");
        ImGui::EndDisabled();
    }
    if (ActionButton("Launch up", "CAPI_Player.SetWorldSpeed")) Lua("FC4Cheats.launch(25)", "=cheats");
    Help("Throws you into the air. Pair it with No fall damage, or the wingsuit.");

    ImGui::SeparatorText("Camera & HUD");
    CheatSwitch("FOV", "fov", "CAPI_Player.SetPlayerFOV", "Your own field of view, past the game's menu limit. Uses the game's FOV override, so aiming and cutscenes still work.");
    ImGui::SameLine(ImGui::GetFontSize() * 14);
    CfgSlider("##fov", "cheat.fov", "90", 50, 150, "%.0f degrees", "FC4Cheats.setFov('fov', %.0f)");
    CheatSwitch("Weapon FOV", "fovnear", "CAPI_Player.SetPlayerNearFOV", "Field of view of your arms and gun (the game draws them with their own camera). Lower = gun looks bigger.");
    ImGui::SameLine(ImGui::GetFontSize() * 14);
    CfgSlider("##fovnear", "cheat.fov_near", "70", 30, 120, "%.0f degrees", "FC4Cheats.setFov('fovnear', %.0f)");
    {
        std::string gf = GetState("cheat.fov_game");
        if (!gf.empty()) ImGui::TextDisabled("Game FOV when you loaded in: %s degrees", gf.c_str());
    }
    CheatSwitch("Hide HUD", "hud", "SetHudVisible", "Hides the minimap, crosshair, prompts and meters. Bind a hotkey in Settings to flip it quickly.");

    ImGui::SeparatorText("Vehicles");
    CheatSwitch("Vehicle speed boost", "vboost", "CAPI_Vehicle.SetVehicleSpeed", "While you hold the throttle (W / right trigger) your vehicle speeds up faster, up to the top speed below. Let go or brake and it drives normally.");
    ImGui::SameLine(ImGui::GetFontSize() * 14);
    CfgSlider("##vboostx", "cheat.vboostx", "2", 1.25f, 6.0f, "%.2fx acceleration", "FC4Cheats.setVehicleCfg('vboostx', %.2f)");
    CfgSlider("top speed", "cheat.vmax", "250", 80, 2000, "%.0f km/h", "FC4Cheats.setVehicleCfg('vmax', %.0f)");
    ImGui::SameLine();
    if (ActionButton("Instant brake", "CAPI_Vehicle.SetVehicleSpeed")) Lua("FC4Cheats.brake()", "=cheats");
    Help("Stops your vehicle dead. Bind a hotkey for it in Settings.");

    ImGui::SeparatorText("Karma");
    {
        std::string k = GetState("cheat.karma");
        size_t bar = k.find('|');
        if (!k.empty() && bar != std::string::npos) ImGui::Text("Karma level %s   (%s karma points)", k.substr(0, bar).c_str(), k.substr(bar + 1).c_str());
        static int karma = 500;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::InputInt("##karma", &karma, 100, 1000);
        if (karma < 1) karma = 1;
        ImGui::SameLine();
        if (ActionButton("Give karma", "CAPI_Player.GivePlayerKarma")) { char lua[64]; snprintf(lua, sizeof(lua), "FC4Cheats.karma(%d)", karma); Lua(lua, "=cheats"); }
        Help("Karma levels unlock shop discounts and the karma rewards (like the Kyrat militia helping you).");
    }

    ImGui::SeparatorText("Money & progress");
    {
        static int money = 10000;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::InputInt("##money", &money, 1000, 10000);
        if (money < 0) money = 0;
        if (money > 9999999) money = 9999999;
        ImGui::SameLine();
        char lua[64];
        if (ActionButton("Set money", "CAPI_Player.SetPlayerMoney")) { snprintf(lua, sizeof(lua), "FC4Cheats.money('set', %d)", money); Lua(lua, "=cheats"); }
        ImGui::SameLine();
        if (ActionButton("Add money", "CAPI_Player.AddPlayerMoney")) { snprintf(lua, sizeof(lua), "FC4Cheats.money('add', %d)", money); Lua(lua, "=cheats"); }
        if (ActionButton("+1,000 XP", "CAPI_Player.GivePlayerXP")) Lua("FC4Cheats.xp(1000)", "=cheats"); ImGui::SameLine();
        if (ActionButton("+10,000 XP", "CAPI_Player.GivePlayerXP")) Lua("FC4Cheats.xp(10000)", "=cheats"); ImGui::SameLine();
        if (ActionButton("+1 skill point", "CAPI_Pawn.GiveSkillPoints")) Lua("FC4Cheats.skill(1)", "=cheats"); ImGui::SameLine();
        if (ActionButton("+5 skill points", "CAPI_Pawn.GiveSkillPoints")) Lua("FC4Cheats.skill(5)", "=cheats");
    }

    ImGui::SeparatorText("Slow motion");
    CheatSwitch("Slow motion##sw", "slowmo", "SetSlowMotion", "Slows the whole game down. Speed is set by the slider.");
    static float slow = -1;
    if (slow < 0 || !ImGui::IsAnyItemActive()) { std::string s = GetState("cheat.slow", "0.3"); slow = (float)atof(s.c_str()); }
    ImGui::SameLine(ImGui::GetFontSize() * 14);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    ImGui::BeginDisabled(!LuaReady() || !Avail("SetSlowMotion"));
    ImGui::SliderFloat("speed##slow", &slow, 0.05f, 1.0f, "%.2fx");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        char lua[64]; snprintf(lua, sizeof(lua), "FC4Cheats.setSlow(%.2f)", slow); Lua(lua, "=cheats");
        snprintf(lua, sizeof(lua), "%.2f", slow); SetState("cheat.slow", lua);
    }
    ImGui::EndDisabled();
    {
        bool sa = GetState("cheat.slow_audio", "1") != "0";
        ImGui::BeginDisabled(!LuaReady());
        if (ImGui::Checkbox("Slow the sound too", &sa)) {
            SetState("cheat.slow_audio", sa ? "1" : "0");
            Lua(sa ? "FC4Cheats.setSlowAudio(true)" : "FC4Cheats.setSlowAudio(false)", "=cheats");
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Sound and music play slower and deeper while slow motion is on.\n%d sound voices seen so far.", AudioVoiceCount());
    }

    ImGui::SeparatorText("Time of day");
    static int hour = -1, minute = 0;
    if (hour < 0 || !ImGui::IsAnyItemActive()) { hour = atoi(GetState("cheat.hour", "12").c_str()); minute = atoi(GetState("cheat.minute", "0").c_str()); }
    bool canTime = LuaReady() && Avail("SetTimeOfDay");
    ImGui::BeginDisabled(!canTime);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10); ImGui::SliderInt("hour", &hour, 0, 23, "%02d h");
    bool commit = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SameLine(); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10); ImGui::SliderInt("min", &minute, 0, 59, "%02d m");
    commit |= ImGui::IsItemDeactivatedAfterEdit();
    struct { const char* n; int h, m; } presets[] = { { "Dawn", 6, 0 }, { "Noon", 12, 0 }, { "Sunset", 18, 30 }, { "Night", 22, 0 }, { "Midnight", 0, 0 } };
    for (auto& p : presets) {
        if (&p != presets) ImGui::SameLine();
        if (ImGui::Button(p.n) && Debounced(p.n)) { hour = p.h; minute = p.m; commit = true; }
    }
    if (commit) {
        char lua[64]; snprintf(lua, sizeof(lua), "FC4Cheats.setTime(%d, %d)", hour, minute); Lua(lua, "=cheats");
        SetState("cheat.hour", std::to_string(hour)); SetState("cheat.minute", std::to_string(minute));
    }
    ImGui::EndDisabled();
    CheatSwitch("Lock time", "timelock", "SetTimeOfDay", "Keeps the clock at the time above (re-applied every few seconds).");

    ImGui::SeparatorText("Weather");
    if (ActionButton("Start storm", "StartStorm")) Lua("FC4Cheats.action('storm')", "=cheats"); ImGui::SameLine();
    if (ActionButton("Stop storm", "StopStorm")) Lua("FC4Cheats.action('calm')", "=cheats"); ImGui::SameLine();
    if (ActionButton("Full storm", "SetScriptedStormFactorOverride")) Lua("FC4Cheats.action('fullstorm')", "=cheats"); ImGui::SameLine();
    if (ActionButton("Clear override", "RemoveScriptedStormFactorOverride")) Lua("FC4Cheats.action('clearstorm')", "=cheats");
    if (ActionButton("Blizzard on", "SetBlizzardActive")) Lua("FC4Cheats.action('blizzard')", "=cheats"); ImGui::SameLine();
    if (ActionButton("Blizzard off", "SetBlizzardActive")) Lua("FC4Cheats.action('noblizzard')", "=cheats");
    Help("Blizzards only show up in the Himalayas (snow) areas.");

    ImGui::SeparatorText("Saved positions");
    ImGui::BeginDisabled(!LuaReady() || !Avail("teleport"));
    
    // 1. Read coordinates directly from disk to bypass memory state bridge issues
    std::map<int, std::string> diskPositions;
    std::wstring filePath = g_rootDir + L"positions.txt";
    FILE* posFile = _wfopen(filePath.c_str(), L"r");
    if (posFile) {
        char line[256];
        while (fgets(line, sizeof(line), posFile)) {
            int slotNum = 0;
            float fx = 0, fy = 0, fz = 0;
            // Parses the exact file format: slot1.000000=X,Y,Z
            if (sscanf(line, "slot%d.000000=%f,%f,%f", &slotNum, &fx, &fy, &fz) == 4) {
                char coordBuf[128];
                snprintf(coordBuf, sizeof(coordBuf), "%.2f,%.2f,%.2f", fx, fy, fz);
                diskPositions[slotNum] = coordBuf;
            }
        }
        fclose(posFile);
    }

    for (int i = 1; i <= 5; ++i) {
        ImGui::PushID(i);
        
        char lbl[64];
        if (ImGui::Button("Save here")) { 
            snprintf(lbl, sizeof(lbl), "FC4Cheats.savePos(%d)", i); 
            Lua(lbl, "=cheats"); 
        }
        ImGui::SameLine();
        
        // Check if this slot integer exists inside our file dictionary
        bool hasData = (diskPositions.count(i) > 0);
        
        ImGui::BeginDisabled(!hasData);
        if (ImGui::Button("Go")) { 
            snprintf(lbl, sizeof(lbl), "FC4Cheats.loadPos(%d)", i); 
            Lua(lbl, "=cheats"); 
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear")) { 
            snprintf(lbl, sizeof(lbl), "FC4Cheats.clearPos(%d)", i); 
            Lua(lbl, "=cheats"); 
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        
        float x = 0, y = 0, z = 0;
        if (hasData && sscanf(diskPositions[i].c_str(), "%f,%f,%f", &x, &y, &z) == 3) {
            ImGui::Text("Slot %d   x %6.0f   y %6.0f   height %4.0f", i, x, y, z);
        }
        else {
            ImGui::TextDisabled("Slot %d   (empty)", i);
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("World unlocks (permanent in your save)");
    ImGui::TextDisabled("These change your save game. Make a backup first if you care.");
    if (ActionButton("Unlock all safe houses", "UnlockAllSafeHouses")) Lua("FC4Cheats.action('safehouses')", "=cheats"); ImGui::SameLine();
    if (ActionButton("Enable fast travel", "SetFastTravelEnabled")) Lua("FC4Cheats.action('fasttravel')", "=cheats");
    if (ActionButton("Unlock all shop items", "SetShopItemsUnlockAll")) Lua("FC4Cheats.action('shop')", "=cheats"); ImGui::SameLine();
    if (ActionButton("Unlock all crafting", "SetCraftingUnlockAll")) Lua("FC4Cheats.action('crafting')", "=cheats");

    ImGui::SeparatorText("Outfit (experimental)");
    if (ActionButton("Costume on", "CAPI_Player.EquipCostume")) Lua("FC4Cheats.costume(true)", "=cheats"); ImGui::SameLine();
    if (ActionButton("Costume off", "CAPI_Player.EquipCostume")) Lua("FC4Cheats.costume(false)", "=cheats");
    Help("The game's own costume switch (EquipCostume). What it shows depends on your save and DLC.");

    ImGui::Separator();
    bool remember = GetState("cheat.remember", "1") == "1";
    ImGui::BeginDisabled(!LuaReady() || !ScriptsLoaded());
    if (ImGui::Checkbox("Remember switches after restarting the game", &remember))
        Lua(remember ? "FC4Cheats.setRemember(true)" : "FC4Cheats.setRemember(false)", "=cheats");
    ImGui::EndDisabled();
    ImGui::TextDisabled("Hotkeys for these can be set in Settings.");
}

// ------------------------------------------------------------ armory -----
struct Weapon { std::string name, id, cat, source; };
static std::vector<Weapon> s_weapons;
static std::vector<std::string> s_weaponCats;
static std::set<std::string> s_favs;
static bool s_armoryLoaded = false;

static std::string Trim(const std::string& x) {
    size_t a = x.find_first_not_of(" \t\r\n"), b = x.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : x.substr(a, b - a + 1);
}

static void ReadArmoryFile(const std::wstring& path, const std::string& defCat, const std::string& source) {
    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f) return;
    std::string cat = defCat;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        std::string l = Trim(line);
        if (l.size() >= 3 && (uint8_t)l[0] == 0xEF) l = Trim(l.substr(3));     // UTF-8 BOM
        if (l.empty() || l[0] == '#' || l[0] == ';') continue;
        if (l[0] == '[' && l.back() == ']') { cat = Trim(l.substr(1, l.size() - 2)); continue; }
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        Weapon w{ Trim(l.substr(0, eq)), Trim(l.substr(eq + 1)), cat, source };
        if (w.name.empty() || w.id.empty() || w.id.find_first_not_of("0123456789") != std::string::npos) {
            LogL(LOG_WARN, "armory: ignored line in %s: %s", Narrow(path).c_str(), l.c_str());
            continue;
        }
        bool dup = false;
        for (auto& e : s_weapons) if (e.id == w.id) { dup = true; break; }
        if (dup) continue;
        s_weapons.push_back(w); ++n;
        if (std::find(s_weaponCats.begin(), s_weaponCats.end(), cat) == s_weaponCats.end()) s_weaponCats.push_back(cat);
    }
    fclose(f);
    LogL(LOG_DEBUG, "armory: %d weapons from %s", n, Narrow(path).c_str());
}

static void LoadFavourites() {
    s_favs.clear();
    FILE* f = _wfopen((g_rootDir + L"armory\\favourites.txt").c_str(), L"r");
    char line[64];
    while (f && fgets(line, sizeof(line), f)) { std::string l = Trim(line); if (!l.empty()) s_favs.insert(l); }
    if (f) fclose(f);
}
static void SaveFavourites() {
    FILE* f = _wfopen((g_rootDir + L"armory\\favourites.txt").c_str(), L"w");
    if (!f) return;
    for (auto& id : s_favs) fprintf(f, "%s\n", id.c_str());
    fclose(f);
}

static void EnsureCustomList() {
    std::wstring p = g_rootDir + L"armory\\custom.txt";
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    if (FILE* f = _wfopen(p.c_str(), L"wb")) {
        fputs("# Your own Armory entries (this file is never replaced by updates). Same format as weapons.txt:\r\n"
              "#\r\n#   [My weapons]\r\n#   Display name = template id\r\n#\r\n"
              "# Template ids are the weapon's archetype id (arkArchetype in ShoppingItems.ndb, or the\r\n"
              "# .ark.fcb file name in entityarchetypeslibrary). Lines starting with # are ignored.\r\n", f);
        fclose(f);
    }
}

static void LoadArmory() {
    EnsureCustomList();
    s_weapons.clear(); s_weaponCats.clear();
    ReadArmoryFile(g_rootDir + L"armory\\weapons.txt", "Weapons", "FC4Hook");
    ReadArmoryFile(g_rootDir + L"armory\\custom.txt", "My weapons", "custom.txt");
    // Mods add their own entries with fc4hook\mods\<ModName>\armory.txt
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_rootDir + L"mods\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            std::string mod = Narrow(fd.cFileName);
            if (!VfsModEnabled(mod)) continue;
            ReadArmoryFile(g_rootDir + L"mods\\" + fd.cFileName + L"\\armory.txt", mod, mod);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    LoadFavourites();
    s_armoryLoaded = true;
    Log("armory: %d weapons in %d categories", (int)s_weapons.size(), (int)s_weaponCats.size());
}

static void GiveLua(std::string& lua, const Weapon& w) {
    lua += "FC4Armory.give('" + w.id + "', [[" + LuaSafe(w.name) + "]])\n";
}

static void WeaponRow(const Weapon& w, int idx) {
    ImGui::PushID(idx);
    bool fav = s_favs.count(w.id) != 0;
    ImGui::PushStyleColor(ImGuiCol_Text, fav ? ImVec4(1, 0.85f, 0.2f, 1) : ImVec4(0.5f, 0.5f, 0.5f, 1));
    if (ImGui::SmallButton(fav ? "*##fav" : "+##fav")) { if (fav) s_favs.erase(w.id); else s_favs.insert(w.id); SaveFavourites(); }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(fav ? "Remove from favourites" : "Add to favourites");
    ImGui::SameLine();
    char lbl[48]; snprintf(lbl, sizeof(lbl), "give%s", w.id.c_str());
    ImGui::BeginDisabled(!LuaReady());
    if (ImGui::Button("Give") && Debounced(lbl)) { std::string lua; GiveLua(lua, w); Lua(lua.c_str(), "=armory"); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextUnformatted(w.name.c_str());
    std::string st = GetState("armory." + w.id);
    if (!st.empty()) {
        ImGui::SameLine();
        ImVec4 c = st == "ok" ? ImVec4(0.4f, 1, 0.4f, 1) : st == "failed" ? ImVec4(1, 0.4f, 0.4f, 1) : ImVec4(1, 0.8f, 0.3f, 1);
        ImGui::TextColored(c, "%s", st == "ok" ? "given" : st.c_str());
    }
    if (w.source != "FC4Hook") { ImGui::SameLine(); ImGui::TextDisabled("[%s]", w.source.c_str()); }
    ImGui::PopID();
}

static void GiveAll(const std::vector<const Weapon*>& list) {
    std::string lua;
    for (auto* w : list) GiveLua(lua, *w);
    if (!lua.empty()) Lua(lua.c_str(), "=armory");
}

// Attachments: the list comes live from the gun in your hands (Lua 5.4 module
// fc4hook\lua\autorun\10_attachments.lua publishes att.* state).
static void TabAttachments() {
    static ULONGLONG lastAsk = 0;
    SetState("att.watch", "1");                       // the module re-scans when you switch guns
    if (GetTickCount64() - lastAsk > 5000) { lastAsk = GetTickCount64(); Lua54Queue("if FC4A then FC4A.scan() end", "=attach"); }
    NeedWorldNote();
    ImGui::TextWrapped("Shows the attachments the gun in your hands supports. Switch guns and the list follows.");
    ImGui::BeginDisabled(!LuaReady());
    if (ImGui::Button("Refresh") && Debounced("attscan")) Lua54Queue("FC4A.scan()", "=attach");
    ImGui::SameLine();
    if (ImGui::Button("Remove all") && Debounced("attclear")) Lua54Queue("FC4A.clear()", "=attach");
    ImGui::EndDisabled();
    std::string status = GetState("att.status");
    if (!status.empty()) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", status.c_str());
    int n = atoi(GetState("att.n", "0").c_str());
    ImGui::BeginChild("att", ImVec2(0, 0), true);
    for (int i = 1; i <= n; ++i) {
        std::string row = GetState("att." + std::to_string(i));
        size_t bar = row.rfind('|');
        std::string label = bar == std::string::npos ? row : row.substr(0, bar);
        bool on = bar != std::string::npos && row.substr(bar + 1) == "1";
        ImGui::PushID(i);
        ImGui::BeginDisabled(on || !LuaReady());
        char dbg[32]; snprintf(dbg, sizeof(dbg), "fit%d", i);
        if (ImGui::Button("Fit") && Debounced(dbg)) { char lua[48]; snprintf(lua, sizeof(lua), "FC4A.fit(%d)", i); Lua54Queue(lua, "=attach"); }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (on) ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1), "%s  (fitted)", label.c_str());
        else ImGui::TextUnformatted(label.c_str());
        ImGui::PopID();
    }
    if (n == 0 && status.empty()) ImGui::TextDisabled("Hold a gun, then press Refresh.");
    ImGui::EndChild();
}

static void TabWeapons();
static void TabArmory() {
    if (ImGui::BeginTabBar("armorytabs")) {
        if (ImGui::BeginTabItem("Weapons")) { TabWeapons(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Attachments")) { TabAttachments(); ImGui::EndTabItem(); }
        else SetState("att.watch", "0");
        ImGui::EndTabBar();
    }
}

static void TabWeapons() {
    if (!s_armoryLoaded) LoadArmory();
    NeedWorldNote();
    static char filter[64] = "";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
    ImGui::InputTextWithHint("##wf", "search weapons...", filter, sizeof(filter));
    ImGui::SameLine();
    if (ImGui::Button("Reload list")) LoadArmory();
    ImGui::SameLine();
    static bool refill = true;
    if (ImGui::Checkbox("Refill ammo after", &refill) && LuaReady())
        Lua(refill ? "FC4Armory.refillAfter = true" : "FC4Armory.refillAfter = false", "=armory");
    int queued = atoi(GetState("armory.queue", "0").c_str());
    if (queued > 0) {
        ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%d queued", queued);
        ImGui::SameLine(); if (ImGui::SmallButton("Cancel")) Lua("FC4Armory.clear()", "=armory");
    }
    ImGui::TextDisabled("Weapons go straight into your weapon wheel. Your own list: fc4hook\\armory\\custom.txt");

    std::string flt = Lower(filter);
    ImGui::BeginChild("weapons", ImVec2(0, 0), true);
    int idx = 0;
    // favourites first
    std::vector<const Weapon*> favs;
    for (auto& w : s_weapons) if (s_favs.count(w.id) && (flt.empty() || Lower(w.name).find(flt) != std::string::npos)) favs.push_back(&w);
    if (!favs.empty()) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (ImGui::CollapsingHeader("Favourites")) {
            if (ImGui::SmallButton("Give all favourites") && Debounced("givefav")) GiveAll(favs);
            for (auto* w : favs) WeaponRow(*w, idx++);
        }
    }
    for (auto& cat : s_weaponCats) {
        std::vector<const Weapon*> list;
        for (auto& w : s_weapons)
            if (w.cat == cat && (flt.empty() || Lower(w.name).find(flt) != std::string::npos || Lower(cat).find(flt) != std::string::npos))
                list.push_back(&w);
        if (list.empty()) continue;
        if (!flt.empty()) ImGui::SetNextItemOpen(true);
        char hdr[128]; snprintf(hdr, sizeof(hdr), "%s  (%d)###cat_%s", cat.c_str(), (int)list.size(), cat.c_str());
        if (!ImGui::CollapsingHeader(hdr)) { idx += (int)list.size(); continue; }
        ImGui::PushID(cat.c_str());
        ImGui::BeginDisabled(!LuaReady());
        if (ImGui::SmallButton("Give whole category") && Debounced(("all" + cat).c_str())) GiveAll(list);
        ImGui::EndDisabled();
        if (cat.find("experimental") != std::string::npos)
            ImGui::TextDisabled("These are thrown items rather than guns; the game may only add them as ammo, or refuse.");
        for (auto* w : list) WeaponRow(*w, idx++);
        ImGui::PopID();
    }
    if (s_weapons.empty()) ImGui::TextDisabled("No weapon lists found: is fc4hook\\armory\\weapons.txt installed?");
    ImGui::EndChild();
}

// ------------------------------------------------------------ travel -----
static void TabTravel() {
    static bool asked = false;
    if (!asked && LuaReady()) {
        asked = true;
        Lua("if FC4Travel then FC4Travel.refresh() end if FC4Teleports and #FC4Teleports.list == 0 then FC4Teleports.refresh() end", "=travel");
    }
    NeedWorldNote();
    ImGui::TextWrapped("Jump to any bell tower you've liberated. You're dropped just beside it with "
                       "god mode on until you land.");
    ImGui::BeginDisabled(!LuaReady());
    if (ImGui::Button("Refresh list") && Debounced("trefresh")) Lua("FC4Travel.refresh()", "=travel");
    ImGui::SameLine();
    if (ImGui::Button("Back to where I was") && Debounced("tback")) Lua("FC4Travel.back()", "=travel");
    ImGui::SameLine();
    if (ImGui::Button("Log my position") && Debounced("tpos")) Lua("FC4Travel.logPos()", "=travel");
    ImGui::SameLine();
    if (ImGui::Button("Cancel travel") && Debounced("tcancel")) Lua("FC4Travel.cancel()", "=travel");
    Help("Stops a jump or tour that's still in progress and turns travel's god mode off.");
    if (ImGui::Button("Save arrival spot here") && Debounced("tsave")) Lua("FC4Travel.saveSpot()", "=travel");
    ImGui::SameLine();
    if (ImGui::Button("Forget spot (nearest tower)") && Debounced("tforget")) Lua("FC4Travel.forgetSpot()", "=travel");
    Help("Stand where you'd like to arrive at a tower and save; other towers copy its height and distance.");
    ImGui::SeparatorText("Outposts & safe houses");
    if (ImGui::Button("Refresh##tp") && Debounced("tprefresh")) Lua("FC4Teleports.refresh()", "=travel");
    ImGui::SameLine();
    if (ActionButton("Teleport to current objective", "Game.TeleportToCurrentObjective")) Lua("FC4Cheats.objective()", "=travel");
    {
        std::string list = GetState("tp.list");
        static char tflt[48] = "";
        ImGui::SameLine(); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
        ImGui::InputTextWithHint("##tflt", "filter...", tflt, sizeof(tflt));
        std::string f = Lower(tflt);
        ImGui::BeginChild("tplist", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 8), true);
        int i = 0; size_t pos = 0;
        while (pos < list.size()) {
            size_t nl = list.find('\n', pos); if (nl == std::string::npos) nl = list.size();
            std::string row = list.substr(pos, nl - pos); pos = nl + 1; ++i;
            char kind[16] = "", label[96] = ""; float x = 0, y = 0, z = 0;
            if (sscanf(row.c_str(), "%15[^|]|%95[^|]|%f|%f|%f", kind, label, &x, &y, &z) < 5) continue;
            if (!f.empty() && Lower(label).find(f) == std::string::npos) continue;
            ImGui::PushID(i);
            char lbl[32]; snprintf(lbl, sizeof(lbl), "tp%d", i);
            if (ImGui::Button("Go") && Debounced(lbl)) { char lua[64]; snprintf(lua, sizeof(lua), "FC4Teleports.go(%d)", i); Lua(lua, "=travel"); }
            ImGui::SameLine();
            ImGui::TextColored(strcmp(kind, "outpost") == 0 ? ImVec4(1, 0.7f, 0.4f, 1) : ImVec4(0.6f, 0.85f, 1, 1), "%-10s", kind);
            ImGui::SameLine(); ImGui::Text("%-24s x %6.0f  y %6.0f", label, x, y);
            ImGui::PopID();
        }
        if (list.empty()) ImGui::TextDisabled("Load into the world, then Refresh.");
        ImGui::EndChild();
    }
    ImGui::SeparatorText("Liberated bell towers");
    auto spots = TravelList();
    if (spots.empty()) ImGui::TextDisabled("None yet: load into the world and click Refresh list.");
    ImGui::BeginChild("towers", ImVec2(0, 0), true);
    for (size_t i = 0; i < spots.size(); ++i) {
        auto& s = spots[i];
        ImGui::PushID((int)i);
        char lbl[32]; snprintf(lbl, sizeof(lbl), "go%d", (int)i);
        if (ImGui::Button("Go") && Debounced(lbl)) {
            char lua[96]; snprintf(lua, sizeof(lua), "FC4Travel.go('%s')", s.id);
            Lua(lua, "=travel");
        }
        ImGui::SameLine();
        ImGui::Text("Bell tower %2d   x %7.0f   y %7.0f   height %4.0f", (int)i + 1, s.x, s.y, s.z);
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::EndDisabled();
}

// ------------------------------------------------------------ co-op ------
static void RunScriptWithPrelude(const std::wstring& name, const std::string& prelude);

static void TabCoop() {
    NeedWorldNote();
    ImGui::TextWrapped("Far Cry 4 tells a co-op guest that towers are liberated but never gives them the tower "
                       "rewards (the free weapons). FC4Hook fixes that automatically for guests every 15 seconds; "
                       "these buttons do it on demand. Both players need FC4Hook for co-op features.");
    ImGui::BeginDisabled(!LuaReady());
    ImGui::SeparatorText("Guest");
    if (ImGui::Button("Claim missing tower rewards") && Debounced("claim")) RunScriptWithPrelude(L"coop\\tower_rewards.lua", "TOWER_MODE='claim'");
    ImGui::TextDisabled("Grants every reward it knows about for all liberated towers. Safe to press again.");
    ImGui::SeparatorText("Host / solo");
    if (ImGui::Button("Mark my towers as already claimed") && Debounced("mark")) RunScriptWithPrelude(L"coop\\tower_rewards.lua", "TOWER_MODE='mark'");
    ImGui::TextDisabled("Run once on a save that earned its towers normally.");
    if (ImGui::Button("Record reward lists of nearby towers") && Debounced("learn")) RunScriptWithPrelude(L"coop\\tower_rewards.lua", "TOWER_MODE='learn'");
    ImGui::TextDisabled("Saves each loaded tower's reward list to coop\\tower_reward_table.txt (shareable).");
    if (ImGui::Button("Tour towers (record all reward lists)") && Debounced("ttour")) Lua("FC4Travel.tourStart()", "=travel");
    ImGui::SameLine();
    if (ImGui::Button("Stop tour") && Debounced("tstop")) Lua("FC4Travel.tourStop()", "=travel");
    ImGui::TextDisabled("Visits each liberated tower missing from the reward table, then brings you back.");
    ImGui::EndDisabled();
}

// ------------------------------------------------------------ console ----
static int s_interp = 0;     // console interpreter: 0 game Lua 5.1, 1 Lua 5.4
static std::vector<std::string> s_history;
static int s_histPos = -1;

static int HistoryCb(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag != ImGuiInputTextFlags_CallbackHistory || s_history.empty()) return 0;
    int prev = s_histPos;
    if (d->EventKey == ImGuiKey_UpArrow) s_histPos = s_histPos < 0 ? (int)s_history.size() - 1 : std::max(0, s_histPos - 1);
    else if (d->EventKey == ImGuiKey_DownArrow && s_histPos >= 0) s_histPos = s_histPos + 1 >= (int)s_history.size() ? -1 : s_histPos + 1;
    if (prev != s_histPos) {
        d->DeleteChars(0, d->BufTextLen);
        if (s_histPos >= 0) d->InsertChars(0, s_history[s_histPos].c_str());
    }
    return 0;
}

static void RunLua(const std::string& code) {
    if (code.empty()) return;
    LogL(LOG_INFO, "%s %s", s_interp ? "5.4>" : ">", code.c_str());
    if (s_interp == 1) {
        Lua54Queue(code, "=console");
        if (s_history.empty() || s_history.back() != code) s_history.push_back(code);
        s_histPos = -1;
        return;
    }
    // "=expr" prints the value, like the stock Lua console
    if (code[0] == '=') QueueLua("ModLog(tostring(" + code.substr(1) + "))", "=console");
    else QueueLua(code, "=console");
    if (s_history.empty() || s_history.back() != code) s_history.push_back(code);
    if (s_history.size() > 100) s_history.erase(s_history.begin());
    s_histPos = -1;
}

static void LogView(float height) {
    static bool show[4] = { false, true, true, true };
    static bool autoscroll = true;
    static char filter[96] = "";
    static std::vector<LogLine> lines; static uint64_t serial = ~0ull;
    if (LogSerial() != serial) { serial = LogSerial(); lines = LogSnapshot(); }
    ImGui::Checkbox("Debug", &show[0]); ImGui::SameLine(); ImGui::Checkbox("Info", &show[1]); ImGui::SameLine();
    ImGui::Checkbox("Warnings", &show[2]); ImGui::SameLine(); ImGui::Checkbox("Errors", &show[3]); ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10); ImGui::InputTextWithHint("##lf", "filter...", filter, sizeof(filter)); ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &autoscroll); ImGui::SameLine();
    if (ImGui::Button("Clear")) LogClear(); ImGui::SameLine();
    bool copy = ImGui::Button("Copy"); ImGui::SameLine();
    if (ImGui::Button("Open folder")) OpenFolder(g_rootDir);
    if (g_cfg.logLevel > LOG_DEBUG && show[0]) ImGui::TextDisabled("Debug lines are only recorded when Settings > Log level is Debug.");

    std::string flt = Lower(filter);
    std::vector<int> vis;
    vis.reserve(lines.size());
    for (int i = 0; i < (int)lines.size(); ++i) {
        if (!show[lines[i].level & 3]) continue;
        if (!flt.empty() && Lower(lines[i].text).find(flt) == std::string::npos) continue;
        vis.push_back(i);
    }
    if (copy) {
        std::string all;
        for (int i : vis) { all += lines[i].text; all += "\n"; }
        ImGui::SetClipboardText(all.c_str());
        Log("copied %d log lines to the clipboard", (int)vis.size());
    }
    ImGui::BeginChild("log", ImVec2(0, height), true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGuiListClipper clip;
    clip.Begin((int)vis.size());
    while (clip.Step()) {
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
            const LogLine& l = lines[vis[r]];
            ImVec4 c = l.level == LOG_ERROR ? ImVec4(1, 0.4f, 0.4f, 1) : l.level == LOG_WARN ? ImVec4(1, 0.85f, 0.35f, 1)
                     : l.level == LOG_DEBUG ? ImVec4(0.55f, 0.55f, 0.55f, 1)
                     : l.text.find("[lua]") != std::string::npos ? ImVec4(0.6f, 0.85f, 1, 1) : ImVec4(0.9f, 0.9f, 0.9f, 1);
            ImGui::PushStyleColor(ImGuiCol_Text, c);
            ImGui::TextUnformatted(l.text.c_str());
            ImGui::PopStyleColor();
        }
    }
    if (autoscroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

static void TabConsole() {
    LogView(-ImGui::GetFrameHeightWithSpacing() * 1.2f);
    static char input[1024] = "";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    const char* interps[] = { "Game Lua", "Lua 5.4" };
    ImGui::Combo("##interp", &s_interp, interps, 2);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Game Lua: the game's own Lua 5.1 (all game functions directly).\nLua 5.4: FC4Hook's Lua 5.4 (64-bit numbers; game.*, mem.*, fc4.*).");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 4);
    bool enter = ImGui::InputTextWithHint("##lua", "Lua (Enter to run, Up/Down for history, =expr to print a value)", input, sizeof(input),
                                          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory, HistoryCb);
    ImGui::SameLine();
    if ((ImGui::Button("Run", ImVec2(-1, 0)) || enter) && input[0]) {
        RunLua(input); input[0] = 0;
        ImGui::SetKeyboardFocusHere(-1);
    }
}

// ------------------------------------------------------------ settings ---
static void KeyBind(const char* label, int* key, int slot) {
    ImGui::PushID(slot);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetFontSize() * 11);
    if (s_captureKey == slot) {
        ImGui::Button("press a key... (Esc = cancel, Backspace = none)", ImVec2(ImGui::GetFontSize() * 22, 0));
        for (int vk = 8; vk < 255; ++vk) {
            if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || (vk >= VK_SHIFT && vk <= VK_MENU) ||
                (vk >= VK_LSHIFT && vk <= VK_RMENU) || vk == VK_LWIN || vk == VK_RWIN) continue;
            if (!(GetAsyncKeyState(vk) & 0x8000)) continue;
            if (vk == VK_ESCAPE) { s_captureKey = 0; break; }
            if (vk == VK_BACK) { if (slot != 1) *key = 0; s_captureKey = 0; SaveConfig(); break; }
            if (vk == VK_RETURN || vk == VK_SPACE) continue;
            *key = vk; s_captureKey = 0; SaveConfig();
            KeyPressed(vk);                     // swallow this press so it doesn't fire the action
            Log("settings: %s = %s", label, KeyName(vk).c_str());
            break;
        }
    } else {
        std::string name = KeyName(*key);
        if (ImGui::Button(name.c_str(), ImVec2(ImGui::GetFontSize() * 8, 0))) s_captureKey = slot;
    }
    ImGui::PopID();
}

static void TabSettings() {
    ImGui::SeparatorText("Keys");
    KeyBind("Open menu", &g_cfg.menuKey, 1);
    KeyBind("God mode", &g_cfg.hkGodMode, 2);
    KeyBind("Unlimited ammo", &g_cfg.hkAmmo, 3);
    KeyBind("Refill ammo", &g_cfg.hkRefill, 4);
    KeyBind("Slow motion", &g_cfg.hkSlowMo, 5);
    KeyBind("Open Armory", &g_cfg.hkMenuArmory, 6);
    KeyBind("Fly", &g_cfg.hkFly, 7);
    KeyBind("Hide HUD", &g_cfg.hkHud, 8);
    KeyBind("Instant brake", &g_cfg.hkBrake, 9);
    if (ImGui::Checkbox("Gamepad: hold L3 + R3 to open/close the menu", &g_cfg.padMenu)) SaveConfig();
    Help("Press both thumbsticks in for half a second. In the menu: D-pad / left stick to move between buttons and tabs, "
         "A to press, B to go back. While the menu is open the game doesn't get your pad.");
    ImGui::TextDisabled("Click a key to change it. Hotkeys work while you play (menu closed or open).");

    ImGui::SeparatorText("Menu");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::SliderFloat("Menu size", &g_cfg.uiScale, 0.75f, 2.0f, "%.2fx");
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveConfig();
    if (ImGui::Checkbox("Show the \"FC4Hook loaded\" banner at startup", &g_cfg.showBanner)) SaveConfig();
    if (ImGui::Checkbox("Developer mode (adds the Developer tab)", &g_cfg.developerMode)) SaveConfig();

    ImGui::SeparatorText("Log");
    const char* levels[] = { "Debug (everything)", "Info", "Warnings", "Errors only" };
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    if (ImGui::Combo("Log level", &g_cfg.logLevel, levels, 4)) { SaveConfig(); Log("log level set to %s", LevelName(g_cfg.logLevel)); }
    if (ImGui::Checkbox("Keep the previous session's log (fc4hook.prev.log)", &g_cfg.keepPrevLog)) SaveConfig();
    if (ImGui::Checkbox("Write crash reports (fc4hook\\crashes\\)", &g_cfg.crashReports)) SaveConfig();
    Help("If the game crashes, FC4Hook writes what it knows (where, which module, last log lines). "
         "Attach that file when you report a bug.");
    if (ImGui::Button("Open FC4Hook folder")) OpenFolder(g_rootDir);
    ImGui::SameLine();
    if (ImGui::Button("Open crash reports")) OpenFolder(g_rootDir + L"crashes");

    ImGui::SeparatorText("Scripts");
    if (ImGui::Button("Reload scripts")) { RequestAutorunReload(); Log("reloading scripts\\autorun"); }
    Help("Re-runs everything in fc4hook\\scripts\\autorun\\. Use after editing a script.");
    ImGui::TextDisabled("Settings are saved to fc4hook\\fc4hook.ini as you change them.");
}

// ------------------------------------------------------------ developer --
static std::vector<std::string> s_natives;

static void LoadNativesFile() {
    s_natives.clear();
    FILE* f = _wfopen((g_rootDir + L"dumps\\natives.txt").c_str(), L"r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (n) s_natives.emplace_back(line);
    }
    fclose(f);
}

static std::vector<std::wstring> ListScripts() {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_rootDir + L"scripts\\*.lua").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do { out.push_back(fd.cFileName); } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

static void RunScriptWithPrelude(const std::wstring& name, const std::string& prelude) {
    FILE* f = _wfopen((g_rootDir + L"scripts\\" + name).c_str(), L"rb");
    if (!f) { LogL(LOG_ERROR, "cannot open script %s", Narrow(name).c_str()); return; }
    std::string code = prelude.empty() ? std::string() : prelude + "\n";
    char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) code.append(buf, n);
    fclose(f);
    Log("> run script %s", Narrow(name).c_str());
    QueueLua(code, "@" + Narrow(name));
}

// Calls a game native inside pcall and logs exactly what came back (values or error).
static void ProbeNative(const std::string& fn, const std::string& args) {
    std::string lua =
        "local f = FC4 and FC4.fn('" + fn + "')\n"
        "if not f then ModLogWarn('" + fn + " is not available') return end\n"
        "local r = { pcall(f" + (args.empty() ? "" : ", " + args) + ") }\n"
        "local out = {}\n"
        "for i = 2, select('#', unpack(r)) do\n"
        "  local v = r[i]\n"
        "  if type(v) == 'table' then\n"
        "    local parts = {}\n"
        "    for k, x in pairs(v) do parts[#parts+1] = tostring(k) .. '=' .. tostring(x) end\n"
        "    out[#out+1] = '{' .. table.concat(parts, ', ') .. '}'\n"
        "  else out[#out+1] = type(v) .. ':' .. tostring(v) end\n"
        "end\n"
        "ModLog('" + fn + "(" + LuaSafe(args) + ") -> ' .. (r[1] and 'ok ' or 'ERROR ') .. table.concat(out, ', '))\n";
    Log("> %s(%s)", fn.c_str(), args.c_str());
    QueueLua(lua, "=" + fn);
}

// ------------------------------------------------------------ memory -----
struct Watch { std::string expr, type, freezeVal; bool frozen = false; };
static std::vector<Watch> s_watches;
static bool s_watchLoaded = false;
static const char* kMemTypes[] = { "u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64", "f32", "f64", "ptr", "bool" };

static void SaveWatches() {
    FILE* f = _wfopen((g_rootDir + L"memory_watch.txt").c_str(), L"w");
    if (!f) return;
    for (auto& w : s_watches) fprintf(f, "%s|%s\n", w.type.c_str(), w.expr.c_str());
    fclose(f);
}
static void LoadWatches() {
    s_watchLoaded = true;
    FILE* f = _wfopen((g_rootDir + L"memory_watch.txt").c_str(), L"r");
    char line[512];
    while (f && fgets(line, sizeof(line), f)) {
        std::string l = Trim(line); size_t bar = l.find('|');
        if (bar == std::string::npos) continue;
        Watch w; w.type = l.substr(0, bar); w.expr = l.substr(bar + 1);
        s_watches.push_back(w);
    }
    if (f) fclose(f);
}

static void TypeCombo(const char* id, int* t) {
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
    ImGui::Combo(id, t, kMemTypes, IM_ARRAYSIZE(kMemTypes));
}

static void TabMemory() {
    if (!s_watchLoaded) LoadWatches();
    static char expr[256] = "FC64";
    static int type = 4;
    static char writeVal[64] = "";
    static std::string msg;
    ImGui::TextWrapped("Addresses use Cheat Engine syntax: numbers are hex, FC64+1234 = module + offset, "
                       "[x]+8 = read the pointer at x, then add 8. Lua scripts get the same through Mem.* "
                       "(see scripts\\autorun\\05_memory.lua).");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20);
    ImGui::InputTextWithHint("##addr", "address, e.g. [FC64+2DD8958]+8", expr, sizeof(expr));
    ImGui::SameLine(); TypeCombo("##type", &type);
    ImGui::SameLine();
    ImGui::BeginDisabled(!LuaReady());
    if (ImGui::Button("My player") && Debounced("mplayer"))
        Lua("ModSetState('mem.player', (Mem and Mem.player()) or ('none: ' .. tostring(MemError and MemError())))", "=mem");
    ImGui::EndDisabled();
    std::string pl = GetState("mem.player");
    if (!pl.empty()) { if (pl.rfind("0x", 0) == 0) snprintf(expr, sizeof(expr), "%s", pl.c_str()); else msg = "Player object " + pl; SetState("mem.player", ""); }

    uintptr_t a = 0; std::string err;
    bool ok = ParseAddress(expr, &a, &err);
    if (ok) {
        ImGui::Text("Address %s   value (%s): %s", Hex(a).c_str(), kMemTypes[type], MemFormat(a, kMemTypes[type]).c_str());
        if (!MemReadable(a, 1)) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "not readable");
    } else {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", err.c_str());
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    ImGui::InputTextWithHint("##wv", "new value", writeVal, sizeof(writeVal));
    ImGui::SameLine();
    ImGui::BeginDisabled(!ok);
    if (ImGui::Button("Write")) {
        std::string e;
        msg = MemWriteValue(a, kMemTypes[type], writeVal, &e) ? "Wrote " + std::string(writeVal) + " to " + Hex(a) : "Write failed: " + e;
        Log("memory: %s", msg.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Add to watch list")) { Watch w; w.expr = expr; w.type = kMemTypes[type]; s_watches.push_back(w); SaveWatches(); }
    ImGui::SameLine();
    if (ImGui::Button("Copy address")) ImGui::SetClipboardText(Hex(a).c_str());
    ImGui::EndDisabled();
    if (!msg.empty()) ImGui::TextDisabled("%s", msg.c_str());

    ImGui::SeparatorText("Watch list");
    int remove = -1;
    if (ImGui::BeginTable("watch", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Address expression"); ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3);
        ImGui::TableSetupColumn("Value"); ImGui::TableSetupColumn("Freeze at"); ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 2);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < s_watches.size(); ++i) {
            Watch& w = s_watches[i];
            ImGui::PushID((int)i);
            uintptr_t wa = 0; std::string we;
            bool wok = ParseAddress(w.expr, &wa, &we);
            if (wok && w.frozen) { std::string e2; MemWriteValue(wa, w.type.c_str(), w.freezeVal.c_str(), &e2); }
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(w.expr.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", wok ? Hex(wa).c_str() : we.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(w.type.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(wok ? MemFormat(wa, w.type.c_str()).c_str() : "??");
            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##fz", &w.frozen) && w.frozen && w.freezeVal.empty() && wok) {
                w.freezeVal = MemFormat(wa, w.type.c_str());
                if (w.freezeVal == "??") w.frozen = false;
            }
            ImGui::SameLine();
            char fv[64]; snprintf(fv, sizeof(fv), "%s", w.freezeVal.c_str());
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##fv", fv, sizeof(fv))) w.freezeVal = fv;
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("x")) remove = (int)i;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (remove >= 0) { s_watches.erase(s_watches.begin() + remove); SaveWatches(); }
    if (s_watches.empty()) ImGui::TextDisabled("Empty. Tick a row's box to freeze it at the value next to it (re-written every frame).");

    ImGui::SeparatorText("Hex view");
    static uintptr_t viewOff = 0;
    if (ImGui::SmallButton("-100")) viewOff -= 0x100; ImGui::SameLine();
    if (ImGui::SmallButton("-10")) viewOff -= 0x10; ImGui::SameLine();
    if (ImGui::SmallButton("reset")) viewOff = 0; ImGui::SameLine();
    if (ImGui::SmallButton("+10")) viewOff += 0x10; ImGui::SameLine();
    if (ImGui::SmallButton("+100")) viewOff += 0x100;
    ImGui::BeginChild("hex", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    if (ok) {
        for (int r = 0; r < 32; ++r) {
            uintptr_t row = a + viewOff + r * 16;
            char line[128]; int k = snprintf(line, sizeof(line), "%016llX  ", (unsigned long long)row);
            char asc[17] = {};
            for (int j = 0; j < 16; ++j) {
                if (MemReadable(row + j, 1)) { uint8_t c = *(uint8_t*)(row + j); k += snprintf(line + k, sizeof(line) - k, "%02X ", c); asc[j] = (c >= 32 && c < 127) ? (char)c : '.'; }
                else { k += snprintf(line + k, sizeof(line) - k, "?? "); asc[j] = ' '; }
            }
            snprintf(line + k, sizeof(line) - k, " %s", asc);
            ImGui::TextUnformatted(line);
        }
    }
    ImGui::EndChild();
}

// ------------------------------------------------------------- mods ------
static void TabMods() {
    ImGui::TextWrapped("Mods live in fc4hook\\mods\\<ModName>\\. Put game files in its files\\ folder with their "
                       "path inside the game's data (e.g. files\\graphics\\_textures\\foo.xbt): FC4Hook packs them at startup and "
                       "the game loads them instead of its own. New files (new archetypes, textures, animation packages) work too.");
    ImGui::Text("Loose files: %s", VfsStatus().c_str());
    ImGui::Text("Mod archive (data_win32\\fc4hook_mods.dat): %s", ModArchiveStatus());
    if (ImGui::Button("Open mods folder")) { CreateDirectoryW((g_rootDir + L"mods").c_str(), nullptr); OpenFolder(g_rootDir + L"mods"); }
    ImGui::SameLine();
    Help("Changes here take effect the next time you start the game. Load order: alphabetical, or the order of "
         "mods\\load_order.txt (one folder per line); when two mods ship the same file, the later one wins. "
         "Folders starting with _ are always off.");
    static std::vector<VfsModEntry> mods;
    static ULONGLONG last = 0;
    if (GetTickCount64() - last > 2000) { mods = VfsMods(); last = GetTickCount64(); }
    static std::set<std::string> changed;
    if (!changed.empty()) ImGui::TextColored(ImVec4(1, 0.85f, 0.35f, 1), "Restart the game to apply your changes.");
    if (mods.empty()) { ImGui::TextDisabled("No mods installed."); return; }
    if (ImGui::BeginTable("mods", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                          ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * std::min<int>(8, (int)mods.size() + 2)))) {
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Mod");
        ImGui::TableSetupColumn("Loose files", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 10);
        ImGui::TableHeadersRow();
        for (auto& m : mods) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(m.name.c_str());
            bool on = m.enabled;
            bool locked = !m.name.empty() && (m.name[0] == '_' || m.name[0] == '.');
            ImGui::BeginDisabled(locked);
            if (ImGui::Checkbox("##on", &on)) { VfsSetModEnabled(m.name, on); m.enabled = on; changed.insert(m.name); }
            ImGui::EndDisabled();
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(m.name.c_str());
            ImGui::TableNextColumn();
            if (!m.hasFiles) ImGui::TextDisabled("-");
            else if (!m.enabled) ImGui::TextDisabled("off");
            else ImGui::Text("%d", m.files);
        }
        ImGui::EndTable();
    }

    // ---- which loose file wins, and what it replaces
    ImGui::SeparatorText("Loose files in use");
    static std::vector<VfsFileEntry> files;
    static std::map<uint64_t, int> inGame;            // 1 replaces a game file, 0 new file
    static std::atomic<int> checking{ 0 };            // 0 idle, 1 running, 2 done
    static std::mutex inGameMx;
    if (files.empty() || ImGui::IsWindowAppearing()) files = VfsFiles();
    static char ff[128] = "";
    static bool onlyConflicts = false;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
    ImGui::InputTextWithHint("##vff", "search path or mod...", ff, sizeof(ff));
    ImGui::SameLine(); ImGui::Checkbox("only conflicts", &onlyConflicts);
    ImGui::SameLine();
    ImGui::BeginDisabled(checking == 1 || files.empty());
    if (ImGui::Button(checking == 1 ? "checking..." : "Check against game files")) {
        checking = 1;
        std::vector<uint64_t> ids; for (auto& f : files) ids.push_back(f.id);
        RunExtractJob([ids] {
            std::map<uint64_t, int> r;
            for (uint64_t id : ids) r[id] = GameFileExists(id) ? 1 : 0;
            { std::lock_guard<std::mutex> lk(inGameMx); inGame.swap(r); }
            checking = 2;
        });
    }
    ImGui::EndDisabled();
    Help("Every file the loose-file archive holds right now (from your last game start), the mod it comes from, and which "
         "other mods shipped the same file but lost on load order. \"Check against game files\" also tells you whether each "
         "file replaces one of the game's files or is new.");
    if (files.empty()) { ImGui::TextDisabled("No loose files are in use."); return; }
    int conflicts = 0; for (auto& f : files) if (!f.beaten.empty()) ++conflicts;
    ImGui::TextDisabled("%d files, %d with conflicts", (int)files.size(), conflicts);
    std::string fl = Lower(ff);
    std::map<uint64_t, int> ig;
    { std::lock_guard<std::mutex> lk(inGameMx); ig = inGame; }
    if (ImGui::BeginTable("vfiles", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                          ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 14))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("File");
        ImGui::TableSetupColumn("From mod", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7);
        ImGui::TableSetupColumn("Also in (lost)", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 10);
        ImGui::TableHeadersRow();
        std::vector<int> vis;
        for (int i = 0; i < (int)files.size(); ++i) {
            auto& f = files[i];
            if (onlyConflicts && f.beaten.empty()) continue;
            if (!fl.empty() && Lower(f.rel).find(fl) == std::string::npos && Lower(f.mod).find(fl) == std::string::npos) continue;
            vis.push_back(i);
        }
        ImGuiListClipper clip;
        clip.Begin((int)vis.size());
        while (clip.Step())
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                auto& f = files[vis[r]];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(f.rel.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("file id %016llx, %llu bytes", (unsigned long long)f.id, (unsigned long long)f.size);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(f.mod.c_str());
                ImGui::TableNextColumn();
                auto it = ig.find(f.id);
                if (it == ig.end()) ImGui::TextDisabled("?");
                else if (it->second) ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "replaces");
                else ImGui::TextColored(ImVec4(0.5f, 0.9f, 1, 1), "new file");
                ImGui::TableNextColumn();
                if (f.beaten.empty()) ImGui::TextDisabled("-");
                else {
                    std::string b; for (auto& m : f.beaten) b += (b.empty() ? "" : ", ") + m;
                    ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "%s", b.c_str());
                }
            }
        ImGui::EndTable();
    }
}

// ------------------------------------------------------- dev: file log ---
static std::string s_flSel, s_swapA, s_swapB;     // File log selection + swap pair (shared with the clip monitor)
static void TabFileLog() {
    if (!FileLogAvailable()) { ImGui::TextDisabled("Not available: the game's path-to-id function wasn't found (other game version?)."); return; }
    ImGui::TextWrapped("Records every file path the game asks for, and every resource the game creates (lines with [Type]: "
                       "animation packages, move data, meshes, textures... even ones it loads by id without a path), each once, with "
                       "its file id. Use it to find what to replace. Also written to fc4hook\\dumps\\file_log.txt. "
                       "Lines a loose mod supplies are marked with  <- ModName.");
    if (!ResourceLogAvailable()) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Resource log unavailable (function not found): paths only.");
    bool on = FileLogEnabled();
    if (ImGui::Checkbox("Record file paths", &on)) FileLogSetEnabled(on);
    ImGui::SameLine(); ImGui::Text("%llu entries", (unsigned long long)FileLogCount());
    ImGui::SameLine(); { bool st = FileLogAtStartup(); if (ImGui::Checkbox("Record from game start", &st)) FileLogSetAtStartup(st); }
    ImGui::SameLine(); Help("Starts recording as soon as the game starts, so you also catch what's loaded with the world "
                            "(animations, move data, characters). Restart the game after ticking it. Turn it off when you're done: "
                            "recording makes loading a little slower.");
    ImGui::SameLine(); if (ImGui::Button("Clear")) FileLogClear();
    ImGui::SameLine(); if (ImGui::Button("Open file_log.txt")) { FileLogFlush(); OpenFolder(g_rootDir + L"dumps\\file_log.txt"); }
    static char hpath[260] = "";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24);
    ImGui::InputTextWithHint("##hp", "path -> file id (e.g. graphics\\_textures\\foo.xbt)", hpath, sizeof(hpath));
    if (hpath[0]) {
        uint64_t id = FileId(hpath);
        ImGui::SameLine(); ImGui::Text("%016llx  (%llu)", (unsigned long long)id, (unsigned long long)id);
        ImGui::SameLine(); if (ImGui::SmallButton("copy")) { char b[32]; snprintf(b, sizeof(b), "%016llx", (unsigned long long)id); ImGui::SetClipboardText(b); }
    }
    // ---- extract / swap (click a line below to select it)
    std::string& sel = s_flSel; std::string& swapA = s_swapA; std::string& swapB = s_swapB;
    ImGui::SeparatorText("Extract / swap test");
    ImGui::TextWrapped("Selected: %s", sel.empty() ? "(click a line below)" : sel.c_str());
    ImGui::BeginDisabled(sel.empty());
    if (ImGui::Button("Extract to dumps\\extracted") && Debounced("fx")) {
        std::string line = sel;
        RunExtractJob([line] {
            std::string m, path, type; uint64_t id = 0;
            ParseLogLine(line, &id, &path, &type);
            if (path.empty() && !type.empty()) path = "__unknown\\" + type + "\\" + line.substr(0, 16) + ".bin";
            ExtractGameFile(id, path, &m); SetState("filelog.status", m); Log("%s", m.c_str());
        });
        SetState("filelog.status", "extracting...");
    }
    ImGui::SameLine(); if (ImGui::Button("Replace THIS...")) swapA = sel;
    ImGui::SameLine(); if (ImGui::Button("...WITH this")) swapB = sel;
    ImGui::EndDisabled();
    ImGui::SameLine(); Help("Swap test: select the file to replace and press \"Replace THIS...\", then select a file of the same "
                            "kind (texture for texture, animation for animation) and press \"...WITH this\", then Create swap mod. "
                            "It copies the second file out of the game's archives into mods\\FC4Hook_SwapTest. Restart the game to see it.");
    ImGui::Text("Replace: %s", swapA.empty() ? "-" : swapA.c_str());
    ImGui::Text("With:    %s", swapB.empty() ? "-" : swapB.c_str());
    ImGui::BeginDisabled(swapA.empty() || swapB.empty() || swapA == swapB);
    if (ImGui::Button("Create swap mod") && Debounced("fsw")) {
        std::string a = swapA, b = swapB;
        RunExtractJob([a, b] {
            uint64_t ia = 0, ib = 0; std::string pa, pb, ta, tb, m;
            ParseLogLine(a, &ia, &pa, &ta); ParseLogLine(b, &ib, &pb, &tb);
            if (!ta.empty() && !tb.empty() && ta != tb) LogL(LOG_WARN, "swap: %s replaced with a %s - different kinds of file, this may crash", ta.c_str(), tb.c_str());
            MakeSwap(ia, pa, ib, pb, &m); SetState("filelog.status", m);
        });
        SetState("filelog.status", "creating swap mod...");
    }
    ImGui::EndDisabled();
    ImGui::SameLine(); if (ImGui::Button("Open swap mod folder")) { CreateDirectoryW((g_rootDir + L"mods\\FC4Hook_SwapTest").c_str(), nullptr); OpenFolder(g_rootDir + L"mods\\FC4Hook_SwapTest"); }
    if (ImGui::Button("Install edited textures") && Debounced("fit")) {
        RunExtractJob([] { std::string m; InstallEditedTextures(&m); SetState("filelog.status", m); });
        SetState("filelog.status", "packing edited textures...");
    }
    ImGui::SameLine(); if (ImGui::Button("Label animation clips") && Debounced("flab")) {
        RunExtractJob([] { std::string m; LabelAnimationClips(&m); SetState("filelog.status", m); });
        SetState("filelog.status", "labelling clips...");
    }
    ImGui::SameLine(); Help("Reads every [CAnimationResource] clip in your file log out of the game's archives and tags it with its "
                            "length and the weapon / bone names inside, e.g. {2.30s ak74m RightHand}. Then filter by a weapon name "
                            "and use Replace THIS / WITH this to swap clips. Also written to dumps\\animation_clips.txt.");
    ImGui::SameLine(); if (ImGui::Button("Open extracted folder")) { CreateDirectoryW((g_rootDir + L"dumps\\extracted").c_str(), nullptr); OpenFolder(g_rootDir + L"dumps\\extracted"); }
    ImGui::SameLine(); Help("Texture editing: Extract a texture (.xbt). FC4Hook also writes a .dds of the whole image next to it. "
                            "Open the .dds in paint.net / GIMP / Photoshop, paint, and save it as DDS in the same format (DXT1/BC1 for "
                            "most _d textures) WITH mipmaps, same name. Then press Install edited textures: every changed .dds is packed "
                            "back into the game's format in mods\\MyTextures. Restart the game.");
    std::string fst = GetState("filelog.status");
    if (!fst.empty()) ImGui::TextWrapped("%s", fst.c_str());
    ImGui::Separator();

    static char filter[128] = "";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
    ImGui::InputTextWithHint("##ff", "filter (e.g. .xbt, ak47, .ark.fcb)", filter, sizeof(filter));
    ImGui::SameLine(); ImGui::TextDisabled("newest first; click a line to select it (and copy it)");
    static std::vector<std::string> lines;
    static uint64_t serial = ~0ull;
    static std::string lastFilter;
    if (serial != FileLogSerial() || lastFilter != filter) { lines = FileLogRecent(filter, 3000); serial = FileLogSerial(); lastFilter = filter; }
    ImGui::BeginChild("flog", ImVec2(0, 0), true);
    ImGuiListClipper clip;
    clip.Begin((int)lines.size());
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(lines[i].c_str(), lines[i] == sel)) { sel = lines[i]; ImGui::SetClipboardText(lines[i].c_str()); }
            ImGui::PopID();
        }
    ImGui::EndChild();
}

// ------------------------------------------------------- dev: spawner ----
// Armory weapons, spawned as the pickup you can walk up to and take (FC4Spawn.spawnPickup finds
// the pickup archetype through the weapon's WeaponProperties).
static void SpawnWeapons() {
    static char wf[64] = "";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    ImGui::InputTextWithHint("##wf", "filter...", wf, sizeof(wf));
    ImGui::SameLine(); ImGui::TextDisabled("spawned as pickups: walk up and take them");
    if (!s_armoryLoaded) LoadArmory();
    ImGui::BeginChild("spw", ImVec2(0, 0), true);
    std::string f = Lower(wf);
    ImGui::BeginDisabled(!LuaReady());
    for (auto& w : s_weapons) {
        if (!f.empty() && Lower(w.name).find(f) == std::string::npos) continue;
        ImGui::PushID(w.id.c_str());
        if (ImGui::SmallButton("Spawn") && Debounced(("sp" + w.id).c_str()))
            Lua(("FC4Spawn.spawnPickup('" + w.id + "', [[" + LuaSafe(w.name) + "]])").c_str(), "=spawn");
        ImGui::SameLine(); if (ImGui::SmallButton("Model only") && Debounced(("spm" + w.id).c_str()))
            Lua(("FC4Spawn.spawn('" + w.id + "', [[" + LuaSafe(w.name) + "]])").c_str(), "=spawn");
        ImGui::SameLine(); ImGui::Text("%s", w.name.c_str());
        ImGui::SameLine(); ImGui::TextDisabled("%s", w.id.c_str());
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    ImGui::EndChild();
}

// Every archetype in the game, from fc4hook\archetypes.txt (built once by scanning the archives),
// sorted into plain categories. The raw names are the game's library paths, e.g.
//   fcc_animals:fcc_quadrupeds/tigerbengal.rare_white_tiger_bengal
// where each ".x" is a variant of what's before it. Shown as "Rare white tiger bengal".
struct CatEntry { std::string id, raw, lraw, nice, lnice; int cat = 0; std::string sub; bool variant = false, junk = false; };
enum { CAT_PEOPLE, CAT_ANIMALS, CAT_VEHICLES, CAT_WEAPONS, CAT_PICKUPS, CAT_PROPS, CAT_TECH, CAT_N };
static const char* kCatNames[CAT_N] = { "People", "Animals", "Vehicles", "Weapons", "Pickups & collectibles", "Props", "Technical" };
static std::vector<CatEntry> s_cat;
static std::vector<std::pair<std::string, int>> s_catSubs[CAT_N];
static bool s_catLoaded = false;

// Older catalogs (1.4.0) kept the game's string-length byte in front of the name ("nCfcc_graphics:...").
static std::string CleanArchName(std::string s) {
    for (size_t k = 0; k < 3 && k < s.size(); ++k)
        if ((unsigned char)s[k] == s.size() - k) return s.substr(k + 1);
    return s;
}

static std::string Humanize(std::string s) {
    for (auto& c : s) if (c == '_' || c == '-') c = ' ';
    // drop engine prefixes that mean nothing to a player
    static const char* junk[] = { "basemerc fcx ", "basevillager fcx ", "uniquevillager fcx ", "uniquemerc fcx ", "fcc " };
    for (const char* j : junk) if (s.rfind(j, 0) == 0) s = s.substr(strlen(j));
    while (!s.empty() && s[0] == ' ') s.erase(0, 1);
    if (!s.empty()) s[0] = (char)toupper((unsigned char)s[0]);
    return s;
}

// split "a:b:c/d.e" into path pieces ("a","b","c","d.e")
static std::vector<std::string> Pieces(const std::string& s) {
    std::vector<std::string> v; std::string cur;
    for (char c : s) { if (c == ':' || c == '/') { if (!cur.empty()) v.push_back(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) v.push_back(cur);
    return v;
}

static bool Has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }

static void Classify(CatEntry& e) {
    const std::string& n = e.lraw;
    std::vector<std::string> pc = Pieces(n);
    std::string lib = pc.empty() ? "" : pc[0];
    std::string p1 = pc.size() > 1 ? pc[1] : "", p2 = pc.size() > 2 ? pc[2] : "";
    std::string last = pc.empty() ? n : pc.back();
    // last dotted piece is the most specific variant name
    size_t dot = last.rfind('.');
    std::string leaf = dot == std::string::npos ? last : last.substr(dot + 1);
    std::string base = dot == std::string::npos ? last : last.substr(0, last.find('.'));
    // a bare number or a generic word ("20", "static", "caged") means nothing without its base
    static const char* generic[] = { "static", "caged", "small", "big", "dead", "npc", "a", "b", "c", "snow", "winter", "dynamic", "destroyed" };
    bool weak = leaf.size() <= 3 || strspn(leaf.c_str(), "0123456789") == leaf.size();
    for (const char* g : generic) if (leaf == g) weak = true;
    if (dot != std::string::npos && base != leaf) e.nice = weak ? Humanize(base) + " " + leaf : Humanize(leaf) + "   (" + Humanize(base) + ")";
    else e.nice = Humanize(leaf);
    if (leaf == "basemerc_fcx") e.nice = "Generic soldier";
    e.variant = Has(n, "donotuse") || (Has(n, "pvp") && lib != "fcc_vehicles") || Has(n, ".multi") || Has(n, ".menu") || Has(n, ":test") ||
                Has(n, "_mis") || Has(n, "mis_") || Has(n, "mutliplayer") || Has(n, "multiplayer") || Has(n, ".e3") ||
                Has(n, "dynamic_convo") || Has(n, "dynamic_conversation") || Has(n, "_legacy");

    auto set = [&](int c, const std::string& sub) { e.cat = c; e.sub = sub; };
    if (lib == "fcc_npc" || lib == "mfcc_npc") {
        if (p1 == "merc") set(CAT_PEOPLE, Has(n, "army") ? "Royal Army" : Has(n, "rebel") ? "Golden Path" : "Soldiers");
        else if (p1 == "villagers") set(CAT_PEOPLE, "Villagers");
        else if (p1 == "outsiders") set(CAT_PEOPLE, "Outsiders & quest givers");
        else if (p1 == "dlc_cultist") set(CAT_PEOPLE, "Cultists (DLC)");
        else if (p1 == "corpses") set(CAT_PEOPLE, "Bodies");
        else if (p1 == "animated_mesh") set(CAT_PEOPLE, "Animated models (no AI)");
        else set(CAT_PROPS, "Character gear");
    } else if (lib == "fcc_animals") {
        if (p1 == "fcc_quadrupeds") set(CAT_ANIMALS, Has(n, "tiger") || Has(n, "leopard") || Has(n, "bear") || Has(n, "wolf") ||
                                                     Has(n, "dhole") || Has(n, "honeybadger") || Has(n, "rhino") || Has(n, "elephant") ? "Predators & big game" : "Land animals");
        else if (p1 == "fcc_winged") set(CAT_ANIMALS, "Birds");
        else if (p1 == "fcc_aquatic") set(CAT_ANIMALS, "Fish & water");
        else if (p1 == "fcc_domestic") set(CAT_ANIMALS, "Farm animals");
        else set(CAT_ANIMALS, "Packs & scripted");
    } else if (lib == "fcc_vehicles") {
        if (p1 == "land") set(CAT_VEHICLES, "Land");
        else if (p1 == "sea") set(CAT_VEHICLES, "Water");
        else if (p1 == "air") set(CAT_VEHICLES, "Air");
        else if (p1 == "missionspecific") set(CAT_VEHICLES, "Wrecks & parts");
        else set(CAT_VEHICLES, "Decorations & attachments");
    } else if (lib == "fcc_encounters" && p2 == "vehicle") {
        set(CAT_VEHICLES, "Vehicle encounters (with crew)");
    } else if (lib == "fcc_interactive" && p1 == "pickups") {
        if (p2 == "weapons") set(CAT_WEAPONS, "Weapon pickups (take them)");
        else set(CAT_PICKUPS, p2.empty() ? "Pickups" : "Pickups: " + Humanize(p2));
    } else if (lib == "weapons" && p1 == "fc3") {
        set(CAT_WEAPONS, "Weapon models (can't pick up)");
    } else if (lib == "gadgets") { set(CAT_WEAPONS, "Gadgets");
    } else if (lib == "supportweapons" || lib == "fcc_explosives") { set(CAT_WEAPONS, "Explosives & airstrikes");
    } else if (lib == "fcc_interactive") {
        if (p1 == "collectibles") set(CAT_PICKUPS, "Collectibles");
        else if (p1 == "openworldnotes") set(CAT_PICKUPS, "Notes & letters");
        else if (p1 == "collectibleplants") set(CAT_PICKUPS, "Plants");
        else if (p1 == "healingprops") set(CAT_PICKUPS, "Healing");
        else set(CAT_PROPS, "Interactive (ladders, doors, ziplines...)");
    } else if (lib == "fcc_props") { set(CAT_PROPS, p1 == "narrativeprops" ? "Story & cutscene props" : "Small props");
    } else if (lib == "fcc_graphics" || lib == "sha_dlc" || lib == "archive_fc3_graphics" || lib == "fco_graphics") {
        if (Has(n, "occlusion") || Has(n, ":vistas:") || Has(n, ":lighting:") || Has(n, ":test")) set(CAT_TECH, "Invisible / far-away models");
        else if (Has(n, "breakable")) set(CAT_PROPS, "Breakable objects");
        else if (Has(n, "decal")) set(CAT_PROPS, "Decals (marks on surfaces)");
        else if (Has(n, "rock") || Has(n, "nature") || Has(n, "vegetation") || Has(n, "cavern") || Has(n, "tree")) set(CAT_PROPS, "Nature & rocks");
        else if (Has(n, "statue") || Has(n, "exotic")) set(CAT_PROPS, "Statues & exotic");
        else if (Has(n, "missionspecific") || Has(n, "mission_") || Has(n, ":location:")) set(CAT_PROPS, "Mission & quest objects");
        else if (Has(n, "building") || Has(n, "infrastructure") || Has(n, "wall") || Has(n, "bridge") || Has(n, "fence") ||
                 Has(n, "platform") || Has(n, "temple") || Has(n, "tower") || Has(n, "outpost") || Has(n, "stair") || Has(n, "floor"))
            set(CAT_PROPS, "Buildings & structures");
        else if (Has(n, "vehicle") || Has(n, "wreck") || Has(n, "truck") || Has(n, "car_")) set(CAT_PROPS, "Wrecks & vehicle props");
        else if (Has(n, ":props:") || Has(n, "props")) set(CAT_PROPS, "Furniture & clutter");
        else set(CAT_PROPS, "Other world objects");
        if (lib == "sha_dlc" || Has(n, "shangri")) e.nice += "   [Shangri-La]";
    } else if (lib == "ige_archetypes") { set(CAT_TECH, "Level-editor pieces");
    } else {
        set(CAT_TECH, lib.empty() ? "(no library)" : lib);
    }
    // binary noise that happened to contain a ':' (not a real library name)
    for (char c : lib) if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) { e.junk = true; break; }
    if (lib.empty() || lib.size() < 3) e.junk = true;
}

static void LoadCatalog() {
    s_catLoaded = true; s_cat.clear();
    for (auto& v : s_catSubs) v.clear();
    FILE* f = _wfopen((g_rootDir + L"archetypes.txt").c_str(), L"rb");
    if (!f) return;
    std::map<std::string, int> subs[CAT_N];
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        char* tab = strchr(line, '\t'); if (!tab) continue;
        *tab = 0;
        std::string name = tab + 1;
        while (!name.empty() && (name.back() == '\r' || name.back() == '\n')) name.pop_back();
        CatEntry e; e.id = line; e.raw = CleanArchName(name); e.lraw = Lower(e.raw);
        Classify(e);
        if (e.junk) continue;
        e.lnice = Lower(e.nice);
        subs[e.cat][e.sub]++;
        s_cat.push_back(e);
    }
    fclose(f);
    for (int c = 0; c < CAT_N; ++c) for (auto& kv : subs[c]) s_catSubs[c].push_back(kv);
    std::sort(s_cat.begin(), s_cat.end(), [](const CatEntry& a, const CatEntry& b) { return a.lnice < b.lnice; });
}

static void SpawnCatalog() {
    std::string st = ArchetypeCatalogStatus();
    bool busy = st.rfind("scanning", 0) == 0;
    static bool wasBusy = false;
    if (wasBusy && !busy) s_catLoaded = false;          // scan just finished: reload
    wasBusy = busy;
    if (!s_catLoaded) LoadCatalog();
    if (s_cat.empty() && !busy)
        ImGui::TextWrapped("No catalog yet. Build it once: FC4Hook reads the game's archives and lists every archetype "
                           "(people, animals, vehicles, weapons, props...). Takes a minute or two; the game keeps running.");
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(s_cat.empty() ? "Build catalog" : "Rebuild catalog")) ArchetypeCatalogBuild();
    ImGui::EndDisabled();
    if (!st.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", st.c_str()); }
    if (s_cat.empty()) return;

    static int cat = CAT_PEOPLE, sub = -1;
    static char cf[64] = "";
    static bool variants = false, raw = false;
    // category buttons
    for (int c = 0; c < CAT_N; ++c) {
        if (c) ImGui::SameLine();
        bool on = c == cat;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(kCatNames[c])) { cat = c; sub = -1; }
        if (on) ImGui::PopStyleColor();
    }
    auto& subs = s_catSubs[cat];
    const char* preview = sub < 0 || sub >= (int)subs.size() ? "everything" : subs[sub].first.c_str();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
    if (ImGui::BeginCombo("##sub", preview)) {
        if (ImGui::Selectable("everything", sub < 0)) sub = -1;
        for (int i = 0; i < (int)subs.size(); ++i) {
            char b[200]; snprintf(b, sizeof(b), "%s  (%d)", subs[i].first.c_str(), subs[i].second);
            if (ImGui::Selectable(b, sub == i)) sub = i;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 13);
    ImGui::InputTextWithHint("##cf", "search all categories...", cf, sizeof(cf));
    ImGui::SameLine(); ImGui::Checkbox("variants", &variants);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Also show mission, multiplayer, test and \"do not use\" variants.");
    ImGui::SameLine(); ImGui::Checkbox("game names", &raw);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the game's own archetype paths instead of the tidy names.");

    std::string f = Lower(cf);
    bool searching = !f.empty();                        // a search looks through every category
    ImGui::BeginChild("spc", ImVec2(0, 0), true);
    ImGui::BeginDisabled(!LuaReady());
    int shown = 0;
    for (auto& e : s_cat) {
        if (!searching && e.cat != cat) continue;
        if (!searching && sub >= 0 && sub < (int)subs.size() && e.sub != subs[sub].first) continue;
        if (searching && e.cat == CAT_TECH && cat != CAT_TECH) continue;
        if (!variants && e.variant) continue;
        if (searching && e.lnice.find(f) == std::string::npos && e.lraw.find(f) == std::string::npos && e.id.find(f) == std::string::npos) continue;
        if (++shown > 600) { ImGui::TextDisabled("... more: narrow it down with the search box"); break; }
        ImGui::PushID(e.id.c_str());
        if (ImGui::SmallButton("Spawn") && Debounced(("spc" + e.id).c_str()))
            Lua(("FC4Spawn.spawn('" + e.id + "', [[" + LuaSafe(e.nice) + "]])").c_str(), "=spawn");
        ImGui::SameLine(); ImGui::TextUnformatted(raw ? e.raw.c_str() : e.nice.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nid %s", e.raw.c_str(), e.id.c_str());
        ImGui::SameLine(); ImGui::TextDisabled("%s%s", searching ? (std::string(kCatNames[e.cat]) + " / ").c_str() : "", e.sub.c_str());
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    if (!shown) ImGui::TextDisabled("%s", searching ? "Nothing matches." : "Nothing here (tick \"variants\" to see more).");
    ImGui::EndChild();
}

// Diagnose a custom (or any) archetype: file found? binary FCB? id inside matches? every model /
// texture / material it uses present? Runs in the background (the first archive index takes a
// few seconds).
static std::mutex s_chkMx;
static std::string s_chkText;
static std::atomic<bool> s_chkBusy{ false };

static void StartCheck(std::function<std::string()> job) {
    if (s_chkBusy.exchange(true)) return;
    { std::lock_guard<std::mutex> lk(s_chkMx); s_chkText = "checking..."; }
    RunExtractJob([job] {
        std::string r = job();
        { std::lock_guard<std::mutex> lk(s_chkMx); s_chkText = r; }
        Log("archetype check:\n%s", r.c_str());
        s_chkBusy = false;
    });
}

static void SpawnCheck() {
    static char cid[32] = "";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::InputTextWithHint("##chkid", "archetype id", cid, sizeof(cid), ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine();
    ImGui::BeginDisabled(s_chkBusy || !cid[0]);
    if (ImGui::Button("Check")) { uint64_t id = strtoull(cid, nullptr, 10); StartCheck([id] { return ArchetypeCheck(id); }); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(s_chkBusy);
    if (ImGui::Button("Check every archetype in my mods")) StartCheck([] { return ArchetypeCheckMods(); });
    ImGui::EndDisabled();
    std::string t; { std::lock_guard<std::mutex> lk(s_chkMx); t = s_chkText; }
    if (!t.empty()) {
        ImGui::SameLine(); if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(t.c_str());
        ImGui::BeginChild("chk", ImVec2(0, ImGui::GetFontSize() * 14), true, ImGuiWindowFlags_HorizontalScrollbar);
        std::string line; size_t p = 0;
        while (p < t.size()) {
            size_t e = t.find('\n', p); if (e == std::string::npos) e = t.size();
            line = t.substr(p, e - p); p = e + 1;
            bool bad = line.find("PROBLEM") != std::string::npos || line.find("MISSING") != std::string::npos;
            bool good = line.find("=> no problems") != std::string::npos;
            if (bad) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", line.c_str());
            else if (good) ImGui::TextColored(ImVec4(0.45f, 1, 0.45f, 1), "%s", line.c_str());
            else ImGui::TextUnformatted(line.c_str());
        }
        ImGui::EndChild();
    }
}

static void TabSpawner() {
    ImGui::TextWrapped("Spawns any archetype with the game's own factory (SpawnEntityFromArchetype_Legacy), in front of you: "
                       "weapons, props, vehicles, animals, NPCs, and new archetypes added by a mod's loose files.");
    ImGui::BeginDisabled(!LuaReady());
    static char arch[32] = "";
    static float dist = 4;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::InputTextWithHint("archetype id", "e.g. 7602852352217", arch, sizeof(arch), ImGuiInputTextFlags_CharsDecimal);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    if (ImGui::SliderFloat("distance", &dist, 1, 30, "%.0f m")) { char b[64]; snprintf(b, sizeof(b), "FC4Spawn.dist = %.1f", dist); Lua(b, "=spawn"); }
    if (ImGui::Button("Spawn") && arch[0] && Debounced("spawn")) Lua(("FC4Spawn.spawn('" + std::string(arch) + "')").c_str(), "=spawn");
    ImGui::SameLine(); if (ImGui::Button("Remove last") && Debounced("sprl")) Lua("FC4Spawn.removeLast()", "=spawn");
    ImGui::SameLine(); if (ImGui::Button("Remove all spawned") && Debounced("spra")) Lua("FC4Spawn.removeAll()", "=spawn");
    ImGui::EndDisabled();
    ImGui::Text("Spawned: %s   last entity: %s", GetState("spawn.n", "0").c_str(), GetState("spawn.last", "-").c_str());
    std::string st = GetState("spawn.status");
    if (!st.empty()) ImGui::TextWrapped("%s", st.c_str());
    if (ImGui::CollapsingHeader("Check an archetype (why won't it spawn?)")) SpawnCheck();
    if (ImGui::BeginTabBar("spawntabs")) {
        if (ImGui::BeginTabItem("Weapons")) { SpawnWeapons(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("People, animals, vehicles...")) { SpawnCatalog(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
}

// ------------------------------------------------------- dev: animation --
// State browser: names from animation\states.txt plus every bare name (no path) in your own
// dumps\file_log.txt. The game hashes Move-graph state names with the same CRC-64 as file
// paths, which is why they show up in the file log.
struct AnimName { std::string name, group; };
static std::vector<AnimName> s_animNames;
static bool s_animLoaded = false;

static bool IsBareName(const std::string& n) {
    if (n.size() < 3 || !isalpha((unsigned char)n[0])) return false;
    for (char c : n) if (!isalnum((unsigned char)c) && c != '_') return false;
    return true;
}

static void LoadAnimNames() {
    s_animNames.clear();
    std::set<std::string> seen;
    auto add = [&](const std::string& n, const std::string& g) { if (IsBareName(n) && seen.insert(Lower(n)).second) s_animNames.push_back({ n, g }); };
    FILE* f = _wfopen((g_rootDir + L"animation\\states.txt").c_str(), L"rb");
    char line[512];
    std::string group = "Other";
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            std::string l = line;
            while (!l.empty() && (l.back() == '\r' || l.back() == '\n' || l.back() == ' ')) l.pop_back();
            if (l.empty() || l[0] == '#') continue;
            if (l[0] == '[' && l.back() == ']') { group = l.substr(1, l.size() - 2); continue; }
            add(l, group);
        }
        fclose(f);
    }
    f = _wfopen((g_rootDir + L"dumps\\file_log.txt").c_str(), L"rb");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            std::string l = line;
            while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.pop_back();
            if (l.size() > 18 && l[16] == ' ' && l[17] == ' ') add(l.substr(18), "From your file log");
        }
        fclose(f);
    }
    s_animLoaded = true;
}

static void TabAnimation() {
    ImGui::TextWrapped("Drives the game's animation system: attach animation packages (yours too, shipped as loose files) and "
                       "play animations by path. The engine handles blending and the skeleton, so custom animations transition "
                       "like the game's own.");
    static int target = 0;
    static char custom[32] = "";
    ImGui::RadioButton("me", &target, 0); ImGui::SameLine();
    ImGui::RadioButton("last spawned", &target, 1); ImGui::SameLine();
    ImGui::RadioButton("entity id", &target, 2);
    if (target == 2) { ImGui::SameLine(); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12); ImGui::InputText("##eid", custom, sizeof(custom), ImGuiInputTextFlags_CharsDecimal); }
    std::string t = target == 0 ? "me" : target == 1 ? "last" : std::string(custom);
    std::string tq = "[[" + LuaSafe(t) + "]]";
    ImGui::BeginDisabled(!LuaReady());
    ImGui::SeparatorText("Animation package");
    static char pkg[260] = "";
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##pkg", "animation package path", pkg, sizeof(pkg));
    std::string pq = "[[" + LuaSafe(pkg) + "]]";
    if (ImGui::Button("Attach") && pkg[0] && Debounced("aatt")) Lua(("FC4Anim.attach(" + tq + ", " + pq + ")").c_str(), "=anim");
    ImGui::SameLine(); if (ImGui::Button("Detach") && pkg[0] && Debounced("adet")) Lua(("FC4Anim.detach(" + tq + ", " + pq + ")").c_str(), "=anim");
    ImGui::SameLine(); Help("Use Developer > File log while the game plays an animation to find package paths.");
    ImGui::SeparatorText("Play");
    static char anim[260] = "";
    static int layer = 0;
    static bool loop = false;
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##anim", "animation path or id", anim, sizeof(anim));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6); ImGui::InputInt("layer", &layer); ImGui::SameLine(); ImGui::Checkbox("loop", &loop);
    std::string aq = "[[" + LuaSafe(anim) + "]]";
    std::string extra = ", " + std::to_string(layer) + ", " + (loop ? "true" : "false");
    if (ImGui::Button("Play") && anim[0] && Debounced("aply")) Lua(("FC4Anim.play(" + tq + ", " + aq + extra + ", false)").c_str(), "=anim");
    ImGui::SameLine(); if (ImGui::Button("Force play") && anim[0] && Debounced("afpl")) Lua(("FC4Anim.play(" + tq + ", " + aq + extra + ", true)").c_str(), "=anim");
    ImGui::SameLine(); if (ImGui::Button("Current animation") && Debounced("acur")) Lua(("FC4Anim.current(" + tq + ", " + std::to_string(layer) + ")").c_str(), "=anim");
    ImGui::Text("Current: %s", GetState("anim.current", "-").c_str());
    ImGui::SeparatorText("Player animations by name (StartPlayAnim)");
    static char pname[128] = "";
    static bool b1 = false, b2 = false;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16); ImGui::InputTextWithHint("##pn", "name", pname, sizeof(pname));
    ImGui::SameLine(); ImGui::Checkbox("arg 3", &b1); ImGui::SameLine(); ImGui::Checkbox("arg 4", &b2);
    if (ImGui::Button("Start") && pname[0] && Debounced("ast")) Lua(("FC4Anim.start([[" + LuaSafe(pname) + "]], " + (b1 ? "true" : "false") + ", " + (b2 ? "true" : "false") + ")").c_str(), "=anim");
    ImGui::SameLine(); if (ImGui::Button("Stop") && Debounced("asp")) Lua("FC4Anim.stop()", "=anim");
    ImGui::EndDisabled();
    std::string st = GetState("anim.status");
    if (!st.empty()) ImGui::TextWrapped("Last: %s", st.c_str());

    ImGui::SeparatorText("Clip monitor");
    if (!ClipMonitorAvailable()) ImGui::TextDisabled("Unavailable: the game's clip-start function wasn't found.");
    else {
        bool cm = ClipMonitorEnabled();
        if (ImGui::Checkbox("Watch clips starting", &cm)) ClipMonitorSetEnabled(cm);
        ImGui::SameLine(); if (ImGui::Button("Clear##cm")) ClipMonitorClear();
        ImGui::SameLine(); Help("Lists every animation clip the moment the game starts playing it, newest first, with its labels "
                                "(press Label animation clips in the File log first for labels). Stand somewhere quiet, press Clear, "
                                "reload: the reload clips are at the top. Replace / With fill the swap pair in the File log tab.");
        static std::vector<ClipEvent> ev;
        static uint64_t evSerial = ~0ull;
        if (evSerial != ClipMonitorSerial()) { ev = ClipMonitorEvents(); evSerial = ClipMonitorSerial(); }
        ImGui::TextDisabled("%d clips started%s", (int)ev.size(), s_swapA.empty() ? "" : "   (swap pair set: finish it in File log)");
        ImGui::BeginChild("clips", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 12), true);
        uint64_t now = GetTickCount64();
        for (size_t i = 0; i < ev.size(); ++i) {
            char idb[24]; snprintf(idb, sizeof(idb), "%016llx", (unsigned long long)ev[i].id);
            std::string lab = ClipLabelFor(ev[i].id);
            std::string line = std::string(idb) + "  [CAnimationResource] (no path)" + (lab.empty() ? "" : " {" + lab + "}");
            ImGui::PushID((int)i);
            if (ImGui::SmallButton("Replace")) s_swapA = line;
            ImGui::SameLine(); if (ImGui::SmallButton("With")) s_swapB = line;
            ImGui::SameLine(); ImGui::TextDisabled("%5.1fs ago", (now - ev[i].tick) / 1000.0);
            ImGui::SameLine(); ImGui::Text("%s  %s", idb, lab.c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    ImGui::SeparatorText("State browser");
    if (!s_animLoaded) LoadAnimNames();
    static char af[96] = "";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
    ImGui::InputTextWithHint("##af", "filter (e.g. holster, takedown, Com_)", af, sizeof(af));
    ImGui::SameLine(); if (ImGui::Button("Reload list")) { FileLogFlush(); LoadAnimNames(); }
    ImGui::SameLine(); Help("Play = StartPlayAnim on you with the arg 3 / arg 4 ticks above. PKG_ names are animation packages: "
                            "Attach adds them to the target. Names come from fc4hook\\animation\\states.txt and from your own "
                            "dumps\\file_log.txt (Reload list after recording more).");
    ImGui::TextDisabled("%d names", (int)s_animNames.size());
    ImGui::BeginChild("anames", ImVec2(0, 0), true);
    std::string fl = Lower(af);
    std::vector<int> vis;
    for (int i = 0; i < (int)s_animNames.size(); ++i)
        if (fl.empty() || Lower(s_animNames[i].name).find(fl) != std::string::npos || Lower(s_animNames[i].group).find(fl) != std::string::npos) vis.push_back(i);
    ImGuiListClipper clip;
    clip.Begin((int)vis.size());
    ImGui::BeginDisabled(!LuaReady());
    while (clip.Step())
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
            const AnimName& a = s_animNames[vis[r]];
            ImGui::PushID(vis[r]);
            bool pkg = a.name.rfind("PKG_", 0) == 0;
            if (ImGui::SmallButton(pkg ? "Attach" : "Play") && Debounced(("an" + a.name).c_str())) {
                std::string q = "[[" + LuaSafe(a.name) + "]]";
                if (pkg) Lua(("FC4Anim.attach(" + tq + ", " + q + ")").c_str(), "=anim");
                else Lua(("FC4Anim.start(" + q + ", " + (b1 ? "true" : "false") + ", " + (b2 ? "true" : "false") + ")").c_str(), "=anim");
            }
            ImGui::SameLine(); ImGui::TextUnformatted(a.name.c_str());
            ImGui::SameLine(); ImGui::TextDisabled("%s", a.group.c_str());
            ImGui::PopID();
        }
    ImGui::EndDisabled();
    ImGui::EndChild();
}

// ------------------------------------------------------- dev: models -----
// Extract .xbg models with their materials and textures (extract.cpp ExtractModel).
static std::mutex s_mdlMx;
static std::string s_mdlMsg;
static std::atomic<bool> s_mdlBusy{ false };

static void ModelExtract(const std::vector<std::string>& paths) {
    if (paths.empty() || s_mdlBusy.exchange(true)) return;
    { std::lock_guard<std::mutex> lk(s_mdlMx); s_mdlMsg = "extracting..."; }
    RunExtractJob([paths] {
        std::string all;
        for (auto& p : paths) { std::string m; ExtractModel(p, &m); all += m + "\n"; }
        { std::lock_guard<std::mutex> lk(s_mdlMx); s_mdlMsg = all; }
        s_mdlBusy = false;
    });
}

static void TabModels() {
    ImGui::TextWrapped("Extract a model (.xbg) from the game's archives together with its materials (.material.bin) and "
                       "textures (.xbt, plus a .dds copy of each to view or edit), into fc4hook\\dumps\\extracted\\<game path>. "
                       "Open the .xbg with the community Dunia model tools.");
    ImGui::BeginDisabled(s_mdlBusy);
    static char path[260] = "";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 26);
    ImGui::InputTextWithHint("##mdlp", "graphics\\_common\\weapons\\ak74m\\ak74m.xbg", path, sizeof(path));
    ImGui::SameLine(); if (ImGui::Button("Extract##p") && path[0]) ModelExtract({ path });

    static char aid[32] = "";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::InputTextWithHint("##mdla", "archetype id", aid, sizeof(aid), ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine();
    if (ImGui::Button("Extract every model it uses") && aid[0]) {
        uint64_t id = strtoull(aid, nullptr, 10);
        if (!s_mdlBusy.exchange(true)) {
            { std::lock_guard<std::mutex> lk(s_mdlMx); s_mdlMsg = "reading the archetype..."; }
            RunExtractJob([id] {
                std::vector<std::string> ms = ArchetypeModels(id);
                std::string all = ms.empty() ? "That archetype doesn't use any .xbg model (or isn't there).\n" : "";
                for (auto& p : ms) { std::string m; ExtractModel(p, &m); all += m + "\n"; }
                { std::lock_guard<std::mutex> lk(s_mdlMx); s_mdlMsg = all; }
                s_mdlBusy = false;
            });
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Any archetype: a weapon id from the Armory, a Workshop weapon, an id from the Spawner list...");
    ImGui::EndDisabled();
    {
        std::lock_guard<std::mutex> lk(s_mdlMx);
        if (!s_mdlMsg.empty()) {
            ImGui::BeginChild("mdlmsg", ImVec2(0, ImGui::GetFontSize() * 7), true, ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(s_mdlMsg.c_str());
            ImGui::EndChild();
        }
    }
    if (ImGui::Button("Open the extracted folder")) {
        std::wstring dir = g_rootDir + L"dumps\\extracted";
        CreateDirectoryW((g_rootDir + L"dumps").c_str(), nullptr); CreateDirectoryW(dir.c_str(), nullptr);
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    ImGui::SeparatorText("Turn an edited .obj back into a game model");
    ImGui::TextWrapped("Extract a model, open the .xbg in Blender with a Far Cry .xbg importer, change its shape (move, scale, "
                       "rotate vertices; don't add or delete any), export as .obj. Then give the model's game path, your .obj and a "
                       "mod name. The result REPLACES that model everywhere in the game (mods\\<name>\\files\\<game path>). "
                       "For a new weapon with its own model, use Workshop > Model > Use my .obj instead.");
    static char cvPath[260] = "", cvObj[260] = "", cvMod[64] = "MyModel";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 26); ImGui::InputTextWithHint("model's game path##cv", "graphics\\_common\\weapons\\deserteagle\\desert_eagle.xbg", cvPath, sizeof(cvPath));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 26); ImGui::InputTextWithHint("your .obj##cv", "C:\\...\\long deagle.obj", cvObj, sizeof(cvObj));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12); ImGui::InputText("mod name##cv", cvMod, sizeof(cvMod));
    ImGui::SameLine();
    ImGui::BeginDisabled(!cvPath[0] || !cvObj[0] || !cvMod[0]);
    if (ImGui::Button("Convert")) {
        std::string gp = Lower(cvPath); for (auto& c : gp) if (c == '/') c = '\\';
        std::wstring out = g_rootDir + L"mods\\" + Widen(cvMod) + L"\\files\\" + Widen(gp);
        for (size_t k = 0; k < out.size(); ++k) if (out[k] == L'\\' && k > 2) CreateDirectoryW(out.substr(0, k).c_str(), nullptr);
        std::string m;
        if (ModelFromObj(gp, Widen(cvObj), out, &m)) m += " Written to mods\\" + std::string(cvMod) + "\\files\\" + gp + ". Restart the game.";
        std::lock_guard<std::mutex> lk(s_mdlMx); s_mdlMsg = m;
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Models the game has loaded (from the File log)");
    if (!FileLogEnabled()) {
        ImGui::TextDisabled("Turn on the File log (File log tab > Record file paths), then look at / equip / spawn things:");
        ImGui::TextDisabled("every model the game loads shows up here.");
        if (ImGui::Button("Turn the File log on")) FileLogSetEnabled(true);
    }
    static char flt[64] = "";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    ImGui::InputTextWithHint("##mdlf", "filter, e.g. weapons, ak74", flt, sizeof(flt));
    static std::vector<std::string> lines; static uint64_t serial = ~0ull;
    if (serial != FileLogSerial()) { lines = FileLogRecent(".xbg", 3000); serial = FileLogSerial(); }
    std::string f = Lower(flt);
    ImGui::BeginChild("mdllist", ImVec2(0, 0), true);
    ImGui::BeginDisabled(s_mdlBusy);
    int shown = 0;
    std::set<std::string> seen;
    for (auto& l : lines) {
        uint64_t id; std::string p, type;
        if (!ParseLogLine(l, &id, &p, &type) || p.empty()) continue;
        std::string lp = Lower(p);
        if (lp.size() < 4 || lp.compare(lp.size() - 4, 4, ".xbg") != 0 || !seen.insert(lp).second) continue;
        if (!f.empty() && lp.find(f) == std::string::npos) continue;
        if (++shown > 1000) break;
        ImGui::PushID(lp.c_str());
        if (ImGui::SmallButton("Extract")) ModelExtract({ p });
        ImGui::SameLine(); ImGui::TextUnformatted(p.c_str());
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    if (!shown) ImGui::TextDisabled("No models yet.");
    ImGui::EndChild();
}

static void TabDeveloper() {
    if (!ImGui::BeginTabBar("dev")) return;
    if (ImGui::BeginTabItem("Diagnostics")) {
        ImGui::Text("%s   API v%d", FC4HOOK_TITLE, FC4HOOK_API_VERSION);
        ImGui::Text("Lua bridge: %s   hook calls %llu   pump ticks %llu (skipped mid-Lua %llu)", BridgeStatus(),
                    (unsigned long long)LuaHookCalls(), (unsigned long long)PumpTicks(), (unsigned long long)PumpRejectsMidLua());
        ImGui::Text("Frames: %llu   world loads: %s", (unsigned long long)g_frameCounter.load(), GetState("world.loads", "0").c_str());
        ImGui::Text("Mod archive (data_win32\\fc4hook_mods.dat): %s", ModArchiveStatus());
        ImGui::Text("Loose files (data_win32\\fc4hook_loose.dat): %s", VfsStatus().c_str());
        ImGui::Text("File log: %s", FileLogAvailable() ? "available" : "unavailable");
        ImGui::SeparatorText("Signatures");
        for (auto& s : SignatureReport()) {
            ImGui::TextColored(s.addr ? ImVec4(0.4f, 1, 0.4f, 1) : ImVec4(1, 0.4f, 0.4f, 1), "%-28s %s", s.name, s.addr ? "OK" : "MISSING");
        }
        ImGui::SeparatorText("Plugins");
        auto plugins = LoadedPlugins();
        if (plugins.empty()) ImGui::TextDisabled("none (fc4hook\\plugins\\*.dll)");
        for (auto& p : plugins) ImGui::BulletText("%s", p.c_str());
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Memory")) { TabMemory(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("File log")) { TabFileLog(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Models")) { TabModels(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Animation")) { TabAnimation(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Lua 5.4")) {
        ImGui::Text("Lua 5.4 runtime: %s", Lua54Status());
        ImGui::TextWrapped("Scripts in fc4hook\\lua\\autorun\\ run at startup; require() loads modules from fc4hook\\lua\\. "
                           "APIs: game.* (call any game function), mem.* (memory, 64-bit integers), fc4.* (log, ticks, timers, keys, menu buttons). "
                           "See fc4hook\\lua\\README.txt. Use the Console's \"Lua 5.4\" mode to try things.");
        if (ImGui::Button("Reload Lua 5.4 scripts")) { Lua54Reload(); Log("reloading Lua 5.4"); }
        ImGui::SameLine();
        if (ImGui::Button("Open lua folder")) OpenFolder(g_rootDir + L"lua");
        ImGui::SeparatorText("Buttons from scripts");
        auto btns = Lua54Buttons();
        std::string group;
        for (auto& b : btns) {
            if (b.group != group) { group = b.group; ImGui::TextDisabled("%s", group.c_str()); }
            ImGui::PushID(b.id);
            if (ImGui::Button(b.label.c_str()) && Debounced(("b54" + std::to_string(b.id)).c_str())) Lua54PressButton(b.id);
            ImGui::PopID();
        }
        if (btns.empty()) ImGui::TextDisabled("None yet: scripts add them with fc4.menu.button(\"Label\", function() ... end)");
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Natives")) {
        static char nfilter[128] = "";
        static char args[256] = "";
        if (ImGui::Button("Dump from game") && Debounced("dump")) RunScriptWithPrelude(L"dump_natives.lua", "");
        ImGui::SameLine();
        static FILETIME lastWrite = {};
        static ULONGLONG lastCheck = 0;
        bool forceLoad = ImGui::Button("Load list");
        if (forceLoad || GetTickCount64() - lastCheck > 1000) {
            lastCheck = GetTickCount64();
            WIN32_FILE_ATTRIBUTE_DATA fa;
            if (GetFileAttributesExW((g_rootDir + L"dumps\\natives.txt").c_str(), GetFileExInfoStandard, &fa) &&
                (forceLoad || CompareFileTime(&fa.ftLastWriteTime, &lastWrite) != 0)) {
                lastWrite = fa.ftLastWriteTime;
                LoadNativesFile();
            }
        }
        ImGui::SameLine(); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
        ImGui::InputTextWithHint("##nf", "filter...", nfilter, sizeof(nfilter));
        ImGui::SameLine(); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
        ImGui::InputTextWithHint("##args", "args for Call", args, sizeof(args));
        ImGui::TextDisabled(s_natives.empty() ? "No dump yet: load into the world, then Dump from game."
                                              : "%d functions. Click one to call it with the args above (inside pcall).", (int)s_natives.size());
        ImGui::BeginChild("nat", ImVec2(0, 0), true);
        std::string flt = Lower(nfilter);
        ImGuiListClipper clip;
        std::vector<int> vis;
        for (int i = 0; i < (int)s_natives.size(); ++i)
            if (flt.empty() || Lower(s_natives[i]).find(flt) != std::string::npos) vis.push_back(i);
        clip.Begin((int)vis.size());
        while (clip.Step())
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                const std::string& n = s_natives[vis[r]];
                if (ImGui::Selectable(n.c_str()) && Debounced(n.c_str())) {
                    std::string name = n.size() > 2 && n[1] == ' ' ? n.substr(2) : n;
                    ProbeNative(name, args);
                }
            }
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Scripts")) {
        ImGui::TextDisabled("fc4hook\\scripts\\*.lua   (autorun\\ runs automatically)");
        if (ImGui::Button("Reload autorun")) RequestAutorunReload();
        ImGui::Separator();
        for (auto& s : ListScripts()) {
            ImGui::PushID(Narrow(s).c_str());
            if (ImGui::Button("Run") && Debounced(Narrow(s).c_str())) RunScriptWithPrelude(s, "");
            ImGui::SameLine(); ImGui::TextUnformatted(Narrow(s).c_str());
            ImGui::PopID();
        }
        ImGui::SeparatorText("Lua editor");
        static char editor[8192] = "-- multi-line Lua; Ctrl+Enter or Run\nModLog(\"hello from FC4Hook\")\n";
        ImGui::InputTextMultiline("##ed", editor, sizeof(editor), ImVec2(-1, ImGui::GetTextLineHeight() * 12), ImGuiInputTextFlags_AllowTabInput);
        bool run = ImGui::IsItemFocused() && ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ImGui::IsKeyPressed(ImGuiKey_Enter);
        if ((ImGui::Button("Run") || run) && Debounced("edrun")) { Log("> (editor, %d bytes)", (int)strlen(editor)); QueueLua(editor, "=editor"); }
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Co-op tools")) {
        ImGui::BeginDisabled(!LuaReady());
        if (ImGui::Button("Test: re-grant one tower's rewards") && Debounced("test")) RunScriptWithPrelude(L"coop\\tower_rewards.lua", "TOWER_MODE='test'");
        if (ImGui::Button("Inspect towers -> dumps\\tower_inspect.txt") && Debounced("insp")) RunScriptWithPrelude(L"coop\\tower_rewards.lua", "TOWER_MODE='inspect'");
        if (ImGui::Button("Progress snapshot -> dumps\\progress_*.txt") && Debounced("snap")) RunScriptWithPrelude(L"progress_inspector.lua", "");
        ImGui::SeparatorText("Player info");
        if (ImGui::Button("Number of players")) ProbeNative("GetNumberOfPlayers", ""); ImGui::SameLine();
        if (ImGui::Button("My player id")) ProbeNative("GetLocalPlayerId", ""); ImGui::SameLine();
        if (ImGui::Button("Time of day")) ProbeNative("GetTimeOfDay", "");
        for (int i = 0; i < 4; ++i) {
            if (i) ImGui::SameLine();
            char b[24]; snprintf(b, sizeof(b), "Player in slot %d", i);
            if (ImGui::Button(b)) ProbeNative("GetPlayerInSlot", std::to_string(i));
        }
        ImGui::EndDisabled();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

// ------------------------------------------------------------ about ------
static void TabAbout() {
    ImGui::Text("%s", FC4HOOK_TITLE);
    ImGui::TextWrapped("A script hook and mod menu for Far Cry 4 (PC). Runs Lua scripts and C++ plugins inside the "
                       "game, with cheats, an armory, bell-tower travel and a co-op tower reward fix.");
    ImGui::Spacing();
    ImGui::BulletText("Menu key: %s   (change it in Settings)", KeyName(g_cfg.menuKey).c_str());
    ImGui::BulletText("Your files: Far Cry 4\\bin\\fc4hook\\");
    ImGui::BulletText("Scripts: fc4hook\\scripts\\autorun\\*.lua   Plugins: fc4hook\\plugins\\*.dll");
    ImGui::BulletText("Bug reports: attach fc4hook.log (and any file in fc4hook\\crashes\\)");
    ImGui::Spacing();
    ImGui::TextDisabled("Uses Dear ImGui (MIT), MinHook (BSD 2-clause) and Lua 5.4 (MIT). See LICENSES.txt.");
    ImGui::TextDisabled("Not affiliated with Ubisoft. Use cheats offline or with friends who agree.");
}

static void ModelExtract(const std::vector<std::string>& paths);
// ------------------------------------------------------------- workshop --
// Make new weapons from existing ones and change their stats / model (workshop.cpp).
static std::mutex s_wsMx;
static std::string s_wsMsg;
static std::atomic<bool> s_wsBusy{ false };
static std::vector<WorkshopWeapon> s_wsList;
static bool s_wsListed = false;
static int s_wsSel = -1;
struct WsPart { std::vector<uint8_t> d, base; std::vector<FcbValue> v; std::string err; bool loaded = false; };
static WsPart s_wsParts[3];

static void WsReload() { for (auto& p : s_wsParts) p = WsPart(); }
static WsPart& WsGet(const WorkshopWeapon& w, int part) {
    WsPart& p = s_wsParts[part];
    if (!p.loaded) {
        p.loaded = true;
        if (WorkshopRead(w, part, p.d, p.v, &p.err)) {
            if (!WorkshopReadBase(w, part, p.base) || p.base.size() != p.d.size()) p.base.clear();
        }
    }
    return p;
}

// The stats most people want, with plain-English labels.
static const struct { const char* field; const char* label; } kWsCommon[] = {
    { "iFireRate", "Fire rate (rounds per minute)" }, { "iAmmoInClip", "Magazine size" },
    { "AmmoUsedPerShot", "Ammo used per shot" }, { "fReloadTimeNormal", "Reload time (s)" },
    { "fReloadTimeForced", "Reload time, empty (s)" }, { "fRange", "Range (m)" },
    { "fDrawTime", "Draw time (s)" }, { "fHolsterTime", "Holster time (s)" },
    { "fVerticalRecoilPerShot", "Recoil up per shot" }, { "fLeftRecoilPerShot", "Recoil left per shot" },
    { "fRightRecoilPerShot", "Recoil right per shot" }, { "fRecoilMaxPitch", "Max recoil (up)" },
    { "fMoveSpeedFactor", "Move speed while held" }, { "fMoveSpeedFactorShooting", "Move speed while shooting" },
    { "bIsSilent", "Silent" }, { "bHasInfiniteAmmo", "Infinite ammo" }, { "bAutoReload", "Auto reload" },
    { "fUnjamTime", "Unjam time (s)" }, { "fNearFovDegrees", "Weapon FOV (degrees)" },
};

static char WsKind(const FcbValue& v, const std::vector<uint8_t>* d = nullptr) {
    char k = d ? FcbKindOf(*d, v) : FcbKind(v);
    if (!k && !v.shared && v.size == 4 && v.name == "AmmoUsedPerShot") k = 'i';
    return k;
}

static void WsEditRow(const WorkshopWeapon& w, int part, WsPart& p, const FcbValue& v, const char* label) {
    char k = WsKind(v, &p.d);
    ImGui::PushID((int)v.offset + part * 0x1000000);
    ImGui::TableNextRow();
    ImGui::TableNextColumn(); ImGui::TextUnformatted(label ? label : v.name.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", v.name.c_str(), v.object.c_str());
    ImGui::TableNextColumn();
    std::string cur = FcbFormat(p.d, v);
    bool changed = false; std::string nv;
    if (k == 'b') {
        bool b = p.d[v.offset] != 0;
        if (ImGui::Checkbox("##b", &b)) { changed = true; nv = b ? "1" : "0"; }
    } else if (k == 'f' || k == 'i') {
        char buf[32]; snprintf(buf, sizeof(buf), "%s", cur.c_str());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::InputText("##v", buf, sizeof(buf), k == 'f' ? ImGuiInputTextFlags_CharsScientific : ImGuiInputTextFlags_CharsDecimal);
        if (ImGui::IsItemDeactivatedAfterEdit()) { changed = true; nv = buf; }
    } else ImGui::TextDisabled("%s", cur.c_str());
    ImGui::TableNextColumn();
    if (!p.base.empty()) {
        std::vector<uint8_t>& b = p.base;
        std::string bv = FcbFormat(b, v);
        if (bv != cur) ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "%s", bv.c_str()); else ImGui::TextDisabled("%s", bv.c_str());
    }
    if (changed) {
        std::string err;
        if (!WorkshopSet(w, part, v.offset, k, nv, &err)) { std::lock_guard<std::mutex> lk(s_wsMx); s_wsMsg = "Couldn't change " + v.name + ": " + err; }
        else { std::lock_guard<std::mutex> lk(s_wsMx); s_wsMsg = "Saved " + v.name + " = " + nv + ". Restart the game to use it."; }
        s_wsParts[part].loaded = false;
    }
    ImGui::PopID();
}

static void WsStats(const WorkshopWeapon& w) {
    WsPart& p = WsGet(w, 1);
    if (!p.err.empty()) { ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", p.err.c_str()); return; }
    static bool all = false; static char flt[64] = "";
    ImGui::Checkbox("show every value", &all);
    if (all) { ImGui::SameLine(); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14); ImGui::InputTextWithHint("##wsf", "filter, e.g. recoil, sway, ammo", flt, sizeof(flt)); }
    ImGui::TextDisabled("Orange in the last column = changed from the original weapon.");
    if (!ImGui::BeginTable("wsstats", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, 0))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Stat"); ImGui::TableSetupColumn("Value"); ImGui::TableSetupColumn("Original");
    ImGui::TableHeadersRow();
    if (!all) {
        for (auto& c : kWsCommon)
            for (auto& v : p.v) if (v.name == c.field && WsKind(v)) { WsEditRow(w, 1, p, v, c.label); break; }
    } else {
        std::string f = Lower(flt);
        std::string lastObj;
        for (auto& v : p.v) {
            if (!WsKind(v) || WsKind(v) == 'u') continue;
            if (!f.empty() && Lower(v.name).find(f) == std::string::npos && Lower(v.object).find(f) == std::string::npos) continue;
            if (v.object != lastObj) {
                lastObj = v.object;
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                size_t sl = v.object.rfind('/'); ImGui::TextDisabled("[%s]", sl == std::string::npos ? v.object.c_str() : v.object.c_str() + sl + 1);
            }
            WsEditRow(w, 1, p, v, nullptr);
        }
    }
    ImGui::EndTable();
}

static void WsModel(const WorkshopWeapon& w) {
    WsPart& p = WsGet(w, 0);
    if (!p.err.empty()) { ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", p.err.c_str()); return; }
    ImGui::TextWrapped("The models the weapon uses. Type the game path of your own model (an .xbg you ship in this mod's "
                       "files\\ folder, or any model already in the game) and press Set. Use F8 > Developer > File log to "
                       "find paths. First-person and third-person (_tp) models are separate.");
    int n = 0;
    for (size_t i = 0; i < p.v.size(); ++i) {
        const FcbValue& v = p.v[i];
        if (v.shared || v.size != 8 || v.name != "ModelPath") continue;
        ++n;
        // the readable path is stored just before the hash
        std::string text = i > 0 ? FcbFormat(p.d, p.v[i - 1]) : "";
        ImGui::PushID((int)v.offset);
        ImGui::SeparatorText(v.object.substr(v.object.rfind('/') + 1).c_str());
        ImGui::TextDisabled("now: %s", text.c_str());
        if (text.size() > 2 && text[0] == '"') {
            ImGui::SameLine();
            if (ImGui::SmallButton("Extract it")) ModelExtract({ text.substr(1, text.size() - 2) });
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy this model + its materials and textures to fc4hook\\dumps\\extracted\\ to edit");
            // edited .obj of this model -> new .xbg in the mod, and point the weapon at it
            static std::map<uint32_t, std::string> objs;
            std::string& ob = objs[v.offset];
            char obuf[260]; snprintf(obuf, sizeof(obuf), "%s", ob.c_str());
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20);
            if (ImGui::InputTextWithHint("##obj", "your edited .obj (full path, or its name if it's in this mod's folder)", obuf, sizeof(obuf))) ob = obuf;
            ImGui::SameLine();
            ImGui::BeginDisabled(ob.empty());
            if (ImGui::Button("Use my .obj")) {
                std::string orig = text.substr(1, text.size() - 2);
                std::wstring objPath = Widen(ob);
                if (ob.find(':') == std::string::npos) {                 // a name: look for it anywhere in the mod folder
                    std::wstring base = g_rootDir + L"mods\\" + Widen(w.mod) + L"\\";
                    objPath = base + Widen(ob);
                    if (GetFileAttributesW(objPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                        WIN32_FIND_DATAW fd; std::wstring want = Widen(ob);
                        size_t sl = want.find_last_of(L"\\/"); if (sl != std::wstring::npos) want = want.substr(sl + 1);
                        std::vector<std::wstring> dirs = { base };
                        for (size_t k = 0; k < dirs.size() && k < 200; ++k) {
                            HANDLE h = FindFirstFileW((dirs[k] + L"*").c_str(), &fd);
                            if (h == INVALID_HANDLE_VALUE) continue;
                            do {
                                std::wstring n = fd.cFileName;
                                if (n == L"." || n == L"..") continue;
                                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) dirs.push_back(dirs[k] + n + L"\\");
                                else if (_wcsicmp(n.c_str(), want.c_str()) == 0) objPath = dirs[k] + n;
                            } while (FindNextFileW(h, &fd));
                            FindClose(h);
                        }
                    }
                }
                std::string safe = Lower(w.mod);
                for (auto& c : safe) if (!isalnum((unsigned char)c)) c = '_';
                std::string fname = orig.substr(orig.find_last_of("\\/") + 1);
                std::string gamePath = "graphics\\fc4hook\\" + safe + "\\" + fname;
                std::wstring out = g_rootDir + L"mods\\" + Widen(w.mod) + L"\\files\\" + Widen(gamePath);
                for (size_t k = 0; k < out.size(); ++k) if (out[k] == L'\\' && k > 2) CreateDirectoryW(out.substr(0, k).c_str(), nullptr);
                std::string m, err;
                if (ModelFromObj(orig, objPath, out, &m)) {
                    if (WorkshopSet(w, 0, v.offset, 'p', gamePath, &err)) m += " Saved as " + gamePath + " and the weapon now uses it. Restart the game to see it.";
                    else m += " (but couldn't point the weapon at it: " + err + ")";
                    s_wsParts[0].loaded = false;
                }
                std::lock_guard<std::mutex> lk(s_wsMx); s_wsMsg = m;
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Changed this model's shape in Blender? Export it as .obj (same vertices, same order:\n"
                                  "only move, scale or rotate them) and FC4Hook turns it into a game model for this weapon.");
        }
        static std::map<uint32_t, std::string> edits;
        std::string& e = edits[v.offset];
        char buf[260]; snprintf(buf, sizeof(buf), "%s", e.c_str());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 26);
        if (ImGui::InputTextWithHint("##mp", "graphics\\mymod\\mygun.xbg", buf, sizeof(buf))) e = buf;
        ImGui::SameLine();
        ImGui::BeginDisabled(e.empty());
        if (ImGui::Button("Set")) {
            std::string err;
            uint64_t id = FileId(e.c_str());
            bool exists = GameFileExists(id);
            for (auto& f : VfsFiles()) if (f.id == id) exists = true;
            if (!WorkshopSet(w, 0, v.offset, 'p', e, &err)) { std::lock_guard<std::mutex> lk(s_wsMx); s_wsMsg = "Couldn't set the model: " + err; }
            else { std::lock_guard<std::mutex> lk(s_wsMx);
                   s_wsMsg = "Model set to " + e + (exists ? ". Restart the game to see it." :
                             ". Note: that file doesn't exist yet (not in the game or a loaded mod). Put it in this mod's files\\ folder, then restart."); }
            s_wsParts[0].loaded = false;
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    if (!n) ImGui::TextDisabled("No model paths found in this weapon's item archetype.");
}

static void WsAdvanced(const WorkshopWeapon& w) {
    static int part = 1;
    ImGui::RadioButton("item", &part, 0); ImGui::SameLine(); ImGui::RadioButton("properties", &part, 1); ImGui::SameLine(); ImGui::RadioButton("pickup", &part, 2);
    WsPart& p = WsGet(w, part);
    if (!p.err.empty()) { ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", p.err.c_str()); return; }
    static char flt[64] = "";
    ImGui::SameLine(); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14); ImGui::InputTextWithHint("##waf", "filter", flt, sizeof(flt));
    std::string f = Lower(flt);
    if (!ImGui::BeginTable("wsadv", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, 0))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Field"); ImGui::TableSetupColumn("Value"); ImGui::TableSetupColumn("Original");
    ImGui::TableHeadersRow();
    int shown = 0;
    for (auto& v : p.v) {
        if (!f.empty() && Lower(v.name).find(f) == std::string::npos && Lower(v.object).find(f) == std::string::npos) continue;
        if (++shown > 2000) break;
        WsEditRow(w, part, p, v, nullptr);
    }
    ImGui::EndTable();
}

static void TabWorkshop() {
    ImGui::TextWrapped("Make a new weapon from one that's already in the game, then change its stats and model. "
                       "FC4Hook copies the weapon's three archetypes (item, properties, pickup) into a new mod under new ids "
                       "and adds it to the Armory. Changes take effect after you restart the game.");
    if (!s_armoryLoaded) LoadArmory();
    if (!s_wsListed) { s_wsList = WorkshopList(); s_wsListed = true; }

    ImGui::SeparatorText("1. New weapon");
    static int base = -1; static char bf[64] = "", name[64] = "";
    const char* prev = base >= 0 && base < (int)s_weapons.size() ? s_weapons[base].name.c_str() : "choose the weapon to start from";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
    if (ImGui::BeginCombo("##wsbase", prev, ImGuiComboFlags_HeightLarge)) {
        ImGui::SetNextItemWidth(-1); ImGui::InputTextWithHint("##wsbf", "filter...", bf, sizeof(bf));
        std::string f = Lower(bf);
        for (int i = 0; i < (int)s_weapons.size(); ++i) {
            if (!f.empty() && Lower(s_weapons[i].name).find(f) == std::string::npos) continue;
            if (ImGui::Selectable((s_weapons[i].name + "##" + s_weapons[i].id).c_str(), base == i)) base = i;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::InputTextWithHint("##wsname", "new weapon's name", name, sizeof(name));
    ImGui::SameLine();
    ImGui::BeginDisabled(s_wsBusy || base < 0 || !name[0]);
    if (ImGui::Button("Create")) {
        s_wsBusy = true;
        uint64_t id = strtoull(s_weapons[base].id.c_str(), nullptr, 10);
        std::string nm = name;
        { std::lock_guard<std::mutex> lk(s_wsMx); s_wsMsg = "creating " + nm + "..."; }
        RunExtractJob([id, nm] {
            std::string mod, msg;
            WorkshopCreate(id, nm, &mod, &msg);
            { std::lock_guard<std::mutex> lk(s_wsMx); s_wsMsg = msg; }
            s_wsListed = false; s_wsBusy = false;
        });
    }
    ImGui::EndDisabled();
    { std::lock_guard<std::mutex> lk(s_wsMx); if (!s_wsMsg.empty()) ImGui::TextWrapped("%s", s_wsMsg.c_str()); }

    ImGui::SeparatorText("2. Your weapons");
    if (s_wsList.empty()) { ImGui::TextDisabled("None yet. Create one above."); return; }
    ImGui::TextDisabled("Game registration: %s", WorkshopRegisterStatus().c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Workshop weapons are added to the game's weapon -> properties table when you enter the world\n(and before every give). Without that the game can't use them.");
    std::vector<VfsModEntry> mods = VfsMods();
    for (int i = 0; i < (int)s_wsList.size(); ++i) {
        auto& w = s_wsList[i];
        bool loaded = false; for (auto& m : mods) if (m.name == w.mod && m.enabled) loaded = true;
        char lbl[200]; snprintf(lbl, sizeof(lbl), "%s%s##ws%d", w.name.c_str(), loaded ? "" : "   (restart the game to load it)", i);
        if (ImGui::Selectable(lbl, s_wsSel == i)) { s_wsSel = i; WsReload(); }
    }
    if (s_wsSel < 0 || s_wsSel >= (int)s_wsList.size()) return;
    WorkshopWeapon& w = s_wsList[s_wsSel];
    ImGui::Separator();
    ImGui::Text("%s", w.name.c_str()); ImGui::SameLine();
    ImGui::TextDisabled("mods\\%s   item %llu", w.mod.c_str(), (unsigned long long)w.ids[0]);
    ImGui::BeginDisabled(!LuaReady());
    if (ImGui::Button("Give it to me")) Lua(("FC4Armory.give('" + std::to_string(w.ids[0]) + "', [[" + LuaSafe(w.name) + "]])").c_str(), "=workshop");
    ImGui::SameLine(); if (ImGui::Button("Spawn pickup")) Lua(("FC4Spawn.spawnPickup('" + std::to_string(w.ids[0]) + "', [[" + LuaSafe(w.name) + "]])").c_str(), "=workshop");
    ImGui::EndDisabled();
    ImGui::SameLine(); if (ImGui::Button("Check files")) { uint64_t id = w.ids[0]; StartCheck([id] { return ArchetypeCheck(id); }); s_selectTab = "Spawner"; }
    ImGui::SameLine(); if (ImGui::Button("Open folder")) ShellExecuteW(nullptr, L"open", (g_rootDir + L"mods\\" + Widen(w.mod)).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (ImGui::BeginTabBar("wstabs")) {
        if (ImGui::BeginTabItem("Stats")) { WsStats(w); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Model")) { WsModel(w); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Advanced")) { WsAdvanced(w); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
}

// ------------------------------------------------------------- menu ------
static bool BeginTab(const char* name) {
    ImGuiTabItemFlags fl = 0;
    if (s_selectTab && strcmp(s_selectTab, name) == 0) { fl = ImGuiTabItemFlags_SetSelected; s_selectTab = nullptr; }
    return ImGui::BeginTabItem(name, nullptr, fl);
}

static void DrawMenu() {
    ImGui::GetStyle().FontScaleMain = g_cfg.uiScale;
    ImGui::SetNextWindowSize(ImVec2(760, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(FC4HOOK_TITLE "###fc4hook", &g_menuOpen)) { ImGui::End(); return; }

    ImGui::TextColored(LuaReady() ? ImVec4(0.4f, 1, 0.4f, 1) : ImVec4(1, 0.7f, 0.3f, 1), "Game scripts: %s", BridgeStatus());
    int warns = LogCount(LOG_WARN), errs = LogCount(LOG_ERROR);
    if (warns || errs) {
        ImGui::SameLine();
        ImGui::TextColored(errs ? ImVec4(1, 0.4f, 0.4f, 1) : ImVec4(1, 0.85f, 0.35f, 1), "   %d error%s, %d warning%s",
                           errs, errs == 1 ? "" : "s", warns, warns == 1 ? "" : "s");
        if (ImGui::IsItemClicked()) s_selectTab = "Console";
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to open the Console");
    }

    if (ImGui::BeginTabBar("tabs")) {
        if (BeginTab("Cheats"))   { TabCheats();   ImGui::EndTabItem(); }
        if (BeginTab("Armory"))   { TabArmory();   ImGui::EndTabItem(); }
        if (BeginTab("Workshop")) { TabWorkshop(); ImGui::EndTabItem(); }
        if (BeginTab("Travel"))   { TabTravel();   ImGui::EndTabItem(); }
        if (BeginTab("Spawner"))  { TabSpawner();  ImGui::EndTabItem(); }
        if (BeginTab("Co-op"))    { TabCoop();     ImGui::EndTabItem(); }
        if (BeginTab("Mods"))     { TabMods();     ImGui::EndTabItem(); }
        if (BeginTab("Console"))  { TabConsole();  ImGui::EndTabItem(); }
        if (BeginTab("Settings")) { TabSettings(); ImGui::EndTabItem(); }
        if (g_cfg.developerMode && BeginTab("Developer")) { TabDeveloper(); ImGui::EndTabItem(); }
        if (BeginTab("About"))    { TabAbout();    ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ------------------------------------------------------------- hooks -----
static void CreateRTV(IDXGISwapChain* sc) {
    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb) {
        s_dev->CreateRenderTargetView(bb, nullptr, &s_rtv);
        bb->Release();
    }
}

static void TeardownBackends() {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    if (s_hwnd && IsWindow(s_hwnd) && s_origWndProc &&
        (WNDPROC)GetWindowLongPtrW(s_hwnd, GWLP_WNDPROC) == HookWndProc)
        SetWindowLongPtrW(s_hwnd, GWLP_WNDPROC, (LONG_PTR)s_origWndProc);
    s_origWndProc = nullptr;
    if (s_rtv) { s_rtv->Release(); s_rtv = nullptr; }
    if (s_ctx) { s_ctx->Release(); s_ctx = nullptr; }
    if (s_dev) { s_dev->Release(); s_dev = nullptr; }
    s_hwnd = nullptr;
    s_sc = nullptr;
    s_imguiInit = false;
}

static void DrawBanner() {
    static ULONGLONG bannerUntil = GetTickCount64() + 20000;
    if (!g_cfg.showBanner || g_menuOpen || GetTickCount64() >= bannerUntil) return;
    char txt[160];
    snprintf(txt, sizeof(txt), "%s loaded - press %s for the menu", FC4HOOK_TITLE, KeyName(g_cfg.menuKey).c_str());
    ImVec2 sz = ImGui::CalcTextSize(txt);
    auto* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(8, 8), ImVec2(24 + sz.x, 16 + sz.y + 8), IM_COL32(0, 0, 0, 170), 4.f);
    dl->AddText(ImVec2(16, 12), IM_COL32(120, 255, 120, 255), txt);
}

// Plugin render callbacks (API v3): called every frame on the render thread, before the game's
// Present, after FC4Hook's menu has been drawn. Device/context are the game's.
static std::mutex s_presentMx;
static std::vector<void (*)(void*)> s_presentCbs;
void AddPresentCallback(void (*cb)(void* swapChain)) { if (cb) { std::lock_guard<std::mutex> lk(s_presentMx); s_presentCbs.push_back(cb); } }
void* D3DDevice()  { return s_dev; }
void* D3DContext() { return s_ctx; }
void* D3DSwapChain() { return s_sc; }

static void RunPresentCallbacks(IDXGISwapChain* sc) {
    std::vector<void (*)(void*)> cbs;
    { std::lock_guard<std::mutex> lk(s_presentMx); if (s_presentCbs.empty()) return; cbs = s_presentCbs; }
    for (auto cb : cbs) cb(sc);
}

static HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (++g_frameCounter == 1) LogL(LOG_DEBUG, "First Present() call seen (swapchain %p)", sc);

    // FC4 recreates its swapchain going from the front-end into the world.
    if (s_imguiInit && sc != s_sc) {
        if (sc == s_candidate) ++s_candidateFrames; else { s_candidate = sc; s_candidateFrames = 1; }
        if (s_candidateFrames < 30) return o_Present(sc, sync, flags);
        LogL(LOG_DEBUG, "Swapchain changed (%p -> %p); re-attaching overlay", s_sc, sc);
        TeardownBackends();
    }
    if (sc == s_sc) s_candidateFrames = 0;

    if (!s_imguiInit) {
        if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&s_dev))) {
            s_dev->GetImmediateContext(&s_ctx);
            DXGI_SWAP_CHAIN_DESC d; sc->GetDesc(&d);
            s_hwnd = d.OutputWindow;
            if (!ImGui::GetCurrentContext()) {
                IMGUI_CHECKVERSION();
                ImGui::CreateContext();
                ImGuiIO& io = ImGui::GetIO();
                static std::string ini = Narrow(g_rootDir) + "imgui.ini";
                io.IniFilename = ini.c_str();
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;   // D-pad / A / B navigate the menu
                ImGui::StyleColorsDark();
                ImGuiStyle& st = ImGui::GetStyle();
                st.WindowRounding = 6; st.FrameRounding = 4; st.TabRounding = 4; st.GrabRounding = 3;
                st.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.45f, 0.18f, 0.08f, 1);
                st.Colors[ImGuiCol_Tab] = ImVec4(0.30f, 0.14f, 0.08f, 1);
                st.Colors[ImGuiCol_TabSelected] = ImVec4(0.62f, 0.26f, 0.10f, 1);
                st.Colors[ImGuiCol_TabHovered] = ImVec4(0.75f, 0.33f, 0.13f, 1);
                st.Colors[ImGuiCol_Header] = ImVec4(0.40f, 0.18f, 0.09f, 1);
                st.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.60f, 0.27f, 0.12f, 1);
                st.Colors[ImGuiCol_Button] = ImVec4(0.42f, 0.20f, 0.10f, 1);
                st.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.66f, 0.30f, 0.12f, 1);
                st.Colors[ImGuiCol_CheckMark] = ImVec4(1.0f, 0.62f, 0.25f, 1);
                st.Colors[ImGuiCol_SliderGrab] = ImVec4(0.85f, 0.45f, 0.18f, 1);
            }
            ImGui_ImplWin32_Init(s_hwnd);
            ImGui_ImplDX11_Init(s_dev, s_ctx);
            s_origWndProc = (WNDPROC)SetWindowLongPtrW(s_hwnd, GWLP_WNDPROC, (LONG_PTR)HookWndProc);
            CreateRTV(sc);
            s_sc = sc;
            s_imguiInit = true;
            Log("Overlay attached (%ux%u)", d.BufferDesc.Width, d.BufferDesc.Height);
        }
    }
    if (s_imguiInit) {
        if (!s_rtv) CreateRTV(sc);
        ImGui_ImplDX11_NewFrame();
        PadPassThrough(true);              // ImGui reads the real pad (the game gets an idle one while the menu is open)
        ImGui_ImplWin32_NewFrame();
        PadPassThrough(false);
        PollHotkeys();
        if (g_menuOpen) PollMouse();
        ImGui::NewFrame();
        ImGui::GetIO().MouseDrawCursor = g_menuOpen;

        DrawBanner();
        if (g_menuOpen) {
            DrawMenu();
            std::vector<void (*)()> cbs;
            { std::lock_guard<std::mutex> lk(s_menuMx); cbs = s_menuCbs; }
            for (auto cb : cbs) cb();
        } else {
            s_captureKey = 0;
        }
        LuaUiDraw(g_menuOpen);             // Lua 5.4 scripts' windows (overlay ones also with the menu closed)
        ImGui::Render();
        if (s_rtv) {
            ID3D11RenderTargetView* oldRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
            ID3D11DepthStencilView* oldDsv = nullptr;
            s_ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, &oldDsv);
            s_ctx->OMSetRenderTargets(1, &s_rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            s_ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, oldDsv);
            for (auto* r : oldRtv) if (r) r->Release();
            if (oldDsv) oldDsv->Release();
        }
        if (sc == s_sc) RunPresentCallbacks(sc);
    }
    return o_Present(sc, sync, flags);
}

static HRESULT STDMETHODCALLTYPE hk_Resize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    if (sc == s_sc && s_rtv) { s_rtv->Release(); s_rtv = nullptr; }
    return o_Resize(sc, n, w, h, f, fl);
}

bool OverlayInit() {
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr),
                       nullptr, nullptr, nullptr, nullptr, L"fc4hook_dummy", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                           D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr)) { LogL(LOG_ERROR, "Overlay: dummy device failed (0x%08lx)", hr); DestroyWindow(hwnd); return false; }

    void** vt = *(void***)sc;
    void* present = vt[8];
    void* resize  = vt[13];
    sc->Release(); dev->Release(); ctx->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    bool ok = CreateHook(present, (void*)hk_Present, (void**)&o_Present) &&
              CreateHook(resize,  (void*)hk_Resize,  (void**)&o_Resize);
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    CreateHook((void*)GetProcAddress(u32, "SetCursorPos"), (void*)hk_SetCursorPos, (void**)&o_SetCursorPos);
    CreateHook((void*)GetProcAddress(u32, "ClipCursor"),   (void*)hk_ClipCursor,   (void**)&o_ClipCursor);
    if (ok) LogL(LOG_DEBUG, "Overlay hooks installed"); else LogL(LOG_ERROR, "Overlay hooks FAILED: no in-game menu");
    return ok;
}

} // namespace fc4
