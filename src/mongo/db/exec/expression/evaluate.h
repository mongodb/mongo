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

#include <boost/optional/optional.hpp>

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace exec::expression {

/**
 * Calls function 'function' with zero parameters and returns the result. If AssertionException is
 * raised during the call of 'function', adds all the context 'errorContext' to the exception.
 */
template <typename F, class... Args>
auto addContextToAssertionException(F&& function, Args... errorContext) {
    try {
        return function();
    } catch (AssertionException& exception) {
        str::stream ss;
        ((ss << errorContext), ...);
        exception.addContext(ss);
        throw;
    }
}

/**
 * Converts 'value' to TimeUnit for an expression named 'expressionName'. It assumes that the
 * parameter is named "unit". Throws an AssertionException if 'value' contains an invalid value.
 */
TimeUnit parseTimeUnit(const Value& value, StringData expressionName);

/**
 * Converts 'value' to DayOfWeek for an expression named 'expressionName' with parameter named as
 * 'parameterName'. Throws an AssertionException if 'value' contains an invalid value.
 */
DayOfWeek parseDayOfWeek(const Value& value, StringData expressionName, StringData parameterName);

/**
 * Evaluates the expression in 'timeZone', and loads the corresponding TimeZone object from the
 * 'tzdb' database. Throws an AssertionException if 'timeZone' doesn't contain a string or if it is
 * not the name of a valid timezone. Returns boost::none if 'timeZone' is Null.
 */
boost::optional<TimeZone> makeTimeZone(const TimeZoneDatabase* tzdb,
                                       const Document& root,
                                       const Expression* timeZone,
                                       Variables* variables);

/**
 * Converts $dateTrunc expression parameter "binSize" 'value' to 64-bit integer.
 */
unsigned long long convertDateTruncBinSizeValue(const Value& value);

Value evaluate(const ExpressionDateFromParts& expr, const Document& root, Variables* variables);
Value evaluate(const ExpressionDateFromString& expr, const Document& root, Variables* variables);
Value evaluate(const ExpressionDateToParts& expr, const Document& root, Variables* variables);
Value evaluate(const ExpressionDateToString& expr, const Document& root, Variables* variables);
Value evaluate(const ExpressionDateDiff& expr, const Document& root, Variables* variables);
Value evaluate(const ExpressionDateAdd& expr, const Document& root, Variables* variables);
Value evaluate(const ExpressionDateSubtract& expr, const Document& root, Variables* variables);
Value evaluate(const ExpressionDateTrunc& expr, const Document& root, Variables* variables);
Value evaluate(const ExpressionTsSecond& expr, const Document& root, Variables* variables);
Value evaluate(const ExpressionTsIncrement& expr, const Document& root, Variables* variables);
Value evaluate(const ExpressionDayOfMonth& expr, const Document& root, Variables* variables);
Value evaluate(const ExpressionDayOfWeek& expr, const Document& root, Variables* variables);
Value evaluate(const ExpressionDayOfYear& expr, const Document& root, Variables* variables);
Value evaluate(const ExpressionHour& expr, const Document& root, Variables* variables);
Value evaluate(const ExpressionMillisecond& expr, const Document& root, Variables* variables);
Value evaluate(const ExpressionMinute& expr, const Document& root, Variables* variables);
Value evaluate(const ExpressionMonth& expr, const Document& root, Variables* variables);
Value evaluate(const ExpressionSecond& expr, const Document& root, Variables* variables);
Value evaluate(const ExpressionWeek& expr, const Document& root, Variables* variables);
Value evaluate(const ExpressionIsoWeekYear& expr, const Document& root, Variables* variables);
Value evaluate(const ExpressionIsoDayOfWeek& expr, const Document& root, Variables* variables);
Value evaluate(const ExpressionIsoWeek& expr, const Document& root, Variables* variables);
Value evaluate(const ExpressionYear& expr, const Document& root, Variables* variables);

}  // namespace exec::expression
}  // namespace mongo
