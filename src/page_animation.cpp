// Developer > Animation page: watches which animation clips start, and exports, imports and swaps clips (aniclip.cpp does the format work).
// Shared widgets and cross-page helpers are declared in menu_common.h.
#include "menu_common.h"

namespace fc4 {

// ------------------------------------------------------- dev: animation --
// State browser: names from animation\states.txt plus every bare name (no path) in your own
// dumps\file_log.txt. The game hashes Move-graph state names with the same CRC-64 as file
// paths, which is why they show up in the file log.
struct AnimName { std::string name, group; };
static std::vector<AnimName> s_animNames;
static bool s_animLoaded = false;

static bool IsBareName(const std::string& n) {
    if (n.size() < 3 || !isalpha((unsigned char)n[0])) return false;
    for (char c : n) if (!isalnum((unsigned char)c) && c != '_') return false;
    return true;
}

static void LoadAnimNames() {
    s_animNames.clear();
    std::set<std::string> seen;
    auto add = [&](const std::string& n, const std::string& g) { if (IsBareName(n) && seen.insert(Lower(n)).second) s_animNames.push_back({ n, g }); };
    FILE* f = _wfopen((g_rootDir + L"animation\\states.txt").c_str(), L"rb");
    char line[512];
    std::string group = "Other";
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            std::string l = line;
            while (!l.empty() && (l.back() == '\r' || l.back() == '\n' || l.back() == ' ')) l.pop_back();
            if (l.empty() || l[0] == '#') continue;
            if (l[0] == '[' && l.back() == ']') { group = l.substr(1, l.size() - 2); continue; }
            add(l, group);
        }
        fclose(f);
    }
    f = _wfopen((g_rootDir + L"dumps\\file_log.txt").c_str(), L"rb");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            std::string l = line;
            while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.pop_back();
            if (l.size() > 18 && l[16] == ' ' && l[17] == ' ') add(l.substr(18), "From your file log");
        }
        fclose(f);
    }
    s_animLoaded = true;
}

