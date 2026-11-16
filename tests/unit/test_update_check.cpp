// GitHub #119: the launcher's update check (tools/launcher/update_check.h): versions, the release JSON, the notes.
// Build: c++ -std=c++17 tests/unit/test_update_check.cpp -o t && ./t   (tests/unit/test_update_check.py runs it)
#include "../../tools/launcher/update_check.h"
#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main() {
    // versions: the tags the repository has used ("v14.0.0" and "14.0.1"), pre-releases, local builds
    CHECK(upd_compare("14.0.1", "v14.0.0") > 0);
    CHECK(upd_compare("v14.0.0", "14.0.1") < 0);
    CHECK(upd_compare("v14.0.1", "14.0.1") == 0);
    CHECK(upd_compare("14.0.10", "14.0.9") > 0);
    CHECK(upd_compare("15", "14.9.9") > 0);
    CHECK(upd_compare("14.1.0-rc1", "14.1.0") < 0);
    CHECK(upd_compare("14.1.0-rc1", "14.0.9") > 0);
    CHECK(upd_compare("14.0.1+build5", "14.0.1") == 0);
    CHECK(upd_compare("14.0.1", "dev") > 0);
    CHECK(upd_compare("dev", "dev") == 0);
    UpdVersion v;
    CHECK(!upd_version_parse("dev", v));
    CHECK(upd_version_parse("0.0.0-dev.12-abc", v) && v.pre);  // (CI test packages: a pre-release)
    CHECK(upd_plain("v14.0.1") == "14.0.1" && upd_plain("14.0.1") == "14.0.1");

    // a release as the API returns it (trimmed): the author's html_url must not be taken for the release's
    const char *json = R"({"url":"https://api.github.com/x","author":{"login":"a","html_url":"https://github.com/a"},
        "html_url":"https://github.com/o/r/releases/tag/14.0.2","tag_name":"14.0.2","name":"14.0.2 - fixes é 😀",
        "draft":false,"prerelease":false,"assets":[
          {"name":"Monado-14.0.2-x86_64.AppImage.sha256","size":90,"browser_download_url":"https://d/a.sha256"},
          {"name":"Monado-14.0.2-x86_64.AppImage","size":209715200,"browser_download_url":"https://d/a.AppImage"},
          {"name":"Monado-Windows-x64-14.0.2.zip.sha256","size":90,"browser_download_url":"https://d/w.sha256"},
          {"name":"Monado-Windows-x64-14.0.2.zip","size":104857600,"browser_download_url":"https://d/w.zip"}],
        "body":"## What's new\r\n* **Fixed** a crash\r\n- `x` faster\r\n"})";
    UpdRelease r;
    std::string err;
    CHECK(upd_parse_release(json, false, r, err));
    CHECK(r.tag == "14.0.2");
    CHECK(r.page == "https://github.com/o/r/releases/tag/14.0.2");
    CHECK(r.title == "14.0.2 - fixes \xC3\xA9 \xF0\x9F\x98\x80");
    CHECK(r.asset_url == "https://d/a.AppImage" && r.asset_size == 209715200);
    CHECK(upd_parse_release(json, true, r, err));
    CHECK(r.asset_url == "https://d/w.zip" && r.asset_name == "Monado-Windows-x64-14.0.2.zip");
    CHECK(upd_plain_notes(r.notes, 1000) == "What's new\n- Fixed a crash\n- x faster");
    CHECK(upd_plain_notes(r.notes, 15) == "What's new\n...");
    // (the 14.0.1 notes: a zero-width space before a heading, flag and rocket emoji, typographic punctuation)
    CHECK(upd_plain_notes("\xE2\x80\x8B# Release 14.0.1\r\n## \xF0\x9F\x87\xAC\xF0\x9F\x87\xA7 English\r\n"
                          "### \xF0\x9F\x9A\x80 What\xE2\x80\x99s new \xE2\x80\x94 caf\xC3\xA9\xE2\x80\xA6", 1000) ==
          "Release 14.0.1\nEnglish\nWhat's new - caf\xC3\xA9...");
    CHECK(upd_latin1("ok \xF0\x9F\x98\x80\xC3") == "ok ");  // (a cut sequence is dropped too)

    // GitHub's error answers and broken text are failures with a reason, not releases
    CHECK(!upd_parse_release(R"({"message":"Not Found","documentation_url":"x"})", true, r, err) && err == "Not Found");
    CHECK(!upd_parse_release(R"({"tag_name":"1.0","draft":true})", true, r, err));
    CHECK(!upd_parse_release("<html>", true, r, err));
    CHECK(!upd_parse_release(R"({"tag_name":"1.0")", true, r, err));
    CHECK(!upd_parse_release(std::string(200, '['), true, r, err));  // (nesting limit)
    CHECK(upd_parse_release(R"({"tag_name":"1.0","assets":[]})", true, r, err) && r.asset_url.empty());

    printf(fails ? "%d failure(s)\n" : "update_check: all passed\n", fails);
    return fails != 0;
}
