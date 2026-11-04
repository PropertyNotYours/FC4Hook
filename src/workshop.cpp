// Weapon Workshop: make a new weapon from an existing one, then change its stats and model,
// all inside FC4Hook (no FCB converter, no hand-editing ids).
//
// A weapon is three archetypes that point at each other:
//   item        (the armory / shop id)             --wpnpropsWeaponProperties-->
//   properties  (stats, sounds, pickup link)        --arkPickupArchetype-->
//   pickup      (the thing lying on the ground)
// "Create" copies all three into  mods\<Name>\files\entityarchetypeslibrary\  under new ids
// (same number of digits, so the files keep their exact size), rewrites every id, every
// "entityarchetypeslibrary\<id>.ark.fcb" path and every path hash (hidArchetypeResId) to the
// new ones, writes armory.txt and workshop.txt (the recipe). Edits change values in place.
// Restart the game after creating or editing: loose files are packed at start.
#include "core.h"
#include <map>
#include <functional>
#include <cstring>
#include <atomic>
#include <mutex>

namespace fc4 {

static std::wstring ModDir(const std::string& mod) { return g_rootDir + L"mods\\" + Widen(mod) + L"\\"; }
static std::string ArkPath(uint64_t id) { return "entityarchetypeslibrary\\" + std::to_string(id) + ".ark.fcb"; }
static std::wstring ArkFile(const std::string& mod, uint64_t id) { return ModDir(mod) + L"files\\" + Widen(ArkPath(id)); }

static bool ReadDisk(const std::wstring& p, std::vector<uint8_t>& d) {
    FILE* f = _wfopen(p.c_str(), L"rb"); if (!f) return false;
    d.clear(); uint8_t b[65536]; size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) d.insert(d.end(), b, b + n);
    fclose(f); return true;
}
static bool WriteDisk(const std::wstring& p, const void* data, size_t n) {
    for (size_t i = 0; i < p.size(); ++i) if (p[i] == L'\\' && i > 2) CreateDirectoryW(p.substr(0, i).c_str(), nullptr);
    FILE* f = _wfopen(p.c_str(), L"wb"); if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n; fclose(f); return ok;
}

bool WeaponChainOf(uint64_t item, WeaponChain* c) {
    c->item = item;
    c->props = ArchetypeRefField(item, "wpnpropsWeaponProperties");
    c->pickup = c->props ? ArchetypeRefField(c->props, "arkPickupArchetype") : 0;
    if (!c->pickup) c->pickup = ArchetypeRefField(item, "arkPickupArchetype");
    return c->props != 0;
}

static bool IdTaken(uint64_t id) {
    uint64_t f = FileId(ArkPath(id).c_str());
    if (GameFileExists(f)) return true;
    for (auto& v : VfsFiles()) if (v.id == f) return true;
    return false;
}

// New id with the same number of digits: same first digits, last 7 from a hash of the name.
static uint64_t NewIdLike(uint64_t old, const std::string& salt, const std::vector<uint64_t>& avoid) {
    std::string s = std::to_string(old);
    size_t keep = s.size() > 8 ? s.size() - 7 : 1;
    uint32_t h = FcbHash((salt + s).c_str());
    for (int tries = 0; tries < 1000; ++tries, h = h * 1103515245u + 12345u) {
        std::string tail = std::to_string(h % 10000000u);
        std::string n = s.substr(0, keep) + std::string(s.size() - keep > tail.size() ? s.size() - keep - tail.size() : 0, '0') + tail;
        n = n.substr(0, s.size());
        uint64_t id = strtoull(n.c_str(), nullptr, 10);
        if (std::to_string(id).size() != s.size() || id == old) continue;
        bool clash = false; for (uint64_t a : avoid) if (a == id) clash = true;
        if (!clash && !IdTaken(id)) return id;
    }
    return 0;
}

static std::string SafeName(const std::string& n) {
    std::string r;
    for (char c : n) r += (isalnum((unsigned char)c) || c == '_' || c == '-' || c == ' ') ? c : '_';
    while (!r.empty() && r.back() == ' ') r.pop_back();
    return r.empty() ? "MyWeapon" : r;
}

