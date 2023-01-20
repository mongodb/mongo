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

#include "mongo/db/exec/sbe/abt/abt_lower.h"
#include "mongo/db/query/sbe_stage_builder_eval_frame.h"


namespace mongo::stage_builder {

/**
 * Creates a balanced boolean binary expression tree from given collection of leaf expression.
 */
std::unique_ptr<sbe::EExpression> makeBalancedBooleanOpTree(
    sbe::EPrimBinary::Op logicOp, std::vector<std::unique_ptr<sbe::EExpression>> leaves);

optimizer::ABT makeBalancedBooleanOpTree(optimizer::Operations logicOp,
                                         std::vector<optimizer::ABT> leaves);

EvalExpr makeBalancedBooleanOpTree(sbe::EPrimBinary::Op logicOp,
                                   std::vector<EvalExpr> leaves,
                                   StageBuilderState& state);

std::unique_ptr<sbe::EExpression> abtToExpr(optimizer::ABT& abt,
                                            optimizer::SlotVarMap& slotMap,
                                            const sbe::RuntimeEnvironment& runtimeEnv);

template <typename... Args>
inline auto makeABTFunction(StringData name, Args&&... args) {
    return optimizer::make<optimizer::FunctionCall>(
        name.toString(), optimizer::makeSeq(std::forward<Args>(args)...));
}

template <typename T>
inline auto makeABTConstant(sbe::value::TypeTags tag, T value) {
    return optimizer::make<optimizer::Constant>(tag, sbe::value::bitcastFrom<T>(value));
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
optimizer::ABT makeNot(optimizer::ABT e);

optimizer::ProjectionName makeVariableName(sbe::value::SlotId slotId);
optimizer::ProjectionName makeLocalVariableName(sbe::FrameId frameId, sbe::value::SlotId slotId);
optimizer::ABT makeVariable(optimizer::ProjectionName var);

optimizer::ABT makeUnaryOp(optimizer::Operations unaryOp, optimizer::ABT operand);

optimizer::ABT generateABTNullOrMissing(optimizer::ProjectionName var);
optimizer::ABT generateABTNullOrMissing(optimizer::ABT var);
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
 * Generates an ABT that checks if the input expression is NaN _assuming that_ it has
 * already been verified to be numeric.
 */
optimizer::ABT generateABTNaNCheck(optimizer::ProjectionName var);

optimizer::ABT makeABTFail(ErrorCodes::Error error, StringData errorMessage);

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

}  // namespace mongo::stage_builder
