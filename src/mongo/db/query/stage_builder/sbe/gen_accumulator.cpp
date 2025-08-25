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

#include "mongo/db/query/stage_builder/sbe/gen_accumulator.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/sbe/accumulator_sum_value_enum.h"
#include "mongo/db/exec/sbe/stages/hash_agg_accumulator.h"
#include "mongo/db/exec/sbe/values/arith_common.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_for_window_functions.h"
#include "mongo/db/pipeline/accumulator_multi.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/window_function/window_function_expression.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr_helpers.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::stage_builder {
AccumInputs::~AccumInputs() = default;

AccumInputsPtr AddSingleInput::clone() const {
    return std::make_unique<AddSingleInput>(inputExpr.clone());
}

AccumInputsPtr AddAggsAvgInputs::clone() const {
    return std::make_unique<AddAggsAvgInputs>(inputExpr.clone(), count.clone());
}

AccumInputsPtr AddCovarianceInputs::clone() const {
    return std::make_unique<AddCovarianceInputs>(covarianceX.clone(), covarianceY.clone());
}

AccumInputsPtr AddRankInputs::clone() const {
    return std::make_unique<AddRankInputs>(inputExpr.clone(), isAscending.clone());
}

AccumInputsPtr AddIntegralInputs::clone() const {
    return std::make_unique<AddIntegralInputs>(inputExpr.clone(), sortBy.clone());
}

AccumInputsPtr AddLinearFillInputs::clone() const {
    return std::make_unique<AddLinearFillInputs>(inputExpr.clone(), sortBy.clone());
}

AccumInputsPtr AddTopBottomNInputs::clone() const {
    return std::make_unique<AddTopBottomNInputs>(value.clone(), sortBy.clone(), sortSpec.clone());
}

AccumInputsPtr AddBlockTopBottomNInputs::clone() const {
    SbExpr::Vector clonedValues;
    SbExpr::Vector clonedSortBy;

    for (const auto& e : values) {
        clonedValues.emplace_back(e.clone());
    }
    for (const auto& e : sortBy) {
        clonedSortBy.emplace_back(e.clone());
    }

    return std::make_unique<AddBlockTopBottomNInputs>(
        std::pair{std::move(clonedValues), valueIsArray},
        std::pair{std::move(clonedSortBy), useMK},
        sortSpec.clone());
}

AccumInputsPtr InitAccumNInputs::clone() const {
    return std::make_unique<InitAccumNInputs>(maxSize.clone(), isGroupAccum.clone());
}

AccumInputsPtr InitExpMovingAvgInputs::clone() const {
    return std::make_unique<InitExpMovingAvgInputs>(inputExpr.clone());
}

AccumInputsPtr InitIntegralInputs::clone() const {
    return std::make_unique<InitIntegralInputs>(inputExpr.clone());
}

AccumInputsPtr FinalizeTopBottomNInputs::clone() const {
    return std::make_unique<FinalizeTopBottomNInputs>(sortSpec.clone());
}

AccumInputsPtr FinalizeDerivativeInputs::clone() const {
    return std::make_unique<FinalizeDerivativeInputs>(unit.clone(),
                                                      inputFirst.clone(),
                                                      sortByFirst.clone(),
                                                      inputLast.clone(),
                                                      sortByLast.clone());
}

AccumInputsPtr FinalizeLinearFillInputs::clone() const {
    return std::make_unique<FinalizeLinearFillInputs>(sortBy.clone());
}

AccumInputsPtr FinalizeWindowFirstLastInputs::clone() const {
    return std::make_unique<FinalizeWindowFirstLastInputs>(inputExpr.clone(), defaultVal.clone());
}

AccumInputsPtr CombineAggsTopBottomNInputs::clone() const {
    return std::make_unique<CombineAggsTopBottomNInputs>(sortSpec.clone());
}

namespace {
template <typename ReturnT, typename InputsT, typename... Args>
using BuildFnType =
    std::function<ReturnT(const AccumOp&, std::unique_ptr<InputsT>, StageBuilderState&, Args...)>;

template <typename ReturnT, typename... Args>
using BuildNoInputsFnType = std::function<ReturnT(const AccumOp&, StageBuilderState&, Args...)>;

template <typename T>
using BuildSinglePurposeAccumFnType =
    BuildFnType<SbHashAggAccumulator, T, std::string, SbSlot, SbSlot>;

using BuildSinglePurposeAccumFn = BuildSinglePurposeAccumFnType<AddSingleInput>;

// std::function type for buildAddExprs() with inputs and without inputs.
using BuildAccumExprsNoInputsFn = BuildNoInputsFnType<AccumInputsPtr>;

template <typename T>
using BuildAccumExprsFnType = BuildFnType<AccumInputsPtr, T>;

using BuildAccumExprsFn = BuildAccumExprsFnType<AccumInputs>;

// std::function type for buildAddBlockExprs() with inputs and without inputs.
using BuildAccumBlockExprsNoInputsFn =
    BuildNoInputsFnType<boost::optional<AddBlockExprs>, const PlanStageSlots&>;

template <typename T>
using BuildAccumBlockExprsFnType =
    BuildFnType<boost::optional<AddBlockExprs>, T, const PlanStageSlots&>;

using BuildAccumBlockExprsFn = BuildAccumBlockExprsFnType<AccumInputs>;

// std::function type for buildAddAggs() with inputs and without inputs.
using BuildAccumAggsNoInputsFn = BuildNoInputsFnType<SbExpr::Vector>;

template <typename T>
using BuildAccumAggsFnType = BuildFnType<SbExpr::Vector, T>;

using BuildAccumAggsFn = BuildAccumAggsFnType<AccumInputs>;

// std::function type for buildAddBlockAggs() with inputs and without inputs.
using BuildAccumBlockAggsNoInputsFn =
    BuildNoInputsFnType<boost::optional<std::vector<BlockAggAndRowAgg>>, SbSlot>;

template <typename T>
using BuildAccumBlockAggsFnType =
    BuildFnType<boost::optional<std::vector<BlockAggAndRowAgg>>, T, SbSlot>;

using BuildAccumBlockAggsFn = BuildAccumBlockAggsFnType<AccumInputs>;

// std::function type for buildInit() with inputs and without inputs.
using BuildInitNoInputsFn = BuildNoInputsFnType<SbExpr::Vector>;

template <typename T>
using BuildInitFnType = BuildFnType<SbExpr::Vector, T>;

using BuildInitFn = BuildInitFnType<AccumInputs>;

// std::function type for buildFinalize() with inputs and without inputs.
using BuildFinalizeNoInputsFn = BuildNoInputsFnType<SbExpr, const SbSlotVector&>;

template <typename T>
using BuildFinalizeFnType = BuildFnType<SbExpr, T, const SbSlotVector&>;

using BuildFinalizeFn = BuildFinalizeFnType<AccumInputs>;

// std::function type for buildCombineAggs() with inputs and without inputs.
using BuildCombineAggsNoInputsFn = BuildNoInputsFnType<SbExpr::Vector, const SbSlotVector&>;

template <typename T>
using BuildCombineAggsFnType = BuildFnType<SbExpr::Vector, T, const SbSlotVector&>;

using BuildCombineAggsFn = BuildCombineAggsFnType<AccumInputs>;

// Helper function for casting AccumInputsPtr to a derived class type.
template <typename T>
std::unique_ptr<T> castInputsTo(AccumInputsPtr inputs) {
    // Try casting 'inputs.get()' to T* and check if the cast was succeesful.
    const bool castSuccessful = inputs && dynamic_cast<T*>(inputs.get()) != nullptr;

    // Assert that the cast was successful.
    uassert(8679708, "Casting accumulator input to expected type failed", castSuccessful);

    // Extract the pointer from 'inputs', wrap it in a 'std::unique_ptr<T>' and return it.
    return std::unique_ptr<T>{static_cast<T*>(inputs.release())};
}

/**
 * The makeBuildFn() helper function takes a build callback and returns a "wrapped" build
 * callback.
 *
 * Given an original build callback with an "inputs" parameter of type 'std::unique_ptr<T>'
 * (where T is a subclass of AccumInputs), the "wrapped" build callback returned will have the same
 * parameters and return type as the original, except that the type of its "inputs" parameter
 * will be 'std::unique_ptr<AccumInputs>' instead.
 *
 * When invoked, the wrapped callback will check if the "inputs" arg provided by the caller can
 * be cast to the type of the original callback's "inputs" parameter. If the cast is possible,
 * the wrapped callback will perform the cast, invoke the original callback, and return whatever
 * the original callback returns. If the cast is not possible, the wrapped callback will uassert.
 */
template <typename ReturnT, typename T, typename... Args>
BuildFnType<ReturnT, AccumInputs, Args...> makeBuildFnImpl(BuildFnType<ReturnT, T, Args...> fn) {
    return
        [fn = std::move(fn)](
            const AccumOp& acc, AccumInputsPtr inputs, StageBuilderState& state, Args&&... args) {
            return fn(acc, castInputsTo<T>(std::move(inputs)), state, std::forward<Args>(args)...);
        };
}

template <typename ReturnT, typename... Args>
BuildFnType<ReturnT, AccumInputs, Args...> makeBuildFnImpl(
    BuildNoInputsFnType<ReturnT, Args...> fn) {
    return
        [fn = std::move(fn)](
            const AccumOp& acc, AccumInputsPtr inputs, StageBuilderState& state, Args&&... args) {
            return fn(acc, state, std::forward<Args>(args)...);
        };
}

template <typename FuncT>
auto makeBuildFn(FuncT fn) {
    return makeBuildFnImpl(std::function(std::move(fn)));
}
}  // namespace

