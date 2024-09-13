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

#pragma once

#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/query/stage_builder/sbe/gen_abt_helpers.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr.h"

namespace mongo::stage_builder {
namespace {
inline void makeSbExprOptSbSlotVecHelper(SbExprOptSbSlotVector& result) {}

template <typename... Ts>
inline void makeSbExprOptSbSlotVecHelper(SbExprOptSbSlotVector& result, SbExpr expr, Ts&&... rest) {
    result.emplace_back(std::move(expr), boost::none);
    makeSbExprOptSbSlotVecHelper(result, std::forward<Ts>(rest)...);
}

template <typename... Ts>
inline void makeSbExprOptSbSlotVecHelper(SbExprOptSbSlotVector& result,
                                         std::pair<SbExpr, sbe::value::SlotId> p,
                                         Ts&&... rest) {
    result.emplace_back(std::move(p.first), boost::make_optional(SbSlot{p.second}));
    makeSbExprOptSbSlotVecHelper(result, std::forward<Ts>(rest)...);
}

template <typename... Ts>
inline void makeSbExprOptSbSlotVecHelper(SbExprOptSbSlotVector& result,
                                         std::pair<SbExpr, SbSlot> p,
                                         Ts&&... rest) {
    result.emplace_back(std::move(p.first), boost::make_optional(p.second));
    makeSbExprOptSbSlotVecHelper(result, std::forward<Ts>(rest)...);
}

template <typename... Ts>
inline void makeSbExprOptSbSlotVecHelper(SbExprOptSbSlotVector& result,
                                         std::pair<SbExpr, boost::optional<sbe::value::SlotId>> p,
                                         Ts&&... rest) {
    result.emplace_back(std::move(p.first),
                        p.second ? boost::make_optional(SbSlot{*p.second}) : boost::none);
    makeSbExprOptSbSlotVecHelper(result, std::forward<Ts>(rest)...);
}

template <typename... Ts>
inline void makeSbExprOptSbSlotVecHelper(SbExprOptSbSlotVector& result,
                                         std::pair<SbExpr, boost::optional<SbSlot>> p,
                                         Ts&&... rest) {
    result.emplace_back(std::move(p.first), p.second);
    makeSbExprOptSbSlotVecHelper(result, std::forward<Ts>(rest)...);
}
}  // namespace

template <typename... Ts>
auto makeSbExprOptSbSlotVec(Ts&&... pack) {
    SbExprOptSbSlotVector v;
    if constexpr (sizeof...(pack) > 0) {
        v.reserve(sizeof...(Ts));
        makeSbExprOptSbSlotVecHelper(v, std::forward<Ts>(pack)...);
    }
    return v;
}

struct SbIndexInfoSlots {
    boost::optional<SbSlot> indexIdentSlot;
    boost::optional<SbSlot> indexKeySlot;
    boost::optional<SbSlot> indexKeyPatternSlot;
    boost::optional<SbSlot> snapshotIdSlot;
};

enum class SbIndexInfoType : uint32_t {
    kNoInfo = 0u,
    kIndexIdent = 1u << 0u,
    kIndexKey = 1u << 1u,
    kIndexKeyPattern = 1u << 2u,
    kSnapshotId = 1u << 3u,
};

inline SbIndexInfoType operator&(SbIndexInfoType t1, SbIndexInfoType t2) {
    return static_cast<SbIndexInfoType>(static_cast<uint32_t>(t1) & static_cast<uint32_t>(t2));
}
inline SbIndexInfoType operator|(SbIndexInfoType t1, SbIndexInfoType t2) {
    return static_cast<SbIndexInfoType>(static_cast<uint32_t>(t1) | static_cast<uint32_t>(t2));
}
inline SbIndexInfoType operator^(SbIndexInfoType t1, SbIndexInfoType t2) {
    return static_cast<SbIndexInfoType>(static_cast<uint32_t>(t1) ^ static_cast<uint32_t>(t2));
}
inline SbIndexInfoType operator~(SbIndexInfoType t) {
    return static_cast<SbIndexInfoType>(~static_cast<uint32_t>(t));
}

struct SbScanBounds {
    boost::optional<SbSlot> minRecordIdSlot;
    boost::optional<SbSlot> maxRecordIdSlot;
    bool includeScanStartRecordId = true;
    bool includeScanEndRecordId = true;
};

/**
 * SbExprBuilder is a helper class that offers numerous methods for building expressions. These
 * methods take all take their input expressions in the form of SbExprs and return their result
 * in the form of an SbExpr.
 */
class SbExprBuilder {
public:
    using CaseValuePair = SbExpr::CaseValuePair;

