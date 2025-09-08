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
#include <boost/container/flat_set.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index/preallocated_container_pool.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/window_function/window_function_top_bottom_n.h"
#include "mongo/db/query/compiler/logical_model/projection/projection.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_ast.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_ast_path_tracking_visitor.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_ast_visitor.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr_helpers.h"
#include "mongo/db/query/tree_walker.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/logv2/log.h"
#include "mongo/util/shared_buffer_fragment.h"
#include "mongo/util/str.h"

#include <algorithm>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::stage_builder {

SbExpr makeShardKeyForPersistedDocuments(StageBuilderState& state,
                                         const std::vector<sbe::MatchPath>& shardKeyPaths,
                                         const std::vector<bool>& shardKeyHashed,
                                         const PlanStageSlots& slots) {
    SbExprBuilder b(state);

    // Build an expression to extract the shard key from the document based on the shard key
    // pattern. To do this, we iterate over the shard key pattern parts and build nested 'getField'
    // expressions. This will handle single-element paths, and dotted paths for each shard key part.
    SbExpr::Vector newBsonObjArgs;
    newBsonObjArgs.reserve(shardKeyPaths.size() * 2);

    for (size_t i = 0; i < shardKeyPaths.size(); ++i) {
        const auto& fieldRef = shardKeyPaths[i];

        SbExpr shardKeyBinding =
            SbExpr{slots.get(std::make_pair(PlanStageSlots::kField, fieldRef.getPart(0)))};
        if (fieldRef.numParts() > 1) {
            for (size_t level = 1; level < fieldRef.numParts(); ++level) {
                shardKeyBinding = b.makeFunction(
                    "getField", std::move(shardKeyBinding), b.makeStrConstant(fieldRef[level]));
            }
        }
        shardKeyBinding = b.makeFillEmptyNull(std::move(shardKeyBinding));
        // If this is a hashed shard key then compute the hash value.
        if (shardKeyHashed[i]) {
            shardKeyBinding = b.makeFunction("shardHash"_sd, std::move(shardKeyBinding));
        }

        newBsonObjArgs.emplace_back(b.makeStrConstant(fieldRef.dottedField()));
        newBsonObjArgs.emplace_back(std::move(shardKeyBinding));
    }

    return b.makeFunction("newBsonObj"_sd, std::move(newBsonObjArgs));
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
                    ksTag == sbe::value::TypeTags::keyString);

            const auto msgKpTag = kpTag;
            tassert(5113707,
                    str::stream() << "Index key pattern is of wrong type: " << msgKpTag,
                    kpTag == sbe::value::TypeTags::bsonObject);

            auto keyString = sbe::value::getKeyString(ksVal);
            tassert(5113708, "KeyString does not exist", keyString);

            BSONObj bsonKeyPattern(sbe::value::bitcastTo<const char*>(kpVal));
            auto bsonKeyString = key_string::toBson(keyString->getKeyStringView(),
                                                    Ordering::make(bsonKeyPattern),
                                                    keyString->getTypeBitsView(),
                                                    keyString->getVersion());
            auto hydratedKey = IndexKeyEntry::rehydrateKey(bsonKeyPattern, bsonKeyString);

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
                    ksTag == sbe::value::TypeTags::keyString);

            auto keyString = sbe::value::getKeyString(ksVal);
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

            auto& containerPool = PreallocatedContainerPool::get(opCtx);
            auto keys = containerPool.keys();
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

            return keys->count(keyString->getValueCopy());
        }
    }

    return true;
}

