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
TEST(Executor_Future, Success_getAsync) {
    FUTURE_SUCCESS_TEST(
        [] {},
        [](/*Future<void>*/ auto&& fut) {
            auto exec = InlineCountingExecutor::make();
            auto pf = makePromiseFuture<void>();
            ExecutorFuture<void>(exec).thenRunOn(exec).getAsync([outside = std::move(pf.promise)](
                Status status) mutable {
                ASSERT_OK(status);
                outside.emplaceValue();
            });
            ASSERT_EQ(std::move(pf.future).getNoThrow(), Status::OK());
            ASSERT_EQ(exec->tasksRun.load(), 1);
        });
}

TEST(Executor_Future, Reject_getAsync) {
    FUTURE_SUCCESS_TEST(
        [] {},
        [](/*Future<void>*/ auto&& fut) {
            auto exec = RejectingExecutor::make();
            auto pf = makePromiseFuture<void>();
            std::move(fut).thenRunOn(exec).getAsync([promise = std::move(pf.promise)](
                Status status) mutable {
                promise.emplaceValue();  // shouldn't be run anyway.
                FAIL("how did I run!?!?!");
            });

            // Promise is destroyed without calling the callback.
            ASSERT_EQ(std::move(pf.future).getNoThrow(), ErrorCodes::BrokenPromise);
        });
}

TEST(Executor_Future, Success_then) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            auto exec = InlineCountingExecutor::make();
                            ASSERT_EQ(std::move(fut).thenRunOn(exec).then([]() { return 3; }).get(),
                                      3);
                            ASSERT_EQ(exec->tasksRun.load(), 1);
                        });
}
TEST(Executor_Future, Reject_then) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            auto exec = RejectingExecutor::make();
                            ASSERT_EQ(std::move(fut)
                                          .thenRunOn(exec)
                                          .then([]() {
                                              FAIL("where am I running?");
                                              return 42;
                                          })
                                          .getNoThrow(),
                                      ErrorCodes::ShutdownInProgress);
                        });
}

TEST(Executor_Future, Fail_then) {
    FUTURE_FAIL_TEST<void>([](/*Future<void>*/ auto&& fut) {
        auto exec = InlineCountingExecutor::make();
        ASSERT_EQ(std::move(fut)
                      .thenRunOn(exec)
                      .then([]() {
                          FAIL("then() callback was called");
                          return int();
                      })
                      .getNoThrow(),
                  failStatus());
        ASSERT_EQ(exec->tasksRun.load(), 0);
    });
}

TEST(Executor_Future, Success_onError) {
    FUTURE_SUCCESS_TEST([] { return 3; },
                        [](/*Future<int>*/ auto&& fut) {
                            auto exec = InlineCountingExecutor::make();
                            ASSERT_EQ(std::move(fut)
                                          .thenRunOn(exec)
                                          .onError([](Status&&) {
                                              FAIL("onError() callback was called");
                                              return 42;
                                          })
                                          .get(),
                                      3);
                            ASSERT_EQ(exec->tasksRun.load(), 0);
                        });
}

TEST(Executor_Future, Fail_onErrorSimple) {
    FUTURE_FAIL_TEST<int>([](/*Future<int>*/ auto&& fut) {
        auto exec = InlineCountingExecutor::make();
        ASSERT_EQ(std::move(fut)
                      .thenRunOn(exec)
                      .onError([](Status s) {
                          ASSERT_EQ(s, failStatus());
                          return 3;
                      })
                      .get(),
                  3);
        ASSERT_EQ(exec->tasksRun.load(), 1);
    });
}

TEST(Executor_Future, Fail_onErrorCode_OtherCode) {
    FUTURE_FAIL_TEST<void>([](/*Future<void>*/ auto&& fut) {
        auto exec = InlineCountingExecutor::make();
        ASSERT_EQ(
            std::move(fut)
                .thenRunOn(exec)
                .template onError<ErrorCodes::BadValue>([](Status s) { FAIL("wrong code, sir"); })
                .getNoThrow(),
            failStatus());
        ASSERT_EQ(exec->tasksRun.load(), 0);
    });
}

TEST(Executor_Future, Success_then_onError_onError_then) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            auto exec = InlineCountingExecutor::make();
                            ASSERT_EQ(
                                std::move(fut)
                                    .thenRunOn(exec)
                                    .then([] { return failStatus(); })
                                    .onError([](Status s) { ASSERT_EQ(s, failStatus()); })
                                    .onError([](Status) { FAIL("how did you get this number?"); })
                                    .then([] { return 3; })
                                    .get(),
                                3);

                            // 1 would also be valid if we did the optimization to not reschedule if
                            // running on the same executor.
                            ASSERT_EQ(exec->tasksRun.load(), 3);
                        });
}

TEST(Executor_Future, Success_reject_recoverToFallback) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            auto rejecter = RejectingExecutor::make();
                            auto accepter = InlineCountingExecutor::make();

                            auto res = std::move(fut)
                                           .thenRunOn(rejecter)
                                           .then([] { FAIL("then()"); })
                                           .onError([](Status) { FAIL("onError()"); })
                                           .onCompletion([](Status) { FAIL("onCompletion()"); })
                                           .thenRunOn(accepter)
                                           .then([] {
                                               FAIL("error?");
                                               return 42;
                                           })
                                           .onError([](Status s) {
                                               ASSERT_EQ(s, ErrorCodes::ShutdownInProgress);
                                               return 3;
                                           })
                                           .get();
                            ASSERT_EQ(res, 3);

                            ASSERT_EQ(accepter->tasksRun.load(), 1);
                        });
}
}  // namespace
}  // namespace mongo
