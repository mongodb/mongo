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
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/future_test_utils.h"

#include <list>
#include <map>
#include <memory>
#include <ostream>
#include <set>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {

// A move-only type that isn't default constructible. It has binary ops with int.
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

    friend Widget operator+(const Widget& w, int i) {
        return Widget(w.val + i);
    }

    friend bool operator==(const Widget& a, const Widget& b) {
        return a.val == b.val;
    }
    friend bool operator<(const Widget& a, const Widget& b) {
        return a.val < b.val;
    }

    template <typename H>
    friend H AbslHashValue(H h, const Widget& w) {
        return H::combine(std::move(h), w.val);
    }

    int val;
};

std::ostream& operator<<(std::ostream& stream, const Widget& widget) {
    return stream << "Widget(" << widget.val << ')';
}

/* Run the success tests using a Widget(1) completion function. */
template <DoExecutorFuture doExecutorFuture = kDoExecutorFuture, typename F>
void runTests(F&& f) {
    FUTURE_SUCCESS_TEST<doExecutorFuture>([] { return Widget(1); }, std::forward<F>(f));
}

TEST(Future_MoveOnly, Success_getLvalue) {
    runTests([](auto&& fut) { ASSERT_EQ(fut.get(), Widget(1)); });
}

TEST(Future_MoveOnly, Success_getConstLvalue) {
    runTests([](const auto& fut) { ASSERT_EQ(fut.get(), Widget(1)); });
}

TEST(Future_MoveOnly, Success_getRvalue) {
    runTests([](auto&& fut) { ASSERT_EQ(std::move(fut).get(), Widget(1)); });
}

TEST(Future_MoveOnly, Success_semi_get) {
    runTests([](auto&& fut) { ASSERT_EQ(std::move(fut).semi().get(), Widget(1)); });
}

#if 0  // Needs copy
TEST(Future_MoveOnly, Success_shared_get) {
    runTests([](auto&& fut) { ASSERT_EQ(std::move(fut).share().get(), Widget(1)); });
}

TEST(Future_MoveOnly, Success_getNothrowLvalue) {
    runTests([](auto&& fut) { ASSERT_EQ(fut.getNoThrow(), Widget(1)); });
}

TEST(Future_MoveOnly, Success_getNothrowConstLvalue) {
    runTests([](auto&& fut) { ASSERT_EQ(fut.getNoThrow(), Widget(1)); });
}

TEST(Future_MoveOnly, Success_shared_getNothrow) {
    runTests([](auto&& fut) { ASSERT_EQ(std::move(fut).share().getNoThrow(), Widget(1)); });
}
#endif

TEST(Future_MoveOnly, Success_getNothrowRvalue) {
    runTests([](auto&& fut) { ASSERT_EQ(uassertStatusOK(std::move(fut).getNoThrow()).val, 1); });
}

TEST(Future_MoveOnly, Success_getAsync) {
    runTests([](auto&& fut) {
        auto pf = makePromiseFuture<Widget>();
        std::move(fut).getAsync([outside = std::move(pf.promise)](StatusWith<Widget> sw) mutable {
            ASSERT_OK(sw);
            outside.emplaceValue(std::move(sw.getValue()));
        });
        ASSERT_EQ(std::move(pf.future).get(), Widget(1));
    });
}

TEST(Future_MoveOnly, Fail_getLvalue) {
    FUTURE_FAIL_TEST<Widget>([](auto&& fut) { ASSERT_THROWS_failStatus(fut.get()); });
}

TEST(Future_MoveOnly, Fail_getConstLvalue) {
    FUTURE_FAIL_TEST<Widget>([](const auto& fut) { ASSERT_THROWS_failStatus(fut.get()); });
}

TEST(Future_MoveOnly, Fail_getRvalue) {
    FUTURE_FAIL_TEST<Widget>([](auto&& fut) { ASSERT_THROWS_failStatus(std::move(fut).get()); });
}

TEST(Future_MoveOnly, Fail_semi_get) {
    FUTURE_FAIL_TEST<Widget>(
        [](auto&& fut) { ASSERT_THROWS_failStatus(std::move(fut).semi().get()); });
}

