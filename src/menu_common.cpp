// Small widgets and helpers shared by every menu page (declared in menu_common.h).
#include "menu_common.h"

namespace fc4 {

bool Debounced(const char* label) {
    static std::string lastLabel; static ULONGLONG lastClick = 0;
    ULONGLONG now = GetTickCount64();
    if (lastLabel == label && now - lastClick < 400) return false;
    lastLabel = label; lastClick = now;
    return true;
}

bool Avail(const char* native) { return GetState(std::string("avail.") + native) == "1"; }
bool ScriptsLoaded() { return !GetState("avail.EnableGodMode").empty(); }

void Help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30); ImGui::TextUnformatted(text); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
}

// A button that's greyed out when its native is missing (or Lua isn't live yet).
bool ActionButton(const char* label, const char* native, ImVec2 size) {
    bool ok = LuaReady() && (!native || Avail(native));
    ImGui::BeginDisabled(!ok);
    bool pressed = ImGui::Button(label, size) && Debounced(label);
    ImGui::EndDisabled();
    if (!ok && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", !LuaReady() ? "Load into the world first." : "Not available in this game build.");
    return pressed;
}

void OpenFolder(const std::wstring& path) { ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

std::string LuaSafe(const std::string& s) {     // for use inside [[ ]] long strings
    std::string r; for (char c : s) if (c != ']' && c != '[' && c != '\n' && c != '\r') r += c; return r;
}

std::string Lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; }

void NeedWorldNote() {
    if (!LuaReady()) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Game scripts aren't running yet: load a save and these unlock.");
    else if (!ScriptsLoaded()) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "FC4Hook scripts not loaded: check the Console for errors, then Settings > Reload scripts.");
}

} // namespace fc4
