/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/variables.h"

namespace mongo {

template <typename AccumulatorState>
Value evaluateAccumulator(const ExpressionFromAccumulator<AccumulatorState>& expr,
                          const Document& root,
                          Variables* variables) {
    AccumulatorState accum(expr.getExpressionContext());
    const auto n = expr.getChildren().size();
    // If a single array arg is given, loop through it passing each member to the accumulator.
    // If a single, non-array arg is given, pass it directly to the accumulator.
    if (n == 1) {
        Value singleVal = expr.getChildren()[0]->evaluate(root, variables);
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
            accum.process(argument->evaluate(root, variables), false);
        }
    }
    return accum.getValue(false);
}

}  // namespace mongo
