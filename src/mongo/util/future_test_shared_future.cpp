/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/util/future.h"

#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future_test_utils.h"

namespace mongo {
namespace {

// Tests of SharedFuture based on "core" tests.

TEST(SharedFuture, Success_shared_get) {
    FUTURE_SUCCESS_TEST(
        [] { return 1; },
        [](/*Future<int>*/ auto&& fut) { ASSERT_EQ(std::move(fut).share().get(), 1); });
}

TEST(SharedFuture, Success_shared_getNothrow) {
    FUTURE_SUCCESS_TEST(
        [] { return 1; },
        [](/*Future<int>*/ auto&& fut) { ASSERT_EQ(std::move(fut).share().getNoThrow(), 1); });
}

TEST(SharedFuture, Fail_shared_get) {
    FUTURE_FAIL_TEST<int>(
        [](/*Future<int>*/ auto&& fut) { ASSERT_THROWS_failStatus(std::move(fut).share().get()); });
}

TEST(SharedFuture, Fail_shared_getNothrow) {
    FUTURE_FAIL_TEST<int>([](/*Future<int>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut).share().getNoThrow(), failStatus());
    });
}

TEST(SharedFuture, isReady_shared_TSAN_OK) {
    bool done = false;
    auto fut = async([&] {
                   done = true;
                   return 1;
               })
                   .share();
    //(void)*const_cast<volatile bool*>(&done);  // Data Race! Uncomment to make sure TSAN works.
    while (!fut.isReady()) {
    }
    ASSERT(done);
    (void)fut.get();
    ASSERT(done);
}

TEST(SharedFuture_Void, Success_shared_get) {
    FUTURE_SUCCESS_TEST([] {}, [](/*Future<void>*/ auto&& fut) { std::move(fut).share().get(); });
}

TEST(SharedFuture_Void, Success_shared_getNothrow) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut).share().getNoThrow(), Status::OK());
                        });
}

TEST(SharedFuture_Void, Fail_share_getRvalue) {
    FUTURE_FAIL_TEST<void>([](/*Future<void>*/ auto&& fut) {
        ASSERT_THROWS_failStatus(std::move(fut).share().get());
    });
}

TEST(SharedFuture_Void, Fail_share_getNothrow) {
    FUTURE_FAIL_TEST<void>([](/*Future<void>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut).share().getNoThrow(), failStatus());
    });
}

TEST(SharedFuture_Void, isReady_share_TSAN_OK) {
    bool done = false;
    auto fut = async([&] { done = true; }).share();
    //(void)*const_cast<volatile bool*>(&done);  // Data Race! Uncomment to make sure TSAN works.
    while (!fut.isReady()) {
    }
    ASSERT(done);
    fut.get();
    ASSERT(done);
}

// Test of SharedSemiFuture specific details.

TEST(SharedFuture, ModificationsArePrivate) {
    FUTURE_SUCCESS_TEST([] { return 1; },
                        [](/*Future<int>*/ auto&& fut) {
                            const auto exec = InlineRecursiveCountingExecutor::make();
                            const auto shared = std::move(fut).share();

                            const auto checkFunc = [](int&& i) {
                                ASSERT_EQ(i, 1);
                                return ++i;
                            };

                            auto fut1 = shared.thenRunOn(exec).then(checkFunc);
                            auto fut2 = shared.thenRunOn(exec).then(checkFunc);
                            ASSERT_EQ(fut1.get(), 2);
                            ASSERT_EQ(fut2.get(), 2);
                            ASSERT_NE(&fut1.get(), &fut2.get());


                            // Try mixing continuations before and after completion.
                            auto fut3 = shared.thenRunOn(exec).then(checkFunc);
                            ASSERT_EQ(fut3.get(), 2);
                            ASSERT_NE(&fut3.get(), &fut1.get());
                            ASSERT_NE(&fut3.get(), &fut2.get());
                        });
}

