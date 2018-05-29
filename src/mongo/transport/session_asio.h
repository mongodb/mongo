/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <utility>

#include "mongo/base/system_error.h"
#include "mongo/config.h"
#include "mongo/db/stats/counters.h"
#include "mongo/transport/asio_utils.h"
#include "mongo/transport/baton.h"
#include "mongo/transport/transport_layer_asio.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/socket_utils.h"
#ifdef MONGO_CONFIG_SSL
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_types.h"
#endif

#include "asio.hpp"
#ifdef MONGO_CONFIG_SSL
#include "mongo/util/net/ssl.hpp"
#endif

namespace mongo {
namespace transport {

MONGO_FAIL_POINT_DEFINE(transportLayerASIOshortOpportunisticReadWrite);

template <typename SuccessValue>
auto futurize(const std::error_code& ec, SuccessValue&& successValue) {
    using Result = Future<std::decay_t<SuccessValue>>;
    if (MONGO_unlikely(ec)) {
        return Result::makeReady(errorCodeToStatus(ec));
    }
    return Result::makeReady(successValue);
}

Future<void> futurize(const std::error_code& ec) {
    using Result = Future<void>;
    if (MONGO_unlikely(ec)) {
        return Result::makeReady(errorCodeToStatus(ec));
    }
    return Result::makeReady();
}

using GenericSocket = asio::generic::stream_protocol::socket;

class TransportLayerASIO::ASIOSession final : public Session {
    MONGO_DISALLOW_COPYING(ASIOSession);

public:
    // If the socket is disconnected while any of these options are being set, this constructor
    // may throw, but it is guaranteed to throw a mongo DBException.
    ASIOSession(TransportLayerASIO* tl, GenericSocket socket, bool isIngressSession) try
        : _socket(std::move(socket)),
          _tl(tl),
          _isIngressSession(isIngressSession) {
        auto family = endpointToSockAddr(_socket.local_endpoint()).getType();
        if (family == AF_INET || family == AF_INET6) {
            _socket.set_option(asio::ip::tcp::no_delay(true));
            _socket.set_option(asio::socket_base::keep_alive(true));
            setSocketKeepAliveParams(_socket.native_handle());
        }

        _local = endpointToHostAndPort(_socket.local_endpoint());
        _remote = endpointToHostAndPort(_socket.remote_endpoint());
    } catch (const DBException&) {
        throw;
    } catch (const asio::system_error& error) {
        uasserted(ErrorCodes::SocketException, error.what());
    } catch (...) {
        uasserted(50797, str::stream() << "Unknown exception while configuring socket.");
    }

    ~ASIOSession() {
        end();
    }

    TransportLayer* getTransportLayer() const override {
        return _tl;
    }

    const HostAndPort& remote() const override {
        return _remote;
    }

    const HostAndPort& local() const override {
        return _local;
    }

    void end() override {
        if (getSocket().is_open()) {
            std::error_code ec;
            cancelAsyncOperations();
            getSocket().shutdown(GenericSocket::shutdown_both, ec);
            if ((ec) && (ec != asio::error::not_connected)) {
                error() << "Error shutting down socket: " << ec.message();
            }
        }
    }

    StatusWith<Message> sourceMessage() override {
        ensureSync();
        return sourceMessageImpl().getNoThrow();
    }

    Future<Message> asyncSourceMessage(const transport::BatonHandle& baton = nullptr) override {
        ensureAsync();
        return sourceMessageImpl(baton);
    }

    Status sinkMessage(Message message) override {
        ensureSync();

        return write(asio::buffer(message.buf(), message.size()))
            .then([this, &message] {
                if (_isIngressSession) {
                    networkCounter.hitPhysicalOut(message.size());
                }
            })
            .getNoThrow();
    }

