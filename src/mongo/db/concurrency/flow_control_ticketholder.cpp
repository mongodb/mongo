/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/concurrency/flow_control_ticketholder.h"

#include "mongo/db/operation_context.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {
const auto getFlowControlTicketholder =
    ServiceContext::declareDecoration<std::unique_ptr<FlowControlTicketholder>>();
}  // namespace

void FlowControlTicketholder::CurOp::writeToBuilder(BSONObjBuilder& infoBuilder) {
    infoBuilder.append("waitingForFlowControl", waiting);
    BSONObjBuilder flowControl(infoBuilder.subobjStart("flowControlStats"));
    if (ticketsAcquired > 0) {
        flowControl.append("acquireCount", ticketsAcquired);
    }

    if (acquireWaitCount) {
        flowControl.append("acquireWaitCount", acquireWaitCount);
    }

    if (timeAcquiringMicros) {
        flowControl.append("timeAcquiringMicros", timeAcquiringMicros);
    }
    flowControl.done();
}

FlowControlTicketholder* FlowControlTicketholder::get(ServiceContext* service) {
    return getFlowControlTicketholder(service).get();
}

FlowControlTicketholder* FlowControlTicketholder::get(ServiceContext& service) {
    return getFlowControlTicketholder(service).get();
}

FlowControlTicketholder* FlowControlTicketholder::get(OperationContext* opCtx) {
    return get(opCtx->getClient()->getServiceContext());
}

void FlowControlTicketholder::set(ServiceContext* service,
                                  std::unique_ptr<FlowControlTicketholder> flowControl) {
    auto& globalFlow = getFlowControlTicketholder(service);
    globalFlow = std::move(flowControl);
}

void FlowControlTicketholder::refreshTo(int numTickets) {
    invariant(numTickets >= 0);
    stdx::lock_guard<Latch> lk(_mutex);
    LOG(4) << "Refreshing tickets. Before: " << _tickets << " Now: " << numTickets;
    _tickets = numTickets;
    _cv.notify_all();
}

void FlowControlTicketholder::getTicket(OperationContext* opCtx,
                                        FlowControlTicketholder::CurOp* stats) {
    stdx::unique_lock<Latch> lk(_mutex);
    if (_inShutdown) {
        return;
    }

    LOG(4) << "Taking ticket. Available: " << _tickets;
    if (_tickets == 0) {
        ++stats->acquireWaitCount;
    }

    auto currentWaitTime = curTimeMicros64();
    auto updateTotalTime = [&]() {
        auto oldWaitTime = std::exchange(currentWaitTime, curTimeMicros64());
        _totalTimeAcquiringMicros.fetchAndAddRelaxed(currentWaitTime - oldWaitTime);
    };

    stats->waiting = true;
    ON_BLOCK_EXIT([&] {
        // When this block exits, update the time one last time and note that getTicket() is no
        // longer waiting.
        updateTotalTime();
        stats->waiting = false;
    });

    // getTicket() should block until there are tickets or the Ticketholder is in shutdown
    while (!opCtx->waitForConditionOrInterruptFor(
        _cv, lk, Milliseconds(500), [&] { return _tickets > 0 || _inShutdown; })) {
        updateTotalTime();
    }

    if (_inShutdown) {
        return;
    }

    ++stats->ticketsAcquired;
    --_tickets;
}

// Should only be called once, during shutdown.
void FlowControlTicketholder::setInShutdown() {
    LOG(0) << "Stopping further Flow Control ticket acquisitions.";
    stdx::lock_guard<Latch> lk(_mutex);
    _inShutdown = true;
    _cv.notify_all();
}

}  // namespace mongo
