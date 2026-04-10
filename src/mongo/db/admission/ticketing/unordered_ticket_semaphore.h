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
#include "mongo/db/admission/ticketing/ticket_semaphore.h"
#include "mongo/platform/atomic_word.h"
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
