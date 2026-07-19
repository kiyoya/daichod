#include "engine/map.h"

#include <string>

#include <gtest/gtest.h>

#include "rpc/error.h"
#include "shim.pb.h"

// These tests deliberately avoid any libgnucash engine initialization
// (Session::GlobalInit()): GuidToString/StringToGuid/RedactUri are pure
// functions over strings/GncGUID and must not require it.

namespace daichod {
namespace {

TEST(MapTest, RedactUriRedactsPassword) {
  EXPECT_EQ(RedactUri("postgres://user:pw@h/db"), "postgres://user:***@h/db");
}

TEST(MapTest, RedactUriLeavesSqliteUriUnchanged) {
  EXPECT_EQ(RedactUri("sqlite3:///path"), "sqlite3:///path");
}

TEST(MapTest, RedactUriLeavesNoSchemeStringUnchanged) {
  EXPECT_EQ(RedactUri("just-a-plain-string"), "just-a-plain-string");
}

TEST(MapTest, StringToGuidRejectsGarbage) {
  bool threw = false;
  try {
    StringToGuid("not-a-valid-guid", "some_field");
  } catch (const ShimError& e) {
    threw = true;
    EXPECT_EQ(e.code(), daicho::shim::v1::INVALID_ARGUMENT_DETAIL);
    EXPECT_EQ(e.context(), "some_field");
  }
  EXPECT_TRUE(threw) << "StringToGuid should throw ShimError on malformed input";
}

TEST(MapTest, GuidRoundTripsThroughStringToGuidAndGuidToString) {
  const std::string text = "3d1c5e0cf3244e33aeb1dc327e16ca0f";
  const GncGUID guid = StringToGuid(text, "guid");
  EXPECT_EQ(GuidToString(&guid), text);
}

}  // namespace
}  // namespace daichod
