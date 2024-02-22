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
#include "mongo/db/query/sbe_stage_builder_sbexpr_helpers.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo::stage_builder {
namespace {

SbExpr wrapMinMaxArg(SbExpr arg, StageBuilderState& state) {
    SbExprBuilder b(state);

    auto frameId = state.frameIdGenerator->generate();
    auto binds = SbExpr::makeSeq(std::move(arg));
    auto var = SbVar(frameId, 0);

    auto e = b.makeIf(b.generateNullMissingOrUndefined(var), b.makeNothingConstant(), var);

    return b.makeLet(frameId, std::move(binds), std::move(e));
}

SbExpr::Vector buildAccumulatorMin(const AccumulationExpression& expr,
                                   SbExpr arg,
                                   boost::optional<sbe::value::SlotId> collatorSlot,
                                   StageBuilderState& state) {
    SbExprBuilder b(state);

    if (collatorSlot) {
        return SbExpr::makeSeq(b.makeFunction(
            "collMin"_sd, SbVar{*collatorSlot}, wrapMinMaxArg(std::move(arg), state)));
    } else {
        return SbExpr::makeSeq(b.makeFunction("min"_sd, wrapMinMaxArg(std::move(arg), state)));
    }
}

std::vector<BlockAggAndRowAgg> buildBlockAccumulatorMin(
    const AccumulationExpression& expr,
    SbExpr arg,
    SbSlot bitmapInternalSlot,
    SbSlot accInternalSlot,
    boost::optional<sbe::value::SlotId> collatorSlot,
    StageBuilderState& state) {
    SbExprBuilder b(state);
    std::vector<BlockAggAndRowAgg> pairs;

    pairs.emplace_back(
        BlockAggAndRowAgg{b.makeFunction("valueBlockAggMin"_sd, bitmapInternalSlot, std::move(arg)),
                          b.makeFunction("min"_sd, accInternalSlot)});

    return pairs;
}

SbExpr::Vector buildCombinePartialAggsMin(const AccumulationExpression& expr,
                                          const SbSlotVector& inputSlots,
                                          boost::optional<sbe::value::SlotId> collatorSlot,
                                          StageBuilderState& state) {
    tassert(7039501,
            "partial agg combiner for $min should have exactly one input slot",
            inputSlots.size() == 1);
    auto arg = inputSlots[0];
    return buildAccumulatorMin(expr, std::move(arg), collatorSlot, state);
}

SbExpr buildFinalizeMin(StageBuilderState& state,
                        const AccumulationExpression& expr,
                        const SbSlotVector& minSlots,
                        boost::optional<sbe::value::SlotId> collatorSlot) {
    SbExprBuilder b(state);

    // We can get away with not building a project stage since there's no finalize step but we
    // will stick the slot into an EVariable in case a $min is one of many group clauses and it
    // can be combined into a final project stage.
    tassert(5754702,
            str::stream() << "Expected one input slot for finalization of min, got: "
                          << minSlots.size(),
            minSlots.size() == 1);
    return b.makeFillEmptyNull(minSlots[0]);
}

SbExpr::Vector buildAccumulatorMax(const AccumulationExpression& expr,
                                   SbExpr arg,
                                   boost::optional<sbe::value::SlotId> collatorSlot,
                                   StageBuilderState& state) {
    SbExprBuilder b(state);

    if (collatorSlot) {
        return SbExpr::makeSeq(b.makeFunction(
            "collMax"_sd, SbVar{*collatorSlot}, wrapMinMaxArg(std::move(arg), state)));
    } else {
        return SbExpr::makeSeq(b.makeFunction("max"_sd, wrapMinMaxArg(std::move(arg), state)));
    }
}

std::vector<BlockAggAndRowAgg> buildBlockAccumulatorMax(
    const AccumulationExpression& expr,
    SbExpr arg,
    SbSlot bitmapInternalSlot,
    SbSlot accInternalSlot,
    boost::optional<sbe::value::SlotId> collatorSlot,
    StageBuilderState& state) {
    SbExprBuilder b(state);
    std::vector<BlockAggAndRowAgg> pairs;

    pairs.emplace_back(
        BlockAggAndRowAgg{b.makeFunction("valueBlockAggMax"_sd, bitmapInternalSlot, std::move(arg)),
                          b.makeFunction("max"_sd, accInternalSlot)});

    return pairs;
}

SbExpr::Vector buildCombinePartialAggsMax(const AccumulationExpression& expr,
                                          const SbSlotVector& inputSlots,
                                          boost::optional<sbe::value::SlotId> collatorSlot,
                                          StageBuilderState& state) {
    tassert(7039502,
            "partial agg combiner for $max should have exactly one input slot",
            inputSlots.size() == 1);
    auto arg = inputSlots[0];
    return buildAccumulatorMax(expr, std::move(arg), collatorSlot, state);
}

SbExpr buildFinalizeMax(StageBuilderState& state,
                        const AccumulationExpression& expr,
                        const SbSlotVector& maxSlots,
                        boost::optional<sbe::value::SlotId> collatorSlot) {
    SbExprBuilder b(state);

    tassert(5755100,
            str::stream() << "Expected one input slot for finalization of max, got: "
                          << maxSlots.size(),
            maxSlots.size() == 1);
    return b.makeFillEmptyNull(maxSlots[0]);
}


SbExpr::Vector buildAccumulatorFirst(const AccumulationExpression& expr,
                                     SbExpr arg,
                                     boost::optional<sbe::value::SlotId> collatorSlot,
                                     StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("first", b.makeFillEmptyNull(std::move(arg))));
}

SbExpr::Vector buildCombinePartialAggsFirst(const AccumulationExpression& expr,
                                            const SbSlotVector& inputSlots,
                                            boost::optional<sbe::value::SlotId> collatorSlot,
                                            StageBuilderState& state) {
    tassert(7039503,
            "partial agg combiner for $first should have exactly one input slot",
            inputSlots.size() == 1);
    auto arg = inputSlots[0];
    return buildAccumulatorFirst(expr, std::move(arg), collatorSlot, state);
}

SbExpr::Vector buildAccumulatorLast(const AccumulationExpression& expr,
                                    SbExpr arg,
                                    boost::optional<sbe::value::SlotId> collatorSlot,
                                    StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("last", b.makeFillEmptyNull(std::move(arg))));
}

SbExpr::Vector buildCombinePartialAggsLast(const AccumulationExpression& expr,
                                           const SbSlotVector& inputSlots,
                                           boost::optional<sbe::value::SlotId> collatorSlot,
                                           StageBuilderState& state) {
    tassert(7039504,
            "partial agg combiner for $last should have exactly one input slot",
            inputSlots.size() == 1);
    auto arg = inputSlots[0];
    return buildAccumulatorLast(expr, std::move(arg), collatorSlot, state);
}

