// Weapons page (the armory): lists weapons from armory/weapons.txt plus each mod's armory.txt, gives them to the player, keeps favourites, and applies attachments.
// Shared widgets and cross-page helpers are declared in menu_common.h.
#include "menu_common.h"

namespace fc4 {

static std::vector<Weapon> s_weapons;
static void LoadArmory();
static std::vector<std::string> s_weaponCats;
static std::set<std::string> s_favs;
static bool s_armoryLoaded = false;

const std::vector<Weapon>& ArmoryWeapons() {
    if (!s_armoryLoaded) LoadArmory();
    return s_weapons;
}

std::string Trim(const std::string& x) {
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
static void DrawAttachmentList() {
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

static void DrawArmoryWeaponList();
void DrawWeaponsPage() {
    if (ImGui::BeginTabBar("armorytabs")) {
        if (ImGui::BeginTabItem("Weapons")) { DrawArmoryWeaponList(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Attachments")) { DrawAttachmentList(); ImGui::EndTabItem(); }
        else SetState("att.watch", "0");
        ImGui::EndTabBar();
    }
}

static void DrawArmoryWeaponList() {
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


} // namespace fc4
