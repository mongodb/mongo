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

std::unique_ptr<sbe::EExpression> makeUnaryOp(sbe::EPrimUnary::Op unaryOp,
                                              std::unique_ptr<sbe::EExpression> operand);

/**
 * Wrap expression into logical negation.
 */
std::unique_ptr<sbe::EExpression> makeNot(std::unique_ptr<sbe::EExpression> e);

std::unique_ptr<sbe::EExpression> makeBinaryOp(sbe::EPrimBinary::Op binaryOp,
                                               std::unique_ptr<sbe::EExpression> lhs,
                                               std::unique_ptr<sbe::EExpression> rhs,
                                               std::unique_ptr<sbe::EExpression> collator = {});

std::unique_ptr<sbe::EExpression> makeBinaryOp(sbe::EPrimBinary::Op binaryOp,
                                               std::unique_ptr<sbe::EExpression> lhs,
                                               std::unique_ptr<sbe::EExpression> rhs,
                                               sbe::RuntimeEnvironment* env);

std::unique_ptr<sbe::EExpression> makeIsMember(std::unique_ptr<sbe::EExpression> input,
                                               std::unique_ptr<sbe::EExpression> arr,
                                               std::unique_ptr<sbe::EExpression> collator = {});

std::unique_ptr<sbe::EExpression> makeIsMember(std::unique_ptr<sbe::EExpression> input,
                                               std::unique_ptr<sbe::EExpression> arr,
                                               sbe::RuntimeEnvironment* env);

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
 * Generates an EExpression that checks if the input expression is not a string, _assuming that
 * it has already been verified to be neither null nor missing.
 */
std::unique_ptr<sbe::EExpression> generateNonStringCheck(const sbe::EVariable& var);

/**
 * Generates an EExpression that checks whether the input expression is null, missing, or
 * unable to be converted to the type NumberInt32.
 */
std::unique_ptr<sbe::EExpression> generateNullishOrNotRepresentableInt32Check(
    const sbe::EVariable& var);

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
 * Insert a limit stage on top of the 'input' stage.
 */
std::unique_ptr<sbe::PlanStage> makeLimitTree(std::unique_ptr<sbe::PlanStage> inputStage,
                                              PlanNodeId planNodeId,
                                              long long limit = 1);

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
 * Check if expression returns Nothing and return boolean false if so. Otherwise, return the
 * expression.
 */
std::unique_ptr<sbe::EExpression> makeFillEmptyFalse(std::unique_ptr<sbe::EExpression> e);

/**
 * Creates an EFunction expression with the given name and arguments.
 */
template <typename... Args>
inline std::unique_ptr<sbe::EExpression> makeFunction(std::string_view name, Args&&... args) {
    return sbe::makeE<sbe::EFunction>(name, sbe::makeEs(std::forward<Args>(args)...));
}

template <typename T>
inline auto makeConstant(sbe::value::TypeTags tag, T&& value) {
    return sbe::makeE<sbe::EConstant>(tag, sbe::value::bitcastFrom<T>(std::forward<T>(value)));
}

inline auto makeConstant(std::string_view str) {
    auto [tag, value] = sbe::value::makeNewString(str);
    return sbe::makeE<sbe::EConstant>(tag, value);
}

/**
 * Check if expression returns Nothing and return null if so. Otherwise, return the
 * expression.
 */
std::unique_ptr<sbe::EExpression> makeFillEmptyNull(std::unique_ptr<sbe::EExpression> e);

/**
 * Check if expression returns an array and return Nothing if so. Otherwise, return the expression.
 */
std::unique_ptr<sbe::EExpression> makeNothingArrayCheck(
    std::unique_ptr<sbe::EExpression> isArrayInput, std::unique_ptr<sbe::EExpression> otherwise);

