// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/record_id_range.h"

#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/query/record_id_range_test_utils.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

void testRange(auto getBoundOptional,
               auto maybeNarrowBound,
               auto checkInclusivity,
               int initialValue,
               int narrowerValue,
               int widerValue) {
    ASSERT_FALSE(getBoundOptional());

    auto assertValueEq = [&](auto value) {
        auto bson = BSON("value" << value);
        auto recordId = record_id_helpers::keyForObj(bson);
        ASSERT_EQ(recordId, getBoundOptional()->recordId());
    };

    // narrow from unset
    maybeNarrowBound(BSON("value" << initialValue), true /* inclusive */);
    ASSERT_TRUE(getBoundOptional());
    assertValueEq(initialValue);
    ASSERT_TRUE(checkInclusivity());

    // narrow by removing inclusivity of the bound
    maybeNarrowBound(BSON("value" << initialValue), false /* not inclusive */);
    ASSERT_TRUE(getBoundOptional());
    assertValueEq(initialValue);
    ASSERT_FALSE(checkInclusivity());

    // cannot widen by re-adding inclusivity
    maybeNarrowBound(BSON("value" << initialValue), true /* inclusive */);
    ASSERT_TRUE(getBoundOptional());
    assertValueEq(initialValue);
    ASSERT_FALSE(checkInclusivity());

    // cannot widen by setting a wider bound
    maybeNarrowBound(BSON("value" << widerValue), true /* inclusive */);
    ASSERT_TRUE(getBoundOptional());
    assertValueEq(initialValue);
    ASSERT_FALSE(checkInclusivity());

    // cannot widen by setting a wider bound, regardless of inclusivity
    maybeNarrowBound(BSON("value" << widerValue), false /* not inclusive */);
    ASSERT_TRUE(getBoundOptional());
    assertValueEq(initialValue);
    ASSERT_FALSE(checkInclusivity());

    // narrow to a non-inclusive bound at a narrower value
    maybeNarrowBound(BSON("value" << narrowerValue), false /* not inclusive */);
    ASSERT_TRUE(getBoundOptional());
    assertValueEq(narrowerValue);
    ASSERT_FALSE(checkInclusivity());
}

TEST(RecordIdRangeTest, NarrowMin) {
    RecordIdRange range;

    testRange([&] { return range.getMin(); },
              [&](const BSONObj& newVal, bool inclusive) {
                  return range.maybeNarrowMin(newVal, inclusive);
              },
              [&] { return range.isMinInclusive(); },
              10,
              11,
              9);
}

TEST(RecordIdRangeTest, NarrowMax) {
    RecordIdRange range;

    testRange([&] { return range.getMax(); },
              [&](const BSONObj& newVal, bool inclusive) {
                  return range.maybeNarrowMax(newVal, inclusive);
              },
              [&] { return range.isMaxInclusive(); },
              10,
              9,
              11);
}


// ---------------------------------------------------------------------------
// RecordIdRange::isEmpty and RecordIdRange::compare
// ---------------------------------------------------------------------------

TEST(RecordIdRangeTest, IsEmpty_BothBoundAbsent) {
    RecordIdRange r;
    ASSERT_FALSE(r.isEmpty());
}

TEST(RecordIdRangeTest, IsEmpty_FiniteNonEmptyRange) {
    auto r = makeRange(1, true, 5, true);
    ASSERT_FALSE(r.isEmpty());
}

TEST(RecordIdRangeTest, IsEmpty_PointRange) {
    auto r = makeRange(3, true, 3, true);  // [3, 3] contains exactly 3
    ASSERT_FALSE(r.isEmpty());
}

TEST(RecordIdRangeTest, IsEmpty_ExclusivePointRange) {
    auto r = makeRange(3, false, 3, true);  // (3, 3] — nothing satisfies 3 < x ≤ 3
    ASSERT_TRUE(r.isEmpty());

    auto r2 = makeRange(3, true, 3, false);  // [3, 3) — nothing satisfies 3 ≤ x < 3
    ASSERT_TRUE(r2.isEmpty());

    auto r3 = makeRange(3, false, 3, false);  // (3, 3) — nothing satisfies 3 < x < 3
    ASSERT_TRUE(r3.isEmpty());
}

TEST(RecordIdRangeTest, IsEmpty_MinGreaterThanMax) {
    auto r = makeRange(7, true, 3, true);  // [7, 3] — min > max → empty
    ASSERT_TRUE(r.isEmpty());
}

TEST(RecordIdRangeTest, IsEmpty_OnlyMinBound) {
    auto r = makeRangeMinOnly(5, true);
    ASSERT_FALSE(r.isEmpty());  // [5, +∞) is not empty
}

TEST(RecordIdRangeTest, IsEmpty_OnlyMaxBound) {
    auto r = makeRangeMaxOnly(5, true);
    ASSERT_FALSE(r.isEmpty());  // (-∞, 5] is not empty
}

TEST(RecordIdRangeTest, Compare_WithinInclusiveBounds) {
    auto r = makeRange(3, true, 7, true);   // [3, 7]
    ASSERT_EQ(r.compare(RecordId(2)), -1);  // before range
    ASSERT_EQ(r.compare(RecordId(3)), 0);   // at inclusive min
    ASSERT_EQ(r.compare(RecordId(5)), 0);   // within
    ASSERT_EQ(r.compare(RecordId(7)), 0);   // at inclusive max
    ASSERT_EQ(r.compare(RecordId(8)), 1);   // past range
}

TEST(RecordIdRangeTest, Compare_ExclusiveBounds) {
    auto r = makeRange(3, false, 7, false);  // (3, 7)
    ASSERT_EQ(r.compare(RecordId(3)), -1);   // at excluded min
    ASSERT_EQ(r.compare(RecordId(4)), 0);    // within
    ASSERT_EQ(r.compare(RecordId(7)), 1);    // at excluded max
}

TEST(RecordIdRangeTest, Compare_UnboundedRange) {
    RecordIdRange r;  // (-∞, +∞)
    ASSERT_EQ(r.compare(RecordId(0)), 0);
    ASSERT_EQ(r.compare(RecordId(1000000)), 0);
}

TEST(RecordIdRangeTest, Compare_MinOnlyRange) {
    auto r = makeRangeMinOnly(5, true);  // [5, +∞)
    ASSERT_EQ(r.compare(RecordId(4)), -1);
    ASSERT_EQ(r.compare(RecordId(5)), 0);
    ASSERT_EQ(r.compare(RecordId(1000000)), 0);
}

TEST(RecordIdRangeTest, Compare_MaxOnlyRange) {
    auto r = makeRangeMaxOnly(5, false);  // (-∞, 5)
    ASSERT_EQ(r.compare(RecordId(4)), 0);
    ASSERT_EQ(r.compare(RecordId(5)), 1);  // at excluded max
    ASSERT_EQ(r.compare(RecordId(6)), 1);
}

}  // namespace
