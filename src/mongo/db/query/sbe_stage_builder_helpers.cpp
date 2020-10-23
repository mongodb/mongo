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

#include "mongo/platform/basic.h"

#include "mongo/db/query/sbe_stage_builder_helpers.h"

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/traverse.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/stages/unwind.h"
#include "mongo/db/matcher/matcher_type_set.h"

namespace mongo::stage_builder {

std::unique_ptr<sbe::EExpression> generateNullOrMissing(const sbe::EVariable& var) {
    return sbe::makeE<sbe::EPrimBinary>(
        sbe::EPrimBinary::logicOr,
        sbe::makeE<sbe::EPrimUnary>(sbe::EPrimUnary::logicNot,
                                    sbe::makeE<sbe::EFunction>("exists", sbe::makeEs(var.clone()))),
        sbe::makeE<sbe::EFunction>("isNull", sbe::makeEs(var.clone())));
}

std::unique_ptr<sbe::EExpression> generateNullOrMissing(const sbe::FrameId frameId,
                                                        const sbe::value::SlotId slotId) {
    sbe::EVariable var{frameId, slotId};
    return generateNullOrMissing(var);
}

std::unique_ptr<sbe::EExpression> generateNonNumericCheck(const sbe::EVariable& var) {
    return sbe::makeE<sbe::EPrimUnary>(
        sbe::EPrimUnary::logicNot,
        sbe::makeE<sbe::EFunction>("isNumber", sbe::makeEs(var.clone())));
}

std::unique_ptr<sbe::EExpression> generateLongLongMinCheck(const sbe::EVariable& var) {
    return sbe::makeE<sbe::EPrimBinary>(
        sbe::EPrimBinary::logicAnd,
        sbe::makeE<sbe::ETypeMatch>(var.clone(),
                                    MatcherTypeSet{BSONType::NumberLong}.getBSONTypeMask()),
        sbe::makeE<sbe::EPrimBinary>(
            sbe::EPrimBinary::eq,
            var.clone(),
            sbe::makeE<sbe::EConstant>(
                sbe::value::TypeTags::NumberInt64,
                sbe::value::bitcastFrom<int64_t>(std::numeric_limits<int64_t>::min()))));
}

std::unique_ptr<sbe::EExpression> generateNaNCheck(const sbe::EVariable& var) {
    return sbe::makeE<sbe::EFunction>("isNaN", sbe::makeEs(var.clone()));
}

std::unique_ptr<sbe::EExpression> generateNonPositiveCheck(const sbe::EVariable& var) {
    return sbe::makeE<sbe::EPrimBinary>(
        sbe::EPrimBinary::EPrimBinary::lessEq,
        var.clone(),
        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                   sbe::value::bitcastFrom<int32_t>(0)));
}

std::unique_ptr<sbe::EExpression> generateNegativeCheck(const sbe::EVariable& var) {
    return sbe::makeE<sbe::EPrimBinary>(
        sbe::EPrimBinary::EPrimBinary::less,
        var.clone(),
        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                   sbe::value::bitcastFrom<int32_t>(0)));
}

std::unique_ptr<sbe::EExpression> generateNonObjectCheck(const sbe::EVariable& var) {
    return sbe::makeE<sbe::EPrimUnary>(
        sbe::EPrimUnary::logicNot,
        sbe::makeE<sbe::EFunction>("isObject", sbe::makeEs(var.clone())));
}

std::unique_ptr<sbe::EExpression> generateNonStringCheck(const sbe::EVariable& var) {
    return sbe::makeE<sbe::EPrimUnary>(
        sbe::EPrimUnary::logicNot,
        sbe::makeE<sbe::EFunction>("isString", sbe::makeEs(var.clone())));
}

