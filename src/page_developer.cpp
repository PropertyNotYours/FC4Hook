// Developer pages (Lua, Natives, Memory, Animation, File Activity, Co-op Debug, Diagnostics). DrawDeveloperPage draws whichever one the sidebar selected. Also holds the helpers that run script files and call natives.
// Shared widgets and cross-page helpers are declared in menu_common.h.
#include "menu_common.h"

namespace fc4 {

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

void RunScriptWithPrelude(const std::wstring& name, const std::string& prelude) {
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
void ProbeNative(const std::string& fn, const std::string& args) {
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

// One developer page; the sidebar passes the name of the page that is selected.
static bool DevSec(const char* only, const char* name) { return strcmp(only, name) == 0; }

void DrawDeveloperPage(const char* only) {
    if (DevSec(only, "Diagnostics")) {
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
    }
    if (DevSec(only, "Memory")) { DrawMemoryPage(); }
    if (DevSec(only, "File Activity")) { DrawFileActivityPage(); }
    if (DevSec(only, "Animation")) { DrawAnimationPage(); }
    if (DevSec(only, "Lua")) {
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
    }
    if (DevSec(only, "Natives")) {
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
    }
    if (DevSec(only, "Lua")) {
        ImGui::SeparatorText("Game scripts");
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
    }
    if (DevSec(only, "Co-op Debug")) {
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
    }
}


} // namespace fc4
