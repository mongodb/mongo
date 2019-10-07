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

#include "mongo/util/assert_util.h"

namespace mongo {

/**
 * Returns true if multiplying lhs by rhs would overflow. Otherwise, multiplies 64-bit signed
 * or unsigned integers lhs by rhs and stores the result in *product.
 */
constexpr bool mongoSignedMultiplyOverflow64(int64_t lhs, int64_t rhs, int64_t* product);
constexpr bool mongoUnsignedMultiplyOverflow64(uint64_t lhs, uint64_t rhs, uint64_t* product);

/**
 * Returns true if adding lhs and rhs would overflow. Otherwise, adds 64-bit signed or unsigned
 * integers lhs and rhs and stores the result in *sum.
 */
constexpr bool mongoSignedAddOverflow64(int64_t lhs, int64_t rhs, int64_t* sum);
constexpr bool mongoUnsignedAddOverflow64(uint64_t lhs, uint64_t rhs, uint64_t* sum);

/**
 * Returns true if subtracting rhs from lhs would overflow. Otherwise, subtracts 64-bit signed or
 * unsigned integers rhs from lhs and stores the result in *difference.
 */
constexpr bool mongoSignedSubtractOverflow64(int64_t lhs, int64_t rhs, int64_t* difference);
constexpr bool mongoUnsignedSubtractOverflow64(uint64_t lhs, uint64_t rhs, uint64_t* difference);


#ifdef _MSC_VER

// The SafeInt functions return true on success, false on overflow.

constexpr bool mongoSignedMultiplyOverflow64(int64_t lhs, int64_t rhs, int64_t* product) {
    return !SafeMultiply(lhs, rhs, *product);
}

constexpr bool mongoUnsignedMultiplyOverflow64(uint64_t lhs, uint64_t rhs, uint64_t* product) {
    return !SafeMultiply(lhs, rhs, *product);
}

constexpr bool mongoSignedAddOverflow64(int64_t lhs, int64_t rhs, int64_t* sum) {
    return !SafeAdd(lhs, rhs, *sum);
}

constexpr bool mongoUnsignedAddOverflow64(uint64_t lhs, uint64_t rhs, uint64_t* sum) {
    return !SafeAdd(lhs, rhs, *sum);
}

constexpr bool mongoSignedSubtractOverflow64(int64_t lhs, int64_t rhs, int64_t* difference) {
    return !SafeSubtract(lhs, rhs, *difference);
}

constexpr bool mongoUnsignedSubtractOverflow64(uint64_t lhs, uint64_t rhs, uint64_t* difference) {
    return !SafeSubtract(lhs, rhs, *difference);
}

#else

// On GCC and CLANG we can use __builtin functions to perform these calculations. These return true
// on overflow and false on success.

constexpr bool mongoSignedMultiplyOverflow64(long lhs, long rhs, long* product) {
    return __builtin_mul_overflow(lhs, rhs, product);
}

constexpr bool mongoSignedMultiplyOverflow64(long long lhs, long long rhs, long long* product) {
    return __builtin_mul_overflow(lhs, rhs, product);
}

constexpr bool mongoUnsignedMultiplyOverflow64(unsigned long lhs,
                                               unsigned long rhs,
                                               unsigned long* product) {
    return __builtin_mul_overflow(lhs, rhs, product);
}

constexpr bool mongoUnsignedMultiplyOverflow64(unsigned long long lhs,
                                               unsigned long long rhs,
                                               unsigned long long* product) {
    return __builtin_mul_overflow(lhs, rhs, product);
}

constexpr bool mongoSignedAddOverflow64(long lhs, long rhs, long* sum) {
    return __builtin_add_overflow(lhs, rhs, sum);
}

constexpr bool mongoSignedAddOverflow64(long long lhs, long long rhs, long long* sum) {
    return __builtin_add_overflow(lhs, rhs, sum);
}

constexpr bool mongoUnsignedAddOverflow64(unsigned long lhs,
                                          unsigned long rhs,
                                          unsigned long* sum) {
    return __builtin_add_overflow(lhs, rhs, sum);
}

constexpr bool mongoUnsignedAddOverflow64(unsigned long long lhs,
                                          unsigned long long rhs,
                                          unsigned long long* sum) {
    return __builtin_add_overflow(lhs, rhs, sum);
}

constexpr bool mongoSignedSubtractOverflow64(long lhs, long rhs, long* difference) {
    return __builtin_sub_overflow(lhs, rhs, difference);
}

constexpr bool mongoSignedSubtractOverflow64(long long lhs, long long rhs, long long* difference) {
    return __builtin_sub_overflow(lhs, rhs, difference);
}

constexpr bool mongoUnsignedSubtractOverflow64(unsigned long lhs,
                                               unsigned long rhs,
                                               unsigned long* sum) {
    return __builtin_sub_overflow(lhs, rhs, sum);
}

constexpr bool mongoUnsignedSubtractOverflow64(unsigned long long lhs,
                                               unsigned long long rhs,
                                               unsigned long long* sum) {
    return __builtin_sub_overflow(lhs, rhs, sum);
}

#endif

/**
 * Safe mod function which throws if the divisor is 0 and avoids potential overflow in cases where
 * the divisor is -1. If the absolute value of the divisor is 1, mod will always return 0. We fast-
 * path this to avoid the scenario where the dividend is the smallest negative long or int value and
 * the divisor is -1. Naively performing this % may result in an overflow when the -2^N value is
 * divided and becomes 2^N. See SERVER-43699.
 */
template <typename T>
constexpr T mongoSafeMod(T dividend, T divisor) {
    uassert(51259, "can't mod by zero", divisor != 0);
    return (divisor == 1 || divisor == -1 ? 0 : dividend % divisor);
}

}  // namespace mongo
