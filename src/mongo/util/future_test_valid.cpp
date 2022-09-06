/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/util/future.h"

#include "mongo/stdx/thread.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future_test_utils.h"

#include <boost/optional.hpp>

namespace mongo {
namespace {

/**
 * These tests validate the postconditions of operations on the 4 future types:
 * - Future
 * - SemiFuture
 * - SharedSemiFuture
 * - ExecutorFuture
 */

/** Asserts that the Future or ExecutorFuture is still valid() after `func`. */
template <typename TestFunc>
void assertFutureValidAfter(const TestFunc& func) {
    FUTURE_SUCCESS_TEST([] { return 0; },
                        [func](auto&& fut) {
                            func(std::move(fut));
                            ASSERT_TRUE(fut.valid());
                        });
}

/** Asserts that the Future or ExecutorFuture is invalid() after `func`. */
template <typename TestFunc>
void assertFutureInvalidAfter(const TestFunc& func) {
    FUTURE_SUCCESS_TEST([] { return 0; },
                        [func](auto&& fut) {
                            func(std::move(fut));
                            ASSERT_FALSE(fut.valid());
                        });
}

/**
 * Asserts that `func` returns a valid() future while making the input Future or ExecutorFuture
 * non-valid().
 */
template <DoExecutorFuture doExecutorFuture = kDoExecutorFuture, typename TestFunc>
void assertFutureTransfersValid(const TestFunc& func) {
    FUTURE_SUCCESS_TEST<doExecutorFuture>([] { return 0; },
                                          [func](auto&& fut) {
                                              auto otherFut = func(std::move(fut));
                                              ASSERT_FALSE(fut.valid());
                                              ASSERT_TRUE(otherFut.valid());
                                          });
}

/** Passes an invalid Future or ExecutorFuture into `func`. To be used with DEATH_TEST. */
template <DoExecutorFuture doExecutorFuture = kDoExecutorFuture, typename TestFunc>
void callWithInvalidFuture(const TestFunc& func) {
    FUTURE_SUCCESS_TEST<doExecutorFuture>([] { return 0; },
                                          [func](auto&& fut) {
                                              [[maybe_unused]] auto val = std::move(fut).get();
                                              (void)func(std::move(fut));
                                          });
}

TEST(FutureValid, ValidAtStart) {
    FUTURE_SUCCESS_TEST([] { return 0; }, [](auto&& fut) { ASSERT_TRUE(fut.valid()); });
}

// TODO SERVER-64948: this test is only needed if the lvalue& getter is kept around.
TEST(FutureValid, ValidAfterGetLvalue) {
    assertFutureValidAfter([](auto&& fut) { [[maybe_unused]] auto val = fut.get(); });
}

TEST(FutureValid, ValidAfterGetConstLvalue) {
    assertFutureValidAfter([](const auto& fut) { [[maybe_unused]] auto val = fut.get(); });
}

DEATH_TEST(FutureValid, GetConstLvalueCrashesOnInvalidFuture, "Invariant failure") {
    callWithInvalidFuture([](const auto& fut) { [[maybe_unused]] auto val = fut.get(); });
}

TEST(FutureValid, InvalidAfterGetRvalue) {
    assertFutureInvalidAfter([](auto&& fut) { [[maybe_unused]] auto val = std::move(fut).get(); });
}

DEATH_TEST(FutureValid, GetRvalueCrashesOnInvalidFuture, "Invariant failure") {
    callWithInvalidFuture([](auto&& fut) { [[maybe_unused]] auto val = std::move(fut).get(); });
}

TEST(FutureValid, ValidAfterGetNoThrowConstLvalue) {
    assertFutureValidAfter([](const auto& fut) { [[maybe_unused]] auto val = fut.getNoThrow(); });
}

DEATH_TEST(FutureValid, GetNoThrowConstLvalueCrashesOnInvalidFuture, "Invariant failure") {
    callWithInvalidFuture([](const auto& fut) { [[maybe_unused]] auto val = fut.getNoThrow(); });
}

TEST(FutureValid, InvalidAfterGetNoThrowRvalue) {
    assertFutureInvalidAfter(
        [](auto&& fut) { [[maybe_unused]] auto val = std::move(fut).getNoThrow(); });
}

DEATH_TEST(FutureValid, GetNoThrowRvalueCrashesOnInvalidFuture, "Invariant failure") {
    callWithInvalidFuture(
        [](auto&& fut) { [[maybe_unused]] auto val = std::move(fut).getNoThrow(); });
}

TEST(FutureValid, ValidAfterWait) {
    assertFutureValidAfter([](auto&& fut) { fut.wait(); });
}

DEATH_TEST(FutureValid, WaitCrashesOnInvalidFuture, "Invariant failure") {
    callWithInvalidFuture([](auto&& fut) { fut.wait(); });
}

TEST(FutureValid, ValidAfterWaitNoThrow) {
    assertFutureValidAfter([](auto&& fut) { [[maybe_unused]] auto status = fut.waitNoThrow(); });
}

DEATH_TEST(FutureValid, WaitNoThrowCrashesOnInvalidFuture, "Invariant failure") {
    callWithInvalidFuture([](auto&& fut) { [[maybe_unused]] auto status = fut.waitNoThrow(); });
}

TEST(FutureValid, ThenTransfersValid) {
    assertFutureTransfersValid(
        [](auto&& fut) { return std::move(fut).then([](int i) { return i + 2; }); });
}

DEATH_TEST(FutureValid, ThenCrashesOnInvalidFuture, "Invariant failure") {
    callWithInvalidFuture(
        [](auto&& fut) { return std::move(fut).then([](int i) { return i + 2; }); });
}

TEST(FutureValid, ThenRunOnTransfersValid) {
    assertFutureTransfersValid([](auto&& fut) {
        auto exec = InlineQueuedCountingExecutor::make();
        return std::move(fut).thenRunOn(exec);
    });
}

TEST(FutureValid, InvalidTransfersValid) {
    assertFutureTransfersValid([](auto&& fut) { return std::move(fut).ignoreValue(); });
}

TEST(FutureValid, InvalidAfterGetAsync) {
    assertFutureInvalidAfter([](auto&& fut) { std::move(fut).getAsync([](auto) {}); });
}

TEST(FutureValid, OnCompletionTransfersValid) {
    assertFutureTransfersValid([](auto&& fut) { return std::move(fut).onCompletion([](auto) {}); });
}

TEST(FutureValid, OnErrorTransfersValid) {
    assertFutureTransfersValid(
        [](auto&& fut) { return std::move(fut).onError([](auto) { return 0; }); });
}

TEST(FutureValid, OnErrorCategoryTransfersValid) {
    assertFutureTransfersValid([](auto&& fut) {
        return std::move(fut).template onErrorCategory<ErrorCategory::NetworkError>(
            [](auto) { return 0; });
    });
}

TEST(FutureValid, TapTransfersValid) {
    assertFutureTransfersValid<kNoExecutorFuture_needsTap>(
        [](auto&& fut) { return std::move(fut).tap([](auto) {}); });
}

TEST(FutureValid, TapErrorTransfersValid) {
    assertFutureTransfersValid<kNoExecutorFuture_needsTap>(
        [](auto&& fut) { return std::move(fut).tapError([](auto) {}); });
}

TEST(FutureValid, TapAllTransfersValid) {
    assertFutureTransfersValid<kNoExecutorFuture_needsTap>(
        [](auto&& fut) { return std::move(fut).tapAll([](auto) {}); });
}

TEST(FutureValid, MoveTransfersValid) {
    assertFutureTransfersValid([](auto&& fut) { return std::move(fut); });
}

TEST(FutureValid, SemiTransfersValid) {
    assertFutureTransfersValid([](auto&& fut) { return std::move(fut).semi(); });
}

TEST(FutureValid, ShareTransfersValid) {
    assertFutureTransfersValid([](auto&& fut) { return std::move(fut).share(); });
}

DEATH_TEST(FutureValid, ShareCrashesOnInvalidFuture, "Invariant failure") {
    callWithInvalidFuture([](auto&& fut) { return std::move(fut).share(); });
}

/** Asserts that the SemiFuture is invalid() after `func`. */
template <typename TestFunc>
void assertSemiFutureInvalidAfter(const TestFunc& func) {
    FUTURE_SUCCESS_TEST([] { return 0; },
                        [func](auto&& fut) {
                            auto semiFut = std::move(fut).semi();
                            func(std::move(semiFut));
                            ASSERT_FALSE(semiFut.valid());
                        });
}

/** Asserts that the SemiFuture is still valid() after `func`. */
template <typename TestFunc>
void assertSemiFutureValidAfter(const TestFunc& func) {
    FUTURE_SUCCESS_TEST([] { return 0; },
                        [func](auto&& fut) {
                            auto semiFut = std::move(fut).semi();
                            func(std::move(semiFut));
                            ASSERT_TRUE(semiFut.valid());
                        });
}

/* Asserts that `func` returns a valid() future while making the input SemiFuture non-valid(). */
template <typename TestFunc>
void assertSemiFutureTransfersValid(const TestFunc& func) {
    FUTURE_SUCCESS_TEST([] { return 0; },
                        [func](auto&& fut) {
                            auto semiFut = std::move(fut).semi();
                            auto otherFut = func(std::move(semiFut));
                            ASSERT_FALSE(semiFut.valid());
                            ASSERT_TRUE(otherFut.valid());
                        });
}

// TODO SERVER-64948: this test is only needed if the lvalue& getter is kept around.
TEST(SemiFutureValid, ValidAfterGetLvalue) {
    assertSemiFutureValidAfter([](auto&& fut) { [[maybe_unused]] auto val = fut.get(); });
}

TEST(SemiFutureValid, ValidAfterGetConstLvalue) {
    assertSemiFutureValidAfter([](const auto& fut) { [[maybe_unused]] auto val = fut.get(); });
}

TEST(SemiFutureValid, InvalidAfterGetRvalue) {
    assertSemiFutureInvalidAfter(
        [](auto&& fut) { [[maybe_unused]] auto val = std::move(fut).get(); });
}

TEST(SemiFutureValid, ValidAfterGetNoThrowConstLvalue) {
    assertSemiFutureValidAfter(
        [](const auto& fut) { [[maybe_unused]] auto val = fut.getNoThrow(); });
}

TEST(SemiFutureValid, InvalidAfterGetNoThrowRvalue) {
    assertSemiFutureInvalidAfter(
        [](auto&& fut) { [[maybe_unused]] auto val = std::move(fut).getNoThrow(); });
}

TEST(SemiFutureValid, ValidAfterWait) {
    assertSemiFutureValidAfter([](auto&& fut) { fut.wait(); });
}

TEST(SemiFutureValid, ValidAfterWaitNoThrow) {
    assertSemiFutureValidAfter(
        [](auto&& fut) { [[maybe_unused]] auto status = fut.waitNoThrow(); });
}

TEST(SemiFutureValid, ThenRunOnTransfersValid) {
    assertSemiFutureTransfersValid([](auto&& fut) {
        auto exec = InlineQueuedCountingExecutor::make();
        return std::move(fut).thenRunOn(exec);
    });
}

TEST(SemiFutureValid, IgnoreValueTransfersValid) {
    assertSemiFutureTransfersValid([](auto&& fut) { return std::move(fut).ignoreValue(); });
}

TEST(SemiFutureValid, MoveTransfersValid) {
    assertSemiFutureTransfersValid([](auto&& fut) { return std::move(fut); });
}

TEST(SemiFutureValid, SemiTransfersValid) {
    assertSemiFutureTransfersValid([](auto&& fut) { return std::move(fut).semi(); });
}

TEST(SemiFutureValid, ShareTransfersValid) {
    assertSemiFutureTransfersValid([](auto&& fut) { return std::move(fut).share(); });
}

TEST(SemiFutureValid, UnsafeToInlineFutureTransfersValid) {
    assertSemiFutureTransfersValid(
        [](auto&& fut) { return std::move(fut).unsafeToInlineFuture(); });
}

/** Asserts that the SharedSemiFuture is still valid() after `func`. */
template <typename TestFunc>
void assertSharedSemiFutureValidAfter(const TestFunc& func) {
    FUTURE_SUCCESS_TEST([] { return 0; },
                        [func](auto&& fut) {
                            auto sharedFut = std::move(fut).share();
                            func(std::move(sharedFut));
                            ASSERT_TRUE(sharedFut.valid());
                        });
}

/**
 * Asserts that `func` returns a valid() future while making the input SharedSemiFuture non-valid().
 */
template <typename TestFunc>
void assertSharedSemiFutureTransfersValid(const TestFunc& func) {
    FUTURE_SUCCESS_TEST([] { return 0; },
                        [func](auto&& fut) {
                            auto sharedFut = std::move(fut).share();
                            auto otherFut = func(std::move(sharedFut));
                            ASSERT_FALSE(sharedFut.valid());
                            ASSERT_TRUE(otherFut.valid());
                        });
}

/** Asserts that `func` returns a valid() Future and keeps the input SharedSemiFuture valid(). */
template <typename TestFunc>
void assertSharedSemiFutureSplits(const TestFunc& func) {
    FUTURE_SUCCESS_TEST([] { return 0; },
                        [func](auto&& fut) {
                            auto sharedFut = std::move(fut).share();
                            auto otherFut = func(std::move(sharedFut));
                            ASSERT_TRUE(sharedFut.valid());
                            ASSERT_TRUE(otherFut.valid());
                        });
}

/** Passes an invalid SharedSemiFuture into `func`. To be used with DEATH_TEST. */
template <DoExecutorFuture doExecutorFuture = kDoExecutorFuture, typename TestFunc>
void callWithInvalidSharedSemiFuture(const TestFunc& func) {
    FUTURE_SUCCESS_TEST<doExecutorFuture>([] { return 0; },
                                          [func](auto&& fut) {
                                              auto sharedFut = std::move(fut).share();
                                              auto otherSharedFut = std::move(sharedFut);
                                              (void)func(std::move(sharedFut));
                                          });
}

TEST(SharedSemiFutureValid, ValidAfterGetLvalue) {
    assertSharedSemiFutureValidAfter([](auto&& fut) { [[maybe_unused]] auto val = fut.get(); });
}

TEST(SharedSemiFutureValid, ValidAfterGetConstLvalue) {
    assertSharedSemiFutureValidAfter(
        [](const auto& fut) { [[maybe_unused]] auto val = fut.get(); });
}

TEST(SharedSemiFutureValid, ValidAfterGetNoThrowLvalue) {
    assertSharedSemiFutureValidAfter(
        [](auto&& fut) { [[maybe_unused]] auto val = fut.getNoThrow(); });
}

TEST(SharedSemiFutureValid, ValidAfterGetNoThrowConstLvalue) {
    assertSharedSemiFutureValidAfter(
        [](const auto& fut) { [[maybe_unused]] auto val = fut.getNoThrow(); });
}

TEST(SharedSemiFutureValid, ValidAfterWait) {
    assertSharedSemiFutureValidAfter([](auto&& fut) { fut.wait(); });
}

TEST(SharedSemiFutureValid, ValidAfterWaitNoThrow) {
    assertSharedSemiFutureValidAfter(
        [](auto&& fut) { [[maybe_unused]] auto status = fut.waitNoThrow(); });
}

TEST(SharedSemiFutureValid, ValidAfterThenRunOn) {
    assertSharedSemiFutureSplits([](auto&& fut) {
        auto exec = InlineQueuedCountingExecutor::make();
        return fut.thenRunOn(exec);
    });
}

DEATH_TEST(SharedSemiFutureValid, ThenRunOnCrashesOnInvalidFuture, "Invariant failure") {
    callWithInvalidSharedSemiFuture([](auto&& fut) {
        auto exec = InlineQueuedCountingExecutor::make();
        return fut.thenRunOn(exec);
    });
}

TEST(SharedSemiFutureValid, MoveTransfersValid) {
    assertSharedSemiFutureTransfersValid([](auto&& fut) { return std::move(fut); });
}

TEST(SharedSemiFutureValid, SemiRetainsValid) {
    assertSharedSemiFutureSplits([](auto&& fut) { return std::move(fut).semi(); });
}

DEATH_TEST(SharedSemiFutureValid, SemiCrashesOnInvalidFuture, "Invariant failure") {
    callWithInvalidSharedSemiFuture([](auto&& fut) { return std::move(fut).semi(); });
}

TEST(SharedSemiFutureValid, SplitRetainsValid) {
    assertSharedSemiFutureSplits([](auto&& fut) { return std::move(fut).split(); });
}

DEATH_TEST(SharedSemiFutureValid, SplitCrashesOnInvalidFuture, "Invariant failure") {
    callWithInvalidSharedSemiFuture([](auto&& fut) { return std::move(fut).split(); });
}

TEST(SharedSemiFutureValid, UnsafeToInlineFutureRetainsValid) {
    assertSharedSemiFutureSplits([](auto&& fut) { return std::move(fut).unsafeToInlineFuture(); });
}

/*
 * Handles the case around an interrupted get() operation. We expect that the Future remains valid
 * up until the point where the value is made available. I.e. a continuation can be chained off of
 * a Future whose get() has been interrupted because the caller never gets access to the value.
 */
TEST(FutureValid, InterruptedGetValidity) {
    FUTURE_SUCCESS_TEST([] { return 0; },
                        [](auto&& fut) {
                            const auto exec = InlineRecursiveCountingExecutor::make();
                            DummyInterruptible dummyInterruptible;

                            auto res = std::move(fut).getNoThrow(&dummyInterruptible);

                            if (!res.isOK()) {
                                ASSERT_EQ(res.getStatus(), ErrorCodes::Interrupted);
                                ASSERT_TRUE(fut.valid());
                            } else {
                                ASSERT_FALSE(fut.valid());
                            }
                        });
}
TEST(SemiFutureValid, InterruptedGetValidity) {
    FUTURE_SUCCESS_TEST([] { return 0; },
                        [](auto&& fut) {
                            auto semiFut = std::move(fut).semi();
                            const auto exec = InlineRecursiveCountingExecutor::make();
                            DummyInterruptible dummyInterruptible;

                            auto res = std::move(semiFut).getNoThrow(&dummyInterruptible);

                            if (!res.isOK()) {
                                ASSERT_EQ(res.getStatus(), ErrorCodes::Interrupted);
                                ASSERT_TRUE(semiFut.valid());
                            } else {
                                ASSERT_FALSE(semiFut.valid());
                            }
                        });
}

}  // namespace
}  // namespace mongo
