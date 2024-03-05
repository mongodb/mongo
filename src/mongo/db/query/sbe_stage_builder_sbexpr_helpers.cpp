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

#include "mongo/db/query/sbe_stage_builder_sbexpr_helpers.h"

#include "mongo/db/query/sbe_stage_builder_abt_holder_impl.h"

namespace mongo::stage_builder {
namespace {
inline bool hasABT(const SbExpr& e) {
    return !e.isEExpr() && e.canExtractABT();
}

inline bool hasABT(const SbExpr::Vector& exprs) {
    return std::all_of(exprs.begin(), exprs.end(), [](auto&& e) { return hasABT(e); });
}

template <typename... Ts>
inline bool hasABT(const SbExpr& head, Ts&&... rest) {
    return hasABT(head) && hasABT(std::forward<Ts>(rest)...);
}

template <typename... Ts>
inline bool hasABT(const SbExpr::Vector& head, Ts&&... rest) {
    return hasABT(head) && hasABT(std::forward<Ts>(rest)...);
}

inline optimizer::ABT extractABT(SbExpr& e) {
    return abt::unwrap(e.extractABT());
}

inline optimizer::ABTVector extractABT(SbExpr::Vector& exprs) {
    // Convert the SbExpr vector to an ABT vector.
    optimizer::ABTVector abtExprs;
    for (auto& e : exprs) {
        abtExprs.emplace_back(extractABT(e));
    }

    return abtExprs;
}

inline optimizer::Operations getOptimizerOp(sbe::EPrimUnary::Op op) {
    switch (op) {
        case sbe::EPrimUnary::negate:
            return optimizer::Operations::Neg;
        case sbe::EPrimUnary::logicNot:
            return optimizer::Operations::Not;
        default:
            MONGO_UNREACHABLE;
    }
}

inline optimizer::Operations getOptimizerOp(sbe::EPrimBinary::Op op) {
    switch (op) {
        case sbe::EPrimBinary::eq:
            return optimizer::Operations::Eq;
        case sbe::EPrimBinary::neq:
            return optimizer::Operations::Neq;
        case sbe::EPrimBinary::greater:
            return optimizer::Operations::Gt;
        case sbe::EPrimBinary::greaterEq:
            return optimizer::Operations::Gte;
        case sbe::EPrimBinary::less:
            return optimizer::Operations::Lt;
        case sbe::EPrimBinary::lessEq:
            return optimizer::Operations::Lte;
        case sbe::EPrimBinary::add:
            return optimizer::Operations::Add;
        case sbe::EPrimBinary::sub:
            return optimizer::Operations::Sub;
        case sbe::EPrimBinary::fillEmpty:
            return optimizer::Operations::FillEmpty;
        case sbe::EPrimBinary::logicAnd:
            return optimizer::Operations::And;
        case sbe::EPrimBinary::logicOr:
            return optimizer::Operations::Or;
        case sbe::EPrimBinary::cmp3w:
            return optimizer::Operations::Cmp3w;
        case sbe::EPrimBinary::div:
            return optimizer::Operations::Div;
        case sbe::EPrimBinary::mul:
            return optimizer::Operations::Mult;
        default:
            MONGO_UNREACHABLE;
    }
}
}  // namespace

std::unique_ptr<sbe::EExpression> SbExprBuilder::extractExpr(SbExpr& e) {
    return e.extractExpr(_state);
}

sbe::EExpression::Vector SbExprBuilder::extractExpr(SbExpr::Vector& sbExprs) {
    // Convert the SbExpr vector to an EExpression vector.
    sbe::EExpression::Vector exprs;
    for (auto& e : sbExprs) {
        exprs.emplace_back(extractExpr(e));
    }

    return exprs;
}

SbExpr SbExprBuilder::makeNot(SbExpr e) {
    if (hasABT(e)) {
        return abt::wrap(stage_builder::makeNot(extractABT(e)));
    } else {
        return stage_builder::makeNot(extractExpr(e));
    }
}

SbExpr SbExprBuilder::makeUnaryOp(sbe::EPrimUnary::Op unaryOp, SbExpr e) {
    if (hasABT(e)) {
        return abt::wrap(stage_builder::makeUnaryOp(getOptimizerOp(unaryOp), extractABT(e)));
    } else {
        return stage_builder::makeUnaryOp(unaryOp, extractExpr(e));
    }
}

SbExpr SbExprBuilder::makeUnaryOp(optimizer::Operations unaryOp, SbExpr e) {
    if (hasABT(e)) {
        return abt::wrap(stage_builder::makeUnaryOp(unaryOp, extractABT(e)));
    } else {
        return stage_builder::makeUnaryOp(getEPrimUnaryOp(unaryOp), extractExpr(e));
    }
}

SbExpr SbExprBuilder::makeBinaryOp(sbe::EPrimBinary::Op binaryOp, SbExpr lhs, SbExpr rhs) {
    if (hasABT(lhs, rhs)) {
        return abt::wrap(stage_builder::makeBinaryOp(
            getOptimizerOp(binaryOp), extractABT(lhs), extractABT(rhs)));
    } else {
        return stage_builder::makeBinaryOp(binaryOp, extractExpr(lhs), extractExpr(rhs));
    }
}

SbExpr SbExprBuilder::makeBinaryOp(optimizer::Operations binaryOp, SbExpr lhs, SbExpr rhs) {
    if (hasABT(lhs, rhs)) {
        return abt::wrap(stage_builder::makeBinaryOp(binaryOp, extractABT(lhs), extractABT(rhs)));
    } else {
        return stage_builder::makeBinaryOp(
            getEPrimBinaryOp(binaryOp), extractExpr(lhs), extractExpr(rhs));
    }
}

SbExpr SbExprBuilder::makeBinaryOpWithCollation(sbe::EPrimBinary::Op binaryOp,
                                                SbExpr lhs,
                                                SbExpr rhs) {
    auto collatorSlot = _state.getCollatorSlot();
    if (!collatorSlot) {
        return makeBinaryOp(binaryOp, std::move(lhs), std::move(rhs));
    }

    return sbe::makeE<sbe::EPrimBinary>(
        binaryOp, extractExpr(lhs), extractExpr(rhs), sbe::makeE<sbe::EVariable>(*collatorSlot));
}

SbExpr SbExprBuilder::makeBinaryOpWithCollation(optimizer::Operations binaryOp,
                                                SbExpr lhs,
                                                SbExpr rhs) {
    auto collatorSlot = _state.getCollatorSlot();
    if (!collatorSlot) {
        return makeBinaryOp(binaryOp, std::move(lhs), std::move(rhs));
    }

    return sbe::makeE<sbe::EPrimBinary>(getEPrimBinaryOp(binaryOp),
                                        extractExpr(lhs),
                                        extractExpr(rhs),
                                        sbe::makeE<sbe::EVariable>(*collatorSlot));
}

SbExpr SbExprBuilder::makeConstant(sbe::value::TypeTags tag, sbe::value::Value val) {
    return abt::wrap(optimizer::make<optimizer::Constant>(tag, val));
}

SbExpr SbExprBuilder::makeNothingConstant() {
    return abt::wrap(optimizer::Constant::nothing());
}

SbExpr SbExprBuilder::makeNullConstant() {
    return abt::wrap(optimizer::Constant::null());
}

SbExpr SbExprBuilder::makeBoolConstant(bool boolVal) {
    return abt::wrap(optimizer::Constant::boolean(boolVal));
}

SbExpr SbExprBuilder::makeInt32Constant(int32_t num) {
    return abt::wrap(optimizer::Constant::int32(num));
}

SbExpr SbExprBuilder::makeInt64Constant(int64_t num) {
    return abt::wrap(optimizer::Constant::int64(num));
}

SbExpr SbExprBuilder::makeDoubleConstant(double num) {
    return abt::wrap(optimizer::Constant::fromDouble(num));
}

SbExpr SbExprBuilder::makeDecimalConstant(const Decimal128& num) {
    return abt::wrap(optimizer::Constant::fromDecimal(num));
}

SbExpr SbExprBuilder::makeStrConstant(StringData str) {
    return abt::wrap(optimizer::Constant::str(str));
}

SbExpr SbExprBuilder::makeFunction(StringData name, SbExpr::Vector args) {
    if (hasABT(args)) {
        return abt::wrap(stage_builder::makeABTFunction(name, extractABT(args)));
    } else {
        return stage_builder::makeFunction(name, extractExpr(args));
    }
}

SbExpr SbExprBuilder::makeIf(SbExpr condExpr, SbExpr thenExpr, SbExpr elseExpr) {
    if (hasABT(condExpr, thenExpr, elseExpr)) {
        return abt::wrap(stage_builder::makeIf(
            extractABT(condExpr), extractABT(thenExpr), extractABT(elseExpr)));
    } else {
        return stage_builder::makeIf(
            extractExpr(condExpr), extractExpr(thenExpr), extractExpr(elseExpr));
    }
}

SbExpr SbExprBuilder::makeLet(sbe::FrameId frameId, SbExpr::Vector binds, SbExpr expr) {
    if (hasABT(expr, binds)) {
        return abt::wrap(stage_builder::makeLet(frameId, extractABT(binds), extractABT(expr)));
    } else {
        return stage_builder::makeLet(frameId, extractExpr(binds), extractExpr(expr));
    }
}

SbExpr SbExprBuilder::makeLocalLambda(sbe::FrameId frameId, SbExpr expr) {
    if (hasABT(expr)) {
        return abt::wrap(stage_builder::makeLocalLambda(frameId, extractABT(expr)));
    } else {
        return stage_builder::makeLocalLambda(frameId, extractExpr(expr));
    }
}

SbExpr SbExprBuilder::makeNumericConvert(SbExpr expr, sbe::value::TypeTags tag) {
    if (hasABT(expr)) {
        return abt::wrap(stage_builder::makeNumericConvert(extractABT(expr), tag));
    } else {
        return stage_builder::makeNumericConvert(extractExpr(expr), tag);
    }
}

SbExpr SbExprBuilder::makeFail(ErrorCodes::Error error, StringData errorMessage) {
    return abt::wrap(stage_builder::makeABTFail(error, errorMessage));
}

SbExpr SbExprBuilder::makeFillEmptyFalse(SbExpr expr) {
    if (hasABT(expr)) {
        return abt::wrap(stage_builder::makeFillEmptyFalse(extractABT(expr)));
    } else {
        return stage_builder::makeFillEmptyFalse(extractExpr(expr));
    }
}

SbExpr SbExprBuilder::makeFillEmptyTrue(SbExpr expr) {
    if (hasABT(expr)) {
        return abt::wrap(stage_builder::makeFillEmptyTrue(extractABT(expr)));
    } else {
        return stage_builder::makeFillEmptyTrue(extractExpr(expr));
    }
}

SbExpr SbExprBuilder::makeFillEmptyNull(SbExpr expr) {
    if (hasABT(expr)) {
        return abt::wrap(stage_builder::makeFillEmptyNull(extractABT(expr)));
    } else {
        return stage_builder::makeFillEmptyNull(extractExpr(expr));
    }
}

SbExpr SbExprBuilder::makeFillEmptyUndefined(SbExpr expr) {
    if (hasABT(expr)) {
        return abt::wrap(stage_builder::makeFillEmptyUndefined(extractABT(expr)));
    } else {
        return stage_builder::makeFillEmptyUndefined(extractExpr(expr));
    }
}

SbExpr SbExprBuilder::makeIfNullExpr(SbExpr::Vector values) {
    if (hasABT(values)) {
        return abt::wrap(
            stage_builder::makeIfNullExpr(extractABT(values), _state.frameIdGenerator));
    } else {
        return stage_builder::makeIfNullExpr(extractExpr(values), _state.frameIdGenerator);
    }
}

SbExpr SbExprBuilder::generateNullOrMissing(SbExpr expr) {
    if (hasABT(expr)) {
        return abt::wrap(stage_builder::generateABTNullOrMissing(extractABT(expr)));
    } else {
        return stage_builder::generateNullOrMissing(extractExpr(expr));
    }
}

SbExpr SbExprBuilder::generateNullMissingOrUndefined(SbExpr expr) {
    if (hasABT(expr)) {
        return abt::wrap(stage_builder::generateABTNullMissingOrUndefined(extractABT(expr)));
    } else {
        return stage_builder::generateNullMissingOrUndefined(extractExpr(expr));
    }
}


SbExpr SbExprBuilder::generatePositiveCheck(SbExpr expr) {
    return abt::wrap(stage_builder::generateABTPositiveCheck(extractABT(expr)));
}

SbExpr SbExprBuilder::generateNullOrMissing(SbVar var) {
    return abt::wrap(stage_builder::generateABTNullOrMissing(var.getABTName()));
}

SbExpr SbExprBuilder::generateNullMissingOrUndefined(SbVar var) {
    return abt::wrap(stage_builder::generateABTNullMissingOrUndefined(var.getABTName()));
}

SbExpr SbExprBuilder::generateNonStringCheck(SbVar var) {
    return abt::wrap(stage_builder::generateABTNonStringCheck(var.getABTName()));
}

SbExpr SbExprBuilder::generateNonTimestampCheck(SbVar var) {
    return abt::wrap(stage_builder::generateABTNonTimestampCheck(var.getABTName()));
}

SbExpr SbExprBuilder::generateNegativeCheck(SbVar var) {
    return abt::wrap(stage_builder::generateABTNegativeCheck(var.getABTName()));
}

SbExpr SbExprBuilder::generateNonPositiveCheck(SbVar var) {
    return abt::wrap(stage_builder::generateABTNonPositiveCheck(var.getABTName()));
}

SbExpr SbExprBuilder::generateNonNumericCheck(SbVar var) {
    return abt::wrap(stage_builder::generateABTNonNumericCheck(var.getABTName()));
}

SbExpr SbExprBuilder::generateLongLongMinCheck(SbVar var) {
    return abt::wrap(stage_builder::generateABTLongLongMinCheck(var.getABTName()));
}

SbExpr SbExprBuilder::generateNonArrayCheck(SbVar var) {
    return abt::wrap(stage_builder::generateABTNonArrayCheck(var.getABTName()));
}

SbExpr SbExprBuilder::generateNonObjectCheck(SbVar var) {
    return abt::wrap(stage_builder::generateABTNonObjectCheck(var.getABTName()));
}

SbExpr SbExprBuilder::generateNullishOrNotRepresentableInt32Check(SbVar var) {
    return abt::wrap(
        stage_builder::generateABTNullishOrNotRepresentableInt32Check(var.getABTName()));
}

SbExpr SbExprBuilder::generateNaNCheck(SbVar var) {
    return abt::wrap(stage_builder::generateABTNaNCheck(var.getABTName()));
}

SbExpr SbExprBuilder::generateInfinityCheck(SbVar var) {
    return abt::wrap(stage_builder::generateABTInfinityCheck(var.getABTName()));
}

SbExpr SbExprBuilder::generateInvalidRoundPlaceArgCheck(SbVar var) {
    return abt::wrap(stage_builder::generateInvalidRoundPlaceArgCheck(var.getABTName()));
}

std::pair<SbStage, SbSlotVector> SbBuilder::makeProject(SbStage stage,
                                                        const VariableTypes* varTypes,
                                                        SbExprOptSbSlotVector projects) {
    sbe::SlotExprPairVector slotExprPairs;
    SbSlotVector outSlots;

    for (auto& [expr, optSlot] : projects) {
        expr.optimize(_state, varTypes);

        if (!optSlot && expr.isSlotExpr()) {
            // If 'optSlot' is not set and 'expr' is an SbSlot, then we don't need to
            // project anything and instead we can just store 'expr.toSlot()' directly
            // into 'outSlots'.
            outSlots.emplace_back(expr.toSlot());
        } else {
            // Otherwise, allocate a slot if needed, add a project to 'slotExprPairs' for this
            // update, and then store the SbSlot (annotated with the type signature from 'expr')
            // into 'outSlots'.
            sbe::value::SlotId slot = optSlot ? optSlot->getId() : _state.slotId();
            outSlots.emplace_back(slot, expr.getTypeSignature());
            slotExprPairs.emplace_back(slot, expr.extractExpr(_state));
        }
    }

    if (!slotExprPairs.empty()) {
        return {sbe::makeS<sbe::ProjectStage>(std::move(stage), std::move(slotExprPairs), _nodeId),
                std::move(outSlots)};
    }

    return {std::move(stage), std::move(outSlots)};
}

std::tuple<SbStage, SbSlotVector, SbSlotVector> SbBuilder::makeHashAgg(
    SbStage stage,
    const VariableTypes* varTypes,
    const SbSlotVector& gbs,
    SbAggExprVector sbAggExprs,
    boost::optional<sbe::value::SlotId> collatorSlot,
    bool allowDiskUse,
    SbExprSbSlotVector mergingExprs,
    PlanYieldPolicy* yieldPolicy) {
    // In debug builds or when we explicitly set the query knob, we artificially force frequent
    // spilling. This makes sure that our tests exercise the spilling algorithm and the associated
    // logic for merging partial aggregates which otherwise would require large data sizes to
    // exercise.
    const bool forceIncreasedSpilling = allowDiskUse &&
        (kDebugBuild || internalQuerySlotBasedExecutionHashAggForceIncreasedSpilling.load());

    // For normal (non-block) HashAggStage, the group by "out" slots are the same as the incoming
    // group by slots.
    SbSlotVector groupByOutSlots = gbs;

    // Copy unique slot IDs from 'gbs' to 'gbsVec'.
    sbe::value::SlotVector gbsVec;
    absl::flat_hash_set<sbe::value::SlotId> dedup;

    for (const auto& sbSlot : gbs) {
        auto slotId = sbSlot.getId();

        if (dedup.insert(slotId).second) {
            gbsVec.emplace_back(slotId);
        }
    }

    sbe::AggExprVector aggExprsVec;
    SbSlotVector aggSlots;
    for (auto& [sbAggExpr, optSbSlot] : sbAggExprs) {
        auto sbSlot = optSbSlot ? *optSbSlot : SbSlot{_state.slotId()};
        aggSlots.emplace_back(sbSlot);

        auto exprPair = sbe::AggExprPair{sbAggExpr.init.extractExpr(_state, varTypes),
                                         sbAggExpr.agg.extractExpr(_state, varTypes)};

        aggExprsVec.emplace_back(std::pair(sbSlot.getId(), std::move(exprPair)));
    }

    sbe::SlotExprPairVector mergingExprsVec;
    for (auto& [sbExpr, sbSlot] : mergingExprs) {
        mergingExprsVec.emplace_back(
            std::pair(sbSlot.getId(), sbExpr.extractExpr(_state, varTypes)));
    }

    stage = sbe::makeS<sbe::HashAggStage>(std::move(stage),
                                          std::move(gbsVec),
                                          std::move(aggExprsVec),
                                          sbe::makeSV(),
                                          true /* optimized close */,
                                          collatorSlot,
                                          allowDiskUse,
                                          std::move(mergingExprsVec),
                                          yieldPolicy,
                                          _nodeId,
                                          true /* participateInTrialRunTracking */,
                                          forceIncreasedSpilling);

    return {std::move(stage), std::move(groupByOutSlots), std::move(aggSlots)};
}

std::tuple<SbStage, SbSlotVector, SbSlotVector> SbBuilder::makeBlockHashAgg(
    SbStage stage,
    const VariableTypes* varTypes,
    const SbSlotVector& gbs,
    SbAggExprVector sbAggExprs,
    boost::optional<SbSlot> selectivityBitmapSlot,
    const SbSlotVector& blockAccArgSbSlots,
    const SbSlotVector& blockAccInternalArgSbSlots,
    SbSlot bitmapInternalSlot,
    SbSlot accInternalSlot,
    bool allowDiskUse,
    PlanYieldPolicy* yieldPolicy) {
    using BlockAggExprPair = sbe::BlockHashAggStage::BlockRowAccumulators;

    tassert(8448607, "Expected exactly one group by slot to be provided", gbs.size() == 1);

    SbSlot groupBySlot = gbs[0];

    const auto selectivityBitmapSlotId =
        selectivityBitmapSlot ? boost::make_optional(selectivityBitmapSlot->getId()) : boost::none;

    sbe::BlockHashAggStage::BlockAndRowAggs aggExprsMap;
    SbSlotVector aggSlots;

    for (auto& [sbAggExpr, optSbSlot] : sbAggExprs) {
        auto sbSlot = optSbSlot ? *optSbSlot : SbSlot{_state.slotId()};
        sbSlot.setTypeSignature(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType));

        aggSlots.emplace_back(sbSlot);

        auto exprPair = BlockAggExprPair{sbAggExpr.blockAgg.extractExpr(_state, varTypes),
                                         sbAggExpr.agg.extractExpr(_state, varTypes)};
        aggExprsMap.emplace(sbSlot.getId(), std::move(exprPair));
    }

