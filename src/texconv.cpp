// .xbt <-> .dds for Far Cry 4 textures. Plain C++ (no Windows), shared by fc4hook.asi and
// tools/fc4tex.exe.
//
// An .xbt is "TBX\0", version (0x70), offset of the DDS data, flags, ids, an optional file
// name, then an ordinary DDS file. Big textures are split in two:
//   foo_d_mip0.xbt  the top mip level(s)                     (e.g. 2048x2048, 1 mip)
//   foo_d.xbt       the rest of the chain + the mip0 file name (1024x1024 .. 1x1, 11 mips)
// XbtToDds joins them into one DDS with the full mip chain; DdsToXbt splits an edited DDS
// back, reusing the original files' Ubisoft headers.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace fc4 {

typedef std::vector<uint8_t> Bytes;

static uint32_t rd32(const Bytes& d, size_t o) { return o + 4 <= d.size() ? (uint32_t)d[o] | d[o + 1] << 8 | d[o + 2] << 16 | (uint32_t)d[o + 3] << 24 : 0; }
static void wr32(Bytes& d, size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) d[o + i] = (uint8_t)(v >> (8 * i)); }

struct DdsInfo { size_t hdr = 0; uint32_t w = 0, h = 0, mips = 1, block = 0, bpp = 0; std::string fmt; };

// Parse a DDS starting at `at`. block = bytes per 4x4 block (0 for uncompressed, then bpp is used).
static bool ParseDds(const Bytes& d, size_t at, DdsInfo* di, std::string* err) {
    if (d.size() < at + 128 || memcmp(&d[at], "DDS ", 4) != 0) { *err = "not a DDS file"; return false; }
    di->h = rd32(d, at + 12); di->w = rd32(d, at + 16);
    uint32_t flags = rd32(d, at + 8);
    di->mips = (flags & 0x20000) && rd32(d, at + 28) ? rd32(d, at + 28) : 1;
    uint32_t pfFlags = rd32(d, at + 80);
    di->hdr = 128;
    if (pfFlags & 4) {                                           // FourCC
        char cc[5] = { (char)d[at + 84], (char)d[at + 85], (char)d[at + 86], (char)d[at + 87], 0 };
        di->fmt = cc;
        std::string f = cc;
        if (f == "DXT1" || f == "ATI1" || f == "BC4U" || f == "BC4S") di->block = 8;
        else if (f == "DXT2" || f == "DXT3" || f == "DXT4" || f == "DXT5" || f == "ATI2" || f == "BC5U" || f == "BC5S") di->block = 16;
        else if (f == "DX10") {
            if (d.size() < at + 148) { *err = "truncated DX10 header"; return false; }
            uint32_t g = rd32(d, at + 128);
            di->hdr = 148;
            di->fmt = "DX10/" + std::to_string(g);
            if ((g >= 70 && g <= 72) || (g >= 79 && g <= 81)) di->block = 8;                 // BC1, BC4
            else if ((g >= 73 && g <= 78) || (g >= 82 && g <= 84) || (g >= 94 && g <= 99)) di->block = 16;  // BC2/3/5/6/7
            else if (g == 28 || g == 29 || g == 87 || g == 88 || g == 91) di->bpp = 32;
            else { *err = "unsupported DX10 format " + std::to_string(g); return false; }
        } else { *err = "unsupported format " + f; return false; }
    } else {
        di->bpp = rd32(d, at + 88);
        di->fmt = "RGB" + std::to_string(di->bpp);
        if (!di->bpp || di->bpp % 8) { *err = "unsupported pixel format"; return false; }
    }
    if (!di->w || !di->h || di->mips > 16) { *err = "bad DDS header"; return false; }
    return true;
}

static size_t MipSize(const DdsInfo& di, uint32_t level) {
    uint32_t w = di.w >> level, h = di.h >> level;
    if (!w) w = 1; if (!h) h = 1;
    if (di.block) return (size_t)((w + 3) / 4) * ((h + 3) / 4) * di.block;
    return (size_t)w * h * (di.bpp / 8);
}

