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

#include "mongo/db/pipeline/window_function_expression.h"

using boost::intrusive_ptr;
using boost::optional;

namespace mongo::window_function {

StringMap<Expression::Parser> Expression::parserMap;

intrusive_ptr<Expression> Expression::parse(BSONElement elem,
                                            const optional<SortPattern>& sortBy,
                                            ExpressionContext* expCtx) {
    auto parser = parserMap.find(elem.fieldNameStringData());
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "No such window function: " << elem.fieldName(),
            parser != parserMap.end());
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "Window function " << elem.fieldName() << " requires an object.",
            elem.type() == BSONType::Object);
    return parser->second(elem, sortBy, expCtx);
}

void Expression::registerParser(std::string functionName, Parser parser) {
    invariant(parserMap.find(functionName) == parserMap.end());
    parserMap.emplace(std::move(functionName), std::move(parser));
}

MONGO_INITIALIZER(windowFunctionExpressionMap)(InitializerContext*) {
    // Nothing to do. This initializer exists to tie together all the individual initializers
    // defined by REGISTER_WINDOW_FUNCTION.
}

}  // namespace mongo::window_function
