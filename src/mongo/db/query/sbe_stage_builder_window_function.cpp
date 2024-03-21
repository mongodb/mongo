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

#include "mongo/db/query/sbe_stage_builder_window_function.h"

#include "mongo/db/query/sbe_stage_builder_accumulator.h"
#include "mongo/db/query/sbe_stage_builder_sbexpr_helpers.h"

namespace mongo::stage_builder {

template <int N>
SbExpr::Vector emptyInitializer(StageBuilderState& state,
                                const WindowFunctionStatement& stmt,
                                StringDataMap<SbExpr> argExpr) {
    SbExpr::Vector inits;
    inits.resize(N);
    return inits;
}

SbExpr::Vector addDocument(StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("sum", b.makeInt64Constant(1)));
}

SbExpr::Vector removeDocument(StageBuilderState& state) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("sum", b.makeInt64Constant(-1)));
}

SbExpr::Vector buildWindowAddSum(StageBuilderState& state,
                                 const WindowFunctionStatement& stmt,
                                 SbExpr arg) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("aggRemovableSumAdd", std::move(arg)));
}

SbExpr::Vector buildWindowRemoveSum(StageBuilderState& state,
                                    const WindowFunctionStatement& stmt,
                                    SbExpr arg) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("aggRemovableSumRemove", std::move(arg)));
}

SbExpr buildWindowFinalizeSum(StageBuilderState& state,
                              const WindowFunctionStatement& stmt,
                              SbSlotVector slots) {
    SbExprBuilder b(state);

    SbExpr::Vector exprs;
    for (auto slot : slots) {
        exprs.push_back(b.makeVariable(slot));
    }
    return b.makeFunction("aggRemovableSumFinalize", std::move(exprs));
}

SbExpr::Vector buildWindowAddCovariance(StageBuilderState& state,
                                        const WindowFunctionStatement& stmt,
                                        StringDataMap<SbExpr> args) {
    auto acc = Accum::Op{stmt.expr->getOpName()};
    return buildAccumulatorForWindowFunc(acc, std::move(args), state);
}

SbExpr::Vector buildWindowRemoveCovariance(StageBuilderState& state,
                                           const WindowFunctionStatement& stmt,
                                           StringDataMap<SbExpr> args) {
    SbExprBuilder b(state);

    tassert(7820811, "Incorrect number of arguments", args.size() == 2);

    auto it = args.find(Accum::kCovarianceX);
    tassert(7820812,
            str::stream() << "Window function expects '" << Accum::kCovarianceX << "' argument",
            it != args.end());
    auto argX = std::move(it->second);

    it = args.find(Accum::kCovarianceY);
    tassert(7820813,
            str::stream() << "Window function expects '" << Accum::kCovarianceY << "' argument",
            it != args.end());
    auto argY = std::move(it->second);

    return SbExpr::makeSeq(b.makeFunction("aggCovarianceRemove", std::move(argX), std::move(argY)));
}

SbExpr buildWindowFinalizeCovarianceSamp(StageBuilderState& state,
                                         const WindowFunctionStatement& stmt,
                                         SbSlotVector slots) {
    auto acc = Accum::Op{stmt.expr->getOpName()};
    return buildFinalizeForWindowFunc(acc, state, slots);
}

SbExpr buildWindowFinalizeCovariancePop(StageBuilderState& state,
                                        const WindowFunctionStatement& stmt,
                                        SbSlotVector slots) {
    auto acc = Accum::Op{stmt.expr->getOpName()};
    return buildFinalizeForWindowFunc(acc, state, slots);
}

SbExpr::Vector buildWindowAddPush(StageBuilderState& state,
                                  const WindowFunctionStatement& stmt,
                                  SbExpr arg) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("aggRemovablePushAdd", std::move(arg)));
}

SbExpr::Vector buildWindowRemovePush(StageBuilderState& state,
                                     const WindowFunctionStatement& stmt,
                                     SbExpr arg) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("aggRemovablePushRemove", std::move(arg)));
}

SbExpr buildWindowFinalizePush(StageBuilderState& state,
                               const WindowFunctionStatement& stmt,
                               SbSlotVector slots) {
    SbExprBuilder b(state);

    SbExpr::Vector exprs;
    for (auto slot : slots) {
        exprs.push_back(b.makeVariable(slot));
    }
    return b.makeFunction("aggRemovablePushFinalize", std::move(exprs));
}

SbExpr::Vector buildWindowInitializeIntegral(StageBuilderState& state,
                                             const WindowFunctionStatement& stmt,
                                             StringDataMap<SbExpr> args) {
    SbExprBuilder b(state);

    auto it = args.find(Accum::kInput);
    tassert(8751306, "Expected input argument", it != args.end());
    auto unitExpr = std::move(it->second);

    return SbExpr::makeSeq(
        b.makeFunction("aggIntegralInit", std::move(unitExpr), b.makeBoolConstant(false)));
}

SbExpr::Vector buildWindowAddIntegral(StageBuilderState& state,
                                      const WindowFunctionStatement& stmt,
                                      StringDataMap<SbExpr> args) {
    auto acc = Accum::Op{stmt.expr->getOpName()};
    return buildAccumulatorForWindowFunc(acc, std::move(args), state);
}

