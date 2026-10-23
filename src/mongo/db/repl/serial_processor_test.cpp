// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/serial_processor.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/duration.h"

#include <chrono>
#include <vector>

namespace mongo {
namespace {

TEST(SerialProcessor, DefaultProcessFnAcceptsItemsAndDrains) {
    SerialProcessor<int> sp{{}};
    ASSERT_OK(sp.getStatus());
    sp.push(1);
    sp.push(2);
    sp.push(3);
    sp.drain();
    ASSERT_OK(sp.getStatus());
}

TEST(SerialProcessor, ProcessesItemsInFifoOrder) {
    static constexpr int kItemCount = 1000;
    std::vector<int> seen;
    seen.reserve(kItemCount);
    SerialProcessor<int> sp{{.process = [&](int v) {
        seen.push_back(v);
        return Status::OK();
    }}};
    for (int i = 0; i < kItemCount; ++i) {
        sp.push(i);
    }
    sp.drain();
    ASSERT_EQ(seen.size(), kItemCount);
    for (int i = 0; i < kItemCount; ++i) {
        ASSERT_EQ(seen[i], i);
    }
}

TEST(SerialProcessor, ShutdownIsIdempotentAndReturnsPendingItems) {
    Notification<void> blocked;
    Notification<void> released;

    SerialProcessor<int> sp{{.process = [&](int) {
        blocked.set();
        released.get();
        return Status::OK();
    }}};

    // Push one item that will block in the processor, plus a few more that stay queued.
    sp.push(1);
    blocked.get();
    sp.push(2);
    sp.push(3);

    // Release the in-flight item so the worker can observe shutdown.
    released.set();

    auto leftover = sp.shutdown();
    ASSERT_EQ(leftover, (std::deque<int>{2, 3}));

    auto leftover2 = sp.shutdown();
    ASSERT_TRUE(leftover2.empty());

    auto status = sp.getStatus();
    ASSERT_EQ(status.code(), ErrorCodes::ShutdownInProgress);
    ASSERT_EQ(status.reason(), SerialProcessor<int>::kShutdownReason);

    // Pushes after shutdown throw with the shutdown reason.
    ASSERT_THROWS_CODE(sp.push(99), AssertionException, ErrorCodes::ShutdownInProgress);
}

TEST(SerialProcessor, ProcessorErrorStopsWorkerAndPropagates) {
    const Status kErr{ErrorCodes::InternalError, "error message"};
    size_t calls = 0;
    SerialProcessor<int> sp{{.process = [&](int v) -> Status {
        ++calls;
        if (v == 2) {
            return kErr;
        }
        return Status::OK();
    }}};

    sp.push(1);
    sp.push(2);
    // Give the worker a chance to process; drain returns once the queue is empty.
    sp.drain();
    ASSERT_EQ(sp.getStatus(), kErr);

    // Subsequent pushes throw with the worker error, not the shutdown reason.
    ASSERT_THROWS_CODE(sp.push(3), AssertionException, ErrorCodes::InternalError);

    // Shutdown still works and preserves the worker error.
    sp.shutdown();
    ASSERT_EQ(sp.getStatus(), kErr);
}

TEST(SerialProcessor, DrainWithoutItemsReturnsImmediately) {
    SerialProcessor<int> sp{{}};
    sp.drain();
    ASSERT_OK(sp.getStatus());
}

TEST(SerialProcessor, DrainWaitsForQueueToEmpty) {
    size_t processed = 0;
    SerialProcessor<int> sp{{.process = [&](int) {
        ++processed;
        return Status::OK();
    }}};
    for (int i = 0; i < 10; ++i) {
        sp.push(i);
    }
    sp.drain();
    ASSERT_EQ(processed, 10ull);
}

TEST(SerialProcessor, DrainWithTimeoutTimesOutAndThenCompletes) {
    const Status kErr{ErrorCodes::InternalError, "error message"};
    Notification<void> released;
    SerialProcessor<int> sp{{.process = [&](int) {
        released.get();
        return kErr;
    }}};
    sp.push(1);

    // Worker is parked in released.get(), so drain must time out.
    ASSERT_FALSE(sp.drain(Milliseconds{50}));

    released.set();

    // Worker now returns kErr, pops the item, and sets _status. The queue is empty, so
    // drain() completes without timing out, and the error surfaces via getStatus().
    ASSERT_TRUE(sp.drain(Milliseconds{5000}));
    ASSERT_EQ(sp.getStatus(), kErr);
}

TEST(SerialProcessor, ShutdownAsyncDoesNotJoinButBlocksPush) {
    Notification<void> entered;
    Notification<void> finishedProcessing;
    Notification<void> released;

    SerialProcessor<int> sp{{.process = [&](int) {
        entered.set();
        released.get();
        finishedProcessing.set();
        return Status::OK();
    }}};
    sp.push(1);
    entered.get();
    sp.shutdownAsync();

    ASSERT_THROWS_CODE(sp.push(2), AssertionException, ErrorCodes::ShutdownInProgress);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_FALSE(bool(finishedProcessing));

    released.set();
    finishedProcessing.get();
}

TEST(SerialProcessor, DrainCompletesAfterWorkerFailure) {
    const Status kErr{ErrorCodes::InternalError, "error message"};
    SerialProcessor<int> sp{{.process = [&](int) {
        return kErr;
    }}};
    sp.push(1);
    sp.drain();
    ASSERT_EQ(sp.getStatus(), kErr);
}

TEST(SerialProcessor, InitRunsOnceOnWorkerThreadBeforeProcessing) {
    Notification<void> initDone;
    std::thread::id initThreadId;
    std::thread::id firstProcessThreadId;
    std::thread::id mainThreadId = std::this_thread::get_id();
    size_t initCalls = 0;

    SerialProcessor<int> sp{{
        .process =
            [&](int) {
                if (firstProcessThreadId == std::thread::id{}) {
                    firstProcessThreadId = std::this_thread::get_id();
                }
                return Status::OK();
            },
        .init =
            [&] {
                ++initCalls;
                initThreadId = std::this_thread::get_id();
                initDone.set();
                return Status::OK();
            },
    }};

    initDone.get();
    ASSERT_EQ(initCalls, 1u);
    ASSERT_NE(initThreadId, mainThreadId);

    for (int i = 0; i < 5; ++i) {
        sp.push(i);
    }
    sp.drain();
    ASSERT_EQ(initCalls, 1u);
    ASSERT_EQ(firstProcessThreadId, initThreadId);
}

TEST(SerialProcessor, InitErrorAbortsWorkerAndSurfacesError) {
    const Status kInitErr{ErrorCodes::InternalError, "init failed"};
    Notification<void> initCalled;
    bool processCalled = false;
    SerialProcessor<int> sp{{
        .process =
            [&](int) {
                processCalled = true;
                return Status::OK();
            },
        .init =
            [&] {
                initCalled.set();
                return kInitErr;
            },
    }};

    // Ensure that the worker thread acquired the mutex.
    initCalled.get();
    // Drain acquires the same mutex, so we guarantee a happened-before relation.
    sp.drain();
    ASSERT_EQ(sp.getStatus(), kInitErr);

    ASSERT_THROWS_CODE(sp.push(1), AssertionException, ErrorCodes::InternalError);
    ASSERT_FALSE(processCalled);

    sp.shutdown();
    ASSERT_EQ(sp.getStatus(), kInitErr);
}

TEST(SerialProcessor, ConcurrentPushRaceWithEarlyConsumerStop) {
    static constexpr size_t kProducers = 8;
    static constexpr size_t kConsumerLimit = 100;
    const Status kStop{ErrorCodes::InternalError, "consumer stop"};

    Atomic<int> enqueueAttempts{0};
    Atomic<int> enqueued{0};
    Notification<void> consumerStopped;
    stdx::unordered_set<int> seen;

    // The consumer accepts kConsumerLimit distinct items, then flips a flag and returns
    // NOT-OK.
    // Before returning, it spins until producers have attempted more pushes than
    // the consumer can accept. This ensures that when we call shutdown, we guarantee that at least
    // one item will be dropped.
    SerialProcessor<int> sp{{.process = [&](int v) -> Status {
        seen.insert(v);
        if (static_cast<int>(seen.size()) == kConsumerLimit) {
            ASSERT_FALSE(bool(consumerStopped));
            consumerStopped.set();
            while (static_cast<size_t>(enqueueAttempts.load()) <= kConsumerLimit) {
                std::this_thread::yield();
            }
            return kStop;
        }
        return Status::OK();
    }}};

    // Each producer pushes atomically-assigned ints (globally unique) in a tight loop.
    // The loop exits when a push throws (worker returned NOT_OK or we're shutting down).
    // Then, the producer calls shutdown() and stores whatever leftover items were returned
    // from draining.
    std::vector<stdx::thread> producers;
    std::vector<std::deque<int>> leftover(kProducers);
    producers.reserve(kProducers);
    for (size_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (;;) {
                int value = enqueueAttempts.fetchAndAdd(1);
                try {
                    sp.push(value);
                    enqueued.fetchAndAddRelaxed(1);
                } catch (const DBException&) {
                    // Store the leftover items returned from shutdown.
                    leftover[p] = sp.shutdown();
                    return;
                }
            }
        });
    }

