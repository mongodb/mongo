// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/sbexpr_helpers.h"

#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/stages/agg_project.h"
#include "mongo/db/exec/sbe/stages/and_hash.h"
#include "mongo/db/exec/sbe/stages/block_hashagg.h"
#include "mongo/db/exec/sbe/stages/block_to_row.h"
#include "mongo/db/exec/sbe/stages/branch.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/fetch.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/generic_scan.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/hash_agg_accumulator.h"
#include "mongo/db/exec/sbe/stages/hash_join.h"
#include "mongo/db/exec/sbe/stages/hash_lookup.h"
#include "mongo/db/exec/sbe/stages/hash_lookup_unwind.h"
#include "mongo/db/exec/sbe/stages/ix_scan.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/merge_join.h"
#include "mongo/db/exec/sbe/stages/multi_range_clustered_scan_stage.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/sort.h"
#include "mongo/db/exec/sbe/stages/sorted_merge.h"
#include "mongo/db/exec/sbe/stages/ts_bucket_to_cell_block.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/stages/unique.h"
#include "mongo/db/exec/sbe/stages/unwind.h"
#include "mongo/db/exec/sbe/stages/virtual_scan.h"
#include "mongo/db/query/stage_builder/sbe/builder_data.h"
#include "mongo/db/query/stage_builder/sbe/sbe_stage_blueprint.h"
#include "mongo/db/query/stage_builder/sbe/sbe_stage_lowering.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr.h"
#include "mongo/util/overloaded_visitor.h"

#include <memory>
#include <string_view>
#include <variant>

