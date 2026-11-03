// Extract files from the game's own archives (data_win32\**\*.fat/.dat), and build quick
// "swap" test mods: replace file A with a copy of game file B.
//
// FAT2 v9 entries: id (CRC-64 of the path), offset, compressed size, uncompressed size,
// scheme (0 = stored, 1 = LZO1X). FC4Hook's own archives (fc4hook_loose / fc4hook_mods) are
// skipped, so you always get the game's original. Archives are searched patch_hd, patch, then
// the rest, like the game's mount priorities.
#include "core.h"
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <algorithm>
#include <functional>
#include <cstring>
#include <atomic>

namespace fc4 {

// ------------------------------------------------------------------ LZO1X (decompress_safe)
bool Lzo1xDecompress(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len, size_t* got) {
    const uint8_t* ip = in;
    const uint8_t* const ip_end = in + in_len;
    uint8_t* op = out;
    uint8_t* const op_end = out + out_len;
    const uint8_t* m_pos;
    size_t t = 0;
#define NEED_IP(x) if ((size_t)(ip_end - ip) < (size_t)(x)) return false
#define NEED_OP(x) if ((size_t)(op_end - op) < (size_t)(x)) return false
#define TEST_LB(m) if ((m) < out || (m) >= op) return false
    *got = 0;
    if (in_len < 3) return false;
    if (*ip > 17) {
        t = *ip++ - 17;
        if (t < 4) goto match_next;
        NEED_OP(t); NEED_IP(t + 3);
        do *op++ = *ip++; while (--t > 0);
        goto first_literal_run;
    }
    for (;;) {
        NEED_IP(3);
        t = *ip++;
        if (t >= 16) goto match;
        if (t == 0) {
            while (*ip == 0) { t += 255; ip++; NEED_IP(1); }
            t += 15 + *ip++;
        }
        NEED_OP(t + 3); NEED_IP(t + 6);
        t += 3;
        do *op++ = *ip++; while (--t > 0);
    first_literal_run:
        NEED_IP(1);
        t = *ip++;
        if (t >= 16) goto match;
        m_pos = op - (1 + 0x0800);
        m_pos -= t >> 2;
        NEED_IP(1);
        m_pos -= *ip++ << 2;
        TEST_LB(m_pos); NEED_OP(3);
        *op++ = *m_pos++; *op++ = *m_pos++; *op++ = *m_pos;
        goto match_done;
        for (;;) {
        match:
            if (t >= 64) {
                m_pos = op - 1;
                m_pos -= (t >> 2) & 7;
                NEED_IP(1);
                m_pos -= *ip++ << 3;
                t = (t >> 5) - 1;
                TEST_LB(m_pos); NEED_OP(t + 2);
                goto copy_match;
            } else if (t >= 32) {
                t &= 31;
                if (t == 0) {
                    NEED_IP(1);
                    while (*ip == 0) { t += 255; ip++; NEED_IP(1); }
                    t += 31 + *ip++;
                }
                NEED_IP(2);
                m_pos = op - 1;
                m_pos -= (ip[0] >> 2) + (ip[1] << 6);
                ip += 2;
            } else if (t >= 16) {
                m_pos = op;
                m_pos -= (t & 8) << 11;
                t &= 7;
                if (t == 0) {
                    NEED_IP(1);
                    while (*ip == 0) { t += 255; ip++; NEED_IP(1); }
                    t += 7 + *ip++;
                }
                NEED_IP(2);
                m_pos -= (ip[0] >> 2) + (ip[1] << 6);
                ip += 2;
                if (m_pos == op) goto eof_found;
                m_pos -= 0x4000;
            } else {
                m_pos = op - 1;
                m_pos -= t >> 2;
                NEED_IP(1);
                m_pos -= *ip++ << 2;
                TEST_LB(m_pos); NEED_OP(2);
                *op++ = *m_pos++; *op++ = *m_pos;
                goto match_done;
            }
            TEST_LB(m_pos); NEED_OP(t + 2);
        copy_match:
            *op++ = *m_pos++; *op++ = *m_pos++;
            do *op++ = *m_pos++; while (--t > 0);
        match_done:
            t = ip[-2] & 3;
            if (t == 0) break;
        match_next:
            NEED_OP(t); NEED_IP(t + 3);
            *op++ = *ip++;
            if (t > 1) { *op++ = *ip++; if (t > 2) *op++ = *ip++; }
            t = *ip++;
        }
    }
eof_found:
    *got = (size_t)(op - out);
    return ip == ip_end;
#undef NEED_IP
#undef NEED_OP
#undef TEST_LB
}

// ------------------------------------------------------------------ archive index
struct ArcEntry { int arc; uint64_t off; uint32_t csize, usize; uint8_t scheme; };
static std::mutex s_idxMx;
static std::vector<std::wstring> s_arcs;                    // .dat paths, in search order
static std::map<uint64_t, ArcEntry> s_index;
static bool s_indexed = false;

static std::wstring DataRoot() { return g_rootDir + L"..\\..\\data_win32\\"; }

static std::string LowerA(std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; }

static void FindFats(const std::wstring& dir, std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { FindFats(dir + n + L"\\", out); continue; }
        std::string l = LowerA(Narrow(n));
        if (l.size() > 4 && l.compare(l.size() - 4, 4, ".fat") == 0 && l.rfind("fc4hook_", 0) != 0) out.push_back(dir + n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int FatRank(const std::wstring& p) {
    std::string l = LowerA(Narrow(p.substr(p.find_last_of(L'\\') + 1)));
    if (l == "patch_hd.fat") return 0;
    if (l == "patch.fat") return 1;
    if (l.rfind("patch", 0) == 0) return 2;
    return 3;
}

static bool ReadWhole(const std::wstring& p, std::vector<uint8_t>& d) {
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return false;
    uint8_t b[65536]; size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) d.insert(d.end(), b, b + n);
    fclose(f);
    return true;
}

static void LoadFat(const std::wstring& fat, int arc) {
    std::vector<uint8_t> d;
    if (!ReadWhole(fat, d) || d.size() < 0x18) return;
    auto u32 = [&](size_t o) { return (uint32_t)d[o] | (uint32_t)d[o + 1] << 8 | (uint32_t)d[o + 2] << 16 | (uint32_t)d[o + 3] << 24; };
    if (u32(0) != 0x46415432 || u32(4) != 9) { LogL(LOG_DEBUG, "extract: %s is not a FAT2 v9 index, skipped", Narrow(fat).c_str()); return; }
    uint32_t n = u32(0x14);
    if (0x18 + (size_t)n * 20 > d.size()) return;
    auto add = [&](size_t o) {
        uint64_t id = (uint64_t)u32(o) << 32 | u32(o + 4);
        uint32_t c = u32(o + 8), dd = u32(o + 12), e = u32(o + 16);
        ArcEntry a;
        a.arc = arc; a.usize = c >> 2; a.scheme = (uint8_t)(c & 3);
        a.off = ((uint64_t)dd << 2) | (e >> 30); a.csize = e & 0x3fffffff;
        s_index.emplace(id, a);                  // first archive in search order wins
    };
    for (uint32_t i = 0; i < n; ++i) add(0x18 + (size_t)i * 20);
    // Sub-indexes ("subfats", FAT2 v9): after the main entries come two counted tables (localisation
    // and an unknown one, 16 bytes per item), then subfatCount (header +0x10) blocks of
    // u32 count + entries. Some files the game loads (e.g. the first-person arms) are only listed
    // there. Used only when the whole file parses exactly.
    uint32_t subCount = u32(0x10), subTotal = u32(0x0c);
    size_t p = 0x18 + (size_t)n * 20;
    if (subCount == 0 || subCount > 100000) return;
    for (int variant = 0; variant < 3; ++variant) {
        size_t q = p; bool ok = true;
        // variant 0: two counted tables; 1: one; 2: none
        for (int t = 0; t < 2 - variant && ok; ++t) { if (q + 4 > d.size()) { ok = false; break; } uint32_t c = u32(q); q += 4 + (size_t)c * 16; if (q > d.size()) ok = false; }
        std::vector<size_t> subs; size_t total = 0;
        for (uint32_t k = 0; k < subCount && ok; ++k) {
            if (q + 4 > d.size()) { ok = false; break; }
            uint32_t c = u32(q); q += 4;
            if (q + (size_t)c * 20 > d.size()) { ok = false; break; }
            for (uint32_t i = 0; i < c; ++i) subs.push_back(q + (size_t)i * 20);
            q += (size_t)c * 20; total += c;
        }
        if (!ok || q != d.size() || (subTotal && total != subTotal)) continue;
        for (size_t o : subs) add(o);
        LogL(LOG_DEBUG, "extract: %s: %u entries + %zu in %u sub-indexes", Narrow(fat).c_str(), n, total, subCount);
        return;
    }
    LogL(LOG_WARN, "extract: %s has %u sub-indexes in an unknown layout (not read)", Narrow(fat).c_str(), subCount);
}

static void EnsureIndex() {
    if (s_indexed) return;
    std::vector<std::wstring> fats;
    FindFats(DataRoot(), fats);
    std::stable_sort(fats.begin(), fats.end(), [](const std::wstring& a, const std::wstring& b) { return FatRank(a) < FatRank(b); });
    for (size_t i = 0; i < fats.size(); ++i) {
        std::wstring dat = fats[i].substr(0, fats[i].size() - 4) + L".dat";
        s_arcs.push_back(dat);
        LoadFat(fats[i], (int)i);
    }
    s_indexed = true;
    Log("extract: indexed %d game archives, %d files", (int)fats.size(), (int)s_index.size());
}

// Is `id` one of the game's own files? (indexes the archives the first time: a few seconds)
bool GameFileExists(uint64_t id) {
    std::lock_guard<std::mutex> lk(s_idxMx);
    EnsureIndex();
    return s_index.count(id) > 0;
}

// Read file `id` from the game's archives (decompressed).
bool GameFileRead(uint64_t id, std::vector<uint8_t>& out, std::string* err, std::string* from) {
    ArcEntry a;
    std::wstring dat;
    {
        std::lock_guard<std::mutex> lk(s_idxMx);
        EnsureIndex();
        auto it = s_index.find(id);
        if (it == s_index.end()) { *err = "not in the game's archives"; return false; }
        a = it->second; dat = s_arcs[a.arc];
    }
    if (from) *from = Narrow(dat.substr(DataRoot().size()));
    HANDLE h = CreateFileW(dat.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) { *err = "can't open " + Narrow(dat); return false; }
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)a.off;
    std::vector<uint8_t> raw(a.csize);
    DWORD r = 0;
    bool ok = SetFilePointerEx(h, li, nullptr, FILE_BEGIN) && (a.csize == 0 || (ReadFile(h, raw.data(), a.csize, &r, nullptr) && r == a.csize));
    CloseHandle(h);
    if (!ok) { *err = "read failed"; return false; }
    if (a.scheme == 0 || a.usize == 0) { out.swap(raw); return true; }
    if (a.scheme != 1) { *err = "compression scheme " + std::to_string(a.scheme) + " isn't supported"; return false; }
    out.resize(a.usize);
    size_t got = 0;
    if (!Lzo1xDecompress(raw.data(), raw.size(), out.data(), out.size(), &got) || got != a.usize) {
        *err = "LZO decompression failed"; return false;
    }
    return true;
}

static bool MakeDirs(const std::wstring& path) {           // creates every folder of a file path
    for (size_t i = path.find(L'\\', 3); i != std::wstring::npos; i = path.find(L'\\', i + 1))
        CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
    return true;
}

bool WriteFileAll(const std::wstring& p, const std::vector<uint8_t>& d) {
    MakeDirs(p);
    FILE* f = _wfopen(p.c_str(), L"wb");
    if (!f) return false;
    bool ok = d.empty() || fwrite(d.data(), 1, d.size(), f) == d.size();
    fclose(f);
    return ok;
}

// "graphics\foo.xbt" -> "foo.xbt"'s extension, for naming extracted/swap files
static std::string ExtOf(const std::string& path) {
    size_t s = path.find_last_of("\\/");
    std::string n = s == std::string::npos ? path : path.substr(s + 1);
    size_t d = n.find('.');
    return d == std::string::npos ? std::string(".bin") : n.substr(d);
}

static std::string HexId(uint64_t id) { char b[20]; snprintf(b, sizeof(b), "%016llx", (unsigned long long)id); return b; }

// Extract to dumps\extracted\<path> (or \__unknown\<id>.ext when there's no path).
bool ExtractGameFile(uint64_t id, const std::string& path, std::string* msg) {
    std::vector<uint8_t> d; std::string err, from;
    if (!GameFileRead(id, d, &err, &from)) { *msg = HexId(id) + ": " + err; return false; }
    std::string rel = path.empty() ? "__unknown\\" + HexId(id) + ".bin" : path;
    for (auto& c : rel) if (c == '/') c = '\\';
    while (!rel.empty() && (rel[0] == '\\' || rel[0] == '.')) rel.erase(0, 1);
    if (rel.find(':') != std::string::npos || rel.find("..") != std::string::npos) rel = "__unknown\\" + HexId(id) + ExtOf(rel);
    std::wstring out = g_rootDir + L"dumps\\extracted\\" + Widen(rel);
    if (!WriteFileAll(out, d)) { *msg = "can't write " + Narrow(out); return false; }
    *msg = "extracted " + rel + " (" + std::to_string(d.size()) + " bytes, from " + from + ") to fc4hook\\dumps\\extracted\\";
    std::string l = LowerA(rel);
    if (l.size() > 4 && l.compare(l.size() - 4, 4, ".xbt") == 0) {
        std::string tm; bool ok = TextureToDds(rel, &tm);
        *msg += ok ? ", plus " + tm : " (" + tm + ")";
    }
    return true;
}

// ------------------------------------------------------------------ models
// Game paths with one of `exts` written inside a file (printable strings; a length byte stuck to
// the front is dropped).
static std::vector<std::string> PathsIn(const std::vector<uint8_t>& d, const std::vector<std::string>& exts) {
    std::vector<std::string> r; std::set<std::string> seen; std::string cur;
    for (size_t i = 0; i <= d.size(); ++i) {
        char c = i < d.size() ? (char)d[i] : 0;
        if (c >= 0x20 && c < 0x7f) { cur += c; continue; }
        if (cur.size() >= 5 && (cur.find('\\') != std::string::npos || cur.find('/') != std::string::npos)) {
            for (size_t j = 0; j < 3 && j < cur.size(); ++j) if ((unsigned char)cur[j] == cur.size() - j) { cur = cur.substr(j + 1); break; }
            std::string l = LowerA(cur);
            size_t b = l.find("graphics");
            if (b == std::string::npos) b = l.find_first_not_of("{*)\"' ");
            if (b != std::string::npos) l = l.substr(b);
            for (auto& c2 : l) if (c2 == '/') c2 = '\\';
            for (auto& e : exts)
                if (l.size() > e.size() && l.compare(l.size() - e.size(), e.size(), e) == 0 && seen.insert(l).second) { r.push_back(l); break; }
        }
        cur.clear();
    }
    return r;
}

// Extract a model (.xbg) with everything it needs: its materials (.material.bin) and their
// textures (.xbt, + .dds copies to view/edit). Everything lands in dumps\extracted\<game path>.
bool ExtractModel(const std::string& path, std::string* msg) {
    std::string p = LowerA(path);
    for (auto& c : p) if (c == '/') c = '\\';
    std::string m1;
    if (!ExtractGameFile(FileId(p.c_str()), p, &m1)) { *msg = "Model: " + m1; return false; }
    std::vector<uint8_t> d; std::string err;
    GameFileRead(FileId(p.c_str()), d, &err, nullptr);
    int mats = 0, texs = 0, missing = 0;
    std::set<std::string> texDone;
    std::string list;
    for (auto& mat : PathsIn(d, { ".material.bin", ".mat", ".xbt" })) {
        bool isTex = mat.size() > 4 && mat.compare(mat.size() - 4, 4, ".xbt") == 0;
        std::string m;
        if (!ExtractGameFile(FileId(mat.c_str()), mat, &m)) { ++missing; list += "  not in the game: " + mat + "\n"; continue; }
        if (isTex) { if (texDone.insert(mat).second) ++texs; continue; }
        ++mats;
        std::vector<uint8_t> md;
        if (!GameFileRead(FileId(mat.c_str()), md, &err, nullptr)) continue;
        for (auto& t : PathsIn(md, { ".xbt" })) {
            if (!texDone.insert(t).second) continue;
            if (ExtractGameFile(FileId(t.c_str()), t, &m)) ++texs; else { ++missing; list += "  not in the game: " + t + "\n"; }
        }
    }
    *msg = "Extracted " + p + " with " + std::to_string(mats) + " material(s) and " + std::to_string(texs) +
           " texture(s) (+ .dds copies) to fc4hook\\dumps\\extracted\\" + (missing ? "\n" + std::to_string(missing) + " referenced file(s) not found:\n" + list : "");
    Log("models: %s", msg->c_str());
    return true;
}

bool ModelFromObj(const std::string& originalGamePath, const std::wstring& objFile, const std::wstring& outFile, std::string* msg) {
    std::string p = LowerA(originalGamePath);
    for (auto& c : p) if (c == '/') c = '\\';
    std::vector<uint8_t> xbg, obj; std::string err;
    if (!AnyFileRead(FileId(p.c_str()), xbg, &err, nullptr)) { *msg = "Couldn't read the original model " + p + ": " + err; return false; }
    if (!ReadWhole(objFile, obj) || obj.empty()) { *msg = "Couldn't read " + Narrow(objFile); return false; }
    std::string rep;
    if (!XbgFromObj(xbg, std::string(obj.begin(), obj.end()), &rep)) { *msg = "Not converted: " + rep; return false; }
    if (!WriteFileAll(outFile, xbg)) { *msg = "Couldn't write " + Narrow(outFile); return false; }
    *msg = "Converted: " + rep;
    Log("models: %s -> %s: %s", Narrow(objFile).c_str(), Narrow(outFile).c_str(), rep.c_str());
    return true;
}

struct GlbFile { std::string path; std::vector<uint8_t> data; };
struct GlbOptions { float scale = 1.f; float move[3] = { 0, 0, 0 }; };
bool XbgFromGlb(const std::vector<uint8_t>& tmpl, const std::vector<uint8_t>& glb, const std::string& dir, const std::string& stem,
                const std::function<bool(const std::string&, std::vector<uint8_t>&)>& readGame,
                std::vector<uint8_t>& out, std::vector<GlbFile>& files, std::string* report, const GlbOptions& opt);   // glbwrite.cpp

bool ModelFromGlb(const std::string& baseGamePath, const std::wstring& glbFile, const std::wstring& modFilesDir, const std::string& gameDir,
                  const std::string& stem, float scale, const float move[3], std::string* outGamePath, std::string* msg) {
    std::string p = LowerA(baseGamePath);
    for (auto& c : p) if (c == '/') c = '\\';
    std::vector<uint8_t> xbg, glb, out; std::string err;
    if (!AnyFileRead(FileId(p.c_str()), xbg, &err, nullptr)) { *msg = "Couldn't read the base model " + p + ": " + err; return false; }
    if (!ReadWhole(glbFile, glb) || glb.empty()) { *msg = "Couldn't read " + Narrow(glbFile); return false; }
    auto readGame = [](const std::string& path, std::vector<uint8_t>& d) {
        std::string l = LowerA(path); std::string e;
        return AnyFileRead(FileId(l.c_str()), d, &e, nullptr) && !d.empty();
    };
    std::vector<GlbFile> files; std::string rep;
    GlbOptions opt; opt.scale = scale; for (int k = 0; k < 3; ++k) opt.move[k] = move[k];
    if (!XbgFromGlb(xbg, glb, gameDir, stem, readGame, out, files, &rep, opt)) { *msg = "Not converted: " + rep; return false; }
    for (auto& f : files) {
        std::wstring o = modFilesDir + Widen(f.path);
        for (size_t k = 0; k < o.size(); ++k) if (o[k] == L'\\' && k > 2) CreateDirectoryW(o.substr(0, k).c_str(), nullptr);
        if (!WriteFileAll(o, f.data)) { *msg = "Couldn't write " + Narrow(o); return false; }
    }
    *outGamePath = gameDir + stem + ".xbg";
    *msg = "Built " + *outGamePath + ": " + rep;
    Log("models: %s -> %s", Narrow(glbFile).c_str(), msg->c_str());
    return true;
}

// ------------------------------------------------------------------ animation clips (aniclip.cpp)
bool AniClipToGlb(const std::vector<uint8_t>& clip, const std::vector<uint8_t>& skel, std::vector<uint8_t>& glb, std::string* report, uint64_t clipId);
bool AniGlbClipId(const std::vector<uint8_t>& glb, uint64_t* id);
bool AniClipFromGlb(const std::vector<uint8_t>& tmpl, const std::vector<uint8_t>& glb, const std::vector<uint8_t>& skel, std::vector<uint8_t>& out, std::string* report);
static const char* kArmsSkeleton = "graphics\\_common\\characters\\narrative\\firstpersonarms\\firstperson_rightarm.xbg";

static std::string IdHex(uint64_t id) { char b[20]; snprintf(b, sizeof(b), "%016llx", (unsigned long long)id); return b; }

// The game's own clip `id` -> fc4hook\dumps\animations\<id>.glb (armature + animation, for Blender).
bool ClipExportGlb(uint64_t id, std::wstring* outPath, std::string* msg) {
    std::vector<uint8_t> clip, skel, glb; std::string err, rep;
    if (!GameFileRead(id, clip, &err, nullptr)) { *msg = IdHex(id) + ": " + err; return false; }
    if (!AnyFileRead(FileId(kArmsSkeleton), skel, &err, nullptr)) { *msg = "couldn't read the first-person arms skeleton: " + err; return false; }
    if (!AniClipToGlb(clip, skel, glb, &rep, id)) { *msg = IdHex(id) + ": " + rep; return false; }
    std::wstring out = g_rootDir + L"dumps\\animations\\" + Widen(IdHex(id)) + L".glb";
    for (size_t k = 0; k < out.size(); ++k) if (out[k] == L'\\' && k > 2) CreateDirectoryW(out.substr(0, k).c_str(), nullptr);
    if (!WriteFileAll(out, glb)) { *msg = "can't write " + Narrow(out); return false; }
    if (outPath) *outPath = out;
    *msg = "Exported " + IdHex(id) + ": " + rep + " -> fc4hook\\dumps\\animations\\" + IdHex(id) + ".glb";
    Log("anim: %s", msg->c_str());
    return true;
}

// Your .glb -> a clip that replaces the game's clip `id`, written to `outFile` (a mod's files\__unknown\<id>.ani).
bool ClipFromGlb(uint64_t id, const std::wstring& glbFile, const std::wstring& outFile, std::string* msg) {
    std::vector<uint8_t> tmpl, skel, glb, out; std::string err, rep;
    if (!GameFileRead(id, tmpl, &err, nullptr)) { *msg = IdHex(id) + ": " + err; return false; }
    if (!AnyFileRead(FileId(kArmsSkeleton), skel, &err, nullptr)) { *msg = "couldn't read the first-person arms skeleton: " + err; return false; }
    if (!ReadWhole(glbFile, glb) || glb.empty()) { *msg = "couldn't read " + Narrow(glbFile); return false; }
    uint64_t made = 0;
    if (AniGlbClipId(glb, &made) && made != id) {
        *msg = "Not converted: this .glb was exported from clip " + IdHex(made) + ", not " + IdHex(id) +
               ". Use it on that clip's row, or use the \"finds its clip\" box at the top.";
        return false;
    }
    if (!AniClipFromGlb(tmpl, glb, skel, out, &rep)) { *msg = "Not converted: " + rep; return false; }
    for (size_t k = 0; k < outFile.size(); ++k) if (outFile[k] == L'\\' && k > 2) CreateDirectoryW(outFile.substr(0, k).c_str(), nullptr);
    if (!WriteFileAll(outFile, out)) { *msg = "can't write " + Narrow(outFile); return false; }
    *msg = "Converted: " + rep + ". Restart the game (or reload the save) to see it.";
    Log("anim: %s -> %s: %s", Narrow(glbFile).c_str(), Narrow(outFile).c_str(), rep.c_str());
    return true;
}

// Which clip a .glb was exported from (0 = unknown: exported before FC4Hook 1.8.2, or not by FC4Hook).
uint64_t ClipIdOfGlb(const std::wstring& glbFile) {
    std::vector<uint8_t> glb; uint64_t id = 0;
    if (ReadWhole(glbFile, glb)) AniGlbClipId(glb, &id);
    return id;
}

// Every model an archetype uses (its .xbg paths), e.g. a weapon's first/third-person models.
std::vector<std::string> ArchetypeModels(uint64_t id) {
    std::vector<uint8_t> d; std::string err;
    std::string path = "entityarchetypeslibrary\\" + std::to_string(id) + ".ark.fcb";
    if (!AnyFileRead(FileId(path.c_str()), d, &err, nullptr)) return {};
    return PathsIn(d, { ".xbg" });
}

static bool ReadFileAll(const std::wstring& p, std::vector<uint8_t>& d) { d.clear(); return ReadWhole(p, d) && !d.empty(); }

// A texture was extracted to dumps\extracted\<rel>: make sure its other half is there too
// (foo_d.xbt <-> foo_d_mip0.xbt) and write foo_d.dds with the whole image.
bool TextureToDds(const std::string& rel, std::string* msg) {
    std::wstring root = g_rootDir + L"dumps\\extracted\\";
    std::string base = rel;
    IsMip0Name(rel, &base);
    std::string mip0 = Mip0Name(base);
    for (const std::string* p : { &base, &mip0 }) {
        if (*p == rel || GetFileAttributesW((root + Widen(*p)).c_str()) != INVALID_FILE_ATTRIBUTES) continue;
        std::vector<uint8_t> d; std::string err, from;
        if (GameFileRead(FileId(p->c_str()), d, &err, &from)) WriteFileAll(root + Widen(*p), d);
    }
    std::vector<uint8_t> xd, xm, dds;
    if (!ReadFileAll(root + Widen(base), xd)) { *msg = "couldn't find " + base + " for the .dds"; return false; }
    bool hasMip0 = ReadFileAll(root + Widen(mip0), xm);
    std::string err;
    if (!XbtToDds(xd, hasMip0 ? &xm : nullptr, dds, &err)) { *msg = "no .dds: " + err; return false; }
    std::string ddsRel = base.substr(0, base.size() - 4) + ".dds";
    std::vector<uint8_t> existing;
    if (ReadFileAll(root + Widen(ddsRel), existing) && existing != dds) {      // never overwrite your edits
        *msg = ddsRel.substr(ddsRel.find_last_of('\\') + 1) + " already edited, kept it";
        return true;
    }
    if (!WriteFileAll(root + Widen(ddsRel), dds)) { *msg = "can't write " + ddsRel; return false; }
    *msg = ddsRel.substr(ddsRel.find_last_of('\\') + 1) + " to edit";
    return true;
}

static void FindExt(const std::wstring& dir, const std::string& rel, const char* ext, std::vector<std::string>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::string r = rel + Narrow(n);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { FindExt(dir + n + L"\\", r + "\\", ext, out); continue; }
        std::string l = LowerA(r);
        size_t e = strlen(ext);
        if (l.size() > e && l.compare(l.size() - e, e, ext) == 0) out.push_back(r);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Every .dds in dumps\extracted that differs from the game's original is packed back into
// .xbt files and written to mods\MyTextures\files\<same path>. Returns how many.
int InstallEditedTextures(std::string* msg) {
    std::wstring root = g_rootDir + L"dumps\\extracted\\";
    std::wstring mod = g_rootDir + L"mods\\MyTextures\\files\\";
    std::vector<std::string> list;
    FindExt(root, "", ".dds", list);
    int done = 0, same = 0;
    std::string errs;
    for (auto& r : list) {
        std::string base = r.substr(0, r.size() - 4) + ".xbt", mip0 = Mip0Name(base);
        std::vector<uint8_t> dds, xd, xm, orig, od, om;
        std::string err;
        if (!ReadFileAll(root + Widen(r), dds) || !ReadFileAll(root + Widen(base), xd)) { errs += "\n" + r + ": the original .xbt isn't next to it (extract it first)"; continue; }
        bool hasMip0 = ReadFileAll(root + Widen(mip0), xm);
        if (XbtToDds(xd, hasMip0 ? &xm : nullptr, orig, &err) && orig == dds) { ++same; continue; }   // not edited
        if (!DdsToXbt(dds, xd, hasMip0 ? &xm : nullptr, od, om, &err)) { errs += "\n" + r + ": " + err; continue; }
        if (!WriteFileAll(mod + Widen(base), od) || (hasMip0 && !WriteFileAll(mod + Widen(mip0), om))) { errs += "\n" + r + ": can't write into mods\\MyTextures"; continue; }
        ++done;
        Log("textures: installed %s into mods\\MyTextures", base.c_str());
    }
    *msg = std::to_string(done) + " edited texture(s) installed into mods\\MyTextures (" + std::to_string(same) + " unchanged skipped)";
    if (done) *msg += ". Restart the game to see them.";
    if (!errs.empty()) { *msg += "\nProblems:" + errs; LogL(LOG_WARN, "textures: %s", msg->c_str()); }
    if (list.empty()) *msg = "No .dds files in dumps\\extracted yet: extract a texture (.xbt) first, edit its .dds, save it, then press this.";
    return done;
}

// Test mod mods\FC4Hook_SwapTest: the game gets game file `withId` whenever it asks for
// `replaceId`. Stored under __UNKNOWN\<replaceId>, so the id matches exactly.
bool MakeSwap(uint64_t replaceId, const std::string& replacePath, uint64_t withId, const std::string& withPath, std::string* msg) {
    std::vector<uint8_t> d; std::string err, from;
    if (!GameFileRead(withId, d, &err, &from)) { *msg = "replacement " + HexId(withId) + ": " + err; return false; }
    std::wstring dir = g_rootDir + L"mods\\FC4Hook_SwapTest\\";
    std::wstring out = dir + L"files\\__UNKNOWN\\swap\\" + Widen(HexId(replaceId) + ExtOf(replacePath));
    if (!WriteFileAll(out, d)) { *msg = "can't write " + Narrow(out); return false; }
    FILE* f = _wfopen((dir + L"swaps.txt").c_str(), L"ab");
    if (f) {
        fprintf(f, "%s %s  <-  %s %s\r\n", HexId(replaceId).c_str(), replacePath.c_str(), HexId(withId).c_str(), withPath.c_str());
        fclose(f);
    }
    *msg = "swap mod written: " + (replacePath.empty() ? HexId(replaceId) : replacePath) + " now loads " +
           (withPath.empty() ? HexId(withId) : withPath) + ". Restart the game to see it (mods\\FC4Hook_SwapTest; untick it in the Mods tab to undo).";
    Log("swap: %s", msg->c_str());
    return true;
}

// ------------------------------------------------------------------ animation clip labels
// CAnimationResource files are Ubisoft "aNi" clips: +0x18 "aNi", +0x20 float length in seconds.
// Many name what they drive (weapon / bone track names), e.g. "sg551", "RightHand", "IK_LeftHand".
// LabelAnimationClips reads every clip in the file log out of the game's archives and tags it.
static uint32_t Crc32(const uint8_t* p, size_t n) {
    static uint32_t t[256];
    if (!t[1]) for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = t[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// Names inside an "aNi" clip are stored as  <crc32(name)> <name> <NUL>  (track / bone / weapon
// names). Only strings whose 4 preceding bytes are their own CRC-32 are taken, so binary noise
// never shows up as a name.
static std::string ClipLabel(const std::vector<uint8_t>& d, const std::set<std::string>& known) {
    char head[48] = "";
    if (d.size() >= 0x24 && d[0x18] == 'a' && d[0x19] == 'N' && d[0x1a] == 'i') {
        float len; memcpy(&len, &d[0x20], 4);
        if (len >= 0 && len < 3600) snprintf(head, sizeof(head), "%.2fs", len);
    }
    std::vector<std::string> names;
    std::set<std::string> seen;
    for (size_t z = 5; z < d.size() && names.size() < 12; ++z) {
        if (d[z] != 0 || !(isalnum(d[z - 1]) || d[z - 1] == '_')) continue;
        size_t b = z;
        while (b > 4 && z - b < 96 && (isalnum(d[b - 1]) || d[b - 1] == '_')) --b;
        for (size_t s0 = b; s0 + 1 < z; ++s0) {                 // longest name whose CRC matches
            if (s0 < 4) continue;
            uint32_t h; memcpy(&h, &d[s0 - 4], 4);
            if (h == Crc32(&d[s0], z - s0)) {
                std::string n((const char*)&d[s0], z - s0);
                if (seen.insert(n).second) names.push_back(n);
                break;
            }
        }
    }
    // weapons first (known weapon folders, or the clip names one), then the rest
    std::string out = head, rest;
    for (auto& n : names) {
        std::string l = LowerA(n);
        bool weapon = known.count(l) > 0;
        if (weapon) out += " " + n; else rest += " " + n;
    }
    return out + rest;
}

// ------------------------------------------------------------------ clip names
// A clip's file id is the CRC-64 of its .mab path, but the game ships only the ids. Find
// names: (1) read every archive file and hash each ".mab" path written inside one; (2) guess:
// every animations folder seen x every clip file name seen, also with the weapon name
// swapped for every other weapon. Hits go to dumps\clip_names.txt (id<TAB>path).
static std::map<uint64_t, std::string> LoadClipNames() {
    std::map<uint64_t, std::string> r;
    FILE* f = _wfopen((g_rootDir + L"dumps\\clip_names.txt").c_str(), L"rb");
    if (!f) return r;
    char line[700];
    while (fgets(line, sizeof(line), f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.pop_back();
        size_t t = l.find('\t');
        if (l.empty() || l[0] == '#' || t != 16) continue;
        r[_strtoui64(l.substr(0, 16).c_str(), nullptr, 16)] = l.substr(17);
    }
    fclose(f);
    return r;
}
static std::string ClipStem(const std::string& path) {
    size_t s = path.find_last_of('\\'); std::string n = s == std::string::npos ? path : path.substr(s + 1);
    if (n.size() > 4) n.resize(n.size() - 4);
    return n;
}

static uint64_t s_crcT[256];
static uint64_t CrcMore(uint64_t h, const std::string& s) {
    if (!s_crcT[1]) for (int i = 0; i < 256; ++i) { uint64_t c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0xD800000000000000ull : c >> 1; s_crcT[i] = c; }
    for (unsigned char ch : s) h = s_crcT[(h ^ ch) & 0xff] ^ (h >> 8);
    return h;
}

int FindClipNames(std::string* msg) {
    std::vector<std::pair<uint64_t, ArcEntry>> all; std::set<uint64_t> ids;
    {
        std::lock_guard<std::mutex> lk(s_idxMx);
        EnsureIndex();
        for (auto& kv : s_index) { ids.insert(kv.first); if (kv.second.usize >= 16 && kv.second.usize <= (24u << 20)) all.push_back(kv); }
    }
    std::sort(all.begin(), all.end(), [](const std::pair<uint64_t, ArcEntry>& a, const std::pair<uint64_t, ArcEntry>& b) {
        return a.second.arc != b.second.arc ? a.second.arc < b.second.arc : a.second.off < b.second.off; });
    std::set<std::string> mabs, dirs;
    for (size_t k = 0; k < all.size(); ++k) {
        if (k % 500 == 0) SetState("filelog.status", "finding clip names: reading game files " + std::to_string(k) + " / " + std::to_string(all.size()) + "...");
        std::vector<uint8_t> d; std::string err;
        if (!GameFileRead(all[k].first, d, &err, nullptr)) continue;
        if (d.size() >= 4 && (memcmp(d.data(), "aNi", 3) == 0 || memcmp(d.data(), "TEX", 3) == 0)) continue;
        std::string cur;
        for (size_t i = 0; i <= d.size(); ++i) {
            char c = i < d.size() ? (char)d[i] : 0;
            if (c >= 0x20 && c < 0x7f) { cur += c; continue; }
            if (cur.size() >= 8) {
                std::string l = LowerA(cur);
                for (auto& ch : l) if (ch == '/') ch = '\\';
                size_t g = l.find("graphics\\");
                if (g != std::string::npos) l = l.substr(g);
                size_t m = l.find(".mab");
                if (m != std::string::npos) mabs.insert(l.substr(0, m + 4));
                if (l.find("animations\\") != std::string::npos && g != std::string::npos) {
                    std::string p = l; size_t e = p.find_last_of('\\');
                    if (p.find('.') != std::string::npos) p = p.substr(0, e);
                    while (p.size() > 9) { dirs.insert(p); size_t q = p.find_last_of('\\'); if (q == std::string::npos) break; p = p.substr(0, q); }
                }
            }
            cur.clear();
        }
    }
    std::map<uint64_t, std::string> found = LoadClipNames();
    size_t before = found.size();
    // names in the log too
    {
        FILE* f = _wfopen((g_rootDir + L"dumps\\file_log.txt").c_str(), L"rb");
        char line[700];
        while (f && fgets(line, sizeof(line), f)) {
            std::string l = LowerA(line);
            size_t m = l.find(".mab"), g = l.find("graphics\\");
            if (m != std::string::npos && g != std::string::npos && g < m) mabs.insert(l.substr(g, m + 4 - g));
        }
        if (f) fclose(f);
    }
    std::set<std::string> names;
    for (auto& p : mabs) {
        uint64_t id = FileId(p.c_str());
        if (ids.count(id)) found[id] = p;
        size_t s = p.find_last_of('\\');
        if (s != std::string::npos) { names.insert(p.substr(s + 1)); dirs.insert(p.substr(0, s)); }
    }
    // weapon swaps
    std::vector<std::string> weapons = { "ak47","ak74m","akm","mg42","mkg","kimber","p416","bz19","svd","m249","rpg7","gl94","gl_79",
        "machete","throwing_knife","molotov","grenade","c4","bow","recurve","compound","crossbow","m700","sa_50","dsr1","z93","sa50",
        "a2000","agram2000","vector","mp34","mp5","p226","m1911","1911","d50","desert_eagle","6p9","skorpion","ump45","usp","shredder",
        "u100","mts255","m133","spas12","spas","1887","aa12","m60","pkm","pkp","mg42","hvy","stg90","mx4","ms16","ms_16","f2000","acr",
        "m16","ak","pistol","shotgun","sniper","smg","lmg","rifle","launcher","flamethrower","buzzsaw","harpoon","rpg","lmg","bow","ar" };
    std::set<std::string> extra;
    for (auto& n : names) for (auto& w : weapons) {
        size_t p = n.find(w);
        if (p == std::string::npos) continue;
        for (auto& w2 : weapons) if (w2 != w) extra.insert(n.substr(0, p) + w2 + n.substr(p + w.size()));
    }
    names.insert(extra.begin(), extra.end());
    std::vector<std::string> nv(names.begin(), names.end());
    size_t di = 0;
    for (auto& dname : dirs) {
        if (++di % 50 == 0) SetState("filelog.status", "finding clip names: guessing in folder " + std::to_string(di) + " / " + std::to_string(dirs.size()) + " (" + std::to_string(found.size()) + " named)...");
        uint64_t h = CrcMore(0, dname + "\\");
        for (auto& n : nv) {
            uint64_t id = CrcMore(h, n);
            if (ids.count(id) && !found.count(id)) found[id] = dname + "\\" + n;
        }
    }
    FILE* out = _wfopen((g_rootDir + L"dumps\\clip_names.txt").c_str(), L"wb");
    if (out) {
        fputs("# FC4Hook clip names: <file id><TAB><.mab path>  (made by File log > Find clip names)\r\n", out);
        for (auto& kv : found) fprintf(out, "%s\t%s\r\n", HexId(kv.first).c_str(), kv.second.c_str());
        fclose(out);
    }
    *msg = "Found names for " + std::to_string(found.size()) + " clips (" + std::to_string(found.size() - before) + " new; " +
           std::to_string(mabs.size()) + " .mab paths and " + std::to_string(dirs.size()) + " animation folders seen). "
           "Saved to dumps\\clip_names.txt; press Label animation clips to show them.";
    Log("%s", msg->c_str());
    return (int)found.size();
}

// Which game files mention the clips whose name contains `filter` (by their 8-byte id)?
// Writes dumps\clip_refs.txt and copies each such file to dumps\extracted\refs\.
int FindClipReferences(const std::string& filterIn, std::string* msg) {
    std::string filter = LowerA(filterIn);
    std::map<uint64_t, std::string> names = LoadClipNames();
    std::map<uint64_t, std::string> want;
    for (auto& kv : names) if (!filter.empty() && LowerA(kv.second).find(filter) != std::string::npos) want[kv.first] = ClipStem(kv.second);
    if (want.empty()) { *msg = "No named clips contain \"" + filterIn + "\" (run Find clip names first)."; return 0; }
    std::vector<std::pair<uint64_t, ArcEntry>> all;
    {
        std::lock_guard<std::mutex> lk(s_idxMx);
        EnsureIndex();
        for (auto& kv : s_index) if (kv.second.usize >= 16 && kv.second.usize <= (64u << 20)) all.push_back(kv);
    }
    std::sort(all.begin(), all.end(), [](const std::pair<uint64_t, ArcEntry>& a, const std::pair<uint64_t, ArcEntry>& b) {
        return a.second.arc != b.second.arc ? a.second.arc < b.second.arc : a.second.off < b.second.off; });
    struct Hit { size_t size; int n; std::string head, sample; };
    std::map<uint64_t, Hit> hits;
    for (size_t k = 0; k < all.size(); ++k) {
        if (k % 500 == 0) SetState("filelog.status", "finding files that use \"" + filterIn + "\" clips: " + std::to_string(k) + " / " +
                                   std::to_string(all.size()) + " (" + std::to_string(hits.size()) + " found)...");
        if (want.count(all[k].first)) continue;
        std::vector<uint8_t> d; std::string err;
        if (!GameFileRead(all[k].first, d, &err, nullptr)) continue;
        int n = 0; std::string sample;
        for (size_t i = 0; i + 8 <= d.size(); ++i) {
            uint64_t v; memcpy(&v, &d[i], 8);
            auto it = want.find(v);
            if (it != want.end()) { if (++n <= 3) sample += it->second + " "; i += 7; }
        }
        if (!n) continue;
        Hit h; h.size = d.size(); h.n = n; h.sample = sample;
        for (size_t i = 0; i < 8 && i < d.size(); ++i) h.head += (d[i] >= 0x20 && d[i] < 0x7f) ? (char)d[i] : '.';
        hits[all[k].first] = h;
        WriteFileAll(g_rootDir + L"dumps\\extracted\\refs\\" + Widen(HexId(all[k].first)) + L".bin", d);
    }
    FILE* out = _wfopen((g_rootDir + L"dumps\\clip_refs.txt").c_str(), L"wb");
    if (out) {
        fprintf(out, "# files mentioning %zu clips matching \"%s\": <file id>  <hits>  <size>  <first bytes>  <examples>\r\n", want.size(), filterIn.c_str());
        for (auto& kv : hits) fprintf(out, "%s  %d  %zu  %s  %s\r\n", HexId(kv.first).c_str(), kv.second.n, kv.second.size, kv.second.head.c_str(), kv.second.sample.c_str());
        fclose(out);
    }
    *msg = std::to_string(hits.size()) + " game files mention the " + std::to_string(want.size()) + " \"" + filterIn +
           "\" clips. List in dumps\\clip_refs.txt, the files in dumps\\extracted\\refs\\.";
    Log("%s", msg->c_str());
    return (int)hits.size();
}

int LabelAnimationClips(std::string* msg) {
    std::map<uint64_t, std::string> clipNames = LoadClipNames();
    std::wstring logp = g_rootDir + L"dumps\\file_log.txt";
    FileLogFlush();
    FILE* f = _wfopen(logp.c_str(), L"rb");
    if (!f) { *msg = "No dumps\\file_log.txt yet: record first (File log > Record file paths)."; return 0; }
    std::vector<uint64_t> ids; std::set<uint64_t> idset;
    std::set<std::string> known;
    char line[600];
    while (fgets(line, sizeof(line), f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.pop_back();
        uint64_t id; std::string path, type;
        if (l.empty() || l[0] == '#' || !ParseLogLine(l, &id, &path, &type)) continue;
        if (type == "CAnimationResource") { if (idset.insert(id).second) ids.push_back(id); continue; }
        std::string lp = LowerA(path);
        size_t w = lp.find("weapons\\");
        if (w != std::string::npos) { size_t e = lp.find('\\', w + 8); if (e != std::string::npos && e > w + 8) known.insert(lp.substr(w + 8, e - w - 8)); }
    }
    fclose(f);
    known.erase("_shared"); known.erase("ammo");
    if (ids.empty()) { *msg = "No [CAnimationResource] lines in the file log yet: tick Record from game start, restart, load your save."; return 0; }
    std::map<uint64_t, std::string> labels;
    FILE* out = _wfopen((g_rootDir + L"dumps\\animation_clips.txt").c_str(), L"wb");
    if (out) fputs("# FC4Hook animation clips: <file id>  <length>  <names found inside (weapon, bones)>   [size]\r\n", out);
    int n = 0, missing = 0;
    for (size_t k = 0; k < ids.size(); ++k) {
        if (k % 200 == 0) SetState("filelog.status", "labelling clips " + std::to_string(k) + " / " + std::to_string(ids.size()) + "...");
        std::vector<uint8_t> d; std::string err, from;
        if (!GameFileRead(ids[k], d, &err, &from)) { ++missing; continue; }
        std::string lab = ClipLabel(d, known);
        auto cn = clipNames.find(ids[k]);
        if (cn != clipNames.end()) lab = ClipStem(cn->second) + "  " + lab;
        labels[ids[k]] = lab;
        if (out) fprintf(out, "%s  %s   [%zu bytes]\r\n", HexId(ids[k]).c_str(), lab.c_str(), d.size());
        ++n;
    }
    if (out) fclose(out);
    FileLogAnnotate(labels);
    *msg = "Labelled " + std::to_string(n) + " animation clips (" + std::to_string(missing) + " not in the archives). "
           "Filter the list by a weapon name (e.g. ak74m) or a length (e.g. 2.3). Also in dumps\\animation_clips.txt.";
    Log("%s", msg->c_str());
    return n;
}

// Run on a worker thread (indexing all archives the first time takes a few seconds).
// ------------------------------------------------------------------ archetype catalog
// Archetype files (entityarchetypeslibrary\<id>.ark.fcb) are "nbCF" binaries that start with
// their own path, then their library name ("weapons:fcx/ak47", "npc:...", "vehicles:..."). The
// archives only store hashes, so to list them every small file is checked: the first bytes of
// each entry are read (LZO keeps that text as a literal run) and only real archetypes are
// decompressed. Result: fc4hook\archetypes.txt, "id<TAB>library:name" per line.
static std::atomic<int> s_catDone{ 0 }, s_catTotal{ 0 }, s_catFound{ 0 };
static std::atomic<bool> s_catBusy{ false };

static bool FindBytes(const uint8_t* d, size_t n, const char* s) {
    size_t k = strlen(s);
    for (size_t i = 0; i + k <= n; ++i) if (memcmp(d + i, s, k) == 0) return true;
    return false;
}

// "library:name" from a decompressed archetype (the first printable string with a ':' after the path)
static std::string ArchetypeName(const std::vector<uint8_t>& d, uint64_t* idOut) {
    const char* tag = "entityarchetypeslibrary\\";
    size_t n = d.size() < 4096 ? d.size() : 4096, i = 0, k = strlen(tag);
    for (; i + k <= n; ++i) if (memcmp(&d[i], tag, k) == 0) break;
    if (i + k > n) return "";
    *idOut = strtoull((const char*)&d[i + k], nullptr, 10);
    std::string cur, best;
    for (size_t j = i; j < n; ++j) {
        char c = (char)d[j];
        if (c >= 0x20 && c < 0x7f) { cur += c; continue; }
        if (cur.size() >= 3 && cur.find(':') != std::string::npos && cur.find('\\') == std::string::npos) {
            // FCB strings are length-prefixed; a printable length byte ('n' type tag + length)
            // would stick to the front: drop it when it matches the string's length.
            for (size_t k = 0; k < 3 && k < cur.size(); ++k)
                if ((unsigned char)cur[k] == cur.size() - k) { cur = cur.substr(k + 1); break; }
            best = cur; break;
        }
        cur.clear();
    }
    return best;
}

std::string ArchetypeCatalogStatus() {
    char b[160];
    if (s_catBusy) snprintf(b, sizeof(b), "scanning %d / %d files, %d archetypes found", s_catDone.load(), s_catTotal.load(), s_catFound.load());
    else if (s_catTotal) snprintf(b, sizeof(b), "done: %d archetypes", s_catFound.load());
    else b[0] = 0;
    return b;
}

void ArchetypeCatalogBuild() {
    if (s_catBusy.exchange(true)) return;
    std::thread([] {
        std::vector<std::pair<uint64_t, ArcEntry>> cand;
        std::vector<std::wstring> arcs;
        {
            std::lock_guard<std::mutex> lk(s_idxMx);
            EnsureIndex();
            arcs = s_arcs;
            for (auto& kv : s_index) if (kv.second.usize >= 64 && kv.second.usize <= (4u << 20)) cand.push_back(kv);
        }
        std::sort(cand.begin(), cand.end(), [](const std::pair<uint64_t, ArcEntry>& a, const std::pair<uint64_t, ArcEntry>& b) {
            return a.second.arc != b.second.arc ? a.second.arc < b.second.arc : a.second.off < b.second.off; });
        s_catTotal = (int)cand.size(); s_catDone = 0; s_catFound = 0;
        std::map<uint64_t, std::string> found;
        int curArc = -1; HANDLE h = INVALID_HANDLE_VALUE;
        for (auto& c : cand) {
            ++s_catDone;
            const ArcEntry& a = c.second;
            if (a.arc != curArc) {
                if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
                curArc = a.arc;
                h = CreateFileW(arcs[a.arc].c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
            }
            if (h == INVALID_HANDLE_VALUE) continue;
            uint8_t head[96]; DWORD r = 0;
            DWORD want = a.csize < sizeof(head) ? a.csize : (DWORD)sizeof(head);
            LARGE_INTEGER li; li.QuadPart = (LONGLONG)a.off;
            if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN) || !ReadFile(h, head, want, &r, nullptr) || r != want) continue;
            if (!FindBytes(head, r, "nbCF") || !FindBytes(head, r, "entityarch")) continue;
            std::vector<uint8_t> d; std::string err;
            if (!GameFileRead(c.first, d, &err, nullptr)) continue;
            uint64_t id = 0;
            std::string name = ArchetypeName(d, &id);
            if (!id || name.empty()) continue;
            found[id] = name;
            ++s_catFound;
        }
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        std::string out = "# FC4Hook archetype catalog (made by Spawner > Build catalog). id<TAB>library:name\r\n";
        for (auto& kv : found) out += std::to_string(kv.first) + "\t" + kv.second + "\r\n";
        std::vector<uint8_t> bytes(out.begin(), out.end());
        WriteFileAll(g_rootDir + L"archetypes.txt", bytes);
        Log("extract: archetype catalog: %d archetypes", (int)found.size());
        s_catBusy = false;
    }).detach();
}

// Value of an archetype reference field (8-byte id) inside archetype `id`, found by the CRC32
// of the field's name, e.g. "arkPickupArchetype". "#xxxxxxxx" = a raw field hash. 0 = not there.
uint64_t ArchetypeRefField(uint64_t id, const char* field) {
    std::string path = "entityarchetypeslibrary\\" + std::to_string(id) + ".ark.fcb";
    std::vector<uint8_t> d; std::string err;
    if (!AnyFileRead(FileId(path.c_str()), d, &err, nullptr)) return 0;
    uint32_t hsh = field[0] == '#' ? (uint32_t)strtoul(field + 1, nullptr, 16) : Crc32((const uint8_t*)field, strlen(field));
    uint8_t pat[5] = { (uint8_t)hsh, (uint8_t)(hsh >> 8), (uint8_t)(hsh >> 16), (uint8_t)(hsh >> 24), 8 };
    for (size_t i = 0; i + 13 <= d.size(); ++i)
        if (memcmp(&d[i], pat, 5) == 0) { uint64_t v; memcpy(&v, &d[i + 5], 8); return v; }
    return 0;
}

// ------------------------------------------------------------------ archetype check
// Why won't my archetype spawn? Looks at the file the game would load for `id` and reports,
// in plain words: where it comes from, whether it's a real binary .fcb (not XML), whether the
// id written inside it matches the file name, and whether every model/texture/material/
// archetype it points to exists (in a mod or in the game).
static bool FileKnown(uint64_t id, std::string* where) {
    std::vector<VfsFileEntry> files = VfsFiles();
    for (auto& f : files) if (f.id == id) { *where = "mod " + f.mod; return true; }
    if (GameFileExists(id)) { *where = "game"; return true; }
    return false;
}

static bool EndsWith(const std::string& s, const char* e) { size_t n = strlen(e); return s.size() >= n && s.compare(s.size() - n, n, e) == 0; }

std::string ArchetypeCheck(uint64_t id) {
    std::string path = "entityarchetypeslibrary\\" + std::to_string(id) + ".ark.fcb";
    std::string r = "Archetype " + std::to_string(id) + "\n  file: " + path + "\n";
    std::vector<uint8_t> d; std::string err, from;
    if (!AnyFileRead(FileId(path.c_str()), d, &err, &from)) {
        r += "  PROBLEM: no such file in any enabled mod or in the game.\n"
             "    Put it at  fc4hook\\mods\\<YourMod>\\files\\" + path + "\n"
             "    (exact name, .ark.fcb at the end), make sure the mod is ticked in F8 > Mods, and\n"
             "    restart the game: new loose files are only picked up at start.\n";
        return r;
    }
    r += "  from: " + from + "   (" + std::to_string(d.size()) + " bytes)\n";
    int problems = 0;
    if (d.size() >= 1 && (d[0] == '<' || (d.size() >= 4 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF))) {
        r += "  PROBLEM: this is an XML text file. The game only reads the binary .fcb format.\n"
             "    Convert the edited .xml back to .ark.fcb with the Gibbed FC4 FCB converter\n"
             "    (the same tool that made the .xml) and ship the converted binary file.\n";
        return r;
    }
    if (d.size() < 8 || memcmp(d.data(), "nbCF", 4) != 0) {
        r += "  PROBLEM: not a binary FCB file (it should start with \"nbCF\"). The conversion back to\n"
             "    .fcb probably failed, or the file is something else renamed to .ark.fcb.\n";
        return r;
    }
    r += "  format: binary FCB, ok\n";
    // the id written inside the file
    const char* tag = "entityarchetypeslibrary\\";
    size_t k = strlen(tag), at = std::string::npos;
    for (size_t i = 0; i + k < d.size() && i < 8192; ++i) if (memcmp(&d[i], tag, k) == 0) { at = i; break; }
    if (at == std::string::npos) r += "  note: the file doesn't name its own path (unusual, may still work)\n";
    else {
        uint64_t inner = strtoull((const char*)&d[at + k], nullptr, 10);
        if (inner != id) {
            r += "  PROBLEM: the path inside the file says " + std::to_string(inner) + ", but the file is named " + std::to_string(id) + ".\n"
                 "    In the .xml, change every " + std::to_string(inner) + " that belongs to this archetype (its own\n"
                 "    entityarchetypeslibrary path and id) to " + std::to_string(id) + ", convert back to .fcb.\n";
            ++problems;
        } else r += "  id inside the file: matches\n";
    }
    // the library name, for reference
    { uint64_t x = 0; std::string nm = ArchetypeName(d, &x); if (!nm.empty()) r += "  name: " + nm + "\n"; }
    // referenced files: printable strings that look like game paths
    static const char* exts[] = { ".xbg", ".xbt", ".material.bin", ".mab", ".fcb", ".bin", ".spk", ".sbao", ".xml", ".lua", ".bnk", ".hkx", ".mmb", ".bfx", ".feu", ".part" };
    std::set<std::string> refs;
    std::string cur;
    for (size_t i = 0; i <= d.size(); ++i) {
        char c = i < d.size() ? (char)d[i] : 0;
        if (c >= 0x20 && c < 0x7f) { cur += c; continue; }
        if (cur.size() >= 5 && (cur.find('\\') != std::string::npos || cur.find('/') != std::string::npos)) {
            // drop a printable length byte stuck to the front
            for (size_t j = 0; j < 3 && j < cur.size(); ++j) if ((unsigned char)cur[j] == cur.size() - j) { cur = cur.substr(j + 1); break; }
            std::string lc = LowerA(cur);
            size_t b = lc.find_first_not_of("{*)\"' ");
            if (b != std::string::npos) lc = lc.substr(b);
            for (const char* e : exts) if (EndsWith(lc, e) && lc != LowerA(path)) { refs.insert(lc); break; }
        }
        cur.clear();
    }
    int missing = 0;
    if (!refs.empty()) r += "  files it uses:\n";
    for (auto& p : refs) {
        std::string where;
        bool ok = FileKnown(FileId(p.c_str()), &where);
        bool soft = EndsWith(p, ".xml") || EndsWith(p, ".hkx");      // may be resolved another way: don't call it a problem
        if (!ok && !soft) ++missing;
        r += std::string(ok ? "    ok      " : soft ? "    not found (may be fine) " : "    MISSING ") + p + (ok ? "   (" + where + ")" : "") + "\n";
    }
    if (refs.empty()) r += "  files it uses: none found\n";
    if (missing) {
        r += "  PROBLEM: " + std::to_string(missing) + " file(s) it uses don't exist. Put them in your mod's files\\ folder at\n"
             "    exactly those paths (or point the archetype at files that exist). A model that isn't\n"
             "    there means nothing to show, and the spawn fails or is invisible.\n";
        ++problems;
    }
    // weapons: the chain the spawner follows for pickups
    uint64_t props = ArchetypeRefField(id, "#dd5d4f7d");
    uint64_t pick = ArchetypeRefField(id, "arkPickupArchetype");
    if (props) {
        std::string pp = "entityarchetypeslibrary\\" + std::to_string(props) + ".ark.fcb", where;
        bool ok = FileKnown(FileId(pp.c_str()), &where);
        r += std::string("  weapon properties archetype ") + std::to_string(props) + (ok ? ": ok (" + where + ")\n" : ": MISSING\n");
        if (!ok) ++problems;
        if (!pick) pick = ArchetypeRefField(props, "arkPickupArchetype");
    }
    if (pick) {
        std::string pp = "entityarchetypeslibrary\\" + std::to_string(pick) + ".ark.fcb", where;
        bool ok = FileKnown(FileId(pp.c_str()), &where);
        r += std::string("  pickup archetype ") + std::to_string(pick) + (ok ? ": ok (" + where + ")\n" : ": MISSING\n");
        if (!ok) ++problems;
    }
    r += problems ? "  => " + std::to_string(problems) + " problem(s) found, see above.\n"
                  : "  => no problems found in the file. If it still won't spawn, turn on F8 > Developer > File log,\n"
                    "     spawn it, and send fc4hook.log + dumps\\file_log.txt with the report.\n";
    return r;
}

std::string ArchetypeCheckMods() {
    std::vector<VfsFileEntry> files = VfsFiles();
    std::string r;
    int n = 0;
    for (auto& f : files) {
        std::string rel = LowerA(f.rel);
        for (auto& c : rel) if (c == '/') c = '\\';
        if (rel.find("entityarchetypeslibrary\\") == std::string::npos) continue;
        if (!EndsWith(rel, ".ark.fcb")) {
            r += "File " + f.rel + " (mod " + f.mod + ")\n  PROBLEM: archetype files must end in .ark.fcb (binary). This one will never be loaded.\n"
                 "    If it's the edited .xml, convert it back to .fcb with the Gibbed FCB converter.\n\n";
            ++n; continue;
        }
        size_t s = rel.rfind('\\');
        uint64_t id = strtoull(rel.c_str() + s + 1, nullptr, 10);
        if (!id) { r += "File " + f.rel + " (mod " + f.mod + ")\n  PROBLEM: the file name must be the archetype id (digits only) + .ark.fcb\n\n"; ++n; continue; }
        r += ArchetypeCheck(id) + "\n";
        ++n;
    }
    if (!n) r = "None of the enabled mods add or replace an archetype (files\\entityarchetypeslibrary\\<id>.ark.fcb).\n"
                "Remember: new loose files are picked up when the game starts.\n";
    return r;
}

void RunExtractJob(std::function<void()> job) { std::thread(std::move(job)).detach(); }

} // namespace fc4
