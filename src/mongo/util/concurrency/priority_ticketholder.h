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
#pragma once

#include <queue>

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/concurrency/ticket_broker.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/time_support.h"

namespace mongo {

class Ticket;
/**
 * A ticketholder implementation that centralises all ticket acquisition/releases.  Waiters will get
 * placed in a specific internal queue according to some logic.  Releasers will wake up a waiter
 * from a group chosen according to some logic.
 *
 * MODIFICATIONS TO THIS CLASS MUST BE ACCOMPANIED BY AN UPDATE OF
 * src/mongo/tla_plus/PriorityTicketHolder/MCPriorityTicketHolder.tla TO ENSURE IT IS CORRECT
 */
class PriorityTicketHolder : public TicketHolderWithQueueingStats {
public:
    explicit PriorityTicketHolder(int numTickets,
                                  int lowPriorityBypassThreshold,
                                  ServiceContext* serviceContext);
    ~PriorityTicketHolder() override{};

    int available() const override final {
        return _ticketsAvailable.load();
    };

    int queued() const override final {
        int result = 0;
        for (const auto& queue : _brokers) {
            result += queue.waitingThreadsRelaxed();
        }
        return result;
    }

    /**
     * Number of times low priority operations are expedited for ticket admission over normal
     * priority operations.
     */
    std::int64_t expedited() const {
        return _expeditedLowPriorityAdmissions.loadRelaxed();
    }

    /**
     * Returns the number of times the low priority queue is bypassed in favor of dequeuing from the
     * normal priority queue when a ticket becomes available.
     */
    std::int64_t bypassed() const {
        return _lowPriorityBypassCount.loadRelaxed();
    };

    void updateLowPriorityAdmissionBypassThreshold(const int& newBypassThreshold);

private:
    enum class QueueType : unsigned int { kLowPriority = 0, kNormalPriority = 1, NumQueues = 2 };

    boost::optional<Ticket> _tryAcquireImpl(AdmissionContext* admCtx) override final;

    boost::optional<Ticket> _waitForTicketUntilImpl(OperationContext* opCtx,
                                                    AdmissionContext* admCtx,
                                                    Date_t until,
                                                    WaitMode waitMode) override final;

    void _releaseToTicketPoolImpl(AdmissionContext* admCtx) noexcept override final;

    void _resize(OperationContext* opCtx, int newSize, int oldSize) noexcept override final;

    QueueStats& _getQueueStatsToUse(const AdmissionContext* admCtx) noexcept override final;

    void _appendImplStats(BSONObjBuilder& b) const override final;

    bool _tryAcquireTicket();

    TicketBroker::WaitingResult _attemptToAcquireTicket(TicketBroker& ticketBroker,
                                                        Date_t deadline,
                                                        Milliseconds maxWaitTime);

    /**
     * Wakes up a waiting thread (if it exists) and hands-over the current ticket.
     * Implementors MUST wake at least one waiting thread if at least one thread is waiting in any
     * of the brokers. In other words, attemptToTransferTicket on each non-empty TicketBroker must
     * be called until either it returns true at least once or has been called on all brokers.
     *
     * Care must be taken to ensure that only CPU-bound work is performed here and it doesn't block.
     * We risk stalling all other operations otherwise.
     *
     * When called the following invariants will be held:
     * - Successive checks to the number of waiting threads in a TicketBroker will always be <= the
     * previous value. That is, no new waiters can come in.
     * - Calling TicketBroker::attemptToTransferTicket will always return false if it has previously
     * returned false. Successive calls can change the result from true to false, but never the
     * reverse.
     */
    bool _dequeueWaitingThread(const stdx::unique_lock<stdx::mutex>& growthLock);

    static unsigned int _enumToInt(QueueType queueType) {
        return static_cast<unsigned int>(queueType);
    }

    TicketBroker& _getBroker(QueueType queueType) {
        return _brokers[_enumToInt(queueType)];
    }


    static QueueType _getQueueType(const AdmissionContext* admCtx) {
        auto priority = admCtx->getPriority();
        switch (priority) {
            case AdmissionContext::Priority::kLow:
                return QueueType::kLowPriority;
            case AdmissionContext::Priority::kNormal:
                return QueueType::kNormalPriority;
            default:
                MONGO_UNREACHABLE;
        }
    }

    // This mutex is meant to be used in order to grow the queue or to prevent it from doing so.
    //
    // We use an stdx::mutex here because we want to minimize overhead as much as possible. Using a
    // normal mongo::Mutex would add some unnecessary metrics counters. Additionally we need this
    // type as it is part of the TicketBroker API in order to avoid misuse.
    stdx::mutex _growthMutex;  // NOLINT

    std::array<TicketBroker, static_cast<unsigned int>(QueueType::NumQueues)> _brokers;
    std::array<QueueStats, static_cast<unsigned int>(QueueType::NumQueues)> _stats;

    /**
     * Limits the number times the low priority queue is non-empty and bypassed in favor of the
     * normal priority queue for the next ticket admission.
     *
     * Updates must be done under the _growthMutex.
     */
    int _lowPriorityBypassThreshold;

    /**
     * Counts the number of times normal operations are dequeued over operations queued in the low
     * priority queue. We explicitly use an unsigned type here because rollover is desired.
     */
    AtomicWord<std::uint64_t> _lowPriorityBypassCount{0};

    /**
     * Number of times ticket admission is expedited for low priority operations.
     */
    AtomicWord<std::int64_t> _expeditedLowPriorityAdmissions{0};
    AtomicWord<int> _ticketsAvailable;
    ServiceContext* _serviceContext;
};
}  // namespace mongo
