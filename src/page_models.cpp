// Models & Textures page: extract game models, convert images into game textures, and list the models the game has loaded.
// Shared widgets and cross-page helpers are declared in menu_common.h.
#include "menu_common.h"

namespace fc4 {

// Extract .xbg models with their materials and textures (extract.cpp ExtractModel).
static std::mutex s_mdlMx;
static std::string s_mdlMsg;
static std::atomic<bool> s_mdlBusy{ false };

void ModelExtract(const std::vector<std::string>& paths) {
    if (paths.empty() || s_mdlBusy.exchange(true)) return;
    { std::lock_guard<std::mutex> lk(s_mdlMx); s_mdlMsg = "extracting..."; }
    RunExtractJob([paths] {
        std::string all;
        for (auto& p : paths) { std::string m; ExtractModel(p, &m); all += m + "\n"; }
        { std::lock_guard<std::mutex> lk(s_mdlMx); s_mdlMsg = all; }
        s_mdlBusy = false;
    });
}

void DrawModelsPage() {
    ImGui::TextWrapped("Extract a model (.xbg) from the game's archives together with its materials (.material.bin) and "
                       "textures (.xbt, plus a .dds copy of each to view or edit), into fc4hook\\dumps\\extracted\\<game path>. "
                       "Open the .xbg with the community Dunia model tools. To use your own model, give a .glb to Mods > Weapon Workshop > Model.");
    ImGui::BeginDisabled(s_mdlBusy);
    static char path[260] = "";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 26);
    ImGui::InputTextWithHint("##mdlp", "graphics\\_common\\weapons\\ak74m\\ak74m.xbg", path, sizeof(path));
    ImGui::SameLine(); if (ImGui::Button("Extract##p") && path[0]) ModelExtract({ path });

