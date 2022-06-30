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


#include "mongo/platform/basic.h"

#include "mongo/db/service_context.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/ticketholder.h"

#include <iostream>

#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {

TicketHolder::~TicketHolder() = default;

Status TicketHolder::resize(int newSize) {
    stdx::lock_guard<Latch> lk(_resizeMutex);

    if (newSize < 5)
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Minimum value for ticketholder is 5; given " << newSize);

    AdmissionContext admCtx;
    while (_outof.load() < newSize) {
        _release(&admCtx);
        _outof.fetchAndAdd(1);
    }

    while (_outof.load() > newSize) {
        auto ticket = waitForTicket(nullptr, &admCtx, WaitMode::kUninterruptible);
        ticket.discard();
        _outof.subtractAndFetch(1);
    }

    invariant(_outof.load() == newSize);
    return Status::OK();
}

void TicketHolder::appendStats(BSONObjBuilder& b) const {
    b.append("out", used());
    b.append("available", available());
    b.append("totalTickets", outof());
    auto removed = _totalRemovedQueue.loadRelaxed();
    auto added = _totalAddedQueue.loadRelaxed();
    b.append("addedToQueue", added);
    b.append("removedFromQueue", removed);
    b.append("queueLength", std::max(static_cast<int>(added - removed), 0));
    auto finished = _totalFinishedProcessing.loadRelaxed();
    auto started = _totalStartedProcessing.loadRelaxed();
    b.append("startedProcessing", started);
    b.append("processing", std::max(static_cast<int>(started - finished), 0));
    b.append("finishedProcessing", finished);
    b.append("totalTimeProcessingMicros", _totalTimeProcessingMicros.loadRelaxed());
    b.append("canceled", _totalCanceled.loadRelaxed());
    b.append("newAdmissions", _totalNewAdmissions.loadRelaxed());
    _appendImplStats(b);
}

void TicketHolder::_releaseAndUpdateMetrics(AdmissionContext* admCtx) noexcept {
    _totalFinishedProcessing.fetchAndAddRelaxed(1);
    auto startTime = admCtx->getStartProcessingTime();
    auto tickSource = _serviceContext->getTickSource();
    auto delta = tickSource->spanTo<Microseconds>(startTime, tickSource->getTicks());
    _totalTimeProcessingMicros.fetchAndAddRelaxed(delta.count());
    _release(admCtx);
}

Ticket TicketHolder::waitForTicket(OperationContext* opCtx,
                                   AdmissionContext* admCtx,
                                   WaitMode waitMode) {
    auto res = waitForTicketUntil(opCtx, admCtx, Date_t::max(), waitMode);
    invariant(res);
    return std::move(*res);
}

boost::optional<Ticket> TicketHolder::tryAcquire(AdmissionContext* admCtx) {
    invariant(admCtx);

    auto ticket = _tryAcquireImpl(admCtx);
    // Track statistics.
    if (ticket) {
        if (admCtx->getAdmissions() == 0) {
            _totalNewAdmissions.fetchAndAddRelaxed(1);
        }
        admCtx->start(_serviceContext->getTickSource());
        _totalStartedProcessing.fetchAndAddRelaxed(1);
    }
    return ticket;
}


boost::optional<Ticket> TicketHolder::waitForTicketUntil(OperationContext* opCtx,
                                                         AdmissionContext* admCtx,
                                                         Date_t until,
                                                         WaitMode waitMode) {
    invariant(admCtx);

    // Attempt a quick acquisition first.
    if (auto ticket = tryAcquire(admCtx)) {
        return ticket;
    }

    // Track statistics

    _totalAddedQueue.fetchAndAddRelaxed(1);
    ON_BLOCK_EXIT([&] { _totalRemovedQueue.fetchAndAddRelaxed(1); });

    // Enqueue.
    ScopeGuard cancelWait([&] {
        // Update statistics.
        _totalCanceled.fetchAndAddRelaxed(1);
    });

    auto ticket = _waitForTicketUntilImpl(opCtx, admCtx, until, waitMode);

    if (ticket) {
        cancelWait.dismiss();
        if (admCtx->getAdmissions() == 0) {
            _totalNewAdmissions.fetchAndAddRelaxed(1);
        }
        admCtx->start(_serviceContext->getTickSource());
        _totalStartedProcessing.fetchAndAddRelaxed(1);
        return ticket;
    } else {
        return boost::none;
    }
}

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

SemaphoreTicketHolder::SemaphoreTicketHolder(int numTickets, ServiceContext* serviceContext)
    : TicketHolder(numTickets, serviceContext) {
    check(sem_init(&_sem, 0, numTickets));
}

