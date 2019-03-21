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

TEST(Future, Success_getLvalue) {
    FUTURE_SUCCESS_TEST([] { return 1; },
                        [](/*Future<int>*/ auto&& fut) { ASSERT_EQ(fut.get(), 1); });
}

TEST(Future, Success_getConstLvalue) {
    FUTURE_SUCCESS_TEST([] { return 1; },
                        [](const /*Future<int>*/ auto& fut) { ASSERT_EQ(fut.get(), 1); });
}

TEST(Future, Success_getRvalue) {
    FUTURE_SUCCESS_TEST([] { return 1; },
                        [](/*Future<int>*/ auto&& fut) { ASSERT_EQ(std::move(fut).get(), 1); });
}

TEST(Future, Success_getNothrowLvalue) {
    FUTURE_SUCCESS_TEST([] { return 1; },
                        [](/*Future<int>*/ auto&& fut) { ASSERT_EQ(fut.getNoThrow(), 1); });
}

TEST(Future, Success_getNothrowConstLvalue) {
    FUTURE_SUCCESS_TEST([] { return 1; },
                        [](const /*Future<int>*/ auto& fut) { ASSERT_EQ(fut.getNoThrow(), 1); });
}

TEST(Future, Success_getNothrowRvalue) {
    FUTURE_SUCCESS_TEST(
        [] { return 1; },
        [](/*Future<int>*/ auto&& fut) { ASSERT_EQ(std::move(fut).getNoThrow(), 1); });
}

TEST(Future, Success_semi_get) {
    FUTURE_SUCCESS_TEST(
        [] { return 1; },
        [](/*Future<int>*/ auto&& fut) { ASSERT_EQ(std::move(fut).semi().get(), 1); });
}

TEST(Future, Success_shared_get) {
    FUTURE_SUCCESS_TEST(
        [] { return 1; },
        [](/*Future<int>*/ auto&& fut) { ASSERT_EQ(std::move(fut).share().get(), 1); });
}

TEST(Future, Success_shared_getNothrow) {
    FUTURE_SUCCESS_TEST(
        [] { return 1; },
        [](/*Future<int>*/ auto&& fut) { ASSERT_EQ(std::move(fut).share().getNoThrow(), 1); });
}

TEST(Future, Success_getAsync) {
    FUTURE_SUCCESS_TEST(
        [] { return 1; },
        [](/*Future<int>*/ auto&& fut) {
            auto pf = makePromiseFuture<int>();
            std::move(fut).getAsync([outside = std::move(pf.promise)](StatusWith<int> sw) mutable {
                ASSERT_OK(sw);
                outside.emplaceValue(sw.getValue());
            });
            ASSERT_EQ(std::move(pf.future).get(), 1);
        });
}

TEST(Future, Fail_getLvalue) {
    FUTURE_FAIL_TEST<int>([](/*Future<int>*/ auto&& fut) { ASSERT_THROWS_failStatus(fut.get()); });
}

TEST(Future, Fail_getConstLvalue) {
    FUTURE_FAIL_TEST<int>(
        [](const /*Future<int>*/ auto& fut) { ASSERT_THROWS_failStatus(fut.get()); });
}

TEST(Future, Fail_getRvalue) {
    FUTURE_FAIL_TEST<int>(
        [](/*Future<int>*/ auto&& fut) { ASSERT_THROWS_failStatus(std::move(fut).get()); });
}

TEST(Future, Fail_getNothrowLvalue) {
    FUTURE_FAIL_TEST<int>(
        [](/*Future<int>*/ auto&& fut) { ASSERT_EQ(fut.getNoThrow(), failStatus()); });
}

TEST(Future, Fail_getNothrowConstLvalue) {
    FUTURE_FAIL_TEST<int>(
        [](const /*Future<int>*/ auto& fut) { ASSERT_EQ(fut.getNoThrow(), failStatus()); });
}

TEST(Future, Fail_getNothrowRvalue) {
    FUTURE_FAIL_TEST<int>(
        [](/*Future<int>*/ auto&& fut) { ASSERT_EQ(std::move(fut).getNoThrow(), failStatus()); });
}

