/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/agg/cursor_stage.h"

#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/exec/agg/cursor_stage.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/plan_yield_policy_release_memory.h"
#include "mongo/logv2/log.h"
#include "mongo/s/resharding/resume_token_gen.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceCursorToStageFn(
    const boost::intrusive_ptr<DocumentSource>& source) {
    auto documentSource = boost::dynamic_pointer_cast<const DocumentSourceCursor>(source);

    tassert(10422500, "expected 'DocumentSourceCursor' type", documentSource);

    return make_intrusive<exec::agg::CursorStage>(documentSource->kStageName,
                                                  documentSource->_catalogResourceHandle,
                                                  documentSource->getExpCtx(),
                                                  documentSource->_cursorType,
                                                  documentSource->_resumeTrackingType,
                                                  documentSource->_sharedState);
}

namespace exec::agg {

MONGO_FAIL_POINT_DEFINE(hangBeforeDocumentSourceCursorLoadBatch);

REGISTER_AGG_STAGE_MAPPING(cursorStage, DocumentSourceCursor::id, documentSourceCursorToStageFn);

bool CursorStage::Batch::isEmpty() const {
    switch (_type) {
        case CursorType::kRegular:
            return _batchOfDocs.empty();
        case CursorType::kEmptyDocuments:
            return !_count;
    }
    MONGO_UNREACHABLE;
}

void CursorStage::Batch::enqueue(Document&& doc, boost::optional<BSONObj> resumeToken) {
    switch (_type) {
        case CursorType::kRegular: {
            invariant(doc.isOwned());
            _batchOfDocs.push_back(std::move(doc));
            _memUsageBytes += _batchOfDocs.back().getApproximateSize();
            if (resumeToken) {
                _resumeTokens.push_back(*resumeToken);
                dassert(_resumeTokens.size() == _batchOfDocs.size());
            }
            break;
        }
        case CursorType::kEmptyDocuments: {
            ++_count;
            break;
        }
    }
}

Document CursorStage::Batch::dequeue() {
    invariant(!isEmpty());
    switch (_type) {
        case CursorType::kRegular: {
            Document out = std::move(_batchOfDocs.front());
            _batchOfDocs.pop_front();
            if (_batchOfDocs.empty()) {
                _memUsageBytes = 0;
            }
            if (!_resumeTokens.empty()) {
                _resumeTokens.pop_front();
                dassert(_resumeTokens.size() == _batchOfDocs.size());
            }
            return out;
        }
        case CursorType::kEmptyDocuments: {
            --_count;
            return Document{};
        }
    }
    MONGO_UNREACHABLE;
}

void CursorStage::Batch::clear() {
    _batchOfDocs.clear();
    _count = 0;
    _memUsageBytes = 0;
}

CursorStage::CursorStage(StringData stageName,
                         const boost::intrusive_ptr<CatalogResourceHandle>& catalogResourceHandle,
                         const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                         CursorType cursorType,
                         ResumeTrackingType resumeTrackingType,
                         std::shared_ptr<CursorSharedState> sharedState)
    : Stage(stageName, pExpCtx),
      _currentBatch(cursorType),
      _catalogResourceHandle(catalogResourceHandle),
      _resumeTrackingType(resumeTrackingType),
      _sharedState(std::move(sharedState)) {
    _sharedState->execStageCreated = true;
    auto&& explainer = _sharedState->exec->getPlanExplainer();
    _planSummary = explainer.getPlanSummary();

    initializeBatchSizeCounts();
    _batchSizeBytes = static_cast<size_t>(internalDocumentSourceCursorBatchSizeBytes.load());
}

CursorStage::~CursorStage() {
    if (pExpCtx->getExplain()) {
        tassert(10422503,
                "PlanExecutor is not disposed",
                _sharedState->exec->isDisposed());  // exec should have at least been disposed.
    } else {
        tassert(10422504,
                "PlanExecutor is not cleaned up",
                !_sharedState->exec);  // 'exec' should have been cleaned up
                                       // via dispose() before destruction.
    }
}

GetNextResult CursorStage::doGetNext() {
    if (_currentBatch.isEmpty()) {
        loadBatch();
    }

    // If we are tracking the oplog timestamp, update our cached latest optime.
    if (_resumeTrackingType == ResumeTrackingType::kOplog && _sharedState->exec)
        _updateOplogTimestamp();
    else if (_resumeTrackingType == ResumeTrackingType::kNonOplog && _sharedState->exec)
        _updateNonOplogResumeToken();

    if (_currentBatch.isEmpty()) {
        _currentBatch.clear();
        return GetNextResult::makeEOF();
    }

    return _currentBatch.dequeue();
}

void CursorStage::doForceSpill() {
    if (!_sharedState->exec || _sharedState->exec->isDisposed()) {
        return;
    }
    auto opCtx = pExpCtx->getOperationContext();
    std::unique_ptr<PlanYieldPolicy> yieldPolicy =
        PlanYieldPolicyReleaseMemory::make(opCtx,
                                           PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                           boost::none,
                                           _sharedState->exec->nss());
    _sharedState->exec->forceSpill(yieldPolicy.get());
}

void CursorStage::loadBatch() {
    if (!_sharedState->exec || _sharedState->exec->isDisposed()) {
        // No more documents.
        return;
    }

    auto opCtx = pExpCtx->getOperationContext();
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangBeforeDocumentSourceCursorLoadBatch,
        opCtx,
        "hangBeforeDocumentSourceCursorLoadBatch",
        []() {
            LOGV2(20895,
                  "Hanging aggregation due to 'hangBeforeDocumentSourceCursorLoadBatch' failpoint");
        },
        _sharedState->exec->nss());

