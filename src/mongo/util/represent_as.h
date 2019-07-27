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

#pragma once

#include <cmath>
#include <limits>
#include <type_traits>

#include <boost/numeric/conversion/cast.hpp>
#include <boost/optional.hpp>

#include "mongo/base/static_assert.h"
#include "mongo/stdx/type_traits.h"

namespace mongo {

namespace detail {

/**
 * The following three methods are conversion helpers that allow us to promote
 * all numerical input to three top-level types: int64_t, uint64_t, and double.
 */

// Floating point numbers -> double
template <typename T>
typename stdx::enable_if_t<std::is_floating_point<T>::value, double> upconvert(T t) {
    MONGO_STATIC_ASSERT(sizeof(double) >= sizeof(T));
    return static_cast<double>(t);
}

// Signed integral types -> int64_t
template <typename T>
typename stdx::enable_if_t<std::is_integral<T>::value && std::is_signed<T>::value, int64_t>
upconvert(T t) {
    MONGO_STATIC_ASSERT(sizeof(int64_t) >= sizeof(T));
    return static_cast<int64_t>(t);
}

// Unsigned integral types -> uint64_t
template <typename T>
typename stdx::enable_if_t<std::is_integral<T>::value && !std::is_signed<T>::value, uint64_t>
upconvert(T t) {
    MONGO_STATIC_ASSERT(sizeof(uint64_t) >= sizeof(T));
    return static_cast<uint64_t>(t);
}

/**
 * Compare two values of the same type. Return -1 if a < b, 0 if they are equal, and
 * 1 if a > b.
 */
template <typename T>
int identityCompare(T a, T b) {
    if (a == b) {
        return 0;
    }
    return (a < b) ? -1 : 1;
}

inline int signedCompare(int64_t a, int64_t b) {
    return identityCompare(a, b);
}

inline int signedCompare(double a, double b) {
    return identityCompare(a, b);
}

inline int signedCompare(uint64_t a, uint64_t b) {
    return identityCompare(a, b);
}

/**
 * Compare unsigned and signed integers.
 */
inline int signedCompare(int64_t a, uint64_t b) {
    if (a < 0) {
        return -1;
    }

    auto aUnsigned = static_cast<uint64_t>(a);
    return signedCompare(aUnsigned, b);
}

inline int signedCompare(uint64_t a, int64_t b) {
    return -signedCompare(b, a);
}

/**
 * Compare doubles and signed integers.
 */
inline int signedCompare(double a, int64_t b) {
    // Casting int64_ts to doubles will round them
    // and give the wrong result, so convert doubles to
    // int64_ts if we can, then do the comparison.
    if (a < -std::ldexp(1, 63)) {
        return -1;
    } else if (a >= std::ldexp(1, 63)) {
        return 1;
    }

    auto aAsInt64 = static_cast<int64_t>(a);
    return signedCompare(aAsInt64, b);
}

inline int signedCompare(int64_t a, double b) {
    return -signedCompare(b, a);
}

/**
 * Compare doubles and unsigned integers.
 */
inline int signedCompare(double a, uint64_t b) {
    if (a < 0) {
        return -1;
    }

    // Casting uint64_ts to doubles will round them
    // and give the wrong result, so convert doubles to
    // uint64_ts if we can, then do the comparison.
    if (a >= std::ldexp(1, 64)) {
        return 1;
    }

    auto aAsUInt64 = static_cast<uint64_t>(a);
    return signedCompare(aAsUInt64, b);
}

inline int signedCompare(uint64_t a, double b) {
    return -signedCompare(b, a);
}

/**
 * For any t and u of types T and U, promote t and u to one of the
 * top-level numerical types (int64_t, uint64_t, and double) and
 * compare them.
 *
 * Return -1 if t < u, 0 if they are equal, 1 if t > u.
 */
template <typename T, typename U>
int compare(T t, U u) {
    return signedCompare(upconvert(t), upconvert(u));
}

/**
 * Return true if number can be converted to Output type without underflow or overflow.
 */
template <typename Output, typename Input>
bool inRange(Input i) {
    const auto floor = std::numeric_limits<Output>::lowest();
    const auto ceiling = std::numeric_limits<Output>::max();

    return detail::compare(i, floor) >= 0 && detail::compare(i, ceiling) <= 0;
}

}  // namespace detail

/**
 * Given a number of some type Input and a desired numerical type Output,
 * this method represents the input number in the output type if possible.
 * If the given number cannot be exactly represented in the output type,
 * this method returns a disengaged optional.
 *
 * ex:
 *     auto v1 = representAs<int>(2147483647); // v1 holds 2147483647
 *     auto v2 = representAs<int>(2147483648); // v2 is disengaged
 *     auto v3 = representAs<int>(10.3);       // v3 is disengaged
 */
template <typename Output, typename Input>
boost::optional<Output> representAs(Input number) try {
    if constexpr (std::is_same_v<Input, Output>) {
        return number;
    } else if constexpr (std::is_same_v<Decimal128, Output>) {
        // Use Decimal128's ctor taking (u)int64_t or double, if it's safe to cast to one of those.
        if constexpr (std::is_integral_v<Input>) {
            if constexpr (std::is_signed_v<Input>) {
                return Decimal128{boost::numeric_cast<int64_t>(number)};
            } else {
                return Decimal128{boost::numeric_cast<uint64_t>(number)};
            }
        } else if constexpr (std::is_floating_point_v<Input>) {
            return Decimal128{boost::numeric_cast<double>(number)};
        } else {
            return {};
        }
    } else {
        // If number is NaN and Output can also represent NaN, return NaN
        // Note: We need to specifically handle NaN here because of the way
        // detail::compare is implemented.
        if (std::is_floating_point_v<Input> && std::isnan(number)) {
            if (std::is_floating_point_v<Output>) {
                return {static_cast<Output>(number)};
            }
        }

        // If Output is integral and number is a non-integral floating point value,
        // return a disengaged optional.
        if constexpr (std::is_floating_point_v<Input> && std::is_integral_v<Output>) {
            if (!(std::trunc(number) == number)) {
                return {};
            }
        }

        if (!detail::inRange<Output>(number)) {
            return {};
        }

        Output numberOut(number);

        // Some integers cannot be exactly represented as floating point numbers.
        // To check, we cast back to the input type if we can, and compare.
        if constexpr (std::is_integral_v<Input> && std::is_floating_point_v<Output>) {
            if (!detail::inRange<Input>(numberOut) || static_cast<Input>(numberOut) != number) {
                return {};
            }
        }

        return numberOut;
    }
} catch (const boost::bad_numeric_cast&) {
    return {};
}

// Overload for converting from Decimal128.
template <typename Output>
boost::optional<Output> representAs(const Decimal128& number) try {
    std::uint32_t flags = 0;
    Output numberOut;

    if constexpr (std::is_same_v<Output, Decimal128>) {
        return number;
    } else if constexpr (std::is_floating_point_v<Output>) {
        numberOut = boost::numeric_cast<Output>(number.toDouble(&flags));
    } else if constexpr (std::is_integral_v<Output>) {
        if constexpr (std::is_signed_v<Output>) {
            numberOut = boost::numeric_cast<Output>(number.toLongExact(&flags));
        } else {
            numberOut = boost::numeric_cast<Output>(number.toULongExact(&flags));
        }
    } else {
        // Unsupported type.
        return {};
    }

    // Decimal128::toDouble/toLongExact failed.
    if (flags & (Decimal128::kUnderflow | Decimal128::kOverflow | Decimal128::kInvalid)) {
        return {};
    }

    if (std::is_integral<Output>() && flags & Decimal128::kInexact) {
        return {};
    }

    return numberOut;
} catch (const boost::bad_numeric_cast&) {
    return {};
}

}  // namespace mongo
