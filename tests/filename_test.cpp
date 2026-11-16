// Filename formats, ported from the original Grabber test suite (filename-test.cpp)
#include <catch2/catch_test_macros.hpp>

#include "core/filename.hpp"
#include "core/settings.hpp"

using namespace bg;

namespace {
struct Fixture {
    std::string dir = tempDir() + "/filename-test-" + uuid();
    Settings settings{dir + "/settings.ini"};
    TokenMap tokens;

    Fixture() {
        settings.set("Save/separator", " ");
        settings.set("Save/character_value", "group");
        settings.set("Save/character_multiple", "replaceAll");
        settings.set("Save/copyright_value", "crossover");
        settings.set("Save/copyright_multiple", "replaceAll");
        settings.set("Save/character_empty", "unknown");
        settings.setBool("Save/replaceblanks", true);
        tokens["md5"] = Token(Var("1bc29b36f623ba82aaf6724fd3b16718"));
        tokens["ext"] = Token(Var("jpg"), Var("jpg"));
        tokens["id"] = Token(Var(7331));
        tokens["rating"] = Token(Var("safe"));
        tokens["search"] = Token(Var("testing well"));
        tokens["date"] = Token(Var(DateTime::parse("2016-08-18T09:52:24Z")));
        tokens["artist"] = Token(Var(StringList{"artist1"}), "keepAll", "anonymous", "multiple artists");
        tokens["copyright"] = Token(Var(StringList{"copyright2", "copyright1"}), "keepAll", "misc", "crossover");
        tokens["character"] = Token(Var(StringList{"character1", "character2"}), "keepAll", "unknown", "group");
        tokens["model"] = Token(Var(StringList{}), "keepAll", "unknown", "multiple");
        tokens["general"] = Token(Var(StringList{"tag1", "tag2", "tag3"}), "keepAll", "", "");
        tokens["allos"] = Token(Var(StringList{"tag1", "tag2", "tag3", "artist1", "copyright1", "copyright2", "character1", "character2"}));
        tokens["all"] = tokens["allos"];
    }
    std::string path(const std::string& format, bool fix = true) {
        StringList p = Filename(format).path(tokens, &settings, "", 0, fix ? Filename::Default : Filename::Complex);
        return p.empty() ? "<none>" : join(p, "|");
    }
};
const std::string md5 = "1bc29b36f623ba82aaf6724fd3b16718";
}  // namespace

TEST_CASE("Simple and multiple-value tokens") {
    Fixture f;
    REQUIRE(f.path("%md5%.%ext%") == md5 + ".jpg");
    REQUIRE(f.path("%artist%/%copyright%/%character%/%md5%.%ext%") == "artist1/crossover/group/" + md5 + ".jpg");
    f.settings.set("Save/character_multiple", "keepAll");
    REQUIRE(f.path("%character%") == "character1 character2");
    f.settings.set("Save/character_multiple", "keepN");
    f.settings.setInt("Save/character_multiple_keepN", 1);
    REQUIRE(f.path("%character%") == "character1");
    f.settings.set("Save/character_multiple", "keepNThenAdd");
    f.settings.setInt("Save/character_multiple_keepNThenAdd_keep", 1);
    f.settings.set("Save/character_multiple_keepNThenAdd_add", " (and %count% of %total%)");
    REQUIRE(f.path("%character%") == "character1 (and 1 of 2)");
    f.settings.set("Save/copyright_multiple", "keepAll");
    f.settings.set("Save/copyright_sort", "name");
    REQUIRE(f.path("%copyright%") == "copyright1 copyright2");
    f.settings.set("Save/character_multiple", "multiple");
    REQUIRE(f.path("%character%.%ext%") == "character1.jpg|character2.jpg");
    REQUIRE(f.path("%model%") == "unknown");
}

