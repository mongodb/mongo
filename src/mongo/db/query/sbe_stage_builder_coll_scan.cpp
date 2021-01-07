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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

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
#include "mongo/db/storage/oplog_hack.h"
#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

namespace mongo::stage_builder {
namespace {
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

/**
 * If 'shouldTrackLatestOplogTimestamp' returns a vector holding the name of the oplog 'ts' field
 * along with another vector holding a SlotId to map this field to, as well as the standalone value
 * of the same SlotId (the latter is returned purely for convenience purposes).
 */
std::tuple<std::vector<std::string>, sbe::value::SlotVector, boost::optional<sbe::value::SlotId>>
makeOplogTimestampSlotsIfNeeded(const CollectionPtr& collection,
                                sbe::value::SlotIdGenerator* slotIdGenerator,
                                bool shouldTrackLatestOplogTimestamp) {
    if (shouldTrackLatestOplogTimestamp) {
        invariant(collection->ns().isOplog());

        auto tsSlot = slotIdGenerator->generate();
        return {{repl::OpTime::kTimestampFieldName}, sbe::makeSV(tsSlot), tsSlot};
    }
    return {};
};

/**
 * Creates a collection scan sub-tree optimized for oplog scans. We can built an optimized scan
 * when there is a predicted on the 'ts' field of the oplog collection.
 *
 *   1. If a lower bound on 'ts' is present, the collection scan will seek directly to the RecordId
 *      of an oplog entry as close to this lower bound as possible without going higher.
 *         1.1 If the query is just a lower bound on 'ts' on a forward scan, every document in the
 *             collection after the first matching one must also match. To avoid wasting time
 *             running the filter on every document to be returned, we will stop applying the filter
 *             once it finds the first match.
 *   2. If an upper bound on 'ts' is present, the collection scan will stop and return EOF the first
 *      time it fetches a document that does not pass the filter and has 'ts' greater than the upper
 *      bound.
 */
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> generateOptimizedOplogScan(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const CollectionScanNode* csn,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    sbe::value::FrameIdGenerator* frameIdGenerator,
    PlanYieldPolicy* yieldPolicy,
    sbe::RuntimeEnvironment* env,
    bool isTailableResumeBranch,
    sbe::LockAcquisitionCallback lockAcquisitionCallback) {
    invariant(collection->ns().isOplog());
    // The minTs and maxTs optimizations are not compatible with resumeAfterRecordId and can only
    // be done for a forward scan.
    invariant(!csn->resumeAfterRecordId);
    invariant(csn->direction == CollectionScanParams::FORWARD);

    auto resultSlot = slotIdGenerator->generate();
    auto recordIdSlot = slotIdGenerator->generate();

    // See if the RecordStore supports the oplogStartHack. If so, the scan will start from the
    // RecordId stored in seekRecordId.
    // Otherwise, if we're building a collection scan for a resume branch of a special union
    // sub-tree implementing a tailable cursor scan, we can use the seekRecordIdSlot directly
    // to access the recordId to resume the scan from.
    auto [seekRecordId, seekRecordIdSlot] =
        [&]() -> std::pair<boost::optional<RecordId>, boost::optional<sbe::value::SlotId>> {
        if (isTailableResumeBranch) {
            auto resumeRecordIdSlot = env->getSlot("resumeRecordId"_sd);
            return {{}, resumeRecordIdSlot};
        } else if (csn->minTs) {
            auto goal = oploghack::keyForOptime(*csn->minTs);
            if (goal.isOK()) {
                auto startLoc =
                    collection->getRecordStore()->oplogStartHack(opCtx, goal.getValue());
                if (startLoc && !startLoc->isNull()) {
                    LOGV2_DEBUG(205841, 3, "Using direct oplog seek");
                    return {startLoc, slotIdGenerator->generate()};
                }
            }
        }
        return {};
    }();

    // Check if we need to project out an oplog 'ts' field as part of the collection scan. We will
    // need it either when 'maxTs' bound has been provided, so that we can apply an EOF filter, of
    // if we need to track the latest oplog timestamp.
    const auto shouldTrackLatestOplogTimestamp = !csn->stopApplyingFilterAfterFirstMatch &&
        (csn->maxTs || csn->shouldTrackLatestOplogTimestamp);
    auto&& [fields, slots, tsSlot] = makeOplogTimestampSlotsIfNeeded(
        collection, slotIdGenerator, shouldTrackLatestOplogTimestamp);

    NamespaceStringOrUUID nss{collection->ns().db().toString(), collection->uuid()};
    auto stage = sbe::makeS<sbe::ScanStage>(nss,
                                            resultSlot,
                                            recordIdSlot,
                                            std::move(fields),
                                            std::move(slots),
                                            seekRecordIdSlot,
                                            true /* forward */,
                                            yieldPolicy,
                                            csn->nodeId(),
                                            std::move(lockAcquisitionCallback),
                                            makeOpenCallbackIfNeeded(collection, csn));

    // Start the scan from the seekRecordId if we can use the oplogStartHack.
    if (seekRecordId) {
        invariant(seekRecordIdSlot);

        // Project the start RecordId as a seekRecordIdSlot and feed it to the inner side (scan).
        stage = sbe::makeS<sbe::LoopJoinStage>(
            sbe::makeProjectStage(
                sbe::makeS<sbe::LimitSkipStage>(
                    sbe::makeS<sbe::CoScanStage>(csn->nodeId()), 1, boost::none, csn->nodeId()),
                csn->nodeId(),
                *seekRecordIdSlot,
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::RecordId,
                                           sbe::value::bitcastFrom<int64_t>(seekRecordId->repr()))),
            std::move(stage),
            sbe::makeSV(),
            sbe::makeSV(*seekRecordIdSlot),
            nullptr,
            csn->nodeId());
    }

