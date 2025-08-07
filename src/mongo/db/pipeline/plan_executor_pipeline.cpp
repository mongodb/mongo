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

#include "mongo/db/pipeline/plan_executor_pipeline.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/change_stream_start_after_invalidate_info.h"
#include "mongo/db/pipeline/change_stream_topology_change_info.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/pipeline/plan_explainer_pipeline.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/speculative_majority_read_info.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {
namespace {
auto& changeStreamsLargeEventsFailedCounter =
    *MetricBuilder<Counter64>{"changeStreams.largeEventsFailed"};

// Tests if we are currently running in a router or in a replica set context.
bool isRouterOrReplicaSet(ExpressionContext* expCtx) {
    if (expCtx->getInRouter()) {
        return true;
    }
    auto* replCoord = repl::ReplicationCoordinator::get(expCtx->getOperationContext());
    return replCoord && replCoord->getSettings().isReplSet();
}

}  // namespace

PlanExecutorPipeline::PlanExecutorPipeline(boost::intrusive_ptr<ExpressionContext> expCtx,
                                           std::unique_ptr<Pipeline> pipeline,
                                           ResumableScanType resumableScanType)
    : _expCtx(std::move(expCtx)),
      _pipeline(std::move(pipeline)),
      _execPipeline(exec::agg::buildPipeline(_pipeline->freeze())),
      _planExplainer{_pipeline.get(), _execPipeline.get()},
      _resumableScanType{resumableScanType} {
    // Pipeline plan executors must always have an ExpressionContext.
    invariant(_expCtx);

    // The caller is responsible for disposing this plan executor before deleting it, which will in
    // turn dispose the underlying pipeline. Therefore, there is no need to dispose the pipeline
    // again when it is destroyed.
    _execPipeline->dismissDisposal();

    if (ResumableScanType::kNone != resumableScanType) {
        // For a resumable scan, set the initial _latestOplogTimestamp and _postBatchResumeToken.
        _initializeResumableScanState();
    }
}

PlanExecutor::ExecState PlanExecutorPipeline::getNext(BSONObj* objOut, RecordId* recordIdOut) {
    // The pipeline-based execution engine does not track the record ids associated with documents,
    // so it is an error for the caller to ask for one.
    invariant(!recordIdOut);

    if (!_stash.empty()) {
        if (objOut) {
            *objOut = std::move(_stash.front());
        }
        _stash.pop();
        _planExplainer.incrementNReturned();
        return PlanExecutor::ADVANCED;
    }

    if (auto next = _getNext()) {
        if (objOut) {
            *objOut = _trySerializeToBson(*next);
        }
        _planExplainer.incrementNReturned();
        return PlanExecutor::ADVANCED;
    }
    return PlanExecutor::IS_EOF;
}

PlanExecutor::ExecState PlanExecutorPipeline::getNextDocument(Document& docOut) {
    // Callers which use 'stashResult()' are not allowed to use 'getNextDocument()', and must
    // instead use 'getNext()'.
    tassert(10842100, "expecting stash to be empty in getNextDocument", _stash.empty());

    if (auto next = _getNext()) {
        docOut = std::move(*next);
        _planExplainer.incrementNReturned();
        return PlanExecutor::ADVANCED;
    }

    return PlanExecutor::IS_EOF;
}

bool PlanExecutorPipeline::isEOF() const {
    return _stash.empty() && _pipelineIsEof;
}

boost::optional<Document> PlanExecutorPipeline::_getNext() {
    auto nextDoc = _tryGetNext();
    if (nextDoc) {
        // No change stream control events should ever escape an aggregation pipeline on the router
        // or the replica set.
        tassert(10358906,
                "No control events should escape this aggregation pipeline",
                !nextDoc->metadata().isChangeStreamControlEvent() ||
                    !isRouterOrReplicaSet(_expCtx.get()));
    } else {
        _pipelineIsEof = true;
    }

    if (ResumableScanType::kNone != _resumableScanType) {
        _updateResumableScanState(nextDoc);
    }
    return nextDoc;
}

