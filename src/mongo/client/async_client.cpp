/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/client/async_client.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/authenticate.h"
#include "mongo/config.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/internal_user_auth.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/egress_tag_closer_manager.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/legacy_request_builder.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
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

BSONObj AsyncDBClient::_buildIsMasterRequest(const std::string& appName) {
    BSONObjBuilder bob;

    bob.append("isMaster", 1);
    bob.append("hangUpOnStepDown", false);
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

    return bob.obj();
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
        warning() << "remote host has incompatible wire version: " << validateStatus;
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

Future<void> AsyncDBClient::authenticate(const BSONObj& params) {
    // This check is sufficient to see if auth is enabled on the system,
    // and avoids creating dependencies on deeper, less accessible auth code.
    if (!isInternalAuthSet()) {
        return Future<void>::makeReady();
    }

    // We will only have a valid clientName if SSL is enabled.
    std::string clientName;
#ifdef MONGO_CONFIG_SSL
    if (getSSLManager()) {
        clientName = getSSLManager()->getSSLConfiguration().clientSubjectName.toString();
    }
#endif

    auto pf = makePromiseFuture<void>();
    auto authCompleteCb = [promise = pf.promise.share()](auth::AuthResponse response) mutable {
        if (response.isOK()) {
            promise.emplaceValue();
        } else {
            promise.setError(response.status);
        }
    };

    auto doAuthCb = [this](executor::RemoteCommandRequest request,
                           auth::AuthCompletionHandler handler) {

        runCommandRequest(request).getAsync([handler = std::move(handler)](
            StatusWith<executor::RemoteCommandResponse> response) {
            if (!response.isOK()) {
                handler(executor::RemoteCommandResponse(response.getStatus()));
            } else {
                handler(std::move(response.getValue()));
            }
        });
    };

    auth::authenticateClient(
        params, remote(), clientName, std::move(doAuthCb), std::move(authCompleteCb));

    return std::move(pf.future);
}

Future<void> AsyncDBClient::initWireVersion(const std::string& appName,
                                            executor::NetworkConnectionHook* const hook) {
    auto requestObj = _buildIsMasterRequest(appName);
    // We use a legacy request to create our ismaster request because we may
    // have to communicate with servers that do not support other protocols.
    auto requestMsg =
        rpc::legacyRequestFromOpMsgRequest(OpMsgRequest::fromDBAndBody("admin", requestObj));
    auto clkSource = _svcCtx->getFastClockSource();
    auto start = clkSource->now();

    return _call(requestMsg).then([this, requestObj, hook, clkSource, start](Message response) {
        auto cmdReply = rpc::makeReply(&response);
        if (hook) {
            auto millis = duration_cast<Milliseconds>(clkSource->now() - start);
            executor::RemoteCommandResponse cmdResp(*cmdReply, millis);
            uassertStatusOK(hook->validateHost(_peer, requestObj, std::move(cmdResp)));
        }
        _parseIsMasterResponse(requestObj, cmdReply);
    });
}

Future<Message> AsyncDBClient::_call(Message request, const transport::BatonHandle& baton) {
    auto swm = _compressorManager.compressMessage(request);
    if (!swm.isOK()) {
        return swm.getStatus();
    }

    request = std::move(swm.getValue());
    auto msgId = nextMessageId();
    request.header().setId(msgId);
    request.header().setResponseToMsgId(0);

    return _session->asyncSinkMessage(request, baton)
        .then([this, baton] { return _session->asyncSourceMessage(baton); })
        .then([this, msgId](Message response) -> StatusWith<Message> {
            uassert(50787,
                    "ResponseId did not match sent message ID.",
                    response.header().getResponseToMsgId() == msgId);

            if (response.operation() == dbCompressed) {
                return _compressorManager.decompressMessage(response);
            } else {
                return response;
            }
        });
}

Future<rpc::UniqueReply> AsyncDBClient::runCommand(OpMsgRequest request,
                                                   const transport::BatonHandle& baton) {
    invariant(_negotiatedProtocol);
    auto requestMsg = rpc::messageFromOpMsgRequest(*_negotiatedProtocol, std::move(request));
    return _call(std::move(requestMsg), baton)
        .then([this](Message response) -> Future<rpc::UniqueReply> {
            return rpc::UniqueReply(response, rpc::makeReply(&response));
        });
}

Future<executor::RemoteCommandResponse> AsyncDBClient::runCommandRequest(
    executor::RemoteCommandRequest request, const transport::BatonHandle& baton) {
    auto clkSource = _svcCtx->getPreciseClockSource();
    auto start = clkSource->now();
    auto opMsgRequest = OpMsgRequest::fromDBAndBody(
        std::move(request.dbname), std::move(request.cmdObj), std::move(request.metadata));
    return runCommand(std::move(opMsgRequest), baton)
        .then([start, clkSource, this](rpc::UniqueReply response) {
            auto duration = duration_cast<Milliseconds>(clkSource->now() - start);
            return executor::RemoteCommandResponse(*response, duration);
        })
        .onError([start, clkSource](Status status) {
            auto duration = duration_cast<Milliseconds>(clkSource->now() - start);
            return executor::RemoteCommandResponse(status, duration);
        });
}

void AsyncDBClient::cancel(const transport::BatonHandle& baton) {
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
