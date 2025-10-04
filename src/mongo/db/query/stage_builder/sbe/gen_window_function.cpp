/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/stage_builder/sbe/gen_window_function.h"

#include "mongo/db/query/stage_builder/sbe/sbexpr_helpers.h"

namespace mongo::stage_builder {
namespace {
template <typename ReturnT, typename InputsT, typename... Args>
using BuildFnType =
    std::function<ReturnT(const WindowOp&, std::unique_ptr<InputsT>, StageBuilderState&, Args...)>;

template <typename ReturnT, typename... Args>
using BuildNoInputsFnType = std::function<ReturnT(const WindowOp&, StageBuilderState&, Args...)>;

// std::function type for buildAddAggs() with inputs and without inputs.
using BuildAddAggsNoInputsFn = BuildNoInputsFnType<SbExpr::Vector>;

template <typename T>
using BuildAddAggsFnType = BuildFnType<SbExpr::Vector, T>;

using BuildAddAggsFn = BuildAddAggsFnType<AccumInputs>;

// std::function type for buildRemoveAggs() with inputs and without inputs.
using BuildRemoveAggsNoInputsFn = BuildNoInputsFnType<SbExpr::Vector>;

template <typename T>
using BuildRemoveAggsFnType = BuildFnType<SbExpr::Vector, T>;

using BuildRemoveAggsFn = BuildRemoveAggsFnType<AccumInputs>;

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

template <typename T>
std::unique_ptr<T> castInputsTo(AccumInputsPtr inputs) {
    // Try casting 'inputs.get()' to T* and check if the cast was succeesful.
    const bool castSuccessful = inputs && dynamic_cast<T*>(inputs.get()) != nullptr;

    uassert(8859900, "Casting window function input to expected type failed", castSuccessful);

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
            const WindowOp& op, AccumInputsPtr inputs, StageBuilderState& state, Args&&... args) {
            return fn(op, castInputsTo<T>(std::move(inputs)), state, std::forward<Args>(args)...);
        };
}

template <typename ReturnT, typename... Args>
BuildFnType<ReturnT, AccumInputs, Args...> makeBuildFnImpl(
    BuildNoInputsFnType<ReturnT, Args...> fn) {
    return
        [fn = std::move(fn)](
            const WindowOp& op, AccumInputsPtr inputs, StageBuilderState& state, Args&&... args) {
            return fn(op, state, std::forward<Args>(args)...);
        };
}

template <typename FuncT>
auto makeBuildFn(FuncT fn) {
    return makeBuildFnImpl(std::function(std::move(fn)));
}
}  // namespace

/**
 * The WindowOpInfo struct contains function pointers and other useful information about an AccumOp.
 * The 'accumOpInfoMap' map (defined below) maps the name of each op to the corresponding
 * WindowOpInfo for that op.
 */
struct WindowOpInfo {
    size_t numAggs = 1;
    BuildAddAggsFn buildAddAggs = nullptr;
    BuildRemoveAggsFn buildRemoveAggs = nullptr;
    BuildInitFn buildInit = nullptr;
    BuildFinalizeFn buildFinalize = nullptr;
};

namespace {
SbExpr::Vector buildAccumOpAccumAggsForWindowFunc(const WindowOp& op,
                                                  AccumInputsPtr inputs,
                                                  StageBuilderState& state) {
    // Call buildAddExprs() on the AccumOp corresponding to 'op' to generate the accum exprs,
    // and then call buildAddAggs() to generate the accum aggs.
    auto acc = AccumOp{op.getOpName()};
    return acc.buildAddAggs(state, acc.buildAddExprs(state, std::move(inputs)));
}

SbExpr::Vector buildAccumOpInitializeForWindowFunc(const WindowOp& op,
                                                   AccumInputsPtr inputs,
                                                   StageBuilderState& state) {
    // Call buildInitialize() on the AccumOp corresponding to 'op'.
    auto acc = AccumOp{op.getOpName()};
    return acc.buildInitialize(state, std::move(inputs));
}

SbExpr::Vector buildAccumOpInitializeForWindowFunc(const WindowOp& op, StageBuilderState& state) {
    return buildAccumOpInitializeForWindowFunc(op, AccumInputsPtr{}, state);
}

SbExpr buildAccumOpFinalizeForWindowFunc(const WindowOp& op,
                                         AccumInputsPtr inputs,
                                         StageBuilderState& state,
                                         const SbSlotVector& aggSlots) {
    // Call buildFinalize() on the AccumOp.
    auto acc = AccumOp{op.getOpName()};
    return acc.buildFinalize(state, std::move(inputs), aggSlots);
}

SbExpr buildAccumOpFinalizeForWindowFunc(const WindowOp& op,
                                         StageBuilderState& state,
                                         const SbSlotVector& aggSlots) {
    return buildAccumOpFinalizeForWindowFunc(op, AccumInputsPtr{}, state, aggSlots);
}

SbExpr::Vector buildWindowAddSum(const WindowOp& op,
                                 std::unique_ptr<AddSingleInput> inputs,
                                 StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("aggRemovableSumAdd", std::move(inputs->inputExpr)));
}

SbExpr::Vector buildWindowRemoveSum(const WindowOp& op,
                                    std::unique_ptr<AddSingleInput> inputs,
                                    StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("aggRemovableSumRemove", std::move(inputs->inputExpr)));
}

