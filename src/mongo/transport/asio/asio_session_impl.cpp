/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/transport/asio/asio_session_impl.h"

#include "mongo/config.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/connection_health_metrics_parameter_gen.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/stats/counters.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/asio/asio_utils.h"
#include "mongo/transport/proxy_protocol_header_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future_util.h"
#include "mongo/util/net/socket_utils.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::transport {

MONGO_FAIL_POINT_DEFINE(asioTransportLayerShortOpportunisticReadWrite);
MONGO_FAIL_POINT_DEFINE(asioTransportLayerSessionPauseBeforeSetSocketOption);
MONGO_FAIL_POINT_DEFINE(asioTransportLayerBlockBeforeOpportunisticRead);
MONGO_FAIL_POINT_DEFINE(asioTransportLayerBlockBeforeAddSession);

namespace {

template <int optionName>
class ASIOSocketTimeoutOption {
public:
#ifdef _WIN32
    using TimeoutType = DWORD;

    explicit ASIOSocketTimeoutOption(Milliseconds timeoutVal) : _timeout(timeoutVal.count()) {}

#else
    using TimeoutType = timeval;

    explicit ASIOSocketTimeoutOption(Milliseconds timeoutVal) {
        _timeout.tv_sec = duration_cast<Seconds>(timeoutVal).count();
        const auto minusSeconds = timeoutVal - Seconds{_timeout.tv_sec};
        _timeout.tv_usec = duration_cast<Microseconds>(minusSeconds).count();
    }
#endif

    template <typename Protocol>
    int name(const Protocol&) const {
        return optionName;
    }

    template <typename Protocol>
    const TimeoutType* data(const Protocol&) const {
        return &_timeout;
    }

    template <typename Protocol>
    std::size_t size(const Protocol&) const {
        return sizeof(_timeout);
    }

    template <typename Protocol>
    int level(const Protocol&) const {
        return SOL_SOCKET;
    }

private:
    TimeoutType _timeout;
};

Status makeCanceledStatus() {
    return {ErrorCodes::CallbackCanceled, "Operation was canceled"};
}

bool connHealthMetricsEnabled() {
    // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
    return gFeatureFlagConnHealthMetrics.isEnabledAndIgnoreFCVUnsafe();
}

CounterMetric totalIngressTLSConnections("network.totalIngressTLSConnections",
                                         connHealthMetricsEnabled);
CounterMetric totalIngressTLSHandshakeTimeMillis("network.totalIngressTLSHandshakeTimeMillis",
                                                 connHealthMetricsEnabled);
}  // namespace


CommonAsioSession::CommonAsioSession(
    AsioTransportLayer* tl,
    GenericSocket socket,
    bool isIngressSession,
    Endpoint endpoint,
    std::shared_ptr<const SSLConnectionContext> transientSSLContext) try
    : _socket(std::move(socket)), _tl(tl), _isIngressSession(isIngressSession) {
    auto family = endpointToSockAddr(_socket.local_endpoint()).getType();
    auto sev = logv2::LogSeverity::Debug(3);
    if (family == AF_INET || family == AF_INET6) {
        asioTransportLayerSessionPauseBeforeSetSocketOption.pauseWhileSet();
        setSocketOption(_socket, asio::ip::tcp::no_delay(true), "session no delay", sev);
        setSocketOption(_socket, asio::socket_base::keep_alive(true), "session keep alive", sev);
        setSocketKeepAliveParams(_socket.native_handle(), sev);
    }

    _localAddr = endpointToSockAddr(_socket.local_endpoint());

    if (endpoint == Endpoint()) {
        // Inbound connection, query socket for remote.
        _remoteAddr = endpointToSockAddr(_socket.remote_endpoint());
    } else {
        // Outbound connection, get remote from resolved endpoint.
        // Necessary for TCP_FASTOPEN where the remote isn't connected yet.
        _remoteAddr = endpointToSockAddr(endpoint);
    }

    _local = HostAndPort(_localAddr.toString(true));
    if (tl->loadBalancerPort()) {
        _isFromLoadBalancer = _local.port() == *tl->loadBalancerPort();
    }

    _remote = HostAndPort(_remoteAddr.toString(true));
#ifdef MONGO_CONFIG_SSL
    _sslContext = transientSSLContext ? transientSSLContext : tl->sslContext();
    if (transientSSLContext) {
        logv2::DynamicAttributes attrs;
        if (transientSSLContext->targetClusterURI) {
            attrs.add("targetClusterURI", *transientSSLContext->targetClusterURI);
        }
        attrs.add("isIngress", isIngressSession);
        attrs.add("connectionId", id());
        attrs.add("remote", remote());
        LOGV2(5271001, "Initializing the AsioSession with transient SSL context", attrs);
    }
#endif
} catch (const DBException&) {
    throw;
} catch (const asio::system_error&) {
    throw;
}

