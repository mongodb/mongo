// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_latency_accumulator.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mongo {
namespace {

using namespace std::literals;

// The accumulator records into one serverStatus metric per plan selection strategy. The metric
// serializes as a "histogram" array plus "latency" and "totalCount" fields. Tests read it back out
// of the global metric tree using before/after deltas, since the metrics are process-global.
class QueryLatencyAccumulatorTest : public ServiceContextTest {
protected:
    // Serializes metrics.queryLatencies.<strategy> out of the metric tree.
    BSONObj readMetrics(std::string_view strategy) {
        const MetricTree::ChildMap* children =
            &globalMetricTreeSet()[ClusterRole::ShardServer].children();
        for (std::string_view component : {"metrics"sv, "queryLatencies"sv}) {
            auto it = children->find(component);
            ASSERT(it != children->end()) << "missing metric tree component " << component;
            ASSERT(it->second.isSubtree()) << component << " is not a subtree";
            children = &it->second.getSubtree()->children();
        }

        auto it = children->find(strategy);
        ASSERT(it != children->end()) << "missing metric tree component " << strategy;
        ASSERT(!it->second.isSubtree()) << strategy << " is unexpectedly a subtree";
        BSONObjBuilder bob;
        it->second.getMetric()->appendTo(bob, strategy);
        return bob.obj().firstElement().Obj().getOwned();
    }

    // Number of observations recorded for 'strategy'.
    long long opsFor(std::string_view strategy) {
        return readMetrics(strategy)["totalCount"].Long();
    }

    // Cumulative latency, in microseconds, recorded for 'strategy'.
    long long latencyFor(std::string_view strategy) {
        return readMetrics(strategy)["latency"].Long();
    }

    // The published metrics shape: the field names plus the bucket lowerBound sequence. FTDC starts
    // a new uncompressed reference document whenever this changes, so it must stay constant.
    std::pair<std::vector<std::string>, std::vector<long long>> shapeOfMetrics(
        const BSONObj& metrics) {
        std::vector<std::string> fields;
        for (const auto& field : metrics) {
            fields.emplace_back(field.fieldNameStringData());
        }
        std::vector<long long> lowerBounds;
        for (const auto& bucket : metrics["histogram"].Array()) {
            lowerBounds.push_back(bucket["lowerBound"].Long());
        }
        return {std::move(fields), std::move(lowerBounds)};
    }

