// PNG/JPG/TGA -> Far Cry 4 .xbt textures, and copies of materials that point at them.
// Plain C++ (no Windows), shared by fc4hook.asi and the tools.
//
// A texture is "TBX\0" + header + a DDS. Big game textures are split in two files (_mip0 +
// the rest) with the _mip0 file named in the header; FC4Hook writes the simple single-file kind
// (36-byte header) that the game uses for smaller textures, with every mip level inside.
// The header's three ids say what kind of texture it is; they're copied from a real game
// texture of the same kind:
//   diffuse ("_d"):  DXT1, from weapons\ammo\bullets_01_d.xbt
//   normal  ("_n"):  DXT5, from weapons\deserteagle\deserteagle_n.xbt
//   specular("_s"):  DXT1, from weapons\deserteagle\deserteagle_s.xbt
// Materials (.material.bin) are "TAM\0" + 16 bytes + an FCB whose texture parameters are plain
// path strings (65 bytes in weapon materials): a copy just gets new, shorter-or-equal paths.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_NO_STDIO
#include "../external/stb/stb_image.h"
#define STB_DXT_IMPLEMENTATION
#include "../external/stb/stb_dxt.h"
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

namespace fc4 {
std::string Mip0Name(const std::string& xbt);

typedef std::vector<uint8_t> Bytes;

static void put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); }

// header bytes after "TBX\0" 70 00 00 00 24 00 00 00 : flags, (01|00) ff ff ff, three ids, 0
static const uint8_t kHdrD[] = { 0x08,0,0,0, 0x01,0xff,0xff,0xff, 0x96,0xe9,0x4e,0xa5, 0x3e,0x70,0x36,0x8f, 0xd1,0x8e,0x31,0xcb, 0,0,0,0 };
static const uint8_t kHdrN[] = { 0x08,0,0,0, 0x00,0xff,0xff,0xff, 0x66,0x25,0x3f,0x8a, 0x4b,0x1f,0x94,0xe6, 0x76,0x05,0xbc,0x12, 0,0,0,0 };
static const uint8_t kHdrS[] = { 0x08,0,0,0, 0x00,0xff,0xff,0xff, 0xf7,0xef,0x08,0xc9, 0x10,0xb7,0x1c,0x8a, 0x20,0x3e,0xe6,0xd3, 0,0,0,0 };

static void DdsHeader(Bytes& b, uint32_t w, uint32_t h, uint32_t mips, bool dxt5) {
    b.insert(b.end(), { 'D', 'D', 'S', ' ' });
    put32(b, 124);
    put32(b, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000);         // caps, height, width, pixelformat, mipcount, linearsize
    put32(b, h); put32(b, w);
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    put32(b, bw * bh * (dxt5 ? 16 : 8));
    put32(b, 0); put32(b, mips);
    for (int i = 0; i < 11; ++i) put32(b, 0);
    put32(b, 32); put32(b, 0x4);                                       // pixel format: FourCC
    b.insert(b.end(), { 'D', 'X', 'T', (uint8_t)(dxt5 ? '5' : '1') });
    for (int i = 0; i < 5; ++i) put32(b, 0);
    put32(b, 0x1000 | 0x8 | 0x400000);                                 // texture, complex, mipmap
    for (int i = 0; i < 4; ++i) put32(b, 0);
}

// RGBA8 image -> full mip chain, DXT-compressed
static void Compress(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, bool dxt5, Bytes& out, uint32_t* mips) {
    std::vector<uint8_t> cur = rgba;
    uint32_t cw = w, ch = h, n = 0;
    for (;;) {
        ++n;
        for (uint32_t by = 0; by < ch; by += 4)
            for (uint32_t bx = 0; bx < cw; bx += 4) {
                uint8_t blk[64];
                for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
                    uint32_t sx = bx + x < cw ? bx + x : cw - 1, sy = by + y < ch ? by + y : ch - 1;
                    memcpy(&blk[(y * 4 + x) * 4], &cur[(sy * cw + sx) * 4], 4);
                }
                uint8_t o[16];
                stb_compress_dxt_block(o, blk, dxt5 ? 1 : 0, STB_DXT_HIGHQUAL);
                out.insert(out.end(), o, o + (dxt5 ? 16 : 8));
            }
        if (cw == 1 && ch == 1) break;
        uint32_t nw = cw > 1 ? cw / 2 : 1, nh = ch > 1 ? ch / 2 : 1;
        std::vector<uint8_t> nx(nw * nh * 4);
        for (uint32_t y = 0; y < nh; ++y) for (uint32_t x = 0; x < nw; ++x) for (int c = 0; c < 4; ++c) {
            uint32_t x0 = x * 2, y0 = y * 2, x1 = x0 + 1 < cw ? x0 + 1 : x0, y1 = y0 + 1 < ch ? y0 + 1 : y0;
            nx[(y * nw + x) * 4 + c] = (uint8_t)((cur[(y0 * cw + x0) * 4 + c] + cur[(y0 * cw + x1) * 4 + c] +
                                                  cur[(y1 * cw + x0) * 4 + c] + cur[(y1 * cw + x1) * 4 + c] + 2) / 4);
        }
        cur.swap(nx); cw = nw; ch = nh;
    }
    *mips = n;
}

