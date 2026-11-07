// Installed Mods page: every folder in fc4hook\mods with on/off switches, plus a browser for the loose files the mods provide.
// Shared widgets and cross-page helpers are declared in menu_common.h.
#include "menu_common.h"

namespace fc4 {

void DrawInstalledModsPage() {
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


} // namespace fc4
