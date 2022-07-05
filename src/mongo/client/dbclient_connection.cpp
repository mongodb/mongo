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

/**
 * Connect to a Mongo database as a database, from C++.
 */


#include "mongo/platform/basic.h"

#include "mongo/client/dbclient_connection.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/client/sasl_client_session.h"
#include "mongo/config.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/kill_cursors_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_peer_info.h"
#include "mongo/util/password_digest.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/time_support.h"
#include "mongo/util/version.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {

using std::endl;
using std::map;
using std::string;
using std::unique_ptr;

MONGO_FAIL_POINT_DEFINE(dbClientConnectionDisableChecksum);

namespace {

StatusWith<bool> completeSpeculativeAuth(DBClientConnection* conn,
                                         auth::SpeculativeAuthType speculativeAuthType,
                                         std::shared_ptr<SaslClientSession> session,
                                         const MongoURI& uri,
                                         BSONObj isMaster) {
    auto specAuthElem = isMaster[auth::kSpeculativeAuthenticate];
    if (specAuthElem.eoo()) {
        return false;
    }

    if (speculativeAuthType == auth::SpeculativeAuthType::kNone) {
        return {ErrorCodes::BadValue,
                str::stream() << "Unexpected isMaster." << auth::kSpeculativeAuthenticate
                              << " reply"};
    }

    if (specAuthElem.type() != Object) {
        return {ErrorCodes::BadValue,
                str::stream() << "isMaster." << auth::kSpeculativeAuthenticate
                              << " reply must be an object"};
    }

    auto specAuth = specAuthElem.Obj();
    if (specAuth.isEmpty()) {
        return {ErrorCodes::BadValue,
                str::stream() << "isMaster." << auth::kSpeculativeAuthenticate
                              << " reply must be a non-empty obejct"};
    }

    if (speculativeAuthType == auth::SpeculativeAuthType::kAuthenticate) {
        return specAuth.hasField(saslCommandUserFieldName);
    }

    invariant(speculativeAuthType == auth::SpeculativeAuthType::kSaslStart);

    const auto hook = [conn](OpMsgRequest request) -> Future<BSONObj> {
        try {
            auto ret = conn->runCommand(std::move(request));
            auto status = getStatusFromCommandResult(ret->getCommandReply());
            if (!status.isOK()) {
                return status;
            }
            return ret->getCommandReply();
        } catch (const DBException& e) {
            return e.toStatus();
        }
    };

    return asyncSaslConversation(hook,
                                 session,
                                 BSON(saslContinueCommandName << 1),
                                 specAuth,
                                 uri.getAuthenticationDatabase(),
                                 kSaslClientLogLevelDefault)
        .getNoThrow()
        .isOK();
}

/**
 * Initializes the wire version of conn, and returns the isMaster reply.
 */
executor::RemoteCommandResponse initWireVersion(
    DBClientConnection* conn,
    StringData applicationName,
    const MongoURI& uri,
    std::vector<std::string>* saslMechsForAuth,
    auth::SpeculativeAuthType* speculativeAuthType,
    std::shared_ptr<SaslClientSession>* saslClientSession) try {

    BSONObjBuilder bob;
    bob.append(conn->getApiParameters().getVersion() ? "hello" : "isMaster", 1);

    if (uri.isHelloOk()) {
        // Attach "helloOk: true" to the initial handshake to indicate that the client supports the
        // hello command.
        bob.append("helloOk", true);
    }

    auto loadBalancedOpt = uri.getOption("loadBalanced");
    if (loadBalancedOpt && (loadBalancedOpt.value() == "true")) {
        bob.append("loadBalanced", true);
    }

    *speculativeAuthType = auth::speculateAuth(&bob, uri, saslClientSession);
    if (!uri.getUser().empty()) {
        UserName user(uri.getUser(), uri.getAuthenticationDatabase());
        bob.append("saslSupportedMechs", user.getUnambiguousName());
    }

    if (getTestCommandsEnabled()) {
        // Only include the host:port of this process in the isMaster command request if test
        // commands are enabled. mongobridge uses this field to identify the process opening a
        // connection to it.
        StringBuilder sb;
        sb << getHostName() << ':' << serverGlobalParams.port;
        bob.append("hostInfo", sb.str());
    }

    auto versionString = VersionInfoInterface::instance().version();

    Status serializeStatus =
        ClientMetadata::serialize("MongoDB Internal Client", versionString, applicationName, &bob);
    if (!serializeStatus.isOK()) {
        return serializeStatus;
    }

    conn->getCompressorManager().clientBegin(&bob);

    if (auto wireSpec = WireSpec::instance().get(); wireSpec->isInternalClient) {
        WireSpec::appendInternalClientWireVersion(wireSpec->outgoing, &bob);
    }

    Date_t start{Date_t::now()};
    auto result = conn->runCommand(OpMsgRequest::fromDBAndBody("admin", bob.obj()));
    Date_t finish{Date_t::now()};

    BSONObj isMasterObj = result->getCommandReply().getOwned();

    if (isMasterObj.hasField("minWireVersion") && isMasterObj.hasField("maxWireVersion")) {
        int minWireVersion = isMasterObj["minWireVersion"].numberInt();
        int maxWireVersion = isMasterObj["maxWireVersion"].numberInt();
        conn->setWireVersions(minWireVersion, maxWireVersion);
    }

    if (isMasterObj.hasField("saslSupportedMechs") &&
        isMasterObj["saslSupportedMechs"].type() == Array) {
        auto array = isMasterObj["saslSupportedMechs"].Array();
        for (const auto& elem : array) {
            saslMechsForAuth->push_back(elem.checkAndGetStringData().toString());
        }
    }

    conn->getCompressorManager().clientFinish(isMasterObj);

    return executor::RemoteCommandResponse{std::move(isMasterObj), finish - start};

} catch (...) {
    return exceptionToStatus();
}

}  // namespace

