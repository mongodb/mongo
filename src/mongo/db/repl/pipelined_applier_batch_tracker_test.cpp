// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/pipelined_applier_batch_tracker.h"

#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/platform/waitable_atomic.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/timer.h"

#include <future>
#include <limits>

namespace mongo::repl {
namespace {

OpTimeAndWallTime batchTime(uint32_t timestamp) {
    return {OpTime(Timestamp(timestamp, 1), 1), Date_t::fromMillisSinceEpoch(timestamp * 1000)};
}

// RAII helper to override the pipelined applier server parameters for a scope, restoring the
// previous values on destruction so tests don't leak state to one another.
class ScopedBackpressureParams {
public:
    ScopedBackpressureParams(long long maxBatches, long long maxBytes)
        : _prevMaxBatches(pipelinedApplierMaxInflightBatches.load()),
          _prevMaxBytes(pipelinedApplierMaxInflightBytes.load()) {
        pipelinedApplierMaxInflightBatches.store(maxBatches);
        pipelinedApplierMaxInflightBytes.store(maxBytes);
    }

    ~ScopedBackpressureParams() {
        pipelinedApplierMaxInflightBatches.store(_prevMaxBatches);
        pipelinedApplierMaxInflightBytes.store(_prevMaxBytes);
    }

private:
    long long _prevMaxBatches;
    long long _prevMaxBytes;
};

TEST(PipelinedApplierBatchTrackerTest, WaitTimesOutWithoutACompletedFrontBatch) {
    PipelinedApplierBatchTracker tracker;
    const auto maxWaitTime = Milliseconds(100);
    Timer timer;
    ASSERT_FALSE(tracker.popCompletedBatch(maxWaitTime));
    ASSERT_GTE(timer.elapsed(), maxWaitTime);

    tracker.addBatch(batchTime(1), 1, {}, {}, nullptr, 0);
    timer.reset();
    ASSERT_FALSE(tracker.popCompletedBatch(maxWaitTime));
    ASSERT_GTE(timer.elapsed(), maxWaitTime);
}

TEST(PipelinedApplierBatchTrackerTest, CountsParticipatingWorkersAndPopsInDispatchOrder) {
    PipelinedApplierBatchTracker tracker;
    auto first = tracker.addBatch(batchTime(1), /*numWorkers=*/2, {}, {}, nullptr, 0);
    auto second = tracker.addBatch(batchTime(2), /*numWorkers=*/1, {}, {}, nullptr, 0);
    // Ensure the pointers to the atomic counters are different.
    ASSERT_NE(first, second);
    ASSERT_EQ(first->load(), 2);
    ASSERT_EQ(second->load(), 1);

    // One thread finishes both batches while another is still applying its slice of the first.
    // Ensure the shared counter reflects that.
    tracker.onWorkerCompletion(first);
    tracker.onWorkerCompletion(second);
    ASSERT_EQ(first->load(), 1);
    ASSERT_EQ(second->load(), 0);
    ASSERT_FALSE(tracker.popCompletedBatch(Milliseconds(0)));

    tracker.onWorkerCompletion(first);
    auto completed = tracker.popCompletedBatch(Milliseconds(0));
    ASSERT_TRUE(completed);
    ASSERT_EQ(completed->lastOpTime, batchTime(1));
    ASSERT_EQ(completed->remainingWorkers, first);
    tracker.onBatchPublished();
    completed = tracker.popCompletedBatch(Milliseconds(0));
    ASSERT_TRUE(completed);
    ASSERT_EQ(completed->lastOpTime, batchTime(2));
    ASSERT_EQ(completed->remainingWorkers, second);
    tracker.onBatchPublished();
    ASSERT_FALSE(tracker.popCompletedBatch(Milliseconds(0)));
}

TEST(PipelinedApplierBatchTrackerTest, ZeroWorkerBatchWaitsForEarlierBatch) {
    PipelinedApplierBatchTracker tracker;
    auto first = tracker.addBatch(batchTime(1), 1, {}, {}, nullptr, 0);
    auto empty = tracker.addBatch(batchTime(2), 0, {}, {}, nullptr, 0);
    ASSERT_EQ(empty->load(), 0);
    ASSERT_FALSE(tracker.popCompletedBatch(Milliseconds(0)));

    tracker.onWorkerCompletion(first);
    ASSERT_TRUE(tracker.popCompletedBatch(Milliseconds(0)));
    tracker.onBatchPublished();
    auto completed = tracker.popCompletedBatch(Milliseconds(0));
    ASSERT_TRUE(completed);
    ASSERT_EQ(completed->lastOpTime, batchTime(2));
    ASSERT_EQ(completed->remainingWorkers, empty);
    tracker.onBatchPublished();
}

TEST(PipelinedApplierBatchTrackerTest, CompletionBeforeWaitIsNotLost) {
    PipelinedApplierBatchTracker tracker;
    auto counter = tracker.addBatch(batchTime(1), 1, {}, {}, nullptr, 0);
    tracker.onWorkerCompletion(counter);
    auto completed = tracker.popCompletedBatch(Milliseconds(0));
    ASSERT_TRUE(completed);
    ASSERT_EQ(completed->lastOpTime, batchTime(1));
    tracker.onBatchPublished();
}

// The last worker to finish a batch wakes the waiting consumer before its timeout expires.
TEST(PipelinedApplierBatchTrackerTest, LastWorkerWakesConsumer) {
    PipelinedApplierBatchTracker tracker;
    auto counter = tracker.addBatch(batchTime(1), 2, {}, {}, nullptr, 0);
    tracker.onWorkerCompletion(counter);
    std::promise<boost::optional<PipelinedApplierBatchTracker::InflightBatch>> promise;
    auto result = promise.get_future();
    stdx::thread consumer;
    ON_BLOCK_EXIT([&] {
        if (consumer.joinable()) {
            consumer.join();
        }
    });

    {
        FailPointEnableBlock beforeWait("hangBeforePipelinedApplierBatchWait");
        consumer = stdx::thread([&] { promise.set_value(tracker.popCompletedBatch(Seconds(60))); });
        beforeWait.wait();
    }
    // The consumer holds the mutex with a false predicate until it enters the wait.
    tracker.onWorkerCompletion(counter);
    // Completion must wake the consumer well before its fallback timeout.
    ASSERT_EQ(result.wait_for(Seconds(10).toSystemDuration()), std::future_status::ready);
    auto completed = result.get();
    ASSERT_TRUE(completed);
    ASSERT_EQ(completed->lastOpTime, batchTime(1));
    tracker.onBatchPublished();
}

TEST(PipelinedApplierBatchTrackerTest, EnqueuingZeroWorkerBatchWakesConsumer) {
    PipelinedApplierBatchTracker tracker;
    std::promise<boost::optional<PipelinedApplierBatchTracker::InflightBatch>> promise;
    auto result = promise.get_future();
    stdx::thread consumer;
    ON_BLOCK_EXIT([&] {
        if (consumer.joinable()) {
            consumer.join();
        }
    });

    {
        FailPointEnableBlock beforeWait("hangBeforePipelinedApplierBatchWait");
        consumer = stdx::thread([&] { promise.set_value(tracker.popCompletedBatch(Seconds(60))); });
        beforeWait.wait();
    }
    tracker.addBatch(batchTime(1), 0, {}, {}, nullptr, 0);
    ASSERT_EQ(result.wait_for(Seconds(10).toSystemDuration()), std::future_status::ready);
    auto completed = result.get();
    ASSERT_TRUE(completed);
    ASSERT_EQ(completed->lastOpTime, batchTime(1));
    tracker.onBatchPublished();
}

TEST(PipelinedApplierBatchTrackerTest, EmptyQueueRemainsBusyUntilPublicationFinishes) {
    PipelinedApplierBatchTracker tracker;
    tracker.addBatch(batchTime(1), 0, {}, {}, nullptr, 0);
    ASSERT_TRUE(tracker.popCompletedBatch(Milliseconds(0)));
    std::promise<bool> promise;
    auto idle = promise.get_future();
    stdx::thread waiter;
    bool published = false;
    ON_BLOCK_EXIT([&] {
        if (!published) {
            tracker.onBatchPublished();
        }
        if (waiter.joinable()) {
            waiter.join();
        }
    });
    {
        FailPointEnableBlock beforeWait("hangBeforePipelinedApplierIdleWait");
        waiter = stdx::thread([&] { promise.set_value(tracker.waitUntilIdle()); });
        beforeWait.wait();
        // The only batch is already popped, but the waiter must still see the advancer as busy.
        ASSERT_EQ(idle.wait_for(Milliseconds(0).toSystemDuration()), std::future_status::timeout);
    }
    tracker.onBatchPublished();
    published = true;
    ASSERT_EQ(idle.wait_for(Seconds(10).toSystemDuration()), std::future_status::ready);
    ASSERT_TRUE(idle.get());
}

TEST(PipelinedApplierBatchTrackerTest, AbandonedBatchWakesIdleWaitWithoutCompletingTheBatch) {
    PipelinedApplierBatchTracker tracker;
    auto remaining = tracker.addBatch(batchTime(1), 1, {}, {}, nullptr, 0);
    std::promise<bool> promise;
    auto idle = promise.get_future();
    stdx::thread waiter;
    ON_BLOCK_EXIT([&] {
        tracker.onWorkerAbandonment();
        if (waiter.joinable()) {
            waiter.join();
        }
    });
    {
        FailPointEnableBlock beforeWait("hangBeforePipelinedApplierIdleWait");
        waiter = stdx::thread([&] { promise.set_value(tracker.waitUntilIdle()); });
        beforeWait.wait();
    }
    tracker.onWorkerAbandonment();
    ASSERT_EQ(idle.wait_for(Seconds(10).toSystemDuration()), std::future_status::ready);
    ASSERT_FALSE(idle.get());
    ASSERT_EQ(remaining->load(), 1);
    ASSERT_FALSE(tracker.popCompletedBatch(Milliseconds(0)));
}

TEST(PipelinedApplierBatchTrackerTest, ShutdownWakesConsumerWithoutPoppingWork) {
    PipelinedApplierBatchTracker tracker;
    tracker.addBatch(batchTime(1), 1, {}, {}, nullptr, 0);
    std::promise<boost::optional<PipelinedApplierBatchTracker::InflightBatch>> promise;
    auto result = promise.get_future();
    stdx::thread consumer;
    ON_BLOCK_EXIT([&] {
        tracker.shutdown();
        if (consumer.joinable()) {
            consumer.join();
        }
    });
    {
        FailPointEnableBlock beforeWait("hangBeforePipelinedApplierBatchWait");
        consumer = stdx::thread([&] { promise.set_value(tracker.popCompletedBatch(Seconds(60))); });
        beforeWait.wait();
    }
    tracker.shutdown();
    ASSERT_EQ(result.wait_for(Seconds(10).toSystemDuration()), std::future_status::ready);
    ASSERT_FALSE(result.get());
    ASSERT_TRUE(tracker.isShutdown());
}

TEST(PipelinedApplierBatchTrackerTest, BackpressureNecessaryReportsCorrectly) {
    ASSERT_FALSE(PipelinedApplierBatchTracker::backpressureNecessary(
        0 /* batchCount */, 0 /* byteCount */, 10 /* maxBatches */, 100 /* maxBytes */));
    ASSERT_FALSE(PipelinedApplierBatchTracker::backpressureNecessary(
        10 /* batchCount */, 0 /* byteCount */, 10 /* maxBatches */, 100 /* maxBytes */));
    ASSERT_FALSE(PipelinedApplierBatchTracker::backpressureNecessary(
        0 /* batchCount */, 100 /* byteCount */, 10 /* maxBatches */, 100 /* maxBytes */));
    ASSERT_FALSE(PipelinedApplierBatchTracker::backpressureNecessary(
        10 /* batchCount */, 100 /* byteCount */, 10 /* maxBatches */, 100 /* maxBytes */));

    {
        auto reason = PipelinedApplierBatchTracker::backpressureNecessary(
            12 /* batchCount */, 100 /* byteCount */, 10 /* maxBatches */, 100 /* maxBytes */);
        ASSERT_TRUE(reason);
        ASSERT_EQ(reason->batchesOverThreshold, 2);
        ASSERT_EQ(reason->bytesOverThreshold, 0);
    }
    {
        auto reason = PipelinedApplierBatchTracker::backpressureNecessary(
            5 /* batchCount */, 116 /* byteCount */, 10 /* maxBatches */, 100 /* maxBytes */);
        ASSERT_TRUE(reason);
        ASSERT_EQ(reason->batchesOverThreshold, 0);
        ASSERT_EQ(reason->bytesOverThreshold, 16);
    }
    {
        auto reason = PipelinedApplierBatchTracker::backpressureNecessary(
            12 /* batchCount */, 116 /* byteCount */, 10 /* maxBatches */, 100 /* maxBytes */);
        ASSERT_TRUE(reason);
        ASSERT_EQ(reason->batchesOverThreshold, 2);
        ASSERT_EQ(reason->bytesOverThreshold, 16);
    }
}

TEST(PipelinedApplierBatchTrackerTest, AddBatchBlocksAndPoppingUnblocks) {
    constexpr long long perBatchBytes = 300;

    auto run = [&](std::string_view scenario, long long maxBatches, long long maxBytes) {
        ScopedBackpressureParams params(maxBatches, maxBytes);
        PipelinedApplierBatchTracker tracker;

        // Under limit
        tracker.addBatch(batchTime(1), 0 /* numWorkers */, {}, {}, nullptr, perBatchBytes);
        tracker.addBatch(batchTime(2), 0 /* numWorkers */, {}, {}, nullptr, perBatchBytes);

        // On limit
        tracker.addBatch(batchTime(3), 0 /* numWorkers */, {}, {}, nullptr, perBatchBytes);

        // Over limit, will block on next push
        tracker.addBatch(batchTime(4), 0 /* numWorkers */, {}, {}, nullptr, perBatchBytes);

        std::shared_ptr<std::atomic<uint32_t>> fifth;
        WaitableAtomic<bool> done{false};
        unittest::JoinThread dispatch;
        {
            FailPointEnableBlock beforeCapacityWait("hangBeforePipelinedApplierCapacityWait");
            // Blocks, due to limit
            dispatch = unittest::JoinThread{[&] {
                fifth = tracker.addBatch(batchTime(5), 0, {}, {}, nullptr, perBatchBytes);
                done.store(true);
                done.notifyAll();
            }};
            beforeCapacityWait->waitForTimesEntered(beforeCapacityWait.initialTimesEntered() + 1);
        }
        ASSERT_FALSE(done.waitFor(false, Milliseconds(200))) << scenario;

        // Back on limit, unblocks
        ASSERT_TRUE(tracker.popCompletedBatch(Seconds(10))) << scenario;
        tracker.onBatchPublished();
        ASSERT_TRUE(done.waitFor(false, Milliseconds(200))) << scenario;

        // Drain the remaining batches to ensure there are now 4 items in the tracker's queue
        for (size_t i = 0; i < 4; ++i) {
            ASSERT_TRUE(tracker.popCompletedBatch(Milliseconds(200))) << scenario;
            tracker.onBatchPublished();
        }
        ASSERT_FALSE(tracker.popCompletedBatch(Milliseconds(200))) << scenario;
    };

    // Tight batch cap, no byte cap: trips on batch count.
    run("batches", 3, std::numeric_limits<long long>::max());

    // No batch cap, tight byte cap: trips on bytes.
    run("bytes", std::numeric_limits<long long>::max(), 3 * perBatchBytes);

    // Both caps tight: must re-check both dimensions on wakeup.
    run("both", 3, 3 * perBatchBytes);
}

// A shutdown while a dispatcher is blocked on capacity must wake it and cause addBatch to return
// nullptr rather than a live counter.
TEST(PipelinedApplierBatchTrackerTest, ShutdownWakesDispatcherBlockedOnCapacity) {
    ScopedBackpressureParams params(1 /* maxBatches */,
                                    std::numeric_limits<long long>::max() /* maxBytes */);
    PipelinedApplierBatchTracker tracker;

    // Add one batch to hit the threshold, and another batch to push us over the threshold.
    auto first =
        tracker.addBatch(batchTime(1), 0 /* numWorkers */, {}, {}, nullptr, 0 /* batchBytes */);
    auto second =
        tracker.addBatch(batchTime(2), 0 /* numWorkers */, {}, {}, nullptr, 0 /* batchBytes */);

    WaitableAtomic<bool> done{false};
    std::shared_ptr<std::atomic<uint32_t>> result;
    unittest::JoinThread dispatcher;
    {
        FailPointEnableBlock beforeCapacityWait("hangBeforePipelinedApplierCapacityWait");
        dispatcher = unittest::JoinThread{[&] {
            result = tracker.addBatch(batchTime(3), 1, {}, {}, nullptr, 0 /* batchBytes */);
            done.store(true);
            done.notifyAll();
        }};
        beforeCapacityWait->waitForTimesEntered(beforeCapacityWait.initialTimesEntered() + 1);
    }
    ASSERT_FALSE(done.waitFor(false, Milliseconds(200)));

    tracker.shutdown();
    ASSERT_TRUE(done.waitFor(false, Seconds(10)));
    ASSERT_EQ(result, nullptr);
    ASSERT_TRUE(tracker.isShutdown());

    tracker.onWorkerAbandonment();
}

}  // namespace
}  // namespace mongo::repl
