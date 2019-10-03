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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_cursor.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangBeforeDocumentSourceCursorLoadBatch);

using boost::intrusive_ptr;
using std::shared_ptr;
using std::string;

const char* DocumentSourceCursor::getSourceName() const {
    return kStageName.rawData();
}

DocumentSource::GetNextResult DocumentSourceCursor::doGetNext() {
    if (_currentBatch.empty()) {
        loadBatch();
    }

    // If we are tracking the oplog timestamp, update our cached latest optime.
    if (_trackOplogTS && _exec)
        _updateOplogTimestamp();

    if (_currentBatch.empty())
        return GetNextResult::makeEOF();

    Document out = std::move(_currentBatch.front());
    _currentBatch.pop_front();
    return std::move(out);
}

Document DocumentSourceCursor::transformBSONObjToDocument(const BSONObj& obj) const {
    // Aggregation assumes ownership of underlying BSON.
    return _dependencies ? _dependencies->extractFields(obj)
                         : (_inputHasMetadata ? Document::fromBsonWithMetaData(obj.getOwned())
                                              : Document(obj.getOwned()));
}

void DocumentSourceCursor::loadBatch() {
    if (!_exec || _exec->isDisposed()) {
        // No more documents.
        return;
    }

    while (MONGO_unlikely(hangBeforeDocumentSourceCursorLoadBatch.shouldFail())) {
        log() << "Hanging aggregation due to 'hangBeforeDocumentSourceCursorLoadBatch' failpoint";
        sleepmillis(10);
    }

    PlanExecutor::ExecState state;
    BSONObj resultObj;
    {
        AutoGetCollectionForRead autoColl(pExpCtx->opCtx, _exec->nss());
        uassertStatusOK(repl::ReplicationCoordinator::get(pExpCtx->opCtx)
                            ->checkCanServeReadsFor(pExpCtx->opCtx, _exec->nss(), true));

        _exec->restoreState();

        int memUsageBytes = 0;
        {
            ON_BLOCK_EXIT([this] { recordPlanSummaryStats(); });

            while ((state = _exec->getNext(&resultObj, nullptr)) == PlanExecutor::ADVANCED) {
                if (_shouldProduceEmptyDocs) {
                    _currentBatch.push_back(Document());
                } else {
                    _currentBatch.push_back(transformBSONObjToDocument(resultObj));
                }

                if (_limit) {
                    if (++_docsAddedToBatches == _limit->getLimit()) {
                        break;
                    }
                    verify(_docsAddedToBatches < _limit->getLimit());
                }

                memUsageBytes += _currentBatch.back().getApproximateSize();

                // As long as we're waiting for inserts, we shouldn't do any batching at this level
                // we need the whole pipeline to see each document to see if we should stop waiting.
                if (awaitDataState(pExpCtx->opCtx).shouldWaitForInserts ||
                    memUsageBytes > internalDocumentSourceCursorBatchSizeBytes.load()) {
                    // End this batch and prepare PlanExecutor for yielding.
                    _exec->saveState();
                    return;
                }
            }
            // Special case for tailable cursor -- EOF doesn't preclude more results, so keep
            // the PlanExecutor alive.
            if (state == PlanExecutor::IS_EOF && pExpCtx->isTailableAwaitData()) {
                _exec->saveState();
                return;
            }
        }

        // If we got here, there won't be any more documents, so destroy our PlanExecutor. Note we
        // must hold a collection lock to destroy '_exec', but we can only assume that our locks are
        // still held if '_exec' did not end in an error. If '_exec' encountered an error during a
        // yield, the locks might be yielded.
        if (state != PlanExecutor::FAILURE) {
            cleanupExecutor();
        }
    }

    switch (state) {
        case PlanExecutor::ADVANCED:
        case PlanExecutor::IS_EOF:
            return;  // We've reached our limit or exhausted the cursor.
        case PlanExecutor::FAILURE: {
            _execStatus = WorkingSetCommon::getMemberObjectStatus(resultObj).withContext(
                "Error in $cursor stage");
            uassertStatusOK(_execStatus);
        }
        default:
            MONGO_UNREACHABLE;
    }
}

void DocumentSourceCursor::_updateOplogTimestamp() {
    // If we are about to return a result, set our oplog timestamp to the optime of that result.
    if (!_currentBatch.empty()) {
        const auto& ts = _currentBatch.front().getField(repl::OpTime::kTimestampFieldName);
        invariant(ts.getType() == BSONType::bsonTimestamp);
        _latestOplogTimestamp = ts.getTimestamp();
        return;
    }

    // If we have no more results to return, advance to the latest oplog timestamp.
    _latestOplogTimestamp = _exec->getLatestOplogTimestamp();
}