TEST_CASE("Conditionals") {
    Fixture f;
    REQUIRE(f.path("<\"tag1\"/>%md5%.%ext%") == "tag1/" + md5 + ".jpg");
    REQUIRE(f.path("<\"tag4\"/>%md5%.%ext%") == md5 + ".jpg");
    REQUIRE(f.path("<\"tag1\" \"tag2\"/>%md5%.%ext%") == "tag1 tag2/" + md5 + ".jpg");
    REQUIRE(f.path("<\"tag1\" !\"tag4\"/>%md5%.%ext%") == "tag1 tag4/" + md5 + ".jpg");
    REQUIRE(f.path("<-\"tag1\"\"tag2\"/>%md5%.%ext%", false) == "tag2/" + md5 + ".jpg");
    REQUIRE(f.path("<\"tag1\" %artist%/>%md5%.%ext%") == "tag1 artist1/" + md5 + ".jpg");
    REQUIRE(f.path("<\"tag1\" %nothing%/>%md5%.%ext%") == md5 + ".jpg");
    REQUIRE(f.path("<\"tag1\"?yes/>%md5%.%ext%") == "yes/" + md5 + ".jpg");
    REQUIRE(f.path("<\"tag1\"?yes:no>/%md5%.%ext%") == "yes/" + md5 + ".jpg");
    REQUIRE(f.path("<!\"tag1\"?yes:no>/%md5%.%ext%") == "no/" + md5 + ".jpg");
    REQUIRE(f.path("<\"tag1\" & \"not_found\"?yes/>%md5%.%ext%") == md5 + ".jpg");
    REQUIRE(f.path("<\"tag1\" | \"not_found\"?yes/>%md5%.%ext%") == "yes/" + md5 + ".jpg");
    REQUIRE(f.path("<\"tag1\"?%id% and %rating%/>%md5%.%ext%") == "7331 and safe/" + md5 + ".jpg");
    REQUIRE(f.path("<<Value>>%md5%<</Value>>", false) == "<Value>" + md5 + "</Value>");
}

TEST_CASE("Options and JavaScript") {
    Fixture f;
    REQUIRE(f.path("%md5:maxlength=8%.%ext%") == "1bc29b36.jpg");
    REQUIRE(f.path("%search:htmlescape%.%ext%") == "testing well.jpg");
    REQUIRE(f.path("%date:format=yyyy-MM-dd%.%ext%") == "2016-08-18.jpg");
    REQUIRE(f.path("%id:length=6%") == "007331");
    REQUIRE(f.path("javascript:md5 + '.' + ext") == md5 + ".jpg");
    f.settings.set("Save/copyright_multiple", "keepAll");
    REQUIRE(f.path("javascript:copyrights.join('-')") == "copyright2-copyright1");
    REQUIRE(f.path("javascript:date.toISOString().slice(0, 10)") == "2016-08-18");
    REQUIRE(f.path("javascript:'") == "<none>");
}

TEST_CASE("Filename validation and %num%") {
    Fixture f;
    std::string msg;
    REQUIRE(Filename("%md5%.%ext%").isValid(&f.settings, &msg));
    REQUIRE_FALSE(Filename("%md5%").isValid(&f.settings, &msg));
    REQUIRE_FALSE(Filename("%artist%.%ext%").isValid(&f.settings, &msg));
    REQUIRE_FALSE(Filename("<%md5%.%ext%").isValid(&f.settings, &msg));
    // %num% counts existing files, also with a different extension (noext)
    writeFile(f.dir + "/out/file 1.png", "x");
    writeFile(f.dir + "/out/file 2.jpg", "x");
    StringList p = Filename("file %num:noext%.%ext%").path(f.tokens, &f.settings, f.dir + "/out", 0, Filename::Path);
    REQUIRE(p.size() == 1);
    REQUIRE(endsWith(p[0], "/out/file 3.jpg"));
}

TEST_CASE("Linux filename limits") {
    std::string longName(300, 'a');
    std::string fixed = fixFilename(longName + ".jpg", "/tmp");
    REQUIRE(fixed.size() <= 255);
    REQUIRE(endsWith(fixed, ".jpg"));
    REQUIRE(fixFilename("dir/file.png", "/base") == "dir/file.png");
}
