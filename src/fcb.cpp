// Dunia "nbCF" binary object files (.fcb / .ark.fcb), read and edited in place.
//
// Layout (version 2):  "nbCF" u16 version u16 flags u32 objectCount u32 valueCount, then one
// root object:
//   object  = count childCount | (0xFE u32 offset: same object as at offset)
//             u32 nameHash  count valueCount  value*  object*childCount
//   value   = u32 nameHash  count size | (0xFE u32 offset: same data as at offset)  bytes
//   count   = u8 (< 0xFE) | 0xFF u32
// Names are CRC-32 hashes. The game's own names are strings in FC64.dll, so FC4Hook builds the
// hash -> name table from the loaded DLL (it's decrypted in memory; the file on disk isn't).
//
// Edits never change a value's size, so nothing else in the file moves: that's what makes
// cloning and stat editing safe.
#include "core.h"
#include <map>
#include <mutex>
#include <cstring>

namespace fc4 {

static uint32_t Crc32s(const void* p, size_t n) {
    static uint32_t t[256];
    if (!t[1]) for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; }
    uint32_t c = 0xFFFFFFFFu;
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) c = t[(c ^ b[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
uint32_t FcbHash(const char* name) { return Crc32s(name, strlen(name)); }

// ------------------------------------------------------------------ names
static std::mutex s_nameMx;
static std::map<uint32_t, std::string> s_names;
static bool s_namesBuilt = false;

static void BuildNames() {
    if (s_namesBuilt) return;
    s_namesBuilt = true;
    // a few that only appear inside longer strings, and the ones the Workshop relies on
    static const char* extra[] = { "wpnpropsWeaponProperties", "arkPickupArchetype", "ModelPath", "fileName", "hidArchetypeResId",
                                   "disLibItemId", "Name", "ArchetypeName", "hidName", "ResIds", "ResPack", "ResList" };
    for (const char* e : extra) s_names.emplace(FcbHash(e), e);
    HMODULE m = GetModuleHandleA("FC64.dll");
    if (!m) return;
    auto dos = (IMAGE_DOS_HEADER*)m;
    auto nt = (IMAGE_NT_HEADERS*)((uint8_t*)m + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, ".rdata", 6) != 0 && memcmp(sec->Name, ".data", 5) != 0) continue;
        const uint8_t* p = (const uint8_t*)m + sec->VirtualAddress;
        size_t n = sec->Misc.VirtualSize;
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) continue;
        size_t s = 0;
        for (size_t j = 0; j <= n; ++j) {
            uint8_t c = j < n ? p[j] : 0;
            bool id = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (j > s && c >= '0' && c <= '9');
            if (id) continue;
            size_t len = j - s;
            if (c == 0 && len >= 3 && len <= 63 && !(p[s] >= '0' && p[s] <= '9'))
                s_names.emplace(Crc32s(p + s, len), std::string((const char*)p + s, len));
            s = j + 1;
        }
    }
    Log("fcb: %d field names from FC64.dll", (int)s_names.size());
}

std::string FcbName(uint32_t hash) {
    std::lock_guard<std::mutex> lk(s_nameMx);
    BuildNames();
    auto it = s_names.find(hash);
    if (it != s_names.end()) return it->second;
    char b[16]; snprintf(b, sizeof(b), "#%08x", hash);
    return b;
}

// ------------------------------------------------------------------ parsing
struct Rd {
    const std::vector<uint8_t>& d; size_t p = 0; bool bad = false;
    explicit Rd(const std::vector<uint8_t>& v) : d(v) {}
    uint8_t u8() { if (p + 1 > d.size()) { bad = true; return 0; } return d[p++]; }
    uint32_t u32() { if (p + 4 > d.size()) { bad = true; return 0; } uint32_t v; memcpy(&v, &d[p], 4); p += 4; return v; }
    uint32_t count(bool* isOff) { uint8_t b = u8(); *isOff = false; if (b < 0xFE) return b; if (b == 0xFE) *isOff = true; return u32(); }
};

static bool ParseObj(Rd& r, int depth, const std::string& path, std::vector<FcbValue>& out, int guard) {
    if (r.bad || depth > 64 || guard > 200000) return false;
    bool off;
    uint32_t kids = r.count(&off);
    if (off) return !r.bad;                                    // shared object: listed where it's defined
    uint32_t h = r.u32();
    std::string me = path.empty() ? FcbName(h) : path + "/" + FcbName(h);
    bool voff;
    uint32_t nv = r.count(&voff);
    for (uint32_t i = 0; i < nv && !r.bad; ++i) {
        FcbValue v;
        v.hash = r.u32();
        uint32_t size = r.count(&voff);
        if (voff) { v.shared = true; v.offset = size; v.size = 0; }
        else { v.offset = r.p; v.size = size; if (r.p + size > r.d.size()) { r.bad = true; break; } r.p += size; }
        v.object = me; v.name = FcbName(v.hash); v.depth = depth;
        out.push_back(v);
    }
    for (uint32_t k = 0; k < kids && !r.bad; ++k) if (!ParseObj(r, depth + 1, me, out, guard + (int)out.size())) return false;
    return !r.bad;
}

