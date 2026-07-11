// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/client/dbclient_mockcursor.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/tenant_id.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/platform/atomic.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <mutex>
#include <string_view>

using mongo::BSONObj;

using std::string;
using std::vector;

namespace mongo {
using namespace std::literals::string_view_literals;
MockDBClientConnection::MockDBClientConnection(MockRemoteDBServer* remoteServer, bool autoReconnect)
    : DBClientConnection(autoReconnect),
      _remoteServer(remoteServer),
      _sockCreationTime(mongo::curTimeMicros64()) {
    invariant(remoteServer);
    _remoteServerInstanceID = remoteServer->getInstanceID();
    _callIter = _mockCallResponses.begin();
    _recvIter = _mockRecvResponses.begin();
}

MockDBClientConnection::~MockDBClientConnection() {}

bool MockDBClientConnection::connect(const char* hostName,
                                     std::string_view applicationName,
                                     std::string& errmsg) {
    _serverAddress = _remoteServer->getServerHostAndPort();
    if (_remoteServer->isRunning()) {
        _remoteServerInstanceID = _remoteServer->getInstanceID();
        return true;
    }

    _failed.store(true);
    errmsg.assign("cannot connect to " + _remoteServer->getServerAddress());
    return false;
}

std::pair<rpc::UniqueReply, DBClientBase*> MockDBClientConnection::runCommandWithTarget(
    OpMsgRequest request) {

    ensureConnection();

    try {
        _lastCursorMessage = boost::none;
        auto reply = _remoteServer->runCommand(_remoteServerInstanceID, request);
        auto status = getStatusFromCommandResult(reply->getCommandReply());
        // The real DBClientBase always throws HostUnreachable on network error, so we do the
        // same here.
        uassert(ErrorCodes::HostUnreachable,
                str::stream() << "network error while attempting to run "
                              << "command '" << request.getCommandName() << "' " << status,
                !ErrorCodes::isNetworkError(status));

        auto cursorRes = CursorResponse::parseFromBSON(
            reply->getCommandReply(), nullptr, request.getValidatedTenantId());
        if (cursorRes.isOK() && cursorRes.getValue().getCursorId() != 0) {
            _lastCursorMessage = request;
        }
        return {std::move(reply), this};
    } catch (const mongo::DBException&) {
        _failed.store(true);
        throw;
    }
}

namespace {
int nToSkipFromResumeAfter(const BSONObj& resumeAfter) {
    if (resumeAfter.isEmpty()) {
        return 0;
    }

    auto nElt = resumeAfter["n"];
    if (!nElt || !nElt.isNumber()) {
        return 0;
    }

    return nElt.numberInt();
}
}  // namespace

std::unique_ptr<DBClientCursor> MockDBClientConnection::bsonArrayToCursor(BSONArray results,
                                                                          int nToSkip,
                                                                          bool provideResumeToken,
                                                                          int batchSize) {
    BSONArray resultsInCursor;

    // Resume query.
    if (nToSkip != 0) {
        BSONObjIterator iter(results);
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
        resultsInCursor = BSONArray(results.copy());
    }

    return std::make_unique<DBClientMockCursor>(
        this, resultsInCursor, provideResumeToken, batchSize);
}

std::unique_ptr<DBClientCursor> MockDBClientConnection::find(
    FindCommandRequest findRequest,
    const ReadPreferenceSetting& /*unused*/,
    ExhaustMode /*unused*/) {
    ensureConnection();
    try {
        int nToSkip = nToSkipFromResumeAfter(findRequest.getResumeAfter());
        bool provideResumeToken = findRequest.getRequestResumeToken();
        int batchSize = findRequest.getBatchSize().value_or(0);
        BSONArray results = _remoteServer->find(_remoteServerInstanceID, findRequest);
        return bsonArrayToCursor(std::move(results), nToSkip, provideResumeToken, batchSize);
    } catch (const DBException&) {
        _failed.store(true);
        throw;
    }
    return nullptr;
}

mongo::ConnectionString::ConnectionType MockDBClientConnection::type() const {
    return mongo::ConnectionString::ConnectionType::kCustom;
}

uint64_t MockDBClientConnection::getSockCreationMicroSec() const {
    return _sockCreationTime;
}

void MockDBClientConnection::insert(const NamespaceString& nss,
                                    BSONObj obj,
                                    bool ordered,
                                    boost::optional<BSONObj> writeConcernObj) {
    _remoteServer->insert(nss, obj);
}

void MockDBClientConnection::insert(const NamespaceString& nss,
                                    const vector<BSONObj>& objList,
                                    bool ordered,
                                    boost::optional<BSONObj> writeConcernObj) {
    for (vector<BSONObj>::const_iterator iter = objList.begin(); iter != objList.end(); ++iter) {
        insert(nss, *iter, ordered);
    }
}

void MockDBClientConnection::remove(const NamespaceString& nss,
                                    const BSONObj& filter,
                                    bool removeMany,
                                    boost::optional<BSONObj> writeConcernObj) {
    _remoteServer->remove(nss, filter);
}

void MockDBClientConnection::killCursor(const NamespaceString& ns, long long cursorID) {
    // It is not worth the bother of killing the cursor in the mock.
}

Message MockDBClientConnection::_call(Message& toSend, string* actualServer) {
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
        if (!parsedMsg.body.isEmpty() && parsedMsg.body.firstElement().fieldName() == "getMore"sv) {
            auto reply = runCommandWithTarget(*_lastCursorMessage).first;
            return reply.releaseMessage();
        }
    }