SbExpr buildWindowFinalizeSum(const WindowOp& op, StageBuilderState& state, SbSlotVector slots) {
    SbExprBuilder b(state);

    SbExpr::Vector exprs;
    for (auto slot : slots) {
        exprs.push_back(slot);
    }
    return b.makeFunction("aggRemovableSumFinalize", std::move(exprs));
}

SbExpr::Vector buildWindowAddCovariance(const WindowOp& op,
                                        std::unique_ptr<AddCovarianceInputs> inputs,
                                        StageBuilderState& state) {
    return buildAccumOpAccumAggsForWindowFunc(op, std::move(inputs), state);
}

SbExpr::Vector buildWindowRemoveCovariance(const WindowOp& op,
                                           std::unique_ptr<AddCovarianceInputs> inputs,
                                           StageBuilderState& state) {
    SbExprBuilder b(state);

    auto argX = std::move(inputs->covarianceX);
    auto argY = std::move(inputs->covarianceY);

    return SbExpr::makeSeq(b.makeFunction("aggCovarianceRemove", std::move(argX), std::move(argY)));
}

SbExpr buildWindowFinalizeCovarianceSamp(const WindowOp& op,
                                         StageBuilderState& state,
                                         SbSlotVector slots) {
    return buildAccumOpFinalizeForWindowFunc(op, state, slots);
}

SbExpr buildWindowFinalizeCovariancePop(const WindowOp& op,
                                        StageBuilderState& state,
                                        SbSlotVector slots) {
    return buildAccumOpFinalizeForWindowFunc(op, state, slots);
}

SbExpr::Vector buildWindowAddPush(const WindowOp& op,
                                  std::unique_ptr<AddSingleInput> inputs,
                                  StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("aggRemovablePushAdd", std::move(inputs->inputExpr)));
}

SbExpr::Vector buildWindowRemovePush(const WindowOp& op,
                                     std::unique_ptr<AddSingleInput> inputs,
                                     StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("aggRemovablePushRemove", std::move(inputs->inputExpr)));
}

SbExpr buildWindowFinalizePush(const WindowOp& op, StageBuilderState& state, SbSlotVector slots) {
    SbExprBuilder b(state);

    SbExpr::Vector exprs;
    for (auto slot : slots) {
        exprs.push_back(slot);
    }
    return b.makeFunction("aggRemovablePushFinalize", std::move(exprs));
}

SbExpr::Vector buildWindowAddConcatArrays(const WindowOp& op,
                                          std::unique_ptr<AddSingleInput> inputs,
                                          StageBuilderState& state) {
    SbExprBuilder b(state);

    auto frameId = state.frameId();
    auto argValue = SbLocalVar{frameId, 0};
    auto expr = b.makeIf(b.makeFunction("isArray", argValue),
                         argValue,
                         b.makeFail(ErrorCodes::TypeMismatch,
                                    "Expected new value for $concatArrays to be an array"_sd));
    auto argWithTypeCheck =
        b.makeLet(frameId, SbExpr::makeSeq(std::move(inputs->inputExpr)), std::move(expr));

    const int cap = internalQueryMaxConcatArraysBytes.load();
    return SbExpr::makeSeq(b.makeFunction(
        "aggRemovableConcatArraysAdd", std::move(argWithTypeCheck), b.makeInt32Constant(cap)));
}

SbExpr::Vector buildWindowRemoveConcatArrays(const WindowOp& op,
                                             std::unique_ptr<AddSingleInput> inputs,
                                             StageBuilderState& state) {
    SbExprBuilder b(state);

    auto frameId = state.frameId();
    auto argValue = SbLocalVar{frameId, 0};
    auto expr =
        b.makeIf(b.makeFunction("isArray", argValue),
                 argValue,
                 b.makeFail(ErrorCodes::TypeMismatch,
                            "Expected value to remove for $concatArrays to be an array"_sd));
    auto argWithTypeCheck =
        b.makeLet(frameId, SbExpr::makeSeq(std::move(inputs->inputExpr)), std::move(expr));

    return SbExpr::makeSeq(
        b.makeFunction("aggRemovableConcatArraysRemove", std::move(argWithTypeCheck)));
}

SbExpr::Vector buildWindowInitializeConcatArrays(const WindowOp& op, StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("aggRemovableConcatArraysInit"));
}

SbExpr buildWindowFinalizeConcatArrays(const WindowOp& op,
                                       StageBuilderState& state,
                                       SbSlotVector slots) {
    SbExprBuilder b(state);

    SbExpr::Vector exprs;
    for (auto slot : slots) {
        exprs.push_back(slot);
    }
    return b.makeFunction("aggRemovableConcatArraysFinalize", std::move(exprs));
}

SbExpr::Vector buildWindowInitializeIntegral(const WindowOp& op,
                                             std::unique_ptr<InitIntegralInputs> inputs,
                                             StageBuilderState& state) {
    SbExprBuilder b(state);

    auto unitExpr = std::move(inputs->inputExpr);

    return SbExpr::makeSeq(
        b.makeFunction("aggIntegralInit", std::move(unitExpr), b.makeBoolConstant(false)));
}

SbExpr::Vector buildWindowAddIntegral(const WindowOp& op,
                                      std::unique_ptr<AddIntegralInputs> inputs,
                                      StageBuilderState& state) {
    return buildAccumOpAccumAggsForWindowFunc(op, std::move(inputs), state);
}

