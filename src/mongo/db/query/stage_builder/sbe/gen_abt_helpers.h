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

#include <absl/container/node_hash_map.h>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/stage_builder/sbe/abt/comparison_op.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/expr.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/syntax.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr.h"

namespace mongo::stage_builder {

/**
 * Creates a boolean expression tree from given collection of leaf expression.
 */
SbExpr makeBooleanOpTree(abt::Operations logicOp,
                         std::vector<SbExpr> leaves,
                         StageBuilderState& state);

template <typename Builder>
abt::ABT makeBalancedTreeImpl(Builder builder,
                              std::vector<abt::ABT>& leaves,
                              size_t from,
                              size_t until) {
    invariant(from < until);
    if (from + 1 == until) {
        return std::move(leaves[from]);
    } else {
        size_t mid = from + (until - from) / 2;
        auto lhs = makeBalancedTreeImpl(builder, leaves, from, mid);
        auto rhs = makeBalancedTreeImpl(builder, leaves, mid, until);
        return builder(std::move(lhs), std::move(rhs));
    }
}

template <typename Builder>
abt::ABT makeBalancedTree(Builder builder, std::vector<abt::ABT> leaves) {
    return makeBalancedTreeImpl(builder, leaves, 0, leaves.size());
}

abt::ABT makeBooleanOpTree(abt::Operations logicOp, std::vector<abt::ABT> leaves);

inline auto makeABTFunction(StringData name, abt::ABTVector args) {
    return abt::make<abt::FunctionCall>(name.toString(), std::move(args));
}

template <typename... Args>
inline auto makeABTFunction(StringData name, Args&&... args) {
    return abt::make<abt::FunctionCall>(name.toString(), abt::makeSeq(std::forward<Args>(args)...));
}

inline auto makeABTConstant(sbe::value::TypeTags tag, sbe::value::Value value) {
    return abt::make<abt::Constant>(tag, value);
}

inline auto makeABTConstant(StringData str) {
    auto [tag, value] = sbe::value::makeNewString(str);
    return makeABTConstant(tag, value);
}

abt::ABT makeFillEmpty(abt::ABT expr, abt::ABT altExpr);

/**
 * Check if expression returns Nothing and return boolean false if so. Otherwise, return the
 * expression.
 */
abt::ABT makeFillEmptyFalse(abt::ABT e);
/**
 * Check if expression returns Nothing and return boolean true if so. Otherwise, return the
 * expression.
 */
abt::ABT makeFillEmptyTrue(abt::ABT e);
/**
 * Check if expression returns Nothing and return null if so. Otherwise, return the expression.
 */
abt::ABT makeFillEmptyNull(abt::ABT e);

abt::ABT makeFillEmptyUndefined(abt::ABT e);

abt::ABT makeNot(abt::ABT e);

abt::ABT makeVariable(abt::ProjectionName var);

abt::ABT makeUnaryOp(abt::Operations unaryOp, abt::ABT operand);

abt::ABT makeBinaryOp(abt::Operations binaryOp, abt::ABT lhs, abt::ABT rhs);

abt::ABT makeNaryOp(abt::Operations naryOp, abt::ABTVector args);

abt::ABT generateABTNullOrMissing(abt::ProjectionName var);
abt::ABT generateABTNullOrMissing(abt::ABT var);

abt::ABT generateABTNullMissingOrUndefined(abt::ProjectionName var);
abt::ABT generateABTNullMissingOrUndefined(abt::ABT var);

/**
 * Generates an ABT that checks if the input expression is negative assuming that it has already
 * been verified to have numeric type and to not be NaN.
 */
abt::ABT generateABTNegativeCheck(abt::ProjectionName var);

abt::ABT generateABTNonPositiveCheck(abt::ProjectionName var);
abt::ABT generateABTPositiveCheck(abt::ABT var);
abt::ABT generateABTNonNumericCheck(abt::ProjectionName var);
abt::ABT generateABTLongLongMinCheck(abt::ProjectionName var);
abt::ABT generateABTNonArrayCheck(abt::ProjectionName var);
abt::ABT generateABTNonObjectCheck(abt::ProjectionName var);
abt::ABT generateABTNonStringCheck(abt::ProjectionName var);
abt::ABT generateABTNonStringCheck(abt::ABT var);
abt::ABT generateABTNonTimestampCheck(abt::ProjectionName var);
abt::ABT generateABTNullishOrNotRepresentableInt32Check(abt::ProjectionName var);
/**
 * Generates an ABT to check the given variable is a number between -20 and 100 inclusive, and is a
 * whole number.
 */
abt::ABT generateInvalidRoundPlaceArgCheck(const abt::ProjectionName& var);
/**
 * Generates an ABT that checks if the input expression is NaN _assuming that_ it has
 * already been verified to be numeric.
 */
abt::ABT generateABTNaNCheck(abt::ProjectionName var);

abt::ABT generateABTInfinityCheck(abt::ProjectionName var);

/**
 * A pair representing a 1) true/false condition and 2) the value that should be returned if that
 * condition evaluates to true.
 */
using ABTCaseValuePair = std::pair<abt::ABT, abt::ABT>;

/**
 * Convert a list of CaseValuePairs into a chain of abt::If expressions, with the final else
 * case evaluating to the 'defaultValue' abt::ABT.
 */
template <typename... Ts>
abt::ABT buildABTMultiBranchConditional(Ts... cases);

template <typename... Ts>
abt::ABT buildABTMultiBranchConditional(ABTCaseValuePair headCase, Ts... rest) {
    return abt::make<abt::If>(std::move(headCase.first),
                              std::move(headCase.second),
                              buildABTMultiBranchConditional(std::move(rest)...));
}

template <>
abt::ABT buildABTMultiBranchConditional(abt::ABT defaultCase);

/**
 * Converts a std::vector of ABTCaseValuePairs into a chain of abt::If expressions in the
 * same manner as the 'buildABTMultiBranchConditional()' function.
 */
abt::ABT buildABTMultiBranchConditionalFromCaseValuePairs(
    std::vector<ABTCaseValuePair> caseValuePairs, abt::ABT defaultValue);

abt::ABT makeIfNullExpr(std::vector<abt::ABT> values,
                        sbe::value::FrameIdGenerator* frameIdGenerator);

abt::ABT makeIf(abt::ABT condExpr, abt::ABT thenExpr, abt::ABT elseExpr);

abt::ABT makeLet(const abt::ProjectionName& name, abt::ABT bindExpr, abt::ABT expr);

abt::ABT makeLet(std::vector<abt::ProjectionName> bindNames,
                 abt::ABTVector bindExprs,
                 abt::ABT inExpr);

abt::ABT makeLet(sbe::FrameId frameId, abt::ABT bindExpr, abt::ABT expr);

abt::ABT makeLet(sbe::FrameId frameId, abt::ABTVector bindExprs, abt::ABT expr);

abt::ABT makeLocalLambda(sbe::FrameId frameId, abt::ABT expr);

abt::ABT makeNumericConvert(abt::ABT expr, sbe::value::TypeTags tag);

abt::ABT makeABTFail(ErrorCodes::Error error, StringData errorMessage);

}  // namespace mongo::stage_builder
