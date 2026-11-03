// Minimal JSON reader for glTF files (shared by glbwrite.cpp and aniclip.cpp).
#pragma once
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <utility>
namespace fc4 {
// ------------------------------------------------------------------ tiny JSON
struct J {
    enum T { NUL, NUM, STR, ARR, OBJ, BOOL } t = NUL;
    double n = 0; std::string s; std::vector<J> a; std::vector<std::pair<std::string, J>> o;
    const J& operator[](const char* k) const { static J none; if (t == OBJ) for (auto& p : o) if (p.first == k) return p.second; return none; }
    const J& operator[](size_t i) const { static J none; return t == ARR && i < a.size() ? a[i] : none; }
    bool has(const char* k) const { return (*this)[k].t != NUL; }
    int i(int def = -1) const { return t == NUM ? (int)n : def; }
    size_t size() const { return t == ARR ? a.size() : 0; }
};
struct JP {
    const char* p; const char* e; bool bad = false;
    void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p; }
    std::string str() {
        std::string r; ++p;
        while (p < e && *p != '"') {
            if (*p == '\\' && p + 1 < e) {
                ++p;
                char c = *p;
                if (c == 'u') { unsigned v = 0; for (int k = 0; k < 4 && p + 1 < e; ++k) { ++p; v = v * 16 + (unsigned)(isdigit((unsigned char)*p) ? *p - '0' : (tolower(*p) - 'a' + 10)); } r += v < 128 ? (char)v : '?'; }
                else r += c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c == 'b' ? '\b' : c == 'f' ? '\f' : c;
            } else r += *p;
            ++p;
        }
        ++p; return r;
    }
    J val(int depth = 0) {
        J j; ws();
        if (p >= e || depth > 64) { bad = true; return j; }
        if (*p == '{') {
            j.t = J::OBJ; ++p; ws();
            if (p < e && *p == '}') { ++p; return j; }
            while (p < e && !bad) {
                ws(); if (p >= e || *p != '"') { bad = true; break; }
                std::string k = str(); ws();
                if (p >= e || *p != ':') { bad = true; break; }
                ++p; j.o.emplace_back(k, val(depth + 1)); ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == '}') { ++p; break; }
                bad = true;
            }
        } else if (*p == '[') {
            j.t = J::ARR; ++p; ws();
            if (p < e && *p == ']') { ++p; return j; }
            while (p < e && !bad) {
                j.a.push_back(val(depth + 1)); ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == ']') { ++p; break; }
                bad = true;
            }
        } else if (*p == '"') { j.t = J::STR; j.s = str(); }
        else if (!strncmp(p, "true", 4) && e - p >= 4) { j.t = J::BOOL; j.n = 1; p += 4; }
        else if (!strncmp(p, "false", 5) && e - p >= 5) { j.t = J::BOOL; p += 5; }
        else if (!strncmp(p, "null", 4) && e - p >= 4) { p += 4; }
        else { char* q; j.n = strtod(p, &q); if (q == p) bad = true; j.t = J::NUM; p = q; }
        return j;
    }
};

} // namespace fc4
