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

#include <string>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

TEST(StatusWith, MakeCtad) {
    auto validate = [](auto&& arg) {
        auto sw = StatusWith(arg);
        ASSERT_TRUE(sw.isOK());
        ASSERT_TRUE(uassertStatusOK(sw) == arg);
        using Arg = std::decay_t<decltype(arg)>;
        return std::is_same_v<decltype(sw), StatusWith<Arg>>;
    };
    ASSERT_TRUE(validate(3));
    ASSERT_TRUE(validate(false));
    ASSERT_TRUE(validate(123.45));
    ASSERT_TRUE(validate(std::string("foo")));
    ASSERT_TRUE(validate(std::vector<int>()));
    ASSERT_TRUE(validate(std::vector<int>({1, 2, 3})));
}

/** Check uassertStatusOK with various reference types */
TEST(StatusWith, UassertStatusOKReferenceTypes) {
    auto sd = "barbaz"_sd;
    auto sw = StatusWith(sd);

    const StatusWith<StringData>& cref = sw;
    ASSERT_EQUALS(uassertStatusOK(cref), sd);

    StatusWith<StringData>& ncref = sw;
    ASSERT_EQUALS(uassertStatusOK(ncref), sd);

    StatusWith<StringData>&& rref = std::move(sw);
    ASSERT_EQUALS(uassertStatusOK(std::move(rref)), sd);
}

TEST(StatusWith, nonDefaultConstructible) {
    class NoDefault {
    public:
        NoDefault() = delete;
        NoDefault(int x) : x{x} {}
        int x;
    };

    auto swND = StatusWith(NoDefault(1));
    ASSERT_EQ(swND.getValue().x, 1);

    auto swNDerror = StatusWith<NoDefault>(ErrorCodes::BadValue, "foo");
    ASSERT_FALSE(swNDerror.isOK());
}

TEST(StatusWith, ignoreTest) {
    // A compile-only test
    [] { return StatusWith(false); }().getStatus().ignore();
}

TEST(StatusWith, MonadicTestLValue) {
    {
        auto from = StatusWith<int>{3};
        auto to = from.transform([](auto&& i) { return static_cast<double>(i); });
        static_assert(std::is_same_v<StatusWith<double>, decltype(to)>,
                      "StatusWith::transform returns incorrect type");
        ASSERT_EQ(3.0, to.getValue());
    }
    {
        auto from = StatusWith<int>{Status{ErrorCodes::IllegalOperation, "broke the law"}};
        auto to = from.transform([](auto&& i) { return static_cast<double>(i); });
        static_assert(std::is_same_v<StatusWith<double>, decltype(to)>,
                      "StatusWith::transform returns incorrect type");
        ASSERT_EQ(Status(ErrorCodes::IllegalOperation, "broke the law"), to.getStatus());
    }
    {
        auto from = StatusWith<int>{3};
        auto to = from.andThen([](auto&& i) { return StatusWith<double>{static_cast<double>(i)}; });
        static_assert(std::is_same_v<StatusWith<double>, decltype(to)>,
                      "StatusWith::andThen returns incorrect type");
        ASSERT_EQ(3.0, to.getValue());
    }
    {
        auto from = StatusWith<int>{Status{ErrorCodes::IllegalOperation, "broke the law"}};
        auto to = from.andThen([](auto&& i) { return StatusWith<double>{static_cast<double>(i)}; });
        static_assert(std::is_same_v<StatusWith<double>, decltype(to)>,
                      "StatusWith::andThen returns incorrect type");
        ASSERT_EQ(Status(ErrorCodes::IllegalOperation, "broke the law"), to.getStatus());
    }
    {
        auto from = StatusWith<int>{3};
        auto to = from.andThen([](auto&& i) {
            return StatusWith<double>{Status{ErrorCodes::BadValue, "lousy value"}};
        });
        static_assert(std::is_same_v<StatusWith<double>, decltype(to)>,
                      "StatusWith::andThen returns incorrect type");
        ASSERT_EQ(Status(ErrorCodes::BadValue, "lousy value"), to.getStatus());
    }
    {
        auto from = StatusWith<int>{Status{ErrorCodes::IllegalOperation, "broke the law"}};
        auto to = from.andThen([](auto&& i) {
            return StatusWith<double>{Status{ErrorCodes::BadValue, "lousy value"}};
        });
        static_assert(std::is_same_v<StatusWith<double>, decltype(to)>,
                      "StatusWith::andThen returns incorrect type");
        ASSERT_EQ(Status(ErrorCodes::IllegalOperation, "broke the law"), to.getStatus());
    }
}

