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

#include "mongo/platform/basic.h"

#include "summation.h"

#include <cmath>

#include "mongo/util/assert_util.h"

namespace mongo {
void DoubleDoubleSummation::addLong(long long x) {
    // Split 64-bit integers into two doubles, so the sum remains exact.
    int64_t high = x / (1ll << 32) * (1ll << 32);
    int64_t low = x - high;
    dassert(high + low == x && 1.0 * high == high && 1.0 * low == low);
    addDouble(low);
    addDouble(high);
}

/**
 * Returns whether the sum is in range of the 64-bit signed integer long long type.
 */
bool DoubleDoubleSummation::fitsLong() const {
    using limits = std::numeric_limits<long long>;
    // Fast path: if the rounded _sum is strictly between the minimum and maximum long long value,
    // it must be valid. This is the common case. Note that this is correct for NaNs/infinities.
    if (_sum > limits::min() && _sum < limits::max())
        return true;

    // Now check the cases where the _sum equals one of the boundaries, and check the compensation
    // amount to determine to what integer the value would round.

    // If _sum is equal to limits::max() + 1, _addend must cause us to round down to a lower integer
    // and thus be strictly less than -0.5. limits.max() rounds up to limits.max() + 1, as double
    // precision does not have enough precision.
    if (_sum == limits::max())
        return _addend < -0.5;

    // If _sum is equal to limits::min(), _addend must not cause us to round down and thus be
    // greater than or equal to -0.5.
    if (_sum == limits::min())
        return _addend >= -0.5;

    // The sum is out of range, an infinity or a NaN.
    return false;
}

/**
 * Returns result of sum rounded to nearest integer, rounding half-way cases away from zero.
 */
long long DoubleDoubleSummation::getLong() const {
    uassert(ErrorCodes::Overflow, "sum out of range of a 64-bit signed integer", fitsLong());
    if (_sum == std::numeric_limits<long long>::max()) {
        // Can't directly convert, because _sum would overflow a signed 64-bit number.
        dassert(_addend < -0.5 && -_sum == std::numeric_limits<long long>::min());
        return llround(_addend) - std::numeric_limits<long long>::min();
    }
    long long sum = llround(_sum);
    sum += llround((_sum - sum) + _addend);
    return sum;
}
}  // namespace mongo