std::tuple<SbStage, SbSlot, SbSlot, SbSlotVector> makeLoopJoinForFetch(
    SbStage inputStage,
    std::vector<std::string> fields,
    SbSlot seekRecordIdSlot,
    SbSlot snapshotIdSlot,
    SbSlot indexIdentSlot,
    SbSlot indexKeySlot,
    SbSlot indexKeyPatternSlot,
    boost::optional<SbSlot> prefetchedResultSlot,
    const CollectionPtr& collToFetch,
    StageBuilderState& state,
    PlanNodeId planNodeId,
    SbSlotVector slotsToForward) {
    SbBuilder b(state, planNodeId);

    // It is assumed that we are generating a fetch loop join over the main collection. If we are
    // generating a fetch over a secondary collection, it is the responsibility of a parent node
    // in the QSN tree to indicate which collection we are fetching over.
    tassert(6355301, "Cannot fetch from a collection that doesn't exist", collToFetch);

    sbe::ScanCallbacks callbacks(indexKeyCorruptionCheckCallback, indexKeyConsistencyCheckCallback);

    SbIndexInfoSlots indexInfoSlots;
    indexInfoSlots.indexIdentSlot = indexIdentSlot;
    indexInfoSlots.indexKeySlot = indexKeySlot;
    indexInfoSlots.indexKeyPatternSlot = indexKeyPatternSlot;
    indexInfoSlots.snapshotIdSlot = snapshotIdSlot;

    // Create a limit-1/scan subtree to perform the seek.
    auto [scanStage, resultSlot, recordIdSlot, fieldSlots] = b.makeScan(collToFetch->uuid(),
                                                                        collToFetch->ns().dbName(),
                                                                        true /* forward */,
                                                                        seekRecordIdSlot,
                                                                        fields,
                                                                        {} /* scanBounds */,
                                                                        std::move(indexInfoSlots),
                                                                        std::move(callbacks));

    auto seekStage = b.makeLimit(std::move(scanStage), b.makeInt64Constant(1));

    if (prefetchedResultSlot) {
        // Prepare to create a BranchStage, with one branch performing retrieving the result object
        // by performing a seek, and with the other branch getting the result object by moving it
        // from the kPrefetchedResult slot. The resulting subtree will look something like this:
        //
        //   branch {!exists(prefetchedResultSlot)} [resultSlot, recordIdSlot]
        //     [thenResultSlot, thenRecordIdSlot]
        //       limit 1
        //       seek seekRecordId thenResultSlot thenRecordIdSlot snapshotIdSlot indexIdentSlot
        //              indexKeySlot indexKeyPatternSlot none none [] @collUuid true false
        //     [elseResultSlot, elseRecordIdSlot]
        //       project [elseResultSlot = prefetchedResultSlot, elseRecordIdSlot = seekRecordId]
        //       limit 1
        //       coscan

        // Move 'resultSlot', 'recordIdSlot', and 'fieldSlots' to new variables
        // 'thenResultSlot', 'thenRecordIdSlot', and 'thenFieldSlots', respectively.
        SbSlot thenResultSlot = resultSlot;
        SbSlot thenRecordIdSlot = recordIdSlot;
        SbSlotVector thenFieldSlots = std::move(fieldSlots);
        fieldSlots = SbSlotVector{};

        // Move 'seekStage' into 'thenStage'. We will re-initialize 'seekStage' later.
        auto thenStage = std::move(seekStage);
        seekStage = {};

        // Allocate new slots for the result and record ID produced by the else branch.
        auto elseResultSlot = SbSlot{state.slotId()};
        auto elseRecordIdSlot = SbSlot{state.slotId()};

        // Create a project/limit-1/coscan subtree for the else branch.
        auto [elseStage, _] =
            b.makeProject(b.makeLimitOneCoScanTree(),
                          std::pair(SbExpr{*prefetchedResultSlot}, elseResultSlot),
                          std::pair(SbExpr{seekRecordIdSlot}, elseRecordIdSlot));

        // Project the fields to slots for the else branch.
        auto [projectStage, elseFieldSlots] = projectFieldsToSlots(
            std::move(elseStage), fields, elseResultSlot, planNodeId, state.slotIdGenerator, state);
        elseStage = std::move(projectStage);

        auto conditionExpr = b.makeNot(b.makeFunction("exists", *prefetchedResultSlot));

        auto thenOutputSlots = SbExpr::makeSV(thenResultSlot, thenRecordIdSlot);
        thenOutputSlots.insert(thenOutputSlots.end(), thenFieldSlots.begin(), thenFieldSlots.end());

        auto elseOutputSlots = SbExpr::makeSV(elseResultSlot, elseRecordIdSlot);
        elseOutputSlots.insert(elseOutputSlots.end(), elseFieldSlots.begin(), elseFieldSlots.end());

        // Create a BranchStage that combines 'thenStage' and 'elseStage' and store it in
        // 'seekStage'.
        auto [outStage, outputSlots] = b.makeBranch(std::move(thenStage),
                                                    std::move(elseStage),
                                                    std::move(conditionExpr),
                                                    thenOutputSlots,
                                                    elseOutputSlots);
        seekStage = std::move(outStage);

        // Set 'resultSlot' and 'recordIdSlot' to point to newly allocated slots. Also,
        // clear 'fieldSlots' and then fill it with newly allocated slots for each field.
        resultSlot = outputSlots[0];
        recordIdSlot = outputSlots[1];
        for (size_t i = 2; i < outputSlots.size(); ++i) {
            fieldSlots.emplace_back(outputSlots[i]);
        }
    }

    auto correlatedSlots = SbExpr::makeSV(
        seekRecordIdSlot, snapshotIdSlot, indexIdentSlot, indexKeySlot, indexKeyPatternSlot);
    if (prefetchedResultSlot) {
        correlatedSlots.emplace_back(*prefetchedResultSlot);
    }

    // Create a LoopJoinStage that combines 'inputStage' and 'seekStage'.
    auto loopJoinStage = b.makeLoopJoin(
        std::move(inputStage), std::move(seekStage), slotsToForward, correlatedSlots);

    return {std::move(loopJoinStage), resultSlot, recordIdSlot, std::move(fieldSlots)};
}

