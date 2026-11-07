// Declarations shared by the menu pages (src/page_*.cpp), the menu frame (overlay.cpp) and the
// small helper widgets (menu_common.cpp). Nothing here is part of the public plugin API.
#pragma once
#include "core.h"
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include "imgui.h"

namespace fc4 {

// ---- menu frame (overlay.cpp) -------------------------------------------------------------
void ShowMenuPage(const char* pageName);    // switch the sidebar to this page on the next frame
bool KeyPressed(int virtualKey);            // true on the frame a key goes down (polled, so it works while the game has raw input)
extern int g_captureKey;                    // Settings sets this to N while waiting for a key press for hotkey slot N

// Queue a snippet for the game's own Lua 5.1 (runs on the game's script thread).
inline void Lua(const char* code, const char* chunkName) { QueueLua(code, chunkName); }

// ---- helper widgets (menu_common.cpp) -----------------------------------------------------
bool Debounced(const char* label);                    // false if this same label fired within the last 400 ms
bool Avail(const char* native);                       // does this game build have that native function?
bool ScriptsLoaded();                                 // have FC4Hook's own Lua scripts finished loading?
void Help(const char* text);                          // a "(?)" after the previous widget that shows text on hover
bool ActionButton(const char* label, const char* native, ImVec2 size = ImVec2(0, 0));   // disabled until Lua is live and the native exists
void OpenFolder(const std::wstring& path);
std::string LuaSafe(const std::string& text);         // strips characters that would break a [[long string]]
std::string Lower(std::string text);
void NeedWorldNote();                                 // "load a save first" banner at the top of pages that need the world

// ---- data shared between pages ---------------------------------------------------------------
struct Weapon { std::string name, id, cat, source; };   // one line of armory\weapons.txt (or a mod's armory.txt)
const std::vector<Weapon>& ArmoryWeapons();             // loads the lists on first use (page_weapons.cpp)
std::string Trim(const std::string& text);              // strip spaces / tabs / newlines from both ends
extern std::string g_swapReplace, g_swapWith;           // the "Replace A with B" swap pair (page_file_activity.cpp)

// ---- pages: one function per sidebar entry -----------------------------------------------
void DrawCheatsPage();
void DrawWeaponsPage();
void DrawTeleportPage();
void DrawSpawnPage();
void DrawCoopPage();
void DrawWorkshopPage();
void DrawModelsPage();
void DrawInstalledModsPage();
void DrawConsolePage();
void DrawSettingsPage();
void DrawAboutPage();
void DrawDeveloperPage(const char* pageName);   // Lua, Natives, Memory, Animation, File Activity, Co-op Debug, Diagnostics
void DrawMemoryPage();
void DrawFileActivityPage();
void DrawAnimationPage();

// ---- cross-page helpers ---------------------------------------------------------------------
void RunScriptWithPrelude(const std::wstring& scriptName, const std::string& prelude);   // run fc4hook\scripts\<name> (page_developer.cpp)
void ProbeNative(const std::string& nativeName, const std::string& args);                // call a native inside pcall and log the result (page_developer.cpp)
void StartArchetypeCheck(std::function<std::string()> job);                              // background "why won't this spawn?" check (page_spawn.cpp)
void ModelExtract(const std::vector<std::string>& gamePaths);                            // extract models to the mod folder (page_models.cpp)

} // namespace fc4
