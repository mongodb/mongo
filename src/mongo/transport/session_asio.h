/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <utility>

#include "mongo/base/system_error.h"
#include "mongo/config.h"
#include "mongo/db/stats/counters.h"
#include "mongo/transport/asio_utils.h"
#include "mongo/transport/baton.h"
#include "mongo/transport/ssl_connection_context.h"
#include "mongo/transport/transport_layer_asio.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/socket_utils.h"
#ifdef MONGO_CONFIG_SSL
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_peer_info.h"
#include "mongo/util/net/ssl_types.h"
#endif

#include "asio.hpp"
#ifdef MONGO_CONFIG_SSL
#include "mongo/util/net/ssl.hpp"
#endif

namespace mongo::transport {

extern FailPoint transportLayerASIOshortOpportunisticReadWrite;
extern FailPoint transportLayerASIOSessionPauseBeforeSetSocketOption;

template <typename SuccessValue>
auto futurize(const std::error_code& ec, SuccessValue&& successValue) {
    using Result = Future<std::decay_t<SuccessValue>>;
    if (MONGO_unlikely(ec)) {
        return Result::makeReady(errorCodeToStatus(ec));
    }
    return Result::makeReady(successValue);
}

inline Future<void> futurize(const std::error_code& ec) {
    using Result = Future<void>;
    if (MONGO_unlikely(ec)) {
        return Result::makeReady(errorCodeToStatus(ec));
    }
    return Result::makeReady();
}

class TransportLayerASIO::ASIOSession final : public Session {
public:
    using GenericSocket = asio::generic::stream_protocol::socket;

    using Endpoint = asio::generic::stream_protocol::endpoint;

    // If the socket is disconnected while any of these options are being set, this constructor
    // may throw, but it is guaranteed to throw a mongo DBException.
    ASIOSession(TransportLayerASIO* tl,
                GenericSocket socket,
                bool isIngressSession,
                Endpoint endpoint = Endpoint(),
                std::shared_ptr<const SSLConnectionContext> transientSSLContext = nullptr);

    ASIOSession(const ASIOSession&) = delete;
    ASIOSession& operator=(const ASIOSession&) = delete;

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

    const SockAddr& remoteAddr() const override {
        return _remoteAddr;
    }

    const SockAddr& localAddr() const override {
        return _localAddr;
    }

    void end() override;

    StatusWith<Message> sourceMessage() noexcept override;

    Future<Message> asyncSourceMessage(const BatonHandle& baton = nullptr) noexcept override;

    Status waitForData() noexcept override;

    Future<void> asyncWaitForData() noexcept override;

    Status sinkMessage(Message message) noexcept override;

    Future<void> asyncSinkMessage(Message message,
                                  const BatonHandle& baton = nullptr) noexcept override;

    void cancelAsyncOperations(const BatonHandle& baton = nullptr) override;

    void setTimeout(boost::optional<Milliseconds> timeout) override;

    bool isConnected() override;

    bool isFromLoadBalancer() const override {
        return _isFromLoadBalancer;
    }

#ifdef MONGO_CONFIG_SSL
    const SSLConfiguration* getSSLConfiguration() const override;

    const std::shared_ptr<SSLManagerInterface> getSSLManager() const override;
#endif

protected:
    friend class TransportLayerASIO;
    friend TransportLayerASIO::BatonASIO;

#ifdef MONGO_CONFIG_SSL
    // The unique_lock here is held by TransportLayerASIO to synchronize with the asyncConnect
    // timeout callback. It will be unlocked before the SSL actually handshake begins.
    Future<void> handshakeSSLForEgressWithLock(stdx::unique_lock<Latch> lk,
                                               const HostAndPort& target,
                                               const ReactorHandle& reactor);

    // For synchronous connections where we don't have an async timer, just take a dummy lock and
    // pass it to the WithLock version of handshakeSSLForEgress
    Future<void> handshakeSSLForEgress(const HostAndPort& target);
#endif

    void ensureSync();

    void ensureAsync();

private:
    GenericSocket& getSocket();

    ExecutorFuture<void> parseProxyProtocolHeader(const ReactorHandle& reactor);
    Future<Message> sourceMessageImpl(const BatonHandle& baton = nullptr);

    template <typename MutableBufferSequence>
    Future<void> read(const MutableBufferSequence& buffers, const BatonHandle& baton = nullptr);

    template <typename ConstBufferSequence>
    Future<void> write(const ConstBufferSequence& buffers, const BatonHandle& baton = nullptr);

    template <typename Stream, typename MutableBufferSequence>
    Future<void> opportunisticRead(Stream& stream,
                                   const MutableBufferSequence& buffers,
                                   const BatonHandle& baton = nullptr);

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
                                             const BatonHandle& baton) {
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

    boost::optional<std::string> getSniName() const override;
#endif

    template <typename Stream, typename ConstBufferSequence>
    Future<void> opportunisticWrite(Stream& stream,
                                    const ConstBufferSequence& buffers,
                                    const BatonHandle& baton = nullptr);

#ifdef MONGO_CONFIG_SSL
    template <typename MutableBufferSequence>
    Future<bool> maybeHandshakeSSLForIngress(const MutableBufferSequence& buffer);
#endif

    template <typename Buffer>
    bool checkForHTTPRequest(const Buffer& buffers);

    // Called from read() to send an HTTP response back to a client that's trying to use HTTP
    // over a native MongoDB port. This returns a Future<Message> to match its only caller, but it
    // always contains an error, so it could really return Future<Anything>
    Future<Message> sendHTTPResponse(const BatonHandle& baton = nullptr);

    enum BlockingMode {
        Unknown,
        Sync,
        Async,
    };

    BlockingMode _blockingMode = Unknown;

    HostAndPort _remote;
    HostAndPort _local;

    SockAddr _remoteAddr;
    SockAddr _localAddr;

    boost::optional<Milliseconds> _configuredTimeout;
    boost::optional<Milliseconds> _socketTimeout;

    GenericSocket _socket;
#ifdef MONGO_CONFIG_SSL
    boost::optional<asio::ssl::stream<decltype(_socket)>> _sslSocket;
    bool _ranHandshake = false;
    std::shared_ptr<const SSLConnectionContext> _sslContext;
#endif

    TransportLayerASIO* const _tl;
    bool _isIngressSession;
    bool _isFromLoadBalancer = false;
    boost::optional<SockAddr> _proxiedSrcEndpoint;
    boost::optional<SockAddr> _proxiedDstEndpoint;
};

}  // namespace mongo::transport
