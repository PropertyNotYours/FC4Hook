// Weapon Workshop page: clone any weapon into a new mod, then edit its stats, model, arm animations and raw properties (workshop.cpp, fcb.cpp).
// Shared widgets and cross-page helpers are declared in menu_common.h.
#include "menu_common.h"

namespace fc4 {

// ------------------------------------------------------------- workshop --
// Make new weapons from existing ones and change their stats / model (workshop.cpp).
static std::mutex s_wsMx;
static std::string s_wsMsg;
static std::atomic<bool> s_wsBusy{ false };
static std::vector<WorkshopWeapon> s_wsList;
static bool s_wsListed = false;
static int s_wsSel = -1;
struct WsPart { std::vector<uint8_t> d, base; std::vector<FcbValue> v; std::string err; bool loaded = false; };
static WsPart s_wsParts[3];

static void WsReload() { for (auto& p : s_wsParts) p = WsPart(); }
static WsPart& WsGet(const WorkshopWeapon& w, int part) {
    WsPart& p = s_wsParts[part];
    if (!p.loaded) {
        p.loaded = true;
        if (WorkshopRead(w, part, p.d, p.v, &p.err)) {
            if (!WorkshopReadBase(w, part, p.base) || p.base.size() != p.d.size()) p.base.clear();
        }
    }
    return p;
}

// The stats most people want, with plain-English labels.
static const struct { const char* field; const char* label; } kWsCommon[] = {
    { "iFireRate", "Fire rate (rounds per minute)" }, { "iAmmoInClip", "Magazine size" },
    { "AmmoUsedPerShot", "Ammo used per shot" }, { "fReloadTimeNormal", "Reload time (s)" },
    { "fReloadTimeForced", "Reload time, empty (s)" }, { "fRange", "Range (m)" },
    { "fDrawTime", "Draw time (s)" }, { "fHolsterTime", "Holster time (s)" },
    { "fVerticalRecoilPerShot", "Recoil up per shot" }, { "fLeftRecoilPerShot", "Recoil left per shot" },
    { "fRightRecoilPerShot", "Recoil right per shot" }, { "fRecoilMaxPitch", "Max recoil (up)" },
    { "fMoveSpeedFactor", "Move speed while held" }, { "fMoveSpeedFactorShooting", "Move speed while shooting" },
    { "bIsSilent", "Silent" }, { "bHasInfiniteAmmo", "Infinite ammo" }, { "bAutoReload", "Auto reload" },
    { "fUnjamTime", "Unjam time (s)" }, { "fNearFovDegrees", "Weapon FOV (degrees)" },
};

static char WsKind(const FcbValue& v, const std::vector<uint8_t>* d = nullptr) {
    char k = d ? FcbKindOf(*d, v) : FcbKind(v);
    if (!k && !v.shared && v.size == 4 && v.name == "AmmoUsedPerShot") k = 'i';
    return k;
}

static void WsEditRow(const WorkshopWeapon& w, int part, WsPart& p, const FcbValue& v, const char* label) {
    char k = WsKind(v, &p.d);
    ImGui::PushID((int)v.offset + part * 0x1000000);
    ImGui::TableNextRow();
    ImGui::TableNextColumn(); ImGui::TextUnformatted(label ? label : v.name.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", v.name.c_str(), v.object.c_str());
    ImGui::TableNextColumn();
    std::string cur = FcbFormat(p.d, v);
    bool changed = false; std::string nv;
    if (k == 'b') {
        bool b = p.d[v.offset] != 0;
        if (ImGui::Checkbox("##b", &b)) { changed = true; nv = b ? "1" : "0"; }
    } else if (k == 'f' || k == 'i') {
        char buf[32]; snprintf(buf, sizeof(buf), "%s", cur.c_str());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::InputText("##v", buf, sizeof(buf), k == 'f' ? ImGuiInputTextFlags_CharsScientific : ImGuiInputTextFlags_CharsDecimal);
        if (ImGui::IsItemDeactivatedAfterEdit()) { changed = true; nv = buf; }
    } else ImGui::TextDisabled("%s", cur.c_str());
    ImGui::TableNextColumn();
    if (!p.base.empty()) {
        std::vector<uint8_t>& b = p.base;
        std::string bv = FcbFormat(b, v);
        if (bv != cur) ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "%s", bv.c_str()); else ImGui::TextDisabled("%s", bv.c_str());
    }
    if (changed) {
        std::string err;
        if (!WorkshopSet(w, part, v.offset, k, nv, &err)) { std::lock_guard<std::mutex> lk(s_wsMx); s_wsMsg = "Couldn't change " + v.name + ": " + err; }
        else { std::lock_guard<std::mutex> lk(s_wsMx); s_wsMsg = "Saved " + v.name + " = " + nv + ". Restart the game to use it."; }
        s_wsParts[part].loaded = false;
    }
    ImGui::PopID();
}

static void WsStats(const WorkshopWeapon& w) {
    WsPart& p = WsGet(w, 1);
    if (!p.err.empty()) { ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", p.err.c_str()); return; }
    static bool all = false; static char flt[64] = "";
    ImGui::Checkbox("show every value", &all);
    if (all) { ImGui::SameLine(); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14); ImGui::InputTextWithHint("##wsf", "filter, e.g. recoil, sway, ammo", flt, sizeof(flt)); }
    ImGui::TextDisabled("Orange in the last column = changed from the original weapon.");
    if (!ImGui::BeginTable("wsstats", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, 0))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Stat"); ImGui::TableSetupColumn("Value"); ImGui::TableSetupColumn("Original");
    ImGui::TableHeadersRow();
    if (!all) {
        for (auto& c : kWsCommon)
            for (auto& v : p.v) if (v.name == c.field && WsKind(v)) { WsEditRow(w, 1, p, v, c.label); break; }
    } else {
        std::string f = Lower(flt);
        std::string lastObj;
        for (auto& v : p.v) {
            if (!WsKind(v) || WsKind(v) == 'u') continue;
            if (!f.empty() && Lower(v.name).find(f) == std::string::npos && Lower(v.object).find(f) == std::string::npos) continue;
            if (v.object != lastObj) {
                lastObj = v.object;
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                size_t sl = v.object.rfind('/'); ImGui::TextDisabled("[%s]", sl == std::string::npos ? v.object.c_str() : v.object.c_str() + sl + 1);
            }
            WsEditRow(w, 1, p, v, nullptr);
        }
    }
    ImGui::EndTable();
}

// What the player typed into a model box -> the model's GAME path (that's what the game hashes).
// Accepts a game path, a full path to an .xbg inside any mod's files\ folder, or a full path to
// an .xbg anywhere else (copied into this mod).
static bool WsModelPathFromInput(const WorkshopWeapon& w, std::string in, std::string* gamePath, std::string* msg) {
    while (!in.empty() && (in.back() == ' ' || in.back() == '"')) in.pop_back();
    while (!in.empty() && (in[0] == ' ' || in[0] == '"')) in.erase(0, 1);
    for (auto& c : in) if (c == '/') c = '\\';
    std::string l = Lower(in);
    if (l.size() > 4 && l.compare(l.size() - 4, 4, ".glb") == 0) { *msg = "That's a .glb: use the \"Use my .glb\" box below to convert it."; return false; }
    if (l.size() < 5 || l.compare(l.size() - 4, 4, ".xbg") != 0) { *msg = "A model has to be an .xbg file."; return false; }
    if (l.find(':') == std::string::npos && l.rfind("\\\\", 0) != 0) {             // already a game path
        if (l.rfind("files\\", 0) == 0) l = l.substr(6);
        *gamePath = l;
    } else {
        size_t f = l.find("\\files\\");
        size_t mods = l.find("\\mods\\");
        if (f != std::string::npos && mods != std::string::npos && mods < f) *gamePath = l.substr(f + 7);   // inside a mod: its game path
        else {                                                                       // elsewhere: copy it into this mod
            std::string safe = Lower(w.mod); for (auto& c : safe) if (!isalnum((unsigned char)c)) c = '_';
            std::string name = l.substr(l.find_last_of('\\') + 1);
            *gamePath = "graphics\\fc4hook\\" + safe + "\\" + name;
            std::wstring dst = g_rootDir + L"mods\\" + Widen(w.mod) + L"\\files\\" + Widen(*gamePath);
            for (size_t k = 0; k < dst.size(); ++k) if (dst[k] == L'\\' && k > 2) CreateDirectoryW(dst.substr(0, k).c_str(), nullptr);
            if (!CopyFileW(Widen(in).c_str(), dst.c_str(), FALSE)) { *msg = "Couldn't copy " + in + " into the mod."; return false; }
            *msg = "Copied it into the mod. ";
        }
    }
    // does it exist (game, a loaded mod, or a file just put in a mod folder)?
    uint64_t id = FileId(gamePath->c_str());
    bool exists = GameFileExists(id);
    for (auto& f : VfsFiles()) if (f.id == id) exists = true;
    if (!exists) {
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((g_rootDir + L"mods\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do { if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.' &&
                     GetFileAttributesW((g_rootDir + L"mods\\" + fd.cFileName + L"\\files\\" + Widen(*gamePath)).c_str()) != INVALID_FILE_ATTRIBUTES) exists = true;
            } while (!exists && FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    if (!exists) { *msg = "There's no file " + *gamePath + " in the game or in any mod's files\\ folder."; return false; }
    return true;
}

// Workshop > Animations: the clips the game played (clip monitor), export them to Blender and replace them
// with your own .glb. Replacements go in this weapon's mod as files\__unknown\<clip id>.ani.

// "1stge_uppb_reloadregular_+000fw_sekimber" -> "Reload". Clip names come from Find clip names
// (dumps\clip_names.txt); unknown clips -> "".
static std::string FriendlyClip(uint64_t id) {
    std::string n = ClipNameOf(id);
    if (n.empty()) return "";
    std::vector<std::string> p; { std::string cur; for (char c : n) { if (c == '_') { p.push_back(cur); cur.clear(); } else cur += c; } p.push_back(cur); }
    // action = the parts after "<view>ge" and "<body part>", up to the "+000fw" angle part
    std::string act;
    for (size_t i = 2; i < p.size(); ++i) { if (!p[i].empty() && (p[i][0] == '+' || p[i][0] == '-')) break; act += (act.empty() ? "" : "_") + p[i]; }
    if (act.empty() && p.size() > 1) act = p[1];
    static const struct { const char* key; const char* name; } kA[] = {
        { "reloadregular", "Reload" }, { "reloadforced", "Reload (empty)" }, { "reloadreg", "Reload" }, { "reload", "Reload" },
        { "shootcycle", "Shoot" }, { "shootoffset", "Shoot recoil" }, { "shootempty", "Shoot (last shot)" }, { "shoot", "Shoot" },
        { "walk2sprint", "Walk to sprint" }, { "sprint2walk", "Sprint to walk" }, { "transaim2sprint", "Aim to sprint" },
        { "sprintcycle", "Sprint" }, { "sprint", "Sprint" }, { "jog", "Jog" }, { "wlk", "Walk" }, { "walk", "Walk" }, { "run", "Run" },
        { "draw", "Draw" }, { "holster", "Holster" }, { "unjam", "Unjam" }, { "inspect", "Inspect" }, { "melee", "Melee" },
        { "btheaim", "Aim (breathing)" }, { "aim", "Aim" }, { "idle", "Idle" }, { "jump", "Jump" }, { "land", "Land" },
        { "crh2std", "Crouch to stand" }, { "std2crh", "Stand to crouch" }, { "slide", "Slide" }, { "throw", "Throw" }, { "fire", "Fire" } };
    std::string l = act, out;
    for (auto& a : kA) if (l.find(a.key) != std::string::npos) { out = a.name; break; }
    if (out.empty()) { out = act; for (auto& c : out) if (c == '_') c = ' '; if (!out.empty()) out[0] = (char)toupper((unsigned char)out[0]); }
    std::string extra;
    if (l.find("iron") != std::string::npos) extra += ", aiming";
    if (l.find("zero") != std::string::npos) extra += ", last round";
    if (l.find("zip") != std::string::npos) extra += ", zipline";
    if (l.find("cover") != std::string::npos) extra += ", cover";
    if (l.find("cr") != std::string::npos && out.find("rouch") == std::string::npos && (l.find("wlkcr") != std::string::npos || l.find("crouch") != std::string::npos || l.find("aimingcr") != std::string::npos)) extra += ", crouched";
    if (l.find("rt") == l.size() - 2 && l.size() > 2) extra += ", right";
    if (l.find("lt") == l.size() - 2 && l.size() > 2) extra += ", left";
    if (!extra.empty()) out += " (" + extra.substr(2) + ")";
    return out;
}

static bool byNameUi = true;
static bool ReadFileW(const std::wstring& p, std::vector<uint8_t>& d) {
    FILE* f = _wfopen(p.c_str(), L"rb"); if (!f) return false;
    uint8_t b[65536]; size_t n; d.clear();
    while ((n = fread(b, 1, sizeof(b), f)) > 0) d.insert(d.end(), b, b + n);
    fclose(f); return true;
}
static void WsAnimations(const WorkshopWeapon& w) {
    ImGui::TextWrapped("Custom first-person animations. 1) Tick Watch and press Clear. 2) Close the menu, hold this weapon and do "
                       "what you want to change (draw, idle, fire, reload, sprint). 3) Come back: the clips the game played are listed, "
                       "newest first. Export one to Blender (.glb), edit it, export it as glTF Binary and give it here. Note: a clip "
                       "belongs to an animation set shared by weapons; a replaced clip plays for every weapon that uses it.");
    if (!ClipMonitorAvailable()) { ImGui::TextDisabled("Unavailable: the game's clip-start function wasn't found."); return; }
    bool cm = ClipMonitorEnabled();
    if (ImGui::Checkbox("Watch##wsa", &cm)) ClipMonitorSetEnabled(cm);
    ImGui::SameLine(); if (ImGui::Button("Clear##wsa")) ClipMonitorClear();
    ImGui::SameLine(); ImGui::Checkbox("Sort by name", &byNameUi);
    ImGui::SameLine(); ImGui::TextDisabled("(%zu clip names)", ClipNamesLoaded());
    ImGui::SameLine(); if (ImGui::Button("Open the exported folder")) {
        std::wstring dir = g_rootDir + L"dumps\\animations\\"; CreateDirectoryW((g_rootDir + L"dumps").c_str(), nullptr); CreateDirectoryW(dir.c_str(), nullptr);
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    static std::vector<ClipEvent> ev; static uint64_t ser = ~0ull;
    if (ser != ClipMonitorSerial()) { ev = ClipMonitorEvents(); ser = ClipMonitorSerial(); }
    // one row per clip (first time seen), newest first
    std::vector<uint64_t> ids; std::set<uint64_t> seen;
    for (auto& e : ev) if (seen.insert(e.id).second) ids.push_back(e.id);
    bool byName = byNameUi;
    if (byName) std::stable_sort(ids.begin(), ids.end(), [](uint64_t a, uint64_t b) {
        std::string x = FriendlyClip(a), y = FriendlyClip(b);
        if (x.empty() != y.empty()) return y.empty();
        return x < y; });
    // Which weapon stat a clip probably belongs to: the stat says when the action is done in gameplay, the clip
    // also has the arms settling back, so a clip runs a little longer than its stat (MG42 reload: stat 3.9 s, clip 4.17 s).
    struct Stat { const char* field; const char* label; float v; };
    std::vector<Stat> stats;
    {
        WsPart& pp = WsGet(w, 1);
        static const struct { const char* f; const char* l; } kT[] = { { "fReloadTimeNormal", "reload" }, { "fReloadTimeForced", "reload, empty" },
            { "fDrawTime", "draw" }, { "fHolsterTime", "holster" }, { "fUnjamTime", "unjam" } };
        for (auto& t : kT) for (auto& v : pp.v) if (!v.shared && v.size == 4 && v.name == t.f) {
            float f; memcpy(&f, &pp.d[v.offset], 4);
            if (f > 0.05f && f < 30.f) stats.push_back({ t.f, t.l, f });
        }
    }
    static std::map<uint64_t, float> lenCache;
    auto clipLen = [&](uint64_t id) {
        auto it = lenCache.find(id); if (it != lenCache.end()) return it->second;
        std::vector<uint8_t> d; std::string e; float L = -1;
        if (GameFileRead(id, d, &e, nullptr) && d.size() > 0x24 && memcmp(&d[0x18], "aNi", 3) == 0) memcpy(&L, &d[0x20], 4);
        return lenCache[id] = L;
    };
    auto guess = [&](float L) {
        std::string best; float bestErr = 1e9f;
        for (auto& st : stats) {
            float r = L / st.v;                                 // clip from ~90% to ~140% of its stat
            if (r < 0.9f || r > 1.4f) continue;
            float e = fabsf(r - 1.07f);
            if (e < bestErr) { bestErr = e; char b[96]; snprintf(b, sizeof(b), "probably %s (stat %.2f s)", st.label, st.v); best = b; }
        }
        return best;
    };
    if (!stats.empty()) {
        std::string line = "This weapon's timing stats:";
        for (auto& st : stats) { char b[64]; snprintf(b, sizeof(b), "  %s %.2f s", st.label, st.v); line += b; }
        ImGui::TextDisabled("%s", line.c_str());
        ImGui::TextDisabled("A clip runs a little longer than its stat (the arms settle back at the end).");
    }
    static std::map<uint64_t, std::string> glbIn;
    // a replaced clip goes in the mod as files\__unknown\<clip id>.ani and plays for every weapon that uses it
    std::wstring modFiles = g_rootDir + L"mods\\" + Widen(w.mod) + L"\\files\\__unknown\\";
    for (size_t k = 0; k < modFiles.size(); ++k) if (modFiles[k] == L'\\' && k > 2) CreateDirectoryW(modFiles.substr(0, k).c_str(), nullptr);

    auto resolve = [&](std::string p) {
        while (!p.empty() && (p.back() == '"' || p.back() == ' ')) p.pop_back(); while (!p.empty() && (p[0] == '"' || p[0] == ' ')) p.erase(0, 1);
        if (p.find(':') == std::string::npos) return g_rootDir + L"dumps\\animations\\" + Widen(p);
        return Widen(p);
    };
    {   // a .glb exported by FC4Hook 1.8.2+ knows its clip: no need to find the row
        static char any[260] = "";
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22);
        ImGui::InputTextWithHint("##anyglb", "your .glb (full path, or a name in dumps\\animations)", any, sizeof(any));
        ImGui::SameLine();
        ImGui::BeginDisabled(!any[0]);
        if (ImGui::Button("Use my .glb (finds its clip)")) {
            std::wstring src = resolve(any); uint64_t cid = ClipIdOfGlb(src); std::string m;
            if (!cid) {
                std::vector<uint8_t> gd; bool anim = false;
                if (ReadFileW(src, gd)) { std::string head(gd.begin(), gd.begin() + std::min<size_t>(gd.size(), 1 << 20)); anim = head.find("\"animations\"") != std::string::npos; }
                m = anim ? "This .glb doesn't say which clip it came from (exported before FC4Hook 1.8.2?). Export the clip again from the list below, or use the box on its row."
                         : "This .glb has no animation in it: it looks like a model. Put models in the Model tab (Use my .glb).";
            }
            else { char ib[24]; snprintf(ib, sizeof(ib), "%016llx", (unsigned long long)cid); ClipFromGlb(cid, src, modFiles + Widen(ib) + L".ani", &m); m = std::string("Clip ") + ib + ": " + m; }
            std::lock_guard<std::mutex> lk(s_wsMx); s_wsMsg = m;
        }
        ImGui::EndDisabled();
    }
    ImGui::BeginChild("wsclips", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 14), true);
    if (ids.empty()) ImGui::TextDisabled("No clips yet: tick Watch, Clear, then use the weapon in game.");
    for (uint64_t id : ids) {
        char idb[24]; snprintf(idb, sizeof(idb), "%016llx", (unsigned long long)id);
        ImGui::PushID(idb);
        std::wstring repl = modFiles + Widen(idb) + L".ani";
        bool replaced = GetFileAttributesW(repl.c_str()) != INVALID_FILE_ATTRIBUTES;
        float L = clipLen(id);
        std::string fr = FriendlyClip(id);
        if (!fr.empty()) { if (L >= 0) ImGui::Text("%s (%.2f s)", fr.c_str(), L); else ImGui::Text("%s", fr.c_str()); ImGui::SameLine(); ImGui::TextDisabled("%s", idb); ImGui::SameLine(); }
        else { ImGui::Text("%s", idb); ImGui::SameLine(); if (L >= 0) { ImGui::Text("%.2f s", L); ImGui::SameLine(); } }
        std::string gs = L > 0 ? guess(L) : "";
        if (!gs.empty()) { ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1), "%s", gs.c_str()); ImGui::SameLine(); }
        std::string lab = ClipLabelFor(id);
        ImGui::TextDisabled("%s%s", lab.c_str(), replaced ? "  [replaced for every weapon]" : "");
        if (ImGui::SmallButton("Export .glb")) { std::string m; ClipExportGlb(id, nullptr, &m); std::lock_guard<std::mutex> lk(s_wsMx); s_wsMsg = m; }
        ImGui::SameLine();
        std::string& g = glbIn[id];
        char buf[260]; snprintf(buf, sizeof(buf), "%s", g.c_str());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
        if (ImGui::InputTextWithHint("##g", "your .glb (full path, or a name in dumps\\animations)", buf, sizeof(buf))) g = buf;
        ImGui::SameLine();
        ImGui::BeginDisabled(g.empty());
        if (ImGui::SmallButton("Use my .glb")) {
            std::wstring src = resolve(g);
            std::string m; ClipFromGlb(id, src, repl, &m);
            std::lock_guard<std::mutex> lk(s_wsMx); s_wsMsg = m;
        }
        ImGui::EndDisabled();
        if (replaced) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Restore the game's")) { DeleteFileW(repl.c_str()); std::lock_guard<std::mutex> lk(s_wsMx); s_wsMsg = std::string("Restored ") + idb + ". Restart the game to see it."; }
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

struct WsFit { float scale = 1.f; float move[3] = { 0, 0, 0 }; };
static std::map<uint32_t, WsFit> s_wsFit;

// which .xbg in the mod's files\ folder has this game path hash ("" if none)
static std::string ModModelForHash(const std::string& mod, uint64_t h) {
    static std::map<std::string, std::pair<uint64_t, std::string>> cache;   // per mod: last hash looked up
    char kb[24]; snprintf(kb, sizeof(kb), "%016llx", (unsigned long long)h);
    auto it = cache.find(mod + kb);
    if (it != cache.end() && GetTickCount64() - it->second.first < 5000) return it->second.second;
    std::wstring root = g_rootDir + L"mods\\" + Widen(mod) + L"\\files\\";
    std::string found;
    std::function<void(const std::wstring&, const std::string&)> walk = [&](const std::wstring& dir, const std::string& rel) {
        WIN32_FIND_DATAW fd; HANDLE hf = FindFirstFileW((dir + L"*").c_str(), &fd);
        if (hf == INVALID_HANDLE_VALUE) return;
        do {
            std::wstring n = fd.cFileName; if (n == L"." || n == L"..") continue;
            std::string r = rel + Narrow(n);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) walk(dir + n + L"\\", r + "\\");
            else if (r.size() > 4 && _stricmp(r.c_str() + r.size() - 4, ".xbg") == 0 && FileId(r.c_str()) == h) found = r;
        } while (found.empty() && FindNextFileW(hf, &fd));
        FindClose(hf);
    };
    walk(root, "");
    cache[mod + kb] = { GetTickCount64(), found };
    return found;
}
static void WsModel(const WorkshopWeapon& w) {
    {
    WsPart& p0 = WsGet(w, 0);
    if (!p0.err.empty()) { ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", p0.err.c_str()); return; }
    }
    ImGui::TextWrapped("The models the weapon uses. Type the game path of your own model (an .xbg you ship in this mod's "
                       "files\\ folder, or any model already in the game) and press Set. Use F8 > Developer > File log to "
                       "find paths. First-person and third-person (_tp) models are separate.");
    int n = 0;
    // the item archetype holds the models in your hands (first-person high-res + third-person), the
    // pickup archetype the one lying on the ground
    for (int part : { 0 }) {
    WsPart& p = WsGet(w, part);
    if (!p.err.empty()) continue;
    for (size_t i = 0; i < p.v.size(); ++i) {
        const FcbValue& v = p.v[i];
        if (v.shared || v.size != 8 || v.name != "ModelPath") continue;
        ++n;
        // the readable path is stored just before the hash
        std::string text = i > 0 ? FcbFormat(p.d, p.v[i - 1]) : "";
        {   // the readable path is only rewritten when the new one fits: trust the hash the game uses
            uint64_t h = 0; if (v.offset + 8 <= p.d.size()) memcpy(&h, &p.d[v.offset], 8);
            std::string t = text; if (t.size() > 2 && t[0] == '"') t = t.substr(1, t.size() - 2);
            if (h && FileId(t.c_str()) != h) {
                std::string mine = ModModelForHash(w.mod, h);
                text = "\"" + (mine.empty() ? std::string("a model FC4Hook made (path not stored)") : mine) + "\"";
            }
        }
        ImGui::PushID((int)v.offset + part * 1000000);
        uint32_t key = v.offset + (uint32_t)part * 1000000u;
        ImGui::SeparatorText(v.object.substr(v.object.rfind('/') + 1).c_str());
        ImGui::TextDisabled("now: %s", text.c_str());
        if (text.size() > 2 && text[0] == '"') {
            ImGui::SameLine();
            if (ImGui::SmallButton("Extract it")) ModelExtract({ text.substr(1, text.size() - 2) });
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy this model + its materials and textures to fc4hook\\dumps\\extracted\\ to edit");
            // a .glb -> new .xbg in the mod, and point the weapon at it
            static std::map<uint32_t, std::string> objs;
            std::string& ob = objs[key];
            char obuf[260]; snprintf(obuf, sizeof(obuf), "%s", ob.c_str());
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20);
            if (ImGui::InputTextWithHint("##glb", "your .glb: full path, or its name if it's in this mod's folder", obuf, sizeof(obuf))) ob = obuf;
            ImGui::SameLine();
            ImGui::BeginDisabled(ob.empty());
            if (ImGui::Button("Use my .glb")) {
                std::string orig = text.substr(1, text.size() - 2);
                std::wstring objPath = Widen(ob);
                if (ob.find(':') == std::string::npos) {                 // a name: look for it anywhere in the mod folder
                    std::wstring base = g_rootDir + L"mods\\" + Widen(w.mod) + L"\\";
                    objPath = base + Widen(ob);
                    if (GetFileAttributesW(objPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                        WIN32_FIND_DATAW fd; std::wstring want = Widen(ob);
                        size_t sl = want.find_last_of(L"\\/"); if (sl != std::wstring::npos) want = want.substr(sl + 1);
                        std::vector<std::wstring> dirs = { base };
                        for (size_t k = 0; k < dirs.size() && k < 200; ++k) {
                            HANDLE h = FindFirstFileW((dirs[k] + L"*").c_str(), &fd);
                            if (h == INVALID_HANDLE_VALUE) continue;
                            do {
                                std::wstring n = fd.cFileName;
                                if (n == L"." || n == L"..") continue;
                                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) dirs.push_back(dirs[k] + n + L"\\");
                                else if (_wcsicmp(n.c_str(), want.c_str()) == 0) objPath = dirs[k] + n;
                            } while (FindNextFileW(h, &fd));
                            FindClose(h);
                        }
                    }
                }
                std::string safe = Lower(w.mod);
                for (auto& c : safe) if (!isalnum((unsigned char)c)) c = '_';
                std::string lo = Lower(ob);
                if (lo.size() > 4 && lo.compare(lo.size() - 4, 4, ".glb") == 0) {       // a brand-new model
                    std::string stem = Narrow(objPath.substr(objPath.find_last_of(L"\\/") + 1));
                    stem = Lower(stem.substr(0, stem.size() - 4));
                    for (auto& c : stem) if (!isalnum((unsigned char)c)) c = '_';
                    // one model per slot: first-person, third-person (_tp)... must not overwrite each other
                    std::string on = Lower(orig.substr(orig.find_last_of("\\/") + 1));
                    if (on.find("_tp") != std::string::npos) stem += "_tp";
                    else if (v.name != "ModelPath" || p.v[i - 1].object.find("HighRes") == std::string::npos) stem += "_" + std::to_string(v.offset);
                    std::string gp, m, err;
                    std::wstring filesDir = g_rootDir + L"mods\\" + Widen(w.mod) + L"\\files\\";
                    WsFit& ft = s_wsFit[key];
                    // built on the bones of the weapon whose arm animations it uses (Arms tab), if set
                    std::string bonesOf = on.find("_tp") != std::string::npos ? w.bonesTp : w.bones;
                    std::string baseModel = !bonesOf.empty() && part == 0 ? bonesOf : orig;
                    if (ModelFromGlb(baseModel, objPath, filesDir, "graphics\\fc4hook\\" + safe + "\\", stem, ft.scale, ft.move, &gp, &m)) {
                        if (WorkshopSet(w, part, v.offset, 'p', gp, &err)) m += "\nThe weapon now uses it. Restart the game to see it.";
                        else m += "\n(but couldn't point the weapon at it: " + err + ")";
                        s_wsParts[part].loaded = false;
                    }
                    std::lock_guard<std::mutex> lk(s_wsMx); s_wsMsg = m;
                } else {
                    std::lock_guard<std::mutex> lk(s_wsMx);
                    s_wsMsg = "Only .glb models can be used here (export from Blender as glTF Binary, textures packed in).";
                }
            }
            ImGui::EndDisabled();
            bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
            {   // fit a .glb: scale + move (applied when the button is pressed)
                WsFit& ft = s_wsFit[key];
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
                ImGui::DragFloat("scale", &ft.scale, 0.005f, 0.05f, 10.f, "%.2fx");
                ImGui::SameLine();
                float cm[3] = { ft.move[0] * 100, ft.move[1] * 100, ft.move[2] * 100 };
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
                if (ImGui::DragFloat3("move cm (right, forward, up)", cm, 0.1f, -50.f, 50.f, "%.1f"))
                    for (int k = 0; k < 3; ++k) ft.move[k] = cm[k] / 100;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("For .glb models: resize and shift your model to fit the weapon (applied when you press\n\"Use my .glb\"; press it again after changing these). Ctrl+click to type a value.");
            }
            if (hov)
                ImGui::SetTooltip("A whole new model: export it from Blender as glTF Binary (.glb) with its textures packed in,\n"
                                  "lined up where this weapon's model sits (load the extracted model to compare), 1 unit = 1 m.\n"
                                  "It replaces this weapon's model and moves with the gun; it keeps this weapon's animations.");
        }
        static std::map<uint32_t, std::string> edits;
        std::string& e = edits[key];
        char buf[260]; snprintf(buf, sizeof(buf), "%s", e.c_str());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 26);
        if (ImGui::InputTextWithHint("##mp", "graphics\\mymod\\mygun.xbg", buf, sizeof(buf))) e = buf;
        ImGui::SameLine();
        ImGui::BeginDisabled(e.empty());
        if (ImGui::Button("Set")) {
            std::string gamePath, m;
            if (WsModelPathFromInput(w, e, &gamePath, &m)) {
                std::string err;
                if (!WorkshopSet(w, part, v.offset, 'p', gamePath, &err)) m = "Couldn't set the model: " + err;
                else m = "Model set to " + gamePath + ". " + m + "Restart the game to see it.";
                s_wsParts[part].loaded = false;
            }
            std::lock_guard<std::mutex> lk(s_wsMx); s_wsMsg = m;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Reset to original")) {
            std::string err;
            if (!WorkshopSet(w, part, v.offset, 'R', "", &err)) { std::lock_guard<std::mutex> lk(s_wsMx); s_wsMsg = "Couldn't reset: " + err; }
            else { std::lock_guard<std::mutex> lk(s_wsMx); s_wsMsg = "Back to the original model. Restart the game to see it."; }
            s_wsParts[part].loaded = false;
        }
        ImGui::PopID();
    }
    }
    if (!n) ImGui::TextDisabled("No model paths found in this weapon's archetypes.");