TEST(Future, Fail_semi_get) {
    FUTURE_FAIL_TEST<int>(
        [](/*Future<int>*/ auto&& fut) { ASSERT_THROWS_failStatus(std::move(fut).semi().get()); });
}

TEST(Future, Fail_shared_get) {
    FUTURE_FAIL_TEST<int>(
        [](/*Future<int>*/ auto&& fut) { ASSERT_THROWS_failStatus(std::move(fut).share().get()); });
}

TEST(Future, Fail_shared_getNothrow) {
    FUTURE_FAIL_TEST<int>([](/*Future<int>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut).share().getNoThrow(), failStatus());
    });
}

TEST(Future, Fail_getAsync) {
    FUTURE_FAIL_TEST<int>([](/*Future<int>*/ auto&& fut) {
        auto pf = makePromiseFuture<int>();
        std::move(fut).getAsync([outside = std::move(pf.promise)](StatusWith<int> sw) mutable {
            ASSERT(!sw.isOK());
            outside.setError(sw.getStatus());
        });
        ASSERT_EQ(std::move(pf.future).getNoThrow(), failStatus());
    });
}

TEST(Future, Success_isReady) {
    FUTURE_SUCCESS_TEST([] { return 1; },
                        [](/*Future<int>*/ auto&& fut) {
                            const auto id = stdx::this_thread::get_id();
                            while (!fut.isReady()) {
                            }
                            std::move(fut).getAsync([&](StatusWith<int> status) {
                                ASSERT_EQ(stdx::this_thread::get_id(), id);
                                ASSERT_EQ(status, 1);
                            });

                        });
}

TEST(Future, Fail_isReady) {
    FUTURE_FAIL_TEST<int>([](/*Future<int>*/ auto&& fut) {
        const auto id = stdx::this_thread::get_id();
        while (!fut.isReady()) {
        }
        std::move(fut).getAsync([&](StatusWith<int> status) {
            ASSERT_EQ(stdx::this_thread::get_id(), id);
            ASSERT_NOT_OK(status);
        });

    });
}

TEST(Future, Success_wait) {
    FUTURE_SUCCESS_TEST([] { return 1; },
                        [](/*Future<int>*/ auto&& fut) {
                            fut.wait();
                            ASSERT_EQ(fut.get(), 1);
                        });
}

TEST(Future, Success_waitNoThrow) {
    FUTURE_SUCCESS_TEST([] { return 1; },
                        [](/*Future<int>*/ auto&& fut) {
                            ASSERT_OK(fut.waitNoThrow());
                            ASSERT_EQ(fut.get(), 1);
                        });
}

TEST(Future, Fail_wait) {
    FUTURE_FAIL_TEST<int>([](/*Future<int>*/ auto&& fut) {
        fut.wait();
        ASSERT_THROWS_failStatus(fut.get());
    });
}

TEST(Future, Fail_waitNoThrow) {
    FUTURE_FAIL_TEST<int>([](/*Future<int>*/ auto&& fut) {
        ASSERT_OK(fut.waitNoThrow());
        ASSERT_THROWS_failStatus(fut.get());
    });
}

TEST(Future, isReady_TSAN_OK) {
    bool done = false;
    auto fut = async([&] {
        done = true;
        return 1;
    });
    //(void)*const_cast<volatile bool*>(&done);  // Data Race! Uncomment to make sure TSAN works.
    while (!fut.isReady()) {
    }
    ASSERT(done);
    (void)fut.get();
    ASSERT(done);
}

TEST(Future, isReady_shared_TSAN_OK) {
    bool done = false;
    auto fut = async([&] {
                   done = true;
                   return 1;
               }).share();
    //(void)*const_cast<volatile bool*>(&done);  // Data Race! Uncomment to make sure TSAN works.
    while (!fut.isReady()) {
    }
    ASSERT(done);
    (void)fut.get();
    ASSERT(done);
}

