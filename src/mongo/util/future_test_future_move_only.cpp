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

// A move-only type that isn't default constructible. It has binary ops with int to make it easier
// to have a common format with the above tests.
struct Widget {
    explicit Widget(int val) : val(val) {}

    Widget(Widget&& other) : Widget(other.val) {}
    Widget& operator=(Widget&& other) {
        val = other.val;
        return *this;
    }

    Widget() = delete;
    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;

    Widget operator+(int i) const {
        return Widget(val + i);
    }

    bool operator==(int i) const {
        return val == i;
    }

    bool operator==(Widget w) const {
        return val == w.val;
    }

    int val;
};

std::ostream& operator<<(std::ostream& stream, const Widget& widget) {
    return stream << "Widget(" << widget.val << ')';
}

TEST(Future_MoveOnly, Success_getLvalue) {
    FUTURE_SUCCESS_TEST([] { return Widget(1); },
                        [](/*Future<Widget>*/ auto&& fut) { ASSERT_EQ(fut.get(), 1); });
}

TEST(Future_MoveOnly, Success_getConstLvalue) {
    FUTURE_SUCCESS_TEST([] { return Widget(1); },
                        [](const /*Future<Widget>*/ auto& fut) { ASSERT_EQ(fut.get(), 1); });
}

TEST(Future_MoveOnly, Success_getRvalue) {
    FUTURE_SUCCESS_TEST([] { return Widget(1); },
                        [](/*Future<Widget>*/ auto&& fut) { ASSERT_EQ(std::move(fut).get(), 1); });
}

TEST(Future_MoveOnly, Success_semi_get) {
    FUTURE_SUCCESS_TEST(
        [] { return Widget(1); },
        [](/*Future<Widget>*/ auto&& fut) { ASSERT_EQ(std::move(fut).semi().get(), 1); });
}

#if 0  // Needs copy
TEST(Future_MoveOnly, Success_shared_get) {
    FUTURE_SUCCESS_TEST(
        [] { return Widget(1); },
        [](/*Future<Widget>*/ auto&& fut) { ASSERT_EQ(std::move(fut).share().get(), 1); });
}

TEST(Future_MoveOnly, Success_getNothrowLvalue) {
    FUTURE_SUCCESS_TEST([] { return Widget(1); },
                        [](/*Future<Widget>*/ auto&& fut) { ASSERT_EQ(fut.getNoThrow(), 1); });
}

TEST(Future_MoveOnly, Success_getNothrowConstLvalue) {
    FUTURE_SUCCESS_TEST([] { return Widget(1); },
                        [](const /*Future<Widget>*/ auto& fut) { ASSERT_EQ(fut.getNoThrow(), 1); });
}

TEST(Future_MoveOnly, Success_shared_getNothrow) {
    FUTURE_SUCCESS_TEST(
        [] { return Widget(1); },
        [](/*Future<Widget>*/ auto&& fut) { ASSERT_EQ(std::move(fut).share().getNoThrow(), 1); });
}
#endif

TEST(Future_MoveOnly, Success_getNothrowRvalue) {
    FUTURE_SUCCESS_TEST([] { return Widget(1); },
                        [](/*Future<Widget>*/ auto&& fut) {
                            ASSERT_EQ(uassertStatusOK(std::move(fut).getNoThrow()).val, 1);
                        });
}

TEST(Future_MoveOnly, Success_getAsync) {
    FUTURE_SUCCESS_TEST([] { return Widget(1); },
                        [](/*Future<Widget>*/ auto&& fut) {
                            auto pf = makePromiseFuture<Widget>();
                            std::move(fut).getAsync(
                                [outside = std::move(pf.promise)](StatusWith<Widget> sw) mutable {
                                    ASSERT_OK(sw);
                                    outside.emplaceValue(std::move(sw.getValue()));
                                });
                            ASSERT_EQ(std::move(pf.future).get(), 1);
                        });
}

TEST(Future_MoveOnly, Fail_getLvalue) {
    FUTURE_FAIL_TEST<Widget>(
        [](/*Future<Widget>*/ auto&& fut) { ASSERT_THROWS_failStatus(fut.get()); });
}

TEST(Future_MoveOnly, Fail_getConstLvalue) {
    FUTURE_FAIL_TEST<Widget>(
        [](const /*Future<Widget>*/ auto& fut) { ASSERT_THROWS_failStatus(fut.get()); });
}

