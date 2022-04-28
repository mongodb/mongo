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

#include "mongo/db/service_context.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/ticket.h"
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
                "errnoWithDescription_err"_attr = errorMessage(posixError(err)));
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

SemaphoreTicketHolder::SemaphoreTicketHolder(int num, ServiceContext*) : _outof(num) {
    check(sem_init(&_sem, 0, num));
}

SemaphoreTicketHolder::~SemaphoreTicketHolder() {
    check(sem_destroy(&_sem));
}

boost::optional<Ticket> SemaphoreTicketHolder::tryAcquire(AdmissionContext* admCtx) {
    while (0 != sem_trywait(&_sem)) {
        if (errno == EAGAIN)
            return boost::none;
        if (errno != EINTR)
            failWithErrno(errno);
    }
    return Ticket{};
}

Ticket SemaphoreTicketHolder::waitForTicket(OperationContext* opCtx,
                                            AdmissionContext* admCtx,
                                            WaitMode waitMode) {
    auto ticket = waitForTicketUntil(opCtx, admCtx, Date_t::max(), waitMode);
    invariant(ticket);
    return std::move(*ticket);
}

boost::optional<Ticket> SemaphoreTicketHolder::waitForTicketUntil(OperationContext* opCtx,
                                                                  AdmissionContext* admCtx,
                                                                  Date_t until,
                                                                  WaitMode waitMode) {

    // Attempt to get a ticket without waiting in order to avoid expensive time calculations.
    if (sem_trywait(&_sem) == 0) {
        return Ticket{};
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
                return boost::none;

            deadline = std::min(until, Date_t::now() + intervalMs);
            tsFromDate(deadline, ts);
        } else if (errno != EINTR) {
            failWithErrno(errno);
        }

        // To correctly handle errors from sem_timedwait, we should check for interrupts last.
        // It is possible to unset 'errno' after a call to checkForInterrupt().
        if (waitMode == WaitMode::kInterruptible)
            opCtx->checkForInterrupt();
    }
    return Ticket{};
}