/**
 * The AccumOpInfo struct contains function pointers and other useful information about an AccumOp.
 * The 'accumOpInfoMap' map (defined below) maps the name of each op to the corresponding
 * AccumOpInfo for that op.
 */
struct AccumOpInfo {
    size_t numAggs = 1;
    BuildSinglePurposeAccumFn buildSinglePurposeAccum = nullptr;
    BuildSinglePurposeAccumFn buildSinglePurposeAccumForMerge = nullptr;
    BuildAccumExprsFn buildAddExprs = nullptr;
    BuildAccumBlockExprsFn buildAddBlockExprs = nullptr;
    BuildAccumAggsFn buildAddAggs = nullptr;
    BuildAccumBlockAggsFn buildAddBlockAggs = nullptr;
    BuildInitFn buildInit = nullptr;
    BuildFinalizeFn buildFinalize = nullptr;
    BuildCombineAggsFn buildCombineAggs = nullptr;
};

namespace {
/**
 * Wraps an SbExpr in a let-if that resolves null, missing, and undefined values all to a
 * TypeTags::Nothing constant, else retains the original value.
 */
SbExpr nullMissingUndefinedToNothing(SbExpr arg, StageBuilderState& state) {
    SbExprBuilder b(state);

    return b.makeFunction(
        "fillType",
        std::move(arg),
        b.makeInt32Constant(getBSONTypeMask(BSONType::null) | getBSONTypeMask(BSONType::undefined)),
        b.makeNothingConstant());
}

template <class Implementation>
SbHashAggAccumulator buildSinglePurposeAccum(const AccumOp& acc,
                                             std::unique_ptr<AddSingleInput> inputs,
                                             StageBuilderState& state,
                                             std::string fieldName,
                                             SbSlot outSlot,
                                             SbSlot spillSlot) {
    return SbHashAggAccumulator{.fieldName = fieldName,
                                .outSlot = outSlot,
                                .spillSlot = std::move(spillSlot),
                                .resultExpr = SbExpr{outSlot},
                                .implementation =
                                    SbHashAggSinglePurposeScalarAccumulator<Implementation>{
                                        .transform = std::move(inputs->inputExpr),
                                    }};
}

/**
 * Used to create buildAddExprs values for $min and $max, as these otherwise won't handle all the
 * null, missing, undefined cases correctly.
 */
AccumInputsPtr buildAccumExprsMinMax(const AccumOp& acc,
                                     std::unique_ptr<AddSingleInput> inputs,
                                     StageBuilderState& state) {
    inputs->inputExpr = nullMissingUndefinedToNothing(std::move(inputs->inputExpr), state);
    return inputs;
}

SbExpr::Vector buildAccumAggsMin(const AccumOp& acc,
                                 std::unique_ptr<AddSingleInput> inputs,
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

SbExpr::Vector buildCombineAggsMin(const AccumOp& acc,
                                   StageBuilderState& state,
                                   const SbSlotVector& inputSlots) {
    tassert(7039501,
            "partial agg combiner for $min should have exactly one input slot",
            inputSlots.size() == 1);

    SbExprBuilder b(state);

    SbExpr arg = nullMissingUndefinedToNothing(SbExpr{inputSlots[0]}, state);

    boost::optional<sbe::value::SlotId> collatorSlot = state.getCollatorSlot();

    if (collatorSlot) {
        return SbExpr::makeSeq(b.makeFunction("collMin"_sd, SbVar{*collatorSlot}, std::move(arg)));
    } else {
        return SbExpr::makeSeq(b.makeFunction("min"_sd, std::move(arg)));
    }
}

SbExpr buildFinalizeMin(const AccumOp& acc,
                        StageBuilderState& state,
                        const SbSlotVector& minSlots) {
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

SbExpr::Vector buildAccumAggsMax(const AccumOp& acc,
                                 std::unique_ptr<AddSingleInput> inputs,
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

SbExpr::Vector buildCombineAggsMax(const AccumOp& acc,
                                   StageBuilderState& state,
                                   const SbSlotVector& inputSlots) {
    tassert(7039502,
            "partial agg combiner for $max should have exactly one input slot",
            inputSlots.size() == 1);

    SbExprBuilder b(state);

    SbExpr arg = nullMissingUndefinedToNothing(SbExpr{inputSlots[0]}, state);

    boost::optional<sbe::value::SlotId> collatorSlot = state.getCollatorSlot();

    if (collatorSlot) {
        return SbExpr::makeSeq(b.makeFunction("collMax"_sd, SbVar{*collatorSlot}, std::move(arg)));
    } else {
        return SbExpr::makeSeq(b.makeFunction("max"_sd, std::move(arg)));
    }
}

SbExpr buildFinalizeMax(const AccumOp& acc,
                        StageBuilderState& state,
                        const SbSlotVector& maxSlots) {
    SbExprBuilder b(state);

    tassert(5755100,
            str::stream() << "Expected one input slot for finalization of max, got: "
                          << maxSlots.size(),
            maxSlots.size() == 1);
    return b.makeFillEmptyNull(maxSlots[0]);
}

AccumInputsPtr buildAccumExprsFirstLast(const AccumOp& acc,
                                        std::unique_ptr<AddSingleInput> inputs,
                                        StageBuilderState& state) {
    SbExprBuilder b(state);

    inputs->inputExpr = b.makeFillEmptyNull(std::move(inputs->inputExpr));
    return inputs;
}

SbExpr::Vector buildAccumAggsFirst(const AccumOp& acc,
                                   std::unique_ptr<AddSingleInput> inputs,
                                   StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("first", std::move(inputs->inputExpr)));
}

SbExpr::Vector buildCombineAggsFirst(const AccumOp& acc,
                                     StageBuilderState& state,
                                     const SbSlotVector& inputSlots) {
    tassert(7039503,
            "partial agg combiner for $first should have exactly one input slot",
            inputSlots.size() == 1);

    SbExprBuilder b(state);

    auto arg = b.makeFillEmptyNull(SbExpr{inputSlots[0]});
    return SbExpr::makeSeq(b.makeFunction("first", std::move(arg)));
}

SbExpr::Vector buildAccumAggsLast(const AccumOp& acc,
                                  std::unique_ptr<AddSingleInput> inputs,
                                  StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("last", std::move(inputs->inputExpr)));
}

SbExpr::Vector buildCombineAggsLast(const AccumOp& acc,
                                    StageBuilderState& state,
                                    const SbSlotVector& inputSlots) {
    tassert(7039504,
            "partial agg combiner for $last should have exactly one input slot",
            inputSlots.size() == 1);

    SbExprBuilder b(state);

    auto arg = b.makeFillEmptyNull(SbExpr{inputSlots[0]});
    return SbExpr::makeSeq(b.makeFunction("last", std::move(arg)));
}

AccumInputsPtr buildAccumExprsAvg(const AccumOp& acc,
                                  std::unique_ptr<AddSingleInput> inputs,
                                  StageBuilderState& state) {
    SbExprBuilder b(state);

    // Generate the addend expression.
    auto frameId = state.frameIdGenerator->generate();
    auto binds = SbExpr::makeSeq(inputs->inputExpr.clone());
    auto var = SbVar{frameId, 0};

    auto e = b.makeIf(b.makeBooleanOpTree(abt::Operations::Or,
                                          b.generateNullMissingOrUndefined(var),
                                          b.generateNonNumericCheck(var)),
                      b.makeInt64Constant(0),
                      b.makeInt64Constant(1));

    // For the counter we need to skip non-numeric values ourselves.
    auto addend = b.makeLet(frameId, std::move(binds), std::move(e));

    // Use 'inputs->inputExpr' as the input for the "aggDoubleDoubleSum()" agg and use 'addend'
    // as the input for the "sum()" agg.
    return std::make_unique<AddAggsAvgInputs>(std::move(inputs->inputExpr), std::move(addend));
}

boost::optional<AddBlockExprs> buildAccumBlockExprsAvg(const AccumOp& acc,
                                                       std::unique_ptr<AddSingleInput> inputsIn,
                                                       StageBuilderState& state,
                                                       const PlanStageSlots& outputs) {
    // Call buildAddExprs() and cast the result to AddAggsAvgInputs. This will uassert if the
    // result type of buildAddExprs() is not AddAggsAvgInputs.
    auto inputs = castInputsTo<AddAggsAvgInputs>(acc.buildAddExprs(state, std::move(inputsIn)));

    // Try to vectorize 'inputs->inputExpr' and 'inputs->count'.
    SbExpr inputExpr = buildVectorizedExpr(state, std::move(inputs->inputExpr), outputs, false);
    SbExpr countExpr = buildVectorizedExpr(state, std::move(inputs->count), outputs, false);

    if (inputExpr && countExpr) {
        // If vectorization succeeded, allocate a slot and update 'inputs->inputExpr' to refer to
        // the slot. Then put 'inputs', the vectorized expression, and the internal slot into an
        // AccumBlockExprs struct and return it.
        boost::optional<AddBlockExprs> addBlockExprs;
        addBlockExprs.emplace();

        SbSlot inputInternalSlot = SbSlot{state.slotId()};
        SbSlot countInternalSlot = SbSlot{state.slotId()};

        inputs->inputExpr = SbExpr{inputInternalSlot};
        inputs->count = SbExpr{countInternalSlot};

        addBlockExprs->exprs.emplace_back(std::move(inputExpr));
        addBlockExprs->exprs.emplace_back(std::move(countExpr));

        addBlockExprs->slots.emplace_back(inputInternalSlot);
        addBlockExprs->slots.emplace_back(countInternalSlot);

        addBlockExprs->inputs = std::move(inputs);

        return addBlockExprs;
    }

    // If vectorization failed, return boost::none.
    return boost::none;
}  // buildAccumBlockExprsAvg

SbExpr::Vector buildAccumAggsAvg(const AccumOp& acc,
                                 std::unique_ptr<AddAggsAvgInputs> inputs,
                                 StageBuilderState& state) {
    SbExprBuilder b(state);

    SbExpr::Vector aggs;
    aggs.push_back(b.makeFunction("aggDoubleDoubleSum", std::move(inputs->inputExpr)));
    aggs.push_back(b.makeFunction("sum", std::move(inputs->count)));

    return aggs;
}  // buildAccumAggsAvg

boost::optional<std::vector<BlockAggAndRowAgg>> buildAccumBlockAggsAvg(
    const AccumOp& acc,
    std::unique_ptr<AddAggsAvgInputs> inputs,
    StageBuilderState& state,
    SbSlot bitmapInternalSlot) {
    SbExprBuilder b(state);

    auto inputBlockAgg = b.makeFunction(
        "valueBlockAggDoubleDoubleSum"_sd, bitmapInternalSlot, inputs->inputExpr.clone());
    auto inputRowAgg = b.makeFunction("aggDoubleDoubleSum"_sd, std::move(inputs->inputExpr));

    auto countBlockAgg =
        b.makeFunction("valueBlockAggSum"_sd, bitmapInternalSlot, inputs->count.clone());
    auto countRowAgg = b.makeFunction("sum"_sd, std::move(inputs->count));

    boost::optional<std::vector<BlockAggAndRowAgg>> pairs;
    pairs.emplace();
    pairs->emplace_back(BlockAggAndRowAgg{std::move(inputBlockAgg), std::move(inputRowAgg)});
    pairs->emplace_back(BlockAggAndRowAgg{std::move(countBlockAgg), std::move(countRowAgg)});

    return pairs;
}  // buildAccumBlockAggsAvg

SbExpr::Vector buildCombineAggsAvg(const AccumOp& acc,
                                   StageBuilderState& state,
                                   const SbSlotVector& inputSlots) {
    SbExprBuilder b(state);

    tassert(7039539,
            "partial agg combiner for $avg should have exactly two input slots",
            inputSlots.size() == 2);

    return SbExpr::makeSeq(b.makeFunction("aggMergeDoubleDoubleSums", inputSlots[0]),
                           b.makeFunction("sum", inputSlots[1]));
}

SbExpr buildFinalizeAvg(const AccumOp& acc,
                        StageBuilderState& state,
                        const SbSlotVector& aggSlots) {
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
            b.makeIf(b.makeBinaryOp(abt::Operations::Eq, aggSlots[1], b.makeInt64Constant(0)),
                     b.makeNullConstant(),
                     b.makeBinaryOp(abt::Operations::Div,
                                    b.makeFunction("doubleDoubleSumFinalize", aggSlots[0]),
                                    aggSlots[1]));

        return finalizingExpression;
    }
}

SbExpr::Vector buildAccumAggsSum(const AccumOp& acc,
                                 std::unique_ptr<AddSingleInput> inputs,
                                 StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("aggDoubleDoubleSum", std::move(inputs->inputExpr)));
}