namespace {
/**
 * Given a field path, this function will return an expression that will be true if evaluating the
 * field path involves array traversal at any level of the path (including the leaf field).
 */
SbExpr generateArrayCheckForSort(StageBuilderState& state,
                                 SbExpr inputExpr,
                                 const FieldPath& fp,
                                 FieldIndex level,
                                 sbe::value::FrameIdGenerator* frameIdGenerator,
                                 boost::optional<SbSlot> fieldSlot = boost::none) {
    invariant(level < fp.getPathLength());

    tassert(8102000,
            "Expected either 'inputExpr' or 'fieldSlot' to be defined",
            !inputExpr.isNull() || fieldSlot.has_value());

    SbExprBuilder b(state);
    auto resultExpr = [&] {
        auto fieldExpr = fieldSlot ? SbExpr{*fieldSlot}
                                   : b.makeFunction("getField"_sd,
                                                    std::move(inputExpr),
                                                    b.makeStrConstant(fp.getFieldName(level)));
        if (level == fp.getPathLength() - 1u) {
            return b.makeFunction("isArray"_sd, std::move(fieldExpr));
        }
        sbe::FrameId frameId = frameIdGenerator->generate();
        return b.makeLet(
            frameId,
            SbExpr::makeSeq(std::move(fieldExpr)),
            b.makeBooleanOpTree(abt::Operations::Or,
                                b.makeFunction("isArray"_sd, SbVar{frameId, 0}),
                                generateArrayCheckForSort(
                                    state, SbVar{frameId, 0}, fp, level + 1, frameIdGenerator)));
    }();

    if (level == 0) {
        resultExpr = b.makeFillEmptyFalse(std::move(resultExpr));
    }

    return resultExpr;
}

/**
 * Given a field path, this function recursively builds an expression tree that will produce the
 * corresponding sort key for that path.
 */
SbExpr generateSortTraverse(boost::optional<SbVar> inputVar,
                            bool isAscending,
                            const FieldPath& fp,
                            size_t level,
                            StageBuilderState& state,
                            boost::optional<SbSlot> fieldSlot = boost::none) {
    using namespace std::literals;

    invariant(level < fp.getPathLength());

    tassert(8102001,
            "Expected either 'inputVar' or 'fieldSlot' to be defined",
            inputVar || fieldSlot.has_value());

    SbExprBuilder b(state);

    auto collatorSlot = state.getCollatorSlot();

    // Generate an expression to read a sub-field at the current nested level.
    auto fieldExpr = fieldSlot ? SbExpr{*fieldSlot}
                               : b.makeFunction("getField"_sd,
                                                std::move(inputVar),
                                                b.makeStrConstant(fp.getFieldName(level)));

    if (level == fp.getPathLength() - 1) {
        // For the last level, we can just return the field slot without the need for a
        // traverse expression.
        auto frameId =
            fieldSlot ? boost::optional<sbe::FrameId>{} : boost::make_optional(state.frameId());
        auto var = fieldSlot ? fieldExpr.clone() : SbExpr{SbVar{*frameId, 0}};

        auto helperArgs = SbExpr::makeSeq(std::move(var));
        if (collatorSlot) {
            helperArgs.emplace_back(SbExpr{SbVar{*collatorSlot}});
        }

        StringData helperFn = isAscending ? "getSortKeyAsc"_sd : "getSortKeyDesc"_sd;

        auto resultExpr = b.makeFunction(helperFn, std::move(helperArgs));

        if (!fieldSlot) {
            resultExpr =
                b.makeLet(*frameId, SbExpr::makeSeq(std::move(fieldExpr)), std::move(resultExpr));
        }
        return resultExpr;
    }

    // Prepare a lambda expression that will navigate to the next component of the field path.
    auto lambdaFrameId = state.frameId();
    auto lambdaExpr = b.makeLocalLambda(
        lambdaFrameId,
        generateSortTraverse(SbVar{lambdaFrameId, 0}, isAscending, fp, level + 1, state));

    // Generate the traverse expression for the current nested level.
    // Be sure to invoke the least/greatest fold expression only if the current nested level is an
    // array.
    auto frameId = state.frameId();
    auto var = fieldSlot ? SbExpr{*fieldSlot} : SbExpr{SbVar{frameId, 0}};
    auto resultVar = SbExpr{SbVar{frameId, fieldSlot ? 0 : 1}};

    SbExpr::Vector binds;
    if (!fieldSlot) {
        binds.emplace_back(std::move(fieldExpr));
    }
    binds.emplace_back(b.makeFunction(
        "traverseP", var.clone(), std::move(lambdaExpr), b.makeInt32Constant(1) /* maxDepth */));

    auto helperArgs = SbExpr::makeSeq(resultVar.clone());
    if (collatorSlot) {
        helperArgs.emplace_back(SbExpr{SbVar{*collatorSlot}});
    }

    // According to MQL's sorting semantics, when a non-leaf field is an empty array or does not
    // exist we should use Null as the sort key.
    StringData helperFn = isAscending ? "getNonLeafSortKeyAsc"_sd : "getNonLeafSortKeyDesc"_sd;

    return b.makeLet(frameId,
                     std::move(binds),
                     b.makeIf(b.makeFillEmptyFalse(b.makeFunction("isArray"_sd, std::move(var))),
                              b.makeFunction(helperFn, std::move(helperArgs)),
                              b.makeFillEmptyNull(std::move(resultVar))));
}
}  // namespace