    // Add an EOF filter to stop the scan after we fetch the first document that has 'ts' greater
    // than the upper bound.
    if (csn->maxTs) {
        // The 'maxTs' optimization is not compatible with 'stopApplyingFilterAfterFirstMatch'.
        invariant(!csn->stopApplyingFilterAfterFirstMatch);
        invariant(tsSlot);

        stage = sbe::makeS<sbe::FilterStage<false, true>>(
            std::move(stage),
            sbe::makeE<sbe::EPrimBinary>(
                sbe::EPrimBinary::lessEq,
                sbe::makeE<sbe::EVariable>(*tsSlot),
                sbe::makeE<sbe::EConstant>(
                    sbe::value::TypeTags::Timestamp,
                    sbe::value::bitcastFrom<uint64_t>((*csn->maxTs).asULL()))),
            csn->nodeId());
    }

    // If csn->stopApplyingFilterAfterFirstMatch is true, assert that csn has a filter.
    invariant(!csn->stopApplyingFilterAfterFirstMatch || csn->filter);

    if (csn->filter) {
        auto relevantSlots = sbe::makeSV(resultSlot, recordIdSlot);
        if (tsSlot) {
            relevantSlots.push_back(*tsSlot);
        }

        stage = generateFilter(opCtx,
                               csn->filter.get(),
                               std::move(stage),
                               slotIdGenerator,
                               frameIdGenerator,
                               resultSlot,
                               env,
                               std::move(relevantSlots),
                               csn->nodeId());

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
            invariant(csn->minTs);
            invariant(csn->direction == CollectionScanParams::FORWARD);

            std::tie(fields, slots, tsSlot) = makeOplogTimestampSlotsIfNeeded(
                collection, slotIdGenerator, csn->shouldTrackLatestOplogTimestamp);

            seekRecordIdSlot = recordIdSlot;
            resultSlot = slotIdGenerator->generate();
            recordIdSlot = slotIdGenerator->generate();

            stage = sbe::makeS<sbe::LoopJoinStage>(
                sbe::makeS<sbe::LimitSkipStage>(std::move(stage), 1, boost::none, csn->nodeId()),
                sbe::makeS<sbe::ScanStage>(nss,
                                           resultSlot,
                                           recordIdSlot,
                                           std::move(fields),
                                           std::move(slots),
                                           seekRecordIdSlot,
                                           true /* forward */,
                                           yieldPolicy,
                                           csn->nodeId(),
                                           std::move(lockAcquisitionCallback)),
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

    if (csn->shouldTrackLatestOplogTimestamp) {
        outputs.set(PlanStageSlots::kOplogTs, *tsSlot);
    }

    return {std::move(stage), std::move(outputs)};
}

/**
 * Generates a generic collecion scan sub-tree. If a resume token has been provided, the scan will
 * start from a RecordId contained within this token, otherwise from the beginning of the
 * collection.
 */
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> generateGenericCollScan(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const CollectionScanNode* csn,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    sbe::value::FrameIdGenerator* frameIdGenerator,
    PlanYieldPolicy* yieldPolicy,
    sbe::RuntimeEnvironment* env,
    bool isTailableResumeBranch,
    sbe::LockAcquisitionCallback lockAcquisitionCallback) {
    const auto forward = csn->direction == CollectionScanParams::FORWARD;

    invariant(!csn->shouldTrackLatestOplogTimestamp || collection->ns().isOplog());
    invariant(!csn->resumeAfterRecordId || forward);
    invariant(!csn->resumeAfterRecordId || !csn->tailable);

    auto resultSlot = slotIdGenerator->generate();
    auto recordIdSlot = slotIdGenerator->generate();
    auto seekRecordIdSlot = [&]() -> boost::optional<sbe::value::SlotId> {
        if (csn->resumeAfterRecordId) {
            return slotIdGenerator->generate();
        } else if (isTailableResumeBranch) {
            auto resumeRecordIdSlot = env->getSlot("resumeRecordId"_sd);
            invariant(resumeRecordIdSlot);
            return resumeRecordIdSlot;
        }
        return {};
    }();

    // See if we need to project out an oplog latest timestamp.
    auto&& [fields, slots, tsSlot] = makeOplogTimestampSlotsIfNeeded(
        collection, slotIdGenerator, csn->shouldTrackLatestOplogTimestamp);

    NamespaceStringOrUUID nss{collection->ns().db().toString(), collection->uuid()};
    auto stage = sbe::makeS<sbe::ScanStage>(nss,
                                            resultSlot,
                                            recordIdSlot,
                                            std::move(fields),
                                            std::move(slots),
                                            seekRecordIdSlot,
                                            forward,
                                            yieldPolicy,
                                            csn->nodeId(),
                                            lockAcquisitionCallback,
                                            makeOpenCallbackIfNeeded(collection, csn));

    // Check if the scan should be started after the provided resume RecordId and construct a nested
    // loop join sub-tree to project out the resume RecordId as a seekRecordIdSlot and feed it to
    // the inner side (scan). We will also construct a union sub-tree as an outer side of the loop
    // join to implement the check that the record we're trying to reposition the scan exists.
    if (seekRecordIdSlot && !isTailableResumeBranch) {
        // Project out the RecordId we want to resume from as 'seekSlot'.
        auto seekSlot = slotIdGenerator->generate();
        auto projStage = sbe::makeProjectStage(
            sbe::makeS<sbe::LimitSkipStage>(
                sbe::makeS<sbe::CoScanStage>(csn->nodeId()), 1, boost::none, csn->nodeId()),
            csn->nodeId(),
            seekSlot,
            sbe::makeE<sbe::EConstant>(
                sbe::value::TypeTags::RecordId,
                sbe::value::bitcastFrom<int64_t>(csn->resumeAfterRecordId->repr())));

        // Construct a 'seek' branch of the 'union'. If we're succeeded to reposition the cursor,
        // the branch will output  the 'seekSlot' to start the real scan from, otherwise it will
        // produce EOF.
        auto seekBranch =
            sbe::makeS<sbe::LoopJoinStage>(std::move(projStage),
                                           sbe::makeS<sbe::ScanStage>(nss,
                                                                      boost::none,
                                                                      boost::none,
                                                                      std::vector<std::string>{},
                                                                      sbe::makeSV(),
                                                                      seekSlot,
                                                                      forward,
                                                                      yieldPolicy,
                                                                      csn->nodeId(),
                                                                      lockAcquisitionCallback),
                                           sbe::makeSV(seekSlot),
                                           sbe::makeSV(seekSlot),
                                           nullptr,
                                           csn->nodeId());

        // Construct a 'fail' branch of the union. The 'unusedSlot' is needed as each union branch
        // must have the same number of slots, and we use just one in the 'seek' branch above. This
        // branch will only be executed if the 'seek' branch produces EOF, which can only happen if
        // if the seek did not find the record id specified in $_resumeAfter.
        auto unusedSlot = slotIdGenerator->generate();
        auto failBranch = sbe::makeProjectStage(
            sbe::makeS<sbe::CoScanStage>(csn->nodeId()),
            csn->nodeId(),
            unusedSlot,
            sbe::makeE<sbe::EFail>(
                ErrorCodes::KeyNotFound,
                str::stream() << "Failed to resume collection scan: the recordId from which we are "
                              << "attempting to resume no longer exists in the collection: "
                              << csn->resumeAfterRecordId));

        // Construct a union stage from the 'seek' and 'fail' branches. Note that this stage will
        // ever produce a single call to getNext() due to a 'limit 1' sitting on top of it.
        auto unionStage = sbe::makeS<sbe::UnionStage>(
            makeVector<std::unique_ptr<sbe::PlanStage>>(std::move(seekBranch),
                                                        std::move(failBranch)),
            std::vector<sbe::value::SlotVector>{sbe::makeSV(seekSlot), sbe::makeSV(unusedSlot)},
            sbe::makeSV(*seekRecordIdSlot),
            csn->nodeId());

        // Construct the final loop join. Note that we also inject a 'skip 1' stage on top of the
        // inner branch, as we need to start _after_ the resume RecordId, and a 'limit 1' stage on
        // top of the outer branch, as it should produce just a single seek recordId.
        stage = sbe::makeS<sbe::LoopJoinStage>(
            sbe::makeS<sbe::LimitSkipStage>(std::move(unionStage), 1, boost::none, csn->nodeId()),
            sbe::makeS<sbe::LimitSkipStage>(std::move(stage), boost::none, 1, csn->nodeId()),
            sbe::makeSV(),
            sbe::makeSV(*seekRecordIdSlot),
            nullptr,
            csn->nodeId());
    }

    if (csn->filter) {
        // The 'stopApplyingFilterAfterFirstMatch' optimization is only applicable when the 'ts'
        // lower bound is also provided for an oplog scan, and is handled in
        // 'generateOptimizedOplogScan()'.
        invariant(!csn->stopApplyingFilterAfterFirstMatch);

        auto relevantSlots = sbe::makeSV(resultSlot, recordIdSlot);
        if (tsSlot) {
            relevantSlots.push_back(*tsSlot);
        }

        stage = generateFilter(opCtx,
                               csn->filter.get(),
                               std::move(stage),
                               slotIdGenerator,
                               frameIdGenerator,
                               resultSlot,
                               env,
                               std::move(relevantSlots),
                               csn->nodeId());
    }

    PlanStageSlots outputs;
    outputs.set(PlanStageSlots::kResult, resultSlot);
    outputs.set(PlanStageSlots::kRecordId, recordIdSlot);

    if (tsSlot) {
        outputs.set(PlanStageSlots::kOplogTs, *tsSlot);
    }

    return {std::move(stage), std::move(outputs)};
}
}  // namespace

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> generateCollScan(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const CollectionScanNode* csn,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    sbe::value::FrameIdGenerator* frameIdGenerator,
    PlanYieldPolicy* yieldPolicy,
    sbe::RuntimeEnvironment* env,
    bool isTailableResumeBranch,
    sbe::LockAcquisitionCallback lockAcquisitionCallback) {
    if (csn->minTs || csn->maxTs) {
        return generateOptimizedOplogScan(opCtx,
                                          collection,
                                          csn,
                                          slotIdGenerator,
                                          frameIdGenerator,
                                          yieldPolicy,
                                          env,
                                          isTailableResumeBranch,
                                          std::move(lockAcquisitionCallback));
    } else {
        return generateGenericCollScan(opCtx,
                                       collection,
                                       csn,
                                       slotIdGenerator,
                                       frameIdGenerator,
                                       yieldPolicy,
                                       env,
                                       isTailableResumeBranch,
                                       std::move(lockAcquisitionCallback));
    }
}
}  // namespace mongo::stage_builder
