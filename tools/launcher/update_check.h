// GitHub #119: the launcher's update check - the pure part (no SDL, no processes), so tests/unit/test_update_check.cpp
// can run it on its own. The launcher downloads https://api.github.com/repos/<repo>/releases/latest with the system's
// curl (Windows 10+ ships curl.exe; Linux distributions have curl), then:
//   update_parse_release()   reads the tag, title, notes, page and the package for this platform from that JSON
//   update_compare()          orders two versions ("v14.0.1", "14.0.2", "14.1.0-rc1")
// It only tells the player and opens the download in their browser: a release package holds the game code the
// launcher built from the player's own dump, their saves and settings, so replacing files is left to the player.
#pragma once
#include <stdlib.h>
#include <string.h>
#include <string>
#include <utility>
#include <vector>

// ------------------------------------------------------------------ a minimal JSON reader (what the release API returns)
struct UpdJson {
    enum Kind { NUL, BOOL, NUM, STR, ARR, OBJ } kind = NUL;
    std::string str;  // STR: the decoded text (UTF-8); NUM / BOOL: the literal
    std::vector<UpdJson> arr;
    std::vector<std::pair<std::string, UpdJson>> obj;
    const UpdJson *get(const char *key) const {
        if (kind != OBJ) return nullptr;
        for (const auto &kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string text(const char *key) const {  // a string member, "" when missing or not a string
        const UpdJson *v = get(key);
        return v && v->kind == STR ? v->str : std::string();
    }
    bool flag(const char *key) const { const UpdJson *v = get(key); return v && v->kind == BOOL && v->str == "true"; }
};

struct UpdJsonParser {
    const char *p, *end;
    int depth = 0;
    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }
    static void utf8(std::string &o, unsigned c) {
        if (c < 0x80) o += (char)c;
        else if (c < 0x800) o += (char)(0xC0 | c >> 6), o += (char)(0x80 | (c & 63));
        else if (c < 0x10000) o += (char)(0xE0 | c >> 12), o += (char)(0x80 | (c >> 6 & 63)), o += (char)(0x80 | (c & 63));
        else o += (char)(0xF0 | c >> 18), o += (char)(0x80 | (c >> 12 & 63)), o += (char)(0x80 | (c >> 6 & 63)), o += (char)(0x80 | (c & 63));
    }
    bool hex4(unsigned &c) {
        if (end - p < 4) return false;
        c = 0;
        for (int i = 0; i < 4; i++) {
            char h = *p++;
            c <<= 4;
            if (h >= '0' && h <= '9') c |= h - '0';
            else if (h >= 'a' && h <= 'f') c |= h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') c |= h - 'A' + 10;
            else return false;
        }
        return true;
    }
    bool string(std::string &o) {
        if (p >= end || *p != '"') return false;
        p++;
        while (p < end && *p != '"') {
            if (*p != '\\') { o += *p++; continue; }
            if (++p >= end) return false;
            char e = *p++;
            switch (e) {
            case '"': o += '"'; break;
            case '\\': o += '\\'; break;
            case '/': o += '/'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'n': o += '\n'; break;
            case 'r': o += '\r'; break;
            case 't': o += '\t'; break;
            case 'u': {
                unsigned c;
                if (!hex4(c)) return false;
                if (c >= 0xD800 && c < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {  // a surrogate pair
                    const char *keep = p;
                    p += 2;
                    unsigned lo;
                    if (hex4(lo) && lo >= 0xDC00 && lo < 0xE000) c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                    else p = keep;
                }
                if (c >= 0xD800 && c < 0xE000) c = 0xFFFD;  // (a lone surrogate)
                utf8(o, c);
                break;
            }
            default: return false;
            }
        }
        if (p >= end) return false;
        p++;
        return true;
    }
    bool value(UpdJson &v) {
        ws();
        if (p >= end || ++depth > 64) return false;
        bool ok = true;
        if (*p == '{') {
            v.kind = UpdJson::OBJ;
            p++;
            ws();
            if (p < end && *p == '}') p++;
            else for (;;) {
                std::pair<std::string, UpdJson> kv;
                ws();
                if (!string(kv.first)) { ok = false; break; }
                ws();
                if (p >= end || *p++ != ':' || !value(kv.second)) { ok = false; break; }
                v.obj.push_back(std::move(kv));
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == '}') { p++; break; }
                ok = false;
                break;
            }
        } else if (*p == '[') {
            v.kind = UpdJson::ARR;
            p++;
            ws();
            if (p < end && *p == ']') p++;
            else for (;;) {
                v.arr.emplace_back();
                if (!value(v.arr.back())) { ok = false; break; }
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == ']') { p++; break; }
                ok = false;
                break;
            }
        } else if (*p == '"') {
            v.kind = UpdJson::STR;
            ok = string(v.str);
        } else {  // true / false / null / a number: kept as their literal
            const char *s = p;
            while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
            v.str.assign(s, p);
            if (v.str == "true" || v.str == "false") v.kind = UpdJson::BOOL;
            else if (v.str == "null") v.kind = UpdJson::NUL;
            else if (!v.str.empty() && (v.str[0] == '-' || (v.str[0] >= '0' && v.str[0] <= '9'))) v.kind = UpdJson::NUM;
            else ok = false;
        }
        depth--;
        return ok;
    }
};