#if 0  // Needs copy
TEST(Future_MoveOnly, Fail_shared_get) {
    FUTURE_FAIL_TEST<Widget>([](auto&& fut) {
        ASSERT_THROWS_failStatus(std::move(fut).share().get());
    });
}

TEST(Future_MoveOnly, Fail_getNothrowLvalue) {
    FUTURE_FAIL_TEST<Widget>([](auto&& fut) { ASSERT_EQ(fut.getNoThrow(), failStatus); });
}

TEST(Future_MoveOnly, Fail_getNothrowConstLvalue) {
    FUTURE_FAIL_TEST<Widget>(
        [](const auto& fut) { ASSERT_EQ(fut.getNoThrow(), failStatus); });
}

TEST(Future_MoveOnly, Fail_shared_getNothrow) {
    FUTURE_FAIL_TEST<Widget>([](auto&& fut) {
        ASSERT_EQ(std::move(fut).share().getNoThrow().getStatus(), failStatus());
    });
}
#endif

TEST(Future_MoveOnly, Fail_getNothrowRvalue) {
    FUTURE_FAIL_TEST<Widget>(
        [](auto&& fut) { ASSERT_EQ(std::move(fut).getNoThrow().getStatus(), failStatus()); });
}

TEST(Future_MoveOnly, Fail_getAsync) {
    FUTURE_FAIL_TEST<Widget>([](auto&& fut) {
        auto pf = makePromiseFuture<Widget>();
        std::move(fut).getAsync([outside = std::move(pf.promise)](StatusWith<Widget> sw) mutable {
            ASSERT(!sw.isOK());
            outside.setError(sw.getStatus());
        });
        ASSERT_EQ(std::move(pf.future).getNoThrow(), failStatus());
    });
}

TEST(Future_MoveOnly, Success_thenSimple) {
    runTests([](auto&& fut) {
        ASSERT_EQ(std::move(fut).then([](Widget i) FTU_LAMBDA_R(Widget) { return i + 2; }).get(),
                  Widget(3));
    });
}

TEST(Future_MoveOnly, Success_thenSimpleAuto) {
    runTests([](auto&& fut) {
        ASSERT_EQ(std::move(fut).then([](auto&& i) { return i + 2; }).get(), Widget(3));
    });
}

TEST(Future_MoveOnly, Success_thenVoid) {
    runTests([](auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .then([](Widget i) FTU_LAMBDA_R(void) { ASSERT_EQ(i, Widget(1)); })
                      .then([]() FTU_LAMBDA_R(Widget) { return Widget(3); })
                      .get(),
                  Widget(3));
    });
}

TEST(Future_MoveOnly, Success_thenStatus) {
    runTests([](auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .then([](Widget i) FTU_LAMBDA_R(Status) {
                          ASSERT_EQ(i, Widget(1));
                          return Status::OK();
                      })
                      .then([] { return Widget(3); })
                      .get(),
                  Widget(3));
    });
}

TEST(Future_MoveOnly, Success_thenError_Status) {
    runTests([](auto&& fut) {
        auto fut2 = std::move(fut).then(
            [](Widget i) FTU_LAMBDA_R(Status) { return Status(ErrorCodes::BadValue, "oh no!"); });
        static_assert(future_details::isFutureLike<decltype(fut2)>);
        static_assert(std::is_same_v<typename decltype(fut2)::value_type, void>);
        ASSERT_EQ(fut2.getNoThrow(), ErrorCodes::BadValue);
    });
}

TEST(Future_MoveOnly, Success_thenError_StatusWith) {
    runTests([](auto&& fut) {
        auto fut2 = std::move(fut).then([](Widget i) FTU_LAMBDA_R(StatusWith<double>) {
            return StatusWith<double>(ErrorCodes::BadValue, "oh no!");
        });
        static_assert(future_details::isFutureLike<decltype(fut2)>);
        static_assert(std::is_same_v<typename decltype(fut2)::value_type, double>);
        ASSERT_EQ(fut2.getNoThrow(), ErrorCodes::BadValue);
    });
}

TEST(Future_MoveOnly, Success_thenFutureImmediate) {
    runTests([](auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .then([](Widget i) FTU_LAMBDA_R(Future<Widget>) {
                          return Future<Widget>::makeReady(Widget(i + 2));
                      })
                      .get(),
                  Widget(3));
    });
}

