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
#include <type_traits>

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
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/db/query/sbe_stage_builder_accumulator.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/query/sbe_stage_builder_sbexpr_helpers.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo::stage_builder {
namespace Accum {
const StringData kCount = "count"_sd;
const StringData kCovarianceX = "covarianceX"_sd;
const StringData kCovarianceY = "covarianceY"_sd;
const StringData kDefaultVal = "defaultVal"_sd;
const StringData kInput = "input"_sd;
const StringData kInputFirst = "inputFirst"_sd;
const StringData kInputLast = "inputLast"_sd;
const StringData kIsAscending = "isAscending"_sd;
const StringData kIsGroupAccum = "isGroupAccum"_sd;
const StringData kMaxSize = "maxSize"_sd;
const StringData kSortBy = "sortBy"_sd;
const StringData kSortByFirst = "sortByFirst"_sd;
const StringData kSortByLast = "sortByLast"_sd;
const StringData kSortSpec = "sortSpec"_sd;
const StringData kUnit = "unit"_sd;
const StringData kValue = "value"_sd;

Inputs::~Inputs() {}

template <typename... Ts>
auto decodeNamedExprsMap(StringDataMap<std::unique_ptr<sbe::EExpression>> args,
                         Ts&&... expectedParamNames) {
    auto extractArg = [&](const auto& expectedParamName) {
        auto it = args.find(expectedParamName);
        uassert(8679700,
                str::stream() << "Expected parameter not found: " << expectedParamName,
                it != args.end());

        return SbExpr{std::move(it->second)};
    };

    return std::tuple(extractArg(expectedParamNames)...);
}

AccumSingleInput::AccumSingleInput(StringDataMap<std::unique_ptr<sbe::EExpression>> args) {
    std::tie(inputExpr) = decodeNamedExprsMap(std::move(args), kInput);
}

AccumAggsAvgInputs::AccumAggsAvgInputs(StringDataMap<std::unique_ptr<sbe::EExpression>> args) {
    std::tie(inputExpr, count) = decodeNamedExprsMap(std::move(args), kInput, kCount);
}

AccumCovarianceInputs::AccumCovarianceInputs(
    StringDataMap<std::unique_ptr<sbe::EExpression>> args) {
    std::tie(covarianceX, covarianceY) =
        decodeNamedExprsMap(std::move(args), kCovarianceX, kCovarianceY);
}

AccumRankInputs::AccumRankInputs(StringDataMap<std::unique_ptr<sbe::EExpression>> args) {
    std::tie(inputExpr, isAscending) = decodeNamedExprsMap(std::move(args), kInput, kIsAscending);
}

AccumIntegralInputs::AccumIntegralInputs(StringDataMap<std::unique_ptr<sbe::EExpression>> args) {
    std::tie(inputExpr, sortBy) = decodeNamedExprsMap(std::move(args), kInput, kSortBy);
}

AccumLinearFillInputs::AccumLinearFillInputs(
    StringDataMap<std::unique_ptr<sbe::EExpression>> args) {
    std::tie(inputExpr, sortBy) = decodeNamedExprsMap(std::move(args), kInput, kSortBy);
}

AccumTopBottomNInputs::AccumTopBottomNInputs(
    StringDataMap<std::unique_ptr<sbe::EExpression>> args) {
    std::tie(value, sortBy, sortSpec) =
        decodeNamedExprsMap(std::move(args), kValue, kSortBy, kSortSpec);
}

InitAccumNInputs::InitAccumNInputs(StringDataMap<std::unique_ptr<sbe::EExpression>> args) {
    std::tie(maxSize, isGroupAccum) = decodeNamedExprsMap(std::move(args), kMaxSize, kIsGroupAccum);
}

InitExpMovingAvgInputs::InitExpMovingAvgInputs(
    StringDataMap<std::unique_ptr<sbe::EExpression>> args) {
    std::tie(inputExpr) = decodeNamedExprsMap(std::move(args), kInput);
}

InitIntegralInputs::InitIntegralInputs(StringDataMap<std::unique_ptr<sbe::EExpression>> args) {
    std::tie(inputExpr) = decodeNamedExprsMap(std::move(args), kInput);
}

FinalizeTopBottomNInputs::FinalizeTopBottomNInputs(
    StringDataMap<std::unique_ptr<sbe::EExpression>> args) {
    std::tie(sortSpec) = decodeNamedExprsMap(std::move(args), kSortSpec);
}

FinalizeDerivativeInputs::FinalizeDerivativeInputs(
    StringDataMap<std::unique_ptr<sbe::EExpression>> args) {
    std::tie(unit, inputFirst, sortByFirst, inputLast, sortByLast) = decodeNamedExprsMap(
        std::move(args), kUnit, kInputFirst, kSortByFirst, kInputLast, kSortByLast);
}

FinalizeLinearFillInputs::FinalizeLinearFillInputs(
    StringDataMap<std::unique_ptr<sbe::EExpression>> args) {
    std::tie(sortBy) = decodeNamedExprsMap(std::move(args), kSortBy);
}

CombineAggsTopBottomNInputs::CombineAggsTopBottomNInputs(
    StringDataMap<std::unique_ptr<sbe::EExpression>> args) {
    std::tie(sortSpec) = decodeNamedExprsMap(std::move(args), kSortSpec);
}

namespace {
template <typename ReturnT, typename InputsT, typename... Args>
using BuildFnType =
    std::function<ReturnT(const Op&, std::unique_ptr<InputsT>, StageBuilderState&, Args...)>;

template <typename ReturnT, typename... Args>
using BuildNoInputsFnType = std::function<ReturnT(const Op&, StageBuilderState&, Args...)>;

// Define std::function types for functions whose inputs are passed in via 'unique_ptr<T>'
// (where 'T' is a class derived from the Inputs base class).
template <typename T>
using BuildAccumExprsFnType = BuildFnType<InputsPtr, T>;

template <typename T>
using BuildAccumBlockExprsFnType =
    BuildFnType<boost::optional<Accum::AccumBlockExprs>, T, const PlanStageSlots&>;

template <typename T>
using BuildAccumAggsFnType = BuildFnType<SbExpr::Vector, T>;

template <typename T>
using BuildAccumBlockAggsFnType =
    BuildFnType<boost::optional<std::vector<BlockAggAndRowAgg>>, T, SbSlot, SbSlot>;

template <typename T>
using BuildInitFnType = BuildFnType<SbExpr::Vector, T>;

template <typename T>
using BuildFinalizeFnType = BuildFnType<SbExpr, T, const SbSlotVector&>;

template <typename T>
using BuildCombineAggsFnType = BuildFnType<SbExpr::Vector, T, const SbSlotVector&>;

// Define std::function types for functions that don't take any inputs.
using BuildAccumExprsNoInputsFn = BuildNoInputsFnType<InputsPtr>;
using BuildAccumBlockExprsNoInputsFn =
    BuildNoInputsFnType<boost::optional<Accum::AccumBlockExprs>, const PlanStageSlots&>;
using BuildAccumAggsNoInputsFn = BuildNoInputsFnType<std::vector<BlockAggAndRowAgg>>;
using BuildAccumBlockAggsNoInputsFn =
    BuildNoInputsFnType<boost::optional<std::vector<BlockAggAndRowAgg>>, SbSlot, SbSlot>;
using BuildInitNoInputsFn = BuildNoInputsFnType<SbExpr::Vector>;
using BuildFinalizeNoInputsFn = BuildNoInputsFnType<SbExpr, const SbSlotVector&>;
using BuildCombineAggsNoInputsFn = BuildNoInputsFnType<SbExpr::Vector, const SbSlotVector&>;

// Define std::function types for functions whose inputs are passed in via 'InputsPtr'
// (aka 'unique_ptr<Inputs>').
using BuildAccumExprsFn = BuildAccumExprsFnType<Inputs>;
using BuildAccumBlockExprsFn = BuildAccumBlockExprsFnType<Inputs>;
using BuildAccumAggsFn = BuildAccumAggsFnType<Inputs>;
using BuildAccumBlockAggsFn = BuildAccumBlockAggsFnType<Inputs>;
using BuildInitFn = BuildInitFnType<Inputs>;
using BuildFinalizeFn = BuildFinalizeFnType<Inputs>;
using BuildCombineAggsFn = BuildCombineAggsFnType<Inputs>;

template <typename T>
std::unique_ptr<T> castInputsTo(InputsPtr inputs) {
    // Try casting 'inputs.get()' to T* and check if the cast was succeesful.
    const bool castSuccessful = inputs && dynamic_cast<T*>(inputs.get()) != nullptr;

    constexpr bool isConstructibleFromNamedExprsMap =
        std::is_constructible_v<T, StringDataMap<std::unique_ptr<sbe::EExpression>>>;

    // If the cast failed and the expected type is constructible from a named expr map,
    // check if 'inputs' is a NamedExprsMapWrapper.
    if constexpr (isConstructibleFromNamedExprsMap) {
        if (!castSuccessful && inputs) {
            if (auto wrapper = dynamic_cast<NamedExprsMapWrapper*>(inputs.get())) {
                // If 'inputs' is a NamedExprsMapWrapper and its possible to convert it
                // to the expected type, perform the conversion and return the result.
                return std::make_unique<T>(std::move(wrapper->args));
            }
        }
    }

    // If we reach this point, assert that the cast was castSuccessfulful.
    uassert(8679708, "Casting accumulator input to expected type failed", castSuccessful);

    // Extract the pointer from 'inputs', wrap it in a 'std::unique_ptr<T>' and return it.
    return std::unique_ptr<T>{static_cast<T*>(inputs.release())};
}

/**
 * The makeBuildFn() helper function takes a build callback and returns a "wrapped" build
 * callback.
 *
 * Given an original build callback with an "inputs" parameter of type 'std::unique_ptr<T>'
 * (where T is a subclass of Inputs), the "wrapped" build callback returned will have the same
 * parameters and return type as the original, except that the type of its "inputs" parameter
 * will be 'std::unique_ptr<Inputs>' instead.
 *
 * When invoked, the wrapped callback will check if the "inputs" arg provided by the caller can
 * be cast to the type of the original callback's "inputs" parameter. If the cast is possible,
 * the wrapped callback will perform the cast, invoke the original callback, and return whatever
 * the original callback returns. If the cast is not possible, the wrapped callback will uassert.
 */
template <typename ReturnT, typename T, typename... Args>
BuildFnType<ReturnT, Inputs, Args...> makeBuildFnImpl(BuildFnType<ReturnT, T, Args...> fn) {
    return [fn = std::move(fn)](
               const Op& acc, InputsPtr inputs, StageBuilderState& state, Args&&... args) {
        return fn(acc, castInputsTo<T>(std::move(inputs)), state, std::forward<Args>(args)...);
    };
}

template <typename ReturnT, typename... Args>
BuildFnType<ReturnT, Inputs, Args...> makeBuildFnImpl(BuildNoInputsFnType<ReturnT, Args...> fn) {
    return [fn = std::move(fn)](
               const Op& acc, InputsPtr inputs, StageBuilderState& state, Args&&... args) {
        return fn(acc, state, std::forward<Args>(args)...);
    };
}

template <typename FuncT>
auto makeBuildFn(FuncT fn) {
    return makeBuildFnImpl(std::function(std::move(fn)));
}
}  // namespace

/**
 * The OpInfo struct contains function pointers and other useful information about an Accum::Op.
 * The 'accumOpInfoMap' map (defined below) maps the name of each op to the corresponding OpInfo
 * for that op.
 */
struct OpInfo {
    size_t numAggs = 1;
    BuildAccumExprsFn buildAccumExprs = nullptr;
    BuildAccumBlockExprsFn buildAccumBlockExprs = nullptr;
    BuildAccumAggsFn buildAccumAggs = nullptr;
    BuildAccumBlockAggsFn buildAccumBlockAggs = nullptr;
    BuildInitFn buildInit = nullptr;
    BuildFinalizeFn buildFinalize = nullptr;
    BuildCombineAggsFn buildCombineAggs = nullptr;
};

namespace {
SbExpr wrapMinMaxArg(SbExpr arg, StageBuilderState& state) {
    SbExprBuilder b(state);

    auto frameId = state.frameIdGenerator->generate();
    auto binds = SbExpr::makeSeq(std::move(arg));
    auto var = SbVar(frameId, 0);

    auto e = b.makeIf(b.generateNullMissingOrUndefined(var), b.makeNothingConstant(), var);

    return b.makeLet(frameId, std::move(binds), std::move(e));
}

InputsPtr buildAccumExprsMinMax(const Op& acc,
                                std::unique_ptr<AccumSingleInput> inputs,
                                StageBuilderState& state) {
    inputs->inputExpr = wrapMinMaxArg(std::move(inputs->inputExpr), state);
    return inputs;
}

SbExpr::Vector buildAccumAggsMin(const Op& acc,
                                 std::unique_ptr<AccumSingleInput> inputs,
                                 StageBuilderState& state) {
    SbExprBuilder b(state);

    auto collatorSlot = state.getCollatorSlot();

    if (collatorSlot) {
        return SbExpr::makeSeq(
            b.makeFunction("collMin"_sd, SbVar{*collatorSlot}, std::move(inputs->inputExpr)));
    } else {
        return SbExpr::makeSeq(b.makeFunction("min"_sd, std::move(inputs->inputExpr)));
    }
}

boost::optional<std::vector<BlockAggAndRowAgg>> buildAccumBlockAggsMin(
    const Op& acc,
    std::unique_ptr<AccumSingleInput> inputs,
    StageBuilderState& state,
    SbSlot bitmapInternalSlot,
    SbSlot accInternalSlot) {
    SbExprBuilder b(state);
    boost::optional<std::vector<BlockAggAndRowAgg>> pairs;
    pairs.emplace();

    pairs->emplace_back(BlockAggAndRowAgg{
        b.makeFunction("valueBlockAggMin"_sd, bitmapInternalSlot, std::move(inputs->inputExpr)),
        b.makeFunction("min"_sd, accInternalSlot)});

    return pairs;
}

SbExpr::Vector buildCombineAggsMin(const Op& acc,
                                   StageBuilderState& state,
                                   const SbSlotVector& inputSlots) {
    tassert(7039501,
            "partial agg combiner for $min should have exactly one input slot",
            inputSlots.size() == 1);

    SbExprBuilder b(state);

    auto arg = wrapMinMaxArg(SbExpr{inputSlots[0]}, state);

    auto collatorSlot = state.getCollatorSlot();

    if (collatorSlot) {
        return SbExpr::makeSeq(b.makeFunction("collMin"_sd, SbVar{*collatorSlot}, std::move(arg)));
    } else {
        return SbExpr::makeSeq(b.makeFunction("min"_sd, std::move(arg)));
    }
}

SbExpr buildFinalizeMin(const Op& acc, StageBuilderState& state, const SbSlotVector& minSlots) {
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

SbExpr::Vector buildAccumAggsMax(const Op& acc,
                                 std::unique_ptr<AccumSingleInput> inputs,
                                 StageBuilderState& state) {
    SbExprBuilder b(state);

    auto collatorSlot = state.getCollatorSlot();

    if (collatorSlot) {
        return SbExpr::makeSeq(
            b.makeFunction("collMax"_sd, SbVar{*collatorSlot}, std::move(inputs->inputExpr)));
    } else {
        return SbExpr::makeSeq(b.makeFunction("max"_sd, std::move(inputs->inputExpr)));
    }
}

boost::optional<std::vector<BlockAggAndRowAgg>> buildAccumBlockAggsMax(
    const Op& acc,
    std::unique_ptr<AccumSingleInput> inputs,
    StageBuilderState& state,
    SbSlot bitmapInternalSlot,
    SbSlot accInternalSlot) {
    SbExprBuilder b(state);
    boost::optional<std::vector<BlockAggAndRowAgg>> pairs;
    pairs.emplace();

    pairs->emplace_back(BlockAggAndRowAgg{
        b.makeFunction("valueBlockAggMax"_sd, bitmapInternalSlot, std::move(inputs->inputExpr)),
        b.makeFunction("max"_sd, accInternalSlot)});

    return pairs;
}

SbExpr::Vector buildCombineAggsMax(const Op& acc,
                                   StageBuilderState& state,
                                   const SbSlotVector& inputSlots) {
    tassert(7039502,
            "partial agg combiner for $max should have exactly one input slot",
            inputSlots.size() == 1);

    SbExprBuilder b(state);

    auto arg = wrapMinMaxArg(SbExpr{inputSlots[0]}, state);

    auto collatorSlot = state.getCollatorSlot();

    if (collatorSlot) {
        return SbExpr::makeSeq(b.makeFunction("collMax"_sd, SbVar{*collatorSlot}, std::move(arg)));
    } else {
        return SbExpr::makeSeq(b.makeFunction("max"_sd, std::move(arg)));
    }
}

SbExpr buildFinalizeMax(const Op& acc, StageBuilderState& state, const SbSlotVector& maxSlots) {
    SbExprBuilder b(state);

    tassert(5755100,
            str::stream() << "Expected one input slot for finalization of max, got: "
                          << maxSlots.size(),
            maxSlots.size() == 1);
    return b.makeFillEmptyNull(maxSlots[0]);
}

InputsPtr buildAccumExprsFirstLast(const Op& acc,
                                   std::unique_ptr<AccumSingleInput> inputs,
                                   StageBuilderState& state) {
    SbExprBuilder b(state);

    inputs->inputExpr = b.makeFillEmptyNull(std::move(inputs->inputExpr));
    return inputs;
}

SbExpr::Vector buildAccumAggsFirst(const Op& acc,
                                   std::unique_ptr<AccumSingleInput> inputs,
                                   StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("first", std::move(inputs->inputExpr)));
}

SbExpr::Vector buildCombineAggsFirst(const Op& acc,
                                     StageBuilderState& state,
                                     const SbSlotVector& inputSlots) {
    tassert(7039503,
            "partial agg combiner for $first should have exactly one input slot",
            inputSlots.size() == 1);

    SbExprBuilder b(state);

    auto arg = b.makeFillEmptyNull(SbExpr{inputSlots[0]});
    return SbExpr::makeSeq(b.makeFunction("first", std::move(arg)));
}

SbExpr::Vector buildAccumAggsLast(const Op& acc,
                                  std::unique_ptr<AccumSingleInput> inputs,
                                  StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("last", std::move(inputs->inputExpr)));
}

SbExpr::Vector buildCombineAggsLast(const Op& acc,
                                    StageBuilderState& state,
                                    const SbSlotVector& inputSlots) {
    tassert(7039504,
            "partial agg combiner for $last should have exactly one input slot",
            inputSlots.size() == 1);

    SbExprBuilder b(state);

    auto arg = b.makeFillEmptyNull(SbExpr{inputSlots[0]});
    return SbExpr::makeSeq(b.makeFunction("last", std::move(arg)));
}

InputsPtr buildAccumExprsAvg(const Op& acc,
                             std::unique_ptr<AccumSingleInput> inputs,
                             StageBuilderState& state) {
    SbExprBuilder b(state);

    // Generate the addend expression.
    auto frameId = state.frameIdGenerator->generate();
    auto binds = SbExpr::makeSeq(inputs->inputExpr.clone());
    auto var = SbVar{frameId, 0};

    auto e = b.makeIf(b.makeBinaryOp(sbe::EPrimBinary::logicOr,
                                     b.generateNullMissingOrUndefined(var),
                                     b.generateNonNumericCheck(var)),
                      b.makeInt64Constant(0),
                      b.makeInt64Constant(1));

    // For the counter we need to skip non-numeric values ourselves.
    auto addend = b.makeLet(frameId, std::move(binds), std::move(e));

    // Use 'inputs->inputExpr' as the input for the "aggDoubleDoubleSum()" agg and use 'addend'
    // as the input for the "sum()" agg.
    return std::make_unique<AccumAggsAvgInputs>(std::move(inputs->inputExpr), std::move(addend));
}

SbExpr::Vector buildAccumAggsAvg(const Op& acc,
                                 std::unique_ptr<AccumAggsAvgInputs> inputs,
                                 StageBuilderState& state) {
    SbExprBuilder b(state);

    SbExpr::Vector aggs;
    aggs.push_back(b.makeFunction("aggDoubleDoubleSum", std::move(inputs->inputExpr)));
    aggs.push_back(b.makeFunction("sum", std::move(inputs->count)));

    return aggs;
}

SbExpr::Vector buildCombineAggsAvg(const Op& acc,
                                   StageBuilderState& state,
                                   const SbSlotVector& inputSlots) {
    SbExprBuilder b(state);

    tassert(7039539,
            "partial agg combiner for $avg should have exactly two input slots",
            inputSlots.size() == 2);

    return SbExpr::makeSeq(b.makeFunction("aggMergeDoubleDoubleSums", inputSlots[0]),
                           b.makeFunction("sum", inputSlots[1]));
}

SbExpr buildFinalizeAvg(const Op& acc, StageBuilderState& state, const SbSlotVector& aggSlots) {
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

SbExpr::Vector buildAccumAggsSum(const Op& acc,
                                 std::unique_ptr<AccumSingleInput> inputs,
                                 StageBuilderState& state) {
    SbExprBuilder b(state);

    if (acc.countAddendIsIntegerOrDouble()) {
        // Optimize for a count-like accumulator like {$sum: 1}.
        return SbExpr::makeSeq(b.makeFunction("sum", std::move(inputs->inputExpr)));
    } else {
        return SbExpr::makeSeq(b.makeFunction("aggDoubleDoubleSum", std::move(inputs->inputExpr)));
    }
}

SbExpr::Vector buildCombineAggsSum(const Op& acc,
                                   StageBuilderState& state,
                                   const SbSlotVector& inputSlots) {
    SbExprBuilder b(state);

    tassert(7039530,
            "partial agg combiner for $sum should have exactly one input slot",
            inputSlots.size() == 1);
    auto arg = inputSlots[0];

    // Optimize for a count-like accumulator like {$sum: 1}. In particular, we will spill the
    // constant sum, and we need to convert it to a 4 element array that can be used to initialize a
    // DoubleDoubleSummation.
    if (acc.countAddendIsIntegerOrDouble()) {
        return SbExpr::makeSeq(b.makeFunction("convertSimpleSumToDoubleDoubleSum", std::move(arg)));
    } else {
        return SbExpr::makeSeq(b.makeFunction("aggMergeDoubleDoubleSums", std::move(arg)));
    }
}

SbExpr buildFinalizeSum(const Op& acc, StageBuilderState& state, const SbSlotVector& sumSlots) {
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

    if (acc.countAddendIsIntegerOrDouble()) {
        auto var = makeVariable(sumSlots[0]);
        return sbe::makeE<sbe::EIf>(makeFunction("isNumber", var->clone()),
                                    var->clone(),
                                    makeFunction("doubleDoubleSumFinalize", var->clone()));
    } else {
        return b.makeFunction("doubleDoubleSumFinalize", sumSlots[0]);
    }
}

SbExpr::Vector buildAccumAggsAddToSetHelper(SbExpr arg,
                                            StringData funcName,
                                            StringData funcNameWithCollator,
                                            StageBuilderState& state) {
    SbExprBuilder b(state);

    const int cap = internalQueryMaxAddToSetBytes.load();

    auto collatorSlot = state.getCollatorSlot();

    if (collatorSlot) {
        return SbExpr::makeSeq(b.makeFunction(
            funcNameWithCollator, SbVar{*collatorSlot}, std::move(arg), b.makeInt32Constant(cap)));
    } else {
        return SbExpr::makeSeq(b.makeFunction(funcName, std::move(arg), b.makeInt32Constant(cap)));
    }
}

SbExpr::Vector buildAccumAggsAddToSet(const Op& acc,
                                      std::unique_ptr<AccumSingleInput> inputs,
                                      StageBuilderState& state) {
    return buildAccumAggsAddToSetHelper(
        std::move(inputs->inputExpr), "addToSetCapped"_sd, "collAddToSetCapped"_sd, state);
}

SbExpr::Vector buildCombineAggsAddToSet(const Op& acc,
                                        StageBuilderState& state,
                                        const SbSlotVector& inputSlots) {
    tassert(7039506,
            "partial agg combiner for $addToSet should have exactly one input slot",
            inputSlots.size() == 1);
    auto arg = inputSlots[0];
    return buildAccumAggsAddToSetHelper(
        std::move(arg), "aggSetUnionCapped"_sd, "aggCollSetUnionCapped"_sd, state);
}

SbExpr buildFinalizeCappedAccumulator(const Op& acc,
                                      StageBuilderState& state,
                                      const SbSlotVector& accSlots) {
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

SbExpr::Vector buildAccumAggsPushHelper(SbExpr arg,
                                        StringData aggFuncName,
                                        StageBuilderState& state) {
    SbExprBuilder b(state);

    const int cap = internalQueryMaxPushBytes.load();
    return SbExpr::makeSeq(b.makeFunction(aggFuncName, std::move(arg), b.makeInt32Constant(cap)));
}

SbExpr::Vector buildAccumAggsPush(const Op& acc,
                                  std::unique_ptr<AccumSingleInput> inputs,
                                  StageBuilderState& state) {
    return buildAccumAggsPushHelper(std::move(inputs->inputExpr), "addToArrayCapped"_sd, state);
}

SbExpr::Vector buildCombineAggsPush(const Op& acc,
                                    StageBuilderState& state,
                                    const SbSlotVector& inputSlots) {
    tassert(7039505,
            "partial agg combiner for $push should have exactly one input slot",
            inputSlots.size() == 1);

    auto arg = inputSlots[0];
    return buildAccumAggsPushHelper(std::move(arg), "aggConcatArraysCapped"_sd, state);
}

SbExpr::Vector buildAccumAggsStdDev(const Op& acc,
                                    std::unique_ptr<AccumSingleInput> inputs,
                                    StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("aggStdDev", std::move(inputs->inputExpr)));
}

SbExpr::Vector buildCombineAggsStdDev(const Op& acc,
                                      StageBuilderState& state,
                                      const SbSlotVector& inputSlots) {
    SbExprBuilder b(state);

    tassert(7039540,
            "partial agg combiner for stddev should have exactly one input slot",
            inputSlots.size() == 1);
    auto arg = inputSlots[0];
    return SbExpr::makeSeq(b.makeFunction("aggMergeStdDevs", arg));
}

SbExpr buildFinalizePartialStdDevHelper(SbSlot stdDevSlot, StageBuilderState& state) {
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

SbExpr buildFinalizeStdDevPop(const Op& acc,
                              StageBuilderState& state,
                              const SbSlotVector& stdDevSlots) {
    SbExprBuilder b(state);

    tassert(5755204,
            str::stream() << "Expected one input slot for finalization of stdDevPop, got: "
                          << stdDevSlots.size(),
            stdDevSlots.size() == 1);

    if (state.needsMerge) {
        return buildFinalizePartialStdDevHelper(stdDevSlots[0], state);
    } else {
        auto stdDevPopFinalize = b.makeFunction("stdDevPopFinalize", stdDevSlots[0]);
        return stdDevPopFinalize;
    }
}

SbExpr buildFinalizeStdDevSamp(const Op& acc,
                               StageBuilderState& state,
                               const SbSlotVector& stdDevSlots) {
    SbExprBuilder b(state);

    tassert(5755209,
            str::stream() << "Expected one input slot for finalization of stdDevSamp, got: "
                          << stdDevSlots.size(),
            stdDevSlots.size() == 1);

    if (state.needsMerge) {
        return buildFinalizePartialStdDevHelper(stdDevSlots[0], state);
    } else {
        return b.makeFunction("stdDevSampFinalize", stdDevSlots[0]);
    }
}

SbExpr wrapMergeObjectsArg(SbExpr arg, StageBuilderState& state) {
    SbExprBuilder b(state);

    auto frameId = state.frameIdGenerator->generate();
    auto binds = SbExpr::makeSeq(std::move(arg));
    auto var = SbVar{frameId, 0};

    auto expr =
        b.makeIf(b.makeBinaryOp(sbe::EPrimBinary::logicOr,
                                b.generateNullMissingOrUndefined(var),
                                b.makeFunction("isObject", var)),
                 SbExpr{var},
                 b.makeFail(ErrorCodes::Error{5911200}, "$mergeObjects only supports objects"));

    return b.makeLet(frameId, std::move(binds), std::move(expr));
}

InputsPtr buildAccumExprsMergeObjects(const Op& acc,
                                      std::unique_ptr<AccumSingleInput> inputs,
                                      StageBuilderState& state) {
    inputs->inputExpr = wrapMergeObjectsArg(std::move(inputs->inputExpr), state);
    return inputs;
}

SbExpr::Vector buildAccumAggsMergeObjects(const Op& acc,
                                          std::unique_ptr<AccumSingleInput> inputs,
                                          StageBuilderState& state) {
    SbExprBuilder b(state);

    return SbExpr::makeSeq(b.makeFunction("mergeObjects", std::move(inputs->inputExpr)));
}

SbExpr::Vector buildCombineAggsMergeObjects(const Op& acc,
                                            StageBuilderState& state,
                                            const SbSlotVector& inputSlots) {
    tassert(7039507,
            "partial agg combiner for $mergeObjects should have exactly one input slot",
            inputSlots.size() == 1);

    SbExprBuilder b(state);

    return SbExpr::makeSeq(
        b.makeFunction("mergeObjects", wrapMergeObjectsArg(SbExpr{inputSlots[0]}, state)));
}

SbExpr::Vector buildInitializeAccumN(const Op& acc,
                                     std::unique_ptr<InitAccumNInputs> inputs,
                                     StageBuilderState& state) {
    SbExprBuilder b(state);

    auto maxSizeExpr = std::move(inputs->maxSize);
    auto isGroupAccumExpr = std::move(inputs->isGroupAccum);

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

SbExpr::Vector buildAccumAggsFirstN(const Op& acc,
                                    std::unique_ptr<AccumSingleInput> inputs,
                                    StageBuilderState& state) {
    SbExprBuilder b(state);

    auto frameId = state.frameIdGenerator->generate();
    auto binds = SbExpr::makeSeq(b.makeFunction("aggState"));

    auto varExpr = SbExpr{SbVar{frameId, 0}};
    auto moveVarExpr = SbExpr{makeMoveVariable(frameId, 0)};

    auto e = b.makeIf(b.makeFunction("aggFirstNNeedsMoreInput", std::move(varExpr)),
                      b.makeFunction("aggFirstN",
                                     moveVarExpr.clone(),
                                     b.makeFillEmptyNull(std::move(inputs->inputExpr))),
                      moveVarExpr.clone());

    return SbExpr::makeSeq(b.makeLet(frameId, std::move(binds), std::move(e)));
}

SbExpr::Vector buildCombineAggsFirstN(const Op& acc,
                                      StageBuilderState& state,
                                      const SbSlotVector& inputSlots) {
    SbExprBuilder b(state);

    uassert(7548608,
            str::stream() << "Expected one input slot for merging $firstN, got: "
                          << inputSlots.size(),
            inputSlots.size() == 1);

    return SbExpr::makeSeq(b.makeFunction("aggFirstNMerge", inputSlots[0]));
}

SbExpr buildFinalizeFirstN(const Op& acc,
                           StageBuilderState& state,
                           const SbSlotVector& inputSlots) {
    SbExprBuilder b(state);

    uassert(7548609,
            str::stream() << "Expected one input slot for finalization of $firstN, got: "
                          << inputSlots.size(),
            inputSlots.size() == 1);
    return b.makeFunction("aggFirstNFinalize", inputSlots[0]);
}

InputsPtr buildAccumExprsLastN(const Op& acc,
                               std::unique_ptr<AccumSingleInput> inputs,
                               StageBuilderState& state) {
    SbExprBuilder b(state);

    inputs->inputExpr = b.makeFillEmptyNull(std::move(inputs->inputExpr));
    return inputs;
}

SbExpr::Vector buildAccumAggsLastN(const Op& acc,
                                   std::unique_ptr<AccumSingleInput> inputs,
                                   StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("aggLastN", std::move(inputs->inputExpr)));
}

SbExpr::Vector buildCombineAggsLastN(const Op& acc,
                                     StageBuilderState& state,
                                     const SbSlotVector& inputSlots) {
    SbExprBuilder b(state);

    uassert(7548701,
            str::stream() << "Expected one input slot for merging $lastN, got: "
                          << inputSlots.size(),
            inputSlots.size() == 1);

    return SbExpr::makeSeq(b.makeFunction("aggLastNMerge", inputSlots[0]));
}

SbExpr buildFinalizeLastN(const Op& acc, StageBuilderState& state, const SbSlotVector& inputSlots) {
    SbExprBuilder b(state);

    uassert(7548702,
            str::stream() << "Expected one input slot for finalization of $lastN, got: "
                          << inputSlots.size(),
            inputSlots.size() == 1);
    return b.makeFunction("aggLastNFinalize", inputSlots[0]);
}

bool isAccumulatorTopN(const Op& acc) {
    const auto& name = acc.getOpName();
    return name == AccumulatorTopN::getName() || name == AccumulatorTop::getName();
}

SbExpr::Vector buildAccumAggsTopBottomN(const Op& acc,
                                        std::unique_ptr<AccumTopBottomNInputs> inputs,
                                        StageBuilderState& state) {
    SbExprBuilder b(state);

    auto value = std::move(inputs->value);
    auto key = std::move(inputs->sortBy);
    auto sortSpec = std::move(inputs->sortSpec);

    return SbExpr::makeSeq(b.makeFunction(isAccumulatorTopN(acc) ? "aggTopN" : "aggBottomN",
                                          std::move(key),
                                          std::move(value),
                                          std::move(sortSpec)));
}

SbExpr::Vector buildCombineAggsTopBottomN(const Op& acc,
                                          std::unique_ptr<CombineAggsTopBottomNInputs> inputs,
                                          StageBuilderState& state,
                                          const SbSlotVector& inputSlots) {
    SbExprBuilder b(state);

    tassert(5807011,
            str::stream() << "Expected one input slot for merging " << acc.getOpName()
                          << ", got: " << inputSlots.size(),
            inputSlots.size() == 1);

    auto sortSpec = std::move(inputs->sortSpec);

    return SbExpr::makeSeq(
        b.makeFunction(isAccumulatorTopN(acc) ? "aggTopNMerge" : "aggBottomNMerge",
                       inputSlots[0],
                       std::move(sortSpec)));
}

SbExpr buildFinalizeTopBottomNImpl(const Op& acc,
                                   std::unique_ptr<FinalizeTopBottomNInputs> inputs,
                                   StageBuilderState& state,
                                   const SbSlotVector& inputSlots,
                                   bool single) {
    SbExprBuilder b(state);

    tassert(5807012,
            str::stream() << "Expected one input slot for finalization of " << acc.getOpName()
                          << ", got: " << inputSlots.size(),
            inputSlots.size() == 1);
    auto inputVar = inputSlots[0];

    auto sortSpec = std::move(inputs->sortSpec);

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
            b.makeFunction(isAccumulatorTopN(acc) ? "aggTopNFinalize" : "aggBottomNFinalize",
                           inputVar,
                           std::move(sortSpec));
        if (single) {
            finalExpr = b.makeFunction("getElement", std::move(finalExpr), b.makeInt32Constant(0));
        }
        return finalExpr;
    }
}

SbExpr buildFinalizeTopBottomN(const Op& acc,
                               std::unique_ptr<FinalizeTopBottomNInputs> inputs,
                               StageBuilderState& state,
                               const SbSlotVector& inputSlots) {
    return buildFinalizeTopBottomNImpl(acc, std::move(inputs), state, inputSlots, false);
}

SbExpr buildFinalizeTopBottom(const Op& acc,
                              std::unique_ptr<FinalizeTopBottomNInputs> inputs,
                              StageBuilderState& state,
                              const SbSlotVector& inputSlots) {
    return buildFinalizeTopBottomNImpl(acc, std::move(inputs), state, inputSlots, true);
}

InputsPtr buildAccumExprsMinMaxN(const Op& acc,
                                 std::unique_ptr<AccumSingleInput> inputs,
                                 StageBuilderState& state) {
    SbExprBuilder b(state);

    inputs->inputExpr = b.makeFunction("setToArray", std::move(inputs->inputExpr));
    return inputs;
}

SbExpr::Vector buildAccumAggsMinMaxN(const Op& acc,
                                     std::unique_ptr<AccumSingleInput> inputs,
                                     StageBuilderState& state) {
    SbExprBuilder b(state);

    auto aggExprName = acc.getOpName() == AccumulatorMaxN::kName ? "aggMaxN" : "aggMinN";

    auto collatorSlot = state.getCollatorSlot();

    if (collatorSlot) {
        return SbExpr::makeSeq(b.makeFunction(
            std::move(aggExprName), std::move(inputs->inputExpr), SbVar{*collatorSlot}));
    } else {
        return SbExpr::makeSeq(
            b.makeFunction(std::move(aggExprName), std::move(inputs->inputExpr)));
    }
}

SbExpr::Vector buildCombineAggsMinMaxN(const Op& acc,
                                       StageBuilderState& state,
                                       const SbSlotVector& inputSlots) {
    SbExprBuilder b(state);

    uassert(7548808,
            str::stream() << "Expected one input slot for merging, got: " << inputSlots.size(),
            inputSlots.size() == 1);

    auto aggExprName = acc.getOpName() == AccumulatorMaxN::kName ? "aggMaxNMerge" : "aggMinNMerge";

    auto collatorSlot = state.getCollatorSlot();

    if (collatorSlot) {
        return SbExpr::makeSeq(
            b.makeFunction(std::move(aggExprName), inputSlots[0], SbVar{*collatorSlot}));
    } else {
        return SbExpr::makeSeq(b.makeFunction(std::move(aggExprName), inputSlots[0]));
    }
}

SbExpr buildFinalizeMinMaxN(const Op& acc,
                            StageBuilderState& state,
                            const SbSlotVector& inputSlots) {
    SbExprBuilder b(state);

    uassert(7548809,
            str::stream() << "Expected one input slot for finalization, got: " << inputSlots.size(),
            inputSlots.size() == 1);
    auto aggExprName =
        acc.getOpName() == AccumulatorMaxN::kName ? "aggMaxNFinalize" : "aggMinNFinalize";

    auto collatorSlot = state.getCollatorSlot();

    if (collatorSlot) {
        return b.makeFunction(std::move(aggExprName), inputSlots[0], SbVar{*collatorSlot});
    } else {
        return b.makeFunction(std::move(aggExprName), inputSlots[0]);
    }
}

SbExpr::Vector buildAccumAggsCovariance(const Op& acc,
                                        std::unique_ptr<AccumCovarianceInputs> inputs,
                                        StageBuilderState& state) {
    auto argX = std::move(inputs->covarianceX);
    auto argY = std::move(inputs->covarianceY);

    SbExprBuilder b(state);

    return SbExpr::makeSeq(b.makeFunction("aggCovarianceAdd", std::move(argX), std::move(argY)));
}

SbExpr buildFinalizeCovarianceSamp(const Op& acc,
                                   StageBuilderState& state,
                                   const SbSlotVector& slots) {
    SbExprBuilder b(state);

    tassert(7820814, "Incorrect number of arguments", slots.size() == 1);
    SbExpr::Vector exprs;
    for (auto slot : slots) {
        exprs.push_back(slot);
    }
    return b.makeFunction("aggCovarianceSampFinalize", std::move(exprs));
}

SbExpr buildFinalizeCovariancePop(const Op& acc,
                                  StageBuilderState& state,
                                  const SbSlotVector& slots) {
    SbExprBuilder b(state);

    tassert(7820815, "Incorrect number of arguments", slots.size() == 1);
    SbExpr::Vector exprs;
    for (auto slot : slots) {
        exprs.push_back(slot);
    }
    return b.makeFunction("aggCovariancePopFinalize", std::move(exprs));
}

SbExpr::Vector buildInitializeExpMovingAvg(const Op& acc,
                                           std::unique_ptr<InitExpMovingAvgInputs> inputs,
                                           StageBuilderState& state) {
    SbExprBuilder b(state);

    auto alphaExpr = std::move(inputs->inputExpr);

    return SbExpr::makeSeq(b.makeFunction(
        "newArray", b.makeNullConstant(), std::move(alphaExpr), b.makeBoolConstant(false)));
}

SbExpr::Vector buildAccumAggsExpMovingAvg(const Op& acc,
                                          std::unique_ptr<AccumSingleInput> inputs,
                                          StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("aggExpMovingAvg", std::move(inputs->inputExpr)));
}

SbExpr buildFinalizeExpMovingAvg(const Op& acc,
                                 StageBuilderState& state,
                                 const SbSlotVector& slots) {
    SbExprBuilder b(state);
    tassert(7996802, "Incorrect number of arguments", slots.size() == 1);
    return b.makeFunction("aggExpMovingAvgFinalize", slots[0]);
}

SbExpr::Vector buildAccumAggsLocf(const Op& acc,
                                  std::unique_ptr<AccumSingleInput> inputs,
                                  StageBuilderState& state) {
    SbExprBuilder b(state);

    auto frameId = state.frameIdGenerator->generate();
    auto binds = SbExpr::makeSeq(std::move(inputs->inputExpr));
    auto var = SbVar{frameId, 0};

    auto e = b.makeIf(b.generateNullMissingOrUndefined(var), b.makeFunction("aggState"), var);

    auto localBind = b.makeLet(frameId, std::move(binds), std::move(e));

    return SbExpr::makeSeq(std::move(localBind));
}

SbExpr::Vector buildAccumAggsDocumentNumber(const Op& acc,
                                            std::unique_ptr<AccumSingleInput> inputs,
                                            StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("sum", b.makeInt64Constant(1)));
}

SbExpr::Vector buildAccumAggsRankImpl(const StringData rankFuncName,
                                      const StringData collRankFuncName,
                                      const Op& acc,
                                      SbExpr input,
                                      SbExpr sortOrder,
                                      StageBuilderState& state) {
    SbExprBuilder b(state);

    auto collatorSlot = state.getCollatorSlot();

    if (collatorSlot) {
        return SbExpr::makeSeq(b.makeFunction(
            collRankFuncName, std::move(input), std::move(sortOrder), SbVar{*collatorSlot}));
    } else {
        return SbExpr::makeSeq(
            b.makeFunction(rankFuncName, std::move(input), std::move(sortOrder)));
    }
}

SbExpr::Vector buildAccumAggsRank(const Op& acc,
                                  std::unique_ptr<AccumRankInputs> inputs,
                                  StageBuilderState& state) {
    auto input = std::move(inputs->inputExpr);
    auto sortOrder = std::move(inputs->isAscending);

    return buildAccumAggsRankImpl(
        "aggRank", "aggRankColl", acc, std::move(input), std::move(sortOrder), state);
}

SbExpr buildFinalizeRank(const Op& acc, StageBuilderState& state, const SbSlotVector& slots) {
    SbExprBuilder b(state);

    tassert(7996805, "Incorrect number of arguments", slots.size() == 1);
    return b.makeFunction("aggRankFinalize", slots[0]);
}

SbExpr::Vector buildAccumAggsDenseRank(const Op& acc,
                                       std::unique_ptr<AccumRankInputs> inputs,
                                       StageBuilderState& state) {
    auto input = std::move(inputs->inputExpr);
    auto sortOrder = std::move(inputs->isAscending);

    return buildAccumAggsRankImpl(
        "aggDenseRank", "aggDenseRankColl", acc, std::move(input), std::move(sortOrder), state);
}

SbExpr::Vector buildInitializeIntegral(const Op& acc,
                                       std::unique_ptr<InitIntegralInputs> inputs,
                                       StageBuilderState& state) {
    SbExprBuilder b(state);

    auto unitExpr = std::move(inputs->inputExpr);

    return SbExpr::makeSeq(
        b.makeFunction("aggIntegralInit", std::move(unitExpr), b.makeBoolConstant(true)));
}

SbExpr::Vector buildAccumAggsIntegral(const Op& acc,
                                      std::unique_ptr<AccumIntegralInputs> inputs,
                                      StageBuilderState& state) {
    auto input = std::move(inputs->inputExpr);
    auto sortBy = std::move(inputs->sortBy);

    SbExprBuilder b(state);

    return SbExpr::makeSeq(b.makeFunction("aggIntegralAdd", std::move(input), std::move(sortBy)));
}

SbExpr buildFinalizeIntegral(const Op& acc, StageBuilderState& state, const SbSlotVector& slots) {
    SbExprBuilder b(state);

    tassert(7996809, "Incorrect number of arguments", slots.size() == 1);
    return b.makeFunction("aggIntegralFinalize", slots[0]);
}

SbExpr::Vector buildAccumAggsDerivative(const Op& acc, StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("sum", b.makeInt64Constant(1)));
}