    Future<void> asyncSinkMessage(Message message,
                                  const transport::BatonHandle& baton = nullptr) override {
        ensureAsync();
        return write(asio::buffer(message.buf(), message.size()), baton)
            .then([this, message /*keep the buffer alive*/]() {
                if (_isIngressSession) {
                    networkCounter.hitPhysicalOut(message.size());
                }
            });
    }

    void cancelAsyncOperations(const transport::BatonHandle& baton = nullptr) override {
        LOG(3) << "Cancelling outstanding I/O operations on connection to " << _remote;
        if (baton) {
            baton->cancelSession(*this);
        } else {
            getSocket().cancel();
        }
    }

    void setTimeout(boost::optional<Milliseconds> timeout) override {
        invariant(!timeout || timeout->count() > 0);
        _configuredTimeout = timeout;
    }

    bool isConnected() override {
        // socket.is_open() only returns whether the socket is a valid file descriptor and
        // if we haven't marked this socket as closed already.
        if (!getSocket().is_open())
            return false;

        auto swPollEvents = pollASIOSocket(getSocket(), POLLIN, Milliseconds{0});
        if (!swPollEvents.isOK()) {
            if (swPollEvents != ErrorCodes::NetworkTimeout) {
                warning() << "Failed to poll socket for connectivity check: "
                          << swPollEvents.getStatus();
                return false;
            }
            return true;
        }

        auto revents = swPollEvents.getValue();
        if (revents & POLLIN) {
            char testByte;
            int size = ::recv(getSocket().native_handle(), &testByte, sizeof(testByte), MSG_PEEK);
            if (size == sizeof(testByte)) {
                return true;
            } else if (size == -1) {
                auto errDesc = errnoWithDescription(errno);
                warning() << "Failed to check socket connectivity: " << errDesc;
            }
            // If size == 0 then we got disconnected and we should return false.
        }

        return false;
    }

protected:
    friend class TransportLayerASIO;
    friend TransportLayerASIO::BatonASIO;

#ifdef MONGO_CONFIG_SSL
    Future<void> handshakeSSLForEgress(const HostAndPort& target) {
        if (!_tl->_egressSSLContext) {
            return Future<void>::makeReady(Status(ErrorCodes::SSLHandshakeFailed,
                                                  "SSL requested but SSL support is disabled"));
        }

        _sslSocket.emplace(
            std::move(_socket), *_tl->_egressSSLContext, removeFQDNRoot(target.host()));
        auto doHandshake = [&] {
            if (_blockingMode == Sync) {
                std::error_code ec;
                _sslSocket->handshake(asio::ssl::stream_base::client, ec);
                return Future<void>::makeReady(errorCodeToStatus(ec));
            } else {
                return _sslSocket->async_handshake(asio::ssl::stream_base::client, UseFuture{});
            }
        };
        return doHandshake().then([this, target] {
            _ranHandshake = true;

            auto sslManager = getSSLManager();
            auto swPeerInfo = uassertStatusOK(sslManager->parseAndValidatePeerCertificate(
                _sslSocket->native_handle(), target.host()));

            if (swPeerInfo) {
                SSLPeerInfo::forSession(shared_from_this()) = std::move(*swPeerInfo);
            }
        });
    }
#endif

    void ensureSync() {
        asio::error_code ec;
        if (_blockingMode != Sync) {
            getSocket().non_blocking(false, ec);
            fassert(40490, errorCodeToStatus(ec));
            _blockingMode = Sync;
        }

        if (_socketTimeout != _configuredTimeout) {
            // Change boost::none (which means no timeout) into a zero value for the socket option,
            // which also means no timeout.
            auto timeout = _configuredTimeout.value_or(Milliseconds{0});
            getSocket().set_option(ASIOSocketTimeoutOption<SO_SNDTIMEO>(timeout), ec);
            uassertStatusOK(errorCodeToStatus(ec));

            getSocket().set_option(ASIOSocketTimeoutOption<SO_RCVTIMEO>(timeout), ec);
            uassertStatusOK(errorCodeToStatus(ec));

            _socketTimeout = _configuredTimeout;
        }
    }

