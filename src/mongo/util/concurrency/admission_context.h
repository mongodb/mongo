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

#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/util/tick_source.h"
#include <boost/optional.hpp>

namespace mongo {

/**
 * Stores state and statistics related to admission control for a given transactional context.
 */
class AdmissionContext {

public:
    AdmissionContext() {}

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
     * user and internal, should use this priority unless they qualify as 'kLow' or 'kHigh'
     * priority.
     *
     * 'kHigh': It's crucial that the operation makes forward progress - bypassing ticket
     * acquisition. Reserved for operations critical to availability (e.g. replication workers) or
     * observability (e.g. FTDC), and any operation that is releasing resources (e.g. committing or
     * aborting prepared transactions). Should be used sparingly.
     *
     * TODO SERVER-67951: Update comment to address that kHigh priority operations are always
     * granted a ticket immediately upon request.
     */
    enum class AcquisitionPriority { kLow, kNormal, kHigh };

    void start(TickSource* tickSource) {
        admissions++;
        if (tickSource) {
            _startProcessingTime = tickSource->getTicks();
        }
    }

    TickSource::Tick getStartProcessingTime() const {
        return _startProcessingTime;
    }

    /**
     * Returns the number of times this context has taken a ticket.
     */
    int getAdmissions() const {
        return admissions;
    }

    void setLockMode(LockMode lockMode) {
        _lockMode = lockMode;
    }

    LockMode getLockMode() const {
        return _lockMode;
    }

    void setPriority(AcquisitionPriority priority) {
        _priority = priority;
    }

    AcquisitionPriority getPriority() {
        invariant(_priority);
        return _priority.get();
    }

private:
    TickSource::Tick _startProcessingTime{};
    int admissions{};
    LockMode _lockMode = LockMode::MODE_NONE;
    boost::optional<AcquisitionPriority> _priority;
};

}  // namespace mongo