bool WorkshopCreate(uint64_t baseItem, const std::string& displayName, std::string* modOut, std::string* msg) {
    WeaponChain b;
    if (!WeaponChainOf(baseItem, &b)) { *msg = "That archetype isn't a weapon item (it has no WeaponProperties link)."; return false; }
    uint64_t old[3] = { b.item, b.props, b.pickup };
    std::vector<uint8_t> data[3];
    for (int i = 0; i < 3; ++i) {
        if (!old[i]) continue;
        std::string err;
        if (!AnyFileRead(FileId(ArkPath(old[i]).c_str()), data[i], &err, nullptr)) { *msg = "Couldn't read " + ArkPath(old[i]) + ": " + err; return false; }
    }
    std::string mod = "Workshop_" + SafeName(displayName);
    if (GetFileAttributesW(ModDir(mod).c_str()) != INVALID_FILE_ATTRIBUTES) { *msg = "A mod called " + mod + " already exists: pick another name."; return false; }
    std::vector<uint64_t> used;
    uint64_t nu[3] = {};
    for (int i = 0; i < 3; ++i) if (old[i]) { nu[i] = NewIdLike(old[i], displayName + char('a' + i), used); if (!nu[i]) { *msg = "Couldn't find a free id."; return false; } used.push_back(nu[i]); }
    // archetype names ("FCX/AK47", "weapons:fcx/ak47"...) get a unique ending of the same length,
    // so nothing that looks archetypes up by name finds the original instead
    std::vector<std::pair<std::string, std::string>> renames;
    {
        std::vector<FcbValue> vals; std::string err;
        if (FcbParse(data[0], vals, &err))
            for (auto& v : vals)
                if (!v.shared && v.name == "Name" && v.size >= 4 && data[0][v.offset + v.size - 1] == 0) {
                    std::string n((const char*)&data[0][v.offset], v.size - 1);
                    uint32_t h = FcbHash((displayName + n).c_str());
                    const char* al = "abcdefghijklmnopqrstuvwxyz0123456789";
                    std::string nn = n.substr(0, n.size() - 2) + al[h % 36] + al[(h / 36) % 36];
                    renames.push_back({ n, nn });
                    break;
                }
    }
    for (int f = 0; f < 3; ++f) {
        if (!old[f]) continue;
        for (auto& rn : renames) {
            std::string a = rn.first, b2 = rn.second, la = a, lb = b2, ua = a, ub = b2;
            for (auto& c : la) c = (char)tolower((unsigned char)c);
            for (auto& c : lb) c = (char)tolower((unsigned char)c);
            for (auto& c : ua) c = (char)toupper((unsigned char)c);
            for (auto& c : ub) c = (char)toupper((unsigned char)c);
            FcbReplaceText(data[f], a, b2); FcbReplaceText(data[f], la, lb); FcbReplaceText(data[f], ua, ub);
        }
        for (int i = 0; i < 3; ++i) {
            if (!old[i]) continue;
            FcbReplaceU64(data[f], old[i], nu[i]);                                             // ids and links
            FcbReplaceU64(data[f], FileId(ArkPath(old[i]).c_str()), FileId(ArkPath(nu[i]).c_str()));   // path hashes
            FcbReplaceText(data[f], std::to_string(old[i]), std::to_string(nu[i]));           // "entityarchetypeslibrary\<id>.ark.fcb"
        }
        std::vector<FcbValue> vals; std::string err;
        if (!FcbParse(data[f], vals, &err)) { *msg = "Internal error: the copy doesn't parse (" + err + ")"; return false; }
    }
    for (int i = 0; i < 3; ++i) if (old[i] && !WriteDisk(ArkFile(mod, nu[i]), data[i].data(), data[i].size())) { *msg = "Couldn't write into " + Narrow(ModDir(mod)); return false; }
    char buf[1024];
    snprintf(buf, sizeof(buf),
             "# FC4Hook Weapon Workshop recipe (F8 > Workshop edits these files)\r\nname=%s\r\nbase=%llu\r\nitem=%llu\r\nprops=%llu\r\npickup=%llu\r\n",
             displayName.c_str(), (unsigned long long)baseItem, (unsigned long long)nu[0], (unsigned long long)nu[1], (unsigned long long)nu[2]);
    WriteDisk(ModDir(mod) + L"workshop.txt", buf, strlen(buf));
    snprintf(buf, sizeof(buf), "[Workshop]\r\n%s = %llu\r\n", displayName.c_str(), (unsigned long long)nu[0]);
    WriteDisk(ModDir(mod) + L"armory.txt", buf, strlen(buf));
    *modOut = mod;
    snprintf(buf, sizeof(buf), "Created %s: item %llu, properties %llu, pickup %llu. Restart the game to load it.",
             mod.c_str(), (unsigned long long)nu[0], (unsigned long long)nu[1], (unsigned long long)nu[2]);
    *msg = buf;
    Log("workshop: %s", buf);
    return true;
}

std::vector<WorkshopWeapon> WorkshopList() {
    std::vector<WorkshopWeapon> r;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_rootDir + L"mods\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return r;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        std::string mod = Narrow(fd.cFileName);
        std::vector<uint8_t> t;
        if (!ReadDisk(ModDir(mod) + L"workshop.txt", t)) continue;
        WorkshopWeapon w; w.mod = mod;
        std::string s(t.begin(), t.end());
        size_t p = 0;
        while (p < s.size()) {
            size_t e = s.find('\n', p); if (e == std::string::npos) e = s.size();
            std::string l = s.substr(p, e - p); p = e + 1;
            while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
            size_t q = l.find('='); if (q == std::string::npos || l[0] == '#') continue;
            std::string k = l.substr(0, q), v = l.substr(q + 1);
            if (k == "name") w.name = v;
            else if (k == "base") w.base = strtoull(v.c_str(), nullptr, 10);
            else if (k == "item") w.ids[0] = strtoull(v.c_str(), nullptr, 10);
            else if (k == "props") w.ids[1] = strtoull(v.c_str(), nullptr, 10);
            else if (k == "pickup") w.ids[2] = strtoull(v.c_str(), nullptr, 10);
            else if (k == "bones") w.bones = v;
            else if (k == "bones_tp") w.bonesTp = v;
        }
        r.push_back(w);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return r;
}

