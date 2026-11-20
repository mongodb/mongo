// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/instrumentation/wiredtiger_metrics.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

#include <array>
#include <string_view>
#include <vector>

namespace mongo {
namespace {

using namespace std::literals::string_view_literals;

using otel::metrics::MetricName;
using otel::metrics::MetricNames;
using otel::metrics::OtelMetricsCapturer;

class WiredTigerOtelMetricsTest : public unittest::Test {
protected:
    void setUp() override {
        if (!OtelMetricsCapturer::canReadMetrics()) {
            GTEST_SKIP() << "Skipping test: OTel metrics unavailable on this platform";
        }
    }

    OtelMetricsCapturer _capturer;
    WiredTigerMetrics _metrics;
};

TEST(TicketingSystemParseStatsTest, ParsesAllKeys) {
    BSONObj stats = BSON("read" << BSON("available" << 20) << "write" << BSON("available" << 40));

    TicketingSystemStatsSnapshot snap = parseTicketingSystemStats(stats);

    ASSERT_EQ(snap.readAvailable, 20);
    ASSERT_EQ(snap.writeAvailable, 40);
}

struct TSGaugeField {
    std::string_view name;
    int64_t TicketingSystemStatsSnapshot::* field;
    MetricName metric;
};

inline const auto kTicketingSystemGaugeFields = std::to_array<TSGaugeField>({
    {"readAvailable"sv,
     &TicketingSystemStatsSnapshot::readAvailable,
     MetricNames::kConcurrentTransactionsReadAvailable},
    {"writeAvailable"sv,
     &TicketingSystemStatsSnapshot::writeAvailable,
     MetricNames::kConcurrentTransactionsWriteAvailable},
});

TEST_F(WiredTigerOtelMetricsTest, PointInTimeTSMetrics) {
    struct Case {
        std::string_view name;
        std::vector<int64_t> readings;
        int64_t expected;
    };

    const auto cases = std::to_array<Case>({
        {"single_reading"sv, {1000}, 1000},
        {"latest_reading_wins"sv, {1000, 1500}, 1500},
        {"decrease_is_reflected"sv, {5000, 1000}, 1000},
        {"zero"sv, {0}, 0},
    });

    for (const auto& tc : cases) {
        SCOPED_TRACE(fmt::format("name={}", tc.name));
        for (int64_t reading : tc.readings) {
            TicketingSystemStatsSnapshot snap;
            for (const auto& f : kTicketingSystemGaugeFields)
                snap.*f.field = reading;
            _metrics.updateTicketingSystem(snap);
        }

        for (const auto& f : kTicketingSystemGaugeFields) {
            SCOPED_TRACE(fmt::format("field={}", f.name));
            EXPECT_EQ(tc.expected, _capturer.readInt64Gauge(f.metric));
        }
    }
}

}  // namespace
}  // namespace mongo
