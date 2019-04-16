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

TEST(Future_Void, Success_getLvalue) {
    FUTURE_SUCCESS_TEST([] {}, [](/*Future<void>*/ auto&& fut) { fut.get(); });
}

TEST(Future_Void, Success_getConstLvalue) {
    FUTURE_SUCCESS_TEST([] {}, [](const /*Future<void>*/ auto& fut) { fut.get(); });
}

TEST(Future_Void, Success_getRvalue) {
    FUTURE_SUCCESS_TEST([] {}, [](/*Future<void>*/ auto&& fut) { std::move(fut).get(); });
}

TEST(Future_Void, Success_getNothrowLvalue) {
    FUTURE_SUCCESS_TEST(
        [] {}, [](/*Future<void>*/ auto&& fut) { ASSERT_EQ(fut.getNoThrow(), Status::OK()); });
}

TEST(Future_Void, Success_getNothrowConstLvalue) {
    FUTURE_SUCCESS_TEST(
        [] {}, [](const /*Future<void>*/ auto& fut) { ASSERT_EQ(fut.getNoThrow(), Status::OK()); });
}

TEST(Future_Void, Success_getNothrowRvalue) {
    FUTURE_SUCCESS_TEST(
        [] {},
        [](/*Future<void>*/ auto&& fut) { ASSERT_EQ(std::move(fut).getNoThrow(), Status::OK()); });
}

TEST(Future_Void, Success_semi_get) {
    FUTURE_SUCCESS_TEST([] {}, [](/*Future<void>*/ auto&& fut) { std::move(fut).semi().get(); });
}

TEST(Future_Void, Success_getAsync) {
    FUTURE_SUCCESS_TEST(
        [] {},
        [](/*Future<void>*/ auto&& fut) {
            auto pf = makePromiseFuture<void>();
            std::move(fut).getAsync([outside = std::move(pf.promise)](Status status) mutable {
                ASSERT_OK(status);
                outside.emplaceValue();
            });
            ASSERT_EQ(std::move(pf.future).getNoThrow(), Status::OK());
        });
}

TEST(Future_Void, Fail_getLvalue) {
    FUTURE_FAIL_TEST<void>(
        [](/*Future<void>*/ auto&& fut) { ASSERT_THROWS_failStatus(fut.get()); });
}

TEST(Future_Void, Fail_getConstLvalue) {
    FUTURE_FAIL_TEST<void>(
        [](const /*Future<void>*/ auto& fut) { ASSERT_THROWS_failStatus(fut.get()); });
}

TEST(Future_Void, Fail_getRvalue) {
    FUTURE_FAIL_TEST<void>(
        [](/*Future<void>*/ auto&& fut) { ASSERT_THROWS_failStatus(std::move(fut).get()); });
}

TEST(Future_Void, Fail_getNothrowLvalue) {
    FUTURE_FAIL_TEST<void>(
        [](/*Future<void>*/ auto&& fut) { ASSERT_EQ(fut.getNoThrow(), failStatus()); });
}

TEST(Future_Void, Fail_getNothrowConstLvalue) {
    FUTURE_FAIL_TEST<void>(
        [](const /*Future<void>*/ auto& fut) { ASSERT_EQ(fut.getNoThrow(), failStatus()); });
}

TEST(Future_Void, Fail_getNothrowRvalue) {
    FUTURE_FAIL_TEST<void>(
        [](/*Future<void>*/ auto&& fut) { ASSERT_EQ(std::move(fut).getNoThrow(), failStatus()); });
}

TEST(Future_Void, Fail_semi_get) {
    FUTURE_FAIL_TEST<void>(
        [](/*Future<void>*/ auto&& fut) { ASSERT_THROWS_failStatus(std::move(fut).semi().get()); });
}

TEST(Future_Void, Fail_getAsync) {
    FUTURE_FAIL_TEST<void>([](/*Future<void>*/ auto&& fut) {
        auto pf = makePromiseFuture<void>();
        std::move(fut).getAsync([outside = std::move(pf.promise)](Status status) mutable {
            ASSERT(!status.isOK());
            outside.setError(status);
        });
        ASSERT_EQ(std::move(pf.future).getNoThrow(), failStatus());
    });
}