boost::optional<std::vector<BlockAggAndRowAgg>> buildAccumBlockAggsSum(
    const AccumOp& acc,
    std::unique_ptr<AddSingleInput> inputs,
    StageBuilderState& state,
    SbSlot bitmapInternalSlot) {
    SbExprBuilder b(state);

    SbExpr blockAgg = b.makeFunction(
        "valueBlockAggDoubleDoubleSum"_sd, bitmapInternalSlot, inputs->inputExpr.clone());
    SbExpr rowAgg = b.makeFunction("aggDoubleDoubleSum"_sd, std::move(inputs->inputExpr));

    boost::optional<std::vector<BlockAggAndRowAgg>> pairs;
    pairs.emplace();
    pairs->emplace_back(BlockAggAndRowAgg{std::move(blockAgg), std::move(rowAgg)});

    return pairs;
}

SbExpr::Vector buildCombineAggsSum(const AccumOp& acc,
                                   StageBuilderState& state,
                                   const SbSlotVector& inputSlots) {
    SbExprBuilder b(state);

    tassert(7039530,
            "partial agg combiner for $sum should have exactly one input slot",
            inputSlots.size() == 1);
    auto arg = inputSlots[0];

    return SbExpr::makeSeq(b.makeFunction("aggMergeDoubleDoubleSums", std::move(arg)));
}

SbExpr buildFinalizeSum(const AccumOp& acc,
                        StageBuilderState& state,
                        const SbSlotVector& sumSlots) {
    SbExprBuilder b(state);

    tassert(5755300,
            str::stream() << "Expected one input slot for finalization of sum, got: "
                          << sumSlots.size(),
            sumSlots.size() == 1);

    if (state.needsMerge) {
        // To support the sharding behavior, the mongos splits "{$group: {..$sum..}}" into two
        // separate "{$group: {..$sum..}}" stages, one at the mongos-side and the other at the
        // shard-side. This stage builder builds the shard-side plan. The shard-side $sum
        // accumulator is responsible to return the partial sum in one of the following forms:
        //   {nonDecimalTag: val, nonDecimalTotal: val, nonDecimalAddend: val}
        //     -OR-
        //   {nonDecimalTag: val, nonDecimalTotal: val, nonDecimalAddend: val, decimalTotal: val}
        return b.makeFunction("doubleDoublePartialSumFinalize", sumSlots[0]);
    } else {
        return b.makeFunction("doubleDoubleSumFinalize", sumSlots[0]);
    }
}

SbExpr::Vector buildAccumAggsCount(const AccumOp& acc, StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("count"));
}

SbExpr::Vector buildCombineAggsCount(const AccumOp& acc,
                                     StageBuilderState& state,
                                     const SbSlotVector& inputSlots) {
    SbExprBuilder b(state);

    tassert(8448800,
            "partial agg combiner for $count should have exactly one input slot",
            inputSlots.size() == 1);

    auto arg = inputSlots[0];

    return SbExpr::makeSeq(b.makeFunction("sum", std::move(arg)));
}

SbExpr buildFinalizeCount(const AccumOp& acc,
                          StageBuilderState& state,
                          const SbSlotVector& sumSlots) {
    SbExprBuilder b(state);

    tassert(8448801,
            str::stream() << "Expected one input slot for finalization of $count, got: "
                          << sumSlots.size(),
            sumSlots.size() == 1);

    // If the final result fits in a 32-bit integer then convert it to a 32-bit int, otherwise
    // leave it as-is.
    auto finalResultExpr =
        b.makeFillEmpty(b.makeNumericConvert(SbVar{sumSlots[0]}, sbe::value::TypeTags::NumberInt32),
                        SbVar{sumSlots[0]});

    if (state.needsMerge) {
        // To support the sharding behavior, the mongos splits "{$group: {..$count..}}" into two
        // separate "{$group: {..$count..}}" stages, one at the mongos-side and the other at the
        // shard-side. This stage builder builds the shard-side plan. The shard-side $count
        // accumulator is responsible to return the partial count in one of the following forms:
        //   {nonDecimalTag: val, nonDecimalTotal: val, nonDecimalAddend: val}
        //     -OR-
        //   {nonDecimalTag: val, nonDecimalTotal: val, nonDecimalAddend: val, decimalTotal: val}
        return b.makeFunction(
            "doubleDoublePartialSumFinalize",
            b.makeFunction("convertSimpleSumToDoubleDoubleSum", std::move(finalResultExpr)));
    } else {
        return finalResultExpr;
    }
}

SbExpr::Vector buildAccumAggsConcatArraysHelper(SbExpr arg,
                                                StringData funcName,
                                                StageBuilderState& state) {
    SbExprBuilder b(state);

    auto frameId = state.frameId();
    auto argValue = SbLocalVar{frameId, 0};
    auto expr = b.makeIf(b.makeFunction("isArray", argValue),
                         argValue,
                         b.makeFail(ErrorCodes::TypeMismatch,
                                    "Expected new value for $concatArrays to be an array"_sd));

    auto argWithTypeCheck = b.makeLet(frameId, SbExpr::makeSeq(std::move(arg)), std::move(expr));

    const int cap = internalQueryMaxConcatArraysBytes.load();

    return SbExpr::makeSeq(
        b.makeFunction(funcName, std::move(argWithTypeCheck), b.makeInt32Constant(cap)));
}

SbExpr::Vector buildAccumAggsConcatArrays(const AccumOp& acc,
                                          std::unique_ptr<AddSingleInput> inputs,
                                          StageBuilderState& state) {
    return buildAccumAggsConcatArraysHelper(
        std::move(inputs->inputExpr), "concatArraysCapped"_sd, state);
}

