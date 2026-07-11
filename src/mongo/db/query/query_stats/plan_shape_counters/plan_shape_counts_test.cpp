// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/plan_shape_counters/plan_shape_counts.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/ctype.h"

#include <cstddef>

namespace mongo::plan_shape_counters {
namespace {

TEST(PlanShapeCountsTest, EmptyByDefault) {
    PlanShapeCounts counts;
    ASSERT_TRUE(counts.empty());
    ASSERT_BSONOBJ_EQ(counts.toBSON(), BSONObj());
}

TEST(PlanShapeCountsTest, IncrementAccumulates) {
    PlanShapeCounts counts;
    counts.increment(PlanShapeCounter::kCollscan);
    counts.increment(PlanShapeCounter::kCollscan);
    counts.increment(PlanShapeCounter::kIxscanFetch, 3);
    ASSERT_FALSE(counts.empty());
    ASSERT_BSONOBJ_EQ(counts.toBSON(), BSON("collscan" << 2LL << "ixscanFetch" << 3LL));
}

DEATH_TEST_REGEX(PlanShapeCountsDeathTest,
                 NegativeIncrementTasserts,
                 "Tripwire assertion.*13022405") {
    PlanShapeCounts counts;
    ASSERT_THROWS_CODE(
        counts.increment(PlanShapeCounter::kCollscan, -1), AssertionException, 13022405);
}

TEST(PlanShapeCountsTest, AddMergesCounts) {
    PlanShapeCounts counts;
    counts.increment(PlanShapeCounter::kCollscan);
    ASSERT_BSONOBJ_EQ(counts.toBSON(), BSON("collscan" << 1LL));

    PlanShapeCounts other;
    other.increment(PlanShapeCounter::kCollscan, 2);
    other.increment(PlanShapeCounter::kIxscanOrFetch);

    counts.add(other);
    counts.add(PlanShapeCounts{});
    ASSERT_BSONOBJ_EQ(counts.toBSON(), BSON("collscan" << 3LL << "ixscanOrFetch" << 1LL));
}

TEST(PlanShapeCountsTest, RoundTripsThroughBSON) {
    PlanShapeCounts counts;
    counts.increment(PlanShapeCounter::kCollscan, 5);
    counts.increment(PlanShapeCounter::kIxscanSortFetch, 7);
    ASSERT_BSONOBJ_EQ(PlanShapeCounts::fromBSON(counts.toBSON()).toBSON(), counts.toBSON());

    ASSERT_TRUE(PlanShapeCounts::fromBSON(BSONObj()).empty());
}

TEST(PlanShapeCountsTest, CounterNamesAreDerivedFromEnumeratorNames) {
    ASSERT_EQ(toCounterName(PlanShapeCounter::kCollscan), "collscan");
    ASSERT_EQ(toCounterName(PlanShapeCounter::kIxscanFetch), "ixscanFetch");
    ASSERT_EQ(toCounterName(PlanShapeCounter::kIxscanSortMergeFetchProject),
              "ixscanSortMergeFetchProject");

    for (size_t i = 0; i < kNumPlanShapeCounters; ++i) {
        auto shape = static_cast<PlanShapeCounter>(i);
        auto name = toCounterName(shape);
        ASSERT_FALSE(name.empty());
        ASSERT_TRUE(ctype::isLower(name[0])) << name;

        // Every name parses back to the counter it came from.
        PlanShapeCounts counts;
        counts.increment(shape);
        ASSERT_TRUE(counts.toBSON().hasField(name)) << name;
        ASSERT_BSONOBJ_EQ(PlanShapeCounts::fromBSON(counts.toBSON()).toBSON(), counts.toBSON());
    }
}

}  // namespace
}  // namespace mongo::plan_shape_counters