boost::optional<Document> PlanExecutorPipeline::_tryGetNext() try {
    return _execPipeline->getNext();
} catch (const ExceptionFor<ErrorCodes::ChangeStreamTopologyChange>& ex) {
    // This exception contains the next document to be returned by the pipeline.
    const auto extraInfo = ex.extraInfo<ChangeStreamTopologyChangeInfo>();
    tassert(5669600, "Missing ChangeStreamTopologyChangeInfo on exception", extraInfo);
    return Document::fromBsonWithMetaData(extraInfo->getTopologyChangeEvent());
} catch (const ExceptionFor<ErrorCodes::ChangeStreamStartAfterInvalidate>& ex) {
    // This exception contains an event that captures the client-provided resume token.
    const auto extraInfo = ex.extraInfo<ChangeStreamStartAfterInvalidateInfo>();
    tassert(5779202, "Missing ChangeStreamStartAfterInvalidationInfo on exception", extraInfo);
    return Document::fromBsonWithMetaData(extraInfo->getStartAfterInvalidateEvent());
}

BSONObj PlanExecutorPipeline::_trySerializeToBson(const Document& doc) try {
    // Include metadata if the output will be consumed by a merging node.
    return _expCtx->getNeedsMerge() || _expCtx->getForPerShardCursor() ? doc.toBsonWithMetaData()
                                                                       : doc.toBson();
} catch (const ExceptionFor<ErrorCodes::BSONObjectTooLarge>&) {
    // If in a change stream pipeline, increment change stream large event failed error
    // count metric.
    if (ResumableScanType::kChangeStream == _resumableScanType) {
        changeStreamsLargeEventsFailedCounter.increment();
    }
    throw;
}

void PlanExecutorPipeline::_updateResumableScanState(const boost::optional<Document>& document) {
    switch (_resumableScanType) {
        case ResumableScanType::kChangeStream:
            _performChangeStreamsAccounting(document);
            break;
        case ResumableScanType::kOplogScan:
            _performResumableOplogScanAccounting();
            break;
        case ResumableScanType::kNaturalOrderScan:
            _performResumableNaturalOrderScanAccounting();
            break;
        case ResumableScanType::kNone:
            break;
        default:
            MONGO_UNREACHABLE_TASSERT(5353402);
    }
}

void PlanExecutorPipeline::_performChangeStreamsAccounting(const boost::optional<Document>& doc) {
    tassert(5353405,
            "expected _resumableScanType == kChangeStream",
            ResumableScanType::kChangeStream == _resumableScanType);
    if (doc) {
        // While we have more results to return, we track both the timestamp and the resume token of
        // the latest event observed in the oplog, the latter via its sort key metadata field.
        _validateChangeStreamsResumeToken(*doc);
        _latestOplogTimestamp = PipelineD::getLatestOplogTimestamp(_execPipeline.get());
        _postBatchResumeToken = doc->metadata().getSortKey().getDocument().toBson();
        _setSpeculativeReadTimestamp();
    } else {
        // We ran out of results to return. Check whether the oplog cursor has moved forward since
        // the last recorded timestamp. Because we advance _latestOplogTimestamp for every event we
        // return, if the new time is higher than the last then we are guaranteed not to have
        // already returned any events at this timestamp. We can set _postBatchResumeToken to a new
        // high-water-mark token at the current clusterTime.
        auto highWaterMark = PipelineD::getLatestOplogTimestamp(_execPipeline.get());
        if (highWaterMark > _latestOplogTimestamp) {
            auto token = ResumeToken::makeHighWaterMarkToken(
                highWaterMark, _pipeline->getContext()->getChangeStreamTokenVersion());
            _postBatchResumeToken = token.toDocument().toBson();
            _latestOplogTimestamp = highWaterMark;
            _setSpeculativeReadTimestamp();
        }
    }
}

void PlanExecutorPipeline::_validateChangeStreamsResumeToken(const Document& event) const {
    // Confirm that the document _id field matches the original resume token in the sort key field.
    auto resumeToken = event.metadata().getSortKey();
    auto idField = event.getField("_id");
    invariant(!resumeToken.missing());
    uassert(ErrorCodes::ChangeStreamFatalError,
            str::stream() << "Encountered an event whose _id field, which contains the resume "
                             "token, was modified by the pipeline. Modifying the _id field of an "
                             "event makes it impossible to resume the stream from that point. Only "
                             "transformations that retain the unmodified _id field are allowed. "
                             "Expected: "
                          << BSON("_id" << resumeToken) << " but found: "
                          << (idField.missing() ? BSONObj() : BSON("_id" << idField)),
            resumeToken.getType() == BSONType::object &&
                ValueComparator::kInstance.evaluate(idField == resumeToken));
}