TEST(Future_MoveOnly, Fail_getRvalue) {
    FUTURE_FAIL_TEST<Widget>(
        [](/*Future<Widget>*/ auto&& fut) { ASSERT_THROWS_failStatus(std::move(fut).get()); });
}

TEST(Future_MoveOnly, Fail_semi_get) {
    FUTURE_FAIL_TEST<Widget>([](/*Future<Widget>*/ auto&& fut) {
        ASSERT_THROWS_failStatus(std::move(fut).semi().get());
    });
}

#if 0  // Needs copy
TEST(Future_MoveOnly, Fail_shared_get) {
    FUTURE_FAIL_TEST<Widget>([](/*Future<Widget>*/ auto&& fut) {
        ASSERT_THROWS_failStatus(std::move(fut).share().get());
    });
}

TEST(Future_MoveOnly, Fail_getNothrowLvalue) {
    FUTURE_FAIL_TEST<Widget>([](/*Future<Widget>*/ auto&& fut) { ASSERT_EQ(fut.getNoThrow(), failStatus); });
}

TEST(Future_MoveOnly, Fail_getNothrowConstLvalue) {
    FUTURE_FAIL_TEST<Widget>(
        [](const /*Future<Widget>*/ auto& fut) { ASSERT_EQ(fut.getNoThrow(), failStatus); });
}

TEST(Future_MoveOnly, Fail_shared_getNothrow) {
    FUTURE_FAIL_TEST<Widget>([](/*Future<Widget>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut).share().getNoThrow().getStatus(), failStatus());
    });
}
#endif

TEST(Future_MoveOnly, Fail_getNothrowRvalue) {
    FUTURE_FAIL_TEST<Widget>([](/*Future<Widget>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut).getNoThrow().getStatus(), failStatus());
    });
}

TEST(Future_MoveOnly, Fail_getAsync) {
    FUTURE_FAIL_TEST<Widget>([](/*Future<Widget>*/ auto&& fut) {
        auto pf = makePromiseFuture<Widget>();
        std::move(fut).getAsync([outside = std::move(pf.promise)](StatusWith<Widget> sw) mutable {
            ASSERT(!sw.isOK());
            outside.setError(sw.getStatus());
        });
        ASSERT_EQ(std::move(pf.future).getNoThrow(), failStatus());
    });
}

TEST(Future_MoveOnly, Success_thenSimple) {
    FUTURE_SUCCESS_TEST([] { return Widget(1); },
                        [](/*Future<Widget>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut).then([](Widget i) { return i + 2; }).get(), 3);
                        });
}

TEST(Future_MoveOnly, Success_thenSimpleAuto) {
    FUTURE_SUCCESS_TEST([] { return Widget(1); },
                        [](/*Future<Widget>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut).then([](auto&& i) { return i + 2; }).get(), 3);
                        });
}

TEST(Future_MoveOnly, Success_thenVoid) {
    FUTURE_SUCCESS_TEST([] { return Widget(1); },
                        [](/*Future<Widget>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .then([](Widget i) { ASSERT_EQ(i, 1); })
                                          .then([] { return Widget(3); })
                                          .get(),
                                      3);
                        });
}

TEST(Future_MoveOnly, Success_thenStatus) {
    FUTURE_SUCCESS_TEST([] { return Widget(1); },
                        [](/*Future<Widget>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .then([](Widget i) {
                                              ASSERT_EQ(i, 1);
                                              return Status::OK();
                                          })
                                          .then([] { return Widget(3); })
                                          .get(),
                                      3);
                        });
}

TEST(Future_MoveOnly, Success_thenError_Status) {
    FUTURE_SUCCESS_TEST([] { return Widget(1); },
                        [](/*Future<Widget>*/ auto&& fut) {
                            auto fut2 = std::move(fut).then(
                                [](Widget i) { return Status(ErrorCodes::BadValue, "oh no!"); });
                            static_assert(future_details::isFutureLike<decltype(fut2)>);
                            static_assert(
                                std::is_same_v<typename decltype(fut2)::value_type, void>);
                            ASSERT_EQ(fut2.getNoThrow(), ErrorCodes::BadValue);
                        });
}

TEST(Future_MoveOnly, Success_thenError_StatusWith) {
    FUTURE_SUCCESS_TEST(
        [] { return Widget(1); },
        [](/*Future<Widget>*/ auto&& fut) {
            auto fut2 = std::move(fut).then(
                [](Widget i) { return StatusWith<double>(ErrorCodes::BadValue, "oh no!"); });
            static_assert(future_details::isFutureLike<decltype(fut2)>);
            static_assert(std::is_same_v<typename decltype(fut2)::value_type, double>);
            ASSERT_EQ(fut2.getNoThrow(), ErrorCodes::BadValue);
        });
}

