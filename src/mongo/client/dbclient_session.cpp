/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/client/dbclient_session.h"


#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/client/sasl_client_session.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_peer_info.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/version.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {

using std::endl;
using std::map;
using std::string;
using std::unique_ptr;

MONGO_FAIL_POINT_DEFINE(dbClientSessionDisableChecksum);

namespace {

StatusWith<bool> completeSpeculativeAuth(DBClientSession* conn,
                                         auth::SpeculativeAuthType speculativeAuthType,
                                         std::shared_ptr<SaslClientSession> session,
                                         const MongoURI& uri,
                                         BSONObj helloReply) {
    auto specAuthElem = helloReply[auth::kSpeculativeAuthenticate];
    if (specAuthElem.eoo()) {
        return false;
    }

    if (speculativeAuthType == auth::SpeculativeAuthType::kNone) {
        return {ErrorCodes::BadValue,
                str::stream() << "Unexpected hello." << auth::kSpeculativeAuthenticate << " reply"};
    }

    if (specAuthElem.type() != Object) {
        return {ErrorCodes::BadValue,
                str::stream() << "hello." << auth::kSpeculativeAuthenticate
                              << " reply must be an object"};
    }

    auto specAuth = specAuthElem.Obj();
    if (specAuth.isEmpty()) {
        return {ErrorCodes::BadValue,
                str::stream() << "hello." << auth::kSpeculativeAuthenticate
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
 * Initializes the wire version of conn, and returns the "hello" reply.
 */
executor::RemoteCommandResponse initWireVersion(
    DBClientSession* conn,
    StringData applicationName,
    const MongoURI& uri,
    std::vector<std::string>* saslMechsForAuth,
    auth::SpeculativeAuthType* speculativeAuthType,
    std::shared_ptr<SaslClientSession>* saslClientSession) try {

    BSONObjBuilder bob;
    bob.append("hello", 1);

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
        // Only include the host:port of this process in the "hello" command request if test
        // commands are enabled. mongobridge uses this field to identify the process opening a
        // connection to it.
        StringBuilder sb;
        sb << getHostName() << ':' << serverGlobalParams.port;
        bob.append("hostInfo", sb.str());
    }

    if (auto status = DBClientSession::appendClientMetadata(applicationName, &bob);
        !status.isOK()) {
        return status;
    }

    conn->getCompressorManager().clientBegin(&bob);

    WireSpec::getWireSpec(getGlobalServiceContext()).appendInternalClientWireVersionIfNeeded(&bob);

    Date_t start{Date_t::now()};
    auto result = conn->runCommand(OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired /* admin is not per-tenant. */,
        DatabaseName::kAdmin,
        bob.obj()));
    Date_t finish{Date_t::now()};

    BSONObj helloObj = result->getCommandReply().getOwned();

    auto replyWireVersion = wire_version::parseWireVersionFromHelloReply(helloObj);
    if (replyWireVersion.isOK()) {
        conn->setWireVersions(replyWireVersion.getValue().minWireVersion,
                              replyWireVersion.getValue().maxWireVersion);
    }

    if (helloObj.hasField("saslSupportedMechs") && helloObj["saslSupportedMechs"].type() == Array) {
        auto array = helloObj["saslSupportedMechs"].Array();
        for (const auto& elem : array) {
            saslMechsForAuth->push_back(elem.checkAndGetStringData().toString());
        }
    }

    conn->getCompressorManager().clientFinish(helloObj);

    return executor::RemoteCommandResponse{std::move(helloObj), finish - start};

} catch (...) {
    return exceptionToStatus();
}

boost::optional<Milliseconds> clampTimeout(double timeoutInSec) {
    if (timeoutInSec <= 0) {
        return boost::none;
    }
    double timeout = std::floor(timeoutInSec * 1000);
    return (timeout >= static_cast<double>(Milliseconds::max().count()))
        ? Milliseconds::max()
        : Milliseconds{static_cast<Milliseconds::rep>(timeout)};
}

}  // namespace

void DBClientSession::connect(const HostAndPort& serverAddress,
                              StringData applicationName,
                              boost::optional<TransientSSLParams> transientSSLParams) {
    connectNoHello(serverAddress, transientSSLParams);

    // NOTE: If the 'applicationName' parameter is a view of the '_applicationName' member, as
    // happens, for instance, in the call to DBClientSession::connect from
    // DBClientSession::_checkConnection then the following line will invalidate the
    // 'applicationName' parameter, since the memory that it views within _applicationName will be
    // freed. Do not reference the 'applicationName' parameter after this line. If you need to
    // access the application name, do it through the _applicationName member.
    _applicationName = applicationName.toString();

    auto speculativeAuthType = auth::SpeculativeAuthType::kNone;
    std::shared_ptr<SaslClientSession> saslClientSession;
    auto swHelloReply = initWireVersion(
        this, _applicationName, _uri, &_saslMechsForAuth, &speculativeAuthType, &saslClientSession);
    if (!swHelloReply.isOK()) {
        _markFailed(kSetFlag);
        swHelloReply.status.addContext(
            "Connection handshake failed. Is your mongod/mongos 3.4 or older?"_sd);
        uassertStatusOK(swHelloReply.status);
    }

    // Ensure that the "hello" response is "ok:1".
    uassertStatusOK(getStatusFromCommandResult(swHelloReply.data));

    auto replyWireVersion =
        uassertStatusOK(wire_version::parseWireVersionFromHelloReply(swHelloReply.data));

    {
        // The Server Discovery and Monitoring (SDAM) specification identifies a replica set member
        // as either (a) having a "setName" field in the "hello" response, or (b) having
        // "isreplicaset: true" in the "hello" response.
        //
        // https://github.com/mongodb/specifications/blob/c386e23724318e2fa82f4f7663d77581b755b2c3/
        // source/server-discovery-and-monitoring/server-discovery-and-monitoring.rst#type
        const bool hasSetNameField = swHelloReply.data.hasField("setName");
        const bool isReplicaSetField = swHelloReply.data.getBoolField("isreplicaset");
        _isReplicaSetMember = hasSetNameField || isReplicaSetField;
    }

    {
        std::string msgField;
        auto msgFieldExtractStatus = bsonExtractStringField(swHelloReply.data, "msg", &msgField);

        if (msgFieldExtractStatus == ErrorCodes::NoSuchKey) {
            _isMongos = false;
        } else {
            uassertStatusOK(msgFieldExtractStatus);
            _isMongos = (msgField == "isdbgrid");
        }
    }

    auto wireSpec = WireSpec::getWireSpec(getGlobalServiceContext()).get();
    auto validateStatus = wire_version::validateWireVersion(wireSpec->outgoing, replyWireVersion);
    if (!validateStatus.isOK()) {
        LOGV2_WARNING(
            20126, "Remote host has incompatible wire version", "error"_attr = validateStatus);
        uassertStatusOK(validateStatus);
    }

    if (_hook) {
        auto validationStatus = _hook(swHelloReply);
        if (!validationStatus.isOK()) {
            // Disconnect and mark failed.
            _markFailed(kReleaseSession);
            uassertStatusOK(validationStatus);
        }
    }

    auto didAuth = uassertStatusOK(completeSpeculativeAuth(
        this, speculativeAuthType, saslClientSession, _uri, swHelloReply.data));
    if (didAuth) {
        _authenticatedDuringConnect = true;
    }
}

void DBClientSession::connectNoHello(const HostAndPort& serverAddress,
                                     boost::optional<TransientSSLParams> transientSSLParams) {
    _serverAddress = serverAddress;
    _transientSSLParams = transientSSLParams;
    _markFailed(kReleaseSession);

    if (_stayFailed.load()) {
        // This is just an optimization so we don't waste time connecting just to throw it away.
        // The check below is the one that is important for correctness.
        throwSocketError(SocketErrorKind::FAILED_STATE, toString());
    }

    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "couldn't connect to server " << _serverAddress.toString()
                          << ", host is empty",
            !serverAddress.host().empty());

    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "couldn't connect to server " << _serverAddress.toString()
                          << ", address resolved to 0.0.0.0",
            serverAddress.host() != "0.0.0.0");

    auto sws = _makeSession(serverAddress,
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

        uasserted(code,
                  str::stream() << "couldn't connect to server " << _serverAddress.toString()
                                << ", connection attempt failed: " << connectStatus);
    }

    {
        stdx::lock_guard<Latch> lk(_sessionMutex);
        if (_stayFailed.load()) {
            // This object is still in a failed state. The session we just created will be destroyed
            // immediately since we aren't holding on to it.
            throwSocketError(SocketErrorKind::FAILED_STATE, toString());
        }
        _session = std::move(sws.getValue());
        _failed.store(false);
    }
    _sessionCreationTimeMicros = curTimeMicros64();
    _lastConnectivityCheck = Date_t::now();
    if (_socketTimeout) {
        _session->setTimeout(_socketTimeout);
    }
    LOGV2_DEBUG(20119, 1, "Connected to host", "connString"_attr = toString());
}