BuildSortKeysPlan makeSortKeysPlan(const SortPattern& sortPattern, bool allowCallGenCheapSortKey) {
    BuildSortKeysPlan plan;

    const bool hasPartsWithCommonPrefix = sortPatternHasPartsWithCommonPrefix(sortPattern);
    const bool sortPatternIsEmpty = sortPattern.size() == 0;

    if (!hasPartsWithCommonPrefix && !sortPatternIsEmpty) {
        DepsTracker deps;
        sortPattern.addDependencies(&deps);

        if (!deps.needWholeDocument) {
            // If the sort pattern doesn't need the whole document and there are no common
            // prefixes, then we set 'type' to kTraverseFields, we set 'needsResultObj' to
            // false, and we take all the top-level fields referenced by the sort pattern and
            // we add them to 'fieldsForSortKeys'.
            plan.type = BuildSortKeysPlan::kTraverseFields;
            plan.needsResultObj = false;
            plan.fieldsForSortKeys = getTopLevelFields(deps.fields);

            return plan;
        }
    }

    // Otherwise, we set 'type' to either kCallGenSortKey or kCallGenCheapSortKey (depending
    // on 'allowCallGenCheapSortKey') and we set 'needsResultObj' to true and return.
    plan.type = allowCallGenCheapSortKey ? BuildSortKeysPlan::kCallGenCheapSortKey
                                         : BuildSortKeysPlan::kCallGenSortKey;
    plan.needsResultObj = true;

    return plan;
}

