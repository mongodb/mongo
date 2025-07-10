/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#pragma once

#include <deque>
#include <tuple>
#include <type_traits>

#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {

template <class T, class ExactType>
concept non_void = !std::same_as<T, void> && std::same_as<T, ExactType>;

template <class Ret, class... Args>
class MiniMockFunction {
public:
    MiniMockFunction(std::string name = "") : name(name) {}
    Ret operator()(Args... args) {
        if constexpr (sizeof...(Args) > 0) {
            if (expected.empty()) {
                throw std::logic_error(
                    fmt::format("Unexpected call to mock function \"{}\"", name));
            }
            if (std::tie(args...) != expected.front()) {
                throw std::logic_error(
                    fmt::format("Expected call doesn't match for mock function \"{}\"", name));
            }
            expected.pop_front();
        }
        if constexpr (!std::is_same_v<void, Ret>) {
            if (!toReturn.empty()) {
                auto&& value = toReturn.front();
                toReturn.pop_front();
                return value;
            }
            if (defaultReturnValue.has_value()) {
                return *defaultReturnValue;
            }
            throw std::logic_error(
                fmt::format("Call without a return value set for mock function \"{}\"", name));
        }
    }

    MiniMockFunction& expect(Args... args)
    requires std::is_same_v<void, Ret>
    {
        expected.emplace_back(args...);
        return *this;
    }

    MiniMockFunction& expect(std::tuple<Args...> args, non_void<Ret> auto ret) {
        expected.emplace_back(args);
        toReturn.emplace_back(ret);
        return *this;
    }

    MiniMockFunction& ret(non_void<Ret> auto ret)
    requires(sizeof...(Args) == 0)
    {
        toReturn.emplace_back(ret);
        return *this;
    }

    MiniMockFunction& defaultRet(non_void<Ret> auto ret) {
        defaultReturnValue = ret;
        return *this;
    }

    std::deque<std::tuple<Args...>> expected;
    using RetValueType = std::conditional_t<std::is_same_v<void, Ret>, void*, Ret>;
    std::deque<RetValueType> toReturn;
    boost::optional<RetValueType> defaultReturnValue;
    std::string name;
};
}  // namespace mongo