    SbExprBuilder(StageBuilderState& state) : _state(state) {}

    SbExpr cloneExpr(const SbExpr& expr) {
        return expr.clone();
    }

    SbExpr makeVariable(sbe::value::SlotId slotId) {
        return SbVar{slotId};
    }

    SbExpr makeVariable(SbVar var) {
        return var;
    }

    SbExpr makeVariable(sbe::FrameId frameId, sbe::value::SlotId slotId) {
        return SbVar(frameId, slotId);
    }

    SbExpr makeNot(SbExpr e);

    SbExpr makeUnaryOp(sbe::EPrimUnary::Op unaryOp, SbExpr e);
    SbExpr makeUnaryOp(optimizer::Operations unaryOp, SbExpr e);

    SbExpr makeBinaryOp(sbe::EPrimBinary::Op unaryOp, SbExpr lhs, SbExpr rhs);
    SbExpr makeBinaryOp(optimizer::Operations unaryOp, SbExpr lhs, SbExpr rhs);

    SbExpr makeConstant(sbe::value::TypeTags tag, sbe::value::Value val);
    SbExpr makeNothingConstant();
    SbExpr makeNullConstant();
    SbExpr makeBoolConstant(bool boolVal);
    SbExpr makeInt32Constant(int32_t num);
    SbExpr makeInt64Constant(int64_t num);
    SbExpr makeDoubleConstant(double num);
    SbExpr makeDecimalConstant(const Decimal128& num);
    SbExpr makeStrConstant(StringData str);
    SbExpr makeUndefinedConstant();

    SbExpr makeFunction(StringData name, SbExpr::Vector args);

    template <typename... Args>
    inline SbExpr makeFunction(StringData name, Args&&... args) {
        return makeFunction(name, SbExpr::makeSeq(std::forward<Args>(args)...));
    }

    SbExpr makeIf(SbExpr condExpr, SbExpr thenExpr, SbExpr elseExpr);

    SbExpr makeLet(sbe::FrameId frameId, SbExpr::Vector binds, SbExpr expr);

    SbExpr makeLocalLambda(sbe::FrameId frameId, SbExpr expr);

    SbExpr makeNumericConvert(SbExpr expr, sbe::value::TypeTags tag);

    SbExpr makeFail(ErrorCodes::Error error, StringData errorMessage);

    SbExpr makeFillEmpty(SbExpr expr, SbExpr altExpr);
    SbExpr makeFillEmptyFalse(SbExpr expr);
    SbExpr makeFillEmptyTrue(SbExpr expr);
    SbExpr makeFillEmptyNull(SbExpr expr);
    SbExpr makeFillEmptyUndefined(SbExpr expr);

    SbExpr makeIfNullExpr(SbExpr::Vector values);

    SbExpr generateNullOrMissing(SbExpr expr);
    SbExpr generatePositiveCheck(SbExpr expr);
    SbExpr generateNullMissingOrUndefined(SbExpr expr);

    SbExpr generateNullOrMissing(SbVar var);
    SbExpr generateNullMissingOrUndefined(SbVar var);
    SbExpr generateNonStringCheck(SbVar var);
    SbExpr generateNonTimestampCheck(SbVar var);
    SbExpr generateNegativeCheck(SbVar var);
    SbExpr generateNonPositiveCheck(SbVar var);
    SbExpr generateNonNumericCheck(SbVar var);
    SbExpr generateLongLongMinCheck(SbVar var);
    SbExpr generateNonArrayCheck(SbVar var);
    SbExpr generateNonObjectCheck(SbVar var);
    SbExpr generateNullishOrNotRepresentableInt32Check(SbVar var);
    SbExpr generateNaNCheck(SbVar var);
    SbExpr generateInfinityCheck(SbVar var);
    SbExpr generateInvalidRoundPlaceArgCheck(SbVar var);

    SbExpr buildMultiBranchConditional(SbExpr defaultCase) {
        return defaultCase;
    }

    template <typename... Ts>
    SbExpr buildMultiBranchConditional(SbExpr::CaseValuePair headCase, Ts... rest) {
        return makeIf(std::move(headCase.first),
                      std::move(headCase.second),
                      buildMultiBranchConditional(std::forward<Ts>(rest)...));
    }