void DBClientSession::_markFailed(FailAction action) {
    _failed.store(true);
    if (_session) {
        if (action == kKillSession) {
            _killSession();
        } else if (action == kReleaseSession) {
            std::shared_ptr<transport::Session> destroyedOutsideMutex;

            stdx::lock_guard<Latch> lk(_sessionMutex);
            _session.swap(destroyedOutsideMutex);
        }
    }
}

bool DBClientSession::isStillConnected() {
    // This method tries to figure out whether the connection is still open, but with several
    // caveats.

    // If we don't have a _session then we are definitely not connected. If we've been marked failed
    // then we are supposed to pretend that we aren't connected, even though we may be.
    // HOWEVER, some unit tests have poorly designed mocks that never populate _session, even when
    // the DBClientSession should be considered healthy and connected.

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

    if (_session->isConnected())
        return true;

    _markFailed(kSetFlag);
    return false;
}

void DBClientSession::shutdown() {
    stdx::lock_guard<Latch> lk(_sessionMutex);
    _markFailed(kKillSession);
}

void DBClientSession::shutdownAndDisallowReconnect() {
    stdx::lock_guard<Latch> lk(_sessionMutex);
    _stayFailed.store(true);
    _markFailed(kKillSession);
}

void DBClientSession::setSoTimeout(double timeout) {
    _socketTimeout = clampTimeout(timeout);
    if (_session) {
        _session->setTimeout(_socketTimeout);
    }
}

