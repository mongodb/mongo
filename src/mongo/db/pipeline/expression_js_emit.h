// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

#include <string>
#include <utility>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * This expression takes in a JavaScript function and a "this" reference, and returns an array of
 * key/value objects which are the results of calling emit() from the provided JS function.
 */
class ExpressionInternalJsEmit final : public Expression {
public:
    static constexpr auto kExpressionName = "$_internalJsEmit"sv;

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);

    static boost::intrusive_ptr<ExpressionInternalJsEmit> create(
        ExpressionContext* const expCtx,
        boost::intrusive_ptr<Expression> thisRef,
        std::string funcSourceString) {
        return new ExpressionInternalJsEmit{expCtx, thisRef, std::move(funcSourceString)};
    }

    Value evaluate(const Document& root,
                   Variables* variables,
                   const EvaluationContext& ctx) const final;

    Value serialize(const query_shape::SerializationOptions& options) const final;

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    const Expression* getThisRef() const {
        return _thisRef.get();
    }

    const std::string& getFuncSource() const {
        return _funcSource;
    }

    boost::intrusive_ptr<Expression> clone(ExpressionContext& expCtx) const final {
        return ExpressionInternalJsEmit::create(&expCtx, cloneChild(0, expCtx), _funcSource);
    }

private:
    ExpressionInternalJsEmit(ExpressionContext* expCtx,
                             boost::intrusive_ptr<Expression> thisRef,
                             std::string funcSourceString);

    const boost::intrusive_ptr<Expression>& _thisRef;
    std::string _funcSource;
};
}  // namespace mongo
