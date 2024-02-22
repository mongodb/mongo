/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/container/inlined_vector.h>
#include <absl/meta/type_traits.h>
#include <boost/container/flat_set.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <algorithm>
#include <iterator>
#include <numeric>
#include <sstream>
#include <string_view>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/health_log_gen.h"
#include "mongo/db/catalog/health_log_interface.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/exec/docval_to_sbeval.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/branch.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/makeobj.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/traverse.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/stages/unwind.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/matcher/matcher_type_set.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_dependencies.h"
#include "mongo/db/query/bson_typemask.h"
#include "mongo/db/query/projection.h"
#include "mongo/db/query/projection_ast.h"
#include "mongo/db/query/projection_ast_path_tracking_visitor.h"
#include "mongo/db/query/projection_ast_visitor.h"
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/query/tree_walker.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_options.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/shared_buffer_fragment.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo::stage_builder {

std::unique_ptr<sbe::EExpression> makeUnaryOp(sbe::EPrimUnary::Op unaryOp,
                                              std::unique_ptr<sbe::EExpression> operand) {
    return sbe::makeE<sbe::EPrimUnary>(unaryOp, std::move(operand));
}

std::unique_ptr<sbe::EExpression> makeNot(std::unique_ptr<sbe::EExpression> e) {
    return makeUnaryOp(sbe::EPrimUnary::logicNot, std::move(e));
}

std::unique_ptr<sbe::EExpression> makeBinaryOp(sbe::EPrimBinary::Op binaryOp,
                                               std::unique_ptr<sbe::EExpression> lhs,
                                               std::unique_ptr<sbe::EExpression> rhs) {
    return sbe::makeE<sbe::EPrimBinary>(binaryOp, std::move(lhs), std::move(rhs));
}

std::unique_ptr<sbe::EExpression> makeBinaryOpWithCollation(sbe::EPrimBinary::Op binaryOp,
                                                            std::unique_ptr<sbe::EExpression> lhs,
                                                            std::unique_ptr<sbe::EExpression> rhs,
                                                            StageBuilderState& state) {
    auto collatorSlot = state.getCollatorSlot();
    auto collatorVar = collatorSlot ? sbe::makeE<sbe::EVariable>(*collatorSlot) : nullptr;

    return sbe::makeE<sbe::EPrimBinary>(
        binaryOp, std::move(lhs), std::move(rhs), std::move(collatorVar));
}

std::unique_ptr<sbe::EExpression> generateNullOrMissingExpr(const sbe::EExpression& expr) {
    return makeBinaryOp(sbe::EPrimBinary::fillEmpty,
                        makeFunction("typeMatch",
                                     expr.clone(),
                                     makeInt32Constant(getBSONTypeMask(BSONType::jstNULL))),
                        makeBoolConstant(true));
}

std::unique_ptr<sbe::EExpression> generateNullOrMissing(const sbe::EVariable& var) {
    return generateNullOrMissingExpr(var);
}

std::unique_ptr<sbe::EExpression> generateNullOrMissing(const sbe::FrameId frameId,
                                                        const sbe::value::SlotId slotId) {
    sbe::EVariable var{frameId, slotId};
    return generateNullOrMissing(var);
}

std::unique_ptr<sbe::EExpression> generateNullOrMissing(std::unique_ptr<sbe::EExpression> arg) {
    return generateNullOrMissingExpr(*arg);
}

std::unique_ptr<sbe::EExpression> generateNullMissingOrUndefinedExpr(const sbe::EExpression& expr) {
    return makeBinaryOp(sbe::EPrimBinary::fillEmpty,
                        makeFunction("typeMatch",
                                     expr.clone(),
                                     makeInt32Constant(getBSONTypeMask(BSONType::jstNULL) |
                                                       getBSONTypeMask(BSONType::Undefined))),
                        makeBoolConstant(true));
}


std::unique_ptr<sbe::EExpression> generateNullMissingOrUndefined(const sbe::EVariable& var) {
    return generateNullMissingOrUndefinedExpr(var);
}

std::unique_ptr<sbe::EExpression> generateNullMissingOrUndefined(sbe::FrameId frameId,
                                                                 sbe::value::SlotId slotId) {
    sbe::EVariable var{frameId, slotId};
    return generateNullMissingOrUndefined(var);
}

std::unique_ptr<sbe::EExpression> generateNullMissingOrUndefined(
    std::unique_ptr<sbe::EExpression> arg) {
    return generateNullMissingOrUndefinedExpr(*arg);
}

std::unique_ptr<sbe::EExpression> generateNonNumericCheck(const sbe::EVariable& var) {
    return makeNot(makeFunction("isNumber", var.clone()));
}

std::unique_ptr<sbe::EExpression> generateLongLongMinCheck(const sbe::EVariable& var) {
    return makeBinaryOp(
        sbe::EPrimBinary::logicAnd,
        makeFunction("typeMatch",
                     var.clone(),
                     makeInt32Constant(MatcherTypeSet{BSONType::NumberLong}.getBSONTypeMask())),
        makeBinaryOp(sbe::EPrimBinary::eq,
                     var.clone(),
                     makeInt64Constant(std::numeric_limits<int64_t>::min())));
}

std::unique_ptr<sbe::EExpression> makeBalancedBooleanOpTree(
    sbe::EPrimBinary::Op logicOp, std::vector<std::unique_ptr<sbe::EExpression>> leaves);

std::unique_ptr<sbe::EExpression> generateNaNCheck(const sbe::EVariable& var) {
    return makeFunction("isNaN", var.clone());
}

std::unique_ptr<sbe::EExpression> generateInfinityCheck(const sbe::EVariable& var) {
    return makeFunction("isInfinity"_sd, var.clone());
}

std::unique_ptr<sbe::EExpression> generateNonPositiveCheck(const sbe::EVariable& var) {
    return makeBinaryOp(sbe::EPrimBinary::EPrimBinary::lessEq, var.clone(), makeInt32Constant(0));
}

std::unique_ptr<sbe::EExpression> generatePositiveCheck(const sbe::EVariable& var) {
    return makeBinaryOp(sbe::EPrimBinary::EPrimBinary::greater, var.clone(), makeInt32Constant(0));
}

std::unique_ptr<sbe::EExpression> generateNegativeCheck(const sbe::EVariable& var) {
    return makeBinaryOp(sbe::EPrimBinary::EPrimBinary::less, var.clone(), makeInt32Constant(0));
}

std::unique_ptr<sbe::EExpression> generateNonObjectCheck(const sbe::EVariable& var) {
    return makeNot(makeFunction("isObject", var.clone()));
}

std::unique_ptr<sbe::EExpression> generateNonStringCheck(const sbe::EVariable& var) {
    return makeNot(makeFunction("isString", var.clone()));
}

std::unique_ptr<sbe::EExpression> generateNullishOrNotRepresentableInt32Check(
    const sbe::EVariable& var) {
    auto numericConvert32 =
        sbe::makeE<sbe::ENumericConvert>(var.clone(), sbe::value::TypeTags::NumberInt32);
    return makeBinaryOp(sbe::EPrimBinary::logicOr,
                        generateNullMissingOrUndefined(var),
                        makeNot(makeFunction("exists", std::move(numericConvert32))));
}

std::unique_ptr<sbe::EExpression> generateNonTimestampCheck(const sbe::EVariable& var) {
    return makeNot(makeFunction("isTimestamp", var.clone()));
}

template <>
std::unique_ptr<sbe::EExpression> buildMultiBranchConditional(
    std::unique_ptr<sbe::EExpression> defaultCase) {
    return defaultCase;
}

std::unique_ptr<sbe::EExpression> buildMultiBranchConditionalFromCaseValuePairs(
    std::vector<CaseValuePair> caseValuePairs, std::unique_ptr<sbe::EExpression> defaultValue) {
    return std::accumulate(
        std::make_reverse_iterator(std::make_move_iterator(caseValuePairs.end())),
        std::make_reverse_iterator(std::make_move_iterator(caseValuePairs.begin())),
        std::move(defaultValue),
        [](auto&& expression, auto&& caseValuePair) {
            return buildMultiBranchConditional(std::move(caseValuePair), std::move(expression));
        });
}

std::unique_ptr<sbe::PlanStage> makeLimitCoScanTree(PlanNodeId planNodeId, long long limit) {
    return sbe::makeS<sbe::LimitSkipStage>(
        sbe::makeS<sbe::CoScanStage>(planNodeId), makeInt64Constant(limit), nullptr, planNodeId);
}

std::unique_ptr<sbe::EExpression> makeFillEmptyFalse(std::unique_ptr<sbe::EExpression> e) {
    return makeBinaryOp(sbe::EPrimBinary::fillEmpty, std::move(e), makeBoolConstant(false));
}

std::unique_ptr<sbe::EExpression> makeFillEmptyTrue(std::unique_ptr<sbe::EExpression> e) {
    return makeBinaryOp(sbe::EPrimBinary::fillEmpty, std::move(e), makeBoolConstant(true));
}

std::unique_ptr<sbe::EExpression> makeVariable(TypedSlot ts) {
    return sbe::makeE<sbe::EVariable>(ts.slotId);
}

std::unique_ptr<sbe::EExpression> makeVariable(sbe::FrameId frameId, sbe::value::SlotId slotId) {
    return sbe::makeE<sbe::EVariable>(frameId, slotId);
}

std::unique_ptr<sbe::EExpression> makeMoveVariable(sbe::FrameId frameId,
                                                   sbe::value::SlotId slotId) {
    return sbe::makeE<sbe::EVariable>(frameId, slotId, true);
}

std::unique_ptr<sbe::EExpression> makeFillEmptyNull(std::unique_ptr<sbe::EExpression> e) {
    using namespace std::literals;
    return makeBinaryOp(sbe::EPrimBinary::fillEmpty, std::move(e), makeNullConstant());
}

std::unique_ptr<sbe::EExpression> makeFillEmptyUndefined(std::unique_ptr<sbe::EExpression> e) {
    using namespace std::literals;
    return makeBinaryOp(sbe::EPrimBinary::fillEmpty,
                        std::move(e),
                        makeConstant(sbe::value::TypeTags::bsonUndefined, 0));
}

std::unique_ptr<sbe::EExpression> makeNewBsonObject(std::vector<std::string> fields,
                                                    sbe::EExpression::Vector values) {
    tassert(7103507,
            "Expected 'fields' and 'values' to be the same size",
            fields.size() == values.size());

    std::vector<sbe::MakeObjSpec::FieldAction> fieldActions;
    for (size_t i = 0; i < fields.size(); ++i) {
        fieldActions.emplace_back(sbe::MakeObjSpec::ValueArg{i});
    }

    auto makeObjSpec =
        makeConstant(sbe::value::TypeTags::makeObjSpec,
                     sbe::value::bitcastFrom<sbe::MakeObjSpec*>(new sbe::MakeObjSpec(
                         FieldListScope::kOpen, std::move(fields), std::move(fieldActions))));
    auto makeObjRoot = makeNothingConstant();
    auto hasInputFieldsExpr = makeBoolConstant(false);
    sbe::EExpression::Vector makeObjArgs;
    makeObjArgs.reserve(3 + values.size());
    makeObjArgs.push_back(std::move(makeObjSpec));
    makeObjArgs.push_back(std::move(makeObjRoot));
    makeObjArgs.push_back(std::move(hasInputFieldsExpr));
    std::move(values.begin(), values.end(), std::back_inserter(makeObjArgs));

    return sbe::makeE<sbe::EFunction>("makeBsonObj", std::move(makeObjArgs));
}

std::unique_ptr<sbe::EExpression> makeShardKeyFunctionForPersistedDocuments(
    const std::vector<sbe::MatchPath>& shardKeyPaths,
    const std::vector<bool>& shardKeyHashed,
    const PlanStageSlots& slots) {
    // Build an expression to extract the shard key from the document based on the shard key
    // pattern. To do this, we iterate over the shard key pattern parts and build nested 'getField'
    // expressions. This will handle single-element paths, and dotted paths for each shard key part.
    std::vector<std::string> projectFields;
    sbe::EExpression::Vector projectValues;

    projectFields.reserve(shardKeyPaths.size());
    projectValues.reserve(shardKeyPaths.size());
    for (size_t i = 0; i < shardKeyPaths.size(); ++i) {
        const auto& fieldRef = shardKeyPaths[i];

        auto shardKeyBinding = sbe::makeE<sbe::EVariable>(
            slots.get(std::make_pair(PlanStageSlots::kField, fieldRef.getPart(0))).slotId);
        if (fieldRef.numParts() > 1) {
            for (size_t level = 1; level < fieldRef.numParts(); ++level) {
                shardKeyBinding = makeFunction(
                    "getField", std::move(shardKeyBinding), makeStrConstant(fieldRef[level]));
            }
        }
        shardKeyBinding = makeFillEmptyNull(std::move(shardKeyBinding));
        // If this is a hashed shard key then compute the hash value.
        if (shardKeyHashed[i]) {
            shardKeyBinding = makeFunction("shardHash"_sd, std::move(shardKeyBinding));
        }

        projectFields.push_back(fieldRef.dottedField().toString());
        projectValues.push_back(std::move(shardKeyBinding));
    }

    return makeNewBsonObject(std::move(projectFields), std::move(projectValues));
}

SbStage makeProject(SbStage stage, sbe::SlotExprPairVector projects, PlanNodeId nodeId) {
    return sbe::makeS<sbe::ProjectStage>(std::move(stage), std::move(projects), nodeId);
}

SbStage makeHashAgg(SbStage stage,
                    const sbe::value::SlotVector& gbs,
                    sbe::AggExprVector aggs,
                    boost::optional<sbe::value::SlotId> collatorSlot,
                    bool allowDiskUse,
                    sbe::SlotExprPairVector mergingExprs,
                    PlanNodeId planNodeId) {
    // In debug builds or when we explicitly set the query knob, we artificially force frequent
    // spilling. This makes sure that our tests exercise the spilling algorithm and the associated
    // logic for merging partial aggregates which otherwise would require large data sizes to
    // exercise.
    const bool forceIncreasedSpilling = allowDiskUse &&
        (kDebugBuild || internalQuerySlotBasedExecutionHashAggForceIncreasedSpilling.load());
    return sbe::makeS<sbe::HashAggStage>(std::move(stage),
                                         gbs,
                                         std::move(aggs),
                                         sbe::makeSV(),
                                         true /* optimized close */,
                                         collatorSlot,
                                         allowDiskUse,
                                         std::move(mergingExprs),
                                         planNodeId,
                                         true /* participateInTrialRunTracking */,
                                         forceIncreasedSpilling);
}

std::unique_ptr<sbe::EExpression> makeIf(std::unique_ptr<sbe::EExpression> condExpr,
                                         std::unique_ptr<sbe::EExpression> thenExpr,
                                         std::unique_ptr<sbe::EExpression> elseExpr) {
    return sbe::makeE<sbe::EIf>(std::move(condExpr), std::move(thenExpr), std::move(elseExpr));
}

std::unique_ptr<sbe::EExpression> makeLet(sbe::FrameId frameId,
                                          sbe::EExpression::Vector bindExprs,
                                          std::unique_ptr<sbe::EExpression> expr) {
    return sbe::makeE<sbe::ELocalBind>(frameId, std::move(bindExprs), std::move(expr));
}

std::unique_ptr<sbe::EExpression> makeLocalLambda(sbe::FrameId frameId,
                                                  std::unique_ptr<sbe::EExpression> expr) {
    return sbe::makeE<sbe::ELocalLambda>(frameId, std::move(expr));
}

std::unique_ptr<sbe::EExpression> makeNumericConvert(std::unique_ptr<sbe::EExpression> expr,
                                                     sbe::value::TypeTags tag) {
    return sbe::makeE<sbe::ENumericConvert>(std::move(expr), tag);
}

std::unique_ptr<sbe::EExpression> makeIfNullExpr(sbe::EExpression::Vector values,
                                                 sbe::value::FrameIdGenerator* frameIdGenerator) {
    tassert(6987503, "Expected 'values' to be non-empty", values.size() > 0);

    size_t idx = values.size() - 1;
    auto expr = std::move(values[idx]);

    while (idx > 0) {
        --idx;

        auto frameId = frameIdGenerator->generate();
        auto var = sbe::EVariable{frameId, 0};

        expr = sbe::makeE<sbe::ELocalBind>(
            frameId,
            sbe::makeEs(std::move(values[idx])),
            sbe::makeE<sbe::EIf>(
                makeNot(generateNullMissingOrUndefined(var)), var.clone(), std::move(expr)));
    }

    return expr;
}

std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>> generateVirtualScan(
    sbe::value::SlotIdGenerator* slotIdGenerator,
    sbe::value::TypeTags arrTag,
    sbe::value::Value arrVal,
    PlanYieldPolicy* yieldPolicy) {
    // The value passed in must be an array.
    invariant(sbe::value::isArray(arrTag));

    // Make an EConstant expression for the array.
    auto arrayExpression = makeConstant(arrTag, arrVal);

    // Build the unwind/project/limit/coscan subtree.
    auto projectSlot = slotIdGenerator->generate();
    auto unwindSlot = slotIdGenerator->generate();
    auto unwind = sbe::makeS<sbe::UnwindStage>(
        sbe::makeProjectStage(makeLimitCoScanTree(kEmptyPlanNodeId, 1),
                              kEmptyPlanNodeId,
                              projectSlot,
                              std::move(arrayExpression)),
        projectSlot,
        unwindSlot,
        slotIdGenerator->generate(),  // We don't need an index slot but must to provide it.
        false,                        // Don't preserve null and empty arrays.
        kEmptyPlanNodeId,
        yieldPolicy);

    // Return the UnwindStage and its output slot. The UnwindStage can be used as an input
    // to other PlanStages.
    return {unwindSlot, std::move(unwind)};
}

std::pair<sbe::value::SlotVector, std::unique_ptr<sbe::PlanStage>> generateVirtualScanMulti(
    sbe::value::SlotIdGenerator* slotIdGenerator,
    int numSlots,
    sbe::value::TypeTags arrTag,
    sbe::value::Value arrVal,
    PlanYieldPolicy* yieldPolicy) {
    using namespace std::literals;

    invariant(numSlots >= 1);

    // Generate a mock scan with a single output slot.
    auto [scanSlot, scanStage] = generateVirtualScan(slotIdGenerator, arrTag, arrVal, yieldPolicy);

    // Create a ProjectStage that will read the data from 'scanStage' and split it up
    // across multiple output slots.
    sbe::value::SlotVector projectSlots;
    sbe::SlotExprPairVector projections;
    for (int32_t i = 0; i < numSlots; ++i) {
        projectSlots.emplace_back(slotIdGenerator->generate());
        projections.emplace_back(projectSlots.back(),
                                 makeFunction("getElement"_sd,
                                              sbe::makeE<sbe::EVariable>(scanSlot),
                                              makeInt32Constant(i)));
    }

    return {std::move(projectSlots),
            sbe::makeS<sbe::ProjectStage>(
                std::move(scanStage), std::move(projections), kEmptyPlanNodeId)};
}

std::pair<sbe::value::TypeTags, sbe::value::Value> makeValue(const BSONObj& bo) {
    return sbe::value::copyValue(sbe::value::TypeTags::bsonObject,
                                 sbe::value::bitcastFrom<const char*>(bo.objdata()));
}

std::pair<sbe::value::TypeTags, sbe::value::Value> makeValue(const BSONArray& ba) {
    return sbe::value::copyValue(sbe::value::TypeTags::bsonArray,
                                 sbe::value::bitcastFrom<const char*>(ba.objdata()));
}

uint32_t dateTypeMask() {
    return (getBSONTypeMask(sbe::value::TypeTags::Date) |
            getBSONTypeMask(sbe::value::TypeTags::Timestamp) |
            getBSONTypeMask(sbe::value::TypeTags::ObjectId) |
            getBSONTypeMask(sbe::value::TypeTags::bsonObjectId));
}

/**
 * Callback function that logs a message and uasserts if it detects a corrupt index key. An index
 * key is considered corrupt if it has no corresponding Record.
 */
void indexKeyCorruptionCheckCallback(OperationContext* opCtx,
                                     sbe::value::SlotAccessor* snapshotIdAccessor,
                                     sbe::value::SlotAccessor* indexKeyAccessor,
                                     sbe::value::SlotAccessor* indexKeyPatternAccessor,
                                     const RecordId& rid,
                                     const NamespaceString& nss) {
    // Having a recordId but no record is only an issue when we are not ignoring prepare conflicts.
    if (shard_role_details::getRecoveryUnit(opCtx)->getPrepareConflictBehavior() ==
        PrepareConflictBehavior::kEnforce) {
        tassert(5113700, "Should have snapshot id accessor", snapshotIdAccessor);
        auto currentSnapshotId = shard_role_details::getRecoveryUnit(opCtx)->getSnapshotId();
        auto [snapshotIdTag, snapshotIdVal] = snapshotIdAccessor->getViewOfValue();
        const auto msgSnapshotIdTag = snapshotIdTag;
        tassert(5113701,
                str::stream() << "SnapshotId is of wrong type: " << msgSnapshotIdTag,
                snapshotIdTag == sbe::value::TypeTags::NumberInt64);
        auto snapshotId = sbe::value::bitcastTo<uint64_t>(snapshotIdVal);

        // If we have a recordId but no corresponding record, this means that said record has been
        // deleted. This can occur during yield, in which case the snapshot id would be incremented.
        // If, on the other hand, the current snapshot id matches that of the recordId, this
        // indicates an error as no yield could have taken place.
        if (snapshotId == currentSnapshotId.toNumber()) {
            tassert(5113703, "Should have index key accessor", indexKeyAccessor);
            tassert(5113704, "Should have key pattern accessor", indexKeyPatternAccessor);

            auto [ksTag, ksVal] = indexKeyAccessor->getViewOfValue();
            auto [kpTag, kpVal] = indexKeyPatternAccessor->getViewOfValue();

            const auto msgKsTag = ksTag;
            tassert(5113706,
                    str::stream() << "KeyString is of wrong type: " << msgKsTag,
                    ksTag == sbe::value::TypeTags::ksValue);

            const auto msgKpTag = kpTag;
            tassert(5113707,
                    str::stream() << "Index key pattern is of wrong type: " << msgKpTag,
                    kpTag == sbe::value::TypeTags::bsonObject);

            auto keyString = sbe::value::getKeyStringView(ksVal);
            tassert(5113708, "KeyString does not exist", keyString);

            BSONObj bsonKeyPattern(sbe::value::bitcastTo<const char*>(kpVal));
            auto bsonKeyString = key_string::toBson(*keyString, Ordering::make(bsonKeyPattern));
            auto hydratedKey = IndexKeyEntry::rehydrateKey(bsonKeyPattern, bsonKeyString);

            HealthLogEntry entry;
            entry.setNss(nss);
            entry.setTimestamp(Date_t::now());
            entry.setSeverity(SeverityEnum::Error);
            entry.setScope(ScopeEnum::Index);
            entry.setOperation("Index scan");
            entry.setMsg("Erroneous index key found with reference to non-existent record id");

            BSONObjBuilder bob;
            bob.append("recordId", rid.toString());
            bob.append("indexKeyData", hydratedKey);
            bob.appendElements(getStackTrace().getBSONRepresentation());
            entry.setData(bob.obj());

            HealthLogInterface::get(opCtx)->log(entry);

            LOGV2_ERROR_OPTIONS(
                5113709,
                {logv2::UserAssertAfterLog(ErrorCodes::DataCorruptionDetected)},
                "Erroneous index key found with reference to non-existent record id. Consider "
                "dropping and then re-creating the index and then running the validate command "
                "on the collection.",
                logAttrs(nss),
                "recordId"_attr = rid,
                "indexKeyData"_attr = hydratedKey);
        }
    }
}

/**
 * Callback function that returns true if a given index key is valid, false otherwise. An index key
 * is valid if either the snapshot id of the underlying index scan matches the current snapshot id,
 * or that the index keys are still part of the underlying index.
 */
bool indexKeyConsistencyCheckCallback(OperationContext* opCtx,
                                      StringMap<const IndexCatalogEntry*>& entryMap,
                                      sbe::value::SlotAccessor* snapshotIdAccessor,
                                      sbe::value::SlotAccessor* indexIdentAccessor,
                                      sbe::value::SlotAccessor* indexKeyAccessor,
                                      const CollectionPtr& collection,
                                      const Record& nextRecord) {
    // The index consistency check is only performed when 'snapshotIdAccessor' is set.
    if (snapshotIdAccessor) {
        auto currentSnapshotId = shard_role_details::getRecoveryUnit(opCtx)->getSnapshotId();
        auto [snapshotIdTag, snapshotIdVal] = snapshotIdAccessor->getViewOfValue();
        const auto msgSnapshotIdTag = snapshotIdTag;
        tassert(5290704,
                str::stream() << "SnapshotId is of wrong type: " << msgSnapshotIdTag,
                snapshotIdTag == sbe::value::TypeTags::NumberInt64);

        auto snapshotId = sbe::value::bitcastTo<uint64_t>(snapshotIdVal);
        if (currentSnapshotId.toNumber() != snapshotId) {
            tassert(5290707, "Should have index key accessor", indexKeyAccessor);
            tassert(5290714, "Should have index ident accessor", indexIdentAccessor);

            auto [identTag, identVal] = indexIdentAccessor->getViewOfValue();
            auto [ksTag, ksVal] = indexKeyAccessor->getViewOfValue();

            const auto msgIdentTag = identTag;
            tassert(5290708,
                    str::stream() << "Index name is of wrong type: " << msgIdentTag,
                    sbe::value::isString(identTag));

            const auto msgKsTag = ksTag;
            tassert(5290710,
                    str::stream() << "KeyString is of wrong type: " << msgKsTag,
                    ksTag == sbe::value::TypeTags::ksValue);

            auto keyString = sbe::value::getKeyStringView(ksVal);
            auto indexIdent = sbe::value::getStringView(identTag, identVal);
            tassert(5290712, "KeyString does not exist", keyString);

            auto it = entryMap.find(indexIdent);

            // If 'entryMap' doesn't contain an entry for 'indexIdent', create one.
            if (it == entryMap.end()) {
                auto indexCatalog = collection->getIndexCatalog();
                auto indexDesc = indexCatalog->findIndexByIdent(opCtx, indexIdent);
                auto entry = indexDesc ? indexDesc->getEntry() : nullptr;

                // Throw an error if we can't get the IndexDescriptor or the IndexCatalogEntry
                // (or if the index is dropped).
                uassert(ErrorCodes::QueryPlanKilled,
                        str::stream() << "query plan killed :: index dropped: " << indexIdent,
                        indexDesc && entry);

                auto [newIt, _] = entryMap.emplace(indexIdent, entry);

                it = newIt;
            }

            auto entry = it->second;
            auto iam = entry->accessMethod()->asSortedData();
            tassert(5290709,
                    str::stream() << "Expected to find SortedDataIndexAccessMethod for index: "
                                  << indexIdent,
                    iam);

            auto& executionCtx = StorageExecutionContext::get(opCtx);
            auto keys = executionCtx.keys();
            SharedBufferFragmentBuilder pooledBuilder(
                key_string::HeapBuilder::kHeapAllocatorDefaultBytes);

            // There's no need to compute the prefixes of the indexed fields that cause the
            // index to be multikey when ensuring the keyData is still valid.
            KeyStringSet* multikeyMetadataKeys = nullptr;
            MultikeyPaths* multikeyPaths = nullptr;

            iam->getKeys(opCtx,
                         collection,
                         entry,
                         pooledBuilder,
                         nextRecord.data.toBson(),
                         InsertDeleteOptions::ConstraintEnforcementMode::kEnforceConstraints,
                         SortedDataIndexAccessMethod::GetKeysContext::kValidatingKeys,
                         keys.get(),
                         multikeyMetadataKeys,
                         multikeyPaths,
                         nextRecord.id);

            return keys->count(*keyString);
        }
    }

    return true;
}

std::unique_ptr<sbe::PlanStage> makeLoopJoinForFetch(std::unique_ptr<sbe::PlanStage> inputStage,
                                                     TypedSlot resultSlot,
                                                     TypedSlot recordIdSlot,
                                                     std::vector<std::string> fields,
                                                     sbe::value::SlotVector fieldSlots,
                                                     TypedSlot seekRecordIdSlot,
                                                     TypedSlot snapshotIdSlot,
                                                     TypedSlot indexIdentSlot,
                                                     TypedSlot indexKeySlot,
                                                     TypedSlot indexKeyPatternSlot,
                                                     const CollectionPtr& collToFetch,
                                                     PlanNodeId planNodeId,
                                                     sbe::value::SlotVector slotsToForward) {
    // It is assumed that we are generating a fetch loop join over the main collection. If we are
    // generating a fetch over a secondary collection, it is the responsibility of a parent node
    // in the QSN tree to indicate which collection we are fetching over.
    tassert(6355301, "Cannot fetch from a collection that doesn't exist", collToFetch);

    sbe::ScanCallbacks callbacks(indexKeyCorruptionCheckCallback, indexKeyConsistencyCheckCallback);

    // Scan the collection in the range [seekRecordIdSlot, Inf).
    auto scanStage = sbe::makeS<sbe::ScanStage>(collToFetch->uuid(),
                                                resultSlot.slotId,
                                                recordIdSlot.slotId,
                                                snapshotIdSlot.slotId,
                                                indexIdentSlot.slotId,
                                                indexKeySlot.slotId,
                                                indexKeyPatternSlot.slotId,
                                                boost::none /* oplogTsSlot */,
                                                std::move(fields),
                                                std::move(fieldSlots),
                                                seekRecordIdSlot.slotId,
                                                boost::none /* minRecordIdSlot */,
                                                boost::none /* maxRecordIdSlot */,
                                                true /* forward */,
                                                nullptr /* yieldPolicy */,
                                                planNodeId,
                                                std::move(callbacks));

    // Get the recordIdSlot from the outer side (e.g., IXSCAN) and feed it to the inner side,
    // limiting the result set to 1 row.
    return sbe::makeS<sbe::LoopJoinStage>(
        std::move(inputStage),
        sbe::makeS<sbe::LimitSkipStage>(
            std::move(scanStage), makeInt64Constant(1), nullptr, planNodeId),
        std::move(slotsToForward),
        sbe::makeSV(seekRecordIdSlot.slotId,
                    snapshotIdSlot.slotId,
                    indexIdentSlot.slotId,
                    indexKeySlot.slotId,
                    indexKeyPatternSlot.slotId),
        nullptr /* predicate */,
        planNodeId);
}

/**
 * Given a key pattern and an array of slots of equal size, builds a SlotTreeNode representing the
 * mapping between key pattern component and slot.
 *
 * Note that this will "short circuit" in cases where the index key pattern contains two components
 * where one is a subpath of the other. For example with the key pattern {a:1, a.b: 1}, the "a.b"
 * component will not be represented in the output tree. For the purpose of rehydrating index keys,
 * this is fine (and actually preferable).
 */
std::unique_ptr<SlotTreeNode> buildKeyPatternTree(const BSONObj& keyPattern,
                                                  const sbe::value::SlotVector& slots) {
    std::vector<StringData> paths;
    for (auto&& elem : keyPattern) {
        paths.emplace_back(elem.fieldNameStringData());
    }

    return buildPathTree<boost::optional<sbe::value::SlotId>>(
        paths, slots.begin(), slots.end(), BuildPathTreeMode::RemoveConflictingPaths);
}

/**
 * Given a root SlotTreeNode, this function will construct an SBE expression for producing a partial
 * object from an index key.
 *
 * Example: Given the key pattern {a.b: 1, x: 1, a.c: 1} and the index key {"": 1, "": 2, "": 3},
 * the SBE expression returned by this function would produce the object {a: {b: 1, c: 3}, x: 2}.
 */
std::unique_ptr<sbe::EExpression> buildNewObjExpr(const SlotTreeNode* kpTree) {
    sbe::EExpression::Vector args;

    for (auto&& node : kpTree->children) {
        auto& fieldName = node->name;

        args.emplace_back(makeStrConstant(fieldName));
        if (node->value) {
            args.emplace_back(makeVariable(*node->value));
        } else {
            // The reason this is in an else branch is that in the case where we have an index key
            // like {a.b: ..., a: ...}, we've already made the logic for reconstructing the 'a'
            // portion, so the 'a.b' subtree can be skipped.
            args.push_back(buildNewObjExpr(node.get()));
        }
    }

    return sbe::makeE<sbe::EFunction>("newObj", std::move(args));
}

/**
 * Given a stage, and index key pattern a corresponding array of slot IDs, this function
 * add a ProjectStage to the tree which rehydrates the index key and stores the result in
 * 'resultSlot.'
 */
std::unique_ptr<sbe::PlanStage> rehydrateIndexKey(std::unique_ptr<sbe::PlanStage> stage,
                                                  const BSONObj& indexKeyPattern,
                                                  PlanNodeId nodeId,
                                                  const sbe::value::SlotVector& indexKeySlots,
                                                  sbe::value::SlotId resultSlot) {
    auto kpTree = buildKeyPatternTree(indexKeyPattern, indexKeySlots);
    auto keyExpr = buildNewObjExpr(kpTree.get());

    return sbe::makeProjectStage(std::move(stage), nodeId, resultSlot, std::move(keyExpr));
}

namespace {
struct GetProjectNodesData {
    projection_ast::ProjectType projectType = projection_ast::ProjectType::kInclusion;
    std::vector<std::string> paths;
    std::vector<ProjectNode> nodes;
};
using GetProjectNodesContext = projection_ast::PathTrackingVisitorContext<GetProjectNodesData>;

class GetProjectNodesVisitor final : public projection_ast::ProjectionASTConstVisitor {
public:
    explicit GetProjectNodesVisitor(GetProjectNodesContext* context) : _context{context} {}

    void visit(const projection_ast::BooleanConstantASTNode* node) final {
        bool isInclusion = _context->data().projectType == projection_ast::ProjectType::kInclusion;
        auto path = getCurrentPath();

        // For inclusion projections, if we encounter "{_id: 0}" we ignore it. Likewise, for
        // exclusion projections, if we encounter "{_id: 1}" we ignore it. ("_id" is the only
        // field that gets special treatment by the projection parser, so it's the only field
        // where this check is necessary.)
        if (isInclusion != node->value() && path == "_id") {
            return;
        }

        _context->data().paths.emplace_back(std::move(path));
        _context->data().nodes.emplace_back(node);
    }
    void visit(const projection_ast::ExpressionASTNode* node) final {
        _context->data().paths.emplace_back(getCurrentPath());
        _context->data().nodes.emplace_back(node);
    }
    void visit(const projection_ast::ProjectionSliceASTNode* node) final {
        _context->data().paths.emplace_back(getCurrentPath());
        _context->data().nodes.emplace_back(node);
    }
    void visit(const projection_ast::ProjectionPositionalASTNode* node) final {
        tasserted(7580705, "Positional projections are not supported in SBE");
    }
    void visit(const projection_ast::ProjectionElemMatchASTNode* node) final {
        tasserted(7580706, "ElemMatch projections are not supported in SBE");
    }
    void visit(const projection_ast::ProjectionPathASTNode* node) final {}
    void visit(const projection_ast::MatchExpressionASTNode* node) final {}

private:
    std::string getCurrentPath() {
        return _context->fullPath().fullPath();
    }

    GetProjectNodesContext* _context;
};
}  // namespace

std::pair<std::vector<std::string>, std::vector<ProjectNode>> getProjectNodes(
    const projection_ast::Projection& projection) {
    GetProjectNodesContext ctx{{projection.type(), {}, {}}};
    GetProjectNodesVisitor visitor(&ctx);

    projection_ast::PathTrackingConstWalker<GetProjectNodesData> walker{&ctx, {}, {&visitor}};

    tree_walker::walk<true, projection_ast::ASTNode>(projection.root(), &walker);

    return {std::move(ctx.data().paths), std::move(ctx.data().nodes)};
}

struct ProjectFieldsNodeValue {
    SbExpr expr;
    bool visited{false};
    bool incrementedDepth{false};
};

std::pair<std::unique_ptr<sbe::PlanStage>, TypedSlotVector> projectFieldsToSlots(
    std::unique_ptr<sbe::PlanStage> stage,
    const std::vector<std::string>& fields,
    TypedSlot resultSlot,
    PlanNodeId nodeId,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    StageBuilderState& state,
    const PlanStageSlots* slots) {
    // 'outputSlots' will match the order of 'fields'. Bail out early if 'fields' is empty.
    TypedSlotVector outputSlots;
    if (fields.empty()) {
        return {std::move(stage), std::move(outputSlots)};
    }

    // Handle the case where 'fields' contains only top-level fields.
    const bool topLevelFieldsOnly = std::all_of(
        fields.begin(), fields.end(), [](auto&& s) { return s.find('.') == std::string::npos; });
    if (topLevelFieldsOnly) {
        sbe::SlotExprPairVector projects;
        for (size_t i = 0; i < fields.size(); ++i) {
            auto name = std::make_pair(PlanStageSlots::kField, StringData(fields[i]));
            auto fieldSlot = slots->getIfExists(name);
            if (fieldSlot) {
                outputSlots.emplace_back(*fieldSlot);
            } else {
                auto slot = slotIdGenerator->generate();
                auto getFieldExpr = makeFunction(
                    "getField"_sd, makeVariable(resultSlot), makeStrConstant(fields[i]));
                outputSlots.emplace_back(slot);
                projects.emplace_back(slot, std::move(getFieldExpr));
            }
        }
        if (!projects.empty()) {
            stage = sbe::makeS<sbe::ProjectStage>(std::move(stage), std::move(projects), nodeId);
        }

        return {std::move(stage), std::move(outputSlots)};
    }

    // Handle the case where 'fields' contains at least one dotted path. We begin by creating a
    // path tree from 'fields'.
    using NodeValue = ProjectFieldsNodeValue;
    using Node = PathTreeNode<NodeValue>;

    auto treeRoot = buildPathTree<NodeValue>(fields, BuildPathTreeMode::AllowConflictingPaths);

    std::vector<Node*> fieldNodes;
    for (const auto& field : fields) {
        auto fieldRef = sbe::MatchPath{field};
        fieldNodes.emplace_back(treeRoot->findNode(fieldRef));
    }

    auto fieldNodesSet = absl::flat_hash_set<Node*>{fieldNodes.begin(), fieldNodes.end()};

    std::vector<Node*> roots;
    treeRoot->value.expr = resultSlot;
    treeRoot->value.visited = true;
    roots.emplace_back(treeRoot.get());

    // If 'slots' is not null, then we perform a DFS traversal over the path tree to get it set up.
    if (slots != nullptr) {
        auto hasNodesToVisit = [&](const Node::ChildrenVector& v) {
            return std::any_of(v.begin(), v.end(), [](auto&& c) { return !c->value.visited; });
        };
        auto preVisit = [&](Node* node, const std::string& path) {
            auto name = std::make_pair(PlanStageSlots::kField, StringData(path));
            // Look for a kField slot that corresponds to node's path.
            if (auto slot = slots->getIfExists(name); slot) {
                // We found a kField slot. Assign it to 'node->value.expr' and mark 'node'
                // as "visited", and add 'node' to 'roots'.
                node->value.expr = *slot;
                node->value.visited = true;
                roots.emplace_back(node);
            }
        };
        auto postVisit = [&](Node* node) {
            // When 'node' hasn't been visited and it's not in 'fieldNodesSet' and when all of
            // node's children have already been visited, mark 'node' as having been "visited".
            if (!node->value.visited && !fieldNodesSet.count(node) &&
                !hasNodesToVisit(node->children)) {
                node->value.visited = true;
            }
        };
        visitPathTreeNodes(treeRoot.get(), preVisit, postVisit);
    }

    std::vector<sbe::SlotExprPairVector> stackOfProjects;
    using DfsState = std::vector<std::pair<Node*, size_t>>;
    size_t depth = 0;

    for (auto&& root : roots) {
        // For each node in 'roots' we perform a DFS traversal, taking care to avoid visiting nodes
        // that were marked as "visited" during the previous phase.
        visitPathTreeNodes(
            root,
            [&](Node* node, const DfsState& dfs) {
                // Skip this node if 'visited' is true.
                if (node->value.visited) {
                    return false;
                }
                // visitRootNode is false, so we should be guaranteed that that there are at least
                // two entries in the DfsState: an entry for 'node' and an entry for node's parent.
                tassert(7182002, "Expected DfsState to have at least 2 entries", dfs.size() >= 2);

                auto parent = dfs[dfs.size() - 2].first;
                auto getFieldExpr = makeFunction(
                    "getField"_sd, parent->value.expr.getExpr(state), makeStrConstant(node->name));

                auto hasOneChildToVisit = [&] {
                    size_t count = 0;
                    auto it = node->children.begin();
                    for (; it != node->children.end() && count <= 1; ++it) {
                        count += !(*it)->value.visited;
                    }
                    return count == 1;
                };

                if (!fieldNodesSet.count(node) && hasOneChildToVisit()) {
                    // If 'fieldNodesSet.count(node)' is false and 'node' doesn't have multiple
                    // children that need to be visited, then we don't need to project value to
                    // a slot. Store 'getFieldExpr' into 'node->value' and return.
                    node->value.expr = std::move(getFieldExpr);
                    node->value.visited = true;
                    return true;
                }

                // We need to project 'getFieldExpr' to a slot.
                auto slot = slotIdGenerator->generate();
                node->value.expr = slot;
                node->value.visited = true;
                // Grow 'stackOfProjects' if needed so that 'stackOfProjects[depth]' is valid.
                if (depth >= stackOfProjects.size()) {
                    stackOfProjects.resize(depth + 1);
                }
                // Add the projection to the appropriate level of 'stackOfProjects'.
                auto& projects = stackOfProjects[depth];
                projects.emplace_back(slot, std::move(getFieldExpr));
                // Increment the depth while we visit node's descendents.
                ++depth;
                node->value.incrementedDepth = true;

                return true;
            },
            [&](Node* node) {
                // Now that we are done visiting node's descendents, we decrement 'depth'
                // if 'node->value.incrementedDepth' is true.
                if (node->value.incrementedDepth) {
                    --depth;
                    node->value.incrementedDepth = false;
                }
            });
    }

    // Generate a ProjectStage for each level of 'stackOfProjects'.
    for (auto&& projects : stackOfProjects) {
        if (!projects.empty()) {
            stage = sbe::makeS<sbe::ProjectStage>(std::move(stage), std::move(projects), nodeId);
        }
    }

    for (auto* node : fieldNodes) {
        outputSlots.emplace_back(node->value.expr.toSlot());
    }

    return {std::move(stage), std::move(outputSlots)};
}

ProjectionEffects::ProjectionEffects(bool isInclusion,
                                     const std::vector<std::string>& paths,
                                     const std::vector<ProjectNode>& nodes) {
    _defaultEffect = isInclusion ? kDrop : kKeep;

    // Loop over 'paths' / 'nodes'.
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        const auto& path = paths[i];
        bool isDottedPath = path.find('.') != std::string::npos;
        StringData f = getTopLevelField(path);

        if (!isDottedPath) {
            _fields.emplace_back(f.toString());

            if (node.isBool()) {
                _effects.emplace(f.toString(), isInclusion ? kKeep : kDrop);
            } else if (node.isExpr() || node.isSbExpr()) {
                _effects.emplace(f.toString(), kCreate);
            } else if (node.isSlice()) {
                _effects.emplace(f.toString(), kModify);
            }
        } else {
            auto it = _effects.find(f);
            if (it == _effects.end()) {
                _fields.emplace_back(f.toString());

                if (node.isBool() || node.isSlice()) {
                    _effects.emplace(f.toString(), kModify);
                } else if (node.isExpr() || node.isSbExpr()) {
                    _effects.emplace(f.toString(), kCreate);
                }
            } else {
                if (it->second != kCreate && (node.isExpr() || node.isSbExpr())) {
                    it->second = kCreate;
                }
            }
        }
    }
}

