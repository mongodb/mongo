/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/util/concurrency/ticketholder.h"

#include <iostream>

#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

namespace mongo {

TicketHolder::~TicketHolder() = default;

#if defined(__linux__)
namespace {

/**
 * Accepts an errno code, prints its error message, and exits.
 */
void failWithErrno(int err) {
    LOGV2_FATAL(28604,
                "error in Ticketholder: {errnoWithDescription_err}",
                "errnoWithDescription_err"_attr = errnoWithDescription(err));
}

/*
 * Checks the return value from a Linux semaphore function call, and fails with the set errno if the
 * call was unsucessful.
 */
void check(int ret) {
    if (ret == 0)
        return;
    failWithErrno(errno);
}

/**
 * Takes a Date_t deadline and sets the appropriate values in a timespec structure.
 */
void tsFromDate(const Date_t& deadline, struct timespec& ts) {
    ts.tv_sec = deadline.toTimeT();
    ts.tv_nsec = (deadline.toMillisSinceEpoch() % 1000) * 1'000'000;
}
}  // namespace

SemaphoreTicketHolder::SemaphoreTicketHolder(int num) : _outof(num) {
    check(sem_init(&_sem, 0, num));
}

SemaphoreTicketHolder::~SemaphoreTicketHolder() {
    check(sem_destroy(&_sem));
}

bool SemaphoreTicketHolder::tryAcquire() {
    while (0 != sem_trywait(&_sem)) {
        if (errno == EAGAIN)
            return false;
        if (errno != EINTR)
            failWithErrno(errno);
    }
    return true;
}

void SemaphoreTicketHolder::waitForTicket(OperationContext* opCtx) {
    waitForTicketUntil(opCtx, Date_t::max());
}

bool SemaphoreTicketHolder::waitForTicketUntil(OperationContext* opCtx, Date_t until) {
    // Attempt to get a ticket without waiting in order to avoid expensive time calculations.
    if (sem_trywait(&_sem) == 0) {
        return true;
    }

    const Milliseconds intervalMs(500);
    struct timespec ts;

    // To support interrupting ticket acquisition while still benefiting from semaphores, we do a
    // timed wait on an interval to periodically check for interrupts.
    // The wait period interval is the smaller of the default interval and the provided
    // deadline.
    Date_t deadline = std::min(until, Date_t::now() + intervalMs);
    tsFromDate(deadline, ts);

    while (0 != sem_timedwait(&_sem, &ts)) {
        if (errno == ETIMEDOUT) {
            // If we reached the deadline without being interrupted, we have completely timed out.
            if (deadline == until)
                return false;

            deadline = std::min(until, Date_t::now() + intervalMs);
            tsFromDate(deadline, ts);
        } else if (errno != EINTR) {
            failWithErrno(errno);
        }

        // To correctly handle errors from sem_timedwait, we should check for interrupts last.
        // It is possible to unset 'errno' after a call to checkForInterrupt().
        if (opCtx)
            opCtx->checkForInterrupt();
    }
    return true;
}

void SemaphoreTicketHolder::release() {
    check(sem_post(&_sem));
}

Status SemaphoreTicketHolder::resize(int newSize) {
    stdx::lock_guard<Latch> lk(_resizeMutex);

    if (newSize < 5)
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Minimum value for semaphore is 5; given " << newSize);

    if (newSize > SEM_VALUE_MAX)
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Maximum value for semaphore is " << SEM_VALUE_MAX
                                    << "; given " << newSize);

    while (_outof.load() < newSize) {
        release();
        _outof.fetchAndAdd(1);
    }

    while (_outof.load() > newSize) {
        this->TicketHolder::waitForTicket();
        _outof.subtractAndFetch(1);
    }

    invariant(_outof.load() == newSize);
    return Status::OK();
}

int SemaphoreTicketHolder::available() const {
    int val = 0;
    check(sem_getvalue(&_sem, &val));
    return val;
}

int SemaphoreTicketHolder::used() const {
    return outof() - available();
}

int SemaphoreTicketHolder::outof() const {
    return _outof.load();
}

#else

SemaphoreTicketHolder::SemaphoreTicketHolder(int num) : _outof(num), _num(num) {}

SemaphoreTicketHolder::~SemaphoreTicketHolder() = default;

bool SemaphoreTicketHolder::tryAcquire() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _tryAcquire();
}

void SemaphoreTicketHolder::waitForTicket(OperationContext* opCtx) {
    stdx::unique_lock<Latch> lk(_mutex);

    if (opCtx) {
        opCtx->waitForConditionOrInterrupt(_newTicket, lk, [this] { return _tryAcquire(); });
    } else {
        _newTicket.wait(lk, [this] { return _tryAcquire(); });
    }
}

bool SemaphoreTicketHolder::waitForTicketUntil(OperationContext* opCtx, Date_t until) {
    stdx::unique_lock<Latch> lk(_mutex);

    if (opCtx) {
        return opCtx->waitForConditionOrInterruptUntil(
            _newTicket, lk, until, [this] { return _tryAcquire(); });
    } else {
        return _newTicket.wait_until(
            lk, until.toSystemTimePoint(), [this] { return _tryAcquire(); });
    }
}

void SemaphoreTicketHolder::release() {
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _num++;
    }
    _newTicket.notify_one();
}