bool WorkshopRead(const WorkshopWeapon& w, int part, std::vector<uint8_t>& d, std::vector<FcbValue>& vals, std::string* err) {
    if (part < 0 || part > 2 || !w.ids[part]) { *err = "no such part"; return false; }
    if (!ReadDisk(ArkFile(w.mod, w.ids[part]), d)) { *err = "can't read " + Narrow(ArkFile(w.mod, w.ids[part])); return false; }
    return FcbParse(d, vals, err);
}

// Set one value (by file offset) from text. Kind from FcbKind.
bool WorkshopSet(const WorkshopWeapon& w, int part, uint32_t offset, char kind, const std::string& text, std::string* err) {
    std::vector<uint8_t> d; std::vector<FcbValue> vals;
    if (!WorkshopRead(w, part, d, vals, err)) return false;
    const FcbValue* v = nullptr;
    for (auto& x : vals) if (!x.shared && x.offset == offset) v = &x;
    if (!v) { *err = "value not found"; return false; }
    if (kind == 'f' && v->size == 4) { float f = (float)atof(text.c_str()); memcpy(&d[offset], &f, 4); }
    else if (kind == 'i' && v->size == 4) { int32_t i = atoi(text.c_str()); memcpy(&d[offset], &i, 4); }
    else if (kind == 'b' && v->size == 1) d[offset] = (text == "1" || text == "yes" || text == "true") ? 1 : 0;
    else if (kind == 'u' && v->size == 8) { uint64_t u = strtoull(text.c_str(), nullptr, 10); memcpy(&d[offset], &u, 8); }
    else if ((kind == 'p' || kind == 'R') && v->size == 8) {
        // A model path: the 8-byte hash of its game path, with the readable path stored just
        // before it. The archetype also lists the model in its resource list (ResPack/ResIds:
        // files loaded with it). Only THIS slot and the matching resource-list entry change.
        // 'R' = back to the base weapon's model for this slot.
        std::vector<uint8_t> base;
        bool haveBase = WorkshopReadBase(w, part, base) && base.size() == d.size();
        uint64_t cur, orig = 0, u;
        memcpy(&cur, &d[offset], 8);
        if (haveBase) memcpy(&orig, &base[offset], 8);
        std::string newText = text;
        if (kind == 'R') {
            if (!haveBase) { *err = "the base weapon can't be read"; return false; }
            u = orig;
            const FcbValue* tv = nullptr;
            for (size_t i = 1; i < vals.size(); ++i) if (&vals[i] == v) tv = &vals[i - 1];
            if (tv && !tv->shared && tv->offset + tv->size <= base.size()) newText = std::string((const char*)&base[tv->offset], strnlen((const char*)&base[tv->offset], tv->size));
        } else u = FileId(text.c_str());
        // other model slots that still use a hash: those entries in the resource list must stay
        auto usedElsewhere = [&](uint64_t h) {
            for (auto& x : vals) if (!x.shared && x.offset != offset && x.size == 8 && x.name == "ModelPath") {
                uint64_t o; memcpy(&o, &d[x.offset], 8); if (o == h) return true; }
            return false;
        };
        for (auto& x : vals) {
            if (x.shared || x.name != "ResIds" || x.size < 12) continue;
            for (uint32_t k = x.offset + 4; k + 8 <= x.offset + x.size; k += 8) {
                uint64_t r; memcpy(&r, &d[k], 8);
                if ((r == cur && cur != u && !usedElsewhere(cur)) || (r == orig && orig != u && orig != cur && !usedElsewhere(orig)))
                    memcpy(&d[k], &u, 8);
            }
        }
        memcpy(&d[offset], &u, 8);
        // the readable path (value just before the hash), when the new one fits
        for (size_t i = 1; i < vals.size(); ++i) if (&vals[i] == v) {
            const FcbValue& t = vals[i - 1];
            if (!t.shared && t.size > 1 && t.size >= newText.size() + 1) { memset(&d[t.offset], 0, t.size); memcpy(&d[t.offset], newText.data(), newText.size()); }
        }
    }
    else { *err = "can't edit that kind of value"; return false; }
    std::vector<FcbValue> chk;
    if (!FcbParse(d, chk, err)) return false;
    if (!WriteDisk(ArkFile(w.mod, w.ids[part]), d.data(), d.size())) { *err = "can't write the file"; return false; }
    return true;
}