SbExpr::Vector buildWindowRemoveIntegral(StageBuilderState& state,
                                         const WindowFunctionStatement& stmt,
                                         StringDataMap<SbExpr> args) {
    SbExprBuilder b(state);

    tassert(7996814, "Incorrect number of arguments", args.size() == 2);

    auto it = args.find(Accum::kInput);
    tassert(7996815,
            str::stream() << "Window function expects '" << Accum::kInput << "' argument",
            it != args.end());
    auto input = std::move(it->second);

    it = args.find(Accum::kSortBy);
    tassert(7996816,
            str::stream() << "Window function expects '" << Accum::kSortBy << "' argument",
            it != args.end());
    auto sortBy = std::move(it->second);

    return SbExpr::makeSeq(
        b.makeFunction("aggIntegralRemove", std::move(input), std::move(sortBy)));
}

SbExpr buildWindowFinalizeIntegral(StageBuilderState& state,
                                   const WindowFunctionStatement& stmt,
                                   SbSlotVector slots) {
    auto acc = Accum::Op{stmt.expr->getOpName()};
    return buildFinalizeForWindowFunc(acc, state, slots);
}

SbExpr::Vector buildWindowInitializeDerivative(StageBuilderState& state,
                                               const WindowFunctionStatement& stmt,
                                               StringDataMap<SbExpr> args) {
    auto acc = Accum::Op{stmt.expr->getOpName()};
    return buildInitializeForWindowFunc(acc, std::move(args), state);
}

SbExpr::Vector buildWindowAddDerivative(StageBuilderState& state,
                                        const WindowFunctionStatement& stmt,
                                        StringDataMap<SbExpr> args) {
    return addDocument(state);
}

SbExpr::Vector buildWindowRemoveDerivative(StageBuilderState& state,
                                           const WindowFunctionStatement& stmt,
                                           StringDataMap<SbExpr> args) {
    return removeDocument(state);
}

SbExpr buildWindowFinalizeDerivative(StageBuilderState& state,
                                     const WindowFunctionStatement& stmt,
                                     SbSlotVector slots,
                                     StringDataMap<SbExpr> args) {
    auto acc = Accum::Op{stmt.expr->getOpName()};
    return buildFinalizeForWindowFunc(acc, std::move(args), state, slots);
}

SbExpr::Vector buildWindowAddStdDev(StageBuilderState& state,
                                    const WindowFunctionStatement& stmt,
                                    SbExpr arg) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("aggRemovableStdDevAdd", std::move(arg)));
}

SbExpr::Vector buildWindowRemoveStdDev(StageBuilderState& state,
                                       const WindowFunctionStatement& stmt,
                                       SbExpr arg) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("aggRemovableStdDevRemove", std::move(arg)));
}

SbExpr buildWindowFinalizeStdDevSamp(StageBuilderState& state,
                                     const WindowFunctionStatement& stmt,
                                     SbSlotVector slots) {
    SbExprBuilder b(state);

    tassert(8019606, "Incorrect number of arguments", slots.size() == 1);
    SbExpr::Vector exprs;
    for (auto slot : slots) {
        exprs.push_back(b.makeVariable(slot));
    }
    return b.makeFunction("aggRemovableStdDevSampFinalize", std::move(exprs));
}

SbExpr buildWindowFinalizeStdDevPop(StageBuilderState& state,
                                    const WindowFunctionStatement& stmt,
                                    SbSlotVector slots) {
    SbExprBuilder b(state);

    tassert(8019607, "Incorrect number of arguments", slots.size() == 1);
    SbExpr::Vector exprs;
    for (auto slot : slots) {
        exprs.push_back(b.makeVariable(slot));
    }
    return b.makeFunction("aggRemovableStdDevPopFinalize", std::move(exprs));
}

SbExpr::Vector buildWindowAddAvg(StageBuilderState& state,
                                 const WindowFunctionStatement& stmt,
                                 SbExpr arg) {
    SbExprBuilder b(state);

    SbExpr::Vector exprs;

    exprs.push_back(b.makeFunction("aggRemovableSumAdd", arg.clone()));

    // For the counter we need to skip non-numeric values ourselves.
    auto addend = b.makeIf(b.makeFunction("isNumber", b.makeFillEmptyNull(std::move(arg))),
                           b.makeInt64Constant(1),
                           b.makeInt64Constant(0));

    auto counterExpr = b.makeFunction("sum", std::move(addend));
    exprs.push_back(std::move(counterExpr));

    return exprs;
}

SbExpr::Vector buildWindowRemoveAvg(StageBuilderState& state,
                                    const WindowFunctionStatement& stmt,
                                    SbExpr arg) {
    SbExprBuilder b(state);

    SbExpr::Vector exprs;
    exprs.push_back(b.makeFunction("aggRemovableSumRemove", arg.clone()));

    // For the counter we need to skip non-numeric values ourselves.
    auto subtrahend = b.makeIf(b.makeFunction("isNumber", b.makeFillEmptyNull(std::move(arg))),
                               b.makeInt64Constant(-1),
                               b.makeInt64Constant(0));
    auto counterExpr = b.makeFunction("sum", std::move(subtrahend));
    exprs.push_back(std::move(counterExpr));
    return exprs;
}

