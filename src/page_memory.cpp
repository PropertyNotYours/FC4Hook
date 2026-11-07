// Developer > Memory page: watch list, freeze values, and a hex viewer over the game's memory (memory.cpp does the reading and writing).
// Shared widgets and cross-page helpers are declared in menu_common.h.
#include "menu_common.h"

namespace fc4 {

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

void DrawMemoryPage() {
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


} // namespace fc4
