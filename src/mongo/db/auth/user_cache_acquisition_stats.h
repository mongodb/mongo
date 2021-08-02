/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <memory>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/util/tick_source.h"

namespace mongo {

/**
 * Tracks and stores statistics related to user cache access on a per-operation
 * basis. These statistics are tracked and reported from within CurOp.
 */
class UserCacheAcquisitionStats {
    using AccessInterval = std::pair<Microseconds, Microseconds>;

public:
    UserCacheAcquisitionStats() = default;
    ~UserCacheAcquisitionStats() = default;
    UserCacheAcquisitionStats(const UserCacheAcquisitionStats&) = delete;
    UserCacheAcquisitionStats& operator=(const UserCacheAcquisitionStats&) = delete;

    /**
     * Returns true if the cache has been accessed during the operation and has stats that should
     * be reported.
     */
    bool shouldReport() const {
        return _totalStartedAcquisitionAttempts != 0 || _totalCompletedAcquisitionAttempts != 0;
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
     * Increments the number of cache acquisition attempts.
     **/
    void incrementAcquisitionAttempts() {
        ++_totalStartedAcquisitionAttempts;
    }

    /**
     * Increments the number of cache acquisition attempts.
     **/
    void incrementAcquisitionCompletions() {
        ++_totalCompletedAcquisitionAttempts;
    }

    /**
     * Setters for the Cache Access start and end time.
     **/

    void setCacheAccessStartTime(Microseconds startTime) {
        _cacheAccessStartTime = startTime;
    }

    void setCacheAccessEndTime(Microseconds endTime) {
        invariant(_cacheAccessStartTime != Microseconds{0});
        _cacheAccessEndTime = endTime;
    }

private:
    /**
     * Computes and returns total time spent on all cache accesses.
     */
    Microseconds _timeElapsed(TickSource* tickSource) const;

    /**
     * Total number of started attempts to get a user from the cache.
     */
    std::uint64_t _totalStartedAcquisitionAttempts{0};

    /**
     * Total number of completed user cache accesses.
     */
    std::uint64_t _totalCompletedAcquisitionAttempts{0};


    /**
     * Start and end times of user cache access. If the access is still
     * pending, then the end time will be 0.
     */
    Microseconds _cacheAccessStartTime{Microseconds{0}};
    Microseconds _cacheAccessEndTime{Microseconds{0}};
};
}  // namespace mongo