std::unique_ptr<sbe::EExpression> generateNullishOrNotRepresentableInt32Check(
    const sbe::EVariable& var) {
    auto numericConvert32 =
        sbe::makeE<sbe::ENumericConvert>(var.clone(), sbe::value::TypeTags::NumberInt32);
    return sbe::makeE<sbe::EPrimBinary>(
        sbe::EPrimBinary::logicOr,
        generateNullOrMissing(var),
        sbe::makeE<sbe::EPrimUnary>(
            sbe::EPrimUnary::logicNot,
            sbe::makeE<sbe::EFunction>("exists", sbe::makeEs(std::move(numericConvert32)))));
}

template <>
std::unique_ptr<sbe::EExpression> buildMultiBranchConditional(
    std::unique_ptr<sbe::EExpression> defaultCase) {
    return defaultCase;
}

std::unique_ptr<sbe::PlanStage> makeLimitCoScanTree(PlanNodeId planNodeId, long long limit) {
    return sbe::makeS<sbe::LimitSkipStage>(
        sbe::makeS<sbe::CoScanStage>(planNodeId), limit, boost::none, planNodeId);
}

std::unique_ptr<sbe::EExpression> makeNot(std::unique_ptr<sbe::EExpression> e) {
    return sbe::makeE<sbe::EPrimUnary>(sbe::EPrimUnary::logicNot, std::move(e));
}

std::unique_ptr<sbe::EExpression> makeFillEmptyFalse(std::unique_ptr<sbe::EExpression> e) {
    using namespace std::literals;
    return sbe::makeE<sbe::EFunction>(
        "fillEmpty"sv,
        sbe::makeEs(std::move(e),
                    sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean,
                                               sbe::value::bitcastFrom<bool>(false))));
}

EvalStage makeLimitCoScanStage(PlanNodeId planNodeId, long long limit) {
    return {makeLimitCoScanTree(planNodeId, limit), sbe::makeSV()};
}

EvalStage stageOrLimitCoScan(EvalStage stage, PlanNodeId planNodeId, long long limit) {
    if (stage.stage) {
        return stage;
    }
    return makeLimitCoScanStage(planNodeId, limit);
}

std::pair<sbe::value::SlotId, EvalStage> projectEvalExpr(
    EvalExpr expr,
    EvalStage stage,
    PlanNodeId planNodeId,
    sbe::value::SlotIdGenerator* slotIdGenerator) {
    // If expr's value is already in a slot, return the slot.
    if (expr.getSlot()) {
        return {*expr.getSlot(), std::move(stage)};
    }

    // If expr's value is an expression, create a ProjectStage to evaluate the expression
    // into a slot.
    auto slot = slotIdGenerator->generate();
    stage = makeProject(std::move(stage), planNodeId, slot, expr.extractExpr());
    return {slot, std::move(stage)};
}

EvalStage makeLoopJoin(EvalStage left,
                       EvalStage right,
                       PlanNodeId planNodeId,
                       const sbe::value::SlotVector& lexicalEnvironment) {
    // If 'left' and 'right' are both null, we just return null. If one of 'left'/'right' is null
    // and the other is non-null, return whichever one is non-null.
    if (!left.stage) {
        return right;
    } else if (!right.stage) {
        return left;
    }

    auto outerProjects = left.outSlots;
    auto outerCorrelated = left.outSlots;

    outerCorrelated.insert(
        outerCorrelated.end(), lexicalEnvironment.begin(), lexicalEnvironment.end());

    auto outSlots = std::move(left.outSlots);
    outSlots.insert(outSlots.end(), right.outSlots.begin(), right.outSlots.end());

    return {sbe::makeS<sbe::LoopJoinStage>(std::move(left.stage),
                                           std::move(right.stage),
                                           std::move(outerProjects),
                                           std::move(outerCorrelated),
                                           nullptr,
                                           planNodeId),
            std::move(outSlots)};
}

