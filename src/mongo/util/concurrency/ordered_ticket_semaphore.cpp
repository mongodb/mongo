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

#include "mongo/util/concurrency/ordered_ticket_semaphore.h"

#include "mongo/db/operation_context.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

bool OrderedTicketSemaphore::tryAcquire() {
    stdx::lock_guard lock(_mutex);
    return _tryAcquireWithoutQueuing(lock);
}

bool OrderedTicketSemaphore::acquire(OperationContext* opCtx,
                                     AdmissionContext* admCtx,
                                     Date_t until,
                                     bool interruptible) {
    stdx::unique_lock lk(_mutex);

    if (_tryAcquireWithoutQueuing(lk)) {
        return true;
    }

    if (admCtx->getLowAdmissions() == 0 &&
        static_cast<int>(_waitQueue.size()) >= _maxWaiters.loadRelaxed() &&
        !admCtx->isLoadShedExempt()) {
        admCtx->recordOperationLoadShed();
        lk.unlock();
        uasserted(ErrorCodes::AdmissionQueueOverflow,
                  "MongoDB is overloaded and cannot accept new operations. Try again later.");
    }

    auto waiter = std::make_shared<Waiter>();
    waiter->admissions = admCtx->getAdmissions();
    auto it = _waitQueue.insert(waiter);
    _numWaiters.fetchAndAddRelaxed(1);
    ON_BLOCK_EXIT([&] {
        _waitQueue.erase(it);
        _numWaiters.fetchAndSubtractRelaxed(1);
        // If the thread was awakened but didn't get any permits (which means there are available
        // permits) wake another one.
        if (!waiter->permitted && waiter->awake) {
            _wakeUpWaiters(lk, 1);
        }
    });

    bool predicateBool = true;

    if (interruptible) {
        predicateBool = opCtx->waitForConditionOrInterruptUntil(
            waiter->cv, lk, until, [&] { return waiter->awake; });

        opCtx->checkForInterrupt();
    } else if (until == Date_t::max()) {
        waiter->cv.wait(lk, [&] { return waiter->awake; });
    } else {
        predicateBool =
            waiter->cv.wait_until(lk, until.toSystemTimePoint(), [&] { return waiter->awake; });
    }

    if (!predicateBool) {
        return false;
    }

    _permits.fetchAndSubtractRelaxed(1);
    waiter->permitted = true;
    return true;
}

void OrderedTicketSemaphore::release() {
    stdx::lock_guard lock(_mutex);

    int available = _permits.addAndFetch(1);

    if (available > 0 && !_waitQueue.empty()) {
        _wakeUpWaiters(lock, 1);
    }
}

void OrderedTicketSemaphore::resize(int delta) {
    stdx::lock_guard lock(_mutex);

    int available = _permits.addAndFetch(delta);

    if (available > 0 && !_waitQueue.empty()) {
        int numToWake = std::min(static_cast<int>(_waitQueue.size()), available);

        _wakeUpWaiters(lock, numToWake);
    }
}

void OrderedTicketSemaphore::setMaxWaiters(int size) {
    _maxWaiters.store(size);
}

int OrderedTicketSemaphore::available() const {
    return _permits.loadRelaxed();
}

int OrderedTicketSemaphore::waiters() const {
    return _numWaiters.loadRelaxed();
}

void OrderedTicketSemaphore::_wakeUpWaiters(WithLock, int count) {
    int i = 0;
    for (auto& waiter : _waitQueue) {
        if (!waiter->awake) {
            waiter->awake = true;
            waiter->cv.notify_one();
            i++;
        }
        if (i == count) {
            break;
        }
    }
}

bool OrderedTicketSemaphore::_tryAcquireWithoutQueuing(WithLock) {
    // We can take a ticket immediately if there's a surplus after taking into account all queued
    // operations.
    auto permits = _permits.load();
    if (permits > 0 && static_cast<size_t>(permits) > _waitQueue.size()) {
        _permits.fetchAndSubtractRelaxed(1);
        return true;
    }
    return false;
}

}  // namespace mongo
