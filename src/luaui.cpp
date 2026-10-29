// fc4.ui: Lua 5.4 scripts build their own ImGui windows inside FC4Hook's overlay.
//
// Lua runs on the game's script thread, ImGui on the render thread, so scripts don't call
// ImGui directly: they DECLARE windows and widgets (fc4.ui.window(...):button(...)); this file
// keeps the description, draws it every frame, and queues what the player does (clicks,
// edits). Lua54RunJobs hands the queue to fc4.ui._event on the script thread, which calls the
// widget's Lua callback. Safe from either side, and a broken script can't crash the renderer.
//
// Normal windows show while the F8 menu is open. Overlay windows (opts.overlay = true) are
// drawn all the time, click-through while the menu is closed: for HUD-style mod info.
#include "core.h"
#include "imgui.h"
extern "C++" {
#include "lua.h"
#include "lauxlib.h"
}
#include <map>
#include <mutex>
#include <array>
#include <cstring>

namespace fc4 {

enum UiKind { UI_TEXT, UI_BUTTON, UI_CHECKBOX, UI_SLIDER, UI_INPUT, UI_COMBO, UI_PROGRESS, UI_SEPARATOR };

struct UiWidget {
    int id = 0; UiKind kind = UI_TEXT;
    std::string label;
    float lo = 0, hi = 1, value = 0;      // slider/progress value, checkbox 0/1, combo index (0-based)
    bool sameLine = false;
    std::array<char, 256> text{};          // input contents
    std::vector<std::string> items;        // combo items
};

struct UiWindow {
    int id = 0; std::string title;
    bool visible = true, overlay = false;
    float w = 0, h = 0;
    std::vector<UiWidget> widgets;
};

static std::mutex s_uiMx;
static std::map<int, UiWindow> s_windows;
static std::vector<UiEvent> s_events;
static int s_nextId = 1;

static UiWidget* FindWidget(int id) {
    for (auto& kv : s_windows) for (auto& w : kv.second.widgets) if (w.id == id) return &w;
    return nullptr;
}

static UiKind KindFrom(const char* s) {
    static const char* names[] = { "text", "button", "checkbox", "slider", "input", "combo", "progress", "separator" };
    for (int i = 0; i < 8; ++i) if (strcmp(s, names[i]) == 0) return (UiKind)i;
    return UI_TEXT;
}

// fc4.ui._window(title, overlay, width, height) -> id
static int u_window(lua_State* L) {
    UiWindow w;
    w.title = luaL_checkstring(L, 1);
    w.overlay = lua_toboolean(L, 2) != 0;
    w.w = (float)luaL_optnumber(L, 3, 0); w.h = (float)luaL_optnumber(L, 4, 0);
    std::lock_guard<std::mutex> lk(s_uiMx);
    if (s_windows.size() >= 64) return luaL_error(L, "fc4.ui: too many windows (64)");
    w.id = s_nextId++;
    s_windows[w.id] = w;
    lua_pushinteger(L, w.id);
    return 1;
}

// fc4.ui._add(window, kind, label, a, b, c, sameLine) -> widget id
static int u_add(lua_State* L) {
    int win = (int)luaL_checkinteger(L, 1);
    UiWidget w;
    w.kind = KindFrom(luaL_checkstring(L, 2));
    w.label = luaL_optstring(L, 3, "");
    float a = (float)luaL_optnumber(L, 4, 0), b = (float)luaL_optnumber(L, 5, 0), c = (float)luaL_optnumber(L, 6, 0);
    w.sameLine = lua_toboolean(L, 7) != 0;
    switch (w.kind) {
    case UI_SLIDER: w.lo = a; w.hi = b > a ? b : a + 1; w.value = c; break;
    case UI_CHECKBOX: w.value = a != 0 ? 1.f : 0.f; break;
    case UI_COMBO: w.value = a > 0 ? a - 1 : 0; break;      // Lua index is 1-based
    case UI_PROGRESS: w.value = a; break;
    default: break;
    }
    std::lock_guard<std::mutex> lk(s_uiMx);
    auto it = s_windows.find(win);
    if (it == s_windows.end()) return luaL_error(L, "fc4.ui: no such window");
    if (it->second.widgets.size() >= 256) return luaL_error(L, "fc4.ui: too many widgets in one window (256)");
    w.id = s_nextId++;
    it->second.widgets.push_back(w);
    lua_pushinteger(L, w.id);
    return 1;
}

// fc4.ui._set(widget, value): text/button/separator -> label, checkbox -> bool, slider/progress
// -> number, input -> text, combo -> 1-based index
static int u_set(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    std::lock_guard<std::mutex> lk(s_uiMx);
    UiWidget* w = FindWidget(id);
    if (!w) return 0;
    switch (w->kind) {
    case UI_CHECKBOX: w->value = lua_toboolean(L, 2) ? 1.f : 0.f; break;
    case UI_SLIDER: case UI_PROGRESS: w->value = (float)luaL_checknumber(L, 2); break;
    case UI_COMBO: w->value = (float)(luaL_checkinteger(L, 2) - 1); break;
    case UI_INPUT: { const char* s = luaL_checkstring(L, 2); snprintf(w->text.data(), w->text.size(), "%s", s); break; }
    default: w->label = luaL_tolstring(L, 2, nullptr); lua_pop(L, 1); break;
    }
    return 0;
}

static int u_get(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    std::lock_guard<std::mutex> lk(s_uiMx);
    UiWidget* w = FindWidget(id);
    if (!w) return 0;
    switch (w->kind) {
    case UI_CHECKBOX: lua_pushboolean(L, w->value != 0); break;
    case UI_SLIDER: case UI_PROGRESS: lua_pushnumber(L, w->value); break;
    case UI_COMBO: lua_pushinteger(L, (lua_Integer)w->value + 1); break;
    case UI_INPUT: lua_pushstring(L, w->text.data()); break;
    default: lua_pushstring(L, w->label.c_str()); break;
    }
    return 1;
}

static int u_items(lua_State* L) {                       // fc4.ui._items(combo, "a\nb\nc")
    int id = (int)luaL_checkinteger(L, 1);
    std::string all = luaL_checkstring(L, 2);
    std::lock_guard<std::mutex> lk(s_uiMx);
    UiWidget* w = FindWidget(id);
    if (!w) return 0;
    w->items.clear();
    size_t p = 0;
    while (p <= all.size()) { size_t e = all.find('\n', p); if (e == std::string::npos) e = all.size(); w->items.push_back(all.substr(p, e - p)); p = e + 1; }
    return 0;
}

static int u_show(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    std::lock_guard<std::mutex> lk(s_uiMx);
    auto it = s_windows.find(id);
    if (it != s_windows.end()) it->second.visible = lua_toboolean(L, 2) != 0;
    return 0;
}

static int u_remove(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    std::lock_guard<std::mutex> lk(s_uiMx);
    s_windows.erase(id);
    return 0;
}

void LuaUiOpen(lua_State* L) {                            // fc4 table on top of the stack
    static const luaL_Reg f[] = { { "_window", u_window }, { "_add", u_add }, { "_set", u_set }, { "_get", u_get },
                                  { "_items", u_items }, { "_show", u_show }, { "_remove", u_remove }, { nullptr, nullptr } };
    lua_newtable(L);
    luaL_setfuncs(L, f, 0);
    lua_setfield(L, -2, "ui");
}

void LuaUiReset() {                                       // the 5.4 state restarted: its windows are gone
    std::lock_guard<std::mutex> lk(s_uiMx);
    s_windows.clear();
    s_events.clear();
}

std::vector<UiEvent> LuaUiTakeEvents() {
    std::lock_guard<std::mutex> lk(s_uiMx);
    std::vector<UiEvent> e;
    e.swap(s_events);
    return e;
}

static void Push(int id, double n, const char* s = nullptr) {
    UiEvent e; e.id = id; e.num = n; e.isText = s != nullptr; if (s) e.text = s;
    if (s_events.size() < 1024) s_events.push_back(e);
}

static void DrawWidget(UiWidget& w) {                     // s_uiMx held
    if (w.sameLine) ImGui::SameLine();
    ImGui::PushID(w.id);
    switch (w.kind) {
    case UI_TEXT: ImGui::TextWrapped("%s", w.label.c_str()); break;
    case UI_SEPARATOR: if (w.label.empty()) ImGui::Separator(); else ImGui::SeparatorText(w.label.c_str()); break;
    case UI_BUTTON: if (ImGui::Button(w.label.c_str())) Push(w.id, 1); break;
    case UI_CHECKBOX: { bool b = w.value != 0; if (ImGui::Checkbox(w.label.c_str(), &b)) { w.value = b ? 1.f : 0.f; Push(w.id, b ? 1 : 0); } break; }
    case UI_SLIDER:
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        ImGui::SliderFloat(w.label.c_str(), &w.value, w.lo, w.hi, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) Push(w.id, w.value);
        break;
    case UI_INPUT:
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
        if (ImGui::InputText(w.label.c_str(), w.text.data(), w.text.size(), ImGuiInputTextFlags_EnterReturnsTrue)) Push(w.id, 0, w.text.data());
        else if (ImGui::IsItemDeactivatedAfterEdit()) Push(w.id, 0, w.text.data());
        break;
    case UI_COMBO: {
        int cur = (int)w.value;
        const char* preview = cur >= 0 && cur < (int)w.items.size() ? w.items[cur].c_str() : "";
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        if (ImGui::BeginCombo(w.label.c_str(), preview)) {
            for (int i = 0; i < (int)w.items.size(); ++i)
                if (ImGui::Selectable(w.items[i].c_str(), i == cur)) { w.value = (float)i; Push(w.id, i + 1); }
            ImGui::EndCombo();
        }
        break;
    }
    case UI_PROGRESS: ImGui::ProgressBar(w.value, ImVec2(ImGui::GetFontSize() * 14, 0), w.label.empty() ? nullptr : w.label.c_str()); break;
    }
    ImGui::PopID();
}

// Render thread, inside the ImGui frame.
void LuaUiDraw(bool menuOpen) {
    std::lock_guard<std::mutex> lk(s_uiMx);
    for (auto& kv : s_windows) {
        UiWindow& w = kv.second;
        if (!w.visible || (!menuOpen && !w.overlay)) continue;
        ImGuiWindowFlags fl = 0;
        if (w.overlay && !menuOpen) fl |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;
        if (w.overlay) fl |= ImGuiWindowFlags_AlwaysAutoResize;
        if (w.w > 0 && w.h > 0) ImGui::SetNextWindowSize(ImVec2(w.w, w.h), ImGuiCond_FirstUseEver);
        else ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
        if (w.overlay && !menuOpen) ImGui::SetNextWindowBgAlpha(0.55f);
        std::string title = w.title + "###fc4ui" + std::to_string(w.id);
        if (ImGui::Begin(title.c_str(), nullptr, fl))
            for (auto& wd : w.widgets) DrawWidget(wd);
        ImGui::End();
    }
}

int LuaUiWindowCount() { std::lock_guard<std::mutex> lk(s_uiMx); return (int)s_windows.size(); }

} // namespace fc4