TEST(Future_MoveOnly, Success_thenFutureImmediate) {
    FUTURE_SUCCESS_TEST(
        [] { return Widget(1); },
        [](/*Future<Widget>*/ auto&& fut) {
            ASSERT_EQ(std::move(fut)
                          .then([](Widget i) { return Future<Widget>::makeReady(Widget(i + 2)); })
                          .get(),
                      3);
        });
}

TEST(Future_MoveOnly, Success_thenFutureReady) {
    FUTURE_SUCCESS_TEST([] { return Widget(1); },
                        [](/*Future<Widget>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .then([](Widget i) {
                                              auto pf = makePromiseFuture<Widget>();
                                              pf.promise.emplaceValue(i + 2);
                                              return std::move(pf.future);
                                          })
                                          .get(),
                                      3);
                        });
}

TEST(Future_MoveOnly, Success_thenFutureAsync) {
    FUTURE_SUCCESS_TEST([] { return Widget(1); },
                        [](/*Future<Widget>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .then([&](Widget i) {
                                              return async([i = i.val] { return Widget(i + 2); });
                                          })
                                          .get(),
                                      3);
                        });
}

TEST(Future_MoveOnly, Success_thenSemiFutureAsync) {
    FUTURE_SUCCESS_TEST([] { return Widget(1); },
                        [](/*Future<Widget>*/ auto&& fut) {
                            ASSERT_EQ(
                                std::move(fut)
                                    .then([&](Widget i) {
                                        return async([i = i.val] { return Widget(i + 2); }).semi();
                                    })
                                    .get(),
                                3);
                        });
}

TEST(Future_MoveOnly, Success_thenFutureAsyncThrow) {
    FUTURE_SUCCESS_TEST([] { return Widget(1); },
                        [](/*Future<Widget>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .then([](Widget i) {
                                              uasserted(ErrorCodes::BadValue, "oh no!");
                                              return Future<Widget>();
                                          })
                                          .getNoThrow(),
                                      ErrorCodes::BadValue);
                        });
}

TEST(Future_MoveOnly, Fail_thenSimple) {
    FUTURE_FAIL_TEST<Widget>([](/*Future<Widget>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .then([](Widget i) {
                          FAIL("then() callback was called");
                          return Widget(0);
                      })
                      .getNoThrow(),
                  failStatus());
    });
}

TEST(Future_MoveOnly, Fail_thenFutureAsync) {
    FUTURE_FAIL_TEST<Widget>([](/*Future<Widget>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .then([](Widget i) {
                          FAIL("then() callback was called");
                          return Future<Widget>();
                      })
                      .getNoThrow(),
                  failStatus());
    });
}

TEST(Future_MoveOnly, Success_onErrorSimple) {
    FUTURE_SUCCESS_TEST([] { return Widget(1); },
                        [](/*Future<Widget>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .onError([](Status) {
                                              FAIL("onError() callback was called");
                                              return Widget(0);
                                          })
                                          .then([](Widget i) { return i + 2; })
                                          .get(),
                                      3);
                        });
}

TEST(Future_MoveOnly, Success_onErrorFutureAsync) {
    FUTURE_SUCCESS_TEST([] { return Widget(1); },
                        [](/*Future<Widget>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .onError([](Status) {
                                              FAIL("onError() callback was called");
                                              return Future<Widget>();
                                          })
                                          .then([](Widget i) { return i + 2; })
                                          .get(),
                                      3);
                        });
}

TEST(Future_MoveOnly, Fail_onErrorSimple) {
    FUTURE_FAIL_TEST<Widget>([](/*Future<Widget>*/ auto&& fut) {
        ASSERT_EQ(uassertStatusOK(std::move(fut)
                                      .onError([](Status s) {
                                          ASSERT_EQ(s, failStatus());
                                          return Widget(3);
                                      })
                                      .getNoThrow()),
                  3);
    });
}
TEST(Future_MoveOnly, Fail_onErrorError_throw) {
    FUTURE_FAIL_TEST<Widget>([](/*Future<Widget>*/ auto&& fut) {
        auto fut2 = std::move(fut).onError([](Status s) -> Widget {
            ASSERT_EQ(s, failStatus());
            uasserted(ErrorCodes::BadValue, "oh no!");
        });
        ASSERT_EQ(std::move(fut2).getNoThrow(), ErrorCodes::BadValue);
    });
}

