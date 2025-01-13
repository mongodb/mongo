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

#include "mongo/transport/grpc/grpc_transport_layer_impl.h"

#include "mongo/db/commands/server_status.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/grpc/client.h"
#include "mongo/transport/grpc/grpc_session_manager.h"
#include "mongo/transport/grpc/service.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/net/ssl_options.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::transport::grpc {
namespace {
const Seconds kSessionManagerShutdownTimeout{10};

inline std::string makeGRPCUnixSockPath(int port) {
    return makeUnixSockPath(port, "grpc");
}

class GRPCSection : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement&) const override {
        BSONObjBuilder section;
        auto tl = opCtx->getServiceContext()->getTransportLayerManager()->getTransportLayer(
            TransportProtocol::GRPC);

        if (!tl) {
            return section.obj();
        }

        if (tl->isIngress()) {
            BSONObjBuilder ingressSection(section.subobjStart("ingress"_sd));
            if (auto sm = dynamic_cast<GRPCSessionManager*>(tl->getSessionManager())) {
                sm->appendStats(&ingressSection);
            };
        }

        if (tl->isEgress()) {
            BSONObjBuilder egressSection(section.subobjStart("egress"));
            tl->appendStatsForServerStatus(&egressSection);
        }

        return section.obj();
    }
};

auto& grpcSection = *ServerStatusSectionBuilder<GRPCSection>("gRPC").forShard().forRouter();

}  // namespace

GRPCTransportLayerImpl::Options::Options(const ServerGlobalParams& params) {
    bindIpList = params.bind_ips;
    bindPort = params.grpcPort;
    useUnixDomainSockets = !params.noUnixSocket;
    unixDomainSocketPermissions = params.unixSocketPermissions;
    maxServerThreads = params.grpcServerMaxThreads;
}

GRPCTransportLayerImpl::GRPCTransportLayerImpl(ServiceContext* svcCtx,
                                               Options options,
                                               std::unique_ptr<SessionManager> sm)
    : _svcCtx{svcCtx},
      _egressReactor(std::make_shared<GRPCReactor>()),
      _options{std::move(options)},
      _sessionManager(std::move(sm)) {}

std::unique_ptr<GRPCTransportLayerImpl> GRPCTransportLayerImpl::createWithConfig(
    ServiceContext* svcCtx,
    Options options,
    std::vector<std::shared_ptr<ClientTransportObserver>> observers) {

    auto clientCache = std::make_shared<ClientCache>();

    auto sm = options.enableIngress
        ? std::make_unique<GRPCSessionManager>(svcCtx, clientCache, std::move(observers))
        : nullptr;

    auto tl = std::make_unique<GRPCTransportLayerImpl>(svcCtx, std::move(options), std::move(sm));

    if (options.enableIngress) {
        uassertStatusOK(tl->registerService(std::make_unique<CommandService>(
            tl.get(),
            [tlPtr = tl.get()](auto session) {
                invariant(session->getTransportLayer() == tlPtr);
                tlPtr->getSessionManager()->startSession(std::move(session));
            },
            std::make_shared<grpc::WireVersionProvider>(),
            std::move(clientCache))));
    }

    return tl;
}

Status GRPCTransportLayerImpl::registerService(std::unique_ptr<Service> svc) {
    try {
        stdx::lock_guard lk(_mutex);
        invariant(
            !_server,
            "Cannot register gRPC services after GRPCTransportLayer::setup() has been invoked");
        iassert(TransportLayer::ShutdownStatus.code(),
                "Cannot register gRPC services after the GRPCTransportLayer has been shut down",
                !_isShutdown);
        _services.push_back(std::move(svc));
        return Status::OK();
    } catch (const DBException& e) {
        return e.toStatus();
    }
}

