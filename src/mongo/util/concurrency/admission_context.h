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

#include <boost/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/platform/waitable_atomic.h"
#include "mongo/util/duration.h"
#include "mongo/util/tick_source.h"

namespace mongo {

class OperationContext;

/**
 * Stores state and statistics related to admission control for a given transactional context.
 */
class AdmissionContext {
public:
    AdmissionContext(const AdmissionContext& other);
    AdmissionContext& operator=(const AdmissionContext& other);

    /**
     * Classifies the priority that an operation acquires a ticket when the system is under high
     * load (operations are throttled waiting for a ticket). Only applicable when there are no
     * available tickets.
     *
     * 'kLow': It's of low importance that the operation acquires a ticket. These low-priority
     * operations are reserved for background tasks that have no other operations dependent on them.
     * These operations may be throttled under load and make significantly less progress as compared
     * to operations of a higher priority.
     *
     * 'kNormal': It's important that the operation be throttled under load. If this operation is
     * throttled, it will not affect system availability or observability. Most operations, both
     * user and internal, should use this priority unless they qualify as 'kLow' or 'kExempt'
     * priority.
     *
     * 'kExempt': It's crucial that the operation makes forward progress - bypasses the ticketing
     * mechanism.
     *
     * Reserved for operations critical to availability (e.g. replication workers) or observability
     * (e.g. FTDC), and any operation that is releasing resources (e.g. committing or aborting
     * prepared transactions). Should be used sparingly.
     */
    enum class Priority { kExempt = 0, kLow, kNormal };

    /**
     * Returns the total time this admission context has ever waited in a queue.
     */
    Microseconds totalTimeQueuedMicros() const;

    /**
     * Returns the time this admission context started waiting to be queued, if it is currently
     * queued.
     */
    boost::optional<TickSource::Tick> startQueueingTime() const;

    /**
     * Returns the number of times this context has taken a ticket.
     */
    int getAdmissions() const;

    /**
     * Returns true if the operation is already holding a ticket.
     */
    bool isHoldingTicket() const {
        return _holdingTicket.loadRelaxed();
    }

    Priority getPriority() const;

protected:
    friend class ScopedAdmissionPriorityBase;
    friend class Ticket;
    friend class TicketHolder;
    friend class WaitingForAdmissionGuard;

    AdmissionContext() = default;

    void recordAdmission();

    void markTicketHeld() {
        tassert(
            9150200,
            "Operation may not hold more than one ticket at a time for the same admission context",
            !_holdingTicket.loadRelaxed());
        _holdingTicket.store(true);
    }

    void markTicketReleased() {
        _holdingTicket.store(false);
    }

    constexpr static TickSource::Tick kNotQueueing = -1;

    Atomic<int32_t> _admissions{0};
    Atomic<Priority> _priority{Priority::kNormal};
    Atomic<int64_t> _totalTimeQueuedMicros;
    WaitableAtomic<TickSource::Tick> _startQueueingTime{kNotQueueing};
    Atomic<bool> _holdingTicket;
};

/**
 * Default-constructible admission context to use for testing purposes.
 */
class MockAdmissionContext : public AdmissionContext {
public:
    MockAdmissionContext() = default;
    // Block until this AdmissionContext is queued waiting on a ticket or the timeout expires.
    // Returns true if we got queued, or false if the timeout expired.
    bool waitUntilQueued(Nanoseconds timeout) {
        return bool(_startQueueingTime.waitFor(kNotQueueing, timeout));
    }
};

/**
 * RAII-style class to set the priority for the ticket admission mechanism when acquiring a global
 * lock.
 */
class ScopedAdmissionPriorityBase {
public:
    explicit ScopedAdmissionPriorityBase(OperationContext* opCtx,
                                         AdmissionContext& admCtx,
                                         AdmissionContext::Priority priority);
    ScopedAdmissionPriorityBase(const ScopedAdmissionPriorityBase&) = delete;
    ScopedAdmissionPriorityBase& operator=(const ScopedAdmissionPriorityBase&) = delete;
    ~ScopedAdmissionPriorityBase();

private:
    OperationContext* const _opCtx;
    AdmissionContext* const _admCtx;
    AdmissionContext::Priority _originalPriority;
};

template <typename AdmissionContextType>
class ScopedAdmissionPriority : public ScopedAdmissionPriorityBase {
public:
    explicit ScopedAdmissionPriority(OperationContext* opCtx, AdmissionContext::Priority priority)
        : ScopedAdmissionPriorityBase(opCtx, AdmissionContextType::get(opCtx), priority) {}
    ~ScopedAdmissionPriority() = default;
};

class WaitingForAdmissionGuard {
public:
    explicit WaitingForAdmissionGuard(AdmissionContext* admCtx, TickSource* tickSource);
    ~WaitingForAdmissionGuard();

private:
    AdmissionContext* _admCtx;
    TickSource* _tickSource;
};

StringData toString(AdmissionContext::Priority priority);

inline int compare(AdmissionContext::Priority lhs, AdmissionContext::Priority rhs) {
    using enum_t = std::underlying_type_t<AdmissionContext::Priority>;
    return static_cast<enum_t>(lhs) - static_cast<enum_t>(rhs);
}

inline bool operator==(AdmissionContext::Priority lhs, AdmissionContext::Priority rhs) {
    return compare(lhs, rhs) == 0;
}

inline bool operator!=(AdmissionContext::Priority lhs, AdmissionContext::Priority rhs) {
    return compare(lhs, rhs) != 0;
}

inline bool operator<(AdmissionContext::Priority lhs, AdmissionContext::Priority rhs) {
    return compare(lhs, rhs) < 0;
}

inline bool operator>(AdmissionContext::Priority lhs, AdmissionContext::Priority rhs) {
    return compare(lhs, rhs) > 0;
}

inline bool operator<=(AdmissionContext::Priority lhs, AdmissionContext::Priority rhs) {
    return compare(lhs, rhs) <= 0;
}

inline bool operator>=(AdmissionContext::Priority lhs, AdmissionContext::Priority rhs) {
    return compare(lhs, rhs) >= 0;
}

}  // namespace mongo
