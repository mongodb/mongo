// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/util/modules.h"

namespace mongo {

template <typename AccumulatorState>
Value evaluateAccumulator(const ExpressionFromAccumulator<AccumulatorState>& expr,
                          const Document& root,
                          Variables* variables,
                          const EvaluationContext& ctx) {
    AccumulatorState accum(expr.getExpressionContext());
    const auto n = expr.getChildren().size();
    // If a single array arg is given, loop through it passing each member to the accumulator.
    // If a single, non-array arg is given, pass it directly to the accumulator.
    if (n == 1) {
        Value singleVal = expr.getChildren()[0]->evaluate(root, variables, ctx);
        if (singleVal.getType() == BSONType::array) {
            for (const Value& val : singleVal.getArray()) {
                accum.process(val, false);
            }
        } else {
            accum.process(singleVal, false);
        }
    } else {
        // If multiple arguments are given, pass all arguments to the accumulator.
        for (auto&& argument : expr.getChildren()) {
            accum.process(argument->evaluate(root, variables, ctx), false);
        }
    }
    return accum.getValue(false);
}

}  // namespace mongo