SbExpr::Vector buildAccumulatorAvg(const AccumulationExpression& expr,
                                   SbExpr arg,
                                   boost::optional<sbe::value::SlotId> collatorSlot,
                                   StageBuilderState& state) {
    SbExprBuilder b(state);

    SbExpr::Vector aggs;

    // 'aggDoubleDoubleSum' will ignore non-numeric values automatically.
    aggs.push_back(b.makeFunction("aggDoubleDoubleSum", arg.clone()));

    auto frameId = state.frameIdGenerator->generate();
    auto binds = SbExpr::makeSeq(std::move(arg));
    auto var = SbVar{frameId, 0};

    auto e = b.makeIf(b.makeBinaryOp(sbe::EPrimBinary::logicOr,
                                     b.generateNullMissingOrUndefined(var),
                                     b.generateNonNumericCheck(var)),
                      b.makeInt64Constant(0),
                      b.makeInt64Constant(1));

    // For the counter we need to skip non-numeric values ourselves.
    auto addend = b.makeLet(frameId, std::move(binds), std::move(e));

    auto counterExpr = b.makeFunction("sum", std::move(addend));
    aggs.push_back(std::move(counterExpr));

    return aggs;
}

SbExpr::Vector buildCombinePartialAggsAvg(const AccumulationExpression& expr,
                                          const SbSlotVector& inputSlots,
                                          boost::optional<sbe::value::SlotId> collatorSlot,
                                          StageBuilderState& state) {
    SbExprBuilder b(state);

    tassert(7039539,
            "partial agg combiner for $avg should have exactly two input slots",
            inputSlots.size() == 2);

    return SbExpr::makeSeq(b.makeFunction("aggMergeDoubleDoubleSums", inputSlots[0]),
                           b.makeFunction("sum", inputSlots[1]));
}

SbExpr buildFinalizeAvg(StageBuilderState& state,
                        const AccumulationExpression& expr,
                        const SbSlotVector& aggSlots,
                        boost::optional<sbe::value::SlotId> collatorSlot) {
    SbExprBuilder b(state);

    // Slot 0 contains the accumulated sum, and slot 1 contains the count of summed items.
    tassert(5754703,
            str::stream() << "Expected two slots to finalize avg, got: " << aggSlots.size(),
            aggSlots.size() == 2);

    if (state.needsMerge) {
        // To support the sharding behavior, the mongos splits $group into two separate $group
        // stages one at the mongos-side and the other at the shard-side. This stage builder builds
        // the shard-side plan. The shard-side $avg accumulator is responsible to return the partial
        // avg in the form of {count: val, ps: array_val}.
        auto sumResult = aggSlots[0];
        auto countResult = aggSlots[1];
        auto partialSumExpr = b.makeFunction("doubleDoublePartialSumFinalize", sumResult);

        // Returns {count: val, ps: array_val}.
        auto partialAvgFinalize = b.makeFunction("newObj"_sd,
                                                 b.makeStrConstant(countName),
                                                 countResult,
                                                 b.makeStrConstant(partialSumName),
                                                 std::move(partialSumExpr));

        return partialAvgFinalize;
    } else {
        // If we've encountered any numeric input, the counter would contain a positive integer.
        // Unlike $sum, when there is no numeric input, $avg should return null.
        auto finalizingExpression =
            b.makeIf(b.makeBinaryOp(sbe::EPrimBinary::eq, aggSlots[1], b.makeInt64Constant(0)),
                     b.makeNullConstant(),
                     b.makeBinaryOp(sbe::EPrimBinary::div,
                                    b.makeFunction("doubleDoubleSumFinalize", aggSlots[0]),
                                    aggSlots[1]));

        return finalizingExpression;
    }
}

std::tuple<bool, sbe::value::TypeTags, sbe::value::Value> getCountAddend(
    const AccumulationExpression& expr) {
    auto constArg = dynamic_cast<ExpressionConstant*>(expr.argument.get());
    if (!constArg) {
        return {false, sbe::value::TypeTags::Nothing, 0};
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
            return {false, sbe::value::TypeTags::Nothing, 0};
    }
}

SbExpr::Vector buildAccumulatorSum(const AccumulationExpression& expr,
                                   SbExpr arg,
                                   boost::optional<sbe::value::SlotId> collatorSlot,
                                   StageBuilderState& state) {
    SbExprBuilder b(state);

    // Optimize for a count-like accumulator like {$sum: 1}.
    if (auto [isCount, addendTag, addendVal] = getCountAddend(expr); isCount) {
        return SbExpr::makeSeq(b.makeFunction("sum", b.makeConstant(addendTag, addendVal)));
    }

    return SbExpr::makeSeq(b.makeFunction("aggDoubleDoubleSum", std::move(arg)));
}

SbExpr::Vector buildCombinePartialAggsSum(const AccumulationExpression& expr,
                                          const SbSlotVector& inputSlots,
                                          boost::optional<sbe::value::SlotId> collatorSlot,
                                          StageBuilderState& state) {
    SbExprBuilder b(state);

    tassert(7039530,
            "partial agg combiner for $sum should have exactly one input slot",
            inputSlots.size() == 1);
    auto arg = inputSlots[0];

    // If the user specifies a count-like accumulator like {$sum: 1}, then we optimize the plan to
    // use the simple "sum" accumulator rather than a DoubleDouble summation. Therefore, the partial
    // aggregates are simple sums and we require nothing special to combine multiple DoubleDouble
    // summations.
    if (auto [isCount, _1, _2] = getCountAddend(expr); isCount) {
        return SbExpr::makeSeq(b.makeFunction("sum", std::move(arg)));
    }

    return SbExpr::makeSeq(b.makeFunction("aggMergeDoubleDoubleSums", std::move(arg)));
}

SbExpr buildFinalizeSum(StageBuilderState& state,
                        const AccumulationExpression& expr,
                        const SbSlotVector& sumSlots,
                        boost::optional<sbe::value::SlotId> collatorSlot) {
    SbExprBuilder b(state);

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
        return b.makeFunction("doubleDoublePartialSumFinalize", sumSlots[0]);
    }

    if (auto [isCount, tag, val] = getCountAddend(expr); isCount) {
        // The accumulation result is a scalar value. So, the final project is not necessary.
        return {};
    }

    return b.makeFunction("doubleDoubleSumFinalize", sumSlots[0]);
}

SbExpr::Vector buildAccumulatorAddToSetHelper(SbExpr arg,
                                              StringData funcName,
                                              boost::optional<sbe::value::SlotId> collatorSlot,
                                              StringData funcNameWithCollator,
                                              StageBuilderState& state) {
    SbExprBuilder b(state);

    const int cap = internalQueryMaxAddToSetBytes.load();
    if (collatorSlot) {
        return SbExpr::makeSeq(b.makeFunction(
            funcNameWithCollator, SbVar{*collatorSlot}, std::move(arg), b.makeInt32Constant(cap)));
    } else {
        return SbExpr::makeSeq(b.makeFunction(funcName, std::move(arg), b.makeInt32Constant(cap)));
    }
}

