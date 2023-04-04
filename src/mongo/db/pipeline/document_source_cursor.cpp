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

#include "mongo/db/pipeline/document_source_cursor.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/logv2/log.h"
#include "mongo/s/resharding/resume_token_gen.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangBeforeDocumentSourceCursorLoadBatch);

using boost::intrusive_ptr;
using std::shared_ptr;
using std::string;

const char* DocumentSourceCursor::getSourceName() const {
    return kStageName.rawData();
}

bool DocumentSourceCursor::Batch::isEmpty() const {
    switch (_type) {
        case CursorType::kRegular:
            return _batchOfDocs.empty();
        case CursorType::kEmptyDocuments:
            return !_count;
    }
    MONGO_UNREACHABLE;
}

void DocumentSourceCursor::Batch::enqueue(Document&& doc) {
    switch (_type) {
        case CursorType::kRegular: {
            invariant(doc.isOwned());
            _batchOfDocs.push_back(std::move(doc));
            _memUsageBytes += _batchOfDocs.back().getApproximateSize();
            break;
        }
        case CursorType::kEmptyDocuments: {
            ++_count;
            break;
        }
    }
}

Document DocumentSourceCursor::Batch::dequeue() {
    invariant(!isEmpty());
    switch (_type) {
        case CursorType::kRegular: {
            Document out = std::move(_batchOfDocs.front());
            _batchOfDocs.pop_front();
            if (_batchOfDocs.empty()) {
                _memUsageBytes = 0;
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

void DocumentSourceCursor::Batch::clear() {
    _batchOfDocs.clear();
    _count = 0;
    _memUsageBytes = 0;
}

DocumentSource::GetNextResult DocumentSourceCursor::doGetNext() {
    if (_currentBatch.isEmpty()) {
        loadBatch();
    }

    // If we are tracking the oplog timestamp, update our cached latest optime.
    if (_trackOplogTS && _exec)
        _updateOplogTimestamp();

    if (_currentBatch.isEmpty())
        return GetNextResult::makeEOF();

    return _currentBatch.dequeue();
}

void DocumentSourceCursor::loadBatch() {
    if (!_exec || _exec->isDisposed()) {
        // No more documents.
        return;
    }

    while (MONGO_unlikely(hangBeforeDocumentSourceCursorLoadBatch.shouldFail())) {
        LOGV2(20895,
              "Hanging aggregation due to 'hangBeforeDocumentSourceCursorLoadBatch' failpoint");
        sleepmillis(10);
    }

    PlanExecutor::ExecState state;
    Document resultObj;

    boost::optional<AutoGetCollectionForReadMaybeLockFree> autoColl;
    tassert(5565800,
            "Expected PlanExecutor to use an external lock policy",
            _exec->lockPolicy() == PlanExecutor::LockPolicy::kLockExternally);
    autoColl.emplace(
        pExpCtx->opCtx,
        _exec->nss(),
        AutoGetCollection::Options{}.secondaryNssOrUUIDs(_exec->getSecondaryNamespaces()));
    uassertStatusOK(repl::ReplicationCoordinator::get(pExpCtx->opCtx)
                        ->checkCanServeReadsFor(pExpCtx->opCtx, _exec->nss(), true));

    _exec->restoreState(autoColl ? &autoColl->getCollection() : nullptr);

    try {
        ON_BLOCK_EXIT([this] { recordPlanSummaryStats(); });

        while ((state = _exec->getNextDocument(&resultObj, nullptr)) == PlanExecutor::ADVANCED) {
            _currentBatch.enqueue(transformDoc(std::move(resultObj)));

            // As long as we're waiting for inserts, we shouldn't do any batching at this level we
            // need the whole pipeline to see each document to see if we should stop waiting.
            if (awaitDataState(pExpCtx->opCtx).shouldWaitForInserts ||
                static_cast<long long>(_currentBatch.memUsageBytes()) >
                    internalDocumentSourceCursorBatchSizeBytes.load()) {
                // End this batch and prepare PlanExecutor for yielding.
                _exec->saveState();
                return;
            }
        }

        invariant(state == PlanExecutor::IS_EOF);

        // Keep the inner PlanExecutor alive if the cursor is tailable, since more results may
        // become available in the future, or if we are tracking the latest oplog timestamp, since
        // we will need to retrieve the last timestamp the executor observed before hitting EOF.
        if (_trackOplogTS || pExpCtx->isTailableAwaitData()) {
            _exec->saveState();
            return;
        }
    } catch (...) {
        // Record error details before re-throwing the exception.
        _execStatus = exceptionToStatus().withContext("Error in $cursor stage");
        throw;
    }

    // If we got here, there won't be any more documents and we no longer need our PlanExecutor, so
    // destroy it.
    cleanupExecutor();
}

void DocumentSourceCursor::_updateOplogTimestamp() {
    // If we are about to return a result, set our oplog timestamp to the optime of that result.
    if (!_currentBatch.isEmpty()) {
        const auto& ts = _currentBatch.peekFront().getField(repl::OpTime::kTimestampFieldName);
        invariant(ts.getType() == BSONType::bsonTimestamp);
        _latestOplogTimestamp = ts.getTimestamp();
        return;
    }

    // If we have no more results to return, advance to the latest oplog timestamp.
    _latestOplogTimestamp = _exec->getLatestOplogTimestamp();
}

void DocumentSourceCursor::recordPlanSummaryStats() {
    invariant(_exec);
    _exec->getPlanExplainer().getSummaryStats(&_stats.planSummaryStats);
}

Value DocumentSourceCursor::serialize(SerializationOptions opts) const {
    auto verbosity = opts.verbosity;
    if (opts.redactIdentifiers || opts.replacementForLiteralArgs) {
        MONGO_UNIMPLEMENTED_TASSERT(7484350);
    }
    // We never parse a DocumentSourceCursor, so we only serialize for explain.
    if (!verbosity)
        return Value();

    invariant(_exec);

    uassert(50660,
            "Mismatch between verbosity passed to serialize() and expression context verbosity",
            verbosity == pExpCtx->explain);

    MutableDocument out;

    BSONObjBuilder explainStatsBuilder;

    {
        auto opCtx = pExpCtx->opCtx;
        auto secondaryNssList = _exec->getSecondaryNamespaces();
        AutoGetCollectionForReadMaybeLockFree readLock(
            opCtx,
            _exec->nss(),
            AutoGetCollection::Options{}.secondaryNssOrUUIDs(secondaryNssList));
        MultipleCollectionAccessor collections(opCtx,
                                               &readLock.getCollection(),
                                               readLock.getNss(),
                                               readLock.isAnySecondaryNamespaceAViewOrSharded(),
                                               secondaryNssList);

        Explain::explainStages(_exec.get(),
                               collections,
                               verbosity.value(),
                               _execStatus,
                               _winningPlanTrialStats,
                               BSONObj(),
                               BSONObj(),
                               &explainStatsBuilder);
    }

    BSONObj explainStats = explainStatsBuilder.obj();
    invariant(explainStats["queryPlanner"]);
    out["queryPlanner"] = Value(explainStats["queryPlanner"]);

    if (verbosity.value() >= ExplainOptions::Verbosity::kExecStats) {
        invariant(explainStats["executionStats"]);
        out["executionStats"] = Value(explainStats["executionStats"]);
    }

    return Value(DOC(getSourceName() << out.freezeToValue()));
}

void DocumentSourceCursor::detachFromOperationContext() {
    // Only detach the underlying executor if it hasn't been detached already.
    if (_exec && _exec->getOpCtx()) {
        _exec->detachFromOperationContext();
    }
}

void DocumentSourceCursor::reattachToOperationContext(OperationContext* opCtx) {
    if (_exec) {
        _exec->reattachToOperationContext(opCtx);
    }
}

void DocumentSourceCursor::doDispose() {
    _currentBatch.clear();
    if (!_exec || _exec->isDisposed()) {
        // We've already properly disposed of our PlanExecutor.
        return;
    }
    cleanupExecutor();
}

void DocumentSourceCursor::cleanupExecutor() {
    invariant(_exec);
    _exec->dispose(pExpCtx->opCtx);

    // Not freeing _exec if we're in explain mode since it will be used in serialize() to gather
    // execution stats.
    if (!pExpCtx->explain) {
        _exec.reset();
    }
}

BSONObj DocumentSourceCursor::getPostBatchResumeToken() const {
    if (_trackOplogTS) {
        return ResumeTokenOplogTimestamp{getLatestOplogTimestamp()}.toBSON();
    }
    return BSONObj{};
}

DocumentSourceCursor::~DocumentSourceCursor() {
    if (pExpCtx->explain) {
        invariant(_exec->isDisposed());  // _exec should have at least been disposed.
    } else {
        invariant(!_exec);  // '_exec' should have been cleaned up via dispose() before destruction.
    }
}

DocumentSourceCursor::DocumentSourceCursor(
    const MultipleCollectionAccessor& collections,
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
    const intrusive_ptr<ExpressionContext>& pCtx,
    CursorType cursorType,
    bool trackOplogTimestamp)
    : DocumentSource(kStageName, pCtx),
      _currentBatch(cursorType),
      _exec(std::move(exec)),
      _trackOplogTS(trackOplogTimestamp),
      _queryFramework(_exec->getQueryFramework()) {
    // It is illegal for both 'kEmptyDocuments' and 'trackOplogTimestamp' to be set.
    invariant(!(cursorType == CursorType::kEmptyDocuments && trackOplogTimestamp));

    // Later code in the DocumentSourceCursor lifecycle expects that '_exec' is in a saved state.
    _exec->saveState();

    auto&& explainer = _exec->getPlanExplainer();
    _planSummary = explainer.getPlanSummary();
    recordPlanSummaryStats();

    if (pExpCtx->explain) {
        // It's safe to access the executor even if we don't have the collection lock since we're
        // just going to call getStats() on it.
        _winningPlanTrialStats = explainer.getWinningPlanTrialStats();
    }

    if (collections.hasMainCollection()) {
        const auto& coll = collections.getMainCollection();
        CollectionQueryInfo::get(coll).notifyOfQuery(pExpCtx->opCtx, coll, _stats.planSummaryStats);
    }
    for (auto& [nss, coll] : collections.getSecondaryCollections()) {
        if (coll) {
            PlanSummaryStats stats;
            explainer.getSecondarySummaryStats(nss.toString(), &stats);
            CollectionQueryInfo::get(coll).notifyOfQuery(pExpCtx->opCtx, coll, stats);
        }
    }
}

intrusive_ptr<DocumentSourceCursor> DocumentSourceCursor::create(
    const MultipleCollectionAccessor& collections,
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
    const intrusive_ptr<ExpressionContext>& pExpCtx,
    CursorType cursorType,
    bool trackOplogTimestamp) {
    intrusive_ptr<DocumentSourceCursor> source(new DocumentSourceCursor(
        collections, std::move(exec), pExpCtx, cursorType, trackOplogTimestamp));
    return source;
}
}  // namespace mongo
