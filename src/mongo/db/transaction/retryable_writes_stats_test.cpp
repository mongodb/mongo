// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/transaction/retryable_writes_stats.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

namespace mongo {
namespace {

using namespace std::string_literals;

// Reads the `retriedWritesDelayMillis` array emitted by appendRetriedWriteStats into a flat vector
// of (lowerBound, count) pairs, one per bucket in bucket order.
std::vector<std::pair<long long, long long>> readBuckets(RetryableWritesStats& stats) {
    BSONObjBuilder bob;
    stats.appendRetriedWriteStats(bob);
    BSONObj obj = bob.obj();

    std::vector<std::pair<long long, long long>> buckets;
    for (auto&& elem : obj["retriedWritesDelayMillis"s].Array()) {
        auto bucket = elem.Obj();
        buckets.emplace_back(bucket["lowerBound"s].Long(), bucket["count"s].Long());
    }
    return buckets;
}

TEST(RetryableWritesStats, NoSamplesYieldsEmptyBuckets) {
    RetryableWritesStats stats;
    auto buckets = readBuckets(stats);

    // 13 partitions -> 14 buckets (first = 0ms sub-lowest, last open-ended >=20min).
    ASSERT_EQ(buckets.size(), 14u);
    for (const auto& [lowerBound, count] : buckets) {
        ASSERT_EQ(count, 0);
    }
    // The implicit first bucket is represented with a 0 lowerBound.
    ASSERT_EQ(buckets[0].first, 0);
}

TEST(RetryableWritesStats, NegativeDelayClampsToZeroBucket) {
    RetryableWritesStats stats;
    stats.recordRetriedWriteDelay(Milliseconds{-1});      // -1ms -> recorded as 0ms.
    stats.recordRetriedWriteDelay(Milliseconds{-1'000});  // -1s -> recorded as 0ms.

    auto buckets = readBuckets(stats);
    ASSERT_EQ(buckets[0].second, 2);
    // Nothing else was recorded.
    for (size_t i = 1; i < buckets.size(); ++i) {
        ASSERT_EQ(buckets[i].second, 0);
    }
}

TEST(RetryableWritesStats, RecordsDelaysIntoExpectedBuckets) {
    RetryableWritesStats stats;

    // Partitions {1,10,100,500,1000,5000,10000,30000,60000,180000,300000,600000,1200000} give 14
    // buckets: index i covers [part[i-1], part[i]). The top bucket is open-ended >=20min, capped
    // under gTransactionRecordMinimumLifetimeMinutes so only detectable retries are counted.
    // Bucket 0 captures 0ms and any negative sample (recorded as 0ms).
    stats.recordRetriedWriteDelay(Milliseconds{0});
    stats.recordRetriedWriteDelay(Milliseconds{0});
    stats.recordRetriedWriteDelay(Milliseconds{-5});  // -5ms is recorded as 0ms.
    // Bucket 1: [1, 10)ms.
    stats.recordRetriedWriteDelay(Milliseconds{1});
    stats.recordRetriedWriteDelay(Milliseconds{4});
    // Bucket 2: [10, 100)ms.
    stats.recordRetriedWriteDelay(Milliseconds{10});
    // Bucket 3: [100, 500)ms.
    stats.recordRetriedWriteDelay(Milliseconds{100});
    // Bucket 4: [500, 1000)ms.
    stats.recordRetriedWriteDelay(Milliseconds{500});
    stats.recordRetriedWriteDelay(Milliseconds{550});
    // Bucket 5: [1s, 5s).
    stats.recordRetriedWriteDelay(Milliseconds{1'000});
    // Bucket 6: [5s, 10s).
    stats.recordRetriedWriteDelay(Milliseconds{5'000});
    // Bucket 7: [10s, 30s).
    stats.recordRetriedWriteDelay(Milliseconds{10'000});
    // Bucket 8: [30s, 60s).
    stats.recordRetriedWriteDelay(Milliseconds{30'000});
    // Bucket 9: [1m, 3m).
    stats.recordRetriedWriteDelay(Milliseconds{60'000});
    // Bucket 10: [3m, 5m).
    stats.recordRetriedWriteDelay(Milliseconds{180'000});
    // Bucket 11: [5m, 10m).
    stats.recordRetriedWriteDelay(Milliseconds{300'000});
    // Bucket 12: [10m, 20m).
    stats.recordRetriedWriteDelay(Milliseconds{600'000});
    // Open-ended last bucket: [>=20m).
    stats.recordRetriedWriteDelay(Milliseconds{1'200'000});
    stats.recordRetriedWriteDelay(Milliseconds{86'400'000});  // 1 day.

    auto buckets = readBuckets(stats);
    ASSERT_EQ(buckets.size(), 14u);
    ASSERT_EQ(buckets[0].second, 3);
    ASSERT_EQ(buckets[1].second, 2);
    ASSERT_EQ(buckets[2].second, 1);
    ASSERT_EQ(buckets[3].second, 1);
    ASSERT_EQ(buckets[4].second, 2);
    ASSERT_EQ(buckets[5].second, 1);
    ASSERT_EQ(buckets[6].second, 1);
    ASSERT_EQ(buckets[7].second, 1);
    ASSERT_EQ(buckets[8].second, 1);
    ASSERT_EQ(buckets[9].second, 1);
    ASSERT_EQ(buckets[10].second, 1);
    ASSERT_EQ(buckets[11].second, 1);
    ASSERT_EQ(buckets[12].second, 1);
    ASSERT_EQ(buckets[13].second, 2);
}

}  // namespace
}  // namespace mongo