TEST(Future_MoveOnly, Fail_onErrorError_StatusWith) {
    FUTURE_FAIL_TEST<Widget>([](/*Future<Widget>*/ auto&& fut) {
        auto fut2 = std::move(fut).onError([](Status s) {
            ASSERT_EQ(s, failStatus());
            return StatusWith<Widget>(ErrorCodes::BadValue, "oh no!");
        });
        ASSERT_EQ(std::move(fut2).getNoThrow(), ErrorCodes::BadValue);
    });
}

TEST(Future_MoveOnly, Fail_onErrorFutureImmediate) {
    FUTURE_FAIL_TEST<Widget>([](/*Future<Widget>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onError([](Status s) {
                          ASSERT_EQ(s, failStatus());
                          return Future<Widget>::makeReady(Widget(3));
                      })
                      .get(),
                  3);
    });
}

TEST(Future_MoveOnly, Fail_onErrorFutureReady) {
    FUTURE_FAIL_TEST<Widget>([](/*Future<Widget>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onError([](Status s) {
                          ASSERT_EQ(s, failStatus());
                          auto pf = makePromiseFuture<Widget>();
                          pf.promise.emplaceValue(3);
                          return std::move(pf.future);
                      })
                      .get(),
                  3);
    });
}

TEST(Future_MoveOnly, Fail_onErrorFutureAsync) {
    FUTURE_FAIL_TEST<Widget>([](/*Future<Widget>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onError([&](Status s) {
                          ASSERT_EQ(s, failStatus());
                          return async([] { return Widget(3); });
                      })
                      .get(),
                  3);
    });
}

TEST(Future_MoveOnly, Success_tap) {
    FUTURE_SUCCESS_TEST<kNoExecutorFuture_needsTap>(
        [] { return Widget(1); },
        [](/*Future<Widget>*/ auto&& fut) {
            bool tapCalled = false;
            ASSERT_EQ(std::move(fut)
                          .tap([&tapCalled](const Widget& i) {
                              ASSERT_EQ(i, 1);
                              tapCalled = true;
                          })
                          .then([](Widget i) { return i + 2; })
                          .get(),
                      3);
            ASSERT(tapCalled);
        });
}

TEST(Future_MoveOnly, Success_tapError) {
    FUTURE_SUCCESS_TEST<kNoExecutorFuture_needsTap>(
        [] { return Widget(1); },
        [](/*Future<Widget>*/ auto&& fut) {
            ASSERT_EQ(std::move(fut)
                          .tapError([](Status s) { FAIL("tapError() callback was called"); })
                          .then([](Widget i) { return i + 2; })
                          .get(),
                      3);
        });
}

#if 0  // Needs copy
TEST(Future_MoveOnly, Success_tapAll_StatusWith) {
    FUTURE_SUCCESS_TEST<kNoExecutorFuture_needsTap>(
        [] { return Widget(1); },
        [](/*Future<Widget>*/ auto&& fut) {
            bool tapCalled = false;
            ASSERT_EQ(std::move(fut)
                          .tapAll([&tapCalled](StatusWith<Widget> sw) {
                              ASSERT_EQ(uassertStatusOK(sw).val, 1);
                              tapCalled = true;
                          })
                          .then([](Widget i) { return i + 2; })
                          .get(),
                      3);
            ASSERT(tapCalled);
        });
}
#endif

TEST(Future_MoveOnly, Fail_tap) {
    FUTURE_FAIL_TEST<Widget, kNoExecutorFuture_needsTap>([](/*Future<Widget>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .tap([](const Widget& i) { FAIL("tap() callback was called"); })
                      .onError([](Status s) {
                          ASSERT_EQ(s, failStatus());
                          return Widget(3);
                      })
                      .get(),
                  3);
    });
}

TEST(Future_MoveOnly, Fail_tapError) {
    FUTURE_FAIL_TEST<Widget, kNoExecutorFuture_needsTap>([](/*Future<Widget>*/ auto&& fut) {
        bool tapCalled = false;
        ASSERT_EQ(std::move(fut)
                      .tapError([&tapCalled](Status s) {
                          ASSERT_EQ(s, failStatus());
                          tapCalled = true;
                      })
                      .onError([](Status s) {
                          ASSERT_EQ(s, failStatus());
                          return Widget(3);
                      })
                      .get(),
                  3);
        ASSERT(tapCalled);
    });
}

