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

#include "mongo/pch.h"

#include "mongo/db/pipeline/document_source.h"

#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/instance.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/query/find_constants.h"
#include "mongo/db/query/type_explain.h"
#include "mongo/db/storage_options.h"
#include "mongo/s/d_logic.h"
#include "mongo/s/stale_exception.h" // for SendStaleConfigException

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
        // Can't call in to Runner or ClientCursor registries from this function since it will be
        // called when an agg cursor is killed which would cause a deadlock.
        _runner.reset();
        _currentBatch.clear();
    }

    void DocumentSourceCursor::loadBatch() {
        if (!_runner) {
            dispose();
            return;
        }

        // We have already validated the sharding version when we constructed the Runner
        // so we shouldn't check it again.
        Lock::DBRead lk(pExpCtx->opCtx->lockState(), _ns);
        Client::Context ctx(_ns, /*doVersion=*/false);

        _runner->restoreState(pExpCtx->opCtx);

        int memUsageBytes = 0;
        BSONObj obj;
        Runner::RunnerState state;
        while ((state = _runner->getNext(&obj, NULL)) == Runner::RUNNER_ADVANCED) {
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
                // End this batch and prepare Runner for yielding.
                _runner->saveState();
                return;
            }
        }

        // If we got here, there won't be any more documents, so destroy the runner. Can't use
        // dispose since we want to keep the _currentBatch.
        _runner.reset();

        uassert(16028, "collection or index disappeared when cursor yielded",
                state != Runner::RUNNER_DEAD);

        uassert(17285, "cursor encountered an error: " + WorkingSetCommon::toStatusString(obj),
                state != Runner::RUNNER_ERROR);

        massert(17286, str::stream() << "Unexpected return from Runner::getNext: " << state,
                state == Runner::RUNNER_EOF || state == Runner::RUNNER_ADVANCED);
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

namespace {
    Document extractInfo(ptr<const TypeExplain> info) {
        MutableDocument out;

        if (info->isClausesSet()) {
            vector<Value> clauses;
            for (size_t i = 0; i < info->sizeClauses(); i++) {
                clauses.push_back(Value(extractInfo(info->getClausesAt(i))));
            }
            out[TypeExplain::clauses()] = Value::consume(clauses);
        }

        if (info->isCursorSet())
            out[TypeExplain::cursor()] = Value(info->getCursor());

        if (info->isIsMultiKeySet())
            out[TypeExplain::isMultiKey()] = Value(info->getIsMultiKey());

        if (info->isScanAndOrderSet())
            out[TypeExplain::scanAndOrder()] = Value(info->getScanAndOrder());

#if 0 // Disabled pending SERVER-12015 since until then no aggs will be index only.
        if (info->isIndexOnlySet())
            out[TypeExplain::indexOnly()] = Value(info->getIndexOnly());
#endif

        if (info->isIndexBoundsSet())
            out[TypeExplain::indexBounds()] = Value(info->getIndexBounds());

        if (info->isAllPlansSet()) {
            vector<Value> allPlans;
            for (size_t i = 0; i < info->sizeAllPlans(); i++) {
                allPlans.push_back(Value(extractInfo(info->getAllPlansAt(i))));
            }
            out[TypeExplain::allPlans()] = Value::consume(allPlans);
        }

        return out.freeze();
    }
} // namespace

    Value DocumentSourceCursor::serialize(bool explain) const {
        // we never parse a documentSourceCursor, so we only serialize for explain
        if (!explain)
            return Value();

        Status explainStatus(ErrorCodes::InternalError, "");
        scoped_ptr<TypeExplain> plan;
        {
            Lock::DBRead lk(pExpCtx->opCtx->lockState(), _ns);
            Client::Context ctx(_ns, /*doVersion=*/false);
            massert(17392, "No _runner. Were we disposed before explained?",
                    _runner);

            _runner->restoreState(pExpCtx->opCtx);

            TypeExplain* explainRaw;
            explainStatus = _runner->getInfo(&explainRaw, NULL);
            if (explainStatus.isOK())
                plan.reset(explainRaw);

            _runner->saveState();
        }

        MutableDocument out;
        out["query"] = Value(_query);

        if (!_sort.isEmpty())
            out["sort"] = Value(_sort);

        if (_limit)
            out["limit"] = Value(_limit->getLimit());

        if (!_projection.isEmpty())
            out["fields"] = Value(_projection);

        if (explainStatus.isOK()) {
            out["plan"] = Value(extractInfo(plan));
        } else {
            out["planError"] = Value(explainStatus.toString());
        }

        return Value(DOC(getSourceName() << out.freezeToValue()));
    }

    DocumentSourceCursor::DocumentSourceCursor(const string& ns,
                                               const boost::shared_ptr<Runner>& runner,
                                               const intrusive_ptr<ExpressionContext> &pCtx)
        : DocumentSource(pCtx)
        , _docsAddedToBatches(0)
        , _ns(ns)
        , _runner(runner)
    {}

    intrusive_ptr<DocumentSourceCursor> DocumentSourceCursor::create(
            const string& ns,
            const boost::shared_ptr<Runner>& runner,
            const intrusive_ptr<ExpressionContext> &pExpCtx) {
        return new DocumentSourceCursor(ns, runner, pExpCtx);
    }

    void DocumentSourceCursor::setProjection(
            const BSONObj& projection,
            const boost::optional<ParsedDeps>& deps) {
        _projection = projection;
        _dependencies = deps;
    }
}
