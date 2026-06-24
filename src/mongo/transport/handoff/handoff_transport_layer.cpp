/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/transport/handoff/handoff_transport_layer.h"

#include "mongo/logv2/log.h"
#include "mongo/transport/handoff/handoff_s2n_init.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

#include <memory>

#include <s2n.h>

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::transport {

using ListeningSocket = HandoffListenerThread::ListeningSocket;

HandoffTransportLayer::HandoffTransportLayer(Params params)
    : _posix(params.posix ? std::move(params.posix) : std::make_unique<POSIXInterface>()),
      _sessionManager(std::move(params.sessionManager)),
      _s2nConfig(nullptr),  // initialized in `setup()`
      _priorityListenerThread(HandoffListenerThread::Params{
          .threadName = "listenerForHandoffPriority",
          .sockets = {ListeningSocket{
              .path = params.socketPrefix /
                  fmt::format("handoff-mongodb-priority-{}.sock", params.port),
              .isLoadBalanced = false,
              .isPriority = true}},
          .socketGroupID = params.socketGroupID,
          .listenBacklog = params.listenBacklog,
          .sessionManager = _sessionManager.get(),
          .transportLayer = this,
          .posix = *_posix}),
      _standardListenerThread(HandoffListenerThread::Params{
          .threadName = "listenerForHandoffStandard",
          .sockets = {ListeningSocket{.path = params.socketPrefix /
                                          fmt::format("handoff-mongodb-{}.sock", params.port),
                                      .isLoadBalanced = false,
                                      .isPriority = false},
                      ListeningSocket{
                          .path = params.socketPrefix /
                              fmt::format("handoff-mongodb-load-balanced-{}.sock", params.port),
                          .isLoadBalanced = true,
                          .isPriority = false}},
          .socketGroupID = params.socketGroupID,
          .listenBacklog = params.listenBacklog,
          .sessionManager = _sessionManager.get(),
          .transportLayer = this,
          .posix = *_posix}) {
    invariant(_posix);
    invariant(_sessionManager);
    invariant(params.listenBacklog > 0);
}

HandoffTransportLayer::~HandoffTransportLayer() {
    if (_s2nConfig) {
        (void)s2n_config_free(_s2nConfig);
    }
}

StatusWith<std::shared_ptr<Session>> HandoffTransportLayer::connect(
    HostAndPort, ConnectSSLMode, Milliseconds, const boost::optional<TransientSSLParams>&) {
    return Status(ErrorCodes::IllegalOperation, "HandoffTransportLayer is ingress-only");
}

Future<std::shared_ptr<Session>> HandoffTransportLayer::asyncConnect(
    HostAndPort,
    ConnectSSLMode,
    const ReactorHandle&,
    Milliseconds,
    std::shared_ptr<ConnectionMetrics>,
    std::shared_ptr<const SSLConnectionContext>) {
    return Status(ErrorCodes::IllegalOperation, "HandoffTransportLayer is ingress-only");
}

Status HandoffTransportLayer::setup() {
    // s2n-tls configuration related setup
    if (auto status = s2nInitOnce(); !status.isOK()) {
        return status;
    }
    s2n_config* const s2nConfig = s2n_config_new_minimal();
    if (!s2nConfig) {
        return Status(
            ErrorCodes::InternalError,
            fmt::format("s2n_config_new_minimal failed: {}", s2n_strerror(s2n_errno, "EN")));
    }

    _s2nConfig = s2nConfig;
    ScopeGuard cleanupOnFailure([this]() {
        (void)s2n_config_free(_s2nConfig);
        _s2nConfig = nullptr;
    });

    if (s2n_config_set_serialization_version(_s2nConfig, S2N_SERIALIZED_CONN_V1) != S2N_SUCCESS) {
        return Status(ErrorCodes::InternalError,
                      fmt::format("s2n_config_set_serialization_version failed: {}",
                                  s2n_strerror(s2n_errno, "EN")));
    }

    Status status = _priorityListenerThread.setup({.s2nConfig = _s2nConfig});
    if (!status.isOK()) {
        return status;
    }
    status = _standardListenerThread.setup({.s2nConfig = _s2nConfig});
    if (!status.isOK()) {
        _priorityListenerThread.shutdown();
        return status;
    }

    cleanupOnFailure.dismiss();
    return Status::OK();
}

Status HandoffTransportLayer::start() {
    ScopeGuard cleanupOnError([this]() {
        _priorityListenerThread.shutdown();
        _standardListenerThread.shutdown();
    });

    Status status = _priorityListenerThread.listen();
    if (!status.isOK()) {
        return status;
    }
    status = _standardListenerThread.listen();
    if (!status.isOK()) {
        return status;
    }

    _priorityListenerThread.start();
    _standardListenerThread.start();
    cleanupOnError.dismiss();
    return status;
}

void HandoffTransportLayer::stopAcceptingSessions() {
    _priorityListenerThread.stopAcceptingSessions();
    _standardListenerThread.stopAcceptingSessions();
}

void HandoffTransportLayer::shutdown() {
    stopAcceptingSessions();

    _priorityListenerThread.shutdown();
    _standardListenerThread.shutdown();

    const Seconds kSessionShutdownTimeout{10};
    if (!_sessionManager->shutdown(kSessionShutdownTimeout)) {
        LOGV2(12779307,
              "HandoffTransportLayer: SessionManager did not shut down within the time limit",
              "timeout"_attr = kSessionShutdownTimeout);
    }

    LOGV2(12779303, "HandoffTransportLayer shut down");
}

StringData HandoffTransportLayer::getNameForLogging() const {
    return "HandoffTransportLayer"_sd;
}

ReactorHandle HandoffTransportLayer::getReactor(WhichReactor) {
    return nullptr;
}

TransportProtocol HandoffTransportLayer::getTransportProtocol() const {
    return TransportProtocol::MongoRPC;
}

SessionManager* HandoffTransportLayer::getSessionManager() const {
    return _sessionManager.get();
}

std::shared_ptr<SessionManager> HandoffTransportLayer::getSharedSessionManager() const {
    return _sessionManager;
}

bool HandoffTransportLayer::isIngress() const {
    return true;
}

bool HandoffTransportLayer::isEgress() const {
    return false;
}

#ifdef MONGO_CONFIG_SSL
Status HandoffTransportLayer::rotateCertificates(std::shared_ptr<SSLManagerInterface>, bool) {
    // Certification rotation is not applicable to this transport layer, but the transport layer
    // manager will call this function, so return success.
    return Status::OK();
}

StatusWith<std::shared_ptr<const SSLConnectionContext>>
HandoffTransportLayer::createTransientSSLContext(const TransientSSLParams&) {
    return Status(ErrorCodes::IllegalOperation,
                  "HandoffTransportLayer does not support transient SSL contexts");
}
#endif

}  // namespace mongo::transport
