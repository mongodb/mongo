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
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/security/tls_certificate_provider.h>
#include <grpcpp/security/tls_certificate_verifier.h>
#include <grpcpp/security/tls_credentials_options.h>
#include <grpcpp/support/async_stream.h>

#include "src/core/tsi/ssl_transport_security.h"
#include "src/core/tsi/transport_security_interface.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/grpc/channel_pool.h"
#include "mongo/transport/grpc/client_stream.h"
#include "mongo/transport/grpc/grpc_client_context.h"
#include "mongo/transport/grpc/grpc_client_stream.h"
#include "mongo/transport/grpc/grpc_session.h"
#include "mongo/transport/grpc/util.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_types.h"
#include "mongo/util/net/ssl_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/synchronized_value.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::transport::grpc {
namespace {
inline Status makeShutdownTerminationStatus() {
    return {ErrorCodes::ShutdownInProgress, "gRPC client is shutting down"};
}
}  // namespace

Client::Client(TransportLayer* tl, ServiceContext* svcCtx, const BSONObj& clientMetadata)
    : _tl(tl),
      _svcCtx(svcCtx),
      _id(UUID::gen()),
      _clientMetadata(base64::encode(clientMetadata.objdata(), clientMetadata.objsize())),
      _sharedState(std::make_shared<EgressSession::SharedState>()) {
    _sharedState->clusterMaxWireVersion.store(util::constants::kMinimumWireVersion);
}

void Client::start() {
    stdx::lock_guard lk(_mutex);
    invariant(std::exchange(_state, ClientState::kStarted) == ClientState::kUninitialized,
              "Cannot start a gRPC client more than once");
}