SbExpr::Vector buildAccumulatorAddToSet(const AccumulationExpression& expr,
                                        SbExpr arg,
                                        boost::optional<sbe::value::SlotId> collatorSlot,
                                        StageBuilderState& state) {
    return buildAccumulatorAddToSetHelper(
        std::move(arg), "addToSetCapped"_sd, collatorSlot, "collAddToSetCapped"_sd, state);
}

SbExpr::Vector buildCombinePartialAggsAddToSet(const AccumulationExpression& expr,
                                               const SbSlotVector& inputSlots,
                                               boost::optional<sbe::value::SlotId> collatorSlot,
                                               StageBuilderState& state) {
    tassert(7039506,
            "partial agg combiner for $addToSet should have exactly one input slot",
            inputSlots.size() == 1);
    auto arg = inputSlots[0];
    return buildAccumulatorAddToSetHelper(
        std::move(arg), "aggSetUnionCapped"_sd, collatorSlot, "aggCollSetUnionCapped"_sd, state);
}

SbExpr buildFinalizeCappedAccumulator(StageBuilderState& state,
                                      const AccumulationExpression& expr,
                                      const SbSlotVector& accSlots,
                                      boost::optional<sbe::value::SlotId> collatorSlot) {
    SbExprBuilder b(state);

    tassert(6526500,
            str::stream() << "Expected one input slot for finalization of capped accumulator, got: "
                          << accSlots.size(),
            accSlots.size() == 1);

    // 'accSlots[0]' should contain an array of size two, where the front element is the accumulated
    // values and the back element is their cumulative size in bytes. We just ignore the size
    // because if it exceeded the size cap, we should have thrown an error during accumulation.
    auto pushFinalize =
        b.makeFunction("getElement",
                       accSlots[0],
                       b.makeInt32Constant(static_cast<int>(sbe::vm::AggArrayWithSize::kValues)));

    return pushFinalize;
}

SbExpr::Vector buildAccumulatorPushHelper(SbExpr arg,
                                          StringData aggFuncName,
                                          StageBuilderState& state) {
    SbExprBuilder b(state);

    const int cap = internalQueryMaxPushBytes.load();
    return SbExpr::makeSeq(b.makeFunction(aggFuncName, std::move(arg), b.makeInt32Constant(cap)));
}

SbExpr::Vector buildAccumulatorPush(const AccumulationExpression& expr,
                                    SbExpr arg,
                                    boost::optional<sbe::value::SlotId> collatorSlot,
                                    StageBuilderState& state) {
    return buildAccumulatorPushHelper(std::move(arg), "addToArrayCapped"_sd, state);
}

SbExpr::Vector buildCombinePartialAggsPush(const AccumulationExpression& expr,
                                           const SbSlotVector& inputSlots,
                                           boost::optional<sbe::value::SlotId> collatorSlot,
                                           StageBuilderState& state) {
    tassert(7039505,
            "partial agg combiner for $push should have exactly one input slot",
            inputSlots.size() == 1);
    auto arg = inputSlots[0];
    return buildAccumulatorPushHelper(std::move(arg), "aggConcatArraysCapped"_sd, state);
}

SbExpr::Vector buildAccumulatorStdDev(const AccumulationExpression& expr,
                                      SbExpr arg,
                                      boost::optional<sbe::value::SlotId> collatorSlot,
                                      StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("aggStdDev", std::move(arg)));
}

SbExpr::Vector buildCombinePartialAggsStdDev(const AccumulationExpression& expr,
                                             const SbSlotVector& inputSlots,
                                             boost::optional<sbe::value::SlotId> collatorSlot,
                                             StageBuilderState& state) {
    SbExprBuilder b(state);

    tassert(7039540,
            "partial agg combiner for stddev should have exactly one input slot",
            inputSlots.size() == 1);
    auto arg = inputSlots[0];
    return SbExpr::makeSeq(b.makeFunction("aggMergeStdDevs", arg));
}

SbExpr buildFinalizePartialStdDev(SbSlot stdDevSlot, StageBuilderState& state) {
    SbExprBuilder b(state);

    // To support the sharding behavior, the mongos splits $group into two separate $group
    // stages one at the mongos-side and the other at the shard-side. This stage builder builds
    // the shard-side plan. The shard-side $stdDevSamp and $stdDevPop accumulators are responsible
    // to return the partial stddev in the form of {m2: val1, mean: val2, count: val3}.
    auto stdDevResult = stdDevSlot;

    auto m2Field = b.makeFunction(
        "getElement",
        stdDevResult,
        b.makeInt32Constant(static_cast<int>(sbe::vm::AggStdDevValueElems::kRunningM2)));

    auto meanField = b.makeFunction(
        "getElement",
        stdDevResult,
        b.makeInt32Constant(static_cast<int>(sbe::vm::AggStdDevValueElems::kRunningMean)));

    auto countField =
        b.makeFunction("getElement",
                       stdDevResult,
                       b.makeInt32Constant(static_cast<int>(sbe::vm::AggStdDevValueElems::kCount)));

    return b.makeFunction("newObj"_sd,
                          b.makeStrConstant("m2"_sd),
                          std::move(m2Field),
                          b.makeStrConstant("mean"_sd),
                          std::move(meanField),
                          b.makeStrConstant("count"_sd),
                          std::move(countField));
}

SbExpr buildFinalizeStdDevPop(StageBuilderState& state,
                              const AccumulationExpression& expr,
                              const SbSlotVector& stdDevSlots,
                              boost::optional<sbe::value::SlotId> collatorSlot) {
    SbExprBuilder b(state);

    tassert(5755204,
            str::stream() << "Expected one input slot for finalization of stdDevPop, got: "
                          << stdDevSlots.size(),
            stdDevSlots.size() == 1);

    if (state.needsMerge) {
        return buildFinalizePartialStdDev(stdDevSlots[0], state);
    } else {
        auto stdDevPopFinalize = b.makeFunction("stdDevPopFinalize", stdDevSlots[0]);
        return stdDevPopFinalize;
    }
}

SbExpr buildFinalizeStdDevSamp(StageBuilderState& state,
                               const AccumulationExpression& expr,
                               const SbSlotVector& stdDevSlots,
                               boost::optional<sbe::value::SlotId> collatorSlot) {
    SbExprBuilder b(state);

    tassert(5755209,
            str::stream() << "Expected one input slot for finalization of stdDevSamp, got: "
                          << stdDevSlots.size(),
            stdDevSlots.size() == 1);

    if (state.needsMerge) {
        return buildFinalizePartialStdDev(stdDevSlots[0], state);
    } else {
        return b.makeFunction("stdDevSampFinalize", stdDevSlots[0]);
    }
}

