// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/expression_trigonometric.h"

#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/db/pipeline/expression.h"

namespace mongo {

// The parse methods of the expressions are registered here in the .cpp file to prevent multiple
// definition errors.

/**
 * Inclusive Bounds
 */
REGISTER_STABLE_EXPRESSION(acos, ExpressionArcCosine::parse);
REGISTER_STABLE_EXPRESSION(asin, ExpressionArcSine::parse);
REGISTER_STABLE_EXPRESSION(atanh, ExpressionHyperbolicArcTangent::parse);
REGISTER_STABLE_EXPRESSION(acosh, ExpressionHyperbolicArcCosine::parse);

/**
 * Exclusive Bounds
 */
REGISTER_STABLE_EXPRESSION(cos, ExpressionCosine::parse);
REGISTER_STABLE_EXPRESSION(sin, ExpressionSine::parse);
REGISTER_STABLE_EXPRESSION(tan, ExpressionTangent::parse);

/**
 * Unbounded
 */
REGISTER_STABLE_EXPRESSION(atan, ExpressionArcTangent::parse);
REGISTER_STABLE_EXPRESSION(asinh, ExpressionHyperbolicArcSine::parse);
REGISTER_STABLE_EXPRESSION(cosh, ExpressionHyperbolicCosine::parse);
REGISTER_STABLE_EXPRESSION(sinh, ExpressionHyperbolicSine::parse);
REGISTER_STABLE_EXPRESSION(tanh, ExpressionHyperbolicTangent::parse);

REGISTER_STABLE_EXPRESSION(atan2, ExpressionArcTangent2::parse);

REGISTER_STABLE_EXPRESSION(degreesToRadians, ExpressionDegreesToRadians::parse);
REGISTER_STABLE_EXPRESSION(radiansToDegrees, ExpressionRadiansToDegrees::parse);

Value ExpressionArcCosine::evaluate(const Document& root,
                                    Variables* variables,
                                    const EvaluationContext& ctx) const {
    return exec::expression::evaluate(*this, root, variables, ctx);
}

Value ExpressionArcSine::evaluate(const Document& root,
                                  Variables* variables,
                                  const EvaluationContext& ctx) const {
    return exec::expression::evaluate(*this, root, variables, ctx);
}

Value ExpressionHyperbolicArcTangent::evaluate(const Document& root,
                                               Variables* variables,
                                               const EvaluationContext& ctx) const {
    return exec::expression::evaluate(*this, root, variables, ctx);
}

Value ExpressionHyperbolicArcCosine::evaluate(const Document& root,
                                              Variables* variables,
                                              const EvaluationContext& ctx) const {
    return exec::expression::evaluate(*this, root, variables, ctx);
}

Value ExpressionCosine::evaluate(const Document& root,
                                 Variables* variables,
                                 const EvaluationContext& ctx) const {
    return exec::expression::evaluate(*this, root, variables, ctx);
}

Value ExpressionSine::evaluate(const Document& root,
                               Variables* variables,
                               const EvaluationContext& ctx) const {
    return exec::expression::evaluate(*this, root, variables, ctx);
}

Value ExpressionTangent::evaluate(const Document& root,
                                  Variables* variables,
                                  const EvaluationContext& ctx) const {
    return exec::expression::evaluate(*this, root, variables, ctx);
}

Value ExpressionArcTangent::evaluate(const Document& root,
                                     Variables* variables,
                                     const EvaluationContext& ctx) const {
    return exec::expression::evaluate(*this, root, variables, ctx);
}

Value ExpressionHyperbolicArcSine::evaluate(const Document& root,
                                            Variables* variables,
                                            const EvaluationContext& ctx) const {
    return exec::expression::evaluate(*this, root, variables, ctx);
}

Value ExpressionHyperbolicCosine::evaluate(const Document& root,
                                           Variables* variables,
                                           const EvaluationContext& ctx) const {
    return exec::expression::evaluate(*this, root, variables, ctx);
}

Value ExpressionHyperbolicSine::evaluate(const Document& root,
                                         Variables* variables,
                                         const EvaluationContext& ctx) const {
    return exec::expression::evaluate(*this, root, variables, ctx);
}

Value ExpressionHyperbolicTangent::evaluate(const Document& root,
                                            Variables* variables,
                                            const EvaluationContext& ctx) const {
    return exec::expression::evaluate(*this, root, variables, ctx);
}

Value ExpressionArcTangent2::evaluate(const Document& root,
                                      Variables* variables,
                                      const EvaluationContext& ctx) const {
    return exec::expression::evaluate(*this, root, variables, ctx);
}

Value ExpressionDegreesToRadians::evaluate(const Document& root,
                                           Variables* variables,
                                           const EvaluationContext& ctx) const {
    return exec::expression::evaluate(*this, root, variables, ctx);
}

Value ExpressionRadiansToDegrees::evaluate(const Document& root,
                                           Variables* variables,
                                           const EvaluationContext& ctx) const {
    return exec::expression::evaluate(*this, root, variables, ctx);
}

}  // namespace mongo
