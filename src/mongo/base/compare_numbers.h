/*    Copyright 2015 MongoDB Inc.
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

#include <cmath>

#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"

namespace mongo {

/**
 * These functions compare numbers using the same rules as BSON. Care is taken to always give
 * numerically correct results when comparing different types. Returns are always -1, 0, or 1 to
 * ensure it is safe to negate the result to invert the direction of the comparison.
 *
 * lhs > rhs returns 1
 * lhs < rhs returns -1
 * lhs == rhs returns 0
 *
 */

inline int compareInts(int lhs, int rhs) {
    return lhs == rhs ? 0 : lhs < rhs ? -1 : 1;
}

inline int compareLongs(long long lhs, long long rhs) {
    return lhs == rhs ? 0 : lhs < rhs ? -1 : 1;
}

inline int compareDoubles(double lhs, double rhs) {
    if (lhs == rhs)
        return 0;
    if (lhs < rhs)
        return -1;
    if (lhs > rhs)
        return 1;

    // If none of the above cases returned, lhs or rhs must be NaN.
    if (std::isnan(lhs))
        return std::isnan(rhs) ? 0 : -1;
    dassert(std::isnan(rhs));
    return 1;
}

// This is the tricky one. Needs to support the following cases:
// * Doubles with a fractional component.
// * Longs that can't be precisely represented as a double.
// * Doubles outside of the range of Longs (including +/- Inf).
// * NaN (defined by us as less than all Longs)
// * Return value is always -1, 0, or 1 to ensure it is safe to negate.
inline int compareLongToDouble(long long lhs, double rhs) {
    // All Longs are > NaN
    if (std::isnan(rhs))
        return 1;

    // Ints with magnitude <= 2**53 can be precisely represented as doubles.
    // Additionally, doubles outside of this range can't have a fractional component.
    static const long long kEndOfPreciseDoubles = 1ll << 53;
    if (lhs <= kEndOfPreciseDoubles && lhs >= -kEndOfPreciseDoubles) {
        return compareDoubles(lhs, rhs);
    }

    // Large magnitude doubles (including +/- Inf) are strictly > or < all Longs.
    static const double kBoundOfLongRange = -static_cast<double>(LLONG_MIN);  // positive 2**63
    if (rhs >= kBoundOfLongRange)
        return -1;  // Can't be represented in a Long.
    if (rhs < -kBoundOfLongRange)
        return 1;  // Can be represented in a Long.

    // Remaining Doubles can have their integer component precisely represented as long longs.
    // If they have a fractional component, they must be strictly > or < lhs even after
    // truncation of the fractional component since low-magnitude lhs were handled above.
    return compareLongs(lhs, rhs);
}

inline int compareDoubleToLong(double lhs, long long rhs) {
    // Only implement the real logic once.
    return -compareLongToDouble(rhs, lhs);
}

/** Decimal type comparisons
 * These following cases need support:
 * 1. decimal and decimal: directly compare (enforce ordering: NaN < -Inf < N < +Inf)
 * 2. decimal and int: convert int to decimal and compare
 * 3. decimal and long: convert long to decimal and compare
 * 4. decimal to double: convert decimal to double with round toward negative.
 *    Check for exact conversion and determine ordering based on result.
 */

// Case 1: Compare two decimal values, but enforce MongoDB's total ordering convention
inline int compareDecimals(Decimal128 lhs, Decimal128 rhs) {
    // When we're comparing, lhs is always a decimal, which means more often than not
    // the rhs will be less than the lhs (decimal type has the largest capacity)
    if (lhs.isGreater(rhs))
        return 1;
    if (lhs.isLess(rhs))
        return -1;
    if (lhs.isNaN())
        return (rhs.isNaN() ? 0 : -1);
    if (rhs.isNaN())
        return 1;
    else  // lhs is necessarily equal to rhs
        return 0;
}

// Compare decimal and int
inline int compareDecimalToInt(Decimal128 lhs, int rhs) {
    return compareDecimals(lhs, Decimal128(rhs));
}

inline int compareIntToDecimal(int lhs, Decimal128 rhs) {
    return -compareDecimalToInt(rhs, lhs);
}

// Compare decimal and long
inline int compareDecimalToLong(Decimal128 lhs, long long rhs) {
    return compareDecimals(lhs, Decimal128(static_cast<int64_t>(rhs)));
}

inline int compareLongToDecimal(long long lhs, Decimal128 rhs) {
    return -compareDecimalToLong(rhs, lhs);
}

// Compare decimal and double
inline int compareDecimalToDouble(Decimal128 lhs, double rhs) {
    uint32_t sigFlags = Decimal128::SignalingFlag::kNoFlag;
    double decToDouble = lhs.toDouble(&sigFlags, Decimal128::RoundingMode::kRoundTowardNegative);

    if (decToDouble == rhs) {
        // If our conversion was not exact, lhs was necessarily greater than rhs
        // otherwise, they are equal
        if (Decimal128::hasFlag(sigFlags, Decimal128::SignalingFlag::kInexact)) {
            return 1;
        } else {
            return 0;
        }
    } else if (decToDouble < rhs) {
        return -1;
    } else if (decToDouble > rhs) {
        return 1;
    }

    if (lhs.isNaN())
        return (std::isnan(rhs) ? 0 : -1);
    invariant(std::isnan(rhs));
    return 1;
}

inline int compareDoubleToDecimal(double lhs, Decimal128 rhs) {
    return -compareDecimalToDouble(rhs, lhs);
}

}  // namespace mongo
