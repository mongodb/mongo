// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/end_of_transaction_metadata_shard_cache.h"

#include "mongo/db/s/transaction_coordinator_test_fixture.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

#include <string>
#include <vector>

namespace mongo {
namespace {

class EndOfTransactionMetadataShardCacheTest : public TransactionCoordinatorTestFixture {
protected:
    EndOfTransactionMetadataShardCacheTest()
        : TransactionCoordinatorTestFixture(Options{}.useMockClock(true)) {}

    EndOfTransactionMetadataShardCache& cache() {
        return EndOfTransactionMetadataShardCache::get(getServiceContext());
    }

    static ShardId shardIdFor(int i) {
        return ShardId("s" + std::to_string(i));
    }
};

TEST_F(EndOfTransactionMetadataShardCacheTest, ShardsAreEligibleByDefault) {
    ASSERT_TRUE(cache().isShardDefinitelyEligible(shardIdFor(1)));
    ASSERT_TRUE(cache().isShardDefinitelyEligible(shardIdFor(2)));
}

TEST_F(EndOfTransactionMetadataShardCacheTest, MarkedShardBecomesIneligibleButOthersStayEligible) {
    cache().markShardIneligible(shardIdFor(1));

    ASSERT_FALSE(cache().isShardDefinitelyEligible(shardIdFor(1)));
    ASSERT_TRUE(cache().isShardDefinitelyEligible(shardIdFor(2)));
}

TEST_F(EndOfTransactionMetadataShardCacheTest, IneligibilityExpiresAfterTtl) {
    cache().markShardIneligible(shardIdFor(1));
    ASSERT_FALSE(cache().isShardDefinitelyEligible(shardIdFor(1)));

    clockSource()->advance(EndOfTransactionMetadataShardCache::kIneligibilityTtl + Seconds(1));
    ASSERT_TRUE(cache().isShardDefinitelyEligible(shardIdFor(1)));
}

TEST_F(EndOfTransactionMetadataShardCacheTest, IneligibleShardBecomesEligibleAtExactlyTtl) {
    cache().markShardIneligible(shardIdFor(1));
    ASSERT_FALSE(cache().isShardDefinitelyEligible(shardIdFor(1)));

    clockSource()->advance(EndOfTransactionMetadataShardCache::kIneligibilityTtl);
    ASSERT_TRUE(cache().isShardDefinitelyEligible(shardIdFor(1)));
}

TEST_F(EndOfTransactionMetadataShardCacheTest, RemarkingExtendsIneligibility) {
    cache().markShardIneligible(shardIdFor(1));

    clockSource()->advance(EndOfTransactionMetadataShardCache::kIneligibilityTtl - Seconds(1));
    cache().markShardIneligible(shardIdFor(1));

    // The original entry would have expired by now, but re-marking refreshed it.
    clockSource()->advance(Seconds(2));
    ASSERT_FALSE(cache().isShardDefinitelyEligible(shardIdFor(1)));
}

TEST_F(EndOfTransactionMetadataShardCacheTest, ExpiredShardCanBeMarkedIneligibleAgain) {
    cache().markShardIneligible(shardIdFor(1));
    clockSource()->advance(EndOfTransactionMetadataShardCache::kIneligibilityTtl + Seconds(1));
    ASSERT_TRUE(cache().isShardDefinitelyEligible(shardIdFor(1)));

    cache().markShardIneligible(shardIdFor(1));
    ASSERT_FALSE(cache().isShardDefinitelyEligible(shardIdFor(1)));
}

TEST_F(EndOfTransactionMetadataShardCacheTest, MultipleShardsExpireIndependently) {
    cache().markShardIneligible(shardIdFor(1));
    clockSource()->advance(EndOfTransactionMetadataShardCache::kIneligibilityTtl - Seconds(1));
    cache().markShardIneligible(shardIdFor(2));

    // 's1' was marked a second earlier than 's2', so it expires first.
    clockSource()->advance(Seconds(2));
    ASSERT_TRUE(cache().isShardDefinitelyEligible(shardIdFor(1)));
    ASSERT_FALSE(cache().isShardDefinitelyEligible(shardIdFor(2)));
}

TEST_F(EndOfTransactionMetadataShardCacheTest, ConcurrentReadsAndWritesAreSafe) {
    constexpr int kNumShards = 8;
    constexpr int kIterations = 5000;

    std::vector<stdx::thread> threads;
    threads.reserve(2 * kNumShards);

    // Join every spawned thread on scope exit, so that an early return (for example a failed
    // assertion) does not leave running threads behind, which would terminate the test process.
    // 'joinable()' guards against joining twice when the threads are also joined explicitly below.
    auto joinAllThreads = [&threads] {
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    };
    ON_BLOCK_EXIT(joinAllThreads);

    // Hold every thread at a barrier until all of them have been spawned, then release them
    // together so that the readers and writers actually contend.
    unittest::Barrier barrier(2 * kNumShards);

    for (int t = 0; t < kNumShards; ++t) {
        threads.emplace_back([this, &barrier, t] {
            barrier.countDownAndWait();
            for (int i = 0; i < kIterations; ++i) {
                (void)cache().isShardDefinitelyEligible(shardIdFor((t + i) % kNumShards));
            }
        });
    }
    for (int t = 0; t < kNumShards; ++t) {
        threads.emplace_back([this, &barrier, t] {
            barrier.countDownAndWait();
            for (int i = 0; i < kIterations; ++i) {
                cache().markShardIneligible(shardIdFor((t + i) % kNumShards));
            }
        });
    }

    joinAllThreads();

    // Every shard was marked ineligible at least once and the clock never advanced, so none of them
    // may have expired.
    for (int t = 0; t < kNumShards; ++t) {
        ASSERT_FALSE(cache().isShardDefinitelyEligible(shardIdFor(t)));
    }
}

}  // namespace
}  // namespace mongo