    // Count in the histogram bucket whose lowerBound is 'lowerBound'.
    long long bucketCountFor(std::string_view strategy, long long lowerBound) {
        // 'metrics' must outlive the loop: the BSONElements own no storage of their own.
        const BSONObj metrics = readMetrics(strategy);
        for (const auto& bucket : metrics["histogram"].Array()) {
            if (bucket["lowerBound"].Long() == lowerBound) {
                return bucket["count"].Long();
            }
        }
        FAIL(std::string{"no bucket with lowerBound "} + std::to_string(lowerBound));
        MONGO_UNREACHABLE;
    }
};

TEST_F(QueryLatencyAccumulatorTest, AccumulatesAcrossOpsAndRecordsOnceOnDestruction) {
    constexpr auto strategy = "multiPlanner"sv;
    auto opCtx = makeOperationContext();

    const auto beforeOps = opsFor(strategy);
    const auto beforeLatency = latencyFor(strategy);
    // Query with latency 11000us below falls in the [8192, 16384) bucket.
    const auto beforeBucket = bucketCountFor(strategy, 8192);

    {
        auto& acc = QueryLatencyAccumulator::get(opCtx.get());
        acc.recordStrategy(PlanSelectionStrategy::kMultiPlanner);
        acc.addLatency(Microseconds(5000));  // initial find
        acc.addLatency(Microseconds(3000));  // getMore
        acc.addLatency(Microseconds(3000));  // getMore

        // Nothing is recorded until the query (QueryLifespan) is destroyed.
        ASSERT_EQ(opsFor(strategy), beforeOps);
    }

    // Destroying the opCtx releases its QueryLifespan, destroying the accumulator and recording one
    // observation.
    opCtx.reset();

    ASSERT_EQ(opsFor(strategy), beforeOps + 1);
    ASSERT_EQ(latencyFor(strategy), beforeLatency + 11000);
    // The single observation is added to the correct histogram bucket.
    ASSERT_EQ(bucketCountFor(strategy, 8192), beforeBucket + 1);
}

TEST_F(QueryLatencyAccumulatorTest, ExcludedQueryDoesNotRecord) {
    constexpr auto strategy = "costBased"sv;
    auto opCtx = makeOperationContext();
    const auto beforeOps = opsFor(strategy);

    {
        auto& acc = QueryLatencyAccumulator::get(opCtx.get());
        acc.exclude();
        acc.recordStrategy(PlanSelectionStrategy::kCostBasedRanker);
        acc.addLatency(Microseconds(5000));
    }
    opCtx.reset();

    ASSERT_EQ(opsFor(strategy), beforeOps);
}

TEST_F(QueryLatencyAccumulatorTest, NoStrategyDoesNotRecord) {
    constexpr auto single = "singlePlan"sv;
    constexpr auto cached = "cachedPlan"sv;
    auto opCtx = makeOperationContext();
    const auto beforeSingle = opsFor(single);
    const auto beforeCached = opsFor(cached);

    {
        // A non-query cursor never records a strategy, so addLatency is a no-op.
        auto& acc = QueryLatencyAccumulator::get(opCtx.get());
        acc.addLatency(Microseconds(5000));
    }
    opCtx.reset();

    ASSERT_EQ(opsFor(single), beforeSingle);
    ASSERT_EQ(opsFor(cached), beforeCached);
}

// Each strategy must be recorded in its own set of metrics.
TEST_F(QueryLatencyAccumulatorTest, RoutesEachStrategyToItsOwnHistogram) {
    struct {
        PlanSelectionStrategy strategy;
        std::string_view name;
        long long micros;
    } const cases[] = {
        {PlanSelectionStrategy::kMultiPlanner, "multiPlanner"sv, 500},
        {PlanSelectionStrategy::kCostBasedRanker, "costBased"sv, 1500},
        {PlanSelectionStrategy::kSinglePlan, "singlePlan"sv, 250},
        {PlanSelectionStrategy::kCachedPlan, "cachedPlan"sv, 750},
        {PlanSelectionStrategy::kJoinOptimization, "joinOptimization"sv, 1250},
        {PlanSelectionStrategy::kJoinCachedPlan, "joinCachedPlan"sv, 1750},
    };

    for (const auto& [strategy, name, micros] : cases) {
        std::vector<long long> beforeOps;
        for (const auto& testcase : cases) {
            beforeOps.push_back(opsFor(testcase.name));
        }
        const auto beforeLatency = latencyFor(name);

        {
            auto opCtx = makeOperationContext();
            auto& acc = QueryLatencyAccumulator::get(opCtx.get());
            acc.recordStrategy(strategy);
            acc.addLatency(Microseconds(micros));
        }

        for (size_t i = 0; i < std::size(cases); ++i) {
            const bool expectRecorded = cases[i].name == name;
            ASSERT_EQ(opsFor(cases[i].name), beforeOps[i] + (expectRecorded ? 1 : 0))
                << "unexpected count change for " << cases[i].name;
        }
        ASSERT_EQ(latencyFor(name), beforeLatency + micros);
    }
}

// Only the first call to recordStrategy has effect, so a getMore cannot re-attribute a query
// mid-flight.
TEST_F(QueryLatencyAccumulatorTest, FirstRecordedStrategyWins) {
    constexpr auto single = "singlePlan"sv;
    constexpr auto cached = "cachedPlan"sv;
    const auto beforeSingle = opsFor(single);
    const auto beforeCached = opsFor(cached);

    {
        auto opCtx = makeOperationContext();
        auto& acc = QueryLatencyAccumulator::get(opCtx.get());
        acc.recordStrategy(PlanSelectionStrategy::kSinglePlan);
        acc.recordStrategy(PlanSelectionStrategy::kCachedPlan);
        acc.addLatency(Microseconds(5000));
    }

    ASSERT_EQ(opsFor(single), beforeSingle + 1);
    ASSERT_EQ(opsFor(cached), beforeCached);
}

// A query that recorded a strategy but no elapsed time is not observed at all.
TEST_F(QueryLatencyAccumulatorTest, ZeroTotalDoesNotRecord) {
    constexpr auto strategy = "singlePlan"sv;
    const auto beforeOps = opsFor(strategy);

    {
        auto opCtx = makeOperationContext();
        QueryLatencyAccumulator::get(opCtx.get())
            .recordStrategy(PlanSelectionStrategy::kSinglePlan);
    }

    ASSERT_EQ(opsFor(strategy), beforeOps);
}

// The histogram, latency and totalCount metrics must stay mutually consistent and the published
// metrics shape stays fixed as observations accumulate.
TEST_F(QueryLatencyAccumulatorTest, PublishesFixedShapeConsistentMetrics) {
    constexpr auto strategy = "singlePlan"sv;

    // Two observations landing in different buckets, so each bucket is checked independently.
    constexpr long long kFirstMicros = 1000;   // -> [512, 1024)
    constexpr long long kSecondMicros = 5000;  // -> [4096, 8192)
    constexpr long long kFirstBucket = 512;
    constexpr long long kSecondBucket = 4096;

    const BSONObj before = readMetrics(strategy);
    const auto beforeShape = shapeOfMetrics(before);
    const auto beforeOps = before["totalCount"].Long();
    const auto beforeLatency = before["latency"].Long();
    const auto beforeFirstBucket = bucketCountFor(strategy, kFirstBucket);
    const auto beforeSecondBucket = bucketCountFor(strategy, kSecondBucket);

    for (const auto micros : {kFirstMicros, kSecondMicros}) {
        auto opCtx = makeOperationContext();
        auto& acc = QueryLatencyAccumulator::get(opCtx.get());
        acc.recordStrategy(PlanSelectionStrategy::kSinglePlan);
        acc.addLatency(Microseconds(micros));
    }

    const BSONObj after = readMetrics(strategy);

    // Recording must not alter the field set or the bucket boundaries.
    ASSERT_EQ(after.nFields(), 3) << after;
    ASSERT(shapeOfMetrics(after) == beforeShape)
        << "published shape changed while recording: " << before << " -> " << after;

    const auto buckets = after["histogram"].Array();
    // pow(31, 64, 2) yields 31 boundaries, so 32 buckets.
    ASSERT_EQ(buckets.size(), 32u) << after;

    long long sum = 0;
    long long previousLowerBound = -1;
    for (const auto& bucket : buckets) {
        const auto lowerBound = bucket["lowerBound"].Long();
        ASSERT_GT(lowerBound, previousLowerBound) << "lowerBounds must strictly increase";
        previousLowerBound = lowerBound;
        sum += bucket["count"].Long();
    }

    // Both observations counted, each in its own bucket, with latency summing the two.
    ASSERT_EQ(after["totalCount"].Long(), beforeOps + 2);
    ASSERT_EQ(after["latency"].Long(), beforeLatency + kFirstMicros + kSecondMicros);
    ASSERT_EQ(bucketCountFor(strategy, kFirstBucket), beforeFirstBucket + 1);
    ASSERT_EQ(bucketCountFor(strategy, kSecondBucket), beforeSecondBucket + 1);

    // The counters must not drift from the histogram.
    ASSERT_EQ(after["totalCount"].Long(), sum);
}

}  // namespace
}  // namespace mongo
