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
#include "mongo/stdx/mutex.h"
#include "mongo/util/duration.h"
#include "mongo/util/tick_source.h"

#include <cstdint>
#include <memory>

namespace mongo {

class LDAPOperationStats;

/**
 * Class used to track statistics associated with LDAP operations for a specfic
 * UserAcquisitionStats object.
 */
class LDAPCumulativeOperationStats {
public:
    LDAPCumulativeOperationStats() = default;
    ~LDAPCumulativeOperationStats() = default;
    LDAPCumulativeOperationStats(const LDAPCumulativeOperationStats&) = delete;
    LDAPCumulativeOperationStats& operator=(const LDAPCumulativeOperationStats&) = delete;

    /**
     * Marshals all statistics into BSON for reporting.
     */
    void report(BSONObjBuilder* builder) const;

    /**
     * Marshals all statistics into a string for reporting.
     */
    void toString(StringBuilder* sb) const;

    /**
     * Indicates whether any data was recorded
     */
    bool hasData() const;

    /**
     * Record stats for an LDAP operation.
     */
    void recordOpStats(const LDAPOperationStats& stats);

    /**
     * Gets pointer to a global instance or nullptr if not initialized.
     */
    static LDAPCumulativeOperationStats* get();

private:
    /**
     * Struct Stats is used to contain information about the bind and search stats
     * of the LDAP Operations.
     */
    struct Stats {
        int64_t numOps{0};
        Microseconds totalTime{Microseconds{0}};
    };

    /**
     * Number of successful referrals to other LDAP servers
     */
    int64_t _numSuccessfulReferrals{0};

    /**
     * Number of failed referrals to other LDAP servers
     */
    int64_t _numFailedReferrals{0};

    /**
     * Metrics associated with binding or search/query against an LDAP server.
     */
    Stats _bindStats;
    Stats _searchStats;

    /**
     * Protects access to member variables.
     */
    mutable stdx::mutex _memberAccessMutex;
};
}  // namespace mongo
