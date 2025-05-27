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
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_helpers.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"
#include "mongo/db/pipeline/window_function/window_function_min_max.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

template <>
Value ExpressionFromAccumulator<AccumulatorMax>::evaluate(const Document& root,
                                                          Variables* variables) const {
    return evaluateAccumulator(*this, root, variables);
}

template <>
Value ExpressionFromAccumulator<AccumulatorMin>::evaluate(const Document& root,
                                                          Variables* variables) const {
    return evaluateAccumulator(*this, root, variables);
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