SemaphoreTicketHolder::~SemaphoreTicketHolder() {
    check(sem_destroy(&_sem));
}

boost::optional<Ticket> SemaphoreTicketHolder::_tryAcquireImpl(AdmissionContext* admCtx) {
    while (0 != sem_trywait(&_sem)) {
        if (errno == EAGAIN)
            return boost::none;
        if (errno != EINTR)
            failWithErrno(errno);
    }
    return Ticket{this, admCtx};
}

boost::optional<Ticket> SemaphoreTicketHolder::_waitForTicketUntilImpl(OperationContext* opCtx,
                                                                       AdmissionContext* admCtx,
                                                                       Date_t until,
                                                                       WaitMode waitMode) {

    auto tickSource = _serviceContext->getTickSource();
    // Track statistics
    auto currentWaitTime = tickSource->getTicks();
    auto updateQueuedTime = [&]() {
        auto oldWaitTime = std::exchange(currentWaitTime, tickSource->getTicks());
        auto waitDelta = tickSource->spanTo<Microseconds>(oldWaitTime, currentWaitTime).count();
        _totalTimeQueuedMicros.fetchAndAddRelaxed(waitDelta);
    };

    ON_BLOCK_EXIT(updateQueuedTime);

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

        updateQueuedTime();
    }
    return Ticket{this, admCtx};
}

void SemaphoreTicketHolder::_release(AdmissionContext* admCtx) noexcept {
    check(sem_post(&_sem));
}

int SemaphoreTicketHolder::available() const {
    int val = 0;
    check(sem_getvalue(&_sem, &val));
    return val;
}

#else

SemaphoreTicketHolder::SemaphoreTicketHolder(int numTickets, ServiceContext* svcCtx)
    : TicketHolder(numTickets, svcCtx), _num(numTickets) {}

SemaphoreTicketHolder::~SemaphoreTicketHolder() = default;

boost::optional<Ticket> SemaphoreTicketHolder::_tryAcquireImpl(AdmissionContext* admCtx) {
    stdx::lock_guard<Latch> lk(_mutex);
    if (!_tryAcquire()) {
        return boost::none;
    }
    return Ticket{this, admCtx};
}

boost::optional<Ticket> SemaphoreTicketHolder::_waitForTicketUntilImpl(OperationContext* opCtx,
                                                                       AdmissionContext* admCtx,
                                                                       Date_t until,
                                                                       WaitMode waitMode) {
    stdx::unique_lock<Latch> lk(_mutex);

    bool taken = [&] {
        if (waitMode == WaitMode::kInterruptible) {
            return opCtx->waitForConditionOrInterruptUntil(
                _newTicket, lk, until, [this] { return _tryAcquire(); });
        } else {
            if (until == Date_t::max()) {
                _newTicket.wait(lk, [this] { return _tryAcquire(); });
                return true;
            } else {
                return _newTicket.wait_until(
                    lk, until.toSystemTimePoint(), [this] { return _tryAcquire(); });
            }
        }
    }();
    if (!taken) {
        return boost::none;
    }
    return Ticket{this, admCtx};
}

void SemaphoreTicketHolder::_release(AdmissionContext* admCtx) noexcept {
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _num++;
    }
    _newTicket.notify_one();
}