TEST(Future_MoveOnly, Success_thenFutureReady) {
    runTests([](auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .then([](Widget i) FTU_LAMBDA_R(Future<Widget>) {
                          auto pf = makePromiseFuture<Widget>();
                          pf.promise.emplaceValue(i + 2);
                          return std::move(pf.future);
                      })
                      .get(),
                  Widget(3));
    });
}

TEST(Future_MoveOnly, Success_thenFutureAsync) {
    runTests([](auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .then([&](Widget i) { return async([i = i.val] { return Widget(i + 2); }); })
                      .get(),
                  Widget(3));
    });
}

TEST(Future_MoveOnly, Success_thenSemiFutureAsync) {
    runTests([](auto&& fut) {
        ASSERT_EQ(
            std::move(fut)
                .then([&](Widget i) { return async([i = i.val] { return Widget(i + 2); }).semi(); })
                .get(),
            Widget(3));
    });
}

TEST(Future_MoveOnly, Success_thenFutureAsyncThrow) {
    runTests([](auto&& fut) {
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
    FUTURE_FAIL_TEST<Widget>([](auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .then([](Widget i) FTU_LAMBDA_R(Widget) {
                          FAIL("then() callback was called");
                          return Widget(0);
                      })
                      .getNoThrow(),
                  failStatus());
    });
}

TEST(Future_MoveOnly, Fail_thenFutureAsync) {
    FUTURE_FAIL_TEST<Widget>([](auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .then([](Widget i) FTU_LAMBDA_R(Future<Widget>) {
                          FAIL("then() callback was called");
                          return Future<Widget>();
                      })
                      .getNoThrow(),
                  failStatus());
    });
}

TEST(Future_MoveOnly, Success_onErrorSimple) {
    runTests([](auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onError([](Status) FTU_LAMBDA_R(Widget) {
                          FAIL("onError() callback was called");
                          return Widget(0);
                      })
                      .then([](Widget i) FTU_LAMBDA_R(Widget) { return i + 2; })
                      .get(),
                  Widget(3));
    });
}

TEST(Future_MoveOnly, Success_onErrorFutureAsync) {
    runTests([](auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onError([](Status) FTU_LAMBDA_R(Future<Widget>) {
                          FAIL("onError() callback was called");
                          return Future<Widget>();
                      })
                      .then([](Widget i) FTU_LAMBDA_R(Widget) { return i + 2; })
                      .get(),
                  Widget(3));
    });
}

TEST(Future_MoveOnly, Fail_onErrorSimple) {
    FUTURE_FAIL_TEST<Widget>([](auto&& fut) {
        ASSERT_EQ(uassertStatusOK(std::move(fut)
                                      .onError([](Status s) FTU_LAMBDA_R(Widget) {
                                          ASSERT_EQ(s, failStatus());
                                          return Widget(3);
                                      })
                                      .getNoThrow()),
                  Widget(3));
    });
}
TEST(Future_MoveOnly, Fail_onErrorError_throw) {
    FUTURE_FAIL_TEST<Widget>([](auto&& fut) {
        auto fut2 = std::move(fut).onError([](Status s) -> Widget {
            ASSERT_EQ(s, failStatus());
            uasserted(ErrorCodes::BadValue, "oh no!");
        });
        ASSERT_EQ(std::move(fut2).getNoThrow(), ErrorCodes::BadValue);
    });
}

TEST(Future_MoveOnly, Fail_onErrorError_StatusWith) {
    FUTURE_FAIL_TEST<Widget>([](auto&& fut) {
        auto fut2 = std::move(fut).onError([](Status s) FTU_LAMBDA_R(StatusWith<Widget>) {
            ASSERT_EQ(s, failStatus());
            return StatusWith<Widget>(ErrorCodes::BadValue, "oh no!");
        });
        ASSERT_EQ(std::move(fut2).getNoThrow(), ErrorCodes::BadValue);
    });
}

TEST(Future_MoveOnly, Fail_onErrorFutureImmediate) {
    FUTURE_FAIL_TEST<Widget>([](auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onError([](Status s) FTU_LAMBDA_R(Future<Widget>) {
                          ASSERT_EQ(s, failStatus());
                          return Future<Widget>::makeReady(Widget(3));
                      })
                      .get(),
                  Widget(3));
    });
}