Status SemaphoreTicketHolder::resize(int newSize) {
    stdx::lock_guard<Latch> lk(_mutex);

    int used = _outof.load() - _num;
    if (used > newSize) {
        std::stringstream ss;
        ss << "can't resize since we're using (" << used << ") "
           << "more than newSize(" << newSize << ")";

        std::string errmsg = ss.str();
        LOGV2(23120, "{errmsg}", "errmsg"_attr = errmsg);
        return Status(ErrorCodes::BadValue, errmsg);
    }

    _outof.store(newSize);
    _num = _outof.load() - used;

    // Potentially wasteful, but easier to see is correct
    _newTicket.notify_all();
    return Status::OK();
}

int SemaphoreTicketHolder::available() const {
    return _num;
}

int SemaphoreTicketHolder::used() const {
    return outof() - _num;
}

int SemaphoreTicketHolder::outof() const {
    return _outof.load();
}

bool SemaphoreTicketHolder::_tryAcquire() {
    if (_num <= 0) {
        if (_num < 0) {
            std::cerr << "DISASTER! in TicketHolder" << std::endl;
        }
        return false;
    }
    _num--;
    return true;
}
#endif

FifoTicketHolder::FifoTicketHolder(int num)
    : _capacity(num), _numAvailable(num), _elementsInQueue(0) {}

FifoTicketHolder::~FifoTicketHolder() {}

int FifoTicketHolder::available() const {
    return _numAvailable.load();
}
int FifoTicketHolder::used() const {
    return _capacity.load() - _numAvailable.load();
}
int FifoTicketHolder::outof() const {
    return _capacity.load();
}

void FifoTicketHolder::_release(WithLock) {
    auto newAvailable = _numAvailable.addAndFetch(1);
    // This is not an optimization but defensively programming against possible edge cases that we
    // haven't thought of. For example, if we have the situation where we have N tickets available
    // but something is in the queue, we must push it to the end of the queue. In this case having a
    // batch release process would be beneficial as it would remove as many elements as it can from
    // the queue, leading us back to the fast ticket path in the normal case.
    while (newAvailable > 0) {
        auto waitingElement = _queue.tryPop();
        if (waitingElement) {
            auto& elem = *waitingElement;
            _elementsInQueue.subtractAndFetch(1);
            stdx::lock_guard lk(elem->modificationMutex);
            if (elem->state != WaitingState::Waiting) {
                // If the operation has already been finalized we skip the element and don't assign
                // a ticket.
                continue;
            }
            elem->state = WaitingState::Assigned;
            elem->signaler.notify_all();
            newAvailable = _numAvailable.subtractAndFetch(1);
        } else {
            return;
        }
    }
}  // namespace mongo

void FifoTicketHolder::release() {
    stdx::lock_guard lk(_queueMutex);
    _release(lk);
}

bool FifoTicketHolder::tryAcquire() {
    stdx::lock_guard lk(_queueMutex);
    if (_numAvailable.load() > 0 && _elementsInQueue.load() == 0) {
        _numAvailable.subtractAndFetch(1);
        return true;
    } else {
        return false;
    }
}

void FifoTicketHolder::waitForTicket(OperationContext* opCtx) {
    waitForTicketUntil(opCtx, Date_t::max());
}

bool FifoTicketHolder::waitForTicketUntil(OperationContext* opCtx, Date_t until) {
    // Attempt a quick acquisition first.
    if (tryAcquire()) {
        return true;
    }

    auto waitingElement = std::make_shared<WaitingElement>();
    waitingElement->state = WaitingState::Waiting;
    {
        stdx::lock_guard lk(_queueMutex);
        if (opCtx) {
            _queue.push(std::shared_ptr(waitingElement), opCtx);
        } else {
            _queue.push(std::shared_ptr(waitingElement));
        }
        _elementsInQueue.addAndFetch(1);
    }

    ScopeGuard cancelWait([&] {
        bool hasAssignedTicket = false;
        {
            stdx::lock_guard lk(waitingElement->modificationMutex);
            hasAssignedTicket = waitingElement->state == WaitingState::Assigned;
            waitingElement->state = WaitingState::Cancelled;
        }
        if (hasAssignedTicket) {
            // To cover the edge case of getting a ticket assigned before cancelling the ticket
            // request. As we have been granted a ticket we must release it.
            stdx::lock_guard queueLock(_queueMutex);
            _release(queueLock);
        }
    });

    auto interruptible = opCtx ? opCtx : Interruptible::notInterruptible();

    stdx::unique_lock lk(waitingElement->modificationMutex);
    auto assigned =
        interruptible->waitForConditionOrInterruptUntil(waitingElement->signaler, lk, until, [&]() {
            return waitingElement->state == WaitingState::Assigned;
        });

    if (assigned) {
        cancelWait.dismiss();
        return true;
    } else {
        return false;
    }
}

Status FifoTicketHolder::resize(int newSize) {
    stdx::lock_guard<Latch> lk(_resizeMutex);

    if (newSize < 5)
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Minimum value for ticket holder is 5; given " << newSize);

    while (_capacity.load() < newSize) {
        release();
        _capacity.fetchAndAdd(1);
    }

    while (_capacity.load() > newSize) {
        this->TicketHolder::waitForTicket();
        _capacity.subtractAndFetch(1);
    }

    invariant(_capacity.load() == newSize);
    return Status::OK();
}

}  // namespace mongo
