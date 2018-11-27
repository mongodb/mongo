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

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/pipeline/resume_token.h"

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
    if (!_pipeline->getContext()->needsMerge || _pipeline->getContext()->mergeByPBRT) {
        _latestOplogTimestamp = ResumeToken::parse(_postBatchResumeToken).getData().clusterTime;
    }
}

boost::optional<BSONObj> ChangeStreamProxyStage::getNextBson() {
    if (auto next = _pipeline->getNext()) {
        // While we have more results to return, we track both the timestamp and the resume token of
        // the latest event observed in the oplog, the latter via its sort key metadata field.
        auto nextBSON = (_includeMetaData ? next->toBsonWithMetaData() : next->toBson());
        _latestOplogTimestamp = PipelineD::getLatestOplogTimestamp(_pipeline.get());
        _postBatchResumeToken = next->getSortKeyMetaField();
        return nextBSON;
    }

    // We ran out of results to return. Check whether the oplog cursor has moved forward since the
    // last recorded timestamp. Because we advance _latestOplogTimestamp for every event we return,
    // if the new time is higher than the last then we are guaranteed not to have already returned
    // any events at this timestamp. We can set _postBatchResumeToken to a new high-water-mark token
    // at the current clusterTime.
    auto highWaterMark = PipelineD::getLatestOplogTimestamp(_pipeline.get());
    if (highWaterMark > _latestOplogTimestamp) {
        auto token =
            ResumeToken::makeHighWaterMarkToken(highWaterMark, _pipeline->getContext()->uuid);
        _postBatchResumeToken =
            token.toDocument(ResumeToken::SerializationFormat::kHexString).toBson();
        _latestOplogTimestamp = highWaterMark;
    }
    return boost::none;
}

std::unique_ptr<PlanStageStats> ChangeStreamProxyStage::getStats() {
    std::unique_ptr<PlanStageStats> ret =
        std::make_unique<PlanStageStats>(CommonStats(kStageType), STAGE_CHANGE_STREAM_PROXY);
    ret->specific = std::make_unique<CollectionScanStats>();
    return ret;
}

}  // namespace mongo