SbExpr buildWindowFinalizeAvg(StageBuilderState& state,
                              const WindowFunctionStatement& stmt,
                              SbSlotVector slots) {
    SbExprBuilder b(state);

    // Slot 0 contains the accumulated sum, and slot 1 contains the count of summed items.
    tassert(7965900,
            str::stream() << "Expected two slots to finalize avg, got: " << slots.size(),
            slots.size() == 2);

    SbExpr::Vector exprs;
    for (auto slot : slots) {
        exprs.push_back(b.makeVariable(slot));
    }

    return b.makeFunction("aggRemovableAvgFinalize", std::move(exprs));
}

SbExpr::Vector buildWindowAddFirstLast(StageBuilderState& state,
                                       const WindowFunctionStatement& stmt,
                                       SbExpr args) {
    return addDocument(state);
}

SbExpr::Vector buildWindowRemoveFirstLast(StageBuilderState& state,
                                          const WindowFunctionStatement& stmt,
                                          SbExpr args) {
    return removeDocument(state);
}

SbExpr buildWindowFinalizeFirstLast(StageBuilderState& state,
                                    const WindowFunctionStatement& stmt,
                                    SbSlotVector slots,
                                    StringDataMap<SbExpr> args) {
    SbExprBuilder b(state);

    tassert(8085500, "Expected a single slot", slots.size() == 1);
    auto it = args.find(Accum::kInput);
    tassert(8085501,
            str::stream() << "Window function " << stmt.expr->getOpName() << " expects '"
                          << Accum::kInput << "' argument",
            it != args.end());
    auto input = std::move(it->second);

    it = args.find(Accum::kDefaultVal);
    tassert(8293502,
            str::stream() << "Window function " << stmt.expr->getOpName() << " expects '"
                          << Accum::kDefaultVal << "' argument",
            it != args.end());
    auto defaultVal = std::move(it->second);

    return b.makeIf(b.makeBinaryOp(sbe::EPrimBinary::logicAnd,
                                   b.makeFunction("exists", b.makeVariable(slots[0])),
                                   b.makeBinaryOp(sbe::EPrimBinary::greater,
                                                  b.makeVariable(slots[0]),
                                                  b.makeInt64Constant(0))),
                    b.makeFillEmptyNull(std::move(input)),
                    std::move(defaultVal));
}

SbExpr::Vector buildWindowInitializeFirstN(StageBuilderState& state,
                                           const WindowFunctionStatement& stmt,
                                           StringDataMap<SbExpr> args) {
    SbExprBuilder b(state);

    auto it = args.find(Accum::kMaxSize);
    uassert(8070617, "Expected max size argument", it != args.end());
    auto maxSizeArg = std::move(it->second);
    uassert(8070609, "$firstN init argument should be a constant", maxSizeArg.isConstantExpr());

    return SbExpr::makeSeq(b.makeFunction("aggRemovableFirstNInit", std::move(maxSizeArg)));
}

SbExpr::Vector buildWindowAddFirstN(StageBuilderState& state,
                                    const WindowFunctionStatement& stmt,
                                    SbExpr arg) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(
        b.makeFunction("aggRemovableFirstNAdd", b.makeFillEmptyNull(std::move(arg))));
}

SbExpr::Vector buildWindowRemoveFirstN(StageBuilderState& state,
                                       const WindowFunctionStatement& stmt,
                                       SbExpr arg) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(
        b.makeFunction("aggRemovableFirstNRemove", b.makeFillEmptyNull(std::move(arg))));
}

SbExpr buildWindowFinalizeFirstN(StageBuilderState& state,
                                 const WindowFunctionStatement& stmt,
                                 SbSlotVector slots) {
    SbExprBuilder b(state);

    tassert(8070605, "Expected a single slot", slots.size() == 1);
    return b.makeFunction("aggRemovableFirstNFinalize", b.makeVariable(slots[0]));
}

SbExpr::Vector buildWindowInitializeLastN(StageBuilderState& state,
                                          const WindowFunctionStatement& stmt,
                                          StringDataMap<SbExpr> args) {
    SbExprBuilder b(state);

    auto it = args.find(Accum::kMaxSize);
    uassert(8070616, "Expected max size argument", it != args.end());
    auto maxSizeArg = std::move(it->second);
    uassert(8070610, "$lastN init argument should be a constant", maxSizeArg.isConstantExpr());

    return SbExpr::makeSeq(b.makeFunction("aggRemovableLastNInit", std::move(maxSizeArg)));
}

SbExpr::Vector buildWindowAddLastN(StageBuilderState& state,
                                   const WindowFunctionStatement& stmt,
                                   SbExpr arg) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(
        b.makeFunction("aggRemovableLastNAdd", b.makeFillEmptyNull(std::move(arg))));
}

SbExpr::Vector buildWindowRemoveLastN(StageBuilderState& state,
                                      const WindowFunctionStatement& stmt,
                                      SbExpr arg) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(
        b.makeFunction("aggRemovableLastNRemove", b.makeFillEmptyNull(std::move(arg))));
}

SbExpr buildWindowFinalizeLastN(StageBuilderState& state,
                                const WindowFunctionStatement& stmt,
                                SbSlotVector slots) {
    SbExprBuilder b(state);

    tassert(8070606, "Expected a single slot", slots.size() == 1);
    return b.makeFunction("aggRemovableLastNFinalize", b.makeVariable(slots[0]));
}

