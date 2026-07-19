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
    EXPECT_EQ(e.code(), daicho::shim::v1::ERROR_CODE_INVALID_ARGUMENT);
    EXPECT_EQ(e.context(), "some_field");
  }
  EXPECT_TRUE(threw) << "StringToGuid should throw ShimError on malformed input";
}

TEST(MapTest, GuidRoundTripsThroughStringToGuidAndGuidToString) {
  const std::string text = "3d1c5e0cf3244e33aeb1dc327e16ca0f";
  const GncGUID guid = StringToGuid(text, "guid");
  EXPECT_EQ(GuidToString(&guid), text);
}

void ExpectDateFromProtoRejects(int year, int month, int day) {
  daicho::shim::v1::Date date;
  date.set_year(year);
  date.set_month(month);
  date.set_day(day);
  bool threw = false;
  try {
    DateFromProto(date, "some_date");
  } catch (const ShimError& e) {
    threw = true;
    EXPECT_EQ(e.code(), daicho::shim::v1::ERROR_CODE_INVALID_ARGUMENT);
    EXPECT_EQ(e.context(), "some_date");
  }
  EXPECT_TRUE(threw) << "DateFromProto should throw ShimError for "
                     << year << "-" << month << "-" << day;
}

TEST(MapTest, DateFromProtoRejectsYearThatWrapsWhenNarrowed) {
  ExpectDateFromProtoRejects(67562, 1, 1);
}

TEST(MapTest, DateFromProtoRejectsDayThatWrapsWhenNarrowed) {
  ExpectDateFromProtoRejects(2026, 1, 287);
}

TEST(MapTest, DateFromProtoRejectsMonthThatWrapsWhenNarrowed) {
  ExpectDateFromProtoRejects(2026, 268, 1);
}

TEST(MapTest, DateFromProtoRejectsZeroFields) {
  ExpectDateFromProtoRejects(0, 1, 1);
  ExpectDateFromProtoRejects(2026, 0, 1);
  ExpectDateFromProtoRejects(2026, 1, 0);
}

TEST(MapTest, DateFromProtoRejectsNegativeFields) {
  ExpectDateFromProtoRejects(-1, 1, 1);
  ExpectDateFromProtoRejects(2026, -1, 1);
  ExpectDateFromProtoRejects(2026, 1, -1);
}

TEST(MapTest, DateRoundTripsThroughDateFromProtoAndDateToProto) {
  daicho::shim::v1::Date date;
  date.set_year(2026);
  date.set_month(2);
  date.set_day(28);
  const GDate gdate = DateFromProto(date, "date");
  daicho::shim::v1::Date out;
  DateToProto(gdate, &out);
  EXPECT_EQ(out.year(), 2026);
  EXPECT_EQ(out.month(), 2);
  EXPECT_EQ(out.day(), 28);
}

TEST(MapTest, DateFromProtoRejectsInRangeButInvalidCalendarDate) {
  ExpectDateFromProtoRejects(2025, 2, 30);
}

TEST(MapTest, NumericFromProtoRejectsZeroDenom) {
  daicho::shim::v1::Numeric numeric;
  numeric.set_num(1);
  numeric.set_denom(0);
  bool threw = false;
  try {
    NumericFromProto(numeric, "some_numeric");
  } catch (const ShimError& e) {
    threw = true;
    EXPECT_EQ(e.code(), daicho::shim::v1::ERROR_CODE_INVALID_ARGUMENT);
    EXPECT_EQ(e.context(), "some_numeric");
  }
  EXPECT_TRUE(threw) << "NumericFromProto should throw ShimError for denom=0";
}

