/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/util/chunked_memory_aggregator.h"
#include "mongo/util/concurrent_memory_aggregator.h"

#include <memory>
#include <tuple>
#include <vector>

#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {

namespace {

static const int64_t kMemoryUsageUpdateBatchSize = 32 * 1024 * 1024;  // 32 MB
static const ChunkedMemoryAggregator::Options kOptions = {.memoryUsageUpdateBatchSize =
                                                              kMemoryUsageUpdateBatchSize};

};  // namespace

class ConcurrentMemoryAggregatorTest : public unittest::Test {
public:
    const auto& getChunkedMemoryAggregators(ConcurrentMemoryAggregator* memoryAggregator) const {
        return memoryAggregator->_chunkedMemoryAggregators;
    }
};  // class MemoryAggregatorTest

TEST_F(ConcurrentMemoryAggregatorTest, Simple) {
    auto memoryAggregator = std::make_shared<ConcurrentMemoryAggregator>();
    const auto& localMemoryAggregators = getChunkedMemoryAggregators(memoryAggregator.get());
    ASSERT_EQUALS(0, memoryAggregator->getCurrentMemoryUsageBytes());
    ASSERT_TRUE(localMemoryAggregators.empty());

    {
        // Create a local memory aggregator and make sure that it has been added to the parent
        // memory aggregator's registry map.
        auto local = memoryAggregator->createChunkedMemoryAggregator(kOptions);
        ASSERT_EQUALS(0, local->getCurrentMemoryUsageBytes());
        ASSERT_EQUALS(1, localMemoryAggregators.size());

        // Create a resource handle and make sure that usage changes are propagated upstream
        // to the local memory aggregator and the global memory aggregator.
        int64_t usageBytes = kMemoryUsageUpdateBatchSize + 1;
        auto handle = local->createUsageHandle();
        handle.set(usageBytes);
        ASSERT_EQUALS(usageBytes, handle.getCurrentMemoryUsageBytes());
        ASSERT_EQUALS(usageBytes, local->getCurrentMemoryUsageBytes());

        // Memory updates are propagated to the global memory aggregator in batch sizes, so it's
        // not exact.
        ASSERT_EQUALS(2 * kMemoryUsageUpdateBatchSize,
                      memoryAggregator->getCurrentMemoryUsageBytes());

        // Decrease memory and make sure that change is also propagated stream.
        handle.set(1);
        ASSERT_EQUALS(1, handle.getCurrentMemoryUsageBytes());
        ASSERT_EQUALS(1, local->getCurrentMemoryUsageBytes());

        // Global parent memory aggregator should get updated but be non-zero aligned to the batch
        // size since we over count.
        ASSERT_EQUALS(kMemoryUsageUpdateBatchSize, memoryAggregator->getCurrentMemoryUsageBytes());
    }

    // After the local memory aggregator falls out of scope, it should have been removed from
    // the parent memory aggregator.
    ASSERT_TRUE(localMemoryAggregators.empty());
    ASSERT_EQUALS(0, memoryAggregator->getCurrentMemoryUsageBytes());
}