    ScopeGuard killSessionOnDisconnect([this] { shutdown(); });

    std::unique_lock lk(_netMutex);
    ensureConnection();
    if (!isStillConnected() || !_remoteServer->isRunning()) {
        uasserted(ErrorCodes::SocketException, "Broken pipe in call");
    }

    _lastSentMessage = toSend;
    _mockCallResponsesCV.wait(lk, [&] {
        _blockedOnNetwork = (_callIter == _mockCallResponses.end());
        return !_blockedOnNetwork || !isStillConnected() || !_remoteServer->isRunning();
    });

    uassert(ErrorCodes::HostUnreachable,
            "Socket was shut down while in call",
            isStillConnected() && _remoteServer->isRunning());

    killSessionOnDisconnect.dismiss();

    const auto& swResponse = *_callIter;
    _callIter++;
    return uassertStatusOK(swResponse);
}

Message MockDBClientConnection::recv(int lastRequestId) {
    ScopeGuard killSessionOnDisconnect([this] { shutdown(); });

    std::unique_lock lk(_netMutex);
    uassert(ErrorCodes::SocketException,
            "Broken pipe in recv",
            isStillConnected() && _remoteServer->isRunning());

    _mockRecvResponsesCV.wait(lk, [&] {
        _blockedOnNetwork = (_recvIter == _mockRecvResponses.end());
        return !_blockedOnNetwork || !isStillConnected() || !_remoteServer->isRunning();
    });

    uassert(ErrorCodes::HostUnreachable,
            "Socket was shut down while in recv",
            isStillConnected() && _remoteServer->isRunning());

    killSessionOnDisconnect.dismiss();

    const auto& swResponse = *_recvIter;
    _recvIter++;
    return uassertStatusOK(swResponse);
}

void MockDBClientConnection::shutdown() {
    std::lock_guard lk(_netMutex);
    DBClientConnection::shutdown();
    _mockCallResponsesCV.notify_all();
    _mockRecvResponsesCV.notify_all();
}

void MockDBClientConnection::shutdownAndDisallowReconnect() {
    std::lock_guard lk(_netMutex);
    DBClientConnection::shutdownAndDisallowReconnect();
    _mockCallResponsesCV.notify_all();
    _mockRecvResponsesCV.notify_all();
}

void MockDBClientConnection::setCallResponses(Responses responses) {
    std::lock_guard lk(_netMutex);
    _mockCallResponses = std::move(responses);
    _callIter = _mockCallResponses.begin();
    if (_blockedOnNetwork && !_mockCallResponses.empty()) {
        _blockedOnNetwork = false;
        _mockCallResponsesCV.notify_all();
    }
}

void MockDBClientConnection::setRecvResponses(Responses responses) {
    std::lock_guard lk(_netMutex);
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

void MockDBClientConnection::ensureConnection() {
    if (_failed.load()) {
        uassert(ErrorCodes::SocketException, toString(), _autoReconnect);
        uassert(ErrorCodes::HostUnreachable,
                "cannot connect to " + _remoteServer->getServerAddress(),
                _remoteServer->isRunning());
        _remoteServerInstanceID = _remoteServer->getInstanceID();
        _failed.store(false);
    }
}
}  // namespace mongo
