// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/util/modules.h"

#include <deque>
#include <tuple>
#include <type_traits>

#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {

template <typename... Args>
std::string tupleToString(const std::tuple<Args...>& tuple) {
    std::ostringstream os;
    std::apply(
        [&](const auto& first, const auto&... rest) {
            os << first;
            ((os << ", " << rest), ...);
        },
        tuple);
    return os.str();
}

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
                    fmt::format("Expected call doesn't match for mock function \"{}\". Actual: {}, "
                                "Expected: {}",
                                name,
                                tupleToString(std::tie(args...)),
                                tupleToString(expected.front())));
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
