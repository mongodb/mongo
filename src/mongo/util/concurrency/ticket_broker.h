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

#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * A ticket broker between threads waiting (Ticket-waiters) and threads willing to give a ticket
 * (Ticket-releasers).
 *
 * This broker requires external synchronisation (growthLock) so that it can function correctly. The
 * methods that require synchronisation have their signature written so that it enforces correct
 * usage. Using it with a different mutex in two separate calls is undefined behaviour and will
 * almost certainly cause a deadlock or segmentation fault.
 *
 * This class is to be used when more than one broker is necessary in a given scope and certain
 * guarantees have to be made. Given the following conditions for usage:
 *
 * - Ticket-waiters must acquire the growthLock in order to enter the broker.
 * - Ticket-releasers must acquire the growthLock in order to transfer their ticket to a waiter in
 * the broker.
 *
 * Then this class can provide the following guarantees across the multiple instances of it in
 * scope:
 *
 * - Ticket-releasers can attempt to transfer their ticket to each broker only once. No other waiter
 * will appear between attempts.
 * - Ticket-waiters will get scheduled for execution once transferred a ticket without having to
 * acquire any mutex.
 *
 * This is useful for example if you need to build a scheduler based on top of multiple brokers
 * representing different groups of operations. The ticket-releasers can have "snapshot" guarantees
 * of the state of the system and choose who to transfer the ticket to based on some arbitrary
 * logic.
 *
 * Note that the implementation allows granular thread selection for wakeup but we've chosen to use
 * FIFO semantics to make the critical section as short lived as possible.
 */
class TicketBroker {
public:
    TicketBroker();

    struct WaitingResult {
        bool hasTimedOut;
        bool hasTicket;
    };

    /**
     * Attempts to wait for a ticket until it reaches the specified timeout. The return type will
     * contain whether the attempt was successful and/or if it timed out.
     *
     * This method consumes the lock since it will internally unlock it. Only locking again if the
     * call times out in order to remove the thread from the waiting list.
     */
    WaitingResult attemptWaitForTicketUntil(stdx::unique_lock<stdx::mutex> growthLock,
                                            Date_t until) noexcept;

    /**
     * Transfers the ticket if there is a thread to transfer it to. Returns true if the ticket was
     * transferred.
     *
     * Guarantee: No modifications to the internal linked list will take place while holding the
     * lock.
     */
    bool attemptToTransferTicket(const stdx::unique_lock<stdx::mutex>& growthLock) noexcept;

    /**
     * Returns the number of threads waiting.
     *
     * This method is meant for monitoring and tests only. The value is a snapshot of the system at
     * the moment of calling the method. It will potentially be out of date as soon as it returns.
     *
     * This value will be consistent if called while holding the growthLock.
     */
    int waitingThreadsRelaxed() const noexcept {
        return _numQueued.loadRelaxed();
    }

private:
    /**
     * Node structure of the linked list, it has to be a doubly linked list in order to allow
     * random removal of nodes. Lifetime of these nodes will reside in the stack memory of the
     * thread waiting.
     */
    struct Node {
        Node* previous{nullptr};
        AtomicWord<uint32_t> futexWord{0};
        Node* next{nullptr};
    };

    // Append the node to the linked list.
    void _registerAsWaiter(const stdx::unique_lock<stdx::mutex>& growthLock, Node& node) noexcept;

    // Removes the node from the linked list.
    void _unregisterAsWaiter(const stdx::unique_lock<stdx::mutex>& growthLock, Node& node) noexcept;

    /**
     * Edge nodes of the linked list. To append we need to know the end of the list in order to make
     * appends O(1) instead of O(n).
     */
    Node* _queueBegin;
    Node* _queueEnd;

    /**
     * Number of queued threads in the linked list.
     */
    AtomicWord<int> _numQueued;
};

}  // namespace mongo
