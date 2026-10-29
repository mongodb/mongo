// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
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

static_assert(!canSetFrom<Promise<void>, void>, "Use Promise<T>::emplaceValue() instead");
static_assert(canSetFrom<Promise<void>, Status>);
static_assert(canSetFrom<Promise<void>, Future<void>>);

TEST(Promise_void, Success_setFrom_future) {
    FUTURE_SUCCESS_TEST<kNoExecutorFuture_needsPromiseSetFrom>(
        [] {},
        [](/*Future<void>*/ auto&& fut) {
            // This intentionally doesn't work with ExecutorFuture.
            auto pf = makePromiseFuture<void>();
            pf.promise.setFrom(std::move(fut));
            ASSERT_OK(std::move(pf.future).getNoThrow());
        });
}

TEST(Promise_void, Success_setFrom_status) {
    auto pf = makePromiseFuture<void>();
    pf.promise.setFrom(Status::OK());
    ASSERT_OK(std::move(pf.future).getNoThrow());
}

TEST(Promise_void, Fail_setFrom_future) {
    FUTURE_FAIL_TEST<void, kNoExecutorFuture_needsPromiseSetFrom>([](/*Future<void>*/ auto&& fut) {
        auto pf = makePromiseFuture<void>();
        pf.promise.setFrom(std::move(fut));
        ASSERT_THROWS_failStatus(std::move(pf.future).get());
    });
}

TEST(Promise_void, Fail_setFrom_status) {
    auto pf = makePromiseFuture<void>();
    pf.promise.setFrom(failStatus());
    ASSERT_THROWS_failStatus(std::move(pf.future).get());
}

TEST(Promise_void, Success_setWith_value) {
    auto pf = makePromiseFuture<void>();
    pf.promise.setWith([&] {});
    ASSERT_OK(std::move(pf.future).getNoThrow());
}

TEST(Promise_void, Fail_setWith_throw) {
    auto pf = makePromiseFuture<void>();
    pf.promise.setWith([&] { uassertStatusOK(failStatus()); });
    ASSERT_THROWS_failStatus(std::move(pf.future).get());
}

TEST(Promise_void, Success_setWith_Status) {
    auto pf = makePromiseFuture<void>();
    pf.promise.setWith([&] { return Status::OK(); });
    ASSERT_OK(std::move(pf.future).getNoThrow());
}

TEST(Promise_void, Fail_setWith_Status) {
    auto pf = makePromiseFuture<void>();
    pf.promise.setWith([&] { return failStatus(); });
    ASSERT_THROWS_failStatus(std::move(pf.future).get());
}

TEST(Promise_void, Success_setWith_Future) {
    FUTURE_SUCCESS_TEST<kNoExecutorFuture_needsPromiseSetFrom>(
        [] {},
        [](/*Future<void>*/ auto&& fut) {
            auto pf = makePromiseFuture<void>();
            pf.promise.setWith([&] { return std::move(fut); });
            ASSERT_OK(std::move(pf.future).getNoThrow());
        });
}

TEST(Promise_void, Fail_setWith_Future) {
    FUTURE_FAIL_TEST<void, kNoExecutorFuture_needsPromiseSetFrom>([](/*Future<void>*/ auto&& fut) {
        auto pf = makePromiseFuture<void>();
        pf.promise.setWith([&] { return std::move(fut); });
        ASSERT_THROWS_failStatus(std::move(pf.future).get());
    });
}

TEST(Promise_void, MoveAssignBreaksPromise) {
    auto pf = makePromiseFuture<void>();
    pf.promise = Promise<void>();  // This should break the promise.
    ASSERT_THROWS_CODE(std::move(pf.future).get(), DBException, ErrorCodes::BrokenPromise);
}

TEST(Promise_void, MoveAssignedPromiseIsTheSameAsTheOldOne) {
    auto pf = makePromiseFuture<void>();
    auto promise = std::move(pf.promise);
    promise.setWith([] {});
    ASSERT_OK(std::move(pf.future).getNoThrow());
}

}  // namespace
}  // namespace mongo
