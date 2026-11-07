// Console page: the log viewer and a one-line box that runs game Lua 5.1 or Lua 5.4 code.
// Shared widgets and cross-page helpers are declared in menu_common.h.
#include "menu_common.h"

namespace fc4 {

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

void DrawConsolePage() {
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


} // namespace fc4
