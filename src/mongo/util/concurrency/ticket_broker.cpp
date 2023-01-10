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

#include "mongo/util/concurrency/ticket_broker.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/errno_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

// TODO SERVER-72616: Remove futex usage from this class in favour of atomic waits.
#include <linux/futex.h> /* Definition of FUTEX_* constants */
#include <sys/syscall.h> /* Definition of SYS_* constants */
#include <unistd.h>

namespace mongo {
namespace {
static int futex(uint32_t* uaddr,
                 int futex_op,
                 uint32_t val,
                 const struct timespec* timeout,
                 uint32_t* uaddr2,
                 uint32_t val3) noexcept {
    return syscall(SYS_futex, uaddr, futex_op, val, timeout, uaddr2, val3);
}

// TODO SERVER-72616: This can go away once we're on C++20 and std::atomic<T>::wait exists
static stdx::cv_status atomic_wait(AtomicWord<uint32_t>& atomic,
                                   uint32_t expectedValue,
                                   Date_t until) noexcept {
    while (atomic.load() == expectedValue) {
        // Prepare the timeout value for the futex call.
        timespec ts;
        auto now = Date_t::now();
        if (now >= until) {
            return stdx::cv_status::timeout;
        }
        auto millis = until - now;
        ts.tv_sec = millis.count() / 1'000;
        ts.tv_nsec = (millis.count() % 1'000) * 1'000'000;

        auto futexResult = futex(reinterpret_cast<uint32_t*>(&atomic),
                                 FUTEX_WAIT_PRIVATE,
                                 expectedValue,
                                 &ts,
                                 nullptr,
                                 0);
        if (futexResult != 0) {
            switch (errno) {
                // The value has changed before we called futex wait, we treat this as a
                // notification and exit.
                case EAGAIN:
                    return stdx::cv_status::no_timeout;
                case ETIMEDOUT:
                    return stdx::cv_status::timeout;
                // We ignore signal interruptions as other signals are handled by either crashing
                // the server or gracefully exiting the server and waiting for operations to finish.
                case EINTR:
                    break;
                // All other errors are unrecoverable, fassert and crash the server.
                default: {
                    LOGV2_FATAL(7206704,
                                "Error in atomic wait for ticket",
                                "error"_attr = errorMessage(posixError(errno)));
                }
            }
        }
    }
    return stdx::cv_status::no_timeout;
}

// TODO SERVER-72616: This can go away once we're on C++20 and std::atomic<T>::notify_one exists.
static void atomic_notify_one(AtomicWord<uint32_t>& atomic) noexcept {
    auto result =
        futex(reinterpret_cast<uint32_t*>(&atomic), FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
    if (result < 0) {
        // We treat possible errors here as a server crash since we cannot recover from them.
        LOGV2_FATAL(7206703,
                    "Error in atomic notify for ticket",
                    "error"_attr = errorMessage(posixError(errno)));
    }
}
}  // namespace

TicketBroker::TicketBroker() : _queueBegin(nullptr), _queueEnd(nullptr), _numQueued(0) {}

void TicketBroker::_registerAsWaiter(const stdx::unique_lock<stdx::mutex>& growthLock,
                                     Node& node) noexcept {
    // We register the node.
    _numQueued.fetchAndAdd(1);

    if (_queueBegin == nullptr) {
        // If the list is empty we are the first node.
        _queueBegin = &node;
        _queueEnd = &node;
    } else {
        // Otherwise we're the new end and must link the preceding node to us, and us to the
        // preceding node.
        _queueEnd->next = &node;
        node.previous = _queueEnd;
        _queueEnd = &node;
    }
}

void TicketBroker::_unregisterAsWaiter(const stdx::unique_lock<stdx::mutex>& growthLock,
                                       Node& node) noexcept {
    // We've been unregistered by a ticket transfer, nothing to do as the transferer already removed
    // us.
    if (node.futexWord.loadRelaxed() != 0) {
        return;
    }

    auto previousLength = _numQueued.fetchAndSubtract(1);
    // If there was only 1 node it was us, the queue will now be empty.
    if (previousLength == 1) {
        _queueBegin = _queueEnd = nullptr;
        return;
    }
    // If the beginning of the linked list is this node we advance it to the next element.
    if (_queueBegin == &node) {
        _queueBegin = node.next;
        node.next->previous = nullptr;
        return;
    }

    // If the end of the queue is this node, then the new end is the preceding node.
    if (_queueEnd == &node) {
        _queueEnd = node.previous;
        node.previous->next = nullptr;
        return;
    }

    // Otherwise we're in the middle of the list. Preceding and successive nodes must be updated
    // accordingly.
    node.previous->next = node.next;
    node.next->previous = node.previous;
}

TicketBroker::WaitingResult TicketBroker::attemptWaitForTicketUntil(
    stdx::unique_lock<stdx::mutex> growthLock, Date_t until) noexcept {
    // Stack allocate the node of the linked list, this approach lets us ignore heap memory in
    // favour of stack memory which is dramatically cheaper. Care must be taken to ensure that there
    // are no references left in the queue to this node once returning from the method.
    //
    // If std::promise didn't perform a heap allocation we could use it here.
    Node node;

    // We add ourselves as a waiter, we are still holding the lock here.
    _registerAsWaiter(growthLock, node);

    // Finished modifying the linked list, the lock can be released now.
    growthLock.unlock();

    // We now wait until obtaining the notification via the futex word.
    auto waitResult = atomic_wait(node.futexWord, 0, until);
    bool hasTimedOut = waitResult == stdx::cv_status::timeout;

    if (hasTimedOut) {
        // Timing out implies that the node must be removed from the list, block list modifications
        // to prevent segmentation faults.
        growthLock.lock();
        _unregisterAsWaiter(growthLock, node);
        growthLock.unlock();
    }

    // If we haven't timed out it means that the ticket has been transferred to our node. The
    // transfer method removes the node from the linked list, so there's no cleanup to be done.

    auto hasTicket = node.futexWord.load() != 0;

    return TicketBroker::WaitingResult{hasTimedOut, hasTicket};
}

bool TicketBroker::attemptToTransferTicket(
    const stdx::unique_lock<stdx::mutex>& growthLock) noexcept {
    // We can only transfer a ticket if there is a thread waiting for it.
    if (_numQueued.loadRelaxed() > 0) {
        _numQueued.fetchAndSubtract(1);

        // We notify the first element in the queue. To avoid race conditions we first remove the
        // node and then notify the waiting thread. Doing the opposite risks a segmentation fault if
        // the node gets deallocated before we remove it from the list.
        auto node = _queueBegin;
        _queueBegin = node->next;
        if (_queueBegin) {
            // Next node isn't empty, we must inform it that it's first in line.
            _queueBegin->previous = nullptr;
        }
        auto& futexAtomic = node->futexWord;
        futexAtomic.store(1);
        // We've transferred a ticket and removed the node from the list, inform the waiting thread
        // that it can proceed.
        atomic_notify_one(futexAtomic);
        return true;
    }
    return false;
}

}  // namespace mongo
