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
#include "mongo/db/exec/sbe/stages/window.h"
#include "mongo/db/exec/sbe/values/cell_interface.h"
#include "mongo/db/query/stage_builder/sbe/builder_state.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr.h"

namespace mongo::stage_builder {
inline void makeSbExprOptSbSlotVecHelper(SbExprOptSlotVector& result) {}

template <typename... Ts>
inline void makeSbExprOptSbSlotVecHelper(SbExprOptSlotVector& result, SbExpr expr, Ts&&... rest) {
    result.emplace_back(std::move(expr), boost::none);
    makeSbExprOptSbSlotVecHelper(result, std::forward<Ts>(rest)...);
}

template <typename... Ts>
inline void makeSbExprOptSbSlotVecHelper(SbExprOptSlotVector& result,
                                         std::pair<SbExpr, sbe::value::SlotId> p,
                                         Ts&&... rest) {
    result.emplace_back(std::move(p.first), boost::make_optional(SbSlot{p.second}));
    makeSbExprOptSbSlotVecHelper(result, std::forward<Ts>(rest)...);
}

template <typename... Ts>
inline void makeSbExprOptSbSlotVecHelper(SbExprOptSlotVector& result,
                                         std::pair<SbExpr, SbSlot> p,
                                         Ts&&... rest) {
    result.emplace_back(std::move(p.first), boost::make_optional(p.second));
    makeSbExprOptSbSlotVecHelper(result, std::forward<Ts>(rest)...);
}

template <typename... Ts>
inline void makeSbExprOptSbSlotVecHelper(SbExprOptSlotVector& result,
                                         std::pair<SbExpr, boost::optional<sbe::value::SlotId>> p,
                                         Ts&&... rest) {
    result.emplace_back(std::move(p.first),
                        p.second ? boost::make_optional(SbSlot{*p.second}) : boost::none);
    makeSbExprOptSbSlotVecHelper(result, std::forward<Ts>(rest)...);
}

template <typename... Ts>
inline void makeSbExprOptSbSlotVecHelper(SbExprOptSlotVector& result,
                                         std::pair<SbExpr, boost::optional<SbSlot>> p,
                                         Ts&&... rest) {
    result.emplace_back(std::move(p.first), p.second);
    makeSbExprOptSbSlotVecHelper(result, std::forward<Ts>(rest)...);
}

template <typename... Ts>
auto makeSbExprOptSbSlotVec(Ts&&... pack) {
    SbExprOptSlotVector v;
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
    using CaseValuePair = SbExprPair;

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

    SbExpr makeUnaryOp(abt::Operations unaryOp, SbExpr e);
    SbExpr makeBinaryOp(abt::Operations binaryOp, SbExpr lhs, SbExpr rhs);
    SbExpr makeNaryOp(abt::Operations naryOp, SbExpr::Vector args);

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

    /**
     * Check if expression returns Nothing and return 'altExpr' if so. Otherwise, return the
     * expression.
     */
    SbExpr makeFillEmpty(SbExpr expr, SbExpr altExpr);

    /**
     * Check if expression returns Nothing and return boolean false if so. Otherwise, return the
     * expression.
     */
    SbExpr makeFillEmptyFalse(SbExpr expr);

    /**
     * Check if expression returns Nothing and return boolean true if so. Otherwise, return the
     * expression.
     */
    SbExpr makeFillEmptyTrue(SbExpr expr);

    /**
     * Check if expression returns Nothing and return null if so. Otherwise, return the expression.
     */
    SbExpr makeFillEmptyNull(SbExpr expr);

    /**
     * Check if expression returns Nothing and return undefined if so. Otherwise, return the
     * expression.
     */
    SbExpr makeFillEmptyUndefined(SbExpr expr);

    SbExpr makeIfNullExpr(SbExpr::Vector values);

    SbExpr generateNullOrMissing(SbExpr expr);
    SbExpr generatePositiveCheck(SbExpr expr);
    SbExpr generateNullMissingOrUndefined(SbExpr expr);

    SbExpr generateNullOrMissing(SbVar var);
    SbExpr generateNullMissingOrUndefined(SbVar var);
    SbExpr generateNonStringCheck(SbVar var);
    SbExpr generateNonTimestampCheck(SbVar var);

    /**
     * Generates an expression that checks if the input expression is negative assuming that it has
     * already been verified to have numeric type and to not be NaN.
     */
    SbExpr generateNegativeCheck(SbVar var);

    SbExpr generateNonPositiveCheck(SbVar var);
    SbExpr generateNonNumericCheck(SbVar var);
    SbExpr generateLongLongMinCheck(SbVar var);
    SbExpr generateNonArrayCheck(SbVar var);
    SbExpr generateNonObjectCheck(SbVar var);
    SbExpr generateNullishOrNotRepresentableInt32Check(SbVar var);

    /**
     * Generates an expression that checks if the input expression is NaN _assuming that_ it has
     * already been verified to be numeric.
     */
    SbExpr generateNaNCheck(SbVar var);

    SbExpr generateInfinityCheck(SbVar var);

    /**
     * Generates an expression to check the given variable is a number between -20 and 100
     * inclusive, and is a whole number.
     */
    SbExpr generateInvalidRoundPlaceArgCheck(SbVar var);

    /**
     * Convert a list of case/value pairs into a chain of "If" expressions, with the final else
     * case evaluating to the 'defaultCase' expression.
     */
    SbExpr buildMultiBranchConditional(SbExpr defaultCase) {
        return defaultCase;
    }

    template <typename... Ts>
    SbExpr buildMultiBranchConditional(SbExprPair headCase, Ts... rest) {
        return makeIf(std::move(headCase.first),
                      std::move(headCase.second),
                      buildMultiBranchConditional(std::forward<Ts>(rest)...));
    }

    /**
     * Converts a std::vector of case/value pairs into a chain of "If" expressions in the
     * same manner as the buildMultiBranchConditional() method.
     */
    SbExpr buildMultiBranchConditionalFromCaseValuePairs(std::vector<SbExprPair> caseValPairs,
                                                         SbExpr defaultVal);

    /**
     * Creates a boolean expression tree from given collection of leaf expressions.
     */
    SbExpr makeBooleanOpTree(abt::Operations logicOp, SbExpr::Vector leaves);

    SbExpr makeBooleanOpTree(abt::Operations logicOp, SbExpr lhs, SbExpr rhs);

    std::unique_ptr<sbe::EExpression> lower(SbExpr& e, const VariableTypes* varTypes = nullptr) {
        return e.lower(_state, varTypes);
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

    sbe::SlotExprPairVector lower(SbExprSlotVector& sbSlotSbExprVec,
                                  const VariableTypes* varTypes = nullptr);

    sbe::WindowStage::Window lower(SbWindow& sbWindow, const VariableTypes* varTypes = nullptr);

    std::vector<sbe::WindowStage::Window> lower(std::vector<SbWindow>& sbWindows,
                                                const VariableTypes* varTypes = nullptr);

protected:
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
        boost::optional<SbSlot> oplogTsSlot = boost::none);

    std::tuple<SbStage, SbSlot, SbSlotVector, SbIndexInfoSlots> makeSimpleIndexScan(
        UUID collectionUuid,
        DatabaseName dbName,
        StringData indexName,
        const BSONObj& keyPattern,
        bool forward = true,
        SbExpr lowKeyExpr = SbExpr{},
        SbExpr highKeyExpr = SbExpr{},
        sbe::IndexKeysInclusionSet indexKeysToInclude = sbe::IndexKeysInclusionSet{},
        SbIndexInfoType indexInfoTypeMask = SbIndexInfoType::kNoInfo) {
        return makeSimpleIndexScan(VariableTypes{},
                                   std::move(collectionUuid),
                                   std::move(dbName),
                                   indexName,
                                   keyPattern,
                                   forward,
                                   std::move(lowKeyExpr),
                                   std::move(highKeyExpr),
                                   std::move(indexKeysToInclude),
                                   indexInfoTypeMask);
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
        SbIndexInfoType indexInfoTypeMask = SbIndexInfoType::kNoInfo);

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

    SbStage makeCoScan();

    SbStage makeLimit(SbStage stage, SbExpr limit) {
        return makeLimit(VariableTypes{}, std::move(stage), std::move(limit));
    }

    SbStage makeLimit(const VariableTypes& varTypes, SbStage stage, SbExpr limit);

    SbStage makeLimitSkip(SbStage stage, SbExpr limit, SbExpr skip) {
        return makeLimitSkip(VariableTypes{}, std::move(stage), std::move(limit), std::move(skip));
    }

    SbStage makeLimitSkip(const VariableTypes& varTypes, SbStage stage, SbExpr limit, SbExpr skip);

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
                                                        SbExprOptSlotVector projects) {
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
                                                 SbExprOptSlotVector projects);