EvalStage makeTraverse(EvalStage outer,
                       EvalStage inner,
                       sbe::value::SlotId inField,
                       sbe::value::SlotId outField,
                       sbe::value::SlotId outFieldInner,
                       std::unique_ptr<sbe::EExpression> foldExpr,
                       std::unique_ptr<sbe::EExpression> finalExpr,
                       PlanNodeId planNodeId,
                       boost::optional<size_t> nestedArraysDepth,
                       const sbe::value::SlotVector& lexicalEnvironment) {
    outer = stageOrLimitCoScan(std::move(outer), planNodeId);
    inner = stageOrLimitCoScan(std::move(inner), planNodeId);

    sbe::value::SlotVector outerCorrelated = lexicalEnvironment;
    for (auto slot : outer.outSlots) {
        if (slot != inField) {
            outerCorrelated.push_back(slot);
        }
    }

    auto outSlots = std::move(outer.outSlots);
    outSlots.push_back(outField);

    return {sbe::makeS<sbe::TraverseStage>(std::move(outer.stage),
                                           std::move(inner.stage),
                                           inField,
                                           outField,
                                           outFieldInner,
                                           std::move(outerCorrelated),
                                           std::move(foldExpr),
                                           std::move(finalExpr),
                                           planNodeId,
                                           nestedArraysDepth),
            std::move(outSlots)};
}

EvalExprStagePair generateSingleResultUnion(std::vector<EvalExprStagePair> branches,
                                            BranchFn branchFn,
                                            PlanNodeId planNodeId,
                                            sbe::value::SlotIdGenerator* slotIdGenerator) {
    std::vector<std::unique_ptr<sbe::PlanStage>> stages;
    std::vector<sbe::value::SlotVector> inputs;
    stages.reserve(branches.size());
    inputs.reserve(branches.size());

    for (size_t i = 0; i < branches.size(); i++) {
        auto [slot, stage] = [&]() {
            auto& [expr, stage] = branches[i];

            if (!branchFn || i + 1 == branches.size()) {
                return projectEvalExpr(
                    std::move(expr), std::move(stage), planNodeId, slotIdGenerator);
            }

            return branchFn(std::move(expr), std::move(stage), planNodeId, slotIdGenerator);
        }();

        stages.emplace_back(std::move(stage.stage));
        inputs.emplace_back(sbe::makeSV(slot));
    }

    auto outputSlot = slotIdGenerator->generate();
    auto unionStage = sbe::makeS<sbe::UnionStage>(
        std::move(stages), std::move(inputs), sbe::makeSV(outputSlot), planNodeId);
    EvalStage outputStage = {
        sbe::makeS<sbe::LimitSkipStage>(std::move(unionStage), 1, boost::none, planNodeId),
        sbe::makeSV(outputSlot)};

    return {outputSlot, std::move(outputStage)};
}

EvalExprStagePair generateShortCircuitingLogicalOp(sbe::EPrimBinary::Op logicOp,
                                                   std::vector<EvalExprStagePair> branches,
                                                   PlanNodeId planNodeId,
                                                   sbe::value::SlotIdGenerator* slotIdGenerator) {
    invariant(logicOp == sbe::EPrimBinary::logicAnd || logicOp == sbe::EPrimBinary::logicOr);

    // For AND and OR, if 'branches' only has one element, we can just return branches[0].
    if (branches.size() == 1) {
        return std::move(branches[0]);
    }

    // Prepare to create limit-1/union with N branches (where N is the number of operands). Each
    // branch will be evaluated from left to right until one of the branches produces a value. The
    // first N-1 branches have a FilterStage to control whether they produce a value. If a branch's
    // filter condition is true, the branch will produce a value and the remaining branches will not
    // be evaluated. In other words, the evaluation process will "short-circuit". If a branch's
    // filter condition is false, the branch will not produce a value and the evaluation process
    // will continue. The last branch doesn't have a FilterStage and will always produce a value.
    auto branchFn = [logicOp](EvalExpr expr,
                              EvalStage stage,
                              PlanNodeId planNodeId,
                              sbe::value::SlotIdGenerator* slotIdGenerator) {
        // Create a FilterStage for each branch (except the last one). If a branch's filter
        // condition is true, it will "short-circuit" the evaluation process. For AND, short-
        // circuiting should happen if an operand evalautes to false. For OR, short-circuiting
        // should happen if an operand evaluates to true.
        auto filterExpr = (logicOp == sbe::EPrimBinary::logicAnd) ? makeNot(expr.extractExpr())
                                                                  : expr.extractExpr();

        stage = makeFilter<false>(std::move(stage), std::move(filterExpr), planNodeId);

        // Set up an output value to be returned if short-circuiting occurs. For AND, when
        // short-circuiting occurs, the output returned should be false. For OR, when short-
        // circuiting occurs, the output returned should be true.
        auto shortCircuitVal = sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean,
                                                          (logicOp == sbe::EPrimBinary::logicOr));
        auto slot = slotIdGenerator->generate();
        auto resultStage =
            makeProject(std::move(stage), planNodeId, slot, std::move(shortCircuitVal));
        return std::make_pair(slot, std::move(resultStage));
    };

    return generateSingleResultUnion(std::move(branches), branchFn, planNodeId, slotIdGenerator);
}