TEST(Future_MoveOnly, Fail_onErrorFutureReady) {
    FUTURE_FAIL_TEST<Widget>([](auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onError([](Status s) FTU_LAMBDA_R(Future<Widget>) {
                          ASSERT_EQ(s, failStatus());
                          auto pf = makePromiseFuture<Widget>();
                          pf.promise.emplaceValue(3);
                          return std::move(pf.future);
                      })
                      .get(),
                  Widget(3));
    });
}

TEST(Future_MoveOnly, Fail_onErrorFutureAsync) {
    FUTURE_FAIL_TEST<Widget>([](auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onError([&](Status s) FTU_LAMBDA_R(Future<Widget>) {
                          ASSERT_EQ(s, failStatus());
                          return async([] { return Widget(3); });
                      })
                      .get(),
                  Widget(3));
    });
}

TEST(Future_MoveOnly, Success_tap) {
    runTests<kNoExecutorFuture_needsTap>([](auto&& fut) {
        bool tapCalled = false;
        ASSERT_EQ(std::move(fut)
                      .tap([&tapCalled](const Widget& i) {
                          ASSERT_EQ(i, Widget(1));
                          tapCalled = true;
                      })
                      .then([](Widget i) FTU_LAMBDA_R(Widget) { return i + 2; })
                      .get(),
                  Widget(3));
        ASSERT(tapCalled);
    });
}

TEST(Future_MoveOnly, Success_tapError) {
    runTests<kNoExecutorFuture_needsTap>([](auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .tapError([](Status s)
                                    FTU_LAMBDA_R(void) { FAIL("tapError() callback was called"); })
                      .then([](Widget i) FTU_LAMBDA_R(Widget) { return i + 2; })
                      .get(),
                  Widget(3));
    });
}

#if 0  // Needs copy
TEST(Future_MoveOnly, Success_tapAll_StatusWith) {
    runTests<kNoExecutorFuture_needsTap>([](auto&& fut) {
            bool tapCalled = false;
            ASSERT_EQ(std::move(fut)
                          .tapAll([&tapCalled](StatusWith<Widget> sw) {
                              ASSERT_EQ(uassertStatusOK(sw).val, 1);
                              tapCalled = true;
                          })
                          .then([](Widget i) { return i + 2; })
                          .get(),
                      Widget(3));
            ASSERT(tapCalled);
        });
}
#endif

TEST(Future_MoveOnly, Fail_tap) {
    FUTURE_FAIL_TEST<Widget, kNoExecutorFuture_needsTap>([](auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .tap([](const Widget& i) { FAIL("tap() callback was called"); })
                      .onError([](Status s) FTU_LAMBDA_R(Widget) {
                          ASSERT_EQ(s, failStatus());
                          return Widget(3);
                      })
                      .get(),
                  Widget(3));
    });
}

TEST(Future_MoveOnly, Fail_tapError) {
    FUTURE_FAIL_TEST<Widget, kNoExecutorFuture_needsTap>([](auto&& fut) {
        bool tapCalled = false;
        ASSERT_EQ(std::move(fut)
                      .tapError([&tapCalled](Status s) {
                          ASSERT_EQ(s, failStatus());
                          tapCalled = true;
                      })
                      .onError([](Status s) FTU_LAMBDA_R(Widget) {
                          ASSERT_EQ(s, failStatus());
                          return Widget(3);
                      })
                      .get(),
                  Widget(3));
        ASSERT(tapCalled);
    });
}

#if 0  // Needs copy
TEST(Future_MoveOnly, Fail_tapAll_StatusWith) {
    FUTURE_FAIL_TEST<Widget, kNoExecutorFuture_needsTap>( [](auto&& fut) {
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
    runTests([](auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onCompletion([](StatusWith<Widget> i)
                                        FTU_LAMBDA_R(Widget) { return i.getValue() + 2; })
                      .get(),
                  Widget(3));
    });
}

TEST(Future_MoveOnly, Success_onCompletionVoid) {
    runTests([](auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onCompletion([](StatusWith<Widget> i)
                                        FTU_LAMBDA_R(void) { ASSERT_EQ(i.getValue(), Widget(1)); })
                      .onCompletion([](Status s) FTU_LAMBDA_R(Widget) {
                          ASSERT_OK(s);
                          return Widget(3);
                      })
                      .get(),
                  Widget(3));
    });
}