// The base weapon's value at the same place, for "default" columns. Same layout as the copy.
// The pickup (the weapon lying on the ground) doesn't have a ModelPath like the item: its model path
// is written as text inside a packed descriptor and a graphic component. These find and swap it.
std::vector<std::string> WorkshopPickupModels(const WorkshopWeapon& w) {
    std::vector<std::string> r; std::vector<uint8_t> d;
    if (!w.ids[2] || !ReadDisk(ArkFile(w.mod, w.ids[2]), d)) return r;
    std::string cur;
    for (size_t i = 0; i <= d.size(); ++i) {
        char c = i < d.size() ? (char)d[i] : 0;
        if (c >= 0x20 && c < 0x7f) { cur += c; continue; }
        std::string l = cur; for (auto& ch : l) ch = (char)tolower((unsigned char)ch);
        size_t g = l.find("graphics\\");
        if (g != std::string::npos && l.size() > 4 && l.compare(l.size() - 4, 4, ".xbg") == 0) {
            std::string pth = cur.substr(g), lp = l.substr(g);
            bool dup = false;
            for (auto& x : r) { std::string lx = x; for (auto& ch : lx) ch = (char)tolower((unsigned char)ch); if (lx == lp) dup = true; }
            if (!dup) r.push_back(pth);
        }
        cur.clear();
    }
    return r;
}

// Every copy of `from` (any case) becomes `to` (same length), and its file id too.
bool WorkshopRepointPickup(const WorkshopWeapon& w, const std::string& from, const std::string& to, std::string* err) {
    if (from.size() != to.size()) { *err = "the new path must be as long as the old one"; return false; }
    std::vector<uint8_t> d;
    if (!w.ids[2] || !ReadDisk(ArkFile(w.mod, w.ids[2]), d)) { *err = "can't read the pickup archetype"; return false; }
    int n = 0;
    for (size_t i = 16; i + from.size() <= d.size(); ++i) {
        bool eq = true;
        for (size_t k = 0; k < from.size() && eq; ++k) eq = tolower(d[i + k]) == tolower((unsigned char)from[k]);
        if (!eq) continue;
        memcpy(&d[i], to.data(), to.size()); ++n; i += from.size() - 1;
    }
    std::string lf = from, lt = to;
    for (auto& c : lf) c = (char)tolower((unsigned char)c);
    for (auto& c : lt) c = (char)tolower((unsigned char)c);
    n += FcbReplaceU64(d, FileId(lf.c_str()), FileId(lt.c_str()));
    if (!n) { *err = "the pickup doesn't mention " + from; return false; }
    if (!WriteDisk(ArkFile(w.mod, w.ids[2]), d.data(), d.size())) { *err = "can't write the pickup archetype"; return false; }
    return true;
}

bool WorkshopReadBase(const WorkshopWeapon& w, int part, std::vector<uint8_t>& d) {
    WeaponChain b;
    if (!WeaponChainOf(w.base, &b)) return false;
    uint64_t id = part == 0 ? b.item : part == 1 ? b.props : b.pickup;
    std::string err;
    return id && AnyFileRead(FileId(ArkPath(id).c_str()), d, &err, nullptr);
}

// ------------------------------------------------------------------ safety net
// FC64+0xb9fad0 adds a weapon to a pawn's inventory and reads the weapon's WeaponProperties
// (weapon+0xa0 -> +0x68) without checking. A weapon whose properties archetype hasn't loaded yet
// (a new Workshop weapon given too early) has none there and the game crashes. Skip it instead.
typedef void (*AddWeapon_t)(void* inv, void* weapon, uint32_t a3, uint8_t a4);
static AddWeapon_t o_AddWeapon;
static std::atomic<int> s_skipped{ 0 };

static bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

static void hk_AddWeapon(void* inv, void* weapon, uint32_t a3, uint8_t a4) {
    if (weapon) {
        uint8_t* w = (uint8_t*)weapon;
        void* comp = Readable(w + 0xa0, 8) ? *(void**)(w + 0xa0) : nullptr;
        void* props = comp && Readable((uint8_t*)comp + 0x68, 8) ? *(void**)((uint8_t*)comp + 0x68) : nullptr;
        if (!props) {
            ++s_skipped;
            LogL(LOG_WARN, "workshop: a weapon without loaded WeaponProperties was about to be added (it would crash the game); skipped. "
                           "Give it again in a moment (FC4Hook preloads Workshop weapons when you enter the world).");
            return;
        }
    }
    o_AddWeapon(inv, weapon, a3, a4);
}

void WorkshopInstallGuard() {
    uintptr_t b = (uintptr_t)GetModuleHandleA("FC64.dll");
    if (!b) return;
    void* f = (void*)(b + 0xb9fad0);
    // patch 1.10:  test rdx,rdx ; jz +0x1c0 ; mov [rsp+18],rbx
    static const uint8_t expect[] = { 0x48, 0x85, 0xd2, 0x0f, 0x84, 0xc0, 0x01, 0x00, 0x00, 0x48, 0x89, 0x5c, 0x24, 0x18 };
    if (memcmp(f, expect, sizeof(expect)) != 0) {
        const uint8_t* p = (const uint8_t*)f;
        LogL(LOG_WARN, "workshop: inventory guard not installed (unexpected code %02x %02x %02x %02x: different game version?)", p[0], p[1], p[2], p[3]);
        return;
    }
    if (CreateHook(f, (void*)hk_AddWeapon, (void**)&o_AddWeapon)) LogL(LOG_DEBUG, "workshop: inventory guard installed");
}