TEST(Future_Void, Success_isReady) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            const auto id = stdx::this_thread::get_id();
                            while (!fut.isReady()) {
                            }
                            std::move(fut).getAsync([&](Status status) {
                                ASSERT_EQ(stdx::this_thread::get_id(), id);
                                ASSERT_OK(status);
                            });

                        });
}

TEST(Future_Void, Fail_isReady) {
    FUTURE_FAIL_TEST<void>([](/*Future<void>*/ auto&& fut) {
        const auto id = stdx::this_thread::get_id();
        while (!fut.isReady()) {
        }
        std::move(fut).getAsync([&](Status status) {
            ASSERT_EQ(stdx::this_thread::get_id(), id);
            ASSERT_NOT_OK(status);
        });

    });
}

TEST(Future_Void, isReady_TSAN_OK) {
    bool done = false;
    auto fut = async([&] { done = true; });
    //(void)*const_cast<volatile bool*>(&done);  // Data Race! Uncomment to make sure TSAN works.
    while (!fut.isReady()) {
    }
    ASSERT(done);
    fut.get();
    ASSERT(done);
}

TEST(Future_Void, Success_thenSimple) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut).then([]() { return 3; }).get(), 3);
                        });
}

TEST(Future_Void, Success_thenVoid) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut).then([] {}).then([] { return 3; }).get(), 3);
                        });
}

TEST(Future_Void, Success_thenStatus) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut).then([] {}).then([] { return 3; }).get(), 3);
                        });
}

TEST(Future_Void, Success_thenError_Status) {
    FUTURE_SUCCESS_TEST(
        [] {},
        [](/*Future<void>*/ auto&& fut) {
            auto fut2 =
                std::move(fut).then([]() { return Status(ErrorCodes::BadValue, "oh no!"); });
            static_assert(future_details::isFutureLike<decltype(fut2)>);
            static_assert(std::is_same_v<typename decltype(fut2)::value_type, void>);
            ASSERT_EQ(fut2.getNoThrow(), ErrorCodes::BadValue);
        });
}

TEST(Future_Void, Success_thenError_StatusWith) {
    FUTURE_SUCCESS_TEST(
        [] {},
        [](/*Future<void>*/ auto&& fut) {
            auto fut2 = std::move(fut).then(
                []() { return StatusWith<double>(ErrorCodes::BadValue, "oh no!"); });
            static_assert(future_details::isFutureLike<decltype(fut2)>);
            static_assert(std::is_same_v<typename decltype(fut2)::value_type, double>);
            ASSERT_EQ(fut2.getNoThrow(), ErrorCodes::BadValue);
        });
}

TEST(Future_Void, Success_thenFutureImmediate) {
    FUTURE_SUCCESS_TEST(
        [] {},
        [](/*Future<void>*/ auto&& fut) {
            ASSERT_EQ(std::move(fut).then([]() { return Future<int>::makeReady(3); }).get(), 3);
        });
}

TEST(Future_Void, Success_thenFutureReady) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .then([]() {
                                              auto pf = makePromiseFuture<int>();
                                              pf.promise.emplaceValue(3);
                                              return std::move(pf.future);
                                          })
                                          .get(),
                                      3);
                        });
}

TEST(Future_Void, Success_thenFutureAsync) {
    FUTURE_SUCCESS_TEST(
        [] {},
        [](/*Future<void>*/ auto&& fut) {
            ASSERT_EQ(std::move(fut).then([]() { return async([] { return 3; }); }).get(), 3);
        });
}

TEST(Future_Void, Success_thenSemiFutureAsync) {
    FUTURE_SUCCESS_TEST(
        [] {},
        [](/*Future<void>*/ auto&& fut) {
            ASSERT_EQ(std::move(fut).then([]() { return async([] { return 3; }).semi(); }).get(),
                      3);
        });
}

