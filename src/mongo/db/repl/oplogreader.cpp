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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/oplogreader.h"

#include <string>

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/internal_user_auth.h"
#include "mongo/executor/network_interface.h"
#include "mongo/util/log.h"

namespace mongo {

using std::shared_ptr;
using std::endl;
using std::string;

namespace repl {

const BSONObj reverseNaturalObj = BSON("$natural" << -1);

bool replAuthenticate(DBClientBase* conn) {
    if (isInternalAuthSet())
        return conn->authenticateInternalUser();
    if (getGlobalAuthorizationManager()->isAuthEnabled())
        return false;
    return true;
}

const Seconds OplogReader::kSocketTimeout(30);

OplogReader::OplogReader() {
    _tailingQueryOptions = QueryOption_SlaveOk;
    _tailingQueryOptions |= QueryOption_CursorTailable | QueryOption_OplogReplay;

    /* TODO: slaveOk maybe shouldn't use? */
    _tailingQueryOptions |= QueryOption_AwaitData;

    // Currently find command doesn't do the cursor tracking that master-slave relies on.
    _tailingQueryOptions |= DBClientCursor::QueryOptionLocal_forceOpQuery;
}

bool OplogReader::connect(const HostAndPort& host) {
    if (conn() == NULL || _host != host) {
        resetConnection();
        _conn = shared_ptr<DBClientConnection>(
            new DBClientConnection(false, durationCount<Seconds>(kSocketTimeout)));
        string errmsg;
        if (!_conn->connect(host, StringData(), errmsg) || !replAuthenticate(_conn.get())) {
            resetConnection();
            error() << errmsg << endl;
            return false;
        }
        _conn->port().setTag(_conn->port().getTag() |
                             executor::NetworkInterface::kMessagingPortKeepOpen);
        _host = host;
    }
    return true;
}

void OplogReader::tailCheck() {
    if (cursor.get() && cursor->isDead()) {
        log() << "old cursor isDead, will initiate a new one" << std::endl;
        resetCursor();
    }
}

void OplogReader::tailingQuery(const char* ns, const BSONObj& query) {
    verify(!haveCursor());
    LOG(2) << ns << ".find(" << redact(query) << ')' << endl;
    cursor.reset(_conn->query(ns, query, 0, 0, nullptr, _tailingQueryOptions).release());
}

}  // namespace repl
}  // namespace mongo
