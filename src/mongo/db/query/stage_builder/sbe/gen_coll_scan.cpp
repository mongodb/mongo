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


#include <cstddef>
#include <set>
#include <tuple>

#include <absl/container/inlined_vector.h>
#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/matcher/match_expression_dependencies.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/sbe/gen_coll_scan.h"
#include "mongo/db/query/stage_builder/sbe/gen_filter.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr_helpers.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo::stage_builder {
namespace {

void openCallback(OperationContext* opCtx, const CollectionPtr& collection) {
    // Forward, non-tailable scans from the oplog need to wait until all oplog entries
    // before the read begins to be visible. This isn't needed for reverse scans because
    // we only hide oplog entries from forward scans, and it isn't necessary for tailing
    // cursors because they ignore EOF and will eventually see all writes. Forward,
    // non-tailable scans are the only case where a meaningful EOF will be seen that
    // might not include writes that finished before the read started. This also must be
    // done before we create the cursor as that is when we establish the endpoint for
    // the cursor. Also call abandonSnapshot to make sure that we are using a fresh
    // storage engine snapshot while waiting. Otherwise, we will end up reading from the
    // snapshot where the oplog entries are not yet visible even after the wait.
    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    storageEngine->waitForAllEarlierOplogWritesToBeVisible(opCtx, collection->getRecordStore());
}

/**
 * Checks whether a callback function should be created for a ScanStage and returns it, if so. The
 * logic in the provided callback will be executed when the ScanStage is opened (but not reopened).
 */
sbe::ScanOpenCallback makeOpenCallbackIfNeeded(const CollectionPtr& collection,
                                               const CollectionScanNode* csn) {
    if (csn->direction == CollectionScanParams::FORWARD && csn->shouldWaitForOplogVisibility) {
        tassert(7714200, "Expected 'tailable' to be false", !csn->tailable);
        tassert(7714201, "Expected 'collection' to be the oplog", collection->ns().isOplog());

        return &openCallback;
    } else {
        return nullptr;
    }
}

// If the scan should be started after the provided resume RecordId, we will construct a subtree
// to project out the resume RecordId. This subtree will also perform a check to ensure that the
// record we're trying to reposition to exists. The caller can then use a LoopJoinStage to combine
// this subtree wite a ScanStage to actually perform the seek.
//
//   limit 1
//      union [outSeekSlot]
//         [seekSlot]
//            nlj
//               left
//                  project seekSlot = <resumeRecordIdExpr>
//                  limit 1
//                  coscan
//               right
//                  seek seekSlot ...
//         [unusedSlot]
//            project unusedSlot = efail(KeyNotFound)
//            limit 1
//            coscan
//
std::pair<SbStage, SbSlot> buildResumeFromRecordIdTree(StageBuilderState& state,
                                                       const CollectionPtr& collection,
                                                       const CollectionScanNode* csn,
                                                       SbExpr resumeRecordIdExpr,
                                                       bool isResumingTailableScan) {
    SbBuilder b(state, csn->nodeId());

    const auto forward = csn->direction == CollectionScanParams::FORWARD;
    invariant(!resumeRecordIdExpr.isNull());

    // Project out the RecordId we want to resume from as 'seekSlot'.
    auto [projStage, projOutSlots] =
        b.makeProject(b.makeLimitOneCoScanTree(), std::move(resumeRecordIdExpr));
    auto seekSlot = projOutSlots[0];

    // Construct a 'seek' branch of the 'union'. If we're succeeded to reposition the cursor,
    // the branch will output the 'seekSlot' to start the real scan from, otherwise it will
    // produce EOF.
    auto [scanStage, _, __, ___] =
        b.makeScan(collection->uuid(), collection->ns().dbName(), forward, seekSlot);

    auto seekBranch = b.makeLoopJoin(std::move(projStage),
                                     std::move(scanStage),
                                     SbExpr::makeSV(seekSlot),
                                     SbExpr::makeSV(seekSlot));

    // Construct a 'fail' branch of the union. The 'unusedSlot' is needed as each union branch must
    // have the same number of slots, and we use just one in the 'seek' branch above. This branch
    // will only be executed if the 'seek' branch produces EOF, which can only happen if the seek
    // did not find the resume record of a tailable cursor or the record id specified in
    // $_resumeAfter.
    auto [errorCode, errorMessage] = [&]() -> std::pair<ErrorCodes::Error, std::string> {
        if (isResumingTailableScan) {
            return {ErrorCodes::CappedPositionLost,
                    "CollectionScan died due to failure to restore tailable cursor position."};
        }
        return {ErrorCodes::ErrorCodes::KeyNotFound,
                str::stream() << "Failed to resume collection scan: the recordId from which we are "
                                 "attempting to resume no longer exists in the collection: "
                              << csn->resumeAfterRecordId};
    }();

    auto [failBranch, failOutSlots] =
        b.makeProject(b.makeLimitOneCoScanTree(), b.makeFail(errorCode, errorMessage));
    auto unusedSlot = failOutSlots[0];

    std::vector<SbSlotVector> inputVals;
    inputVals.emplace_back(SbExpr::makeSV(seekSlot));
    inputVals.emplace_back(SbExpr::makeSV(unusedSlot));

    // Construct a union stage from the 'seek' and 'fail' branches, and then add a 'limit 1'
    // stage so that the tree will produce just a single seek recordId.
    auto [unionStage, outputVals] =
        b.makeUnion(sbe::makeSs(std::move(seekBranch), std::move(failBranch)), inputVals);

    auto outStage = b.makeLimit(std::move(unionStage), b.makeInt64Constant(1));

    auto outSeekSlot = outputVals[0];

    return {std::move(outStage), outSeekSlot};
}

SbStage combineResumeRecordIdTreeWithScan(StageBuilderState& state,
                                          PlanNodeId nodeId,
                                          SbStage resumeRecordIdTree,
                                          SbStage scanStage,
                                          SbSlot seekSlot,
                                          bool isResumingTailableScan) {
    SbBuilder b(state, nodeId);

    // Construct the final loop join. Note that for the resume branch of a tailable cursor case
    // we use the 'seek' stage as an inner branch, since we need to produce all records starting
    // from the supplied position. For a resume token case we also inject a 'skip 1' stage on
    // top of the inner branch, as we need to start _after_ the resume RecordId.
    if (!isResumingTailableScan) {
        scanStage = b.makeLimitSkip(std::move(scanStage), {} /*limit*/, b.makeInt64Constant(1));
    }

    return b.makeLoopJoin(std::move(resumeRecordIdTree),
                          std::move(scanStage),
                          SbSlotVector{},
                          SbExpr::makeSV(seekSlot));
}

/**
 * Creates a collection scan sub-tree optimized for clustered collection scans. Should only be
 * called on clustered collections. We can build an optimized scan when any of the following
 * scenarios apply:
 *
 * 1. 'csn->minRecord' and/or 'csn->maxRecord' exist.
 *    1.1 CollectionScanParams::FORWARD scan:
 *        a. If 'csn->minRecord' is present, the collection scan will seek directly to the RecordId
 *           of a record as close to this lower bound as possible without going higher.
 *        b. If 'csn->maxRecord' is present, the collection scan will stop and return EOF the first
 *           time it fetches a document greater than this upper bound.
 *    1.2 CollectionScanParams::BACKWARD scan:
 *        a. If 'csn->maxRecord' is present, the collection scan will seek directly to the RecordId
 *           of a record as close to this upper bound as possible without going lower.
 *        b. If 'csn->minRecord' is present, the collection scan will stop and return EOF the first
 *           time it fetches a document less than this lower bound.
 * 2. The user request specified a $_resumeAfter RecordId from which to begin the scan AND the scan
 *    is forward AND neither 'csn->minRecord' nor 'csn->maxRecord' exist.
 *    2a. The scan will continue with the next RecordId after $_resumeAfter.
 */
std::pair<SbStage, PlanStageSlots> generateClusteredCollScan(
    StageBuilderState& state,
    const CollectionPtr& collection,
    const CollectionScanNode* csn,
    std::vector<std::string> scanFieldNames,
    bool isResumingTailableScan) {
    SbBuilder b(state, csn->nodeId());

    const bool forward = csn->direction == CollectionScanParams::FORWARD;
    sbe::RuntimeEnvironment* env = state.env.runtimeEnv;

    invariant(csn->doClusteredCollectionScanSbe());
    invariant(!csn->resumeAfterRecordId || forward);
    invariant(!csn->resumeAfterRecordId || !csn->tailable);
    // The minRecord and maxRecord optimizations are not compatible with resumeAfterRecordId.
    invariant(!(csn->resumeAfterRecordId && (csn->minRecord || csn->maxRecord)));
    // 'stopApplyingFilterAfterFirstMatch' is only for oplog scans; this method doesn't do them.
    invariant(!csn->stopApplyingFilterAfterFirstMatch);

    SbStage resumeRecordIdTree;
    boost::optional<SbSlot> seekSlot;

    // Iff this is a resume or fetch, build the subtree to start the scan from the seekRecordId.
    if (isResumingTailableScan || csn->resumeAfterRecordId) {
        // Iff this is a resume or fetch, set 'resumeRecordIdExpr' to the RecordId resume point
        // of the scan.
        SbExpr resumeRecordIdExpr;
        if (isResumingTailableScan) {
            resumeRecordIdExpr = SbSlot{env->getSlot("resumeRecordId"_sd)};
        } else if (csn->resumeAfterRecordId) {
            auto [tag, val] = sbe::value::makeCopyRecordId(*csn->resumeAfterRecordId);
            resumeRecordIdExpr = b.makeConstant(tag, val);
        } else {
            MONGO_UNREACHABLE_TASSERT(9405102);
        }

        auto [outTree, outSlot] = buildResumeFromRecordIdTree(
            state, collection, csn, std::move(resumeRecordIdExpr), isResumingTailableScan);

        resumeRecordIdTree = std::move(outTree);
        seekSlot = outSlot;
    }

    // Create minRecordId and/or maxRecordId slots as needed.
    boost::optional<SbSlot> minRecordSlot;
    boost::optional<SbSlot> maxRecordSlot;
    if (csn->minRecord) {
        auto [tag, val] = sbe::value::makeCopyRecordId(csn->minRecord->recordId());
        minRecordSlot = SbSlot{state.env->registerSlot(tag, val, true, state.slotIdGenerator)};
    }
    if (csn->maxRecord) {
        auto [tag, val] = sbe::value::makeCopyRecordId(csn->maxRecord->recordId());
        maxRecordSlot = SbSlot{state.env->registerSlot(tag, val, true, state.slotIdGenerator)};
    }
    state.data->clusteredCollBoundsInfos.emplace_back(
        ParameterizedClusteredScanSlots{b.lower(minRecordSlot), b.lower(maxRecordSlot)});

    // Create the ScanStage.
    bool includeScanStartRecordId =
        (csn->boundInclusion ==
             CollectionScanParams::ScanBoundInclusion::kIncludeBothStartAndEndRecords ||
         csn->boundInclusion == CollectionScanParams::ScanBoundInclusion::kIncludeStartRecordOnly);
    bool includeScanEndRecordId =
        (csn->boundInclusion ==
             CollectionScanParams::ScanBoundInclusion::kIncludeBothStartAndEndRecords ||
         csn->boundInclusion == CollectionScanParams::ScanBoundInclusion::kIncludeEndRecordOnly);

    SbScanBounds scanBounds;
    scanBounds.minRecordIdSlot = minRecordSlot;
    scanBounds.maxRecordIdSlot = maxRecordSlot;
    scanBounds.includeScanStartRecordId = includeScanStartRecordId;
    scanBounds.includeScanEndRecordId = includeScanEndRecordId;

    auto [stage, resultSlot, recordIdSlot, scanFieldSlots] =
        b.makeScan(collection->uuid(),
                   collection->ns().dbName(),
                   forward,
                   seekSlot,
                   scanFieldNames,  // do not std::move - used later
                   std::move(scanBounds));

    if (isResumingTailableScan || csn->resumeAfterRecordId) {
        stage = combineResumeRecordIdTreeWithScan(state,
                                                  csn->nodeId(),
                                                  std::move(resumeRecordIdTree),
                                                  std::move(stage),
                                                  *seekSlot,
                                                  isResumingTailableScan);
    }

    PlanStageSlots outputs;
    outputs.setResultObj(resultSlot);
    outputs.set(PlanStageSlots::kRecordId, recordIdSlot);
    for (size_t i = 0; i < scanFieldNames.size(); ++i) {
        outputs.set(std::make_pair(PlanStageSlots::kField, scanFieldNames[i]), scanFieldSlots[i]);
    }

    // When the start and/or end scan bounds are from an expression, ScanStage::getNext() treats
    // them both as inclusive, and 'csn->filter' will enforce any exclusions. If the bound(s) came
    // from the "min" (always inclusive) and/or "max" (always exclusive) keywords, there may be no
    // filter, so ScanStage->getNext() must directly enforce the bounds. min's inclusivity matches
    // getNext()'s default behavior, but max's exclusivity does not and thus is enforced by the
    // includeScanEndRecordId argument to the ScanStage constructor above.
    SbExpr filterExpr = generateFilter(state, csn->filter.get(), resultSlot, outputs);
    if (!filterExpr.isNull()) {
        stage = b.makeFilter(std::move(stage), std::move(filterExpr));
    }

    return {std::move(stage), std::move(outputs)};
}  // generateClusteredCollScan

/**
 * Generates a generic collection scan sub-tree.
 *  - If a resume token has been provided, the scan will start from a RecordId contained within this
 *    token.
 *  - Else if 'isResumingTailableScan' is true, the scan will start from a RecordId contained in
 *    slot "resumeRecordId".
 *  - Otherwise the scan will start from the beginning of the collection.
 */
std::pair<SbStage, PlanStageSlots> generateGenericCollScan(StageBuilderState& state,
                                                           const CollectionPtr& collection,
                                                           const CollectionScanNode* csn,
                                                           std::vector<std::string> fields,
                                                           bool isResumingTailableScan) {
    SbBuilder b(state, csn->nodeId());

    const bool forward = csn->direction == CollectionScanParams::FORWARD;

    invariant(!csn->shouldTrackLatestOplogTimestamp || collection->ns().isOplog());
    invariant(!csn->resumeAfterRecordId || forward);
    invariant(!csn->resumeAfterRecordId || !csn->tailable);

    if (csn->filter) {
        DepsTracker deps;
        match_expression::addDependencies(csn->filter.get(), &deps);
        // If the filter predicate doesn't need the whole document, then we take all the top-level
        // fields referenced by the filter predicate and we add them to 'fields'.
        if (!deps.needWholeDocument) {
            auto topLevelFields = getTopLevelFields(deps.fields);
            fields = appendVectorUnique(std::move(fields), std::move(topLevelFields));
        }
    }

    sbe::ScanCallbacks callbacks({}, {}, makeOpenCallbackIfNeeded(collection, csn));

    SbStage resumeRecordIdTree;
    boost::optional<SbSlot> seekSlot;

    if (isResumingTailableScan || csn->resumeAfterRecordId) {
        SbExpr resumeRecordIdExpr;
        if (isResumingTailableScan) {
            resumeRecordIdExpr = SbSlot{state.env->getSlot("resumeRecordId"_sd)};
        } else if (csn->resumeAfterRecordId) {
            auto [tag, val] = sbe::value::makeCopyRecordId(*csn->resumeAfterRecordId);
            resumeRecordIdExpr = b.makeConstant(tag, val);
        } else {
            MONGO_UNREACHABLE_TASSERT(9405103);
        }

        auto [outTree, outSlot] = buildResumeFromRecordIdTree(
            state, collection, csn, std::move(resumeRecordIdExpr), isResumingTailableScan);

        resumeRecordIdTree = std::move(outTree);
        seekSlot = outSlot;
    }

    boost::optional<SbSlot> oplogTsSlot;
    if (csn->shouldTrackLatestOplogTimestamp) {
        // Add the "ts" field to 'fields' if it's not already present.
        std::string tsField = repl::OpTime::kTimestampFieldName.toString();
        fields = appendVectorUnique(std::move(fields), std::vector{std::move(tsField)});

        // Retrieve the "oplogTs" slot so we can pass it to makeScan() below.
        auto oplogTsSlotId = state.getOplogTsSlot();
        oplogTsSlot = oplogTsSlotId ? boost::make_optional(SbSlot{*oplogTsSlotId}) : boost::none;
    }

    auto [stage, resultSlot, recordIdSlot, fieldSlots] = b.makeScan(collection->uuid(),
                                                                    collection->ns().dbName(),
                                                                    forward,
                                                                    seekSlot,
                                                                    fields,
                                                                    SbScanBounds{},
                                                                    SbIndexInfoSlots{},
                                                                    std::move(callbacks),
                                                                    oplogTsSlot);

    if (isResumingTailableScan || csn->resumeAfterRecordId) {
        stage = combineResumeRecordIdTreeWithScan(state,
                                                  csn->nodeId(),
                                                  std::move(resumeRecordIdTree),
                                                  std::move(stage),
                                                  *seekSlot,
                                                  isResumingTailableScan);
    }

    PlanStageSlots outputs;
    outputs.setResultObj(resultSlot);
    outputs.set(PlanStageSlots::kRecordId, recordIdSlot);
    for (size_t i = 0; i < fields.size(); ++i) {
        outputs.set(std::make_pair(PlanStageSlots::kField, fields[i]), fieldSlots[i]);
    }

    if (csn->filter) {
        // 'stopApplyingFilterAfterFirstMatch' is only for oplog scans; this method doesn't do them.
        invariant(!csn->stopApplyingFilterAfterFirstMatch);

        auto filterExpr = generateFilter(state, csn->filter.get(), resultSlot, outputs);
        if (!filterExpr.isNull()) {
            stage = b.makeFilter(std::move(stage), std::move(filterExpr));
        }
    }

    return {std::move(stage), std::move(outputs)};
}  // generateGenericCollScan

}  // namespace

std::pair<SbStage, PlanStageSlots> generateCollScan(StageBuilderState& state,
                                                    const CollectionPtr& collection,
                                                    const CollectionScanNode* csn,
                                                    std::vector<std::string> fields,
                                                    bool isResumingTailableScan) {

    if (csn->doClusteredCollectionScanSbe()) {
        return generateClusteredCollScan(
            state, collection, csn, std::move(fields), isResumingTailableScan);
    } else {
        return generateGenericCollScan(
            state, collection, csn, std::move(fields), isResumingTailableScan);
    }
}
}  // namespace mongo::stage_builder
