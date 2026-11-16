#include <catch2/catch_test_macros.hpp>

#include "core/datetime.hpp"

using namespace bg;

TEST_CASE("Booru date formats are parsed as UTC") {
    REQUIRE(DateTime::parse("1471513944").toIso() == "2016-08-18T09:52:24Z");
    REQUIRE(DateTime::parse("2016-08-18T09:52:24").toIso() == "2016-08-18T09:52:24Z");
    REQUIRE(DateTime::parse("2016-08-18 09:52:24").toIso() == "2016-08-18T09:52:24Z");
    REQUIRE(DateTime::parse("2016-08-18T11:52:24+02:00").toUtc().toIso() == "2016-08-18T09:52:24Z");
    REQUIRE(DateTime::parse("2016/08/18 09:52:24").toIso() == "2016-08-18T09:52:24Z");
    REQUIRE(DateTime::parse("2016/08/18 09:52").toIso() == "2016-08-18T09:52:00Z");
    REQUIRE(DateTime::parse("Thu Aug 18 09:52:24 2016").toIso() == "2016-08-18T09:52:24Z");
    // Offsets with minutes (the original treated "+0530" as 5.3 hours)
    REQUIRE(DateTime::parse("Thu Aug 18 15:22:24 +0530 2016").toUtc().toIso() == "2016-08-18T09:52:24Z");
    REQUIRE_FALSE(DateTime::parse("not a date").valid());
}

TEST_CASE("Qt date format strings") {
    DateTime d = DateTime::parse("2016-08-18T09:05:04Z");
    REQUIRE(d.format("yyyy-MM-dd HH-mm") == "2016-08-18 09-05");
    REQUIRE(d.format("MM-dd-yyyy HH.mm") == "08-18-2016 09.05");
    REQUIRE(d.format("d/M/yy h:m:s") == "18/8/16 9:5:4");
    REQUIRE(d.format("ddd MMM dddd MMMM") == "Thu Aug Thursday August");
    REQUIRE(d.format("'Year' yyyy") == "Year 2016");
    REQUIRE(d.format("hh:mm AP") == "09:05 AM");
}
