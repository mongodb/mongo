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
 *
 * Each GRPCSession corresponds to a single bidirectional gRPC stream, and each stream is created
 * by an invocation of a gRPC method (a gRPC "call" or RPC). Each gRPC call ultimately terminates
 * with a status, typically returned by the server-side if the call completes normally, which is a
 * tuple of a gRPC status code and optionally a message if the status code is not OK. `GRPCSession`
 * has a notion of a "termination status", which maps to this final status of the call, and it can
 * be set in a few different ways depending on whether the session is an ingress or egress session.
 *
 * At any time, the call associated with the session may be cancelled, in which case the termination
 * status will be set to a status that reflects this cancelled state.
 *
 * Invoking `GRPCSession::end()` will cancel the call, interrupting any in-progress reads and
 * writes, unless a termination status had already been set, in which case `GRPCSession::end()` will
 * have no effect. If a session does not have a termination status when it is destructed,
 * `GRPCSession::end()` will be invoked in the destructor.
 *
 * If a session has been terminated, attempting to sink or source a message will return that
 * termination status. If the session was terminated with an OK status, then
 * `ErrorCodes::StreamTerminated` will be returned.
 *
 * See the documentation for `IngressSession` and `EgressSession` for more information on the
 * termination semantics of each type of session.
 *
 * For more information on the lifecycle of gRPC calls, see
 * https://grpc.io/docs/what-is-grpc/core-concepts/#bidirectional-streaming-rpc.
 */
class GRPCSession : public Session {
public:
    explicit GRPCSession(TransportLayer* tl, HostAndPort remote)
        : _tl(tl), _remote(std::move(remote)) {
        SockAddr remoteAddr;
        try {
            remoteAddr = SockAddr::create(_remote.host(), _remote.port(), AF_UNSPEC);
        } catch (const DBException& ex) {
            // If {remote} fails to parse for any reason, allow the session to continue anyway.
            // {_restrictionEnvironment} will end up with an AF_UNSPEC remote address
            // and will fail closed, rejecting any AddressRestriction present for the user/role.
            LOGV2_DEBUG(8128400,
                        2,
                        "Unable to parse peer name",
                        "host"_attr = _remote.host(),
                        "port"_attr = _remote.port(),
                        "error"_attr = ex.toStatus());
        }

        // libgrpc does not expose local socket name for us.
        // This means that any attempt to use a {serverAddress} authentication restriction
        // with the GRPC protocol will fail to permit login.
        _restrictionEnvironment = RestrictionEnvironment(std::move(remoteAddr), SockAddr());
    }

    virtual ~GRPCSession() {
        if (_cleanupCallback)
            (*_cleanupCallback)(*this);
    }

    const HostAndPort& remote() const override {
        return _remote;
    }

    const HostAndPort& local() const override {
        return _local;
    }

    StatusWith<Message> sourceMessage() noexcept override {
        if (auto s = _verifyNotTerminated(); !s.isOK()) {
            return s.withContext("Could not read from gRPC stream");
        }

        return _readFromStream();
    }

    Status sinkMessage(Message m) noexcept override {
        if (auto s = _verifyNotTerminated(); !s.isOK()) {
            return s.withContext("Could not write to gRPC stream");
        }

        return _writeToStream(std::move(m));
    }