SbExpr buildFinalizeDerivative(const Op& acc,
                               std::unique_ptr<FinalizeDerivativeInputs> inputs,
                               StageBuilderState& state,
                               const SbSlotVector& slots) {
    SbExprBuilder b(state);

    tassert(8085504, "Expected a single slot", slots.size() == 1);

    auto unit = std::move(inputs->unit);
    auto inputFirst = std::move(inputs->inputFirst);
    auto sortByFirst = std::move(inputs->sortByFirst);
    auto inputLast = std::move(inputs->inputLast);
    auto sortByLast = std::move(inputs->sortByLast);

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

SbExpr::Vector buildInitializeLinearFill(const Op& acc, StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("newArray",
                                          b.makeNullConstant(),
                                          b.makeNullConstant(),
                                          b.makeNullConstant(),
                                          b.makeNullConstant(),
                                          b.makeNullConstant(),
                                          b.makeInt64Constant(0)));
}

InputsPtr buildAccumExprsLinearFill(const Op& acc,
                                    std::unique_ptr<AccumLinearFillInputs> inputs,
                                    StageBuilderState& state) {
    SbExprBuilder b(state);

    inputs->inputExpr = b.makeFillEmptyNull(std::move(inputs->inputExpr));
    return inputs;
}

SbExpr::Vector buildAccumAggsLinearFill(const Op& acc,
                                        std::unique_ptr<AccumLinearFillInputs> inputs,
                                        StageBuilderState& state) {
    auto input = std::move(inputs->inputExpr);
    auto sortBy = std::move(inputs->sortBy);

    SbExprBuilder b(state);

    return SbExpr::makeSeq(b.makeFunction("aggLinearFillAdd", std::move(input), std::move(sortBy)));
}

