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


#include "mongo/platform/basic.h"

#include "mongo/db/query/sbe_stage_builder_coll_scan.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/exchange.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/matcher/match_expression_dependencies.h"
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/db/query/sbe_stage_builder_filter.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo::stage_builder {
namespace {

boost::optional<sbe::value::SlotId> registerOplogTs(sbe::RuntimeEnvironment* env,
                                                    sbe::value::SlotIdGenerator* slotIdGenerator) {
    boost::optional<sbe::value::SlotId> slotId = env->getSlotIfExists("oplogTs"_sd);
    if (!slotId) {
        return env->registerSlot(
            "oplogTs"_sd, sbe::value::TypeTags::Nothing, 0, false, slotIdGenerator);
    }
    return slotId;
}

/**
 * If 'shouldTrackLatestOplogTimestamp' is true, then returns a vector holding the name of the oplog
 * 'ts' field along with another vector holding a SlotId to map this field to, as well as the
 * standalone value of the same SlotId (the latter is returned purely for convenience purposes).
 */
std::tuple<std::vector<std::string>, sbe::value::SlotVector, boost::optional<sbe::value::SlotId>>
makeOplogTimestampSlotIfNeeded(sbe::RuntimeEnvironment* env,
                               sbe::value::SlotIdGenerator* slotIdGenerator,
                               bool shouldTrackLatestOplogTimestamp) {
    if (shouldTrackLatestOplogTimestamp) {
        boost::optional<sbe::value::SlotId> slotId = registerOplogTs(env, slotIdGenerator);
        return {{repl::OpTime::kTimestampFieldName.toString()}, sbe::makeSV(*slotId), slotId};
    }
    return {};
}

/**
 * Checks whether a callback function should be created for a ScanStage and returns it, if so. The
 * logic in the provided callback will be executed when the ScanStage is opened (but not reopened).
 */
sbe::ScanOpenCallback makeOpenCallbackIfNeeded(const CollectionPtr& collection,
                                               const CollectionScanNode* csn) {
    if (csn->direction == CollectionScanParams::FORWARD && csn->shouldWaitForOplogVisibility) {
        invariant(!csn->tailable);
        invariant(collection->ns().isOplog());

        return [](OperationContext* opCtx, const CollectionPtr& collection) {
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

            opCtx->recoveryUnit()->abandonSnapshot();
            collection->getRecordStore()->waitForAllEarlierOplogWritesToBeVisible(opCtx);
        };
    }
    return {};
}

// If the scan should be started after the provided resume RecordId, we will construct a nested-loop
// join sub-tree to project out the resume RecordId and feed it into the inner side (scan). We will
// also construct a union sub-tree as an outer side of the loop join to implement the check that the
// record we're trying to reposition the scan exists.
//
//      nlj [] [seekRecordIdSlot]
//         left
//            limit 1
//            union [seekRecordIdSlot]
//               [seekSlot]
//                  nlj
//                     left
//                        project seekSlot = <seekRecordIdExpression>
//                        limit 1
//                        coscan
//                     right
//                        seek seekSlot ...
//               [unusedSlot]
//                  project unusedSlot = efail(KeyNotFound)
//                  limit 1
//                  coscan
//          right
//            skip 1
//            <inputStage>
std::unique_ptr<sbe::PlanStage> buildResumeFromRecordIdSubtree(
    StageBuilderState& state,
    const CollectionPtr& collection,
    const CollectionScanNode* csn,
    std::unique_ptr<sbe::PlanStage> inputStage,
    sbe::value::SlotId seekRecordIdSlot,
    std::unique_ptr<sbe::EExpression> seekRecordIdExpression,
    PlanYieldPolicy* yieldPolicy,
    bool isTailableResumeBranch,
    bool resumeAfterRecordId) {
    invariant(seekRecordIdExpression);

    const auto forward = csn->direction == CollectionScanParams::FORWARD;
    // Project out the RecordId we want to resume from as 'seekSlot'.
    auto seekSlot = state.slotId();
    auto projStage = sbe::makeProjectStage(
        sbe::makeS<sbe::LimitSkipStage>(
            sbe::makeS<sbe::CoScanStage>(csn->nodeId()), 1, boost::none, csn->nodeId()),
        csn->nodeId(),
        seekSlot,
        std::move(seekRecordIdExpression));

    // Construct a 'seek' branch of the 'union'. If we're succeeded to reposition the cursor,
    // the branch will output the 'seekSlot' to start the real scan from, otherwise it will
    // produce EOF.
    auto seekBranch =
        sbe::makeS<sbe::LoopJoinStage>(std::move(projStage),
                                       sbe::makeS<sbe::ScanStage>(collection->uuid(),
                                                                  boost::none /* recordSlot */,
                                                                  boost::none /* recordIdSlot*/,
                                                                  boost::none /* snapshotIdSlot */,
                                                                  boost::none /* indexIdentSlot */,
                                                                  boost::none /* indexKeySlot */,
                                                                  boost::none /* keyPatternSlot */,
                                                                  boost::none /* oplogTsSlot */,
                                                                  std::vector<std::string>{},
                                                                  sbe::makeSV(),
                                                                  seekSlot,
                                                                  boost::none /* minRecordIdSlot */,
                                                                  boost::none /* maxRecordIdSlot */,
                                                                  forward,
                                                                  yieldPolicy,
                                                                  csn->nodeId(),
                                                                  sbe::ScanCallbacks{}),
                                       sbe::makeSV(seekSlot),
                                       sbe::makeSV(seekSlot),
                                       nullptr,
                                       csn->nodeId());

    // Construct a 'fail' branch of the union. The 'unusedSlot' is needed as each union branch must
    // have the same number of slots, and we use just one in the 'seek' branch above. This branch
    // will only be executed if the 'seek' branch produces EOF, which can only happen if the seek
    // did not find the resume record of a tailable cursor or the record id specified in
    // $_resumeAfter.
    auto unusedSlot = state.slotId();
    auto [errorCode, errorMessage] = [&]() -> std::pair<ErrorCodes::Error, std::string> {
        if (isTailableResumeBranch) {
            return {ErrorCodes::CappedPositionLost,
                    "CollectionScan died due to failure to restore tailable cursor position."};
        }
        return {ErrorCodes::ErrorCodes::KeyNotFound,
                str::stream() << "Failed to resume collection scan the recordId from which we are "
                                 "attempting to resume no longer exists in the collection: "
                              << csn->resumeAfterRecordId};
    }();
    auto failBranch = sbe::makeProjectStage(sbe::makeS<sbe::CoScanStage>(csn->nodeId()),
                                            csn->nodeId(),
                                            unusedSlot,
                                            sbe::makeE<sbe::EFail>(errorCode, errorMessage));

    // Construct a union stage from the 'seek' and 'fail' branches. Note that this stage will ever
    // produce a single call to getNext() due to a 'limit 1' sitting on top of it.
    auto unionStage = sbe::makeS<sbe::UnionStage>(
        sbe::makeSs(std::move(seekBranch), std::move(failBranch)),
        std::vector<sbe::value::SlotVector>{sbe::makeSV(seekSlot), sbe::makeSV(unusedSlot)},
        sbe::makeSV(seekRecordIdSlot),
        csn->nodeId());

    // Construct the final loop join. Note that for the resume branch of a tailable cursor case we
    // use the 'seek' stage as an inner branch, since we need to produce all records starting from
    // the supplied position. For a resume token case we also inject a 'skip 1' stage on top of the
    // inner branch, as we need to start _after_ the resume RecordId. In both cases we inject a
    // 'limit 1' stage on top of the outer branch, as it should produce just a single seek recordId.
    auto innerStage = isTailableResumeBranch || !resumeAfterRecordId
        ? std::move(inputStage)
        : sbe::makeS<sbe::LimitSkipStage>(std::move(inputStage), boost::none, 1, csn->nodeId());
    return sbe::makeS<sbe::LoopJoinStage>(
        sbe::makeS<sbe::LimitSkipStage>(std::move(unionStage), 1, boost::none, csn->nodeId()),
        std::move(innerStage),
        sbe::makeSV(),
        sbe::makeSV(seekRecordIdSlot),
        nullptr,
        csn->nodeId());
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
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> generateClusteredCollScan(
    StageBuilderState& state,
    const CollectionPtr& collection,
    const CollectionScanNode* csn,
    std::vector<std::string> scanFieldNames,
    PlanYieldPolicy* yieldPolicy,
    bool isTailableResumeBranch) {
    const bool forward = csn->direction == CollectionScanParams::FORWARD;
    sbe::RuntimeEnvironment* env = state.data->env;

    invariant(csn->doSbeClusteredCollectionScan());
    invariant(!csn->resumeAfterRecordId || forward);
    invariant(!csn->resumeAfterRecordId || !csn->tailable);
    // The minRecord and maxRecord optimizations are not compatible with resumeAfterRecordId.
    invariant(!(csn->resumeAfterRecordId && (csn->minRecord || csn->maxRecord)));
    // 'stopApplyingFilterAfterFirstMatch' is only for oplog scans; this method doesn't do them.
    invariant(!csn->stopApplyingFilterAfterFirstMatch);

    auto scanFieldSlots = state.slotIdGenerator->generateMultiple(scanFieldNames.size());

    sbe::value::SlotId resultSlot = state.slotId();
    sbe::value::SlotId recordIdSlot = state.slotId();

    // Iff this is a resume or fetch, set 'seekRecordIdSlot' and 'seekRecordIdExpression' to the
    // RecordId resume point of the scan. If we're building a collection scan for a resume branch of
    // a special union sub-tree implementing a tailable cursor scan, we can use the already existing
    // 'resumeRecordIdSlot' directly as the 'seekRecordIdSlot' to access the recordId to resume the
    // scan from. Otherwise we must create a slot for it.
    auto [seekRecordIdSlot, seekRecordIdExpression] =
        [&]() -> std::pair<boost::optional<sbe::value::SlotId>, std::unique_ptr<sbe::EExpression>> {
        if (isTailableResumeBranch) {
            sbe::value::SlotId resumeRecordIdSlot = env->getSlot("resumeRecordId"_sd);
            return {resumeRecordIdSlot, makeVariable(resumeRecordIdSlot)};
        } else if (csn->resumeAfterRecordId) {
            auto [tag, val] = sbe::value::makeCopyRecordId(*csn->resumeAfterRecordId);
            return {state.slotId(), makeConstant(tag, val)};
        }
        return {};
    }();  // lambda end and call

    // Create minRecordId and/or maxRecordId slots as needed.
    boost::optional<sbe::value::SlotId> minRecordSlot;
    boost::optional<sbe::value::SlotId> maxRecordSlot;
    if (csn->minRecord) {
        auto [tag, val] = sbe::value::makeCopyRecordId(csn->minRecord->recordId());
        minRecordSlot = env->registerSlot("minRecordId"_sd, tag, val, true, state.slotIdGenerator);
    }
    if (csn->maxRecord) {
        auto [tag, val] = sbe::value::makeCopyRecordId(csn->maxRecord->recordId());
        maxRecordSlot = env->registerSlot("maxRecordId"_sd, tag, val, true, state.slotIdGenerator);
    }

    // Create the ScanStage.
    bool excludeScanEndRecordId =
        (csn->boundInclusion ==
             CollectionScanParams::ScanBoundInclusion::kExcludeBothStartAndEndRecords ||
         csn->boundInclusion == CollectionScanParams::ScanBoundInclusion::kIncludeStartRecordOnly);
    auto stage = sbe::makeS<sbe::ScanStage>(collection->uuid(),
                                            resultSlot,
                                            recordIdSlot,
                                            boost::none /* snapshotIdSlot */,
                                            boost::none /* indexIdentSlot */,
                                            boost::none /* indexKeySlot */,
                                            boost::none /* keyPatternSlot */,
                                            boost::none /* oplogTsSlot */,
                                            scanFieldNames,  // do not std::move - used later
                                            scanFieldSlots,  // do not std::move - used later
                                            seekRecordIdSlot,
                                            minRecordSlot,
                                            maxRecordSlot,
                                            forward,
                                            yieldPolicy,
                                            csn->nodeId(),
                                            sbe::ScanCallbacks{},
                                            false /* lowPriority default */,
                                            false /* useRandomCursor default */,
                                            true /* participateInTrialRunTracking default */,
                                            excludeScanEndRecordId);

    // Iff this is a resume or fetch, build the subtree to start the scan from the seekRecordId.
    if (seekRecordIdSlot) {
        stage = buildResumeFromRecordIdSubtree(state,
                                               collection,
                                               csn,
                                               std::move(stage),
                                               *seekRecordIdSlot,
                                               std::move(seekRecordIdExpression),
                                               yieldPolicy,
                                               isTailableResumeBranch,
                                               csn->resumeAfterRecordId.has_value());
    }

    // When the start and/or end scan bounds are from an expression, ScanStage::getNext() treats
    // them both as inclusive, and 'csn->filter' will enforce any exclusions. If the bound(s) came
    // from the "min" (always inclusive) and/or "max" (always exclusive) keywords, there may be no
    // filter, so ScanStage->getNext() must directly enforce the bounds. min's inclusivity matches
    // getNext()'s default behavior, but max's exclusivity does not and thus is enforced by the
    // excludeScanEndRecordId argument to the ScanStage constructor above.
    EvalExpr filterExpr = generateFilter(state, csn->filter.get(), resultSlot, nullptr);
    if (!filterExpr.isNull()) {
        stage = sbe::makeS<sbe::FilterStage<false>>(
            std::move(stage), filterExpr.extractExpr(state), csn->nodeId());
    }

    PlanStageSlots outputs;
    outputs.set(PlanStageSlots::kResult, resultSlot);
    outputs.set(PlanStageSlots::kRecordId, recordIdSlot);
    for (size_t i = 0; i < scanFieldNames.size(); ++i) {
        outputs.set(std::make_pair(PlanStageSlots::kField, scanFieldNames[i]), scanFieldSlots[i]);
    }

    return {std::move(stage), std::move(outputs)};
}  // generateClusteredCollScan

/**
 * Generates a generic collection scan sub-tree.
 *  - If a resume token has been provided, the scan will start from a RecordId contained within this
 *    token.
 *  - Else if 'isTailableResumeBranch' is true, the scan will start from a RecordId contained in
 *    slot "resumeRecordId".
 *  - Otherwise the scan will start from the beginning of the collection.
 */
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> generateGenericCollScan(
    StageBuilderState& state,
    const CollectionPtr& collection,
    const CollectionScanNode* csn,
    std::vector<std::string> fields,
    PlanYieldPolicy* yieldPolicy,
    bool isTailableResumeBranch) {
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

    auto fieldSlots = state.slotIdGenerator->generateMultiple(fields.size());

    auto resultSlot = state.slotId();
    auto recordIdSlot = state.slotId();
    auto [seekRecordIdSlot, seekRecordIdExpression] =
        [&]() -> std::pair<boost::optional<sbe::value::SlotId>, std::unique_ptr<sbe::EExpression>> {
        if (csn->resumeAfterRecordId) {
            auto [tag, val] = sbe::value::makeCopyRecordId(*csn->resumeAfterRecordId);
            return {state.slotId(), makeConstant(tag, val)};
        } else if (isTailableResumeBranch) {
            auto resumeRecordIdSlot = state.data->env->getSlot("resumeRecordId"_sd);
            return {resumeRecordIdSlot, makeVariable(resumeRecordIdSlot)};
        }
        return {};
    }();

    // See if we need to project out an oplog latest timestamp.
    auto&& [scanFields, scanFieldSlots, oplogTsSlot] = makeOplogTimestampSlotIfNeeded(
        state.data->env, state.slotIdGenerator, csn->shouldTrackLatestOplogTimestamp);

    scanFields.insert(scanFields.end(), fields.begin(), fields.end());
    scanFieldSlots.insert(scanFieldSlots.end(), fieldSlots.begin(), fieldSlots.end());

    sbe::ScanCallbacks callbacks({}, {}, makeOpenCallbackIfNeeded(collection, csn));
    auto stage = sbe::makeS<sbe::ScanStage>(collection->uuid(),
                                            resultSlot,
                                            recordIdSlot,
                                            boost::none /* snapshotIdSlot */,
                                            boost::none /* indexIdentSlot */,
                                            boost::none /* indexKeySlot */,
                                            boost::none /* keyPatternSlot */,
                                            oplogTsSlot,
                                            std::move(scanFields),
                                            std::move(scanFieldSlots),
                                            seekRecordIdSlot,
                                            boost::none /* minRecordIdSlot */,
                                            boost::none /* maxRecordIdSlot */,
                                            forward,
                                            yieldPolicy,
                                            csn->nodeId(),
                                            std::move(callbacks),
                                            csn->lowPriority);

    if (seekRecordIdSlot) {
        stage = buildResumeFromRecordIdSubtree(state,
                                               collection,
                                               csn,
                                               std::move(stage),
                                               *seekRecordIdSlot,
                                               std::move(seekRecordIdExpression),
                                               yieldPolicy,
                                               isTailableResumeBranch,
                                               true /* resumeAfterRecordId  */);
    }

    PlanStageSlots outputs;
    outputs.set(PlanStageSlots::kResult, resultSlot);
    outputs.set(PlanStageSlots::kRecordId, recordIdSlot);
    for (size_t i = 0; i < fields.size(); ++i) {
        outputs.set(std::make_pair(PlanStageSlots::kField, fields[i]), fieldSlots[i]);
    }

    if (csn->filter) {
        // 'stopApplyingFilterAfterFirstMatch' is only for oplog scans; this method doesn't do them.
        invariant(!csn->stopApplyingFilterAfterFirstMatch);

        auto filterExpr = generateFilter(state, csn->filter.get(), resultSlot, &outputs);
        if (!filterExpr.isNull()) {
            stage = sbe::makeS<sbe::FilterStage<false>>(
                std::move(stage), filterExpr.extractExpr(state), csn->nodeId());
        }
    }

    return {std::move(stage), std::move(outputs)};
}  // generateGenericCollScan

}  // namespace

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> generateCollScan(
    StageBuilderState& state,
    const CollectionPtr& collection,
    const CollectionScanNode* csn,
    std::vector<std::string> fields,
    PlanYieldPolicy* yieldPolicy,
    bool isTailableResumeBranch) {

    if (csn->doSbeClusteredCollectionScan()) {
        return generateClusteredCollScan(
            state, collection, csn, std::move(fields), yieldPolicy, isTailableResumeBranch);
    } else {
        return generateGenericCollScan(
            state, collection, csn, std::move(fields), yieldPolicy, isTailableResumeBranch);
    }
}
}  // namespace mongo::stage_builder
