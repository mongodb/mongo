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

    /*
     * The RAII handle has access to private members of UserCacheAcquisitionStats but only exposes
     * _recordCacheAccessStart() and _recordCacheAccessEnd().
     */
    friend class UserCacheAcquisitionStatsHandle;

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

private:
    /**
     * Records the start time of a new cache access attempt.
     */
    void _recordCacheAccessStart(Client* client, TickSource* tickSource) {
        stdx::lock_guard<Client> lk(*client);
        _cacheAccessStartTime = tickSource->ticksTo<Microseconds>(tickSource->getTicks());
        ++_totalStartedAcquisitionAttempts;
    }

    /**
     * Records the completion of a cache access attempt by setting the end time. The start time must
     * already be set.
     */
    void _recordCacheAccessEnd(Client* client, TickSource* tickSource) {
        stdx::lock_guard<Client> lk(*client);
        invariant(_cacheAccessStartTime != Microseconds{0});
        _cacheAccessEndTime = tickSource->ticksTo<Microseconds>(tickSource->getTicks());
        ++_totalCompletedAcquisitionAttempts;
    }

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

/**
 * RAII handle that is used to mutate a UserCacheAcquisitionStats object. It automatically records
 * the start of a cache access attempt upon construction and provides access to
 * UserCacheAcquisitionStats::recordCacheAccessEnd(). Additionally, it guarantees that an ongoing
 * user cache attempt will be recorded as complete as soon as the handle goes out of scope. This
 * ensures that UserCacheAcquisitionStats::recordCacheAccessEnd() will be called even if an
 * exception is thrown.
 */
class UserCacheAcquisitionStatsHandle {
public:
    UserCacheAcquisitionStatsHandle() = delete;

    UserCacheAcquisitionStatsHandle(UserCacheAcquisitionStats* statsParam,
                                    Client* client,
                                    TickSource* tickSource)
        : _stats(statsParam), _client(client), _tickSource(tickSource) {
        _stats->_recordCacheAccessStart(_client, _tickSource);
    }

    UserCacheAcquisitionStatsHandle(const UserCacheAcquisitionStatsHandle&) = delete;
    UserCacheAcquisitionStatsHandle& operator=(const UserCacheAcquisitionStatsHandle&) = delete;

    UserCacheAcquisitionStatsHandle(UserCacheAcquisitionStatsHandle&& handle)
        : _stats(std::exchange(handle._stats, nullptr)),
          _client(std::move(handle._client)),
          _tickSource(std::move(handle._tickSource)) {}

    UserCacheAcquisitionStatsHandle& operator=(UserCacheAcquisitionStatsHandle&& handle) {
        _stats = std::exchange(handle._stats, nullptr);
        _client = std::move(handle._client);
        _tickSource = std::move(handle._tickSource);

        return *this;
    }

    ~UserCacheAcquisitionStatsHandle() {
        recordCacheAccessEnd();
    }

    void recordCacheAccessEnd() {
        if (_stats) {
            _stats->_recordCacheAccessEnd(_client, _tickSource);
        }
        _stats = nullptr;
    }

private:
    UserCacheAcquisitionStats* _stats;
    Client* _client;
    TickSource* _tickSource;
};

}  // namespace mongo
