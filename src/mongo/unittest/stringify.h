/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <algorithm>
#include <array>
#include <boost/optional.hpp>
#include <cstddef>
#include <fmt/format.h>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/util/optional_util.h"

/**
 * Mechanisms and extensibility hooks used by this library to format arbitrary
 * user-provided objects.
 */
namespace mongo::unittest::stringify {

template <typename T>
std::string invoke(const T& x);

std::string formatTypedObj(const std::type_info& ti, StringData obj);

std::string lastResortFormat(const std::type_info& ti, const void* p, size_t sz);

template <typename T>
std::string doFormat(const T& x) {
    return format(FMT_STRING("{}"), x);
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
        _out += format(FMT_STRING("{}{}"), _sep, stringify::invoke(v));
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
    std::string r;
    Joiner joiner;
    for (const auto& e : seq)
        joiner(e);
    return format(FMT_STRING("[{}]"), std::string{joiner});
}

template <typename T, size_t... Is>
std::string doTuple(const T& tup, std::index_sequence<Is...>) {
    Joiner joiner;
    (joiner(std::get<Is>(tup)), ...);
    return format(FMT_STRING("({})"), std::string{joiner});
}

template <typename T>
std::string doTuple(const T& tup) {
    return doTuple(tup, std::make_index_sequence<std::tuple_size_v<T>>{});
}

/**
 * The only definitions in this namespace are some "built-in" overloads of
 * `stringifyForAssert`. It defines no types, so ADL will not find it. A
 * `stringify::invoke` call will consider these in the overload set along with
 * any overloads found by ADL on the argument.
 */
namespace adl_barrier {
/**
 * The default `stringifyForAssert` implementation.
 * Encodes the steps by which we determine how to print an object.
 * There's a wildcard branch so everything is printable in some way.
 */
template <typename T>
std::string stringifyForAssert(const T& x) {
    if constexpr (optional_io::canStreamWithExtension<T>) {
        return doOstream(optional_io::Extension{x});
    } else if constexpr (HasToString<T>) {
        return x.toString();
    } else if constexpr (std::is_convertible_v<T, StringData>) {
        return doFormat(StringData(x));
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
inline std::string stringifyForAssert(std::nullptr_t) {
    return "nullptr";
}

/** Built-in support to stringify `ErrorCode::Error`. */
inline std::string stringifyForAssert(ErrorCodes::Error ec) {
    return ErrorCodes::errorString(ec);
}
}  // namespace adl_barrier

/**
 * The entry point for the `unittest::stringify` system, this is
 * called to produce a string representation of an arbitrary value
 * `x` through the `stringifyForAssert` extension hook.
 *
 * An overload for `stringifyForAssert` is selected from a few
 * "built-in" overloads, and then from any that are found in
 * namespaces associated with `x` via argument-dependent lookup.
 *
 * The `stringifyForAssert` name is an ADL extension point for
 * user-defined types, and should not be invoked directly.  Call
 * `stringify::invoke` instead.
 */
template <typename T>
std::string invoke(const T& x) {
    using adl_barrier::stringifyForAssert;
    return stringifyForAssert(x);
}

}  // namespace mongo::unittest::stringify