ProjectionEffects::ProjectionEffects(const FieldSet& keepFieldSet) {
    bool isClosed = keepFieldSet.getScope() == FieldListScope::kClosed;

    _defaultEffect = isClosed ? kDrop : kKeep;

    for (auto&& name : keepFieldSet.getList()) {
        _fields.emplace_back(name);
        _effects[name] = isClosed ? kKeep : kDrop;
    }
}

ProjectionEffects::ProjectionEffects(const FieldSet& allowedFieldSet,
                                     const FieldSet& modifiedOrCreatedFieldSet,
                                     const FieldSet& createdFieldSet,
                                     std::vector<std::string> displayOrder) {
    tassert(8238900,
            "Expected 'createdFieldSet' to be a closed FieldSet",
            createdFieldSet.getScope() == FieldListScope::kClosed);

    bool ndIsClosed = allowedFieldSet.getScope() == FieldListScope::kClosed;
    bool mocIsClosed = modifiedOrCreatedFieldSet.getScope() == FieldListScope::kClosed;

    _defaultEffect = mocIsClosed ? (ndIsClosed ? kDrop : kKeep) : kModify;

    for (auto&& name : createdFieldSet.getList()) {
        _fields.emplace_back(name);
        _effects[name] = kCreate;
    }

    for (auto&& name : modifiedOrCreatedFieldSet.getList()) {
        if (!_effects.count(name) && (mocIsClosed || allowedFieldSet.count(name))) {
            _fields.emplace_back(name);
            _effects[name] = mocIsClosed ? kModify : kKeep;
        }
    }

    for (auto&& name : allowedFieldSet.getList()) {
        if (!_effects.count(name) && !modifiedOrCreatedFieldSet.count(name)) {
            _fields.emplace_back(name);
            _effects[name] = ndIsClosed ? kKeep : kDrop;
        }
    }

    if (!mocIsClosed) {
        for (auto&& name : modifiedOrCreatedFieldSet.getList()) {
            if (!_effects.count(name)) {
                _fields.emplace_back(name);
                _effects[name] = kDrop;
            }
        }
    }

    if (!displayOrder.empty()) {
        auto fieldSet = StringDataSet(_fields.begin(), _fields.end());
        displayOrder =
            filterVector(std::move(displayOrder), [&](auto&& f) { return fieldSet.count(f) > 0; });
        _fields = appendVectorUnique(std::move(displayOrder), std::move(_fields));
    }
}