// ------------------------------------------------------------------ animation set
// What decides a weapon's first-person arm animations (found 2026-11, patch 1.10) lives in the
// item archetype, and three things must agree:
//   object 53facff5 field ecdd88a8 (u32)       the animation-type number: picks the weapon's branch
//                                               in the move data (1911 = 4, D50 = 20)
//   object 09d5e1b2 (animation component)      package name (7d06447f, c22dbbc8), package id
//                                               (cb44c86d) and gun skeleton (f9f2d5f4 path, 0b80933a id)
//   d9c50d17 dependency lists                   type 4cdda42c = skeleton ids, 84a30af0 = package ids
// Package and skeleton names change length, so the file is rebuilt as a tree. FCB layout: an
// object is  count(children) u32(hash) count(values) {u32 hash, count(size) bytes}  children;
// count = 1 byte, or FF + u32. FE + u32 is a back-reference: for an object the index of an
// earlier object (kept as is), for a value the distance back to an earlier value's count byte.
// Header: 'nbCF', u32 2, u32 objects, u32 objects + object refs - 1.
namespace fcbt {
struct Val { uint32_t hash; std::vector<uint8_t> data; bool ref = false; size_t target = 0, pos = 0; };
struct Obj { bool ref = false; uint32_t refIndex = 0; uint32_t hash = 0; std::vector<Val> vals; std::vector<Obj> kids; };
struct Rd {
    const std::vector<uint8_t>& d; size_t p; bool bad = false;
    uint8_t u8() { if (p >= d.size()) { bad = true; return 0; } return d[p++]; }
    uint32_t u32() { if (p + 4 > d.size()) { bad = true; return 0; } uint32_t v; memcpy(&v, &d[p], 4); p += 4; return v; }
    uint32_t cnt(bool* off) { uint8_t b = u8(); *off = false; if (b < 0xFE) return b; *off = b == 0xFE; return u32(); }
};
static bool ReadObj(Rd& r, Obj& o, int depth) {
    if (depth > 64 || r.bad) return false;
    bool off; uint32_t k = r.cnt(&off);
    if (off) { o.ref = true; o.refIndex = k; return true; }
    o.hash = r.u32();
    uint32_t n = r.cnt(&off);
    for (uint32_t i = 0; i < n && !r.bad; ++i) {
        Val v; v.hash = r.u32(); v.pos = r.p;
        uint32_t sz = r.cnt(&off);
        if (off) { v.ref = true; v.target = v.pos - sz; }
        else { if (r.p + sz > r.d.size()) { r.bad = true; break; } v.data.assign(r.d.begin() + r.p, r.d.begin() + r.p + sz); r.p += sz; }
        o.vals.push_back(std::move(v));
    }
    o.kids.resize(k);
    for (uint32_t i = 0; i < k && !r.bad; ++i) if (!ReadObj(r, o.kids[i], depth + 1)) return false;
    return !r.bad;
}
static bool Parse(const std::vector<uint8_t>& d, Obj& root) {
    if (d.size() < 16 || memcmp(d.data(), "nbCF", 4) != 0) return false;
    Rd r{ d, 16 };
    return ReadObj(r, root, 0) && r.p == d.size();
}
static void PutCnt(std::vector<uint8_t>& o, uint32_t n) {
    if (n < 0xFE) o.push_back((uint8_t)n); else { o.push_back(0xFF); o.insert(o.end(), (uint8_t*)&n, (uint8_t*)&n + 4); }
}
static void Write(const Obj& o, std::vector<uint8_t>& out, std::map<size_t, size_t>& posMap, std::vector<std::pair<size_t, size_t>>& fix,
                  uint32_t& objs, uint32_t& refs) {
    if (o.ref) { out.push_back(0xFE); out.insert(out.end(), (uint8_t*)&o.refIndex, (uint8_t*)&o.refIndex + 4); ++refs; return; }
    ++objs;
    PutCnt(out, (uint32_t)o.kids.size());
    out.insert(out.end(), (uint8_t*)&o.hash, (uint8_t*)&o.hash + 4);
    PutCnt(out, (uint32_t)o.vals.size());
    for (auto& v : o.vals) {
        out.insert(out.end(), (uint8_t*)&v.hash, (uint8_t*)&v.hash + 4);
        posMap[v.pos] = out.size();
        if (v.ref) { fix.push_back({ out.size(), v.target }); out.insert(out.end(), 5, 0); out[out.size() - 5] = 0xFE; }
        else { PutCnt(out, (uint32_t)v.data.size()); out.insert(out.end(), v.data.begin(), v.data.end()); }
    }
    for (auto& k : o.kids) Write(k, out, posMap, fix, objs, refs);
}
static bool Serialize(const std::vector<uint8_t>& orig, const Obj& root, std::vector<uint8_t>& out) {
    out.assign(orig.begin(), orig.begin() + 16);
    std::map<size_t, size_t> posMap; std::vector<std::pair<size_t, size_t>> fix; uint32_t objs = 0, refs = 0;
    Write(root, out, posMap, fix, objs, refs);
    for (auto& f : fix) {
        auto it = posMap.find(f.second); if (it == posMap.end()) return false;
        uint32_t off = (uint32_t)(f.first - it->second); memcpy(&out[f.first + 1], &off, 4);
    }
    uint32_t h2 = objs, h3 = objs + refs - 1; memcpy(&out[8], &h2, 4); memcpy(&out[12], &h3, 4);
    return true;
}
template <class F> static void Walk(Obj& o, F f) { if (o.ref) return; f(o); for (auto& k : o.kids) Walk(k, f); }
static Val* Field(Obj& o, uint32_t h) { for (auto& v : o.vals) if (v.hash == h && !v.ref) return &v; return nullptr; }
} // namespace fcbt