std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>> generateVirtualScan(
    sbe::value::SlotIdGenerator* slotIdGenerator,
    sbe::value::TypeTags arrTag,
    sbe::value::Value arrVal) {
    // The value passed in must be an array.
    invariant(sbe::value::isArray(arrTag));

    // Make an EConstant expression for the array.
    auto arrayExpression = sbe::makeE<sbe::EConstant>(arrTag, arrVal);

    // Build the unwind/project/limit/coscan subtree.
    auto projectSlot = slotIdGenerator->generate();
    auto unwindSlot = slotIdGenerator->generate();
    auto unwind = sbe::makeS<sbe::UnwindStage>(
        sbe::makeProjectStage(makeLimitCoScanTree(kEmptyPlanNodeId, 1),
                              kEmptyPlanNodeId,
                              projectSlot,
                              std::move(arrayExpression)),
        projectSlot,
        unwindSlot,
        slotIdGenerator->generate(),  // We don't need an index slot but must to provide it.
        false,                        // Don't preserve null and empty arrays.
        kEmptyPlanNodeId);

    // Return the UnwindStage and its output slot. The UnwindStage can be used as an input
    // to other PlanStages.
    return {unwindSlot, std::move(unwind)};
}

std::pair<sbe::value::SlotVector, std::unique_ptr<sbe::PlanStage>> generateVirtualScanMulti(
    sbe::value::SlotIdGenerator* slotIdGenerator,
    int numSlots,
    sbe::value::TypeTags arrTag,
    sbe::value::Value arrVal) {
    using namespace std::literals;

    invariant(numSlots >= 1);

    // Generate a mock scan with a single output slot.
    auto [scanSlot, scanStage] = generateVirtualScan(slotIdGenerator, arrTag, arrVal);

    // Create a ProjectStage that will read the data from 'scanStage' and split it up
    // across multiple output slots.
    sbe::value::SlotVector projectSlots;
    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projections;
    for (int32_t i = 0; i < numSlots; ++i) {
        projectSlots.emplace_back(slotIdGenerator->generate());
        projections.emplace(
            projectSlots.back(),
            sbe::makeE<sbe::EFunction>(
                "getElement"sv,
                sbe::makeEs(sbe::makeE<sbe::EVariable>(scanSlot),
                            sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                                       sbe::value::bitcastFrom<int32_t>(i)))));
    }

    return {std::move(projectSlots),
            sbe::makeS<sbe::ProjectStage>(
                std::move(scanStage), std::move(projections), kEmptyPlanNodeId)};
}

std::pair<sbe::value::TypeTags, sbe::value::Value> makeValue(const BSONArray& ba) {
    int numBytes = ba.objsize();
    uint8_t* data = new uint8_t[numBytes];
    memcpy(data, reinterpret_cast<const uint8_t*>(ba.objdata()), numBytes);
    return {sbe::value::TypeTags::bsonArray, sbe::value::bitcastFrom<uint8_t*>(data)};
}

}  // namespace mongo::stage_builder
