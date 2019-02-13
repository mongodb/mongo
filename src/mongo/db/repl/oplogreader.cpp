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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/oplogreader.h"

#include <string>

#include "mongo/client/authenticate.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/executor/network_interface.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {
namespace {

// Gets the singleton AuthorizationManager object for this server process
//
// TODO (SERVER-37563): Pass the service context instead of calling getGlobalServiceContext.
AuthorizationManager* getGlobalAuthorizationManager() {
    AuthorizationManager* globalAuthManager = AuthorizationManager::get(getGlobalServiceContext());
    fassert(16842, globalAuthManager != nullptr);
    return globalAuthManager;
}

}  // namespace

bool replAuthenticate(DBClientBase* conn) {
    if (auth::isInternalAuthSet())
        return conn->authenticateInternalUser().isOK();
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
}

bool OplogReader::connect(const HostAndPort& host) {
    if (conn() == NULL || _host != host) {
        resetConnection();
        _conn = std::shared_ptr<DBClientConnection>(
            new DBClientConnection(false, durationCount<Seconds>(kSocketTimeout)));

        std::string errmsg;
        if (!_conn->connect(host, StringData(), errmsg) || !replAuthenticate(_conn.get())) {
            resetConnection();
            error() << errmsg;
            return false;
        }
        _conn->setTags(transport::Session::kKeepOpen);
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
    LOG(2) << ns << ".find(" << redact(query) << ')';
    cursor.reset(
        _conn->query(NamespaceString(ns), query, 0, 0, nullptr, _tailingQueryOptions).release());
}

}  // namespace repl
}  // namespace mongo
