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

#include "mongo/platform/basic.h"

#include "mongo/dbtests/mock/mock_dbclient_connection.h"

#include "mongo/client/dbclient_mockcursor.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/time_support.h"

using mongo::BSONObj;

using std::string;
using std::vector;

namespace mongo {
MockDBClientConnection::MockDBClientConnection()
    : _remoteServer(nullptr),
      _isFailed(false),
      _sockCreationTime(mongo::curTimeMicros64()),
      _autoReconnect(false) {
    _setServerRPCProtocols(rpc::supports::kAll);
    _callIter = _mockCallResponses.begin();
    _recvIter = _mockRecvResponses.begin();
}

MockDBClientConnection::MockDBClientConnection(MockRemoteDBServer* remoteServer, bool autoReconnect)
    : _remoteServerInstanceID(remoteServer->getInstanceID()),
      _remoteServer(remoteServer),
      _isFailed(false),
      _sockCreationTime(mongo::curTimeMicros64()),
      _autoReconnect(autoReconnect) {
    _callIter = _mockCallResponses.begin();
    _recvIter = _mockRecvResponses.begin();
}

MockDBClientConnection::~MockDBClientConnection() {}

bool MockDBClientConnection::connect(const char* hostName,
                                     StringData applicationName,
                                     std::string& errmsg) {
    invariant(_remoteServer);
    if (_remoteServer->isRunning()) {
        _remoteServerInstanceID = _remoteServer->getInstanceID();
        _setServerRPCProtocols(rpc::supports::kAll);
        return true;
    }

    errmsg.assign("cannot connect to " + _remoteServer->getServerAddress());
    return false;
}

std::pair<rpc::UniqueReply, DBClientBase*> MockDBClientConnection::runCommandWithTarget(
    OpMsgRequest request) {

    checkConnection();

    try {
        _lastCursorMessage = boost::none;
        invariant(_remoteServer);
        auto reply = _remoteServer->runCommand(_remoteServerInstanceID, request);
        auto status = getStatusFromCommandResult(reply->getCommandReply());
        // The real DBClientBase always throws HostUnreachable on network error, so we do the
        // same here.
        uassert(ErrorCodes::HostUnreachable,
                str::stream() << "network error while attempting to run "
                              << "command '" << request.getCommandName() << "' " << status,
                !ErrorCodes::isNetworkError(status));
        auto cursorRes = CursorResponse::parseFromBSON(reply->getCommandReply());
        if (cursorRes.isOK() && cursorRes.getValue().getCursorId() != 0) {
            _lastCursorMessage = request;
        }
        return {std::move(reply), this};
    } catch (const mongo::DBException&) {
        _isFailed = true;
        throw;
    }
}  // namespace mongo


std::unique_ptr<mongo::DBClientCursor> MockDBClientConnection::query(
    const NamespaceStringOrUUID& nsOrUuid,
    mongo::Query query,
    int nToReturn,
    int nToSkip,
    const BSONObj* fieldsToReturn,
    int queryOptions,
    int batchSize) {
    checkConnection();

    try {
        invariant(_remoteServer);
        mongo::BSONArray result(_remoteServer->query(_remoteServerInstanceID,
                                                     nsOrUuid,
                                                     query,
                                                     nToReturn,
                                                     nToSkip,
                                                     fieldsToReturn,
                                                     queryOptions,
                                                     batchSize));

        BSONArray resultsInCursor;

        // A simple mock implementation of a resumable query, where we skip the first 'n' fields
        // where 'n' is given by the mock resume token.
        auto nToSkip = 0;
        auto queryBson = fromjson(query.toString());
        if (queryBson.hasField("$_resumeAfter")) {
            if (queryBson["$_resumeAfter"].Obj().hasField("n")) {
                nToSkip = queryBson["$_resumeAfter"]["n"].numberInt();
            }
        }

        bool provideResumeToken = false;
        if (queryBson.hasField("$_requestResumeToken")) {
            provideResumeToken = true;
        }

        // Resume query.
        if (nToSkip != 0) {
            BSONObjIterator iter(result);
            BSONArrayBuilder builder;
            auto numExamined = 0;

            while (iter.more()) {
                numExamined++;

                if (numExamined < nToSkip + 1) {
                    iter.next();
                    continue;
                }

                builder.append(iter.next().Obj());
            }
            resultsInCursor = BSONArray(builder.obj());
        } else {
            // Yield all results instead (default).
            resultsInCursor = BSONArray(result.copy());
        }

        std::unique_ptr<mongo::DBClientCursor> cursor;
        cursor.reset(new DBClientMockCursor(
            this, BSONArray(resultsInCursor), provideResumeToken, batchSize));
        return cursor;
    } catch (const mongo::DBException&) {
        _isFailed = true;
        throw;
    }

    std::unique_ptr<mongo::DBClientCursor> nullPtr;
    return nullPtr;
}

mongo::ConnectionString::ConnectionType MockDBClientConnection::type() const {
    return mongo::ConnectionString::CUSTOM;
}

bool MockDBClientConnection::isFailed() const {
    return _isFailed;
}

string MockDBClientConnection::getServerAddress() const {
    return _remoteServer ? _remoteServer->getServerAddress() : "localhost:27017";
}

string MockDBClientConnection::toString() const {
    return _remoteServer ? _remoteServer->toString() : "localhost:27017";
}

unsigned long long MockDBClientConnection::query(
    std::function<void(mongo::DBClientCursorBatchIterator&)> f,
    const NamespaceStringOrUUID& nsOrUuid,
    mongo::Query query,
    const mongo::BSONObj* fieldsToReturn,
    int queryOptions,
    int batchSize) {
    return DBClientBase::query(f, nsOrUuid, query, fieldsToReturn, queryOptions, batchSize);
}

uint64_t MockDBClientConnection::getSockCreationMicroSec() const {
    return _sockCreationTime;
}

void MockDBClientConnection::insert(const string& ns, BSONObj obj, int flags) {
    invariant(_remoteServer);
    _remoteServer->insert(ns, obj, flags);
}

void MockDBClientConnection::insert(const string& ns, const vector<BSONObj>& objList, int flags) {
    for (vector<BSONObj>::const_iterator iter = objList.begin(); iter != objList.end(); ++iter) {
        insert(ns, *iter, flags);
    }
}

void MockDBClientConnection::remove(const string& ns, Query query, int flags) {
    invariant(_remoteServer);
    _remoteServer->remove(ns, query, flags);
}

void MockDBClientConnection::killCursor(const NamespaceString& ns, long long cursorID) {
    // Unimplemented if there is a remote server. Without a remote server, there is nothing that
    // needs to be done.
    invariant(!_remoteServer);
}

bool MockDBClientConnection::call(mongo::Message& toSend,
                                  mongo::Message& response,
                                  bool assertOk,
                                  string* actualServer) {
    // Here we check for a getMore command, and if it is that, we respond with the next
    // reply message from the previous command that returned a cursor response.
    // This allows us to mock commands with implicit cursors (e.g. listCollections).
    // It is not used for query() calls.
    if (_lastCursorMessage && !toSend.empty() && toSend.operation() == dbMsg) {
        // This might be a getMore.
        OpMsg parsedMsg;
        try {
            parsedMsg = OpMsg::parse(toSend);
        } catch (...) {
            // Any exceptions in parsing fall through to unsupported case.
        }
        if (!parsedMsg.body.isEmpty() &&
            parsedMsg.body.firstElement().fieldName() == "getMore"_sd) {
            auto reply = runCommandWithTarget(*_lastCursorMessage).first;
            response = reply.releaseMessage();
            return true;
        }
    }
    stdx::unique_lock lk(_netMutex);

    _lastSentMessage = toSend;
    _mockCallResponsesCV.wait(lk, [&] {
        _blockedOnNetwork = (_callIter == _mockCallResponses.end());
        return !_blockedOnNetwork || !isStillConnected();
    });

    uassert(ErrorCodes::HostUnreachable, "Socket was shut down while in call", isStillConnected());

    const auto& swResponse = *_callIter;
    _callIter++;
    response = uassertStatusOK(swResponse);
    return true;
}

Status MockDBClientConnection::recv(mongo::Message& m, int lastRequestId) {
    stdx::unique_lock lk(_netMutex);

    _mockRecvResponsesCV.wait(lk, [&] {
        _blockedOnNetwork = (_recvIter == _mockRecvResponses.end());
        return !_blockedOnNetwork || !isStillConnected();
    });

    uassert(ErrorCodes::HostUnreachable, "Socket was shut down while in recv", isStillConnected());

    const auto& swResponse = *_recvIter;
    _recvIter++;
    m = uassertStatusOK(swResponse);
    return Status::OK();
}

void MockDBClientConnection::shutdownAndDisallowReconnect() {
    DBClientConnection::shutdownAndDisallowReconnect();
    _mockCallResponsesCV.notify_all();
    _mockRecvResponsesCV.notify_all();
}

void MockDBClientConnection::setCallResponses(Responses responses) {
    stdx::lock_guard lk(_netMutex);
    _mockCallResponses = std::move(responses);
    _callIter = _mockCallResponses.begin();
    if (_blockedOnNetwork && !_mockCallResponses.empty()) {
        _blockedOnNetwork = false;
        _mockCallResponsesCV.notify_all();
    }
}

void MockDBClientConnection::setRecvResponses(Responses responses) {
    stdx::lock_guard lk(_netMutex);
    _mockRecvResponses = std::move(responses);
    _recvIter = _mockRecvResponses.begin();
    if (_blockedOnNetwork && !_mockRecvResponses.empty()) {
        _blockedOnNetwork = false;
        _mockRecvResponsesCV.notify_all();
    }
}

void MockDBClientConnection::say(mongo::Message& toSend, bool isRetry, string* actualServer) {
    invariant(false);  // unimplemented
}

bool MockDBClientConnection::lazySupported() const {
    invariant(false);  // unimplemented
    return false;
}

void MockDBClientConnection::checkConnection() {
    if (_isFailed && _autoReconnect && _remoteServer->isRunning()) {
        _remoteServerInstanceID = _remoteServer->getInstanceID();
        _isFailed = false;
    }
}
}  // namespace mongo