ProjectionEffects::ProjectionEffects(const FieldSet& allowedFieldSet,
                                     const std::vector<std::string>& modifiedOrCreatedFields,
                                     const std::vector<std::string>& createdFields,
                                     std::vector<std::string> displayOrder) {
    bool ndIsClosed = allowedFieldSet.getScope() == FieldListScope::kClosed;

    _defaultEffect = ndIsClosed ? kDrop : kKeep;

    for (auto&& name : createdFields) {
        _fields.emplace_back(name);
        _effects[name] = kCreate;
    }

    for (auto&& name : modifiedOrCreatedFields) {
        if (!_effects.count(name)) {
            _fields.emplace_back(name);
            _effects[name] = kModify;
        }
    }

    for (auto&& name : allowedFieldSet.getList()) {
        if (!_effects.count(name)) {
            _fields.emplace_back(name);
            _effects[name] = ndIsClosed ? kKeep : kDrop;
        }
    }

    if (!displayOrder.empty()) {
        auto fieldSet = StringDataSet(_fields.begin(), _fields.end());
        displayOrder =
            filterVector(std::move(displayOrder), [&](auto&& f) { return fieldSet.count(f) > 0; });
        _fields = appendVectorUnique(std::move(displayOrder), std::move(_fields));
    }
}

