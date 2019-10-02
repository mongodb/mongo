/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/stdx/thread.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include "mongo/util/future_test_utils.h"

namespace mongo {
namespace {

// Test FutureContinuationResult<Func, Args...>:
static_assert(std::is_same<FutureContinuationResult<std::function<void()>>, void>::value);
static_assert(std::is_same<FutureContinuationResult<std::function<Status()>>, void>::value);
static_assert(std::is_same<FutureContinuationResult<std::function<Future<void>()>>, void>::value);
static_assert(std::is_same<FutureContinuationResult<std::function<int()>>, int>::value);
static_assert(std::is_same<FutureContinuationResult<std::function<StatusWith<int>()>>, int>::value);
static_assert(std::is_same<FutureContinuationResult<std::function<Future<int>()>>, int>::value);
static_assert(std::is_same<FutureContinuationResult<std::function<int(bool)>, bool>, int>::value);

template <typename T>
auto overloadCheck(T) -> FutureContinuationResult<std::function<std::true_type(bool)>, T>;
auto overloadCheck(...) -> std::false_type;

static_assert(decltype(overloadCheck(bool()))::value);          // match.
static_assert(!decltype(overloadCheck(std::string()))::value);  // SFINAE-failure.

// Don't allow banned conversions:
static_assert(!std::is_constructible_v<SemiFuture<int>, SemiFuture<double>>);
static_assert(!std::is_constructible_v<SemiFuture<int>, SemiFuture<void>>);
static_assert(!std::is_constructible_v<SemiFuture<void>, SemiFuture<int>>);
static_assert(!std::is_constructible_v<SemiFuture<void>, SemiFuture<double>>);

static_assert(!std::is_constructible_v<Future<int>, Future<double>>);
static_assert(!std::is_constructible_v<Future<int>, Future<void>>);
static_assert(!std::is_constructible_v<Future<void>, Future<int>>);
static_assert(!std::is_constructible_v<Future<void>, Future<double>>);

static_assert(!std::is_constructible_v<Future<int>, SemiFuture<int>>);
static_assert(!std::is_constructible_v<Future<int>, SemiFuture<double>>);
static_assert(!std::is_constructible_v<Future<int>, SemiFuture<void>>);
static_assert(!std::is_constructible_v<Future<void>, SemiFuture<int>>);
static_assert(!std::is_constructible_v<Future<void>, SemiFuture<double>>);
static_assert(!std::is_constructible_v<Future<void>, SemiFuture<void>>);

// This isn't currently allowed for implementation reasons, but it isn't fundamentally undesirable.
// We may want to allow it at some point.
#ifndef _MSC_VER
// https://developercommunity.visualstudio.com/content/problem/507821/is-constructible.html
static_assert(!std::is_constructible_v<SemiFuture<int>, Future<int>>);
#endif

// Check the return types of then-chaining a Future with a function that returns a SemiFuture or an
// ExecutorFuture.
static_assert(std::is_same_v<decltype(Future<void>().then(std::function<SemiFuture<void>()>())),
                             SemiFuture<void>>);
static_assert(std::is_same_v<decltype(Future<int>().then(std::function<SemiFuture<void>(int)>())),
                             SemiFuture<void>>);
static_assert(std::is_same_v<decltype(Future<void>().then(std::function<SemiFuture<int>()>())),
                             SemiFuture<int>>);
static_assert(std::is_same_v<decltype(Future<int>().then(std::function<SemiFuture<int>(int)>())),
                             SemiFuture<int>>);
static_assert(std::is_same_v<  //
              decltype(Future<void>().then(std::function<ExecutorFuture<void>()>())),
              SemiFuture<void>>);
static_assert(std::is_same_v<  //
              decltype(Future<int>().then(std::function<ExecutorFuture<void>(int)>())),
              SemiFuture<void>>);
static_assert(std::is_same_v<  //
              decltype(Future<void>().then(std::function<ExecutorFuture<int>()>())),
              SemiFuture<int>>);
static_assert(std::is_same_v<  //
              decltype(Future<int>().then(std::function<ExecutorFuture<int>(int)>())),
              SemiFuture<int>>);

static_assert(std::is_same_v<  //
              decltype(Future<void>().then(std::function<SharedSemiFuture<void>()>())),
              SemiFuture<void>>);
static_assert(std::is_same_v<  //
              decltype(Future<int>().then(std::function<SharedSemiFuture<void>(int)>())),
              SemiFuture<void>>);
static_assert(std::is_same_v<  //
              decltype(Future<void>().then(std::function<SharedSemiFuture<int>()>())),
              SemiFuture<int>>);
static_assert(std::is_same_v<  //
              decltype(Future<int>().then(std::function<SharedSemiFuture<int>(int)>())),
              SemiFuture<int>>);


// Check deduction guides:
static_assert(std::is_same_v<decltype(SemiFuture(0)), SemiFuture<int>>);
static_assert(std::is_same_v<decltype(SemiFuture(StatusWith(0))), SemiFuture<int>>);
static_assert(std::is_same_v<decltype(Future(0)), Future<int>>);
static_assert(std::is_same_v<decltype(Future(StatusWith(0))), Future<int>>);
static_assert(std::is_same_v<decltype(SharedSemiFuture(0)), SharedSemiFuture<int>>);
static_assert(std::is_same_v<decltype(SharedSemiFuture(StatusWith(0))), SharedSemiFuture<int>>);

static_assert(std::is_same_v<  //
              decltype(ExecutorFuture(ExecutorPtr())),
              ExecutorFuture<void>>);
static_assert(std::is_same_v<  //
              decltype(ExecutorFuture(ExecutorPtr(), 0)),
              ExecutorFuture<int>>);
static_assert(std::is_same_v<  //
              decltype(ExecutorFuture(ExecutorPtr(), StatusWith(0))),
              ExecutorFuture<int>>);

template <template <typename...> typename FutureLike, typename... Args>
auto ctadCheck(int) -> decltype(FutureLike(std::declval<Args>()...), std::true_type());
template <template <typename...> typename FutureLike, typename... Args>
std::false_type ctadCheck(...);

// Future() and Future(status) are both banned even though they could resolve to Future<void>
// It just seems too likely to lead to mistakes.
static_assert(!decltype(ctadCheck<Future>(0))::value);
static_assert(!decltype(ctadCheck<Future, Status>(0))::value);

static_assert(!decltype(ctadCheck<Future, SemiFuture<int>>(0))::value);
static_assert(!decltype(ctadCheck<SemiFuture, Future<int>>(0))::value);
static_assert(!decltype(ctadCheck<ExecutorFuture, SemiFuture<int>>(0))::value);
static_assert(decltype(ctadCheck<Future, Future<int>>(0))::value);
static_assert(decltype(ctadCheck<SemiFuture, SemiFuture<int>>(0))::value);
static_assert(decltype(ctadCheck<ExecutorFuture, ExecutorFuture<int>>(0))::value);

// sanity checks of ctadCheck: (watch those watchmen!)
static_assert(!decltype(ctadCheck<std::basic_string>(0))::value);
static_assert(decltype(ctadCheck<std::basic_string, std::string>(0))::value);
static_assert(decltype(ctadCheck<Future, int>(0))::value);
static_assert(decltype(ctadCheck<Future, StatusWith<int>>(0))::value);

// This is the motivating case for SharedStateBase::isJustForContinuation. Without that logic, there
// would be a long chain of SharedStates, growing longer with each recursion. That logic exists to
// limit it to a fixed-size chain.
TEST(Future_EdgeCases, looping_onError) {
    int tries = 10;
    std::function<Future<int>()> read = [&] {
        return async([&] {
                   uassert(ErrorCodes::BadValue, "", --tries == 0);
                   return tries;
               })
            .onError([&](Status) { return read(); });
    };
    ASSERT_EQ(read().get(), 0);
}

// This tests for a bug in an earlier implementation of isJustForContinuation. Due to an off-by-one,
// it would replace the "then" continuation's SharedState. A different type is used for the return
// from then to cause it to fail a checked_cast close to the bug in debug builds.
TEST(Future_EdgeCases, looping_onError_with_then) {
    int tries = 10;
    std::function<Future<int>()> read = [&] {
        return async([&] {
                   uassert(ErrorCodes::BadValue, "", --tries == 0);
                   return tries;
               })
            .onError([&](Status) { return read(); });
    };
    ASSERT_EQ(read().then([](int x) { return x + 0.5; }).get(), 0.5);
}

TEST(Future_EdgeCases, interrupted_wait_then_get) {
    DummyInterruptable dummyInterruptable;

    auto pf = makePromiseFuture<void>();
    ASSERT_EQ(pf.future.waitNoThrow(&dummyInterruptable), ErrorCodes::Interrupted);
    ASSERT_EQ(pf.future.getNoThrow(&dummyInterruptable), ErrorCodes::Interrupted);

    pf.promise.emplaceValue();
    pf.future.get();
}

TEST(Future_EdgeCases, interrupted_wait_then_get_with_bgthread) {
    DummyInterruptable dummyInterruptable;

    // Note, this is intentionally somewhat racy. async() is defined to sleep 100ms before running
    // the function so it will generally test blocking in the final get(). Under TSAN the sleep is
    // removed to allow it to find more interesting interleavings, and give it a better chance at
    // detecting data races.
    auto future = async([] {});

    auto res = future.waitNoThrow(&dummyInterruptable);
    if (!res.isOK()) {
        ASSERT_EQ(res, ErrorCodes::Interrupted);
    }

    res = future.getNoThrow(&dummyInterruptable);
    if (!res.isOK()) {
        ASSERT_EQ(res, ErrorCodes::Interrupted);
    }

    future.get();
}

TEST(Future_EdgeCases, interrupted_wait_then_then) {
    DummyInterruptable dummyInterruptable;

    auto pf = makePromiseFuture<void>();
    ASSERT_EQ(pf.future.waitNoThrow(&dummyInterruptable), ErrorCodes::Interrupted);
    auto fut2 = std::move(pf.future).then([] {});

    pf.promise.emplaceValue();
    fut2.get();
}

TEST(Future_EdgeCases, interrupted_wait_then_then_with_bgthread) {
    DummyInterruptable dummyInterruptable;

    // Note, this is intentionally somewhat racy. async() is defined to sleep 100ms before running
    // the function so it will generally test blocking in the final get(). Under TSAN the sleep is
    // removed to allow it to find more interesting interleavings, and give it a better chance at
    // detecting data races.
    auto future = async([] {});

    auto res = future.waitNoThrow(&dummyInterruptable);
    if (!res.isOK()) {
        ASSERT_EQ(res, ErrorCodes::Interrupted);
    }

    res = future.getNoThrow(&dummyInterruptable);
    if (!res.isOK()) {
        ASSERT_EQ(res, ErrorCodes::Interrupted);
    }

    std::move(future).then([] {}).get();
}

TEST(Future_EdgeCases, Racing_SharedPromise_getFuture_and_emplaceValue) {
    SharedPromise<void> sp;
    std::vector<Future<void>> futs;
    futs.reserve(30);

    // Note, this is intentionally somewhat racy. async() is defined to sleep 100ms before running
    // the function so the first batch of futures will generally block before getting the value is
    // emplaced, and the second batch will happen around the same time. In all cases the final batch
    // happen after the emplaceValue(), but roughly at the same time. Under TSAN the sleep is
    // removed to allow it to find more interesting interleavings, and give it a better chance at
    // detecting data races.

    for (int i = 0; i < 10; i++) {
        futs.push_back(async([&] { sp.getFuture().get(); }));
    }

    sleepIfShould();

    for (int i = 0; i < 10; i++) {
        futs.push_back(async([&] { sp.getFuture().get(); }));
    }

    sleepIfShould();

    sp.emplaceValue();

    for (int i = 0; i < 10; i++) {
        futs.push_back(async([&] { sp.getFuture().get(); }));
    }

    for (auto& fut : futs) {
        fut.get();
    }
}

TEST(Future_EdgeCases, Racing_SharedPromise_getFuture_and_setError) {
    SharedPromise<void> sp;
    std::vector<Future<void>> futs;
    futs.reserve(30);

    // Note, this is intentionally somewhat racy. async() is defined to sleep 100ms before running
    // the function so the first batch of futures will generally block before getting the value is
    // emplaced, and the second batch will happen around the same time. In all cases the final batch
    // happen after the emplaceValue(), but roughly at the same time. Under TSAN the sleep is
    // removed to allow it to find more interesting interleavings, and give it a better chance at
    // detecting data races.

    for (int i = 0; i < 10; i++) {
        futs.push_back(async([&] { sp.getFuture().get(); }));
    }

    sleepIfShould();

    for (int i = 0; i < 10; i++) {
        futs.push_back(async([&] { sp.getFuture().get(); }));
    }

    sleepIfShould();

    sp.setError(failStatus());

    for (int i = 0; i < 10; i++) {
        futs.push_back(async([&] { sp.getFuture().get(); }));
    }

    for (auto& fut : futs) {
        ASSERT_EQ(fut.getNoThrow(), failStatus());
    }
}

TEST(Future_EdgeCases, SharedPromise_CompleteWithUnreadyFuture) {
    SharedSemiFuture<void> sf;
    auto [promise, future] = makePromiseFuture<void>();

    {
        SharedPromise<void> sp;
        sf = sp.getFuture();
        sp.setFrom(std::move(future));
    }

    ASSERT(!sf.isReady());

    promise.emplaceValue();
    ASSERT(sf.isReady());
    sf.get();
}

// Make sure we actually die if someone throws from the getAsync callback.
//
// With gcc 5.8 we terminate, but print "terminate() called. No exception is active". This works in
// clang and gcc 7, so hopefully we can change the death-test search string to "die die die!!!" when
// we upgrade the toolchain.
DEATH_TEST(Future_EdgeCases, Success_getAsync_throw, "terminate() called") {
    Future<void>::makeReady().getAsync(
        [](Status) { uasserted(ErrorCodes::BadValue, "die die die!!!"); });
}

}  // namespace
}  // namespace mongo
