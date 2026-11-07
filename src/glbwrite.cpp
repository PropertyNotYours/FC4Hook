// .glb (glTF 2.0 binary) -> Far Cry 4 .xbg, for COMPLETELY new weapon models.
// Plain C++ (no Windows), used by fc4hook.asi.
//
// A weapon model isn't only triangles: it has the bones the weapon's animations move (FRAME,
// CLIP, SLIDE...), skin clusters, LODs and materials. Writing all that from nothing would mean
// guessing at a lot of the format, so the new model is built ON the base weapon's model:
// everything (skeleton, nodes, bones, effects points) stays the game's, and only the geometry
// and materials are replaced.
//
// Layout used (FC4 1.10, "HSEM" 0x37), learned from desert_eagle.xbg:
//   RMTL  u32 count, u32 0, then per material: u32 len, path\0, u32 len, name\0
//   SKND > CLUS  per PART (FRAME_LOD0, CLIP_LOD0...): u32 n, n x 174-byte entries (see XbgFromGlb)
//                entry: u16 bone, u16 tris, u16 tris, u16 indices, u16 stride, u16 vertices,
//                u16 vertex format, 96 bytes (ff), u32 vertices + 28 zero, u32 indices + 28 zero
//   LODS  u32 lods(1) float dist, u32 formats, formats x (flags, stride, count, byteOffset),
//         u32 submeshes, submeshes x (format, material, entry-in-material, firstIndex,
//         lastVertex, vertexByteOffset, 0), u32 vbytes, 04 03 02 01, vertices,
//         u32 indices, 0c 0b .. 01, u16 indices (global within their vertex format)
//   vertex 0xCA  (20 B): int16 pos[3], int16 1, int16 uv[2], ubyte4 normal, ubyte4 colour
//   vertex 0xBCA (32 B): int16 pos[3], int16 1, uv0, uv1, normal, colour, tangent, binormal
//   PCMP pos = int16 * scale + offset;  UCMP uv = int16 * scale + offset
//   triangles are clockwise; glTF is counter-clockwise. glTF (x, y, z) = game (x, -z, y).
//
// The new geometry goes to the FRAME part of every LOD (so it moves with the gun), one glTF
// material per piece of that part (extra ones share an atlas). Every
// other piece is emptied. Each used piece gets its own material (a copy of a plain
// diffuse/normal/specular material of the base model) pointing at the glTF's own textures.
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <algorithm>
#include "minijson.h"

namespace fc4 {

typedef std::vector<uint8_t> Bytes;
bool ImageToXbt(const Bytes& img, char kind, Bytes& out, std::string* err);
bool ImageToXbtSplit(const Bytes& img, char kind, const std::string& mip0Path, Bytes& mainOut, Bytes& mip0Out, std::string* err);
std::string Mip0Name(const std::string& xbt);
bool AtlasXbtSplit(const std::vector<Bytes>& imgs, const std::string& mip0Path, Bytes& mainOut, Bytes& mip0Out, std::string* err);
struct TexSrc { Bytes colour, normal, mr; float rough = 0.5f, metal = 0.f; bool gameMask = false; };
bool TextureSetXbt(const std::vector<TexSrc>& src, const std::string pathD, const std::string pathN, const std::string pathS, Bytes out[3][2], std::string* err);
void FlatXbt(char kind, uint8_t level, Bytes& out);
bool MaterialWithTextures(const Bytes& mat, const std::vector<std::pair<std::string, std::string>>& swaps, Bytes& out, std::string* err);
std::vector<std::string> MaterialTextures(const Bytes& mat);

// tiny JSON reader: minijson.h
// ------------------------------------------------------------------ helpers
static uint32_t rd32(const Bytes& d, size_t o) { uint32_t v = 0; if (o + 4 <= d.size()) memcpy(&v, &d[o], 4); return v; }
static uint16_t rd16(const Bytes& d, size_t o) { uint16_t v = 0; if (o + 2 <= d.size()) memcpy(&v, &d[o], 2); return v; }
static float rdf(const Bytes& d, size_t o) { float v = 0; if (o + 4 <= d.size()) memcpy(&v, &d[o], 4); return v; }
static void w32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
static void w16(Bytes& b, uint16_t v) { b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); }
static void p32(Bytes& b, size_t o, uint32_t v) { memcpy(&b[o], &v, 4); }
static void p16(Bytes& b, size_t o, uint16_t v) { memcpy(&b[o], &v, 2); }

struct Chunk { std::string tag; size_t off = 0; uint32_t size = 0, dataSize = 0, kids = 0; };
static void TopChunks(const Bytes& d, size_t off, size_t end, std::vector<Chunk>& out) {
    while (off + 20 <= end) {
        Chunk c;
        char t[5] = { (char)d[off + 3], (char)d[off + 2], (char)d[off + 1], (char)d[off], 0 };
        c.tag = t; c.off = off; c.size = rd32(d, off + 8); c.dataSize = rd32(d, off + 12); c.kids = rd32(d, off + 16);
        if (c.size < 20 || off + c.size > end || 20 + (size_t)c.dataSize > c.size) return;   // trailing bytes (some models, e.g. the Kimber, have some): kept as they are
        out.push_back(c);
        off += c.size;
    }
}

struct V3 { float x, y, z; };
static V3 sub(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static V3 cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 norm(V3 a) { float l = sqrtf(dot(a, a)); return l > 1e-12f ? V3{ a.x / l, a.y / l, a.z / l } : V3{ 0, 0, 1 }; }
static uint8_t snorm8(float v) { long q = lroundf(v * 127.f + 128.f); return (uint8_t)(q < 0 ? 0 : q > 255 ? 255 : q); }

// one glTF primitive, already in game space
struct Prim { std::vector<V3> pos, nrm; std::vector<float> uv; std::vector<uint32_t> idx; int material = -1; std::string name; };

// ------------------------------------------------------------------ glTF reading
struct Gltf {
    J j; Bytes bin; const Bytes* file = nullptr; std::string err;
    bool view(int bv, const uint8_t** p, size_t* n, size_t* stride) {
        const J& v = j["bufferViews"][(size_t)bv];
        if (v.t != J::OBJ || v["buffer"].i(0) != 0) { err = "a buffer view isn't in the .glb (only single-file .glb is supported; export as glTF Binary)"; return false; }
        size_t o = (size_t)v["byteOffset"].i(0), len = (size_t)v["byteLength"].i(0);
        if (o + len > bin.size()) { err = "damaged .glb (buffer view out of range)"; return false; }
        *p = bin.data() + o; *n = len; if (stride) *stride = (size_t)v["byteStride"].i(0);
        return true;
    }
    // accessor -> floats (comps per element), handles float and normalised ints
    bool floats(int ai, int comps, std::vector<float>& out) {
        const J& a = j["accessors"][(size_t)ai];
        if (a.t != J::OBJ || !a.has("bufferView")) { err = "missing accessor"; return false; }
        const uint8_t* p; size_t n, st;
        if (!view(a["bufferView"].i(), &p, &n, &st)) return false;
        int ct = a["componentType"].i(); bool nz = a["normalized"].t == J::BOOL && a["normalized"].n;
        size_t cs = ct == 5126 ? 4 : (ct == 5123 || ct == 5122) ? 2 : 1;
        if (!st) st = cs * comps;
        size_t cnt = (size_t)a["count"].i(0), o = (size_t)a["byteOffset"].i(0);
        if (cnt && o + (cnt - 1) * st + cs * comps > n) { err = "damaged .glb (accessor out of range)"; return false; }
        out.resize(cnt * comps);
        for (size_t e = 0; e < cnt; ++e) for (int c = 0; c < comps; ++c) {
            const uint8_t* q = p + o + e * st + c * cs; float v = 0;
            if (ct == 5126) memcpy(&v, q, 4);
            else if (ct == 5123) { uint16_t u; memcpy(&u, q, 2); v = nz ? u / 65535.f : u; }
            else if (ct == 5122) { int16_t s; memcpy(&s, q, 2); v = nz ? std::max(s / 32767.f, -1.f) : s; }
            else if (ct == 5121) { v = nz ? *q / 255.f : *q; }
            else if (ct == 5120) { int8_t s = (int8_t)*q; v = nz ? std::max(s / 127.f, -1.f) : s; }
            else { err = "unsupported accessor type"; return false; }
            out[e * comps + c] = v;
        }
        return true;
    }
    bool ints(int ai, std::vector<uint32_t>& out) {
        const J& a = j["accessors"][(size_t)ai];
        const uint8_t* p; size_t n, st;
        if (a.t != J::OBJ || !view(a["bufferView"].i(), &p, &n, &st)) { if (err.empty()) err = "missing index accessor"; return false; }
        int ct = a["componentType"].i(); size_t cs = ct == 5125 ? 4 : ct == 5123 ? 2 : 1;
        if (!st) st = cs;
        size_t cnt = (size_t)a["count"].i(0), o = (size_t)a["byteOffset"].i(0);
        if (cnt && o + (cnt - 1) * st + cs > n) { err = "damaged .glb (index accessor out of range)"; return false; }
        out.resize(cnt);
        for (size_t e = 0; e < cnt; ++e) { const uint8_t* q = p + o + e * st; uint32_t v = 0; memcpy(&v, q, cs); out[e] = v; }
        return true;
    }
    bool image(int texIndex, Bytes& out) {
        const J& t = j["textures"][(size_t)texIndex];
        int src = t["source"].i();
        const J& im = j["images"][(size_t)src];
        if (im.t != J::OBJ || !im.has("bufferView")) return false;
        const uint8_t* p; size_t n;
        if (!view(im["bufferView"].i(), &p, &n, nullptr)) return false;
        out.assign(p, p + n); return true;
    }
};

typedef float M4[16];   // column-major, like glTF
static void Mul(const M4 a, const M4 b, M4 r) { M4 t; for (int c = 0; c < 4; ++c) for (int rr = 0; rr < 4; ++rr) { float s = 0; for (int k = 0; k < 4; ++k) s += a[k * 4 + rr] * b[c * 4 + k]; t[c * 4 + rr] = s; } memcpy(r, t, sizeof(t)); }
static void NodeMatrix(const J& n, M4 m) {
    static const M4 I = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    memcpy(m, I, sizeof(M4));
    if (n["matrix"].size() == 16) { for (int k = 0; k < 16; ++k) m[k] = (float)n["matrix"][k].n; return; }
    float t[3] = { 0, 0, 0 }, s[3] = { 1, 1, 1 }, q[4] = { 0, 0, 0, 1 };
    if (n["translation"].size() == 3) for (int k = 0; k < 3; ++k) t[k] = (float)n["translation"][k].n;
    if (n["scale"].size() == 3) for (int k = 0; k < 3; ++k) s[k] = (float)n["scale"][k].n;
    if (n["rotation"].size() == 4) for (int k = 0; k < 4; ++k) q[k] = (float)n["rotation"][k].n;
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float R[9] = { 1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
                   2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                   2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y) };
    for (int c = 0; c < 3; ++c) for (int rr = 0; rr < 3; ++rr) m[c * 4 + rr] = R[c * 3 + rr] * s[c];
    m[12] = t[0]; m[13] = t[1]; m[14] = t[2];
}