SbExpr::Vector buildWindowInitializeAddToSet(StageBuilderState& state,
                                             const WindowFunctionStatement& stmt,
                                             StringDataMap<SbExpr> args) {
    SbExprBuilder b(state);
    auto collatorSlot = state.getCollatorSlot();

    SbExpr::Vector exprs;

    if (collatorSlot) {
        exprs.push_back(
            b.makeFunction("aggRemovableAddToSetCollInit", b.makeVariable(*collatorSlot)));
    } else {
        exprs.push_back(b.makeFunction("aggRemovableAddToSetInit"));
    }
    return exprs;
}

SbExpr::Vector buildWindowAddAddToSet(StageBuilderState& state,
                                      const WindowFunctionStatement& stmt,
                                      SbExpr arg) {
    SbExprBuilder b(state);

    const int cap = internalQueryMaxAddToSetBytes.load();
    return SbExpr::makeSeq(
        b.makeFunction("aggRemovableAddToSetAdd", std::move(arg), b.makeInt32Constant(cap)));
}

SbExpr::Vector buildWindowRemoveAddToSet(StageBuilderState& state,
                                         const WindowFunctionStatement& stmt,
                                         SbExpr arg) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(b.makeFunction("aggRemovableAddToSetRemove", std::move(arg)));
}

SbExpr buildWindowFinalizeAddToSet(StageBuilderState& state,
                                   const WindowFunctionStatement& stmt,
                                   SbSlotVector slots) {
    SbExprBuilder b(state);

    SbExpr::Vector exprs;
    for (auto slot : slots) {
        exprs.push_back(b.makeVariable(slot));
    }
    return b.makeFunction("aggRemovableAddToSetFinalize", std::move(exprs));
}

SbExpr::Vector buildWindowInitializeMinMax(StageBuilderState& state,
                                           const WindowFunctionStatement& stmt,
                                           StringDataMap<SbExpr> args) {
    SbExprBuilder b(state);
    auto collatorSlot = state.getCollatorSlot();

    SbExpr::Vector exprs;

    auto cap = internalQueryTopNAccumulatorBytes.load();

    if (collatorSlot) {
        exprs.push_back(b.makeFunction("aggRemovableMinMaxNCollInit",
                                       b.makeInt32Constant(1),
                                       b.makeInt32Constant(cap),
                                       b.makeVariable(*collatorSlot)));
    } else {
        exprs.push_back(b.makeFunction(
            "aggRemovableMinMaxNInit", b.makeInt32Constant(1), b.makeInt32Constant(cap)));
    }
    return exprs;
}

SbExpr::Vector buildWindowInitializeMinMaxN(StageBuilderState& state,
                                            const WindowFunctionStatement& stmt,
                                            StringDataMap<SbExpr> args) {
    SbExprBuilder b(state);

    auto it = args.find(Accum::kMaxSize);
    uassert(8178112, "Expected max size argument", it != args.end());
    auto maxSizeArg = std::move(it->second);
    uassert(8178113, "$minN/$maxN init argument should be a constant", maxSizeArg.isConstantExpr());

    SbExpr::Vector exprs;

    auto cap = internalQueryTopNAccumulatorBytes.load();

    auto collatorSlot = state.getCollatorSlot();

    if (collatorSlot) {
        exprs.push_back(b.makeFunction("aggRemovableMinMaxNCollInit",
                                       std::move(maxSizeArg),
                                       b.makeInt32Constant(cap),

                                       b.makeVariable(*collatorSlot)));
    } else {
        exprs.push_back(b.makeFunction(
            "aggRemovableMinMaxNInit", std::move(maxSizeArg), b.makeInt32Constant(cap)));
    }
    return exprs;
}

SbExpr::Vector buildWindowAddMinMaxN(StageBuilderState& state,
                                     const WindowFunctionStatement& stmt,
                                     SbExpr arg) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(
        b.makeFunction("aggRemovableMinMaxNAdd", b.makeFunction("setToArray", std::move(arg))));
}

SbExpr::Vector buildWindowRemoveMinMaxN(StageBuilderState& state,
                                        const WindowFunctionStatement& stmt,
                                        SbExpr arg) {
    SbExprBuilder b(state);
    return SbExpr::makeSeq(
        b.makeFunction("aggRemovableMinMaxNRemove", b.makeFunction("setToArray", std::move(arg))));
}

SbExpr buildWindowFinalizeMinN(StageBuilderState& state,
                               const WindowFunctionStatement& stmt,
                               SbSlotVector slots) {
    SbExprBuilder b(state);

    tassert(8178130, "Expected a single slot", slots.size() == 1);
    return b.makeFunction("aggRemovableMinNFinalize", b.makeVariable(slots[0]));
}

SbExpr buildWindowFinalizeMaxN(StageBuilderState& state,
                               const WindowFunctionStatement& stmt,
                               SbSlotVector slots) {
    SbExprBuilder b(state);

    tassert(8178131, "Expected a single slot", slots.size() == 1);
    return b.makeFunction("aggRemovableMaxNFinalize", b.makeVariable(slots[0]));
}

SbExpr buildWindowFinalizeMin(StageBuilderState& state,
                              const WindowFunctionStatement& stmt,
                              SbSlotVector slots) {
    SbExprBuilder b(state);

    tassert(8124914, "Expected a single slot", slots.size() == 1);
    return b.makeFillEmptyNull(
        b.makeFunction("getElement",
                       b.makeFunction("aggRemovableMinNFinalize", b.makeVariable(slots[0])),
                       b.makeInt32Constant(0)));
}

