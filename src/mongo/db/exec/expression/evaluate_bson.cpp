/**
 *    Copyright (C) 2024-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "mongo/bson/bsonelement_comparator_interface.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/db/query/bson/multikey_dotted_path_support.h"

namespace mongo {

namespace exec::expression {

Value evaluate(const ExpressionObject& expr, const Document& root, Variables* variables) {
    MutableDocument outputDoc;
    auto& expressions = expr.getChildExpressions();
    for (auto&& pair : expressions) {
        outputDoc.addField(pair.first, pair.second->evaluate(root, variables));
    }
    return outputDoc.freezeToValue();
}

Value evaluate(const ExpressionBsonSize& expr, const Document& root, Variables* variables) {
    Value arg = expr.getChildren()[0]->evaluate(root, variables);

    if (arg.nullish()) {
        return Value(BSONNULL);
    }

    uassert(31393,
            str::stream() << "$bsonSize requires a document input, found: "
                          << typeName(arg.getType()),
            arg.getType() == BSONType::object);

    return Value(arg.getDocument().toBson().objsize());
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

Value evaluate(const ExpressionGetField& expr, const Document& root, Variables* variables) {
    auto fieldValue = expr.getField()->evaluate(root, variables);
    // If '_children[_kField]' is a constant expression, the parser guarantees that it evaluates to
    // a string. If it's a dynamic expression, its type can't be deduced during parsing.
    uassert(3041704,
            str::stream() << ExpressionGetField::kExpressionName
                          << " requires 'field' to evaluate to type String, "
                             "but got "
                          << typeName(fieldValue.getType()),
            fieldValue.getType() == BSONType::string);

    auto inputValue = expr.getInput()->evaluate(root, variables);
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

Value evaluate(const ExpressionSetField& expr, const Document& root, Variables* variables) {
    auto input = expr.getInput()->evaluate(root, variables);
    if (input.nullish()) {
        return Value(BSONNULL);
    }

    uassert(4161105,
            str::stream() << ExpressionSetField::kExpressionName
                          << " requires 'input' to evaluate to type Object",
            input.getType() == BSONType::object);

    auto value = expr.getValue()->evaluate(root, variables);

    // Build output document and modify 'field'.
    MutableDocument outputDoc(input.getDocument());
    outputDoc.setField(expr.getFieldName(), value);
    return outputDoc.freezeToValue();
}

Value evaluate(const ExpressionInternalFindAllValuesAtPath& expr,
               const Document& root,
               Variables* variables) {
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