static size_t ChainSize(const DdsInfo& di, uint32_t first, uint32_t count) {
    size_t n = 0;
    for (uint32_t i = 0; i < count; ++i) n += MipSize(di, first + i);
    return n;
}

static bool XbtSplit(const Bytes& x, size_t* ddsAt, std::string* err) {
    if (x.size() < 16 || memcmp(x.data(), "TBX\0", 4) != 0) { *err = "not an .xbt file"; return false; }
    *ddsAt = rd32(x, 8);
    if (*ddsAt + 128 > x.size() || memcmp(&x[*ddsAt], "DDS ", 4) != 0) { *err = "no DDS inside the .xbt"; return false; }
    return true;
}

// Rewrite a DDS header (128 or 148 bytes) for a new size / mip count.
static Bytes MakeHeader(const Bytes& src, size_t at, const DdsInfo& di, uint32_t w, uint32_t h, uint32_t mips) {
    Bytes hd(src.begin() + at, src.begin() + at + di.hdr);
    DdsInfo t = di; t.w = w; t.h = h;
    wr32(hd, 12, h); wr32(hd, 16, w);
    uint32_t flags = rd32(hd, 8);
    flags = mips > 1 ? (flags | 0x20000) : (flags & ~0x20000u);
    wr32(hd, 8, flags);
    wr32(hd, 20, (uint32_t)(di.block ? MipSize(t, 0) : (size_t)w * (di.bpp / 8)));   // linear size / pitch
    wr32(hd, 28, mips > 1 ? mips : 0);
    uint32_t caps = rd32(hd, 108) | 0x1000;
    caps = mips > 1 ? (caps | 0x400008) : (caps & ~0x400008u);
    wr32(hd, 108, caps);
    return hd;
}

// The game's own header (from the original .xbt), resized: keeps its flags/caps/format exactly.
static Bytes TemplateHeader(const Bytes& tmpl, size_t at, const DdsInfo& ti, uint32_t w, uint32_t h, uint32_t mips) {
    Bytes hd(tmpl.begin() + at, tmpl.begin() + at + ti.hdr);
    DdsInfo t = ti; t.w = w; t.h = h;
    wr32(hd, 12, h); wr32(hd, 16, w);
    wr32(hd, 20, (uint32_t)(ti.block ? MipSize(t, 0) : (size_t)w * (ti.bpp / 8)));
    if (mips > 1) { wr32(hd, 8, rd32(hd, 8) | 0x20000); wr32(hd, 108, rd32(hd, 108) | 0x400008); }
    if (rd32(hd, 8) & 0x20000) wr32(hd, 28, mips);
    return hd;
}

// d = foo.xbt, mip0 = foo_mip0.xbt (or null) -> one DDS with every mip level.
bool XbtToDds(const Bytes& d, const Bytes* mip0, Bytes& out, std::string* err) {
    size_t at; DdsInfo di;
    if (!XbtSplit(d, &at, err) || !ParseDds(d, at, &di, err)) return false;
    if (!mip0) { out.assign(d.begin() + at, d.end()); return true; }
    size_t at0; DdsInfo d0;
    if (!XbtSplit(*mip0, &at0, err) || !ParseDds(*mip0, at0, &d0, err)) { *err = "mip0 file: " + *err; return false; }
    if (d0.block != di.block || d0.bpp != di.bpp) { *err = "the two files have different formats"; return false; }
    if ((d0.w >> d0.mips) != di.w || (d0.h >> d0.mips) != di.h) { *err = "the mip0 file doesn't belong to this texture (sizes don't line up)"; return false; }
    size_t n0 = ChainSize(d0, 0, d0.mips), n1 = ChainSize(di, 0, di.mips);
    if (mip0->size() < at0 + d0.hdr + n0 || d.size() < at + di.hdr + n1) { *err = "texture data is shorter than its header says"; return false; }
    out = MakeHeader(*mip0, at0, d0, d0.w, d0.h, d0.mips + di.mips);
    out.insert(out.end(), mip0->begin() + at0 + d0.hdr, mip0->begin() + at0 + d0.hdr + n0);
    out.insert(out.end(), d.begin() + at + di.hdr, d.begin() + at + di.hdr + n1);
    return true;
}