SbExpr buildWindowFinalizeMax(StageBuilderState& state,
                              const WindowFunctionStatement& stmt,
                              SbSlotVector slots) {
    SbExprBuilder b(state);

    tassert(8124915, "Expected a single slot", slots.size() == 1);
    return b.makeFillEmptyNull(
        b.makeFunction("getElement",
                       b.makeFunction("aggRemovableMaxNFinalize", b.makeVariable(slots[0])),
                       b.makeInt32Constant(0)));
}

SbExpr::Vector buildWindowInitializeTopBottomN(StageBuilderState& state,
                                               std::string func,
                                               StringDataMap<SbExpr> args) {
    SbExprBuilder b(state);

    auto maxAccumulatorBytes = internalQueryTopNAccumulatorBytes.load();
    auto it = args.find(Accum::kMaxSize);
    uassert(8155719, "Expected max size argument", it != args.end());
    auto nExpr = std::move(it->second);
    uassert(8155720, "$topN/$bottomN init argument should be a constant", nExpr.isConstantExpr());

    return SbExpr::makeSeq(
        b.makeFunction(func, std::move(nExpr), b.makeInt32Constant(maxAccumulatorBytes)));
}

SbExpr::Vector buildWindowInitializeTopN(StageBuilderState& state,
                                         const WindowFunctionStatement& stmt,
                                         StringDataMap<SbExpr> args) {
    return buildWindowInitializeTopBottomN(state, "aggRemovableTopNInit", std::move(args));
}

SbExpr::Vector buildRemovableTopBottomN(StageBuilderState& state,
                                        std::string func,
                                        StringDataMap<SbExpr> args) {
    SbExprBuilder b(state);

    auto it = args.find(Accum::kSortBy);
    tassert(8155712,
            str::stream() << "Expected a '" << Accum::kSortBy << "' argument",
            it != args.end());
    auto key = std::move(it->second);

    it = args.find(Accum::kValue);
    tassert(8155713,
            str::stream() << "Expected a '" << Accum::kValue << "' argument",
            it != args.end());
    auto value = std::move(it->second);

    return SbExpr::makeSeq(b.makeFunction(func, std::move(key), std::move(value)));
}

SbExpr::Vector buildWindowAddTopN(StageBuilderState& state,
                                  const WindowFunctionStatement& stmt,
                                  StringDataMap<SbExpr> args) {
    return buildRemovableTopBottomN(state, "aggRemovableTopNAdd", std::move(args));
}

SbExpr::Vector buildWindowRemoveTopN(StageBuilderState& state,
                                     const WindowFunctionStatement& stmt,
                                     StringDataMap<SbExpr> args) {
    return buildRemovableTopBottomN(state, "aggRemovableTopNRemove", std::move(args));
}

SbExpr buildWindowFinalizeTopN(StageBuilderState& state,
                               const WindowFunctionStatement& stmt,
                               SbSlotVector slots) {
    SbExprBuilder b(state);

    tassert(8155710, "Expected a single slot", slots.size() == 1);
    return b.makeFunction("aggRemovableTopNFinalize", b.makeVariable(slots[0]));
}

SbExpr buildWindowFinalizeTop(StageBuilderState& state,
                              const WindowFunctionStatement& stmt,
                              SbSlotVector slots) {
    SbExprBuilder b(state);

    tassert(8155721, "Expected a single slot", slots.size() == 1);
    return b.makeFillEmptyNull(
        b.makeFunction("getElement",
                       b.makeFunction("aggRemovableTopNFinalize", b.makeVariable(slots[0])),
                       b.makeInt32Constant(0)));
}

SbExpr::Vector buildWindowInitializeBottomN(StageBuilderState& state,
                                            const WindowFunctionStatement& stmt,
                                            StringDataMap<SbExpr> args) {
    return buildWindowInitializeTopBottomN(state, "aggRemovableBottomNInit", std::move(args));
}

SbExpr::Vector buildWindowAddBottomN(StageBuilderState& state,
                                     const WindowFunctionStatement& stmt,
                                     StringDataMap<SbExpr> args) {
    return buildRemovableTopBottomN(state, "aggRemovableBottomNAdd", std::move(args));
}

SbExpr::Vector buildWindowRemoveBottomN(StageBuilderState& state,
                                        const WindowFunctionStatement& stmt,
                                        StringDataMap<SbExpr> args) {
    return buildRemovableTopBottomN(state, "aggRemovableBottomNRemove", std::move(args));
}

SbExpr buildWindowFinalizeBottomN(StageBuilderState& state,
                                  const WindowFunctionStatement& stmt,
                                  SbSlotVector slots) {
    SbExprBuilder b(state);

    tassert(8155714, "Expected a single slot", slots.size() == 1);
    return b.makeFunction("aggRemovableBottomNFinalize", b.makeVariable(slots[0]));
}

SbExpr buildWindowFinalizeBottom(StageBuilderState& state,
                                 const WindowFunctionStatement& stmt,
                                 SbSlotVector slots) {
    SbExprBuilder b(state);

    tassert(8155722, "Expected a single slot", slots.size() == 1);
    return b.makeFillEmptyNull(
        b.makeFunction("getElement",
                       b.makeFunction("aggRemovableBottomNFinalize", b.makeVariable(slots[0])),
                       b.makeInt32Constant(0)));
}

