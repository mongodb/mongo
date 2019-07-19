/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/exec/change_stream_proxy.h"

#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/repl/speculative_majority_read_info.h"

namespace mongo {

const char* ChangeStreamProxyStage::kStageType = "CHANGE_STREAM_PROXY";

ChangeStreamProxyStage::ChangeStreamProxyStage(OperationContext* opCtx,
                                               std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
                                               WorkingSet* ws)
    : PipelineProxyStage(opCtx, std::move(pipeline), ws, kStageType) {
    // Set _postBatchResumeToken to the initial PBRT that was added to the expression context during
    // pipeline construction, and use it to obtain the starting time for _latestOplogTimestamp.
    invariant(!_pipeline->getContext()->initialPostBatchResumeToken.isEmpty());
    _postBatchResumeToken = _pipeline->getContext()->initialPostBatchResumeToken.getOwned();
    _latestOplogTimestamp = ResumeToken::parse(_postBatchResumeToken).getData().clusterTime;
}

boost::optional<BSONObj> ChangeStreamProxyStage::getNextBson() {
    if (auto next = _pipeline->getNext()) {
        // While we have more results to return, we track both the timestamp and the resume token of
        // the latest event observed in the oplog, the latter via its sort key metadata field.
        auto nextBSON = _validateAndConvertToBSON(*next);
        _latestOplogTimestamp = PipelineD::getLatestOplogTimestamp(_pipeline.get());
        _postBatchResumeToken = next->metadata().getSortKey();
        _setSpeculativeReadTimestamp();
        return nextBSON;
    }

    // We ran out of results to return. Check whether the oplog cursor has moved forward since the
    // last recorded timestamp. Because we advance _latestOplogTimestamp for every event we return,
    // if the new time is higher than the last then we are guaranteed not to have already returned
    // any events at this timestamp. We can set _postBatchResumeToken to a new high-water-mark token
    // at the current clusterTime.
    auto highWaterMark = PipelineD::getLatestOplogTimestamp(_pipeline.get());
    if (highWaterMark > _latestOplogTimestamp) {
        auto token = ResumeToken::makeHighWaterMarkToken(highWaterMark);
        _postBatchResumeToken = token.toDocument().toBson();
        _latestOplogTimestamp = highWaterMark;
        _setSpeculativeReadTimestamp();
    }
    return boost::none;
}

BSONObj ChangeStreamProxyStage::_validateAndConvertToBSON(const Document& event) const {
    // If we are producing output to be merged on mongoS, then no stages can have modified the _id.
    if (_includeMetaData) {
        return event.toBsonWithMetaData();
    }
    // Confirm that the document _id field matches the original resume token in the sort key field.
    auto eventBSON = event.toBson();
    auto resumeToken = event.metadata().getSortKey();
    auto idField = eventBSON.getObjectField("_id");
    invariant(!resumeToken.isEmpty());
    uassert(ErrorCodes::ChangeStreamFatalError,
            str::stream() << "Encountered an event whose _id field, which contains the resume "
                             "token, was modified by the pipeline. Modifying the _id field of an "
                             "event makes it impossible to resume the stream from that point. Only "
                             "transformations that retain the unmodified _id field are allowed. "
                             "Expected: "
                          << BSON("_id" << resumeToken)
                          << " but found: "
                          << (eventBSON["_id"] ? BSON("_id" << eventBSON["_id"]) : BSONObj()),
            idField.binaryEqual(resumeToken));
    return eventBSON;
}

void ChangeStreamProxyStage::_setSpeculativeReadTimestamp() {
    repl::SpeculativeMajorityReadInfo& speculativeMajorityReadInfo =
        repl::SpeculativeMajorityReadInfo::get(_pipeline->getContext()->opCtx);
    if (speculativeMajorityReadInfo.isSpeculativeRead() && !_latestOplogTimestamp.isNull()) {
        speculativeMajorityReadInfo.setSpeculativeReadTimestampForward(_latestOplogTimestamp);
    }
}

std::unique_ptr<PlanStageStats> ChangeStreamProxyStage::getStats() {
    std::unique_ptr<PlanStageStats> ret =
        std::make_unique<PlanStageStats>(CommonStats(kStageType), STAGE_CHANGE_STREAM_PROXY);
    ret->specific = std::make_unique<CollectionScanStats>();
    return ret;
}

}  // namespace mongo