Status GRPCTransportLayerImpl::setup() {
    try {
        stdx::lock_guard lk(_mutex);
        iassert(TransportLayer::ShutdownStatus.code(),
                "Cannot set up GRPCTransportLayer after it has been shut down",
                !_isShutdown);

        if (!_services.empty()) {
            std::vector<std::unique_ptr<Service>> services;
            services.swap(_services);
            Server::Options serverOptions;
            if (_options.bindIpList.empty()) {
                _options.bindIpList.push_back("127.0.0.1");
            }
            std::vector<HostAndPort> addresses;
            for (auto& ip : _options.bindIpList) {
                addresses.push_back(HostAndPort(ip, _options.bindPort));
            }
            if (_options.useUnixDomainSockets) {
                addresses.push_back(HostAndPort(makeGRPCUnixSockPath(_options.bindPort)));
            }
            serverOptions.addresses = std::move(addresses);
            serverOptions.maxThreads = _options.maxServerThreads;

            uassert(ErrorCodes::InvalidOptions,
                    "Unable to start GRPC transport for ingress without tlsCertificateKeyFile",
                    !sslGlobalParams.sslPEMKeyFile.empty());
            serverOptions.tlsCertificateKeyFile = sslGlobalParams.sslPEMKeyFile;

            if (!sslGlobalParams.sslCAFile.empty()) {
                serverOptions.tlsCAFile = sslGlobalParams.sslCAFile;
            }

            serverOptions.tlsAllowInvalidCertificates = sslGlobalParams.sslAllowInvalidCertificates;
            serverOptions.tlsAllowConnectionsWithoutCertificates =
                sslGlobalParams.sslWeakCertificateValidation;

            uassert(ErrorCodes::InvalidOptions,
                    "Unable to start GRPCTransportLayerImpl for ingress without SessionManager",
                    _sessionManager);

            _server = std::make_unique<Server>(std::move(services), serverOptions);
        }
        if (_options.enableEgress) {
            GRPCClient::Options clientOptions{};

            if (!sslGlobalParams.sslCAFile.empty()) {
                clientOptions.tlsCAFile = sslGlobalParams.sslCAFile;
            }
            if (!sslGlobalParams.sslPEMKeyFile.empty()) {
                clientOptions.tlsCertificateKeyFile = sslGlobalParams.sslPEMKeyFile;
            }
            clientOptions.tlsAllowInvalidHostnames = sslGlobalParams.sslAllowInvalidHostnames;
            clientOptions.tlsAllowInvalidCertificates = sslGlobalParams.sslAllowInvalidCertificates;
            iassert(ErrorCodes::InvalidOptions,
                    "gRPC egress networking enabled but no client metadata document was provided",
                    _options.clientMetadata.has_value());
            _client = std::make_shared<GRPCClient>(
                this, _svcCtx, *_options.clientMetadata, std::move(clientOptions));
        }

        iassert(ErrorCodes::InvalidOptions,
                "gRPC cannot be setup with both ingress and egress disabled",
                _client || _server);

        return Status::OK();
    } catch (const DBException& e) {
        return e.toStatus();
    }
}

Status GRPCTransportLayerImpl::start() {
    try {
        stdx::lock_guard lk(_mutex);
        iassert(TransportLayer::ShutdownStatus.code(),
                "Cannot start GRPCTransportLayer after it has been shut down",
                !_isShutdown);
        iassert(ErrorCodes::NotYetInitialized,
                "gRPC networking has not been setup yet",
                _client || _server);

        // TODO SERVER-97619: Depending on the resolution of SERVER-97619, we may need to move the
        // log and uasserts out of this block.
        if (_server) {
            // We can't distinguish between the default list of disabled protocols and one specified
            // by via options, so we just log that the list is being ignored when using gRPC.
            if (!sslGlobalParams.sslDisabledProtocols.empty()) {
                LOGV2_DEBUG(8000811,
                            3,
                            "Ignoring tlsDisabledProtocols for gRPC-based connections",
                            "tlsDisabledProtocols"_attr = sslGlobalParams.sslDisabledProtocols);
            }
            uassert(ErrorCodes::InvalidSSLConfiguration,
                    "Specifying a CRL file is not supported when gRPC mode is enabled",
                    sslGlobalParams.sslCRLFile.empty());
            uassert(ErrorCodes::InvalidSSLConfiguration,
                    "Certificate passwords are not supported when gRPC mode is enabled",
                    sslGlobalParams.sslPEMKeyPassword.empty());
            uassert(ErrorCodes::InvalidSSLConfiguration,
                    "tlsFIPSMode is not supported when gRPC mode is enabled",
                    !sslGlobalParams.sslFIPSMode);

            invariant(_sessionManager);
            _server->start();
            if (_options.useUnixDomainSockets) {
                setUnixDomainSocketPermissions(makeGRPCUnixSockPath(_options.bindPort),
                                               _options.unixDomainSocketPermissions);
            }
        }
        if (_client) {
            _ioThread = stdx::thread([this] {
                setThreadName("GRPCDefaultEgressReactor");
                LOGV2_DEBUG(9715105, 2, "Starting the default egress gRPC reactor");
                _egressReactor->run();
                LOGV2_DEBUG(9715106, 2, "Draining the default egress gRPC reactor");
                _egressReactor->drain();
                LOGV2_DEBUG(9715107, 2, "Finished drain of the default egress gRPC reactor");
            });
            _client->start();
        }
        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

StatusWith<std::shared_ptr<Session>> GRPCTransportLayerImpl::connectWithAuthToken(
    HostAndPort peer,
    ConnectSSLMode sslMode,
    Milliseconds timeout,
    boost::optional<std::string> authToken) {
    invariant(_client);
    return _client
        ->connect(std::move(peer), _egressReactor, timeout, {std::move(authToken), sslMode})
        .getNoThrow();
}

StatusWith<std::shared_ptr<Session>> GRPCTransportLayerImpl::connect(
    HostAndPort peer,
    ConnectSSLMode sslMode,
    Milliseconds timeout,
    const boost::optional<TransientSSLParams>& transientSSLParams) {
    if (transientSSLParams) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      "Transient SSL parameters are not supported when using gRPC");
    }
    return connectWithAuthToken(std::move(peer), sslMode, timeout);
}

