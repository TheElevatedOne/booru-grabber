// QSettings INI compatibility (profiles and site defaults.ini of the original Grabber)
#include <catch2/catch_test_macros.hpp>

#include "core/settings.hpp"

using namespace bg;

TEST_CASE("INI files written by QSettings are read") {
    IniFile f;
    f.parse("[General]\n"
            "limit=20\n"
            "sites=safebooru.org\n"
            "extensionRotationStatic=jpg, png, gif\n"
            "userAgent=\"Mozilla/5.0 (X11; Linux) Firefox/153.0\"\n"
            "batchSplitter=\"14,14\"\n"
            "empty=@Invalid()\n"
            "\n"
            "[Coloring]\n"
            "Colors\\artists=#aa0000\n"
            "\n"
            "[Save]\n"
            "separator=\" \"\n"
            "artist_multiple_keepNThenAdd_add=\" (+ %count%)\"\n"
            "escaped=\"a\\\"b\\\\c\\nd\"\n"
            "at=@@literal\n"
            "[login]\n"
            "post\\url=\"/index.php?page=account&s=login&code=00\"\n");
    REQUIRE(f.values["limit"].str() == "20");
    REQUIRE(f.values["extensionRotationStatic"].isList);
    REQUIRE(f.values["extensionRotationStatic"].items == StringList{"jpg", "png", "gif"});
    REQUIRE(f.values["userAgent"].str() == "Mozilla/5.0 (X11; Linux) Firefox/153.0");
    REQUIRE(f.values["batchSplitter"].str() == "14,14");
    REQUIRE(f.values["empty"].invalid);
    REQUIRE(f.values["Coloring/Colors/artists"].str() == "#aa0000");
    REQUIRE(f.values["Save/separator"].str() == " ");
    REQUIRE(f.values["Save/artist_multiple_keepNThenAdd_add"].str() == " (+ %count%)");
    REQUIRE(f.values["Save/escaped"].str() == "a\"b\\c\nd");
    REQUIRE(f.values["Save/at"].str() == "@literal");
    REQUIRE(f.values["login/post/url"].str() == "/index.php?page=account&s=login&code=00");
}

TEST_CASE("INI files round-trip") {
    IniFile f;
    f.values["a"] = IniValue{{"plain"}, false, false};
    f.values["Save/separator"] = IniValue{{" "}, false, false};
    f.values["Save/Customs/artist"] = IniValue{{"one, two"}, false, false};
    f.values["list"] = IniValue{{"x", "y z"}, true, false};
    f.values["%odd key"] = IniValue{{"v;w"}, false, false};
    IniFile g;
    g.parse(f.serialize());
    REQUIRE(g.values["a"].str() == "plain");
    REQUIRE(g.values["Save/separator"].str() == " ");
    REQUIRE(g.values["Save/Customs/artist"].str() == "one, two");
    REQUIRE_FALSE(g.values["Save/Customs/artist"].isList);
    REQUIRE(g.values["list"].items == StringList{"x", "y z"});
    REQUIRE(g.values["%odd key"].str() == "v;w");
}

TEST_CASE("Settings layer a writable file over read-only fallbacks") {
    std::string dir = tempDir() + "/settings-test-" + uuid();
    writeFile(dir + "/defaults.ini", "[General]\nname=Default\nssl=true\n[Headers]\nUser-Agent=UA\n");
    {
        Settings s(dir + "/settings.ini", {dir + "/defaults.ini"});
        REQUIRE(s.get("name") == "Default");
        REQUIRE(s.getBool("ssl"));
        REQUIRE(s.childKeys("Headers") == StringList{"User-Agent"});
        s.set("name", "Mine");
        s.setWithDefault("ssl", "true", "false");  // equal to the fallback: no override stored
        REQUIRE_FALSE(s.containsOwn("ssl"));
        s.writeArray("Save/MetadataExiftool", {{{"key", "XMP:Subject"}, {"value", "%all%"}}});
        s.sync();
    }
    Settings s2(dir + "/settings.ini", {dir + "/defaults.ini"});
    REQUIRE(s2.get("name") == "Mine");
    auto arr = s2.readArray("Save/MetadataExiftool");
    REQUIRE(arr.size() == 1);
    REQUIRE(arr[0]["key"] == "XMP:Subject");
    s2.remove("Save");
    REQUIRE_FALSE(s2.contains("Save/MetadataExiftool/size"));
}