// Should we pass in a sortPattern here, or pass in a sortSpec instead?
SortKeysExprs buildSortKeys(StageBuilderState& state,
                            const BuildSortKeysPlan& plan,
                            const SortPattern& sortPattern,
                            const PlanStageSlots& outputs,
                            SbExpr sortSpecExpr) {
    SbExprBuilder b(state);

    auto collatorSlot = state.getCollatorSlot();

    SortKeysExprs sortKeysExprs;

    if (plan.type == BuildSortKeysPlan::kTraverseFields) {
        // Sorting has a limitation where only one of the sort patterns can involve arrays.
        // If there are at least two sort patterns, check the data for this possibility.
        sortKeysExprs.parallelArraysCheckExpr = [&]() -> SbExpr {
            if (sortPattern.size() < 2) {
                // If the sort pattern only has one part, we don't need to generate a "parallel
                // arrays" check.
                return {};
            } else if (sortPattern.size() == 2) {
                // If the sort pattern has two parts, we can generate a simpler expression to
                // perform the "parallel arrays" check.
                auto makeIsNotArrayCheck = [&](const FieldPath& fp) {
                    return b.makeNot(generateArrayCheckForSort(
                        state,
                        SbExpr{},
                        fp,
                        0 /* level */,
                        state.frameIdGenerator,
                        outputs.getIfExists(
                            std::make_pair(PlanStageSlots::kField, fp.getFieldName(0)))));
                };

                return b.makeBooleanOpTree(abt::Operations::Or,
                                           makeIsNotArrayCheck(*sortPattern[0].fieldPath),
                                           makeIsNotArrayCheck(*sortPattern[1].fieldPath));
            } else {
                // If the sort pattern has three or more parts, we generate an expression to
                // perform the "parallel arrays" check that works (and scales well) for an
                // arbitrary number of sort pattern parts.
                auto makeIsArrayCheck = [&](const FieldPath& fp) {
                    return b.makeBinaryOp(
                        abt::Operations::Cmp3w,
                        generateArrayCheckForSort(state,
                                                  SbExpr{},
                                                  fp,
                                                  0,
                                                  state.frameIdGenerator,
                                                  outputs.getIfExists(std::make_pair(
                                                      PlanStageSlots::kField, fp.getFieldName(0)))),
                        b.makeBoolConstant(false));
                };

                SbExpr::Vector args;
                for (size_t idx = 0; idx < sortPattern.size(); ++idx) {
                    args.emplace_back(makeIsArrayCheck(*sortPattern[idx].fieldPath));
                }

                auto numArraysExpr = b.makeNaryOp(abt::Operations::Add, std::move(args));

                return b.makeBinaryOp(
                    abt::Operations::Lte, std::move(numArraysExpr), b.makeInt32Constant(1));
            }
        }();

        for (const auto& part : sortPattern) {
            auto topLevelFieldSlot = outputs.get(
                std::make_pair(PlanStageSlots::kField, part.fieldPath->getFieldName(0)));

            SbExpr sortKeyExpr = generateSortTraverse(
                boost::none, part.isAscending, *part.fieldPath, 0, state, topLevelFieldSlot);

            // Apply the transformation required by the collation, if specified.
            if (collatorSlot) {
                sortKeyExpr = b.makeFunction(
                    "collComparisonKey"_sd, std::move(sortKeyExpr), SbExpr{SbVar{*collatorSlot}});
            }

            sortKeysExprs.keyExprs.emplace_back(std::move(sortKeyExpr));
        }
    } else if (plan.type == BuildSortKeysPlan::kCallGenSortKey ||
               plan.type == BuildSortKeysPlan::kCallGenCheapSortKey) {
        // generateSortKey() will handle the parallel arrays check and sort key traversal for us,
        // so we don't need to generate our own sort key traversal logic in the SBE plan.
        const SbSlot childResultSlotId = outputs.getResultObj();

        StringData generateSortKeyFnName = plan.type == BuildSortKeysPlan::kCallGenSortKey
            ? "generateSortKey"
            : "generateCheapSortKey";

        if (!sortSpecExpr) {
            auto sortSpec = makeSortSpecFromSortPattern(sortPattern);
            sortSpecExpr =
                b.makeConstant(sbe::value::TypeTags::sortSpec,
                               sbe::value::bitcastFrom<sbe::SortSpec*>(sortSpec.release()));
        }

        // generateSortKey() will handle the parallel arrays check and sort key traversal for us,
        // so we don't need to generate our own sort key traversal logic in the SBE plan.
        sortKeysExprs.fullKeyExpr = collatorSlot
            ? b.makeFunction(generateSortKeyFnName,
                             std::move(sortSpecExpr),
                             childResultSlotId,
                             SbVar{*collatorSlot})
            : b.makeFunction(generateSortKeyFnName, std::move(sortSpecExpr), childResultSlotId);
    } else {
        MONGO_UNREACHABLE;
    }

    return sortKeysExprs;
}