int SemaphoreTicketHolder::available() const {
    return _num;
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

void SemaphoreTicketHolder::_appendImplStats(BSONObjBuilder& b) const {
    b.append("totalTimeQueuedMicros", _totalTimeQueuedMicros.loadRelaxed());
}

FifoTicketHolder::FifoTicketHolder(int numTickets, ServiceContext* serviceContext)
    : TicketHolder(numTickets, serviceContext) {
    _ticketsAvailable.store(numTickets);
    _enqueuedElements.store(0);
}

FifoTicketHolder::~FifoTicketHolder() {}

int FifoTicketHolder::available() const {
    return _ticketsAvailable.load();
}

void FifoTicketHolder::_release(AdmissionContext* admCtx) noexcept {
    invariant(admCtx);

    stdx::lock_guard lk(_queueMutex);
    // This loop will most of the time be executed only once. In case some operations in the
    // queue have been cancelled or already took a ticket the releasing operation should search for
    // a waiting operation to avoid leaving an operation waiting indefinitely.
    while (true) {
        if (!_queue.empty()) {
            auto elem = _queue.front();
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

boost::optional<Ticket> FifoTicketHolder::_tryAcquireImpl(AdmissionContext* admCtx) {

    auto queued = _enqueuedElements.load();
    if (queued > 0)
        return boost::none;

    auto remaining = _ticketsAvailable.subtractAndFetch(1);
    if (remaining < 0) {
        _ticketsAvailable.addAndFetch(1);
        return boost::none;
    }

    return Ticket{this, admCtx};
}


boost::optional<Ticket> FifoTicketHolder::_waitForTicketUntilImpl(OperationContext* opCtx,
                                                                  AdmissionContext* admCtx,
                                                                  Date_t until,
                                                                  WaitMode waitMode) {
    auto tickSource = _serviceContext->getTickSource();
    // Track statistics
    auto currentWaitTime = tickSource->getTicks();
    auto updateQueuedTime = [&]() {
        auto oldWaitTime = std::exchange(currentWaitTime, tickSource->getTicks());
        auto waitDelta = tickSource->spanTo<Microseconds>(oldWaitTime, currentWaitTime).count();
        _totalTimeQueuedMicros.fetchAndAddRelaxed(waitDelta);
    };

    ON_BLOCK_EXIT(updateQueuedTime);

    // Enqueue.
    auto waitingElement = std::make_shared<WaitingElement>();
    waitingElement->state = WaitingState::Waiting;
    {
        stdx::lock_guard lk(_queueMutex);
        // Check for available tickets under the queue lock, in case a ticket has just been
        // released.
        auto remaining = _ticketsAvailable.subtractAndFetch(1);
        if (remaining >= 0) {
            return Ticket{this, admCtx};
        }
        _ticketsAvailable.addAndFetch(1);
        // We copy-construct the shared_ptr here as the waiting element needs to be alive in both
        // release() and waitForTicket(). Otherwise the code could lead to a segmentation fault
        _queue.emplace(waitingElement);
        _enqueuedElements.addAndFetch(1);
    }

    ON_BLOCK_EXIT([&] { _enqueuedElements.subtractAndFetch(1); });
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
            _release(admCtx);
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
        return Ticket{this, admCtx};
    } else {
        return boost::none;
    }
}

void FifoTicketHolder::_appendImplStats(BSONObjBuilder& b) const {
    b.append("totalTimeQueuedMicros", _totalTimeQueuedMicros.loadRelaxed());
}

SchedulingTicketHolder::SchedulingTicketHolder(int numTickets,
                                               unsigned int numQueues,
                                               ServiceContext* serviceContext)
    : TicketHolder(numTickets, serviceContext), _serviceContext(serviceContext) {
    for (std::size_t i = 0; i < numQueues; i++) {
        _queues.emplace_back(this);
    }
    _queues.shrink_to_fit();
    _ticketsAvailable.store(numTickets);
}

SchedulingTicketHolder::~SchedulingTicketHolder() {}

int SchedulingTicketHolder::available() const {
    return _ticketsAvailable.load();
}

void SchedulingTicketHolder::_release(AdmissionContext* admCtx) noexcept {
    invariant(admCtx);

    // The idea behind the release mechanism consists of a consistent view of queued elements
    // waiting for a ticket and many threads releasing tickets simultaneously. The releasers will
    // proceed to attempt to dequeue an element by seeing if there are threads not woken and waking
    // one, having increased the number of woken threads for accuracy. Once the thread gets woken it
    // will then decrease the number of woken threads (as it has been woken) and then attempt to
    // acquire a ticket. The two possible states are either one or more releasers releasing or a
    // thread waking up due to the RW mutex.
    //
    // Under this lock the queues cannot be modified in terms of someone attempting to enqueue on
    // them, only waking threads is allowed.
    ReleaserLockGuard lk(_queueMutex);  // NOLINT
    _ticketsAvailable.addAndFetch(1);
    if (std::all_of(_queues.begin(), _queues.end(), [](const Queue& queue) {
            return queue.queuedElems() == 0;
        })) {
        return;
    }
    _dequeueWaitingThread();
}

bool SchedulingTicketHolder::_tryAcquireTicket() {
    auto remaining = _ticketsAvailable.subtractAndFetch(1);
    if (remaining < 0) {
        _ticketsAvailable.addAndFetch(1);
        return false;
    }
    return true;
}

boost::optional<Ticket> SchedulingTicketHolder::_tryAcquireImpl(AdmissionContext* admCtx) {
    invariant(admCtx);

    auto hasAcquired = _tryAcquireTicket();
    if (hasAcquired) {
        return Ticket{this, admCtx};
    }
    return boost::none;
}

boost::optional<Ticket> SchedulingTicketHolder::_waitForTicketUntilImpl(OperationContext* opCtx,
                                                                        AdmissionContext* admCtx,
                                                                        Date_t until,
                                                                        WaitMode waitMode) {
    invariant(admCtx);

    auto interruptible =
        waitMode == WaitMode::kInterruptible ? opCtx : Interruptible::notInterruptible();

    auto& queue = _getQueueToUse(opCtx, admCtx);

    bool assigned;
    {
        stdx::unique_lock lk(_queueMutex);
        assigned = queue.enqueue(interruptible, lk, until);
    }
    if (assigned) {
        return Ticket{this, admCtx};
    } else {
        return boost::none;
    }
}

bool SchedulingTicketHolder::Queue::attemptToDequeue() {
    auto threadsToBeWoken = _threadsToBeWoken.load();
    while (threadsToBeWoken < _queuedThreads) {
        auto canDequeue = _threadsToBeWoken.compareAndSwap(&threadsToBeWoken, threadsToBeWoken + 1);
        if (canDequeue) {
            _queue.notify_one();
            return true;
        }
    }
    return false;
}

void SchedulingTicketHolder::Queue::_signalThreadWoken() {
    auto currentThreadsToBeWoken = _threadsToBeWoken.load();
    while (currentThreadsToBeWoken > 0) {
        if (_threadsToBeWoken.compareAndSwap(&currentThreadsToBeWoken,
                                             currentThreadsToBeWoken - 1)) {
            return;
        }
    }
}

bool SchedulingTicketHolder::Queue::enqueue(Interruptible* interruptible,
                                            EnqueuerLockGuard& queueLock,
                                            const Date_t& until) {
    _queuedThreads++;
    // Before exiting we remove ourselves from the count of queued threads, we are still holding the
    // lock here so this is safe.
    ON_BLOCK_EXIT([&] { _queuedThreads--; });
    bool isFirstCheck = true;
    do {
        try {
            interruptible->waitForConditionOrInterruptUntil(_queue, queueLock, until, [&] {
                // As this block is executed when getting woken we must modify the woken count.
                // Otherwise we are prone to deadlocking if we get woken and there are no tickets
                // available, permanently signalling that a thread has been woken.
                //
                // We don't signal that a thread has been woken during the first predicate check as
                // the underlying implementation does the following:
                //
                // while(!pred()) {
                //     cv.wait(lk);
                // }
                //
                // Thus the predicate will always be called once before waiting.
                if (isFirstCheck) {
                    isFirstCheck = false;
                } else {
                    _signalThreadWoken();
                }
                return _holder->_ticketsAvailable.load() > 0;
            });
        } catch (...) {
            _signalThreadWoken();
            throw;
        }
        if (Date_t::now() >= until) {
            return false;
        }
    } while (!_holder->_tryAcquireTicket());
    return true;
}

void StochasticTicketHolder::_dequeueWaitingThread() {
    QueueType preferredQueue = QueueType::ReaderQueue;
    if (auto client = Client::getCurrent()) {
        auto& prng = client->getPrng();
        auto randomNumber =
            std::uniform_int_distribution<std::uint32_t>(1, _totalWeight)(prng.urbg());
        if (randomNumber <= _readerWeight) {
            preferredQueue = QueueType::ReaderQueue;
        } else {
            preferredQueue = QueueType::WriterQueue;
        }
    }
    if (!_queues[static_cast<unsigned int>(preferredQueue)].attemptToDequeue()) {
        std::size_t otherQueueIndex;
        switch (preferredQueue) {
            case QueueType::ReaderQueue:
                otherQueueIndex = static_cast<std::size_t>(QueueType::WriterQueue);
                break;
            case QueueType::WriterQueue:
                otherQueueIndex = static_cast<std::size_t>(QueueType::ReaderQueue);
                break;
        }
        _queues[otherQueueIndex].attemptToDequeue();
    }
}

SchedulingTicketHolder::Queue& StochasticTicketHolder::_getQueueToUse(
    OperationContext* opCtx, const AdmissionContext* admCtx) {
    auto lockMode = admCtx->getLockMode();
    invariant(lockMode != MODE_NONE);
    switch (lockMode) {
        case MODE_IS:
        case MODE_S:
            return _queues[static_cast<unsigned int>(QueueType::ReaderQueue)];
        case MODE_IX:
            return _queues[static_cast<unsigned int>(QueueType::WriterQueue)];
        default:
            MONGO_UNREACHABLE;
    };
}
StochasticTicketHolder::StochasticTicketHolder(int numTickets,
                                               int readerWeight,
                                               int writerWeight,
                                               ServiceContext* serviceContext)
    : SchedulingTicketHolder(numTickets, 2, serviceContext),
      _readerWeight(readerWeight),
      _totalWeight(readerWeight + writerWeight) {}
}  // namespace mongo
