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
#include "mongo/db/exec/sbe/accumulator_sum_value_enum.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_for_window_functions.h"
#include "mongo/db/pipeline/accumulator_js_reduce.h"
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/db/query/sbe_stage_builder_accumulator.h"
#include "mongo/db/query/sbe_stage_builder_expression.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"

namespace mongo::stage_builder {
namespace {

std::unique_ptr<sbe::EExpression> wrapMinMaxArg(std::unique_ptr<sbe::EExpression> arg,
                                                sbe::value::FrameIdGenerator& frameIdGenerator) {
    return makeLocalBind(&frameIdGenerator,
                         [](sbe::EVariable input) {
                             return sbe::makeE<sbe::EIf>(
                                 generateNullOrMissing(input),
                                 makeConstant(sbe::value::TypeTags::Nothing, 0),
                                 input.clone());
                         },
                         std::move(arg));
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorMin(
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    if (collatorSlot) {
        aggs.push_back(makeFunction("collMin"_sd,
                                    sbe::makeE<sbe::EVariable>(*collatorSlot),
                                    wrapMinMaxArg(std::move(arg), frameIdGenerator)));
    } else {
        aggs.push_back(makeFunction("min"_sd, wrapMinMaxArg(std::move(arg), frameIdGenerator)));
    }
    return aggs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildCombinePartialAggsMin(
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& inputSlots,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    tassert(7039501,
            "partial agg combiner for $min should have exactly one input slot",
            inputSlots.size() == 1);
    auto arg = makeVariable(inputSlots[0]);
    return buildAccumulatorMin(expr, std::move(arg), collatorSlot, frameIdGenerator);
}

std::unique_ptr<sbe::EExpression> buildFinalizeMin(StageBuilderState& state,
                                                   const AccumulationExpression& expr,
                                                   const sbe::value::SlotVector& minSlots) {
    // We can get away with not building a project stage since there's no finalize step but we
    // will stick the slot into an EVariable in case a $min is one of many group clauses and it
    // can be combined into a final project stage.
    tassert(5754702,
            str::stream() << "Expected one input slot for finalization of min, got: "
                          << minSlots.size(),
            minSlots.size() == 1);
    return makeFillEmptyNull(makeVariable(minSlots[0]));
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorMax(
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    if (collatorSlot) {
        aggs.push_back(makeFunction("collMax"_sd,
                                    sbe::makeE<sbe::EVariable>(*collatorSlot),
                                    wrapMinMaxArg(std::move(arg), frameIdGenerator)));
    } else {
        aggs.push_back(makeFunction("max"_sd, wrapMinMaxArg(std::move(arg), frameIdGenerator)));
    }
    return aggs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildCombinePartialAggsMax(
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& inputSlots,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    tassert(7039502,
            "partial agg combiner for $max should have exactly one input slot",
            inputSlots.size() == 1);
    auto arg = makeVariable(inputSlots[0]);
    return buildAccumulatorMax(expr, std::move(arg), collatorSlot, frameIdGenerator);
}

std::unique_ptr<sbe::EExpression> buildFinalizeMax(StageBuilderState& state,
                                                   const AccumulationExpression& expr,
                                                   const sbe::value::SlotVector& maxSlots) {
    tassert(5755100,
            str::stream() << "Expected one input slot for finalization of max, got: "
                          << maxSlots.size(),
            maxSlots.size() == 1);
    return makeFillEmptyNull(makeVariable(maxSlots[0]));
}


std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorFirst(
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(makeFunction("first", makeFillEmptyNull(std::move(arg))));
    return aggs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildCombinePartialAggsFirst(
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& inputSlots,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    tassert(7039503,
            "partial agg combiner for $first should have exactly one input slot",
            inputSlots.size() == 1);
    auto arg = makeVariable(inputSlots[0]);
    return buildAccumulatorFirst(expr, std::move(arg), collatorSlot, frameIdGenerator);
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorLast(
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(makeFunction("last", makeFillEmptyNull(std::move(arg))));
    return aggs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildCombinePartialAggsLast(
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& inputSlots,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    tassert(7039504,
            "partial agg combiner for $last should have exactly one input slot",
            inputSlots.size() == 1);
    auto arg = makeVariable(inputSlots[0]);
    return buildAccumulatorLast(expr, std::move(arg), collatorSlot, frameIdGenerator);
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorAvg(
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;

    // 'aggDoubleDoubleSum' will ignore non-numeric values automatically.
    aggs.push_back(makeFunction("aggDoubleDoubleSum", arg->clone()));

    // For the counter we need to skip non-numeric values ourselves.
    auto addend = makeLocalBind(&frameIdGenerator,
                                [](sbe::EVariable input) {
                                    return sbe::makeE<sbe::EIf>(
                                        makeBinaryOp(sbe::EPrimBinary::logicOr,
                                                     generateNullOrMissing(input),
                                                     generateNonNumericCheck(input)),
                                        makeConstant(sbe::value::TypeTags::NumberInt64, 0),
                                        makeConstant(sbe::value::TypeTags::NumberInt64, 1));
                                },
                                std::move(arg));
    auto counterExpr = makeFunction("sum", std::move(addend));
    aggs.push_back(std::move(counterExpr));

    return aggs;
}

std::unique_ptr<sbe::EExpression> buildFinalizeAvg(StageBuilderState& state,
                                                   const AccumulationExpression& expr,
                                                   const sbe::value::SlotVector& aggSlots) {
    // Slot 0 contains the accumulated sum, and slot 1 contains the count of summed items.
    tassert(5754703,
            str::stream() << "Expected two slots to finalize avg, got: " << aggSlots.size(),
            aggSlots.size() == 2);

    if (state.needsMerge) {
        // To support the sharding behavior, the mongos splits $group into two separate $group
        // stages one at the mongos-side and the other at the shard-side. This stage builder builds
        // the shard-side plan. The shard-side $avg accumulator is responsible to return the partial
        // avg in the form of {subTotal: val1, count: val2, ps: array_val} when the type of sum is
        // decimal or {subTotal: val1, count: val2, subTotalError: val3, ps: array_val} when the
        // type of sum is non-decimal.
        auto sumResult = makeVariable(aggSlots[0]);
        auto countResult = makeVariable(aggSlots[1]);
        auto partialSumExpr = makeFunction("doubleDoublePartialSumFinalize", sumResult->clone());

        // Existence of 'kDecimalTotal' element in the sum result means the type of sum result is
        // decimal.
        auto ifCondExpr = makeFunction(
            "exists",
            makeFunction("getElement",
                         sumResult->clone(),
                         makeConstant(sbe::value::TypeTags::NumberInt32,
                                      static_cast<int>(AggSumValueElems::kDecimalTotal))));
        // Returns {subTotal: val1, count: val2, ps: array_val} if the type of the sum result is
        // decimal.
        // TODO SERVER-64227 Remove 'subTotal' and 'subTotalError' fields when we branch for 6.1
        // because all nodes in a sharded cluster would use the new data format.
        auto thenExpr = makeNewObjFunction(
            FieldPair{"subTotal"_sd,
                      // 'doubleDoubleSumFinalize' returns the sum, adding decimal
                      // sum and non-decimal sum.
                      makeFunction("doubleDoubleSumFinalize", sumResult->clone())},
            FieldPair{"count"_sd, countResult->clone()},
            FieldPair{"ps"_sd, partialSumExpr->clone()});
        // Returns {subTotal: val1, count: val2: subTotalError: val3, ps: array_val} otherwise.
        auto elseExpr = makeNewObjFunction(
            FieldPair{"subTotal"_sd,
                      makeFunction(
                          "getElement",
                          sumResult->clone(),
                          makeConstant(sbe::value::TypeTags::NumberInt32,
                                       static_cast<int>(AggSumValueElems::kNonDecimalTotalSum)))},
            FieldPair{"count"_sd, countResult->clone()},
            FieldPair{"subTotalError"_sd,
                      makeFunction("getElement",
                                   sumResult->clone(),
                                   makeConstant(sbe::value::TypeTags::NumberInt32,
                                                static_cast<int>(
                                                    AggSumValueElems::kNonDecimalTotalAddend)))},
            FieldPair{"ps"_sd, partialSumExpr->clone()});
        auto partialAvgFinalize =
            sbe::makeE<sbe::EIf>(std::move(ifCondExpr), std::move(thenExpr), std::move(elseExpr));

        return partialAvgFinalize;
    } else {
        // If we've encountered any numeric input, the counter would contain a positive integer.
        // Unlike $sum, when there is no numeric input, $avg should return null.
        auto finalizingExpression = sbe::makeE<sbe::EIf>(
            makeBinaryOp(sbe::EPrimBinary::eq,
                         makeVariable(aggSlots[1]),
                         makeConstant(sbe::value::TypeTags::NumberInt64, 0)),
            makeConstant(sbe::value::TypeTags::Null, 0),
            makeBinaryOp(sbe::EPrimBinary::div,
                         makeFunction("doubleDoubleSumFinalize", makeVariable(aggSlots[0])),
                         makeVariable(aggSlots[1])));

        return finalizingExpression;
    }
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorSum(
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(makeFunction("aggDoubleDoubleSum", std::move(arg)));
    return aggs;
}

std::unique_ptr<sbe::EExpression> buildFinalizeSum(StageBuilderState& state,
                                                   const AccumulationExpression& expr,
                                                   const sbe::value::SlotVector& sumSlots) {
    tassert(5755300,
            str::stream() << "Expected one input slot for finalization of sum, got: "
                          << sumSlots.size(),
            sumSlots.size() == 1);

    if (state.needsMerge) {
        // Serialize the full state of the partial sum result to avoid incorrect results for certain
        // data set which are composed of 'NumberDecimal' values which cancel each other when being
        // summed and other numeric type values which contribute mostly to sum result and a partial
        // sum of some of 'NumberDecimal' values and other numeric type values happen to lose
        // precision because 'NumberDecimal' can't represent the partial sum precisely, or the other
        // way around.
        //
        // For example, [{n: 1e+34}, {n: NumberDecimal("0,1")}, {n: NumberDecimal("0.11")}, {n:
        // -1e+34}].
        //
        // More fundamentally, addition is neither commutative nor associative on computer. So, it's
        // desirable to keep the full state of the partial sum along the way to maintain the result
        // as close to the real truth as possible until all additions are done.
        //
        // This requires changing over-the-wire data format from a shard-side to a merge-side and is
        // incompatible change and is gated with FCV until 5.0 LTS will reach the end of support.
        //
        // TODO SERVER-64227: Remove FCV gating which is unnecessary when we branch for 6.1.
        auto&& fcv = serverGlobalParams.featureCompatibility;
        auto canUseNewPartialResultFormat = fcv.isVersionInitialized() &&
            fcv.isGreaterThanOrEqualTo(multiversion::FeatureCompatibilityVersion::kVersion_6_0);
        if (canUseNewPartialResultFormat) {
            return makeFunction("doubleDoublePartialSumFinalize", makeVariable(sumSlots[0]));
        }

        // To support the sharding behavior, the mongos splits $group into two separate $group
        // stages one at the mongos-side and the other at the shard-side. The shard-side $sum
        // accumulator is responsible to return the partial sum which is mostly same format to the
        // global sum but in the cases of overflowed 'NumberInt32'/'NumberInt64', return a
        // sub-document {subTotal: val1, subTotalError: val2}. The builtin function for $sum
        // ('builtinDoubleDoubleSumFinalize()') returns an 'Array' when there's an overflow. So,
        // only when the return value is 'Array'-typed, we compose the sub-document.
        //
        // TODO SERVER-64227: Remove all following statements and
        // 'doubleDoubleMergeSumFinalize'-related functions when we branch for 6.1.
        auto sumFinalize = makeFunction("doubleDoubleMergeSumFinalize", makeVariable(sumSlots[0]));

        auto partialSumFinalize = makeLocalBind(
            state.frameIdGenerator,
            [](sbe::EVariable input) {
                return sbe::makeE<sbe::EIf>(
                    makeFunction("isArray", input.clone()),
                    makeNewObjFunction(
                        FieldPair{
                            "subTotal"_sd,
                            makeFunction("getElement",
                                         input.clone(),
                                         makeConstant(sbe::value::TypeTags::NumberInt32,
                                                      static_cast<int>(
                                                          sbe::vm::AggPartialSumElems::kTotal)))},
                        FieldPair{
                            "subTotalError"_sd,
                            makeFunction("getElement",
                                         input.clone(),
                                         makeConstant(sbe::value::TypeTags::NumberInt32,
                                                      static_cast<int>(
                                                          sbe::vm::AggPartialSumElems::kError)))}),
                    input.clone());
            },
            std::move(sumFinalize));
        return partialSumFinalize;
    } else {
        return makeFunction("doubleDoubleSumFinalize", makeVariable(sumSlots[0]));
    }
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorAddToSet(
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    const int cap = internalQueryMaxAddToSetBytes.load();
    if (collatorSlot) {
        aggs.push_back(makeFunction(
            "collAddToSetCapped"_sd,
            sbe::makeE<sbe::EVariable>(*collatorSlot),
            std::move(arg),
            makeConstant(sbe::value::TypeTags::NumberInt32, sbe::value::bitcastFrom<int>(cap))));
    } else {
        aggs.push_back(makeFunction(
            "addToSetCapped",
            std::move(arg),
            makeConstant(sbe::value::TypeTags::NumberInt32, sbe::value::bitcastFrom<int>(cap))));
    }
    return aggs;
}

std::unique_ptr<sbe::EExpression> buildFinalizeCappedAccumulator(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& accSlots) {
    tassert(6526500,
            str::stream() << "Expected one input slot for finalization of capped accumulator, got: "
                          << accSlots.size(),
            accSlots.size() == 1);

    // 'accSlots[0]' should contain an array of size two, where the front element is the accumulated
    // values and the back element is their cumulative size in bytes. We just ignore the size
    // because if it exceeded the size cap, we should have thrown an error during accumulation.
    auto pushFinalize =
        makeFunction("getElement",
                     makeVariable(accSlots[0]),
                     makeConstant(sbe::value::TypeTags::NumberInt32,
                                  static_cast<int>(sbe::vm::AggArrayWithSize::kValues)));

    return pushFinalize;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorPush(
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    const int cap = internalQueryMaxPushBytes.load();
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(makeFunction(
        "addToArrayCapped"_sd,
        std::move(arg),
        makeConstant(sbe::value::TypeTags::NumberInt32, sbe::value::bitcastFrom<int>(cap))));
    return aggs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorStdDev(
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(makeFunction("aggStdDev", std::move(arg)));
    return aggs;
}

std::unique_ptr<sbe::EExpression> buildFinalizePartialStdDev(sbe::value::SlotId stdDevSlot) {
    // To support the sharding behavior, the mongos splits $group into two separate $group
    // stages one at the mongos-side and the other at the shard-side. This stage builder builds
    // the shard-side plan. The shard-side $stdDevSamp and $stdDevPop accumulators are responsible
    // to return the partial stddev in the form of {m2: val1, mean: val2, count: val3}.
    auto stdDevResult = makeVariable(stdDevSlot);

    return makeNewObjFunction(
        FieldPair{
            "m2"_sd,
            makeFunction("getElement",
                         stdDevResult->clone(),
                         makeConstant(sbe::value::TypeTags::NumberInt32,
                                      static_cast<int>(sbe::vm::AggStdDevValueElems::kRunningM2)))},
        FieldPair{"mean"_sd,
                  makeFunction(
                      "getElement",
                      stdDevResult->clone(),
                      makeConstant(sbe::value::TypeTags::NumberInt32,
                                   static_cast<int>(sbe::vm::AggStdDevValueElems::kRunningMean)))},
        FieldPair{
            "count"_sd,
            makeFunction("getElement",
                         stdDevResult->clone(),
                         makeConstant(sbe::value::TypeTags::NumberInt32,
                                      static_cast<int>(sbe::vm::AggStdDevValueElems::kCount)))});
}

std::unique_ptr<sbe::EExpression> buildFinalizeStdDevPop(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& stdDevSlots) {
    tassert(5755204,
            str::stream() << "Expected one input slot for finalization of stdDevPop, got: "
                          << stdDevSlots.size(),
            stdDevSlots.size() == 1);

    if (state.needsMerge) {
        return buildFinalizePartialStdDev(stdDevSlots[0]);
    } else {
        auto stdDevPopFinalize = makeFunction("stdDevPopFinalize", makeVariable(stdDevSlots[0]));
        return stdDevPopFinalize;
    }
}

std::unique_ptr<sbe::EExpression> buildFinalizeStdDevSamp(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& stdDevSlots) {
    tassert(5755209,
            str::stream() << "Expected one input slot for finalization of stdDevSamp, got: "
                          << stdDevSlots.size(),
            stdDevSlots.size() == 1);

    if (state.needsMerge) {
        return buildFinalizePartialStdDev(stdDevSlots[0]);
    } else {
        return makeFunction("stdDevSampFinalize", makeVariable(stdDevSlots[0]));
    }
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorMergeObjects(
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;

    auto filterExpr =
        makeLocalBind(&frameIdGenerator,
                      [](sbe::EVariable input) {
                          auto typeCheckExpr =
                              makeBinaryOp(sbe::EPrimBinary::logicOr,
                                           generateNullOrMissing(input),
                                           makeFunction("isObject", input.clone()));
                          return sbe::makeE<sbe::EIf>(
                              std::move(typeCheckExpr),
                              makeFunction("mergeObjects", input.clone()),
                              sbe::makeE<sbe::EFail>(ErrorCodes::Error{5911200},
                                                     "$mergeObjects only supports objects"));
                      },
                      std::move(arg));

    aggs.push_back(std::move(filterExpr));
    return aggs;
}
};  // namespace

std::pair<std::unique_ptr<sbe::EExpression>, EvalStage> buildArgument(
    StageBuilderState& state,
    const AccumulationStatement& acc,
    EvalStage stage,
    boost::optional<sbe::value::SlotId> optionalRootSlot,
    PlanNodeId planNodeId) {
    auto [argExpr, outStage] = generateExpression(
        state, acc.expr.argument.get(), std::move(stage), optionalRootSlot, planNodeId);
    return {argExpr.extractExpr(), std::move(outStage)};
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulator(
    const AccumulationStatement& acc,
    std::unique_ptr<sbe::EExpression> argExpr,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    using BuildAccumulatorFn = std::function<std::vector<std::unique_ptr<sbe::EExpression>>(
        const AccumulationExpression&,
        std::unique_ptr<sbe::EExpression>,
        boost::optional<sbe::value::SlotId>,
        sbe::value::FrameIdGenerator&)>;

    static const StringDataMap<BuildAccumulatorFn> kAccumulatorBuilders = {
        {AccumulatorMin::kName, &buildAccumulatorMin},
        {AccumulatorMax::kName, &buildAccumulatorMax},
        {AccumulatorFirst::kName, &buildAccumulatorFirst},
        {AccumulatorLast::kName, &buildAccumulatorLast},
        {AccumulatorAvg::kName, &buildAccumulatorAvg},
        {AccumulatorAddToSet::kName, &buildAccumulatorAddToSet},
        {AccumulatorSum::kName, &buildAccumulatorSum},
        {AccumulatorPush::kName, &buildAccumulatorPush},
        {AccumulatorMergeObjects::kName, &buildAccumulatorMergeObjects},
        {AccumulatorStdDevPop::kName, &buildAccumulatorStdDev},
        {AccumulatorStdDevSamp::kName, &buildAccumulatorStdDev},
    };

    auto accExprName = acc.expr.name;
    uassert(5754701,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << accExprName,
            kAccumulatorBuilders.find(accExprName) != kAccumulatorBuilders.end());

    return std::invoke(kAccumulatorBuilders.at(accExprName),
                       acc.expr,
                       std::move(argExpr),
                       collatorSlot,
                       frameIdGenerator);
}

std::vector<std::unique_ptr<sbe::EExpression>> buildCombinePartialAggregates(
    const AccumulationStatement& acc,
    const sbe::value::SlotVector& inputSlots,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    using BuildAggCombinerFn = std::function<std::vector<std::unique_ptr<sbe::EExpression>>(
        const AccumulationExpression&,
        const sbe::value::SlotVector&,
        boost::optional<sbe::value::SlotId>,
        sbe::value::FrameIdGenerator&)>;

    static const StringDataMap<BuildAggCombinerFn> kAggCombinerBuilders = {
        {AccumulatorFirst::kName, &buildCombinePartialAggsFirst},
        {AccumulatorLast::kName, &buildCombinePartialAggsLast},
        {AccumulatorMax::kName, &buildCombinePartialAggsMax},
        {AccumulatorMin::kName, &buildCombinePartialAggsMin},
    };

    auto accExprName = acc.expr.name;
    uassert(7039500,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << accExprName,
            kAggCombinerBuilders.find(accExprName) != kAggCombinerBuilders.end());

    return std::invoke(
        kAggCombinerBuilders.at(accExprName), acc.expr, inputSlots, collatorSlot, frameIdGenerator);
}

std::unique_ptr<sbe::EExpression> buildFinalize(StageBuilderState& state,
                                                const AccumulationStatement& acc,
                                                const sbe::value::SlotVector& aggSlots) {
    using BuildFinalizeFn = std::function<std::unique_ptr<sbe::EExpression>(
        StageBuilderState&, const AccumulationExpression&, sbe::value::SlotVector)>;

    static const StringDataMap<BuildFinalizeFn> kAccumulatorBuilders = {
        {AccumulatorMin::kName, &buildFinalizeMin},
        {AccumulatorMax::kName, &buildFinalizeMax},
        {AccumulatorFirst::kName, nullptr},
        {AccumulatorLast::kName, nullptr},
        {AccumulatorAvg::kName, &buildFinalizeAvg},
        {AccumulatorAddToSet::kName, &buildFinalizeCappedAccumulator},
        {AccumulatorSum::kName, &buildFinalizeSum},
        {AccumulatorPush::kName, &buildFinalizeCappedAccumulator},
        {AccumulatorMergeObjects::kName, nullptr},
        {AccumulatorStdDevPop::kName, &buildFinalizeStdDevPop},
        {AccumulatorStdDevSamp::kName, &buildFinalizeStdDevSamp},
    };

    auto accExprName = acc.expr.name;
    uassert(5754700,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << accExprName,
            kAccumulatorBuilders.find(accExprName) != kAccumulatorBuilders.end());

    if (auto fn = kAccumulatorBuilders.at(accExprName); fn) {
        return std::invoke(fn, state, acc.expr, aggSlots);
    } else {
        // nullptr for 'EExpression' signifies that no final project is necessary.
        return nullptr;
    }
}
}  // namespace mongo::stage_builder