boost::optional<UnfetchedIxscans> getUnfetchedIxscans(const QuerySolutionNode* root) {
    using DfsItem = std::pair<const QuerySolutionNode*, size_t>;

    absl::InlinedVector<DfsItem, 64> dfs;
    std::vector<const QuerySolutionNode*> ixscans;
    bool hasFetchesOrCollScans = false;

    for (auto&& child : root->children) {
        dfs.emplace_back(DfsItem(child.get(), 0));
    }

    while (!dfs.empty()) {
        auto& dfsBack = dfs.back();
        auto node = dfsBack.first;
        auto childIdx = dfsBack.second;
        bool popDfs = true;

        auto visitNextChild = [&] {
            if (childIdx < node->children.size()) {
                popDfs = false;
                dfsBack.second++;
                dfs.emplace_back(DfsItem(node->children[childIdx].get(), 0));
                return true;
            }
            return false;
        };

        switch (node->getType()) {
            case STAGE_IXSCAN: {
                ixscans.push_back(node);
                break;
            }
            case STAGE_LIMIT:
            case STAGE_SKIP:
            case STAGE_MATCH:
            case STAGE_SHARDING_FILTER:
            case STAGE_SORT_SIMPLE:
            case STAGE_SORT_DEFAULT:
            case STAGE_OR:
            case STAGE_SORT_MERGE: {
                visitNextChild();
                break;
            }
            case STAGE_COLLSCAN:
            case STAGE_VIRTUAL_SCAN:
            case STAGE_FETCH: {
                hasFetchesOrCollScans = true;
                break;
            }
            default: {
                return boost::none;
            }
        }

        if (popDfs) {
            dfs.pop_back();
        }
    }

    return {UnfetchedIxscans{std::move(ixscans), hasFetchesOrCollScans}};
}

bool isAccumulatorN(StringData name) {
    return name == AccumulatorTop::getName() || name == AccumulatorBottom::getName() ||
        name == AccumulatorTopN::getName() || name == AccumulatorBottomN::getName() ||
        name == AccumulatorMinN::getName() || name == AccumulatorMaxN::getName() ||
        name == AccumulatorFirstN::getName() || name == AccumulatorLastN::getName();
}

bool isTopBottomN(StringData name) {
    return name == AccumulatorTop::getName() || name == AccumulatorBottom::getName() ||
        name == AccumulatorTopN::getName() || name == AccumulatorBottomN::getName();
}

StringData getAccumulationOpName(const AccumulationStatement& accStmt) {
    return accStmt.expr.name;
}

StringData getWindowFunctionOpName(const WindowFunctionStatement& wfStmt) {
    return wfStmt.expr->getOpName();
}

bool isAccumulatorN(const AccumulationStatement& accStmt) {
    return isAccumulatorN(getAccumulationOpName(accStmt));
}

bool isAccumulatorN(const WindowFunctionStatement& wfStmt) {
    return isAccumulatorN(getWindowFunctionOpName(wfStmt));
}

bool isTopBottomN(const AccumulationStatement& accStmt) {
    return isTopBottomN(getAccumulationOpName(accStmt));
}

bool isTopBottomN(const WindowFunctionStatement& wfStmt) {
    return isTopBottomN(getWindowFunctionOpName(wfStmt));
}