    SbStage makeUnique(SbStage stage, SbSlot key);

    SbStage makeUnique(SbStage stage, const SbSlotVector& keys);

    SbStage makeUniqueRoaring(SbStage stage, SbSlot key);

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
        const VariableTypes& varTypes,
        SbStage stage,
        const SbSlotVector& gbs,
        const SbHashAggAccumulatorVector& accumulatorList,
        boost::optional<sbe::value::SlotId> collatorSlot);

    std::tuple<SbStage, SbSlotVector, SbSlotVector> makeBlockHashAgg(
        const VariableTypes& varTypes,
        SbStage stage,
        const SbSlotVector& groupBySlots,
        SbBlockAggExprVector sbBlockAggExprs,
        SbSlot selectivityBitmapSlot,
        const SbSlotVector& blockAccArgSbSlots,
        SbSlot bitmapInternalSlot,
        const SbSlotVector& accumulatorDataSbSlots,
        SbExprSlotVector mergingExprs);

    std::tuple<SbStage, SbSlotVector> makeAggProject(SbStage stage,
                                                     SbBlockAggExprVector sbBlockAggExprs) {
        return makeAggProject(VariableTypes{}, std::move(stage), std::move(sbBlockAggExprs));
    }

    std::tuple<SbStage, SbSlotVector> makeAggProject(const VariableTypes& varTypes,
                                                     SbStage stage,
                                                     SbBlockAggExprVector sbBlockAggExprs);

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

    std::tuple<SbStage, SbSlot, SbSlotVector, SbSlotVector> makeTsBucketToCellBlock(
        SbStage stage,
        SbSlot bucketSlot,
        const std::vector<sbe::value::CellBlock::PathRequest>& topLevelReqs,
        const std::vector<sbe::value::CellBlock::PathRequest>& traverseReqs,
        const std::string& timeField);

    std::pair<SbStage, SbSlotVector> makeBlockToRow(SbStage stage,
                                                    const SbSlotVector& blockSlots,
                                                    SbSlot bitmapSlot);

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

    std::pair<SbStage, SbSlot> makeHashLookup(SbStage localStage,
                                              SbStage foreignStage,
                                              SbSlot localKeySlot,
                                              SbSlot foreignKeySlot,
                                              SbSlot foreignRecordSlot,
                                              SbBlockAggExpr sbBlockAggExpr,
                                              boost::optional<SbSlot> optOutputSlot,
                                              boost::optional<sbe::value::SlotId> collatorSlot) {
        return makeHashLookup(VariableTypes{},
                              std::move(localStage),
                              std::move(foreignStage),
                              localKeySlot,
                              foreignKeySlot,
                              foreignRecordSlot,
                              std::move(sbBlockAggExpr),
                              optOutputSlot,
                              collatorSlot);
    }

    std::pair<SbStage, SbSlot> makeHashLookup(const VariableTypes& varTypes,
                                              SbStage localStage,
                                              SbStage foreignStage,
                                              SbSlot localKeySlot,
                                              SbSlot foreignKeySlot,
                                              SbSlot foreignRecordSlot,
                                              SbBlockAggExpr sbBlockAggExpr,
                                              boost::optional<SbSlot> optOutputSlot,
                                              boost::optional<sbe::value::SlotId> collatorSlot);

    std::pair<SbStage, SbSlot> makeHashLookupUnwind(
        SbStage localStage,
        SbStage foreignStage,
        SbSlot localKeySlot,
        SbSlot foreignKeySlot,
        SbSlot foreignRecordSlot,
        boost::optional<sbe::value::SlotId> collatorSlot) {
        return makeHashLookupUnwind(VariableTypes{},
                                    std::move(localStage),
                                    std::move(foreignStage),
                                    localKeySlot,
                                    foreignKeySlot,
                                    foreignRecordSlot,
                                    collatorSlot);
    }

    std::pair<SbStage, SbSlot> makeHashLookupUnwind(
        const VariableTypes& varTypes,
        SbStage localStage,
        SbStage foreignStage,
        SbSlot localKeySlot,
        SbSlot foreignKeySlot,
        SbSlot foreignRecordSlot,
        boost::optional<sbe::value::SlotId> collatorSlot);

    SbStage makeHashJoin(SbStage outerStage,
                         SbStage innerStage,
                         const SbSlotVector& outerCondSlots,
                         const SbSlotVector& outerProjectSlots,
                         const SbSlotVector& innerCondSlots,
                         const SbSlotVector& innerProjectSlots,
                         boost::optional<sbe::value::SlotId> collatorSlot);

    SbStage makeMergeJoin(SbStage outerStage,
                          SbStage innerStage,
                          const SbSlotVector& outerKeySlots,
                          const SbSlotVector& outerProjectSlots,
                          const SbSlotVector& innerKeySlots,
                          const SbSlotVector& innerProjectSlots,
                          std::vector<sbe::value::SortDirection> dirs);

protected:
    SbIndexInfoSlots allocateIndexInfoSlots(SbIndexInfoType indexInfoTypeMask,
                                            const BSONObj& keyPattern);

    SbSlotVector allocateOutSlotsForMergeStage(const std::vector<SbSlotVector>& slots);

    PlanNodeId _nodeId;

private:
    bool useIncreasedSpilling(bool allowDiskUse,
                              SbeHashAggIncreasedSpillingModeEnum forceIncreasedSpillingMode) {
        switch (forceIncreasedSpillingMode) {
            case SbeHashAggIncreasedSpillingModeEnum::kAlways:
                return true;
            case SbeHashAggIncreasedSpillingModeEnum::kNever:
                return false;
            case SbeHashAggIncreasedSpillingModeEnum::kInDebug:
                return kDebugBuild && allowDiskUse;
            default:
                tasserted(9915702, "Unknown forceIncreasedSpillingMode");
        }
    }
};
}  // namespace mongo::stage_builder