SbExpr::Vector buildWindowInit(StageBuilderState& state, const WindowFunctionStatement& stmt) {
    StringDataMap<SbExpr> args;

    return buildWindowInit(state, stmt, std::move(args));
}

SbExpr::Vector buildWindowInit(StageBuilderState& state,
                               const WindowFunctionStatement& stmt,
                               StringDataMap<SbExpr> args) {
    using BuildInitFn = std::function<SbExpr::Vector(
        StageBuilderState&, const WindowFunctionStatement&, StringDataMap<SbExpr>)>;

    static const StringDataMap<BuildInitFn> kWindowFunctionBuilders = {
        {AccumulatorMin::kName, &buildWindowInitializeMinMax},
        {AccumulatorMax::kName, &buildWindowInitializeMinMax},
        {AccumulatorFirst::kName, &emptyInitializer<1>},
        {AccumulatorLast::kName, &emptyInitializer<1>},
        {"$top", &buildWindowInitializeTopN},
        {"$bottom", &buildWindowInitializeBottomN},
        {"$sum", &emptyInitializer<1>},
        {AccumulatorAvg::kName, &emptyInitializer<2>},
        {"$stdDevSamp", &emptyInitializer<1>},
        {"$stdDevPop", &emptyInitializer<1>},
        {"$push", &emptyInitializer<1>},
        {AccumulatorAddToSet::kName, &buildWindowInitializeAddToSet},
        {AccumulatorMinN::kName, &buildWindowInitializeMinMaxN},
        {AccumulatorMaxN::kName, &buildWindowInitializeMinMaxN},
        {"$firstN", &buildWindowInitializeFirstN},
        {"$lastN", &buildWindowInitializeLastN},
        {"$topN", &buildWindowInitializeTopN},
        {"$bottomN", &buildWindowInitializeBottomN},
        {"$covarianceSamp", &emptyInitializer<1>},
        {"$covariancePop", &emptyInitializer<1>},
        {"$integral", &buildWindowInitializeIntegral},
        {"$derivative", &buildWindowInitializeDerivative},
        {"$shift", &emptyInitializer<1>},
    };

    auto opName = stmt.expr->getOpName();
    uassert(8070615,
            str::stream() << "Unsupported window function in SBE stage builder: " << opName,
            kWindowFunctionBuilders.find(opName) != kWindowFunctionBuilders.end());

    return std::invoke(kWindowFunctionBuilders.at(opName), state, stmt, std::move(args));
}

SbExpr::Vector buildWindowAdd(StageBuilderState& state,
                              const WindowFunctionStatement& stmt,
                              SbExpr arg) {
    using BuildAddFn =
        std::function<SbExpr::Vector(StageBuilderState&, const WindowFunctionStatement&, SbExpr)>;

    static const StringDataMap<BuildAddFn> kWindowFunctionBuilders = {
        {"$sum", &buildWindowAddSum},
        {"$push", &buildWindowAddPush},
        {"$stdDevSamp", &buildWindowAddStdDev},
        {"$stdDevPop", &buildWindowAddStdDev},
        {AccumulatorAvg::kName, &buildWindowAddAvg},
        {AccumulatorFirst::kName, &buildWindowAddFirstLast},
        {AccumulatorLast::kName, &buildWindowAddFirstLast},
        {"$firstN", &buildWindowAddFirstN},
        {"$lastN", &buildWindowAddLastN},
        {AccumulatorAddToSet::kName, &buildWindowAddAddToSet},
        {AccumulatorMinN::kName, &buildWindowAddMinMaxN},
        {AccumulatorMaxN::kName, &buildWindowAddMinMaxN},
        {AccumulatorMin::kName, &buildWindowAddMinMaxN},
        {AccumulatorMax::kName, &buildWindowAddMinMaxN},
        {"$shift", &buildWindowAddFirstLast},
    };

    auto opName = stmt.expr->getOpName();
    uassert(7914604,
            str::stream() << "Unsupported window function in SBE stage builder: " << opName,
            kWindowFunctionBuilders.find(opName) != kWindowFunctionBuilders.end());

    return std::invoke(kWindowFunctionBuilders.at(opName), state, stmt, std::move(arg));
}

SbExpr::Vector buildWindowAdd(StageBuilderState& state,
                              const WindowFunctionStatement& stmt,
                              StringDataMap<SbExpr> args) {
    using BuildAddFn = std::function<SbExpr::Vector(
        StageBuilderState&, const WindowFunctionStatement&, StringDataMap<SbExpr>)>;

    static const StringDataMap<BuildAddFn> kWindowFunctionBuilders = {
        {"$covarianceSamp", &buildWindowAddCovariance},
        {"$covariancePop", &buildWindowAddCovariance},
        {"$integral", &buildWindowAddIntegral},
        {"$derivative", &buildWindowAddDerivative},
        {"$topN", &buildWindowAddTopN},
        {"$top", &buildWindowAddTopN},
        {"$bottomN", &buildWindowAddBottomN},
        {"$bottom", &buildWindowAddBottomN},
    };

    auto opName = stmt.expr->getOpName();
    uassert(7820816,
            str::stream() << "Unsupported window function in SBE stage builder: " << opName,
            kWindowFunctionBuilders.find(opName) != kWindowFunctionBuilders.end());

    return std::invoke(kWindowFunctionBuilders.at(opName), state, stmt, std::move(args));
}