void CommonAsioSession::end() {
    if (getSocket().is_open()) {
        std::error_code ec;
        getSocket().shutdown(GenericSocket::shutdown_both, ec);
        if ((ec) && (ec != asio::error::not_connected)) {
            LOGV2_ERROR(23841,
                        "Error shutting down socket: {error}",
                        "Error shutting down socket",
                        "error"_attr = ec.message());
        }
    }
}

StatusWith<Message> CommonAsioSession::sourceMessage() noexcept try {
    ensureSync();
    return sourceMessageImpl().getNoThrow();
} catch (const DBException& ex) {
    return ex.toStatus();
}

Future<Message> CommonAsioSession::asyncSourceMessage(const BatonHandle& baton) noexcept try {
    ensureAsync();
    return sourceMessageImpl(baton);
} catch (const DBException& ex) {
    return ex.toStatus();
}

Status CommonAsioSession::waitForData() noexcept try {
    ensureSync();
    asio::error_code ec;
    getSocket().wait(asio::ip::tcp::socket::wait_read, ec);
    return errorCodeToStatus(ec, "waitForData");
} catch (const DBException& ex) {
    return ex.toStatus();
}

Future<void> CommonAsioSession::asyncWaitForData() noexcept try {
    ensureAsync();
    return getSocket().async_wait(asio::ip::tcp::socket::wait_read, UseFuture{});
} catch (const DBException& ex) {
    return ex.toStatus();
}

Status CommonAsioSession::sinkMessage(Message message) noexcept try {
    ensureSync();
    return sinkMessageImpl(std::move(message)).getNoThrow();
} catch (const DBException& ex) {
    return ex.toStatus();
}

Future<void> CommonAsioSession::asyncSinkMessage(Message message, const BatonHandle& baton) noexcept
    try {
    ensureAsync();
    return sinkMessageImpl(std::move(message), baton);
} catch (const DBException& ex) {
    return ex.toStatus();
}

Future<void> CommonAsioSession::sinkMessageImpl(Message message, const BatonHandle& baton) {
    _asyncOpState.start();
    return write(asio::buffer(message.buf(), message.size()), baton)
        .then([this, message /*keep the buffer alive*/]() {
            if (_isIngressSession) {
                networkCounter.hitPhysicalOut(message.size());
            }
        })
        .onCompletion([this](Status status) {
            _asyncOpState.complete();
            return status;
        });
}

void CommonAsioSession::cancelAsyncOperations(const BatonHandle& baton) {
    LOGV2_DEBUG(4615608,
                3,
                "Canceling outstanding I/O operations on connection to {remote}",
                "Canceling outstanding I/O operations on connection to remote",
                "remote"_attr = _remote);
    stdx::lock_guard lk(_asyncOpMutex);
    _asyncOpState.cancel();
    if (baton && baton->networking() && baton->networking()->cancelSession(*this)) {
        // If we have a baton, it was for networking, and it owned our session, then we're done.
        return;
    }

    getSocket().cancel();
}

void CommonAsioSession::setTimeout(boost::optional<Milliseconds> timeout) {
    invariant(!timeout || timeout->count() > 0);
    _configuredTimeout = timeout;
}