TEST(StatusWith, MonadicTestConst) {
    {
        const auto from = StatusWith<int>{3};
        auto to = from.transform([](auto&& i) { return static_cast<double>(i); });
        static_assert(std::is_same_v<StatusWith<double>, decltype(to)>,
                      "StatusWith::transform returns incorrect type");
        ASSERT_EQ(3.0, to.getValue());
    }
    {
        const auto from = StatusWith<int>{Status{ErrorCodes::IllegalOperation, "broke the law"}};
        auto to = from.transform([](auto&& i) { return static_cast<double>(i); });
        static_assert(std::is_same_v<StatusWith<double>, decltype(to)>,
                      "StatusWith::transform returns incorrect type");
        ASSERT_EQ(Status(ErrorCodes::IllegalOperation, "broke the law"), to.getStatus());
    }
    {
        const auto from = StatusWith<int>{3};
        auto to = from.andThen([](auto&& i) { return StatusWith<double>{static_cast<double>(i)}; });
        static_assert(std::is_same_v<StatusWith<double>, decltype(to)>,
                      "StatusWith::andThen returns incorrect type");
        ASSERT_EQ(3.0, to.getValue());
    }
    {
        const auto from = StatusWith<int>{Status{ErrorCodes::IllegalOperation, "broke the law"}};
        auto to = from.andThen([](auto&& i) { return StatusWith<double>{static_cast<double>(i)}; });
        static_assert(std::is_same_v<StatusWith<double>, decltype(to)>,
                      "StatusWith::andThen returns incorrect type");
        ASSERT_EQ(Status(ErrorCodes::IllegalOperation, "broke the law"), to.getStatus());
    }
    {
        const auto from = StatusWith<int>{3};
        auto to = from.andThen([](auto&& i) {
            return StatusWith<double>{Status{ErrorCodes::BadValue, "lousy value"}};
        });
        static_assert(std::is_same_v<StatusWith<double>, decltype(to)>,
                      "StatusWith::andThen returns incorrect type");
        ASSERT_EQ(Status(ErrorCodes::BadValue, "lousy value"), to.getStatus());
    }
    {
        const auto from = StatusWith<int>{Status{ErrorCodes::IllegalOperation, "broke the law"}};
        auto to = from.andThen([](auto&& i) {
            return StatusWith<double>{Status{ErrorCodes::BadValue, "lousy value"}};
        });
        static_assert(std::is_same_v<StatusWith<double>, decltype(to)>,
                      "StatusWith::andThen returns incorrect type");
        ASSERT_EQ(Status(ErrorCodes::IllegalOperation, "broke the law"), to.getStatus());
    }
}

