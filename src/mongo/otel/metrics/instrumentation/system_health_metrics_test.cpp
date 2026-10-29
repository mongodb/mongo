// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/instrumentation/system_health_metrics.h"

#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

#ifdef __linux__
#include "mongo/util/fail_point.h"
#endif

#ifdef __linux__
namespace mongo {
extern void runSystemHealthCollectionCycle(SystemHealthMetrics&);
}
#endif

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

using otel::metrics::AttributeDefinition;
using otel::metrics::MetricNames;
using otel::metrics::OtelMetricsCapturer;

class SystemHealthOtelMetricsTest : public unittest::Test {
protected:
    void setUp() override {
        if (!OtelMetricsCapturer::canReadMetrics()) {
            GTEST_SKIP() << "Skipping test: OTel metrics unavailable on this platform";
        }
    }

    OtelMetricsCapturer _capturer;
    SystemHealthMetrics _metrics;
};

TEST_F(SystemHealthOtelMetricsTest, FirstUpdateSetsMetricsCorrectly) {
    SystemHealthSnapshot snap;
    snap.cpuUserMs = 1000;
    snap.cpuSystemMs = 500;
    snap.cpuIowaitMs = 200;
    snap.procsRunning = 4;
    snap.procsBlocked = 1;
    snap.fdOpen = 512;

    _metrics.update(snap);

    ASSERT_EQ(1000,
              _capturer.readInt64Counter<std::string_view>(MetricNames::kCpuTime, {"user"sv}));
    ASSERT_EQ(500,
              _capturer.readInt64Counter<std::string_view>(MetricNames::kCpuTime, {"system"sv}));
    ASSERT_EQ(200,
              _capturer.readInt64Counter<std::string_view>(MetricNames::kCpuTime, {"iowait"sv}));
    ASSERT_EQ(4, _capturer.readInt64Gauge(MetricNames::kThreadActive));
    ASSERT_EQ(1, _capturer.readInt64Gauge(MetricNames::kThreadQueued));
    ASSERT_EQ(512, _capturer.readInt64Gauge(MetricNames::kFdOpen));
}

TEST_F(SystemHealthOtelMetricsTest, SecondUpdateUpdatesMetricsCorrectly) {
    SystemHealthSnapshot first;
    first.cpuUserMs = 1000;
    first.cpuSystemMs = 500;
    first.cpuIowaitMs = 200;
    first.procsRunning = 2;
    first.procsBlocked = 0;
    first.fdOpen = 256;
    _metrics.update(first);

    SystemHealthSnapshot second;
    second.cpuUserMs = 1300;
    second.cpuSystemMs = 650;
    second.cpuIowaitMs = 250;
    second.procsRunning = 5;
    second.procsBlocked = 3;
    second.fdOpen = 100;
    _metrics.update(second);

    // Counters accumulate deltas: first=1000/500/200, delta=300/150/50.
    ASSERT_EQ(1300,
              _capturer.readInt64Counter<std::string_view>(MetricNames::kCpuTime, {"user"sv}));
    ASSERT_EQ(650,
              _capturer.readInt64Counter<std::string_view>(MetricNames::kCpuTime, {"system"sv}));
    ASSERT_EQ(250,
              _capturer.readInt64Counter<std::string_view>(MetricNames::kCpuTime, {"iowait"sv}));

    ASSERT_EQ(5, _capturer.readInt64Gauge(MetricNames::kThreadActive));
    ASSERT_EQ(3, _capturer.readInt64Gauge(MetricNames::kThreadQueued));
    ASSERT_EQ(100, _capturer.readInt64Gauge(MetricNames::kFdOpen));
}

TEST_F(SystemHealthOtelMetricsTest, NegativeDeltaOnWrapIsSkipped) {
    SystemHealthSnapshot high;
    high.cpuUserMs = 5000;
    high.cpuSystemMs = 2000;
    high.cpuIowaitMs = 800;
    high.procsRunning = 1;
    high.procsBlocked = 0;
    high.fdOpen = 100;
    _metrics.update(high);

    SystemHealthSnapshot low;
    low.cpuUserMs = 100;
    low.cpuSystemMs = 50;
    low.cpuIowaitMs = 10;
    low.procsRunning = 1;
    low.procsBlocked = 0;
    low.fdOpen = 100;
    _metrics.update(low);

    // Negative deltas are skipped; counters retain the value from before the reset.
    ASSERT_EQ(5000,
              _capturer.readInt64Counter<std::string_view>(MetricNames::kCpuTime, {"user"sv}));
    ASSERT_EQ(2000,
              _capturer.readInt64Counter<std::string_view>(MetricNames::kCpuTime, {"system"sv}));
    ASSERT_EQ(800,
              _capturer.readInt64Counter<std::string_view>(MetricNames::kCpuTime, {"iowait"sv}));
}