SbExpr::Vector buildWindowRemoveIntegral(const WindowOp& op,
                                         std::unique_ptr<AddIntegralInputs> inputs,
                                         StageBuilderState& state) {
    SbExprBuilder b(state);

    auto inputExpr = std::move(inputs->inputExpr);
    auto sortByExpr = std::move(inputs->sortBy);

    return SbExpr::makeSeq(
        b.makeFunction("aggIntegralRemove", std::move(inputExpr), std::move(sortByExpr)));
}

SbExpr buildWindowFinalizeIntegral(const WindowOp& op,
                                   StageBuilderState& state,
                                   SbSlotVector slots) {
    return buildAccumOpFinalizeForWindowFunc(op, state, slots);
}

SbExpr::Vector buildWindowInitializeDerivative(const WindowOp& op, StageBuilderState& state) {
    return buildAccumOpInitializeForWindowFunc(op, state);
}

SbExpr::Vector buildWindowAddDerivative(const WindowOp& op, StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("sum", b.makeInt64Constant(1)));
}

SbExpr::Vector buildWindowRemoveDerivative(const WindowOp& op, StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("sum", b.makeInt64Constant(-1)));
}

SbExpr buildWindowFinalizeDerivative(const WindowOp& op,
                                     std::unique_ptr<FinalizeDerivativeInputs> inputs,
                                     StageBuilderState& state,
                                     SbSlotVector slots) {
    return buildAccumOpFinalizeForWindowFunc(op, std::move(inputs), state, slots);
}

SbExpr::Vector buildWindowAddStdDev(const WindowOp& op,
                                    std::unique_ptr<AddSingleInput> inputs,
                                    StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("aggRemovableStdDevAdd", std::move(inputs->inputExpr)));
}

SbExpr::Vector buildWindowRemoveStdDev(const WindowOp& op,
                                       std::unique_ptr<AddSingleInput> inputs,
                                       StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(
        b.makeFunction("aggRemovableStdDevRemove", std::move(inputs->inputExpr)));
}

SbExpr buildWindowFinalizeStdDevSamp(const WindowOp& op,
                                     StageBuilderState& state,
                                     SbSlotVector slots) {
    SbExprBuilder b(state);

    tassert(8019606, "Incorrect number of arguments", slots.size() == 1);
    SbExpr::Vector exprs;
    for (auto slot : slots) {
        exprs.push_back(slot);
    }
    return b.makeFunction("aggRemovableStdDevSampFinalize", std::move(exprs));
}

SbExpr buildWindowFinalizeStdDevPop(const WindowOp& op,
                                    StageBuilderState& state,
                                    SbSlotVector slots) {
    SbExprBuilder b(state);

    tassert(8019607, "Incorrect number of arguments", slots.size() == 1);
    SbExpr::Vector exprs;
    for (auto slot : slots) {
        exprs.push_back(slot);
    }
    return b.makeFunction("aggRemovableStdDevPopFinalize", std::move(exprs));
}

SbExpr::Vector buildWindowAddAvg(const WindowOp& op,
                                 std::unique_ptr<AddSingleInput> inputs,
                                 StageBuilderState& state) {
    SbExprBuilder b(state);

    SbExpr::Vector exprs;

    exprs.push_back(b.makeFunction("aggRemovableSumAdd", inputs->inputExpr.clone()));

    // For the counter we need to skip non-numeric values ourselves.
    auto addend =
        b.makeIf(b.makeFunction("isNumber", b.makeFillEmptyNull(std::move(inputs->inputExpr))),
                 b.makeInt64Constant(1),
                 b.makeInt64Constant(0));

    auto counterExpr = b.makeFunction("sum", std::move(addend));
    exprs.push_back(std::move(counterExpr));

    return exprs;
}

SbExpr::Vector buildWindowRemoveAvg(const WindowOp& op,
                                    std::unique_ptr<AddSingleInput> inputs,
                                    StageBuilderState& state) {
    SbExprBuilder b(state);

    SbExpr::Vector exprs;
    exprs.push_back(b.makeFunction("aggRemovableSumRemove", inputs->inputExpr.clone()));

    // For the counter we need to skip non-numeric values ourselves.
    auto subtrahend =
        b.makeIf(b.makeFunction("isNumber", b.makeFillEmptyNull(std::move(inputs->inputExpr))),
                 b.makeInt64Constant(-1),
                 b.makeInt64Constant(0));
    auto counterExpr = b.makeFunction("sum", std::move(subtrahend));
    exprs.push_back(std::move(counterExpr));
    return exprs;
}

SbExpr buildWindowFinalizeAvg(const WindowOp& op, StageBuilderState& state, SbSlotVector slots) {
    SbExprBuilder b(state);

    // Slot 0 contains the accumulated sum, and slot 1 contains the count of summed items.
    tassert(7965900,
            str::stream() << "Expected two slots to finalize avg, got: " << slots.size(),
            slots.size() == 2);

    SbExpr::Vector exprs;
    for (auto slot : slots) {
        exprs.push_back(slot);
    }

    return b.makeFunction("aggRemovableAvgFinalize", std::move(exprs));
}

SbExpr::Vector buildWindowAddFirstLast(const WindowOp& op, StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("sum", b.makeInt64Constant(1)));
}

SbExpr::Vector buildWindowRemoveFirstLast(const WindowOp& op, StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("sum", b.makeInt64Constant(-1)));
}

