// Co-op page: buttons for the co-op tower reward fix (10_coop_towers.lua).
// Shared widgets and cross-page helpers are declared in menu_common.h.
#include "menu_common.h"

namespace fc4 {


void DrawCoopPage() {
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


} // namespace fc4