    // the pickup (lying on the ground): its model path is text inside the pickup archetype
    static std::vector<std::string> pkModels; static std::string pkFor;
    if (pkFor != w.mod + "|" + std::to_string(s_wsParts[0].loaded)) { pkModels = WorkshopPickupModels(w); pkFor = w.mod + "|" + std::to_string(s_wsParts[0].loaded); }
    if (!pkModels.empty()) {
        ImGui::SeparatorText("On the ground (pickup)");
        const std::string& cur = pkModels[0];
        ImGui::TextDisabled("now: \"%s\"", cur.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Extract it##pk")) ModelExtract({ cur });
        static char pkb[260] = "";
        static WsFit pkFit;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20);
        ImGui::InputTextWithHint("##pkglb", "your .glb: full path, or its name if it's in this mod's folder", pkb, sizeof(pkb));
        ImGui::SameLine();
        ImGui::BeginDisabled(!pkb[0]);
        if (ImGui::Button("Use my .glb##pk")) {
            std::string ob = pkb; while (!ob.empty() && (ob.back() == '"' || ob.back() == ' ')) ob.pop_back(); while (!ob.empty() && (ob[0] == '"' || ob[0] == ' ')) ob.erase(0, 1);
            std::wstring objPath = Widen(ob);
            if (ob.find(':') == std::string::npos) {               // a name: look for it in the mod folder
                std::wstring base = g_rootDir + L"mods\\" + Widen(w.mod) + L"\\", want = Widen(ob);
                size_t sl = want.find_last_of(L"\\/"); if (sl != std::wstring::npos) want = want.substr(sl + 1);
                objPath = base + Widen(ob);
                std::vector<std::wstring> dirs = { base };
                for (size_t k = 0; k < dirs.size() && k < 200; ++k) {
                    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dirs[k] + L"*").c_str(), &fd);
                    if (h == INVALID_HANDLE_VALUE) continue;
                    do {
                        std::wstring nn = fd.cFileName;
                        if (nn == L"." || nn == L"..") continue;
                        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) dirs.push_back(dirs[k] + nn + L"\\");
                        else if (_wcsicmp(nn.c_str(), want.c_str()) == 0) objPath = dirs[k] + nn;
                    } while (FindNextFileW(h, &fd));
                    FindClose(h);
                }
            }
            // the new path must be exactly as long as the old one (it's patched in place)
            std::string safe = Lower(w.mod); for (auto& c : safe) if (!isalnum((unsigned char)c)) c = '_';
            std::string dir = "graphics\\fc4hook\\";
            size_t L = cur.size();
            std::string m, err, gp;
            if (L < dir.size() + 6) m = "The pickup's model path is too short to replace (" + cur + ").";
            else {
                uint32_t h = 2166136261u; for (char c : safe) h = (h ^ (uint8_t)c) * 16777619u;
                char hx[16]; snprintf(hx, sizeof(hx), "%08x", h);
                std::string stem = std::string("pk") + hx + "_" + safe;          // unique per weapon
                size_t want = L - dir.size() - 4;
                if (stem.size() > want) stem = stem.substr(0, want);
                while (stem.size() < want) stem += '_';
                std::wstring filesDir = g_rootDir + L"mods\\" + Widen(w.mod) + L"\\files\\";
                if (ModelFromGlb(cur, objPath, filesDir, dir, stem, pkFit.scale, pkFit.move, &gp, &m)) {
                    if (WorkshopRepointPickup(w, cur, gp, &err)) { m += "\nThe pickup now uses it. Restart the game to see it."; pkFor.clear(); }
                    else m += "\n(but couldn't point the pickup at it: " + err + ")";
                }
            }
            std::lock_guard<std::mutex> lk(s_wsMx); s_wsMsg = m;
        }
        ImGui::EndDisabled();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        ImGui::DragFloat("scale##pk", &pkFit.scale, 0.005f, 0.05f, 10.f, "%.2fx");
        ImGui::SameLine();
        float cm[3] = { pkFit.move[0] * 100, pkFit.move[1] * 100, pkFit.move[2] * 100 };
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        if (ImGui::DragFloat3("move cm (right, forward, up)##pk", cm, 0.1f, -50.f, 50.f, "%.1f")) for (int k = 0; k < 3; ++k) pkFit.move[k] = cm[k] / 100;
        ++n;
    }
}