Pipeline::SourceContainer::iterator DocumentSourceCursor::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    if (std::next(itr) == container->end()) {
        return container->end();
    }

    auto nextLimit = dynamic_cast<DocumentSourceLimit*>((*std::next(itr)).get());

    if (nextLimit) {
        if (_limit) {
            // We already have an internal limit, set it to the more restrictive of the two.
            _limit->setLimit(std::min(_limit->getLimit(), nextLimit->getLimit()));
        } else {
            _limit = nextLimit;
        }
        container->erase(std::next(itr));
        return itr;
    }
    return std::next(itr);
}

void DocumentSourceCursor::recordPlanSummaryStats() {
    invariant(_exec);
    // Aggregation handles in-memory sort outside of the query sub-system. Given that we need to
    // preserve the existing value of hasSortStage rather than overwrite with the underlying
    // PlanExecutor's value.
    auto hasSortStage = _planSummaryStats.hasSortStage;

    Explain::getSummaryStats(*_exec, &_planSummaryStats);

    _planSummaryStats.hasSortStage = hasSortStage;
}

Value DocumentSourceCursor::serialize(boost::optional<ExplainOptions::Verbosity> verbosity) const {
    // We never parse a DocumentSourceCursor, so we only serialize for explain.
    if (!verbosity)
        return Value();

    invariant(_exec);

    uassert(50660,
            "Mismatch between verbosity passed to serialize() and expression context verbosity",
            verbosity == pExpCtx->explain);

    MutableDocument out;
    out["query"] = Value(_query);

    if (!_sort.isEmpty())
        out["sort"] = Value(_sort);

    if (_limit)
        out["limit"] = Value(_limit->getLimit());

    if (!_projection.isEmpty())
        out["fields"] = Value(_projection);

    BSONObjBuilder explainStatsBuilder;

    {
        auto opCtx = pExpCtx->opCtx;
        auto lockMode = getLockModeForQuery(opCtx, _exec->nss());
        AutoGetDb dbLock(opCtx, _exec->nss().db(), lockMode);
        Lock::CollectionLock collLock(opCtx, _exec->nss(), lockMode);
        auto collection = dbLock.getDb()
            ? CollectionCatalog::get(opCtx).lookupCollectionByNamespace(_exec->nss())
            : nullptr;

        Explain::explainStages(_exec.get(),
                               collection,
                               verbosity.get(),
                               _execStatus,
                               _winningPlanTrialStats.get(),
                               BSONObj(),
                               &explainStatsBuilder);
    }

    BSONObj explainStats = explainStatsBuilder.obj();
    invariant(explainStats["queryPlanner"]);
    out["queryPlanner"] = Value(explainStats["queryPlanner"]);

    if (verbosity.get() >= ExplainOptions::Verbosity::kExecStats) {
        invariant(explainStats["executionStats"]);
        out["executionStats"] = Value(explainStats["executionStats"]);
    }

    return Value(DOC(getSourceName() << out.freezeToValue()));
}

void DocumentSourceCursor::detachFromOperationContext() {
    if (_exec && !_exec->isDetached()) {
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

DocumentSourceCursor::~DocumentSourceCursor() {
    if (pExpCtx->explain) {
        invariant(_exec->isDisposed());  // _exec should have at least been disposed.
    } else {
        invariant(!_exec);  // '_exec' should have been cleaned up via dispose() before destruction.
    }
}

DocumentSourceCursor::DocumentSourceCursor(
    Collection* collection,
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
    const intrusive_ptr<ExpressionContext>& pCtx,
    bool trackOplogTimestamp)
    : DocumentSource(kStageName, pCtx),
      _docsAddedToBatches(0),
      _exec(std::move(exec)),
      _trackOplogTS(trackOplogTimestamp) {
    // Later code in the DocumentSourceCursor lifecycle expects that '_exec' is in a saved state.
    _exec->saveState();

    _planSummary = Explain::getPlanSummary(_exec.get());
    recordPlanSummaryStats();

    if (pExpCtx->explain) {
        // It's safe to access the executor even if we don't have the collection lock since we're
        // just going to call getStats() on it.
        _winningPlanTrialStats = Explain::getWinningPlanTrialStats(_exec.get());
    }

    if (collection) {
        CollectionQueryInfo::get(collection).notifyOfQuery(pExpCtx->opCtx, _planSummaryStats);
    }
}

intrusive_ptr<DocumentSourceCursor> DocumentSourceCursor::create(
    Collection* collection,
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
    const intrusive_ptr<ExpressionContext>& pExpCtx,
    bool trackOplogTimestamp) {
    intrusive_ptr<DocumentSourceCursor> source(
        new DocumentSourceCursor(collection, std::move(exec), pExpCtx, trackOplogTimestamp));
    return source;
}
}  // namespace mongo
