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
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <memory>
#include <mutex>

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