TEST(Future, Success_thenSimple) {
    FUTURE_SUCCESS_TEST([] { return 1; },
                        [](/*Future<int>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut).then([](int i) { return i + 2; }).get(), 3);
                        });
}

TEST(Future, Success_thenSimpleAuto) {
    FUTURE_SUCCESS_TEST([] { return 1; },
                        [](/*Future<int>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut).then([](auto i) { return i + 2; }).get(), 3);
                        });
}

TEST(Future, Success_thenVoid) {
    FUTURE_SUCCESS_TEST(
        [] { return 1; },
        [](/*Future<int>*/ auto&& fut) {
            ASSERT_EQ(
                std::move(fut).then([](int i) { ASSERT_EQ(i, 1); }).then([] { return 3; }).get(),
                3);
        });
}

TEST(Future, Success_thenStatus) {
    FUTURE_SUCCESS_TEST([] { return 1; },
                        [](/*Future<int>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .then([](int i) {
                                              ASSERT_EQ(i, 1);
                                              return Status::OK();
                                          })
                                          .then([] { return 3; })
                                          .get(),
                                      3);
                        });
}

TEST(Future, Success_thenError_Status) {
    FUTURE_SUCCESS_TEST(
        [] { return 1; },
        [](/*Future<int>*/ auto&& fut) {
            auto fut2 =
                std::move(fut).then([](int i) { return Status(ErrorCodes::BadValue, "oh no!"); });
            static_assert(future_details::isFutureLike<decltype(fut2)>);
            static_assert(std::is_same_v<typename decltype(fut2)::value_type, void>);
            ASSERT_THROWS(fut2.get(), ExceptionFor<ErrorCodes::BadValue>);
        });
}

TEST(Future, Success_thenError_StatusWith) {
    FUTURE_SUCCESS_TEST(
        [] { return 1; },
        [](/*Future<int>*/ auto&& fut) {
            auto fut2 = std::move(fut).then(
                [](int i) { return StatusWith<double>(ErrorCodes::BadValue, "oh no!"); });
            static_assert(future_details::isFutureLike<decltype(fut2)>);
            static_assert(std::is_same_v<typename decltype(fut2)::value_type, double>);
            ASSERT_THROWS(fut2.get(), ExceptionFor<ErrorCodes::BadValue>);
        });
}

TEST(Future, Success_thenFutureImmediate) {
    FUTURE_SUCCESS_TEST(
        [] { return 1; },
        [](/*Future<int>*/ auto&& fut) {
            ASSERT_EQ(
                std::move(fut).then([](int i) { return Future<int>::makeReady(i + 2); }).get(), 3);
        });
}

TEST(Future, Success_thenFutureReady) {
    FUTURE_SUCCESS_TEST([] { return 1; },
                        [](/*Future<int>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .then([](int i) {
                                              auto pf = makePromiseFuture<int>();
                                              pf.promise.emplaceValue(i + 2);
                                              return std::move(pf.future);
                                          })
                                          .get(),
                                      3);
                        });
}

TEST(Future, Success_thenFutureAsync) {
    FUTURE_SUCCESS_TEST(
        [] { return 1; },
        [](/*Future<int>*/ auto&& fut) {
            ASSERT_EQ(std::move(fut).then([](int i) { return async([i] { return i + 2; }); }).get(),
                      3);
        });
}

TEST(Future, Success_thenSemiFutureAsync) {
    FUTURE_SUCCESS_TEST([] { return 1; },
                        [](/*Future<int>*/ auto&& fut) {
                            ASSERT_EQ(
                                std::move(fut)
                                    .then([](int i) { return async([i] { return i + 2; }).semi(); })
                                    .get(),
                                3);
                        });
}

