// Loads native C++ mods from fc4hook\plugins\*.dll
#include "core.h"
#include <mutex>

namespace fc4 {

static std::vector<std::string> s_loaded;
static std::mutex s_mx;

void LoadPlugins() {
    std::wstring dir = g_rootDir + L"plugins\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring path = dir + fd.cFileName;
        HMODULE m = LoadLibraryW(path.c_str());
        std::string name = Narrow(fd.cFileName);
        if (!m) { LogL(LOG_ERROR, "plugin %s: LoadLibrary failed (%lu)", name.c_str(), GetLastError()); continue; }
        auto init = (FC4ModInitFn)GetProcAddress(m, "FC4ModInit");
        if (!init) { LogL(LOG_WARN, "plugin %s: no FC4ModInit export, unloading", name.c_str()); FreeLibrary(m); continue; }
        if (!init(Api())) { LogL(LOG_WARN, "plugin %s: FC4ModInit returned false, unloading", name.c_str()); FreeLibrary(m); continue; }
        Log("plugin %s loaded", name.c_str());
        std::lock_guard<std::mutex> lk(s_mx);
        s_loaded.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::vector<std::string> LoadedPlugins() {
    std::lock_guard<std::mutex> lk(s_mx);
    return s_loaded;
}

} // namespace fc4
