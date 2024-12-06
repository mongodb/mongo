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

Value evaluate(const ExpressionAnd& expr, const Document& root, Variables* variables) {
    const auto& children = expr.getChildren();
    const size_t n = children.size();
    for (size_t i = 0; i < n; ++i) {
        Value pValue(children[i]->evaluate(root, variables));
        if (!pValue.coerceToBool()) {
            return Value(false);
        }
    }

    return Value(true);
}

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

Value evaluate(const ExpressionCoerceToBool& expr, const Document& root, Variables* variables) {
    Value pResult(expr.getExpression()->evaluate(root, variables));
    bool b = pResult.coerceToBool();
    if (b) {
        return Value(true);
    }
    return Value(false);
}

namespace {
// Lookup table for truth value returns
struct CmpLookup {
    const bool truthValue[3];  // truth value for -1, 0, 1
};
static const CmpLookup cmpLookup[6] = {
    /*             -1      0      1      */
    /* EQ  */ {{false, true, false}},
    /* NE  */ {{true, false, true}},
    /* GT  */ {{false, false, true}},
    /* GTE */ {{false, true, true}},
    /* LT  */ {{true, false, false}},
    /* LTE */ {{true, true, false}},

    // We don't require the lookup table for CMP.
};
}  // namespace

Value evaluate(const ExpressionCompare& expr, const Document& root, Variables* variables) {
    Value pLeft(expr.getChildren()[0]->evaluate(root, variables));
    Value pRight(expr.getChildren()[1]->evaluate(root, variables));

    int cmp = expr.getExpressionContext()->getValueComparator().compare(pLeft, pRight);

    // Make cmp one of 1, 0, or -1.
    if (cmp == 0) {
        // leave as 0
    } else if (cmp < 0) {
        cmp = -1;
    } else if (cmp > 0) {
        cmp = 1;
    }

    if (expr.getOp() == ExpressionCompare::CmpOp::CMP) {
        return Value(cmp);
    }

    bool returnValue = cmpLookup[expr.getOp()].truthValue[cmp + 1];
    return Value(returnValue);
}

Value evaluate(const ExpressionCond& expr, const Document& root, Variables* variables) {
    Value pCond(expr.getChildren()[0]->evaluate(root, variables));
    int idx = pCond.coerceToBool() ? 1 : 2;
    return expr.getChildren()[idx]->evaluate(root, variables);
}

Value evaluate(const ExpressionIfNull& expr, const Document& root, Variables* variables) {
    const size_t n = expr.getChildren().size();
    for (size_t i = 0; i < n; ++i) {
        Value pValue(expr.getChildren()[i]->evaluate(root, variables));
        if (!pValue.nullish() || i == n - 1) {
            return pValue;
        }
    }
    return Value();
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

Value evaluate(const ExpressionNot& expr, const Document& root, Variables* variables) {
    Value pOp(expr.getChildren()[0]->evaluate(root, variables));

    bool b = pOp.coerceToBool();
    return Value(!b);
}

Value evaluate(const ExpressionOr& expr, const Document& root, Variables* variables) {
    for (size_t i = 0; i < expr.getChildren().size(); ++i) {
        Value pValue(expr.getChildren()[i]->evaluate(root, variables));
        if (pValue.coerceToBool()) {
            return Value(true);
        }
    }

    return Value(false);
}

Value evaluate(const ExpressionSwitch& expr, const Document& root, Variables* variables) {
    for (int i = 0; i < expr.numBranches(); ++i) {
        auto [caseExpr, thenExpr] = expr.getBranch(i);
        Value caseResult = caseExpr->evaluate(root, variables);

        if (caseResult.coerceToBool()) {
            return thenExpr->evaluate(root, variables);
        }
    }

    uassert(40066,
            "$switch could not find a matching branch for an input, and no default was specified.",
            expr.defaultExpr());

    return expr.defaultExpr()->evaluate(root, variables);
}

namespace {
StatusWith<SafeNum> safeNumFromValue(const Value& val, const char* opName) {
    switch (val.getType()) {
        case NumberInt:
            return val.getInt();
        case NumberLong:
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