TEST(Future, Success_thenFutureAsyncThrow) {
    FUTURE_SUCCESS_TEST([] { return 1; },
                        [](/*Future<int>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .then([](int i) {
                                              uasserted(ErrorCodes::BadValue, "oh no!");
                                              return Future<int>();
                                          })
                                          .getNoThrow(),
                                      ErrorCodes::BadValue);
                        });
}

TEST(Future, Fail_thenSimple) {
    FUTURE_FAIL_TEST<int>([](/*Future<int>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .then([](int i) {
                          FAIL("then() callback was called");
                          return int();
                      })
                      .getNoThrow(),
                  failStatus());
    });
}

TEST(Future, Fail_thenFutureAsync) {
    FUTURE_FAIL_TEST<int>([](/*Future<int>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .then([](int i) {
                          FAIL("then() callback was called");
                          return Future<int>();
                      })
                      .getNoThrow(),
                  failStatus());
    });
}

TEST(Future, Success_onErrorSimple) {
    FUTURE_SUCCESS_TEST([] { return 1; },
                        [](/*Future<int>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .onError([](Status) {
                                              FAIL("onError() callback was called");
                                              return 0;
                                          })
                                          .then([](int i) { return i + 2; })
                                          .get(),
                                      3);
                        });
}

TEST(Future, Success_onErrorFutureAsync) {
    FUTURE_SUCCESS_TEST([] { return 1; },
                        [](/*Future<int>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .onError([](Status) {
                                              FAIL("onError() callback was called");
                                              return Future<int>();
                                          })
                                          .then([](int i) { return i + 2; })
                                          .get(),
                                      3);
                        });
}

TEST(Future, Fail_onErrorSimple) {
    FUTURE_FAIL_TEST<int>([](/*Future<int>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onError([](Status s) {
                          ASSERT_EQ(s, failStatus());
                          return 3;
                      })
                      .getNoThrow(),
                  3);
    });
}

TEST(Future, Fail_onErrorError_throw) {
    FUTURE_FAIL_TEST<int>([](/*Future<int>*/ auto&& fut) {
        auto fut2 = std::move(fut).onError([](Status s) -> int {
            ASSERT_EQ(s, failStatus());
            uasserted(ErrorCodes::BadValue, "oh no!");
        });
        ASSERT_EQ(fut2.getNoThrow(), ErrorCodes::BadValue);
    });
}

TEST(Future, Fail_onErrorError_StatusWith) {
    FUTURE_FAIL_TEST<int>([](/*Future<int>*/ auto&& fut) {
        auto fut2 = std::move(fut).onError([](Status s) {
            ASSERT_EQ(s, failStatus());
            return StatusWith<int>(ErrorCodes::BadValue, "oh no!");
        });
        ASSERT_EQ(fut2.getNoThrow(), ErrorCodes::BadValue);
    });
}

TEST(Future, Fail_onErrorFutureImmediate) {
    FUTURE_FAIL_TEST<int>([](/*Future<int>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onError([](Status s) {
                          ASSERT_EQ(s, failStatus());
                          return Future<int>::makeReady(3);
                      })
                      .get(),
                  3);
    });
}

TEST(Future, Fail_onErrorFutureReady) {
    FUTURE_FAIL_TEST<int>([](/*Future<int>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onError([](Status s) {
                          ASSERT_EQ(s, failStatus());
                          auto pf = makePromiseFuture<int>();
                          pf.promise.emplaceValue(3);
                          return std::move(pf.future);
                      })
                      .get(),
                  3);
    });
}

TEST(Future, Fail_onErrorFutureAsync) {
    FUTURE_FAIL_TEST<int>([](/*Future<int>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onError([](Status s) {
                          ASSERT_EQ(s, failStatus());
                          return async([] { return 3; });
                      })
                      .get(),
                  3);
    });
}

TEST(Future, Success_onErrorCode) {
    FUTURE_SUCCESS_TEST([] { return 1; },
                        [](/*Future<int>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .template onError<ErrorCodes::InternalError>([](Status) {
                                              FAIL("onError<code>() callback was called");
                                              return 0;
                                          })
                                          .then([](int i) { return i + 2; })
                                          .get(),
                                      3);
                        });
}

TEST(Future, Fail_onErrorCodeMatch) {
    FUTURE_FAIL_TEST<int>([](/*Future<int>*/ auto&& fut) {
        auto res = std::move(fut)
                       .onError([](Status s) {
                           ASSERT_EQ(s, failStatus());
                           return StatusWith<int>(ErrorCodes::InternalError, "");
                       })
                       .template onError<ErrorCodes::InternalError>(
                           [](Status&&) { return StatusWith<int>(3); })
                       .getNoThrow();
        ASSERT_EQ(res, 3);
    });
}

