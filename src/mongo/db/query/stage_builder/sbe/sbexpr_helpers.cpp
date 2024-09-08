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

#include "mongo/db/query/stage_builder/sbe/sbexpr_helpers.h"

#include "mongo/db/exec/sbe/stages/agg_project.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/sort.h"
#include "mongo/db/exec/sbe/stages/sorted_merge.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/stages/unique.h"
#include "mongo/db/exec/sbe/stages/unwind.h"
#include "mongo/db/query/stage_builder/sbe/abt_holder_impl.h"

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
    abtExprs.reserve(exprs.size());

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

sbe::EExpression::Vector SbExprBuilder::lower(SbExpr::Vector& sbExprs,
                                              const VariableTypes* varTypes) {
    // Convert the SbExpr vector to an EExpression vector.
    sbe::EExpression::Vector exprs;
    for (auto& e : sbExprs) {
        exprs.emplace_back(lower(e, varTypes));
    }

    return exprs;
}

sbe::value::SlotVector SbExprBuilder::lower(const SbSlotVector& sbSlots, const VariableTypes*) {
    sbe::value::SlotVector slotVec;
    slotVec.reserve(sbSlots.size());

    for (const auto& sbSlot : sbSlots) {
        slotVec.push_back(sbSlot.getId());
    }

    return slotVec;
}

std::vector<sbe::value::SlotVector> SbExprBuilder::lower(
    const std::vector<SbSlotVector>& sbSlotVectors, const VariableTypes* varTypes) {
    std::vector<sbe::value::SlotVector> slotVectors;
    slotVectors.reserve(sbSlotVectors.size());

    for (const auto& sbSlotVec : sbSlotVectors) {
        slotVectors.emplace_back(lower(sbSlotVec, varTypes));
    }

    return slotVectors;
}

sbe::SlotExprPairVector SbExprBuilder::lower(SbExprSbSlotVector& sbSlotSbExprVec,
                                             const VariableTypes* varTypes) {
    sbe::SlotExprPairVector slotExprVec;
    slotExprVec.reserve(sbSlotSbExprVec.size());

    for (auto& [sbExpr, sbSlot] : sbSlotSbExprVec) {
        slotExprVec.emplace_back(std::pair(sbSlot.getId(), sbExpr.extractExpr(_state, varTypes)));
    }

    return slotExprVec;
}

SbExpr SbExprBuilder::makeNot(SbExpr e) {
    if (hasABT(e)) {
        return abt::wrap(stage_builder::makeNot(extractABT(e)));
    } else {
        return stage_builder::makeNot(lower(e));
    }
}

SbExpr SbExprBuilder::makeUnaryOp(sbe::EPrimUnary::Op unaryOp, SbExpr e) {
    if (hasABT(e)) {
        return abt::wrap(stage_builder::makeUnaryOp(getOptimizerOp(unaryOp), extractABT(e)));
    } else {
        return stage_builder::makeUnaryOp(unaryOp, lower(e));
    }
}

SbExpr SbExprBuilder::makeUnaryOp(optimizer::Operations unaryOp, SbExpr e) {
    if (hasABT(e)) {
        return abt::wrap(stage_builder::makeUnaryOp(unaryOp, extractABT(e)));
    } else {
        return stage_builder::makeUnaryOp(getEPrimUnaryOp(unaryOp), lower(e));
    }
}

SbExpr SbExprBuilder::makeBinaryOp(sbe::EPrimBinary::Op binaryOp, SbExpr lhs, SbExpr rhs) {
    if (hasABT(lhs, rhs)) {
        return abt::wrap(stage_builder::makeBinaryOp(
            getOptimizerOp(binaryOp), extractABT(lhs), extractABT(rhs)));
    } else {
        return stage_builder::makeBinaryOp(binaryOp, lower(lhs), lower(rhs), _state);
    }
}