#if 0  // Needs copy
TEST(Future_MoveOnly, Fail_tapAll_StatusWith) {
    FUTURE_FAIL_TEST<Widget, kNoExecutorFuture_needsTap>( [](/*Future<Widget>*/ auto&& fut) {
        bool tapCalled = false;
        ASSERT_EQ(std::move(fut)
                      .tapAll([&tapCalled](StatusWith<Widget> sw) {
                          ASSERT_EQ(sw.getStatus(), failStatus());
                          tapCalled = true;
                      })
                      .onError([](Status s) {
                          ASSERT_EQ(s, failStatus());
                          return Widget(3);
                      })
                      .get(),
                  3);
        ASSERT(tapCalled);
    });
}
#endif

TEST(Future_MoveOnly, Success_onCompletionSimple) {
    FUTURE_SUCCESS_TEST(
        [] { return Widget(1); },
        [](/*Future<Widget>*/ auto&& fut) {
            ASSERT_EQ(std::move(fut)
                          .onCompletion([](StatusWith<Widget> i) { return i.getValue() + 2; })
                          .get(),
                      3);
        });
}

TEST(Future_MoveOnly, Success_onCompletionVoid) {
    FUTURE_SUCCESS_TEST(
        [] { return Widget(1); },
        [](/*Future<Widget>*/ auto&& fut) {
            ASSERT_EQ(std::move(fut)
                          .onCompletion([](StatusWith<Widget> i) { ASSERT_EQ(i.getValue(), 1); })
                          .onCompletion([](Status s) {
                              ASSERT_OK(s);
                              return Widget(3);
                          })
                          .get(),
                      3);
        });
}

TEST(Future_MoveOnly, Success_onCompletionStatus) {
    FUTURE_SUCCESS_TEST([] { return Widget(1); },
                        [](/*Future<Widget>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .onCompletion([](StatusWith<Widget> i) {
                                              ASSERT_EQ(i.getValue(), 1);
                                              return Status::OK();
                                          })
                                          .onCompletion([](Status s) {
                                              ASSERT_OK(s);
                                              return Widget(3);
                                          })
                                          .get(),
                                      3);
                        });
}

TEST(Future_MoveOnly, Success_onCompletionError_Status) {
    FUTURE_SUCCESS_TEST(
        [] { return Widget(1); },
        [](/*Future<Widget>*/ auto&& fut) {
            auto fut2 = std::move(fut).onCompletion(
                [](StatusWith<Widget> i) { return Status(ErrorCodes::BadValue, "oh no!"); });
            static_assert(future_details::isFutureLike<decltype(fut2)>);
            static_assert(std::is_same_v<typename decltype(fut2)::value_type, void>);
            ASSERT_EQ(fut2.getNoThrow(), ErrorCodes::BadValue);
        });
}

TEST(Future_MoveOnly, Success_onCompletionError_StatusWith) {
    FUTURE_SUCCESS_TEST([] { return Widget(1); },
                        [](/*Future<Widget>*/ auto&& fut) {
                            auto fut2 = std::move(fut).onCompletion([](StatusWith<Widget> i) {
                                return StatusWith<double>(ErrorCodes::BadValue, "oh no!");
                            });
                            static_assert(future_details::isFutureLike<decltype(fut2)>);
                            static_assert(
                                std::is_same_v<typename decltype(fut2)::value_type, double>);
                            ASSERT_EQ(fut2.getNoThrow(), ErrorCodes::BadValue);
                        });
}

