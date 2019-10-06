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

#include <cstdint>

#ifdef _MSC_VER
#include <SafeInt.hpp>
#endif

#include "mongo/stdx/type_traits.h"
#include "mongo/util/assert_util.h"

namespace mongo::overflow {

/**
 * Synopsis:
 *
 *   bool mul(A a, A b, T* r);
 *   bool add(A a, A b, T* r);
 *   bool sub(A a, A b, T* r);
 *
 * The domain type `A` evaluates to `T`, which is deduced from the `r` parameter.
 * That is, the input parameters are coerced into the type accepted by the output parameter.
 * All functions return true if operation would overflow, otherwise they store result in `*r`.
 */

// MSVC : The SafeInt functions return false on overflow.
// GCC, Clang: The __builtin_*_overflow functions return true on overflow.

template <typename T>
constexpr bool mul(stdx::type_identity_t<T> a, stdx::type_identity_t<T> b, T* r) {
#ifdef _MSC_VER
    return !SafeMultiply(a, b, *r);
#else
    return __builtin_mul_overflow(a, b, r);
#endif
}

template <typename T>
constexpr bool add(stdx::type_identity_t<T> a, stdx::type_identity_t<T> b, T* r) {
#ifdef _MSC_VER
    return !SafeAdd(a, b, *r);
#else
    return __builtin_add_overflow(a, b, r);
#endif
}

template <typename T>
constexpr bool sub(stdx::type_identity_t<T> a, stdx::type_identity_t<T> b, T* r) {
#ifdef _MSC_VER
    return !SafeSubtract(a, b, *r);
#else
    return __builtin_sub_overflow(a, b, r);
#endif
}

/**
 * Safe mod function which throws if the divisor is 0 and avoids potential overflow in cases where
 * the divisor is -1. If the absolute value of the divisor is 1, mod will always return 0. We fast-
 * path this to avoid the scenario where the dividend is the smallest negative long or int value and
 * the divisor is -1. Naively performing this % may result in an overflow when the -2^N value is
 * divided and becomes 2^N. See SERVER-43699.
 */
template <typename T>
constexpr T safeMod(T dividend, T divisor) {
    uassert(51259, "can't mod by zero", divisor != 0);
    return (divisor == 1 || divisor == -1 ? 0 : dividend % divisor);
}

}  // namespace mongo::overflow