SbExpr buildWindowFinalizeFirstLast(const WindowOp& op,
                                    std::unique_ptr<FinalizeWindowFirstLastInputs> inputs,
                                    StageBuilderState& state,
                                    SbSlotVector slots) {
    SbExprBuilder b(state);

    auto inputExpr = std::move(inputs->inputExpr);
    auto defaultVal = std::move(inputs->defaultVal);

    auto thenExpr = b.makeFillEmpty(std::move(inputExpr), defaultVal.clone());
    return b.makeIf(
        b.makeBooleanOpTree(abt::Operations::And,
                            b.makeFunction("exists", slots[0]),
                            b.makeBinaryOp(abt::Operations::Gt, slots[0], b.makeInt64Constant(0))),
        std::move(thenExpr),
        std::move(defaultVal));
}

SbExpr::Vector buildWindowInitializeFirstN(const WindowOp& op,
                                           std::unique_ptr<InitAccumNInputs> inputs,
                                           StageBuilderState& state) {
    SbExprBuilder b(state);

    auto maxSizeArg = std::move(inputs->maxSize);
    uassert(8070609, "$firstN init argument should be a constant", maxSizeArg.isConstantExpr());

    return SbExpr::makeSeq(b.makeFunction("aggRemovableFirstNInit", std::move(maxSizeArg)));
}

SbExpr::Vector buildWindowAddFirstN(const WindowOp& op,
                                    std::unique_ptr<AddSingleInput> inputs,
                                    StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(
        b.makeFunction("aggRemovableFirstNAdd", b.makeFillEmptyNull(std::move(inputs->inputExpr))));
}

SbExpr::Vector buildWindowRemoveFirstN(const WindowOp& op,
                                       std::unique_ptr<AddSingleInput> inputs,
                                       StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("aggRemovableFirstNRemove",
                                          b.makeFillEmptyNull(std::move(inputs->inputExpr))));
}

SbExpr buildWindowFinalizeFirstN(const WindowOp& op, StageBuilderState& state, SbSlotVector slots) {
    SbExprBuilder b(state);

    tassert(8070605, "Expected a single slot", slots.size() == 1);
    return b.makeFunction("aggRemovableFirstNFinalize", slots[0]);
}

SbExpr::Vector buildWindowInitializeLastN(const WindowOp& op,
                                          std::unique_ptr<InitAccumNInputs> inputs,
                                          StageBuilderState& state) {
    SbExprBuilder b(state);

    auto maxSizeArg = std::move(inputs->maxSize);
    uassert(8070610, "$lastN init argument should be a constant", maxSizeArg.isConstantExpr());

    return SbExpr::makeSeq(b.makeFunction("aggRemovableLastNInit", std::move(maxSizeArg)));
}

SbExpr::Vector buildWindowAddLastN(const WindowOp& op,
                                   std::unique_ptr<AddSingleInput> inputs,
                                   StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(
        b.makeFunction("aggRemovableLastNAdd", b.makeFillEmptyNull(std::move(inputs->inputExpr))));
}

SbExpr::Vector buildWindowRemoveLastN(const WindowOp& op,
                                      std::unique_ptr<AddSingleInput> inputs,
                                      StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("aggRemovableLastNRemove",
                                          b.makeFillEmptyNull(std::move(inputs->inputExpr))));
}

SbExpr buildWindowFinalizeLastN(const WindowOp& op, StageBuilderState& state, SbSlotVector slots) {
    SbExprBuilder b(state);

    tassert(8070606, "Expected a single slot", slots.size() == 1);
    return b.makeFunction("aggRemovableLastNFinalize", slots[0]);
}

SbExpr::Vector buildWindowInitializeSetCommon(const WindowOp& op, StageBuilderState& state) {
    SbExprBuilder b(state);
    auto collatorSlot = state.getCollatorSlot();

    SbExpr::Vector exprs;

    if (collatorSlot) {
        exprs.push_back(b.makeFunction("aggRemovableSetCommonCollInit", SbSlot{*collatorSlot}));
    } else {
        exprs.push_back(b.makeFunction("aggRemovableSetCommonInit"));
    }
    return exprs;
}

SbExpr::Vector buildWindowAddAddToSet(const WindowOp& op,
                                      std::unique_ptr<AddSingleInput> inputs,
                                      StageBuilderState& state) {
    SbExprBuilder b(state);

    const int cap = internalQueryMaxAddToSetBytes.load();
    return SbExpr::makeSeq(b.makeFunction(
        "aggRemovableAddToSetAdd", std::move(inputs->inputExpr), b.makeInt32Constant(cap)));
}

SbExpr::Vector buildWindowRemoveAddToSet(const WindowOp& op,
                                         std::unique_ptr<AddSingleInput> inputs,
                                         StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(
        b.makeFunction("aggRemovableAddToSetRemove", std::move(inputs->inputExpr)));
}

SbExpr buildWindowFinalizeSetCommon(const WindowOp& op,
                                    StageBuilderState& state,
                                    SbSlotVector slots) {
    SbExprBuilder b(state);

    SbExpr::Vector exprs;
    for (auto slot : slots) {
        exprs.push_back(slot);
    }
    return b.makeFunction("aggRemovableSetCommonFinalize", std::move(exprs));
}