SbExpr::Vector buildAccumulatorMergeObjects(const AccumulationExpression& expr,
                                            SbExpr arg,
                                            boost::optional<sbe::value::SlotId> collatorSlot,
                                            StageBuilderState& state) {
    SbExprBuilder b(state);

    auto frameId = state.frameIdGenerator->generate();
    auto binds = SbExpr::makeSeq(std::move(arg));
    auto var = SbVar{frameId, 0};

    auto typeCheckExpr = b.makeBinaryOp(sbe::EPrimBinary::logicOr,
                                        b.generateNullMissingOrUndefined(var),
                                        b.makeFunction("isObject", var));

    auto e =
        b.makeIf(std::move(typeCheckExpr),
                 b.makeFunction("mergeObjects", var),
                 b.makeFail(ErrorCodes::Error{5911200}, "$mergeObjects only supports objects"));

    auto filterExpr = b.makeLet(frameId, std::move(binds), std::move(e));

    return SbExpr::makeSeq(std::move(filterExpr));
}

SbExpr::Vector buildCombinePartialAggsMergeObjects(const AccumulationExpression& expr,
                                                   const SbSlotVector& inputSlots,
                                                   boost::optional<sbe::value::SlotId> collatorSlot,
                                                   StageBuilderState& state) {
    tassert(7039507,
            "partial agg combiner for $mergeObjects should have exactly one input slot",
            inputSlots.size() == 1);
    auto arg = inputSlots[0];
    return buildAccumulatorMergeObjects(expr, std::move(arg), collatorSlot, state);
}

SbExpr::Vector buildInitializeAccumulatorMulti(StringDataMap<SbExpr> argExprs,
                                               StageBuilderState& state) {
    SbExprBuilder b(state);

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
    auto maxAccumulatorBytes = internalQueryTopNAccumulatorBytes.load();
    if (maxSizeExpr.isConstantExpr()) {
        auto [tag, val] = maxSizeExpr.getConstantValue();
        auto [convertOwn, convertTag, convertVal] =
            genericNumConvert(tag, val, sbe::value::TypeTags::NumberInt64);
        uassert(7548606,
                "parameter 'n' must be coercible to a positive 64-bit integer",
                convertTag != sbe::value::TypeTags::Nothing &&
                    static_cast<int64_t>(convertVal) > 0);
        return SbExpr::makeSeq(b.makeFunction("newArray",
                                              b.makeFunction("newArray"),
                                              b.makeInt64Constant(0),
                                              b.makeConstant(convertTag, convertVal),
                                              b.makeInt32Constant(0),
                                              b.makeInt32Constant(maxAccumulatorBytes),
                                              std::move(isGroupAccumExpr)));
    } else {
        auto frameId = state.frameIdGenerator->generate();
        auto binds = SbExpr::makeSeq(
            b.makeNumericConvert(std::move(maxSizeExpr), sbe::value::TypeTags::NumberInt64));
        auto var = SbVar{frameId, 0};

        auto e = b.makeIf(
            b.makeBinaryOp(sbe::EPrimBinary::logicAnd,
                           b.makeFunction("exists", var),
                           b.makeBinaryOp(sbe::EPrimBinary::greater, var, b.makeInt64Constant(0))),
            b.makeFunction("newArray",
                           b.makeFunction("newArray"),
                           b.makeInt64Constant(0),
                           var,
                           b.makeInt32Constant(0),
                           b.makeInt32Constant(maxAccumulatorBytes),
                           std::move(isGroupAccumExpr)),
            b.makeFail(ErrorCodes::Error{7548607},
                       "parameter 'n' must be coercible to a positive 64-bit integer"));

        auto localBind = b.makeLet(frameId, std::move(binds), std::move(e));

        return SbExpr::makeSeq(std::move(localBind));
    }
}

SbExpr::Vector buildAccumulatorFirstN(const AccumulationExpression& expr,
                                      SbExpr arg,
                                      boost::optional<sbe::value::SlotId> collatorSlot,
                                      StageBuilderState& state) {
    SbExprBuilder b(state);

    auto frameId = state.frameIdGenerator->generate();
    auto binds = SbExpr::makeSeq(b.makeFunction("aggState"));

    auto varExpr = SbExpr{SbVar{frameId, 0}};
    auto moveVarExpr = SbExpr{makeMoveVariable(frameId, 0)};

    auto e = b.makeIf(
        b.makeFunction("aggFirstNNeedsMoreInput", varExpr.clone()),
        b.makeFunction("aggFirstN", moveVarExpr.clone(), b.makeFillEmptyNull(std::move(arg))),
        moveVarExpr.clone());

    return SbExpr::makeSeq(b.makeLet(frameId, std::move(binds), std::move(e)));
}

SbExpr::Vector buildCombinePartialAggsFirstN(const AccumulationExpression& expr,
                                             const SbSlotVector& inputSlots,
                                             boost::optional<sbe::value::SlotId> collatorSlot,
                                             StageBuilderState& state) {
    SbExprBuilder b(state);

    uassert(7548608,
            str::stream() << "Expected one input slot for merging $firstN, got: "
                          << inputSlots.size(),
            inputSlots.size() == 1);

    return SbExpr::makeSeq(b.makeFunction("aggFirstNMerge", inputSlots[0]));
}

SbExpr buildFinalizeFirstN(StageBuilderState& state,
                           const AccumulationExpression& expr,
                           const SbSlotVector& inputSlots,
                           boost::optional<sbe::value::SlotId> collatorSlot) {
    SbExprBuilder b(state);

    uassert(7548609,
            str::stream() << "Expected one input slot for finalization of $firstN, got: "
                          << inputSlots.size(),
            inputSlots.size() == 1);
    return b.makeFunction("aggFirstNFinalize", inputSlots[0]);
}

SbExpr::Vector buildAccumulatorLastN(const AccumulationExpression& expr,
                                     SbExpr arg,
                                     boost::optional<sbe::value::SlotId> collatorSlot,
                                     StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("aggLastN", b.makeFillEmptyNull(std::move(arg))));
}

SbExpr::Vector buildCombinePartialAggsLastN(const AccumulationExpression& expr,
                                            const SbSlotVector& inputSlots,
                                            boost::optional<sbe::value::SlotId> collatorSlot,
                                            StageBuilderState& state) {
    SbExprBuilder b(state);

    uassert(7548701,
            str::stream() << "Expected one input slot for merging $lastN, got: "
                          << inputSlots.size(),
            inputSlots.size() == 1);

    return SbExpr::makeSeq(b.makeFunction("aggLastNMerge", inputSlots[0]));
}

SbExpr buildFinalizeLastN(StageBuilderState& state,
                          const AccumulationExpression& expr,
                          const SbSlotVector& inputSlots,
                          boost::optional<sbe::value::SlotId> collatorSlot) {
    SbExprBuilder b(state);

    uassert(7548702,
            str::stream() << "Expected one input slot for finalization of $lastN, got: "
                          << inputSlots.size(),
            inputSlots.size() == 1);
    return b.makeFunction("aggLastNFinalize", inputSlots[0]);
}

bool isAccumulatorTopN(const AccumulationExpression& expr) {
    return expr.name == AccumulatorTopBottomN<kTop, false /* single */>::getName() ||
        expr.name == AccumulatorTopBottomN<kTop, true /* single */>::getName();
}

