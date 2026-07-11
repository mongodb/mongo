// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/client.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/tick_source.h"

#include <cstdint>
#include <memory>
#include <utility>

namespace mongo {

/**
 * Tracks and stores statistics related to user cache access on a per-operation
 * basis. These statistics are tracked and reported from within CurOp.
 */
class [[MONGO_MOD_PUBLIC]] UserCacheAccessStats {
    using AccessInterval = std::pair<Microseconds, Microseconds>;

public:
    UserCacheAccessStats() = default;
    ~UserCacheAccessStats() = default;

    /**
     * Returns true if the cache has been accessed during the operation and has stats that should
     * be reported.
     */
    bool shouldReport() const {
        return _startedCacheAccessAttempts > 0 || _completedCacheAccessAttempts > 0;
    }

    /**
     * Marshals all statistics into BSON for reporting.
     */
    void report(BSONObjBuilder* builder, TickSource* tickSource) const;

    /**
     * Marshals all statistics into a string for reporting.
     */
    void toString(StringBuilder* sb, TickSource* tickSource) const;

    /**
     * Updates statistics to reflect the start of a new cache access.
     * If the ongoingCacheAccessStartTime is not 0, then another concurrent thread
     * must have already started its attempt without completing yet. In that case, the
     * start time is left as-is.
     **/
    void recordUserCacheAccessStart(Microseconds startTime) {
        ++_startedCacheAccessAttempts;

        if (_ongoingCacheAccessStartTime == Microseconds{0}) {
            _ongoingCacheAccessStartTime = startTime;
        }
    }

    /**
     * Updates statistics to reflect the completion of an ongoing cache access.
     * If the ongoingCacheAccessStartTime is 0, then another concurrent thread must have
     * already recorded its completion beforehand, so in that case the total completed
     * time is not updated.
     **/
    void recordUserCacheAccessComplete(TickSource* tickSource) {
        ++_completedCacheAccessAttempts;

        if (_ongoingCacheAccessStartTime > Microseconds{0}) {
            _totalCompletedCacheAccessTime = _totalCompletedCacheAccessTime +
                (tickSource->ticksTo<Microseconds>(tickSource->getTicks()) -
                 _ongoingCacheAccessStartTime);
        }

        _ongoingCacheAccessStartTime = Microseconds{0};
    }

private:
    friend class UserAcquisitionStatsTest;
    /**
     * Computes and returns total time spent on cache access.
     * (_totalCompletedAccessTime + (currTime - _ongoingCacheAccessStartTime))
     */
    Microseconds _timeElapsed(TickSource* tickSource) const;

    /**
     * Total number of started attempts to get a user from the cache.
     */
    std::uint64_t _startedCacheAccessAttempts{0};

    /**
     * Total number of completed user cache accesses.
     */
    std::uint64_t _completedCacheAccessAttempts{0};

    /**
     * Start time of an ongoing user cache access attempt, if any.
     */
    Microseconds _ongoingCacheAccessStartTime{Microseconds{0}};

    /**
     * Total time spent on already-completed user cache accesses.
     */
    Microseconds _totalCompletedCacheAccessTime{Microseconds{0}};
};
}  // namespace mongo
