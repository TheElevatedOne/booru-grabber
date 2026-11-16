// Every site source must parse the sample pages stored next to it (sites/<Source>/resources)
#include <catch2/catch_test_macros.hpp>

#include "core/logger.hpp"
#include "core/source.hpp"

using namespace bg;

TEST_CASE("Site sources parse their sample search pages") {
    Logger::instance().setConsoleLevel(LogLevel::Error);
    js::setHelperFile(std::string(BG_TEST_SITES_BUILD) + "/helper.js");
    // Known broken upstream fixtures (the original's jest tests skip them too)
    const StringList skipped{"Danbooru (2.0)/Html"};
    int checked = 0;
    for(auto& dir : listDir(BG_TEST_SITES_BUILD, true, false)) {
        Source src(std::string(BG_TEST_SITES_BUILD) + "/" + dir, "/nonexistent");
        INFO("source " << dir);
        REQUIRE(src.valid());
        SiteInfo si;
        si.baseUrl = "https://example.com";
        si.fixUrl = si.fixMediaUrl = [](const std::string& s) { return s; };
        for(auto& api : src.apis()) {
            std::string ext = toLower(api->name());
            auto data = readFile(std::string(BG_TEST_SITES_SOURCE) + "/" + dir + "/resources/search." + ext);
            if(!data || contains(skipped, dir + "/" + api->name())) continue;
            INFO("API " << api->name());
            ParsedPage p = api->parsePage(*data, 200, 0, si);
            CHECK(p.error.empty());
            CHECK(!p.images.empty());
            checked++;
        }
    }
    REQUIRE(checked >= 5);
}
