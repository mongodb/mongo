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
sbe::ScanOpenCallback makeOpenCallbackIfNeeded(const Collection* collection,
                                               const CollectionScanNode* csn) {
    if (csn->direction == CollectionScanParams::FORWARD && csn->shouldWaitForOplogVisibility) {
        invariant(!csn->tailable);
        invariant(collection->ns().isOplog());

        return [](OperationContext* opCtx, const Collection* collection, bool reOpen) {
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
std::tuple<sbe::value::SlotId,
           sbe::value::SlotId,
           boost::optional<sbe::value::SlotId>,
           std::unique_ptr<sbe::PlanStage>>
generateOptimizedOplogScan(OperationContext* opCtx,
                           const Collection* collection,
                           const CollectionScanNode* csn,
                           sbe::value::SlotIdGenerator* slotIdGenerator,
                           PlanYieldPolicy* yieldPolicy,
                           TrialRunProgressTracker* tracker) {
    invariant(collection->ns().isOplog());
    // The minTs and maxTs optimizations are not compatible with resumeAfterRecordId and can only
    // be done for a forward scan.
    invariant(!csn->resumeAfterRecordId);
    invariant(csn->direction == CollectionScanParams::FORWARD);

    auto resultSlot = slotIdGenerator->generate();
    auto recordIdSlot = slotIdGenerator->generate();

    // See if the RecordStore supports the oplogStartHack. If so, the scan will start from the
    // RecordId stored in seekRecordId.
    auto [seekRecordId, seekRecordIdSlot] =
        [&]() -> std::pair<boost::optional<RecordId>, boost::optional<sbe::value::SlotId>> {
        if (csn->minTs) {
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
    auto [fields, slots, tsSlot] = [&]() -> std::tuple<std::vector<std::string>,
                                                       sbe::value::SlotVector,
                                                       boost::optional<sbe::value::SlotId>> {
        // Don't project the 'ts' if stopApplyingFilterAfterFirstMatch is 'true'. We will have
        // another scan stage where it will be done.
        if (!csn->stopApplyingFilterAfterFirstMatch &&
            (csn->maxTs || csn->shouldTrackLatestOplogTimestamp)) {
            auto tsSlot = slotIdGenerator->generate();
            return {{repl::OpTime::kTimestampFieldName}, sbe::makeSV(tsSlot), tsSlot};
        }
        return {};
    }();

    NamespaceStringOrUUID nss{collection->ns().db().toString(), collection->uuid()};
    auto stage = sbe::makeS<sbe::ScanStage>(nss,
                                            resultSlot,
                                            recordIdSlot,
                                            std::move(fields),
                                            std::move(slots),
                                            seekRecordIdSlot,
                                            true /* forward */,
                                            yieldPolicy,
                                            tracker,
                                            makeOpenCallbackIfNeeded(collection, csn));

    // Start the scan from the seekRecordId if we can use the oplogStartHack.
    if (seekRecordId) {
        invariant(seekRecordIdSlot);

        // Project the start RecordId as a seekRecordIdSlot and feed it to the inner side (scan).
        stage = sbe::makeS<sbe::LoopJoinStage>(
            sbe::makeProjectStage(
                sbe::makeS<sbe::LimitSkipStage>(sbe::makeS<sbe::CoScanStage>(), 1, boost::none),
                *seekRecordIdSlot,
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                           seekRecordId->repr())),
            std::move(stage),
            sbe::makeSV(),
            sbe::makeSV(*seekRecordIdSlot),
            nullptr);
    }

    // Add an EOF filter to stop the scan after we fetch the first document that has 'ts' greater
    // than the upper bound.
    if (csn->maxTs) {
        // The 'maxTs' optimization is not compatible with 'stopApplyingFilterAfterFirstMatch'.
        invariant(!csn->stopApplyingFilterAfterFirstMatch);
        invariant(tsSlot);

        stage = sbe::makeS<sbe::FilterStage<false, true>>(
            std::move(stage),
            sbe::makeE<sbe::EPrimBinary>(sbe::EPrimBinary::lessEq,
                                         sbe::makeE<sbe::EVariable>(*tsSlot),
                                         sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Timestamp,
                                                                    (*csn->maxTs).asULL())));
    }

    if (csn->filter) {
        stage = generateFilter(csn->filter.get(), std::move(stage), slotIdGenerator, resultSlot);

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
            std::tie(fields, slots, tsSlot) =
                [&]() -> std::tuple<std::vector<std::string>,
                                    sbe::value::SlotVector,
                                    boost::optional<sbe::value::SlotId>> {
                if (csn->shouldTrackLatestOplogTimestamp) {
                    auto tsSlot = slotIdGenerator->generate();
                    return {{repl::OpTime::kTimestampFieldName}, sbe::makeSV(tsSlot), tsSlot};
                }
                return {};
            }();

            seekRecordIdSlot = recordIdSlot;
            resultSlot = slotIdGenerator->generate();
            recordIdSlot = slotIdGenerator->generate();

            stage = sbe::makeS<sbe::LoopJoinStage>(
                sbe::makeS<sbe::LimitSkipStage>(std::move(stage), 1, boost::none),
                sbe::makeS<sbe::ScanStage>(nss,
                                           resultSlot,
                                           recordIdSlot,
                                           std::move(fields),
                                           std::move(slots),
                                           seekRecordIdSlot,
                                           true /* forward */,
                                           yieldPolicy,
                                           tracker),
                sbe::makeSV(),
                sbe::makeSV(*seekRecordIdSlot),
                nullptr);
        }
    }

    return {resultSlot,
            recordIdSlot,
            csn->shouldTrackLatestOplogTimestamp ? tsSlot : boost::none,
            std::move(stage)};
}

/**
 * Generates a generic collecion scan sub-tree. If a resume token has been provided, the scan will
 * start from a RecordId contained within this token, otherwise from the beginning of the
 * collection.
 */
std::tuple<sbe::value::SlotId,
           sbe::value::SlotId,
           boost::optional<sbe::value::SlotId>,
           std::unique_ptr<sbe::PlanStage>>
generateGenericCollScan(const Collection* collection,
                        const CollectionScanNode* csn,
                        sbe::value::SlotIdGenerator* slotIdGenerator,
                        PlanYieldPolicy* yieldPolicy,
                        TrialRunProgressTracker* tracker) {
    const auto forward = csn->direction == CollectionScanParams::FORWARD;

    auto resultSlot = slotIdGenerator->generate();
    auto recordIdSlot = slotIdGenerator->generate();
    auto seekRecordIdSlot = boost::make_optional(static_cast<bool>(csn->resumeAfterRecordId),
                                                 slotIdGenerator->generate());

    // See if we need to project out an oplog latest timestamp.
    auto [fields, slots, tsSlot] = [&]() -> std::tuple<std::vector<std::string>,
                                                       sbe::value::SlotVector,
                                                       boost::optional<sbe::value::SlotId>> {
        if (csn->shouldTrackLatestOplogTimestamp) {
            invariant(collection->ns().isOplog());

            auto tsSlot = slotIdGenerator->generate();
            return {{repl::OpTime::kTimestampFieldName}, sbe::makeSV(tsSlot), tsSlot};
        }
        return {};
    }();

    NamespaceStringOrUUID nss{collection->ns().db().toString(), collection->uuid()};
    auto stage = sbe::makeS<sbe::ScanStage>(nss,
                                            resultSlot,
                                            recordIdSlot,
                                            std::move(fields),
                                            std::move(slots),
                                            seekRecordIdSlot,
                                            forward,
                                            yieldPolicy,
                                            tracker,
                                            makeOpenCallbackIfNeeded(collection, csn));

    // Check if the scan should be started after the provided resume RecordId and construct a nested
    // loop join sub-tree to project out the resume RecordId as a seekRecordIdSlot and feed it to
    // the inner side (scan).
    //
    // Note that we also inject a 'skip 1' stage on top of the inner branch, as we need to start
    // _after_ the resume RecordId.
    //
    // TODO SERVER-48472: raise KeyNotFound error if we cannot position the cursor on
    // seekRecordIdSlot.
    if (seekRecordIdSlot) {
        stage = sbe::makeS<sbe::LoopJoinStage>(
            sbe::makeProjectStage(
                sbe::makeS<sbe::LimitSkipStage>(sbe::makeS<sbe::CoScanStage>(), 1, boost::none),
                *seekRecordIdSlot,
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64,
                                           csn->resumeAfterRecordId->repr())),
            sbe::makeS<sbe::LimitSkipStage>(std::move(stage), boost::none, 1),
            sbe::makeSV(),
            sbe::makeSV(*seekRecordIdSlot),
            nullptr);
    }

    if (csn->filter) {
        // The 'stopApplyingFilterAfterFirstMatch' optimization is only applicable when the 'ts'
        // lower bound is also provided for an oplog scan, and is handled in
        // 'generateOptimizedOplogScan()'.
        invariant(!csn->stopApplyingFilterAfterFirstMatch);

        stage = generateFilter(csn->filter.get(), std::move(stage), slotIdGenerator, resultSlot);
    }

    return {resultSlot, recordIdSlot, tsSlot, std::move(stage)};
}
}  // namespace

std::tuple<sbe::value::SlotId,
           sbe::value::SlotId,
           boost::optional<sbe::value::SlotId>,
           std::unique_ptr<sbe::PlanStage>>
generateCollScan(OperationContext* opCtx,
                 const Collection* collection,
                 const CollectionScanNode* csn,
                 sbe::value::SlotIdGenerator* slotIdGenerator,
                 PlanYieldPolicy* yieldPolicy,
                 TrialRunProgressTracker* tracker) {
    uassert(4822889, "Tailable collection scans are not supported in SBE", !csn->tailable);

    auto [resultSlot, recordIdSlot, oplogTsSlot, stage] = [&]() {
        if (csn->minTs || csn->maxTs) {
            return generateOptimizedOplogScan(
                opCtx, collection, csn, slotIdGenerator, yieldPolicy, tracker);
        } else {
            return generateGenericCollScan(collection, csn, slotIdGenerator, yieldPolicy, tracker);
        }
    }();

    return {resultSlot, recordIdSlot, oplogTsSlot, std::move(stage)};
}
}  // namespace mongo::stage_builder