bool CommonAsioSession::isConnected() {
    // socket.is_open() only returns whether the socket is a valid file descriptor and
    // if we haven't marked this socket as closed already.
    if (!getSocket().is_open())
        return false;

    auto swPollEvents = pollASIOSocket(getSocket(), POLLIN, Milliseconds{0});
    if (!swPollEvents.isOK()) {
        if (swPollEvents != ErrorCodes::NetworkTimeout) {
            LOGV2_WARNING(4615609,
                          "Failed to poll socket for connectivity check: {error}",
                          "Failed to poll socket for connectivity check",
                          "error"_attr = swPollEvents.getStatus());
            return false;
        }
        return true;
    }

    auto revents = swPollEvents.getValue();
    if (revents & POLLIN) {
        try {
            char testByte;
            const auto bytesRead =
                peekASIOStream(getSocket(), asio::buffer(&testByte, sizeof(testByte)));
            uassert(ErrorCodes::SocketException,
                    "Couldn't peek from underlying socket",
                    bytesRead == sizeof(testByte));
            return true;
        } catch (const DBException& e) {
            LOGV2_WARNING(4615610,
                          "Failed to check socket connectivity: {error}",
                          "Failed to check socket connectivity",
                          "error"_attr = e);
        }
    }

    return false;
}

#ifdef MONGO_CONFIG_SSL

const std::shared_ptr<SSLManagerInterface>& CommonAsioSession::getSSLManager() const {
    return _sslContext->manager;
}

Status CommonAsioSession::buildSSLSocket(const HostAndPort& target) {
    invariant(!_sslSocket, "SSL socket is already constructed");
    if (!_sslContext->egress) {
        return Status(ErrorCodes::SSLHandshakeFailed, "SSL requested but SSL support is disabled");
    }
    _sslSocket.emplace(std::move(_socket), *_sslContext->egress, removeFQDNRoot(target.host()));
    return Status::OK();
}

Future<void> CommonAsioSession::handshakeSSLForEgress(const HostAndPort& target,
                                                      const ReactorHandle& reactor) {
    invariant(_sslSocket, "SSL Socket expected to be built");
    auto doHandshake = [&] {
        if (_blockingMode == sync) {
            std::error_code ec;
            _sslSocket->handshake(asio::ssl::stream_base::client, ec);
            return futurize(ec);
        } else {
            return _sslSocket->async_handshake(asio::ssl::stream_base::client, UseFuture{});
        }
    };
    return doHandshake().then([this, target, reactor] {
        _ranHandshake = true;

        return getSSLManager()
            ->parseAndValidatePeerCertificate(
                _sslSocket->native_handle(), _sslSocket->get_sni(), target.host(), target, reactor)
            .then([this](SSLPeerInfo info) { SSLPeerInfo::forSession(shared_from_this()) = info; });
    });
}
#endif

void AsyncAsioSession::ensureSync() {
    invariant(false, "Attempted to use AsyncAsioSession in sync mode.");
}

void SyncAsioSession::ensureSync() {
    asio::error_code ec;
    if (_blockingMode != sync) {
        getSocket().non_blocking(false, ec);
        fassert(40490, errorCodeToStatus(ec, "ensureSync non_blocking"));
        _blockingMode = sync;
    }

    if (_socketTimeout != _configuredTimeout) {
        // Change boost::none (which means no timeout) into a zero value for the socket option,
        // which also means no timeout.
        auto timeout = _configuredTimeout.value_or(Milliseconds{0});
        setSocketOption(getSocket(),
                        ASIOSocketTimeoutOption<SO_SNDTIMEO>(timeout),
                        "session send timeout",
                        logv2::LogSeverity::Info(),
                        ec);
        uassertStatusOK(errorCodeToStatus(ec, "ensureSync session send timeout"));

        setSocketOption(getSocket(),
                        ASIOSocketTimeoutOption<SO_RCVTIMEO>(timeout),
                        "session receive timeout",
                        logv2::LogSeverity::Info(),
                        ec);
        uassertStatusOK(errorCodeToStatus(ec, "ensureSync session receive timeout"));

        _socketTimeout = _configuredTimeout;
    }
}

void AsyncAsioSession::ensureAsync() {
    if (_blockingMode == async)
        return;

    // Socket timeouts currently only effect synchronous calls, so make sure the caller isn't
    // expecting a socket timeout when they do an async operation.
    invariant(!_configuredTimeout);

    asio::error_code ec;
    getSocket().non_blocking(true, ec);
    fassert(50706, errorCodeToStatus(ec, "ensureAsync non_blocking"));
    _blockingMode = async;
}

void SyncAsioSession::ensureAsync() {
    invariant(false, "Attempted to use SyncAsioSession in async mode.");
}