SbExpr buildFinalizeLinearFill(const Op& acc,
                               std::unique_ptr<FinalizeLinearFillInputs> inputs,
                               StageBuilderState& state,
                               const SbSlotVector& inputSlots) {
    SbExprBuilder b(state);

    tassert(7971213,
            str::stream() << "Expected one input slot for finalization of " << acc.getOpName()
                          << ", got: " << inputSlots.size(),
            inputSlots.size() == 1);
    auto inputVar = inputSlots[0];

    auto sortBy = std::move(inputs->sortBy);

    return b.makeFunction("aggLinearFillFinalize", std::move(inputVar), std::move(sortBy));
}

// Helper function for vectorizing an expression.
SbExpr vectorizeExpr(StageBuilderState& state, const PlanStageSlots& outputs, SbExpr exprIn) {
    // Call buildVectorizedExpr() and then check if it produced a vectorized expression.
    SbExpr expr = buildVectorizedExpr(state, std::move(exprIn), outputs, false);
    boost::optional<TypeSignature> typeSig = expr.getTypeSignature();
    bool isVectorized = expr && typeSig && TypeSignature::kBlockType.isSubset(*typeSig);
    // If 'expr' is a vectorized expression return it, otherwise return a null SbExpr.
    if (isVectorized) {
        return expr;
    }
    return SbExpr{};
}