TEST_F(ConcurrentMemoryAggregatorTest, OverCount) {
    auto memoryAggregator = std::make_shared<ConcurrentMemoryAggregator>();
    auto local = memoryAggregator->createChunkedMemoryAggregator(kOptions);
    auto handle = local->createUsageHandle();

    // Parent memory aggregator memory usgae should be set to the batch size since we always over
    // count.
    handle.set(1);
    ASSERT_EQUALS(1, handle.getCurrentMemoryUsageBytes());
    ASSERT_EQUALS(1, local->getCurrentMemoryUsageBytes());
    ASSERT_EQUALS(kMemoryUsageUpdateBatchSize, memoryAggregator->getCurrentMemoryUsageBytes());

    // Parent memory aggregator should not be updated since it hasn't changed significantly since
    // the last update.
    handle.set(2);
    ASSERT_EQUALS(2, handle.getCurrentMemoryUsageBytes());
    ASSERT_EQUALS(2, local->getCurrentMemoryUsageBytes());
    ASSERT_EQUALS(kMemoryUsageUpdateBatchSize, memoryAggregator->getCurrentMemoryUsageBytes());

    // Exact alignment on the batch size should lead to a precise recording.
    handle.set(kMemoryUsageUpdateBatchSize);
    ASSERT_EQUALS(kMemoryUsageUpdateBatchSize, handle.getCurrentMemoryUsageBytes());
    ASSERT_EQUALS(kMemoryUsageUpdateBatchSize, local->getCurrentMemoryUsageBytes());
    ASSERT_EQUALS(kMemoryUsageUpdateBatchSize, memoryAggregator->getCurrentMemoryUsageBytes());

    // One over the batch size should round up to over count.
    handle.set(kMemoryUsageUpdateBatchSize + 1);
    ASSERT_EQUALS(kMemoryUsageUpdateBatchSize + 1, handle.getCurrentMemoryUsageBytes());
    ASSERT_EQUALS(kMemoryUsageUpdateBatchSize + 1, local->getCurrentMemoryUsageBytes());
    ASSERT_EQUALS(2 * kMemoryUsageUpdateBatchSize, memoryAggregator->getCurrentMemoryUsageBytes());

    // Setting the memory usage back down to 1 should update the parent memory aggregator from 2x
    // the batch size to just 1x.
    handle.set(1);
    ASSERT_EQUALS(1, handle.getCurrentMemoryUsageBytes());
    ASSERT_EQUALS(1, local->getCurrentMemoryUsageBytes());
    ASSERT_EQUALS(kMemoryUsageUpdateBatchSize, memoryAggregator->getCurrentMemoryUsageBytes());

    // Setting the memory usage to zero should zero out the parent memory aggregator.
    handle.set(0);
    ASSERT_EQUALS(0, handle.getCurrentMemoryUsageBytes());
    ASSERT_EQUALS(0, local->getCurrentMemoryUsageBytes());
    ASSERT_EQUALS(0, memoryAggregator->getCurrentMemoryUsageBytes());
}

TEST_F(ConcurrentMemoryAggregatorTest, LowMemoryUsage) {
    auto memoryAggregator = std::make_shared<ConcurrentMemoryAggregator>();
    const auto& localMemoryAggregators = getChunkedMemoryAggregators(memoryAggregator.get());
    ASSERT_EQUALS(0, memoryAggregator->getCurrentMemoryUsageBytes());
    ASSERT_TRUE(localMemoryAggregators.empty());

    {
        // Create a local memory aggregator and make sure that it has been added to the parent
        // memory aggregator's registry map.
        auto local = memoryAggregator->createChunkedMemoryAggregator(kOptions);
        ASSERT_EQUALS(0, local->getCurrentMemoryUsageBytes());
        ASSERT_EQUALS(1, localMemoryAggregators.size());

        // Create a resource handle and make sure that usage changes are propagated upstream
        // to the local memory aggregator and the global memory aggregator.
        int64_t usageBytes = 1;
        auto handle = local->createUsageHandle();
        handle.set(usageBytes);
        ASSERT_EQUALS(usageBytes, handle.getCurrentMemoryUsageBytes());
        ASSERT_EQUALS(usageBytes, local->getCurrentMemoryUsageBytes());

        // Global parent memory aggregator should still get updated since we over count.
        ASSERT_EQUALS(kMemoryUsageUpdateBatchSize, memoryAggregator->getCurrentMemoryUsageBytes());
    }

    // After the local memory aggregator falls out of scope, it should have been removed from
    // the parent memory aggregator, and the parent memory aggregator should still be set to zero.
    ASSERT_TRUE(localMemoryAggregators.empty());
    ASSERT_EQUALS(0, memoryAggregator->getCurrentMemoryUsageBytes());
}

