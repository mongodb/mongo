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

#pragma once

#include "mongo/platform/basic.h"

#include "mongo/db/cst/bson_lexer.h"
#include "mongo/db/cst/c_node.h"
#include "mongo/db/cst/cst_match_translation.h"
#include "mongo/db/cst/cst_sort_translation.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/extensions_callback.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/sort_pattern.h"

namespace mongo::cst {

/**
 * Parses the given 'filter' to a MatchExpression. Throws an exception if the filter fails to parse.
 */
std::unique_ptr<MatchExpression> parseToMatchExpression(
    BSONObj filter,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ExtensionsCallback& extensionsCallback) {
    BSONLexer lexer{filter, ParserGen::token::START_MATCH};
    CNode cst;
    ParserGen(lexer, &cst).parse();
    return cst_match_translation::translateMatchExpression(cst, expCtx, extensionsCallback);
}

/**
 * Parses the given 'sort' object into a SortPattern. Throws an exception if the sort object fails
 * to parse.
 */
SortPattern parseToSortPattern(BSONObj sort,
                               const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    BSONLexer lexer{sort, ParserGen::token::START_SORT};
    CNode cst;
    ParserGen(lexer, &cst).parse();
    return cst_sort_translation::translateSortSpec(cst, expCtx);
}

}  // namespace mongo::cst
