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

#include "mongo/db/pipeline/plan_executor_pipeline.h"

#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/repl/speculative_majority_read_info.h"

namespace mongo {

PlanExecutorPipeline::PlanExecutorPipeline(boost::intrusive_ptr<ExpressionContext> expCtx,
                                           std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
                                           bool isChangeStream)
    : _expCtx(std::move(expCtx)), _pipeline(std::move(pipeline)), _isChangeStream(isChangeStream) {
    // Pipeline plan executors must always have an ExpressionContext.
    invariant(_expCtx);

    // The caller is responsible for disposing this plan executor before deleting it, which will in
    // turn dispose the underlying pipeline. Therefore, there is no need to dispose the pipeline
    // again when it is destroyed.
    _pipeline.get_deleter().dismissDisposal();

    if (_isChangeStream) {
        // Set _postBatchResumeToken to the initial PBRT that was added to the expression context
        // during pipeline construction, and use it to obtain the starting time for
        // _latestOplogTimestamp.
        invariant(!_expCtx->initialPostBatchResumeToken.isEmpty());
        _postBatchResumeToken = _expCtx->initialPostBatchResumeToken.getOwned();
        _latestOplogTimestamp = ResumeToken::parse(_postBatchResumeToken).getData().clusterTime;
    }
}

PlanExecutor::ExecState PlanExecutorPipeline::getNext(BSONObj* objOut, RecordId* recordIdOut) {
    // The pipeline-based execution engine does not track the record ids associated with documents,
    // so it is an error for the caller to ask for one. For the same reason, we expect the caller to
    // provide a non-null BSONObj pointer for 'objOut'.
    invariant(!recordIdOut);
    invariant(objOut);

    if (!_stash.empty()) {
        *objOut = std::move(_stash.front());
        _stash.pop();
        ++_nReturned;
        return PlanExecutor::ADVANCED;
    }

    Document docOut;
    auto execState = getNextDocument(&docOut, nullptr);
    if (execState == PlanExecutor::ADVANCED) {
        // Include metadata if the output will be consumed by a merging node.
        *objOut = _expCtx->needsMerge ? docOut.toBsonWithMetaData() : docOut.toBson();
    }
    return execState;
}

PlanExecutor::ExecState PlanExecutorPipeline::getNextDocument(Document* docOut,
                                                              RecordId* recordIdOut) {
    // The pipeline-based execution engine does not track the record ids associated with documents,
    // so it is an error for the caller to ask for one. For the same reason, we expect the caller to
    // provide a non-null pointer for 'docOut'.
    invariant(!recordIdOut);
    invariant(docOut);

    // Callers which use 'enqueue()' are not allowed to use 'getNextDocument()', and must instead
    // use 'getNext()'.
    invariant(_stash.empty());

    if (auto next = _getNext()) {
        *docOut = std::move(*next);
        ++_nReturned;
        return PlanExecutor::ADVANCED;
    }

    return PlanExecutor::IS_EOF;
}

bool PlanExecutorPipeline::isEOF() {
    if (!_stash.empty()) {
        return false;
    }

    return _pipelineIsEof;
}

boost::optional<Document> PlanExecutorPipeline::_getNext() {
    auto nextDoc = _pipeline->getNext();
    if (!nextDoc) {
        _pipelineIsEof = true;
    }

    if (_isChangeStream) {
        _performChangeStreamsAccounting(nextDoc);
    }
    return nextDoc;
}

void PlanExecutorPipeline::_performChangeStreamsAccounting(const boost::optional<Document> doc) {
    invariant(_isChangeStream);
    if (doc) {
        // While we have more results to return, we track both the timestamp and the resume token of
        // the latest event observed in the oplog, the latter via its sort key metadata field.
        _validateResumeToken(*doc);
        _latestOplogTimestamp = PipelineD::getLatestOplogTimestamp(_pipeline.get());
        _postBatchResumeToken = doc->metadata().getSortKey().getDocument().toBson();
        _setSpeculativeReadTimestamp();
    } else {
        // We ran out of results to return. Check whether the oplog cursor has moved forward since
        // the last recorded timestamp. Because we advance _latestOplogTimestamp for every event we
        // return, if the new time is higher than the last then we are guaranteed not to have
        // already returned any events at this timestamp. We can set _postBatchResumeToken to a new
        // high-water-mark token at the current clusterTime.
        auto highWaterMark = PipelineD::getLatestOplogTimestamp(_pipeline.get());
        if (highWaterMark > _latestOplogTimestamp) {
            auto token = ResumeToken::makeHighWaterMarkToken(highWaterMark);
            _postBatchResumeToken = token.toDocument().toBson();
            _latestOplogTimestamp = highWaterMark;
            _setSpeculativeReadTimestamp();
        }
    }
}

std::string PlanExecutorPipeline::getPlanSummary() const {
    return PipelineD::getPlanSummaryStr(_pipeline.get());
}

void PlanExecutorPipeline::getSummaryStats(PlanSummaryStats* statsOut) const {
    invariant(statsOut);
    PipelineD::getPlanSummaryStats(_pipeline.get(), statsOut);
    statsOut->nReturned = _nReturned;
}

void PlanExecutorPipeline::_validateResumeToken(const Document& event) const {
    // If we are producing output to be merged on mongoS, then no stages can have modified the _id.
    if (_expCtx->needsMerge) {
        return;
    }

    // Confirm that the document _id field matches the original resume token in the sort key field.
    auto eventBSON = event.toBson();
    auto resumeToken = event.metadata().getSortKey();
    auto idField = eventBSON.getObjectField("_id");
    invariant(!resumeToken.missing());
    uassert(ErrorCodes::ChangeStreamFatalError,
            str::stream() << "Encountered an event whose _id field, which contains the resume "
                             "token, was modified by the pipeline. Modifying the _id field of an "
                             "event makes it impossible to resume the stream from that point. Only "
                             "transformations that retain the unmodified _id field are allowed. "
                             "Expected: "
                          << BSON("_id" << resumeToken) << " but found: "
                          << (eventBSON["_id"] ? BSON("_id" << eventBSON["_id"]) : BSONObj()),
            (resumeToken.getType() == BSONType::Object) &&
                idField.binaryEqual(resumeToken.getDocument().toBson()));
}

void PlanExecutorPipeline::_setSpeculativeReadTimestamp() {
    repl::SpeculativeMajorityReadInfo& speculativeMajorityReadInfo =
        repl::SpeculativeMajorityReadInfo::get(_expCtx->opCtx);
    if (speculativeMajorityReadInfo.isSpeculativeRead() && !_latestOplogTimestamp.isNull()) {
        speculativeMajorityReadInfo.setSpeculativeReadTimestampForward(_latestOplogTimestamp);
    }
}

void PlanExecutorPipeline::markAsKilled(Status killStatus) {
    invariant(!killStatus.isOK());
    // If killed multiple times, only retain the first status.
    if (_killStatus.isOK()) {
        _killStatus = killStatus;
    }
}

}  // namespace mongo
