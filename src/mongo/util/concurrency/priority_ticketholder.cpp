/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/service_context.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/priority_ticketholder.h"

#include <iostream>

#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {

PriorityTicketHolder::PriorityTicketHolder(int32_t numTickets,
                                           int32_t lowPriorityBypassThreshold,
                                           ServiceContext* serviceContext)
    : TicketHolder(numTickets, serviceContext),
      _serviceContext(serviceContext),
      _pool(numTickets, lowPriorityBypassThreshold) {}

int32_t PriorityTicketHolder::available() const {
    return _pool.available();
}

int64_t PriorityTicketHolder::queued() const {
    return _pool.queued();
}

int64_t PriorityTicketHolder::numFinishedProcessing() const {
    return _stats[_enumToInt(QueueType::kLowPriority)].totalFinishedProcessing.load() +
        _stats[_enumToInt(QueueType::kNormalPriority)].totalFinishedProcessing.load();
}

int64_t PriorityTicketHolder::expedited() const {
    return _pool.getQueue().expedited();
}

int64_t PriorityTicketHolder::bypassed() const {
    return _pool.getQueue().bypassed();
}

void PriorityTicketHolder::updateLowPriorityAdmissionBypassThreshold(int32_t newBypassThreshold) {
    auto& queue = _pool.getQueue();
    queue.updateLowPriorityAdmissionBypassThreshold(newBypassThreshold);
}

boost::optional<Ticket> PriorityTicketHolder::_tryAcquireImpl(AdmissionContext* admCtx) {
    invariant(admCtx);

    if (_pool.tryAcquire()) {
        return Ticket(this, admCtx);
    }

    return boost::none;
}

boost::optional<Ticket> PriorityTicketHolder::_waitForTicketUntilImpl(OperationContext* opCtx,
                                                                      AdmissionContext* admCtx,
                                                                      Date_t until) {
    invariant(admCtx);

    while (true) {
        // To support interruptibility of ticket acquisition, we attempt to acquire a ticket for a
        // maximum period of time. This may or may not succeed, in which case we will retry until
        // timing out or getting interrupted.
        auto maxUntil = std::min(until, Date_t::now() + Milliseconds(500));
        auto acquired = _pool.acquire(admCtx, maxUntil);
        ScopeGuard rereleaseIfTimedOutOrInterrupted([&] {
            // We may have gotten a ticket that we can't use, release it back to the ticket pool.
            if (acquired) {
                _pool.release();
            }
        });

        if (opCtx) {
            opCtx->checkForInterrupt();
        }

        if (acquired) {
            rereleaseIfTimedOutOrInterrupted.dismiss();
            return Ticket(this, admCtx);
        } else if (maxUntil == until) {
            // We hit the end of our deadline, so return nothing.
            return boost::none;
        }
        // We hit the periodic deadline, but are still within the caller's deadline, so retry.
    }

    return boost::none;
}

void PriorityTicketHolder::_releaseToTicketPoolImpl(AdmissionContext* admCtx) noexcept {
    // 'Immediate' priority operations should bypass the ticketing system completely.
    invariant(admCtx && admCtx->getPriority() != AdmissionContext::Priority::kImmediate);
    _pool.release();
}

void PriorityTicketHolder::_resize(int32_t newSize, int32_t oldSize) noexcept {
    auto difference = newSize - oldSize;

    if (difference > 0) {
        // Hand out tickets one-by-one until we've given them all out.
        for (auto remaining = difference; remaining > 0; remaining--) {
            _pool.release();
        }
    } else {
        AdmissionContext admCtx;
        // Take tickets one-by-one without releasing.
        for (auto remaining = -difference; remaining > 0; remaining--) {
            _pool.acquire(&admCtx, Date_t::max());
        }
    }
}

TicketHolder::QueueStats& PriorityTicketHolder::_getQueueStatsToUse(
    const AdmissionContext* admCtx) noexcept {
    auto queueType = _getQueueType(admCtx);
    return _stats[_enumToInt(queueType)];
}

void PriorityTicketHolder::_appendImplStats(BSONObjBuilder& b) const {
    {
        BSONObjBuilder bbb(b.subobjStart("lowPriority"));
        const auto& lowPriorityTicketStats = _stats[_enumToInt(QueueType::kLowPriority)];
        _appendCommonQueueImplStats(bbb, lowPriorityTicketStats);
        bbb.append("expedited", expedited());
        bbb.append("bypassed", bypassed());
        bbb.done();
    }
    {
        BSONObjBuilder bbb(b.subobjStart("normalPriority"));
        const auto& normalPriorityTicketStats = _stats[_enumToInt(QueueType::kNormalPriority)];
        _appendCommonQueueImplStats(bbb, normalPriorityTicketStats);
        bbb.done();
    }
}

}  // namespace mongo