struct AnimSet { bool ok = false; uint32_t type = 0; std::vector<uint8_t> pkgName, pkgName2, pkgId, skel, skelId, depSkel, depPkg; };
static AnimSet ReadAnimSet(fcbt::Obj& root) {
    AnimSet a; int found = 0;
    fcbt::Walk(root, [&](fcbt::Obj& o) {
        using namespace fcbt;
        if (o.hash == 0x53facff5) if (Val* v = Field(o, 0xecdd88a8)) if (v->data.size() == 4) { memcpy(&a.type, v->data.data(), 4); ++found; }
        if (o.hash == 0x09d5e1b2) {
            if (Val* v = Field(o, 0x7d06447f)) { a.pkgName = v->data; ++found; }
            if (Val* v = Field(o, 0xc22dbbc8)) a.pkgName2 = v->data;
            if (Val* v = Field(o, 0xcb44c86d)) a.pkgId = v->data;
            if (Val* v = Field(o, 0xf9f2d5f4)) a.skel = v->data;
            if (Val* v = Field(o, 0x0b80933a)) a.skelId = v->data;
        }
        if (o.hash == 0x0ad3ab2c && o.vals.size() >= 2 && !o.vals[0].ref && o.vals[0].data.size() == 4) {
            uint32_t t; memcpy(&t, o.vals[0].data.data(), 4);
            if (t == 0x4cdda42cu && !o.vals[1].ref) a.depSkel = o.vals[1].data;
            if (t == 0x84a30af0u && !o.vals[1].ref) a.depPkg = o.vals[1].data;
        }
    });
    a.ok = found >= 2 && a.pkgId.size() == 8;
    return a;
}
static void WriteAnimSet(fcbt::Obj& root, const AnimSet& a) {
    fcbt::Walk(root, [&](fcbt::Obj& o) {
        using namespace fcbt;
        if (o.hash == 0x53facff5) if (Val* v = Field(o, 0xecdd88a8)) if (v->data.size() == 4) memcpy(v->data.data(), &a.type, 4);
        if (o.hash == 0x09d5e1b2) {
            if (Val* v = Field(o, 0x7d06447f)) v->data = a.pkgName;
            if (Val* v = Field(o, 0xc22dbbc8)) if (!a.pkgName2.empty()) v->data = a.pkgName2;
            if (Val* v = Field(o, 0xcb44c86d)) v->data = a.pkgId;
            if (Val* v = Field(o, 0xf9f2d5f4)) if (!a.skel.empty()) v->data = a.skel;
            if (Val* v = Field(o, 0x0b80933a)) if (!a.skelId.empty()) v->data = a.skelId;
        }
        if (o.hash == 0x0ad3ab2c && o.vals.size() >= 2 && !o.vals[0].ref && o.vals[0].data.size() == 4) {
            uint32_t t; memcpy(&t, o.vals[0].data.data(), 4);
            if (t == 0x4cdda42cu && !a.depSkel.empty()) { o.vals[1].ref = false; o.vals[1].data = a.depSkel; }
            if (t == 0x84a30af0u && !a.depPkg.empty()) { o.vals[1].ref = false; o.vals[1].data = a.depPkg; }
        }
    });
}
static std::string AnimSetText(const AnimSet& a) {
    std::string n((const char*)a.pkgName.data(), strnlen((const char*)a.pkgName.data(), a.pkgName.size()));
    return n + " (type " + std::to_string(a.type) + ")";
}

// The animation set the weapon uses now, e.g. "kimber (type 4)".
std::string WorkshopAnimSet(const WorkshopWeapon& w) {
    std::vector<uint8_t> d;
    if (!ReadDisk(ArkFile(w.mod, w.ids[0]), d)) return "?";
    fcbt::Obj root; if (!fcbt::Parse(d, root)) return "?";
    AnimSet a = ReadAnimSet(root);
    return a.ok ? AnimSetText(a) : "?";
}