TEST_F(ConcurrentMemoryAggregatorTest, UsageMonitorCallback) {
    class MockUsageMonitor : public ConcurrentMemoryAggregator::UsageMonitor {
    public:
        void onMemoryUsageIncreased(int64_t memoryUsageBytes,
                                    int64_t sourceId,
                                    const ConcurrentMemoryAggregator* memoryAggregator) override {
            _invocations.push_back({memoryUsageBytes, sourceId, memoryAggregator});
        }

        auto getInvocations() {
            std::vector<std::tuple<int64_t, int64_t, const ConcurrentMemoryAggregator*>> out;
            std::swap(_invocations, out);
            return out;
        }

        std::vector<std::tuple<int64_t, int64_t, const ConcurrentMemoryAggregator*>> _invocations;
    };  // class MockUsageMonitor

    auto _mockUsageMonitor = std::make_shared<MockUsageMonitor>();
    MockUsageMonitor* mockUsageMonitor = _mockUsageMonitor.get();
    auto memoryAggregator =
        std::make_shared<ConcurrentMemoryAggregator>(std::move(_mockUsageMonitor));
    const auto& localMemoryAggregators = getChunkedMemoryAggregators(memoryAggregator.get());

    auto local = memoryAggregator->createChunkedMemoryAggregator(kOptions);
    ASSERT_EQUALS(1, localMemoryAggregators.size());

    auto handle = local->createUsageHandle();
    handle.set(kMemoryUsageUpdateBatchSize + 1);
    ASSERT_EQUALS(kMemoryUsageUpdateBatchSize + 1, handle.getCurrentMemoryUsageBytes());
    ASSERT_EQUALS(handle.getCurrentMemoryUsageBytes(), local->getCurrentMemoryUsageBytes());
    ASSERT_EQUALS(2 * kMemoryUsageUpdateBatchSize, memoryAggregator->getCurrentMemoryUsageBytes());

    auto invocations = mockUsageMonitor->getInvocations();
    ASSERT_EQUALS(1, invocations.size());
    auto invocation = invocations[0];
    ASSERT_EQUALS(std::get<0>(invocation), 2 * kMemoryUsageUpdateBatchSize);
    ASSERT_EQUALS(std::get<1>(invocation), local->getId());
    ASSERT_EQUALS(std::get<2>(invocation), memoryAggregator.get());

    // Increment memory usage by 1 byte, this should not propagate up local to the
    // global memory aggregator since it's an insignificant change, so the usage monitor
    // should not be invoked.
    handle.add(1);
    ASSERT_EQUALS(kMemoryUsageUpdateBatchSize + 2, handle.getCurrentMemoryUsageBytes());
    ASSERT_EQUALS(kMemoryUsageUpdateBatchSize + 2, local->getCurrentMemoryUsageBytes());
    ASSERT_EQUALS(2 * kMemoryUsageUpdateBatchSize, memoryAggregator->getCurrentMemoryUsageBytes());
    ASSERT_TRUE(mockUsageMonitor->getInvocations().empty());

    // Triggering `poll()` should always force the usage monitor callback to be invoked.
    local->poll();
    invocations = mockUsageMonitor->getInvocations();
    ASSERT_EQUALS(1, invocations.size());
    invocation = invocations[0];
    ASSERT_EQUALS(std::get<0>(invocation), 2 * kMemoryUsageUpdateBatchSize);
    ASSERT_EQUALS(std::get<1>(invocation), local->getId());
    ASSERT_EQUALS(std::get<2>(invocation), memoryAggregator.get());

    // Decrease memory by a significant amount, this should trigger an update to the parent memory
    // aggregator but not the usage monitor callback since the memory did not increase.
    handle.set(1);
    ASSERT_EQUALS(1, handle.getCurrentMemoryUsageBytes());
    ASSERT_EQUALS(1, local->getCurrentMemoryUsageBytes());
    ASSERT_EQUALS(kMemoryUsageUpdateBatchSize, memoryAggregator->getCurrentMemoryUsageBytes());
    ASSERT_TRUE(mockUsageMonitor->getInvocations().empty());
}

