/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/matcher/extensions_callback_real.h"

#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/matcher/expression_where.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_function.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/scripting/engine.h"

namespace mongo {

ExtensionsCallbackReal::ExtensionsCallbackReal(OperationContext* opCtx, const NamespaceString* nss)
    : _opCtx(opCtx), _nss(nss) {}

StatusWithMatchExpression ExtensionsCallbackReal::parseText(BSONElement text) const {
    auto textParams = extractTextMatchExpressionParams(text);
    if (!textParams.isOK()) {
        return textParams.getStatus();
    }

    auto exp =
        std::make_unique<TextMatchExpression>(_opCtx, *_nss, std::move(textParams.getValue()));

    return {std::move(exp)};
}

StatusWithMatchExpression ExtensionsCallbackReal::parseWhere(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, BSONElement where) const {
    auto whereParams = extractWhereMatchExpressionParams(where);
    if (!whereParams.isOK()) {
        return whereParams.getStatus();
    }

    if (getTestCommandsEnabled() && internalQueryDesugarWhereToFunction.load()) {
        uassert(ErrorCodes::BadValue, "ns for $where cannot be empty", expCtx->ns.db().size() != 0);

        auto code = whereParams.getValue().code;

        // Desugar $where to $expr. The $where function is invoked through a $function expression by
        // passing the document as $$CURRENT.
        auto fnExpression = ExpressionFunction::createForWhere(
            expCtx.get(),
            ExpressionArray::create(
                expCtx.get(),
                makeVector<boost::intrusive_ptr<Expression>>(ExpressionFieldPath::parse(
                    expCtx.get(), "$$CURRENT", expCtx->variablesParseState))),
            code,
            ExpressionFunction::kJavaScript);

        return {std::make_unique<ExprMatchExpression>(fnExpression, expCtx)};
    } else {
        expCtx->hasWhereClause = true;
        auto exp = std::make_unique<WhereMatchExpression>(
            _opCtx, std::move(whereParams.getValue()), expCtx->ns.db());
        return {std::move(exp)};
    }
}

}  // namespace mongo
