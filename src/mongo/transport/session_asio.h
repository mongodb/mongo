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
#include "mongo/transport/transport_layer_asio.h"
#include "mongo/util/net/sock.h"
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

using GenericSocket = asio::generic::stream_protocol::socket;

class TransportLayerASIO::ASIOSession final : public Session {
    MONGO_DISALLOW_COPYING(ASIOSession);

public:
    ASIOSession(TransportLayerASIO* tl, GenericSocket socket)
        : _socket(std::move(socket)), _tl(tl) {
        std::error_code ec;

        auto family = endpointToSockAddr(_socket.local_endpoint()).getType();
        if (family == AF_INET || family == AF_INET6) {
            _socket.set_option(asio::ip::tcp::no_delay(true));
            _socket.set_option(asio::socket_base::keep_alive(true));
            setSocketKeepAliveParams(_socket.native_handle());
        }

        _local = endpointToHostAndPort(_socket.local_endpoint());
        _remote = endpointToHostAndPort(_socket.remote_endpoint(ec));
        if (ec) {
            LOG(3) << "Unable to get remote endpoint address: " << ec.message();
        }
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
            getSocket().cancel();
            getSocket().shutdown(GenericSocket::shutdown_both, ec);
            if (ec) {
                error() << "Error shutting down socket: " << ec.message();
            }
        }
    }

    StatusWith<Message> sourceMessage() override {
        ensureSync();
        auto out = StatusWith<Message>(ErrorCodes::InternalError, "uninitialized...");
        bool called = false;
        sourceMessageImpl(true, [&](StatusWith<Message> in) {
            out = std::move(in);
            called = true;
        });
        invariant(called);
        return out;
    }

    void asyncSourceMessage(std::function<void(StatusWith<Message>)> cb) override {
        ensureAsync();
        sourceMessageImpl(false, std::move(cb));
    }

    Status sinkMessage(Message message) override {
        ensureSync();

        std::error_code ec;
        size_t size;
        bool called = false;

        write(true,
              asio::buffer(message.buf(), message.size()),
              [&](const std::error_code& ec_, size_t size_) {
                  ec = ec_;
                  size = size_;
                  called = true;
              });
        invariant(called);

        if (ec)
            return errorCodeToStatus(ec);

        invariant(size == size_t(message.size()));
        networkCounter.hitPhysicalOut(message.size());
        return Status::OK();
    }

    void asyncSinkMessage(Message message, std::function<void(Status)> cb) override {
        ensureAsync();

        write(false, asio::buffer(message.buf(), message.size()), [
            message,  // keep the buffer alive.
            cb = std::move(cb),
            this
        ](const std::error_code& ec, size_t size) {
            if (ec) {
                cb(errorCodeToStatus(ec));
                return;
            }
            invariant(size == size_t(message.size()));
            networkCounter.hitPhysicalOut(message.size());
            cb(Status::OK());
        });
    };