TEST_F(ConcurrentMemoryAggregatorTest, MultipleChunkedMemoryAggregators) {
    auto memoryAggregator = std::make_shared<ConcurrentMemoryAggregator>();
    const auto& localMemoryAggregators = getChunkedMemoryAggregators(memoryAggregator.get());
    ASSERT_EQUALS(0, memoryAggregator->getCurrentMemoryUsageBytes());
    ASSERT_TRUE(localMemoryAggregators.empty());

    auto local1 = memoryAggregator->createChunkedMemoryAggregator(kOptions);
    auto handle1 = local1->createUsageHandle();
    handle1.set(kMemoryUsageUpdateBatchSize);
    ASSERT_EQUALS(1, localMemoryAggregators.size());

    {
        auto local2 = memoryAggregator->createChunkedMemoryAggregator(kOptions);
        ASSERT_EQUALS(2, localMemoryAggregators.size());

        auto handle2 = local2->createUsageHandle();
        handle2.set(kMemoryUsageUpdateBatchSize + 1);

        // Verify both local memory aggregators have the correct states.
        ASSERT_EQUALS(kMemoryUsageUpdateBatchSize, handle1.getCurrentMemoryUsageBytes());
        ASSERT_EQUALS(kMemoryUsageUpdateBatchSize, local1->getCurrentMemoryUsageBytes());
        ASSERT_EQUALS(kMemoryUsageUpdateBatchSize + 1, handle2.getCurrentMemoryUsageBytes());
        ASSERT_EQUALS(kMemoryUsageUpdateBatchSize + 1, local2->getCurrentMemoryUsageBytes());

        // Global memory aggregator is only updated in the specified batch size, so it's not exact.
        ASSERT_EQUALS(3 * kMemoryUsageUpdateBatchSize,
                      memoryAggregator->getCurrentMemoryUsageBytes());

        // Decrease memory usage on the first SP resource handle and check
        // that the outcome state is correct.
        handle1.set(1);
        ASSERT_EQUALS(1, handle1.getCurrentMemoryUsageBytes());
        ASSERT_EQUALS(1, local1->getCurrentMemoryUsageBytes());
        ASSERT_EQUALS(kMemoryUsageUpdateBatchSize + 1, handle2.getCurrentMemoryUsageBytes());
        ASSERT_EQUALS(kMemoryUsageUpdateBatchSize + 1, local2->getCurrentMemoryUsageBytes());

        // Global memory aggregator is only updated in the specified batch size, so it's not exact.
        ASSERT_EQUALS(3 * kMemoryUsageUpdateBatchSize,
                      memoryAggregator->getCurrentMemoryUsageBytes());
    }

    // After SP2 is freed, the global memory aggregator should only reflect the memory usage of SP1.
    ASSERT_EQUALS(1, localMemoryAggregators.size());
    ASSERT_EQUALS(kMemoryUsageUpdateBatchSize, memoryAggregator->getCurrentMemoryUsageBytes());
}

TEST_F(ConcurrentMemoryAggregatorTest, ConcurrentChunkedMemoryAggregators) {
    static const int64_t kNumConcurrentChunkedMemoryAggregators = 10;
    static const int64_t kNumUpdates = 1'000'000;

    auto memoryAggregator = std::make_shared<ConcurrentMemoryAggregator>();
    const auto& localMemoryAggregators = getChunkedMemoryAggregators(memoryAggregator.get());
    ASSERT_EQUALS(0, memoryAggregator->getCurrentMemoryUsageBytes());
    ASSERT_TRUE(localMemoryAggregators.empty());

    stdx::mutex mutex;
    stdx::condition_variable completedCv;
    stdx::condition_variable shutdownCv;
    int numCompleted{0};
    std::vector<stdx::thread> threads;

    // Start up all concurrent local memory aggregators that propagate updates to the same global
    // memory aggregator.
    for (int i = 0; i < kNumConcurrentChunkedMemoryAggregators; ++i) {
        threads.emplace_back([&, i]() {
            auto local = memoryAggregator->createChunkedMemoryAggregator(kOptions);
            auto handle = local->createUsageHandle();
            for (int64_t j = kNumUpdates; j >= 1; --j) {
                bool neg = j % 2 != 0;
                int multiplier = neg ? -1 : 1;
                handle.add(multiplier * j * kMemoryUsageUpdateBatchSize);
            }

            stdx::unique_lock<stdx::mutex> lock(mutex);
            ++numCompleted;
            completedCv.notify_one();
            shutdownCv.wait(lock);
        });
    }

    {
        // Wait until all the concurrent local memory aggregators are done.
        stdx::unique_lock<stdx::mutex> lock(mutex);
        completedCv.wait(
            lock, [&]() -> bool { return numCompleted == kNumConcurrentChunkedMemoryAggregators; });
    }

    // Validate that the total memory usage in the global memory aggregator is correct.
    int64_t expectedMemoryUsageBytes =
        (kMemoryUsageUpdateBatchSize * kNumConcurrentChunkedMemoryAggregators * kNumUpdates) / 2;
    ASSERT_EQUALS(expectedMemoryUsageBytes, memoryAggregator->getCurrentMemoryUsageBytes());
    ASSERT_EQUALS(kNumConcurrentChunkedMemoryAggregators, localMemoryAggregators.size());

    shutdownCv.notify_all();
    for (auto& thd : threads) {
        thd.join();
    }
}

};  // namespace mongo