auto CommonAsioSession::getSocket() -> GenericSocket& {
#ifdef MONGO_CONFIG_SSL
    if (_sslSocket) {
        return static_cast<GenericSocket&>(_sslSocket->lowest_layer());
    }
#endif
    return _socket;
}

ExecutorFuture<void> CommonAsioSession::parseProxyProtocolHeader(const ReactorHandle& reactor) {
    invariant(_isIngressSession);
    invariant(reactor);
    auto buffer = std::make_shared<std::array<char, kProxyProtocolHeaderSizeUpperBound>>();
    return AsyncTry([this, buffer] {
               const auto bytesRead = peekASIOStream(
                   _socket, asio::buffer(buffer->data(), kProxyProtocolHeaderSizeUpperBound));
               return transport::parseProxyProtocolHeader(StringData(buffer->data(), bytesRead));
           })
        .until([](StatusWith<boost::optional<ParserResults>> sw) {
            return !sw.isOK() || sw.getValue();
        })
        .on(reactor, CancellationToken::uncancelable())
        .then([this, buffer](const boost::optional<ParserResults>& results) mutable {
            invariant(results);

            // There may not be any endpoints if this connection is directly
            // from the proxy itself or the information isn't available.
            if (results->endpoints) {
                _proxiedSrcEndpoint = results->endpoints->sourceAddress;
                _proxiedDstEndpoint = results->endpoints->destinationAddress;
            } else {
                _proxiedSrcEndpoint = {};
                _proxiedDstEndpoint = {};
            }

            // `opportunisticRead` expects to run as part of an asynchronous operation. We start the
            // operation below and make sure to mark it as completed, regardless of the completion
            // status of the future continuation returned by `opportunisticRead`.
            _asyncOpState.start();
            ScopeGuard guard([&] { _asyncOpState.complete(); });

            // Drain the read buffer.
            opportunisticRead(_socket, asio::buffer(buffer.get(), results->bytesParsed)).get();
        })
        .onError([this](Status s) {
            LOGV2_ERROR(
                6067900, "Error while parsing proxy protocol header", "error"_attr = redact(s));
            end();
            return s;
        });
}

Future<Message> CommonAsioSession::sourceMessageImpl(const BatonHandle& baton) {
    static constexpr auto kHeaderSize = sizeof(MSGHEADER::Value);

    auto headerBuffer = SharedBuffer::allocate(kHeaderSize);
    auto ptr = headerBuffer.get();
    _asyncOpState.start();
    return read(asio::buffer(ptr, kHeaderSize), baton)
        .then([headerBuffer = std::move(headerBuffer), this, baton]() mutable {
            if (checkForHTTPRequest(asio::buffer(headerBuffer.get(), kHeaderSize))) {
                return sendHTTPResponse(baton);
            }

            const auto msgLen = size_t(MSGHEADER::View(headerBuffer.get()).getMessageLength());
            if (msgLen < kHeaderSize || msgLen > MaxMessageSizeBytes) {
                StringBuilder sb;
                sb << "recv(): message msgLen " << msgLen << " is invalid. "
                   << "Min " << kHeaderSize << " Max: " << MaxMessageSizeBytes;
                const auto str = sb.str();
                LOGV2(4615638,
                      "recv(): message msgLen {msgLen} is invalid. Min: {min} Max: {max}",
                      "recv(): message mstLen is invalid.",
                      "msgLen"_attr = msgLen,
                      "min"_attr = kHeaderSize,
                      "max"_attr = MaxMessageSizeBytes);

                return Future<Message>::makeReady(Status(ErrorCodes::ProtocolError, str));
            }

            if (msgLen == kHeaderSize) {
                // This probably isn't a real case since all (current) messages have bodies.
                if (_isIngressSession) {
                    networkCounter.hitPhysicalIn(msgLen);
                }
                return Future<Message>::makeReady(Message(std::move(headerBuffer)));
            }

            auto buffer = SharedBuffer::allocate(msgLen);
            memcpy(buffer.get(), headerBuffer.get(), kHeaderSize);

            MsgData::View msgView(buffer.get());
            return read(asio::buffer(msgView.data(), msgView.dataLen()), baton)
                .then([this, buffer = std::move(buffer), msgLen]() mutable {
                    if (_isIngressSession) {
                        networkCounter.hitPhysicalIn(msgLen);
                    }
                    return Message(std::move(buffer));
                });
        })
        .onCompletion([this](StatusWith<Message> swMessage) {
            _asyncOpState.complete();
            return swMessage;
        });
}

