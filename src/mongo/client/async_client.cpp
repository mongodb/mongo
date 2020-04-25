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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/client/async_client.h"

#include <memory>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/config.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/server_options.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/egress_tag_closer_manager.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/legacy_request_builder.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/version.h"

namespace mongo {

Future<AsyncDBClient::Handle> AsyncDBClient::connect(const HostAndPort& peer,
                                                     transport::ConnectSSLMode sslMode,
                                                     ServiceContext* const context,
                                                     transport::ReactorHandle reactor,
                                                     Milliseconds timeout) {
    auto tl = context->getTransportLayer();
    return tl->asyncConnect(peer, sslMode, std::move(reactor), timeout)
        .then([peer, context](transport::SessionHandle session) {
            return std::make_shared<AsyncDBClient>(peer, std::move(session), context);
        });
}

BSONObj AsyncDBClient::_buildIsMasterRequest(const std::string& appName,
                                             executor::NetworkConnectionHook* hook) {
    BSONObjBuilder bob;

    bob.append("isMaster", 1);

    const auto versionString = VersionInfoInterface::instance().version();
    ClientMetadata::serialize(appName, versionString, &bob);

    if (getTestCommandsEnabled()) {
        // Only include the host:port of this process in the isMaster command request if test
        // commands are enabled. mongobridge uses this field to identify the process opening a
        // connection to it.
        StringBuilder sb;
        sb << getHostNameCached() << ':' << serverGlobalParams.port;
        bob.append("hostInfo", sb.str());
    }

    _compressorManager.clientBegin(&bob);

    if (WireSpec::instance().isInternalClient) {
        WireSpec::appendInternalClientWireVersion(WireSpec::instance().outgoing, &bob);
    }

    if (hook) {
        return hook->augmentIsMasterRequest(bob.obj());
    } else {
        return bob.obj();
    }
}

void AsyncDBClient::_parseIsMasterResponse(BSONObj request,
                                           const std::unique_ptr<rpc::ReplyInterface>& response) {
    uassert(50786,
            "Expected opQuery response to isMaster",
            response->getProtocol() == rpc::Protocol::kOpQuery);
    auto responseBody = response->getCommandReply();
    uassertStatusOK(getStatusFromCommandResult(responseBody));

    auto protocolSet = uassertStatusOK(rpc::parseProtocolSetFromIsMasterReply(responseBody));
    auto validateStatus =
        rpc::validateWireVersion(WireSpec::instance().outgoing, protocolSet.version);
    if (!validateStatus.isOK()) {
        LOGV2_WARNING(23741,
                      "Remote host has incompatible wire version: {error}",
                      "Remote host has incompatible wire version",
                      "error"_attr = validateStatus);
        uasserted(validateStatus.code(),
                  str::stream() << "remote host has incompatible wire version: "
                                << validateStatus.reason());
    }

    auto& egressTagManager = executor::EgressTagCloserManager::get(_svcCtx);
    // Tag outgoing connection so it can be kept open on FCV upgrade if it is not to a
    // server with a lower binary version.
    if (protocolSet.version.maxWireVersion >= WireSpec::instance().outgoing.maxWireVersion) {
        egressTagManager.mutateTags(
            _peer, [](transport::Session::TagMask tags) { return transport::Session::kKeepOpen; });
    }

    auto clientProtocols = rpc::computeProtocolSet(WireSpec::instance().outgoing);
    invariant(clientProtocols != rpc::supports::kNone);
    // Set the operation protocol
    _negotiatedProtocol = uassertStatusOK(rpc::negotiate(protocolSet.protocolSet, clientProtocols));

    _compressorManager.clientFinish(responseBody);
}

auth::RunCommandHook AsyncDBClient::_makeAuthRunCommandHook() {
    return [this](OpMsgRequest request) {
        return runCommand(std::move(request)).then([](rpc::UniqueReply reply) -> Future<BSONObj> {
            auto status = getStatusFromCommandResult(reply->getCommandReply());
            if (!status.isOK()) {
                return status;
            } else {
                return reply->getCommandReply();
            }
        });
    };
}

Future<void> AsyncDBClient::authenticate(const BSONObj& params) {
    // We will only have a valid clientName if SSL is enabled.
    std::string clientName;
#ifdef MONGO_CONFIG_SSL
    if (getSSLManager()) {
        clientName = getSSLManager()->getSSLConfiguration().clientSubjectName.toString();
    }
#endif

    return auth::authenticateClient(params, remote(), clientName, _makeAuthRunCommandHook());
}

Future<void> AsyncDBClient::authenticateInternal(boost::optional<std::string> mechanismHint) {
    // If no internal auth information is set, don't bother trying to authenticate.
    if (!auth::isInternalAuthSet()) {
        return Future<void>::makeReady();
    }
    // We will only have a valid clientName if SSL is enabled.
    std::string clientName;
#ifdef MONGO_CONFIG_SSL
    if (getSSLManager()) {
        clientName = getSSLManager()->getSSLConfiguration().clientSubjectName.toString();
    }
#endif

    return auth::authenticateInternalClient(clientName, mechanismHint, _makeAuthRunCommandHook());
}

Future<bool> AsyncDBClient::completeSpeculativeAuth(std::shared_ptr<SaslClientSession> session,
                                                    std::string authDB,
                                                    BSONObj specAuth,
                                                    auth::SpeculativeAuthType speculativeAuthType) {
    if (specAuth.isEmpty()) {
        // No reply could mean failed auth, or old server.
        // A false reply will result in an explicit auth later.
        return false;
    }

    if (speculativeAuthType == auth::SpeculativeAuthType::kNone) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Received unexpected isMaster."
                                    << auth::kSpeculativeAuthenticate << " reply");
    }

    if (speculativeAuthType == auth::SpeculativeAuthType::kAuthenticate) {
        return specAuth.hasField(saslCommandUserFieldName);
    }

    invariant(speculativeAuthType == auth::SpeculativeAuthType::kSaslStart);
    invariant(session);

    return asyncSaslConversation(_makeAuthRunCommandHook(),
                                 session,
                                 BSON(saslContinueCommandName << 1),
                                 specAuth,
                                 std::move(authDB),
                                 kSaslClientLogLevelDefault)
        // Swallow failure even if the initial saslStart was okay.
        // It's possible for our speculative authentication to fail
        // while explicit auth succeeds if we're in a keyfile rollover state.
        // The first passphrase can fail, but later ones may be okay.
        .onCompletion([](Status status) { return status.isOK(); });
}