Future<std::shared_ptr<Session>> GRPCTransportLayerImpl::asyncConnectWithAuthToken(
    HostAndPort peer,
    ConnectSSLMode sslMode,
    const ReactorHandle& reactor,
    Milliseconds timeout,
    std::shared_ptr<ConnectionMetrics> connectionMetrics,
    boost::optional<std::string> authToken) {
    invariant(_client);
    return _client
        ->connect(std::move(peer),
                  checked_pointer_cast<GRPCReactor>(reactor),
                  timeout,
                  {std::move(authToken), sslMode},
                  connectionMetrics)
        .then([](std::shared_ptr<EgressSession> egressSession) -> std::shared_ptr<Session> {
            return egressSession;
        });
}

Future<std::shared_ptr<Session>> GRPCTransportLayerImpl::asyncConnect(
    HostAndPort peer,
    ConnectSSLMode sslMode,
    const ReactorHandle& reactor,
    Milliseconds timeout,
    std::shared_ptr<ConnectionMetrics> connectionMetrics,
    std::shared_ptr<const SSLConnectionContext> transientSSLContext) {
    if (transientSSLContext) {
        return Future<std::shared_ptr<Session>>::makeReady(
            Status(ErrorCodes::InvalidSSLConfiguration,
                   "Transient SSL parameters are not supported when using gRPC"));
    }
    return asyncConnectWithAuthToken(std::move(peer), sslMode, reactor, timeout, connectionMetrics);
}

void GRPCTransportLayerImpl::shutdown() {
    stdx::lock_guard lk(_mutex);
    if (std::exchange(_isShutdown, true)) {
        return;
    }

    if (_server) {
        _server->shutdown();
    }
    if (_client) {
        _client->shutdown();
    }
    if (_ioThread.joinable()) {
        LOGV2_DEBUG(9715108, 2, "Stopping default egress gRPC reactor");
        _egressReactor->stop();
        LOGV2_DEBUG(9715109, 2, "Joining the default egress gRPC reactor thread");
        _ioThread.join();
    }

    if (_sessionManager) {
        _sessionManager->shutdown(kSessionManagerShutdownTimeout);
    }
}

void GRPCTransportLayerImpl::stopAcceptingSessions() {
    if (_server) {
        _server->stopAcceptingRequests();
    }
}

#ifdef MONGO_CONFIG_SSL
Status GRPCTransportLayerImpl::rotateCertificates(std::shared_ptr<SSLManagerInterface> manager,
                                                  bool asyncOCSPStaple) {
    if (_server) {
        if (auto status = _server->rotateCertificates(); !status.isOK()) {
            // Log at debug level since this is logged at a higher level in TransportLayerManager.
            LOGV2_DEBUG(9886802,
                        1,
                        "Failed to rotate ingress gRPC TLS certificates",
                        "error"_attr = status);
            return status;
        }
    }

    if (_client) {
        if (auto status = _client->rotateCertificates(); !status.isOK()) {
            LOGV2_DEBUG(
                9886803, 1, "Failed to rotate egress gRPC TLS certificates", "error"_attr = status);
            return status;
        }
    }

    LOGV2_DEBUG(9886804, 1, "gRPC TLS certificates successfully rotated");
    return Status::OK();
}
#endif

const std::vector<HostAndPort>& GRPCTransportLayerImpl::getListeningAddresses() const {
    invariant(_server);
    return _server->getListeningAddresses();
}

}  // namespace mongo::transport::grpc