TEST(MapTest, NumericFromProtoRejectsNegativeDenom) {
  daicho::shim::v1::Numeric numeric;
  numeric.set_num(1);
  numeric.set_denom(-1);
  bool threw = false;
  try {
    NumericFromProto(numeric, "some_numeric");
  } catch (const ShimError& e) {
    threw = true;
    EXPECT_EQ(e.code(), daicho::shim::v1::ERROR_CODE_INVALID_ARGUMENT);
    EXPECT_EQ(e.context(), "some_numeric");
  }
  EXPECT_TRUE(threw) << "NumericFromProto should throw ShimError for denom=-1";
}

TEST(MapTest, NumericRoundTripsThroughNumericFromProtoAndNumericToProto) {
  daicho::shim::v1::Numeric numeric;
  numeric.set_num(7);
  numeric.set_denom(100);
  const gnc_numeric value = NumericFromProto(numeric, "numeric");
  daicho::shim::v1::Numeric out;
  NumericToProto(value, &out);
  EXPECT_EQ(out.num(), 7);
  EXPECT_EQ(out.denom(), 100);
}

TEST(MapTest, AccountTypeRoundTripsThroughFromProtoAndToProto) {
  EXPECT_EQ(AccountTypeToProto(AccountTypeFromProto(
                daicho::shim::v1::ACCOUNT_TYPE_ASSET, "type")),
            daicho::shim::v1::ACCOUNT_TYPE_ASSET);
  EXPECT_EQ(AccountTypeToProto(AccountTypeFromProto(
                daicho::shim::v1::ACCOUNT_TYPE_EXPENSE, "type")),
            daicho::shim::v1::ACCOUNT_TYPE_EXPENSE);
}

TEST(MapTest, AccountTypeFromProtoRejectsRoot) {
  bool threw = false;
  try {
    AccountTypeFromProto(daicho::shim::v1::ACCOUNT_TYPE_ROOT, "type");
  } catch (const ShimError& e) {
    threw = true;
    EXPECT_EQ(e.code(), daicho::shim::v1::ERROR_CODE_INVALID_ARGUMENT);
    EXPECT_EQ(e.context(), "type");
  }
  EXPECT_TRUE(threw)
      << "AccountTypeFromProto should throw ShimError for ACCOUNT_TYPE_ROOT";
}

TEST(MapTest, AccountTypeFromProtoRejectsUnspecified) {
  bool threw = false;
  try {
    AccountTypeFromProto(daicho::shim::v1::ACCOUNT_TYPE_UNSPECIFIED, "type");
  } catch (const ShimError& e) {
    threw = true;
    EXPECT_EQ(e.code(), daicho::shim::v1::ERROR_CODE_INVALID_ARGUMENT);
    EXPECT_EQ(e.context(), "type");
  }
  EXPECT_TRUE(threw) << "AccountTypeFromProto should throw ShimError for "
                        "ACCOUNT_TYPE_UNSPECIFIED";
}

TEST(MapTest, ReconcileStateRoundTripsThroughFromProtoAndToProto) {
  EXPECT_EQ(ReconcileStateToProto(ReconcileStateFromProto(
                daicho::shim::v1::RECONCILE_STATE_CLEARED, "state")),
            daicho::shim::v1::RECONCILE_STATE_CLEARED);
  EXPECT_EQ(ReconcileStateToProto(ReconcileStateFromProto(
                daicho::shim::v1::RECONCILE_STATE_RECONCILED, "state")),
            daicho::shim::v1::RECONCILE_STATE_RECONCILED);
}

TEST(MapTest, ReconcileStateFromProtoRejectsUnspecified) {
  bool threw = false;
  try {
    ReconcileStateFromProto(daicho::shim::v1::RECONCILE_STATE_UNSPECIFIED,
                            "state");
  } catch (const ShimError& e) {
    threw = true;
    EXPECT_EQ(e.code(), daicho::shim::v1::ERROR_CODE_INVALID_ARGUMENT);
    EXPECT_EQ(e.context(), "state");
  }
  EXPECT_TRUE(threw) << "ReconcileStateFromProto should throw ShimError for "
                        "RECONCILE_STATE_UNSPECIFIED";
}

}  // namespace
}  // namespace daichod
