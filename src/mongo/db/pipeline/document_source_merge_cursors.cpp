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


namespace mongo {

using boost::intrusive_ptr;
using std::make_pair;
using std::string;
using std::vector;

DocumentSourceMergeCursors::DocumentSourceMergeCursors(
    std::vector<CursorDescriptor> cursorDescriptors,
    const intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSource(pExpCtx), _cursorDescriptors(std::move(cursorDescriptors)), _unstarted(true) {}

REGISTER_DOCUMENT_SOURCE(mergeCursors, DocumentSourceMergeCursors::createFromBson);

const char* DocumentSourceMergeCursors::getSourceName() const {
    return "$mergeCursors";
}

intrusive_ptr<DocumentSource> DocumentSourceMergeCursors::create(
    std::vector<CursorDescriptor> cursorDescriptors,
    const intrusive_ptr<ExpressionContext>& pExpCtx) {
    intrusive_ptr<DocumentSourceMergeCursors> source(
        new DocumentSourceMergeCursors(std::move(cursorDescriptors), pExpCtx));
    source->injectExpressionContext(pExpCtx);
    return source;
}

intrusive_ptr<DocumentSource> DocumentSourceMergeCursors::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    massert(17026,
            string("Expected an Array, but got a ") + typeName(elem.type()),
            elem.type() == Array);

    std::vector<CursorDescriptor> cursorDescriptors;
    BSONObj array = elem.embeddedObject();
    BSONForEach(cursor, array) {
        massert(17027,
                string("Expected an Object, but got a ") + typeName(cursor.type()),
                cursor.type() == Object);

        // The cursor descriptors for the merge cursors stage used to lack an "ns" field; "ns" was
        // understood to be the expression context namespace in that case. For mixed-version
        // compatibility, we accept both the old and new formats here.
        std::string cursorNs = cursor["ns"] ? cursor["ns"].String() : pExpCtx->ns.ns();

        cursorDescriptors.emplace_back(ConnectionString(HostAndPort(cursor["host"].String())),
                                       std::move(cursorNs),
                                       cursor["id"].Long());
    }

    return new DocumentSourceMergeCursors(std::move(cursorDescriptors), pExpCtx);
}

Value DocumentSourceMergeCursors::serialize(bool explain) const {
    vector<Value> cursors;
    for (size_t i = 0; i < _cursorDescriptors.size(); i++) {
        cursors.push_back(
            Value(DOC("host" << Value(_cursorDescriptors[i].connectionString.toString()) << "ns"
                             << _cursorDescriptors[i].ns
                             << "id"
                             << _cursorDescriptors[i].cursorId)));
    }
    return Value(DOC(getSourceName() << Value(cursors)));
}

DocumentSourceMergeCursors::CursorAndConnection::CursorAndConnection(
    const CursorDescriptor& cursorDescriptor)
    : connection(cursorDescriptor.connectionString),
      cursor(connection.get(), cursorDescriptor.ns, cursorDescriptor.cursorId, 0, 0) {}

vector<DBClientCursor*> DocumentSourceMergeCursors::getCursors() {
    verify(_unstarted);
    start();
    vector<DBClientCursor*> out;
    for (Cursors::const_iterator it = _cursors.begin(); it != _cursors.end(); ++it) {
        out.push_back(&((*it)->cursor));
    }

    return out;
}

void DocumentSourceMergeCursors::start() {
    _unstarted = false;

    // open each cursor and send message asking for a batch
    for (auto&& cursorDescriptor : _cursorDescriptors) {
        _cursors.push_back(std::make_shared<CursorAndConnection>(cursorDescriptor));
        verify(_cursors.back()->connection->lazySupported());
        _cursors.back()->cursor.initLazy();  // shouldn't block
    }

    // wait for all cursors to return a batch
    // TODO need a way to keep cursors alive if some take longer than 10 minutes.
    for (auto&& cursor : _cursors) {
        bool retry = false;
        bool ok = cursor->cursor.initLazyFinish(retry);  // blocks here for first batch

        uassert(
            17028, "error reading response from " + _cursors.back()->connection->toString(), ok);
        verify(!retry);
    }

    _currentCursor = _cursors.begin();
}

Document DocumentSourceMergeCursors::nextSafeFrom(DBClientCursor* cursor) {
    const BSONObj next = cursor->next();
    if (next.hasField("$err")) {
        const int code = next.hasField("code") ? next["code"].numberInt() : 17029;
        uasserted(code,
                  str::stream() << "Received error in response from " << cursor->originalHost()
                                << ": "
                                << next);
    }
    return Document::fromBsonWithMetaData(next);
}

boost::optional<Document> DocumentSourceMergeCursors::getNext() {
    if (_unstarted)
        start();

    // purge eof cursors and release their connections
    while (!_cursors.empty() && !(*_currentCursor)->cursor.more()) {
        (*_currentCursor)->connection.done();
        _cursors.erase(_currentCursor);
        _currentCursor = _cursors.begin();
    }

    if (_cursors.empty())
        return boost::none;

    const Document next = nextSafeFrom(&((*_currentCursor)->cursor));

    // advance _currentCursor, wrapping if needed
    if (++_currentCursor == _cursors.end())
        _currentCursor = _cursors.begin();

    return next;
}

void DocumentSourceMergeCursors::dispose() {
    // Note it is an error to call done() on a connection before consuming the response from a
    // request. Therefore it is an error to call dispose() if there are any outstanding connections
    // which have not received a reply.
    for (auto&& cursorAndConn : _cursors) {
        cursorAndConn->cursor.kill();
        cursorAndConn->connection.done();
    }
    _cursors.clear();
    _currentCursor = _cursors.end();
}
}