static void Wrap(char kind, uint32_t w, uint32_t h, uint32_t mips, const Bytes& data, Bytes& out) {
    out.clear();
    out.insert(out.end(), { 'T', 'B', 'X', 0, 0x70, 0, 0, 0, 0x24, 0, 0, 0 });
    const uint8_t* hd = kind == 'n' ? kHdrN : kind == 's' ? kHdrS : kHdrD;
    out.insert(out.end(), hd, hd + 24);
    DdsHeader(out, w, h, mips, kind == 'n');
    out.insert(out.end(), data.begin(), data.end());
}

// Big textures (over 256) are split like the game's own: foo_d_mip0.xbt holds the top level,
// foo_d.xbt the rest of the chain plus the _mip0 file's game path (0x68-byte header). A big
// texture written as one file crashes the graphics driver when the game streams it.
static const uint8_t kSplitD[] = { 0x01,0xff,0xff,0xff, 0x25,0xd3,0x2b,0x49, 0x3e,0x70,0x36,0x8f, 0xd1,0x8e,0x31,0xcb };
static const uint8_t kSplitN[] = { 0x00,0xff,0xff,0xff, 0x66,0x25,0x3f,0x8a, 0x4b,0x1f,0x94,0xe6, 0x76,0x05,0xbc,0x12 };
static const uint8_t kSplitS[] = { 0x00,0xff,0xff,0xff, 0xf7,0xef,0x08,0xc9, 0x10,0xb7,0x1c,0x8a, 0x20,0x3e,0xe6,0xd3 };

static bool LoadPow2(const Bytes& img, char kind, std::vector<uint8_t>& rgba, int* tw, int* th, std::string* err);

static bool RgbaToXbtSplit(const std::vector<uint8_t>& rgba, int tw, int th, char kind, const std::string& mip0Path, Bytes& mainOut, Bytes& mip0Out, std::string* err) {
    Bytes data; uint32_t mips;
    bool dxt5 = kind == 'n';
    Compress(rgba, tw, th, dxt5, data, &mips);
    mip0Out.clear();
    if (tw <= 256 || th <= 256 || mips < 2) { Wrap(kind, tw, th, mips, data, mainOut); return true; }
    if (mip0Path.size() > 0x68 - 0x20 - 1) { *err = "texture path too long: " + mip0Path; return false; }
    size_t top = (size_t)((tw + 3) / 4) * ((th + 3) / 4) * (dxt5 ? 16 : 8);
    const uint8_t* id = kind == 'n' ? kSplitN : kind == 's' ? kSplitS : kSplitD;
    // _mip0: 0x24 header, flags 4, then the top level alone
    mip0Out.insert(mip0Out.end(), { 'T', 'B', 'X', 0, 0x70, 0, 0, 0, 0x24, 0, 0, 0, 0x04, 0, 0, 0 });
    mip0Out.insert(mip0Out.end(), id, id + 16); put32(mip0Out, 0);
    DdsHeader(mip0Out, tw, th, 1, dxt5);
    mip0Out.insert(mip0Out.end(), data.begin(), data.begin() + top);
    // main: 0x68 header, flags 0x204, the _mip0 path, then the rest of the chain
    mainOut.clear();
    mainOut.insert(mainOut.end(), { 'T', 'B', 'X', 0, 0x70, 0, 0, 0, 0x68, 0, 0, 0, 0x04, 0x02, 0, 0 });
    mainOut.insert(mainOut.end(), id, id + 16);
    size_t at = mainOut.size(); mainOut.resize(0x68, 0);
    memcpy(&mainOut[at], mip0Path.data(), mip0Path.size());
    DdsHeader(mainOut, tw / 2, th / 2, mips - 1, dxt5);
    mainOut.insert(mainOut.end(), data.begin() + top, data.end());
    return true;
}

