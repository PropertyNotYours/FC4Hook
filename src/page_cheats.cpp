// Cheats page: god mode, infinite ammo, fly, slow motion, time of day and other toggles. Each switch sends a call to the FC4Cheats Lua script (dist/fc4hook/scripts/autorun/40_cheats.lua) and reads its state back with GetState.
// Shared widgets and cross-page helpers are declared in menu_common.h.
#include "menu_common.h"

namespace fc4 {

static void CheatSwitch(const char* label, const char* key, const char* native, const char* help) {
    bool on = GetState(std::string("cheat.") + key) == "1";
    bool ok = LuaReady() && Avail(native);
    ImGui::BeginDisabled(!ok);
    if (ImGui::Checkbox(label, &on) && Debounced(label)) {
        char lua[128]; snprintf(lua, sizeof(lua), "FC4Cheats.set('%s', %s)", key, on ? "true" : "false");
        Lua(lua, "=cheats");
        SetState(std::string("cheat.") + key, on ? "1" : "0");   // optimistic; script republishes
    }
    ImGui::EndDisabled();
    if (help) Help(help);
}

// Slider bound to a cheats.txt value: shows the live value, sends FC4Cheats.<fn>(<args>, v) on release.
static void CfgSlider(const char* id, const char* stateKey, const char* def, float lo, float hi, const char* fmt,
                      const char* luaFmt, float width = 14) {
    static std::map<std::string, float> vals;
    float& v = vals.emplace(id, -1e9f).first->second;
    if (v < -1e8f || !ImGui::IsAnyItemActive()) v = (float)atof(GetState(stateKey, def).c_str());
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * width);
    ImGui::BeginDisabled(!LuaReady());
    ImGui::SliderFloat(id, &v, lo, hi, fmt);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        char lua[128]; snprintf(lua, sizeof(lua), luaFmt, v); Lua(lua, "=cheats");
        snprintf(lua, sizeof(lua), "%.2f", v); SetState(stateKey, lua);
    }
    ImGui::EndDisabled();
}

