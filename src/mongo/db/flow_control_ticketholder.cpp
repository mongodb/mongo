// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/flow_control_ticketholder.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

#include <mutex>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


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
    std::lock_guard<std::mutex> lk(_mutex);
    LOGV2_DEBUG(20518,
                4,
                "Refreshing tickets. Before: {tickets} Now: {numTickets}",
                "tickets"_attr = _tickets,
                "numTickets"_attr = numTickets);
    _tickets = numTickets;
    _cv.notify_all();
}

void FlowControlTicketholder::getTicket(OperationContext* opCtx,
                                        FlowControlTicketholder::CurOp* stats) {
    std::unique_lock<std::mutex> lk(_mutex);
    if (_inShutdown) {
        return;
    }

    LOGV2_DEBUG(20519, 4, "Taking ticket.", "Available"_attr = _tickets);
    if (_tickets == 0) {
        ++stats->acquireWaitCount;

        // Since tickets are only added every second, the fast clock source is good enough.
        // We record the time in micros anyway to be consistent with other metrics like mutexes.
        auto& clockSource = opCtx->fastClockSource();
        auto currentWaitTime = clockSource.now();
        auto updateTotalTime = [&]() {
            auto oldWaitTime = std::exchange(currentWaitTime, clockSource.now());
            auto waitTimeDelta = currentWaitTime - oldWaitTime;
            auto waitTimeDeltaMicros = durationCount<Microseconds>(waitTimeDelta);
            _totalTimeAcquiringMicros.fetchAndAddRelaxed(waitTimeDeltaMicros);
            stats->timeAcquiringMicros += waitTimeDeltaMicros;
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
    }

    ++stats->ticketsAcquired;
    --_tickets;
}

// Should only be called once, during shutdown.
void FlowControlTicketholder::setInShutdown() {
    LOGV2(20520, "Stopping further Flow Control ticket acquisitions.");
    std::lock_guard<std::mutex> lk(_mutex);
    _inShutdown = true;
    _cv.notify_all();
}

}  // namespace mongo
