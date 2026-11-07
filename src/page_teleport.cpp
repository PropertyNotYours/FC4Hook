// Teleport page: jump to liberated bell towers and saved positions, and run the tower tour. The work is done by 20_tower_travel.lua.
// Shared widgets and cross-page helpers are declared in menu_common.h.
#include "menu_common.h"

namespace fc4 {

void DrawTeleportPage() {
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


} // namespace fc4
