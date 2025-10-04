/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/transport/grpc/grpc_session.h"

#include "mongo/config.h"
#include "mongo/db/stats/counters.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/transport/grpc/util.h"
#include "mongo/util/base64.h"

#include <charconv>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::transport::grpc {

GRPCSession::GRPCSession(TransportLayer* tl, HostAndPort remote)
    : _tl(tl), _remote(std::move(remote)) {
    SockAddr remoteAddr;
    try {
        remoteAddr = SockAddr::create(_remote.host(), _remote.port(), AF_UNSPEC);
        _local = HostAndPort();
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

StatusWith<Message> GRPCSession::sourceMessage() {
    if (auto s = _verifyNotTerminated(); !s.isOK()) {
        return s.withContext("Could not read from gRPC stream");
    }

    return _readFromStream();
}

Status GRPCSession::sinkMessage(Message m) {
    if (auto s = _verifyNotTerminated(); !s.isOK()) {
        return s.withContext("Could not write to gRPC stream");
    }

    return _writeToStream(std::move(m));
}

Future<Message> GRPCSession::asyncSourceMessage(const BatonHandle&) {
    if (auto s = _verifyNotTerminated(); !s.isOK()) {
        return Future<Message>::makeReady(s.withContext("Could not read from gRPC stream"));
    }

    return _asyncReadFromStream();
}

Future<void> GRPCSession::asyncSinkMessage(Message m, const BatonHandle&) {
    if (auto s = _verifyNotTerminated(); !s.isOK()) {
        return Future<void>::makeReady(s.withContext("Could not write to gRPC stream"));
    }

    return _asyncWriteToStream(std::move(m));
}

void GRPCSession::cancel(Status reason) {
    invariant(ErrorCodes::isCancellationError(reason));
    // Need to update terminationStatus before cancelling so that when the RPC caller/handler is
    // interrupted, it will be guaranteed to have access to the reason for cancellation.
    if (_setTerminationStatus(reason)) {
        LOGV2_DEBUG(9936113, 2, "Cancelling gRPC stream", "reason"_attr = reason);
        _tryCancel();
    }
}

void GRPCSession::setisLoadBalancerPeer(bool helloHasLoadBalancedOption) {
    tassert(ErrorCodes::OperationFailed,
            "Unable to set loadBalancer option on GRPC connection",
            !helloHasLoadBalancedOption);
}

boost::optional<Status> GRPCSession::terminationStatus() const {
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

bool GRPCSession::_setTerminationStatus(Status status) {
    auto ts = _terminationStatus.synchronize();
    if (ts->has_value() || _isCancelled())
        return false;
    ts->emplace(std::move(status));
    return true;
}

Status GRPCSession::_verifyNotTerminated() const {
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

IngressSession::IngressSession(TransportLayer* tl,
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
    LOGV2_DEBUG(7401101, 2, "Constructed a new gRPC ingress session", "session"_attr = toBSON());
}

IngressSession::~IngressSession() {
    end();
    LOGV2_DEBUG(7401402,
                2,
                "Finished cleaning up a gRPC ingress session",
                "session"_attr = toBSON(),
                "status"_attr = terminationStatus());
}

StatusWith<Message> IngressSession::_readFromStream() {
    if (auto maybeBuffer = _stream->read()) {
        networkCounter.hitPhysicalIn(NetworkCounter::ConnectionType::kIngress,
                                     MsgData::ConstView(maybeBuffer->get()).getLen());
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

Status IngressSession::_writeToStream(Message message) {
    if (_stream->write(message.sharedBuffer())) {
        networkCounter.hitPhysicalOut(NetworkCounter::ConnectionType::kIngress, message.size());
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

boost::optional<const ClientMetadata&> IngressSession::getClientMetadata() const {
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

void IngressSession::appendToBSON(BSONObjBuilder& bb) const {
    // No uint64_t BSON type
    bb.append("id", static_cast<long>(id()));
    bb.append("remoteClientId", remoteClientIdToString());
    bb.append("remote", remote().toString());
}


EgressSession::EgressSession(TransportLayer* tl,
                             const std::shared_ptr<GRPCReactor>& reactor,
                             std::shared_ptr<ClientContext> ctx,
                             std::shared_ptr<ClientStream> stream,
                             boost::optional<SSLConfiguration> sslConfig,
                             UUID channelId,
                             UUID clientId,
                             std::shared_ptr<EgressSession::SharedState> sharedState)
    : GRPCSession(tl, ctx->getRemote()),
      _reactor(reactor),
      _ctx(std::move(ctx)),
      _stream(std::move(stream)),
      _channelId(channelId),
      _clientId(clientId),
      _sharedState(std::move(sharedState)),
      _sslConfig(std::move(sslConfig)) {
    LOGV2_DEBUG(7401401, 2, "Constructed a new gRPC egress session", "session"_attr = toBSON());
}

EgressSession::~EgressSession() {
    end();

    auto status = terminationStatus();
    invariant(status.has_value());
    LOGV2_DEBUG(7401403,
                2,
                "Finished cleaning up a gRPC egress session",
                "session"_attr = toBSON(),
                "status"_attr = status);

    if (_cleanupCallback)
        (*_cleanupCallback)(*status);
}

Future<Message> EgressSession::_asyncReadFromStream() {
    std::unique_ptr<SharedBuffer> msg = std::make_unique<SharedBuffer>();
    auto pf = makePromiseFuture<void>();
    _stream->read(msg.get(), _reactor->_registerCompletionQueueEntry(std::move(pf.promise)));

    return std::move(pf.future)
        .onError([this](Status s) {
            // If _stream->read() fails, then the server
            // has no more messages to send and a final RPC
            // status should be available. Set the
            // termination status to that and return it
            // here.
            return asyncFinish().onCompletion([&](Status termStatus) {
                return termStatus.isOK()
                    ? Status(ErrorCodes::StreamTerminated,
                             "Could not read from gRPC client stream: remote done writing")
                    : termStatus;
            });
        })
        .then([this, msg = std::move(msg)]() {
            _updateWireVersion();
            networkCounter.hitPhysicalIn(NetworkCounter::ConnectionType::kEgress,
                                         MsgData::ConstView(msg->get()).getLen());
            return Message(std::move(*msg));
        });
}

Future<void> EgressSession::_asyncWriteToStream(Message message) {
    auto const msgLen = message.size();
    auto pf = makePromiseFuture<void>();
    _stream->write(message.sharedBuffer(),
                   _reactor->_registerCompletionQueueEntry(std::move(pf.promise)));

    return std::move(pf.future)
        .onError([this](Status s) {
            // If _stream->write() fails, then the RPC has been terminated and a final
            // RPC status should be available. Set the termination status to that and
            // return it here.
            return asyncFinish().onCompletion([&](Status termStatus) {
                return termStatus.isOK()
                    ? Status(ErrorCodes::StreamTerminated,
                             "Could not write to gRPC client stream: RPC terminated")
                    : termStatus;
            });
        })
        .then([msgLen]() {
            networkCounter.hitPhysicalOut(NetworkCounter::ConnectionType::kEgress, msgLen);
        });
}

void EgressSession::_cancelAsyncOperations() {
    cancel(Status(ErrorCodes::CallbackCanceled, "gRPC stream was canceled"));
}

Future<void> EgressSession::asyncFinish() {
    if (boost::optional<Status> status = _terminationStatus.synchronize(); status.has_value()) {
        return Future<void>::makeReady(status.get());
    }

    auto writesDonePF = makePromiseFuture<void>();
    _stream->writesDone(_reactor->_registerCompletionQueueEntry(std::move(writesDonePF.promise)));

    return std::move(writesDonePF.future).onCompletion([this](Status s) {
        if (!s.isOK()) {
            // This may fail if the underlying call has already been canceled.
            LOGV2_DEBUG(
                9936112, 3, "Failed to half-close the gRPC client stream", "error"_attr = s);
        }

        auto pf = makePromiseFuture<void>();
        auto finishStatus = std::make_unique<::grpc::Status>();
        _stream->finish(finishStatus.get(),
                        _reactor->_registerCompletionQueueEntry(std::move(pf.promise)));

        return std::move(pf.future)
            .onError([this](Status s) {
                _setTerminationStatus(s);
                return s.withContext("Failed to retrieve gRPC call's termination status");
            })
            .then([this, finishStatus = std::move(finishStatus)]() {
                Status finalStatus = util::convertStatus(*finishStatus);
                _setTerminationStatus(finalStatus);
                return finalStatus;
            });
    });
}

void EgressSession::appendToBSON(BSONObjBuilder& bb) const {
    // No uint64_t BSON type
    bb.append("id", static_cast<long>(id()));
    bb.append("clientId", _clientId.toString());
    bb.append("remote", remote().toString());
}

void EgressSession::_updateWireVersion() {
    // The cluster's wire version is communicated via the initial metadata, so only need to
    // check it after reading the first message.
    if (_checkedWireVersion.load() || _checkedWireVersion.swap(true)) {
        return;
    }

    auto metadata = _ctx->getServerInitialMetadata();

    auto log = [](auto error) {
        LOGV2_WARNING(7401602, "Failed to parse cluster's maxWireVersion", "error"_attr = error);
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

#ifdef MONGO_CONFIG_SSL
const SSLConfiguration* EgressSession::getSSLConfiguration() const {
    return _sslConfig ? &*_sslConfig : nullptr;
}
#endif

}  // namespace mongo::transport::grpc
