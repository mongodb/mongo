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

#include <cstdint>
#include <functional>
#include <tuple>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/sbe/accumulator_sum_value_enum.h"
#include "mongo/db/exec/sbe/values/arith_common.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_for_window_functions.h"
#include "mongo/db/pipeline/accumulator_multi.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/sbe_stage_builder_accumulator.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo::stage_builder {
namespace {

std::unique_ptr<sbe::EExpression> wrapMinMaxArg(std::unique_ptr<sbe::EExpression> arg,
                                                sbe::value::FrameIdGenerator& frameIdGenerator) {
    return makeLocalBind(
        &frameIdGenerator,
        [](sbe::EVariable input) {
            return sbe::makeE<sbe::EIf>(
                generateNullOrMissing(input), makeNothingConstant(), input.clone());
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
                                                   const sbe::value::SlotVector& minSlots,
                                                   boost::optional<sbe::value::SlotId> collatorSlot,
                                                   sbe::value::FrameIdGenerator& frameIdGenerator) {
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
                                                   const sbe::value::SlotVector& maxSlots,
                                                   boost::optional<sbe::value::SlotId> collatorSlot,
                                                   sbe::value::FrameIdGenerator& frameIdGenerator) {
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
                                        makeInt64Constant(0),
                                        makeInt64Constant(1));
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
                                                   const sbe::value::SlotVector& aggSlots,
                                                   boost::optional<sbe::value::SlotId> collatorSlot,
                                                   sbe::value::FrameIdGenerator& frameIdGenerator) {
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
            makeBinaryOp(sbe::EPrimBinary::eq, makeVariable(aggSlots[1]), makeInt64Constant(0)),
            makeNullConstant(),
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
                                                   const sbe::value::SlotVector& sumSlots,
                                                   boost::optional<sbe::value::SlotId> collatorSlot,
                                                   sbe::value::FrameIdGenerator& frameIdGenerator) {
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
        aggs.push_back(makeFunction(funcNameWithCollator,
                                    sbe::makeE<sbe::EVariable>(*collatorSlot),
                                    std::move(arg),
                                    makeInt32Constant(cap)));
    } else {
        aggs.push_back(makeFunction(funcName, std::move(arg), makeInt32Constant(cap)));
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
    const sbe::value::SlotVector& accSlots,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
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
                     makeInt32Constant(static_cast<int>(sbe::vm::AggArrayWithSize::kValues)));

    return pushFinalize;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorPushHelper(
    std::unique_ptr<sbe::EExpression> arg, StringData aggFuncName) {
    const int cap = internalQueryMaxPushBytes.load();
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(makeFunction(aggFuncName, std::move(arg), makeInt32Constant(cap)));
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
        FieldPair{"m2"_sd,
                  makeFunction("getElement",
                               stdDevResult->clone(),
                               makeInt32Constant(
                                   static_cast<int>(sbe::vm::AggStdDevValueElems::kRunningM2)))},
        FieldPair{"mean"_sd,
                  makeFunction("getElement",
                               stdDevResult->clone(),
                               makeInt32Constant(
                                   static_cast<int>(sbe::vm::AggStdDevValueElems::kRunningMean)))},
        FieldPair{"count"_sd,
                  makeFunction(
                      "getElement",
                      stdDevResult->clone(),
                      makeInt32Constant(static_cast<int>(sbe::vm::AggStdDevValueElems::kCount)))});
}

std::unique_ptr<sbe::EExpression> buildFinalizeStdDevPop(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& stdDevSlots,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
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
    const sbe::value::SlotVector& stdDevSlots,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
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
    StringDataMap<std::unique_ptr<sbe::EExpression>> argExprs,
    sbe::value::FrameIdGenerator& frameIdGenerator) {

    auto it = argExprs.find(AccArgs::kMaxSize);
    tassert(8070612,
            str::stream() << "Expected a '" << AccArgs::kMaxSize << "' argument",
            it != argExprs.end());
    auto maxSizeExpr = std::move(it->second);

    it = argExprs.find(AccArgs::kIsGroupAccum);
    tassert(8070613,
            str::stream() << "Expected a '" << AccArgs::kIsGroupAccum << "' argument",
            it != argExprs.end());
    auto isGroupAccumExpr = std::move(it->second);

    // Create an array of four elements [value holder, max size, memory used, memory limit,
    // isGroupAccum].
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
        aggs.push_back(makeFunction("newArray",
                                    makeFunction("newArray"),
                                    makeInt64Constant(0),
                                    makeConstant(convertTag, convertVal),
                                    makeInt32Constant(0),
                                    makeInt32Constant(maxAccumulatorBytes),
                                    std::move(isGroupAccumExpr)));
    } else {
        auto localBind = makeLocalBind(
            &frameIdGenerator,
            [&](sbe::EVariable maxSizeConvertVar) {
                return sbe::makeE<sbe::EIf>(
                    sbe::makeE<sbe::EPrimBinary>(
                        sbe::EPrimBinary::logicAnd,
                        makeFunction("exists", maxSizeConvertVar.clone()),
                        sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::greater,
                                                     maxSizeConvertVar.clone(),
                                                     makeInt64Constant(0))),
                    makeFunction("newArray",
                                 makeFunction("newArray"),
                                 makeInt64Constant(0),
                                 maxSizeConvertVar.clone(),
                                 makeInt32Constant(0),
                                 makeInt32Constant(maxAccumulatorBytes),
                                 std::move(isGroupAccumExpr)),
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
    aggs.push_back(makeLocalBind(
        &frameIdGenerator,
        [&](sbe::EVariable accumulatorState) {
            return sbe::makeE<sbe::EIf>(
                makeFunction("aggFirstNNeedsMoreInput", accumulatorState.clone()),
                makeFunction(
                    "aggFirstN",
                    makeMoveVariable(*accumulatorState.getFrameId(), accumulatorState.getSlotId()),
                    makeFillEmptyNull(std::move(arg))),
                makeMoveVariable(*accumulatorState.getFrameId(), accumulatorState.getSlotId()));
        },
        makeFunction("aggState")));
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

std::unique_ptr<sbe::EExpression> buildFinalizeFirstN(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& inputSlots,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
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

std::unique_ptr<sbe::EExpression> buildFinalizeLastN(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& inputSlots,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
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
                         makeInt32Constant(static_cast<int>(sbe::vm::AggMultiElems::kInternalArr)));
        auto lambdaFrameId = frameIdGenerator.generate();
        auto pairVar = makeVariable(lambdaFrameId, 0);
        auto lambdaExpr = sbe::makeE<sbe::ELocalLambda>(
            lambdaFrameId,
            makeNewObjFunction(
                FieldPair{AccumulatorN::kFieldNameGeneratedSortKey,
                          makeFunction("getElement", pairVar->clone(), makeInt32Constant(0))},
                FieldPair{AccumulatorN::kFieldNameOutput,
                          makeFunction("getElement", pairVar->clone(), makeInt32Constant(1))}));
        // Convert the array pair representation [key, output] to an object format that the merging
        // code expects.
        return makeFunction(
            "traverseP", std::move(heapExpr), std::move(lambdaExpr), makeInt32Constant(1));
    } else {
        auto finalExpr =
            makeFunction(isAccumulatorTopN(expr) ? "aggTopNFinalize" : "aggBottomNFinalize",
                         inputVar->clone(),
                         std::move(sortSpec));
        if (single) {
            finalExpr = makeFunction("getElement", std::move(finalExpr), makeInt32Constant(0));
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

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorMinMaxN(
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    auto aggExprName = expr.name == AccumulatorMaxN::kName ? "aggMaxN" : "aggMinN";
    if (collatorSlot) {
        aggs.push_back(
            makeFunction(std::move(aggExprName), std::move(arg), makeVariable(*collatorSlot)));

    } else {
        aggs.push_back(makeFunction(std::move(aggExprName), std::move(arg)));
    }
    return aggs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildCombinePartialAggsMinMaxN(
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& inputSlots,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    uassert(7548808,
            str::stream() << "Expected one input slot for merging, got: " << inputSlots.size(),
            inputSlots.size() == 1);

    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    auto aggExprName = expr.name == AccumulatorMaxN::kName ? "aggMaxNMerge" : "aggMinNMerge";
    if (collatorSlot) {
        aggs.push_back(makeFunction(
            std::move(aggExprName), makeVariable(inputSlots[0]), makeVariable(*collatorSlot)));
    } else {
        aggs.push_back(makeFunction(std::move(aggExprName), makeVariable(inputSlots[0])));
    }
    return aggs;
}

std::unique_ptr<sbe::EExpression> buildFinalizeMinMaxN(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& inputSlots,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    uassert(7548809,
            str::stream() << "Expected one input slot for finalization, got: " << inputSlots.size(),
            inputSlots.size() == 1);
    auto aggExprName = expr.name == AccumulatorMaxN::kName ? "aggMaxNFinalize" : "aggMinNFinalize";
    if (collatorSlot) {
        return makeFunction(
            std::move(aggExprName), makeVariable(inputSlots[0]), makeVariable(*collatorSlot));
    } else {
        return makeFunction(std::move(aggExprName), makeVariable(inputSlots[0]));
    }
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorCovariance(
    const AccumulationExpression& expr,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    tassert(7820808, "Incorrect number of arguments", args.size() == 2);

    auto it = args.find(AccArgs::kCovarianceX);
    tassert(7820809,
            str::stream() << "Window function expects '" << AccArgs::kCovarianceX << "' argument",
            it != args.end());
    auto argX = std::move(it->second);

    it = args.find(AccArgs::kCovarianceY);
    tassert(7820810,
            str::stream() << "Window function expects '" << AccArgs::kCovarianceY << "' argument",
            it != args.end());
    auto argY = std::move(it->second);

    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(makeFunction("aggCovarianceAdd", std::move(argX), std::move(argY)));
    return exprs;
}

std::unique_ptr<sbe::EExpression> buildFinalizeCovarianceSamp(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& slots,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    tassert(7820814, "Incorrect number of arguments", slots.size() == 1);
    sbe::EExpression::Vector exprs;
    for (auto slot : slots) {
        exprs.push_back(makeVariable(slot));
    }
    return makeE<sbe::EFunction>("aggCovarianceSampFinalize", std::move(exprs));
}

std::unique_ptr<sbe::EExpression> buildFinalizeCovariancePop(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& slots,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    tassert(7820815, "Incorrect number of arguments", slots.size() == 1);
    sbe::EExpression::Vector exprs;
    for (auto slot : slots) {
        exprs.push_back(makeVariable(slot));
    }
    return makeE<sbe::EFunction>("aggCovariancePopFinalize", std::move(exprs));
}

std::vector<std::unique_ptr<sbe::EExpression>> buildInitializeExpMovingAvg(
    std::unique_ptr<sbe::EExpression> alphaExpr, sbe::value::FrameIdGenerator& frameIdGenerator) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(makeFunction(
        "newArray", makeNullConstant(), std::move(alphaExpr), makeBoolConstant(false)));
    return aggs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorExpMovingAvg(
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(makeFunction("aggExpMovingAvg", std::move(arg)));
    return exprs;
}

std::unique_ptr<sbe::EExpression> buildFinalizeExpMovingAvg(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& slots,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    tassert(7996802, "Incorrect number of arguments", slots.size() == 1);
    return makeFunction("aggExpMovingAvgFinalize", makeVariable(slots[0]));
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorLocf(
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(makeLocalBind(
        &frameIdGenerator,
        [](sbe::EVariable input) {
            return sbe::makeE<sbe::EIf>(
                generateNullOrMissing(input), makeFunction("aggState"), input.clone());
        },
        std::move(arg)));
    return exprs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorDocumentNumber(
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(makeFunction("sum", makeInt64Constant(1)));
    return exprs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorRank(
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    if (collatorSlot) {
        exprs.push_back(makeFunction("aggRankColl", std::move(arg), makeVariable(*collatorSlot)));
    } else {
        exprs.push_back(makeFunction("aggRank", std::move(arg)));
    }
    return exprs;
}

std::unique_ptr<sbe::EExpression> buildFinalizeRank(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& slots,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    tassert(7996805, "Incorrect number of arguments", slots.size() == 1);
    return makeFunction("aggRankFinalize", makeVariable(slots[0]));
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorDenseRank(
    const AccumulationExpression& expr,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    if (collatorSlot) {
        exprs.push_back(
            makeFunction("aggDenseRankColl", std::move(arg), makeVariable(*collatorSlot)));
    } else {
        exprs.push_back(makeFunction("aggDenseRank", std::move(arg)));
    }
    return exprs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildInitializeIntegral(
    std::unique_ptr<sbe::EExpression> unitExpr, sbe::value::FrameIdGenerator& frameIdGenerator) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(makeFunction("aggIntegralInit", std::move(unitExpr), makeBoolConstant(true)));
    return aggs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorIntegral(
    const AccumulationExpression& expr,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    tassert(7996806, "Incorrect number of arguments", args.size() == 2);

    auto it = args.find(AccArgs::kInput);
    tassert(7996807,
            str::stream() << "Window function expects '" << AccArgs::kInput << "' argument",
            it != args.end());
    auto input = std::move(it->second);

    it = args.find(AccArgs::kSortBy);
    tassert(7996808,
            str::stream() << "Window function expects '" << AccArgs::kSortBy << "' argument",
            it != args.end());
    auto sortBy = std::move(it->second);

    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(makeFunction("aggIntegralAdd", std::move(input), std::move(sortBy)));
    return exprs;
}

std::unique_ptr<sbe::EExpression> buildFinalizeIntegral(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& slots,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    tassert(7996809, "Incorrect number of arguments", slots.size() == 1);
    return makeFunction("aggIntegralFinalize", makeVariable(slots[0]));
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorDerivative(
    const AccumulationExpression& expr,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(nullptr);
    return exprs;
}

std::unique_ptr<sbe::EExpression> buildFinalizeDerivative(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& slots,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    auto it = args.find(AccArgs::kUnit);
    tassert(7993403,
            str::stream() << "Window function expects '" << AccArgs::kUnit << "' argument",
            it != args.end());
    auto unit = std::move(it->second);

    it = args.find(AccArgs::kDerivativeInputFirst);
    tassert(7993404,
            str::stream() << "Window function expects '" << AccArgs::kDerivativeInputFirst
                          << "' argument",
            it != args.end());
    auto inputFirst = std::move(it->second);

    it = args.find(AccArgs::kDerivativeSortByFirst);
    tassert(7993405,
            str::stream() << "Window function expects '" << AccArgs::kDerivativeSortByFirst
                          << "' argument",
            it != args.end());
    auto sortByFirst = std::move(it->second);

    it = args.find(AccArgs::kDerivativeInputLast);
    tassert(7993406,
            str::stream() << "Window function expects '" << AccArgs::kDerivativeInputLast
                          << "' argument",
            it != args.end());
    auto inputLast = std::move(it->second);

    it = args.find(AccArgs::kDerivativeSortByLast);
    tassert(7993407,
            str::stream() << "Window function expects '" << AccArgs::kDerivativeSortByLast
                          << "' argument",
            it != args.end());
    auto sortByLast = std::move(it->second);

    return makeFunction("aggDerivativeFinalize",
                        std::move(unit),
                        std::move(inputFirst),
                        std::move(sortByFirst),
                        std::move(inputLast),
                        std::move(sortByLast));
}

std::vector<std::unique_ptr<sbe::EExpression>> buildInitializeLinearFill(
    std::unique_ptr<sbe::EExpression> unitExpr, sbe::value::FrameIdGenerator& frameIdGenerator) {
    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(makeFunction("newArray",
                                makeNullConstant(),
                                makeNullConstant(),
                                makeNullConstant(),
                                makeNullConstant(),
                                makeNullConstant(),
                                makeInt64Constant(0)));
    return aggs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorLinearFill(
    const AccumulationExpression& expr,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    tassert(7971210, "Incorrect number of arguments", args.size() == 2);

    auto it = args.find(AccArgs::kInput);
    tassert(7971211,
            str::stream() << "Window function expects '" << AccArgs::kInput << "' argument",
            it != args.end());
    auto input = std::move(it->second);

    it = args.find(AccArgs::kSortBy);
    tassert(7971212,
            str::stream() << "Window function expects '" << AccArgs::kSortBy << "' argument",
            it != args.end());
    auto sortBy = std::move(it->second);

    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(
        makeFunction("aggLinearFillAdd", makeFillEmptyNull(std::move(input)), std::move(sortBy)));
    return exprs;
}

std::unique_ptr<sbe::EExpression> buildFinalizeLinearFill(
    StageBuilderState& state,
    const AccumulationExpression& expr,
    const sbe::value::SlotVector& inputSlots,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    tassert(7971213,
            str::stream() << "Expected one input slot for finalization of " << expr.name
                          << ", got: " << inputSlots.size(),
            inputSlots.size() == 1);
    auto inputVar = makeVariable(inputSlots[0]);

    auto it = args.find(AccArgs::kSortBy);
    tassert(7971214,
            str::stream() << "Window function expects '" << AccArgs::kSortBy << "' argument",
            it != args.end());
    auto sortBy = std::move(it->second);

    return makeFunction("aggLinearFillFinalize", std::move(inputVar), std::move(sortBy));
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
        {AccumulatorMaxN::kName, &buildAccumulatorMinMaxN},
        {AccumulatorMinN::kName, &buildAccumulatorMinMaxN},
        {AccumulatorExpMovingAvg::kName, &buildAccumulatorExpMovingAvg},
        {AccumulatorLocf::kName, &buildAccumulatorLocf},
        {AccumulatorDocumentNumber::kName, &buildAccumulatorDocumentNumber},
        {AccumulatorRank::kName, &buildAccumulatorRank},
        {AccumulatorDenseRank::kName, &buildAccumulatorDenseRank},
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
        {AccumulatorCovarianceSamp::kName, &buildAccumulatorCovariance},
        {AccumulatorCovariancePop::kName, &buildAccumulatorCovariance},
        {AccumulatorIntegral::kName, &buildAccumulatorIntegral},
        {window_function::ExpressionDerivative::kName, &buildAccumulatorDerivative},
        {window_function::ExpressionLinearFill::kName, &buildAccumulatorLinearFill},
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
        {AccumulatorMaxN::kName, &buildCombinePartialAggsMinMaxN},
        {AccumulatorMinN::kName, &buildCombinePartialAggsMinMaxN},
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
    using BuildFinalizeFn =
        std::function<std::unique_ptr<sbe::EExpression>(StageBuilderState&,
                                                        const AccumulationExpression&,
                                                        sbe::value::SlotVector,
                                                        boost::optional<sbe::value::SlotId>,
                                                        sbe::value::FrameIdGenerator&)>;

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
        {AccumulatorMaxN::kName, &buildFinalizeMinMaxN},
        {AccumulatorMinN::kName, &buildFinalizeMinMaxN},
        {AccumulatorCovarianceSamp::kName, &buildFinalizeCovarianceSamp},
        {AccumulatorCovariancePop::kName, &buildFinalizeCovariancePop},
        {AccumulatorExpMovingAvg::kName, &buildFinalizeExpMovingAvg},
        {AccumulatorRank::kName, &buildFinalizeRank},
        {AccumulatorDenseRank::kName, &buildFinalizeRank},  // same as $rank
        {AccumulatorIntegral::kName, &buildFinalizeIntegral},
        {AccumulatorLocf::kName, nullptr},
        {AccumulatorDocumentNumber::kName, nullptr},
    };

    auto accExprName = acc.expr.name;
    uassert(5754700,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << accExprName,
            kAccumulatorBuilders.find(accExprName) != kAccumulatorBuilders.end());

    if (auto fn = kAccumulatorBuilders.at(accExprName); fn) {
        return std::invoke(fn, state, acc.expr, aggSlots, collatorSlot, frameIdGenerator);
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
        {window_function::ExpressionDerivative::kName, &buildFinalizeDerivative},
        {window_function::ExpressionLinearFill::kName, &buildFinalizeLinearFill},
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
        {AccumulatorCovarianceSamp::kName, &emptyInitializer<1>},
        {AccumulatorCovariancePop::kName, &emptyInitializer<1>},
        {AccumulatorExpMovingAvg::kName, &buildInitializeExpMovingAvg},
        {AccumulatorLocf::kName, &emptyInitializer<1>},
        {AccumulatorDocumentNumber::kName, &emptyInitializer<1>},
        {AccumulatorRank::kName, &emptyInitializer<1>},
        {AccumulatorDenseRank::kName, &emptyInitializer<1>},
        {AccumulatorIntegral::kName, &buildInitializeIntegral},
        {window_function::ExpressionDerivative::kName, &emptyInitializer<1>},
        {window_function::ExpressionLinearFill::kName, &buildInitializeLinearFill},
    };

    auto accExprName = acc.expr.name;
    uassert(7567300,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << accExprName,
            kAccumulatorBuilders.find(accExprName) != kAccumulatorBuilders.end());

    return std::invoke(kAccumulatorBuilders.at(accExprName), std::move(initExpr), frameIdGenerator);
}

std::vector<std::unique_ptr<sbe::EExpression>> buildInitialize(
    const AccumulationStatement& acc,
    StringDataMap<std::unique_ptr<sbe::EExpression>> argExprs,
    sbe::value::FrameIdGenerator& frameIdGenerator) {
    using BuildInitializeFn = std::function<std::vector<std::unique_ptr<sbe::EExpression>>(
        StringDataMap<std::unique_ptr<sbe::EExpression>>, sbe::value::FrameIdGenerator&)>;

    static const StringDataMap<BuildInitializeFn> kAccumulatorBuilders = {
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
        {AccumulatorMaxN::kName, &buildInitializeAccumulatorMulti},
        {AccumulatorMinN::kName, &buildInitializeAccumulatorMulti},
    };

    auto accExprName = acc.expr.name;
    uassert(8070614,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << accExprName,
            kAccumulatorBuilders.find(accExprName) != kAccumulatorBuilders.end());

    return std::invoke(kAccumulatorBuilders.at(accExprName), std::move(argExprs), frameIdGenerator);
}
}  // namespace mongo::stage_builder