boost::optional<Accum::AccumBlockExprs> buildAccumBlockExprsSingleInput(
    const Op& acc,
    std::unique_ptr<AccumSingleInput> inputsIn,
    StageBuilderState& state,
    const PlanStageSlots& outputs) {
    // Call buildAccumExprs() and cast the result to AccumSingleInput. This will uassert if the
    // result type of buildAccumExprs() is not AccumSingleInput.
    auto inputs = castInputsTo<AccumSingleInput>(acc.buildAccumExprs(state, std::move(inputsIn)));

    // Try to vectorize 'inputs->inputExpr'.
    SbExpr expr = vectorizeExpr(state, outputs, std::move(inputs->inputExpr));

    if (expr) {
        // If vectorization succeeded, allocate an internal slot and update 'inputs->inputExpr' to
        // refer to the slot. Then put 'inputs', the vectorized expression, and the internal slot
        // into an AccumBlockExprs struct and return it.
        auto internalSlot = SbSlot{state.slotId()};
        inputs->inputExpr = SbExpr{internalSlot};

        boost::optional<Accum::AccumBlockExprs> accumBlockExprs;
        accumBlockExprs.emplace();

        accumBlockExprs->inputs = std::move(inputs);
        accumBlockExprs->exprs.emplace_back(std::move(expr));
        accumBlockExprs->slots.emplace_back(internalSlot);

        return accumBlockExprs;
    }

    // If vectorization failed, return boost::none.
    return boost::none;
}

