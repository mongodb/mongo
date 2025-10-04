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

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/executor/connection_metrics.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/transport/session.h"
#include "mongo/transport/session_manager.h"
#include "mongo/transport/ssl_connection_context.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_peer_info.h"
#include "mongo/util/net/ssl_types.h"
#include "mongo/util/time_support.h"

#include <functional>
#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace transport {

/**
 * This TransportLayerMock is a noop TransportLayer implementation.
 */
class TransportLayerMock : public TransportLayer {
    TransportLayerMock(const TransportLayerMock&) = delete;
    TransportLayerMock& operator=(const TransportLayerMock&) = delete;

public:
    TransportLayerMock();
    explicit TransportLayerMock(std::unique_ptr<SessionManager> sm)
        : _sessionManager(std::move(sm)) {}
    ~TransportLayerMock() override;

    std::shared_ptr<Session> createSession();
    std::shared_ptr<Session> get(Session::Id id);
    bool owns(Session::Id id);
    void deleteSession(Session::Id id);

    StatusWith<std::shared_ptr<Session>> connect(
        HostAndPort peer,
        ConnectSSLMode sslMode,
        Milliseconds timeout,
        const boost::optional<TransientSSLParams>& transientSSLParams) override;
    Future<std::shared_ptr<Session>> asyncConnect(
        HostAndPort peer,
        ConnectSSLMode sslMode,
        const ReactorHandle& reactor,
        Milliseconds timeout,
        std::shared_ptr<ConnectionMetrics> connectionMetrics,
        std::shared_ptr<const SSLConnectionContext> transientSSLContext = nullptr) override;

    Status setup() override;
    Status start() override;
    void shutdown() override;
    bool inShutdown() const;
    void stopAcceptingSessions() override {
        MONGO_UNIMPLEMENTED;
    }

    StringData getNameForLogging() const override {
        return "mock"_sd;
    }

    TransportProtocol getTransportProtocol() const override {
        MONGO_UNIMPLEMENTED;
    }

    ReactorHandle getReactor(WhichReactor which) override;

    // Set to a factory function to use your own session type.
    std::function<std::shared_ptr<Session>(TransportLayer*)> createSessionHook;

#ifdef MONGO_CONFIG_SSL
    Status rotateCertificates(std::shared_ptr<SSLManagerInterface> manager,
                              bool asyncOCSPStaple) override {
        return Status::OK();
    }

    StatusWith<std::shared_ptr<const transport::SSLConnectionContext>> createTransientSSLContext(
        const TransientSSLParams& transientSSLParams) override;
#endif

    SessionManager* getSessionManager() const override {
        return _sessionManager.get();
    }

    std::shared_ptr<SessionManager> getSharedSessionManager() const override {
        return _sessionManager;
    }

    bool isIngress() const override {
        return false;
    }

    bool isEgress() const override {
        return false;
    }

private:
    friend class MockSession;

    struct Connection {
        bool ended;
        std::shared_ptr<Session> session;
        SSLPeerInfo peerInfo;
    };
    stdx::unordered_map<Session::Id, Connection> _sessions;
    bool _shutdown = false;

    std::shared_ptr<SessionManager> _sessionManager;
};

}  // namespace transport
}  // namespace mongo
