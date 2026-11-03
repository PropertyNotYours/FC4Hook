// Loose-file mods + the file request log.
//
// LOOSE FILES
//   fc4hook\mods\<ModName>\files\<game path>      e.g. ...\files\graphics\_textures\foo.xbt
// At startup FC4Hook collects every enabled mod's files, packs them (uncompressed) into
// data_win32\fc4hook_loose.fat/.dat and mounts that archive above everything else
// (fc4hook_mods.dat, patch.dat and the game's own archives). The archive is only rebuilt
// when a file was added, removed or changed (data_win32\fc4hook_loose.txt is the manifest).
// New paths work too: the game finds files by a hash of their path, so a file with a path the
// game never shipped (a new entityarchetypeslibrary\<id>.ark.fcb, a new texture...) is simply
// there when something asks for it.
//   * Files the Gibbed unpacker couldn't name (__UNKNOWN\...\<16 hex digits>.ext) keep their
//     hash: anything inside a folder called __unknown whose name is 16 hex digits is stored
//     under that hash.
//   * Load order: mods are applied alphabetically, or in the order of mods\load_order.txt
//     (one folder name per line); a later mod wins when two mods ship the same file.
//   * Disable a mod: F8 > Developer > Mods, or start its folder name with "_".
//
// FILE LOG
//   Every path the game turns into a file id goes through one function in FC64.dll
//   (FUN_180060ed0 on patch 1.10: normalize, then CRC-64 of the lower-case path). FC4Hook
//   hooks it; with the log switched on, each new path is written to dumps\file_log.txt with
//   its id, and marked when a loose mod supplies it. That's how you find what to replace.
#include "core.h"
#include <map>
#include <set>
#include <mutex>
#include <algorithm>
#include <deque>
#include <unordered_map>
#include <map>

namespace fc4 {

static uintptr_t FC64Base() { return (uintptr_t)GetModuleHandleA("FC64.dll"); }

// ------------------------------------------------------------------ hashing
static uint64_t s_crc[256];
static void CrcInit() {
    if (s_crc[1]) return;
    for (int i = 0; i < 256; ++i) {
        uint64_t c = (uint64_t)i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0xD800000000000000ull : c >> 1;
        s_crc[i] = c;
    }
}

uint64_t FileId(const char* path) {
    CrcInit();
    if (!path || !*path) return ~0ull;
    uint64_t h = 0;
    for (const unsigned char* p = (const unsigned char*)path; *p; ++p) {
        unsigned char ch = *p == '/' ? '\\' : (unsigned char)tolower(*p);
        h = s_crc[(h ^ ch) & 0xff] ^ (h >> 8);
    }
    return h;
}

// ------------------------------------------------------------------ scanning
struct LooseFile {
    std::wstring src;
    std::string  rel, mod;
    uint64_t     size = 0, mtime = 0;
    std::vector<std::string> beaten;     // mods that also ship this file and lost (load order)
};

struct VfsModInfo { std::string name; int files = 0; bool enabled = true; bool hasFiles = false; };

static std::mutex s_vfsMx;
static std::vector<VfsModInfo> s_mods;
static std::map<uint64_t, LooseFile> s_files;          // id -> file (after load order)
static std::string s_vfsStatus = "not started";
static std::atomic<int> s_vfsState{ 0 };               // 0 pending, 1 ready, 2 nothing, -1 error

static std::wstring IniPath() { return g_rootDir + L"fc4hook.ini"; }

bool VfsModEnabled(const std::string& name) {
    if (name.empty() || name[0] == '_' || name[0] == '.') return false;
    return GetPrivateProfileIntW(L"Mods", Widen(name).c_str(), 1, IniPath().c_str()) != 0;
}

void VfsSetModEnabled(const std::string& name, bool on) {
    WritePrivateProfileStringW(L"Mods", Widen(name).c_str(), on ? L"1" : L"0", IniPath().c_str());
    std::lock_guard<std::mutex> lk(s_vfsMx);
    for (auto& m : s_mods) if (m.name == name) m.enabled = on;
}

static std::string Lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; }

static bool IsHex16(const std::string& s) {
    if (s.size() != 16) return false;
    for (char c : s) if (!isxdigit((unsigned char)c)) return false;
    return true;
}

// id for a file at <files>\rel ; rel uses backslashes
static uint64_t IdForRel(const std::string& rel) {
    std::string l = Lower(rel);
    size_t slash = l.find_last_of('\\');
    std::string name = slash == std::string::npos ? l : l.substr(slash + 1);
    std::string stem = name.substr(0, name.find('.'));
    bool unknown = l.find("__unknown\\") != std::string::npos || l.rfind("__unknown", 0) == 0;
    if (unknown && IsHex16(stem)) return _strtoui64(stem.c_str(), nullptr, 16);
    return FileId(l.c_str());
}

