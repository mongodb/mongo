// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/util/retry.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;
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
        "RetryOnWithStateTest"sv, initialThrowsRemaining, kMaxRetries, fn, onError);

    ASSERT_EQ(result, 42);
    // 2 throws + 1 success = 3 total attempts.
    ASSERT_EQ(numCalls, 3);
    ASSERT_EQ(numOnErrorCalls, 2);
    // onError should have seen the state after the last throw.
    ASSERT_EQ(lastObservedThrowsRemaining, 0);
}

TEST(RetryOnWithStateMultiTest, MultipleErrorCodesRetryWithState) {
    constexpr auto kMaxRetries = 5;
    auto numCalls = 0;
    struct State {
        int badValueCount = 0;
        int namespaceNotFoundCount = 0;
    };
    State initialState{};
    auto throwsRemaining = 3;
    int finalBadValueCount = 0;
    int finalNamespaceNotFoundCount = 0;

    auto fn = [&](State& state) {
        ++numCalls;
        if (throwsRemaining > 0) {
            if (--throwsRemaining % 2 == 0) {
                uasserted(ErrorCodes::BadValue, "BadValue error");
            } else {
                uasserted(ErrorCodes::NamespaceNotFound, "NamespaceNotFound error");
            }
        }
        // Capture final state values from inside the function, because the retryOn logic passes
        // state by value, not by reference.
        finalBadValueCount = state.badValueCount;
        finalNamespaceNotFoundCount = state.namespaceNotFoundCount;
        return 42;
    };

    auto onError1 = [&](ExceptionFor<ErrorCodes::BadValue>& ex, State& state) {
        ++state.badValueCount;
    };
    auto onError2 = [&](ExceptionFor<ErrorCodes::NamespaceNotFound>& ex, State& state) {
        ++state.namespaceNotFoundCount;
    };

    auto result = retryOnWithState("StateUpdateTest"sv,
                                   initialState,
                                   kMaxRetries,
                                   fn,
                                   makeErrorHandler<ErrorCodes::BadValue>(onError1),
                                   makeErrorHandler<ErrorCodes::NamespaceNotFound>(onError2));
    ASSERT_EQ(result, 42);
    // We expect the function to run 4 times - once for each of the 3 errors, and once for success.
    ASSERT_EQ(numCalls, 4);
    ASSERT_EQ(finalBadValueCount, 2);
    ASSERT_EQ(finalNamespaceNotFoundCount, 1);
}

TEST(RetryOnWithStateMultiTest, ThrowsAfterExhaustingMaxRetries) {
    constexpr auto kMaxRetries = 2;
    auto numCalls = 0;

    auto fn = [&](int& state) {
        ++numCalls;
        uasserted(ErrorCodes::BadValue, "mocking bad value error");
        return 42;
    };

    auto onError1 = [&](ExceptionFor<ErrorCodes::BadValue>& ex, int& state) {
    };

    ASSERT_THROWS_CODE(retryOnWithState("ExhaustRetriesTest"sv,
                                        0,
                                        kMaxRetries,
                                        fn,
                                        makeErrorHandler<ErrorCodes::BadValue>(onError1)),
                       DBException,
                       ErrorCodes::BadValue);
    ASSERT_EQ(numCalls, kMaxRetries + 1);
}

TEST(RetryOnWithStateMultiTest, HandlersAreNotCalledForMismatchedErrorCode) {
    constexpr auto kMaxRetries = 5;
    auto numCalls = 0;
    auto handlerCalls = 0;

    auto fn = [&](int& state) {
        ++numCalls;
        uasserted(ErrorCodes::InternalError, "unhandled error code");
        return 42;
    };

    auto onError1 = [&](ExceptionFor<ErrorCodes::BadValue>& ex, int& state) {
        ++handlerCalls;
    };
    auto onError2 = [&](ExceptionFor<ErrorCodes::NamespaceNotFound>& ex, int& state) {
        ++handlerCalls;
    };

    ASSERT_THROWS_CODE(retryOnWithState("UnhandledErrorTest"sv,
                                        0,
                                        kMaxRetries,
                                        fn,
                                        makeErrorHandler<ErrorCodes::BadValue>(onError1),
                                        makeErrorHandler<ErrorCodes::NamespaceNotFound>(onError2)),
                       DBException,
                       ErrorCodes::InternalError);
    ASSERT_EQ(numCalls, 1);
    ASSERT_EQ(handlerCalls, 0);
}

}  // namespace
}  // namespace mongo