SbExpr::Vector buildWindowAddSetUnion(const WindowOp& op,
                                      std::unique_ptr<AddSingleInput> inputs,
                                      StageBuilderState& state) {
    SbExprBuilder b(state);

    auto frameId = state.frameId();
    auto argValue = SbLocalVar{frameId, 0};
    auto expr = b.makeIf(
        b.makeFunction("isArray", argValue),
        argValue,
        b.makeFail(ErrorCodes::TypeMismatch, "Expected new value for $setUnion to be an array"_sd));
    auto argWithTypeCheck =
        b.makeLet(frameId, SbExpr::makeSeq(std::move(inputs->inputExpr)), std::move(expr));

    const int cap = internalQueryMaxSetUnionBytes.load();
    return SbExpr::makeSeq(b.makeFunction(
        "aggRemovableSetUnionAdd", std::move(argWithTypeCheck), b.makeInt32Constant(cap)));
}

SbExpr::Vector buildWindowRemoveSetUnion(const WindowOp& op,
                                         std::unique_ptr<AddSingleInput> inputs,
                                         StageBuilderState& state) {
    SbExprBuilder b(state);

    auto frameId = state.frameId();
    auto argValue = SbLocalVar{frameId, 0};
    auto expr = b.makeIf(b.makeFunction("isArray", argValue),
                         argValue,
                         b.makeFail(ErrorCodes::TypeMismatch,
                                    "Expected value to remove for $setUnion to be an array"_sd));
    auto argWithTypeCheck =
        b.makeLet(frameId, SbExpr::makeSeq(std::move(inputs->inputExpr)), std::move(expr));

    return SbExpr::makeSeq(
        b.makeFunction("aggRemovableSetUnionRemove", std::move(argWithTypeCheck)));
}

SbExpr::Vector buildWindowInitializeMinMax(const WindowOp& op, StageBuilderState& state) {
    SbExprBuilder b(state);
    auto collatorSlot = state.getCollatorSlot();

    SbExpr::Vector exprs;

    auto cap = internalQueryTopNAccumulatorBytes.load();

    if (collatorSlot) {
        exprs.push_back(b.makeFunction("aggRemovableMinMaxNCollInit",
                                       b.makeInt32Constant(1),
                                       b.makeInt32Constant(cap),
                                       SbSlot{*collatorSlot}));
    } else {
        exprs.push_back(b.makeFunction(
            "aggRemovableMinMaxNInit", b.makeInt32Constant(1), b.makeInt32Constant(cap)));
    }
    return exprs;
}

SbExpr::Vector buildWindowInitializeMinMaxN(const WindowOp& op,
                                            std::unique_ptr<InitAccumNInputs> inputs,
                                            StageBuilderState& state) {
    SbExprBuilder b(state);

    auto maxSizeArg = std::move(inputs->maxSize);
    uassert(8178113, "$minN/$maxN init argument should be a constant", maxSizeArg.isConstantExpr());

    SbExpr::Vector exprs;

    auto cap = internalQueryTopNAccumulatorBytes.load();

    auto collatorSlot = state.getCollatorSlot();

    if (collatorSlot) {
        exprs.push_back(b.makeFunction("aggRemovableMinMaxNCollInit",
                                       std::move(maxSizeArg),
                                       b.makeInt32Constant(cap),
                                       SbSlot{*collatorSlot}));
    } else {
        exprs.push_back(b.makeFunction(
            "aggRemovableMinMaxNInit", std::move(maxSizeArg), b.makeInt32Constant(cap)));
    }
    return exprs;
}

SbExpr::Vector buildWindowAddMinMaxN(const WindowOp& op,
                                     std::unique_ptr<AddSingleInput> inputs,
                                     StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction(
        "aggRemovableMinMaxNAdd", b.makeFunction("setToArray", std::move(inputs->inputExpr))));
}

SbExpr::Vector buildWindowRemoveMinMaxN(const WindowOp& op,
                                        std::unique_ptr<AddSingleInput> inputs,
                                        StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction(
        "aggRemovableMinMaxNRemove", b.makeFunction("setToArray", std::move(inputs->inputExpr))));
}

SbExpr buildWindowFinalizeMinN(const WindowOp& op, StageBuilderState& state, SbSlotVector slots) {
    SbExprBuilder b(state);

    tassert(8178130, "Expected a single slot", slots.size() == 1);
    return b.makeFunction("aggRemovableMinNFinalize", slots[0]);
}

SbExpr buildWindowFinalizeMaxN(const WindowOp& op, StageBuilderState& state, SbSlotVector slots) {
    SbExprBuilder b(state);

    tassert(8178131, "Expected a single slot", slots.size() == 1);
    return b.makeFunction("aggRemovableMaxNFinalize", slots[0]);
}

SbExpr buildWindowFinalizeMin(const WindowOp& op, StageBuilderState& state, SbSlotVector slots) {
    SbExprBuilder b(state);

    tassert(8124914, "Expected a single slot", slots.size() == 1);
    return b.makeFillEmptyNull(b.makeFunction("getElement",
                                              b.makeFunction("aggRemovableMinNFinalize", slots[0]),
                                              b.makeInt32Constant(0)));
}

SbExpr buildWindowFinalizeMax(const WindowOp& op, StageBuilderState& state, SbSlotVector slots) {
    SbExprBuilder b(state);

    tassert(8124915, "Expected a single slot", slots.size() == 1);
    return b.makeFillEmptyNull(b.makeFunction("getElement",
                                              b.makeFunction("aggRemovableMaxNFinalize", slots[0]),
                                              b.makeInt32Constant(0)));
}