SbExpr::Vector buildAccumulatorTopBottomN(const AccumulationExpression& expr,
                                          StringDataMap<SbExpr> args,
                                          boost::optional<sbe::value::SlotId> collatorSlot,
                                          StageBuilderState& state) {
    SbExprBuilder b(state);

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

    return SbExpr::makeSeq(b.makeFunction(isAccumulatorTopN(expr) ? "aggTopN" : "aggBottomN",
                                          std::move(key),
                                          std::move(value),
                                          std::move(sortSpec)));
}

SbExpr::Vector buildCombinePartialTopBottomN(const AccumulationExpression& expr,
                                             const SbSlotVector& inputSlots,
                                             StringDataMap<SbExpr> args,
                                             boost::optional<sbe::value::SlotId> collatorSlot,
                                             StageBuilderState& state) {
    SbExprBuilder b(state);

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

    return SbExpr::makeSeq(
        b.makeFunction(isAccumulatorTopN(expr) ? "aggTopNMerge" : "aggBottomNMerge",
                       inputSlots[0],
                       std::move(sortSpec)));
}

SbExpr buildFinalizeTopBottomNImpl(StageBuilderState& state,
                                   const AccumulationExpression& expr,
                                   const SbSlotVector& inputSlots,
                                   StringDataMap<SbExpr> args,
                                   boost::optional<sbe::value::SlotId> collatorSlot,
                                   bool single) {
    SbExprBuilder b(state);

    tassert(5807012,
            str::stream() << "Expected one input slot for finalization of " << expr.name
                          << ", got: " << inputSlots.size(),
            inputSlots.size() == 1);
    auto inputVar = inputSlots[0];

    auto it = args.find(AccArgs::kTopBottomNSortSpec);
    tassert(5807023,
            str::stream() << "Accumulator " << expr.name << " expects a '"
                          << AccArgs::kTopBottomNSortSpec << "' argument",
            it != args.end());
    auto sortSpec = std::move(it->second);

    if (state.needsMerge) {
        // When the data will be merged, the heap itself doesn't need to be sorted since the merging
        // code will handle the sorting.
        auto heapExpr = b.makeFunction(
            "getElement",
            inputVar,
            b.makeInt32Constant(static_cast<int>(sbe::vm::AggMultiElems::kInternalArr)));
        auto lambdaFrameId = state.frameIdGenerator->generate();
        auto pairVar = SbVar{lambdaFrameId, 0};
        auto lambdaExpr = b.makeLocalLambda(
            lambdaFrameId,
            b.makeFunction("newObj"_sd,
                           b.makeStrConstant(AccumulatorN::kFieldNameGeneratedSortKey),
                           b.makeFunction("getElement", pairVar, b.makeInt32Constant(0)),
                           b.makeStrConstant(AccumulatorN::kFieldNameOutput),
                           b.makeFunction("getElement", pairVar, b.makeInt32Constant(1))));
        // Convert the array pair representation [key, output] to an object format that the merging
        // code expects.
        return b.makeFunction(
            "traverseP", std::move(heapExpr), std::move(lambdaExpr), b.makeInt32Constant(1));
    } else {
        auto finalExpr =
            b.makeFunction(isAccumulatorTopN(expr) ? "aggTopNFinalize" : "aggBottomNFinalize",
                           inputVar,
                           std::move(sortSpec));
        if (single) {
            finalExpr = b.makeFunction("getElement", std::move(finalExpr), b.makeInt32Constant(0));
        }
        return finalExpr;
    }
}

SbExpr buildFinalizeTopBottomN(StageBuilderState& state,
                               const AccumulationExpression& expr,
                               const SbSlotVector& inputSlots,
                               StringDataMap<SbExpr> args,
                               boost::optional<sbe::value::SlotId> collatorSlot) {
    return buildFinalizeTopBottomNImpl(
        state, expr, inputSlots, std::move(args), collatorSlot, false /* single */);
}

SbExpr buildFinalizeTopBottom(StageBuilderState& state,
                              const AccumulationExpression& expr,
                              const SbSlotVector& inputSlots,
                              StringDataMap<SbExpr> args,
                              boost::optional<sbe::value::SlotId> collatorSlot) {
    return buildFinalizeTopBottomNImpl(
        state, expr, inputSlots, std::move(args), collatorSlot, true /* single */);
}

SbExpr::Vector buildAccumulatorMinMaxN(const AccumulationExpression& expr,
                                       SbExpr arg,
                                       boost::optional<sbe::value::SlotId> collatorSlot,
                                       StageBuilderState& state) {
    SbExprBuilder b(state);

    auto aggExprName = expr.name == AccumulatorMaxN::kName ? "aggMaxN" : "aggMinN";
    if (collatorSlot) {
        return SbExpr::makeSeq(b.makeFunction(std::move(aggExprName),
                                              b.makeFunction("setToArray", std::move(arg)),
                                              SbVar{*collatorSlot}));

    } else {
        return SbExpr::makeSeq(
            b.makeFunction(std::move(aggExprName), b.makeFunction("setToArray", std::move(arg))));
    }
}

SbExpr::Vector buildCombinePartialAggsMinMaxN(const AccumulationExpression& expr,
                                              const SbSlotVector& inputSlots,
                                              boost::optional<sbe::value::SlotId> collatorSlot,
                                              StageBuilderState& state) {
    SbExprBuilder b(state);

    uassert(7548808,
            str::stream() << "Expected one input slot for merging, got: " << inputSlots.size(),
            inputSlots.size() == 1);

    auto aggExprName = expr.name == AccumulatorMaxN::kName ? "aggMaxNMerge" : "aggMinNMerge";
    if (collatorSlot) {
        return SbExpr::makeSeq(
            b.makeFunction(std::move(aggExprName), inputSlots[0], SbVar{*collatorSlot}));
    } else {
        return SbExpr::makeSeq(b.makeFunction(std::move(aggExprName), inputSlots[0]));
    }
}

SbExpr buildFinalizeMinMaxN(StageBuilderState& state,
                            const AccumulationExpression& expr,
                            const SbSlotVector& inputSlots,
                            boost::optional<sbe::value::SlotId> collatorSlot) {
    SbExprBuilder b(state);

    uassert(7548809,
            str::stream() << "Expected one input slot for finalization, got: " << inputSlots.size(),
            inputSlots.size() == 1);
    auto aggExprName = expr.name == AccumulatorMaxN::kName ? "aggMaxNFinalize" : "aggMinNFinalize";
    if (collatorSlot) {
        return b.makeFunction(std::move(aggExprName), inputSlots[0], SbVar{*collatorSlot});
    } else {
        return b.makeFunction(std::move(aggExprName), inputSlots[0]);
    }
}

SbExpr::Vector buildAccumulatorCovariance(const AccumulationExpression& expr,
                                          StringDataMap<SbExpr> args,
                                          boost::optional<sbe::value::SlotId> collatorSlot,
                                          StageBuilderState& state) {
    SbExprBuilder b(state);

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

    return SbExpr::makeSeq(b.makeFunction("aggCovarianceAdd", std::move(argX), std::move(argY)));
}

