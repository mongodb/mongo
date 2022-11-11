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
#include "mongo/util/concurrency/ticket_queues.h"

#include <iostream>

#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace ticket_queues {
bool Queue::enqueue(UniqueLockGuard& uniqueQueueLock,
                    OperationContext* opCtx,
                    const Date_t& until,
                    bool interruptible) {
    _queuedThreads++;
    // Before exiting we remove ourselves from the count of queued threads, we are still holding the
    // lock here so this is safe.
    ON_BLOCK_EXIT([&] { _queuedThreads--; });

    // TODO SERVER-69179: Replace the custom version of waiting on a condition variable with what
    // comes out of SERVER-69178.
    auto clockSource = opCtx->getServiceContext()->getPreciseClockSource();
    auto baton = interruptible ? opCtx->getBaton().get() : nullptr;
    auto deadline = interruptible ? std::min(until, opCtx->getDeadline()) : Date_t::max();

    ON_BLOCK_EXIT([&] { _signalThreadWoken(uniqueQueueLock); });
    auto waitResult = clockSource->waitForConditionUntil(_cv, uniqueQueueLock, deadline, baton);

    // We check if the operation has been interrupted (timeout, killed, etc.) here.
    if (interruptible) {
        opCtx->checkForInterrupt();
    }

    if (waitResult == stdx::cv_status::timeout) {
        return false;
    }

    return true;
}

bool Queue::attemptToDequeue(const SharedLockGuard& sharedQueueLock) {
    auto threadsToBeWoken = _threadsToBeWoken.load();
    while (threadsToBeWoken < _queuedThreads) {
        auto canDequeue = _threadsToBeWoken.compareAndSwap(&threadsToBeWoken, threadsToBeWoken + 1);
        if (canDequeue) {
            _cv.notify_one();
            return true;
        }
    }
    return false;
}

void Queue::_signalThreadWoken(const UniqueLockGuard& uniqueQueueLock) {
    auto currentThreadsToBeWoken = _threadsToBeWoken.load();
    while (currentThreadsToBeWoken > 0) {
        if (_threadsToBeWoken.compareAndSwap(&currentThreadsToBeWoken,
                                             currentThreadsToBeWoken - 1)) {
            return;
        }
    }
}

}  // namespace ticket_queues
}  // namespace mongo
