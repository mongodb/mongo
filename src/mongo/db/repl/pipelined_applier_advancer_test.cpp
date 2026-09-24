// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/pipelined_applier_advancer.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/platform/waitable_atomic.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

#include <mutex>
#include <stdexcept>
#include <vector>

namespace mongo::repl {
namespace {

OpTimeAndWallTime batchTime(uint32_t timestamp) {
    return {OpTime(Timestamp(timestamp, 1), 1), Date_t::fromMillisSinceEpoch(timestamp * 1000)};
}

// Records publications from a real advancer thread while tests control batch completion.
class PipelinedApplierAdvancerTest : public ServiceContextTest {
protected:
    using InflightBatch = PipelinedApplierBatchTracker::InflightBatch;

    // Records the published batch and whether the client is authorized and exempt from stepdown
    // kills.
    void record(const InflightBatch& batch) {
        std::lock_guard lk(_mutex);
        _published.push_back(batch.lastOpTime);
        _onKillExemptClient = haveClient() && !cc().canKillOperationInStepdown() &&
            AuthorizationSession::get(cc())->isAuthenticated();
    }

    // Returns a synchronized snapshot of the publication sequence.
    std::vector<OpTimeAndWallTime> published() {
        std::lock_guard lk(_mutex);
        return _published;
    }

