/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
#include "mongo/util/duration.h"
#include "mongo/util/tick_source.h"

namespace mongo {

/**
 * The PrepareConflictTracker tracks if a read operation encounters a prepare conflict. If it
 * is blocked on a prepare conflict, we will kill the operation during state transitions (step
 * up/step down). This will help us avoid deadlocks between prepare conflicts and state transitions.
 */
class PrepareConflictTracker {
public:
    /**
     * Returns whether a read thread is currently blocked on a prepare conflict.
     */
    bool isWaitingOnPrepareConflict() const;

    /**
     * Marks the start of a prepare conflict. Call updatePrepareConflict() to advance waiting
     * metrics during a conflict, and endPrepareConflict() to mark a prepare conflict as resolved.
     */
    void beginPrepareConflict(TickSource& tickSource);

    void updatePrepareConflict(TickSource& tickSource);

    void endPrepareConflict(TickSource& tickSource);

    /**
     * Returns the duration of time spent blocked on this prepare conflict.
     */
    Microseconds getThisOpPrepareConflictDuration();

    /**
     * Returns the number of prepare conflicts caused by this operation.
     */
    long long getThisOpPrepareConflictCount() const;

    /**
     * Returns the statistics about prepare conflicts as would show up in serverStatus
     */
    static long long getGlobalNumPrepareConflicts();
    static long long getGlobalWaitingForPrepareConflictsMicros();

    /**
     * Sets the global prepare conflict statistics to zero.
     */
    void resetGlobalPrepareConflictStats();

private:
    /**
     * Set to true when a read operation is currently blocked on a prepare conflict.
     */
    AtomicWord<bool> _waitingOnPrepareConflict{false};

    /**
     * Prepare conflicts are monitored continuously over their duration.
     * _prepareConflictLastUpdateTime indicates the time since a prepare conflict was last measured.
     */
    TickSource::Tick _prepareConflictLastUpdateTime{0};

    /**
     * Stores the number of prepare conflicts caused by this operation, if any.
     */
    AtomicWord<long long> _numPrepareConflictsThisOp{0};

    /**
     * Stores the amount of time spent blocked on a prepare read conflict for this operation, if
     * any.
     */
    AtomicWord<int64_t> _thisOpPrepareConflictDuration{0};
};

}  // namespace mongo
