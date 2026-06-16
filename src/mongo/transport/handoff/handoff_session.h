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
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/message.h"
#include "mongo/transport/proxy_protocol_header_parser.h"
#include "mongo/transport/session.h"
#include "mongo/util/net/hostandport.h"

#include <s2n.h>

namespace mongo::transport::handoff_transport {

/**
 * The two operating modes for a HandoffSession. A session begins in Cleartext mode,
 * accepting unencrypted messages from the pre-auth process over a unix domain socket.
 * Upon receiving an OP_HANDOFF message, the session transitions to TLS mode where all
 * I/O is encrypted via s2n on the downstream client's file descriptor, which was sent as
 * ancillary data over the unix domain socket.
 */
enum class HandoffSessionState { Cleartext, TLS };

/**
 * HandoffSession implements the transport::Session interface using synchronous I/O.
 *
 * It serves the pre-auth process session handoff as follows.
 *  1. The session starts in Cleartext mode on a unix domain socket (UDS) file descriptor (fd)
 *     connected to the pre-auth process.
 *  2. The session accepts standard pre-auth messages via the UDS fd.
 *  3. When an OP_HANDOFF message arrives, the session extracts the downstream client TLS fd (passed
 *     via SCM_RIGHTS), parses the serialized s2n TLS state, deserializes it into an s2n_connection,
 *     closes the UDS fd, and transitions to TLS mode. On failure, both the downstream client fd
 *     and the UDS fd are closed, and the session is left disconnected. On success, only the UDS fd
 *     is closed. Closing the UDS fd causes EOF on the pre-auth process's end, which is the
 *     observable signal that the handoff completed.
 *  4. All subsequent reads/writes go through s2n_recv/s2n_send on the downstream client fd.
 *
 * Invoking end() always closes the current fd, whether that is the pre-auth process UDS fd (before
 * handoff) or the downstream client fd (after handoff).
 *
 * Thread safety. I/O methods (sourceMessage, sinkMessage, waitForData) must be called from a single
 * thread. end(), isConnected(), and getState() may be called concurrently from any thread.
 * remote(), local(), and restrictionEnvironment() are not safe to read concurrently until the
 * handoff is complete, which can be observed via getState() == TLS.
 *
 * Async operations are not supported and will terminate the process.
 */
class HandoffSession final : public Session {
public:
    /**
     * Constructs a HandoffSession.
     * @param tl        The owning TransportLayer.
     * @param fd        The file descriptor for the UDS connection to the pre-auth process.
     * @param remote    The remote endpoint (the pre-auth process's UDS address).
     * @param local     The local endpoint (our UDS address).
     * @param s2nConfig The s2n_config to use when deserializing TLS sessions.
     */
    HandoffSession(TransportLayer* tl,
                   int fd,
                   HostAndPort remote,
                   HostAndPort local,
                   struct s2n_config* s2nConfig);

    ~HandoffSession() override;

    TransportLayer* getTransportLayer() const override {
        return _tl;
    }

    const HostAndPort& remote() const override {
        return _remote;
    }

    const HostAndPort& local() const override {
        return _local;
    }

    StatusWith<Message> sourceMessage() override;
    Status sinkMessage(Message message) override;

    Future<Message> asyncSourceMessage(const BatonHandle& handle = nullptr) override;
    Future<void> asyncSinkMessage(Message message, const BatonHandle& handle = nullptr) override;

    Status waitForData() override;
    Future<void> asyncWaitForData() override;

    void cancelAsyncOperations(const BatonHandle& handle = nullptr) override;

    void end() override;
    bool isConnected() override;
    void setTimeout(boost::optional<Milliseconds> timeout) override;

    /**
     * Parses the PROXY v2 header from the UDS connection, and applies any TLVs to the session.
     */
    void prelude() override;

    void setIsLoadBalancerPeer(bool) override;

    Status validateProxyUnixSocketPeerPermissions() override {
        return Status::OK();
    }

    bool bindsToOperationState() const override {
        return true;  // TODO(SERVER-128486): Use policy passed from transport layer
    }

    bool isExemptedByCIDRList(const CIDRList& exemptions) const override {
        return false;  // TODO(SERVER-128486): Document preference for the "priority" interface
    }

    void appendToBSON(BSONObjBuilder& bb) const override;

#ifdef MONGO_CONFIG_SSL
    const SSLConfiguration* getSSLConfiguration() const override {
        return nullptr;
    }
#endif

    const RestrictionEnvironment& getAuthEnvironment() const override {
        return _restrictionEnvironment;
    }

    HandoffSessionState getState() const {
        return _state.load();
    }

private:
    /**
     * Polls fd for readability. Returns OK if data is available, Returns NetworkTimeout on timeout,
     * or SocketException on error or peer disconnect. timeoutMs of -1 blocks indefinitely.
     */
    static Status _pollForRead(int fd, int timeoutMs);

    /**
     * Applies the current _timeout to fd via SO_RCVTIMEO and SO_SNDTIMEO. A nullopt timeout
     * sets both options to zero, which disables the timeout.
     */
    Status _applyTimeout(int fd);

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
     * it into an s2n_connection, updates the session endpoints, transitions to TLS mode, then
     * closes the UDS fd. On failure, closes both the received downstream client fd and the UDS
     * fd. The session is left disconnected.
     */
    Status _handleSessionHandoff(const Message& msg, int clientFd);

    /**
     * Updates _remote, _local, and _restrictionEnvironment from the given downstream client fd.
     * Returns an error if the client address cannot be determined.
     */
    Status _updateEndpointsForClientFd(int clientFd);

    TransportLayer* const _tl;
    AtomicWord<int> _fd;
    AtomicWord<HandoffSessionState> _state;
    HostAndPort _remote;
    HostAndPort _local;
    RestrictionEnvironment _restrictionEnvironment;
    struct s2n_connection* _s2nConnection;
    struct s2n_config* _s2nConfig;
    boost::optional<Milliseconds> _timeout;
};

}  // namespace mongo::transport::handoff_transport
