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
#include <src/core/lib/security/security_connector/ssl_utils.h>

#include <src/core/tsi/ssl_transport_security.h>
#include <src/core/tsi/transport_security_interface.h>

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

inline Status makeDroppedConnectionCancelStatus() {
    return {ErrorCodes::CallbackCanceled,
            "Cancelled stream establishment due to dropping the associated connection"};
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
    decltype(_remoteStates) remoteStates;
    {
        stdx::lock_guard lk(_mutex);
        invariant(std::exchange(_state, ClientState::kShutdown) != ClientState::kShutdown,
                  "Cannot shut down a gRPC client more than once");
        sessions = _sessions;
        remoteStates = _remoteStates;
    }

    // Cancel all outstanding connect timers and stream establishment attempts.
    for (auto&& it : remoteStates) {
        for (auto&& streamState : it.second.pendingStreamStates) {
            streamState->cancel(
                Status(ErrorCodes::ShutdownInProgress,
                       "Cancelled stream establishment due to gRPC client shutdown"));
        }
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
    return _state == ClientState::kShutdown && _numPendingStreams == 0 && _sessions.empty();
}

Future<std::shared_ptr<EgressSession>> Client::connect(
    const HostAndPort& remote,
    const std::shared_ptr<GRPCReactor>& reactor,
    Milliseconds timeout,
    ConnectOptions options,
    const CancellationToken& token,
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

        streamState = std::make_shared<PendingStreamState>(
            remote, options.sslMode, std::move(connectionMetrics), token);
        streamState->registerWithClient(lk, remote, *this);
        _numPendingStreams++;
    }

    if (timeout > Milliseconds(0) && timeout < Milliseconds::max()) {
        streamState->setDeadline(reactor->now() + timeout);
    }

    return _streamFactory(remote,
                          reactor,
                          streamState->getDeadline() ? streamState->getDeadline().get()
                                                     : Date_t::max(),
                          options,
                          streamState->getCancellationToken())
        .then([this, reactor, streamState](CallContext call) -> Future<CallContext> {
            LOGV2_DEBUG(9715300,
                        3,
                        "Acquired an established gRPC channel, now starting gRPC call",
                        "remote"_attr = streamState->getRemote(),
                        "deadline"_attr = streamState->getDeadline(),
                        "sslMode"_attr = connectSSLModeToString(streamState->getSSLMode()));
            if (auto& connMetrics = streamState->getConnectionMetrics(); connMetrics) {
                // We don't have visibility into these events in gRPC, so mark them
                // all as complete once the connection has been established.
                connMetrics->onDNSResolved();
                connMetrics->onTCPConnectionEstablished();
                connMetrics->onTLSHandshakeFinished();
            }

            // Start the gRPC call.
            Future<void> fut;
            {
                auto shutdownLock = reactor->_shutdownMutex.readLock();
                if (reactor->_inShutdownFlag) {
                    return Future<CallContext>::makeReady(
                        Status(ErrorCodes::ShutdownInProgress,
                               "Cannot create a gRPC stream after reactor shutdown"));
                }
                auto pf = makePromiseFuture<void>();
                call.stream->startCall(
                    reactor->_registerCompletionQueueEntry(std::move(pf.promise)));
                fut = std::move(pf.future);
            }

            // Set the timeout for the call to start.
            if (auto& deadline = streamState->getDeadline(); deadline) {
                streamState->setTimer(reactor->makeTimer());

                streamState->getTimer()
                    ->waitUntil(deadline.get())
                    .getAsync([streamState, deadline](Status s) mutable {
                        if (!s.isOK()) {
                            return;
                        }

                        streamState->cancel(Status(
                            ErrorCodes::ExceededTimeLimit,
                            fmt::format("Exceeded time limit waiting for gRPC call to start")));
                    });
            }

            // Cancel the StartCall attempt if a timeout or arbitrary cancellation has occurred.
            streamState->getCancellationToken().onCancel().unsafeToInlineFuture().getAsync(
                [ctx = call.ctx, reactor](Status s) {
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

            return std::move(fut).then([c = std::move(call)]() { return c; });
        })
        .then([this, reactor, streamState](
                  CallContext call) -> StatusWith<std::shared_ptr<EgressSession>> {
            LOGV2_DEBUG(9715301,
                        3,
                        "Successfully started gRPC call",
                        "remote"_attr = streamState->getRemote(),
                        "deadline"_attr = streamState->getDeadline(),
                        "sslMode"_attr = connectSSLModeToString(streamState->getSSLMode()));
            auto session = std::make_shared<EgressSession>(_tl,
                                                           reactor,
                                                           std::move(call.ctx),
                                                           std::move(call.stream),
                                                           std::move(call.sslConfig),
                                                           std::move(call.channelUUID),
                                                           _id,
                                                           _sharedState);

            stdx::lock_guard lk(_mutex);
            if (MONGO_unlikely(_state == ClientState::kShutdown)) {
                auto status = makeShutdownTerminationStatus();
                session->cancel(status);
                return status;
            }

            auto it = _sessions.insert(_sessions.begin(), session);
            _numActiveStreams.increment();

            session->setCleanupCallback(
                [this, client = weak_from_this(), it = std::move(it)](Status terminationStatus) {
                    if (terminationStatus.isOK()) {
                        _numSuccessfulStreams.increment();
                    } else {
                        _numFailedStreams.increment();
                    }
                    if (auto anchor = client.lock()) {
                        stdx::lock_guard lk(_mutex);
                        _sessions.erase(it);
                        _numActiveStreams.decrement();

                        if (MONGO_unlikely(_isShutdownComplete_inlock())) {
                            _shutdownCV.notify_one();
                        }
                    }
                });

            return session;
        })
        .tapAll([this, streamState](const StatusWith<std::shared_ptr<EgressSession>>& swSession) {
            streamState->cancelTimer();

            stdx::lock_guard lk(_mutex);
            _numPendingStreams--;
            streamState->unregisterFromClient(lk, streamState->getRemote(), *this);
            if (MONGO_unlikely(_isShutdownComplete_inlock())) {
                _shutdownCV.notify_one();
            }
        })
        .onError<ErrorCodes::CallbackCanceled>(
            [streamState](const Status& s) -> StatusWith<std::shared_ptr<EgressSession>> {
                // The completion queue produces CallbackCanceled errors for any tag that does not
                // complete successfully, so we need to catch them here and add more
                // context-specific information to them.
                if (auto reason = streamState->getCancellationReason(); !reason.isOK()) {
                    return reason;
                }

                // If the failure was not due to cancellation, we can assume it is because the
                // remote is unreachable.
                return Status(ErrorCodes::HostUnreachable, "gRPC stream establishment failed");
            })
        .tapError([](const Status& s) {
            LOGV2_DEBUG(9936109, 1, "Stream establishment failed", "error"_attr = s);
        });
}

void Client::PendingStreamState::registerWithClient(WithLock,
                                                    const HostAndPort& remote,
                                                    Client& client) {
    auto& streamList = client._remoteStates[remote].pendingStreamStates;
    _iter = streamList.insert(streamList.end(), shared_from_this());
}

void Client::PendingStreamState::unregisterFromClient(WithLock,
                                                      const HostAndPort& remote,
                                                      Client& client) {
    auto streamStateIt = client._remoteStates.find(remote);
    invariant(streamStateIt != client._remoteStates.end());
    streamStateIt->second.pendingStreamStates.erase(_iter);
}

void Client::PendingStreamState::cancel(Status reason) {
    invariant(!reason.isOK());

    {
        stdx::lock_guard lg(_mutex);
        if (!_cancellationReason.isOK()) {
            // Already cancelled, can early return.
            return;
        }
        _cancellationReason = reason;
    }

    LOGV2_DEBUG(9936110, 3, "Canceling gRPC stream establishment", "reason"_attr = reason);
    _cancelSource.cancel();
    cancelTimer();
}

Status Client::PendingStreamState::getCancellationReason() {
    if (!_cancelSource.token().isCanceled()) {
        return Status::OK();
    }

    stdx::lock_guard lg(_mutex);
    if (!_cancellationReason.isOK()) {
        return _cancellationReason;
    }
    return Status(ErrorCodes::CallbackCanceled, "gRPC stream establishment was cancelled");
}

size_t Client::getPendingStreamEstablishments(const HostAndPort& target) {
    stdx::lock_guard lg(_mutex);
    if (auto it = _remoteStates.find(target); it != _remoteStates.end()) {
        return it->second.pendingStreamStates.size();
    }
    return 0;
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

class Channel {
public:
    static constexpr auto kChannelPollingIntervalMs = Milliseconds(100);

    Channel(std::shared_ptr<::grpc::Channel> ch, boost::optional<SSLConfiguration> ssl, UUID id)
        : _grpcChannel(std::move(ch)), _sslConfig(std::move(ssl)), _uuid(std::move(id)) {}

    const boost::optional<SSLConfiguration>& getSSLConfiguration() {
        return _sslConfig;
    }

    const std::shared_ptr<::grpc::Channel>& getGRPCChannel() {
        return _grpcChannel;
    }

    const UUID& getId() {
        return _uuid;
    }

    Future<void> waitForConnected(std::shared_ptr<GRPCReactor> reactor,
                                  Date_t deadline,
                                  const CancellationToken& token) {
        // Check for the happy case first to avoid unnecessary polling.
        auto channelState = _grpcChannel->GetState(true /*try_to_connect*/);
        if (MONGO_likely(channelState == GRPC_CHANNEL_READY)) {
            return Future<void>::makeReady();
        }

        return AsyncTry([channel = _grpcChannel, reactor, deadline] {
                   // Check for the happy case first to avoid unnecessary polling.
                   auto channelState = channel->GetState(true /*try_to_connect*/);
                   if (MONGO_likely(channelState == GRPC_CHANNEL_READY)) {
                       return Future<grpc_connectivity_state>::makeReady(channelState);
                   }

                   // We poll every kChannelPollingIntervalMs to allow us to notice cancellation, as
                   // the gRPC NotifyOnStateChange API does not support cancellation:
                   // https://github.com/grpc/grpc/issues/3064
                   auto nextPoll = reactor->now() + kChannelPollingIntervalMs;

                   PromiseAndFuture<void> pf;
                   {
                       auto shutdownLock = reactor->_shutdownMutex.readLock();
                       if (reactor->_inShutdownFlag) {
                           return Future<grpc_connectivity_state>::makeReady(
                               Status(ErrorCodes::ShutdownInProgress,
                                      "Cannot create a gRPC stream after reactor shutdown"));
                       }
                       pf = makePromiseFuture<void>();
                       channel->NotifyOnStateChange(
                           channelState,
                           (deadline > nextPoll ? nextPoll : deadline).toSystemTimePoint(),
                           reactor->_getCompletionQueue(),
                           reactor->_registerCompletionQueueEntry(std::move(pf.promise)));
                   }

                   return std::move(pf.future).then(
                       [channel]() { return channel->GetState(true /*try_to_connect*/); });
               })
            .until([reactor, deadline](const StatusWith<grpc_connectivity_state>& swChannelState) {
                // The channel is ready.
                if (MONGO_likely(swChannelState.isOK() &&
                                 swChannelState.getValue() == GRPC_CHANNEL_READY)) {
                    return true;
                }

                // We've exceeded the deadline.
                if (deadline < reactor->now()) {
                    uasserted(
                        ErrorCodes::HostUnreachable,
                        fmt::format(
                            "Failed to establish connectivity of gRPC channel before deadline {}",
                            deadline.toString()));
                }

                if (swChannelState.getStatus().code() == ErrorCodes::CallbackCanceled) {
                    return false;  // Retry if the timeout expired without a state change.
                }

                uassertStatusOK(swChannelState);  // Any other failure should terminate the loop.

                // The channel will never be ready.
                if (MONGO_unlikely(swChannelState.getValue() == GRPC_CHANNEL_SHUTDOWN)) {
                    uasserted(ErrorCodes::HostUnreachable,
                              "GRPC channel has encountered an unrecoverable error and is unable "
                              "to connect");
                }

                // If the channel state is CONNECTING, TRANSIENT_FAILURE, or IDLE and we have not
                // yet exceeded the deadline or been cancelled, then retry.
                return false;
            })
            .on(reactor, token)
            .onCompletion([](const StatusWith<grpc_connectivity_state>& swChannelState) {
                return Future<void>::makeReady(swChannelState.getStatus());
            })
            .unsafeToInlineFuture();
    }

private:
    std::shared_ptr<::grpc::Channel> _grpcChannel;
    boost::optional<SSLConfiguration> _sslConfig;
    UUID _uuid;
};

class StubFactoryImpl : public GRPCClient::StubFactory {
    class Stub {
    public:
        using ReadMessageType = SharedBuffer;
        using WriteMessageType = ConstSharedBuffer;

        explicit Stub(const std::shared_ptr<Channel>& channel)
            : _channel(channel),
              _unauthenticatedCommandStreamMethod(
                  util::constants::kUnauthenticatedCommandStreamMethodName,
                  ::grpc::internal::RpcMethod::BIDI_STREAMING,
                  channel->getGRPCChannel()),
              _authenticatedCommandStreamMethod(
                  util::constants::kAuthenticatedCommandStreamMethodName,
                  ::grpc::internal::RpcMethod::BIDI_STREAMING,
                  channel->getGRPCChannel()) {}

        std::shared_ptr<ClientStream> authenticatedCommandStream(
            GRPCClientContext* context, const std::shared_ptr<GRPCReactor>& reactor) {
            return _makeStream(_authenticatedCommandStreamMethod, context, reactor);
        }

        std::shared_ptr<ClientStream> unauthenticatedCommandStream(
            GRPCClientContext* context, const std::shared_ptr<GRPCReactor>& reactor) {
            return _makeStream(_unauthenticatedCommandStreamMethod, context, reactor);
        }

        const std::shared_ptr<Channel>& getChannel() {
            return _channel;
        }

    private:
        std::shared_ptr<ClientStream> _makeStream(::grpc::internal::RpcMethod& method,
                                                  GRPCClientContext* context,
                                                  const std::shared_ptr<GRPCReactor>& reactor) {
            using StreamType = ::grpc::ClientAsyncReaderWriter<ConstSharedBuffer, SharedBuffer>;
            // Create the stream using gRPC's Async "Prepare" API, which creates the stream/call
            // object, but doesn't initiate it. The call is initiated via startCall in the
            // Client::connect function.
            return std::make_shared<GRPCClientStream>(std::unique_ptr<StreamType>(
                ::grpc::internal::ClientAsyncReaderWriterFactory<ConstSharedBuffer, SharedBuffer>::
                    Create(_channel->getGRPCChannel().get(),
                           reactor->_getCompletionQueue(),
                           method,
                           context->getGRPCClientContext(),
                           false,
                           nullptr)));
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
            [](ConnectSSLMode sslMode) -> bool {
#ifndef MONGO_CONFIG_SSL
                if (sslMode == kEnableSSL) {
                    uasserted(ErrorCodes::InvalidSSLConfiguration,
                              "SSL requested but not supported");
                }
                return false;
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
                auto uuid = UUID::gen();

                ::grpc::ChannelArguments channel_args;
                channel_args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS,
                                    serverGlobalParams.grpcKeepAliveTimeMs);
                channel_args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS,
                                    serverGlobalParams.grpcKeepAliveTimeoutMs);
                channel_args.SetMaxReceiveMessageSize(MaxMessageSizeBytes);
                channel_args.SetMaxSendMessageSize(MaxMessageSizeBytes);
                channel_args.SetCompressionAlgorithm(
                    ::grpc_compression_algorithm::GRPC_COMPRESS_NONE);
                // We must set unique channel arguments on each channel object to force gRPC to use
                // a separate TCP connection per channel:
                // https://stackoverflow.com/questions/53564748
                channel_args.SetString("channelId", uuid.toString());
                return std::make_shared<Channel>(
                    ::grpc::CreateCustomChannel(uri, credentials, channel_args), sslConfig, uuid);
            },
            [](std::shared_ptr<Channel>& channel) { return Stub(channel); });
    }

    void start() {
        std::shared_ptr<SSLManagerInterface> manager = nullptr;
        if (SSLManagerCoordinator::get() &&
            (manager = SSLManagerCoordinator::get()->getSSLManager())) {
            _loadTlsCertificates(manager->getSSLConfiguration());
        }
        _prunerService.start(_svcCtx, _pool);
    }

    auto createStub(const HostAndPort& remote, ConnectSSLMode sslMode) {
        return _pool->createStub(std::move(remote), sslMode);
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

    const std::shared_ptr<ChannelPool<std::shared_ptr<Channel>, Stub>>& getChannelPool() {
        return _pool;
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

        if (result != TSI_OK) {
            if (_options.tlsAllowInvalidCertificates) {
                LOGV2_WARNING(9977700, "Invalid certificates provided");
                return;
            } else {
                uasserted(ErrorCodes::InvalidSSLConfiguration, "Invalid certificates provided");
            }
        }

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
                uassert(
                    9985600,
                    "The use of both tlsCAFile and the System Certificate store is not supported.",
                    !sslGlobalParams.sslUseSystemCA);
                caCert.emplace(ssl_util::readPEMFile(_options.tlsCAFile.get()).getValue());
            } else if (sslGlobalParams.sslUseSystemCA) {
                caCert.emplace(grpc_core::DefaultSslRootStore::GetPemRootCerts());
            } else if (_options.tlsAllowInvalidCertificates) {
                LOGV2_WARNING(9985603, "No tlsCAFile specified, and tlsUseSystemCA not specified");
            } else {
                uasserted(9985604, "No tlsCAFile specified, and tlsUseSystemCA not specified");
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

        if (_options.tlsCAFile || sslGlobalParams.sslUseSystemCA) {
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

void GRPCClient::appendStats(GRPCConnectionStats& stats) const {
    stats.setTotalOpenChannels(
        static_cast<StubFactoryImpl&>(*_stubFactory).getChannelPool()->size());
    stats.setTotalActiveStreams(_numActiveStreams.get());
    stats.setTotalSuccessfulStreams(_numSuccessfulStreams.get());
    stats.setTotalFailedStreams(_numFailedStreams.get());
}

#ifdef MONGO_CONFIG_SSL
Status GRPCClient::rotateCertificates(const SSLConfiguration& config) {
    return static_cast<StubFactoryImpl&>(*_stubFactory).rotateCertificates(config);
}
#endif

void GRPCClient::dropConnections() {
    _dropPendingStreamEstablishments([](const HostAndPort& remote) { return true; });

    // Now drop all of the channels themselves.
    static_cast<StubFactoryImpl&>(*_stubFactory).getChannelPool()->dropAllChannels();
}

void GRPCClient::dropConnections(const HostAndPort& target) {
    _dropPendingStreamEstablishments(
        [target](const HostAndPort& remote) { return remote == target; });

    // Now drop all of the channels themselves.
    static_cast<StubFactoryImpl&>(*_stubFactory).getChannelPool()->dropChannelsByTarget(target);
}

void GRPCClient::_dropPendingStreamEstablishments(
    std::function<bool(const HostAndPort&)> shouldDrop) {
    stdx::lock_guard lk(_mutex);
    // Cancel all outstanding connect timers and stream establishment attempts.
    for (auto&& it : _remoteStates) {
        if (!it.second.keepOpen && shouldDrop(it.first)) {
            for (auto&& streamState : it.second.pendingStreamStates) {
                streamState->cancel(makeDroppedConnectionCancelStatus());
            }
        }
    }
}

void GRPCClient::setKeepOpen(const HostAndPort& hostAndPort, bool keepOpen) {
    stdx::lock_guard lk(_mutex);
    _remoteStates[hostAndPort].keepOpen = keepOpen;

    static_cast<StubFactoryImpl&>(*_stubFactory)
        .getChannelPool()
        ->setKeepOpen(hostAndPort, keepOpen);
}

Future<Client::CallContext> GRPCClient::_streamFactory(const HostAndPort& remote,
                                                       const std::shared_ptr<GRPCReactor>& reactor,
                                                       boost::optional<Date_t> deadline,
                                                       const ConnectOptions& options,
                                                       const CancellationToken& token) {
    auto stub =
        static_cast<StubFactoryImpl&>(*_stubFactory).createStub(std::move(remote), options.sslMode);
    auto ctx = std::make_shared<GRPCClientContext>();
    setMetadataOnClientContext(*ctx, options);
    // Rather than using gRPC's wait for ready functionality, we call waitForConnected for better
    // visibility into timeouts.
    ctx->setWaitForReady(false);

    return stub->stub()
        .getChannel()
        ->waitForConnected(reactor, deadline ? deadline.get() : Date_t::max(), token)
        .then([stub = stub.get(), clientContext = std::move(ctx), reactor, options]() {
            auto stream = [&]() {
                if (options.authToken) {
                    return stub->stub().authenticatedCommandStream(clientContext.get(), reactor);
                } else {
                    return stub->stub().unauthenticatedCommandStream(clientContext.get(), reactor);
                }
            }();

            return Client::CallContext{std::move(clientContext),
                                       std::move(stream),
                                       stub->stub().getChannel()->getSSLConfiguration(),
                                       stub->stub().getChannel()->getId()};
        })
        .onCompletion([stub = std::move(stub)](const StatusWith<CallContext>& swCall) mutable {
            // We manually reset the stub unique_ptr here so that the Promise/Future used here
            // doesn't hold onto the stub after we are finished with it.
            stub.reset();
            return swCall;
        });
}

}  // namespace mongo::transport::grpc