void SemaphoreTicketHolder::release(AdmissionContext* admCtx, Ticket&& ticket) {
    check(sem_post(&_sem));
    ticket.release();
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

    AdmissionContext admCtx;
    while (_outof.load() < newSize) {
        Ticket ticket;
        release(&admCtx, std::move(ticket));
        _outof.fetchAndAdd(1);
    }

    while (_outof.load() > newSize) {
        auto ticket = waitForTicket(nullptr, &admCtx, WaitMode::kUninterruptible);
        ticket.release();
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

SemaphoreTicketHolder::SemaphoreTicketHolder(int num, ServiceContext*) : _outof(num), _num(num) {}

SemaphoreTicketHolder::~SemaphoreTicketHolder() = default;

boost::optional<Ticket> SemaphoreTicketHolder::tryAcquire(AdmissionContext* admCtx) {
    stdx::lock_guard<Latch> lk(_mutex);
    if (!_tryAcquire()) {
        return boost::none;
    }
    return Ticket{};
}

Ticket SemaphoreTicketHolder::waitForTicket(OperationContext* opCtx,
                                            AdmissionContext* admCtx,
                                            WaitMode waitMode) {
    stdx::unique_lock<Latch> lk(_mutex);

    if (waitMode == WaitMode::kInterruptible) {
        opCtx->waitForConditionOrInterrupt(_newTicket, lk, [this] { return _tryAcquire(); });
    } else {
        _newTicket.wait(lk, [this] { return _tryAcquire(); });
    }
    return Ticket{};
}


boost::optional<Ticket> SemaphoreTicketHolder::waitForTicketUntil(OperationContext* opCtx,
                                                                  AdmissionContext* admCtx,
                                                                  Date_t until,
                                                                  WaitMode waitMode) {
    stdx::unique_lock<Latch> lk(_mutex);

    bool taken = [&] {
        if (waitMode == WaitMode::kInterruptible) {
            return opCtx->waitForConditionOrInterruptUntil(
                _newTicket, lk, until, [this] { return _tryAcquire(); });
        } else {
            return _newTicket.wait_until(
                lk, until.toSystemTimePoint(), [this] { return _tryAcquire(); });
        }
    }();
    if (!taken) {
        return boost::none;
    }
    return Ticket{};
}

void SemaphoreTicketHolder::release(AdmissionContext* admCtx, Ticket&& ticket) {
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _num++;
    }
    _newTicket.notify_one();
    ticket.release();
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

void SemaphoreTicketHolder::appendStats(BSONObjBuilder& b) const {
    b.append("out", used());
    b.append("available", available());
    b.append("totalTickets", outof());
}
FifoTicketHolder::FifoTicketHolder(int num, ServiceContext* serviceContext)
    : _capacity(num), _serviceContext(serviceContext) {
    _ticketsAvailable.store(num);
    _enqueuedElements.store(0);
}

FifoTicketHolder::~FifoTicketHolder() {}

int FifoTicketHolder::available() const {
    return _ticketsAvailable.load();
}
int FifoTicketHolder::used() const {
    return outof() - available();
}
int FifoTicketHolder::outof() const {
    return _capacity.load();
}
int FifoTicketHolder::queued() const {
    auto removed = _totalRemovedQueue.loadRelaxed();
    auto added = _totalAddedQueue.loadRelaxed();
    return std::max(static_cast<int>(added - removed), 0);
}

void FifoTicketHolder::release(AdmissionContext* admCtx, Ticket&& ticket) {
    invariant(admCtx);

    ticket.release();
    auto tickSource = _serviceContext->getTickSource();

    // Update statistics.
    _totalFinishedProcessing.fetchAndAddRelaxed(1);
    auto startTime = admCtx->getStartProcessingTime();
    auto delta = tickSource->spanTo<Microseconds>(startTime, tickSource->getTicks());
    _totalTimeProcessingMicros.fetchAndAddRelaxed(delta.count());

    stdx::lock_guard lk(_queueMutex);
    // This loop will most of the time be executed only once. In case some operations in the
    // queue have been cancelled or already took a ticket the releasing operation should search for
    // a waiting operation to avoid leaving an operation waiting indefinitely.
    while (true) {
        if (!_queue.empty()) {
            auto elem = _queue.front();
            _enqueuedElements.subtractAndFetch(1);
            {
                stdx::lock_guard elemLk(elem->modificationMutex);
                if (elem->state != WaitingState::Waiting) {
                    // If the operation has already been finalized we skip the element and don't
                    // assign a ticket.
                    _queue.pop();
                    continue;
                }
                elem->state = WaitingState::Assigned;
            }
            elem->signaler.notify_all();
            _queue.pop();
        } else {
            _ticketsAvailable.addAndFetch(1);
        }
        return;
    }
}

boost::optional<Ticket> FifoTicketHolder::tryAcquire(AdmissionContext* admCtx) {
    invariant(admCtx);

    auto queued = _enqueuedElements.load();
    if (queued > 0)
        return boost::none;

    auto remaining = _ticketsAvailable.subtractAndFetch(1);
    if (remaining < 0) {
        _ticketsAvailable.addAndFetch(1);
        return boost::none;
    }

    // Update statistics.
    if (admCtx->getAdmissions() == 0) {
        _totalNewAdmissions.fetchAndAddRelaxed(1);
    }
    admCtx->start(_serviceContext->getTickSource());
    _totalStartedProcessing.fetchAndAddRelaxed(1);
    return Ticket{};
}

Ticket FifoTicketHolder::waitForTicket(OperationContext* opCtx,
                                       AdmissionContext* admCtx,
                                       WaitMode waitMode) {
    auto res = waitForTicketUntil(opCtx, admCtx, Date_t::max(), waitMode);
    invariant(res);
    return std::move(*res);
}

boost::optional<Ticket> FifoTicketHolder::waitForTicketUntil(OperationContext* opCtx,
                                                             AdmissionContext* admCtx,
                                                             Date_t until,
                                                             WaitMode waitMode) {
    invariant(admCtx);

    // Attempt a quick acquisition first.
    if (auto ticket = tryAcquire(admCtx)) {
        return ticket;
    }

    auto tickSource = _serviceContext->getTickSource();
    // Track statistics
    auto currentWaitTime = tickSource->getTicks();
    auto updateQueuedTime = [&]() {
        auto oldWaitTime = std::exchange(currentWaitTime, tickSource->getTicks());
        auto waitDelta = tickSource->spanTo<Microseconds>(oldWaitTime, currentWaitTime).count();
        _totalTimeQueuedMicros.fetchAndAddRelaxed(waitDelta);
    };
    auto startProcessing = [&] {
        _totalStartedProcessing.fetchAndAddRelaxed(1);
        admCtx->start(tickSource);
    };
    _totalAddedQueue.fetchAndAddRelaxed(1);
    if (admCtx->getAdmissions() == 0) {
        _totalNewAdmissions.fetchAndAddRelaxed(1);
    }

    ScopeGuard dequed([&] {
        updateQueuedTime();
        _totalRemovedQueue.fetchAndAddRelaxed(1);
    });

    // Enqueue.
    auto waitingElement = std::make_shared<WaitingElement>();
    waitingElement->state = WaitingState::Waiting;
    {
        stdx::lock_guard lk(_queueMutex);
        _enqueuedElements.addAndFetch(1);
        // Check for available tickets under the queue lock, in case a ticket has just been
        // released.
        auto remaining = _ticketsAvailable.subtractAndFetch(1);
        if (remaining >= 0) {
            _enqueuedElements.subtractAndFetch(1);
            startProcessing();
            return Ticket{};
        }
        _ticketsAvailable.addAndFetch(1);
        // We copy-construct the shared_ptr here as the waiting element needs to be alive in both
        // release() and waitForTicket(). Otherwise the code could lead to a segmentation fault
        _queue.emplace(waitingElement);
    }

    ScopeGuard cancelWait([&] {
        // Update statistics.
        _totalCanceled.fetchAndAddRelaxed(1);

        bool hasAssignedTicket = false;
        {
            stdx::lock_guard lk(waitingElement->modificationMutex);
            hasAssignedTicket = waitingElement->state == WaitingState::Assigned;
            waitingElement->state = WaitingState::Cancelled;
        }
        if (hasAssignedTicket) {
            // To cover the edge case of getting a ticket assigned before cancelling the ticket
            // request. As we have been granted a ticket we must release it.
            startProcessing();
            release(admCtx, Ticket{});
        }
    });

    auto interruptible =
        waitMode == WaitMode::kInterruptible ? opCtx : Interruptible::notInterruptible();

    auto assigned = [&]() {
        stdx::unique_lock lk(waitingElement->modificationMutex);
        while (true) {
            Date_t deadline = std::min(Date_t::now() + Milliseconds(500), until);
            bool taken = interruptible->waitForConditionOrInterruptUntil(
                waitingElement->signaler, lk, deadline, [&]() {
                    return waitingElement->state == WaitingState::Assigned;
                });

            if (taken || deadline >= until) {
                return taken;
            }
            // Update queued time and retry.
            updateQueuedTime();
        }
    }();

    if (assigned) {
        cancelWait.dismiss();
        startProcessing();
        return Ticket{};
    } else {
        return boost::none;
    }
}

Status FifoTicketHolder::resize(int newSize) {
    stdx::lock_guard<Latch> lk(_resizeMutex);

    if (newSize < 5)
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Minimum value for ticket holder is 5; given " << newSize);

    AdmissionContext admCtx;
    while (_capacity.load() < newSize) {
        Ticket ticket;
        admCtx.start(_serviceContext->getTickSource());
        release(&admCtx, std::move(ticket));
        _capacity.fetchAndAdd(1);
    }

    while (_capacity.load() > newSize) {
        Ticket ticket = waitForTicket(nullptr, &admCtx, WaitMode::kUninterruptible);
        ticket.release();
        _capacity.subtractAndFetch(1);
    }

    invariant(_capacity.load() == newSize);
    return Status::OK();
}

void FifoTicketHolder::appendStats(BSONObjBuilder& b) const {
    b.append("out", used());
    b.append("available", available());
    b.append("totalTickets", outof());
    auto removed = _totalRemovedQueue.loadRelaxed();
    auto added = _totalAddedQueue.loadRelaxed();
    b.append("addedToQueue", added);
    b.append("removedFromQueue", removed);
    b.append("queueLength", std::max(static_cast<int>(added - removed), 0));
    b.append("totalTimeQueuedMicros", _totalTimeQueuedMicros.loadRelaxed());
    auto finished = _totalFinishedProcessing.loadRelaxed();
    auto started = _totalStartedProcessing.loadRelaxed();
    b.append("startedProcessing", started);
    b.append("finishedProcessing", finished);
    b.append("processing", std::max(static_cast<int>(started - finished), 0));
    b.append("totalTimeProcessingMicros", _totalTimeProcessingMicros.loadRelaxed());
    b.append("canceled", _totalCanceled.loadRelaxed());
    b.append("newAdmissions", _totalNewAdmissions.loadRelaxed());
}

}  // namespace mongo
