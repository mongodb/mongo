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

std::unique_ptr<EExpression> replaceUndefinedWithNullOrPassthrough(SlotId slot) {
    return makeE<EIf>(
        makeFunction("typeMatch",
                     makeVariable(slot),
                     sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                                sbe::value::bitcastFrom<int64_t>(
                                                    getBSONTypeMask(TypeTags::bsonUndefined)))),
        makeConstant(TypeTags::Null, 0),
        makeVariable(slot));
}

// Creates stages for building the key value. Missing keys are replaced with "null".
// TODO SERVER-63690: implement handling of paths.
std::pair<SlotId /*keySlot*/, std::unique_ptr<sbe::PlanStage>> buildLookupKey(
    std::unique_ptr<sbe::PlanStage> inputStage,
    SlotId recordSlot,
    StringData fieldName,
    const PlanNodeId nodeId,
    SlotIdGenerator& slotIdGenerator) {
    SlotId keySlot = slotIdGenerator.generate();
    EvalStage innerBranch = makeProject(
        EvalStage{std::move(inputStage), SlotVector{recordSlot}},
        nodeId,
        keySlot,
        makeFunction("fillEmpty"_sd,
                     makeFunction("getField"_sd, makeVariable(recordSlot), makeConstant(fieldName)),
                     makeConstant(TypeTags::Null, 0)));

    return {keySlot, std::move(innerBranch.stage)};
}