SbExpr::Vector buildWindowInitializeTopBottomN(StageBuilderState& state,
                                               std::string func,
                                               std::unique_ptr<InitAccumNInputs> inputs) {
    SbExprBuilder b(state);

    auto maxAccumulatorBytes = internalQueryTopNAccumulatorBytes.load();

    auto nExpr = std::move(inputs->maxSize);
    uassert(8155720, "$topN/$bottomN init argument should be a constant", nExpr.isConstantExpr());

    return SbExpr::makeSeq(
        b.makeFunction(func, std::move(nExpr), b.makeInt32Constant(maxAccumulatorBytes)));
}

SbExpr::Vector buildWindowInitializeTopN(const WindowOp& op,
                                         std::unique_ptr<InitAccumNInputs> inputs,
                                         StageBuilderState& state) {
    return buildWindowInitializeTopBottomN(state, "aggRemovableTopNInit", std::move(inputs));
}

SbExpr::Vector buildRemovableTopBottomN(StageBuilderState& state,
                                        std::string func,
                                        std::unique_ptr<AddTopBottomNInputs> inputs) {
    SbExprBuilder b(state);

    auto key = std::move(inputs->sortBy);
    auto value = std::move(inputs->value);

    return SbExpr::makeSeq(b.makeFunction(func, std::move(key), std::move(value)));
}

SbExpr::Vector buildWindowAddTopN(const WindowOp& op,
                                  std::unique_ptr<AddTopBottomNInputs> inputs,
                                  StageBuilderState& state) {
    return buildRemovableTopBottomN(state, "aggRemovableTopNAdd", std::move(inputs));
}

SbExpr::Vector buildWindowRemoveTopN(const WindowOp& op,
                                     std::unique_ptr<AddTopBottomNInputs> inputs,
                                     StageBuilderState& state) {
    return buildRemovableTopBottomN(state, "aggRemovableTopNRemove", std::move(inputs));
}

SbExpr buildWindowFinalizeTopN(const WindowOp& op, StageBuilderState& state, SbSlotVector slots) {
    SbExprBuilder b(state);

    tassert(8155710, "Expected a single slot", slots.size() == 1);
    return b.makeFunction("aggRemovableTopNFinalize", slots[0]);
}

SbExpr buildWindowFinalizeTop(const WindowOp& op, StageBuilderState& state, SbSlotVector slots) {
    SbExprBuilder b(state);

    tassert(8155721, "Expected a single slot", slots.size() == 1);
    return b.makeFillEmptyNull(b.makeFunction("getElement",
                                              b.makeFunction("aggRemovableTopNFinalize", slots[0]),
                                              b.makeInt32Constant(0)));
}

SbExpr::Vector buildWindowInitializeBottomN(const WindowOp& op,
                                            std::unique_ptr<InitAccumNInputs> inputs,
                                            StageBuilderState& state) {
    return buildWindowInitializeTopBottomN(state, "aggRemovableBottomNInit", std::move(inputs));
}

SbExpr::Vector buildWindowAddBottomN(const WindowOp& op,
                                     std::unique_ptr<AddTopBottomNInputs> inputs,
                                     StageBuilderState& state) {
    return buildRemovableTopBottomN(state, "aggRemovableBottomNAdd", std::move(inputs));
}

SbExpr::Vector buildWindowRemoveBottomN(const WindowOp& op,
                                        std::unique_ptr<AddTopBottomNInputs> inputs,
                                        StageBuilderState& state) {
    return buildRemovableTopBottomN(state, "aggRemovableBottomNRemove", std::move(inputs));
}

SbExpr buildWindowFinalizeBottomN(const WindowOp& op,
                                  StageBuilderState& state,
                                  SbSlotVector slots) {
    SbExprBuilder b(state);

    tassert(8155714, "Expected a single slot", slots.size() == 1);
    return b.makeFunction("aggRemovableBottomNFinalize", slots[0]);
}

SbExpr buildWindowFinalizeBottom(const WindowOp& op, StageBuilderState& state, SbSlotVector slots) {
    SbExprBuilder b(state);

    tassert(8155722, "Expected a single slot", slots.size() == 1);
    return b.makeFillEmptyNull(
        b.makeFunction("getElement",
                       b.makeFunction("aggRemovableBottomNFinalize", slots[0]),
                       b.makeInt32Constant(0)));
}

