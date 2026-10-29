// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/util/modules.h"
#include "mongo/util/optional_util.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>

#include <boost/optional.hpp>
#include <fmt/format.h>

/**
 * Mechanisms and extensibility hooks used by this library to format arbitrary
 * user-provided objects.
 */
namespace mongo::unittest::stringify {

template <typename T>
std::string invoke(const T& x);

std::string formatTypedObj(const std::type_info& ti, std::string_view obj);

std::string lastResortFormat(const std::type_info& ti, const void* p, size_t sz);

template <typename T>
std::string doFormat(const T& x) {
    return fmt::format("{}", x);
}

template <typename T>
std::string doOstream(const T& x) {
    std::ostringstream os;
    os << x;
    return os.str();
}

using std::begin;
using std::end;

template <typename T>
using HasToStringOp = decltype(std::declval<T>().toString());
template <typename T>
constexpr bool HasToString = stdx::is_detected_v<HasToStringOp, T>;

template <typename T>
using HasBeginEndOp =
    std::tuple<decltype(begin(std::declval<T>())), decltype(end(std::declval<T>()))>;
template <typename T>
constexpr bool IsSequence = stdx::is_detected_v<HasBeginEndOp, T>;

template <typename T>
using IsTupleOp = decltype(std::tuple_size<T>::value);
template <typename T>
constexpr bool IsTuple = stdx::is_detected_v<IsTupleOp, T>;

class Joiner {
public:
    template <typename T>
    Joiner& operator()(const T& v) {
        // `stringify::` qualification necessary to disable ADL on `v`.
        _out += fmt::format("{}{}", _sep, stringify::invoke(v));
        _sep = ", ";
        return *this;
    }
    explicit operator const std::string&() const {
        return _out;
    }

private:
    std::string _out;
    const char* _sep = "";
};

template <typename T>
std::string doSequence(const T& seq) {
    Joiner joiner;
    for (const auto& e : seq)
        joiner(e);
    return fmt::format("[{}]", std::string{joiner});
}

template <typename T, size_t... Is>
std::string doTuple(const T& tup, std::index_sequence<Is...>) {
    Joiner joiner;
    (joiner(std::get<Is>(tup)), ...);
    return fmt::format("({})", std::string{joiner});
}

template <typename T>
std::string doTuple(const T& tup) {
    return doTuple(tup, std::make_index_sequence<std::tuple_size_v<T>>{});
}

/**
 * The only definitions in this namespace are some "built-in" overloads of
 * `stringify_forTest`. It defines no types, so ADL will not find it. A
 * `stringify::invoke` call will consider these in the overload set along with
 * any overloads found by ADL on the argument.
 */
namespace adl_barrier {
/**
 * The default `stringify_forTest` implementation.
 * Encodes the steps by which we determine how to print an object.
 * There's a wildcard branch so everything is printable in some way.
 */
template <typename T>
std::string stringify_forTest(const T& x) {
    if constexpr (optional_io::canStreamWithExtension<T>) {
        return doOstream(optional_io::Extension{x});
    } else if constexpr (HasToString<T>) {
        return x.toString();
    } else if constexpr (std::is_convertible_v<T, std::string_view>) {
        return doFormat(std::string_view(x));
    } else if constexpr (std::is_pointer_v<T>) {
        return doFormat(static_cast<const void*>(x));
    } else if constexpr (IsSequence<T>) {
        return doSequence(x);
    } else if constexpr (IsTuple<T>) {
        return doTuple(x);
    } else if constexpr (std::is_enum_v<T>) {
        return formatTypedObj(typeid(T), doFormat(static_cast<std::underlying_type_t<T>>(x)));
    } else {
        return lastResortFormat(typeid(x), &x, sizeof(x));
    }
}

/** Portably support stringifying `nullptr`. */
inline std::string stringify_forTest(std::nullptr_t) {
    return "nullptr";
}

/** Built-in support to stringify `ErrorCode::Error`. */
inline std::string stringify_forTest(ErrorCodes::Error ec) {
    return ErrorCodes::errorString(ec);
}
}  // namespace adl_barrier

/**
 * The entry point for the `unittest::stringify` system, this is
 * called to produce a string representation of an arbitrary value
 * `x` through the `stringify_forTest` extension hook.
 *
 * An overload for `stringify_forTest` is selected from a few
 * "built-in" overloads, and then from any that are found in
 * namespaces associated with `x` via argument-dependent lookup.
 *
 * The `stringify_forTest` name is an ADL extension point for
 * user-defined types, and should not be invoked directly.  Call
 * `stringify::invoke` instead.
 */
template <typename T>
std::string invoke(const T& x) {
    using adl_barrier::stringify_forTest;
    return stringify_forTest(x);
}

}  // namespace mongo::unittest::stringify