    static char aid[32] = "";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::InputTextWithHint("##mdla", "archetype id", aid, sizeof(aid), ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine();
    if (ImGui::Button("Extract every model it uses") && aid[0]) {
        uint64_t id = strtoull(aid, nullptr, 10);
        if (!s_mdlBusy.exchange(true)) {
            { std::lock_guard<std::mutex> lk(s_mdlMx); s_mdlMsg = "reading the archetype..."; }
            RunExtractJob([id] {
                std::vector<std::string> ms = ArchetypeModels(id);
                std::string all = ms.empty() ? "That archetype doesn't use any .xbg model (or isn't there).\n" : "";
                for (auto& p : ms) { std::string m; ExtractModel(p, &m); all += m + "\n"; }
                { std::lock_guard<std::mutex> lk(s_mdlMx); s_mdlMsg = all; }
                s_mdlBusy = false;
            });
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Any archetype: a weapon id from the Armory, a Workshop weapon, an id from the Spawner list...");
    ImGui::EndDisabled();
    {
        std::lock_guard<std::mutex> lk(s_mdlMx);
        if (!s_mdlMsg.empty()) {
            ImGui::BeginChild("mdlmsg", ImVec2(0, ImGui::GetFontSize() * 7), true, ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(s_mdlMsg.c_str());
            ImGui::EndChild();
        }
    }
    if (ImGui::Button("Open the extracted folder")) {
        std::wstring dir = g_rootDir + L"dumps\\extracted";
        CreateDirectoryW((g_rootDir + L"dumps").c_str(), nullptr); CreateDirectoryW(dir.c_str(), nullptr);
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    ImGui::SeparatorText("Image -> game texture (.xbt)");
    ImGui::TextWrapped("PNG, JPG or TGA -> a Far Cry 4 texture with all its mip levels. Sizes are rounded down to powers of two "
                       "(up to 2048). Written into a mod at the game path you choose: that path REPLACES the game's texture "
                       "there, or is a new texture your own materials can use.");
    static char txImg[260] = "", txPath[260] = "", txMod[64] = "MyTextures";
    static int txKind = 0;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 26); ImGui::InputTextWithHint("image##tx", "C:\\...\\ray_gun.png", txImg, sizeof(txImg));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 26); ImGui::InputTextWithHint("game path##tx", "graphics\\mymod\\ray_gun_d.xbt", txPath, sizeof(txPath));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12); ImGui::InputText("mod name##tx", txMod, sizeof(txMod));
    ImGui::RadioButton("colour (_d)", &txKind, 0); ImGui::SameLine(); ImGui::RadioButton("normal map (_n)", &txKind, 1); ImGui::SameLine(); ImGui::RadioButton("specular (_s)", &txKind, 2);
    ImGui::SameLine();
    ImGui::BeginDisabled(!txImg[0] || !txPath[0] || !txMod[0]);
    if (ImGui::Button("Convert##tx")) {
        std::string img = txImg; while (!img.empty() && (img.back() == '"' || img.back() == ' ')) img.pop_back(); while (!img.empty() && (img[0] == '"' || img[0] == ' ')) img.erase(0, 1);
        std::string gp = Lower(txPath); for (auto& c : gp) if (c == '/') c = '\\';
        if (gp.size() < 4 || gp.compare(gp.size() - 4, 4, ".xbt") != 0) gp += ".xbt";
        std::vector<uint8_t> in, out, mip0; std::string m, err;
        FILE* f = _wfopen(Widen(img).c_str(), L"rb");
        if (f) { uint8_t b[65536]; size_t n; while ((n = fread(b, 1, sizeof(b), f)) > 0) in.insert(in.end(), b, b + n); fclose(f); }
        if (in.empty()) m = "Couldn't read " + img;
        else if (!ImageToXbtSplit(in, txKind == 1 ? 'n' : txKind == 2 ? 's' : 'd', Mip0Name(gp), out, mip0, &err)) m = "Not converted: " + err;
        else {
            std::wstring dst = g_rootDir + L"mods\\" + Widen(txMod) + L"\\files\\" + Widen(gp);
            for (size_t k = 0; k < dst.size(); ++k) if (dst[k] == L'\\' && k > 2) CreateDirectoryW(dst.substr(0, k).c_str(), nullptr);
            FILE* o = _wfopen(dst.c_str(), L"wb");
            if (o && fwrite(out.data(), 1, out.size(), o) == out.size()) m = "Written to mods\\" + std::string(txMod) + "\\files\\" + gp + ". Restart the game.";
            else m = "Couldn't write " + Narrow(dst);
            if (o) fclose(o);
            if (!mip0.empty()) {                                   // big texture: top level in its own _mip0 file, like the game's
                std::wstring d0 = g_rootDir + L"mods\\" + Widen(txMod) + L"\\files\\" + Widen(Mip0Name(gp));
                FILE* o2 = _wfopen(d0.c_str(), L"wb");
                if (!o2 || fwrite(mip0.data(), 1, mip0.size(), o2) != mip0.size()) m = "Couldn't write " + Narrow(d0);
                else m += " (big texture: also wrote " + Mip0Name(gp) + ")";
                if (o2) fclose(o2);
            }
        }
        std::lock_guard<std::mutex> lk(s_mdlMx); s_mdlMsg = m;
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Models the game has loaded (from the File log)");
    if (!FileLogEnabled()) {
        ImGui::TextDisabled("Turn on the File log (File Activity page > Record file paths), then look at / equip / spawn things:");
        ImGui::TextDisabled("every model the game loads shows up here.");
        if (ImGui::Button("Turn the File log on")) FileLogSetEnabled(true);
    }
    static char flt[64] = "";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    ImGui::InputTextWithHint("##mdlf", "filter, e.g. weapons, ak74", flt, sizeof(flt));
    static std::vector<std::string> lines; static uint64_t serial = ~0ull;
    if (serial != FileLogSerial()) { lines = FileLogRecent(".xbg", 3000); serial = FileLogSerial(); }
    std::string f = Lower(flt);
    ImGui::BeginChild("mdllist", ImVec2(0, 0), true);
    ImGui::BeginDisabled(s_mdlBusy);
    int shown = 0;
    std::set<std::string> seen;
    for (auto& l : lines) {
        uint64_t id; std::string p, type;
        if (!ParseLogLine(l, &id, &p, &type) || p.empty()) continue;
        std::string lp = Lower(p);
        if (lp.size() < 4 || lp.compare(lp.size() - 4, 4, ".xbg") != 0 || !seen.insert(lp).second) continue;
        if (!f.empty() && lp.find(f) == std::string::npos) continue;
        if (++shown > 1000) break;
        ImGui::PushID(lp.c_str());
        if (ImGui::SmallButton("Extract")) ModelExtract({ p });
        ImGui::SameLine(); ImGui::TextUnformatted(p.c_str());
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    if (!shown) ImGui::TextDisabled("No models yet.");
    ImGui::EndChild();
}



} // namespace fc4
