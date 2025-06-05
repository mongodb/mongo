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

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/expression/evaluate.h"

namespace mongo {

namespace exec::expression {

Value evaluate(const ExpressionAllElementsTrue& expr, const Document& root, Variables* variables) {
    const Value arr = expr.getChildren()[0]->evaluate(root, variables);
    uassert(17040,
            str::stream() << expr.getOpName() << "'s argument must be an array, but is "
                          << typeName(arr.getType()),
            arr.isArray());
    const std::vector<Value>& array = arr.getArray();
    for (auto it = array.begin(); it != array.end(); ++it) {
        if (!it->coerceToBool()) {
            return Value(false);
        }
    }
    return Value(true);
}

Value evaluate(const ExpressionAnyElementTrue& expr, const Document& root, Variables* variables) {
    const Value arr = expr.getChildren()[0]->evaluate(root, variables);
    uassert(17041,
            str::stream() << expr.getOpName() << "'s argument must be an array, but is "
                          << typeName(arr.getType()),
            arr.isArray());
    const std::vector<Value>& array = arr.getArray();
    for (auto it = array.begin(); it != array.end(); ++it) {
        if (it->coerceToBool()) {
            return Value(true);
        }
    }
    return Value(false);
}

Value evaluate(const ExpressionIn& expr, const Document& root, Variables* variables) {
    Value argument(expr.getChildren()[0]->evaluate(root, variables));
    Value arrayOfValues(expr.getChildren()[1]->evaluate(root, variables));

    uassert(40081,
            str::stream() << "$in requires an array as a second argument, found: "
                          << typeName(arrayOfValues.getType()),
            arrayOfValues.isArray());
    for (auto&& value : arrayOfValues.getArray()) {
        if (expr.getExpressionContext()->getValueComparator().evaluate(argument == value)) {
            return Value(true);
        }
    }
    return Value(false);
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
                        std::function<SafeNum(const SafeNum&, const SafeNum&)> bitwiseOp) {
    auto result = expr.getIdentity();
    for (auto&& child : expr.getChildren()) {
        Value val = child->evaluate(root, variables);
        if (val.nullish()) {
            return Value(BSONNULL);
        }
        auto valNum = uassertStatusOK(safeNumFromValue(val, expr.getOpName()));
        result = bitwiseOp(result, valNum);
    }
    return Value(result);
}
}  // namespace

Value evaluate(const ExpressionBitAnd& expr, const Document& root, Variables* variables) {
    auto op = [](const SafeNum& a, const SafeNum& b) -> SafeNum {
        return a.bitAnd(b);
    };
    return evaluateBitwiseOp(expr, root, variables, op);
}

Value evaluate(const ExpressionBitOr& expr, const Document& root, Variables* variables) {
    auto op = [](const SafeNum& a, const SafeNum& b) -> SafeNum {
        return a.bitOr(b);
    };
    return evaluateBitwiseOp(expr, root, variables, op);
}

Value evaluate(const ExpressionBitXor& expr, const Document& root, Variables* variables) {
    auto op = [](const SafeNum& a, const SafeNum& b) -> SafeNum {
        return a.bitXor(b);
    };
    return evaluateBitwiseOp(expr, root, variables, op);
}

}  // namespace exec::expression
}  // namespace mongo
