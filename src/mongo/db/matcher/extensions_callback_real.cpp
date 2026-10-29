// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/extensions_callback_real.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/matcher/expression_where.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_function.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

ExtensionsCallbackReal::ExtensionsCallbackReal(OperationContext* opCtx, const NamespaceString* nss)
    : _opCtx(opCtx), _nss(nss) {}

std::unique_ptr<MatchExpression> ExtensionsCallbackReal::createText(
    TextMatchExpressionBase::TextParams text) const {
    return std::make_unique<TextMatchExpression>(_opCtx, *_nss, std::move(text));
}

std::unique_ptr<MatchExpression> ExtensionsCallbackReal::createWhere(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    WhereMatchExpressionBase::WhereParams where) const {
    if (getTestCommandsEnabled() && internalQueryDesugarWhereToFunction.load()) {
        uassert(ErrorCodes::BadValue,
                "ns for $where cannot be empty",
                expCtx->getNamespaceString().dbSize() != 0);

        auto code = where.code;

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

        return std::make_unique<ExprMatchExpression>(fnExpression, expCtx);
    } else {
        expCtx->setHasWhereClause(true);
        return std::make_unique<WhereMatchExpression>(
            _opCtx, std::move(where), expCtx->getNamespaceString().dbName());
    }
}

}  // namespace mongo