TEST(StatusWith, MonadicTestRValue) {
    {
        auto from = StatusWith<int>{3};
        auto to = std::move(from).transform([](auto&& i) { return static_cast<double>(i); });
        static_assert(std::is_same_v<StatusWith<double>, decltype(to)>,
                      "StatusWith::transform returns incorrect type");
        ASSERT_EQ(3.0, to.getValue());
    }
    {
        auto from = StatusWith<int>{Status{ErrorCodes::IllegalOperation, "broke the law"}};
        auto to = std::move(from).transform([](auto&& i) { return static_cast<double>(i); });
        static_assert(std::is_same_v<StatusWith<double>, decltype(to)>,
                      "StatusWith::transform returns incorrect type");
        ASSERT_EQ(Status(ErrorCodes::IllegalOperation, "broke the law"), to.getStatus());
    }
    {
        auto from = StatusWith<int>{3};
        auto to = std::move(from).andThen(
            [](auto&& i) { return StatusWith<double>{static_cast<double>(i)}; });
        static_assert(std::is_same_v<StatusWith<double>, decltype(to)>,
                      "StatusWith::andThen returns incorrect type");
        ASSERT_EQ(3.0, to.getValue());
    }
    {
        auto from = StatusWith<int>{Status{ErrorCodes::IllegalOperation, "broke the law"}};
        auto to = std::move(from).andThen(
            [](auto&& i) { return StatusWith<double>{static_cast<double>(i)}; });
        static_assert(std::is_same_v<StatusWith<double>, decltype(to)>,
                      "StatusWith::andThen returns incorrect type");
        ASSERT_EQ(Status(ErrorCodes::IllegalOperation, "broke the law"), to.getStatus());
    }
    {
        auto from = StatusWith<int>{3};
        auto to = std::move(from).andThen([](auto&& i) {
            return StatusWith<double>{Status{ErrorCodes::BadValue, "lousy value"}};
        });
        static_assert(std::is_same_v<StatusWith<double>, decltype(to)>,
                      "StatusWith::andThen returns incorrect type");
        ASSERT_EQ(Status(ErrorCodes::BadValue, "lousy value"), to.getStatus());
    }
    {
        auto from = StatusWith<int>{Status{ErrorCodes::IllegalOperation, "broke the law"}};
        auto to = std::move(from).andThen([](auto&& i) {
            return StatusWith<double>{Status{ErrorCodes::BadValue, "lousy value"}};
        });
        static_assert(std::is_same_v<StatusWith<double>, decltype(to)>,
                      "StatusWith::andThen returns incorrect type");
        ASSERT_EQ(Status(ErrorCodes::IllegalOperation, "broke the law"), to.getStatus());
    }
}

TEST(StatusWith, MonadicTestConstRValue) {
    {
        const auto from = StatusWith<int>{3};
        auto to = std::move(from).transform([](auto&& i) { return static_cast<double>(i); });
        static_assert(std::is_same_v<StatusWith<double>, decltype(to)>,
                      "StatusWith::transform returns incorrect type");
        ASSERT_EQ(3.0, to.getValue());
    }
    {
        const auto from = StatusWith<int>{Status{ErrorCodes::IllegalOperation, "broke the law"}};
        auto to = std::move(from).transform([](auto&& i) { return static_cast<double>(i); });
        static_assert(std::is_same_v<StatusWith<double>, decltype(to)>,
                      "StatusWith::transform returns incorrect type");
        ASSERT_EQ(Status(ErrorCodes::IllegalOperation, "broke the law"), to.getStatus());
    }
    {
        const auto from = StatusWith<int>{3};
        auto to = std::move(from).andThen(
            [](auto&& i) { return StatusWith<double>{static_cast<double>(i)}; });
        static_assert(std::is_same_v<StatusWith<double>, decltype(to)>,
                      "StatusWith::andThen returns incorrect type");
        ASSERT_EQ(3.0, to.getValue());
    }
    {
        const auto from = StatusWith<int>{Status{ErrorCodes::IllegalOperation, "broke the law"}};
        auto to = std::move(from).andThen(
            [](auto&& i) { return StatusWith<double>{static_cast<double>(i)}; });
        static_assert(std::is_same_v<StatusWith<double>, decltype(to)>,
                      "StatusWith::andThen returns incorrect type");
        ASSERT_EQ(Status(ErrorCodes::IllegalOperation, "broke the law"), to.getStatus());
    }
    {
        const auto from = StatusWith<int>{3};
        auto to = std::move(from).andThen([](auto&& i) {
            return StatusWith<double>{Status{ErrorCodes::BadValue, "lousy value"}};
        });
        static_assert(std::is_same_v<StatusWith<double>, decltype(to)>,
                      "StatusWith::andThen returns incorrect type");
        ASSERT_EQ(Status(ErrorCodes::BadValue, "lousy value"), to.getStatus());
    }
    {
        const auto from = StatusWith<int>{Status{ErrorCodes::IllegalOperation, "broke the law"}};
        auto to = std::move(from).andThen([](auto&& i) {
            return StatusWith<double>{Status{ErrorCodes::BadValue, "lousy value"}};
        });
        static_assert(std::is_same_v<StatusWith<double>, decltype(to)>,
                      "StatusWith::andThen returns incorrect type");
        ASSERT_EQ(Status(ErrorCodes::IllegalOperation, "broke the law"), to.getStatus());
    }
}