// Give the weapon the first-person animation set of another weapon (its type number, package and
// gun skeleton). The donor is read from the game (or a mod).
bool WorkshopSetAnimSet(const WorkshopWeapon& w, uint64_t donorItem, std::string* msg) {
    std::vector<uint8_t> dd, d; std::string err;
    if (!AnyFileRead(FileId(ArkPath(donorItem).c_str()), dd, &err, nullptr)) { *msg = "Couldn't read that weapon: " + err; return false; }
    fcbt::Obj droot; if (!fcbt::Parse(dd, droot)) { *msg = "That weapon's file has an unknown layout."; return false; }
    AnimSet a = ReadAnimSet(droot);
    if (!a.ok) { *msg = "That weapon has no first-person animation set (is it a weapon item?)."; return false; }
    std::wstring path = ArkFile(w.mod, w.ids[0]);
    if (!ReadDisk(path, d)) { *msg = "Couldn't read " + Narrow(path); return false; }
    fcbt::Obj root; if (!fcbt::Parse(d, root)) { *msg = "This weapon's file has an unknown layout."; return false; }
    AnimSet cur = ReadAnimSet(root);
    if (!cur.ok) { *msg = "This weapon's file has no animation component."; return false; }
    // keep a backup of the first version
    std::wstring bak = path + L".orig";
    if (GetFileAttributesW(bak.c_str()) == INVALID_FILE_ATTRIBUTES) WriteDisk(bak, d.data(), d.size());
    WriteAnimSet(root, a);
    std::vector<uint8_t> out;
    if (!fcbt::Serialize(d, root, out)) { *msg = "Couldn't rebuild the file (unexpected references)."; return false; }
    fcbt::Obj check; if (!fcbt::Parse(out, check) || !ReadAnimSet(check).ok) { *msg = "Rebuilt file didn't check out; nothing written."; return false; }
    if (!WriteDisk(path, out.data(), out.size())) { *msg = "Couldn't write " + Narrow(path); return false; }
    // reload style lives in the weapon properties (magazine vs shell by shell...): copy every
    // field whose name mentions reload from the donor's properties, matched by object path
    std::string reloadNote;
    {
        WeaponChain dc; std::vector<uint8_t> dp, mp;
        std::wstring ppath = ArkFile(w.mod, w.ids[1]);
        if (WeaponChainOf(donorItem, &dc) && AnyFileRead(FileId(ArkPath(dc.props).c_str()), dp, &err, nullptr) && ReadDisk(ppath, mp)) {
            fcbt::Obj dr, mr;
            if (fcbt::Parse(dp, dr) && fcbt::Parse(mp, mr)) {
                std::map<std::string, fcbt::Obj*> dobj;
                std::function<void(fcbt::Obj&, const std::string&, std::map<std::string, fcbt::Obj*>&)> idx =
                    [&](fcbt::Obj& o, const std::string& path, std::map<std::string, fcbt::Obj*>& m) {
                        if (o.ref) return;
                        std::string me = path + "/" + std::to_string(o.hash);
                        if (!m.count(me)) m[me] = &o;
                        for (auto& k : o.kids) idx(k, me, m);
                    };
                std::map<std::string, fcbt::Obj*> mobj;
                idx(dr, "", dobj); idx(mr, "", mobj);
                int n = 0; std::string names;
                for (auto& kv : mobj) {
                    auto it = dobj.find(kv.first); if (it == dobj.end()) continue;
                    for (auto& v : kv.second->vals) {
                        if (v.ref) continue;
                        std::string nm = FcbName(v.hash), l = nm;
                        for (auto& c : l) c = (char)tolower((unsigned char)c);
                        if (l.find("reload") == std::string::npos) continue;
                        if (fcbt::Val* dv = fcbt::Field(*it->second, v.hash)) if (dv->data != v.data) {
                            v.data = dv->data; ++n;
                            if (names.size() < 200) names += (names.empty() ? "" : ", ") + nm;
                        }
                    }
                }
                std::vector<uint8_t> pout;
                if (n && fcbt::Serialize(mp, mr, pout)) {
                    std::wstring pb = ppath + L".orig";
                    if (GetFileAttributesW(pb.c_str()) == INVALID_FILE_ATTRIBUTES) WriteDisk(pb, mp.data(), mp.size());
                    fcbt::Obj chk;
                    if (fcbt::Parse(pout, chk) && WriteDisk(ppath, pout.data(), pout.size()))
                        reloadNote = " Reload style copied too (" + std::to_string(n) + " settings: " + names + ").";
                }
            }
        }
    }
    // remember the donor's models: new models for this weapon are built on their bones
    std::string m1, mtp;
    fcbt::Walk(droot, [&](fcbt::Obj& o) {
        for (auto& v : o.vals) if (v.hash == 0xc230e653 && !v.ref && v.data.size() > 5) {
            std::string t((const char*)v.data.data(), strnlen((const char*)v.data.data(), v.data.size()));
            std::string l = t; for (auto& c : l) c = (char)tolower((unsigned char)c);
            if (l.size() < 5 || l.compare(l.size() - 4, 4, ".xbg") != 0) continue;
            if (l.find("_tp") != std::string::npos) { if (mtp.empty()) mtp = t; } else if (m1.empty()) m1 = t;
        }
    });
    {
        std::vector<uint8_t> t; ReadDisk(ModDir(w.mod) + L"workshop.txt", t);
        std::string s(t.begin(), t.end()), o;
        size_t p = 0;
        while (p < s.size()) {
            size_t e = s.find('\n', p); if (e == std::string::npos) e = s.size();
            std::string l = s.substr(p, e - p + (e < s.size() ? 1 : 0)); p = e + 1;
            if (l.rfind("bones=", 0) == 0 || l.rfind("bones_tp=", 0) == 0 || l.rfind("animset=", 0) == 0) continue;
            o += l;
        }
        if (!o.empty() && o.back() != '\n') o += "\r\n";
        o += "animset=" + std::to_string(donorItem) + "\r\n";
        if (!m1.empty()) o += "bones=" + m1 + "\r\n";
        if (!mtp.empty()) o += "bones_tp=" + mtp + "\r\n";
        WriteDisk(ModDir(w.mod) + L"workshop.txt", o.data(), o.size());
    }
    *msg = "Animation set is now " + AnimSetText(a) + " (was " + AnimSetText(cur) + ")." + reloadNote + " Restart the game. "
           "Rebuild the gun's models (Model tab, Use my .glb) so they sit right in the hands: they're now built on " + (m1.empty() ? std::string("that weapon's") : m1) + "'s bones.";
    return true;
}