Future<void> AsyncDBClient::initWireVersion(const std::string& appName,
                                            executor::NetworkConnectionHook* const hook) {
    auto requestObj = _buildIsMasterRequest(appName, hook);
    // We use a legacy request to create our ismaster request because we may
    // have to communicate with servers that do not support other protocols.
    auto requestMsg =
        rpc::legacyRequestFromOpMsgRequest(OpMsgRequest::fromDBAndBody("admin", requestObj));
    auto clkSource = _svcCtx->getFastClockSource();
    auto start = clkSource->now();

    auto msgId = nextMessageId();
    return _call(requestMsg, msgId)
        .then([msgId, this]() { return _waitForResponse(msgId); })
        .then([this, requestObj, hook, clkSource, start](Message response) {
            auto cmdReply = rpc::makeReply(&response);
            _parseIsMasterResponse(requestObj, cmdReply);
            if (hook) {
                auto millis = duration_cast<Milliseconds>(clkSource->now() - start);
                executor::RemoteCommandResponse cmdResp(*cmdReply, millis);
                uassertStatusOK(hook->validateHost(_peer, requestObj, std::move(cmdResp)));
            }
        });
}

Future<void> AsyncDBClient::_call(Message request, int32_t msgId, const BatonHandle& baton) {
    auto swm = _compressorManager.compressMessage(request);
    if (!swm.isOK()) {
        return swm.getStatus();
    }

    request = std::move(swm.getValue());
    request.header().setId(msgId);
    request.header().setResponseToMsgId(0);
#ifdef MONGO_CONFIG_SSL
    if (!SSLPeerInfo::forSession(_session).isTLS) {
        OpMsg::appendChecksum(&request);
    }
#else
    OpMsg::appendChecksum(&request);
#endif

    return _session->asyncSinkMessage(request, baton);
}

Future<Message> AsyncDBClient::_waitForResponse(boost::optional<int32_t> msgId,
                                                const BatonHandle& baton) {
    return _session->asyncSourceMessage(baton).then(
        [this, msgId](Message response) -> StatusWith<Message> {
            uassert(50787,
                    "ResponseId did not match sent message ID.",
                    msgId ? response.header().getResponseToMsgId() == msgId : true);
            if (response.operation() == dbCompressed) {
                return _compressorManager.decompressMessage(response);
            } else {
                return response;
            }
        });
}