ProjectionEffects& ProjectionEffects::merge(const ProjectionEffects& other) {
    // Loop over '_fields'.
    for (const std::string& field : _fields) {
        auto it = other._effects.find(field);

        Effect& effect = _effects[field];
        Effect otherEffect = it != other._effects.end() ? it->second : other._defaultEffect;
        bool isCreate = effect == kCreate || otherEffect == kCreate;

        effect = isCreate ? kCreate : (effect != otherEffect ? kModify : effect);
    }

    // Loop over 'other._fields' and only visit fields that are not present in '_fields'.
    for (size_t i = 0; i < other._fields.size(); ++i) {
        const std::string& field = other._fields[i];

        if (!_effects.count(field)) {
            Effect effect = _defaultEffect;
            Effect otherEffect = other._effects.find(field)->second;
            bool isCreate = otherEffect == kCreate;

            _fields.emplace_back(field);
            _effects[field] = isCreate ? kCreate : (effect != otherEffect ? kModify : effect);
        }
    }

    // Update '_defaultEffect' as appropriate.
    if (_defaultEffect != other._defaultEffect) {
        _defaultEffect = kModify;
    }

    removeRedundantEffects();

    return *this;
}

ProjectionEffects& ProjectionEffects::compose(const ProjectionEffects& child) {
    // Loop over '_fields'.
    for (const std::string& field : _fields) {
        auto it = child._effects.find(field);

        Effect& effect = _effects[field];
        Effect childEffect = it != child._effects.end() ? it->second : child._defaultEffect;

        if (effect == kKeep || (effect == kModify && childEffect != kKeep)) {
            effect = childEffect;
        }
    }

    // Loop over 'child._fields' and only visit fields that are not present in '_fields'.
    for (size_t i = 0; i < child._fields.size(); ++i) {
        const std::string& field = child._fields[i];

        if (!_effects.count(field)) {
            Effect effect = _defaultEffect;
            Effect childEffect = child._effects.find(field)->second;

            Effect newEffect = effect;
            if (effect == kKeep || (effect == kModify && childEffect != kKeep)) {
                newEffect = childEffect;
            }

            _fields.emplace_back(field);
            _effects[field] = newEffect;
        }
    }

    if (_defaultEffect == kKeep || child._defaultEffect == kDrop) {
        _defaultEffect = child._defaultEffect;
    }

    removeRedundantEffects();

    return *this;
}

