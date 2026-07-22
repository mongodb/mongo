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

// Maps each field of the snapshot to the counter it hydrates.
struct CounterField {
    std::string_view name;
    int64_t WiredTigerStatsSnapshot::* field;
    MetricName metric;
};

inline const auto kCounterFields = std::to_array<CounterField>({
    {"evictionCallsToGetPageFoundQueueEmpty"sv,
     &WiredTigerStatsSnapshot::evictionCallsToGetPageFoundQueueEmpty,
     MetricNames::kEvictionCallsToGetAPageFoundQueueEmpty},
    {"evictPageAttemptsByWorkerThreads"sv,
     &WiredTigerStatsSnapshot::evictPageAttemptsByWorkerThreads,
     MetricNames::kEvictPageAttemptsByEvictionWorkerThreads},
    {"evictPageFailuresByWorkerThreads"sv,
     &WiredTigerStatsSnapshot::evictPageFailuresByWorkerThreads,
     MetricNames::kEvictPageFailuresByEvictionWorkerThreads},
    {"pageEvictAttemptsByAppThreads"sv,
     &WiredTigerStatsSnapshot::pageEvictAttemptsByAppThreads,
     MetricNames::kPageEvictAttemptsByApplicationThreads},
    {"pageEvictFailuresByAppThreads"sv,
     &WiredTigerStatsSnapshot::pageEvictFailuresByAppThreads,
     MetricNames::kPageEvictFailuresByApplicationThreads},
    {"bytesReadIntoCache"sv,
     &WiredTigerStatsSnapshot::bytesReadIntoCache,
     MetricNames::kBytesReadIntoCache},
    {"bytesWrittenFromCache"sv,
     &WiredTigerStatsSnapshot::bytesWrittenFromCache,
     MetricNames::kBytesWrittenFromCache},
    {"pagesReadIntoCache"sv,
     &WiredTigerStatsSnapshot::pagesReadIntoCache,
     MetricNames::kPagesReadIntoCache},
    {"pagesRequestedFromCache"sv,
     &WiredTigerStatsSnapshot::pagesRequestedFromCache,
     MetricNames::kPagesRequestedFromTheCache},
});

// Maps each field of the snapshot to the gauge it hydrates.
struct WTGaugeField {
    std::string_view name;
    int64_t WiredTigerStatsSnapshot::* field;
    MetricName metric;
};

inline const auto kWiredTigerGaugeFields = std::to_array<WTGaugeField>({
    {"evictionEmptyScore"sv,
     &WiredTigerStatsSnapshot::evictionEmptyScore,
     MetricNames::kEvictionEmptyScore},
    {"evictionWorkerThreadActive"sv,
     &WiredTigerStatsSnapshot::evictionWorkerThreadActive,
     MetricNames::kEvictionWorkerThreadActive},
    {"evictionWorkerThreadStableNumber"sv,
     &WiredTigerStatsSnapshot::evictionWorkerThreadStableNumber,
     MetricNames::kEvictionWorkerThreadStableNumber},
    {"bytesCurrentlyInCache"sv,
     &WiredTigerStatsSnapshot::bytesCurrentlyInCache,
     MetricNames::kBytesCurrentlyInTheCache},
    {"trackedDirtyBytesInCache"sv,
     &WiredTigerStatsSnapshot::trackedDirtyBytesInCache,
     MetricNames::kTrackedDirtyBytesInTheCache},
    {"maximumBytesConfigured"sv,
     &WiredTigerStatsSnapshot::maximumBytesConfigured,
     MetricNames::kMaximumBytesConfigured},
    {"connectionDataHandlesCurrentlyActive"sv,
     &WiredTigerStatsSnapshot::connectionDataHandlesCurrentlyActive,
     MetricNames::kConnectionDataHandlesCurrentlyActive},
    {"transactionCheckpointMostRecentTimeMsecs"sv,
     &WiredTigerStatsSnapshot::transactionCheckpointMostRecentTimeMsecs,
     MetricNames::kTransactionCheckpointMostRecentTime},
});