bool ImageToXbtSplit(const Bytes& img, char kind, const std::string& mip0Path, Bytes& mainOut, Bytes& mip0Out, std::string* err) {
    std::vector<uint8_t> rgba; int tw, th;
    if (!LoadPow2(img, kind, rgba, &tw, &th, err)) return false;
    return RgbaToXbtSplit(rgba, tw, th, kind, mip0Path, mainOut, mip0Out, err);
}

// Up to 4 images side by side in a 2x2 grid (image k at column k%2, row k/2), each cell up to
// 1024 square; a missing/unreadable image is a grey cell. For models with more materials than
// the base weapon has free pieces.
bool AtlasXbtSplit(const std::vector<Bytes>& imgs, const std::string& mip0Path, Bytes& mainOut, Bytes& mip0Out, std::string* err) {
    std::vector<std::vector<uint8_t>> px(imgs.size()); std::vector<int> W(imgs.size(), 0), H(imgs.size(), 0);
    int cell = 64;
    for (size_t k = 0; k < imgs.size() && k < 4; ++k) {
        std::string e;
        if (!imgs[k].empty() && LoadPow2(imgs[k], 'd', px[k], &W[k], &H[k], &e)) cell = std::max(cell, std::max(W[k], H[k]));
    }
    if (cell > 1024) cell = 1024;
    int S = cell * 2;
    std::vector<uint8_t> rgba((size_t)S * S * 4, 150);
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    for (size_t k = 0; k < imgs.size() && k < 4; ++k) {
        if (px[k].empty()) continue;
        int ox = (int)(k % 2) * cell, oy = (int)(k / 2) * cell;
        for (int y = 0; y < cell; ++y) for (int x = 0; x < cell; ++x) {
            int sx = x * W[k] / cell, sy = y * H[k] / cell;
            memcpy(&rgba[((size_t)(oy + y) * S + ox + x) * 4], &px[k][((size_t)sy * W[k] + sx) * 4], 4);
        }
    }
    return RgbaToXbtSplit(rgba, S, S, 'd', mip0Path, mainOut, mip0Out, err);
}