static bool LoadGlb(const Bytes& f, Gltf& g, std::vector<Prim>& prims, std::string* err) {
    if (f.size() < 20 || memcmp(f.data(), "glTF", 4) != 0) { *err = "not a .glb file (export from Blender as glTF Binary, .glb)"; return false; }
    size_t o = 12; std::string json;
    while (o + 8 <= f.size()) {
        uint32_t len = rd32(f, o), type = rd32(f, o + 4);
        if (o + 8 + len > f.size()) break;
        if (type == 0x4E4F534A) json.assign((const char*)&f[o + 8], len);
        else if (type == 0x004E4942) g.bin.assign(f.begin() + o + 8, f.begin() + o + 8 + len);
        o += 8 + ((len + 3) & ~3u);
    }
    JP jp{ json.data(), json.data() + json.size() };
    g.j = jp.val();
    if (jp.bad || g.j.t != J::OBJ) { *err = "the .glb's description (JSON) couldn't be read"; return false; }
    if (g.j["skins"].size()) { /* skinned exports are fine: the armature is ignored, the mesh is used as posed */ }
    // walk the scene's nodes with their transforms
    std::vector<std::pair<int, std::vector<float>>> todo;
    const J& sc = g.j["scenes"][(size_t)g.j["scene"].i(0)];
    std::vector<float> I = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    if (sc.t == J::OBJ) for (auto& n : sc["nodes"].a) todo.push_back({ n.i(), I });
    else for (size_t k = 0; k < g.j["nodes"].size(); ++k) todo.push_back({ (int)k, I });
    int guard = 0;
    while (!todo.empty() && ++guard < 10000) {
        auto cur = todo.back(); todo.pop_back();
        const J& n = g.j["nodes"][(size_t)cur.first];
        if (n.t != J::OBJ) continue;
        M4 local, world; NodeMatrix(n, local); Mul(cur.second.data(), local, world);
        for (auto& c : n["children"].a) todo.push_back({ c.i(), std::vector<float>(world, world + 16) });
        if (!n.has("mesh")) continue;
        const J& mesh = g.j["meshes"][(size_t)n["mesh"].i()];
        // normals use the inverse-transpose; for the usual rotate/scale/translate exports the
        // upper 3x3 then renormalise is close enough (uniform scale)
        for (auto& pr : mesh["primitives"].a) {
            if (pr["mode"].i(4) != 4) continue;        // triangles only
            const J& at = pr["attributes"];
            if (!at.has("POSITION")) continue;
            Prim P; P.material = pr["material"].i(); P.name = n["name"].s;
            std::vector<float> pos, nr, uv;
            if (!g.floats(at["POSITION"].i(), 3, pos)) { *err = g.err; return false; }
            if (at.has("NORMAL") && !g.floats(at["NORMAL"].i(), 3, nr)) { *err = g.err; return false; }
            if (at.has("TEXCOORD_0") && !g.floats(at["TEXCOORD_0"].i(), 2, uv)) { *err = g.err; return false; }
            size_t nv = pos.size() / 3;
            if (pr.has("indices")) { if (!g.ints(pr["indices"].i(), P.idx)) { *err = g.err; return false; } }
            else for (size_t k = 0; k < nv; ++k) P.idx.push_back((uint32_t)k);
            for (auto ix : P.idx) if (ix >= nv) { *err = "damaged .glb (index past the vertices)"; return false; }
            for (size_t k = 0; k < nv; ++k) {
                float x = pos[k * 3], y = pos[k * 3 + 1], z = pos[k * 3 + 2];
                float wx = world[0] * x + world[4] * y + world[8] * z + world[12];
                float wy = world[1] * x + world[5] * y + world[9] * z + world[13];
                float wz = world[2] * x + world[6] * y + world[10] * z + world[14];
                P.pos.push_back({ wx, -wz, wy });                                   // glTF -> game axes
                V3 nn = { 0, 0, 1 };
                if (!nr.empty()) {
                    float a = nr[k * 3], b = nr[k * 3 + 1], c = nr[k * 3 + 2];
                    V3 t = { world[0] * a + world[4] * b + world[8] * c, world[1] * a + world[5] * b + world[9] * c, world[2] * a + world[6] * b + world[10] * c };
                    nn = norm({ t.x, -t.z, t.y });
                }
                P.nrm.push_back(nn);
                P.uv.push_back(uv.empty() ? 0.f : uv[k * 2]); P.uv.push_back(uv.empty() ? 0.f : uv[k * 2 + 1]);
            }
            if (nr.empty()) {                                   // no normals exported: flat-ish from the faces
                std::vector<V3> acc(nv, { 0, 0, 0 });
                for (size_t t = 0; t + 2 < P.idx.size(); t += 3) {
                    V3 a = P.pos[P.idx[t]], b = P.pos[P.idx[t + 1]], c = P.pos[P.idx[t + 2]];
                    V3 f2 = cross(sub(b, a), sub(c, a));
                    for (int k = 0; k < 3; ++k) { V3& q = acc[P.idx[t + k]]; q.x += f2.x; q.y += f2.y; q.z += f2.z; }
                }
                for (size_t k = 0; k < nv; ++k) P.nrm[k] = norm(acc[k]);
            }
            if (!P.idx.empty()) prims.push_back(std::move(P));
        }
    }
    if (prims.empty()) { *err = "the .glb has no triangle meshes"; return false; }
    return true;
}

// ------------------------------------------------------------------ the writer
struct GlbFile { std::string path; Bytes data; };
struct GlbOptions { float scale = 1.f; float move[3] = { 0, 0, 0 }; };   // move: game metres (x right, y forward, z up)

struct Rec { uint32_t fmt, part, ent, first, last, vbo, z; };
struct Fmt { uint32_t flags, stride, count, off; };
struct Lod { float dist = 0; std::vector<Fmt> fmts; std::vector<Rec> recs; std::vector<uint32_t> idxCount; };

