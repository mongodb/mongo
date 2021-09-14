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

#include <cstdint>
#include <iostream>
#include <memory>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/duration.h"
#include "mongo/util/tick_source.h"

namespace mongo {
/**
 * Class used to track statistics associated with LDAP operations for a specfic
 * UserAcquisitionStats object.
 */
class LDAPOperationStats {
public:
    LDAPOperationStats() = default;
    ~LDAPOperationStats() = default;
    LDAPOperationStats(const LDAPOperationStats&) = delete;
    LDAPOperationStats& operator=(const LDAPOperationStats&) = delete;
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
     * Increment the total number of referrals for an LDAP operation.
     */
    void incrementReferrals() {
        ++_numReferrals;
    }

    /**
     * Increment LDAPOperationStats bind, search, and unbind number of operations feild
     */
    void incrementBindNumOps() {
        ++_bindStats.numOps;
    }

    void incrementSearchNumOps() {
        ++_searchStats.numOps;
    }

    void incrementUnbindNumOps() {
        ++_unbindStats.numOps;
    }

    /**
     * Setters for bind, search, and unbind start times
     */
    void setBindStatsStartTime(Microseconds startTime) {
        _bindStats.startTime = startTime;
    }

    void setSearchStatsStartTime(Microseconds startTime) {
        _searchStats.startTime = startTime;
    }

    void setUnbindStatsStartTime(Microseconds startTime) {
        _unbindStats.startTime = startTime;
    }

    /**
     * Setters for bind, search, and unbind end times
     */
    void setBindStatsEndTime(Microseconds startTime) {
        invariant(_bindStats.startTime != Microseconds{0});
        _bindStats.endTime = startTime;
    }

    void setSearchStatsEndTime(Microseconds startTime) {
        invariant(_searchStats.startTime != Microseconds{0});
        _searchStats.endTime = startTime;
    }

    void setUnbindStatsEndTime(Microseconds startTime) {
        invariant(_unbindStats.startTime != Microseconds{0});
        _unbindStats.endTime = startTime;
    }

private:
    friend class LDAPCumulativeOperationStats;

    /**
     * Struct Stats is used to contain information about the bind, search, and unbind stats
     * of the LDAP Operations.
     */
    struct Stats {
        int64_t numOps{0};
        Microseconds startTime{Microseconds{0}};
        Microseconds endTime{Microseconds{0}};
    };

    /**
     * Helper function to reduce redundancy for constructing BSON report of LDAPOperationsStats.
     */
    void reportHelper(BSONObjBuilder* builder,
                      TickSource* tickSource,
                      Stats ldapOpStats,
                      StringData statsName) const;

    /**
     * Helper function to reduce redundancy for constructing string of LDAPOperationsStats.
     */
    void toStringHelper(StringBuilder* sb,
                        TickSource* tickSource,
                        Stats ldapOpStats,
                        StringData statsName) const;

    /**
     * Computes and returns total time spent on all cache accesses.
     */
    Microseconds _timeElapsed(TickSource* tickSource, Stats ldapOpStats) const;

    /**
     * Number of referrals to other LDAP servers
     */
    int64_t _numReferrals{0};

    /**
     * Metrics associated with binding, search/query, and unbinding from an LDAP server.
     */
    Stats _bindStats;
    Stats _searchStats;
    Stats _unbindStats;
};
}  // namespace mongo