TEST(Future_MoveOnly, Success_onCompletionFutureImmediate) {
    FUTURE_SUCCESS_TEST([] { return Widget(1); },
                        [](/*Future<Widget>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .onCompletion([](StatusWith<Widget> i) {
                                              return Future<Widget>::makeReady(
                                                  Widget(i.getValue() + 2));
                                          })
                                          .get(),
                                      3);
                        });
}

TEST(Future_MoveOnly, Success_onCompletionFutureReady) {
    FUTURE_SUCCESS_TEST([] { return Widget(1); },
                        [](/*Future<Widget>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .onCompletion([](StatusWith<Widget> i) {
                                              auto pf = makePromiseFuture<Widget>();
                                              pf.promise.emplaceValue(i.getValue() + 2);
                                              return std::move(pf.future);
                                          })
                                          .get(),
                                      3);
                        });
}

TEST(Future_MoveOnly, Success_onCompletionFutureAsync) {
    FUTURE_SUCCESS_TEST([] { return Widget(1); },
                        [](/*Future<Widget>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .onCompletion([&](StatusWith<Widget> i) {
                                              return async(
                                                  [i = i.getValue().val] { return Widget(i + 2); });
                                          })
                                          .get(),
                                      3);
                        });
}

TEST(Future_MoveOnly, Success_onCompletionFutureAsyncThrow) {
    FUTURE_SUCCESS_TEST([] { return Widget(1); },
                        [](/*Future<Widget>*/ auto&& fut) {
                            ASSERT_EQ(std::move(fut)
                                          .onCompletion([](StatusWith<Widget> i) {
                                              uasserted(ErrorCodes::BadValue, "oh no!");
                                              return Future<Widget>();
                                          })
                                          .getNoThrow(),
                                      ErrorCodes::BadValue);
                        });
}

TEST(Future_MoveOnly, Fail_onCompletionSimple) {
    FUTURE_FAIL_TEST<Widget>([](/*Future<Widget>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onCompletion([](StatusWith<Widget> i) {
                          ASSERT_NOT_OK(i);
                          return i.getStatus();
                      })
                      .getNoThrow(),
                  failStatus());
    });
}

TEST(Future_MoveOnly, Fail_onCompletionFutureAsync) {
    FUTURE_FAIL_TEST<Widget>([](/*Future<Widget>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onCompletion([](StatusWith<Widget> i) {
                          ASSERT_NOT_OK(i);
                          return i.getStatus();
                      })
                      .getNoThrow(),
                  failStatus());
    });
}

TEST(Future_MoveOnly, Fail_onCompletionError_throw) {
    FUTURE_FAIL_TEST<Widget>([](/*Future<Widget>*/ auto&& fut) {
        auto fut2 = std::move(fut).onCompletion([](StatusWith<Widget> s) -> Widget {
            ASSERT_EQ(s.getStatus(), failStatus());
            uasserted(ErrorCodes::BadValue, "oh no!");
        });
        ASSERT_EQ(std::move(fut2).getNoThrow(), ErrorCodes::BadValue);
    });
}

TEST(Future_MoveOnly, Fail_onCompletionError_StatusWith) {
    FUTURE_FAIL_TEST<Widget>([](/*Future<Widget>*/ auto&& fut) {
        auto fut2 = std::move(fut).onCompletion([](StatusWith<Widget> s) {
            ASSERT_EQ(s.getStatus(), failStatus());
            return StatusWith<Widget>(ErrorCodes::BadValue, "oh no!");
        });
        ASSERT_EQ(std::move(fut2).getNoThrow(), ErrorCodes::BadValue);
    });
}

TEST(Future_MoveOnly, Fail_onCompletionFutureImmediate) {
    FUTURE_FAIL_TEST<Widget>([](/*Future<Widget>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onCompletion([](StatusWith<Widget> s) {
                          ASSERT_EQ(s.getStatus(), failStatus());
                          return Future<Widget>::makeReady(Widget(3));
                      })
                      .get(),
                  3);
    });
}

TEST(Future_MoveOnly, Fail_onCompletionFutureReady) {
    FUTURE_FAIL_TEST<Widget>([](/*Future<Widget>*/ auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onCompletion([](StatusWith<Widget> s) {
                          ASSERT_EQ(s.getStatus(), failStatus());
                          auto pf = makePromiseFuture<Widget>();
                          pf.promise.emplaceValue(3);
                          return std::move(pf.future);
                      })
                      .get(),
                  3);
    });
}

// Tests for containers of move-only types.
TEST(Future_MoveOnly, Success_vector) {
    FUTURE_SUCCESS_TEST(
        [] {
            std::vector<Widget> vec;
            vec.emplace_back(1);
            return vec;
        },
        [](/*Future<vector<Widget>>*/ auto&& fut) { ASSERT_EQ(fut.get()[0], 1); });
}

TEST(Future_MoveOnly, Success_list) {
    FUTURE_SUCCESS_TEST(
        [] {
            std::list<Widget> lst;
            lst.emplace_back(1);
            return lst;
        },
        [](/*Future<list<Widget>>*/ auto&& fut) { ASSERT_EQ(fut.get().front(), 1); });
}

}  // namespace
}  // namespace mongo