Future<rpc::UniqueReply> AsyncDBClient::runCommand(OpMsgRequest request,
                                                   const BatonHandle& baton,
                                                   bool fireAndForget) {
    invariant(_negotiatedProtocol);
    auto requestMsg = rpc::messageFromOpMsgRequest(*_negotiatedProtocol, std::move(request));
    if (fireAndForget) {
        OpMsg::setFlag(&requestMsg, OpMsg::kMoreToCome);
    }
    auto msgId = nextMessageId();
    auto future = _call(std::move(requestMsg), msgId, baton);

    if (fireAndForget) {
        return std::move(future).then([msgId, this]() -> Future<rpc::UniqueReply> {
            // Return a mock status OK response since we do not expect a real response.
            OpMsgBuilder builder;
            builder.setBody(BSON("ok" << 1));
            Message responseMsg = builder.finish();
            responseMsg.header().setResponseToMsgId(msgId);
            responseMsg.header().setId(msgId);
            return rpc::UniqueReply(responseMsg, rpc::makeReply(&responseMsg));
        });
    }

    return std::move(future)
        .then([msgId, baton, this]() { return _waitForResponse(msgId, baton); })
        .then([this](Message response) -> Future<rpc::UniqueReply> {
            return rpc::UniqueReply(response, rpc::makeReply(&response));
        });
}

Future<executor::RemoteCommandResponse> AsyncDBClient::runCommandRequest(
    executor::RemoteCommandRequest request, const BatonHandle& baton) {
    auto clkSource = _svcCtx->getPreciseClockSource();
    auto start = clkSource->now();
    auto opMsgRequest = OpMsgRequest::fromDBAndBody(
        std::move(request.dbname), std::move(request.cmdObj), std::move(request.metadata));
    auto fireAndForget =
        request.fireAndForgetMode == executor::RemoteCommandRequest::FireAndForgetMode::kOn;
    return runCommand(std::move(opMsgRequest), baton, fireAndForget)
        .then([start, clkSource, this](rpc::UniqueReply response) {
            auto duration = duration_cast<Milliseconds>(clkSource->now() - start);
            return executor::RemoteCommandResponse(*response, duration);
        });
}

Future<executor::RemoteCommandResponse> AsyncDBClient::_continueReceiveExhaustResponse(
    ClockSource::StopWatch stopwatch, boost::optional<int32_t> msgId, const BatonHandle& baton) {
    return _waitForResponse(msgId, baton)
        .then([stopwatch, msgId, baton, this](Message responseMsg) mutable {
            bool isMoreToComeSet = OpMsg::isFlagSet(responseMsg, OpMsg::kMoreToCome);
            rpc::UniqueReply response = rpc::UniqueReply(responseMsg, rpc::makeReply(&responseMsg));
            auto rcResponse = executor::RemoteCommandResponse(
                *response, duration_cast<Milliseconds>(stopwatch.elapsed()), isMoreToComeSet);
            return rcResponse;
        });
}

Future<executor::RemoteCommandResponse> AsyncDBClient::awaitExhaustCommand(
    const BatonHandle& baton) {
    return _continueReceiveExhaustResponse(ClockSource::StopWatch(), boost::none, baton);
}

Future<executor::RemoteCommandResponse> AsyncDBClient::runExhaustCommand(OpMsgRequest request,
                                                                         const BatonHandle& baton) {
    invariant(_negotiatedProtocol);
    auto requestMsg = rpc::messageFromOpMsgRequest(*_negotiatedProtocol, std::move(request));
    OpMsg::setFlag(&requestMsg, OpMsg::kExhaustSupported);

    auto msgId = nextMessageId();
    return _call(std::move(requestMsg), msgId, baton).then([msgId, baton, this]() mutable {
        return _continueReceiveExhaustResponse(ClockSource::StopWatch(), msgId, baton);
    });
}

Future<executor::RemoteCommandResponse> AsyncDBClient::beginExhaustCommandRequest(
    executor::RemoteCommandRequest request, const BatonHandle& baton) {
    auto opMsgRequest = OpMsgRequest::fromDBAndBody(
        std::move(request.dbname), std::move(request.cmdObj), std::move(request.metadata));

    return runExhaustCommand(std::move(opMsgRequest), baton);
}

void AsyncDBClient::cancel(const BatonHandle& baton) {
    _session->cancelAsyncOperations(baton);
}

bool AsyncDBClient::isStillConnected() {
    return _session->isConnected();
}

void AsyncDBClient::end() {
    _session->end();
}

const HostAndPort& AsyncDBClient::remote() const {
    return _peer;
}

const HostAndPort& AsyncDBClient::local() const {
    return _session->local();
}

}  // namespace mongo