// What the parts mean (from desert_eagle.xbg, desert_eagle_tp.xbg, kimber.xbg):
//  * SKND > CLUS holds the model's PARTS in order (FRAME_LOD0, CLIP_LOD0, ..., FRAME_LOD1, ...; their names
//    are in SKND's own data). Each part: u32 n, n x 174-byte entries, one per material it uses;
//    entry +0 u16 = the RMTL material index, then tris, tris, indices, stride, vertices, format,
//    96 bytes (ff), u32 vertices + 28 zero, u32 indices + 28 zero.
//  * a LODS submesh = (format, part, entry in part, firstIndex, lastVertex, vertexByteOffset, 0)
//  * parts are moved by the animations (FRAME = the gun, CLIP = magazine, SLIDE...). New geometry goes
//    to each LOD's FRAME part; the other parts are emptied.
// vertex layouts FC4Hook writes: plain (0xCA 20 B, 0xBCA 32 B) and skinned (0x10 set: 0xDA 28 B, 0xBDA 40 B)
static bool VtxKnown(uint32_t f, uint32_t stride) { return (f == 0xCA && stride == 20) || (f == 0xBCA && stride == 32) || (f == 0xDA && stride == 28) || (f == 0xBDA && stride == 40); }
static bool VtxTangents(uint32_t f) { return (f & 0xF00) == 0xB00; }
// A skinned vertex's bone index points into its piece's bone palette: 48 u16 NODE numbers at entry +14
// (0xffff = unused). FC4Hook puts every vertex of a piece it fills on slot 0 and sets slot 0 to the
// part's own node (the PKM's main piece lists PKM_Frame first, but others start with the feed cover,
// the ammo box or a belt bullet, which the reload moves).

