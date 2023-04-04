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

#include "mongo/client/async_client.h"

#include <memory>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/config.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/connection_health_metrics_parameter_gen.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/egress_tag_closer_manager.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_peer_info.h"
#include "mongo/util/version.h"


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo {
MONGO_FAIL_POINT_DEFINE(pauseBeforeMarkKeepOpen);
MONGO_FAIL_POINT_DEFINE(alwaysLogConnAcquisitionToWireTime)

namespace {
bool connHealthMetricsEnabled() {
    // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
    return gFeatureFlagConnHealthMetrics.isEnabledAndIgnoreFCVUnsafe();
}
CounterMetric totalTimeForEgressConnectionAcquiredToWireMicros(
    "network.totalTimeForEgressConnectionAcquiredToWireMicros", connHealthMetricsEnabled);
}  // namespace

Future<AsyncDBClient::Handle> AsyncDBClient::connect(
    const HostAndPort& peer,
    transport::ConnectSSLMode sslMode,
    ServiceContext* const context,
    transport::ReactorHandle reactor,
    Milliseconds timeout,
    std::shared_ptr<ConnectionMetrics> connectionMetrics,
    std::shared_ptr<const transport::SSLConnectionContext> transientSSLContext) {
    auto tl = context->getTransportLayer();
    return tl
        ->asyncConnect(
            peer, sslMode, std::move(reactor), timeout, connectionMetrics, transientSSLContext)
        .then([peer, context](std::shared_ptr<transport::Session> session) {
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

    if (auto wireSpec = WireSpec::instance().get(); wireSpec->isInternalClient) {
        WireSpec::appendInternalClientWireVersion(wireSpec->outgoing, &bob);
    }

    if (hook) {
        return hook->augmentIsMasterRequest(remote(), bob.obj());
    } else {
        return bob.obj();
    }
}

void AsyncDBClient::_parseIsMasterResponse(BSONObj request,
                                           const std::unique_ptr<rpc::ReplyInterface>& response) {
    uassert(50786,
            "Expected OP_MSG response to isMaster",
            response->getProtocol() == rpc::Protocol::kOpMsg);
    auto wireSpec = WireSpec::instance().get();
    auto responseBody = response->getCommandReply();
    uassertStatusOK(getStatusFromCommandResult(responseBody));

    auto replyWireVersion =
        uassertStatusOK(wire_version::parseWireVersionFromHelloReply(responseBody));
    auto validateStatus = wire_version::validateWireVersion(wireSpec->outgoing, replyWireVersion);
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
    if (replyWireVersion.maxWireVersion >= wireSpec->outgoing.maxWireVersion) {
        pauseBeforeMarkKeepOpen.pauseWhileSet();
        egressTagManager.mutateTags(
            _peer, [](transport::Session::TagMask tags) { return transport::Session::kKeepOpen; });
    } else {
        // The outgoing connection is to a server with a lower binary version, unset the pending
        // flag if it's set to ensure that connections will be dropped.
        egressTagManager.mutateTags(_peer, [](transport::Session::TagMask tags) {
            return tags & ~transport::Session::kPending;
        });
    }

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
    auto& sslManager = _session->getSSLManager();
    if (sslManager) {
        clientName = sslManager->getSSLConfiguration().clientSubjectName.toString();
    }
#endif

    return auth::authenticateClient(params, remote(), clientName, _makeAuthRunCommandHook());
}

Future<void> AsyncDBClient::authenticateInternal(
    boost::optional<std::string> mechanismHint,
    std::shared_ptr<auth::InternalAuthParametersProvider> authProvider) {
    // If no internal auth information is set, don't bother trying to authenticate.
    if (!auth::isInternalAuthSet()) {
        return Future<void>::makeReady();
    }
    // We will only have a valid clientName if SSL is enabled.
    std::string clientName;
#ifdef MONGO_CONFIG_SSL
    auto& sslManager = _session->getSSLManager();
    if (sslManager) {
        clientName = sslManager->getSSLConfiguration().clientSubjectName.toString();
    }
#endif

    return auth::authenticateInternalClient(clientName,
                                            remote(),
                                            mechanismHint,
                                            auth::StepDownBehavior::kKillConnection,
                                            _makeAuthRunCommandHook(),
                                            std::move(authProvider));
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
    auto opMsgRequest = OpMsgRequest::fromDBAndBody("admin", requestObj);

    auto msgId = nextMessageId();
    return _call(opMsgRequest.serialize(), msgId)
        .then([msgId, this]() { return _waitForResponse(msgId); })
        .then([this, requestObj, hook, timer = Timer{}](Message response) {
            auto cmdReply = rpc::makeReply(&response);
            _parseIsMasterResponse(requestObj, cmdReply);
            if (hook) {
                executor::RemoteCommandResponse cmdResp(*cmdReply, timer.elapsed());
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
    if (!SSLPeerInfo::forSession(_session).isTLS()) {
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

Future<rpc::UniqueReply> AsyncDBClient::runCommand(
    OpMsgRequest request,
    const BatonHandle& baton,
    bool fireAndForget,
    boost::optional<std::shared_ptr<Timer>> fromConnAcquiredTimer) {
    auto requestMsg = request.serialize();
    if (fireAndForget) {
        OpMsg::setFlag(&requestMsg, OpMsg::kMoreToCome);
    }
    auto msgId = nextMessageId();
    auto future = _call(std::move(requestMsg), msgId, baton);
    auto logMetrics = [this, fromConnAcquiredTimer] {
        if (fromConnAcquiredTimer) {
            const auto timeElapsedMicros =
                durationCount<Microseconds>(fromConnAcquiredTimer.get()->elapsed());
            totalTimeForEgressConnectionAcquiredToWireMicros.increment(timeElapsedMicros);

            if ((!gEnableDetailedConnectionHealthMetricLogLines || timeElapsedMicros < 1000) &&
                !MONGO_unlikely(alwaysLogConnAcquisitionToWireTime.shouldFail())) {
                return;
            }

            // Log slow acquisition times at info level but rate limit it to prevent spamming
            // users.
            static auto& logSeverity = *new logv2::SeveritySuppressor{
                Seconds{1}, logv2::LogSeverity::Info(), logv2::LogSeverity::Debug(2)};
            LOGV2_DEBUG(6496702,
                        logSeverity().toInt(),
                        "Acquired connection for remote operation and completed writing to wire",
                        "durationMicros"_attr = timeElapsedMicros);
        }
    };

    if (fireAndForget) {
        return std::move(future).then([msgId, logMetrics, this]() -> Future<rpc::UniqueReply> {
            logMetrics();
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
        .then([msgId, logMetrics, baton, this]() {
            logMetrics();
            return _waitForResponse(msgId, baton);
        })
        .then([this](Message response) -> Future<rpc::UniqueReply> {
            return rpc::UniqueReply(response, rpc::makeReply(&response));
        });
}

Future<executor::RemoteCommandResponse> AsyncDBClient::runCommandRequest(
    executor::RemoteCommandRequest request,
    const BatonHandle& baton,
    boost::optional<std::shared_ptr<Timer>> fromConnAcquiredTimer) {
    auto startTimer = Timer();
    auto opMsgRequest = OpMsgRequest::fromDBAndBody(
        std::move(request.dbname), std::move(request.cmdObj), std::move(request.metadata));
    opMsgRequest.validatedTenancyScope = request.validatedTenancyScope;
    return runCommand(
               std::move(opMsgRequest), baton, request.options.fireAndForget, fromConnAcquiredTimer)
        .then([this, startTimer = std::move(startTimer)](rpc::UniqueReply response) {
            return executor::RemoteCommandResponse(*response, startTimer.elapsed());
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
    auto requestMsg = request.serialize();
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
    opMsgRequest.validatedTenancyScope = request.validatedTenancyScope;

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
