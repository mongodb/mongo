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
#include "mongo/util/duration.h"

namespace mongo {

class OperationContext;

/**
 * Stores state and statistics related to admission control for a given transactional context.
 */
class AdmissionContext {
public:
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
    Microseconds totalTimeQueuedMicros() const {
        return _totalTimeQueuedMicros;
    }

    /**
     * Returns the number of times this context has taken a ticket.
     */
    int getAdmissions() const {
        return _admissions;
    }

    Priority getPriority() const {
        return _priority;
    }

protected:
    friend class ScopedAdmissionPriorityBase;
    friend class TicketHolder;

    AdmissionContext() = default;

    void recordAdmission() {
        _admissions += 1;
    }
    void recordTimeQueued(Microseconds timeQueued) {
        _totalTimeQueuedMicros += timeQueued;
    }

    int32_t _admissions{0};
    Priority _priority{Priority::kNormal};
    Microseconds _totalTimeQueuedMicros{0};
};

/**
 * Default-constructible admission context to use for testing purposes.
 */
class MockAdmissionContext : public AdmissionContext {
public:
    MockAdmissionContext() = default;
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
