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
#include <memory>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/abt/abt_lower.h"
#include "mongo/db/exec/sbe/abt/abt_lower_defs.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/optimizer/comparison_op.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/sbe_stage_builder_sbexpr.h"

namespace mongo::stage_builder {

/**
 * Creates a balanced boolean binary expression tree from given collection of leaf expression.
 */
std::unique_ptr<sbe::EExpression> makeBalancedBooleanOpTree(
    sbe::EPrimBinary::Op logicOp, std::vector<std::unique_ptr<sbe::EExpression>> leaves);

SbExpr makeBalancedBooleanOpTree(sbe::EPrimBinary::Op logicOp,
                                 std::vector<SbExpr> leaves,
                                 StageBuilderState& state);

inline auto makeABTFunction(StringData name, optimizer::ABTVector args) {
    return optimizer::make<optimizer::FunctionCall>(name.toString(), std::move(args));
}

template <typename... Args>
inline auto makeABTFunction(StringData name, Args&&... args) {
    return optimizer::make<optimizer::FunctionCall>(
        name.toString(), optimizer::makeSeq(std::forward<Args>(args)...));
}

inline auto makeABTConstant(sbe::value::TypeTags tag, sbe::value::Value value) {
    return optimizer::make<optimizer::Constant>(tag, value);
}

inline auto makeABTConstant(StringData str) {
    auto [tag, value] = sbe::value::makeNewString(str);
    return makeABTConstant(tag, value);
}

/**
 * Check if expression returns Nothing and return boolean false if so. Otherwise, return the
 * expression.
 */
optimizer::ABT makeFillEmptyFalse(optimizer::ABT e);
/**
 * Check if expression returns Nothing and return boolean true if so. Otherwise, return the
 * expression.
 */
optimizer::ABT makeFillEmptyTrue(optimizer::ABT e);
/**
 * Check if expression returns Nothing and return null if so. Otherwise, return the expression.
 */
optimizer::ABT makeFillEmptyNull(optimizer::ABT e);

optimizer::ABT makeFillEmptyUndefined(optimizer::ABT e);

optimizer::ABT makeNot(optimizer::ABT e);

optimizer::ABT makeVariable(optimizer::ProjectionName var);

optimizer::ABT makeUnaryOp(optimizer::Operations unaryOp, optimizer::ABT operand);

optimizer::ABT makeBinaryOp(optimizer::Operations binaryOp, optimizer::ABT lhs, optimizer::ABT rhs);

optimizer::ABT generateABTNullOrMissing(optimizer::ProjectionName var);
optimizer::ABT generateABTNullOrMissing(optimizer::ABT var);

optimizer::ABT generateABTNullMissingOrUndefined(optimizer::ProjectionName var);
optimizer::ABT generateABTNullMissingOrUndefined(optimizer::ABT var);

/**
 * Generates an ABT that checks if the input expression is negative assuming that it has already
 * been verified to have numeric type and to not be NaN.
 */
optimizer::ABT generateABTNegativeCheck(optimizer::ProjectionName var);

optimizer::ABT generateABTNonPositiveCheck(optimizer::ProjectionName var);
optimizer::ABT generateABTPositiveCheck(optimizer::ABT var);
optimizer::ABT generateABTNonNumericCheck(optimizer::ProjectionName var);
optimizer::ABT generateABTLongLongMinCheck(optimizer::ProjectionName var);
optimizer::ABT generateABTNonArrayCheck(optimizer::ProjectionName var);
optimizer::ABT generateABTNonObjectCheck(optimizer::ProjectionName var);
optimizer::ABT generateABTNonStringCheck(optimizer::ProjectionName var);
optimizer::ABT generateABTNonStringCheck(optimizer::ABT var);
optimizer::ABT generateABTNonTimestampCheck(optimizer::ProjectionName var);
optimizer::ABT generateABTNullishOrNotRepresentableInt32Check(optimizer::ProjectionName var);
/**
 * Generates an ABT to check the given variable is a number between -20 and 100 inclusive, and is a
 * whole number.
 */
optimizer::ABT generateInvalidRoundPlaceArgCheck(const optimizer::ProjectionName& var);
/**
 * Generates an ABT that checks if the input expression is NaN _assuming that_ it has
 * already been verified to be numeric.
 */
optimizer::ABT generateABTNaNCheck(optimizer::ProjectionName var);

optimizer::ABT generateABTInfinityCheck(optimizer::ProjectionName var);

/**
 * A pair representing a 1) true/false condition and 2) the value that should be returned if that
 * condition evaluates to true.
 */
using ABTCaseValuePair = std::pair<optimizer::ABT, optimizer::ABT>;

/**
 * Convert a list of CaseValuePairs into a chain of optimizer::If expressions, with the final else
 * case evaluating to the 'defaultValue' optimizer::ABT.
 */
template <typename... Ts>
optimizer::ABT buildABTMultiBranchConditional(Ts... cases);

template <typename... Ts>
optimizer::ABT buildABTMultiBranchConditional(ABTCaseValuePair headCase, Ts... rest) {
    return optimizer::make<optimizer::If>(std::move(headCase.first),
                                          std::move(headCase.second),
                                          buildABTMultiBranchConditional(std::move(rest)...));
}

template <>
optimizer::ABT buildABTMultiBranchConditional(optimizer::ABT defaultCase);

/**
 * Converts a std::vector of ABTCaseValuePairs into a chain of optimizer::If expressions in the
 * same manner as the 'buildABTMultiBranchConditional()' function.
 */
optimizer::ABT buildABTMultiBranchConditionalFromCaseValuePairs(
    std::vector<ABTCaseValuePair> caseValuePairs, optimizer::ABT defaultValue);

optimizer::ABT makeIfNullExpr(std::vector<optimizer::ABT> values,
                              sbe::value::FrameIdGenerator* frameIdGenerator);

using SlotABTExprPairVector = std::vector<std::pair<sbe::value::SlotId, optimizer::ABT>>;

struct ABTAggExprPair {
    optimizer::ABT init;
    optimizer::ABT acc;
};

using ABTAggExprVector = std::vector<std::pair<sbe::value::SlotId, ABTAggExprPair>>;

optimizer::ABT makeIf(optimizer::ABT condExpr, optimizer::ABT thenExpr, optimizer::ABT elseExpr);

optimizer::ABT makeLet(const optimizer::ProjectionName& name,
                       optimizer::ABT bindExpr,
                       optimizer::ABT expr);

optimizer::ABT makeLet(sbe::FrameId frameId, optimizer::ABT bindExpr, optimizer::ABT expr);

optimizer::ABT makeLet(sbe::FrameId frameId, optimizer::ABTVector bindExprs, optimizer::ABT expr);

optimizer::ABT makeLocalLambda(sbe::FrameId frameId, optimizer::ABT expr);

optimizer::ABT makeNumericConvert(optimizer::ABT expr, sbe::value::TypeTags tag);

optimizer::ABT makeABTFail(ErrorCodes::Error error, StringData errorMessage);

}  // namespace mongo::stage_builder