void DBClientConnection::_auth(const BSONObj& params) {
    if (autoReconnect) {
        /* note we remember the auth info before we attempt to auth -- if the connection is broken,
         * we will then have it for the next autoreconnect attempt.
         */
        authCache[params[auth::getSaslCommandUserDBFieldName()].str()] = params.getOwned();
    }

    DBClientBase::_auth(params);
}

Status DBClientConnection::authenticateInternalUser(auth::StepDownBehavior stepDownBehavior) {
    if (autoReconnect) {
        _internalAuthOnReconnect = true;
        _internalAuthStepDownBehavior = stepDownBehavior;
    }

    return DBClientBase::authenticateInternalUser(stepDownBehavior);
}

bool DBClientConnection::connect(const HostAndPort& server,
                                 StringData applicationName,
                                 std::string& errmsg,
                                 boost::optional<TransientSSLParams> transientSSLParams) {
    auto connectStatus = connect(server, applicationName, transientSSLParams);
    if (!connectStatus.isOK()) {
        errmsg = connectStatus.reason();
        return false;
    }
    return true;
}

Status DBClientConnection::connect(const HostAndPort& serverAddress,
                                   StringData applicationName,
                                   boost::optional<TransientSSLParams> transientSSLParams) {
    auto connectStatus = connectSocketOnly(serverAddress, transientSSLParams);
    if (!connectStatus.isOK()) {
        return connectStatus;
    }

    // NOTE: If the 'applicationName' parameter is a view of the '_applicationName' member, as
    // happens, for instance, in the call to DBClientConnection::connect from
    // DBClientConnection::_checkConnection then the following line will invalidate the
    // 'applicationName' parameter, since the memory that it views within _applicationName will be
    // freed. Do not reference the 'applicationName' parameter after this line. If you need to
    // access the application name, do it through the _applicationName member.
    _applicationName = applicationName.toString();

    auto speculativeAuthType = auth::SpeculativeAuthType::kNone;
    std::shared_ptr<SaslClientSession> saslClientSession;
    auto swIsMasterReply = initWireVersion(
        this, _applicationName, _uri, &_saslMechsForAuth, &speculativeAuthType, &saslClientSession);
    if (!swIsMasterReply.isOK()) {
        _markFailed(kSetFlag);
        swIsMasterReply.status.addContext(
            "Connection handshake failed. Is your mongod/mongos 3.4 or older?"_sd);
        return swIsMasterReply.status;
    }

    // Ensure that the isMaster response is "ok:1".
    auto isMasterStatus = getStatusFromCommandResult(swIsMasterReply.data);
    if (!isMasterStatus.isOK()) {
        return isMasterStatus;
    }

    auto replyWireVersion = wire_version::parseWireVersionFromHelloReply(swIsMasterReply.data);
    if (!replyWireVersion.isOK()) {
        return replyWireVersion.getStatus();
    }

    {
        // The Server Discovery and Monitoring (SDAM) specification identifies a replica set member
        // as either (a) having a "setName" field in the isMaster response, or (b) having
        // "isreplicaset: true" in the isMaster response.
        //
        // https://github.com/mongodb/specifications/blob/c386e23724318e2fa82f4f7663d77581b755b2c3/
        // source/server-discovery-and-monitoring/server-discovery-and-monitoring.rst#type
        const bool hasSetNameField = swIsMasterReply.data.hasField("setName");
        const bool isReplicaSetField = swIsMasterReply.data.getBoolField("isreplicaset");
        _isReplicaSetMember = hasSetNameField || isReplicaSetField;
    }

    {
        std::string msgField;
        auto msgFieldExtractStatus = bsonExtractStringField(swIsMasterReply.data, "msg", &msgField);

        if (msgFieldExtractStatus == ErrorCodes::NoSuchKey) {
            _isMongos = false;
        } else if (!msgFieldExtractStatus.isOK()) {
            return msgFieldExtractStatus;
        } else {
            _isMongos = (msgField == "isdbgrid");
        }
    }

    auto wireSpec = WireSpec::instance().get();
    auto validateStatus =
        wire_version::validateWireVersion(wireSpec->outgoing, replyWireVersion.getValue());
    if (!validateStatus.isOK()) {
        LOGV2_WARNING(20126,
                      "Remote host has incompatible wire version: {error}",
                      "Remote host has incompatible wire version",
                      "error"_attr = validateStatus);

        return validateStatus;
    }

    if (_hook) {
        auto validationStatus = _hook(swIsMasterReply);
        if (!validationStatus.isOK()) {
            // Disconnect and mark failed.
            _markFailed(kReleaseSession);
            return validationStatus;
        }
    }

    {
        auto swAuth = completeSpeculativeAuth(
            this, speculativeAuthType, saslClientSession, _uri, swIsMasterReply.data);
        if (!swAuth.isOK()) {
            return swAuth.getStatus();
        }

        if (swAuth.getValue()) {
            _authenticatedDuringConnect = true;
        }
    }

    return Status::OK();
}