static void WsAdvanced(const WorkshopWeapon& w) {
    static int part = 1;
    ImGui::RadioButton("item", &part, 0); ImGui::SameLine(); ImGui::RadioButton("properties", &part, 1); ImGui::SameLine(); ImGui::RadioButton("pickup", &part, 2);
    WsPart& p = WsGet(w, part);
    if (!p.err.empty()) { ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", p.err.c_str()); return; }
    static char flt[64] = "";
    ImGui::SameLine(); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14); ImGui::InputTextWithHint("##waf", "filter", flt, sizeof(flt));
    std::string f = Lower(flt);
    if (!ImGui::BeginTable("wsadv", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, 0))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Field"); ImGui::TableSetupColumn("Value"); ImGui::TableSetupColumn("Original");
    ImGui::TableHeadersRow();
    int shown = 0;
    for (auto& v : p.v) {
        if (!f.empty() && Lower(v.name).find(f) == std::string::npos && Lower(v.object).find(f) == std::string::npos) continue;
        if (++shown > 2000) break;
        WsEditRow(w, part, p, v, nullptr);
    }
    ImGui::EndTable();
}

// Workshop > Arms: which weapon's first-person animation set (hold, reload, sprint...) this weapon uses.
static void WsArms(const WorkshopWeapon& w) {
    static std::string s_cur, s_msg; static uint64_t s_curFor = 0; static int pick = -1; static char f[64] = "";
    if (s_curFor != w.ids[0]) { s_cur = WorkshopAnimSet(w); s_curFor = w.ids[0]; s_msg.clear(); }
    ImGui::TextWrapped("The arms' animations (how it's held, drawn, reloaded, sprinted with) come from an animation set. "
                       "Give this weapon another weapon's set. The gun's model should be built on a weapon with the same "
                       "set, or it sits off-centre in the hands (Model tab: pick that weapon as the base).");
    ImGui::Text("Now: %s", s_cur.c_str());
    if (!w.bones.empty()) ImGui::TextDisabled("New models for it are built on %s (Model tab > Use my .glb)", w.bones.c_str());
    const char* prev = pick >= 0 && pick < (int)ArmoryWeapons().size() ? ArmoryWeapons()[pick].name.c_str() : "use the animations of...";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
    if (ImGui::BeginCombo("##wsarms", prev, ImGuiComboFlags_HeightLarge)) {
        ImGui::SetNextItemWidth(-1); ImGui::InputTextWithHint("##wsarmsf", "filter...", f, sizeof(f));
        std::string fl = Lower(f);
        for (int i = 0; i < (int)ArmoryWeapons().size(); ++i) {
            if (!fl.empty() && Lower(ArmoryWeapons()[i].name).find(fl) == std::string::npos) continue;
            if (ImGui::Selectable((ArmoryWeapons()[i].name + "##a" + ArmoryWeapons()[i].id).c_str(), pick == i)) pick = i;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(pick < 0);
    if (ImGui::Button("Use these animations")) {
        uint64_t donor = strtoull(ArmoryWeapons()[pick].id.c_str(), nullptr, 10);
        WorkshopSetAnimSet(w, donor, &s_msg);
        s_cur = WorkshopAnimSet(w);
        s_wsListed = false;                              // re-read workshop.txt (bones=)
    }
    ImGui::EndDisabled();
    if (!s_msg.empty()) ImGui::TextWrapped("%s", s_msg.c_str());
}

void DrawWorkshopPage() {
    ImGui::TextWrapped("Make a new weapon from one that's already in the game, then change its stats and model. "
                       "FC4Hook copies the weapon's three archetypes (item, properties, pickup) into a new mod under new ids "
                       "and adds it to the Armory. Changes take effect after you restart the game.");
    if (!s_wsListed) { s_wsList = WorkshopList(); s_wsListed = true; }

    ImGui::SeparatorText("1. New weapon");
    static int base = -1; static char bf[64] = "", name[64] = "";
    const char* prev = base >= 0 && base < (int)ArmoryWeapons().size() ? ArmoryWeapons()[base].name.c_str() : "choose the weapon to start from";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
    if (ImGui::BeginCombo("##wsbase", prev, ImGuiComboFlags_HeightLarge)) {
        ImGui::SetNextItemWidth(-1); ImGui::InputTextWithHint("##wsbf", "filter...", bf, sizeof(bf));
        std::string f = Lower(bf);
        for (int i = 0; i < (int)ArmoryWeapons().size(); ++i) {
            if (!f.empty() && Lower(ArmoryWeapons()[i].name).find(f) == std::string::npos) continue;
            if (ImGui::Selectable((ArmoryWeapons()[i].name + "##" + ArmoryWeapons()[i].id).c_str(), base == i)) base = i;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::InputTextWithHint("##wsname", "new weapon's name", name, sizeof(name));
    ImGui::SameLine();
    ImGui::BeginDisabled(s_wsBusy || base < 0 || !name[0]);
    if (ImGui::Button("Create")) {
        s_wsBusy = true;
        uint64_t id = strtoull(ArmoryWeapons()[base].id.c_str(), nullptr, 10);
        std::string nm = name;
        { std::lock_guard<std::mutex> lk(s_wsMx); s_wsMsg = "creating " + nm + "..."; }
        RunExtractJob([id, nm] {
            std::string mod, msg;
            WorkshopCreate(id, nm, &mod, &msg);
            { std::lock_guard<std::mutex> lk(s_wsMx); s_wsMsg = msg; }
            s_wsListed = false; s_wsBusy = false;
        });
    }
    ImGui::EndDisabled();
    { std::lock_guard<std::mutex> lk(s_wsMx); if (!s_wsMsg.empty()) ImGui::TextWrapped("%s", s_wsMsg.c_str()); }

    ImGui::SeparatorText("2. Your weapons");
    if (s_wsList.empty()) { ImGui::TextDisabled("None yet. Create one above."); return; }
    ImGui::TextDisabled("Game registration: %s", WorkshopRegisterStatus().c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Workshop weapons are added to the game's weapon -> properties table when you enter the world\n(and before every give). Without that the game can't use them.");
    std::vector<VfsModEntry> mods = VfsMods();
    for (int i = 0; i < (int)s_wsList.size(); ++i) {
        auto& w = s_wsList[i];
        bool loaded = false; for (auto& m : mods) if (m.name == w.mod && m.enabled) loaded = true;
        char lbl[200]; snprintf(lbl, sizeof(lbl), "%s%s##ws%d", w.name.c_str(), loaded ? "" : "   (restart the game to load it)", i);
        if (ImGui::Selectable(lbl, s_wsSel == i)) { s_wsSel = i; WsReload(); }
    }
    if (s_wsSel < 0 || s_wsSel >= (int)s_wsList.size()) return;
    WorkshopWeapon& w = s_wsList[s_wsSel];
    ImGui::Separator();
    ImGui::Text("%s", w.name.c_str()); ImGui::SameLine();
    ImGui::TextDisabled("mods\\%s   item %llu", w.mod.c_str(), (unsigned long long)w.ids[0]);
    ImGui::BeginDisabled(!LuaReady());
    if (ImGui::Button("Give it to me")) Lua(("FC4Armory.give('" + std::to_string(w.ids[0]) + "', [[" + LuaSafe(w.name) + "]])").c_str(), "=workshop");
    ImGui::SameLine(); if (ImGui::Button("Spawn pickup")) Lua(("FC4Spawn.spawnPickup('" + std::to_string(w.ids[0]) + "', [[" + LuaSafe(w.name) + "]])").c_str(), "=workshop");
    ImGui::EndDisabled();
    ImGui::SameLine(); if (ImGui::Button("Check files")) { uint64_t id = w.ids[0]; StartArchetypeCheck([id] { return ArchetypeCheck(id); }); ShowMenuPage("Spawn"); }
    ImGui::SameLine(); if (ImGui::Button("Open folder")) ShellExecuteW(nullptr, L"open", (g_rootDir + L"mods\\" + Widen(w.mod)).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (ImGui::BeginTabBar("wstabs")) {
        if (ImGui::BeginTabItem("Stats")) { WsStats(w); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Model")) { WsModel(w); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Arms")) { WsArms(w); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Animations")) { WsAnimations(w); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Advanced")) { WsAdvanced(w); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
}


} // namespace fc4