bool XbgFromGlb(const Bytes& tmpl, const Bytes& glbFile, const std::string& dir, const std::string& stem,
                const std::function<bool(const std::string&, Bytes&)>& readGame,
                Bytes& out, std::vector<GlbFile>& files, std::string* report, const GlbOptions& opt) {
    std::string& R = *report;
    files.clear();
    if (tmpl.size() < 64 || memcmp(tmpl.data(), "HSEM", 4) != 0) { R = "the base model isn't an .xbg"; return false; }
    std::vector<Chunk> cs;
    TopChunks(tmpl, 32, tmpl.size(), cs);
    const Chunk *rmtl = nullptr, *sknd = nullptr, *lods = nullptr, *pcmp = nullptr, *ucmp = nullptr;
    for (auto& c : cs) {
        if (c.tag == "RMTL") rmtl = &c; else if (c.tag == "SKND") sknd = &c; else if (c.tag == "LODS") lods = &c;
        else if (c.tag == "PCMP") pcmp = &c; else if (c.tag == "UCMP") ucmp = &c;
    }
    if (!rmtl || !sknd || !lods || !pcmp || !ucmp) { R = "the base model has an unsupported layout (needs RMTL/SKND/LODS/PCMP/UCMP)"; return false; }

    // --- materials
    size_t p = rmtl->off + 20;
    uint32_t nMat = rd32(tmpl, p); p += 8;
    if (nMat > 256) { R = "damaged RMTL"; return false; }
    std::vector<std::string> matPath(nMat), matName(nMat);
    for (uint32_t m = 0; m < nMat; ++m) {
        uint32_t l = rd32(tmpl, p); if (p + 4 + l + 1 > tmpl.size()) { R = "damaged RMTL"; return false; }
        matPath[m].assign((const char*)&tmpl[p + 4], l); p += 4 + l + 1;
        l = rd32(tmpl, p); if (p + 4 + l + 1 > tmpl.size()) { R = "damaged RMTL"; return false; }
        matName[m].assign((const char*)&tmpl[p + 4], l); p += 4 + l + 1;
    }
    if (p != rmtl->off + 20 + rmtl->dataSize) { R = "unexpected material list layout in the base model"; return false; }

    // --- parts (SKND > CLUS) and their names (SKND data)
    size_t clus = sknd->off + 20;                        // children come before a chunk's own data
    if (memcmp(&tmpl[clus], "SULC", 4) != 0) { R = "unexpected skin layout in the base model"; return false; }
    size_t c0 = clus + 20, cq = c0, cEnd = c0 + rd32(tmpl, clus + 12);
    std::vector<std::vector<size_t>> ent;                // entry offsets per part
    while (cq < cEnd) {
        uint32_t n = rd32(tmpl, cq); cq += 4;
        if (n > 64 || ent.size() > 256) { R = "unexpected skin cluster layout in the base model"; return false; }
        ent.emplace_back();
        for (uint32_t k = 0; k < n; ++k) { ent.back().push_back(cq); cq += 174; }
    }
    if (cq != cEnd) { R = "unexpected skin cluster size in the base model"; return false; }
    std::vector<std::string> partName(ent.size());
    {
        size_t a = clus + rd32(tmpl, clus + 8), b = sknd->off + sknd->size;
        size_t k = 0; std::string cur;
        for (size_t i = a; i < b && k < partName.size(); ++i) {
            char ch = (char)tmpl[i];
            if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || (ch >= 'a' && ch <= 'z')) { cur += ch; continue; }
            if (ch == 0 && cur.size() >= 4 && cur.find("_LOD") != std::string::npos) partName[k++] = cur;
            cur.clear();
        }
    }

    // --- bones: each part's vertices are stored relative to its bone (NODE), e.g. the Kimber's FRAME
    // bone sits 7 cm back from the origin. NODE: u32 count, per node: u32 parentNameHash, i32
    // firstChild, i32 nextSibling, i32 parent, quat xyzw, pos xyz, scale xyz, i32, f32, u32, u32 len,
    // name\0. SKID: u32 count, per bone: u32 hash, u16 index, u16 node, u32. Part k uses bone k.
    struct Xf { float q[4] = { 0, 0, 0, 1 }; V3 t = { 0, 0, 0 }; };
    auto qrot = [](const float q[4], V3 v) {                 // rotate v by unit quaternion q (x, y, z, w)
        V3 u = { q[0], q[1], q[2] }; float w = q[3];
        V3 c1 = cross(u, v); V3 c2 = cross(u, c1);
        return V3{ v.x + 2 * (w * c1.x + c2.x), v.y + 2 * (w * c1.y + c2.y), v.z + 2 * (w * c1.z + c2.z) };
    };
    std::vector<Xf> partXf(ent.size());
    std::vector<int> partNode(ent.size(), -1);           // the part's node (NODE index), for skinned palettes
    std::vector<Xf> nodeWorld, nodeLocal; std::vector<std::string> nodeNames; std::vector<size_t> nodeOffs; std::vector<int> nodeParent;
    {
        const Chunk *node = nullptr, *skid = nullptr;
        for (auto& c : cs) { if (c.tag == "NODE") node = &c; else if (c.tag == "SKID") skid = &c; }
        std::vector<Xf> world; std::vector<std::string> nodeName;
        if (node) {
            size_t a = node->off + 20, e = a + node->dataSize;
            uint32_t n = rd32(tmpl, a); a += 4;
            std::vector<int> parent; std::vector<Xf> local;
            for (uint32_t k = 0; k < n && k < 512 && a + 72 <= e; ++k) {
                nodeOffs.push_back(a);
                parent.push_back((int32_t)rd32(tmpl, a + 12));
                Xf x; for (int j = 0; j < 4; ++j) x.q[j] = rdf(tmpl, a + 16 + 4 * j);
                x.t = { rdf(tmpl, a + 32), rdf(tmpl, a + 36), rdf(tmpl, a + 40) };
                local.push_back(x);
                uint32_t ln = rd32(tmpl, a + 68);
                nodeName.push_back(a + 72 + ln <= e ? std::string((const char*)&tmpl[a + 72], ln) : "");
                a += 72 + ln + 1;
            }
            world.resize(local.size());
            for (size_t k = 0; k < local.size(); ++k) {         // parents come first
                int pa = parent[k];
                if (pa < 0 || pa >= (int)k) { world[k] = local[k]; continue; }
                const Xf& P = world[pa]; Xf w;
                V3 rt = qrot(P.q, local[k].t); w.t = { P.t.x + rt.x, P.t.y + rt.y, P.t.z + rt.z };
                const float* a1 = P.q; const float* b1 = local[k].q;      // q = P.q * local.q
                w.q[0] = a1[3] * b1[0] + a1[0] * b1[3] + a1[1] * b1[2] - a1[2] * b1[1];
                w.q[1] = a1[3] * b1[1] - a1[0] * b1[2] + a1[1] * b1[3] + a1[2] * b1[0];
                w.q[2] = a1[3] * b1[2] + a1[0] * b1[1] - a1[1] * b1[0] + a1[2] * b1[3];
                w.q[3] = a1[3] * b1[3] - a1[0] * b1[0] - a1[1] * b1[1] - a1[2] * b1[2];
                world[k] = w;
            }
            nodeWorld = world; nodeLocal = local; nodeNames = nodeName; nodeParent = parent;
        }
        std::vector<int> boneNode;
        if (skid) { uint32_t n = rd32(tmpl, skid->off + 20); for (uint32_t k = 0; k < n && k < 512; ++k) boneNode.push_back(rd16(tmpl, skid->off + 24 + 12 * k + 6)); }
        for (size_t pidx = 0; pidx < ent.size(); ++pidx) {
            int nd = -1;
            if (boneNode.size() == ent.size() && boneNode[pidx] < nodeName.size()) nd = boneNode[pidx];
            else {                                            // by name: "CLIP_LOD0" -> node "CLIP"
                std::string pn = partName[pidx].substr(0, partName[pidx].rfind("_LOD"));
                for (size_t k = 0; k < nodeName.size(); ++k) {
                    std::string a2 = nodeName[k], b2 = pn;
                    for (auto& c : a2) c = (char)toupper((unsigned char)c);
                    if (a2 == b2) { nd = (int)k; break; }
                }
            }
            if (nd >= 0 && nd < (int)world.size()) { partXf[pidx] = world[nd]; partNode[pidx] = nd; }
        }
    }

    // --- LODS
    size_t L = lods->off + 20, LE = L + lods->dataSize;
    uint32_t nLod = rd32(tmpl, L);
    if (nLod == 0 || nLod > 8) { R = "unexpected LOD count in the base model"; return false; }
    // padding up to the next 16-byte boundary of the file: k bytes k, k-1 .. 1 (none when already aligned)
    auto padOk = [&](size_t at) { int k = (int)((16 - at % 16) % 16); if (at + k > tmpl.size()) return -1; for (int i = 0; i < k; ++i) if (tmpl[at + i] != k - i) return -1; return k; };
    std::vector<Lod> lodv(nLod);
    size_t q = L + 4;
    for (auto& lo : lodv) {
        lo.dist = rdf(tmpl, q); uint32_t nf = rd32(tmpl, q + 4); q += 8;
        if (nf == 0 || nf > 8) { R = "unexpected vertex formats in the base model"; return false; }
        for (uint32_t f = 0; f < nf; ++f) {
            Fmt fm = { rd32(tmpl, q), rd32(tmpl, q + 4), rd32(tmpl, q + 8), rd32(tmpl, q + 12) }; q += 16;
            if (!VtxKnown(fm.flags, fm.stride)) { char b[120]; snprintf(b, sizeof(b), "the base model uses a vertex layout FC4Hook doesn't know yet (%x, %u bytes)", fm.flags, fm.stride); R = b; return false; }
            lo.fmts.push_back(fm);
        }
        uint32_t ns = rd32(tmpl, q); q += 4;
        if (ns == 0 || ns > 256) { R = "unexpected submesh count"; return false; }
        for (uint32_t k = 0; k < ns; ++k) {
            Rec r = { rd32(tmpl, q), rd32(tmpl, q + 4), rd32(tmpl, q + 8), rd32(tmpl, q + 12), rd32(tmpl, q + 16), rd32(tmpl, q + 20), rd32(tmpl, q + 24) }; q += 28;
            if (r.fmt >= nf || r.part >= ent.size() || r.ent >= ent[r.part].size()) { R = "the base model's submeshes don't match its parts"; return false; }
            lo.recs.push_back(r);
        }
        uint32_t vbytes = rd32(tmpl, q); int vp = padOk(q + 4);
        if (vp < 0) { R = "vertex data marker not found in the base model"; return false; }
        size_t ie = q + 4 + vp + vbytes;
        uint32_t ni = rd32(tmpl, ie); int ip = padOk(ie + 4);
        if (ip < 0) { R = "index data marker not found in the base model"; return false; }
        q = ie + 4 + ip + 2 * (size_t)ni;
        for (uint32_t k = 0; k < ns; ++k) lo.idxCount.push_back((k + 1 < ns ? lo.recs[k + 1].first : ni) - lo.recs[k].first);
    }
    if (q != LE) { R = "the base model's geometry block didn't parse exactly"; return false; }

    float pOff = rdf(tmpl, pcmp->off + 20), pScale = rdf(tmpl, pcmp->off + 24);
    float uOff = rdf(tmpl, ucmp->off + 20), uScale = rdf(tmpl, ucmp->off + 24);
    if (!(pScale > 0) || !(uScale > 0)) { R = "unexpected compression values in the base model"; return false; }

    // --- the glTF
    Gltf g; std::vector<Prim> prims; std::string err;
    if (!LoadGlb(glbFile, g, prims, &err)) { R = err; return false; }
    float sc = opt.scale > 0.001f ? opt.scale : 1.f;
    for (auto& P : prims) for (auto& v : P.pos) v = { v.x * sc + opt.move[0], v.y * sc + opt.move[1], v.z * sc + opt.move[2] };

    // --- skinning: MB2O holds each skinned node's inverse bind matrix (row vectors: 3x3 = the node's world
    // rotation R, last row = -R^T t). Which entry is which node isn't stored, so match them by value.
    auto qmat = [](const float q[4], float R[3][3]) {
        float x = q[0], y = q[1], z = q[2], w = q[3];
        R[0][0] = 1 - 2 * (y * y + z * z); R[0][1] = 2 * (x * y - z * w); R[0][2] = 2 * (x * z + y * w);
        R[1][0] = 2 * (x * y + z * w); R[1][1] = 1 - 2 * (x * x + z * z); R[1][2] = 2 * (y * z - x * w);
        R[2][0] = 2 * (x * z - y * w); R[2][1] = 2 * (y * z + x * w); R[2][2] = 1 - 2 * (x * x + y * y);
    };
    auto invBind = [&](const Xf& W, float m[16]) {
        float R[3][3]; qmat(W.q, R);
        for (int r = 0; r < 3; ++r) { for (int c = 0; c < 3; ++c) m[r * 4 + c] = R[r][c]; m[r * 4 + 3] = 0; }
        for (int c = 0; c < 3; ++c) m[12 + c] = -(R[0][c] * W.t.x + R[1][c] * W.t.y + R[2][c] * W.t.z);
        m[15] = 1;
    };
    std::vector<int> nodeMb(nodeWorld.size(), -1);           // node -> MB2O entry
    const Chunk* mb2o = nullptr;
    for (auto& c : cs) if (c.tag == "O2BM") mb2o = &c;
    if (mb2o && mb2o->dataSize >= 4) {
        uint32_t n = rd32(tmpl, mb2o->off + 20);
        for (size_t k = 0; k < nodeWorld.size(); ++k) {
            float m[16]; invBind(nodeWorld[k], m); int hit = -1, hits = 0;
            for (uint32_t e = 0; e < n && mb2o->off + 24 + 64 * (e + 1) <= mb2o->off + mb2o->size; ++e) {
                float d = 0; for (int i = 0; i < 16; ++i) d = std::max(d, fabsf(rdf(tmpl, mb2o->off + 24 + 64 * e + 4 * i) - m[i]));
                if (d < 2e-3f) { hit = (int)e; ++hits; }
            }
            if (hits == 1) nodeMb[k] = hit;
        }
    }
    // an object named like one of the base model's skinned nodes (PKM_AmmoLid, Handle...) rides that node, so
    // the game's animations move it (skinned pieces only: the vertex's bone slot points at the node)
    std::vector<int> primNode(prims.size(), -1);
    for (size_t pi = 0; pi < prims.size(); ++pi) {
        std::string n; for (char c : prims[pi].name) { if (c == '.') break; n += (char)toupper((unsigned char)c); }
        for (size_t k = 0; k < nodeNames.size(); ++k) {
            std::string nn = nodeNames[k]; for (auto& c : nn) c = (char)toupper((unsigned char)c);
            if (nn == n && nodeMb[k] >= 0) primNode[pi] = (int)k;
        }
    }

    // --- parts the .glb fills. A Blender object named like a part of the base weapon (CLIP, SLIDE,
    // HAMMER..., or MAG/MAGAZINE/BATTERY for the CLIP) goes on that part, so the animations move it
    // (the clip comes out on reload); everything else goes on FRAME.
    auto partIn = [&](const std::string& nm, uint32_t l) {      // part index named nm in LOD l, or -1
        std::string want = nm + "_LOD" + std::to_string(l);
        for (auto& r : lodv[l].recs) if (partName[r.part] == want) return (int)r.part;
        return -1;
    };
    auto frameIn = [&](uint32_t l) {
        int fp = partIn("FRAME", l);
        if (fp >= 0) return fp;
        Lod& lo = lodv[l]; int best = -1;                     // unnamed: the part of the biggest piece
        for (size_t k = 0; k < lo.recs.size(); ++k) if (VtxTangents(lo.fmts[lo.recs[k].fmt].flags) && (best < 0 || lo.idxCount[k] > lo.idxCount[best])) best = (int)k;
        return best < 0 ? -1 : (int)lo.recs[best].part;
    };
    auto groupOf = [&](const std::string& node) {
        std::string n;
        for (char c : node) { if (c == '.') break; n += (char)toupper((unsigned char)c); }
        while (!n.empty() && (n.back() == '_' || n.back() == ' ')) n.pop_back();
        if (n == "MAG" || n == "MAGAZINE" || n == "BATTERY" || n == "AMMO") n = "CLIP";
        if (n.empty() || n == "FRAME") return std::string("FRAME");
        for (auto& pn : partName) if (pn.rfind(n + "_LOD", 0) == 0) return n;
        return std::string("FRAME");
    };
    struct Grp {
        std::string name; std::vector<int> prims; std::vector<std::vector<int>> lodSlots; std::vector<bool> own;
        size_t minSlots = 1000; std::vector<std::vector<int>> slotPrimList, slotMats; std::map<int, int> primCell;
        bool atlas = false; std::vector<uint32_t> slotMatIndex; int merged = 0;
    };
    std::vector<Grp> grps(1); grps[0].name = "FRAME";
    for (size_t k = 0; k < prims.size(); ++k) {
        std::string gn = groupOf(prims[k].name);
        size_t gi = 0; while (gi < grps.size() && grps[gi].name != gn) ++gi;
        if (gi == grps.size()) { grps.emplace_back(); grps.back().name = gn; }
        grps[gi].prims.push_back((int)k);
    }
    for (auto& G : grps) {
        G.lodSlots.resize(nLod); G.own.resize(nLod);
        for (uint32_t l = 0; l < nLod; ++l) {
            Lod& lo = lodv[l];
            int part = G.name == "FRAME" ? frameIn(l) : partIn(G.name, l);
            if (part < 0) { if (G.name == "FRAME") { R = "the base model has no main mesh piece"; return false; } continue; }
            for (size_t k = 0; k < lo.recs.size(); ++k) if ((int)lo.recs[k].part == part && VtxTangents(lo.fmts[lo.recs[k].fmt].flags)) G.lodSlots[l].push_back((int)k);
            if (G.lodSlots[l].empty()) { if (G.name == "FRAME") { R = "the base model's main part has no usable piece"; return false; } continue; }
            std::sort(G.lodSlots[l].begin(), G.lodSlots[l].end(), [&](int x, int y) { return lo.idxCount[x] > lo.idxCount[y]; });
            G.own[l] = true;
            G.minSlots = std::min(G.minSlots, G.lodSlots[l].size());
        }
        if (G.minSlots == 1000) G.minSlots = 1;
        // glTF materials -> slots; with more materials than pieces, slot 0 holds an atlas of the rest
        std::map<int, std::vector<int>> byMat;
        for (int k : G.prims) byMat[prims[k].material].push_back(k);
        std::vector<std::pair<int, size_t>> order;
        for (auto& e : byMat) { size_t n = 0; for (int k : e.second) n += prims[k].pos.size(); order.push_back({ e.first, n }); }
        std::sort(order.begin(), order.end(), [](const std::pair<int, size_t>& x, const std::pair<int, size_t>& y) { return x.second > y.second; });
        size_t nSlots = std::min(order.size(), G.minSlots);
        G.slotPrimList.resize(nSlots); G.slotMats.resize(nSlots);
        size_t own = order.size() <= G.minSlots ? order.size() : G.minSlots - 1;
        for (size_t k = 0; k < order.size(); ++k) {
            size_t sl = order.size() <= G.minSlots ? k : ((k >= 1 && k <= own) ? k : 0);
            int cell = 0;
            if (order.size() > G.minSlots && sl == 0) {
                if (k > 0) ++G.merged;
                if (G.slotMats[0].size() < 4) G.slotMats[0].push_back(order[k].first);
                cell = G.slotMats[0].back() == order[k].first ? (int)G.slotMats[0].size() - 1 : 0;
            } else G.slotMats[sl].push_back(order[k].first);
            for (int pi : byMat[order[k].first]) { G.slotPrimList[sl].push_back(pi); G.primCell[pi] = cell; }
        }
        G.atlas = !G.slotMats.empty() && G.slotMats[0].size() > 1;
    }
    int merged = 0; for (auto& G : grps) merged += G.merged;

    // --- a plain diffuse/normal/specular material to copy
    Bytes plainMat; std::vector<std::string> plainTex;
    for (uint32_t m = 0; m < nMat && plainMat.empty(); ++m) {
        Bytes md;
        if (!readGame(matPath[m], md)) continue;
        auto tx = MaterialTextures(md);
        // a colour AND a normal map (the Deagle's "illuminated" sight material only has a glow
        // texture: copying it gives an invisible gun)
        bool hasD = false, hasN = false;
        for (auto& t : tx) {
            std::string l = t; for (auto& c : l) c = (char)tolower((unsigned char)c);
            if (l.size() > 6 && l.compare(l.size() - 6, 6, "_d.xbt") == 0 && l.find("illum") == std::string::npos && l.find("null") == std::string::npos) hasD = true;
            if (l.size() > 6 && l.compare(l.size() - 6, 6, "_n.xbt") == 0) hasN = true;
        }
        if (hasD && hasN) { plainMat = md; plainTex = tx; }
    }
    if (plainMat.empty()) {
        Bytes md; const char* fb = "graphics\\_materials\\GRASSAM-M-2103201252077243.material.bin";   // the Deagle's DE_PIECES
        if (readGame(fb, md)) { plainMat = md; plainTex = MaterialTextures(md); }
    }
    if (plainMat.empty()) { R = "couldn't read a usable material of the base model from the game"; return false; }

    // --- textures + one new material per slot (appended to the material list)
    std::string base = dir + stem;
    size_t maxLen = 1000;
    for (auto& t : plainTex) maxLen = std::min(maxLen, t.size());
    auto fit = [&](std::string s2) {                          // material texture slots are fixed-size
        if (s2.size() <= maxLen) return s2;
        uint32_t h = 2166136261u; for (char c : s2) h = (h ^ (uint8_t)c) * 16777619u;
        char b[64]; snprintf(b, sizeof(b), "graphics\\fc4hook\\%08x", h);
        return std::string(b) + s2.substr(s2.size() - std::min<size_t>(s2.size(), 8));
    };

    std::string texNote; int texCount = 0;
    auto texOf = [&](int gm, Bytes& png) {
        int ti = g.j["materials"][(size_t)(gm < 0 ? 100000 : gm)]["pbrMetallicRoughness"]["baseColorTexture"]["index"].i();
        return ti >= 0 && g.image(ti, png);
    };
    for (auto& G : grps) {
    G.slotMatIndex.resize(G.slotMats.size());
    std::vector<std::vector<int>>& slotMats = G.slotMats;
    bool atlas = G.atlas;
    std::string gtag = G.name == "FRAME" ? "" : "_" + std::string(1, (char)tolower((unsigned char)G.name[0])) + std::to_string(&G - &grps[0]);
    for (size_t sl = 0; sl < slotMats.size(); ++sl) {
        std::string stemS = base + gtag + "_" + std::to_string(sl);
        std::string dPath = fit(stemS + "_d.xbt"), nPathS = fit(stemS + "_n.xbt"), sPathS = fit(stemS + "_s.xbt"), e2;
        // colour + normal + shine for this piece (several glTF materials = a 2x2 atlas)
        std::vector<TexSrc> srcs;
        size_t cnt = atlas && sl == 0 ? slotMats[0].size() : 1;
        for (size_t k = 0; k < cnt; ++k) {
            int gm = slotMats[sl].empty() ? -1 : slotMats[sl][k];
            const J& gmat = g.j["materials"][(size_t)(gm < 0 ? 100000 : gm)];
            const J& pbr = gmat["pbrMetallicRoughness"];
            TexSrc ts;
            std::string label = gmat["name"].s.empty() ? "material " + std::to_string(gm) : gmat["name"].s;
            if (!texOf(gm, ts.colour)) texNote += "\n  " + label + ": no colour texture packed in the .glb, plain grey used";
            else ++texCount;
            int ni = gmat["normalTexture"]["index"].i(), mi = pbr["metallicRoughnessTexture"]["index"].i();
            if (ni >= 0) g.image(ni, ts.normal);
            if (mi >= 0) g.image(mi, ts.mr);
            // extras.dunia_mask: the metal/roughness texture is really a Dunia "_m"/"_s" mask (R shine, G
            // occlusion, B gloss), e.g. from fc3xbg2glb.py: used as it is
            ts.gameMask = mi >= 0 && gmat["extras"]["dunia_mask"].i(0) != 0;
            ts.rough = pbr.has("roughnessFactor") ? (float)pbr["roughnessFactor"].n : 1.f;
            ts.metal = pbr.has("metallicFactor") ? (float)pbr["metallicFactor"].n : 1.f;
            srcs.push_back(ts);
        }
        Bytes tex[3][2];
        if (!TextureSetXbt(srcs, dPath, nPathS, sPathS, tex, &e2)) { R = "couldn't make the textures: " + e2; return false; }
        const std::string* tp[3] = { &dPath, &nPathS, &sPathS };
        for (int t = 0; t < 3; ++t) {
            files.push_back({ *tp[t], tex[t][0] });
            if (!tex[t][1].empty()) files.push_back({ Mip0Name(*tp[t]), tex[t][1] });
        }
        std::string nPath = nPathS, sPath = sPathS;
        std::vector<std::pair<std::string, std::string>> swaps;
        for (auto& t : plainTex) {
            std::string l = t; for (auto& c : l) c = (char)tolower((unsigned char)c);
            if (l.size() > 6 && l.compare(l.size() - 6, 6, "_n.xbt") == 0) swaps.push_back({ t, nPath });
            else if (l.size() > 6 && l.compare(l.size() - 6, 6, "_d.xbt") == 0) swaps.push_back({ t, dPath });
            else swaps.push_back({ t, sPath });
        }
        Bytes nm; std::string e3;
        if (!MaterialWithTextures(plainMat, swaps, nm, &e3)) { R = "couldn't make a material: " + e3; return false; }
        std::string mp = base + gtag + "_" + std::to_string(sl) + ".material.bin";
        files.push_back({ mp, nm });
        G.slotMatIndex[sl] = (uint32_t)matPath.size();
        matPath.push_back(mp); matName.push_back(stem + gtag + "_" + std::to_string(sl));
    }
    }

    // which (group, slot)s each submesh of LOD l gets. A part missing in a LOD (far LODs often
    // only have FRAME) puts its geometry on that LOD's main FRAME piece.
    auto assignFor = [&](uint32_t l) {
        std::map<int, std::vector<std::pair<int, size_t>>> m;
        for (size_t gi = 0; gi < grps.size(); ++gi) {
            const Grp& G = grps[gi];
            for (size_t k = 0; k < G.slotPrimList.size(); ++k) {
                if (G.own[l] && k < G.lodSlots[l].size()) m[G.lodSlots[l][k]].push_back({ (int)gi, k });
                else m[grps[0].lodSlots[l][0]].push_back({ (int)gi, k });
            }
        }
        return m;
    };
    // --- geometry per LOD
    // the base model's position range (PCMP, int16 * scale + offset) may be too small for the new model
    // (a 1.2 m minigun on a PKM that holds +-1 m): every vertex is rewritten anyway, so widen it to fit
    bool widened = false;
    {
        float need = 0;
        for (size_t pi = 0; pi < prims.size(); ++pi) for (auto& v : prims[pi].pos)
            need = std::max({ need, fabsf(v.x - pOff), fabsf(v.y - pOff), fabsf(v.z - pOff) });
        need += 0.25f;                                     // room for bone-space offsets of the parts
        if (need > 32767 * pScale * 0.98f) { pScale = need / (32767 * 0.98f); widened = true; }
    }
    // same for texture coordinates (UCMP): Far Cry 3 / Blood Dragon models repeat textures (u up to 2+)
    bool uvWidened = false;
    {
        float need = 0;
        for (auto& P : prims) for (float u : P.uv) need = std::max(need, fabsf(u - uOff));
        need += 0.01f;
        if (need > 32767 * uScale) { uScale = need / 32767; uvWidened = true; }
    }
    auto Qp = [&](float v, bool* bad) { long q2 = lroundf((v - pOff) / pScale); if (q2 < -32768 || q2 > 32767) *bad = true; return (int16_t)std::max(-32768L, std::min(32767L, q2)); };
    auto Qu = [&](float v) { long q2 = lroundf((v - uOff) / uScale); return (int16_t)std::max(-32768L, std::min(32767L, q2)); };
    bool tooBig = false;
    V3 mn = { 1e9f, 1e9f, 1e9f }, mx = { -1e9f, -1e9f, -1e9f };
    for (auto& P : prims) for (auto& v : P.pos) { mn = { std::min(mn.x, v.x), std::min(mn.y, v.y), std::min(mn.z, v.z) }; mx = { std::max(mx.x, v.x), std::max(mx.y, v.y), std::max(mx.z, v.z) }; }
    // tangents once per primitive
    std::vector<std::vector<V3>> pT(prims.size()), pB(prims.size());
    for (size_t pi = 0; pi < prims.size(); ++pi) {
        const Prim& P = prims[pi]; size_t nv = P.pos.size();
        std::vector<V3> tan(nv, { 0, 0, 0 }), bit(nv, { 0, 0, 0 });
        for (size_t t = 0; t + 2 < P.idx.size(); t += 3) {
            uint32_t a = P.idx[t], b = P.idx[t + 1], c = P.idx[t + 2];
            V3 e1 = sub(P.pos[b], P.pos[a]), e2 = sub(P.pos[c], P.pos[a]);
            float du1 = P.uv[b * 2] - P.uv[a * 2], dv1 = P.uv[b * 2 + 1] - P.uv[a * 2 + 1];
            float du2 = P.uv[c * 2] - P.uv[a * 2], dv2 = P.uv[c * 2 + 1] - P.uv[a * 2 + 1];
            float d = du1 * dv2 - du2 * dv1; if (fabsf(d) < 1e-12f) continue;
            float f = 1.f / d;
            V3 T = { f * (dv2 * e1.x - dv1 * e2.x), f * (dv2 * e1.y - dv1 * e2.y), f * (dv2 * e1.z - dv1 * e2.z) };
            V3 B = { f * (du1 * e2.x - du2 * e1.x), f * (du1 * e2.y - du2 * e1.y), f * (du1 * e2.z - du2 * e1.z) };
            for (uint32_t k : { a, b, c }) { tan[k].x += T.x; tan[k].y += T.y; tan[k].z += T.z; bit[k].x += B.x; bit[k].y += B.y; bit[k].z += B.z; }
        }
        pT[pi].resize(nv); pB[pi].resize(nv);
        for (size_t k = 0; k < nv; ++k) {
            V3 N = P.nrm[k], T = tan[k];
            T = sub(T, { N.x * dot(N, T), N.y * dot(N, T), N.z * dot(N, T) });
            if (dot(T, T) < 1e-12f) T = fabsf(N.x) < 0.9f ? cross(N, { 1, 0, 0 }) : cross(N, { 0, 1, 0 });
            T = norm(T);
            V3 B = cross(N, T); if (dot(B, bit[k]) < 0) B = { -B.x, -B.y, -B.z };
            pT[pi][k] = T; pB[pi][k] = B;
        }
    }
    struct LodOut { std::vector<Bytes> vbuf; std::vector<uint32_t> fcount; std::vector<std::vector<uint16_t>> sIdx; std::vector<uint32_t> sFirstV, sNumV; std::vector<std::vector<uint16_t>> sPal; };
    std::vector<LodOut> lout(nLod);
    size_t totalTris = 0; uint32_t mainVerts = 0;
    for (uint32_t l = 0; l < nLod; ++l) {
        Lod& lo = lodv[l]; LodOut& o = lout[l];
        size_t nf = lo.fmts.size(), ns = lo.recs.size();
        o.vbuf.resize(nf); o.fcount.assign(nf, 0); o.sIdx.resize(ns); o.sFirstV.resize(ns); o.sNumV.resize(ns); o.sPal.resize(ns);
        auto slotOf = assignFor(l);                           // submesh -> (group, slot)s
        for (size_t s = 0; s < ns; ++s) {
            const Rec& r = lo.recs[s];
            Bytes& vb = o.vbuf[r.fmt];
            uint32_t first = o.fcount[r.fmt], stride = lo.fmts[r.fmt].stride;
            o.sFirstV[s] = first;
            uint32_t vflags = lo.fmts[r.fmt].flags;
            bool skinned = (vflags & 0x10) != 0, tangents = VtxTangents(vflags);
            uint8_t slot = 0;                                     // skinned: the vertex's palette slot (SKND below: 0 = the part's node)
            std::vector<uint16_t>& pal = o.sPal[s];
            if (skinned && partNode[r.part] >= 0) pal.push_back((uint16_t)partNode[r.part]);
            auto put = [&](V3 P, V3 N, float u, float v, V3 T, V3 B) {
                // layouts (all: int16 pos x4 at 0, int16 uv at 8):
                //   0xCA  20 B: pos uv normal colour                  0xBCA 32 B: pos uv uv1 normal colour tangent binormal
                //   0xDA  28 B: pos uv weights bones normal colour   0xBDA 40 B: pos uv uv1 weights bones normal colour tangent binormal
                int16_t px[4] = { Qp(P.x, &tooBig), Qp(P.y, &tooBig), Qp(P.z, &tooBig), 1 };
                size_t at = vb.size(); vb.resize(at + stride);
                memcpy(&vb[at], px, 8);
                int16_t uv[2] = { Qu(u), Qu(v) };
                memcpy(&vb[at + 8], uv, 4);
                size_t no = 12;
                if (tangents) { memcpy(&vb[at + 12], uv, 4); no = 16; }
                if (skinned) {                                    // the whole vertex on the part's own bone
                    uint8_t w[4] = { 255, 0, 0, 0 }, bi[4] = { slot, 0, 0, 0 };
                    memcpy(&vb[at + no], w, 4); memcpy(&vb[at + no + 4], bi, 4); no += 8;
                }
                uint8_t nn[4] = { snorm8(N.z), snorm8(N.y), snorm8(N.x), 128 };   // stored z, y, x (D3DCOLOR order)
                memcpy(&vb[at + no], nn, 4);
                memset(&vb[at + no + 4], 0xff, 4);
                if (tangents) {
                    uint8_t tt[4] = { snorm8(T.z), snorm8(T.y), snorm8(T.x), 128 }, bb[4] = { snorm8(B.z), snorm8(B.y), snorm8(B.x), 128 };
                    memcpy(&vb[at + no + 8], tt, 4); memcpy(&vb[at + no + 12], bb, 4);
                }
            };
            auto it = slotOf.find((int)s);
            if (it == slotOf.end()) {                         // emptied piece: one point, one degenerate triangle
                int n = 1;
                if (stride % 32) {                            // keep the next format 32-byte aligned
                    bool lastOfFmt = true;
                    for (size_t k = s + 1; k < ns; ++k) if (lo.recs[k].fmt == r.fmt) lastOfFmt = false;
                    if (lastOfFmt) while (((o.fcount[r.fmt] + n) * stride) % 32) ++n;
                }
                for (int k = 0; k < n; ++k) put({ 0, 0, 0 }, { 0, 0, 1 }, 0.5f, 0.5f, { 1, 0, 0 }, { 0, 1, 0 });
                o.fcount[r.fmt] += n; o.sNumV[s] = n;
                o.sIdx[s] = { (uint16_t)first, (uint16_t)first, (uint16_t)first };
                continue;
            }
            for (auto& gs : it->second) {
            const Grp& G = grps[gs.first]; size_t sl = gs.second;
            for (int pi : G.slotPrimList[sl]) {
                const Prim& P = prims[pi];
                size_t nv = P.pos.size();
                uint32_t vb0 = o.fcount[r.fmt];
                slot = 0;
                if (skinned && primNode[pi] >= 0 && !pal.empty()) {
                    size_t at = std::find(pal.begin(), pal.end(), (uint16_t)primNode[pi]) - pal.begin();
                    if (at == pal.size() && pal.size() < 48) pal.push_back((uint16_t)primNode[pi]);
                    if (at < pal.size()) slot = (uint8_t)at;
                }
                if (vb0 + nv > 65535) { R = "the model has too many vertices (65535 at most); simplify it (Decimate in Blender)"; return false; }
                for (size_t k = 0; k < nv; ++k) {
                    float u = P.uv[k * 2], v = P.uv[k * 2 + 1];
                    if (G.atlas && sl == 0) {                     // into this material's quarter of the atlas
                        int c = G.primCell.count(pi) ? G.primCell.at(pi) : 0;
                        u = (c % 2 + std::min(1.f, std::max(0.f, u))) * 0.5f; v = (c / 2 + std::min(1.f, std::max(0.f, v))) * 0.5f;
                    }
                    // model space -> the part's bone space
                    Xf X = partXf[r.part];
                    if (skinned) X = Xf();                        // skinned layouts are in model space
                    float iq[4] = { -X.q[0], -X.q[1], -X.q[2], X.q[3] };
                    V3 lp = qrot(iq, sub(P.pos[k], X.t));
                    put(lp, qrot(iq, P.nrm[k]), u, v, qrot(iq, pT[pi][k]), qrot(iq, pB[pi][k]));
                }
                o.fcount[r.fmt] += (uint32_t)nv;
                for (size_t t = 0; t + 2 < P.idx.size(); t += 3) {      // counter-clockwise -> clockwise
                    o.sIdx[s].push_back((uint16_t)(vb0 + P.idx[t])); o.sIdx[s].push_back((uint16_t)(vb0 + P.idx[t + 2])); o.sIdx[s].push_back((uint16_t)(vb0 + P.idx[t + 1]));
                }
                if (l == 0) totalTris += P.idx.size() / 3;
            }
            }
            o.sNumV[s] = o.fcount[r.fmt] - first;
            if (o.sNumV[s] == 0) {                            // a slot with nothing (can't happen, but keep it valid)
                put({ 0, 0, 0 }, { 0, 0, 1 }, 0.5f, 0.5f, { 1, 0, 0 }, { 0, 1, 0 });
                o.fcount[r.fmt] += 1; o.sNumV[s] = 1; o.sIdx[s] = { (uint16_t)first, (uint16_t)first, (uint16_t)first };
            }
            if (l == 0) mainVerts += o.sNumV[s];
        }
    }
    if (tooBig) { char b[220]; snprintf(b, sizeof(b), "the model is too big or too far from its origin (this format holds about +-%.2f m). Apply scale in Blender, keep the gun at the origin, or lower Scale/Move.", 32767 * pScale); R = b; return false; }
    if (totalTris == 0) { R = "no triangles ended up in the model"; return false; }

    // --- LODS (built once its place in the file is known: the paddings depend on it)
    auto pad = [](Bytes& b, size_t base0) { int k = (int)((16 - (base0 + b.size()) % 16) % 16); for (int i = k; i >= 1; --i) b.push_back((uint8_t)i); };
    auto buildLods = [&](size_t base0) {
        Bytes nl;
        w32(nl, nLod);
        for (uint32_t l = 0; l < nLod; ++l) {
            Lod& lo = lodv[l]; LodOut& o = lout[l];
            uint32_t u; memcpy(&u, &lo.dist, 4); w32(nl, u);
            w32(nl, (uint32_t)lo.fmts.size());
            uint32_t off = 0; std::vector<uint32_t> fOff(lo.fmts.size());
            for (size_t f = 0; f < lo.fmts.size(); ++f) { fOff[f] = off; w32(nl, lo.fmts[f].flags); w32(nl, lo.fmts[f].stride); w32(nl, o.fcount[f]); w32(nl, off); off += (uint32_t)o.vbuf[f].size(); }
            w32(nl, (uint32_t)lo.recs.size());
            uint32_t firstIdx = 0;
            for (size_t s = 0; s < lo.recs.size(); ++s) {
                const Rec& r = lo.recs[s];
                w32(nl, r.fmt); w32(nl, r.part); w32(nl, r.ent); w32(nl, firstIdx); w32(nl, o.sFirstV[s] + o.sNumV[s] - 1);
                w32(nl, fOff[r.fmt] + o.sFirstV[s] * lo.fmts[r.fmt].stride); w32(nl, r.z);
                firstIdx += (uint32_t)o.sIdx[s].size();
            }
            w32(nl, off); pad(nl, base0);
            for (auto& v : o.vbuf) nl.insert(nl.end(), v.begin(), v.end());
            w32(nl, firstIdx); pad(nl, base0);
            for (auto& v : o.sIdx) for (auto i2 : v) w16(nl, i2);
        }
        return nl;
    };

    // --- RMTL (the base model's materials + ours)
    Bytes nr;
    w32(nr, (uint32_t)matPath.size()); w32(nr, rd32(tmpl, rmtl->off + 24));
    for (size_t m = 0; m < matPath.size(); ++m) {
        w32(nr, (uint32_t)matPath[m].size()); nr.insert(nr.end(), matPath[m].begin(), matPath[m].end()); nr.push_back(0);
        w32(nr, (uint32_t)matName[m].size()); nr.insert(nr.end(), matName[m].begin(), matName[m].end()); nr.push_back(0);
    }

    // --- assemble
    out.assign(tmpl.begin(), tmpl.begin() + 32);
    auto chunk = [&](const Chunk& c, const Bytes& data) {
        size_t o = out.size();
        out.insert(out.end(), tmpl.begin() + c.off, tmpl.begin() + c.off + 20);
        out.insert(out.end(), data.begin(), data.end());
        p32(out, o + 8, (uint32_t)(20 + data.size()));
        p32(out, o + 12, (uint32_t)data.size());
    };
    for (auto& c : cs) {
        if (c.tag == "RMTL") { chunk(c, nr); continue; }
        if (c.tag == "LODS") { chunk(c, buildLods(out.size() + 20)); continue; }
        size_t o = out.size();
        out.insert(out.end(), tmpl.begin() + c.off, tmpl.begin() + c.off + c.size);
        if (c.tag == "SKND") {                                // every piece's counts, and our material on the used ones
            for (uint32_t l = 0; l < nLod; ++l) {
                auto slotOf = assignFor(l);
                for (size_t s = 0; s < lodv[l].recs.size(); ++s) {
                    const Rec& r = lodv[l].recs[s];
                    size_t e = o + (ent[r.part][r.ent] - c.off);
                    uint32_t ni = (uint32_t)lout[l].sIdx[s].size(), nv = lout[l].sNumV[s];
                    if (slotOf.count((int)s)) { auto& gs = slotOf[(int)s][0]; p16(out, e, (uint16_t)grps[gs.first].slotMatIndex[gs.second]); }
                    p16(out, e + 2, (uint16_t)(ni / 3)); p16(out, e + 4, (uint16_t)(ni / 3));
                    p16(out, e + 6, (uint16_t)std::min<uint32_t>(ni, 65535)); p16(out, e + 10, (uint16_t)nv);
                    p32(out, e + 110, nv); p32(out, e + 142, ni);
                    if (slotOf.count((int)s) && (lodv[l].fmts[r.fmt].flags & 0x10))      // skinned: our palette
                        for (size_t k = 0; k < lout[l].sPal[s].size(); ++k) p16(out, e + 14 + 2 * k, lout[l].sPal[s][k]);
                }
            }
        } else if (c.tag == "PCMP" && widened && c.dataSize >= 8) {
            memcpy(&out[o + 24], &pScale, 4);
        } else if (c.tag == "UCMP" && uvWidened && c.dataSize >= 8) {
            memcpy(&out[o + 24], &uScale, 4);
        } else if (c.tag == "BBOX" && c.dataSize >= 24) {
            memcpy(&out[o + 20], &mn, 12); memcpy(&out[o + 32], &mx, 12);
        } else if (c.tag == "BSPH" && c.dataSize >= 16) {
            V3 ce = { (mn.x + mx.x) / 2, (mn.y + mx.y) / 2, (mn.z + mx.z) / 2 }; float rad = 0;
            for (auto& P : prims) for (auto& v : P.pos) rad = std::max(rad, sqrtf(dot(sub(v, ce), sub(v, ce))));
            memcpy(&out[o + 20], &ce, 12); memcpy(&out[o + 32], &rad, 4);
        }
    }
    if (!cs.empty()) { size_t tail = cs.back().off + cs.back().size; out.insert(out.end(), tmpl.begin() + tail, tmpl.end()); }

    // --- points: an empty in the .glb named like one of the base model's nodes (IK_LeftHand, FX_FIRE,
    // FX_CASING...) moves that node there, so the left hand's IK grip, the muzzle flash and the shell
    // ejection match the new gun. glTF (x, y, z) = game (x, -z, y).
    std::string moved;
    {
        std::vector<Chunk> oc; TopChunks(out, 32, out.size(), oc);
        const Chunk* onode = nullptr; const Chunk* tnode = nullptr; const Chunk* omb = nullptr;
        for (auto& c : oc) { if (c.tag == "NODE") onode = &c; else if (c.tag == "O2BM") omb = &c; }
        for (auto& c : cs) if (c.tag == "NODE") tnode = &c;
        const J& gn = g.j["nodes"];
        std::vector<Xf> W = nodeWorld, Lc = nodeLocal; std::vector<bool> changed(nodeWorld.size(), false);
        auto mulX = [&](const Xf& P, const Xf& L) {
            Xf w; V3 rt = qrot(P.q, L.t); w.t = { P.t.x + rt.x, P.t.y + rt.y, P.t.z + rt.z };
            const float* a1 = P.q; const float* b1 = L.q;
            w.q[0] = a1[3] * b1[0] + a1[0] * b1[3] + a1[1] * b1[2] - a1[2] * b1[1];
            w.q[1] = a1[3] * b1[1] - a1[0] * b1[2] + a1[1] * b1[3] + a1[2] * b1[0];
            w.q[2] = a1[3] * b1[2] + a1[0] * b1[1] - a1[1] * b1[0] + a1[2] * b1[3];
            w.q[3] = a1[3] * b1[3] - a1[0] * b1[0] - a1[1] * b1[1] - a1[2] * b1[2];
            return w;
        };
        for (size_t k = 0; onode && tnode && k < nodeNames.size() && k < Lc.size(); ++k) {     // parents come first
            int pa = nodeParent[k];
            std::string nn = nodeNames[k]; for (auto& ch : nn) ch = (char)toupper((unsigned char)ch);
            for (size_t gi = 0; gi < gn.a.size(); ++gi) {
                const J& N = gn.a[gi];
                if (N.has("mesh") || N["name"].s.empty()) continue;
                std::string want = N["name"].s; for (auto& ch : want) ch = (char)toupper((unsigned char)ch);
                if (nn != want) continue;
                const J& T = N["translation"]; const J& Q = N["rotation"];
                Xf tw;
                V3 wt = { T.a.size() == 3 ? (float)T.a[0].n : 0.f, T.a.size() == 3 ? -(float)T.a[2].n : 0.f, T.a.size() == 3 ? (float)T.a[1].n : 0.f };
                tw.t = { wt.x * sc + opt.move[0], wt.y * sc + opt.move[1], wt.z * sc + opt.move[2] };
                if (Q.a.size() == 4) { tw.q[0] = (float)Q.a[0].n; tw.q[1] = -(float)Q.a[2].n; tw.q[2] = (float)Q.a[1].n; tw.q[3] = (float)Q.a[3].n; }
                Xf P; if (pa >= 0 && pa < (int)k) P = W[pa];
                Xf iP; iP.q[0] = -P.q[0]; iP.q[1] = -P.q[1]; iP.q[2] = -P.q[2]; iP.q[3] = P.q[3];
                iP.t = qrot(iP.q, { -P.t.x, -P.t.y, -P.t.z });
                Lc[k] = mulX(iP, tw);                         // local = inverse(parent's world, as moved) * target
                size_t at = onode->off + (nodeOffs[k] - tnode->off);
                memcpy(&out[at + 16], Lc[k].q, 16); memcpy(&out[at + 32], &Lc[k].t, 12);
                moved += (moved.empty() ? "" : ", ") + nodeNames[k];
                changed[k] = true;
                break;
            }
            if (pa >= 0 && pa < (int)k) { W[k] = mulX(W[pa], Lc[k]); if (changed[pa]) changed[k] = true; }
            else W[k] = Lc[k];
            // a moved skinned node (or one under it) gets its new inverse bind, so its geometry stays put
            if (changed[k] && omb && nodeMb[k] >= 0) { float m[16]; invBind(W[k], m); memcpy(&out[omb->off + 24 + 64 * nodeMb[k]], m, 64); }
        }
    }
    // header field at +20 tracks the file's size
    int64_t delta = (int64_t)out.size() - (int64_t)tmpl.size();
    p32(out, 20, (uint32_t)((int64_t)rd32(tmpl, 20) + delta));
    files.push_back({ dir + stem + ".xbg", out });

    char b[400];
    snprintf(b, sizeof(b), "%zu triangles, %u vertices, %u LOD(s), size %.3f x %.3f x %.3f m (x right, y forward, z up). %d texture(s) from the .glb.",
             totalTris, mainVerts, nLod, mx.x - mn.x, mx.y - mn.y, mx.z - mn.z, texCount);
    R = b;
    for (auto& G : grps) if (G.name != "FRAME" && !G.prims.empty()) R += " " + G.name + ": on the weapon's " + G.name + " part (animated).";
    if (!moved.empty()) R += " Moved to the .glb's points: " + moved + ".";
    if (merged) R += " " + std::to_string(merged) + " extra material(s) share a texture (packed into an atlas).";
    R += texNote;
    return true;
}

} // namespace fc4
