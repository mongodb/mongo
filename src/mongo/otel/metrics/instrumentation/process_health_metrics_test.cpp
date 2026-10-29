// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/instrumentation/process_health_metrics.h"

#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

#ifdef __linux__
#include "mongo/util/fail_point.h"
namespace mongo {
extern void runProcessHealthCollectionCycle(ProcessHealthMetrics&);
}
#endif

namespace mongo {
namespace {

using namespace std::literals::string_view_literals;

using otel::metrics::MetricNames;
using otel::metrics::OtelMetricsCapturer;

class ProcessHealthOtelMetricsTest : public unittest::Test {
protected:
    void setUp() override {
        if (!OtelMetricsCapturer::canReadMetrics()) {
            GTEST_SKIP() << "Skipping test: OTel metrics unavailable on this platform";
        }
    }

    OtelMetricsCapturer _capturer;
    ProcessHealthMetrics _metrics;
};

struct CPUModeField {
    std::string_view mode;
    int64_t ProcessHealthSnapshot::* field;
};

inline constexpr auto kCpuModeFields = std::to_array<CPUModeField>({
    {"user"sv, &ProcessHealthSnapshot::userMs},
    {"system"sv, &ProcessHealthSnapshot::systemMs},
});

TEST_F(ProcessHealthOtelMetricsTest, CpuTime) {
    struct Case {
        std::string_view name;
        std::vector<int64_t> deltaSeries;
        int64_t expected;
    };

    const auto cases = std::to_array<Case>({
        {"first_update_sets_metrics_correctly"sv, {1000}, 1000},
        {"second_update_sets_metrics_correctly"sv, {1000, 1500}, 1500},
        {"negative_delta_on_wrap_is_skipped"sv, {5000, 1000}, 5000},
    });

    for (const auto& tc : cases) {
        SCOPED_TRACE(fmt::format("name={}", tc.name));
        for (const auto& delta : tc.deltaSeries) {
            ProcessHealthSnapshot snap;
            for (const auto& modeField : kCpuModeFields)
                snap.*modeField.field = delta;
            _metrics.update(snap);
        }

        for (const auto& modeField : kCpuModeFields)
            EXPECT_EQ(tc.expected,
                      _capturer.readInt64Counter<std::string_view>(MetricNames::kProcessCpuTime,
                                                                   {modeField.mode}));
    }
}

TEST_F(ProcessHealthOtelMetricsTest, CpuUtilization) {
    struct Case {
        std::string_view name;
        std::array<int64_t, 2> deltas;
        std::array<double, 2> expectedRatios;
    };
    constexpr auto kCases = std::to_array<Case>({
        {"uniform_deltas"sv, {50, 50}, {0.5, 0.5}},
        {"varied_deltas"sv, {600, 400}, {0.6, 0.4}},
        {"single_mode"sv, {0, 1000}, {0.0, 1.0}},
        // In the case where nothing changes in the delta, the utilization will not be
        // recomputed and the previous value will be reused. Since the test starts with
        // every field at 100, utilization is equal.
        {"zero_total_delta"sv, {0, 0}, {0.5, 0.5}},
    });

    // This test creates two points in time for the CPU (`first` and `second`), setting the
    // first point to a default value (100) and the second point to `first + delta`.
    // Then, checks that the CPU utilization computes the correct ratio based on those snapshots.
    for (const auto& c : kCases) {
        SCOPED_TRACE(fmt::format("name={}", c.name));
        // We're not using the `_metrics` member here on purpose; we don't want
        // to accidentally carry forward state across cases.
        ProcessHealthMetrics metrics;
        ProcessHealthSnapshot first{}, second{};
        for (size_t i = 0; i < kCpuModeFields.size(); ++i) {
            first.*kCpuModeFields[i].field = 100;
            second.*kCpuModeFields[i].field = first.*kCpuModeFields[i].field + c.deltas[i];
        }
        metrics.update(first);
        metrics.update(second);

        for (size_t i = 0; i < c.expectedRatios.size(); ++i) {
            EXPECT_DOUBLE_EQ(c.expectedRatios[i],
                             _capturer.readDoubleGauge<std::string_view>(
                                 MetricNames::kProcessCpuUtilization, {kCpuModeFields[i].mode}));
        }
    }
}

TEST_F(ProcessHealthOtelMetricsTest, ContextSwitch) {
    struct Case {
        std::string_view name;
        int64_t voluntaryReading;
        int64_t involuntaryReading;
        int64_t expectedVoluntary;
        int64_t expectedInvoluntary;
    };

    // This is basically the same as the CPU Time test, but simpler.
    // We just update monotonic totals of the voluntary and involuntary context switches.
    // The reader is interested in the largest number of context switches over a period,
    // so negative deltas are not recorded.
    constexpr auto kCases = std::to_array<Case>({
        {"first_update_sets_both_buckets"sv, 100, 30, 100, 30},
        {"second_update_adds_deltas"sv, 250, 80, 250, 80},
        {"decrease_on_wrap_is_skipped"sv, 400, 50, 400, 80},
    });

    for (const auto& tc : kCases) {
        SCOPED_TRACE(fmt::format("name={}", tc.name));
        ProcessHealthSnapshot snap;
        snap.voluntaryContextSwitches = tc.voluntaryReading;
        snap.involuntaryContextSwitches = tc.involuntaryReading;
        _metrics.update(snap);

        EXPECT_EQ(tc.expectedVoluntary,
                  _capturer.readInt64Counter<std::string_view>(MetricNames::kProcessContextSwitches,
                                                               {"voluntary"sv}));
        EXPECT_EQ(tc.expectedInvoluntary,
                  _capturer.readInt64Counter<std::string_view>(MetricNames::kProcessContextSwitches,
                                                               {"involuntary"sv}));
    }
}

TEST_F(ProcessHealthOtelMetricsTest, ThreadCount) {
    struct Case {
        std::string_view name;
        int64_t threads;
        int64_t expected;
    };

    // Thread count is an updown counter or level type metric.
    // Its value should simply be that of the most recent update.
    constexpr auto kCases = std::to_array<Case>({
        {"first_update"sv, 100, 100},
        {"second_update"sv, 160, 160},
        {"decrease"sv, 120, 120},
        {"zero"sv, 0, 0},
        // One would hope that the mongod process is never using -10 threads,
        // but this is technically valid for an UpDown counter.
        {"negative"sv, -10, -10},
    });

    for (const auto& tc : kCases) {
        SCOPED_TRACE(fmt::format("name={}", tc.name));
        ProcessHealthSnapshot snap;
        snap.threadCount = tc.threads;
        _metrics.update(snap);

        // readInt64Counter is used for UpDown counters too
        EXPECT_EQ(tc.expected, _capturer.readInt64Counter(MetricNames::kProcessThreadCount));
    }
}

struct FaultTypeField {
    std::string_view type;
    int64_t ProcessHealthSnapshot::* field;
};
inline constexpr auto kFaultTypeFields = std::to_array<FaultTypeField>({
    {"major"sv, &ProcessHealthSnapshot::majorPagingFaults},
    {"minor"sv, &ProcessHealthSnapshot::minorPagingFaults},
});

TEST_F(ProcessHealthOtelMetricsTest, PagingFaults) {
    struct Case {
        std::string_view name;
        std::vector<int64_t> deltaSeries;
        int64_t expected;
    };

    const auto cases = std::to_array<Case>({
        {"first_update_sets_metrics_correctly"sv, {1000}, 1000},
        {"second_update_sets_metrics_correctly"sv, {1000, 1500}, 1500},
        {"negative_delta_on_wrap_is_skipped"sv, {5000, 1000}, 5000},
    });

    for (const auto& tc : cases) {
        SCOPED_TRACE(fmt::format("name={}", tc.name));
        for (const auto& delta : tc.deltaSeries) {
            ProcessHealthSnapshot snap;
            for (const auto& typeField : kFaultTypeFields)
                snap.*typeField.field = delta;
            _metrics.update(snap);
        }

        for (const auto& typeField : kFaultTypeFields)
            EXPECT_EQ(tc.expected,
                      _capturer.readInt64Counter<std::string_view>(
                          MetricNames::kProcessPagingFaults, {typeField.type}));
    }
}

#ifdef __linux__
TEST_F(ProcessHealthOtelMetricsTest, CollectErrorsIncrementOnParseFailure) {
    FailPointEnableBlock fp("failCollectProcessHealthSnapshot");
    runProcessHealthCollectionCycle(_metrics);
    ASSERT_EQ(1, _capturer.readInt64Counter(MetricNames::kProcessHealthCollectErrors));
}
#endif

}  // namespace
}  // namespace mongo
