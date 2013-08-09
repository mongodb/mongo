/**
 * Copyright 2013 (c) 10gen Inc.
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
 */

#include "mongo/pch.h"

#include "mongo/db/pipeline/document_source.h"

#include "mongo/db/cmdline.h"

namespace mongo {

    const char DocumentSourceMergeCursors::name[] = "$mergeCursors";

    const char* DocumentSourceMergeCursors::getSourceName() const {
        return name;
    }

    bool DocumentSourceMergeCursors::eof() {
        /* if we haven't even started yet, do so */
        if (_unstarted)
            getNextDocument();

        return !_hasCurrent;
    }

    bool DocumentSourceMergeCursors::advance() {
        DocumentSource::advance(); // check for interrupts

        if (_unstarted)
            getNextDocument(); // skip first

        /* advance */
        getNextDocument();

        return _hasCurrent;
    }

    Document DocumentSourceMergeCursors::getCurrent() {
        verify(!eof());
        return _current;
    }

    void DocumentSourceMergeCursors::setSource(DocumentSource *pSource) {
        /* this doesn't take a source */
        verify(false);
    }

    DocumentSourceMergeCursors::DocumentSourceMergeCursors(
            const CursorIds& cursorIds,
            const intrusive_ptr<ExpressionContext> &pExpCtx)
        : DocumentSource(pExpCtx)
        , _cursorIds(cursorIds)
        , _unstarted(true)
        , _hasCurrent(false)
    {}

    intrusive_ptr<DocumentSource> DocumentSourceMergeCursors::create(
            const CursorIds& cursorIds,
            const intrusive_ptr<ExpressionContext> &pExpCtx) {
        return new DocumentSourceMergeCursors(cursorIds, pExpCtx);
    }

    intrusive_ptr<DocumentSource> DocumentSourceMergeCursors::createFromBson(
            BSONElement* pBsonElement,
            const intrusive_ptr<ExpressionContext>& pExpCtx) {

        massert(17026, string("Expected an Array, but got a ") + typeName(pBsonElement->type()),
                pBsonElement->type() == Array);

        CursorIds cursorIds;
        BSONObj array = pBsonElement->embeddedObject();
        BSONForEach(cursor, array) {
            massert(17027, string("Expected an Object, but got a ") + typeName(cursor.type()),
                    cursor.type() == Object);

            cursorIds.push_back(make_pair(ConnectionString(cursor["host"].String()),
                                          cursor["id"].Long()));
        }
        
        return new DocumentSourceMergeCursors(cursorIds, pExpCtx);
    }

    void DocumentSourceMergeCursors::sourceToBson(BSONObjBuilder* builder, bool explain) const {
        BSONArrayBuilder cursors(builder->subarrayStart("$mergeCursors"));
        for (size_t i = 0; i < _cursorIds.size(); i++) {
            cursors.append(BSON("host" << _cursorIds[i].first.toString()
                             << "id" << _cursorIds[i].second));
        }
    }

    DocumentSourceMergeCursors::CursorAndConnection::CursorAndConnection(
            ConnectionString host,
            NamespaceString ns,
            CursorId id)
        : connection(host)
        , cursor(connection.get(), ns, id, 0, 0)
    {}

    void DocumentSourceMergeCursors::getNextDocument() {
        if (boost::optional<Document> doc = getNextDocumentImpl()) {
            _current = *doc;
            _hasCurrent = true;
        }
        else {
            _hasCurrent = false;
        }
    }

    boost::optional<Document> DocumentSourceMergeCursors::getNextDocumentImpl() {
        if (_unstarted) {
            _unstarted = false;

            // open each cursor and send message asking for a batch
            for (CursorIds::const_iterator it = _cursorIds.begin(); it !=_cursorIds.end(); ++it) {
                _cursors.push_back(boost::make_shared<CursorAndConnection>(
                            it->first, pExpCtx->getNs(), it->second));
                verify(_cursors.back()->connection->lazySupported());
                _cursors.back()->cursor.initLazy(); // shouldn't block
            }

            // wait for all cursors to return a batch
            // TODO need a way to keep cursors alive if some take longer than 10 minutes.
            for (Cursors::const_iterator it = _cursors.begin(); it !=_cursors.end(); ++it) {
                bool retry = false;
                bool ok = (*it)->cursor.initLazyFinish(retry); // blocks here for first batch

                uassert(17028,
                        "error reading response from " + _cursors.back()->connection->toString(),
                        ok);
                verify(!retry);
            }

            _currentCursor = _cursors.begin();
        }

        // purge eof cursors and release their connections
        while (!_cursors.empty() && !(*_currentCursor)->cursor.more()) {
            (*_currentCursor)->connection.done();
            _cursors.erase(_currentCursor);
            _currentCursor = _cursors.begin();
        }

        if (_cursors.empty())
            return boost::none;

        BSONObj next = (*_currentCursor)->cursor.next();
        uassert(17029, str::stream() << "Received error in response from "
                                     << (*_currentCursor)->connection->toString()
                                     << ": " << next,
                !next.hasField("$err"));

        // advance _currentCursor, wrapping if needed
        if (++_currentCursor == _cursors.end())
            _currentCursor = _cursors.begin();

        return Document(next);
    }

    void DocumentSourceMergeCursors::dispose() {
        _cursors.clear();
        _currentCursor = _cursors.end();
        _hasCurrent = false;
    }
}
