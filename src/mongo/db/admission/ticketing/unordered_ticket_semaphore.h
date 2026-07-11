// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/db/admission/ticketing/ticket_semaphore.h"
#include "mongo/platform/atomic.h"
#include "mongo/platform/waitable_atomic.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstdint>


namespace mongo {

/**
 * A Semaphore implementation where waiters compete for permits using a futex-based wait on an
 * atomic permit counter.
 *
 * On each release, one sleeping waiter is woken to compete again against concurrent tryAcquire()
 * callers. There is no fairness guarantee in this implementation; a newly arrived operation can
 * claim a permit ahead of existing waiters.
 */
class UnorderedTicketSemaphore : public TicketSemaphore {
public:
    UnorderedTicketSemaphore(int numPermits, int maxWaiters)
        : _permits(numPermits), _maxWaiters(maxWaiters) {}

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
    BasicWaitableAtomic<int> _permits;
    Atomic<int> _waiters{0};
    Atomic<int> _maxWaiters;
};

}  // namespace mongo