SbExpr SbExprBuilder::makeBinaryOp(optimizer::Operations binaryOp, SbExpr lhs, SbExpr rhs) {
    if (hasABT(lhs, rhs)) {
        return abt::wrap(stage_builder::makeBinaryOp(binaryOp, extractABT(lhs), extractABT(rhs)));
    } else {
        return stage_builder::makeBinaryOp(
            getEPrimBinaryOp(binaryOp), lower(lhs), lower(rhs), _state);
    }
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
        return stage_builder::makeFunction(name, lower(args));
    }
}

SbExpr SbExprBuilder::makeIf(SbExpr condExpr, SbExpr thenExpr, SbExpr elseExpr) {
    if (hasABT(condExpr, thenExpr, elseExpr)) {
        return abt::wrap(stage_builder::makeIf(
            extractABT(condExpr), extractABT(thenExpr), extractABT(elseExpr)));
    } else {
        return stage_builder::makeIf(lower(condExpr), lower(thenExpr), lower(elseExpr));
    }
}

SbExpr SbExprBuilder::makeLet(sbe::FrameId frameId, SbExpr::Vector binds, SbExpr expr) {
    if (hasABT(expr, binds)) {
        return abt::wrap(stage_builder::makeLet(frameId, extractABT(binds), extractABT(expr)));
    } else {
        return stage_builder::makeLet(frameId, lower(binds), lower(expr));
    }
}

SbExpr SbExprBuilder::makeLocalLambda(sbe::FrameId frameId, SbExpr expr) {
    if (hasABT(expr)) {
        return abt::wrap(stage_builder::makeLocalLambda(frameId, extractABT(expr)));
    } else {
        return stage_builder::makeLocalLambda(frameId, lower(expr));
    }
}

SbExpr SbExprBuilder::makeNumericConvert(SbExpr expr, sbe::value::TypeTags tag) {
    if (hasABT(expr)) {
        return abt::wrap(stage_builder::makeNumericConvert(extractABT(expr), tag));
    } else {
        return stage_builder::makeNumericConvert(lower(expr), tag);
    }
}

SbExpr SbExprBuilder::makeFail(ErrorCodes::Error error, StringData errorMessage) {
    return abt::wrap(stage_builder::makeABTFail(error, errorMessage));
}

SbExpr SbExprBuilder::makeFillEmpty(SbExpr expr, SbExpr altExpr) {
    if (hasABT(expr) && hasABT(altExpr)) {
        return abt::wrap(stage_builder::makeFillEmpty(extractABT(expr), extractABT(altExpr)));
    } else {
        return stage_builder::makeFillEmpty(lower(expr), lower(altExpr));
    }
}

SbExpr SbExprBuilder::makeFillEmptyFalse(SbExpr expr) {
    if (hasABT(expr)) {
        return abt::wrap(stage_builder::makeFillEmptyFalse(extractABT(expr)));
    } else {
        return stage_builder::makeFillEmptyFalse(lower(expr));
    }
}

SbExpr SbExprBuilder::makeFillEmptyTrue(SbExpr expr) {
    if (hasABT(expr)) {
        return abt::wrap(stage_builder::makeFillEmptyTrue(extractABT(expr)));
    } else {
        return stage_builder::makeFillEmptyTrue(lower(expr));
    }
}

SbExpr SbExprBuilder::makeFillEmptyNull(SbExpr expr) {
    if (hasABT(expr)) {
        return abt::wrap(stage_builder::makeFillEmptyNull(extractABT(expr)));
    } else {
        return stage_builder::makeFillEmptyNull(lower(expr));
    }
}

SbExpr SbExprBuilder::makeFillEmptyUndefined(SbExpr expr) {
    if (hasABT(expr)) {
        return abt::wrap(stage_builder::makeFillEmptyUndefined(extractABT(expr)));
    } else {
        return stage_builder::makeFillEmptyUndefined(lower(expr));
    }
}

SbExpr SbExprBuilder::makeIfNullExpr(SbExpr::Vector values) {
    if (hasABT(values)) {
        return abt::wrap(
            stage_builder::makeIfNullExpr(extractABT(values), _state.frameIdGenerator));
    } else {
        return stage_builder::makeIfNullExpr(lower(values), _state.frameIdGenerator);
    }
}