static const StringDataMap<WindowOpInfo> windowOpInfoMap = {
    // AddToSet
    {AccumulatorAddToSet::kName,
     WindowOpInfo{.buildAddAggs = makeBuildFn(&buildWindowAddAddToSet),
                  .buildRemoveAggs = makeBuildFn(&buildWindowRemoveAddToSet),
                  .buildInit = makeBuildFn(&buildWindowInitializeSetCommon),
                  .buildFinalize = makeBuildFn(&buildWindowFinalizeSetCommon)}},

    // Avg
    {AccumulatorAvg::kName,
     WindowOpInfo{.numAggs = 2,
                  .buildAddAggs = makeBuildFn(&buildWindowAddAvg),
                  .buildRemoveAggs = makeBuildFn(&buildWindowRemoveAvg),
                  .buildFinalize = makeBuildFn(&buildWindowFinalizeAvg)}},

    // Bottom
    {"$bottom",
     WindowOpInfo{.buildAddAggs = makeBuildFn(&buildWindowAddBottomN),
                  .buildRemoveAggs = makeBuildFn(&buildWindowRemoveBottomN),
                  .buildInit = makeBuildFn(&buildWindowInitializeBottomN),
                  .buildFinalize = makeBuildFn(&buildWindowFinalizeBottom)}},

    // BottomN
    {"$bottomN",
     WindowOpInfo{.buildAddAggs = makeBuildFn(&buildWindowAddBottomN),
                  .buildRemoveAggs = makeBuildFn(&buildWindowRemoveBottomN),
                  .buildInit = makeBuildFn(&buildWindowInitializeBottomN),
                  .buildFinalize = makeBuildFn(&buildWindowFinalizeBottomN)}},

    // ConcatArrays
    {"$concatArrays",
     WindowOpInfo{.buildAddAggs = makeBuildFn(&buildWindowAddConcatArrays),
                  .buildRemoveAggs = makeBuildFn(&buildWindowRemoveConcatArrays),
                  .buildInit = makeBuildFn(&buildWindowInitializeConcatArrays),
                  .buildFinalize = makeBuildFn(&buildWindowFinalizeConcatArrays)}},

    // CovariancePop
    {"$covariancePop",
     WindowOpInfo{.buildAddAggs = makeBuildFn(&buildWindowAddCovariance),
                  .buildRemoveAggs = makeBuildFn(&buildWindowRemoveCovariance),
                  .buildFinalize = makeBuildFn(&buildWindowFinalizeCovariancePop)}},

    // CovarianceSamp
    {"$covarianceSamp",
     WindowOpInfo{.buildAddAggs = makeBuildFn(&buildWindowAddCovariance),
                  .buildRemoveAggs = makeBuildFn(&buildWindowRemoveCovariance),
                  .buildFinalize = makeBuildFn(&buildWindowFinalizeCovarianceSamp)}},

    // Derivative
    {"$derivative",
     WindowOpInfo{.buildAddAggs = makeBuildFn(&buildWindowAddDerivative),
                  .buildRemoveAggs = makeBuildFn(&buildWindowRemoveDerivative),
                  .buildInit = makeBuildFn(&buildWindowInitializeDerivative),
                  .buildFinalize = makeBuildFn(&buildWindowFinalizeDerivative)}},

    // First
    {AccumulatorFirst::kName,
     WindowOpInfo{.buildAddAggs = makeBuildFn(&buildWindowAddFirstLast),
                  .buildRemoveAggs = makeBuildFn(&buildWindowRemoveFirstLast),
                  .buildFinalize = makeBuildFn(&buildWindowFinalizeFirstLast)}},

    // FirstN
    {"$firstN",
     WindowOpInfo{.buildAddAggs = makeBuildFn(&buildWindowAddFirstN),
                  .buildRemoveAggs = makeBuildFn(&buildWindowRemoveFirstN),
                  .buildInit = makeBuildFn(&buildWindowInitializeFirstN),
                  .buildFinalize = makeBuildFn(&buildWindowFinalizeFirstN)}},

    // Integral
    {"$integral",
     WindowOpInfo{.buildAddAggs = makeBuildFn(&buildWindowAddIntegral),
                  .buildRemoveAggs = makeBuildFn(&buildWindowRemoveIntegral),
                  .buildInit = makeBuildFn(&buildWindowInitializeIntegral),
                  .buildFinalize = makeBuildFn(&buildWindowFinalizeIntegral)}},

    // Last
    {AccumulatorLast::kName,
     WindowOpInfo{.buildAddAggs = makeBuildFn(&buildWindowAddFirstLast),
                  .buildRemoveAggs = makeBuildFn(&buildWindowRemoveFirstLast),
                  .buildFinalize = makeBuildFn(&buildWindowFinalizeFirstLast)}},

    // LastN
    {"$lastN",
     WindowOpInfo{.buildAddAggs = makeBuildFn(&buildWindowAddLastN),
                  .buildRemoveAggs = makeBuildFn(&buildWindowRemoveLastN),
                  .buildInit = makeBuildFn(&buildWindowInitializeLastN),
                  .buildFinalize = makeBuildFn(&buildWindowFinalizeLastN)}},

    // Max
    {AccumulatorMax::kName,
     WindowOpInfo{.buildAddAggs = makeBuildFn(&buildWindowAddMinMaxN),
                  .buildRemoveAggs = makeBuildFn(&buildWindowRemoveMinMaxN),
                  .buildInit = makeBuildFn(&buildWindowInitializeMinMax),
                  .buildFinalize = makeBuildFn(&buildWindowFinalizeMax)}},

    // MaxN
    {AccumulatorMaxN::kName,
     WindowOpInfo{.buildAddAggs = makeBuildFn(&buildWindowAddMinMaxN),
                  .buildRemoveAggs = makeBuildFn(&buildWindowRemoveMinMaxN),
                  .buildInit = makeBuildFn(&buildWindowInitializeMinMaxN),
                  .buildFinalize = makeBuildFn(&buildWindowFinalizeMaxN)}},

    // Min
    {AccumulatorMin::kName,
     WindowOpInfo{.buildAddAggs = makeBuildFn(&buildWindowAddMinMaxN),
                  .buildRemoveAggs = makeBuildFn(&buildWindowRemoveMinMaxN),
                  .buildInit = makeBuildFn(&buildWindowInitializeMinMax),
                  .buildFinalize = makeBuildFn(&buildWindowFinalizeMin)}},

    // MinN
    {AccumulatorMinN::kName,
     WindowOpInfo{.buildAddAggs = makeBuildFn(&buildWindowAddMinMaxN),
                  .buildRemoveAggs = makeBuildFn(&buildWindowRemoveMinMaxN),
                  .buildInit = makeBuildFn(&buildWindowInitializeMinMaxN),
                  .buildFinalize = makeBuildFn(&buildWindowFinalizeMinN)}},

    // Push
    {"$push",
     WindowOpInfo{.buildAddAggs = makeBuildFn(&buildWindowAddPush),
                  .buildRemoveAggs = makeBuildFn(&buildWindowRemovePush),
                  .buildFinalize = makeBuildFn(&buildWindowFinalizePush)}},

    // SetUnion
    {AccumulatorSetUnion::kName,
     WindowOpInfo{.buildAddAggs = makeBuildFn(&buildWindowAddSetUnion),
                  .buildRemoveAggs = makeBuildFn(&buildWindowRemoveSetUnion),
                  .buildInit = makeBuildFn(&buildWindowInitializeSetCommon),
                  .buildFinalize = makeBuildFn(&buildWindowFinalizeSetCommon)}},

    // Shift
    {"$shift",
     WindowOpInfo{.buildAddAggs = makeBuildFn(&buildWindowAddFirstLast),
                  .buildRemoveAggs = makeBuildFn(&buildWindowRemoveFirstLast),
                  .buildFinalize = makeBuildFn(&buildWindowFinalizeFirstLast)}},

    // StdDevPop
    {"$stdDevPop",
     WindowOpInfo{.buildAddAggs = makeBuildFn(&buildWindowAddStdDev),
                  .buildRemoveAggs = makeBuildFn(&buildWindowRemoveStdDev),
                  .buildFinalize = makeBuildFn(&buildWindowFinalizeStdDevPop)}},

    // StdDevSamp
    {"$stdDevSamp",
     WindowOpInfo{.buildAddAggs = makeBuildFn(&buildWindowAddStdDev),
                  .buildRemoveAggs = makeBuildFn(&buildWindowRemoveStdDev),
                  .buildFinalize = makeBuildFn(&buildWindowFinalizeStdDevSamp)}},

    // Sum
    {"$sum",
     WindowOpInfo{.buildAddAggs = makeBuildFn(&buildWindowAddSum),
                  .buildRemoveAggs = makeBuildFn(&buildWindowRemoveSum),
                  .buildFinalize = makeBuildFn(&buildWindowFinalizeSum)}},

    // Top
    {"$top",
     WindowOpInfo{.buildAddAggs = makeBuildFn(&buildWindowAddTopN),
                  .buildRemoveAggs = makeBuildFn(&buildWindowRemoveTopN),
                  .buildInit = makeBuildFn(&buildWindowInitializeTopN),
                  .buildFinalize = makeBuildFn(&buildWindowFinalizeTop)}},

    // TopN
    {"$topN",
     WindowOpInfo{.buildAddAggs = makeBuildFn(&buildWindowAddTopN),
                  .buildRemoveAggs = makeBuildFn(&buildWindowRemoveTopN),
                  .buildInit = makeBuildFn(&buildWindowInitializeTopN),
                  .buildFinalize = makeBuildFn(&buildWindowFinalizeTopN)}},
};
}  // namespace

