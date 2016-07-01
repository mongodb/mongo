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

#include "mongo/db/pipeline/document_source.h"

#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/instance.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using boost::intrusive_ptr;
using std::shared_ptr;
using std::string;

DocumentSourceCursor::~DocumentSourceCursor() {
    dispose();
}

const char* DocumentSourceCursor::getSourceName() const {
    return "$cursor";
}

boost::optional<Document> DocumentSourceCursor::getNext() {
    pExpCtx->checkForInterrupt();

    if (_currentBatch.empty()) {
        loadBatch();

        if (_currentBatch.empty())  // exhausted the cursor
            return boost::none;
    }

    Document out = _currentBatch.front();
    _currentBatch.pop_front();
    return out;
}

void DocumentSourceCursor::dispose() {
    // Can't call in to PlanExecutor or ClientCursor registries from this function since it
    // will be called when an agg cursor is killed which would cause a deadlock.
    _exec.reset();
    _currentBatch.clear();
}

void DocumentSourceCursor::loadBatch() {
    if (!_exec) {
        dispose();
        return;
    }

    // We have already validated the sharding version when we constructed the PlanExecutor
    // so we shouldn't check it again.
    const NamespaceString nss(_ns);
    AutoGetCollectionForRead autoColl(pExpCtx->opCtx, nss);

    _exec->restoreState();

    int memUsageBytes = 0;
    BSONObj obj;
    PlanExecutor::ExecState state;
    {
        ON_BLOCK_EXIT([this] { recordPlanSummaryStats(); });

        while ((state = _exec->getNext(&obj, NULL)) == PlanExecutor::ADVANCED) {
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

            if (memUsageBytes > FindCommon::kMaxBytesToReturnToClientAtOnce) {
                // End this batch and prepare PlanExecutor for yielding.
                _exec->saveState();
                return;
            }
        }
    }

    // If we got here, there won't be any more documents, so destroy the executor. Can't use
    // dispose since we want to keep the _currentBatch.
    _exec.reset();

    uassert(16028,
            str::stream() << "collection or index disappeared when cursor yielded: "
                          << WorkingSetCommon::toStatusString(obj),
            state != PlanExecutor::DEAD);

    uassert(17285,
            str::stream() << "cursor encountered an error: "
                          << WorkingSetCommon::toStatusString(obj),
            state != PlanExecutor::FAILURE);

    massert(17286,
            str::stream() << "Unexpected return from PlanExecutor::getNext: " << state,
            state == PlanExecutor::IS_EOF || state == PlanExecutor::ADVANCED);
}

long long DocumentSourceCursor::getLimit() const {
    return _limit ? _limit->getLimit() : -1;
}

Pipeline::SourceContainer::iterator DocumentSourceCursor::optimizeAt(
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


void DocumentSourceCursor::recordPlanSummaryStr() {
    invariant(_exec);
    _planSummary = Explain::getPlanSummary(_exec.get());
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

Value DocumentSourceCursor::serialize(bool explain) const {
    // we never parse a documentSourceCursor, so we only serialize for explain
    if (!explain)
        return Value();

    // Get planner-level explain info from the underlying PlanExecutor.
    BSONObjBuilder explainBuilder;
    {
        const NamespaceString nss(_ns);
        AutoGetCollectionForRead autoColl(pExpCtx->opCtx, nss);

        massert(17392, "No _exec. Were we disposed before explained?", _exec);

        _exec->restoreState();
        Explain::explainStages(
            _exec.get(), autoColl.getCollection(), ExplainCommon::QUERY_PLANNER, &explainBuilder);

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

    return Value(DOC(getSourceName() << out.freezeToValue()));
}

void DocumentSourceCursor::doInjectExpressionContext() {
    if (_limit) {
        _limit->injectExpressionContext(pExpCtx);
    }
}

DocumentSourceCursor::DocumentSourceCursor(const string& ns,
                                           const std::shared_ptr<PlanExecutor>& exec,
                                           const intrusive_ptr<ExpressionContext>& pCtx)
    : DocumentSource(pCtx),
      _docsAddedToBatches(0),
      _ns(ns),
      _exec(exec),
      _outputSorts(exec->getOutputSorts()) {
    recordPlanSummaryStr();

    // We record execution metrics here to allow for capture of indexes used prior to execution.
    recordPlanSummaryStats();
}

intrusive_ptr<DocumentSourceCursor> DocumentSourceCursor::create(
    const string& ns,
    const std::shared_ptr<PlanExecutor>& exec,
    const intrusive_ptr<ExpressionContext>& pExpCtx) {
    intrusive_ptr<DocumentSourceCursor> source(new DocumentSourceCursor(ns, exec, pExpCtx));
    source->injectExpressionContext(pExpCtx);
    return source;
}

void DocumentSourceCursor::setProjection(const BSONObj& projection,
                                         const boost::optional<ParsedDeps>& deps) {
    _projection = projection;
    _dependencies = deps;
}

const std::string& DocumentSourceCursor::getPlanSummaryStr() const {
    return _planSummary;
}

const PlanSummaryStats& DocumentSourceCursor::getPlanSummaryStats() const {
    return _planSummaryStats;
}
}
