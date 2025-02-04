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
                                   BidirectionalPipe::End&& pipe,
                                   const std::shared_ptr<GRPCReactor>& reactor)
    : _remote{std::move(remote)},
      _serverInitialMetadata{std::move(initialMetadataFuture)},
      _rpcReturnStatus{std::move(rpcReturnStatus)},
      _rpcCancellationState(std::move(rpcCancellationState)),
      _pipe{std::move(pipe)},
      _reactor(reactor) {}

void MockClientStream::read(SharedBuffer* msg, GRPCReactor::CompletionQueueEntry* tag) {
    // Even if the server side handler of this stream has set a final status for the RPC (i.e.
    // _rpcReturnStatus is ready), there may still be unread messages in the queue that the server
    // sent before setting that status, so only return early here if the RPC was cancelled.
    // Otherwise, try to read whatever messages are in the queue.
    if (_rpcCancellationState->isCancelled()) {
        _reactor->_processCompletionQueueNotification(tag, false /* ok */);
        return;
    }

    auto res = runWithDeadline<boost::optional<SharedBuffer>>(
        _rpcCancellationState->getDeadline(), [&](Interruptible* i) { return _pipe.read(i); });

    if (res.has_value()) {
        *msg = res.get();
        _reactor->_processCompletionQueueNotification(tag, true /* ok */);
    } else {
        _reactor->_processCompletionQueueNotification(tag, false /* ok */);
    }
}

void MockClientStream::write(ConstSharedBuffer msg, GRPCReactor::CompletionQueueEntry* tag) {
    if (_rpcCancellationState->isCancelled() || _rpcReturnStatus.isReady()) {
        _reactor->_processCompletionQueueNotification(tag, false /* ok */);
        return;
    }

    auto res = runWithDeadline<bool>(_rpcCancellationState->getDeadline(),
                                     [&](Interruptible* i) { return _pipe.write(msg, i); });
    if (res) {
        _reactor->_processCompletionQueueNotification(tag, true /* ok */);
    } else {
        _reactor->_processCompletionQueueNotification(tag, false /* ok */);
    }
}

void MockClientStream::finish(::grpc::Status* status, GRPCReactor::CompletionQueueEntry* tag) {
    _pipe.closeWriting();

    // We use a busy wait here because there is no easy way to wait until all the messages in the
    // pipe have been read.
    while (!_pipe.isConsumed() && !_rpcCancellationState->isDeadlineExceeded()) {
        sleepFor(Milliseconds(1));
    }

    invariant(_rpcReturnStatus.isReady() || _rpcCancellationState->isCancelled());

    if (auto cancellationStatus = _rpcCancellationState->getCancellationStatus();
        cancellationStatus.has_value()) {
        *status = *cancellationStatus;
        _reactor->_processCompletionQueueNotification(tag, true /* ok */);
        return;
    }

    *status = _rpcReturnStatus.get();
    _reactor->_processCompletionQueueNotification(tag, true /* ok */);
}

void MockClientStream::writesDone(GRPCReactor::CompletionQueueEntry* tag) {
    _reactor->_processCompletionQueueNotification(tag, true /* ok */);
}

boost::optional<SharedBuffer> MockClientStream::syncRead(
    const std::shared_ptr<GRPCReactor>& reactor) {
    auto pf = makePromiseFuture<void>();
    std::unique_ptr<SharedBuffer> msg = std::make_unique<SharedBuffer>();

    read(msg.get(), reactor->_registerCompletionQueueEntry(std::move(pf.promise)));
    auto res = std::move(pf.future).getNoThrow();
    if (res.isOK()) {
        return *msg.get();
    } else {
        return boost::none;
    }
}

bool MockClientStream::syncWrite(const std::shared_ptr<GRPCReactor>& reactor,
                                 ConstSharedBuffer msg) {
    auto pf = makePromiseFuture<void>();
    write(msg, reactor->_registerCompletionQueueEntry(std::move(pf.promise)));

    return std::move(pf.future).getNoThrow().isOK();
}

::grpc::Status MockClientStream::syncFinish(const std::shared_ptr<GRPCReactor>& reactor) {
    auto pf = makePromiseFuture<void>();
    auto finishStatus = std::make_unique<::grpc::Status>();

    finish(finishStatus.get(), reactor->_registerCompletionQueueEntry(std::move(pf.promise)));

    return std::move(pf.future)
        .then([finishStatus = std::move(finishStatus)]() { return *finishStatus; })
        .get();
}

void MockClientStream::_cancel() {
    _rpcCancellationState->cancel(::grpc::Status::CANCELLED);
    _pipe.close();
}

}  // namespace mongo::transport::grpc
