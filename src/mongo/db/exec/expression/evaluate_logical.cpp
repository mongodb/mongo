// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/expression/evaluate.h"

namespace mongo {

namespace exec::expression {

namespace {
template <typename Predicate>
bool anyWithInterrupt(const std::vector<Value>& values,
                      OperationContext* opCtx,
                      Predicate predicate) {
    // Keep interrupt accounting and its branch outside the cheap per-element loop.
    constexpr size_t kInterval = 1024;
    for (size_t start = 0; start < values.size();) {
        if (opCtx) {
            opCtx->checkForInterrupt();
        }
        const auto count = std::min(kInterval, values.size() - start);
        const auto end = start + count;
        for (; start < end; ++start) {
            if (predicate(values[start])) {
                return true;
            }
        }
    }
    return false;
}
}  // namespace

Value evaluate(const ExpressionAllElementsTrue& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    const Value arr = expr.getChildren()[0]->evaluate(root, variables, ctx);
    uassert(17040,
            str::stream() << expr.getOpName() << "'s argument must be an array, but is "
                          << typeName(arr.getType()),
            arr.isArray());
    const std::vector<Value>& array = arr.getArray();
    return Value(!anyWithInterrupt(array,
                                   expr.getExpressionContext()->getOperationContext(),
                                   [](const Value& value) { return !value.coerceToBool(); }));
}

Value evaluate(const ExpressionAnyElementTrue& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    const Value arr = expr.getChildren()[0]->evaluate(root, variables, ctx);
    uassert(17041,
            str::stream() << expr.getOpName() << "'s argument must be an array, but is "
                          << typeName(arr.getType()),
            arr.isArray());
    const std::vector<Value>& array = arr.getArray();
    return Value(anyWithInterrupt(array,
                                  expr.getExpressionContext()->getOperationContext(),
                                  [](const Value& value) { return value.coerceToBool(); }));
}

Value evaluate(const ExpressionIn& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    Value argument(expr.getChildren()[0]->evaluate(root, variables, ctx));
    Value arrayOfValues(expr.getChildren()[1]->evaluate(root, variables, ctx));

    uassert(40081,
            str::stream() << "$in requires an array as a second argument, found: "
                          << typeName(arrayOfValues.getType()),
            arrayOfValues.isArray());
    return Value(anyWithInterrupt(
        arrayOfValues.getArray(),
        expr.getExpressionContext()->getOperationContext(),
        [&](const Value& value) {
            return expr.getExpressionContext()->getValueComparator().evaluate(argument == value);
        }));
}

namespace {
StatusWith<SafeNum> safeNumFromValue(const Value& val, const char* opName) {
    switch (val.getType()) {
        case BSONType::numberInt:
            return val.getInt();
        case BSONType::numberLong:
            return (int64_t)val.getLong();
        default:
            return Status(ErrorCodes::TypeMismatch,
                          str::stream() << opName << " only supports int and long operands.");
    }
}

template <typename ExpressionBitwiseOp>
Value evaluateBitwiseOp(ExpressionBitwiseOp& expr,
                        const Document& root,
                        Variables* variables,
                        const EvaluationContext& ctx,
                        std::function<SafeNum(const SafeNum&, const SafeNum&)> bitwiseOp) {
    auto result = expr.getIdentity();
    for (auto&& child : expr.getChildren()) {
        Value val = child->evaluate(root, variables, ctx);
        if (val.nullish()) {
            return Value(BSONNULL);
        }
        auto valNum = uassertStatusOK(safeNumFromValue(val, expr.getOpName()));
        result = bitwiseOp(result, valNum);
    }
    return Value(result);
}
}  // namespace

Value evaluate(const ExpressionBitAnd& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    auto op = [](const SafeNum& a, const SafeNum& b) -> SafeNum {
        return a.bitAnd(b);
    };
    return evaluateBitwiseOp(expr, root, variables, ctx, op);
}

Value evaluate(const ExpressionBitOr& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    auto op = [](const SafeNum& a, const SafeNum& b) -> SafeNum {
        return a.bitOr(b);
    };
    return evaluateBitwiseOp(expr, root, variables, ctx, op);
}

Value evaluate(const ExpressionBitXor& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    auto op = [](const SafeNum& a, const SafeNum& b) -> SafeNum {
        return a.bitXor(b);
    };
    return evaluateBitwiseOp(expr, root, variables, ctx, op);
}

}  // namespace exec::expression
}  // namespace mongo
