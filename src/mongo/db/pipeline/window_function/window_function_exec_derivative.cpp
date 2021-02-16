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

#include "mongo/db/pipeline/window_function/window_function_exec_derivative.h"

namespace mongo {

namespace {
// Convert expected error codes to BSONNULL, but uassert other unexpected codes.
Value orNull(StatusWith<Value> val) {
    if (val.getStatus().code() == ErrorCodes::BadValue ||
        val.getStatus().code() == ErrorCodes::TypeMismatch)
        return Value(BSONNULL);
    return uassertStatusOK(val);
}
}  // namespace

Value WindowFunctionExecDerivative::getNext() {
    auto endpoints = _iter->getEndpoints(_bounds);
    if (!endpoints)
        return kDefault;

    auto [leftOffset, rightOffset] = *endpoints;
    const Document& leftDoc = *(*_iter)[leftOffset];
    const Document& rightDoc = *(*_iter)[rightOffset];

    // Conceptually, $derivative computes 'rise/run' where 'rise' is dimensionless and 'run' is
    // a time. The result has dimension 1/time, which doesn't correspond to any BSON type, so
    // 'outputUnit' tells us how to express the result as a dimensionless BSON number.
    //
    // However, BSON also can't represent a time (duration) directly. BSONType::Date represents
    // a point in time, but there is no type that represents an amount of time. Subtracting two
    // Date values implicitly converts them to milliseconds.

    // So, when we compute 'rise/run', the answer is expressed in units '1/millisecond'. If an
    // 'outputUnit' is specified, we scale the answer by 'millisecond/outputUnit' to
    // re-express it in '1/outputUnit'.
    Value leftTime = _time->evaluate(leftDoc, &_time->getExpressionContext()->variables);
    Value rightTime = _time->evaluate(rightDoc, &_time->getExpressionContext()->variables);
    if (_outputUnitMillis) {
        // If an outputUnit is specified, we require both endpoints to be dates. We don't
        // want to interpret bare numbers as milliseconds, when we don't know what unit they
        // really represent.
        //
        // For example: imagine the '_time' field contains floats representing seconds: then
        // 'rise/run' will already be expressed in units of 1/second. If you think "my data is
        // seconds" and write 'outputUnit: "second"', and we applied the scale factor of
        // 'millisecond/outputUnit', then the final answer would be wrong by a factor of 1000.
        if (leftTime.getType() != BSONType::Date || rightTime.getType() != BSONType::Date) {
            return kDefault;
        }
    } else {
        // Without outputUnit, we require both time values to be numeric.
        if (!leftTime.numeric() || !rightTime.numeric())
            return kDefault;
    }
    // Now leftTime and rightTime are either both numeric, or both dates.
    // $subtract on two dates gives us the difference in milliseconds.
    Value run = orNull(ExpressionSubtract::apply(std::move(rightTime), std::move(leftTime)));
    if (run.nullish())
        return kDefault;

    Value rise = orNull(ExpressionSubtract::apply(
        _position->evaluate(rightDoc, &_position->getExpressionContext()->variables),
        _position->evaluate(leftDoc, &_position->getExpressionContext()->variables)));
    if (rise.nullish())
        return kDefault;

    Value result = orNull(ExpressionDivide::apply(std::move(rise), std::move(run)));
    if (result.nullish())
        return kDefault;

    if (_outputUnitMillis) {
        // 'result' has units 1/millisecond; scale by millisecond/outputUnit to express in
        // 1/outputUnit.

        // tassert because at this point the result should already be numeric, so if
        // ExpressionMultiply returns a non-OK Status then something has gone wrong.
        auto statusWithResult = ExpressionMultiply::apply(result, Value(*_outputUnitMillis));
        tassert(statusWithResult);
        result = statusWithResult.getValue();
    }
    return result;
}
}  // namespace mongo
