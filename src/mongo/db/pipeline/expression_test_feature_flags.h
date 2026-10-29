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
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;
/**
 * The following expressions will be used to validate enabling feature flags on the latest or
 * lastLTS FCV and are only enabled in tests. The expressions are registered behind
 * 'gFeatureFlagBlender', enabled on the latest FCV and 'gFeatureFlagSpoon', enabled on the lastLTS.
 * These expressions are used to write permanent tests that enable feature flags.
 *
 * These expressions only take in and always return the integer 1.
 */
class ExpressionTestFeatureFlags : public Expression {
public:
    ExpressionTestFeatureFlags(ExpressionContext* const expCtx, std::string_view exprName)
        : Expression(expCtx), _exprName(exprName) {};

    Value evaluate(const Document& root,
                   Variables* variables,
                   const EvaluationContext& ctx) const final;

    Value serialize(const query_shape::SerializationOptions& options = {}) const final {
        return Value(Document{{_exprName, Value(1)}});
    }

protected:
    static void _validateInternal(const BSONElement& expr, std::string_view testExpressionName);

private:
    const std::string_view _exprName;
};

class ExpressionTestFeatureFlagLatest final : public ExpressionTestFeatureFlags {
public:
    static constexpr std::string_view kName = "$_testFeatureFlagLatest"sv;

    ExpressionTestFeatureFlagLatest(ExpressionContext* const expCtx)
        : ExpressionTestFeatureFlags(expCtx, kName) {};

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vpsIn);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    boost::intrusive_ptr<Expression> clone(ExpressionContext& expCtx) const final {
        return make_intrusive<ExpressionTestFeatureFlagLatest>(&expCtx);
    }
};

class ExpressionTestFeatureFlagLastLTS final : public ExpressionTestFeatureFlags {
public:
    static constexpr std::string_view kName = "$_testFeatureFlagLastLTS"sv;

    ExpressionTestFeatureFlagLastLTS(ExpressionContext* const expCtx)
        : ExpressionTestFeatureFlags(expCtx, kName) {};

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vpsIn);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    boost::intrusive_ptr<Expression> clone(ExpressionContext& expCtx) const final {
        return make_intrusive<ExpressionTestFeatureFlagLastLTS>(&expCtx);
    }
};
}  // namespace mongo
