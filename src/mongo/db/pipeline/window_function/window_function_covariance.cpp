/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/pipeline/window_function/window_function_covariance.h"
#include "mongo/db/pipeline/window_function/window_function_sum.h"

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"

namespace mongo {

namespace {
// The input Value must be a vector of exactly two numeric Value.
bool validateValue(const Value& val) {
    return (val.isArray() && val.getArray().size() == 2 && val.getArray()[0].numeric() &&
            val.getArray()[1].numeric());
}

// Convert the non-finite input value to a single Value that is then added to the underlying
// 'WindowFunctionSum'.
Value convertNonFiniteInputValue(Value value) {
    int posCnt = 0, negCnt = 0, nanCnt = 0;
    bool isDecimal = false;
    for (auto val : value.getArray()) {
        if (val.isNaN()) {
            nanCnt++;
        } else if (val.getType() == NumberDecimal) {
            if (val.isInfinite())
                val.coerceToDecimal().isNegative() ? negCnt++ : posCnt++;
            isDecimal = true;
        } else if (val.numeric()) {
            auto doubleVal = val.coerceToDouble();
            if (doubleVal == std::numeric_limits<double>::infinity())
                posCnt++;
            else if (doubleVal == -std::numeric_limits<double>::infinity())
                negCnt++;
        }
    }

    // Should return NaN over Inf value if both NaN and Inf exist.
    // Returns NaN if any NaN in 'value' or the two values are of different sign.
    if (nanCnt > 0 || posCnt * negCnt > 0)
        return isDecimal ? Value(Decimal128::kPositiveNaN)
                         : Value(std::numeric_limits<double>::quiet_NaN());

    if (isDecimal)
        return posCnt > 0 ? Value(Decimal128::kPositiveInfinity)
                          : Value(Decimal128::kNegativeInfinity);
    else
        return posCnt > 0 ? Value(std::numeric_limits<double>::infinity())
                          : Value(-std::numeric_limits<double>::infinity());
}
}  // namespace

WindowFunctionCovariance::WindowFunctionCovariance(ExpressionContext* const expCtx, bool isSamp)
    : WindowFunctionState(expCtx), _isSamp(isSamp), _meanX(expCtx), _meanY(expCtx), _cXY(expCtx) {
    _memUsageBytes = sizeof(*this);
}

Value WindowFunctionCovariance::getValue() const {
    if (_count == 1 && !_isSamp)
        return Value(0.0);

    const double adjustedCount = (_isSamp ? _count - 1 : _count);
    if (adjustedCount <= 0)
        return kDefault;  // Covariance not well defined in this case.

    auto output = _cXY.getValue();
    if (output.getType() == NumberDecimal) {
        output = uassertStatusOK(ExpressionDivide::apply(output, Value(adjustedCount)));
    } else if (output.numeric()) {
        output = Value(output.coerceToDouble() / adjustedCount);
    }

    return output;
}

void WindowFunctionCovariance::add(Value value) {
    // Not supported type of input have no impact on covariance.
    if (!validateValue(value))
        return;

    const auto& arr = value.getArray();
    // The non-finite (nan/inf) value is handled by 'WindowFunctionSum' directly and is not taken
    // into account when calculating the intermediate values and covariance.
    if (arr[0].isNaN() || arr[1].isNaN() || arr[0].isInfinite() || arr[1].isInfinite()) {
        auto infValue = convertNonFiniteInputValue(value);
        _cXY.add(infValue);
        return;
    }

    _count++;
    // Update covariance and means.
    // This is an implementation of the following algorithm:
    // https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Online
    auto deltaX = uassertStatusOK(ExpressionSubtract::apply(arr[0], _meanX.getValue()));
    _meanX.add(arr[0]);
    _meanY.add(arr[1]);
    auto deltaY = uassertStatusOK(ExpressionSubtract::apply(arr[1], _meanY.getValue()));
    auto deltaCXY = uassertStatusOK(ExpressionMultiply::apply(deltaX, deltaY));
    _cXY.add(deltaCXY);
}

void WindowFunctionCovariance::remove(Value value) {
    // Not supported type of input have no impact on covariance.
    if (!validateValue(value))
        return;

    const auto& arr = value.getArray();
    if (arr[0].isNaN() || arr[1].isNaN() || arr[0].isInfinite() || arr[1].isInfinite()) {
        auto infValue = convertNonFiniteInputValue(value);
        _cXY.remove(infValue);
        return;
    }

    tassert(5424100, "Can't remove from an empty WindowFunctionCovariance", _count > 0);
    _count--;
    if (_count == 0) {
        reset();
        return;
    }

    _meanX.remove(arr[0]);
    auto deltaX = uassertStatusOK(ExpressionSubtract::apply(arr[0], _meanX.getValue()));
    auto deltaY = uassertStatusOK(ExpressionSubtract::apply(arr[1], _meanY.getValue()));
    auto deltaCXY = uassertStatusOK(ExpressionMultiply::apply(deltaX, deltaY));
    _cXY.remove(deltaCXY);
    _meanY.remove(arr[1]);
}

void WindowFunctionCovariance::reset() {
    _count = 0;
    _meanX.reset();
    _meanY.reset();
    _cXY.reset();
}

}  // namespace mongo