static const StringDataMap<OpInfo> accumOpInfoMap = {
    // AddToSet
    {AccumulatorAddToSet::kName,
     OpInfo{.buildAccumAggs = makeBuildFn(&buildAccumAggsAddToSet),
            .buildFinalize = makeBuildFn(&buildFinalizeCappedAccumulator),
            .buildCombineAggs = makeBuildFn(&buildCombineAggsAddToSet)}},

    // Avg
    {AccumulatorAvg::kName,
     OpInfo{.numAggs = 2,
            .buildAccumExprs = makeBuildFn(&buildAccumExprsAvg),
            .buildAccumAggs = makeBuildFn(&buildAccumAggsAvg),
            .buildFinalize = makeBuildFn(&buildFinalizeAvg),
            .buildCombineAggs = makeBuildFn(&buildCombineAggsAvg)}},

    // Bottom
    {AccumulatorBottom::getName(),
     OpInfo{.buildAccumAggs = makeBuildFn(&buildAccumAggsTopBottomN),
            .buildInit = makeBuildFn(&buildInitializeAccumN),
            .buildFinalize = makeBuildFn(&buildFinalizeTopBottom),
            .buildCombineAggs = makeBuildFn(&buildCombineAggsTopBottomN)}},

    // BottomN
    {AccumulatorBottomN::getName(),
     OpInfo{.buildAccumAggs = makeBuildFn(&buildAccumAggsTopBottomN),
            .buildInit = makeBuildFn(&buildInitializeAccumN),
            .buildFinalize = makeBuildFn(&buildFinalizeTopBottomN),
            .buildCombineAggs = makeBuildFn(&buildCombineAggsTopBottomN)}},

    // CovariancePop
    {AccumulatorCovariancePop::kName,
     OpInfo{.buildAccumAggs = makeBuildFn(&buildAccumAggsCovariance),
            .buildFinalize = makeBuildFn(&buildFinalizeCovariancePop)}},

    // CovarianceSamp
    {AccumulatorCovarianceSamp::kName,
     OpInfo{.buildAccumAggs = makeBuildFn(&buildAccumAggsCovariance),
            .buildFinalize = makeBuildFn(&buildFinalizeCovarianceSamp)}},

    // DenseRank
    {AccumulatorDenseRank::kName,
     OpInfo{.buildAccumAggs = makeBuildFn(&buildAccumAggsDenseRank),
            .buildFinalize = makeBuildFn(&buildFinalizeRank)}},

    // Derivative
    {window_function::ExpressionDerivative::kName,
     OpInfo{.buildAccumAggs = makeBuildFn(&buildAccumAggsDerivative),
            .buildFinalize = makeBuildFn(&buildFinalizeDerivative)}},

    // DocumentNumber
    {AccumulatorDocumentNumber::kName,
     OpInfo{.buildAccumAggs = makeBuildFn(&buildAccumAggsDocumentNumber)}},

    // ExpMovingAvg
    {AccumulatorExpMovingAvg::kName,
     OpInfo{.buildAccumAggs = makeBuildFn(&buildAccumAggsExpMovingAvg),
            .buildInit = makeBuildFn(&buildInitializeExpMovingAvg),
            .buildFinalize = makeBuildFn(buildFinalizeExpMovingAvg)}},

    // First
    {AccumulatorFirst::kName,
     OpInfo{.buildAccumExprs = makeBuildFn(&buildAccumExprsFirstLast),
            .buildAccumAggs = makeBuildFn(&buildAccumAggsFirst),
            .buildCombineAggs = makeBuildFn(&buildCombineAggsFirst)}},

    // FirstN
    {AccumulatorFirstN::kName,
     OpInfo{.buildAccumAggs = makeBuildFn(&buildAccumAggsFirstN),
            .buildInit = makeBuildFn(&buildInitializeAccumN),
            .buildFinalize = makeBuildFn(&buildFinalizeFirstN),
            .buildCombineAggs = makeBuildFn(&buildCombineAggsFirstN)}},

    // Integral
    {AccumulatorIntegral::kName,
     OpInfo{.buildAccumAggs = makeBuildFn(&buildAccumAggsIntegral),
            .buildInit = makeBuildFn(&buildInitializeIntegral),
            .buildFinalize = makeBuildFn(&buildFinalizeIntegral)}},

    // Last
    {AccumulatorLast::kName,
     OpInfo{.buildAccumExprs = makeBuildFn(&buildAccumExprsFirstLast),
            .buildAccumAggs = makeBuildFn(&buildAccumAggsLast),
            .buildCombineAggs = makeBuildFn(&buildCombineAggsLast)}},

    // LastN
    {AccumulatorLastN::kName,
     OpInfo{.buildAccumExprs = makeBuildFn(&buildAccumExprsLastN),
            .buildAccumAggs = makeBuildFn(&buildAccumAggsLastN),
            .buildInit = makeBuildFn(&buildInitializeAccumN),
            .buildFinalize = makeBuildFn(&buildFinalizeLastN),
            .buildCombineAggs = makeBuildFn(&buildCombineAggsLastN)}},

    // LinearFill
    {window_function::ExpressionLinearFill::kName,
     OpInfo{.buildAccumExprs = makeBuildFn(&buildAccumExprsLinearFill),
            .buildAccumAggs = makeBuildFn(&buildAccumAggsLinearFill),
            .buildInit = makeBuildFn(&buildInitializeLinearFill),
            .buildFinalize = makeBuildFn(&buildFinalizeLinearFill)}},

    // Locf
    {AccumulatorLocf::kName, OpInfo{.buildAccumAggs = makeBuildFn(&buildAccumAggsLocf)}},

    // Max
    {AccumulatorMax::kName,
     OpInfo{.buildAccumExprs = makeBuildFn(&buildAccumExprsMinMax),
            .buildAccumBlockExprs = makeBuildFn(&buildAccumBlockExprsSingleInput),
            .buildAccumAggs = makeBuildFn(&buildAccumAggsMax),
            .buildAccumBlockAggs = makeBuildFn(&buildAccumBlockAggsMax),
            .buildFinalize = makeBuildFn(&buildFinalizeMax),
            .buildCombineAggs = makeBuildFn(&buildCombineAggsMax)}},

    // MaxN
    {AccumulatorMaxN::kName,
     OpInfo{.buildAccumExprs = makeBuildFn(&buildAccumExprsMinMaxN),
            .buildAccumAggs = makeBuildFn(&buildAccumAggsMinMaxN),
            .buildInit = makeBuildFn(&buildInitializeAccumN),
            .buildFinalize = makeBuildFn(&buildFinalizeMinMaxN),
            .buildCombineAggs = makeBuildFn(&buildCombineAggsMinMaxN)}},

    // MergeObjects
    {AccumulatorMergeObjects::kName,
     OpInfo{.buildAccumExprs = makeBuildFn(&buildAccumExprsMergeObjects),
            .buildAccumAggs = makeBuildFn(&buildAccumAggsMergeObjects),
            .buildCombineAggs = makeBuildFn(&buildCombineAggsMergeObjects)}},

    // Min
    {AccumulatorMin::kName,
     OpInfo{.buildAccumExprs = makeBuildFn(&buildAccumExprsMinMax),
            .buildAccumBlockExprs = makeBuildFn(&buildAccumBlockExprsSingleInput),
            .buildAccumAggs = makeBuildFn(&buildAccumAggsMin),
            .buildAccumBlockAggs = makeBuildFn(&buildAccumBlockAggsMin),
            .buildFinalize = makeBuildFn(&buildFinalizeMin),
            .buildCombineAggs = makeBuildFn(&buildCombineAggsMin)}},

    // MinN
    {AccumulatorMinN::kName,
     OpInfo{.buildAccumExprs = makeBuildFn(&buildAccumExprsMinMaxN),
            .buildAccumAggs = makeBuildFn(&buildAccumAggsMinMaxN),
            .buildInit = makeBuildFn(&buildInitializeAccumN),
            .buildFinalize = makeBuildFn(&buildFinalizeMinMaxN),
            .buildCombineAggs = makeBuildFn(&buildCombineAggsMinMaxN)}},

    // Push
    {AccumulatorPush::kName,
     OpInfo{.buildAccumAggs = makeBuildFn(&buildAccumAggsPush),
            .buildFinalize = makeBuildFn(&buildFinalizeCappedAccumulator),
            .buildCombineAggs = makeBuildFn(&buildCombineAggsPush)}},

    // Rank
    {AccumulatorRank::kName,
     OpInfo{.buildAccumAggs = makeBuildFn(&buildAccumAggsRank),
            .buildFinalize = makeBuildFn(&buildFinalizeRank)}},

    // Sum
    {AccumulatorSum::kName,
     OpInfo{.buildAccumAggs = makeBuildFn(&buildAccumAggsSum),
            .buildFinalize = makeBuildFn(&buildFinalizeSum),
            .buildCombineAggs = makeBuildFn(&buildCombineAggsSum)}},

    // StdDevPop
    {AccumulatorStdDevPop::kName,
     OpInfo{.buildAccumAggs = makeBuildFn(&buildAccumAggsStdDev),
            .buildFinalize = makeBuildFn(&buildFinalizeStdDevPop),
            .buildCombineAggs = makeBuildFn(&buildCombineAggsStdDev)}},

    // StdDevSamp
    {AccumulatorStdDevSamp::kName,
     OpInfo{.buildAccumAggs = makeBuildFn(&buildAccumAggsStdDev),
            .buildFinalize = makeBuildFn(&buildFinalizeStdDevSamp),
            .buildCombineAggs = makeBuildFn(&buildCombineAggsStdDev)}},

    // Top
    {AccumulatorTop::getName(),
     OpInfo{.buildAccumAggs = makeBuildFn(&buildAccumAggsTopBottomN),
            .buildInit = makeBuildFn(&buildInitializeAccumN),
            .buildFinalize = makeBuildFn(&buildFinalizeTopBottom),
            .buildCombineAggs = makeBuildFn(&buildCombineAggsTopBottomN)}},

    // TopN
    {AccumulatorTopN::getName(),
     OpInfo{.buildAccumAggs = makeBuildFn(&buildAccumAggsTopBottomN),
            .buildInit = makeBuildFn(&buildInitializeAccumN),
            .buildFinalize = makeBuildFn(&buildFinalizeTopBottomN),
            .buildCombineAggs = makeBuildFn(&buildCombineAggsTopBottomN)}},
};
}  // namespace