static void Walk(const std::wstring& dir, const std::string& relPrefix, const std::string& mod,
                 std::map<uint64_t, LooseFile>& out, int& count) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::string rel = relPrefix + Narrow(n);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            Walk(dir + n + L"\\", rel + "\\", mod, out, count);
            continue;
        }
        uint64_t size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        if (size >= (1ull << 30)) { LogL(LOG_WARN, "mods: %s\\%s is over 1 GB, skipped", mod.c_str(), rel.c_str()); continue; }
        LooseFile f;
        f.src = dir + n; f.rel = rel; f.mod = mod; f.size = size;
        f.mtime = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        uint64_t id = IdForRel(rel);
        auto it = out.find(id);
        if (it != out.end() && it->second.mod != mod) {
            LogL(LOG_INFO, "mods: %s overrides %s for %s", mod.c_str(), it->second.mod.c_str(), rel.c_str());
            f.beaten = it->second.beaten;
            f.beaten.push_back(it->second.mod);
        }
        out[id] = f;
        ++count;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static std::vector<std::string> ModFolders() {
    std::vector<std::string> all;
    std::wstring root = g_rootDir + L"mods\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            all.push_back(Narrow(n));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(all.begin(), all.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
    // load_order.txt: listed folders first, in that order; the rest after, alphabetically
    std::vector<std::string> order;
    FILE* f = _wfopen((root + L"load_order.txt").c_str(), L"rb");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            std::string s = line;
            while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
            if (s.empty() || s[0] == '#') continue;
            for (auto& a : all) if (Lower(a) == Lower(s) && std::find(order.begin(), order.end(), a) == order.end()) order.push_back(a);
        }
        fclose(f);
    }
    for (auto& a : all) if (std::find(order.begin(), order.end(), a) == order.end()) order.push_back(a);
    return order;
}

// ------------------------------------------------------------------ archive writer
static std::wstring DataDir() { return g_rootDir + L"..\\..\\data_win32\\"; }

static bool WriteAll(HANDLE h, const void* p, size_t n) {
    const char* c = (const char*)p;
    while (n) {
        DWORD w = 0, chunk = (DWORD)std::min<size_t>(n, 1 << 20);
        if (!WriteFile(h, c, chunk, &w, nullptr) || !w) return false;
        c += w; n -= w;
    }
    return true;
}

static std::string Manifest(const std::map<uint64_t, LooseFile>& files) {
    std::string m = "# FC4Hook loose-file archive manifest (rebuilt automatically, don't edit)\n";
    char buf[96];
    for (auto& kv : files) {
        snprintf(buf, sizeof(buf), "%016llx %llu %llu ", (unsigned long long)kv.first,
                 (unsigned long long)kv.second.size, (unsigned long long)kv.second.mtime);
        m += buf; m += kv.second.mod + "\\" + kv.second.rel + "\n";
    }
    return m;
}

static std::string ReadText(const std::wstring& p) {
    std::string s;
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return s;
    char b[65536]; size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}

static bool BuildArchive(const std::map<uint64_t, LooseFile>& files, std::string* err) {
    std::wstring base = DataDir() + L"fc4hook_loose";
    std::wstring datTmp = base + L".dat.tmp", fatTmp = base + L".fat.tmp";
    HANDLE dat = CreateFileW(datTmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dat == INVALID_HANDLE_VALUE) { *err = "can't write to data_win32 (error " + std::to_string(GetLastError()) + ")"; return false; }
    struct Ent { uint64_t id, off, size; };
    std::vector<Ent> ents;
    uint64_t off = 0;
    std::vector<char> buf(1 << 20);
    static const char zeros[16] = {};
    bool ok = true;
    for (auto& kv : files) {
        uint32_t pad = (uint32_t)((16 - (off & 15)) & 15);       // uncompressed entries are 16-byte aligned
        if (pad && !WriteAll(dat, zeros, pad)) { ok = false; break; }
        off += pad;
        HANDLE in = CreateFileW(kv.second.src.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (in == INVALID_HANDLE_VALUE) { *err = "can't read " + kv.second.mod + "\\" + kv.second.rel; ok = false; break; }
        uint64_t got = 0;
        DWORD r = 0;
        while (ReadFile(in, buf.data(), (DWORD)buf.size(), &r, nullptr) && r) {
            if (!WriteAll(dat, buf.data(), r)) { ok = false; break; }
            got += r;
        }
        CloseHandle(in);
        if (!ok) break;
        ents.push_back({ kv.first, off, got });
        off += got;
    }
    CloseHandle(dat);
    if (!ok) { if (err->empty()) *err = "write failed (disk full?)"; DeleteFileW(datTmp.c_str()); return false; }

    // FAT2 v9: magic, version, 12-byte header, count, 20-byte entries sorted by id, 8 zero bytes
    std::sort(ents.begin(), ents.end(), [](const Ent& a, const Ent& b) { return a.id < b.id; });
    std::vector<uint8_t> fat;
    auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) fat.push_back((uint8_t)(v >> (8 * i))); };
    put32(0x46415432); put32(9);
    put32(0x00000301); put32(0); put32(0);          // same header as the game's own patch archives
    put32((uint32_t)ents.size());
    for (auto& e : ents) {
        put32((uint32_t)(e.id >> 32)); put32((uint32_t)e.id);
        put32(0);                                          // uncompressed size 0 + scheme 0 = stored
        put32((uint32_t)(e.off >> 2));
        put32((uint32_t)(((e.off & 3) << 30) | (e.size & 0x3fffffff)));
    }
    put32(0); put32(0);
    HANDLE fh = CreateFileW(fatTmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE || !WriteAll(fh, fat.data(), fat.size())) {
        if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
        *err = "can't write fc4hook_loose.fat"; DeleteFileW(datTmp.c_str()); return false;
    }
    CloseHandle(fh);
    if (!MoveFileExW(datTmp.c_str(), (base + L".dat").c_str(), MOVEFILE_REPLACE_EXISTING) ||
        !MoveFileExW(fatTmp.c_str(), (base + L".fat").c_str(), MOVEFILE_REPLACE_EXISTING)) {
        *err = "can't replace the old fc4hook_loose archive (is another copy of the game running?)";
        return false;
    }
    return true;
}

