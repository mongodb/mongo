// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/tick_source.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

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
    Atomic<bool> _waitingOnPrepareConflict{false};

    /**
     * Prepare conflicts are monitored continuously over their duration.
     * _prepareConflictLastUpdateTime indicates the time since a prepare conflict was last measured.
     */
    TickSource::Tick _prepareConflictLastUpdateTime{0};

    /**
     * Stores the number of prepare conflicts caused by this operation, if any.
     */
    Atomic<long long> _numPrepareConflictsThisOp{0};

    /**
     * Stores the amount of time spent blocked on a prepare read conflict for this operation, if
     * any.
     */
    Atomic<int64_t> _thisOpPrepareConflictDuration{0};
};

}  // namespace mongo
