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
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/instance.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/find_constants.h"
#include "mongo/db/storage_options.h"
#include "mongo/s/d_state.h"


namespace mongo {

    DocumentSourceCursor::~DocumentSourceCursor() {
        dispose();
    }

    const char *DocumentSourceCursor::getSourceName() const {
        return "$cursor";
    }

    boost::optional<Document> DocumentSourceCursor::getNext() {
        pExpCtx->checkForInterrupt();

        if (_currentBatch.empty()) {
            loadBatch();

            if (_currentBatch.empty()) // exhausted the cursor
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

        _exec->restoreState(pExpCtx->opCtx);

        int memUsageBytes = 0;
        BSONObj obj;
        PlanExecutor::ExecState state;
        while ((state = _exec->getNext(&obj, NULL)) == PlanExecutor::ADVANCED) {
            if (_dependencies) {
                _currentBatch.push_back(_dependencies->extractFields(obj));
            }
            else {
                _currentBatch.push_back(Document::fromBsonWithMetaData(obj));
            }

            if (_limit) {
                if (++_docsAddedToBatches == _limit->getLimit()) {
                    break;
                }
                verify(_docsAddedToBatches < _limit->getLimit());
            }

            memUsageBytes += _currentBatch.back().getApproximateSize();

            if (memUsageBytes > MaxBytesToReturnToClientAtOnce) {
                // End this batch and prepare PlanExecutor for yielding.
                _exec->saveState();
                return;
            }
        }

        // If we got here, there won't be any more documents, so destroy the executor. Can't use
        // dispose since we want to keep the _currentBatch.
        _exec.reset();

        uassert(16028, "collection or index disappeared when cursor yielded",
                state != PlanExecutor::DEAD);

        uassert(17285, "cursor encountered an error: " + WorkingSetCommon::toStatusString(obj),
                state != PlanExecutor::FAILURE);

        massert(17286, str::stream() << "Unexpected return from PlanExecutor::getNext: " << state,
                state == PlanExecutor::IS_EOF || state == PlanExecutor::ADVANCED);
    }

    void DocumentSourceCursor::setSource(DocumentSource *pSource) {
        /* this doesn't take a source */
        verify(false);
    }

    long long DocumentSourceCursor::getLimit() const {
        return _limit ? _limit->getLimit() : -1;
    }

    bool DocumentSourceCursor::coalesce(const intrusive_ptr<DocumentSource>& nextSource) {
        // Note: Currently we assume the $limit is logically after any $sort or
        // $match. If we ever pull in $match or $sort using this method, we
        // will need to keep track of the order of the sub-stages.

        if (!_limit) {
            _limit = dynamic_cast<DocumentSourceLimit*>(nextSource.get());
            return _limit.get(); // false if next is not a $limit
        }
        else {
            return _limit->coalesce(nextSource);
        }

        return false;
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

            _exec->restoreState(pExpCtx->opCtx);
            Explain::explainStages(_exec.get(), ExplainCommon::QUERY_PLANNER, &explainBuilder);
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

    DocumentSourceCursor::DocumentSourceCursor(const string& ns,
                                               const boost::shared_ptr<PlanExecutor>& exec,
                                               const intrusive_ptr<ExpressionContext> &pCtx)
        : DocumentSource(pCtx)
        , _docsAddedToBatches(0)
        , _ns(ns)
        , _exec(exec)
    {}

    intrusive_ptr<DocumentSourceCursor> DocumentSourceCursor::create(
            const string& ns,
            const boost::shared_ptr<PlanExecutor>& exec,
            const intrusive_ptr<ExpressionContext> &pExpCtx) {
        return new DocumentSourceCursor(ns, exec, pExpCtx);
    }

    void DocumentSourceCursor::setProjection(
            const BSONObj& projection,
            const boost::optional<ParsedDeps>& deps) {
        _projection = projection;
        _dependencies = deps;
    }
}
