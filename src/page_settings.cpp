// Settings page (menu key, hotkeys, UI scale, developer mode) and the About page.
// Shared widgets and cross-page helpers are declared in menu_common.h.
#include "menu_common.h"

namespace fc4 {

static void KeyBind(const char* label, int* key, int slot) {
    ImGui::PushID(slot);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetFontSize() * 11);
    if (g_captureKey == slot) {
        ImGui::Button("press a key... (Esc = cancel, Backspace = none)", ImVec2(ImGui::GetFontSize() * 22, 0));
        for (int vk = 8; vk < 255; ++vk) {
            if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || (vk >= VK_SHIFT && vk <= VK_MENU) ||
                (vk >= VK_LSHIFT && vk <= VK_RMENU) || vk == VK_LWIN || vk == VK_RWIN) continue;
            if (!(GetAsyncKeyState(vk) & 0x8000)) continue;
            if (vk == VK_ESCAPE) { g_captureKey = 0; break; }
            if (vk == VK_BACK) { if (slot != 1) *key = 0; g_captureKey = 0; SaveConfig(); break; }
            if (vk == VK_RETURN || vk == VK_SPACE) continue;
            *key = vk; g_captureKey = 0; SaveConfig();
            KeyPressed(vk);                     // swallow this press so it doesn't fire the action
            Log("settings: %s = %s", label, KeyName(vk).c_str());
            break;
        }
    } else {
        std::string name = KeyName(*key);
        if (ImGui::Button(name.c_str(), ImVec2(ImGui::GetFontSize() * 8, 0))) g_captureKey = slot;
    }
    ImGui::PopID();
}

void DrawSettingsPage() {
    ImGui::SeparatorText("Keys");
    KeyBind("Open menu", &g_cfg.menuKey, 1);
    KeyBind("God mode", &g_cfg.hkGodMode, 2);
    KeyBind("Unlimited ammo", &g_cfg.hkAmmo, 3);
    KeyBind("Refill ammo", &g_cfg.hkRefill, 4);
    KeyBind("Slow motion", &g_cfg.hkSlowMo, 5);
    KeyBind("Open Armory", &g_cfg.hkMenuArmory, 6);
    KeyBind("Fly", &g_cfg.hkFly, 7);
    KeyBind("Hide HUD", &g_cfg.hkHud, 8);
    KeyBind("Instant brake", &g_cfg.hkBrake, 9);
    if (ImGui::Checkbox("Gamepad: hold L3 + R3 to open/close the menu", &g_cfg.padMenu)) SaveConfig();
    Help("Press both thumbsticks in for half a second. In the menu: D-pad / left stick to move between buttons and tabs, "
         "A to press, B to go back. While the menu is open the game doesn't get your pad.");
    ImGui::TextDisabled("Click a key to change it. Hotkeys work while you play (menu closed or open).");

    ImGui::SeparatorText("Menu");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::SliderFloat("Menu size", &g_cfg.uiScale, 0.75f, 2.0f, "%.2fx");
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveConfig();
    if (ImGui::Checkbox("Show the \"FC4Hook loaded\" banner at startup", &g_cfg.showBanner)) SaveConfig();
    if (ImGui::Checkbox("Developer mode (adds the Developer tab)", &g_cfg.developerMode)) SaveConfig();

    ImGui::SeparatorText("Log");
    const char* levels[] = { "Debug (everything)", "Info", "Warnings", "Errors only" };
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    if (ImGui::Combo("Log level", &g_cfg.logLevel, levels, 4)) { SaveConfig(); Log("log level set to %s", LevelName(g_cfg.logLevel)); }
    if (ImGui::Checkbox("Keep the previous session's log (fc4hook.prev.log)", &g_cfg.keepPrevLog)) SaveConfig();
    if (ImGui::Checkbox("Write crash reports (fc4hook\\crashes\\)", &g_cfg.crashReports)) SaveConfig();
    Help("If the game crashes, FC4Hook writes what it knows (where, which module, last log lines). "
         "Attach that file when you report a bug.");
    if (ImGui::Button("Open FC4Hook folder")) OpenFolder(g_rootDir);
    ImGui::SameLine();
    if (ImGui::Button("Open crash reports")) OpenFolder(g_rootDir + L"crashes");

    ImGui::SeparatorText("Scripts");
    if (ImGui::Button("Reload scripts")) { RequestAutorunReload(); Log("reloading scripts\\autorun"); }
    Help("Re-runs everything in fc4hook\\scripts\\autorun\\. Use after editing a script.");
    ImGui::TextDisabled("Settings are saved to fc4hook\\fc4hook.ini as you change them.");
}


void DrawAboutPage() {
    ImGui::Text("%s", FC4HOOK_TITLE);
    ImGui::TextWrapped("A script hook and mod menu for Far Cry 4 (PC). Runs Lua scripts and C++ plugins inside the "
                       "game, with cheats, an armory, bell-tower travel and a co-op tower reward fix.");
    ImGui::Spacing();
    ImGui::BulletText("Menu key: %s   (change it in Settings)", KeyName(g_cfg.menuKey).c_str());
    ImGui::BulletText("Your files: Far Cry 4\\bin\\fc4hook\\");
    ImGui::BulletText("Scripts: fc4hook\\scripts\\autorun\\*.lua   Plugins: fc4hook\\plugins\\*.dll");
    ImGui::BulletText("Bug reports: attach fc4hook.log (and any file in fc4hook\\crashes\\)");
    ImGui::Spacing();
    ImGui::TextDisabled("Uses Dear ImGui (MIT), MinHook (BSD 2-clause) and Lua 5.4 (MIT). See LICENSES.txt.");
    ImGui::TextDisabled("Not affiliated with Ubisoft. Use cheats offline or with friends who agree.");
}


} // namespace fc4