void PlanExecutorPipeline::_performResumableOplogScanAccounting() {
    tassert(5353404,
            "expected _resumableScanType == kOplogScan",
            ResumableScanType::kOplogScan == _resumableScanType);

    // Update values of latest oplog timestamp and postBatchResumeToken.
    _latestOplogTimestamp = PipelineD::getLatestOplogTimestamp(_execPipeline.get());
    _postBatchResumeToken = PipelineD::getPostBatchResumeToken(_execPipeline.get());
    _setSpeculativeReadTimestamp();
}

void PlanExecutorPipeline::_performResumableNaturalOrderScanAccounting() {
    tassert(7979200,
            "expected _resumableScanType == kNaturalOrderScan",
            ResumableScanType::kNaturalOrderScan == _resumableScanType);

    // Update value of postBatchResumeToken.
    _postBatchResumeToken = PipelineD::getPostBatchResumeToken(_execPipeline.get());
}

void PlanExecutorPipeline::_setSpeculativeReadTimestamp() {
    repl::SpeculativeMajorityReadInfo& speculativeMajorityReadInfo =
        repl::SpeculativeMajorityReadInfo::get(_expCtx->getOperationContext());
    if (speculativeMajorityReadInfo.isSpeculativeRead() && !_latestOplogTimestamp.isNull()) {
        speculativeMajorityReadInfo.setSpeculativeReadTimestampForward(_latestOplogTimestamp);
    }
}

void PlanExecutorPipeline::_initializeResumableScanState() {
    switch (_resumableScanType) {
        case ResumableScanType::kChangeStream:
            // Set _postBatchResumeToken to the initial PBRT that was added to the expression
            // context during pipeline construction, and use it to obtain the starting time for
            // _latestOplogTimestamp.
            tassert(5353403,
                    "expected initialPostBatchResumeToken to be not empty",
                    !_expCtx->getInitialPostBatchResumeToken().isEmpty());
            _postBatchResumeToken = _expCtx->getInitialPostBatchResumeToken().getOwned();
            _latestOplogTimestamp = ResumeToken::parse(_postBatchResumeToken).getData().clusterTime;
            break;
        case ResumableScanType::kOplogScan:
            // Initialize the oplog timestamp and postBatchResumeToken here in case the request has
            // batchSize 0, in which case the PBRT of the first batch would be empty.
            _performResumableOplogScanAccounting();
            break;
        case ResumableScanType::kNaturalOrderScan:
        case ResumableScanType::kNone:
            break;
        default:
            MONGO_UNREACHABLE_TASSERT(5353401);
    }
}

void PlanExecutorPipeline::markAsKilled(Status killStatus) {
    invariant(!killStatus.isOK());
    // If killed multiple times, only retain the first status.
    if (_killStatus.isOK()) {
        _killStatus = killStatus;
    }
}

PlanExecutor::QueryFramework PlanExecutorPipeline::getQueryFramework() const {
    // If this executor has a $cursor source at the front, use the query framework of that executor
    // backing the cursor stage in order to determine whether the current pipeline is a hybrid plan.
    if (auto cursor = dynamic_cast<DocumentSourceCursor*>(_pipeline->peekFront())) {
        switch (cursor->getQueryFramework()) {
            case PlanExecutor::QueryFramework::kClassicOnly:
                return PlanExecutor::QueryFramework::kClassicHybrid;
            case PlanExecutor::QueryFramework::kSBEOnly:
                return PlanExecutor::QueryFramework::kSBEHybrid;
            default:
                MONGO_UNREACHABLE_TASSERT(6884701);
        }
    }
    // If this executor doesn't have a $cursor source, then return classicOnly as it cannot be a
    // hybrid plan.
    return PlanExecutor::QueryFramework::kClassicOnly;
}

}  // namespace mongo