    SbExpr buildMultiBranchConditionalFromCaseValuePairs(
        std::vector<SbExpr::CaseValuePair> caseValPairs, SbExpr defaultVal) {
        SbExpr result = std::move(defaultVal);

        for (size_t i = caseValPairs.size(); i > 0;) {
            --i;
            result = makeIf(std::move(caseValPairs[i].first),
                            std::move(caseValPairs[i].second),
                            std::move(result));
        }

        return result;
    }

    SbExpr makeBalancedBooleanOpTree(sbe::EPrimBinary::Op logicOp, SbExpr::Vector leaves) {
        return stage_builder::makeBalancedBooleanOpTree(logicOp, std::move(leaves), _state);
    }

    SbExpr makeBalancedBooleanOpTree(optimizer::Operations logicOp, SbExpr::Vector leaves) {
        return stage_builder::makeBalancedBooleanOpTree(
            getEPrimBinaryOp(logicOp), std::move(leaves), _state);
    }

    std::unique_ptr<sbe::EExpression> lower(SbExpr& e, const VariableTypes* varTypes = nullptr) {
        return e.extractExpr(_state, varTypes);
    }

    sbe::EExpression::Vector lower(SbExpr::Vector& sbExprs,
                                   const VariableTypes* varTypes = nullptr);

    sbe::value::SlotId lower(SbSlot s, const VariableTypes* = nullptr) {
        return s.getId();
    }

    boost::optional<sbe::value::SlotId> lower(boost::optional<SbSlot> s,
                                              const VariableTypes* = nullptr) {
        return s ? boost::make_optional(s->getId()) : boost::none;
    }

    sbe::value::SlotVector lower(const SbSlotVector& sbSlots, const VariableTypes* = nullptr);

    std::vector<sbe::value::SlotVector> lower(const std::vector<SbSlotVector>& sbSlotVectors,
                                              const VariableTypes* varTypes = nullptr);

    sbe::SlotExprPairVector lower(SbExprSbSlotVector& sbSlotSbExprVec,
                                  const VariableTypes* varTypes = nullptr);

    StageBuilderState& _state;
};

/**
 * The SbBuilder class extends SbExprBuilder with additional methods for creating SbStages.
 */
class SbBuilder : public SbExprBuilder {
public:
    using SbExprBuilder::lower;

    SbBuilder(StageBuilderState& state, PlanNodeId nodeId)
        : SbExprBuilder(state), _nodeId(nodeId) {}

    inline PlanNodeId getNodeId() const {
        return _nodeId;
    }
    inline void setNodeId(PlanNodeId nodeId) {
        _nodeId = nodeId;
    }

    std::tuple<SbStage, SbSlot, SbSlot, SbSlotVector> makeScan(
        UUID collectionUuid,
        DatabaseName dbName,
        bool forward = true,
        boost::optional<SbSlot> seekSlot = boost::none,
        std::vector<std::string> scanFieldNames = {},
        const SbScanBounds& scanBounds = {},
        const SbIndexInfoSlots& indexInfoSlots = {},
        sbe::ScanCallbacks scanCallbacks = {},
        boost::optional<SbSlot> oplogTsSlot = boost::none,
        bool lowPriority = false);

    std::tuple<SbStage, SbSlot, SbSlotVector, SbIndexInfoSlots> makeSimpleIndexScan(
        UUID collectionUuid,
        DatabaseName dbName,
        StringData indexName,
        const BSONObj& keyPattern,
        bool forward = true,
        SbExpr lowKeyExpr = SbExpr{},
        SbExpr highKeyExpr = SbExpr{},
        sbe::IndexKeysInclusionSet indexKeysToInclude = sbe::IndexKeysInclusionSet{},
        SbIndexInfoType indexInfoTypeMask = SbIndexInfoType::kNoInfo,
        bool lowPriority = false) {
        return makeSimpleIndexScan(VariableTypes{},
                                   std::move(collectionUuid),
                                   std::move(dbName),
                                   indexName,
                                   keyPattern,
                                   forward,
                                   std::move(lowKeyExpr),
                                   std::move(highKeyExpr),
                                   std::move(indexKeysToInclude),
                                   indexInfoTypeMask,
                                   lowPriority);
    }

