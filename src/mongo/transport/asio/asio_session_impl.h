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

#pragma once

#include "mongo/config.h"
#include "mongo/transport/asio/asio_session.h"
#include "mongo/transport/asio/asio_transport_layer.h"
#include "mongo/transport/baton.h"

#include <utility>

#include <asio.hpp>

#ifdef MONGO_CONFIG_SSL
#include "mongo/util/net/ssl.hpp"
#endif

namespace mongo::transport {

/**
 * Provides common functionality between AsyncAsioSession and SyncAsioSession.
 *
 * NOTE: This functionality is currently provided by inheritance, but composition might be a
 * preferred approach after more refactoring.
 */
class CommonAsioSession : public AsioSession {
public:
    /**
     * If the socket is disconnected while any of these options are being set, this constructor
     * may throw, but it is guaranteed to throw a mongo DBException.
     */
    CommonAsioSession(AsioTransportLayer* tl,
                      GenericSocket socket,
                      bool isIngressSession,
                      Endpoint endpoint = Endpoint(),
                      std::shared_ptr<const SSLConnectionContext> transientSSLContext = nullptr);

    CommonAsioSession(const CommonAsioSession&) = delete;
    CommonAsioSession& operator=(const CommonAsioSession&) = delete;

    TransportLayer* getTransportLayer() const override {
        return _tl;
    }

    const HostAndPort& remote() const override {
        return _remote;
    }

    const HostAndPort& local() const override {
        return _local;
    }

    const HostAndPort& getSourceRemoteEndpoint() const override {
        if (_proxiedSrcEndpoint) {
            return _proxiedSrcEndpoint.value();
        }

        return remote();
    }

    boost::optional<const HostAndPort&> getProxiedDstEndpoint() const override {
        if (_proxiedDstEndpoint) {
            return _proxiedDstEndpoint.value();
        }

        return boost::none;
    }

    const SockAddr& remoteAddr() const {
        return _remoteAddr;
    }

    const SockAddr& getProxiedSrcRemoteAddr() const {
        if (_proxiedSrcRemoteAddr) {
            return *_proxiedSrcRemoteAddr;
        }

        return _remoteAddr;
    }

    const SockAddr& localAddr() const {
        return _localAddr;
    }

    void appendToBSON(BSONObjBuilder& bb) const override {
        bb.append("remote", _remote.toString());
        bb.append("local", _local.toString());
    }

    void end() override;

    StatusWith<Message> sourceMessage() override;

    Future<Message> asyncSourceMessage(const BatonHandle& baton = nullptr) override;

    Status waitForData() override;

    Future<void> asyncWaitForData() override;

    Status sinkMessage(Message message) override;

    Future<void> asyncSinkMessage(Message message, const BatonHandle& baton = nullptr) override;

    void cancelAsyncOperations(const BatonHandle& baton = nullptr) override;

    void setTimeout(boost::optional<Milliseconds> timeout) override;

    bool isConnected() override;

    bool isConnectedToLoadBalancerPort() const override;

    bool isLoadBalancerPeer() const override;

    void setisLoadBalancerPeer(bool helloHasLoadBalancedOption) override;

    bool bindsToOperationState() const override {
        return isLoadBalancerPeer();
    }

#ifdef MONGO_CONFIG_SSL
    const SSLConfiguration* getSSLConfiguration() const override;
#endif

protected:
#ifdef MONGO_CONFIG_SSL
    const std::shared_ptr<SSLManagerInterface>& getSSLManager() const;
    /** Constructs a SSL socket required to initiate SSL handshake for egress connections. */
    Status buildSSLSocket(const HostAndPort& target) override;
    Future<void> handshakeSSLForEgress(const HostAndPort& target,
                                       const ReactorHandle& reactor) override;
#endif

    GenericSocket& getSocket() override;
    ExecutorFuture<void> parseProxyProtocolHeader(const ReactorHandle& reactor) override;

    const RestrictionEnvironment& getAuthEnvironment() const override {
        return _restrictionEnvironment;
    }

    /**
     * Provides the means to track and cancel async I/O operations scheduled through `Session`.
     * Any I/O operation goes through the following steps:
     * - `start()`: changes the state from `kNotStarted` to `kRunning`.
     * - Before scheduling the async operation, checks for cancellation through `isCanceled()`.
     * - `complete()`: clears the state, and prepares the session for future operations.
     *
     * This class is thread-safe.
     */
    class AsyncOperationState {
    public:
        void start() {
            const auto prev = _state.swap(State::kRunning);
            invariant(prev == State::kNotStarted, "Another operation was in progress");
        }

        bool isCanceled() const {
            return _state.load() == State::kCanceled;
        }

        void complete() {
            const auto prev = _state.swap(State::kNotStarted);
            invariant(prev != State::kNotStarted, "No operation was running");
        }

