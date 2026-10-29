// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/db/exec/projection_executor_utils.h"

namespace mongo {

namespace exec::expression {

Value evaluate(const ExpressionInternalFindPositional& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    auto& children = expr.getChildren();

    auto preImage = children[0]->evaluate(root, variables, ctx);
    auto postImage = children[1]->evaluate(root, variables, ctx);
    uassert(51255,
            fmt::format("Positional operator pre-image can only be an object, but got {}",
                        typeName(preImage.getType())),
            preImage.getType() == BSONType::object);
    uassert(51258,
            fmt::format("Positional operator post-image can only be an object, but got {}",
                        typeName(postImage.getType())),
            postImage.getType() == BSONType::object);
    return Value{projection_executor_utils::applyFindPositionalProjection(preImage.getDocument(),
                                                                          postImage.getDocument(),
                                                                          *expr.getMatchExpr(),
                                                                          expr.getFieldPath())};
}

Value evaluate(const ExpressionInternalFindSlice& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    auto postImage = expr.getChildren()[0]->evaluate(root, variables, ctx);
    uassert(51256,
            fmt::format("$slice operator can only be applied to an object, but got {}",
                        typeName(postImage.getType())),
            postImage.getType() == BSONType::object);
    return Value{projection_executor_utils::applyFindSliceProjection(
        postImage.getDocument(), expr.getFieldPath(), expr.getSkip(), expr.getLimit())};
}

Value evaluate(const ExpressionInternalFindElemMatch& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    auto input = expr.getChildren()[0]->evaluate(root, variables, ctx);
    tassert(
        11103501,
        fmt::format("Expected the input to be an object, but got {}", typeName(input.getType())),
        input.getType() == BSONType::object);
    return projection_executor_utils::applyFindElemMatchProjection(
        input.getDocument(), *expr.getMatchExpr(), expr.getFieldPath());
}

}  // namespace exec::expression
}  // namespace mongo