struct CPUModeField {
    std::string_view mode;
    int64_t SystemHealthSnapshot::* field;
};

inline constexpr auto kCpuModeFields = std::to_array<CPUModeField>({
    {"user"sv, &SystemHealthSnapshot::cpuUserMs},
    {"nice"sv, &SystemHealthSnapshot::cpuNiceMs},
    {"system"sv, &SystemHealthSnapshot::cpuSystemMs},
    {"idle"sv, &SystemHealthSnapshot::cpuIdleMs},
    {"iowait"sv, &SystemHealthSnapshot::cpuIowaitMs},
    {"irq"sv, &SystemHealthSnapshot::cpuIrqMs},
    {"softirq"sv, &SystemHealthSnapshot::cpuSoftirqMs},
    {"steal"sv, &SystemHealthSnapshot::cpuStealMs},
    {"guest"sv, &SystemHealthSnapshot::cpuGuestMs},
    {"guest_nice"sv, &SystemHealthSnapshot::cpuGuestNiceMs},
});

TEST_F(SystemHealthOtelMetricsTest, CpuTimeDeltaAllFields) {
    static constexpr int64_t kDelta = 50;

    // Build a table of snapshots, all 10 CPU time metrics have a value that is
    // a unique multiple of 100 to avoid confusion in different test cases. They
    // all increase by `kDelta` for the second snapshot.
    SystemHealthSnapshot first{}, second{};
    for (size_t i = 0; i < kCpuModeFields.size(); ++i) {
        first.*kCpuModeFields[i].field = 1000 - 100 * static_cast<int64_t>(i);
        second.*kCpuModeFields[i].field = first.*kCpuModeFields[i].field + kDelta;
    }
    _metrics.update(first);
    _metrics.update(second);

    for (const auto& c : kCpuModeFields) {
        SCOPED_TRACE(fmt::format("mode={}", c.mode));
        EXPECT_EQ(second.*c.field,
                  _capturer.readInt64Counter<std::string_view>(MetricNames::kCpuTime, {c.mode}));
    }
}

TEST_F(SystemHealthOtelMetricsTest, CpuUtilization) {
    struct Case {
        std::string_view name;
        std::array<int64_t, 10> deltas;
        std::array<double, 10> expectedRatios;
    };
    constexpr auto kCases = std::to_array<Case>({
        {"uniform_deltas",
         {50, 50, 50, 50, 50, 50, 50, 50, 50, 50},
         {0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1}},
        {"varied_deltas",
         {50, 100, 150, 50, 100, 150, 50, 100, 150, 100},
         {0.05, 0.1, 0.15, 0.05, 0.1, 0.15, 0.05, 0.1, 0.15, 0.1}},
        {"single_mode",
         {0, 0, 1000, 0, 0, 0, 0, 0, 0, 0},
         {0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
    });

    for (const auto& c : kCases) {
        SCOPED_TRACE(fmt::format("name={}", c.name));
        // We're not using the `_metrics` member here on purpose; we don't want
        // to accidentally carry forward state across cases.
        SystemHealthMetrics metrics;
        SystemHealthSnapshot first{}, second{};
        for (size_t i = 0; i < kCpuModeFields.size(); ++i) {
            first.*kCpuModeFields[i].field = 100;
            second.*kCpuModeFields[i].field = first.*kCpuModeFields[i].field + c.deltas[i];
        }
        metrics.update(first);
        metrics.update(second);

        for (size_t i = 0; i < c.expectedRatios.size(); ++i) {
            EXPECT_DOUBLE_EQ(c.expectedRatios[i],
                             _capturer.readDoubleGauge<std::string_view>(
                                 MetricNames::kCpuUtilization, {kCpuModeFields[i].mode}));
        }
    }
}

#ifdef __linux__
TEST_F(SystemHealthOtelMetricsTest, CollectErrorsIncrementOnParseFailure) {
    FailPointEnableBlock fp("failCollectSystemHealthSnapshot");
    runSystemHealthCollectionCycle(_metrics);
    ASSERT_EQ(1, _capturer.readInt64Counter(MetricNames::kSystemHealthCollectErrors));
}
#endif

}  // namespace
}  // namespace mongo