        /**
         * Instructs an active operation to cancel (if there is any). Otherwise, it does nothing.
         * Cancellation is non-blocking and `cancel()` doesn't block for completion of ongoing
         * operations.
         */
        void cancel() {
            auto expected = State::kRunning;
            _state.compareAndSwap(&expected, State::kCanceled);
        }

    private:
        /**
         * State transition diagram:
         * -+-> [kNotStarted] --> [kRunning] --> [kCanceled]
         *  |                          |              |
         *  +--------------------------+--------------+
         */
        enum class State { kNotStarted, kRunning, kCanceled };
        AtomicWord<State> _state{State::kNotStarted};
    };

    Future<Message> sourceMessageImpl(const BatonHandle& baton = nullptr);
    Future<void> sinkMessageImpl(Message message, const BatonHandle& baton = nullptr);

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

    bool isExemptedByCIDRList(const CIDRList& exemptions) const override;

    enum BlockingMode {
        unknown,
        sync,
        async,
    };

    BlockingMode _blockingMode = unknown;

    HostAndPort _remote;
    HostAndPort _local;

    SockAddr _remoteAddr;
    SockAddr _localAddr;

    RestrictionEnvironment _restrictionEnvironment;

    boost::optional<Milliseconds> _configuredTimeout;
    boost::optional<Milliseconds> _socketTimeout;

    GenericSocket _socket;
#ifdef MONGO_CONFIG_SSL
    boost::optional<asio::ssl::stream<decltype(_socket)>> _sslSocket;
    bool _ranHandshake = false;
    std::shared_ptr<const SSLConnectionContext> _sslContext;
#endif

    /**
     * Synchronizes construction of _sslSocket in maybeHandshakeSSLForIngress and access of
     * _sslSocket during shutdown in end(). Without synchronization, end() can operate on _socket
     * while its ownership is being passed to _sslSocket.
     * TODO (SERVER-83933) Remove this mutex after SSL handshake logic is moved to occur before
     * concurrent accesses can occur.
     */
    stdx::mutex _sslSocketLock{};

    AsioTransportLayer* const _tl;
    bool _isIngressSession;

    /**
     * We have a distinction here. A load balancer port can accept connections that are
     * either attempting to connect to a load balancer or as a normal targeted connection.
     * The bools below describe if 1/ the connection is connecting to the load balancer port,
     * and 2/ the connection is a load balancer type connection. We only find out if the
     * connection is a LoadBalancerConnection if the hello command parses {loadBalancer: 1}.
     */
    bool _isConnectedToLoadBalancerPort = false;
    bool _isLoadBalancerPeer = false;
    boost::optional<HostAndPort> _proxiedSrcEndpoint;
    boost::optional<HostAndPort> _proxiedDstEndpoint;

    boost::optional<SockAddr> _proxiedSrcRemoteAddr;

    AsyncOperationState _asyncOpState;

    /**
     * Strictly orders the start and cancellation of asynchronous operations:
     * - Holding the mutex while starting asynchronous operations (e.g., adding the session to the
     *   networking baton) ensures cancellation either happens before or after scheduling of the
     *   operation.
     * - Holding the mutex while canceling asynchronous operations guarantees no operation can start
     *   while cancellation is in progress.
     *
     * Opportunistic read and write are implemented as recursive future continuations, so we may
     * recursively acquire the mutex when the future is readied inline.
     */
    stdx::recursive_mutex _asyncOpMutex;  // NOLINT
};

/**
 * This is an AsioSession which is intended to only use the `asyncSourceMessage`,
 * `asyncSinkMessage`, and `asyncWaitForData` subset of the Session's read/write/wait interface.
 * Usage of non async counterparts of these functions causes an invariant to be triggered.
 *
 * NOTE: Longer-term the intention of the split between Sync and Async implementations is to firstly
 * have two cleaner implementations instead of one implementation that supports both modes of
 * operation, and secondly to reduce the size of the read/write/wait Session interface into a
 * generic future-returning interface, where the readiness of those futures depends on the
 * implementing subclass. At the moment, the distinction between the classes is limited to which
 * functions are allowed to be called.
 */
class AsyncAsioSession : public CommonAsioSession {
public:
    using CommonAsioSession::CommonAsioSession;

    ~AsyncAsioSession() override {
        end();
    }

protected:
    void ensureSync() override;
    void ensureAsync() override;
};

/**
 * This is an AsioSession which is intended to only use the `sourceMessage`, `sinkMessage`, and
 * `waitForData` subset of the Session's read/write/wait interface. Usage of async counterparts of
 * these functions causes an invariant to be triggered.
 *
 * NOTE: See AsyncAsioSession's note explaining the current state and purpose of the separation.
 */
class SyncAsioSession : public CommonAsioSession {
public:
    using CommonAsioSession::CommonAsioSession;

    ~SyncAsioSession() override {
        end();
    }

protected:
    void ensureSync() override;
    void ensureAsync() override;
};

}  // namespace mongo::transport
