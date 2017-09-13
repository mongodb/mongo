/**
 *    Copyright (C) 2016 MongoDB Inc.
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
#include <tuple>
#include <utility>

#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"

namespace mongo {

using DoubleDouble = std::pair<double, double>;

/**
 * Class to accurately sum series of numbers using a 2Sum and Fast2Sum formulas to maintain an
 * unevaluated sum of two numbers: a rounded-to-nearest _sum and an _addend.
 * See Sylvie Boldo, Stef Graillat, Jean-Michel Muller. On the robustness of the 2Sum and Fast2Sum
 * algorithms. 2016. https://hal-ens-lyon.archives-ouvertes.fr/ensl-01310023
 */
class DoubleDoubleSummation {
public:
    /**
     * Adds x to the sum, keeping track of a compensation amount to be subtracted later.
     */
    void addDouble(double x) {
        _special += x;                                 // Keep a simple sum to use in case of NaN
        std::tie(x, _addend) = _fast2Sum(x, _addend);  // Compensated add: _addend tinier than _sum
        std::tie(_sum, x) = _2Sum(_sum, x);            // Compensated add: x maybe larger than _sum
        _addend += x;                                  // Store away lowest part of sum
    }

    /**
     * Adds x to internal sum. Extra precision guarantees that sum is exact, unless intermediate
     * sums exceed a magnitude of 2**106.
     */
    void addLong(long long x);

    /**
     * Adds x to internal sum. Adds as double as that is more efficient.
     */
    void addInt(int x) {
        addDouble(x);
    }

    /**
     * Returns the double nearest to the accumulated sum.
     */
    double getDouble() const {
        return std::isnan(_sum) ? _special : _sum;
    }

    /**
     * Return a pair of double representing the sum, with first being the nearest double and second
     * the amount to add for full precision.
     */
    DoubleDouble getDoubleDouble() const {
        return std::isnan(_sum) ? DoubleDouble{_special, 0.0} : DoubleDouble{_sum, _addend};
    }

    /**
     * The result will generally have about 107 bits of precision, or about 32 decimal digits.
     * Summations of even extremely long series of 32-bit and 64-bit integers should be exact.
     */
    Decimal128 getDecimal() const {
        return std::isnan(_sum) ? Decimal128(_special, Decimal128::kRoundTo34Digits)
                                : Decimal128(_sum, Decimal128::kRoundTo34Digits)
                                      .add(Decimal128(_addend, Decimal128::kRoundTo34Digits));
    }

    /**
     * Returns whether the sum is in range of the 64-bit signed integer long long type.
     */
    bool fitsLong() const;

    /**
     * Returns whether the accumulated sum has a fractional part.
     */
    bool isInteger() const {
        return std::trunc(_sum) == _sum && std::trunc(_addend) == _addend;
    }

    /**
     * Returns result of sum rounded to nearest integer, rounding half-way cases away from zero.
     */
    long long getLong() const;

private:
    /**
     * Assuming |b| <= |a|, returns exact unevaluated sum of a and b, where the first member is the
     * double nearest the sum (ties to even) and the second member is the remainder.
     *
     * T. J. Dekker. A floating-point technique for extending the available precision. Numerische
     * Mathematik, 18(3):224–242, 1971.
     */
    DoubleDouble _fast2Sum(double a, double b) {
        double s = a + b;
        double z = s - a;
        double t = b - z;
        return {s, t};
    }

    /**
     * returns exact unevaluated sum of a and b, where the first member is the double nearest the
     * sum (ties to even) and the second member is the remainder.
     *
     * O. Møller. Quasi double-precision in floating-point addition. BIT, 5:37–50, 1965.
     * D. Knuth. The Art of Computer Programming, vol 2. Addison-Wesley, Reading, MA, 3rd ed, 1998.
     */
    DoubleDouble _2Sum(double a, double b) {
        double s = a + b;
        double aPrime = s - b;
        double bPrime = s - aPrime;
        double deltaA = a - aPrime;
        double deltaB = b - bPrime;
        double t = deltaA + deltaB;
        return {s, t};
    }

    double _sum = 0.0;
    double _addend = 0.0;

    // Simple sum to be returned if _sum is NaN. This addresses infinities turning into NaNs when
    // using compensated addition.
    double _special = 0.0;
};
}  // namespace mongo