void Client::shutdown() {
    decltype(_sessions) sessions;
    decltype(_pendingStreamStates) pendingStreamStates;
    {
        stdx::lock_guard lk(_mutex);
        invariant(std::exchange(_state, ClientState::kShutdown) != ClientState::kShutdown,
                  "Cannot shut down a gRPC client more than once");
        sessions = _sessions;
        pendingStreamStates = _pendingStreamStates;
    }

    // Cancel all outstanding connect timers and stream establishment attempts.
    for (auto& streamState : pendingStreamStates) {
        streamState->cancel(Status(ErrorCodes::ShutdownInProgress,
                                   "Cancelled stream establishment due to gRPC client shutdown"));
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
    return _state == ClientState::kShutdown && _pendingStreamStates.size() == 0 &&
        _sessions.empty();
}

Future<std::shared_ptr<EgressSession>> Client::connect(
    const HostAndPort& remote,
    const std::shared_ptr<GRPCReactor>& reactor,
    Milliseconds timeout,
    ConnectOptions options,
    std::shared_ptr<ConnectionMetrics> connectionMetrics) {
    // TODO SERVER-98254: this implementation currently acquires _mutex twice, which will have
    // negative performance implications. Egress performance is not a priority at the moment, but we
    // should revisit how lock contention can be reduced here in the future.
    std::shared_ptr<PendingStreamState> streamState;
    {
        std::lock_guard lk(_mutex);
        invariant(_state != ClientState::kUninitialized,
                  "Client must be started before connect can be called");
        if (_state == ClientState::kShutdown) {
            return Future<std::shared_ptr<EgressSession>>::makeReady(
                makeShutdownTerminationStatus());
        }

        streamState = std::make_shared<PendingStreamState>();
        auto iter = _pendingStreamStates.insert(_pendingStreamStates.end(), streamState);
        _pendingStreamStates.back()->iter = iter;
    }

    LOGV2_DEBUG(9715300,
                3,
                "Establishing gRPC stream",
                "remote"_attr = remote,
                "sslMode"_attr = options.sslMode,
                "timeout"_attr = timeout);
    if (connectionMetrics) {
        connectionMetrics->onConnectionStarted();
    }

    if (timeout > Milliseconds(0)) {
        streamState->setTimer(reactor->makeTimer());
        streamState->getTimer()
            ->waitUntil(reactor->now() + timeout)
            .getAsync([streamState](Status s) mutable {
                if (!s.isOK()) {
                    return;
                }

                streamState->cancel(Status(ErrorCodes::NetworkTimeout,
                                           "Timed out waiting for gRPC stream to establish"));
            });
    }

    return future_util::withCancellation(
               _streamFactory(
                   remote, reactor, timeout, options, streamState->getCancellationToken())
                   .then([this, reactor, connMetrics = std::move(connectionMetrics)](
                             CallContext call) -> StatusWith<std::shared_ptr<EgressSession>> {
                       if (connMetrics) {
                           // We don't have visibility into these events in gRPC, so mark them
                           // all as complete once the rpc call has been started.
                           connMetrics->onDNSResolved();
                           connMetrics->onTCPConnectionEstablished();
                           connMetrics->onTLSHandshakeFinished();
                       }

                       auto session = std::make_shared<EgressSession>(_tl,
                                                                      reactor,
                                                                      std::move(call.ctx),
                                                                      std::move(call.stream),
                                                                      std::move(call.sslConfig),
                                                                      _id,
                                                                      _sharedState);

                       stdx::lock_guard lk(_mutex);
                       if (MONGO_unlikely(_state == ClientState::kShutdown)) {
                           auto status = makeShutdownTerminationStatus();
                           session->cancel(status);
                           return status;
                       }

                       auto it = _sessions.insert(_sessions.begin(), session);
                       _numCurrentStreams.increment();

                       session->setCleanupCallback(
                           [this, client = weak_from_this(), it = std::move(it)]() {
                               if (auto anchor = client.lock()) {
                                   stdx::lock_guard lk(_mutex);
                                   _sessions.erase(it);
                                   _numCurrentStreams.decrement();

                                   if (MONGO_unlikely(_isShutdownComplete_inlock())) {
                                       _shutdownCV.notify_one();
                                   }
                               }
                           });

                       return session;
                   })
                   .onError<ErrorCodes::CallbackCanceled>(
                       [streamState](
                           const Status& s) -> StatusWith<std::shared_ptr<EgressSession>> {
                           if (streamState->getCancellationToken().isCanceled()) {
                               // Use the outer Future's cancellation reason if stream establishment
                               // failed due to token cancellation.
                               return s;
                           }

                           return Status(ErrorCodes::HostUnreachable,
                                         "Could not contact remote host to establish gRPC stream");
                       })
                   .onCompletion(
                       [this, streamState](StatusWith<std::shared_ptr<EgressSession>> swSession) {
                           streamState->cancelTimer();

                           stdx::lock_guard lk(_mutex);
                           _pendingStreamStates.erase(streamState->iter);
                           if (MONGO_unlikely(_isShutdownComplete_inlock())) {
                               _shutdownCV.notify_one();
                           }

                           return swSession;
                       }),
               streamState->getCancellationToken())
        .thenRunOn(reactor)
        .onError<ErrorCodes::CallbackCanceled>(
            [streamState](const Status& s) -> StatusWith<std::shared_ptr<EgressSession>> {
                return streamState->getCancellationReason();
            })
        .unsafeToInlineFuture();  // If cancellation occurred, this will be guaranteed to be running
                                  // on the reactor thread due to the onError continuation, and in a
                                  // success case this will already be running on the reactor
                                  // thread.
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
    struct Channel {
        std::shared_ptr<::grpc::Channel> grpcChannel;
        boost::optional<SSLConfiguration> sslConfig;

        Channel(std::shared_ptr<::grpc::Channel> ch, boost::optional<SSLConfiguration> ssl)
            : grpcChannel(std::move(ch)), sslConfig(std::move(ssl)) {}
    };

    class Stub {
    public:
        using ReadMessageType = SharedBuffer;
        using WriteMessageType = ConstSharedBuffer;

        explicit Stub(const std::shared_ptr<Channel>& channel)
            : _channel(channel),
              _unauthenticatedCommandStreamMethod(
                  util::constants::kUnauthenticatedCommandStreamMethodName,
                  ::grpc::internal::RpcMethod::BIDI_STREAMING,
                  channel->grpcChannel),
              _authenticatedCommandStreamMethod(
                  util::constants::kAuthenticatedCommandStreamMethodName,
                  ::grpc::internal::RpcMethod::BIDI_STREAMING,
                  channel->grpcChannel) {}

        Future<std::shared_ptr<ClientStream>> authenticatedCommandStream(
            GRPCClientContext* context, const std::shared_ptr<GRPCReactor>& reactor) {
            return _makeStream(_authenticatedCommandStreamMethod, context, reactor);
        }

        Future<std::shared_ptr<ClientStream>> unauthenticatedCommandStream(
            GRPCClientContext* context, const std::shared_ptr<GRPCReactor>& reactor) {
            return _makeStream(_unauthenticatedCommandStreamMethod, context, reactor);
        }

        boost::optional<SSLConfiguration>& getSSLConfiguration() {
            return _channel->sslConfig;
        }

    private:
        Future<std::shared_ptr<ClientStream>> _makeStream(
            ::grpc::internal::RpcMethod& method,
            GRPCClientContext* context,
            const std::shared_ptr<GRPCReactor>& reactor) {
            using StreamType = ::grpc::ClientAsyncReaderWriter<ConstSharedBuffer, SharedBuffer>;
            auto pf = makePromiseFuture<void>();
            std::unique_ptr<StreamType> readerWriter;

            {
                auto shutdownLock = reactor->_shutdownMutex.readLock();
                if (reactor->_inShutdownFlag) {
                    return Future<std::shared_ptr<ClientStream>>::makeReady(
                        Status(ErrorCodes::ShutdownInProgress,
                               "Cannot create a gRPC stream after reactor shutdown"));
                }

                readerWriter = std::unique_ptr<StreamType>(
                    ::grpc::internal::
                        ClientAsyncReaderWriterFactory<ConstSharedBuffer, SharedBuffer>::Create(
                            _channel->grpcChannel.get(),
                            reactor->_getCompletionQueue(),
                            method,
                            context->getGRPCClientContext(),
                            true,
                            reactor->_registerCompletionQueueEntry(std::move(pf.promise))));
            }

            return std::move(pf.future).then(
                [rw = std::move(readerWriter)]() mutable -> std::shared_ptr<ClientStream> {
                    return std::make_shared<GRPCClientStream>(std::move(rw));
                });
        }

        std::shared_ptr<Channel> _channel;
        ::grpc::internal::RpcMethod _unauthenticatedCommandStreamMethod;
        ::grpc::internal::RpcMethod _authenticatedCommandStreamMethod;
    };

public:
    explicit StubFactoryImpl(GRPCClient::Options options, ServiceContext* svcCtx)
        : _options(std::move(options)), _svcCtx(svcCtx) {
        // The pool calls into `ClockSource` to record the last usage of gRPC channels. Since the
        // pool is not concerned with sub-minute durations and this call happens as part of
        // destroying gRPC stubs (i.e., on threads running user operations), it is important to use
        // `FastClockSource` to minimize the performance implications of recording time on user
        // operations.
        _pool = std::make_shared<ChannelPool<std::shared_ptr<Channel>, Stub>>(
            svcCtx->getFastClockSource(),
            [](ConnectSSLMode sslMode) {
#ifndef MONGO_CONFIG_SSL
                if (sslMode == kEnableSSL) {
                    uasserted(ErrorCodes::InvalidSSLConfiguration,
                              "SSL requested but not supported");
                }
#else
                auto globalSSLMode =
                    static_cast<SSLParams::SSLModes>(getSSLGlobalParams().sslMode.load());

                return (sslMode == kEnableSSL ||
                        (sslMode == kGlobalSSLMode &&
                         ((globalSSLMode == SSLParams::SSLMode_preferSSL) ||
                          (globalSSLMode == SSLParams::SSLMode_requireSSL))));
#endif
            },
            [&](const HostAndPort& remote, bool useSSL) {
                auto uri = util::toGRPCFormattedURI(remote);
                auto [credentials,
                      sslConfig] = [&]() -> std::pair<std::shared_ptr<::grpc::ChannelCredentials>,
                                                      boost::optional<SSLConfiguration>> {
                    if (!useSSL || util::isUnixSchemeGRPCFormattedURI(uri)) {
                        return {::grpc::InsecureChannelCredentials(), boost::none};
                    }
                    auto tlsCache = _tlsCache.synchronize();
                    invariant(tlsCache->has_value());
                    return {::grpc::experimental::TlsCredentials(_makeTlsOptions(**tlsCache)),
                            (*tlsCache)->sslConfig};
                }();

                ::grpc::ChannelArguments channel_args;
                channel_args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS,
                                    serverGlobalParams.grpcKeepAliveTimeMs);
                channel_args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS,
                                    serverGlobalParams.grpcKeepAliveTimeoutMs);
                channel_args.SetMaxReceiveMessageSize(MaxMessageSizeBytes);
                channel_args.SetMaxSendMessageSize(MaxMessageSizeBytes);
                channel_args.SetCompressionAlgorithm(
                    ::grpc_compression_algorithm::GRPC_COMPRESS_NONE);
                return std::make_shared<StubFactoryImpl::Channel>(
                    ::grpc::CreateCustomChannel(uri, credentials, channel_args), sslConfig);
            },
            [](std::shared_ptr<Channel>& channel, Milliseconds connectTimeout) {
                return Stub(channel);
            });
    }

    void start() {
#ifdef MONGO_CONFIG_SSL
        _loadTlsCertificates(SSLManagerCoordinator::get()->getSSLManager()->getSSLConfiguration());
#endif
        _prunerService.start(_svcCtx, _pool);
    }

    auto createStub(const HostAndPort& remote,
                    ConnectSSLMode sslMode,
                    Milliseconds connectTimeout) {
        return _pool->createStub(std::move(remote), sslMode, connectTimeout);
    }

    auto getPoolSize() const {
        return _pool->size();
    }

    void stop() {
        _prunerService.stop();
        _pool->dropAllChannels();
    }

