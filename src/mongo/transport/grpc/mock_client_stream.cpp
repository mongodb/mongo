// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/grpc/mock_client_stream.h"

#include "mongo/transport/grpc/mock_util.h"
#include "mongo/util/interruptible.h"

namespace mongo::transport::grpc {

MONGO_FAIL_POINT_DEFINE(grpcHangOnStreamEstablishment);
MONGO_FAIL_POINT_DEFINE(grpcFailStreamEstablishment);

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

void MockClientStream::startCall(GRPCReactor::CompletionQueueEntry* tag) {
    if (MONGO_unlikely(grpcHangOnStreamEstablishment.shouldFail())) {
        _rpcCancellationState->onCancel().thenRunOn(_reactor).getAsync(
            [reactor = _reactor, tag](Status s) {
                reactor->_processCompletionQueueNotification(tag, false /* ok */);
            });

        return;
    } else if (MONGO_unlikely(grpcFailStreamEstablishment.shouldFail())) {
        _cancel();
        _reactor->_processCompletionQueueNotification(tag, false /* ok */);
        return;
    }
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
