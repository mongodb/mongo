/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/util/retry.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
TEST(RetryOnTest, RetriesUntilSuccessUpToMax) {
    constexpr auto kMaxRetries = 3;
    auto numCalls = 0;
    auto throwsRemaining = 2;

    auto fn = [&]() {
        ++numCalls;
        if (throwsRemaining > 0) {
            --throwsRemaining;
            uasserted(ErrorCodes::BadValue, "mocking bad value error response for test");
        }
        return 42;
    };

    auto result = retryOn<ErrorCodes::BadValue>("RetryOnTest", fn, kMaxRetries);

    ASSERT_EQ(result, 42);
    // 2 throws + 1 success = 3 total attempts.
    ASSERT_EQ(numCalls, 3);
    ASSERT_EQ(throwsRemaining, 0);
}

TEST(RetryOnTest, ThrowsAfterExhaustingMaxRetries) {
    constexpr auto kMaxRetries = 3;
    auto numCalls = 0;

    auto fn = [&]() -> int {
        ++numCalls;
        uasserted(ErrorCodes::BadValue, "mocking bad value error response for test");
    };

    ASSERT_THROWS_CODE(retryOn<ErrorCodes::BadValue>("RetryOnTest", fn, kMaxRetries),
                       DBException,
                       ErrorCodes::BadValue);

    // We should attempt exactly (maxRetries + 1) times before propagating.
    ASSERT_EQ(numCalls, kMaxRetries + 1);
}

TEST(RetryOnTest, DoesNotRetryOnDifferentErrorCode) {
    constexpr auto kMaxRetries = 3;
    auto numCalls = 0;

    auto fn = [&]() -> int {
        ++numCalls;
        uasserted(ErrorCodes::BadValue, "mocking bad value error response for test");
    };

    ASSERT_THROWS_CODE(retryOn<ErrorCodes::NamespaceNotFound>("RetryOnTest", fn, kMaxRetries),
                       DBException,
                       ErrorCodes::BadValue);

    // Because the error code doesn't match E, we should not retry at all.
    ASSERT_EQ(numCalls, 1);
}

TEST(RetryOnTest, ZeroMaxRetriesMeansSingleAttempt) {
    constexpr auto kMaxRetries = 0;
    auto numCalls = 0;

    auto fn = [&]() {
        ++numCalls;
        uasserted(ErrorCodes::BadValue, "mocking bad value error response for test");
        return 0;
    };

    ASSERT_THROWS_CODE(retryOn<ErrorCodes::BadValue>("RetryOnTest", fn, kMaxRetries),
                       DBException,
                       ErrorCodes::BadValue);
    // One attempt, zero retries.
    ASSERT_EQ(numCalls, 1);
}

TEST(RetryOnTest, OnErrorInvokedForEachRetryWithState) {
    constexpr auto kMaxRetries = 5;
    auto numCalls = 0;
    auto numOnErrorCalls = 0;
    // Initial state: number of throws remaining before success.
    auto initialThrowsRemaining = 2;

    auto lastObservedThrowsRemaining = -1;

    auto fn = [&](int& throwsRemaining) {
        ++numCalls;
        if (throwsRemaining > 0) {
            --throwsRemaining;
            uasserted(ErrorCodes::BadValue,
                      "mocking bad value error response for onErrorWithState test");
        }
        return 42;
    };

    auto onError = [&](ExceptionFor<ErrorCodes::BadValue>& ex, int& throwsRemaining) {
        ++numOnErrorCalls;
        lastObservedThrowsRemaining = throwsRemaining;
    };

    auto result = retryOnWithState<ErrorCodes::BadValue>(
        "RetryOnWithStateTest"_sd, initialThrowsRemaining, kMaxRetries, fn, onError);

    ASSERT_EQ(result, 42);
    // 2 throws + 1 success = 3 total attempts.
    ASSERT_EQ(numCalls, 3);
    ASSERT_EQ(numOnErrorCalls, 2);
    // onError should have seen the state after the last throw.
    ASSERT_EQ(lastObservedThrowsRemaining, 0);
}
}  // namespace
}  // namespace mongo