Op::Op(std::string opName) : _opName(std::move(opName)), _opInfo(lookupOpInfo(_opName)) {}

Op::Op(const AccumulationStatement& accStmt)
    : _opName(accStmt.expr.name), _opInfo(lookupOpInfo(_opName)) {
    if (_opName == AccumulatorSum::kName) {
        auto constArg = dynamic_cast<ExpressionConstant*>(accStmt.expr.argument.get());

        if (constArg) {
            mongo::Value value = constArg->getValue();

            switch (value.getType()) {
                case BSONType::NumberInt:
                case BSONType::NumberLong:
                case BSONType::NumberDouble:
                    _countAddendIsIntegerOrDouble = true;
                    break;
                default:
                    // 'value' is NumberDecimal type in which case, 'sum' function may not be
                    // efficient due to decimal data copying which involves memory allocation.
                    // To avoid such inefficiency, does not support NumberDecimal type for this
                    // optimization.
                    break;
            }
        }
    }
}

const OpInfo* Op::lookupOpInfo(const std::string& opName) {
    auto it = accumOpInfoMap.find(opName);
    return it != accumOpInfoMap.end() ? &it->second : nullptr;
}

size_t Op::getNumAggs() const {
    return getOpInfo()->numAggs;
}

bool Op::hasBuildAccumBlockExprs() const {
    return getOpInfo()->buildAccumBlockExprs != nullptr;
}