TEST(Future_MoveOnly, Success_onCompletionStatus) {
    runTests([](auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onCompletion([](StatusWith<Widget> i) FTU_LAMBDA_R(Status) {
                          ASSERT_EQ(i.getValue(), Widget(1));
                          return Status::OK();
                      })
                      .onCompletion([](Status s) FTU_LAMBDA_R(Widget) {
                          ASSERT_OK(s);
                          return Widget(3);
                      })
                      .get(),
                  Widget(3));
    });
}

TEST(Future_MoveOnly, Success_onCompletionError_Status) {
    runTests([](auto&& fut) {
        auto fut2 = std::move(fut).onCompletion([](StatusWith<Widget> i) FTU_LAMBDA_R(Status) {
            return Status(ErrorCodes::BadValue, "oh no!");
        });
        static_assert(future_details::isFutureLike<decltype(fut2)>);
        static_assert(std::is_same_v<typename decltype(fut2)::value_type, void>);
        ASSERT_EQ(fut2.getNoThrow(), ErrorCodes::BadValue);
    });
}

TEST(Future_MoveOnly, Success_onCompletionError_StatusWith) {
    runTests([](auto&& fut) {
        auto fut2 =
            std::move(fut).onCompletion([](StatusWith<Widget> i) FTU_LAMBDA_R(StatusWith<double>) {
                return StatusWith<double>(ErrorCodes::BadValue, "oh no!");
            });
        static_assert(future_details::isFutureLike<decltype(fut2)>);
        static_assert(std::is_same_v<typename decltype(fut2)::value_type, double>);
        ASSERT_EQ(fut2.getNoThrow(), ErrorCodes::BadValue);
    });
}

TEST(Future_MoveOnly, Success_onCompletionFutureImmediate) {
    runTests([](auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onCompletion([](StatusWith<Widget> i) FTU_LAMBDA_R(Future<Widget>) {
                          return Future<Widget>::makeReady(Widget(i.getValue() + 2));
                      })
                      .get(),
                  Widget(3));
    });
}

TEST(Future_MoveOnly, Success_onCompletionFutureReady) {
    runTests([](auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onCompletion([](StatusWith<Widget> i) FTU_LAMBDA_R(Future<Widget>) {
                          auto pf = makePromiseFuture<Widget>();
                          pf.promise.emplaceValue(i.getValue() + 2);
                          return std::move(pf.future);
                      })
                      .get(),
                  Widget(3));
    });
}

TEST(Future_MoveOnly, Success_onCompletionFutureAsync) {
    runTests([](auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onCompletion([&](StatusWith<Widget> i) {
                          return async([i = i.getValue().val] { return Widget(i + 2); });
                      })
                      .get(),
                  Widget(3));
    });
}

TEST(Future_MoveOnly, Success_onCompletionFutureAsyncThrow) {
    runTests([](auto&& fut) {
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
    FUTURE_FAIL_TEST<Widget>([](auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onCompletion([](StatusWith<Widget> i) FTU_LAMBDA_R(Status) {
                          ASSERT_NOT_OK(i);
                          return i.getStatus();
                      })
                      .getNoThrow(),
                  failStatus());
    });
}

TEST(Future_MoveOnly, Fail_onCompletionFutureAsync) {
    FUTURE_FAIL_TEST<Widget>([](auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onCompletion([](StatusWith<Widget> i) FTU_LAMBDA_R(Status) {
                          ASSERT_NOT_OK(i);
                          return i.getStatus();
                      })
                      .getNoThrow(),
                  failStatus());
    });
}

TEST(Future_MoveOnly, Fail_onCompletionError_throw) {
    FUTURE_FAIL_TEST<Widget>([](auto&& fut) {
        auto fut2 = std::move(fut).onCompletion([](StatusWith<Widget> s) -> Widget {
            ASSERT_EQ(s.getStatus(), failStatus());
            uasserted(ErrorCodes::BadValue, "oh no!");
        });
        ASSERT_EQ(std::move(fut2).getNoThrow(), ErrorCodes::BadValue);
    });
}