    tassert(5565800,
            "Expected PlanExecutor to use an external lock policy",
            _sharedState->exec->lockPolicy() == PlanExecutor::LockPolicy::kLockExternally);

    // Acquire catalog resources and ensure they are released at the end of this block.
    _catalogResourceHandle->acquire(opCtx, *_sharedState->exec);
    ON_BLOCK_EXIT([&]() { _catalogResourceHandle->release(); });

    _catalogResourceHandle->checkCanServeReads(opCtx, *_sharedState->exec);
    RestoreContext restoreContext(nullptr);

    try {
        // As soon as we call restoreState(), the executor may hold onto storage engine
        // resources. This includes cases where restoreState() throws an exception. We must
        // guarantee that if an exception is thrown, the executor is cleaned up, along with
        // references to storage engine resources, before the catalog resources are released.
        // This is done in the 'catch' block below.
        _sharedState->exec->restoreState(restoreContext);

        const bool shouldDestroyExec = pullDataFromExecutor(opCtx);

        recordPlanSummaryStats();

        // At any given time only one operation can own the entirety of resources used by a
        // multi-document transaction. As we can perform a remote call during the query
        // execution we will check in the session to avoid deadlocks. If we don't release the
        // storage engine resources used here then we could have two operations interacting with
        // resources of a session at the same time. This will leave the plan in the saved state
        // as a side-effect.
        _sharedState->exec->releaseAllAcquiredResources();

        if (!shouldDestroyExec) {
            return;
        }
    } catch (...) {
        // Record error details before re-throwing the exception.
        _sharedState->execStatus = exceptionToStatus().withContext("Error in $cursor stage");

        // '_exec' must be cleaned up before the catalog resources are freed. Since '_exec' is a
        // member variable, and the catalog resources are maintained via a ScopeGuard within
        // this function, by default, the catalog resources will be released first. In order to
        // get around this, we dispose of '_exec' here.
        doDispose();

        throw;
    }

    // If we got here, there won't be any more documents and we no longer need our PlanExecutor,
    // so destroy it, but leave our current batch intact.
    cleanupExecutor();
}

void CursorStage::_updateOplogTimestamp() {
    // If we are about to return a result, set our oplog timestamp to the optime of that result.
    if (!_currentBatch.isEmpty()) {
        const auto& ts = _currentBatch.peekFront().getField(repl::OpTime::kTimestampFieldName);
        invariant(ts.getType() == BSONType::timestamp);
        _latestOplogTimestamp = ts.getTimestamp();
        return;
    }

    // If we have no more results to return, advance to the latest oplog timestamp.
    _latestOplogTimestamp = _sharedState->exec->getLatestOplogTimestamp();
}

void CursorStage::_updateNonOplogResumeToken() {
    // If we are about to return a result, set our resume token to the one for that result.
    if (!_currentBatch.isEmpty()) {
        _latestNonOplogResumeToken = _currentBatch.peekFrontResumeToken();
        return;
    }

    // If we have no more results to return, advance to the latest executor resume token.
    _latestNonOplogResumeToken = _sharedState->exec->getPostBatchResumeToken();
}

