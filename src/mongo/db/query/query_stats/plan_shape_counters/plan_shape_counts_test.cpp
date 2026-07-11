// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/plan_shape_counters/plan_shape_counts.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
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
    ASSERT_BSONOBJ_EQ(counts.toBSON(),
                      BSON("patterns" << BSON("collscan" << 2LL << "ixscanFetch" << 3LL)));
}

TEST(PlanShapeCountsTest, EachCategoryAccumulatesSeparately) {
    PlanShapeCounts counts;
    counts.increment(PlanShapeCounter::kCollscan);
    counts.increment(QsnNodeCounter::kCollscanNoFilter);
    ASSERT_BSONOBJ_EQ(
        counts.toBSON(),
        BSON("patterns" << BSON("collscan" << 1LL) << "nodes" << BSON("collscanNoFilter" << 1LL)));
    counts.increment(QsnNodeCounter::kFetchWithFilter, 2);
    counts.increment(AccessPathCounter::kCollscan, 3);
    ASSERT_BSONOBJ_EQ(counts.toBSON(),
                      BSON("patterns" << BSON("collscan" << 1LL) << "nodes"
                                      << BSON("collscanNoFilter" << 1LL << "fetchWithFilter" << 2LL)
                                      << "accessPaths" << BSON("collscan" << 3LL)));

    ASSERT_EQ(counts.getCount(PlanShapeCounter::kCollscan), 1);
    ASSERT_EQ(counts.getCount(QsnNodeCounter::kCollscanNoFilter), 1);
    ASSERT_EQ(counts.getCount(QsnNodeCounter::kFetchWithFilter), 2);
    ASSERT_EQ(counts.getCount(AccessPathCounter::kCollscan), 3);
    ASSERT_EQ(counts.getCount(AccessPathCounter::kCoveredIxscan), 0);
}

TEST(PlanShapeCountsTest, EmptySectionsAreOmitted) {
    PlanShapeCounts counts;
    counts.increment(AccessPathCounter::kIxscanFetch);
    ASSERT_FALSE(counts.empty());
    ASSERT_BSONOBJ_EQ(counts.toBSON(), BSON("accessPaths" << BSON("ixscanFetch" << 1LL)));
}

DEATH_TEST_REGEX(PlanShapeCountsDeathTest,
                 NegativeIncrementTasserts,
                 "Tripwire assertion.*13022405") {
    unittest::ServerParameterGuard errorsAreFatal("internalQueryStatsErrorsAreCommandFatal", true);
    PlanShapeCounts counts;
    ASSERT_THROWS_CODE(
        counts.increment(PlanShapeCounter::kCollscan, -1), AssertionException, 13022405);
}

TEST(PlanShapeCountsTest, NegativeIncrementIgnored) {
    unittest::ServerParameterGuard errorsAreFatal("internalQueryStatsErrorsAreCommandFatal", false);
    if (!kDebugBuild) {
        PlanShapeCounts counts;
        counts.increment(PlanShapeCounter::kCollscan, -1);
        // First increment should be ignored.
        ASSERT_BSONOBJ_EQ(counts.toBSON(), BSONObj());
        counts.increment(PlanShapeCounter::kCollscan, 1);
        ASSERT_BSONOBJ_EQ(counts.toBSON(), BSON("patterns" << BSON("collscan" << 1LL)));
    }
}

TEST(PlanShapeCountsTest, AddMergesCounts) {
    PlanShapeCounts counts;
    counts.increment(PlanShapeCounter::kCollscan);
    counts.increment(QsnNodeCounter::kCollscanWithFilter);
    ASSERT_BSONOBJ_EQ(counts.toBSON(),
                      BSON("patterns" << BSON("collscan" << 1LL) << "nodes"
                                      << BSON("collscanWithFilter" << 1LL)));

    PlanShapeCounts other;
    other.increment(PlanShapeCounter::kCollscan, 2);
    other.increment(PlanShapeCounter::kIxscanOrFetch);
    other.increment(QsnNodeCounter::kCollscanWithFilter);
    other.increment(AccessPathCounter::kCollscan);

    counts.add(other);
    counts.add(PlanShapeCounts{});
    ASSERT_BSONOBJ_EQ(counts.toBSON(),
                      BSON("patterns" << BSON("collscan" << 3LL << "ixscanOrFetch" << 1LL)
                                      << "nodes" << BSON("collscanWithFilter" << 2LL)
                                      << "accessPaths" << BSON("collscan" << 1LL)));
}

