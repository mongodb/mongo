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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/ldap_cumulative_operation_stats.h"
#include "mongo/db/auth/ldap_operation_stats.h"
#include "mongo/db/auth/user_cache_access_stats.h"
#include "mongo/db/client.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/duration.h"
#include "mongo/util/tick_source.h"

#include <memory>

namespace mongo {
enum UserAcquisitionOpType { kCache, kBind, kSearch, kSuccessfulReferral, kFailedReferral };

/**
 * Generalized wrapper class for CurOp. Has access to the `UserCacheAccessStats` class and to
 * `LDAPOperationStats` class. This allows CurOp to have encapsulated information about LDAP
 * specific metrics and existing user cache metrics.
 */
class UserAcquisitionStats {
    /**
     * The RAII handle has access to private members of UserAcquisitionStats but only exposes
     * methods for recording start times, end times of LDAP Operations or User Cache operations.
     * And a method to increment the referral count of an LDAP operation.
     */
    friend class UserAcquisitionStatsHandle;

public:
    UserAcquisitionStats()
        : _userCacheAccessStats(UserCacheAccessStats()),
          _ldapOperationStats(LDAPOperationStats()) {};
    ~UserAcquisitionStats() = default;

    /**
     * Functions for determining if there is data to report in the userCacheAcquisitionStats or
     * LDAPOperationStats object
     */
    bool shouldReportUserCacheAccessStats() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _userCacheAccessStats.shouldReport();
    }

    bool shouldReportLDAPOperationStats() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _ldapOperationStats.shouldReport();
    }

    /**
     * Read only serialization methods for UserCacheAcquisitionStats object. Methods will serialize
     * to string and to BSON. Used for reporting to $currentOp, database profiling, and logging.
     */
    void reportUserCacheAcquisitionStats(BSONObjBuilder* builder, TickSource* tickSource) const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _userCacheAccessStats.report(builder, tickSource);
    }

    void userCacheAcquisitionStatsToString(StringBuilder* sb, TickSource* tickSource) const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _userCacheAccessStats.toString(sb, tickSource);
    }

    /**
     * Read only serialization methods for LDAPOperationStats object. Methods will serialize
     * to string and to BSON. Used for reporting to $currentOp, database profiling, and logging.
     */
    void reportLdapOperationStats(BSONObjBuilder* builder, TickSource* tickSource) const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _ldapOperationStats.report(builder, tickSource);
    }

    void ldapOperationStatsToString(StringBuilder* sb, TickSource* tickSource) const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _ldapOperationStats.toString(sb, tickSource);
    }

    /**
     * Returns a copy of the current _userCacheAccessStats object.
     */
    UserCacheAccessStats getUserCacheAccessStatsSnapshot() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _userCacheAccessStats;
    }

    /**
     * Returns a copy of the current _ldapOperationStats object.
     */
    LDAPOperationStats getLdapOperationStatsSnapshot() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _ldapOperationStats;
    }