private:
    GenericSocket& getSocket() {
#ifdef MONGO_CONFIG_SSL
        if (_sslSocket) {
            return static_cast<GenericSocket&>(_sslSocket->lowest_layer());
        }
#endif
        return _socket;
    }

    bool isOpen() const {
#ifdef MONGO_CONFIG_SSL
        return _sslSocket ? _sslSocket->lowest_layer().is_open() : _socket.is_open();
#else
        return _socket.is_open();
#endif
    }

    template <typename Callback>
    void sourceMessageImpl(bool sync, Callback&& cb) {
        static constexpr auto kHeaderSize = sizeof(MSGHEADER::Value);

        auto headerBuffer = SharedBuffer::allocate(kHeaderSize);
        auto ptr = headerBuffer.get();
        read(
            sync,
            asio::buffer(ptr, kHeaderSize),
            [ sync, cb = std::forward<Callback>(cb), headerBuffer = std::move(headerBuffer), this ](
                const std::error_code& ec, size_t size) mutable {

                if (ec)
                    return cb(errorCodeToStatus(ec));
                invariant(size == kHeaderSize);

                const auto msgLen = size_t(MSGHEADER::View(headerBuffer.get()).getMessageLength());
                if (msgLen < kHeaderSize || msgLen > MaxMessageSizeBytes) {
                    StringBuilder sb;
                    sb << "recv(): message msgLen " << msgLen << " is invalid. "
                       << "Min " << kHeaderSize << " Max: " << MaxMessageSizeBytes;
                    const auto str = sb.str();
                    LOG(0) << str;

                    return cb(Status(ErrorCodes::ProtocolError, str));
                }

                if (msgLen == size) {
                    // This probably isn't a real case since all (current) messages have bodies.
                    networkCounter.hitPhysicalIn(msgLen);
                    return cb(Message(std::move(headerBuffer)));
                }

                auto buffer = SharedBuffer::allocate(msgLen);
                memcpy(buffer.get(), headerBuffer.get(), kHeaderSize);

                MsgData::View msgView(buffer.get());
                read(sync,
                     asio::buffer(msgView.data(), msgView.dataLen()),
                     [ cb = std::move(cb), buffer = std::move(buffer), msgLen, this ](
                         const std::error_code& ec, size_t size) mutable {
                         if (ec)
                             return cb(errorCodeToStatus(ec));
                         networkCounter.hitPhysicalIn(msgLen);
                         return cb(Message(std::move(buffer)));
                     });
            });
    }


    template <typename MutableBufferSequence, typename CompleteHandler>
    void read(bool sync, const MutableBufferSequence& buffers, CompleteHandler&& handler) {
#ifdef MONGO_CONFIG_SSL
        if (_sslSocket) {
            return opportunisticRead(
                sync, *_sslSocket, buffers, std::forward<CompleteHandler>(handler));
        } else if (!_ranHandshake) {
            invariant(asio::buffer_size(buffers) >= sizeof(MSGHEADER::Value));
            auto postHandshakeCb = [this, sync, buffers, handler](Status status,
                                                                  bool needsRead) mutable {
                if (status.isOK()) {
                    if (needsRead) {
                        read(sync, buffers, handler);
                    } else {
                        std::error_code ec;
                        handler(ec, asio::buffer_size(buffers));
                    }
                } else {
                    handler(std::error_code(status.code(), mongoErrorCategory()), 0);
                }
            };

            auto handshakeRecvCb =
                [ this, postHandshakeCb = std::move(postHandshakeCb), sync, buffers ](
                    const std::error_code& ec, size_t size) mutable {
                _ranHandshake = true;
                if (ec) {
                    postHandshakeCb(errorCodeToStatus(ec), size);
                    return;
                }

                maybeHandshakeSSL(sync, buffers, std::move(postHandshakeCb));
            };

            return opportunisticRead(sync, _socket, buffers, std::move(handshakeRecvCb));
        }
#endif
        return opportunisticRead(sync, _socket, buffers, std::forward<CompleteHandler>(handler));
    }

    template <typename ConstBufferSequence, typename CompleteHandler>
    void write(bool sync, const ConstBufferSequence& buffers, CompleteHandler&& handler) {
#ifdef MONGO_CONFIG_SSL
        if (_sslSocket) {
            return opportunisticWrite(
                sync, *_sslSocket, buffers, std::forward<CompleteHandler>(handler));
        }
#endif
        return opportunisticWrite(sync, _socket, buffers, std::forward<CompleteHandler>(handler));
    }

    void ensureSync() {
        if (_blockingMode == Sync)
            return;
        asio::error_code ec;
        getSocket().non_blocking(false, ec);
        fassertStatusOK(40490, errorCodeToStatus(ec));
        _blockingMode = Sync;
    }

    void ensureAsync() {
        if (_blockingMode == Async)
            return;
        asio::error_code ec;
        getSocket().non_blocking(true, ec);
        fassertStatusOK(50706, errorCodeToStatus(ec));
        _blockingMode = Async;
    }

    template <typename Stream, typename MutableBufferSequence, typename CompleteHandler>
    void opportunisticRead(bool sync,
                           Stream& stream,
                           const MutableBufferSequence& buffers,
                           CompleteHandler&& handler) {
        std::error_code ec;
        auto size = asio::read(stream, buffers, ec);
        if ((ec == asio::error::would_block || ec == asio::error::try_again) && !sync) {
            // asio::read is a loop internally, so some of buffers may have been read into already.
            // So we need to adjust the buffers passed into async_read to be offset by size, if
            // size is > 0.
            MutableBufferSequence asyncBuffers(buffers);
            if (size > 0) {
                asyncBuffers += size;
            }
            asio::async_read(stream,
                             asyncBuffers,
                             [ size, handler = std::forward<CompleteHandler>(handler) ](
                                 const std::error_code& ec, size_t asyncSize) mutable {
                                 // Add back in the size read opportunistically.
                                 handler(ec, size + asyncSize);
                             });
        } else {
            handler(ec, size);
        }
    }

    template <typename Stream, typename ConstBufferSequence, typename CompleteHandler>
    void opportunisticWrite(bool sync,
                            Stream& stream,
                            const ConstBufferSequence& buffers,
                            CompleteHandler&& handler) {
        std::error_code ec;
        auto size = asio::write(stream, buffers, ec);
        if ((ec == asio::error::would_block || ec == asio::error::try_again) && !sync) {
            // asio::write is a loop internally, so some of buffers may have been read into already.
            // So we need to adjust the buffers passed into async_write to be offset by size, if
            // size is > 0.
            ConstBufferSequence asyncBuffers(buffers);
            if (size > 0) {
                asyncBuffers += size;
            }
            asio::async_write(stream,
                              asyncBuffers,
                              [ size, handler = std::forward<CompleteHandler>(handler) ](
                                  const std::error_code& ec, size_t asyncSize) mutable {
                                  // Add back in the size written opportunistically.
                                  handler(ec, size + asyncSize);
                              });
        } else {
            handler(ec, size);
        }
    }

