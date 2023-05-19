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

#include "mongo/transport/grpc/mock_client_stream.h"

#include "mongo/transport/grpc/mock_util.h"
#include "mongo/util/interruptible.h"

namespace mongo::transport::grpc {

MockClientStream::MockClientStream(HostAndPort remote,
                                   Future<MetadataContainer>&& initialMetadataFuture,
                                   Future<::grpc::Status>&& rpcReturnStatus,
                                   std::shared_ptr<MockCancellationState> rpcCancellationState,
                                   BidirectionalPipe::End&& pipe)
    : _remote{std::move(remote)},
      _serverInitialMetadata{std::move(initialMetadataFuture)},
      _rpcReturnStatus{std::move(rpcReturnStatus)},
      _rpcCancellationState(std::move(rpcCancellationState)),
      _pipe{std::move(pipe)} {}

boost::optional<SharedBuffer> MockClientStream::read() {
    // Even if the server side handler of this stream has set a final status for the RPC (i.e.
    // _rpcReturnStatus is ready), there may still be unread messages in the queue that the server
    // sent before setting that status, so only return early here if the RPC was cancelled.
    // Otherwise, try to read whatever messages are in the queue.
    if (_rpcCancellationState->isCancelled()) {
        return boost::none;
    }

    return runWithDeadline<boost::optional<SharedBuffer>>(
        _rpcCancellationState->getDeadline(), [&](Interruptible* i) { return _pipe.read(i); });
}

bool MockClientStream::write(ConstSharedBuffer msg) {
    if (_rpcCancellationState->isCancelled() || _rpcReturnStatus.isReady()) {
        return false;
    }

    return runWithDeadline<bool>(_rpcCancellationState->getDeadline(),
                                 [&](Interruptible* i) { return _pipe.write(msg, i); });
}

::grpc::Status MockClientStream::finish() {
    // We use a busy wait here because there is no easy way to wait until all the messages in the
    // pipe have been read.
    while (!_pipe.isConsumed() && !_rpcCancellationState->isDeadlineExceeded()) {
        sleepFor(Milliseconds(1));
    }

    invariant(_rpcReturnStatus.isReady() || _rpcCancellationState->isCancelled());

    if (auto cancellationStatus = _rpcCancellationState->getCancellationStatus();
        cancellationStatus.has_value()) {
        return *cancellationStatus;
    }

    return _rpcReturnStatus.get();
}

void MockClientStream::_cancel() {
    _rpcCancellationState->cancel(::grpc::Status::CANCELLED);
    _pipe.close();
}

}  // namespace mongo::transport::grpc
