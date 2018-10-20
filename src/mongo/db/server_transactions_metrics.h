
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <set>

#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/transactions_stats_gen.h"

namespace mongo {

/**
 * Container for server-wide multi-document transaction statistics.
 */
class ServerTransactionsMetrics {
    MONGO_DISALLOW_COPYING(ServerTransactionsMetrics);

public:
    ServerTransactionsMetrics() = default;

    static ServerTransactionsMetrics* get(ServiceContext* service);
    static ServerTransactionsMetrics* get(OperationContext* opCtx);

    unsigned long long getCurrentActive() const;
    void decrementCurrentActive();
    void incrementCurrentActive();

    unsigned long long getCurrentInactive() const;
    void decrementCurrentInactive();
    void incrementCurrentInactive();

    unsigned long long getCurrentOpen() const;
    void decrementCurrentOpen();
    void incrementCurrentOpen();

    unsigned long long getTotalStarted() const;
    void incrementTotalStarted();

    unsigned long long getTotalAborted() const;
    void incrementTotalAborted();

    unsigned long long getTotalCommitted() const;
    void incrementTotalCommitted();

    /**
     * Returns the Timestamp of the oldest oplog entry written across all open transactions.
     * Returns boost::none if there are no transaction oplog entry Timestamps stored.
     */
    boost::optional<Timestamp> getOldestActiveTS() const;

    /**
     * Add the transaction's oplog entry Timestamp to a set of Timestamps.
     */
    void addActiveTS(Timestamp oldestOplogEntryTS);

    /**
     * Remove the corresponding transaction oplog entry Timestamp if the transaction commits or
     * aborts.
     */
    void removeActiveTS(Timestamp oldestOplogEntryTS);

    /**
     * Returns the number of transaction oplog entry Timestamps currently stored.
     */
    unsigned int getTotalActiveTS() const;

    /**
     * Appends the accumulated stats to a transactions stats object.
     */
    void updateStats(TransactionsStats* stats);

private:
    // The number of multi-document transactions currently active.
    AtomicUInt64 _currentActive{0};

    // The number of multi-document transactions currently inactive.
    AtomicUInt64 _currentInactive{0};

    // The total number of open transactions.
    AtomicUInt64 _currentOpen{0};

    // The total number of multi-document transactions started since the last server startup.
    AtomicUInt64 _totalStarted{0};

    // The total number of multi-document transaction aborts.
    AtomicUInt64 _totalAborted{0};

    // The total number of multi-document transaction commits.
    AtomicUInt64 _totalCommitted{0};

    // Maintain the oldest oplog entry Timestamp across all active transactions. Currently, we only
    // write an oplog entry for an ongoing transaction if it is in the `prepare` state. By
    // maintaining an ordered set of timestamps, the timestamp at the beginning will be the oldest.
    std::set<Timestamp> _oldestActiveOplogEntryTS;
};

}  // namespace mongo
