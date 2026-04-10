/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
