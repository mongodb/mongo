// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstdint>


namespace mongo {

/**
 * Controls concurrent access to a finite pool of permits.
 *
 * Callers acquire a permit before proceeding with a unit of work, and release it when done. If no
 * permits are available, callers either fail immediately (tryAcquire) or block until one becomes
 * available (acquire).
 */
class TicketSemaphore {
public:
    virtual ~TicketSemaphore() = default;

    /**
     * Non-blocking acquire. Returns true if a permit was consumed.
     */
    virtual bool tryAcquire() = 0;

    /**
     * Blocks until a permit is acquired, 'until' expires, or the operation is interrupted.
     * Returns true if acquired.
     *
     * Throws 'AdmissionQueueOverflow' if the wait queue is full.
     */
    virtual bool acquire(OperationContext* opCtx,
                         AdmissionContext* admCtx,
                         Date_t until,
                         bool interruptible) = 0;

    /**
     * Returns one permit to the sempahore, waking a queued waiter.
     */
    virtual void release() = 0;

    /**
     * Adjusts the number of total permit count by 'delta'.
     */
    virtual void resize(int delta) = 0;

    /**
     * Returns the instantaneous number of un-acquired permits (not checked out by an operation).
     */
    virtual int available() const = 0;

    /**
     * Adjusts the maximum number of threads waiting.
     */
    virtual void setMaxWaiters(int waiters) = 0;

    /**
     * Returns the instantaneous number of threads blocked in 'acquire'.
     */
    virtual int waiters() const = 0;
};

}  // namespace mongo
