// The overlay: hooks the game's D3D11 Present, starts Dear ImGui, polls keyboard / mouse / gamepad,
// and draws the menu frame (sidebar + the selected page). The pages themselves are in page_*.cpp.
#include "menu_common.h"
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
int                            g_captureKey = 0;        // >0: Settings is waiting for a key press for slot N
static const char*             s_selectTab = nullptr;   // page to jump to on the next frame
void ShowMenuPage(const char* pageName) { s_selectTab = pageName; }

void AddMenuCallback(void (*cb)()) { std::lock_guard<std::mutex> lk(s_menuMx); s_menuCbs.push_back(cb); }
void* GetImGuiContextPtr() { return ImGui::GetCurrentContext(); }

bool MenuWantsKeyboard() { return g_menuOpen && ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput; }
// True while the game window (or any window of this process) is the foreground window.
bool GameHasFocus() {
    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
    return foregroundPid == GetCurrentProcessId();
}
static bool GameFocused() { return GameHasFocus(); }

// ------------------------------------------------------------ input ------
// Keys and mouse are polled (the game uses raw input, so window messages don't always arrive).
// Edge-detected key press; safe to ask about the same key several times in one frame.
bool KeyPressed(int vk) {
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
    if (!focused || g_captureKey) return;
    if (menu) { g_menuOpen = !g_menuOpen; LogL(LOG_DEBUG, "Menu %s", g_menuOpen ? "opened" : "closed"); }
    bool typing = g_menuOpen && ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput;
    if (typing || !LuaReady()) return;
    if (god)    Lua("if FC4Cheats then FC4Cheats.toggle('god') end", "=hotkey");
    if (ammo)   Lua("if FC4Cheats then FC4Cheats.toggle('ammo') end", "=hotkey");
    if (refill) Lua("if FC4Cheats then FC4Cheats.refill() end", "=hotkey");
    if (slow)   Lua("if FC4Cheats then FC4Cheats.toggle('slowmo') end", "=hotkey");
    if (armory) { g_menuOpen = true; s_selectTab = "Weapons"; }
    if (fly)    Lua("if FC4Cheats then FC4Cheats.toggle('fly') end", "=hotkey");
    if (hud)    Lua("if FC4Cheats then FC4Cheats.toggle('hud') end", "=hotkey");
    if (brake)  Lua("if FC4Cheats then FC4Cheats.brake() end", "=hotkey");
}

// Feeds the real mouse into ImGui. GetAsyncKeyState reports clicks anywhere on the desktop, so when
// the game is not the foreground window we tell ImGui the mouse has left instead; otherwise clicks
// on other windows would press menu buttons hidden behind them.
static void PollMouse(bool gameFocused) {
    ImGuiIO& io = ImGui::GetIO();
    if (!gameFocused) {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        for (int button = 0; button < 3; ++button) io.AddMouseButtonEvent(button, false);
        return;
    }
    POINT cursor;
    if (GetCursorPos(&cursor) && ScreenToClient(s_hwnd, &cursor)) io.AddMousePosEvent((float)cursor.x, (float)cursor.y);
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

// ------------------------------------------------------------- menu ------
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

    // The sidebar groups pages by what the player is trying to do. Developer pages only appear
    // once "Developer mode" is switched on in Settings.
    struct Page { const char* group; const char* name; bool developerOnly; };
    static const Page pages[] = {
        { "Play",      "Cheats",           false },
        { "Play",      "Weapons",          false },
        { "Play",      "Teleport",         false },
        { "Play",      "Spawn",            false },
        { "Play",      "Co-op",            false },
        { "Mods",      "Weapon Workshop",  false },
        { "Mods",      "Models & Textures", false },
        { "Mods",      "Installed Mods",   false },
        { "Developer", "Lua",              true  },
        { "Developer", "Natives",          true  },
        { "Developer", "Memory",           true  },
        { "Developer", "Animation",        true  },
        { "Developer", "File Activity",    true  },
        { "Developer", "Co-op Debug",      true  },
        { "Developer", "Diagnostics",      true  },
        { "FC4Hook",   "Console",          false },
        { "FC4Hook",   "Settings",         false },
        { "FC4Hook",   "About",            false },
    };
    static std::string page = "Cheats";
    if (s_selectTab) { page = s_selectTab; s_selectTab = nullptr; }
    bool pageIsValid = false;
    for (auto& entry : pages)
        if (page == entry.name && (!entry.developerOnly || g_cfg.developerMode)) pageIsValid = true;
    if (!pageIsValid) page = "Cheats";

    float navW = ImGui::GetFontSize() * 9.5f;
    ImGui::BeginChild("nav", ImVec2(navW, 0), true);
    const char* lastGroup = "";
    for (auto& entry : pages) {
        if (entry.developerOnly && !g_cfg.developerMode) continue;
        if (strcmp(lastGroup, entry.group) != 0) {
            if (lastGroup[0]) ImGui::Spacing();
            ImGui::TextDisabled("%s", entry.group);
            lastGroup = entry.group;
        }
        if (ImGui::Selectable(entry.name, page == entry.name)) page = entry.name;
    }
    if (!g_cfg.developerMode) { ImGui::Spacing(); ImGui::TextDisabled("More tools: turn on"); ImGui::TextDisabled("Developer mode in Settings"); }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("page", ImVec2(0, 0), false);
    ImGui::Text("%s", page.c_str());
    ImGui::Separator();
    const std::string& P = page;
    if      (P == "Cheats")            DrawCheatsPage();
    else if (P == "Weapons")           DrawWeaponsPage();
    else if (P == "Teleport")          DrawTeleportPage();
    else if (P == "Spawn")             DrawSpawnPage();
    else if (P == "Co-op")             DrawCoopPage();
    else if (P == "Weapon Workshop")   DrawWorkshopPage();
    else if (P == "Models & Textures") DrawModelsPage();
    else if (P == "Installed Mods")    DrawInstalledModsPage();
    else if (P == "Console")           DrawConsolePage();
    else if (P == "Settings")          DrawSettingsPage();
    else if (P == "About")             DrawAboutPage();
    else                               DrawDeveloperPage(P.c_str());
    ImGui::EndChild();
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

// Segoe UI ships with every Windows install and is far easier to read than ImGui's built-in
// pixel font. If it is missing we silently keep the built-in one.
static void LoadMenuFont(ImGuiIO& io) {
    wchar_t windowsDir[MAX_PATH] = {};
    if (!GetWindowsDirectoryW(windowsDir, MAX_PATH)) return;
    std::string fontPath = Narrow(std::wstring(windowsDir) + L"\\Fonts\\segoeui.ttf");
    if (GetFileAttributesA(fontPath.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 18.0f);
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
                LoadMenuFont(io);
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
        const bool gameFocused = GameFocused();
        if (g_menuOpen) PollMouse(gameFocused);
        ImGui::NewFrame();
        ImGui::GetIO().MouseDrawCursor = g_menuOpen && gameFocused;

        DrawBanner();
        if (g_menuOpen) {
            DrawMenu();
            std::vector<void (*)()> cbs;
            { std::lock_guard<std::mutex> lk(s_menuMx); cbs = s_menuCbs; }
            for (auto cb : cbs) cb();
        } else {
            g_captureKey = 0;
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

