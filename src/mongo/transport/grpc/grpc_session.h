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

#include <memory>

#include <boost/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/config.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/transport/grpc/client_context.h"
#include "mongo/transport/grpc/client_stream.h"
#include "mongo/transport/grpc/server_context.h"
#include "mongo/transport/grpc/server_stream.h"
#include "mongo/transport/grpc/util.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/synchronized_value.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::transport::grpc {

/**
 * Captures the common semantics for ingress and egress gRPC sessions.
 */
class GRPCSession : public Session {
public:
    explicit GRPCSession(TransportLayer* tl, HostAndPort remote, boost::optional<UUID> clientId)
        : _tl(tl), _remote(std::move(remote)), _clientId(std::move(clientId)) {}

    virtual ~GRPCSession() = default;

    /**
     * Returns the unique identifier used for the underlying gRPC stream.
     */
    boost::optional<UUID> clientId() const {
        return _clientId;
    };

    const HostAndPort& remote() const {
        return _remote;
    }

    /**
     * Cancels the RPC associated with the underlying gRPC stream and updates the termination status
     * of the session to include the provided reason.
     *
     * In-progress reads and writes to this session will be interrupted, and future reads and writes
     * will fail with an error.
     *
     * If this session is already terminated, this has no effect.
     */
    void cancel(StringData reason) {
        // Need to update terminationStatus before cancelling so that when the RPC caller/handler is
        // interrupted, it will be guaranteed to have access to the reason for cancellation.
        if (_setTerminationStatus({ErrorCodes::CallbackCanceled, std::move(reason)})) {
            _tryCancel();
        }
    }

    /**
     * Mark the session as gracefully terminated.
     *
     * In-progress reads and writes to this session will not be interrupted, but future ones will
     * fail with an error.
     *
     * If this session is already terminated, this has no effect.
     */
    void end() final {
        _setTerminationStatus(Status::OK());
    }

    StatusWith<Message> sourceMessage() noexcept override {
        if (MONGO_likely(isConnected())) {
            if (auto maybeBuffer = _readFromStream()) {
                return Message(std::move(*maybeBuffer));
            }
        }
        return Status(ErrorCodes::StreamTerminated, "Unable to read from gRPC stream");
    }

    Status sinkMessage(Message message) noexcept override {
        if (MONGO_likely(isConnected() && _writeToStream(message.sharedBuffer()))) {
            return Status::OK();
        }
        return Status(ErrorCodes::StreamTerminated, "Unable to write to gRPC stream");
    }

    /**
     * Returns the reason for which this stream was terminated, if any. "Termination" includes
     * cancellation events (e.g. network interruption, explicit cancellation, or
     * exceeding the deadline) as well as graceful closing of the session via end().
     *
     * Remains unset until termination.
     */
    boost::optional<Status> terminationStatus() const {
        auto status = _terminationStatus.synchronize();
        // If the RPC was cancelled, return a status reflecting that, including in the case where
        // the RPC was cancelled after the session was already locally ended (i.e. after the
        // termination status was set to OK).
        if (_isCancelled() && (!status->has_value() || (*status)->isOK())) {
            return Status(ErrorCodes::CallbackCanceled,
                          "gRPC session was terminated due to the associated RPC being cancelled");
        }
        return *status;
    }

    TransportLayer* getTransportLayer() const final {
        return _tl;
    }

    /**
     * The following inspects the logical state of the underlying stream: the session is considered
     * not connected when the user has terminated the session (either with or without an error).
     */
    bool isConnected() final {
        return !_terminationStatus->has_value();
    }

    /**
     * For ingress sessions, we do not distinguish between load-balanced and non-load-balanced
     * streams. Egress sessions never originate from load-balancers.
     */
    bool isFromLoadBalancer() const final {
        return false;
    }