SbExpr buildFinalizeCovarianceSamp(StageBuilderState& state,
                                   const AccumulationExpression& expr,
                                   const SbSlotVector& slots,
                                   boost::optional<sbe::value::SlotId> collatorSlot) {
    SbExprBuilder b(state);

    tassert(7820814, "Incorrect number of arguments", slots.size() == 1);
    SbExpr::Vector exprs;
    for (auto slot : slots) {
        exprs.push_back(slot);
    }
    return b.makeFunction("aggCovarianceSampFinalize", std::move(exprs));
}

SbExpr buildFinalizeCovariancePop(StageBuilderState& state,
                                  const AccumulationExpression& expr,
                                  const SbSlotVector& slots,
                                  boost::optional<sbe::value::SlotId> collatorSlot) {
    SbExprBuilder b(state);

    tassert(7820815, "Incorrect number of arguments", slots.size() == 1);
    SbExpr::Vector exprs;
    for (auto slot : slots) {
        exprs.push_back(slot);
    }
    return b.makeFunction("aggCovariancePopFinalize", std::move(exprs));
}

SbExpr::Vector buildInitializeExpMovingAvg(SbExpr alphaExpr, StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction(
        "newArray", b.makeNullConstant(), std::move(alphaExpr), b.makeBoolConstant(false)));
}

SbExpr::Vector buildAccumulatorExpMovingAvg(const AccumulationExpression& expr,
                                            SbExpr arg,
                                            boost::optional<sbe::value::SlotId> collatorSlot,
                                            StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("aggExpMovingAvg", std::move(arg)));
}

SbExpr buildFinalizeExpMovingAvg(StageBuilderState& state,
                                 const AccumulationExpression& expr,
                                 const SbSlotVector& slots,
                                 boost::optional<sbe::value::SlotId> collatorSlot) {
    SbExprBuilder b(state);
    tassert(7996802, "Incorrect number of arguments", slots.size() == 1);
    return b.makeFunction("aggExpMovingAvgFinalize", slots[0]);
}

SbExpr::Vector buildAccumulatorLocf(const AccumulationExpression& expr,
                                    SbExpr arg,
                                    boost::optional<sbe::value::SlotId> collatorSlot,
                                    StageBuilderState& state) {
    SbExprBuilder b(state);

    auto frameId = state.frameIdGenerator->generate();
    auto binds = SbExpr::makeSeq(std::move(arg));
    auto var = SbVar{frameId, 0};

    auto e = b.makeIf(b.generateNullMissingOrUndefined(var), b.makeFunction("aggState"), var);

    auto localBind = b.makeLet(frameId, std::move(binds), std::move(e));

    return SbExpr::makeSeq(std::move(localBind));
}

SbExpr::Vector buildAccumulatorDocumentNumber(const AccumulationExpression& expr,
                                              SbExpr arg,
                                              boost::optional<sbe::value::SlotId> collatorSlot,
                                              StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("sum", b.makeInt64Constant(1)));
}

SbExpr::Vector buildAccumulatorRankImpl(const StringData rankFuncName,
                                        const StringData collRankFuncName,
                                        const AccumulationExpression& expr,
                                        StringDataMap<SbExpr> args,
                                        boost::optional<sbe::value::SlotId> collatorSlot,
                                        StageBuilderState& state) {
    SbExprBuilder b(state);

    auto it = args.find(AccArgs::kInput);
    tassert(8216801,
            str::stream() << "Accumulator " << expr.name << " expects a '" << AccArgs::kInput
                          << "' argument",
            it != args.end());
    auto input = std::move(it->second);

    it = args.find(AccArgs::kRankIsAscending);
    tassert(8216802,
            str::stream() << "Accumulator " << expr.name << " expects a '"
                          << AccArgs::kRankIsAscending << "' argument",
            it != args.end());
    auto sortOrder = std::move(it->second);

    if (collatorSlot) {
        return SbExpr::makeSeq(b.makeFunction(
            collRankFuncName, std::move(input), std::move(sortOrder), SbVar{*collatorSlot}));
    } else {
        return SbExpr::makeSeq(
            b.makeFunction(rankFuncName, std::move(input), std::move(sortOrder)));
    }
}

SbExpr::Vector buildAccumulatorRank(const AccumulationExpression& expr,
                                    StringDataMap<SbExpr> args,
                                    boost::optional<sbe::value::SlotId> collatorSlot,
                                    StageBuilderState& state) {
    return buildAccumulatorRankImpl(
        "aggRank", "aggRankColl", expr, std::move(args), collatorSlot, state);
}

SbExpr buildFinalizeRank(StageBuilderState& state,
                         const AccumulationExpression& expr,
                         const SbSlotVector& slots,
                         boost::optional<sbe::value::SlotId> collatorSlot) {
    SbExprBuilder b(state);

    tassert(7996805, "Incorrect number of arguments", slots.size() == 1);
    return b.makeFunction("aggRankFinalize", slots[0]);
}

SbExpr::Vector buildAccumulatorDenseRank(const AccumulationExpression& expr,
                                         StringDataMap<SbExpr> args,
                                         boost::optional<sbe::value::SlotId> collatorSlot,
                                         StageBuilderState& state) {
    return buildAccumulatorRankImpl(
        "aggDenseRank", "aggDenseRankColl", expr, std::move(args), collatorSlot, state);
}

SbExpr::Vector buildInitializeIntegral(SbExpr unitExpr, StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(
        b.makeFunction("aggIntegralInit", std::move(unitExpr), b.makeBoolConstant(true)));
}

SbExpr::Vector buildAccumulatorIntegral(const AccumulationExpression& expr,
                                        StringDataMap<SbExpr> args,
                                        boost::optional<sbe::value::SlotId> collatorSlot,
                                        StageBuilderState& state) {
    SbExprBuilder b(state);

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

    return SbExpr::makeSeq(b.makeFunction("aggIntegralAdd", std::move(input), std::move(sortBy)));
}

SbExpr buildFinalizeIntegral(StageBuilderState& state,
                             const AccumulationExpression& expr,
                             const SbSlotVector& slots,
                             boost::optional<sbe::value::SlotId> collatorSlot) {
    SbExprBuilder b(state);

    tassert(7996809, "Incorrect number of arguments", slots.size() == 1);
    return b.makeFunction("aggIntegralFinalize", slots[0]);
}

SbExpr::Vector buildAccumulatorDerivative(const AccumulationExpression& expr,
                                          StringDataMap<SbExpr> args,
                                          boost::optional<sbe::value::SlotId> collatorSlot,
                                          StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("sum", b.makeInt64Constant(1)));
}

