/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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

    ASSERT_EQ(1000, _capturer.readInt64Counter<StringData>(MetricNames::kCpuTime, {"user"_sd}));
    ASSERT_EQ(500, _capturer.readInt64Counter<StringData>(MetricNames::kCpuTime, {"system"_sd}));
    ASSERT_EQ(200, _capturer.readInt64Counter<StringData>(MetricNames::kCpuTime, {"iowait"_sd}));
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
    ASSERT_EQ(1300, _capturer.readInt64Counter<StringData>(MetricNames::kCpuTime, {"user"_sd}));
    ASSERT_EQ(650, _capturer.readInt64Counter<StringData>(MetricNames::kCpuTime, {"system"_sd}));
    ASSERT_EQ(250, _capturer.readInt64Counter<StringData>(MetricNames::kCpuTime, {"iowait"_sd}));

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
    ASSERT_EQ(5000, _capturer.readInt64Counter<StringData>(MetricNames::kCpuTime, {"user"_sd}));
    ASSERT_EQ(2000, _capturer.readInt64Counter<StringData>(MetricNames::kCpuTime, {"system"_sd}));
    ASSERT_EQ(800, _capturer.readInt64Counter<StringData>(MetricNames::kCpuTime, {"iowait"_sd}));
}

struct CPUModeField {
    StringData mode;
    int64_t SystemHealthSnapshot::* field;
};

inline constexpr auto kCpuModeFields = std::to_array<CPUModeField>({
    {"user"_sd, &SystemHealthSnapshot::cpuUserMs},
    {"nice"_sd, &SystemHealthSnapshot::cpuNiceMs},
    {"system"_sd, &SystemHealthSnapshot::cpuSystemMs},
    {"idle"_sd, &SystemHealthSnapshot::cpuIdleMs},
    {"iowait"_sd, &SystemHealthSnapshot::cpuIowaitMs},
    {"irq"_sd, &SystemHealthSnapshot::cpuIrqMs},
    {"softirq"_sd, &SystemHealthSnapshot::cpuSoftirqMs},
    {"steal"_sd, &SystemHealthSnapshot::cpuStealMs},
    {"guest"_sd, &SystemHealthSnapshot::cpuGuestMs},
    {"guest_nice"_sd, &SystemHealthSnapshot::cpuGuestNiceMs},
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
                  _capturer.readInt64Counter<StringData>(MetricNames::kCpuTime, {c.mode}));
    }
}

TEST_F(SystemHealthOtelMetricsTest, CpuUtilization) {
    struct Case {
        StringData name;
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
                             _capturer.readDoubleGauge<StringData>(MetricNames::kCpuUtilization,
                                                                   {kCpuModeFields[i].mode}));
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