static inline bool upd_json_parse(const std::string &text, UpdJson &out) {
    UpdJsonParser ps{text.data(), text.data() + text.size()};
    if (!ps.value(out)) return false;
    ps.ws();
    return ps.p == ps.end;
}

// ------------------------------------------------------------------ versions
// "v14.0.1" / "14.0.1" / "14.1.0-rc1": up to 4 numeric parts; text after them ("-rc1") is a pre-release, which comes
// before the same numbers without it. Returns false for a version without numbers ("dev": a local build).
struct UpdVersion { long part[4] = {0, 0, 0, 0}; bool pre = false; };
static inline bool upd_version_parse(const char *s, UpdVersion &v) {
    v = UpdVersion{};
    if (!s) return false;
    while (*s == ' ') s++;
    if (*s == 'v' || *s == 'V') s++;
    if (*s < '0' || *s > '9') return false;
    for (int i = 0; i < 4; i++) {
        char *e;
        v.part[i] = strtol(s, &e, 10);
        s = e;
        if (*s != '.' || s[1] < '0' || s[1] > '9') break;
        s++;
    }
    v.pre = *s != 0 && *s != '+' && *s != ' ';  // (+build metadata does not order)
    return true;
}
// < 0: a is older than b, 0: the same, > 0: a is newer. A version that does not parse is the oldest.
static inline int upd_compare(const char *a, const char *b) {
    UpdVersion x, y;
    bool okx = upd_version_parse(a, x), oky = upd_version_parse(b, y);
    if (!okx || !oky) return okx - oky;
    for (int i = 0; i < 4; i++) if (x.part[i] != y.part[i]) return x.part[i] < y.part[i] ? -1 : 1;
    return y.pre - x.pre;
}
// "v14.0.1" -> "14.0.1" (how the release workflow names the packages)
static inline std::string upd_plain(const std::string &tag) {
    return !tag.empty() && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
}