namespace mongo::stage_builder {
namespace {

// Adds an already-lowered stage to the state's blueprint as a leaf, so that it can be used as the
// child of a blueprint node.
SbBlueprintNodeIdx addStage(StageBuilderState& state, SbStage stage) {
    return state.blueprint().add(SbBlueprintPassthrough{std::move(stage)});
}

SbBlueprintChildVector addStages(StageBuilderState& state, sbe::PlanStage::Vector stages) {
    SbBlueprintChildVector children;
    children.reserve(stages.size());
    for (auto& stage : stages) {
        children.push_back(addStage(state, std::move(stage)));
    }
    return children;
}

// Adds 'blueprint' to the state's blueprint tree and immediately lowers it along with its
// children. Lowering leaves SbBlueprintLowered placeholders behind, so the tree only ever holds
// nodes that are lowered or about to be.
template <typename T>
SbStage lowerBlueprint(StageBuilderState& state,
                       T blueprint,
                       const VariableTypes* varTypes = nullptr) {
    auto root = state.blueprint().add(std::move(blueprint));
    return lowerSbeBlueprint(state.blueprint(), root, state, varTypes);
}

inline abt::ABT extractABT(SbExpr& e) {
    return e.extractABT();
}

inline abt::ABTVector extractABT(SbExpr::Vector& exprs) {
    // Convert the SbExpr vector to an ABT vector.
    abt::ABTVector abtExprs;
    abtExprs.reserve(exprs.size());

    for (auto& e : exprs) {
        abtExprs.emplace_back(extractABT(e));
    }

    return abtExprs;
}

inline std::vector<std::pair<abt::ABT, abt::ABT>> extractABT(std::vector<SbExprPair>& exprPairs) {
    // Convert the SbExprPair vector to a pair<ABT,ABT> vector.
    std::vector<std::pair<abt::ABT, abt::ABT>> abtExprPairs;

    abtExprPairs.reserve(exprPairs.size());

    for (auto& p : exprPairs) {
        abtExprPairs.emplace_back(extractABT(p.first), extractABT(p.second));
    }

    return abtExprPairs;
}
}  // namespace

sbe::EExpression::Vector SbExprBuilder::lower(SbExpr::Vector& sbExprs,
                                              const VariableTypes* varTypes) {
    // Convert the SbExpr vector to an EExpression vector.
    sbe::EExpression::Vector exprs;
    exprs.reserve(sbExprs.size());

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

sbe::SlotExprPairVector SbExprBuilder::lower(SbExprSlotVector& sbSlotSbExprVec,
                                             const VariableTypes* varTypes) {
    sbe::SlotExprPairVector slotExprVec;
    slotExprVec.reserve(sbSlotSbExprVec.size());

    for (auto& [sbExpr, sbSlot] : sbSlotSbExprVec) {
        slotExprVec.emplace_back(std::pair(sbSlot.getId(), sbExpr.lower(_state, varTypes)));
    }

    return slotExprVec;
}


SbExpr SbExprBuilder::makeNot(SbExpr e) {
    return makeUnaryOp(abt::Operations::Not, std::move(e));
}

SbExpr SbExprBuilder::makeUnaryOp(abt::Operations unaryOp, SbExpr e) {
    return abt::make<abt::UnaryOp>(unaryOp, extractABT(e));
}

SbExpr SbExprBuilder::makeBinaryOp(abt::Operations binaryOp, SbExpr lhs, SbExpr rhs) {
    return abt::make<abt::BinaryOp>(binaryOp, extractABT(lhs), extractABT(rhs));
}

SbExpr SbExprBuilder::makeNaryOp(abt::Operations naryOp, SbExpr::Vector args) {
    tassert(10199700, "Expected at least one argument", !args.empty());

    if (feature_flags::gFeatureFlagSbeUpgradeBinaryTrees.checkEnabled()) {
        return abt::make<abt::NaryOp>(naryOp, extractABT(args));
    } else {
        return std::accumulate(std::make_move_iterator(args.begin() + 1),
                               std::make_move_iterator(args.end()),
                               std::move(args.front()),
                               [&](auto&& acc, auto&& ex) -> SbExpr {
                                   return makeBinaryOp(naryOp, std::move(acc), std::move(ex));
                               });
    }
}

SbExpr SbExprBuilder::makeConstant(sbe::value::TypeTags tag, sbe::value::Value val) {
    return abt::make<abt::Constant>(tag, val);
}

SbExpr SbExprBuilder::makeNothingConstant() {
    return abt::Constant::nothing();
}

SbExpr SbExprBuilder::makeNullConstant() {
    return abt::Constant::null();
}

SbExpr SbExprBuilder::makeBoolConstant(bool boolVal) {
    return abt::Constant::boolean(boolVal);
}

SbExpr SbExprBuilder::makeInt32Constant(int32_t num) {
    return abt::Constant::int32(num);
}

SbExpr SbExprBuilder::makeInt64Constant(int64_t num) {
    return abt::Constant::int64(num);
}

SbExpr SbExprBuilder::makeDoubleConstant(double num) {
    return abt::Constant::fromDouble(num);
}

SbExpr SbExprBuilder::makeDecimalConstant(const Decimal128& num) {
    return abt::Constant::fromDecimal(num);
}

SbExpr SbExprBuilder::makeStrConstant(std::string_view str) {
    return abt::Constant::str(str);
}

SbExpr SbExprBuilder::makeUndefinedConstant() {
    return abt::make<abt::Constant>(sbe::value::TypeTags::bsonUndefined, 0);
}

SbExpr SbExprBuilder::makeFunction(sbe::EFn fn, SbExpr::Vector args) {
    return abt::make<abt::FunctionCall>(fn, extractABT(args));
}

SbExpr SbExprBuilder::makeIf(SbExpr condExpr, SbExpr thenExpr, SbExpr elseExpr) {
    return abt::make<abt::If>(extractABT(condExpr), extractABT(thenExpr), extractABT(elseExpr));
}

SbExpr SbExprBuilder::makeLet(sbe::FrameId frameId, SbExpr::Vector binds, SbExpr expr) {
    if (!feature_flags::gFeatureFlagSbeUpgradeBinaryTrees.checkEnabled()) {
        for (size_t idx = binds.size(); idx > 0;) {
            --idx;
            expr = abt::make<abt::Let>(
                SbVar(frameId, idx).toProjectionName(), extractABT(binds[idx]), extractABT(expr));
        }

        return expr;
    } else {
        std::vector<abt::ProjectionName> bindNames;
        bindNames.reserve(binds.size());
        for (size_t idx = 0; idx < binds.size(); ++idx) {
            bindNames.emplace_back(SbVar(frameId, idx).toProjectionName());
        }

        binds.emplace_back(std::move(expr));
        return abt::make<abt::MultiLet>(std::move(bindNames), extractABT(binds));
    }
}

SbExpr SbExprBuilder::makeLocalLambda(sbe::FrameId frameId, SbExpr expr) {
    return abt::make<abt::LambdaAbstraction>(SbVar(frameId, 0).toProjectionName(),
                                             extractABT(expr));
}

SbExpr SbExprBuilder::makeLocalLambda2(sbe::FrameId frameId, SbExpr expr) {
    return abt::make<abt::LambdaAbstraction>(SbVar(frameId, 0).toProjectionName(),
                                             SbVar(frameId, 1).toProjectionName(),
                                             extractABT(expr));
}

SbExpr SbExprBuilder::makeNumericConvert(SbExpr expr, sbe::value::TypeTags tag) {
    return makeFunction(
        sbe::EFn::kConvert, std::move(expr), makeInt32Constant(static_cast<int32_t>(tag)));
}

SbExpr SbExprBuilder::makeFail(ErrorCodes::Error error, std::string_view errorMessage) {
    return makeFunction(sbe::EFn::kFail, makeInt32Constant(error), makeStrConstant(errorMessage));
}

SbExpr SbExprBuilder::makeFillEmpty(SbExpr expr, SbExpr altExpr) {
    return makeBinaryOp(abt::Operations::FillEmpty, std::move(expr), std::move(altExpr));
}

SbExpr SbExprBuilder::makeFillEmptyFalse(SbExpr expr) {
    return makeFillEmpty(std::move(expr), makeBoolConstant(false));
}

SbExpr SbExprBuilder::makeFillEmptyTrue(SbExpr expr) {
    return makeFillEmpty(std::move(expr), makeBoolConstant(true));
}

SbExpr SbExprBuilder::makeFillEmptyNull(SbExpr expr) {
    return makeFillEmpty(std::move(expr), makeNullConstant());
}

SbExpr SbExprBuilder::makeFillEmptyUndefined(SbExpr expr) {
    return makeFillEmpty(std::move(expr), makeUndefinedConstant());
}

SbExpr SbExprBuilder::makeIfNullExpr(SbExpr::Vector values) {
    tassert(6987505, "Expected 'values' to be non-empty", values.size() > 0);

    size_t idx = values.size() - 1;
    auto expr = std::move(values[idx]);

    while (idx > 0) {
        --idx;

        auto frameId = _state.frameId();
        SbVar var{frameId, 0};

        expr = makeLet(frameId,
                       SbExpr::makeSeq(std::move(values[idx])),
                       makeIf(generateNullMissingOrUndefined(var), std::move(expr), var));
    }

    return expr;
}

SbExpr SbExprBuilder::generateNullOrMissing(SbExpr expr) {
    return makeFillEmptyTrue(makeFunction(sbe::EFn::kIsNull, std::move(expr)));
}

SbExpr SbExprBuilder::generateNullMissingOrUndefined(SbExpr expr) {
    return makeFunction(sbe::EFn::kIsNullish, std::move(expr));
}

SbExpr SbExprBuilder::generatePositiveCheck(SbExpr expr) {
    return makeBinaryOp(abt::Operations::Gt, std::move(expr), makeInt32Constant(0));
}

SbExpr SbExprBuilder::generateNullOrMissing(SbVar var) {
    return makeFillEmptyTrue(makeFunction(sbe::EFn::kIsNull, var));
}

SbExpr SbExprBuilder::generateNullMissingOrUndefined(SbVar var) {
    return makeFunction(sbe::EFn::kIsNullish, var);
}

SbExpr SbExprBuilder::generateNonStringCheck(SbVar var) {
    return makeNot(makeFunction(sbe::EFn::kIsString, var));
}

SbExpr SbExprBuilder::generateNonTimestampCheck(SbVar var) {
    return makeNot(makeFunction(sbe::EFn::kIsTimestamp, var));
}

SbExpr SbExprBuilder::generateNegativeCheck(SbVar var) {
    return makeBinaryOp(abt::Operations::And,
                        makeNot(makeFunction(sbe::EFn::kIsNaN, var)),
                        makeBinaryOp(abt::Operations::Lt, var, makeInt32Constant(0)));
}

SbExpr SbExprBuilder::generateNonPositiveCheck(SbVar var) {
    return makeBinaryOp(abt::Operations::Lte, var, makeInt32Constant(0));
}

SbExpr SbExprBuilder::generateNonNumericCheck(SbVar var) {
    return makeNot(makeFunction(sbe::EFn::kIsNumber, var));
}

SbExpr SbExprBuilder::generateLongLongMinCheck(SbVar var) {
    return makeBinaryOp(
        abt::Operations::And,
        makeFunction(
            sbe::EFn::kTypeMatch, var, makeInt32Constant(getBSONTypeMask(BSONType::numberLong))),
        makeBinaryOp(
            abt::Operations::Eq, var, makeInt64Constant(std::numeric_limits<int64_t>::min())));
}

SbExpr SbExprBuilder::generateNonArrayCheck(SbVar var) {
    return makeNot(makeFunction(sbe::EFn::kIsArray, var));
}

SbExpr SbExprBuilder::generateNonObjectCheck(SbVar var) {
    return makeNot(makeFunction(sbe::EFn::kIsObject, var));
}

SbExpr SbExprBuilder::generateNullishOrNotRepresentableInt32Check(SbVar var) {
    return makeBinaryOp(
        abt::Operations::Or,
        generateNullMissingOrUndefined(var),
        makeNot(makeFunction(sbe::EFn::kExists,
                             makeFunction(sbe::EFn::kConvert,
                                          var,
                                          makeInt32Constant(static_cast<int32_t>(
                                              sbe::value::TypeTags::NumberInt32))))));
}

SbExpr SbExprBuilder::generateNaNCheck(SbVar var) {
    return makeFunction(sbe::EFn::kIsNaN, var);
}

SbExpr SbExprBuilder::generateInfinityCheck(SbVar var) {
    return makeFunction(sbe::EFn::kIsInfinity, var);
}

SbExpr SbExprBuilder::generateInvalidRoundPlaceArgCheck(SbVar var) {
    return makeBooleanOpTree(
        abt::Operations::Or,
        SbExpr::makeSeq(
            // We can perform our numerical test with trunc. trunc will return nothing if we pass a
            // non-number to it. We return true if the comparison returns nothing, or if
            // var != trunc(var), indicating this is not a whole number.
            makeFillEmptyTrue(
                makeBinaryOp(abt::Operations::Neq, var, makeFunction(sbe::EFn::kTrunc, var))),
            makeBinaryOp(abt::Operations::Lt, var, makeInt32Constant(-20)),
            makeBinaryOp(abt::Operations::Gt, var, makeInt32Constant(100))));
}

SbExpr SbExprBuilder::buildMultiBranchConditionalFromCaseValuePairs(
    std::vector<SbExprPair> caseValPairs, SbExpr defaultVal) {
    if (!feature_flags::gFeatureFlagSbeUpgradeBinaryTrees.checkEnabled()) {
        return std::accumulate(
            std::make_reverse_iterator(std::make_move_iterator(caseValPairs.end())),
            std::make_reverse_iterator(std::make_move_iterator(caseValPairs.begin())),
            std::move(defaultVal),
            [&](auto&& expression, auto&& caseValuePair) {
                return buildMultiBranchConditional(std::move(caseValuePair), std::move(expression));
            });
    } else {
        return abt::make<abt::Switch>(extractABT(caseValPairs), extractABT(defaultVal));
    }
}

SbExpr SbExprBuilder::makeBooleanOpTree(abt::Operations logicOp, SbExpr::Vector leaves) {
    tassert(10668302, "Expected at least one expression", !leaves.empty());

    if (leaves.size() == 1) {
        return std::move(leaves[0]);
    }

    if ((logicOp == abt::Operations::And || logicOp == abt::Operations::Or) &&
        feature_flags::gFeatureFlagSbeUpgradeBinaryTrees.checkEnabled()) {
        return makeNaryOp(logicOp, std::move(leaves));
    }

    auto builder = [&](SbExpr lhs, SbExpr rhs) {
        return makeBinaryOp(logicOp, std::move(lhs), std::move(rhs));
    };

    return SbExpr::makeBalancedTree(builder, std::move(leaves));
}

SbExpr SbExprBuilder::makeBooleanOpTree(abt::Operations logicOp, SbExpr lhs, SbExpr rhs) {
    SbExpr::Vector leaves;
    leaves.emplace_back(std::move(lhs));
    leaves.emplace_back(std::move(rhs));
    return makeBooleanOpTree(logicOp, std::move(leaves));
}

SbBuilder::MakeScanResult SbBuilder::makeScan(UUID collectionUuid,
                                              DatabaseName dbName,
                                              bool forward,
                                              std::vector<std::string> scanFieldNames,
                                              const SbScanBounds& scanBounds,
                                              const SbIndexInfoSlots& indexInfoSlots,
                                              sbe::ScanOpenCallback scanOpenCallback,
                                              boost::optional<SbSlot> oplogTsSlot) {
    auto resultSlot = SbSlot{_state.slotId()};
    auto recordIdSlot = SbSlot{_state.slotId()};

    SbSlotVector scanFieldSlots;
    scanFieldSlots.reserve(scanFieldNames.size());

    for (size_t i = 0; i < scanFieldNames.size(); ++i) {
        scanFieldSlots.emplace_back(SbSlot{_state.slotId()});
    }

    auto stage = lowerBlueprint(_state,
                                SbBlueprintScan{.nodeId = _nodeId,
                                                .collectionUuid = std::move(collectionUuid),
                                                .dbName = std::move(dbName),
                                                .forward = forward,
                                                .scanFieldNames = std::move(scanFieldNames),
                                                .scanBounds = scanBounds,
                                                .indexInfoSlots = indexInfoSlots,
                                                .scanOpenCallback = std::move(scanOpenCallback),
                                                .recordSlot = resultSlot.getId(),
                                                .recordIdSlot = recordIdSlot.getId(),
                                                .scanFieldSlots = lower(scanFieldSlots)});
    return {std::move(stage), resultSlot, recordIdSlot, std::move(scanFieldSlots)};
}

SbBuilder::MakeScanResult SbBuilder::makeScan(UUID collectionUuid,
                                              DatabaseName dbName,
                                              bool forward,
                                              std::vector<std::string> scanFieldNames,
                                              RecordIdRangeList scanBounds,
                                              const SbIndexInfoSlots& indexInfoSlots,
                                              sbe::ScanOpenCallback scanOpenCallback) {
    auto resultSlot = SbSlot{_state.slotId()};
    auto recordIdSlot = SbSlot{_state.slotId()};

    SbSlotVector scanFieldSlots;
    scanFieldSlots.reserve(scanFieldNames.size());
    for (size_t i = 0; i < scanFieldNames.size(); ++i) {
        scanFieldSlots.emplace_back(SbSlot{_state.slotId()});
    }

    auto scanStage =
        sbe::makeS<sbe::MultiRangeClusteredScanStage>(collectionUuid,
                                                      std::move(dbName),
                                                      lower(resultSlot),
                                                      lower(recordIdSlot),
                                                      lower(indexInfoSlots.snapshotIdSlot),
                                                      lower(indexInfoSlots.indexIdentSlot),
                                                      lower(indexInfoSlots.indexKeySlot),
                                                      lower(indexInfoSlots.indexKeyPatternSlot),
                                                      std::move(scanFieldNames),
                                                      lower(scanFieldSlots),
                                                      std::move(scanBounds),
                                                      forward,
                                                      _state.yieldPolicy,
                                                      _nodeId,
                                                      std::move(scanOpenCallback),
                                                      true /* participateInTrialRunTracking */);

    return {std::move(scanStage), resultSlot, recordIdSlot, std::move(scanFieldSlots)};
}

std::tuple<SbStage, SbSlot, SbSlotVector, SbIndexInfoSlots> SbBuilder::makeSimpleIndexScan(
    const VariableTypes& varTypes,
    UUID collectionUuid,
    DatabaseName dbName,
    std::string_view indexName,
    const BSONObj& keyPattern,
    bool forward,
    SbExpr lowKeyExpr,
    SbExpr highKeyExpr,
    sbe::IndexKeysInclusionSet indexKeysToInclude,
    SbIndexInfoType indexInfoTypeMask) {
    SbSlot recordIdSlot = SbSlot{_state.slotId()};
    const size_t numIndexKeys = indexKeysToInclude.count();

    SbSlotVector indexKeySlots;
    indexKeySlots.reserve(numIndexKeys);
    for (size_t i = 0; i < numIndexKeys; ++i) {
        indexKeySlots.emplace_back(SbSlot{_state.slotId()});
    }

    SbIndexInfoSlots indexInfoSlots = allocateIndexInfoSlots(indexInfoTypeMask, keyPattern);

    auto stage = lowerBlueprint(
        _state,
        SbBlueprintSimpleIndexScan{.nodeId = _nodeId,
                                   .collectionUuid = std::move(collectionUuid),
                                   .dbName = std::move(dbName),
                                   .indexName = std::string{indexName},
                                   .forward = forward,
                                   .lowKeyExpr = std::move(lowKeyExpr),
                                   .highKeyExpr = std::move(highKeyExpr),
                                   .indexKeysToInclude = std::move(indexKeysToInclude),
                                   .recordIdSlot = recordIdSlot.getId(),
                                   .indexKeySlots = lower(indexKeySlots),
                                   .indexInfoSlots = indexInfoSlots},
        &varTypes);
    return {std::move(stage), recordIdSlot, std::move(indexKeySlots), std::move(indexInfoSlots)};
}

std::tuple<SbStage, SbSlot, SbSlotVector, SbIndexInfoSlots> SbBuilder::makeGenericIndexScan(
    const VariableTypes& varTypes,
    UUID collectionUuid,
    DatabaseName dbName,
    std::string_view indexName,
    const BSONObj& keyPattern,
    bool forward,
    SbExpr boundsExpr,
    key_string::Version version,
    Ordering ordering,
    sbe::IndexKeysInclusionSet indexKeysToInclude,
    SbIndexInfoType indexInfoTypeMask) {
    SbSlot recordIdSlot = SbSlot{_state.slotId()};
    const size_t numIndexKeys = indexKeysToInclude.count();

    SbSlotVector indexKeySlots;
    indexKeySlots.reserve(numIndexKeys);
    for (size_t i = 0; i < numIndexKeys; ++i) {
        indexKeySlots.emplace_back(SbSlot{_state.slotId()});
    }

    SbIndexInfoSlots indexInfoSlots = allocateIndexInfoSlots(indexInfoTypeMask, keyPattern);

    auto stage = lowerBlueprint(
        _state,
        SbBlueprintGenericIndexScan{.nodeId = _nodeId,
                                    .collectionUuid = std::move(collectionUuid),
                                    .dbName = std::move(dbName),
                                    .indexName = std::string{indexName},
                                    .keyPattern = keyPattern,
                                    .forward = forward,
                                    .boundsExpr = std::move(boundsExpr),
                                    .version = version,
                                    .ordering = ordering,
                                    .indexKeysToInclude = std::move(indexKeysToInclude),
                                    .recordIdSlot = recordIdSlot.getId(),
                                    .indexKeySlots = lower(indexKeySlots),
                                    .indexInfoSlots = indexInfoSlots},
        &varTypes);
    return {std::move(stage), recordIdSlot, std::move(indexKeySlots), std::move(indexInfoSlots)};
}

std::pair<SbStage, SbSlot> SbBuilder::makeVirtualScan(sbe::value::TypeTags inputTag,
                                                      sbe::value::Value inputVal) {
    auto outSlotId = _state.slotId();
    auto outSlot = SbSlot{outSlotId};

    auto stage = lowerBlueprint(
        _state,
        SbBlueprintVirtualScan{.nodeId = _nodeId,
                               .outSlot = outSlotId,
                               .input = sbe::value::TagValueOwned::fromRaw(inputTag, inputVal)});
    return {std::move(stage), outSlot};
}

SbStage SbBuilder::makeCoScan() {
    return lowerBlueprint(_state, SbBlueprintCoScan{.nodeId = _nodeId});
}

SbStage SbBuilder::makeLimit(const VariableTypes& varTypes, SbStage stage, SbExpr limitConstant) {
    return lowerBlueprint(_state,
                          SbBlueprintLimitSkip{.child = addStage(_state, std::move(stage)),
                                               .nodeId = _nodeId,
                                               .limitExpr = std::move(limitConstant)},
                          &varTypes);
}

SbStage SbBuilder::makeLimitSkip(const VariableTypes& varTypes,
                                 SbStage stage,
                                 SbExpr limitConstant,
                                 SbExpr skipConstant) {
    return lowerBlueprint(_state,
                          SbBlueprintLimitSkip{.child = addStage(_state, std::move(stage)),
                                               .nodeId = _nodeId,
                                               .limitExpr = std::move(limitConstant),
                                               .skipExpr = std::move(skipConstant)},
                          &varTypes);
}

SbStage SbBuilder::makeLimitOneCoScanTree() {
    auto coScan = _state.blueprint().add(SbBlueprintCoScan{.nodeId = _nodeId});
    return lowerBlueprint(_state,
                          SbBlueprintLimitSkip{.child = coScan,
                                               .nodeId = _nodeId,
                                               .limitExpr = makeInt64Constant(1)});
}

SbStage SbBuilder::makeFilter(const VariableTypes& varTypes, SbStage stage, SbExpr condition) {
    return lowerBlueprint(_state,
                          SbBlueprintFilter{.child = addStage(_state, std::move(stage)),
                                            .nodeId = _nodeId,
                                            .condition = std::move(condition)},
                          &varTypes);
}

SbStage SbBuilder::makeConstFilter(const VariableTypes& varTypes, SbStage stage, SbExpr condition) {
    return lowerBlueprint(_state,
                          SbBlueprintFilter{.child = addStage(_state, std::move(stage)),
                                            .nodeId = _nodeId,
                                            .condition = std::move(condition),
                                            .filterType = SbBlueprintFilter::FilterType::kConst},
                          &varTypes);
}

std::pair<SbStage, SbSlotVector> SbBuilder::makeProject(const VariableTypes& varTypes,
                                                        SbStage stage,
                                                        SbExprOptSlotVector projects) {
    SbExprSlotVector slotExprPairs;
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
            slotExprPairs.emplace_back(std::move(expr), SbSlot{slot});
        }
    }

