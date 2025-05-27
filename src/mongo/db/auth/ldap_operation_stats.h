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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/auth/ldap_cumulative_operation_stats.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules_incompletely_marked_header.h"
#include "mongo/util/tick_source.h"

#include <cstdint>
#include <iostream>
#include <memory>

namespace mongo {
/**
 * Class used to track statistics associated with LDAP operations for a specfic
 * UserAcquisitionStats object. All methods must be called while holding that UserAcquisitionStats'
 * lock.
 */
class LDAPOperationStats {
public:
    LDAPOperationStats() = default;
    ~LDAPOperationStats() = default;

    /**
     * Marshals all statistics into BSON for reporting.
     */
    void report(BSONObjBuilder* builder, TickSource* tickSource) const;

    /**
     * Marshals all statistics into a string for reporting.
     */
    void toString(StringBuilder* sb, TickSource* tickSource) const;

    /**
     * Checks if any memebers of the LDAPOperationStats object have been updated.
     */
    bool shouldReport() const;

    /**
     * Increment the number of successful or failed referrals for an LDAP operation.
     */
    void incrementSuccessfulReferrals() {
        ++_numSuccessfulReferrals;
    }

    void incrementFailedReferrals() {
        ++_numFailedReferrals;
    }

    /**
     * Update number of binds and searches and set their start times.
     * If the start time is not 0, then some other bind/search is still running on the same
     * OperationContext.
     * In those cases, the start time will not be updated and whichever operation
     * finishes first will record its time and update the start time back to 0.
     */
    void recordBindStart(Microseconds startTime) {
        ++_bindStats.numOps;
        if (_bindStats.startTime == Microseconds{0}) {
            _bindStats.startTime = startTime;
        }
    }

    void recordSearchStart(Microseconds startTime) {
        ++_searchStats.numOps;
        if (_searchStats.startTime == Microseconds{0}) {
            _searchStats.startTime = startTime;
        }
    }

    /**
     * Update bind and search completion.
     * If the start time is Microseconds{0}, then another concurrent LDAP operation on the same
     * UserAcquisitionStats instance completed before this one. In those cases, don't record
     * the elapsed time.
     */
    void recordBindComplete(TickSource* tickSource) {
        if (_bindStats.startTime > Microseconds{0}) {
            _bindStats.totalCompletedOpTime =
                tickSource->ticksTo<Microseconds>(tickSource->getTicks()) - _bindStats.startTime;
        }
        _bindStats.startTime = Microseconds{0};
    }

    void recordSearchComplete(TickSource* tickSource) {
        if (_searchStats.startTime > Microseconds{0}) {
            _searchStats.totalCompletedOpTime =
                tickSource->ticksTo<Microseconds>(tickSource->getTicks()) - _searchStats.startTime;
        }
        _searchStats.startTime = Microseconds{0};
    }

private:
    friend class LDAPCumulativeOperationStats;
    friend class UserAcquisitionStatsTest;
    friend class LDAPTransformContext;

    /**
     * Struct Stats is used to contain information about the bind and search stats
     * of the LDAP Operations.
     */
    struct Stats {
        void report(BSONObjBuilder* builder, TickSource* tickSource, StringData statsName) const;
        void toString(StringBuilder* sb, TickSource* tickSource, StringData statsName) const;
        Microseconds timeElapsed(TickSource* tickSource) const;

        int64_t numOps{0};
        Microseconds startTime{Microseconds{0}};
        Microseconds totalCompletedOpTime{Microseconds{0}};
    };

    /**
     * Number of successful referrals to other LDAP servers
     */
    std::uint64_t _numSuccessfulReferrals{0};

    /**
     * Number of failed referrals to other LDAP servers
     */
    std::uint64_t _numFailedReferrals{0};

    /**
     * Metrics associated with binding and search/querying an LDAP server.
     */
    Stats _bindStats;
    MONGO_MOD_NEEDS_REPLACEMENT Stats _searchStats;  // Used by a friend-ed test.
};
}  // namespace mongo
