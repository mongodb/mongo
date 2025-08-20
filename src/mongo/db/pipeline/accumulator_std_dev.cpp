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

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_helpers.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"
#include "mongo/db/pipeline/window_function/window_function_stddev.h"
#include "mongo/util/assert_util.h"

#include <cmath>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

template <>
Value ExpressionFromAccumulator<AccumulatorStdDevPop>::evaluate(const Document& root,
                                                                Variables* variables) const {
    return evaluateAccumulator(*this, root, variables);
}

template <>
Value ExpressionFromAccumulator<AccumulatorStdDevSamp>::evaluate(const Document& root,
                                                                 Variables* variables) const {
    return evaluateAccumulator(*this, root, variables);
}

REGISTER_ACCUMULATOR(stdDevPop, genericParseSingleExpressionAccumulator<AccumulatorStdDevPop>);
REGISTER_ACCUMULATOR(stdDevSamp, genericParseSingleExpressionAccumulator<AccumulatorStdDevSamp>);
REGISTER_STABLE_EXPRESSION(stdDevPop, ExpressionFromAccumulator<AccumulatorStdDevPop>::parse);
REGISTER_STABLE_EXPRESSION(stdDevSamp, ExpressionFromAccumulator<AccumulatorStdDevSamp>::parse);
REGISTER_STABLE_REMOVABLE_WINDOW_FUNCTION(stdDevPop, AccumulatorStdDevPop, WindowFunctionStdDevPop);
REGISTER_STABLE_REMOVABLE_WINDOW_FUNCTION(stdDevSamp,
                                          AccumulatorStdDevSamp,
                                          WindowFunctionStdDevSamp);

void AccumulatorStdDev::processInternal(const Value& input, bool merging) {
    if (!merging) {
        // non numeric types have no impact on standard deviation
        if (!input.numeric())
            return;

        const double val = input.getDouble();

        // This is an implementation of the following algorithm:
        // https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Welford's_online_algorithm
        _count += 1;
        const double delta = val - _mean;
        if (delta != 0.0) {
            _mean += delta / _count;
            _m2 += delta * (val - _mean);
        }
    } else {
        // This is what getValue(true) produced below.
        assertMergingInputType(input, BSONType::object);
        const double m2 = input["m2"].getDouble();
        const double mean = input["mean"].getDouble();
        const long long count = input["count"].getLong();

        if (count == 0)
            return;  // This partition had no data to contribute.

        // This is an implementation of the following algorithm:
        // http://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Parallel_algorithm
        const double delta = mean - _mean;
        const long long newCount = count + _count;

        if (delta != 0.0) {
            // Avoid potential numerical stability issues.
            _mean = ((_count * _mean) + (count * mean)) / newCount;
            _m2 += (delta * (double(_count) * count / newCount)) * delta;
        }
        _m2 += m2;

        _count = newCount;
    }
}

Value AccumulatorStdDev::getValue(bool toBeMerged) {
    if (!toBeMerged) {
        const long long adjustedCount = (_isSamp ? _count - 1 : _count);
        if (adjustedCount <= 0)
            return Value(BSONNULL);  // standard deviation not well defined in this case

        return Value(sqrt(_m2 / adjustedCount));
    } else {
        return Value(DOC("m2" << _m2 << "mean" << _mean << "count" << _count));
    }
}

AccumulatorStdDev::AccumulatorStdDev(ExpressionContext* const expCtx, bool isSamp)
    : AccumulatorState(expCtx), _isSamp(isSamp), _count(0), _mean(0), _m2(0) {
    // This is a fixed size AccumulatorState so we never need to update this
    _memUsageTracker.set(sizeof(*this));
}

void AccumulatorStdDev::reset() {
    _count = 0;
    _mean = 0;
    _m2 = 0;
}
}  // namespace mongo
