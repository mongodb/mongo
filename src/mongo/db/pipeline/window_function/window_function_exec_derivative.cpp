// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/window_function/window_function_exec_derivative.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/db/pipeline/expression_context.h"

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

Value WindowFunctionExecDerivative::getNext(boost::optional<Document> current) {
    auto endpoints = _iter.getEndpoints(_bounds);
    if (!endpoints)
        return kDefault;

    auto [leftOffset, rightOffset] = *endpoints;
    const Document leftDoc = *(_iter)[leftOffset];
    const Document rightDoc = *(_iter)[rightOffset];

    // Conceptually, $derivative computes 'rise/run' where 'rise' is dimensionless and 'run' is
    // a time. The result has dimension 1/time, which doesn't correspond to any BSON type, so
    // 'unit' tells us how to express the result as a dimensionless BSON number.
    //
    // However, BSON also can't represent a time (duration) directly. BSONType::date represents
    // a point in time, but there is no type that represents an amount of time. Subtracting two
    // Date values implicitly converts them to milliseconds.

    // So, when we compute 'rise/run', the answer is expressed in units '1/millisecond'. If an
    // 'unit' is specified, we scale the answer by 'millisecond/unit' to
    // re-express it in '1/unit'.
    Value leftTime = _time->evaluate(leftDoc, &_time->getExpressionContext()->variables);
    Value rightTime = _time->evaluate(rightDoc, &_time->getExpressionContext()->variables);
    if (_unitMillis) {
        // If a unit is specified, we require both endpoints to be dates. We don't
        // want to interpret bare numbers as milliseconds, when we don't know what unit they
        // really represent.
        //
        // For example: imagine the '_time' field contains floats representing seconds: then
        // 'rise/run' will already be expressed in units of 1/second. If you think "my data is
        // seconds" and write 'unit: "second"', and we applied the scale factor of
        // 'millisecond/unit', then the final answer would be wrong by a factor of 1000.
        uassert(5624900,
                "$derivative with 'unit' expects the sortBy field to be a Date",
                leftTime.getType() == BSONType::date && rightTime.getType() == BSONType::date);
    } else {
        // Without unit, we require both time values to be numeric.
        uassert(5624901,
                "$derivative where the sortBy is a Date requires an 'unit'",
                leftTime.getType() != BSONType::date && rightTime.getType() != BSONType::date);
        uassert(5624902,
                "$derivative (with no 'unit') expects the sortBy field to be numeric",
                leftTime.numeric() && rightTime.numeric());
    }
    // Now leftTime and rightTime are either both numeric, or both dates.
    // $subtract on two dates gives us the difference in milliseconds.
    Value run = uassertStatusOK(
        exec::expression::evaluateSubtract(std::move(rightTime), std::move(leftTime)));

    Value rise = uassertStatusOK(exec::expression::evaluateSubtract(
        _position->evaluate(rightDoc, &_position->getExpressionContext()->variables),
        _position->evaluate(leftDoc, &_position->getExpressionContext()->variables)));
    uassert(5624903, "$derivative input must not be null or missing", !rise.nullish());

    auto divideStatus = exec::expression::evaluateDivide(std::move(rise), std::move(run));
    if (divideStatus.getStatus().code() == ErrorCodes::BadValue) {
        // Divide by zero can't be an error. On the first document of a partition, a window like
        // 'documents: [-1, 0]' contains only one document, so 'run' is zero.
        return kDefault;
    }
    Value result = uassertStatusOK(divideStatus);

    if (_unitMillis) {
        // 'result' has units 1/millisecond; scale by millisecond/unit to express in
        // 1/unit.

        // tassert because at this point the result should already be numeric, so if
        // evaluateMultiply returns a non-OK Status then something has gone wrong.
        auto statusWithResult = exec::expression::evaluateMultiply(result, Value(*_unitMillis));
        tassert(statusWithResult);
        result = statusWithResult.getValue();
    }
    return result;
}
}  // namespace mongo