boost::optional<SortPattern> getSortPattern(const AccumulationStatement& accStmt) {
    if (isTopBottomN(accStmt)) {
        auto acc = accStmt.expr.factory();

        if (accStmt.expr.name == AccumulatorTop::getName()) {
            return dynamic_cast<AccumulatorTop*>(acc.get())->getSortPattern();
        }
        if (accStmt.expr.name == AccumulatorBottom::getName()) {
            return dynamic_cast<AccumulatorBottom*>(acc.get())->getSortPattern();
        }
        if (accStmt.expr.name == AccumulatorTopN::getName()) {
            return dynamic_cast<AccumulatorTopN*>(acc.get())->getSortPattern();
        }
        if (accStmt.expr.name == AccumulatorBottomN::getName()) {
            return dynamic_cast<AccumulatorBottomN*>(acc.get())->getSortPattern();
        }
    }
    return {};
}

boost::optional<SortPattern> getSortPattern(const WindowFunctionStatement& wfStmt) {
    using TopExpr = window_function::ExpressionN<WindowFunctionTop, AccumulatorTop>;
    using BottomExpr = window_function::ExpressionN<WindowFunctionBottom, AccumulatorBottom>;
    using TopNExpr = window_function::ExpressionN<WindowFunctionTopN, AccumulatorTopN>;
    using BottomNExpr = window_function::ExpressionN<WindowFunctionBottomN, AccumulatorBottomN>;

    if (wfStmt.expr->getOpName() == AccumulatorTop::getName()) {
        return *dynamic_cast<TopExpr*>(wfStmt.expr.get())->sortPattern;
    }
    if (wfStmt.expr->getOpName() == AccumulatorBottom::getName()) {
        return *dynamic_cast<BottomExpr*>(wfStmt.expr.get())->sortPattern;
    }
    if (wfStmt.expr->getOpName() == AccumulatorTopN::getName()) {
        return *dynamic_cast<TopNExpr*>(wfStmt.expr.get())->sortPattern;
    }
    if (wfStmt.expr->getOpName() == AccumulatorBottomN::getName()) {
        return *dynamic_cast<BottomNExpr*>(wfStmt.expr.get())->sortPattern;
    }
    return {};
}

std::unique_ptr<sbe::SortSpec> makeSortSpecFromSortPattern(const SortPattern& sortPattern) {
    return std::make_unique<sbe::SortSpec>(
        sortPattern.serialize(SortPattern::SortKeySerialization::kForExplain).toBson());
}

std::unique_ptr<sbe::SortSpec> makeSortSpecFromSortPattern(
    const boost::optional<SortPattern>& sortPattern) {
    return sortPattern ? makeSortSpecFromSortPattern(*sortPattern)
                       : std::unique_ptr<sbe::SortSpec>{};
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
                                                  const SbSlotVector& slots) {
    std::vector<StringData> paths;
    for (auto&& elem : keyPattern) {
        paths.emplace_back(elem.fieldNameStringData());
    }

    return buildPathTree<boost::optional<SbSlot>>(
        paths, slots.begin(), slots.end(), BuildPathTreeMode::RemoveConflictingPaths);
}

/**
 * Given a root SlotTreeNode, this function will construct an SBE expression for producing a partial
 * object from an index key.
 *
 * Example: Given the key pattern {a.b: 1, x: 1, a.c: 1} and the index key {"": 1, "": 2, "": 3},
 * the SBE expression returned by this function would produce the object {a: {b: 1, c: 3}, x: 2}.
 */
SbExpr buildNewObjExpr(StageBuilderState& state, const SlotTreeNode* kpTree) {
    SbExprBuilder b(state);

    SbExpr::Vector args;
    for (auto&& node : kpTree->children) {
        auto& fieldName = node->name;

        args.emplace_back(b.makeStrConstant(fieldName));
        if (node->value) {
            args.emplace_back(*node->value);
        } else {
            // The reason this is in an else branch is that in the case where we have an index key
            // like {a.b: ..., a: ...}, we've already made the logic for reconstructing the 'a'
            // portion, so the 'a.b' subtree can be skipped.
            args.push_back(buildNewObjExpr(state, node.get()));
        }
    }

    return b.makeFunction("newObj", std::move(args));
}

