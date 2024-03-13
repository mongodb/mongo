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

#include "window_function_shift.h"

#include <absl/container/node_hash_map.h>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

namespace mongo::window_function {
REGISTER_STABLE_WINDOW_FUNCTION(shift, ExpressionShift::parse);

boost::intrusive_ptr<Expression> ExpressionShift::parseShiftArgs(BSONObj obj,
                                                                 mongo::StringData accName,
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
        expCtx, accName.toString(), std::move(output), std::move(defaultVal), offset);
}

boost::intrusive_ptr<Expression> ExpressionShift::parse(BSONObj obj,
                                                        const boost::optional<SortPattern>& sortBy,
                                                        ExpressionContext* expCtx) {
    // 'obj' is something like '{$shift: {<args>}}'.
    boost::optional<StringData> accumulatorName;
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
                    arg.type() == BSONType::Object);
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

Value ExpressionShift::serialize(const SerializationOptions& opts) const {
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