bool Op::hasBuildAccumBlockAggs() const {
    return getOpInfo()->buildAccumBlockAggs != nullptr;
}

InputsPtr Op::buildAccumExprs(StageBuilderState& state, InputsPtr inputs) const {
    uassert(8679702,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << _opName,
            _opInfo != nullptr);

    if (!_opInfo->buildAccumExprs) {
        // If the "buildAccumExprs" callback wasn't defined for this op, then we will use the
        // inputs as-is for the accumulator args.
        return inputs;
    }

    return _opInfo->buildAccumExprs(*this, std::move(inputs), state);
}

boost::optional<Accum::AccumBlockExprs> Op::buildAccumBlockExprs(
    StageBuilderState& state, InputsPtr inputs, const PlanStageSlots& outputs) const {
    uassert(8751303,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << _opName,
            _opInfo != nullptr);

    if (!_opInfo->buildAccumBlockAggs) {
        // If this accumulator doesn't support generated block aggs, then return boost::none.
        return boost::none;
    }

    return _opInfo->buildAccumBlockExprs(*this, std::move(inputs), state, outputs);
}

SbExpr::Vector Op::buildAccumAggs(StageBuilderState& state, InputsPtr inputs) const {
    uassert(5754701,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << _opName,
            _opInfo && _opInfo->buildAccumAggs);

    return _opInfo->buildAccumAggs(*this, std::move(inputs), state);
}