/**
 * Given a stage, and index key pattern a corresponding array of slot IDs, this function
 * add a ProjectStage to the tree which rehydrates the index key and stores the result in
 * 'resultSlot.'
 */
SbExpr rehydrateIndexKey(StageBuilderState& state,
                         const BSONObj& indexKeyPattern,
                         const SbSlotVector& indexKeySlots) {
    auto kpTree = buildKeyPatternTree(indexKeyPattern, indexKeySlots);
    return buildNewObjExpr(state, kpTree.get());
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

std::vector<ProjectNode> cloneProjectNodes(const std::vector<ProjectNode>& nodes) {
    std::vector<ProjectNode> clonedNodes;
    clonedNodes.reserve(nodes.size());
    for (const auto& node : nodes) {
        clonedNodes.emplace_back(node.clone());
    }

    return clonedNodes;
}

struct ProjectFieldsNodeValue {
    SbExpr expr;
    bool visited{false};
    bool incrementedDepth{false};
};

std::pair<SbStage, SbSlotVector> projectFieldsToSlots(SbStage stage,
                                                      const std::vector<std::string>& fields,
                                                      boost::optional<SbSlot> resultSlot,
                                                      PlanNodeId nodeId,
                                                      sbe::value::SlotIdGenerator* slotIdGenerator,
                                                      StageBuilderState& state,
                                                      const PlanStageSlots* slots) {
    SbBuilder b(state, nodeId);

    // 'outputSlots' will match the order of 'fields'. Bail out early if 'fields' is empty.
    SbSlotVector outputSlots;
    if (fields.empty()) {
        return {std::move(stage), std::move(outputSlots)};
    }

    // Handle the case where 'fields' contains only top-level fields.
    const bool topLevelFieldsOnly = std::all_of(
        fields.begin(), fields.end(), [](auto&& s) { return s.find('.') == std::string::npos; });

    if (topLevelFieldsOnly) {
        SbExprOptSlotVector projects;

        for (size_t i = 0; i < fields.size(); ++i) {
            auto name = std::make_pair(PlanStageSlots::kField, StringData(fields[i]));
            auto fieldSlot = slots ? slots->getIfExists(name) : boost::none;
            if (fieldSlot) {
                projects.emplace_back(*fieldSlot, boost::none);
            } else {
                tassert(9380405,
                        "Expected result slot or kField slot to be defined",
                        resultSlot.has_value());

                auto getFieldExpr = b.makeFunction(
                    "getField"_sd, SbExpr{*resultSlot}, b.makeStrConstant(fields[i]));
                projects.emplace_back(std::move(getFieldExpr), boost::none);
            }
        }

        return b.makeProject(std::move(stage), std::move(projects));
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
    treeRoot->value.expr = resultSlot ? SbExpr{*resultSlot} : SbExpr{};
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

    std::vector<SbExprOptSlotVector> stackOfProjects;
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
                tassert(9380406,
                        "Expected parent's expression to be set",
                        !parent->value.expr.isNull());

                auto getFieldExpr = b.makeFunction(
                    "getField"_sd, parent->value.expr.clone(), b.makeStrConstant(node->name));

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
                auto slot = SbSlot{slotIdGenerator->generate()};
                node->value.expr = slot;
                node->value.visited = true;
                // Grow 'stackOfProjects' if needed so that 'stackOfProjects[depth]' is valid.
                if (depth >= stackOfProjects.size()) {
                    stackOfProjects.resize(depth + 1);
                }
                // Add the projection to the appropriate level of 'stackOfProjects'.
                auto& projects = stackOfProjects[depth];
                projects.emplace_back(std::move(getFieldExpr), slot);
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
        auto [outStage, _] = b.makeProject(std::move(stage), std::move(projects));
        stage = std::move(outStage);
    }

    for (auto* node : fieldNodes) {
        outputSlots.emplace_back(node->value.expr.toSlot());
    }

    return {std::move(stage), std::move(outputSlots)};
}

}  // namespace mongo::stage_builder
