// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/window_function/window_function_shift.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::window_function {
REGISTER_STABLE_WINDOW_FUNCTION(shift, ExpressionShift::parse);

boost::intrusive_ptr<Expression> ExpressionShift::parseShiftArgs(BSONObj obj,
                                                                 std::string_view accName,
                                                                 ExpressionContext* expCtx) {
    // 'obj' is something like '{output: EXPR, by: INT, default: CONSTEXPR}'.
    // only default is optional.
    boost::optional<::mongo::Value> defaultVal;
    boost::intrusive_ptr<::mongo::Expression> output;
    int offset;

    bool offsetFound = false;
    for (const auto& arg : obj) {
        auto argName = arg.fieldNameStringData();

        if (kOutputArg == argName) {
            output = ::mongo::Expression::parseOperand(expCtx, arg, expCtx->variablesParseState);
        } else if (kDefaultArg == argName) {
            auto defaultExp =
                ::mongo::Expression::parseOperand(expCtx, arg, expCtx->variablesParseState);
            defaultExp = defaultExp->optimize();
            ExpressionConstant* ec = dynamic_cast<ExpressionConstant*>(defaultExp.get());
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "'$shift:" << kDefaultArg
                                  << "' expression must yield a constant value.",
                    ec);
            defaultVal = ec->getValue();
        } else if (kByArg == argName) {
            auto parsedOffset = arg.parseIntegerElementToInt();
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "'$shift:" << kByArg
                                  << "' field must be an integer, but found  " << arg,
                    parsedOffset.isOK());
            offset = parsedOffset.getValue();
            offsetFound = true;
        } else {
            uasserted(ErrorCodes::FailedToParse,
                      str::stream() << "Unknown argument in " << accName);
        }
    }

    uassert(ErrorCodes::FailedToParse,
            str::stream() << accName << " requires an '" << kOutputArg << "' expression.",
            output);

    uassert(ErrorCodes::FailedToParse,
            str::stream() << accName << " requires '" << kByArg << "' as an integer value.",
            offsetFound);

    return make_intrusive<ExpressionShift>(
        expCtx, std::string{accName}, std::move(output), std::move(defaultVal), offset);
}

boost::intrusive_ptr<Expression> ExpressionShift::parse(BSONObj obj,
                                                        const boost::optional<SortPattern>& sortBy,
                                                        ExpressionContext* expCtx) {
    // 'obj' is something like '{$shift: {<args>}}'.
    boost::optional<std::string_view> accumulatorName;
    boost::intrusive_ptr<Expression> shiftExpr;

    for (const auto& arg : obj) {
        auto argName = arg.fieldNameStringData();
        if (argName == kWindowArg) {
            uasserted(ErrorCodes::FailedToParse,
                      str::stream() << "$shift does not accept a '" << kWindowArg << "' field");
        } else if (parserMap.find(argName) != parserMap.end()) {
            uassert(ErrorCodes::FailedToParse,
                    "Cannot specify multiple functions in window function spec",
                    !accumulatorName);
            accumulatorName = argName;
            uassert(ErrorCodes::FailedToParse,
                    "Argument to $shift must be an object",
                    arg.type() == BSONType::object);
            shiftExpr = parseShiftArgs(arg.Obj(), argName, expCtx);
        } else {
            uasserted(ErrorCodes::FailedToParse,
                      str::stream() << "Window function found an unknown argument: " << argName);
        }
    }

    uassert(ErrorCodes::FailedToParse,
            str::stream() << "'" << accumulatorName << "' requires a sortBy",
            sortBy);

    return shiftExpr;
}

Value ExpressionShift::serialize(const query_shape::SerializationOptions& opts) const {
    MutableDocument args;
    args.addField(kByArg, opts.serializeLiteral(_offset));
    args.addField(kOutputArg, _input->serialize(opts));
    args.addField(kDefaultArg,
                  opts.serializeLiteral(_defaultVal.get_value_or(mongo::Value(BSONNULL))));
    MutableDocument windowFun;
    windowFun.addField(_accumulatorName, args.freezeToValue());
    return windowFun.freezeToValue();
}

}  // namespace mongo::window_function