TEST(Future, Fail_onErrorCodeMatchFuture) {
    FUTURE_FAIL_TEST<int>([](/*Future<int>*/ auto&& fut) {
        auto res = std::move(fut)
                       .onError([](Status s) {
                           ASSERT_EQ(s, failStatus());
                           return StatusWith<int>(ErrorCodes::InternalError, "");
                       })
                       .template onError<ErrorCodes::InternalError>(
                           [](Status&&) { return Future<int>(3); })
                       .getNoThrow();
        ASSERT_EQ(res, 3);
    });
}

TEST(Future, Fail_onErrorCodeMismatch) {
    FUTURE_FAIL_TEST<int>([](/*Future<int>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .template onError<ErrorCodes::InternalError>([](Status s) -> int {
                          FAIL("Why was this called?") << s;
                          MONGO_UNREACHABLE;
                      })
                      .onError([](Status s) {
                          ASSERT_EQ(s, failStatus());
                          return 3;
                      })
                      .getNoThrow(),
                  3);
    });
}

TEST(Future, Success_onErrorCategory) {
    FUTURE_SUCCESS_TEST(
        [] { return 1; },
        [](/*Future<int>*/ auto&& fut) {
            ASSERT_EQ(std::move(fut)
                          .template onErrorCategory<ErrorCategory::NetworkError>([](Status) {
                              FAIL("onErrorCategory<category>() callback was called");
                              return 0;
                          })
                          .then([](int i) { return i + 2; })
                          .get(),
                      3);
        });
}

TEST(Future, Fail_onErrorCategoryMatch) {
    FUTURE_FAIL_TEST<int>([](/*Future<int>*/ auto&& fut) {
        auto res = std::move(fut)
                       .onError([](Status s) {
                           ASSERT_EQ(s, failStatus());
                           return StatusWith<int>(ErrorCodes::HostUnreachable, "");
                       })
                       .template onErrorCategory<ErrorCategory::NetworkError>(
                           [](Status&&) { return StatusWith<int>(3); })
                       .getNoThrow();
        ASSERT_EQ(res, 3);
    });
}

TEST(Future, Fail_onErrorCategoryMismatch) {
    FUTURE_FAIL_TEST<int>([](/*Future<int>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .template onErrorCategory<ErrorCategory::NetworkError>([](Status s) -> int {
                          FAIL("Why was this called?") << s;
                          MONGO_UNREACHABLE;
                      })
                      .onError([](Status s) {
                          ASSERT_EQ(s, failStatus());
                          return 3;
                      })
                      .getNoThrow(),
                  3);
    });
}

TEST(Future, Success_tap) {
    FUTURE_SUCCESS_TEST<kNoExecutorFuture_needsTap>([] { return 1; },
                                                    [](/*Future<int>*/ auto&& fut) {
                                                        bool tapCalled = false;
                                                        ASSERT_EQ(
                                                            std::move(fut)
                                                                .tap([&tapCalled](int i) {
                                                                    ASSERT_EQ(i, 1);
                                                                    tapCalled = true;
                                                                })
                                                                .then([](int i) { return i + 2; })
                                                                .get(),
                                                            3);
                                                        ASSERT(tapCalled);
                                                    });
}

TEST(Future, Success_tapError) {
    FUTURE_SUCCESS_TEST<kNoExecutorFuture_needsTap>(
        [] { return 1; },
        [](/*Future<int>*/ auto&& fut) {
            ASSERT_EQ(std::move(fut)
                          .tapError([](Status s) { FAIL("tapError() callback was called"); })
                          .then([](int i) { return i + 2; })
                          .get(),
                      3);
        });
}

