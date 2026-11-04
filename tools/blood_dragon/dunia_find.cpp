// dunia_find <folder> <out.txt> <word> [word...]: every printable string (>= 6 chars) in any file under
// <folder> that contains one of the words (case-insensitive), with the file it came from.
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
namespace fs = std::filesystem;
int wmain(int argc, wchar_t** argv) {
    if (argc < 4) { printf("usage: dunia_find <folder> <out.txt> <word> [word...]\n"); return 1; }
    std::vector<std::string> w; for (int i = 3; i < argc; ++i) { std::wstring s = argv[i]; std::string a(s.begin(), s.end()); for (auto& c : a) c = (char)tolower((unsigned char)c); w.push_back(a); }
    FILE* o = _wfopen(argv[2], L"w"); size_t n = 0, files = 0;
    for (auto& e : fs::recursive_directory_iterator(argv[1])) {
        if (!e.is_regular_file()) continue; ++files;
        FILE* f = _wfopen(e.path().c_str(), L"rb"); if (!f) continue;
        std::vector<char> d(e.file_size()); size_t got = fread(d.data(), 1, d.size(), f); fclose(f); d.resize(got);
        std::string cur;
        auto flush = [&] {
            if (cur.size() >= 6) { std::string l = cur; for (auto& c : l) c = (char)tolower((unsigned char)c);
                for (auto& x : w) if (l.find(x) != std::string::npos) { fprintf(o, "%s\t%s\n", e.path().filename().string().c_str(), cur.c_str()); ++n; break; } }
            cur.clear(); };
        for (char c : d) { if (c >= 32 && c < 127) cur += c; else flush(); }
        flush();
    }
    fclose(o); printf("searched %zu files, %zu matches -> %ls\n", files, n, argv[2]); return 0;
}