private:
    /**
     * Records the start time of an LDAP bind operation
     */
    void _recordBindStart(TickSource* tickSource) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _ldapOperationStats.recordBindStart(_getTime(tickSource));
    }

    /**
     * Records the completion of a LDAP bind operation by setting the end time. The start time
     * must already be set.
     */
    void _recordBindComplete(TickSource* tickSource) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _ldapOperationStats.recordBindComplete(tickSource);
    }

    /**
     * Records the start time of an LDAP search/query operation
     */
    void _recordSearchStart(TickSource* tickSource) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _ldapOperationStats.recordSearchStart(_getTime(tickSource));
    }

    /**
     * Records the completion of a LDAP search/query operation by setting the end time. The start
     * time must already be set.
     */
    void _recordSearchComplete(TickSource* tickSource) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _ldapOperationStats.recordSearchComplete(tickSource);
    }

    /**
     * Increments the number of successful referrals to another LDAP server.
     */
    void _incrementSuccessfulReferrals() {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _ldapOperationStats.incrementSuccessfulReferrals();
    }

    /**
     * Increments the number of failed referrals to another LDAP server.
     */
    void _incrementFailedReferrals() {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _ldapOperationStats.incrementFailedReferrals();
    }

    /**
     * Records the start time of a new cache access attempt.
     */
    void _recordCacheAccessStart(TickSource* tickSource) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _userCacheAccessStats.recordUserCacheAccessStart(_getTime(tickSource));
    }

    /**
     * Records the completion of a cache access attempt by setting the end time. The start time must
     * already be set.
     */
    void _recordCacheAccessComplete(TickSource* tickSource) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _userCacheAccessStats.recordUserCacheAccessComplete(tickSource);
    }

    /**
     * Helper function to get the time from a TickSouce and convert it to Microseconds.
     */
    Microseconds _getTime(TickSource* tickSource) {
        return tickSource->ticksTo<Microseconds>(tickSource->getTicks());
    }

    /**
     * UserCacheAccessStats and LDAPOperationStats
     * associated with this UserAcquisitionStats object.
     */
    UserCacheAccessStats _userCacheAccessStats;
    LDAPOperationStats _ldapOperationStats;

    mutable stdx::mutex _mutex;
};

using SharedUserAcquisitionStats = std::shared_ptr<UserAcquisitionStats>;

/**
 * RAII handle that is used to mutate a UserAcquisitionStats object. It automatically records
 * the start time of either an LDAP or User Cache Acquisition operation upon construction.
 * Additionally, it guarantees that an ongoing user attempt will be recorded as complete as
 * soon as the handle goes out of scope. This ensures that a call to the corresponding
 * operations set end timer  will be called even if an exception is thrown.
 */
class UserAcquisitionStatsHandle {
public:
    UserAcquisitionStatsHandle() = delete;

    UserAcquisitionStatsHandle(UserAcquisitionStats* statsParam,
                               TickSource* tickSource,
                               UserAcquisitionOpType type)
        : _stats(statsParam), _tickSource(tickSource), _type(type) {
        if (_stats) {
            switch (_type) {
                case kCache:
                    _stats->_recordCacheAccessStart(_tickSource);
                    break;
                case kBind:
                    _stats->_recordBindStart(_tickSource);
                    break;
                case kSearch:
                    _stats->_recordSearchStart(_tickSource);
                    break;
                case kSuccessfulReferral:
                case kFailedReferral:
                    break;
            }
        }
    }

    UserAcquisitionStatsHandle(const UserAcquisitionStatsHandle&) = delete;

    UserAcquisitionStatsHandle& operator=(const UserAcquisitionStatsHandle&) = delete;

    UserAcquisitionStatsHandle(UserAcquisitionStatsHandle&& handle)
        : _stats(std::exchange(handle._stats, nullptr)),
          _tickSource(std::move(handle._tickSource)) {}

    UserAcquisitionStatsHandle& operator=(UserAcquisitionStatsHandle&& handle) {
        _stats = std::exchange(handle._stats, nullptr);
        _tickSource = std::move(handle._tickSource);
        return *this;
    }

    ~UserAcquisitionStatsHandle() {
        recordTimerEnd();
    }

    void recordTimerEnd() {
        if (_stats) {
            switch (_type) {
                case kCache:
                    _stats->_recordCacheAccessComplete(_tickSource);
                    break;
                case kBind:
                    _stats->_recordBindComplete(_tickSource);
                    break;
                case kSearch:
                    _stats->_recordSearchComplete(_tickSource);
                    break;
                case kSuccessfulReferral:
                    _stats->_incrementSuccessfulReferrals();
                    break;
                case kFailedReferral:
                    _stats->_incrementFailedReferrals();
                    break;
            }
        }
        _stats = nullptr;
    }

private:
    UserAcquisitionStats* _stats;
    TickSource* _tickSource;
    UserAcquisitionOpType _type;
};
}  // namespace mongo