TEST(PlanShapeCountsTest, RoundTripsThroughBSON) {
    PlanShapeCounts counts;
    counts.increment(PlanShapeCounter::kCollscan, 5);
    counts.increment(PlanShapeCounter::kIxscanSortFetch, 7);
    counts.increment(QsnNodeCounter::kIxscanNoFilter, 2);
    counts.increment(AccessPathCounter::kCoveredIxscan, 4);
    ASSERT_BSONOBJ_EQ(PlanShapeCounts::fromBSON(counts.toBSON()).toBSON(), counts.toBSON());

    ASSERT_TRUE(PlanShapeCounts::fromBSON(BSONObj()).empty());
}

TEST(PlanShapeCountsTest, FromBSONSkipsUnknownSectionsAndCounters) {
    auto parsed = PlanShapeCounts::fromBSON(
        fromjson("{patterns: {collscan: 2, notACounter: 5}, notASection: {}}"));
    ASSERT_BSONOBJ_EQ(parsed.toBSON(), BSON("patterns" << BSON("collscan" << 2LL)));
}

DEATH_TEST_REGEX(PlanShapeCountsDeathTest,
                 FromBSONTassertsNonNumericCounterValues,
                 "Tripwire assertion.*13022403") {
    unittest::ServerParameterGuard errorsAreFatal("internalQueryStatsErrorsAreCommandFatal", true);
    PlanShapeCounts counts;
    ASSERT_THROWS_CODE(
        PlanShapeCounts::fromBSON(fromjson("{patterns: {collscan: 'foo', ixscanFetch: 3}}")),
        AssertionException,
        13022403);
}

TEST(PlanShapeCountsTest, FromBSONSkipsNonNumericCounterValues) {
    unittest::ServerParameterGuard errorsAreFatal("internalQueryStatsErrorsAreCommandFatal", false);
    if (!kDebugBuild) {
        auto parsed =
            PlanShapeCounts::fromBSON(fromjson("{patterns: {collscan: 'foo', ixscanFetch: 3}}"));
        ASSERT_BSONOBJ_EQ(parsed.toBSON(), BSON("patterns" << BSON("ixscanFetch" << 3LL)));
    }
}

TEST(PlanShapeCountsTest, FromBSONSkipsFlatFormatCounters) {
    ASSERT_TRUE(PlanShapeCounts::fromBSON(fromjson("{collscan: 2, ixscanFetch: 1}")).empty());
}

template <typename CounterEnum>
void assertCounterNamesRoundTrip() {
    for (size_t i = 0; i < static_cast<size_t>(CounterEnum::kNumCounters); ++i) {
        auto counter = static_cast<CounterEnum>(i);
        auto name = toCounterName(counter);
        ASSERT_FALSE(name.empty());
        ASSERT_TRUE(ctype::isLower(name[0])) << name;

        // Every name parses back to the counter it came from.
        PlanShapeCounts counts;
        counts.increment(counter);
        ASSERT_EQ(PlanShapeCounts::fromBSON(counts.toBSON()).getCount(counter), 1) << name;
        ASSERT_BSONOBJ_EQ(PlanShapeCounts::fromBSON(counts.toBSON()).toBSON(), counts.toBSON());
    }
}

TEST(PlanShapeCountsTest, CounterNamesAreDerivedFromEnumeratorNames) {
    ASSERT_EQ(toCounterName(PlanShapeCounter::kCollscan), "collscan");
    ASSERT_EQ(toCounterName(PlanShapeCounter::kIxscanFetch), "ixscanFetch");
    ASSERT_EQ(toCounterName(PlanShapeCounter::kIxscanSortMergeFetchProject),
              "ixscanSortMergeFetchProject");
    ASSERT_EQ(toCounterName(QsnNodeCounter::kOrNoFilterLte100Children), "orNoFilterLte100Children");
    ASSERT_EQ(toCounterName(AccessPathCounter::kBoundsMinKeyToValue), "boundsMinKeyToValue");

    assertCounterNamesRoundTrip<PlanShapeCounter>();
    assertCounterNamesRoundTrip<QsnNodeCounter>();
    assertCounterNamesRoundTrip<AccessPathCounter>();
}

}  // namespace
}  // namespace mongo::plan_shape_counters
