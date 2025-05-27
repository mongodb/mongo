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

#include "mongo/db/pipeline/window_function/window_function_exec_linear_fill.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {
namespace value_arithmetic_operators {
Value operator+(const Value& a, const Value& b) {
    return uassertStatusOK(exec::expression::evaluateAdd(a, b));
}
Value operator-(const Value& a, const Value& b) {
    return uassertStatusOK(exec::expression::evaluateSubtract(a, b));
}
Value operator*(const Value& a, const Value& b) {
    return uassertStatusOK(exec::expression::evaluateMultiply(a, b));
}
Value operator/(const Value& a, const Value& b) {
    return uassertStatusOK(exec::expression::evaluateDivide(a, b));
}
}  // namespace value_arithmetic_operators

// Given two known points (x1, y1) and (x2, y2) and a value x that lies between those two
// points, we solve (or fill) for y with the following formula: y = y1 + (x - x1) * ((y2 -
// y1)/(x2 - x1))
Value interpolate(Value x1, Value y1, Value x2, Value y2, Value x) {
    using namespace value_arithmetic_operators;
    return y1 + (x - x1) * ((y2 - y1) / (x2 - x1));
}
}  // namespace

boost::optional<Value> WindowFunctionExecLinearFill::evaluateInput(const Document& doc) {
    Value fillFieldValue = _input->evaluate(doc, &_input->getExpressionContext()->variables);
    if (!fillFieldValue.nullish()) {
        return boost::optional<Value>(fillFieldValue);
    }
    return boost::none;
}

boost::optional<std::pair<Value, Value>> WindowFunctionExecLinearFill::findX2Y2() {
    auto index = 1;
    while (const auto doc = _iter[index]) {
        if (const auto fillFieldValue = evaluateInput(*doc)) {
            Value sortFieldValue =
                _sortBy->evaluate(*doc, &_sortBy->getExpressionContext()->variables);
            if (!sortFieldValue.nullish()) {
                _prevX2Y2 = boost::optional<std::pair<Value, Value>>(
                    std::make_pair(sortFieldValue, *fillFieldValue));
                return _prevX2Y2;
            }
        }
        index++;
    }
    return boost::none;
}

Value WindowFunctionExecLinearFill::getNext(boost::optional<Document> current) {
    const auto currentDoc = *_iter[0];
    Value fillFieldValue = _input->evaluate(currentDoc, &_input->getExpressionContext()->variables);
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "Value to be filled must be numeric or nullish, but found "
                          << fillFieldValue.getType(),
            fillFieldValue.numeric() || fillFieldValue.nullish());
    Value sortFieldValue =
        _sortBy->evaluate(currentDoc, &_sortBy->getExpressionContext()->variables);
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "Value of the sortBy field must be numeric or a date, but found "
                          << sortFieldValue.getType(),
            sortFieldValue.numeric() || sortFieldValue.coercibleToDate());

    // We do not allow repeated sort field values. This is because if we have the following
    // collection that has a repeated sort value and different fill values, eg [(10, 100), (10,
    // -100), (20, null), (30, 50)], it is unclear if the left value, (X1, Y1), should be (10, 100)
    // or (10, -100) when we interpolate on the third document.

    uassert(6050106,
            "There can be no repeated values in the sort field in the same partition",
            ValueComparator{}.evaluate(sortFieldValue != _lastSeenElement));
    if (!_lastSeenElement.missing())
        // Throw an error If the sort value was previously of type numeric, but we've just found a
        // date (or vice versa).
        uassert(ErrorCodes::TypeMismatch,
                str::stream() << "Conflicting sort value types, previously received type "
                              << _lastSeenElement.getType() << ", but found "
                              << sortFieldValue.getType(),
                (sortFieldValue.coercibleToDate() && _lastSeenElement.coercibleToDate()) ||
                    (sortFieldValue.numeric() && _lastSeenElement.numeric()));
    _lastSeenElement = sortFieldValue;

    // We have found either (x1, y1) or (x2, y2).
    if (!fillFieldValue.nullish()) {
        // We can expire all documents before the current document in the cache. We don't want to
        // expire the current document, because it may become our *first* non-null, ie (x1, x2), for
        // the next set of null documents.
        _iter.manualExpireUpTo(-1);
        // If (x2, y2) is known, it becomes our (x1, y1) for the next sequence of null documents.
        // Otherwise the current document becomes (x1, y1).
        _prevX1Y1 = _prevX2Y2 ? _prevX2Y2
                              : boost::optional<std::pair<Value, Value>>(
                                    std::make_pair(sortFieldValue, fillFieldValue));
        _prevX2Y2 = boost::none;
        return fillFieldValue;
    }
    // Interpolation requires that the documents with null values for the field we are filling,
    // be bookended with documents with non-null sort and input field numeric values.
    // To do this, we store the previous known coordinates so that we don't scan the same null
    // documents multiple times as we search for a non-null value.
    auto firstCoord = _prevX1Y1;
    if (!firstCoord) {
        return kDefault;
    }

    auto secondCoord = _prevX2Y2 ? _prevX2Y2 : findX2Y2();
    if (!secondCoord) {
        return kDefault;
    }

    return interpolate(firstCoord->first,
                       firstCoord->second,
                       secondCoord->first,
                       secondCoord->second,
                       sortFieldValue);
}
}  // namespace mongo