    std::tuple<SbStage, SbSlot, SbSlotVector, SbIndexInfoSlots> makeSimpleIndexScan(
        const VariableTypes& varTypes,
        UUID collectionUuid,
        DatabaseName dbName,
        StringData indexName,
        const BSONObj& keyPattern,
        bool forward = true,
        SbExpr lowKeyExpr = SbExpr{},
        SbExpr highKeyExpr = SbExpr{},
        sbe::IndexKeysInclusionSet indexKeysToInclude = sbe::IndexKeysInclusionSet{},
        SbIndexInfoType indexInfoTypeMask = SbIndexInfoType::kNoInfo,
        bool lowPriority = false);

    std::tuple<SbStage, SbSlot, SbSlotVector, SbIndexInfoSlots> makeGenericIndexScan(
        UUID collectionUuid,
        DatabaseName dbName,
        StringData indexName,
        const BSONObj& keyPattern,
        bool forward,
        SbExpr boundsExpr,
        key_string::Version version,
        Ordering ordering,
        sbe::IndexKeysInclusionSet indexKeysToInclude,
        SbIndexInfoType indexInfoTypeMask) {
        return makeGenericIndexScan(VariableTypes{},
                                    std::move(collectionUuid),
                                    std::move(dbName),
                                    indexName,
                                    keyPattern,
                                    forward,
                                    std::move(boundsExpr),
                                    version,
                                    ordering,
                                    std::move(indexKeysToInclude),
                                    indexInfoTypeMask);
    }

    std::tuple<SbStage, SbSlot, SbSlotVector, SbIndexInfoSlots> makeGenericIndexScan(
        const VariableTypes& varTypes,
        UUID collectionUuid,
        DatabaseName dbName,
        StringData indexName,
        const BSONObj& keyPattern,
        bool forward,
        SbExpr boundsExpr,
        key_string::Version version,
        Ordering ordering,
        sbe::IndexKeysInclusionSet indexKeysToInclude = sbe::IndexKeysInclusionSet{},
        SbIndexInfoType indexInfoTypeMask = SbIndexInfoType::kNoInfo);

    std::pair<SbStage, SbSlot> makeVirtualScan(sbe::value::TypeTags inputTag,
                                               sbe::value::Value inputVal);

    SbStage makeLimit(SbStage stage, SbExpr limit) {
        return makeLimit(VariableTypes{}, std::move(stage), std::move(limit));
    }

    SbStage makeLimit(const VariableTypes& varTypes, SbStage stage, SbExpr limit);

    SbStage makeLimitSkip(SbStage stage, SbExpr limit, SbExpr skip) {
        return makeLimitSkip(VariableTypes{}, std::move(stage), std::move(limit), std::move(skip));
    }

    SbStage makeLimitSkip(const VariableTypes& varTypes, SbStage stage, SbExpr limit, SbExpr skip);

    SbStage makeCoScan();

    SbStage makeLimitOneCoScanTree();

    inline SbStage makeFilter(SbStage stage, SbExpr condition) {
        return makeFilter(VariableTypes{}, std::move(stage), std::move(condition));
    }

    SbStage makeFilter(const VariableTypes& varTypes, SbStage stage, SbExpr condition);

    inline SbStage makeConstFilter(SbStage stage, SbExpr condition) {
        return makeConstFilter(VariableTypes{}, std::move(stage), std::move(condition));
    }

    SbStage makeConstFilter(const VariableTypes& varTypes, SbStage, SbExpr condition);

    template <typename... Args>
    inline std::pair<SbStage, SbSlotVector> makeProject(SbStage stage, SbExpr arg, Args&&... args) {
        return makeProject(VariableTypes{},
                           std::move(stage),
                           makeSbExprOptSbSlotVec(std::move(arg), std::forward<Args>(args)...));
    }

    template <typename T, typename... Args>
    inline std::pair<SbStage, SbSlotVector> makeProject(SbStage stage,
                                                        std::pair<SbExpr, T> arg,
                                                        Args&&... args) {
        return makeProject(VariableTypes{},
                           std::move(stage),
                           makeSbExprOptSbSlotVec(std::move(arg), std::forward<Args>(args)...));
    }

    inline std::pair<SbStage, SbSlotVector> makeProject(SbStage stage,
                                                        SbExprOptSbSlotVector projects) {
        return makeProject(VariableTypes{}, std::move(stage), std::move(projects));
    }