TEST(Future, Success_tapAll_StatusWith) {
    FUTURE_SUCCESS_TEST<kNoExecutorFuture_needsTap>(
        [] { return 1; },
        [](/*Future<int>*/ auto&& fut) {
            bool tapCalled = false;
            ASSERT_EQ(std::move(fut)
                          .tapAll([&tapCalled](StatusWith<int> sw) {
                              ASSERT_EQ(sw, 1);
                              tapCalled = true;
                          })
                          .then([](int i) { return i + 2; })
                          .get(),
                      3);
            ASSERT(tapCalled);
        });
}

TEST(Future, Fail_tap) {
    FUTURE_FAIL_TEST<int, kNoExecutorFuture_needsTap>([](/*Future<int>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .tap([](int i) { FAIL("tap() callback was called"); })
                      .onError([](Status s) {
                          ASSERT_EQ(s, failStatus());
                          return 3;
                      })
                      .get(),
                  3);
    });
}

TEST(Future, Fail_tapError) {
    FUTURE_FAIL_TEST<int, kNoExecutorFuture_needsTap>([](/*Future<int>*/ auto&& fut) {
        bool tapCalled = false;
        ASSERT_EQ(std::move(fut)
                      .tapError([&tapCalled](Status s) {
                          ASSERT_EQ(s, failStatus());
                          tapCalled = true;
                      })
                      .onError([](Status s) {
                          ASSERT_EQ(s, failStatus());
                          return 3;
                      })
                      .get(),
                  3);
        ASSERT(tapCalled);
    });
}

TEST(Future, Fail_tapAll_StatusWith) {
    FUTURE_FAIL_TEST<int, kNoExecutorFuture_needsTap>([](/*Future<int>*/ auto&& fut) {
        bool tapCalled = false;
        ASSERT_EQ(std::move(fut)
                      .tapAll([&tapCalled](StatusWith<int> sw) {
                          ASSERT_EQ(sw.getStatus(), failStatus());
                          tapCalled = true;
                      })
                      .onError([](Status s) {
                          ASSERT_EQ(s, failStatus());
                          return 3;
                      })
                      .get(),
                  3);
        ASSERT(tapCalled);
    });
}

TEST(Future, Success_onCompletionSimple) {
    FUTURE_SUCCESS_TEST(
        [] { return 1; },
        [](/*Future<int>*/ auto&& fut) {
            ASSERT_EQ(std::move(fut)
                          .onCompletion([](StatusWith<int> i) { return i.getValue() + 2; })
                          .get(),
                      3);
        });
}

TEST(Future, Success_onCompletionVoid) {
    FUTURE_SUCCESS_TEST(
        [] { return 1; },
        [](/*Future<int>*/ auto&& fut) {
            ASSERT_EQ(std::move(fut)
                          .onCompletion([](StatusWith<int> i) { ASSERT_EQ(i.getValue(), 1); })
                          .onCompletion([](Status s) {
                              ASSERT_OK(s);
                              return 3;
                          })
                          .get(),
                      3);
        });
}

TEST(Future, Success_onCompletionStatus) {
    FUTURE_SUCCESS_TEST([] { return 1; },
                        [](/*Future<int>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .onCompletion([](StatusWith<int> i) {
                                              ASSERT_EQ(i, 1);
                                              return Status::OK();
                                          })
                                          .onCompletion([](Status s) {
                                              ASSERT_OK(s);
                                              return 3;
                                          })
                                          .get(),
                                      3);
                        });
}

TEST(Future, Success_onCompletionError_Status) {
    FUTURE_SUCCESS_TEST(
        [] { return 1; },
        [](/*Future<int>*/ auto&& fut) {
            auto fut2 = std::move(fut).onCompletion(
                [](StatusWith<int> i) { return Status(ErrorCodes::BadValue, "oh no!"); });
            static_assert(future_details::isFutureLike<decltype(fut2)>);
            static_assert(std::is_same_v<typename decltype(fut2)::value_type, void>);
            ASSERT_THROWS(fut2.get(), ExceptionFor<ErrorCodes::BadValue>);
        });
}

