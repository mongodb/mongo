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

#include "mongo/transport/grpc/client.h"

#include <grpcpp/channel.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/security/tls_certificate_provider.h>
#include <grpcpp/security/tls_certificate_verifier.h>
#include <grpcpp/security/tls_credentials_options.h>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/grpc/channel_pool.h"
#include "mongo/transport/grpc/client_stream.h"
#include "mongo/transport/grpc/grpc_client_context.h"
#include "mongo/transport/grpc/grpc_client_stream.h"
#include "mongo/transport/grpc/grpc_session.h"
#include "mongo/transport/grpc/util.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/ssl_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::transport::grpc {
namespace {
inline Status makeShutdownTerminationStatus() {
    return {ErrorCodes::ShutdownInProgress, "gRPC client is shutting down"};
}
}  // namespace

Client::Client(TransportLayer* tl, const BSONObj& clientMetadata)
    : _tl(tl),
      _id(UUID::gen()),
      _clientMetadata(base64::encode(clientMetadata.objdata(), clientMetadata.objsize())),
      _sharedState(std::make_shared<EgressSession::SharedState>()) {
    _sharedState->clusterMaxWireVersion.store(util::constants::kMinimumWireVersion);
}

void Client::start(ServiceContext*) {
    stdx::lock_guard lk(_mutex);
    invariant(std::exchange(_state, ClientState::kStarted) == ClientState::kUninitialized,
              "Cannot start a gRPC client more than once");
}

void Client::shutdown() {
    decltype(_sessions) sessions;
    {
        stdx::lock_guard lk(_mutex);
        invariant(std::exchange(_state, ClientState::kShutdown) != ClientState::kShutdown,
                  "Cannot shut down a gRPC client more than once");
        sessions = _sessions;
    }

    size_t terminated = 0;
    const auto shutdownStatus = makeShutdownTerminationStatus();
    for (auto& ptr : sessions) {
        if (auto session = ptr.lock()) {
            terminated++;
            session->cancel(shutdownStatus);
        }
    }

    stdx::unique_lock lk(_mutex);
    _shutdownCV.wait(lk, [&]() { return _isShutdownComplete_inlock(); });

    LOGV2_DEBUG(7401601,
                1,
                "Shutting down gRPC client",
                "id"_attr = id(),
                "terminatedSessions"_attr = terminated);
}

int Client::getClusterMaxWireVersion() const {
    return _sharedState->clusterMaxWireVersion.load();
}

void Client::setMetadataOnClientContext(ClientContext& ctx, const ConnectOptions& options) {
    if (options.authToken) {
        ctx.addMetadataEntry(util::constants::kAuthenticationTokenKey.toString(),
                             *options.authToken);
    }
    ctx.addMetadataEntry(util::constants::kClientMetadataKey.toString(), _clientMetadata);
    ctx.addMetadataEntry(util::constants::kClientIdKey.toString(), _id.toString());
    ctx.addMetadataEntry(util::constants::kWireVersionKey.toString(),
                         std::to_string(getClusterMaxWireVersion()));
}

bool Client::_isShutdownComplete_inlock() {
    return _state == ClientState::kShutdown && _ongoingConnects == 0 && _sessions.empty();
}

std::shared_ptr<EgressSession> Client::connect(const HostAndPort& remote,
                                               Milliseconds timeout,
                                               ConnectOptions options) {
    // TODO: this implementation currently acquires _mutex twice, which will have negative
    // performance implications. Egress performance is not a priority at the moment, but we should
    // revisit how lock contention can be reduced here in the future.
    {
        std::lock_guard lk(_mutex);
        invariant(_state != ClientState::kUninitialized,
                  "Client must be started before connect can be called");
        if (_state == ClientState::kShutdown) {
            iasserted(makeShutdownTerminationStatus());
        }
        _ongoingConnects++;
    }

    ON_BLOCK_EXIT([&] {
        stdx::lock_guard lk(_mutex);
        _ongoingConnects--;
        if (MONGO_unlikely(_isShutdownComplete_inlock())) {
            _shutdownCV.notify_one();
        }
    });

    auto [ctx, stream] = _streamFactory(remote, timeout, options);
    auto session =
        std::make_shared<EgressSession>(_tl, std::move(ctx), std::move(stream), _id, _sharedState);

    stdx::lock_guard lk(_mutex);
    if (MONGO_unlikely(_state == ClientState::kShutdown)) {
        auto status = makeShutdownTerminationStatus();
        session->cancel(status);
        iasserted(status);
    }

    auto it = _sessions.insert(_sessions.begin(), session);
    session->setCleanupCallback([this, client = weak_from_this(), it = std::move(it)](const auto&) {
        if (auto anchor = client.lock()) {
            stdx::lock_guard lk(_mutex);
            _sessions.erase(it);

            if (MONGO_unlikely(_isShutdownComplete_inlock())) {
                _shutdownCV.notify_one();
            }
        }
    });

    return session;
}