template <typename MutableBufferSequence>
Future<void> CommonAsioSession::read(const MutableBufferSequence& buffers,
                                     const BatonHandle& baton) {
#ifdef MONGO_CONFIG_SSL
    if (_sslSocket) {
        return opportunisticRead(*_sslSocket, buffers, baton);
    } else if (!_ranHandshake) {
        invariant(asio::buffer_size(buffers) >= sizeof(MSGHEADER::Value));

        return opportunisticRead(_socket, buffers, baton)
            .then([this, buffers]() mutable {
                _ranHandshake = true;
                return maybeHandshakeSSLForIngress(buffers);
            })
            .then([this, buffers, baton](bool needsRead) mutable {
                if (needsRead) {
                    return read(buffers, baton);
                } else {
                    return Future<void>::makeReady();
                }
            });
    }
#endif
    return opportunisticRead(_socket, buffers, baton);
}

template <typename ConstBufferSequence>
Future<void> CommonAsioSession::write(const ConstBufferSequence& buffers,
                                      const BatonHandle& baton) {
#ifdef MONGO_CONFIG_SSL
    _ranHandshake = true;
    if (_sslSocket) {
#ifdef __linux__
        // We do some trickery in asio (see moreToSend), which appears to work well on linux,
        // but fails on other platforms.
        return opportunisticWrite(*_sslSocket, buffers, baton);
#else
        if (_blockingMode == async) {
            // Opportunistic writes are broken for async egress SSL (switching between blocking
            // and non-blocking mode corrupts the TLS exchange).
            return asio::async_write(*_sslSocket, buffers, UseFuture{}).ignoreValue();
        } else {
            return opportunisticWrite(*_sslSocket, buffers, baton);
        }
#endif
    }
#endif  // MONGO_CONFIG_SSL
    return opportunisticWrite(_socket, buffers, baton);
}

template <typename Stream, typename MutableBufferSequence>
Future<void> CommonAsioSession::opportunisticRead(Stream& stream,
                                                  const MutableBufferSequence& buffers,
                                                  const BatonHandle& baton) {
    std::error_code ec;
    size_t size;

    asioTransportLayerBlockBeforeOpportunisticRead.pauseWhileSet();

    if (MONGO_unlikely(asioTransportLayerShortOpportunisticReadWrite.shouldFail()) &&
        _blockingMode == async) {
        asio::mutable_buffer localBuffer = buffers;

        if (buffers.size()) {
            localBuffer = asio::mutable_buffer(buffers.data(), 1);
        }

        do {
            size = asio::read(stream, localBuffer, ec);
        } while (ec == asio::error::interrupted);  // retry syscall EINTR

        if (!ec && buffers.size() > 1) {
            ec = asio::error::would_block;
        }
    } else {
        do {
            size = asio::read(stream, buffers, ec);
        } while (ec == asio::error::interrupted);  // retry syscall EINTR
    }

    if (((ec == asio::error::would_block) || (ec == asio::error::try_again)) &&
        (_blockingMode == async)) {
        // asio::read is a loop internally, so some of buffers may have been read into already.
        // So we need to adjust the buffers passed into async_read to be offset by size, if
        // size is > 0.
        MutableBufferSequence asyncBuffers(buffers);
        if (size > 0) {
            asyncBuffers += size;
        }

        stdx::lock_guard lk(_asyncOpMutex);
        if (_asyncOpState.isCanceled())
            return makeCanceledStatus();
        if (auto networkingBaton = baton ? baton->networking() : nullptr;
            networkingBaton && networkingBaton->canWait()) {
            asioTransportLayerBlockBeforeAddSession.pauseWhileSet();
            return networkingBaton->addSession(*this, NetworkingBaton::Type::In)
                .onError([](Status error) {
                    if (ErrorCodes::isShutdownError(error)) {
                        // If the baton has detached, it will cancel its polling. We catch that
                        // error here and return Status::OK so that we invoke
                        // opportunisticRead() again and switch to asio::async_read() below.
                        return Status::OK();
                    }

                    return error;
                })
                .then([&stream, asyncBuffers, baton, this] {
                    return opportunisticRead(stream, asyncBuffers, baton);
                });
        }

        return asio::async_read(stream, asyncBuffers, UseFuture{}).ignoreValue();
    } else {
        return futurize(ec);
    }
}