SbExpr::Vector buildCombineAggsConcatArrays(const AccumOp& acc,
                                            StageBuilderState& state,
                                            const SbSlotVector& inputSlots) {
    tassert(9447000,
            "partial agg combiner for $concatArrays should have exactly one input slot",
            inputSlots.size() == 1);
    auto arg = inputSlots[0];
    return buildAccumAggsConcatArraysHelper(std::move(arg), "aggConcatArraysCapped"_sd, state);
}

SbExpr::Vector buildAccumAggsSetUnionHelper(SbExpr arg,
                                            StringData funcName,
                                            StringData funcNameWithCollator,
                                            StageBuilderState& state) {
    SbExprBuilder b(state);

    auto frameId = state.frameId();
    auto argValue = SbLocalVar{frameId, 0};
    auto expr = b.makeIf(
        b.makeFunction("isArray", argValue),
        argValue,
        b.makeFail(ErrorCodes::TypeMismatch, "Expected new value for $setUnion to be an array"_sd));

    auto argWithTypeCheck = b.makeLet(frameId, SbExpr::makeSeq(std::move(arg)), std::move(expr));

    const int cap = internalQueryMaxSetUnionBytes.load();

    auto collatorSlot = state.getCollatorSlot();

    if (collatorSlot) {
        return SbExpr::makeSeq(b.makeFunction(funcNameWithCollator,
                                              SbVar{*collatorSlot},
                                              std::move(argWithTypeCheck),
                                              b.makeInt32Constant(cap)));
    } else {
        return SbExpr::makeSeq(
            b.makeFunction(funcName, std::move(argWithTypeCheck), b.makeInt32Constant(cap)));
    }
}

SbExpr::Vector buildAccumAggsSetUnion(const AccumOp& acc,
                                      std::unique_ptr<AddSingleInput> inputs,
                                      StageBuilderState& state) {
    return buildAccumAggsSetUnionHelper(
        std::move(inputs->inputExpr), "setUnionCapped"_sd, "collSetUnionCapped"_sd, state);
}