uint64_t DBClientSession::getSockCreationMicroSec() const {
    if (_session) {
        return _sessionCreationTimeMicros;
    } else {
        return INVALID_SOCK_CREATION_TIME;
    }
}

Status DBClientSession::appendClientMetadata(StringData applicationName, BSONObjBuilder* bob) {
    auto versionString = VersionInfoInterface::instance().version();

    return ClientMetadata::serialize(
        "MongoDB Internal Client", versionString, applicationName, bob);
}

DBClientSession::DBClientSession(bool autoReconnect,
                                 double soTimeout,
                                 MongoURI uri,
                                 const HandshakeValidationHook& hook,
                                 const ClientAPIVersionParameters* apiParameters)
    : DBClientBase(apiParameters),
      _socketTimeout(clampTimeout(soTimeout)),
      _autoReconnect(autoReconnect),
      _hook(hook),
      _uri(std::move(uri)) {}

void DBClientSession::say(Message& toSend, bool isRetry, string* actualServer) {
    ensureConnection();
    ScopeGuard killSessionOnError([this] { _markFailed(kKillSession); });

    toSend.header().setId(nextMessageId());
    toSend.header().setResponseToMsgId(0);
    if (!MONGO_unlikely(dbClientSessionDisableChecksum.shouldFail())) {
#ifdef MONGO_CONFIG_SSL
        if (!SSLPeerInfo::forSession(_session).isTLS()) {
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

Message DBClientSession::recv(int lastRequestId) {
    ScopeGuard killSessionOnError([this] { _markFailed(kKillSession); });
    auto m = uassertStatusOK(_session->sourceMessage());

    uassert(40570,
            "Response ID did not match the sent message ID.",
            m.header().getResponseToMsgId() == lastRequestId);

    if (m.operation() == dbCompressed) {
        m = uassertStatusOK(_compressorManager.decompressMessage(m));
    }

    killSessionOnError.dismiss();
    return m;
}

Message DBClientSession::_call(Message& toSend, string* actualServer) {
    ensureConnection();
    ScopeGuard killSessionOnError([this] { _markFailed(kKillSession); });

    toSend.header().setId(nextMessageId());
    toSend.header().setResponseToMsgId(0);
    if (!MONGO_unlikely(dbClientSessionDisableChecksum.shouldFail())) {
#ifdef MONGO_CONFIG_SSL
        if (!SSLPeerInfo::forSession(_session).isTLS()) {
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
              "DBClientSession failed to send message",
              "connString"_attr = getServerAddress(),
              "error"_attr = redact(sinkStatus));
        uassertStatusOKWithContext(sinkStatus,
                                   str::stream() << "dbclient error communicating with server "
                                                 << getServerAddress());
    }

    swm = _session->sourceMessage();
    Message response;
    if (swm.isOK()) {
        response = std::move(swm.getValue());
    } else {
        LOGV2(20125,
              "DBClientSession failed to receive message",
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
    return response;
}

#ifdef MONGO_CONFIG_SSL
const SSLConfiguration* DBClientSession::getSSLConfiguration() {
    auto& sslManager = _session->getSSLManager();
    if (!sslManager) {
        return nullptr;
    }
    return &sslManager->getSSLConfiguration();
}

bool DBClientSession::isUsingTransientSSLParams() const {
    return _transientSSLParams.has_value();
}

#endif

}  // namespace mongo