template <typename Stream, typename ConstBufferSequence>
Future<void> CommonAsioSession::opportunisticWrite(Stream& stream,
                                                   const ConstBufferSequence& buffers,
                                                   const BatonHandle& baton) {
    std::error_code ec;
    std::size_t size;

    if (MONGO_unlikely(asioTransportLayerShortOpportunisticReadWrite.shouldFail()) &&
        _blockingMode == async) {
        asio::const_buffer localBuffer = buffers;

        if (buffers.size()) {
            localBuffer = asio::const_buffer(buffers.data(), 1);
        }

        do {
            size = asio::write(stream, localBuffer, ec);
        } while (ec == asio::error::interrupted);  // retry syscall EINTR
        if (!ec && buffers.size() > 1) {
            ec = asio::error::would_block;
        }
    } else {
        do {
            size = asio::write(stream, buffers, ec);
        } while (ec == asio::error::interrupted);  // retry syscall EINTR
    }

    if (((ec == asio::error::would_block) || (ec == asio::error::try_again)) &&
        (_blockingMode == async)) {

        // asio::write is a loop internally, so some of buffers may have been read into already.
        // So we need to adjust the buffers passed into async_write to be offset by size, if
        // size is > 0.
        ConstBufferSequence asyncBuffers(buffers);
        if (size > 0) {
            asyncBuffers += size;
        }

        if (auto more = moreToSend(stream, asyncBuffers, baton)) {
            return std::move(*more);
        }

        stdx::lock_guard lk(_asyncOpMutex);
        if (_asyncOpState.isCanceled())
            return makeCanceledStatus();
        if (auto networkingBaton = baton ? baton->networking() : nullptr;
            networkingBaton && networkingBaton->canWait()) {
            asioTransportLayerBlockBeforeAddSession.pauseWhileSet();
            return networkingBaton->addSession(*this, NetworkingBaton::Type::Out)
                .onError([](Status error) {
                    if (ErrorCodes::isShutdownError(error)) {
                        // If the baton has detached, it will cancel its polling. We catch that
                        // error here and return Status::OK so that we invoke
                        // opportunisticWrite() again and switch to asio::async_write() below.
                        return Status::OK();
                    }

                    return error;
                })
                .then([&stream, asyncBuffers, baton, this] {
                    return opportunisticWrite(stream, asyncBuffers, baton);
                });
        }

        return asio::async_write(stream, asyncBuffers, UseFuture{}).ignoreValue();
    } else {
        return futurize(ec);
    }
}