SbExpr::Vector buildWindowRemove(StageBuilderState& state,
                                 const WindowFunctionStatement& stmt,
                                 SbExpr arg) {
    using BuildRemoveFn =
        std::function<SbExpr::Vector(StageBuilderState&, const WindowFunctionStatement&, SbExpr)>;

    static const StringDataMap<BuildRemoveFn> kWindowFunctionBuilders = {
        {"$sum", &buildWindowRemoveSum},
        {"$push", &buildWindowRemovePush},
        {"$stdDevSamp", &buildWindowRemoveStdDev},
        {"$stdDevPop", &buildWindowRemoveStdDev},
        {AccumulatorAvg::kName, &buildWindowRemoveAvg},
        {AccumulatorFirst::kName, &buildWindowRemoveFirstLast},
        {AccumulatorLast::kName, &buildWindowRemoveFirstLast},
        {"$firstN", &buildWindowRemoveFirstN},
        {"$lastN", &buildWindowRemoveLastN},
        {AccumulatorAddToSet::kName, &buildWindowRemoveAddToSet},
        {AccumulatorMinN::kName, &buildWindowRemoveMinMaxN},
        {AccumulatorMaxN::kName, &buildWindowRemoveMinMaxN},
        {AccumulatorMin::kName, &buildWindowRemoveMinMaxN},
        {AccumulatorMax::kName, &buildWindowRemoveMinMaxN},
        {"$shift", &buildWindowRemoveFirstLast},
    };

    auto opName = stmt.expr->getOpName();
    uassert(7914605,
            str::stream() << "Unsupported window function in SBE stage builder: " << opName,
            kWindowFunctionBuilders.find(opName) != kWindowFunctionBuilders.end());

    return std::invoke(kWindowFunctionBuilders.at(opName), state, stmt, std::move(arg));
}  // namespace mongo::stage_builder

SbExpr::Vector buildWindowRemove(StageBuilderState& state,
                                 const WindowFunctionStatement& stmt,
                                 StringDataMap<SbExpr> args) {
    using BuildRemoveFn = std::function<SbExpr::Vector(
        StageBuilderState&, const WindowFunctionStatement&, StringDataMap<SbExpr>)>;

    static const StringDataMap<BuildRemoveFn> kWindowFunctionBuilders = {
        {"$covarianceSamp", &buildWindowRemoveCovariance},
        {"$covariancePop", &buildWindowRemoveCovariance},
        {"$integral", &buildWindowRemoveIntegral},
        {"$derivative", &buildWindowRemoveDerivative},
        {"$topN", &buildWindowRemoveTopN},
        {"$top", &buildWindowRemoveTopN},
        {"$bottomN", &buildWindowRemoveBottomN},
        {"$bottom", &buildWindowRemoveBottomN},
    };

    auto opName = stmt.expr->getOpName();
    uassert(7820817,
            str::stream() << "Unsupported window function in SBE stage builder: " << opName,
            kWindowFunctionBuilders.find(opName) != kWindowFunctionBuilders.end());

    return std::invoke(kWindowFunctionBuilders.at(opName), state, stmt, std::move(args));
}

SbExpr buildWindowFinalize(StageBuilderState& state,
                           const WindowFunctionStatement& stmt,
                           SbSlotVector values) {
    using BuildFinalizeFn = std::function<SbExpr(
        StageBuilderState&, const WindowFunctionStatement&, SbSlotVector values)>;

    static const StringDataMap<BuildFinalizeFn> kWindowFunctionBuilders = {
        {"$sum", &buildWindowFinalizeSum},
        {"$covarianceSamp", &buildWindowFinalizeCovarianceSamp},
        {"$covariancePop", &buildWindowFinalizeCovariancePop},
        {"$push", &buildWindowFinalizePush},
        {"$integral", &buildWindowFinalizeIntegral},
        {"$stdDevSamp", &buildWindowFinalizeStdDevSamp},
        {"$stdDevPop", &buildWindowFinalizeStdDevPop},
        {AccumulatorAvg::kName, &buildWindowFinalizeAvg},
        {"$firstN", &buildWindowFinalizeFirstN},
        {"$lastN", &buildWindowFinalizeLastN},
        {AccumulatorAddToSet::kName, &buildWindowFinalizeAddToSet},
        {AccumulatorMinN::kName, &buildWindowFinalizeMinN},
        {AccumulatorMaxN::kName, &buildWindowFinalizeMaxN},
        {AccumulatorMin::kName, &buildWindowFinalizeMin},
        {AccumulatorMax::kName, &buildWindowFinalizeMax},
        {"$topN", &buildWindowFinalizeTopN},
        {"$top", &buildWindowFinalizeTop},
        {"$bottomN", &buildWindowFinalizeBottomN},
        {"$bottom", &buildWindowFinalizeBottom},
    };

    auto opName = stmt.expr->getOpName();
    uassert(7914606,
            str::stream() << "Unsupported window function in SBE stage builder: " << opName,
            kWindowFunctionBuilders.find(opName) != kWindowFunctionBuilders.end());

    return std::invoke(kWindowFunctionBuilders.at(opName), state, stmt, std::move(values));
}