    std::mutex _mutex;
    std::vector<OpTimeAndWallTime> _published;
    bool _onKillExemptClient = false;
};

// Later completed batches must wait for every worker in the first batch before publication.
TEST_F(PipelinedApplierAdvancerTest, PublishesInFifoOrderAfterEveryWorkerCompletes) {
    PipelinedApplierBatchTracker tracker;
    auto first = tracker.addBatch(batchTime(1), 2);
    auto second = tracker.addBatch(batchTime(2), 1);
    // Synthetic batches preserve their optime order even when the wall clock moves backwards.
    auto thirdTime = batchTime(3);
    thirdTime.wallTime = Date_t::fromMillisSinceEpoch(1);
    tracker.addBatch(thirdTime, 0);
    // first has 1 remaining worker.
    tracker.onWorkerCompletion(first);
    // second has 0 remaining workers.
    tracker.onWorkerCompletion(second);

    PipelinedApplierAdvancer advancer(tracker, [&](const auto& batch) { record(batch); });
    ON_BLOCK_EXIT([&] {
        tracker.onWorkerAbandonment();
        advancer.shutdownAndJoin();
    });
    {
        FailPointEnableBlock beforeWait("hangBeforePipelinedApplierBatchWait");
        advancer.startup();
        // first is not complete, so the failpoint is entered without pulling first off the tracker.
        beforeWait->waitForTimesEntered(beforeWait.initialTimesEntered() + 1);
        ASSERT_TRUE(published().empty());
    }
    // This marks the first batch as complete, as it had two workers.
    tracker.onWorkerCompletion(first);
    advancer.shutdownAndJoin();

    // All three timestamps should be published when first was pulled off the queue.
    ASSERT_EQ(published(), (std::vector<OpTimeAndWallTime>{batchTime(1), batchTime(2), thirdTime}));
    ASSERT_TRUE(_onKillExemptClient);
}

// An incomplete middle batch must block later publication without delaying the completed earlier
// batches.
TEST_F(PipelinedApplierAdvancerTest, PublishesReadyBatchesThenStopsAtTheFirstIncompleteBatch) {
    PipelinedApplierBatchTracker tracker;
    tracker.addBatch(batchTime(1), 0);
    auto middle = tracker.addBatch(batchTime(2), 1);
    tracker.addBatch(batchTime(3), 0);
    PipelinedApplierAdvancer advancer(tracker, [&](const auto& batch) { record(batch); });
    ON_BLOCK_EXIT([&] {
        tracker.onWorkerAbandonment();
        advancer.shutdownAndJoin();
    });
    {
        FailPointEnableBlock beforeWait("hangBeforePipelinedApplierBatchWait");
        advancer.startup();
        beforeWait->waitForTimesEntered(beforeWait.initialTimesEntered() + 1);
        ASSERT_EQ(published(), (std::vector<OpTimeAndWallTime>{batchTime(1)}));
    }
    tracker.onWorkerCompletion(middle);
    advancer.shutdownAndJoin();
    ASSERT_EQ(published(),
              (std::vector<OpTimeAndWallTime>{batchTime(1), batchTime(2), batchTime(3)}));
}

// The timed wait must discover completed work even when no worker notification arrives.
TEST_F(PipelinedApplierAdvancerTest, BackstopPublishesCompletionWithoutNotification) {
    PipelinedApplierBatchTracker tracker;
    auto remaining = tracker.addBatch(batchTime(1), 1);
    WaitableAtomic<bool> publicationFinished{false};
    PipelinedApplierAdvancer advancer(tracker, [&](const auto& batch) {
        record(batch);
        publicationFinished.store(true);
        publicationFinished.notifyAll();
    });
    ON_BLOCK_EXIT([&] {
        tracker.onWorkerAbandonment();
        advancer.shutdownAndJoin();
    });
    {
        FailPointEnableBlock beforeWait("hangBeforePipelinedApplierBatchWait");
        advancer.startup();
        beforeWait->waitForTimesEntered(beforeWait.initialTimesEntered() + 1);
        // Simulate completion without its notification after the predicate observed an incomplete
        // batch.
        remaining->store(0, std::memory_order_release);
    }
    // Even without the signal from the tracker itself, the backstop sees the completed batch.
    ASSERT_EQ(publicationFinished.waitFor(false, Seconds(10)), boost::optional<bool>{true});
    advancer.shutdownAndJoin();
    ASSERT_EQ(published(), (std::vector<OpTimeAndWallTime>{batchTime(1)}));
}

// A blocked publication in the advancer must allow new batches to be added to the tracker, but
// prevent shutdown from finishing.
TEST_F(PipelinedApplierAdvancerTest, PublicationDoesNotHoldTrackerMutexAndShutdownWaitsForIt) {
    PipelinedApplierBatchTracker tracker;
    WaitableAtomic<bool> publicationStarted{false};
    WaitableAtomic<bool> releasePublication{false};
    PipelinedApplierAdvancer advancer(tracker, [&](const auto& batch) {
        if (batch.lastOpTime == batchTime(1)) {
            // Tell the test the first batch was popped, then block until it allows publication.
            publicationStarted.store(true);
            publicationStarted.notifyAll();
            releasePublication.wait(false);
        }
        record(batch);
    });
    WaitableAtomic<bool> shutdownFinished{false};
    stdx::thread shutdownThread;
    ON_BLOCK_EXIT([&] {
        // Keep the release set even if cleanup runs before the callback starts.
        releasePublication.store(true);
        releasePublication.notifyAll();
        tracker.onWorkerAbandonment();
        if (shutdownThread.joinable()) {
            shutdownThread.join();
        }
        advancer.shutdownAndJoin();
    });
    tracker.addBatch(batchTime(1), 0);
    advancer.startup();
    // The advancer callback has started, but the first batch cannot finish publishing until we
    // release it.
    ASSERT_EQ(publicationStarted.waitFor(false, Seconds(10)), boost::optional<bool>{true});

    // Both calls take the tracker mutex, so completing them proves publication does not hold it
    // while it is blocked.
    auto next = tracker.addBatch(batchTime(2), 1);
    tracker.onWorkerCompletion(next);
    {
        FailPointEnableBlock beforeWait("hangBeforePipelinedApplierIdleWait");
        // Run shutdown separately because it must wait for the callback this thread will release.
        shutdownThread = stdx::thread([&] {
            advancer.shutdownAndJoin();
            shutdownFinished.store(true);
            shutdownFinished.notifyAll();
        });
        beforeWait->waitForTimesEntered(beforeWait.initialTimesEntered() + 1);
        // The failpoint confirms shutdown reached its idle wait, not just that it was scheduled.
        ASSERT_FALSE(shutdownFinished.load());
    }
    // Let the first publication finish so the advancer can publish the second batch and shut down.
    releasePublication.store(true);
    releasePublication.notifyAll();
    ASSERT_EQ(shutdownFinished.waitFor(false, Seconds(10)), boost::optional<bool>{true});
    ASSERT_EQ(published(), (std::vector<OpTimeAndWallTime>{batchTime(1), batchTime(2)}));
}

// Abandonment must unblock shutdown without making incomplete work or its successors publishable.
TEST_F(PipelinedApplierAdvancerTest, AbandonmentStopsShutdownWithoutPublishingPastTheHole) {
    PipelinedApplierBatchTracker tracker;
    auto abandoned = tracker.addBatch(batchTime(1), 1);
    tracker.addBatch(batchTime(2), 0);
    PipelinedApplierAdvancer advancer(tracker, [&](const auto& batch) { record(batch); });
    WaitableAtomic<bool> shutdownFinished{false};
    stdx::thread shutdownThread;
    ON_BLOCK_EXIT([&] {
        tracker.onWorkerAbandonment();
        if (shutdownThread.joinable()) {
            shutdownThread.join();
        }
        advancer.shutdownAndJoin();
    });
    advancer.startup();
    {
        FailPointEnableBlock beforeWait("hangBeforePipelinedApplierIdleWait");
        shutdownThread = stdx::thread([&] {
            advancer.shutdownAndJoin();
            shutdownFinished.store(true);
            shutdownFinished.notifyAll();
        });
        beforeWait->waitForTimesEntered(beforeWait.initialTimesEntered() + 1);
    }
    tracker.onWorkerAbandonment();
    ASSERT_EQ(shutdownFinished.waitFor(false, Seconds(10)), boost::optional<bool>{true});
    ASSERT_EQ(abandoned->load(), 1);
    // The two batches were abandoned and not published.
    ASSERT_TRUE(published().empty());
}

// Abandoning a later batch must not let shutdown return while an earlier publication is running.
TEST_F(PipelinedApplierAdvancerTest, AbandonmentDuringPublicationStillJoinsThePublisher) {
    PipelinedApplierBatchTracker tracker;
    tracker.addBatch(batchTime(1), 0);
    auto abandoned = tracker.addBatch(batchTime(2), 1);
    tracker.addBatch(batchTime(3), 0);
    WaitableAtomic<bool> publicationStarted{false};
    WaitableAtomic<bool> releasePublication{false};
    PipelinedApplierAdvancer advancer(tracker, [&](const auto& batch) {
        publicationStarted.store(true);
        publicationStarted.notifyAll();
        releasePublication.wait(false);
        record(batch);
    });
    WaitableAtomic<bool> shutdownFinished{false};
    stdx::thread shutdownThread;
    ON_BLOCK_EXIT([&] {
        // Keep the release set even if cleanup runs before the callback starts.
        releasePublication.store(true);
        releasePublication.notifyAll();
        tracker.onWorkerAbandonment();
        if (shutdownThread.joinable()) {
            shutdownThread.join();
        }
        advancer.shutdownAndJoin();
    });
    advancer.startup();
    ASSERT_EQ(publicationStarted.waitFor(false, Seconds(10)), boost::optional<bool>{true});

    // One worker abandons the later batch while the first batch is still publishing.
    tracker.onWorkerAbandonment();
    shutdownThread = stdx::thread([&] {
        advancer.shutdownAndJoin();
        shutdownFinished.store(true);
        shutdownFinished.notifyAll();
    });
    const auto deadline = Date_t::now() + Seconds(10);
    while (!tracker.isShutdown()) {
        ASSERT_LT(Date_t::now(), deadline);
        sleepmillis(1);
    }
    // Abandonment ends the idle wait in the shutdown thread, but joining must still wait for the
    // active callback that is processing the first batch.
    ASSERT_FALSE(shutdownFinished.load());
    releasePublication.store(true);
    releasePublication.notifyAll();
    ASSERT_EQ(shutdownFinished.waitFor(false, Seconds(10)), boost::optional<bool>{true});
    ASSERT_EQ(abandoned->load(), 1);
    ASSERT_EQ(published(), (std::vector<OpTimeAndWallTime>{batchTime(1)}));
}

// An empty pipeline must shut down without work, and shutdown must be idempotent.
TEST_F(PipelinedApplierAdvancerTest, StartsAndStopsWithoutAnyBatches) {
    PipelinedApplierBatchTracker tracker;
    PipelinedApplierAdvancer advancer(tracker, [&](const auto& batch) { record(batch); });
    advancer.startup();
    advancer.shutdownAndJoin();
    advancer.shutdownAndJoin();
    ASSERT_TRUE(published().empty());
}

using PipelinedApplierAdvancerDeathTest = PipelinedApplierAdvancerTest;

// A repeated optime must terminate the process rather than be published twice.
DEATH_TEST_REGEX_F(PipelinedApplierAdvancerDeathTest,
                   InvariantsOnNonMonotonicPublication,
                   "Pipelined oplog applier publication must advance in oplog order") {
    PipelinedApplierBatchTracker tracker;
    tracker.addBatch(batchTime(1), 0);
    tracker.addBatch(batchTime(1), 0);
    PipelinedApplierAdvancer advancer(tracker, [&](const auto& batch) { record(batch); });
    advancer.startup();
    advancer.shutdownAndJoin();
}

// A database exception during publication must be fatal rather than silently stop the advancer.
DEATH_TEST_REGEX_F(PipelinedApplierAdvancerDeathTest,
                   FassertsOnPublicationDBException,
                   "13469704.*injected publication failure") {
    PipelinedApplierBatchTracker tracker;
    tracker.addBatch(batchTime(1), 0);
    PipelinedApplierAdvancer advancer(tracker, [](const auto&) {
        uasserted(ErrorCodes::OperationFailed, "injected publication failure");
    });
    advancer.startup();
    advancer.shutdownAndJoin();
}

// Standard exceptions must also terminate the process.
DEATH_TEST_REGEX_F(PipelinedApplierAdvancerDeathTest,
                   FassertsOnPublicationStdException,
                   "13469705.*injected publication failure") {
    PipelinedApplierBatchTracker tracker;
    tracker.addBatch(batchTime(1), 0);
    PipelinedApplierAdvancer advancer(
        tracker, [](const auto&) { throw std::runtime_error("injected publication failure"); });
    advancer.startup();
    advancer.shutdownAndJoin();
}

}  // namespace
}  // namespace mongo::repl
