// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/expression_where_base.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

class OperationContext;

// This forward declaration is necessary because some translation units that include this header do
// not link with MozJS and must not transitively include js_function.h.
class JsFunction;

class WhereMatchExpression final : public WhereMatchExpressionBase {
public:
    WhereMatchExpression(OperationContext* opCtx, WhereParams params, const DatabaseName& dbName);

    // Out-of-line destructor required because _jsFunction holds an incomplete JsFunction type.
    ~WhereMatchExpression() final;

    std::unique_ptr<MatchExpression> clone() const final;

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    const JsFunction& getPredicate() const;
    std::unique_ptr<JsFunction> extractPredicate();
    void setPredicate(std::unique_ptr<JsFunction> jsFunction);
    void validateState() const;

    bool runPredicate(const BSONObj& doc) const final;

private:
    OperationContext* const _opCtx;
    std::unique_ptr<JsFunction> _jsFunction;
};

}  // namespace mongo