// A full texture set for one material: colour, normal and specular, from up to 4 glTF materials
// (more than one = a 2x2 atlas, same layout as AtlasXbtSplit). For each cell:
//   normal:   its own normal map, or one derived from the colour image (bright = raised)
//   specular: from its metal/roughness map (G = roughness, B = metal), or its roughness/metallic values
struct TexSrc { Bytes colour, normal, mr; float rough = 0.5f, metal = 0.f; bool gameMask = false; };
bool TextureSetXbt(const std::vector<TexSrc>& src, const std::string pathD, const std::string pathN, const std::string pathS,
                   Bytes out[3][2], std::string* err) {
    size_t nc = std::min<size_t>(src.size(), 4);
    if (!nc) { *err = "no material"; return false; }
    int cell = 64;
    std::vector<std::vector<uint8_t>> col(nc), nor(nc), mr(nc); std::vector<int> cw(nc), ch(nc), nw(nc), nh(nc), mw(nc), mh(nc);
    for (size_t k = 0; k < nc; ++k) {
        std::string e;
        if (!src[k].colour.empty() && LoadPow2(src[k].colour, 'd', col[k], &cw[k], &ch[k], &e)) cell = std::max(cell, std::max(cw[k], ch[k]));
        if (!src[k].normal.empty()) LoadPow2(src[k].normal, 'd', nor[k], &nw[k], &nh[k], &e);
        if (!src[k].mr.empty()) LoadPow2(src[k].mr, 'd', mr[k], &mw[k], &mh[k], &e);
    }
    cell = std::min(cell, 1024);
    int S = nc > 1 ? cell * 2 : cell;
    std::vector<uint8_t> D((size_t)S * S * 4), N((size_t)S * S * 4), P((size_t)S * S * 4);
    for (size_t i = 0; i < D.size(); i += 4) { D[i] = D[i + 1] = D[i + 2] = 150; D[i + 3] = 255; N[i] = 128; N[i + 1] = 128; N[i + 2] = 255; N[i + 3] = 255; P[i] = 100; P[i + 1] = 240; P[i + 2] = 128; P[i + 3] = 255; }
    auto sample = [&](const std::vector<uint8_t>& px, int w, int h, int x, int y) { int sx = x * w / cell, sy = y * h / cell; return &px[((size_t)sy * w + sx) * 4]; };
    for (size_t k = 0; k < nc; ++k) {
        int ox = nc > 1 ? (int)(k % 2) * cell : 0, oy = nc > 1 ? (int)(k / 2) * cell : 0;
        // colour + its brightness (for a derived normal map)
        std::vector<float> lum((size_t)cell * cell, 0.5f);
        for (int y = 0; y < cell; ++y) for (int x = 0; x < cell; ++x) {
            uint8_t* d = &D[((size_t)(oy + y) * S + ox + x) * 4];
            if (!col[k].empty()) { memcpy(d, sample(col[k], cw[k], ch[k], x, y), 4); d[3] = 255; }
            lum[(size_t)y * cell + x] = (0.299f * d[0] + 0.587f * d[1] + 0.114f * d[2]) / 255.f;
        }
        for (int y = 0; y < cell; ++y) for (int x = 0; x < cell; ++x) {
            uint8_t* n = &N[((size_t)(oy + y) * S + ox + x) * 4];
            if (!nor[k].empty()) { const uint8_t* q = sample(nor[k], nw[k], nh[k], x, y); n[0] = q[0]; n[1] = q[1]; n[2] = q[2]; }
            else if (!col[k].empty()) {                       // bumps from brightness (Sobel), gentle
                auto L = [&](int xx, int yy) { xx = (xx + cell) % cell; yy = (yy + cell) % cell; return lum[(size_t)yy * cell + xx]; };
                float dx = (L(x + 1, y - 1) + 2 * L(x + 1, y) + L(x + 1, y + 1)) - (L(x - 1, y - 1) + 2 * L(x - 1, y) + L(x - 1, y + 1));
                float dy = (L(x - 1, y + 1) + 2 * L(x, y + 1) + L(x + 1, y + 1)) - (L(x - 1, y - 1) + 2 * L(x, y - 1) + L(x + 1, y - 1));
                float st = 1.5f * cell / 512.f;
                float nx = -dx * st, ny = -dy * st, nz = 1.f, l = sqrtf(nx * nx + ny * ny + nz * nz);
                n[0] = (uint8_t)lroundf((nx / l) * 127 + 128); n[1] = (uint8_t)lroundf((ny / l) * 127 + 128); n[2] = (uint8_t)lroundf((nz / l) * 127 + 128);
            }
            // the game's "_s"/"_m" mask (same layout in the Deagle's and the Kimber's): R = shine strength,
            // G = ambient occlusion (white = open, dark = crevice), B = gloss. A plain grey here made the
            // whole gun "in shadow" (black).
            uint8_t* sp = &P[((size_t)(oy + y) * S + ox + x) * 4];
            if (src[k].gameMask && !mr[k].empty()) {             // already the game's mask: copy it
                const uint8_t* q = sample(mr[k], mw[k], mh[k], x, y); sp[0] = q[0]; sp[1] = q[1]; sp[2] = q[2]; continue;
            }
            float rough = std::min(1.f, std::max(0.f, src[k].rough)), metal = std::min(1.f, std::max(0.f, src[k].metal));
            if (!mr[k].empty()) { const uint8_t* q = sample(mr[k], mw[k], mh[k], x, y); rough = q[1] / 255.f; metal = q[2] / 255.f; }
            float lumv = col[k].empty() ? 0.5f : lum[(size_t)y * cell + x];
            float shine = (1.f - rough) * (0.55f + 0.45f * metal) * (0.7f + 0.6f * lumv);
            float ao = 0.92f;                                     // a touch of cavity from the bump map's slopes
            { float nz = (N[((size_t)(oy + y) * S + ox + x) * 4 + 2] - 128) / 127.f; ao = std::max(0.55f, std::min(1.f, 0.6f + 0.4f * nz)); }
            sp[0] = (uint8_t)std::min(255.f, 40.f + shine * 180.f);
            sp[1] = (uint8_t)std::min(255.f, ao * 255.f);
            sp[2] = (uint8_t)std::min(255.f, (1.f - rough) * 255.f);
        }
    }
    for (size_t i = 0; i < N.size(); i += 4) N[i + 3] = N[i];              // DXT5 normal: X in alpha, Y in green
    const std::string* paths[3] = { &pathD, &pathN, &pathS };
    const char kinds[3] = { 'd', 'n', 's' };
    std::vector<uint8_t>* imgs[3] = { &D, &N, &P };
    for (int t = 0; t < 3; ++t)
        if (!RgbaToXbtSplit(*imgs[t], S, S, kinds[t], Mip0Name(*paths[t]), out[t][0], out[t][1], err)) return false;
    return true;
}

