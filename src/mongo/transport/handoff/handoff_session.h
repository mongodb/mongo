/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/auth/restriction_environment.h"
#include "mongo/rpc/message.h"
#include "mongo/transport/handoff/handoff_posix_interface.h"
#include "mongo/transport/proxy_protocol_header_parser.h"
#include "mongo/transport/session.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/sockaddr.h"

#include <cstdint>

#include <s2n.h>

namespace mongo::transport {

/**
 * HandoffSession implements the transport::Session interface using synchronous I/O.
 *
 * It serves the pre-auth process session handoff as follows.
 *  1. The session starts in Cleartext mode on a unix domain socket (UDS) file descriptor (fd)
 *     connected to the pre-auth process.
 *  2. The session accepts standard pre-auth messages via the UDS fd.
 *  3. When an OP_HANDOFF message arrives, the session extracts the downstream client TLS fd (passed
 *     via SCM_RIGHTS), parses the serialized s2n TLS state, deserializes it into an s2n_connection,
 *     closes the UDS fd, and transitions to TLS mode. On failure, both the downstream client fd and
 *     the UDS fd are shut down, the downstream client fd is closed, and the session is left
 *     disconnected. On success, only the UDS fd is closed. Closing the UDS fd causes EOF on the
 *     pre-auth process's end, which is the observable signal that the handoff completed.
 *  4. All subsequent reads/writes go through s2n_recv/s2n_send on the downstream client fd.
 *
 * Invoking end() always shuts down the current fd, whether that is the pre-auth process UDS fd
 * (before handoff) or the downstream client fd (after handoff).
 *
 * Thread safety:
 * - sourceMessage(), sinkMessage(), waitForData(), getAuthEnvironment(), and
 *   setIsLoadBalancerPeer() must be called sequentially from the session workflow thread.
 * - end(), isConnected(), local(), remote(), getSourceRemoteEndpoint(), and appendToBSON()
 *   (all other public methods) may be called concurrently from any thread, including concurrently
 *   with the session workflow thread operations above.
 *
 * Async operations are not supported and will terminate the process.
 */
class HandoffSession final : public Session {
public:
    enum class State : std::int8_t {
        /**
         * Cleartext is the initial state. When in this state, the session first reads a PROXY
         * header from the accepted unix domain socket client, and then processes mongoRPC messages
         * in the clear. The Cleartext stage continues until an OP_HANDOFF message is received.
         */
        Cleartext,
        /**
         * TLS is the state that follows the receipt of an OP_HANDOFF message.
         * Upon receiving an OP_HANDOFF message, the session transitions to TLS mode where all I/O
         * is encrypted via s2n on the downstream client's file descriptor, which was sent as
         * ancillary data over the unix domain socket.
         */
        TLS,
    };

    struct Params {
        /** The file descriptor for the unix domain socket connection to the pre-auth process. */
        int fd;
        /**
         * The path of the unix domain socket that accepted the connection. It's the local socket
         * address of `fd`.
         */
        std::filesystem::path localAddress;
        /**
         * The transport layer that accepted the connection.
         * `transportLayer` must be non-null, except for in unit tests.
         */
        TransportLayer* transportLayer;
        /** Whether this connection was accepted on a load balanced socket. */
        bool acceptedOnLoadBalancedSocket;
        /** Whether this connection was accepted on a priority socket. */
        bool acceptedOnPrioritySocket;
        /**
         * The TLS configuration to use for the s2n_connection into which the handed-off TLS state
         * will be deserialized.
         * `s2nConfig` must be non-null, except for in unit tests.
         */
        const s2n_config* s2nConfig;
        /**
         * Dependency injection for libc functions used by `HandoffSession`.
         * Unit tests supply a value for `posix` to simulate failures.
         */
        POSIXInterface& posix;
    };

    HandoffSession(Params params);

    ~HandoffSession() override;

    TransportLayer* getTransportLayer() const override {
        return _tl;
    }

    /**
     * Returns the peer address of the current socket. Before handoff, this is the pre-auth
     * process's unix domain "address" ("anonymous unix socket"). After handoff, it is the client's
     * TLS socket address.
     */
    const HostAndPort& remote() const override;

    /**
     * Returns the local address of the current socket. Before handoff, this is the address of the
     * unix domain socket to which the secure pre-auth process connects. After handoff, it is the
     * address on which the secure pre-auth process was listening.
     */
    const HostAndPort& local() const override;

    /**
     * Returns the address of the peer that is being proxied via a chain of PROXY protocol speaking
     * peers, i.e. the "true" origin of the connection. After prelude(), this is the proxied client
     * IP extracted from the PROXY v2 header. Before prelude(), or if the PROXY header carried no
     * address block, falls back to remote().
     */
    const HostAndPort& getSourceRemoteEndpoint() const override;

    StatusWith<Message> sourceMessage() override;
    Status sinkMessage(Message message) override;

    Future<Message> asyncSourceMessage(const BatonHandle& handle = nullptr) override;
    Future<void> asyncSinkMessage(Message message, const BatonHandle& handle = nullptr) override;

    Status waitForData() override;
    Future<void> asyncWaitForData() override;

    void cancelAsyncOperations(const BatonHandle& handle = nullptr) override;

