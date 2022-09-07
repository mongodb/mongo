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
    auto slotId = env->getSlotIfExists("oplogTs"_sd);
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
makeOplogTimestampSlotsIfNeeded(sbe::RuntimeEnvironment* env,
                                sbe::value::SlotIdGenerator* slotIdGenerator,
                                bool shouldTrackLatestOplogTimestamp) {
    if (shouldTrackLatestOplogTimestamp) {
        auto slotId = registerOplogTs(env, slotIdGenerator);
        return {{repl::OpTime::kTimestampFieldName.toString()}, sbe::makeSV(*slotId), slotId};
    }
    return {};
}

/**
 * Checks whether a callback function should be created for a ScanStage and returns it, if so. The
 * logic in the provided callback will be executed when the ScanStage is opened or reopened.
 */
sbe::ScanOpenCallback makeOpenCallbackIfNeeded(const CollectionPtr& collection,
                                               const CollectionScanNode* csn) {
    if (csn->direction == CollectionScanParams::FORWARD && csn->shouldWaitForOplogVisibility) {
        invariant(!csn->tailable);
        invariant(collection->ns().isOplog());

        return [](OperationContext* opCtx, const CollectionPtr& collection, bool reOpen) {
            if (!reOpen) {
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
            }
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
    // the branch will output  the 'seekSlot' to start the real scan from, otherwise it will
    // produce EOF.
    auto seekBranch =
        sbe::makeS<sbe::LoopJoinStage>(std::move(projStage),
                                       sbe::makeS<sbe::ScanStage>(collection->uuid(),
                                                                  boost::none /* recordSlot */,
                                                                  boost::none /* recordIdSlot*/,
                                                                  boost::none /* snapshotIdSlot */,
                                                                  boost::none /* indexIdSlot */,
                                                                  boost::none /* indexKeySlot */,
                                                                  boost::none /* keyPatternSlot */,
                                                                  boost::none /* oplogTsSlot */,
                                                                  std::vector<std::string>{},
                                                                  sbe::makeSV(),
                                                                  seekSlot,
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
    // use the 'seek' stage as an inner branch, since we need to produce all records starting  from
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
 * Creates a collection scan sub-tree optimized for oplog scans. We can built an optimized scan
 * when any of the following scenarios apply:
 *
 * 1. There is a predicted on the 'ts' field of the oplog collection.
 *    1.1 If a lower bound on 'ts' is present, the collection scan will seek directly to the
 *        RecordId of an oplog entry as close to this lower bound as possible without going higher.
 *    1.2 If the query is *only* a lower bound on 'ts' on a forward scan, every document in the
 *        collection after the first matching one must also match. To avoid wasting time running the
 *        filter on every document to be returned, we will stop applying the filter once it finds
 *        the first match.
 *    1.3 If an upper bound on 'ts' is present, the collection scan will stop and return EOF the
 *        first time it fetches a document that does not pass the filter and has 'ts' greater than
 *        the upper bound.
 * 2. The user request specified a $_resumeAfter recordId from which to begin the scan.
 */
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> generateOptimizedOplogScan(
    StageBuilderState& state,
    const CollectionPtr& collection,
    const CollectionScanNode* csn,
    PlanYieldPolicy* yieldPolicy,
    bool isTailableResumeBranch) {
    invariant(collection->ns().isOplog());
    // We can apply oplog scan optimizations only when at least one of the following was specified.
    invariant(csn->resumeAfterRecordId || csn->minRecord || csn->maxRecord);
    // The minRecord and maxRecord optimizations are not compatible with resumeAfterRecordId.
    invariant(!(csn->resumeAfterRecordId && (csn->minRecord || csn->maxRecord)));
    // Oplog scan optimizations can only be done for a forward scan.
    invariant(csn->direction == CollectionScanParams::FORWARD);

    auto resultSlot = state.slotId();
    auto recordIdSlot = state.slotId();

    // Start the scan from the RecordId stored in seekRecordId.
    // Otherwise, if we're building a collection scan for a resume branch of a special union
    // sub-tree implementing a tailable cursor scan, we can use the seekRecordIdSlot directly
    // to access the recordId to resume the scan from.
    auto [seekRecordIdSlot, seekRecordIdExpression] =
        [&]() -> std::pair<boost::optional<sbe::value::SlotId>, std::unique_ptr<sbe::EExpression>> {
        if (isTailableResumeBranch) {
            auto resumeRecordIdSlot = state.data->env->getSlot("resumeRecordId"_sd);
            return {resumeRecordIdSlot, makeVariable(resumeRecordIdSlot)};
        } else if (csn->resumeAfterRecordId) {
            auto [tag, val] = sbe::value::makeCopyRecordId(*csn->resumeAfterRecordId);
            return {state.slotId(), makeConstant(tag, val)};
        } else if (csn->minRecord) {
            auto cursor = collection->getRecordStore()->getCursor(state.opCtx);
            auto startRec = cursor->seekNear(csn->minRecord->recordId());
            if (startRec) {
                LOGV2_DEBUG(205841, 3, "Using direct oplog seek");
                auto [tag, val] = sbe::value::makeCopyRecordId(startRec->id);
                return {state.slotId(), makeConstant(tag, val)};
            }
        }
        return {};
    }();

    // Check if we need to project out an oplog 'ts' field as part of the collection scan. We will
    // need it either when 'maxRecord' bound has been provided, so that we can apply an EOF filter,
    // of if we need to track the latest oplog timestamp.
    const auto shouldTrackLatestOplogTimestamp =
        (csn->maxRecord || csn->shouldTrackLatestOplogTimestamp);
    auto&& [fields, slots, tsSlot] = makeOplogTimestampSlotsIfNeeded(
        state.data->env, state.slotIdGenerator, shouldTrackLatestOplogTimestamp);

    sbe::ScanCallbacks callbacks({}, {}, makeOpenCallbackIfNeeded(collection, csn));
    auto stage = sbe::makeS<sbe::ScanStage>(collection->uuid(),
                                            resultSlot,
                                            recordIdSlot,
                                            boost::none /* snapshotIdSlot */,
                                            boost::none /* indexIdSlot */,
                                            boost::none /* indexKeySlot */,
                                            boost::none /* keyPatternSlot */,
                                            tsSlot,
                                            std::move(fields),
                                            std::move(slots),
                                            seekRecordIdSlot,
                                            true /* forward */,
                                            yieldPolicy,
                                            csn->nodeId(),
                                            std::move(callbacks));

    // Start the scan from the seekRecordId.
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

    // Create a filter which checks the first document to ensure either that its 'ts' is less than
    // or equal the minimum timestamp that should not have rolled off the oplog, or that it is a
    // replica set initialization message. If this fails, then we throw
    // ErrorCodes::OplogQueryMinTsMissing. We avoid doing this check on the resumable branch of a
    // tailable scan; it only needs to be done once, when the initial branch is run.
    if (csn->assertTsHasNotFallenOff && !isTailableResumeBranch) {
        invariant(csn->shouldTrackLatestOplogTimestamp);

        // There should always be a 'tsSlot' already allocated on the RuntimeEnvironment for the
        // existing scan that we created previously.
        invariant(tsSlot);

        // We will be constructing a filter that needs to see the 'ts' field. We name it 'minTsSlot'
        // here so that it does not shadow the 'tsSlot' which we allocated earlier. Our filter will
        // also need to see the 'op' and 'o.msg' fields.
        auto opTypeSlot = state.slotId();
        auto oObjSlot = state.slotId();
        auto minTsSlot = state.slotId();
        sbe::value::SlotVector minTsSlots = {minTsSlot, opTypeSlot, oObjSlot};
        std::vector<std::string> fields = {repl::OpTime::kTimestampFieldName.toString(), "op", "o"};

        // If the first entry we see in the oplog is the replset initialization, then it doesn't
        // matter if its timestamp is later than the specified minTs; no events earlier than the
        // minTs can have fallen off this oplog. Otherwise, we must verify that the timestamp of the
        // first observed oplog entry is earlier than or equal to the minTs time.
        //
        // To achieve this, we build a two-branch union subtree. The left branch is a scan with a
        // filter that checks the first entry in the oplog for the above criteria, throws via EFail
        // if they are not met, and EOFs otherwise. The right branch of the union plan is the tree
        // that we originally built above.
        //
        // union [s9, s10, s11] [
        //     [s6, s7, s8] efilter {if (ts <= minTs || op == "n" && isObject (o) &&
        //                      getField (o, "msg") == "initiating set", false, fail ( 326 ))}
        //     scan [s6 = ts, s7 = op, s8 = o] @oplog,
        //     <stage>

        // Set up the filter stage to be used in the left branch of the union. If the main body of
        // the expression does not match the input document, it throws OplogQueryMinTsMissing. If
        // the expression does match, then it returns 'false', which causes the filter (and as a
        // result, the branch) to EOF immediately. Note that the resultSlot and recordIdSlot
        // arguments to the ScanStage are boost::none, as we do not need them.
        sbe::ScanCallbacks branchCallbacks{};
        auto minTsBranch = sbe::makeS<sbe::FilterStage<false, true>>(
            sbe::makeS<sbe::ScanStage>(collection->uuid(),
                                       boost::none /* resultSlot */,
                                       boost::none /* recordIdSlot */,
                                       boost::none /* snapshotIdSlot */,
                                       boost::none /* indexIdSlot */,
                                       boost::none /* indexKeySlot */,
                                       boost::none /* keyPatternSlot */,
                                       boost::none /* oplogTsSlot*/,
                                       std::move(fields),
                                       minTsSlots, /* don't move this */
                                       boost::none,
                                       true /* forward */,
                                       yieldPolicy,
                                       csn->nodeId(),
                                       branchCallbacks),
            sbe::makeE<sbe::EIf>(
                makeBinaryOp(
                    sbe::EPrimBinary::logicOr,
                    makeBinaryOp(sbe::EPrimBinary::lessEq,
                                 makeVariable(minTsSlot),
                                 makeConstant(sbe::value::TypeTags::Timestamp,
                                              csn->assertTsHasNotFallenOff->asULL())),
                    makeBinaryOp(
                        sbe::EPrimBinary::logicAnd,
                        makeBinaryOp(sbe::EPrimBinary::eq,
                                     makeVariable(opTypeSlot),
                                     makeConstant("n")),
                        makeBinaryOp(sbe::EPrimBinary::logicAnd,
                                     makeFunction("isObject", makeVariable(oObjSlot)),
                                     makeBinaryOp(sbe::EPrimBinary::eq,
                                                  makeFunction("getField",
                                                               makeVariable(oObjSlot),
                                                               makeConstant("msg")),
                                                  makeConstant(repl::kInitiatingSetMsg))))),
                makeConstant(sbe::value::TypeTags::Boolean, false),
                sbe::makeE<sbe::EFail>(ErrorCodes::OplogQueryMinTsMissing,
                                       "Specified minTs has already fallen off the oplog")),
            csn->nodeId());

        // All branches of the UnionStage must have the same number of input and output slots, and
        // we want to remap all slots from the basic scan we constructed earlier through the union
        // stage to the output. We're lucky that the real scan happens to have the same number of
        // slots (resultSlot, recordSlot, tsSlot) as the minTs check branch (minTsSlot, opTypeSlot,
        // oObjSlot), so we don't have to compensate with any unused slots. Note that the minTsSlots
        // will never be mapped to output in practice, since the minTs branch either throws or EOFs.
        //
        // We also need to update the local variables for each slot to their remapped values, so
        // subsequent subtrees constructed by this function refer to the correct post-union slots.
        auto realSlots = sbe::makeSV(resultSlot, recordIdSlot, *tsSlot);
        resultSlot = state.slotId();
        recordIdSlot = state.slotId();
        tsSlot = state.slotId();
        auto outputSlots = sbe::makeSV(resultSlot, recordIdSlot, *tsSlot);

        // Create the union stage. The left branch, which runs first, is our resumability check.
        stage = sbe::makeS<sbe::UnionStage>(
            sbe::makeSs(std::move(minTsBranch), std::move(stage)),
            makeVector<sbe::value::SlotVector>(std::move(minTsSlots), std::move(realSlots)),
            std::move(outputSlots),
            csn->nodeId());
    }

    // Add an EOF filter to stop the scan after we fetch the first document that has 'ts' greater
    // than the upper bound.
    if (csn->maxRecord) {
        // The 'maxRecord' optimization is not compatible with 'stopApplyingFilterAfterFirstMatch'.
        invariant(!csn->stopApplyingFilterAfterFirstMatch);
        invariant(tsSlot);

        stage = sbe::makeS<sbe::FilterStage<false, true>>(
            std::move(stage),
            makeBinaryOp(sbe::EPrimBinary::lessEq,
                         makeVariable(*tsSlot),
                         makeConstant(sbe::value::TypeTags::Timestamp,
                                      csn->maxRecord->recordId().getLong())),
            csn->nodeId());
    }

    // If csn->stopApplyingFilterAfterFirstMatch is true, assert that csn has a filter.
    invariant(!csn->stopApplyingFilterAfterFirstMatch || csn->filter);

    if (csn->filter) {
        auto relevantSlots = sbe::makeSV(resultSlot, recordIdSlot);
        if (tsSlot) {
            relevantSlots.push_back(*tsSlot);
        }

        auto [_, outputStage] = generateFilter(state,
                                               csn->filter.get(),
                                               {std::move(stage), std::move(relevantSlots)},
                                               resultSlot,
                                               csn->nodeId());
        stage = outputStage.extractStage(csn->nodeId());

        // We may be requested to stop applying the filter after the first match. This can happen
        // if the query is just a lower bound on 'ts' on a forward scan. In this case every document
        // in the collection after the first matching one must also match, so there is no need to
        // run the filter on such elements.
        //
        // To apply this optimization we will construct the following sub-tree:
        //
        //       nlj [] [seekRecordIdSlot]
        //           left
        //              limit 1
        //              filter <predicate>
        //              <stage>
        //           right
        //              seek seekRecordIdSlot resultSlot recordIdSlot @coll
        //
        // Here, the nested loop join outer branch is the collection scan we constructed above, with
        // a csn->filter predicate sitting on top. The 'limit 1' stage is to ensure this branch
        // returns a single row. Once executed, this branch will filter out documents which doesn't
        // satisfy the predicate, and will return the first document, along with a RecordId, that
        // matches. This RecordId is then used as a starting point of the collection scan in the
        // inner branch, and the execution will continue from this point further on, without
        // applying the filter.
        if (csn->stopApplyingFilterAfterFirstMatch) {
            invariant(!csn->maxRecord);
            invariant(csn->direction == CollectionScanParams::FORWARD);

            seekRecordIdSlot = recordIdSlot;
            resultSlot = state.slotId();
            recordIdSlot = state.slotId();

            std::tie(fields, slots, tsSlot) = makeOplogTimestampSlotsIfNeeded(
                state.data->env, state.slotIdGenerator, shouldTrackLatestOplogTimestamp);

            stage = sbe::makeS<sbe::LoopJoinStage>(
                sbe::makeS<sbe::LimitSkipStage>(std::move(stage), 1, boost::none, csn->nodeId()),
                sbe::makeS<sbe::ScanStage>(collection->uuid(),
                                           resultSlot,
                                           recordIdSlot,
                                           boost::none /* snapshotIdSlot */,
                                           boost::none /* indexIdSlot */,
                                           boost::none /* indexKeySlot */,
                                           boost::none /* keyPatternSlot */,
                                           tsSlot,
                                           std::move(fields),
                                           std::move(slots),
                                           seekRecordIdSlot,
                                           true /* forward */,
                                           yieldPolicy,
                                           csn->nodeId(),
                                           sbe::ScanCallbacks{}),
                sbe::makeSV(),
                sbe::makeSV(*seekRecordIdSlot),
                nullptr,
                csn->nodeId());
        }
    }

    // If csn->shouldTrackLatestOplogTimestamp is true, assert that we generated tsSlot.
    invariant(!csn->shouldTrackLatestOplogTimestamp || tsSlot);

    PlanStageSlots outputs;
    outputs.set(PlanStageSlots::kResult, resultSlot);
    outputs.set(PlanStageSlots::kRecordId, recordIdSlot);

    return {std::move(stage), std::move(outputs)};
}

/**
 * Generates a generic collection scan sub-tree.
 *  - If a resume token has been provided, the scan will start from a RecordId contained within this
 * token.
 *  - Else if 'isTailableResumeBranch' is true, the scan will start from a RecordId contained in
 * slot "resumeRecordId".
 *  - Otherwise the scan will start from the beginning of the collection.
 */
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> generateGenericCollScan(
    StageBuilderState& state,
    const CollectionPtr& collection,
    const CollectionScanNode* csn,
    PlanYieldPolicy* yieldPolicy,
    bool isTailableResumeBranch) {
    const auto forward = csn->direction == CollectionScanParams::FORWARD;

    invariant(!csn->shouldTrackLatestOplogTimestamp || collection->ns().isOplog());
    invariant(!csn->resumeAfterRecordId || forward);
    invariant(!csn->resumeAfterRecordId || !csn->tailable);

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
    auto&& [fields, slots, tsSlot] = makeOplogTimestampSlotsIfNeeded(
        state.data->env, state.slotIdGenerator, csn->shouldTrackLatestOplogTimestamp);

    sbe::ScanCallbacks callbacks({}, {}, makeOpenCallbackIfNeeded(collection, csn));
    auto stage = sbe::makeS<sbe::ScanStage>(collection->uuid(),
                                            resultSlot,
                                            recordIdSlot,
                                            boost::none /* snapshotIdSlot */,
                                            boost::none /* indexIdSlot */,
                                            boost::none /* indexKeySlot */,
                                            boost::none /* keyPatternSlot */,
                                            tsSlot,
                                            std::move(fields),
                                            std::move(slots),
                                            seekRecordIdSlot,
                                            forward,
                                            yieldPolicy,
                                            csn->nodeId(),
                                            std::move(callbacks));

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

    if (csn->filter) {
        // The 'stopApplyingFilterAfterFirstMatch' optimization is only applicable when the 'ts'
        // lower bound is also provided for an oplog scan, and is handled in
        // 'generateOptimizedOplogScan()'.
        invariant(!csn->stopApplyingFilterAfterFirstMatch);

        auto relevantSlots = sbe::makeSV(resultSlot, recordIdSlot);

        auto [_, outputStage] = generateFilter(state,
                                               csn->filter.get(),
                                               {std::move(stage), std::move(relevantSlots)},
                                               resultSlot,
                                               csn->nodeId());
        stage = outputStage.extractStage(csn->nodeId());
    }

    PlanStageSlots outputs;
    outputs.set(PlanStageSlots::kResult, resultSlot);
    outputs.set(PlanStageSlots::kRecordId, recordIdSlot);

    return {std::move(stage), std::move(outputs)};
}
}  // namespace

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> generateCollScan(
    StageBuilderState& state,
    const CollectionPtr& collection,
    const CollectionScanNode* csn,
    PlanYieldPolicy* yieldPolicy,
    bool isTailableResumeBranch) {
    if (csn->minRecord || csn->maxRecord || csn->stopApplyingFilterAfterFirstMatch) {
        return generateOptimizedOplogScan(
            state, collection, csn, yieldPolicy, isTailableResumeBranch);
    } else {
        return generateGenericCollScan(state, collection, csn, yieldPolicy, isTailableResumeBranch);
    }
}
}  // namespace mongo::stage_builder