#ifdef MONGO_CONFIG_SSL
    Status rotateCertificates(const SSLConfiguration& sslConfig) try {
        LOGV2_DEBUG(9886801, 3, "Rotating certificates used for creating gRPC channels");
        _loadTlsCertificates(sslConfig);
        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
#endif

    void dropAllChannels_forTest() {
        _pool->dropAllChannels();
    }

private:
    struct TLSCache {
        std::shared_ptr<::grpc::experimental::CertificateProviderInterface> certificateProvider;
        SSLConfiguration sslConfig;
    };

    /**
     * Utilize gRPC's ssl_client_handshaker_factory to verify that the user has provided valid TLS
     * certificates. Throws an exception if the provided certificates are not valid.
     * Leaves encryption-specific options as their defaults.
     */
    void _verifyTLSCertificates(
        const boost::optional<const ::grpc::experimental::IdentityKeyCertPair&> certKeyPair,
        const boost::optional<std::string>& caCert) {
        tsi_ssl_pem_key_cert_pair pemKeyCertPair;
        tsi_ssl_client_handshaker_factory* handshakerFactory = nullptr;
        tsi_ssl_client_handshaker_options options;

        if (certKeyPair) {
            pemKeyCertPair = {certKeyPair->private_key.c_str(),
                              certKeyPair->certificate_chain.c_str()};
            options.pem_key_cert_pair = &pemKeyCertPair;
        }

        if (caCert) {
            options.pem_root_certs = caCert->c_str();
        }

        tsi_result result =
            tsi_create_ssl_client_handshaker_factory_with_options(&options, &handshakerFactory);
        uassert(ErrorCodes::InvalidSSLConfiguration,
                "Invalid certificates provided.",
                result == TSI_OK);
        tsi_ssl_client_handshaker_factory_unref(handshakerFactory);
    }

#ifdef MONGO_CONFIG_SSL
    void _loadTlsCertificates(const SSLConfiguration& sslConfig) {
        auto cache = [&]() -> boost::optional<TLSCache> {
            if (!_options.tlsCAFile && !_options.tlsCertificateKeyFile) {
                return boost::none;
            }

            std::vector<::grpc::experimental::IdentityKeyCertPair> certKeyPairs;
            if (_options.tlsCertificateKeyFile) {
                auto sslPair = util::parsePEMKeyFile(_options.tlsCertificateKeyFile.get());
                certKeyPairs.push_back(
                    {std::move(sslPair.private_key), std::move(sslPair.cert_chain)});
            }

            boost::optional<std::string> caCert;
            if (_options.tlsCAFile) {
                caCert.emplace(ssl_util::readPEMFile(_options.tlsCAFile.get()).getValue());
            }

            boost::optional<const ::grpc::experimental::IdentityKeyCertPair&> certPair;
            if (!certKeyPairs.empty()) {
                certPair = certKeyPairs.front();
            }
            _verifyTLSCertificates(certPair, caCert);

            TLSCache cache{};
            cache.certificateProvider =
                std::make_shared<::grpc::experimental::StaticDataCertificateProvider>(
                    caCert.get_value_or(""), certKeyPairs);
            cache.sslConfig = sslConfig;

            return cache;
        }();

        _tlsCache.synchronize() = std::move(cache);
    }
#endif

    ::grpc::experimental::TlsChannelCredentialsOptions _makeTlsOptions(const TLSCache& tlsInfo) {
        ::grpc::experimental::TlsChannelCredentialsOptions tlsOps;

        tlsOps.set_certificate_provider(tlsInfo.certificateProvider);

        if (_options.tlsCertificateKeyFile) {
            tlsOps.watch_identity_key_cert_pairs();
        }

        if (_options.tlsCAFile) {
            tlsOps.watch_root_certs();
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
    ServiceContext* const _svcCtx;

    std::shared_ptr<ChannelPool<std::shared_ptr<Channel>, Stub>> _pool;
    ChannelPrunerService<decltype(_pool)> _prunerService;

    synchronized_value<boost::optional<TLSCache>> _tlsCache;
};

GRPCClient::GRPCClient(TransportLayer* tl,
                       ServiceContext* svcCtx,
                       const BSONObj& clientMetadata,
                       Options options)
    : Client(tl, svcCtx, clientMetadata) {
    _stubFactory = std::make_unique<StubFactoryImpl>(std::move(options), svcCtx);
}

void GRPCClient::start() {
    Client::start();
    static_cast<StubFactoryImpl&>(*_stubFactory).start();
}

void GRPCClient::shutdown() {
    Client::shutdown();
    static_cast<StubFactoryImpl&>(*_stubFactory).stop();
}

void GRPCClient::appendStats(BSONObjBuilder* section) const {
    auto numCurrentChannels = static_cast<StubFactoryImpl&>(*_stubFactory).getPoolSize();

    section->append(kCurrentChannelsFieldName, numCurrentChannels);

    {
        BSONObjBuilder streamSection(section->subobjStart(kStreamsSubsectionFieldName));
        streamSection.append(kCurrentStreamsFieldName, _numCurrentStreams.get());
    }
}

#ifdef MONGO_CONFIG_SSL
Status GRPCClient::rotateCertificates(const SSLConfiguration& config) {
    return static_cast<StubFactoryImpl&>(*_stubFactory).rotateCertificates(config);
}
#endif

void GRPCClient::dropAllChannels_forTest() {
    static_cast<StubFactoryImpl&>(*_stubFactory).dropAllChannels_forTest();
}

Future<Client::CallContext> GRPCClient::_streamFactory(const HostAndPort& remote,
                                                       const std::shared_ptr<GRPCReactor>& reactor,
                                                       Milliseconds connectTimeout,
                                                       const ConnectOptions& options,
                                                       const CancellationToken& token) {
    auto stub = static_cast<StubFactoryImpl&>(*_stubFactory)
                    .createStub(std::move(remote), options.sslMode, connectTimeout);
    auto ctx = std::make_shared<GRPCClientContext>();
    setMetadataOnClientContext(*ctx, options);
    token.onCancel().unsafeToInlineFuture().getAsync([ctx, reactor](Status s) {
        if (!s.isOK()) {
            return;
        }
        // Send a best-effort attempt to cancel the ongoing stream establishment.
        reactor->schedule([ctx](Status s) {
            if (!s.isOK()) {
                return;
            }
            ctx->tryCancel();
        });
    });

    auto fut = [&]() {
        if (options.authToken) {
            return stub->stub().authenticatedCommandStream(ctx.get(), reactor);
        } else {
            return stub->stub().unauthenticatedCommandStream(ctx.get(), reactor);
        }
    }();

    return std::move(fut).then(
        [clientContext = std::move(ctx),
         sslConfig = stub->stub().getSSLConfiguration()](std::shared_ptr<ClientStream> stream) {
            return Client::CallContext{std::move(clientContext), std::move(stream), sslConfig};
        });
}

}  // namespace mongo::transport::grpc
