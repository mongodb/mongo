/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/values/value.h"

namespace mongo::sbe_helper {

template <typename ExpressionType>
using ValueExpressionFn = std::function<ExpressionType(sbe::value::TypeTags, sbe::value::Value)>;

/*
 * Converts SBE comparison operation and value to an expression.
 */
template <typename BuilderType, typename ExpressionType>
ExpressionType generateComparisonExpr(BuilderType& b,
                                      sbe::value::TypeTags tag,
                                      sbe::value::Value val,
                                      sbe::EPrimBinary::Op binaryOp,
                                      ExpressionType inputExpr,
                                      ValueExpressionFn<ExpressionType> makeValExpr) {
    // Most commonly the comparison does not do any kind of type conversions (i.e. 12 > "10" does
    // not evaluate to true as we do not try to convert a string to a number). Internally, SBE
    // returns Nothing for mismatched types. However, there is a wrinkle with MQL (and there always
    // is one). We can compare any type to MinKey or MaxKey type and expect a true/false answer.
    if (tag == sbe::value::TypeTags::MinKey) {
        switch (binaryOp) {
            case sbe::EPrimBinary::eq:
            case sbe::EPrimBinary::neq:
                break;
            case sbe::EPrimBinary::greater:
                return b.makeFillEmptyFalse(
                    b.makeNot(b.makeFunction("isMinKey", std::move(inputExpr))));
            case sbe::EPrimBinary::greaterEq:
                return b.makeFunction("exists", std::move(inputExpr));
            case sbe::EPrimBinary::less:
                return b.makeBoolConstant(false);
            case sbe::EPrimBinary::lessEq:
                return b.makeFillEmptyFalse(b.makeFunction("isMinKey", std::move(inputExpr)));
            default:
                MONGO_UNREACHABLE_TASSERT(8217105);
        }
    } else if (tag == sbe::value::TypeTags::MaxKey) {
        switch (binaryOp) {
            case sbe::EPrimBinary::eq:
            case sbe::EPrimBinary::neq:
                break;
            case sbe::EPrimBinary::greater:
                return b.makeBoolConstant(false);
            case sbe::EPrimBinary::greaterEq:
                return b.makeFillEmptyFalse(b.makeFunction("isMaxKey", std::move(inputExpr)));
            case sbe::EPrimBinary::less:
                return b.makeFillEmptyFalse(
                    b.makeNot(b.makeFunction("isMaxKey", std::move(inputExpr))));
            case sbe::EPrimBinary::lessEq:
                return b.makeFunction("exists", std::move(inputExpr));
            default:
                MONGO_UNREACHABLE_TASSERT(8217101);
        }
    } else if (tag == sbe::value::TypeTags::Null) {
        // When comparing to null we have to consider missing.
        inputExpr = b.buildMultiBranchConditional(
            typename BuilderType::CaseValuePair{b.generateNullOrMissing(b.cloneExpr(inputExpr)),
                                                b.makeNullConstant()},
            b.cloneExpr(inputExpr));

        return b.makeFillEmptyFalse(
            b.makeBinaryOpWithCollation(binaryOp, std::move(inputExpr), b.makeNullConstant()));
    } else if (sbe::value::isNaN(tag, val)) {
        // Construct an expression to perform a NaN check.
        switch (binaryOp) {
            case sbe::EPrimBinary::eq:
            case sbe::EPrimBinary::greaterEq:
            case sbe::EPrimBinary::lessEq:
                // If 'rhs' is NaN, then return whether the lhs is NaN.
                return b.makeFillEmptyFalse(b.makeFunction("isNaN", std::move(inputExpr)));
            case sbe::EPrimBinary::less:
            case sbe::EPrimBinary::greater:
                // Always return false for non-equality operators.
                return b.makeBoolConstant(false);
            default:
                tasserted(5449400,
                          str::stream()
                              << "Could not construct expression for comparison op " << binaryOp);
        }
    }

    return b.makeFillEmptyFalse(
        b.makeBinaryOpWithCollation(binaryOp, std::move(inputExpr), makeValExpr(tag, val)));
}
}  // namespace mongo::sbe_helper