    template <typename... Args>
    inline std::pair<SbStage, SbSlotVector> makeProject(const VariableTypes& varTypes,
                                                        SbStage stage,
                                                        SbExpr arg,
                                                        Args&&... args) {
        return makeProject(varTypes,
                           std::move(stage),
                           makeSbExprOptSbSlotVec(std::move(arg), std::forward<Args>(args)...));
    }

    template <typename T, typename... Args>
    inline std::pair<SbStage, SbSlotVector> makeProject(const VariableTypes& varTypes,
                                                        SbStage stage,
                                                        std::pair<SbExpr, T> arg,
                                                        Args&&... args) {
        return makeProject(varTypes,
                           std::move(stage),
                           makeSbExprOptSbSlotVec(std::move(arg), std::forward<Args>(args)...));
    }

    std::pair<SbStage, SbSlotVector> makeProject(const VariableTypes& varTypes,
                                                 SbStage stage,
                                                 SbExprOptSbSlotVector projects);

    inline SbStage makeLoopJoin(SbStage outer,
                                SbStage inner,
                                const SbSlotVector& outerProjects,
                                const SbSlotVector& outerCorrelated,
                                const SbSlotVector& innerProjects = SbSlotVector{},
                                SbExpr predicate = SbExpr{},
                                sbe::JoinType joinType = sbe::JoinType::Inner) {
        return makeLoopJoin(VariableTypes{},
                            std::move(outer),
                            std::move(inner),
                            outerProjects,
                            outerCorrelated,
                            innerProjects,
                            std::move(predicate),
                            joinType);
    }

    SbStage makeLoopJoin(const VariableTypes& varTypes,
                         SbStage outer,
                         SbStage inner,
                         const SbSlotVector& outerProjects,
                         const SbSlotVector& outerCorrelated,
                         const SbSlotVector& innerProjects = SbSlotVector{},
                         SbExpr predicate = SbExpr{},
                         sbe::JoinType joinType = sbe::JoinType::Inner);

    SbStage makeUnique(SbStage stage, SbSlot key) {
        return makeUnique(VariableTypes{}, std::move(stage), key);
    }

    SbStage makeUnique(const VariableTypes& varTypes, SbStage stage, SbSlot key);

    SbStage makeUnique(SbStage stage, const SbSlotVector& keys) {
        return makeUnique(VariableTypes{}, std::move(stage), keys);
    }

    SbStage makeUnique(const VariableTypes& varTypes, SbStage stage, const SbSlotVector& keys);

    SbStage makeSort(SbStage stage,
                     const SbSlotVector& orderBy,
                     std::vector<sbe::value::SortDirection> dirs,
                     const SbSlotVector& forwardedSlots,
                     SbExpr limitExpr,
                     size_t memoryLimit) {
        return makeSort(VariableTypes{},
                        std::move(stage),
                        orderBy,
                        std::move(dirs),
                        forwardedSlots,
                        std::move(limitExpr),
                        memoryLimit);
    }

    SbStage makeSort(const VariableTypes& varTypes,
                     SbStage stage,
                     const SbSlotVector& orderBy,
                     std::vector<sbe::value::SortDirection> dirs,
                     const SbSlotVector& forwardedSlots,
                     SbExpr limitExpr,
                     size_t memoryLimit);

    std::tuple<SbStage, SbSlotVector, SbSlotVector> makeHashAgg(
        SbStage stage,
        const SbSlotVector& groupBySlots,
        SbAggExprVector sbAggExprs,
        boost::optional<sbe::value::SlotId> collatorSlot,
        SbExprSbSlotVector mergingExprs) {
        return makeHashAgg(VariableTypes{},
                           std::move(stage),
                           groupBySlots,
                           std::move(sbAggExprs),
                           collatorSlot,
                           std::move(mergingExprs));
    }

    std::tuple<SbStage, SbSlotVector, SbSlotVector> makeHashAgg(
        const VariableTypes& varTypes,
        SbStage stage,
        const SbSlotVector& groupBySlots,
        SbAggExprVector sbAggExprs,
        boost::optional<sbe::value::SlotId> collatorSlot,
        SbExprSbSlotVector mergingExprs);

