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
 * TODO(SERVER-66036): Expand testing to ensure that `valid()` semantics are in line with "valid
 * usage" semantics.
 */

/**
 * These tests validate the postconditions of operations on the 4 future types:
 * - Future
 * - SemiFuture
 * - SharedSemiFuture
 * - ExecutorFuture
 *
 * TODO(SERVER-66036): use FUTURE_SUCCESS_TEST in all helpers for better coverage of ExecutorFuture.
 */

/** Asserts that the Future is still valid() after `func`. */
template <typename TestFunc>
void assertFutureValidAfter(const TestFunc& func) {
    FUTURE_SUCCESS_TEST([] { return 0; },
                        [func](auto&& fut) {
                            func(std::move(fut));
                            ASSERT_TRUE(fut.valid());
                        });
}

/** Asserts that `func` returns a valid() future while making the input Future non-valid(). */
template <DoExecutorFuture doExecutorFuture = kDoExecutorFuture, typename TestFunc>
void assertFutureTransfersValid(const TestFunc& func) {
    // TODO(SERVER-66036): use FUTURE_SUCCESS_TEST once moves from _immediate have the same
    // semantics as moves from SharedState
    auto [promise, fut] = makePromiseFuture<int>();
    promise.emplaceValue(0);
    auto otherFut = func(std::move(fut));
    ASSERT_FALSE(fut.valid());  // NOLINT
    ASSERT_TRUE(otherFut.valid());
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

TEST(FutureValid, ValidAfterGetNoThrowConstLvalue) {
    assertFutureValidAfter([](const auto& fut) { [[maybe_unused]] auto val = fut.getNoThrow(); });
}

TEST(FutureValid, ValidAfterWait) {
    assertFutureValidAfter([](auto&& fut) { fut.wait(); });
}

TEST(FutureValid, ValidAfterWaitNoThrow) {
    assertFutureValidAfter([](auto&& fut) { [[maybe_unused]] auto status = fut.waitNoThrow(); });
}

TEST(FutureValid, ThenRunOnTransfersValid) {
    assertFutureTransfersValid([](auto&& fut) {
        auto exec = InlineQueuedCountingExecutor::make();
        return std::move(fut).thenRunOn(exec);
    });
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
    // TODO(SERVER-66036): use FUTURE_SUCCESS_TEST once moves from _immediate have the same
    // semantics as moves from SharedState
    auto [promise, fut] = makePromiseFuture<int>();
    promise.emplaceValue(0);
    auto semiFut = std::move(fut).semi();
    auto otherFut = func(std::move(semiFut));
    ASSERT_FALSE(semiFut.valid());  // NOLINT
    ASSERT_TRUE(otherFut.valid());
}

// TODO SERVER-64948: this test is only needed if the lvalue& getter is kept around.
TEST(SemiFutureValid, ValidAfterGetLvalue) {
    assertSemiFutureValidAfter([](auto&& fut) { [[maybe_unused]] auto val = fut.get(); });
}

TEST(SemiFutureValid, ValidAfterGetConstLvalue) {
    assertSemiFutureValidAfter([](const auto& fut) { [[maybe_unused]] auto val = fut.get(); });
}

TEST(SemiFutureValid, ValidAfterGetNoThrowConstLvalue) {
    assertSemiFutureValidAfter(
        [](const auto& fut) { [[maybe_unused]] auto val = fut.getNoThrow(); });
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
    // TODO(SERVER-66036): use FUTURE_SUCCESS_TEST once moves from _immediate have the same
    // semantics as moves from SharedState
    auto [promise, fut] = makePromiseFuture<int>();
    promise.emplaceValue(0);
    auto sharedFut = std::move(fut).share();
    auto otherFut = func(std::move(sharedFut));
    ASSERT_FALSE(sharedFut.valid());  // NOLINT
    ASSERT_TRUE(otherFut.valid());
}

/** Asserts that `func` returns a valid() Future and keeps the input SharedSemiFuture valid(). */
template <typename TestFunc>
void assertSharedSemiFutureSplits(const TestFunc& func) {
    // TODO(SERVER-66036): use FUTURE_SUCCESS_TEST once moves from _immediate have the same
    // semantics as moves from SharedState
    auto [promise, fut] = makePromiseFuture<int>();
    promise.emplaceValue(0);
    auto sharedFut = std::move(fut).share();
    auto otherFut = func(std::move(sharedFut));
    ASSERT_TRUE(sharedFut.valid());  // NOLINT
    ASSERT_TRUE(otherFut.valid());
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

TEST(SharedSemiFutureValid, MoveTransfersValid) {
    assertSharedSemiFutureTransfersValid([](auto&& fut) { return std::move(fut); });
}

TEST(SharedSemiFutureValid, SemiRetainsValid) {
    assertSharedSemiFutureSplits([](auto&& fut) { return std::move(fut).semi(); });
}

TEST(SharedSemiFutureValid, SplitRetainsValid) {
    assertSharedSemiFutureSplits([](auto&& fut) { return std::move(fut).split(); });
}

TEST(SharedSemiFutureValid, UnsafeToInlineFutureRetainsValid) {
    assertSharedSemiFutureSplits([](auto&& fut) { return std::move(fut).unsafeToInlineFuture(); });
}

}  // namespace
}  // namespace mongo
