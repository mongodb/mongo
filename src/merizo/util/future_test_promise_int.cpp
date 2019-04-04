/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#include "merizo/platform/basic.h"

#include "merizo/util/future.h"

#include "merizo/stdx/thread.h"
#include "merizo/unittest/death_test.h"
#include "merizo/unittest/unittest.h"

#include "merizo/util/future_test_utils.h"

namespace merizo {
namespace {

TEST(Promise, Success_setFrom) {
    FUTURE_SUCCESS_TEST([] { return 1; },
                        [](Future<int>&& fut) {
                            auto pf = makePromiseFuture<int>();
                            pf.promise.setFrom(std::move(fut));
                            ASSERT_EQ(std::move(pf.future).get(), 1);
                        });
}

TEST(Promise, Fail_setFrom) {
    FUTURE_FAIL_TEST<int>([](Future<int>&& fut) {
        auto pf = makePromiseFuture<int>();
        pf.promise.setFrom(std::move(fut));
        ASSERT_THROWS_failStatus(std::move(pf.future).get());
    });
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
    FUTURE_SUCCESS_TEST([] { return 1; },
                        [](Future<int>&& fut) {
                            auto pf = makePromiseFuture<int>();
                            pf.promise.setWith([&] { return std::move(fut); });
                            ASSERT_EQ(std::move(pf.future).get(), 1);
                        });
}

TEST(Promise, Fail_setWith_Future) {
    FUTURE_FAIL_TEST<int>([](Future<int>&& fut) {
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
}  // namespace merizo