    std::string clientIdStr() const {
        return _clientId ? _clientId->toString() : "N/A";
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

protected:
    /**
     * Sets the termination status if it hasn't been set already.
     * Returns whether the termination status was updated or not.
     */
    bool _setTerminationStatus(Status status) {
        auto ts = _terminationStatus.synchronize();
        if (MONGO_unlikely(ts->has_value() || _isCancelled()))
            return false;
        ts->emplace(std::move(status));
        return true;
    }

private:
    virtual void _tryCancel() = 0;

    virtual bool _isCancelled() const = 0;

    virtual boost::optional<SharedBuffer> _readFromStream() = 0;

    virtual bool _writeToStream(ConstSharedBuffer msg) = 0;

    // TODO SERVER-74020: replace this with `GRPCTransportLayer`.
    TransportLayer* const _tl;

    const HostAndPort _remote;
    const boost::optional<UUID> _clientId;

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
        : GRPCSession(tl, ctx->getRemote(), std::move(clientId)), _ctx(ctx), _stream(stream) {
        LOGV2_DEBUG(7401101,
                    2,
                    "Constructed a new gRPC ingress session",
                    "id"_attr = id(),
                    "remoteClientId"_attr = clientIdStr(),
                    "remote"_attr = remote());
    }

    ~IngressSession() {
        auto ts = terminationStatus();
        tassert(
            7401491, "gRPC sessions must be terminated before being destructed", ts.has_value());
        LOGV2_DEBUG(7401402,
                    2,
                    "Finished cleaning up a gRPC ingress session",
                    "id"_attr = id(),
                    "remoteClientId"_attr = clientIdStr(),
                    "remote"_attr = remote(),
                    "status"_attr = *ts);
    }

    /**
     * Mark the session as logically terminated with the provided status. In-progress reads and
     * writes to this session will not be interrupted, but future attempts to read or write to this
     * session will fail.
     *
     * This has no effect if the stream is already terminated.
     */
    void terminate(Status status) {
        _setTerminationStatus(std::move(status));
    }

private:
    void _tryCancel() override {
        _ctx->tryCancel();
    }

    bool _isCancelled() const override {
        return _ctx->isCancelled();
    }

    boost::optional<SharedBuffer> _readFromStream() override {
        return _stream->read();
    }

    bool _writeToStream(ConstSharedBuffer msg) override {
        return _stream->write(msg);
    }

    // _stream is only valid while the RPC handler is still running. It should not be
    // accessed after the stream has been terminated.
    ServerContext* const _ctx;
    ServerStream* const _stream;
};

/**
 * Represents the client side of a gRPC stream.
 */
class EgressSession final : public GRPCSession {
public:
    EgressSession(TransportLayer* tl,
                  std::shared_ptr<ClientContext> ctx,
                  std::shared_ptr<ClientStream> stream,
                  boost::optional<UUID> clientId)
        : GRPCSession(tl, ctx->getRemote(), std::move(clientId)),
          _ctx(std::move(ctx)),
          _stream(std::move(stream)) {
        LOGV2_DEBUG(7401401,
                    2,
                    "Constructed a new gRPC egress session",
                    "id"_attr = id(),
                    "remoteClientId"_attr = clientIdStr(),
                    "remote"_attr = remote());
    }

    ~EgressSession() {
        auto ts = terminationStatus();
        tassert(
            7401411, "gRPC sessions must be terminated before being destructed", ts.has_value());
        LOGV2_DEBUG(7401403,
                    2,
                    "Finished cleaning up a gRPC egress session",
                    "id"_attr = id(),
                    "localClientId"_attr = clientIdStr(),
                    "remote"_attr = remote(),
                    "status"_attr = *ts);
    }

    /**
     * Indicates to the server side that the client will not be sending any further messages, then
     * blocks until all messages from the server have been read and the server has returned a final
     * status. Once a status has been received, this session's termination status is updated
     * accordingly.
     *
     * Returns the termination status.
     *
     * This method should only be called once.
     * This method should be used instead of end() in most cases, since it retrieves the server's
     * return status for the RPC.
     */
    Status finish() {
        _setTerminationStatus(util::convertStatus(_stream->finish()));
        return *terminationStatus();
    }

private:
    void _tryCancel() override {
        _ctx->tryCancel();
    }

    bool _isCancelled() const override {
        // There is no way of determining this client-side outside of finish().
        return false;
    }

    boost::optional<SharedBuffer> _readFromStream() override {
        return _stream->read();
    }

    bool _writeToStream(ConstSharedBuffer msg) override {
        return _stream->write(msg);
    }

    const std::shared_ptr<ClientContext> _ctx;
    const std::shared_ptr<ClientStream> _stream;
};

}  // namespace mongo::transport::grpc

#undef MONGO_LOGV2_DEFAULT_COMPONENT
