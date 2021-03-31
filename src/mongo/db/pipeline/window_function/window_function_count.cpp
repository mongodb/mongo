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

#include "mongo/db/pipeline/window_function/window_function_count.h"
#include "mongo/db/pipeline/window_function/window_function_sum.h"

namespace mongo::window_function {

boost::intrusive_ptr<window_function::Expression> parseCountWindowFunction(
    BSONObj obj, const boost::optional<SortPattern>& sortBy, ExpressionContext* expCtx) {
    // 'obj' should be something like '{$count: {}}, window: {...}}'.
    boost::optional<StringData> accumulatorName;
    WindowBounds bounds = WindowBounds::defaultBounds();

    for (const auto& arg : obj) {
        auto argName = arg.fieldNameStringData();
        if (argName == Expression::kWindowArg) {
            uassert(ErrorCodes::FailedToParse,
                    "'window' field must be an object",
                    arg.type() == BSONType::Object);

            bounds = WindowBounds::parse(arg.embeddedObject(), sortBy, expCtx);
        } else if (Expression::isFunction(argName)) {
            uassert(ErrorCodes::FailedToParse,
                    "Cannot specify multiple functions in window function spec",
                    !accumulatorName);
            uassert(ErrorCodes::FailedToParse,
                    "$count only accepts an empty object as input",
                    arg.type() == BSONType::Object && arg.Obj().isEmpty());

            accumulatorName = argName;
        } else {
            uasserted(ErrorCodes::FailedToParse,
                      str::stream() << "Window function found an unknown argument: " << argName);
        }
    }
    uassert(ErrorCodes::FailedToParse,
            "Must specify a window function in output field",
            accumulatorName);

    // Returns an expression representing "{ $sum: 1, window: bounds }".
    return make_intrusive<ExpressionRemovable<AccumulatorSum, WindowFunctionSum>>(
        expCtx, "$sum", ExpressionConstant::create(expCtx, Value(1)), std::move(bounds));
}

}  // namespace mongo::window_function
