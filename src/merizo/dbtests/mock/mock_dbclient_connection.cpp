/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#include "merizo/platform/basic.h"

#include "merizo/dbtests/mock/mock_dbclient_connection.h"

#include "merizo/client/dbclient_mockcursor.h"
#include "merizo/util/net/socket_exception.h"
#include "merizo/util/time_support.h"

using merizo::BSONObj;

using std::string;
using std::vector;

namespace merizo {
MockDBClientConnection::MockDBClientConnection(MockRemoteDBServer* remoteServer, bool autoReconnect)
    : _remoteServerInstanceID(remoteServer->getInstanceID()),
      _remoteServer(remoteServer),
      _isFailed(false),
      _sockCreationTime(merizo::curTimeMicros64()),
      _autoReconnect(autoReconnect) {}

MockDBClientConnection::~MockDBClientConnection() {}

bool MockDBClientConnection::connect(const char* hostName,
                                     StringData applicationName,
                                     std::string& errmsg) {
    if (_remoteServer->isRunning()) {
        _remoteServerInstanceID = _remoteServer->getInstanceID();
        return true;
    }

    errmsg.assign("cannot connect to " + _remoteServer->getServerAddress());
    return false;
}

std::pair<rpc::UniqueReply, DBClientBase*> MockDBClientConnection::runCommandWithTarget(
    OpMsgRequest request) {

    checkConnection();

    try {
        return {_remoteServer->runCommand(_remoteServerInstanceID, request), this};
    } catch (const merizo::DBException&) {
        _isFailed = true;
        throw;
    }
}


std::unique_ptr<merizo::DBClientCursor> MockDBClientConnection::query(
    const NamespaceStringOrUUID& nsOrUuid,
    merizo::Query query,
    int nToReturn,
    int nToSkip,
    const BSONObj* fieldsToReturn,
    int queryOptions,
    int batchSize) {
    checkConnection();

    try {
        merizo::BSONArray result(_remoteServer->query(_remoteServerInstanceID,
                                                     nsOrUuid,
                                                     query,
                                                     nToReturn,
                                                     nToSkip,
                                                     fieldsToReturn,
                                                     queryOptions,
                                                     batchSize));

        std::unique_ptr<merizo::DBClientCursor> cursor;
        cursor.reset(new DBClientMockCursor(this, BSONArray(result.copy()), batchSize));
        return cursor;
    } catch (const merizo::DBException&) {
        _isFailed = true;
        throw;
    }

    std::unique_ptr<merizo::DBClientCursor> nullPtr;
    return nullPtr;
}

merizo::ConnectionString::ConnectionType MockDBClientConnection::type() const {
    return merizo::ConnectionString::CUSTOM;
}

bool MockDBClientConnection::isFailed() const {
    return _isFailed;
}

string MockDBClientConnection::getServerAddress() const {
    return _remoteServer->getServerAddress();
}

string MockDBClientConnection::toString() const {
    return _remoteServer->toString();
}

unsigned long long MockDBClientConnection::query(
    stdx::function<void(merizo::DBClientCursorBatchIterator&)> f,
    const NamespaceStringOrUUID& nsOrUuid,
    merizo::Query query,
    const merizo::BSONObj* fieldsToReturn,
    int queryOptions,
    int batchSize) {
    return DBClientBase::query(f, nsOrUuid, query, fieldsToReturn, queryOptions, batchSize);
}

uint64_t MockDBClientConnection::getSockCreationMicroSec() const {
    return _sockCreationTime;
}

void MockDBClientConnection::insert(const string& ns, BSONObj obj, int flags) {
    _remoteServer->insert(ns, obj, flags);
}

void MockDBClientConnection::insert(const string& ns, const vector<BSONObj>& objList, int flags) {
    for (vector<BSONObj>::const_iterator iter = objList.begin(); iter != objList.end(); ++iter) {
        insert(ns, *iter, flags);
    }
}

void MockDBClientConnection::remove(const string& ns, Query query, int flags) {
    _remoteServer->remove(ns, query, flags);
}

void MockDBClientConnection::killCursor(const NamespaceString& ns, long long cursorID) {
    verify(false);  // unimplemented
}

bool MockDBClientConnection::call(merizo::Message& toSend,
                                  merizo::Message& response,
                                  bool assertOk,
                                  string* actualServer) {
    verify(false);  // unimplemented
    return false;
}

void MockDBClientConnection::say(merizo::Message& toSend, bool isRetry, string* actualServer) {
    verify(false);  // unimplemented
}

bool MockDBClientConnection::lazySupported() const {
    verify(false);  // unimplemented
    return false;
}

double MockDBClientConnection::getSoTimeout() const {
    return 0;
}

void MockDBClientConnection::checkConnection() {
    if (_isFailed && _autoReconnect) {
        _remoteServerInstanceID = _remoteServer->getInstanceID();
    }
}
}
