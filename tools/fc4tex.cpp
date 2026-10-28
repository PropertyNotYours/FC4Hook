// fc4tex: Far Cry 4 texture converter (drag and drop files onto fc4tex.exe).
//   foo_d.xbt (or foo_d_mip0.xbt) -> foo_d.dds   (both halves joined; the other half must be next to it)
//   foo_d.dds                    -> foo_d.xbt + foo_d_mip0.xbt in a "packed" folder next to it
//                                   (needs the original foo_d.xbt / foo_d_mip0.xbt next to the .dds)
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
namespace fc4 {
typedef std::vector<uint8_t> Bytes;
bool XbtToDds(const Bytes&, const Bytes*, Bytes&, std::string*);
bool DdsToXbt(const Bytes&, const Bytes&, const Bytes*, Bytes&, Bytes&, std::string*);
std::string Mip0Name(const std::string&); bool IsMip0Name(const std::string&, std::string*);
}
using fc4::Bytes;
static bool Rd(const std::string& p, Bytes& b) { FILE* f = fopen(p.c_str(), "rb"); if (!f) return false; b.clear(); int c; char buf[65536]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n); fclose(f); (void)c; return !b.empty(); }
static bool Wr(const std::string& p, const Bytes& b) { FILE* f = fopen(p.c_str(), "wb"); if (!f) return false; bool ok = fwrite(b.data(), 1, b.size(), f) == b.size(); fclose(f); return ok; }
static std::string Lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; }
static std::string NameOf(const std::string& p) { size_t s = p.find_last_of("\\/"); return s == std::string::npos ? p : p.substr(s + 1); }
static std::string DirOf(const std::string& p) { size_t s = p.find_last_of("\\/"); return s == std::string::npos ? "" : p.substr(0, s + 1); }

static int One(const std::string& path) {
    std::string l = Lower(path), err;
    if (l.size() > 4 && l.substr(l.size() - 4) == ".xbt") {
        std::string base = path; fc4::IsMip0Name(path, &base);
        Bytes d, m, dds;
        if (!Rd(base, d)) { printf("  %s: can't read %s\n", NameOf(path).c_str(), NameOf(base).c_str()); return 1; }
        bool hm = Rd(fc4::Mip0Name(base), m);
        if (!fc4::XbtToDds(d, hm ? &m : nullptr, dds, &err)) { printf("  %s: %s\n", NameOf(path).c_str(), err.c_str()); return 1; }
        std::string out = base.substr(0, base.size() - 4) + ".dds";
        if (!Wr(out, dds)) { printf("  can't write %s\n", out.c_str()); return 1; }
        printf("  %s -> %s%s\n", NameOf(base).c_str(), NameOf(out).c_str(), hm ? " (with its _mip0 half)" : "");
        return 0;
    }
    if (l.size() > 4 && l.substr(l.size() - 4) == ".dds") {
        std::string base = path.substr(0, path.size() - 4) + ".xbt", mip0 = fc4::Mip0Name(base);
        Bytes dds, d, m, od, om;
        if (!Rd(path, dds)) { printf("  can't read %s\n", path.c_str()); return 1; }
        if (!Rd(base, d)) { printf("  %s: put the original %s next to it\n", NameOf(path).c_str(), NameOf(base).c_str()); return 1; }
        bool hm = Rd(mip0, m);
        if (!fc4::DdsToXbt(dds, d, hm ? &m : nullptr, od, om, &err)) { printf("  %s: %s\n", NameOf(path).c_str(), err.c_str()); return 1; }
        std::string dir = DirOf(path) + "packed";
#ifdef _WIN32
        std::string mk = "mkdir \"" + dir + "\" 2>nul"; system(mk.c_str());
        std::string sep = "\\";
#else
        std::string mk = "mkdir -p \"" + dir + "\""; if (system(mk.c_str())) {}
        std::string sep = "/";
#endif
        bool ok = Wr(dir + sep + NameOf(base), od) && (!hm || Wr(dir + sep + NameOf(mip0), om));
        if (!ok) { printf("  can't write into %s\n", dir.c_str()); return 1; }
        printf("  %s -> packed%s%s%s\n", NameOf(path).c_str(), sep.c_str(), NameOf(base).c_str(), hm ? (" + " + NameOf(mip0)).c_str() : "");
        return 0;
    }
    printf("  %s: not an .xbt or .dds\n", NameOf(path).c_str());
    return 1;
}

int main(int argc, char** argv) {
    printf("fc4tex - Far Cry 4 texture converter (FC4Hook)\n");
    if (argc < 2) {
        printf("\nDrag .xbt files onto fc4tex.exe to get .dds files you can edit.\n"
               "Drag edited .dds files onto it to get game .xbt files (in a 'packed' folder).\n"
               "Keep the original .xbt files (and their _mip0 halves) next to the .dds.\n"
               "Save DDS files in the game's format (usually DXT1/BC1) with mipmaps.\n");
    }
    int bad = 0;
    for (int i = 1; i < argc; ++i) bad += One(argv[i]);
    printf("\n%s Press Enter to close.\n", bad ? "Some files failed (see above)." : "Done.");
    getchar();
    return bad ? 1 : 0;
}