    for (auto& t : producers) {
        t.join();
    }

    // Assert the following:

    // (1) Exactly one producer's shutdown() call yielded the leftover items (which should not be
    // empty). Losing racers see an empty deque because the first winner already drained _items.
    const std::deque<int>* winner = [&] {
        const std::deque<int>* result = nullptr;
        int nonEmpty = 0;
        for (const auto& d : leftover) {
            if (!d.empty()) {
                ++nonEmpty;
                result = &d;
            }
        }
        ASSERT_EQ(nonEmpty, 1);
        return result;
    }();

    // (2) Consumer consumed exactly kConsumerLimit distinct items.
    ASSERT_EQ(seen.size(), static_cast<size_t>(kConsumerLimit));

    // (3) The number of dropped items should be equal to the number of items we pushed into the
    // queue minus the number of items that the worker processed.
    stdx::unordered_set<int> dropped(winner->begin(), winner->end());
    ASSERT_EQ(dropped.size(), static_cast<size_t>(enqueued.load() - kConsumerLimit));

    // (4) The dropped items have no duplicates and do not contain items the worker consumed.
    ASSERT_EQ(dropped.size(), winner->size());
    ASSERT_EQ(
        0, std::count_if(dropped.begin(), dropped.end(), [&](int v) { return seen.contains(v); }));