    if (!slotExprPairs.empty()) {
        // The expressions were already optimized with 'varTypes' above, so they are lowered
        // without it.
        return {lowerBlueprint(_state,
                               SbBlueprintProject{.child = addStage(_state, std::move(stage)),
                                                  .nodeId = _nodeId,
                                                  .projects = std::move(slotExprPairs)}),
                std::move(outSlots)};
    }

    return {std::move(stage), std::move(outSlots)};
}

SbStage SbBuilder::makeUnique(SbStage stage, SbSlot key) {
    return lowerBlueprint(_state,
                          SbBlueprintUnique{.child = addStage(_state, std::move(stage)),
                                            .nodeId = _nodeId,
                                            .keys = sbe::value::SlotVector{key.getId()}});
}

SbStage SbBuilder::makeUnique(SbStage stage, const SbSlotVector& keys) {
    return lowerBlueprint(_state,
                          SbBlueprintUnique{.child = addStage(_state, std::move(stage)),
                                            .nodeId = _nodeId,
                                            .keys = lower(keys)});
}

SbStage SbBuilder::makeUniqueRoaring(SbStage stage, SbSlot key) {
    return lowerBlueprint(_state,
                          SbBlueprintUniqueRoaring{.child = addStage(_state, std::move(stage)),
                                                   .nodeId = _nodeId,
                                                   .key = key.getId()});
}

