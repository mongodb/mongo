/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/base/string_data_comparator_interface.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_for_window_functions.h"
#include "mongo/db/pipeline/accumulator_js_reduce.h"
#include "mongo/db/query/sbe_stage_builder_accumulator.h"
#include "mongo/db/query/sbe_stage_builder_expression.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"

namespace mongo::stage_builder {
namespace {
std::pair<std::vector<std::unique_ptr<sbe::EExpression>>, EvalStage> buildAccumulatorMin(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(buildAccumulatorMinMax("min", std::move(arg)));
    return {std::move(aggs), std::move(inputStage)};
}

std::pair<std::unique_ptr<sbe::EExpression>, EvalStage> buildFinalizeMin(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& minSlots,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    // We can get away with not building a project stage since there's no finalize step but we
    // will stick the slot into an EVariable in case a $min is one of many group clauses and it
    // can be combined into a final project stage.
    tassert(5754702,
            str::stream() << "Expected one input slot for finalization of min, got: "
                          << minSlots.size(),
            minSlots.size() == 1);
    return {makeFillEmptyNull(makeVariable(minSlots[0])), std::move(inputStage)};
}

std::pair<std::vector<std::unique_ptr<sbe::EExpression>>, EvalStage> buildAccumulatorMax(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(buildAccumulatorMinMax("max", std::move(arg)));
    return {std::move(aggs), std::move(inputStage)};
}

std::pair<std::unique_ptr<sbe::EExpression>, EvalStage> buildFinalizeMax(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& maxSlots,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    tassert(5755100,
            str::stream() << "Expected one input slot for finalization of max, got: "
                          << maxSlots.size(),
            maxSlots.size() == 1);
    return {makeFillEmptyNull(makeVariable(maxSlots[0])), std::move(inputStage)};
}


std::pair<std::vector<std::unique_ptr<sbe::EExpression>>, EvalStage> buildAccumulatorFirst(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(makeFunction("first", makeFillEmptyNull(std::move(arg))));
    return {std::move(aggs), std::move(inputStage)};
}

std::pair<std::unique_ptr<sbe::EExpression>, EvalStage> buildFinalizeFirst(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& firstSlots,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    tassert(5755101,
            str::stream() << "Expected one input slot for finalization of first, got: "
                          << firstSlots.size(),
            firstSlots.size() == 1);
    return {makeVariable(firstSlots[0]), std::move(inputStage)};
}

std::pair<std::vector<std::unique_ptr<sbe::EExpression>>, EvalStage> buildAccumulatorLast(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(makeFunction("last", makeFillEmptyNull(std::move(arg))));
    return {std::move(aggs), std::move(inputStage)};
}

std::pair<std::unique_ptr<sbe::EExpression>, EvalStage> buildFinalizeLast(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& lastSlots,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    tassert(5755102,
            str::stream() << "Expected one input slot for finalization of last, got: "
                          << lastSlots.size(),
            lastSlots.size() == 1);
    return {makeVariable(lastSlots[0]), std::move(inputStage)};
}

std::pair<std::vector<std::unique_ptr<sbe::EExpression>>, EvalStage> buildAccumulatorAvg(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;

    // $avg is translated into a sum(..) expression and a count expression.
    aggs.push_back(makeFunction("sum", std::move(arg)));
    aggs.push_back(makeFunction("sum", makeConstant(sbe::value::TypeTags::NumberInt64, 1)));
    return {std::move(aggs), std::move(inputStage)};
}

std::pair<std::unique_ptr<sbe::EExpression>, EvalStage> buildFinalizeAvg(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& aggSlots,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    tassert(5754703,
            str::stream() << "Expected two slots to finalize avg, got: " << aggSlots.size(),
            aggSlots.size() == 2);

    // Takes the two input slots carried in 'aggSlots' where the first slot is a sum expression
    // and the second is a count expression to compute a final division expression.
    return {
        makeBinaryOp(sbe::EPrimBinary::div, makeVariable(aggSlots[0]), makeVariable(aggSlots[1])),
        std::move(inputStage)};
}

std::pair<std::vector<std::unique_ptr<sbe::EExpression>>, EvalStage> buildAccumulatorSum(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(makeFunction("aggDoubleDoubleSum", std::move(arg)));
    return {std::move(aggs), std::move(inputStage)};
}

std::pair<std::unique_ptr<sbe::EExpression>, EvalStage> buildFinalizeSum(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& sumSlots,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    tassert(5755300,
            str::stream() << "Expected one input slot for finalization of sum, got: "
                          << sumSlots.size(),
            sumSlots.size() == 1);
    auto sumFinalize =
        sbe::makeE<sbe::EIf>(generateNullOrMissing(makeVariable(sumSlots[0])),
                             makeConstant(sbe::value::TypeTags::NumberInt32, 0),
                             makeFunction("doubleDoubleSumFinalize", makeVariable(sumSlots[0])));
    return {std::move(sumFinalize), std::move(inputStage)};
}

std::pair<std::vector<std::unique_ptr<sbe::EExpression>>, EvalStage> buildAccumulatorAddToSet(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(makeFunction("addToSet", std::move(arg)));
    return {std::move(aggs), std::move(inputStage)};
}

std::pair<std::unique_ptr<sbe::EExpression>, EvalStage> buildFinalizeAddToSet(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& addToSetSlots,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    tassert(5755001,
            str::stream() << "Expected one slot to finalize addToSet, got: "
                          << addToSetSlots.size(),
            addToSetSlots.size() == 1);

    return {makeVariable(addToSetSlots[0]), std::move(inputStage)};
}

};  // namespace

std::pair<std::unique_ptr<sbe::EExpression>, EvalStage> buildArgument(
    StageBuilderState& state,
    const AccumulationStatement& acc,
    EvalStage stage,
    sbe::value::SlotId inputVar,
    PlanNodeId planNodeId) {
    auto [argExpr, outStage] =
        generateExpression(state, acc.expr.argument.get(), std::move(stage), inputVar, planNodeId);
    return {argExpr.extractExpr(), std::move(outStage)};
}

std::pair<std::vector<std::unique_ptr<sbe::EExpression>>, EvalStage> buildAccumulator(
    StageBuilderState& state,
    const AccumulationStatement& acc,
    EvalStage inputStage,
    std::unique_ptr<sbe::EExpression> inputExpr,
    PlanNodeId planNodeId) {
    using BuildAccumulatorFn =
        std::function<std::pair<std::vector<std::unique_ptr<sbe::EExpression>>, EvalStage>(
            StageBuilderState&,
            const AccumulationExpression&,
            std::unique_ptr<sbe::EExpression>,
            EvalStage,
            PlanNodeId)>;

    static const StringDataMap<BuildAccumulatorFn> kAccumulatorBuilders = {
        {AccumulatorMin::kName, &buildAccumulatorMin},
        {AccumulatorMax::kName, &buildAccumulatorMax},
        {AccumulatorFirst::kName, &buildAccumulatorFirst},
        {AccumulatorLast::kName, &buildAccumulatorLast},
        {AccumulatorAvg::kName, &buildAccumulatorAvg},
        {AccumulatorSum::kName, &buildAccumulatorSum},
        {AccumulatorAddToSet::kName, &buildAccumulatorAddToSet}};

    auto accExprName = acc.expr.name;
    uassert(5754701,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << accExprName,
            kAccumulatorBuilders.find(accExprName) != kAccumulatorBuilders.end());

    return std::invoke(kAccumulatorBuilders.at(accExprName),
                       state,
                       acc.expr,
                       std::move(inputExpr),
                       std::move(inputStage),
                       planNodeId);
}

std::pair<std::unique_ptr<sbe::EExpression>, EvalStage> buildFinalize(
    StageBuilderState& state,
    const AccumulationStatement& acc,
    const sbe::value::SlotVector& aggSlots,
    EvalStage inputStage,
    PlanNodeId planNodeId) {
    using BuildFinalizeFn = std::function<std::pair<std::unique_ptr<sbe::EExpression>, EvalStage>(
        StageBuilderState&,
        const AccumulationExpression&,
        sbe::value::SlotVector,
        EvalStage,
        PlanNodeId)>;

    static const StringDataMap<BuildFinalizeFn> kAccumulatorBuilders = {
        {AccumulatorMin::kName, &buildFinalizeMin},
        {AccumulatorMax::kName, &buildFinalizeMax},
        {AccumulatorFirst::kName, &buildFinalizeFirst},
        {AccumulatorLast::kName, &buildFinalizeLast},
        {AccumulatorAvg::kName, &buildFinalizeAvg},
        {AccumulatorSum::kName, &buildFinalizeSum},
        {AccumulatorAddToSet::kName, &buildFinalizeAddToSet}};

    auto accExprName = acc.expr.name;
    uassert(5754700,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << accExprName,
            kAccumulatorBuilders.find(accExprName) != kAccumulatorBuilders.end());

    return std::invoke(kAccumulatorBuilders.at(accExprName),
                       state,
                       acc.expr,
                       aggSlots,
                       std::move(inputStage),
                       planNodeId);
}
}  // namespace mongo::stage_builder
