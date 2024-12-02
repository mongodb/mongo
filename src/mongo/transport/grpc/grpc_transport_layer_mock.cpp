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

#include "mongo/transport/grpc/grpc_transport_layer_mock.h"

#include "mongo/base/error_codes.h"
#include "mongo/platform/random.h"
#include "mongo/transport/grpc/test_fixtures.h"
#include "mongo/util/duration.h"

namespace mongo::transport::grpc {

GRPCTransportLayerMock::GRPCTransportLayerMock(ServiceContext* svcCtx,
                                               GRPCTransportLayer::Options options,
                                               MockClient::MockResolver resolver,
                                               const HostAndPort& mockClientAddress)
    : _svcCtx{std::move(svcCtx)},
      _options{std::move(options)},
      _reactor{std::make_shared<GRPCReactor>()},
      _resolver{std::move(resolver)},
      _mockClientAddress{std::move(mockClientAddress)} {}

Status GRPCTransportLayerMock::registerService(std::unique_ptr<Service> svc) {
    if (_startupState.load() != StartupState::kNotStarted) {
        return {ErrorCodes::AlreadyInitialized,
                "registerService can only be called before setup()"};
    }
    return Status::OK();
}

Status GRPCTransportLayerMock::setup() {
    auto oldState = StartupState::kNotStarted;
    if (!_startupState.compareAndSwap(&oldState, StartupState::kSetup)) {
        switch (oldState) {
            case StartupState::kShutDown:
                return TransportLayer::ShutdownStatus;
            case StartupState::kSetup:
            case StartupState::kStarted:
                return {ErrorCodes::AlreadyInitialized,
                        "setup() must be called only once and before start()"};
            case StartupState::kNotStarted:
                MONGO_UNREACHABLE
        }
    }

    if (_options.enableEgress) {
        _client = std::make_shared<MockClient>(this,
                                               std::move(_mockClientAddress),
                                               std::move(_resolver),
                                               makeClientMetadataDocument());
    }

    if (_options.bindIpList.empty()) {
        _options.bindIpList.push_back("127.0.0.1");
    }

    return Status::OK();
}

Status GRPCTransportLayerMock::start() {
    auto oldState = StartupState::kSetup;
    if (!_startupState.compareAndSwap(&oldState, StartupState::kStarted)) {
        switch (oldState) {
            case StartupState::kShutDown:
                return TransportLayer::ShutdownStatus;
            case StartupState::kNotStarted:
                return {ErrorCodes::IllegalOperation, "Must call setup() before start()"};
            case StartupState::kStarted:
                return {ErrorCodes::AlreadyInitialized, "start() can only be invoked once"};
            case StartupState::kSetup:
                MONGO_UNREACHABLE
        }
    }

    if (_client) {
        _client->start(_svcCtx);
    }

    PseudoRandom _random(12);
    for (const auto& ip : _options.bindIpList) {
        auto port = _options.bindPort == 0 ? _random.nextInt32() : _options.bindPort;
        _listenAddresses.push_back(HostAndPort(ip, port));
    }
    return Status::OK();
}

void GRPCTransportLayerMock::shutdown() {
    if (_startupState.swap(StartupState::kShutDown) == StartupState::kShutDown) {
        return;
    }
    if (_client) {
        _client->shutdown();
    }
}

StatusWith<std::shared_ptr<Session>> GRPCTransportLayerMock::connectWithAuthToken(
    HostAndPort peer,
    ConnectSSLMode sslMode,
    Milliseconds timeout,
    boost::optional<std::string> authToken) {
    if (!_client) {
        return Status(
            ErrorCodes::IllegalOperation,
            "start() must have been called with useEgress = true before attempting to connect");
    }
    return _client->connect(
        std::move(peer), _reactor, std::move(timeout), {std::move(authToken), sslMode});
}

StatusWith<std::shared_ptr<Session>> GRPCTransportLayerMock::connect(
    HostAndPort peer,
    ConnectSSLMode sslMode,
    Milliseconds timeout,
    const boost::optional<TransientSSLParams>& transientSSLParams) {
    return connectWithAuthToken(std::move(peer), sslMode, std::move(timeout));
}

const std::vector<HostAndPort>& GRPCTransportLayerMock::getListeningAddresses() const {
    auto state = _startupState.load();
    invariant(state != StartupState::kNotStarted && state != StartupState::kSetup);

    return _listenAddresses;
}

}  // namespace mongo::transport::grpc
