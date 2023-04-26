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

#include <boost/optional.hpp>
#include <memory>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/config.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/transport/grpc/server_context.h"
#include "mongo/transport/grpc/server_stream.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/synchronized_value.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::transport::grpc {

/**
 * Captures the common semantics for ingress and egress gRPC sessions.
 */
class GRPCSession : public Session {
public:
    explicit GRPCSession(TransportLayer* tl) : _tl(tl) {}

    virtual ~GRPCSession() {
        invariant(terminationStatus(), "gRPC sessions must always be terminated");
    }

    /**
     * Returns the unique identifier used for the underlying gRPC stream.
     */
    virtual boost::optional<UUID> clientId() const = 0;

    /**
     * Terminates the underlying gRPC stream.
     */
    virtual void terminate(Status status) {
        auto ts = _terminationStatus.synchronize();
        if (MONGO_unlikely(ts->has_value()))
            return;
        ts->emplace(std::move(status));
    }

    /**
     * Returns the termination status (always set at termination). Remains unset until termination.
     */
    boost::optional<Status> terminationStatus() const {
        return **_terminationStatus;
    }

    TransportLayer* getTransportLayer() const final {
        return _tl;
    }

    void end() final {
        terminate(Status::OK());
    }

    /**
     * For ingress sessions, we do not distinguish between load-balanced and non-load-balanced
     * streams. Egress sessions never originate from load-balancers.
     */
    bool isFromLoadBalancer() const final {
        return false;
    }

    /**
     * The following APIs are not implemented for both ingress and egress gRPC sessions.
     */
    Future<Message> asyncSourceMessage(const BatonHandle&) noexcept final {
        MONGO_UNIMPLEMENTED;
    }

    Status waitForData() noexcept final {
        MONGO_UNIMPLEMENTED;
    }

    Future<void> asyncWaitForData() noexcept final {
        MONGO_UNIMPLEMENTED;
    }

    Future<void> asyncSinkMessage(Message, const BatonHandle&) noexcept final {
        MONGO_UNIMPLEMENTED;
    }

    void cancelAsyncOperations(const BatonHandle&) final {
        MONGO_UNIMPLEMENTED;
    }

    void setTimeout(boost::optional<Milliseconds>) final {
        MONGO_UNIMPLEMENTED;
    }

#ifdef MONGO_CONFIG_SSL
    const std::shared_ptr<SSLManagerInterface>& getSSLManager() const final {
        MONGO_UNIMPLEMENTED;
    }
#endif

    // TODO SERVER-71100: remove `local()`, `remoteAddr()`, and `localAddr()`.
    const HostAndPort& local() const final {
        MONGO_UNIMPLEMENTED;
    }

    const SockAddr& remoteAddr() const final {
        MONGO_UNIMPLEMENTED;
    }

    const SockAddr& localAddr() const final {
        MONGO_UNIMPLEMENTED;
    }

private:
    // TODO SERVER-74020: replace this with `GRPCTransportLayer`.
    TransportLayer* const _tl;

    synchronized_value<boost::optional<Status>> _terminationStatus;
};

/**
 * Represents an ingress gRPC session (the server side of the stream).
 *
 * Calling sinkMessage() or sourceMessage() is only allowed from the thread that owns the underlying
 * gRPC stream. This is necessary to ensure accessing _ctx and _stream does not result in
 * use-after-free.
 */
class IngressSession final : public GRPCSession {
public:
    IngressSession(TransportLayer* tl,
                   ServerContext* ctx,
                   ServerStream* stream,
                   boost::optional<UUID> clientId)
        : GRPCSession(tl),
          _ctx(ctx),
          _stream(stream),
          _clientId(std::move(clientId)),
          _remote(ctx->getHostAndPort()) {
        LOGV2_DEBUG(7401101,
                    2,
                    "Constructed a new gRPC ingress session",
                    "id"_attr = id(),
                    "remoteClientId"_attr = clientIdStr(),
                    "remote"_attr = _remote);
    }

    ~IngressSession() {
        auto ts = terminationStatus();
        LOGV2_DEBUG(7401102,
                    2,
                    "Finished cleaning up a gRPC ingress session",
                    "id"_attr = id(),
                    "remoteClientId"_attr = clientIdStr(),
                    "remote"_attr = _remote,
                    "status"_attr = ts);
        if (MONGO_unlikely(!ts)) {
            terminate({ErrorCodes::StreamTerminated, "Terminating session through destructor"});
        }
    }

    boost::optional<UUID> clientId() const override {
        return _clientId;
    }
    /**
     * The following inspects the logical state of the underlying stream: the session is considered
     * not connected when: the underlying stream is closed/canceled, or the user has terminated the
     * session (either with or without an error).
     */
    bool isConnected() override {
        return !(terminationStatus() || _ctx->isCancelled());
    }

    StatusWith<Message> sourceMessage() noexcept override {
        if (MONGO_likely(isConnected())) {
            if (auto maybeBuffer = _stream->read()) {
                return Message(std::move(*maybeBuffer));
            }
        }
        return Status(ErrorCodes::StreamTerminated, "Unable to read from ingress session");
    }

    Status sinkMessage(Message message) noexcept override {
        if (MONGO_likely(isConnected() && _stream->write(message.sharedBuffer()))) {
            return Status::OK();
        }
        return Status(ErrorCodes::StreamTerminated, "Unable to write to ingress session");
    }

    void terminate(Status status) override {
        auto shouldCancel = !status.isOK() && !terminationStatus();
        // Need to invoke GRPCSession::terminate before cancelling the server context so that when
        // an RPC handler is interrupted, it will be guaranteed to have access to its termination
        // status.
        GRPCSession::terminate(std::move(status));
        if (MONGO_unlikely(shouldCancel)) {
            _ctx->tryCancel();
        }
    }

    const HostAndPort& remote() const override {
        return _remote;
    }

    std::string clientIdStr() const {
        return _clientId ? _clientId->toString() : "N/A";
    }

private:
    // _ctx and _stream are only valid while the RPC handler is still running. They should not be
    // accessed after the stream has been terminated.
    ServerContext* const _ctx;
    ServerStream* const _stream;
    const boost::optional<UUID> _clientId;
    const HostAndPort _remote;
};

}  // namespace mongo::transport::grpc

#undef MONGO_LOGV2_DEFAULT_COMPONENT