TEST(Future_Void, Fail_thenSimple) {
    FUTURE_FAIL_TEST<void>([](/*Future<void>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .then([]() {
                          FAIL("then() callback was called");
                          return int();
                      })
                      .getNoThrow(),
                  failStatus());
    });
}

TEST(Future_Void, Fail_thenFutureAsync) {
    FUTURE_FAIL_TEST<void>([](/*Future<void>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .then([]() {
                          FAIL("then() callback was called");
                          return Future<int>();
                      })
                      .getNoThrow(),
                  failStatus());
    });
}

TEST(Future_Void, Success_onErrorSimple) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            ASSERT_EQ(
                                std::move(fut)
                                    .onError([](Status) { FAIL("onError() callback was called"); })
                                    .then([] { return 3; })
                                    .get(),
                                3);
                        });
}

TEST(Future_Void, Success_onErrorFutureAsync) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .onError([](Status) {
                                              FAIL("onError() callback was called");
                                              return Future<void>();
                                          })
                                          .then([] { return 3; })
                                          .get(),
                                      3);
                        });
}

TEST(Future_Void, Fail_onErrorSimple) {
    FUTURE_FAIL_TEST<void>([](/*Future<void>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onError([](Status s) { ASSERT_EQ(s, failStatus()); })
                      .then([] { return 3; })
                      .getNoThrow(),
                  3);
    });
}

TEST(Future_Void, Fail_onErrorError_throw) {
    FUTURE_FAIL_TEST<void>([](/*Future<void>*/ auto&& fut) {
        auto fut2 = std::move(fut).onError([](Status s) {
            ASSERT_EQ(s, failStatus());
            uasserted(ErrorCodes::BadValue, "oh no!");
        });
        ASSERT_EQ(fut2.getNoThrow(), ErrorCodes::BadValue);
    });
}

TEST(Future_Void, Fail_onErrorError_Status) {
    FUTURE_FAIL_TEST<void>([](/*Future<void>*/ auto&& fut) {
        auto fut2 = std::move(fut).onError([](Status s) {
            ASSERT_EQ(s, failStatus());
            return Status(ErrorCodes::BadValue, "oh no!");
        });
        ASSERT_EQ(fut2.getNoThrow(), ErrorCodes::BadValue);
    });
}

TEST(Future_Void, Fail_onErrorFutureImmediate) {
    FUTURE_FAIL_TEST<void>([](/*Future<void>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onError([](Status s) {
                          ASSERT_EQ(s, failStatus());
                          return Future<void>::makeReady();
                      })
                      .then([] { return 3; })
                      .get(),
                  3);
    });
}

TEST(Future_Void, Fail_onErrorFutureReady) {
    FUTURE_FAIL_TEST<void>([](/*Future<void>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onError([](Status s) {
                          ASSERT_EQ(s, failStatus());
                          auto pf = makePromiseFuture<void>();
                          pf.promise.emplaceValue();
                          return std::move(pf.future);
                      })
                      .then([] { return 3; })
                      .get(),
                  3);
    });
}

TEST(Future_Void, Fail_onErrorFutureAsync) {
    FUTURE_FAIL_TEST<void>([](/*Future<void>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onError([&](Status s) {
                          ASSERT_EQ(s, failStatus());
                          return async([] {});
                      })
                      .then([] { return 3; })
                      .get(),
                  3);
    });
}

TEST(Future_Void, Success_onErrorCode) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .template onError<ErrorCodes::InternalError>([](Status) {
                                              FAIL("onError<code>() callback was called");
                                          })
                                          .then([] { return 3; })
                                          .get(),
                                      3);
                        });
}

TEST(Future_Void, Fail_onErrorCodeMatch) {
    FUTURE_FAIL_TEST<void>([](/*Future<void>*/ auto&& fut) {
        bool called = false;
        auto res =
            std::move(fut)
                .onError([](Status s) {
                    ASSERT_EQ(s, failStatus());
                    return Status(ErrorCodes::InternalError, "");
                })
                .template onError<ErrorCodes::InternalError>([&](Status&&) { called = true; })
                .then([] { return 3; })
                .getNoThrow();
        ASSERT_EQ(res, 3);
        ASSERT(called);
    });
}

TEST(Future_Void, Fail_onErrorCodeMatchFuture) {
    FUTURE_FAIL_TEST<void>([](/*Future<void>*/ auto&& fut) {
        bool called = false;
        auto res = std::move(fut)
                       .onError([](Status s) {
                           ASSERT_EQ(s, failStatus());
                           return Status(ErrorCodes::InternalError, "");
                       })
                       .template onError<ErrorCodes::InternalError>([&](Status&&) {
                           called = true;
                           return Future<void>::makeReady();
                       })
                       .then([] { return 3; })
                       .getNoThrow();
        ASSERT_EQ(res, 3);
        ASSERT(called);
    });
}