// Edited DDS -> new foo.xbt (+ foo_mip0.xbt when the original had one), keeping the originals'
// Ubisoft headers. The DDS must have mipmaps (all levels) in the same family of format.
bool DdsToXbt(const Bytes& dds, const Bytes& tmplD, const Bytes* tmplMip0, Bytes& outD, Bytes& outMip0, std::string* err) {
    DdsInfo e, td;
    size_t atD;
    if (!ParseDds(dds, 0, &e, err)) return false;
    if (!XbtSplit(tmplD, &atD, err) || !ParseDds(tmplD, atD, &td, err)) { *err = "original .xbt: " + *err; return false; }
    if (e.block != td.block || e.bpp != td.bpp)
        { *err = "format is " + e.fmt + " but the game's texture is " + td.fmt + ": save the DDS as " + td.fmt; return false; }
    uint32_t top = 0;
    if (tmplMip0) {
        size_t at0; DdsInfo t0;
        if (!XbtSplit(*tmplMip0, &at0, err) || !ParseDds(*tmplMip0, at0, &t0, err)) { *err = "original mip0 .xbt: " + *err; return false; }
        top = t0.mips;
    }
    uint32_t full = 1; for (uint32_t s = e.w > e.h ? e.w : e.h; s > 1; s >>= 1) ++full;
    if (e.mips < top + 1)
        { *err = "the DDS has no mipmaps: save it again with \"generate mip maps\" ticked"; return false; }
    if (e.mips != full)
        { *err = "the DDS has " + std::to_string(e.mips) + " mip levels, it needs all " + std::to_string(full) + " (\"generate mip maps\")"; return false; }
    if (dds.size() < e.hdr + ChainSize(e, 0, e.mips)) { *err = "the DDS is shorter than its header says"; return false; }
    size_t pos = e.hdr;
    if (tmplMip0) {
        size_t at0; DdsInfo t0; XbtSplit(*tmplMip0, &at0, err); ParseDds(*tmplMip0, at0, &t0, err);
        size_t n0 = ChainSize(e, 0, top);
        outMip0.assign(tmplMip0->begin(), tmplMip0->begin() + at0);
        Bytes h = TemplateHeader(*tmplMip0, at0, t0, e.w, e.h, top);
        outMip0.insert(outMip0.end(), h.begin(), h.end());
        outMip0.insert(outMip0.end(), dds.begin() + pos, dds.begin() + pos + n0);
        pos += n0;
    }
    uint32_t rest = e.mips - top;
    uint32_t w = e.w >> top, hh = e.h >> top;
    if (!w) w = 1; if (!hh) hh = 1;
    size_t n1 = ChainSize(e, top, rest);
    outD.assign(tmplD.begin(), tmplD.begin() + atD);
    Bytes h = TemplateHeader(tmplD, atD, td, w, hh, rest);
    outD.insert(outD.end(), h.begin(), h.end());
    outD.insert(outD.end(), dds.begin() + pos, dds.begin() + pos + n1);
    return true;
}

// "graphics\x\foo_d.xbt" -> "graphics\x\foo_d_mip0.xbt" (and back with IsMip0Name)
std::string Mip0Name(const std::string& xbt) {
    size_t dot = xbt.rfind('.');
    return dot == std::string::npos ? xbt + "_mip0" : xbt.substr(0, dot) + "_mip0" + xbt.substr(dot);
}
bool IsMip0Name(const std::string& xbt, std::string* base) {
    size_t p = xbt.rfind("_mip0.");
    if (p == std::string::npos) return false;
    if (base) *base = xbt.substr(0, p) + xbt.substr(p + 5);
    return true;
}

} // namespace fc4
