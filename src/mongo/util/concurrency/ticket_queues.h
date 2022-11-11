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

#include "mongo/db/operation_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/hierarchical_acquisition.h"

namespace mongo {

namespace ticket_queues {

/**
 * Using a shared_mutex is fine here because usual considerations for avoiding them do not apply in
 * this case:
 *      (1) Operations are short and do not block while holding the lock (i.e. they only do
 *      CPU-bound work)
 *      (2) Writer starvation is not possible as there are a finite number of operations to be
 *      performed in the reader case. Once all tickets get released no other thread can take the
 *      shared lock.
 *
 * The alternative of using ResourceMutex is not appropriate as the class serves as a concurrency
 * primitive and is performance sensitive.
 *
 */
using QueueMutex = std::shared_mutex;                  // NOLINT
using SharedLockGuard = std::shared_lock<QueueMutex>;  // NOLINT
using UniqueLockGuard = std::unique_lock<QueueMutex>;  // NOLINT

class Queue {
public:
    /**
     * Returns true if the thread waits until woken without the deadline expiring or
     * interruption. If the deadline expires, returns false. Throws if interrupted via the
     * OperationContext.
     */
    bool enqueue(UniqueLockGuard& uniqueQueueLock,
                 OperationContext* opCtx,
                 const Date_t& until,
                 bool interruptible);

    /**
     * Returns true if there is a thread to wake in the queue, false otherwise.
     */
    bool attemptToDequeue(const SharedLockGuard& sharedQueueLock);

    /**
     * Returns the number of threads queued.
     */
    int queuedElems() {
        return _queuedThreads;
    }

    /**
     * Returns the number of threads that are ready to be woken from the queue.
     */
    int getThreadsPendingToWake() const {
        return _threadsToBeWoken.load();
    }

private:
    /**
     * Signals when a thread is woken.
     */
    void _signalThreadWoken(const UniqueLockGuard& uniqueQueueLock);

    /**
     * Threads queued.
     *
     * IMPORTANT: Must be protected by the UniqueLockGuard when modified.
     *
     * Guaranteed to not change when holding the SharedLockGuard.
     */
    int _queuedThreads{0};

    /**
     * Threads to be woken.
     *
     * IMPORTANT: Must be protected by either the UniqueLockGuard or SharedLockGuard when modified.
     * Stored as an atomic since it may be modified under the shared lock.
     *
     * Guaranteed not to be modified by other threads when holding UniqueLockGuard.
     */
    AtomicWord<int> _threadsToBeWoken{0};
    stdx::condition_variable _cv;
};

}  // namespace ticket_queues
}  // namespace mongo
