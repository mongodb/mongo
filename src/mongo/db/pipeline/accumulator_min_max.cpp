// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_helpers.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"
#include "mongo/db/pipeline/window_function/window_function_min_max.h"


namespace mongo {

template <>
Value ExpressionFromAccumulator<AccumulatorMax>::evaluate(const Document& root,
                                                          Variables* variables,
                                                          const EvaluationContext& ctx) const {
    return evaluateAccumulator(*this, root, variables, ctx);
}

template <>
Value ExpressionFromAccumulator<AccumulatorMin>::evaluate(const Document& root,
                                                          Variables* variables,
                                                          const EvaluationContext& ctx) const {
    return evaluateAccumulator(*this, root, variables, ctx);
}

REGISTER_ACCUMULATOR(max, genericParseSingleExpressionAccumulator<AccumulatorMax>);
REGISTER_ACCUMULATOR(min, genericParseSingleExpressionAccumulator<AccumulatorMin>);
REGISTER_STABLE_EXPRESSION(max, ExpressionFromAccumulator<AccumulatorMax>::parse);
REGISTER_STABLE_EXPRESSION(min, ExpressionFromAccumulator<AccumulatorMin>::parse);
REGISTER_STABLE_REMOVABLE_WINDOW_FUNCTION(max, AccumulatorMax, WindowFunctionMax);
REGISTER_STABLE_REMOVABLE_WINDOW_FUNCTION(min, AccumulatorMin, WindowFunctionMin);

void AccumulatorMinMax::processInternal(const Value& input, bool merging) {
    // nullish values should have no impact on result
    if (!input.nullish()) {
        /* compare with the current value; swap if appropriate */
        int cmp = getExpressionContext()->getValueComparator().compare(_val, input) * _sense;
        if (cmp > 0 || _val.missing()) {  // missing is lower than all other values
            _val = input;
            _memUsageTracker.set(sizeof(*this) + input.getApproximateSize() - sizeof(Value));
        }
    }
}

Value AccumulatorMinMax::getValue(bool toBeMerged) {
    if (_val.missing()) {
        return Value(BSONNULL);
    }
    return _val;
}

AccumulatorMinMax::AccumulatorMinMax(ExpressionContext* const expCtx, Sense sense)
    : AccumulatorState(expCtx), _sense(sense) {
    _memUsageTracker.set(sizeof(*this));
}

void AccumulatorMinMax::reset() {
    _val = Value();
    _memUsageTracker.set(sizeof(*this));
}
}  // namespace mongo