bool FcbParse(const std::vector<uint8_t>& d, std::vector<FcbValue>& out, std::string* err) {
    out.clear();
    if (d.size() < 16 || memcmp(d.data(), "nbCF", 4) != 0) { if (err) *err = "not a binary FCB file"; return false; }
    Rd r(d); r.p = 16;
    if (!ParseObj(r, 0, "", out, 0)) { if (err) *err = "the file is damaged or in an unknown FCB layout"; return false; }
    if (r.p != d.size()) { if (err) *err = "unexpected data after the root object"; return false; }
    return true;
}

// ------------------------------------------------------------------ edits
int FcbReplaceU64(std::vector<uint8_t>& d, uint64_t from, uint64_t to) {
    int n = 0;
    for (size_t i = 16; i + 8 <= d.size(); ++i) if (memcmp(&d[i], &from, 8) == 0) { memcpy(&d[i], &to, 8); ++n; i += 7; }
    return n;
}

int FcbReplaceText(std::vector<uint8_t>& d, const std::string& from, const std::string& to) {
    if (from.size() != to.size() || from.empty()) return 0;
    int n = 0;
    for (size_t i = 16; i + from.size() <= d.size(); ++i)
        if (memcmp(&d[i], from.data(), from.size()) == 0) { memcpy(&d[i], to.data(), to.size()); ++n; i += from.size() - 1; }
    return n;
}

// Some "i" fields are really stored as floats (the D50's iFireRate is 380.0f). A 4-byte value
// whose int reading is huge but whose float reading is an ordinary number is shown as a float.
static bool LooksFloat(int32_t i) {
    if (i > -100000000 && i < 100000000) return false;
    uint32_t e = ((uint32_t)i >> 23) & 0xff;
    return e >= 100 && e <= 160;                   // |x| roughly 1e-8 .. 1e10
}

// Value as text, by its Hungarian prefix (f float, i/sel int, b bool, ark/dis/hid id, s text).
std::string FcbFormat(const std::vector<uint8_t>& d, const FcbValue& v) {
    if (v.shared || v.offset + v.size > d.size()) return "(shared)";
    const uint8_t* p = &d[v.offset];
    char b[64];
    const std::string& n = v.name;
    if (v.size == 4) {
        float f; int32_t i; memcpy(&f, p, 4); memcpy(&i, p, 4);
        if (n[0] == 'f' || LooksFloat(i)) snprintf(b, sizeof(b), "%g", f);
        else snprintf(b, sizeof(b), "%d", i);
        return b;
    }
    if (v.size == 1) return p[0] ? (n[0] == 'b' ? "yes" : "1") : (n[0] == 'b' ? "no" : "0");
    if (v.size == 8) { uint64_t u; memcpy(&u, p, 8); snprintf(b, sizeof(b), "%llu", (unsigned long long)u); return b; }
    // text: printable characters, then zeros (shorter text written into a longer slot is zero-padded)
    uint32_t len = 0;
    while (len < v.size && p[len] >= 0x20 && p[len] < 0x7f) ++len;
    bool txt = v.size >= 1 && len < v.size && len > 0;
    for (uint32_t i = len; txt && i < v.size; ++i) if (p[i] != 0) txt = false;
    if (txt) return std::string("\"") + std::string((const char*)p, len) + "\"";
    std::string h;
    for (uint32_t i = 0; i < v.size && i < 12; ++i) { snprintf(b, sizeof(b), "%02x", p[i]); h += b; }
    if (v.size > 12) h += "...";
    return h + "  (" + std::to_string(v.size) + " bytes)";
}

// Kind used by the editor: 'f' float, 'i' int, 'b' bool, 'u' 64-bit id, 0 = not editable here.
// Same, looking at the stored value too (int-named fields that hold floats edit as floats).
char FcbKindOf(const std::vector<uint8_t>& d, const FcbValue& v) {
    char k = FcbKind(v);
    if (v.shared || v.size != 4 || v.offset + 4 > d.size()) return k;
    int32_t i; memcpy(&i, &d[v.offset], 4);
    if (k == 'i' && LooksFloat(i)) return 'f';
    return k;
}

char FcbKind(const FcbValue& v) {
    if (v.shared) return 0;
    const std::string& n = v.name;
    if (v.size == 4 && n[0] == 'f') return 'f';
    if (v.size == 4 && (n[0] == 'i' || n.rfind("sel", 0) == 0)) return 'i';
    if (v.size == 1 && n[0] == 'b') return 'b';
    if (v.size == 8) return 'u';
    return 0;
}

} // namespace fc4