Status DBClientConnection::connectSocketOnly(
    const HostAndPort& serverAddress, boost::optional<TransientSSLParams> transientSSLParams) {
    _serverAddress = serverAddress;
    _transientSSLParams = transientSSLParams;
    _markFailed(kReleaseSession);


    if (_stayFailed.load()) {
        // This is just an optimization so we don't waste time connecting just to throw it away.
        // The check below is the one that is important for correctness.
        return makeSocketError(SocketErrorKind::FAILED_STATE, toString());
    }

    if (serverAddress.host().empty()) {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << "couldn't connect to server " << _serverAddress.toString()
                                    << ", host is empty");
    }

    if (serverAddress.host() == "0.0.0.0") {
        return Status(ErrorCodes::InvalidOptions,
                      str::stream() << "couldn't connect to server " << _serverAddress.toString()
                                    << ", address resolved to 0.0.0.0");
    }

    auto sws = getGlobalServiceContext()->getTransportLayer()->connect(
        serverAddress,
        transientSSLParams ? transport::kEnableSSL : _uri.getSSLMode(),
        _socketTimeout.value_or(Milliseconds{5000}),
        transientSSLParams);
    if (!sws.isOK()) {
        auto connectStatus = sws.getStatus();
        // InvalidSSLConfiguration error needs to be propagated up since it is not a retriable
        // error.
        auto code = connectStatus == ErrorCodes::InvalidSSLConfiguration
            ? ErrorCodes::InvalidSSLConfiguration
            : ErrorCodes::HostUnreachable;
        return Status(code,
                      str::stream() << "couldn't connect to server " << _serverAddress.toString()
                                    << ", connection attempt failed: " << connectStatus);
    }

    {
        stdx::lock_guard<Latch> lk(_sessionMutex);
        if (_stayFailed.load()) {
            // This object is still in a failed state. The session we just created will be destroyed
            // immediately since we aren't holding on to it.
            return makeSocketError(SocketErrorKind::FAILED_STATE, toString());
        }
        _session = std::move(sws.getValue());
        _failed.store(false);
    }
    _sessionCreationMicros = curTimeMicros64();
    _lastConnectivityCheck = Date_t::now();
    _session->setTimeout(_socketTimeout);
    _session->setTags(_tagMask);
    LOGV2_DEBUG(20119,
                1,
                "Connected to host {connString}",
                "Connected to host",
                "connString"_attr = toString());
    return Status::OK();
}

