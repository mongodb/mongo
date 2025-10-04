/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/transport/transport_layer.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/future_util.h"

#include <cstdint>

namespace mongo {
namespace transport {

namespace {

AtomicWord<uint64_t> reactorTimerIdCounter(0);

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

Date_t Reactor::ReactorClockSource::now() {
    return _reactor->now();
}

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
