/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/pipeline/window_function/window_function_min_max_scaler_util.h"

#include "mongo/db/exec/expression/evaluate.h"

namespace mongo {
namespace min_max_scaler {

Value computeResult(const Value& currentValue,
                    const MinAndMax& windowMinAndMax,
                    const MinAndMax& sMinAndMax) {
    tassert(9522905,
            "All provided arguments to min_max_scaler::computeResult must be numeric Value types.",
            currentValue.numeric() && windowMinAndMax.min().numeric() &&
                windowMinAndMax.max().numeric() && sMinAndMax.min().numeric() &&
                sMinAndMax.max().numeric());
    tassert(9459903,
            "The current value must be bounded (inclusively) between the min and max "
            "of the window. Current value = " +
                currentValue.toString() +
                ", window min/max = "
                "[" +
                windowMinAndMax.min().toString() + ", " + windowMinAndMax.max().toString() + "]",
            (Value::compare(currentValue, windowMinAndMax.min(), nullptr) >= 0) &&
                (Value::compare(currentValue, windowMinAndMax.max(), nullptr) <= 0));

    // Check for the special case where the min and max of the window are equal,
    // meaning there is no range of the possible output domain. In this case, we squash the
    // output value to the minimum of the output domain.
    // Mathematically, we prevent a divide by zero (when max is subtracted by min in the
    // denominator).
    if (Value::compare(windowMinAndMax.min(), windowMinAndMax.max(), nullptr) == 0) {
        return Value(sMinAndMax.min());
    }

    // Get the return value scaled between 0 and 1.
    Value minMaxUnscaled = uassertStatusOK(exec::expression::evaluateDivide(
        uassertStatusOK(exec::expression::evaluateSubtract(currentValue, windowMinAndMax.min())),
        uassertStatusOK(
            exec::expression::evaluateSubtract(windowMinAndMax.max(), windowMinAndMax.min()))));

    // Scale the return value between the configured bounds.
    return uassertStatusOK(
        exec::expression::evaluateAdd(uassertStatusOK(exec::expression::evaluateMultiply(
                                          minMaxUnscaled,
                                          uassertStatusOK(exec::expression::evaluateSubtract(
                                              sMinAndMax.max(), sMinAndMax.min())))),
                                      sMinAndMax.min()));
}

};  // namespace min_max_scaler
};  // namespace mongo
