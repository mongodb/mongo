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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
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
