// fc4mdl: turn an EDITED game model (.obj exported from Blender) back into a Far Cry 4 .xbg.
//   fc4mdl original.xbg edited.obj [out.xbg]
// or drag the two files onto fc4mdl.exe. The original .xbg is the one you imported into Blender
// (F8 > Developer > Models > Extract). Only vertex positions change: keep the vertex count and
// order (move/scale/rotate vertices; don't add or delete any). Output: <original>_new.xbg.
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
namespace fc4 { bool XbgFromObj(std::vector<uint8_t>& xbg, const std::string& objText, std::string* report); }

static bool ReadAll(const char* p, std::vector<uint8_t>& d) {
    FILE* f = fopen(p, "rb"); if (!f) return false;
    uint8_t b[65536]; size_t n; d.clear();
    while ((n = fread(b, 1, sizeof(b), f)) > 0) d.insert(d.end(), b, b + n);
    fclose(f); return true;
}
static bool EndsWith(const std::string& s, const char* e) { std::string l = s; for (auto& c : l) c = (char)tolower((unsigned char)c); std::string x = e; return l.size() >= x.size() && l.compare(l.size() - x.size(), x.size(), x) == 0; }

int main(int argc, char** argv) {
    std::string xbgPath, objPath, out;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (EndsWith(a, ".xbg") && xbgPath.empty()) xbgPath = a;
        else if (EndsWith(a, ".obj")) objPath = a;
        else if (EndsWith(a, ".xbg")) out = a;
    }
    if (xbgPath.empty() || objPath.empty()) {
        printf("fc4mdl - edited .obj -> Far Cry 4 .xbg\n\n  fc4mdl original.xbg edited.obj [out.xbg]\n\n"
               "Or drag both files onto fc4mdl.exe. The .obj must be the original model with its vertices\n"
               "moved/scaled/rotated (same count, same order).\n\nPress Enter.");
        getchar(); return 1;
    }
    if (out.empty()) out = xbgPath.substr(0, xbgPath.size() - 4) + "_new.xbg";
    std::vector<uint8_t> xbg, obj;
    if (!ReadAll(xbgPath.c_str(), xbg)) { printf("can't read %s\n", xbgPath.c_str()); getchar(); return 1; }
    if (!ReadAll(objPath.c_str(), obj)) { printf("can't read %s\n", objPath.c_str()); getchar(); return 1; }
    std::string rep;
    if (!fc4::XbgFromObj(xbg, std::string(obj.begin(), obj.end()), &rep)) { printf("Not converted: %s\n\nPress Enter.", rep.c_str()); getchar(); return 1; }
    FILE* f = fopen(out.c_str(), "wb");
    if (!f || fwrite(xbg.data(), 1, xbg.size(), f) != xbg.size()) { printf("can't write %s\n", out.c_str()); getchar(); return 1; }
    fclose(f);
    printf("%s\nWrote %s\n", rep.c_str(), out.c_str());
    if (argc <= 3) { printf("\nPress Enter."); getchar(); }
    return 0;
}
