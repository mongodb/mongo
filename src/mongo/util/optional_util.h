// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/stdx/type_traits.h"
#include "mongo/util/modules.h"

#include <optional>
#include <ostream>
#include <string>
#include <type_traits>

#include <boost/none_t.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

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
using CanStreamOp [[MONGO_MOD_FILE_PRIVATE]] =
    decltype(std::declval<std::ostream&>() << std::declval<const T&>());
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
Extension(const T& t) -> Extension<T>;

}  // namespace optional_io
}  // namespace mongo