SbStage SbBuilder::makeSort(const VariableTypes& varTypes,
                            SbStage stage,
                            const SbSlotVector& orderBy,
                            std::vector<sbe::value::SortDirection> dirs,
                            const SbSlotVector& forwardedSlots,
                            SbExpr limitExpr,
                            size_t memoryLimit) {
    return lowerBlueprint(_state,
                          SbBlueprintSort{.child = addStage(_state, std::move(stage)),
                                          .nodeId = _nodeId,
                                          .orderBy = lower(orderBy),
                                          .dirs = {dirs.begin(), dirs.end()},
                                          .forwardedSlots = lower(forwardedSlots),
                                          .limitExpr = std::move(limitExpr),
                                          .memoryLimit = memoryLimit},
                          &varTypes);
}

std::tuple<SbStage, SbSlotVector, SbSlotVector> SbBuilder::makeHashAgg(
    const VariableTypes& varTypes,
    SbStage stage,
    const SbSlotVector& gbs,
    const SbHashAggAccumulatorVector& accumulatorList,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    // In debug builds or when we explicitly set the query knob, we artificially force frequent
    // spilling. This makes sure that our tests exercise the spilling algorithm and the associated
    // logic for merging partial aggregates which otherwise would require large data sizes to
    // exercise.

    const bool forceIncreasedSpilling = useIncreasedSpilling(
        _state.allowDiskUse,
        _state.expCtx->getQueryKnobConfiguration().getSbeHashAggIncreasedSpillingMode());


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

    using AccumulatorImpl = decltype(SbHashAggAccumulator::implementation);

    std::vector<SbBlueprintHashAggAccumulator> accumulators;
    accumulators.reserve(accumulatorList.size());
    SbSlotVector aggOutSlots;
    for (auto& sbAccumulator : accumulatorList) {
        auto outSlot = sbAccumulator.outSlot ? *sbAccumulator.outSlot : SbSlot{_state.slotId()};
        aggOutSlots.emplace_back(outSlot);

        auto implementation =
            std::visit(OverloadedVisitor{
                           [](const SbHashAggCompiledAccumulator& impl) -> AccumulatorImpl {
                               return SbHashAggCompiledAccumulator{
                                   impl.init.clone(), impl.agg.clone(), impl.merge.clone()};
                           },
                           []<class Implementation>(
                               const SbHashAggSinglePurposeScalarAccumulator<Implementation>& impl)
                               -> AccumulatorImpl {
                               return SbHashAggSinglePurposeScalarAccumulator<Implementation>{
                                   impl.transform.clone()};
                           }},
                       sbAccumulator.implementation);

        accumulators.push_back(
            SbBlueprintHashAggAccumulator{.outSlot = outSlot.getId(),
                                          .spillSlot = sbAccumulator.spillSlot.getId(),
                                          .implementation = std::move(implementation)});
    }

    stage = lowerBlueprint(_state,
                           SbBlueprintHashAgg{.child = addStage(_state, std::move(stage)),
                                              .nodeId = _nodeId,
                                              .groupBySlots = std::move(groupBySlots),
                                              .accumulators = std::move(accumulators),
                                              .collatorSlot = collatorSlot,
                                              .forceIncreasedSpilling = forceIncreasedSpilling},
                           &varTypes);

    return {std::move(stage), std::move(groupByOutSlots), std::move(aggOutSlots)};
}