SbExpr buildFinalizeDerivative(StageBuilderState& state,
                               const AccumulationExpression& expr,
                               const SbSlotVector& slots,
                               StringDataMap<SbExpr> args,
                               boost::optional<sbe::value::SlotId> collatorSlot) {
    SbExprBuilder b(state);

    tassert(8085504, "Expected a single slot", slots.size() == 1);
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

    return b.makeIf(
        b.makeBinaryOp(sbe::EPrimBinary::logicAnd,
                       b.makeFunction("exists", slots[0]),
                       b.makeBinaryOp(sbe::EPrimBinary::greater, slots[0], b.makeInt64Constant(0))),
        b.makeFunction("aggDerivativeFinalize",
                       std::move(unit),
                       std::move(inputFirst),
                       std::move(sortByFirst),
                       std::move(inputLast),
                       std::move(sortByLast)),
        b.makeNullConstant());
}

SbExpr::Vector buildInitializeLinearFill(SbExpr unitExpr, StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("newArray",
                                          b.makeNullConstant(),
                                          b.makeNullConstant(),
                                          b.makeNullConstant(),
                                          b.makeNullConstant(),
                                          b.makeNullConstant(),
                                          b.makeInt64Constant(0)));
}

SbExpr::Vector buildAccumulatorLinearFill(const AccumulationExpression& expr,
                                          StringDataMap<SbExpr> args,
                                          boost::optional<sbe::value::SlotId> collatorSlot,
                                          StageBuilderState& state) {
    SbExprBuilder b(state);

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

    return SbExpr::makeSeq(b.makeFunction(
        "aggLinearFillAdd", b.makeFillEmptyNull(std::move(input)), std::move(sortBy)));
}

SbExpr buildFinalizeLinearFill(StageBuilderState& state,
                               const AccumulationExpression& expr,
                               const SbSlotVector& inputSlots,
                               StringDataMap<SbExpr> args,
                               boost::optional<sbe::value::SlotId> collatorSlot) {
    SbExprBuilder b(state);

    tassert(7971213,
            str::stream() << "Expected one input slot for finalization of " << expr.name
                          << ", got: " << inputSlots.size(),
            inputSlots.size() == 1);
    auto inputVar = inputSlots[0];

    auto it = args.find(AccArgs::kSortBy);
    tassert(7971214,
            str::stream() << "Window function expects '" << AccArgs::kSortBy << "' argument",
            it != args.end());
    auto sortBy = std::move(it->second);

    return b.makeFunction("aggLinearFillFinalize", std::move(inputVar), std::move(sortBy));
}

template <int N>
SbExpr::Vector emptyInitializer(SbExpr maxSizeExpr, StageBuilderState& state) {
    SbExpr::Vector result;
    result.resize(N);
    return result;
}
}  // namespace

SbExpr::Vector buildAccumulator(const AccumulationStatement& acc,
                                SbExpr argExpr,
                                boost::optional<sbe::value::SlotId> collatorSlot,
                                StageBuilderState& state) {
    using BuildAccumulatorFn = std::function<SbExpr::Vector(const AccumulationExpression&,
                                                            SbExpr,
                                                            boost::optional<sbe::value::SlotId>,
                                                            StageBuilderState&)>;

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
    };

    auto accExprName = acc.expr.name;
    uassert(5754701,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << accExprName,
            kAccumulatorBuilders.find(accExprName) != kAccumulatorBuilders.end());

    auto& fn = kAccumulatorBuilders.at(accExprName);
    return std::invoke(fn, acc.expr, std::move(argExpr), collatorSlot, state);
}

std::vector<BlockAggAndRowAgg> buildBlockAccumulator(
    const AccumulationStatement& acc,
    SbExpr argExpr,
    SbSlot bitmapInternalSlot,
    SbSlot accInternalSlot,
    boost::optional<sbe::value::SlotId> collatorSlot,
    StageBuilderState& state) {
    using BuildBlockAccumulatorFn =
        std::function<std::vector<BlockAggAndRowAgg>(const AccumulationExpression&,
                                                     SbExpr,
                                                     SbSlot,
                                                     SbSlot,
                                                     boost::optional<sbe::value::SlotId>,
                                                     StageBuilderState&)>;

    static const StringDataMap<BuildBlockAccumulatorFn> kBlockAccumulatorBuilders = {
        {AccumulatorMin::kName, &buildBlockAccumulatorMin},
        {AccumulatorMax::kName, &buildBlockAccumulatorMax},
    };

    auto accExprName = acc.expr.name;

    auto it = kBlockAccumulatorBuilders.find(accExprName);
    if (it != kBlockAccumulatorBuilders.end()) {
        auto& fn = it->second;
        return std::invoke(fn,
                           acc.expr,
                           std::move(argExpr),
                           bitmapInternalSlot,
                           accInternalSlot,
                           collatorSlot,
                           state);
    }

    return {};
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulator(
    const AccumulationStatement& acc,
    std::unique_ptr<sbe::EExpression> argExpr,
    boost::optional<sbe::value::SlotId> collatorSlot,
    StageBuilderState& state) {
    // Call the other overload of buildAccumulator().
    auto sbExprs = buildAccumulator(acc, SbExpr{std::move(argExpr)}, collatorSlot, state);

    // Convert 'sbExprs' to a vector of sbe::EExpressions and return it.
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    for (auto&& e : sbExprs) {
        exprs.emplace_back(e.extractExpr(state));
    }

    return exprs;
}

SbExpr::Vector buildAccumulator(const AccumulationStatement& acc,
                                StringDataMap<SbExpr> argExprs,
                                boost::optional<sbe::value::SlotId> collatorSlot,
                                StageBuilderState& state) {
    using BuildAccumulatorFn = std::function<SbExpr::Vector(const AccumulationExpression&,
                                                            StringDataMap<SbExpr>,
                                                            boost::optional<sbe::value::SlotId>,
                                                            StageBuilderState&)>;

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
        {AccumulatorRank::kName, &buildAccumulatorRank},
        {AccumulatorDenseRank::kName, &buildAccumulatorDenseRank},
    };

    auto accExprName = acc.expr.name;
    uassert(5807017,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << accExprName,
            kAccumulatorBuilders.find(accExprName) != kAccumulatorBuilders.end());

    auto& fn = kAccumulatorBuilders.at(accExprName);
    return std::invoke(fn, acc.expr, std::move(argExprs), collatorSlot, state);
}

