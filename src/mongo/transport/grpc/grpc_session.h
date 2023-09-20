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

#include <charconv>
#include <memory>
#include <string>

#include <boost/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/config.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/transport/grpc/client_context.h"
#include "mongo/transport/grpc/client_stream.h"
#include "mongo/transport/grpc/server_context.h"
#include "mongo/transport/grpc/server_stream.h"
#include "mongo/transport/grpc/util.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/base64.h"
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

    virtual ~GRPCSession() {
        if (_cleanupCallback)
            (*_cleanupCallback)(*this);
    }

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
     *
     * The provided reason MUST be a cancellation error.
     */
    void cancel(Status reason) {
        invariant(ErrorCodes::isCancellationError(reason));
        // Need to update terminationStatus before cancelling so that when the RPC caller/handler is
        // interrupted, it will be guaranteed to have access to the reason for cancellation.
        if (_setTerminationStatus(std::move(reason))) {
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
     * not connected when the user has terminated the session (either with or without an error) or
     * if the RPC had been cancelled (either remotely/externally or locally).
     */
    bool isConnected() final {
        return !_terminationStatus->has_value() && !_isCancelled();
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
     * Runs the provided callback when destroying the session.
     * Not synchronized, thus not safe to call once the session is visible to other threads.
     */
    void setCleanupCallback(std::function<void(const GRPCSession&)> callback) {
        _cleanupCallback.emplace(std::move(callback));
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

    void appendToBSON(BSONObjBuilder& bb) const override {
        // No uint64_t BSON type
        bb.append("id", static_cast<long>(id()));
        bb.append("clientId", clientIdStr());
        bb.append("remote", remote().toString());
    }

    bool shouldOverrideMaxConns(
        const std::vector<stdx::variant<CIDR, std::string>>& exemptions) const override {
        MONGO_UNIMPLEMENTED;
    }

    const RestrictionEnvironment& getAuthEnvironment() const override {
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

    TransportLayer* const _tl;

    const HostAndPort _remote;
    const boost::optional<UUID> _clientId;

    boost::optional<std::function<void(const GRPCSession&)>> _cleanupCallback;
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
                   boost::optional<UUID> clientId,
                   boost::optional<std::string> authToken,
                   boost::optional<StringData> encodedClientMetadata)
        : GRPCSession(tl, ctx->getRemote(), std::move(clientId)),
          _ctx(ctx),
          _stream(stream),
          _authToken(std::move(authToken)),
          _encodedClientMetadata(std::move(encodedClientMetadata)) {
        LOGV2_DEBUG(
            7401101, 2, "Constructed a new gRPC ingress session", "session"_attr = toBSON());
    }

    ~IngressSession() {
        auto ts = terminationStatus();
        tassert(
            7401491, "gRPC sessions must be terminated before being destructed", ts.has_value());
        LOGV2_DEBUG(7401402,
                    2,
                    "Finished cleaning up a gRPC ingress session",
                    "session"_attr = toBSON(),
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

    /**
     * The client-provided authentication token, if any.
     *
     * This will only return a value if the underlying stream was created via
     * AuthenticatedCommandStream. Any authentication token provided by the client to
     * UnauthenticatedCommandStream will be ignored.
     */
    const boost::optional<std::string>& authToken() const {
        return _authToken;
    }

    /**
     * Retrieve the ClientMetadata, if any.
     * The first time this method is called, the metadata document will be decoded and parsed, which
     * can be expensive. It will be cached for future invocations.
     *
     * Throws an exception if the client provided improperly formatted metadata.
     */
    boost::optional<const ClientMetadata&> getClientMetadata() const {
        if (!_encodedClientMetadata) {
            return boost::none;
        }

        auto decoded = _decodedClientMetadata.synchronize();

        if (decoded->has_value()) {
            return **decoded;
        }

        fmt::memory_buffer buffer{};
        base64::decode(buffer, *_encodedClientMetadata);
        BSONObj metadataDocument(buffer.data());
        decoded->emplace(std::move(metadataDocument));
        return **decoded;
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

    boost::optional<std::string> _authToken;
    boost::optional<StringData> _encodedClientMetadata;
    mutable synchronized_value<boost::optional<ClientMetadata>> _decodedClientMetadata;
};

/**
 * Represents the client side of a gRPC stream.
 */
class EgressSession final : public GRPCSession {
public:
    /**
     * Holds the state shared between multiple instances of egress session.
     * This state is currently limited to the cluster's maxWireVersion.
     * No alignment is needed as the shared state is not expected to be modified frequently.
     */
    struct SharedState {
        AtomicWord<int> clusterMaxWireVersion;
    };

    EgressSession(TransportLayer* tl,
                  std::shared_ptr<ClientContext> ctx,
                  std::shared_ptr<ClientStream> stream,
                  boost::optional<UUID> clientId,
                  std::shared_ptr<SharedState> sharedState)
        : GRPCSession(tl, ctx->getRemote(), std::move(clientId)),
          _ctx(std::move(ctx)),
          _stream(std::move(stream)),
          _sharedState(std::move(sharedState)) {
        LOGV2_DEBUG(7401401, 2, "Constructed a new gRPC egress session", "session"_attr = toBSON());
    }

    ~EgressSession() {
        auto ts = terminationStatus();
        tassert(
            7401411, "gRPC sessions must be terminated before being destructed", ts.has_value());
        LOGV2_DEBUG(7401403,
                    2,
                    "Finished cleaning up a gRPC egress session",
                    "session"_attr = toBSON(),
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

    void _updateWireVersion() {
        // The cluster's wire version is communicated via the initial metadata, so only need to
        // check it after reading the first message.
        if (_checkedWireVersion.load() || _checkedWireVersion.swap(true)) {
            return;
        }

        auto metadata = _ctx->getServerInitialMetadata();

        auto log = [](auto error) {
            LOGV2_WARNING(
                7401602, "Failed to parse cluster's maxWireVersion", "error"_attr = error);
        };

        auto clusterWireVersionEntry = metadata.find(util::constants::kClusterMaxWireVersionKey);
        if (MONGO_unlikely(clusterWireVersionEntry == metadata.end())) {
            log(fmt::format("cannot find metadata field: {}",
                            util::constants::kClusterMaxWireVersionKey));
            return;
        }

        int wireVersion = 0;
        if (std::from_chars(clusterWireVersionEntry->second.begin(),
                            clusterWireVersionEntry->second.end(),
                            wireVersion)
                .ec != std::errc{}) {

            log(fmt::format("invalid cluster maxWireVersion value: \"{}\"",
                            clusterWireVersionEntry->second));
            return;
        }

        // The following tries to avoid modifying the cache-line that holds the
        // `clusterMaxWireVersion` when possible. Considering this code-path runs once for each
        // outgoing stream, and the cluster maxWireVersion is not expected to change frequently,
        // this avoids unnecessary cache evictions and is considered a performance optimization.
        if (_sharedState->clusterMaxWireVersion.load() != wireVersion) {
            _sharedState->clusterMaxWireVersion.store(wireVersion);
        }
    }

    boost::optional<SharedBuffer> _readFromStream() override {
        if (auto msg = _stream->read()) {
            _updateWireVersion();
            return msg;
        }
        return boost::none;
    }

    bool _writeToStream(ConstSharedBuffer msg) override {
        return _stream->write(msg);
    }

    AtomicWord<bool> _checkedWireVersion;
    const std::shared_ptr<ClientContext> _ctx;
    const std::shared_ptr<ClientStream> _stream;
    std::shared_ptr<SharedState> _sharedState;
};

}  // namespace mongo::transport::grpc

#undef MONGO_LOGV2_DEFAULT_COMPONENT
