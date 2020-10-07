/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/query/sbe_stage_builder_eval_frame.h"
#include "mongo/db/query/stage_types.h"

namespace mongo::stage_builder {

/**
 * Generates an EExpression that checks if the input expression is null or missing.
 */
std::unique_ptr<sbe::EExpression> generateNullOrMissing(const sbe::EVariable& var);

std::unique_ptr<sbe::EExpression> generateNullOrMissing(const sbe::FrameId frameId,
                                                        const sbe::value::SlotId slotId);

/**
 * Generates an EExpression that checks if the input expression is a non-numeric type _assuming
 * that_ it has already been verified to be neither null nor missing.
 */
std::unique_ptr<sbe::EExpression> generateNonNumericCheck(const sbe::EVariable& var);

/**
 * Generates an EExpression that checks if the input expression is the value NumberLong(-2^64).
 */
std::unique_ptr<sbe::EExpression> generateLongLongMinCheck(const sbe::EVariable& var);

/**
 * Generates an EExpression that checks if the input expression is NaN _assuming that_ it has
 * already been verified to be numeric.
 */
std::unique_ptr<sbe::EExpression> generateNaNCheck(const sbe::EVariable& var);

/**
 * Generates an EExpression that checks if the input expression is a non-positive number (i.e. <= 0)
 * _assuming that_ it has already been verified to be numeric.
 */
std::unique_ptr<sbe::EExpression> generateNonPositiveCheck(const sbe::EVariable& var);

/**
 * Generates an EExpression that checks if the input expression is a negative (i.e., < 0) number
 * _assuming that_ it has already been verified to be numeric.
 */
std::unique_ptr<sbe::EExpression> generateNegativeCheck(const sbe::EVariable& var);

/**
 * Generates an EExpression that checks if the input expression is _not_ an object, _assuming that_
 * it has already been verified to be neither null nor missing.
 */
std::unique_ptr<sbe::EExpression> generateNonObjectCheck(const sbe::EVariable& var);

/**
 * A pair representing a 1) true/false condition and 2) the value that should be returned if that
 * condition evaluates to true.
 */
using CaseValuePair =
    std::pair<std::unique_ptr<sbe::EExpression>, std::unique_ptr<sbe::EExpression>>;

/**
 * Convert a list of CaseValuePairs into a chain of EIf expressions, with the final else case
 * evaluating to the 'defaultValue' EExpression.
 */
template <typename... Ts>
std::unique_ptr<sbe::EExpression> buildMultiBranchConditional(Ts... cases);

template <typename... Ts>
std::unique_ptr<sbe::EExpression> buildMultiBranchConditional(CaseValuePair headCase, Ts... rest) {
    return sbe::makeE<sbe::EIf>(std::move(headCase.first),
                                std::move(headCase.second),
                                buildMultiBranchConditional(std::move(rest)...));
}

template <>
std::unique_ptr<sbe::EExpression> buildMultiBranchConditional(
    std::unique_ptr<sbe::EExpression> defaultCase);

/**
 * Create tree consisting of coscan stage followed by limit stage.
 */
std::unique_ptr<sbe::PlanStage> makeLimitCoScanTree(PlanNodeId planNodeId, long long limit = 1);

/**
 * Same as 'makeLimitCoScanTree()', but returns 'EvalStage' with empty 'outSlots' vector.
 */
EvalStage makeLimitCoScanStage(PlanNodeId planNodeId, long long limit = 1);

/**
 * If 'stage.stage' is 'nullptr', return limit-1/coscan tree. Otherwise, return stage.
 */
EvalStage stageOrLimitCoScan(EvalStage stage, PlanNodeId planNodeId, long long limit = 1);

/**
 * Wrap expression into logical negation.
 */
std::unique_ptr<sbe::EExpression> makeNot(std::unique_ptr<sbe::EExpression> e);

/**
 * Check if expression returns Nothing and return boolean false if so. Otherwise, return the
 * expression.
 */
std::unique_ptr<sbe::EExpression> makeFillEmptyFalse(std::unique_ptr<sbe::EExpression> e);

/**
 * If given 'EvalExpr' already contains a slot, simply returns it. Otherwise, allocates a new slot
 * and creates project stage to assign expression to this new slot. After that, new slot and project
 * stage are returned.
 */
std::pair<sbe::value::SlotId, EvalStage> projectEvalExpr(
    EvalExpr expr,
    EvalStage stage,
    PlanNodeId planNodeId,
    sbe::value::SlotIdGenerator* slotIdGenerator);

template <bool IsConst>
EvalStage makeFilter(EvalStage stage,
                     std::unique_ptr<sbe::EExpression> filter,
                     PlanNodeId planNodeId) {
    stage = stageOrLimitCoScan(std::move(stage), planNodeId);

    return {sbe::makeS<sbe::FilterStage<IsConst>>(
                std::move(stage.stage), std::move(filter), planNodeId),
            std::move(stage.outSlots)};
}

template <typename... Ts>
EvalStage makeProject(EvalStage stage, PlanNodeId planNodeId, Ts&&... pack) {
    stage = stageOrLimitCoScan(std::move(stage), planNodeId);

    auto outSlots = std::move(stage.outSlots);
    auto projects = makeEM(std::forward<Ts>(pack)...);

    for (auto& [slot, expr] : projects) {
        outSlots.push_back(slot);
    }

    return {sbe::makeS<sbe::ProjectStage>(std::move(stage.stage), std::move(projects), planNodeId),
            std::move(outSlots)};
}

/**
 * Creates loop join stage. All 'outSlots' from the 'left' argument along with slots from the
 * 'lexicalEnvironment' argument are passed as correlated.
 * If stage in 'left' or 'right' argument is 'nullptr', it is treated as if it was limit-1/coscan.
 * In this case, loop join stage is not created. 'right' stage is returned if 'left' is 'nullptr'.
 * 'left' stage is returned if 'right' is 'nullptr'.
 */
EvalStage makeLoopJoin(EvalStage left,
                       EvalStage right,
                       PlanNodeId planNodeId,
                       const sbe::value::SlotVector& lexicalEnvironment = {});

/**
 * Creates traverse stage. All 'outSlots' from 'outer' argument (except for 'inField') along with
 * slots from the 'lexicalEnvironment' argument are passed as correlated.
 */
EvalStage makeTraverse(EvalStage outer,
                       EvalStage inner,
                       sbe::value::SlotId inField,
                       sbe::value::SlotId outField,
                       sbe::value::SlotId outFieldInner,
                       std::unique_ptr<sbe::EExpression> foldExpr,
                       std::unique_ptr<sbe::EExpression> finalExpr,
                       PlanNodeId planNodeId,
                       boost::optional<size_t> nestedArraysDepth,
                       const sbe::value::SlotVector& lexicalEnvironment = {});

using BranchFn = std::function<std::pair<sbe::value::SlotId, EvalStage>(
    EvalExpr expr,
    EvalStage stage,
    PlanNodeId planNodeId,
    sbe::value::SlotIdGenerator* slotIdGenerator)>;

/**
 * Creates limit-1/union stage with specified branches. Each branch is passed to 'branchFn' first.
 * If 'branchFn' is not set, expression from branch is simply projected to a slot.
 */
EvalExprStagePair generateSingleResultUnion(std::vector<EvalExprStagePair> branches,
                                            BranchFn branchFn,
                                            PlanNodeId planNodeId,
                                            sbe::value::SlotIdGenerator* slotIdGenerator);

/**
 * Creates tree with short-circuiting for OR and AND. Each element in 'braches' argument represents
 * logical expression and sub-tree generated for it.
 */
EvalExprStagePair generateShortCircuitingLogicalOp(sbe::EPrimBinary::Op logicOp,
                                                   std::vector<EvalExprStagePair> branches,
                                                   PlanNodeId planNodeId,
                                                   sbe::value::SlotIdGenerator* slotIdGenerator);

}  // namespace mongo::stage_builder
