// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/pipelined_applier_batch_tracker.h"

#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/timer.h"

#include <future>

namespace mongo::repl {
namespace {

OpTimeAndWallTime batchTime(uint32_t timestamp) {
    return {OpTime(Timestamp(timestamp, 1), 1), Date_t::fromMillisSinceEpoch(timestamp * 1000)};
}

TEST(PipelinedApplierBatchTrackerTest, WaitTimesOutWithoutACompletedFrontBatch) {
    PipelinedApplierBatchTracker tracker;
    const auto maxWaitTime = Milliseconds(100);
    Timer timer;
    ASSERT_FALSE(tracker.popCompletedBatch(maxWaitTime));
    ASSERT_GTE(timer.elapsed(), maxWaitTime);

    tracker.addBatch(batchTime(1), 1);
    timer.reset();
    ASSERT_FALSE(tracker.popCompletedBatch(maxWaitTime));
    ASSERT_GTE(timer.elapsed(), maxWaitTime);
}

TEST(PipelinedApplierBatchTrackerTest, CountsParticipatingWorkersAndPopsInDispatchOrder) {
    PipelinedApplierBatchTracker tracker;
    auto first = tracker.addBatch(batchTime(1), 2 /* remainingWorkers */);
    auto second = tracker.addBatch(batchTime(2), 1 /* remainingWorkers */);
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
    auto first = tracker.addBatch(batchTime(1), 1);
    auto empty = tracker.addBatch(batchTime(2), 0);
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
    auto counter = tracker.addBatch(batchTime(1), 1);
    tracker.onWorkerCompletion(counter);
    auto completed = tracker.popCompletedBatch(Milliseconds(0));
    ASSERT_TRUE(completed);
    ASSERT_EQ(completed->lastOpTime, batchTime(1));
    tracker.onBatchPublished();
}

// The last worker to finish a batch wakes the waiting consumer before its timeout expires.
TEST(PipelinedApplierBatchTrackerTest, LastWorkerWakesConsumer) {
    PipelinedApplierBatchTracker tracker;
    auto counter = tracker.addBatch(batchTime(1), 2);
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
        beforeWait->waitForTimesEntered(beforeWait.initialTimesEntered() + 1);
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
        beforeWait->waitForTimesEntered(beforeWait.initialTimesEntered() + 1);
    }
    tracker.addBatch(batchTime(1), 0);
    ASSERT_EQ(result.wait_for(Seconds(10).toSystemDuration()), std::future_status::ready);
    auto completed = result.get();
    ASSERT_TRUE(completed);
    ASSERT_EQ(completed->lastOpTime, batchTime(1));
    tracker.onBatchPublished();
}

TEST(PipelinedApplierBatchTrackerTest, EmptyQueueRemainsBusyUntilPublicationFinishes) {
    PipelinedApplierBatchTracker tracker;
    tracker.addBatch(batchTime(1), 0);
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
        beforeWait->waitForTimesEntered(beforeWait.initialTimesEntered() + 1);
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
    auto remaining = tracker.addBatch(batchTime(1), 1);
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
        beforeWait->waitForTimesEntered(beforeWait.initialTimesEntered() + 1);
    }
    tracker.onWorkerAbandonment();
    ASSERT_EQ(idle.wait_for(Seconds(10).toSystemDuration()), std::future_status::ready);
    ASSERT_FALSE(idle.get());
    ASSERT_EQ(remaining->load(), 1);
    ASSERT_FALSE(tracker.popCompletedBatch(Milliseconds(0)));
}

TEST(PipelinedApplierBatchTrackerTest, ShutdownWakesConsumerWithoutPoppingWork) {
    PipelinedApplierBatchTracker tracker;
    tracker.addBatch(batchTime(1), 1);
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
        beforeWait->waitForTimesEntered(beforeWait.initialTimesEntered() + 1);
    }
    tracker.shutdown();
    ASSERT_EQ(result.wait_for(Seconds(10).toSystemDuration()), std::future_status::ready);
    ASSERT_FALSE(result.get());
    ASSERT_TRUE(tracker.isShutdown());
}

}  // namespace
}  // namespace mongo::repl