MONGO_COMPILER_NOINLINE void useALotOfStackSpace() {
    // Try to force the compiler to allocate 100K of stack.
    volatile char buffer[100'000];  // NOLINT
    buffer[99'999] = 'x';
    buffer[0] = buffer[99'999];
    ASSERT_EQ(buffer[0], 'x');
}

TEST(SharedFuture, NoStackOverflow_Call) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            const auto exec = InlineRecursiveCountingExecutor::make();
                            const auto shared = std::move(fut).share();

                            std::vector<SemiFuture<void>> collector;

                            // Add 100 children that each use 100K of stack space.
                            for (int i = 0; i < 100; i++) {
                                collector.push_back(
                                    shared.thenRunOn(exec).then(useALotOfStackSpace).semi());
                            }

                            for (auto&& collected : collector) {
                                collected.get();
                            }
                        });
}

TEST(SharedFuture, NoStackOverflow_Destruction) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            const auto exec = InlineRecursiveCountingExecutor::make();
                            const auto shared = std::move(fut).share();

                            std::vector<SemiFuture<void>> collector;

                            struct Evil {
                                ~Evil() {
                                    useALotOfStackSpace();
                                }
                            };

                            // Add 100 children that each use 100K of stack space on destruction.
                            for (int i = 0; i < 100; i++) {
                                collector.push_back(
                                    shared.thenRunOn(exec).then([x = Evil()] {}).semi());
                            }

                            for (auto&& collected : collector) {
                                collected.get();
                            }
                        });
}

TEST(SharedFuture, ThenChaining_Sync) {
    FUTURE_SUCCESS_TEST(
        [] {},
        [](/*Future<void>*/ auto&& fut) {
            const auto exec = InlineRecursiveCountingExecutor::make();

            auto res = std::move(fut).then([] { return SharedSemiFuture(1); });

            if constexpr (std::is_same_v<std::decay_t<decltype(fut)>, ExecutorFuture<void>>) {
                static_assert(std::is_same_v<decltype(res), ExecutorFuture<int>>);
            } else {
                static_assert(std::is_same_v<decltype(res), SemiFuture<int>>);
            }
            ASSERT_EQ(res.get(), 1);
        });
}

TEST(SharedFuture, ThenChaining_Async) {
    FUTURE_SUCCESS_TEST(
        [] {},
        [](/*Future<void>*/ auto&& fut) {
            const auto exec = InlineRecursiveCountingExecutor::make();

            auto res = std::move(fut).then([] { return async([] { return 1; }).share(); });

            if constexpr (std::is_same_v<std::decay_t<decltype(fut)>, ExecutorFuture<void>>) {
                static_assert(std::is_same_v<decltype(res), ExecutorFuture<int>>);
            } else {
                static_assert(std::is_same_v<decltype(res), SemiFuture<int>>);
            }
            ASSERT_EQ(res.get(), 1);
        });
}

TEST(SharedFuture, ThenChaining_Async_DoubleShare) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            const auto exec = InlineRecursiveCountingExecutor::make();

                            auto res = std::move(fut).share().thenRunOn(exec).then(
                                [] { return async([] { return 1; }).share(); });

                            ASSERT_EQ(res.get(), 1);
                        });
}

TEST(SharedFuture, AddChild_ThenRunOn_Get) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            const auto exec = InlineRecursiveCountingExecutor::make();
                            auto shared = std::move(fut).share();
                            auto fut2 = shared.thenRunOn(exec).then([] {});
                            shared.get();
                            fut2.get();
                        });
}

TEST(SharedFuture, AddChild_Split_Get) {
    FUTURE_SUCCESS_TEST([] { return 1; },
                        [](/*Future<void>*/ auto&& fut) {
                            auto shared = std::move(fut).share();
                            auto shared2 = shared.split();
                            ASSERT_EQ(shared.get(), 1);
                            ASSERT_EQ(shared2.get(), 1);
                        });
}

TEST(SharedFuture, InterruptedGet_AddChild_Get) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            const auto exec = InlineRecursiveCountingExecutor::make();
                            DummyInterruptible dummyInterruptible;

                            auto shared = std::move(fut).share();
                            auto res = shared.waitNoThrow(&dummyInterruptible);
                            if (!shared.isReady()) {
                                ASSERT_EQ(res, ErrorCodes::Interrupted);
                            }

                            auto fut2 = shared.thenRunOn(exec).then([] {});

                            shared.get();
                            fut2.get();
                        });
}

