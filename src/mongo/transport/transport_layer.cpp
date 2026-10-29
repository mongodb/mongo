// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/transport_layer.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/future_util.h"

#include <cstdint>

namespace mongo {
namespace transport {

namespace {

Atomic<uint64_t> reactorTimerIdCounter(0);

}  // namespace

const Status TransportLayer::SessionUnknownStatus =
    Status(ErrorCodes::TransportSessionUnknown, "TransportLayer does not own the Session.");

const Status TransportLayer::ShutdownStatus =
    Status(ErrorCodes::ShutdownInProgress, "TransportLayer is in shutdown.");

const Status TransportLayer::TicketSessionUnknownStatus = Status(
    ErrorCodes::TransportSessionUnknown, "TransportLayer does not own the Ticket's Session.");

const Status TransportLayer::TicketSessionClosedStatus = Status(
    ErrorCodes::TransportSessionClosed, "Operation attempted on a closed transport Session.");

ReactorTimer::ReactorTimer() : _id(reactorTimerIdCounter.addAndFetch(1)) {}

thread_local Reactor* Reactor::_reactorForThread = nullptr;

ExecutorFuture<void> Reactor::sleepFor(Milliseconds duration, const CancellationToken& token) {
    auto when = now() + duration;

    if (token.isCanceled()) {
        return ExecutorFuture<void>(
            shared_from_this(), Status(ErrorCodes::CallbackCanceled, "Cancelled reactor sleep"));
    }

    if (when <= now()) {
        return ExecutorFuture<void>(shared_from_this());
    }

    std::unique_ptr<ReactorTimer> timer = makeTimer();
    return future_util::withCancellation(timer->waitUntil(when), token)
        .thenRunOn(shared_from_this())
        .onCompletion([t = std::move(timer)](const Status& s) { return s; });
}

}  // namespace transport
}  // namespace mongo