/**
 * Creates an expression to extract a shard key part from inputExpr. The generated expression is a
 * let binding that binds a getField expression to extract the shard key part value from the
 * inputExpr. The entire let binding evaluates to a constant expression carrying the Nothing value
 * if the binding is an array. Otherwise, it evaluates to a fillEmpty null expression. Here is an
 * example expression generated from this function for a shard key pattern {'a.b': 1}:
 *
 * let [l1.0 = getField (s1, "a")]
 *   if (isArray (l1.0), NOTHING,
 *     let [l2.0 = getField (l1.0, "b")]
 *       if (isArray (l2.0), NOTHING, fillEmpty (l2.0, null)))
 */
std::unique_ptr<sbe::EExpression> generateShardKeyBinding(
    const FieldRef& keyPatternField,
    sbe::value::FrameIdGenerator& frameIdGenerator,
    std::unique_ptr<sbe::EExpression> inputExpr,
    int level);

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

template <bool IsConst, bool IsEof = false>
EvalStage makeFilter(EvalStage stage,
                     std::unique_ptr<sbe::EExpression> filter,
                     PlanNodeId planNodeId) {
    stage = stageOrLimitCoScan(std::move(stage), planNodeId);

    return {sbe::makeS<sbe::FilterStage<IsConst, IsEof>>(
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
 * Creates an unwind stage and an output slot for it using the first slot in the outSlots vector of
 * the inputEvalStage as the input slot to the new stage. The preserveNullAndEmptyArrays is passed
 * to the UnwindStage constructor to specify the treatment of null or missing inputs.
 */
EvalStage makeUnwind(EvalStage inputEvalStage,
                     sbe::value::SlotIdGenerator* slotIdGenerator,
                     PlanNodeId planNodeId,
                     bool preserveNullAndEmptyArrays = true);

/**
 * Creates a branch stage with the specified condition ifExpr and creates output slots for the
 * branch stage. This forwards the outputs of the thenStage to the output slots of the branchStage
 * if the condition evaluates to true, and forwards the elseStage outputs if the condition is false.
 */
EvalStage makeBranch(std::unique_ptr<sbe::EExpression> ifExpr,
                     EvalStage thenStage,
                     EvalStage elseStage,
                     sbe::value::SlotIdGenerator* slotIdGenerator,
                     PlanNodeId planNodeId);

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
 * Creates a union stage with specified branches. Each branch is passed to 'branchFn' first. If
 * 'branchFn' is not set, expression from branch is simply projected to a slot.
 */
EvalExprStagePair generateUnion(std::vector<EvalExprStagePair> branches,
                                BranchFn branchFn,
                                PlanNodeId planNodeId,
                                sbe::value::SlotIdGenerator* slotIdGenerator);
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

/** This helper takes an SBE SlotIdGenerator and an SBE Array and returns an output slot and a
 * unwind/project/limit/coscan subtree that streams out the elements of the array one at a time via
 * the output slot over a series of calls to getNext(), mimicking the output of a collection scan or
 * an index scan. Note that this method assumes ownership of the SBE Array being passed in.
 */
std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>> generateVirtualScan(
    sbe::value::SlotIdGenerator* slotIdGenerator,
    sbe::value::TypeTags arrTag,
    sbe::value::Value arrVal);

/**
 * Make a mock scan with multiple output slots from an BSON array. This method does NOT assume
 * ownership of the BSONArray passed in.
 */
std::pair<sbe::value::SlotVector, std::unique_ptr<sbe::PlanStage>> generateVirtualScanMulti(
    sbe::value::SlotIdGenerator* slotIdGenerator,
    int numSlots,
    sbe::value::TypeTags arrTag,
    sbe::value::Value arrVal);

/**
 * Converts a BSONArray to an SBE Array. Caller owns the SBE Array returned. This method does not
 * assume ownership of the BSONArray.
 */
std::pair<sbe::value::TypeTags, sbe::value::Value> makeValue(const BSONArray& ba);

/**
 * Returns a BSON type mask of all data types coercible to date.
 */
uint32_t dateTypeMask();
}  // namespace mongo::stage_builder
