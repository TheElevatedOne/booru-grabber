#include <catch2/catch_test_macros.hpp>

#include "core/filters.hpp"

using namespace bg;

static TokenMap image() {
    TokenMap t;
    t["allos"] = Token(Var(StringList{"tag1", "tag2", "cat_ears"}));
    t["rating"] = Token(Var("safe"));
    t["score"] = Token(Var("12"));
    t["width"] = Token(Var(1920));
    t["date"] = Token(Var(DateTime::parse("2016-08-18T09:52:24Z")));
    t["source"] = Token(Var("https://pixiv.net/1"));
    return t;
}

TEST_CASE("Blacklist") {
    Blacklist b(StringList{"tag3", "tag1 tag2", "cat_*"});
    StringList detected = b.match(image());
    REQUIRE(detected.size() == 2);
    REQUIRE(b.contains("tag3"));
    REQUIRE(b.remove("tag1 tag2"));
    REQUIRE(b.match(image()).size() == 1);
    REQUIRE(Blacklist(StringList{"rating:explicit"}).match(image()).empty());
    REQUIRE(Blacklist(StringList{"rating:s"}).match(image()).size() == 1);
}

TEST_CASE("Post-filters") {
    TokenMap t = image();
    REQUIRE(PostFilter(StringList{"tag1"}).match(t).empty());
    REQUIRE(PostFilter(StringList{"-tag1"}).match(t).size() == 1);
    REQUIRE(PostFilter(StringList{"width:>=1920", "score:>10", "source:https://pixiv"}).match(t).empty());
    REQUIRE(PostFilter(StringList{"width:<1000"}).match(t).size() == 1);
    REQUIRE(PostFilter(StringList{"date:2016-08-01..2016-09-01"}).match(t).empty());
    REQUIRE(PostFilter("tag1 & (tag4 | tag2)").match(t).empty());
    REQUIRE(PostFilter("tag4 | tag5").match(t).size() == 1);
    REQUIRE(PostFilter(StringList{"%rating%"}).match(t).empty());
    REQUIRE(PostFilter(StringList{"%nothing%"}).match(t).size() == 1);
}