    /**
     * Cancels the RPC associated with this session's stream.
     * If the session had already been terminated, this has no effect.
     */
    void end() override {
        cancel(Status(ErrorCodes::CallbackCanceled, "gRPC call was cancelled"));
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
     * Returns the reason for which this session was terminated, if any. "Termination" includes
     * cancellation events (e.g. network interruption, explicit cancellation, or
     * exceeding the deadline) as well as explicit setting of the status via setTerminationStatus().
     *
     * Remains unset until termination.
     */
    boost::optional<Status> terminationStatus() const {
        auto cancelled = _isCancelled();
        auto status = _terminationStatus.synchronize();
        // If the RPC was cancelled, return a status reflecting that, including in the case
        // where the RPC was cancelled after the session was already locally ended (i.e. after
        // the termination status was set to OK).
        if (cancelled && (!status->has_value() || (*status)->isOK())) {
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
    bool isConnectedToLoadBalancerPort() const final {
        return false;
    }

    bool isLoadBalancerPeer() const final {
        return false;
    }

    void setisLoadBalancerPeer(bool helloHasLoadBalancedOption) final {
        tassert(ErrorCodes::OperationFailed,
                "Unable to set loadBalancer option on GRPC connection",
                !helloHasLoadBalancedOption);
    }

    /**
     * All gRPC sessions are considered bound to the operation state.
     */
    bool bindsToOperationState() const final {
        return true;
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

    bool isExemptedByCIDRList(
        const std::vector<std::variant<CIDR, std::string>>& exemptions) const override {
        return false;
    }

    const RestrictionEnvironment& getAuthEnvironment() const override {
        return _restrictionEnvironment;
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

    synchronized_value<boost::optional<Status>> _terminationStatus;

private:
    virtual void _tryCancel() = 0;

    virtual bool _isCancelled() const = 0;

    virtual StatusWith<Message> _readFromStream() = 0;

    virtual Status _writeToStream(Message m) = 0;

    /**
     * Perform all the pre-read/write checks on the underlying stream, returning a status that the
     * read/write should fail with if the session had already been terminated.
     *
     * Note that this does not check to see if the call had been externally cancelled when
     * determining whether to return OK or not, since doing so on every read/write would be
     * expensive. If a termination status had been set locally, this method will check for
     * cancellation before returning that status, however.
     */
    Status _verifyNotTerminated() const {
        // Check the cached _terminationStatus directly rather than invoking terminationStatus() to
        // avoid the overhead of checking if the RPC has been cancelled. If it has been cancelled
        // then reading from or writing to the stream will fail anyways.
        if (_terminationStatus->has_value()) {
            if (auto ts = terminationStatus(); !ts->isOK()) {
                return *ts;
            }
            return Status(ErrorCodes::StreamTerminated, "gRPC stream is terminated");
        }

        return Status::OK();
    }

    TransportLayer* const _tl;

    const HostAndPort _remote;
    HostAndPort _local;
    RestrictionEnvironment _restrictionEnvironment;

    boost::optional<std::function<void(const GRPCSession&)>> _cleanupCallback;
};

/**
 * Represents an ingress gRPC session (the server side of the stream).
 *
 * Calling sinkMessage() or sourceMessage() is only allowed from the thread that owns the underlying
 * gRPC stream. This is necessary to ensure accessing _ctx and _stream does not result in
 * use-after-free.
 *
 * If reading from a non-terminated session fails but the associated gRPC call was not cancelled, it
 * indicates that the client side of the stream stopped writing gracefully, and that the call can
 * terminate cleanly. In such cases, the termination status of the session will be set to OK.
 *
 * If writing to a non-terminated session fails for any reason, it indicates that the client side
 * will not be able to consume the response of the call or its final status, and thus the
 * termination status will be set to CANCELLED.
 *
 * The termination status may also be set to a specific status manually (e.g. when an improperly
 * formatted metadata entry has been received) via `IngressSession::setTerminationStatus`. This will
 * prevent any future reads or writes from being performed, but it will not interrupt any
 * in-progress reads or writes. If the stream had already been terminated, this will have no effect.
 *
 * If, at any time, the call is cancelled (e.g. explicitly by the client, via a network
 * event, or by a server thread), the termination status will be set to CANCELLED, regardless of
 * whether it had been set to some other value via setTerminationStatus.
 *
 * The following state diagram is an overview of the various ways an ingress session's termination
 * status can be set.
 *
 *                +----------------------------------------------------------
 *                |                                                         |
 *       Read fails, client                           +------------------+  |
 *         done writing                        +----->|Other termination |  |
 *                +                            |      |     status       |  |
 *                |                            |      +------------------+  |
 *   +------------------------+                |              +----+        |
 *   | Non-terminated session | ---- setTerminationStatus---> | OK | <------+
 *   +------------------------+                |              +----+
 *      |                 |                    |        +---------+
 * Write fails      cancellation               +------> |CANCELLED|
 *      |               event                           +---------+
 *      |                 |                                ^   ^
 *      |                 |                                |   |
 *      |                 +---------------------------------   |
 *      +-------------------------------------------------------
 */
class IngressSession final : public GRPCSession {
public:
    IngressSession(TransportLayer* tl,
                   ServerContext* ctx,
                   ServerStream* stream,
                   boost::optional<UUID> clientId,
                   boost::optional<std::string> authToken,
                   boost::optional<StringData> encodedClientMetadata)
        : GRPCSession(tl, ctx->getRemote()),
          _ctx(ctx),
          _stream(stream),
          _authToken(std::move(authToken)),
          _remoteClientId(clientId),
          _encodedClientMetadata(std::move(encodedClientMetadata)) {
        LOGV2_DEBUG(
            7401101, 2, "Constructed a new gRPC ingress session", "session"_attr = toBSON());
    }

    ~IngressSession() {
        end();
        LOGV2_DEBUG(7401402,
                    2,
                    "Finished cleaning up a gRPC ingress session",
                    "session"_attr = toBSON(),
                    "status"_attr = terminationStatus());
    }

    StatusWith<Message> _readFromStream() noexcept override {
        if (auto maybeBuffer = _stream->read()) {
            return Message(std::move(*maybeBuffer));
        }

        if (auto ts = terminationStatus()) {
            return *ts;
        } else {
            // If the client gracefully terminated, set the RPC's final status to OK.
            _setTerminationStatus(Status::OK());
            return Status(ErrorCodes::StreamTerminated,
                          "Could not read from gRPC server stream: remote done writing");
        }
    }

    Status _writeToStream(Message message) noexcept override {
        if (_stream->write(message.sharedBuffer())) {
            return Status::OK();
        }

        if (auto ts = terminationStatus()) {
            return *ts;
        } else {
            // If the client closed the stream before we had a chance to return our response, mark
            // the RPC as cancelled.
            auto status = Status(ErrorCodes::CallbackCanceled,
                                 "Could not write to gRPC server stream: remote done reading");
            _setTerminationStatus(status);
            return status;
        }
    }

    boost::optional<UUID> getRemoteClientId() const {
        return _remoteClientId;
    }

    std::string remoteClientIdToString() const {
        return _remoteClientId ? _remoteClientId->toString() : "N/A";
    }

    /**
     * Mark the session as logically terminated with the provided status. In-progress reads and
     * writes to this session will not be interrupted, but future attempts to read or write to this
     * session will fail.
     *
     * This has no effect if the session is already terminated.
     */
    void setTerminationStatus(Status status) {
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

    void appendToBSON(BSONObjBuilder& bb) const override {
        // No uint64_t BSON type
        bb.append("id", static_cast<long>(id()));
        bb.append("remoteClientId", remoteClientIdToString());
        bb.append("remote", remote().toString());
    }

private:
    void _tryCancel() override {
        _ctx->tryCancel();
    }

    bool _isCancelled() const override {
        return _ctx->isCancelled();
    }

    // _stream is only valid while the RPC handler is still running. It should not be
    // accessed after the stream has been terminated.
    ServerContext* const _ctx;
    ServerStream* const _stream;

    boost::optional<std::string> _authToken;
    boost::optional<UUID> _remoteClientId;
    boost::optional<StringData> _encodedClientMetadata;
    mutable synchronized_value<boost::optional<ClientMetadata>> _decodedClientMetadata;
};

/**
 * Represents the client side of a gRPC stream.
 *
 * If reading from or writing to a non-terminated session fails, it indicates that the call has been
 * terminated and that the final termination status can be retrieved (most likely as determined by
 * the server-side). In such cases, the session will retrieve this status, set its termination
 * status to that value, and then return it from the read/write method that failed.
 *
 * If, at any time, the call is cancelled (e.g. explicitly by the server, via a network
 * event, or explicitly by a client thread), the termination status will be set to CANCELLED.
 *
 * A caller may explicitly indicate that no more client writes are forthcoming and block until the
 * a final termination status has been determined by calling EgressSession::finish(). Note that this
 * will block until any outstanding messages sent by the server have been read, if any.
 *
 * The following state diagram is an overview of the various ways an egress session's termination
 * status can be set.
 *
 *             +-------------  read/write fails
 *             |                      |
 * +------------------------+         V              +------------------+
 * | Non-terminated session | ----- finish() ------> |   RPC's final    |
 * +------------------------+                        |termination status|
 *    |         |                                    +------------------+
 *    |         +---- external cancellation event
 *    |                              |
 *    |                              V
 *    |                           +---------+
 *    +--- cancel(), end() -----> |CANCELLED|
 *          ~GRPCSession          +---------+
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
                  UUID clientId,
                  std::shared_ptr<SharedState> sharedState)
        : GRPCSession(tl, ctx->getRemote()),
          _ctx(std::move(ctx)),
          _stream(std::move(stream)),
          _clientId(clientId),
          _sharedState(std::move(sharedState)) {
        LOGV2_DEBUG(7401401, 2, "Constructed a new gRPC egress session", "session"_attr = toBSON());
    }

    ~EgressSession() {
        end();
        LOGV2_DEBUG(7401403,
                    2,
                    "Finished cleaning up a gRPC egress session",
                    "session"_attr = toBSON(),
                    "status"_attr = terminationStatus());
    }

    StatusWith<Message> _readFromStream() noexcept override {
        if (auto maybeBuffer = _stream->read()) {
            _updateWireVersion();
            return Message(std::move(*maybeBuffer));
        }

        // If _stream->read() fails, then the server has no more messages to send and a final RPC
        // status should be available. Set the termination status to that and return it here.
        return finish();
    }

    Status _writeToStream(Message message) noexcept override {
        if (_stream->write(message.sharedBuffer())) {
            return Status::OK();
        }

        // If _stream->write() fails, then the RPC has been terminated and a final RPC
        // status should be available. Set the termination status to that and return it here.
        return finish();
    }

    /**
     * Get this session's current idea of what the cluster's maxWireVersion is.
     *
     * The initial value for this is the first wire version that included gRPC support.
     */
    int getClusterMaxWireVersion() const {
        return _sharedState->clusterMaxWireVersion.load();
    };

    /**
     * Indicates to the server side that the client will not be sending any further messages, then
     * blocks until all messages from the server have been read and the server has returned a final
     * status. Once a status has been received, this session's termination status is updated
     * accordingly.
     *
     * Returns the termination status.
     *
     * This method should be used instead of end() in most cases, since it retrieves the server's
     * return status for the RPC.
     */
    Status finish() {
        auto status = _terminationStatus.synchronize();
        if (!status->has_value()) {
            *status = util::convertStatus(_stream->finish());
        }
        return **status;
    }

    UUID getClientId() const {
        return _clientId;
    }

    void appendToBSON(BSONObjBuilder& bb) const override {
        // No uint64_t BSON type
        bb.append("id", static_cast<long>(id()));
        bb.append("clientId", _clientId.toString());
        bb.append("remote", remote().toString());
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

    AtomicWord<bool> _checkedWireVersion;
    const std::shared_ptr<ClientContext> _ctx;
    const std::shared_ptr<ClientStream> _stream;
    UUID _clientId;
    std::shared_ptr<SharedState> _sharedState;
};

}  // namespace mongo::transport::grpc

#undef MONGO_LOGV2_DEFAULT_COMPONENT