std::vector<BlockAggAndRowAgg> buildBlockAccumulator(
    const AccumulationStatement& acc,
    StringDataMap<SbExpr> argExprs,
    SbSlot bitmapInternalSlot,
    SbSlot accInternalSlot,
    boost::optional<sbe::value::SlotId> collatorSlot,
    StageBuilderState& state) {
    using BuildBlockAccumulatorFn =
        std::function<std::vector<BlockAggAndRowAgg>(const AccumulationExpression&,
                                                     StringDataMap<SbExpr>,
                                                     SbSlot,
                                                     SbSlot,
                                                     boost::optional<sbe::value::SlotId>,
                                                     StageBuilderState&)>;

    static const StringDataMap<BuildBlockAccumulatorFn> kBlockAccumulatorBuilders = {};

    auto accExprName = acc.expr.name;

    auto it = kBlockAccumulatorBuilders.find(accExprName);
    if (it != kBlockAccumulatorBuilders.end()) {
        auto& fn = it->second;
        return std::invoke(fn,
                           acc.expr,
                           std::move(argExprs),
                           bitmapInternalSlot,
                           accInternalSlot,
                           collatorSlot,
                           state);
    }

    return {};
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulator(
    const AccumulationStatement& acc,
    StringDataMap<std::unique_ptr<sbe::EExpression>> argExprs,
    boost::optional<sbe::value::SlotId> collatorSlot,
    StageBuilderState& state) {
    // Convert 'argExprs' into a StringDataMap of SbExprs.
    StringDataMap<SbExpr> argSbExprs;
    for (auto&& [k, v] : argExprs) {
        argSbExprs.emplace(k, SbExpr{v->clone()});
    }

    // Call the other overload of buildAccumulator().
    auto sbExprs = buildAccumulator(acc, std::move(argSbExprs), collatorSlot, state);

    // Convert 'sbExprs' to a vector of sbe::EExpressions and return it.
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    for (auto&& e : sbExprs) {
        exprs.emplace_back(e.extractExpr(state));
    }

    return exprs;
}

SbExpr::Vector buildCombinePartialAggregates(const AccumulationStatement& acc,
                                             const SbSlotVector& inputSlots,
                                             boost::optional<sbe::value::SlotId> collatorSlot,
                                             StageBuilderState& state) {
    using BuildAggCombinerFn = std::function<SbExpr::Vector(const AccumulationExpression&,
                                                            const SbSlotVector&,
                                                            boost::optional<sbe::value::SlotId>,
                                                            StageBuilderState&)>;

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
        kAggCombinerBuilders.at(accExprName), acc.expr, inputSlots, collatorSlot, state);
}

SbExpr::Vector buildCombinePartialAggregates(const AccumulationStatement& acc,
                                             const SbSlotVector& inputSlots,
                                             StringDataMap<SbExpr> argExprs,
                                             boost::optional<sbe::value::SlotId> collatorSlot,
                                             StageBuilderState& state) {
    using BuildAggCombinerFn = std::function<SbExpr::Vector(const AccumulationExpression&,
                                                            const SbSlotVector&,
                                                            StringDataMap<SbExpr>,
                                                            boost::optional<sbe::value::SlotId>,
                                                            StageBuilderState&)>;

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
                       state);
}

SbExpr buildFinalize(StageBuilderState& state,
                     const AccumulationStatement& acc,
                     const sbe::value::SlotVector& aggSlots,
                     boost::optional<sbe::value::SlotId> collatorSlot) {
    SbSlotVector aggSlotsVec;
    for (auto&& slot : aggSlots) {
        aggSlotsVec.emplace_back(SbSlot{slot});
    }

    return buildFinalize(state, acc, aggSlotsVec, collatorSlot);
}

SbExpr buildFinalize(StageBuilderState& state,
                     const AccumulationStatement& acc,
                     const SbSlotVector& aggSlots,
                     boost::optional<sbe::value::SlotId> collatorSlot) {
    using BuildFinalizeFn = std::function<SbExpr(StageBuilderState&,
                                                 const AccumulationExpression&,
                                                 const SbSlotVector&,
                                                 boost::optional<sbe::value::SlotId>)>;

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
        return std::invoke(fn, state, acc.expr, aggSlots, collatorSlot);
    } else {
        // Returning a null SbExpr signifies that no final project is necessary.
        return {};
    }
}

SbExpr buildFinalize(StageBuilderState& state,
                     const AccumulationStatement& acc,
                     const SbSlotVector& aggSlots,
                     StringDataMap<SbExpr> argExprs,
                     boost::optional<sbe::value::SlotId> collatorSlot) {
    using BuildFinalizeFn = std::function<SbExpr(StageBuilderState&,
                                                 const AccumulationExpression&,
                                                 const SbSlotVector&,
                                                 StringDataMap<SbExpr>,
                                                 boost::optional<sbe::value::SlotId>)>;

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
                       collatorSlot);
}

SbExpr buildFinalize(StageBuilderState& state,
                     const AccumulationStatement& acc,
                     const sbe::value::SlotVector& aggSlots,
                     StringDataMap<std::unique_ptr<sbe::EExpression>> argExprs,
                     boost::optional<sbe::value::SlotId> collatorSlot) {
    // Convert 'argExprs' into a StringDataMap of SbExprs and convert 'aggSlots' into
    // an SbSlotVector and then call the other overload of buildFinalize().
    StringDataMap<SbExpr> argSbExprs;
    SbSlotVector aggSlotsVec;

    for (auto&& [k, v] : argExprs) {
        argSbExprs.emplace(k, SbExpr{v->clone()});
    }
    for (auto&& slot : aggSlots) {
        aggSlotsVec.emplace_back(SbSlot{slot});
    }

    return buildFinalize(state, acc, aggSlotsVec, std::move(argSbExprs), collatorSlot);
}

SbExpr::Vector buildInitialize(const AccumulationStatement& acc,
                               SbExpr initExpr,
                               StageBuilderState& state) {
    using BuildInitializeFn = std::function<SbExpr::Vector(SbExpr, StageBuilderState&)>;

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

    return std::invoke(kAccumulatorBuilders.at(accExprName), std::move(initExpr), state);
}

std::vector<std::unique_ptr<sbe::EExpression>> buildInitialize(
    const AccumulationStatement& acc,
    std::unique_ptr<sbe::EExpression> initExpr,
    StageBuilderState& state) {
    // Call the other overload of buildAccumulator().
    auto sbExprs = buildInitialize(acc, SbExpr{std::move(initExpr)}, state);

    // Convert 'sbExprs' to a vector of sbe::EExpressions and return it.
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    for (auto&& e : sbExprs) {
        exprs.emplace_back(e.extractExpr(state));
    }
    return exprs;
}

SbExpr::Vector buildInitialize(const AccumulationStatement& acc,
                               StringDataMap<SbExpr> argExprs,
                               StageBuilderState& state) {
    using BuildInitializeFn =
        std::function<SbExpr::Vector(StringDataMap<SbExpr>, StageBuilderState&)>;

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

    return std::invoke(kAccumulatorBuilders.at(accExprName), std::move(argExprs), state);
}

std::vector<std::unique_ptr<sbe::EExpression>> buildInitialize(
    const AccumulationStatement& acc,
    StringDataMap<std::unique_ptr<sbe::EExpression>> argExprs,
    StageBuilderState& state) {
    // Convert 'argExprs' into a StringDataMap of SbExprs.
    StringDataMap<SbExpr> argSbExprs;
    for (auto&& [k, v] : argExprs) {
        argSbExprs.emplace(k, SbExpr{v->clone()});
    }

    // Call the other overload of buildAccumulator().
    auto sbExprs = buildInitialize(acc, std::move(argSbExprs), state);

    // Convert 'sbExprs' to a vector of sbe::EExpressions and return it.
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    for (auto&& e : sbExprs) {
        exprs.emplace_back(e.extractExpr(state));
    }
    return exprs;
}
}  // namespace mongo::stage_builder
