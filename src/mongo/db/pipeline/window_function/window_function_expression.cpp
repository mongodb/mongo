/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_set_window_fields.h"
#include "mongo/db/pipeline/document_source_set_window_fields_gen.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/query_feature_flags_gen.h"

#include "mongo/db/pipeline/window_function/window_function_expression.h"

using boost::intrusive_ptr;
using boost::optional;

namespace mongo::window_function {

REGISTER_WINDOW_FUNCTION(derivative, ExpressionDerivative::parse);

StringMap<Expression::Parser> Expression::parserMap;

intrusive_ptr<Expression> Expression::parse(BSONObj obj,
                                            const optional<SortPattern>& sortBy,
                                            ExpressionContext* expCtx) {
    boost::optional<Expression::Parser> parser;
    for (const auto& field : obj) {
        // Found one valid window function. If there are multiple window functions they will be
        // caught as invalid arguments to the Expression parser later.
        auto parser = parserMap.find(field.fieldNameStringData());
        if (parser != parserMap.end()) {
            return parser->second(obj, sortBy, expCtx);
        }
    }
    uasserted(ErrorCodes::FailedToParse, "Unrecognized window function");
}

void Expression::registerParser(std::string functionName, Parser parser) {
    invariant(parserMap.find(functionName) == parserMap.end());
    parserMap.emplace(std::move(functionName), std::move(parser));
}


boost::intrusive_ptr<Expression> ExpressionExpMovingAvg::parse(
    BSONObj obj, const boost::optional<SortPattern>& sortBy, ExpressionContext* expCtx) {
    // 'obj' is something like '{$expMovingAvg: {input: <arg>, <N/alpha>: <int/float>}}'
    boost::optional<StringData> accumulatorName;
    boost::intrusive_ptr<::mongo::Expression> input;
    uassert(ErrorCodes::FailedToParse,
            "$expMovingAvg must have exactly one argument that is an object",
            obj.nFields() == 1 && obj.hasField(kAccName) &&
                obj[kAccName].type() == BSONType::Object);
    auto subObj = obj[kAccName].embeddedObject();
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "$expMovingAvg sub object must have exactly two fields: An '"
                          << kInputArg << "' field, and either an '" << kNArg << "' field or an '"
                          << kAlphaArg << "' field",
            subObj.nFields() == 2 && subObj.hasField(kInputArg));
    input =
        ::mongo::Expression::parseOperand(expCtx, subObj[kInputArg], expCtx->variablesParseState);
    // ExpMovingAvg is always unbounded to current.
    WindowBounds bounds = WindowBounds{
        WindowBounds::DocumentBased{WindowBounds::Unbounded{}, WindowBounds::Current{}}};
    if (subObj.hasField(kNArg)) {
        auto nVal = subObj[kNArg];
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "'" << kNArg << "' field must be an integer, but found type "
                              << nVal.type(),
                nVal.isNumber());
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "'" << kNArg << "' field must be an integer, but found  " << nVal
                              << ". To use a non-integer, use the '" << kAlphaArg
                              << "' argument instead",
                nVal.safeNumberDouble() == floor(nVal.safeNumberDouble()));
        auto nNum = nVal.safeNumberLong();
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "'" << kNArg << "' must be greater than zero. Got " << nNum,
                nNum > 0);
        return make_intrusive<ExpressionExpMovingAvg>(
            expCtx, std::string(kAccName), std::move(input), std::move(bounds), nNum);
    } else if (subObj.hasField(kAlphaArg)) {
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "'" << kAlphaArg << "' must be a number",
                subObj[kAlphaArg].isNumber());
        return make_intrusive<ExpressionExpMovingAvg>(expCtx,
                                                      std::string(kAccName),
                                                      std::move(input),
                                                      std::move(bounds),
                                                      subObj[kAlphaArg].numberDecimal());
    } else {
        uasserted(ErrorCodes::FailedToParse,
                  str::stream() << "Got unrecognized field in $expMovingAvg"
                                << "$expMovingAvg sub object must have exactly two fields: An '"
                                << kInputArg << "' field, and either an '" << kNArg
                                << "' field or an '" << kAlphaArg << "' field");
    }
}

MONGO_INITIALIZER(windowFunctionExpressionMap)(InitializerContext*) {
    // Nothing to do. This initializer exists to tie together all the individual initializers
    // defined by REGISTER_WINDOW_FUNCTION and REGISTER_REMOVABLE_WINDOW_FUNCTION
}

}  // namespace mongo::window_function