TEST(Future_Void, Fail_onErrorCodeMismatch) {
    FUTURE_FAIL_TEST<void>([](/*Future<void>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .template onError<ErrorCodes::InternalError>(
                          [](Status s) { FAIL("Why was this called?") << s; })
                      .onError([](Status s) { ASSERT_EQ(s, failStatus()); })
                      .then([] { return 3; })
                      .getNoThrow(),
                  3);
    });
}

TEST(Future_Void, Success_tap) {
    FUTURE_SUCCESS_TEST<kNoExecutorFuture_needsTap>(
        [] {},
        [](/*Future<void>*/ auto&& fut) {
            bool tapCalled = false;
            ASSERT_EQ(
                std::move(fut).tap([&tapCalled] { tapCalled = true; }).then([] { return 3; }).get(),
                3);
            ASSERT(tapCalled);
        });
}

TEST(Future_Void, Success_tapError) {
    FUTURE_SUCCESS_TEST<kNoExecutorFuture_needsTap>(
        [] {},
        [](/*Future<void>*/ auto&& fut) {
            ASSERT_EQ(std::move(fut)
                          .tapError([](Status s) { FAIL("tapError() callback was called"); })
                          .then([] { return 3; })
                          .get(),
                      3);
        });
}

TEST(Future_Void, Success_tapAll_StatusWith) {
    FUTURE_SUCCESS_TEST<kNoExecutorFuture_needsTap>([] {},
                                                    [](/*Future<void>*/ auto&& fut) {
                                                        bool tapCalled = false;
                                                        ASSERT_EQ(
                                                            std::move(fut)
                                                                .tapAll([&tapCalled](Status s) {
                                                                    ASSERT_OK(s);
                                                                    tapCalled = true;
                                                                })
                                                                .then([] { return 3; })
                                                                .get(),
                                                            3);
                                                        ASSERT(tapCalled);
                                                    });
}

TEST(Future_Void, Fail_tap) {
    FUTURE_FAIL_TEST<void, kNoExecutorFuture_needsTap>([](/*Future<void>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .tap([] { FAIL("tap() callback was called"); })
                      .onError([](Status s) { ASSERT_EQ(s, failStatus()); })
                      .then([] { return 3; })
                      .get(),
                  3);
    });
}

TEST(Future_Void, Fail_tapError) {
    FUTURE_FAIL_TEST<void, kNoExecutorFuture_needsTap>([](/*Future<void>*/ auto&& fut) {
        bool tapCalled = false;
        ASSERT_EQ(std::move(fut)
                      .tapError([&tapCalled](Status s) {
                          ASSERT_EQ(s, failStatus());
                          tapCalled = true;
                      })
                      .onError([](Status s) { ASSERT_EQ(s, failStatus()); })
                      .then([] { return 3; })
                      .get(),
                  3);
        ASSERT(tapCalled);
    });
}

TEST(Future_Void, Fail_tapAll_StatusWith) {
    FUTURE_FAIL_TEST<void, kNoExecutorFuture_needsTap>([](/*Future<void>*/ auto&& fut) {
        bool tapCalled = false;
        ASSERT_EQ(std::move(fut)
                      .tapAll([&tapCalled](Status sw) {
                          ASSERT_EQ(sw, failStatus());
                          tapCalled = true;
                      })
                      .onError([](Status s) { ASSERT_EQ(s, failStatus()); })
                      .then([] { return 3; })
                      .get(),
                  3);
        ASSERT(tapCalled);
    });
}

TEST(Future_Void, Success_onCompletionSimple) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .onCompletion([](Status status) {
                                              ASSERT_OK(status);
                                              return 3;
                                          })
                                          .get(),
                                      3);
                        });
}

TEST(Future_Void, Success_onCompletionVoid) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .onCompletion([](Status status) { ASSERT_OK(status); })
                                          .then([]() { return 3; })
                                          .get(),
                                      3);
                        });
}

