// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/pipelined_applier_worker_pool.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/oplog_applier_batcher_test_fixture.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/scopeguard.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace mongo::repl {
namespace {

class PipelinedApplierWorkerPoolTest : public ServiceContextTest {
protected:
    using WorkItem = PipelinedApplierWorkerPool::WorkItem;

    // One consumption record per consumed op, in the order the worker threads consumed them.
    // Ops are identified by their unique timestamps, as returned by enqueueItem().
    struct Consumed {
        size_t workerIdx;
        Timestamp opTs;
        // Whether the consuming thread had an internally authorized Client that stepdown cannot
        // kill.
        bool onKillExemptClient;
    };

    // Makes a pool whose consume function records each consumed op into _consumed and then,
    // if blockWorkers() is in effect, blocks until releaseWorkers() is called.
    std::unique_ptr<PipelinedApplierWorkerPool> makePool(size_t numWorkers) {
        return std::make_unique<PipelinedApplierWorkerPool>(
            numWorkers, [this](size_t workerIdx, const WorkItem& item) {
                _recordConsumed(workerIdx, item);
                std::unique_lock lk(_gateMutex);
                _gateCv.wait(lk, [&] { return !_blocking; });
            });
    }

    // Makes workers block after recording each work item, until releaseWorkers() is called.
    // Used to hold workers mid-consumption. Call before enqueueing.
    void blockWorkers() {
        std::lock_guard lk(_gateMutex);
        _blocking = true;
    }

    // Unblocks all held workers, permanently.
    void releaseWorkers() {
        std::lock_guard lk(_gateMutex);
        _blocking = false;
        _gateCv.notify_all();
    }

    // RAII type to block workers while in-scope and release workers upon destruction.
    // Optionally takes a pointer to a PipelinedApplierWorkerPool to shutdown and join after the
    // workers are released.
    struct ScopedBlockedWorkers {
        ScopedBlockedWorkers(PipelinedApplierWorkerPoolTest* test,
                             PipelinedApplierWorkerPool* poolToShutdown = nullptr)
            : _test(test), _poolToShutdown(poolToShutdown) {
            _test->blockWorkers();
        }
        ~ScopedBlockedWorkers() {
            _test->releaseWorkers();
            if (_poolToShutdown) {
                _poolToShutdown->shutdownAndJoin();
            }
        }
        PipelinedApplierWorkerPoolTest* const _test;
        PipelinedApplierWorkerPool* const _poolToShutdown;
    };

    // Blocks until at least n ops have been recorded as consumed.
    void waitForConsumedCount(size_t n) {
        std::unique_lock lk(_mutex);
        _consumedCv.wait(lk, [&] { return _consumed.size() >= n; });
    }

    // Enqueues a work item holding one op with a unique timestamp to the given worker and
    // returns that timestamp for matching against consumption records.
    Timestamp enqueueItem(PipelinedApplierWorkerPool& pool, size_t workerIdx) {
        WorkItem item;
        item.ops.push_back(repl::makeInsertOplogEntry(
            _nextSeq++, NamespaceString::createNamespaceString_forTest("test.poa_workers")));
        const Timestamp ts = item.ops.front().getTimestamp();
        pool.enqueue(workerIdx, std::move(item));
        return ts;
    }

    size_t consumedCount() {
        std::lock_guard lk(_mutex);
        return _consumed.size();
    }

    bool allConsumedOnKillExemptClients() {
        std::lock_guard lk(_mutex);
        return std::all_of(_consumed.begin(), _consumed.end(), [](const Consumed& c) {
            return c.onKillExemptClient;
        });
    }

    // Asserts the given worker consumed exactly `expected`, in order.
    void assertConsumedInOrder(size_t workerIdx, const std::vector<Timestamp>& expected) {
        auto actual = consumedByWorker(workerIdx);
        ASSERT_EQ(actual.size(), expected.size()) << "worker " << workerIdx;
        for (size_t i = 0; i < expected.size(); ++i) {
            ASSERT_EQ(actual[i], expected[i]) << "worker " << workerIdx << " item " << i;
        }
    }

    // Retrieve all the operations consumed by a particular `workerIdx`.
    std::vector<Timestamp> consumedByWorker(size_t workerIdx) {
        std::lock_guard lk(_mutex);
        std::vector<Timestamp> out;
        for (const auto& c : _consumed) {
            if (c.workerIdx == workerIdx) {
                out.push_back(c.opTs);
            }
        }
        return out;
    }

private:
    void _recordConsumed(size_t workerIdx, const WorkItem& item) {
        const bool onKillExemptClient = haveClient() && !cc().canKillOperationInStepdown() &&
            AuthorizationSession::get(cc())->isAuthenticated();
        std::lock_guard lk(_mutex);
        for (const auto& op : item.ops) {
            _consumed.push_back({workerIdx, op.getTimestamp(), onKillExemptClient});
        }
        // If the test is blocking on n items to be consumed, unblock.
        _consumedCv.notify_all();
    }

