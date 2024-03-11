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

namespace mongo::stage_builder {

template <int N>
std::vector<std::unique_ptr<sbe::EExpression>> emptyInitializer(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    StringDataMap<std::unique_ptr<sbe::EExpression>> argExpr,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    return std::vector<std::unique_ptr<sbe::EExpression>>{N};
}

std::vector<std::unique_ptr<sbe::EExpression>> addDocument() {
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(makeFunction("sum", makeInt64Constant(1)));
    return exprs;
}

std::vector<std::unique_ptr<sbe::EExpression>> removeDocument() {
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(makeFunction("sum", makeInt64Constant(-1)));
    return exprs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowAddSum(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(makeFunction("aggRemovableSumAdd", std::move(arg)));
    return exprs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowRemoveSum(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(makeFunction("aggRemovableSumRemove", std::move(arg)));
    return exprs;
}

std::unique_ptr<sbe::EExpression> buildWindowFinalizeSum(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    sbe::value::SlotVector slots,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    sbe::EExpression::Vector exprs;
    for (auto slot : slots) {
        exprs.push_back(makeVariable(slot));
    }
    return makeE<sbe::EFunction>("aggRemovableSumFinalize", std::move(exprs));
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowAddCovariance(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    auto acc = Accum::Op{stmt.expr->getOpName()};
    return buildAccumulatorForWindowFunc(acc, std::move(args), state);
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowRemoveCovariance(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args) {
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

    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(makeFunction("aggCovarianceRemove", std::move(argX), std::move(argY)));
    return exprs;
}

std::unique_ptr<sbe::EExpression> buildWindowFinalizeCovarianceSamp(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    sbe::value::SlotVector slots,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    auto acc = Accum::Op{stmt.expr->getOpName()};
    return buildFinalizeForWindowFunc(acc, state, slots).extractExpr(state);
}

std::unique_ptr<sbe::EExpression> buildWindowFinalizeCovariancePop(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    sbe::value::SlotVector slots,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    auto acc = Accum::Op{stmt.expr->getOpName()};
    return buildFinalizeForWindowFunc(acc, state, slots).extractExpr(state);
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowAddPush(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(makeFunction("aggRemovablePushAdd", std::move(arg)));
    return exprs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowRemovePush(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(makeFunction("aggRemovablePushRemove", std::move(arg)));
    return exprs;
}

std::unique_ptr<sbe::EExpression> buildWindowFinalizePush(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    sbe::value::SlotVector slots,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    sbe::EExpression::Vector exprs;
    for (auto slot : slots) {
        exprs.push_back(makeVariable(slot));
    }
    return makeE<sbe::EFunction>("aggRemovablePushFinalize", std::move(exprs));
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowInitializeIntegral(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    auto it = args.find(Accum::kInput);
    tassert(8751306, "Expected input argument", it != args.end());
    auto unitExpr = std::move(it->second);

    std::vector<std::unique_ptr<sbe::EExpression>> aggs;
    aggs.push_back(makeFunction("aggIntegralInit", std::move(unitExpr), makeBoolConstant(false)));
    return aggs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowAddIntegral(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    auto acc = Accum::Op{stmt.expr->getOpName()};
    return buildAccumulatorForWindowFunc(acc, std::move(args), state);
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowRemoveIntegral(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args) {
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

    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(makeFunction("aggIntegralRemove", std::move(input), std::move(sortBy)));
    return exprs;
}

std::unique_ptr<sbe::EExpression> buildWindowFinalizeIntegral(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    sbe::value::SlotVector slots,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    auto acc = Accum::Op{stmt.expr->getOpName()};
    return buildFinalizeForWindowFunc(acc, state, slots).extractExpr(state);
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowInitializeDerivative(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    auto acc = Accum::Op{stmt.expr->getOpName()};

    return buildInitializeForWindowFunc(acc, std::move(args), state);
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowAddDerivative(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    return addDocument();
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowRemoveDerivative(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args) {
    return removeDocument();
}

std::unique_ptr<sbe::EExpression> buildWindowFinalizeDerivative(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    sbe::value::SlotVector slots,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    auto acc = Accum::Op{stmt.expr->getOpName()};
    return buildFinalizeForWindowFunc(acc, std::move(args), state, slots).extractExpr(state);
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowAddStdDev(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(makeFunction("aggRemovableStdDevAdd", std::move(arg)));
    return exprs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowRemoveStdDev(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(makeFunction("aggRemovableStdDevRemove", std::move(arg)));
    return exprs;
}

std::unique_ptr<sbe::EExpression> buildWindowFinalizeStdDevSamp(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    sbe::value::SlotVector slots,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    tassert(8019606, "Incorrect number of arguments", slots.size() == 1);
    sbe::EExpression::Vector exprs;
    for (auto slot : slots) {
        exprs.push_back(makeVariable(slot));
    }
    return makeE<sbe::EFunction>("aggRemovableStdDevSampFinalize", std::move(exprs));
}

std::unique_ptr<sbe::EExpression> buildWindowFinalizeStdDevPop(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    sbe::value::SlotVector slots,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    tassert(8019607, "Incorrect number of arguments", slots.size() == 1);
    sbe::EExpression::Vector exprs;
    for (auto slot : slots) {
        exprs.push_back(makeVariable(slot));
    }
    return makeE<sbe::EFunction>("aggRemovableStdDevPopFinalize", std::move(exprs));
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowAddAvg(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot) {

    std::vector<std::unique_ptr<sbe::EExpression>> exprs;

    exprs.push_back(makeFunction("aggRemovableSumAdd", arg->clone()));

    // For the counter we need to skip non-numeric values ourselves.
    auto addend = sbe::makeE<sbe::EIf>(makeFunction("isNumber", makeFillEmptyNull(std::move(arg))),
                                       makeInt64Constant(1),
                                       makeInt64Constant(0));

    auto counterExpr = makeFunction("sum", std::move(addend));
    exprs.push_back(std::move(counterExpr));

    return exprs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowRemoveAvg(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(makeFunction("aggRemovableSumRemove", arg->clone()));

    // For the counter we need to skip non-numeric values ourselves.
    auto subtrahend =
        sbe::makeE<sbe::EIf>(makeFunction("isNumber", makeFillEmptyNull(std::move(arg))),
                             makeInt64Constant(-1),
                             makeInt64Constant(0));
    auto counterExpr = makeFunction("sum", std::move(subtrahend));
    exprs.push_back(std::move(counterExpr));
    return exprs;
}

std::unique_ptr<sbe::EExpression> buildWindowFinalizeAvg(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    sbe::value::SlotVector slots,
    boost::optional<sbe::value::SlotId> collatorSlot) {

    // Slot 0 contains the accumulated sum, and slot 1 contains the count of summed items.
    tassert(7965900,
            str::stream() << "Expected two slots to finalize avg, got: " << slots.size(),
            slots.size() == 2);

    sbe::EExpression::Vector exprs;
    for (auto slot : slots) {
        exprs.push_back(makeVariable(slot));
    }

    return makeFunction("aggRemovableAvgFinalize", std::move(exprs));
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowAddFirstLast(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    std::unique_ptr<sbe::EExpression> args,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    return addDocument();
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowRemoveFirstLast(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    std::unique_ptr<sbe::EExpression> args,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    return removeDocument();
}

std::unique_ptr<sbe::EExpression> buildWindowFinalizeFirstLast(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    sbe::value::SlotVector slots,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot) {
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

    return sbe::makeE<sbe::EIf>(
        makeBinaryOp(
            sbe::EPrimBinary::logicAnd,
            makeFunction("exists", makeVariable(slots[0])),
            makeBinaryOp(sbe::EPrimBinary::greater, makeVariable(slots[0]), makeInt64Constant(0))),
        makeFillEmptyNull(std::move(input)),
        std::move(defaultVal));
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowInitializeFirstN(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    auto it = args.find(Accum::kMaxSize);
    uassert(8070617, "Expected max size argument", it != args.end());
    auto maxSizeArg = std::move(it->second);
    uassert(8070609,
            "$firstN init argument should be a constant",
            maxSizeArg->as<sbe::EConstant>() != nullptr);
    exprs.push_back(makeFunction("aggRemovableFirstNInit", std::move(maxSizeArg)));
    return exprs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowAddFirstN(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(makeFunction("aggRemovableFirstNAdd", makeFillEmptyNull(std::move(arg))));
    return exprs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowRemoveFirstN(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(makeFunction("aggRemovableFirstNRemove", makeFillEmptyNull(std::move(arg))));
    return exprs;
}

std::unique_ptr<sbe::EExpression> buildWindowFinalizeFirstN(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    sbe::value::SlotVector slots,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    tassert(8070605, "Expected a single slot", slots.size() == 1);
    return makeFunction("aggRemovableFirstNFinalize", makeVariable(slots[0]));
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowInitializeLastN(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    auto it = args.find(Accum::kMaxSize);
    uassert(8070616, "Expected max size argument", it != args.end());
    auto maxSizeArg = std::move(it->second);
    uassert(8070610,
            "$lastN init argument should be a constant",
            maxSizeArg->as<sbe::EConstant>() != nullptr);
    exprs.push_back(makeFunction("aggRemovableLastNInit", std::move(maxSizeArg)));
    return exprs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowAddLastN(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(makeFunction("aggRemovableLastNAdd", makeFillEmptyNull(std::move(arg))));
    return exprs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowRemoveLastN(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(makeFunction("aggRemovableLastNRemove", makeFillEmptyNull(std::move(arg))));
    return exprs;
}

std::unique_ptr<sbe::EExpression> buildWindowFinalizeLastN(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    sbe::value::SlotVector slots,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    tassert(8070606, "Expected a single slot", slots.size() == 1);
    return makeFunction("aggRemovableLastNFinalize", makeVariable(slots[0]));
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowInitializeAddToSet(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    if (collatorSlot) {
        exprs.push_back(makeFunction("aggRemovableAddToSetCollInit",
                                     sbe::makeE<sbe::EVariable>(*collatorSlot)));
    } else {
        exprs.push_back(makeFunction("aggRemovableAddToSetInit"));
    }
    return exprs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowAddAddToSet(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    const int cap = internalQueryMaxAddToSetBytes.load();
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(
        makeFunction("aggRemovableAddToSetAdd", std::move(arg), makeInt32Constant(cap)));
    return exprs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowRemoveAddToSet(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(makeFunction("aggRemovableAddToSetRemove", std::move(arg)));
    return exprs;
}

std::unique_ptr<sbe::EExpression> buildWindowFinalizeAddToSet(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    sbe::value::SlotVector slots,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    sbe::EExpression::Vector exprs;
    for (auto slot : slots) {
        exprs.push_back(makeVariable(slot));
    }
    return makeE<sbe::EFunction>("aggRemovableAddToSetFinalize", std::move(exprs));
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowInitializeMinMax(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;

    auto cap = internalQueryTopNAccumulatorBytes.load();

    if (collatorSlot) {
        exprs.push_back(makeFunction(
            "aggRemovableMinMaxNCollInit",
            makeConstant(sbe::value::TypeTags::NumberInt32, sbe::value::bitcastFrom<int32_t>(1)),
            makeInt32Constant(cap),
            sbe::makeE<sbe::EVariable>(*collatorSlot)));
    } else {
        exprs.push_back(makeFunction(
            "aggRemovableMinMaxNInit",
            makeConstant(sbe::value::TypeTags::NumberInt32, sbe::value::bitcastFrom<int32_t>(1)),
            makeInt32Constant(cap)));
    }
    return exprs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowInitializeMinMaxN(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    auto it = args.find(Accum::kMaxSize);
    uassert(8178112, "Expected max size argument", it != args.end());
    auto maxSizeArg = std::move(it->second);
    uassert(8178113,
            "$minN/$maxN init argument should be a constant",
            maxSizeArg->as<sbe::EConstant>() != nullptr);

    std::vector<std::unique_ptr<sbe::EExpression>> exprs;

    auto cap = internalQueryTopNAccumulatorBytes.load();

    if (collatorSlot) {
        exprs.push_back(makeFunction("aggRemovableMinMaxNCollInit",
                                     std::move(maxSizeArg),
                                     makeInt32Constant(cap),
                                     sbe::makeE<sbe::EVariable>(*collatorSlot)));
    } else {
        exprs.push_back(
            makeFunction("aggRemovableMinMaxNInit", std::move(maxSizeArg), makeInt32Constant(cap)));
    }
    return exprs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowAddMinMaxN(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(
        makeFunction("aggRemovableMinMaxNAdd", makeFunction("setToArray", std::move(arg))));
    return exprs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowRemoveMinMaxN(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(
        makeFunction("aggRemovableMinMaxNRemove", makeFunction("setToArray", std::move(arg))));
    return exprs;
}

std::unique_ptr<sbe::EExpression> buildWindowFinalizeMinN(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    sbe::value::SlotVector slots,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    tassert(8178130, "Expected a single slot", slots.size() == 1);
    return makeFunction("aggRemovableMinNFinalize", makeVariable(slots[0]));
}

std::unique_ptr<sbe::EExpression> buildWindowFinalizeMaxN(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    sbe::value::SlotVector slots,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    tassert(8178131, "Expected a single slot", slots.size() == 1);
    return makeFunction("aggRemovableMaxNFinalize", makeVariable(slots[0]));
}

std::unique_ptr<sbe::EExpression> buildWindowFinalizeMin(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    sbe::value::SlotVector slots,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    tassert(8124914, "Expected a single slot", slots.size() == 1);
    return makeFillEmptyNull(
        makeFunction("getElement",
                     makeFunction("aggRemovableMinNFinalize", makeVariable(slots[0])),
                     makeInt32Constant(0)));
}

std::unique_ptr<sbe::EExpression> buildWindowFinalizeMax(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    sbe::value::SlotVector slots,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    tassert(8124915, "Expected a single slot", slots.size() == 1);
    return makeFillEmptyNull(
        makeFunction("getElement",
                     makeFunction("aggRemovableMaxNFinalize", makeVariable(slots[0])),
                     makeInt32Constant(0)));
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowInitializeTopBottomN(
    std::string func, StringDataMap<std::unique_ptr<sbe::EExpression>> args) {
    auto maxAccumulatorBytes = internalQueryTopNAccumulatorBytes.load();
    auto it = args.find(Accum::kMaxSize);
    uassert(8155719, "Expected max size argument", it != args.end());
    auto nExpr = std::move(it->second);
    uassert(8155720,
            "$topN/$bottomN init argument should be a constant",
            nExpr->as<sbe::EConstant>() != nullptr);
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(makeFunction(func, std::move(nExpr), makeInt32Constant(maxAccumulatorBytes)));
    return exprs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowInitializeTopN(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    return buildWindowInitializeTopBottomN("aggRemovableTopNInit", std::move(args));
}

std::vector<std::unique_ptr<sbe::EExpression>> buildRemovableTopBottomN(
    std::string func, StringDataMap<std::unique_ptr<sbe::EExpression>> args) {

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

    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(makeFunction(func, std::move(key), std::move(value)));
    return exprs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowAddTopN(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    return buildRemovableTopBottomN("aggRemovableTopNAdd", std::move(args));
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowRemoveTopN(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args) {
    return buildRemovableTopBottomN("aggRemovableTopNRemove", std::move(args));
}

std::unique_ptr<sbe::EExpression> buildWindowFinalizeTopN(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    sbe::value::SlotVector slots,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    tassert(8155710, "Expected a single slot", slots.size() == 1);
    return makeFunction("aggRemovableTopNFinalize", makeVariable(slots[0]));
}

std::unique_ptr<sbe::EExpression> buildWindowFinalizeTop(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    sbe::value::SlotVector slots,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    tassert(8155721, "Expected a single slot", slots.size() == 1);
    return makeFillEmptyNull(
        makeFunction("getElement",
                     makeFunction("aggRemovableTopNFinalize", makeVariable(slots[0])),
                     makeInt32Constant(0)));
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowInitializeBottomN(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    return buildWindowInitializeTopBottomN("aggRemovableBottomNInit", std::move(args));
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowAddBottomN(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    return buildRemovableTopBottomN("aggRemovableBottomNAdd", std::move(args));
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowRemoveBottomN(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args) {
    return buildRemovableTopBottomN("aggRemovableBottomNRemove", std::move(args));
}

std::unique_ptr<sbe::EExpression> buildWindowFinalizeBottomN(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    sbe::value::SlotVector slots,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    tassert(8155714, "Expected a single slot", slots.size() == 1);
    return makeFunction("aggRemovableBottomNFinalize", makeVariable(slots[0]));
}

std::unique_ptr<sbe::EExpression> buildWindowFinalizeBottom(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    sbe::value::SlotVector slots,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    tassert(8155722, "Expected a single slot", slots.size() == 1);
    return makeFillEmptyNull(
        makeFunction("getElement",
                     makeFunction("aggRemovableBottomNFinalize", makeVariable(slots[0])),
                     makeInt32Constant(0)));
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowInit(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    StringDataMap<std::unique_ptr<sbe::EExpression>> args;

    return buildWindowInit(state, stmt, std::move(args), collatorSlot);
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowInit(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    using BuildInitFn = std::function<std::vector<std::unique_ptr<sbe::EExpression>>(
        StageBuilderState&,
        const WindowFunctionStatement&,
        StringDataMap<std::unique_ptr<sbe::EExpression>>,
        boost::optional<sbe::value::SlotId>)>;

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

    return std::invoke(
        kWindowFunctionBuilders.at(opName), state, stmt, std::move(args), collatorSlot);
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowAdd(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    using BuildAddFn = std::function<std::vector<std::unique_ptr<sbe::EExpression>>(
        StageBuilderState&,
        const WindowFunctionStatement&,
        std::unique_ptr<sbe::EExpression>,
        boost::optional<sbe::value::SlotId>)>;

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

    return std::invoke(
        kWindowFunctionBuilders.at(opName), state, stmt, std::move(arg), collatorSlot);
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowAdd(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    using BuildAddFn = std::function<std::vector<std::unique_ptr<sbe::EExpression>>(
        StageBuilderState&,
        const WindowFunctionStatement&,
        StringDataMap<std::unique_ptr<sbe::EExpression>>,
        boost::optional<sbe::value::SlotId>)>;

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

    return std::invoke(
        kWindowFunctionBuilders.at(opName), state, stmt, std::move(args), collatorSlot);
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowRemove(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    std::unique_ptr<sbe::EExpression> arg,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    using BuildRemoveFn = std::function<std::vector<std::unique_ptr<sbe::EExpression>>(
        StageBuilderState&,
        const WindowFunctionStatement&,
        std::unique_ptr<sbe::EExpression>,
        boost::optional<sbe::value::SlotId>)>;

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

    return std::invoke(
        kWindowFunctionBuilders.at(opName), state, stmt, std::move(arg), collatorSlot);
}  // namespace mongo::stage_builder

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowRemove(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args) {
    using BuildRemoveFn = std::function<std::vector<std::unique_ptr<sbe::EExpression>>(
        StageBuilderState&,
        const WindowFunctionStatement&,
        StringDataMap<std::unique_ptr<sbe::EExpression>>)>;

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

std::unique_ptr<sbe::EExpression> buildWindowFinalize(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    sbe::value::SlotVector values,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    using BuildFinalizeFn =
        std::function<std::unique_ptr<sbe::EExpression>(StageBuilderState&,
                                                        const WindowFunctionStatement&,
                                                        sbe::value::SlotVector values,
                                                        boost::optional<sbe::value::SlotId>)>;

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

    return std::invoke(
        kWindowFunctionBuilders.at(opName), state, stmt, std::move(values), collatorSlot);
}

std::unique_ptr<sbe::EExpression> buildWindowFinalize(
    StageBuilderState& state,
    const WindowFunctionStatement& stmt,
    sbe::value::SlotVector values,
    StringDataMap<std::unique_ptr<sbe::EExpression>> args,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    using BuildFinalizeFn = std::function<std::unique_ptr<sbe::EExpression>(
        StageBuilderState&,
        const WindowFunctionStatement&,
        sbe::value::SlotVector,
        StringDataMap<std::unique_ptr<sbe::EExpression>>,
        boost::optional<sbe::value::SlotId>)>;

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

    return std::invoke(kWindowFunctionBuilders.at(opName),
                       state,
                       stmt,
                       std::move(values),
                       std::move(args),
                       collatorSlot);
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorForWindowFunc(
    const Accum::Op& acc, std::unique_ptr<sbe::EExpression> input, StageBuilderState& state) {
    // Convert 'input' into a map with a single entry named 'kInput'.
    StringDataMap<std::unique_ptr<sbe::EExpression>> inputs;
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

    // Pass the accum exprs to buildAccumAggs() to generate the accum aggs.
    auto sbExprs = acc.buildAccumAggs(state, std::move(accArgs));

    // Convert 'sbExprs' to a vector of sbe::EExpressions and return it.
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    for (auto&& e : sbExprs) {
        exprs.emplace_back(e.extractExpr(state));
    }

    return exprs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulatorForWindowFunc(
    const Accum::Op& acc,
    StringDataMap<std::unique_ptr<sbe::EExpression>> inputs,
    StageBuilderState& state) {
    // Convert 'inputs' into the format that the buildAccumExprs() method expects.
    SbExpr::Vector inputSbExprs;
    std::vector<std::string> inputNames;

    // Call buildAccumExprs() to generate the accum exprs. The inputs to buildAccumExprs() are
    // wrapped in an Accum::NamedExprsMapWrapper. buildAccumExprs() will take care of converting
    // the Accum::NamedExprsMapWrapper to the appropriate input type.
    //
    // Once the SBE window function implementation has been updated to use the appropriate
    // subclasses of Accum::Inputs, the use of Accum::NamedExprsMapWrapper will no longer be
    // necessary.
    auto accArgs = acc.buildAccumExprs(
        state, std::make_unique<Accum::NamedExprsMapWrapper>(std::move(inputs)));
    auto sbExprs = acc.buildAccumAggs(state, std::move(accArgs));

    // Convert 'sbExprs' to a vector of sbe::EExpressions and return it.
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    for (auto&& e : sbExprs) {
        exprs.emplace_back(e.extractExpr(state));
    }

    return exprs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildInitializeForWindowFunc(
    const Accum::Op& acc,
    StringDataMap<std::unique_ptr<sbe::EExpression>> inputs,
    StageBuilderState& state) {
    // Call buildInitialize() on the Accum::Op. The inputs to buildInitialize() are wrapped
    // in an Accum::NamedExprsMapWrapper. buildInitialize() will take care of converting the
    // Accum::NamedExprsMapWrapper to the appropriate input type.
    //
    // Once the SBE window function implementation has been updated to use the appropriate
    // subclasses of Accum::Inputs, the use of Accum::NamedExprsMapWrapper will no longer be
    // necessary.
    auto sbExprs = acc.buildInitialize(
        state, std::make_unique<Accum::NamedExprsMapWrapper>(std::move(inputs)));

    // Convert 'sbExprs' to a vector of sbe::EExpressions and return it.
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    for (auto&& e : sbExprs) {
        exprs.emplace_back(e.extractExpr(state));
    }
    return exprs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildInitializeForWindowFunc(
    const Accum::Op& acc, StageBuilderState& state) {
    StringDataMap<std::unique_ptr<sbe::EExpression>> inputs;
    return buildInitializeForWindowFunc(acc, std::move(inputs), state);
}

SbExpr buildFinalizeForWindowFunc(const Accum::Op& acc,
                                  StringDataMap<std::unique_ptr<sbe::EExpression>> inputs,
                                  StageBuilderState& state,
                                  const sbe::value::SlotVector& aggSlots) {
    SbSlotVector aggSlotsVec;
    for (auto&& slot : aggSlots) {
        aggSlotsVec.emplace_back(SbSlot{slot});
    }

    // Call buildFinalize() on the Accum::Op. The inputs to buildFinalize() are wrapped
    // in an Accum::NamedExprsMapWrapper. buildFinalize() will take care of converting the
    // Accum::NamedExprsMapWrapper to the appropriate input type.
    //
    // Once the SBE window function implementation has been updated to use the appropriate
    // subclasses of Accum::Inputs, the use of Accum::NamedExprsMapWrapper will no longer be
    // necessary.
    return acc.buildFinalize(
        state, std::make_unique<Accum::NamedExprsMapWrapper>(std::move(inputs)), aggSlotsVec);
}

SbExpr buildFinalizeForWindowFunc(const Accum::Op& acc,
                                  StageBuilderState& state,
                                  const sbe::value::SlotVector& aggSlots) {
    StringDataMap<std::unique_ptr<sbe::EExpression>> inputs;
    return buildFinalizeForWindowFunc(acc, std::move(inputs), state, aggSlots);
}

}  // namespace mongo::stage_builder
