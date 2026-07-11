// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
        _client = std::make_shared<MockClient>(
            this, _svcCtx, _mockClientAddress, _resolver, makeClientMetadataDocument());
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
        _ioThread = stdx::thread([this] {
            setThreadName("MockGRPCTLEgressReactor");
            _reactor->run();
            _reactor->drain();
        });
        _client->start();
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
        _reactor->stop();
        _ioThread.join();
    }
}

std::shared_ptr<Client> GRPCTransportLayerMock::createGRPCClient(BSONObj clientMetadata) {
    return std::make_shared<MockClient>(
        this, _svcCtx, _mockClientAddress, _resolver, clientMetadata);
}

StatusWith<std::shared_ptr<Session>> GRPCTransportLayerMock::connectWithAuthToken(
    HostAndPort peer,
    ConnectSSLMode sslMode,
    Milliseconds timeout,
    boost::optional<std::string> authToken) {
    return asyncConnectWithAuthToken(peer,
                                     sslMode,
                                     _reactor,
                                     timeout,
                                     nullptr,
                                     CancellationToken::uncancelable(),
                                     authToken)
        .getNoThrow();
}

StatusWith<std::shared_ptr<Session>> GRPCTransportLayerMock::connect(
    HostAndPort peer,
    ConnectSSLMode sslMode,
    Milliseconds timeout,
    const boost::optional<TransientSSLParams>& transientSSLParams) {
    return connectWithAuthToken(std::move(peer), sslMode, std::move(timeout));
}

Future<std::shared_ptr<Session>> GRPCTransportLayerMock::asyncConnectWithAuthToken(
    HostAndPort peer,
    ConnectSSLMode sslMode,
    const ReactorHandle& reactor,
    Milliseconds timeout,
    std::shared_ptr<ConnectionMetrics> connectionMetrics,
    const CancellationToken& token,
    boost::optional<std::string> authToken) {
    if (!_client) {
        return Status(
            ErrorCodes::IllegalOperation,
            "start() must have been called with useEgress = true before attempting to connect");
    }
    return _client
        ->connect(std::move(peer),
                  checked_pointer_cast<GRPCReactor>(reactor),
                  std::move(timeout),
                  {std::move(authToken), sslMode},
                  token,
                  connectionMetrics)
        .then([](std::shared_ptr<EgressSession> egressSession) -> std::shared_ptr<Session> {
            return egressSession;
        });
}

Future<std::shared_ptr<Session>> GRPCTransportLayerMock::asyncConnect(
    HostAndPort peer,
    ConnectSSLMode sslMode,
    const ReactorHandle& reactor,
    Milliseconds timeout,
    std::shared_ptr<ConnectionMetrics> connectionMetrics,
    std::shared_ptr<const SSLConnectionContext> transientSSLContext) {
    return asyncConnectWithAuthToken(peer, sslMode, reactor, timeout, connectionMetrics);
}

const std::vector<HostAndPort>& GRPCTransportLayerMock::getListeningAddresses() const {
    auto state = _startupState.load();
    invariant(state != StartupState::kNotStarted && state != StartupState::kSetup);

    return _listenAddresses;
}

}  // namespace mongo::transport::grpc