namespace {
/**
 * Periodically calls into a channel pool to drop idle channels. Internally, creates a periodic
 * task that drops all channels that have been idle for `kDefaultChannelTimeout`. Not
 * thread-safe.
 */
template <typename PoolType>
class ChannelPrunerService {
public:
    void start(ServiceContext* svcCtx, PoolType pool) {
        PeriodicRunner::PeriodicJob prunerJob(
            "GRPCIdleChannelPrunerJob",
            [pool](::mongo::Client*) { pool->dropIdleChannels(Client::kDefaultChannelTimeout); },
            Client::kDefaultChannelTimeout,
            // TODO(SERVER-74659): Please revisit if this periodic job could be made killable.
            false /*isKillableByStepdown*/);
        invariant(!_pruner);
        invariant(svcCtx->getPeriodicRunner() != nullptr);
        _pruner.emplace(svcCtx->getPeriodicRunner()->makeJob(std::move(prunerJob)));
        _pruner->start();
    }

    void stop() {
        if (_pruner) {
            _pruner->stop();
        }
    }

private:
    boost::optional<PeriodicRunner::JobAnchor> _pruner;
};
}  // namespace

class StubFactoryImpl : public GRPCClient::StubFactory {
    class Stub {
    public:
        using ReadMessageType = SharedBuffer;
        using WriteMessageType = ConstSharedBuffer;

        Stub(const std::shared_ptr<::grpc::Channel>& channel)
            : _channel(channel),
              _unauthenticatedCommandStreamMethod(
                  util::constants::kUnauthenticatedCommandStreamMethodName,
                  ::grpc::internal::RpcMethod::BIDI_STREAMING,
                  channel),
              _authenticatedCommandStreamMethod(
                  util::constants::kAuthenticatedCommandStreamMethodName,
                  ::grpc::internal::RpcMethod::BIDI_STREAMING,
                  channel) {}

        std::shared_ptr<ClientStream> authenticatedCommandStream(GRPCClientContext* context) {
            return _makeStream(_authenticatedCommandStreamMethod, context);
        }

        std::shared_ptr<ClientStream> unauthenticatedCommandStream(GRPCClientContext* context) {
            return _makeStream(_unauthenticatedCommandStreamMethod, context);
        }

    private:
        std::shared_ptr<ClientStream> _makeStream(::grpc::internal::RpcMethod& method,
                                                  GRPCClientContext* context) {
            std::unique_ptr<::grpc::ClientReaderWriter<ConstSharedBuffer, SharedBuffer>>
                readerWriter(
                    ::grpc::internal::ClientReaderWriterFactory<WriteMessageType, ReadMessageType>::
                        Create(&*_channel, method, context->getGRPCClientContext()));

            return std::make_shared<GRPCClientStream>(std::move(readerWriter));
        }

        std::shared_ptr<::grpc::Channel> _channel;
        ::grpc::internal::RpcMethod _unauthenticatedCommandStreamMethod;
        ::grpc::internal::RpcMethod _authenticatedCommandStreamMethod;
    };

public:
    explicit StubFactoryImpl(GRPCClient::Options options) : _options(std::move(options)) {}

    void start(ServiceContext* const svcCtx) {
        // The pool calls into `ClockSource` to record the last usage of gRPC channels. Since the
        // pool is not concerned with sub-minute durations and this call happens as part of
        // destroying gRPC stubs (i.e., on threads running user operations), it is important to
        // use
        // `FastClockSource` to minimize the performance implications of recording time on user
        // operations.
        _pool = std::make_shared<ChannelPool<std::shared_ptr<::grpc::Channel>, Stub>>(
            svcCtx->getFastClockSource(),
            // The SSL mode resolver callback always returns true here because the current
            // implemention of Server requires the use of SSL. If that ever needs to change, this
            // resolver will need to be updated.
            [](auto) { return true; },
            [&](const HostAndPort& remote, bool useSSL) {
                invariant(useSSL, "SSL is required when using gRPC");
                auto uri = util::toGRPCFormattedURI(remote);
                auto credentials = util::isUnixSchemeGRPCFormattedURI(uri)
                    ? ::grpc::InsecureChannelCredentials()
                    : ::grpc::experimental::TlsCredentials(_makeTlsOptions());

                ::grpc::ChannelArguments channel_args;
                channel_args.SetMaxReceiveMessageSize(MaxMessageSizeBytes);
                channel_args.SetMaxSendMessageSize(MaxMessageSizeBytes);
                channel_args.SetCompressionAlgorithm(
                    ::grpc_compression_algorithm::GRPC_COMPRESS_NONE);
                return ::grpc::CreateCustomChannel(uri, credentials, channel_args);
            },
            [](std::shared_ptr<::grpc::Channel>& channel, Milliseconds connectTimeout) {
                iassert(ErrorCodes::NetworkTimeout,
                        "Timed out waiting for gRPC channel to establish",
                        channel->WaitForConnected(
                            (Date_t::now() + connectTimeout).toSystemTimePoint()));
                return Stub(channel);
            });

        _prunerService.start(svcCtx, _pool);
    }