// ------------------------------------------------------------------ registration
// The game doesn't take a weapon's properties from the weapon's own file: at startup it reads
// entitylibraryNamesToResId.fcb into its archetype library (global FC64+0x2e22dd0) and builds a
// map  weapon archetype resource id -> WeaponProperties  (library+0xe8; WeaponArchetypesResIdsList /
// WeaponPropertiesIdsList). A Workshop weapon isn't in that file, so it gets no properties: it
// can't be given and its pickup can't be taken. FC4Hook adds every Workshop weapon to the map
// with the game's own insert (FC64+0x71fa50). Whether the map stores the properties' archetype id
// or their resource id is checked on the base weapon first.
typedef uint64_t* (*MapInsert_t)(void* map, const uint64_t* key);
typedef uint64_t* (*MapFind_t)(void* lib, uint64_t* out, uint64_t key);
static std::mutex s_regMx;
static std::string s_regStatusV = "not done yet";
static void SetRegStatus(const std::string& v) { std::lock_guard<std::mutex> lk(s_regMx); s_regStatusV = v; }
std::string WorkshopRegisterStatus() { std::lock_guard<std::mutex> lk(s_regMx); return s_regStatusV; }

int WorkshopRegister() {
    uintptr_t b = (uintptr_t)GetModuleHandleA("FC64.dll");
    if (!b) return -1;
    void** gLib = (void**)(b + 0x2e22dd0);
    if (!Readable(gLib, 8) || !*gLib) { SetRegStatus("the game's archetype library isn't loaded yet"); return -1; }
    void* lib = *gLib;
    // sanity: the library's vtable slot 12 must be the map lookup we expect (patch 1.10)
    void** vt = Readable(lib, 8) ? *(void***)lib : nullptr;
    if (!vt || !Readable(vt + 12, 8) || (uintptr_t)vt[12] != b + 0x713080) { SetRegStatus("unexpected archetype library (different game version?)"); return -1; }
    MapFind_t find = (MapFind_t)(b + 0x713080);
    MapInsert_t insert = (MapInsert_t)(b + 0x71fa50);
    int n = 0, already = 0, bad = 0;
    std::string why;
    for (auto& w : WorkshopList()) {
        WeaponChain base;
        if (!WeaponChainOf(w.base, &base)) { ++bad; why = "base weapon of " + w.name + " not found"; continue; }
        uint64_t baseKey = FileId(ArkPath(base.item).c_str()), out = 0;
        find(lib, &out, baseKey);
        uint64_t newVal;
        if (out == 0) { ++bad; why = "the game hasn't filled its weapon table yet (retrying)"; continue; }
        if (out == base.props) newVal = w.ids[1];
        else if (out == FileId(ArkPath(base.props).c_str())) newVal = FileId(ArkPath(w.ids[1]).c_str());
        else {
            ++bad;
            char m[200]; snprintf(m, sizeof(m), "the base weapon isn't mapped the way FC4Hook expects (value %llu)", (unsigned long long)out);
            why = m; continue;
        }
        uint64_t key = FileId(ArkPath(w.ids[0]).c_str()), cur = 0;
        find(lib, &cur, key);
        if (cur == newVal) { ++already; continue; }
        uint64_t* slot = insert((uint8_t*)lib + 0xe8, &key);
        if (slot) { *slot = newVal; ++n; LogL(LOG_INFO, "workshop: registered %s (item %llu -> properties %llu)", w.name.c_str(), (unsigned long long)w.ids[0], (unsigned long long)w.ids[1]); }
    }
    char m[300];
    snprintf(m, sizeof(m), "%d weapon(s) registered, %d already%s%s", n, already, bad ? ", problem: " : "", bad ? why.c_str() : "");
    SetRegStatus(m);
    if (bad) LogL(LOG_WARN, "workshop: %s", m);
    return n + already;
}

// "item,props,pickup;item,props,pickup" for Lua (preloading)
std::string WorkshopIdList() {
    std::string r;
    for (auto& w : WorkshopList()) {
        if (!r.empty()) r += ";";
        r += std::to_string(w.ids[0]) + "," + std::to_string(w.ids[1]) + "," + std::to_string(w.ids[2]);
    }
    return r;
}

} // namespace fc4