void DBClientConnection::logout(const string& dbname, BSONObj& info) {
    authCache.erase(dbname);
    _internalAuthOnReconnect = false;
    runCommand(dbname, BSON("logout" << 1), info);
}

std::pair<rpc::UniqueReply, DBClientBase*> DBClientConnection::runCommandWithTarget(
    OpMsgRequest request) {
    auto out = DBClientBase::runCommandWithTarget(std::move(request));
    if (!_parentReplSetName.empty()) {
        const auto replyBody = out.first->getCommandReply();
        if (!isOk(replyBody)) {
            handleNotPrimaryResponse(replyBody, "errmsg");
        }
    }

    return out;
}

std::pair<rpc::UniqueReply, std::shared_ptr<DBClientBase>> DBClientConnection::runCommandWithTarget(
    OpMsgRequest request, std::shared_ptr<DBClientBase> me) {
    auto out = DBClientBase::runCommandWithTarget(std::move(request), std::move(me));
    if (!_parentReplSetName.empty()) {
        const auto replyBody = out.first->getCommandReply();
        if (!isOk(replyBody)) {
            handleNotPrimaryResponse(replyBody, "errmsg");
        }
    }

    return out;
}

rpc::UniqueReply DBClientConnection::parseCommandReplyMessage(const std::string& host,
                                                              const Message& replyMsg) {
    try {
        return DBClientBase::parseCommandReplyMessage(host, std::move(replyMsg));
    } catch (const DBException& ex) {
        if (ErrorCodes::isConnectionFatalMessageParseError(ex.code())) {
            _markFailed(kEndSession);
        }
        throw;
    }
}

void DBClientConnection::_markFailed(FailAction action) {
    _failed.store(true);
    if (_session) {
        if (action == kEndSession) {
            _session->end();
        } else if (action == kReleaseSession) {
            transport::SessionHandle destroyedOutsideMutex;

            stdx::lock_guard<Latch> lk(_sessionMutex);
            _session.swap(destroyedOutsideMutex);
        }
    }
}

bool DBClientConnection::isStillConnected() {
    // This method tries to figure out whether the connection is still open, but with several
    // caveats.

    // If we don't have a _session then we are definitely not connected. If we've been marked failed
    // then we are supposed to pretend that we aren't connected, even though we may be.
    // HOWEVER, some unit tests have poorly designed mocks that never populate _session, even when
    // the DBClientConnection should be considered healthy and connected.

    if (_stayFailed.load()) {
        // Ensures there is no chance that a perma-failed connection can go back into a pool.
        return false;
    } else if (!_session) {
        // This should always return false in practice, but needs to do this to work around poorly
        // designed mocks as described above.
        return !_failed.load();
    } else if (_failed.load()) {
        return false;
    }

    // Checking whether the socket actually has an error by calling _session->isConnected()
    // is actually pretty expensive, so we cache the result for 5 seconds
    auto now = getGlobalServiceContext()->getFastClockSource()->now();
    if (now - _lastConnectivityCheck < Seconds{5}) {
        return true;
    }

    _lastConnectivityCheck = now;

    // This will poll() the underlying socket and do a 1 byte recv to see if the connection
    // has been closed.
    if (_session->isConnected())
        return true;

    _markFailed(kSetFlag);
    return false;
}

void DBClientConnection::setTags(transport::Session::TagMask tags) {
    _tagMask = tags;
    if (!_session)
        return;
    _session->setTags(tags);
}