    void ensureAsync() {
        if (_blockingMode == Async)
            return;

        // Socket timeouts currently only effect synchronous calls, so make sure the caller isn't
        // expecting a socket timeout when they do an async operation.
        invariant(!_configuredTimeout);

        asio::error_code ec;
        getSocket().non_blocking(true, ec);
        fassert(50706, errorCodeToStatus(ec));
        _blockingMode = Async;
    }

private:
    template <int Name>
    class ASIOSocketTimeoutOption {
    public:
#ifdef _WIN32
        using TimeoutType = DWORD;

        ASIOSocketTimeoutOption(Milliseconds timeoutVal) : _timeout(timeoutVal.count()) {}

#else
        using TimeoutType = timeval;

        ASIOSocketTimeoutOption(Milliseconds timeoutVal) {
            _timeout.tv_sec = duration_cast<Seconds>(timeoutVal).count();
            const auto minusSeconds = timeoutVal - Seconds{_timeout.tv_sec};
            _timeout.tv_usec = duration_cast<Microseconds>(minusSeconds).count();
        }
#endif

        template <typename Protocol>
        int name(const Protocol&) const {
            return Name;
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

    GenericSocket& getSocket() {
#ifdef MONGO_CONFIG_SSL
        if (_sslSocket) {
            return static_cast<GenericSocket&>(_sslSocket->lowest_layer());
        }
#endif
        return _socket;
    }

    Future<Message> sourceMessageImpl(const transport::BatonHandle& baton = nullptr) {
        static constexpr auto kHeaderSize = sizeof(MSGHEADER::Value);

        auto headerBuffer = SharedBuffer::allocate(kHeaderSize);
        auto ptr = headerBuffer.get();
        return read(asio::buffer(ptr, kHeaderSize), baton)
            .then([ headerBuffer = std::move(headerBuffer), this, baton ]() mutable {
                if (checkForHTTPRequest(asio::buffer(headerBuffer.get(), kHeaderSize))) {
                    return sendHTTPResponse(baton);
                }

                const auto msgLen = size_t(MSGHEADER::View(headerBuffer.get()).getMessageLength());
                if (msgLen < kHeaderSize || msgLen > MaxMessageSizeBytes) {
                    StringBuilder sb;
                    sb << "recv(): message msgLen " << msgLen << " is invalid. "
                       << "Min " << kHeaderSize << " Max: " << MaxMessageSizeBytes;
                    const auto str = sb.str();
                    LOG(0) << str;

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
                    .then([ this, buffer = std::move(buffer), msgLen ]() mutable {
                        if (_isIngressSession) {
                            networkCounter.hitPhysicalIn(msgLen);
                        }
                        return Message(std::move(buffer));
                    });
            });
    }

    template <typename MutableBufferSequence>
    Future<void> read(const MutableBufferSequence& buffers,
                      const transport::BatonHandle& baton = nullptr) {
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
    Future<void> write(const ConstBufferSequence& buffers,
                       const transport::BatonHandle& baton = nullptr) {
#ifdef MONGO_CONFIG_SSL
        _ranHandshake = true;
        if (_sslSocket) {
#ifdef __linux__
            // We do some trickery in asio (see moreToSend), which appears to work well on linux,
            // but fails on other platforms.
            return opportunisticWrite(*_sslSocket, buffers, baton);
#else
            if (_blockingMode == Async) {
                // Opportunistic writes are broken for async egress SSL (switching between blocking
                // and non-blocking mode corrupts the TLS exchange).
                return asio::async_write(*_sslSocket, buffers, UseFuture{}).ignoreValue();
            } else {
                return opportunisticWrite(*_sslSocket, buffers, baton);
            }
#endif
        }
#endif
        return opportunisticWrite(_socket, buffers, baton);
    }

    template <typename Stream, typename MutableBufferSequence>
    Future<void> opportunisticRead(Stream& stream,
                                   const MutableBufferSequence& buffers,
                                   const transport::BatonHandle& baton = nullptr) {
        std::error_code ec;
        size_t size;

        if (MONGO_FAIL_POINT(transportLayerASIOshortOpportunisticReadWrite) &&
            _blockingMode == Async) {
            asio::mutable_buffer localBuffer = buffers;

            if (buffers.size()) {
                localBuffer = asio::mutable_buffer(buffers.data(), 1);
            }

            size = asio::read(stream, localBuffer, ec);
            if (!ec && buffers.size() > 1) {
                ec = asio::error::would_block;
            }
        } else {
            size = asio::read(stream, buffers, ec);
        }

        if (((ec == asio::error::would_block) || (ec == asio::error::try_again)) &&
            (_blockingMode == Async)) {
            // asio::read is a loop internally, so some of buffers may have been read into already.
            // So we need to adjust the buffers passed into async_read to be offset by size, if
            // size is > 0.
            MutableBufferSequence asyncBuffers(buffers);
            if (size > 0) {
                asyncBuffers += size;
            }

            if (baton) {
                return baton->addSession(*this, Baton::Type::In)
                    .then([&stream, asyncBuffers, baton, this] {
                        return opportunisticRead(stream, asyncBuffers, baton);
                    });
            }

            return asio::async_read(stream, asyncBuffers, UseFuture{}).ignoreValue();
        } else {
            return futurize(ec);
        }
    }

    /**
     * moreToSend checks the ssl socket after an opportunisticWrite.  If there are still bytes to
     * send, we manually send them off the underlying socket.  Then we hook that up with a future
     * that gets us back to sending from the ssl side.
     *
     * There are two variants because we call opportunisticWrite on generic sockets and ssl sockets.
     * The generic socket impl never has more to send (because it doesn't have an inner socket it
     * needs to keep sending).
     */
    template <typename ConstBufferSequence>
    boost::optional<Future<void>> moreToSend(GenericSocket& socket,
                                             const ConstBufferSequence& buffers,
                                             const transport::BatonHandle& baton) {
        return boost::none;
    }

#ifdef MONGO_CONFIG_SSL
    template <typename ConstBufferSequence>
    boost::optional<Future<void>> moreToSend(asio::ssl::stream<GenericSocket>& socket,
                                             const ConstBufferSequence& buffers,
                                             const BatonHandle& baton) {
        if (_sslSocket->getCoreOutputBuffer().size()) {
            return opportunisticWrite(getSocket(), _sslSocket->getCoreOutputBuffer(), baton)
                .then([this, &socket, buffers, baton] {
                    return opportunisticWrite(socket, buffers, baton);
                });
        }

        return boost::none;
    }
#endif

    template <typename Stream, typename ConstBufferSequence>
    Future<void> opportunisticWrite(Stream& stream,
                                    const ConstBufferSequence& buffers,
                                    const transport::BatonHandle& baton = nullptr) {
        std::error_code ec;
        std::size_t size;

        if (MONGO_FAIL_POINT(transportLayerASIOshortOpportunisticReadWrite) &&
            _blockingMode == Async) {
            asio::const_buffer localBuffer = buffers;

            if (buffers.size()) {
                localBuffer = asio::const_buffer(buffers.data(), 1);
            }

            size = asio::write(stream, localBuffer, ec);
            if (!ec && buffers.size() > 1) {
                ec = asio::error::would_block;
            }
        } else {
            size = asio::write(stream, buffers, ec);
        }

        if (((ec == asio::error::would_block) || (ec == asio::error::try_again)) &&
            (_blockingMode == Async)) {

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

            if (baton) {
                return baton->addSession(*this, Baton::Type::Out)
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
    Future<bool> maybeHandshakeSSLForIngress(const MutableBufferSequence& buffer) {
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
            if (!_tl->_ingressSSLContext) {
                return Future<bool>::makeReady(
                    Status(ErrorCodes::SSLHandshakeFailed,
                           "SSL handshake received but server is started without SSL support"));
            }

            auto tlsAlert = checkTLSRequest(buffer);
            if (tlsAlert) {
                return opportunisticWrite(getSocket(),
                                          asio::buffer(tlsAlert->data(), tlsAlert->size()))
                    .then([] {
                        return Future<bool>::makeReady(
                            Status(ErrorCodes::SSLHandshakeFailed,
                                   "SSL handshake failed, as client requested disabled protocol"));
                    });
            }

            _sslSocket.emplace(std::move(_socket), *_tl->_ingressSSLContext, "");
            auto doHandshake = [&] {
                if (_blockingMode == Sync) {
                    std::error_code ec;
                    _sslSocket->handshake(asio::ssl::stream_base::server, buffer, ec);
                    return futurize(ec, asio::buffer_size(buffer));
                } else {
                    return _sslSocket->async_handshake(
                        asio::ssl::stream_base::server, buffer, UseFuture{});
                }
            };
            return doHandshake().then([this](size_t size) {
                auto& sslPeerInfo = SSLPeerInfo::forSession(shared_from_this());

                if (sslPeerInfo.subjectName.empty()) {
                    auto sslManager = getSSLManager();
                    auto swPeerInfo = sslManager->parseAndValidatePeerCertificate(
                        _sslSocket->native_handle(), "");

                    // The value of swPeerInfo is a bit complicated:
                    //
                    // If !swPeerInfo.isOK(), then there was an error doing the SSL
                    // handshake and we should reject the connection.
                    //
                    // If !sslPeerInfo.getValue(), then the SSL handshake was successful,
                    // but the peer didn't provide a SSL certificate, and we do not require
                    // one. sslPeerInfo should be empty.
                    //
                    // Otherwise the SSL handshake was successful and the peer did provide
                    // a certificate that is valid, and we should store that info on the
                    // session's SSLPeerInfo decoration.
                    if (auto optPeerInfo = uassertStatusOK(swPeerInfo)) {
                        sslPeerInfo = *optPeerInfo;
                    }
                }
                return true;
            });
        } else if (_tl->_sslMode() == SSLParams::SSLMode_requireSSL) {
            uasserted(ErrorCodes::SSLHandshakeFailed,
                      "The server is configured to only allow SSL connections");
        } else {
            if (_tl->_sslMode() == SSLParams::SSLMode_preferSSL) {
                LOG(0) << "SSL mode is set to 'preferred' and connection " << id() << " to "
                       << remote() << " is not using SSL.";
            }
            return Future<bool>::makeReady(false);
        }
    }
#endif

    template <typename Buffer>
    bool checkForHTTPRequest(const Buffer& buffers) {
        invariant(asio::buffer_size(buffers) >= 4);
        const StringData bufferAsStr(asio::buffer_cast<const char*>(buffers), 4);
        return (bufferAsStr == "GET "_sd);
    }

    // Called from read() to send an HTTP response back to a client that's trying to use HTTP
    // over a native MongoDB port. This returns a Future<Message> to match its only caller, but it
    // always contains an error, so it could really return Future<Anything>
    Future<Message> sendHTTPResponse(const BatonHandle& baton = nullptr) {
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
            .onError(
                [](const Status& status) {
                    return Status(
                        ErrorCodes::ProtocolError,
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

    enum BlockingMode {
        Unknown,
        Sync,
        Async,
    };

    BlockingMode _blockingMode = Unknown;

    HostAndPort _remote;
    HostAndPort _local;

    boost::optional<Milliseconds> _configuredTimeout;
    boost::optional<Milliseconds> _socketTimeout;

    GenericSocket _socket;
#ifdef MONGO_CONFIG_SSL
    boost::optional<asio::ssl::stream<decltype(_socket)>> _sslSocket;
    bool _ranHandshake = false;
#endif

    TransportLayerASIO* const _tl;
    bool _isIngressSession;
};

}  // namespace transport
}  // namespace mongo