// ------------------------------------------------------------------ the release
struct UpdRelease {
    std::string tag, title, notes, page;  // page: the release on github.com
    std::string asset_name, asset_url;    // the package for this platform ("" when the release has none)
    long long asset_size = 0;
};
static inline bool upd_ends_with(const std::string &s, const char *suffix) {
    size_t n = strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; i++) {
        char a = s[s.size() - n + i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}
// windows: the Monado-Windows-x64-<version>.zip; else the Monado-<version>-x86_64.AppImage (.github/workflows/release.yml)
static inline bool upd_is_package(const std::string &name, bool windows) {
    if (windows) return upd_ends_with(name, ".zip") && name.find("Windows") != std::string::npos;
    return upd_ends_with(name, ".AppImage");
}
// false (and err) when the text is not a release: GitHub's error answers ({"message": "Not Found"}, the rate limit) too
static inline bool upd_parse_release(const std::string &json, bool windows, UpdRelease &r, std::string &err) {
    r = UpdRelease{};
    UpdJson root;
    if (!upd_json_parse(json, root) || root.kind != UpdJson::OBJ) { err = "unreadable answer"; return false; }
    r.tag = root.text("tag_name");
    if (r.tag.empty()) {
        std::string m = root.text("message");
        err = m.empty() ? "no release in the answer" : m;
        return false;
    }
    if (root.flag("draft") || root.flag("prerelease")) { err = "not a published release"; return false; }
    r.title = root.text("name");
    r.notes = root.text("body");
    r.page = root.text("html_url");
    if (const UpdJson *assets = root.get("assets"); assets && assets->kind == UpdJson::ARR)
        for (const UpdJson &a : assets->arr) {
            std::string n = a.text("name"), u = a.text("browser_download_url");
            if (u.empty() || !upd_is_package(n, windows)) continue;
            r.asset_name = n;
            r.asset_url = u;
            if (const UpdJson *sz = a.get("size"); sz && sz->kind == UpdJson::NUM) r.asset_size = atoll(sz->str.c_str());
            break;
        }
    return true;
}

// The launcher's font covers Latin-1 only (U+0000-U+00FF): typographic punctuation becomes its ASCII form, anything
// else past it (emoji, flags, zero-width spaces) is dropped instead of drawn as '?'.
static inline std::string upd_latin1(const std::string &in) {
    std::string out;
    for (size_t i = 0; i < in.size();) {
        unsigned char c = (unsigned char)in[i];
        unsigned cp;
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (n == 1 || i + n > in.size()) cp = c < 0x80 ? c : 0xFFFD;
        else {
            cp = c & (0x7F >> n);
            for (int k = 1; k < n; k++) cp = cp << 6 | ((unsigned char)in[i + k] & 63);
        }
        if (cp < 0x100) out.append(in, i, (n > 1 && i + n <= in.size()) ? n : 1);
        else if (cp == 0x2010 || cp == 0x2011 || cp == 0x2013 || cp == 0x2014 || cp == 0x2212 || cp == 0x2022) out += '-';
        else if (cp == 0x2018 || cp == 0x2019) out += '\'';
        else if (cp == 0x201C || cp == 0x201D) out += '"';
        else if (cp == 0x2026) out += "...";
        else if (cp == 0x2192) out += "->";
        i += (n > 1 && i + n <= in.size()) ? n : 1;
    }
    return out;
}

// Release notes are Markdown; the launcher shows plain wrapped text: no carriage returns, headings and emphasis
// markers dropped, at most max_bytes (cut at a line end).
static inline std::string upd_plain_notes(const std::string &md_in, size_t max_bytes) {
    const std::string md = upd_latin1(md_in);
    std::string out;
    size_t i = 0;
    while (i < md.size()) {
        size_t e = md.find('\n', i);
        if (e == std::string::npos) e = md.size();
        std::string line = md.substr(i, e - i);
        i = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t h = 0;
        while (h < line.size() && line[h] == '#') h++;
        if (h && h < line.size() && line[h] == ' ') {
            line = line.substr(h + 1);
            while (!line.empty() && line[0] == ' ') line.erase(0, 1);  // (an emoji after the # was dropped)
        }
        std::string clean;
        for (size_t k = 0; k < line.size(); k++) {
            if ((line[k] == '*' || line[k] == '_') && k + 1 < line.size() && line[k + 1] == line[k]) { k++; continue; }
            if (line[k] == '`') continue;
            clean += line[k];
        }
        if (clean.size() > 1 && clean[0] == '*' && clean[1] == ' ') clean[0] = '-';  // (the font has no bullet glyph)
        if (out.size() + clean.size() + 1 > max_bytes) { out += "..."; break; }
        out += clean;
        out += '\n';
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}