void DBClientConnection::shutdown() {
    stdx::lock_guard<Latch> lk(_sessionMutex);
    _markFailed(kEndSession);
}

void DBClientConnection::shutdownAndDisallowReconnect() {
    stdx::lock_guard<Latch> lk(_sessionMutex);
    _stayFailed.store(true);
    _markFailed(kEndSession);
}

void DBClientConnection::_checkConnection() {
    dassert(_failed.load());  // only called when in failed state.

    if (!autoReconnect)
        throwSocketError(SocketErrorKind::FAILED_STATE, toString());

    // Don't hammer reconnects, backoff if needed
    sleepFor(_autoReconnectBackoff.nextSleep());

    LOGV2_DEBUG(20120,
                _logLevel.toInt(),
                "Trying to reconnect to {connString}",
                "Trying to reconnect",
                "connString"_attr = toString());
    string errmsg;

    auto connectStatus = connect(_serverAddress, _applicationName, _transientSSLParams);
    if (!connectStatus.isOK()) {
        _markFailed(kSetFlag);
        LOGV2_DEBUG(20121,
                    _logLevel.toInt(),
                    "Reconnect attempt to {connString} failed: {reason}",
                    "Reconnect attempt failed",
                    "connString"_attr = toString(),
                    "error"_attr = errmsg);
        if (connectStatus == ErrorCodes::IncompatibleCatalogManager) {
            uassertStatusOK(connectStatus);  // Will always throw
        } else {
            throwSocketError(SocketErrorKind::CONNECT_ERROR, connectStatus.reason());
        }
    }

    LOGV2_DEBUG(20122,
                _logLevel.toInt(),
                "Reconnected to {connString}",
                "Reconnected",
                "connString"_attr = toString());
    if (_internalAuthOnReconnect) {
        uassertStatusOK(authenticateInternalUser(_internalAuthStepDownBehavior));
    } else {
        for (const auto& kv : authCache) {
            try {
                DBClientConnection::_auth(kv.second);
            } catch (ExceptionFor<ErrorCodes::AuthenticationFailed>& ex) {
                LOGV2_DEBUG(20123,
                            _logLevel.toInt(),
                            "Reconnect: auth failed for on {db} using {user}: {reason}",
                            "Reconnect: auth failed",
                            "db"_attr = kv.second[auth::getSaslCommandUserDBFieldName()],
                            "user"_attr = kv.second[auth::getSaslCommandUserFieldName()],
                            "error"_attr = ex.what());
            }
        }
    }
}

void DBClientConnection::setSoTimeout(double timeout) {
    Milliseconds::rep timeoutMs = std::floor(timeout * 1000);
    if (timeout <= 0) {
        _socketTimeout = boost::none;
    } else if (timeoutMs >= Milliseconds::max().count()) {
        _socketTimeout = Milliseconds::max();
    } else {
        _socketTimeout = Milliseconds{timeoutMs};
    }

    if (_session) {
        _session->setTimeout(_socketTimeout);
    }
}

uint64_t DBClientConnection::getSockCreationMicroSec() const {
    if (_session) {
        return _sessionCreationMicros;
    } else {
        return INVALID_SOCK_CREATION_TIME;
    }
}

DBClientConnection::DBClientConnection(bool _autoReconnect,
                                       double so_timeout,
                                       MongoURI uri,
                                       const HandshakeValidationHook& hook,
                                       const ClientAPIVersionParameters* apiParameters)
    : DBClientBase(apiParameters),
      autoReconnect(_autoReconnect),
      _autoReconnectBackoff(Seconds(1), Seconds(2)),
      _hook(hook),
      _uri(std::move(uri)) {
    _numConnections.fetchAndAdd(1);
}

void DBClientConnection::say(Message& toSend, bool isRetry, string* actualServer) {
    checkConnection();
    ScopeGuard killSessionOnError([this] { _markFailed(kEndSession); });

    toSend.header().setId(nextMessageId());
    toSend.header().setResponseToMsgId(0);
    if (!MONGO_unlikely(dbClientConnectionDisableChecksum.shouldFail())) {
#ifdef MONGO_CONFIG_SSL
        if (!SSLPeerInfo::forSession(_session).isTLS) {
            OpMsg::appendChecksum(&toSend);
        }
#else
        OpMsg::appendChecksum(&toSend);
#endif
    }
    uassertStatusOK(
        _session->sinkMessage(uassertStatusOK(_compressorManager.compressMessage(toSend))));
    killSessionOnError.dismiss();
}

