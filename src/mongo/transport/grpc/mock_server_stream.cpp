// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/grpc/mock_server_stream.h"

#include "mongo/db/service_context.h"
#include "mongo/transport/grpc/mock_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/interruptible.h"

#include <string_view>

namespace mongo::transport::grpc {

MockServerStream::MockServerStream(HostAndPort remote,
                                   Promise<MetadataContainer>&& initialMetadataPromise,
                                   Promise<::grpc::Status>&& rpcTerminationStatusPromise,
                                   std::shared_ptr<MockCancellationState> rpcCancellationState,
                                   BidirectionalPipe::End&& serverPipeEnd,
                                   MetadataContainer clientMetadata)
    : _remote(std::move(remote)),
      _initialMetadata(std::move(initialMetadataPromise)),
      _rpcReturnStatus(std::move(rpcTerminationStatusPromise)),
      _finalStatusReturned(false),
      _rpcCancellationState(std::move(rpcCancellationState)),
      _pipe{std::move(serverPipeEnd)},
      _clientMetadata{std::move(clientMetadata)} {
    // Hold onto the MetadataContainer, but also write out a view for use by the server context API.
    for (auto& kvp : _clientMetadata) {
        _clientMetadataView.insert({std::string_view{kvp.first.data(), kvp.first.length()},
                                    std::string_view{kvp.second.data(), kvp.second.length()}});
    }
}

boost::optional<SharedBuffer> MockServerStream::read() {
    invariant(!*_finalStatusReturned);

    return runWithDeadline<boost::optional<SharedBuffer>>(
        _rpcCancellationState->getDeadline(), [&](Interruptible* i) { return _pipe.read(i); });
}

bool MockServerStream::isCancelled() const {
    return _rpcCancellationState->isCancelled();
}

bool MockServerStream::write(ConstSharedBuffer msg) {
    invariant(!*_finalStatusReturned);
    if (isCancelled()) {
        return false;
    }

    _initialMetadata.trySend();
    return runWithDeadline<bool>(_rpcCancellationState->getDeadline(),
                                 [&](Interruptible* i) { return _pipe.write(msg, i); });
}

void MockServerStream::sendReturnStatus(::grpc::Status status) {
    {
        auto finalStatusReturned = _finalStatusReturned.synchronize();
        invariant(!*finalStatusReturned);
        *finalStatusReturned = true;
        // Client side ignores the mocked return value in the event of a cancellation, so don't need
        // to check if stream has been cancelled before sending the status.
    }
    _rpcReturnStatus.emplaceValue(std::move(status));
    _pipe.close();
}

void MockServerStream::cancel(::grpc::Status status) {
    // Only mark the RPC as cancelled if a status hasn't already been returned to client.
    auto finalStatusReturned = _finalStatusReturned.synchronize();
    if (*finalStatusReturned) {
        return;
    }
    // Need to update the cancellation state before closing the pipe so that when a stream
    // read/write is interrupted, the cancellation state will already be up to date.
    _rpcCancellationState->cancel(std::move(status));
    _pipe.close();
}

}  // namespace mongo::transport::grpc
