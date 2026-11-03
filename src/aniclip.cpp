// Far Cry 4 animation clips ("aNi", CAnimationResource): read, export to .glb, and write from a .glb.
// Plain C++ (no Windows). Format notes: docs/ANI_FORMAT.md. Prototypes (verified against game clips):
// tools/ani_decode.py, tools/ani2glb.py, tools/ani_encode.py.
//
// File = 0x18-byte resource header + the clip. Offsets below are from the clip start.
//   +0x04 u16 tracks (low 12 bits; top 2 bits = layout, only 0 supported)   +0x06 u16 frames (keys = frames + 1)
//   +0x08 f32 length   +0x10 u32 track flags offset   +0x14 u32 track bone-hash offset (crc32 of bone names)
//   +0x18/+0x1a/+0x1c/+0x1e  counts: constant rotations / constant positions / animated rotations / animated positions
//   +0x20 position scale entries   +0x28.. u16 section offsets with high bytes at +0x3e..: 0x28 const rot,
//   0x2a const pos, 0x2c ?, 0x2e pos scales, 0x30 anim rot, 0x32 anim pos, 0x34/0x36 root tracks,
//   0x38 nested clips (the weapon's own animation), 0x3a events
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <array>
#include <algorithm>
#include "minijson.h"

namespace fc4 {

typedef std::vector<uint8_t> Bytes;
namespace ani {

struct Q { float x, y, z, w; };
struct V { float x, y, z; };

static uint32_t Crc32(const char* s, size_t n) {
    static uint32_t t[256];
    if (!t[1]) for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = t[(c ^ (uint8_t)s[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static uint16_t r16(const Bytes& d, size_t o) { uint16_t v = 0; if (o + 2 <= d.size()) memcpy(&v, &d[o], 2); return v; }
static uint32_t r32(const Bytes& d, size_t o) { uint32_t v = 0; if (o + 4 <= d.size()) memcpy(&v, &d[o], 4); return v; }
static float rf(const Bytes& d, size_t o) { float v = 0; if (o + 4 <= d.size()) memcpy(&v, &d[o], 4); return v; }
static const size_t C = 0x18;

struct BitR {
    const Bytes& d; size_t base; bool bad = false;
    BitR(const Bytes& b, size_t o) : d(b), base(o) {}
    uint32_t get(size_t pos, int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) {
            size_t p = pos + i, b = base + (p >> 3);
            if (b >= d.size()) { bad = true; return v; }
            v |= (uint32_t)((d[b] >> (p & 7)) & 1) << i;
        }
        return v;
    }
};
struct BitW {
    std::vector<uint8_t> bits;
    void put(uint32_t v, int n) { for (int i = 0; i < n; ++i) bits.push_back((v >> i) & 1); }
    Bytes bytes() const {
        Bytes o((bits.size() + 7) / 8 + 4, 0);                   // the game reads a few bytes past a value
        for (size_t i = 0; i < bits.size(); ++i) if (bits[i]) o[i >> 3] |= (uint8_t)(1 << (i & 7));
        return o;
    }
};

static Q ConstQuat(uint16_t v0, uint16_t v1, uint16_t v2) {
    const float S = 4.315969e-05f, H = 0.70710677f;
    float a = (v0 & 0x7fff) * S - H, b = (v1 & 0x7fff) * S - H, c = (int16_t)v2 * S - H;
    float r = sqrtf(std::max(0.f, 1 - a * a - b * b - c * c));
    bool t0 = v0 & 0x8000, t1 = v1 & 0x8000;
    if (t0 && t1) return { a, b, c, r };
    if (!t0 && t1) return { a, b, r, c };
    if (t0 && !t1) return { a, r, b, c };
    return { r, a, b, c };
}

struct Clip {
    int tracks = 0, frames = 0; float length = 0;
    std::vector<uint8_t> flags; std::vector<uint32_t> hashes;
    // per track: 0 = none, else keys (1 key = constant)
    std::vector<std::vector<Q>> rot; std::vector<std::vector<V>> pos;
};

// Short header (track word bit 0x8000, used by the gun clips nested in weapon clips): the section
// offsets sit 6 bytes earlier (0x22.. instead of 0x28..) and have no high bytes.
static bool ShortHdr(const Bytes& d) { return d.size() >= C + 6 && (r16(d, C + 4) & 0x8000); }
static size_t Off(const Bytes& d, int lo, int hi) { return ShortHdr(d) ? (lo >= 0x38 ? 0 : r16(d, C + lo - 6)) : (((size_t)d[C + hi] << 16) | r16(d, C + lo)); }

static bool Decode(const Bytes& d, Clip& c, std::string* err) {
    if (d.size() < C + 0x50 || memcmp(&d[C], "aNi", 3) != 0) { *err = "not an animation clip (no aNi)"; return false; }
    uint16_t th = r16(d, C + 4);
    if (th & 0x4000) { *err = "this clip uses a header layout FC4Hook doesn't read yet"; return false; }
    c.tracks = th & 0xfff; c.frames = r16(d, C + 6); c.length = rf(d, C + 8);
    int keys = c.frames + 1, nblocks = (keys + 7) / 8;
    size_t foff = r32(d, C + 0x10), hoff = r32(d, C + 0x14);
    if (C + foff + c.tracks > d.size() || C + hoff + 4 * c.tracks > d.size() || c.tracks > 1024) { *err = "damaged clip (track tables)"; return false; }
    c.flags.assign(d.begin() + C + foff, d.begin() + C + foff + c.tracks);
    c.hashes.resize(c.tracks);
    for (int i = 0; i < c.tracks; ++i) c.hashes[i] = r32(d, C + hoff + 4 * i);
    c.rot.assign(c.tracks, {}); c.pos.assign(c.tracks, {});
    // constant rotations / positions
    size_t co = C + Off(d, 0x28, 0x3e), po = C + Off(d, 0x2a, 0x3f);
    bool compact = d[C + 3] & 0x20;
    std::vector<int> arot, apos;
    for (int t = 0; t < c.tracks; ++t) {
        int k = c.flags[t] & 0x30;
        if (k == 0x30) { if (co + 6 > d.size()) { *err = "damaged clip (rotations)"; return false; } c.rot[t] = { ConstQuat(r16(d, co), r16(d, co + 2), r16(d, co + 4)) }; co += 6; }
        else if (k == 0x10) arot.push_back(t);
        int p = c.flags[t] & 0xc0;
        if (p == 0xc0) {
            if (compact) { c.pos[t] = { { (int16_t)r16(d, po) / 16384.f, (int16_t)r16(d, po + 2) / 16384.f, (int16_t)r16(d, po + 4) / 16384.f } }; po += 6; }
            else { c.pos[t] = { { rf(d, po), rf(d, po + 4), rf(d, po + 8) } }; po += 12; }
        } else if (p == 0x40) apos.push_back(t);
    }
    auto component = [&](BitR& b, size_t& q, bool constant, int nk, int bits, std::vector<float>& out) {
        out.resize(nk);
        if (constant) { float v = b.get(q, 16) * 3.051851e-05f - 1; q += 16; for (auto& x : out) x = v; return; }
        float mn = b.get(q, 8) / 127.f - 1, rg = b.get(q + 8, 8) / 127.5f; q += 16;
        float sc = 1.f / (float)((1u << bits) - 1);
        for (int k = 0; k < nk; ++k) { out[k] = rg * sc * b.get(q, bits) + mn; q += bits; }
    };
    if (!arot.empty()) {
        size_t ao = C + Off(d, 0x30, 0x42);
        for (int t : arot) c.rot[t].reserve(keys);
        for (int b = 0; b < nblocks; ++b) {
            int nk = std::min(8, keys - 8 * b);
            BitR br(d, ao + r32(d, ao + 4 * b)); size_t q = 0;
            for (int t : arot) {
                int bits = (c.flags[t] & 0xf) + 1;
                uint32_t hdr = br.get(q, 6); q += 6;
                uint32_t sign = 0;
                if (hdr & 8) { sign = br.get(q, nk); q += nk; }
                std::vector<float> cp[3];
                for (int k = 0; k < 3; ++k) component(br, q, (hdr >> k) & 1, nk, bits, cp[k]);
                int big = (hdr >> 4) & 3;
                for (int k = 0; k < nk; ++k) {
                    float a = cp[0][k], bb = cp[1][k], cc = cp[2][k];
                    float r = sqrtf(std::max(0.f, 1 - a * a - bb * bb - cc * cc));
                    if ((sign >> k) & 1) r = -r;
                    float v[4]; int j = 0; float s3[3] = { a, bb, cc };
                    for (int i = 0; i < 4; ++i) v[i] = i == big ? r : s3[j++];
                    c.rot[t].push_back({ v[0], v[1], v[2], v[3] });
                }
            }
            if (br.bad) { *err = "damaged clip (animated rotations)"; return false; }
        }
    }
    if (!apos.empty() && r16(d, C + 0x1e)) {
        size_t ao = C + Off(d, 0x32, 0x43), so = C + Off(d, 0x2e, 0x41);
        int si = Off(d, 0x36, 0x45) ? 1 : 0;
        for (int b = 0; b < nblocks; ++b) {
            int nk = std::min(8, keys - 8 * b);
            BitR br(d, ao + r32(d, ao + 4 * b)); size_t q = 0;
            for (size_t j = 0; j < apos.size(); ++j) {
                uint32_t w = r32(d, so + 4 * (si + j));
                int bits = w & 0xff; float sc = (w >> 8) * 0.00029802325f;
                uint32_t hdr = br.get(q, 3); q += 3;
                std::vector<float> cp[3];
                for (int k = 0; k < 3; ++k) component(br, q, (hdr >> k) & 1, nk, bits, cp[k]);
                for (int k = 0; k < nk; ++k) c.pos[apos[j]].push_back({ sc * cp[0][k], sc * cp[1][k], sc * cp[2][k] });
            }
            if (br.bad) { *err = "damaged clip (animated positions)"; return false; }
        }
    }
    return true;
}

// ------------------------------------------------------------------ skeleton (an .xbg's NODE list)
struct Node { std::string name; int parent; Q q; V t; };
static bool Skeleton(const Bytes& x, std::vector<Node>& nodes, std::string* err) {
    for (size_t i = 0; i + 20 < x.size(); ++i) {
        if (memcmp(&x[i], "EDON", 4) != 0) continue;
        size_t p = i + 20, e = p + r32(x, i + 12);
        uint32_t n = r32(x, p); p += 4;
        if (n == 0 || n > 4096 || e > x.size()) break;
        for (uint32_t k = 0; k < n && p + 72 <= e; ++k) {
            Node nd; nd.parent = (int32_t)r32(x, p + 12);
            nd.q = { rf(x, p + 16), rf(x, p + 20), rf(x, p + 24), rf(x, p + 28) };
            nd.t = { rf(x, p + 32), rf(x, p + 36), rf(x, p + 40) };
            uint32_t ln = r32(x, p + 68);
            if (p + 72 + ln > e) break;
            nd.name.assign((const char*)&x[p + 72], ln); p += 72 + ln + 1;
            nodes.push_back(nd);
        }
        if (nodes.size() == n) return true;
        break;
    }
    *err = "couldn't read the skeleton (NODE list) of the model"; return false;
}


// ------------------------------------------------------------------ nested clips (section 0x38)
// A weapon clip carries the gun's own animation: section 0x38 = u32 count, then count entries of
// 0x80 bytes: +8 u32 offset of that entry's aNi clip (from the entry), +0x14 u32 hash + name[32]
// (the weapon's package, e.g. kimber), +0x38 u32 hash + name[32] (the hand it hangs from, RightHand).
// Entry 0 moves the gun's bones (CLIP, SLIDE, HAMMER...); entry 1 is the gun's root in the other hand.
struct NestedClip { size_t entry, clip, end; std::string pkg, bone; };
struct Nested { size_t sec = 0, secEnd = 0; std::vector<NestedClip> e; };
static std::string CStr(const Bytes& d, size_t o, size_t n) { std::string r; for (size_t i = 0; i < n && o + i < d.size() && d[o + i]; ++i) r += (char)d[o + i]; return r; }
static bool ParseNested(const Bytes& d, Nested& n) {
    if (d.size() < C + 0x50) return false;
    size_t o = Off(d, 0x38, 0x46); if (!o) return false;
    static const int F[10][2] = { { 0x28, 0x3e }, { 0x2a, 0x3f }, { 0x2c, 0x40 }, { 0x2e, 0x41 }, { 0x30, 0x42 }, { 0x32, 0x43 }, { 0x34, 0x44 }, { 0x36, 0x45 }, { 0x38, 0x46 }, { 0x3a, 0x47 } };
    size_t next = d.size() - C;
    for (auto& f : F) { size_t x = Off(d, f[0], f[1]); if (x > o && x < next) next = x; }
    for (size_t x : { (size_t)r32(d, C + 0x10), (size_t)r32(d, C + 0x14) }) if (x > o && x < next) next = x;
    n.sec = C + o; n.secEnd = C + next;
    uint32_t cnt = r32(d, n.sec);
    if (cnt == 0 || cnt > 16 || n.sec + 4 + 0x80 * cnt > n.secEnd) return false;
    for (uint32_t k = 0; k < cnt; ++k) {
        NestedClip c; c.entry = n.sec + 4 + 0x80 * k; c.clip = c.entry + r32(d, c.entry + 8);
        if (c.clip + 0x20 > d.size() || memcmp(&d[c.clip], "aNi", 3) != 0) return false;
        c.pkg = CStr(d, c.entry + 0x18, 32); c.bone = CStr(d, c.entry + 0x3c, 32);
        n.e.push_back(c);
    }
    for (size_t k = 0; k < n.e.size(); ++k) n.e[k].end = k + 1 < n.e.size() && n.e[k + 1].clip > n.e[k].clip ? n.e[k + 1].clip : d.size();
    return true;
}
// a nested clip as a stand-alone clip file (decoders expect the 0x18-byte file header first)
// (its flag and track-hash tables sit at the end of the weapon clip, addressed from the nested aNi,
// so it runs to the end of the file)
static Bytes NestedAsFile(const Bytes& d, const NestedClip& c) { Bytes b(C, 0); b.insert(b.end(), d.begin() + c.clip, d.end()); return b; }
static const char* kGunBoneNames[] = { "FRAME", "CLIP", "SLIDE", "HAMMER", "Hammer", "BULLET", "CLIP_BULLET", "SLIDE_LOCK", "TRIGGER", "BOLT",
    "MAG", "MAGAZINE", "BATTERY", "Root", "root", "CHARGING_HANDLE", "BOLT_CARRIER", "DUST_COVER", "SELECTOR", "LEVER", "SHELL", "SHELLS",
    "CYLINDER", "BARREL", "PUMP", "STOCK", "SAFETY", "COVER", "BELT", "FEED_COVER", "ARROW", "STRING", "LIMB_TOP", "LIMB_BOTTOM", "DRUM", nullptr };

static Q qmul(Q a, Q b) {
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
static float qdot(Q a, Q b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

// ------------------------------------------------------------------ clip -> .glb
static void put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); }

bool ToGlb(const Bytes& clipFile, const Bytes& skel, Bytes& glb, std::string* report, uint64_t clipId, const Bytes* gunSkel = nullptr) {
    std::string& R = *report;
    Clip c; std::vector<Node> nodes;
    if (!Decode(clipFile, c, &R) || !Skeleton(skel, nodes, &R)) return false;
    std::map<uint32_t, int> byHash;
    for (size_t k = 0; k < nodes.size(); ++k) byHash[Crc32(nodes[k].name.data(), nodes[k].name.size())] = (int)k;
    std::vector<int> tm(c.tracks, -1); int matched = 0;
    for (int t = 0; t < c.tracks; ++t) { auto it = byHash.find(c.hashes[t]); if (it != byHash.end()) { tm[t] = it->second; ++matched; } }
    if (matched < c.tracks / 2) { R = "this clip is for another skeleton (" + std::to_string(matched) + " of " + std::to_string(c.tracks) + " bones match the first-person arms)"; return false; }
    int keys = c.frames + 1;
    float fps = c.length > 0 && c.frames > 0 ? c.frames / c.length : 30.f;
    // additive layers: every constant rotation is the identity -> shown on top of the rest pose
    int nc = 0, idn = 0;
    for (auto& r : c.rot) if (r.size() == 1) { ++nc; if (fabsf(r[0].w) > 0.9999f) ++idn; }
    bool additive = nc > 0 && idn * 10 >= nc * 9;
    Bytes bin; std::string bv, ac;
    int nView = 0, nAcc = 0;
    auto acc = [&](const std::vector<float>& v, int comp, const char* type, bool minmax) {
        size_t off = bin.size();
        for (float f : v) { uint32_t u; memcpy(&u, &f, 4); put32(bin, u); }
        char b[256];
        snprintf(b, sizeof(b), "%s{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu}", nView ? "," : "", off, v.size() * 4); bv += b;
        std::string mm;
        if (minmax) { char m[96]; snprintf(m, sizeof(m), ",\"min\":[%g],\"max\":[%g]", v.front(), v.back()); mm = m; }
        snprintf(b, sizeof(b), "%s{\"bufferView\":%d,\"componentType\":5126,\"count\":%zu,\"type\":\"%s\"%s}", nAcc ? "," : "", nView, v.size() / comp, type, mm.c_str()); ac += b;
        ++nView; return nAcc++;
    };
    // the gun's own animation (nested clip 0): its bones become extra nodes hanging from the hand
    struct GunNode { std::string name; int parent; Q q; V t; int track; };   // parent: -1-k = arm node k, else gun node index
    std::vector<GunNode> gun; Clip gc; bool haveGun = false;
    {
        Nested ns;
        if (ParseNested(clipFile, ns) && !ns.e.empty()) {
            std::string e2;
            int hand = -1; for (size_t k = 0; k < nodes.size(); ++k) if (nodes[k].name == ns.e[0].bone) hand = (int)k;
            if (hand >= 0 && Decode(NestedAsFile(clipFile, ns.e[0]), gc, &e2)) {
                haveGun = true;
                std::map<uint32_t, std::string> known;
                for (int i = 0; kGunBoneNames[i]; ++i) known[Crc32(kGunBoneNames[i], strlen(kGunBoneNames[i]))] = kGunBoneNames[i];
                std::vector<Node> gn; std::string e3; std::map<uint32_t, int> gidx;
                if (gunSkel && Skeleton(*gunSkel, gn, &e3)) for (size_t k = 0; k < gn.size(); ++k) { known[Crc32(gn[k].name.data(), gn[k].name.size())] = gn[k].name; gidx[Crc32(gn[k].name.data(), gn[k].name.size())] = (int)k; }
                std::map<int, int> trackOfGunSkel;
                for (int t = 0; t < gc.tracks; ++t) {
                    GunNode g; g.track = t; g.parent = -1 - hand;
                    auto it = known.find(gc.hashes[t]);
                    char nb[32]; snprintf(nb, sizeof(nb), "gun_%08x", gc.hashes[t]);
                    g.name = it != known.end() ? it->second : nb;
                    g.q = gc.rot[t].empty() ? Q{ 0, 0, 0, 1 } : gc.rot[t][0];
                    g.t = gc.pos[t].empty() ? V{ 0, 0, 0 } : gc.pos[t][0];
                    auto gi = gidx.find(gc.hashes[t]); if (gi != gidx.end()) trackOfGunSkel[gi->second] = (int)gun.size();
                    gun.push_back(g);
                }
                // hierarchy from the gun's skeleton, where known
                for (auto& kv : trackOfGunSkel) {
                    int p = gn[kv.first].parent;
                    while (p >= 0 && !trackOfGunSkel.count(p)) p = gn[p].parent;
                    if (p >= 0) gun[kv.second].parent = trackOfGunSkel[p];
                }
            }
        }
    }
    // nodes: 0 = FC4_ROOT (Z-up -> Y-up), 1.. = skeleton, then the gun's bones
    std::string nj = "{\"name\":\"FC4_ROOT\",\"rotation\":[-0.70710678,0,0,0.70710678],\"children\":[";
    bool first = true;
    for (size_t k = 0; k < nodes.size(); ++k) if (nodes[k].parent < 0) { nj += (first ? "" : ",") + std::to_string(k + 1); first = false; }
    nj += "]}";
    for (size_t k = 0; k < nodes.size(); ++k) {
        const Node& n = nodes[k];
        std::string name; for (char ch : n.name) if (ch != '"' && ch != '\\' && (unsigned char)ch >= 0x20) name += ch;
        char b[512];
        snprintf(b, sizeof(b), ",{\"name\":\"%s\",\"rotation\":[%.9g,%.9g,%.9g,%.9g],\"translation\":[%.9g,%.9g,%.9g]", name.c_str(), n.q.x, n.q.y, n.q.z, n.q.w, n.t.x, n.t.y, n.t.z);
        nj += b;
        std::string ch; for (size_t j = 0; j < nodes.size(); ++j) if (nodes[j].parent == (int)k) ch += (ch.empty() ? "" : ",") + std::to_string(j + 1);
        for (size_t j = 0; j < gun.size(); ++j) if (gun[j].parent == -1 - (int)k) ch += (ch.empty() ? "" : ",") + std::to_string(nodes.size() + 1 + j);
        if (!ch.empty()) nj += ",\"children\":[" + ch + "]";
        nj += "}";
    }
    for (size_t k = 0; k < gun.size(); ++k) {
        const GunNode& g = gun[k];
        char b[512];
        snprintf(b, sizeof(b), ",{\"name\":\"%s\",\"rotation\":[%.9g,%.9g,%.9g,%.9g],\"translation\":[%.9g,%.9g,%.9g],\"extras\":{\"fc4gun\":1}", g.name.c_str(), g.q.x, g.q.y, g.q.z, g.q.w, g.t.x, g.t.y, g.t.z);
        nj += b;
        std::string ch; for (size_t j = 0; j < gun.size(); ++j) if (gun[j].parent == (int)k) ch += (ch.empty() ? "" : ",") + std::to_string(nodes.size() + 1 + j);
        if (!ch.empty()) nj += ",\"children\":[" + ch + "]";
        nj += "}";
    }
    // skin: inverse bind matrices of the rest pose (world, including the root's rotation)
    std::vector<std::array<double, 16>> W(nodes.size());
    auto mat = [](Q q, V t) {
        std::array<double, 16> m{};                      // column-major
        double x = q.x, y = q.y, z = q.z, w = q.w;
        m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y + z * w); m[2] = 2 * (x * z - y * w);
        m[4] = 2 * (x * y - z * w); m[5] = 1 - 2 * (x * x + z * z); m[6] = 2 * (y * z + x * w);
        m[8] = 2 * (x * z + y * w); m[9] = 2 * (y * z - x * w); m[10] = 1 - 2 * (x * x + y * y);
        m[12] = t.x; m[13] = t.y; m[14] = t.z; m[15] = 1; return m;
    };
    auto mul = [](const std::array<double, 16>& a, const std::array<double, 16>& b) {
        std::array<double, 16> r{};
        for (int col = 0; col < 4; ++col) for (int row = 0; row < 4; ++row) { double s = 0; for (int k = 0; k < 4; ++k) s += a[k * 4 + row] * b[col * 4 + k]; r[col * 4 + row] = s; }
        return r;
    };
    auto rootM = mat({ -0.70710678f, 0, 0, 0.70710678f }, { 0, 0, 0 });
    std::vector<float> ibm;
    for (size_t k = 0; k < nodes.size(); ++k) {
        auto L = mat(nodes[k].q, nodes[k].t);
        W[k] = mul(nodes[k].parent < 0 || nodes[k].parent >= (int)k ? rootM : W[nodes[k].parent], L);
        // inverse of a rigid transform: R^T, -R^T t
        const auto& m = W[k]; std::array<double, 16> inv{};
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) inv[j * 4 + i] = m[i * 4 + j];
        for (int i = 0; i < 3; ++i) inv[12 + i] = -(inv[0 + i] * m[12] + inv[4 + i] * m[13] + inv[8 + i] * m[14]);
        inv[15] = 1;
        for (double v : inv) ibm.push_back((float)v);
    }
    // the gun's parts are joints too (so Blender makes them bones of the same armature)
    {
        std::vector<std::array<double, 16>> GW(gun.size()); std::vector<int> done(gun.size(), 0);
        std::function<void(int)> world = [&](int j) {
            if (done[j]) return;
            done[j] = 1;
            auto L = mat(gun[j].q, gun[j].t);
            if (gun[j].parent < 0) GW[j] = mul(W[-1 - gun[j].parent], L);
            else { world(gun[j].parent); GW[j] = mul(GW[gun[j].parent], L); }
        };
        for (size_t j = 0; j < gun.size(); ++j) {
            world((int)j);
            const auto& m = GW[j]; std::array<double, 16> inv{};
            for (int i = 0; i < 3; ++i) for (int k = 0; k < 3; ++k) inv[k * 4 + i] = m[i * 4 + k];
            for (int i = 0; i < 3; ++i) inv[12 + i] = -(inv[0 + i] * m[12] + inv[4 + i] * m[13] + inv[8 + i] * m[14]);
            inv[15] = 1;
            for (double v : inv) ibm.push_back((float)v);
        }
    }
    int ibmAcc = acc(ibm, 16, "MAT4", false);
    // tagged files: key k sits exactly on Blender frame k (24 fps) so Blender never resamples it; FromGlb converts back
    // to the clip's real speed. (Untagged, the old way: real seconds.)
    float tfps = clipId ? 24.f : fps;
    std::vector<float> times(keys); for (int k = 0; k < keys; ++k) times[k] = k / tfps;
    int tAcc = acc(times, 1, "SCALAR", true);
    std::string samp, chan; int ns = 0;
    for (int t = 0; t < c.tracks; ++t) {
        if (tm[t] < 0) continue;
        int node = tm[t] + 1;
        if (!c.rot[t].empty()) {
            std::vector<float> v; Q prev{ 0, 0, 0, 1 };
            for (int k = 0; k < keys; ++k) {
                Q q = c.rot[t][c.rot[t].size() == 1 ? 0 : std::min(k, (int)c.rot[t].size() - 1)];
                if (additive) q = qmul(nodes[tm[t]].q, q);
                if (k && qdot(q, prev) < 0) q = { -q.x, -q.y, -q.z, -q.w };
                prev = q; v.insert(v.end(), { q.x, q.y, q.z, q.w });
            }
            int a = acc(v, 4, "VEC4", false);
            samp += (ns ? "," : "") + std::string("{\"input\":") + std::to_string(tAcc) + ",\"output\":" + std::to_string(a) + ",\"interpolation\":\"LINEAR\"}";
            chan += (ns ? "," : "") + std::string("{\"sampler\":") + std::to_string(ns) + ",\"target\":{\"node\":" + std::to_string(node) + ",\"path\":\"rotation\"}}";
            ++ns;
        }
        if (!c.pos[t].empty()) {
            std::vector<float> v;
            for (int k = 0; k < keys; ++k) { V p = c.pos[t][c.pos[t].size() == 1 ? 0 : std::min(k, (int)c.pos[t].size() - 1)]; v.insert(v.end(), { p.x, p.y, p.z }); }
            int a = acc(v, 3, "VEC3", false);
            samp += (ns ? "," : "") + std::string("{\"input\":") + std::to_string(tAcc) + ",\"output\":" + std::to_string(a) + ",\"interpolation\":\"LINEAR\"}";
            chan += (ns ? "," : "") + std::string("{\"sampler\":") + std::to_string(ns) + ",\"target\":{\"node\":" + std::to_string(node) + ",\"path\":\"translation\"}}";
            ++ns;
        }
    }
    if (haveGun) {
        int gkeys = gc.frames + 1;
        for (size_t j = 0; j < gun.size(); ++j) {
            int t = gun[j].track, node = (int)(nodes.size() + 1 + j);
            // the gun clip has its own key count; sample it onto the arms' keys
            auto at = [&](int k) { return gkeys <= 1 ? 0 : std::min(gkeys - 1, (int)lroundf((float)k * (gkeys - 1) / std::max(1, keys - 1))); };
            if (!gc.rot[t].empty()) {
                std::vector<float> v; Q prev{ 0, 0, 0, 1 };
                for (int k = 0; k < keys; ++k) {
                    Q q = gc.rot[t][gc.rot[t].size() == 1 ? 0 : std::min(at(k), (int)gc.rot[t].size() - 1)];
                    if (k && qdot(q, prev) < 0) q = { -q.x, -q.y, -q.z, -q.w };
                    prev = q; v.insert(v.end(), { q.x, q.y, q.z, q.w });
                }
                int a = acc(v, 4, "VEC4", false);
                samp += (ns ? "," : "") + std::string("{\"input\":") + std::to_string(tAcc) + ",\"output\":" + std::to_string(a) + ",\"interpolation\":\"LINEAR\"}";
                chan += (ns ? "," : "") + std::string("{\"sampler\":") + std::to_string(ns) + ",\"target\":{\"node\":" + std::to_string(node) + ",\"path\":\"rotation\"}}";
                ++ns;
            }
            if (!gc.pos[t].empty()) {
                std::vector<float> v;
                for (int k = 0; k < keys; ++k) { V p = gc.pos[t][gc.pos[t].size() == 1 ? 0 : std::min(at(k), (int)gc.pos[t].size() - 1)]; v.insert(v.end(), { p.x, p.y, p.z }); }
                int a = acc(v, 3, "VEC3", false);
                samp += (ns ? "," : "") + std::string("{\"input\":") + std::to_string(tAcc) + ",\"output\":" + std::to_string(a) + ",\"interpolation\":\"LINEAR\"}";
                chan += (ns ? "," : "") + std::string("{\"sampler\":") + std::to_string(ns) + ",\"target\":{\"node\":" + std::to_string(node) + ",\"path\":\"translation\"}}";
                ++ns;
            }
        }
    }
    // the clip id travels with the file (Blender keeps the animation name as the action name): FC4_<16 hex>
    char tagb[32]; snprintf(tagb, sizeof(tagb), "FC4_%016llx", (unsigned long long)clipId);
    std::string tag = clipId ? tagb : "clip";
    std::string joints; for (size_t k = 0; k < nodes.size() + gun.size(); ++k) joints += (k ? "," : "") + std::to_string(k + 1);
    std::string json = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"FC4Hook\",\"extras\":{\"fc4clip\":\"" + tag + "\"}},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[" + nj +
        "],\"skins\":[{\"name\":\"FC4_Skeleton\",\"joints\":[" + joints + "],\"inverseBindMatrices\":" + std::to_string(ibmAcc) + ",\"skeleton\":1}]" +
        ",\"animations\":[{\"name\":\"" + tag + "\",\"samplers\":[" + samp + "],\"channels\":[" + chan + "]}]" +
        ",\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}],\"bufferViews\":[" + bv + "],\"accessors\":[" + ac + "]}";
    while (json.size() % 4) json += ' ';
    while (bin.size() % 4) bin.push_back(0);
    glb.clear();
    put32(glb, 0x46546C67); put32(glb, 2); put32(glb, (uint32_t)(12 + 8 + json.size() + 8 + bin.size()));
    put32(glb, (uint32_t)json.size()); put32(glb, 0x4E4F534A); glb.insert(glb.end(), json.begin(), json.end());
    put32(glb, (uint32_t)bin.size()); put32(glb, 0x004E4942); glb.insert(glb.end(), bin.begin(), bin.end());
    char b[240]; snprintf(b, sizeof(b), "%d frames (%.2f s), %d of %d bones%s%s", c.frames, c.length, matched, c.tracks, additive ? ", additive layer (shown on the rest pose)" : "",
                          haveGun ? (", plus the gun's " + std::to_string(gun.size()) + " moving parts").c_str() : "");
    R = b;
    return true;
}

// The clip id a .glb was exported from: "FC4_<16 hex>" in an animation name (Blender may add a suffix).
bool GlbClipId(const Bytes& f, uint64_t* id) {
    *id = 0;
    std::string s(f.begin(), f.begin() + std::min<size_t>(f.size(), 1 << 22));
    for (size_t p = s.find("FC4_"); p != std::string::npos; p = s.find("FC4_", p + 4)) {
        if (p + 20 > s.size()) break;
        bool hex = true; for (int k = 0; k < 16 && hex; ++k) hex = isxdigit((unsigned char)s[p + 4 + k]) != 0;
        if (!hex || s.compare(p, 8, "FC4_ROOT") == 0) continue;
        *id = strtoull(s.substr(p + 4, 16).c_str(), nullptr, 16);
        return *id != 0;
    }
    return false;
}


// ------------------------------------------------------------------ .glb -> clip
struct GlbAnim { std::map<std::string, std::pair<std::vector<float>, std::vector<Q>>> rot; std::map<std::string, std::pair<std::vector<float>, std::vector<V>>> pos; float dur = 0; };

static bool ReadGlbAnim(const Bytes& f, GlbAnim& a, std::string* err) {
    if (f.size() < 20 || memcmp(f.data(), "glTF", 4) != 0) { *err = "not a .glb file (export from Blender as glTF Binary)"; return false; }
    size_t o = 12; std::string json; Bytes bin;
    while (o + 8 <= f.size()) {
        uint32_t len = r32(f, o), type = r32(f, o + 4);
        if (o + 8 + len > f.size()) break;
        if (type == 0x4E4F534A) json.assign((const char*)&f[o + 8], len);
        else if (type == 0x004E4942) bin.assign(f.begin() + o + 8, f.begin() + o + 8 + len);
        o += 8 + ((len + 3) & ~3u);
    }
    JP jp{ json.data(), json.data() + json.size() };
    J j = jp.val();
    if (jp.bad || j.t != J::OBJ) { *err = "the .glb's description couldn't be read"; return false; }
    if (!j["animations"].size()) { *err = "the .glb has no animation (export with Animation ticked)"; return false; }
    auto floats = [&](int ai, int comps, std::vector<float>& out) {
        const J& acc = j["accessors"][(size_t)ai];
        const J& view = j["bufferViews"][(size_t)acc["bufferView"].i(0)];
        if (acc["componentType"].i() != 5126) return false;
        size_t off = (size_t)view["byteOffset"].i(0) + (size_t)acc["byteOffset"].i(0), st = (size_t)view["byteStride"].i(0);
        if (!st) st = 4 * comps;
        size_t n = (size_t)acc["count"].i(0);
        if (n && off + (n - 1) * st + 4 * comps > bin.size()) return false;
        out.resize(n * comps);
        for (size_t k = 0; k < n; ++k) memcpy(&out[k * comps], &bin[off + k * st], 4 * comps);
        return true;
    };
    const J& an = j["animations"][(size_t)0];
    for (auto& ch : an["channels"].a) {
        const J& s = an["samplers"][(size_t)ch["sampler"].i(0)];
        std::string node = j["nodes"][(size_t)ch["target"]["node"].i(0)]["name"].s, path = ch["target"]["path"].s;
        std::vector<float> t, v;
        if (!floats(s["input"].i(), 1, t) || t.empty()) continue;
        a.dur = std::max(a.dur, t.back());
        if (path == "rotation" && floats(s["output"].i(), 4, v)) {
            std::vector<Q> q; for (size_t k = 0; k + 3 < v.size(); k += 4) q.push_back({ v[k], v[k + 1], v[k + 2], v[k + 3] });
            if (q.size() == t.size()) a.rot[node] = { t, q };
        } else if (path == "translation" && floats(s["output"].i(), 3, v)) {
            std::vector<V> p; for (size_t k = 0; k + 2 < v.size(); k += 3) p.push_back({ v[k], v[k + 1], v[k + 2] });
            if (p.size() == t.size()) a.pos[node] = { t, p };
        }
    }
    return true;
}
template <class T> static size_t Seg(const std::vector<float>& t, float x, float* f) {
    if (x <= t.front()) { *f = 0; return 0; }
    if (x >= t.back()) { *f = 0; return t.size() - 1; }
    size_t i = std::upper_bound(t.begin(), t.end(), x) - t.begin() - 1;
    *f = (x - t[i]) / (t[i + 1] - t[i]);
    if (*f < 1e-3f) *f = 0;                                  // on a key: take it exactly (no float noise)
    else if (*f > 1 - 1e-3f) { *f = 0; ++i; }
    return i;
}
static Q SampleQ(const std::pair<std::vector<float>, std::vector<Q>>& s, float x) {
    float f; size_t i = Seg<Q>(s.first, x, &f);
    Q a = s.second[i]; if (f == 0) return a;
    Q b = s.second[i + 1]; if (qdot(a, b) < 0) b = { -b.x, -b.y, -b.z, -b.w };
    Q q = { a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f, a.w + (b.w - a.w) * f };
    float n = sqrtf(qdot(q, q)); return { q.x / n, q.y / n, q.z / n, q.w / n };
}
static V SampleV(const std::pair<std::vector<float>, std::vector<V>>& s, float x) {
    float f; size_t i = Seg<V>(s.first, x, &f);
    V a = s.second[i]; if (f == 0) return a;
    V b = s.second[i + 1]; return { a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f };
}

static void EncComponent(BitW& w, const std::vector<float>& v, int bits) {
    float mn = *std::min_element(v.begin(), v.end()), mx = *std::max_element(v.begin(), v.end());
    int qmin = std::max(0, std::min(255, (int)floorf((mn + 1) * 127)));
    float m = qmin / 127.f - 1;
    int qr = std::max(1, std::min(255, (int)ceilf((mx - m) * 127.5f)));
    float rg = qr / 127.5f; uint32_t top = (1u << bits) - 1;
    w.put(qmin, 8); w.put(qr, 8);
    for (float x : v) w.put((uint32_t)std::max(0.f, std::min((float)top, roundf((x - m) / rg * top))), bits);
}
static uint32_t ConstU16(float v) { return (uint32_t)std::max(0.f, std::min(65535.f, roundf((v + 1) * 32767.5f))); }

static void EncRotBlock(BitW& w, const std::vector<Q>& keys, int bits) {
    int idx = 0; float best = -1;
    for (int i = 0; i < 4; ++i) { float s = 0; for (auto& q : keys) s += fabsf((&q.x)[i]); if (s > best) { best = s; idx = i; } }
    std::vector<float> cp[3];
    for (auto q : keys) {
        if ((&q.x)[idx] < 0) q = { -q.x, -q.y, -q.z, -q.w };      // the dropped one >= 0: no sign mask
        int j = 0; for (int i = 0; i < 4; ++i) if (i != idx) cp[j++].push_back((&q.x)[i]);
    }
    uint32_t hdr = (uint32_t)idx << 4;
    for (int k = 0; k < 3; ++k) { auto mm = std::minmax_element(cp[k].begin(), cp[k].end()); if (*mm.second - *mm.first < 2e-5f) hdr |= 1u << k; }
    w.put(hdr, 6);
    for (int k = 0; k < 3; ++k) { if ((hdr >> k) & 1) w.put(ConstU16(cp[k][0]), 16); else EncComponent(w, cp[k], bits); }
}
static void EncPosBlock(BitW& w, const std::vector<V>& keys, int bits, float scale) {
    std::vector<float> cp[3];
    for (auto& p : keys) { cp[0].push_back(p.x / scale); cp[1].push_back(p.y / scale); cp[2].push_back(p.z / scale); }
    uint32_t hdr = 0;
    for (int k = 0; k < 3; ++k) { auto mm = std::minmax_element(cp[k].begin(), cp[k].end()); if (*mm.second - *mm.first < 2e-5f) hdr |= 1u << k; }
    w.put(hdr, 3);
    for (int k = 0; k < 3; ++k) { if ((hdr >> k) & 1) w.put(ConstU16(cp[k][0]), 16); else EncComponent(w, cp[k], bits); }
}
static void EncConstQuat(Bytes& o, Q q) {
    int idx = 0; for (int i = 1; i < 4; ++i) if (fabsf((&q.x)[i]) > fabsf((&q.x)[idx])) idx = i;
    if ((&q.x)[idx] < 0) q = { -q.x, -q.y, -q.z, -q.w };
    uint16_t v[3]; int j = 0;
    for (int i = 0; i < 4; ++i) if (i != idx) v[j++] = (uint16_t)std::max(0.f, std::min(32767.f, roundf(((&q.x)[i] + 0.70710677f) / 4.315969e-05f)));
    static const int t0[4] = { 0, 1, 0, 1 }, t1[4] = { 0, 0, 1, 1 };
    v[0] |= (uint16_t)(t0[idx] << 15); v[1] |= (uint16_t)(t1[idx] << 15);
    for (int i = 0; i < 3; ++i) { o.push_back((uint8_t)v[i]); o.push_back((uint8_t)(v[i] >> 8)); }
}
template <class F> static Bytes AnimRegion(int keys, F block) {
    int nb = (keys + 7) / 8;
    std::vector<Bytes> bl; for (int b = 0; b < nb; ++b) bl.push_back(block(b, std::min(8, keys - 8 * b)).bytes());
    Bytes out; uint32_t o = 4 * (nb + 1);
    for (auto& b : bl) { put32(out, o); o += (uint32_t)b.size(); }
    put32(out, o);
    for (auto& b : bl) out.insert(out.end(), b.begin(), b.end());
    return out;
}

bool FromGlb(const Bytes& tmpl, const Bytes& glbFile, const Bytes& skel, Bytes& out, std::string* report, int bits = 16,
             const std::map<uint32_t, std::string>* nameOverride = nullptr, bool nested = true, bool allowNone = false) {
    std::string& R = *report;
    if (tmpl.size() < C + 0x50 || memcmp(&tmpl[C], "aNi", 3) != 0) { R = "the clip to replace isn't an animation clip"; return false; }
    if (r16(tmpl, C + 4) & 0x4000) { R = "the clip to replace uses a header layout FC4Hook doesn't write yet"; return false; }
    bool shortHdr = ShortHdr(tmpl);
    Clip orig; if (!Decode(tmpl, orig, &R)) return false;
    std::vector<Node> nodes; std::map<uint32_t, std::string> nameOf;
    if (nameOverride) nameOf = *nameOverride;
    else {
        if (!Skeleton(skel, nodes, &R)) return false;
        for (auto& n : nodes) nameOf[Crc32(n.name.data(), n.name.size())] = n.name;
    }
    GlbAnim ga; if (!ReadGlbAnim(glbFile, ga, &R)) return false;
    int ntr = orig.tracks;
    float fps = orig.length > 0 && orig.frames > 0 ? orig.frames / orig.length : 30.f;
    // a tagged .glb (FC4_<id>) has one key per Blender frame at 24 fps: game key k = Blender frame k
    uint64_t tagged = 0; GlbClipId(glbFile, &tagged);
    float gfps = tagged ? 24.f : fps;
    int frames = std::max(1, (int)lroundf(ga.dur * gfps)), keys = frames + 1;
    float length = frames / fps;
    std::vector<std::vector<Q>> rot(ntr); std::vector<std::vector<V>> pos(ntr);
    int fromGlb = 0;
    for (int t = 0; t < ntr; ++t) {
        auto nm = nameOf.find(orig.hashes[t]);
        std::string name = nm == nameOf.end() ? "" : nm->second;
        auto ir = ga.rot.find(name); auto ip = ga.pos.find(name);
        if (ir != ga.rot.end()) { ++fromGlb; for (int k = 0; k < keys; ++k) rot[t].push_back(SampleQ(ir->second, k / gfps)); }
        else if (!orig.rot[t].empty()) for (int k = 0; k < keys; ++k) rot[t].push_back(orig.rot[t][orig.rot[t].size() == 1 ? 0 : std::min(k, (int)orig.rot[t].size() - 1)]);
        if (orig.flags[t] & 0xc0) {
            if (ip != ga.pos.end()) for (int k = 0; k < keys; ++k) pos[t].push_back(SampleV(ip->second, k / gfps));
            else if (!orig.pos[t].empty()) for (int k = 0; k < keys; ++k) pos[t].push_back(orig.pos[t][orig.pos[t].size() == 1 ? 0 : std::min(k, (int)orig.pos[t].size() - 1)]);
        }
    }
    // additive layer (identity constant rotations): the .glb shows it on top of the rest pose (see ToGlb); take that back off
    int nc = 0, idn = 0;
    for (auto& r : orig.rot) if (r.size() == 1) { ++nc; if (fabsf(r[0].w) > 0.9999f) ++idn; }
    bool additive = nc > 0 && idn * 10 >= nc * 9;
    if (additive) {
        std::map<uint32_t, Q> restOf;
        for (auto& n : nodes) restOf[Crc32(n.name.data(), n.name.size())] = n.q;
        for (int t = 0; t < ntr; ++t) {
            auto nm = nameOf.find(orig.hashes[t]); auto rs = restOf.find(orig.hashes[t]);
            if (nm == nameOf.end() || rs == restOf.end() || !ga.rot.count(nm->second)) continue;
            Q inv = { -rs->second.x, -rs->second.y, -rs->second.z, rs->second.w };
            for (auto& q : rot[t]) q = qmul(inv, q);
        }
    }
    if (fromGlb == 0 && !allowNone) { R = "none of the .glb's bones match this clip's bones (use a .glb exported by FC4Hook from a game clip)"; return false; }
    // classify
    std::vector<uint8_t> flags(ntr, 0); std::vector<int> crot, arot, cpos, apos;
    for (int t = 0; t < ntr; ++t) {
        if (!rot[t].empty()) {
            // fixed if every key equals the first to well under the format's resolution (4e-5 per component)
            bool cst = true; Q r0 = rot[t][0];
            for (auto q : rot[t]) {
                if (qdot(q, r0) < 0) q = { -q.x, -q.y, -q.z, -q.w };
                if (fabsf(q.x - r0.x) > 1e-5f || fabsf(q.y - r0.y) > 1e-5f || fabsf(q.z - r0.z) > 1e-5f || fabsf(q.w - r0.w) > 1e-5f) { cst = false; break; }
            }
            if (cst) { flags[t] |= 0x30; crot.push_back(t); } else { flags[t] |= (uint8_t)(0x10 | (bits - 1)); arot.push_back(t); }
        }
        if (!pos[t].empty()) {
            bool cst = true; for (auto& p : pos[t]) if (fabsf(p.x - pos[t][0].x) > 1e-6f || fabsf(p.y - pos[t][0].y) > 1e-6f || fabsf(p.z - pos[t][0].z) > 1e-6f) { cst = false; break; }
            if (cst) { flags[t] |= 0xc0; cpos.push_back(t); } else { flags[t] |= 0x40; apos.push_back(t); }
        }
    }
    std::map<int, Bytes> reg;
    for (int t : crot) EncConstQuat(reg[0x28], rot[t][0]);
    bool compact = tmpl[C + 3] & 0x20;
    for (int t : cpos) {
        Bytes& b = reg[0x2a]; V p = pos[t][0];
        if (compact) for (float v : { p.x, p.y, p.z }) { int16_t s = (int16_t)std::max(-32768.f, std::min(32767.f, roundf(v * 16384))); b.push_back((uint8_t)s); b.push_back((uint8_t)(s >> 8)); }
        else for (float v : { p.x, p.y, p.z }) { uint32_t u; memcpy(&u, &v, 4); put32(b, u); }
    }
    size_t extra = Off(tmpl, 0x36, 0x45), oldSc = Off(tmpl, 0x2e, 0x41);
    std::vector<float> scales;
    if (extra && oldSc) reg[0x2e].insert(reg[0x2e].end(), tmpl.begin() + C + oldSc, tmpl.begin() + C + oldSc + 4);
    for (int t : apos) {
        float m = 1e-3f; for (auto& p : pos[t]) m = std::max({ m, fabsf(p.x), fabsf(p.y), fabsf(p.z) });
        uint32_t q24 = std::min(0xffffffu, (uint32_t)ceilf(m / 0.00029802325f) + 1);
        scales.push_back(q24 * 0.00029802325f); put32(reg[0x2e], (uint32_t)bits | (q24 << 8));
    }
    if (!arot.empty()) reg[0x30] = AnimRegion(keys, [&](int b, int nk) { BitW w; for (int t : arot) EncRotBlock(w, std::vector<Q>(rot[t].begin() + 8 * b, rot[t].begin() + 8 * b + nk), bits); return w; });
    if (!apos.empty()) reg[0x32] = AnimRegion(keys, [&](int b, int nk) { BitW w; for (size_t j = 0; j < apos.size(); ++j) EncPosBlock(w, std::vector<V>(pos[apos[j]].begin() + 8 * b, pos[apos[j]].begin() + 8 * b + nk), bits, scales[j]); return w; });
    // every section of the template, in file order; ours are rebuilt, the others copied and moved
    static const int F[10][2] = { { 0x28, 0x3e }, { 0x2a, 0x3f }, { 0x2c, 0x40 }, { 0x2e, 0x41 }, { 0x30, 0x42 }, { 0x32, 0x43 }, { 0x34, 0x44 }, { 0x36, 0x45 }, { 0x38, 0x46 }, { 0x3a, 0x47 } };
    struct Sec { size_t off; int field; };              // field: 0..9 = F[], 100 = flags (u32 +0x10), 101 = hashes (u32 +0x14)
    std::vector<Sec> secs;
    for (int i = 0; i < 10; ++i) { size_t o = Off(tmpl, F[i][0], F[i][1]); if (o) secs.push_back({ o, i }); }
    secs.push_back({ r32(tmpl, C + 0x10), 100 }); secs.push_back({ r32(tmpl, C + 0x14), 101 });
    std::sort(secs.begin(), secs.end(), [](const Sec& a, const Sec& b) { return a.off < b.off; });
    size_t clipEnd = tmpl.size() - C;
    Bytes body(tmpl.begin() + C, tmpl.begin() + C + secs[0].off);
    std::map<int, size_t> newOff; std::map<int, bool> produced;
    for (size_t k = 0; k < secs.size(); ++k) {
        size_t o = secs[k].off, end = k + 1 < secs.size() ? secs[k + 1].off : clipEnd;
        int f = secs[k].field;
        bool rebuilt = f < 10 && reg.count(F[f][0]);
        // keep a copied section where it sat relative to 16 bytes; if it wasn't 4-aligned itself (gun
        // clips can be), 4-aligning too can never be met - that looped forever (1.12.4 crash)
        if (rebuilt || o % 4 == 0) { while (body.size() % 4 || (!rebuilt && (body.size() - o) % 16)) body.push_back(0); }
        else { while ((body.size() - o) % 16) body.push_back(0); }
        size_t start = body.size();
        if (rebuilt) { const Bytes& r = reg[F[f][0]]; body.insert(body.end(), r.begin(), r.end()); produced[F[f][0]] = true; }
        else if (f == 100) { body.insert(body.end(), flags.begin(), flags.end()); body.insert(body.end(), tmpl.begin() + C + o + ntr, tmpl.begin() + C + end); }
        else if (f < 10 && (F[f][0] == 0x28 || F[f][0] == 0x2a || F[f][0] == 0x2e || F[f][0] == 0x30 || F[f][0] == 0x32)) { /* ours, now empty */ start = 0; }
        else body.insert(body.end(), tmpl.begin() + C + o, tmpl.begin() + C + end);
        newOff[f] = start;
    }
    for (int i = 0; i < 10; ++i) {
        int key = F[i][0];
        if (!reg.count(key) || produced[key] || reg[key].empty()) continue;
        while (body.size() % 4) body.push_back(0);
        newOff[i] = body.size(); body.insert(body.end(), reg[key].begin(), reg[key].end());
    }
    for (auto& no : newOff) {
        if (no.first == 100) memcpy(&body[0x10], &no.second, 4);
        else if (no.first == 101) memcpy(&body[0x14], &no.second, 4);
        else if (shortHdr) { if (no.second > 0xffff) { R = "the gun clip got too big for its short header"; return false; } uint16_t lo = (uint16_t)no.second; memcpy(&body[F[no.first][0] - 6], &lo, 2); }
        else { uint16_t lo = (uint16_t)no.second; memcpy(&body[F[no.first][0]], &lo, 2); body[F[no.first][1]] = (uint8_t)(no.second >> 16); }
    }
    auto w16 = [&](size_t o, uint16_t v) { memcpy(&body[o], &v, 2); };
    w16(6, (uint16_t)frames); memcpy(&body[8], &length, 4);
    w16(0x18, (uint16_t)crot.size()); w16(0x1a, (uint16_t)cpos.size()); w16(0x1c, (uint16_t)arot.size()); w16(0x1e, (uint16_t)apos.size());
    w16(0x20, (uint16_t)(scales.size() + (extra && oldSc ? 1 : 0)));
    out.assign(tmpl.begin(), tmpl.begin() + C); out.insert(out.end(), body.begin(), body.end());
    // check: our own decoder must read it back
    Clip chk; std::string e2;
    if (!Decode(out, chk, &e2) || chk.frames != frames) { R = "internal check failed: " + e2; return false; }
    char b[240];
    snprintf(b, sizeof(b), "%d frames (%.2f s), %d bones from the .glb, %zu animated rotations, %zu animated positions", frames, length, fromGlb, arot.size(), apos.size());
    R = b;
    if (nested) {
        // the gun's own animation: if the .glb animates the gun's bones (exported with the clip as
        // CLIP, SLIDE... / gun_<hash>), rewrite nested clip 0 from them; the section is rebuilt and
        // everything after it moved (by a multiple of 16)
        Nested ns;
        if (ParseNested(out, ns) && !ns.e.empty()) {
            std::map<uint32_t, std::string> gnames;
            for (auto& kv : ga.rot) gnames[Crc32(kv.first.data(), kv.first.size())] = kv.first;
            for (auto& kv : ga.pos) gnames[Crc32(kv.first.data(), kv.first.size())] = kv.first;
            for (auto& kv : std::map<uint32_t, std::string>(gnames)) {     // gun_<hash> names stand for that hash
                if (kv.second.size() == 12 && kv.second.compare(0, 4, "gun_") == 0) gnames[(uint32_t)strtoul(kv.second.c_str() + 4, nullptr, 16)] = kv.second;
            }
            // every nested clip is rewritten to the new length (entry 0 from the .glb's gun bones; the others keep
            // their own keys, stretched); each new clip goes at the end of the file and its entry points at it
            Bytes n2 = out; std::string nr0; bool okAll = true;
            for (size_t k = 0; k < ns.e.size(); ++k) {
                Bytes nf = NestedAsFile(out, ns.e[k]), nout; std::string nr;
                if (!FromGlb(nf, glbFile, skel, nout, &nr, bits, &gnames, false, true)) {
                    if (k == 0) { okAll = false; R += "; (gun parts not written: " + nr + ")"; break; }
                    continue;                                   // a layout we can't write yet: that one stays as it was
                }
                if (k == 0) nr0 = nr;
                while ((n2.size() - C) % 16) n2.push_back(0);
                size_t at = n2.size();
                n2.insert(n2.end(), nout.begin() + C, nout.end());
                uint32_t rel = (uint32_t)(at - ns.e[k].entry);
                memcpy(&n2[ns.e[k].entry + 8], &rel, 4);
            }
            if (okAll) {
                Clip chk2; std::string e3; Nested ns2; bool good = Decode(n2, chk2, &e3) && ParseNested(n2, ns2);
                { Clip g; good = good && !ns2.e.empty() && Decode(NestedAsFile(n2, ns2.e[0]), g, &e3) && g.frames == frames; }
                if (good) { out.swap(n2); R += "; gun parts: " + nr0; }
                else R += "; (gun parts not written: " + e3 + ")";
            }
        }
    }
    return true;
}

} // namespace ani

bool AniClipToGlb(const Bytes& clip, const Bytes& skel, Bytes& glb, std::string* report, uint64_t clipId, const Bytes* gunSkel) { return ani::ToGlb(clip, skel, glb, report, clipId, gunSkel); }
// the weapon package a weapon clip's gun animation belongs to (e.g. kimber), "" if none
std::string AniClipWeapon(const Bytes& clip) { ani::Nested n; return ani::ParseNested(clip, n) && !n.e.empty() ? n.e[0].pkg : std::string(); }
bool AniGlbClipId(const Bytes& glb, uint64_t* id) { return ani::GlbClipId(glb, id); }
bool AniClipFromGlb(const Bytes& tmpl, const Bytes& glb, const Bytes& skel, Bytes& out, std::string* report) { return ani::FromGlb(tmpl, glb, skel, out, report); }

} // namespace fc4