#ifdef MONGO_CONFIG_SSL
    template <typename MutableBufferSequence, typename HandshakeCb>
    void maybeHandshakeSSL(bool sync, const MutableBufferSequence& buffer, HandshakeCb onComplete) {
        invariant(asio::buffer_size(buffer) >= sizeof(MSGHEADER::Value));
        MSGHEADER::ConstView headerView(asio::buffer_cast<char*>(buffer));
        auto responseTo = headerView.getResponseToMsgId();

        // This logic was taken from the old mongo/util/net/sock.cpp.
        //
        // It lets us run both TLS and unencrypted mongo over the same port.
        //
        // The first message received from the client should have the responseTo field of the wire
        // protocol message needs to be 0 or -1. Otherwise the connection is either sending
        // garbage or a TLS Hello packet which will be caught by the TLS handshake.
        if (responseTo != 0 && responseTo != -1) {
            if (!_tl->_sslContext) {
                return onComplete(
                    {ErrorCodes::SSLHandshakeFailed,
                     "SSL handshake received but server is started without SSL support"},
                    false);
            }

            _sslSocket.emplace(std::move(_socket), *_tl->_sslContext);

            auto handshakeCompleteCb = [ this, onComplete = std::move(onComplete) ](
                const std::error_code& ec, size_t size) mutable {
                auto& sslPeerInfo = SSLPeerInfo::forSession(shared_from_this());

                if (!ec && sslPeerInfo.subjectName.empty()) {
                    auto sslManager = getSSLManager();
                    auto swPeerInfo = sslManager->parseAndValidatePeerCertificate(
                        _sslSocket->native_handle(), "");

                    if (swPeerInfo.isOK()) {
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
                        if (swPeerInfo.getValue()) {
                            sslPeerInfo = *swPeerInfo.getValue();
                        }
                    } else {
                        return onComplete(swPeerInfo.getStatus(), false);
                    }
                }

                onComplete(ec ? errorCodeToStatus(ec) : Status::OK(), true);
            };

            if (sync) {
                std::error_code ec;
                _sslSocket->handshake(asio::ssl::stream_base::server, buffer, ec);
                handshakeCompleteCb(ec, asio::buffer_size(buffer));
            } else {
                return _sslSocket->async_handshake(
                    asio::ssl::stream_base::server, buffer, handshakeCompleteCb);
            }
        } else if (_tl->_sslMode() == SSLParams::SSLMode_requireSSL) {
            onComplete({ErrorCodes::SSLHandshakeFailed,
                        "The server is configured to only allow SSL connections"},
                       false);
        } else {
            if (_tl->_sslMode() == SSLParams::SSLMode_preferSSL) {
                LOG(0) << "SSL mode is set to 'preferred' and connection " << id() << " to "
                       << remote() << " is not using SSL.";
            }
            onComplete(Status::OK(), false);
        }
    }
#endif

    enum BlockingMode {
        Unknown,
        Sync,
        Async,
    };

    BlockingMode _blockingMode = Unknown;

    HostAndPort _remote;
    HostAndPort _local;

    GenericSocket _socket;
#ifdef MONGO_CONFIG_SSL
    boost::optional<asio::ssl::stream<decltype(_socket)>> _sslSocket;
    bool _ranHandshake = false;
#endif

    TransportLayerASIO* const _tl;
};

}  // namespace transport
}  // namespace mongo
