// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_latency_accumulator.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// The accumulator records into one OTel histogram per strategy. Tests read them back via
// OtelMetricsCapturer using before/after deltas, since the histograms are process-global.
class QueryLatencyAccumulatorTest : public ServiceContextTest {
protected:
    void setUp() override {
        ServiceContextTest::setUp();
        if (!_capturer.canReadMetrics())
            GTEST_SKIP() << "OTel metrics reader unavailable";
    }

    // Cumulative op count (histogram count) recorded for 'name', or 0 if nothing was recorded yet.
    long long opsFor(otel::metrics::MetricName name) {
        try {
            return static_cast<long long>(_capturer.readInt64Histogram(name).count);
        } catch (const DBException&) {
            return 0;
        }
    }

    // Cumulative latency (micros, histogram sum) recorded for 'name', or 0 if nothing yet.
    long long latencyFor(otel::metrics::MetricName name) {
        try {
            return _capturer.readInt64Histogram(name).sum;
        } catch (const DBException&) {
            return 0;
        }
    }

    otel::metrics::OtelMetricsCapturer _capturer;
};

TEST_F(QueryLatencyAccumulatorTest, AccumulatesAcrossOpsAndRecordsOnceOnDestruction) {
    const auto name = otel::metrics::MetricNames::kQueryLatencyMultiPlanner;
    auto opCtx = makeOperationContext();

    const auto beforeOps = opsFor(name);
    const auto beforeLatency = latencyFor(name);

    {
        auto& acc = QueryLatencyAccumulator::get(opCtx.get());
        acc.recordStrategy(PlanSelectionStrategy::kMultiPlanner);
        acc.addLatency(Microseconds(5000));  // initial find
        acc.addLatency(Microseconds(3000));  // getMore
        acc.addLatency(Microseconds(3000));  // getMore

        // Nothing is recorded until the query (QueryLifespan) is destroyed.
        ASSERT_EQ(opsFor(name), beforeOps);
    }

    // Destroying the opCtx releases its QueryLifespan, destroying the accumulator and recording one
    // observation.
    opCtx.reset();

    ASSERT_EQ(opsFor(name), beforeOps + 1);
    ASSERT_EQ(latencyFor(name), beforeLatency + 11000);
}

TEST_F(QueryLatencyAccumulatorTest, ExcludedQueryDoesNotRecord) {
    const auto name = otel::metrics::MetricNames::kQueryLatencyCostBased;
    auto opCtx = makeOperationContext();
    const auto beforeOps = opsFor(name);

    {
        auto& acc = QueryLatencyAccumulator::get(opCtx.get());
        acc.exclude();
        acc.recordStrategy(PlanSelectionStrategy::kCostBasedRanker);
        acc.addLatency(Microseconds(5000));
    }
    opCtx.reset();

    ASSERT_EQ(opsFor(name), beforeOps);
}

TEST_F(QueryLatencyAccumulatorTest, NoStrategyDoesNotRecord) {
    const auto single = otel::metrics::MetricNames::kQueryLatencySinglePlan;
    const auto cached = otel::metrics::MetricNames::kQueryLatencyCachedPlan;
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

// Each strategy must be recorded in its own histogram.
TEST_F(QueryLatencyAccumulatorTest, RoutesEachStrategyToItsOwnHistogram) {
    struct {
        PlanSelectionStrategy strategy;
        otel::metrics::MetricName name;
        long long micros;
    } const cases[] = {
        {PlanSelectionStrategy::kMultiPlanner,
         otel::metrics::MetricNames::kQueryLatencyMultiPlanner,
         500},
        {PlanSelectionStrategy::kCostBasedRanker,
         otel::metrics::MetricNames::kQueryLatencyCostBased,
         1500},
        {PlanSelectionStrategy::kSinglePlan,
         otel::metrics::MetricNames::kQueryLatencySinglePlan,
         250},
        {PlanSelectionStrategy::kCachedPlan,
         otel::metrics::MetricNames::kQueryLatencyCachedPlan,
         750},
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
                << "unexpected count change for " << cases[i].name.getName();
        }
        ASSERT_EQ(latencyFor(name), beforeLatency + micros);
    }
}

// Only the first call to recordStrategy has effect, so a getMore cannot re-attribute a query
// mid-flight.
TEST_F(QueryLatencyAccumulatorTest, FirstRecordedStrategyWins) {
    const auto single = otel::metrics::MetricNames::kQueryLatencySinglePlan;
    const auto cached = otel::metrics::MetricNames::kQueryLatencyCachedPlan;
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
    const auto name = otel::metrics::MetricNames::kQueryLatencySinglePlan;
    const auto beforeOps = opsFor(name);

    {
        auto opCtx = makeOperationContext();
        QueryLatencyAccumulator::get(opCtx.get())
            .recordStrategy(PlanSelectionStrategy::kSinglePlan);
    }

    ASSERT_EQ(opsFor(name), beforeOps);
}

}  // namespace
}  // namespace mongo