    std::tuple<SbStage, SbSlotVector, SbSlotVector> makeBlockHashAgg(
        SbStage stage,
        const SbSlotVector& groupBySlots,
        SbAggExprVector sbAggExprs,
        SbSlot selectivityBitmapSlot,
        const SbSlotVector& blockAccArgSlots,
        SbSlot bitmapInternalSlot,
        const SbSlotVector& accumulatorDataSlots,
        SbExprSbSlotVector mergingExprs) {
        return makeBlockHashAgg(VariableTypes{},
                                std::move(stage),
                                groupBySlots,
                                std::move(sbAggExprs),
                                selectivityBitmapSlot,
                                blockAccArgSlots,
                                bitmapInternalSlot,
                                accumulatorDataSlots,
                                std::move(mergingExprs));
    }

    std::tuple<SbStage, SbSlotVector, SbSlotVector> makeBlockHashAgg(
        const VariableTypes& varTypes,
        SbStage stage,
        const SbSlotVector& groupBySlots,
        SbAggExprVector sbAggExprs,
        SbSlot selectivityBitmapSlot,
        const SbSlotVector& blockAccArgSbSlots,
        SbSlot bitmapInternalSlot,
        const SbSlotVector& accumulatorDataSbSlots,
        SbExprSbSlotVector mergingExprs);

    std::tuple<SbStage, SbSlotVector> makeAggProject(SbStage stage, SbAggExprVector sbAggExprs) {
        return makeAggProject(VariableTypes{}, std::move(stage), std::move(sbAggExprs));
    }

    std::tuple<SbStage, SbSlotVector> makeAggProject(const VariableTypes& varTypes,
                                                     SbStage stage,
                                                     SbAggExprVector sbAggExprs);

    SbStage makeWindow(SbStage stage,
                       const SbSlotVector& currSlots,
                       const SbSlotVector& boundTestingSlots,
                       size_t partitionSlotCount,
                       std::vector<SbWindow> windows,
                       boost::optional<sbe::value::SlotId> collatorSlot) {
        return makeWindow(VariableTypes{},
                          std::move(stage),
                          currSlots,
                          boundTestingSlots,
                          partitionSlotCount,
                          std::move(windows),
                          collatorSlot);
    }

    SbStage makeWindow(const VariableTypes& varTypes,
                       SbStage stage,
                       const SbSlotVector& currSlots,
                       const SbSlotVector& boundTestingSlots,
                       size_t partitionSlotCount,
                       std::vector<SbWindow> windows,
                       boost::optional<sbe::value::SlotId> collatorSlot);

    std::tuple<SbStage, SbSlot, SbSlot> makeUnwind(SbStage stage,
                                                   SbSlot inputSlot,
                                                   bool preserveNullAndEmptyArrays);

    std::pair<SbStage, SbSlotVector> makeUnion(sbe::PlanStage::Vector stages,
                                               const std::vector<SbSlotVector>& slots);

    std::pair<SbStage, SbSlotVector> makeSortedMerge(sbe::PlanStage::Vector stages,
                                                     const std::vector<SbSlotVector>& slots,
                                                     const std::vector<SbSlotVector>& keys,
                                                     std::vector<sbe::value::SortDirection> dirs);

    std::pair<SbStage, SbSlotVector> makeBranch(SbStage thenStage,
                                                SbStage elseStage,
                                                SbExpr conditionExpr,
                                                const SbSlotVector& thenSlots,
                                                const SbSlotVector& elseSlots) {
        return makeBranch(VariableTypes{},
                          std::move(thenStage),
                          std::move(elseStage),
                          std::move(conditionExpr),
                          thenSlots,
                          elseSlots);
    }

    std::pair<SbStage, SbSlotVector> makeBranch(const VariableTypes& varTypes,
                                                SbStage thenStage,
                                                SbStage elseStage,
                                                SbExpr conditionExpr,
                                                const SbSlotVector& thenSlots,
                                                const SbSlotVector& elseSlots);

protected:
    SbIndexInfoSlots allocateIndexInfoSlots(SbIndexInfoType indexInfoTypeMask,
                                            const BSONObj& keyPattern);

    SbSlotVector allocateOutSlotsForMergeStage(const std::vector<SbSlotVector>& slots);

    sbe::WindowStage::Window lower(SbWindow& sbWindow, const VariableTypes* varTypes = nullptr);

    std::vector<sbe::WindowStage::Window> lower(std::vector<SbWindow>& sbWindows,
                                                const VariableTypes* varTypes = nullptr);

    PlanNodeId _nodeId;
};
}  // namespace mongo::stage_builder