TEST(Future_MoveOnly, Fail_onCompletionError_StatusWith) {
    FUTURE_FAIL_TEST<Widget>([](auto&& fut) {
        auto fut2 =
            std::move(fut).onCompletion([](StatusWith<Widget> s) FTU_LAMBDA_R(StatusWith<Widget>) {
                ASSERT_EQ(s.getStatus(), failStatus());
                return StatusWith<Widget>(ErrorCodes::BadValue, "oh no!");
            });
        ASSERT_EQ(std::move(fut2).getNoThrow(), ErrorCodes::BadValue);
    });
}

TEST(Future_MoveOnly, Fail_onCompletionFutureImmediate) {
    FUTURE_FAIL_TEST<Widget>([](auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onCompletion([](StatusWith<Widget> s) FTU_LAMBDA_R(Future<Widget>) {
                          ASSERT_EQ(s.getStatus(), failStatus());
                          return Future<Widget>::makeReady(Widget(3));
                      })
                      .get(),
                  Widget(3));
    });
}

TEST(Future_MoveOnly, Fail_onCompletionFutureReady) {
    FUTURE_FAIL_TEST<Widget>([](auto&& fut) {
        ASSERT_EQ(std::move(fut)
                      .onCompletion([](StatusWith<Widget> s) FTU_LAMBDA_R(Future<Widget>) {
                          ASSERT_EQ(s.getStatus(), failStatus());
                          auto pf = makePromiseFuture<Widget>();
                          pf.promise.emplaceValue(3);
                          return std::move(pf.future);
                      })
                      .get(),
                  Widget(3));
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
        [](auto&& fut) { ASSERT_EQ(fut.get()[0], Widget(1)); });
}

TEST(Future_MoveOnly, Success_list) {
    FUTURE_SUCCESS_TEST(
        [] {
            std::list<Widget> lst;
            lst.emplace_back(1);
            return lst;
        },
        [](auto&& fut) { ASSERT_EQ(fut.get().front(), Widget(1)); });
}

TEST(Future_MoveOnly, Success_set) {
    FUTURE_SUCCESS_TEST(
        [] {
            std::set<Widget> set;
            set.emplace(1);
            return set;
        },
        [](auto&& fut) { ASSERT_EQ(fut.get().count(Widget{1}), 1); });
}

TEST(Future_MoveOnly, Success_unordered_set) {
    FUTURE_SUCCESS_TEST(
        [] {
            stdx::unordered_set<Widget> set;
            set.emplace(1);
            return set;
        },
        [](auto&& fut) { ASSERT_EQ(fut.get().count(Widget{1}), 1); });
}

TEST(Future_MoveOnly, Success_pair) {
    FUTURE_SUCCESS_TEST([] { return std::pair<Widget, Widget>{1, 1}; },
                        [](auto&& fut) {
                            auto&& pair = fut.get();
                            ASSERT_EQ(pair.first, Widget(1));
                            ASSERT_EQ(pair.second, Widget(1));
                        });
}

TEST(Future_MoveOnly, Success_map) {
    FUTURE_SUCCESS_TEST(
        [] {
            std::map<Widget, Widget> map;
            map.emplace(1, 1);
            return map;
        },
        [](auto&& fut) { ASSERT_EQ(fut.get().at(Widget{1}).val, 1); });
}

TEST(Future_MoveOnly, Success_unordered_map) {
    FUTURE_SUCCESS_TEST(
        [] {
            stdx::unordered_map<Widget, Widget> map;
            map.emplace(1, 1);
            return map;
        },
        [](auto&& fut) { ASSERT_EQ(fut.get().at(Widget{1}).val, 1); });
}

TEST(Future_MoveOnly, Success_nested_container) {
    FUTURE_SUCCESS_TEST(
        [] {
            std::vector<std::vector<Widget>> outer;
            std::vector<Widget> inner;
            inner.emplace_back(1);
            outer.push_back(std::move(inner));

            return outer;
        },
        [](auto&& fut) { ASSERT_EQ(fut.get()[0][0], Widget(1)); });
}


}  // namespace
}  // namespace mongo