TEST_F(WiredTigerOtelMetricsTest, CountableWTMetrics) {
    struct Case {
        std::string_view name;
        std::vector<int64_t> readings;
        int64_t expectedIncrease;
    };

    // Counters accumulate the delta between sequential readings.
    // Negative deltas are skipped.
    const auto cases = std::to_array<Case>({
        {"first_reading_sets_full_total"sv, {1000}, 1000},
        {"second_reading_adds_delta"sv, {1000, 1500}, 1500},
        {"negative_delta_on_wrap_is_skipped"sv, {5000, 1000}, 5000},
        {"total_delta_zero"sv, {1000, 1000}, 1000},
    });

    for (const auto& tc : cases) {
        SCOPED_TRACE(fmt::format("name={}", tc.name));

        // These tests create a new WiredTigerMetrics instance per case so
        // metrics do not persist across different test cases.
        std::array<int64_t, kCounterFields.size()> base;
        for (size_t i = 0; i < kCounterFields.size(); ++i)
            base[i] = _capturer.readInt64Counter(kCounterFields[i].metric);

        WiredTigerMetrics metrics;
        for (int64_t reading : tc.readings) {
            WiredTigerStatsSnapshot snap;
            for (const auto& f : kCounterFields)
                snap.*f.field = reading;
            metrics.updateWiredTiger(snap);
        }

        for (size_t i = 0; i < kCounterFields.size(); ++i) {
            SCOPED_TRACE(fmt::format("field={}", kCounterFields[i].name));
            EXPECT_EQ(base[i] + tc.expectedIncrease,
                      _capturer.readInt64Counter(kCounterFields[i].metric));
        }
    }
}

TEST_F(WiredTigerOtelMetricsTest, PointInTimeWTMetrics) {
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
            WiredTigerStatsSnapshot snap;
            for (const auto& f : kWiredTigerGaugeFields)
                snap.*f.field = reading;
            _metrics.updateWiredTiger(snap);
        }

        for (const auto& f : kWiredTigerGaugeFields) {
            SCOPED_TRACE(fmt::format("field={}", f.name));
            EXPECT_EQ(tc.expected, _capturer.readInt64Gauge(f.metric));
        }
    }
}

TEST(WiredTigerParseStatsTest, ParsesAllKeys) {
    BSONObj stats =
        BSON("cache" << BSON("eviction calls to get a page found queue empty"
                             << 1 << "evict page attempts by eviction worker threads" << 2
                             << "evict page failures by eviction worker threads" << 3
                             << "page evict attempts by application threads" << 4
                             << "page evict failures by application threads" << 5
                             << "bytes read into cache" << 6 << "bytes written from cache" << 7
                             << "pages read into cache" << 8 << "pages requested from the cache"
                             << 9 << "eviction empty score" << 10 << "eviction worker thread active"
                             << 11 << "eviction worker thread stable number" << 12
                             << "bytes currently in the cache" << 13
                             << "tracked dirty bytes in the cache" << 14
                             << "maximum bytes configured" << 15)
                     << "checkpoint" << BSON("most recent time (msecs)" << 16) << "data-handle"
                     << BSON("connection data handles currently active" << 17));

    WiredTigerStatsSnapshot snap = parseWiredTigerStats(stats);

    ASSERT_EQ(snap.evictionCallsToGetPageFoundQueueEmpty, 1);
    ASSERT_EQ(snap.evictPageAttemptsByWorkerThreads, 2);
    ASSERT_EQ(snap.evictPageFailuresByWorkerThreads, 3);
    ASSERT_EQ(snap.pageEvictAttemptsByAppThreads, 4);
    ASSERT_EQ(snap.pageEvictFailuresByAppThreads, 5);
    ASSERT_EQ(snap.bytesReadIntoCache, 6);
    ASSERT_EQ(snap.bytesWrittenFromCache, 7);
    ASSERT_EQ(snap.pagesReadIntoCache, 8);
    ASSERT_EQ(snap.pagesRequestedFromCache, 9);
    ASSERT_EQ(snap.evictionEmptyScore, 10);
    ASSERT_EQ(snap.evictionWorkerThreadActive, 11);
    ASSERT_EQ(snap.evictionWorkerThreadStableNumber, 12);
    ASSERT_EQ(snap.bytesCurrentlyInCache, 13);
    ASSERT_EQ(snap.trackedDirtyBytesInCache, 14);
    ASSERT_EQ(snap.maximumBytesConfigured, 15);
    ASSERT_EQ(snap.transactionCheckpointMostRecentTimeMsecs, 16);
    ASSERT_EQ(snap.connectionDataHandlesCurrentlyActive, 17);
}

// Missing keys return 0 instead of crashing.
TEST(WiredTigerParseStatsTest, MissingFieldsDefaultToZero) {
    WiredTigerStatsSnapshot snap = parseWiredTigerStats(BSONObj());

    ASSERT_EQ(snap.evictionCallsToGetPageFoundQueueEmpty, 0);
    ASSERT_EQ(snap.bytesReadIntoCache, 0);
    ASSERT_EQ(snap.maximumBytesConfigured, 0);
    ASSERT_EQ(snap.transactionCheckpointMostRecentTimeMsecs, 0);
    ASSERT_EQ(snap.connectionDataHandlesCurrentlyActive, 0);
}

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
