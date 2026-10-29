// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/instrumentation/wiredtiger_metric_registry.h"

#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/otel/metrics/otel_metric_name_validation.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <string_view>

namespace mongo {
namespace {

using otel::metrics::DynamicMetricNameMaker;
using otel::metrics::DynamicMetricNameTestPasskeyMaker;
using otel::metrics::OtelMetricsCapturer;

class WiredTigerMetricRegistryTest : public unittest::Test {
protected:
    void setUp() override {
        if (!OtelMetricsCapturer::canReadMetrics()) {
            GTEST_SKIP() << "Skipping test: OTel metrics unavailable on this platform";
        }
    }

    int64_t readCounter(const std::string& statName) {
        auto name = wtStatNameToOtelName(statName);
        return _capturer.readInt64Counter(DynamicMetricNameMaker::make(
            std::string_view{name}, DynamicMetricNameTestPasskeyMaker::make()));
    }

    int64_t readGauge(const std::string& statName) {
        auto name = wtStatNameToOtelName(statName);
        return _capturer.readInt64Gauge(DynamicMetricNameMaker::make(
            std::string_view{name}, DynamicMetricNameTestPasskeyMaker::make()));
    }

    OtelMetricsCapturer _capturer;
    WiredTigerMetricRegistry _registry;
};

TEST(WtStatNameTranslationTest, TranslatesSectionsAndSeparators) {
    ASSERT_EQ(wtStatNameToOtelName("cache: bytes read into cache"),
              "mongodb.serverStatus.wiredTiger.cache.bytes_read_into_cache");
    ASSERT_EQ(wtStatNameToOtelName("data-handle: connection data handles currently active"),
              "mongodb.serverStatus.wiredTiger.data-handle.connection_data_handles_currently_"
              "active");
    // Punctuation is dropped rather than turned into an extra underscore.
    ASSERT_EQ(wtStatNameToOtelName("checkpoint: most recent time (msecs)"),
              "mongodb.serverStatus.wiredTiger.checkpoint.most_recent_time_msecs");
    ASSERT_EQ(wtStatNameToOtelName("bytes read into cache"),
              "mongodb.serverStatus.wiredTiger.bytes_read_into_cache");
}

TEST_F(WiredTigerMetricRegistryTest, CounterAccumulatesIncrements) {
    const std::string stat = "test-registry: counter accumulates increments";

    // WiredTiger reports the increase since the last reading, so the counter is the running sum.
    _registry.updateCounter(stat, 500);
    // A stat that did not move reports no increase.
    _registry.updateCounter(stat, 0);
    _registry.updateCounter(stat, 300);

    ASSERT_EQ(readCounter(stat), 800);
}

TEST_F(WiredTigerMetricRegistryTest, GaugeReportsLatestReading) {
    const std::string stat = "test-registry: gauge reports latest reading";

    _registry.updateGauge(stat, 1000);
    ASSERT_EQ(readGauge(stat), 1000);

    _registry.updateGauge(stat, 300);
    ASSERT_EQ(readGauge(stat), 300);
}

TEST_F(WiredTigerMetricRegistryTest, UntranslatableStatsAreSkipped) {
    // Neither call should throw; the stat is dropped on the first attempt and never retried.
    _registry.updateCounter("(...)", 10);
    _registry.updateCounter("(...)", 20);
    _registry.updateGauge("cache: 64KB pages", 30);
    _registry.updateGauge("cache: 64KB pages", 40);
}

}  // namespace
}  // namespace mongo
