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
#include "mongo/util/concurrency/semaphore_ticketholder.h"
#include "mongo/util/concurrency/ticketholder.h"

#include <iostream>

#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {

void SemaphoreTicketHolder::_appendImplStats(BSONObjBuilder& b) const {
    _appendCommonQueueImplStats(b, _semaphoreStats);
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
    : TicketHolderWithQueueingStats(numTickets, serviceContext) {
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
    return Ticket{this, admCtx};
}

void SemaphoreTicketHolder::_releaseToTicketPoolImpl(AdmissionContext* admCtx) noexcept {
    check(sem_post(&_sem));
}

int SemaphoreTicketHolder::available() const {
    int val = 0;
    check(sem_getvalue(&_sem, &val));
    return val;
}

void SemaphoreTicketHolder::_resize(OperationContext* opCtx, int newSize, int oldSize) noexcept {
    auto difference = newSize - oldSize;

    if (difference > 0) {
        for (int i = 0; i < difference; i++) {
            check(sem_post(&_sem));
        }
    } else if (difference < 0) {
        for (int i = 0; i < -difference; i++) {
            check(sem_wait(&_sem));
        }
    }
}

#else

SemaphoreTicketHolder::SemaphoreTicketHolder(int numTickets, ServiceContext* svcCtx)
    : TicketHolderWithQueueingStats(numTickets, svcCtx), _numTickets(numTickets) {}

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

void SemaphoreTicketHolder::_releaseToTicketPoolImpl(AdmissionContext* admCtx) noexcept {
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _numTickets++;
    }
    _newTicket.notify_one();
}

int SemaphoreTicketHolder::available() const {
    return _numTickets;
}

bool SemaphoreTicketHolder::_tryAcquire() {
    if (_numTickets <= 0) {
        if (_numTickets < 0) {
            std::cerr << "DISASTER! in TicketHolder" << std::endl;
        }
        return false;
    }
    _numTickets--;
    return true;
}

void SemaphoreTicketHolder::_resize(OperationContext* opCtx, int newSize, int oldSize) noexcept {
    auto difference = newSize - oldSize;

    stdx::lock_guard<Latch> lk(_mutex);
    _numTickets += difference;

    if (difference > 0) {
        for (int i = 0; i < difference; i++) {
            _newTicket.notify_one();
        }
    }
    // No need to do anything in the other cases as the number of tickets being <= 0 implies they'll
    // have to wait until the current ticket holders release their tickets.
}
#endif
}  // namespace mongo