SbExpr buildWindowFinalize(StageBuilderState& state,
                           const WindowFunctionStatement& stmt,
                           SbSlotVector values,
                           StringDataMap<SbExpr> args) {
    using BuildFinalizeFn = std::function<SbExpr(
        StageBuilderState&, const WindowFunctionStatement&, SbSlotVector, StringDataMap<SbExpr>)>;

    static const StringDataMap<BuildFinalizeFn> kWindowFunctionBuilders = {
        {"$derivative", &buildWindowFinalizeDerivative},
        {AccumulatorFirst::kName, &buildWindowFinalizeFirstLast},
        {AccumulatorLast::kName, &buildWindowFinalizeFirstLast},
        {"$shift", &buildWindowFinalizeFirstLast},
    };

    auto opName = stmt.expr->getOpName();
    uassert(7993415,
            str::stream() << "Unsupported window function in SBE stage builder: " << opName,
            kWindowFunctionBuilders.find(opName) != kWindowFunctionBuilders.end());

    return std::invoke(
        kWindowFunctionBuilders.at(opName), state, stmt, std::move(values), std::move(args));
}

SbExpr::Vector buildAccumulatorForWindowFunc(const Accum::Op& acc,
                                             SbExpr input,
                                             StageBuilderState& state) {
    StringDataMap<SbExpr> inputs;
    inputs.emplace(Accum::kInput, std::move(input));

    // Call buildAccumExprs() to generate the accum exprs. The inputs to buildAccumExprs() are
    // wrapped in an Accum::NamedExprsMapWrapper. buildAccumExprs() will take care of converting
    // the Accum::NamedExprsMapWrapper to the appropriate input type.
    //
    // Once the SBE window function implementation has been updated to use the appropriate
    // subclasses of Accum::Inputs, the use of Accum::NamedExprsMapWrapper will no longer be
    // necessary.
    auto accArgs = acc.buildAccumExprs(
        state, std::make_unique<Accum::NamedExprsMapWrapper>(std::move(inputs)));

    // Pass the accum exprs to buildAccumAggs() to generate the accum aggs, and then return
    // the generated accum aggs.
    return acc.buildAccumAggs(state, std::move(accArgs));
}

SbExpr::Vector buildAccumulatorForWindowFunc(const Accum::Op& acc,
                                             StringDataMap<SbExpr> inputs,
                                             StageBuilderState& state) {
    // Call buildAccumExprs() to generate the accum exprs. The inputs to buildAccumExprs() are
    // wrapped in an Accum::NamedExprsMapWrapper. buildAccumExprs() will take care of converting
    // the Accum::NamedExprsMapWrapper to the appropriate input type.
    //
    // Once the SBE window function implementation has been updated to use the appropriate
    // subclasses of Accum::Inputs, the use of Accum::NamedExprsMapWrapper will no longer be
    // necessary.
    auto accArgs = acc.buildAccumExprs(
        state, std::make_unique<Accum::NamedExprsMapWrapper>(std::move(inputs)));

    // Call buildAccumAggs() to generate the accum aggs, and then return the generated accum aggs.
    return acc.buildAccumAggs(state, std::move(accArgs));
}

SbExpr::Vector buildInitializeForWindowFunc(const Accum::Op& acc,
                                            StringDataMap<SbExpr> inputs,
                                            StageBuilderState& state) {
    // Call buildInitialize() on the Accum::Op. The inputs to buildInitialize() are wrapped
    // in an Accum::NamedExprsMapWrapper. buildInitialize() will take care of converting the
    // Accum::NamedExprsMapWrapper to the appropriate input type.
    //
    // Once the SBE window function implementation has been updated to use the appropriate
    // subclasses of Accum::Inputs, the use of Accum::NamedExprsMapWrapper will no longer be
    // necessary.
    return acc.buildInitialize(state,
                               std::make_unique<Accum::NamedExprsMapWrapper>(std::move(inputs)));
}

SbExpr::Vector buildInitializeForWindowFunc(const Accum::Op& acc, StageBuilderState& state) {
    StringDataMap<SbExpr> inputs;
    return buildInitializeForWindowFunc(acc, std::move(inputs), state);
}

SbExpr buildFinalizeForWindowFunc(const Accum::Op& acc,
                                  StringDataMap<SbExpr> inputs,
                                  StageBuilderState& state,
                                  const SbSlotVector& aggSlots) {
    // Call buildFinalize() on the Accum::Op. The inputs to buildFinalize() are wrapped
    // in an Accum::NamedExprsMapWrapper. buildFinalize() will take care of converting the
    // Accum::NamedExprsMapWrapper to the appropriate input type.
    //
    // Once the SBE window function implementation has been updated to use the appropriate
    // subclasses of Accum::Inputs, the use of Accum::NamedExprsMapWrapper will no longer be
    // necessary.
    return acc.buildFinalize(
        state, std::make_unique<Accum::NamedExprsMapWrapper>(std::move(inputs)), aggSlots);
}

SbExpr buildFinalizeForWindowFunc(const Accum::Op& acc,
                                  StageBuilderState& state,
                                  const SbSlotVector& aggSlots) {
    StringDataMap<SbExpr> inputs;
    return buildFinalizeForWindowFunc(acc, std::move(inputs), state, aggSlots);
}

}  // namespace mongo::stage_builder
