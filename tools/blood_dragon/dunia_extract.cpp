// dunia_extract: unpack Dunia (Far Cry 3 / Blood Dragon / Far Cry 4) .fat/.dat archives into a folder.
// Files are named <id hex>.<type>, sorted into folders by type (guessed from the first bytes), and listed
// in index.csv (id, archive, size, type).   usage: dunia_extract <game data_win32 folder> <output folder>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <set>
#include <algorithm>
extern "C" bool Lzo1xDecompress(const uint8_t*, size_t, uint8_t*, size_t, size_t*);
namespace fs = std::filesystem;
struct E { uint64_t id; uint32_t usize, csize; uint8_t scheme; uint64_t off; };
static uint32_t u32(const std::vector<uint8_t>& d, size_t o) { uint32_t v; memcpy(&v, &d[o], 4); return v; }
static bool readAll(const fs::path& p, std::vector<uint8_t>& d) {
    FILE* f = _wfopen(p.c_str(), L"rb"); if (!f) return false;
    _fseeki64(f, 0, SEEK_END); long long n = _ftelli64(f); _fseeki64(f, 0, SEEK_SET);
    d.resize((size_t)n); bool ok = fread(d.data(), 1, d.size(), f) == d.size(); fclose(f); return ok;
}
static std::vector<E> loadFat(const std::vector<uint8_t>& d) {
    std::vector<E> out; if (d.size() < 0x18 || u32(d, 0) != 0x46415432) return out;
    uint32_t n = u32(d, 0x14);
    auto add = [&](size_t o) { E e; e.id = (uint64_t)u32(d, o) << 32 | u32(d, o + 4); uint32_t c = u32(d, o + 8), dd = u32(d, o + 12), x = u32(d, o + 16);
        e.usize = c >> 2; e.scheme = c & 3; e.off = ((uint64_t)dd << 2) | (x >> 30); e.csize = x & 0x3fffffff; out.push_back(e); };
    for (uint32_t i = 0; i < n && 0x18 + (size_t)i * 20 + 20 <= d.size(); ++i) add(0x18 + (size_t)i * 20);
    uint32_t sub = u32(d, 0x10); size_t p = 0x18 + (size_t)n * 20;
    for (int v = 0; v < 3 && sub && sub < 100000; ++v) {
        size_t q = p; bool ok = true; std::vector<size_t> s;
        for (int t = 0; t < 2 - v && ok; ++t) { if (q + 4 > d.size()) { ok = false; break; } q += 4 + (size_t)u32(d, q) * 16; }
        for (uint32_t k = 0; k < sub && ok; ++k) { if (q + 4 > d.size()) { ok = false; break; } uint32_t c = u32(d, q); q += 4;
            if (q + (size_t)c * 20 > d.size()) { ok = false; break; } for (uint32_t i = 0; i < c; ++i) s.push_back(q + i * 20); q += (size_t)c * 20; }
        if (ok && q == d.size()) { for (size_t o : s) add(o); break; }
    }
    return out;
}
static std::string typeOf(const std::vector<uint8_t>& x) {
    if (x.size() < 4) return "bin";
    auto is = [&](const char* m) { return memcmp(x.data(), m, strlen(m)) == 0; };
    if (is("TBX")) return "xbt"; if (is("nbCF")) return "fcb"; if (is("MVM")) return "mvm"; if (is("BIK")) return "bik";
    if (is("HSEM")) return "xbg"; if (is("aNi")) return "ani"; if (is("<?xml") || is("<") || is("\xef\xbb\xbf<")) return "xml";
    if (is("UEF")) return "fev"; if (is("\x04KPS")) return "spk"; if (is("\x57\xe0\xe0\x57")) return "hkx";
    if (x.size() > 8 && memcmp(x.data() + 4, "\x10\xc0\xc0\x10", 4) == 0) return "hkx";
    char b[16]; snprintf(b, sizeof b, "m%02x%02x%02x%02x", x[0], x[1], x[2], x[3]); return b;
}
int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { printf("usage: dunia_extract <data_win32 folder> <output folder> [ids.txt]\n"); return 1; }
    fs::path in = argv[1], out = argv[2]; fs::create_directories(out);
    std::set<uint64_t> only;                                     // optional: just these ids (hex, one per line)
    if (argc > 3) { FILE* f = _wfopen(argv[3], L"r"); char ln[128];
        while (f && fgets(ln, sizeof ln, f)) { uint64_t v = strtoull(ln, nullptr, 16); if (v) only.insert(v); } if (f) fclose(f);
        printf("extracting %zu listed files\n", only.size()); }
    FILE* csv = _wfopen((out / L"index.csv").c_str(), L"w"); fprintf(csv, "id,archive,size,type\n");
    std::map<uint64_t, bool> seen; size_t files = 0, bytes = 0;
    std::vector<fs::path> fats;
    for (auto& e : fs::recursive_directory_iterator(in)) if (e.path().extension() == L".fat") fats.push_back(e.path());
    // patch first (newest wins), like the game
    std::stable_sort(fats.begin(), fats.end(), [](const fs::path& a, const fs::path& b) { return (a.filename().wstring().rfind(L"patch", 0) == 0) > (b.filename().wstring().rfind(L"patch", 0) == 0); });
    for (auto& fp : fats) {
        std::vector<uint8_t> fd; if (!readAll(fp, fd)) continue;
        auto es = loadFat(fd); fs::path dp = fp; dp.replace_extension(L".dat");
        FILE* df = _wfopen(dp.c_str(), L"rb"); if (!df) continue;
        printf("%ls: %zu files\n", fp.filename().c_str(), es.size());
        std::string arc = fp.filename().string();
        for (auto& e : es) {
            if (!only.empty() && !only.count(e.id)) continue;
            if (seen[e.id]) continue; seen[e.id] = true;
            std::vector<uint8_t> raw(e.csize), x;
            _fseeki64(df, (long long)e.off, SEEK_SET); if (e.csize && fread(raw.data(), 1, e.csize, df) != e.csize) continue;
            if (e.scheme == 0 || e.usize == 0) x.swap(raw);
            else { x.resize(e.usize); size_t got = 0; if (!Lzo1xDecompress(raw.data(), raw.size(), x.data(), x.size(), &got)) continue; x.resize(got); }
            std::string t = typeOf(x); char nm[64]; snprintf(nm, sizeof nm, "%016llx.%s", (unsigned long long)e.id, t.c_str());
            fs::path dir = out / t; fs::create_directories(dir);
            FILE* o = _wfopen((dir / nm).c_str(), L"wb"); if (o) { fwrite(x.data(), 1, x.size(), o); fclose(o); }
            fprintf(csv, "%016llx,%s,%zu,%s\n", (unsigned long long)e.id, arc.c_str(), x.size(), t.c_str());
            ++files; bytes += x.size();
        }
        fclose(df);
    }
    fclose(csv); printf("done: %zu files, %zu MB in %ls\n", files, bytes >> 20, out.c_str()); return 0;
}