    // (5) The final processor status carries the consumer's error, not the shutdown reason.
    ASSERT_EQ(sp.getStatus(), kStop);
}

TEST(SerialProcessor, ConcurrentPushRaceAgainstShutdownDropsSomeItems) {
    static constexpr int kProducers = 8;

    Notification<void> blocked;
    Notification<void> released;
    Atomic<int> processed{0};
    Atomic<int> nextItem{0};
    Atomic<int> enqueued{0};

    // The first processed item parks the worker so pending items can pile up in the queue.
    // Subsequent items process normally and bump `processed`.
    SerialProcessor<int> sp{{.process = [&](int) {
        if (!bool(blocked)) {
            blocked.set();
            released.get();
        }
        processed.fetchAndAddRelaxed(1);
        return Status::OK();
    }}};

    // Producers wait on `go` and then push continuously until a push throws (shutdown).
    // This ensures that the producers actually race with the shutdown thread, instead of finishing
    // before it.
    Notification<void> go;
    std::vector<stdx::thread> producers;
    std::vector<std::vector<int>> okItems(kProducers);
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            go.get();
            for (;;) {
                int v = nextItem.fetchAndAddRelaxed(1);
                try {
                    sp.push(v);
                    okItems[p].push_back(v);
                    enqueued.fetchAndAddRelaxed(1);
                } catch (const DBException& e) {
                    ASSERT_EQ(e.code(), ErrorCodes::ShutdownInProgress);
                    ASSERT_EQ(e.reason(), SerialProcessor<int>::kShutdownReason);
                    return;
                }
            }
        });
    }

    go.set();

    // Wait for the worker to park AND for a second successful push, so at least one item is
    // sitting behind the in-flight one.
    blocked.get();
    while (enqueued.loadRelaxed() < 2) {
        std::this_thread::yield();
    }
    // shutdown() must be on its own thread because it joins the worker, which is still blocked.
    std::deque<int> leftover;
    auto shutdownThread = stdx::thread([&] { leftover = sp.shutdown(); });

    released.set();

    for (auto& t : producers) {
        t.join();
    }
    shutdownThread.join();

    // Every push() that returned successfully is either processed by the worker or returned as
    // leftover by shutdown() — no accepted item may vanish.
    stdx::unordered_set<int> accepted;
    for (const auto& v : okItems) {
        accepted.insert(v.begin(), v.end());
    }
    ASSERT_EQ(accepted.size(), processed.load() + leftover.size());

    // And we must have actually raced: at least one accepted item was abandoned at shutdown.
    ASSERT_GT(leftover.size(), 0u);

    ASSERT_THROWS_CODE(sp.push(-1), AssertionException, ErrorCodes::ShutdownInProgress);
}

}  // namespace
}  // namespace mongo