void DrawCheatsPage() {
    NeedWorldNote();
    ImGui::BeginDisabled(!LuaReady());
    if (ImGui::Button("Save the game now") && Debounced("savegame"))
        Lua("local ok, e = pcall(SaveGame) ModSetState('cheat.saved', ok and os.date('%H:%M:%S') or ('failed: ' .. tostring(e)))", "=save");
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Asks the game to save right now (its own save, same as after a mission or an outpost).\n"
                          "The game may refuse while a save isn't allowed (in a mission, in combat, during a cutscene).");
    { std::string sv = GetState("cheat.saved"); if (!sv.empty()) { ImGui::SameLine(); ImGui::TextDisabled("last save request: %s", sv.c_str()); } }
    ImGui::SeparatorText("Player");
    CheatSwitch("God mode", "god", "EnableGodMode", "You can't die. Stays on through deaths, cutscenes and loads until you switch it off.");
    ImGui::SameLine(ImGui::GetFontSize() * 14);
    CheatSwitch("Unlimited ammo", "ammo", "EnableUnlimitedAmmo", "Never run out of ammo or throwables.");
    CheatSwitch("Infinite magazine", "mag", "CAPI_Pawn.SetWeaponAmmoClip", "Your clip never empties: shoot forever without reloading.");
    ImGui::SameLine(ImGui::GetFontSize() * 14);
    CheatSwitch("No fall damage", "nofall", "CAPI_Player.SetLandingDamageEnabled", "Land from any height.");
    if (ActionButton("Refill ammo", "RefillPlayerAmmo")) Lua("FC4Cheats.refill()", "=cheats");
    ImGui::SameLine();
    if (ActionButton("Unstick (up 30 m)", "teleport")) Lua("FC4Cheats.up(30)", "=cheats");
    Help("Teleports you 30 m straight up with god mode on for 20 seconds. For when you're stuck in a rock.");

    ImGui::SeparatorText("Movement");
    CheatSwitch("Speed boost", "speed", "CAPI_Entity.SetEntityPositionVec3", "Moves you further every step (on foot).");
    ImGui::SameLine(ImGui::GetFontSize() * 14);
    {
        static float sx = -1;
        if (sx < 0 || !ImGui::IsAnyItemActive()) sx = (float)atof(GetState("cheat.speedx", "2").c_str());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        ImGui::BeginDisabled(!LuaReady());
        ImGui::SliderFloat("##speedx", &sx, 1.25f, 6.0f, "%.2fx speed");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            char lua[64]; snprintf(lua, sizeof(lua), "FC4Cheats.setCfg('speedx', %.2f)", sx); Lua(lua, "=cheats");
            snprintf(lua, sizeof(lua), "%.2f", sx); SetState("cheat.speedx", lua);
        }
        ImGui::EndDisabled();
    }
    CheatSwitch("Fly", "fly", "CAPI_Entity.SetEntityPositionVec3", "W/A/S/D to move, Space up, Ctrl down, hold Shift to go fast.");
    ImGui::SameLine(ImGui::GetFontSize() * 14);
    CheatSwitch("Noclip", "noclip", "CAPI_Entity.SetImmuneToCollisions", "Fly through walls and terrain (turns fly on).");
    {
        static float fs = -1;
        if (fs < 0 || !ImGui::IsAnyItemActive()) fs = (float)atof(GetState("cheat.fly_speed", "12").c_str());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        ImGui::BeginDisabled(!LuaReady());
        ImGui::SliderFloat("fly speed", &fs, 2.0f, 60.0f, "%.0f m/s");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            char lua[64]; snprintf(lua, sizeof(lua), "FC4Cheats.setCfg('fly_speed', %.0f)", fs); Lua(lua, "=cheats");
            snprintf(lua, sizeof(lua), "%.0f", fs); SetState("cheat.fly_speed", lua);
        }
        ImGui::EndDisabled();
    }
    {
        bool freeze = GetState("cheat.fly_freeze", "0") == "1";
        ImGui::BeginDisabled(!LuaReady());
        if (ImGui::Checkbox("Freeze body while flying", &freeze)) {
            Lua(freeze ? "FC4Cheats.setCfg('fly_freeze', 1)" : "FC4Cheats.setCfg('fly_freeze', 0)", "=cheats");
            SetState("cheat.fly_freeze", freeze ? "1" : "0");
        }
        Help("Stops your arms/body animating while you fly (no flailing). Your gun won't animate either. Works straight away, also mid-flight.");
        ImGui::EndDisabled();
    }
    if (ActionButton("Launch up", "CAPI_Player.SetWorldSpeed")) Lua("FC4Cheats.launch(25)", "=cheats");
    Help("Throws you into the air. Pair it with No fall damage, or the wingsuit.");

    ImGui::SeparatorText("Camera & HUD");
    CheatSwitch("FOV", "fov", "CAPI_Player.SetPlayerFOV", "Your own field of view, past the game's menu limit. Uses the game's FOV override, so aiming and cutscenes still work.");
    ImGui::SameLine(ImGui::GetFontSize() * 14);
    CfgSlider("##fov", "cheat.fov", "90", 50, 150, "%.0f degrees", "FC4Cheats.setFov('fov', %.0f)");
    CheatSwitch("Weapon FOV", "fovnear", "CAPI_Player.SetPlayerNearFOV", "Field of view of your arms and gun (the game draws them with their own camera). Lower = gun looks bigger.");
    ImGui::SameLine(ImGui::GetFontSize() * 14);
    CfgSlider("##fovnear", "cheat.fov_near", "70", 30, 120, "%.0f degrees", "FC4Cheats.setFov('fovnear', %.0f)");
    {
        std::string gf = GetState("cheat.fov_game");
        if (!gf.empty()) ImGui::TextDisabled("Game FOV when you loaded in: %s degrees", gf.c_str());
    }
    CheatSwitch("Hide HUD", "hud", "SetHudVisible", "Hides the minimap, crosshair, prompts and meters. Bind a hotkey in Settings to flip it quickly.");

    ImGui::SeparatorText("Vehicles");
    CheatSwitch("Vehicle speed boost", "vboost", "CAPI_Vehicle.SetVehicleSpeed", "While you hold the throttle (W / right trigger) your vehicle speeds up faster, up to the top speed below. Let go or brake and it drives normally.");
    ImGui::SameLine(ImGui::GetFontSize() * 14);
    CfgSlider("##vboostx", "cheat.vboostx", "2", 1.25f, 6.0f, "%.2fx acceleration", "FC4Cheats.setVehicleCfg('vboostx', %.2f)");
    CfgSlider("top speed", "cheat.vmax", "250", 80, 2000, "%.0f km/h", "FC4Cheats.setVehicleCfg('vmax', %.0f)");
    ImGui::SameLine();
    if (ActionButton("Instant brake", "CAPI_Vehicle.SetVehicleSpeed")) Lua("FC4Cheats.brake()", "=cheats");
    Help("Stops your vehicle dead. Bind a hotkey for it in Settings.");

    ImGui::SeparatorText("Karma");
    {
        std::string k = GetState("cheat.karma");
        size_t bar = k.find('|');
        if (!k.empty() && bar != std::string::npos) ImGui::Text("Karma level %s   (%s karma points)", k.substr(0, bar).c_str(), k.substr(bar + 1).c_str());
        static int karma = 500;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::InputInt("##karma", &karma, 100, 1000);
        if (karma < 1) karma = 1;
        ImGui::SameLine();
        if (ActionButton("Give karma", "CAPI_Player.GivePlayerKarma")) { char lua[64]; snprintf(lua, sizeof(lua), "FC4Cheats.karma(%d)", karma); Lua(lua, "=cheats"); }
        Help("Karma levels unlock shop discounts and the karma rewards (like the Kyrat militia helping you).");
    }

    ImGui::SeparatorText("Money & progress");
    {
        static int money = 10000;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::InputInt("##money", &money, 1000, 10000);
        if (money < 0) money = 0;
        if (money > 9999999) money = 9999999;
        ImGui::SameLine();
        char lua[64];
        if (ActionButton("Set money", "CAPI_Player.SetPlayerMoney")) { snprintf(lua, sizeof(lua), "FC4Cheats.money('set', %d)", money); Lua(lua, "=cheats"); }
        ImGui::SameLine();
        if (ActionButton("Add money", "CAPI_Player.AddPlayerMoney")) { snprintf(lua, sizeof(lua), "FC4Cheats.money('add', %d)", money); Lua(lua, "=cheats"); }
        if (ActionButton("+1,000 XP", "CAPI_Player.GivePlayerXP")) Lua("FC4Cheats.xp(1000)", "=cheats"); ImGui::SameLine();
        if (ActionButton("+10,000 XP", "CAPI_Player.GivePlayerXP")) Lua("FC4Cheats.xp(10000)", "=cheats"); ImGui::SameLine();
        if (ActionButton("+1 skill point", "CAPI_Pawn.GiveSkillPoints")) Lua("FC4Cheats.skill(1)", "=cheats"); ImGui::SameLine();
        if (ActionButton("+5 skill points", "CAPI_Pawn.GiveSkillPoints")) Lua("FC4Cheats.skill(5)", "=cheats");
    }

    ImGui::SeparatorText("Slow motion");
    CheatSwitch("Slow motion##sw", "slowmo", "SetSlowMotion", "Slows the whole game down. Speed is set by the slider.");
    static float slow = -1;
    if (slow < 0 || !ImGui::IsAnyItemActive()) { std::string s = GetState("cheat.slow", "0.3"); slow = (float)atof(s.c_str()); }
    ImGui::SameLine(ImGui::GetFontSize() * 14);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    ImGui::BeginDisabled(!LuaReady() || !Avail("SetSlowMotion"));
    ImGui::SliderFloat("speed##slow", &slow, 0.05f, 1.0f, "%.2fx");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        char lua[64]; snprintf(lua, sizeof(lua), "FC4Cheats.setSlow(%.2f)", slow); Lua(lua, "=cheats");
        snprintf(lua, sizeof(lua), "%.2f", slow); SetState("cheat.slow", lua);
    }
    ImGui::EndDisabled();
    {
        bool sa = GetState("cheat.slow_audio", "1") != "0";
        ImGui::BeginDisabled(!LuaReady());
        if (ImGui::Checkbox("Slow the sound too", &sa)) {
            SetState("cheat.slow_audio", sa ? "1" : "0");
            Lua(sa ? "FC4Cheats.setSlowAudio(true)" : "FC4Cheats.setSlowAudio(false)", "=cheats");
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Sound and music play slower and deeper while slow motion is on.\n%d sound voices seen so far.", AudioVoiceCount());
    }

    ImGui::SeparatorText("Time of day");
    static int hour = -1, minute = 0;
    if (hour < 0 || !ImGui::IsAnyItemActive()) { hour = atoi(GetState("cheat.hour", "12").c_str()); minute = atoi(GetState("cheat.minute", "0").c_str()); }
    bool canTime = LuaReady() && Avail("SetTimeOfDay");
    ImGui::BeginDisabled(!canTime);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10); ImGui::SliderInt("hour", &hour, 0, 23, "%02d h");
    bool commit = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SameLine(); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10); ImGui::SliderInt("min", &minute, 0, 59, "%02d m");
    commit |= ImGui::IsItemDeactivatedAfterEdit();
    struct { const char* n; int h, m; } presets[] = { { "Dawn", 6, 0 }, { "Noon", 12, 0 }, { "Sunset", 18, 30 }, { "Night", 22, 0 }, { "Midnight", 0, 0 } };
    for (auto& p : presets) {
        if (&p != presets) ImGui::SameLine();
        if (ImGui::Button(p.n) && Debounced(p.n)) { hour = p.h; minute = p.m; commit = true; }
    }
    if (commit) {
        char lua[64]; snprintf(lua, sizeof(lua), "FC4Cheats.setTime(%d, %d)", hour, minute); Lua(lua, "=cheats");
        SetState("cheat.hour", std::to_string(hour)); SetState("cheat.minute", std::to_string(minute));
    }
    ImGui::EndDisabled();
    CheatSwitch("Lock time", "timelock", "SetTimeOfDay", "Keeps the clock at the time above (re-applied every few seconds).");

    ImGui::SeparatorText("Weather");
    if (ActionButton("Start storm", "StartStorm")) Lua("FC4Cheats.action('storm')", "=cheats"); ImGui::SameLine();
    if (ActionButton("Stop storm", "StopStorm")) Lua("FC4Cheats.action('calm')", "=cheats"); ImGui::SameLine();
    if (ActionButton("Full storm", "SetScriptedStormFactorOverride")) Lua("FC4Cheats.action('fullstorm')", "=cheats"); ImGui::SameLine();
    if (ActionButton("Clear override", "RemoveScriptedStormFactorOverride")) Lua("FC4Cheats.action('clearstorm')", "=cheats");
    if (ActionButton("Blizzard on", "SetBlizzardActive")) Lua("FC4Cheats.action('blizzard')", "=cheats"); ImGui::SameLine();
    if (ActionButton("Blizzard off", "SetBlizzardActive")) Lua("FC4Cheats.action('noblizzard')", "=cheats");
    Help("Blizzards only show up in the Himalayas (snow) areas.");

    ImGui::SeparatorText("Saved positions");
    ImGui::BeginDisabled(!LuaReady() || !Avail("teleport"));
    
    // 1. Read coordinates directly from disk to bypass memory state bridge issues
    std::map<int, std::string> diskPositions;
    std::wstring filePath = g_rootDir + L"positions.txt";
    FILE* posFile = _wfopen(filePath.c_str(), L"r");
    if (posFile) {
        char line[256];
        while (fgets(line, sizeof(line), posFile)) {
            int slotNum = 0;
            float fx = 0, fy = 0, fz = 0;
            // Parses the exact file format: slot1.000000=X,Y,Z
            if (sscanf(line, "slot%d.000000=%f,%f,%f", &slotNum, &fx, &fy, &fz) == 4) {
                char coordBuf[128];
                snprintf(coordBuf, sizeof(coordBuf), "%.2f,%.2f,%.2f", fx, fy, fz);
                diskPositions[slotNum] = coordBuf;
            }
        }
        fclose(posFile);
    }

    for (int i = 1; i <= 5; ++i) {
        ImGui::PushID(i);
        
        char lbl[64];
        if (ImGui::Button("Save here")) { 
            snprintf(lbl, sizeof(lbl), "FC4Cheats.savePos(%d)", i); 
            Lua(lbl, "=cheats"); 
        }
        ImGui::SameLine();
        
        // Check if this slot integer exists inside our file dictionary
        bool hasData = (diskPositions.count(i) > 0);
        
        ImGui::BeginDisabled(!hasData);
        if (ImGui::Button("Go")) { 
            snprintf(lbl, sizeof(lbl), "FC4Cheats.loadPos(%d)", i); 
            Lua(lbl, "=cheats"); 
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear")) { 
            snprintf(lbl, sizeof(lbl), "FC4Cheats.clearPos(%d)", i); 
            Lua(lbl, "=cheats"); 
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        
        float x = 0, y = 0, z = 0;
        if (hasData && sscanf(diskPositions[i].c_str(), "%f,%f,%f", &x, &y, &z) == 3) {
            ImGui::Text("Slot %d   x %6.0f   y %6.0f   height %4.0f", i, x, y, z);
        }
        else {
            ImGui::TextDisabled("Slot %d   (empty)", i);
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("World unlocks (permanent in your save)");
    ImGui::TextDisabled("These change your save game. Make a backup first if you care.");
    if (ActionButton("Unlock all safe houses", "UnlockAllSafeHouses")) Lua("FC4Cheats.action('safehouses')", "=cheats"); ImGui::SameLine();
    if (ActionButton("Enable fast travel", "SetFastTravelEnabled")) Lua("FC4Cheats.action('fasttravel')", "=cheats");
    if (ActionButton("Unlock all shop items", "SetShopItemsUnlockAll")) Lua("FC4Cheats.action('shop')", "=cheats"); ImGui::SameLine();
    if (ActionButton("Unlock all crafting", "SetCraftingUnlockAll")) Lua("FC4Cheats.action('crafting')", "=cheats");

    ImGui::SeparatorText("Outfit (experimental)");
    if (ActionButton("Costume on", "CAPI_Player.EquipCostume")) Lua("FC4Cheats.costume(true)", "=cheats"); ImGui::SameLine();
    if (ActionButton("Costume off", "CAPI_Player.EquipCostume")) Lua("FC4Cheats.costume(false)", "=cheats");
    Help("The game's own costume switch (EquipCostume). What it shows depends on your save and DLC.");

    ImGui::Separator();
    bool remember = GetState("cheat.remember", "1") == "1";
    ImGui::BeginDisabled(!LuaReady() || !ScriptsLoaded());
    if (ImGui::Checkbox("Remember switches after restarting the game", &remember))
        Lua(remember ? "FC4Cheats.setRemember(true)" : "FC4Cheats.setRemember(false)", "=cheats");
    ImGui::EndDisabled();
    ImGui::TextDisabled("Hotkeys for these can be set in Settings.");
}


} // namespace fc4
