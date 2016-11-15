/**
 * Copyright 2011 (c) 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_cursor.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using boost::intrusive_ptr;
using std::shared_ptr;
using std::string;

const char* DocumentSourceCursor::getSourceName() const {
    return "$cursor";
}

DocumentSource::GetNextResult DocumentSourceCursor::getNext() {
    pExpCtx->checkForInterrupt();

    if (_currentBatch.empty()) {
        loadBatch();

        if (_currentBatch.empty())
            return GetNextResult::makeEOF();
    }

    Document out = std::move(_currentBatch.front());
    _currentBatch.pop_front();
    return std::move(out);
}

void DocumentSourceCursor::dispose() {
    _currentBatch.clear();
    if (!_exec) {
        return;
    }

    // We must hold a collection lock to destroy a PlanExecutor to ensure the CursorManager will
    // still exist and can be safely told to deregister the PlanExecutor.
    invariant(pExpCtx->opCtx);
    AutoGetCollection autoColl(pExpCtx->opCtx, _exec->nss(), MODE_IS);
    _exec.reset();
    _rangePreserver.release();
}

void DocumentSourceCursor::loadBatch() {
    if (!_exec) {
        // No more documents.
        dispose();
        return;
    }

    AutoGetCollectionForRead autoColl(pExpCtx->opCtx, _exec->nss());
    _exec->restoreState();

    int memUsageBytes = 0;
    BSONObj obj;
    PlanExecutor::ExecState state;
    {
        ON_BLOCK_EXIT([this] { recordPlanSummaryStats(); });

        while ((state = _exec->getNext(&obj, nullptr)) == PlanExecutor::ADVANCED) {
            if (_shouldProduceEmptyDocs) {
                _currentBatch.push_back(Document());
            } else if (_dependencies) {
                _currentBatch.push_back(_dependencies->extractFields(obj));
            } else {
                _currentBatch.push_back(Document::fromBsonWithMetaData(obj));
            }

            if (_limit) {
                if (++_docsAddedToBatches == _limit->getLimit()) {
                    break;
                }
                verify(_docsAddedToBatches < _limit->getLimit());
            }

            memUsageBytes += _currentBatch.back().getApproximateSize();

            if (memUsageBytes > internalDocumentSourceCursorBatchSizeBytes.load()) {
                // End this batch and prepare PlanExecutor for yielding.
                _exec->saveState();
                return;
            }
        }
    }

    // If we got here, there won't be any more documents, so destroy our PlanExecutor. Note we can't
    // use dispose() since we want to keep the current batch.
    _exec.reset();
    _rangePreserver.release();

    switch (state) {
        case PlanExecutor::ADVANCED:
        case PlanExecutor::IS_EOF:
            return;  // We've reached our limit or exhausted the cursor.
        case PlanExecutor::DEAD: {
            uasserted(ErrorCodes::QueryPlanKilled,
                      str::stream() << "collection or index disappeared when cursor yielded: "
                                    << WorkingSetCommon::toStatusString(obj));
        }
        case PlanExecutor::FAILURE: {
            uasserted(17285,
                      str::stream() << "cursor encountered an error: "
                                    << WorkingSetCommon::toStatusString(obj));
        }
        default:
            MONGO_UNREACHABLE;
    }
}

Pipeline::SourceContainer::iterator DocumentSourceCursor::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

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

Value DocumentSourceCursor::serialize(boost::optional<ExplainOptions::Verbosity> explain) const {
    // We never parse a DocumentSourceCursor, so we only serialize for explain.
    if (!explain)
        return Value();

    // Get planner-level explain info from the underlying PlanExecutor.
    invariant(_exec);
    BSONObjBuilder explainBuilder;
    {
        AutoGetCollectionForRead autoColl(pExpCtx->opCtx, _exec->nss());
        _exec->restoreState();
        Explain::explainStages(_exec.get(), autoColl.getCollection(), *explain, &explainBuilder);
        _exec->saveState();
    }

    MutableDocument out;
    out["query"] = Value(_query);

    if (!_sort.isEmpty())
        out["sort"] = Value(_sort);

    if (_limit)
        out["limit"] = Value(_limit->getLimit());

    if (!_projection.isEmpty())
        out["fields"] = Value(_projection);

    // Add explain results from the query system into the agg explain output.
    BSONObj explainObj = explainBuilder.obj();
    invariant(explainObj.hasField("queryPlanner"));
    out["queryPlanner"] = Value(explainObj["queryPlanner"]);
    if (*explain >= ExplainOptions::Verbosity::kExecStats) {
        invariant(explainObj.hasField("executionStats"));
        out["executionStats"] = Value(explainObj["executionStats"]);
    }

    return Value(DOC(getSourceName() << out.freezeToValue()));
}

void DocumentSourceCursor::detachFromOperationContext() {
    if (_exec) {
        _exec->detachFromOperationContext();
    }
}

void DocumentSourceCursor::reattachToOperationContext(OperationContext* opCtx) {
    if (_exec) {
        _exec->reattachToOperationContext(opCtx);
    }
}

DocumentSourceCursor::DocumentSourceCursor(Collection* collection,
                                           std::unique_ptr<PlanExecutor> exec,
                                           const intrusive_ptr<ExpressionContext>& pCtx)
    : DocumentSource(pCtx),
      _docsAddedToBatches(0),
      _rangePreserver(collection),
      _exec(std::move(exec)),
      _outputSorts(_exec->getOutputSorts()) {

    _planSummary = Explain::getPlanSummary(_exec.get());
    recordPlanSummaryStats();

    if (collection) {
        collection->infoCache()->notifyOfQuery(pExpCtx->opCtx, _planSummaryStats.indexesUsed);
    }
}

intrusive_ptr<DocumentSourceCursor> DocumentSourceCursor::create(
    Collection* collection,
    std::unique_ptr<PlanExecutor> exec,
    const intrusive_ptr<ExpressionContext>& pExpCtx) {
    intrusive_ptr<DocumentSourceCursor> source(
        new DocumentSourceCursor(collection, std::move(exec), pExpCtx));
    return source;
}
}
