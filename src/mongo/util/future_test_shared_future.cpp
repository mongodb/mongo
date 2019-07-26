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
                            const auto exec = InlineCountingExecutor::make();
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

NOINLINE_DECL void useALotOfStackSpace() {
    // Try to force the compiler to allocate 100K of stack.
    volatile char buffer[100'000];  // NOLINT
    buffer[99'999] = 'x';
    buffer[0] = buffer[99'999];
    ASSERT_EQ(buffer[0], 'x');
}

TEST(SharedFuture, NoStackOverflow_Call) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            const auto exec = InlineCountingExecutor::make();
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
                            const auto exec = InlineCountingExecutor::make();
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
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            const auto exec = InlineCountingExecutor::make();

                            auto res = std::move(fut).then([] { return SharedSemiFuture(1); });

                            IF_CONSTEXPR(
                                std::is_same_v<std::decay_t<decltype(fut)>, ExecutorFuture<void>>) {
                                static_assert(std::is_same_v<decltype(res), ExecutorFuture<int>>);
                            }
                            else {
                                static_assert(std::is_same_v<decltype(res), SemiFuture<int>>);
                            }
                            ASSERT_EQ(res.get(), 1);
                        });
}

TEST(SharedFuture, ThenChaining_Async) {
    FUTURE_SUCCESS_TEST(
        [] {},
        [](/*Future<void>*/ auto&& fut) {
            const auto exec = InlineCountingExecutor::make();

            auto res = std::move(fut).then([] { return async([] { return 1; }).share(); });

            IF_CONSTEXPR(std::is_same_v<std::decay_t<decltype(fut)>, ExecutorFuture<void>>) {
                static_assert(std::is_same_v<decltype(res), ExecutorFuture<int>>);
            }
            else {
                static_assert(std::is_same_v<decltype(res), SemiFuture<int>>);
            }
            ASSERT_EQ(res.get(), 1);
        });
}

TEST(SharedFuture, ThenChaining_Async_DoubleShare) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            const auto exec = InlineCountingExecutor::make();

                            auto res = std::move(fut).share().thenRunOn(exec).then(
                                [] { return async([] { return 1; }).share(); });

                            ASSERT_EQ(res.get(), 1);
                        });
}

TEST(SharedFuture, AddChild_Get) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            const auto exec = InlineCountingExecutor::make();
                            auto shared = std::move(fut).share();
                            auto fut2 = shared.thenRunOn(exec).then([] {});
                            shared.get();
                            fut2.get();
                        });
}

TEST(SharedFuture, InterruptedGet_AddChild_Get) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            const auto exec = InlineCountingExecutor::make();
                            DummyInterruptable dummyInterruptable;

                            auto shared = std::move(fut).share();
                            auto res = shared.waitNoThrow(&dummyInterruptable);
                            if (!shared.isReady()) {
                                ASSERT_EQ(res, ErrorCodes::Interrupted);
                            }

                            auto fut2 = shared.thenRunOn(exec).then([] {});

                            shared.get();
                            fut2.get();
                        });
}

TEST(SharedFuture, ConcurrentTest_OneSharedFuture) {
    auto nTries = 16;
    while (nTries--) {
        const auto nThreads = 16;
        auto threads = std::vector<stdx::thread>(nThreads);
        const auto exec = InlineCountingExecutor::make();

        SharedPromise<void> promise;

        auto shared = promise.getFuture();

        for (int i = 0; i < nThreads; i++) {
            threads[i] = stdx::thread([i, &shared, &exec] {
                if (i % 5 == 0) {
                    // just wait directly on shared.
                    shared.get();
                } else if (i % 7 == 0) {
                    // interrupted wait, then blocking wait.
                    DummyInterruptable dummyInterruptable;
                    auto res = shared.waitNoThrow(&dummyInterruptable);
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
            });
        }

        if (nTries % 2 == 0)
            stdx::this_thread::yield();  // Slightly increase the chance of racing.

        promise.emplaceValue();

        for (auto&& thread : threads) {
            thread.join();
        }
    }
}

TEST(SharedFuture, ConcurrentTest_ManySharedFutures) {
    auto nTries = 16;
    while (nTries--) {
        const auto nThreads = 16;
        auto threads = std::vector<stdx::thread>(nThreads);
        const auto exec = InlineCountingExecutor::make();

        SharedPromise<void> promise;

        for (int i = 0; i < nThreads; i++) {
            threads[i] = stdx::thread([i, &promise, &exec] {
                auto shared = promise.getFuture();

                if (i % 5 == 0) {
                    // just wait directly on shared.
                    shared.get();
                } else if (i % 7 == 0) {
                    // interrupted wait, then blocking wait.
                    DummyInterruptable dummyInterruptable;
                    auto res = shared.waitNoThrow(&dummyInterruptable);
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
            });
        }

        if (nTries % 2 == 0)
            stdx::this_thread::yield();  // Slightly increase the chance of racing.

        promise.emplaceValue();

        for (auto&& thread : threads) {
            thread.join();
        }
    }
}

}  // namespace
}  // namespace mongo