TEST(StatusWith, Overload) {
    struct LValue {};
    struct Const {};
    struct RValue {};
    struct ConstRValue {};

    struct {
        auto operator()(int&) & {
            return std::pair{LValue{}, LValue{}};
        }
        auto operator()(const int&) & {
            return std::pair{Const{}, LValue{}};
        }
        auto operator()(int&&) & {
            return std::pair{RValue{}, LValue{}};
        }
        auto operator()(const int&&) & {
            return std::pair{ConstRValue{}, LValue{}};
        }

        auto operator()(int&) const& {
            return std::pair{LValue{}, Const{}};
        }
        auto operator()(const int&) const& {
            return std::pair{Const{}, Const{}};
        }
        auto operator()(int&&) const& {
            return std::pair{RValue{}, Const{}};
        }
        auto operator()(const int&&) const& {
            return std::pair{ConstRValue{}, Const{}};
        }

        auto operator()(int&) && {
            return std::pair{LValue{}, RValue{}};
        }
        auto operator()(const int&) && {
            return std::pair{Const{}, RValue{}};
        }
        auto operator()(int&&) && {
            return std::pair{RValue{}, RValue{}};
        }
        auto operator()(const int&&) && {
            return std::pair{ConstRValue{}, RValue{}};
        }

        auto operator()(int&) const&& {
            return std::pair{LValue{}, ConstRValue{}};
        }
        auto operator()(const int&) const&& {
            return std::pair{Const{}, ConstRValue{}};
        }
        auto operator()(int&&) const&& {
            return std::pair{RValue{}, ConstRValue{}};
        }
        auto operator()(const int&&) const&& {
            return std::pair{ConstRValue{}, ConstRValue{}};
        }
    } transformFuncs;
    struct {
        auto operator()(int&) & {
            return StatusWith{std::pair{LValue{}, LValue{}}};
        }
        auto operator()(const int&) & {
            return StatusWith{std::pair{Const{}, LValue{}}};
        }
        auto operator()(int&&) & {
            return StatusWith{std::pair{RValue{}, LValue{}}};
        }
        auto operator()(const int&&) & {
            return StatusWith{std::pair{ConstRValue{}, LValue{}}};
        }

        auto operator()(int&) const& {
            return StatusWith{std::pair{LValue{}, Const{}}};
        }
        auto operator()(const int&) const& {
            return StatusWith{std::pair{Const{}, Const{}}};
        }
        auto operator()(int&&) const& {
            return StatusWith{std::pair{RValue{}, Const{}}};
        }
        auto operator()(const int&&) const& {
            return StatusWith{std::pair{ConstRValue{}, Const{}}};
        }

        auto operator()(int&) && {
            return StatusWith{std::pair{LValue{}, RValue{}}};
        }
        auto operator()(const int&) && {
            return StatusWith{std::pair{Const{}, RValue{}}};
        }
        auto operator()(int&&) && {
            return StatusWith{std::pair{RValue{}, RValue{}}};
        }
        auto operator()(const int&&) && {
            return StatusWith{std::pair{ConstRValue{}, RValue{}}};
        }

        auto operator()(int&) const&& {
            return StatusWith{std::pair{LValue{}, ConstRValue{}}};
        }
        auto operator()(const int&) const&& {
            return StatusWith{std::pair{Const{}, ConstRValue{}}};
        }
        auto operator()(int&&) const&& {
            return StatusWith{std::pair{RValue{}, ConstRValue{}}};
        }
        auto operator()(const int&&) const&& {
            return StatusWith{std::pair{ConstRValue{}, ConstRValue{}}};
        }
    } andThenFuncs;
    {
        auto in = StatusWith<int>{3};
        static_assert(std::is_same_v<StatusWith<std::pair<LValue, LValue>>,
                                     decltype(in.transform(transformFuncs))>,
                      "StatusWith::transform returns incorrect type");
        static_assert(std::is_same_v<StatusWith<std::pair<LValue, LValue>>,
                                     decltype(in.andThen(andThenFuncs))>,
                      "StatusWith::andThen returns incorrect type");
        static_assert(std::is_same_v<StatusWith<std::pair<LValue, Const>>,
                                     decltype(in.transform(std::as_const(transformFuncs)))>,
                      "StatusWith::transform returns incorrect type");
        static_assert(std::is_same_v<StatusWith<std::pair<LValue, Const>>,
                                     decltype(in.andThen(std::as_const(andThenFuncs)))>,
                      "StatusWith::andThen returns incorrect type");
        static_assert(std::is_same_v<StatusWith<std::pair<LValue, RValue>>,
                                     decltype(in.transform(std::move(transformFuncs)))>,
                      "StatusWith::transform returns incorrect type");
        static_assert(std::is_same_v<StatusWith<std::pair<LValue, RValue>>,
                                     decltype(in.andThen(std::move(andThenFuncs)))>,
                      "StatusWith::andThen returns incorrect type");
        static_assert(
            std::is_same_v<StatusWith<std::pair<LValue, ConstRValue>>,
                           decltype(in.transform(std::move(std::as_const(transformFuncs))))>,
            "StatusWith::transform returns incorrect type");
        static_assert(std::is_same_v<StatusWith<std::pair<LValue, ConstRValue>>,
                                     decltype(in.andThen(std::move(std::as_const(andThenFuncs))))>,
                      "StatusWith::andThen returns incorrect type");
    }
    {
        const auto in = StatusWith<int>{3};
        static_assert(std::is_same_v<StatusWith<std::pair<Const, LValue>>,
                                     decltype(in.transform(transformFuncs))>,
                      "StatusWith::transform returns incorrect type");
        static_assert(std::is_same_v<StatusWith<std::pair<Const, LValue>>,
                                     decltype(in.andThen(andThenFuncs))>,
                      "StatusWith::andThen returns incorrect type");
        static_assert(std::is_same_v<StatusWith<std::pair<Const, Const>>,
                                     decltype(in.transform(std::as_const(transformFuncs)))>,
                      "StatusWith::transform returns incorrect type");
        static_assert(std::is_same_v<StatusWith<std::pair<Const, Const>>,
                                     decltype(in.andThen(std::as_const(andThenFuncs)))>,
                      "StatusWith::andThen returns incorrect type");
        static_assert(std::is_same_v<StatusWith<std::pair<Const, RValue>>,
                                     decltype(in.transform(std::move(transformFuncs)))>,
                      "StatusWith::transform returns incorrect type");
        static_assert(std::is_same_v<StatusWith<std::pair<Const, RValue>>,
                                     decltype(in.andThen(std::move(andThenFuncs)))>,
                      "StatusWith::andThen returns incorrect type");
        static_assert(
            std::is_same_v<StatusWith<std::pair<Const, ConstRValue>>,
                           decltype(in.transform(std::move(std::as_const(transformFuncs))))>,
            "StatusWith::transform returns incorrect type");
        static_assert(std::is_same_v<StatusWith<std::pair<Const, ConstRValue>>,
                                     decltype(in.andThen(std::move(std::as_const(andThenFuncs))))>,
                      "StatusWith::andThen returns incorrect type");
    }
    {
        auto in = StatusWith<int>{3};
        static_assert(std::is_same_v<StatusWith<std::pair<RValue, LValue>>,
                                     decltype(std::move(in).transform(transformFuncs))>,
                      "StatusWith::transform returns incorrect type");
        static_assert(std::is_same_v<StatusWith<std::pair<RValue, LValue>>,
                                     decltype(std::move(in).andThen(andThenFuncs))>,
                      "StatusWith::andThen returns incorrect type");
        static_assert(
            std::is_same_v<StatusWith<std::pair<RValue, Const>>,
                           decltype(std::move(in).transform(std::as_const(transformFuncs)))>,
            "StatusWith::transform returns incorrect type");
        static_assert(std::is_same_v<StatusWith<std::pair<RValue, Const>>,
                                     decltype(std::move(in).andThen(std::as_const(andThenFuncs)))>,
                      "StatusWith::andThen returns incorrect type");
        static_assert(std::is_same_v<StatusWith<std::pair<RValue, RValue>>,
                                     decltype(std::move(in).transform(std::move(transformFuncs)))>,
                      "StatusWith::transform returns incorrect type");
        static_assert(std::is_same_v<StatusWith<std::pair<RValue, RValue>>,
                                     decltype(std::move(in).andThen(std::move(andThenFuncs)))>,
                      "StatusWith::andThen returns incorrect type");
        static_assert(std::is_same_v<StatusWith<std::pair<RValue, ConstRValue>>,
                                     decltype(std::move(in).transform(
                                         std::move(std::as_const(transformFuncs))))>,
                      "StatusWith::transform returns incorrect type");
        static_assert(
            std::is_same_v<StatusWith<std::pair<RValue, ConstRValue>>,
                           decltype(std::move(in).andThen(std::move(std::as_const(andThenFuncs))))>,
            "StatusWith::andThen returns incorrect type");
    }
    {
        const auto in = StatusWith<int>{3};
        static_assert(std::is_same_v<StatusWith<std::pair<ConstRValue, LValue>>,
                                     decltype(std::move(in).transform(transformFuncs))>,
                      "StatusWith::transform returns incorrect type");
        static_assert(std::is_same_v<StatusWith<std::pair<ConstRValue, LValue>>,
                                     decltype(std::move(in).andThen(andThenFuncs))>,
                      "StatusWith::andThen returns incorrect type");
        static_assert(
            std::is_same_v<StatusWith<std::pair<ConstRValue, Const>>,
                           decltype(std::move(in).transform(std::as_const(transformFuncs)))>,
            "StatusWith::transform returns incorrect type");
        static_assert(std::is_same_v<StatusWith<std::pair<ConstRValue, Const>>,
                                     decltype(std::move(in).andThen(std::as_const(andThenFuncs)))>,
                      "StatusWith::andThen returns incorrect type");
        static_assert(std::is_same_v<StatusWith<std::pair<ConstRValue, RValue>>,
                                     decltype(std::move(in).transform(std::move(transformFuncs)))>,
                      "StatusWith::transform returns incorrect type");
        static_assert(std::is_same_v<StatusWith<std::pair<ConstRValue, RValue>>,
                                     decltype(std::move(in).andThen(std::move(andThenFuncs)))>,
                      "StatusWith::andThen returns incorrect type");
        static_assert(std::is_same_v<StatusWith<std::pair<ConstRValue, ConstRValue>>,
                                     decltype(std::move(in).transform(
                                         std::move(std::as_const(transformFuncs))))>,
                      "StatusWith::transform returns incorrect type");
        static_assert(
            std::is_same_v<StatusWith<std::pair<ConstRValue, ConstRValue>>,
                           decltype(std::move(in).andThen(std::move(std::as_const(andThenFuncs))))>,
            "StatusWith::andThen returns incorrect type");
    }
}

}  // namespace
}  // namespace mongo
