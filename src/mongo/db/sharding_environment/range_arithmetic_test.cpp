// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/sharding_environment/range_arithmetic.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <utility>

namespace mongo {
namespace {

TEST(BSONRange, SmallerLowerRangeNonSubset) {
    ASSERT_TRUE(
        rangeOverlaps(BSON("x" << 100), BSON("x" << 200), BSON("x" << 50), BSON("x" << 200)));
    ASSERT_TRUE(
        rangeOverlaps(BSON("x" << 100), BSON("x" << 200), BSON("x" << 60), BSON("x" << 199)));

    ASSERT_FALSE(
        rangeOverlaps(BSON("x" << 100), BSON("x" << 200), BSON("x" << 70), BSON("x" << 99)));
    ASSERT_FALSE(
        rangeOverlaps(BSON("x" << 100), BSON("x" << 200), BSON("x" << 80), BSON("x" << 100)));
}

TEST(BSONRange, BiggerUpperRangeNonSubset) {
    ASSERT_TRUE(
        rangeOverlaps(BSON("x" << 100), BSON("x" << 200), BSON("x" << 150), BSON("x" << 200)));
    ASSERT_TRUE(
        rangeOverlaps(BSON("x" << 100), BSON("x" << 200), BSON("x" << 160), BSON("x" << 201)));
    ASSERT_TRUE(
        rangeOverlaps(BSON("x" << 100), BSON("x" << 200), BSON("x" << 170), BSON("x" << 220)));

    ASSERT_FALSE(
        rangeOverlaps(BSON("x" << 100), BSON("x" << 200), BSON("x" << 200), BSON("x" << 240)));
}

TEST(BSONRange, RangeIsSubsetOfOther) {
    ASSERT_TRUE(
        rangeOverlaps(BSON("x" << 100), BSON("x" << 200), BSON("x" << 70), BSON("x" << 300)));
    ASSERT_TRUE(
        rangeOverlaps(BSON("x" << 100), BSON("x" << 200), BSON("x" << 140), BSON("x" << 180)));
}

TEST(BSONRange, EqualRange) {
    ASSERT_TRUE(
        rangeOverlaps(BSON("x" << 100), BSON("x" << 200), BSON("x" << 100), BSON("x" << 200)));
}

TEST(RangeMap, RangeMapOverlaps) {
    RangeMap rangeMap = SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<BSONObj>();
    rangeMap.insert(std::make_pair(BSON("x" << 100), BSON("x" << 200)));

    ASSERT(rangeMapOverlaps(rangeMap, BSON("x" << 100), BSON("x" << 200)));
    ASSERT(rangeMapOverlaps(rangeMap, BSON("x" << 99), BSON("x" << 200)));
    ASSERT(rangeMapOverlaps(rangeMap, BSON("x" << 100), BSON("x" << 201)));
    ASSERT(rangeMapOverlaps(rangeMap, BSON("x" << 100), BSON("x" << 200)));
    ASSERT(!rangeMapOverlaps(rangeMap, BSON("x" << 99), BSON("x" << 100)));
    ASSERT(!rangeMapOverlaps(rangeMap, BSON("x" << 200), BSON("x" << 201)));
}

}  // namespace
}  // namespace mongo
