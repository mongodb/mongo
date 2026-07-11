// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/bson/bsonelement_comparator_interface.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/query/bson/multikey_dotted_path_support.h"

namespace mongo {

namespace exec::expression {

Value evaluate(const ExpressionObject& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    auto& expressions = expr.getChildExpressions();
    MutableDocument outputDoc(expressions.size());

    auto& tracker = getMemoryTracker(expr, ctx);
    SimpleMemoryUsageToken memToken(0, &tracker);

    for (auto&& pair : expressions) {
        Value fieldVal = pair.second->evaluate(root, variables, ctx);

        // Account for the evaluated value plus the field name
        memToken.add(static_cast<int64_t>(pair.first.size() + 1 + fieldVal.getApproximateSize()));
        tracker.assertWithinMemoryLimit(expr.getOpName(), ctx.stageName);

        outputDoc.addField(pair.first, std::move(fieldVal));
    }
    return outputDoc.freezeToValue();
}

Value evaluate(const ExpressionBsonSize& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    Value arg = expr.getChildren()[0]->evaluate(root, variables, ctx);

    if (arg.nullish()) {
        return Value(BSONNULL);
    }

    uassert(31393,
            str::stream() << "$bsonSize requires a document input, found: "
                          << typeName(arg.getType()),
            arg.getType() == BSONType::object);

    return Value(arg.getDocument().toBson<BSONObj::LargeSizeTrait>().objsize());
}

Value evaluatePath(const FieldPath& fieldPath, size_t index, const Document& input) {
    // Note this function is very hot so it is important that is is well optimized.
    // In particular, all return paths should support RVO.

    /* if we've hit the end of the path, stop */
    if (index == fieldPath.getPathLength() - 1) {
        return input[fieldPath.getFieldNameHashed(index)];
    }

    // Try to dive deeper
    const Value val = input[fieldPath.getFieldNameHashed(index)];
    switch (val.getType()) {
        case BSONType::object:
            return evaluatePath(fieldPath, index + 1, val.getDocument());

        case BSONType::array:
            return evaluatePathArray(fieldPath, index + 1, val);

        default:
            return Value();
    }
}

Value evaluatePathArray(const FieldPath& fieldPath, size_t index, const Value& input) {
    dassert(input.isArray());

    // Check for remaining path in each element of array
    std::vector<Value> result;
    const std::vector<Value>& array = input.getArray();
    for (size_t i = 0; i < array.size(); i++) {
        if (array[i].getType() != BSONType::object) {
            continue;
        }

        const Value nested = evaluatePath(fieldPath, index, array[i].getDocument());
        if (!nested.missing()) {
            result.push_back(nested);
        }
    }

    return Value(std::move(result));
}

Value evaluate(const ExpressionGetField& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    auto fieldValue = expr.getField()->evaluate(root, variables, ctx);
    // If '_children[_kField]' is a constant expression, the parser guarantees that it evaluates to
    // a string. If it's a dynamic expression, its type can't be deduced during parsing.
    uassert(3041704,
            str::stream() << ExpressionGetField::kExpressionName
                          << " requires 'field' to evaluate to type String, "
                             "but got "
                          << typeName(fieldValue.getType()),
            fieldValue.getType() == BSONType::string);

    auto inputValue = expr.getInput()->evaluate(root, variables, ctx);
    if (inputValue.nullish()) {
        if (inputValue.missing()) {
            return Value();
        } else {
            return Value(BSONNULL);
        }
    } else if (inputValue.getType() != BSONType::object) {
        return Value();
    }

    return inputValue.getDocument().getField(fieldValue.getStringData());
}

Value evaluate(const ExpressionSetField& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    auto input = expr.getInput()->evaluate(root, variables, ctx);
    if (input.nullish()) {
        return Value(BSONNULL);
    }

    uassert(4161105,
            str::stream() << ExpressionSetField::kExpressionName
                          << " requires 'input' to evaluate to type Object",
            input.getType() == BSONType::object);

    auto value = expr.getValue()->evaluate(root, variables, ctx);

    // Build output document and modify 'field'.
    MutableDocument outputDoc(input.getDocument());
    outputDoc.setField(expr.getFieldName(), value);
    return outputDoc.freezeToValue();
}

Value evaluate(const ExpressionInternalFindAllValuesAtPath& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    BSONElementSet elts(expr.getExpressionContext()->getCollator());
    auto bsonRoot = root.toBson();
    multikey_dotted_path_support::extractAllElementsAlongPath(
        bsonRoot, expr.getFieldPath().fullPath(), elts);
    std::vector<Value> outputVals;
    for (BSONElementSet::iterator it = elts.begin(); it != elts.end(); ++it) {
        BSONElement elt = *it;
        outputVals.push_back(Value(elt));
    }

    return Value(std::move(outputVals));
}

}  // namespace exec::expression
}  // namespace mongo