// Any PNG/JPG/TGA -> a diffuse (or normal / specular) .xbt. Width and height are rounded down
// to powers of two (game textures are); up to 2048.
static bool LoadPow2(const Bytes& img, char kind, std::vector<uint8_t>& rgba, int* tw_, int* th_, std::string* err) {
    int w = 0, h = 0, c = 0;
    uint8_t* px = stbi_load_from_memory(img.data(), (int)img.size(), &w, &h, &c, 4);
    if (!px) { *err = std::string("couldn't read the image: ") + stbi_failure_reason(); return false; }
    auto pow2 = [](int v) { int p = 1; while (p * 2 <= v && p < 2048) p *= 2; return p; };
    int tw = pow2(w), th = pow2(h);
    rgba.assign((size_t)tw * th * 4, 0);
    for (int y = 0; y < th; ++y) for (int x = 0; x < tw; ++x) {           // nearest-sample resize to the power-of-two size
        int sx = x * w / tw, sy = y * h / th;
        memcpy(&rgba[((size_t)y * tw + x) * 4], &px[((size_t)sy * w + sx) * 4], 4);
    }
    stbi_image_free(px);
    if (kind == 'n') for (size_t i = 0; i < rgba.size(); i += 4) { rgba[i + 3] = rgba[i]; }   // DXT5 normal: X in alpha, Y in green
    *tw_ = tw; *th_ = th;
    return true;
}

// Single file; only safe up to 256x256 in game (bigger ones: ImageToXbtSplit).
bool ImageToXbt(const Bytes& img, char kind, Bytes& out, std::string* err) {
    std::vector<uint8_t> rgba; int tw, th;
    if (!LoadPow2(img, kind, rgba, &tw, &th, err)) return false;
    Bytes data; uint32_t mips;
    Compress(rgba, tw, th, kind == 'n', data, &mips);
    Wrap(kind, tw, th, mips, data, out);
    return true;
}

// A flat texture: normal map pointing straight out ('n'), or a plain specular level ('s').
void FlatXbt(char kind, uint8_t level, Bytes& out) {
    const uint32_t s = 64;
    std::vector<uint8_t> rgba(s * s * 4);
    for (uint32_t i = 0; i < s * s; ++i) {
        uint8_t* p = &rgba[i * 4];
        if (kind == 'n') { p[0] = 128; p[1] = 128; p[2] = 255; p[3] = 128; }
        else { p[0] = p[1] = p[2] = level; p[3] = 255; }
    }
    Bytes data; uint32_t mips;
    Compress(rgba, s, s, kind == 'n', data, &mips);
    Wrap(kind, s, s, mips, data, out);
}

// Copy of a material with its texture paths swapped. `swaps`: old path (as in the file,
// case-insensitive) -> new path. New paths must not be longer than the old ones.
bool MaterialWithTextures(const Bytes& mat, const std::vector<std::pair<std::string, std::string>>& swaps, Bytes& out, std::string* err) {
    if (mat.size() < 24 || memcmp(mat.data(), "TAM", 3) != 0) { *err = "not a material file"; return false; }
    out = mat;
    int done = 0;
    for (auto& s : swaps) {
        const std::string& a = s.first; const std::string& b = s.second;
        if (b.size() > a.size()) { *err = "new texture path is longer than the old one: " + b; return false; }
        for (size_t i = 20; i + a.size() < out.size(); ++i) {
            bool eq = true;
            for (size_t k = 0; k < a.size() && eq; ++k) eq = tolower(out[i + k]) == tolower((unsigned char)a[k]);
            if (!eq || out[i + a.size()] != 0) continue;
            memset(&out[i], 0, a.size());
            memcpy(&out[i], b.data(), b.size());
            ++done;
        }
    }
    if (!done) { *err = "none of the material's texture paths were found"; return false; }
    return true;
}

// The texture paths a material uses (to know what to swap).
std::vector<std::string> MaterialTextures(const Bytes& mat) {
    std::vector<std::string> r; std::string cur;
    for (size_t i = 20; i <= mat.size(); ++i) {
        char c = i < mat.size() ? (char)mat[i] : 0;
        if (c >= 0x20 && c < 0x7f) { cur += c; continue; }
        if (cur.size() > 5) {
            std::string l = cur; for (auto& ch : l) ch = (char)tolower((unsigned char)ch);
            size_t g = l.find("graphics\\");
            if (g != std::string::npos && l.size() > 4 && l.compare(l.size() - 4, 4, ".xbt") == 0) r.push_back(cur.substr(g));
        }
        cur.clear();
    }
    return r;
}

} // namespace fc4