boost::optional<std::vector<BlockAggAndRowAgg>> Op::buildAccumBlockAggs(
    StageBuilderState& state,
    InputsPtr inputs,
    SbSlot bitmapInternalSlot,
    SbSlot accInternalSlot) const {
    uassert(8751304,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << _opName,
            _opInfo != nullptr);

    // If this accumulator doesn't support generated block aggs, then return boost::none.
    if (!_opInfo->buildAccumBlockAggs) {
        return boost::none;
    }

    return _opInfo->buildAccumBlockAggs(
        *this, std::move(inputs), state, bitmapInternalSlot, accInternalSlot);
}

SbExpr::Vector Op::buildInitialize(StageBuilderState& state, InputsPtr inputs) const {
    uassert(8070614,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << _opName,
            _opInfo);

    if (!_opInfo->buildInit) {
        // If the 'buildInit' callback wasn't defined for this op, perform default initialization.
        SbExpr::Vector result;
        result.resize(_opInfo->numAggs);
        return result;
    }

    return _opInfo->buildInit(*this, std::move(inputs), state);
}

SbExpr Op::buildFinalize(StageBuilderState& state,
                         InputsPtr inputs,
                         const SbSlotVector& aggSlots) const {
    uassert(5807020,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << _opName,
            _opInfo);

    if (!_opInfo->buildFinalize) {
        // If the 'buildFinalize' callback wasn't defined for this op, perform default finalization.
        return SbExpr{};
    }

    return _opInfo->buildFinalize(*this, std::move(inputs), state, aggSlots);
}

SbExpr::Vector Op::buildCombineAggs(StageBuilderState& state,
                                    InputsPtr inputs,
                                    const SbSlotVector& inputSlots) const {
    uassert(7039500,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << _opName,
            _opInfo && _opInfo->buildCombineAggs);

    return _opInfo->buildCombineAggs(*this, std::move(inputs), state, inputSlots);
}
}  // namespace Accum
}  // namespace mongo::stage_builder
