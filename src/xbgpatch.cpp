// .obj -> .xbg for EDITED game models. Plain C++ (no Windows), shared by fc4hook.asi and
// tools/fc4mdl.exe.
//
// Building an .xbg from nothing would mean writing bones, skinning, LODs and the game's
// material setup. But the usual job is "I took the game's model into Blender and changed its
// shape" (a longer barrel, a bigger scope). For that, everything except the vertex positions
// can stay exactly as the game made it. So: take the ORIGINAL .xbg and the edited .obj, and
// write the .obj's positions into the original's vertex buffer.
//
// What's known about the format (FC4 patch 1.10, "HSEM" version 0x37):
//   header 32 bytes, then chunks: tag(4, reversed) version size dataSize childCount (20 bytes)
//   LODS  geometry. Vertex data = u32 size, bytes 04 03 02 01, then the vertices of every submesh, each
//         submesh with its own stride (20 or 32 bytes seen), position = 3 x int16 at offset 0
//   PCMP  position decompression: float offset, float scale  (pos = int16 * scale + offset)
//   BBOX  6 floats (min xyz, max xyz)      BSPH  4 floats (centre xyz, radius)
// The .obj must keep the model's vertices in the same order (what the importers do; Blender
// keeps it as long as you only move/scale/rotate vertices and don't add or delete any).
// Blender's .obj axes: obj (x, y, z) = game (x, z, -y).
//
// Matching: vertices are walked in order. Each one is expected right after the previous one
// (same stride, or a new submesh at the next 32-byte boundary). Unchanged vertices are found
// exactly; moved ones take the slot the stride predicts. The walk must end exactly at the end
// of the vertex data, or nothing is written.
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

namespace fc4 {

typedef std::vector<uint8_t> Bytes;

static uint32_t rd(const Bytes& d, size_t o) { uint32_t v = 0; if (o + 4 <= d.size()) memcpy(&v, &d[o], 4); return v; }
static float rdf(const Bytes& d, size_t o) { float v = 0; if (o + 4 <= d.size()) memcpy(&v, &d[o], 4); return v; }

struct XChunk { std::string tag; size_t off = 0; uint32_t size = 0, dataSize = 0; };

static void Chunks(const Bytes& d, size_t off, size_t end, std::vector<XChunk>& out) {
    while (off + 20 <= end) {
        XChunk c;
        char t[5] = { (char)d[off + 3], (char)d[off + 2], (char)d[off + 1], (char)d[off], 0 };
        c.tag = t; c.off = off; c.size = rd(d, off + 8); c.dataSize = rd(d, off + 12);
        if (c.size < 20 || off + c.size > end || 20 + (size_t)c.dataSize > c.size) return;
        out.push_back(c);
        Chunks(d, off + 20 + c.dataSize, off + c.size, out);
        off += c.size;
    }
}

static const XChunk* Find(const std::vector<XChunk>& cs, const char* tag) {
    for (auto& c : cs) if (c.tag == tag) return &c;
    return nullptr;
}

struct V3 { float x, y, z; };

static bool LoadObj(const std::string& text, std::vector<V3>& out, std::string* err) {
    out.clear();
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p); if (e == std::string::npos) e = text.size();
        if (e - p > 2 && text[p] == 'v' && text[p + 1] == ' ') {
            float x = 0, y = 0, z = 0;
            if (sscanf(text.c_str() + p + 2, "%f %f %f", &x, &y, &z) == 3) out.push_back({ x, -z, y });   // Blender -> game axes
        }
        p = e + 1;
    }
    if (out.empty()) { *err = "the .obj has no vertices"; return false; }
    return true;
}