void ProjectionEffects::removeRedundantEffects() {
    size_t outIdx = 0;
    for (size_t idx = 0; idx < _fields.size(); ++idx) {
        auto& field = _fields[idx];

        if (_effects[field] != _defaultEffect) {
            if (outIdx != idx) {
                _fields[outIdx] = std::move(field);
            }
            ++outIdx;
        } else {
            _effects.erase(field);
        }
    }

    if (outIdx != _fields.size()) {
        _fields.resize(outIdx);
    }
}

FieldSet ProjectionEffects::getAllowedFieldSet() const {
    bool defEffectIsDrop = _defaultEffect == kDrop;
    std::vector<std::string> fields;

    for (auto&& field : _fields) {
        bool isNonDrop = !isDrop(field);
        if (isNonDrop == defEffectIsDrop) {
            fields.emplace_back(field);
        }
    }

    auto scope = defEffectIsDrop ? FieldListScope::kClosed : FieldListScope::kOpen;
    return FieldSet(std::move(fields), scope);
}

FieldSet ProjectionEffects::getModifiedOrCreatedFieldSet() const {
    bool defEffectIsKeepOrDrop = _defaultEffect != kModify;
    std::vector<std::string> fields;

    for (auto&& field : _fields) {
        bool isModifyOrCreate = isModify(field) || isCreate(field);
        if (isModifyOrCreate == defEffectIsKeepOrDrop) {
            fields.emplace_back(field);
        }
    }

    auto scope = defEffectIsKeepOrDrop ? FieldListScope::kClosed : FieldListScope::kOpen;
    return FieldSet(std::move(fields), scope);
}