    sbe::value::SlotVector groupBySlots;
    groupBySlots.push_back(groupBySlot.getId());

    sbe::value::SlotVector blockAccArgSlots;
    sbe::value::SlotVector blockAccInternalArgSlots;

    for (const auto& sbSlot : blockAccArgSbSlots) {
        blockAccArgSlots.push_back(sbSlot.getId());
    }
    for (const auto& sbSlot : blockAccInternalArgSbSlots) {
        blockAccInternalArgSlots.push_back(sbSlot.getId());
    }

    const bool forceIncreasedSpilling = allowDiskUse &&
        (kDebugBuild || internalQuerySlotBasedExecutionHashAggForceIncreasedSpilling.load());
    stage = sbe::makeS<sbe::BlockHashAggStage>(std::move(stage),
                                               groupBySlots,
                                               selectivityBitmapSlotId,
                                               std::move(blockAccArgSlots),
                                               accInternalSlot.getId(),
                                               bitmapInternalSlot.getId(),
                                               std::move(blockAccInternalArgSlots),
                                               std::move(aggExprsMap),
                                               yieldPolicy,
                                               _nodeId,
                                               allowDiskUse,
                                               true /* participateInTrialRunTracking */,
                                               forceIncreasedSpilling);

    // For BlockHashAggStage, the group by "out" slot is the same as the incoming group by slot,
    // except that the "out" slot will always be a block even if the incoming group by slot was
    // scalar.
    const auto groupByOutSlotType = TypeSignature::kBlockType.include(
        groupBySlot.getTypeSignature().value_or(TypeSignature::kAnyScalarType));

    auto groupByOutSlot = groupBySlot;
    groupByOutSlot.setTypeSignature(groupByOutSlotType);

    SbSlotVector groupByOutSlots;
    groupByOutSlots.emplace_back(groupByOutSlot);

    return {std::move(stage), std::move(groupByOutSlots), std::move(aggSlots)};
}
}  // namespace mongo::stage_builder
