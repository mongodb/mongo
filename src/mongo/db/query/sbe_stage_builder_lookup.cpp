/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/query/sbe_stage_builder.h"

#include <fmt/format.h>

#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/ix_scan.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/unwind.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/query/sbe_stage_builder_coll_scan.h"
#include "mongo/db/query/sbe_stage_builder_expression.h"
#include "mongo/db/query/sbe_stage_builder_filter.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/query/sbe_stage_builder_index_scan.h"
#include "mongo/db/query/sbe_stage_builder_projection.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/logv2/log.h"

namespace mongo::stage_builder {
/**
 * Helpers for building $lookup.
 */
namespace {
using namespace sbe;
using namespace sbe::value;

// Creates stages for traversing path 'fp' in the record from 'inputSlot'. Returns one key value at
// a time. For example, if the record in the 'inputSlot' is:
//     {a: [{b:[1,[2,3]]}, {b:4}, {b:1}, {b:2}]},
// the returned values for path "a.b" will be streamed as: 1, [2,3], 4, 1, 2.
std::pair<SlotId /*keyValueSlot*/, std::unique_ptr<sbe::PlanStage>> buildKeysStream(
    SlotId inputSlot,
    const FieldPath& fp,
    const PlanNodeId nodeId,
    SlotIdGenerator& slotIdGenerator) {
    const FieldIndex numParts = fp.getPathLength();

    std::unique_ptr<sbe::PlanStage> currentStage = makeLimitCoScanTree(nodeId, 1);
    SlotId keyValueSlot = inputSlot;
    for (size_t i = 0; i < numParts; i++) {
        const StringData fieldName = fp.getFieldName(i);

        SlotId getFieldSlot = slotIdGenerator.generate();
        EvalStage getFieldStage = makeProject(
            EvalStage{std::move(currentStage), makeSV()},
            nodeId,
            getFieldSlot,
            makeFunction("getField"_sd, makeVariable(keyValueSlot), makeConstant(fieldName)));

        SlotId unwindOutputSlot = slotIdGenerator.generate();
        currentStage = makeS<UnwindStage>(std::move(getFieldStage.stage) /*child stage*/,
                                          getFieldSlot,
                                          unwindOutputSlot,
                                          slotIdGenerator.generate() /*outIndex*/,
                                          true /*preserveNullAndEmptyArrays*/,
                                          nodeId);
        keyValueSlot = unwindOutputSlot;
    }
    return {keyValueSlot, std::move(currentStage)};
}

// Creates stages for traversing path 'fp' in the record from 'inputSlot'. Puts the set of key
// values into 'keyValuesSetSlot. For example, if the record in the 'inputSlot' is:
//     {a: [{b:[1,[2,3]]}, {b:4}, {b:1}, {b:2}]},
// the returned slot will contain for path "a.b" a set of {1, 2, 4, [2,3]}.
// If the stream produces no values, that is, would result in an empty set, the empty set is
// replaced with a set that contains a single 'null' value, so that it matches MQL semantics when
// empty arrays and all missing are matched to 'null'.
std::pair<SlotId /*keyValuesSetSlot*/, std::unique_ptr<sbe::PlanStage>> buildLocalKeySet(
    std::unique_ptr<sbe::PlanStage> inputStage,
    SlotId recordSlot,
    const FieldPath& fp,
    const PlanNodeId nodeId,
    SlotIdGenerator& slotIdGenerator) {
    // Create the branch to stream individual key values from every terminal of the path.
    auto [keyValueSlot, keyValuesStage] = buildKeysStream(recordSlot, fp, nodeId, slotIdGenerator);

    // Re-pack the individual key values into a set.
    SlotId keyValuesSetSlot = slotIdGenerator.generate();
    EvalStage packedKeyValuesStage = makeHashAgg(
        EvalStage{std::move(keyValuesStage), SlotVector{}},
        makeSV(), /*groupBy slots - "none" means creating a single group*/
        makeEM(keyValuesSetSlot, makeFunction("addToSet"_sd, makeVariable(keyValueSlot))),
        {} /*collatorSlot*/,
        false /*allowDiskUse*/,
        nodeId);

    // The set in 'keyValuesSetSlot' might end up empty if the localField contained only missing and
    // empty arrays (e.g. path "a.b" in {a: [{no_b:1}, {b:[]}]}). The semantics of MQL require these
    // cases to match to 'null', so we replace the empty set with a constant set that contains a
    // single 'null' value.
    auto [arrayWithNullTag, arrayWithNullVal] = makeNewArray();
    std::unique_ptr<EExpression> arrayWithNull = makeConstant(arrayWithNullTag, arrayWithNullVal);
    value::Array* arrayWithNullView = getArrayView(arrayWithNullVal);
    arrayWithNullView->push_back(TypeTags::Null, 0);

    std::unique_ptr<EExpression> isNonEmptySetExpr =
        makeBinaryOp(sbe::EPrimBinary::greater,
                     makeFunction("getArraySize", makeVariable(keyValuesSetSlot)),
                     makeConstant(TypeTags::NumberInt32, 0));

    SlotId nonEmptySetSlot = slotIdGenerator.generate();
    packedKeyValuesStage = makeProject(std::move(packedKeyValuesStage),
                                       nodeId,
                                       nonEmptySetSlot,
                                       sbe::makeE<sbe::EIf>(std::move(isNonEmptySetExpr),
                                                            makeVariable(keyValuesSetSlot),
                                                            std::move(arrayWithNull)));

    // Attach the set of key values to the original local record.
    std::unique_ptr<sbe::PlanStage> nljLocalWithKeyValuesSet =
        makeS<LoopJoinStage>(std::move(inputStage),
                             std::move(packedKeyValuesStage.stage),
                             makeSV(recordSlot) /*outerProjects*/,
                             makeSV(recordSlot) /*outerCorrelated*/,
                             nullptr /*predicate*/,
                             nodeId);

    return {nonEmptySetSlot, std::move(nljLocalWithKeyValuesSet)};
}

// Creates stages for grouping matched foreign records into an array. If there's no match, the
// stages return an empty array instead.
std::pair<SlotId /*resultSlot*/, std::unique_ptr<sbe::PlanStage>> buildForeignMatchedArray(
    EvalStage innerBranch,
    SlotId foreignRecordSlot,
    const PlanNodeId nodeId,
    SlotIdGenerator& slotIdGenerator) {
    // $lookup's aggregates the matching records into an array. We currently don't have a stage
    // that could do this grouping _after_ Nlj, so we achieve it by having a hash_agg inside the
    // inner branch that aggregates all matched records into a single accumulator. When there
    // are no matches, return an empty array.
    SlotId accumulatorSlot = slotIdGenerator.generate();
    innerBranch = makeHashAgg(
        std::move(innerBranch),
        makeSV(), /*groupBy slots*/
        makeEM(accumulatorSlot, makeFunction("addToArray"_sd, makeVariable(foreignRecordSlot))),
        {} /*collatorSlot*/,
        false /*allowDiskUse*/,
        nodeId);

    // $lookup is an _outer_ left join that returns an empty array for "as" field rather than
    // dropping the unmatched local records. The branch that accumulates the matched records into an
    // array returns either 1 or 0 results, so to return an empty array for no-matches case we union
    // this branch with a const scan that produces an empty array but limit it to 1, so if the given
    // branch does produce a record, only that record is returned.
    auto [emptyArrayTag, emptyArrayVal] = makeNewArray();
    // Immediately take ownership of the new array (we could use a ValueGuard here but we'll
    // need the constant below anyway).
    std::unique_ptr<EExpression> emptyArrayConst = makeConstant(emptyArrayTag, emptyArrayVal);

    SlotId emptyArraySlot = slotIdGenerator.generate();
    std::unique_ptr<sbe::PlanStage> emptyArrayStage = makeProjectStage(
        makeLimitCoScanTree(nodeId, 1), nodeId, emptyArraySlot, std::move(emptyArrayConst));

    SlotId unionOutputSlot = slotIdGenerator.generate();
    EvalStage unionStage =
        makeUnion(makeVector(EvalStage{std::move(innerBranch.stage), SlotVector{}},
                             EvalStage{std::move(emptyArrayStage), SlotVector{}}),
                  {makeSV(accumulatorSlot), makeSV(emptyArraySlot)} /*inputs*/,
                  makeSV(unionOutputSlot),
                  nodeId);

    return std::make_pair(
        unionOutputSlot,
        std::move(makeLimitSkip(std::move(unionStage), nodeId, 1 /*limit*/).stage));
}

std::pair<SlotId /*matched docs*/, std::unique_ptr<sbe::PlanStage>> buildNljLookupStage(
    StageBuilderState& state,
    std::unique_ptr<sbe::PlanStage> localStage,
    SlotId localRecordSlot,
    const FieldPath& localFieldName,
    std::unique_ptr<sbe::PlanStage> foreignStage,
    SlotId foreignRecordSlot,
    StringData foreignFieldName,
    const PlanNodeId nodeId,
    SlotIdGenerator& slotIdGenerator) {
    // Build the outer branch that produces the set of local key values.
    auto [localKeySlot, outerRootStage] = buildLocalKeySet(
        std::move(localStage), localRecordSlot, localFieldName, nodeId, slotIdGenerator);

    // Build the inner branch. It involves getting the foreign keys and then building the nested
    // lookup join.
    // TODO SERVER-64483: implement handling of paths in foreignField.
    SlotId foreignKeySlot = slotIdGenerator.generate();
    EvalStage innerBranch =
        makeProject(EvalStage{std::move(foreignStage), SlotVector{foreignKeySlot}},
                    nodeId,
                    foreignKeySlot,
                    makeFunction("fillEmpty"_sd,
                                 makeFunction("getField"_sd,
                                              makeVariable(foreignRecordSlot),
                                              makeConstant(foreignFieldName)),
                                 makeConstant(TypeTags::Null, 0)));

    // If the foreign key is an array, $lookup should match against the array itself and all
    // elements of the array, so need to traverse into it, while applying the equality filter to
    // each element. Contents of 'foreignKeySlot' will be overwritten with true/false result of the
    // traversal - do we want to keep it?
    {
        SlotId traverseOutputSlot = slotIdGenerator.generate();
        // "in" branch of the traverse applies the filter and populates the output slot with
        // true/false. If the local key is an array check for membership rather than equality.
        // Also, need to compare "undefined" in foreign as "null".
        std::unique_ptr<EExpression> checkInTraverse =
            makeFunction("isMember"_sd, makeVariable(foreignKeySlot), makeVariable(localKeySlot));

        SlotId innerTraverseOutputSlot = slotIdGenerator.generate();
        EvalStage innerTraverseBranch = makeProject(makeLimitCoScanStage(nodeId),
                                                    nodeId,
                                                    innerTraverseOutputSlot,
                                                    std::move(checkInTraverse));

        innerBranch = makeTraverse(
            std::move(innerBranch) /* "from" branch */,
            std::move(innerTraverseBranch) /* "in" branch */,
            foreignKeySlot /* inField */,
            traverseOutputSlot /* outField */,
            innerTraverseOutputSlot /* outFieldInner */,
            makeVariable(innerTraverseOutputSlot) /* foldExpr */,
            makeVariable(traverseOutputSlot) /* finalExpr: stop as soon as find a match */,
            nodeId,
            1 /*nestedArraysDepth*/);

        // Add a filter that only lets through matched records.
        std::unique_ptr<EExpression> predicate =
            makeFunction("fillEmpty"_sd,
                         makeVariable(traverseOutputSlot),
                         makeConstant(TypeTags::Boolean, false));
        innerBranch = makeFilter<false /*IsConst*/, false /*IsEof*/>(
            std::move(innerBranch), std::move(predicate), nodeId);
    }

    // Group the matched foreign documents into a list, stored in the 'innerResultSlot'.
    // It creates a union stage internally so that when there's no matching foreign records, an
    // empty array will be returned.
    auto [innerResultSlot, innerRootStage] = buildForeignMatchedArray(
        std::move(innerBranch), foreignRecordSlot, nodeId, slotIdGenerator);

    // Connect the two branches with a nested loop join. For each outer record with a corresponding
    // value in the 'localKeySlot', the inner branch will be executed and will place the result into
    // 'innerResultSlot'.
    std::unique_ptr<sbe::PlanStage> nlj =
        makeS<LoopJoinStage>(std::move(outerRootStage),
                             std::move(innerRootStage),
                             makeSV(localRecordSlot) /*outerProjects*/,
                             makeSV(localKeySlot) /*outerCorrelated*/,
                             nullptr /*predicate*/,
                             nodeId);

    return {innerResultSlot, std::move(nlj)};
}

std::pair<SlotId, std::unique_ptr<sbe::PlanStage>> buildLookupResultObject(
    std::unique_ptr<sbe::PlanStage> stage,
    SlotId localDocumentSlot,
    SlotId resultArraySlot,
    const FieldPath& fieldPath,
    const PlanNodeId nodeId,
    SlotIdGenerator& slotIdGenerator) {
    const int32_t pathLength = fieldPath.getPathLength();

    // Extract values of all fields along the path except the last one.
    auto fieldSlots = slotIdGenerator.generateMultiple(pathLength - 1);
    for (int32_t i = 0; i < pathLength - 1; i++) {
        const auto fieldName = fieldPath.getFieldName(i);
        const auto inputSlot = i == 0 ? localDocumentSlot : fieldSlots[i - 1];
        stage = makeProjectStage(
            std::move(stage),
            nodeId,
            fieldSlots[i],
            makeFunction("getField"_sd, makeVariable(inputSlot), makeConstant(fieldName)));
    }

    // Construct new objects for each path level.
    auto objectSlots = slotIdGenerator.generateMultiple(pathLength);
    for (int32_t i = pathLength - 1; i >= 0; i--) {
        const auto rootObjectSlot = i == 0 ? localDocumentSlot : fieldSlots[i - 1];
        const auto fieldName = fieldPath.getFieldName(i).toString();
        const auto valueSlot = i == pathLength - 1 ? resultArraySlot : objectSlots[i + 1];
        stage = makeS<MakeBsonObjStage>(std::move(stage),
                                        objectSlots[i],                        /* objSlot */
                                        rootObjectSlot,                        /* rootSlot */
                                        MakeBsonObjStage::FieldBehavior::drop, /* fieldBehaviour */
                                        std::vector<std::string>{},            /* fields */
                                        std::vector<std::string>{fieldName},   /* projectFields */
                                        SlotVector{valueSlot},                 /* projectVars */
                                        true,                                  /* forceNewObject */
                                        false,                                 /* returnOldObject */
                                        nodeId);
    }

    return {objectSlots.front(), std::move(stage)};
}

/*
 * Build $lookup stage using index join strategy. Below is an example plan for the aggregation
 * [{$lookup: {localField: "a", foreignField: "b"}}] with an index {b: 1} on the foreign
 * collection.
 *
 * nlj [localRecord]
 * left
 *     project [localField = getField (localRecord, "a")]
 *     scan localRecord
 * right
 *     limit 1
 *     union [foreignGroup] [
 *         group [] [foreignGroupOrNothing = addToArray (foreignRecord)]
 *         nlj []
 *         left
 *             nlj [indexId, indexKeyPattern]
 *             left
 *                 project [lowKey = ks (localField, _, _, kExclusiveBefore),
 *                          highKey = ks (localField, _, _, kExclusiveAfter),
 *                          indexId = "b_1",
 *                          indexKeyPattern = {"b" : 1}]
 *                 limit 1
 *                 coscan
 *             right
 *                 ixseek lowKey highKey indexKey foreignRecordId snapshotId _ @"b_1"
 *         right
 *             limit 1
 *             seek foreignRecordId foreignRecord _ snapshotId indexId indexKey indexKeyPattern
 *         ,
 *         project [emptyArray = []]
 *         limit 1
 *         coscan
 *     ]
 *
 */
std::pair<SlotId, std::unique_ptr<sbe::PlanStage>> buildIndexJoinLookupStage(
    StageBuilderState& state,
    std::unique_ptr<sbe::PlanStage> localStage,
    SlotId localRecordSlot,
    const FieldPath& localFieldName,
    const CollectionPtr& foreignColl,
    const IndexEntry& index,
    StringMap<const IndexAccessMethod*>& iamMap,
    PlanYieldPolicySBE* yieldPolicy,
    const PlanNodeId nodeId,
    SlotIdGenerator& slotIdGenerator) {
    const auto foreignCollUUID = foreignColl->uuid();
    const auto indexName = index.identifier.catalogName;
    const auto indexDescriptor =
        foreignColl->getIndexCatalog()->findIndexByName(state.opCtx, indexName);
    tassert(6447401,
            str::stream() << "Index " << indexName
                          << " is unexpectedly missing for $lookup index join",
            indexDescriptor);
    const auto indexAccessMethod =
        foreignColl->getIndexCatalog()->getEntry(indexDescriptor)->accessMethod()->asSortedData();
    const auto indexVersion = indexAccessMethod->getSortedDataInterface()->getKeyStringVersion();
    const auto indexOrdering = indexAccessMethod->getSortedDataInterface()->getOrdering();
    iamMap.insert({indexName, indexAccessMethod});

    // Build the outer branch that produces the correlated local key slot.
    auto [localKeysSetSlot, localKeysSetStage] = buildLocalKeySet(
        std::move(localStage), localRecordSlot, localFieldName, nodeId, slotIdGenerator);

    // 'localFieldSlot' is a set even if it contains a single item. Extract this single value until
    // SERVER-63574 is implemented.
    SlotId localFieldSlot = slotIdGenerator.generate();
    auto localFieldStage = makeS<UnwindStage>(std::move(localKeysSetStage) /*child stage*/,
                                              localKeysSetSlot,
                                              localFieldSlot,
                                              slotIdGenerator.generate() /*outIndex*/,
                                              true /*preserveNullAndEmptyArrays*/,
                                              nodeId);

    // Calculate the low key and high key of each individual local field. They are stored in
    // 'lowKeySlot' and 'highKeySlot', respectively. These two slots will be made available in
    // the loop join stage to perform index seek. We also set 'indexIdSlot' and
    // 'indexKeyPatternSlot' constants for the seek stage later to perform consistency check.
    auto lowKeySlot = slotIdGenerator.generate();
    auto highKeySlot = slotIdGenerator.generate();
    auto indexIdSlot = slotIdGenerator.generate();
    auto indexKeyPatternSlot = slotIdGenerator.generate();
    auto [_, indexKeyPatternValue] =
        sbe::value::copyValue(sbe::value::TypeTags::bsonObject,
                              sbe::value::bitcastFrom<const char*>(index.keyPattern.objdata()));
    auto indexBoundKeyStage = makeProjectStage(
        makeLimitCoScanTree(nodeId, 1),
        nodeId,
        lowKeySlot,
        makeFunction(
            "ks"_sd,
            makeConstant(value::TypeTags::NumberInt64, static_cast<int64_t>(indexVersion)),
            makeConstant(value::TypeTags::NumberInt32, indexOrdering.getBits()),
            makeVariable(localFieldSlot),
            makeConstant(value::TypeTags::NumberInt64,
                         static_cast<int64_t>(KeyString::Discriminator::kExclusiveBefore))),
        highKeySlot,
        makeFunction("ks"_sd,
                     makeConstant(value::TypeTags::NumberInt64, static_cast<int64_t>(indexVersion)),
                     makeConstant(value::TypeTags::NumberInt32, indexOrdering.getBits()),
                     makeVariable(localFieldSlot),
                     makeConstant(value::TypeTags::NumberInt64,
                                  static_cast<int64_t>(KeyString::Discriminator::kExclusiveAfter))),
        indexIdSlot,
        makeConstant(indexName),
        indexKeyPatternSlot,
        makeConstant(value::TypeTags::bsonObject, indexKeyPatternValue));

    // Perform the index seek based on the 'lowKeySlot' and 'highKeySlot' from the outer side.
    // The foreign record id of the seek is stored in 'foreignRecordIdSlot'. We also keep
    // 'indexKeySlot' and 'snapshotIdSlot' for the seek stage later to perform consistency
    // check.
    auto foreignRecordIdSlot = slotIdGenerator.generate();
    auto indexKeySlot = slotIdGenerator.generate();
    auto snapshotIdSlot = slotIdGenerator.generate();
    auto ixScanStage = makeS<IndexScanStage>(foreignCollUUID,
                                             indexName,
                                             true /* forward */,
                                             indexKeySlot,
                                             foreignRecordIdSlot,
                                             snapshotIdSlot,
                                             IndexKeysInclusionSet{} /* indexKeysToInclude */,
                                             makeSV() /* vars */,
                                             lowKeySlot,
                                             highKeySlot,
                                             yieldPolicy,
                                             nodeId);

    // Loop join the low key and high key generation with the index seek stage to produce the
    // foreign record id to seek.
    auto ixScanNljStage =
        makeS<LoopJoinStage>(std::move(indexBoundKeyStage),
                             std::move(ixScanStage),
                             makeSV(indexIdSlot, indexKeyPatternSlot) /* outerProjects */,
                             makeSV(lowKeySlot, highKeySlot) /* outerCorrelated */,
                             nullptr /* predicate */,
                             nodeId);

    // Loop join the foreign record id produced by the index seek on the outer side with seek
    // stage on the inner side to get matched foreign documents. The foreign documents are
    // stored in 'foreignRecordSlot'. We also pass in 'snapshotIdSlot', 'indexIdSlot',
    // 'indexKeySlot' and 'indexKeyPatternSlot' to perform index consistency check during the
    // seek.
    auto [foreignRecordSlot, __, scanNljStage] = makeLoopJoinForFetch(std::move(ixScanNljStage),
                                                                      foreignRecordIdSlot,
                                                                      snapshotIdSlot,
                                                                      indexIdSlot,
                                                                      indexKeySlot,
                                                                      indexKeyPatternSlot,
                                                                      foreignColl,
                                                                      iamMap,
                                                                      nodeId,
                                                                      makeSV() /* slotsToForward */,
                                                                      slotIdGenerator);

    // Group the matched foreign documents into a list, stored in the 'foreignGroupSlot'.
    // It creates a union stage internally so that when there's no matching foreign records, an
    // empty array will be returned.
    auto [foreignGroupSlot, foreignGroupStage] = buildForeignMatchedArray(
        {std::move(scanNljStage), makeSV()}, foreignRecordSlot, nodeId, slotIdGenerator);

    // The top level loop join stage that joins each local field with the matched foreign
    // documents.
    auto nljStage = makeS<LoopJoinStage>(std::move(localFieldStage),
                                         std::move(foreignGroupStage),
                                         makeSV(localRecordSlot) /* outerProjects */,
                                         makeSV(localFieldSlot) /* outerCorrelated */,
                                         nullptr,
                                         nodeId);

    return {foreignGroupSlot, std::move(nljStage)};
}

/*
 * Builds a project stage that projects an empty array for each local document.
 */
std::pair<SlotId, std::unique_ptr<sbe::PlanStage>> buildNonExistentForeignCollLookupStage(
    StageBuilderState& state,
    std::unique_ptr<sbe::PlanStage> localStage,
    const PlanNodeId nodeId,
    SlotIdGenerator& slotIdGenerator) {
    auto [emptyArrayTag, emptyArrayVal] = makeNewArray();
    SlotId emptyArraySlot = slotIdGenerator.generate();
    return {emptyArraySlot,
            makeProjectStage(std::move(localStage),
                             nodeId,
                             emptyArraySlot,
                             makeConstant(emptyArrayTag, emptyArrayVal))};
}
}  // namespace

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildLookup(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    const auto eqLookupNode = static_cast<const EqLookupNode*>(root);

    // $lookup creates its own output documents.
    _shouldProduceRecordIdSlot = false;

    auto localReqs = reqs.copy().set(kResult);
    auto [localStage, localOutputs] = build(eqLookupNode->children[0], localReqs);
    SlotId localDocumentSlot = localOutputs.get(PlanStageSlots::kResult);

    auto [matchedDocumentsSlot, foreignStage] = [&, localStage = std::move(localStage)]() mutable
        -> std::pair<SlotId, std::unique_ptr<sbe::PlanStage>> {
        const auto& foreignColl =
            _collections.lookupCollection(NamespaceString(eqLookupNode->foreignCollection));
        // When foreign collection doesn't exist, we create stages that simply append empty arrays
        // to each local document and do not consider the case that foreign collection may be
        // created during the query, since we cannot easily create dynamic plan stages and it has
        // messier semantics. Builds a project stage that projects an empty array for each local
        // document.
        if (!foreignColl) {
            return buildNonExistentForeignCollLookupStage(
                _state, std::move(localStage), eqLookupNode->nodeId(), _slotIdGenerator);
        }

        switch (eqLookupNode->lookupStrategy) {
            case EqLookupNode::LookupStrategy::kHashJoin:
                uasserted(5842602, "$lookup planning logic picked hash join");
                break;
            case EqLookupNode::LookupStrategy::kIndexedLoopJoin: {
                tassert(
                    6357201,
                    "$lookup using index join should have one child and a populated index entry",
                    eqLookupNode->children.size() == 1 && eqLookupNode->idxEntry);

                const auto& index = *eqLookupNode->idxEntry;

                uassert(6357203,
                        str::stream() << "$lookup using index join doesn't work for hashed index '"
                                      << index.identifier.catalogName << "'",
                        index.type != INDEX_HASHED);

                return buildIndexJoinLookupStage(_state,
                                                 std::move(localStage),
                                                 localDocumentSlot,
                                                 eqLookupNode->joinFieldLocal,
                                                 foreignColl,
                                                 index,
                                                 _data.iamMap,
                                                 _yieldPolicy,
                                                 eqLookupNode->nodeId(),
                                                 _slotIdGenerator);
            }
            case EqLookupNode::LookupStrategy::kNestedLoopJoin: {
                auto numChildren = eqLookupNode->children.size();
                tassert(6355300, "An EqLookupNode can only have one child", numChildren == 1);

                auto foreignResultSlot = _slotIdGenerator.generate();
                auto foreignRecordIdSlot = _slotIdGenerator.generate();

                auto foreignStage = makeS<ScanStage>(foreignColl->uuid(),
                                                     foreignResultSlot,
                                                     foreignRecordIdSlot,
                                                     boost::none /* snapshotIdSlot */,
                                                     boost::none /* indexIdSlot */,
                                                     boost::none /* indexKeySlot */,
                                                     boost::none /* indexKeyPatternSlot */,
                                                     boost::none /* tsSlot */,
                                                     std::vector<std::string>{} /* fields */,
                                                     makeSV() /* vars */,
                                                     boost::none /* seekKeySlot */,
                                                     true /* forward */,
                                                     _yieldPolicy,
                                                     eqLookupNode->nodeId(),
                                                     ScanCallbacks{});

                return buildNljLookupStage(_state,
                                           std::move(localStage),
                                           localDocumentSlot,
                                           eqLookupNode->joinFieldLocal,
                                           std::move(foreignStage),
                                           foreignResultSlot,
                                           eqLookupNode->joinFieldForeign.fullPath(),
                                           eqLookupNode->nodeId(),
                                           _slotIdGenerator);
            }
            default:
                MONGO_UNREACHABLE_TASSERT(5842605);
        }
    }();

    auto [resultSlot, resultStage] = buildLookupResultObject(std::move(foreignStage),
                                                             localDocumentSlot,
                                                             matchedDocumentsSlot,
                                                             eqLookupNode->joinField,
                                                             eqLookupNode->nodeId(),
                                                             _slotIdGenerator);

    PlanStageSlots outputs;
    outputs.set(kResult, resultSlot);
    return {std::move(resultStage), std::move(outputs)};
}

}  // namespace mongo::stage_builder
