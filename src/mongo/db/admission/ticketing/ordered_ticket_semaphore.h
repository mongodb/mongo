// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/db/admission/ticketing/ticket_semaphore.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <set>

namespace mongo {

/**
 * A Semaphore implementation where waiters are dispatched depending on the number of admissions.
 *
 * On each release, the waiter with less admissions is woken to take a ticket. This class guarantees
 * order of execution based on number of admissions.
 */
class OrderedTicketSemaphore : public TicketSemaphore {
public:
    OrderedTicketSemaphore(int numTickets, int maxQueueDepth)
        : _permits(numTickets), _maxWaiters(maxQueueDepth) {}

    bool tryAcquire() override;

    bool acquire(OperationContext* opCtx,
                 AdmissionContext* admCtx,
                 Date_t until,
                 bool interruptible) override;

    void release() override;

    void resize(int delta) override;

    int available() const override;

    void setMaxWaiters(int waiters) override;

    int waiters() const override;

private:
    struct Waiter {
        int admissions;
        stdx::condition_variable cv;
        bool awake{false};
        bool permitted{false};
    };

    struct WaiterCompare {
        bool operator()(const std::shared_ptr<Waiter>& a, const std::shared_ptr<Waiter>& b) const {
            // Lower admission count = higher priority (should be at top of queue).
            return a->admissions < b->admissions;
        }
    };

    void _wakeUpWaiters(WithLock, int count);
    bool _tryAcquireWithoutQueuing(WithLock);
    mutable std::mutex _mutex;
    Atomic<int> _permits;
    Atomic<int> _maxWaiters;
    // Only used to prevent contention with observability threads. In other places to ensure
    // correctness we check the state of _waitQueue directly.
    Atomic<int> _numWaiters;

    std::multiset<std::shared_ptr<Waiter>, WaiterCompare> _waitQueue;
};

}  // namespace mongo