TEST(Future, Success_onCompletionError_StatusWith) {
    FUTURE_SUCCESS_TEST([] { return 1; },
                        [](/*Future<int>*/ auto&& fut) {
                            auto fut2 = std::move(fut).onCompletion([](StatusWith<int> i) {
                                return StatusWith<double>(ErrorCodes::BadValue, "oh no!");
                            });
                            static_assert(future_details::isFutureLike<decltype(fut2)>);
                            static_assert(
                                std::is_same_v<typename decltype(fut2)::value_type, double>);
                            ASSERT_THROWS(fut2.get(), ExceptionFor<ErrorCodes::BadValue>);
                        });
}

TEST(Future, Success_onCompletionFutureImmediate) {
    FUTURE_SUCCESS_TEST([] { return 1; },
                        [](/*Future<int>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .onCompletion([](StatusWith<int> i) {
                                              return Future<int>::makeReady(i.getValue() + 2);
                                          })
                                          .get(),
                                      3);
                        });
}

TEST(Future, Success_onCompletionFutureReady) {
    FUTURE_SUCCESS_TEST([] { return 1; },
                        [](/*Future<int>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .onCompletion([](StatusWith<int> i) {
                                              auto pf = makePromiseFuture<int>();
                                              pf.promise.emplaceValue(i.getValue() + 2);
                                              return std::move(pf.future);
                                          })
                                          .get(),
                                      3);
                        });
}

TEST(Future, Success_onCompletionFutureAsync) {
    FUTURE_SUCCESS_TEST([] { return 1; },
                        [](/*Future<int>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .onCompletion([](StatusWith<int> i) {
                                              return async([i = i.getValue()] { return i + 2; });
                                          })
                                          .get(),
                                      3);
                        });
}

TEST(Future, Success_onCompletionFutureAsyncThrow) {
    FUTURE_SUCCESS_TEST([] { return 1; },
                        [](/*Future<int>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .onCompletion([](StatusWith<int> i) {
                                              uasserted(ErrorCodes::BadValue, "oh no!");
                                              return Future<int>();
                                          })
                                          .getNoThrow(),
                                      ErrorCodes::BadValue);
                        });
}

TEST(Future, Fail_onCompletionSimple) {
    FUTURE_FAIL_TEST<int>([](/*Future<int>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onCompletion([](StatusWith<int> i) {
                          ASSERT_NOT_OK(i);

                          return i.getStatus();
                      })
                      .getNoThrow(),
                  failStatus());
    });
}

TEST(Future, Fail_onCompletionError_throw) {
    FUTURE_FAIL_TEST<int>([](/*Future<int>*/ auto&& fut) {
        auto fut2 = std::move(fut).onCompletion([](StatusWith<int> s) -> int {
            ASSERT_EQ(s.getStatus(), failStatus());
            uasserted(ErrorCodes::BadValue, "oh no!");
        });
        ASSERT_EQ(fut2.getNoThrow(), ErrorCodes::BadValue);
    });
}

TEST(Future, Fail_onCompletionError_StatusWith) {
    FUTURE_FAIL_TEST<int>([](/*Future<int>*/ auto&& fut) {
        auto fut2 = std::move(fut).onCompletion([](StatusWith<int> s) {
            ASSERT_EQ(s.getStatus(), failStatus());
            return StatusWith<int>(ErrorCodes::BadValue, "oh no!");
        });
        ASSERT_EQ(fut2.getNoThrow(), ErrorCodes::BadValue);
    });
}

TEST(Future, Fail_onCompletionFutureImmediate) {
    FUTURE_FAIL_TEST<int>([](/*Future<int>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onCompletion([](StatusWith<int> s) {
                          ASSERT_EQ(s.getStatus(), failStatus());
                          return Future<int>::makeReady(3);
                      })
                      .get(),
                  3);
    });
}

TEST(Future, Fail_onCompletionFutureReady) {
    FUTURE_FAIL_TEST<int>([](/*Future<int>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onCompletion([](StatusWith<int> s) {
                          ASSERT_EQ(s.getStatus(), failStatus());
                          auto pf = makePromiseFuture<int>();
                          pf.promise.emplaceValue(3);
                          return std::move(pf.future);
                      })
                      .get(),
                  3);
    });
}

}  // namespace
}  // namespace mongo