    auto createStub(const HostAndPort& remote, Milliseconds connectTimeout) {
        return _pool->createStub(std::move(remote), ConnectSSLMode::kEnableSSL, connectTimeout);
    }

    void stop() {
        _prunerService.stop();
        _pool->dropAllChannels();
    }

private:
    ::grpc::experimental::TlsChannelCredentialsOptions _makeTlsOptions() {
        ::grpc::experimental::TlsChannelCredentialsOptions tlsOps;
        std::vector<::grpc::experimental::IdentityKeyCertPair> certKeyPairTls;

        if (_options.tlsCertificateKeyFile) {
            auto certKeyPair = util::parsePEMKeyFile(_options.tlsCertificateKeyFile.get());
            certKeyPairTls.push_back(
                {std::move(certKeyPair.private_key), std::move(certKeyPair.cert_chain)});
            tlsOps.watch_identity_key_cert_pairs();
        }

        if (_options.tlsCAFile) {
            auto caCert = ssl_util::readPEMFile(_options.tlsCAFile.get()).getValue();
            tlsOps.set_certificate_provider(
                std::make_shared<::grpc::experimental::StaticDataCertificateProvider>(
                    caCert, certKeyPairTls));
            tlsOps.watch_root_certs();
        } else {
            tlsOps.set_certificate_provider(
                std::make_shared<::grpc::experimental::StaticDataCertificateProvider>(
                    certKeyPairTls));
        }

        if (_options.tlsAllowInvalidCertificates || _options.tlsAllowInvalidHostnames) {
            // The CertificateVerifier handles extended attribute validation, and does not actually
            // pertain to validating the whole certificate chain. Setting it to NoOp ensures that
            // the default verifier, which verifies hostnames, is not used.
            tlsOps.set_certificate_verifier(
                std::make_shared<::grpc::experimental::NoOpCertificateVerifier>());
            // libgrpc also performs per-call (as opposed to per-connection) hostname verification
            // by default. This codepath is separate from the certificate verifier set above, so we
            // also need to disable this.
            tlsOps.set_check_call_host(false);

            if (_options.tlsAllowInvalidCertificates) {
                // This invocation ensures the certificate chain is not verified. The prior steps
                // also need to be taken when tlsAllowInvalidCertificates is set even when this is
                // called, since hostname verification and certificate chain verification are
                // also separate codepaths within libgrpc.
                tlsOps.set_verify_server_certs(false);
            }
        } else {
            // This is the default certificate verifier used by libgrpc, but we set it explicitly
            // here for clarity.
            tlsOps.set_certificate_verifier(
                std::make_shared<::grpc::experimental::HostNameCertificateVerifier>());
        }

        return tlsOps;
    }

    GRPCClient::Options _options;
    std::shared_ptr<ChannelPool<std::shared_ptr<::grpc::Channel>, Stub>> _pool;
    ChannelPrunerService<decltype(_pool)> _prunerService;
};

GRPCClient::GRPCClient(TransportLayer* tl, const BSONObj& clientMetadata, Options options)
    : Client(tl, clientMetadata) {
    _stubFactory = std::make_unique<StubFactoryImpl>(std::move(options));
}

void GRPCClient::start(ServiceContext* const svcCtx) {
    Client::start(svcCtx);
    static_cast<StubFactoryImpl&>(*_stubFactory).start(svcCtx);
}

void GRPCClient::shutdown() {
    Client::shutdown();
    static_cast<StubFactoryImpl&>(*_stubFactory).stop();
}

Client::CtxAndStream GRPCClient::_streamFactory(const HostAndPort& remote,
                                                Milliseconds connectTimeout,
                                                const ConnectOptions& options) {
    auto stub =
        static_cast<StubFactoryImpl&>(*_stubFactory).createStub(std::move(remote), connectTimeout);
    auto ctx = std::make_shared<GRPCClientContext>();
    setMetadataOnClientContext(*ctx, options);
    if (options.authToken) {
        return {ctx, stub->stub().authenticatedCommandStream(ctx.get())};
    } else {
        return {ctx, stub->stub().unauthenticatedCommandStream(ctx.get())};
    }
}

}  // namespace mongo::transport::grpc
