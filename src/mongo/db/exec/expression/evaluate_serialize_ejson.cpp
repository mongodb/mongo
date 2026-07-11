// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/db/exec/serialize_ejson_utils.h"

namespace mongo {

namespace exec::expression {

Value evaluate(const ExpressionSerializeEJSON& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    auto input = expr.getInput().evaluate(root, variables, ctx);
    auto relaxed =
        expr.getRelaxed() ? expr.getRelaxed()->evaluate(root, variables, ctx) : Value(true);
    uassert(ErrorCodes::BadValue,
            str::stream() << "Unexpected value for relaxed: " << relaxed.toString(),
            relaxed.getType() == BSONType::boolean);
    if (input.missing()) {
        return Value(BSONNULL);
    }
    try {
        return serialize_ejson_utils::serializeToExtendedJson(input, relaxed.getBool());
    } catch (const ExceptionFor<ErrorCodes::ConversionFailure>&) {
        if (expr.getOnError()) {
            return expr.getOnError()->evaluate(root, variables, ctx);
        }
        throw;
    }
}

Value evaluate(const ExpressionDeserializeEJSON& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    auto input = expr.getInput().evaluate(root, variables, ctx);
    if (input.missing()) {
        return Value(BSONNULL);
    }
    try {
        return serialize_ejson_utils::deserializeFromExtendedJson(input);
    } catch (const ExceptionFor<ErrorCodes::ConversionFailure>&) {
        if (expr.getOnError()) {
            return expr.getOnError()->evaluate(root, variables, ctx);
        }
        throw;
    }
}

}  // namespace exec::expression
}  // namespace mongo
