/** @file oplogreader.h */

/**
*    Copyright (C) 2012 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/


#pragma once

#include "mongo/client/dbclientcursor.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace repl {

/**
 * Authenticates conn using the server's cluster-membership credentials.
 *
 * Returns true on successful authentication.
 */
bool replAuthenticate(DBClientBase* conn);

/* started abstracting out the querying of the primary/master's oplog
   still fairly awkward but a start.
*/

class OplogReader {
private:
    std::shared_ptr<DBClientConnection> _conn;
    std::shared_ptr<DBClientCursor> cursor;
    int _tailingQueryOptions;

    // If _conn was actively connected, _host represents the current HostAndPort of the
    // connection.
    HostAndPort _host;

public:
    OplogReader();
    ~OplogReader() {}
    void resetCursor() {
        cursor.reset();
    }
    void resetConnection() {
        cursor.reset();
        _conn.reset();
        _host = HostAndPort();
    }
    DBClientConnection* conn() {
        return _conn.get();
    }
    BSONObj findOne(const char* ns, const Query& q) {
        return conn()->findOne(ns, q, 0, QueryOption_SlaveOk);
    }
    BSONObj findOneByUUID(const std::string& db, UUID uuid, const BSONObj& filter) {
        // Note that the findOneByUUID() function of DBClient passes SlaveOK to the client.
        BSONObj foundDoc;
        std::tie(foundDoc, std::ignore) = conn()->findOneByUUID(db, uuid, filter);
        return foundDoc;
    }

    /* SO_TIMEOUT (send/recv time out) for our DBClientConnections */
    static const Seconds kSocketTimeout;

    /* ok to call if already connected */
    bool connect(const HostAndPort& host);

    void tailCheck();

    bool haveCursor() {
        return cursor.get() != 0;
    }

    void tailingQuery(const char* ns, const BSONObj& query);

    bool more() {
        uassert(15910, "Doesn't have cursor for reading oplog", cursor.get());
        return cursor->more();
    }

    bool moreInCurrentBatch() {
        uassert(15911, "Doesn't have cursor for reading oplog", cursor.get());
        return cursor->moreInCurrentBatch();
    }

    BSONObj nextSafe() {
        return cursor->nextSafe();
    }

    // master/slave only
    void peek(std::vector<BSONObj>& v, int n) {
        if (cursor.get())
            cursor->peek(v, n);
    }

    // master/slave only
    void putBack(BSONObj op) {
        cursor->putBack(op);
    }
};

}  // namespace repl
}  // namespace mongo