std::tuple<SbStage, SbSlotVector, SbSlotVector> SbBuilder::makeBlockHashAgg(
    const VariableTypes& varTypes,
    SbStage stage,
    const SbSlotVector& gbs,
    SbBlockAggExprVector SbBlockAggExprs,
    SbSlot selectivityBitmapSlot,
    const SbSlotVector& blockAccArgSbSlots,
    SbSlot bitmapInternalSlot,
    const SbSlotVector& accumulatorDataSbSlots,
    SbExprSlotVector mergingExprs) {
    tassert(8448607, "Expected at least one group by slot to be provided", gbs.size() > 0);

    std::vector<std::pair<sbe::value::SlotId, SbBlockAggExpr>> aggs;
    SbSlotVector aggOutSlots;

    for (auto& [sbBlockAggExpr, optSbSlot] : SbBlockAggExprs) {
        auto sbSlot = optSbSlot ? *optSbSlot : SbSlot{_state.slotId()};
        sbSlot.setTypeSignature(TypeSignature::kBlockType.include(TypeSignature::kAnyScalarType));

        aggOutSlots.emplace_back(sbSlot);
        aggs.emplace_back(sbSlot.getId(), std::move(sbBlockAggExpr));
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

    const bool forceIncreasedSpilling = useIncreasedSpilling(
        _state.allowDiskUse,
        _state.expCtx->getQueryKnobConfiguration().getSbeHashAggIncreasedSpillingMode());

    stage = lowerBlueprint(
        _state,
        SbBlueprintBlockHashAgg{.child = addStage(_state, std::move(stage)),
                                .nodeId = _nodeId,
                                .groupBySlots = std::move(groupBySlots),
                                .selectivityBitmapSlot = selectivityBitmapSlot.getId(),
                                .blockAccArgSlots = lower(blockAccArgSbSlots),
                                .bitmapInternalSlot = bitmapInternalSlot.getId(),
                                .accumulatorDataSlots = lower(accumulatorDataSbSlots),
                                .aggs = std::move(aggs),
                                .mergingExprs = std::move(mergingExprs),
                                .forceIncreasedSpilling = forceIncreasedSpilling},
        &varTypes);

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
                                                            SbBlockAggExprVector sbBlockAggExprs) {
    sbe::AggExprVector aggExprsVec;
    SbSlotVector aggOutSlots;

    for (auto& [sbBlockAggExpr, optSbSlot] : sbBlockAggExprs) {
        auto sbSlot = optSbSlot ? *optSbSlot : SbSlot{_state.slotId()};
        aggOutSlots.emplace_back(sbSlot);

        auto exprPair = sbe::AggExprPair{sbBlockAggExpr.init.lower(_state, &varTypes),
                                         sbBlockAggExpr.agg.lower(_state, &varTypes)};

        aggExprsVec.emplace_back(std::pair(sbSlot.getId(), std::move(exprPair)));
    }

    stage = sbe::makeS<sbe::AggProjectStage>(std::move(stage), std::move(aggExprsVec), _nodeId);

    return {std::move(stage), std::move(aggOutSlots)};
}

std::tuple<SbStage, SbSlot, SbSlot> SbBuilder::makeUnwind(SbStage stage,
                                                          SbSlot inputSlot,
                                                          bool preserveNullAndEmptyArrays) {
    auto unwindOutputSlot = SbSlot{_state.slotId()};
    auto indexOutputSlot = SbSlot{_state.slotId()};

    auto result =
        lowerBlueprint(_state,
                       SbBlueprintUnwind{.child = addStage(_state, std::move(stage)),
                                         .nodeId = _nodeId,
                                         .inSlot = inputSlot.getId(),
                                         .outSlot = unwindOutputSlot.getId(),
                                         .idxSlot = indexOutputSlot.getId(),
                                         .preserveNullAndEmptyArrays = preserveNullAndEmptyArrays});
    return {std::move(result), unwindOutputSlot, indexOutputSlot};
}

std::tuple<SbStage, SbSlot, SbSlotVector, SbSlotVector> SbBuilder::makeTsBucketToCellBlock(
    SbStage stage,
    SbSlot bucketSlot,
    const std::vector<sbe::value::PathRequest>& topLevelReqs,
    const std::vector<sbe::value::PathRequest>& traverseReqs,
    const std::string& timeField) {
    const auto bitmapSlot = SbSlot{_state.slotId()};

    SbSlotVector topLevelSlots;
    topLevelSlots.reserve(topLevelReqs.size());
    for (size_t i = 0; i < topLevelReqs.size(); ++i) {
        auto field = topLevelReqs[i].getTopLevelField();
        auto typeSig = field == timeField
            ? TypeSignature::kCellType.include(TypeSignature::kDateTimeType)
            : TypeSignature::kCellType.include(TypeSignature::kAnyScalarType);

        topLevelSlots.emplace_back(SbSlot{_state.slotId(), typeSig});
    }

    SbSlotVector traverseSlots;
    traverseSlots.reserve(traverseReqs.size());
    for (size_t i = 0; i < traverseReqs.size(); ++i) {
        auto field = traverseReqs[i].getFullPath();
        auto typeSig = field == timeField
            ? TypeSignature::kCellType.include(TypeSignature::kDateTimeType)
            : TypeSignature::kCellType.include(TypeSignature::kAnyScalarType);

        traverseSlots.emplace_back(SbSlot{_state.slotId(), typeSig});
    }

    auto result =
        lowerBlueprint(_state,
                       SbBlueprintTsBucketToCellBlock{.child = addStage(_state, std::move(stage)),
                                                      .nodeId = _nodeId,
                                                      .bucketSlot = bucketSlot.getId(),
                                                      .topLevelReqs = topLevelReqs,
                                                      .traverseReqs = traverseReqs,
                                                      .timeField = std::string{timeField},
                                                      .bitmapSlot = bitmapSlot.getId(),
                                                      .topLevelSlots = lower(topLevelSlots),
                                                      .traverseSlots = lower(traverseSlots)});
    return {std::move(result), bitmapSlot, std::move(topLevelSlots), std::move(traverseSlots)};
}

std::pair<SbStage, SbSlotVector> SbBuilder::makeBlockToRow(SbStage stage,
                                                           const SbSlotVector& blockSlots,
                                                           SbSlot bitmapSlot) {
    SbSlotVector unpackedSlots;
    unpackedSlots.reserve(blockSlots.size());

    for (size_t i = 0; i < blockSlots.size(); ++i) {
        // 'blockSlots[i]' and 'unpackedSlots[i]' will have the same type except that the
        // unpacked slot's type will be scalar.
        boost::optional<TypeSignature> typeSig = blockSlots[i].getTypeSignature();
        if (typeSig) {
            typeSig = typeSig->exclude(TypeSignature::kBlockType).exclude(TypeSignature::kCellType);
        }

        unpackedSlots.emplace_back(SbSlot{_state.slotId(), typeSig});
    }

    auto result = lowerBlueprint(_state,
                                 SbBlueprintBlockToRow{.child = addStage(_state, std::move(stage)),
                                                       .nodeId = _nodeId,
                                                       .bitmapSlot = bitmapSlot.getId(),
                                                       .blockSlots = lower(blockSlots),
                                                       .outputSlots = lower(unpackedSlots)});
    return {std::move(result), std::move(unpackedSlots)};
}

std::pair<SbStage, SbSlotVector> SbBuilder::makeUnion(sbe::PlanStage::Vector stages,
                                                      const std::vector<SbSlotVector>& slots) {
    tassert(9380400,
            "Expected the same number of stages and input slot vectors",
            stages.size() == slots.size());

    SbSlotVector outSlots = allocateOutSlotsForMergeStage(slots);

    auto result = lowerBlueprint(_state,
                                 SbBlueprintUnion{.nodeId = _nodeId,
                                                  .children = addStages(_state, std::move(stages)),
                                                  .inputSlots = lower(slots),
                                                  .outputSlots = lower(outSlots)});
    return {std::move(result), std::move(outSlots)};
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

    auto result =
        lowerBlueprint(_state,
                       SbBlueprintSortedMerge{.nodeId = _nodeId,
                                              .children = addStages(_state, std::move(stages)),
                                              .inputKeys = lower(keys),
                                              .dirs = {dirs.begin(), dirs.end()},
                                              .inputVals = lower(slots),
                                              .outputVals = lower(outSlots)});
    return {std::move(result), std::move(outSlots)};
}

SbStage SbBuilder::makeAndHash(SbStage outerStage,
                               SbStage innerStage,
                               const SbSlotVector& outerCondSlots,
                               const SbSlotVector& outerProjectSlots,
                               const SbSlotVector& innerCondSlots,
                               const SbSlotVector& innerProjectSlots,
                               boost::optional<sbe::value::SlotId> collatorSlot) {
    return lowerBlueprint(_state,
                          SbBlueprintAndHash{.outer = addStage(_state, std::move(outerStage)),
                                             .inner = addStage(_state, std::move(innerStage)),
                                             .nodeId = _nodeId,
                                             .outerCondSlots = lower(outerCondSlots),
                                             .outerProjectSlots = lower(outerProjectSlots),
                                             .innerCondSlots = lower(innerCondSlots),
                                             .innerProjectSlots = lower(innerProjectSlots),
                                             .collatorSlot = collatorSlot});
}

std::pair<SbStage, SbSlotVector> SbBuilder::makeBranch(const VariableTypes& varTypes,
                                                       SbStage thenStage,
                                                       SbStage elseStage,
                                                       SbExpr conditionExpr,
                                                       const SbSlotVector& thenSlots,
                                                       const SbSlotVector& elseSlots) {
    const size_t n = thenSlots.size();

    tassert(9405101, "Expected both input slot vectors to be the same size", n == elseSlots.size());

    SbSlotVector outSlots;
    outSlots.reserve(n);

    for (size_t i = 0; i < n; ++i) {
        // Get the type signatures of the jth element from both input slot vectors and compute
        // the union of these type signatures.
        boost::optional<TypeSignature> unionTypeSig = thenSlots[i].getTypeSignature();

        if (unionTypeSig) {
            auto typeSig = elseSlots[i].getTypeSignature();
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

    auto result =
        lowerBlueprint(_state,
                       SbBlueprintBranch{.thenChild = addStage(_state, std::move(thenStage)),
                                         .elseChild = addStage(_state, std::move(elseStage)),
                                         .nodeId = _nodeId,
                                         .conditionExpr = std::move(conditionExpr),
                                         .thenSlots = lower(thenSlots),
                                         .elseSlots = lower(elseSlots),
                                         .outputSlots = lower(outSlots)},
                       &varTypes);
    return {std::move(result), std::move(outSlots)};
}

SbStage SbBuilder::makeLoopJoin(const VariableTypes& varTypes,
                                SbStage outer,
                                SbStage inner,
                                const SbSlotVector& outerProjects,
                                const SbSlotVector& outerCorrelated,
                                const SbSlotVector& innerProjects,
                                SbExpr predicate,
                                sbe::JoinType joinType) {
    return lowerBlueprint(_state,
                          SbBlueprintLoopJoin{.outer = addStage(_state, std::move(outer)),
                                              .inner = addStage(_state, std::move(inner)),
                                              .nodeId = _nodeId,
                                              .joinType = joinType,
                                              .outerProjects = lower(outerProjects),
                                              .outerCorrelated = lower(outerCorrelated),
                                              .innerProjects = lower(innerProjects),
                                              .predicate = std::move(predicate)},
                          &varTypes);
}

std::pair<SbStage, SbSlot> SbBuilder::makeHashLookup(
    const VariableTypes& varTypes,
    SbStage localStage,
    SbStage foreignStage,
    SbSlot localKeySlot,
    SbSlot foreignKeySlot,
    SbSlot foreignRecordSlot,
    SbBlockAggExpr sbBlockAggExpr,
    boost::optional<SbSlot> optOutputSlot,
    boost::optional<sbe::value::SlotId> collatorSlot) {
    auto outputSlot = optOutputSlot ? *optOutputSlot : SbSlot{_state.slotId()};

    auto result = lowerBlueprint(
        _state,
        SbBlueprintHashLookup{.localStage = addStage(_state, std::move(localStage)),
                              .foreignStage = addStage(_state, std::move(foreignStage)),
                              .nodeId = _nodeId,
                              .localKeySlot = localKeySlot.getId(),
                              .foreignKeySlot = foreignKeySlot.getId(),
                              .foreignRecordSlot = foreignRecordSlot.getId(),
                              .outSlot = outputSlot.getId(),
                              .aggExpr = std::move(sbBlockAggExpr.agg),
                              .collatorSlot = collatorSlot},
        &varTypes);
    return {std::move(result), outputSlot};
}

std::pair<SbStage, SbSlot> SbBuilder::makeHashLookupUnwind(
    const VariableTypes& varTypes,
    SbStage localStage,
    SbStage foreignStage,
    SbSlot localKeySlot,
    SbSlot foreignKeySlot,
    SbSlot foreignRecordSlot,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::JoinType joinType,
    boost::optional<sbe::value::SlotId> indexSlot) {
    auto outputSlot = SbSlot{_state.slotId()};

    auto result = lowerBlueprint(
        _state,
        SbBlueprintHashLookupUnwind{.localStage = addStage(_state, std::move(localStage)),
                                    .foreignStage = addStage(_state, std::move(foreignStage)),
                                    .nodeId = _nodeId,
                                    .joinType = joinType,
                                    .localKeySlot = localKeySlot.getId(),
                                    .foreignKeySlot = foreignKeySlot.getId(),
                                    .foreignRecordSlot = foreignRecordSlot.getId(),
                                    .outSlot = outputSlot.getId(),
                                    .collatorSlot = collatorSlot,
                                    .indexSlot = indexSlot});
    return {std::move(result), outputSlot};
}

SbStage SbBuilder::makeHashJoin(SbStage outerStage,
                                SbStage innerStage,
                                const SbSlotVector& outerCondSlots,
                                const SbSlotVector& outerProjectSlots,
                                const SbSlotVector& innerCondSlots,
                                const SbSlotVector& innerProjectSlots,
                                boost::optional<sbe::value::SlotId> collatorSlot,
                                boost::optional<size_t> estimatedBuildCardinality) {
    return lowerBlueprint(
        _state,
        SbBlueprintHashJoin{.outer = addStage(_state, std::move(outerStage)),
                            .inner = addStage(_state, std::move(innerStage)),
                            .nodeId = _nodeId,
                            .outerCondSlots = lower(outerCondSlots),
                            .outerProjectSlots = lower(outerProjectSlots),
                            .innerCondSlots = lower(innerCondSlots),
                            .innerProjectSlots = lower(innerProjectSlots),
                            .collatorSlot = collatorSlot,
                            .estimatedBuildCardinality = estimatedBuildCardinality});
}

SbStage SbBuilder::makeMergeJoin(SbStage outerStage,
                                 SbStage innerStage,
                                 const SbSlotVector& outerKeySlots,
                                 const SbSlotVector& outerProjectSlots,
                                 const SbSlotVector& innerKeySlots,
                                 const SbSlotVector& innerProjectSlots,
                                 std::vector<sbe::value::SortDirection> dirs) {
    return lowerBlueprint(_state,
                          SbBlueprintMergeJoin{.outer = addStage(_state, std::move(outerStage)),
                                               .inner = addStage(_state, std::move(innerStage)),
                                               .nodeId = _nodeId,
                                               .outerKeySlots = lower(outerKeySlots),
                                               .outerProjectSlots = lower(outerProjectSlots),
                                               .innerKeySlots = lower(innerKeySlots),
                                               .innerProjectSlots = lower(innerProjectSlots),
                                               .dirs = {dirs.begin(), dirs.end()}});
}

SbBuilder::FetchBuildResult SbBuilder::makeFetch(SbStage child,
                                                 UUID collectionUuid,
                                                 DatabaseName dbName,
                                                 SbSlot seekSlot,
                                                 std::vector<std::string> scanFieldNames,
                                                 const SbIndexInfoSlots& indexInfoSlots,
                                                 sbe::FetchCallbacks scanCallbacks) {
    auto resultSlot = SbSlot{_state.slotId()};
    auto recordIdSlot = SbSlot{_state.slotId()};

    SbSlotVector scanFieldSlots;
    scanFieldSlots.reserve(scanFieldNames.size());
    for (size_t i = 0; i < scanFieldNames.size(); ++i) {
        scanFieldSlots.emplace_back(SbSlot{_state.slotId()});
    }

    auto stage = lowerBlueprint(_state,
                                SbBlueprintFetch{.child = addStage(_state, std::move(child)),
                                                 .nodeId = _nodeId,
                                                 .collectionUuid = std::move(collectionUuid),
                                                 .dbName = std::move(dbName),
                                                 .seekSlot = seekSlot.getId(),
                                                 .indexInfoSlots = indexInfoSlots,
                                                 .scanFieldNames = std::move(scanFieldNames),
                                                 .scanCallbacks = std::move(scanCallbacks),
                                                 .recordSlot = resultSlot.getId(),
                                                 .recordIdSlot = recordIdSlot.getId(),
                                                 .scanFieldSlots = lower(scanFieldSlots)});
    return FetchBuildResult{std::move(stage), resultSlot, recordIdSlot, std::move(scanFieldSlots)};
}

SbIndexInfoSlots SbBuilder::allocateIndexInfoSlots(SbIndexInfoType indexInfoTypeMask,
                                                   const BSONObj& keyPattern) {
    SbIndexInfoSlots indexInfoSlots;

    if ((indexInfoTypeMask & SbIndexInfoType::kIndexIdent) != SbIndexInfoType::kNoInfo) {
        indexInfoSlots.indexIdentSlot = SbSlot{_state.slotId()};
    }

    if ((indexInfoTypeMask & SbIndexInfoType::kIndexKey) != SbIndexInfoType::kNoInfo) {
        indexInfoSlots.indexKeySlot = SbSlot{_state.slotId()};
    }

    if ((indexInfoTypeMask & SbIndexInfoType::kSnapshotId) != SbIndexInfoType::kNoInfo) {
        indexInfoSlots.snapshotIdSlot = SbSlot{_state.slotId()};
    }

    if ((indexInfoTypeMask & SbIndexInfoType::kIndexKeyPattern) != SbIndexInfoType::kNoInfo) {
        auto it = _state.keyPatternToSlotMap.find(keyPattern);

        if (it != _state.keyPatternToSlotMap.end()) {
            indexInfoSlots.indexKeyPatternSlot = SbSlot{it->second};
        } else {
            auto [bsonObjTag, bsonObjVal] =
                sbe::value::copyValue(sbe::value::TypeTags::bsonObject,
                                      sbe::value::bitcastFrom<const char*>(keyPattern.objdata()));
            auto slotId =
                _state.env->registerSlot(bsonObjTag, bsonObjVal, true, _state.slotIdGenerator);
            _state.keyPatternToSlotMap[keyPattern] = slotId;

            indexInfoSlots.indexKeyPatternSlot = SbSlot{slotId};
        }
    }

    return indexInfoSlots;
}

SbStage SbBuilder::makeExtractFieldPaths(SbStage child,
                                         std::vector<sbe::PathSlot> inputs,
                                         std::vector<sbe::PathSlot> outputs,
                                         PlanNodeId nodeId) {
    return lowerBlueprint(_state,
                          SbBlueprintExtractFieldPaths{.child = addStage(_state, std::move(child)),
                                                       .nodeId = nodeId,
                                                       .inputs = std::move(inputs),
                                                       .outputs = std::move(outputs)});
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
