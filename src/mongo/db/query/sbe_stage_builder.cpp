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

#include "mongo/platform/basic.h"

#include "mongo/db/query/sbe_stage_builder.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/makeobj.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/sort.h"
#include "mongo/db/exec/sbe/stages/sorted_merge.h"
#include "mongo/db/exec/sbe/stages/text_match.h"
#include "mongo/db/exec/sbe/stages/traverse.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/stages/unique.h"
#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/fts/fts_index_format.h"
#include "mongo/db/fts/fts_query_impl.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/query/sbe_stage_builder_coll_scan.h"
#include "mongo/db/query/sbe_stage_builder_filter.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/query/sbe_stage_builder_index_scan.h"
#include "mongo/db/query/sbe_stage_builder_projection.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/db/s/collection_sharding_state.h"

namespace mongo::stage_builder {
std::unique_ptr<sbe::RuntimeEnvironment> makeRuntimeEnvironment(
    OperationContext* opCtx, sbe::value::SlotIdGenerator* slotIdGenerator) {
    auto env = std::make_unique<sbe::RuntimeEnvironment>();

    // Register an unowned global timezone database for datetime expression evaluation.
    env->registerSlot("timeZoneDB"_sd,
                      sbe::value::TypeTags::timeZoneDB,
                      sbe::value::bitcastFrom<const TimeZoneDatabase*>(getTimeZoneDatabase(opCtx)),
                      false,
                      slotIdGenerator);
    return env;
}

PlanStageSlots::PlanStageSlots(const PlanStageReqs& reqs,
                               sbe::value::SlotIdGenerator* slotIdGenerator) {
    for (auto&& [slotName, isRequired] : reqs._slots) {
        if (isRequired) {
            _slots[slotName] = slotIdGenerator->generate();
        }
    }
}

std::string PlanStageData::debugString() const {
    StringBuilder builder;

    if (auto slot = outputs.getIfExists(PlanStageSlots::kResult); slot) {
        builder << "$$RESULT=s" << *slot << " ";
    }
    if (auto slot = outputs.getIfExists(PlanStageSlots::kRecordId); slot) {
        builder << "$$RID=s" << *slot << " ";
    }
    if (auto slot = outputs.getIfExists(PlanStageSlots::kOplogTs); slot) {
        builder << "$$OPLOGTS=s" << *slot << " ";
    }

    env->debugString(&builder);

    return builder.str();
}

namespace {
const QuerySolutionNode* getNodeByType(const QuerySolutionNode* root, StageType type) {
    if (root->getType() == type) {
        return root;
    }

    for (auto&& child : root->children) {
        if (auto result = getNodeByType(child, type)) {
            return result;
        }
    }

    return nullptr;
}

sbe::LockAcquisitionCallback makeLockAcquisitionCallback(bool checkNodeCanServeReads) {
    if (!checkNodeCanServeReads) {
        return {};
    }

    return [](OperationContext* opCtx, const AutoGetCollectionForReadMaybeLockFree& coll) {
        uassertStatusOK(repl::ReplicationCoordinator::get(opCtx)->checkCanServeReadsFor(
            opCtx, coll.getNss(), true));
    };
}

}  // namespace

SlotBasedStageBuilder::SlotBasedStageBuilder(OperationContext* opCtx,
                                             const CollectionPtr& collection,
                                             const CanonicalQuery& cq,
                                             const QuerySolution& solution,
                                             PlanYieldPolicySBE* yieldPolicy,
                                             ShardFiltererFactoryInterface* shardFiltererFactory)
    : StageBuilder(opCtx, collection, cq, solution),
      _yieldPolicy(yieldPolicy),
      _data(makeRuntimeEnvironment(_opCtx, &_slotIdGenerator)),
      _shardFiltererFactory(shardFiltererFactory),
      _lockAcquisitionCallback(makeLockAcquisitionCallback(solution.shouldCheckCanServeReads())) {
    // SERVER-52803: In the future if we need to gather more information from the QuerySolutionNode
    // tree, rather than doing one-off scans for each piece of information, we should add a formal
    // analysis pass here.
    if (auto node = getNodeByType(solution.root(), STAGE_COLLSCAN)) {
        auto csn = static_cast<const CollectionScanNode*>(node);
        _data.shouldTrackLatestOplogTimestamp = csn->shouldTrackLatestOplogTimestamp;
        _data.shouldTrackResumeToken = csn->requestResumeToken;
        _data.shouldUseTailableScan = csn->tailable;
    }

    if (auto node = getNodeByType(solution.root(), STAGE_VIRTUAL_SCAN)) {
        auto vsn = static_cast<const VirtualScanNode*>(node);
        _shouldProduceRecordIdSlot = vsn->hasRecordId;
    }
}

std::unique_ptr<sbe::PlanStage> SlotBasedStageBuilder::build(const QuerySolutionNode* root) {
    // For a given SlotBasedStageBuilder instance, this build() method can only be called once.
    invariant(!_buildHasStarted);
    _buildHasStarted = true;

    // We always produce a 'resultSlot' and conditionally produce a 'recordIdSlot' based on the
    // 'shouldProduceRecordIdSlot'. If the solution contains a CollectionScanNode with the
    // 'shouldTrackLatestOplogTimestamp' flag set to true, then we will also produce an
    // 'oplogTsSlot'.
    PlanStageReqs reqs;
    reqs.set(kResult);
    reqs.setIf(kRecordId, _shouldProduceRecordIdSlot);
    reqs.setIf(kOplogTs, _data.shouldTrackLatestOplogTimestamp);

    // Build the SBE plan stage tree.
    auto [stage, outputs] = build(root, reqs);

    // Assert that we produced a 'resultSlot' and that we prouced a 'recordIdSlot' if the
    // 'shouldProduceRecordIdSlot' flag was set. Also assert that we produced an 'oplogTsSlot' if
    // it's needed.
    invariant(outputs.has(kResult));
    invariant(!_shouldProduceRecordIdSlot || outputs.has(kRecordId));
    invariant(!_data.shouldTrackLatestOplogTimestamp || outputs.has(kOplogTs));

    _data.outputs = std::move(outputs);

    return std::move(stage);
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildCollScan(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    invariant(!reqs.getIndexKeyBitset());

    auto csn = static_cast<const CollectionScanNode*>(root);

    auto [stage, outputs] = generateCollScan(_opCtx,
                                             _collection,
                                             csn,
                                             &_slotIdGenerator,
                                             &_frameIdGenerator,
                                             _yieldPolicy,
                                             _data.env,
                                             reqs.getIsTailableCollScanResumeBranch(),
                                             _lockAcquisitionCallback);

    if (reqs.has(kReturnKey)) {
        // Assign the 'returnKeySlot' to be the empty object.
        outputs.set(kReturnKey, _slotIdGenerator.generate());
        stage = sbe::makeProjectStage(std::move(stage),
                                      root->nodeId(),
                                      outputs.get(kReturnKey),
                                      sbe::makeE<sbe::EFunction>("newObj", sbe::makeEs()));
    }

    // Assert that generateCollScan() generated an oplogTsSlot if it's needed.
    invariant(!reqs.has(kOplogTs) || outputs.has(kOplogTs));

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildVirtualScan(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto vsn = static_cast<const VirtualScanNode*>(root);
    invariant(!reqs.getIndexKeyBitset());

    // Virtual scans cannot produce an oplogTsSlot, so assert that the caller doesn't need it.
    invariant(!reqs.has(kOplogTs));

    auto [inputTag, inputVal] = sbe::value::makeNewArray();
    sbe::value::ValueGuard inputGuard{inputTag, inputVal};
    auto inputView = sbe::value::getArrayView(inputVal);

    for (auto& doc : vsn->docs) {
        auto [tag, val] = makeValue(doc);
        inputView->push_back(tag, val);
    }

    inputGuard.reset();
    auto [scanSlots, scanStage] =
        generateVirtualScanMulti(&_slotIdGenerator, vsn->hasRecordId ? 2 : 1, inputTag, inputVal);

    PlanStageSlots outputs;

    if (vsn->hasRecordId) {
        invariant(scanSlots.size() == 2);
        outputs.set(kRecordId, scanSlots[0]);
        outputs.set(kResult, scanSlots[1]);
    } else {
        invariant(scanSlots.size() == 1);
        invariant(!reqs.has(kRecordId));
        outputs.set(kResult, scanSlots[0]);
    }

    return {std::move(scanStage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildIndexScan(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto ixn = static_cast<const IndexScanNode*>(root);
    invariant(reqs.has(kReturnKey) || !ixn->addKeyMetadata);

    // Index scans cannot produce an oplogTsSlot, so assert that the caller doesn't need it.
    invariant(!reqs.has(kOplogTs));

    return generateIndexScan(_opCtx,
                             _collection,
                             ixn,
                             reqs,
                             &_slotIdGenerator,
                             &_spoolIdGenerator,
                             _yieldPolicy,
                             _lockAcquisitionCallback);
}

std::tuple<sbe::value::SlotId, sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>>
SlotBasedStageBuilder::makeLoopJoinForFetch(std::unique_ptr<sbe::PlanStage> inputStage,
                                            sbe::value::SlotId seekKeySlot,
                                            PlanNodeId planNodeId,
                                            sbe::value::SlotVector slotsToForward) {
    auto resultSlot = _slotIdGenerator.generate();
    auto recordIdSlot = _slotIdGenerator.generate();

    // Scan the collection in the range [seekKeySlot, Inf).
    auto scanStage = sbe::makeS<sbe::ScanStage>(
        NamespaceStringOrUUID{_collection->ns().db().toString(), _collection->uuid()},
        resultSlot,
        recordIdSlot,
        std::vector<std::string>{},
        sbe::makeSV(),
        seekKeySlot,
        true,
        nullptr,
        planNodeId,
        _lockAcquisitionCallback);

    // Get the recordIdSlot from the outer side (e.g., IXSCAN) and feed it to the inner side,
    // limiting the result set to 1 row.
    auto stage = sbe::makeS<sbe::LoopJoinStage>(
        std::move(inputStage),
        sbe::makeS<sbe::LimitSkipStage>(std::move(scanStage), 1, boost::none, planNodeId),
        std::move(slotsToForward),
        sbe::makeSV(seekKeySlot),
        nullptr,
        planNodeId);

    return {resultSlot, recordIdSlot, std::move(stage)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildFetch(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto fn = static_cast<const FetchNode*>(root);
    invariant(!reqs.getIndexKeyBitset());

    // At present, makeLoopJoinForFetch() doesn't have the necessary logic for producing an
    // oplogTsSlot, so assert that the caller doesn't need oplogTsSlot.
    invariant(!reqs.has(kOplogTs));

    // The child must produce all of the slots required by the parent of this FetchNode, except for
    // 'resultSlot' which will be produced by the call to makeLoopJoinForFetch() below. In addition
    // to that, the child must always produce a 'recordIdSlot' because it's needed for the call to
    // makeLoopJoinForFetch() below.
    auto childReqs = reqs.copy().clear(kResult).set(kRecordId);

    auto [stage, outputs] = build(fn->children[0], childReqs);

    uassert(4822880, "RecordId slot is not defined", outputs.has(kRecordId));
    uassert(
        4953600, "ReturnKey slot is not defined", !reqs.has(kReturnKey) || outputs.has(kReturnKey));

    auto forwardingReqs = reqs.copy().clear(kResult).clear(kRecordId);

    auto relevantSlots = sbe::makeSV();
    outputs.forEachSlot(forwardingReqs, [&](auto&& slot) { relevantSlots.push_back(slot); });

    sbe::value::SlotId fetchResultSlot, fetchRecordIdSlot;
    std::tie(fetchResultSlot, fetchRecordIdSlot, stage) = makeLoopJoinForFetch(
        std::move(stage), outputs.get(kRecordId), root->nodeId(), std::move(relevantSlots));

    outputs.set(kResult, fetchResultSlot);
    outputs.set(kRecordId, fetchRecordIdSlot);

    if (fn->filter) {
        forwardingReqs = reqs.copy().set(kResult).set(kRecordId);

        relevantSlots = sbe::makeSV();
        outputs.forEachSlot(forwardingReqs, [&](auto&& slot) { relevantSlots.push_back(slot); });

        stage = generateFilter(_opCtx,
                               fn->filter.get(),
                               std::move(stage),
                               &_slotIdGenerator,
                               &_frameIdGenerator,
                               outputs.get(kResult),
                               _data.env,
                               std::move(relevantSlots),
                               root->nodeId());
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildLimit(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    const auto ln = static_cast<const LimitNode*>(root);
    boost::optional<long long> skip;

    auto [stage, outputs] = [&]() {
        if (ln->children[0]->getType() == StageType::STAGE_SKIP) {
            // If we have both limit and skip stages and the skip stage is beneath the limit, then
            // we can combine these two stages into one.
            const auto sn = static_cast<const SkipNode*>(ln->children[0]);
            skip = sn->skip;
            return build(sn->children[0], reqs);
        } else {
            return build(ln->children[0], reqs);
        }
    }();

    if (!reqs.getIsTailableCollScanResumeBranch()) {
        stage = std::make_unique<sbe::LimitSkipStage>(
            std::move(stage), ln->limit, skip, root->nodeId());
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildSkip(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    const auto sn = static_cast<const SkipNode*>(root);
    auto [stage, outputs] = build(sn->children[0], reqs);

    if (!reqs.getIsTailableCollScanResumeBranch()) {
        stage = std::make_unique<sbe::LimitSkipStage>(
            std::move(stage), boost::none, sn->skip, root->nodeId());
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildSort(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    using namespace std::literals;
    invariant(!reqs.getIndexKeyBitset());

    const auto sn = static_cast<const SortNode*>(root);
    auto sortPattern = SortPattern{sn->pattern, _cq.getExpCtx()};

    // The child must produce all of the slots required by the parent of this SortNode. In addition
    // to that, the child must always produce a 'resultSlot' because it's needed by the sort logic
    // below.
    auto childReqs = reqs.copy().set(kResult);
    auto [inputStage, outputs] = build(sn->children[0], childReqs);

    sbe::value::SlotVector orderBy;
    std::vector<sbe::value::SortDirection> direction;
    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projectMap;

    for (const auto& part : sortPattern) {
        uassert(5073801, "Sorting by expression not supported", !part.expression);
        uassert(5073802,
                "Sorting by dotted paths not supported",
                part.fieldPath && part.fieldPath->getPathLength() == 1);

        // Slot holding the sort key.
        auto sortFieldVar{_slotIdGenerator.generate()};
        orderBy.push_back(sortFieldVar);
        direction.push_back(part.isAscending ? sbe::value::SortDirection::Ascending
                                             : sbe::value::SortDirection::Descending);

        // Generate projection to get the value of the sort key. Ideally, this should be
        // tracked by a 'reference tracker' at higher level.
        auto fieldName = part.fieldPath->getFieldName(0);
        auto fieldNameSV = std::string_view{fieldName.rawData(), fieldName.size()};
        projectMap.emplace(
            sortFieldVar,
            sbe::makeE<sbe::EFunction>("getField"sv,
                                       sbe::makeEs(sbe::makeE<sbe::EVariable>(outputs.get(kResult)),
                                                   sbe::makeE<sbe::EConstant>(fieldNameSV))));
    }

    inputStage =
        sbe::makeS<sbe::ProjectStage>(std::move(inputStage), std::move(projectMap), root->nodeId());

    // Generate traversals to pick the min/max element from arrays.
    for (size_t idx = 0; idx < orderBy.size(); ++idx) {
        auto resultVar{_slotIdGenerator.generate()};
        auto innerVar{_slotIdGenerator.generate()};

        auto innerBranch = sbe::makeProjectStage(
            sbe::makeS<sbe::LimitSkipStage>(
                sbe::makeS<sbe::CoScanStage>(root->nodeId()), 1, boost::none, root->nodeId()),
            root->nodeId(),
            innerVar,
            sbe::makeE<sbe::EVariable>(orderBy[idx]));

        auto op = direction[idx] == sbe::value::SortDirection::Ascending
            ? sbe::EPrimBinary::less
            : sbe::EPrimBinary::greater;
        auto minmax = sbe::makeE<sbe::EIf>(
            sbe::makeE<sbe::EPrimBinary>(
                op,
                sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::cmp3w,
                                             sbe::makeE<sbe::EVariable>(innerVar),
                                             sbe::makeE<sbe::EVariable>(resultVar)),
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                           sbe::value::bitcastFrom<int64_t>(0))),
            sbe::makeE<sbe::EVariable>(innerVar),
            sbe::makeE<sbe::EVariable>(resultVar));

        inputStage = sbe::makeS<sbe::TraverseStage>(std::move(inputStage),
                                                    std::move(innerBranch),
                                                    orderBy[idx],
                                                    resultVar,
                                                    innerVar,
                                                    sbe::makeSV(),
                                                    std::move(minmax),
                                                    nullptr,
                                                    root->nodeId(),
                                                    boost::none);
        orderBy[idx] = resultVar;
    }

    if (auto recordIdSlot = outputs.getIfExists(kRecordId); recordIdSlot) {
        // Break ties with record id if available.
        orderBy.push_back(*recordIdSlot);
        // This is arbitrary.
        direction.push_back(sbe::value::SortDirection::Ascending);
    }

    auto forwardingReqs = reqs.copy().set(kResult).clear(kRecordId);

    auto values = sbe::makeSV();
    outputs.forEachSlot(forwardingReqs, [&](auto&& slot) { values.push_back(slot); });

    inputStage =
        sbe::makeS<sbe::SortStage>(std::move(inputStage),
                                   std::move(orderBy),
                                   std::move(direction),
                                   std::move(values),
                                   sn->limit ? sn->limit : std::numeric_limits<std::size_t>::max(),
                                   sn->maxMemoryUsageBytes,
                                   _cq.getExpCtx()->allowDiskUse,
                                   root->nodeId());

    return {std::move(inputStage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildSortKeyGeneraror(const QuerySolutionNode* root,
                                             const PlanStageReqs& reqs) {
    uasserted(4822883, "Sort key generator in not supported in SBE yet");
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildSortMerge(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    using namespace std::literals;
    invariant(!reqs.getIndexKeyBitset());

    auto mergeSortNode = static_cast<const MergeSortNode*>(root);

    uassert(5073803,
            "SORT_MERGE stage with unfetched children not supported",
            mergeSortNode->fetched());

    const auto sortPattern = SortPattern{mergeSortNode->sort, _cq.getExpCtx()};
    std::vector<sbe::value::SortDirection> direction;

    for (const auto& part : sortPattern) {
        uassert(4822881, "Sorting by expression not supported", !part.expression);
        uassert(4822882,
                "Sorting by dotted paths not supported",
                part.fieldPath && part.fieldPath->getPathLength() == 1);

        direction.push_back(part.isAscending ? sbe::value::SortDirection::Ascending
                                             : sbe::value::SortDirection::Descending);
    }

    std::vector<std::unique_ptr<sbe::PlanStage>> inputStages;
    std::vector<sbe::value::SlotVector> inputKeys;
    std::vector<sbe::value::SlotVector> inputVals;

    // Children must produce all of the slots required by the parent of this SortMergeNode. In
    // addition to that, children must always produce a 'recordIdSlot' if the 'dedup' flag is true,
    // and children must always produce a 'resultSlot' because it's needed by the sort logic below.
    auto childReqs = reqs.copy().set(kResult).setIf(kRecordId, mergeSortNode->dedup);

    for (auto&& child : mergeSortNode->children) {
        auto [stage, outputs] = build(child, childReqs);

        sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projectMap;
        sbe::value::SlotVector inputKeysForChild;

        for (const auto& part : sortPattern) {
            // Slot holding the sort key.
            auto sortFieldSlot{_slotIdGenerator.generate()};
            inputKeysForChild.push_back(sortFieldSlot);

            auto fieldName = part.fieldPath->getFieldName(0);
            auto fieldNameSV = std::string_view{fieldName.rawData(), fieldName.size()};
            projectMap.emplace(sortFieldSlot,
                               sbe::makeE<sbe::EFunction>(
                                   "getField"sv,
                                   sbe::makeEs(sbe::makeE<sbe::EVariable>(outputs.get(kResult)),
                                               sbe::makeE<sbe::EConstant>(fieldNameSV))));
        }

        stage =
            sbe::makeS<sbe::ProjectStage>(std::move(stage), std::move(projectMap), root->nodeId());

        inputStages.push_back(std::move(stage));

        invariant(outputs.has(kResult));
        invariant(!mergeSortNode->dedup || outputs.has(kRecordId));

        inputKeys.push_back(std::move(inputKeysForChild));

        auto sv = sbe::makeSV();
        outputs.forEachSlot(childReqs, [&](auto&& slot) { sv.push_back(slot); });

        inputVals.push_back(std::move(sv));
    }

    auto outputVals = sbe::makeSV();

    PlanStageSlots outputs(childReqs, &_slotIdGenerator);
    outputs.forEachSlot(childReqs, [&](auto&& slot) { outputVals.push_back(slot); });

    auto stage = sbe::makeS<sbe::SortedMergeStage>(std::move(inputStages),
                                                   std::move(inputKeys),
                                                   std::move(direction),
                                                   std::move(inputVals),
                                                   std::move(outputVals),
                                                   root->nodeId());

    if (mergeSortNode->dedup) {
        stage = sbe::makeS<sbe::UniqueStage>(
            std::move(stage), sbe::makeSV(outputs.get(kRecordId)), root->nodeId());
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildProjectionSimple(const QuerySolutionNode* root,
                                             const PlanStageReqs& reqs) {
    using namespace std::literals;
    invariant(!reqs.getIndexKeyBitset());

    auto pn = static_cast<const ProjectionNodeSimple*>(root);

    // The child must produce all of the slots required by the parent of this ProjectionNodeSimple.
    // In addition to that, the child must always produce a 'resultSlot' because it's needed by the
    // projection logic below.
    auto childReqs = reqs.copy().set(kResult);
    auto [inputStage, outputs] = build(pn->children[0], childReqs);

    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projections;
    sbe::value::SlotVector fieldSlots;

    for (const auto& field : pn->proj.getRequiredFields()) {
        fieldSlots.push_back(_slotIdGenerator.generate());
        projections.emplace(
            fieldSlots.back(),
            sbe::makeE<sbe::EFunction>("getField"sv,
                                       sbe::makeEs(sbe::makeE<sbe::EVariable>(outputs.get(kResult)),
                                                   sbe::makeE<sbe::EConstant>(std::string_view{
                                                       field.c_str(), field.size()}))));
    }

    outputs.set(kResult, _slotIdGenerator.generate());
    inputStage = sbe::makeS<sbe::MakeObjStage>(sbe::makeS<sbe::ProjectStage>(std::move(inputStage),
                                                                             std::move(projections),
                                                                             root->nodeId()),
                                               outputs.get(kResult),
                                               boost::none,
                                               std::vector<std::string>{},
                                               pn->proj.getRequiredFields(),
                                               fieldSlots,
                                               true,
                                               false,
                                               root->nodeId());

    return {std::move(inputStage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildProjectionCovered(const QuerySolutionNode* root,
                                              const PlanStageReqs& reqs) {
    using namespace std::literals;
    invariant(!reqs.getIndexKeyBitset());

    auto pn = static_cast<const ProjectionNodeCovered*>(root);
    invariant(pn->proj.isSimple());

    // For now, we only support ProjectionNodeCovered when its child is an IndexScanNode.
    uassert(5037301,
            str::stream() << "Can't build exec tree for node: " << root->toString(),
            pn->children[0]->getType() == STAGE_IXSCAN);

    // This is a ProjectionCoveredNode, so we will be pulling all the data we need from one index.
    // Prepare a bitset to indicate which parts of the index key we need for the projection.
    std::vector<std::string> keyFieldNames;
    StringSet requiredFields = {pn->proj.getRequiredFields().begin(),
                                pn->proj.getRequiredFields().end()};

    // The child must produce all of the slots required by the parent of this ProjectionNodeSimple,
    // except for 'resultSlot' which will be produced by the MakeObjStage below. In addition to
    // that, the child must produce the index key slots that are needed by this covered projection.
    //
    // pn->coveredKeyObj is the "index.keyPattern" from the child (which is either an IndexScanNode
    // or DistinctNode). pn->coveredKeyObj lists all the fields that the index can provide, not the
    // fields that the projection wants. requiredFields lists all of the fields that the projection
    // needs. Since this is a covered projection, we're guaranteed that pn->coveredKeyObj contains
    // all of the fields that the projection needs.
    auto childReqs = reqs.copy().clear(kResult);

    sbe::IndexKeysInclusionSet indexKeyBitset;
    size_t i = 0;
    for (auto&& elt : pn->coveredKeyObj) {
        if (requiredFields.count(elt.fieldNameStringData())) {
            indexKeyBitset.set(i);
            keyFieldNames.push_back(elt.fieldName());
        }

        ++i;
    }

    childReqs.getIndexKeyBitset() = std::move(indexKeyBitset);

    auto [inputStage, outputs] = build(pn->children[0], childReqs);

    // Assert that the index scan produced index key slots for this covered projection.
    auto indexKeySlots = *outputs.extractIndexKeySlots();

    outputs.set(kResult, _slotIdGenerator.generate());
    inputStage = sbe::makeS<sbe::MakeObjStage>(std::move(inputStage),
                                               outputs.get(kResult),
                                               boost::none,
                                               std::vector<std::string>{},
                                               std::move(keyFieldNames),
                                               std::move(indexKeySlots),
                                               true,
                                               false,
                                               root->nodeId());

    return {std::move(inputStage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildProjectionDefault(const QuerySolutionNode* root,
                                              const PlanStageReqs& reqs) {
    using namespace std::literals;
    invariant(!reqs.getIndexKeyBitset());

    auto pn = static_cast<const ProjectionNodeDefault*>(root);

    // The child must produce all of the slots required by the parent of this ProjectionNodeDefault.
    // In addition to that, the child must always produce a 'resultSlot' because it's needed by the
    // projection logic below.
    auto childReqs = reqs.copy().set(kResult);
    auto [inputStage, outputs] = build(pn->children[0], childReqs);

    auto [slot, stage] = generateProjection(_opCtx,
                                            &pn->proj,
                                            std::move(inputStage),
                                            &_slotIdGenerator,
                                            &_frameIdGenerator,
                                            outputs.get(kResult),
                                            _data.env,
                                            root->nodeId());
    outputs.set(kResult, slot);

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildOr(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    invariant(!reqs.getIndexKeyBitset());

    std::vector<std::unique_ptr<sbe::PlanStage>> inputStages;
    std::vector<sbe::value::SlotVector> inputSlots;

    auto orn = static_cast<const OrNode*>(root);

    // Children must produce all of the slots required by the parent of this OrNode. In addition
    // to that, children must always produce a 'recordIdSlot' if the 'dedup' flag is true, and
    // children must always produce a 'resultSlot' if 'filter' is non-null.
    auto childReqs = reqs.copy().setIf(kResult, orn->filter.get()).setIf(kRecordId, orn->dedup);

    for (auto&& child : orn->children) {
        auto [stage, outputs] = build(child, childReqs);

        auto sv = sbe::makeSV();
        outputs.forEachSlot(childReqs, [&](auto&& slot) { sv.push_back(slot); });

        inputStages.push_back(std::move(stage));
        inputSlots.emplace_back(std::move(sv));
    }

    // Construct a union stage whose branches are translated children of the 'Or' node.
    auto unionOutputSlots = sbe::makeSV();

    PlanStageSlots outputs(childReqs, &_slotIdGenerator);
    outputs.forEachSlot(childReqs, [&](auto&& slot) { unionOutputSlots.push_back(slot); });

    auto stage = sbe::makeS<sbe::UnionStage>(
        std::move(inputStages), std::move(inputSlots), std::move(unionOutputSlots), root->nodeId());

    if (orn->dedup) {
        stage = sbe::makeS<sbe::UniqueStage>(
            std::move(stage), sbe::makeSV(outputs.get(kRecordId)), root->nodeId());
    }

    if (orn->filter) {
        auto relevantSlots = sbe::makeSV(outputs.get(kResult));

        auto forwardingReqs = reqs.copy().clear(kResult);
        outputs.forEachSlot(forwardingReqs, [&](auto&& slot) { relevantSlots.push_back(slot); });

        stage = generateFilter(_opCtx,
                               orn->filter.get(),
                               std::move(stage),
                               &_slotIdGenerator,
                               &_frameIdGenerator,
                               outputs.get(kResult),
                               _data.env,
                               std::move(relevantSlots),
                               root->nodeId());
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildText(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    invariant(_collection);
    invariant(!reqs.getIndexKeyBitset());

    // At present, makeLoopJoinForFetch() doesn't have the necessary logic for producing an
    // oplogTsSlot, so assert that the caller doesn't need oplogTsSlot.
    invariant(!reqs.has(kOplogTs));

    auto textNode = static_cast<const TextNode*>(root);

    auto&& indexName = textNode->index.identifier.catalogName;
    const auto desc = _collection->getIndexCatalog()->findIndexByName(_opCtx, indexName);
    invariant(desc);
    const auto accessMethod = static_cast<const FTSAccessMethod*>(
        _collection->getIndexCatalog()->getEntry(desc)->accessMethod());
    invariant(accessMethod);
    auto&& ftsSpec = accessMethod->getSpec();

    // We assume here that node->ftsQuery is an FTSQueryImpl, not an FTSQueryNoop. In practice, this
    // means that it is illegal to use the StageBuilder on a QuerySolution created by planning a
    // query that contains "no-op" expressions.
    auto ftsQuery = static_cast<fts::FTSQueryImpl&>(*textNode->ftsQuery);

    // A vector of the output slots for each index scan stage. Each stage outputs a record id and a
    // record, so we expect each inner vector to be of length two.
    std::vector<sbe::value::SlotVector> ixscanOutputSlots;

    const bool forward = true;
    const bool inclusive = true;
    auto makeKeyString = [&](const BSONObj& bsonKey) {
        return std::make_unique<KeyString::Value>(
            IndexEntryComparison::makeKeyStringFromBSONKeyForSeek(
                bsonKey,
                accessMethod->getSortedDataInterface()->getKeyStringVersion(),
                accessMethod->getSortedDataInterface()->getOrdering(),
                forward,
                inclusive));
    };

    std::vector<std::unique_ptr<sbe::PlanStage>> indexScanList;
    for (const auto& term : ftsQuery.getTermsForBounds()) {
        // TODO: Should we scan in the opposite direction?
        auto startKeyBson = fts::FTSIndexFormat::getIndexKey(
            0, term, textNode->indexPrefix, ftsSpec.getTextIndexVersion());
        auto endKeyBson = fts::FTSIndexFormat::getIndexKey(
            fts::MAX_WEIGHT, term, textNode->indexPrefix, ftsSpec.getTextIndexVersion());

        auto&& [recordIdSlot, ixscan] =
            generateSingleIntervalIndexScan(_collection,
                                            indexName,
                                            forward,
                                            makeKeyString(startKeyBson),
                                            makeKeyString(endKeyBson),
                                            sbe::IndexKeysInclusionSet{},
                                            sbe::makeSV(),
                                            boost::none,  // recordSlot
                                            &_slotIdGenerator,
                                            _yieldPolicy,
                                            root->nodeId(),
                                            _lockAcquisitionCallback);
        indexScanList.push_back(std::move(ixscan));
        ixscanOutputSlots.push_back(sbe::makeSV(recordIdSlot));
    }

    PlanStageSlots outputs;

    // Union will output a slot for the record id and another for the record.
    auto recordIdSlot = _slotIdGenerator.generate();
    auto unionOutputSlots = sbe::makeSV(recordIdSlot);

    // Index scan output slots become the input slots to the union.
    auto stage = sbe::makeS<sbe::UnionStage>(
        std::move(indexScanList), ixscanOutputSlots, unionOutputSlots, root->nodeId());

    // TODO: If text score metadata is requested, then we should sum over the text scores inside the
    // index keys for a given document. This will require expression evaluation to be able to
    // extract the score directly from the key string.
    stage =
        sbe::makeS<sbe::UniqueStage>(std::move(stage), sbe::makeSV(recordIdSlot), root->nodeId());

    sbe::value::SlotId resultSlot;
    std::tie(resultSlot, recordIdSlot, stage) =
        makeLoopJoinForFetch(std::move(stage), recordIdSlot, root->nodeId());

    // Add a special stage to apply 'ftsQuery' to matching documents, and then add a FilterStage to
    // discard documents which do not match.
    auto textMatchResultSlot = _slotIdGenerator.generate();
    stage = sbe::makeS<sbe::TextMatchStage>(
        std::move(stage), ftsQuery, ftsSpec, resultSlot, textMatchResultSlot, root->nodeId());

    // Filter based on the contents of the slot filled out by the TextMatchStage.
    stage = sbe::makeS<sbe::FilterStage<false>>(
        std::move(stage), sbe::makeE<sbe::EVariable>(textMatchResultSlot), root->nodeId());

    outputs.set(kResult, resultSlot);
    outputs.set(kRecordId, recordIdSlot);

    if (reqs.has(kReturnKey)) {
        // Assign the 'returnKeySlot' to be the empty object.
        outputs.set(kReturnKey, _slotIdGenerator.generate());
        stage = sbe::makeProjectStage(std::move(stage),
                                      root->nodeId(),
                                      outputs.get(kReturnKey),
                                      sbe::makeE<sbe::EFunction>("newObj", sbe::makeEs()));
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildReturnKey(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    invariant(!reqs.getIndexKeyBitset());

    // TODO SERVER-49509: If the projection includes {$meta: "sortKey"}, the result of this stage
    // should also include the sort key. Everything else in the projection is ignored.
    auto returnKeyNode = static_cast<const ReturnKeyNode*>(root);

    // The child must produce all of the slots required by the parent of this ReturnKeyNode except
    // for 'resultSlot'. In addition to that, the child must always produce a 'returnKeySlot'.
    // After build() returns, we take the 'returnKeySlot' produced by the child and store it into
    // 'resultSlot' for the parent of this ReturnKeyNode to consume.
    auto childReqs = reqs.copy().clear(kResult).set(kReturnKey);
    auto [stage, outputs] = build(returnKeyNode->children[0], childReqs);

    outputs.set(kResult, outputs.get(kReturnKey));
    outputs.clear(kReturnKey);

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildEof(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projects;

    PlanStageSlots outputs(reqs, &_slotIdGenerator);
    outputs.forEachSlot(reqs, [&](auto&& slot) {
        projects.insert({slot, sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Nothing, 0)});
    });

    auto stage = sbe::makeS<sbe::LimitSkipStage>(
        sbe::makeS<sbe::CoScanStage>(root->nodeId()), 0, boost::none, root->nodeId());

    if (!projects.empty()) {
        // Even though this SBE tree will produce zero documents, we still need a ProjectStage to
        // define the slots in 'outputSlots' so that calls to getAccessor() won't fail.
        stage =
            sbe::makeS<sbe::ProjectStage>(std::move(stage), std::move(projects), root->nodeId());
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::makeUnionForTailableCollScan(const QuerySolutionNode* root,
                                                    const PlanStageReqs& reqs) {
    using namespace std::literals;
    invariant(!reqs.getIndexKeyBitset());

    // Register a SlotId in the global environment which would contain a recordId to resume a
    // tailable collection scan from. A PlanStage executor will track the last seen recordId and
    // will reset a SlotAccessor for the resumeRecordIdSlot with this recordId.
    auto resumeRecordIdSlot = _data.env->registerSlot(
        "resumeRecordId"_sd, sbe::value::TypeTags::Nothing, 0, false, &_slotIdGenerator);

    // For tailable collection scan we need to build a special union sub-tree consisting of two
    // branches:
    //   1) An anchor branch implementing an initial collection scan before the first EOF is hit.
    //   2) A resume branch implementing all consecutive collection scans from a recordId which was
    //      seen last.
    //
    // The 'makeStage' parameter is used to build a PlanStage tree which is served as a root stage
    // for each of the union branches. The same machanism is used to build each union branch, and
    // the special logic which needs to be triggered depending on which branch we build is
    // controlled by setting the isTailableCollScanResumeBranch flag in PlanStageReqs.
    auto makeUnionBranch = [&](bool isTailableCollScanResumeBranch)
        -> std::pair<sbe::value::SlotVector, std::unique_ptr<sbe::PlanStage>> {
        auto childReqs = reqs;
        childReqs.setIsTailableCollScanResumeBranch(isTailableCollScanResumeBranch);
        auto [branch, outputs] = build(root, childReqs);

        auto branchSlots = sbe::makeSV();
        outputs.forEachSlot(reqs, [&](auto&& slot) { branchSlots.push_back(slot); });

        return {std::move(branchSlots), std::move(branch)};
    };

    // Build an anchor branch of the union and add a constant filter on top of it, so that it would
    // only execute on an initial collection scan, that is, when resumeRecordId is not available
    // yet.
    auto&& [anchorBranchSlots, anchorBranch] = makeUnionBranch(false);
    anchorBranch = sbe::makeS<sbe::FilterStage<true>>(
        std::move(anchorBranch),
        sbe::makeE<sbe::EPrimUnary>(
            sbe::EPrimUnary::logicNot,
            sbe::makeE<sbe::EFunction>(
                "exists"sv, sbe::makeEs(sbe::makeE<sbe::EVariable>(resumeRecordIdSlot)))),
        root->nodeId());

    // Build a resume branch of the union and add a constant filter on op of it, so that it would
    // only execute when we resume a collection scan from the resumeRecordId.
    auto&& [resumeBranchSlots, resumeBranch] = makeUnionBranch(true);
    resumeBranch = sbe::makeS<sbe::FilterStage<true>>(
        sbe::makeS<sbe::LimitSkipStage>(std::move(resumeBranch), boost::none, 1, root->nodeId()),
        sbe::makeE<sbe::EFunction>("exists"sv,
                                   sbe::makeEs(sbe::makeE<sbe::EVariable>(resumeRecordIdSlot))),
        root->nodeId());

    invariant(anchorBranchSlots.size() == resumeBranchSlots.size());

    // A vector of the output slots for each union branch.
    auto branchSlots = makeVector<sbe::value::SlotVector>(std::move(anchorBranchSlots),
                                                          std::move(resumeBranchSlots));

    auto unionOutputSlots = sbe::makeSV();

    PlanStageSlots outputs(reqs, &_slotIdGenerator);
    outputs.forEachSlot(reqs, [&](auto&& slot) { unionOutputSlots.push_back(slot); });

    // Branch output slots become the input slots to the union.
    auto unionStage =
        sbe::makeS<sbe::UnionStage>(makeVector<std::unique_ptr<sbe::PlanStage>>(
                                        std::move(anchorBranch), std::move(resumeBranch)),
                                    branchSlots,
                                    unionOutputSlots,
                                    root->nodeId());

    return {std::move(unionStage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildShardFilter(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    using namespace std::literals;

    const auto filterNode = static_cast<const ShardingFilterNode*>(root);

    uassert(5071201,
            "STAGE_SHARD_FILTER is curently only supported in SBE for collection scan plans",
            filterNode->children[0]->getType() == StageType::STAGE_COLLSCAN ||
                filterNode->children[0]->getType() == StageType::STAGE_VIRTUAL_SCAN);

    auto childReqs = reqs.copy().set(kResult);
    auto [stage, outputs] = build(filterNode->children[0], childReqs);

    // If we're sharded make sure that we don't return data that isn't owned by the shard. This
    // situation can occur when pending documents from in-progress migrations are inserted and when
    // there are orphaned documents from aborted migrations. To check if the document is owned by
    // the shard, we need to own a 'ShardFilterer', and extract the document's shard key as a
    // BSONObj.
    auto shardFilterer = _shardFiltererFactory->makeShardFilterer(_opCtx);

    // Build an expression to extract the shard key from the document based on the shard key
    // pattern. To do this, we iterate over the shard key pattern parts and build nested 'getField'
    // expressions. This will handle single-element paths, and dotted paths for each shard key part.
    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projections;
    sbe::value::SlotVector fieldSlots;
    std::vector<std::string> projectFields;
    std::unique_ptr<sbe::EExpression> bindShardKeyPart;

    BSONObjIterator keyPatternIter(shardFilterer->getKeyPattern().toBSON());
    while (auto keyPatternElem = keyPatternIter.next()) {
        auto fieldRef = FieldRef{keyPatternElem.fieldNameStringData()};
        fieldSlots.push_back(_slotIdGenerator.generate());
        projectFields.push_back(fieldRef.dottedField().toString());

        auto currentFieldSlot = sbe::makeE<sbe::EVariable>(outputs.get(kResult));
        auto shardKeyBinding =
            generateShardKeyBinding(fieldRef, _frameIdGenerator, std::move(currentFieldSlot), 0);

        projections.emplace(fieldSlots.back(), std::move(shardKeyBinding));
    }

    auto shardKeySlot{_slotIdGenerator.generate()};

    // Build an object which will hold a flattened shard key from the projections above.
    auto shardKeyObjStage = sbe::makeS<sbe::MakeObjStage>(
        sbe::makeS<sbe::ProjectStage>(std::move(stage), std::move(projections), root->nodeId()),
        shardKeySlot,
        boost::none,
        std::vector<std::string>{},
        projectFields,
        fieldSlots,
        true,
        false,
        root->nodeId());

    // Build a project stage that checks if any of the fieldSlots for the shard key parts are an
    // Array which is represented by Nothing.
    invariant(fieldSlots.size() > 0);
    auto arrayChecks = makeNot(sbe::makeE<sbe::EFunction>(
        "exists", sbe::makeEs(sbe::makeE<sbe::EVariable>(fieldSlots[0]))));
    for (size_t ind = 1; ind < fieldSlots.size(); ++ind) {
        arrayChecks = sbe::makeE<sbe::EPrimBinary>(
            sbe::EPrimBinary::Op::logicOr,
            std::move(arrayChecks),
            makeNot(sbe::makeE<sbe::EFunction>(
                "exists", sbe::makeEs(sbe::makeE<sbe::EVariable>(fieldSlots[ind])))));
    }
    arrayChecks = sbe::makeE<sbe::EIf>(std::move(arrayChecks),
                                       sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Nothing, 0),
                                       sbe::makeE<sbe::EVariable>(shardKeySlot));

    auto finalShardKeySlot{_slotIdGenerator.generate()};

    auto finalShardKeyObjStage = makeProjectStage(
        std::move(shardKeyObjStage), root->nodeId(), finalShardKeySlot, std::move(arrayChecks));

    // Build a 'FilterStage' to skip over documents that don't belong to the shard. Shard membership
    // of the document is checked by invoking 'shardFilter' with the owned 'ShardFilterer' along
    // with the shard key that sits in the 'finalShardKeySlot' of 'MakeObjStage'.
    auto shardFilterFn = sbe::makeE<sbe::EFunction>(
        "shardFilter"sv,
        sbe::makeEs(sbe::makeE<sbe::EConstant>(
                        sbe::value::TypeTags::shardFilterer,
                        sbe::value::bitcastFrom<ShardFilterer*>(shardFilterer.release())),
                    sbe::makeE<sbe::EVariable>(finalShardKeySlot)));

    return {sbe::makeS<sbe::FilterStage<false>>(
                std::move(finalShardKeyObjStage), std::move(shardFilterFn), root->nodeId()),
            std::move(outputs)};
}

// Returns a non-null pointer to the root of a plan tree, or a non-OK status if the PlanStage tree
// could not be constructed.
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::build(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    static const stdx::unordered_map<
        StageType,
        std::function<std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>(
            SlotBasedStageBuilder&, const QuerySolutionNode* root, const PlanStageReqs& reqs)>>
        kStageBuilders = {
            {STAGE_COLLSCAN, &SlotBasedStageBuilder::buildCollScan},
            {STAGE_VIRTUAL_SCAN, &SlotBasedStageBuilder::buildVirtualScan},
            {STAGE_IXSCAN, &SlotBasedStageBuilder::buildIndexScan},
            {STAGE_FETCH, &SlotBasedStageBuilder::buildFetch},
            {STAGE_LIMIT, &SlotBasedStageBuilder::buildLimit},
            {STAGE_SKIP, &SlotBasedStageBuilder::buildSkip},
            {STAGE_SORT_SIMPLE, &SlotBasedStageBuilder::buildSort},
            {STAGE_SORT_DEFAULT, &SlotBasedStageBuilder::buildSort},
            {STAGE_SORT_KEY_GENERATOR, &SlotBasedStageBuilder::buildSortKeyGeneraror},
            {STAGE_PROJECTION_SIMPLE, &SlotBasedStageBuilder::buildProjectionSimple},
            {STAGE_PROJECTION_DEFAULT, &SlotBasedStageBuilder::buildProjectionDefault},
            {STAGE_PROJECTION_COVERED, &SlotBasedStageBuilder::buildProjectionCovered},
            {STAGE_OR, &SlotBasedStageBuilder::buildOr},
            {STAGE_TEXT, &SlotBasedStageBuilder::buildText},
            {STAGE_RETURN_KEY, &SlotBasedStageBuilder::buildReturnKey},
            {STAGE_EOF, &SlotBasedStageBuilder::buildEof},
            {STAGE_SORT_MERGE, &SlotBasedStageBuilder::buildSortMerge},
            {STAGE_SHARDING_FILTER, &SlotBasedStageBuilder::buildShardFilter}};

    uassert(4822884,
            str::stream() << "Can't build exec tree for node: " << root->toString(),
            kStageBuilders.find(root->getType()) != kStageBuilders.end());

    // If this plan is for a tailable cursor scan, and we're not already in the process of building
    // a special union sub-tree implementing such scans, then start building a union sub-tree. Note
    // that LIMIT or SKIP stage is used as a splitting point of the two union branches, if present,
    // because we need to apply limit (or skip) only in the initial scan (in the anchor branch), and
    // the resume branch should not have it.
    switch (root->getType()) {
        case STAGE_COLLSCAN:
        case STAGE_LIMIT:
        case STAGE_SKIP:
            if (_cq.getQueryRequest().isTailable() &&
                !reqs.getIsBuildingUnionForTailableCollScan()) {
                auto childReqs = reqs;
                childReqs.setIsBuildingUnionForTailableCollScan(true);
                return makeUnionForTailableCollScan(root, childReqs);
            }
        default:
            break;
    }

    return std::invoke(kStageBuilders.at(root->getType()), *this, root, reqs);
}
}  // namespace mongo::stage_builder
