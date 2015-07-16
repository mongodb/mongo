/**
 * Copyright (c) 2011 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {
using boost::intrusive_ptr;

REGISTER_ACCUMULATOR(stdDevPop, AccumulatorStdDevPop::create);
REGISTER_ACCUMULATOR(stdDevSamp, AccumulatorStdDevSamp::create);
REGISTER_EXPRESSION(stdDevPop, ExpressionFromAccumulator<AccumulatorStdDevPop>::parse);
REGISTER_EXPRESSION(stdDevSamp, ExpressionFromAccumulator<AccumulatorStdDevSamp>::parse);

const char* AccumulatorStdDev::getOpName() const {
    return (_isSamp ? "$stdDevSamp" : "$stdDevPop");
}

void AccumulatorStdDev::processInternal(const Value& input, bool merging) {
    if (!merging) {
        // non numeric types have no impact on standard deviation
        if (!input.numeric())
            return;

        const double val = input.getDouble();

        // This is an implementation of the following algorithm:
        // http://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Online_algorithm
        _count += 1;
        const double delta = val - _mean;
        _mean += delta / _count;
        _m2 += delta * (val - _mean);
    } else {
        // This is what getValue(true) produced below.
        verify(input.getType() == Object);
        const double m2 = input["m2"].getDouble();
        const double mean = input["mean"].getDouble();
        const long long count = input["count"].getLong();

        if (count == 0)
            return;  // This partition had no data to contribute.

        // This is an implementation of the following algorithm:
        // http://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Parallel_algorithm
        const double delta = mean - _mean;
        const long long newCount = count + _count;

        _mean = ((_count * _mean) + (count * mean)) / newCount;
        _m2 += m2 + (delta * delta * (double(_count) * count / newCount));
        _count = newCount;
    }
}

Value AccumulatorStdDev::getValue(bool toBeMerged) const {
    if (!toBeMerged) {
        const long long adjustedCount = (_isSamp ? _count - 1 : _count);
        if (adjustedCount <= 0)
            return Value(BSONNULL);  // standard deviation not well defined in this case

        return Value(sqrt(_m2 / adjustedCount));
    } else {
        return Value(DOC("m2" << _m2 << "mean" << _mean << "count" << _count));
    }
}

intrusive_ptr<Accumulator> AccumulatorStdDevSamp::create() {
    return new AccumulatorStdDevSamp();
}

intrusive_ptr<Accumulator> AccumulatorStdDevPop::create() {
    return new AccumulatorStdDevPop();
}

AccumulatorStdDev::AccumulatorStdDev(bool isSamp) : _isSamp(isSamp), _count(0), _mean(0), _m2(0) {
    // This is a fixed size Accumulator so we never need to update this
    _memUsageBytes = sizeof(*this);
}

void AccumulatorStdDev::reset() {
    _count = 0;
    _mean = 0;
    _m2 = 0;
}
}
