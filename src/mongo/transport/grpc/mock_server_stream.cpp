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

#include "mongo/transport/grpc/mock_server_stream.h"

#include "mongo/db/service_context.h"
#include "mongo/transport/grpc/mock_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/interruptible.h"

namespace mongo::transport::grpc {

MockServerStream::MockServerStream(HostAndPort remote,
                                   Promise<MetadataContainer>&& initialMetadataPromise,
                                   Promise<::grpc::Status>&& rpcTerminationStatusPromise,
                                   std::shared_ptr<MockCancellationState> rpcCancellationState,
                                   BidirectionalPipe::End&& serverPipeEnd,
                                   MetadataView clientMetadata)
    : _remote(std::move(remote)),
      _initialMetadata(std::move(initialMetadataPromise)),
      _rpcReturnStatus(std::move(rpcTerminationStatusPromise)),
      _finalStatusReturned(false),
      _rpcCancellationState(std::move(rpcCancellationState)),
      _pipe{std::move(serverPipeEnd)},
      _clientMetadata{std::move(clientMetadata)} {}

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