/** Punt until we have `std::jthread`. Joins itself in destructor. Move-only. */
class JoinThread : public stdx::thread {
public:
    explicit JoinThread(stdx::thread thread) : stdx::thread(std::move(thread)) {}
    JoinThread(const JoinThread&) = delete;
    JoinThread& operator=(const JoinThread&) = delete;
    JoinThread(JoinThread&&) noexcept = default;
    JoinThread& operator=(JoinThread&&) noexcept = default;
    ~JoinThread() {
        if (joinable())
            join();
    }
};

/** Try a simple single-worker shared get. Exercise JoinThread. */
TEST(SharedFuture, ConcurrentTest_Simple) {
    SharedPromise<void> promise;
    auto shared = promise.getFuture();
    JoinThread thread{stdx::thread{[&] { shared.get(); }}};
    stdx::this_thread::yield();  // Slightly increase the chance of racing.
    promise.emplaceValue();
}

void sharedFutureTestWorker(size_t i, SharedSemiFuture<void>& shared) {
    auto exec = InlineRecursiveCountingExecutor::make();
    if (i % 5 == 0) {
        // just wait directly on shared.
        shared.get();
    } else if (i % 7 == 0) {
        // interrupted wait, then blocking wait.
        DummyInterruptible dummyInterruptible;
        auto res = shared.waitNoThrow(&dummyInterruptible);
        if (!shared.isReady()) {
            ASSERT_EQ(res, ErrorCodes::Interrupted);
        }
        shared.get();
    } else if (i % 2 == 0) {
        // add a child.
        shared.thenRunOn(exec).then([] {}).get();
    } else {
        // add a grand child.
        shared.thenRunOn(exec).share().thenRunOn(exec).then([] {}).get();
    }
}

/**
 * Define a common structure between `ConcurrentTest_OneSharedFuture` and
 * `ConcurrentTest_ManySharedFutures`. They can vary only in the ways specified
 * by the `policy` hooks. The `policy` object defines per-try state (returned by
 * `onTryBegin`), and then per-thread state within each worker thread of each
 * try (returned by `onThreadBegin`). We want to ensure that the SharedPromise
 * API works the same whether you make multiple calls to getFuture() or just
 * one and copy it around.
 */
template <typename Policy>
void sharedFutureConcurrentTest(unittest::ThreadAssertionMonitor& monitor, Policy& policy) {
    const size_t nTries = 16;
    for (size_t tryCount = 0; tryCount < nTries; ++tryCount) {
        const size_t nThreads = 16;

        SharedPromise<void> promise;

        auto&& tryState = policy.onTryBegin(promise);
        std::vector<JoinThread> threads;
        for (size_t i = 0; i < nThreads; i++) {
            threads.push_back(JoinThread{monitor.spawn([&, i] {
                auto&& shared = policy.onThreadBegin(tryState);
                sharedFutureTestWorker(i, shared);
            })});
        }

        if (tryCount % 2 == 0)
            stdx::this_thread::yield();  // Slightly increase the chance of racing.

        promise.emplaceValue();
    }
}

/**
 * Make a SharedSemiFuture from the SharedPromise at the beginning of each try.
 * Use that same object in all of the worker threads.
 */
TEST(SharedFuture, ConcurrentTest_OneSharedFuture) {
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        struct {
            decltype(auto) onTryBegin(SharedPromise<void>& promise) {
                return promise.getFuture();
            }
            decltype(auto) onThreadBegin(SharedSemiFuture<void>& shared) {
                return shared;
            }
        } policy;
        sharedFutureConcurrentTest(monitor, policy);
    });
}

/**
 * Retain a SharedPromise through all the tries.
 * Peel multiple SharedSemiFuture from it, one per worker thread.
 */
TEST(SharedFuture, ConcurrentTest_ManySharedFutures) {
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        struct {
            decltype(auto) onTryBegin(SharedPromise<void>& promise) {
                return promise;
            }
            decltype(auto) onThreadBegin(SharedPromise<void>& promise) {
                return promise.getFuture();
            }
        } policy;
        sharedFutureConcurrentTest(monitor, policy);
    });
}

}  // namespace
}  // namespace mongo
