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

#include "mongo/db/clientcursor.h"
#include "mongo/db/instance.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/query/find_constants.h"
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
        if (_cursorId) {
            ClientCursor::erase(_cursorId);
            _cursorId = 0;
        }

        _currentBatch.clear();
    }

    void DocumentSourceCursor::loadBatch() {
        if (!_cursorId) {
            dispose();
            return;
        }

        // We have already validated the sharding version when we constructed the cursor
        // so we shouldn't check it again.
        Lock::DBRead lk(_ns);
        Client::Context ctx(_ns, storageGlobalParams.dbpath, /*doVersion=*/false);

        ClientCursorPin pin(_cursorId);
        ClientCursor* cursor = pin.c();

        uassert(16950, "Cursor deleted. Was the collection or database dropped?",
                cursor);

        Runner* runner = cursor->getRunner();
        runner->restoreState();

        int memUsageBytes = 0;
        BSONObj obj;
        Runner::RunnerState state;
        while ((state = runner->getNext(&obj, NULL)) == Runner::RUNNER_ADVANCED) {
            // TODO SERVER-11831: consider using documentFromBsonWithDeps(obj, _dependencies)

            _currentBatch.push_back(Document(obj));

            if (_limit) {
                if (++_docsAddedToBatches == _limit->getLimit()) {
                    break;
                }
                verify(_docsAddedToBatches < _limit->getLimit());
            }

            memUsageBytes += _currentBatch.back().getApproximateSize();

            if (memUsageBytes > MaxBytesToReturnToClientAtOnce) {
                // End this batch and prepare cursor for yielding.
                runner->saveState();
                cc().curop()->yielded();
                return;
            }
        }

        // If we got here, there won't be any more documents, so destroy the cursor and runner.
        _cursorId = 0;
        pin.deleteUnderlying();

        uassert(16028, "collection or index disappeared when cursor yielded",
                state != Runner::RUNNER_DEAD);

        uassert(17285, "cursor encountered an error",
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
            return _limit; // false if next is not a $limit
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

        Lock::DBRead lk(_ns);
        Client::Context ctx(_ns, storageGlobalParams.dbpath, /*doVersion=*/false);

        ClientCursorPin pin(_cursorId);
        ClientCursor* cursor = pin.c();

        uassert(17135, "Cursor deleted. Was the collection or database dropped?",
                cursor);

        Runner* runner = cursor->getRunner();
        runner->restoreState();

        return Value(DOC(getSourceName() <<
            DOC("query" << Value(_query)
             << "sort" << (!_sort.isEmpty() ? Value(_sort) : Value())
             << "limit" << (_limit ? Value(_limit->getLimit()) : Value())
             << "fields" << (!_projection.isEmpty() ? Value(_projection) : Value())
             // << "indexOnly" << canUseCoveredIndex(cursor)
             // << "cursorType" << cursor->c()->toString()
        ))); // TODO get more plan information
    }

    DocumentSourceCursor::DocumentSourceCursor(const string& ns,
                                               CursorId cursorId,
                                               const intrusive_ptr<ExpressionContext> &pCtx)
        : DocumentSource(pCtx)
        , _docsAddedToBatches(0)
        , _ns(ns)
        , _cursorId(cursorId)
    {}

    intrusive_ptr<DocumentSourceCursor> DocumentSourceCursor::create(
            const string& ns,
            CursorId cursorId,
            const intrusive_ptr<ExpressionContext> &pExpCtx) {
        return new DocumentSourceCursor(ns, cursorId, pExpCtx);
    }

    void DocumentSourceCursor::setProjection(const BSONObj& projection, const ParsedDeps& deps) {
        _projection = projection;
        _dependencies = deps;
    }
}