void CursorStage::recordPlanSummaryStats() {
    invariant(_sharedState->exec);
    _sharedState->exec->getPlanExplainer().getSummaryStats(&_sharedState->stats.planSummaryStats);
}

bool CursorStage::pullDataFromExecutor(OperationContext* opCtx) {
    PlanExecutor::ExecState state;
    Document resultObj;

    while ((state = _sharedState->exec->getNextDocument(resultObj)) == PlanExecutor::ADVANCED) {
        boost::optional<BSONObj> resumeToken;
        if (_resumeTrackingType == ResumeTrackingType::kNonOplog)
            resumeToken = _sharedState->exec->getPostBatchResumeToken();
        _currentBatch.enqueue(transformDoc(std::move(resultObj)), std::move(resumeToken));

        // As long as we're waiting for inserts, we shouldn't do any batching at this level we
        // need the whole pipeline to see each document to see if we should stop waiting.
        bool batchCountFull = _batchSizeCount != 0 && _currentBatch.count() >= _batchSizeCount;
        if (batchCountFull || _currentBatch.memUsageBytes() > _batchSizeBytes ||
            awaitDataState(opCtx).shouldWaitForInserts) {
            // Double the size for next batch when batch is full.
            if (batchCountFull && overflow::mul(_batchSizeCount, 2, &_batchSizeCount)) {
                _batchSizeCount = 0;  // Go unlimited if we overflow.
            }
            // Return false indicating the executor should not be destroyed.
            return false;
        }
    }

    tassert(10271304, "Expected PlanExecutor to be EOF", state == PlanExecutor::IS_EOF);

    // Keep the inner PlanExecutor alive if the cursor is tailable, since more results may
    // become available in the future, or if we are tracking the latest oplog resume inforation,
    // since we will need to retrieve the resume information the executor observed before
    // hitting EOF.
    if (_resumeTrackingType != ResumeTrackingType::kNone || pExpCtx->isTailableAwaitData()) {
        return false;
    }

    // Return true indicating the executor should be destroyed.
    return true;
}

void CursorStage::doDispose() {
    _currentBatch.clear();
    if (!_sharedState->exec || _sharedState->exec->isDisposed()) {
        // We've already properly disposed of our PlanExecutor.
        return;
    }
    cleanupExecutor();
}

void CursorStage::cleanupExecutor() {
    invariant(_sharedState->exec);
    _sharedState->exec->dispose(pExpCtx->getOperationContext());

    // Not freeing _exec if we're in explain mode since it will be used in serialize() to gather
    // execution stats.
    if (!pExpCtx->getExplain()) {
        _sharedState->exec.reset();
    }
}

BSONObj CursorStage::getPostBatchResumeToken() const {
    if (_resumeTrackingType == ResumeTrackingType::kOplog) {
        return ResumeTokenOplogTimestamp{getLatestOplogTimestamp()}.toBSON();
    } else if (_resumeTrackingType == ResumeTrackingType::kNonOplog) {
        return _latestNonOplogResumeToken;
    }
    return BSONObj{};
}

void CursorStage::detachFromOperationContext() {
    // Only detach the underlying executor if it hasn't been detached already.
    if (_sharedState->exec && _sharedState->exec->getOpCtx()) {
        _sharedState->exec->detachFromOperationContext();
    }
}

void CursorStage::reattachToOperationContext(OperationContext* opCtx) {
    if (_sharedState->exec && _sharedState->exec->getOpCtx() != opCtx) {
        _sharedState->exec->reattachToOperationContext(opCtx);
    }
}

void CursorStage::initializeBatchSizeCounts() {
    // '0' means there's no limitation.
    _batchSizeCount = 0;
    if (auto cq = _sharedState->exec->getCanonicalQuery()) {
        if (cq->getFindCommandRequest().getLimit().has_value()) {
            // $limit is pushed down into executor, skipping batch size count limitation.
            return;
        }
        for (const auto& ds : cq->cqPipeline()) {
            if (ds->getSourceName() == DocumentSourceLimit::kStageName) {
                // $limit is pushed down into executor, skipping batch size count limitation.
                return;
            }
        }
    }
    // No $limit is pushed down into executor, reading limit from knobs.
    _batchSizeCount = internalDocumentSourceCursorInitialBatchSize.load();
}
}  // namespace exec::agg
}  // namespace mongo