#ifdef MONGO_CONFIG_SSL
template <typename MutableBufferSequence>
Future<bool> CommonAsioSession::maybeHandshakeSSLForIngress(const MutableBufferSequence& buffer) {
    invariant(asio::buffer_size(buffer) >= sizeof(MSGHEADER::Value));
    MSGHEADER::ConstView headerView(asio::buffer_cast<char*>(buffer));
    auto responseTo = headerView.getResponseToMsgId();

    if (checkForHTTPRequest(buffer)) {
        return Future<bool>::makeReady(false);
    }
    // This logic was taken from the old mongo/util/net/sock.cpp.
    //
    // It lets us run both TLS and unencrypted mongo over the same port.
    //
    // The first message received from the client should have the responseTo field of the wire
    // protocol message needs to be 0 or -1. Otherwise the connection is either sending
    // garbage or a TLS Hello packet which will be caught by the TLS handshake.
    if (responseTo != 0 && responseTo != -1) {
        if (!_sslContext->ingress) {
            return Future<bool>::makeReady(
                Status(ErrorCodes::SSLHandshakeFailed,
                       "SSL handshake received but server is started without SSL support"));
        }

        auto tlsAlert = checkTLSRequest(buffer);
        if (tlsAlert) {
            return opportunisticWrite(getSocket(), asio::buffer(tlsAlert->data(), tlsAlert->size()))
                .then([] {
                    return Future<bool>::makeReady(
                        Status(ErrorCodes::SSLHandshakeFailed,
                               "SSL handshake failed, as client requested disabled protocol"));
                });
        }

        _sslSocket.emplace(std::move(_socket), *_sslContext->ingress, "");
        auto doHandshake = [&] {
            if (_blockingMode == sync) {
                std::error_code ec;
                _sslSocket->handshake(asio::ssl::stream_base::server, buffer, ec);
                return futurize(ec, asio::buffer_size(buffer));
            } else {
                return _sslSocket->async_handshake(
                    asio::ssl::stream_base::server, buffer, UseFuture{});
            }
        };
        auto startTimer = Timer();
        return doHandshake().then([this, startTimer = std::move(startTimer)](size_t size) {
            if (_sslSocket->get_sni()) {
                auto sniName = _sslSocket->get_sni().value();
                LOGV2_DEBUG(
                    4908000, 2, "Client connected with SNI extension", "sniName"_attr = sniName);
            } else {
                LOGV2_DEBUG(4908001, 2, "Client connected without SNI extension");
            }
            const auto handshakeDurationMillis = durationCount<Milliseconds>(startTimer.elapsed());
            if (gEnableDetailedConnectionHealthMetricLogLines) {
                LOGV2(6723804,
                      "Ingress TLS handshake complete",
                      "durationMillis"_attr = handshakeDurationMillis);
            }
            totalIngressTLSConnections.increment(1);
            totalIngressTLSHandshakeTimeMillis.increment(handshakeDurationMillis);
            if (SSLPeerInfo::forSession(shared_from_this()).subjectName().empty()) {
                return getSSLManager()
                    ->parseAndValidatePeerCertificate(
                        _sslSocket->native_handle(), _sslSocket->get_sni(), "", _remote, nullptr)
                    .then([this](SSLPeerInfo info) -> bool {
                        SSLPeerInfo::forSession(shared_from_this()) = info;
                        return true;
                    });
            }

            return Future<bool>::makeReady(true);
        });
    } else if (_tl->sslMode() == SSLParams::SSLMode_requireSSL) {
        uasserted(ErrorCodes::SSLHandshakeFailed,
                  "The server is configured to only allow SSL connections");
    } else {
        if (!sslGlobalParams.disableNonSSLConnectionLogging &&
            _tl->sslMode() == SSLParams::SSLMode_preferSSL) {
            LOGV2(23838,
                  "SSL mode is set to 'preferred' and connection {connectionId} to {remote} is "
                  "not using SSL.",
                  "SSL mode is set to 'preferred' and connection to remote is not using SSL.",
                  "connectionId"_attr = id(),
                  "remote"_attr = remote());
        }
        return Future<bool>::makeReady(false);
    }
}
#endif  // MONGO_CONFIG_SSL

template <typename Buffer>
bool CommonAsioSession::checkForHTTPRequest(const Buffer& buffers) {
    invariant(asio::buffer_size(buffers) >= 4);
    const StringData bufferAsStr(asio::buffer_cast<const char*>(buffers), 4);
    return (bufferAsStr == "GET "_sd);
}

Future<Message> CommonAsioSession::sendHTTPResponse(const BatonHandle& baton) {
    constexpr auto userMsg =
        "It looks like you are trying to access MongoDB over HTTP"
        " on the native driver port.\r\n"_sd;

    static const std::string httpResp = str::stream() << "HTTP/1.0 200 OK\r\n"
                                                         "Connection: close\r\n"
                                                         "Content-Type: text/plain\r\n"
                                                         "Content-Length: "
                                                      << userMsg.size() << "\r\n\r\n"
                                                      << userMsg;

    return write(asio::buffer(httpResp.data(), httpResp.size()), baton)
        .onError([](const Status& status) {
            return Status(ErrorCodes::ProtocolError,
                          str::stream()
                              << "Client sent an HTTP request over a native MongoDB connection, "
                                 "but there was an error sending a response: "
                              << status.toString());
        })
        .then([] {
            return StatusWith<Message>(
                ErrorCodes::ProtocolError,
                "Client sent an HTTP request over a native MongoDB connection");
        });
}

}  // namespace mongo::transport
