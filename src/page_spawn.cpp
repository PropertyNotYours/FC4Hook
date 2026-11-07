// Spawn page: weapon pickups, plus a catalogue of people, animals and vehicles to spawn, and a checker that explains why an archetype will not spawn.
// Shared widgets and cross-page helpers are declared in menu_common.h.
#include "menu_common.h"

namespace fc4 {

// Armory weapons, spawned as the pickup you can walk up to and take (FC4Spawn.spawnPickup finds
// the pickup archetype through the weapon's WeaponProperties).
static void SpawnWeapons() {
    static char wf[64] = "";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    ImGui::InputTextWithHint("##wf", "filter...", wf, sizeof(wf));
    ImGui::SameLine(); ImGui::TextDisabled("spawned as pickups: walk up and take them");
    ImGui::BeginChild("spw", ImVec2(0, 0), true);
    std::string f = Lower(wf);
    ImGui::BeginDisabled(!LuaReady());
    for (auto& w : ArmoryWeapons()) {
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

void StartArchetypeCheck(std::function<std::string()> job) {
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
    if (ImGui::Button("Check")) { uint64_t id = strtoull(cid, nullptr, 10); StartArchetypeCheck([id] { return ArchetypeCheck(id); }); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(s_chkBusy);
    if (ImGui::Button("Check every archetype in my mods")) StartArchetypeCheck([] { return ArchetypeCheckMods(); });
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

void DrawSpawnPage() {
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


} // namespace fc4
