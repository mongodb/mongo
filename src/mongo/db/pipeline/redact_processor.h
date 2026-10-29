// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/util/modules.h"

#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>

namespace mongo {

/**
 * This class is used by the aggregation framework and streams enterprise module to perform the
 * document processing needed for $redact.
 */
class [[MONGO_MOD_PUBLIC]] RedactProcessor final {
public:
    RedactProcessor(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                    const boost::intrusive_ptr<Expression>& expression,
                    Variables::Id currentId);

    // Processes the given document and returns the redacted document. The optional 'ctx' parameter
    // carries evaluation state (see EvaluationContext); when it holds a memory tracker, memory
    // usage observed while evaluating the redact expression is accumulated against it.
    boost::optional<Document> process(const Document& input,
                                      const EvaluationContext& ctx = {}) const;

    boost::intrusive_ptr<Expression>& getExpression() {
        return _expression;
    }

    const boost::intrusive_ptr<Expression>& getExpression() const {
        return _expression;
    }

    void setExpression(boost::intrusive_ptr<Expression> expression) {
        _expression = std::move(expression);
    }

private:
    // These both work over pExpCtx->variables.
    boost::optional<Document> redactObject(const Document& root,
                                           const EvaluationContext& ctx) const;  // redacts CURRENT
    Value redactValue(const Value& in, const Document& root, const EvaluationContext& ctx) const;

    boost::intrusive_ptr<ExpressionContext> _expCtx;
    boost::intrusive_ptr<Expression> _expression;
    Variables::Id _currentId;
};

}  // namespace mongo
