// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/window_function/window_function_count.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/window_function/window_bounds.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"
#include "mongo/db/pipeline/window_function/window_function_sum.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <string_view>
#include <utility>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::window_function {

boost::intrusive_ptr<window_function::Expression> parseCountWindowFunction(
    BSONObj obj, const boost::optional<SortPattern>& sortBy, ExpressionContext* expCtx) {
    // 'obj' should be something like '{$count: {}}, window: {...}}'.
    boost::optional<std::string_view> accumulatorName;
    WindowBounds bounds = WindowBounds::defaultBounds();

    for (const auto& arg : obj) {
        auto argName = arg.fieldNameStringData();
        if (argName == Expression::kWindowArg) {
            bounds = WindowBounds::parse(arg, sortBy, expCtx);
        } else if (Expression::isFunction(argName)) {
            uassert(ErrorCodes::FailedToParse,
                    "Cannot specify multiple functions in window function spec",
                    !accumulatorName);
            uassert(ErrorCodes::FailedToParse,
                    "$count only accepts an empty object as input",
                    arg.type() == BSONType::object && arg.Obj().isEmpty());

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