WindowOp::WindowOp(std::string opName)
    : _opName(std::move(opName)), _opInfo(lookupOpInfo(_opName)) {}

WindowOp::WindowOp(const WindowFunctionStatement& wfStmt)
    : _opName(wfStmt.expr->getOpName()), _opInfo(lookupOpInfo(_opName)) {}

const WindowOpInfo* WindowOp::lookupOpInfo(const std::string& opName) {
    auto it = windowOpInfoMap.find(opName);
    return it != windowOpInfoMap.end() ? &it->second : nullptr;
}

size_t WindowOp::getNumAggs() const {
    return getOpInfo()->numAggs;
}

SbExpr::Vector WindowOp::buildAddAggs(StageBuilderState& state, AccumInputsPtr inputs) const {
    uassert(7914604,
            str::stream() << "Unsupported op in SBE window function builder: " << _opName,
            _opInfo && _opInfo->buildAddAggs);

    return _opInfo->buildAddAggs(*this, std::move(inputs), state);
}

SbExpr::Vector WindowOp::buildRemoveAggs(StageBuilderState& state, AccumInputsPtr inputs) const {
    uassert(7914605,
            str::stream() << "Unsupported op in SBE window function builder: " << _opName,
            _opInfo && _opInfo->buildRemoveAggs);

    return _opInfo->buildRemoveAggs(*this, std::move(inputs), state);
}

SbExpr::Vector WindowOp::buildInitialize(StageBuilderState& state, AccumInputsPtr inputs) const {
    uassert(8070615,
            str::stream() << "Unsupported op in SBE window function builder: " << _opName,
            _opInfo);

    if (!_opInfo->buildInit) {
        // If the 'buildInit' callback wasn't defined for this op, perform default initialization.
        SbExpr::Vector result;
        result.resize(_opInfo->numAggs);
        return result;
    }

    return _opInfo->buildInit(*this, std::move(inputs), state);
}

SbExpr WindowOp::buildFinalize(StageBuilderState& state,
                               AccumInputsPtr inputs,
                               const SbSlotVector& aggSlots) const {
    uassert(7914606,
            str::stream() << "Unsupported op in SBE window function builder: " << _opName,
            _opInfo);

    if (!_opInfo->buildFinalize) {
        // If the 'buildFinalize' callback wasn't defined for this op, perform default finalization.
        return SbExpr{};
    }

    return _opInfo->buildFinalize(*this, std::move(inputs), state, aggSlots);
}
}  // namespace mongo::stage_builder
