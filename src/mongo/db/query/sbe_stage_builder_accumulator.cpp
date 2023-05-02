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
#include "mongo/db/exec/sbe/values/arith_common.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_for_window_functions.h"
#include "mongo/db/pipeline/accumulator_js_reduce.h"
#include "mongo/db/pipeline/accumulator_multi.h"
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/db/query/sbe_stage_builder_accumulator.h"
#include "mongo/db/query/sbe_stage_builder_expression.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"

namespace mongo::stage_builder {
namespace {

std::unique_ptr<sbe::EExpression> wrapMinMaxArg(std::unique_ptr<sbe::EExpression> arg,
                                                sbe::value::FrameIdGenerator& frameIdGenerator) {
    return makeLocalBind(
        &frameIdGenerator,
        [](sbe::EVariable input) {
            return sbe::makeE<sbe::EIf>(generateNullOrMissing(input),
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
    auto addend = makeLocalBind(
        &frameIdGenerator,
        [](sbe::EVariable input) {
            return sbe::makeE<sbe::EIf>(makeBinaryOp(sbe::EPrimBinary::logicOr,
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

std::vector<std::unique_ptr<sbe::EExpression>> buildCombinePartialAggsAvg(
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& inputSlots,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    tassert(7039539,
            "partial agg combiner for $avg should have exactly two input slots",
            inputSlots.size() == 2);

    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(makeFunction("aggMergeDoubleDoubleSums", makeVariable(inputSlots[0])));
    aggs.push_back(makeFunction("sum", makeVariable(inputSlots[1])));
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
        // avg in the form of {count: val, ps: array_val}.
        auto sumResult = makeVariable(aggSlots[0]);
        auto countResult = makeVariable(aggSlots[1]);
        auto partialSumExpr = makeFunction("doubleDoublePartialSumFinalize", sumResult->clone());

        // Returns {count: val, ps: array_val}.
        auto partialAvgFinalize =
            makeNewObjFunction(FieldPair{countName, countResult->clone()},
                               FieldPair{partialSumName, partialSumExpr->clone()});

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

std::tuple<bool, boost::optional<sbe::value::TypeTags>, boost::optional<sbe::value::Value>>
getCountAddend(const AccumulationExpression& expr) {
    auto constArg = dynamic_cast<ExpressionConstant*>(expr.argument.get());
    if (!constArg) {
        return {false, boost::none, boost::none};
    }

    auto value = constArg->getValue();
    switch (value.getType()) {
        case BSONType::NumberInt:
            return {true,
                    sbe::value::TypeTags::NumberInt32,
                    sbe::value::bitcastFrom<int>(value.getInt())};
        case BSONType::NumberLong:
            return {true,
                    sbe::value::TypeTags::NumberInt64,
                    sbe::value::bitcastFrom<long long>(value.getLong())};
        case BSONType::NumberDouble:
            return {true,
                    sbe::value::TypeTags::NumberDouble,
                    sbe::value::bitcastFrom<double>(value.getDouble())};
        default:
            // 'value' is NumberDecimal type in which case, 'sum' function may not be efficient
            // due to decimal data copying which involves memory allocation. To avoid such
            // inefficiency, does not support NumberDecimal type for this optimization.
            return {false, boost::none, boost::none};
    }
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorSum(
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;

    // Optimize for a count-like accumulator like {$sum: 1}.
    if (auto [isCount, addendTag, addendVal] = getCountAddend(expr); isCount) {
        aggs.push_back(makeFunction("sum", makeConstant(*addendTag, *addendVal)));
        return aggs;
    }

    aggs.push_back(makeFunction("aggDoubleDoubleSum", std::move(arg)));
    return aggs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildCombinePartialAggsSum(
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& inputSlots,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    tassert(7039530,
            "partial agg combiner for $sum should have exactly one input slot",
            inputSlots.size() == 1);
    auto arg = makeVariable(inputSlots[0]);
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;

    // If the user specifies a count-like accumulator like {$sum: 1}, then we optimize the plan to
    // use the simple "sum" accumulator rather than a DoubleDouble summation. Therefore, the partial
    // aggregates are simple sums and we require nothing special to combine multiple DoubleDouble
    // summations.
    if (auto [isCount, _1, _2] = getCountAddend(expr); isCount) {
        aggs.push_back(makeFunction("sum", std::move(arg)));
        return aggs;
    }

    aggs.push_back(makeFunction("aggMergeDoubleDoubleSums", std::move(arg)));
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
        return makeFunction("doubleDoublePartialSumFinalize", makeVariable(sumSlots[0]));
    }

    if (auto [isCount, tag, val] = getCountAddend(expr); isCount) {
        // The accumulation result is a scalar value. So, the final project is not necessary.
        return nullptr;
    }

    return makeFunction("doubleDoubleSumFinalize", makeVariable(sumSlots[0]));
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorAddToSetHelper(
    std::unique_ptr<sbe::EExpression> arg,
    StringData funcName,
    boost::optional<sbe::value::SlotId> collatorSlot,
    StringData funcNameWithCollator) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    const int cap = internalQueryMaxAddToSetBytes.load();
    if (collatorSlot) {
        aggs.push_back(makeFunction(
            funcNameWithCollator,
            sbe::makeE<sbe::EVariable>(*collatorSlot),
            std::move(arg),
            makeConstant(sbe::value::TypeTags::NumberInt32, sbe::value::bitcastFrom<int>(cap))));
    } else {
        aggs.push_back(makeFunction(
            funcName,
            std::move(arg),
            makeConstant(sbe::value::TypeTags::NumberInt32, sbe::value::bitcastFrom<int>(cap))));
    }
    return aggs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorAddToSet(
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    return buildAccumulatorAddToSetHelper(
        std::move(arg), "addToSetCapped"_sd, collatorSlot, "collAddToSetCapped"_sd);
}

std::vector<std::unique_ptr<sbe::EExpression>> buildCombinePartialAggsAddToSet(
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& inputSlots,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    tassert(7039506,
            "partial agg combiner for $addToSet should have exactly one input slot",
            inputSlots.size() == 1);
    auto arg = makeVariable(inputSlots[0]);
    return buildAccumulatorAddToSetHelper(
        std::move(arg), "aggSetUnionCapped"_sd, collatorSlot, "aggCollSetUnionCapped"_sd);
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

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorPushHelper(
    std::unique_ptr<sbe::EExpression> arg, StringData aggFuncName) {
    const int cap = internalQueryMaxPushBytes.load();
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(makeFunction(
        aggFuncName,
        std::move(arg),
        makeConstant(sbe::value::TypeTags::NumberInt32, sbe::value::bitcastFrom<int>(cap))));
    return aggs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorPush(
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    return buildAccumulatorPushHelper(std::move(arg), "addToArrayCapped"_sd);
}

std::vector<std::unique_ptr<sbe::EExpression>> buildCombinePartialAggsPush(
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& inputSlots,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    tassert(7039505,
            "partial agg combiner for $push should have exactly one input slot",
            inputSlots.size() == 1);
    auto arg = makeVariable(inputSlots[0]);
    return buildAccumulatorPushHelper(std::move(arg), "aggConcatArraysCapped"_sd);
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

std::vector<std::unique_ptr<sbe::EExpression>> buildCombinePartialAggsStdDev(
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& inputSlots,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    tassert(7039540,
            "partial agg combiner for stddev should have exactly one input slot",
            inputSlots.size() == 1);
    auto arg = makeVariable(inputSlots[0]);
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(makeFunction("aggMergeStdDevs", std::move(arg)));
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

    auto filterExpr = makeLocalBind(
        &frameIdGenerator,
        [](sbe::EVariable input) {
            auto typeCheckExpr = makeBinaryOp(sbe::EPrimBinary::logicOr,
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

std::vector<std::unique_ptr<sbe::EExpression>> buildCombinePartialAggsMergeObjects(
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& inputSlots,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    tassert(7039507,
            "partial agg combiner for $mergeObjects should have exactly one input slot",
            inputSlots.size() == 1);
    auto arg = makeVariable(inputSlots[0]);
    return buildAccumulatorMergeObjects(expr, std::move(arg), collatorSlot, frameIdGenerator);
}

std::vector<std::unique_ptr<sbe::EExpression>> buildInitializeAccumulatorMulti(
    std::unique_ptr<sbe::EExpression> maxSizeExpr, sbe::value::FrameIdGenerator& frameIdGenerator) {
    // Create an array of four elements [value holder, max size, memory used, memory limit].
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    auto maxAccumulatorBytes = internalQueryTopNAccumulatorBytes.load();
    if (auto* maxSizeConstExpr = maxSizeExpr->as<sbe::EConstant>()) {
        auto [tag, val] = maxSizeConstExpr->getConstant();
        auto [convertOwn, convertTag, convertVal] =
            genericNumConvert(tag, val, sbe::value::TypeTags::NumberInt64);
        uassert(7548606,
                "parameter 'n' must be coercible to a positive 64-bit integer",
                convertTag != sbe::value::TypeTags::Nothing &&
                    static_cast<int64_t>(convertVal) > 0);
        aggs.push_back(
            makeFunction("newArray",
                         makeFunction("newArray"),
                         makeConstant(sbe::value::TypeTags::NumberInt64, 0),
                         makeConstant(convertTag, convertVal),
                         makeConstant(sbe::value::TypeTags::NumberInt32, 0),
                         makeConstant(sbe::value::TypeTags::NumberInt32, maxAccumulatorBytes)));
    } else {
        auto localBind = makeLocalBind(
            &frameIdGenerator,
            [&](sbe::EVariable maxSizeConvertVar) {
                return sbe::makeE<sbe::EIf>(
                    sbe::makeE<sbe::EPrimBinary>(
                        sbe::EPrimBinary::logicAnd,
                        makeFunction("exists", maxSizeConvertVar.clone()),
                        sbe::makeE<sbe::EPrimBinary>(
                            sbe::EPrimBinary::greater,
                            maxSizeConvertVar.clone(),
                            makeConstant(sbe::value::TypeTags::NumberInt64, 0))),
                    makeFunction(
                        "newArray",
                        makeFunction("newArray"),
                        makeConstant(sbe::value::TypeTags::NumberInt64, 0),
                        maxSizeConvertVar.clone(),
                        makeConstant(sbe::value::TypeTags::NumberInt32, 0),
                        makeConstant(sbe::value::TypeTags::NumberInt32, maxAccumulatorBytes)),
                    makeFail(7548607,
                             "parameter 'n' must be coercible to a positive 64-bit integer"));
            },
            sbe::makeE<sbe::ENumericConvert>(std::move(maxSizeExpr),
                                             sbe::value::TypeTags::NumberInt64));
        aggs.push_back(std::move(localBind));
    }
    return aggs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorFirstN(
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(makeFunction("aggFirstN", makeFillEmptyNull(std::move(arg))));
    return aggs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildCombinePartialAggsFirstN(
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& inputSlots,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    uassert(7548608,
            str::stream() << "Expected one input slot for merging $firstN, got: "
                          << inputSlots.size(),
            inputSlots.size() == 1);

    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(makeFunction("aggFirstNMerge", makeVariable(inputSlots[0])));
    return aggs;
}

std::unique_ptr<sbe::EExpression> buildFinalizeFirstN(StageBuilderState& state,
                                                      const AccumulationExpression& expr,
                                                      const sbe::value::SlotVector& inputSlots) {
    uassert(7548609,
            str::stream() << "Expected one input slot for finalization of $firstN, got: "
                          << inputSlots.size(),
            inputSlots.size() == 1);
    return makeFunction("aggFirstNFinalize", makeVariable(inputSlots[0]));
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorLastN(
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(makeFunction("aggLastN", makeFillEmptyNull(std::move(arg))));
    return aggs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildCombinePartialAggsLastN(
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& inputSlots,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    uassert(7548701,
            str::stream() << "Expected one input slot for merging $lastN, got: "
                          << inputSlots.size(),
            inputSlots.size() == 1);

    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(makeFunction("aggLastNMerge", makeVariable(inputSlots[0])));
    return aggs;
}

std::unique_ptr<sbe::EExpression> buildFinalizeLastN(StageBuilderState& state,
                                                     const AccumulationExpression& expr,
                                                     const sbe::value::SlotVector& inputSlots) {
    uassert(7548702,
            str::stream() << "Expected one input slot for finalization of $lastN, got: "
                          << inputSlots.size(),
            inputSlots.size() == 1);
    return makeFunction("aggLastNFinalize", makeVariable(inputSlots[0]));
}

bool isAccumulatorTopN(const AccumulationExpression& expr) {
    return expr.name == AccumulatorTopBottomN<kTop, false /* single */>::getName() ||
        expr.name == AccumulatorTopBottomN<kTop, true /* single */>::getName();
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorTopBottomN(
    const AccumulationExpression& expr,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    auto it = args.find(AccArgs::kTopBottomNKey);
    tassert(5807009,
            str::stream() << "Accumulator " << expr.name << " expects a '"
                          << AccArgs::kTopBottomNKey << "' argument",
            it != args.end());
    auto key = std::move(it->second);

    it = args.find(AccArgs::kTopBottomNValue);
    tassert(5807010,
            str::stream() << "Accumulator " << expr.name << " expects a '"
                          << AccArgs::kTopBottomNValue << "' argument",
            it != args.end());
    auto value = std::move(it->second);

    it = args.find(AccArgs::kTopBottomNSortSpec);
    tassert(5807021,
            str::stream() << "Accumulator " << expr.name << " expects a '"
                          << AccArgs::kTopBottomNSortSpec << "' argument",
            it != args.end());
    auto sortSpec = std::move(it->second);

    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(makeFunction(isAccumulatorTopN(expr) ? "aggTopN" : "aggBottomN",
                                std::move(key),
                                std::move(value),
                                std::move(sortSpec)));
    return aggs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildCombinePartialTopBottomN(
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& inputSlots,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    tassert(5807011,
            str::stream() << "Expected one input slot for merging " << expr.name
                          << ", got: " << inputSlots.size(),
            inputSlots.size() == 1);

    auto it = args.find(AccArgs::kTopBottomNSortSpec);
    tassert(5807022,
            str::stream() << "Accumulator " << expr.name << " expects a '"
                          << AccArgs::kTopBottomNSortSpec << "' argument",
            it != args.end());
    auto sortSpec = std::move(it->second);

    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(makeFunction(isAccumulatorTopN(expr) ? "aggTopNMerge" : "aggBottomNMerge",
                                makeVariable(inputSlots[0]),
                                std::move(sortSpec)));
    return aggs;
}

std::unique_ptr<sbe::EExpression> buildFinalizeTopBottomNImpl(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& inputSlots,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator,
    bool single) {
    tassert(5807012,
            str::stream() << "Expected one input slot for finalization of " << expr.name
                          << ", got: " << inputSlots.size(),
            inputSlots.size() == 1);
    auto inputVar = makeVariable(inputSlots[0]);

    auto it = args.find(AccArgs::kTopBottomNSortSpec);
    tassert(5807023,
            str::stream() << "Accumulator " << expr.name << " expects a '"
                          << AccArgs::kTopBottomNSortSpec << "' argument",
            it != args.end());
    auto sortSpec = std::move(it->second);

    if (state.needsMerge) {
        // When the data will be merged, the heap itself doesn't need to be sorted since the merging
        // code will handle the sorting.
        auto heapExpr =
            makeFunction("getElement",
                         inputVar->clone(),
                         makeConstant(sbe::value::TypeTags::NumberInt32,
                                      static_cast<int>(sbe::vm::AggMultiElems::kInternalArr)));
        auto lambdaFrameId = frameIdGenerator.generate();
        auto pairVar = makeVariable(lambdaFrameId, 0);
        auto lambdaExpr = sbe::makeE<sbe::ELocalLambda>(
            lambdaFrameId,
            makeNewObjFunction(
                FieldPair{AccumulatorN::kFieldNameGeneratedSortKey,
                          makeFunction("getElement",
                                       pairVar->clone(),
                                       makeConstant(sbe::value::TypeTags::NumberInt32, 0))},
                FieldPair{AccumulatorN::kFieldNameOutput,
                          makeFunction("getElement",
                                       pairVar->clone(),
                                       makeConstant(sbe::value::TypeTags::NumberInt32, 1))}));
        // Convert the array pair representation [key, output] to an object format that the merging
        // code expects.
        return makeFunction("traverseP",
                            std::move(heapExpr),
                            std::move(lambdaExpr),
                            makeConstant(sbe::value::TypeTags::NumberInt32, 1));
    } else {
        auto finalExpr =
            makeFunction(isAccumulatorTopN(expr) ? "aggTopNFinalize" : "aggBottomNFinalize",
                         inputVar->clone(),
                         std::move(sortSpec));
        if (single) {
            finalExpr = makeFunction("getElement",
                                     std::move(finalExpr),
                                     makeConstant(sbe::value::TypeTags::NumberInt32, 0));
        }
        return finalExpr;
    }
}

std::unique_ptr<sbe::EExpression> buildFinalizeTopBottomN(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& inputSlots,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    return buildFinalizeTopBottomNImpl(state,
                                       expr,
                                       inputSlots,
                                       std::move(args),
                                       collatorSlot,
                                       frameIdGenerator,
                                       false /* single */);
}

std::unique_ptr<sbe::EExpression> buildFinalizeTopBottom(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& inputSlots,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    return buildFinalizeTopBottomNImpl(state,
                                       expr,
                                       inputSlots,
                                       std::move(args),
                                       collatorSlot,
                                       frameIdGenerator,
                                       true /* single */);
}

template <int N>
std::vector<std::unique_ptr<sbe::EExpression>> emptyInitializer(
    std::unique_ptr<sbe::EExpression> maxSizeExpr, sbe::value::FrameIdGenerator& frameIdGenerator) {
    return std::vector<std::unique_ptr<sbe::EExpression>>{N};
}
}  // namespace

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
        {AccumulatorFirstN::kName, &buildAccumulatorFirstN},
        {AccumulatorLastN::kName, &buildAccumulatorLastN},
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

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulator(
    const AccumulationStatement& acc,
    StringDataMap<std::unique_ptr<sbe::EExpression>> argExprs,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    using BuildAccumulatorFn = std::function<std::vector<std::unique_ptr<sbe::EExpression>>(
        const AccumulationExpression&,
        StringDataMap<std::unique_ptr<sbe::EExpression>>,
        boost::optional<sbe::value::SlotId>,
        sbe::value::FrameIdGenerator&)>;

    static const StringDataMap<BuildAccumulatorFn> kAccumulatorBuilders = {
        {AccumulatorTopBottomN<kTop, true /* single */>::getName(), &buildAccumulatorTopBottomN},
        {AccumulatorTopBottomN<kBottom, true /* single */>::getName(), &buildAccumulatorTopBottomN},
        {AccumulatorTopBottomN<kTop, false /* single */>::getName(), &buildAccumulatorTopBottomN},
        {AccumulatorTopBottomN<kBottom, false /* single */>::getName(),
         &buildAccumulatorTopBottomN},
    };

    auto accExprName = acc.expr.name;
    uassert(5807017,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << accExprName,
            kAccumulatorBuilders.find(accExprName) != kAccumulatorBuilders.end());

    return std::invoke(kAccumulatorBuilders.at(accExprName),
                       acc.expr,
                       std::move(argExprs),
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
        {AccumulatorAddToSet::kName, &buildCombinePartialAggsAddToSet},
        {AccumulatorAvg::kName, &buildCombinePartialAggsAvg},
        {AccumulatorFirst::kName, &buildCombinePartialAggsFirst},
        {AccumulatorLast::kName, &buildCombinePartialAggsLast},
        {AccumulatorMax::kName, &buildCombinePartialAggsMax},
        {AccumulatorMergeObjects::kName, &buildCombinePartialAggsMergeObjects},
        {AccumulatorMin::kName, &buildCombinePartialAggsMin},
        {AccumulatorPush::kName, &buildCombinePartialAggsPush},
        {AccumulatorStdDevPop::kName, &buildCombinePartialAggsStdDev},
        {AccumulatorStdDevSamp::kName, &buildCombinePartialAggsStdDev},
        {AccumulatorSum::kName, &buildCombinePartialAggsSum},
        {AccumulatorFirstN::kName, &buildCombinePartialAggsFirstN},
        {AccumulatorLastN::kName, &buildCombinePartialAggsLastN},
    };

    auto accExprName = acc.expr.name;
    uassert(7039500,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << accExprName,
            kAggCombinerBuilders.find(accExprName) != kAggCombinerBuilders.end());

    return std::invoke(
        kAggCombinerBuilders.at(accExprName), acc.expr, inputSlots, collatorSlot, frameIdGenerator);
}

std::vector<std::unique_ptr<sbe::EExpression>> buildCombinePartialAggregates(
    const AccumulationStatement& acc,
    const sbe::value::SlotVector& inputSlots,
    StringDataMap<std::unique_ptr<sbe::EExpression>> argExprs,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    using BuildAggCombinerFn = std::function<std::vector<std::unique_ptr<sbe::EExpression>>(
        const AccumulationExpression&,
        const sbe::value::SlotVector&,
        StringDataMap<std::unique_ptr<sbe::EExpression>>,
        boost::optional<sbe::value::SlotId>,
        sbe::value::FrameIdGenerator&)>;

    static const StringDataMap<BuildAggCombinerFn> kAggCombinerBuilders = {
        {AccumulatorTopBottomN<kTop, true /* single */>::getName(), &buildCombinePartialTopBottomN},
        {AccumulatorTopBottomN<kBottom, true /* single */>::getName(),
         &buildCombinePartialTopBottomN},
        {AccumulatorTopBottomN<kTop, false /* single */>::getName(),
         &buildCombinePartialTopBottomN},
        {AccumulatorTopBottomN<kBottom, false /* single */>::getName(),
         &buildCombinePartialTopBottomN},
    };

    auto accExprName = acc.expr.name;
    uassert(5807019,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << accExprName,
            kAggCombinerBuilders.find(accExprName) != kAggCombinerBuilders.end());

    return std::invoke(kAggCombinerBuilders.at(accExprName),
                       acc.expr,
                       inputSlots,
                       std::move(argExprs),
                       collatorSlot,
                       frameIdGenerator);
}

std::unique_ptr<sbe::EExpression> buildFinalize(StageBuilderState& state,
                                                const AccumulationStatement& acc,
                                                const sbe::value::SlotVector& aggSlots,
                                                boost::optional<sbe::value::SlotId> collatorSlot,
                                                sbe::value::FrameIdGenerator& frameIdGenerator) {
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
        {AccumulatorFirstN::kName, &buildFinalizeFirstN},
        {AccumulatorLastN::kName, &buildFinalizeLastN},
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

std::unique_ptr<sbe::EExpression> buildFinalize(
    StageBuilderState& state,
    const AccumulationStatement& acc,
    const sbe::value::SlotVector& aggSlots,
    StringDataMap<std::unique_ptr<sbe::EExpression>> argExprs,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    using BuildFinalizeFn = std::function<std::unique_ptr<sbe::EExpression>(
        StageBuilderState&,
        const AccumulationExpression&,
        sbe::value::SlotVector,
        StringDataMap<std::unique_ptr<sbe::EExpression>>,
        boost::optional<sbe::value::SlotId>,
        sbe::value::FrameIdGenerator&)>;

    static const StringDataMap<BuildFinalizeFn> kAccumulatorBuilders = {
        {AccumulatorTopBottomN<kTop, true /* single */>::getName(), &buildFinalizeTopBottom},
        {AccumulatorTopBottomN<kBottom, true /* single */>::getName(), &buildFinalizeTopBottom},
        {AccumulatorTopBottomN<kTop, false /* single */>::getName(), &buildFinalizeTopBottomN},
        {AccumulatorTopBottomN<kBottom, false /* single */>::getName(), &buildFinalizeTopBottomN},
    };

    auto accExprName = acc.expr.name;
    uassert(5807020,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << accExprName,
            kAccumulatorBuilders.find(accExprName) != kAccumulatorBuilders.end());

    return std::invoke(kAccumulatorBuilders.at(accExprName),
                       state,
                       acc.expr,
                       aggSlots,
                       std::move(argExprs),
                       collatorSlot,
                       frameIdGenerator);
}

std::vector<std::unique_ptr<sbe::EExpression>> buildInitialize(
    const AccumulationStatement& acc,
    std::unique_ptr<sbe::EExpression> initExpr,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    using BuildInitializeFn = std::function<std::vector<std::unique_ptr<sbe::EExpression>>(
        std::unique_ptr<sbe::EExpression>, sbe::value::FrameIdGenerator&)>;

    static const StringDataMap<BuildInitializeFn> kAccumulatorBuilders = {
        {AccumulatorMin::kName, &emptyInitializer<1>},
        {AccumulatorMax::kName, &emptyInitializer<1>},
        {AccumulatorFirst::kName, &emptyInitializer<1>},
        {AccumulatorLast::kName, &emptyInitializer<1>},
        {AccumulatorAvg::kName, &emptyInitializer<2>},
        {AccumulatorAddToSet::kName, &emptyInitializer<1>},
        {AccumulatorSum::kName, &emptyInitializer<1>},
        {AccumulatorPush::kName, &emptyInitializer<1>},
        {AccumulatorMergeObjects::kName, &emptyInitializer<1>},
        {AccumulatorStdDevPop::kName, &emptyInitializer<1>},
        {AccumulatorStdDevSamp::kName, &emptyInitializer<1>},
        {AccumulatorFirstN::kName, &buildInitializeAccumulatorMulti},
        {AccumulatorLastN::kName, &buildInitializeAccumulatorMulti},
        {AccumulatorTopBottomN<kTop, true /* single */>::getName(),
         &buildInitializeAccumulatorMulti},
        {AccumulatorTopBottomN<kBottom, true /* single */>::getName(),
         &buildInitializeAccumulatorMulti},
        {AccumulatorTopBottomN<kTop, false /* single */>::getName(),
         &buildInitializeAccumulatorMulti},
        {AccumulatorTopBottomN<kBottom, false /* single */>::getName(),
         &buildInitializeAccumulatorMulti},
    };

    auto accExprName = acc.expr.name;
    uassert(7567300,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << accExprName,
            kAccumulatorBuilders.find(accExprName) != kAccumulatorBuilders.end());

    return std::invoke(kAccumulatorBuilders.at(accExprName), std::move(initExpr), frameIdGenerator);
}
}  // namespace mongo::stage_builder