// Patch `xbg` in place with the positions from `objText`. Returns a report.
bool XbgFromObj(Bytes& xbg, const std::string& objText, std::string* report) {
    std::string& r = *report;
    if (xbg.size() < 64 || memcmp(xbg.data(), "HSEM", 4) != 0) { r = "not an .xbg model (no HSEM header)"; return false; }
    std::vector<XChunk> cs;
    Chunks(xbg, 32, xbg.size(), cs);
    const XChunk* lods = Find(cs, "LODS"); const XChunk* pcmp = Find(cs, "PCMP");
    if (!lods || !pcmp) { r = "this .xbg has no LODS/PCMP chunk: unsupported model layout"; return false; }
    float off = rdf(xbg, pcmp->off + 20), scale = rdf(xbg, pcmp->off + 24);
    if (!(scale > 0)) { r = "unexpected position compression in the .xbg"; return false; }
    // vertex data: u32 size, then the bytes 04 03 02 01
    size_t L0 = lods->off + 20, L1 = lods->off + 20 + lods->dataSize, D0 = 0, DE = 0;
    for (size_t i = L0 + 4; i + 4 <= L1; ++i)
        if (rd(xbg, i) == 0x01020304u) { uint32_t n = rd(xbg, i - 4); if (n > 0 && i + 4 + n <= L1) { D0 = i + 4; DE = D0 + n; break; } }
    if (!D0) { r = "couldn't find the vertex data in the .xbg"; return false; }

    std::vector<V3> ov; std::string err;
    if (!LoadObj(objText, ov, &err)) { r = err; return false; }

    auto Q = [&](float v) { return (long)lround((v - off) / scale); };
    auto at = [&](size_t a, int k) { int16_t s; memcpy(&s, &xbg[a + 2 * k], 2); return (long)s; };
    auto exact = [&](size_t a, const V3& g) {
        return a + 6 <= DE && labs(at(a, 0) - Q(g.x)) <= 1 && labs(at(a, 1) - Q(g.y)) <= 1 && labs(at(a, 2) - Q(g.z)) <= 1;
    };

    std::vector<size_t> addr(ov.size(), 0);
    std::vector<char> moved(ov.size(), 0);
    size_t prev = 0; size_t stride = 0;
    for (size_t i = 0; i < ov.size(); ++i) {
        size_t got = 0;
        if (i == 0) got = D0;
        else {
            size_t cand[6] = { prev + stride, prev + 20, prev + 32, (prev + stride + 31) / 32 * 32, (prev + stride + 31) / 32 * 32 + 32, 0 };
            // 32-byte boundaries are relative to the vertex data start
            cand[3] = D0 + ((prev + stride - D0 + 31) / 32) * 32; cand[4] = cand[3] + 32;
            for (int k = 0; k < 5 && !got; ++k) if (cand[k] > prev && exact(cand[k], ov[i])) got = cand[k];
            if (!got) got = prev + stride;                         // moved: the slot the stride predicts
        }
        if (got + 6 > DE) { r = "the .obj has more vertices than the model (" + std::to_string(ov.size()) + "): add/delete vertices isn't supported, only moving them"; return false; }
        if (i > 0 && (got - prev == 20 || got - prev == 32 || got - prev == 24 || got - prev == 28)) stride = got - prev;
        else if (i == 1) stride = got - prev;
        if (i == 0) stride = 20;                                   // refined by the next exact hit
        moved[i] = !exact(got, ov[i]);
        addr[i] = got; prev = got;
    }
    // the walk must end exactly at the end of the vertex data
    size_t lastEnd = addr.back() + stride;
    if (lastEnd != DE) {
        char b[256];
        snprintf(b, sizeof(b), "the .obj doesn't line up with this model (walk ended at %zu of %zu bytes). Use the .obj exported from THIS "
                 "model, keep the vertex order, don't add or delete vertices, and export with 'Keep vertex order' if offered.", lastEnd - D0, DE - D0);
        r = b; return false;
    }
    int nMoved = 0, unchanged = 0;
    for (size_t i = 0; i < ov.size(); ++i) (moved[i] ? nMoved : unchanged)++;
    if (unchanged < (int)ov.size() / 4) { r = "fewer than a quarter of the vertices match the original: this .obj probably isn't made from this model (or it was moved/rotated as a whole)"; return false; }
    // write
    float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
    for (size_t i = 0; i < ov.size(); ++i) {
        const V3& g = ov[i];
        long q[3] = { Q(g.x), Q(g.y), Q(g.z) };
        for (int k = 0; k < 3; ++k) if (q[k] < -32768 || q[k] > 32767) {
            char b[200]; snprintf(b, sizeof(b), "vertex %zu is too far from the model's origin (the format holds about +-%.2f m)", i + 1, 32767 * scale);
            r = b; return false;
        }
        if (moved[i]) for (int k = 0; k < 3; ++k) { int16_t s = (int16_t)q[k]; memcpy(&xbg[addr[i] + 2 * k], &s, 2); }
        float v[3] = { g.x, g.y, g.z };
        for (int k = 0; k < 3; ++k) { mn[k] = std::min(mn[k], v[k]); mx[k] = std::max(mx[k], v[k]); }
    }
    if (const XChunk* bb = Find(cs, "BBOX")) if (bb->dataSize >= 24) { memcpy(&xbg[bb->off + 20], mn, 12); memcpy(&xbg[bb->off + 32], mx, 12); }
    if (const XChunk* bs = Find(cs, "BSPH")) if (bs->dataSize >= 16) {
        float c[3] = { (mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, (mn[2] + mx[2]) / 2 }, rad = 0;
        for (auto& g : ov) { float dx = g.x - c[0], dy = g.y - c[1], dz = g.z - c[2]; rad = std::max(rad, sqrtf(dx * dx + dy * dy + dz * dz)); }
        memcpy(&xbg[bs->off + 20], c, 12); memcpy(&xbg[bs->off + 32], &rad, 4);
    }
    char b[300];
    snprintf(b, sizeof(b), "%zu vertices: %d moved, %d unchanged. New size %.3f x %.3f x %.3f m.",
             ov.size(), nMoved, unchanged, mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]);
    r = b;
    return true;
}

} // namespace fc4