// Creates stages for the local side of $lookup.
std::pair<SlotId /*localKey*/, std::unique_ptr<sbe::PlanStage>> buildLocalLookupBranch(
    StageBuilderState& state,
    std::unique_ptr<sbe::PlanStage> localInputStage,
    SlotId localRecordSlot,
    StringData localFieldName,
    const PlanNodeId nodeId,
    SlotIdGenerator& slotIdGenerator) {
    auto [localKeySlot, localKeyStage] = buildLookupKey(
        std::move(localInputStage), localRecordSlot, localFieldName, nodeId, slotIdGenerator);

    // TODO SERVER-63691: repack local keys from an array into a set.
    return std::make_pair(localKeySlot, std::move(localKeyStage));
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
    StringData localFieldName,
    std::unique_ptr<sbe::PlanStage> foreignStage,
    SlotId foreignRecordSlot,
    StringData foreignFieldName,
    const PlanNodeId nodeId,
    SlotIdGenerator& slotIdGenerator) {
    // Build the outer branch that produces the correclated local key slot.
    auto [localKeySlot, outerRootStage] = buildLocalLookupBranch(
        state, std::move(localStage), localRecordSlot, localFieldName, nodeId, slotIdGenerator);

    // Build the inner branch. It involves getting the key and then building the nested lookup
    // join.
    auto [foreignKeySlot, foreignKeyStage] = buildLookupKey(
        std::move(foreignStage), foreignRecordSlot, foreignFieldName, nodeId, slotIdGenerator);
    EvalStage innerBranch{std::move(foreignKeyStage), SlotVector{}};

    // If the foreign key is an array, $lookup should match against any element of the array,
    // so need to traverse into it, while applying the equality filter to each element. Contents
    // of 'foreignKeySlot' will be overwritten with true/false result of the traversal - do we
    // want to keep it?
    {
        SlotId traverseOutputSlot = slotIdGenerator.generate();
        // "in" branch of the traverse applies the filter and populates the output slot with
        // true/false. If the local key is an array check for membership rather than equality.
        // Also, need to compare "undefined" in foreign as "null".
        std::unique_ptr<EExpression> checkInTraverse = makeE<EIf>(
            makeFunction("isArray"_sd, makeVariable(localKeySlot)),
            makeFunction("isMember"_sd, makeVariable(foreignKeySlot), makeVariable(localKeySlot)),
            makeBinaryOp(EPrimBinary::eq,
                         makeVariable(localKeySlot),
                         replaceUndefinedWithNullOrPassthrough(foreignKeySlot)));

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
    std::string localFieldName,
    std::string joinFieldName,
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
    const auto indexAccessMethod =
        foreignColl->getIndexCatalog()->getEntry(indexDescriptor)->accessMethod()->asSortedData();
    const auto indexVersion = indexAccessMethod->getSortedDataInterface()->getKeyStringVersion();
    const auto indexOrdering = indexAccessMethod->getSortedDataInterface()->getOrdering();
    iamMap.insert({indexName, indexAccessMethod});

    // Build the outer side of the index join which extracts the local join field. The local
    // field is stored in 'localFieldSlot' and made available to the inner side of the join.
    auto [localFieldSlot, localFieldStage] = buildLocalLookupBranch(
        state, std::move(localStage), localRecordSlot, localFieldName, nodeId, slotIdGenerator);

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

    // TODO(SERVER-63753): Remove mkbson stage here as it's temporarily added to enable testing
    // index join.
    auto resultSlot = slotIdGenerator.generate();
    auto resultStage = makeS<MakeBsonObjStage>(
        std::move(nljStage),
        resultSlot,
        localRecordSlot,
        MakeObjFieldBehavior::drop /* fieldBehavior */,
        std::vector<std::string>{} /* fields */,
        std::vector<std::string>{std::move(joinFieldName)} /* projectFields */,
        makeSV(foreignGroupSlot) /* projectVars */,
        true,
        false,
        nodeId);

    return {resultSlot, std::move(resultStage)};
}
}  // namespace

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildLookup(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    const auto eqLookupNode = static_cast<const EqLookupNode*>(root);

    // $lookup creates its own output documents.
    _shouldProduceRecordIdSlot = false;
    switch (eqLookupNode->lookupStrategy) {
        case EqLookupNode::LookupStrategy::kHashJoin:
            uasserted(5842602, "$lookup planning logic picked hash join");
            break;
        case EqLookupNode::LookupStrategy::kIndexedLoopJoin: {
            tassert(6357201,
                    "$lookup using index join should have one child and a populated index entry",
                    eqLookupNode->children.size() == 1 && eqLookupNode->idxEntry);

            const NamespaceString foreignCollNs(eqLookupNode->foreignCollection);
            const auto& foreignColl = _collections.lookupCollection(foreignCollNs);
            tassert(6357202,
                    str::stream() << "$lookup using index join with unknown foreign collection '"
                                  << foreignCollNs << "'",
                    foreignColl);
            const auto& index = *eqLookupNode->idxEntry;

            uassert(6357203,
                    str::stream() << "$lookup using index join doesn't work for hashed index '"
                                  << index.identifier.catalogName << "'",
                    index.type != INDEX_HASHED);

            const auto& localRoot = eqLookupNode->children[0];
            auto [localStage, localOutputs] = build(localRoot, reqs);
            sbe::value::SlotId localScanSlot = localOutputs.get(PlanStageSlots::kResult);

            auto [resultSlot, indexJoinStage] =
                buildIndexJoinLookupStage(_state,
                                          std::move(localStage),
                                          localScanSlot,
                                          eqLookupNode->joinFieldLocal,
                                          eqLookupNode->joinField,
                                          foreignColl,
                                          index,
                                          _data.iamMap,
                                          _yieldPolicy,
                                          eqLookupNode->nodeId(),
                                          _slotIdGenerator);

            PlanStageSlots outputs;
            outputs.set(kResult, resultSlot);
            return {std::move(indexJoinStage), std::move(outputs)};
        }
        case EqLookupNode::LookupStrategy::kNestedLoopJoin: {
            auto numChildren = eqLookupNode->children.size();
            tassert(6355300, "An EqLookupNode can only have one child", numChildren == 1);
            const auto& localRoot = eqLookupNode->children[0];
            auto [localStage, localOutputs] = build(localRoot, reqs);
            sbe::value::SlotId localResultSlot = localOutputs.get(PlanStageSlots::kResult);

            auto foreignResultSlot = _slotIdGenerator.generate();
            auto foreignRecordIdSlot = _slotIdGenerator.generate();
            const auto& foreignColl =
                _collections.lookupCollection(NamespaceString(eqLookupNode->foreignCollection));

            // TODO SERVER-64091: Delete this tassert when we correctly handle the case of a non
            //  existent foreign collection.
            tassert(6355302, "The foreign collection should exist", foreignColl);
            auto foreignStage = sbe::makeS<sbe::ScanStage>(foreignColl->uuid(),
                                                           foreignResultSlot,
                                                           foreignRecordIdSlot,
                                                           boost::none /* snapshotIdSlot */,
                                                           boost::none /* indexIdSlot */,
                                                           boost::none /* indexKeySlot */,
                                                           boost::none /* indexKeyPatternSlot */,
                                                           boost::none /* tsSlot */,
                                                           std::vector<std::string>{} /* fields */,
                                                           sbe::makeSV() /* vars */,
                                                           boost::none /* seekKeySlot */,
                                                           true /* forward */,
                                                           _yieldPolicy,
                                                           eqLookupNode->nodeId(),
                                                           sbe::ScanCallbacks{});

            auto [matchedSlot, nljStage] = buildNljLookupStage(_state,
                                                               std::move(localStage),
                                                               localResultSlot,
                                                               eqLookupNode->joinFieldLocal,
                                                               std::move(foreignStage),
                                                               foreignResultSlot,
                                                               eqLookupNode->joinFieldForeign,
                                                               eqLookupNode->nodeId(),
                                                               _slotIdGenerator);

            PlanStageSlots outputs;
            outputs.set(kResult,
                        localResultSlot);  // TODO SERVER-63753: create an object for $lookup result
            outputs.set("local"_sd, localResultSlot);
            outputs.set("matched"_sd, matchedSlot);
            return {std::move(nljStage), std::move(outputs)};
        }
        default:
            MONGO_UNREACHABLE_TASSERT(5842605);
    }
    MONGO_UNREACHABLE_TASSERT(5842606);
}

}  // namespace mongo::stage_builder