FieldSet ProjectionEffects::getCreatedFieldSet() const {
    std::vector<std::string> fields;

    for (auto&& field : _fields) {
        if (isCreate(field)) {
            fields.emplace_back(field);
        }
    }

    return FieldSet::makeClosedSet(std::move(fields));
}

std::string ProjectionEffects::toString() const {
    std::stringstream ss;

    auto effectToString = [&](Effect e) -> StringData {
        switch (e) {
            case kKeep:
                return "Keep"_sd;
            case kDrop:
                return "Drop"_sd;
            case kModify:
                return "Modify"_sd;
            case kCreate:
                return "Create"_sd;
            default:
                return ""_sd;
        }
    };

    ss << "{";

    bool first = true;
    for (auto&& field : _fields) {
        if (!first) {
            ss << ", ";
        }
        first = false;

        auto effect = _effects.find(field)->second;
        ss << field << " : " << effectToString(effect);
    }

    ss << (!first ? ", * : " : "* : ") << effectToString(_defaultEffect) << "}";

    return ss.str();
}

FieldSet makeAllowedFieldSet(bool isInclusion,
                             const std::vector<std::string>& paths,
                             const std::vector<ProjectNode>& nodes) {
    // For inclusion projections, we make a list of the top-level fields referenced by the
    // projection and make a closed FieldSet.
    if (isInclusion) {
        std::vector<std::string> fields;
        StringSet fieldSet;
        for (size_t i = 0; i < paths.size(); ++i) {
            const auto& path = paths[i];
            auto field = getTopLevelField(path).toString();

            auto [_, inserted] = fieldSet.insert(field);
            if (inserted) {
                fields.emplace_back(field);
            }
        }

        return FieldSet::makeClosedSet(std::move(fields));
    }

    // For exclusion projections, we build a list of the top-level fields that are dropped by this
    // projection, and then we use that list to make an open FieldSet that represents the set of
    // fields _not_ dropped by this projection.
    std::vector<std::string> fields;
    StringSet fieldSet;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        const auto& path = paths[i];

        if (node.isBool() && path.find('.') == std::string::npos) {
            auto [_, inserted] = fieldSet.insert(path);
            if (inserted) {
                fields.emplace_back(path);
            }
        }
    }

    return FieldSet::makeOpenSet(std::move(fields));
}

