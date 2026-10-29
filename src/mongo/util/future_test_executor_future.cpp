// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/future.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/executor_test_util.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/future_test_utils.h"

#include <atomic>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {

TEST(Executor_Future, Success_getAsync) {
    FUTURE_SUCCESS_TEST([] {},
                        [](auto&& fut) {
                            auto exec = InlineQueuedCountingExecutor::make();
                            auto pf = makePromiseFuture<void>();
                            ExecutorFuture<void>(exec).thenRunOn(exec).getAsync(
                                [outside = std::move(pf.promise)](Status status) mutable {
                                    ASSERT_OK(status);
                                    outside.emplaceValue();
                                });
                            ASSERT_EQ(std::move(pf.future).getNoThrow(), Status::OK());
                            ASSERT_EQ(exec->tasksRun.load(), 1);
                        });
}

TEST(Executor_Future, Reject_getAsync) {
    FUTURE_SUCCESS_TEST([] {},
                        [](auto&& fut) {
                            auto exec = RejectingExecutor::make();
                            auto pf = makePromiseFuture<void>();
                            std::move(fut).thenRunOn(exec).getAsync(
                                [promise = std::move(pf.promise)](Status status) mutable {
                                    promise.emplaceValue();  // shouldn't be run anyway.
                                    FAIL("how did I run!?!?!");
                                });

                            // Promise is destroyed without calling the callback.
                            ASSERT_EQ(std::move(pf.future).getNoThrow(), ErrorCodes::BrokenPromise);
                        });
}

TEST(Executor_Future, Success_then) {
    FUTURE_SUCCESS_TEST([] {},
                        [](auto&& fut) {
                            auto exec = InlineQueuedCountingExecutor::make();
                            ASSERT_EQ(std::move(fut).thenRunOn(exec).then([]() { return 3; }).get(),
                                      3);
                            ASSERT_EQ(exec->tasksRun.load(), 1);
                        });
}
TEST(Executor_Future, Reject_then) {
    FUTURE_SUCCESS_TEST([] {},
                        [](auto&& fut) {
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
    FUTURE_FAIL_TEST<void>([](auto&& fut) {
        auto exec = InlineQueuedCountingExecutor::make();
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
                        [](auto&& fut) {
                            auto exec = InlineQueuedCountingExecutor::make();
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
    FUTURE_FAIL_TEST<int>([](auto&& fut) {
        auto exec = InlineQueuedCountingExecutor::make();
        ASSERT_EQ(std::move(fut)
                      .thenRunOn(exec)
                      .onError([](Status s) FTU_LAMBDA_R(int) {
                          ASSERT_EQ(s, failStatus());
                          return 3;
                      })
                      .get(),
                  3);
        ASSERT_EQ(exec->tasksRun.load(), 1);
    });
}

TEST(Executor_Future, Fail_onErrorCode_OtherCode) {
    FUTURE_FAIL_TEST<void>([](auto&& fut) {
        auto exec = InlineQueuedCountingExecutor::make();
        ASSERT_EQ(std::move(fut)
                      .thenRunOn(exec)
                      .template onError<ErrorCodes::BadValue>(
                          [](Status s) FTU_LAMBDA_R(void) { FAIL("wrong code, sir"); })
                      .getNoThrow(),
                  failStatus());
        ASSERT_EQ(exec->tasksRun.load(), 0);
    });
}

TEST(Executor_Future, Success_then_onError_onError_then) {
    FUTURE_SUCCESS_TEST(
        [] {},
        [](auto&& fut) {
            auto exec = InlineQueuedCountingExecutor::make();
            ASSERT_EQ(std::move(fut)
                          .thenRunOn(exec)
                          .then([] { return failStatus(); })
                          .onError([](Status s) FTU_LAMBDA_R(void) { ASSERT_EQ(s, failStatus()); })
                          .onError([](Status)
                                       FTU_LAMBDA_R(void) { FAIL("how did you get this number?"); })
                          .then([] { return 3; })
                          .get(),
                      3);

            // 1 would also be valid if we did the optimization to not reschedule if
            // running on the same executor.
            ASSERT_EQ(exec->tasksRun.load(), 3);
        });
}

TEST(Executor_Future, Success_reject_recoverToFallback) {
    FUTURE_SUCCESS_TEST(
        [] {},
        [](auto&& fut) {
            auto rejecter = RejectingExecutor::make();
            auto accepter = InlineQueuedCountingExecutor::make();

            auto res = std::move(fut)
                           .thenRunOn(rejecter)
                           .then([] { FAIL("then()"); })
                           .onError([](Status) FTU_LAMBDA_R(void) { FAIL("onError()"); })
                           .onCompletion([](Status) FTU_LAMBDA_R(void) { FAIL("onCompletion()"); })
                           .thenRunOn(accepter)
                           .then([] {
                               FAIL("error?");
                               return 42;
                           })
                           .onError([](Status s) FTU_LAMBDA_R(int) {
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