SbExpr SbExprBuilder::generateNullOrMissing(SbExpr expr) {
    if (hasABT(expr)) {
        return abt::wrap(stage_builder::generateABTNullOrMissing(extractABT(expr)));
    } else {
        return stage_builder::generateNullOrMissing(lower(expr));
    }
}

SbExpr SbExprBuilder::generateNullMissingOrUndefined(SbExpr expr) {
    if (hasABT(expr)) {
        return abt::wrap(stage_builder::generateABTNullMissingOrUndefined(extractABT(expr)));
    } else {
        return stage_builder::generateNullMissingOrUndefined(lower(expr));
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

sbe::WindowStage::Window SbBuilder::lower(SbWindow& sbWindow, const VariableTypes* varTypes) {
    sbe::WindowStage::Window window;

    window.windowExprSlots = lower(sbWindow.windowExprSlots, varTypes);
    window.frameFirstSlots = lower(sbWindow.frameFirstSlots, varTypes);
    window.frameLastSlots = lower(sbWindow.frameLastSlots, varTypes);
    window.initExprs = lower(sbWindow.initExprs, varTypes);
    window.addExprs = lower(sbWindow.addExprs, varTypes);
    window.removeExprs = lower(sbWindow.removeExprs, varTypes);
    window.lowBoundExpr = lower(sbWindow.lowBoundExpr, varTypes);
    window.highBoundExpr = lower(sbWindow.highBoundExpr, varTypes);

    return window;
}

std::vector<sbe::WindowStage::Window> SbBuilder::lower(std::vector<SbWindow>& sbWindows,
                                                       const VariableTypes* varTypes) {
    std::vector<sbe::WindowStage::Window> windows;
    windows.reserve(sbWindows.size());

    for (auto& sbWindow : sbWindows) {
        windows.emplace_back(lower(sbWindow, varTypes));
    }

    return windows;
}

std::tuple<SbStage, SbSlot, SbSlot, SbSlotVector> SbBuilder::makeScan(
    UUID collectionUuid,
    DatabaseName dbName,
    bool forward,
    boost::optional<SbSlot> seekSlot,
    std::vector<std::string> scanFieldNames,
    const SbScanBounds& scanBounds,
    const SbIndexInfoSlots& indexInfoSlots,
    sbe::ScanCallbacks scanCallbacks,
    boost::optional<SbSlot> oplogTsSlot,
    bool lowPriority) {
    auto resultSlot = SbSlot{_state.slotId()};
    auto recordIdSlot = SbSlot{_state.slotId()};

    SbSlotVector scanFieldSlots;
    scanFieldSlots.reserve(scanFieldNames.size());

    for (size_t i = 0; i < scanFieldNames.size(); ++i) {
        scanFieldSlots.emplace_back(SbSlot{_state.slotId()});
    }

    auto scanStage = sbe::makeS<sbe::ScanStage>(collectionUuid,
                                                std::move(dbName),
                                                lower(resultSlot),
                                                lower(recordIdSlot),
                                                lower(indexInfoSlots.snapshotIdSlot),
                                                lower(indexInfoSlots.indexIdentSlot),
                                                lower(indexInfoSlots.indexKeySlot),
                                                lower(indexInfoSlots.indexKeyPatternSlot),
                                                lower(oplogTsSlot),
                                                std::move(scanFieldNames),
                                                lower(scanFieldSlots),
                                                lower(seekSlot),
                                                lower(scanBounds.minRecordIdSlot),
                                                lower(scanBounds.maxRecordIdSlot),
                                                forward,
                                                _state.yieldPolicy,
                                                _nodeId,
                                                std::move(scanCallbacks),
                                                lowPriority,
                                                false /* useRandomCursor */,
                                                true /* participateInTrialRunTracking */,
                                                scanBounds.includeScanStartRecordId,
                                                scanBounds.includeScanEndRecordId);

    return {std::move(scanStage), resultSlot, recordIdSlot, std::move(scanFieldSlots)};
}

SbStage SbBuilder::makeLimit(const VariableTypes& varTypes, SbStage stage, SbExpr limitConstant) {
    return sbe::makeS<sbe::LimitSkipStage>(
        std::move(stage), lower(limitConstant, &varTypes), nullptr, _nodeId);
}

SbStage SbBuilder::makeLimitSkip(const VariableTypes& varTypes,
                                 SbStage stage,
                                 SbExpr limitConstant,
                                 SbExpr skipConstant) {
    return sbe::makeS<sbe::LimitSkipStage>(
        std::move(stage), lower(limitConstant, &varTypes), lower(skipConstant, &varTypes), _nodeId);
}

SbStage SbBuilder::makeCoScan() {
    return sbe::makeS<sbe::CoScanStage>(_nodeId);
}

SbStage SbBuilder::makeLimitOneCoScanTree() {
    return makeLimit(sbe::makeS<sbe::CoScanStage>(_nodeId), makeInt64Constant(1));
}

SbStage SbBuilder::makeFilter(const VariableTypes& varTypes, SbStage stage, SbExpr condition) {
    return sbe::makeS<sbe::FilterStage<false>>(
        std::move(stage), lower(condition, &varTypes), _nodeId);
}

SbStage SbBuilder::makeConstFilter(const VariableTypes& varTypes, SbStage stage, SbExpr condition) {
    return sbe::makeS<sbe::FilterStage<true>>(
        std::move(stage), lower(condition, &varTypes), _nodeId);
}

SbStage SbBuilder::makeLoopJoin(const VariableTypes& varTypes,
                                SbStage outer,
                                SbStage inner,
                                const SbSlotVector& outerProjects,
                                const SbSlotVector& outerCorrelated,
                                const SbSlotVector& innerProjects,
                                SbExpr predicate,
                                sbe::JoinType joinType) {
    return sbe::makeS<sbe::LoopJoinStage>(std::move(outer),
                                          std::move(inner),
                                          lower(outerProjects, &varTypes),
                                          lower(outerCorrelated, &varTypes),
                                          lower(innerProjects, &varTypes),
                                          lower(predicate, &varTypes),
                                          joinType,
                                          _nodeId);
}

std::pair<SbStage, SbSlotVector> SbBuilder::makeProject(const VariableTypes& varTypes,
                                                        SbStage stage,
                                                        SbExprOptSbSlotVector projects) {
    sbe::SlotExprPairVector slotExprPairs;
    SbSlotVector outSlots;

    for (auto& [expr, optSlot] : projects) {
        expr.optimize(_state, &varTypes);

        if (expr.isSlotExpr() && (!optSlot || expr.toSlot().getId() == optSlot->getId())) {
            // If 'expr' is an SbSlot -AND- if 'optSlot' is equal to either 'expr.toSlot()' or
            // boost::none, then we don't need to project anything and instead we can just store
            // 'expr.toSlot()' directly into 'outSlots'.
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

SbStage SbBuilder::makeUnique(const VariableTypes& varTypes, SbStage stage, SbSlot key) {
    sbe::value::SlotVector keySlots;
    keySlots.emplace_back(key.getId());

    return sbe::makeS<sbe::UniqueStage>(std::move(stage), std::move(keySlots), _nodeId);
}

SbStage SbBuilder::makeUnique(const VariableTypes& varTypes,
                              SbStage stage,
                              const SbSlotVector& keys) {
    return sbe::makeS<sbe::UniqueStage>(std::move(stage), lower(keys, &varTypes), _nodeId);
}

SbStage SbBuilder::makeSort(const VariableTypes& varTypes,
                            SbStage stage,
                            const SbSlotVector& orderBy,
                            std::vector<sbe::value::SortDirection> dirs,
                            const SbSlotVector& forwardedSlots,
                            SbExpr limitExpr,
                            size_t memoryLimit) {
    return sbe::makeS<sbe::SortStage>(std::move(stage),
                                      lower(orderBy, &varTypes),
                                      std::move(dirs),
                                      lower(forwardedSlots, &varTypes),
                                      lower(limitExpr, &varTypes),
                                      memoryLimit,
                                      _state.allowDiskUse,
                                      _state.yieldPolicy,
                                      _nodeId);
}

std::tuple<SbStage, SbSlotVector, SbSlotVector> SbBuilder::makeHashAgg(
    const VariableTypes& varTypes,
    SbStage stage,
    const SbSlotVector& gbs,
    SbAggExprVector sbAggExprs,
    boost::optional<sbe::value::SlotId> collatorSlot,
    SbExprSbSlotVector mergingExprs) {
    // In debug builds or when we explicitly set the query knob, we artificially force frequent
    // spilling. This makes sure that our tests exercise the spilling algorithm and the associated
    // logic for merging partial aggregates which otherwise would require large data sizes to
    // exercise.
    const bool forceIncreasedSpilling = _state.allowDiskUse &&
        (kDebugBuild || internalQuerySlotBasedExecutionHashAggForceIncreasedSpilling.load());

    // For normal (non-block) HashAggStage, the group by "out" slots are the same as the incoming
    // group by slots.
    SbSlotVector groupByOutSlots = gbs;

    // Copy unique slot IDs from 'gbs' to 'groupBySlots'.
    sbe::value::SlotVector groupBySlots;
    absl::flat_hash_set<sbe::value::SlotId> dedup;

    for (const auto& sbSlot : gbs) {
        auto slotId = sbSlot.getId();

        if (dedup.insert(slotId).second) {
            groupBySlots.emplace_back(slotId);
        }
    }

    sbe::AggExprVector aggExprsVec;
    SbSlotVector aggOutSlots;
    for (auto& [sbAggExpr, optSbSlot] : sbAggExprs) {
        auto sbSlot = optSbSlot ? *optSbSlot : SbSlot{_state.slotId()};
        aggOutSlots.emplace_back(sbSlot);

        auto exprPair = sbe::AggExprPair{sbAggExpr.init.extractExpr(_state, &varTypes),
                                         sbAggExpr.agg.extractExpr(_state, &varTypes)};

        aggExprsVec.emplace_back(std::pair(sbSlot.getId(), std::move(exprPair)));
    }

    sbe::SlotExprPairVector mergingExprsVec = lower(mergingExprs);

    stage = sbe::makeS<sbe::HashAggStage>(std::move(stage),
                                          std::move(groupBySlots),
                                          std::move(aggExprsVec),
                                          sbe::value::SlotVector{},
                                          true /* optimized close */,
                                          collatorSlot,
                                          _state.allowDiskUse,
                                          std::move(mergingExprsVec),
                                          _state.yieldPolicy,
                                          _nodeId,
                                          true /* participateInTrialRunTracking */,
                                          forceIncreasedSpilling);

    return {std::move(stage), std::move(groupByOutSlots), std::move(aggOutSlots)};
}

std::tuple<SbStage, SbSlotVector, SbSlotVector> SbBuilder::makeBlockHashAgg(
    const VariableTypes& varTypes,
    SbStage stage,
    const SbSlotVector& gbs,
    SbAggExprVector sbAggExprs,
    SbSlot selectivityBitmapSlot,
    const SbSlotVector& blockAccArgSbSlots,
    SbSlot bitmapInternalSlot,
    const SbSlotVector& accumulatorDataSbSlots,
    SbExprSbSlotVector mergingExprs) {
    tassert(8448607, "Expected at least one group by slot to be provided", gbs.size() > 0);

    const auto selectivityBitmapSlotId = selectivityBitmapSlot.getId();

    sbe::AggExprTupleVector aggs;
    SbSlotVector aggOutSlots;

    for (auto& [sbAggExpr, optSbSlot] : sbAggExprs) {
        auto sbSlot = optSbSlot ? *optSbSlot : SbSlot{_state.slotId()};
        sbSlot.setTypeSignature(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType));

        aggOutSlots.emplace_back(sbSlot);

        std::unique_ptr<sbe::EExpression> init, blockAgg, agg;
        if (sbAggExpr.init) {
            init = sbAggExpr.init.extractExpr(_state, &varTypes);
        }
        if (sbAggExpr.blockAgg) {
            blockAgg = sbAggExpr.blockAgg.extractExpr(_state, &varTypes);
        }
        agg = sbAggExpr.agg.extractExpr(_state, &varTypes);

        aggs.emplace_back(sbSlot.getId(),
                          sbe::AggExprTuple{std::move(init), std::move(blockAgg), std::move(agg)});
    }

    // Copy unique slot IDs from 'gbs' to 'groupBySlots'.
    sbe::value::SlotVector groupBySlots;
    absl::flat_hash_set<sbe::value::SlotId> dedupedGbs;

    for (const auto& sbSlot : gbs) {
        auto slotId = sbSlot.getId();

        if (dedupedGbs.insert(slotId).second) {
            groupBySlots.emplace_back(slotId);
        }
    }

    sbe::value::SlotVector blockAccArgSlots = lower(blockAccArgSbSlots);
    sbe::value::SlotVector accumulatorDataSlots = lower(accumulatorDataSbSlots);
    sbe::SlotExprPairVector mergingExprsVec = lower(mergingExprs);

    const bool forceIncreasedSpilling = _state.allowDiskUse &&
        (kDebugBuild || internalQuerySlotBasedExecutionHashAggForceIncreasedSpilling.load());

    stage = sbe::makeS<sbe::BlockHashAggStage>(std::move(stage),
                                               std::move(groupBySlots),
                                               selectivityBitmapSlotId,
                                               std::move(blockAccArgSlots),
                                               std::move(accumulatorDataSlots),
                                               bitmapInternalSlot.getId(),
                                               std::move(aggs),
                                               _state.allowDiskUse,
                                               std::move(mergingExprsVec),
                                               _state.yieldPolicy,
                                               _nodeId,
                                               true /* participateInTrialRunTracking */,
                                               forceIncreasedSpilling);

    // For BlockHashAggStage, the group by "out" slots are the same as the incoming group by slots,
    // except that each "out" slot will always be a block even if the corresponding incoming group
    // by slot was scalar.
    SbSlotVector groupByOutSlots;
    for (size_t i = 0; i < gbs.size(); ++i) {
        auto slotId = gbs[i].getId();
        auto inputSig = gbs[i].getTypeSignature().value_or(TypeSignature::kAnyScalarType);
        auto outputSig = TypeSignature::kBlockType.include(inputSig);

        groupByOutSlots.push_back(SbSlot(slotId, outputSig));
    }

    return {std::move(stage), std::move(groupByOutSlots), std::move(aggOutSlots)};
}

std::tuple<SbStage, SbSlotVector> SbBuilder::makeAggProject(const VariableTypes& varTypes,
                                                            SbStage stage,
                                                            SbAggExprVector sbAggExprs) {
    sbe::AggExprVector aggExprsVec;
    SbSlotVector aggOutSlots;

    for (auto& [sbAggExpr, optSbSlot] : sbAggExprs) {
        auto sbSlot = optSbSlot ? *optSbSlot : SbSlot{_state.slotId()};
        aggOutSlots.emplace_back(sbSlot);

        auto exprPair = sbe::AggExprPair{sbAggExpr.init.extractExpr(_state, &varTypes),
                                         sbAggExpr.agg.extractExpr(_state, &varTypes)};

        aggExprsVec.emplace_back(std::pair(sbSlot.getId(), std::move(exprPair)));
    }

    stage = sbe::makeS<sbe::AggProjectStage>(std::move(stage), std::move(aggExprsVec), _nodeId);

    return {std::move(stage), std::move(aggOutSlots)};
}

SbStage SbBuilder::makeWindow(const VariableTypes& varTypes,
                              SbStage stage,
                              const SbSlotVector& currSlots,
                              const SbSlotVector& boundTestingSlots,
                              size_t partitionSlotCount,
                              std::vector<SbWindow> windows,
                              boost::optional<sbe::value::SlotId> collatorSlot) {
    return sbe::makeS<sbe::WindowStage>(std::move(stage),
                                        lower(currSlots, &varTypes),
                                        lower(boundTestingSlots, &varTypes),
                                        partitionSlotCount,
                                        lower(windows, &varTypes),
                                        collatorSlot,
                                        _state.allowDiskUse,
                                        _nodeId);
}

std::tuple<SbStage, SbSlot, SbSlot> SbBuilder::makeUnwind(SbStage stage,
                                                          SbSlot inputSlot,
                                                          bool preserveNullAndEmptyArrays) {
    auto unwindOutputSlot = SbSlot{_state.slotId()};
    auto indexOutputSlot = SbSlot{_state.slotId()};

    stage = sbe::makeS<sbe::UnwindStage>(std::move(stage),
                                         inputSlot.getId(),
                                         unwindOutputSlot.getId(),
                                         indexOutputSlot.getId(),
                                         preserveNullAndEmptyArrays,
                                         _nodeId);

    return {std::move(stage), unwindOutputSlot, indexOutputSlot};
}

std::pair<SbStage, SbSlotVector> SbBuilder::makeUnion(sbe::PlanStage::Vector stages,
                                                      const std::vector<SbSlotVector>& slots) {
    tassert(9380400,
            "Expected the same number of stages and input slot vectors",
            stages.size() == slots.size());

    SbSlotVector outSlots = allocateOutSlotsForMergeStage(slots);

    auto unionStage =
        sbe::makeS<sbe::UnionStage>(std::move(stages), lower(slots), lower(outSlots), _nodeId);

    return {std::move(unionStage), std::move(outSlots)};
}

std::pair<SbStage, SbSlotVector> SbBuilder::makeSortedMerge(
    sbe::PlanStage::Vector stages,
    const std::vector<SbSlotVector>& slots,
    const std::vector<SbSlotVector>& keys,
    std::vector<sbe::value::SortDirection> dirs) {
    tassert(9380401,
            "Expected the same number of stages and input slot vectors",
            stages.size() == slots.size());

    SbSlotVector outSlots = allocateOutSlotsForMergeStage(slots);

    auto sortedMergeStage = sbe::makeS<sbe::SortedMergeStage>(
        std::move(stages), lower(keys), std::move(dirs), lower(slots), lower(outSlots), _nodeId);

    return {std::move(sortedMergeStage), std::move(outSlots)};
}

SbSlotVector SbBuilder::allocateOutSlotsForMergeStage(const std::vector<SbSlotVector>& slots) {
    tassert(9380402, "Expected at least one input stage", !slots.empty());

    const size_t n = slots[0].size();
    for (size_t i = 1; i < slots.size(); ++i) {
        tassert(
            9380403, "Expected all input slot vectors to be the same size", slots[i].size() == n);
    }

    SbSlotVector outSlots;
    outSlots.reserve(n);

    for (size_t j = 0; j < n; ++j) {
        // Get the type signatures of the jth element from each input slot vector and compute
        // the union of these type signatures.
        boost::optional<TypeSignature> unionTypeSig = slots[0][j].getTypeSignature();

        for (size_t i = 1; i < slots.size() && unionTypeSig; ++i) {
            auto typeSig = slots[i][j].getTypeSignature();
            if (typeSig) {
                unionTypeSig = unionTypeSig->include(*typeSig);
            } else {
                unionTypeSig = boost::none;
            }
        }

        // Allocate a new slot ID and add it to 'outSlots', using 'unionTypeSig' for the
        // type signature.
        outSlots.emplace_back(SbSlot{_state.slotId(), unionTypeSig});
    }

    return outSlots;
}
}  // namespace mongo::stage_builder
