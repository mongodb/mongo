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

#pragma once

#include <memory>

#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/transport/grpc/grpc_transport_layer.h"
#include "mongo/transport/grpc/mock_client.h"
#include "mongo/transport/grpc/reactor.h"

namespace mongo::transport::grpc {

class Service;

/**
 * Currently only mocks the egress portion of GRPCTransportLayer.
 *
 * setup() must be called exactly once before start(), which also can only be called exactly once.
 * Neither of these methods are thread-safe.
 */
class GRPCTransportLayerMock : public GRPCTransportLayer {
public:
    GRPCTransportLayerMock(ServiceContext* svcCtx,
                           Options options,
                           MockClient::MockResolver resolver,
                           const HostAndPort& mockClientAddress);

    Status registerService(std::unique_ptr<Service> svc) override;

    Status setup() override;

    Status start() override;

    void shutdown() override;

    void stopAcceptingSessions() override {
        MONGO_UNIMPLEMENTED;
    }

    StatusWith<std::shared_ptr<Session>> connectWithAuthToken(
        HostAndPort peer,
        Milliseconds timeout,
        boost::optional<std::string> authToken = boost::none) override;

    StatusWith<std::shared_ptr<Session>> connect(
        HostAndPort peer,
        ConnectSSLMode sslMode,
        Milliseconds timeout,
        const boost::optional<TransientSSLParams>& transientSSLParams) override;

#ifdef MONGO_CONFIG_SSL
    Status rotateCertificates(std::shared_ptr<SSLManagerInterface> manager, bool asyncOCSPStaple) {
        MONGO_UNIMPLEMENTED;
    };
#endif

    const std::vector<HostAndPort>& getListeningAddresses() const override;

    SessionManager* getSessionManager() const override {
        return nullptr;
    }

    std::shared_ptr<SessionManager> getSharedSessionManager() const override {
        return {};
    }

    ReactorHandle getReactor(WhichReactor which) override {
        return _reactor;
    }

private:
    enum class StartupState { kNotStarted, kSetup, kStarted, kShutDown };

    AtomicWord<StartupState> _startupState;

    std::vector<HostAndPort> _listenAddresses;
    std::shared_ptr<Client> _client;
    ServiceContext* const _svcCtx;
    Options _options;
    // This reactor is used to produce CompletionQueueEntry tags for the mocks that don't use the
    // completion queue, but it does not need to be run.
    std::shared_ptr<GRPCReactor> _reactor;

    // Invalidated after setup().
    MockClient::MockResolver _resolver;
    const HostAndPort _mockClientAddress;
};

}  // namespace mongo::transport::grpc