    void end() override;
    bool isConnected() override;
    void setTimeout(boost::optional<Milliseconds>) override;

    /**
     * Parses the PROXY v2 header from the UDS connection, extracts the proxied source address if
     * present, and applies any TLVs to the session.
     */
    void prelude() override;

    void setIsLoadBalancerPeer(bool) override;

    Status validateProxyUnixSocketPeerPermissions() override;

    bool bindsToOperationState() const override;

    bool isExemptedByCIDRList(const CIDRList& exemptions) const override;

    void appendToBSON(BSONObjBuilder& bb) const override;

#ifdef MONGO_CONFIG_SSL
    const SSLConfiguration* getSSLConfiguration() const override {
        return nullptr;
    }
#endif

    const RestrictionEnvironment& getAuthEnvironment() const override {
        return _restrictionEnvironment;
    }

    State getState_forTest() const {
        std::lock_guard lock{_mutex};
        return _state;
    }

private:
    struct ConnectionEndpoints {
        /** the server side (our side) of the connection */
        HostAndPort local;
        /** the client side (their side) of the connection */
        HostAndPort remote;
    };

    struct ProxiedSource {
        /**
         * The downstream client's source address from the PROXY v2 header. Set in prelude(). Stored
         * as SockAddr for RestrictionEnvironment construction in _updateEndpointsForClientFd().
         */
        SockAddr address;
        /**
         * Same as `address`, but stored as a HostAndPort so that getSourceRemoteEndpoint() can
         * return a const reference (required by the base class).
         */
        HostAndPort endpoint;
    };

    /**
     * Reads exactly `len` bytes into `buf` from the current fd using blocking I/O.
     * Returns NetworkTimeout if SO_RCVTIMEO fires before all bytes are read.
     */
    StatusWith<size_t> _syncRead(char* buf, size_t len);

    /**
     * Writes exactly `len` bytes from `buf` to the current fd using blocking I/O.
     * Returns NetworkTimeout if SO_SNDTIMEO fires before all bytes are written.
     */
    Status _syncWrite(const char* buf, size_t len);

    /**
     * Receives a message from the UDS using recvmsg(), which allows receiving ancillary data
     * (SCM_RIGHTS file descriptors) alongside the payload. Returns the number of bytes read and
     * sets receivedFd if one was received via SCM_RIGHTS.
     */
    StatusWith<size_t> _recvWithFd(char* buf, size_t len, int* receivedFd);

    /**
     * Reads and parses the PROXY v2 header sent by the pre-auth process before OP_HANDOFF.
     * Returns the parsed header. Uasserts on any I/O or parse error.
     */
    transport::ParserResults _parseProxyProtocolHeader();

    /**
     * Handles an OP_HANDOFF message. Parses the serialized s2n state from the message, deserializes
     * it into an s2n_connection, updates the remote and local addresses and restriction
     * environment, transitions to TLS mode, then closes the UDS fd. On failure, shuts down both the
     * received downstream client fd and the UDS fd, and closes the received downstream client fd.
     * The session is left disconnected.
     */
    Status _handleSessionHandoff(const Message& msg, int clientFd);

    /**
     * Updates the remote and local addresses from the given client fd, and sets the restriction
     * environment using the proxied source address if available and the restriction mode is
     * "origin", otherwise using the client fd's peer address. Returns an error if the client
     * address cannot be determined.
     */
    Status _updateEndpointsForClientFd(int clientFd);

    const HostAndPort& _remote(WithLock) const;
    const HostAndPort& _local(WithLock) const;

    /**
     * Restricts access to _fd, _state, _isShutDown, _endpointsBeforeHandoff,
     * _endpointsAfterHandoff, and _proxiedSource. Specifically:
     * - _fd, _state, _endpointsBeforeHandoff, _endpointsAfterHandoff, and _proxiedSource can be
     *   modified by the session workflow thread, and so must be locked when accessed from other
     *   threads.
     * - _isShutDown can be modified by any thread that calls end(), and so must be locked when
     *   accessed from anywhere.
     * - any operations performed using _fd on a thread other than the session workflow thread must
     *   lock _mutex for the duration of the operation.
     * - the session workflow thread must lock _mutex when calling close() on _fd.
     */
    mutable std::mutex _mutex;
    /**
     * The socket file descriptor with which we're communicating. This changes during
     * OP_HANDOFF.
     */
    int _fd;
    /** Indicates whether handoff has occurred. Cleartext before, TLS after. */
    State _state;
    /** Indicates whether end() has been called on this session. */
    bool _isShutDown;
    /**
     * The remote and local addresses associated with the connection _fd. This changes during
     * OP_HANDOFF.
     */
    ConnectionEndpoints _endpointsBeforeHandoff;
    ConnectionEndpoints _endpointsAfterHandoff;
    /**
     * The remote address of the proxied client. This is parsed from the PROXY header sent at the
     * beginning of the accepted unix domain socket connection, but might be absent.
     */
    boost::optional<ProxiedSource> _proxiedSource;

    RestrictionEnvironment _restrictionEnvironment;
    s2n_connection* _s2nConnection;
    const s2n_config* const _s2nConfig;
    POSIXInterface& _posix;
    TransportLayer* const _tl;
};

}  // namespace mongo::transport
