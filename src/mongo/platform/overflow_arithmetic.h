/*    Copyright 2016 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <cstdint>

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace mongo {

/**
 * Returns true if multiplying lhs by rhs would overflow. Otherwise, ultiplies 64-bit signed
 * integers lhs by rhs and stores the result in *product.
 */
inline bool mongoSignedMultiplyOverflow64(int64_t lhs, int64_t rhs, int64_t* product);

/**
 * Returns true if adding lhs and rhs would overflow. Otherwise, adds 64-bit signed integers lhs and
 * rhs and stores the result in *sum.
 */
inline bool mongoSignedAddOverflow64(int64_t lhs, int64_t rhs, int64_t* sum);

/**
 * Returns true if subtracting rhs from lhs would overflow. Otherwise, subtracts 64-bit signed
 * integers rhs from lhs and stores the result in *difference.
 */
inline bool mongoSignedSubtractOverflow64(int64_t lhs, int64_t rhs, int64_t* difference);


#ifdef _MSC_VER

// In MSVC, we use an MSVC intrinsic for the multiplication form of this calculation, and unsigned
// arithmetic and careful casting for addition and subtraction.

inline bool mongoSignedMultiplyOverflow64(int64_t lhs, int64_t rhs, int64_t* product) {
    int64_t hi;
    *product = _mul128(lhs, rhs, &hi);
    if (hi == 0) {
        return *product < 0;
    }
    if (hi == -1) {
        return *product >= 0;
    }
    return true;
}

inline bool mongoSignedAddOverflow64(int64_t lhs, int64_t rhs, int64_t* sum) {
    *sum = static_cast<int64_t>(static_cast<uint64_t>(lhs) + static_cast<uint64_t>(rhs));
    if (lhs >= 0 && rhs >= 0) {
        return (*sum) < 0;
    }
    if (lhs < 0 && rhs < 0) {
        return (*sum) >= 0;
    }
    return false;
}

inline bool mongoSignedSubtractOverflow64(int64_t lhs, int64_t rhs, int64_t* difference) {
    *difference = static_cast<int64_t>(static_cast<uint64_t>(lhs) - static_cast<uint64_t>(rhs));
    if (lhs >= 0 && rhs < 0) {
        return (*difference) < 0;
    }
    if (lhs < 0 && rhs >= 0) {
        return (*difference >= 0);
    }
    return false;
}

#else

// On GCC and CLANG we can use __builtin functions to perform these calculations.

inline bool mongoSignedMultiplyOverflow64(long lhs, long rhs, long* product) {
    return __builtin_smull_overflow(lhs, rhs, product);
}

inline bool mongoSignedAddOverflow64(long lhs, long rhs, long* sum) {
    return __builtin_saddl_overflow(lhs, rhs, sum);
}

inline bool mongoSignedSubtractOverflow64(long lhs, long rhs, long* difference) {
    return __builtin_ssubl_overflow(lhs, rhs, difference);
}

inline bool mongoSignedMultiplyOverflow64(long long lhs, long long rhs, long long* product) {
    return __builtin_smulll_overflow(lhs, rhs, product);
}

inline bool mongoSignedAddOverflow64(long long lhs, long long rhs, long long* sum) {
    return __builtin_saddll_overflow(lhs, rhs, sum);
}

inline bool mongoSignedSubtractOverflow64(long long lhs, long long rhs, long long* difference) {
    return __builtin_ssubll_overflow(lhs, rhs, difference);
}

#endif

}  // namespace mongo