FieldSet makeModifiedOrCreatedFieldSet(bool isInclusion,
                                       const std::vector<std::string>& paths,
                                       const std::vector<ProjectNode>& nodes) {
    std::vector<std::string> fields;
    StringSet fieldSet;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        const auto& path = paths[i];
        bool isTopLevelField = path.find('.') == std::string::npos;

        if (node.isBool() && isTopLevelField) {
            continue;
        }

        if (node.isBool() || node.isExpr() || node.isSbExpr() || node.isSlice()) {
            auto field = getTopLevelField(path).toString();
            auto [_, inserted] = fieldSet.insert(field);

            if (inserted) {
                fields.emplace_back(field);
            }
        }
    }

    return FieldSet::makeClosedSet(std::move(fields));
}

FieldSet makeCreatedFieldSet(bool isInclusion,
                             const std::vector<std::string>& paths,
                             const std::vector<ProjectNode>& nodes) {
    std::vector<std::string> fields;
    StringSet fieldSet;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        const auto& path = paths[i];

        if (node.isExpr() || node.isSbExpr()) {
            auto field = getTopLevelField(path).toString();
            auto [_, inserted] = fieldSet.insert(field);

            if (inserted) {
                fields.emplace_back(field);
            }
        }
    }

    return FieldSet::makeClosedSet(std::move(fields));
}

}  // namespace mongo::stage_builder