TEST(Future_Void, Success_onCompletionError_Status) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            auto fut2 = std::move(fut).onCompletion([](Status status) {
                                ASSERT_OK(status);
                                return Status(ErrorCodes::BadValue, "oh no!");
                            });
                            static_assert(future_details::isFutureLike<decltype(fut2)>);
                            static_assert(
                                std::is_same_v<typename decltype(fut2)::value_type, void>);
                            ASSERT_EQ(fut2.getNoThrow(), ErrorCodes::BadValue);
                        });
}

TEST(Future_Void, Success_onCompletionError_StatusWith) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            auto fut2 = std::move(fut).onCompletion([](Status status) {
                                ASSERT_OK(status);
                                return StatusWith<double>(ErrorCodes::BadValue, "oh no!");
                            });
                            static_assert(future_details::isFutureLike<decltype(fut2)>);
                            static_assert(
                                std::is_same_v<typename decltype(fut2)::value_type, double>);
                            ASSERT_EQ(fut2.getNoThrow(), ErrorCodes::BadValue);
                        });
}

TEST(Future_Void, Success_onCompletionFutureImmediate) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .onCompletion([](Status status) {
                                              ASSERT_OK(status);
                                              return Future<int>::makeReady(3);
                                          })
                                          .get(),
                                      3);
                        });
}

TEST(Future_Void, Success_onCompletionFutureReady) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .onCompletion([](Status status) {
                                              ASSERT_OK(status);
                                              auto pf = makePromiseFuture<int>();
                                              pf.promise.emplaceValue(3);
                                              return std::move(pf.future);
                                          })
                                          .get(),
                                      3);
                        });
}

TEST(Future_Void, Success_onCompletionFutureAsync) {
    FUTURE_SUCCESS_TEST([] {},
                        [](/*Future<void>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .onCompletion([](Status status) {
                                              ASSERT_OK(status);
                                              return async([] { return 3; });
                                          })
                                          .get(),
                                      3);
                        });
}

TEST(Future_Void, Fail_onCompletionSimple) {
    FUTURE_FAIL_TEST<void>([](/*Future<void>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onCompletion([](Status s) { ASSERT_EQ(s, failStatus()); })
                      .then([] { return 3; })
                      .getNoThrow(),
                  3);
    });
}

TEST(Future_Void, Fail_onCompletionError_throw) {
    FUTURE_FAIL_TEST<void>([](/*Future<void>*/ auto&& fut) {
        auto fut2 = std::move(fut).onCompletion([](Status s) {
            ASSERT_EQ(s, failStatus());
            uasserted(ErrorCodes::BadValue, "oh no!");
        });
        ASSERT_EQ(fut2.getNoThrow(), ErrorCodes::BadValue);
    });
}

TEST(Future_Void, Fail_onCompletionError_Status) {
    FUTURE_FAIL_TEST<void>([](/*Future<void>*/ auto&& fut) {
        auto fut2 = std::move(fut).onCompletion([](Status s) {
            ASSERT_EQ(s, failStatus());
            return Status(ErrorCodes::BadValue, "oh no!");
        });
        ASSERT_EQ(fut2.getNoThrow(), ErrorCodes::BadValue);
    });
}

TEST(Future_Void, Fail_onCompletionFutureImmediate) {
    FUTURE_FAIL_TEST<void>([](/*Future<void>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onCompletion([](Status s) {
                          ASSERT_EQ(s, failStatus());
                          return Future<void>::makeReady();
                      })
                      .then([] { return 3; })
                      .get(),
                  3);
    });
}

TEST(Future_Void, Fail_onCompletionFutureReady) {
    FUTURE_FAIL_TEST<void>([](/*Future<void>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onCompletion([](Status s) {
                          ASSERT_EQ(s, failStatus());
                          auto pf = makePromiseFuture<void>();
                          pf.promise.emplaceValue();
                          return std::move(pf.future);
                      })
                      .then([] { return 3; })
                      .get(),
                  3);
    });
}

TEST(Future_Void, Fail_onCompletionFutureAsync) {
    FUTURE_FAIL_TEST<void>([](/*Future<void>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onCompletion([&](Status s) {
                          ASSERT_EQ(s, failStatus());
                          return async([] {});
                      })
                      .then([] { return 3; })
                      .get(),
                  3);
    });
}

}  // namespace
}  // namespace mongo
