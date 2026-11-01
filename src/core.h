#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include <atomic>
#include <functional>
#include <map>
#include "../include/fc4hook.h"

#define FC4HOOK_VERSION      "1.6.0"
#define FC4HOOK_TITLE        "FC4Hook v" FC4HOOK_VERSION

namespace fc4 {

// ---- paths / config ------------------------------------------------------
extern std::wstring g_rootDir;     // <bin>\fc4hook\ (trailing slash)
extern bool         g_menuOpen;
extern std::atomic<uint64_t> g_frameCounter;   // Present() calls (overlay.cpp)

struct Config {
    int   menuKey        = VK_F8;
    bool  showBanner     = true;
    float uiScale        = 1.0f;
    bool  developerMode  = false;
    int   logLevel       = 1;        // 0 debug, 1 info, 2 warn, 3 error
    bool  keepPrevLog    = true;
    bool  crashReports   = true;
    bool  traceFiles     = false;    // log the game's file opens for the first 2 minutes
    int   hkGodMode      = 0;        // hotkeys: virtual-key codes, 0 = none
    int   hkAmmo         = 0;
    int   hkRefill       = 0;
    int   hkSlowMo       = 0;
    int   hkMenuArmory   = 0;
    int   hkFly          = 0;
    int   hkHud          = 0;
    int   hkBrake        = 0;
    bool  padMenu        = true;     // hold L3+R3 to open/close the menu with a gamepad
};
extern Config g_cfg;
void LoadConfig();
void SaveConfig();
std::string KeyName(int vk);

std::wstring Widen(const std::string& s);
std::string  Narrow(const std::wstring& s);

// ---- log -------------------------------------------------------------------
enum LogLevel { LOG_DEBUG = 0, LOG_INFO = 1, LOG_WARN = 2, LOG_ERROR = 3 };
struct LogLine { int level; std::string text; };
void LogInit();
void Log(const char* fmt, ...);                 // info
void LogL(int level, const char* fmt, ...);
void LogV(int level, const char* fmt, va_list ap);
std::vector<LogLine> LogSnapshot();             // for the overlay console
uint64_t LogSerial();                           // bumps on every new line
void LogClear();                                // clears the console view (not the file)
int  LogCount(int level);                       // warnings/errors since start
const char* LevelName(int level);
void InstallCrashHandler();
void CheckCrashHandler();                       // re-chains if the game replaced our filter

// ---- memory ---------------------------------------------------------------
uintptr_t FindPattern(const char* module, const char* pattern);
uintptr_t ResolveRip(uintptr_t instr, int dispOffset, int instrLen);
int  CreateHook(void* target, void* detour, void** original);
int  RemoveHook(void* target);

// ---- game / lua bridge ---------------------------------------------------
bool GameInit();                          // resolve signatures, install hooks
bool GameReady();                         // signatures resolved
bool LuaReady();                          // script thread has ticked at least once
const char* BridgeStatus();               // human-readable state for the menu
uint64_t LuaHookCalls();                  // how many times the Lua VM called our hook
uint64_t PumpTicks();                     // ticks run from the message pump
uint64_t PumpRejectsMidLua();             // pump ticks skipped because Lua was mid-execution
void QueueLua(const std::string& code, const std::string& chunk);
void RegisterLuaFunction(const char* name, lua_CFunction fn);
void AddTickCallback(void (*cb)(lua_State*));
void RequestAutorunReload();
const FC4HookAPI* Api();

// Small key/value store that Lua scripts write with ModSetState(key, value) so the
// menu can show live state (cheat toggles, armory results...) without touching Lua.
void        SetState(const std::string& key, const std::string& value);
std::string GetState(const std::string& key, const std::string& def = "");
uint64_t    StateSerial();

const char* ModArchiveStatus();          // custom content archive (fc4hook_mods.dat)

// ---- loose-file mods + file log (vfs.cpp) ------------------------------------
uint64_t FileId(const char* path);       // the game's file id: CRC-64 of the lower-case path
bool VfsPrepare();                       // scan mods\*\files, (re)build data_win32\fc4hook_loose.*
void VfsMounted(bool ok);
std::string VfsStatus();
struct VfsModEntry { std::string name; int files; bool enabled; bool hasFiles; };
std::vector<VfsModEntry> VfsMods();
struct VfsFileEntry { uint64_t id; std::string rel, mod; uint64_t size; std::vector<std::string> beaten; };
std::vector<VfsFileEntry> VfsFiles();
void ArchetypeCatalogBuild();             // extract.cpp: scan the archives -> fc4hook\archetypes.txt
std::string ArchetypeCatalogStatus();
uint64_t ArchetypeRefField(uint64_t id, const char* field);   // archetype reference field, 0 = none
bool AnyFileRead(uint64_t id, std::vector<uint8_t>& out, std::string* err, std::string* from);  // vfs.cpp: loose mods first
std::string ArchetypeCheck(uint64_t id);   // extract.cpp: human-readable diagnosis of an archetype
std::string ArchetypeCheckMods();          // every archetype the enabled mods add or replace
// fcb.cpp: binary FCB files (archetypes etc.)
struct FcbValue { uint32_t hash = 0; std::string name, object; uint32_t offset = 0, size = 0; int depth = 0; bool shared = false; };
uint32_t FcbHash(const char* name);
std::string FcbName(uint32_t hash);                 // the game's own field name (from FC64.dll)
bool FcbParse(const std::vector<uint8_t>& d, std::vector<FcbValue>& out, std::string* err);
int FcbReplaceU64(std::vector<uint8_t>& d, uint64_t from, uint64_t to);
int FcbReplaceText(std::vector<uint8_t>& d, const std::string& from, const std::string& to);
std::string FcbFormat(const std::vector<uint8_t>& d, const FcbValue& v);
char FcbKind(const FcbValue& v);
char FcbKindOf(const std::vector<uint8_t>& d, const FcbValue& v);
// workshop.cpp: Weapon Workshop
struct WeaponChain { uint64_t item = 0, props = 0, pickup = 0; };
struct WorkshopWeapon { std::string mod, name; uint64_t base = 0; uint64_t ids[3] = {}; };
bool WeaponChainOf(uint64_t item, WeaponChain* c);
void WorkshopInstallGuard();
std::string WorkshopIdList();
int WorkshopRegister();                  // add Workshop weapons to the game's weapon -> properties map
std::string WorkshopRegisterStatus();
bool WorkshopCreate(uint64_t baseItem, const std::string& displayName, std::string* modOut, std::string* msg);
std::vector<WorkshopWeapon> WorkshopList();
bool WorkshopRead(const WorkshopWeapon& w, int part, std::vector<uint8_t>& d, std::vector<FcbValue>& vals, std::string* err);
bool WorkshopReadBase(const WorkshopWeapon& w, int part, std::vector<uint8_t>& d);
bool WorkshopSet(const WorkshopWeapon& w, int part, uint32_t offset, char kind, const std::string& text, std::string* err);
bool GameFileExists(uint64_t id);         // extract.cpp: indexes the game's archives on first use
bool VfsModEnabled(const std::string& name);
void VfsSetModEnabled(const std::string& name, bool on);   // takes effect next start
void FileLogInstall();
bool ResourceLogAvailable();
void FileLogAnnotate(const std::map<uint64_t, std::string>& labels);
int  LabelAnimationClips(std::string* msg);        // extract.cpp
struct ClipEvent { uint64_t tick; uint64_t id; uintptr_t ctx; };
void ClipMonitorInstall();
bool ClipMonitorAvailable();
bool ClipMonitorEnabled();
void ClipMonitorSetEnabled(bool on);              // also (re)loads labels from dumps\animation_clips.txt
void ClipMonitorClear();
uint64_t ClipMonitorSerial();
std::vector<ClipEvent> ClipMonitorEvents();       // newest first
std::string ClipLabelFor(uint64_t id);
bool FileLogAtStartup();
void FileLogSetAtStartup(bool on);
bool ParseLogLine(const std::string& line, uint64_t* id, std::string* path, std::string* type);
bool FileLogAvailable();
bool FileLogEnabled();
void FileLogSetEnabled(bool on);
void FileLogFlush();
void FileLogClear();
uint64_t FileLogCount();
uint64_t FileLogSerial();
std::vector<std::string> FileLogRecent(const std::string& filter, size_t max);

// ---- game archive extraction + swap test mods (extract.cpp) --------------------
bool Lzo1xDecompress(const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen, size_t* got);
bool GameFileRead(uint64_t id, std::vector<uint8_t>& out, std::string* err, std::string* from);
bool ExtractGameFile(uint64_t id, const std::string& path, std::string* msg);
bool ExtractModel(const std::string& path, std::string* msg);
bool XbgFromObj(std::vector<uint8_t>& xbg, const std::string& objText, std::string* report);   // xbgpatch.cpp
// Edited .obj -> .xbg: the original model is read from the game (or a mod), patched, written to outFile.
bool ModelFromObj(const std::string& originalGamePath, const std::wstring& objFile, const std::wstring& outFile, std::string* msg);      // .xbg + its materials + textures
std::vector<std::string> ArchetypeModels(uint64_t id);             // .xbg paths an archetype uses
void RunExtractJob(std::function<void()> job);
bool TextureToDds(const std::string& rel, std::string* msg);    // dumps\extracted\<rel>.xbt -> .dds
int  InstallEditedTextures(std::string* msg);                   // edited .dds -> mods\MyTextures
// texconv.cpp
bool XbtToDds(const std::vector<uint8_t>& d, const std::vector<uint8_t>* mip0, std::vector<uint8_t>& out, std::string* err);
bool DdsToXbt(const std::vector<uint8_t>& dds, const std::vector<uint8_t>& tmplD, const std::vector<uint8_t>* tmplMip0,
              std::vector<uint8_t>& outD, std::vector<uint8_t>& outMip0, std::string* err);
std::string Mip0Name(const std::string& xbt);
bool IsMip0Name(const std::string& xbt, std::string* base);
bool MakeSwap(uint64_t replaceId, const std::string& replacePath, uint64_t withId, const std::string& withPath, std::string* msg);
struct TravelSpot { char id[24]; float x, y, z; };
std::vector<TravelSpot> TravelList();     // unlocked towers, filled by ModTowerPositions()

struct SigStatus { const char* name; uintptr_t addr; };
std::vector<SigStatus> SignatureReport();

// ---- Lua helpers (game.cpp) + memory binder (memory.cpp) -------------------
int         LuaTop(lua_State* L);
const char* LuaArg(lua_State* L, int i);          // argument as string (numbers converted), or nullptr
int         LuaArgType(lua_State* L, int i);      // 0 nil 1 bool 3 number 4 string 5 table 6 function, -1 none
bool        LuaArgBool(lua_State* L, int i);
int         LuaReturn(lua_State* L, const std::string& expr);   // pushes the value of a Lua expression
std::string LuaQuote(const std::string& s);
void*       LuaNativeFn(lua_State* L, const char* name);
void*       EntityObject(lua_State* L, uint64_t id);
struct LuaReg { const char* name; lua_CFunction fn; };
bool        GameEvalString(lua_State* L, const std::string& code, const char* chunk, std::string* out);
lua_State*  GameLuaState();
bool        GameHasFocus();                      // overlay.cpp
bool        MenuWantsKeyboard();                 // overlay.cpp

// ---- Lua 5.4 runtime (lua54.cpp): runs alongside the game's Lua 5.1 ----------------
void Lua54OnGameState(lua_State* gameL);         // game Lua (re)started: init / notify scripts
void Lua54Tick(lua_State* gameL);                // once per frame, on the game's script thread
void Lua54RunJobs(lua_State* gameL);             // queued console code etc.
void Lua54Queue(const std::string& code, const std::string& chunk);
void Lua54Reload();                              // restart the 5.4 state and rerun lua\autorun
const char* Lua54Status();
struct Lua54Button { int id; std::string label, group; };
std::vector<Lua54Button> Lua54Buttons();         // buttons scripts added with fc4.menu.button()
void Lua54PressButton(int id);
const LuaReg* MemoryFunctions(int* count);
// fc4.ui (luaui.cpp): Lua 5.4 scripts' own ImGui windows
struct UiEvent { int id; double num; bool isText; std::string text; };
void LuaUiOpen(lua_State* L54);                  // adds fc4.ui to the fc4 table on top of the 5.4 stack
void LuaUiReset();
std::vector<UiEvent> LuaUiTakeEvents();
void LuaUiDraw(bool menuOpen);                   // render thread
int  LuaUiWindowCount();

// Cheat Engine style address expressions: "FC64+2DD8958", "[FC64+2DD8958]+8", "0x7FF6...",
// "[[game.exe+10]+20]-8". Numbers are hex (0x optional). Returns false + reason on error.
bool ParseAddress(const std::string& expr, uintptr_t* out, std::string* err);
bool MemReadable(uintptr_t a, size_t n);
bool MemWritable(uintptr_t a, size_t n);
std::string Hex(uintptr_t v);
std::string MemFormat(uintptr_t a, const char* type);           // value as text, "??" if unreadable
bool        MemWriteValue(uintptr_t a, const char* type, const char* value, std::string* err);

// ---- overlay / plugins -----------------------------------------------------
bool OverlayInit();
bool AudioInit();                                // XAudio2 hooks for slow-motion sound (audio.cpp)
void AudioSetSpeed(float speed);
float AudioGetSpeed();
int AudioVoiceCount();
bool InputInit();                                // DirectInput keyboard filter (input.cpp)
void PadPassThrough(bool on);                    // this thread reads the real pad (menu)
bool PadTriggers(int user, int* lt, int* rt);    // real XInput triggers 0..255
bool PadRead(int user, uint16_t* buttons);       // real XInput buttons, for the menu combo
void AddMenuCallback(void (*cb)());
void AddPresentCallback(void (*cb)(void* swapChain));
void* D3DDevice();                               // ID3D11Device* (null until the overlay attached)
void* D3DContext();                              // ID3D11DeviceContext*
void* D3DSwapChain();                            // IDXGISwapChain*
int  HookVtable(void* object, int index, void* detour, void** original);   // util.cpp
int  UnhookVtable(void* object, int index, void* original);
void* GetImGuiContextPtr();

void LoadPlugins();
std::vector<std::string> LoadedPlugins();

} // namespace fc4
