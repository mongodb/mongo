// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
