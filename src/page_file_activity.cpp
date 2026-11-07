// Developer > File Activity page: records which game files are requested, and builds quick "replace file A with file B" swap mods.
// Shared widgets and cross-page helpers are declared in menu_common.h.
#include "menu_common.h"

namespace fc4 {

static std::string s_flSel;
std::string g_swapReplace, g_swapWith;     // "Replace A with B" pair; the Animation page fills it too
void DrawFileActivityPage() {
    if (!FileLogAvailable()) { ImGui::TextDisabled("Not available: the game's path-to-id function wasn't found (other game version?)."); return; }
    ImGui::TextWrapped("Records every file path the game asks for, and every resource the game creates (lines with [Type]: "
                       "animation packages, move data, meshes, textures... even ones it loads by id without a path), each once, with "
                       "its file id. Use it to find what to replace. Also written to fc4hook\\dumps\\file_log.txt. "
                       "Lines a loose mod supplies are marked with  <- ModName.");
    if (!ResourceLogAvailable()) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Resource log unavailable (function not found): paths only.");
    bool on = FileLogEnabled();
    if (ImGui::Checkbox("Record file paths", &on)) FileLogSetEnabled(on);
    ImGui::SameLine(); ImGui::Text("%llu entries", (unsigned long long)FileLogCount());
    ImGui::SameLine(); { bool st = FileLogAtStartup(); if (ImGui::Checkbox("Record from game start", &st)) FileLogSetAtStartup(st); }
    ImGui::SameLine(); Help("Starts recording as soon as the game starts, so you also catch what's loaded with the world "
                            "(animations, move data, characters). Restart the game after ticking it. Turn it off when you're done: "
                            "recording makes loading a little slower.");
    ImGui::SameLine(); if (ImGui::Button("Clear")) FileLogClear();
    ImGui::SameLine(); if (ImGui::Button("Open file_log.txt")) { FileLogFlush(); OpenFolder(g_rootDir + L"dumps\\file_log.txt"); }
    static char hpath[260] = "";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24);
    ImGui::InputTextWithHint("##hp", "path -> file id (e.g. graphics\\_textures\\foo.xbt)", hpath, sizeof(hpath));
    if (hpath[0]) {
        uint64_t id = FileId(hpath);
        ImGui::SameLine(); ImGui::Text("%016llx  (%llu)", (unsigned long long)id, (unsigned long long)id);
        ImGui::SameLine(); if (ImGui::SmallButton("copy")) { char b[32]; snprintf(b, sizeof(b), "%016llx", (unsigned long long)id); ImGui::SetClipboardText(b); }
    }
    // ---- extract / swap (click a line below to select it)
    std::string& sel = s_flSel; std::string& swapA = g_swapReplace; std::string& swapB = g_swapWith;
    ImGui::SeparatorText("Extract / swap test");
    ImGui::TextWrapped("Selected: %s", sel.empty() ? "(click a line below)" : sel.c_str());
    ImGui::BeginDisabled(sel.empty());
    if (ImGui::Button("Extract to dumps\\extracted") && Debounced("fx")) {
        std::string line = sel;
        RunExtractJob([line] {
            std::string m, path, type; uint64_t id = 0;
            ParseLogLine(line, &id, &path, &type);
            if (path.empty() && !type.empty()) path = "__unknown\\" + type + "\\" + line.substr(0, 16) + ".bin";
            if (!ExtractGameFile(id, path, &m) && type == "CAnimationPackageResource") DumpResourceMemory(id, &m);
            SetState("filelog.status", m); Log("%s", m.c_str());
        });
        SetState("filelog.status", "extracting...");
    }
    ImGui::SameLine(); if (ImGui::Button("Replace THIS...")) swapA = sel;
    ImGui::SameLine(); if (ImGui::Button("...WITH this")) swapB = sel;
    ImGui::EndDisabled();
    ImGui::SameLine(); Help("Swap test: select the file to replace and press \"Replace THIS...\", then select a file of the same "
                            "kind (texture for texture, animation for animation) and press \"...WITH this\", then Create swap mod. "
                            "It copies the second file out of the game's archives into mods\\FC4Hook_SwapTest. Restart the game to see it.");
    ImGui::Text("Replace: %s", swapA.empty() ? "-" : swapA.c_str());
    ImGui::Text("With:    %s", swapB.empty() ? "-" : swapB.c_str());
    ImGui::BeginDisabled(swapA.empty() || swapB.empty() || swapA == swapB);
    if (ImGui::Button("Create swap mod") && Debounced("fsw")) {
        std::string a = swapA, b = swapB;
        RunExtractJob([a, b] {
            uint64_t ia = 0, ib = 0; std::string pa, pb, ta, tb, m;
            ParseLogLine(a, &ia, &pa, &ta); ParseLogLine(b, &ib, &pb, &tb);
            if (!ta.empty() && !tb.empty() && ta != tb) LogL(LOG_WARN, "swap: %s replaced with a %s - different kinds of file, this may crash", ta.c_str(), tb.c_str());
            MakeSwap(ia, pa, ib, pb, &m); SetState("filelog.status", m);
        });
        SetState("filelog.status", "creating swap mod...");
    }
    ImGui::EndDisabled();
    ImGui::SameLine(); if (ImGui::Button("Open swap mod folder")) { CreateDirectoryW((g_rootDir + L"mods\\FC4Hook_SwapTest").c_str(), nullptr); OpenFolder(g_rootDir + L"mods\\FC4Hook_SwapTest"); }
    if (ImGui::Button("Install edited textures") && Debounced("fit")) {
        RunExtractJob([] { std::string m; InstallEditedTextures(&m); SetState("filelog.status", m); });
        SetState("filelog.status", "packing edited textures...");
    }
    ImGui::SameLine(); if (ImGui::Button("Label animation clips") && Debounced("flab")) {
        RunExtractJob([] { std::string m; LabelAnimationClips(&m); SetState("filelog.status", m); });
        SetState("filelog.status", "labelling clips...");
    }
    ImGui::SameLine(); Help("Reads every [CAnimationResource] clip in your file log out of the game's archives and tags it with its "
                            "length and the weapon / bone names inside, e.g. {2.30s ak74m RightHand}. Then filter by a weapon name "
                            "and use Replace THIS / WITH this to swap clips. Also written to dumps\\animation_clips.txt.");
    ImGui::SameLine(); if (ImGui::Button("Find clip names") && Debounced("fcn")) {
        RunExtractJob([] { std::string m; FindClipNames(&m); SetState("filelog.status", m); });
        SetState("filelog.status", "finding clip names...");
    }
    ImGui::SameLine(); Help("Searches every game file for animation file names (.mab), and tries likely names (each animation folder x "
                            "each name, with the weapon swapped for every other weapon), to give clips their real names. Takes a few "
                            "minutes. Then press Label animation clips. Results in dumps\\clip_names.txt.");
    {
        static char refFilter[64] = "sekimber";
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::InputText("##reff", refFilter, sizeof(refFilter));
        ImGui::SameLine(); if (ImGui::Button("Find files using these clips") && Debounced("fref")) {
            std::string f = refFilter;
            RunExtractJob([f] { std::string m; FindClipReferences(f, &m); SetState("filelog.status", m); });
            SetState("filelog.status", "searching...");
        }
        ImGui::SameLine(); Help("Searches every game file for the ids of the named clips containing this text (e.g. sekimber = "
                                "the 1911's clips) and lists the files that use them in dumps\\clip_refs.txt. Takes a few minutes.");
    }
    ImGui::SameLine(); if (ImGui::Button("Snapshot arms")) { std::string m; ArmsSnapshot(&m); SetState("filelog.status", m); }
    ImGui::SameLine(); Help("Saves the player's first-person animation state (from the newest first-person clip) to "
                            "dumps\\arms_snapshot_N.bin. Draw one gun, wait a second, press; draw another, press again.");
    ImGui::SameLine(); if (ImGui::Button("Open extracted folder")) { CreateDirectoryW((g_rootDir + L"dumps\\extracted").c_str(), nullptr); OpenFolder(g_rootDir + L"dumps\\extracted"); }
    ImGui::SameLine(); Help("Texture editing: Extract a texture (.xbt). FC4Hook also writes a .dds of the whole image next to it. "
                            "Open the .dds in paint.net / GIMP / Photoshop, paint, and save it as DDS in the same format (DXT1/BC1 for "
                            "most _d textures) WITH mipmaps, same name. Then press Install edited textures: every changed .dds is packed "
                            "back into the game's format in mods\\MyTextures. Restart the game.");
    std::string fst = GetState("filelog.status");
    if (!fst.empty()) ImGui::TextWrapped("%s", fst.c_str());
    ImGui::Separator();

    static char filter[128] = "";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
    ImGui::InputTextWithHint("##ff", "filter (e.g. .xbt, ak47, .ark.fcb)", filter, sizeof(filter));
    ImGui::SameLine(); ImGui::TextDisabled("newest first; click a line to select it (and copy it)");
    static std::vector<std::string> lines;
    static uint64_t serial = ~0ull;
    static std::string lastFilter;
    if (serial != FileLogSerial() || lastFilter != filter) { lines = FileLogRecent(filter, 3000); serial = FileLogSerial(); lastFilter = filter; }
    ImGui::BeginChild("flog", ImVec2(0, 0), true);
    ImGuiListClipper clip;
    clip.Begin((int)lines.size());
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(lines[i].c_str(), lines[i] == sel)) { sel = lines[i]; ImGui::SetClipboardText(lines[i].c_str()); }
            ImGui::PopID();
        }
    ImGui::EndChild();
}


} // namespace fc4