// Scan mods\ and make sure data_win32\fc4hook_loose.* matches. Returns true when there is an
// archive to mount. Runs on the mod-archive thread at startup.
bool VfsPrepare() {
    ULONGLONG t0 = GetTickCount64();
    std::map<uint64_t, LooseFile> files;
    std::vector<VfsModInfo> mods;
    for (auto& name : ModFolders()) {
        VfsModInfo mi; mi.name = name; mi.enabled = VfsModEnabled(name);
        std::wstring fdir = g_rootDir + L"mods\\" + Widen(name) + L"\\files\\";
        mi.hasFiles = GetFileAttributesW(fdir.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (mi.hasFiles && mi.enabled) Walk(fdir, "", name, files, mi.files);
        mods.push_back(mi);
    }
    std::wstring base = DataDir() + L"fc4hook_loose";
    std::string manifest = Manifest(files);
    std::string status;
    int state;
    if (files.empty()) {
        DeleteFileW((base + L".fat").c_str()); DeleteFileW((base + L".dat").c_str()); DeleteFileW((base + L".txt").c_str());
        status = "no loose-file mods"; state = 2;
    } else if (ReadText(base + L".txt") == manifest &&
               GetFileAttributesW((base + L".fat").c_str()) != INVALID_FILE_ATTRIBUTES &&
               GetFileAttributesW((base + L".dat").c_str()) != INVALID_FILE_ATTRIBUTES) {
        status = std::to_string(files.size()) + " files (unchanged, reused)"; state = 1;
    } else {
        std::string err;
        DeleteFileW((base + L".txt").c_str());
        if (BuildArchive(files, &err)) {
            FILE* f = _wfopen((base + L".txt").c_str(), L"wb");
            if (f) { fwrite(manifest.data(), 1, manifest.size(), f); fclose(f); }
            status = std::to_string(files.size()) + " files (rebuilt in " + std::to_string(GetTickCount64() - t0) + " ms)"; state = 1;
        } else {
            status = "FAILED: " + err; state = -1;
            LogL(LOG_ERROR, "mods: building the loose-file archive failed: %s", err.c_str());
        }
    }
    {
        std::lock_guard<std::mutex> lk(s_vfsMx);
        s_files = std::move(files); s_mods = std::move(mods); s_vfsStatus = status;
    }
    s_vfsState = state;
    LogL(state < 0 ? LOG_ERROR : LOG_INFO, "mods: loose files: %s", status.c_str());
    return state == 1;
}

void VfsMounted(bool ok) {
    std::lock_guard<std::mutex> lk(s_vfsMx);
    if (!ok) { s_vfsStatus += " - but mounting FAILED (see log)"; s_vfsState = -1; }
    else s_vfsStatus += ", mounted";
}

// Every loose file the archive holds (for the Mods tab's file browser).
std::vector<VfsFileEntry> VfsFiles() {
    std::lock_guard<std::mutex> lk(s_vfsMx);
    std::vector<VfsFileEntry> r;
    r.reserve(s_files.size());
    for (auto& kv : s_files) r.push_back({ kv.first, kv.second.rel, kv.second.mod, kv.second.size, kv.second.beaten });
    return r;
}

std::string VfsStatus() { std::lock_guard<std::mutex> lk(s_vfsMx); return s_vfsStatus; }

std::vector<VfsModEntry> VfsMods() {
    std::lock_guard<std::mutex> lk(s_vfsMx);
    std::vector<VfsModEntry> r;
    for (auto& m : s_mods) r.push_back({ m.name, m.files, m.enabled, m.hasFiles });
    return r;
}

// Read file `id` the way the game will see it: a loose mod's file first, then the game's
// archives. *from = "mod <name>: <path>" or the archive name.
bool AnyFileRead(uint64_t id, std::vector<uint8_t>& out, std::string* err, std::string* from) {
    std::wstring src; std::string where;
    {
        std::lock_guard<std::mutex> lk(s_vfsMx);
        auto it = s_files.find(id);
        if (it != s_files.end()) { src = it->second.src; where = "mod " + it->second.mod + ": " + it->second.rel; }
    }
    if (!src.empty()) {
        FILE* f = _wfopen(src.c_str(), L"rb");
        if (!f) { if (err) *err = "can't open " + Narrow(src); return false; }
        out.clear(); uint8_t b[65536]; size_t n;
        while ((n = fread(b, 1, sizeof(b), f)) > 0) out.insert(out.end(), b, b + n);
        fclose(f);
        if (from) *from = where;
        return true;
    }
    std::string e;
    return GameFileRead(id, out, err ? err : &e, from);
}

// game path (as written in the mod) supplied by a loose mod for this id, or ""
static std::string LooseNameFor(uint64_t id) {
    std::lock_guard<std::mutex> lk(s_vfsMx);
    auto it = s_files.find(id);
    return it == s_files.end() ? std::string() : it->second.mod;
}

// ------------------------------------------------------------------ file log
typedef void (*PathToId_t)(uint64_t* out, const char* path, unsigned flags);
static PathToId_t o_PathToId = nullptr;
static std::atomic<bool> s_logOn{ false };
static std::mutex s_logMx;
static std::set<uint64_t> s_seen;
static std::deque<std::string> s_recent;
static std::atomic<uint64_t> s_logCount{ 0 }, s_logSerial{ 0 };
static FILE* s_logFile = nullptr;

static std::unordered_map<uint64_t, std::string> s_names;   // id -> path, from the path hook
static std::set<uint64_t> s_resSeen;

static void AddLogLine(const std::string& line) {           // s_logMx held
    ++s_logCount; ++s_logSerial;
    s_recent.push_back(line);
    if (s_recent.size() > 6000) s_recent.pop_front();
    if (s_logFile) { fputs(line.c_str(), s_logFile); fputc('\n', s_logFile); }
}

static void hk_PathToId(uint64_t* out, const char* path, unsigned flags) {
    o_PathToId(out, path, flags);
    if (!s_logOn.load(std::memory_order_relaxed) || !path || !out) return;
    char p[272];
    size_t n = 0;
    for (; n < sizeof(p) - 1 && path[n]; ++n) p[n] = path[n];      // the game just read it, so it's valid
    p[n] = 0;
    if (!n) return;
    uint64_t id = *out;
    std::string mod = LooseNameFor(id);
    char line[400];
    snprintf(line, sizeof(line), "%016llx  %s%s%s", (unsigned long long)id, p, mod.empty() ? "" : "   <- ", mod.c_str());
    std::lock_guard<std::mutex> lk(s_logMx);
    if (!s_seen.insert(id).second) return;
    s_names[id] = p;
    AddLogLine(line);
}

// RESOURCE LOG: FC64's resource manager creates every resource (textures, meshes, materials,
// animation packages, move data, sounds...) through one function the first time anything
// asks for it: FUN_1800d4950(manager, &id, typeKey, flag) on patch 1.10. The id is the same
// file id the archives use, so Extract / Swap work on these lines too. Logged with the
// resource's engine type (the object's vfunc 1 returns its type descriptor; name at +0).
// Animation packages aren't archive files (built in memory), so keep their objects to dump.
static std::mutex s_objMx;
static std::map<uint64_t, void*> s_resObj;

// Writes the object and everything reachable through pointers (3 levels, 4 KB each) as
// records: u64 address, u32 size, bytes. First record: module base (size 0).
bool DumpResourceMemory(uint64_t id, std::string* msg) {
    void* obj = nullptr;
    { std::lock_guard<std::mutex> lk(s_objMx); auto it = s_resObj.find(id); if (it != s_resObj.end()) obj = it->second; }
    if (!obj) { *msg = "not in the archives, and no package object seen for it this session"; return false; }
    std::vector<uint8_t> out;
    auto put = [&](const void* p, size_t n) { const uint8_t* b = (const uint8_t*)p; out.insert(out.end(), b, b + n); };
    uint64_t base = (uint64_t)GetModuleHandleW(L"FC64.dll"); uint32_t z = 0;
    put(&base, 8); put(&z, 4);
    std::set<uintptr_t> seen; std::vector<std::pair<uintptr_t, int>> q{ {(uintptr_t)obj, 0} };
    for (size_t qi = 0; qi < q.size() && out.size() < (32u << 20); ++qi) {
        uintptr_t a = q[qi].first; int depth = q[qi].second;
        if (!seen.insert(a).second) continue;
        uint32_t n = 4096;
        while (n >= 64 && !MemReadable(a, n)) n /= 2;
        if (n < 64) continue;
        uint64_t a64 = a; put(&a64, 8); put(&n, 4); put((void*)a, n);
        if (depth >= 3) continue;
        for (uint32_t o = 0; o + 8 <= n; o += 8) {
            uintptr_t p = *(uintptr_t*)(a + o);
            if (p < 0x10000 || (p & 7) || p > 0x7fffffffffffULL) continue;
            if (base && p >= base && p < base + 0x4000000) continue;   // code / vtables
            if (MemReadable(p, 64)) q.push_back({ p, depth + 1 });
        }
    }
    char nm[64]; snprintf(nm, sizeof(nm), "%016llx", (unsigned long long)id);
    std::wstring path = g_rootDir + L"dumps\\extracted\\__unknown\\packages\\" + Widen(nm) + L".mem";
    if (!WriteFileAll(path, out)) { *msg = "can't write " + Narrow(path); return false; }
    *msg = std::string("dumped package ") + nm + " from memory (" + std::to_string(out.size() / 1024) + " KB) to dumps\\extracted\\__unknown\\packages\\";
    return true;
}


// PACKAGE USE: every time the game looks up an animation package (FUN_1800c9bf0, the resource
// manager's id -> resource lookup, called for everything), log which code asked for it as
// FC64 offsets of the call stack, once per (package, caller chain). Packages are found in a
// small lock-free id table filled by the resource-create hook.
static std::atomic<uint64_t> s_pkgTab[4096];
static void PkgAdd(uint64_t id) {
    if (!id || id == ~0ull) return;
    for (uint32_t i = (uint32_t)(id ^ (id >> 29)) & 4095, n = 0; n < 4096; i = (i + 1) & 4095, ++n) {
        uint64_t cur = s_pkgTab[i].load(std::memory_order_relaxed);
        if (cur == id) return;
        if (!cur) { uint64_t z = 0; if (s_pkgTab[i].compare_exchange_strong(z, id)) return; if (z == id) return; }
    }
}
static bool PkgHas(uint64_t id) {
    for (uint32_t i = (uint32_t)(id ^ (id >> 29)) & 4095, n = 0; n < 4096; i = (i + 1) & 4095, ++n) {
        uint64_t cur = s_pkgTab[i].load(std::memory_order_relaxed);
        if (cur == id) return true;
        if (!cur) return false;
    }
    return false;
}
typedef void* (*FindRes_t)(void* mgr, uint64_t* id);
static FindRes_t o_FindRes = nullptr;
static std::set<std::string> s_pkgSeen;
static void* hk_FindRes(void* mgr, uint64_t* idp) {
    void* r = o_FindRes(mgr, idp);
    if (!idp || !s_logOn.load(std::memory_order_relaxed) || !PkgHas(*idp)) return r;
    void* st[14] = {}; USHORT n = RtlCaptureStackBackTrace(1, 14, st, nullptr);
    uintptr_t base = FC64Base();
    std::string chain;
    for (USHORT k = 0; k < n; ++k) {
        uintptr_t a = (uintptr_t)st[k];
        if (a < base || a > base + 0x4000000) continue;
        char b[24]; snprintf(b, sizeof(b), " +%llx", (unsigned long long)(a - base)); chain += b;
    }
    char idb[20]; snprintf(idb, sizeof(idb), "%016llx", (unsigned long long)*idp);
    std::lock_guard<std::mutex> lk(s_logMx);
    if (!s_pkgSeen.insert(std::string(idb) + chain).second) return r;
    auto it = s_names.find(*idp);
    std::string line = std::string(idb) + "  [PackageUse] " + (it == s_names.end() ? "?" : it->second) + "  from FC64" + chain;
    AddLogLine(line.c_str());
    return r;
}

// PACKAGE ADD: the player's animation object gets packages through a virtual method (vtable
// +0xe0 add, +0xe8 remove; FUN_181806b20 adds additive_offset / vehicle_enter that way). Hook
// that function, and through the object it gets, the add/remove methods: logs every package
// added to / removed from a character, with the calling code.
typedef void (*PkgOp_t)(void* self, void* pkg);
static PkgOp_t o_PkgAdd = nullptr, o_PkgRemove = nullptr;
typedef void (*AddDefaults_t)(void* a, void* animObj);
static AddDefaults_t o_AddDefaults = nullptr;
static std::atomic<bool> s_pkgOpsHooked{ false };
static void LogPkgOp(const char* what, void* self, void* pkg) {
    if (!s_logOn.load(std::memory_order_relaxed) || !pkg || !MemReadable((uintptr_t)pkg + 0x10, 8)) return;
    uint64_t id = *(uint64_t*)((uintptr_t)pkg + 0x10);
    void* st[14] = {}; USHORT n = RtlCaptureStackBackTrace(2, 14, st, nullptr);
    uintptr_t base = FC64Base(); std::string chain;
    for (USHORT k = 0; k < n; ++k) {
        uintptr_t a = (uintptr_t)st[k];
        if (a < base || a > base + 0x4000000) continue;
        char b[24]; snprintf(b, sizeof(b), " +%llx", (unsigned long long)(a - base)); chain += b;
    }
    char idb[20]; snprintf(idb, sizeof(idb), "%016llx", (unsigned long long)id);
    std::lock_guard<std::mutex> lk(s_logMx);
    auto it = s_names.find(id);
    std::string nm = it == s_names.end() ? "?" : it->second;
    if (!s_pkgSeen.insert(std::string(what) + idb + chain).second) return;
    char ob[32]; snprintf(ob, sizeof(ob), " to %p", self);
    AddLogLine(std::string(idb) + "  [" + what + "] " + nm + ob + "  from FC64" + chain);
}
static void hk_PkgAdd(void* self, void* pkg) { LogPkgOp("PackageAdd", self, pkg); o_PkgAdd(self, pkg); }
static void hk_PkgRemove(void* self, void* pkg) { LogPkgOp("PackageRemove", self, pkg); o_PkgRemove(self, pkg); }
static void hk_AddDefaults(void* a, void* animObj) {
    if (animObj && !s_pkgOpsHooked.exchange(true)) {
        HookVtable(animObj, 0xe0 / 8, (void*)hk_PkgAdd, (void**)&o_PkgAdd);
        HookVtable(animObj, 0xe8 / 8, (void*)hk_PkgRemove, (void**)&o_PkgRemove);
        LogL(LOG_DEBUG, "file log: package add/remove hooked");
    }
    o_AddDefaults(a, animObj);
}

typedef void* (*CreateRes_t)(void* mgr, uint64_t* id, void* type, uint8_t flag);
static CreateRes_t o_CreateRes = nullptr;

static void* hk_CreateRes(void* mgr, uint64_t* idp, void* type, uint8_t flag) {
    void* obj = o_CreateRes(mgr, idp, type, flag);
    if (!s_logOn.load(std::memory_order_relaxed) || !obj || !idp) return obj;
    uint64_t id = *idp;
    const char* tname = "?";
    void** vt = *(void***)obj;
    if (vt && MemReadable((uintptr_t)(vt + 1), 8)) {
        typedef void* (*TypeOf_t)(void*);
        void* desc = ((TypeOf_t)vt[1])(obj);                  // the game does the same right after
        if (desc && MemReadable((uintptr_t)desc, 8)) {
            const char* nm = *(const char**)desc;
            if (nm && MemReadable((uintptr_t)nm, 2) && nm[0] >= 'A' && nm[0] <= 'z') tname = nm;
        }
    }
    char tn[96];
    snprintf(tn, sizeof(tn), "%s", tname);
    if (strcmp(tn, "CAnimationPackageResource") == 0) { { std::lock_guard<std::mutex> lk2(s_objMx); s_resObj[id] = obj; } PkgAdd(id); }
    std::string mod = LooseNameFor(id);
    std::lock_guard<std::mutex> lk(s_logMx);
    if (!s_resSeen.insert(id).second) return obj;
    auto it = s_names.find(id);
    char line[480];
    snprintf(line, sizeof(line), "%016llx  [%s] %s%s%s", (unsigned long long)id, tn,
             it == s_names.end() ? "(no path)" : it->second.c_str(), mod.empty() ? "" : "   <- ", mod.c_str());
    AddLogLine(line);
    return obj;
}

// "<16 hex>  [Type] path   <- Mod" / "<16 hex>  path   <- Mod" -> id, path ("" if none), type
bool ParseLogLine(const std::string& line, uint64_t* id, std::string* path, std::string* type) {
    if (line.size() < 16) return false;
    *id = _strtoui64(line.substr(0, 16).c_str(), nullptr, 16);
    std::string rest = line.size() > 18 ? line.substr(18) : "";
    size_t mk = rest.find("   <- "); if (mk != std::string::npos) rest.resize(mk);
    if (type) type->clear();
    if (!rest.empty() && rest[0] == '[') {
        size_t e = rest.find("] ");
        if (e != std::string::npos) { if (type) *type = rest.substr(1, e - 1); rest = rest.substr(e + 2); }
    }
    size_t lb = rest.find(" {");
    if (lb != std::string::npos && rest.back() == '}') rest.resize(lb);
    if (rest == "(no path)") rest.clear();
    *path = rest;
    return true;
}

void FileLogInstall() {
    uintptr_t f = FindPattern("FC64.dll",
        "48 89 5C 24 08 48 89 74 24 10 57 48 81 EC 30 01 00 00 41 8B F8 48 8B DA 48 8B F1 C6 44 24 20 00 48 85 D2");
    if (!f) { LogL(LOG_WARN, "file log: path-to-id function not found; file log unavailable"); }
    else if (!CreateHook((void*)f, (void*)hk_PathToId, (void**)&o_PathToId)) { LogL(LOG_WARN, "file log: hook failed"); o_PathToId = nullptr; }
    else LogL(LOG_DEBUG, "file log: hooked path-to-id at FC64+0x%llx", (unsigned long long)(f - FC64Base()));
    uintptr_t r = FindPattern("FC64.dll",
        "48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 41 0F B6 E9 48 8B F2 48 8B F9 48 39 02 75 12 33 C0");
    if (!r) LogL(LOG_WARN, "file log: resource-create function not found; resource log unavailable");
    else if (!CreateHook((void*)r, (void*)hk_CreateRes, (void**)&o_CreateRes)) { LogL(LOG_WARN, "file log: resource hook failed"); o_CreateRes = nullptr; }
    else LogL(LOG_DEBUG, "file log: hooked resource-create at FC64+0x%llx", (unsigned long long)(r - FC64Base()));
    uintptr_t q = FindPattern("FC64.dll", "4C 8B 59 48 4C 8B 0A 4C 8B D1 4D 85 DB 74 ?? B8 5F 0B 4E 83 45 8B C1 41 81 F0 EF BE AD DE");
    if (!q) LogL(LOG_WARN, "file log: resource-lookup function not found; package use log unavailable");
    else if (!CreateHook((void*)q, (void*)hk_FindRes, (void**)&o_FindRes)) { LogL(LOG_WARN, "file log: lookup hook failed"); o_FindRes = nullptr; }
    uintptr_t a = FindPattern("FC64.dll", "40 56 48 83 EC 30 48 8B F2 E8 ?? ?? ?? ?? 48 85 F6 0F 84 32 01 00 00 48 89 5C 24 40 48 89 6C 24 58 48 8D 15");
    if (!a) LogL(LOG_WARN, "file log: package-defaults function not found");
    else if (!CreateHook((void*)a, (void*)hk_AddDefaults, (void**)&o_AddDefaults)) o_AddDefaults = nullptr;
}

bool ResourceLogAvailable() { return o_CreateRes != nullptr; }

// CLIP MONITOR: FC64's animation-clip node starts playing a clip in one function
// (FUN_180514b00 on patch 1.10, shared by the Move graph's clip node classes): node+0x28 is
// the CAnimationResource, node+0x38 the clip id (m_animNameHash = the clip's file id). Logging
// it shows exactly which clips start when you do something (reload, draw, melee...).
typedef void (*ClipStart_t)(void* node, void* ctx, uint64_t a3, uint64_t a4, uint8_t a5, uint8_t a6, void** out);
static ClipStart_t o_ClipStart = nullptr;
static std::atomic<bool> s_clipOn{ false };
static std::mutex s_clipMx;
static std::deque<ClipEvent> s_clips;
static std::atomic<uint64_t> s_clipSerial{ 0 };
static std::map<uint64_t, std::string> s_clipLabels;

static void hk_ClipStart(void* node, void* ctx, uint64_t a3, uint64_t a4, uint8_t a5, uint8_t a6, void** out) {
    o_ClipStart(node, ctx, a3, a4, a5, a6, out);
    if ((!s_clipOn.load(std::memory_order_relaxed) && !s_logOn.load(std::memory_order_relaxed)) || !node) return;
    uint64_t id = *(uint64_t*)((uint8_t*)node + 0x38);
    if (id == ~0ull) return;
    if (s_logOn.load(std::memory_order_relaxed)) {           // [ClipStart]: who started it, and the node's bytes
        void* st[16] = {}; USHORT n = RtlCaptureStackBackTrace(1, 16, st, nullptr);
        uintptr_t base = FC64Base(); std::string chain;
        for (USHORT k = 0; k < n; ++k) {
            uintptr_t a = (uintptr_t)st[k];
            if (a < base || a > base + 0x4000000) continue;
            char b[24]; snprintf(b, sizeof(b), " +%llx", (unsigned long long)(a - base)); chain += b;
        }
        char idb[20]; snprintf(idb, sizeof(idb), "%016llx", (unsigned long long)id);
        std::lock_guard<std::mutex> lk(s_logMx);
        if (s_pkgSeen.insert(std::string("clip") + idb + chain).second) {
            std::string hex;
            uintptr_t vt = MemReadable((uintptr_t)node, 8) ? *(uintptr_t*)node : 0;
            char vb[40]; snprintf(vb, sizeof(vb), " vt=+%llx node=", (unsigned long long)(vt > base ? vt - base : vt)); hex = vb;
            if (MemReadable((uintptr_t)node, 0x80)) for (int i = 0; i < 0x80; ++i) { char h[4]; snprintf(h, sizeof(h), "%02x", ((uint8_t*)node)[i]); hex += h; if ((i & 7) == 7) hex += ' '; }
            // where does the current weapon's name hash (crc32 "kimber" = 634df464, "ak74m" = 275ac4e5) live?
            std::string where;
            static const uint32_t want[] = { 0x634df464u, 0x275ac4e5u };
            auto scan = [&](uintptr_t b, size_t len, const char* tag) {
                for (size_t o = 0; o + 4 <= len; o += 4) {
                    if (!MemReadable(b + o, 4)) { o = (o | 0xfff); continue; }
                    uint32_t v = *(uint32_t*)(b + o);
                    for (uint32_t w : want) if (v == w) { char t[64]; snprintf(t, sizeof(t), " %s+%zx=%08x", tag, o, v); where += t; }
                    if (o + 8 <= len && !(o & 7)) {
                        uint64_t v8 = *(uint64_t*)(b + o);
                        if (v8 == 0x423d2930a6655000ull || v8 == 0x50dbe9450bb00000ull) { char t[64]; snprintf(t, sizeof(t), " %s+%zx=pkg%016llx", tag, o, (unsigned long long)v8); where += t; }
                    }
                }
            };
            scan((uintptr_t)ctx, 0x3000, "ctx");
            if (MemReadable((uintptr_t)ctx, 0x400)) for (int k = 0; k < 0x400 / 8; ++k) {
                uintptr_t q = *(uintptr_t*)((uintptr_t)ctx + k * 8);
                if (q > 0x10000 && q < 0x7fffffffffffULL && !(q & 7) && MemReadable(q, 0x800)) { char t[24]; snprintf(t, sizeof(t), "[%x]", k * 8); scan(q, 0x800, t); }
            }
            AddLogLine(std::string(idb) + "  [ClipStart] from FC64" + chain + hex + " ctx=" + std::to_string((uintptr_t)ctx) + " hash@" + where);
        }
    }
    if (!s_clipOn.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lk(s_clipMx);
    s_clips.push_front({ GetTickCount64(), id, (uintptr_t)ctx });
    if (s_clips.size() > 400) s_clips.pop_back();
    ++s_clipSerial;
}

void ClipMonitorInstall() {
    uintptr_t f = FindPattern("FC64.dll",
        "4C 89 4C 24 20 53 55 41 56 48 81 EC D0 00 00 00 48 8B E9 48 8B 49 28 4C 8B F2 48 85 C9 0F 84");
    if (!f) { LogL(LOG_WARN, "clip monitor: clip-start function not found"); return; }
    if (!CreateHook((void*)f, (void*)hk_ClipStart, (void**)&o_ClipStart)) { LogL(LOG_WARN, "clip monitor: hook failed"); o_ClipStart = nullptr; return; }
    LogL(LOG_DEBUG, "clip monitor: hooked clip start at FC64+0x%llx", (unsigned long long)(f - FC64Base()));
}
bool ClipMonitorAvailable() { return o_ClipStart != nullptr; }
bool ClipMonitorEnabled() { return s_clipOn.load(); }
void ClipMonitorSetEnabled(bool on) {
    if (on) {                                           // pick up labels from "Label animation clips"
        std::map<uint64_t, std::string> lab;
        FILE* f = _wfopen((g_rootDir + L"dumps\\animation_clips.txt").c_str(), L"rb");
        if (f) {
            char line[600];
            while (fgets(line, sizeof(line), f)) {
                std::string l = line;
                while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.pop_back();
                if (l.size() < 19 || l[0] == '#') continue;
                std::string t = l.substr(18);
                size_t b = t.find("   [");
                if (b != std::string::npos) t.resize(b);
                lab[_strtoui64(l.substr(0, 16).c_str(), nullptr, 16)] = t;
            }
            fclose(f);
        }
        std::lock_guard<std::mutex> lk(s_clipMx);
        s_clipLabels.swap(lab);
    }
    s_clipOn = on;
}
void ClipMonitorClear() { std::lock_guard<std::mutex> lk(s_clipMx); s_clips.clear(); ++s_clipSerial; }
uint64_t ClipMonitorSerial() { return s_clipSerial.load(); }
std::vector<ClipEvent> ClipMonitorEvents() { std::lock_guard<std::mutex> lk(s_clipMx); return std::vector<ClipEvent>(s_clips.begin(), s_clips.end()); }
std::string ClipLabelFor(uint64_t id) {
    std::lock_guard<std::mutex> lk(s_clipMx);
    auto it = s_clipLabels.find(id);
    return it == s_clipLabels.end() ? std::string() : it->second;
}

// Append " {label}" to "(no path)" lines of these ids (in the list and for future lines' sake
// in the recent buffer; the file on disk is left as it is).
void FileLogAnnotate(const std::map<uint64_t, std::string>& labels) {
    std::lock_guard<std::mutex> lk(s_logMx);
    bool any = false;
    for (auto& l : s_recent) {
        if (l.size() < 16 || l.find("(no path)") == std::string::npos || l.find(" {") != std::string::npos) continue;
        auto it = labels.find(_strtoui64(l.substr(0, 16).c_str(), nullptr, 16));
        if (it == labels.end() || it->second.empty()) continue;
        size_t p = l.find("(no path)") + 9;
        l.insert(p, " {" + it->second + "}");
        any = true;
    }
    // lines from earlier sessions are only in the file: add them to the list
    std::set<uint64_t> shown;
    for (auto& l : s_recent) if (l.size() >= 16) shown.insert(_strtoui64(l.substr(0, 16).c_str(), nullptr, 16));
    for (auto& kv : labels) {
        if (shown.count(kv.first)) continue;
        char b[40]; snprintf(b, sizeof(b), "%016llx", (unsigned long long)kv.first);
        s_recent.push_front(std::string(b) + "  [CAnimationResource] (no path) {" + kv.second + "}");
        any = true;
    }
    if (any) ++s_logSerial;
}

// [Debug] FileLogAtStartup=1 in fc4hook.ini: record from the moment the game starts (to catch
// what's loaded with the world, like animations and move data).
bool FileLogAtStartup() { return GetPrivateProfileIntW(L"Debug", L"FileLogAtStartup", 0, IniPath().c_str()) != 0; }
void FileLogSetAtStartup(bool on) { WritePrivateProfileStringW(L"Debug", L"FileLogAtStartup", on ? L"1" : L"0", IniPath().c_str()); }

bool FileLogAvailable() { return o_PathToId != nullptr || o_CreateRes != nullptr; }
bool FileLogEnabled() { return s_logOn.load(); }

void FileLogSetEnabled(bool on) {
    std::lock_guard<std::mutex> lk(s_logMx);
    if (on && !s_logFile) {
        CreateDirectoryW((g_rootDir + L"dumps").c_str(), nullptr);
        s_logFile = _wfopen((g_rootDir + L"dumps\\file_log.txt").c_str(), L"ab");
        if (s_logFile) {
            setvbuf(s_logFile, nullptr, _IOFBF, 1 << 16);
            fputs("# FC4Hook file log: <file id>  <path the game asked for>  or  <file id>  [ResourceType] <path or (no path)>   [<- loose mod]\n", s_logFile);
        }
    } else if (!on && s_logFile) {
        fclose(s_logFile); s_logFile = nullptr;
    }
    s_logOn = on;
    Log("file log %s%s", on ? "on: writing new paths to " : "off", on ? "fc4hook\\dumps\\file_log.txt" : "");
}

void FileLogFlush() { std::lock_guard<std::mutex> lk(s_logMx); if (s_logFile) fflush(s_logFile); }
void FileLogClear() { std::lock_guard<std::mutex> lk(s_logMx); s_recent.clear(); s_seen.clear(); s_resSeen.clear(); s_logCount = 0; ++s_logSerial; }
uint64_t FileLogCount() { return s_logCount.load(); }
uint64_t FileLogSerial() { return s_logSerial.load(); }
std::vector<std::string> FileLogRecent(const std::string& filter, size_t max) {
    std::lock_guard<std::mutex> lk(s_logMx);
    std::vector<std::string> r;
    std::string f = Lower(filter);
    for (auto it = s_recent.rbegin(); it != s_recent.rend() && r.size() < max; ++it)
        if (f.empty() || Lower(*it).find(f) != std::string::npos) r.push_back(*it);
    return r;
}

} // namespace fc4