void DrawAnimationPage() {
    ImGui::TextWrapped("Drives the game's animation system: attach animation packages (yours too, shipped as loose files) and "
                       "play animations by path. The engine handles blending and the skeleton, so custom animations transition "
                       "like the game's own.");
    static int target = 0;
    static char custom[32] = "";
    ImGui::RadioButton("me", &target, 0); ImGui::SameLine();
    ImGui::RadioButton("last spawned", &target, 1); ImGui::SameLine();
    ImGui::RadioButton("entity id", &target, 2);
    if (target == 2) { ImGui::SameLine(); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12); ImGui::InputText("##eid", custom, sizeof(custom), ImGuiInputTextFlags_CharsDecimal); }
    std::string t = target == 0 ? "me" : target == 1 ? "last" : std::string(custom);
    std::string tq = "[[" + LuaSafe(t) + "]]";
    ImGui::BeginDisabled(!LuaReady());
    ImGui::SeparatorText("Animation package");
    static char pkg[260] = "";
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##pkg", "animation package path", pkg, sizeof(pkg));
    std::string pq = "[[" + LuaSafe(pkg) + "]]";
    if (ImGui::Button("Attach") && pkg[0] && Debounced("aatt")) Lua(("FC4Anim.attach(" + tq + ", " + pq + ")").c_str(), "=anim");
    ImGui::SameLine(); if (ImGui::Button("Detach") && pkg[0] && Debounced("adet")) Lua(("FC4Anim.detach(" + tq + ", " + pq + ")").c_str(), "=anim");
    ImGui::SameLine(); Help("Use Developer > File log while the game plays an animation to find package paths.");
    ImGui::SeparatorText("Play");
    static char anim[260] = "";
    static int layer = 0;
    static bool loop = false;
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##anim", "animation path or id", anim, sizeof(anim));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6); ImGui::InputInt("layer", &layer); ImGui::SameLine(); ImGui::Checkbox("loop", &loop);
    std::string aq = "[[" + LuaSafe(anim) + "]]";
    std::string extra = ", " + std::to_string(layer) + ", " + (loop ? "true" : "false");
    if (ImGui::Button("Play") && anim[0] && Debounced("aply")) Lua(("FC4Anim.play(" + tq + ", " + aq + extra + ", false)").c_str(), "=anim");
    ImGui::SameLine(); if (ImGui::Button("Force play") && anim[0] && Debounced("afpl")) Lua(("FC4Anim.play(" + tq + ", " + aq + extra + ", true)").c_str(), "=anim");
    ImGui::SameLine(); if (ImGui::Button("Current animation") && Debounced("acur")) Lua(("FC4Anim.current(" + tq + ", " + std::to_string(layer) + ")").c_str(), "=anim");
    ImGui::Text("Current: %s", GetState("anim.current", "-").c_str());
    ImGui::SeparatorText("Player animations by name (StartPlayAnim)");
    static char pname[128] = "";
    static bool b1 = false, b2 = false;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16); ImGui::InputTextWithHint("##pn", "name", pname, sizeof(pname));
    ImGui::SameLine(); ImGui::Checkbox("arg 3", &b1); ImGui::SameLine(); ImGui::Checkbox("arg 4", &b2);
    if (ImGui::Button("Start") && pname[0] && Debounced("ast")) Lua(("FC4Anim.start([[" + LuaSafe(pname) + "]], " + (b1 ? "true" : "false") + ", " + (b2 ? "true" : "false") + ")").c_str(), "=anim");
    ImGui::SameLine(); if (ImGui::Button("Stop") && Debounced("asp")) Lua("FC4Anim.stop()", "=anim");
    ImGui::EndDisabled();
    std::string st = GetState("anim.status");
    if (!st.empty()) ImGui::TextWrapped("Last: %s", st.c_str());

    ImGui::SeparatorText("Clip monitor");
    if (!ClipMonitorAvailable()) ImGui::TextDisabled("Unavailable: the game's clip-start function wasn't found.");
    else {
        bool cm = ClipMonitorEnabled();
        if (ImGui::Checkbox("Watch clips starting", &cm)) ClipMonitorSetEnabled(cm);
        ImGui::SameLine(); if (ImGui::Button("Clear##cm")) ClipMonitorClear();
        ImGui::SameLine(); Help("Lists every animation clip the moment the game starts playing it, newest first, with its labels "
                                "(press Label animation clips in the File log first for labels). Stand somewhere quiet, press Clear, "
                                "reload: the reload clips are at the top. Replace / With fill the swap pair on the File Activity page.");
        static std::vector<ClipEvent> ev;
        static uint64_t evSerial = ~0ull;
        if (evSerial != ClipMonitorSerial()) { ev = ClipMonitorEvents(); evSerial = ClipMonitorSerial(); }
        ImGui::TextDisabled("%d clips started%s", (int)ev.size(), g_swapReplace.empty() ? "" : "   (swap pair set: finish it on File Activity)");
        ImGui::SameLine();
        if (ImGui::SmallButton("Extract all listed")) {
            int ok = 0; std::string m;
            for (auto& e2 : ev) { char ib[24]; snprintf(ib, sizeof(ib), "%016llx", (unsigned long long)e2.id); if (ExtractGameFile(e2.id, "animations\\" + std::string(ib) + ".ani", &m)) ++ok; }
            SetState("anim.status", "extracted " + std::to_string(ok) + " clip(s) to fc4hook\\dumps\\extracted\\animations\\");
        }
        ImGui::BeginChild("clips", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 12), true);
        uint64_t now = GetTickCount64();
        for (size_t i = 0; i < ev.size(); ++i) {
            char idb[24]; snprintf(idb, sizeof(idb), "%016llx", (unsigned long long)ev[i].id);
            std::string lab = ClipLabelFor(ev[i].id);
            std::string line = std::string(idb) + "  [CAnimationResource] (no path)" + (lab.empty() ? "" : " {" + lab + "}");
            ImGui::PushID((int)i);
            if (ImGui::SmallButton("Replace")) g_swapReplace = line;
            ImGui::SameLine(); if (ImGui::SmallButton("With")) g_swapWith = line;
            ImGui::SameLine();
            if (ImGui::SmallButton("Extract")) { std::string m; ExtractGameFile(ev[i].id, "animations\\" + std::string(idb) + ".ani", &m); SetState("anim.status", m); }
            ImGui::SameLine();
            if (ImGui::SmallButton(".glb")) { std::string m; ClipExportGlb(ev[i].id, nullptr, &m); SetState("anim.status", m); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Export to fc4hook\\dumps\\animations\\<id>.glb: open in Blender (File > Import > glTF)");
            ImGui::SameLine(); ImGui::TextDisabled("%5.1fs ago", (now - ev[i].tick) / 1000.0);
            ImGui::SameLine(); ImGui::Text("%s  %s", idb, lab.c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    ImGui::SeparatorText("State browser");
    if (!s_animLoaded) LoadAnimNames();
    static char af[96] = "";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
    ImGui::InputTextWithHint("##af", "filter (e.g. holster, takedown, Com_)", af, sizeof(af));
    ImGui::SameLine(); if (ImGui::Button("Reload list")) { FileLogFlush(); LoadAnimNames(); }
    ImGui::SameLine(); Help("Play = StartPlayAnim on you with the arg 3 / arg 4 ticks above. PKG_ names are animation packages: "
                            "Attach adds them to the target. Names come from fc4hook\\animation\\states.txt and from your own "
                            "dumps\\file_log.txt (Reload list after recording more).");
    ImGui::TextDisabled("%d names", (int)s_animNames.size());
    ImGui::BeginChild("anames", ImVec2(0, 0), true);
    std::string fl = Lower(af);
    std::vector<int> vis;
    for (int i = 0; i < (int)s_animNames.size(); ++i)
        if (fl.empty() || Lower(s_animNames[i].name).find(fl) != std::string::npos || Lower(s_animNames[i].group).find(fl) != std::string::npos) vis.push_back(i);
    ImGuiListClipper clip;
    clip.Begin((int)vis.size());
    ImGui::BeginDisabled(!LuaReady());
    while (clip.Step())
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
            const AnimName& a = s_animNames[vis[r]];
            ImGui::PushID(vis[r]);
            bool pkg = a.name.rfind("PKG_", 0) == 0;
            if (ImGui::SmallButton(pkg ? "Attach" : "Play") && Debounced(("an" + a.name).c_str())) {
                std::string q = "[[" + LuaSafe(a.name) + "]]";
                if (pkg) Lua(("FC4Anim.attach(" + tq + ", " + q + ")").c_str(), "=anim");
                else Lua(("FC4Anim.start(" + q + ", " + (b1 ? "true" : "false") + ", " + (b2 ? "true" : "false") + ")").c_str(), "=anim");
            }
            ImGui::SameLine(); ImGui::TextUnformatted(a.name.c_str());
            ImGui::SameLine(); ImGui::TextDisabled("%s", a.group.c_str());
            ImGui::PopID();
        }
    ImGui::EndDisabled();
    ImGui::EndChild();
}


} // namespace fc4
