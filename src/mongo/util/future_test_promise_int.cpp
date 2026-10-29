// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/future_test_utils.h"

#include <memory>
#include <type_traits>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>

namespace mongo {
namespace {

static_assert(!canSetFrom<Promise<int>, int>, "Use Promise<T>::emplaceValue(...) instead");
static_assert(!canSetFrom<Promise<int>, Status>, "Use Promise<T>::setError(...) instead");
static_assert(canSetFrom<Promise<int>, StatusWith<int>>);
static_assert(canSetFrom<Promise<int>, Future<int>>);

TEST(Promise, Success_setFrom_future) {
    FUTURE_SUCCESS_TEST<kNoExecutorFuture_needsPromiseSetFrom>(
        [] { return 1; },
        [](/*Future<int>*/ auto&& fut) {
            auto pf = makePromiseFuture<int>();
            pf.promise.setFrom(std::move(fut));
            ASSERT_EQ(std::move(pf.future).get(), 1);
        });
}

TEST(Promise, Success_setFrom_statusWith) {
    auto pf = makePromiseFuture<int>();
    pf.promise.setFrom(StatusWith<int>(3));
    ASSERT_EQ(std::move(pf.future).getNoThrow(), 3);
}

TEST(Promise, Fail_setFrom_future) {
    FUTURE_FAIL_TEST<int, kNoExecutorFuture_needsPromiseSetFrom>([](/*Future<int>*/ auto&& fut) {
        auto pf = makePromiseFuture<int>();
        pf.promise.setFrom(std::move(fut));
        ASSERT_THROWS_failStatus(std::move(pf.future).get());
    });
}

TEST(Promise, Fail_setFrom_statusWith_error) {
    auto pf = makePromiseFuture<int>();
    pf.promise.setFrom(StatusWith<int>(failStatus()));
    ASSERT_THROWS_failStatus(std::move(pf.future).get());
}

TEST(Promise, Success_setWith_value) {
    auto pf = makePromiseFuture<int>();
    pf.promise.setWith([&] { return 1; });
    ASSERT_EQ(std::move(pf.future).get(), 1);
}

TEST(Promise, Fail_setWith_throw) {
    auto pf = makePromiseFuture<int>();
    pf.promise.setWith([&] {
        uassertStatusOK(failStatus());
        return 1;
    });
    ASSERT_THROWS_failStatus(std::move(pf.future).get());
}

TEST(Promise, Success_setWith_StatusWith) {
    auto pf = makePromiseFuture<int>();
    pf.promise.setWith([&] { return StatusWith<int>(1); });
    ASSERT_EQ(std::move(pf.future).get(), 1);
}

TEST(Promise, Fail_setWith_StatusWith) {
    auto pf = makePromiseFuture<int>();
    pf.promise.setWith([&] { return StatusWith<int>(failStatus()); });
    ASSERT_THROWS_failStatus(std::move(pf.future).get());
}

TEST(Promise, Success_setWith_Future) {
    FUTURE_SUCCESS_TEST<kNoExecutorFuture_needsPromiseSetFrom>(
        [] { return 1; },
        [](/*Future<int>*/ auto&& fut) {
            auto pf = makePromiseFuture<int>();
            pf.promise.setWith([&] { return std::move(fut); });
            ASSERT_EQ(std::move(pf.future).get(), 1);
        });
}

TEST(Promise, Fail_setWith_Future) {
    FUTURE_FAIL_TEST<int, kNoExecutorFuture_needsPromiseSetFrom>([](/*Future<int>*/ auto&& fut) {
        auto pf = makePromiseFuture<int>();
        pf.promise.setWith([&] { return std::move(fut); });
        ASSERT_THROWS_failStatus(std::move(pf.future).get());
    });
}

TEST(Promise, MoveAssignBreaksPromise) {
    auto pf = makePromiseFuture<int>();
    pf.promise = Promise<int>();  // This should break the promise.
    ASSERT_THROWS_CODE(std::move(pf.future).get(), DBException, ErrorCodes::BrokenPromise);
}

TEST(Promise, MoveAssignedPromiseIsTheSameAsTheOldOne) {
    const int kResult = 11;
    auto pf = makePromiseFuture<int>();
    auto promise = std::move(pf.promise);
    promise.emplaceValue(kResult);
    ASSERT_EQ(std::move(pf.future).get(), kResult);
}

}  // namespace
}  // namespace mongo
