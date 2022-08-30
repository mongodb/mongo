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

#include <boost/optional.hpp>
#include <optional>
#include <ostream>
#include <string>
#include <type_traits>

#include "mongo/stdx/type_traits.h"

namespace mongo {

template <typename T>
inline constexpr bool isBoostOptional = false;
template <typename... Ts>
inline constexpr bool isBoostOptional<boost::optional<Ts...>> = true;

template <typename T>
inline constexpr bool isStdOptional = false;
template <typename... Ts>
inline constexpr bool isStdOptional<std::optional<Ts...>> = true;  // NOLINT

/**
 * Some utilities for formatting optional<T> values, (either std or boost).
 * Meant as a substitute for boost/optional_io.hpp.
 */
namespace optional_io {

template <typename T>
using CanStreamOp = decltype(std::declval<std::ostream&>() << std::declval<const T&>());
template <typename T>
inline constexpr bool canStream = stdx::is_detected_v<CanStreamOp, T>;

/**
 * True if `Extension<T>` is supported. Cumbersome, but this set of
 * specializations is necessary because `boost::optional<T>` defines a
 * SFINAE-hostile `operator<<`.
 */
template <typename T>
inline constexpr bool canStreamWithExtension = canStream<T>;
template <>
inline constexpr bool canStreamWithExtension<std::nullopt_t> = true;  // NOLINT
template <typename T>
inline constexpr bool canStreamWithExtension<std::optional<T>> =  // NOLINT
    canStreamWithExtension<T>;
template <>
inline constexpr bool canStreamWithExtension<boost::none_t> = true;
template <typename T>
inline constexpr bool canStreamWithExtension<boost::optional<T>> = canStreamWithExtension<T>;

namespace detail {
/**
 * The `stream` overloads provide the same formatting as boost/optional_io.hpp,
 * but support both the `boost` and `std` optional types.
 */
template <typename T, std::enable_if_t<canStream<T>, int> = 0>
std::ostream& stream(std::ostream& os, const T& v) {
    return os << v;
}

inline std::ostream& stream(std::ostream& os, std::nullopt_t) {  // NOLINT
    return os << "--";
}

template <typename T, std::enable_if_t<canStreamWithExtension<T>, int> = 0>
std::ostream& stream(std::ostream& os, const std::optional<T>& v) {  // NOLINT
    if (!v)
        return stream(os, std::nullopt);
    return stream(os << " ", *v);
}

inline std::ostream& stream(std::ostream& os, boost::none_t) {
    return os << "--";
}

template <typename T, std::enable_if_t<canStreamWithExtension<T>, int> = 0>
std::ostream& stream(std::ostream& os, const boost::optional<T>& v) {
    if (!v)
        return stream(os, std::nullopt);
    return stream(os << " ", *v);
}
}  // namespace detail

/**
 * Streamable wrapper for an object reference.
 * This object formats just like the object it refers to.
 *
 * But if the object happens to be an optional, then the wrapper will be
 * streamble in the `boost/optional_io.hpp` formatting style. That is, non-empty
 * optionals are formatted with a space followed by the value's representation.
 * Empty optionals are "--".
 *
 * Example:
 *     std::cout << Extension{someOptional};
 *     std::cout << Extension{123};
 */
template <typename T, std::enable_if_t<canStreamWithExtension<T>, int> = 0>
class Extension {
public:
    explicit Extension(const T& v) : v{v} {}

private:
    friend std::ostream& operator<<(std::ostream& os, const Extension& o) {
        return detail::stream(os, o.v);
    }

    const T& v;
};

template <typename T, std::enable_if_t<canStreamWithExtension<T>, int> = 0>
Extension(const T& t)->Extension<T>;

}  // namespace optional_io

namespace optional_util {
template <typename T, typename U>
void setOrAdd(boost::optional<T>& counter, U value) {
    if (!counter) {
        counter = value;
        return;
    }
    counter = *counter + value;
}
}  // namespace optional_util
}  // namespace mongo