Status DBClientConnection::recv(Message& m, int lastRequestId) {
    ScopeGuard killSessionOnError([this] { _markFailed(kEndSession); });
    auto swm = _session->sourceMessage();
    if (!swm.isOK()) {
        return swm.getStatus();
    }

    m = std::move(swm.getValue());
    uassert(40570,
            "Response ID did not match the sent message ID.",
            m.header().getResponseToMsgId() == lastRequestId);

    if (m.operation() == dbCompressed) {
        m = uassertStatusOK(_compressorManager.decompressMessage(m));
    }

    killSessionOnError.dismiss();
    return Status::OK();
}

void DBClientConnection::_call(Message& toSend, Message& response, string* actualServer) {
    checkConnection();
    ScopeGuard killSessionOnError([this] { _markFailed(kEndSession); });

    toSend.header().setId(nextMessageId());
    toSend.header().setResponseToMsgId(0);
    if (!MONGO_unlikely(dbClientConnectionDisableChecksum.shouldFail())) {
#ifdef MONGO_CONFIG_SSL
        if (!SSLPeerInfo::forSession(_session).isTLS) {
            OpMsg::appendChecksum(&toSend);
        }
#else
        OpMsg::appendChecksum(&toSend);
#endif
    }
    auto swm = _compressorManager.compressMessage(toSend);
    uassertStatusOK(swm.getStatus());

    auto sinkStatus = _session->sinkMessage(swm.getValue());
    if (!sinkStatus.isOK()) {
        LOGV2(20124,
              "DBClientConnection failed to send message to {connString}: {error}",
              "DBClientConnection failed to send message",
              "connString"_attr = getServerAddress(),
              "error"_attr = redact(sinkStatus));
        uassertStatusOKWithContext(sinkStatus,
                                   str::stream() << "dbclient error communicating with server "
                                                 << getServerAddress());
    }

    swm = _session->sourceMessage();
    if (swm.isOK()) {
        response = std::move(swm.getValue());
    } else {
        LOGV2(20125,
              "DBClientConnection failed to receive message from {connString}: {error}",
              "DBClientConnection failed to receive message",
              "connString"_attr = getServerAddress(),
              "error"_attr = redact(swm.getStatus()));
        uassertStatusOKWithContext(swm.getStatus(),
                                   str::stream() << "dbclient error communicating with server "
                                                 << getServerAddress());
    }

    if (response.operation() == dbCompressed) {
        response = uassertStatusOK(_compressorManager.decompressMessage(response));
    }

    killSessionOnError.dismiss();
}

void DBClientConnection::setParentReplSetName(const string& replSetName) {
    _parentReplSetName = replSetName;
}

void DBClientConnection::handleNotPrimaryResponse(const BSONObj& replyBody,
                                                  StringData errorMsgFieldName) {
    const BSONElement errorMsgElem = replyBody[errorMsgFieldName];
    const BSONElement codeElem = replyBody["code"];

    if (!isNotPrimaryErrorString(errorMsgElem) &&
        !ErrorCodes::isNotPrimaryError(ErrorCodes::Error(codeElem.numberInt()))) {
        return;
    }

    auto monitor = ReplicaSetMonitor::get(_parentReplSetName);
    if (monitor) {
        monitor->failedHost(_serverAddress,
                            {ErrorCodes::NotWritablePrimary,
                             str::stream() << "got not primary from: " << _serverAddress
                                           << " of repl set: " << _parentReplSetName});
    }

    _markFailed(kSetFlag);
}

#ifdef MONGO_CONFIG_SSL
const SSLConfiguration* DBClientConnection::getSSLConfiguration() {
    return _session->getSSLConfiguration();
}

bool DBClientConnection::isUsingTransientSSLParams() const {
    return _transientSSLParams.has_value();
}

bool DBClientConnection::isTLS() {
    return SSLPeerInfo::forSession(_session).isTLS;
}

#endif

AtomicWord<int> DBClientConnection::_numConnections;

}  // namespace mongo