    std::mutex _mutex;
    // Used to signal when a certain number of operations have been consumed.
    stdx::condition_variable _consumedCv;
    std::vector<Consumed> _consumed;  // (M)
    int _nextSeq = 1;

    std::mutex _gateMutex;
    stdx::condition_variable _gateCv;
    bool _blocking = false;  // (_gateMutex)
};

TEST_F(PipelinedApplierWorkerPoolTest, SingleWorkerConsumesInFifoOrder) {
    auto pool = makePool(1 /* numWorkers */);
    std::vector<Timestamp> enqueued;
    for (int i = 0; i < 100; ++i) {
        enqueued.push_back(enqueueItem(*pool, 0));
    }
    pool->shutdownAndJoin();

    assertConsumedInOrder(0, enqueued);
    ASSERT_EQ(consumedCount(), enqueued.size());
}

TEST_F(PipelinedApplierWorkerPoolTest, WorkersConsumeIndependently) {
    const size_t kNumWorkers = 4;
    auto pool = makePool(kNumWorkers);
    std::vector<std::vector<Timestamp>> enqueued(kNumWorkers);
    for (int i = 0; i < 100; ++i) {
        for (size_t w = 0; w < kNumWorkers; ++w) {
            enqueued[w].push_back(enqueueItem(*pool, w));
        }
    }
    pool->shutdownAndJoin();

    size_t total = 0;
    for (size_t w = 0; w < kNumWorkers; ++w) {
        assertConsumedInOrder(w, enqueued[w]);
        total += enqueued[w].size();
    }
    ASSERT_EQ(consumedCount(), total);
}

TEST_F(PipelinedApplierWorkerPoolTest, WorkersConsumeOnInternalKillExemptClients) {
    const size_t kNumWorkers = 4;
    auto pool = makePool(kNumWorkers);
    for (size_t w = 0; w < kNumWorkers; ++w) {
        enqueueItem(*pool, w);
    }
    pool->shutdownAndJoin();

    ASSERT_EQ(consumedCount(), kNumWorkers);
    ASSERT_TRUE(allConsumedOnKillExemptClients());
}

TEST_F(PipelinedApplierWorkerPoolTest, ShutdownWithIdleWorkers) {
    auto pool = makePool(4 /* numWorkers */);
    // Workers are idle waiting on their empty queues; shutdown joins all of them without
    // hanging.
    pool->shutdownAndJoin();
    ASSERT_EQ(consumedCount(), 0);
}

TEST_F(PipelinedApplierWorkerPoolTest, ShutdownUnderLoadConsumesEveryItem) {
    const size_t kNumWorkers = 8;
    auto pool = makePool(kNumWorkers);
    std::vector<std::vector<Timestamp>> enqueued(kNumWorkers);

    {
        ScopedBlockedWorkers _{this, pool.get()};

        // Give each worker one item and wait until every worker is held mid-consumption.
        for (size_t w = 0; w < kNumWorkers; ++w) {
            enqueued[w].push_back(enqueueItem(*pool, w));
        }
        waitForConsumedCount(kNumWorkers);

        // Fill every queue while its worker is held, then start the shutdown from another thread
        // (shutdownAndJoin blocks on the held workers). This exercises a shutdown that begins with
        // undrained queues and workers mid-consumption.
        for (int i = 0; i < 500; ++i) {
            for (size_t w = 0; w < kNumWorkers; ++w) {
                enqueued[w].push_back(enqueueItem(*pool, w));
            }
        }

        // The held workers pin the in-flight shutdown: nothing beyond the first items can have
        // been consumed while shutdown is running but workers are blocked.
        ASSERT_EQ(consumedCount(), kNumWorkers);
    }

    // Every enqueued item must still be consumed, in FIFO order, before shutdown completes.
    for (size_t w = 0; w < kNumWorkers; ++w) {
        assertConsumedInOrder(w, enqueued[w]);
    }
}

// waitForIdle() must wait for the blocked worker and its queued work even if other workers are
// idle.
TEST_F(PipelinedApplierWorkerPoolTest, WaitForIdleWaitsForEveryWorkerAndKeepsPoolReusable) {
    constexpr size_t kNumWorkers = 3;
    auto opCtx = makeOperationContext();
    Notification<void> held;
    Notification<void> release;
    PipelinedApplierWorkerPool pool(kNumWorkers, [&](size_t workerIdx, const WorkItem&) {
        // Block only worker 2; workers 0 and 1 can finish their work independently.
        if (workerIdx == kNumWorkers - 1) {
            // Signal once when worker 2 starts its first item. Its second item uses the same gate.
            if (!held) {
                held.set();
            }
            release.get();
        }
    });
    Notification<void> waiterStarted;
    Notification<void> drained;
    stdx::thread waiter;
    ON_BLOCK_EXIT([&] {
        // Release the worker before joining the waiter, including when an assertion fails.
        if (!release) {
            release.set();
        }
        if (waiter.joinable()) {
            waiter.join();
        }
    });

    // Waiting on an unused pool must return, and leave the workers available for dispatch.
    pool.waitForIdle();
    // Issue two WorkItems per worker. Worker 2 will be blocked inside its first item with its
    // second item still queued.
    for (size_t workerIdx = 0; workerIdx < kNumWorkers; ++workerIdx) {
        enqueueItem(pool, workerIdx);
        enqueueItem(pool, workerIdx);
    }
    // Wait until worker 2 is inside its callback before starting the idle wait.
    held.get();
    // Run the blocking wait on another thread so this thread can release worker 2.
    waiter = stdx::thread([&] {
        waiterStarted.set();
        pool.waitForIdle();
        drained.set();
    });
    // Start the blocking check only after the waiter thread has been scheduled.
    ASSERT_TRUE(waiterStarted.waitFor(opCtx.get(), Seconds(10)));
    // The idle wait must not finish while worker 2 is still held.
    ASSERT_FALSE(drained.waitFor(opCtx.get(), Milliseconds(100)));
    // Opening the gate lets worker 2 finish both its active item and its queued item.
    release.set();
    ASSERT_TRUE(drained.waitFor(opCtx.get(), Seconds(10)));
    waiter.join();

    // Returning from waitForIdle() must mean both items on every worker have finished.
    BSONObjBuilder bob;
    pool.report(bob);
    for (const auto& worker : bob.obj()) {
        ASSERT_EQ(worker.Obj()["scheduled"].numberLong(), 2);
        ASSERT_EQ(worker.Obj()["executed"].numberLong(), 2);
    }

    // A second dispatch/drain cycle uses the same workers without shutting them down.
    for (size_t workerIdx = 0; workerIdx < kNumWorkers; ++workerIdx) {
        enqueueItem(pool, workerIdx);
    }
    pool.waitForIdle();
    // Waiting again with no new work must also return immediately.
    pool.waitForIdle();
    BSONObjBuilder afterReuse;
    pool.report(afterReuse);
    for (const auto& worker : afterReuse.obj()) {
        ASSERT_EQ(worker.Obj()["scheduled"].numberLong(), 3);
        ASSERT_EQ(worker.Obj()["executed"].numberLong(), 3);
    }
}

TEST_F(PipelinedApplierWorkerPoolTest, EnqueueAfterShutdownThrows) {
    auto pool = makePool(2 /* numWorkers */);
    enqueueItem(*pool, 0);
    pool->shutdownAndJoin();

    ASSERT_THROWS_CODE(enqueueItem(*pool, 1), DBException, ErrorCodes::ShutdownInProgress);
    ASSERT_EQ(consumedCount(), 1);
}

TEST_F(PipelinedApplierWorkerPoolTest, ShutdownAndJoinIsIdempotent) {
    auto pool = makePool(2 /* numWorkers */);
    enqueueItem(*pool, 0);
    pool->shutdownAndJoin();
    pool->shutdownAndJoin();
    ASSERT_EQ(consumedCount(), 1);
}

TEST_F(PipelinedApplierWorkerPoolTest, ReportsPerWorkerCounters) {
    const size_t kNumWorkers = 2;
    auto pool = makePool(kNumWorkers);
    for (int i = 0; i < 3; ++i) {
        enqueueItem(*pool, 0);
    }
    enqueueItem(*pool, 1);
    pool->shutdownAndJoin();

    BSONObjBuilder bob;
    pool->report(bob);
    auto report = bob.obj();
    ASSERT_EQ(report["worker0"]["scheduled"].numberLong(), 3);
    ASSERT_EQ(report["worker0"]["executed"].numberLong(), 3);
    ASSERT_EQ(report["worker1"]["scheduled"].numberLong(), 1);
    ASSERT_EQ(report["worker1"]["executed"].numberLong(), 1);
}

TEST_F(PipelinedApplierWorkerPoolTest, RepeatedPoolLifecycles) {
    for (int cycle = 0; cycle < 5; ++cycle) {
        auto pool = makePool(4 /* numWorkers */);
        for (size_t w = 0; w < 4; ++w) {
            enqueueItem(*pool, w);
        }
        pool->shutdownAndJoin();
    }
    ASSERT_EQ(consumedCount(), 20);
}

}  // namespace
}  // namespace mongo::repl
