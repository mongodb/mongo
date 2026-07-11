// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

#include <cmath>


namespace mongo {

template <>
Value ExpressionFromAccumulator<AccumulatorStdDevPop>::evaluate(
    const Document& root, Variables* variables, const EvaluationContext& ctx) const {
    return evaluateAccumulator(*this, root, variables, ctx);
}

template <>
Value ExpressionFromAccumulator<AccumulatorStdDevSamp>::evaluate(
    const Document& root, Variables* variables, const EvaluationContext& ctx) const {
    return evaluateAccumulator(*this, root, variables, ctx);
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
