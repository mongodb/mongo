// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/tick_source.h"

#include <cstdint>
#include <memory>
#include <mutex>

namespace mongo {

class LDAPOperationStats;

/**
 * Class used to track statistics associated with LDAP operations for a specfic
 * UserAcquisitionStats object.
 */
class [[MONGO_MOD_PUBLIC]] LDAPCumulativeOperationStats {
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
    mutable std::mutex _memberAccessMutex;
};
}  // namespace mongo