SbExpr::Vector buildCombineAggsSetUnion(const AccumOp& acc,
                                        StageBuilderState& state,
                                        const SbSlotVector& inputSlots) {
    tassert(9238805,
            "partial agg combiner for $setUnion should have exactly one input slot",
            inputSlots.size() == 1);
    auto arg = inputSlots[0];
    return buildAccumAggsSetUnionHelper(
        std::move(arg), "aggSetUnionCapped"_sd, "aggCollSetUnionCapped"_sd, state);
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

SbExpr::Vector buildAccumAggsAddToSet(const AccumOp& acc,
                                      std::unique_ptr<AddSingleInput> inputs,
                                      StageBuilderState& state) {
    return buildAccumAggsAddToSetHelper(
        std::move(inputs->inputExpr), "addToSetCapped"_sd, "collAddToSetCapped"_sd, state);
}

SbExpr::Vector buildCombineAggsAddToSet(const AccumOp& acc,
                                        StageBuilderState& state,
                                        const SbSlotVector& inputSlots) {
    tassert(7039506,
            "partial agg combiner for $addToSet should have exactly one input slot",
            inputSlots.size() == 1);
    auto arg = inputSlots[0];
    return buildAccumAggsAddToSetHelper(
        std::move(arg), "aggSetUnionCapped"_sd, "aggCollSetUnionCapped"_sd, state);
}

SbExpr buildFinalizeCappedAccumulator(const AccumOp& acc,
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

SbExpr::Vector buildAccumAggsPush(const AccumOp& acc,
                                  std::unique_ptr<AddSingleInput> inputs,
                                  StageBuilderState& state) {
    return buildAccumAggsPushHelper(std::move(inputs->inputExpr), "addToArrayCapped"_sd, state);
}

SbExpr::Vector buildCombineAggsPush(const AccumOp& acc,
                                    StageBuilderState& state,
                                    const SbSlotVector& inputSlots) {
    tassert(7039505,
            "partial agg combiner for $push should have exactly one input slot",
            inputSlots.size() == 1);

    auto arg = inputSlots[0];
    return buildAccumAggsPushHelper(std::move(arg), "aggConcatArraysCapped"_sd, state);
}

SbExpr::Vector buildAccumAggsStdDev(const AccumOp& acc,
                                    std::unique_ptr<AddSingleInput> inputs,
                                    StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("aggStdDev", std::move(inputs->inputExpr)));
}

SbExpr::Vector buildCombineAggsStdDev(const AccumOp& acc,
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

SbExpr buildFinalizeStdDevPop(const AccumOp& acc,
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

SbExpr buildFinalizeStdDevSamp(const AccumOp& acc,
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
        b.makeIf(b.makeBooleanOpTree(abt::Operations::Or,
                                     b.generateNullMissingOrUndefined(var),
                                     b.makeFunction("isObject", var)),
                 SbExpr{var},
                 b.makeFail(ErrorCodes::Error{5911200}, "$mergeObjects only supports objects"));

    return b.makeLet(frameId, std::move(binds), std::move(expr));
}

AccumInputsPtr buildAccumExprsMergeObjects(const AccumOp& acc,
                                           std::unique_ptr<AddSingleInput> inputs,
                                           StageBuilderState& state) {
    inputs->inputExpr = wrapMergeObjectsArg(std::move(inputs->inputExpr), state);
    return inputs;
}

SbExpr::Vector buildAccumAggsMergeObjects(const AccumOp& acc,
                                          std::unique_ptr<AddSingleInput> inputs,
                                          StageBuilderState& state) {
    SbExprBuilder b(state);

    return SbExpr::makeSeq(b.makeFunction("mergeObjects", std::move(inputs->inputExpr)));
}

SbExpr::Vector buildCombineAggsMergeObjects(const AccumOp& acc,
                                            StageBuilderState& state,
                                            const SbSlotVector& inputSlots) {
    tassert(7039507,
            "partial agg combiner for $mergeObjects should have exactly one input slot",
            inputSlots.size() == 1);

    SbExprBuilder b(state);

    return SbExpr::makeSeq(
        b.makeFunction("mergeObjects", wrapMergeObjectsArg(SbExpr{inputSlots[0]}, state)));
}

SbExpr::Vector buildInitializeAccumN(const AccumOp& acc,
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
            b.makeBooleanOpTree(abt::Operations::And,
                                b.makeFunction("exists", var),
                                b.makeBinaryOp(abt::Operations::Gt, var, b.makeInt64Constant(0))),
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

SbExpr::Vector buildAccumAggsFirstN(const AccumOp& acc,
                                    std::unique_ptr<AddSingleInput> inputs,
                                    StageBuilderState& state) {
    SbExprBuilder b(state);

    auto frameId = state.frameIdGenerator->generate();
    auto binds = SbExpr::makeSeq(b.makeFunction("aggState"));

    auto e = b.makeIf(b.makeFunction("aggFirstNNeedsMoreInput", SbLocalVar{frameId, 0}),
                      b.makeFunction("aggFirstN",
                                     SbLocalVar{frameId, 0},
                                     b.makeFillEmptyNull(std::move(inputs->inputExpr))),
                      SbLocalVar{frameId, 0});

    return SbExpr::makeSeq(b.makeLet(frameId, std::move(binds), std::move(e)));
}

SbExpr::Vector buildCombineAggsFirstN(const AccumOp& acc,
                                      StageBuilderState& state,
                                      const SbSlotVector& inputSlots) {
    SbExprBuilder b(state);

    uassert(7548608,
            str::stream() << "Expected one input slot for merging $firstN, got: "
                          << inputSlots.size(),
            inputSlots.size() == 1);

    return SbExpr::makeSeq(b.makeFunction("aggFirstNMerge", inputSlots[0]));
}

SbExpr buildFinalizeFirstN(const AccumOp& acc,
                           StageBuilderState& state,
                           const SbSlotVector& inputSlots) {
    SbExprBuilder b(state);

    uassert(7548609,
            str::stream() << "Expected one input slot for finalization of $firstN, got: "
                          << inputSlots.size(),
            inputSlots.size() == 1);
    return b.makeFunction("aggFirstNFinalize", inputSlots[0]);
}

AccumInputsPtr buildAccumExprsLastN(const AccumOp& acc,
                                    std::unique_ptr<AddSingleInput> inputs,
                                    StageBuilderState& state) {
    SbExprBuilder b(state);

    inputs->inputExpr = b.makeFillEmptyNull(std::move(inputs->inputExpr));
    return inputs;
}

SbExpr::Vector buildAccumAggsLastN(const AccumOp& acc,
                                   std::unique_ptr<AddSingleInput> inputs,
                                   StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("aggLastN", std::move(inputs->inputExpr)));
}

SbExpr::Vector buildCombineAggsLastN(const AccumOp& acc,
                                     StageBuilderState& state,
                                     const SbSlotVector& inputSlots) {
    SbExprBuilder b(state);

    uassert(7548701,
            str::stream() << "Expected one input slot for merging $lastN, got: "
                          << inputSlots.size(),
            inputSlots.size() == 1);

    return SbExpr::makeSeq(b.makeFunction("aggLastNMerge", inputSlots[0]));
}

SbExpr buildFinalizeLastN(const AccumOp& acc,
                          StageBuilderState& state,
                          const SbSlotVector& inputSlots) {
    SbExprBuilder b(state);

    uassert(7548702,
            str::stream() << "Expected one input slot for finalization of $lastN, got: "
                          << inputSlots.size(),
            inputSlots.size() == 1);
    return b.makeFunction("aggLastNFinalize", inputSlots[0]);
}

bool isAccumulatorTopN(const AccumOp& acc) {
    const auto& name = acc.getOpName();
    return name == AccumulatorTopN::getName() || name == AccumulatorTop::getName();
}

SbExpr::Vector buildAccumAggsTopBottomN(const AccumOp& acc,
                                        std::unique_ptr<AddTopBottomNInputs> inputs,
                                        StageBuilderState& state) {
    SbExprBuilder b(state);

    auto value = std::move(inputs->value);
    auto key = std::move(inputs->sortBy);
    auto sortSpec = std::move(inputs->sortSpec);

    return SbExpr::makeSeq(b.makeFunction(isAccumulatorTopN(acc) ? "aggTopN" : "aggBottomN",
                                          std::move(sortSpec),
                                          b.makeNullConstant(),
                                          std::move(key),
                                          std::move(value)));
}

SbExpr::Vector buildCombineAggsTopBottomN(const AccumOp& acc,
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

SbExpr buildFinalizeTopBottomNImpl(const AccumOp& acc,
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

SbExpr buildFinalizeTopBottomN(const AccumOp& acc,
                               std::unique_ptr<FinalizeTopBottomNInputs> inputs,
                               StageBuilderState& state,
                               const SbSlotVector& inputSlots) {
    return buildFinalizeTopBottomNImpl(acc, std::move(inputs), state, inputSlots, false);
}

SbExpr buildFinalizeTopBottom(const AccumOp& acc,
                              std::unique_ptr<FinalizeTopBottomNInputs> inputs,
                              StageBuilderState& state,
                              const SbSlotVector& inputSlots) {
    return buildFinalizeTopBottomNImpl(acc, std::move(inputs), state, inputSlots, true);
}

AccumInputsPtr buildAccumExprsMinMaxN(const AccumOp& acc,
                                      std::unique_ptr<AddSingleInput> inputs,
                                      StageBuilderState& state) {
    SbExprBuilder b(state);

    inputs->inputExpr = b.makeFunction("setToArray", std::move(inputs->inputExpr));
    return inputs;
}

SbExpr::Vector buildAccumAggsMinMaxN(const AccumOp& acc,
                                     std::unique_ptr<AddSingleInput> inputs,
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

SbExpr::Vector buildCombineAggsMinMaxN(const AccumOp& acc,
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

SbExpr buildFinalizeMinMaxN(const AccumOp& acc,
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

SbExpr::Vector buildAccumAggsCovariance(const AccumOp& acc,
                                        std::unique_ptr<AddCovarianceInputs> inputs,
                                        StageBuilderState& state) {
    auto argX = std::move(inputs->covarianceX);
    auto argY = std::move(inputs->covarianceY);

    SbExprBuilder b(state);

    return SbExpr::makeSeq(b.makeFunction("aggCovarianceAdd", std::move(argX), std::move(argY)));
}

SbExpr buildFinalizeCovarianceSamp(const AccumOp& acc,
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

SbExpr buildFinalizeCovariancePop(const AccumOp& acc,
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

SbExpr::Vector buildInitializeExpMovingAvg(const AccumOp& acc,
                                           std::unique_ptr<InitExpMovingAvgInputs> inputs,
                                           StageBuilderState& state) {
    SbExprBuilder b(state);

    auto alphaExpr = std::move(inputs->inputExpr);

    return SbExpr::makeSeq(b.makeFunction(
        "newArray", b.makeNullConstant(), std::move(alphaExpr), b.makeBoolConstant(false)));
}

SbExpr::Vector buildAccumAggsExpMovingAvg(const AccumOp& acc,
                                          std::unique_ptr<AddSingleInput> inputs,
                                          StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("aggExpMovingAvg", std::move(inputs->inputExpr)));
}

SbExpr buildFinalizeExpMovingAvg(const AccumOp& acc,
                                 StageBuilderState& state,
                                 const SbSlotVector& slots) {
    SbExprBuilder b(state);
    tassert(7996802, "Incorrect number of arguments", slots.size() == 1);
    return b.makeFunction("aggExpMovingAvgFinalize", slots[0]);
}

SbExpr::Vector buildAccumAggsLocf(const AccumOp& acc,
                                  std::unique_ptr<AddSingleInput> inputs,
                                  StageBuilderState& state) {
    SbExprBuilder b(state);

    auto frameId = state.frameIdGenerator->generate();
    auto binds = SbExpr::makeSeq(std::move(inputs->inputExpr));
    auto var = SbVar{frameId, 0};

    auto e = b.makeIf(b.generateNullMissingOrUndefined(var), b.makeFunction("aggState"), var);

    auto localBind = b.makeLet(frameId, std::move(binds), std::move(e));

    return SbExpr::makeSeq(std::move(localBind));
}

SbExpr::Vector buildAccumAggsDocumentNumber(const AccumOp& acc,
                                            std::unique_ptr<AddSingleInput> inputs,
                                            StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("sum", b.makeInt64Constant(1)));
}

SbExpr::Vector buildAccumAggsRankImpl(const StringData rankFuncName,
                                      const StringData collRankFuncName,
                                      const AccumOp& acc,
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

SbExpr::Vector buildAccumAggsRank(const AccumOp& acc,
                                  std::unique_ptr<AddRankInputs> inputs,
                                  StageBuilderState& state) {
    auto input = std::move(inputs->inputExpr);
    auto sortOrder = std::move(inputs->isAscending);

    return buildAccumAggsRankImpl(
        "aggRank", "aggRankColl", acc, std::move(input), std::move(sortOrder), state);
}

SbExpr buildFinalizeRank(const AccumOp& acc, StageBuilderState& state, const SbSlotVector& slots) {
    SbExprBuilder b(state);

    tassert(7996805, "Incorrect number of arguments", slots.size() == 1);
    return b.makeFunction("aggRankFinalize", slots[0]);
}

SbExpr::Vector buildAccumAggsDenseRank(const AccumOp& acc,
                                       std::unique_ptr<AddRankInputs> inputs,
                                       StageBuilderState& state) {
    auto input = std::move(inputs->inputExpr);
    auto sortOrder = std::move(inputs->isAscending);

    return buildAccumAggsRankImpl(
        "aggDenseRank", "aggDenseRankColl", acc, std::move(input), std::move(sortOrder), state);
}

SbExpr::Vector buildInitializeIntegral(const AccumOp& acc,
                                       std::unique_ptr<InitIntegralInputs> inputs,
                                       StageBuilderState& state) {
    SbExprBuilder b(state);

    auto unitExpr = std::move(inputs->inputExpr);

    return SbExpr::makeSeq(
        b.makeFunction("aggIntegralInit", std::move(unitExpr), b.makeBoolConstant(true)));
}

SbExpr::Vector buildAccumAggsIntegral(const AccumOp& acc,
                                      std::unique_ptr<AddIntegralInputs> inputs,
                                      StageBuilderState& state) {
    auto input = std::move(inputs->inputExpr);
    auto sortBy = std::move(inputs->sortBy);

    SbExprBuilder b(state);

    return SbExpr::makeSeq(b.makeFunction("aggIntegralAdd", std::move(input), std::move(sortBy)));
}

SbExpr buildFinalizeIntegral(const AccumOp& acc,
                             StageBuilderState& state,
                             const SbSlotVector& slots) {
    SbExprBuilder b(state);

    tassert(7996809, "Incorrect number of arguments", slots.size() == 1);
    return b.makeFunction("aggIntegralFinalize", slots[0]);
}

SbExpr::Vector buildAccumAggsDerivative(const AccumOp& acc, StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("sum", b.makeInt64Constant(1)));
}

SbExpr buildFinalizeDerivative(const AccumOp& acc,
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
        b.makeBooleanOpTree(abt::Operations::And,
                            b.makeFunction("exists", slots[0]),
                            b.makeBinaryOp(abt::Operations::Gt, slots[0], b.makeInt64Constant(0))),
        b.makeFunction("aggDerivativeFinalize",
                       std::move(unit),
                       std::move(inputFirst),
                       std::move(sortByFirst),
                       std::move(inputLast),
                       std::move(sortByLast)),
        b.makeNullConstant());
}

SbExpr::Vector buildInitializeLinearFill(const AccumOp& acc, StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("newArray",
                                          b.makeNullConstant(),
                                          b.makeNullConstant(),
                                          b.makeNullConstant(),
                                          b.makeNullConstant(),
                                          b.makeNullConstant(),
                                          b.makeInt64Constant(0)));
}

AccumInputsPtr buildAccumExprsLinearFill(const AccumOp& acc,
                                         std::unique_ptr<AddLinearFillInputs> inputs,
                                         StageBuilderState& state) {
    SbExprBuilder b(state);

    inputs->inputExpr = b.makeFillEmptyNull(std::move(inputs->inputExpr));
    return inputs;
}

SbExpr::Vector buildAccumAggsLinearFill(const AccumOp& acc,
                                        std::unique_ptr<AddLinearFillInputs> inputs,
                                        StageBuilderState& state) {
    auto input = std::move(inputs->inputExpr);
    auto sortBy = std::move(inputs->sortBy);

    SbExprBuilder b(state);

    return SbExpr::makeSeq(b.makeFunction("aggLinearFillAdd", std::move(input), std::move(sortBy)));
}

SbExpr buildFinalizeLinearFill(const AccumOp& acc,
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

boost::optional<AddBlockExprs> buildAccumBlockExprsSingleInput(
    const AccumOp& acc,
    std::unique_ptr<AddSingleInput> inputsIn,
    StageBuilderState& state,
    const PlanStageSlots& outputs) {
    // Call buildAddExprs() and cast the result to AddSingleInput. This will uassert if the
    // result type of buildAddExprs() is not AddSingleInput.
    auto inputs = castInputsTo<AddSingleInput>(acc.buildAddExprs(state, std::move(inputsIn)));

    // Try to vectorize 'inputs->inputExpr'.
    SbExpr expr = buildVectorizedExpr(state, std::move(inputs->inputExpr), outputs, false);

    if (expr) {
        // If vectorization succeeded, allocate a slot and update 'inputs->inputExpr' to refer to
        // the slot. Then put 'inputs', the vectorized expression, and the internal slot into an
        // AddBlockExprs struct and return it.
        boost::optional<AddBlockExprs> addBlockExprs;
        addBlockExprs.emplace();

        SbSlot internalSlot = SbSlot{state.slotId()};
        inputs->inputExpr = SbExpr{internalSlot};

        addBlockExprs->exprs.emplace_back(std::move(expr));
        addBlockExprs->slots.emplace_back(internalSlot);

        addBlockExprs->inputs = std::move(inputs);

        return addBlockExprs;
    }

    // If vectorization failed, return boost::none.
    return boost::none;
}

boost::optional<AddBlockExprs> buildAccumBlockExprsNoInputs(const AccumOp& acc,
                                                            StageBuilderState& state,
                                                            const PlanStageSlots& outputs) {
    // Default construct a 'AddBlockExprs' and return it.
    return boost::optional<AddBlockExprs>{boost::in_place_init};
}

boost::optional<std::vector<BlockAggAndRowAgg>> buildAccumBlockAggsMin(
    const AccumOp& acc,
    std::unique_ptr<AddSingleInput> inputs,
    StageBuilderState& state,
    SbSlot bitmapInternalSlot) {
    SbExprBuilder b(state);

    auto blockAgg =
        b.makeFunction("valueBlockAggMin"_sd, bitmapInternalSlot, inputs->inputExpr.clone());

    auto rowAgg = b.makeFunction("min"_sd, std::move(inputs->inputExpr));

    boost::optional<std::vector<BlockAggAndRowAgg>> pairs;
    pairs.emplace();
    pairs->emplace_back(BlockAggAndRowAgg{std::move(blockAgg), std::move(rowAgg)});

    return pairs;
}

boost::optional<std::vector<BlockAggAndRowAgg>> buildAccumBlockAggsMax(
    const AccumOp& acc,
    std::unique_ptr<AddSingleInput> inputs,
    StageBuilderState& state,
    SbSlot bitmapInternalSlot) {
    SbExprBuilder b(state);

    auto blockAgg =
        b.makeFunction("valueBlockAggMax"_sd, bitmapInternalSlot, inputs->inputExpr.clone());

    auto rowAgg = b.makeFunction("max"_sd, std::move(inputs->inputExpr));

    boost::optional<std::vector<BlockAggAndRowAgg>> pairs;
    pairs.emplace();
    pairs->emplace_back(BlockAggAndRowAgg{std::move(blockAgg), std::move(rowAgg)});

    return pairs;
}

boost::optional<std::vector<BlockAggAndRowAgg>> buildAccumBlockAggsCount(
    const AccumOp& acc, StageBuilderState& state, SbSlot bitmapInternalSlot) {
    SbExprBuilder b(state);

    auto blockAgg = b.makeFunction("valueBlockAggCount", bitmapInternalSlot);
    auto rowAgg = b.makeFunction("count");

    boost::optional<std::vector<BlockAggAndRowAgg>> pairs;
    pairs.emplace();
    pairs->emplace_back(BlockAggAndRowAgg{std::move(blockAgg), std::move(rowAgg)});

    return pairs;
}

boost::optional<AddBlockExprs> buildAccumBlockExprsTopBottomN(
    const AccumOp& acc,
    std::unique_ptr<AddBlockTopBottomNInputs> inputs,
    StageBuilderState& state,
    const PlanStageSlots& outputs) {
    // Try to vectorize each element of 'inputs->values'.
    SbExpr::Vector valueExprs;
    for (size_t i = 0; i < inputs->values.size(); ++i) {
        SbExpr valueExpr = buildVectorizedExpr(state, std::move(inputs->values[i]), outputs, false);
        if (!valueExpr) {
            // If vectorization failed, return boost::none.
            return boost::none;
        }

        valueExprs.emplace_back(std::move(valueExpr));
    }

    // Try to vectorize each element of 'inputs->sortBy'.
    SbExpr::Vector keyExprs;
    for (size_t i = 0; i < inputs->sortBy.size(); ++i) {
        SbExpr keyExpr = buildVectorizedExpr(state, std::move(inputs->sortBy[i]), outputs, false);
        if (!keyExpr) {
            // If vectorization failed, return boost::none.
            return boost::none;
        }

        keyExprs.emplace_back(std::move(keyExpr));
    }

    // If vectorization succeeded, allocate K+1 slots and update 'inputs->values' and
    // 'inputs->sortBy' to refer to these slots. Then put 'inputs', the vectorized
    // expressions, and the K+1 internal slots into an AddBlockExprs struct and return it.
    boost::optional<AddBlockExprs> addBlockExprs;
    addBlockExprs.emplace();

    inputs->values = SbExpr::Vector{};
    for (size_t i = 0; i < valueExprs.size(); ++i) {
        SbSlot valueInternalSlot = SbSlot{state.slotId()};
        inputs->values.emplace_back(SbExpr{valueInternalSlot});
        addBlockExprs->exprs.emplace_back(std::move(valueExprs[i]));
        addBlockExprs->slots.emplace_back(valueInternalSlot);
    }

    inputs->sortBy = SbExpr::Vector{};
    for (size_t i = 0; i < keyExprs.size(); ++i) {
        SbSlot keyInternalSlot = SbSlot{state.slotId()};
        inputs->sortBy.emplace_back(SbExpr{keyInternalSlot});
        addBlockExprs->exprs.emplace_back(std::move(keyExprs[i]));
        addBlockExprs->slots.emplace_back(keyInternalSlot);
    }

    addBlockExprs->inputs = std::move(inputs);

    return addBlockExprs;
}

boost::optional<std::vector<BlockAggAndRowAgg>> buildAccumBlockAggsTopBottomN(
    const AccumOp& acc,
    std::unique_ptr<AddBlockTopBottomNInputs> inputs,
    StageBuilderState& state,
    SbSlot bitmapInternalSlot) {
    SbExprBuilder b(state);

    boost::optional<std::vector<BlockAggAndRowAgg>> pairs;
    pairs.emplace();

    tassert(8448717,
            "Expected single sortBy when 'useMK' is false",
            inputs->useMK || inputs->sortBy.size() == 1);

    tassert(8448718,
            "Expected single value when 'valueIsArray' is false",
            inputs->valueIsArray || inputs->values.size() == 1);

    bool isTopN = isAccumulatorTopN(acc);
    auto [fnName, blockFnName] = inputs->valueIsArray
        ? std::pair(isTopN ? "aggTopNArray"_sd : "aggBottomNArray"_sd,
                    isTopN ? "valueBlockAggTopNArray"_sd : "valueBlockAggBottomNArray"_sd)
        : std::pair(isTopN ? "aggTopN"_sd : "aggBottomN"_sd,
                    isTopN ? "valueBlockAggTopN"_sd : "valueBlockAggBottomN"_sd);

    auto blockArgs = SbExpr::makeSeq(bitmapInternalSlot, inputs->sortSpec.clone());
    auto args = SbExpr::makeSeq(std::move(inputs->sortSpec));

    auto numKeysExpr =
        inputs->useMK ? b.makeInt32Constant(inputs->sortBy.size()) : b.makeNullConstant();

    blockArgs.emplace_back(numKeysExpr.clone());
    args.emplace_back(std::move(numKeysExpr));

    for (auto& keyExpr : inputs->sortBy) {
        blockArgs.emplace_back(keyExpr.clone());
        args.emplace_back(std::move(keyExpr));
    }

    if (!inputs->valueIsArray) {
        auto valueExpr = std::move(inputs->values[0]);
        blockArgs.emplace_back(valueExpr.clone());
        args.emplace_back(std::move(valueExpr));
    } else {
        for (auto& valueExpr : inputs->values) {
            blockArgs.emplace_back(valueExpr.clone());
            args.emplace_back(std::move(valueExpr));
        }
    }

    auto blockAgg = b.makeFunction(blockFnName, std::move(blockArgs));
    auto rowAgg = b.makeFunction(fnName, std::move(args));

    pairs->emplace_back(BlockAggAndRowAgg{std::move(blockAgg), std::move(rowAgg)});

    return pairs;
}
}  // namespace

// Map from accumulator name (e.g. "$avg" or "$count"), which is currently the only ID we have for
// accumulators, to functions used for stage building various parts of them.
static const StringDataMap<AccumOpInfo> accumOpInfoMap = {
    // AddToSet
    {AccumulatorAddToSet::kName,
     AccumOpInfo{.buildSinglePurposeAccum =
                     makeBuildFn(&buildSinglePurposeAccum<sbe::AddToSetHashAggAccumulator>),
                 .buildAddAggs = makeBuildFn(&buildAccumAggsAddToSet),
                 .buildFinalize = makeBuildFn(&buildFinalizeCappedAccumulator),
                 .buildCombineAggs = makeBuildFn(&buildCombineAggsAddToSet)}},

    // Avg
    {AccumulatorAvg::kName,
     AccumOpInfo{.numAggs = 2,  // $avg is the only accumulator decomposed into two or more
                 .buildSinglePurposeAccum = makeBuildFn(
                     &buildSinglePurposeAccum<sbe::ArithmeticAverageHashAggAccumulatorTerminal>),
                 .buildSinglePurposeAccumForMerge = makeBuildFn(
                     &buildSinglePurposeAccum<sbe::ArithmeticAverageHashAggAccumulatorPartial>),
                 .buildAddExprs = makeBuildFn(&buildAccumExprsAvg),
                 .buildAddBlockExprs = makeBuildFn(&buildAccumBlockExprsAvg),
                 .buildAddAggs = makeBuildFn(&buildAccumAggsAvg),
                 .buildAddBlockAggs = makeBuildFn(&buildAccumBlockAggsAvg),
                 .buildFinalize = makeBuildFn(&buildFinalizeAvg),
                 .buildCombineAggs = makeBuildFn(&buildCombineAggsAvg)}},

    // Bottom
    {AccumulatorBottom::getName(),
     AccumOpInfo{.buildAddBlockExprs = makeBuildFn(&buildAccumBlockExprsTopBottomN),
                 .buildAddAggs = makeBuildFn(&buildAccumAggsTopBottomN),
                 .buildAddBlockAggs = makeBuildFn(&buildAccumBlockAggsTopBottomN),
                 .buildInit = makeBuildFn(&buildInitializeAccumN),
                 .buildFinalize = makeBuildFn(&buildFinalizeTopBottom),
                 .buildCombineAggs = makeBuildFn(&buildCombineAggsTopBottomN)}},

    // BottomN
    {AccumulatorBottomN::getName(),
     AccumOpInfo{.buildAddBlockExprs = makeBuildFn(&buildAccumBlockExprsTopBottomN),
                 .buildAddAggs = makeBuildFn(&buildAccumAggsTopBottomN),
                 .buildAddBlockAggs = makeBuildFn(&buildAccumBlockAggsTopBottomN),
                 .buildInit = makeBuildFn(&buildInitializeAccumN),
                 .buildFinalize = makeBuildFn(&buildFinalizeTopBottomN),
                 .buildCombineAggs = makeBuildFn(&buildCombineAggsTopBottomN)}},

    // ConcatArrays
    {AccumulatorConcatArrays::kName,
     AccumOpInfo{.buildAddAggs = makeBuildFn(&buildAccumAggsConcatArrays),
                 .buildFinalize = makeBuildFn(&buildFinalizeCappedAccumulator),
                 .buildCombineAggs = makeBuildFn(&buildCombineAggsConcatArrays)}},

    // Count
    {kAccumulatorCountName,
     AccumOpInfo{.buildAddBlockExprs = makeBuildFn(&buildAccumBlockExprsNoInputs),
                 .buildAddAggs = makeBuildFn(&buildAccumAggsCount),
                 .buildAddBlockAggs = makeBuildFn(&buildAccumBlockAggsCount),
                 .buildFinalize = makeBuildFn(&buildFinalizeCount),
                 .buildCombineAggs = makeBuildFn(&buildCombineAggsCount)}},

    // CovariancePop
    {AccumulatorCovariancePop::kName,
     AccumOpInfo{.buildAddAggs = makeBuildFn(&buildAccumAggsCovariance),
                 .buildFinalize = makeBuildFn(&buildFinalizeCovariancePop)}},

    // CovarianceSamp
    {AccumulatorCovarianceSamp::kName,
     AccumOpInfo{.buildAddAggs = makeBuildFn(&buildAccumAggsCovariance),
                 .buildFinalize = makeBuildFn(&buildFinalizeCovarianceSamp)}},

    // DenseRank
    {AccumulatorDenseRank::kName,
     AccumOpInfo{.buildAddAggs = makeBuildFn(&buildAccumAggsDenseRank),
                 .buildFinalize = makeBuildFn(&buildFinalizeRank)}},

    // Derivative
    {window_function::ExpressionDerivative::kName,
     AccumOpInfo{.buildAddAggs = makeBuildFn(&buildAccumAggsDerivative),
                 .buildFinalize = makeBuildFn(&buildFinalizeDerivative)}},

    // DocumentNumber
    {AccumulatorDocumentNumber::kName,
     AccumOpInfo{.buildAddAggs = makeBuildFn(&buildAccumAggsDocumentNumber)}},

    // ExpMovingAvg
    {AccumulatorExpMovingAvg::kName,
     AccumOpInfo{.buildAddAggs = makeBuildFn(&buildAccumAggsExpMovingAvg),
                 .buildInit = makeBuildFn(&buildInitializeExpMovingAvg),
                 .buildFinalize = makeBuildFn(buildFinalizeExpMovingAvg)}},

    // First
    {AccumulatorFirst::kName,
     AccumOpInfo{.buildAddExprs = makeBuildFn(&buildAccumExprsFirstLast),
                 .buildAddAggs = makeBuildFn(&buildAccumAggsFirst),
                 .buildCombineAggs = makeBuildFn(&buildCombineAggsFirst)}},

    // FirstN
    {AccumulatorFirstN::kName,
     AccumOpInfo{.buildAddAggs = makeBuildFn(&buildAccumAggsFirstN),
                 .buildInit = makeBuildFn(&buildInitializeAccumN),
                 .buildFinalize = makeBuildFn(&buildFinalizeFirstN),
                 .buildCombineAggs = makeBuildFn(&buildCombineAggsFirstN)}},

    // Integral
    {AccumulatorIntegral::kName,
     AccumOpInfo{.buildAddAggs = makeBuildFn(&buildAccumAggsIntegral),
                 .buildInit = makeBuildFn(&buildInitializeIntegral),
                 .buildFinalize = makeBuildFn(&buildFinalizeIntegral)}},

    // Last
    {AccumulatorLast::kName,
     AccumOpInfo{.buildAddExprs = makeBuildFn(&buildAccumExprsFirstLast),
                 .buildAddAggs = makeBuildFn(&buildAccumAggsLast),
                 .buildCombineAggs = makeBuildFn(&buildCombineAggsLast)}},

    // LastN
    {AccumulatorLastN::kName,
     AccumOpInfo{.buildAddExprs = makeBuildFn(&buildAccumExprsLastN),
                 .buildAddAggs = makeBuildFn(&buildAccumAggsLastN),
                 .buildInit = makeBuildFn(&buildInitializeAccumN),
                 .buildFinalize = makeBuildFn(&buildFinalizeLastN),
                 .buildCombineAggs = makeBuildFn(&buildCombineAggsLastN)}},

    // LinearFill
    {window_function::ExpressionLinearFill::kName,
     AccumOpInfo{.buildAddExprs = makeBuildFn(&buildAccumExprsLinearFill),
                 .buildAddAggs = makeBuildFn(&buildAccumAggsLinearFill),
                 .buildInit = makeBuildFn(&buildInitializeLinearFill),
                 .buildFinalize = makeBuildFn(&buildFinalizeLinearFill)}},

    // Locf
    {AccumulatorLocf::kName, AccumOpInfo{.buildAddAggs = makeBuildFn(&buildAccumAggsLocf)}},

    // Max
    {AccumulatorMax::kName,
     AccumOpInfo{.buildAddExprs = makeBuildFn(&buildAccumExprsMinMax),
                 .buildAddBlockExprs = makeBuildFn(&buildAccumBlockExprsSingleInput),
                 .buildAddAggs = makeBuildFn(&buildAccumAggsMax),
                 .buildAddBlockAggs = makeBuildFn(&buildAccumBlockAggsMax),
                 .buildFinalize = makeBuildFn(&buildFinalizeMax),
                 .buildCombineAggs = makeBuildFn(&buildCombineAggsMax)}},

    // MaxN
    {AccumulatorMaxN::kName,
     AccumOpInfo{.buildAddExprs = makeBuildFn(&buildAccumExprsMinMaxN),
                 .buildAddAggs = makeBuildFn(&buildAccumAggsMinMaxN),
                 .buildInit = makeBuildFn(&buildInitializeAccumN),
                 .buildFinalize = makeBuildFn(&buildFinalizeMinMaxN),
                 .buildCombineAggs = makeBuildFn(&buildCombineAggsMinMaxN)}},

    // MergeObjects
    {AccumulatorMergeObjects::kName,
     AccumOpInfo{.buildAddExprs = makeBuildFn(&buildAccumExprsMergeObjects),
                 .buildAddAggs = makeBuildFn(&buildAccumAggsMergeObjects),
                 .buildCombineAggs = makeBuildFn(&buildCombineAggsMergeObjects)}},

    // Min
    {AccumulatorMin::kName,
     AccumOpInfo{.buildAddExprs = makeBuildFn(&buildAccumExprsMinMax),
                 .buildAddBlockExprs = makeBuildFn(&buildAccumBlockExprsSingleInput),
                 .buildAddAggs = makeBuildFn(&buildAccumAggsMin),
                 .buildAddBlockAggs = makeBuildFn(&buildAccumBlockAggsMin),
                 .buildFinalize = makeBuildFn(&buildFinalizeMin),
                 .buildCombineAggs = makeBuildFn(&buildCombineAggsMin)}},

    // MinN
    {AccumulatorMinN::kName,
     AccumOpInfo{.buildAddExprs = makeBuildFn(&buildAccumExprsMinMaxN),
                 .buildAddAggs = makeBuildFn(&buildAccumAggsMinMaxN),
                 .buildInit = makeBuildFn(&buildInitializeAccumN),
                 .buildFinalize = makeBuildFn(&buildFinalizeMinMaxN),
                 .buildCombineAggs = makeBuildFn(&buildCombineAggsMinMaxN)}},

    // Push
    {AccumulatorPush::kName,
     AccumOpInfo{.buildAddAggs = makeBuildFn(&buildAccumAggsPush),
                 .buildFinalize = makeBuildFn(&buildFinalizeCappedAccumulator),
                 .buildCombineAggs = makeBuildFn(&buildCombineAggsPush)}},

    // Rank
    {AccumulatorRank::kName,
     AccumOpInfo{.buildAddAggs = makeBuildFn(&buildAccumAggsRank),
                 .buildFinalize = makeBuildFn(&buildFinalizeRank)}},

    // SetUnion
    {AccumulatorSetUnion::kName,
     AccumOpInfo{.buildAddAggs = makeBuildFn(&buildAccumAggsSetUnion),
                 .buildFinalize = makeBuildFn(&buildFinalizeCappedAccumulator),
                 .buildCombineAggs = makeBuildFn(&buildCombineAggsSetUnion)}},

    // StdDevPop
    {AccumulatorStdDevPop::kName,
     AccumOpInfo{.buildAddAggs = makeBuildFn(&buildAccumAggsStdDev),
                 .buildFinalize = makeBuildFn(&buildFinalizeStdDevPop),
                 .buildCombineAggs = makeBuildFn(&buildCombineAggsStdDev)}},

    // StdDevSamp
    {AccumulatorStdDevSamp::kName,
     AccumOpInfo{.buildAddAggs = makeBuildFn(&buildAccumAggsStdDev),
                 .buildFinalize = makeBuildFn(&buildFinalizeStdDevSamp),
                 .buildCombineAggs = makeBuildFn(&buildCombineAggsStdDev)}},

    // Sum
    {AccumulatorSum::kName,
     AccumOpInfo{.buildAddBlockExprs = makeBuildFn(&buildAccumBlockExprsSingleInput),
                 .buildAddAggs = makeBuildFn(&buildAccumAggsSum),
                 .buildAddBlockAggs = makeBuildFn(&buildAccumBlockAggsSum),
                 .buildFinalize = makeBuildFn(&buildFinalizeSum),
                 .buildCombineAggs = makeBuildFn(&buildCombineAggsSum)}},

    // Top
    {AccumulatorTop::getName(),
     AccumOpInfo{.buildAddBlockExprs = makeBuildFn(&buildAccumBlockExprsTopBottomN),
                 .buildAddAggs = makeBuildFn(&buildAccumAggsTopBottomN),
                 .buildAddBlockAggs = makeBuildFn(&buildAccumBlockAggsTopBottomN),
                 .buildInit = makeBuildFn(&buildInitializeAccumN),
                 .buildFinalize = makeBuildFn(&buildFinalizeTopBottom),
                 .buildCombineAggs = makeBuildFn(&buildCombineAggsTopBottomN)}},

    // TopN
    {AccumulatorTopN::getName(),
     AccumOpInfo{.buildAddBlockExprs = makeBuildFn(&buildAccumBlockExprsTopBottomN),
                 .buildAddAggs = makeBuildFn(&buildAccumAggsTopBottomN),
                 .buildAddBlockAggs = makeBuildFn(&buildAccumBlockAggsTopBottomN),
                 .buildInit = makeBuildFn(&buildInitializeAccumN),
                 .buildFinalize = makeBuildFn(&buildFinalizeTopBottomN),
                 .buildCombineAggs = makeBuildFn(&buildCombineAggsTopBottomN)}},
};  // accumOpInfoMap

std::string AccumOp::getOpNameForAccStmt(const AccumulationStatement& accStmt) {
    std::string opName = std::string{accStmt.expr.name};

    // The parser transforms "{$count: ..}" into "{$group: {..: {$sum: NumberInt(1)}}}".
    // We pattern match for "{$sum: 1}" here to reverse the transform performed by the parser.
    if (auto constArg = dynamic_cast<ExpressionConstant*>(accStmt.expr.argument.get())) {
        mongo::Value value = constArg->getValue();
        if (opName == AccumulatorSum::kName && value.getType() == BSONType::numberInt &&
            value.coerceToInt() == 1) {
            return std::string{kAccumulatorCountName};
        }
    }

    return opName;
}

AccumOp::AccumOp(std::string opName) : _opName(std::move(opName)), _opInfo(lookupOpInfo(_opName)) {}

AccumOp::AccumOp(const AccumulationStatement& accStmt)
    : _opName(getOpNameForAccStmt(accStmt)), _opInfo(lookupOpInfo(_opName)) {}

const AccumOpInfo* AccumOp::lookupOpInfo(const std::string& opName) {
    auto it = accumOpInfoMap.find(opName);
    return it != accumOpInfoMap.end() ? &it->second : nullptr;
}

size_t AccumOp::getNumAggs() const {
    return getOpInfo()->numAggs;
}

bool AccumOp::hasBuildAddBlockExprs() const {
    return getOpInfo()->buildAddBlockExprs != nullptr;
}

bool AccumOp::hasBuildAddBlockAggs() const {
    return getOpInfo()->buildAddBlockAggs != nullptr;
}

bool AccumOp::canBuildSinglePurposeAccumulator() const {
    return bool{_opInfo->buildSinglePurposeAccum};
}

SbHashAggAccumulator AccumOp::buildSinglePurposeAccumulator(StageBuilderState& state,
                                                            SbExpr inputExpression,
                                                            std::string fieldName,
                                                            SbSlot outSlot,
                                                            SbSlot spillSlot) const {
    return _opInfo->buildSinglePurposeAccum(
        *this,
        std::make_unique<AddSingleInput>(std::move(inputExpression)),
        state,
        std::move(fieldName),
        std::move(outSlot),
        std::move(spillSlot));
}

SbHashAggAccumulator AccumOp::buildSinglePurposeAccumulatorForMerge(StageBuilderState& state,
                                                                    SbExpr inputExpression,
                                                                    std::string fieldName,
                                                                    SbSlot outSlot,
                                                                    SbSlot spillSlot) const {
    auto& builderFunction = _opInfo->buildSinglePurposeAccumForMerge
        ? _opInfo->buildSinglePurposeAccumForMerge
        : _opInfo->buildSinglePurposeAccum;
    return builderFunction(*this,
                           std::make_unique<AddSingleInput>(std::move(inputExpression)),
                           state,
                           std::move(fieldName),
                           std::move(outSlot),
                           std::move(spillSlot));
}

AccumInputsPtr AccumOp::buildAddExprs(StageBuilderState& state, AccumInputsPtr inputs) const {
    uassert(8679702,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << _opName,
            _opInfo != nullptr);

    if (!_opInfo->buildAddExprs) {
        // If the "buildAddExprs" callback wasn't defined for this op, then we will use the
        // inputs as-is for the accumulator args.
        return inputs;
    }

    return _opInfo->buildAddExprs(*this, std::move(inputs), state);
}

boost::optional<AddBlockExprs> AccumOp::buildAddBlockExprs(StageBuilderState& state,
                                                           AccumInputsPtr inputs,
                                                           const PlanStageSlots& outputs) const {
    uassert(8751303,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << _opName,
            _opInfo != nullptr);

    if (!_opInfo->buildAddBlockAggs) {
        // If this accumulator doesn't support generated block aggs, then return boost::none.
        return boost::none;
    }

    return _opInfo->buildAddBlockExprs(*this, std::move(inputs), state, outputs);
}

SbExpr::Vector AccumOp::buildAddAggs(StageBuilderState& state, AccumInputsPtr inputs) const {
    uassert(5754701,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << _opName,
            _opInfo && _opInfo->buildAddAggs);

    return _opInfo->buildAddAggs(*this, std::move(inputs), state);
}

boost::optional<std::vector<BlockAggAndRowAgg>> AccumOp::buildAddBlockAggs(
    StageBuilderState& state, AccumInputsPtr inputs, SbSlot bitmapInternalSlot) const {
    uassert(8751304,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << _opName,
            _opInfo != nullptr);

    // If this accumulator doesn't support generated block aggs, then return boost::none.
    if (!_opInfo->buildAddBlockAggs) {
        return boost::none;
    }

    return _opInfo->buildAddBlockAggs(*this, std::move(inputs), state, bitmapInternalSlot);
}

SbExpr::Vector AccumOp::buildInitialize(StageBuilderState& state, AccumInputsPtr inputs) const {
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

SbExpr AccumOp::buildFinalize(StageBuilderState& state,
                              AccumInputsPtr inputs,
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

SbExpr::Vector AccumOp::buildCombineAggs(StageBuilderState& state,
                                         AccumInputsPtr inputs,
                                         const SbSlotVector& inputSlots) const {
    uassert(7039500,
            str::stream() << "Unsupported Accumulator in SBE accumulator builder: " << _opName,
            _opInfo && _opInfo->buildCombineAggs);

    return _opInfo->buildCombineAggs(*this, std::move(inputs), state, inputSlots);
}
}  // namespace mongo::stage_builder
